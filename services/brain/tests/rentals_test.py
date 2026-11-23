#!/usr/bin/env python3
"""Krunker v10.0.7's Rent Dedicated Server on kn_brain (services/brain/rentals.c; migration 0390_rented_servers;
doc/v10/rented-servers.md).

  rentals_test.py <kn_brain> <repo root> [database]
Database: argument, else $KN_MM_TEST_DATABASE, else config/config.json's (or $KN_TEST_CONFIG's) database.name; every run uses its own
schema and an ephemeral port. 77 = skip. KN_TEST_DB_DRIVER=sqlite|mysql runs on that store driver.

Covers: the pool from brain.rentals.pool (the window's rows {ID, REG, TRM, OID, SVN, SID, CONF}); the refusals
(guest, unknown server, no game, bad duration, an offline host agent, not enough KR, the per-account limit,
another player's server, an update of a server not rented); a rent (7,500 KR for 30 days, the ledger row, the
generation, the agent's desired state); extend (14,000 KR, +60 days; the same game keeps the generation, a new one
bumps it); Restart/Update (free, generation + 1); the agent API (Bearer secret, status running keeps the startup
guarantee); the startup guarantee's refund (a rental that never ran within start_timeout_s); the expiry of a
lapsed rental (freed, generation + 1, the agent stops it); the HTTP agent API's auth.
"""
import asyncio
import json
import os
import re
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

try:
    import websockets
except ImportError:  # pragma: no cover
    print("python websockets missing: skip")
    sys.exit(77)

BRAIN, ROOT = sys.argv[1], os.path.abspath(sys.argv[2])
DRIVER = os.environ.get("KN_TEST_DB_DRIVER", "postgres")
DB = sys.argv[3] if len(sys.argv) > 3 else os.environ.get("KN_MM_TEST_DATABASE", "")
CONFIG_DB = {}
try:
    with open(os.environ.get("KN_TEST_CONFIG", os.path.join(ROOT, "config", "config.json"))) as fp:
        CONFIG_DB = json.load(fp).get("database", {})
except (OSError, ValueError):
    pass
if not DB:
    DB = CONFIG_DB.get("name", "")
if not DB and DRIVER == "postgres":
    print("rentals test: no database configured; skipped")
    sys.exit(77)
SCHEMA = "kn_test_rentals_%d" % os.getpid()
SECRET = "kn-rentals-test-%d-xxxxxxxxxxxxxxxx" % os.getpid()
FAILS = []
DBCFG = {}


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what, flush=True)
    if not cond:
        FAILS.append(what)


def bound_port(log_path, proc):
    for _ in range(200):
        if proc.poll() is not None:
            raise RuntimeError("kn_brain exited early (%s)" % log_path)
        with open(log_path) as fp:
            m = re.search(r"listening on http://127\.0\.0\.1:(\d+) \(ws", fp.read().split("== start ")[-1])
        if m:
            return int(m.group(1))
        time.sleep(0.05)
    raise RuntimeError("kn_brain did not start listening")


def query(sql):
    """Every row of `sql` on the test database, the first column as text (writes commit too)."""
    if DRIVER == "sqlite":
        con = sqlite3.connect(DBCFG["path"])
        try:
            rows = [str(r[0]) for r in con.execute(sql).fetchall()]
            con.commit()
            return rows
        finally:
            con.close()
    if DRIVER == "postgres":
        get = lambda k, d: os.environ.get("KN_DATABASE_" + k.upper(), str(CONFIG_DB.get(k, d)))
        env = dict(os.environ, PGPASSWORD=get("password", ""))
        r = subprocess.run(["psql", "-h", get("host", "127.0.0.1"), "-p", get("port", 5432), "-U", get("user", "krunknative"),
                            "-d", DB, "-v", "ON_ERROR_STOP=1", "-qAt", "-c", "SET search_path TO %s; %s" % (SCHEMA, sql)],
                           env=env, capture_output=True, timeout=30)
        return r.stdout.decode().split("\n")[:-1] if r.returncode == 0 else None
    tool = shutil.which("mariadb") or shutil.which("mysql")
    r = subprocess.run([tool, "--no-defaults", "--protocol=TCP", "-h", DBCFG["host"], "-P", str(DBCFG["port"]), "-u", DBCFG["user"],
                        "-N", "-B", DBCFG["name"], "-e", sql], env=dict(os.environ, MYSQL_PWD=DBCFG.get("password", "")),
                       capture_output=True, timeout=30)
    return r.stdout.decode().split("\n")[:-1] if r.returncode == 0 else None


def one(sql, default=0):
    rows = query(sql)
    return int(rows[0]) if rows else default


class Social:
    def __init__(self, url):
        self.url = url + "?enc=json"
        self.inbox = []

    async def __aenter__(self):
        self.ws = await websockets.connect(self.url, max_size=4 << 20)
        return self

    async def __aexit__(self, *a):
        await self.ws.close()

    async def call(self, label, *frame):
        if frame:
            await self.ws.send(json.dumps(list(frame)))
        deadline = time.time() + 10
        while True:
            for i, f in enumerate(self.inbox):
                if f[0] == label:
                    return self.inbox.pop(i)
            self.inbox.append(json.loads(await asyncio.wait_for(self.ws.recv(), deadline - time.time())))


class Node:
    """Game server side of /mm/internal/ws: the "acct" ops (give_skin, junk_credit)."""

    NODES = [0]

    def __init__(self, port):
        self.url = "ws://127.0.0.1:%d/mm/internal/ws" % port
        self.rid = 0
        Node.NODES[0] += 1
        self.node_id = "KRT%d" % Node.NODES[0]

    async def __aenter__(self):
        self.ws = await websockets.connect(self.url, additional_headers={"Authorization": "Bearer " + SECRET})
        await self.ws.send(json.dumps({"type": "hello", "nodeId": self.node_id, "region": "local", "publicHost": "127.0.0.1",
                                       "publicPort": 1, "version": "test"}))
        while json.loads(await asyncio.wait_for(self.ws.recv(), 10)).get("type") != "welcome":
            pass
        return self

    async def __aexit__(self, *a):
        await self.ws.close()

    async def op(self, op, **args):
        self.rid += 1
        rid = self.rid
        await self.ws.send(json.dumps({"type": "acct", "rid": rid, "op": op, "args": args}))
        while True:
            msg = json.loads(await asyncio.wait_for(self.ws.recv(), 15))
            if msg.get("type") == "acct-r" and msg.get("rid") == rid:
                return msg.get("err"), msg.get("data")



async def register(sock, name):
    r = await sock.call("a", "a", 0, [name, "rentpass1", 0], "captcha")
    assert r[1] is None, r
    return r[2], r[5]


def http(port, method, path, body=None, token=None):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request("http://127.0.0.1:%d%s" % (port, path), data=data, method=method)
    if data is not None:
        req.add_header("Content-Type", "application/json")
    if token:
        req.add_header("Authorization", "Bearer " + token)
    try:
        with urllib.request.urlopen(req, timeout=15) as resp:
            return resp.status, json.loads(resp.read() or b"null")
    except urllib.error.HTTPError as e:
        raw = e.read()
        try:
            return e.code, json.loads(raw or b"null")
        except ValueError:
            return e.code, raw.decode(errors="replace")


POOL = [{"id": 1, "host": "testhost", "region": 0, "name": "Ashburn", "sid": 1, "port": 31024, "public_host": "a.example"},
        {"id": 2, "host": "testhost", "region": 0, "name": "Ashburn", "sid": 2, "port": 31025, "public_host": "a.example"},
        {"id": 3, "host": "otherhost", "region": 13, "name": "Frankfurt", "sid": 1, "port": 31026, "public_host": "f.example"}]
POOL_ARGS = ["--brain.rentals.pool=" + json.dumps(POOL), "--brain.rentals.check_ms=500", "--brain.rentals.start_timeout_s=3",
             "--brain.rentals.max_per_account=1"]
GAME = {"typ": 0, "mid": 77, "gnm": "Arena", "cr": "alice"}


def agent_poll(port, host):
    return http(port, "GET", "/mm/internal/rentals?host=" + host, token=SECRET)


async def rent(sock, sid, game, update=None):
    frame = ["rdsrv", sid, game] + ([update] if update is not None else [])
    sock.inbox = [f for f in sock.inbox if f[0] != "dsrvl"]
    r, before = await sock.call("dsrvl", *frame), None
    while r[1] != 1:  # the fresh list (a success's, or a stray one)
        before, r = r, await sock.call("dsrvl")
    # a success's list comes first: dediRes [0] redraws the window and would wipe the text (deob-10.0.7.js:219812)
    if r[3] is True:
        check(before is not None and before[1] == 0, "rdsrv: a success sends the fresh list before its text")
    return r


async def listing(sock):
    sock.inbox = [f for f in sock.inbox if f[0] != "dsrvl"]
    while True:
        r = await sock.call("dsrvl", "dsrvl")
        if r[1] == 0:
            return {row["ID"]: row for row in r[2]}


def wait(cond, timeout=15):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if cond():
            return True
        time.sleep(0.25)
    return False


async def scenario(port):
    stamp = str(int(time.time()) % 100000)
    social = "ws://127.0.0.1:%d/social/ws" % port
    async with Social(social) as guest, Social(social) as sock_a, Social(social) as sock_b:
        rows = await listing(guest)
        check(sorted(rows) == [1, 2, 3], "the pool's three slots are listed: %s" % sorted(rows))
        r1 = rows.get(1, {})
        check(r1.get("REG") == 0 and r1.get("SVN") == "Ashburn" and r1.get("SID") == 1 and r1.get("TRM") == 0
              and r1.get("OID") is None and r1.get("CONF") is None, "a free row: REG 0, Ashburn_1, TRM 0, no owner / CONF: %s" % r1)
        r = await rent(guest, 1, GAME)
        check(r[2] == "Sign in to rent a server." and r[3] is False, "a guest is refused: %s" % r)

        a_id, a_tok = await register(sock_a, "renta" + stamp)
        b_id, b_tok = await register(sock_b, "rentb" + stamp)
        query("UPDATE players SET player_funds = 30000 WHERE player_id = %d" % a_id)
        query("UPDATE players SET player_funds = 100 WHERE player_id = %d" % b_id)

        # the agent API needs the secret
        status, _ = http(port, "GET", "/mm/internal/rentals?host=testhost")
        check(status == 401, "the agent API without the secret is 401: %s" % status)
        r = await rent(sock_a, 1, GAME)
        check(r[3] is False and "offline" in r[2], "a slot whose agent never polled is offline: %s" % r)
        status, body = agent_poll(port, "testhost")
        check(status == 200 and [s["id"] for s in body.get("slots", [])] == [1, 2] and not any(s["run"] for s in body["slots"]),
              "the agent's poll: its two slots, none to run: %s" % body)

        r = await rent(sock_a, 99, GAME)
        check(r[2] == "That server doesn't exist.", "an unknown server: %s" % r[2])
        r = await rent(sock_a, 1, {"typ": 5, "gnm": "Arena"})
        check(r[2] == "Pick a rental duration.", "a duration outside 0..2: %s" % r[2])
        r = await rent(sock_a, 1, {"typ": 0})
        check(r[2] == "Pick a game for the server first.", "no game: %s" % r[2])
        r = await rent(sock_b, 1, GAME)
        check(r[2] == "Not enough KR" and r[3] is False, "100 KR does not rent: %s" % r)
        check(one("SELECT COUNT(*) FROM rented_servers WHERE owner_id IS NOT NULL") == 0, "a refusal writes nothing")

        gen0 = one("SELECT generation FROM rented_servers WHERE id = 1")
        r = await rent(sock_a, 1, GAME)
        check(r[3] is True and "rented" in r[2], "the rent succeeds (keepHidden true): %s" % r)
        check(one("SELECT player_funds FROM players WHERE player_id = %d" % a_id) == 22500, "7,500 KR charged (dediPrices[0])")
        check(one("SELECT COUNT(*) FROM kr_transactions WHERE account_id = %d AND source = 'dedi' AND amount = -7500 AND ref = '1:0'" % a_id) == 1,
              "the ledger row: source dedi, ref 1:0")
        rows = await listing(sock_a)
        row = rows[1]
        days = row["TRM"] / 86400000.0
        check(row["OID"] == a_id and 29.99 < days <= 30.0 and json.loads(row["CONF"]).get("mn") == "Arena"
              and json.loads(row["CONF"]).get("gid") == 77, "the row: owner, 30 days left, CONF {gid 77, mn Arena}: %s" % row)
        # the startup guarantee is kept once the agent reports the generation running (right away: start_timeout_s 3)
        status, body = http(port, "POST", "/mm/internal/rentals/status",
                            {"host": "testhost", "slots": [{"id": 1, "generation": gen0 + 1, "state": "running"}]}, SECRET)
        check(status == 200 and body.get("updated") == 1, "the status post: %s %s" % (status, body))
        check(one("SELECT paid FROM rented_servers WHERE id = 1") == 0, "running the rental's generation keeps the guarantee (paid 0)")

        check(one("SELECT generation FROM rented_servers WHERE id = 1") == gen0 + 1, "the rent bumps the generation")
        status, body = agent_poll(port, "testhost")
        s1 = [s for s in body["slots"] if s["id"] == 1][0]
        check(s1["run"] is True and s1["map"] == "Arena" and s1["creator"] == "alice" and s1["name"] == "Ashburn_1"
              and s1["generation"] == gen0 + 1, "the agent runs Ashburn_1 with published alice/Arena: %s" % s1)

        r = await rent(sock_a, 2, GAME)
        check(r[3] is False and "most one account" in r[2], "the per-account limit (max_per_account 1): %s" % r[2])
        query("UPDATE players SET player_funds = 30000 WHERE player_id = %d" % b_id)
        r = await rent(sock_b, 1, GAME)
        check(r[2] == "Someone else is renting this server.", "another player's server: %s" % r[2])
        r = await rent(sock_b, 2, GAME, 1)
        check(r[2] == "You don't rent this server.", "an update of a server not rented: %s" % r[2])

        # extend: 60 days for 14,000 KR; the same game keeps the generation
        g = one("SELECT generation FROM rented_servers WHERE id = 1")
        r = await rent(sock_a, 1, dict(GAME, typ=1))
        check(r[3] is True and r[2] == "Rental extended.", "extend: %s" % r)
        check(one("SELECT player_funds FROM players WHERE player_id = %d" % a_id) == 8500, "14,000 KR charged (dediPrices[1])")
        rows = await listing(sock_a)
        check(89.9 < rows[1]["TRM"] / 86400000.0 <= 90.0, "30 + 60 days left: %.2f" % (rows[1]["TRM"] / 86400000.0))
        check(one("SELECT generation FROM rented_servers WHERE id = 1") == g, "the same game keeps the generation")
        r = await rent(sock_a, 1, dict(GAME, typ=2))
        check(r[2] == "Not enough KR", "extending past the balance is refused: %s" % r[2])

        # Restart/Update: free, a new game, generation + 1
        r = await rent(sock_a, 1, {"typ": 0, "mid": 5, "gnm": "Other", "cr": "bob"}, 1)
        check(r[3] is True and "restarting" in r[2], "restart/update: %s" % r)
        check(one("SELECT player_funds FROM players WHERE player_id = %d" % a_id) == 8500, "an update is free")
        check(one("SELECT generation FROM rented_servers WHERE id = 1") == g + 1, "an update bumps the generation")
        status, body = agent_poll(port, "testhost")
        s1 = [s for s in body["slots"] if s["id"] == 1][0]
        check(s1["map"] == "Other" and s1["creator"] == "bob", "the agent gets the new game: %s" % s1)

        # the startup guarantee's refund: b rents slot 3 (its agent polled) and it never runs
        agent_poll(port, "otherhost")
        r = await rent(sock_b, 3, GAME)
        check(r[3] is True, "b rents Frankfurt_1: %s" % r)
        check(one("SELECT player_funds FROM players WHERE player_id = %d" % b_id) == 22500, "b paid 7,500")
        check(wait(lambda: one("SELECT COUNT(*) FROM rented_servers WHERE id = 3 AND owner_id IS NULL") == 1),
              "a rental that never ran within start_timeout_s is freed")
        check(one("SELECT player_funds FROM players WHERE player_id = %d" % b_id) == 30000, "and refunded in full")
        check(one("SELECT COUNT(*) FROM kr_transactions WHERE account_id = %d AND source = 'dedi_refund' AND amount = 7500" % b_id) == 1,
              "the refund's ledger row (dedi_refund)")

        # expiry: a's rental lapses
        g = one("SELECT generation FROM rented_servers WHERE id = 1")
        query("UPDATE rented_servers SET expires_s = 1 WHERE id = 1")
        check(wait(lambda: one("SELECT COUNT(*) FROM rented_servers WHERE id = 1 AND owner_id IS NULL") == 1), "a lapsed rental is freed")
        check(one("SELECT generation FROM rented_servers WHERE id = 1") == g + 1, "the expiry bumps the generation")
        check(one("SELECT player_funds FROM players WHERE player_id = %d" % a_id) == 8500, "an expiry refunds nothing")
        status, body = agent_poll(port, "testhost")
        check(not any(s["run"] for s in body["slots"]), "the agent stops it (run false)")
        rows = await listing(sock_a)
        check(rows[1]["TRM"] == 0 and rows[1]["OID"] is None, "the window lists it free again")


def start_brain(env, log_path, extra):
    with open(log_path, "a") as log:
        log.write("== start %s\n" % extra)
        proc = subprocess.Popen([BRAIN, "--brain.host=127.0.0.1", "--brain.port=0", "--brain.id=krt-%d" % os.getpid(),
                                 "--accounts.signup_rewards.sweep_s=1", "--accounts.login_throttle_s=0"] + extra,
                                cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT)
    return proc, bound_port(log_path, proc)


def main():
    global DBCFG
    if DRIVER == "postgres":
        pg = [sys.executable, "-I", os.path.join(ROOT, "services", "brain", "tests", "pg_schema.py"), ROOT, DB]
        rc = subprocess.run(pg + ["create", SCHEMA]).returncode
        if rc:
            sys.exit(rc)
        dbenv = {"KN_DATABASE_NAME": DB, "KN_DATABASE_SEARCH_PATH": SCHEMA}
        drop = pg + ["drop", SCHEMA]
    else:
        scratch = [sys.executable, "-I", os.path.join(ROOT, "services", "brain", "tests", "db_scratch.py"), ROOT]
        r = subprocess.run(scratch + ["create", DRIVER, SCHEMA], capture_output=True)
        if r.returncode:
            print("rentals test: %s unavailable: skipped" % DRIVER)
            sys.exit(r.returncode)
        DBCFG = json.loads(r.stdout)
        dbenv = {"KN_DATABASE_" + k.upper(): str(v) for k, v in DBCFG.items()}
        drop = scratch + ["drop", DRIVER, SCHEMA]
    work = tempfile.mkdtemp(prefix="kn-rentals.", dir=os.path.join(os.path.expanduser("~"), ".cache"))
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=SECRET, **dbenv)
    log_path = os.path.join(work, "brain.log")
    proc = None
    try:
        subprocess.run([BRAIN, "--migrate-only"], cwd=ROOT, env=env, check=True, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)
        proc, port = start_brain(env, log_path, POOL_ARGS)
        asyncio.run(scenario(port))
    finally:
        if proc:
            proc.terminate()
            proc.wait(5)
        subprocess.run(drop)
    with open(log_path) as f:
        bad = [l.strip() for l in f if re.search(r"failed:|ERROR|FATAL", l)]
    check(not bad, "no statement errors in the brain log %s" % bad[:3])
    print("rentals test: " + ("OK" if not FAILS else "%d failure(s)" % len(FAILS)))
    if FAILS:
        print("log kept in " + work)
    else:
        shutil.rmtree(work, ignore_errors=True)
    sys.exit(1 if FAILS else 0)


main()
