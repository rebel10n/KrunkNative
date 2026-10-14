#!/usr/bin/env python3
"""Krunker v10 Invite / Join end to end (client/src/ui/v10_party.c, services/brain/game_presence.c).

  python3 -I server/tests/v10_party_e2e.py BUILD_DIR      (run from the repository root)

kn_brain on a scratch database (KN_TEST_DB_DRIVER, default sqlite: no server needed) and three
brain-linked server_bin nodes: PTA and PTB (public) and PTC (gameserver.password); every port from
kn_proto.free_ports(). Two accounts (alice, bob) follow each other: v10's friends.

  1. presence (protocol clients + JSON social sockets): alice joins PTA with her session -> bob, her
     friend, gets ["eb", {"type":"presence:game","id":alice,"gameInstanceId":"PTA:..",
     "gamePasswordProtected":false}], carol (who only follows alice) gets nothing; alice in PTC
     -> gamePasswordProtected true; alice leaves -> gameInstanceId null
  2. Invite (headless client_bin on the v10 page, alice, in PTA): a click on #inviteButton copies
     https://krunk.io/?game=<id> (the clipboard), the button says "Copied URL" (it gets wider) and
     "Invite" again 1250 ms later; bob's social socket gets alice's presence:game for PTA
  3. Join from the clipboard (bob, in PTB): the link pasted into the clipboard, a click on
     #menuBtnJoin -> bob lands in alice's game (PTA)
  4. Join window (bob, in PTB): showWindow(24), the bare code typed into #gameURL, Join -> PTA
  5. a friend's Join (bob, in PTB): openURL('/?game=<alice's gameInstanceId from presence:game>', 1)
     (the profile window's Friends tab) -> PTA
  6. a password-protected game (bob, in PTB, joins PTC): refused (004005) -> v10's "Enter Server
     Password to join" popup; the password typed + Submit -> in PTC

Needs a headless GL device (KRUNKNATIVE_HEADLESS=1: EGL) and python websockets. Exit 77 when the
database or websockets is unavailable.
"""
import asyncio
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

try:
    import websockets
except ImportError:  # pragma: no cover
    print("v10_party_e2e: python websockets missing: skipped")
    sys.exit(77)

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
ROOT = os.getcwd()
DRIVER = os.environ.get("KN_TEST_DB_DRIVER", "sqlite")
NAME = "kn_test_party_%d" % os.getpid()
SECRET = "kn-party-%d" % os.getpid()
PASSWORD = "pw%d" % (os.getpid() % 10000)
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-party-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
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


def http(port, path, body=None):
    req = urllib.request.Request("http://127.0.0.1:%d%s" % (port, path),
                                 data=json.dumps(body).encode() if body is not None else None,
                                 headers={"Content-Type": "application/json"} if body is not None else {})
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status, json.loads(r.read())
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b"null")


def lobby_of(port, node):
    code, body = http(port, "/mm/game-list")
    ids = [g[0] for g in body.get("games", [])] if code == 200 else []
    return next((g for g in ids if g.startswith(node + ":")), None)


class Node:
    def __init__(self, brain_port, node_id, game_port, password=""):
        self.node_id = node_id
        self.log = os.path.join(WORK, node_id + ".log")
        gs = os.path.join(WORK, node_id + ".json")
        cfg = {"node_id": node_id, "name": "party e2e " + node_id, "region": "local", "listen_port": game_port,
               "public_host": "127.0.0.1", "brain_url": "ws://127.0.0.1:%d/mm/internal/ws" % brain_port,
               "brain_secret": SECRET, "modes": ["ffa"], "max_players": 8, "shutdown_grace": 1, "idle_kick_seconds": 0}
        if password:
            cfg["password"] = password
        with open(gs, "w") as f:
            json.dump({"gameserver": cfg}, f)
        self.p = subprocess.Popen([f"{BUILD}/server_bin", "--headless", "--config", gs, "--cfg", "none"], cwd=ROOT,
                                  stdout=open(self.log, "w"), stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                                  start_new_session=True)
        procs.append(self.p)
        self.linked = bool(wait_for(lambda: "welcomed" in read_log(self.log), 20))
        self.game = wait_for(lambda: lobby_of(brain_port, node_id), 10)
        self.port = game_port

    def text(self):
        return read_log(self.log)


# ------------------------------------------------------------------ social sockets (JSON)

class Social:
    def __init__(self, url):
        self.url = url + "?enc=json"
        self.inbox = []
        self.ws = None

    async def open(self):
        self.ws = await websockets.connect(self.url, max_size=4 << 20)
        await self.recv("io-init")
        return self

    async def close(self):
        await self.ws.close()

    async def send(self, *frame):
        await self.ws.send(json.dumps(list(frame)))

    async def pump(self, timeout):
        try:
            msg = await asyncio.wait_for(self.ws.recv(), timeout)
        except asyncio.TimeoutError:
            return
        if isinstance(msg, str):
            self.inbox.append(json.loads(msg))

    async def recv(self, label, timeout=10.0, match=None):
        deadline = time.time() + timeout
        while True:
            for i, f in enumerate(self.inbox):
                if f and f[0] == label and (match is None or match(f)):
                    return self.inbox.pop(i)
            left = deadline - time.time()
            if left <= 0:
                return None
            await self.pump(left)

    async def call(self, reply, *frame, timeout=10.0):
        await self.send(*frame)
        return await self.recv(reply, timeout)


def presence(sock_frame):
    """["eb", {"type": "presence:game", ...}] -> the object, else None"""
    o = sock_frame[1] if sock_frame and len(sock_frame) > 1 else None
    return o if isinstance(o, dict) and o.get("type") == "presence:game" else None


def is_presence(account_id, game_prefix=None, null=False):
    def m(f):
        o = presence(f)
        if not o or o.get("id") != account_id:
            return False
        if null:
            return o.get("gameInstanceId") is None
        return isinstance(o.get("gameInstanceId"), str) and (game_prefix is None or o["gameInstanceId"].startswith(game_prefix))
    return m


class Loop:
    """An asyncio loop on its own thread: the social sockets stay open while client_bin runs."""

    def __init__(self):
        self.loop = asyncio.new_event_loop()
        self.t = threading.Thread(target=self.loop.run_forever, daemon=True)
        self.t.start()

    def run(self, coro, timeout=60):
        return asyncio.run_coroutine_threadsafe(coro, self.loop).result(timeout)

    def stop(self):
        self.loop.call_soon_threadsafe(self.loop.stop)


# ------------------------------------------------------------------ clients

def client(name, brain_port, script, game, account):
    """A headless client_bin on the v10 page in brain mode, signed in as account (name, token),
    starting in `game` (KRUNKNATIVE_GAME), with an input script. Started, not waited for."""
    path = os.path.join(WORK, name + ".script")
    with open(path, "w") as f:
        f.write(script)
    cfg = os.path.join(WORK, name + "-cfg")
    folder = os.path.join(cfg, "krunknative")
    os.makedirs(folder, exist_ok=True)
    for key, value in (("krunker_username", account[0]), ("krunker_token", account[1]), ("consent", "1")):
        with open(os.path.join(folder, key + ".json"), "w") as f:
            f.write(value)
    log = os.path.join(WORK, name + ".log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": cfg, "XDG_STATE_HOME": os.path.join(WORK, name + "-state"),
                "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_INPUT_SCRIPT": path, "KRUNKNATIVE_WINDOW_SIZE": "1280x720",
                "KRUNKNATIVE_BRAIN_URL": "http://127.0.0.1:%d" % brain_port, "KRUNKNATIVE_NET_TRANSPORT": "tcp", "KRUNKNATIVE_AUDIO_BACKEND": "null", "KRUNKNATIVE_UPDATES_URL": "off",
                "KRUNKNATIVE_GAME": game})
    p = subprocess.Popen(["stdbuf", "-oL", "-eL", f"{BUILD}/client_bin"], cwd=ROOT, stdout=open(log, "w"),
                         stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, start_new_session=True)
    procs.append(p)
    return p, log


def stop(p, secs=0):
    try:
        p.wait(secs)
        return
    except subprocess.TimeoutExpired:
        pass
    os.killpg(p.pid, signal.SIGTERM)
    try:
        p.wait(5)
    except subprocess.TimeoutExpired:
        os.killpg(p.pid, signal.SIGKILL)
        p.wait()


def joined(text):
    """game ids of the client's "[mm] joining <id> at host:port" lines"""
    return re.findall(r"\[mm\] joining (\S+) at ", text)


def rects(text, label, elem):
    m = re.search(r"\[input_script\] rect t=\d+ @%s %s=([-\d.]+),([-\d.]+),([-\d.]+)" % (label, elem), text)
    return tuple(map(float, m.groups())) if m else None


def same_game(a, b):
    """a link / short id and a canonical id name the same lobby (the node and the lobby part)"""
    return bool(a and b) and a.split(":")[0][:3] == b.split(":")[0][:3] and a.split(":")[-1] == b.split(":")[-1]


# ------------------------------------------------------------------ cases

async def presence_cases(base, nodes, alice, bob, carol):
    pta, ptc = nodes["PTA"], nodes["PTC"]
    b, c = await Social(base).open(), await Social(base).open()
    r = await b.call("a", "a", 1, [bob["name"], None, bob["token"]])
    r2 = await c.call("a", "a", 1, [carol["name"], None, carol["token"]])
    check("1: bob and carol signed in on their social sockets", r and r[1] is None and r2 and r2[1] is None, str(r)[:200])

    code, rep = http(nodes["port"], "/mm/seek-game?hostname=krunknative&region=local&autoChangeGame=false&game=%s&token=%s"
                     % (pta.game, alice["token"]))
    conn = kn_proto.Conn("127.0.0.1", int(rep["port"])) if code == 200 else None
    if conn:
        conn.hello()
        conn.join(client_key=rep.get("clientId", ""), game_id=rep.get("gameId", ""), session=alice["token"])
        conn.ent()
    f = await b.recv("eb", 10, is_presence(alice["id"], "PTA:"))
    o = presence(f) or {}
    check("1: alice joins PTA -> bob (a friend) gets presence:game with PTA's game id",
          o.get("gameInstanceId") == pta.game and o.get("gamePasswordProtected") is False, str(f))
    f = await c.recv("eb", 2, is_presence(alice["id"]))
    check("1: carol (follows alice, not followed back) gets nothing", f is None, str(f))
    if conn:
        conn.close()
    f = await b.recv("eb", 10, is_presence(alice["id"], null=True))
    check("1: alice leaves -> presence:game with gameInstanceId null", f is not None, str(b.inbox[-3:]))

    code, rep = http(nodes["port"], "/mm/seek-game?hostname=krunknative&region=local&autoChangeGame=false&game=%s&token=%s"
                     % (ptc.game, alice["token"]))
    conn = kn_proto.Conn("127.0.0.1", int(rep["port"])) if code == 200 else None
    if conn:
        conn.hello()
        conn.join(client_key=rep.get("clientId", ""), game_id=rep.get("gameId", ""), session=alice["token"], password=PASSWORD)
        conn.ent()
    f = await b.recv("eb", 10, is_presence(alice["id"], "PTC:"))
    o = presence(f) or {}
    check("1: alice in the password-protected PTC -> gamePasswordProtected true", o.get("gamePasswordProtected") is True, str(f))
    if conn:
        conn.close()
    await b.recv("eb", 10, is_presence(alice["id"], null=True))
    await c.close()
    return b  # bob's socket stays open for the client cases


def client_cases(loop, brain_port, nodes, alice, bob, bob_sock):
    pta, ptb, ptc = nodes["PTA"], nodes["PTB"], nodes["PTC"]
    acc_a, acc_b = (alice["name"], alice["token"]), (bob["name"], bob["token"])

    # 2. Invite: alice in PTA, on the menu
    a, a_log = client("alice", brain_port, "\n".join([
        "0 menu",
        "1500 rect @before inviteButton",
        "1600 clickid inviteButton",
        "1700 rect @copied inviteButton",
        "1800 clip @invite",
        "3300 rect @after inviteButton",
        "240000 quit"]) + "\n", pta.game, acc_a)
    text = wait_for(lambda: (lambda t: t if "@after" in t else None)(read_log(a_log)), 90) or read_log(a_log)
    m = re.search(r"\[party\] invite link copied: (\S+)", text)
    link = m.group(1) if m else ""
    clip = re.search(r"\[input_script\] clip t=\d+ @invite text=(\S*)", text)
    check("2: alice is in PTA", any(same_game(g, pta.game) for g in joined(text)), str(joined(text)))
    check("2: Invite copies https://krunk.io/?game=<PTA game>",
          link.startswith("https://krunk.io/?game=") and same_game(link.split("game=", 1)[-1], pta.game), link)
    check("2: the clipboard holds the link", clip is not None and clip.group(1) == link, clip.group(1) if clip else text[-400:])
    before, copied, after = rects(text, "before", "inviteButton"), rects(text, "copied", "inviteButton"), rects(text, "after", "inviteButton")
    check("2: #inviteButton says \"Copied URL\" (wider), then \"Invite\" again after 1250 ms",
          before and copied and after and copied[2] > before[2] + 5 and abs(after[2] - before[2]) < 0.5, "%s %s %s" % (before, copied, after))
    f = loop.run(bob_sock.recv("eb", 15, is_presence(alice["id"], "PTA:")), 30)
    gid = (presence(f) or {}).get("gameInstanceId")
    check("2: bob's social socket: alice's presence:game names PTA (her client's session)", gid == pta.game, str(f))
    code = pta.game

    def bob_run(name, lines, game=None, until="@done", secs=90):
        p, log = client(name, brain_port, "\n".join(["0 menu"] + lines + ["%d state" % 60000, "61000 quit"]) + "\n",
                        game or ptb.game, acc_b)
        wait_for(lambda: until in read_log(log), secs)
        stop(p, 0)
        return read_log(log)

    # 3. Join from the clipboard
    text = bob_run("bob-clipboard", [
        "1000 call showWindow 24",
        "1400 clickid gameURL",
        "1500 type " + link,
        "1700 down CTRL+A", "1750 up CTRL+A",
        "1800 down CTRL+C", "1850 up CTRL+C",
        "1900 clip @pasted",
        "2000 call closWind",
        "2300 clickid menuBtnJoin",
        "9000 vis @done menuBtnJoin"])
    ids = joined(text)
    check("3: bob starts in PTB", ids[:1] and same_game(ids[0], ptb.game), str(ids))
    check("3: Join reads the clipboard", "[party] join from the clipboard: " + link in text, text[-500:])
    check("3: bob lands in alice's game (PTA)", len(ids) >= 2 and same_game(ids[-1], pta.game), str(ids))
    check("3: PTA admitted bob", wait_for(lambda: "authenticated as account %d " % bob["id"] in pta.text(), 5) is not None,
          pta.text()[-300:])

    # 4. the Join window with the bare code
    text = bob_run("bob-window", [
        "1000 clickid menuBtnJoin",  # nothing joinable on the clipboard: the Join window
        "1600 vis @win gameURL",
        "1800 clickid gameURL",
        "1900 type " + code,
        "2200 clicksel .joinBtn",
        "9000 vis @done menuBtnJoin"])
    ids = joined(text)
    check("4: Join with an empty clipboard opens the Join window", re.search(r"@win gameURL=1", text) is not None, text[-300:])
    check("4: the code typed into the Join window -> PTA", len(ids) >= 2 and same_game(ids[-1], pta.game), str(ids))

    # 5. a friend's Join (the Friends tab: openURL('/?game=<gid>', 1))
    text = bob_run("bob-friend", [
        "1000 call openURL \"/?game=%s\" 1" % gid,
        "9000 vis @done menuBtnJoin"])
    ids = joined(text)
    check("5: the Friends tab's Join (openURL of a same-site game link) -> alice's game, in place",
          len(ids) >= 2 and same_game(ids[-1], pta.game) and "[mm] join game " + gid in text, str(ids))
    stop(a, 0)

    # 6. a password-protected game
    text = bob_run("bob-password", [
        "1000 call openURL \"/?game=%s\" 1" % ptc.game,
        "5000 vis @asked genericPop codeInput",
        "5200 clickid codeInput",
        "5300 type " + PASSWORD,
        "5500 clickid confirmBtn",
        "12000 vis @done genericPop"])
    ids = joined(text)
    check("6: PTC refuses bob without its password, the popup asks for it",
          "This server needs a password" in ptc.text() and re.search(r"@asked genericPop=1 codeInput=1", text) is not None,
          text[-400:])
    check("6: Submit joins PTC again with the password", "joining %s again with the server password" % ptc.game in text
          and len(ids) >= 3 and same_game(ids[-1], ptc.game), str(ids))
    check("6: bob is in PTC, the popup gone", wait_for(lambda: "authenticated as account %d " % bob["id"] in ptc.text(), 5) is not None
          and re.search(r"@done genericPop=0", text) is not None, ptc.text()[-300:])


def main():
    if not os.access(f"{BUILD}/client_bin", os.X_OK) or not os.access(f"{BUILD}/services/brain/kn_brain", os.X_OK):
        print("v10_party_e2e: no client_bin / kn_brain in %s: skipped" % BUILD)
        return 77
    scratch = [sys.executable, "-I", os.path.join(ROOT, "services", "brain", "tests", "db_scratch.py"), ROOT]
    r = subprocess.run(scratch + ["create", DRIVER, NAME], capture_output=True)
    if r.returncode:
        print("v10_party_e2e: %s unavailable: skipped" % DRIVER)
        return r.returncode
    dbenv = {"KN_DATABASE_" + k.upper(): str(v) for k, v in json.loads(r.stdout).items()}
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=SECRET, **dbenv)
    brain_log = os.path.join(WORK, "brain.log")
    loop = Loop()
    try:
        subprocess.run([f"{BUILD}/services/brain/kn_brain", "--migrate-only"], cwd=ROOT, env=env, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        brain = subprocess.Popen([f"{BUILD}/services/brain/kn_brain", "--brain.host=127.0.0.1", "--brain.port=0",
                                  "--brain.region=local", "--brain.id=party-%d" % os.getpid()],
                                 cwd=ROOT, env=env, stdout=open(brain_log, "w"), stderr=subprocess.STDOUT,
                                 stdin=subprocess.DEVNULL, start_new_session=True)
        procs.append(brain)
        m = wait_for(lambda: re.search(r"listening on http://127\.0\.0\.1:(\d+) \(ws", read_log(brain_log)), 15)
        check("kn_brain starts", bool(m), read_log(brain_log)[-300:])
        if not m:
            return 1
        port = int(m.group(1))
        ports = kn_proto.free_ports(3)
        nodes = {"port": port, "PTA": Node(port, "PTA", ports[0]), "PTB": Node(port, "PTB", ports[1]),
                 "PTC": Node(port, "PTC", ports[2], PASSWORD)}
        check("three nodes linked with a lobby each", all(nodes[n].linked and nodes[n].game for n in ("PTA", "PTB", "PTC")),
              str([nodes[n].game for n in ("PTA", "PTB", "PTC")]))

        stamp = str(os.getpid() % 100000)
        users = {}
        for who in ("alice", "bob", "carol"):
            name = "%s%s" % (who, stamp)
            st, reg = http(port, "/auth/register", {"username": name, "password": "pw-party-%s" % stamp})
            users[who] = {"name": name, "token": reg.get("token", "") if isinstance(reg, dict) else "",
                          "id": (reg.get("account") or {}).get("id") if isinstance(reg, dict) else None}
        check("accounts registered", all(u["token"] for u in users.values()), str(users)[:300])

        async def follows():
            for who, others in (("alice", ("bob",)), ("bob", ("alice",)), ("carol", ("alice",))):
                s = await Social("ws://127.0.0.1:%d/social/ws" % port).open()
                r = await s.call("a", "a", 1, [users[who]["name"], None, users[who]["token"]])
                users[who]["id"] = users[who]["id"] or (r[2] if r else None)
                for o in others:
                    await s.send("f", users[o]["name"], 1)
                await s.call("fwr", "gfg", users[who]["name"], users[who]["id"], 1, "c")  # the follows ran (in order)
                await s.close()
        loop.run(follows())
        check("alice and bob follow each other, carol follows alice", all(u["id"] for u in users.values()), str(users)[:300])

        base = "ws://127.0.0.1:%d/social/ws" % port
        bob_sock = loop.run(presence_cases(base, nodes, users["alice"], users["bob"], users["carol"]), 120)
        client_cases(loop, port, nodes, users["alice"], users["bob"], bob_sock)
        loop.run(bob_sock.close())
    finally:
        loop.stop()
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
