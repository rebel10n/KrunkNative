#!/usr/bin/env python3
"""Krunker v10.0.2 Turf Wars + clan wars end to end (doc/v10/ranked-impl.md) on a real server_bin.

  python3 -I server/tests/turf_e2e.py BUILD_DIR      (run from the repository root)

Creates its own PostgreSQL database (sudo -n -u postgres createdb; KN_TEST_DB to reuse one),
migrates it, starts one coordinator (kn_brain) and one server_bin (ports from
kn_proto.free_ports(), cheats "all" so /kill <name> makes kills), registers 4 accounts in 2 clans
(AAA: a0 a1, BBB: b0) and a clanless c0, then:
  1. public Burg FFA: a0 kills b0 twice, c0 once and a1 (its own clan) once; the round ends ->
     "turf-kills" reaches the brain: a0 3 kills, b0 1 death; nothing for the own-clan kill
     (/api/territorial-wars/status, clan/AAA/players)
  2. a hosted clan war (knClanWar, knClanA AAA, knClanB BBB, TDM): a0 / a1 spawn on team 1, b0 on
     team 2, c0 (no clan) is moved to spectating with the "Clan War: [AAA] vs [BBB]" notice; the
     kills of a hosted game never count for Turf Wars (official public games only)
Prints PASS/FAIL lines; exit code 0 only when everything passed.
"""
import json
import os
import secrets
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import time
import urllib.parse
import urllib.request

sys.path.insert(0, os.path.join(os.getcwd(), "server/tests"))
import kn_proto  # noqa: E402

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
DB = os.environ.get("KN_TEST_DB", f"kn_turf_e2e_{os.getpid()}")
BRAIN_PORT, GAME_PORT = kn_proto.free_ports(2)
WORK = tempfile.mkdtemp(prefix="kn-turf-e2e-", dir=os.path.expanduser("~/.cache"))
CHAT_SEND, CHAT = 8, 9
results = []
procs = []


def check(name, ok, detail=""):
    results.append(bool(ok))
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def cfgdb():
    with open("config/config.json") as f:
        return json.load(f)["database"]


def psql(sql):
    d = cfgdb()
    env = dict(os.environ, PGPASSWORD=d.get("password", ""))
    out = subprocess.run(["psql", "-h", d.get("host", "127.0.0.1"), "-U", d.get("user", "krunknative"), "-d", DB,
                          "-v", "ON_ERROR_STOP=1", "-qAt", "-c", sql], env=env, capture_output=True, text=True)
    if out.returncode:
        raise RuntimeError(out.stderr)
    return out.stdout.strip()


def http(path, body=None, **q):
    url = f"http://127.0.0.1:{BRAIN_PORT}{path}" + ("?" + urllib.parse.urlencode(q) if q else "")
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(url, data=data, headers={"Content-Type": "application/json"} if data else {})
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status, json.loads(r.read() or b"null")
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b"null")


def log(name):
    try:
        with open(os.path.join(WORK, name)) as f:
            return f.read()
    except OSError:
        return ""


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


def chats(conn):
    return [kn_proto.uncstr(p[37:37 + 288]) for t, p in conn.packets if t == CHAT and len(p) >= 4 + 1 + 32 + 288]


def spawn_team(conn, uid):
    for t, p in conn.packets:
        if t == kn_proto.SPAWN and len(p) >= 14:
            v = struct.unpack("<iBBii", p[:14])
            if v[0] == uid:
                return v[4]
    return None


def play(seek, token):
    c = kn_proto.Conn("127.0.0.1", int(seek["port"]))
    c.send(1, bytes([1, 3, 0]))
    c.join(client_key=seek["clientId"], session=token)
    init = c.read(until=kn_proto.INIT, timeout=10)
    uid = struct.unpack("<BBBiiHi", init[:struct.calcsize("<BBBiiHi")])[6] if init else -1
    c.read(timeout=1.0)
    return c, uid


def say(conn, text):
    conn.send(CHAT_SEND, kn_proto.cstr(text, 288))


def main():
    with open(os.path.join(WORK, "none.json"), "w") as f:
        json.dump({"database": cfgdb()}, f)
    created = False
    if not os.environ.get("KN_TEST_DB"):
        subprocess.run(["sudo", "-n", "-u", "postgres", "createdb", "-O", cfgdb().get("user", "krunknative"), DB], check=True)
        created = True
    try:
        run()
    finally:
        for p in procs:
            if p.poll() is None:
                p.send_signal(signal.SIGTERM)
        for p in procs:
            try:
                p.wait(15)
            except subprocess.TimeoutExpired:
                p.kill()
        if created and not os.environ.get("KN_TEST_KEEP"):
            subprocess.run(["sudo", "-n", "-u", "postgres", "dropdb", "--if-exists", DB])
    ok = all(results) and results
    if ok and not os.environ.get("KN_TEST_KEEP"):
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print(f"logs: {WORK}")
    print(f"{'ALL PASSED' if ok else 'FAILED'} ({sum(results)}/{len(results)})")
    sys.exit(0 if ok else 1)


def run():
    subprocess.run([f"{BUILD}/services/common/kn_migrate", "--set", f"database.name={DB}"], check=True,
                   stdout=open(os.path.join(WORK, "migrate.log"), "w"), stderr=subprocess.STDOUT)
    procs.append(subprocess.Popen([f"{BUILD}/services/brain/kn_brain", "--set", f"database.name={DB}", "--set", f"brain.port={BRAIN_PORT}",
                                   "--set", "brain.region=local"],
                                  stdout=open(os.path.join(WORK, "brain.log"), "w"), stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL))
    up = bool(wait_for(lambda: http("/mm/status")[0] == 200)) and procs[-1].poll() is None
    check("coordinator up", up, "" if up else log("brain.log")[-300:])
    cfg = os.path.join(WORK, "turf.cfg")
    with open(cfg, "w") as f:
        f.write(f'hostname "Turf E2E"\nport {GAME_PORT}\nnode_id TRE\nregion local\nmaxplayers 10\ncheats all\n'
                f"coordinator_url ws://127.0.0.1:{BRAIN_PORT}/mm/internal/ws\n"
                f"coordinator_http_url http://127.0.0.1:{BRAIN_PORT}\nmapcycle Burg\nmode ffa\njoin_timeout 4\n")
    game = subprocess.Popen([f"{BUILD}/server_bin", "--headless", "--cfg", cfg, "--config", os.path.join(WORK, "none.json")],
                            stdout=open(os.path.join(WORK, "game.log"), "w"), stderr=subprocess.STDOUT, stdin=subprocess.PIPE, text=True)
    procs.append(game)
    check("server linked to the coordinator", bool(wait_for(lambda: "welcomed" in log("game.log"))))

    names = ["te_a0", "te_a1", "te_b0", "te_c0"]
    tokens = {n: secrets.token_hex(16) for n in names}
    for n in names:
        psql(f"SELECT kn_register('{n}', 'x', '127.0.0.1', '', 0, 0, '{tokens[n]}', 3600, 'e2e')")
    ids = {n: int(psql(f"SELECT id FROM accounts WHERE username = '{n}'")) for n in names}
    for tag, owner, members in (("AAA", "te_a0", ["te_a0", "te_a1"]), ("BBB", "te_b0", ["te_b0"])):
        cid = psql(f"INSERT INTO clans (clan_name, clan_playerid) VALUES ('{tag}', {ids[owner]}) RETURNING clan_id").splitlines()[0]
        for m in members:
            psql(f"INSERT INTO clan_members (account_id, clan_id, rank) VALUES ({ids[m]}, {cid}, {3 if m == owner else 0})")

    # 1. public Burg FFA
    lobby = wait_for(lambda: ([g[0] for g in http("/mm/game-list")[1].get("games", []) if g[0].startswith("TRE:")] + [None])[0])
    check("public lobby listed", bool(lobby))
    conns = {}
    for n in names:
        code, seek = http("/mm/seek-game", token=tokens[n], game=lobby)
        conns[n] = play(seek, tokens[n])
    for n, (c, uid) in conns.items():
        c.ent(class_index=0)
    for c, _ in conns.values():
        c.read(timeout=1.5)
    a0 = conns["te_a0"][0]
    for target in ("te_b0", "te_c0", "te_a1", "te_b0"):
        say(a0, f"/kill {target}")
        a0.read(timeout=3.2)  # the victim's death delay before it may respawn
        conns[target][0].ent(class_index=0)
        for c, _ in conns.values():
            c.read(timeout=0.3)
    game.stdin.write("round end\n")
    game.stdin.flush()
    sent = wait_for(lambda: "[turf] round on map 0" in log("game.log"), timeout=15)
    check("the round end flushes the clan kills", bool(sent), log("game.log")[-400:] if not sent else "")
    status = wait_for(lambda: (lambda d: d if d and d[0]["leaderboard"] else None)(
        [m for m in http("/api/territorial-wars/status")[1].get("data", []) if m["map_id"] == 0]), timeout=10)
    burg = status[0] if status else {}
    check("status: AAA leads Burg with 3 kills (the own-clan kill does not count)",
          burg.get("total_kills_today") == 3 and burg["leaderboard"][0]["clan_name"] == "AAA" and burg["leaderboard"][0]["kills"] == 3,
          json.dumps(burg)[:300])
    code, pl = http("/api/territorial-wars/clan/BBB/players", map_id=0)
    b0 = (pl.get("data", {}).get("players") or [{}])[0]
    check("BBB: b0's 2 deaths by another clan, no kills", b0.get("deaths") == 2 and b0.get("kills") == 0, json.dumps(pl)[:300])
    for c, _ in conns.values():
        c.close()
    time.sleep(1.0)

    # 2. a hosted clan war
    code, host = http("/mm/host-game", body={"config": {"modes": [1], "maps": [0], "maxPlayers": 6, "knClanWar": True, "knClanA": "AAA", "knClanB": "BBB"}},
                      token=tokens["te_a0"])
    check("clan war hosted", code == 200 and host.get("clientId"), str(host)[:200])
    if code != 200:
        return
    war = {"te_a0": play(host, tokens["te_a0"])}
    for n in ("te_a1", "te_b0", "te_c0"):
        code, seek = http("/mm/seek-game", token=tokens[n], game=host["gameId"])
        if code != 200 or "port" not in seek:
            check(f"{n} seeks the clan war", False, f"{code} {str(seek)[:200]}")
            return
        war[n] = play(seek, tokens[n])
    for n, (c, uid) in war.items():
        c.ent(class_index=0)
    for c, _ in war.values():
        c.read(timeout=2.0)
    teams = {n: spawn_team(c, uid) for n, (c, uid) in war.items()}
    check("clan war teams: AAA on team 1, BBB on team 2", teams["te_a0"] == 1 and teams["te_a1"] == 1 and teams["te_b0"] == 2, json.dumps(teams))
    c0, _ = war["te_c0"]
    check("a clanless player spectates", teams["te_c0"] is None and any("Clan War: [AAA] vs [BBB]" in t for t in chats(c0)), str(chats(c0)[-3:]))
    before = psql("SELECT coalesce(sum(kills), 0) FROM turf_kills")
    say(war["te_a0"][0], "/kill te_b0")
    war["te_a0"][0].read(timeout=1.0)
    game.stdin.write("round end\n")
    game.stdin.flush()
    time.sleep(3.0)
    after = psql("SELECT coalesce(sum(kills), 0) FROM turf_kills")
    check("a hosted clan war never counts for Turf Wars", before == after and before == "3", f"{before} -> {after}")
    for c, _ in war.values():
        c.close()


main()
