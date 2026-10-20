#!/usr/bin/env python3
"""Start Game when the brain never answers its /mm/host-game: the request times out and the Host window says so.

  KRUNKNATIVE_ASSETS_DIR=<assets dir> python3 -I client/tests/host_game_timeout_e2e.py BUILD_DIR   (from the repository root)

A stub brain answers /mm/status (so the Host window's Start Game is sent to it) and holds /mm/host-game without an
answer. The client's brain timeout (backend_native.c BACKEND_HTTP_TIMEOUT_MS, 8 s) then ends the request with status 0,
and the Host window's error line is v10's failure text, "Please try again." (on_host -> host_not_created, custom.tryagain),
not "Creating Game..." left behind. The brain sees the Start Game once: a /mm/host-game is never retried (it may have
created a game already).

Exit code 0 when every check passed; the input-script log is kept in ~/.cache/kn-hostgame-timeout-e2e-* on failure.
"""
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-hostgame-timeout-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
failures = []
release = threading.Event()  # the stalled /mm/host-game answers (nothing) once the run is over


def check(ok, what):
    print(("PASS " if ok else "FAIL ") + what)
    if not ok:
        failures.append(what)


def fold(s):
    return re.sub(r"\s+", " ", s or "").strip()


class Brain(BaseHTTPRequestHandler):
    paths = []  # every path asked, in order (main() gives each stub its own list)

    def log_message(self, *args):
        pass

    def do_GET(self):  # noqa: N802
        path = urlparse(self.path).path
        self.paths.append(path)
        if path == "/mm/status":
            body = b'{"brain":"stub"}'
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        elif path == "/mm/host-game":
            release.wait(120)  # no answer: the client's timeout ends the request
        else:
            self.send_error(404)


class Stub(ThreadingHTTPServer):
    daemon_threads = True


def run_native(script, brain_port, timeout=240):
    path = os.path.join(WORK, "native.script")
    with open(path, "w") as f:
        f.write(script.strip() + "\n")
    log = os.path.join(WORK, "native.log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": os.path.join(WORK, "cfg"),
                "XDG_STATE_HOME": os.path.join(WORK, "state"), "KRUNKNATIVE_NO_BROWSER": "1",
                "KRUNKNATIVE_UPDATES_URL": "off", "KRUNKNATIVE_INPUT_SCRIPT": path, "KRUNKNATIVE_WINDOW_SIZE": "1280x720",
                # the stub brain: an explicit URL counts as available (no probe), the Start Game goes to it
                "KRUNKNATIVE_BRAIN_URL": "http://127.0.0.1:%d" % brain_port})
    with open(log, "w") as out:
        p = subprocess.Popen([f"{BUILD}/client_bin", "--offline", "--map", "kanji"], stdout=out, stderr=subprocess.STDOUT,
                             stdin=subprocess.DEVNULL, env=env, start_new_session=True)
        try:
            p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            check(False, f"client_bin finished within {timeout} s")
        finally:
            if p.poll() is None:
                os.killpg(p.pid, signal.SIGKILL)
                p.wait()
    with open(log, errors="replace") as f:
        return f.read()


def text(log, label):
    m = re.search(r"\[input_script\] text t=\d+ " + re.escape(label) + r" n=(\d+) checked=\d value=.*? text=(.*)", log)
    return m.group(2) if m else None


SCRIPT = """
0 menu
300 call showWindow 42
800 call openHostWindow 0 0
1300 text @header [id=hostHeader]
1695 call knHostFlow "map" "kanji"
1700 clickid startServBtn
1750 text @creating [id=hostGameMsg]
1800 waitfor [id=hostGameMsg] span.error
1850 text @msg [id=hostGameMsg]
1900 quit
"""


def main():
    handler = type("BrainStub", (Brain,), {"paths": []})
    brain = Stub(("127.0.0.1", 0), handler)
    threading.Thread(target=brain.serve_forever, daemon=True).start()
    try:
        log = run_native(SCRIPT, brain.server_address[1])
    finally:
        release.set()
        brain.shutdown()

    check(fold(text(log, "@creating")) == "Creating Game...", f"the request is out: the Host window says Creating Game... ({text(log, '@creating')!r})")
    msg = fold(text(log, "@msg"))
    check(msg == "Please try again.", f"no answer by the timeout: the Host window says v10's Please try again. ({msg!r})")
    check("Creating Game" not in msg, "the spinner does not outlive the request")
    host_asks = [p for p in handler.paths if p == "/mm/host-game"]
    check(len(host_asks) == 1, f"the brain sees the Start Game once, never retried ({len(host_asks)} asks)")

    if failures:
        print(f"host_game_timeout_e2e: {len(failures)} failed (logs in {WORK})")
        sys.exit(1)
    shutil.rmtree(WORK, ignore_errors=True)
    print("host_game_timeout_e2e: all checks passed")


if __name__ == "__main__":
    main()
