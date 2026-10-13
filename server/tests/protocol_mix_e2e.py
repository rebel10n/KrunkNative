#!/usr/bin/env python3
"""Protocol 1.3.0 / 1.3.1 peers on a dedicated server (player-viewer WP-C, NET_PROTOCOL_PATCH).

  python3 -I server/tests/protocol_mix_e2e.py BUILD_DIR [OLD_SERVER_BIN [WPC_SERVER_BIN]]   (run from the repository root)

1.3.1 grows three packets: ENT V8 (attachIndex, challMode, anonMode), SPAWN V3 (attachIndex, headMlt)
and RECON V2 (wallJumpC, canSlideT, slide ms). A 1.3.0 build checks exact sizes, so:
  A. BUILD_DIR/server_bin (1.3.1) with three peers over TCP:
     - a 1.3.0 peer (HELLO 1.3.0, ENT V7) spawns and only ever gets SPAWN V2 / RECON V1,
       also for the 1.3.1 player;
     - a 2-byte HELLO (1.3, no patch byte) counts as 1.3.0;
     - a 1.3.1 peer (HELLO 1.3.1, ENT V8) gets INIT patch 1, SPAWN V3 carrying its attachIndex and
       headMlt 1, RECON V2, and SPAWN V3 for the 1.3.0 player too.
  B. with OLD_SERVER_BIN (a server_bin built before WP-C, INIT 1.3.0): a 1.3.1 client picks the ENT
     size from INIT (net_ent_size_for: V7), spawns, and reads SPAWN V2 / RECON V1; the V8 ENT the gate
     avoids is dropped by that server (no spawn).
WP-F (still 1.3.1, no new patch number) appends the v7 cosmetic slots: ENT V9 (+ showBadges) and
SPAWN V4. 1.3.1 readers prefix-read longer packets, so a 1.3.1 peer gets SPAWN V4 (>= V3) and sends
ENT V9; A checks the V4 tail (the 1.3.1 peer's pet / shoes, no badge; none for the 1.3.0 player).
  C. with WPC_SERVER_BIN (a 1.3.1 server_bin built before WP-F): the same client's ENT V9 spawns it
     (V8 prefix) and it reads that server's SPAWN V3.
The offline sandbox path is server/tests/sandbox_test.c (test_wpc_peer_mix).
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
OLD_SERVER = sys.argv[2] if len(sys.argv) > 2 else None
WPC_SERVER = sys.argv[3] if len(sys.argv) > 3 else None
PORT = 22441
WORK = tempfile.mkdtemp(prefix="kn-proto-mix-")
INIT, SPAWN, INPUT, RECON = 2, 4, 5, 110
SPAWN_V2, SPAWN_V3, SPAWN_V4 = 106, 114, 150  # NET_SPAWN_PACKET_V2_SIZE / V3 / V4 (WP-F)
RECON_V1, RECON_V2 = 58, 72       # NET_RECON_PACKET_V1_SIZE / V2
ENT_V7, ENT_V8, ENT_V9 = 58, 64, 101  # NET_ENT_PACKET_V7_SIZE / V8 / V9 (WP-F)
# WP-F PlayerCosmeticsV7: head, back, shoe, pet, wrist, charms[2], player_card, badge
PET, SHOES = 2964, 2860
V7_SLOTS = (-1, -1, SHOES, PET, -1, -1, -1, -1, 5)  # the ENT's badge is ignored (the account's)
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


def start_server(binary, tag):
    with open(os.path.join(WORK, "none.json"), "w") as f:
        f.write("{}")
    cfg = os.path.join(WORK, f"{tag}.cfg")
    with open(cfg, "w") as f:
        f.write(f'hostname "proto mix {tag}"\nport {PORT}\nregion local\nmaxplayers 8\nmapcycle "Soul Sanctum"\nmode ffa\n'
                f"round_seconds 600\n")
    log = open(os.path.join(WORK, f"{tag}.log"), "w")
    p = subprocess.Popen([binary, "--headless", "--cfg", cfg, "--config", os.path.join(WORK, "none.json")],
                         stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
    procs.append(p)
    return p, wait_for(lambda: query(PORT))


def stop(p):
    p.terminate()
    try:
        p.wait(timeout=5)
    except subprocess.TimeoutExpired:
        p.kill()
    time.sleep(0.5)


def ent_payload(size, attach=1, chall=1, anon=1):
    """ENT V7 (58 bytes: class, secondary, cosmetics 7i, spectate, slid, spray, skins 3i, lagComp) + V8 tail."""
    payload = struct.pack("<ii7iBBi3if", 0, -1, *([-1] * 7), 0, 1, 0, -1, -1, -1, 1.0)
    if size >= ENT_V8:
        payload += struct.pack("<iBB", attach, chall, anon)
    if size >= ENT_V9:
        payload += struct.pack("<9iB", *V7_SLOTS, 0x0F)
    return payload


def ent_size_for(major, minor, patch):
    """shared/src/net.c net_ent_size_for, for the 1.x servers this test meets."""
    if major == 1 and (minor > 3 or (minor == 3 and patch >= 1)):
        return ENT_V9
    return ENT_V7 if major == 1 and minor >= 2 else 0


def connect(hello, ent_size=None, name="p"):
    c = kn_proto.Conn("127.0.0.1", PORT)
    c.send(kn_proto.HELLO, bytes(hello))
    c.join(name=name)
    init = c.read(until=INIT, timeout=5)
    info = struct.unpack("<BBBiiHi", init[:17]) if init and len(init) >= 17 else None
    size = ent_size if ent_size is not None else (ent_size_for(*info[:3]) if info else ENT_V7)
    c.send(kn_proto.ENT, ent_payload(size))
    return c, info, size


def spawns(c):
    """[(length, uid, is_you, active, attach, head, v7)] of every SPAWN read so far (v7: the WP-F
    slots of a V4, else None)."""
    out = []
    for t, p in c.packets:
        if t != SPAWN or len(p) < SPAWN_V2:
            continue
        uid, is_you, active = struct.unpack("<iBB", p[:6])
        attach, head = struct.unpack("<if", p[SPAWN_V2:SPAWN_V3]) if len(p) >= SPAWN_V3 else (0, 1.0)
        v7 = struct.unpack("<9i", p[SPAWN_V3:SPAWN_V4]) if len(p) >= SPAWN_V4 else None
        out.append((len(p), uid, is_you, active, attach, head, v7))
    return out


def me(c):
    mine = [s for s in spawns(c) if s[2] and s[3]]
    return mine[-1] if mine else None


def move(c, frames=30):
    for seq in range(frames):
        c.send(INPUT, struct.pack("<iifffBBBb", seq, 0, 1.0 / 60.0, 0.0, 0.0, 0, 0, 0, 0))


def recon_lengths(c):
    return sorted({len(p) for t, p in c.packets if t == RECON})


def pump(conns, seconds):
    end = time.time() + seconds
    while time.time() < end:
        for c in conns:
            c.read(timeout=0.05)


def phase_new_server():
    p, info = start_server(f"{BUILD}/server_bin", "new")
    check("1.3.1 server up", bool(info))
    old, _, _ = connect([1, 3, 0], ent_size=ENT_V7, name="Old")  # a 1.3.0 build always sends V7
    bare, _, _ = connect([1, 3], ent_size=ENT_V7, name="Bare")
    new, new_init, new_size = connect([1, 3, 1], name="New")
    pump([old, bare, new], 2.0)
    # 1.4 (NET_PROTOCOL_MINOR_EXT, the v10 envelope) keeps every 1.3.1 size: patch 1 rides along
    check("INIT announces 1.3.1 or later", new_init is not None and (new_init[1] > 3 or new_init[1:3] == (3, 1)) and new_init[2] >= 1, str(new_init))
    check("ENT size from INIT: V9 to a 1.3.1 server", new_size == ENT_V9, str(new_size))

    m_old, m_bare, m_new = me(old), me(bare), me(new)
    check("1.3.0 peer: spawned, SPAWN V2", m_old is not None and m_old[0] == SPAWN_V2, str(m_old))
    check("2-byte HELLO: patch 0, SPAWN V2", m_bare is not None and m_bare[0] == SPAWN_V2, str(m_bare))
    check("1.3.1 peer: SPAWN >= V3, attachIndex 1, headMlt 1", m_new is not None and m_new[0] >= SPAWN_V3 and m_new[4] == 1 and m_new[5] == 1.0,
          str(m_new))
    check("1.3.1 peer: SPAWN V4 with its ENT V9 pet / shoes, no badge (a guest)",
          m_new is not None and m_new[0] == SPAWN_V4 and m_new[6] is not None and m_new[6][3] == PET and m_new[6][2] == SHOES and m_new[6][8] == -1,
          str(m_new))
    if m_old and m_new:
        old_sees = [s[0] for s in spawns(old) if s[1] == m_new[1] and s[3]]
        new_sees = [(s[0], s[4], s[6]) for s in spawns(new) if s[1] == m_old[1] and s[3]]
        check("1.3.0 peer sees the 1.3.1 player as V2", bool(old_sees) and set(old_sees) == {SPAWN_V2}, str(old_sees))
        check("1.3.1 peer sees the 1.3.0 player as V3+ (attachIndex 0, no v7 slots)",
              bool(new_sees) and all(n >= SPAWN_V3 and a == 0 and (v7 is None or set(v7) == {-1}) for n, a, v7 in new_sees), str(new_sees))
    move(old)
    move(new)
    pump([old, bare, new], 2.5)
    check("1.3.0 peer: RECON V1 only", recon_lengths(old) == [RECON_V1], str(recon_lengths(old)))
    check("1.3.1 peer: RECON V2 only", recon_lengths(new) == [RECON_V2], str(recon_lengths(new)))
    for c in (old, bare, new):
        c.close()
    stop(p)


def phase_old_server():
    p, info = start_server(OLD_SERVER, "old")
    check("1.3.0 server up", bool(info))
    new, init, size = connect([1, 3, 1], name="New")
    pump([new], 2.0)
    check("old server INIT 1.3.0 -> the client sends ENT V7", init is not None and init[1:3] == (3, 0) and size == ENT_V7, f"{init} {size}")
    m = me(new)
    check("1.3.1 client on a 1.3.0 server: spawned, SPAWN V2", m is not None and m[0] == SPAWN_V2, str(m))
    move(new)
    pump([new], 2.5)
    check("1.3.1 client on a 1.3.0 server: RECON V1", recon_lengths(new) == [RECON_V1], str(recon_lengths(new)))
    # why the gate exists: the same server drops a V8 ENT without a spawn
    v8, _, _ = connect([1, 3, 1], ent_size=ENT_V8, name="Eager")
    pump([new, v8], 2.0)
    check("a V8 ENT to a 1.3.0 server spawns nobody (gated by INIT)", me(v8) is None)
    new.close()
    v8.close()
    stop(p)


def phase_wpc_server():
    p, info = start_server(WPC_SERVER, "wpc")
    check("1.3.1 (pre-WP-F) server up", bool(info))
    new, init, size = connect([1, 3, 1], name="New")
    pump([new], 2.0)
    check("pre-WP-F 1.3.1 server INIT 1.3.1 -> the client sends ENT V9", init is not None and init[1:3] == (3, 1) and size == ENT_V9, f"{init} {size}")
    m = me(new)
    check("its V9 ENT spawns it (V8 prefix), SPAWN V3", m is not None and m[0] == SPAWN_V3 and m[4] == 1, str(m))
    new.close()
    stop(p)


try:
    phase_new_server()
    if OLD_SERVER:
        phase_old_server()
    else:
        print("SKIP  old-server phase (no OLD_SERVER_BIN given)")
    if WPC_SERVER:
        phase_wpc_server()
    else:
        print("SKIP  pre-WP-F 1.3.1 server phase (no WPC_SERVER_BIN given)")
finally:
    for proc in procs:
        if proc.poll() is None:
            proc.kill()

print(f"{sum(results)}/{len(results)} passed  (logs: {WORK})")
sys.exit(0 if results and all(results) else 1)
