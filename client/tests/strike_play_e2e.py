#!/usr/bin/env python3
"""Krunker Strike's PLAY with no connection (client/src/ui/strike_menu.c play, menu_ui.c menu_play_when_ready,
strike_connect.c the Reconnecting... loader and its deadline): PLAY never fails silently.

  python3 -I client/tests/strike_play_e2e.py BUILD_DIR      (run from the repository root)

A headless Strike client (--flavor strike) started with --connect to a local port nothing listens on, so the
transport is down from the start. The page boots (boot: the clock starts with no map), and PLAY is pressed
through the page's own handler (strikeAction "play"), which the Lost Connection popup does not stop.
  1. the press is answered in the log: "[strike] play: not connected, reconnecting" (fails before the fix: PLAY
     showed "Loading  ..." and returned from menu_play with nothing said).
  2. the reconnect ends in an answer on the page, never a loader for good: once the loader has gone (the reconnect's
     15 s deadline, strike_connect.c, or the transport's own give-up) the page shows the Lost Connection popup, with
     its Reconnect button (the error with Retry). The loader is "waited for" with a wall-clock limit of 90 s.
The client's own log is the judge; the page's text is read with the input script's `text` command.
Needs a headless GL device (KRUNKNATIVE_EGL_DEVICE picks it). Prints PASS/FAIL lines; exit 0 only when all passed.
"""
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-strike-play-", dir=CACHE if os.path.isdir(CACHE) else None)
results = []

NOT_CONNECTED = "[strike] play: not connected, reconnecting"
LOST_TEXT = "Lost Connection"


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail and not ok else ''}", flush=True)


def closed_port():
    """A local port with nothing listening on it (bound, then released)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def run_client(script, port):
    base = os.path.join(WORK, "down")
    cfg = os.path.join(base, "cfg")
    # a returning player: the first run's controls tutorial would replace the menu (strike_ftue.c), as in strike_vsync_e2e.py
    os.makedirs(os.path.join(cfg, "krunknative-strike"), exist_ok=True)
    with open(os.path.join(cfg, "krunknative-strike", "knFtue.json"), "w") as f:
        f.write('{"guest":{"played":true}}')
    os.makedirs(os.path.join(base, "state"), exist_ok=True)
    script_path = os.path.join(base, "input.script")
    with open(script_path, "w") as f:
        f.write(script)
    drop = ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_VSYNC", "KRUNKNATIVE_FRAME_CAP", "KRUNKNATIVE_BRAIN_URL", "KN_FLAVOR")
    env = {k: v for k, v in os.environ.items() if k not in drop}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": cfg, "XDG_STATE_HOME": os.path.join(base, "state"),
                "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "KRUNKNATIVE_UPDATES_URL": "off",
                "KRUNKNATIVE_WINDOW_SIZE": "1280x720",
                "KRUNKNATIVE_INPUT_SCRIPT": script_path, "KRUNKNATIVE_FIXED_DELTA": "16.667",
                "KRUNKNATIVE_ASSETS_DIR": os.path.abspath("assets") + os.sep})
    log = os.path.join(base, "client.log")
    with open(log, "w") as out:
        try:
            code = subprocess.run([f"{BUILD}/client_bin", "--flavor", "strike", "--connect", f"127.0.0.1:{port}"],
                                  stdout=out, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env,
                                  timeout=240).returncode
        except subprocess.TimeoutExpired:
            code = None
    with open(log, errors="replace") as f:
        return code, f.read()


def main():
    port = closed_port()
    # the clock starts with the page (boot: no map ever comes); PLAY is pressed once the menu is up; the text is read
    # after the press (the Lost Connection popup, if the transport has given up), and after the reconnect's deadline
    # the reconnect's deadline is wall clock (html timers) and the script's clock is per frame: the script waits for the
    # loader to go (waitfor, wall clock, 90 s at most), not for a frame count
    script = """0 boot
0 waitfor [id=mobMenuPlay]
1000 call strikeAction "play"
3000 text @after_play .alert-text
3100 waitfor !.frvr-loading-holder .loading-screen
3200 text @settled .alert-text
3210 text @loader .frvr-loading-holder .loading-screen
3300 quit
"""
    code, text = run_client(script, port)
    if code is None:
        print("FAIL  client_bin timed out")
    check("the client ran to its quit", code == 0, f"exit {code}")
    check("PLAY is answered in the log (not connected: reconnecting)", NOT_CONNECTED in text)
    loaders = re.findall(r"text t=\d+ @loader n=(\d+)", text)
    check("no loading screen is left up after the reconnect (it ends in the popup, not Reconnecting... for good)",
          loaders and loaders[-1] == "0", f"loader matches={loaders[-1:] or 'none read'}")
    settled = re.findall(r"\[input_script\] text t=\d+ @settled .*?text=(.*)", text)
    shown = settled[-1] if settled else ""
    check("the page says what happened: Lost Connection, with Reconnect",
          LOST_TEXT in shown, f"text={shown[:160]!r}")
    print(f"log: {WORK}/down/client.log")
    failed = len(results) - sum(results)
    print(f"{len(results) - failed}/{len(results)} passed")
    return 0 if all(results) else 1


if __name__ == "__main__":
    rc = main()
    if rc == 0:
        shutil.rmtree(WORK, ignore_errors=True)
    sys.exit(rc)
