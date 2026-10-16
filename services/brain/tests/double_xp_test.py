#!/usr/bin/env python3
"""Krunker v10.0.2's double XP (0341_double_xp; shared/src/double_xp.c, db_double_xp.c,
double_xp_social.c, the commit in db_accounts.c, kn_commit_stats / kn_dxp on PostgreSQL).

  double_xp_test.py <kn_brain> <repo root> [database]
Database: argument, else $KN_MM_TEST_DATABASE, else config/config.json's database.name. Every run
uses its own schema (postgres) or file (sqlite). KN_TEST_DB_DRIVER=postgres (default) | sqlite.
77 = skip (no PostgreSQL / python websockets).

A fake game server commits matches for one account (100 / 50 / 100 / 70 partial / 30 competitive /
40 / 60 XP) in a clan. The round flag ("classDouble") is sent explicitly, so the class checks do not
depend on the day: class XP doubles on a flagged round, and a commit without it takes the schedule
of the commit (the fallback for an older server). The end-screen offer ("dxp".offer) follows the last
committed match; the claim ("rqdxp" -> "rrqdxp") is refused unless the account's games are a multiple
of 3, the offer is the last match and it was not claimed yet. A claim sets the account score to the
match's XP doubled and nothing else: no clan score, no class score. A second brain with
accounts.double_xp.ad_provider=off refuses claims even when one is claimable.
"""
import asyncio
import datetime
import json
import os
import re
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
    print("double xp test: no database configured; skipped")
    sys.exit(77)
SCHEMA = "kn_test_dxp_%d" % os.getpid()
SECRET = "kn-dxp-test-%d" % os.getpid()
FAILS = []
DBQ = {}


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what, flush=True)
    if not cond:
        FAILS.append(what)


def q(sql):
    """One value read straight from the database (the test's own check, not the brain's)."""
    if DRIVER == "sqlite":
        con = sqlite3.connect(DBQ["path"], timeout=30)
        try:
            return con.execute(sql).fetchone()[0]
        finally:
            con.close()
    out = subprocess.run(DBQ["psql"] + ["-c", "SET search_path TO %s; %s" % (SCHEMA, sql)], env=DBQ["env"],
                         capture_output=True, text=True, check=True).stdout
    lines = [line for line in out.splitlines() if line and line != "SET"]
    return int(lines[-1]) if re.fullmatch(r"-?\d+", lines[-1]) else lines[-1]


def exec_sql(sql):
    """A write the test makes itself (a clan and its member row)."""
    if DRIVER == "sqlite":
        con = sqlite3.connect(DBQ["path"], timeout=30)
        try:
            con.execute(sql)
            con.commit()
        finally:
            con.close()
        return
    subprocess.run(DBQ["psql"] + ["-c", "SET search_path TO %s; %s" % (SCHEMA, sql)], env=DBQ["env"], capture_output=True,
                   text=True, check=True)


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


def expected_schedule():
    """v10 isDoubleClassXP / isDoubleRaidDrops for the current UTC day."""
    now = datetime.datetime.now(datetime.timezone.utc)
    weekend = now.weekday() >= 5  # Saturday, Sunday
    return weekend and now.isocalendar()[1] % 2 == 0, weekend


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
    """Game server side of /mm/internal/ws: commits and "acct" ops."""

    def __init__(self, port):
        self.url = "ws://127.0.0.1:%d/mm/internal/ws" % port
        self.rid = 0

    async def __aenter__(self):
        self.ws = await websockets.connect(self.url, additional_headers={"Authorization": "Bearer " + SECRET})
        await self.ws.send(json.dumps({"type": "hello", "nodeId": "KDX", "region": "local", "publicHost": "127.0.0.1",
                                       "publicPort": 1, "version": "test"}))
        while json.loads(await asyncio.wait_for(self.ws.recv(), 10)).get("type") != "welcome":
            pass
        return self

    async def __aexit__(self, *a):
        await self.ws.close()

    async def commit(self, match, final, player, **extra):
        self.rid += 1
        rid = self.rid
        frame = {"type": "commit", "rid": rid, "lobbyId": "dxp1", "matchId": match, "final": final,
                 "map": "Burg", "mode": 0, "custom": False, "players": [player]}
        frame.update(extra)
        await self.ws.send(json.dumps(frame))
        while True:
            msg = json.loads(await asyncio.wait_for(self.ws.recv(), 15))
            if msg.get("type") == "commit-reply" and msg.get("rid") == rid:
                return (msg.get("results") or [{}])[0]

    async def op(self, op, **args):
        self.rid += 1
        rid = self.rid
        await self.ws.send(json.dumps({"type": "acct", "rid": rid, "op": op, "args": args}))
        while True:
            msg = json.loads(await asyncio.wait_for(self.ws.recv(), 15))
            if msg.get("type") == "acct-r" and msg.get("rid") == rid:
                return msg.get("err"), msg.get("data")


async def register(sock, name):
    r = await sock.call("a", "a", 0, [name, "dxppass1", 0], "captcha")
    assert r[1] is None, r
    return r[2], r[5], r


def class_stat(account, key):
    """The class XP stat (player_stats, account data slot 16)."""
    data = (account or {}).get("data") or []
    stats = data[16] if len(data) > 16 and isinstance(data[16], dict) else {}
    return stats.get(key)


def games_of(account):
    """The account's games played (account data slot 3)."""
    data = (account or {}).get("data") or []
    return data[3] if len(data) > 3 else None


def player(account_id, xp, class_index, kills=2, games=1):
    """One player's stats as the game server sends them: games 1 only when the match ended (a final commit)."""
    return {"accountId": account_id, "kills": kills, "deaths": 1, "score": xp, "xp": xp, "won": 1, "games": games,
            "timePlayed": 60000, "kr": 0, "classIndex": class_index, "stats": {}}


async def claim(s, body=None):
    if body is None:
        return await s.call("rrqdxp", "rqdxp")
    return await s.call("rrqdxp", "rqdxp", body)


async def scenario(port, stamp):
    social = "ws://127.0.0.1:%d/social/ws" % port
    name = "dxp" + stamp
    class_day, raid_day = expected_schedule()
    cm = 2 if class_day else 1
    print("== guest and a new account")
    async with Social(social) as g:
        r = await claim(g)
        check(r[1] is None and r[2] == "Must be logged in", "a guest's claim is refused: %s" % (r,))
    s = await Social(social).__aenter__()  # the logged-in socket: every claim runs on it
    try:
        acct, token, reply = await register(s, name)
        dxp = (reply[6] if len(reply) > 6 else {}).get("dxp") or {}
        check(dxp.get("offer") is None, "a new account has no offer")
        check(dxp.get("doubleClassXP") is class_day and dxp.get("doubleRaidDrops") is raid_day,
              "the weekend schedules match v10 for today (class %s, raid %s)" % (class_day, raid_day))
        r = await claim(s)
        check(r[1] is None and r[2] == "No double XP for this match", "a claim with no offer is refused: %s" % (r,))
        exec_sql("INSERT INTO clans (clan_name) VALUES ('dxpclan%s')" % stamp)
        exec_sql("INSERT INTO clan_members (account_id, clan_id) VALUES (%d, (SELECT clan_id FROM clans WHERE clan_name = 'dxpclan%s'))"
                 % (acct, stamp))
        async with Node(port) as n:
            await scenario_matches(n, s, acct, stamp, cm)
    finally:
        await s.__aexit__(None, None, None)
    return acct


async def scenario_matches(n, s, acct, stamp, cm):
    """cm: the class multiplier of a commit that sends no round flag (the schedule today, the fallback)."""
    clan = "SELECT clan_score FROM clans WHERE clan_name = 'dxpclan%s'" % stamp
    print("== the offer after a match, the claim gate (games % 3)")
    # the round's class flag comes from the game server ("classDouble"): explicit here, so the test does
    # not depend on the day it runs on
    m1 = await n.commit("dxp-m1-" + stamp, True, player(acct, 100, 3), classDouble=True)
    check(m1.get("ok") is True and m1.get("duplicate") == 0, "m1 committed")
    err, data = await n.op("get", id=acct)
    # c<class> is cumulative (v10 saveClassScores adds to stats['c'+i]): the expected totals run on
    c3 = 200
    check(data["score"] == 100 and class_stat(data, "c3") == c3, "m1: score 100, class 3 doubled by the round flag (200)")
    check(q(clan) == 100, "m1: the clan gets the match's XP (100)")
    check(data["dxp"]["offer"] == {"games": 1, "xp": 100, "claimable": False}, "m1: offered, not claimable (games 1): %s" % data["dxp"]["offer"])
    r = await claim(s)
    check(r[1] is None and r[2] == "No double XP for this match", "games 1: the claim is refused: %s" % (r,))
    await n.commit("dxp-m2-" + stamp, True, player(acct, 50, 3), classDouble=False)
    err, data = await n.op("get", id=acct)
    c3 += 50
    check(class_stat(data, "c3") == c3, "m2: a round without the flag adds its 50 class XP (250)")
    check(data["dxp"]["offer"] == {"games": 2, "xp": 50, "claimable": False}, "m2: the offer follows the last match")
    await n.commit("dxp-m3-" + stamp, True, player(acct, 100, 3))  # no round flag: the schedule at the commit
    err, data = await n.op("get", id=acct)
    check(data["score"] == 250 and games_of(data) == 3, "m3: score 250 and games 3 before the claim")
    c3 += 100 * cm
    check(class_stat(data, "c3") == c3, "m3 (no round flag): the commit-time schedule adds 100 at x%d" % cm)
    check(data["dxp"]["offer"] == {"games": 3, "xp": 100, "claimable": True}, "m3: claimable at games 3: %s" % data["dxp"]["offer"])
    clan_before = q(clan)

    print("== the claim doubles the match and nothing else")
    r = await claim(s, {"ad": "proof-x"})
    v = r[1] or {}
    check(v.get("playerScore") == 350 and v.get("xp") == 100 and v.get("games") == 3,
          "the claim: playerScore 250 -> 350 (the match's 100 again): %s" % (v,))
    check(q(clan) == clan_before, "the claim leaves the clan score alone (v10 RECIEVE_DOUBLE_XP sets the account score only)")
    err, data = await n.op("get", id=acct)
    check(class_stat(data, "c3") == c3, "the claim leaves the class score alone")
    r = await claim(s)
    check(r[1] is None and r[2] == "No double XP for this match", "the same match is not doubled twice")
    err, data = await n.op("get", id=acct)
    check(data["score"] == 350 and data["dxp"]["offer"]["claimable"] is False, "the score is 350, the offer is claimed")
    check(q("SELECT count(*) FROM double_xp_claims WHERE account_id = %d" % acct) == 1, "one claim is logged")
    check(q("SELECT count(*) FROM double_xp_claims WHERE account_id = %d AND provider = 'placeholder' AND ad_ref IS NULL"
            % acct) == 1, "the placeholder's claim records no ad proof")

    print("== partial, competitive and duplicate commits")
    await n.commit("dxp-part-" + stamp, False, player(acct, 70, 3, games=0), classDouble=False)
    err, data = await n.op("get", id=acct)
    c3 += 70
    check(data["score"] == 420 and games_of(data) == 3, "a partial commit still credits its XP (score 420, games 3)")
    check(class_stat(data, "c3") == c3, "a partial commit adds its class XP as well")
    check(data["dxp"]["offer"] is None, "a partial commit (no game counted) offers nothing")
    r = await claim(s)
    check(r[2] == "No double XP for this match", "nothing to claim after a partial commit")
    await n.commit("dxp-comp-" + stamp, True, player(acct, 30, 3), comp=True, classDouble=True)
    err, data = await n.op("get", id=acct)
    c3 += 60  # the flagged round: 30 x 2
    check(class_stat(data, "c3") == c3, "a competitive round with the flag: 30 class XP doubled to 60")
    check(data["dxp"]["offer"] is None, "a competitive match offers no 2x XP (v10 isComp)")
    await n.commit("dxp-m5-" + stamp, True, player(acct, 40, 3), classDouble=False)
    await n.commit("dxp-m6-" + stamp, True, player(acct, 60, 3), classDouble=False)
    dup = await n.commit("dxp-m6-" + stamp, True, player(acct, 60, 3), classDouble=False)
    check(dup.get("duplicate") == 1, "a duplicate commit is recognised")
    err, data = await n.op("get", id=acct)
    check(data["score"] == 550 and games_of(data) == 6, "the duplicate changes nothing (score 550, games 6)")
    check(data["dxp"]["offer"] == {"games": 6, "xp": 60, "claimable": True}, "games 6: claimable")
    r = await claim(s)
    check((r[1] or {}).get("playerScore") == 610, "games 6: the claim doubles the 60 XP (610): %s" % (r,))
    err, data = await n.op("get", id=acct)
    check(data["score"] == 610 and data["dxp"]["offer"]["claimable"] is False, "after the second claim the score is 610")
    check(q("SELECT count(*) FROM double_xp_claims WHERE account_id = %d" % acct) == 2, "two claims are logged")
    check(q(clan) == 100 + 50 + 100 + 70 + 30 + 40 + 60, "the clan is credited by the commits only (450), never by a claim")


async def scenario_off(port, acct, stamp):
    """provider=off: a claimable offer is still refused."""
    social = "ws://127.0.0.1:%d/social/ws" % port
    print("== provider off")
    async with Node(port) as n:
        for i, xp in enumerate((10, 20, 30), start=7):
            await n.commit("dxp-off%d-%s" % (i, stamp), True, player(acct, xp, 3))
        err, data = await n.op("get", id=acct)
        check(((data.get("dxp") or {}).get("offer") or {}).get("claimable") is True, "games 9 on the second brain: claimable")
        score = data["score"]
        s = await Social(social).__aenter__()
        try:
            r = await s.call("a", "a", 1, ["dxp" + stamp, "dxppass1", 0], "captcha")  # action 1: a login
            check(r[1] is None, "the account logs in on the second brain (%s)" % (r[1],))
            r = await claim(s)
            check(r[1] is None and r[2] == "Rewarded ads are not available right now", "a claim is refused with the provider off: %s" % (r,))
        finally:
            await s.__aexit__(None, None, None)
        err, data = await n.op("get", id=acct)
        check(data["score"] == score, "the refused claim leaves the score alone")


def start_brain(work, env, extra, log_name):
    log_path = os.path.join(work, log_name)
    log = open(log_path, "w")
    proc = subprocess.Popen([BRAIN, "--brain.host=127.0.0.1", "--brain.port=0", "--brain.id=kdx-%d" % os.getpid()] + extra,
                            cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT)
    return proc, log_path


def main():
    global DBQ
    if DRIVER == "postgres":
        pg = [sys.executable, "-I", os.path.join(ROOT, "services", "brain", "tests", "pg_schema.py"), ROOT, DB]
        rc = subprocess.run(pg + ["create", SCHEMA]).returncode
        if rc:
            sys.exit(rc)
        dbenv = {"KN_DATABASE_NAME": DB, "KN_DATABASE_SEARCH_PATH": SCHEMA}
        drop = pg + ["drop", SCHEMA]
        host = os.environ.get("KN_DATABASE_HOST", str(CONFIG_DB.get("host", "127.0.0.1")))
        port = os.environ.get("KN_DATABASE_PORT", str(CONFIG_DB.get("port", 5432)))
        user = os.environ.get("KN_DATABASE_USER", str(CONFIG_DB.get("user", "krunknative")))
        password = os.environ.get("KN_DATABASE_PASSWORD", str(CONFIG_DB.get("password", "")))
        DBQ = {"psql": ["psql", "-h", host, "-p", port, "-U", user, "-d", DB, "-v", "ON_ERROR_STOP=1", "-qAt"],
               "env": dict(os.environ, PGPASSWORD=password)}
    else:
        scratch = [sys.executable, "-I", os.path.join(ROOT, "services", "brain", "tests", "db_scratch.py"), ROOT]
        r = subprocess.run(scratch + ["create", DRIVER, SCHEMA], capture_output=True)
        if r.returncode:
            print("double xp test: %s unavailable: skipped" % DRIVER)
            sys.exit(r.returncode)
        dbcfg = json.loads(r.stdout)
        dbenv = {"KN_DATABASE_" + k.upper(): str(v) for k, v in dbcfg.items()}
        DBQ = {"path": dbcfg.get("path", "")}
        drop = scratch + ["drop", DRIVER, SCHEMA]
    work = tempfile.mkdtemp(prefix="kn-dxp.")
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=SECRET, **dbenv)
    stamp = str(int(time.time()) % 100000)
    procs = []
    try:
        subprocess.run([BRAIN, "--migrate-only"], cwd=ROOT, env=env, check=True, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)
        proc, log_path = start_brain(work, env, [], "brain.log")
        procs.append(proc)
        port = bound_port(log_path, proc)
        acct = asyncio.run(scenario(port, stamp))
        proc.terminate()
        proc.wait(10)
        proc, log_path = start_brain(work, env, ["--accounts.double_xp.ad_provider=off"], "brain-off.log")
        procs.append(proc)
        port = bound_port(log_path, proc)
        asyncio.run(scenario_off(port, acct, stamp))
    finally:
        for p in procs:
            if p.poll() is None:
                p.terminate()
        subprocess.run(drop, capture_output=True)
    if FAILS:
        print("double xp test: %d FAILED" % len(FAILS))
        sys.exit(1)
    print("double xp test: ok")


if __name__ == "__main__":
    main()
