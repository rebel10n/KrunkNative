#!/usr/bin/env python3
"""Dedicated server end-to-end test (doc/backend.md 8.7-8.10).

  python3 -I server/tests/dedicated_e2e.py BUILD_DIR      (run from the repository root)

Creates its own PostgreSQL database (sudo -n -u postgres createdb), migrates it, starts one
coordinator (kn_brain) and two server_bin processes from generated server.cfg files, then:
  1. server-info query on both servers (name, map, players, community, tags)
  2. accounts: kn_register x2, give_skin (svc-accounts' kn_add_skin) to account A only
  3. matchmaking: /mm/seek-game with A's session -> ticket -> JOIN on that server
     -> the server learns A's inventory: A's owned hat stays, an unowned one is stripped;
     B (session-only JOIN, no skins) wearing the same hat gets it stripped
  4. password-protected server refuses a wrong password
  5. rcon (local TCP): status, say, kick; `send` redirects a player to the other lobby
  6. SIGTERM on the server: drain, partial stats commit lands in match_results
Prints PASS/FAIL lines; exit code 0 only when everything passed.
"""
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
BRAIN_PORT = 5391
PORTS = [22311, 22312]
RCON_PORT = 22399
WORK = tempfile.mkdtemp(prefix="kn-ds-e2e-")
HAT_OWNED, HAT_UNOWNED = 1890, 1487  # store.skins indices (class_preview.c defaults)
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
    check("coordinator up", bool(wait_for(lambda: http("/mm/status")[0] == 200)))
    start_server(0, f"rcon_password rc-pw\nrcon_port {RCON_PORT}\n")
    start_server(1, "sv_password letmein\n")
    for i in (0, 1):
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
    check("give_skin stored", psql(f"SELECT count FROM inventory WHERE account_id = {alice} AND skin_id = {HAT_OWNED}") == "1")

    # 3. matchmaking: seek-game with Alice's session -> ticket -> JOIN
    status, seek = http(f"/mm/seek-game?hostname=krunknative&region=local&game={game_id}&token={ta}")
    check("seek-game issues a ticket", status == 200 and bool(seek.get("clientId")), json.dumps(seek)[:200])
    a = kn_proto.Conn("127.0.0.1", int(seek.get("port", PORTS[0])))
    a.hello()
    a.join(client_key=seek.get("clientId", ""), session=ta, game_id=seek.get("gameId", ""))
    a.ent(cosmetics=(-1, -1, HAT_OWNED, HAT_UNOWNED, -1, -1, -1))  # skin, hair, hat, back, waist, face, dye
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
    check("server logged the inventory", "ds_alice owns 1 skin(s)" in log("s0.log"))

    b = kn_proto.Conn("127.0.0.1", PORTS[0])
    b.hello()
    b.join(session=tb)
    b.ent(cosmetics=(-1, -1, HAT_OWNED, -1, -1, -1, -1))
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
    other = [g for g in http("/mm/game-list?hostname=krunknative")[1]["games"] if g[0].startswith("E2E1:")]
    rc, out = rcon(RCON_PORT, "send", "ds_bob", other[0][0] if other else "E2E1:x", "Moving you to E2E #1")
    red = b.read(until=kn_proto.REDIRECT, timeout=4)
    check("send -> REDIRECT packet", rc == 0 and red is not None and kn_proto.uncstr(red[:48]).startswith("E2E1:"), out)
    rc, out = rcon(RCON_PORT, "kick", "ds_bob")
    check("rcon kick", rc == 0 and "Kicked ds_bob" in out, out)
    target = "Burg" if query(PORTS[0]).get("map") == "Sandstorm" else "Sandstorm"
    rc, out = rcon(RCON_PORT, "changelevel", target)
    check("rcon changelevel", rc == 0 and wait_for(lambda: query(PORTS[0]).get("map") == target, 5) is True, out)

    # 6. SIGTERM drain -> partial commit for Alice (she played since her spawn)
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


if __name__ == "__main__":
    main()
