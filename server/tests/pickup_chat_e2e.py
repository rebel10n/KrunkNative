#!/usr/bin/env python3
"""Pickup feedback on a public server_bin: Junk in chat (v7 io_foundJunk) and the weapon pickup sound.

  python3 -I server/tests/pickup_chat_e2e.py BUILD_DIR      (run from the repository root)

No database or coordinator: one standalone server_bin (a public game) from a generated server.cfg,
on ports from kn_proto.free_ports(). The host connects over loopback (a DEV peer, so the Object
Manager places things for it); the guest writes a NET_PACKET_FORWARDED first (a remote player).
  1. the host places the Object Manager's Junk (a TRIGGER, onInteractKey, Reward Junk "0.01") at its
     feet and presses G ("int" 7): the host gets CHAT kind NET_CHAT_FOUND_JUNK
     "You found [0.01] Junk", the guest gets no such line, and the Junk is gone (a second press:
     no second line)
  2. the host drops a Crossbow and takes it back ("int" 6): "pick_0" at 0.2 as a 2D sound
     (uid -2) to the host only (v7 io_playerInteractions)
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
WORK = tempfile.mkdtemp(prefix="kn-pickup-chat-e2e-", dir=os.path.expanduser("~/.cache"))
SPAWN, CHAT, INTERACT, FORWARDED, SOUND, PICKUP = 4, 9, 61, 85, 102, 128
SPAWN_REQ, SPAWN_RESULT, SPAWNED_OBJ = 250, 251, 253
DROP_WEAPON, SPAWN_ENTITY = 4, 5
OK = 0
ENTITY_JUNK = 17  # object_manager_entries(OM_TAB_ENTITIES): after KR Tag .. Airdrop (shared/src/object_manager.c k_entities)
CROSSBOW = 13
INTERACT_PICKUP, INTERACT_KEY = 6, 7
CHAT_FOUND_JUNK = 3  # NET_CHAT_FOUND_JUNK
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
        f.write(f'hostname "Pickup chat e2e"\nport {PORT}\nregion local\nmaxplayers 8\nmapcycle Sandstorm\nmode ffa\nround_seconds 600\n')
    out = open(os.path.join(WORK, "s.log"), "w")
    p = subprocess.Popen([f"{BUILD}/server_bin", "--headless", "--cfg", cfg, "--config", os.path.join(WORK, "none.json")],
                         stdout=out, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
    procs.append(p)
    return p


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
    def __init__(self, port, remote_address=None):
        super().__init__("127.0.0.1", port)
        if remote_address:
            self.send(FORWARDED, remote_address.encode() + b"\0")
        self.send(kn_proto.HELLO, bytes([1, 3, 1]))
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
    host = Peer(PORT)
    check("host spawned", host.spawn() is not None)
    guest = Peer(PORT, "203.0.113.9")
    check("guest spawned", guest.spawn() is not None)
    if not host.me or not guest.me:
        return

    # 1. Junk
    mark_h, mark_g = len(host.packets), len(guest.packets)
    status = host.request(SPAWN_ENTITY, ENTITY_JUNK, -1.45)  # at the host's feet
    check("dev places the Object Manager's Junk", status == OK, str(status))
    host.pump(0.5)
    junk = [a for a in spawned_adds(host, mark_h) if '"inm":"pickup Junk"' in a[2]]
    check("SPAWNED_OBJ: the Junk trigger record", len(junk) == 1, str(spawned_adds(host, mark_h)))
    if not junk:
        return
    mark_h = host.interact(INTERACT_KEY, junk[0][0])
    guest.pump(0.3)
    lines = [c for c in chat_lines(host, mark_h) if "Junk" in c[2]]
    check("the host: CHAT kind FOUND_JUNK \"You found [0.01] Junk\"", lines == [(-1, CHAT_FOUND_JUNK, "You found [0.01] Junk")], str(chat_lines(host, mark_h)))
    check("the guest hears no Junk line", not [c for c in chat_lines(guest, mark_g) if "Junk" in c[2]], str(chat_lines(guest, mark_g)))
    mark_h = host.interact(INTERACT_KEY, junk[0][0])
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
    for c in (host, guest):
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
