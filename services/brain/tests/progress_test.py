#!/usr/bin/env python3
"""player-viewer WP-E part 2c: badges, v7 challenges and per-account medal counts
(doc/backend.md 7.4 "chg" / "bdg" / "cbdg" / "mdl", 7.6 / 8.4 the commit's "stats" / "prog" /
"medals"; services/brain/db_progress.c, progress.c, migration 0107_progression).

  progress_test.py <kn_brain> <repo root> [database]
Database: argument, else $KN_MM_TEST_DATABASE, else config/config.json's (or $KN_TEST_CONFIG's) database.name; every
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

v10.0.7's mid-match CHALLENGE_COMPLETED: the game server reads the account's list ("challenges", the chgL order) and
on a target reached mid-match ("challenge_reached" {id, ci}) kn_brain pushes ["chgC", index, true, REW]; a completed
challenge pushes nothing. v10.0.7's World KrunkCup (0399_krunkcup, db_krunkcup.c): commits played in the KrunkCup
Jerseys ("kcJersey") credit the team of the jerseys' style (none: style 0), others nothing; the leaderboards' KrunkCup
routes (teams, a team's contributors, the top players) and the Participation Band (GET /krunkcup/wristband, POST
/krunkcup/wristband/claim) follow.
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
    print("progress test: no database configured; skipped")
    sys.exit(77)
SCHEMA = "kn_test_kpg_%d" % os.getpid()
SECRET = "kn-progress-test-%d" % os.getpid()
FAILS = []
DBQ = {}  # the test's own database access (exec_sql)


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what, flush=True)
    if not cond:
        FAILS.append(what)


def exec_sql(sql):
    """A statement the test runs itself (challenge 2's reward, before kn_brain starts; the KrunkCup rows): the rows'
    text, a line per row."""
    if DRIVER == "sqlite":
        con = sqlite3.connect(DBQ["path"], timeout=30)
        try:
            rows = con.execute(sql).fetchall()
            con.commit()
        finally:
            con.close()
        return "\n".join("|".join(str(v) for v in r) for r in rows)
    if DRIVER == "mysql":
        return subprocess.run(DBQ["mysql"] + ["-N", "-B", "-e", sql], env=DBQ["env"], capture_output=True, text=True,
                              check=True).stdout
    return subprocess.run(DBQ["psql"] + ["-c", "SET search_path TO %s; %s" % (SCHEMA, sql)], env=DBQ["env"],
                          capture_output=True, text=True, check=True).stdout


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


def http(port, method, path, token=None, body=None):
    """(status, json) of a brain HTTP route."""
    req = urllib.request.Request("http://127.0.0.1:%d%s" % (port, path), method=method,
                                 data=json.dumps(body).encode() if body is not None else None)
    if body is not None:
        req.add_header("Content-Type", "application/json")
    if token:
        req.add_header("Authorization", "Bearer " + token)
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status, json.loads(r.read() or b"null")
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b"null")


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
        check(types.get("daily") == 5 and types.get("weekly") == 3, "5 daily (krunker.io, 0410) + 3 weekly of the rotation: %s" % types)
        check(types.get("map") == 6 and types.get("weapon") == 6 and types.get("nightmare") == 2, "map / weapon / nightmare: %s" % types)
        c1 = by_ci(rows, 1)
        check(c1 and c1["DAT"] == {"ky": "k", "val": 25, "mp": 0} and c1["REW"] == {"kr": 10} and c1["PV"] == 0 and c1["PT"] == 0
              and c1["N"] == "Get 25 kills on Burg", "challenge 1 object: %s" % c1)
        daily_rew = {c["CI"]: c["REW"] for c in rows if c["T"] == "daily"}
        check(all(r.get("bpp") == 50 for r in daily_rew.values()), "every daily gives 50 BPP, as krunker.io (0397, 0409): %s" % daily_rew)
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
            # v10.0.7's UPDATE_CHALLENGE_REWARDS [skins, funds, junk, jnk, kr, titles, rp, rankedPoints]
            # (deob-10.0.7.js:180855-180902, 215154-215197): challenge 2's REW (set below) has jnk, rp and a title
            check(chlr and len(chlr) == 9 and chlr[4] == 2.5 and chlr[3] == 2.5, "chlR: the junk won and the balance: %s" % chlr)
            check(chlr and chlr[6] == [90], "chlR: title 90 granted: %s" % (chlr and chlr[6:]))
            check(chlr and chlr[7] == 40 and chlr[8] == 40, "chlR: 40 ranked points won, balance 40: %s" % (chlr and chlr[6:]))
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

            print("== v10.0.3 badges (shared/src/badges.c: 68, deob-10.0.3.js:115767-116206)")
            r = await s.call("po", "cbdg", 60)
            check(r[2] == "Requirements not met", "KrunkCup Badge without skin 7794: %s" % r)
            err, _ = await n.op("give_skin", id=alice_id, skin=7794, source="test")
            check(err is None, "give skin 7794 (KrunkCup Jerseys): %s" % err)
            r = await s.call("cbdgr", "cbdg", 60)
            check(r[1] == 60, "claim the KrunkCup Badge with skin 7794: %s" % r)
            r = await s.call("po", "cbdg", 61)
            check(r[2] == "Requirements not met", "S10 Battle Pass at 0 BPP: %s" % r)
            r = await s.call("po", "cbdg", 1)
            check(r[2] == "Requirements not met", "Skin Creator I (never met on Box Office): %s" % r)
            r = await s.call("po", "cbdg", 64)
            check(r[2] == "Badge not claimable", "Community Event Winner is a staff badge: %s" % r)
            r = await s.call("po", "cbdg", 68)
            check(r[2] == "Badge not claimable", "past the 68 badges: %s" % r)
            r = await s.call("bdgr", "bdg", 60)
            check(r[1] == 60, "select the KrunkCup Badge: %s" % r)
            r = await s.call("bdgr", "bdg", -1)
            check(r[1] == -1, "clear it again: %s" % r)

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

            print("== hide detailed online status (stats.hds)")
            r = await s.call("hdsr", "hds", True)
            check(r[1] == 1, "hide the detailed status: %s" % r)
            r = await s.call("a", "a", 1, [alice, None, token])
            check(r[4][16].get("hds") == 1, "account stats.hds 1: %s" % r[4][16].get("hds"))
            r = await s.call("hdsr", "hds", False)
            check(r[1] == 0, "show it again: %s" % r)

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

            print("== v10.0.7 mid-match CHALLENGE_COMPLETED (challenges / challenge_reached)")
            menu = (await s.call("chgL", "chg"))[1]
            err, data = await n.op("challenges", id=alice_id)
            check(err is None and [c["CI"] for c in data or []] == [c["CI"] for c in menu],
                  "the game server reads the menu's list: %s" % err)
            idx9 = [c["CI"] for c in menu].index(9)
            err, data = await n.op("challenge_reached", id=alice_id, ci=9)
            check(err is None and data == {"index": idx9, "indexes": [idx9]}, "challenge 9 reached: index %s: %s %s" % (idx9, err, data))
            await s.drain(1.0)
            f = s.take("chgC")
            check(f == ["chgC", idx9, True, by_ci(menu, 9)["REW"]], "chgC [index, true, REW] pushed: %s" % f)
            err, data = await n.op("challenge_reached", id=alice_id, ci=1)
            await s.drain(0.5)
            check(err is None and data == {"index": -1, "indexes": [-1]} and s.take("chgC") is None, "a completed one: no push: %s" % data)
            # one event's frames (deob-10.0.7.js:180731-180741): the chgC of each challenge it completed, in list order,
            # then its UPDATE_CHALLENGES_PROGRESS [CI, progVal, ...]; a progress-only event pushes uchp alone
            idx10 = [c["CI"] for c in menu].index(10)
            err, data = await n.op("challenge_reached", id=alice_id, cis=[9, 10], prog=[9, 50, 10, 15, 11, 7])
            await s.drain(1.0)
            order = [f[0] for f in s.inbox if f[0] in ("chgC", "uchp")]
            f9, f10, u = s.take("chgC"), s.take("chgC"), s.take("uchp")
            check(err is None and data == {"index": idx9, "indexes": [idx9, idx10]} and f9 and f9[1] == idx9 and f10 and f10[1] == idx10
                  and u == ["uchp", [9, 50, 10, 15, 11, 7]] and order == ["chgC", "chgC", "uchp"],
                  "an event with two completions: chgC, chgC, then uchp: %s %s %s" % (data, order, u))
            err, data = await n.op("challenge_reached", id=alice_id, prog=[11, 8])
            await s.drain(0.5)
            u = s.take("uchp")
            check(err is None and data == {"index": -1, "indexes": []} and u == ["uchp", [11, 8]] and s.take("chgC") is None,
                  "progress only: uchp alone: %s %s" % (data, u))
            err, _ = await n.op("challenges", id=0)
            check(err == "Account not found.", "no account: %s" % err)

            print("== v10.0.7 World KrunkCup (0399_krunkcup)")
            # alice owns the KrunkCup Jerseys (given above); no style row: style 0 (Algeria)
            kc = dict(player, prog=[], stats={}, medals={}, kcJersey=True, kills=7, score=900, games=1)
            await n.commit("kc1", True, kc)
            exec_sql("INSERT INTO item_styles (account_id, skin_id, copy_id, style) VALUES (%d, 7794, 0, 5)" % alice_id)
            await n.commit("kc2", True, dict(kc, kills=11, score=1500))
            await n.commit("kc3", True, dict(kc, kills=3, score=200))
            await n.commit("kc4", True, dict(kc, kcJersey=False, kills=50, score=9000))  # not worn: no team
            await asyncio.sleep(0.5)  # the progress work runs after the commit reply
            st, body = http(port, "GET", "/api/leaderboards/krunkcup/teams")
            teams = (body or {}).get("data")
            check(st == 200 and teams == [{"id": 5, "total_score": 1700, "total_kills": 14, "total_games": 2},
                                          {"id": 0, "total_score": 900, "total_kills": 7, "total_games": 1}],
                  "teams by score: %s %s" % (st, body))
            st, body = http(port, "GET", "/leaderboards/krunkcup/teams/5/contributors")
            rows = (body or {}).get("data") or []
            check(st == 200 and len(rows) == 1 and rows[0].get("player_name") == alice and rows[0].get("kills") == 14
                  and rows[0].get("score") == 1700 and rows[0].get("games") == 2 and "player_clan" in rows[0],
                  "team 5's contributors: %s" % body)
            st, body = http(port, "GET", "/api/leaderboards/krunkcup/teams/7/contributors")
            check(st == 200 and body == {"data": []}, "a team nobody played: %s" % body)
            st, body = http(port, "GET", "/api/leaderboards/krunkcup/players?limit=5")
            rows = (body or {}).get("data") or []
            check(st == 200 and len(rows) == 1 and rows[0].get("team") == 5 and rows[0].get("score") == 2600
                  and rows[0].get("games") == 3, "the top players: %s" % body)

            print("== the KrunkCup Participation Band (/krunkcup/wristband)")
            st, body = http(port, "GET", "/krunkcup/wristband")
            check(st == 401, "the band needs a session: %s %s" % (st, body))
            st, body = http(port, "GET", "/krunkcup/wristband", token)
            check(st == 200 and body.get("data") == {"claimed": False, "eligible": True, "team": 5, "item_id": 7803},
                  "status before the claim: %s %s" % (st, body))
            st, body = http(port, "POST", "/krunkcup/wristband/claim", token, {"team": 7})
            check(st == 400 and (body.get("error") or {}).get("message") == "Play a match for that team in its KrunkCup jersey first.",
                  "a team not played for is refused: %s %s" % (st, body))
            st, body = http(port, "POST", "/krunkcup/wristband/claim", token, {"team": 0})
            check(st == 200 and body.get("data") == {"claimed": True, "eligible": True, "team": 0, "item_id": 7803},
                  "claim team 0: %s %s" % (st, body))
            st, body = http(port, "POST", "/krunkcup/wristband/claim", token, {})
            check(st == 400 and (body.get("error") or {}).get("message") == "You already claimed your KrunkCup Participation Band.",
                  "once: %s %s" % (st, body))
            check(exec_sql("SELECT count FROM inventory WHERE account_id = %d AND skin_id = 7803" % alice_id).strip() == "1",
                  "the band is in the inventory")
            check(exec_sql("SELECT count(*) FROM item_styles WHERE account_id = %d AND skin_id = 7803" % alice_id).strip() == "0",
                  "team 0 is the band's default style (no style row)")

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
        DBQ.update(psql=["psql", "-h", os.environ.get("KN_DATABASE_HOST", str(CONFIG_DB.get("host", "127.0.0.1"))),
                         "-p", os.environ.get("KN_DATABASE_PORT", str(CONFIG_DB.get("port", 5432))),
                         "-U", os.environ.get("KN_DATABASE_USER", str(CONFIG_DB.get("user", "krunknative"))),
                         "-d", DB, "-v", "ON_ERROR_STOP=1", "-qAt"],
                   env=dict(os.environ, PGPASSWORD=os.environ.get("KN_DATABASE_PASSWORD", str(CONFIG_DB.get("password", "")))))
    else:
        scratch = [sys.executable, "-I", os.path.join(ROOT, "services", "brain", "tests", "db_scratch.py"), ROOT]
        r = subprocess.run(scratch + ["create", DRIVER, SCHEMA], capture_output=True)
        if r.returncode:
            print("progress test: %s unavailable: skipped" % DRIVER)
            sys.exit(r.returncode)
        dbcfg = json.loads(r.stdout)
        dbenv = {"KN_DATABASE_" + k.upper(): str(v) for k, v in dbcfg.items()}
        drop = scratch + ["drop", DRIVER, SCHEMA]
        DBQ.update(path=dbcfg.get("path", ""),
                   mysql=[shutil.which("mariadb") or shutil.which("mysql") or "mysql", "--no-defaults", "--protocol=TCP", "-h", str(dbcfg.get("host", "127.0.0.1")), "-P", str(dbcfg.get("port", 3306)),
                          "-u", str(dbcfg.get("user", "")), str(dbcfg.get("name", SCHEMA))],
                   env=dict(os.environ, MYSQL_PWD=str(dbcfg.get("password", ""))))
    work = tempfile.mkdtemp(prefix="kn-progress.")
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=SECRET, **dbenv)
    log_path = os.path.join(work, "brain.log")
    proc = None
    try:
        subprocess.run([BRAIN, "--migrate-only"], cwd=ROOT, env=env, check=True, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)
        # challenge 2 (10 headshots on Burg, completed by the commit) also gives junk, ranked points and title 90
        exec_sql("UPDATE challenges SET rew = '{\"kr\":15,\"jnk\":2.5,\"rp\":40,\"title\":[90]}' WHERE challenge_id = 2")
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
