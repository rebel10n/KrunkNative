#!/usr/bin/env python3
"""player-viewer WP-E part 2c: badges, v7 challenges and per-account medal counts
(doc/backend.md 7.4 "chg" / "bdg" / "cbdg" / "mdl", 7.6 / 8.4 the commit's "stats" / "prog" /
"medals"; services/brain/db_progress.c, progress.c, migration 0107_progression).

  progress_test.py <kn_brain> <repo root> [database]
Database: argument, else $KN_MM_TEST_DATABASE, else config/config.json's database.name; every
run uses its own schema (kn_test_kpg_<pid>, dropped after) and an ephemeral port. 77 = skip
(no PostgreSQL / python websockets). KN_TEST_DB_DRIVER=sqlite|mysql runs on that store driver
(db_scratch.py).

An account registers over the social socket (JSON) and reads its challenge list; a fake game
server on /mm/internal/ws commits a match with challenge tallies, stat keys and medals. The
socket gets the match-end pushes (chg, chgC, chlR, nbdgr), the list shows the progress, a
duplicate commit and a partial (left mid-match) commit change nothing, the Nuclear I badge is
auto-awarded and can be selected, claims are refused, the medal counts add up, and the account
object (social "a", internal "get") carries "badges" and v7's levelProg. The Country Flag ("flg" with
an index) is stored in stats.flg, refused past the list, and a name still goes to the staff flag.
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
    print("progress test: no database configured; skipped")
    sys.exit(77)
SCHEMA = "kn_test_kpg_%d" % os.getpid()
SECRET = "kn-progress-test-%d" % os.getpid()
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
        if frame:
            await self.ws.send(json.dumps(list(frame)))
        deadline = time.time() + 10
        while True:
            for i, f in enumerate(self.inbox):
                if f[0] == label:
                    return self.inbox.pop(i)
            self.inbox.append(json.loads(await asyncio.wait_for(self.ws.recv(), deadline - time.time())))

    async def drain(self, seconds):
        """Collects pushes for `seconds`."""
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
    """Game server side of /mm/internal/ws: "acct" ops only."""

    def __init__(self, port):
        self.url = "ws://127.0.0.1:%d/mm/internal/ws" % port
        self.rid = 0

    async def __aenter__(self):
        self.ws = await websockets.connect(self.url, additional_headers={"Authorization": "Bearer " + SECRET})
        await self.ws.send(json.dumps({"type": "hello", "nodeId": "KPG", "region": "local", "publicHost": "127.0.0.1",
                                       "publicPort": 1, "version": "test"}))
        while json.loads(await asyncio.wait_for(self.ws.recv(), 10)).get("type") != "welcome":
            pass
        return self

    async def __aexit__(self, *a):
        await self.ws.close()

    async def commit(self, match, final, player):
        self.rid += 1
        rid = self.rid
        await self.ws.send(json.dumps({"type": "commit", "rid": rid, "lobbyId": "pg1", "matchId": match, "final": final,
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


async def register(sock, name):
    r = await sock.call("a", "a", 0, [name, "progresspass1", 0], "captcha")
    assert r[1] is None, r
    return r[2], r[5], r


def by_ci(rows, ci):
    for r in rows or []:
        if r.get("CI") == ci:
            return r
    return None


async def scenario(port):
    stamp = str(int(time.time()) % 100000)
    alice = "kpga" + stamp
    social = "ws://127.0.0.1:%d/social/ws" % port
    async with Social(social) as s:
        alice_id, token, reply = await register(s, alice)
        info = reply[6] if len(reply) > 6 else {}
        check(info.get("badges") == [], "a new account has no badges: %s" % info.get("badges"))
        check(isinstance(info.get("levelProg"), (int, float)), "levelProg in the info: %s" % info.get("levelProg"))

        print("== chg: the challenge list")
        r = await s.call("chgL", "chg")
        rows = r[1]
        types = {}
        for c in rows:
            types[c["T"]] = types.get(c["T"], 0) + 1
        check(types.get("daily") == 3 and types.get("weekly") == 3, "3 daily + 3 weekly of the rotation: %s" % types)
        check(types.get("map") == 6 and types.get("weapon") == 6 and types.get("nightmare") == 2, "map / weapon / nightmare: %s" % types)
        c1 = by_ci(rows, 1)
        check(c1 and c1["DAT"] == {"ky": "k", "val": 25, "mp": 0} and c1["REW"] == {"kr": 10} and c1["PV"] == 0 and c1["PT"] == 0
              and c1["N"] == "Get 25 kills on Burg", "challenge 1 object: %s" % c1)
        daily = [c for c in rows if c["T"] == "daily"]
        check(all(0 < c.get("TR", 0) <= 86400000 for c in daily), "daily TR within a day")
        r2 = await s.call("chgL", "chg")
        check([c["CI"] for c in r2[1]] == [c["CI"] for c in rows], "the rotation is stable within the day")

        async with Node(port) as n:
            print("== commit with tallies")
            player = {"accountId": alice_id, "name": alice, "kills": 55, "deaths": 3, "score": 4000, "won": True,
                      "timePlayed": 60000, "games": 1, "kr": 0, "xp": 4000, "classIndex": 0,
                      "stats": {"hs": 10, "n": 600, "crc": 2, "cad": 1},
                      "prog": [["k", 25, -1, -1], ["k", 30, 1, -1], ["hs", 10, 1, -1], ["score", 4000, -1, -1],
                               ["kstk", 9, -1, -1], ["w", 1, -1, -1], ["gp", 1, -1, -1]],
                      "medals": {"Headshot": 3, "Double Kill": 2, "Bogus": 4},
                      "mapIndex": 0, "ranked": False}
            reply = await n.commit("m1", True, player)
            res = (reply.get("results") or [{}])[0]
            check(reply.get("ok") and res.get("ok"), "commit ok: %s" % reply)
            await s.drain(1.5)
            chg = s.take("chg")
            check(chg is not None, "chg pushed")
            triples = chg[1] if chg else []
            done = {triples[i + 1]: (triples[i], triples[i + 2]) for i in range(0, len(triples), 3)}
            check(done.get(1) == (1, 55) and done.get(2) == (1, 10), "map challenges 1 / 2 done: %s" % done)
            check(done.get(9) == (0, 30) and done.get(10) == (0, 10), "weapon challenges 9 / 10 progress: %s" % done)
            check(14 not in done and 13 not in done, "single-match score and the unreached killstreak are not saved")
            chgc = s.take("chgC")
            check(chgc and {c["CI"] for c in chgc[1]} >= {1, 2}, "chgC lists the completed challenges: %s" % (chgc and [c["CI"] for c in chgc[1]]))
            chlr = s.take("chlR")
            check(chlr and chlr[5] >= 25 and chlr[2] >= 25, "chlR: KR reward and funds: %s" % chlr)
            nb = s.take("nbdgr")
            check(nb and nb[1] == [11], "Nuclear I auto-awarded (stats.n 600): %s" % nb)
            check(s.take("ua") is not None, "ua pushed")

            r = await s.call("chgL", "chg")
            c1, c9, c14 = by_ci(r[1], 1), by_ci(r[1], 9), by_ci(r[1], 14)
            check(c1["PT"] == 1 and c1["PV"] == 55, "challenge 1 completed: %s" % c1)
            check(c9["PT"] == 0 and c9["PV"] == 30, "challenge 9 at 30: %s" % c9)
            check(c14["PV"] == 0, "single-match challenge reset: %s" % c14)

            print("== duplicate and partial commits")
            await n.commit("m1", True, player)
            p2 = dict(player, prog=[["k", 25, 1, -1]], stats={}, medals={})  # a left match: no "nem" challenge
            await n.commit("m2#c1", False, p2)
            await s.drain(1.0)
            check(s.take("chg") is None, "no challenge push for a duplicate / a left match")
            r = await s.call("chgL", "chg")
            check(by_ci(r[1], 9)["PV"] == 30, "a duplicate and a partial commit keep challenge 9 at 30")

            print("== badges")
            r = await s.call("po", "cbdg", 11)
            check(r[2] == "Badge already claimed", "claim an owned badge: %s" % r)
            r = await s.call("po", "cbdg", 13)
            check(r[2] == "Requirements not met", "Nuclear II needs 750 nukes: %s" % r)
            r = await s.call("po", "cbdg", 5)
            check(r[2] == "Badge not claimable", "a staff badge: %s" % r)
            r = await s.call("po", "bdg", 5)
            check(r[2] == "You dont own this badge", "select an unowned badge: %s" % r)
            r = await s.call("bdgr", "bdg", 11)
            check(r[1] == 11, "select Nuclear I: %s" % r)
            r = await s.call("a", "a", 1, [alice, None, token])
            check(r[6].get("badges") == [11] and r[4][16].get("bdg") == 11, "account: badges [11], stats.bdg 11: %s %s"
                  % (r[6].get("badges"), r[4][16].get("bdg")))
            check(r[4][16].get("crc") == 2 and r[4][16].get("cad") == 1 and r[4][16].get("n") == 600,
                  "v7 stat keys starting with c are kept: %s" % r[4][16])
            err, data = await n.op("get", id=alice_id)
            check(err is None and data.get("badges") == [11], "internal get: badges %s" % (data or {}).get("badges"))
            r = await s.call("bdgr", "bdg", -1)
            check(r[1] == -1, "clear the badge: %s" % r)

            print("== country flag (stats.flg; the staff flag shares the label)")
            r = await s.call("flgr", "flg", 5)
            check(r[1] == 5, "pick Country Flag 5: %s" % r)
            r = await s.call("a", "a", 1, [alice, None, token])
            check(r[4][16].get("flg") == 5, "account stats.flg 5: %s" % r[4][16].get("flg"))
            err, data = await n.op("get", id=alice_id)
            slots = (data or {}).get("data") or []  # the 40-slot account array: stats at slot 16
            check(err is None and len(slots) > 16 and (slots[16] or {}).get("flg") == 5, "internal get stats.flg 5")
            r = await s.call("po", "flg", 240)
            check(r[2] == "Unknown country flag", "an index past the list is refused: %s" % r)
            r = await s.call("flgr", "flg", -1)
            check(r[1] == -1, "clear the Country Flag: %s" % r)
            r = await s.call("fgr", "flg", alice, 0)
            check(r[1] is None, "a name still takes the staff flag path (a plain account: refused): %s" % r)

            print("== medals")
            r = await s.call("mdl", "mdl")
            check(r[1] == {"Headshot": 3, "Double Kill": 2}, "medal counts (unknown names dropped): %s" % r[1])
            await n.commit("m3", True, dict(player, prog=[], stats={}, medals={"Headshot": 1}))
            for _ in range(30):  # the progress work runs after the commit reply
                r = await s.call("mdl", "mdl")
                if r[1].get("Headshot") == 4:
                    break
                await asyncio.sleep(0.1)
            check(r[1].get("Headshot") == 4, "medal counts add up: %s" % r[1])

    async with Social(social) as g:
        r = await g.call("po", "chg")
        check(r[2] == "Must be logged in", "chg needs an account: %s" % r)


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
            print("progress test: %s unavailable: skipped" % DRIVER)
            sys.exit(r.returncode)
        dbcfg = json.loads(r.stdout)
        dbenv = {"KN_DATABASE_" + k.upper(): str(v) for k, v in dbcfg.items()}
        drop = scratch + ["drop", DRIVER, SCHEMA]
    work = tempfile.mkdtemp(prefix="kn-progress.")
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=SECRET, **dbenv)
    log_path = os.path.join(work, "brain.log")
    proc = None
    try:
        subprocess.run([BRAIN, "--migrate-only"], cwd=ROOT, env=env, check=True, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)
        with open(log_path, "w") as log:
            proc = subprocess.Popen([BRAIN, "--brain.host=127.0.0.1", "--brain.port=0", "--brain.id=kpg-%d" % os.getpid()],
                                    cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT)
        port = bound_port(log_path, proc)
        asyncio.run(scenario(port))
    finally:
        if proc:
            proc.terminate()
            proc.wait(5)
        subprocess.run(drop)
    with open(log_path) as f:
        bad = [l.strip() for l in f if re.search(r"failed:|ERROR|FATAL", l)]
    check(not bad, "no statement errors in the brain log %s" % bad[:3])
    print("progress test: " + ("OK" if not FAILS else "%d failure(s)" % len(FAILS)))
    if FAILS:
        print("log kept in " + work)
    else:
        shutil.rmtree(work, ignore_errors=True)
    sys.exit(1 if FAILS else 0)


main()
