#!/usr/bin/env python3
"""Krunker Strike's friends end to end (client/src/ui/strike_social.c on kn_brain: services/brain/
social_eb.c friends + game:invite, game_presence.c gameFlavor, registry.c's flavor pools (strike-backend)).

  python3 -I server/tests/strike_social_e2e.py BUILD_DIR      (run from the repository root)

kn_brain on a scratch database (KN_TEST_DB_DRIVER, default sqlite) and two brain-linked server_bin
nodes: STK and STK2 (gameserver.flavor strike) and BAS (the base game); every port from
kn_proto.free_ports(). alice's client starts in STK2 (KRUNKNATIVE_GAME: a known lobby, so the invite and
Private Match checks know which game each one names).
alice and bob follow each other (friends), carol follows alice. bob is a protocol client plus a JSON
social socket; alice is a headless Strike client (--flavor strike) signed in, driven by an input script:

  1. MAIN_MENU (a signed-in account: FRVR's getFriends platform): the friends strip has bob with the
     online dot, the Private Match button is there
  2. bob plays in BAS (a Box Office game): the Friends List shows him under Playing Krunker as a plain
     row, not in a game; bob moves to STK: a large bullet with the lobby's mode, "N Player(s)" and Join;
     the arrow closes the list
  3. Private Match: bob's row with Invite; Invite -> bob gets game:invite with STK2, the button says Invited
  4. bob invites alice (game:invite with STK's game): INVITE_NOTIFICATION with bob's name; Join ->
     alice lands in STK; bob sees alice in STK with gameFlavor strike
  5. in STK (PLAY, then ESC: TAP_TO_PLAY, isInGame): bob in the same game -> no Join, no Invite; bob moves to
     BAS -> his row has Invite; a click sends bob game:invite with STK's game, the button says Invited
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
    print("strike_social_e2e: python websockets missing: skipped")
    sys.exit(77)

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
ROOT = os.getcwd()
DRIVER = os.environ.get("KN_TEST_DB_DRIVER", "sqlite")
NAME = "kn_test_strike_social_%d" % os.getpid()
SECRET = "kn-strike-social-%d" % os.getpid()
PASSWORD = "pw%d" % (os.getpid() % 10000)
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-strike-social-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
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
    code, body = http(port, "/mm/game-list?flavor=all")  # both flavor pools (strike-backend)
    ids = [g[0] for g in body.get("games", [])] if code == 200 else []
    return next((g for g in ids if g.startswith(node + ":")), None)


class Node:
    def __init__(self, brain_port, node_id, game_port, password="", flavor=""):
        self.node_id = node_id
        self.log = os.path.join(WORK, node_id + ".log")
        gs = os.path.join(WORK, node_id + ".json")
        cfg = {"node_id": node_id, "name": "strike social e2e " + node_id, "region": "local", "listen_port": game_port,
               "public_host": "127.0.0.1", "brain_url": "ws://127.0.0.1:%d/mm/internal/ws" % brain_port,
               "brain_secret": SECRET, "modes": ["ffa"], "max_players": 8, "shutdown_grace": 1, "idle_kick_seconds": 0}
        if password:
            cfg["password"] = password
        if flavor:
            cfg["flavor"] = flavor
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
    """A headless Krunker Strike client_bin (--flavor strike: the ui/strike page) in brain mode, signed
    in as account (name, token), with an input script. Started, not waited for."""
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
                # the script's clock and the UI's animations count the same frames: a slow frame (a screenshot,
                # a loaded host) cannot jump the clock past a panel's hide, so the next step's check sees it late
                "KRUNKNATIVE_FIXED_DELTA": "16.667",
                })
    if game:
        env["KRUNKNATIVE_GAME"] = game
    p = subprocess.Popen(["stdbuf", "-oL", "-eL", f"{BUILD}/client_bin", "--flavor", "strike"], cwd=ROOT, stdout=open(log, "w"),
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



def wait_log(log, label, secs=60.0):
    """the client's script reached `label` (its probe line is in the log)"""
    return wait_for(lambda: ("@%s " % label) in read_log(log), secs)


def nodes(text, label):
    m = re.search(r"\[input_script\] nodes t=\d+ @%s elements=\d+ match=(\d+)" % re.escape(label), text)
    return int(m.group(1)) if m else -1


def text_of(text, label):
    m = re.search(r"\[input_script\] text t=\d+ @%s n=(\d+) checked=\d value=.*? text=(.*)" % re.escape(label), text)
    return (int(m.group(1)), m.group(2).strip()) if m else (-1, "")


def proto_join(port, node, account):
    # the node's own flavor pool: a Strike lobby is sought as a Strike app (strike-backend)
    code, rep = http(port, "/mm/seek-game?hostname=krunknative&region=local&autoChangeGame=false&game=%s&token=%s%s"
                     % (node.game, account["token"], "&flavor=strike" if node.node_id.startswith("STK") else ""))
    if code != 200:
        return None
    conn = kn_proto.Conn("127.0.0.1", int(rep["port"]))
    conn.hello()
    conn.join(client_key=rep.get("clientId", ""), game_id=rep.get("gameId", ""), session=account["token"])
    conn.ent()
    return conn


SCRIPT = """0 menu
4000 nodes @strip .menu-friends .friends-pfp-img
4050 nodes @dot .menu-friends .menu-friends__online-dot
4100 nodes @pm .frvr-main-menu--bottom-right .frvr-button--orange
4200 clicksel .menu-friends
4800 nodes @panel .menu-friends-list
4850 nodes @large .menu-friends-list__section-bullet--large
4900 text @title .menu-friends-list__title span
4950 text @sec .menu-friends-list__section-title span
4960 nodes @row .menu-friends-list__section-bullet
12000 nodes @large2 .menu-friends-list__section-bullet--large
12050 text @mode .menu-friends-list__section-game-mode span
12100 text @count .menu-friends-list__section-player-count span
12150 nodes @joinbtn .menu-friends-list__section-extra--bot .frvr-button
12200 frames {shots}/friends-list 1
12300 clicksel .menu-friends-list__arrow
13000 nodes @closed .menu-friends-list
13050 nodes @strip2 .menu-friends
13100 clicksel .frvr-main-menu--bottom-right .frvr-button--orange
13800 nodes @cmrows .frvr-create-match-invite-friends__invite-bullet
13850 text @cmbtn .frvr-create-match-invite-friends__invite-bullet .frvr-button
13900 nodes @cmdot .frvr-create-match-invite-friends__friend-state--online
13950 clicksel .frvr-create-match-invite-friends__invite-bullet .frvr-button
14400 text @cmbtn2 .frvr-create-match-invite-friends__invite-bullet .frvr-button
14450 frames {shots}/private-match 1
14500 clicksel .frvr-create-match-screen .frvr-button-back
20000 nodes @note .game-invite-notification
20050 text @notename .game-invite-notification__text--blue
20100 frames {shots}/invite 1
20200 clickid join_game
25000 clickid mobMenuPlay
25100 spawn
30000 down ESC
30020 up ESC
31000 nodes @ttp .frvr-main-menu__bottom--tap-to-play
31050 clicksel .menu-friends
31800 nodes @samegame .menu-friends-list__section-bullet--large
31850 nodes @joinhidden .menu-friends-list__section-extra--bot .frvr-button
31900 nodes @invhidden .menu-friends-list__section-invite
40000 nodes @invbtn .menu-friends-list__section-invite
40050 clicksel .menu-friends-list__section-invite
40600 text @invbtn2 .menu-friends-list__section-invite
40700 frames {shots}/tap-to-play-invite 1
41000 quit
"""


def run_case(loop, port, nodes_, alice, bob):
    base = "ws://127.0.0.1:%d/social/ws" % port
    stk, bas = nodes_["STK"], nodes_["BAS"]

    async def bob_open():
        s = await Social(base).open()
        r = await s.call("a", "a", 1, [bob["name"], None, bob["token"]])
        return s, r
    b, r = loop.run(bob_open())
    check("bob signed in on his social socket", r and r[1] is None, str(r)[:200])
    conn = proto_join(port, bas, bob)
    check("bob plays in BAS (a Box Office game)", conn is not None)
    p, log = client("alice", port, SCRIPT.format(shots=WORK), nodes_["STK2"].game, (alice["name"], alice["token"]))

    ok = wait_log(log, "sec", 90)
    text = read_log(log)
    check("1: alice's MAIN_MENU friends strip: bob, with the online dot", nodes(text, "strip") == 1 and nodes(text, "dot") == 1,
          f"strip {nodes(text, 'strip')} dot {nodes(text, 'dot')}")
    check("1: the Private Match button (createMatchBtn)", nodes(text, "pm") == 1, str(nodes(text, "pm")))
    check("2: the Friends List opens", ok and nodes(text, "panel") == 1 and text_of(text, "title")[1] == "Friends List",
          f"{nodes(text, 'panel')} {text_of(text, 'title')}")
    check("2: bob in a Box Office game: Playing Krunker, a plain row (no Strike game)",
          text_of(text, "sec")[1] == "Playing Krunker" and nodes(text, "large") == 0 and nodes(text, "row") == 1,
          f"{text_of(text, 'sec')} large {nodes(text, 'large')} row {nodes(text, 'row')}")
    if conn:
        conn.close()
    conn = proto_join(port, stk, bob)

    ok = wait_log(log, "joinbtn", 60)
    text = read_log(log)
    check("2: bob moves to STK: a large bullet with the lobby's mode and players, Join",
          ok and nodes(text, "large2") == 1 and text_of(text, "mode")[1] != "" and re.match(r"\d+ Players?$", text_of(text, "count")[1] or "")
          and nodes(text, "joinbtn") == 1, f"{nodes(text, 'large2')} {text_of(text, 'mode')} {text_of(text, 'count')} {nodes(text, 'joinbtn')}")

    ok = wait_log(log, "cmbtn2", 30)
    text = read_log(log)
    check("2: the arrow closes the list (the strip again)", nodes(text, "closed") == 0 and nodes(text, "strip2") == 1,
          f"{nodes(text, 'closed')} {nodes(text, 'strip2')}")
    check("3: Private Match lists bob, online, with Invite", nodes(text, "cmrows") == 1 and nodes(text, "cmdot") == 1
          and text_of(text, "cmbtn")[1] == "Invite", f"{nodes(text, 'cmrows')} {nodes(text, 'cmdot')} {text_of(text, 'cmbtn')}")
    check("3: Invite -> Invited", text_of(text, "cmbtn2")[1] == "Invited", str(text_of(text, "cmbtn2")))

    async def bob_gets_invite():
        return await b.recv("eb", 15, lambda f: f[1].get("type") == "game:invite")
    inv = loop.run(bob_gets_invite(), 30)
    check("3: bob gets alice's Private Match invite with her game (STK2)", inv and inv[1].get("from") == alice["id"]
          and same_game(inv[1].get("gameId"), nodes_["STK2"].game), str(inv))

    async def invite_alice():
        await b.send("eb", {"type": "game:invite", "to": alice["id"], "gameId": stk.game})
        return await b.recv("eb", 10, lambda f: f[1].get("type") in ("game:invite:sent", "error"))
    f = loop.run(invite_alice())
    check("4: bob invites alice to STK (game:invite:sent)", f and f[1].get("type") == "game:invite:sent", str(f))
    ok = wait_log(log, "notename", 30)
    text = read_log(log)
    check("4: alice gets INVITE_NOTIFICATION with bob's name", nodes(text, "note") == 1 and text_of(text, "notename")[1] == bob["name"],
          f"{nodes(text, 'note')} {text_of(text, 'notename')}")

    async def bob_hears():
        return await b.recv("eb", 25, lambda f: f[1].get("type") == "presence:game" and f[1].get("id") == alice["id"]
                            and (f[1].get("gameInstanceId") or "").startswith("STK:"))
    pres = loop.run(bob_hears(), 60)
    text = read_log(log)
    check("4: Join -> alice joins STK", same_game(next(iter(joined(text)[-1:]), ""), stk.game), str(joined(text)))
    check("4: bob sees alice in STK with gameFlavor strike", pres and pres[1].get("gameFlavor") == "strike", str(pres))

    ok = wait_log(log, "invhidden", 40)
    text = read_log(log)
    check("5: ESC in the game: TAP_TO_PLAY's Friends List: bob in alice's game, no Join, no Invite",
          ok and nodes(text, "ttp") == 1 and nodes(text, "samegame") == 1 and nodes(text, "joinhidden") == 0 and nodes(text, "invhidden") == 0,
          f"ttp {nodes(text, 'ttp')} {nodes(text, 'samegame')} {nodes(text, 'joinhidden')} {nodes(text, 'invhidden')}")
    if conn:
        conn.close()
    conn = proto_join(port, bas, bob)

    async def bob_invited():
        return await b.recv("eb", 30, lambda f: f[1].get("type") == "game:invite")
    inv = loop.run(bob_invited(), 40)
    ok = wait_log(log, "invbtn2", 30)
    text = read_log(log)
    check("5: bob back in BAS: his row has Invite", nodes(text, "invbtn") == 1, str(nodes(text, "invbtn")))
    check("5: Invite sends bob game:invite with alice's STK game; the button says Invited",
          inv and same_game(inv[1].get("gameId"), stk.game) and text_of(text, "invbtn2")[1] == "Invited", f"{inv} {text_of(text, 'invbtn2')}")
    stop(p, 20)
    if conn:
        conn.close()
    loop.run(b.close())
    check("alice's client ran its script", "[input_script]" in read_log(log) and "Krunker Strike page ready" in read_log(log))


def main():
    if not os.access(f"{BUILD}/client_bin", os.X_OK) or not os.access(f"{BUILD}/services/brain/kn_brain", os.X_OK):
        print("strike_social_e2e: no client_bin / kn_brain in %s: skipped" % BUILD)
        return 77
    scratch = [sys.executable, "-I", os.path.join(ROOT, "services", "brain", "tests", "db_scratch.py"), ROOT]
    r = subprocess.run(scratch + ["create", DRIVER, NAME], capture_output=True)
    if r.returncode:
        print("strike_social_e2e: %s unavailable: skipped" % DRIVER)
        return r.returncode
    dbenv = {"KN_DATABASE_" + k.upper(): str(v) for k, v in json.loads(r.stdout).items()}
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=SECRET, **dbenv)
    brain_log = os.path.join(WORK, "brain.log")
    loop = Loop()
    try:
        subprocess.run([f"{BUILD}/services/brain/kn_brain", "--migrate-only"], cwd=ROOT, env=env, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        brain = subprocess.Popen([f"{BUILD}/services/brain/kn_brain", "--brain.host=127.0.0.1", "--brain.port=0",
                                  "--brain.region=local", "--brain.id=strike-social-%d" % os.getpid()],
                                 cwd=ROOT, env=env, stdout=open(brain_log, "w"), stderr=subprocess.STDOUT,
                                 stdin=subprocess.DEVNULL, start_new_session=True)
        procs.append(brain)
        m = wait_for(lambda: re.search(r"listening on http://127\.0\.0\.1:(\d+) \(ws", read_log(brain_log)), 15)
        check("kn_brain starts", bool(m), read_log(brain_log)[-300:])
        if not m:
            return 1
        port = int(m.group(1))
        ports = kn_proto.free_ports(3)
        nodes_ = {"STK": Node(port, "STK", ports[0], flavor="strike"), "STK2": Node(port, "STK2", ports[1], flavor="strike"),
                  "BAS": Node(port, "BAS", ports[2])}
        check("STK, STK2 (strike) and BAS (base) linked with a lobby each", all(nodes_[n].linked and nodes_[n].game for n in nodes_),
              str([nodes_[n].game for n in nodes_]))
        stamp = str(os.getpid() % 100000)
        users = {}
        for who in ("alice", "bob", "carol"):
            name = "%s%s" % (who, stamp)
            st, reg = http(port, "/auth/register", {"username": name, "password": "pw-strike-%s" % stamp})
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
                await s.call("fwr", "gfg", users[who]["name"], users[who]["id"], 1, "c")
                await s.close()
        loop.run(follows())
        check("alice and bob follow each other, carol follows alice", all(u["id"] for u in users.values()), str(users)[:300])
        run_case(loop, port, nodes_, users["alice"], users["bob"])
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
    if all(results) and not os.environ.get("KN_KEEP"):
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print("logs kept in " + WORK)
    print(f"{sum(results)}/{len(results)} passed")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
