#!/usr/bin/env python3
"""Krunker v7.1.4 seasons and the Battle Pass on kn_brain (doc/backend.md 7.8; services/brain/
season.c, db_season.c, migration 0108_seasons): "bpdl" / "bppd", the match BPP of a stats commit
(capped per day) and the challenges' REW.bpp ("bpEnd" = tmpBPData), claims ("bpclm"), the VIP
tier and level skips paid in KR through the wallet ("xstbp" / "xstbpl" -> kr_transactions), the
season badges (Founder, Battle Pass Master, Badge Master), the profile's season block ("sst"),
the season leaderboards and the season rollover (a restart with accounts.current_season 8).

  season_test.py <kn_brain> <repo root> [database]
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
    print("season test: no database configured; skipped")
    sys.exit(77)
SCHEMA = "kn_test_kse_%d" % os.getpid()
SECRET = "kn-season-test-%d" % os.getpid()
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
            return [str(r[0]) for r in con.execute(sql).fetchall()]
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
        await self.ws.send(json.dumps({"type": "hello", "nodeId": "KSE", "region": "local", "publicHost": "127.0.0.1",
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


async def register(sock, name):
    r = await sock.call("a", "a", 0, [name, "seasonpass1", 0], "captcha")
    assert r[1] is None, r
    return r[2], r[5], r


async def funds_of(sock, name, token):
    r = await sock.call("a", "a", 1, [name, None, token])
    return r[4][5]


async def give_kr(node, account_id, kr):
    while kr > 0:  # the "rewards" op caps one reward at accounts.max_match_kr (500)
        step = min(kr, 500)
        err, _ = await node.op("rewards", items=[[account_id, step]])
        assert err is None, err
        kr -= step


def items_of(bp):
    parts = [int(x) for x in bp["itemdata"].split(",") if x != ""]
    return [parts[i:i + 4] for i in range(0, len(parts) - 3, 4)]


async def wait_push(sock, label, seconds=3.0):
    deadline = time.time() + seconds
    while time.time() < deadline:
        f = sock.take(label)
        if f:
            return f
        await sock.drain(0.2)
    return None


async def scenario(port):
    stamp = str(int(time.time()) % 100000)
    alice, bob = "ksea" + stamp, "kseb" + stamp
    social = "ws://127.0.0.1:%d/social/ws" % port
    async with Social(social) as g:
        print("== bpdl as a guest")
        r = await g.call("bpdl", "bpdl")
        bp = r[1]
        check(bp and bp["id"] == 2 and bp["season"] == 7, "the Season 7 pass (currentBattlepass 2): %s" % (bp and bp.get("id")))
        check(bp and bp["itemcount"] * bp["bpp_perlevel"] >= 15000, "the pass reaches the 15000 BPP of Battle Pass Master")
        check(bp and bp["tier1name"] == "VIP" and bp["tier1price"] > 0 and bp["skipprice"] > 0, "VIP tier and skip prices in KR")
        check(bp and all(k in bp for k in ("name", "startdate", "enddate", "itemdata", "tier0name")), "v7's BattlePassPayload keys")
        await g.drain(0.3)
        check(g.take("bppd") is None, "no account.BP for a guest")
        r = await g.call("po", "bpclm", 1)
        check(r[2] == "Must be logged in", "bpclm needs an account: %s" % r)
        r = await g.call("purbp", "xstbp", 1, None)
        check(r[2] == {"err": "Must be logged in"}, "xstbp needs an account: %s" % r)

    per, count = bp["bpp_perlevel"], bp["itemcount"]
    items = items_of(bp)
    free1 = [it for it in items if it[1] == 0 and it[2] == 0 and it[3] > 0]
    vip1 = [it for it in items if it[1] == 1 and it[2] == 0 and it[3] > 0]
    vip_last = [it for it in items if it[1] == 1 and it[3] > 0]
    vip_last = max(vip_last, key=lambda it: it[2]) if vip_last else None
    check(free1 and vip1 and vip_last, "level 1 has a free and a VIP card")

    async with Social(social) as s, Node(port) as n:
        alice_id, token, _ = await register(s, alice)
        print("== bpdl + bppd")
        r = await s.call("bpdl", "bpdl")
        pr = await s.call("bppd")
        check(pr[1] == {"id": 2, "bpp": 0, "tier": 0, "items_claimed": ""}, "a new account.BP: %s" % pr[1])

        print("== claims")
        r = await s.call("bpclmr", "bpclm", free1[0][0])
        check(r[3] == free1[0][3] and r[6]["id"] == free1[0][0] and r[6]["tier"] == 0 and r[6]["col"] == 0,
              "claimItemRes: the level 1 free skin %s" % r)
        pr = await s.call("bppd")
        check(pr[1]["items_claimed"] == str(free1[0][0]), "items_claimed: %s" % pr[1]["items_claimed"])
        r = await s.call("po", "bpclm", free1[0][0])
        check(r[2] == "Item already claimed", "claimed twice: %s" % r)
        r = await s.call("po", "bpclm", vip1[0][0])
        check(r[2] == "VIP Tier required", "a VIP card without the tier: %s" % r)
        lvl2 = [it for it in items if it[1] == 0 and it[2] == 1 and it[3] > 0]
        if lvl2:
            r = await s.call("po", "bpclm", lvl2[0][0])
            check(r[2] == "Level not reached", "level 2 at 0 BPP: %s" % r)
        inv = query("SELECT count FROM inventory WHERE account_id = %d AND skin_id = %d" % (alice_id, free1[0][3]))
        check(inv == ["1"], "the skin is in the inventory: %s" % inv)

        print("== the match BPP (stats commit)")
        player = {"accountId": alice_id, "name": alice, "kills": 10, "deaths": 4, "score": 1500, "won": True,
                  "timePlayed": 240000, "games": 1, "kr": 0, "xp": 1500, "classIndex": 0, "stats": {},
                  "prog": [["k", 10, -1, -1, -1]], "medals": {}, "mapIndex": 0, "ranked": False, "bpp": 70}
        reply = await n.commit("s1", True, player)
        check(reply.get("ok"), "commit ok")
        end = await wait_push(s, "bpEnd")
        check(end and end[1]["gn"] == 70 and end[1]["pbpp"] == 0 and end[1]["bpp"] == 70 and end[1]["mx"] == per
              and end[1]["ml"] == count and end[1]["id"] == 2, "bpEnd (tmpBPData): %s" % (end and end[1]))
        check(end and end[1]["rbpp"] == 500 - 70 and end[1]["unbpp"] == 70, "remaining daily cap: %s" % (end and end[1]))
        nxt = [it for it in items if it[2] == 1 and it[3] > 0]
        nxt = sorted(nxt, key=lambda it: -it[1])[0] if nxt else None  # the VIP card first
        want = {"id": nxt[0], "sk": nxt[3], "tier": nxt[1], "col": 1} if nxt else None
        check(end and end[1].get("re") == want, "next level reward (level 2): %s" % (end and end[1].get("re")))
        pr = await wait_push(s, "bppd")
        check(pr and pr[1]["bpp"] == 70, "bppd after the match: %s" % (pr and pr[1]))
        for i in range(2, 9):  # the day's cap: 500 BPP (v10.0.2)
            await n.commit("s%d" % i, True, player)
        await s.drain(1.5)
        s.inbox.clear()
        r = await s.call("bpdl", "bpdl")
        pr = await s.call("bppd")
        check(pr[1]["bpp"] == 500, "the daily cap stops at 500 BPP: %s" % pr[1]["bpp"])
        await n.commit("s1", True, player)  # a duplicate commit
        await s.drain(1.0)
        check(s.take("bpEnd") is None, "no BPP for a duplicate commit")
        await n.commit("s9#c1", False, dict(player, bpp=None, kills=5, games=0, won=False, score=0))  # a left match: no endGame BPP
        await s.drain(1.0)
        check(s.take("bpEnd") is None, "no tmpBPData for a player who left")

        print("== the VIP tier in KR")
        funds0 = await funds_of(s, alice, token)
        await s.drain(0.5)
        s.inbox.clear()
        r = await s.call("purbp", "xstbp", 1, None)
        check(r[2] == {"err": "Not enough KR"} and r[3] == "1", "VIP without the KR: %s" % r)
        price = bp["tier1price"]
        await give_kr(n, alice_id, price + 100 - funds0)
        r = await s.call("purbp", "xstbp", 1, None)
        check(r[1] == "kr" and r[2] is None and r[4] == 1 and r[5] == price and r[6] == "KR", "purchaseRes: %s" % r)
        pr = await s.call("bppd")
        check(pr[1]["tier"] == 1, "tier 1: %s" % pr[1])
        nb = await wait_push(s, "nbdgr")
        check(nb and 26 in nb[1], "Season 7 Founder (bought in the first week): %s" % (nb and nb[1]))
        ua = await wait_push(s, "ua")
        check(ua and ua[2].get("funds") == 100, "ua: the header's KR after the purchase: %s" % (ua and ua[2].get("funds")))
        tx = query("SELECT amount FROM kr_transactions WHERE account_id = %d AND source = 'battlepass'" % alice_id)
        check(tx == [str(-price)], "kr_transactions: one 'battlepass' debit of %d: %s" % (price, tx))
        r = await s.call("purbp", "xstbp", 1, None)
        check(r[2] == {"err": "You already own this Battle Pass"}, "a second VIP purchase: %s" % r)
        check(await funds_of(s, alice, token) == 100, "no second debit")
        r = await s.call("bpclmr", "bpclm", vip1[0][0])
        check(r[3] == vip1[0][3], "the level 1 VIP card with the tier: %s" % r)

        print("== level skips in KR")
        r = await s.call("purbpl", "xstbpl", 1)
        check(r[2] == {"err": "Not enough KR"}, "skip without the KR: %s" % r)
        r = await s.call("purbpl", "xstbpl", count)
        check(r[2] == {"err": "Invalid amount of levels"}, "skip past the last level: %s" % r)
        levels = max(1, count - (500 // per + 1))  # increaseSkip's limit at bpp 500
        skip = bp["skipprice"]
        await give_kr(n, alice_id, levels * skip)
        r = await s.call("purbpl", "xstbpl", levels)
        check(r[2] is None and r[4] == levels and r[5] == min(500 + levels * per, count * per) and r[6] == levels * skip,
              "purchaseLvlRes: %s" % r[:8])
        tx = query("SELECT amount FROM kr_transactions WHERE account_id = %d AND source = 'battlepass-skip' ORDER BY id" % alice_id)
        check(tx and tx[0] == str(-levels * skip), "kr_transactions: the 'battlepass-skip' debit: %s" % tx)
        bpp_now = r[5]
        if bpp_now < count * per:  # on the last level: skipping it completes the pass
            await give_kr(n, alice_id, skip)
            r = await s.call("purbpl", "xstbpl", 1)
            check(r[2] is None and r[5] == count * per, "the last level skipped: %s" % r[:8])
        nb = await wait_push(s, "nbdgr")
        badges = nb[1] if nb else []
        check(count * per < 15000 or (27 in badges and 28 in badges), "S7 Battle Pass Master + S7 Badge Master: %s" % badges)
        r = await s.call("bpclmr", "bpclm", vip_last[0])
        check(r[3] == vip_last[3], "the last VIP card after the skips: %s" % r)
        r = await s.call("purbpl", "xstbpl", 1)
        check(r[2] == {"err": "Battle Pass already complete"}, "skip on a complete pass: %s" % r)

        print("== found skins (give_skin, v7 game.giveSkin)")
        err, data = await n.op("give_skin", id=alice_id, skin=5923, source="mapreward")
        check(err is None and data and data.get("skin") == 5923 and data.get("count") == 1, "the Season 7 map reward skin: %s %s" % (err, data))
        err, _ = await n.op("give_skin", id=alice_id, skin=999999, source="trigger")
        check(err == "Unknown skin.", "a skin the store does not have: %s" % err)

        print("== season stats and leaderboards")
        r = await s.call("sst", "sst", alice)
        st = r[1]
        check(st and st["season"] == 7 and st["kills"] == 85 and st["games"] == 8 and st["wins"] == 8 and st["score"] == 12000,
              "sst: 8 matches + the left one's kills, the duplicate not counted: %s" % st)
        check(st and st["tier"] == 1 and st["level"] == count, "sst: VIP, level %s" % (st and st.get("level")))
        r = await s.call("0", "r", "leaders", "season_score", 0, "", None, 0)
        rows = r[3]
        check(rows and rows[0]["player_name"] == alice and int(rows[0]["season_score"]) == 12000,
              "the season score leaderboard: %s" % (rows[:1] if rows else rows))
        r = await s.call("sst", "sst", "nobody" + stamp)
        check(r[1] is None, "sst of an unknown player: %s" % r)

    async with Social(social) as b:
        bob_id, _, _ = await register(b, bob)
        r = await b.call("po", "cbdg", 26)
        check(r[2] == "Requirements not met", "Founder without the VIP tier: %s" % r)
    return alice


def start_brain(env, log_path, extra):
    with open(log_path, "a") as log:
        log.write("== start %s\n" % extra)
        proc = subprocess.Popen([BRAIN, "--brain.host=127.0.0.1", "--brain.port=0", "--brain.id=kse-%d" % os.getpid()] + extra,
                                cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT)
    return proc, bound_port(log_path, proc)


async def rollover(port, name):
    async with Social("ws://127.0.0.1:%d/social/ws" % port) as s:
        r = await s.call("bpdl", "bpdl")
        check(r[1] is None, "season 8 has no pass yet: bpdl null")
        r = await s.call("sst", "sst", name, 7)
        check(r[1] and r[1]["season"] == 7 and r[1]["games"] == 8, "season 7 stats kept after the rollover: %s" % r[1])
        r = await s.call("sst", "sst", name)
        check(r[1] and r[1]["season"] == 8 and r[1]["games"] == 0, "season 8 starts empty: %s" % r[1])


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
            print("season test: %s unavailable: skipped" % DRIVER)
            sys.exit(r.returncode)
        DBCFG = json.loads(r.stdout)
        dbenv = {"KN_DATABASE_" + k.upper(): str(v) for k, v in DBCFG.items()}
        drop = scratch + ["drop", DRIVER, SCHEMA]
    work = tempfile.mkdtemp(prefix="kn-season.", dir=os.path.join(os.path.expanduser("~"), ".cache"))
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=SECRET, **dbenv)
    log_path = os.path.join(work, "brain.log")
    proc = None
    try:
        subprocess.run([BRAIN, "--migrate-only"], cwd=ROOT, env=env, check=True, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)
        # the scenario is the Season 7 pass (its cards, Founder / Master badges, map reward skin): the brain's
        # default season is 10 (0354_season_10, SEASON_CURRENT), so it is pinned to 7 here
        proc, port = start_brain(env, log_path, ["--accounts.current_season=7"])
        name = asyncio.run(scenario(port))
        proc.terminate()
        proc.wait(5)
        print("== restart: the season row is kept (insert only)")
        proc, port = start_brain(env, log_path, ["--accounts.current_season=7"])
        seasons = query("SELECT season FROM seasons ORDER BY season")
        check(seasons == ["7", "10"], "the migrations' season rows (7, 10) and no second row 7 after two starts: %s" % seasons)
        proc.terminate()
        proc.wait(5)
        print("== rollover: accounts.current_season 8")
        proc, port = start_brain(env, log_path, ["--accounts.current_season=8"])
        for _ in range(50):
            seasons = query("SELECT season FROM seasons ORDER BY season")
            if seasons == ["7", "8", "10"]:
                break
            time.sleep(0.1)
        check(seasons == ["7", "8", "10"], "season 8 recorded: %s" % seasons)
        ended = query("SELECT season FROM seasons WHERE ends_at IS NOT NULL")
        check(ended == ["7"], "season 7 closed by the rollover (10 is newer: still open): %s" % ended)
        asyncio.run(rollover(port, name))
    finally:
        if proc:
            proc.terminate()
            proc.wait(5)
        subprocess.run(drop)
    with open(log_path) as f:
        bad = [l.strip() for l in f if re.search(r"failed:|ERROR|FATAL", l)]
    check(not bad, "no statement errors in the brain log %s" % bad[:3])
    print("season test: " + ("OK" if not FAILS else "%d failure(s)" % len(FAILS)))
    if FAILS:
        print("log kept in " + work)
    else:
        shutil.rmtree(work, ignore_errors=True)
    sys.exit(1 if FAILS else 0)


main()
