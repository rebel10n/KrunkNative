#!/usr/bin/env python3
"""Krunker v7.1.4's /xp and /kr on kn_brain: the game server's "acct" op dev_grant (services/brain/db_accounts.c
run_dev_grant, services/brain/accounts.c).
  dev_grant_test.py <kn_brain> <repo root> [database]
Database: argument, else $KN_MM_TEST_DATABASE, else config/config.json's (or $KN_TEST_CONFIG's) database.name (local
PostgreSQL); every run uses its own schema (kn_test_dev_grant_<pid>, dropped after) and an ephemeral port.
KN_TEST_DB_DRIVER=sqlite|mysql runs on that store driver (db_scratch.py). 77 = skip.
Two accounts register over the social socket (alice a player, bob made a developer); a fake game server on
/mm/internal/ws then:
  - dev_grant xp / kr for alice (a player): "Only developers can grant ..."; her score, funds, ledger unchanged
  - an actor 0 (the console, a guest) is refused the same way
  - bob: xp 5000 adds player_score (the reply's level follows), kr 250 adds player_funds with one
    kr_transactions row (amount 250, source 'admin', ref 'dev_grant'); the reply is bob's account
    {id, funds, score, level, kind, granted}; bob's open social socket gets "ua" with the new funds
  - amount 0, -5 and 10000001, and a kind other than xp / kr, are refused; nothing changes
  - moderation_log has bob's two grants (actor = target = bob, dev_grant_xp / dev_grant_kr, detail the amount)
  - no purchase table is touched: kr_transactions has no other row for bob
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

BRAIN, ROOT = sys.argv[1], sys.argv[2]
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
    print("dev grant test: no database configured; skipped")
    sys.exit(77)
SCHEMA = "kn_test_dev_grant_%d" % os.getpid()
SECRET = "kn-dev-grant-test-%d" % os.getpid()
FAILS = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what, flush=True)
    if not cond:
        FAILS.append(what)


def bound_port(log_path, proc):
    for _ in range(200):
        if proc.poll() is not None:
            raise RuntimeError("kn_brain exited early (%s)" % log_path)
        with open(log_path) as fp:
            m = re.search(r"listening on http://127\.0\.0\.1:(\d+) \(ws", fp.read())
        if m:
            return int(m.group(1))
        time.sleep(0.05)
    raise RuntimeError("kn_brain did not start listening")


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
        await self.ws.send(json.dumps(list(frame)))
        return await self.wait(label)

    async def wait(self, label, secs=10, pred=lambda f: True):
        deadline = time.time() + secs
        while True:
            for i, f in enumerate(self.inbox):
                if f[0] == label and pred(f):
                    return self.inbox.pop(i)
            left = deadline - time.time()
            if left <= 0:
                return None
            try:
                self.inbox.append(json.loads(await asyncio.wait_for(self.ws.recv(), left)))
            except asyncio.TimeoutError:
                return None


class Node:
    """Game server side of /mm/internal/ws: "acct" ops only."""

    def __init__(self, port):
        self.url = "ws://127.0.0.1:%d/mm/internal/ws" % port
        self.rid = 0

    async def __aenter__(self):
        self.ws = await websockets.connect(self.url, additional_headers={"Authorization": "Bearer " + SECRET})
        await self.ws.send(json.dumps({"type": "hello", "nodeId": "GRANT", "region": "local", "publicHost": "127.0.0.1",
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
    r = await sock.call("a", "a", 0, [name, "grantpass12", 0], "captcha")
    assert r[1] is None, r
    return r[2]


def sql(dbcfg, text):
    """Runs one statement on the test schema; returns rows (lists of strings) or None when not possible."""
    if DRIVER == "sqlite":
        con = sqlite3.connect(dbcfg["path"])
        rows = [list(map(lambda v: "" if v is None else str(v), r)) for r in con.execute(text).fetchall()]
        con.commit()
        con.close()
        return rows
    if DRIVER == "postgres":
        get = lambda k, d: os.environ.get("KN_DATABASE_" + k.upper(), str(CONFIG_DB.get(k, d)))
        env = dict(os.environ, PGPASSWORD=get("password", ""))
        r = subprocess.run(["psql", "-h", get("host", "127.0.0.1"), "-p", get("port", 5432), "-U", get("user", "krunknative"),
                            "-d", DB, "-v", "ON_ERROR_STOP=1", "-qAt", "-c", "SET search_path TO %s; %s" % (SCHEMA, text)],
                           env=env, capture_output=True, text=True, timeout=30)
        if r.returncode:
            return None
        return [l.split("|") for l in r.stdout.splitlines() if l and l != "SET" and not l.startswith(("UPDATE", "INSERT"))]
    if DRIVER == "mysql":
        tool = shutil.which("mariadb") or shutil.which("mysql")
        if not tool:
            return None
        r = subprocess.run([tool, "--no-defaults", "--protocol=TCP", "-h", dbcfg["host"], "-P", str(dbcfg["port"]), "-u", dbcfg["user"],
                            "-D", dbcfg["name"], "-N", "-B", "-e", text],
                           env=dict(os.environ, MYSQL_PWD=dbcfg.get("password", "")), capture_output=True, text=True, timeout=30)
        if r.returncode:
            return None
        return [["" if v == "NULL" else v for v in l.split("\t")] for l in r.stdout.splitlines() if l]
    return None


def money(dbcfg, aid):
    rows = sql(dbcfg, "SELECT player_score, player_funds FROM players WHERE player_id = %d" % aid)
    return (int(rows[0][0]), int(rows[0][1])) if rows else None


def ledger(dbcfg, aid):
    return sql(dbcfg, "SELECT amount, source, coalesce(ref, '') FROM kr_transactions WHERE account_id = %d ORDER BY id" % aid)


async def scenario(port, dbcfg):
    stamp = str(int(time.time()) % 100000)
    alice, bob = "grna" + stamp, "grnb" + stamp
    social = "ws://127.0.0.1:%d/social/ws" % port
    async with Social(social) as s:
        alice_id = await register(s, alice)
    bob_sock = Social(social)
    await bob_sock.__aenter__()
    bob_id = await register(bob_sock, bob)
    check(sql(dbcfg, "UPDATE players SET player_developer = 1 WHERE player_id = %d" % bob_id) is not None, "bob is made a developer")
    alice0, bob0 = money(dbcfg, alice_id), money(dbcfg, bob_id)
    alice_ledger0, bob_ledger0 = ledger(dbcfg, alice_id), ledger(dbcfg, bob_id)
    check(alice0 is not None and bob0 is not None, "the players rows: %s %s" % (alice0, bob0))

    async with Node(port) as n:
        print("== a player is refused")
        err, data = await n.op("dev_grant", actor=alice_id, kind="xp", amount=5000)
        check(err == "Only developers can grant XP" and data is None, "alice's xp: %s %s" % (err, data))
        err, data = await n.op("dev_grant", actor=alice_id, kind="kr", amount=250)
        check(err == "Only developers can grant KR" and data is None, "alice's kr: %s %s" % (err, data))
        err, _ = await n.op("dev_grant", actor=0, kind="kr", amount=250)
        check(err == "Only developers can grant KR", "actor 0 (the console, a guest): %s" % err)
        check(money(dbcfg, alice_id) == alice0 and ledger(dbcfg, alice_id) == alice_ledger0,
              "alice's score, funds and ledger are unchanged: %s" % (money(dbcfg, alice_id),))

        print("== a developer grants to itself")
        err, data = await n.op("dev_grant", actor=bob_id, kind="xp", amount=5000)
        check(err is None and data and data.get("id") == bob_id and data.get("kind") == "xp" and data.get("granted") == 5000
              and data.get("score") == bob0[0] + 5000, "bob's xp 5000: %s %s" % (err, data and {k: data.get(k) for k in ("id", "score", "kind", "granted")}))
        level_after_xp = data.get("level") if data else None
        check(level_after_xp is not None and level_after_xp >= 2, "the reply carries the new level (%s)" % level_after_xp)
        ua = await bob_sock.wait("ua", 5, lambda f: isinstance(f[2], dict) and f[2].get("level") == level_after_xp)
        check(ua is not None, "bob's social socket gets \"ua\" with the new level: %s" % (ua and ua[2].get("level")))
        err, data = await n.op("dev_grant", actor=bob_id, kind="kr", amount=250)
        check(err is None and data and data.get("kind") == "kr" and data.get("granted") == 250 and data.get("funds") == bob0[1] + 250,
              "bob's kr 250: %s %s" % (err, data and {k: data.get(k) for k in ("funds", "kind", "granted")}))
        ua = await bob_sock.wait("ua", 5, lambda f: isinstance(f[2], dict) and f[2].get("funds") == bob0[1] + 250)
        check(ua is not None, "bob's social socket gets \"ua\" with the new funds: %s" % (ua and ua[2].get("funds")))
        check(money(dbcfg, bob_id) == (bob0[0] + 5000, bob0[1] + 250), "bob's players row: %s" % (money(dbcfg, bob_id),))
        rows = ledger(dbcfg, bob_id)
        check(rows == bob_ledger0 + [["250", "admin", "dev_grant"]], "bob's kr_transactions: the one grant row only: %s" % rows)

        print("== bad amounts and kinds")
        for kind, amount, want in (("xp", 0, "Amount must be 1 to 10000000"), ("kr", -5, "Amount must be 1 to 10000000"),
                                   ("kr", 10000001, "Amount must be 1 to 10000000"), ("skins", 5, "Unknown grant")):
            err, data = await n.op("dev_grant", actor=bob_id, kind=kind, amount=amount)
            check(err == want and data is None, "%s %d: %s" % (kind, amount, err))
        check(money(dbcfg, bob_id) == (bob0[0] + 5000, bob0[1] + 250) and ledger(dbcfg, bob_id) == rows,
              "the refusals changed nothing: %s" % (money(dbcfg, bob_id),))

        rows = sql(dbcfg, "SELECT coalesce(actor_id, 0), target_id, action, detail FROM moderation_log WHERE target_id IN (%d, %d) ORDER BY id"
                   % (alice_id, bob_id))
        check(rows == [[str(bob_id), str(bob_id), "dev_grant_xp", "5000"], [str(bob_id), str(bob_id), "dev_grant_kr", "250"]],
              "moderation_log: bob's two grants only: %s" % rows)
    await bob_sock.__aexit__()


def main():
    if DRIVER == "postgres":
        pg = [sys.executable, "-I", os.path.join(ROOT, "services", "brain", "tests", "pg_schema.py"), ROOT, DB]
        rc = subprocess.run(pg + ["create", SCHEMA]).returncode
        if rc:
            sys.exit(rc)
        dbenv = {"KN_DATABASE_NAME": DB, "KN_DATABASE_SEARCH_PATH": SCHEMA}
        dbcfg = {}
        drop = pg + ["drop", SCHEMA]
    else:
        scratch = [sys.executable, "-I", os.path.join(ROOT, "services", "brain", "tests", "db_scratch.py"), ROOT]
        r = subprocess.run(scratch + ["create", DRIVER, SCHEMA], capture_output=True)
        if r.returncode:
            print("dev grant test: %s unavailable: skipped" % DRIVER)
            sys.exit(r.returncode)
        dbcfg = json.loads(r.stdout)
        dbenv = {"KN_DATABASE_" + k.upper(): str(v) for k, v in dbcfg.items()}
        drop = scratch + ["drop", DRIVER, SCHEMA]
    work = tempfile.mkdtemp(prefix="kn-dev-grant.")
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=SECRET, **dbenv)
    log_path = os.path.join(work, "brain.log")
    proc = None
    try:
        subprocess.run([BRAIN, "--migrate-only"], cwd=ROOT, env=env, check=True, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)
        with open(log_path, "w") as log:
            proc = subprocess.Popen([BRAIN, "--brain.host=127.0.0.1", "--brain.port=0", "--brain.id=grant-%d" % os.getpid(),
                                     "--uploads.dir=" + os.path.join(work, "uploads")],
                                    cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT)
        port = bound_port(log_path, proc)
        asyncio.run(scenario(port, dbcfg))
    finally:
        if proc:
            proc.terminate()
            proc.wait(5)
        subprocess.run(drop)
    with open(log_path) as f:
        bad = [l.strip() for l in f if re.search(r"failed:|ERROR|FATAL", l)]
    check(not bad, "no statement errors in the brain log %s" % bad[:3])
    print("dev grant test: " + ("OK" if not FAILS else "%d failure(s)" % len(FAILS)))
    if FAILS:
        print("log kept in " + work)
    else:
        shutil.rmtree(work, ignore_errors=True)
    sys.exit(1 if FAILS else 0)


main()
