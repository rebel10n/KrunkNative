#!/usr/bin/env python3
"""The v10 lottery on kn_brain (services/brain/db_lottery.c, lottery.c; migration 0348_lottery).

  lottery_test.py <kn_brain> <repo root> [database]
Database: argument, else $KN_MM_TEST_DATABASE, else config/config.json's database.name; every
run uses its own schema and an ephemeral port. 77 = skip (no PostgreSQL / python websockets).
KN_TEST_DB_DRIVER=sqlite|mysql runs on that store driver (db_scratch.py).

Covers: the guest view of the seeded lottery 3 (costs, no open window while the event is closed,
"lottery not found" for an unknown id); a refused entry while closed; the event opened by the test
(active = 1, the open window with its end); the refusals (a token needed, zero bundles, a cost the
lottery does not have, "Not enough Junk" with nothing written); a skin entry (the skin removed, the
entries added); a Junk entry (the Junk spent through junk_spend with reason "lottery", the ledger row)
and an RP entry; the view with a token (the account's own entries); and the settlement: an open window
moved into the past is drawn once by the timer, its prizes are the seeded pools' winner counts, each
prize is a skin of its pool and goes to an entry holder, and a second pass draws nothing again.
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
    with open(os.path.join(ROOT, "config", "config.json")) as fp:
        CONFIG_DB = json.load(fp).get("database", {})
except (OSError, ValueError):
    pass
if not DB:
    DB = CONFIG_DB.get("name", "")
if not DB and DRIVER == "postgres":
    print("lottery test: no database configured; skipped")
    sys.exit(77)
SCHEMA = "kn_test_lottery_%d" % os.getpid()
SECRET = "kn-lottery-test-%d" % os.getpid()
FAILS = []
DBCFG = {}
LOTTERY = 3


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
        self.node_id = "KLT%d" % Node.NODES[0]

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
    r = await sock.call("a", "a", 0, [name, "lotterypass1", 0], "captcha")
    assert r[1] is None, r
    return r[2], r[5]


def http(port, method, path, body=None, token=None):
    """(status, json) of one request to the brain's HTTP port."""
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


def err_of(body):
    return body.get("error") if isinstance(body, dict) else None


async def scenario(port):
    stamp = str(int(time.time()) % 100000)
    alice, bob = "lota" + stamp, "lotb" + stamp
    social = "ws://127.0.0.1:%d/social/ws" % port
    async with Social(social) as sock_a, Social(social) as sock_b, Node(port) as node:
        a_id, a_tok = await register(sock_a, alice)
        b_id, b_tok = await register(sock_b, bob)
        check(a_id > 0 and b_id > 0, "two accounts registered")
        base = "/api/lottery/%d" % LOTTERY

        status, body = http(port, "GET", base)
        data = body.get("data", {}) if isinstance(body, dict) else {}
        costs = data.get("costs", [])
        check(status == 200 and data.get("lottery", {}).get("title") == "Anniversary"
              and data["lottery"].get("frequency") == 12, "the guest view of lottery 3 is the seeded Anniversary raffle: %s" % status)
        check(costs[:1] == [{"cost_type": "skin", "cost_ref": "7782", "cost_quantity": 1, "num_entries": 1}],
              "the view lists the skin cost of 1 entry: %s" % costs[:1])
        check(data.get("event_ended") is True and "current_draw" not in data,
              "a closed event has no open window (event_ended true, no current_draw)")
        check(data.get("previous_draws") == [], "a fresh lottery has no previous draws")

        status, body = http(port, "GET", "/api/lottery/99")
        check(status == 400 and err_of(body) == "lottery not found", "an unknown lottery is 400 'lottery not found': %s %s" % (status, body))

        status, body = http(port, "POST", base + "/participate", {"cost_type": "skin", "cost_ref": "7782", "cost_quantity": 1})
        check(status == 401 and err_of(body) == "LoginRequired", "an entry without a token is refused: %s %s" % (status, body))
        status, body = http(port, "POST", base + "/participate", {"cost_type": "skin", "cost_ref": "7782", "cost_quantity": 1}, a_tok)
        check(status == 400 and "ended" in (err_of(body) or ""), "a closed event refuses an entry: %s %s" % (status, body))

        # open the event: the window that ends next is the open draw
        query("UPDATE lotteries SET active = 1 WHERE lottery_id = %d" % LOTTERY)
        status, body = http(port, "GET", base)
        cur = body.get("data", {}).get("current_draw", {}) if isinstance(body, dict) else {}
        check(status == 200 and body["data"].get("event_ended") is False and cur.get("ends_at", "").endswith("Z")
              and cur.get("player_entries") == 0, "an open event shows its window (ends_at, no entries yet): %s" % cur)

        status, body = http(port, "POST", base + "/participate", {"cost_type": "skin", "cost_ref": "7782", "cost_quantity": 0}, a_tok)
        check(status == 400 and err_of(body) == "Please select at least one item to participate", "zero bundles are refused: %s" % body)
        status, body = http(port, "POST", base + "/participate", {"cost_type": "skin", "cost_ref": "1", "cost_quantity": 1}, a_tok)
        check(status == 400 and err_of(body) == "Invalid cost", "a cost the lottery does not have is refused: %s" % body)

        # a skin entry: two bundles of the 7782 ticket (one entry each); the skins go
        for _ in range(2):
            err, _data = await node.op("give_skin", id=a_id, skin=7782, source="test")
            assert err is None, err
        status, body = http(port, "POST", base + "/participate", {"cost_type": "skin", "cost_ref": "7782", "cost_quantity": 2}, a_tok)
        check(status == 200 and body.get("data") == {"player_entries": 2, "total_entries": 2},
              "two skin bundles give 2 entries: %s %s" % (status, body))
        check(one("SELECT COALESCE(SUM(delta), 0) FROM skin_transactions WHERE account_id = %d AND skin_id = 7782" % a_id) == 0,
              "the two skins are spent (the account holds none)")
        status, body = http(port, "POST", base + "/participate", {"cost_type": "skin", "cost_ref": "7782", "cost_quantity": 1}, a_tok)
        check(status == 400 and err_of(body) == "Item not owned", "a skin the account does not own is refused: %s" % body)

        # a Junk entry: 5.00 Junk a bundle, 10 entries (the cost row is the test's, the lottery's own shape)
        query("INSERT INTO lottery_costs (lottery_id, ord, cost_type, cost_ref, cost_quantity, num_entries)"
              " VALUES (%d, 10, 'currency', '2', 500, 10)" % LOTTERY)
        err, _ = await node.op("junk_credit", id=a_id, amount=1000, reason="trigger")
        assert err is None, err
        # cost_quantity is the units paid, a multiple of the cost's (v10.0.3: the stepper moves by cost_quantity and the
        # menu takes the selected units off the balance, deob-10.0.3.js:142871, 142699, 142917)
        status, body = http(port, "POST", base + "/participate", {"cost_type": "currency", "cost_ref": "2", "cost_quantity": 750}, a_tok)
        check(status == 400 and err_of(body) == "Invalid cost", "units that are not a multiple of the cost's (500) are refused: %s %s" % (status, body))
        status, body = http(port, "POST", base + "/participate", {"cost_type": "currency", "cost_ref": "2", "cost_quantity": 1000}, a_tok)
        check(status == 200 and body.get("data") == {"player_entries": 22, "total_entries": 22},
              "two Junk bundles (1000 hundredths, 10.00) give 20 entries: %s %s" % (status, body))
        check(one("SELECT player_junk FROM players WHERE player_id = %d" % a_id) == 0, "the Junk is spent (balance 0)")
        check(one("SELECT COUNT(*) FROM junk_transactions WHERE account_id = %d AND source = 'lottery' AND amount = -1000" % a_id) == 1,
              "one ledger row of -1000 with reason 'lottery' (junk_spend)")
        status, body = http(port, "POST", base + "/participate", {"cost_type": "currency", "cost_ref": "2", "cost_quantity": 500}, a_tok)
        check(status == 400 and err_of(body) == "Not enough Junk", "an unaffordable Junk entry is refused: %s %s" % (status, body))
        check(one("SELECT entries FROM lottery_entries e JOIN lottery_draws d ON d.id = e.draw_id WHERE e.account_id = %d AND d.lottery_id = %d"
                  % (a_id, LOTTERY)) == 22, "the refused entry adds nothing (still 22 entries)")

        # the view with a token: the account's own entries and the window's total
        status, body = http(port, "GET", base, token=a_tok)
        check(status == 200 and body["data"].get("player_entries") == 22 and body["data"].get("total_entries") == 22,
              "the view with a token reads the account's own entries: %s" % (body.get("data") if isinstance(body, dict) else body))

        # a second account: Junk (10 entries) and Ranked Points (5 entries per bundle)
        err, _ = await node.op("junk_credit", id=b_id, amount=500, reason="trigger")
        assert err is None, err
        status, body = http(port, "POST", base + "/participate", {"cost_type": "currency", "cost_ref": "2", "cost_quantity": 500}, b_tok)
        check(status == 200 and body.get("data", {}).get("player_entries") == 10 and body["data"].get("total_entries") == 32,
              "a second account's Junk entry: 10 of 32 entries: %s" % body)
        query("INSERT INTO lottery_costs (lottery_id, ord, cost_type, cost_ref, cost_quantity, num_entries)"
              " VALUES (%d, 11, 'currency', '3', 50, 5)" % LOTTERY)
        query("INSERT INTO ranked_points (account_id, balance) VALUES (%d, 100)" % b_id)
        status, body = http(port, "POST", base + "/participate", {"cost_type": "currency", "cost_ref": "3", "cost_quantity": 50}, b_tok)
        check(status == 200 and body.get("data") == {"player_entries": 15, "total_entries": 37},
              "an RP entry of 0.50 RP's worth (50 RP, 5 entries): %s %s" % (status, body))
        check(one("SELECT balance FROM ranked_points WHERE account_id = %d" % b_id) == 50, "the RP is spent (balance 50)")

        # settlement: the open window moves into the past; the timer draws it once
        query("UPDATE lottery_draws SET starts_s = starts_s - 200000, ends_s = ends_s - 200000"
              " WHERE lottery_id = %d AND drawn_s IS NULL" % LOTTERY)
        deadline = time.time() + 30
        while time.time() < deadline and one("SELECT COUNT(*) FROM lottery_prizes", 0) < 10:
            time.sleep(0.5)
        prizes_n = one("SELECT COUNT(*) FROM lottery_prizes", 0)
        check(prizes_n == 10, "the draw pays the seeded winners (1 + 9 = 10 prizes): %d" % prizes_n)
        status, body = http(port, "GET", base)
        drawn = body.get("data", {}).get("previous_draws", []) if isinstance(body, dict) else []
        check(len(drawn) == 1 and drawn[0].get("total_entries") == 37 and len(drawn[0].get("prizes", [])) == 10,
              "the previous draw holds 37 entries and its 10 prizes: %s" % [(d.get("total_entries"), len(d.get("prizes", []))) for d in drawn])
        pools = {}
        for pool in query("SELECT DISTINCT pool_id FROM lottery_pool_skins WHERE lottery_id = %d" % LOTTERY):
            pools[int(pool)] = {int(s) for s in query("SELECT skin_id FROM lottery_pool_skins WHERE lottery_id = %d AND pool_id = %s"
                                                       % (LOTTERY, pool))}
        prizes = drawn[0].get("prizes", []) if drawn else []
        check(all(p.get("prize_type") == "skin" and int(p.get("prize_ref")) in pools.get(p.get("prize_pool_id"), set()) for p in prizes),
              "every prize is a skin of its own pool")
        check(all(p.get("won_by_name") in (alice, bob) for p in prizes), "every winner is an account with entries")
        check(one("SELECT COUNT(*) FROM skin_transactions WHERE source = 'lottery' AND delta = 1") == 10,
              "each prize is granted once as a skin (10 grant rows, source 'lottery')")
        await asyncio.sleep(3)
        check(one("SELECT COUNT(*) FROM lottery_prizes", 0) == 10 and one("SELECT COUNT(*) FROM lottery_draws WHERE drawn_s IS NOT NULL") == 1,
              "a later pass draws nothing again (still one draw, 10 prizes)")


def start_brain(env, log_path, extra):
    with open(log_path, "a") as log:
        log.write("== start %s\n" % extra)
        proc = subprocess.Popen([BRAIN, "--brain.host=127.0.0.1", "--brain.port=0", "--brain.id=klt-%d" % os.getpid(),
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
            print("lottery test: %s unavailable: skipped" % DRIVER)
            sys.exit(r.returncode)
        DBCFG = json.loads(r.stdout)
        dbenv = {"KN_DATABASE_" + k.upper(): str(v) for k, v in DBCFG.items()}
        drop = scratch + ["drop", DRIVER, SCHEMA]
    work = tempfile.mkdtemp(prefix="kn-lottery.", dir=os.path.join(os.path.expanduser("~"), ".cache"))
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=SECRET, **dbenv)
    log_path = os.path.join(work, "brain.log")
    proc = None
    try:
        subprocess.run([BRAIN, "--migrate-only"], cwd=ROOT, env=env, check=True, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)
        proc, port = start_brain(env, log_path, ["--brain.lottery.settle_check_ms=1000"])
        asyncio.run(scenario(port))
    finally:
        if proc:
            proc.terminate()
            proc.wait(5)
        subprocess.run(drop)
    with open(log_path) as f:
        bad = [l.strip() for l in f if re.search(r"failed:|ERROR|FATAL", l)]
    check(not bad, "no statement errors in the brain log %s" % bad[:3])
    print("lottery test: " + ("OK" if not FAILS else "%d failure(s)" % len(FAILS)))
    if FAILS:
        print("log kept in " + work)
    else:
        shutil.rmtree(work, ignore_errors=True)
    sys.exit(1 if FAILS else 0)


main()
