#!/usr/bin/env python3
"""A lost game server never leaves the client on a loading screen (client/src/net/net.c, net_udp.c, events.c,
servers_ui.c, ui_console.c), end to end: a headless client_bin (direct connect, or matched through a stub brain /
matchmaker on 127.0.0.1), server_bin or a stub TCP listener, and the page read back through KRUNKNATIVE_UI_CMDS
(textof instructionsUpdate = the Lost Connection text; con serverinfo = the connection state).

  kill-mid-round  a server_bin with a client joined to it is killed (SIGKILL) mid-round. The knet session times out
                  (10 s), and the client shows "Lost connection to the server" on the menu's Lost Connection screen.
  silent-stub     nothing is listening but a TCP stub that accepts and never answers (an edge whose game server
                  is gone). The session has no data for NET_SESSION_IDLE_SECONDS (15 s): the error, not a spinner.
  pings-stub      the stub pings but never sends INIT: the join deadline (NET_JOIN_TIMEOUT_SECONDS, 20 s) ends it.
  matched-gone    the client joined server_bin through the matchmaker (the launch seek) and the server is killed;
                  the matchmaker then lists no lobby (NoAvailableServers). The Lost Connection screen shows within
                  the same deadline, with no rejoin seek on a timer (v10 shows it the moment the socket closes;
                  servers_ui.c connection_dropped), and its Find Game ends on the screen with the matchmaker's
                  answer ("Servers are at Max Capacity"), not on a spinner.
  matched-down    as matched-gone, but the matchmaker itself is gone after the kill: Find Game ends on the screen
                  with "Could not reach the matchmaker".
  In every case the error must show within its deadline and serverinfo must not say "state OPEN" with no socket.

  python3 -I client/tests/crash_limbo_e2e.py BUILD_DIR [REPO_ROOT] [scenario ...]
Headless (KRUNKNATIVE_HEADLESS=1, null audio, no DISPLAY, private XDG dirs); no network beyond 127.0.0.1 (the
finder, the updates check and the krunker.io matchmaker are off; the log must name no krunk.io host).
Logs are kept in ~/.cache/kn-crash-limbo.* on failure. 77 = skip (no binaries or no map).
"""
import json
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

BUILD = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.path.abspath("cmake-build-debug")
ROOT = os.path.abspath(sys.argv[2]) if len(sys.argv) > 2 and os.path.isdir(sys.argv[2]) else os.path.dirname(BUILD)
SCENARIOS = [a for a in sys.argv[2:] if not os.path.isdir(a)] or ["kill-mid-round", "silent-stub", "pings-stub", "matched-gone", "matched-down"]
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
FIND_DEADLINE = 25  # s for the screen's Find Game to end on the Lost Connection screen again (a seek that fails)

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


class Matchmaker:
    """A stub brain / matchmaker on 127.0.0.1 (KRUNKNATIVE_BRAIN_URL): /mm/seek-game answers the lobby `lobby`
    (server_bin's port) while one is set, else NoAvailableServers (the lobby is gone); /mm/generate-token a
    token; anything else 404 (the ping list: the saved Default Region seeks without it). stop() takes it down
    (the matchmaker unreachable). Every request is logged as (time, path)."""

    def __init__(self):
        self.lobby = None
        self.log = []
        me = self

        class H(BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def do_GET(self):
                u = urllib.parse.urlsplit(self.path)
                me.log.append((time.time(), u.path))
                ctype = "application/json"
                if u.path == "/mm/generate-token":
                    status, data, ctype = 200, b"stub-token", "text/plain"
                elif u.path == "/mm/seek-game":
                    if me.lobby:
                        body = {"success": True, "host": "127.0.0.1", "port": me.lobby, "gameId": "LOC:crash1",
                                "clientId": "stub-ticket", "region": "local", "players": 1, "maxPlayers": 8}
                        status = 200
                    else:
                        status, body = 404, {"success": False, "error": "NoAvailableServers"}
                    data = json.dumps(body).encode()
                else:
                    status, data = 404, b'{"error":"NotFound"}'
                self.send_response(status)
                self.send_header("Content-Type", ctype)
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

        self.httpd = ThreadingHTTPServer(("127.0.0.1", 0), H)
        self.port = self.httpd.server_address[1]
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def url(self):
        return "http://127.0.0.1:%d" % self.port

    def seeks(self, since=0.0):
        return [t for t, p in self.log if p == "/mm/seek-game" and t >= since]

    def stop(self):
        self.httpd.shutdown()
        self.httpd.server_close()


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


def wait_text(cmds, log, needle, deadline, since=0):
    """Polls textof instructionsUpdate until a line after `since` holds `needle`: (seconds, text) or None."""
    t0 = time.time()
    while time.time() - t0 < deadline:
        with open(cmds, "a") as f:
            f.write("textof instructionsUpdate\n")
        time.sleep(1.0)
        texts = re.findall(r"textof instructionsUpdate = '(.*)'", read(log)[since:])
        if texts and needle in texts[-1]:
            return (time.time() - t0, texts[-1])
    return None


def run(name, deadline, expect, transport=None, stub_pings=None, kill_server=False, matched=None, expect_find=None):
    print("== %s (the error within %d s: %s)" % (name, deadline, expect), flush=True)
    base = os.path.join(WORK, name)
    os.makedirs(os.path.join(base, "cfg"))
    port = free_port()
    server = stub = client = mm = None
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
        env = {k: v for k, v in os.environ.items()
               if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_UI_DIR", "KRUNKNATIVE_BRAIN_URL", "KRUNKNATIVE_SERVER_PORT")}
        env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": os.path.join(base, "cfg"),
                    "XDG_STATE_HOME": os.path.join(base, "state"), "KRUNKNATIVE_NO_BROWSER": "1",
                    "KRUNKNATIVE_UPDATES_URL": "off", "KRUNKNATIVE_FINDER_URL": "off", "KRUNKNATIVE_KRUNKER_MATCHMAKER": "off",
                    "KRUNKNATIVE_UI_CMDS": cmds, "KRUNKNATIVE_WINDOW_SIZE": "1280x720"})
        args = [CLIENT, "--finder", "off"]
        if matched:
            # matched through the stub matchmaker: the launch seek (Auto Seek on Launch, on by default) joins its lobby
            # in the saved Default Region (no ping race), the way Quick Play / Find Game do
            mm = Matchmaker()
            mm.lobby = port
            os.makedirs(os.path.join(base, "cfg", "krunknative"))
            with open(os.path.join(base, "cfg", "krunknative", "knSettings.json"), "w") as f:
                json.dump({"defaultRegion": "local"}, f)
            env["KRUNKNATIVE_BRAIN_URL"] = mm.url()
        else:
            env["KRUNKNATIVE_SERVER_PORT"] = str(port)
        if transport:
            env["KRUNKNATIVE_NET_TRANSPORT"] = transport
        log = os.path.join(base, "client.log")
        client = spawn(["stdbuf", "-oL", "-eL"] + args, log, ROOT, env)

        t_kill = 0.0
        if kill_server:
            # joined: the client connected to the server (its session is up)
            end = time.time() + 90
            while time.time() < end and "connected to server" not in read(log):
                time.sleep(0.5)
            check("connected to server" in read(log), "%s: the client joined the server before the kill" % name)
            if matched:
                check("[mm] matchmaking through" in read(log) and len(mm.seeks()) >= 1,
                      "%s: the client joined through the matchmaker (%d seek)" % (name, len(mm.seeks())))
            time.sleep(3)
            server.send_signal(signal.SIGKILL)
            server.wait()
            t_kill = time.time()
            print("  server killed (SIGKILL), mid-round", flush=True)
            if matched == "gone":
                mm.lobby = None  # the matchmaker lists no lobby any more
            elif matched == "down":
                mm.stop()  # the matchmaker is gone too
                mm = None
        t0 = time.time()
        shown = wait_text(cmds, log, LOST, deadline)
        if shown:
            print("  %6.1f s  error on the screen: %s" % (shown[0], shown[1][:120]), flush=True)
        else:
            print("  %6.1f s  no error on the screen" % (time.time() - t0), flush=True)
        check(shown is not None, "%s: the client shows \"%s\" within %d s" % (name, LOST, deadline))
        if shown:
            check(expect in shown[1], "%s: the error says why (%s)" % (name, expect))
            check(shown[0] <= deadline, "%s: the error came in %.0f s (deadline %d s)" % (name, shown[0], deadline))
        if matched:
            # v10 shows the screen the moment the socket closes and rejoins nothing: no seek on a timer behind the spinner
            timed = len(mm.seeks(t_kill)) if mm else 0
            check(timed == 0, "%s: no rejoin seek on a timer after the loss (%d)" % (name, timed))
            check("rejoining" not in read(log).split("connection lost", 1)[-1][:400], "%s: the log says the loss is not rejoined" % name)
        # serverinfo after the loss: no state OPEN without a socket
        with open(cmds, "a") as f:
            f.write("con serverinfo\n")
        time.sleep(1.5)
        info = re.findall(r"\[console\] Connection: (.*)", read(log))
        check(bool(info) and "state OPEN" not in info[-1], "%s: serverinfo is not OPEN after the loss (%s)" % (name, info[-1] if info else "no output"))
        if matched:
            # the screen's Find Game (its first tab): a fresh seek, which fails here and must end on the screen again
            mark = len(read(log))
            with open(cmds, "a") as f:
                f.write("clicksel 0 .instructionsTab\n")
            time.sleep(0.5)
            check("[ui_cmds] sel .instructionsTab[0] rect" in read(log), "%s: the Lost Connection screen has its Find Game tab" % name)
            found = wait_text(cmds, log, expect_find, FIND_DEADLINE, since=mark)
            if found:
                print("  %6.1f s  after Find Game: %s" % (found[0], found[1][:120]), flush=True)
            check(found is not None, "%s: Find Game ends on the Lost Connection screen within %d s: \"%s\"" % (name, FIND_DEADLINE, expect_find))
            seeks = len(mm.seeks(t_kill)) if mm else None
            check(seeks is None or seeks >= 1, "%s: Find Game asked the matchmaker (%s seek)" % (name, seeks))
        check(client.poll() is None, "%s: the client is still running (back on the menu)" % name)
        # a test client talks to its own server only: no public server list, no krunk.io host
        check(not re.search(r"https?://[\w.-]*krunk\.io", read(log)), "%s: the client contacted no krunk.io host" % name)
        print("  log: %s" % log, flush=True)
    finally:
        stop(client)
        stop(server)
        stop(stub)
        if mm:
            mm.stop()


SCEN = {
    "kill-mid-round": dict(deadline=40, expect="no data for 10 s", kill_server=True),
    "silent-stub": dict(deadline=40, expect="no data for 15 s", transport="tcp", stub_pings=False),
    "pings-stub": dict(deadline=50, expect="no answer to the join within 20 s", transport="tcp", stub_pings=True),
    "matched-gone": dict(deadline=40, expect="no data for 10 s", kill_server=True, matched="gone", expect_find="Max Capacity"),
    "matched-down": dict(deadline=40, expect="no data for 10 s", kill_server=True, matched="down", expect_find="Could not reach the matchmaker"),
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
