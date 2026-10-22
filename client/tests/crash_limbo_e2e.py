#!/usr/bin/env python3
"""A lost game server never leaves the client on a loading screen (client/src/net/net.c, net_udp.c, events.c,
servers_ui.c, ui_console.c), end to end: a headless client_bin (direct connect, no brain), server_bin or a stub
TCP listener, and the page read back through KRUNKNATIVE_UI_CMDS (textof instructionsUpdate = the Lost Connection
text; con serverinfo = the connection state).

  kill-mid-round  a server_bin with a client joined to it is killed (SIGKILL) mid-round. The knet session times out
                  (10 s), and the client shows "Lost connection to the server" on the menu's Lost Connection screen.
  silent-stub     nothing is listening but a TCP stub that accepts and never answers (an edge whose game server
                  is gone). The session has no data for NET_SESSION_IDLE_SECONDS (15 s): the error, not a spinner.
  pings-stub      the stub pings but never sends INIT: the join deadline (NET_JOIN_TIMEOUT_SECONDS, 20 s) ends it.
  In every case the error must show within its deadline and serverinfo must not say "state OPEN" with no socket.

  python3 -I client/tests/crash_limbo_e2e.py BUILD_DIR [REPO_ROOT] [scenario ...]
Headless (KRUNKNATIVE_HEADLESS=1, null audio, no DISPLAY, private XDG dirs); no brain, no network beyond 127.0.0.1.
Logs are kept in ~/.cache/kn-crash-limbo.* on failure. 77 = skip (no binaries or no map).
"""
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time

BUILD = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.path.abspath("cmake-build-debug")
ROOT = os.path.abspath(sys.argv[2]) if len(sys.argv) > 2 and os.path.isdir(sys.argv[2]) else os.path.dirname(BUILD)
SCENARIOS = [a for a in sys.argv[2:] if not os.path.isdir(a)] or ["kill-mid-round", "silent-stub", "pings-stub"]
SERVER = os.path.join(BUILD, "server_bin")
CLIENT = os.path.join(BUILD, "client_bin")
MAP = os.path.join(ROOT, "assets", "maps", "311_industry.json")
if not all(os.path.exists(p) for p in (SERVER, CLIENT, MAP)):
    print("needs server_bin, client_bin and assets/maps/311_industry.json: skip")
    sys.exit(77)

CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-crash-limbo.", dir=CACHE if os.path.isdir(CACHE) else None)
FAILS = []
PROCS = []
LOST = "Lost connection to the server"

# A TCP listener that accepts and never answers, or (pings) sends a PING (type 17, a 4-byte seq) every 2 s and no INIT.
STUB = r'''
import socket, struct, sys, time
port, pings = int(sys.argv[1]), sys.argv[2] == "1"
srv = socket.socket(); srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", port)); srv.listen(16); srv.settimeout(0.5)
held, seq, next_ping = [], 1, 0.0
while True:
    try:
        c, _ = srv.accept(); c.settimeout(0.01); held.append(c)
    except socket.timeout:
        pass
    now = time.time()
    if pings and now >= next_ping:
        next_ping = now + 2.0; seq += 1
        frame = bytes([17, 0, 4]) + struct.pack(">I", seq)
        for c in list(held):
            try:
                c.sendall(frame)
            except OSError:
                held.remove(c)
'''


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what, flush=True)
    if not cond:
        FAILS.append(what)


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def read(path):
    try:
        with open(path, errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def spawn(args, log, cwd, env=None):
    p = subprocess.Popen(args, cwd=cwd, stdout=open(log, "w"), stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                         env=env, start_new_session=True)
    PROCS.append(p)
    return p


def stop(p):
    # only the processes this test started (their own process groups)
    if p is None:
        return
    if p.poll() is None:
        try:
            os.killpg(p.pid, signal.SIGKILL)
        except OSError:
            pass
    try:
        p.wait(timeout=5)
    except subprocess.TimeoutExpired:
        pass


def run(name, deadline, expect, transport=None, stub_pings=None, kill_server=False):
    print("== %s (the error within %d s: %s)" % (name, deadline, expect), flush=True)
    base = os.path.join(WORK, name)
    os.makedirs(os.path.join(base, "cfg"))
    port = free_port()
    server = stub = client = None
    try:
        if kill_server:
            server = spawn([SERVER, "--headless", "--port", str(port), "--mode", "ffa", "--cfg", "none", MAP],
                           os.path.join(base, "server.log"), ROOT)
        else:
            stub = spawn([sys.executable, "-I", "-c", STUB, str(port), "1" if stub_pings else "0"],
                         os.path.join(base, "stub.log"), ROOT)
            time.sleep(0.5)
        cmds = os.path.join(base, "cmds.txt")
        open(cmds, "w").close()
        env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_UI_DIR")}
        env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": os.path.join(base, "cfg"),
                    "XDG_STATE_HOME": os.path.join(base, "state"), "KRUNKNATIVE_NO_BROWSER": "1",
                    "KRUNKNATIVE_UPDATES_URL": "off", "KRUNKNATIVE_FINDER_URL": "off", "KRUNKNATIVE_UI_CMDS": cmds,
                    "KRUNKNATIVE_SERVER_PORT": str(port), "KRUNKNATIVE_WINDOW_SIZE": "1280x720"})
        if transport:
            env["KRUNKNATIVE_NET_TRANSPORT"] = transport
        log = os.path.join(base, "client.log")
        client = spawn(["stdbuf", "-oL", "-eL", CLIENT], log, ROOT, env)

        if kill_server:
            # joined: the client connected to the server (its session is up)
            end = time.time() + 90
            while time.time() < end and "connected to server" not in read(log):
                time.sleep(0.5)
            check("connected to server" in read(log), "%s: the client joined the server before the kill" % name)
            time.sleep(3)
            server.send_signal(signal.SIGKILL)
            server.wait()
            print("  server killed (SIGKILL), mid-round", flush=True)
        t0 = time.time()
        shown = None
        while time.time() - t0 < deadline:
            with open(cmds, "a") as f:
                f.write("textof instructionsUpdate\n")
            time.sleep(1.0)
            texts = re.findall(r"textof instructionsUpdate = '(.*)'", read(log))
            if texts and LOST in texts[-1]:
                shown = (time.time() - t0, texts[-1])
                break
        if shown:
            print("  %6.1f s  error on the screen: %s" % (shown[0], shown[1][:120]), flush=True)
        else:
            print("  %6.1f s  no error on the screen" % (time.time() - t0), flush=True)
        check(shown is not None, "%s: the client shows \"%s\" within %d s" % (name, LOST, deadline))
        if shown:
            check(expect in shown[1], "%s: the error says why (%s)" % (name, expect))
            check(shown[0] <= deadline, "%s: the error came in %.0f s (deadline %d s)" % (name, shown[0], deadline))
        # serverinfo after the loss: no state OPEN without a socket
        with open(cmds, "a") as f:
            f.write("con serverinfo\n")
        time.sleep(1.5)
        info = re.findall(r"\[console\] Connection: (.*)", read(log))
        check(bool(info) and "state OPEN" not in info[-1], "%s: serverinfo is not OPEN after the loss (%s)" % (name, info[-1] if info else "no output"))
        check(client.poll() is None, "%s: the client is still running (back on the menu)" % name)
        # a test client talks to its own server only: no public server list, no krunk.io host
        check(not re.search(r"https?://[\w.-]*krunk\.io", read(log)), "%s: the client contacted no krunk.io host" % name)
        print("  log: %s" % log, flush=True)
    finally:
        stop(client)
        stop(server)
        stop(stub)


SCEN = {
    "kill-mid-round": dict(deadline=40, expect="no data for 10 s", kill_server=True),
    "silent-stub": dict(deadline=40, expect="no data for 15 s", transport="tcp", stub_pings=False),
    "pings-stub": dict(deadline=50, expect="no answer to the join within 20 s", transport="tcp", stub_pings=True),
}

ok = True
try:
    for name in SCENARIOS:
        if name not in SCEN:
            print("unknown scenario %s" % name)
            sys.exit(2)
        run(name, **SCEN[name])
finally:
    for p in PROCS:
        stop(p)
if FAILS:
    print("FAILED (%d):" % len(FAILS))
    for f in FAILS:
        print("  - " + f)
    print("logs kept in %s" % WORK)
    sys.exit(1)
shutil.rmtree(WORK, ignore_errors=True)
print("crash-limbo e2e: all checks passed")
sys.exit(0)
