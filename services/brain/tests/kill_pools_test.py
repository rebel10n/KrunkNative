#!/usr/bin/env python3
"""Event kill pools on kn_brain (doc/v10/kill-pools.md; services/brain/community.c "gameEvents",
internal_ws.c "welcome" / "game-events", accounts.c op "stats" itemRewards, db_accounts.c give_skin_n).

  kill_pools_test.py <kn_brain> <repo root> [database]
Database: argument, else $KN_MM_TEST_DATABASE, else config/config.json's (or $KN_TEST_CONFIG's) database.name; every
run uses its own schema and an ephemeral port. 77 = skip (no PostgreSQL / python websockets).
KN_TEST_DB_DRIVER=sqlite|mysql runs on that store driver (db_scratch.py).

Covers: a game server's welcome carries the feed's game events running now (not a future or an ended
one), in v10's fields; editing the feed pushes "game-events" with the new set at the next ping; a final
commit with itemRewards [{id, quantity}] adds them to the inventory (skin_transactions source
"killpool"); the same match committed again adds nothing; a commit's items stop at 30; an unknown skin
or a bad quantity is skipped; a game server's named give_skin (ref) is granted once per ref (v10.0.7 lost raid drops); counter skins' skinStats
rows count on the copy they name (0406), a row without one on the index's first owned entry, and 0406 moves an
older brain's per-index counts onto that entry. 0408 moves a copy's counts to the account that owns the copy now (summed
into a count it has), into an open offer's escrowed copy (trade_copies.stats), and drops the counts of a copy that is
gone.
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
    with open(os.environ.get("KN_TEST_CONFIG", os.path.join(ROOT, "config", "config.json"))) as fp:
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


    async def acct(self, op, args):
        self.rid += 1
        rid = self.rid
        await self.ws.send(json.dumps({"type": "acct", "rid": rid, "op": op, "args": args}))
        while True:
            msg = json.loads(await asyncio.wait_for(self.ws.recv(), 15))
            self.note(msg)
            if msg.get("type") == "acct-r" and msg.get("rid") == rid:
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


def owned_entries(od, skin):
    """The owned list's entries of store index `skin` as {copy id (0 the stack): m}."""
    return {r.get("id", 0): r.get("m") or {} for r in od.get("skins", []) if isinstance(r, dict) and r.get("ind") == skin}


def copy_ids(account_id, skin):
    return [int(v) for v in query("SELECT copy_id FROM skin_copies WHERE account_id = %d AND skin_id = %d ORDER BY copy_id" %
                                  (account_id, skin)) or []]


def legacy_setup():
    """On a schema before 0406 (SQL only: this brain reads skin_stats.copy_id): an account with 0365's per-index counts.
    V-CORE 01 (7937) has an unpainted unit next to two copies, so the owned list showed the count on its stack; V-CORE 03
    (7939) is one copy only, so on that copy."""
    name = "kpleg" + str(os.getpid() % 100000)
    query("INSERT INTO accounts (username, password_hash) VALUES ('%s', 'x')" % name)
    ids = query("SELECT id FROM accounts WHERE username = '%s'" % name)
    account = int(ids[0]) if ids else 0
    check(account > 0, "a pre-0406 account (%s)" % ids)
    query("INSERT INTO inventory (account_id, skin_id, count) VALUES (%d, 7937, 3)" % account)
    query("INSERT INTO inventory (account_id, skin_id, count) VALUES (%d, 7939, 1)" % account)
    for skin in (7937, 7937, 7939):
        query("INSERT INTO skin_copies (account_id, skin_id, paint_color, paint_shader) VALUES (%d, %d, '#dc3c3c', NULL)" % (account, skin))
    query("INSERT INTO skin_stats (account_id, skin_id, stat, count) VALUES (%d, 7937, 'kills', 50)" % account)
    query("INSERT INTO skin_stats (account_id, skin_id, stat, count) VALUES (%d, 7939, 'jumps', 6)" % account)
    return account


# copy ids no skin_copies row takes in this test (the sequence starts at 1000000): a quick-sold or crafted copy, an
# open offer's escrowed copy and an accepted offer's copy
GONE, ESCROWED, TRADED = 1999990, 1999991, 1999992


def stats_of(copy):
    """skin_stats rows of `copy` as sorted [account, skin, stat, count] (concat: query() reads one column)."""
    rows = query("SELECT concat(account_id, ':', skin_id, ':', stat, ':', count) FROM skin_stats WHERE copy_id = %d"
                 " ORDER BY account_id, stat" % copy)
    return [[int(a), int(b), c, int(d)] for a, b, c, d in (r.split(":") for r in rows or [])]


def stranded_setup(legacy):
    """On the schema before 0408 (0407): counts whose copy changed hands under a brain before 0407's, left on the old
    owner (copy_take did not take them), or whose copy is gone."""
    name = "kpown" + str(os.getpid() % 100000)
    query("INSERT INTO accounts (username, password_hash) VALUES ('%s', 'x')" % name)
    ids = query("SELECT id FROM accounts WHERE username = '%s'" % name)
    owner = int(ids[0]) if ids else 0
    check(owner > 0, "a second pre-0408 account (%s)" % ids)
    query("INSERT INTO inventory (account_id, skin_id, count) VALUES (%d, 7939, 1)" % owner)
    query("INSERT INTO skin_copies (account_id, skin_id, paint_color, paint_shader) VALUES (%d, 7939, '#dc3c3c', NULL)" % owner)
    moved = int(query("SELECT max(copy_id) FROM skin_copies WHERE account_id = %d" % owner)[0])
    for who, skin, copy, stat, n in ((legacy, 7939, moved, "kills", 7), (legacy, 7939, moved, "jumps", 4), (owner, 7939, moved, "kills", 3),
                                     (legacy, 7937, GONE, "kills", 9), (legacy, 7937, ESCROWED, "headshots", 11),
                                     (legacy, 7937, TRADED, "kills", 2)):
        query("INSERT INTO skin_stats (account_id, skin_id, copy_id, stat, count) VALUES (%d, %d, %d, '%s', %d)" % (who, skin, copy, stat, n))
    for status, copy in (("open", ESCROWED), ("accepted", TRADED)):
        query("INSERT INTO trades (sender_id, receiver_id, status) VALUES (%d, %d, '%s')" % (legacy, owner, status))
        trade = int(query("SELECT max(id) FROM trades WHERE sender_id = %d" % legacy)[0])
        query("INSERT INTO trade_copies (trade_id, side, copy_id, skin_id, paint_color) VALUES (%d, 's', %d, 7937, '#dc3c3c')" % (trade, copy))
    return owner, moved


def stranded_check(owner, moved):
    rows = stats_of(moved)
    check(rows == [[owner, 7939, "jumps", 4], [owner, 7939, "kills", 10]],
          "0408: copy %d's counts left on its old owner go to its owner, 7 kills summed into its 3 (%s)" % (moved, rows))
    check(stats_of(GONE) == [] and stats_of(TRADED) == [],
          "0408: a gone copy's counts are dropped (%s, %s)" % (stats_of(GONE), stats_of(TRADED)))
    esc = query("SELECT stats FROM trade_copies WHERE copy_id = %d" % ESCROWED) or [""]
    try:
        got = json.loads(esc[0])
    except ValueError:
        got = None
    check(got == {"headshots": 11} and stats_of(ESCROWED) == [],
          "0408: an open offer's copy carries its 11 headshots in its escrow row, none left on the sender (%r, %s)" %
          (esc, stats_of(ESCROWED)))


async def scenario(port, feed, legacy):
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

            # v10.0.7 "Fixed double raid drops sometimes being lost" (db_accounts.c give_skin_ref, 0370 skin_grants): a game
            # server's named grant is granted once per ref, so a frame resent after a lost link does not double it; the
            # two drops of a double-drop weekend have their own refs and both land
            before = count(account, 11)
            r1 = await node.acct("give_skin", {"id": account, "skin": 11, "source": "raid", "ref": "gs:test%d:1" % os.getpid()})
            r2 = await node.acct("give_skin", {"id": account, "skin": 11, "source": "raid", "ref": "gs:test%d:2" % os.getpid()})
            check(not r1.get("err") and not r2.get("err") and count(account, 11) == before + 2,
                  "two named raid drops of the same skin both land (%d -> %d): %s %s" % (before, count(account, 11), r1, r2))
            r3 = await node.acct("give_skin", {"id": account, "skin": 11, "source": "raid", "ref": "gs:test%d:2" % os.getpid()})
            d3 = r3.get("data") if isinstance(r3.get("data"), dict) else r3
            check(count(account, 11) == before + 2 and d3.get("dup") is True and d3.get("count") == before + 2,
                  "the same ref again grants nothing and says dup (%d): %s" % (count(account, 11), r3))
            # v10.0.7 "Fixed items sometimes being unusable without a game reload" (db_accounts.c owned_skins): a game server
            # re-reads the owned list after a strip; it has the items granted since the join
            ow = await node.acct("owned_skins", {"id": account})
            od = ow.get("data") if isinstance(ow.get("data"), dict) else {}
            rows = {r.get("ind"): r.get("cnt") for r in od.get("skins", []) if isinstance(r, dict)}
            check(not ow.get("err") and rows.get(11) == before + 2 and rows.get(5) == count(account, 5),
                  "owned_skins: the owned list as the join account's data[10] ({11: %s, 5: %s})" % (rows.get(11), rows.get(5)))
            src = query("SELECT ref FROM skin_transactions WHERE account_id = %d AND skin_id = 11 AND source = 'raid' ORDER BY id" % account)
            check(src == ["gs:test%d:1" % os.getpid(), "gs:test%d:2" % os.getpid()], "the audit rows name the refs: %s" % src)

            # v10.0.4 counter skins (db_accounts.c commit_skin_stats, 0365): the server's takeSkinStats rows add to the
            # account's skin_stats, once per match, on an owned skin and a known stat
            query("INSERT INTO inventory (account_id, skin_id, count) VALUES (%d, 7939, 1)" % account)
            stats = [{"skin": 7939, "stat": "jumps", "count": 4}, {"skin": 7939, "stat": "dances", "count": 5},
                     {"skin": 7937, "stat": "kills", "count": 2}, {"skin": 7939, "stat": "kills", "count": 0}]
            await node.commit("m3", dict(player, itemRewards=[], skinStats=stats))

            def skin_stat(skin, stat, copy=0, who=None):
                rows = query("SELECT count FROM skin_stats WHERE account_id = %d AND skin_id = %d AND copy_id = %d AND stat = '%s'" %
                             (who or account, skin, copy, stat))
                return int(rows[0]) if rows else 0
            for _ in range(50):
                if skin_stat(7939, "jumps") == 4:
                    break
                await asyncio.sleep(0.1)
            check(skin_stat(7939, "jumps") == 4, "skinStats: 4 jumps on the owned counter skin (%d)" % skin_stat(7939, "jumps"))
            check(skin_stat(7939, "dances") == 0 and skin_stat(7937, "kills") == 0 and skin_stat(7939, "kills") == 0,
                  "an unknown stat, a skin not owned and a count below 1 are skipped")
            await node.commit("m3", dict(player, itemRewards=[], skinStats=stats))
            await asyncio.sleep(0.5)
            check(skin_stat(7939, "jumps") == 4, "the same match again adds nothing (%d)" % skin_stat(7939, "jumps"))
            await node.commit("m4", dict(player, itemRewards=[], skinStats=[{"skin": 7939, "stat": "jumps", "count": 3}]))
            for _ in range(50):
                if skin_stat(7939, "jumps") == 7:
                    break
                await asyncio.sleep(0.1)
            check(skin_stat(7939, "jumps") == 7, "a second match adds to it (%d)" % skin_stat(7939, "jumps"))

            # v10.0.7 adds a count to the copy that counted it (takeSkinStats' skinId is the copy's id,
            # deob-10.0.7.js:191914-191936; 0406): a row naming a copy lands on it, a row without one (an older game server,
            # a plain-index slot) on the index's first owned entry (the stack while there are unpainted units), and a copy
            # the account does not hold of that skin is skipped
            query("INSERT INTO inventory (account_id, skin_id, count) VALUES (%d, 7937, 3)" % account)
            for _ in range(2):
                query("INSERT INTO skin_copies (account_id, skin_id, paint_color, paint_shader) VALUES (%d, 7937, '#dc3c3c', NULL)" % account)
            c1, c2 = copy_ids(account, 7937)
            other = copy_ids(legacy, 7937)[0]
            rows = [{"skin": 7937, "copy": c2, "stat": "kills", "count": 5}, {"skin": 7937, "stat": "kills", "count": 2},
                    {"skin": 7937, "copy": 424242, "stat": "kills", "count": 3}, {"skin": 7937, "copy": other, "stat": "kills", "count": 4},
                    {"skin": 7937, "copy": "x", "stat": "kills", "count": 6}]
            await node.commit("m5", dict(player, itemRewards=[], skinStats=rows))
            for _ in range(50):
                if skin_stat(7937, "kills", c2) == 5:
                    break
                await asyncio.sleep(0.1)
            check(skin_stat(7937, "kills", c2) == 5 and skin_stat(7937, "kills", c1) == 0,
                  "a row naming copy %d lands on it, not on copy %d (%d, %d)" % (c2, c1, skin_stat(7937, "kills", c2), skin_stat(7937, "kills", c1)))
            check(skin_stat(7937, "kills", 0) == 2, "a row with no copy lands on the stack, the index's first entry (%d)" % skin_stat(7937, "kills", 0))
            total = query("SELECT sum(count) FROM skin_stats WHERE account_id = %d AND skin_id = 7937" % account)
            check(total == ["7"] and skin_stat(7937, "kills", other, legacy) == 0,
                  "a copy that is no copy, another account's copy and a non-number copy are skipped (total %s)" % total)
            ow = await node.acct("owned_skins", {"id": account})
            entries = owned_entries(ow.get("data") if isinstance(ow.get("data"), dict) else {}, 7937)
            check(entries.get(c2, {}).get("kills") == 5 and entries.get(0, {}).get("kills") == 2 and "kills" not in entries.get(c1, {}),
                  "the owned list: copy %d's m.kills 5, the stack's 2, copy %d none (%s)" % (c2, c1, entries))
            # every unit painted: a row with no copy goes on the lowest copy, and the stack's count follows it there
            query("UPDATE inventory SET count = 2 WHERE account_id = %d AND skin_id = 7937" % account)
            await node.commit("m6", dict(player, itemRewards=[], skinStats=[{"skin": 7937, "stat": "kills", "count": 1}]))
            for _ in range(50):
                if skin_stat(7937, "kills", c1) == 1:
                    break
                await asyncio.sleep(0.1)
            check(skin_stat(7937, "kills", c1) == 1, "no stack: a row with no copy lands on the lowest copy %d (%d)" % (c1, skin_stat(7937, "kills", c1)))
            ow = await node.acct("owned_skins", {"id": account})
            entries = owned_entries(ow.get("data") if isinstance(ow.get("data"), dict) else {}, 7937)
            check(entries.get(c1, {}).get("kills") == 3 and entries.get(c2, {}).get("kills") == 5 and 0 not in entries,
                  "the owned list: copy %d's 1 and the gone stack's 2 on the first entry, copy %d keeps 5 (%s)" % (c1, c2, entries))

            # 0406 on an older brain's rows: each per-index count moved onto the entry the owned list showed it on
            l7937, l7939 = copy_ids(legacy, 7937), copy_ids(legacy, 7939)
            check(skin_stat(7937, "kills", 0, legacy) == 50 and skin_stat(7939, "jumps", l7939[0], legacy) == 6,
                  "0406: V-CORE 01's 50 kills on its stack, V-CORE 03's 6 jumps on its one copy %d (%d, %d)" %
                  (l7939[0], skin_stat(7937, "kills", 0, legacy), skin_stat(7939, "jumps", l7939[0], legacy)))
            ow = await node.acct("owned_skins", {"id": legacy})
            od = ow.get("data") if isinstance(ow.get("data"), dict) else {}
            e37, e39 = owned_entries(od, 7937), owned_entries(od, 7939)
            check(e37.get(0, {}).get("kills") == 50 and all("kills" not in e37.get(c, {}) for c in l7937) and
                  e39.get(l7939[0], {}).get("jumps") == 6,
                  "0406: the owned list reads the same counts on the same entries (%s, %s)" % (e37, e39))


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
        # an empty database: with KN_TEST_MIGRATE db_scratch would run every migration, and the 0406 check seeds rows on
        # the schema before 0406 first
        r = subprocess.run(scratch + ["create", DRIVER, SCHEMA], capture_output=True,
                           env={k: v for k, v in os.environ.items() if k != "KN_TEST_MIGRATE"})
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
        # the schema before 0406 first (the migrations up to 0405), with per-index counts, then the rest
        old = os.path.join(work, "migrations", DRIVER)
        os.makedirs(old)
        src = os.path.join(ROOT, "services", "db", "migrations", DRIVER)
        for f in os.listdir(src):
            if re.match(r"\d{4}_.*\.sql$", f) and int(f[:4]) < 406:
                os.symlink(os.path.join(src, f), os.path.join(old, f))
        oldenv = dict(env, KN_DATABASE_MIGRATIONS_DIR=os.path.join(work, "migrations"))
        subprocess.run([BRAIN, "--migrate-only"], cwd=ROOT, env=oldenv, check=True, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)
        legacy = legacy_setup()
        # then up to 0407, with counts stranded on a copy's old owner, then the rest
        mid = os.path.join(work, "migrations0407", DRIVER)
        os.makedirs(mid)
        for f in os.listdir(src):
            if re.match(r"\d{4}_.*\.sql$", f) and int(f[:4]) < 408:
                os.symlink(os.path.join(src, f), os.path.join(mid, f))
        subprocess.run([BRAIN, "--migrate-only"], cwd=ROOT, env=dict(env, KN_DATABASE_MIGRATIONS_DIR=os.path.dirname(mid)),
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        owner, moved = stranded_setup(legacy)
        # this tree's migrations by path: a config/config.json found above the tree (a worktree nested in a checkout)
        # roots the default directory in that other checkout
        newenv = dict(env, KN_DATABASE_MIGRATIONS_DIR=os.path.join(ROOT, "services", "db", "migrations"))
        subprocess.run([BRAIN, "--migrate-only"], cwd=ROOT, env=newenv, check=True, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)
        stranded_check(owner, moved)
        proc, port = start_brain(env, log_path, ["--brain.community.feed=" + feed])
        asyncio.run(scenario(port, feed, legacy))
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
