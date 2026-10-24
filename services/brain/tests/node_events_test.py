#!/usr/bin/env python3
"""The game servers' v10.0.3 event ops and pushes on kn_brain (services/brain/node_events.c, node_events.h): what
KrunkNative's game server needs to send warsClan, spinW / unbS, unb, lotWin and chreq as 10.0.3's server does.

  node_events_test.py <kn_brain> <repo root>
env: KN_TEST_DB_DRIVER=postgres (default: a per-run schema in config/config.json's database) | sqlite | mysql
(db_scratch.py). 77 = skip.

Covers:
  turf_clan      nobody holds a map (no settled day, not a turf map); the winner of yesterday's turf_days holds it
                 {clan_id, clan_name, clan_logo (the approved version or null), clan_banner_color}; an older day does not
  map_spin       a KR spin of store wheel 0 for a map PRIZE_WHEEL: {skin, rand, funds, rarity, wheel, price}, the KR and
                 the skin in one transaction with their ledger rows (source 'spin'); a ticket-only / missing wheel and
                 "Not enough KR" move nothing; two spins racing for the last 50 KR: exactly one wins
  studio_game    a published map in a monetized studio / an unmonetized one / none
  studio_charge  the KR of a consented charge: the ledger row (source 'studio-charge', ref = key), the studio_charges row
                 with the 90 % share, the "ua" push; the same key again replays without a second debit; "Not enough KR",
                 an unmonetized game and bad amounts refuse with nothing written; the studio's Analytics (KR Earned,
                 Gross spent)
  unbox          a store spin and a Free KR spin of an account in a game: {accountId, item, kr, rarity} to its node; an
                 account in no game (or after its player-leave) sends nothing
  raffle-win     a lottery draw's prizes to the winner's node {accountId, lotteryId, drawId, prize {type, ref, quantity}}
Every balance equals its ledger.
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
NAME = "kn_test_node_events_%d" % os.getpid()
SECRET = "kn-node-events-test-%d" % os.getpid()
STAMP = str(int(time.time()) % 100000)
TURF_DAY = 200          # brain.turf.day: the controller is turf_days of day 199
LOTTERY = 3             # the seeded Anniversary raffle (0348_lottery), skin cost 7782
FREE_KR = [5, 10, 20, 500]
FAILS = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what, flush=True)
    if not cond:
        FAILS.append(what)


class Db:
    """Direct SQL for setup and invariants (live_trade_test.py's)."""

    def __init__(self, cfg):
        self.cfg = cfg

    def q(self, sql, *args):
        def lit(a):
            if a is None:
                return "NULL"
            if isinstance(a, int):
                return str(a)
            return "'" + str(a).replace("'", "''") + "'"
        text = re.sub(r"\$(\d+)", lambda m: lit(args[int(m.group(1)) - 1]), sql)
        d = self.cfg["driver"]
        if d == "sqlite":
            con = sqlite3.connect(self.cfg["path"], timeout=30)
            try:
                rows = con.execute(text).fetchall()
                con.commit()
            finally:
                con.close()
            return [tuple(None if v is None else str(v) for v in r) for r in rows]
        if d == "postgres":
            env = dict(os.environ, PGPASSWORD=str(self.cfg.get("password", "")), PGOPTIONS="-c search_path=" + self.cfg["search_path"])
            cmd = ["psql", "-h", str(self.cfg.get("host", "127.0.0.1")), "-p", str(self.cfg.get("port", 5432)), "-U",
                   str(self.cfg.get("user", "krunknative")), "-d", self.cfg["name"], "-v", "ON_ERROR_STOP=1", "-qAtX",
                   "-F", "\x1f", "-c", text]
        else:
            env = dict(os.environ, MYSQL_PWD=str(self.cfg.get("password", "")))
            cmd = [os.environ.get("KN_TEST_MYSQL_CLIENT", "mariadb"), "--no-defaults", "--protocol=TCP", "-h", str(self.cfg.get("host")),
                   "-P", str(self.cfg.get("port")), "-u", str(self.cfg.get("user")), "-D", self.cfg["name"], "-N", "-B", "-e", text]
        r = subprocess.run(cmd, env=env, capture_output=True, timeout=60)
        if r.returncode:
            raise RuntimeError("sql failed: %s: %s" % (text[:120], r.stderr.decode(errors="replace")))
        sep = "\x1f" if d == "postgres" else "\t"
        return [tuple(None if v in ("", "NULL") and d != "postgres" else v for v in line.split(sep))
                for line in r.stdout.decode().splitlines()]

    def one(self, sql, *args):
        rows = self.q(sql, *args)
        return int(rows[0][0]) if rows and rows[0][0] not in (None, "") else 0


class Social:
    def __init__(self, url):
        self.url = url + "?enc=json"
        self.inbox = []

    async def __aenter__(self):
        self.ws = await websockets.connect(self.url, max_size=8 << 20)
        return self

    async def __aexit__(self, *a):
        await self.ws.close()

    async def call(self, label, *frame, timeout=15):
        if frame:
            await self.ws.send(json.dumps(list(frame)))
        deadline = time.time() + timeout
        while True:
            for i, f in enumerate(self.inbox):
                if f[0] == label:
                    return self.inbox.pop(i)
            self.inbox.append(json.loads(await asyncio.wait_for(self.ws.recv(), deadline - time.time())))

    async def register(self, name):
        r = await self.call("a", "a", 0, [name, "nodeevents_pass1", 0], "captcha")
        assert r[1] is None, "register %s: %r" % (name, r[1])
        return r[2], r[5]

    async def gapi(self, rid, method, path, body=None):
        f = await self.call("gapi", "gapi", rid, method, path, body)
        return f[2], (f[3] or {}).get("data"), (f[3] or {}).get("error")


class Node:
    """The game server's side of /mm/internal/ws: hello, "acct" ops, the brain's pushes."""

    COUNT = [0]

    def __init__(self, port):
        self.url = "ws://127.0.0.1:%d/mm/internal/ws" % port
        self.rid = 0
        Node.COUNT[0] += 1
        self.node_id = "KNE%d" % Node.COUNT[0]
        self.inbox = []

    async def __aenter__(self):
        self.ws = await websockets.connect(self.url, additional_headers={"Authorization": "Bearer " + SECRET}, max_size=8 << 20)
        await self.send({"type": "hello", "nodeId": self.node_id, "region": "local", "publicHost": "127.0.0.1",
                         "publicPort": 1, "version": "test", "maxPlayers": 8})
        while json.loads(await asyncio.wait_for(self.ws.recv(), 10)).get("type") != "welcome":
            pass
        return self

    async def __aexit__(self, *a):
        await self.ws.close()

    async def send(self, frame):
        await self.ws.send(json.dumps(frame))

    async def op(self, op, **args):
        self.rid += 1
        rid = self.rid
        await self.send({"type": "acct", "rid": rid, "op": op, "args": args})
        f = await self.wait(lambda m: m.get("type") == "acct-r" and m.get("rid") == rid, 20)
        return (f.get("err"), f.get("data")) if f else ("timeout", None)

    async def wait(self, pred, secs):
        deadline = time.time() + secs
        while True:
            for i, m in enumerate(self.inbox):
                if pred(m):
                    return self.inbox.pop(i)
            left = deadline - time.time()
            if left <= 0:
                return None
            try:
                self.inbox.append(json.loads(await asyncio.wait_for(self.ws.recv(), left)))
            except asyncio.TimeoutError:
                return None

    async def pushes(self, kind, secs):
        """every `kind` push that arrives within secs"""
        out = []
        while True:
            m = await self.wait(lambda f: f.get("type") == kind, secs)
            if not m:
                return out
            out.append(m)
            secs = 0.5


def http(port, method, path, body=None, token=None):
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
        try:
            return e.code, json.loads(e.read() or b"null")
        except ValueError:
            return e.code, None


def set_funds(db, aid, funds):
    cur = db.one("SELECT player_funds FROM players WHERE player_id = $1", aid)
    if funds != cur:  # keep the ledger whole: an 'admin' row for the difference
        db.q("UPDATE players SET player_funds = $2 WHERE player_id = $1", aid, funds)
        db.q("INSERT INTO kr_transactions (account_id, amount, source) VALUES ($1, $2, 'admin')", aid, funds - cur)


def funds(db, aid):
    return db.one("SELECT player_funds FROM players WHERE player_id = $1", aid)


def ledger_ok(db, ids):
    return [aid for aid in ids if funds(db, aid) != db.one("SELECT coalesce(sum(amount), 0) FROM kr_transactions WHERE account_id = $1", aid)]


async def turf(db, node):
    err, data = await node.op("turf_clan", map=0)
    check(err is None and data is None, "turf_clan: nobody holds Burg before a day is settled (%r %r)" % (err, data))
    owner = db.one("SELECT id FROM accounts ORDER BY id LIMIT 1")
    db.q("INSERT INTO clans (clan_name, clan_playerid) VALUES ($1, $2)", "NE" + STAMP[-3:], owner)
    clan = db.one("SELECT clan_id FROM clans WHERE clan_name = $1", "NE" + STAMP[-3:])
    # the brain's timer settles the finished days at its start (nobody played: no clan); the test names the winners
    for day in (TURF_DAY - 1, TURF_DAY - 2):
        deadline = time.time() + 15
        while time.time() < deadline and not db.one("SELECT count(*) FROM turf_days WHERE day = $1 AND map_id = 0", day):
            await asyncio.sleep(0.2)
    db.q("UPDATE turf_days SET clan_id = $2, kills = 12, total_kills = 20 WHERE day = $1 AND map_id = 0", TURF_DAY - 1, clan)
    db.q("UPDATE turf_days SET clan_id = $2, kills = 3, total_kills = 3 WHERE day = $1 AND map_id = 2", TURF_DAY - 2, clan)
    err, data = await node.op("turf_clan", map=0)
    check(err is None and data == {"clan_id": clan, "clan_name": "NE" + STAMP[-3:], "clan_logo": None, "clan_banner_color": None},
          "turf_clan: yesterday's winner holds Burg, no logo, no banner colour (%r)" % (data,))
    db.q("UPDATE clans SET clan_logo = 4, clan_banner_color = '#ff8800' WHERE clan_id = $1", clan)
    err, data = await node.op("turf_clan", map=0)
    check(err is None and data and data.get("clan_logo") == 4 and data.get("clan_banner_color") == "#ff8800",
          "turf_clan: the approved logo version and the banner paint (%r)" % (data,))
    err, data = await node.op("turf_clan", map=2)
    check(err is None and data is None, "turf_clan: a win of the day before yesterday holds nothing today (%r)" % (data,))
    err, data = await node.op("turf_clan", map=1)
    check(err is None and data is None, "turf_clan: a map that is no turf map is held by nobody (%r)" % (data,))


async def map_spin(db, node, node2, a):
    set_funds(db, a, 1000)
    err, data = await node.op("map_spin", id=a, wheel=0)
    ok = err is None and isinstance(data, dict)
    check(ok and data["wheel"] == 0 and data["price"] == 50 and data["funds"] == 950 and 0 <= data["rand"] < 100 and data["skin"] > 0
          and isinstance(data["rarity"], int), "map_spin wheel 0 (Starter, 50 KR): {skin, rand, funds 950, rarity, wheel, price} (%r %r)" % (err, data))
    skin = data["skin"] if ok else -1
    check(funds(db, a) == 950 and db.one("SELECT count FROM inventory WHERE account_id = $1 AND skin_id = $2", a, skin) >= 1,
          "map_spin: the KR is taken and the skin is in the inventory")
    check(db.one("SELECT count(*) FROM kr_transactions WHERE account_id = $1 AND amount = -50 AND source = 'spin' AND ref = '0:50'", a) == 1
          and db.one("SELECT count(*) FROM skin_transactions WHERE account_id = $1 AND skin_id = $2 AND source = 'spin' AND delta = 1", a, skin) == 1,
          "map_spin: its ledger rows (kr_transactions -50 'spin' '0:50', skin_transactions +1 'spin': the spin history)")
    for wheel, why in ((1, "ticket-only Elite"), (3, "no map wheel"), (-1, "no wheel")):
        err, data = await node.op("map_spin", id=a, wheel=wheel)
        check(err == "Wheel unavailable" and data is None, "map_spin: the %s is refused (%r)" % (why, err))
    set_funds(db, a, 100)
    err, data = await node.op("map_spin", id=a, wheel=2)
    check(err == "Not enough KR" and funds(db, a) == 100, "map_spin: Heroic (500 KR) with 100 KR: 'Not enough KR', nothing taken (%r)" % err)
    err, data = await node.op("map_spin", id=0, wheel=0)
    check(err == "Account not found.", "map_spin: no account (%r)" % err)
    # two spins for the last 50 KR: the funds bound of the one transaction lets exactly one through
    set_funds(db, a, 50)
    rows = db.one("SELECT count(*) FROM kr_transactions WHERE account_id = $1 AND source = 'spin'", a)
    (e1, d1), (e2, d2) = await asyncio.gather(node.op("map_spin", id=a, wheel=0), node2.op("map_spin", id=a, wheel=0))
    wins = [d for e, d in ((e1, d1), (e2, d2)) if e is None]
    check(len(wins) == 1 and "Not enough KR" in (e1, e2) and funds(db, a) == 0
          and db.one("SELECT count(*) FROM kr_transactions WHERE account_id = $1 AND source = 'spin'", a) == rows + 1,
          "map_spin: two racing spins for the last 50 KR: one wins, one 'Not enough KR' (%r / %r)" % (e1, e2))


async def studios(db, node, a, owner, sock_a, sock_o):
    db.q("INSERT INTO studios (name, owner_id, monetization) VALUES ($1, $2, 1)", "NeStudio" + STAMP, owner)
    db.q("INSERT INTO studios (name, owner_id, monetization) VALUES ($1, $2, 0)", "NeFree" + STAMP, owner)
    sid = db.one("SELECT id FROM studios WHERE name = $1", "NeStudio" + STAMP)
    free = db.one("SELECT id FROM studios WHERE name = $1", "NeFree" + STAMP)
    db.q("INSERT INTO studio_members (studio_id, account_id, role) VALUES ($1, $2, 1)", sid, owner)
    for name, studio in (("NeMap" + STAMP, sid), ("NeFreeMap" + STAMP, free), ("NeLone" + STAMP, None)):
        db.q("INSERT INTO published_maps (map_name, creator_id) VALUES ($1, $2)", name, owner)
        if studio:
            db.q("INSERT INTO studio_maps (map_id, studio_id, added_by) VALUES ($1, $2, $3)",
                 db.one("SELECT map_id FROM published_maps WHERE map_name = $1", name), studio, owner)
    mid = db.one("SELECT map_id FROM published_maps WHERE map_name = $1", "NeMap" + STAMP)
    err, data = await node.op("studio_game", map="nemap" + STAMP)
    check(err is None and data == {"map_id": mid, "studio_id": sid, "monetized": True},
          "studio_game: a map of a monetized studio (any case) is monetized (%r)" % (data,))
    err, data = await node.op("studio_game", map="NeFreeMap" + STAMP)
    check(err is None and data and data["monetized"] is False and data["studio_id"] == free, "studio_game: an unmonetized studio's map (%r)" % (data,))
    err, data = await node.op("studio_game", map="NeLone" + STAMP)
    check(err is None and data and data["monetized"] is False and data["studio_id"] == 0, "studio_game: a map in no studio (%r)" % (data,))
    err, data = await node.op("studio_game", map="NoSuchMap" + STAMP)
    check(err is None and data == {"map_id": 0, "studio_id": 0, "monetized": False}, "studio_game: no such published map (%r)" % (data,))

    set_funds(db, a, 400)
    sock_a.inbox.clear()
    err, data = await node.op("studio_charge", id=a, map="NeMap" + STAMP, amount=100, key="sc:t1" + STAMP)
    check(err is None and data == {"success": True, "charged": 100, "earned": 90, "duplicate": False, "krBalance": 300},
          "studio_charge 100 KR: {success, charged 100, earned 90, krBalance 300}, no account object to the node (%r %r)" % (err, data))
    check(funds(db, a) == 300 and db.one("SELECT count(*) FROM kr_transactions WHERE account_id = $1 AND amount = -100"
                                         " AND source = 'studio-charge' AND ref = $2", a, "sc:t1" + STAMP) == 1,
          "studio_charge: the player's ledger row (-100, 'studio-charge', ref = the key)")
    check(db.q("SELECT studio_id, map_id, account_id, amount, earned FROM studio_charges WHERE req_key = $1", "sc:t1" + STAMP)
          == [(str(sid), str(mid), str(a), "100", "90")], "studio_charge: the studio_charges row (studio, map, account, 100, 90)")
    ua = None
    try:
        ua = await sock_a.call("ua", timeout=5)
    except asyncio.TimeoutError:
        pass
    check(ua is not None, "studio_charge: the account's social socket gets the 'ua' push (the menu's KR)")
    err, data = await node.op("studio_charge", id=a, map="NeMap" + STAMP, amount=100, key="sc:t1" + STAMP)
    check(err is None and data and data["success"] is True and data["duplicate"] is True and data["krBalance"] == 300 and funds(db, a) == 300
          and db.one("SELECT count(*) FROM studio_charges WHERE req_key = $1", "sc:t1" + STAMP) == 1,
          "studio_charge: the same key again replays (duplicate) and takes nothing (%r)" % (data,))
    for args, want, why in ((dict(amount=500, key="sc:t2" + STAMP, map="NeMap" + STAMP), "Not enough KR", "500 KR with 300"),
                            (dict(amount=10, key="sc:t3" + STAMP, map="NeFreeMap" + STAMP), "Monetization is not enabled for this studio", "an unmonetized game"),
                            (dict(amount=10, key="sc:t4" + STAMP, map="NeLone" + STAMP), "Monetization is not enabled for this studio", "a game in no studio"),
                            (dict(amount=0, key="sc:t5" + STAMP, map="NeMap" + STAMP), "Invalid charge", "0 KR"),
                            (dict(amount=100001, key="sc:t6" + STAMP, map="NeMap" + STAMP), "Invalid charge", "more than 100000 KR"),
                            (dict(amount=10, key="", map="NeMap" + STAMP), "Invalid charge", "no key")):
        err, data = await node.op("studio_charge", id=a, **args)
        check(err == want and data is None, "studio_charge: %s is refused '%s' (%r)" % (why, want, err))
    check(funds(db, a) == 300 and db.one("SELECT count(*) FROM studio_charges") == 1, "studio_charge: the refusals wrote nothing")
    st, an, e = await sock_o.gapi(71, "GET", "/studios/%d/analytics" % sid)
    check(st == 200 and an and an.get("kr_charged") == 100 and an.get("kr_earned") == 90,
          "the studio's Analytics: Gross spent 100, KR Earned 90 (%r %r)" % (st, an))


async def unboxes(db, port, node, a, b, sock_a, sock_b, b_tok):
    await node.send({"type": "player-join", "lobbyId": "L1", "accountId": a})
    await asyncio.sleep(0.3)
    set_funds(db, a, 1000)
    r = await sock_a.call("spin", "spin", 0, None, False)
    check(r[1] is None, "a store spin of an account in a game (%r)" % (r[:2],))
    push = await node.wait(lambda m: m.get("type") == "unbox", 5)
    check(push is not None and push.get("accountId") == a and push.get("item") == r[3] and push.get("kr") == 0 and isinstance(push.get("rarity"), int),
          "unbox: the store win {accountId, item, kr 0, rarity} reaches the account's node (%r)" % (push,))
    r = await sock_a.call("spinKR", "fkr")
    push = await node.wait(lambda m: m.get("type") == "unbox", 5)
    check(r[1] is not None and push is not None and push.get("item") == r[3] and push.get("rarity") == r[3]
          and push.get("kr") == FREE_KR[r[3]], "unbox: the Free KR win {item = rarity = its freeKR index, kr} (%r %r)" % (r, push))
    set_funds(db, b, 1000)
    await sock_b.call("spin", "spin", 0, None, False)
    stray = await node.pushes("unbox", 1.5)
    check(not stray, "unbox: an account in no game sends nothing (%r)" % (stray,))
    await node.send({"type": "player-leave", "lobbyId": "L1", "accountId": a})
    await asyncio.sleep(0.3)
    await sock_a.call("spin", "spin", 0, None, False)
    stray = await node.pushes("unbox", 1.5)
    check(not stray, "unbox: nothing after the account's player-leave (%r)" % (stray,))

    # the lottery: b enters the seeded raffle in a game; the timer draws the window it entered
    await node.send({"type": "player-join", "lobbyId": "L1", "accountId": b})
    await asyncio.sleep(0.3)
    db.q("UPDATE lotteries SET active = 1 WHERE lottery_id = $1", LOTTERY)
    db.q("INSERT INTO inventory (account_id, skin_id, count) VALUES ($1, 7782, 1)", b)
    st, body = http(port, "GET", "/api/lottery/%d" % LOTTERY)
    st, body = http(port, "POST", "/api/lottery/%d/participate" % LOTTERY, {"cost_type": "skin", "cost_ref": "7782", "cost_quantity": 1}, b_tok)
    check(st == 200, "b enters the raffle (%r %r)" % (st, body))
    draw = db.one("SELECT id FROM lottery_draws WHERE lottery_id = $1 AND drawn_s IS NULL", LOTTERY)
    db.q("UPDATE lottery_draws SET starts_s = starts_s - 200000, ends_s = ends_s - 200000 WHERE id = $1", draw)
    wins = await node.pushes("raffle-win", 30)
    paid = db.q("SELECT prize_ref FROM lottery_prizes WHERE draw_id = $1 AND winner_id = $2", draw, b)
    check(wins and len(wins) == len(paid) and all(w.get("accountId") == b and w.get("lotteryId") == LOTTERY and w.get("drawId") == draw
                                                  and w.get("prize", {}).get("type") == "skin" and w["prize"].get("quantity") == 1 for w in wins)
          and sorted(w["prize"]["ref"] for w in wins) == sorted(r[0] for r in paid),
          "raffle-win: each prize of the draw reaches the winner's node {accountId, lotteryId, drawId, prize {skin, ref, 1}} (%d of %d)"
          % (len(wins), len(paid)))


async def scenario(port, db):
    social = "ws://127.0.0.1:%d/social/ws" % port
    async with Social(social) as sock_a, Social(social) as sock_b, Social(social) as sock_o:
        a, _ = await sock_a.register("nea" + STAMP)
        b, b_tok = await sock_b.register("neb" + STAMP)
        owner, _ = await sock_o.register("neo" + STAMP)
        async with Node(port) as node, Node(port) as node2:
            await turf(db, node)
            await map_spin(db, node, node2, a)
            await studios(db, node, a, owner, sock_a, sock_o)
            await unboxes(db, port, node, a, b, sock_a, sock_b, b_tok)
        bad = ledger_ok(db, [a, b, owner])
        check(not bad, "every balance equals its ledger (%r)" % bad)


def main():
    scratch = [sys.executable, "-I", os.path.join(ROOT, "services", "brain", "tests", "db_scratch.py"), ROOT]
    r = subprocess.run(scratch + ["create", DRIVER, NAME], capture_output=True)
    if r.returncode:
        print("%s unavailable (%s): skip" % (DRIVER, r.stderr.decode(errors="replace").strip()))
        return r.returncode
    dbcfg = json.loads(r.stdout)
    tmp = tempfile.mkdtemp(prefix="kn-node-events-test-", dir=os.path.expanduser("~/.cache"))
    cfg = {
        "database": dict(dbcfg, pool_size=8, migrations_dir=os.path.join(ROOT, "services", "db", "migrations")),
        "brain": {"host": "127.0.0.1", "port": 0, "internal_secret": SECRET, "federation": False,
                  "turf": {"day": TURF_DAY}, "lottery": {"settle_check_ms": 1000}},
        "accounts": {"signup_rewards": {"enabled": False}, "password_iterations": 1000, "login_throttle_s": 0, "starting_kr": 0,
                     "free_premium": False, "skins_file": os.path.join(ROOT, "assets", "skins.json")},
    }
    with open(os.path.join(tmp, "config.json"), "w") as fp:
        json.dump(cfg, fp)
    log_path = os.path.join(tmp, "brain.log")
    log = open(log_path, "w")
    proc = subprocess.Popen([BRAIN, "--config", os.path.join(tmp, "config.json"), "--migrate"], stdout=log, stderr=subprocess.STDOUT,
                            cwd=ROOT, start_new_session=True)
    rc = 1
    try:
        port = None
        for _ in range(1200):
            if proc.poll() is not None:
                raise RuntimeError("kn_brain exited early")
            with open(log_path) as fp:
                m = re.search(r"listening on http://127\.0\.0\.1:(\d+) \(ws", fp.read())
            if m:
                port = int(m.group(1))
                break
            time.sleep(0.05)
        if not port:
            raise RuntimeError("kn_brain did not start listening")
        asyncio.run(asyncio.wait_for(scenario(port, Db(dbcfg)), 300))
        rc = 1 if FAILS else 0
    except Exception as e:  # noqa: BLE001
        import traceback
        traceback.print_exc()
        print("FAIL exception: %r" % (e,))
        rc = 1
    finally:
        proc.terminate()
        try:
            proc.wait(10)
        except subprocess.TimeoutExpired:
            proc.kill()
        log.close()
        with open(log_path) as fp:
            errs = [l for l in fp.read().splitlines() if " ERROR " in l or "deadlock" in l.lower()]
        if errs:
            print("brain log errors: %r" % errs[:5])
            rc = rc or 1
        subprocess.run(scratch + ["drop", DRIVER, NAME], capture_output=True)
    print("node events test: " + ("OK" if rc == 0 else "%d failure(s), log %s" % (len(FAILS), log_path)))
    if rc == 0:
        shutil.rmtree(tmp, ignore_errors=True)
    return rc


sys.exit(main())
