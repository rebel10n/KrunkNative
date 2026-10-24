#!/usr/bin/env python3
"""The live trade of Trade mode on kn_brain (doc/v10/trade.md): the game server's "trade_inventories" and
"live_trade" ops (economy.c, db_economy.c run_live_trade, migration 0357_live_trades) on a scratch database.

  live_trade_test.py <kn_brain> <repo root>
env: KN_TEST_DB_DRIVER=postgres (default: a per-run schema in config/config.json's database) | sqlite | mysql
(db_scratch.py). 77 = skip.

Covers: the window's inventories (v10 rows: stacks {i, c}, a painted copy {i, c: 1, id, m: {paint}}, nt on an
untradable item; funds, level, premium, notrade); a swap of stacks, a painted copy (its paint kept, a new copy id)
and KR with each side's 10 KR tradeCost, in one transaction (removed / added / meta / owned per side, the
live_trades and live_trade_items rows, the ledger); a resent frame (same key) replaying the result and moving
nothing twice; ownership (a missing unit, a copy that is not theirs) refusing the whole trade with nothing moved;
two settles racing for one item (exactly one wins, nothing duplicated); funds for KR + tradeCost; levels, trade
locks, account and 'trade' / 'gift' feature bans; untradable items; the 5 / 20 unit limits; bad requests. Every
balance equals its ledger and KR is conserved but for the fees.
"""
import asyncio
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

BRAIN, ROOT = sys.argv[1], os.path.abspath(sys.argv[2])
DRIVER = os.environ.get("KN_TEST_DB_DRIVER", "postgres")
NAME = "kn_test_live_trade_%d" % os.getpid()
SECRET = "kn-live-trade-test-%d" % os.getpid()
STAMP = str(int(time.time()) % 100000)
FAILS = []
UNTRADABLE = 1483  # 1 Year Cape: assets/skins.json tradable false
LEVEL = lambda lvl: int((lvl / 0.03) ** 2) + 1  # player_score for a level


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what, flush=True)
    if not cond:
        FAILS.append(what)


class Db:
    """Direct SQL for setup and invariants (economy_test.py's)."""

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

    async def __aenter__(self):
        self.ws = await websockets.connect(self.url, max_size=8 << 20)
        return self

    async def __aexit__(self, *a):
        await self.ws.close()

    async def register(self, name):
        await self.ws.send(json.dumps(["a", 0, [name, "live_pass1", None]]))
        deadline = time.time() + 20
        while True:
            f = json.loads(await asyncio.wait_for(self.ws.recv(), deadline - time.time()))
            if f[0] == "a":
                assert f[1] is None, "register %s: %r" % (name, f[1])
                return f[2]


class Node:
    """The game server's side of /mm/internal/ws: hello, then "acct" ops."""

    COUNT = [0]

    def __init__(self, port):
        self.url = "ws://127.0.0.1:%d/mm/internal/ws" % port
        self.rid = 0
        Node.COUNT[0] += 1
        self.node_id = "KLV%d" % Node.COUNT[0]

    async def __aenter__(self):
        self.ws = await websockets.connect(self.url, additional_headers={"Authorization": "Bearer " + SECRET}, max_size=8 << 20)
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
            msg = json.loads(await asyncio.wait_for(self.ws.recv(), 20))
            if msg.get("type") == "acct-r" and msg.get("rid") == rid:
                return msg.get("err"), msg.get("data")


def set_player(db, aid, funds=None, level=None, premium=None, notrade=None):
    if funds is not None:
        cur = db.one("SELECT player_funds FROM players WHERE player_id = $1", aid)
        if funds != cur:  # keep the ledger whole: an 'admin' row for the difference
            db.q("UPDATE players SET player_funds = $2 WHERE player_id = $1", aid, funds)
            db.q("INSERT INTO kr_transactions (account_id, amount, source) VALUES ($1, $2, 'admin')", aid, funds - cur)
    if level is not None:
        db.q("UPDATE players SET player_score = $2 WHERE player_id = $1", aid, LEVEL(level))
    if premium is not None:
        db.q("UPDATE players SET player_premium = $2 WHERE player_id = $1", aid, premium)
    if notrade is not None:
        db.q("UPDATE players SET player_notrade = $2 WHERE player_id = $1", aid, notrade)


def give(db, aid, skin, count=1):
    have = db.one("SELECT count FROM inventory WHERE account_id = $1 AND skin_id = $2", aid, skin)
    if db.q("SELECT 1 FROM inventory WHERE account_id = $1 AND skin_id = $2", aid, skin):
        db.q("UPDATE inventory SET count = $3 WHERE account_id = $1 AND skin_id = $2", aid, skin, have + count)
    else:
        db.q("INSERT INTO inventory (account_id, skin_id, count) VALUES ($1, $2, $3)", aid, skin, count)


def give_copy(db, aid, skin, color):
    """A painted copy (0352 skin_copies): one inventory unit plus its copy row; its copy id."""
    give(db, aid, skin, 1)
    db.q("INSERT INTO skin_copies (account_id, skin_id, paint_color, paint_shader) VALUES ($1, $2, $3, NULL)", aid, skin, color)
    return db.one("SELECT max(copy_id) FROM skin_copies WHERE account_id = $1 AND skin_id = $2", aid, skin)


def funds(db, aid):
    return db.one("SELECT player_funds FROM players WHERE player_id = $1", aid)


def count(db, aid, skin):
    return db.one("SELECT count FROM inventory WHERE account_id = $1 AND skin_id = $2", aid, skin)


def ledger_ok(db, ids):
    bad = []
    for aid in ids:
        f = funds(db, aid)
        s = db.one("SELECT coalesce(sum(amount), 0) FROM kr_transactions WHERE account_id = $1", aid)
        if f != s:
            bad.append((aid, f, s))
    return bad


def side(aid, items, kr=0):
    return {"id": aid, "items": items, "kr": kr}


async def scenario(port, db):
    names = {k: "%s%s" % (k, STAMP) for k in ("lta", "ltb", "ltc", "ltd")}
    ids = {}
    async with Social("ws://127.0.0.1:%d/social/ws" % port) as s:
        for k, n in names.items():
            ids[k] = await s.register(n)
    A, B, C, D = (ids[k] for k in ("lta", "ltb", "ltc", "ltd"))
    for aid in ids.values():
        set_player(db, aid, funds=1000, level=40, premium=0)
    give(db, A, 2, 3)
    give(db, A, 3, 1)
    give(db, A, UNTRADABLE, 1)
    copy = give_copy(db, A, 4, "#dc3c3c")
    give(db, B, 5, 2)
    kr0 = sum(funds(db, i) for i in ids.values())

    async with Node(port) as node, Node(port) as node2:
        # ---- the window's inventories
        err, data = await node.op("trade_inventories", a=A, b=B)
        inv = {(r["i"], r.get("id", 0)): r for r in (data or {}).get("a", {}).get("inv", [])}
        check(err is None and inv.get((2, 0), {}).get("c") == 3, "trade_inventories: A's stack {i: 2, c: 3} (%s)" % err)
        row = inv.get((4, copy), {})
        check(row.get("c") == 1 and row.get("m", {}).get("paint") == "#dc3c3c" and "sid" not in row and "paint_color" not in row,
              "the painted copy in v10's shape {i, c: 1, id, m: {paint}}: %r" % row)
        check(inv.get((UNTRADABLE, 0), {}).get("nt") == 1 and "nt" not in inv.get((2, 0), {}), "nt marks the untradable item only")
        a = data["a"] if data else {}
        check(a.get("funds") == 1000 and a.get("level", 0) >= 10 and a.get("premium") is False and a.get("notrade") is False,
              "A's funds / level / premium / notrade: %r" % {k: a.get(k) for k in ("funds", "level", "premium", "notrade")})
        check([r["i"] for r in data["b"]["inv"]] == [5], "B's inventory: %r" % data["b"]["inv"])
        err, _ = await node.op("trade_inventories", a=A, b=A)
        check(err == "Player not found", "trade_inventories with oneself refused: %r" % err)

        # ---- a swap: A gives 2 x2 + the copy + 100 KR, B gives 5 x1; tradeCost 10 each
        key = "lt:%s0001" % STAMP
        args = dict(key=key, a=side(A, [[2, 2], [4, 1, copy]], 100), b=side(B, [[5, 1]]), cost=10)
        err, res = await node.op("live_trade", **args)
        check(err is None and res and res["a"]["funds"] == 890 and res["b"]["funds"] == 1090,
              "settled: A 1000 - 100 - 10 = 890, B 1000 + 100 - 10 = 1090: %r (%s)" % (res and (res["a"]["funds"], res["b"]["funds"]), err))
        check(funds(db, A) == 890 and funds(db, B) == 1090, "the balances: A %d, B %d" % (funds(db, A), funds(db, B)))
        check(count(db, A, 2) == 1 and count(db, B, 2) == 2 and count(db, A, 5) == 1 and count(db, B, 5) == 1,
              "the stacks moved: A 2 x1 + 5 x1, B 2 x2 + 5 x1")
        check(count(db, A, 4) == 0 and not db.q("SELECT 1 FROM skin_copies WHERE copy_id = $1", copy), "A's copy is gone")
        new = db.q("SELECT copy_id, paint_color FROM skin_copies WHERE account_id = $1 AND skin_id = 4", B)
        check(len(new) == 1 and new[0][1] == "#dc3c3c" and int(new[0][0]) != copy and count(db, B, 4) == 1,
              "B has the copy with its paint under a new copy id: %r" % new)
        new_id = int(new[0][0]) if new else 0
        if res:
            check(sorted(res["a"]["removed"]) == sorted(["2", "2", "4:%d" % copy]) and res["a"]["added"] == ["5"],
                  "A's atrd lists: removed %r, added %r" % (res["a"]["removed"], res["a"]["added"]))
            check(sorted(res["b"]["added"]) == sorted(["2", "2", "4:%d" % new_id]) and res["b"]["removed"] == ["5"] and
                  res["b"]["meta"].get(str(new_id), {}).get("paint") == "#dc3c3c", "B's lists and the new copy's meta: %r" % res["b"])
            owned_a = {s: n for s, n in res["a"]["owned"]}
            owned_b = {s: n for s, n in res["b"]["owned"]}
            check(owned_a.get(2) == 1 and owned_a.get(4) == 0 and owned_a.get(5) == 1 and owned_b.get(4) == 1 and owned_b.get(5) == 1,
                  "owned counts for the server's ENT bits: A %r, B %r" % (owned_a, owned_b))
        lt = db.q("SELECT id, account_a, account_b, kr_a, kr_b, fee FROM live_trades WHERE req_key = $1", key)
        check(len(lt) == 1 and [int(v) for v in lt[0][1:]] == [A, B, 100, 0, 10], "the live_trades row: %r" % lt)
        items = db.q("SELECT side, skin_id, count, copy_id, new_copy_id FROM live_trade_items WHERE trade_id = $1 ORDER BY side, skin_id", int(lt[0][0]) if lt else 0)
        check(len(items) == 3 and items[1][3] == str(copy) and items[1][4] == str(new_id), "live_trade_items: %r" % items)
        fees = db.q("SELECT account_id, amount, fee FROM kr_transactions WHERE source = 'live-trade-fee' ORDER BY account_id")
        check(len(fees) == 2 and all(int(f[1]) == -10 and int(f[2]) == 10 for f in fees), "one tradeCost row per side: %r" % fees)

        # ---- the resent frame (brain.c resends reliable frames): the stored result, nothing twice
        err2, res2 = await node2.op("live_trade", **args)
        check(err2 is None and res2 and res2["a"]["funds"] == 890 and res2["b"]["funds"] == 1090, "the same key replays the result")
        check(funds(db, A) == 890 and count(db, B, 2) == 2 and db.one("SELECT count(*) FROM live_trades WHERE req_key = $1", key) == 1,
              "and moves nothing twice")

        # ---- ownership: a missing unit / a copy that is not theirs refuses everything
        before = (count(db, A, 3), count(db, B, 5), funds(db, A), funds(db, B))
        err, _ = await node.op("live_trade", key="lt:%s0002" % STAMP, a=side(A, [[3, 2]], 5), b=side(B, [[5, 1]]), cost=10)
        check(err == "Item not owned", "A gives 2 of the 1 unit it has: %r" % err)
        check((count(db, A, 3), count(db, B, 5), funds(db, A), funds(db, B)) == before, "nothing moved, not even B's side or the KR")
        err, _ = await node.op("live_trade", key="lt:%s0003" % STAMP, a=side(A, [[4, 1, new_id]]), b=side(B, []), cost=10)
        check(err and "copy" in err.lower(), "A offers B's copy: %r" % err)
        check(count(db, B, 4) == 1 and db.q("SELECT 1 FROM skin_copies WHERE copy_id = $1 AND account_id = $2", new_id, B),
              "B keeps the copy")

        # ---- a double spend: two settles race for A's one unit of skin 3
        r1, r2 = await asyncio.gather(
            node.op("live_trade", key="lt:%s0004" % STAMP, a=side(A, [[3, 1]]), b=side(B, []), cost=10),
            node2.op("live_trade", key="lt:%s0005" % STAMP, a=side(A, [[3, 1]]), b=side(C, []), cost=10))
        wins = [r for r in (r1, r2) if r[0] is None]
        check(len(wins) == 1 and [r for r in (r1, r2) if r[0]][0][0] == "Item not owned",
              "exactly one of two racing settles gets the item: %r" % ([r[0] for r in (r1, r2)],))
        check(count(db, A, 3) + count(db, B, 3) + count(db, C, 3) == 1, "the unit exists once")

        # ---- funds: KR + tradeCost
        set_player(db, D, funds=5)
        give(db, D, 2, 1)
        err, _ = await node.op("live_trade", key="lt:%s0006" % STAMP, a=side(A, [[2, 1]]), b=side(D, [[2, 1]]), cost=10)
        check(err == "Not enough KR" and count(db, D, 2) == 1, "a side without its 10 KR tradeCost: %r" % err)
        err, _ = await node.op("live_trade", key="lt:%s0007" % STAMP, a=side(A, [], 881), b=side(D, [[2, 1]]), cost=10)
        check(err == "Not enough KR" and funds(db, A) == 880, "A's 881 KR + 10 > 880: %r" % err)
        set_player(db, D, funds=1000)

        # ---- who may trade
        set_player(db, C, level=9)
        err, _ = await node.op("live_trade", key="lt:%s0008" % STAMP, a=side(A, [[2, 1]]), b=side(C, []), cost=10)
        check(err == "Both players must be level 10 or higher to trade.", "level 9 refused: %r" % err)
        set_player(db, C, level=40)
        set_player(db, A, notrade=1)
        err, _ = await node.op("live_trade", key="lt:%s0009" % STAMP, a=side(A, [[2, 1]]), b=side(C, []), cost=10)
        check(err == "Account is trade locked", "a trade-locked a: %r" % err)
        set_player(db, A, notrade=0)
        set_player(db, C, notrade=1)
        err, _ = await node.op("live_trade", key="lt:%s0010" % STAMP, a=side(A, [[2, 1]]), b=side(C, []), cost=10)
        check(err == "Player can not trade", "a trade-locked b: %r" % err)
        set_player(db, C, notrade=0)
        db.q("INSERT INTO feature_bans (account_id, feature, reason) VALUES ($1, 'trade', 'test')", C)
        err, _ = await node.op("live_trade", key="lt:%s0011" % STAMP, a=side(A, [[2, 1]]), b=side(C, []), cost=10)
        check(err == "Player can not trade", "b with a 'trade' feature ban: %r" % err)
        err, _ = await node.op("live_trade", key="lt:%s0012" % STAMP, a=side(C, [], 0), b=side(A, [[2, 1]]), cost=10)
        check(err == "You are banned from using the trade", "a with a 'trade' feature ban: %r" % err)
        db.q("UPDATE feature_bans SET revoked_at = created_at WHERE account_id = $1", C)
        db.q("INSERT INTO feature_bans (account_id, feature, reason) VALUES ($1, 'gift', 'test')", C)
        err, _ = await node.op("live_trade", key="lt:%s0013" % STAMP, a=side(A, [[2, 1]]), b=side(C, [], 50), cost=10)
        check(err == "You are banned from using the KR gifts", "KR from a gift-banned side: %r" % err)
        db.q("UPDATE feature_bans SET revoked_at = created_at WHERE account_id = $1", C)

        # ---- the items and the limits
        err, _ = await node.op("live_trade", key="lt:%s0014" % STAMP, a=side(A, [[UNTRADABLE, 1]]), b=side(B, []), cost=10)
        check(err == "One or more items in this offer cannot be traded." and count(db, A, UNTRADABLE) == 1, "an untradable item: %r" % err)
        give(db, D, 6, 25)
        err, _ = await node.op("live_trade", key="lt:%s0015" % STAMP, a=side(D, [[6, 6]]), b=side(B, []), cost=10)
        check(err == "You cannot include that many items in a single trade.", "6 units on a non-premium side: %r" % err)
        set_player(db, D, premium=1)
        err, _ = await node.op("live_trade", key="lt:%s0016" % STAMP, a=side(D, [[6, 20]]), b=side(B, []), cost=10)
        check(err is None and count(db, B, 6) == 20, "20 units on a premium side (tradeMaxOutP): %r" % err)
        err, _ = await node.op("live_trade", key="lt:%s0017" % STAMP, a=side(D, [[6, 5]]), b=side(B, []), cost=10)
        check(err == "You cannot include that many items in a single trade." or err == "Item not owned" or err is None,
              "21+ units: refused or within the stock: %r" % err)
        err, _ = await node.op("live_trade", key="lt:%s0018" % STAMP, a=side(A, []), b=side(B, []), cost=10)
        check(err == "Your trade offer is empty or invalid.", "an empty trade: %r" % err)
        err, _ = await node.op("live_trade", key="nope", a=side(A, [[2, 1]]), b=side(B, []), cost=10)
        check(err == "Invalid request", "a key that is not the game server's: %r" % err)
        err, _ = await node.op("live_trade", key="lt:%s0019" % STAMP, a=side(A, [[2, 1]]), b=side(A, []), cost=10)
        check(err == "Player not found", "a trade with oneself: %r" % err)
        err, _ = await node.op("live_trade", key="lt:%s0020" % STAMP, a=side(A, [[4, 1, 5]]), b=side(B, []), cost=10)
        check(err == "Your trade offer is empty or invalid.", "a copy id below the copy range: %r" % err)

    bad = ledger_ok(db, ids.values())
    check(not bad, "every balance equals its ledger: %r" % bad)
    fees = db.one("SELECT coalesce(sum(fee), 0) FROM kr_transactions WHERE source = 'live-trade-fee'")
    admin = db.one("SELECT coalesce(sum(amount), 0) FROM kr_transactions WHERE source = 'admin' AND account_id IN (%s)"
                   % ",".join(str(i) for i in ids.values())) - 4000
    kr1 = sum(funds(db, i) for i in ids.values())
    check(kr1 == kr0 - fees + admin, "KR conserved but for the tradeCost fees (%d -> %d, fees %d, admin %+d)" % (kr0, kr1, fees, admin))
    check(db.one("SELECT count(*) FROM players WHERE player_funds < 0") == 0, "no negative balance")


def main():
    scratch = [sys.executable, "-I", os.path.join(ROOT, "services", "brain", "tests", "db_scratch.py"), ROOT]
    r = subprocess.run(scratch + ["create", DRIVER, NAME], capture_output=True)
    if r.returncode:
        print("%s unavailable (%s): skip" % (DRIVER, r.stderr.decode(errors="replace").strip()))
        return r.returncode
    dbcfg = json.loads(r.stdout)
    tmp = tempfile.mkdtemp(prefix="kn-live-trade-test-", dir=os.path.expanduser("~/.cache"))
    cfg = {
        "database": dict(dbcfg, pool_size=8, migrations_dir=os.path.join(ROOT, "services", "db", "migrations")),
        "brain": {"host": "127.0.0.1", "port": 0, "internal_secret": SECRET, "federation": False},
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
            text = fp.read()
        errs = [l for l in text.splitlines() if " ERROR " in l or "deadlock" in l.lower()]
        if errs:
            print("brain log errors: %r" % errs[:5])
        subprocess.run(scratch + ["drop", DRIVER, NAME], capture_output=True)
    print("live trade test: " + ("OK" if rc == 0 else "%d failure(s), log %s" % (len(FAILS), log_path)))
    if rc == 0:
        import shutil
        shutil.rmtree(tmp, ignore_errors=True)
    return rc


sys.exit(main())
