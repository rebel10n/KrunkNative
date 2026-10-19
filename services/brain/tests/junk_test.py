#!/usr/bin/env python3
"""The Junk wallet on kn_brain (doc/v10/junk.md; services/brain/db_junk.c, accounts.c ops
junk_balance / junk_credit / junk_spend, social_ws.c "uj" and the account JSON's "junk").

  junk_test.py <kn_brain> <repo root> [database]
Database: argument, else $KN_MM_TEST_DATABASE, else config/config.json's database.name; every
run uses its own schema and an ephemeral port. 77 = skip (no PostgreSQL / python websockets).
KN_TEST_DB_DRIVER=sqlite|mysql runs on that store driver (db_scratch.py).

Covers: the balance starts at 0.00; a credit (hundredths, the reason as the ledger source) reads
back as "x.xx"; a spend takes it down; an unaffordable spend is refused with "Not enough Junk" and
writes nothing (no balance change, no ledger row); bad amounts and unknown accounts are refused;
the balance is pushed to the account's social socket as ["uj", "x.xx"] after a game server's
credit; the account JSON and the social login info carry "junk" as text; and parallel spends at
one balance never take it below 0 (exactly the affordable ones succeed). The ledger rows of an
account sum to its balance.
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
    with open(os.path.join(ROOT, "config", "config.json")) as fp:
        CONFIG_DB = json.load(fp).get("database", {})
except (OSError, ValueError):
    pass
if not DB:
    DB = CONFIG_DB.get("name", "")
if not DB and DRIVER == "postgres":
    print("junk test: no database configured; skipped")
    sys.exit(77)
SCHEMA = "kn_test_junk_%d" % os.getpid()
SECRET = "kn-junk-test-%d" % os.getpid()
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
    """First column of every row of `sql` on the test database, as text."""
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

    async def drain(self, seconds):
        deadline = time.time() + seconds
        while time.time() < deadline:
            try:
                self.inbox.append(json.loads(await asyncio.wait_for(self.ws.recv(), deadline - time.time())))
            except asyncio.TimeoutError:
                break

    def take(self, label):
        for i, f in enumerate(self.inbox):
            if f[0] == label:
                return self.inbox.pop(i)
        return None


class Node:
    """Game server side of /mm/internal/ws: the "acct" ops."""

    NODES = [0]

    def __init__(self, port):
        self.url = "ws://127.0.0.1:%d/mm/internal/ws" % port
        self.rid = 0
        Node.NODES[0] += 1
        self.node_id = "KJW%d" % Node.NODES[0] # one node id per connection: the brain drops a duplicate

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
    r = await sock.call("a", "a", 0, [name, "junkpass1", 0], "captcha")
    assert r[1] is None, r
    return r[2], r[5], r


async def login(sock, name, token):
    r = await sock.call("a", "a", 1, [name, None, token])
    assert r[1] is None, r
    return r


async def wait_push(sock, label, seconds=3.0):
    deadline = time.time() + seconds
    while time.time() < deadline:
        f = sock.take(label)
        if f:
            return f
        await sock.drain(0.2)
    return None


def balance_row(account_id):
    rows = query("SELECT player_junk FROM players WHERE player_id = %d" % account_id)
    return int(rows[0]) if rows else None


def ledger_sum(account_id):
    rows = query("SELECT COALESCE(SUM(amount), 0) FROM junk_transactions WHERE account_id = %d" % account_id)
    return int(rows[0]) if rows else 0


def ledger_count(account_id):
    rows = query("SELECT COUNT(*) FROM junk_transactions WHERE account_id = %d" % account_id)
    return int(rows[0]) if rows else 0


async def scenario(port):
    stamp = str(int(time.time()) % 100000)
    alice, bob = "junka" + stamp, "junkb" + stamp
    social = "ws://127.0.0.1:%d/social/ws" % port
    async with Social(social) as sock, Social(social) as sock_b, Node(port) as node:
        # one social socket per account: a socket signed in as one account gets only that account's pushes
        a_id, a_tok, _ = await register(sock, alice)
        b_id, b_tok, _ = await register(sock_b, bob)
        check(isinstance(a_id, int) and a_id > 0 and isinstance(b_id, int) and b_id > 0, "two accounts registered")

        err, data = await node.op("junk_balance", id=a_id)
        check(err is None and data == {"junk": "0.00"}, "a new account's junk_balance is 0.00: %s %s" % (err, data))
        check(balance_row(a_id) == 0 and ledger_count(a_id) == 0, "a new account has no balance row change and no ledger row")

        err, data = await node.op("junk_credit", id=a_id, amount=5, reason="trigger")
        check(err is None and data.get("junk") == "0.05" and data.get("credited") == 5,
              "credit 5 hundredths (a 0.05 pickup) reads back 0.05: %s %s" % (err, data))
        check(balance_row(a_id) == 5, "the balance row holds 5 hundredths")

        err, data = await node.op("junk_credit", id=a_id, amount=1234, reason="airdrop")
        check(err is None and data.get("junk") == "12.39", "credits add up to 12.39: %s %s" % (err, data))

        # every credit pushes the new balance; the socket's queue holds the 0.05 push first
        pushed = None
        deadline = time.time() + 5
        while time.time() < deadline and not (pushed and pushed[1] == "12.39"):
            pushed = await wait_push(sock, "uj", 1.0) or pushed
        check(pushed is not None and pushed[1] == "12.39", "the social socket gets [\"uj\", \"12.39\"] after the game's credit: %s" % pushed)

        err, data = await node.op("junk_spend", id=a_id, amount=1000, reason="craft")
        check(err is None and data == {"junk": "2.39"}, "spend 10.00 of 12.39 leaves 2.39: %s %s" % (err, data))
        check(balance_row(a_id) == 239 and ledger_sum(a_id) == 239 and ledger_count(a_id) == 3,
              "the ledger's three rows sum to the balance (239)")

        before = (balance_row(a_id), ledger_count(a_id))
        err, data = await node.op("junk_spend", id=a_id, amount=99999, reason="craft")
        check(err == "Not enough Junk" and data is None, "an unaffordable spend is refused with 'Not enough Junk': %s %s" % (err, data))
        check((balance_row(a_id), ledger_count(a_id)) == before, "a refused spend writes nothing (balance and ledger unchanged)")

        err, data = await node.op("junk_spend", id=a_id, amount=239, reason="craft")
        check(err is None and data == {"junk": "0.00"}, "spending exactly the balance leaves 0.00: %s %s" % (err, data))

        err, _ = await node.op("junk_credit", id=a_id, amount=0, reason="trigger")
        check(err == "Invalid amount.", "a zero credit is refused: %s" % err)
        err, _ = await node.op("junk_spend", id=a_id, amount=-5, reason="craft")
        check(err == "Invalid amount.", "a negative spend is refused: %s" % err)
        err, _ = await node.op("junk_credit", id=999999999, amount=5, reason="trigger")
        check(err == "Account not found.", "a credit to an unknown account is refused: %s" % err)
        err, _ = await node.op("junk_balance", id=999999999)
        check(err == "Account not found.", "a balance of an unknown account is refused: %s" % err)

        await node.op("junk_credit", id=b_id, amount=100, reason="trigger")
        nodes = [await Node(port).__aenter__() for _ in range(6)]
        try:
            results = await asyncio.gather(*[n.op("junk_spend", id=b_id, amount=30, reason="craft") for n in nodes])
        finally:
            for n in nodes:
                await n.__aexit__()
        oks = [r for r in results if r[0] is None]
        refused = [r for r in results if r[0] == "Not enough Junk"]
        check(len(oks) == 3 and len(refused) == 3 and len(oks) + len(refused) == 6,
              "six parallel spends of 0.30 at 1.00: exactly three succeed, three are refused (%d/%d)" % (len(oks), len(refused)))
        check(balance_row(b_id) == 10, "the balance after the parallel spends is 0.10, never negative: %s" % balance_row(b_id))
        check(ledger_sum(b_id) == 10, "the ledger of the parallel spends sums to the balance")

        # the account JSON and the social login info carry the balance as text
        r = await login(sock_b, bob, b_tok)
        info = r[6] if len(r) > 6 and isinstance(r[6], dict) else {}
        check(info.get("junk") == "0.10", "the social login info carries junk \"0.10\": %s" % info.get("junk"))
        err, data = await node.op("get", id=b_id)
        check(err is None and isinstance(data, dict) and data.get("junk") == "0.10",
              "the account JSON (op get) carries junk \"0.10\": %s" % (data.get("junk") if isinstance(data, dict) else data))


def start_brain(env, log_path, extra):
    with open(log_path, "a") as log:
        log.write("== start %s\n" % extra)
        proc = subprocess.Popen([BRAIN, "--brain.host=127.0.0.1", "--brain.port=0", "--brain.id=kjw-%d" % os.getpid(),
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
            print("junk test: %s unavailable: skipped" % DRIVER)
            sys.exit(r.returncode)
        DBCFG = json.loads(r.stdout)
        dbenv = {"KN_DATABASE_" + k.upper(): str(v) for k, v in DBCFG.items()}
        drop = scratch + ["drop", DRIVER, SCHEMA]
    work = tempfile.mkdtemp(prefix="kn-junk.", dir=os.path.join(os.path.expanduser("~"), ".cache"))
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=SECRET, **dbenv)
    log_path = os.path.join(work, "brain.log")
    proc = None
    try:
        subprocess.run([BRAIN, "--migrate-only"], cwd=ROOT, env=env, check=True, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)
        proc, port = start_brain(env, log_path, [])
        asyncio.run(scenario(port))
    finally:
        if proc:
            proc.terminate()
            proc.wait(5)
        subprocess.run(drop)
    with open(log_path) as f:
        bad = [l.strip() for l in f if re.search(r"failed:|ERROR|FATAL", l)]
    check(not bad, "no statement errors in the brain log %s" % bad[:3])
    print("junk test: " + ("OK" if not FAILS else "%d failure(s)" % len(FAILS)))
    if FAILS:
        print("log kept in " + work)
    else:
        shutil.rmtree(work, ignore_errors=True)
    sys.exit(1 if FAILS else 0)


main()
