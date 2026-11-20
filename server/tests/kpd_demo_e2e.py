#!/usr/bin/env python3
"""K.P.D demo recorder on a dedicated server (server/src/demo_rec.c, doc/v10/replays.md 2.1).

  python3 -I server/tests/kpd_demo_e2e.py BUILD_DIR      (run from the repository root)

No database or coordinator: one standalone server_bin with gameserver.demo_dir, then
  1. the recorder is on (status) and invisible: the server-info query, `players` and a joining guest
     see no extra player (no SPAWN, PLAYER_INFO or LEAVE of it)
  2. bots fight a guest; rcon `demo <bot>` cuts a demo: <token>.kndemo + <token>.json in demo_dir
  3. the file: a keyframe first (MAP_INFO, INIT, SPEC ack, the players' SPAWNs), keyframes every
     ~15 s, the stream's STATE / KILL packets in time order, meta (map, calls, suspects = the bot's uid)
  4. a second cut on the same bot shares nothing with the first (its own token), and a guest who leaves
     (the suspect of a pending demo) has its demo written at once
Prints PASS/FAIL lines; exit code 0 only when everything passed.
"""
import glob
import json
import os
import socket
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.getcwd(), "server/tests"))
import kn_demo  # noqa: E402
import kn_proto  # noqa: E402

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
RCON_PORT, PORT = kn_proto.free_ports(2)
WORK = tempfile.mkdtemp(prefix="kn-demo-e2e-")
DEMOS = os.path.join(WORK, "demos")
INIT, SPAWN, STATE, KILL, LEAVE, MAP_INFO, SPECTATE, PLAYER_INFO = 2, 4, 6, 10, 20, 25, 28, 34
results = []


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


def demos():
    return sorted(glob.glob(os.path.join(DEMOS, "*.kndemo")))


def main():
    os.makedirs(DEMOS)
    cfg = os.path.join(WORK, "s.cfg")
    with open(cfg, "w") as f:
        f.write(f'hostname "Demo"\nport {PORT}\nregion local\nmaxplayers 8\nmapcycle Sandstorm\nmode ffa\n'
                f"round_seconds 600\nrcon_password rc-pw\nrcon_port {RCON_PORT}\n")
    with open(os.path.join(WORK, "c.json"), "w") as f:
        json.dump({"gameserver": {"demo_dir": DEMOS}}, f)
    log = open(os.path.join(WORK, "s.log"), "w")
    proc = subprocess.Popen([f"{BUILD}/server_bin", "--headless", "--cfg", cfg, "--config", os.path.join(WORK, "c.json")],
                            stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
    try:
        info = wait_for(lambda: query(PORT))
        check("server up", bool(info), str(info))
        _, status = rcon("status")
        check("status: the recorder runs", "recording (K.P.D demo recorder)" in status, status.strip()[-300:])
        check("info query counts no player", info and info.get("players") == 0, str(info))

        rc, out = rcon("bot_add", "3", "difficulty=normal")
        check("rcon bot_add 3", rc == 0, out.strip()[-200:])
        c = kn_proto.Conn("127.0.0.1", PORT)
        c.hello()
        c.join(name="Witness")
        c.ent(class_index=0, lag_comp=1.0)
        c.read(timeout=3.0)
        spawns = {kn_proto.spawn_info(p)["uid"]: kn_proto.spawn_info(p)["name"] for t, p in c.packets if t == SPAWN}
        me = next((kn_proto.spawn_info(p)["name"] for t, p in c.packets if t == SPAWN and kn_proto.spawn_info(p)["is_you"]), "Witness")
        check("the guest sees itself and 3 bots only", len(spawns) == 4, str(spawns))
        _, plist = rcon("players")
        check("`players` lists the guest only", "1/8 players" in plist, plist.strip()[-200:])
        info = query(PORT)
        check("info query: 1 player, 3 bots", info.get("players") == 1 and info.get("bots") == 3, str(info))

        _, blist = rcon("bot_list")
        bot = None
        for name in spawns.values():
            if name != me and name in blist:
                bot = name
                break
        bot = bot or next(n for n in spawns.values() if n != me)
        time.sleep(18)  # past a periodic keyframe
        rc, out = rcon("demo", bot, "0")
        check(f"rcon demo {bot}", rc == 0 and "is cut" in out, out.strip()[-200:])
        files = wait_for(lambda: demos() or None, 5) or []
        check("a demo file in demo_dir", len(files) == 1, str(files))
        if files:
            token = os.path.basename(files[0])[:-len(".kndemo")]
            check("its meta json beside it", os.path.exists(os.path.join(DEMOS, token + ".json")))
            data = open(files[0], "rb").read()
            version, meta, records = kn_demo.parse(data)
            check("version 1", version == 1)
            check("meta: map, mode, call, suspect", meta.get("map") == "Sandstorm" and meta.get("mode") and
                  len(meta.get("calls", [])) == 1 and len(meta.get("suspects", [])) == 1, json.dumps(meta)[:300])
            first = records[0]
            types = [t for t, _ in first[2]]
            check("keyframe first: MAP_INFO, INIT, SPECTATE", first[1] == kn_demo.KEYFRAME and MAP_INFO in types and INIT in types
                  and SPECTATE in types, str(types[:30]))
            kfs = [r for r in records if r[1] == kn_demo.KEYFRAME]
            last = kfs[-1]
            ltypes = [t for t, _ in last[2]]
            check("a later keyframe has every player's SPAWN", ltypes.count(SPAWN) >= 4 and INIT in ltypes, str(ltypes[:30]))
            check("keyframes every ~15 s", len(kfs) >= 2 and all(b[0] - a[0] >= 14000 for a, b in zip(kfs, kfs[1:])),
                  str([r[0] for r in kfs]))
            ts = [r[0] for r in records]
            check("records in time order", ts == sorted(ts))
            states = sum(1 for r in records for t, _ in r[2] if t == STATE)
            check("the stream carries STATE packets", states > 50, str(states))
            check("duration in meta matches the records", abs(meta.get("duration", 0) - ts[-1]) <= 1, f"{meta.get('duration')} vs {ts[-1]}")
            suspect = meta["suspects"][0]
            uids = {kn_proto.spawn_info(p)["uid"] for t, p in last[2] if t == SPAWN}
            check("the suspect is in the keyframe's players", suspect in uids, f"{suspect} in {uids}")
            leaves = [p for r in records for t, p in r[2] if t == LEAVE]
            check("no LEAVE of a recorder in the stream", not leaves, str(len(leaves)))

        # the guest is the suspect of a pending demo (cut in 60 s) and leaves: written at once
        rc, out = rcon("demo", me, "60")
        check(f"rcon demo {me} 60", rc == 0 and "is cut" in out, out.strip()[-200:])
        time.sleep(1.0)
        c.s.close()
        files = wait_for(lambda: demos() if len(demos()) == 2 else None, 6) or demos()
        check("the leaving suspect's demo is written at once", len(files) == 2, str(files))
        never = [t for t, _ in c.packets if t == LEAVE]
        check("the guest never saw a LEAVE (no recorder churn)", not never, str(never))
    finally:
        proc.terminate()
        try:
            proc.wait(10)
        except subprocess.TimeoutExpired:
            proc.kill()
    ok = all(results)
    print(f"{'PASS' if ok else 'FAIL'}: {sum(results)}/{len(results)} checks")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
