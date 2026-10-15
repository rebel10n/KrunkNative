#!/usr/bin/env python3
"""KrunkScript storage + map KR rewards for game servers (doc/v7/krunkscript.md 2.6, WP6;
doc/backend.md 7.6 ops "storage", "storage_load", "map_reward"; services/brain/db_storage.c).

  storage_test.py <kn_brain> <repo root> [database]
Database: argument, else $KN_MM_TEST_DATABASE, else config/config.json's database.name; every
run uses its own schema (kn_test_kss_<pid>, dropped after) and an ephemeral port. 77 = skip
(no PostgreSQL / python websockets). KN_TEST_DB_DRIVER=sqlite|mysql runs on that store driver
(db_scratch.py).

Two accounts register over the social socket and publish maps (alice: two, bob: one); a fake
game server on /mm/internal/ws then drives the ops: the six operation types, the touched rows
in the replies, validation, published-map and account checks, load of this map / another map
of the same creator (global rows only) / another creator's map (nothing), unknown accounts,
and map_reward out of the map's funds.
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
    print("storage test: no database configured; skipped")
    sys.exit(77)
SCHEMA = "kn_test_kss_%d" % os.getpid()
SECRET = "kn-storage-test-%d" % os.getpid()
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
        deadline = time.time() + 10
        while True:
            for i, f in enumerate(self.inbox):
                if f[0] == label:
                    return self.inbox.pop(i)
            self.inbox.append(json.loads(await asyncio.wait_for(self.ws.recv(), deadline - time.time())))


class Node:
    """Game server side of /mm/internal/ws: "acct" ops only."""

    def __init__(self, port):
        self.url = "ws://127.0.0.1:%d/mm/internal/ws" % port
        self.rid = 0

    async def __aenter__(self):
        self.ws = await websockets.connect(self.url, additional_headers={"Authorization": "Bearer " + SECRET})
        await self.ws.send(json.dumps({"type": "hello", "nodeId": "KSS", "region": "local", "publicHost": "127.0.0.1",
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


def fund_map(dbcfg, name, kr):
    """published_maps.map_funds = kr (no route funds a map yet). False when not possible here."""
    sql = "UPDATE published_maps SET map_funds = %d WHERE map_name = '%s'" % (kr, name)
    if DRIVER == "sqlite":
        con = sqlite3.connect(dbcfg["path"])
        con.execute(sql)
        con.commit()
        con.close()
        return True
    if DRIVER == "postgres":
        get = lambda k, d: os.environ.get("KN_DATABASE_" + k.upper(), str(CONFIG_DB.get(k, d)))
        env = dict(os.environ, PGPASSWORD=get("password", ""))
        r = subprocess.run(["psql", "-h", get("host", "127.0.0.1"), "-p", get("port", 5432), "-U", get("user", "krunknative"),
                            "-d", DB, "-v", "ON_ERROR_STOP=1", "-qAt", "-c", "SET search_path TO %s; %s" % (SCHEMA, sql)],
                           env=env, capture_output=True, timeout=30)
        return r.returncode == 0
    return False


async def register(sock, name):
    r = await sock.call("a", "a", 0, [name, "storagepass1", 0], "captcha")
    assert r[1] is None, r
    return r[2]


async def publish(sock, name):
    # validateMap (editor publish, creations.c): a name, objects and two spawns
    mapjson = json.dumps({"name": name, "objects": [{"p": [0, 0, 0], "s": [10, 1, 10]}], "spawns": [[0, 1, 0], [4, 1, 0]]})
    r = await sock.call("pubMR", "uploadM", mapjson, None, "storage test", "", 2)
    return r[1] == "Map Published!"


async def scenario(port, dbcfg):
    stamp = str(int(time.time()) % 100000)
    alice, bob = "kssa" + stamp, "kssb" + stamp
    map_a, map_b, map_c = "KsStoreA_" + stamp, "KsStoreB_" + stamp, "KsStoreC_" + stamp
    social = "ws://127.0.0.1:%d/social/ws" % port
    async with Social(social) as s:
        alice_id = await register(s, alice)
        check(await publish(s, map_a) and await publish(s, map_b), "alice publishes two maps")
    async with Social(social) as s:
        bob_id = await register(s, bob)
        check(await publish(s, map_c), "bob publishes a map")

    async with Node(port) as n:
        def rows(data):
            return (data or {}).get("rows")

        print("== operation types")
        err, data = await n.op("storage", id=alice_id, name=alice, map=map_a, type=0, scope=0,
                               data=[["V_coins", "5"], ["V_gems", "abc"]])
        check(err is None and rows(data) == [["V_coins", "5"], ["V_gems", "abc"]], "set: %s %s" % (err, data))
        err, data = await n.op("storage", id=alice_id, name=alice, map=map_a, type=3, scope=0, data=[["V_coins", "2"]])
        check(rows(data) == [["V_coins", "7"]], "increase: %s" % data)
        err, data = await n.op("storage", id=alice_id, name=alice, map=map_a, type=1, scope=0, data=[["V_coins", "6"]])
        check(rows(data) == [["V_coins", "7"]], "set if higher keeps 7: %s" % data)
        err, data = await n.op("storage", id=alice_id, name=alice, map=map_a, type=1, scope=0, data=[["V_coins", "10"]])
        check(rows(data) == [["V_coins", "10"]], "set if higher: %s" % data)
        err, data = await n.op("storage", id=alice_id, name=alice, map=map_a, type=2, scope=0, data=[["V_coins", "3"]])
        check(rows(data) == [["V_coins", "3"]], "set if lower: %s" % data)
        err, data = await n.op("storage", id=alice_id, name=alice, map=map_a, type=4, scope=0, data=[["V_coins", "0.5"]])
        check(rows(data) == [["V_coins", "2.5"]], "decrease (JS number format): %s" % data)
        err, data = await n.op("storage", id=alice_id, name=alice, map=map_a, type=5, scope=0, data=[["V_gems", ""]])
        check(err is None and rows(data) == [], "delete: %s %s" % (err, data))
        err, data = await n.op("storage", id=0, name=alice.upper(), map=map_a.lower(), type=3, scope=0, data=[["trig", "7"]])
        check(err is None and rows(data) == [["trig", "7"]], "account by name, map name case-insensitive: %s %s" % (err, data))

        print("== load")
        err, data = await n.op("storage_load", name=alice, map=map_a)
        # ORDER BY skey follows the database's collation; the game server sorts bytewise itself
        check(err is None and sorted(rows(data)) == [["V_coins", "2.5"], ["trig", "7"]], "load this map: %s" % data)
        err, data = await n.op("storage", id=alice_id, name=alice, map=map_a, type=0, scope=1, data=[["V_shared", "s"]])
        err, data = await n.op("storage_load", name=alice, map=map_b, **{"from": map_a})
        check(err is None and rows(data) == [["V_shared", "s"]], "same creator's other map: global rows only: %s" % data)
        err, data = await n.op("storage_load", name=alice, map=map_c, **{"from": map_a})
        check(err is None and rows(data) == [], "another creator's map: nothing: %s" % data)
        err, data = await n.op("storage_load", name="nobody_" + stamp, map=map_a)
        check(err is None and rows(data) == [], "unknown account: no rows: %s %s" % (err, data))
        err, data = await n.op("storage_load", name=bob, map=map_a)
        check(err is None and rows(data) == [], "rows are per account")

        print("== refusals")
        err, data = await n.op("storage", id=alice_id, name=alice, map="NotPublished_" + stamp, type=0, scope=0, data=[["k", "v"]])
        check(err == "Storage only supported on published maps", "unpublished map: %s" % err)
        err, data = await n.op("storage_load", name=alice, map="NotPublished_" + stamp)
        check(err == "Storage only supported on published maps", "load on an unpublished map: %s" % err)
        err, data = await n.op("storage", id=alice_id, name=alice, map=map_a, type=0, scope=0, data=[["V_abcdefghijklmnopqrs", "1"]])
        check(err == "Key too long", "Key too long: %s" % err)
        err, data = await n.op("storage", id=alice_id, name=alice, map=map_a, type=0, scope=0, data=[["k", "x" * 101]])
        check(err == "Value too long", "Value too long: %s" % err)
        err, data = await n.op("storage", id=alice_id, name=alice, map=map_a, type=0, scope=0, data=[[str(i), "1"] for i in range(31)])
        check(err == "Too many keys: max 30", "Too many keys: %s" % err)
        err, data = await n.op("storage", id=alice_id, name=alice, map=map_a, type=6, scope=0, data=[["k", "1"]])
        check(err == "Invalid storage type", "Invalid storage type: %s" % err)
        err, data = await n.op("storage", id=0, name="nobody_" + stamp, map=map_a, type=0, scope=0, data=[["k", "1"]])
        check(err == "Player not connected", "unknown account: %s" % err)

        print("== map_reward")
        err, data = await n.op("map_reward", id=bob_id, amount=5, map=map_a)
        check(err is None and data.get("rsp") == "Not enough map funds", "no map funds: %s %s" % (err, data))
        if fund_map(dbcfg, map_a, 100):
            err, data = await n.op("map_reward", id=bob_id, amount=5, map=map_a)
            check(err is None and data.get("rsp") == "Success" and data.get("funds", -1) >= 5, "reward paid: %s %s" % (err, data))
            funds = data.get("funds", 0)
            err, data = await n.op("map_reward", id=bob_id, amount=95, map=map_a)
            check(data.get("rsp") == "Success" and data.get("funds") == funds + 95, "the rest of the funds: %s" % data)
            err, data = await n.op("map_reward", id=bob_id, amount=1, map=map_a)
            check(data.get("rsp") == "Not enough map funds", "the map ran dry: %s" % data)
        else:
            print("  (map funding not available on %s: Success path skipped)" % DRIVER)
        err, data = await n.op("map_reward", id=bob_id, amount=5, map="NotPublished_" + stamp)
        check(data.get("rsp") != "Success", "unpublished map: no reward (%s)" % data)


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
            print("storage test: %s unavailable: skipped" % DRIVER)
            sys.exit(r.returncode)
        dbcfg = json.loads(r.stdout)
        dbenv = {"KN_DATABASE_" + k.upper(): str(v) for k, v in dbcfg.items()}
        drop = scratch + ["drop", DRIVER, SCHEMA]
    work = tempfile.mkdtemp(prefix="kn-storage.")
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=SECRET, **dbenv)
    log_path = os.path.join(work, "brain.log")
    proc = None
    try:
        subprocess.run([BRAIN, "--migrate-only"], cwd=ROOT, env=env, check=True, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)
        with open(log_path, "w") as log:
            proc = subprocess.Popen([BRAIN, "--brain.host=127.0.0.1", "--brain.port=0", "--brain.id=kss-%d" % os.getpid()],
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
    print("storage test: " + ("OK" if not FAILS else "%d failure(s)" % len(FAILS)))
    if FAILS:
        print("log kept in " + work)
    else:
        shutil.rmtree(work, ignore_errors=True)
    sys.exit(1 if FAILS else 0)


main()
