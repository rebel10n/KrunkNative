#!/usr/bin/env python3
"""Console connect / disconnect / reconnect end-to-end test (client/src/ui/ui_console.c).

  python3 -I server/tests/console_connect_e2e.py BUILD_DIR      (run from the repository root)

Starts a kn_brain (sqlite, private config), one game server linked to it (the "public
lobby" the client is matchmade to) and one standalone server_bin (the developer's local
server), then drives a headless client_bin in brain mode through KRUNKNATIVE_UI_CMDS:
  1. startup: matchmade to the linked lobby
  2. `connect localhost:<port>` reaches the local server and STAYS there: no matchmaker
     seek moves it back to the lobby (the old menu_retry_connect -> servers_ui_retry seek)
  3. `reconnect` (while connected) comes back to that same server
  4. `disconnect` stays disconnected: no seek, no transport retry
  5. `reconnect` after `disconnect` reaches the same server again
  6. the local server goes away: the transport retries with a backoff (2 s, 4 s, ...)
     and never falls back to the lobby
Needs a headless GL device (KRUNKNATIVE_HEADLESS=1: EGL; KRUNKNATIVE_EGL_DEVICE picks it).
Prints PASS/FAIL lines; exit code 0 only when everything passed.
"""
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import urllib.request

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
BRAIN_PORT = 5397
LOBBY_PORT, LOCAL_PORT = 22421, 22422
WORK = tempfile.mkdtemp(prefix="kn-con-e2e-")
CMDS = os.path.join(WORK, "cmds")
CLIENT_LOG = os.path.join(WORK, "client.log")
results = []
procs = []


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def start(name, argv, env=None):
    out = open(os.path.join(WORK, f"{name}.log"), "w")
    p = subprocess.Popen(argv, stdout=out, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, start_new_session=True)
    procs.append(p)
    return p


def read(path):
    try:
        with open(path, errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def client_lines():
    return read(CLIENT_LOG).splitlines()


def wait_for(fn, timeout=15.0):
    end = time.time() + timeout
    while time.time() < end:
        v = fn()
        if v:
            return v
        time.sleep(0.2)
    return fn()


def con(line):
    """One console line; returns the client log line count before it (lines after it are its effect)."""
    mark = len(client_lines())
    with open(CMDS, "a") as f:
        f.write(f"con {line}\n")
    wait_for(lambda: any(f"] {line}" in l for l in client_lines()[mark:]), 5.0)
    return mark


def after(mark, pattern):
    return [l for l in client_lines()[mark:] if re.search(pattern, l)]


def brain_up():
    try:
        with urllib.request.urlopen(f"http://127.0.0.1:{BRAIN_PORT}/mm/status", timeout=1) as r:
            return json.load(r).get("lobbies", 0) >= 1
    except Exception:
        return False


def main():
    try:
        run()
    finally:
        for p in procs:
            if p.poll() is None:
                os.killpg(p.pid, signal.SIGTERM)
        for p in procs:
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(p.pid, signal.SIGKILL)
        if all(results) and results:
            shutil.rmtree(WORK, ignore_errors=True)
        else:
            print(f"logs kept in {WORK}")
    print(f"{sum(results)}/{len(results)} passed")
    sys.exit(0 if results and all(results) else 1)


def run():
    brain_cfg = os.path.join(WORK, "brain.json")
    with open(brain_cfg, "w") as f:
        json.dump({"database": {"driver": "sqlite", "path": os.path.join(WORK, "brain.db")},
                   "brain": {"host": "127.0.0.1", "port": BRAIN_PORT, "region": "local",
                             "public_url": f"http://127.0.0.1:{BRAIN_PORT}", "federation": False}}, f)
    start("brain", [f"{BUILD}/services/brain/kn_brain", "--config", brain_cfg])
    start("lobby", [f"{BUILD}/server_bin", "--headless", "--cfg", "none", "--port", str(LOBBY_PORT), "--node-id", "con-e2e",
                    "--region", "local", "--public-host", "127.0.0.1", "--public-port", str(LOBBY_PORT),
                    "--brain", f"ws://127.0.0.1:{BRAIN_PORT}/mm/internal/ws"])
    local = start("local", [f"{BUILD}/server_bin", "--headless", "--cfg", "none", "--port", str(LOCAL_PORT)])
    check("brain lists the linked lobby", wait_for(brain_up, 15.0))

    open(CMDS, "w").close()
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": os.path.join(WORK, "cfg"), "XDG_STATE_HOME": os.path.join(WORK, "state"),
                "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_UI_CMDS": CMDS, "KRUNKNATIVE_WINDOW_SIZE": "640x360",
                "KRUNKNATIVE_BRAIN_URL": f"http://127.0.0.1:{BRAIN_PORT}"})
    with open(CLIENT_LOG, "w") as out:
        procs.append(subprocess.Popen(["stdbuf", "-oL", f"{BUILD}/client_bin"], stdout=out, stderr=subprocess.STDOUT,
                                      stdin=subprocess.DEVNULL, env=env, start_new_session=True))
    lobby = f"connected to server 127.0.0.1:{LOBBY_PORT}"
    here = f"connected to server 127.0.0.1:{LOCAL_PORT}"
    check("startup: matchmade to the lobby", wait_for(lambda: any(lobby in l for l in client_lines()), 40.0))
    time.sleep(2.0)  # the console + DOM are up

    mark = con(f"connect localhost:{LOCAL_PORT}")
    check("connect reaches the local server", wait_for(lambda: after(mark, re.escape(here)), 10.0))
    time.sleep(5.0)
    check("connect is not overridden by a matchmaker seek", not after(mark, r"\[mm\] joining|" + re.escape(lobby)),
          "; ".join(after(mark, r"\[mm\] joining|connected to server")))

    mark = con("reconnect")
    check("reconnect returns to the same server", wait_for(lambda: after(mark, re.escape(here)), 10.0))

    mark = con("disconnect")
    check("disconnect closes the session", wait_for(lambda: after(mark, r"disconnected from server"), 5.0))
    time.sleep(6.0)
    check("disconnect stays disconnected (no seek, no retry)", not after(mark, r"connected to server|\[mm\] joining|\[net\] retrying"),
          "; ".join(after(mark, r"connected to server|\[mm\] joining|\[net\] retrying")))

    mark = con("reconnect")
    check("reconnect after disconnect reaches the same server", wait_for(lambda: after(mark, re.escape(here)), 10.0))

    time.sleep(1.0)
    mark = len(client_lines())
    os.killpg(local.pid, signal.SIGKILL)
    delays = wait_for(lambda: (lambda d: d if len(d) >= 3 else None)(
        [int(m) for l in after(mark, r"\[net\] retrying") for m in re.findall(r" in (\d+) s", l)]), 30.0) or []
    check("a dead server is retried with a backoff", delays[:3] == [2, 4, 8], f"delays {delays}")
    check("the dead pinned server never falls back to the lobby", not after(mark, r"\[mm\] joining|" + re.escape(lobby)))


if __name__ == "__main__":
    main()
