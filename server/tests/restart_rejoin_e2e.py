#!/usr/bin/env python3
"""Server shutdown message and the client's rejoin with a fresh join ticket, end to end
(doc/backend.md 8.5 "shutdown", 9.4 "rejoin"; server/src/server_core.c server_shutdown_message,
client/src/ui/servers_ui.c servers_ui_connection_lost).

  python3 -I server/tests/restart_rejoin_e2e.py BUILD_DIR      (run from the repository root)

kn_brain on a scratch database (KN_TEST_DB_DRIVER, default postgres from config/config.json;
sqlite needs no server) with join_ttl_s 5, one brain-linked server_bin node that is stopped and
started again on the same port (a new lobby id each start, like a systemd restart); every port
from kn_proto.free_ports().

  1. protocol client: SIGTERM -> ERROR "Server shutting down" (004006), exit 0
  2. protocol client: SIGTERM with "update" in gameserver.shutdown_reason_file (a deploy) ->
     ERROR "GAME UPDATING" (004004, v7.1.4's text)
  3. protocol client: console `quit restart` -> ERROR "Server restarting" (004006)
  4. headless client_bin in brain mode, in a game, Settings > Client > Auto-restart for updates
     off: the deploy restart (reason "update" + SIGTERM, then a new server_bin). The client shows
     the reason, seeks again in place (never sends its old ticket), gets a fresh ticket for the
     restarted server's lobby and is back in the game (with the setting on, the default, the
     client restarts itself and rejoins instead: server/tests/auto_restart_e2e.py)
  5. client_bin: the server is killed (-9: no message), restarted after the ticket expired; the
     client shows the transport's reason ("Disconnected: the server closed the connection"),
     holds the transport (the old ticket is never resent), rejoins through the matchmaker with a
     fresh ticket for the restarted server's lobby and is back in the game
  6. client_bin against a server whose join_secret is wrong: every ticket is refused ("Invalid
     join key"); the client rejoins once, then shows "Could not rejoin: ... again"
  0. (first) resuming: a transport reconnect re-sends the seek ticket after it expired (the edges'
     mid-match "Join key expired"): a ticket the lobby admitted is accepted again, a guest's and an
     account's (the coordinator's auth with "resume"); the new connection replaces the old one; a
     never-admitted expired ticket is still refused

Needs a headless GL device (KRUNKNATIVE_HEADLESS=1: EGL). Exit 77 when the database is unavailable.
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

sys.path.insert(0, os.path.join(os.getcwd(), "server/tests"))
import kn_proto  # noqa: E402

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
ROOT = os.getcwd()
DRIVER = os.environ.get("KN_TEST_DB_DRIVER", "postgres")
NAME = "kn_test_rejoin_%d" % os.getpid()
SECRET = "kn-rejoin-%d" % os.getpid()
(GAME_PORT,) = kn_proto.free_ports(1)
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-rejoin-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
REASON_FILE = os.path.join(WORK, "stop-reason")
results = []
procs = []


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


def http(port, path):
    try:
        with urllib.request.urlopen("http://127.0.0.1:%d%s" % (port, path), timeout=10) as r:
            return r.status, json.loads(r.read())
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b"null")


def lobby(port):
    """The e2e node's listed lobby id, or None."""
    code, body = http(port, "/mm/game-list")
    ids = [g[0] for g in body.get("games", [])] if code == 200 else []
    return next((g for g in ids if g.startswith("RRJ:")), None)


class Node:
    """One server_bin start (the node keeps its id and port across restarts)."""
    count = 0

    def __init__(self, brain_port, join_secret=None):
        Node.count += 1
        self.log = os.path.join(WORK, "gs%d.log" % Node.count)
        gs = os.path.join(WORK, "gs%d.json" % Node.count)
        cfg = {"node_id": "RRJ", "name": "rejoin e2e", "region": "local", "listen_port": GAME_PORT,
               "public_host": "127.0.0.1", "brain_url": "ws://127.0.0.1:%d/mm/internal/ws" % brain_port,
               "brain_secret": SECRET, "modes": ["ffa"], "max_players": 6, "shutdown_grace": 1,
               "idle_kick_seconds": 0, "shutdown_reason_file": REASON_FILE}
        if join_secret:
            cfg["join_secret"] = join_secret
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

    def console(self, line):
        self.p.stdin.write((line + "\n").encode())
        self.p.stdin.flush()

    def stop(self, sig=signal.SIGTERM, timeout=15):
        """sig, then wait; SIGKILL with timeout set: wait that long first (a console quit)"""
        if sig == signal.SIGKILL and timeout:
            try:
                return self.p.wait(timeout)
            except subprocess.TimeoutExpired:
                pass
        if self.p.poll() is None:
            os.killpg(self.p.pid, sig)
        try:
            return self.p.wait(timeout or 15)
        except subprocess.TimeoutExpired:
            os.killpg(self.p.pid, signal.SIGKILL)
            return self.p.wait()


def seek(port):
    return http(port, "/mm/seek-game?hostname=krunknative&region=local&autoChangeGame=true")


def join_proto(port):
    """A protocol client in the node's lobby (seek ticket -> JOIN -> ENT -> our SPAWN)."""
    code, rep = seek(port)
    if code != 200:
        return None, "seek HTTP %d %s" % (code, rep)
    c = kn_proto.Conn("127.0.0.1", int(rep["port"]))
    c.hello()
    c.join(client_key=rep.get("clientId", ""), game_id=rep.get("gameId", ""))
    c.ent()
    end = time.time() + 8
    while time.time() < end:
        payload = c.read(until=kn_proto.SPAWN, timeout=max(0.1, end - time.time()))
        if payload is None:
            break
        sp = kn_proto.spawn_info(payload)
        if sp and sp["is_you"]:
            return c, rep["gameId"]
    return c, None


def shutdown_error(c, secs=10):
    payload = c.read(until=kn_proto.ERROR, timeout=secs)
    return kn_proto.error_info(payload) if payload is not None else None


def post(port, path, body):
    req = urllib.request.Request("http://127.0.0.1:%d%s" % (port, path), data=json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json"}, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status, json.loads(r.read())
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b"null")


def connect_spawn(rep, session=""):
    """JOIN with a seek reply's ticket; (conn, spawned, error)"""
    c = kn_proto.Conn("127.0.0.1", int(rep["port"]))
    c.hello()
    c.join(client_key=rep.get("clientId", ""), game_id=rep.get("gameId", ""), session=session)
    c.ent()
    end, seen = time.time() + 8, 0
    while time.time() < end:
        c.read(timeout=0.3)  # collects into c.packets
        for t, data in c.packets[seen:]:
            if t == kn_proto.ERROR:
                return c, False, kn_proto.error_info(data)
            sp = kn_proto.spawn_info(data) if t == kn_proto.SPAWN else None
            if sp and sp["is_you"]:
                return c, True, None
        seen = len(c.packets)
    return c, False, None


def resume_cases(brain_port):
    node = Node(brain_port)
    check("0: node up", node.linked and node.game, str(node.game))
    code, rep = seek(brain_port)
    c, ok, err = connect_spawn(rep) if code == 200 else (None, False, None)
    check("0: a guest joins with its ticket", ok, str(err))
    time.sleep(6)  # brain.join_ttl_s 5: the ticket has expired
    c.close()
    c, ok, err = connect_spawn(rep)
    check("0: the guest's transport reconnect resumes the expired ticket", ok, str(err))
    check("0: logged as a resume", "resumes join ticket" in node.text(), node.text()[-300:])
    c.close()

    code, stale = seek(brain_port)  # never used until it expired
    time.sleep(6)
    c, ok, err = connect_spawn(stale)
    check("0: a never-admitted expired ticket is still refused", not ok and err == ("Join key expired", "004001"), str(err))
    c.close()

    name = "rj%d" % (os.getpid() % 100000)
    st, reg = post(brain_port, "/auth/register", {"username": name, "password": "pw-rejoin-%d" % os.getpid()})
    token = reg.get("token", "") if isinstance(reg, dict) else ""
    check("0: account registered", st == 200 and token, str(reg)[:200])
    code, rep = http(brain_port, "/mm/seek-game?hostname=krunknative&region=local&autoChangeGame=true&token=" + token)
    c1, ok, err = connect_spawn(rep, session=token) if code == 200 else (None, False, None)
    check("0: the account joins", ok and node.text().count("authenticated as account") == 1, str(err))
    time.sleep(6)
    c2, ok, err = connect_spawn(rep, session=token)  # the old transport is still open (a ghost)
    check("0: the account's reconnect resumes the expired ticket (coordinator auth with resume)",
          ok and node.text().count("authenticated as account") == 2, "%s %s" % (err, node.text()[-300:]))
    old = c1.read(until=kn_proto.ERROR, timeout=3) if c1 else None
    check("0: the new connection replaces the old one", old is not None and kn_proto.error_info(old)[1] == "004002", str(old))
    for c in (c1, c2):
        if c:
            c.close()
    node.stop()


def protocol_cases(brain_port):
    # 1. SIGTERM, no reason
    node = Node(brain_port)
    check("node linked, lobby listed", node.linked and node.game, str(node.game))
    c, game = join_proto(brain_port)
    check("1: a player joins the lobby", game == node.game, str(game))
    node.stop()
    err = shutdown_error(c) if c else None
    check("1: SIGTERM -> ERROR 'Server shutting down' (004006)", err == ("Server shutting down", "004006"), str(err))
    check("1: server_bin exits 0 after the drain", node.p.returncode == 0, str(node.p.returncode))
    if c:
        c.close()

    # 2. a deploy: "update" in the reason file, then SIGTERM
    node = Node(brain_port)
    c, game = join_proto(brain_port)
    check("2: a player joins the restarted node's new lobby", game and game == node.game, str(game))
    with open(REASON_FILE, "w") as f:
        f.write("update\n")
    node.stop()
    os.unlink(REASON_FILE)
    err = shutdown_error(c) if c else None
    check("2: reason file 'update' -> ERROR 'GAME UPDATING' (004004)", err == ("GAME UPDATING", "004004"), str(err))
    check("2: the drain names the reason", "reason update" in node.text())
    if c:
        c.close()

    # 3. console `quit restart`
    node = Node(brain_port)
    c, game = join_proto(brain_port)
    check("3: a player joins", bool(game), str(game))
    node.console("quit restart")
    node.stop(signal.SIGKILL, 15)  # only when `quit` did not stop it
    err = shutdown_error(c) if c else None
    check("3: console 'quit restart' -> ERROR 'Server restarting' (004006)", err == ("Server restarting", "004006"), str(err))
    if c:
        c.close()


def client(name, brain_port, script, client_settings=None):
    """A headless client_bin in brain mode with an input script (started, not waited for);
    client_settings: the Settings > Client store (knClient.json) to start with."""
    path = os.path.join(WORK, name + ".script")
    with open(path, "w") as f:
        f.write(script)
    if client_settings is not None:
        store = os.path.join(WORK, name + "-cfg", "krunknative")
        os.makedirs(store, exist_ok=True)
        with open(os.path.join(store, "knClient.json"), "w") as f:
            json.dump(client_settings, f)
    log = os.path.join(WORK, name + ".log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": os.path.join(WORK, name + "-cfg"),
                "XDG_STATE_HOME": os.path.join(WORK, name + "-state"), "KRUNKNATIVE_NO_BROWSER": "1",
                "KRUNKNATIVE_INPUT_SCRIPT": path, "KRUNKNATIVE_WINDOW_SIZE": "640x360",
                "KRUNKNATIVE_BRAIN_URL": "http://127.0.0.1:%d" % brain_port, "KRUNKNATIVE_NET_TRANSPORT": "tcp"})
    p = subprocess.Popen(["stdbuf", "-oL", "-eL", f"{BUILD}/client_bin"], cwd=ROOT, stdout=open(log, "w"),
                         stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, start_new_session=True)
    procs.append(p)
    return p, log


def finish(p, secs):
    try:
        p.wait(secs)
        return True
    except subprocess.TimeoutExpired:
        os.killpg(p.pid, signal.SIGTERM)
        try:
            p.wait(5)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
        return False


def states(text):
    out = []
    for m in re.finditer(r"\[input_script\] state t=(\d+) slot=(-?\d+)/(\d+) weapon=(.+?) menu=(\d)", text):
        out.append({"t": int(m.group(1)), "loadout": int(m.group(3)), "weapon": m.group(4), "menu": int(m.group(5))})
    return out


def in_game(st):
    return bool(st) and st["menu"] == 0 and st["loadout"] > 0 and st["weapon"] != "-"


def joined(text):
    """game ids of the client's "[mm] joining <id> at host:port" lines"""
    return re.findall(r"\[mm\] joining (\S+) at ", text)


# The script clock starts at the first spawn; `respawn` enters the game again once the rejoined
# lobby's map is loaded (retried every frame).
SCRIPT = "0 play\n1000 state\n{respawn} respawn\n{state} state\n{quit} quit\n"


def client_restart(brain_port):
    node = Node(brain_port)
    check("4: node up", node.linked and node.game, str(node.game))
    old = node.game
    p, log = client("deploy", brain_port, SCRIPT.format(respawn=12000, state=26000, quit=27000), {"autoRestart": False})
    check("4: the client joins the lobby and plays", wait_for(lambda: states(read_log(log)), 40), read_log(log)[-400:])
    with open(REASON_FILE, "w") as f:
        f.write("update")
    node.stop()
    os.unlink(REASON_FILE)
    node2 = Node(brain_port)  # systemd: Restart=always (the same port, a new lobby id)
    check("4: the restarted node lists a new lobby", node2.game and node2.game != old, "%s -> %s" % (old, node2.game))
    finish(p, 60)
    text = read_log(log)
    check("4: the client got the server's reason", "server stopped (GAME UPDATING)" in text, text[-600:])
    check("4: Auto-restart off: no restart, the rejoin runs in place", "[reload] the server is restarting" not in text
          and "resume after the restart" not in text, text[-600:])
    check("4: it rejoined through the matchmaker with a fresh ticket",
          re.search(r"\[mm\] rejoin: %s with a fresh join ticket" % re.escape(node2.game or "-"), text) is not None
          and joined(text) == [old, node2.game], str(joined(text)))
    check("4: the old ticket never reached the restarted server", "Join key" not in node2.text())
    st = states(text)
    check("4: back in the game after the restart", len(st) >= 2 and in_game(st[-1]), str(st))
    return node2


def client_key_expired(brain_port, node):
    old = node.game
    p, log = client("expired", brain_port, SCRIPT.format(respawn=20000, state=34000, quit=35000))
    check("5: the client joins the lobby and plays", wait_for(lambda: states(read_log(log)), 40), read_log(log)[-400:])
    node.stop(signal.SIGKILL, 0)  # a crash: no message from the server, only the transport's
    time.sleep(6.5)  # brain.join_ttl_s 5: the old ticket expires
    node2 = Node(brain_port)
    finish(p, 60)
    text = read_log(log)
    check("5: the client shows the transport's reason (no server ERROR)",
          re.search(r"disconnected from server: the (server closed the connection|connection was reset)", text) is not None
          and "rejoining %s through the matchmaker" % old in text, text[-600:])
    check("5: the old ticket never reached the restarted server", "Join key" not in node2.text(), node2.text()[-400:])
    check("5: it rejoined with a fresh ticket for the restarted server's lobby",
          re.search(r"\[mm\] rejoin: %s with a fresh join ticket" % re.escape(node2.game or "-"), text) is not None, text[-600:])
    js = joined(text)
    check("5: joined the restarted server's lobby", len(js) >= 2 and js[0] == old and js[-1] == node2.game, str(js))
    st = states(text)
    check("5: back in the game", len(st) >= 2 and in_game(st[-1]), str(st))
    node2.stop()


def client_key_refused_twice(brain_port):
    node = Node(brain_port, join_secret="not-the-brain-secret-%d" % os.getpid())
    p, log = client("refused", brain_port, "0 play\n1000 quit\n")
    wait_for(lambda: "[mm] rejoin failed" in read_log(log), 30)
    finish(p, 1)
    text = read_log(log)
    check("6: the first refusal rejoins with a fresh ticket", "join key refused (Invalid join key)" in text
          and "[mm] rejoin: %s with a fresh join ticket" % node.game in text, text[-600:])
    check("6: the second refusal is shown, not retried",
          "rejoin failed: a fresh join ticket was refused too (Invalid join key)" in text and len(joined(text)) == 2,
          str(joined(text)))
    check("6: the server refused both tickets", len(re.findall(r"Rejecting \S+ \(id \d+\): Invalid join key", node.text())) == 2,
          node.text()[-300:])
    node.stop()


def main():
    if not os.access(f"{BUILD}/client_bin", os.X_OK):
        print("restart_rejoin_e2e: no client_bin in %s: skipped" % BUILD)
        return 77
    scratch = [sys.executable, "-I", os.path.join(ROOT, "services", "brain", "tests", "db_scratch.py"), ROOT]
    r = subprocess.run(scratch + ["create", DRIVER, NAME], capture_output=True)
    if r.returncode:
        print("restart_rejoin_e2e: %s unavailable: skipped" % DRIVER)
        return r.returncode
    dbenv = {"KN_DATABASE_" + k.upper(): str(v) for k, v in json.loads(r.stdout).items()}
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=SECRET, **dbenv)
    brain_log = os.path.join(WORK, "brain.log")
    try:
        subprocess.run([f"{BUILD}/services/brain/kn_brain", "--migrate-only"], cwd=ROOT, env=env, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        brain = subprocess.Popen([f"{BUILD}/services/brain/kn_brain", "--brain.host=127.0.0.1", "--brain.port=0",
                                  "--brain.region=local", "--brain.join_ttl_s=5", "--brain.id=rejoin-%d" % os.getpid()],
                                 cwd=ROOT, env=env, stdout=open(brain_log, "w"), stderr=subprocess.STDOUT,
                                 stdin=subprocess.DEVNULL, start_new_session=True)
        procs.append(brain)
        m = wait_for(lambda: re.search(r"listening on http://127\.0\.0\.1:(\d+) \(ws", read_log(brain_log)), 15)
        check("kn_brain starts", bool(m), read_log(brain_log)[-300:])
        if not m:
            return 1
        port = int(m.group(1))
        resume_cases(port)
        protocol_cases(port)
        node = client_restart(port)
        client_key_expired(port, node)
        client_key_refused_twice(port)
    finally:
        for p in procs:
            if p.poll() is None:
                os.killpg(p.pid, signal.SIGTERM)
        for p in procs:
            try:
                p.wait(10)
            except subprocess.TimeoutExpired:
                os.killpg(p.pid, signal.SIGKILL)
                p.wait()
        subprocess.run(scratch + ["drop", DRIVER, NAME], capture_output=True)
    if all(results):
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print("logs kept in " + WORK)
    print(f"{sum(results)}/{len(results)} passed")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
