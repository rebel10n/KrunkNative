#!/usr/bin/env python3
"""The in-game player list's K.P.D call on kn_brain (services/brain/db_kpd.c, migration 0209_kpd_calls): the game server's "acct" ops kpd_call / kpd_calls / kpd_log / kpd_rate / kpd_stats / kpd_apply / kpd_apps / kpd_officer / kpd_demos / kpd_reviewers.
  kpd_test.py <kn_brain> <repo root> [database]
Database: argument, else $KN_MM_TEST_DATABASE, else config/config.json's database.name (local
PostgreSQL); every run uses its own schema (kn_test_kpd_<pid>, dropped after) and an ephemeral port.
KN_TEST_DB_DRIVER=sqlite|mysql runs on that store driver (db_scratch.py). 77 = skip.
Two accounts register over the social socket; a fake game server on /mm/internal/ws then:
  - kpd_call from alice on bob: recorded (id, n, calls 1), the row has the lobby, map, mode, type
  - the same suspect in the same lobby again within kpdAutoTime: "Already reported"
  - the same suspect in another lobby: a new call; a guest suspect (no account) by name
  - a guest reporter (id 0): refused
  - kpd_calls: the open calls newest first with v10's Distress Calls fields (can, con, gst, gco, typ,
    jop / jor: the reporter's rating; crs / cos: the reporter's and the suspect's XP, 0 for a guest)
  - kpd_rate: an officer's verdict (rating 1 tagged / 0 not), status handled, once per call; the
    reporter's rating jop / jor follows; kpd_log: the rated calls {modn, htag, hckn}
  - kpd_stats: the officer's tagged verdicts by range (v10's Stats tab: 1day .. alltimeEntries)
  - the reporter's Helpful Reports: kpd_rate counts into players.jobrating / jobratingpositive
  - kpd_apply: refused below 20 positive reports, kept from there (one pending), listed by kpd_apps;
    kpd_officer: refused for a player, a developer grants (moderator flag, application closed), the
    console (actor 0) revokes, moderation_log keeps both; an officer cannot apply
  - kpd_demos (the Demos window, migration 0346_kpd_demos; rows are inserted directly, there is no
    recorder yet): a page is 30 suspect groups with more, the next page the rest; a named suspect's
    name shows only when banned; a guest is gst 1 by its group; the flat rows keep each group's demos
    together; q is the name (case-insensitive exact); dur is milliseconds; myv is null
  - kpd_reviewers (the Reviewers window): the officers' handled calls, the last 30 days and all time
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
    print("kpd test: no database configured; skipped")
    sys.exit(77)
SCHEMA = "kn_test_kpd_%d" % os.getpid()
SECRET = "kn-kpd-test-%d" % os.getpid()
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
        await self.ws.send(json.dumps({"type": "hello", "nodeId": "KPD", "region": "local", "publicHost": "127.0.0.1",
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
    r = await sock.call("a", "a", 0, [name, "kpdpass12", 0], "captcha")
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
    return None


def call(reporter_id, reporter, suspect_id, suspect, lobby="KPD:ab12c", typ=0):
    return dict(id=reporter_id, name=reporter, level=12, suspect=suspect_id, suspect_name=suspect, lobby=lobby, node="KPD",
                region="local", map="Burg", mode=0, typ=typ)


async def scenario(port, dbcfg):
    stamp = str(int(time.time()) % 100000)
    alice, bob = "kpda" + stamp, "kpdb" + stamp
    social = "ws://127.0.0.1:%d/social/ws" % port
    async with Social(social) as s:
        alice_id = await register(s, alice)
    async with Social(social) as s:
        bob_id = await register(s, bob)

    async with Node(port) as n:
        print("== kpd_call")
        err, data = await n.op("kpd_call", **call(alice_id, alice, bob_id, bob))
        check(err is None and data and data.get("id", 0) > 0 and data.get("n") == bob and data.get("calls") == 1,
              "alice calls the K.P.D on bob: %s %s" % (err, data))
        rows = sql(dbcfg, "SELECT reporter_id, suspect_id, suspect_name, lobby, map, mode, typ, status FROM kpd_calls")
        if rows is not None:
            check(rows == [[str(alice_id), str(bob_id), bob, "KPD:ab12c", "Burg", "0", "0", "0"]], "the kpd_calls row: %s" % rows)
        err, data = await n.op("kpd_call", **call(alice_id, alice, bob_id, bob))
        check(err and "Already reported" in err, "bob again in the same lobby: %s" % err)
        err, data = await n.op("kpd_call", **call(bob_id, bob, alice_id, alice, lobby="KPD:zz99x", typ=3))
        check(err is None and data.get("n") == alice, "bob calls on alice from a custom lobby: %s %s" % (err, data))
        err, data = await n.op("kpd_call", **call(alice_id, alice, 0, "Guest_7", lobby="KPD:zz99x"))
        check(err is None and data.get("calls") == 2, "a guest suspect by name; alice's second call: %s %s" % (err, data))
        err, data = await n.op("kpd_call", **call(alice_id, alice, 0, "Guest_7", lobby="KPD:zz99x"))
        check(err and "Already reported" in err, "the guest suspect again: %s" % err)
        err, data = await n.op("kpd_call", **call(0, "Guest_3", bob_id, bob))
        check(err and "Sign in" in err, "a guest reporter is refused: %s" % err)
        err, data = await n.op("kpd_call", **call(alice_id, alice, bob_id, ""))
        check(err and "not found" in err, "no suspect: %s" % err)

        print("== kpd_calls (Distress Calls)")
        sql(dbcfg, "UPDATE players SET player_score = 4000 WHERE player_id = %d" % alice_id)
        sql(dbcfg, "UPDATE players SET player_score = 9000 WHERE player_id = %d" % bob_id)
        err, data = await n.op("kpd_calls", limit=10)
        check(err is None and isinstance(data, list) and len(data) == 3, "three open calls: %s %s" % (err, data))
        if isinstance(data, list) and len(data) == 3:
            names = sorted((d["can"], d["con"], d["gst"]) for d in data)
            check(names == sorted([(alice, bob, 0), (bob, alice, 0), (alice, "Guest_7", 1)]), "reporter / suspect / guest: %s" % names)
            custom = [d for d in data if d["can"] == bob][0]
            check(custom["typ"] == 3 and custom["gco"] == "KPD:zz99x" and custom["tim"] > 0, "type, game code, time: %s" % custom)
            mine = [d for d in data if d["can"] == alice and d["con"] == bob][0]
            check(mine["jor"] == 0 and mine["jop"] == 0, "alice's rating 0/0: %s" % mine)
            check(mine["crs"] == 4000 and mine["cos"] == 9000, "the reporter's and the suspect's XP: %s" % mine)
            guest = [d for d in data if d["con"] == "Guest_7"][0]
            check(guest["cos"] == 0 and guest["crs"] == 4000, "a guest suspect has no XP: %s" % guest)

        print("== kpd_rate (an officer's verdict) and kpd_log (moderation log)")
        err, data = await n.op("kpd_log", limit=10)
        check(err is None and data == [], "nothing rated yet: %s %s" % (err, data))
        rows = sql(dbcfg, "SELECT id FROM kpd_calls WHERE suspect_id = %d" % bob_id)
        bob_call = int(rows[0][0]) if rows else 0
        err, data = await n.op("kpd_rate", call=bob_call, id=bob_id, rating=1)
        check(err is None and data and data.get("id") == bob_call and data.get("rating") == 1 and data.get("n") == bob,
              "the officer tags bob's case: %s %s" % (err, data))
        rows = sql(dbcfg, "SELECT status, rating, handled_by, handled_at IS NOT NULL FROM kpd_calls WHERE id = %d" % bob_call)
        if rows is not None:
            check(rows and rows[0][:3] == ["1", "1", str(bob_id)] and rows[0][3] in ("t", "1", "true"),
                  "status handled, rating 1, handled_by / handled_at: %s" % rows)
        err, data = await n.op("kpd_rate", call=bob_call, id=bob_id, rating=0)
        check(err and "already closed" in err, "a second verdict: %s" % err)
        err, data = await n.op("kpd_rate", call=0, id=bob_id, rating=1)
        check(err and "No K.P.D case" in err, "no call id: %s" % err)
        err, data = await n.op("kpd_log", limit=10)
        check(err is None and data and data[0]["hckn"] == bob and data[0]["htag"] == 1 and data[0]["modn"] == bob,
              "the tagged call is in the log: %s %s" % (err, data))
        err, data = await n.op("kpd_calls", limit=10)
        check(isinstance(data, list) and len(data) == 2, "and no longer open: %d" % len(data or []))
        mine = [d for d in data or [] if d["can"] == alice]
        check(mine and mine[0]["jop"] == 1 and mine[0]["jor"] == 1, "alice's rating is now 1/1: %s" % mine)
        guest = [d for d in data or [] if d["con"] == "Guest_7"]
        err, _ = await n.op("kpd_rate", call=guest[0]["id"] if guest else 0, id=bob_id, rating=0)
        check(err is None, "the guest case: not tagged (%s)" % err)
        err, data = await n.op("kpd_calls", limit=10)
        check(isinstance(data, list) and len(data) == 1 and data[0]["can"] == bob and data[0]["jor"] == 0,
              "one open call left (bob's, unrated reporter): %s" % data)
        rows = sql(dbcfg, "SELECT count(*), sum(CASE WHEN rating = 1 THEN 1 ELSE 0 END) FROM kpd_calls WHERE reporter_id = %d AND status = 1"
                   % alice_id)
        if rows is not None:
            check(rows == [["2", "1"]], "alice's rating 1/2 (one helpful of two rated): %s" % rows)
        err, data = await n.op("kpd_log", limit=10)
        check(err is None and len(data or []) == 2 and sorted(d["htag"] for d in data) == [0, 1], "the log: tagged and untagged: %s" % data)

        print("== kpd_stats (v10's Stats tab: the officer's tags by range)")
        err, data = await n.op("kpd_stats", id=bob_id)
        st = data[0] if isinstance(data, list) and data else {}
        ranges = ("1day", "3day", "7day", "14day", "30day", "90day", "alltime")
        check(err is None and all(st.get(r + "Entries") == 1 for r in ranges),
              "bob tagged one case (the untagged guest case does not count): %s %s" % (err, data))
        err, data = await n.op("kpd_stats", id=alice_id)
        st = data[0] if isinstance(data, list) and data else {}
        check(err is None and all(st.get(r + "Entries") == 0 for r in ranges), "alice rated nothing: %s %s" % (err, data))

        print("== kpd_demos (the Demos window: suspect groups, 30 a page)")
        demo_rows = [("tok-bob-1", "ct-bob", bob_id, bob, 1, 1, 240000, 1, 0, 1, 2, 1),
                     ("tok-bob-2", "ct-bob", bob_id, bob, 1, 0, 90000, 1, 1, 0, 3, 0),
                     ("tok-alice-1", "ct-alice", alice_id, alice, 2, 0, 60000, 0, 1, 0, 0, 0),
                     ("tok-guest-9", "ct-g9", None, "Guest_9", 9, 0, 120000, 0, 0, 1, 0, 0)]
        for i in range(31):  # 31 guest groups more: 34 groups in all, two pages
            demo_rows.append(("tok-pg-%d" % i, "ct-pg", None, "Pager_%d" % i, 100 + i, 3, 30000, 0, 0, 1, 0, 0))
        for tok, ct, sid, name, grp, typ, dur, ban, rsl, opn, yes, no in demo_rows:
            sql(dbcfg, "INSERT INTO kpd_demos (token, ct, suspect_id, suspect_name, suspect_group, typ, map, mode_name, duration_ms,"
                       " banned, resolved, opened, yes_votes, no_votes) VALUES ('%s', '%s', %s, '%s', %d, %d, 'Burg', 'Hardpoint',"
                       " %d, %d, %d, %d, %d, %d)" % (tok, ct, "NULL" if sid is None else str(sid), name, grp, typ, dur, ban, rsl,
                                                       opn, yes, no))
        err, page1 = await n.op("kpd_demos", off=0, q="")
        err2, page2 = await n.op("kpd_demos", off=30, q="")
        groups1 = {(r["grp"], r["gst"]) for r in (page1 or {}).get("demos", [])}
        groups2 = {(r["grp"], r["gst"]) for r in (page2 or {}).get("demos", [])}
        check(err is None and err2 is None and page1 and page1["more"] is True and len(groups1) == 30,
              "the first page: 30 groups and more: %s %s" % (len(groups1), (page1 or {}).get("more")))
        check(page2 and page2["more"] is False and len(groups2) == 4 and not (groups1 & groups2),
              "the next page: the other 4 groups, no more: %s" % (len(groups2),))
        # newest group first: the pagers (inserted last) fill page 1; bob's and alice's groups are older
        all_rows = (page1 or {}).get("demos", []) + (page2 or {}).get("demos", [])
        bob_rows = [r for r in all_rows if r["grp"] == 1]
        if bob_rows:
            r = [x for x in bob_rows if x["tok"] == "tok-bob-1"][0]  # the older demo: 240 s, tagged
            check(len(bob_rows) == 2 and r["name"] == bob and r["ban"] == 1 and r["gst"] == 0 and r["dur"] == 240000
                  and r["myv"] is None and r["dat"] > 0 and r["mod"] == "Hardpoint" and r["map"] == "Burg",
                  "bob's group: named when banned, two demos, duration in ms, no own vote: %s" % r)
            check(r["typ"] == 1 and r["yes"] == 2 and r["f_no"] == 1 and r["opn"] == 1 and r["rsl"] == 0,
                  "the demo's type, votes and status: %s" % r)
            check({x["tok"] for x in bob_rows} == {"tok-bob-1", "tok-bob-2"}, "bob's two demos: %s" % [x["tok"] for x in bob_rows])
            idx = [i for i, x in enumerate(all_rows) if x["grp"] == 1]
            check(idx == list(range(idx[0], idx[0] + len(idx))), "a group's demos are adjacent")
        else:
            check(False, "bob's group is on a page: %s" % [r["grp"] for r in all_rows][:5])
        alice_rows = [r for r in all_rows if r["grp"] == 2]
        check(alice_rows and alice_rows[0]["name"] == "" and alice_rows[0]["ban"] == 0 and alice_rows[0]["gst"] == 0,
              "an unbanned named suspect shows no name: %s" % alice_rows[:1])
        err, data = await n.op("kpd_demos", off=0, q=bob.upper())
        rows = (data or {}).get("demos", [])
        check(err is None and len(rows) == 2 and all(r["grp"] == 1 for r in rows) and (data or {}).get("more") is False,
              "q: the name in any case, bob's demos only: %s %s" % (err, data))
        err, data = await n.op("kpd_demos", off=0, q="Guest_9")
        rows = (data or {}).get("demos", [])
        check(err is None and len(rows) == 1 and rows[0]["gst"] == 1 and rows[0]["name"] == "" and rows[0]["tok"] == "tok-guest-9",
              "q: a guest by name, shown as a guest: %s" % rows)
        err, data = await n.op("kpd_demos", off=0, q="nobody_" + stamp)
        check(err is None and data == {"demos": [], "more": False}, "q: no demos for that name: %s %s" % (err, data))

        print("== kpd_reviewers (the Reviewers window: the officers' handled calls)")
        err, data = await n.op("kpd_reviewers")
        rv = (data or {}).get("reviewers", [])
        check(err is None and rv == [{"name": bob, "cnt": 2, "range": "month"}, {"name": bob, "cnt": 2, "range": "all"}],
              "bob handled two calls (the tagged one and the guest's), listed for the month and all time: %s %s" % (err, rv))

        print("== the reporter's Helpful Reports (players.jobrating / jobratingpositive: v10 jobRating / jobRatingPos)")
        rows = sql(dbcfg, "SELECT jobrating, jobratingpositive FROM players WHERE player_id = %d" % alice_id)
        if rows is not None:
            check(rows == [["2", "1"]], "alice: 2 rated calls, 1 helpful: %s" % rows)
        rows = sql(dbcfg, "SELECT jobrating, jobratingpositive FROM players WHERE player_id = %d" % bob_id)
        if rows is not None:
            check(rows == [["0", "0"]], "bob's call is unrated: 0/0: %s" % rows)

        print("== kpd_apply (\"kpdA\"), kpd_apps, kpd_officer (grant / revoke)")
        app = dict(id=alice_id, name=alice, discord="alice#1", reason="I hunt hackers")
        err, _ = await n.op("kpd_apply", **app)
        check(err and "20 positive reports" in err, "1 helpful report: not yet (%s)" % err)
        err, _ = await n.op("kpd_apply", **dict(app, discord=""))
        check(err and "Invalid Discord" in err, "no Discord: %s" % err)
        sql(dbcfg, "UPDATE players SET jobrating = 22, jobratingpositive = 21 WHERE player_id = %d" % alice_id)  # 20 more helpful ones
        err, data = await n.op("kpd_apply", **app)
        check(err is None and data and data.get("id", 0) > 0, "21 of 22 helpful: the application is kept: %s %s" % (err, data))
        err, _ = await n.op("kpd_apply", **app)
        check(err and "already in review" in err, "a second one while the first is pending: %s" % err)
        err, data = await n.op("kpd_apps", limit=10)
        mine = [d for d in data or [] if d.get("n") == alice]
        check(err is None and len(mine) == 1 and mine[0]["discord"] == "alice#1" and mine[0]["reason"] == "I hunt hackers"
              and mine[0]["jop"] == 21 and mine[0]["jor"] == 22, "kpd_apps lists it with the rating: %s %s" % (err, data))
        err, _ = await n.op("kpd_officer", actor=bob_id, name=alice, on=1)
        check(err and "Only developers" in err, "a player cannot grant: %s" % err)
        sql(dbcfg, "UPDATE players SET player_developer = 1 WHERE player_id = %d" % bob_id)
        err, data = await n.op("kpd_officer", actor=bob_id, name=alice.upper(), on=1)
        check(err is None and data and data.get("id") == alice_id and data.get("n") == alice and data.get("on") == 1,
              "a developer grants it (the name in any case): %s %s" % (err, data))
        rows = sql(dbcfg, "SELECT player_moderator FROM players WHERE player_id = %d" % alice_id)
        if rows is not None:
            check(rows == [["1"]], "alice is a moderator (K.P.D officer): %s" % rows)
        rows = sql(dbcfg, "SELECT status FROM kpd_applications WHERE account_id = %d" % alice_id)
        if rows is not None:
            check(rows == [["1"]], "her application is closed as granted: %s" % rows)
        err, data = await n.op("kpd_apps", limit=10)
        check(err is None and not [d for d in data or [] if d.get("n") == alice], "and no longer pending: %s" % data)
        err, _ = await n.op("kpd_apply", **app)
        check(err and "Already K.P.D" in err, "an officer cannot apply: %s" % err)
        err, data = await n.op("kpd_officer", actor=0, name=alice, on=0)
        check(err is None and data and data.get("on") == 0, "the game server's console revokes it: %s %s" % (err, data))
        rows = sql(dbcfg, "SELECT player_moderator FROM players WHERE player_id = %d" % alice_id)
        if rows is not None:
            check(rows == [["0"]], "alice is no longer a moderator: %s" % rows)
        rows = sql(dbcfg, "SELECT coalesce(actor_id, 0), action FROM moderation_log WHERE target_id = %d ORDER BY id" % alice_id)
        if rows is not None:
            check(rows == [[str(bob_id), "kpd_grant"], ["0", "kpd_revoke"]], "moderation_log: the grant by bob, the revoke: %s" % rows)
        err, _ = await n.op("kpd_officer", actor=0, name="nobody_" + stamp, on=1)
        check(err and "not found" in err, "an unknown account: %s" % err)


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
            print("kpd test: %s unavailable: skipped" % DRIVER)
            sys.exit(r.returncode)
        dbcfg = json.loads(r.stdout)
        dbenv = {"KN_DATABASE_" + k.upper(): str(v) for k, v in dbcfg.items()}
        drop = scratch + ["drop", DRIVER, SCHEMA]
    work = tempfile.mkdtemp(prefix="kn-kpd.")
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=SECRET, **dbenv)
    log_path = os.path.join(work, "brain.log")
    proc = None
    try:
        subprocess.run([BRAIN, "--migrate-only"], cwd=ROOT, env=env, check=True, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)
        with open(log_path, "w") as log:
            proc = subprocess.Popen([BRAIN, "--brain.host=127.0.0.1", "--brain.port=0", "--brain.id=kpd-%d" % os.getpid()],
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
    print("kpd test: " + ("OK" if not FAILS else "%d failure(s)" % len(FAILS)))
    if FAILS:
        print("log kept in " + work)
    else:
        shutil.rmtree(work, ignore_errors=True)
    sys.exit(1 if FAILS else 0)


main()
