#!/usr/bin/env python3
"""Quick Play retries a full lobby until the player is in one (client/src/ui/quick_play_retry.c, servers_ui.c).

  python3 -I server/tests/quickplay_retry_e2e.py BUILD_DIR      (run from the repository root)

A stub brain (KRUNKNATIVE_BRAIN_URL: /mm/ping-list, /mm/seek-game; /mm/generate-token is unknown and so
answers 404: the seek goes without one, as a brain from before the token does) answers each seek from a
script, counted from the start of each run. Every run's first seek is the startup one (Auto Seek on Launch
off) or the launch seek (on). The lobby the client joins is a port nothing listens on: the join is logged
("[mm] joining ...") and fails there; what is checked is the seeks and the messages before it. The region is
saved (defaultRegion), so no ping race runs.

F4 (Find New Game) is handled only in game, and a game needs a joined server, which this harness does not run.
Its entry point (servers_ui_find_new_game) is the same one the menu's Quick Match button reaches, so Q2 and Q4
drive it; Q1 drives the error screen's Find Game (knRetry -> servers_ui_retry).

  Q1 Find Game (the error screen's, after the startup seek failed with NoAvailableServers): two full lobbies,
     then a lobby with room: the client joins that lobby, the seeks after the full ones leave them out
     (excludeRecent), and no error is shown
  Q2 Quick Match (the menu's button, backToOrigin): the same answers, the same joined lobby
  Q3 Auto Seek on Launch: the launch's seek retries the same way
  Q4 Quick Match pressed again stops the search: the retry waiting after the first full answer is dropped
  N1 no lobby at all (NoAvailableServers from the start): the error, one seek, no retry
  N2 a lobby picked by hand that is full (GameFull for its game id): the error at once, one seek, no retry

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
WORK = tempfile.mkdtemp(prefix="kn-quickplay-retry-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
ASSETS = os.environ.get("KRUNKNATIVE_ASSETS_DIR") or (os.path.join(ROOT, "assets") if os.path.isdir(os.path.join(ROOT, "assets", "models"))
                                                       else os.path.expanduser("~/src/KrunkNative/assets"))
LOBBY_FULL_A = "LOCAL:aaaaa"  # full: its seek answer says 8 of 8
LOBBY_FULL_B = "LOCAL:bbbbb"  # full too
LOBBY_ROOM = "LOCAL:ccccc"    # with room: the lobby the client should join
results = []


def check(name, ok, detail=""):
    results.append(bool(ok))
    show = detail != "" and (not ok or len(str(detail)) <= 240)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + str(detail)) if show else ''}", flush=True)


def serve(port, handler):
    httpd = ThreadingHTTPServer(("127.0.0.1", port), handler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    return httpd


class Brain:
    """The stub brain. respond(query, n) answers the n-th /mm/seek-game of the current run (from 0)."""

    def __init__(self, port, dead_port):
        self.log = []  # (path, query dict), in order, every run
        self.times = []  # time.time() of each entry of log
        self.base = 0  # the seeks before the current run
        self.respond = None
        me = self

        def full(game):
            return 200, {"success": True, "host": "127.0.0.1", "port": dead_port, "gameId": game, "clientId": "k",
                         "region": "local", "players": 8, "maxPlayers": 8}

        def room(game):
            return 200, {"success": True, "host": "127.0.0.1", "port": dead_port, "gameId": game, "clientId": "k",
                         "region": "local", "players": 2, "maxPlayers": 8}

        self.full, self.room = full, room

        class H(BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def do_GET(self):
                u = urllib.parse.urlsplit(self.path)
                q = dict(urllib.parse.parse_qsl(u.query, keep_blank_values=True))
                path = u.path[len("/mm"):] if u.path.startswith("/mm") else u.path
                me.log.append((path, q))
                me.times.append(time.time())
                if path == "/ping-list":
                    status, body = 200, {"local": "127.0.0.1:%d" % port}
                elif path == "/seek-game":
                    status, body = me.respond(q, len(me.seeks()) - 1 - me.base)
                else:
                    status, body = 404, {"error": "NotFound"}
                data = json.dumps(body).encode()
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

        self.httpd = serve(port, H)

    def seeks(self):
        return [q for p, q in self.log if p == "/seek-game"]

    def seek_times(self):
        return [t for (p, _), t in zip(self.log, self.times) if p == "/seek-game"]

    def run_seeks(self):
        return self.seeks()[self.base:]

    def begin(self, respond):
        self.base = len(self.seeks())
        self.respond = respond


def error(status, name):
    return status, {"success": False, "error": name}


def run_client(label, script, brain_port, settings, timeout=240, watch=None):
    """One headless client_bin run in brain mode, with the saved settings (knSettings.json). watch(text) is
    called with the log as it grows (every 50 ms), while the client runs."""
    cfg = os.path.join(WORK, label + "-cfg")
    os.makedirs(os.path.join(cfg, "krunknative"), exist_ok=True)
    with open(os.path.join(cfg, "krunknative", "knSettings.json"), "w") as f:
        json.dump(settings, f)
    path = os.path.join(WORK, label + ".script")
    with open(path, "w") as f:
        f.write(script)
    log = os.path.join(WORK, label + ".log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL", "KRUNKNATIVE_KRUNKER_REGION")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": cfg, "XDG_STATE_HOME": cfg + "-state", "KRUNKNATIVE_NO_BROWSER": "1",
                "KRUNKNATIVE_AUDIO_BACKEND": "null", "KRUNKNATIVE_INPUT_SCRIPT": path, "KRUNKNATIVE_WINDOW_SIZE": "960x540",
                "KRUNKNATIVE_ASSETS_DIR": ASSETS, "KRUNKNATIVE_ASSET_FETCH": "off",
                # every frame is one script step of this length: the script runs at the same pace on a loaded host
                "KRUNKNATIVE_FIXED_DELTA": "50",
                "KRUNKNATIVE_BRAIN_URL": "http://127.0.0.1:%d" % brain_port})
    with open(log, "w") as out:
        p = subprocess.Popen(["stdbuf", "-oL", "-eL", f"{BUILD}/client_bin"], stdout=out, stderr=subprocess.STDOUT,
                             stdin=subprocess.DEVNULL, env=env, start_new_session=True, cwd=ROOT)
        deadline = time.time() + timeout
        while p.poll() is None:
            if time.time() > deadline:
                check(f"{label}: client_bin finished its script", False, log)
                os.killpg(p.pid, signal.SIGKILL)
                p.wait()
                break
            if watch:
                with open(log, errors="replace") as f:
                    watch(f.read())
            time.sleep(0.05)
    with open(log, errors="replace") as f:
        return f.read()


def joined(text):
    return re.findall(r"\[mm\] joining (\S+) at", text)


def exclude_recent(q):
    return [x for x in q.get("excludeRecent", "").split(",") if x]


def retried_twice(text):
    return sum("[mm] quick play: the lobby is full" in line for line in text.splitlines()) == 2


def find_failed(text):
    return "find new game failed" in text or "seek failed (HTTP 409)" in text


def check_retries(label, text, run):
    """The search's seeks: full A, full B, then the lobby with room (run[0] is the first of the search)."""
    lines = text.splitlines()
    check(f"{label}: the client joins the lobby with room, not a full one", joined(text)[:1] == [LOBBY_ROOM], joined(text))
    check(f"{label}: two full answers are retried (three seeks of the search)", len(run) >= 3 and retried_twice(text),
          (len(run), [line for line in lines if "quick play" in line or "is full" in line][:6]))
    check(f"{label}: the seek after the first full lobby leaves it out",
          len(run) >= 2 and LOBBY_FULL_A in exclude_recent(run[1]), run[1:2])
    check(f"{label}: the seek after the second full lobby leaves both out",
          len(run) >= 3 and {LOBBY_FULL_A, LOBBY_FULL_B} <= set(exclude_recent(run[2])), run[2:3])
    check(f"{label}: no error is shown (no find new game failure, no GameFull)", not find_failed(text),
          [line for line in lines if "failed" in line][:4])


def main():
    if not os.path.isfile(f"{BUILD}/client_bin"):
        print("quickplay_retry_e2e: no client_bin: skipped")
        return 77
    ports = kn_proto.free_ports(2)
    brain_port, dead_port = ports[0], ports[1]  # nothing listens on dead_port: the join fails after it is logged
    brain = Brain(brain_port, dead_port)
    saved = {"knSeekOnLaunch": False, "defaultRegion": "local"}  # the startup seek; the region saved (no ping race)
    launch = {"defaultRegion": "local"}                           # Auto Seek on Launch (the default): the launch seek

    def startup_error_then_full_full_room(q, n):
        # n = 0 is the startup seek (refused with no lobby at all); the search that follows gets full, full, room
        if n == 0:
            return error(503, "NoAvailableServers")
        if n == 1:
            return brain.full(LOBBY_FULL_A)
        if n == 2:
            return brain.full(LOBBY_FULL_B)
        return brain.room(LOBBY_ROOM)

    def launch_full_full_room(q, n):
        if n == 0:
            return brain.full(LOBBY_FULL_A)
        if n == 1:
            return brain.full(LOBBY_FULL_B)
        return brain.room(LOBBY_ROOM)

    # Q1: the error screen's Find Game (knRetry) after the startup seek failed
    brain.begin(startup_error_then_full_full_room)
    text = run_client("Q1", "0 boot\n4000 call knRetry\n40000 quit\n", brain_port, saved)
    run = brain.run_seeks()[1:]
    check("Q1: Find Game seeks a new game after the startup seek failed", len(run) >= 1, len(run))
    check_retries("Q1", text, run)

    # Q2: Quick Match (the menu's button, backToOrigin): the entry point F4 uses too
    brain.begin(startup_error_then_full_full_room)
    text = run_client("Q2", "0 boot\n4000 call backToOrigin\n40000 quit\n", brain_port, saved)
    run = brain.run_seeks()[1:]
    check("Q2: Quick Match seeks a new game", "[mm] quick match: seeking a lobby other than" in text, len(run))
    check_retries("Q2", text, run)

    # Q3: Auto Seek on Launch: the launch seek is the search itself
    brain.begin(launch_full_full_room)
    text = run_client("Q3", "0 boot\n40000 quit\n", brain_port, launch)
    run = brain.run_seeks()
    check("Q3: the launch seeks", "[mm] launch seek" in text, len(run))
    check_retries("Q3", text, run)

    # Q4: Quick Match pressed again while the search waits for its retry stops it: no seek is sent after the stop.
    # The stop is seen as the client logs it (the watcher); the retry was scheduled before it ("trying again").
    stop = {"at": None}

    def watch(text):
        if stop["at"] is None and "[mm] quick play: search stopped" in text:
            stop["at"] = time.time()

    brain.begin(lambda q, n: error(503, "NoAvailableServers") if n == 0 else brain.full(LOBBY_FULL_A))
    first = len(brain.seek_times())
    text = run_client("Q4", "0 boot\n4000 call backToOrigin\n6000 call backToOrigin\n20000 quit\n", brain_port, saved, watch=watch)
    run = brain.run_seeks()[1:]
    later = [t for t in brain.seek_times()[first:] if stop["at"] is not None and t > stop["at"] + 0.5]
    waiting = text.find("[mm] quick play: the lobby is full: trying again") != -1
    check("Q4: the second press stops the search", stop["at"] is not None and "[mm] quick play: search stopped" in text,
          [line for line in text.splitlines() if "quick play" in line][:4])
    check("Q4: the stop lands while a retry waits (the search was waiting for its next try)",
          waiting and text.find("trying again in") < text.find("search stopped"), [line for line in text.splitlines() if "quick play" in line][:3])
    check("Q4: no seek is sent after the stop", not later, (len(later), len(run)))
    check("Q4: nothing joins", not joined(text), joined(text))

    # N1: no lobby at all: the error at once, no retry
    brain.begin(lambda q, n: error(503, "NoAvailableServers"))
    text = run_client("N1", "0 boot\n4000 call backToOrigin\n10000 quit\n", brain_port, saved)
    run = brain.run_seeks()[1:]
    check("N1: no lobby at all: the search makes one seek, no retry", len(run) == 1 and "[mm] quick play" not in text, len(run))
    check("N1: the error is shown", "find new game failed: NoAvailableServers" in text,
          [line for line in text.splitlines() if "failed" in line][:3])

    # N2: a lobby picked by hand that is full: the error at once, no retry
    brain.begin(lambda q, n: error(503, "NoAvailableServers") if n == 0 else
                (error(409, "GameFull") if q.get("game") == LOBBY_FULL_A else brain.room(LOBBY_ROOM)))
    text = run_client("N2", '0 boot\n4000 call switchServer "%s"\n10000 quit\n' % LOBBY_FULL_A, brain_port, saved)
    run = brain.run_seeks()[1:]
    check("N2: the lobby picked by hand is asked for by its id", run[:1] and run[0].get("game") == LOBBY_FULL_A, run[:1])
    check("N2: the full lobby is refused at once: one seek, no retry, no join",
          len(run) == 1 and "[mm] quick play" not in text and not joined(text), (len(run), joined(text)))
    check("N2: the error is shown (the seek failed as GameFull)", "seek failed (HTTP 409): GameFull" in text,
          [line for line in text.splitlines() if "GameFull" in line][:2])

    brain.httpd.shutdown()
    if all(results):
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print(f"logs kept in {WORK}")
    print(f"{sum(results)}/{len(results)} passed")
    return 0 if results and all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
