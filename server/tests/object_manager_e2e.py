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
  4. `give` / `drop` (chat commands): refused to the guest, the dev's go through; a dropped weapon
     is a pickup the guest hears; the Tool Gun's tools (Info, Rotator, Duplicator, Deleter) for the
     dev, refused to the guest
  5. edits (OM_ACTION_EDIT): the dev's Physics Gun moves a crate, the guest hears OM_SPAWNED_UPDATE and
     may not edit it; Undo puts it back
  6. map edits: the dev's Deleter takes a map object (the guest hears OM_SPAWNED_MAP_DELETE, a late
     joiner gets it replayed), Undo brings it back (MAP_RESTORE), the Rotator and a Physics Gun edit
     change one in place (MAP_EDIT with the new pose); the guest may not touch the map
  6b. the Properties tool: the dev's field patch (OM_EDIT_FIELDS) changes a map object's colour and
     its fields come back in the MAP_EDIT overlay; the guest's is refused, an out-of-range value and
     a field of another prefab too; Undo restores the map
  7. the rate limit answers a burst with "Slow down"
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
DROP_HELD, TOOL, EDIT = 10, 11, 12
PHYS_GUN, EDIT_PHYSGUN, EDIT_BEGIN = 66, 0, 1  # WEAPON_ID_PHYS_GUN (WEAPON_ID_KN_BASE 64 + 2)
EDIT_BTOOLS, EDIT_FIELDS, FIELDS_ANSWER = 1, 4, 1  # the Properties tool's field patch (OM_EDIT_FIELDS)
TOOL_DELETER, TOOL_DUPLICATOR, TOOL_ROTATOR, TOOL_INFO = 0, 1, 2, 3
TOOL_COPIED, TOOL_PASTED, TOOL_SHOWN = 1, 2, 3
NO_TOOL = 9
TOOL_GUN = 64  # WEAPON_ID_TOOL_GUN (WEAPON_ID_KN_BASE)
CHAT_SEND, INPUT, NOTIFY = 8, 5, 82
OBJECT_CRATE = 10  # object_manager_entries(OM_TAB_OBJECTS)[10]
AI_ADD_V7, SPAWNABLE_V7 = 162, 148
NPC_GHOST, ENTITY_KR_TAG = 1, 0  # object_manager_entries(): NPCs[1] Ghost (harmless), Entities[0] KR Tag
OK, DENIED, LIMIT, RATE = 0, 1, 2, 3
CAP_WEAPONS, CAP_SPAWN, CAP_DEV, CAP_MAP = 1, 2, 4, 8
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

    def say(self, text):
        """A chat line (a `/command`); the packet mark before it (for heard())."""
        time.sleep(0.45)  # the server's chat burst limit (3 / s)
        mark = len(self.packets)
        self.send(CHAT_SEND, text.encode() + b"\0")
        self.pump(0.6)
        return mark

    def heard(self, mark, text):
        return any(text.encode() in p for _, p in self.packets[mark:])

    def hold(self, slot, shoot=False):
        """Input frames picking loadout slot `slot` (the scroll byte: slot + 2), past the swap timer."""
        self.seq = getattr(self, "seq", 0)
        for i in range(16):
            self.seq += 1
            scroll = slot + 2 if i == 0 else 0
            self.send(INPUT, struct.pack("<iifffBBBb", self.seq, -1, 1.0 / 60.0, 0.0, 0.0, 1 if shoot and i == 15 else 0, 0, 0, scroll))
        self.pump(0.3)

    def edit(self, uid, pos, size, quarter=0, flags=EDIT_BEGIN):
        """One OM_ACTION_EDIT (the Physics Gun's); its SPAWN_RESULT as (status, caps, limit)."""
        mark = len(self.packets)
        self.send(SPAWN_REQ, struct.pack("<BBhff", EDIT, 0, EDIT_PHYSGUN, 0.0, 0.0) + struct.pack("<i3f3fBBH", uid, *pos, *size, quarter, flags, 0))
        end = time.time() + 3
        while time.time() < end:
            self.read(timeout=0.2)
            for t, p in self.packets[mark:]:
                if t == SPAWN_RESULT and len(p) == 8:
                    a, status, caps, _extra, _it, limit = struct.unpack("<BBBBhH", p)
                    if a == EDIT:
                        return status, caps, limit
        return None

    def fields(self, uid, patch):
        """One field patch (the Properties tool: OM_ACTION_EDIT + OM_EDIT_FIELDS + JSON); its SPAWN_RESULT."""
        time.sleep(0.08)  # the edit pace (OM_EDIT_INTERVAL)
        mark = len(self.packets)
        head = struct.pack("<BBhff", EDIT, 0, EDIT_BTOOLS, 0.0, 0.0) + struct.pack("<i3f3fBBH", uid, 0, 0, 0, 0, 0, 0, 0, EDIT_FIELDS, 0)
        self.send(SPAWN_REQ, head + patch.encode() + b"\0")
        end = time.time() + 3
        while time.time() < end:
            self.read(timeout=0.2)
            for t, p in self.packets[mark:]:
                if t == SPAWN_RESULT and len(p) == 8:
                    a, status, caps, _extra, _it, limit = struct.unpack("<BBBBhH", p)
                    if a == EDIT:
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


def map_ops_since(peer, since, op):
    """(uid, pose JSON) of each SPAWNED_OBJ map op (4 MAP_EDIT, 5 MAP_DELETE, 6 MAP_RESTORE) since the mark"""
    out = []
    for p in peer.of_type(SPAWNED_OBJ, since):
        o, _kind, _r, u, _owner = struct.unpack("<BBHii", p[:12])
        if o == op:
            out.append((u, p[12:].split(b"\0", 1)[0].decode()))
    return out


def updates_since(peer, since, uid):
    out = []
    for p in peer.of_type(SPAWNED_OBJ, since):
        op, _kind, _r, u, _owner = struct.unpack("<BBHii", p[:12])
        if op == 3 and u == uid:
            out.append(p[12:].split(b"\0", 1)[0].decode())
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
    check("dev caps on a public server: weapons + spawn + dev + map", r is not None and r[1] == CAP_WEAPONS | CAP_SPAWN | CAP_DEV | CAP_MAP, str(r))
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


    # part 4: give / drop (chat commands) and the Tool Gun on a public server
    time.sleep(3)  # the request bucket refills
    host.request(CLEAR_WEAPONS)
    mark = guest.say("/give crossbow")
    check("guest: give refused with a reason", guest.heard(mark, "extra weapons are off"))
    mark = guest.say('/give "tool gun"')
    check("guest: no Tool Gun", guest.heard(mark, "only developers and the host can spawn"))
    mark = guest.say("/drop")
    check("guest: class weapons stay", guest.heard(mark, "can't drop your class weapons"))
    r = guest.request(TOOL, TOOL_DELETER, -0.45)
    check("guest: no tools", r is not None and r[0] == DENIED, str(r))
    mark = host.say('/give "tool gun"')
    check("dev: give the Tool Gun", host.heard(mark, "Tool Gun added to your loadout"))
    guest.pump(0.3)
    check("the guest hears the dev's LOADOUT_EXTRA with the Tool Gun", extras_for(guest, host.me["uid"]) == [TOOL_GUN], str(extras_for(guest, host.me["uid"])))
    slot = None
    for s in range(1, 6):  # the class loadout's size: the first extra slot is the Tool Gun
        host.hold(s)
        r = host.request(TOOL, TOOL_INFO, -1.5)
        if r is not None and r[0] != NO_TOOL:
            slot = s
            break
    check("dev: the Tool Gun in hand, Info answers", slot is not None and r[0] == OK and r[2] == TOOL_SHOWN, str(r))
    r = host.request(SPAWN_OBJECT, OBJECT_CRATE, -0.45)
    check("dev places a crate", r is not None and r[0] == OK, str(r))
    mark = len(host.packets)
    r = host.request(TOOL, TOOL_INFO, -0.45)
    notes = [p for p in host.of_type(NOTIFY, mark)]
    check("Info on the crate: a NOTIFY with its type and owner", r is not None and r[0] == OK and any(b"Crate" in p and b"owner" in p for p in notes),
          str([kn_proto.uncstr(p) for p in notes]))
    gmark = len(guest.packets)
    r = host.request(TOOL, TOOL_ROTATOR, -0.45)
    check("Rotator", r is not None and r[0] == OK, str(r))
    guest.pump(0.5)
    ops = [struct.unpack("<BBHii", p[:12])[0] for p in guest.of_type(SPAWNED_OBJ, gmark)]
    check("the guest sees the turn in place (OM_SPAWNED_UPDATE)", 3 in ops, str(ops))
    r = host.request(TOOL, TOOL_DUPLICATOR, -0.45)
    check("Duplicator copies", r is not None and r[0] == OK and r[2] == TOOL_COPIED, str(r))
    r = host.request(TOOL, TOOL_DUPLICATOR, -0.3)
    check("and places the copy", r is not None and r[0] == OK and r[2] == TOOL_PASTED, str(r))
    r = guest.request(TOOL, TOOL_DELETER, -0.45)
    check("guest Deleter refused", r is not None and r[0] == DENIED, str(r))
    r = host.request(TOOL, TOOL_DELETER, -0.45)
    check("Deleter removes the crate", r is not None and r[0] == OK, str(r))
    host.request(UNDO)  # the copy
    # drop: a given weapon in hand goes down in front, a pickup the guest hears
    mark = host.say("/give crossbow")
    check("dev: give crossbow", host.heard(mark, "Crossbow added"))
    if slot is not None:
        host.hold(slot + 1)
    gmark = len(guest.packets)
    mark = host.say("/drop")
    check("dev: drop", host.heard(mark, "Dropped the Crossbow"), str(slot))
    guest.pump(0.5)
    drops = [a for a in spawned_adds_since(guest, gmark) if '"i":26' in a[2] and '"w":13' in a[2]]
    check("the guest gets the dropped Crossbow pickup", len(drops) == 1, str(spawned_adds_since(guest, gmark)))
    check("the dev's extras: the Tool Gun only", extras_for(guest, host.me["uid"]) == [TOOL_GUN], str(extras_for(guest, host.me["uid"])))
    host.request(UNDO)
    host.request(CLEAR_WEAPONS)

    # part 5: edits (the Physics Gun) on a public server
    time.sleep(3)
    mark = host.say('/give "physics gun"')
    check("dev: give the Physics Gun", host.heard(mark, "Physics Gun added to your loadout"))
    gmark = len(guest.packets)
    r = host.request(SPAWN_OBJECT, OBJECT_CRATE, -0.45)
    guest.pump(0.5)
    crates = [a for a in spawned_adds_since(guest, gmark) if '"i":1,' in a[2] or a[2].endswith('"i":1}')]
    check("dev places a crate to carry", r is not None and r[0] == OK and len(crates) == 1, str(r))
    if crates:
        uid, _owner, rec = crates[0]
        import json as _json
        d = _json.loads(rec)
        pos, size = d["p"], d.get("s", [12, 12, 12])
        quarter = round(d.get("r", [0, 0, 0])[1] / 1.5707963) % 4
        r = None
        for s in range(1, 6):  # the Physics Gun's slot: an extra after the class loadout
            host.hold(s)
            r = host.edit(uid, [pos[0], pos[1] + 6.0, pos[2]], size, quarter)
            if r is not None and r[0] != NO_TOOL:
                break
            time.sleep(0.1)
        check("dev: grab and lift it", r is not None and r[0] == OK, str(r))
        guest.pump(0.5)
        ups = updates_since(guest, gmark, uid)
        check("the guest hears OM_SPAWNED_UPDATE with the new spot", bool(ups) and abs(_json.loads(ups[-1])["p"][1] - (pos[1] + 6.0)) < 0.11, str(ups[-1:]))
        r = guest.edit(uid, [pos[0], pos[1] + 12.0, pos[2]], size, quarter)
        check("the guest may not edit it", r is not None and r[0] == DENIED, str(r))
        gmark = len(guest.packets)
        r = host.request(UNDO)
        guest.pump(0.5)
        ups = updates_since(guest, gmark, uid)
        check("Undo puts it back", r is not None and r[0] == OK and bool(ups) and abs(_json.loads(ups[-1])["p"][1] - pos[1]) < 0.11, str(ups[-1:]))
        host.request(UNDO)
    host.request(CLEAR_WEAPONS)

    # part 6: the map's own objects (the floor under the dev, looking straight down)
    time.sleep(3)
    import json as _json
    MAP_EDIT, MAP_DELETE, MAP_RESTORE = 4, 5, 6
    mark = host.say('/give "tool gun"')
    slot = None
    for s in range(1, 6):
        host.hold(s)
        r = host.request(TOOL, TOOL_INFO, -1.5)
        if r is not None and r[0] != NO_TOOL:
            slot = s
            break
    check("dev: the Tool Gun in hand again", slot is not None, str(r))
    for _ in range(6):  # every spawn of the dev's goes (the Ghost from part 3 would stand nearer than the floor)
        r = host.request(UNDO)
        if r is None or r[0] != OK:
            break
    r = guest.request(TOOL, TOOL_DELETER, -1.5)
    check("guest: the map's floor is not the guest's", r is not None and r[0] == DENIED, str(r))
    gmark = len(guest.packets)
    r = host.request(TOOL, TOOL_DELETER, -1.5)
    guest.pump(0.5)
    deleted = map_ops_since(guest, gmark, MAP_DELETE)
    check("dev: the Deleter takes the map object underfoot, the guest hears MAP_DELETE", r is not None and r[0] == OK and len(deleted) == 1,
          f"{r} {deleted}")
    if deleted:
        uid = deleted[0][0]
        late2 = Peer(PORT)
        check("a second late joiner spawned", late2.spawn() is not None)
        late2.pump(1.0)
        check("the joiner gets the deleted map object replayed", [u for u, _ in map_ops_since(late2, 0, MAP_DELETE)] == [uid],
              str(map_ops_since(late2, 0, MAP_DELETE)))
        gmark = len(guest.packets)
        r = host.request(UNDO)
        guest.pump(0.5)
        check("Undo: the map object is back (MAP_RESTORE)", r is not None and r[0] == OK and [u for u, _ in map_ops_since(guest, gmark, MAP_RESTORE)] == [uid],
              str(map_ops_since(guest, gmark, MAP_RESTORE)))
        gmark = len(guest.packets)
        r = host.request(TOOL, TOOL_ROTATOR, -1.5)
        guest.pump(0.5)
        edits = [e for e in map_ops_since(guest, gmark, MAP_EDIT) if e[0] == uid]
        check("Rotator on the map object: MAP_EDIT in place", r is not None and r[0] == OK and len(edits) == 1, f"{r} {edits}")
        if edits:
            pose = _json.loads(edits[0][1])
            host.request(UNDO)
            mark = host.say('/give "physics gun"')
            phys = None
            for s in range(1, 7):
                host.hold(s)
                r = host.edit(uid, [pose["p"][0], pose["p"][1] - 0.5, pose["p"][2]], [pose["s"][2], pose["s"][1], pose["s"][0]])
                if r is not None and r[0] != NO_TOOL:
                    phys = s
                    break
                time.sleep(0.1)
            guest.pump(0.5)
            moved = [_json.loads(e[1]) for e in map_ops_since(guest, gmark, MAP_EDIT) if e[0] == uid]
            check("Physics Gun edit of the map object: down 0.5, the guest hears it", phys is not None and r[0] == OK and bool(moved) and
                  abs(moved[-1]["p"][1] - (pose["p"][1] - 0.5)) < 0.02, f"{r} {moved[-1:]}")
            r = guest.edit(uid, pose["p"], pose["s"])
            check("guest: no edits of the map", r is not None and r[0] == DENIED, str(r))
            gmark = len(guest.packets)
            host.request(UNDO)
            guest.pump(0.5)
            check("Undo: the map's own object again", [u for u, _ in map_ops_since(guest, gmark, MAP_RESTORE)] == [uid],
                  str(map_ops_since(guest, gmark, MAP_RESTORE)))
        late2.close()
        # part 6b: the Properties tool (OM_EDIT_FIELDS): the map object's fields, devs and the host only
        time.sleep(3)  # the request bucket refills
        print("the Properties tool...")
        host.hold(slot)  # the Properties tool is the Tool Gun's: it must be in hand
        r = host.request(TOOL, TOOL_INFO, -1.5)
        check("dev: the Tool Gun in hand again", r is not None and r[0] != NO_TOOL, str(r))
        gmark = len(guest.packets)
        r = host.fields(uid, '{"c":"#ff0000"}')
        check("dev: the map object's colour set", r is not None and r[0] == OK and r[2] == FIELDS_ANSWER, str(r))
        guest.pump(0.4)
        edits = [e for e in map_ops_since(guest, gmark, MAP_EDIT) if e[0] == uid]
        check("the guest hears the MAP_EDIT overlay with the colour", bool(edits) and _json.loads(edits[-1][1]).get("c") == "#ff0000", str(edits[-1:]))
        r = guest.fields(uid, '{"c":"#00ff00"}')
        check("guest: no field edits of the map", r is not None and r[0] == DENIED, str(r))
        r = host.fields(uid, '{"c":"red"}')
        check("a colour that is not #rrggbb: refused", r is not None and r[0] != OK, str(r))
        r = host.fields(uid, '{"li":3}')
        check("a light's Intensity on this object: refused", r is not None and r[0] != OK, str(r))
        r = host.fields(uid, '{"o":9}')
        check("an opacity past 0..1: refused", r is not None and r[0] != OK, str(r))
        gmark = len(guest.packets)
        r = host.request(UNDO)
        guest.pump(0.4)
        check("Undo: the map's own object again (MAP_RESTORE)", r is not None and r[0] == OK and [u for u, _ in map_ops_since(guest, gmark, MAP_RESTORE)] == [uid],
              str(map_ops_since(guest, gmark, MAP_RESTORE)))
    host.request(CLEAR_WEAPONS)

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
