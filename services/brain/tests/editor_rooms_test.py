#!/usr/bin/env python3
"""Krunker v10.0.7's multiplayer map editing rooms on kn_brain (services/brain/editor_rooms.c): the
mp* frames of KE.multiplayer (editor-10.0.7.js:9696-10294) between three editor sockets through the
social socket's JSON framing: host, room check, join (password, full, unknown room), the host's
map for the joiner, object add / confirm / move / update / remove / group remove, selection,
camera, chat, room settings, script lock / deny / update / unlock, kick, ban (a rejoin refused),
the host leaving (closeOnHostDisconnect: the room closes; else the next player hosts).

  editor_rooms_test.py <kn_brain> <repo root>
77 = skip (python websockets missing).
"""
import asyncio
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

try:
    import websockets
except ImportError:  # pragma: no cover
    print("python websockets missing: skip")
    sys.exit(77)

BRAIN, ROOT = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
FAILS = []
PASSES = [0]


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what, flush=True)
    if cond:
        PASSES[0] += 1
    else:
        FAILS.append(what)


def wait_port(log, proc):
    for _ in range(300):
        if proc.poll() is not None:
            raise RuntimeError("kn_brain exited early (%s)" % log)
        m = re.search(r"listening on http://127\.0\.0\.1:(\d+) \(ws", open(log, errors="replace").read())
        if m:
            return int(m.group(1))
        time.sleep(0.05)
    raise RuntimeError("kn_brain did not start listening")


def j(v):
    return json.loads(v) if isinstance(v, str) else v


class Sock:
    def __init__(self, port, tag):
        self.port, self.tag, self.inbox = port, tag, []

    async def open(self, register=None):
        self.ws = await websockets.connect("ws://127.0.0.1:%d/social/ws?enc=json" % self.port)
        if register:
            await self.send(["a", 0, [register, "mppass123", None], None])
            a = await self.expect("a")
            check(isinstance(a[2], int) and a[2] > 0, "%s: account %s registered" % (self.tag, register))
        return self

    async def close(self):
        await self.ws.close()

    async def send(self, frame):
        await self.ws.send(json.dumps(frame))

    async def expect(self, label, timeout=5):
        """the next frame with this label (frames of other labels are kept)"""
        for i, f in enumerate(self.inbox):
            if f[0] == label:
                return self.inbox.pop(i)
        end = time.monotonic() + timeout
        while True:
            left = end - time.monotonic()
            if left <= 0:
                raise TimeoutError("%s: no %s (inbox %s)" % (self.tag, label, [f[0] for f in self.inbox]))
            f = json.loads(await asyncio.wait_for(self.ws.recv(), left))
            if f[0] == label:
                return f
            self.inbox.append(f)

    async def none(self, label, wait=0.4):
        """no frame with this label arrives"""
        try:
            await self.expect(label, wait)
            return False
        except (TimeoutError, asyncio.TimeoutError):
            return True


async def run(port):
    stamp = int(time.time()) % 100000
    host, b, c = Sock(port, "host"), Sock(port, "b"), Sock(port, "c")
    await host.open("mph%d" % stamp)
    await b.open("mpb%d" % stamp)
    await c.open("mpc%d" % stamp)

    # host: mpHost(settings) -> mpRoomJoined(room, players, settings, yourId)
    await host.send(["mpHost", json.dumps({"maxPlayers": 2, "password": "pw1", "closeOnHostDisconnect": False})])
    f = await host.expect("mpRoomJoined")
    room, players, settings, me = f[1], j(f[2]), j(f[3]), f[4]
    check(isinstance(room, str) and room.startswith("room_") and len(room) > 5, "mpRoomJoined: room id %r" % room)
    check(len(players) == 1 and players[0]["id"] == me and players[0]["role"] == 1, "the host is player %r, role 1" % me)
    check(players[0]["name"] == "mph%d" % stamp and players[0]["color"].startswith("#"), "the account's name, a colour (%r)" % players[0])
    check(settings == {"maxPlayers": 2, "hasPassword": True, "closeOnHostDisconnect": False}, "settings %r" % settings)

    # mpCheckRoom
    await b.send(["mpCheckRoom", room])
    f = await b.expect("mpRoomInfo")
    check(f[1] == room and j(f[2]) == {"hasPassword": True, "players": 1, "maxPlayers": 2}, "mpRoomInfo %r" % f[2:])
    await b.send(["mpCheckRoom", "room_nope"])
    f = await b.expect("mpRoomInfo")
    check(j(f[2]).get("error") == "Room not found", "mpRoomInfo of an unknown room: error")

    # joins refused
    await b.send(["mpJoin", "room_nope", ""])
    check((await b.expect("mpError"))[1] == "Room not found", "mpJoin unknown room: Room not found")
    await b.send(["mpJoin", room, "bad"])
    check((await b.expect("mpError"))[1] == "Incorrect password", "mpJoin wrong password: Incorrect password")

    # the host has a map: an object before anyone joins gets sid 1
    await host.send(["mpObjectAdded", "tmp_1", json.dumps({"p": [0, 0, 0], "s": [1, 1, 1]})])
    f = await host.expect("mpObjectConfirmed")
    check(f[1] == "tmp_1" and f[2] == 1, "mpObjectConfirmed(tmp_1, 1)")

    # b joins
    await b.send(["mpJoin", room, "pw1"])
    f = await b.expect("mpRoomJoined")
    bid = f[4]
    check(f[1] == room and len(j(f[2])) == 2 and bid != me, "b joined as player %r of 2" % bid)
    f = await host.expect("mpPlayerJoined")
    check(j(f[1])["id"] == bid and j(f[1])["name"] == "mpb%d" % stamp and j(f[1])["role"] == 0, "the host: mpPlayerJoined(b)")
    f = await host.expect("mpRequestMap")
    check(f[1] == bid, "the host: mpRequestMap(b)")
    # the host answers with its map; the room numbers the object without a sid
    await host.send(["mpSyncMap", json.dumps({"objects": [{"p": [0, 0, 0], "_sid": 1}, {"p": [5, 0, 0]}], "config": {"name": "x"}}), bid])
    f = await host.expect("mpSyncMapConfirmed")
    check(j(f[1]) == [1, 2], "mpSyncMapConfirmed [1, 2]")
    f = await b.expect("mpSyncMap")
    m = j(f[1])
    check([o.get("_sid") for o in m["objects"]] == [1, 2] and m["config"] == {"name": "x"}, "b: the host's map with sids 1, 2")

    # full
    await c.send(["mpJoin", room, "pw1"])
    check((await c.expect("mpError"))[1] == "Room is full", "a third player in a 2-player room: Room is full")
    # settings: host only, clamped, to everyone
    await b.send(["mpRoomSettings", json.dumps({"maxPlayers": 9})])
    check(await host.none("mpRoomSettings"), "mpRoomSettings from a non-host ignored")
    await host.send(["mpRoomSettings", json.dumps({"maxPlayers": 99})])
    f = await b.expect("mpRoomSettings")
    check(j(f[1])["maxPlayers"] == 20, "mpRoomSettings: maxPlayers 99 -> 20 for everyone")
    await host.expect("mpRoomSettings")
    await c.send(["mpJoin", room, "pw1"])
    f = await c.expect("mpRoomJoined")
    cid = f[4]
    check(len(j(f[2])) == 3, "c joined once the room is bigger (3 players)")
    await host.expect("mpPlayerJoined")
    await b.expect("mpPlayerJoined")
    await host.expect("mpRequestMap")

    # objects
    await b.send(["mpObjectAdded", "tmp_1", json.dumps({"p": [1, 2, 3]})])
    f = await b.expect("mpObjectConfirmed")
    check(f[1] == "tmp_1" and f[2] == 3, "b's object: sid 3 (after the synced 1, 2)")
    f = await host.expect("mpObjectAdded")
    check(f[1] == 3 and j(f[2]) == {"p": [1, 2, 3]} and f[3] == bid, "the host: mpObjectAdded(3, obj, b)")
    await c.expect("mpObjectAdded")
    check(await b.none("mpObjectAdded"), "the sender gets no mpObjectAdded")
    moved = json.dumps([{"sid": 3, "p": [4, 5, 6], "r": [0, 0, 0], "s": [1, 1, 1]}])
    await host.send(["mpObjectsMoved", moved])
    check((await b.expect("mpObjectsMoved"))[1] == moved, "mpObjectsMoved relayed as sent")
    await c.expect("mpObjectsMoved")
    await c.send(["mpObjectUpdated", 3, json.dumps({"c": 255})])
    f = await host.expect("mpObjectUpdated")
    check(f[1] == 3 and j(f[2]) == {"c": 255}, "mpObjectUpdated(3, changes) relayed")
    await b.expect("mpObjectUpdated")
    await b.send(["mpGroupRemoved", 1, 2])
    f = await host.expect("mpGroupRemoved")
    check(f[1:] == [1, 2], "mpGroupRemoved(1, 2) relayed")
    await c.expect("mpGroupRemoved")
    await host.send(["mpObjectRemoved", 3])
    check((await c.expect("mpObjectRemoved"))[1] == 3, "mpObjectRemoved(3) relayed")
    await b.expect("mpObjectRemoved")

    # camera, selection, chat
    await b.send(["mpPlayerMove", [1, 2, 3], [0.1, 0.2, 0]])
    f = await host.expect("mpPlayerMove")
    check(f[1:] == [bid, [1, 2, 3], [0.1, 0.2, 0]], "mpPlayerMove(b, pos, rot) to the others")
    await c.expect("mpPlayerMove")
    await b.send(["mpPlayerMove", "x", None])
    check(await host.none("mpPlayerMove"), "a malformed mpPlayerMove is dropped")
    await c.send(["mpPlayerSelect", 2])
    check((await host.expect("mpPlayerSelect"))[1:] == [cid, 2], "mpPlayerSelect(c, 2)")
    await b.expect("mpPlayerSelect")
    await b.send(["mpChat", "  hello @obj  "])
    f = await c.expect("mpChat")
    check(f[1] == bid and f[2] == "hello @obj" and f[3] == "mpb%d" % stamp and f[4].startswith("#"), "mpChat(id, trimmed text, name, colour) %r" % f[1:])
    f = await b.expect("mpChat")
    check(f[2] == "hello @obj", "the sender gets its chat line too")
    await host.expect("mpChat")
    await b.send(["mpChat", "y" * 300])
    check(len((await c.expect("mpChat"))[2]) == 200, "a chat line is cut at 200 characters")
    await b.expect("mpChat")
    await host.expect("mpChat")

    # scripts: lock, denied, update, unlock
    await b.send(["mpScriptLock"])
    await b.expect("mpScriptLockAcquired")
    f = await host.expect("mpScriptLocked")
    check(f[1:] == [bid, "mpb%d" % stamp], "mpScriptLocked(b, name) to the others")
    await c.expect("mpScriptLocked")
    await c.send(["mpScriptLock"])
    f = await c.expect("mpScriptLockDenied")
    check(f[1] == "mpb%d" % stamp, "mpScriptLockDenied(holder) for c")
    await c.send(["mpScriptUpdate", json.dumps({"client": "c"})])
    check(await host.none("mpScriptUpdate"), "a script update from a non-holder is dropped")
    await b.send(["mpScriptUpdate", json.dumps({"client": "# new"})])
    f = await host.expect("mpScriptUpdate")
    check(j(f[1]) == {"client": "# new"}, "the holder's mpScriptUpdate relayed")
    await c.expect("mpScriptUpdate")
    await b.send(["mpScriptUnlock"])
    check((await host.expect("mpScriptUnlocked"))[1] == bid, "mpScriptUnlocked(b)")
    await c.expect("mpScriptUnlocked")

    # clear: host only
    await b.send(["mpClearMap"])
    check(await host.none("mpClearMap"), "mpClearMap from a non-host ignored")
    await host.send(["mpClearMap"])
    await b.expect("mpClearMap")
    await c.expect("mpClearMap")
    check(True, "the host's mpClearMap reaches the others")

    # kick (c), ban (b): host only, never oneself
    await b.send(["mpKick", cid])
    check(await c.none("mpKicked"), "mpKick from a non-host ignored")
    await host.send(["mpKick", me])
    check(await host.none("mpKicked"), "the host cannot kick itself")
    await host.send(["mpKick", cid])
    check((await c.expect("mpKicked"))[1] == "Kicked by host", "c: mpKicked(Kicked by host)")
    check((await b.expect("mpPlayerLeft"))[1] == cid, "b: mpPlayerLeft(c)")
    await host.expect("mpPlayerLeft")
    await host.send(["mpBan", bid])
    check((await b.expect("mpBanned"))[1] == "Banned by host", "b: mpBanned(Banned by host)")
    check((await host.expect("mpPlayerLeft"))[1] == bid, "the host: mpPlayerLeft(b)")
    await b.send(["mpJoin", room, "pw1"])
    check((await b.expect("mpError"))[1] == "You are banned from this room", "b cannot rejoin: banned")
    await c.send(["mpJoin", room, "pw1"])
    f = await c.expect("mpRoomJoined")
    check(f[4] != cid, "a kicked player may rejoin (a new player number)")
    cid = f[4]
    await host.expect("mpPlayerJoined")
    await host.expect("mpRequestMap")

    # the host leaves a room without closeOnHostDisconnect: c hosts
    await host.send(["mpScriptLock"])
    await host.expect("mpScriptLockAcquired")
    await c.expect("mpScriptLocked")
    await host.close()
    check((await c.expect("mpScriptUnlocked"))[1] == me, "the host's socket closes: its script lock is released")
    check((await c.expect("mpPlayerLeft"))[1] == me, "c: mpPlayerLeft(host)")
    check((await c.expect("mpHostChanged"))[1] == cid, "c: mpHostChanged(c)")
    await c.send(["mpKick", 999])
    check(await c.none("mpError"), "the new host's commands are accepted")

    # a room with closeOnHostDisconnect: the others are told and it is gone
    h2 = await Sock(port, "h2").open()
    await h2.send(["mpHost", json.dumps({"maxPlayers": 5, "password": "", "closeOnHostDisconnect": True})])
    f = await h2.expect("mpRoomJoined")
    room2 = f[1]
    check(j(f[2])[0]["name"] == "Guest", "a guest host is named Guest")
    await c.send(["mpCheckRoom", room2])
    check(j((await c.expect("mpRoomInfo"))[2]) == {"hasPassword": False, "players": 1, "maxPlayers": 5}, "room 2: no password")
    await c.send(["mpJoin", room2, ""])
    await c.expect("mpRoomJoined")
    check(True, "c moved to room 2 (leaving room 1)")
    await h2.expect("mpRequestMap")
    await h2.send(["mpLeave"])
    check((await c.expect("mpKicked"))[1] == "The host closed the room", "the host leaves: mpKicked(The host closed the room)")
    await c.send(["mpCheckRoom", room2])
    check(j((await c.expect("mpRoomInfo"))[2]).get("error") == "Room not found", "room 2 is gone")
    await c.send(["mpCheckRoom", room])
    check(j((await c.expect("mpRoomInfo"))[2]).get("error") == "Room not found", "room 1 is gone once empty")
    for s in (b, c, h2):
        await s.close()


def main():
    work = tempfile.mkdtemp(prefix="kn-editor-rooms-test-", dir=os.path.expanduser("~/.cache"))
    db = os.path.join(work, "brain.db")
    cfg = {"database": {"driver": "sqlite", "path": db, "migrations_dir": os.path.join(ROOT, "services", "db", "migrations")},
           "brain": {"host": "127.0.0.1", "port": 0, "internal_secret": "mp-%d" % os.getpid(), "federation": False},
           "accounts": {"password_iterations": 10000, "starting_kr": 0, "skins_file": os.path.join(ROOT, "assets", "skins.json")}}
    with open(os.path.join(work, "config.json"), "w") as f:
        json.dump(cfg, f)
    log = os.path.join(work, "brain.log")
    brain = subprocess.Popen([BRAIN, "--config", os.path.join(work, "config.json"), "--migrate"], cwd=ROOT,
                             stdout=open(log, "w"), stderr=subprocess.STDOUT)
    try:
        asyncio.run(run(wait_port(log, brain)))
    except Exception as e:  # a timeout names the missing frame
        FAILS.append("%s: %s" % (type(e).__name__, e))
    finally:
        brain.terminate()
        try:
            brain.wait(timeout=10)
        except subprocess.TimeoutExpired:
            brain.kill()
    if FAILS:
        print("editor_rooms_test: FAILED (%d): %s (log %s)" % (len(FAILS), "; ".join(FAILS), log))
        sys.exit(1)
    shutil.rmtree(work, ignore_errors=True)
    print("editor_rooms_test: ok (%d checks)" % PASSES[0])


if __name__ == "__main__":
    main()
