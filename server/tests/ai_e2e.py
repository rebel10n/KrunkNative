#!/usr/bin/env python3
"""Krunker v7.1.4 map AIs on a dedicated server (shared/src/ai.c, doc/v7/map-logic.md §2.8 / WP-F).

  python3 -I server/tests/ai_e2e.py BUILD_DIR      (run from the repository root)

One standalone server_bin on the v7.1.4 Soul Sanctum (20 AI_SPAWNERs), then
  1. a protocol 1.0 client (HELLO minor 0) gets the v3.1.1 AI packets: AI_ADD (90) / AI_SYNC (91)
     and none of the v7 block (160-164)
  2. a v7 client (HELLO minor 3) gets AI_ADD_V7 (162) / AI_SYNC_V7 (161, stride 9: didMov, team)
     and no AI_ADD / AI_SYNC
  3. "/kill wandering" in chat (a loopback peer is the host: DEV): the Wandering Souls (no respawn
     time) die and are removed: the v7 client gets AI_REMOVE (160, "rai") for every one it had seen;
     the old client simply stops seeing them
Prints PASS/FAIL lines; exit code 0 only when everything passed.
"""
import json
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.getcwd(), "server/tests"))
import kn_proto  # noqa: E402

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
PORT = 22431
RCON_PORT = 22498
WORK = tempfile.mkdtemp(prefix="kn-ai-e2e-")
SPAWN, CHAT_SEND, CHAT = 4, 8, 9
AI_ADD, AI_SYNC = 90, 91
AI_REMOVE, AI_SYNC_V7, AI_ADD_V7, AI_ANIM, AI_SHOT = 160, 161, 162, 163, 164
V7_TYPES = {AI_REMOVE, AI_SYNC_V7, AI_ADD_V7, AI_ANIM, AI_SHOT}
NAME_SIZE = 40
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
    return json.loads(data[11:])


def rcon(*cmd):
    out = subprocess.run([f"{BUILD}/server_bin", "--rcon", f"127.0.0.1:{RCON_PORT}", "--rcon-password", "rc-pw", *cmd],
                         capture_output=True, text=True, timeout=10)
    return out.returncode, out.stdout + out.stderr


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


def join(minor, name):
    c = kn_proto.Conn("127.0.0.1", PORT)
    c.send(kn_proto.HELLO, bytes([1, minor, 0]))
    c.join(name=name)
    c.ent(class_index=0, lag_comp=1.0)
    me = None
    end = time.time() + 5
    while time.time() < end and me is None:
        p = c.read(until=SPAWN, timeout=1)
        if p is not None and kn_proto.spawn_info(p)["is_you"]:
            me = kn_proto.spawn_info(p)
    return c, me


def types(c):
    return [t for t, _ in c.packets]


def adds_v7(c):
    """sid -> name of every AI_ADD_V7 read so far (NetAIAddV7Packet: sid i, index i, name 40s, ...)."""
    out = {}
    for t, p in c.packets:
        if t == AI_ADD_V7 and len(p) >= 8 + NAME_SIZE:
            sid, _ = struct.unpack("<ii", p[:8])
            out[sid] = kn_proto.uncstr(p[8 : 8 + NAME_SIZE])
    return out


def sync_v7_entries(c):
    """[(sid, x, y, z, xD, yD, health, didMov, team)] of the last AI_SYNC_V7 frames."""
    entries = []
    for t, p in c.packets:
        if t == AI_SYNC_V7 and len(p) >= 2:
            first, count = p[0], p[1]
            if first:
                entries = []
            for i in range(count):
                e = p[2 + i * 31 : 2 + (i + 1) * 31]
                if len(e) == 31:
                    entries.append(struct.unpack("<i5fiBh", e))
    return entries


def main():
    with open(os.path.join(WORK, "none.json"), "w") as f:
        f.write("{}")
    cfg = os.path.join(WORK, "s.cfg")
    with open(cfg, "w") as f:
        f.write(f'hostname "AI e2e"\nport {PORT}\nregion local\nmaxplayers 8\nmapcycle "Soul Sanctum"\nmode ffa\n'
                f"round_seconds 600\nrcon_password rc-pw\nrcon_port {RCON_PORT}\n")
    log = open(os.path.join(WORK, "s.log"), "w")
    procs.append(subprocess.Popen([f"{BUILD}/server_bin", "--headless", "--cfg", cfg, "--config", os.path.join(WORK, "none.json")],
                                  stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL))
    info = wait_for(lambda: query(PORT))
    check("server up", bool(info), str({k: info.get(k) for k in ("map", "mode")}) if info else "no reply")

    old, old_me = join(0, "Old")
    new, new_me = join(3, "New")
    check("both clients spawned", old_me is not None and new_me is not None)
    old.read(timeout=3.0)
    new.read(timeout=3.0)

    # 1. the 1.0 client: v3.1.1 packets only
    ot = types(old)
    check("1.0 client: AI_ADD / AI_SYNC", ot.count(AI_ADD) >= 3 and ot.count(AI_SYNC) >= 3,
          f"aai {ot.count(AI_ADD)} ai {ot.count(AI_SYNC)}")
    check("1.0 client: no v7 AI packets", not any(t in V7_TYPES for t in ot))

    # 2. the v7 client: the v7 block only, stride 9 with team 2 (Soul Sanctum's tm 2)
    nt = types(new)
    added = adds_v7(new)
    check("v7 client: AI_ADD_V7 / AI_SYNC_V7", len(added) >= 3 and nt.count(AI_SYNC_V7) >= 3,
          f"aai {len(added)} ({sorted(set(added.values()))}) ai {nt.count(AI_SYNC_V7)}")
    check("v7 client: no v3.1.1 AI packets", AI_ADD not in nt and AI_SYNC not in nt)
    entries = sync_v7_entries(new)
    check("AI_SYNC_V7 entries: team 2, in range", bool(entries) and all(e[8] == 2 for e in entries),
          f"{len(entries)} entries, teams {sorted(set(e[8] for e in entries))}")

    # 3. kill the Wandering Souls (no respawn time): "rai" for the ones the v7 client had
    wandering = {sid for sid, name in added.items() if name == "Wandering Soul"}
    old.send(CHAT_SEND, kn_proto.cstr("/kill wandering", 288))
    reply = ""
    end = time.time() + 3
    while time.time() < end and "Killed" not in reply:
        p = old.read(until=CHAT, timeout=1)
        if p is not None:
            reply = kn_proto.uncstr(p[-288:]) if len(p) >= 288 else ""
    check("/kill wandering", "Killed" in reply and "AIs" in reply, reply)
    new.read(timeout=2.0)
    removed = {struct.unpack("<i", p[:4])[0] for t, p in new.packets if t == AI_REMOVE and len(p) == 4}
    check("AI_REMOVE for every seen Wandering Soul", bool(wandering) and wandering <= removed,
          f"seen {sorted(wandering)} removed {sorted(removed)}")
    old.read(timeout=1.0)
    check("1.0 client: still no v7 packets", not any(t in V7_TYPES for t in types(old)))

    old.close()
    new.close()


try:
    main()
finally:
    for p in procs:
        p.terminate()
        try:
            p.wait(timeout=5)
        except subprocess.TimeoutExpired:
            p.kill()

print(f"{sum(results)}/{len(results)} passed  (logs: {WORK})")
sys.exit(0 if results and all(results) else 1)
