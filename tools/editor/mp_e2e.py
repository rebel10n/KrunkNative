#!/usr/bin/env python3
"""Editor multiplayer co-editing against Krunker v10.0.7 (KE.multiplayer, editor-10.0.7.js:8982-10294;
doc/v10/editor-social.md 1.2 / 1.5): headless `client_bin --editor` runs against a scratch kn_brain
(sqlite; its room relay, services/brain/editor_rooms.c) with a python websocket peer in the room.

  python3 -I tools/editor/mp_e2e.py [--build /home/dev/.cache/kn-build] [-k NAME]

  test_windows_no_room     window 16 is v10's host form (warning, Max Players 2..20, password, close-on-
                           disconnect, Host Room / Join Room); window 17 says "Not in a room"; the playbar
                           has #mpToolbarBtn.
  test_controls_folder     Controls > Advanced ends with v10's "Multiplayer" folder: Toggle Player List TAB,
                           Focus Chat ENTER (initSettings mpPlayerListKey / mpChatKey).
  test_join_room           the peer hosts with a password and a map; the editor joins with mp:join (room
                           check, password): the room view (Room: id, Copy Invite Link / Player List /
                           Disconnect, no host slider), the peer's map synced with its sids, "Connected to
                           room" / "Map synced" in #mp-chat, the toolbar icon green, the peer's camera and
                           selection; the editor's new object reaches the peer (mpObjectAdded, confirmed
                           sid), its chat line with @obj, its camera (mpPlayerMove); window 17 lists both
                           with TP / SEL and no KICK / BAN; TAB opens window 17 (Toggle Player List).
  test_host_room           the editor hosts (Max Players 4, a password); the peer checks and joins: the
                           editor's map reaches it (mpRequestMap -> mpSyncMap), its chat line shows; window
                           16 has the host's Max Players slider, window 17 KICK / BAN; KICK removes the peer.
  test_streamer_mode       streamerMode: the room id is "***...***" in window 16.
  test_script_lock         the peer holds the script lock: the editor's scriptPopup is refused with
                           "<name> is currently editing scripts"; once unlocked it gets the lock and the
                           window opens; closing it sends the scripts and unlocks.

Exit status 0 = all passed, 77 = not built / python websockets missing.
"""
import argparse
import asyncio
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time

try:
    import websockets
except ImportError:  # pragma: no cover
    print("mp_e2e: python websockets missing: skipped")
    sys.exit(77)

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
TESTS = []


def test(fn):
    TESTS.append(fn)
    return fn


class Ctx:
    pass


def check(cond, msg):
    if not cond:
        raise AssertionError(msg)


def read(path):
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def j(v):
    return json.loads(v) if isinstance(v, str) else v


def run_editor(c, name, script, timeout=180):
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
    cfg = os.path.join(c.tmp, name + "-cfg")
    os.makedirs(cfg, exist_ok=True)
    shot = os.path.join(c.tmp, name + ".png")
    env.update({
        "KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": cfg, "KRUNKNATIVE_WINDOW_SIZE": "1280x720",
        "KRUNKNATIVE_EDITOR_SCRIPT": script + ";wait:0.5", "KRUNKNATIVE_SCREENSHOT": shot,
        "KRUNKNATIVE_BRAIN_URL": "http://127.0.0.1:%d" % c.port,
    })
    if c.egl:
        env["KRUNKNATIVE_EGL_DEVICE"] = c.egl
    p = subprocess.run([c.bin, "--editor", "new"], cwd=c.root, env=env, capture_output=True, text=True, timeout=timeout)
    log = p.stdout + p.stderr
    check(os.path.exists(shot), "%s: the run did not finish its script (rc %d):\n%s" % (name, p.returncode, log[-1500:]))
    return log


class Peer:
    """a python editor in the room: its own asyncio loop on a thread; frames it got are kept"""

    def __init__(self, port):
        self.port, self.got, self.loop = port, [], asyncio.new_event_loop()
        self.ready = threading.Event()
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()
        self.ready.wait(10)

    def _run(self):
        asyncio.set_event_loop(self.loop)
        self.loop.run_until_complete(self._open())
        self.ready.set()
        self.loop.run_forever()

    async def _open(self):
        self.ws = await websockets.connect("ws://127.0.0.1:%d/social/ws?enc=json" % self.port)
        self.reader = self.loop.create_task(self._read())

    async def _read(self):
        try:
            async for m in self.ws:
                f = json.loads(m)
                self.got.append(f)
                if self.on_frame:
                    await self.on_frame(self, f)
        except Exception:
            pass

    on_frame = None

    def send(self, frame):
        asyncio.run_coroutine_threadsafe(self.ws.send(json.dumps(frame)), self.loop).result(5)

    def wait(self, label, pred=lambda f: True, timeout=60):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            for f in list(self.got):
                if f[0] == label and pred(f):
                    return f
            time.sleep(0.05)
        raise AssertionError("peer: no %s (got %s)" % (label, [f[0] for f in self.got]))

    def frames(self, label):
        return [f for f in self.got if f[0] == label]

    def close(self):
        asyncio.run_coroutine_threadsafe(self.ws.close(), self.loop).result(5)
        self.loop.call_soon_threadsafe(self.loop.stop)


PEER_MAP = {"objects": [{"p": [10, 0, 0], "s": [4, 4, 4], "_sid": 1}, {"p": [20, 0, 0], "s": [2, 6, 2], "_sid": 2}],
            "config": {"name": "PeerMap"}, "scripts": {"client": "# peer client"}}


def peer_host(c, password="", map_doc=PEER_MAP):
    """the peer hosts a room and answers mpRequestMap with its map"""
    peer = Peer(c.port)

    async def on_frame(p, f):
        if f[0] == "mpRequestMap":
            await p.ws.send(json.dumps(["mpSyncMap", json.dumps(map_doc), f[1]]))

    peer.on_frame = on_frame
    peer.send(["mpHost", json.dumps({"maxPlayers": 5, "password": password, "closeOnHostDisconnect": True})])
    f = peer.wait("mpRoomJoined", timeout=10)
    peer.room, peer.id = f[1], f[4]
    # the room numbers its map: sids 1, 2 are the host's own
    peer.send(["mpSyncMap", json.dumps(map_doc)])
    peer.wait("mpSyncMapConfirmed", timeout=10)
    return peer


def lines(text, prefix):
    return [l[len(prefix):] for l in text.splitlines() if l.startswith(prefix)]


@test
def test_windows_no_room(c):
    w16, w17, tb = (os.path.join(c.tmp, "nr-%d.txt" % i) for i in range(3))
    run_editor(c, "noroom", "window:16;dump:window:%s;window:17;dump:window:%s;closewindow;dump:toolbar:%s" % (w16, w17, tb))
    t = read(w16)
    for want in ("Only invite people you trust - they have full control over the map", "Max Players", "Room Password (optional)",
                 "Close room when host disconnects", "Host Room", "Join Room"):
        check(want in t, "window 16 lacks %r:\n%s" % (want, t))
    check("Copy Invite Link" not in t, "window 16 outside a room shows the room view:\n%s" % t)
    check("Not in a room" in read(w17), "window 17 outside a room: %r" % read(w17))


@test
def test_controls_folder(c):
    out = os.path.join(c.tmp, "controls.txt")
    run_editor(c, "controls", "ctab:1;window:1;dump:window:%s" % out)
    t = read(out)
    check("Multiplayer" in t, "Controls > Advanced has no Multiplayer folder:\n%s" % t)
    after = t[t.rindex("Multiplayer"):]
    check("Toggle Player List" in after and "TAB" in after, "no Toggle Player List TAB:\n%s" % after)
    check("Focus Chat" in after and "ENTER" in after, "no Focus Chat ENTER:\n%s" % after)
    check(t.rindex("Multiplayer") > t.rindex("Paste preset JSON"), "the Multiplayer folder must follow File:\n%s" % t)


@test
def test_join_room(c):
    peer = peer_host(c, password="sesame")
    try:
        d1, w16, w17, w17b = (os.path.join(c.tmp, "join-%s.txt" % i) for i in ("d1", "w16", "w17", "w17b"))
        script = ("mp:join:%s:sesame;wait:3;" % peer.room +
                  "mp:dump:%s;window:16;dump:window:%s;window:17;dump:window:%s;closewindow;" % (d1, w16, w17) +
                  "add:0;wait:1;mp:chat:look at @obj;wait:1.5;key:tab;dump:window:%s;wait:0.5" % w17b)

        def peer_moves():  # once the editor is in, the peer moves and selects
            f = peer.wait("mpPlayerJoined", timeout=60)
            peer.editor_id = j(f[1])["id"]
            time.sleep(0.3)
            peer.send(["mpPlayerMove", [7, 8, 9], [0, 1, 0]])
            peer.send(["mpPlayerSelect", 2])
        t = threading.Thread(target=peer_moves, daemon=True)
        t.start()
        log = run_editor(c, "join", script)
        t.join(5)
        d = read(d1)
        room = lines(d, "ROOM ")
        check(room and room[0].startswith(peer.room), "the editor is not in the peer's room:\n%s\n%s" % (d, log[-800:]))
        players = lines(d, "PLAYER ")
        check(len(players) == 2 and any("role=1" in p and "local" not in p for p in players) and any("local" in p for p in players),
              "players: the peer host and the editor: %r" % players)
        remote = [p for p in players if "local" not in p][0]
        check("pos=7.00,8.00,9.00" in remote and "sel=2" in remote, "the peer's camera and selection: %r" % remote)
        objs = lines(d, "OBJ ")
        check(len(objs) == 2 and objs[0].startswith("sid=1 ") and objs[1].startswith("sid=2 ") and '"p":[20,0,0]' in objs[1],
              "the peer's map with its sids: %r" % objs)
        check('SCRIPT client # peer client' in d, "the peer's scripts synced:\n%s" % d)
        chat = lines(d, "CHAT ")
        check("Connected to room" in chat and "Map synced" in chat, "#mp-chat: %r" % chat)
        check("CHATVISIBLE 1" in d and "ICON #0f0" in d, "the chat shows and the toolbar icon is green:\n%s" % d)
        t16 = read(w16)
        check("Room:" in t16 and peer.room in t16 and "Copy Invite Link" in t16 and "Player List" in t16 and "Disconnect" in t16,
              "window 16 in a room:\n%s" % t16)
        check("Max Players" not in t16, "a joined player has no Max Players slider:\n%s" % t16)
        t17 = read(w17)
        check("Players [2/5]" in t17 and "[HOST]" in t17 and "(you)" in t17 and "TP" in t17 and "SEL" in t17, "window 17:\n%s" % t17)
        check("KICK" not in t17 and "BAN" not in t17, "KICK / BAN for a non-host:\n%s" % t17)
        check("Players [2/5]" in read(w17b), "TAB (Toggle Player List) did not open window 17:\n%s" % read(w17b))
        # the editor's object, chat and camera reached the peer
        f = peer.wait("mpObjectAdded", timeout=5)
        check(f[1] == 3 and f[3] == peer.editor_id and isinstance(j(f[2]), dict), "the editor's new object: %r" % f)
        f = peer.wait("mpChat", lambda f: f[1] == peer.editor_id, timeout=5)
        check(re.fullmatch(r"look at [A-Za-z_ ]+::3", f[2]) is not None, "@obj in the chat line: %r" % f[2])
        check(f[3] == "Guest", "a guest editor is named Guest: %r" % f[3])
        check(peer.frames("mpPlayerMove"), "the editor's camera (mpPlayerMove) never came")
        # v10: the new object is selected before the room numbers it, so the selection goes as -1
        check(any(x[1] == peer.editor_id and x[2] == -1 for x in peer.frames("mpPlayerSelect")), "the editor's selection: %r" % peer.frames("mpPlayerSelect"))
    finally:
        peer.close()


@test
def test_host_room(c):
    d1, d2, w16, w17, d3 = (os.path.join(c.tmp, "host-%s.txt" % i) for i in ("d1", "d2", "w16", "w17", "d3"))
    peer = Peer(c.port)
    state = {}

    def peer_joins():
        end = time.monotonic() + 90
        while time.monotonic() < end and not lines(read(d1), "ROOM "):
            time.sleep(0.1)
        room = lines(read(d1), "ROOM ")[0].split()[0]
        state["room"] = room
        peer.send(["mpCheckRoom", room])
        state["info"] = peer.wait("mpRoomInfo", timeout=10)
        peer.send(["mpJoin", room, "secret"])
        state["joined"] = peer.wait("mpRoomJoined", timeout=10)
        state["map"] = peer.wait("mpSyncMap", timeout=10)
        peer.send(["mpChat", "hi host"])
    t = threading.Thread(target=peer_joins, daemon=True)
    t.start()
    try:
        script = ("add:0;add:5;mp:host:4:secret;wait:2;mp:dump:%s;wait:4;mp:dump:%s;" % (d1, d2) +
                  "window:16;dump:window:%s;window:17;dump:window:%s;" % (w16, w17))
        # KICK: the peer (player 2) through the window's button handler
        script += "ui:KE.multiplayer.kick(2);wait:1;mp:dump:%s" % d3
        log = run_editor(c, "host", script)
        t.join(5)
        check("room" in state, "the editor never hosted:\n%s\n%s" % (read(d1), log[-800:]))
        check(j(state["info"][2]) == {"hasPassword": True, "players": 1, "maxPlayers": 4}, "mpRoomInfo: %r" % state["info"])
        m = j(state["map"][1])
        check(len(m["objects"]) == 2 and all("_sid" in o for o in m["objects"]), "the editor's map for the joiner: %r" % m)
        d = read(d2)
        check(len(lines(d, "PLAYER ")) == 2, "the peer is in the editor's player list:\n%s" % d)
        chat = lines(d, "CHAT ")
        check("Guest joined" in chat and "Guest: hi host" in chat, "#mp-chat: %r" % chat)
        t16, t17 = read(w16), read(w17)
        check("Max Players" in t16 and "Copy Invite Link" in t16, "the host's window 16:\n%s" % t16)
        check("Players [2/4]" in t17 and "KICK" in t17 and "BAN" in t17, "the host's window 17:\n%s" % t17)
        check(peer.wait("mpKicked", timeout=5)[1] == "Kicked by host", "the peer was not kicked")
        check(len(lines(read(d3), "PLAYER ")) == 1 and "Guest left" in lines(read(d3), "CHAT "), "after KICK:\n%s" % read(d3))
    finally:
        peer.close()


@test
def test_streamer_mode(c):
    peer = peer_host(c)
    try:
        w16 = os.path.join(c.tmp, "streamer.txt")
        run_editor(c, "streamer", "mp:setting:streamerMode:true;mp:join:%s;wait:3;window:16;dump:window:%s" % (peer.room, w16))
        t = read(w16)
        check("***...***" in t and peer.room not in t, "streamer mode shows the room id:\n%s" % t)
    finally:
        peer.close()


@test
def test_script_lock(c):
    peer = peer_host(c)
    try:
        peer.send(["mpScriptLock"])
        peer.wait("mpScriptLockAcquired", timeout=5)
        d1, d2, d3 = (os.path.join(c.tmp, "lock-%s.txt" % i) for i in ("d1", "d2", "d3"))

        def unlock():
            f = peer.wait("mpPlayerJoined", timeout=60)
            time.sleep(2.5)
            peer.send(["mpScriptUpdate", json.dumps({"client": "# from peer"})])
            peer.send(["mpScriptUnlock"])
        t = threading.Thread(target=unlock, daemon=True)
        t.start()
        script = ("mp:join:%s;wait:1.5;mp:script;wait:0.3;mp:dump:%s;wait:2.5;mp:script;wait:0.5;dump:script:%s;mp:dump:%s;mp:scriptclose;wait:1" % (peer.room, d1, d2, d3))
        run_editor(c, "lock", script)
        t.join(5)
        d = read(d1)
        chat = lines(d, "CHAT ")
        check(any(l.endswith("is editing scripts") for l in chat), "mpScriptLocked for the joiner: %r" % chat)
        check(any(l.endswith("is currently editing scripts") for l in chat), "the refused scriptPopup: %r" % chat)
        check(read(d2).startswith("H KRUNKSCRIPT"), "the script window did not open once the lock was free:\n%s" % read(d2)[:600])
        check("SCRIPT client # from peer" in read(d3) and "Scripts unlocked" in lines(read(d3), "CHAT "), "the peer's mpScriptUpdate / unlock:\n%s" % read(d3))
        check(any(l.endswith("is editing scripts") for l in lines(d, "CHAT ")), "mpScriptLocked shown")
        f = peer.wait("mpScriptLocked", timeout=5)
        check(f[2] == "Guest", "the editor's lock reached the peer: %r" % f)
        f = peer.wait("mpScriptUpdate", timeout=5)
        check(j(f[1]).get("client") == "# from peer", "the editor's scripts on close: %r" % f[1][:200])
        peer.wait("mpScriptUnlocked", timeout=5)
    finally:
        peer.close()


def wait_port(log, proc):
    for _ in range(300):
        if proc.poll() is not None:
            raise RuntimeError("kn_brain exited early (%s)" % read(log)[-800:])
        m = re.search(r"listening on http://127\.0\.0\.1:(\d+) \(ws", read(log))
        if m:
            return int(m.group(1))
        time.sleep(0.05)
    raise RuntimeError("kn_brain did not start listening")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default=os.path.join(ROOT, "cmake-build-debug"))
    ap.add_argument("-k", default="", help="run only the tests whose name contains this")
    args = ap.parse_args()
    c = Ctx()
    c.root = ROOT
    c.bin = os.path.join(os.path.abspath(args.build), "client_bin")
    brain_bin = os.path.join(os.path.abspath(args.build), "services", "brain", "kn_brain")
    c.egl = os.environ.get("KRUNKNATIVE_EGL_DEVICE", "")
    if not os.path.exists(c.bin) or not os.path.exists(brain_bin):
        print("mp_e2e: client_bin or kn_brain not built: skipped")
        return 77
    os.makedirs(os.path.expanduser("~/.cache"), exist_ok=True)
    c.tmp = tempfile.mkdtemp(prefix="mp-e2e-", dir=os.path.expanduser("~/.cache"))
    cfg = {"database": {"driver": "sqlite", "path": os.path.join(c.tmp, "brain.db"),
                        "migrations_dir": os.path.join(ROOT, "services", "db", "migrations")},
           "brain": {"host": "127.0.0.1", "port": 0, "internal_secret": "mpe2e-%d" % os.getpid(), "federation": False},
           "accounts": {"password_iterations": 10000, "starting_kr": 0, "skins_file": os.path.join(ROOT, "assets", "skins.json")}}
    with open(os.path.join(c.tmp, "brain.json"), "w") as f:
        json.dump(cfg, f)
    blog = os.path.join(c.tmp, "brain.log")
    brain = subprocess.Popen([brain_bin, "--config", os.path.join(c.tmp, "brain.json"), "--migrate"], cwd=ROOT,
                             stdout=open(blog, "w"), stderr=subprocess.STDOUT)
    failed = 0
    keep = False
    try:
        c.port = wait_port(blog, brain)
        for fn in TESTS:
            if args.k and args.k not in fn.__name__:
                continue
            try:
                fn(c)
                print("ok   %s" % fn.__name__, flush=True)
            except Exception as e:  # report every test
                failed += 1
                keep = True
                print("FAIL %s: %s" % (fn.__name__, e), flush=True)
    finally:
        brain.terminate()
        try:
            brain.wait(timeout=10)
        except subprocess.TimeoutExpired:
            brain.kill()
        if keep:
            print("mp_e2e: kept %s" % c.tmp)
        else:
            shutil.rmtree(c.tmp, ignore_errors=True)
    total = len([f for f in TESTS if not args.k or args.k in f.__name__])
    print("%d/%d passed" % (total - failed, total))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
