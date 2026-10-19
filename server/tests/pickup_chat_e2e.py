#!/usr/bin/env python3
"""Pickup feedback on a public server_bin: Junk pickups (Reward Junk, doc/v10/junk.md) and the weapon pickup sound.

  python3 -I server/tests/pickup_chat_e2e.py BUILD_DIR      (run from the repository root)

No database or coordinator: one standalone server_bin (a public game on Laboratory, whose six map
Junk pickups spawn at 70 %) from a generated server.cfg with `perms 1` and the default cheats
"auth", as the prod game servers' command gates are, on ports from kn_proto.free_ports(). The host
connects over loopback (a DEV peer, so the Object Manager places things for it); the guests write
a NET_PACKET_FORWARDED first (remote players: no DEV rights, no account).
  1. the host places the Object Manager's Junk (a TRIGGER, onInteractKey, Reward Junk "0.01") at its
     feet and presses G ("int" 7): the Junk is gone (a second press: nothing). The "You found [0.01]
     Junk" line (CHAT kind NET_CHAT_FOUND_JUNK) follows a credit kn_brain accepted, and this
     server has no brain and no account: no line for the host, none for the guest
  2. the host drops a Crossbow and takes it back ("int" 6): "pick_0" at 0.2 as a 2D sound
     (uid -2) to the host only (v7 io_playerInteractions)
  3. the non-dev guest: the Object Manager refuses it the Junk (prod caps), but a map Junk it is
     brought to (rcon bring) and an Object Manager Junk the console gives it (rcon give junk) are
     its own: the Junk's sound (asset 29736, 2D), the Junk destroyed for everyone, no line (no
     account), nothing on a second press
  4. a protocol 1.3.0 guest presses G the v3 way ("int" action 1, the server's own prompt scan) on
     another map Junk: destroyed, no line (kind 3 would be the credit's line; see above)
The airdrop Junk Packages (a held claim) and the Reward Junk gates with a scripted account are covered by
shared/tests/zombies_test.c and shared/tests/triggers_test.c; the brain's credit by services/brain/tests/junk_test.py.
Prints PASS/FAIL lines; exit code 0 only when everything passed.
"""
import os
import shutil
import socket
import struct
import subprocess
import json
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.getcwd(), "server/tests"))
import kn_proto  # noqa: E402

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
PORT, RCON_PORT = kn_proto.free_ports(2)
RCON_PASSWORD = "pickup-chat-e2e-rcon"
MAP_FILE = "assets/maps/official/29_laboratory.json"  # mapcycle Laboratory: six "pickup Junk" at rsc 70
WORK = tempfile.mkdtemp(prefix="kn-pickup-chat-e2e-", dir=os.path.expanduser("~/.cache"))
SPAWN, CHAT, INTERACT, FORWARDED, OBJECT_STATE, SOUND, PICKUP = 4, 9, 61, 85, 100, 102, 128
OBJECT_DESTROY, OBJECT_RESPAWN = 0, 1
NO_OBJECT_PREFABS = (40, 41, 55, 95, 98)  # PREFAB_R_NO_OBJECT: EVENT, TERMINAL, AI_SPAWNER, BOT_SPAWNER, ENTITY
SPAWN_REQ, SPAWN_RESULT, SPAWNED_OBJ = 250, 251, 253
DROP_WEAPON, SPAWN_ENTITY = 4, 5
OK, DENIED = 0, 1  # OM_STATUS_OK, OM_STATUS_DENIED
ENTITY_JUNK = 17  # object_manager_entries(OM_TAB_ENTITIES): after KR Tag .. Airdrop (shared/src/object_manager.c k_entities)
CROSSBOW = 13
INTERACT_PICKUP, INTERACT_KEY = 6, 7
CHAT_FOUND_JUNK = 3  # NET_CHAT_FOUND_JUNK (printed only after a brain credit)
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
        f.write(f'hostname "Pickup chat e2e"\nport {PORT}\nregion local\nmaxplayers 8\nmapcycle Laboratory\nmode ffa\nround_seconds 600\n'
                f'perms 1\nrcon_password "{RCON_PASSWORD}"\nrcon_port {RCON_PORT}\n')
    out = open(os.path.join(WORK, "s.log"), "w")
    p = subprocess.Popen([f"{BUILD}/server_bin", "--headless", "--cfg", cfg, "--config", os.path.join(WORK, "none.json")],
                         stdout=out, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
    procs.append(p)
    return p


def rcon(*cmd):
    out = subprocess.run([f"{BUILD}/server_bin", "--rcon", f"127.0.0.1:{RCON_PORT}", "--rcon-password", RCON_PASSWORD, *cmd],
                         capture_output=True, text=True, timeout=10)
    return out.stdout + out.stderr


def three(v):
    return isinstance(v, list) and len(v) >= 3


def map_junk():
    """{object uid: position} of the map's "pickup Junk" triggers (Reward Junk, action 59). The uid
    counts the map objects the server builds, not the JSON array index: map_sanitize_json
    (shared/src/map_catalog.c) keeps a "p" with an "s" or a pooled "si" size, and map_raw_object_kept
    (shared/src/map.c) skips the prefabs that build no object (Laboratory has a few before its Junk)."""
    with open(MAP_FILE) as f:
        data = json.load(f)
    xyz = data.get("xyz") if isinstance(data.get("xyz"), list) else []

    def kept(o):
        if not isinstance(o, dict) or not three(o.get("p")):
            return False
        prefab = o.get("i") or o.get("id") or 0
        if isinstance(prefab, (int, float)) and int(prefab) in NO_OBJECT_PREFABS:
            return False
        si = o.get("si")
        pooled = isinstance(si, (int, float)) and not isinstance(si, bool) and 0 <= int(si) * 3 and int(si) * 3 + 2 < len(xyz) and \
            all(isinstance(n, (int, float)) and not isinstance(n, bool) for n in xyz[int(si) * 3 : int(si) * 3 + 3])
        return three(o.get("s")) or pooled

    objects = [o for o in data["objects"] if kept(o)]
    return {i: o["p"] for i, o in enumerate(objects) if any(isinstance(a, dict) and str(a.get("ta")) == "59" for a in o.get("tas") or [])}


def object_states(peer, since=0):
    """(uid, kind) of each OBJECT_STATE (NetObjectStatePacket: int32 uid, u8 kind, int32 value)"""
    return [struct.unpack("<iB", p[:5]) for p in peer.of_type(OBJECT_STATE, since) if len(p) >= 9]


def alive_junk(peer, junk):
    """The map Junk this round spawned (rsc): the joiner's init "ro" minus any "do" since"""
    alive = set()
    for uid, kind in object_states(peer):
        if uid in junk and kind == OBJECT_RESPAWN:
            alive.add(uid)
        elif uid in junk and kind == OBJECT_DESTROY:
            alive.discard(uid)
    return sorted(alive)


def junk_lines(peer, since=0):
    return [c for c in chat_lines(peer, since) if "Junk" in c[2]]


def chat_lines(peer, since=0):
    """(uid, kind, text) of each CHAT (NetChatPacket: int32 uid, u8 kind, name[32], text[288])"""
    out = []
    for p in peer.of_type(CHAT, since):
        if len(p) >= 37 + 288:
            uid, kind = struct.unpack("<iB", p[:5])
            out.append((uid, kind, kn_proto.uncstr(p[37 : 37 + 288])))
    return out


def sounds(peer, since=0):
    """(name, uid, volume) of each SOUND (NetSoundPacket: name[24], int32 uid, float pos[3], volume, rate)"""
    out = []
    for p in peer.of_type(SOUND, since):
        if len(p) >= 44:
            uid, _x, _y, _z, volume, _rate = struct.unpack("<i5f", p[24:48])
            out.append((kn_proto.uncstr(p[:24]), uid, round(volume, 3)))
    return out


class Peer(kn_proto.Conn):
    def __init__(self, port, remote_address=None, name="", version=(1, 3, 1)):
        super().__init__("127.0.0.1", port)
        if remote_address:
            self.send(FORWARDED, remote_address.encode() + b"\0")
        self.send(kn_proto.HELLO, bytes(version))
        self.join(name=name)
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
        """One SPAWN_REQ; its SPAWN_RESULT status, None when none came."""
        mark = len(self.packets)
        self.send(SPAWN_REQ, struct.pack("<BBhff", action, 0, item, pitch, 0.0))
        end = time.time() + 3
        while time.time() < end:
            self.read(timeout=0.2)
            for t, p in self.packets[mark:]:
                if t == SPAWN_RESULT and len(p) == 8:
                    a, status, _caps, _extra, it, _limit = struct.unpack("<BBBBhH", p)
                    if a == action and it == item:
                        return status
        return None

    def interact(self, itype, uid):
        """v7 "int" type, uid (NetInteractV7Packet: u8 action 1, u8 secondary, int32 type, int32 uid)"""
        time.sleep(0.15)  # config.interactTimer
        mark = len(self.packets)
        self.send(INTERACT, struct.pack("<BBii", 1, 0, itype, uid))
        self.pump(0.6)
        return mark

    def interact_v3(self):
        """v3 "int" 1 (NetInteractPacket: u8 action, u8 secondary): the server scans for the target"""
        time.sleep(0.15)
        mark = len(self.packets)
        self.send(INTERACT, struct.pack("<BB", 1, 0))
        self.pump(0.6)
        return mark

    def of_type(self, ptype, since=0):
        return [p for t, p in self.packets[since:] if t == ptype]


def spawned_adds(peer, since=0):
    out = []
    for p in peer.of_type(SPAWNED_OBJ, since):
        op, _kind, _r, uid, owner = struct.unpack("<BBHii", p[:12])
        if op == 1:
            out.append((uid, owner, p[12:].split(b"\0", 1)[0].decode()))
    return out


def run():
    start_server()
    check("server up", bool(wait_for(lambda: query(PORT))))
    junk = map_junk()
    check("Laboratory has map Junk", len(junk) >= 2, str(junk))
    host = Peer(PORT)
    check("host spawned", host.spawn() is not None)
    # rsc 70 per Junk: a round with fewer than two (about 1 %) is played again
    for _ in range(5):
        if len(alive_junk(host, junk)) >= 2:
            break
        host.close()
        rcon("map", "Laboratory")
        time.sleep(1.0)
        host = Peer(PORT)
        host.spawn()
    alive = alive_junk(host, junk)
    check("this round spawned two map Junk or more", len(alive) >= 2, str(alive))
    guest = Peer(PORT, "203.0.113.9")
    check("guest spawned", guest.spawn() is not None)
    old = Peer(PORT, "203.0.113.10", version=(1, 3, 0))
    check("1.3.0 guest spawned", old.spawn() is not None)
    if not host.me or not guest.me or not old.me or len(alive) < 2:
        return
    check("the guests see the same map Junk", alive_junk(guest, junk) == alive and alive_junk(old, junk) == alive,
          f"{alive_junk(guest, junk)} {alive_junk(old, junk)} {alive}")

    # 1. Junk
    mark_h, mark_g = len(host.packets), len(guest.packets)
    status = host.request(SPAWN_ENTITY, ENTITY_JUNK, -1.45)  # at the host's feet
    check("dev places the Object Manager's Junk", status == OK, str(status))
    host.pump(0.5)
    placed = [a for a in spawned_adds(host, mark_h) if '"inm":"pickup Junk"' in a[2]]
    check("SPAWNED_OBJ: the Junk trigger record", len(placed) == 1, str(spawned_adds(host, mark_h)))
    if not placed:
        return
    junk_uid = placed[0][0]
    mark_h = host.interact(INTERACT_KEY, junk_uid)
    guest.pump(0.3)
    lines = [c for c in chat_lines(host, mark_h) if "Junk" in c[2]]
    check("the host (no brain account): no Junk line, the credit has no answer to print", lines == [], str(chat_lines(host, mark_h)))
    check("the guest hears no Junk line", not [c for c in chat_lines(guest, mark_g) if "Junk" in c[2]], str(chat_lines(guest, mark_g)))
    mark_h = host.interact(INTERACT_KEY, junk_uid)
    check("taken: a second press prints nothing", not [c for c in chat_lines(host, mark_h) if "Junk" in c[2]], str(chat_lines(host, mark_h)))

    # 2. a weapon pickup: pick_0 to the picker
    mark_h, mark_g = len(host.packets), len(guest.packets)
    status = host.request(DROP_WEAPON, CROSSBOW, -1.45)
    check("dev drops a Crossbow at its feet", status == OK, str(status))
    host.pump(0.5)
    picks = [a for a in spawned_adds(host, mark_h) if '"i":26' in a[2]]
    check("SPAWNED_OBJ: the WEAPON_PICKUP record", len(picks) == 1, str(spawned_adds(host, mark_h)))
    if not picks:
        return
    mark_h = host.interact(INTERACT_PICKUP, picks[0][0])
    guest.pump(0.3)
    taken = any(len(p) >= 8 and struct.unpack("<ii", p[:8])[0] == picks[0][0] for p in host.of_type(PICKUP, mark_h))
    check("the host takes the Crossbow (PICKUP state)", taken)
    check("the host: pick_0 at 0.2, 2D", ("pick_0", -2, 0.2) in sounds(host, mark_h), str(sounds(host, mark_h)))
    check("the guest hears no pick_0", not [s for s in sounds(guest, mark_g) if s[0] == "pick_0"], str(sounds(guest, mark_g)))
    check("a weapon pickup prints no chat line (v7 has none)", not chat_lines(host, mark_h), str(chat_lines(host, mark_h)))

    # 3. the non-dev guest on the perms-on public server
    status = guest.request(SPAWN_ENTITY, ENTITY_JUNK, -1.45)
    check("the guest may not place the Object Manager's Junk (prod caps)", status == DENIED, str(status))
    target = alive[0]
    x, y, z = junk[target]
    brought = rcon("bring", guest.me["name"], str(x), str(y + 0.5), str(z))
    check("rcon brings the guest to a map Junk", "Brought" in brought, brought.strip()[-120:])
    guest.pump(0.3)
    mark_h = len(host.packets)
    mark_g = guest.interact(INTERACT_KEY, target)
    host.pump(0.2)
    check("the guest (no brain account): no Junk line for the map Junk", junk_lines(guest, mark_g) == [], str(chat_lines(guest, mark_g)))
    check("the guest: the Junk's sound (asset 29736, 2D)", any(n == "asset:29736" and uid == -2 for n, uid, _v in sounds(guest, mark_g)),
          str(sounds(guest, mark_g)))
    check("the map Junk is destroyed for everyone", (target, OBJECT_DESTROY) in object_states(guest, mark_g) and
          (target, OBJECT_DESTROY) in object_states(host, mark_h), f"{object_states(guest, mark_g)} {object_states(host, mark_h)}")
    check("the host hears no line of the guest's Junk", not junk_lines(host, mark_h), str(chat_lines(host, mark_h)))
    mark_g = guest.interact(INTERACT_KEY, target)
    check("the map Junk is gone: a second press prints nothing", not junk_lines(guest, mark_g), str(chat_lines(guest, mark_g)))

    given = rcon("give", "junk", guest.me["name"])
    guest.pump(0.5)
    om = [a for a in spawned_adds(guest) if '"inm":"pickup Junk"' in a[2] and a[0] != junk_uid]
    check("rcon give junk <the guest>: an Object Manager Junk at the guest's feet", len(om) == 1, given.strip()[-120:] + " " + str(om))
    if om:
        mark_h = len(host.packets)
        mark_g = guest.interact(INTERACT_KEY, om[0][0])
        host.pump(0.2)
        check("the guest (no brain account): no Junk line for the Object Manager Junk", junk_lines(guest, mark_g) == [],
              str(chat_lines(guest, mark_g)))
        check("the Object Manager Junk is destroyed", (om[0][0], OBJECT_DESTROY) in object_states(host, mark_h), str(object_states(host, mark_h)))
        check("the host hears no line of it", not junk_lines(host, mark_h), str(chat_lines(host, mark_h)))

    # 4. a protocol 1.3.0 guest, v3 "int" 1: the server's own scan finds the Junk
    target = alive[1]
    x, y, z = junk[target]
    brought = rcon("bring", old.me["name"], str(x), str(y + 0.5), str(z))
    check("rcon brings the 1.3.0 guest to another map Junk", "Brought" in brought, brought.strip()[-120:])
    old.pump(0.3)
    mark_o = old.interact_v3()
    check("the 1.3.0 guest (v3 \"int\", no brain account): no Junk line", junk_lines(old, mark_o) == [], str(chat_lines(old, mark_o)))
    check("that Junk is destroyed", (target, OBJECT_DESTROY) in object_states(old, mark_o), str(object_states(old, mark_o)))
    for c in (host, guest, old):
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
