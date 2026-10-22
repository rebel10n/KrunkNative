#!/usr/bin/env python3
"""The end screen's raid promo "Play Raid" seeks a public Raid lobby and never starts the offline game
(client/src/ui/ui_handlers.c h_play_raid -> servers_ui.c servers_ui_play_raid).

  python3 -I client/tests/raid_play_e2e.py BUILD_DIR      (run from the repository root)

v10's card ("Battle Bosses in Raids!", Play Now ->) opens the Raid Server Setup (openHostWindow(false, 4)),
which makes a private room. KrunkNative hosts nothing from the client, so Play Raid asks the Server Type's
matchmaker for a Raid lobby on Tortuga (mode 23). Nothing found, or no brain: v10's "Couldnt find any", and
the offline game does not start. A stub brain answers the game list and seeks; krunker.io is off
(KRUNKNATIVE_KRUNKER_MATCHMAKER=off) and no asset fetch goes out. Nothing reaches production.

  C0 the raid end-screen scenario (tools/uidiff/scenarios/ingame-endscreen-collect-raid.json, raid: 1) dumps a
     card whose onclick is knPlayRaid() and no longer opens the Raid Server Setup
  C1 no Raid lobby on Tortuga (the brain's list has a Raid on another map and a Tortuga match of another
     mode): the game list is asked, nothing is seeked, v10's "Couldnt find any" shows, the menu stays open
     (no offline game)
  C2 a Raid lobby on Tortuga with room: the seek for that lobby goes out and the client joins it; no message
  C3 no brain at all (the brain URL refuses): v10's "Couldnt find any", the menu stays open (no offline game)

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
MODE_COMP = 0

# (time ms, command). The boot (the launch seek is refused: no lobby at all, so the error screen), the card's
# handler, then the probes: the host setup, the popup, the game state and the console status (a Raid on Tortuga
# in the status is the offline game).
SCRIPT = """
0 boot
3000 call knPlayRaid
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

    def game_lists(self):
        return [q for p, q in self.log if p == "/game-list"]

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


def setup_opened(lines):
    setup = [l for l in lines if "[input_script] nodes" in l and "@setup" in l]
    return bool(setup) and " match=0" not in setup[-1]


def main():
    doc = card_markup()
    if doc is not None:
        m = re.search(r'<div class="crStat raidPromo"[^>]*onclick="([^"]*)"', doc)
        action = m.group(1) if m else ""
        check("C0 the raid promo card's Play Now calls knPlayRaid()", action == "knPlayRaid()", repr(action))
        check("C0 the card no longer opens the Raid Server Setup (openHostWindow)", "openHostWindow" not in (m.group(0) if m else ""))

    # C1: the brain lists no Raid on Tortuga: a Raid on Laboratory and a Tortuga match of another mode
    brain = Brain(free_port())
    brain.games = [row("LOCAL:labraid", "local", 2, 8, MODE_RAID, "Laboratory"),
                   row("LOCAL:tortcomp", "local", 3, 8, MODE_COMP, "Tortuga")]
    lines, log = run_client("c1", f"http://127.0.0.1:{brain.port}")
    check("C1 Play Raid asks the brain's game list (hostname krunknative)",
          any(q.get("hostname") == "krunknative" for q in brain.game_lists()), f"{len(brain.game_lists())} list requests")
    check("C1 no lobby is sought or joined (the launch seek has no lobby id)",
          not any(q.get("game") for q in brain.seeks()) and not joined(lines), brain.seeks())
    check("C1 v10's \"Couldnt find any\" shows", (probe_hits(lines, "@pop") or 0) >= 1,
          f"grep hits {probe_hits(lines, '@pop')}")
    check("C1 the Raid play ran its no-lobby path (the log line)", any("[mm] raid play: no Raid lobby on Tortuga" in l for l in lines), log)
    check("C1 the offline Raid does not start (no Raid on Tortuga in the console status)", not offline_raid_started(lines), log)
    check("C1 the Raid Server Setup does not open", not setup_opened(lines))
    brain.close()

    # C2: a Raid lobby on Tortuga with room is listed: the client seeks it
    brain = Brain(free_port())
    brain.games = [row(RAID_LOBBY, "local", 2, 8, MODE_RAID, "Tortuga")]
    lines, log = run_client("c2", f"http://127.0.0.1:{brain.port}")
    check("C2 the Raid lobby on Tortuga is sought (its seek goes out)",
          any(q.get("game") == RAID_LOBBY for q in brain.seeks()), brain.seeks())
    check("C2 the client joins it", any(f"[mm] joining {RAID_LOBBY}" in l for l in lines), log)
    check("C2 no \"Couldnt find any\" when a lobby is found", (probe_hits(lines, "@pop") or 0) == 0)
    check("C2 the offline Raid does not start", not offline_raid_started(lines), log)
    brain.close()

    # C3: no brain at all: the brain URL refuses the connection
    lines, log = run_client("c3", f"http://127.0.0.1:{free_port()}")
    check("C3 v10's \"Couldnt find any\" shows with no brain", (probe_hits(lines, "@pop") or 0) >= 1,
          f"grep hits {probe_hits(lines, '@pop')}")
    check("C3 the offline Raid does not start (no Raid on Tortuga in the console status)", not offline_raid_started(lines), log)
    check("C3 the Raid Server Setup does not open", not setup_opened(lines))

    print(f"\n{sum(results)}/{len(results)} passed  (work: {WORK})")
    return all(results)


if __name__ == "__main__":
    sys.exit(0 if main() else 1)
