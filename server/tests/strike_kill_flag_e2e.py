#!/usr/bin/env python3
"""Krunker Strike kill card flag (NET_EXT_KN_STRIKE_KILL_FLAG 0x8131, net_ext.h).

  python3 -I server/tests/strike_kill_flag_e2e.py BUILD_DIR      (run from the repository root)

A Strike server (--flavor strike, --open-cheats so a guest may /kill) with three connections:
  K  a Strike peer (HELLO flavor nibble 1) that kills,
  V1 a Strike peer that is killed,
  V2 a base peer (HELLO flavor 0) that is killed.
Checks:
  1. K's /kill V1: V1 gets the 0x8131 packet before its KILL, with killer_uid = K and flag -1
     (a guest has no Country Flag); the body is 6 bytes.
  2. K's /kill V2: V2 gets its KILL but no 0x8131 (base peers never get the Strike kill card flag).
  3. K gets no 0x8131 (the card is the victim's alone).
"""
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
PORT = kn_proto.free_ports(1)[0]
WORK = tempfile.mkdtemp(prefix="kn-killflag-e2e-", dir=os.path.expanduser("~/.cache"))
EXT, KILL, CHAT_SEND = 254, 10, 8
SUBTYPE = 0x8131
STRIKE_HELLO = bytes([1, 4, 0, 0x10 | 1])  # HELLO major.minor.patch, rulesets: flavor nibble 1 (Strike) | ruleset 1 (EXT needs minor 4)
BASE_HELLO = bytes([1, 4, 0, 1])
results = []
proc = None


def check(name, ok, detail=""):
    results.append(bool(ok))
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def start_server():
    with open(os.path.join(WORK, "none.json"), "w") as f:
        f.write("{}\n")
    cfg = os.path.join(WORK, "s.cfg")
    with open(cfg, "w") as f:
        f.write(f'hostname "Strike killflag"\nport {PORT}\nregion local\nmaxplayers 8\nmapcycle Sandstorm\nmode ffa\n')
    log = os.path.join(WORK, "s.log")
    return subprocess.Popen(
        [f"{BUILD}/server_bin", "--headless", "--cfg", cfg, "--config", os.path.join(WORK, "none.json"),
         "--flavor", "strike", "--open-cheats"],
        stdout=open(log, "w"), stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL), log


def wait_port(timeout=20.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            socket.create_connection(("127.0.0.1", PORT), 0.5).close()
            return True
        except OSError:
            time.sleep(0.2)
    return False


def join_spawned(hello):
    """Connects, joins, enters the game and returns (conn, own spawn info)."""
    c = kn_proto.Conn("127.0.0.1", PORT)
    c.send(kn_proto.HELLO, hello)
    c.join(name="kf")
    if c.read(until=2, timeout=5) is None:  # INIT
        return c, None
    c.ent(class_index=0, lag_comp=1.0)
    end = time.time() + 5
    while time.time() < end:
        p = c.read(until=kn_proto.SPAWN, timeout=1)
        if p is not None:
            info = kn_proto.spawn_info(p)
            if info and info["is_you"]:
                return c, info
    return c, None


def say(c, text):
    c.send(CHAT_SEND, text.encode() + b"\0")


def ext_flags(c):
    """(index, killer_uid, flag) for every 0x8131 packet c has read, with the envelope's index."""
    out = []
    for i, (t, payload) in enumerate(c.packets):
        if t != EXT or len(payload) < 2:
            continue
        subtype = payload[0] | (payload[1] << 8)
        if subtype == SUBTYPE:
            out.append((i, payload[2:]))
    return out


def kill_index(c, uid):
    for i, (t, payload) in enumerate(c.packets):
        if t == KILL and len(payload) >= 4 and struct.unpack("<i", payload[:4])[0] == uid:
            return i
    return None


def main():
    global proc
    proc, log = start_server()
    try:
        check("strike server up", wait_port())
        k, kinfo = join_spawned(STRIKE_HELLO)
        v1, v1info = join_spawned(STRIKE_HELLO)
        v2, v2info = join_spawned(BASE_HELLO)
        check("killer and both victims spawned", all([kinfo, v1info, v2info]),
              f"uids K={kinfo and kinfo['uid']} V1={v1info and v1info['uid']} V2={v2info and v2info['uid']}")
        if not all([kinfo, v1info, v2info]):
            return
        say(k, f"/kill {v1info['name']}")
        check("V1 killed", v1.read(until=KILL, timeout=5) is not None)
        flags = ext_flags(v1)
        check("V1 got one 0x8131 packet", len(flags) == 1, f"{len(flags)} packets")
        if flags:
            i, body = flags[0]
            kill_at = kill_index(v1, v1info["uid"])
            check("0x8131 arrives before V1's KILL", kill_at is not None and i < kill_at,
                  f"ext at {i}, kill at {kill_at}")
            check("body is 6 bytes (int32 killer_uid, int16 flag)", len(body) == 6, f"{len(body)} bytes")
            if len(body) == 6:
                killer_uid, flag = struct.unpack("<ih", body)
                check("killer_uid is K", killer_uid == kinfo["uid"], f"got {killer_uid}, K {kinfo['uid']}")
                check("guest killer: flag -1", flag == -1, f"flag {flag}")
        check("K gets no 0x8131 (the card is the victim's)", not ext_flags(k))

        say(k, f"/kill {v2info['name']}")
        check("V2 (base) killed", v2.read(until=KILL, timeout=5) is not None)
        check("base victim V2 gets no 0x8131", not ext_flags(v2))
    finally:
        if proc:
            proc.terminate()
            try:
                proc.wait(5)
            except subprocess.TimeoutExpired:
                proc.kill()
        for c in ("k", "v1", "v2"):
            obj = locals().get(c)
            if obj:
                obj.close()
    print(f"{sum(results)}/{len(results)} passed; log {log}", flush=True)


if __name__ == "__main__":
    main()
    sys.exit(0 if results and all(results) else 1)
