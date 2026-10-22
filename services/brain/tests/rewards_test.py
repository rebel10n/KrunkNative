#!/usr/bin/env python3
"""Krunker v10.0.2 rewards on kn_brain (doc/backend.md 7.9; services/brain/rewards.c, db_rewards.c,
season.c, migration 0116_v10_rewards): the one-time signup perks ("prr" after the first login of a
new account: 14 cosmetics + 24 h of premium, never twice, not for accounts older than the perks,
the premium taken back when it runs out), the v10 Battle Pass "bpclmAll" (every claimable card,
its skins and krdata KR), "bpdl"'s leveldata / krdata / jnkdata, a per-level cost (leveldata) and
the VIP tier gifted to another player ("xstbp" with a gift name).

  rewards_test.py <kn_brain> <repo root> [database]
Database: argument, else $KN_MM_TEST_DATABASE, else config/config.json's database.name; every
run uses its own schema and an ephemeral port. 77 = skip (no PostgreSQL / python websockets).
KN_TEST_DB_DRIVER=sqlite|mysql runs on that store driver (db_scratch.py).
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
    with open(os.path.join(ROOT, "config", "config.json")) as fp:
        CONFIG_DB = json.load(fp).get("database", {})
except (OSError, ValueError):
    pass
if not DB:
    DB = CONFIG_DB.get("name", "")
if not DB and DRIVER == "postgres":
    print("rewards test: no database configured; skipped")
    sys.exit(77)
SCHEMA = "kn_test_krw_%d" % os.getpid()
SECRET = "kn-rewards-test-%d" % os.getpid()
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
    """Game server side of /mm/internal/ws: commits and "acct" ops."""

    def __init__(self, port):
        self.url = "ws://127.0.0.1:%d/mm/internal/ws" % port
        self.rid = 0

    async def __aenter__(self):
        self.ws = await websockets.connect(self.url, additional_headers={"Authorization": "Bearer " + SECRET})
        await self.ws.send(json.dumps({"type": "hello", "nodeId": "KRW", "region": "local", "publicHost": "127.0.0.1",
                                       "publicPort": 1, "version": "test"}))
        while json.loads(await asyncio.wait_for(self.ws.recv(), 10)).get("type") != "welcome":
            pass
        return self

    async def __aexit__(self, *a):
        await self.ws.close()

    async def commit(self, match, final, player):
        self.rid += 1
        rid = self.rid
        await self.ws.send(json.dumps({"type": "commit", "rid": rid, "lobbyId": "se1", "matchId": match, "final": final,
                                       "map": "Burg", "mode": 0, "custom": False, "players": [player]}))
        while True:
            msg = json.loads(await asyncio.wait_for(self.ws.recv(), 15))
            if msg.get("type") == "commit-reply" and msg.get("rid") == rid:
                return msg

    async def op(self, op, **args):
        self.rid += 1
        rid = self.rid
        await self.ws.send(json.dumps({"type": "acct", "rid": rid, "op": op, "args": args}))
        while True:
            msg = json.loads(await asyncio.wait_for(self.ws.recv(), 15))
            if msg.get("type") == "acct-r" and msg.get("rid") == rid:
                return msg.get("err"), msg.get("data")



SIGNUP_SKINS = [113, 121, 119, 283, 260, 832, 834, 866, 871, 3, 15, 26, 36, 46]
DAY_MS = 24 * 3600 * 1000


async def register(sock, name):
    r = await sock.call("a", "a", 0, [name, "rewardpass1", 0], "captcha")
    assert r[1] is None, r
    return r[2], r[5], r


async def login(sock, name, token):
    r = await sock.call("a", "a", 1, [name, None, token])
    assert r[1] is None, r
    return r


async def give_kr(node, account_id, kr):
    while kr > 0:
        step = min(kr, 500)
        err, _ = await node.op("rewards", items=[[account_id, step]])
        assert err is None, err
        kr -= step


async def wait_push(sock, label, seconds=3.0):
    deadline = time.time() + seconds
    while time.time() < deadline:
        f = sock.take(label)
        if f:
            return f
        await sock.drain(0.2)
    return None


def inventory(account_id):
    rows = query("SELECT skin_id FROM inventory WHERE account_id = %d AND count > 0 ORDER BY skin_id" % account_id)
    return sorted(int(x) for x in rows or [])


def premium(account_id):
    r = query("SELECT player_premium FROM players WHERE player_id = %d" % account_id)
    return int(r[0]) if r else None


def items_of(bp):
    parts = [int(x) for x in bp["itemdata"].split(",") if x != ""]
    return [parts[i:i + 4] for i in range(0, len(parts) - 3, 4)]


async def scenario(port):
    stamp = str(int(time.time()) % 100000)
    alice, bob, old = "krwa" + stamp, "krwb" + stamp, "krwo" + stamp
    social = "ws://127.0.0.1:%d/social/ws" % port
    async with Social(social) as s, Node(port) as n:
        print("== signup rewards: the first login of a new account")
        alice_id, token, _ = await register(s, alice)
        prr = await wait_push(s, "prr")
        check(prr and sorted(prr[1]) == sorted(SIGNUP_SKINS) and prr[2] is True,
              "prr: 14 cosmetics + premium: %s" % (prr and prr[1:]))
        ua = await wait_push(s, "ua")
        inv = sorted(i["ind"] for i in (ua[1][10] if ua else []))
        check(ua and all(k in inv for k in SIGNUP_SKINS), "ua: the inventory with the 14 cosmetics: %s" % inv)
        check(inventory(alice_id) == sorted(SIGNUP_SKINS), "inventory rows: %s" % inventory(alice_id))
        p0 = premium(alice_id)
        check(p0 is not None and p0 >= DAY_MS, "player_premium + 24 h of ms: %s" % p0)
        row = query("SELECT skins FROM signup_rewards WHERE account_id = %d" % alice_id)
        check(row and row[0] == ",".join(str(x) for x in SIGNUP_SKINS), "signup_rewards row: %s" % row)

    print("== never twice: a token login, a password login, a second socket")
    async with Social(social) as s2:
        await login(s2, alice, token)
        await s2.drain(1.0)
        check(s2.take("prr") is None, "no prr at the next login")
    async with Social(social) as s3:
        r = await s3.call("a", "a", 1, [alice, "rewardpass1", None])
        await s3.drain(1.0)
        check(r[1] is None and s3.take("prr") is None, "no prr after a password login")
    check(inventory(alice_id) == sorted(SIGNUP_SKINS) and premium(alice_id) == p0, "the inventory and premium unchanged")

    print("== accounts older than the perks get nothing")
    async with Social(social) as so:
        old_id = None
        r = await so.call("a", "a", 0, [old, "rewardpass1", 0], "captcha")
        old_id = r[2]
        await so.drain(0.5)
    # the second account is backdated before the epoch, its grant row removed: a login grants nothing
    query("DELETE FROM inventory WHERE account_id = %d" % old_id)
    query("DELETE FROM signup_rewards WHERE account_id = %d" % old_id)
    query("UPDATE accounts SET created_at = '2001-01-01 00:00:00' WHERE id = %d" % old_id)
    async with Social(social) as so:
        await so.call("a", "a", 1, [old, "rewardpass1", None])
        await so.drain(1.0)
        check(so.take("prr") is None and inventory(old_id) == [], "an account from before the epoch: no prr, no items")

    print("== the signup premium ends")
    query("UPDATE signup_rewards SET premium_until = '2001-01-01 00:00:00' WHERE account_id = %d" % alice_id)
    for _ in range(40):
        if premium(alice_id) == p0 - DAY_MS:
            break
        time.sleep(0.25)
    check(premium(alice_id) == p0 - DAY_MS, "premium taken back after premium_until: %s -> %s" % (p0, premium(alice_id)))
    sw = query("SELECT premium_swept FROM signup_rewards WHERE account_id = %d" % alice_id)
    check(sw == ["1"], "swept once: %s" % sw)

    async with Social(social) as s, Node(port) as n:
        await login(s, alice, token)
        print("== bpdl: the v10 fields")
        r = await s.call("bpdl", "bpdl")
        bp = r[1]
        check(bp and all(k in bp for k in ("leveldata", "krdata", "jnkdata")), "bpdl has leveldata / krdata / jnkdata")
        await s.call("bppd")
        per = bp["bpp_perlevel"]
        items = items_of(bp)
        free = sorted([it for it in items if it[1] == 0 and it[3] > 0], key=lambda it: it[2])
        print("== bpclmAll: every claimable card")
        s.inbox.clear()
        query("UPDATE battlepass_progress SET bpp = %d WHERE account_id = %d" % (per * 4 + 10, alice_id))  # level 5
        r = await s.call("bpclmr", "bpclmAll")
        got = sorted(c["id"] for c in r[6]) if isinstance(r[6], list) else None
        want = sorted(it[0] for it in free if it[2] <= 4)
        check(got == want, "claimItemRes(_, funds, _, _, junk, items): the free cards of levels 1-5: %s / %s" % (got, want))
        check(all(set(c) >= {"id", "tier", "col", "sk", "kr", "jnk"} for c in r[6]), "v10 item keys")
        pr = await s.call("bppd")
        check(sorted(int(x) for x in pr[1]["items_claimed"].split(",")) == want, "items_claimed: %s" % pr[1]["items_claimed"])
        inv = inventory(alice_id)
        check(all(it[3] in inv for it in free if it[2] <= 4), "the cards' skins in the inventory")
        r = await s.call("po", "bpclmAll")
        check(r[2] == "No items to claim", "claim all again: %s" % r)
        r = await s.call("po", "bpclm", free[0][0])
        check(r[2] == "Item already claimed", "single claim of a claimed card: %s" % r)

        print("== krdata: a KR card")
        kr_card = free[5]
        query("UPDATE battlepasses SET krdata = '%d:150', leveldata = '7:1000' WHERE bp_id = 2" % kr_card[0])
        query("UPDATE battlepass_progress SET bpp = %d WHERE account_id = %d" % (per * 5 + 1, alice_id))  # level 6
        r = await s.call("bpdl", "bpdl")
        check(r[1]["krdata"] == "%d:150" % kr_card[0] and r[1]["leveldata"] == "7:1000", "bpdl carries the new krdata / leveldata")
        await s.call("bppd")
        f0 = (await login(s, alice, token))[4][5]
        await s.drain(0.3)
        s.inbox.clear()
        r = await s.call("bpclmr", "bpclmAll")
        check(isinstance(r[6], list) and len(r[6]) == 1 and r[6][0]["kr"] == 150 and r[2] == f0 + 150,
              "the level 6 card pays 150 KR: %s" % r[1:])
        tx = query("SELECT amount FROM kr_transactions WHERE account_id = %d AND source = 'battlepass'" % alice_id)
        check(tx == ["150"], "kr_transactions: the card's KR: %s" % tx)
        print("== leveldata: level 7 costs 1000 BPP")
        # levels 1-6: 300 each (1800), level 7: 1000 -> level 8 starts at 2800 (uniform: 2100)
        query("UPDATE battlepass_progress SET bpp = %d WHERE account_id = %d" % (per * 6 + 999, alice_id))
        r = await s.call("po", "bpclm", free[7][0])
        check(r[2] == "Level not reached", "2799 BPP: still level 7, the level 8 card waits: %s" % r)
        query("UPDATE battlepass_progress SET bpp = %d WHERE account_id = %d" % (per * 6 + 1000, alice_id))
        r = await s.call("bpclmr", "bpclm", free[7][0])
        check(r[3] == free[7][3], "level 8 reached at 2800 BPP: %s" % r)
        query("UPDATE battlepasses SET krdata = '', leveldata = '' WHERE bp_id = 2")

    print("== the VIP tier gifted (v10 selectBPGift)")
    async with Social(social) as a, Social(social) as b, Node(port) as n:
        await login(a, alice, token)
        bob_id, _, _ = await register(b, bob)
        await a.drain(0.5)
        await b.drain(0.5)
        a.inbox.clear()
        b.inbox.clear()
        price = bp["tier1price"]
        r = await b.call("purbp", "xstbp", 1, "en", None, None, None, "nobody" + stamp)
        check(r[2] == {"err": "Player not found"}, "gift to an unknown player: %s" % r)
        r = await b.call("purbp", "xstbp", 1, "en", None, None, None, bob)
        check(r[2] == {"err": "You can't gift yourself"}, "gift to yourself: %s" % r)
        r = await b.call("purbp", "xstbp", 1, "en", None, None, None, alice)
        check(r[2] == {"err": "Not enough KR"}, "a gift without the KR: %s" % r)
        await give_kr(n, bob_id, price)
        r = await b.call("purbp", "xstbp", 1, "en", None, None, None, alice)
        check(r[2] is None and r[10] is True and r[11] == alice and r[5] == price, "purchaseRes(.., gifted, name): %s" % r)
        pr = await wait_push(a, "bppd")
        check(pr and pr[1]["tier"] == 1, "the recipient's bppd: tier 1: %s" % (pr and pr[1]))
        t = query("SELECT tier FROM battlepass_progress WHERE account_id = %d AND bp_id = 2" % bob_id)
        check(t in (["0"], []), "the payer's own pass is unchanged: %s" % t)
        tx = query("SELECT amount FROM kr_transactions WHERE account_id = %d AND source = 'battlepass-gift'" % bob_id)
        check(tx == [str(-price)], "kr_transactions: the gift's debit: %s" % tx)
        mail = query("SELECT subject FROM mail WHERE account_id = %d" % alice_id)
        check("Battle Pass Gift" in (mail or []), "the recipient's mail: %s" % mail)
        r = await b.call("purbp", "xstbp", 1, "en", None, None, None, alice)
        check(r[2] == {"err": "This player already owns this Battle Pass"}, "a second gift: %s" % r)


def start_brain(env, log_path, extra):
    with open(log_path, "a") as log:
        log.write("== start %s\n" % extra)
        # the pass checks are the Season 7 pass's (its cards, bp_id 2): the brain's default season is 10 (0355_season_10)
        proc = subprocess.Popen([BRAIN, "--brain.host=127.0.0.1", "--brain.port=0", "--brain.id=krw-%d" % os.getpid(),
                                 "--accounts.signup_rewards.sweep_s=1", "--accounts.login_throttle_s=0", "--accounts.current_season=7"] + extra,
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
            print("rewards test: %s unavailable: skipped" % DRIVER)
            sys.exit(r.returncode)
        DBCFG = json.loads(r.stdout)
        dbenv = {"KN_DATABASE_" + k.upper(): str(v) for k, v in DBCFG.items()}
        drop = scratch + ["drop", DRIVER, SCHEMA]
    work = tempfile.mkdtemp(prefix="kn-rewards.", dir=os.path.join(os.path.expanduser("~"), ".cache"))
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
    print("rewards test: " + ("OK" if not FAILS else "%d failure(s)" % len(FAILS)))
    if FAILS:
        print("log kept in " + work)
    else:
        shutil.rmtree(work, ignore_errors=True)
    sys.exit(1 if FAILS else 0)


main()
