#!/usr/bin/env python3
"""The seek's region is ping-aware (Krunker v10.0.2 ClientManager.getRegion, v10.js:147469-147507;
client/include/region_ping.h): with no region chosen or cached, the client fetches the matchmaker's
ping list, pings every eligible region's /ping at the same time and seeks in the FIRST to answer,
caching it; a region the player chose wins over the ping.

  python3 -I server/tests/seek_region_e2e.py BUILD_DIR      (run from the repository root)

Stub ping endpoints (one HTTP server per region, each answering /ping after its own delay), a stub of
krunker.io's matchmaker (KRUNKNATIVE_KRUNKER_MATCHMAKER: /ping-list, /game-list, /seek-game) and a
stub brain (KRUNKNATIVE_BRAIN_URL: /mm/ping-list, /mm/seek-game), every port from kn_proto.free_ports().

  official (Settings > Quickplay > Server Type krunker.io, Find New Game from the offline game):
    O1 a player near New York (us-nj answers first) is never sent to Mumbai, though Mumbai's lobby is the
       busiest: the seek takes the us-nj lobby, us-wa (dedicated only) is never pinged, the region is
       cached and the krunker.io Region row shows it; a second Find New Game pings nothing
    O2 the next run reuses the cached region: no ping list, no pings
    O3 an explicit krunker.io Region (de-fra) wins over the cached one
    O4 the near region's only lobby is full: the seek does not widen (no other region's lobby: v10 seeks one region)
    O5 every ping fails: the list's first region (de-fra), nothing cached
  ours (the brain's seek, Auto Seek on Launch):
    B1 the fastest of us-nj / as-mb / de-fra (us-nj) is the seek's region, cached; Settings > Default Region
       shows it (the brain's ping list is keyed by v10 ids, doc/v10/regions.md)
    B2 the next run reuses it without pinging
    B3 Settings > Default Region (de-fra) wins
    B4 a Default Region saved by an older build as its KrunkNative code (VA) is us-nj: sought as us-nj

Needs a headless GL device (KRUNKNATIVE_HEADLESS=1: EGL).
"""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse

sys.path.insert(0, os.path.join(os.getcwd(), "server/tests"))
import kn_proto  # noqa: E402

BUILD = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug")
ROOT = os.getcwd()
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-seek-region-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
ASSETS = os.environ.get("KRUNKNATIVE_ASSETS_DIR") or (os.path.join(ROOT, "assets") if os.path.isdir(os.path.join(ROOT, "assets", "models"))
                                                       else os.path.expanduser("~/src/KrunkNative/assets"))
results = []


def check(name, ok, detail=""):
    results.append(bool(ok))
    show = detail != "" and (not ok or len(str(detail)) <= 240)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + str(detail)) if show else ''}", flush=True)


def serve(port, handler):
    httpd = ThreadingHTTPServer(("127.0.0.1", port), handler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    return httpd


class Pinger:
    """One region's ping host: /ping answers after `delay` seconds."""

    def __init__(self, port, delay):
        self.port, self.delay, self.hits = port, delay, 0
        me = self

        class H(BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def do_GET(self):
                if urllib.parse.urlsplit(self.path).path != "/ping":
                    self.send_response(404)
                    self.end_headers()
                    return
                me.hits += 1
                time.sleep(me.delay)
                body = b'{"success":"true"}'
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.send_header("Access-Control-Allow-Origin", "*")
                self.end_headers()
                self.wfile.write(body)

        self.httpd = serve(port, H)


class Matchmaker:
    """krunker.io's matchmaker (official) or the brain (prefix "/mm"): ping-list, game-list, seek-game."""

    def __init__(self, port, ping_list, games=None, prefix=""):
        self.log = []  # (path, query dict)
        me = self

        class H(BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def do_GET(self):
                u = urllib.parse.urlsplit(self.path)
                q = dict(urllib.parse.parse_qsl(u.query, keep_blank_values=True))
                path = u.path[len(prefix):] if prefix and u.path.startswith(prefix) else u.path
                me.log.append((path, q))
                status, body = 200, None
                if path == "/ping-list":
                    body = ping_list
                elif path == "/game-list" and games is not None:
                    body = {"success": True, "totalPlayerCount": sum(g[2] for g in games), "games": games}
                elif path == "/seek-game":
                    status, body = 400, {"error": "GameIssue"}  # the region is what is checked; nothing is joined
                else:
                    status, body = 404, {"error": "NotFound"}
                data = json.dumps(body).encode()
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(data)))
                self.send_header("Access-Control-Allow-Origin", "*")
                self.end_headers()
                self.wfile.write(data)

        self.httpd = serve(port, H)

    def paths(self):
        return [p for p, _ in self.log]

    def seeks(self):
        return [q for p, q in self.log if p == "/seek-game"]


def run_client(label, script, env_extra, args, cfg, timeout=90):
    os.makedirs(os.path.join(cfg, "krunknative"), exist_ok=True)
    path = os.path.join(WORK, label + ".script")
    with open(path, "w") as f:
        f.write(script)
    log = os.path.join(WORK, label + ".log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL", "KRUNKNATIVE_KRUNKER_REGION")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": cfg, "XDG_STATE_HOME": cfg + "-state", "KRUNKNATIVE_NO_BROWSER": "1",
                "KRUNKNATIVE_AUDIO_BACKEND": "null", "KRUNKNATIVE_INPUT_SCRIPT": path, "KRUNKNATIVE_WINDOW_SIZE": "960x540",
                "KRUNKNATIVE_ASSETS_DIR": ASSETS, "KRUNKNATIVE_ASSET_FETCH": "off"})
    env.update(env_extra)
    with open(log, "w") as out:
        p = subprocess.Popen(["stdbuf", "-oL", "-eL", f"{BUILD}/client_bin", *args], stdout=out, stderr=subprocess.STDOUT,
                             stdin=subprocess.DEVNULL, env=env, start_new_session=True, cwd=ROOT)
        try:
            p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            check(f"{label}: client_bin finished its script", False, log)
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
    with open(log, errors="replace") as f:
        return f.read()


def text_value(text, name):
    m = re.search(r"\[input_script\] text t=\d+ @%s n=(\d+) checked=\d value=(\S*)" % name, text)
    return m.group(2) if m and m.group(1) != "0" else None


# ---------------------------------------------------------------- official krunker.io seeks

def official():
    ports = kn_proto.free_ports(6)
    near, mid, far, dedicated = Pinger(ports[0], 0.05), Pinger(ports[1], 0.8), Pinger(ports[2], 1.5), Pinger(ports[3], 0.0)
    pingers = [near, mid, far, dedicated]
    ping_list = {"de-fra": "127.0.0.1:%d" % mid.port, "us-nj": "127.0.0.1:%d" % near.port, "as-mb": "127.0.0.1:%d" % far.port,
                 "us-wa": "127.0.0.1:%d" % dedicated.port}  # us-wa: v10's dedicatedOnlyRegions, never pinged
    games = [["MB:mmmmm", "as-mb", 7, 8, {"i": "Burg", "g": 0}, 240],      # the busiest lobby of all, in Mumbai
             ["FRA:fffff", "de-fra", 4, 8, {"i": "Sandstorm", "g": 0}, 240],
             ["NY:nnnnn", "us-nj", 2, 8, {"i": "Littletown", "g": 0}, 240]]
    mm = Matchmaker(ports[4], ping_list, games)
    env = {"KRUNKNATIVE_KRUNKER_MATCHMAKER": "http://127.0.0.1:%d" % ports[4]}
    cfg = os.path.join(WORK, "official-cfg")
    seek = '100 call setSetting "knSeekServers" "official"\n'

    def hits():
        return [p.hits for p in pingers]

    def expect(races):
        # each seek race pings the eligible regions once (us-wa never); each krunker.io list load pings every
        # region once too (v10 getRegionPing, krunker_lobbies.c: the rows' ping icons), so the counts add up
        lists = mm.paths().count("/game-list")
        return [races + lists, races + lists, races + lists, lists]

    # O1: the race
    text = run_client("O1", "0 menu\n" + seek + "500 call backToOrigin\n4000 call backToOrigin\n6500 call showWindow 1\n"
                      "7500 text @kio [id=knKrunkerRegion]\n8000 quit\n", env, ("--offline",), cfg)
    games_sought = [(q.get("game"), q.get("region")) for q in mm.seeks()]
    check("O1: the Mumbai regression: a player near New York seeks the New York lobby, not Mumbai's busier one",
          games_sought[:1] == [("NY:nnnnn", "us-nj")], games_sought)
    check("O1: Mumbai is never sought", not any(g == "MB:mmmmm" or r == "as-mb" for g, r in games_sought), games_sought)
    check("O1: the race asked the ping list once, before the first seek (the list's own ping round asks it too)",
          mm.paths().count("/ping-list") == 2 and mm.paths().index("/ping-list") < mm.paths().index("/seek-game"), mm.paths())
    check("O1: every available region was pinged once by the race, at the same time; the dedicated-only us-wa never by it",
          hits() == expect(1), (hits(), expect(1)))
    check("O1: the race's winner is logged", "[krunker] seek region: us-nj (New York), the first to answer the ping race" in text,
          [l for l in text.splitlines() if "seek region" in l or "ping race" in l])
    check("O1: the second Find New Game reuses the region (no new race ping)", len(games_sought) == 2 and games_sought[1][1] == "us-nj" and
          hits() == expect(1), games_sought)
    check("O1: the krunker.io Region row shows the auto-picked region", text_value(text, "kio") == "us-nj", text_value(text, "kio"))

    # O2: the cache
    before = len(mm.log)
    run_client("O2", "0 menu\n" + seek + "500 call backToOrigin\n3000 quit\n", env, ("--offline",), cfg)
    later = mm.log[before:]
    check("O2: the next run reuses the cached region: no race (its ping list is the list's own round only), no race pings",
          [p for p, _ in later].count("/ping-list") == 1 and hits() == expect(1), ([p for p, _ in later], hits()))
    check("O2: ... and seeks in it", [(q.get("game"), q.get("region")) for p, q in later if p == "/seek-game"][:1] == [("NY:nnnnn", "us-nj")], later)

    # O3: an explicit region
    before = len(mm.log)
    run_client("O3", "0 menu\n" + seek + '200 call setSetting "knKrunkerRegion" "de-fra"\n500 call backToOrigin\n3000 quit\n', env, ("--offline",), cfg)
    later = [(q.get("game"), q.get("region")) for p, q in mm.log[before:] if p == "/seek-game"]
    check("O3: the player's krunker.io Region wins over the ping race's (no race: no race pings)",
          later[:1] == [("FRA:fffff", "de-fra")] and hits() == expect(1), later)
    for p in pingers:
        p.httpd.shutdown()
    mm.httpd.shutdown()

    # O4: the near region has no room: widen
    ports = kn_proto.free_ports(3)
    near, far = Pinger(ports[0], 0.0), Pinger(ports[1], 1.0)
    mm = Matchmaker(ports[2], {"us-nj": "127.0.0.1:%d" % near.port, "as-mb": "127.0.0.1:%d" % far.port},
                    [["MB:mmmmm", "as-mb", 7, 8, {"i": "Burg", "g": 0}, 240], ["NY:nnnnn", "us-nj", 8, 8, {"i": "Littletown", "g": 0}, 240]])
    run_client("O4", "0 menu\n" + seek + "500 call backToOrigin\n4000 quit\n", {"KRUNKNATIVE_KRUNKER_MATCHMAKER": "http://127.0.0.1:%d" % ports[2]},
               ("--offline",), os.path.join(WORK, "widen-cfg"))
    got = [(q.get("game"), q.get("region")) for q in mm.seeks()]
    check("O4: the near region's only lobby is full: no lobby of another region is joined (v10 seeks the one region)",
          ("MB:mmmmm", "as-mb") not in got, got)
    for s in (near, far, mm):
        s.httpd.shutdown()

    # O5: every ping fails
    dead = kn_proto.free_ports(3)
    mm = Matchmaker(dead[2], {"us-nj": "127.0.0.1:%d" % dead[0], "as-mb": "127.0.0.1:%d" % dead[1]},
                    [["MB:mmmmm", "as-mb", 7, 8, {"i": "Burg", "g": 0}, 240], ["FRA:fffff", "de-fra", 2, 8, {"i": "Burg", "g": 0}, 240]])
    cfg5 = os.path.join(WORK, "dead-cfg")
    text = run_client("O5", "0 menu\n" + seek + "500 call backToOrigin\n5000 quit\n", {"KRUNKNATIVE_KRUNKER_MATCHMAKER": "http://127.0.0.1:%d" % dead[2]},
                      ("--offline",), cfg5)
    got = [(q.get("game"), q.get("region")) for q in mm.seeks()]
    check("O5: every ping failed: the list's first region (de-fra), as krunker.io's matchmaker defaults", got[:1] == [("FRA:fffff", "de-fra")], got)
    check("O5: ... logged, and not cached", "[krunker] seek region: every ping failed" in text and "knKrunkerPingRegion" not in
          "".join(open(os.path.join(cfg5, "krunknative", f), errors="replace").read() for f in os.listdir(os.path.join(cfg5, "krunknative"))
                  if os.path.isfile(os.path.join(cfg5, "krunknative", f))), "")
    mm.httpd.shutdown()


# ---------------------------------------------------------------- our own seeks (the brain)

def brain():
    ports = kn_proto.free_ports(4)
    us_nj, as_mb, de_fra = Pinger(ports[0], 0.05), Pinger(ports[1], 1.5), Pinger(ports[2], 0.8)
    pingers = [us_nj, as_mb, de_fra]
    brain = Matchmaker(ports[3], {"us-nj": "127.0.0.1:%d" % us_nj.port, "de-fra": "127.0.0.1:%d" % de_fra.port,
                                  "as-mb": "127.0.0.1:%d" % as_mb.port}, prefix="/mm")
    env = {"KRUNKNATIVE_BRAIN_URL": "http://127.0.0.1:%d" % ports[3]}
    cfg = os.path.join(WORK, "brain-cfg")

    def hits():
        return [p.hits for p in pingers]

    text = run_client("B1", "0 boot\n4000 call showWindow 1\n5000 text @def select[onchange*=defaultRegion]\n5500 quit\n", env, (), cfg)
    regions = [q.get("region") for q in brain.seeks()]
    check("B1: Auto Seek on Launch seeks in the region that answered its ping first (us-nj), not the list's first (as-mb)",
          regions[:1] == ["us-nj"], (regions, brain.paths()[:8]))
    check("B1: each listed region was pinged once", hits() == [1, 1, 1], hits())
    check("B1: the race's winner is logged", "[mm] seek region: us-nj, the first to answer the ping race" in text,
          [l for l in text.splitlines() if "seek region" in l or "ping race" in l])
    check("B1: Settings > Default Region shows the auto-picked region", text_value(text, "def") == "us-nj", text_value(text, "def"))
    before = len(brain.log)
    run_client("B2", "0 boot\n3500 quit\n", env, (), cfg)
    regions = [q.get("region") for p, q in brain.log[before:] if p == "/seek-game"]
    check("B2: the next run reuses the cached region without pinging", regions[:1] == ["us-nj"] and hits() == [1, 1, 1], (regions, hits()))
    before = len(brain.log)
    run_client("B3", '0 boot\n100 call setSetting "defaultRegion" "de-fra"\n2500 call backToOrigin\n5000 quit\n', env, (), cfg)
    regions = [q.get("region") for p, q in brain.log[before:] if p == "/seek-game"]
    check("B3: Settings > Default Region (de-fra) wins over the cached ping region", regions and regions[-1] == "de-fra" and hits() == [1, 1, 1],
          (regions, hits()))
    before = len(brain.log)
    run_client("B4", '0 boot\n100 call setSetting "defaultRegion" "VA"\n2500 call backToOrigin\n5000 quit\n', env, (), cfg)
    regions = [q.get("region") for p, q in brain.log[before:] if p == "/seek-game"]
    check("B4: a Default Region saved as the old KrunkNative code (VA) is sought as its v10 id, us-nj",
          regions and regions[-1] == "us-nj" and hits() == [1, 1, 1], (regions, hits()))
    for s in pingers + [brain]:
        s.httpd.shutdown()


def unraced():
    """A fresh profile whose ping race has not answered yet: the brain's list's first region (as-mb) is no ping result, so it is
    not cached (pingRegion7). Host Game and Settings > Default Region read only a race's confirmed answer."""
    ports = kn_proto.free_ports(4)
    va, mb, fr = Pinger(ports[0], 3.0), Pinger(ports[1], 3.0), Pinger(ports[2], 3.0)
    pingers = [va, mb, fr]
    brain = Matchmaker(ports[3], {"as-mb": "127.0.0.1:%d" % mb.port, "de-fra": "127.0.0.1:%d" % fr.port, "us-nj": "127.0.0.1:%d" % va.port}, prefix="/mm")
    env = {"KRUNKNATIVE_BRAIN_URL": "http://127.0.0.1:%d" % ports[3]}
    cfg = os.path.join(WORK, "unraced-cfg")
    text = run_client("F1", "0 boot\n2000 quit\n", env, (), cfg)  # quits before any ping answers
    check("F1: the race started (three regions pinged) and had not answered when the client quit",
          "[mm] ping race: 3 regions" in text and "answered first" not in text, text[-300:])
    cached = os.path.join(cfg, "krunknative", "pingRegion7.json")
    check("F1: no pingRegion7.json before a race answers: the list's first region (as-mb) is not cached", not os.path.exists(cached),
          open(cached).read() if os.path.exists(cached) else "")
    for p in pingers:
        p.httpd.shutdown()
    brain.httpd.shutdown()


def main():
    if not os.path.isfile(f"{BUILD}/client_bin"):
        print("seek_region_e2e: no client_bin: skipped")
        return 77
    official()
    brain()
    unraced()
    if all(results):
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print(f"logs kept in {WORK}")
    print(f"{sum(results)}/{len(results)} passed")
    return 0 if results and all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
