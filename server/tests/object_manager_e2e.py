#!/usr/bin/env python3
"""KrunkNative Object Manager (F1 spawn menu, shared/include/object_manager.h) on a public server_bin.

  python3 -I server/tests/object_manager_e2e.py BUILD_DIR      (run from the repository root)

No database or coordinator: one standalone server_bin (a public game: not hosted, not the
sandbox) from a generated server.cfg, on ports from kn_proto.free_ports(). The host connects over
loopback (the server's own machine: a DEV peer); the guest writes a NET_PACKET_FORWARDED first (a
bridged remote browser), so it is neither DEV nor host.
  1. caps: the dev may do everything, the guest nothing (public game)
  2. the guest's extra weapon / pickup requests are refused; the dev's go through, and the other
     players hear LOADOUT_EXTRA / SPAWNED_OBJ + PICKUP
  3. join sync: a late v7 peer gets the dev's extras and the pickup; a v3 (1.0) peer never sees
     ids 250-254 and its own requests are ignored
  4. the rate limit answers a burst with "Slow down"
Prints PASS/FAIL lines; exit code 0 only when everything passed.
"""
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.getcwd(), "server/tests"))
import kn_proto  # noqa: E402

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
(PORT,) = kn_proto.free_ports(1)
WORK = tempfile.mkdtemp(prefix="kn-om-e2e-", dir=os.path.expanduser("~/.cache"))
SPAWN, FORWARDED, PICKUP = 4, 85, 128
SPAWN_REQ, SPAWN_RESULT, LOADOUT_EXTRA, SPAWNED_OBJ = 250, 251, 252, 253
QUERY, ADD_WEAPON, REMOVE_WEAPON, CLEAR_WEAPONS, DROP_WEAPON, SPAWN_ENTITY, SPAWN_NPC, SPAWN_OBJECT, REMOVE_AIM, UNDO = 0, 1, 2, 3, 4, 5, 6, 7, 8, 9
OBJECT_CRATE = 10  # object_manager_entries(OM_TAB_OBJECTS)[10]
AI_ADD_V7, SPAWNABLE_V7 = 162, 148
NPC_GHOST, ENTITY_KR_TAG = 1, 0  # object_manager_entries(): NPCs[1] Ghost (harmless), Entities[0] KR Tag
OK, DENIED, LIMIT, RATE = 0, 1, 2, 3
CAP_WEAPONS, CAP_SPAWN, CAP_DEV = 1, 2, 4
SHOTGUN, CROSSBOW = 5, 13
results = []
procs = []


def check(name, ok, detail=""):
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def query(port):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.settimeout(1.0)
        s.sendto(b"\xff\xff\xff\xffKNINFO\x00e2e", ("127.0.0.1", port))
        data, _ = s.recvfrom(2048)
    return data


def wait_for(fn, timeout=15.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            v = fn()
            if v:
                return v
        except Exception:  # noqa: BLE001 - polling
            pass
        time.sleep(0.2)
    return None


def start_server():
    with open(os.path.join(WORK, "none.json"), "w") as f:
        f.write("{}")  # no backend config: no brain, no database
    cfg = os.path.join(WORK, "s.cfg")
    with open(cfg, "w") as f:
        f.write(f'hostname "Object Manager e2e"\nport {PORT}\nregion local\nmaxplayers 8\nmapcycle Sandstorm\nmode ffa\nround_seconds 600\n')
    out = open(os.path.join(WORK, "s.log"), "w")
    p = subprocess.Popen([f"{BUILD}/server_bin", "--headless", "--cfg", cfg, "--config", os.path.join(WORK, "none.json")],
                         stdout=out, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
    procs.append(p)
    return p


class Peer(kn_proto.Conn):
    def __init__(self, port, remote_address=None, v7=True):
        super().__init__("127.0.0.1", port)
        if remote_address:
            self.send(FORWARDED, remote_address.encode() + b"\0")
        self.send(kn_proto.HELLO, bytes([1, 3, 1]) if v7 else bytes([1, 0, 0]))
        self.join()
        self.me = None

    def pump(self, seconds):
        self.read(timeout=seconds)

    def spawn(self):
        self.ent(class_index=0, lag_comp=1.0)
        end = time.time() + 5
        while time.time() < end and self.me is None:
            p = self.read(until=SPAWN, timeout=1)
            if p is not None and kn_proto.spawn_info(p)["is_you"]:
                self.me = kn_proto.spawn_info(p)
        return self.me

    def request(self, action, item=0, pitch=0.0):
        """One SPAWN_REQ; its SPAWN_RESULT as (status, caps, limit), None when none came."""
        mark = len(self.packets)
        self.send(SPAWN_REQ, struct.pack("<BBhff", action, 0, item, pitch, 0.0))
        end = time.time() + 3
        while time.time() < end:
            self.read(timeout=0.2)
            for t, p in self.packets[mark:]:
                if t == SPAWN_RESULT and len(p) == 8:
                    a, status, caps, _extra, it, limit = struct.unpack("<BBBBhH", p)
                    if a == action and it == item:
                        return status, caps, limit
        return None

    def of_type(self, ptype, since=0):
        return [p for t, p in self.packets[since:] if t == ptype]

    def sandbox_block(self):
        return sum(1 for t, _ in self.packets if 250 <= t <= 254)


def extras_for(peer, uid):
    found = None
    for p in peer.of_type(LOADOUT_EXTRA):
        if len(p) >= 5:
            u, n = struct.unpack("<iB", p[:5])
            if u == uid:
                found = list(p[5 : 5 + n])
    return found


def spawned_adds(peer):
    return spawned_adds_since(peer, 0)


def spawned_adds_since(peer, since):
    out = []
    for p in peer.of_type(SPAWNED_OBJ, since):
        op, kind, _r, uid, owner = struct.unpack("<BBHii", p[:12])
        if op == 1:
            out.append((uid, owner, p[12:].split(b"\0", 1)[0].decode()))
    return out


def run():
    start_server()
    check("server up", bool(wait_for(lambda: query(PORT))))
    host = Peer(PORT)
    check("host spawned", host.spawn() is not None)
    guest = Peer(PORT, "203.0.113.9")
    check("guest spawned", guest.spawn() is not None)
    if not host.me or not guest.me:
        return

    r = host.request(QUERY)
    check("dev caps on a public server: weapons + spawn + dev", r is not None and r[1] == CAP_WEAPONS | CAP_SPAWN | CAP_DEV, str(r))
    r = guest.request(QUERY)
    check("guest caps on a public server: none", r is not None and r[1] == 0, str(r))
    r = guest.request(ADD_WEAPON, SHOTGUN)
    check("guest extra weapon refused", r is not None and r[0] == DENIED, str(r))
    r = guest.request(DROP_WEAPON, CROSSBOW, -1.4)
    check("guest pickup refused", r is not None and r[0] == DENIED, str(r))

    mark = len(guest.packets)
    r = host.request(ADD_WEAPON, SHOTGUN)
    check("dev extra weapon", r is not None and r[0] == OK, str(r))
    guest.pump(0.5)
    check("the guest hears the dev's LOADOUT_EXTRA", extras_for(guest, host.me["uid"]) == [SHOTGUN], str(extras_for(guest, host.me["uid"])))
    r = host.request(DROP_WEAPON, CROSSBOW, -1.4)
    check("dev drops a Crossbow at the floor", r is not None and r[0] == OK, str(r))
    guest.pump(0.5)
    adds = spawned_adds(guest)
    check("the guest gets SPAWNED_OBJ (a WEAPON_PICKUP record)", len(adds) == 1 and '"i":26' in adds[0][2] and adds[0][1] == host.me["uid"], str(adds))
    check("and its PICKUP state", any(len(p) >= 8 and struct.unpack("<ii", p[:8]) == (adds[0][0], CROSSBOW) for p in guest.of_type(PICKUP, mark))
          if adds else False)

    # part 2: NPCs and entities, dev only on a public server
    r = guest.request(SPAWN_NPC, NPC_GHOST, -0.6)
    check("guest NPC refused", r is not None and r[0] == DENIED, str(r))
    r = guest.request(SPAWN_ENTITY, ENTITY_KR_TAG, -0.6)
    check("guest entity refused", r is not None and r[0] == DENIED, str(r))
    mark = len(guest.packets)
    r = host.request(SPAWN_NPC, NPC_GHOST, -0.6)
    check("dev spawns a Ghost", r is not None and r[0] == OK, str(r))
    r = host.request(SPAWN_ENTITY, ENTITY_KR_TAG, -0.4)
    check("dev spawns a KR Tag", r is not None and r[0] == OK, str(r))
    guest.pump(1.0)
    check("the guest sees the Ghost (AI_ADD_V7)", any(len(p) > 8 and p[8:].split(b"\0", 1)[0] == b"Ghost" for p in guest.of_type(AI_ADD_V7, mark)))
    check("the guest sees the spawnable (SPAWNABLE_V7)", bool(guest.of_type(SPAWNABLE_V7, mark)))

    # part 3: objects and the remove tool
    r = guest.request(SPAWN_OBJECT, OBJECT_CRATE, -0.6)
    check("guest object refused", r is not None and r[0] == DENIED, str(r))
    r = guest.request(REMOVE_AIM, 0, -0.6)
    check("guest remove tool refused", r is not None and r[0] == DENIED, str(r))
    for _ in range(2):  # the KR Tag and the Ghost go first: the remove ray takes the nearest spawned thing
        host.request(UNDO)
    mark = len(guest.packets)
    r = host.request(SPAWN_OBJECT, OBJECT_CRATE, -0.45)  # a few steps ahead, not around the player
    check("dev places a crate", r is not None and r[0] == OK, str(r))
    guest.pump(0.5)
    crate = [a for a in spawned_adds_since(guest, mark) if '"i":1,' in a[2] or a[2].endswith('"i":1}')]
    check("the guest gets the crate (SPAWNED_OBJ)", len(crate) == 1, str(spawned_adds_since(guest, mark)))
    mark = len(guest.packets)
    r = host.request(REMOVE_AIM, 0, -0.45)
    check("dev removes it at the crosshair", r is not None and r[0] == OK, str(r))
    guest.pump(0.5)
    removed = [struct.unpack("<BBHii", p[:12]) for p in guest.of_type(SPAWNED_OBJ, mark)]
    check("the guest hears the REMOVE", bool(crate) and any(op == 2 and uid == crate[0][0] for op, _k, _r, uid, _o in removed), str(removed))

    r = host.request(SPAWN_NPC, NPC_GHOST, -0.25)  # an AI up for the joiner
    check("dev spawns another Ghost", r is not None and r[0] == OK, str(r))

    late = Peer(PORT)
    check("late v7 joiner spawned", late.spawn() is not None)
    late.pump(1.0)
    check("the joiner gets the dev's extras", extras_for(late, host.me["uid"]) == [SHOTGUN], str(extras_for(late, host.me["uid"])))
    check("the joiner gets the spawned pickup", [a[0] for a in spawned_adds(late)] == [a[0] for a in adds], str(spawned_adds(late)))
    # Sandstorm has AIs of its own: look for the Ghost by name (NetAIAddV7Packet: int32 sid, int32 index, name)
    ghosts = [p for p in late.of_type(AI_ADD_V7) if len(p) > 8 and p[8:].split(b"\0", 1)[0] == b"Ghost"]
    check("the joiner gets the spawned Ghost", len(ghosts) == 1, str(len(ghosts)))

    old = Peer(PORT, v7=False)
    check("v3 peer spawned", old.spawn() is not None)
    host.request(ADD_WEAPON, 6)
    host.request(UNDO)
    old.pump(1.0)
    check("no id 250-254 reaches a v3 peer", old.sandbox_block() == 0, str(old.sandbox_block()))
    old.send(SPAWN_REQ, struct.pack("<BBhff", ADD_WEAPON, 0, SHOTGUN, 0.0, 0.0))
    old.pump(0.5)
    check("a v3 peer's request is ignored", not old.of_type(SPAWN_RESULT))

    time.sleep(3)  # the bucket refills
    mark = len(host.packets)
    for _ in range(30):
        host.send(SPAWN_REQ, struct.pack("<BBhff", ADD_WEAPON, 0, SHOTGUN, 0.0, 0.0))
    host.pump(1.0)
    statuses = [struct.unpack("<BBBBhH", p)[1] for p in host.of_type(SPAWN_RESULT, mark) if len(p) == 8]
    check("a burst of 30: every request answered, the rest of the burst slowed down", len(statuses) == 30 and statuses.count(RATE) >= 18,
          f"{len(statuses)} answers, {statuses.count(RATE)} RATE")
    for c in (host, guest, late, old):
        c.close()


try:
    run()
finally:
    for p in procs:
        p.terminate()
        try:
            p.wait(timeout=5)
        except subprocess.TimeoutExpired:
            p.kill()
ok = bool(results) and all(results)
if ok:
    shutil.rmtree(WORK, ignore_errors=True)
print(f"{sum(results)}/{len(results)} passed" + ("" if ok else f" (logs in {WORK})"))
sys.exit(0 if ok else 1)
