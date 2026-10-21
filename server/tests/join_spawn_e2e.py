#!/usr/bin/env python3
"""The joiner's own SPAWN at join (server/src/net.c net_join_spawn): the official server's join, where the player is
dead and waiting until it sends ENT.

  python3 -I server/tests/join_spawn_e2e.py BUILD_DIR      (run from the repository root)

server_bin on server/tests/arena.json. A bridged browser joins: INIT, then the joiner's own SPAWN with active = 0
(is_you = 1), and nothing active for it until its ENT. After the ENT, the active SPAWN follows.
Prints PASS/FAIL lines; exit code 0 only when everything passed.
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
(PORT,) = kn_proto.free_ports(1)  # ephemeral: concurrent runs must not share servers
WORK = tempfile.mkdtemp(prefix="kn-join-spawn-e2e-")
SPAWN_FMT = "<iBBiifi3f2f32s"  # NetSpawnPacket's head: uid, is_you, active, class, team, hp, max, pos, dir, name
results = []
procs = []


def check(name, ok, detail=""):
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def query():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.settimeout(1.0)
        s.sendto(b"\xff\xff\xff\xffKNINFO\x00e2e", ("127.0.0.1", PORT))
        return s.recvfrom(2048)[0]


def start_server():
    cfg = os.path.join(WORK, "s.cfg")
    with open(cfg, "w") as f:
        f.write(f'hostname "Join spawn"\nport {PORT}\nregion local\nmaxplayers 8\nmode ffa\nround_seconds 600\n')
    with open(os.path.join(WORK, "none.json"), "w") as f:
        f.write("{}")
    out = open(os.path.join(WORK, "server.log"), "w")
    proc = subprocess.Popen([f"{BUILD}/server_bin", "--headless", "--cfg", cfg, "--config", os.path.join(WORK, "none.json"),
                             "server/tests/arena.json"], stdout=out, stderr=subprocess.STDOUT, stdin=subprocess.PIPE, text=True)
    procs.append(proc)
    end = time.time() + 15
    while time.time() < end:
        try:
            if query():
                return proc
        except OSError:
            time.sleep(0.2)
    raise SystemExit("server_bin did not answer the server-info query")


def spawn_of(payload):
    head = struct.calcsize(SPAWN_FMT)
    if len(payload) < head:
        return None
    v = struct.unpack(SPAWN_FMT, payload[:head])
    return {"uid": v[0], "is_you": v[1], "active": v[2], "name": kn_proto.uncstr(v[12])}


def spawns_for_me(conn):
    """Every SPAWN the joiner got with is_you = 1, in arrival order, as (packet index, spawn)."""
    out = []
    for i, (t, payload) in enumerate(conn.packets):
        if t == kn_proto.SPAWN:
            s = spawn_of(payload)
            if s and s["is_you"]:
                out.append((i, s))
    return out


def main():
    proc = start_server()
    try:
        c = kn_proto.Conn("127.0.0.1", PORT, timeout=5.0)
        c.hello()
        c.join(name="joiner")
        init = c.read(until=kn_proto.INIT, timeout=10)
        check("the joiner gets INIT", init is not None)
        if init is None:
            return
        init_at = next(i for i, (t, _) in enumerate(c.packets) if t == kn_proto.INIT)

        # Before ENT: our own SPAWN, dead (active = 0), after INIT and before any active spawn of us
        c.read(timeout=2.0)
        mine = spawns_for_me(c)
        dead = [(i, s) for i, s in mine if not s["active"]]
        check("the joiner gets its own SPAWN after INIT, dead (active = 0)", bool(dead) and dead[0][0] > init_at,
              f"is_you SPAWNs: {[(s['active'], s['name']) for _, s in mine]}")
        check("nothing active is spawned for the joiner before its ENT", not any(s["active"] for _, s in mine))

        # ENT: the joiner spawns (the active SPAWN follows; the join is otherwise unchanged)
        c.ent(class_index=0)
        end = time.time() + 8
        while time.time() < end and not any(s["active"] for _, s in spawns_for_me(c)):
            c.read(timeout=0.5)
        active = [s for _, s in spawns_for_me(c) if s["active"]]
        check("after ENT the joiner spawns (active = 1)", bool(active), f"{len(active)} active SPAWN(s)")
        check("the dead and the active SPAWN name the same player", bool(dead) and bool(active) and dead[0][1]["name"] == active[0]["name"],
              f"dead {dead[0][1]['name'] if dead else None!r}, active {active[0]['name'] if active else None!r}")
        c.close()
    finally:
        for p in procs:
            p.terminate()
            try:
                p.wait(timeout=10)
            except subprocess.TimeoutExpired:
                p.kill()


if __name__ == "__main__":
    main()
    passed = sum(results)
    print(f"{passed}/{len(results)} passed")
    sys.exit(0 if results and all(results) else 1)
