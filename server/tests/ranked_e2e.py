#!/usr/bin/env python3
"""Krunker v10.0.2 ranked end to end (doc/v10/ranked-impl.md, doc/backend.md 8.6): the solo queue, a
4v4 comp match on a real server_bin, the comp lobby rules and the TrueSkill profiles.

  python3 -I server/tests/ranked_e2e.py BUILD_DIR      (run from the repository root)

Creates its own PostgreSQL database (sudo -n -u postgres createdb; KN_TEST_DB to reuse one),
migrates it, starts one coordinator (kn_brain, ranked min level 0) and one server_bin (ports from
kn_proto.free_ports()), registers 8 accounts, then:
  1. all 8 queue /mm/ranked/queue (solo, region na) and long-poll until MATCHED
  2. 8 protocol test clients (kn_proto, v7 HELLO) JOIN the match with their tickets:
     "Joined Ranked Match..." (ranked.join); INIT plays Hardpoint (the comp host's overDat
     modes [2]); NET_PACKET_COMP_LOBBY carries every roster entry on its queue team; a stranger
     is refused ("Game in Progress")
  3. comp lobby rules: ENT before a class -> "Please select a Class to Spawn", no SPAWN;
     Rocketeer (not in config.rankedClasses: classLim7 0) refused, Triggerman taken; a team mate
     asking for Triggerman too is refused (classLim0 1 per team); "t" -> "Can't switch while
     in-game"; spectate -> "Spectating Disabled in Ranked" and a spawn on the roster team
  4. a player quits mid-match: needAll ends the round, ranked-result -> the other team wins:
     winners' MMR up, the abandoner's down + COOLDOWN + 1 abandon (/mm/ranked/profile),
     players.player_elo mirrors the MMR, the match history has it
  5. every roster player had a lobby team (no "Please join a Team to Spawn" for anyone)
  6. a ranked queue's notice ("<name> queued for ranked", v10 server.message.queued, the ranked
     green #5ce05a) goes to every player: a player in a game gets it as NET_CHAT_RANKED (4), and
     a signed-in social socket with no game gets the "ranked:queued" event (eb) on the brain
Prints PASS/FAIL lines; exit code 0 only when everything passed.
"""
import asyncio
import base64
import hashlib
import json
import os
import re
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
DB = os.environ.get("KN_TEST_DB", f"kn_rk_e2e_{os.getpid()}")
BRAIN_PORT, GAME_PORT, GAME2_PORT = kn_proto.free_ports(3)
WORK = tempfile.mkdtemp(prefix="kn-rk-e2e-", dir=os.path.expanduser("~/.cache"))
CHAT, TEAM, COMP_SET, COMP_LOBBY = 9, 42, 184, 185
CHAT_SEND, EXT = 8, 254  # NET_PACKET_EXT: u16 LE subtype + body
EXT_MARK_POSITION = 0x0900
MINOR_V7 = 3
MINOR_EXT = 4  # HELLO 1.4: the EXT envelope (the "mpos" ping), and the roster's "gsc", names push and "vf"
EXT_RANKED_MATCH = 0x8050  # NET_EXT_KN_RANKED_MATCH: the lobby's ranked flag after each INIT
EXT_LRMR = 0x0601  # NET_EXT_LATEST_RANKED_MMR_CHANGE: the rating change (lrmr)
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


def http(path, **q):
    url = f"http://127.0.0.1:{BRAIN_PORT}{path}" + ("?" + urllib.parse.urlencode(q) if q else "")
    try:
        with urllib.request.urlopen(url, timeout=5) as r:
            return r.status, json.loads(r.read() or b"null")
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b"null")


def read_file(path):
    try:
        with open(path) as f:
            return f.read()
    except OSError:
        return ""


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
    out = []
    for t, p in conn.packets:
        if t == CHAT and len(p) >= 4 + 1 + 32 + 288:
            out.append(kn_proto.uncstr(p[37:37 + 288]))
    return out


# NetCompLobbyPacket: op B + entry (uid i, a B, rej B, t b, c b, i i, l i, v B, f h, n 32s, cl 8s)
ENTRY = "<iBBbbiiBh32s8s"


# NET_EXT_GAME_STATE (0x0901, "gsc" [state]) and NET_EXT_PLAYER_NAME (0x0203, [uid, name[32]]) in
# the conn's packet log (kn_proto.Conn.packets); GameState: INITIALIZING 0 .. ENDED 5 (shared.h).
EXT = 254


def ext_packets(conn, subtype):
    want = struct.pack("<H", subtype)
    return [p for t, p in conn.packets if t == EXT and p[:2] == want]


def gsc_states(conn):
    return [p[2] for p in ext_packets(conn, 0x0901) if len(p) >= 3]


def rename_phases(conn):
    # [(last gsc state seen before it or None, uid, name)] in arrival order
    out, state = [], None
    for t, p in conn.packets:
        if t == EXT and p[:2] == struct.pack("<H", 0x0901) and len(p) >= 3:
            state = p[2]
        elif t == EXT and p[:2] == struct.pack("<H", 0x0203) and len(p) >= 2 + 36:
            uid = struct.unpack("<i", p[2:6])[0]
            out.append((state, uid, p[6:38].split(b"\0")[0].decode("utf-8", "replace")))
    return out


def spawn_names(conn):
    # [(last gsc state seen before the spawn or None, uid, name)]: NetSpawnPacket names as the conn got them
    out, state = [], None
    for t, p in conn.packets:
        if t == EXT and p[:2] == struct.pack("<H", 0x0901) and len(p) >= 3:
            state = p[2]
        elif t == kn_proto.SPAWN:
            info = kn_proto.spawn_info(p)
            if info:
                out.append((state, info["uid"], info["name"]))
    return out


# NetVoteForfeitPacket (0x0902, "vf"): active, opened, team, reserved, votes, needed, initiator[32]
VF = "<BBBBii32s"


def vf_packets(conn):
    out = []
    for t, p in conn.packets:
        if t == EXT and p[:2] == struct.pack("<H", 0x0902) and len(p) >= 2 + struct.calcsize(VF):
            a, o, team, _, votes, needed, init = struct.unpack(VF, p[2:2 + struct.calcsize(VF)])
            out.append({"active": a, "opened": o, "team": team, "votes": votes, "needed": needed,
                        "initiator": kn_proto.uncstr(init)})
    return out


def say(conn, text):
    conn.send(8, kn_proto.cstr(text, 288))  # NET_PACKET_CHAT_SEND


def gsc_wait(conn, state, timeout):
    end = time.time() + timeout
    while time.time() < end:
        if state in gsc_states(conn):
            return True
        conn.read(timeout=0.5)
    return state in gsc_states(conn)


def comp_table(conn):
    table = {}
    for t, p in conn.packets:
        if t != COMP_LOBBY or len(p) != 1 + struct.calcsize(ENTRY):
            continue
        op = p[0]
        v = struct.unpack(ENTRY, p[1:])
        if op == 0:
            table = {}
        elif op == 1:
            table[v[0]] = {"a": v[1], "t": v[3], "c": v[4], "i": v[5], "n": kn_proto.uncstr(v[9])}
        elif op == 2:
            table.pop(v[0], None)
    return table


def ext_bodies(conn, subtype):
    out = []
    for t, p in conn.packets:
        if t == EXT and len(p) >= 2 and struct.unpack("<H", p[:2])[0] == subtype:
            out.append(p[2:])
    return out


def comp_set(conn, key, value):
    conn.send(COMP_SET, struct.pack("<Bi", ord(key), value))


def spawned(conn, me_uid):
    for t, p in conn.packets:
        if t == kn_proto.SPAWN:
            info = kn_proto.spawn_info(p)
            if info and info["uid"] == me_uid:
                return struct.unpack("<iBBii", p[:14])[4]  # NetSpawnPacket.team
    return None


def pw_hash(password, iterations=600000):
    """kn_password_hash's "$pbkdf2-sha256$i=N$salt$key" (services/common/src/crypto.c)"""
    salt = secrets.token_bytes(16)
    key = hashlib.pbkdf2_hmac("sha256", password.encode(), salt, iterations, 32)
    return "$pbkdf2-sha256$i=%d$%s$%s" % (iterations, base64.b64encode(salt).decode(), base64.b64encode(key).decode())


async def social_queue_notice(name, password, trigger):
    """A signed-in social socket (JSON) with no game: logs in, runs trigger() (the queue), then
    collects frames until the ranked:queued event shows up or 12 s pass."""
    import websockets  # noqa: PLC0415 - the v10 page e2e needs it too

    frames = []
    async with websockets.connect("ws://127.0.0.1:%d/social/ws?enc=json" % BRAIN_PORT, open_timeout=10) as ws:
        await ws.send(json.dumps(["a", 1, [name, password, 0]]))  # login (sh_auth action 1)
        end = time.time() + 10
        while time.time() < end and not any(f[0] == "a" for f in frames):
            try:
                frames.append(json.loads(await asyncio.wait_for(ws.recv(), 1.0)))
            except asyncio.TimeoutError:
                pass
        logged_in = any(f[0] == "eb" and isinstance(f[1], dict) and f[1].get("type") == "unread:summary" for f in frames) or any(f[0] == "a" for f in frames)
        trigger()
        end = time.time() + 12
        while time.time() < end:
            if any(f[0] == "eb" and isinstance(f[1], dict) and f[1].get("type") == "ranked:queued" for f in frames):
                break
            try:
                frames.append(json.loads(await asyncio.wait_for(ws.recv(), 1.0)))
            except asyncio.TimeoutError:
                pass
    return logged_in, frames


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
                                   "--set", "brain.region=local", "--set", "brain.ranked.min_level=0",
                                   "--set", "brain.ranked.start_grace_s=60"],
                                  stdout=open(os.path.join(WORK, "brain.log"), "w"), stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL))
    up = bool(wait_for(lambda: http("/mm/status")[0] == 200)) and procs[-1].poll() is None
    check("coordinator up", up, "" if up else log("brain.log")[-300:])
    cfg = os.path.join(WORK, "rk.cfg")
    with open(cfg, "w") as f:
        f.write(f'hostname "Ranked E2E"\nport {GAME_PORT}\nnode_id RKE\nregion local\nmaxplayers 10\ncheats all\n'
                f"coordinator_url ws://127.0.0.1:{BRAIN_PORT}/mm/internal/ws\n"
                f"coordinator_http_url http://127.0.0.1:{BRAIN_PORT}\nmapcycle Burg\nmode ffa\njoin_timeout 4\n")
    procs.append(subprocess.Popen([f"{BUILD}/server_bin", "--headless", "--cfg", cfg, "--config", os.path.join(WORK, "none.json")],
                                  stdout=open(os.path.join(WORK, "game.log"), "w"), stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL))
    check("server linked to the coordinator", bool(wait_for(lambda: "welcomed" in log("game.log"))))

    # 1. eight accounts queue (solo, v10) until MATCHED
    tokens = [secrets.token_hex(16) for _ in range(8)]
    names = [f"rk_p{i}" for i in range(8)]
    for name, tok in zip(names, tokens):
        psql(f"SELECT kn_register('{name}', 'x', '127.0.0.1', '', 0, 0, '{tok}', 3600, 'e2e')")
    ids = {n: int(psql(f"SELECT id FROM accounts WHERE username = '{n}'")) for n in names}
    keys = [http("/mm/ranked/queue", token=t, regions="na")[1] for t in tokens]
    check("8 queue entries", all(k.get("payload", {}).get("clientKey") for k in keys), json.dumps([k.get("payload", {}).get("status") for k in keys]))
    keys = [k["payload"]["clientKey"] for k in keys if k.get("payload", {}).get("clientKey")]

    def all_matched():
        states = [http("/mm/ranked/poll", clientKey=k)[1] for k in keys]
        return states if len(states) == 8 and all(s and s["payload"]["status"] == "MATCHED" for s in states) else None
    states = wait_for(all_matched, timeout=30)
    check("all 8 matched into one game", bool(states) and len({s["payload"]["assignment"]["gameId"] for s in states}) == 1)
    if not states:
        return
    states = [{"status": {"c": dict(s["payload"]["assignment"])}} for s in states]  # the shape below: port, clientId

    # 2. the test clients join with their tickets
    conns = []
    for i, s in enumerate(states):
        if i == 7:  # the lobby still has a free seat: a stranger is refused by the roster rule
            stranger = kn_proto.Conn("127.0.0.1", GAME_PORT)
            stranger.send(1, bytes([1, MINOR_EXT, 0]))
            stranger.join(name="intruder")
            err = stranger.read(until=kn_proto.ERROR, timeout=8)
            check("a stranger is refused: Game in Progress", bool(err) and "Game in Progress" in kn_proto.error_info(err)[0],
                  kn_proto.error_info(err)[0] if err else "no ERROR")
            stranger.close()
        c = kn_proto.Conn("127.0.0.1", int(s["status"]["c"]["port"]))
        c.send(1, bytes([1, MINOR_EXT, 2]))  # HELLO 1.4 (v7 lobby packets + the EXT envelope)
        c.join(client_key=s["status"]["c"]["clientId"], session=tokens[i])
        conns.append(c)
    inits = [c.read(until=kn_proto.INIT, timeout=10) for c in conns]
    for c in conns:
        c.read(timeout=1.0)
    flags = [[b[0] for b in ext_bodies(c, EXT_RANKED_MATCH) if len(b) >= 1] for c in conns]
    check("every roster player is told the lobby is ranked after its INIT (EXT 0x8050 ranked=1)", all(f == [1] for f in flags), str(flags))
    modes = {struct.unpack("<BBBiiHi", p[:struct.calcsize("<BBBiiHi")])[4] for p in inits if p}
    check("INIT: Hardpoint (comp overDat modes [2])", modes == {2}, str(modes))
    uids = [struct.unpack("<BBBiiHi", p[:struct.calcsize("<BBBiiHi")])[6] if p else -1 for p in inits]
    check("ranked.join notice", all(any("Joined Ranked Match" in t for t in chats(c)) for c in conns),
          str(chats(conns[0])[-3:]))
    check("gsc: the comp warmup (WARMUP 2) reaches the roster", all(gsc_wait(c, 2, timeout=5) for c in conns),
          str([gsc_states(c) for c in conns][:2]))
    table = wait_for(lambda: (lambda t: t if len(t) == 8 else None)(comp_table(conns[0])), timeout=5) or comp_table(conns[0])
    check("comp lobby: 8 entries with account names", len(table) == 8 and sorted(e["n"] for e in table.values()) == sorted(names),
          json.dumps(table)[:300])
    teams = {e["n"]: e["t"] for e in table.values()}
    check("roster teams 4 + 4", sorted(teams.values()) == [1, 1, 1, 1, 2, 2, 2, 2], json.dumps(teams))
    # 3. comp lobby rules
    a = conns[2]
    a.ent(class_index=0)
    a.read(timeout=1.5)
    check("ENT without a lobby class: 'Please select a Class to Spawn', no spawn",
          any("Please select a Class to Spawn" in t for t in chats(a)) and spawned(a, uids[2]) is None, str(chats(a)[-2:]))
    comp_set(a, "c", 7)  # Rocketeer: classLim7 0
    a.read(timeout=0.8)
    check("Rocketeer refused (not a ranked class)", comp_table(a).get(uids[2], {}).get("c") == -1)
    comp_set(a, "c", 0)
    a.read(timeout=0.8)
    check("Triggerman taken", comp_table(a).get(uids[2], {}).get("c") == 0)
    mate = next(i for i in range(8) if i != 2 and teams.get(names[i]) == teams.get(names[2]))
    comp_set(conns[mate], "c", 0)
    conns[mate].read(timeout=0.8)
    check("a team mate cannot take Triggerman too (classLim0 1 per team)", comp_table(conns[mate]).get(uids[mate], {}).get("c") == -1)
    comp_set(a, "t", 3 - teams[names[2]])
    a.read(timeout=0.8)
    check("team switch refused in ranked", any("Can't switch while in-game" in t for t in chats(a)) and
          comp_table(a).get(uids[2], {}).get("t") == teams[names[2]])
    a.ent(class_index=0, spectate=1)
    a.read(timeout=1.5)
    team = spawned(a, uids[2])
    check("spectating disabled; spawned on the roster team", any("Spectating Disabled in Ranked" in t for t in chats(a)) and team == teams[names[2]],
          f"team {team}")
    for i, c in enumerate(conns):
        if i != 2:
            comp_set(c, "c", [0, 1, 2, 3, 4, 5, 6, 8][i])
            c.ent(class_index=0)
    for c in conns:
        c.read(timeout=1.0)
    no_team_refusal = not any("Please join a Team" in t for c in conns for t in chats(c))
    check("every roster player has a lobby team: no 'Please join a Team to Spawn'", no_team_refusal and len(table) == 8,
          json.dumps([t for c in conns for t in chats(c) if "Please join a Team" in t])[:200])

    # 3b. a ping (v10 markPosition, NET_EXT_MARK_POSITION): every other roster connection gets addPingIcon
    # (key = the pinging connection's id, the hit position); the pinger gets none
    conns[2].send(EXT, struct.pack("<H", EXT_MARK_POSITION) + struct.pack("<3f", 12.5, 2.0, -7.25))
    for c in conns:
        c.read(timeout=0.8)

    def ping_of(conn):
        for t, p in conn.packets:
            if t == EXT and len(p) == 2 + 60 and struct.unpack("<H", p[:2])[0] == EXT_MARK_POSITION:
                return kn_proto.uncstr(p[2:50]), struct.unpack("<3f", p[50:62])
        return None

    pings = [ping_of(c) for c in conns]
    check("a ping reaches the other roster players (addPingIcon, the pinger's id as key)",
          pings[0] is not None and pings[0][0].isdigit() and pings[0][1] == (12.5, 2.0, -7.25) and pings[7] == pings[0],
          str(pings[0]))
    check("the pinger gets no marker of its own", pings[2] is None)

    # 3c. a scoring player: /score (cheats all: the host's dev command) gives conns[0] points for the round
    conns[0].send(CHAT_SEND, kn_proto.cstr("/score 5000", 288))
    conns[0].read(timeout=0.8)

    # v10 setGameState(IN_PROGRESS) when the warmup timers drain, then the names push (G9, G6)
    check("gsc: IN_PROGRESS (3) after the warmup", gsc_wait(conns[0], 3, timeout=45),
          str(gsc_states(conns[0])) + " spawned " + str([spawned(c, uids[i]) for i, c in enumerate(conns)]))
    phases = rename_phases(conns[0])
    warm = spawn_names(conns[0])
    masked = [n for st, uid, n in warm if st == 2 and uid != uids[0]]
    check("names masked as Player_<n> for the others during WARMUP", bool(masked) and all(n.startswith("Player_") for n in masked),
          str(masked[:4]))
    own = [n for st, uid, n in warm if st == 2 and uid == uids[0]]
    check("own name stays real during WARMUP", bool(own) and own[-1] == names[0], str(own[-1:]))
    real = {n for st, uid, n in phases if st == 3}
    check("real names pushed at IN_PROGRESS", set(names) <= real, str(sorted(real)))

    # 3b. the team forfeit vote (G8, v10 voteToForfeit): /forfeit (alias of /ff) opens team 1's vote, only
    # team 1 gets its "vf"; a rival's /ff is refused; a teammate's vote counts "Vote Submitted - n/needed";
    # a second vote from the same player is refused. Passing and expiry: the unit test (forfeit_test),
    # since a passed vote would end the round this match still needs for the abandon check.
    team_of = {i: teams[names[i]] for i in range(8)}
    t1 = [i for i in range(8) if team_of[i] == 1]
    t2 = [i for i in range(8) if team_of[i] == 2]
    starter, mate, rival = t1[0], t1[1], t2[0]
    say(conns[starter], "/forfeit")
    for c in conns:
        c.read(timeout=0.6)
    opened = [vf for i in range(8) for vf in vf_packets(conns[i]) if vf["active"] and vf["opened"]]
    needed = opened[0]["needed"] if opened else -1
    check("forfeit: /forfeit opens team 1's vote, every team 1 player gets its vf",
          all(any(v["active"] and v["opened"] for v in vf_packets(conns[i])) for i in t1) and opened and opened[0]["team"] == 1,
          json.dumps(opened)[:200])
    check("forfeit: vf is team only (team 2 gets nothing)", not any(vf_packets(conns[i]) for i in t2),
          str([len(vf_packets(conns[i])) for i in t2]))
    check("forfeit: the initiator is named in the vf", bool(opened) and opened[0]["initiator"] == names[starter],
          str(opened[:1]))
    say(conns[rival], "/ff")
    say(conns[mate], "/ff")
    for c in conns:
        c.read(timeout=0.6)
    check("forfeit: a rival's /ff is refused while team 1 votes",
          any("already running for the other team" in t for t in chats(conns[rival])), str(chats(conns[rival])[-1:]))
    submitted = [re.search(r"Vote Submitted - (\d+)/(\d+)", t) for t in chats(conns[mate])]
    submitted = [m for m in submitted if m]
    check("forfeit: a teammate's /ff counts (Vote Submitted - 2/%d)" % needed,
          bool(submitted) and submitted[-1].group(1) == "2" and submitted[-1].group(2) == str(needed),
          str([m.group(0) for m in submitted]))
    say(conns[mate], "/ff")
    for c in conns:
        c.read(timeout=0.6)
    check("forfeit: a second vote from the same player is refused",
          any("already voted" in t for t in chats(conns[mate])), str(chats(conns[mate])[-1:]))
    check("forfeit: the round is still running (no end yet)", 4 not in gsc_states(conns[0]), str(gsc_states(conns[0])))

    # 4. a player abandons: the round ends, the other team wins
    quitter = 5
    conns[quitter].close()
    ended = wait_for(lambda: "[ranked] match" in log("game.log") and " over: winner team" in log("game.log"), timeout=20)
    check("abandon ends the match", bool(ended), "" if ended else log("game.log")[-400:])
    conns[0].read(timeout=1.5)
    check("gsc: POST_MATCH (4) when the round ends", 4 in gsc_states(conns[0]), str(gsc_states(conns[0])))
    prof = lambda t: http("/mm/ranked/profile", token=t, region="na")[1].get("profile", {})
    rated = wait_for(lambda: all(prof(t).get("total_matches") == 1 for t in tokens), timeout=15)
    check("every roster player rated once", bool(rated))
    quit_team = teams[names[quitter]]
    for i, t in enumerate(tokens):
        r = prof(t)
        code, h = http("/mm/ranked/history", token=t, region="na")
        m = (h.get("matchHistory") or {}).get("matches", [{}])[0] if code == 200 else {}
        if i == quitter:
            q = http("/mm/ranked/queue", token=t, regions="na")[1]
            check("abandoner: abandons 1, MMR down, COOLDOWN", r.get("abandons") == 1 and m.get("mmr_change", 0) < 0 and m.get("abandoned") is True
                  and q.get("payload", {}).get("code") == "COOLDOWN", json.dumps(r)[:300])
        elif teams[names[i]] != quit_team:
            check(f"winner {names[i]}: MMR up, a win", r.get("wins") == 1 and m.get("mmr_change", 0) > 0 and m.get("victory") == 1, json.dumps(m)[:300])
            elo = psql(f"SELECT player_elo FROM players WHERE player_id = {ids[names[i]]}")
            check(f"players.player_elo mirrors the MMR ({names[i]})", elo == str(r.get("mmr")), f"{elo} vs {r.get('mmr')}")
        else:
            check(f"loser {names[i]}: MMR down, a loss", r.get("losses") == 1 and m.get("mmr_change", 0) < 0, json.dumps(m)[:300])
    scored = int(psql(f"SELECT coalesce(max(score), -1) FROM match_results WHERE account_id = {ids[names[0]]}") or -1)
    check("the score reaches the account's match result (ranked: score, no games or kills)", scored >= 5000, f"score {scored}")
    conns[0].read(timeout=1.5)
    chat = [t for t in chats(conns[0]) if t.startswith("Ranked: ")]
    check("the rating chat (v10 lrmr): games remaining until ranked", any("games remaining until ranked" in t for t in chat), str(chat[-1:]))
    for c in conns:
        c.close()

    # 6. the queue notice: a player in a game (the public lobby, once the ranked match is over and
    # everyone left) queues; the brain's "ranked-notice" reaches its game's chat in NET_CHAT_RANKED
    NOTICE_NAME = "rk_queue"
    notice_token = secrets.token_hex(16)
    psql(f"SELECT kn_register('{NOTICE_NAME}', 'x', '127.0.0.1', '', 0, 0, '{notice_token}', 3600, 'e2e')")
    back_public = wait_for(lambda: "back to the public rotation" in log("game.log"), timeout=20)
    check("the ranked lobby went back to the public rotation", bool(back_public))
    watcher = kn_proto.Conn("127.0.0.1", GAME_PORT)
    watcher.send(1, bytes([1, MINOR_V7, 0]))
    watcher.join(session=notice_token)
    watcher.read(until=kn_proto.INIT, timeout=10)
    watcher.read(timeout=1.5)  # READY (the session settles): the brain sees the player in this game
    queued = http("/mm/ranked/queue", token=notice_token, regions="na")[1]
    check("the player in a game queues for ranked", queued.get("payload", {}).get("status") == "QUEUED",
          json.dumps(queued)[:200])
    notice = None
    end = time.time() + 10
    while notice is None and time.time() < end:
        watcher.read(timeout=0.5)
        for t, p in watcher.packets:
            if t == CHAT and len(p) >= 4 + 1 + 32 + 288 and p[4] == 4:
                text = kn_proto.uncstr(p[37:37 + 288])
                if text == f"{NOTICE_NAME} queued for ranked":
                    notice = text
    check("the game's chat gets '<name> queued for ranked' in NET_CHAT_RANKED (4)", notice is not None,
          str([(p[4], kn_proto.uncstr(p[37:37 + 288])) for t, p in watcher.packets if t == CHAT][-3:]))
    watcher.close()

    # a second game node (another server of the same brain), with a player in its public lobby
    cfg2 = os.path.join(WORK, "rk2.cfg")
    with open(cfg2, "w") as f:
        f.write(f'hostname "Ranked E2E 2"\nport {GAME2_PORT}\nnode_id RKE2\nregion local\nmaxplayers 10\n'
                f"coordinator_url ws://127.0.0.1:{BRAIN_PORT}/mm/internal/ws\n"
                f"coordinator_http_url http://127.0.0.1:{BRAIN_PORT}\nmapcycle Burg\nmode ffa\njoin_timeout 4\n")
    game2_log = os.path.join(WORK, "game2.log")
    procs.append(subprocess.Popen([f"{BUILD}/server_bin", "--headless", "--cfg", cfg2, "--config", os.path.join(WORK, "none.json")],
                                  stdout=open(game2_log, "w"), stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL))
    check("a second game node links to the brain", bool(wait_for(lambda: "welcomed" in read_file(game2_log))))
    other_token = secrets.token_hex(16)
    psql(f"SELECT kn_register('rk_node2', 'x', '127.0.0.1', '', 0, 0, '{other_token}', 3600, 'e2e')")
    node2 = kn_proto.Conn("127.0.0.1", GAME2_PORT)
    node2.send(1, bytes([1, MINOR_V7, 0]))
    node2.join(session=other_token)
    node2.read(until=kn_proto.INIT, timeout=10)
    node2.read(timeout=1.5)

    # the same notice for a signed-in social socket with no game (every player, not only a game's)
    SOCIAL_NAME, SOCIAL_PW = "rk_social", "rk-social-pw"
    social_token = secrets.token_hex(16)
    psql(f"SELECT kn_register('{SOCIAL_NAME}', '{pw_hash(SOCIAL_PW, 1000)}', '127.0.0.1', '', 0, 0, '{social_token}', 3600, 'e2e')")
    queued_again = []

    def queue_social():  # the socket's own account queues: its line goes to every socket, the queuer's too
        queued_again.append(http("/mm/ranked/queue", token=social_token, regions="na")[1])

    try:
        logged_in, frames = asyncio.run(social_queue_notice(SOCIAL_NAME, SOCIAL_PW, queue_social))
    except Exception as e:  # noqa: BLE001 - reported as a failed check
        logged_in, frames = False, [repr(e)]
    node2_notice = None
    end = time.time() + 10
    while node2_notice is None and time.time() < end:
        node2.read(timeout=0.5)
        for t, p in node2.packets:
            if t == CHAT and len(p) >= 4 + 1 + 32 + 288 and p[4] == 4 and kn_proto.uncstr(p[37:37 + 288]) == f"{SOCIAL_NAME} queued for ranked":
                node2_notice = p
    check("a player on another game node gets '<name> queued for ranked' in NET_CHAT_RANKED (#5ce05a)", node2_notice is not None,
          str([(p[4], kn_proto.uncstr(p[37:37 + 288])) for t, p in node2.packets if t == CHAT][-3:]))
    node2.close()
    eb = [f[1] for f in frames if isinstance(f, list) and f and f[0] == "eb" and isinstance(f[1], dict)
          and f[1].get("type") == "ranked:queued"]
    check("the social socket with no game logs in", bool(logged_in), str(frames[:2])[:200])
    check("a social socket with no game gets '<name> queued for ranked' (ranked:queued, #5ce05a)",
          bool(eb) and eb[0].get("text") == f"{SOCIAL_NAME} queued for ranked" and eb[0].get("color") == "#5ce05a",
          str(eb[:1])[:200] if eb else str([f[:2] if isinstance(f, list) else f for f in frames])[:300])


main()
