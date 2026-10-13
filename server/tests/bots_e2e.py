#!/usr/bin/env python3
"""Krunker v7.1.4 bot players on a dedicated server (shared/src/bots.c, doc/console.md "Bots").

  python3 -I server/tests/bots_e2e.py BUILD_DIR      (run from the repository root)

No database or coordinator: one standalone server_bin from a generated server.cfg, then
  1. rcon bot_add 4: the reply, the server-info query (bots 4, players 0)
  2. a guest joins: every bot reaches it as a player (SKINS + SPAWN + an account-style
     PLAYER_INFO: level, clan, account id), names unique (a non-DEV player's bot_* is refused:
     shared/tests/bot_test.c; loopback connections count as the host's own machine here)
  3. the bots fight: KILL packets with bot killers, LEADERBOARD rows for the bots, states
  4. a late joiner gets the bots' SPAWN + PLAYER_INFO
  5. rcon bot_list / bot_difficulty / bot_stop / bot_kill / bot_kick (LEAVE per bot)
  6. a second server with `bot_quota 3`: none in an empty lobby, 2 once a human joins
     (fill mode in depth: server/tests/bot_fill_e2e.py)
Prints PASS/FAIL lines; exit code 0 only when everything passed.
"""
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time
import json

sys.path.insert(0, os.path.join(os.getcwd(), "server/tests"))
import kn_proto  # noqa: E402

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
RCON_PORT, *PORTS = kn_proto.free_ports(3)  # ephemeral: concurrent runs must not share servers
WORK = tempfile.mkdtemp(prefix="kn-bots-e2e-")
SPAWN, KILL, LEADERBOARD, LEAVE, STATE, PLAYER_INFO, SKINS = 4, 10, 14, 20, 6, 34, 35
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


def start_server(i, extra):
    cfg = os.path.join(WORK, f"s{i}.cfg")
    with open(cfg, "w") as f:
        f.write(f'hostname "Bots #{i}"\nport {PORTS[i]}\nregion local\nmaxplayers 8\nmapcycle Sandstorm\nmode ffa\n'
                f"round_seconds 600\n{extra}")
    out = open(os.path.join(WORK, f"s{i}.log"), "w")
    p = subprocess.Popen([f"{BUILD}/server_bin", "--headless", "--cfg", cfg, "--config", os.path.join(WORK, "none.json")],
                         stdout=out, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
    procs.append(p)
    return p


def join(port, name):
    c = kn_proto.Conn("127.0.0.1", port)
    c.hello()
    c.join(name=name)
    c.ent(class_index=0, lag_comp=1.0)
    me = None
    end = time.time() + 5
    while time.time() < end and me is None:
        p = c.read(until=SPAWN, timeout=1)
        if p is not None and kn_proto.spawn_info(p)["is_you"]:
            me = kn_proto.spawn_info(p)
    return c, me


def bot_spawns(c, me_uid):
    """uid -> name of every other player's SPAWN read so far."""
    out = {}
    for t, p in c.packets:
        if t == SPAWN:
            info = kn_proto.spawn_info(p)
            if info and info["uid"] != me_uid:
                out[info["uid"]] = info["name"]
    return out


def infos(c):
    """uid -> (account_id, level, flags, clan) of every PLAYER_INFO read so far."""
    out = {}
    for t, p in c.packets:
        if t == PLAYER_INFO and len(p) >= 21:
            uid, acc, level, flags = struct.unpack("<iiiB", p[:13])
            out[uid] = (acc, level, flags, kn_proto.uncstr(p[13:21]))
    return out


def main():
    with open(os.path.join(WORK, "none.json"), "w") as f:
        f.write("{}")
    start_server(0, f"rcon_password rc-pw\nrcon_port {RCON_PORT}\n")
    info = wait_for(lambda: query(PORTS[0]))
    check("server up", bool(info), str(info))

    # 1. rcon bot_add
    rc, out = rcon("bot_add", "4", "difficulty=normal")
    check("rcon bot_add 4", rc == 0 and "Spawned 4 nextbot bots." in out, out.strip()[-200:])
    info = wait_for(lambda: query(PORTS[0]) if query(PORTS[0]).get("bots") == 4 else None, 5) or query(PORTS[0])
    check("info query: bots 4, players 0", info.get("bots") == 4 and info.get("players") == 0, str({k: info.get(k) for k in ("bots", "players", "max")}))

    # 2. a guest sees them as players
    a, me = join(PORTS[0], "Tester")
    check("guest spawned", me is not None)
    a.read(timeout=2.0)
    bots = bot_spawns(a, me["uid"] if me else -1)
    check("4 bot players (SPAWN)", len(bots) == 4 and all(bots.values()) and len(set(bots.values())) == 4, str(bots))
    acc = infos(a)
    good = [u for u in bots if u in acc and acc[u][0] >= 2000000 and 1 <= acc[u][1] <= 60]
    check("bots have account-style PLAYER_INFO (level / clan)", len(good) == 4, str({u: acc.get(u) for u in bots}))
    skins = {u: kn_proto.skins_of(a, u) for u in bots}
    check("SKINS before each bot SPAWN", all(v is not None for v in skins.values()), str(skins))

    # 3. they fight
    a.packets.clear()
    a.read(timeout=25.0)
    kills = [struct.unpack("<ii", p[:8]) for t, p in a.packets if t == KILL]
    bot_kills = [k for k in kills if k[1] in bots]
    check("bots kill (KILL with a bot killer)", len(bot_kills) >= 1, f"{len(kills)} kills, {len(bot_kills)} by bots")
    boards = [p for t, p in a.packets if t == LEADERBOARD]
    rows = set()
    for p in boards:
        n = p[0]
        for i in range(n):
            rows.add(struct.unpack("<i", p[1 + i * 16: 5 + i * 16])[0])
    check("bots on the leaderboard", set(bots) <= rows, f"rows {sorted(rows)}")
    states = {struct.unpack("<i", p[:4])[0] for t, p in a.packets if t == STATE}
    check("bot states streamed", len(set(bots) & states) >= 3, f"{len(set(bots) & states)} of 4")

    # 4. a late joiner
    b, me_b = join(PORTS[0], "Late")
    b.read(timeout=2.0)
    late = bot_spawns(b, me_b["uid"] if me_b else -1)
    late_bots = {u for u in late if u in bots}
    check("late joiner gets the bots' SPAWN + PLAYER_INFO", late_bots == set(bots) and set(bots) <= set(infos(b)), f"{late_bots} / {set(infos(b))}")

    # 5. rcon commands
    rc, out = rcon("bot_list")
    check("rcon bot_list", rc == 0 and "Bots (4): nextbot:1:" in out, out.strip()[-200:])
    rc, out = rcon("bot_difficulty", "hard")
    check("rcon bot_difficulty hard", "Bot difficulty set to 1.8 (4 live bots updated; applies to future bot_add)." in out, out.strip()[-200:])
    rc, out = rcon("bot_stop")
    check("rcon bot_stop", "Stopped 4 live bots; applies to future bot_add." in out, out.strip()[-200:])
    rc, out = rcon("bot_kill", "all")
    check("rcon bot_kill all", "Killed" in out and "bot" in out, out.strip()[-200:])
    a.packets.clear()
    rc, out = rcon("bot_kick", "all")
    check("rcon bot_kick all", "Removing 4 bots." in out, out.strip()[-200:])
    a.read(timeout=1.5)
    leaves = {struct.unpack("<i", p[:4])[0] for t, p in a.packets if t == LEAVE}
    check("LEAVE per kicked bot", set(bots) <= leaves, str(leaves))
    info = query(PORTS[0])
    check("info query after kick: bots 0, players 2", info.get("bots") == 0 and info.get("players") == 2, str(info.get("bots")))
    a.close()
    b.close()

    # 6. quota
    start_server(1, "bot_quota 3\nbot_difficulty easy\n")
    info = wait_for(lambda: query(PORTS[1]), 10) or {}
    time.sleep(1.0)
    info = query(PORTS[1]) if info else {}
    check("bot_quota 3 keeps the empty lobby empty", info.get("bots") == 0, str(info.get("bots")))
    c, _ = join(PORTS[1], "Human")
    info = wait_for(lambda: query(PORTS[1]) if query(PORTS[1]).get("bots") == 2 else None, 5) or query(PORTS[1])
    check("a human takes a bot's slot", info.get("bots") == 2 and info.get("players") == 1, str({k: info.get(k) for k in ("bots", "players")}))
    c.close()


try:
    main()
except Exception as e:  # noqa: BLE001
    check("exception", False, repr(e))
finally:
    for p in procs:
        p.terminate()
    for p in procs:
        try:
            p.wait(5)
        except subprocess.TimeoutExpired:
            p.kill()
    if not all(results):
        for i in range(2):
            path = os.path.join(WORK, f"s{i}.log")
            if os.path.exists(path):
                print(f"--- s{i}.log (tail)")
                print(open(path).read()[-3000:])
print(f"{sum(results)}/{len(results)} passed", flush=True)
sys.exit(0 if results and all(results) else 1)
