#!/usr/bin/env python3
"""The end screen's raid promo "Play Raid" opens v10's Raid Server Setup
(client/src/ui/hud_endscreen_v10.c mount_raid -> openHostWindow(false, 4) -> host_flow_ui.c).

  python3 -I client/tests/raid_play_e2e.py BUILD_DIR      (run from the repository root)

v10's card ("Battle Bosses in Raids!", Play Now ->) calls openHostWindow(false, 4) (deob-10.0.5.js:200914-200916):
the Host Game window's Raid type, whose Select tab lists the Raid maps (allMaps: Tortuga, Laboratory, Soul
Sanctum, Khepri, Bastion; deob-10.0.5.js:16612). The player creates the room from there. The card never seeks a
lobby and never starts the offline game. A stub brain answers the game list and seeks; krunker.io is off
(KRUNKNATIVE_KRUNKER_MATCHMAKER=off) and no asset fetch goes out. Nothing reaches production.

  C0 the raid end-screen scenario (tools/uidiff/scenarios/ingame-endscreen-collect-raid.json, raid: 1) dumps a
     card whose onclick is openHostWindow(false, 4)
  C1 with a Raid lobby on Tortuga listed by the brain: the click opens the Raid Server Setup with the five Raid
     maps; no lobby is sought or joined (the server browser's own game-list refresh is not the card's),
     the offline game does not start
  C2 no brain at all (the brain URL refuses): the Raid Server Setup opens the same way, no offline game

Prints PASS/FAIL lines; exit code 0 only when everything passed. Needs a headless GL device.
"""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
import re
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import urllib.parse

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
REPO = os.getcwd()
WORK = tempfile.mkdtemp(prefix="kn-raid-play-e2e-", dir=os.path.expanduser("~/.cache"))
# the game's assets: a checkout's excluded ones (css fonts, models) are read from the main checkout when this tree lacks them
ASSETS = os.environ.get("KRUNKNATIVE_ASSETS_DIR") or (os.path.join(REPO, "assets") if os.path.isdir(os.path.join(REPO, "assets", "models"))
                                                       else os.path.expanduser("~/src/KrunkNative/assets"))
results = []

RAID_LOBBY = "LOCAL:raidA"  # a Raid on Tortuga with room (C2)
MODE_RAID = 23

# (time ms, command). The boot (the launch seek is refused: no lobby at all, so the error screen), the card's
# click (its onclick, openHostWindow(false, 4)), then the probes: the window's title, its map cards, the popup and
# the console status (a Raid on Tortuga in the status is the offline game).
SCRIPT = """
0 boot
3000 call openHostWindow 0 4
3500 grep @title Raid [id=hostHeader]
3500 nodes @setup .hostMap
7000 grep @pop Couldnt [id=genericPop]
7000 con status
9000 quit
"""


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + str(detail)) if detail else ''}", flush=True)


def env_base():
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null",
                "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_UPDATES_URL": "off",
                "KRUNKNATIVE_ASSET_FETCH": "off", "KRUNKNATIVE_KRUNKER_MATCHMAKER": "off",
                "KRUNKNATIVE_ASSETS_DIR": ASSETS})
    return env


def free_port():
    """A local port nothing listens on (the brain URL of C3, and the lobby's join in C2)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class Brain:
    """The stub brain: /mm/game-list answers `games` (set per run), /mm/seek-game answers a room for the game
    asked for. Every request is logged as (path, query)."""

    def __init__(self, dead_port):
        self.games = []
        self.log = []
        me = self

        class H(BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def do_GET(self):
                u = urllib.parse.urlsplit(self.path)
                q = dict(urllib.parse.parse_qsl(u.query, keep_blank_values=True))
                path = u.path[len("/mm"):] if u.path.startswith("/mm") else u.path
                me.log.append((path, q))
                if path == "/game-list":
                    status, body = 200, {"games": me.games}
                elif path == "/seek-game" and q.get("game"):
                    # a lobby asked for by id (the Raid play's join): the join goes to a port nothing listens on, so
                    # it is logged and fails there
                    status, body = 200, {"success": True, "host": "127.0.0.1", "port": dead_port, "gameId": q["game"],
                                         "clientId": "k", "region": "local", "players": 2, "maxPlayers": 8}
                elif path == "/seek-game":
                    # the launch seek (every run starts with one): no lobby at all, so nothing is joined
                    status, body = 404, {"success": False, "error": "NoAvailableServers"}
                else:
                    status, body = 404, {"error": "NotFound"}
                data = json.dumps(body).encode()
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

        self.httpd = ThreadingHTTPServer(("127.0.0.1", 0), H)
        self.port = self.httpd.server_address[1]
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def seeks(self):
        return [q for p, q in self.log if p == "/seek-game"]

    def close(self):
        self.httpd.shutdown()
        self.httpd.server_close()


def row(game, region, clients, maxp, mode, map_name):
    """A game-list row: [id, region, clients, maxClients, meta] (on_play_list reads the meta's map and mode)."""
    return [game, region, clients, maxp, {"map": map_name, "mode": mode}]


def card_markup():
    """The raid promo card of the end-screen scenario, as the captured document has it."""
    doc = os.path.join(WORK, "raid-doc.html")
    env = env_base()
    env["KRUNKNATIVE_SCENARIO_HTML"] = doc
    p = subprocess.run([f"{BUILD}/client_bin", "--render-scenario", "tools/uidiff/scenarios/ingame-endscreen-collect-raid.json",
                        os.path.join(WORK, "raid.png")], env=env, stdin=subprocess.DEVNULL, capture_output=True, text=True,
                       timeout=240, start_new_session=True)
    if p.returncode != 0 or not os.path.exists(doc):
        check("C0 the raid end-screen scenario renders", False, f"exit {p.returncode}: {p.stderr[-300:]}")
        return None
    with open(doc, errors="replace") as f:
        return f.read()


def run_client(label, brain_url):
    """One headless client_bin run, online (no --offline): the saved settings keep the startup seek and the
    ping race off, the brain URL is the stub (or a port nothing listens on, C3)."""
    cfg = os.path.join(WORK, label + "-cfg")
    os.makedirs(os.path.join(cfg, "krunknative"), exist_ok=True)
    with open(os.path.join(cfg, "krunknative", "knSettings.json"), "w") as f:
        json.dump({"knSeekOnLaunch": False, "defaultRegion": "local"}, f)
    script = os.path.join(WORK, label + ".script")
    with open(script, "w") as f:
        f.write(SCRIPT.strip() + "\n")
    log = os.path.join(WORK, label + ".log")
    env = env_base()
    env.update({"XDG_CONFIG_HOME": cfg, "XDG_STATE_HOME": cfg + "-state", "KRUNKNATIVE_FIXED_DELTA": "16.6667",
                "KRUNKNATIVE_INPUT_SCRIPT": script, "KRUNKNATIVE_WINDOW_SIZE": "1280x720",
                "KRUNKNATIVE_BRAIN_URL": brain_url})
    with open(log, "w") as out:
        # --finder off: the community server list (matchmaker.krunk.io) is not asked
        p = subprocess.Popen(["stdbuf", "-oL", "-eL", f"{BUILD}/client_bin", "--finder", "off"], stdout=out, stderr=subprocess.STDOUT,
                             stdin=subprocess.DEVNULL, env=env, start_new_session=True, cwd=REPO)
        try:
            p.wait(timeout=240)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
            check(f"{label}: client_bin finished its script", False, f"timeout, log {log}")
        try:
            os.killpg(p.pid, signal.SIGKILL)  # the client's group: nothing of this run is left behind
        except ProcessLookupError:
            pass
    with open(log, errors="replace") as f:
        return f.read().splitlines(), log


def probe_hits(lines, needle_label):
    """The hits of a `grep` probe (the popup's text) in the log, or None when the probe did not run."""
    for l in lines:
        m = re.search(r"\[input_script\] grep t=\S+ " + re.escape(needle_label) + r" .* hits=(\d+)", l)
        if m:
            return int(m.group(1))
    return None


def joined(lines):
    return [m for l in lines for m in re.findall(r"\[mm\] joining (\S+) at", l)]


def offline_raid_started(lines):
    st = [l for l in lines if "[console]" in l and "| map " in l]
    return bool(st) and re.search(r"map Tortuga \(\d+\) \| mode raid \(23\)", st[-1]) is not None


def setup_maps(lines):
    """The map cards of the open Host Game window (the `nodes @setup .hostMap` probe), None when it did not run."""
    setup = [l for l in lines if "[input_script] nodes" in l and "@setup" in l]
    m = re.search(r" match=(\d+)", setup[-1]) if setup else None
    return int(m.group(1)) if m else None


def check_setup(label, lines):
    check(f"{label} the Raid Server Setup opens (its title)", (probe_hits(lines, "@title") or 0) >= 1,
          f"grep hits {probe_hits(lines, '@title')}")
    check(f"{label} it lists the five Raid maps", setup_maps(lines) == 5, f"cards {setup_maps(lines)}")
    check(f"{label} no \"Couldnt find any\"", (probe_hits(lines, "@pop") or 0) == 0)


def main():
    doc = card_markup()
    if doc is not None:
        m = re.search(r'<div class="crStat raidPromo"[^>]*onclick="([^"]*)"', doc)
        action = m.group(1) if m else ""
        check("C0 the raid promo card's Play Now calls openHostWindow(false, 4)", action == "openHostWindow(false, 4)", repr(action))

    # C1: a Raid lobby on Tortuga with room is listed: the card still opens the setup, as v10's does
    brain = Brain(free_port())
    brain.games = [row(RAID_LOBBY, "local", 2, 8, MODE_RAID, "Tortuga")]
    lines, log = run_client("c1", f"http://127.0.0.1:{brain.port}")
    check_setup("C1", lines)
    check("C1 no lobby is sought or joined (the launch seek has no lobby id)",
          not any(q.get("game") for q in brain.seeks()) and not joined(lines), brain.seeks())
    check("C1 the offline Raid does not start (no Raid on Tortuga in the console status)", not offline_raid_started(lines), log)
    brain.close()

    # C2: no brain at all: the brain URL refuses the connection
    lines, log = run_client("c2", f"http://127.0.0.1:{free_port()}")
    check_setup("C2", lines)
    check("C2 the offline Raid does not start (no Raid on Tortuga in the console status)", not offline_raid_started(lines), log)

    print(f"\n{sum(results)}/{len(results)} passed  (work: {WORK})")
    return all(results)


if __name__ == "__main__":
    sys.exit(0 if main() else 1)
