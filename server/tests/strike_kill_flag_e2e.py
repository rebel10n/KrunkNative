#!/usr/bin/env python3
"""Krunker Strike kill card flag (NET_EXT_KN_STRIKE_KILL_FLAG 0x8131, net_ext.h).

  python3 -I server/tests/strike_kill_flag_e2e.py BUILD_DIR      (run from the repository root)

Part 1, a Strike server (--flavor strike, --open-cheats so a guest may /kill) with three connections:
  K  a Strike peer (HELLO flavor nibble 1) that kills,
  V1 a Strike peer that is killed,
  V2 a base peer (HELLO flavor 0) that is killed.
  1. K's /kill V1: V1 gets the 0x8131 packet before its KILL, with killer_uid = K and flag -1
     (a guest has no Country Flag); the body is 6 bytes.
  2. K's /kill V2: V2 gets its KILL but no 0x8131 (base peers never get the Strike kill card flag).
  3. K gets no 0x8131 (the card is the victim's alone).

Part 2, a real kn_brain (sqlite scratch database) and a Strike server linked to it:
  4. an account registers on the social socket and picks Country Flag 5 ("flg" with an index);
  5. the account joins the Strike server with its session token and kills a guest;
  6. the guest's 0x8131 carries flag 5: the pick shows on the next kill.
Part 2 is skipped (exit 77 when every part is skipped) without a kn_brain or the python websockets module.
"""
import asyncio
import json
import os
import re
import socket
import struct
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.getcwd(), "server/tests"))
import kn_proto  # noqa: E402

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
ROOT = os.getcwd()
BRAIN = os.path.join(BUILD, "services", "brain", "kn_brain")
WORK = tempfile.mkdtemp(prefix="kn-killflag-e2e-", dir=os.path.expanduser("~/.cache"))
EXT, KILL, CHAT_SEND = 254, 10, 8
SUBTYPE = 0x8131
STRIKE_HELLO = bytes([1, 4, 0, 0x10 | 1])  # HELLO major.minor.patch, rulesets: flavor nibble 1 (Strike) | ruleset 1 (EXT needs minor 4)
BASE_HELLO = bytes([1, 4, 0, 1])
FLAG_PICK = 5
results = []
procs = []


def check(name, ok, detail=""):
    results.append(bool(ok))
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def free_port():
    return kn_proto.free_ports(1)[0]


def start_server(port, log_name, extra=()):
    with open(os.path.join(WORK, "none.json"), "w") as f:
        f.write("{}\n")
    cfg = os.path.join(WORK, f"{log_name}.cfg")
    with open(cfg, "w") as f:
        f.write(f'hostname "Strike killflag"\nport {port}\nregion local\nmaxplayers 8\nmapcycle Sandstorm\nmode ffa\n')
    log = os.path.join(WORK, f"{log_name}.log")
    p = subprocess.Popen(
        [f"{BUILD}/server_bin", "--headless", "--cfg", cfg, "--config", os.path.join(WORK, "none.json"),
         "--flavor", "strike", "--open-cheats", *extra],
        stdout=open(log, "w"), stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
    procs.append(p)
    return p


def wait_port(port, timeout=20.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            socket.create_connection(("127.0.0.1", port), 0.5).close()
            return True
        except OSError:
            time.sleep(0.2)
    return False


def join_spawned(port, hello, session=""):
    """Connects, joins (with a session token for an account), enters the game; returns (conn, spawn info)."""
    c = kn_proto.Conn("127.0.0.1", port)
    c.send(kn_proto.HELLO, hello)
    c.join(name="kf", session=session)
    if c.read(until=2, timeout=8) is None:  # INIT
        return c, None
    c.ent(class_index=0, lag_comp=1.0)
    end = time.time() + 8
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
    """The 0x8131 bodies c has read, in order, with their packet index."""
    out = []
    for i, (t, payload) in enumerate(c.packets):
        if t == EXT and len(payload) >= 2 and (payload[0] | (payload[1] << 8)) == SUBTYPE:
            out.append((i, payload[2:]))
    return out


def kill_index(c, uid):
    for i, (t, payload) in enumerate(c.packets):
        if t == KILL and len(payload) >= 4 and struct.unpack("<i", payload[:4])[0] == uid:
            return i
    return None


def part_one():
    print("== part 1: guests, no brain")
    port = free_port()
    start_server(port, "s")
    if not wait_port(port):
        check("strike server up", False)
        return
    k, kinfo = join_spawned(port, STRIKE_HELLO)
    v1, v1info = join_spawned(port, STRIKE_HELLO)
    v2, v2info = join_spawned(port, BASE_HELLO)
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
        check("0x8131 arrives before V1's KILL", kill_at is not None and i < kill_at, f"ext at {i}, kill at {kill_at}")
        check("body is 6 bytes (int32 killer_uid, int16 flag)", len(body) == 6, f"{len(body)} bytes")
        if len(body) == 6:
            killer_uid, flag = struct.unpack("<ih", body)
            check("killer_uid is K", killer_uid == kinfo["uid"], f"got {killer_uid}, K {kinfo['uid']}")
            check("guest killer: flag -1", flag == -1, f"flag {flag}")
    check("K gets no 0x8131 (the card is the victim's)", not ext_flags(k))
    say(k, f"/kill {v2info['name']}")
    check("V2 (base) killed", v2.read(until=KILL, timeout=5) is not None)
    check("base victim V2 gets no 0x8131", not ext_flags(v2))
    for c in (k, v1, v2):
        c.close()


async def ws_call(ws, label, *frame):
    if frame:
        await ws.send(json.dumps(list(frame)))
    while True:
        f = json.loads(await asyncio.wait_for(ws.recv(), 15))
        if f[0] == label:
            return f


async def register_and_pick(brain_port, name, flag):
    import websockets  # noqa: PLC0415 (checked before this runs)

    async with websockets.connect(f"ws://127.0.0.1:{brain_port}/social/ws?enc=json", max_size=4 << 20) as ws:
        r = await ws_call(ws, "a", "a", 0, [name, "killflag-pass1", 0], "captcha")
        if r[1] is not None:
            return None, f"register: {r}"
        token = r[5]
        pick = await ws_call(ws, "flgr", "flg", flag)  # ["flg", index] -> ["flgr", index] (or a "po" refusal)
        return token, pick


def part_two():
    print("== part 2: a real kn_brain, the account's Country Flag on the next kill")
    try:
        import websockets  # noqa: F401,PLC0415
    except ImportError:
        print("SKIP  part 2: python websockets is not installed")
        return None
    if not os.path.exists(BRAIN):
        print(f"SKIP  part 2: no {BRAIN}")
        return None
    schema = f"killflag{os.getpid()}"
    scratch = [sys.executable, "-I", os.path.join(ROOT, "services", "brain", "tests", "db_scratch.py"), ROOT]
    r = subprocess.run(scratch + ["create", "sqlite", schema], capture_output=True)
    if r.returncode:
        print(f"SKIP  part 2: sqlite scratch database unavailable ({r.stderr.decode(errors='replace').strip()[:120]})")
        return None
    brain = None
    try:
        cfg = {"database": dict(json.loads(r.stdout), migrations_dir=os.path.join(ROOT, "services", "db", "migrations")),
               "brain": {"host": "127.0.0.1", "port": 0, "internal_secret": "kn-dev-secret", "federation": False}}
        cfg_path = os.path.join(WORK, "brain.json")
        with open(cfg_path, "w") as f:
            json.dump(cfg, f)
        blog = os.path.join(WORK, "brain.log")
        brain = subprocess.Popen([BRAIN, "--config", cfg_path, "--migrate"], stdout=open(blog, "w"),
                                 stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, cwd=ROOT)
        bport = None
        end = time.time() + 60
        while time.time() < end and not bport:
            m = re.search(r"listening on http://127\.0\.0\.1:(\d+) \(ws", open(blog, errors="replace").read())
            bport = int(m.group(1)) if m else None
            if not bport:
                time.sleep(0.2)
        check("kn_brain up", bool(bport))
        if not bport:
            return True
        name = "kfl" + str(int(time.time()) % 100000)
        token, pick = asyncio.run(register_and_pick(bport, name, FLAG_PICK))
        check("account registered and picked Country Flag 5", token is not None and pick[0] == "flgr" and pick[1] == FLAG_PICK,
              str(pick))
        if not token:
            return True
        port = free_port()
        start_server(port, "b", ("--brain", f"ws://127.0.0.1:{bport}/mm/internal/ws"))
        if not wait_port(port):
            check("strike server linked to the brain up", False)
            return True
        k, kinfo = join_spawned(port, STRIKE_HELLO, session=token)
        v, vinfo = join_spawned(port, STRIKE_HELLO)
        check("the account joined with its session and a guest spawned", bool(kinfo and vinfo),
              f"K={kinfo and kinfo['uid']} V={vinfo and vinfo['uid']}")
        if kinfo and vinfo:
            say(k, f"/kill {vinfo['name']}")
            check("the guest was killed by the account", v.read(until=KILL, timeout=8) is not None)
            flags = ext_flags(v)
            check("the guest got one 0x8131 packet", len(flags) == 1, f"{len(flags)} packets")
            if flags and len(flags[0][1]) == 6:
                killer_uid, flag = struct.unpack("<ih", flags[0][1])
                check("killer_uid is the account's player", killer_uid == kinfo["uid"], f"got {killer_uid}, K {kinfo['uid']}")
                check("the kill shows the picked flag (5) on the next kill", flag == FLAG_PICK, f"flag {flag}")
            for c in (k, v):
                c.close()
        return True
    finally:
        if brain:
            brain.terminate()
            try:
                brain.wait(5)
            except subprocess.TimeoutExpired:
                brain.kill()
        subprocess.run(scratch + ["drop", "sqlite", schema], capture_output=True)


def main():
    try:
        part_one()
        ran_two = part_two()
    finally:
        for p in procs:
            p.terminate()
            try:
                p.wait(5)
            except subprocess.TimeoutExpired:
                p.kill()
    print(f"{sum(results)}/{len(results)} passed; logs in {WORK}", flush=True)
    if not results:
        return 77
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
