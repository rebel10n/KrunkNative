#!/usr/bin/env python3
"""Event kill pools on kn_brain (doc/v10/kill-pools.md; services/brain/community.c "gameEvents",
internal_ws.c "welcome" / "game-events", accounts.c op "stats" itemRewards, db_accounts.c give_skin_n).

  kill_pools_test.py <kn_brain> <repo root> [database]
Database: argument, else $KN_MM_TEST_DATABASE, else config/config.json's database.name; every
run uses its own schema and an ephemeral port. 77 = skip (no PostgreSQL / python websockets).
KN_TEST_DB_DRIVER=sqlite|mysql runs on that store driver (db_scratch.py).

Covers: a game server's welcome carries the feed's game events running now (not a future or an ended
one), in v10's fields; editing the feed pushes "game-events" with the new set at the next ping; a final
commit with itemRewards [{id, quantity}] adds them to the inventory (skin_transactions source
"killpool"); the same match committed again adds nothing; a commit's items stop at 30; an unknown skin
or a bad quantity is skipped.
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
    print("kill pools test: no database configured; skipped")
    sys.exit(77)
SCHEMA = "kn_test_killpools_%d" % os.getpid()
SECRET = "kn-killpools-test-%d" % os.getpid()
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


class Node:
    """Game server side of /mm/internal/ws: the welcome, "game-events" pushes and commits."""

    def __init__(self, port):
        self.url = "ws://127.0.0.1:%d/mm/internal/ws" % port
        self.rid = 0
        self.welcome = None
        self.pushes = []

    async def __aenter__(self):
        self.ws = await websockets.connect(self.url, additional_headers={"Authorization": "Bearer " + SECRET})
        await self.ws.send(json.dumps({"type": "hello", "nodeId": "KKP%d" % os.getpid(), "region": "local", "publicHost": "127.0.0.1",
                                       "publicPort": 1, "version": "test"}))
        while True:
            msg = json.loads(await asyncio.wait_for(self.ws.recv(), 10))
            if msg.get("type") == "welcome":
                self.welcome = msg
                return self

    async def __aexit__(self, *a):
        await self.ws.close()

    def note(self, msg):
        if msg.get("type") == "game-events":
            self.pushes.append(msg)

    async def wait_push(self, seconds):
        deadline = time.time() + seconds
        while time.time() < deadline and not self.pushes:
            try:
                self.note(json.loads(await asyncio.wait_for(self.ws.recv(), deadline - time.time())))
            except asyncio.TimeoutError:
                break
        return self.pushes.pop(0) if self.pushes else None

    async def commit(self, match, player):
        self.rid += 1
        rid = self.rid
        await self.ws.send(json.dumps({"type": "commit", "rid": rid, "lobbyId": "kp1", "matchId": match, "final": True,
                                       "map": "Burg", "mode": 0, "custom": False, "players": [player]}))
        while True:
            msg = json.loads(await asyncio.wait_for(self.ws.recv(), 15))
            self.note(msg)
            if msg.get("type") == "commit-reply" and msg.get("rid") == rid:
                return msg


def iso(t):
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(t))


def event(eid, start, end, icon="gift"):
    return {"id": eid, "name": "Event %d" % eid, "startsAt": iso(start), "endsAt": iso(end), "killPoolsWeight": 50,
            "killPoolsMode": "dilute", "killPoolsIcon": icon,
            "killPools": [{"weight": 100, "contents": [{"content_type": "skin", "content_ref": "5"}]}]}


def write_feed(path, events):
    with open(path + ".tmp", "w") as fp:
        json.dump({"news": [], "events": [], "polls": [], "gameEvents": events}, fp)
    os.replace(path + ".tmp", path)


def count(account_id, skin):
    rows = query("SELECT count FROM inventory WHERE account_id = %d AND skin_id = %d" % (account_id, skin))
    return int(rows[0]) if rows else 0


async def scenario(port, feed):
    now = time.time()
    async with Node(port) as node:
        got = node.welcome.get("gameEvents")
        ids = [e.get("id") for e in got] if isinstance(got, list) else None
        check(ids == [1], "the welcome carries the running event only (not the future or the ended one): %s" % ids)
        e = got[0] if ids else {}
        check(e.get("killPoolsWeight") == 50 and e.get("killPoolsMode") == "dilute" and e.get("killPoolsIcon") == "gift" and
              e.get("killPools", [{}])[0].get("contents", [{}])[0].get("content_ref") == "5" and e.get("startsAt", "").endswith("Z"),
              "in v10's fields (startsAt, killPools, killPoolsWeight / Mode / Icon): %s" % e)

        # a second event starts (the feed is edited): the next ping pushes the new set
        time.sleep(1.1)  # the feed's mtime has a 1 s grain
        write_feed(feed, [event(1, now - 3600, now + 3600), event(4, now - 60, now + 3600, "bag"), event(2, now + 3600, now + 7200)])
        pids = None
        deadline = time.time() + 8
        while time.time() < deadline and pids != [1, 4]:  # the first ping may still push the welcome's set
            push = await node.wait_push(deadline - time.time())
            pids = [x.get("id") for x in push.get("events", [])] if push else pids
        check(pids == [1, 4], "a \"game-events\" push with both running events, soonest start first: %s" % pids)

        async with Social("ws://127.0.0.1:%d/social/ws" % port) as sock:
            name = "kpool" + str(int(now) % 100000)
            r = await sock.call("a", "a", 0, [name, "killpass1", 0], "captcha")
            account = r[2]
            check(isinstance(account, int) and account > 0, "an account registered")

            player = {"accountId": account, "name": name, "kills": 4, "deaths": 1, "score": 400, "won": False, "timePlayed": 60000,
                      "games": 1, "kr": 0, "xp": 400, "classIndex": 0, "itemRewards": [{"id": 5, "quantity": 3}, {"id": 7, "quantity": 1}]}
            reply = await node.commit("m1", player)
            check(reply.get("ok"), "the commit is accepted: %s" % reply)
            for _ in range(50):
                if count(account, 5) == 3 and count(account, 7) == 1:
                    break
                await asyncio.sleep(0.1)
            check(count(account, 5) == 3 and count(account, 7) == 1, "itemRewards: 3 of skin 5, 1 of skin 7 in the inventory (%d, %d)" %
                  (count(account, 5), count(account, 7)))
            src = query("SELECT DISTINCT source FROM skin_transactions WHERE account_id = %d AND skin_id IN (5, 7)" % account)
            check(src == ["killpool"], "the audit rows of the items say killpool: %s" % src)

            await node.commit("m1", player)
            await asyncio.sleep(1.0)
            check(count(account, 5) == 3 and count(account, 7) == 1, "the same match again adds nothing")

            capped = dict(player, itemRewards=[{"id": 5, "quantity": 25}, {"id": 7, "quantity": 25}, {"id": 0, "quantity": 2},
                                                {"id": 9, "quantity": -1}])
            await node.commit("m2", capped)
            for _ in range(50):
                if count(account, 7) == 6:
                    break
                await asyncio.sleep(0.1)
            await asyncio.sleep(0.5)
            check(count(account, 5) == 28 and count(account, 7) == 6 and count(account, 9) == 0,
                  "a match's items stop at 30 (25 + 5), skin 0 and a negative quantity are skipped (%d, %d, %d)" %
                  (count(account, 5), count(account, 7), count(account, 9)))


def start_brain(env, log_path, extra):
    with open(log_path, "a") as log:
        log.write("== start %s\n" % extra)
        proc = subprocess.Popen([BRAIN, "--brain.host=127.0.0.1", "--brain.port=0", "--brain.id=kkp-%d" % os.getpid(),
                                 "--brain.ping_interval_s=1", "--accounts.login_throttle_s=0"] + extra,
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
            print("kill pools test: %s unavailable: skipped" % DRIVER)
            sys.exit(r.returncode)
        DBCFG = json.loads(r.stdout)
        dbenv = {"KN_DATABASE_" + k.upper(): str(v) for k, v in DBCFG.items()}
        drop = scratch + ["drop", DRIVER, SCHEMA]
    work = tempfile.mkdtemp(prefix="kn-killpools.", dir=os.path.join(os.path.expanduser("~"), ".cache"))
    feed = os.path.join(work, "community.json")
    now = time.time()
    write_feed(feed, [event(1, now - 3600, now + 3600), event(2, now + 3600, now + 7200), event(3, now - 7200, now - 3600)])
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=SECRET, **dbenv)
    log_path = os.path.join(work, "brain.log")
    proc = None
    try:
        subprocess.run([BRAIN, "--migrate-only"], cwd=ROOT, env=env, check=True, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)
        proc, port = start_brain(env, log_path, ["--brain.community.feed=" + feed])
        asyncio.run(scenario(port, feed))
    finally:
        if proc:
            proc.terminate()
            proc.wait(5)
        subprocess.run(drop)
    with open(log_path) as f:
        bad = [l.strip() for l in f if re.search(r"failed:|ERROR|FATAL", l)]
    check(not bad, "no statement errors in the brain log %s" % bad[:3])
    print("kill pools test: " + ("OK" if not FAILS else "%d failure(s)" % len(FAILS)))
    if FAILS:
        print("log kept in " + work)
    else:
        shutil.rmtree(work, ignore_errors=True)
    sys.exit(1 if FAILS else 0)


main()
