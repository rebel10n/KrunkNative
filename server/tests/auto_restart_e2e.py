#!/usr/bin/env python3
"""Auto-restart for a server update, end to end (client/include/game_reload.h "auto-restart for
updates", Settings > Client > Auto-restart for updates, default on; server_core.c
server_shutdown_message: ERROR "GAME UPDATING" 004004 = restarting for an update, reconnect after).

  python3 -I server/tests/auto_restart_e2e.py BUILD_DIR [WEB_DIST_DIR]    (run from the repository root)

Every port from kn_proto.free_ports(); every process this test starts is stopped by it.

  A. native, matchmade lobby, started by a launcher: kn_brain on a scratch database
     (KN_TEST_DB_DRIVER, default postgres; sqlite needs no server), one brain-linked server_bin node,
     a headless client_bin in its lobby with KRUNKNATIVE_LAUNCHER = a stand-in launcher (records its
     arguments, waits for the pid it was given like the launcher's --wait-pid, then execs client_bin
     with the arguments after "--"). The deploy restart (reason "update" + SIGTERM, then a new
     server_bin): the client shows Game Updating, restarts through the launcher (a new pid; the old
     one waited for), the new process gets the session handoff (KRUNKNATIVE_RELOAD_RESUME "rejoin
     <old lobby>"), seeks that lobby with a fresh ticket (the restart rejoin's backoff while the
     matchmaker has none), lands in the restarted server's lobby and plays; the old ticket never
     reaches the restarted server.
  B. native, direct server (--connect), no launcher: console `quit update` -> ERROR 004004, the
     client re-executes itself (app_reload), the new process resumes "connect host:port", retries
     while the server is down and is back in game once it is up again.
  W. web (WEB_DIST_DIR with client.wasm, Chromium): the page on ?mode=offline&ws=<a WebSocket <-> TCP
     bridge in this test>&keep=..., connected to server_bin; `quit update`: the page shows Game
     Updating, reloads itself (location.reload: the same address, its query kept) and reconnects to
     the restarted server.

Needs a headless GL device for A and B (KRUNKNATIVE_HEADLESS=1: EGL). A is skipped when the database
is unavailable, W without a web build / Chromium / python websockets.
"""
import asyncio
import functools
import http.server
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
import urllib.request

sys.path.insert(0, os.path.join(os.getcwd(), "server/tests"))
import kn_proto  # noqa: E402

BUILD = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug")
DIST = os.path.abspath(sys.argv[2]) if len(sys.argv) > 2 else ""
ROOT = os.getcwd()
DRIVER = os.environ.get("KN_TEST_DB_DRIVER", "postgres")
NAME = "kn_test_autorestart_%d" % os.getpid()
SECRET = "kn-autorestart-%d" % os.getpid()
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-autorestart-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
REASON_FILE = os.path.join(WORK, "stop-reason")
results = []
procs = []        # Popen objects we started (killed by process group at the end)
extra_pids = []   # processes started for us by a stand-in launcher (their own session)


def check(name, ok, detail=""):
    results.append(bool(ok))
    show = detail and (not ok or len(detail) <= 160)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if show else ''}", flush=True)


def read_log(path):
    try:
        with open(path, errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def wait_for(fn, secs=10.0):
    end = time.time() + secs
    while time.time() < end:
        try:
            v = fn()
            if v:
                return v
        except Exception:  # noqa: BLE001 - polling
            pass
        time.sleep(0.1)
    return None


def alive(pid):
    try:
        os.kill(pid, 0)
    except OSError:
        return False
    try:  # a zombie of ours counts as gone
        with open("/proc/%d/stat" % pid) as f:
            return f.read().split(")")[-1].split()[0] != "Z"
    except OSError:
        return False


def kill_pid(pid):
    for sig in (signal.SIGTERM, signal.SIGKILL):
        if not alive(pid):
            return
        try:
            os.kill(pid, sig)
        except OSError:
            return
        wait_for(lambda: not alive(pid), 5)


def states(text):
    out = []
    for m in re.finditer(r"\[input_script\] state t=(\d+) slot=(-?\d+)/(\d+) weapon=(.+?) menu=(\d)", text):
        out.append({"t": int(m.group(1)), "loadout": int(m.group(3)), "weapon": m.group(4), "menu": int(m.group(5))})
    return out


def in_game(st):
    return bool(st) and st["menu"] == 0 and st["loadout"] > 0 and st["weapon"] != "-"


def joined(text):
    return re.findall(r"\[mm\] joining (\S+) at ", text)


# The old process plays until the restart replaces it (its quit is a fallback); the restarted one
# gets KRUNKNATIVE_INPUT_SCRIPT_RELAUNCH: it enters the game once the rejoined map is loaded.
OLD_SCRIPT = "0 play\n1000 state\n90000 quit\n"
NEW_SCRIPT = "0 play\n2000 state\n3000 quit\n"


def client_env(name, extra):
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL",
                                                            "KRUNKNATIVE_LAUNCHER", "KRUNKNATIVE_UPDATES_URL")}
    script, relaunch = os.path.join(WORK, name + ".script"), os.path.join(WORK, name + "-relaunch.script")
    with open(script, "w") as f:
        f.write(OLD_SCRIPT)
    with open(relaunch, "w") as f:
        f.write(NEW_SCRIPT)
    env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": os.path.join(WORK, name + "-cfg"),
                "XDG_STATE_HOME": os.path.join(WORK, name + "-state"), "KRUNKNATIVE_NO_BROWSER": "1",
                "KRUNKNATIVE_AUDIO_BACKEND": "null", "KRUNKNATIVE_INPUT_SCRIPT": script,
                "KRUNKNATIVE_INPUT_SCRIPT_RELAUNCH": relaunch, "KRUNKNATIVE_WINDOW_SIZE": "640x360",
                "KRUNKNATIVE_NET_TRANSPORT": "tcp"})
    env.update(extra)
    return env


def start_client(name, args, extra):
    log = os.path.join(WORK, name + ".log")
    p = subprocess.Popen(["stdbuf", "-oL", "-eL", f"{BUILD}/client_bin"] + args, cwd=ROOT, stdout=open(log, "w"),
                         stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=client_env(name, extra), start_new_session=True)
    procs.append(p)
    return p, log


def http_json(port, path):
    try:
        with urllib.request.urlopen("http://127.0.0.1:%d%s" % (port, path), timeout=10) as r:
            return r.status, json.loads(r.read())
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b"null")


def lobby(port):
    code, body = http_json(port, "/mm/game-list")
    ids = [g[0] for g in body.get("games", [])] if code == 200 else []
    return next((g for g in ids if g.startswith("ARS:")), None)


class Node:
    """One brain-linked server_bin start (the node keeps its id and port across restarts)."""
    count = 0

    def __init__(self, brain_port, game_port):
        Node.count += 1
        self.log = os.path.join(WORK, "gs%d.log" % Node.count)
        gs = os.path.join(WORK, "gs%d.json" % Node.count)
        cfg = {"node_id": "ARS", "name": "auto-restart e2e", "region": "local", "listen_port": game_port,
               "public_host": "127.0.0.1", "brain_url": "ws://127.0.0.1:%d/mm/internal/ws" % brain_port,
               "brain_secret": SECRET, "modes": ["ffa"], "max_players": 6, "shutdown_grace": 1,
               "idle_kick_seconds": 0, "shutdown_reason_file": REASON_FILE}
        with open(gs, "w") as f:
            json.dump({"gameserver": cfg}, f)
        self.p = subprocess.Popen([f"{BUILD}/server_bin", "--headless", "--config", gs, "--cfg", "none"], cwd=ROOT,
                                  stdout=open(self.log, "w"), stderr=subprocess.STDOUT, stdin=subprocess.PIPE,
                                  start_new_session=True)
        procs.append(self.p)
        self.linked = bool(wait_for(lambda: "welcomed" in read_log(self.log), 20))
        self.game = wait_for(lambda: lobby(brain_port), 10)

    def text(self):
        return read_log(self.log)

    def stop(self):
        if self.p.poll() is None:
            os.killpg(self.p.pid, signal.SIGTERM)
        try:
            return self.p.wait(15)
        except subprocess.TimeoutExpired:
            os.killpg(self.p.pid, signal.SIGKILL)
            return self.p.wait()


class DirectServer:
    """server_bin without a brain on a fixed port, its console on stdin."""
    count = 0

    def __init__(self, port):
        DirectServer.count += 1
        self.log = os.path.join(WORK, "direct%d.log" % DirectServer.count)
        self.p = subprocess.Popen([f"{BUILD}/server_bin", "--headless", "--cfg", "none", "--port", str(port)], cwd=ROOT,
                                  stdout=open(self.log, "w"), stderr=subprocess.STDOUT, stdin=subprocess.PIPE,
                                  start_new_session=True)
        procs.append(self.p)
        self.up = bool(wait_for(lambda: "Listening on" in read_log(self.log), 15))

    def text(self):
        return read_log(self.log)

    def quit_update(self):
        try:
            self.p.stdin.write(b"quit update\n")
            self.p.stdin.flush()
        except OSError:
            pass
        try:
            return self.p.wait(15)
        except subprocess.TimeoutExpired:
            os.killpg(self.p.pid, signal.SIGKILL)
            return self.p.wait()

    def stop(self):
        if self.p.poll() is None:
            os.killpg(self.p.pid, signal.SIGTERM)
            try:
                self.p.wait(10)
            except subprocess.TimeoutExpired:
                os.killpg(self.p.pid, signal.SIGKILL)
                self.p.wait()


def fake_launcher(record):
    """A stand-in for the launcher a client restarts through: krunknative --wait-pid PID -- ARGS.
    It records its pid and arguments, waits for PID like the launcher does (30 s at most), then
    becomes the new client (exec: its pid is the new client's)."""
    path = os.path.join(WORK, "fake-launcher.sh")
    with open(path, "w") as f:
        f.write("#!/bin/sh\n"
                f"echo \"$$ $*\" >> '{record}'\n"
                "pid=''\n"
                "if [ \"$1\" = --wait-pid ]; then pid=$2; shift 2; fi\n"
                "i=0\n"
                "while [ -n \"$pid\" ] && kill -0 \"$pid\" 2>/dev/null && [ $i -lt 300 ]; do sleep 0.1; i=$((i+1)); done\n"
                f"echo \"waited $i\" >> '{record}'\n"
                "[ \"$1\" = -- ] && shift\n"
                f"exec '{BUILD}/client_bin' \"$@\"\n")
    os.chmod(path, 0o755)
    return path


# ---------------------------------------------------------------- A: lobby, through the launcher

def case_lobby(brain_port, game_port):
    node = Node(brain_port, game_port)
    check("A: node up, lobby listed", node.linked and node.game, str(node.game))
    old = node.game
    record = os.path.join(WORK, "launcher-record.txt")
    p, log = start_client("lobby", [], {"KRUNKNATIVE_BRAIN_URL": "http://127.0.0.1:%d" % brain_port,
                                        "KRUNKNATIVE_LAUNCHER": fake_launcher(record)})
    old_pid = p.pid
    check("A: the client joins the lobby and plays", wait_for(lambda: states(read_log(log)), 40), read_log(log)[-400:])
    with open(REASON_FILE, "w") as f:
        f.write("update")
    node.stop()
    os.unlink(REASON_FILE)
    gone = wait_for(lambda: p.poll() is not None, 20)  # the old client closes for the launcher
    node2 = Node(brain_port, game_port)  # systemd: Restart=always (the same port, a new lobby id)
    check("A: the restarted node lists a new lobby", node2.game and node2.game != old, "%s -> %s" % (old, node2.game))
    rec = wait_for(lambda: read_log(record) if "waited" in read_log(record) else None, 20) or ""
    first = rec.splitlines()[0].split() if rec else []
    new_pid = int(first[0]) if first and first[0].isdigit() else 0
    if new_pid:
        extra_pids.append(new_pid)
    check("A: the old client exited for the restart", gone is not None, read_log(log)[-400:])
    check("A: restarted through the launcher with --wait-pid <old pid> (the same command line after --)",
          first[1:] == ["--wait-pid", str(old_pid)], rec)
    check("A: a new process (new pid)", new_pid and new_pid != old_pid, "old %d new %d" % (old_pid, new_pid))
    wait_for(lambda: new_pid and not alive(new_pid), 90)
    if new_pid and alive(new_pid):
        kill_pid(new_pid)
        check("A: the restarted client finished its script", False, read_log(log)[-600:])
    text = read_log(log)
    before, _, after = text.partition("resume after the restart")
    check("A: Game Updating: the server's ERROR 004004 on the live session",
          "disconnected from server: GAME UPDATING (004004)" in before, before[-600:])
    check("A: the client restarts for the update (no in-place rejoin)",
          "[reload] the server is restarting for an update" in before and "[mm] server stopped" not in before, before[-600:])
    check("A: the handoff: the new process resumes the old lobby", after.split("\n", 1)[0].strip() == ": rejoin %s" % old,
          after[:300])
    check("A: it rejoins through the matchmaker with a fresh ticket for the restarted server's lobby",
          re.search(r"\[mm\] rejoin: %s with a fresh join ticket" % re.escape(node2.game or "-"), after) is not None
          and joined(after) == [node2.game], str(joined(after)) + " " + after[-500:])
    check("A: the old ticket never reached the restarted server", "Join key" not in node2.text(), node2.text()[-300:])
    st = states(after)
    check("A: back in the game after the restart", st and in_game(st[-1]), str(st))
    node2.stop()


# ---------------------------------------------------------------- B: direct server, exec

def case_direct():
    (port,) = kn_proto.free_ports(1)
    server = DirectServer(port)
    check("B: direct server up", server.up, server.text()[-300:])
    p, log = start_client("direct", ["--connect", "127.0.0.1:%d" % port], {})
    check("B: the client joins and plays", wait_for(lambda: states(read_log(log)), 40), read_log(log)[-400:])
    server.quit_update()
    restarted = wait_for(lambda: "[app] reload: restarting" in read_log(log), 15)
    time.sleep(2.0)  # the server stays down a moment: the new process retries meanwhile
    server2 = DirectServer(port)
    try:
        p.wait(90)
    except subprocess.TimeoutExpired:
        os.killpg(p.pid, signal.SIGKILL)
        p.wait()
        check("B: the restarted client finished its script", False, read_log(log)[-600:])
    text = read_log(log)
    before, _, after = text.partition("resume after the restart")
    check("B: Game Updating (004004)", "disconnected from server: GAME UPDATING (004004)" in before, before[-500:])
    check("B: the client re-executes itself (no launcher)", restarted and "[reload] the server restarted for an update: relaunching" in before,
          before[-500:])
    check("B: the handoff: the new process resumes the same server", after.split("\n", 1)[0].strip() == ": connect 127.0.0.1:%d" % port,
          after[:200])
    check("B: back on the restarted server", ("connected to server 127.0.0.1:%d" % port) in after
          and "connected from 127.0.0.1" in server2.text(), after[-500:])
    st = states(after)
    check("B: back in the game", st and in_game(st[-1]), str(st))
    server2.stop()


# ---------------------------------------------------------------- W: web, page reload

class Bridge:
    """ws://127.0.0.1:<port>/native <-> the TCP game server: the bytes as they are (net-bridge's
    passthrough, net-bridge/webBridge.ts), in a thread with its own event loop."""

    def __init__(self, port, target):
        import websockets
        self.port, self.target, self.ws = port, target, websockets
        self.loop = asyncio.new_event_loop()
        self.ready = threading.Event()
        threading.Thread(target=self.run, daemon=True).start()
        self.ready.wait(10)

    async def pipe(self, ws):
        try:
            reader, writer = await asyncio.open_connection("127.0.0.1", self.target)
        except OSError:
            await ws.close(1011, "game server down")
            return

        async def up():
            async for m in ws:
                writer.write(m if isinstance(m, bytes) else m.encode())
                await writer.drain()

        async def down():
            while True:
                data = await reader.read(65536)
                if not data:
                    break
                await ws.send(data)

        tasks = [asyncio.ensure_future(up()), asyncio.ensure_future(down())]
        await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
        for t in tasks:
            t.cancel()
        writer.close()
        await ws.close()

    def run(self):
        asyncio.set_event_loop(self.loop)

        async def main():
            # "binary" when the page offers it (Emscripten's default), else none: net-bridge accepts both
            def select(_conn, offered):
                return "binary" if "binary" in offered else None

            async with self.ws.serve(self.pipe, "127.0.0.1", self.port, select_subprotocol=select, max_size=None):
                self.ready.set()
                await asyncio.Future()

        try:
            self.loop.run_until_complete(main())
        except Exception:  # noqa: BLE001 - torn down with the test
            pass


def case_web():
    chrome = shutil.which("chromium-browser") or shutil.which("chromium") or shutil.which("google-chrome")
    try:
        import websockets  # noqa: F401
    except ImportError:
        websockets = None
    if not DIST or not os.path.isfile(os.path.join(DIST, "client.wasm")) or not chrome or websockets is None:
        print("SKIP  W: needs WEB_DIST_DIR with a web build, Chromium and python websockets", flush=True)
        return
    game, http_port, ws_port, cdp_port = kn_proto.free_ports(4)
    server = DirectServer(game)
    check("W: server up", server.up, server.text()[-300:])
    Bridge(ws_port, game)
    handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=DIST)
    handler.log_message = lambda *a: None
    httpd = http.server.ThreadingHTTPServer(("127.0.0.1", http_port), handler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    browser = subprocess.Popen([chrome, "--headless=new", "--no-sandbox", "--use-gl=angle", "--use-angle=swiftshader",
                                "--enable-unsafe-swiftshader", "--remote-debugging-port=%d" % cdp_port,
                                "--user-data-dir=" + os.path.join(WORK, "chrome"), "--window-size=1280,720", "about:blank"],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
    procs.append(browser)
    url = "http://127.0.0.1:%d/?mode=offline&ws=ws://127.0.0.1:%d/native&keep=auto-restart" % (http_port, ws_port)
    servers = [server]

    async def session():
        import websockets as wsl
        target = None
        for _ in range(100):
            try:
                with urllib.request.urlopen("http://127.0.0.1:%d/json/list" % cdp_port, timeout=2) as r:
                    target = next((t for t in json.load(r) if t.get("type") == "page"), None)
                if target:
                    break
            except OSError:
                pass
            await asyncio.sleep(0.1)
        if not target:
            check("W: Chromium DevTools came up", False)
            return
        async with wsl.connect(target["webSocketDebuggerUrl"], max_size=1 << 26) as ws:
            ids = iter(range(1, 1 << 30))
            console, replies, loads = [], {}, []

            async def reader():
                async for raw in ws:
                    msg = json.loads(raw)
                    if msg.get("method") == "Runtime.consoleAPICalled":
                        console.append(" ".join(str(a.get("value", a.get("description", ""))) for a in msg["params"].get("args", [])))
                    elif msg.get("method") == "Page.loadEventFired":
                        loads.append(len(console))
                    elif "id" in msg:
                        replies[msg["id"]] = msg

            task = asyncio.create_task(reader())

            async def call(method, **params):
                i = next(ids)
                await ws.send(json.dumps({"id": i, "method": method, "params": params}))
                for _ in range(600):
                    if i in replies:
                        return replies.pop(i).get("result", {})
                    await asyncio.sleep(0.05)
                return {}

            async def ev(expr):
                r = await call("Runtime.evaluate", returnByValue=True, expression=expr)
                return r.get("result", {}).get("value")

            async def until(pred, seconds):
                for _ in range(int(seconds * 10)):
                    if pred():
                        return True
                    await asyncio.sleep(0.1)
                return False

            def connected_after(mark):
                return any("connected to server" in line for line in console[mark:])

            screens = []

            async def watch_screens(seconds, stop):
                for _ in range(int(seconds * 5)):
                    text = await ev("(() => { const u = document.getElementById('instructionsUpdate');"
                                    " return u && getComputedStyle(u).display !== 'none' ? u.innerText : ''; })()")
                    text = re.sub(r"\s+", " ", text or "").strip()
                    if text and (not screens or screens[-1] != text):
                        screens.append(text)
                    if stop():
                        return
                    await asyncio.sleep(0.2)

            await call("Runtime.enable")
            await call("Page.enable")
            await call("Page.navigate", url=url)
            ok = await until(lambda: connected_after(0), 180)
            ready = False
            for _ in range(120):  # the menu is ready (INIT seen) once its play prompt shows
                if "CLICK TO PLAY" in (await ev("(() => { const i = document.getElementById('instructions'); return i ? i.textContent : ''; })()") or ""):
                    ready = True
                    break
                await asyncio.sleep(0.5)
            check("W: the page connects to the server (menu ready)", ok and ready, "\n".join(console[-15:]))
            query_before = await ev("location.search")
            reloads_before = len(loads)
            mark = len(console)
            await asyncio.get_running_loop().run_in_executor(None, servers[0].quit_update)
            await watch_screens(20, lambda: len(loads) > reloads_before)
            reloaded = await until(lambda: len(loads) > reloads_before, 20)
            # a restarting server takes a while: the reloaded page arrives first, its bridge has no
            # game server yet (1011) and the transport's retry has to bring it back
            down = await until(lambda: reloaded and any("websocket closed (code 1011" in line for line in console[loads[-1]:]), 30)
            check("W: the reloaded page found the server still down (the transport retries)", down, "\n".join(console[-8:]))
            await asyncio.get_running_loop().run_in_executor(None, lambda: servers.append(DirectServer(game)))
            # "connected to server" is the WebSocket's open (the bridge answers while its game server is
            # still down): back = the restarted server has the player, after the transport's retry
            back = await until(lambda: connected_after(loads[-1] if loads else mark) and len(servers) > 1
                               and "connected from 127.0.0.1" in servers[-1].text(), 120)
            query_after = await ev("location.search")
            log = "\n".join(console[mark:])
            check("W: the page got the server's ERROR (GAME UPDATING, 004004)", "disconnected from server: GAME UPDATING (004004)" in log,
                  log[-600:])
            check("W: the error screen said Game Updating and Restarting...",
                  any("Game Updating" in s and "Restarting..." in s for s in screens), " || ".join(screens)[-400:])
            check("W: the page reloaded itself for the update", reloaded and "reloading the page" in log, log[-600:])
            check("W: the reload kept the page's query (?ws= / ?mode= / the rest)", query_before and query_after == query_before,
                  "%s -> %s" % (query_before, query_after))
            check("W: reconnected to the restarted server after the reload", back and len(servers) > 1
                  and "connected from 127.0.0.1" in servers[-1].text(), "\n".join(console[-15:]))
            task.cancel()
            with open(os.path.join(WORK, "web-console.log"), "w") as f:
                f.write("\n".join(console) + "\n")

    try:
        asyncio.run(asyncio.wait_for(session(), 480))
    except asyncio.TimeoutError:
        check("W: finished in time", False)
    finally:
        httpd.shutdown()
        for s in servers:
            s.stop()


def main():
    if not os.access(f"{BUILD}/client_bin", os.X_OK):
        print("auto_restart_e2e: no client_bin in %s: skipped" % BUILD)
        return 77
    scratch = [sys.executable, "-I", os.path.join(ROOT, "services", "brain", "tests", "db_scratch.py"), ROOT]
    db = None
    cases = os.environ.get("KN_AUTORESTART_CASES", "ABW")  # e.g. W: the web case only
    try:
        if "B" in cases:
            case_direct()
        r = subprocess.run(scratch + ["create", DRIVER, NAME], capture_output=True) if "A" in cases else None
        if r is None:
            pass
        elif r.returncode:
            print("SKIP  A: %s unavailable" % DRIVER, flush=True)
        else:
            db = {"KN_DATABASE_" + k.upper(): str(v) for k, v in json.loads(r.stdout).items()}
            env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=SECRET, **db)
            brain_log = os.path.join(WORK, "brain.log")
            subprocess.run([f"{BUILD}/services/brain/kn_brain", "--migrate-only"], cwd=ROOT, env=env, check=True,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            brain = subprocess.Popen([f"{BUILD}/services/brain/kn_brain", "--brain.host=127.0.0.1", "--brain.port=0",
                                      "--brain.region=local", "--brain.join_ttl_s=5", "--brain.id=autorestart-%d" % os.getpid()],
                                     cwd=ROOT, env=env, stdout=open(brain_log, "w"), stderr=subprocess.STDOUT,
                                     stdin=subprocess.DEVNULL, start_new_session=True)
            procs.append(brain)
            m = wait_for(lambda: re.search(r"listening on http://127\.0\.0\.1:(\d+) \(ws", read_log(brain_log)), 15)
            check("kn_brain starts", bool(m), read_log(brain_log)[-300:])
            if m:
                case_lobby(int(m.group(1)), kn_proto.free_ports(1)[0])
        if "W" in cases:
            case_web()
    finally:
        for pid in extra_pids:
            kill_pid(pid)
        for p in procs:
            if p.poll() is None:
                try:
                    os.killpg(p.pid, signal.SIGTERM)
                except OSError:
                    pass
        for p in procs:
            try:
                p.wait(10)
            except subprocess.TimeoutExpired:
                os.killpg(p.pid, signal.SIGKILL)
                p.wait()
        if db is not None:
            subprocess.run(scratch + ["drop", DRIVER, NAME], capture_output=True)
    if results and all(results):
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print("logs kept in " + WORK)
    print(f"{sum(results)}/{len(results)} passed")
    return 0 if results and all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
