#!/usr/bin/env python3
"""Dedicated server end-to-end test (doc/backend.md 8.7-8.10).

  python3 -I server/tests/dedicated_e2e.py BUILD_DIR      (run from the repository root)

Creates its own PostgreSQL database (sudo -n -u postgres createdb), migrates it, starts one
coordinator (kn_brain) and two server_bin processes from generated server.cfg files, then:
  1. server-info query on both servers (name, map, players, community, tags)
  2. accounts: kn_register x2, give_skin (svc-accounts' kn_add_skin) to account A only
  3. matchmaking: /mm/seek-game with A's session -> ticket -> JOIN on that server
     -> the server learns A's inventory: A's owned hat stays, an unowned one is stripped;
     B (session-only JOIN, no skins) wearing the same hat gets it stripped; the same for weapon
     and melee skins (V6 ENT -> NET_PACKET_SKINS before each SPAWN, seen by the other player too);
     the V7 ENT lagComp (ent[11] slider) per player, 0 clamped to 0.1 (rcon `lagcomp`)
  4. password-protected server refuses a wrong password
  5. rcon (local TCP): status, say, kick; `send` redirects a player to the other lobby
  6. online_mode server linked to the coordinator: guests are never matched to it (nor to
     the password one), a guest asking for it by id is moved / refused (AccountRequired);
     a signed-in player is matched to it and spawns as the account (ticket JOIN, then the
     client's hasJoined "hj1:" JOIN like client/src/net/online_mode.c)
  7. SIGTERM on the server: drain, partial stats commit lands in match_results
Prints PASS/FAIL lines; exit code 0 only when everything passed.
"""
import hashlib
import json
import os
import secrets
import signal
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request

sys.path.insert(0, os.path.join(os.getcwd(), "server/tests"))
import kn_proto  # noqa: E402

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
DB = os.environ.get("KN_TEST_DB", f"kn_ds_e2e_{os.getpid()}")
# ephemeral ports: with fixed ones (5391, 22311-22313, 22399) concurrent runs reached each other's
# coordinator, servers and rcon (rcon status/players/lagcomp/send/kick and guest seek-game failed)
BRAIN_PORT, RCON_PORT, *PORTS = kn_proto.free_ports(5)
WORK = tempfile.mkdtemp(prefix="kn-ds-e2e-")
HAT_OWNED, HAT_UNOWNED = 1890, 1487  # store.skins indices (class_preview.c defaults)
# weapon skins: array positions in assets/skins.json (not the per-type "id"): Lykos (AK, the Triggerman
# primary), Arctic AK, and the melee items Soul Fang / Dusk Fang
AK_OWNED, AK_UNOWNED, MELEE_OWNED, MELEE_UNOWNED = 1877, 12, 447, 448
CAROL = "ds_carol_with_a_very_long_name_x"  # 32 chars, accounts.username maximum
results = []
procs = []


def check(name, ok, detail=""):
    results.append(ok)
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


def http(path, method="GET", body=None):
    req = urllib.request.Request(f"http://127.0.0.1:{BRAIN_PORT}{path}", method=method,
                                 data=json.dumps(body).encode() if body is not None else None,
                                 headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=5) as r:
            return r.status, json.loads(r.read() or b"null")
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b"null")


def query(port):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.settimeout(1.0)
        s.sendto(b"\xff\xff\xff\xffKNINFO\x00e2e", ("127.0.0.1", port))
        data, _ = s.recvfrom(2048)
    return json.loads(data[11:])


def rcon(port, *cmd, password="rc-pw"):
    out = subprocess.run([f"{BUILD}/server_bin", "--rcon", f"127.0.0.1:{port}", "--rcon-password", password, *cmd],
                         capture_output=True, text=True, timeout=10)
    return out.returncode, out.stdout + out.stderr


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


def start_server(i, extra):
    cfg = os.path.join(WORK, f"s{i}.cfg")
    with open(cfg, "w") as f:
        f.write(f'hostname "E2E #{i}"\nport {PORTS[i]}\nnode_id E2E{i}\nregion local\nmaxplayers 6\n'
                f"coordinator_url ws://127.0.0.1:{BRAIN_PORT}/mm/internal/ws\n"
                f"coordinator_http_url http://127.0.0.1:{BRAIN_PORT}\nmapcycle Burg,Sandstorm\nmode ffa\n"
                f"community \"E2E Network\"\njoin_timeout 2\n{extra}")
    out = open(os.path.join(WORK, f"s{i}.log"), "w")
    p = subprocess.Popen([f"{BUILD}/server_bin", "--headless", "--cfg", cfg, "--config", os.path.join(WORK, "none.json")],
                         stdout=out, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
    procs.append(p)
    return p


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
        print(f"logs: {WORK}")
    ok = all(results) and results
    print(f"{'ALL PASSED' if ok else 'FAILED'} ({sum(results)}/{len(results)})")
    sys.exit(0 if ok else 1)


def run():
    subprocess.run([f"{BUILD}/services/common/kn_migrate", "--set", f"database.name={DB}"], check=True,
                   stdout=open(os.path.join(WORK, "migrate.log"), "w"), stderr=subprocess.STDOUT)
    brain_log = open(os.path.join(WORK, "brain.log"), "w")
    procs.append(subprocess.Popen([f"{BUILD}/services/brain/kn_brain", "--set", f"database.name={DB}", "--set", f"brain.port={BRAIN_PORT}",
                                   "--set", "brain.region=local", "--set", "brain.federation=true"],
                                  stdout=brain_log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL))
    # our own kn_brain must be the one answering (it exits when the port is taken)
    up = bool(wait_for(lambda: http("/mm/status")[0] == 200)) and procs[-1].poll() is None
    check("coordinator up", up, "" if up else log("brain.log")[-300:])
    start_server(0, f"rcon_password rc-pw\nrcon_port {RCON_PORT}\n")
    start_server(1, "sv_password letmein\n")
    start_server(2, f"online_mode 1\nauth_url http://127.0.0.1:{BRAIN_PORT}\n")
    for i in (0, 1, 2):
        check(f"server {i} linked to the coordinator", bool(wait_for(lambda i=i: "welcomed" in log(f"s{i}.log"))))

    # 1. server-info query
    info = wait_for(lambda: query(PORTS[0]))
    check("info query answers", bool(info) and info.get("name") == "E2E #0" and info.get("map") in ("Burg", "Sandstorm"), json.dumps(info))
    check("info query: community + tags", bool(info) and info.get("community") == "E2E Network" and "community" in info.get("tags", []))
    info1 = wait_for(lambda: query(PORTS[1]))
    check("info query: password flag", bool(info1) and info1.get("password") is True and "password" in info1.get("tags", []))
    games = wait_for(lambda: [g for g in http("/mm/game-list?hostname=krunknative")[1]["games"] if g[0].startswith("E2E0:")])
    check("coordinator lists the lobby", bool(games))
    game_id = games[0][0] if games else ""

    # 2. accounts + skins (existing accounts.c ops through SQL helpers)
    ta, tb = secrets.token_hex(16), secrets.token_hex(16)
    psql(f"SELECT kn_register('ds_alice', 'x', '127.0.0.1', '', 0, 0, '{ta}', 3600, 'e2e')")
    psql(f"SELECT kn_register('ds_bob', 'x', '127.0.0.1', '', 0, 0, '{tb}', 3600, 'e2e')")
    alice = int(psql("SELECT id FROM accounts WHERE username = 'ds_alice'"))
    psql(f"SELECT kn_add_skin({alice}, {HAT_OWNED}, 1, 'admin', 'e2e')")
    psql(f"SELECT kn_add_skin({alice}, {AK_OWNED}, 1, 'admin', 'e2e')")
    psql(f"SELECT kn_add_skin({alice}, {MELEE_OWNED}, 1, 'admin', 'e2e')")
    check("give_skin stored", psql(f"SELECT count FROM inventory WHERE account_id = {alice} AND skin_id = {HAT_OWNED}") == "1")

    # 3. matchmaking: seek-game with Alice's session -> ticket -> JOIN
    status, seek = http(f"/mm/seek-game?hostname=krunknative&region=local&game={game_id}&token={ta}")
    check("seek-game issues a ticket", status == 200 and bool(seek.get("clientId")), json.dumps(seek)[:200])
    a = kn_proto.Conn("127.0.0.1", int(seek.get("port", PORTS[0])))
    a.hello()
    a.join(client_key=seek.get("clientId", ""), session=ta, game_id=seek.get("gameId", ""))
    a.ent(cosmetics=(-1, -1, HAT_OWNED, HAT_UNOWNED, -1, -1, -1),  # skin, hair, hat, back, waist, face, dye
          skins=(AK_OWNED, AK_UNOWNED, MELEE_OWNED),  # ent[2] primary / secondary, ent[9] melee
          lag_comp=0.3)  # ent[11] Settings > Network "Lag Compensation" slider (V7 ENT)
    sp = None
    while True:
        payload = a.read(until=kn_proto.SPAWN, timeout=6)
        if payload is None:
            break
        sp = kn_proto.spawn_info(payload)
        if sp and sp["is_you"]:
            break
    check("ticket join spawns as the account", bool(sp) and sp["name"] == "ds_alice", str(sp))
    check("owned hat kept", bool(sp) and sp["cosmetics"][2] == HAT_OWNED, str(sp and sp["cosmetics"]))
    check("unowned back skin stripped", bool(sp) and sp["cosmetics"][3] == -1, str(sp and sp["cosmetics"]))
    check("server logged the inventory", "ds_alice owns 3 skin(s)" in log("s0.log"))
    inits = [p for t, p in a.packets if t == kn_proto.INIT and len(p) >= 3]
    check("INIT announces protocol 1.2 (clients send V5/V6 ENTs from 1.1, V7 from 1.2)", bool(inits) and inits[0][0] == 1 and inits[0][1] >= 2,
          str(inits and list(inits[0][:3])))
    alice_skins = kn_proto.skins_of(a, sp["uid"]) if sp else None
    check("SKINS before Alice's SPAWN: owned weapon + melee skins kept, unowned one stripped",
          alice_skins == [AK_OWNED, -1, MELEE_OWNED], str(alice_skins))

    b = kn_proto.Conn("127.0.0.1", PORTS[0])
    b.hello()
    b.join(session=tb)
    b.ent(cosmetics=(-1, -1, HAT_OWNED, -1, -1, -1, -1), skins=(AK_OWNED, -1, MELEE_UNOWNED), lag_comp=0.0)  # slider at 0
    spb = None
    while True:
        payload = b.read(until=kn_proto.SPAWN, timeout=6)
        if payload is None:
            break
        spb = kn_proto.spawn_info(payload)
        if spb and spb["is_you"]:
            break
    check("session join (no ticket) spawns as the account", bool(spb) and spb["name"] == "ds_bob", str(spb))
    check("Bob cannot wear Alice's hat", bool(spb) and spb["cosmetics"][2] == -1, str(spb and spb["cosmetics"]))
    bob_skins = kn_proto.skins_of(b, spb["uid"]) if spb else None
    check("Bob cannot wear Alice's weapon skin nor an unowned melee", bob_skins == [-1, -1, -1], str(bob_skins))
    seen = kn_proto.skins_of(b, sp["uid"]) if sp else None
    check("Bob sees Alice's weapon skins (SKINS with her existing SPAWN)", seen == [AK_OWNED, -1, MELEE_OWNED], str(seen))
    a.read(timeout=1)
    seen = kn_proto.skins_of(a, spb["uid"]) if spb else None
    check("Alice sees Bob's stripped skins", seen == [-1, -1, -1], str(seen))
    info = wait_for(lambda: query(PORTS[0]) if query(PORTS[0]).get("players") == 2 else None, 3) or query(PORTS[0])
    check("info query counts players", info.get("players") == 2, json.dumps(info))

    # 4. password server
    c = kn_proto.Conn("127.0.0.1", PORTS[1])
    c.hello()
    c.join(password="wrong")
    err = c.read(until=kn_proto.ERROR, timeout=5)
    check("wrong password refused", bool(err) and "password" in kn_proto.error_info(err)[0].lower(), str(err and kn_proto.error_info(err)))
    c.close()
    d = kn_proto.Conn("127.0.0.1", PORTS[1])
    d.hello()
    d.join(password="letmein")
    d.ent()
    check("right password spawns", d.read(until=kn_proto.SPAWN, timeout=6) is not None)
    d.close()

    # 5. rcon + console commands
    rc, out = rcon(RCON_PORT, "status")
    check("rcon status", rc == 0 and "players    2/6" in out and "coordinator linked" in out, out[-300:])
    rc, out = rcon(RCON_PORT, "status", password="nope")
    check("rcon bad password refused", rc != 0 and "bad password" in out)
    rc, out = rcon(RCON_PORT, "say", "hello", "from", "rcon")
    check("rcon say", rc == 0 and "hello from rcon" in out)
    rc, out = rcon(RCON_PORT, "players")
    check("rcon players", rc == 0 and "ds_alice" in out and "ds_bob" in out, out[-300:])
    rc, out = rcon(RCON_PORT, "lagcomp")
    check("ENT V7 lagComp stored per player (0.3 kept, 0 clamped to 0.1 like gameserver.ts applyLagCompFromEnt)",
          rc == 0 and "ds_alice: lagComp 0.30" in out and "ds_bob: lagComp 0.10" in out, out[-400:])
    other = [g for g in http("/mm/game-list?hostname=krunknative")[1]["games"] if g[0].startswith("E2E1:")]
    rc, out = rcon(RCON_PORT, "send", "ds_bob", other[0][0] if other else "E2E1:x", "Moving you to E2E #1")
    red = b.read(until=kn_proto.REDIRECT, timeout=4)
    check("send -> REDIRECT packet", rc == 0 and red is not None and kn_proto.uncstr(red[:48]).startswith("E2E1:"), out)
    rc, out = rcon(RCON_PORT, "kick", "ds_bob")
    check("rcon kick", rc == 0 and "Kicked ds_bob" in out, out)
    target = "Burg" if query(PORTS[0]).get("map") == "Sandstorm" else "Sandstorm"
    rc, out = rcon(RCON_PORT, "changelevel", target)
    check("rcon changelevel", rc == 0 and wait_for(lambda: query(PORTS[0]).get("map") == target, 5) is True, out)

    online_mode_matchmaking()

    # 7. SIGTERM drain -> partial commit for Alice (she played since her spawn)
    time.sleep(1.5)
    procs[1].send_signal(signal.SIGTERM)  # server 0
    try:
        procs[1].wait(20)
    except subprocess.TimeoutExpired:
        pass
    check("SIGTERM drains and exits 0", procs[1].returncode == 0 and "drained" in log("s0.log"), log("s0.log")[-400:])
    rows = wait_for(lambda: int(psql(f"SELECT count(*) FROM match_results WHERE account_id = {alice}")) > 0, 8)
    check("stats committed for the account", bool(rows))
    a.close()
    b.close()


def read_spawn(conn):
    sp = None
    while True:
        payload = conn.read(until=kn_proto.SPAWN, timeout=6)
        if payload is None:
            return sp
        sp = kn_proto.spawn_info(payload)
        if sp and sp["is_you"]:
            return sp


def online_mode_matchmaking():
    # 6. online_mode node (E2E2) in the coordinator pool
    st, status = http("/mm/status")
    check("coordinator knows the online_mode node", st == 200 and status.get("onlineModeNodes") == 1, json.dumps(status))
    online = wait_for(lambda: [g for g in http("/mm/game-list?hostname=krunknative")[1]["games"] if g[0].startswith("E2E2:")])
    online_id = online[0][0] if online else "E2E2:x"
    check("online_mode lobby flagged in the game list", bool(online) and online[0][4].get("onlineMode") is True,
          json.dumps(online)[:300])
    pw = [g for g in http("/mm/game-list?hostname=krunknative")[1]["games"] if g[0].startswith("E2E1:")]
    check("password lobby flagged pr", bool(pw) and pw[0][4].get("pr") is True, json.dumps(pw)[:300])
    picked = []
    for _ in range(3):
        st, seek = http("/mm/seek-game?hostname=krunknative&region=local")
        picked.append(seek.get("gameId", f"HTTP {st}"))
    check("guest seek-game never picks the online_mode or password node",
          all(g.startswith("E2E0:") for g in picked), " ".join(picked))
    st, seek = http(f"/mm/seek-game?hostname=krunknative&region=local&game={online_id}&autoChangeGame=false")
    check("guest asking for the online_mode lobby: AccountRequired", st == 403 and seek.get("error") == "AccountRequired",
          f"{st} {json.dumps(seek)}")
    st, seek = http(f"/mm/seek-game?hostname=krunknative&region=local&game={online_id}&autoChangeGame=true")
    check("guest asking for it with autoChangeGame is moved", st == 200 and seek.get("gameId", "").startswith("E2E0:")
          and seek.get("changeReason") == "game_changed", json.dumps(seek)[:200])

    tc = secrets.token_hex(16)  # a 32-char (maximum) username: the ticket must still fit client_key
    psql(f"SELECT kn_register('{CAROL}', 'x', '127.0.0.1', '', 0, 0, '{tc}', 3600, 'e2e')")
    st, seek = http(f"/mm/seek-game?hostname=krunknative&region=local&game={online_id}&autoChangeGame=false&token={tc}")
    check("signed-in seek-game gets the online_mode lobby", st == 200 and seek.get("gameId") == online_id
          and seek.get("port") == PORTS[2] and 0 < len(seek.get("clientId", "")) <= 351, json.dumps(seek)[:200])
    # what the client does: JOIN (ticket + session) right after HELLO, then on the
    # AUTH_CHALLENGE /auth/join + the "hj1:" JOIN (online_mode.c); the ticket already verifies
    c = kn_proto.Conn("127.0.0.1", PORTS[2])
    c.hello()
    c.join(client_key=seek.get("clientId", ""), session=tc, game_id=seek.get("gameId", ""))
    ch = c.read(until=kn_proto.AUTH_CHALLENGE, timeout=5)
    check("online_mode node sends AUTH_CHALLENGE", ch is not None and ch[33] == 1)
    if ch:
        sn, pk, cn = kn_proto.uncstr(ch[:33]), kn_proto.uncstr(ch[34:99]), secrets.token_hex(16)
        sh = hashlib.sha256(b"kn-join-v1\0" + sn.encode() + b"\0" + pk.encode() + b"\0" + cn.encode()).hexdigest()
        st, j = http("/auth/join", "POST", {"token": tc, "serverHash": sh})
        check("client /auth/join on the coordinator", st == 200 and j.get("ok"), json.dumps(j))
        c.join(client_key="hj1:" + cn, name=CAROL)
    c.ent()
    sp = read_spawn(c)
    check("signed-in player spawns on the online_mode node as the account", bool(sp) and CAROL.startswith(sp["name"]) and len(sp["name"]) >= 16, str(sp))

    # hasJoined only (a finder / direct join of a signed-in client, through the coordinator link)
    td = secrets.token_hex(16)
    psql(f"SELECT kn_register('ds_dave', 'x', '127.0.0.1', '', 0, 0, '{td}', 3600, 'e2e')")
    d = kn_proto.Conn("127.0.0.1", PORTS[2])
    d.hello()
    ch = d.read(until=kn_proto.AUTH_CHALLENGE, timeout=5)
    sn, pk, cn = kn_proto.uncstr(ch[:33]), kn_proto.uncstr(ch[34:99]), secrets.token_hex(16)
    sh = hashlib.sha256(b"kn-join-v1\0" + sn.encode() + b"\0" + pk.encode() + b"\0" + cn.encode()).hexdigest()
    http("/auth/join", "POST", {"token": td, "serverHash": sh})
    d.join(client_key="hj1:" + cn, name="ds_dave")
    d.ent()
    sp = read_spawn(d)
    check("hasJoined through the coordinator spawns as the account", bool(sp) and sp["name"] == "ds_dave", str(sp))

    g = kn_proto.Conn("127.0.0.1", PORTS[2])  # a guest that connects anyway
    g.hello()
    g.join(name="Guest")
    err = g.read(until=kn_proto.ERROR, timeout=8)
    check("guest refused by the online_mode node", err is not None, str(err and kn_proto.error_info(err)))
    for x in (c, d, g):
        x.close()


if __name__ == "__main__":
    main()
