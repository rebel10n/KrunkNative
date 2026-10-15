#!/usr/bin/env python3
"""Console `connect` with game links and garbage (client/src/ui/ui_console.c cmd_connect,
client/src/ui/servers_ui.c servers_ui_connect_game / servers_ui_parse_address).

  python3 -I server/tests/console_join_link_e2e.py BUILD_DIR      (run from the repository root)

No brain (KRUNKNATIVE_BRAIN_URL unset: headless never reaches a production one) and an unreachable
official matchmaker (KRUNKNATIVE_KRUNKER_MATCHMAKER on a port nothing listens on, so no krunker.io
request leaves the test). A headless client_bin driven through KRUNKNATIVE_UI_CMDS, plus one
standalone server_bin for the direct path:
  1. garbage (`connect garbage`, `connect a b`, `connect 127.0.0.1:99999`, `connect ?game=`) is
     refused at once: "[mm] connect refused: ..." within 3 s, and no direct connect follows
  2. a KrunkNative link or lobby id without a matchmaker (`connect CH2:kz862`,
     `connect ?game=ab12:xyz`) is refused at once with the matchmaker text, no direct connect
  3. a krunker.io game link (`connect https://www.krunker.io/?game=ab12:xyz`) takes the official
     path: "[krunker] official lobby ab12:xyz: joining in-client" (krunker_lobbies_open), with no
     brain and no direct connect
  4. the direct path still works: `connect 127.0.0.1:<port>` reaches the standalone server
Stubs: the official matchmaker is an unreachable local port (the join fails there, which is what
the test waits for); the game server is a real standalone server_bin.
Needs a headless GL device (KRUNKNATIVE_HEADLESS=1: EGL; KRUNKNATIVE_EGL_DEVICE picks it).
KRUNKNATIVE_ASSETS_DIR is passed through (worktrees lack the untracked font dirs).
Prints PASS/FAIL lines; exit code 0 only when everything passed.
"""
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from kn_proto import free_ports  # noqa: E402

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-join-link-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
CMDS = os.path.join(WORK, "cmds")
CLIENT_LOG = os.path.join(WORK, "client.log")
results = []
procs = []

GARBAGE = ["connect garbage", "connect a b", "connect 127.0.0.1:99999", "connect ?game="]
NO_MATCHMAKER = ["connect CH2:kz862", "connect ?game=ab12:xyz"]
OFFICIAL = "connect https://www.krunker.io/?game=ab12:xyz"
OFFICIAL_LINE = r"\[krunker\] official lobby ab12:xyz: joining in-client"


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


def wait_for(fn, timeout=10.0):
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


def no_direct(mark):
    """No direct connect (console or Connect window) and no server connection since mark."""
    return not after(mark, r"\[mm\] direct connect|Connecting through|connected to server|\[mm\] connect window")


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
        if results and all(results):
            shutil.rmtree(WORK, ignore_errors=True)
        else:
            print(f"logs kept in {WORK}")
    print(f"{sum(results)}/{len(results)} passed")
    sys.exit(0 if results and all(results) else 1)


def run():
    mm_port, local_port = free_ports(2)
    start("local", [f"{BUILD}/server_bin", "--headless", "--cfg", "none", "--port", str(local_port)])
    time.sleep(2.0)

    open(CMDS, "w").close()
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL", "KRUNKNATIVE_KRUNKER_JOIN")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": os.path.join(WORK, "cfg"), "XDG_STATE_HOME": os.path.join(WORK, "state"),
                "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_UI_CMDS": CMDS, "KRUNKNATIVE_WINDOW_SIZE": "640x360",
                "KRUNKNATIVE_KRUNKER_MATCHMAKER": f"http://127.0.0.1:{mm_port}"})
    with open(CLIENT_LOG, "w") as out:
        procs.append(subprocess.Popen(["stdbuf", "-oL", f"{BUILD}/client_bin"], stdout=out, stderr=subprocess.STDOUT,
                                      stdin=subprocess.DEVNULL, env=env, start_new_session=True))
    time.sleep(8.0)  # the console + DOM are up

    for line in GARBAGE:
        mark = con(line)
        refused = wait_for(lambda: after(mark, r"\[mm\] connect refused: "), 3.0)
        check(f"garbage refused at once: {line}", refused and no_direct(mark),
              refused[0] if refused else "no refusal within 3 s")

    for line in NO_MATCHMAKER:
        mark = con(line)
        refused = wait_for(lambda: after(mark, r"\[mm\] connect refused: .*matchmaker"), 3.0)
        check(f"a game link without a matchmaker is refused: {line}", refused and no_direct(mark),
              refused[0] if refused else "no refusal within 3 s")

    mark = con(OFFICIAL)
    official = wait_for(lambda: after(mark, OFFICIAL_LINE), 10.0)
    check("a krunker.io game link joins the official lobby", official and no_direct(mark),
          official[0] if official else "; ".join(after(mark, r"krunker|mm\]|connect")) or "no official line")

    mark = con(f"connect 127.0.0.1:{local_port}")
    here = f"connected to server 127.0.0.1:{local_port}"
    check("a host:port still connects directly", wait_for(lambda: after(mark, re.escape(here)), 15.0),
          "; ".join(after(mark, r"direct|connected|refused"))[:160])


if __name__ == "__main__":
    main()
