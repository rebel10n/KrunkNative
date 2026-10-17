#!/usr/bin/env python3
"""The server browser's rows and refreshes (Krunker v10.0.2 windows[1]: loadData, gen, setExpandedRegion,
openServerWindow; v10.js:211155-211460, 218373-218417, 216625).

  python3 -I server/tests/server_list_e2e.py BUILD_DIR      (run from the repository root)

  R1 a row's ping icon is the region's measured ping (v10's pingData, getRegionPing with every list load):
     a slow region (1.5 s) shows red on Box Office's rows and on krunker.io's rows, not the low green
  R2 the list is asked again only when it is older than 10 s (v10's serverBrowserRate): a re-render inside
     that time, the region header's click included, asks nothing; a switch after it asks once
  F1 Find Game's Raids (2) joins the official custom game of that mode with room; Zombies (3) and Trade
     Plaza (4) with none listed say "Couldnt find any" (v10 openServerWindow)

Stub ping endpoints (one HTTP server per region, /ping after its delay), a stub brain (KRUNKNATIVE_BRAIN_URL:
/mm/ping-list, /mm/game-list) and a stub of krunker.io's matchmaker (KRUNKNATIVE_KRUNKER_MATCHMAKER:
/ping-list, /game-list). Needs a headless GL device (KRUNKNATIVE_HEADLESS=1: EGL).
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
WORK = tempfile.mkdtemp(prefix="kn-server-list-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
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

    def __init__(self, port, ping_list, games=None, prefix="", seek_answer=None):
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
                elif path == "/seek-game" and seek_answer is not None:
                    status, body = seek_answer(q)
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



GREEN = "rgb(158, 235, 86)"
RED = "rgb(255, 75, 66)"


def attr_value(text, label):
    m = re.search(r"\[input_script\] attr t=\d+ @%s n=\d+ style=([^\n]*)" % label, text)
    return m.group(1) if m else None


def list_loads(text):
    return text.count("[mm] game list (")


def rows():
    """R1: the Box Office region MB (slow) and krunker.io's us-nj (slow): both icons must show the measured ping."""
    ports = kn_proto.free_ports(5)
    mb, us = Pinger(ports[0], 1.5), Pinger(ports[1], 1.5)
    brain_list = {"MB": "127.0.0.1:%d" % mb.port}
    brain = Matchmaker(ports[2], brain_list, [["MB:mmmmm", "MB", 3, 8, {"i": "Burg", "g": 0}, 240]], prefix="/mm")
    kio = Matchmaker(ports[3], {"us-nj": "127.0.0.1:%d" % us.port},
                     [["NY:nnnnn", "us-nj", 2, 8, {"i": "Littletown", "g": 0, "oc": 0, "c": 0}, 240]])
    env = {"KRUNKNATIVE_BRAIN_URL": "http://127.0.0.1:%d" % ports[2], "KRUNKNATIVE_KRUNKER_MATCHMAKER": "http://127.0.0.1:%d" % ports[3],
           "KRUNKNATIVE_KRUNKER_JOIN": "browser", "KRUNKNATIVE_FINDER_URL": "http://127.0.0.1:9/servers"}  # no finder servers: the groups are the stubs'
    cfg = os.path.join(WORK, "rows-cfg")
    # each region group is expanded in turn (one at a time), and the two rows are read by their join ids
    # ([onclick*=...]: Box Office's MB:mmmmm, krunker.io's NY:nnnnn), whichever group holds them
    # each tab's rows: Box Office's on krunk.io (tab 0), krunker.io's on krunker.io (tab 1); each region group is
    # expanded in turn (one at a time), and the rows are read by their join ids ([onclick*=...])
    reads, t = [], 7000
    for tab, key, join_id in ((0, "mb", "mmmmm"), (1, "kio", "nnnnn")):
        reads.append("%d call windows.1.switchTab %d" % (t, tab))
        t += 200
        for group in range(4):
            reads.append("%d call setExpandedRegion %d" % (t, group))
            reads.append("%d attr @%s%d style [onclick*=%s] .serverPing" % (t + 50, key, group, join_id))
            t += 300
    script = ("0 boot\n4000 call showWindow 2\n" + "\n".join(reads) + "\n"
              "%d call windows.1.switchTab 0\n%d call windows.1.switchTab 0\n%d call windows.1.switchTab 0\n%d quit\n" % (t + 200, t + 2200, t + 11500, t + 12500))
    text = run_client("R1", script, env, (), cfg, timeout=120)
    mb_style = next((v for v in (attr_value(text, "mb%d" % g) for g in range(4)) if v and v != "-"), None)
    kio_style = next((v for v in (attr_value(text, "kio%d" % g) for g in range(4)) if v and v != "-"), None)
    check("R1: a Box Office row's ping icon is the measured ping (1.5 s: red), not the low green",
          mb_style is not None and RED in mb_style, (mb_style, text[-300:]))
    check("R1: a krunker.io row's ping icon too (krunker.io's own ping list, measured with its list)",
          kio_style is not None and RED in kio_style, (kio_style, [l for l in text.splitlines() if "krunker" in l][-5:], kio.paths()))
    # R2: the list is asked once at the window's opening, not by the re-renders inside 10 s, and again after
    loads = list_loads(text)
    check("R2: the list is loaded at the opening, and again only after 10 s (re-renders and the region click ask nothing)",
          loads == 2, "loads=%d brain=%s" % (loads, brain.paths()))


def play():
    """P1-P3: ?play=<map> (hub links, v10 playMap): a seek for that map in any region with v10's dataQuery
    (i, the map's index), autoChangeGame false; no lobby plays it (NoMatchingServers): the plain seek, in place."""
    ports = kn_proto.free_ports(1)
    state = {"play": 0}

    def answer(q):
        # the startup seek (plain) gets GameIssue as the other tests do; the play seek finds no lobby; the
        # plain seek after it (v10's retry) joins one
        if q.get("autoChangeGame") == "false" and '"i"' in q.get("dataQuery", ""):
            state["play"] += 1
            return 503, {"error": "NoMatchingServers"}
        if state["play"]:
            return 200, {"host": "127.0.0.1", "port": 9, "gameId": "KAN:ok001", "clientId": "cid-1", "region": "local"}
        return 400, {"error": "GameIssue"}

    mm = Matchmaker(ports[0], {}, [], prefix="/mm", seek_answer=answer)
    env = {"KRUNKNATIVE_BRAIN_URL": "http://127.0.0.1:%d" % ports[0]}
    cfg = os.path.join(WORK, "play-cfg")
    text = run_client("P1", '0 boot\n4000 call playMap "Kanji"\n9000 quit\n', env, (), cfg, timeout=90)
    seeks = mm.seeks()
    played = [q for q in seeks if q.get("autoChangeGame") == "false" and '"i"' in q.get("dataQuery", "")]
    first = played[0] if played else {}
    dq = first.get("dataQuery", "")
    check("P1: ?play=Kanji seeks the map in any region (regionId 'any': an empty region), v10's dataQuery i + the map's index, autoChangeGame false",
          first.get("region", "x") == "" and '"i":"Kanji"' in dq and '"dsma":[' in dq, first)
    check("P1: the play seek names no lobby (no game, no excludeGame)", "game" not in first and "excludeGame" not in first, first)
    i = seeks.index(first) if first else -1
    retry = seeks[i + 1] if 0 <= i < len(seeks) - 1 else {}
    check("P2: no lobby plays it (NoMatchingServers): the plain seek, in place (v10's retry), with the Quickplay lists and autoChangeGame",
          retry.get("autoChangeGame") == "true" and '"i"' not in retry.get("dataQuery", "") and retry.get("region", "") != "", seeks[i:i + 2])
    check("P2: ... and the retry's answer joins its lobby", "KAN:ok001" in text, text[-300:])
    check("P2: the retry is logged", "[mm] play: no lobby plays the map: a plain seek" in text, [l for l in text.splitlines() if "[mm] play" in l])


def list_failure():
    """L1: the game list fails: the Servers window keeps its LOADING text (v10's loadData: loadedServers stays
    false, nothing re-renders), it does not turn into an empty list."""
    ports = kn_proto.free_ports(1)
    mm = Matchmaker(ports[0], {}, None, prefix="/mm")  # no games: /game-list answers 404
    env = {"KRUNKNATIVE_BRAIN_URL": "http://127.0.0.1:%d" % ports[0], "KRUNKNATIVE_FINDER_URL": "http://127.0.0.1:9/servers"}
    cfg = os.path.join(WORK, "list-cfg")
    text = run_client("L1", "0 boot\n4000 call showWindow 2\n9000 text @ld [id=menuWindow]\n9100 quit\n", env, (), cfg, timeout=90)
    m = re.search(r"@ld n=\d+ checked=\d value=\S* text=(.*)", text)
    check("L1: a failed game list keeps the window on LOADING (not an empty list)", m and "LOADING" in m.group(1) and "No Joinable" not in m.group(1),
          m.group(1) if m else text[-300:])
    check("L1: the failure is logged", "[mm] game list failed (HTTP 404): the server list stays loading" in text, [l for l in text.splitlines() if "game list" in l][-2:])


def finder():
    """F1: Find Game's official customs: Raids joins the one with room (busiest first), the others say why."""
    ports = kn_proto.free_ports(2)
    kio = Matchmaker(ports[0], {}, [
        ["NY:fulll", "us-nj", 8, 8, {"i": "Raid", "g": 23, "oc": 1, "h": "Full raid"}, 300],
        ["NY:raid1", "us-nj", 3, 8, {"i": "Raid", "g": 23, "oc": 1, "h": "Raid room"}, 300],
        ["NY:ffa", "us-nj", 6, 8, {"i": "Burg", "g": 0}, 300],
    ])
    env = {"KRUNKNATIVE_KRUNKER_MATCHMAKER": "http://127.0.0.1:%d" % ports[0], "KRUNKNATIVE_KRUNKER_JOIN": "browser"}
    cfg = os.path.join(WORK, "finder-cfg")
    text = run_client("F1", "0 boot\n4000 call openServerWindow 2\n6600 text @took [id=genericPop]\n7000 call openServerWindow 3\n"
                      "7500 text @none [id=genericPop]\n7600 call openServerWindow 4\n8000 quit\n", env, (), cfg, timeout=90)
    took = re.search(r"@took n=\d+ checked=\d value=\S* text=(.*)", text)
    check("F1: the join did not take the player into the game: 2 s later v10's \"Took to long to find\" popup",
          took and "Took to long to find" in took.group(1), took.group(1) if took else text[-300:])
    check("F1: Raids joins the official custom with room (the full one skipped)", "[krunker] find game: official custom NY:raid1 (mode 23)" in text
          and "https://krunker.io/?game=NY:raid1" in text, [l for l in text.splitlines() if "find game" in l or "official lobby" in l][-4:])
    none = re.search(r"@none n=\d+ checked=\d value=\S* text=(.*)", text)
    check("F1: Zombies with no official custom of its mode: the popup says Couldnt find any (v10's responsePop)",
          none and "Couldnt find any" in none.group(1), none.group(1) if none else text[-200:])
    check("F1: Trade Plaza too: Couldnt find any", text.count("no official custom of that mode is listed: Couldnt find any") == 2,
          text.count("Couldnt find any"))
    check("F1: the Raids join is not a seek (no matchmaker seek-game)", "[mm] seek" not in text, "")


def main():
    if not os.path.isfile(f"{BUILD}/client_bin"):
        print("server_list_e2e: no client_bin: skipped")
        return 77
    rows()
    finder()
    list_failure()
    play()
    if all(results):
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print(f"logs kept in {WORK}")
    print(f"{sum(results)}/{len(results)} passed")
    return 0 if results and all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
