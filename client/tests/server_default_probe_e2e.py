#!/usr/bin/env python3
"""The Servers window's krunk.io footer on the default-probe path (backend.h: the default account service is probed).

  python3 -I client/tests/server_default_probe_e2e.py BUILD_DIR      (from the repository root)

A headless client_bin with no --brain and no KRUNKNATIVE_BRAIN_URL: its default account service is the test seam
KRUNKNATIVE_TEST_DEFAULT_BRAIN (a loopback stub), so the real probe (GET /mm/status) runs. The stub answers /mm/status
slowly (1.5 s), so the Servers window is generated while the probe is in flight (backend_available() -1). Checks:
  - the window opened during the probe fills its footer (In Game = the brain's totalPlayerCount, 14) once the probe
    answers, without reopening the window (before the fix nothing re-rendered it: the footer stayed blank);
  - the game list is the brain's (its region rows);
  - the probe ran (the client logged "brain found"), and no request went to a production host.
Exit code 0 when every check passed; the log is kept in ~/.cache/kn-default-probe-e2e-* on failure.
"""
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
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ASSETS = os.environ.get("KRUNKNATIVE_ASSETS_DIR") or next(
    (d for d in (os.path.join(ROOT, "assets"), os.path.expanduser("~/src/KrunkNative/assets")) if os.path.isdir(os.path.join(d, "models"))),
    os.path.join(ROOT, "assets"))
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-default-probe-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
PROBE_DELAY_S = 1.5
failures = []

# the brain's /mm/game-list answer: [id, region, players, max, data, timeLeft]
GAMES = {"success": True, "totalPlayerCount": 14, "games": [
    ["CHITOWN1:ab12c", "CH", 6, 8, {"i": "kanji", "g": 0, "host": "chi1.example", "port": 7777}, 300],
    ["CHITOWN2:cd34e", "CH", 3, 8, {"i": "sandstorm", "g": 1, "host": "chi2.example", "port": 7777}, 200],
    ["SV1:ef56g", "SV", 5, 8, {"i": "burg", "g": 0, "host": "sv1.example", "port": 7777}, 120],
]}


PROBES = []  # the probe's requests the stub answered (the default-probe path ran)


class Stub(BaseHTTPRequestHandler):
    def do_GET(self):  # noqa: N802
        path = self.path.split("?")[0]
        if path == "/mm/status":
            PROBES.append(self.path)
            time.sleep(PROBE_DELAY_S)  # the probe is in flight while the window is generated
            body = {"brain": "stub", "region": "us", "version": "test", "players": 14}
        elif path == "/mm/game-list":
            body = GAMES
        elif path == "/api/player-count":
            body = {"playerCount": 14}
        else:
            self.send_error(404)
            return
        data = json.dumps(body).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, *args):
        pass


def check(ok, what):
    print(("PASS " if ok else "FAIL ") + what)
    if not ok:
        failures.append(what)


def fold(s):
    return re.sub(r"\s+", " ", s or "").strip()


def production_sends(log):
    return [l.strip() for l in log.splitlines()
            if re.search(r"^\[(http|finder|krunker)\].*https?://(?!127\.0\.0\.1|localhost)", l)]


def text(log, label):
    m = re.search(r"\[input_script\] text t=\d+ " + re.escape(label) + r" n=(\d+) checked=\d value=.*? text=(.*)", log)
    return (int(m.group(1)), m.group(2)) if m else (-1, None)


def grep(log, label):
    m = re.search(r"\[input_script\] grep t=\d+ " + re.escape(label) + r" needle=\S+ n=(\d+) hits=(\d+)", log)
    return int(m.group(2)) if m else -1


SCRIPT = ("0 boot\n300 call showWindow 2\n"
          "2500 text @early [id=playerCnt]\n2510 text @early_social [id=socialCnt]\n"
          "2520 grep @early_rows Chicago [id=serverHolder]\n"
          "3000 quit\n")


def main():
    stub = ThreadingHTTPServer(("127.0.0.1", 0), Stub)
    threading.Thread(target=stub.serve_forever, daemon=True).start()
    port = stub.server_address[1]
    script_path = os.path.join(WORK, "native.script")
    with open(script_path, "w") as f:
        f.write(SCRIPT)
    log_path = os.path.join(WORK, "native.log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": os.path.join(WORK, "cfg"),
                "XDG_STATE_HOME": os.path.join(WORK, "state"), "KRUNKNATIVE_NO_BROWSER": "1",
                "KRUNKNATIVE_UPDATES_URL": "off", "KRUNKNATIVE_INPUT_SCRIPT": script_path, "KRUNKNATIVE_WINDOW_SIZE": "1280x720",
                "KRUNKNATIVE_ASSETS_DIR": ASSETS, "KRUNKNATIVE_FINDER_URL": "off",
                "KRUNKNATIVE_TEST_DEFAULT_BRAIN": "http://127.0.0.1:%d" % port,
                "KRUNKNATIVE_KRUNKER_MATCHMAKER": "http://127.0.0.1:%d" % port,
                "KRUNKNATIVE_KRUNKER_SOCIAL": "http://127.0.0.1:%d/social" % port,
                "KRUNKNATIVE_KRUNKER_EDITOR": "http://127.0.0.1:%d/editor" % port,
                "KRUNKNATIVE_KRUNKER_URL": "https://krunker.io"})
    with open(log_path, "w") as out:
        # the menu, no --map (a map start joins a lobby): the account service is the default one (the seam's stub)
        cmd = [f"{BUILD}/client_bin"]  # boot: the menu page, no map (a map start would join one)
        if shutil.which("stdbuf"): cmd = ["stdbuf", "-oL"] + cmd  # the log is flushed line by line (a kill keeps it)
        p = subprocess.Popen(cmd, stdout=out, stderr=subprocess.STDOUT,
                             stdin=subprocess.DEVNULL, env=env, start_new_session=True)
        try:
            p.wait(timeout=120)
        except subprocess.TimeoutExpired:
            check(False, "client_bin finished within 120 s")
        finally:
            if p.poll() is None:
                os.killpg(p.pid, signal.SIGKILL)
                p.wait()
    stub.shutdown()
    with open(log_path, errors="replace") as f:
        log = f.read()
    check(len(PROBES) >= 1 and ("brain found at" in log or "[backend] brain" in log),
          f"the default probe ran against the stub ({len(PROBES)} /mm/status)")
    prod = production_sends(log)
    check(not prod, f"no request went to a production host ({prod[:2]})")
    _, early = text(log, "@early")
    check(fold(early) == "14 In Game", f"the window generated during the probe fills its In Game from the brain's 14 ({fold(early)!r})")
    _, soc = text(log, "@early_social")
    check(fold(soc) == "14 on Social", f"krunk.io's on Social is the brain's own /api/player-count, 14 ({fold(soc)!r})")
    check(grep(log, "@early_rows") >= 1, "the window lists the brain's Chitown lobby")
    if failures:
        print(f"server_default_probe_e2e: {len(failures)} failed (log in {WORK})")
        sys.exit(1)
    shutil.rmtree(WORK, ignore_errors=True)
    print("server_default_probe_e2e: all checks passed")


if __name__ == "__main__":
    main()
