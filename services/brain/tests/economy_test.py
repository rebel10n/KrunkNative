#!/usr/bin/env python3
"""The economy end to end on the social socket (doc/v7/economy.md): KR gifts, the market and
trade offers against kn_brain on a scratch database, with concurrency tests.

  economy_test.py <kn_brain> <repo root>
env: KN_TEST_DB_DRIVER=postgres (default: a per-run schema in config/config.json's database,
     127.0.0.1:5432 in development) | sqlite | mysql (db_scratch.py). 77 = skip.

Covers the v7.1.4 rules (gift min / fee ceil / level 10 (v10; v7.1.4 30), list fee ceil (v10.0.7's list modal; v7.1.4 floor) / level 10 (v7.1.4 15) / MAX / sale
grace, non-tradable items, quick sell, trade level 10 on both sides (v7.1.4 20), 5 / 20 items per side, the
per-rarity trade fee, premium), KrunkNative's KR in offers + escrow, the idempotency keys (a
replayed frame does not move anything twice), feature bans, account bans, trade locks, rate
limits, offer expiry, a pre-0111 (non-escrowed) offer, the ledger (one kr_transactions row per
movement with counterparty and fee: per account, the sum of its rows equals its balance) and
parallel gifts / buys / offers / accepts at the same balance and items: KR is conserved minus
fees, nothing is duplicated, no balance goes negative.
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
NAME = "kn_test_econ_%d" % os.getpid()
FAILS = []
STAMP = str(int(time.time()) % 100000)


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what, flush=True)
    if not cond:
        FAILS.append(what)


# ---------------------------------------------------------------- direct database access (setup + invariants)

class Db:
    def __init__(self, cfg):
        self.cfg = cfg

    def q(self, sql, *args):
        """Rows as tuples of strings (None for NULL). args are inlined as integer / quoted text."""
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
        out = []
        for line in r.stdout.decode().splitlines():
            out.append(tuple(None if v in ("", "NULL") and d != "postgres" else v for v in line.split(sep)))
        return out

    def one(self, sql, *args):
        rows = self.q(sql, *args)
        return int(rows[0][0]) if rows and rows[0][0] not in (None, "") else 0


# ---------------------------------------------------------------- social socket (JSON mode, like the KrunkNative clients)

class Social:
    def __init__(self, url):
        self.url = url + "?enc=json"
        self.inbox = []

    async def __aenter__(self):
        self.ws = await websockets.connect(self.url, max_size=8 << 20)
        return self

    async def __aexit__(self, *a):
        await self.ws.close()

    async def send(self, *frame):
        await self.ws.send(json.dumps(list(frame)))

    async def recv(self, *labels, timeout=20.0):
        deadline = time.time() + timeout
        while True:
            for i, f in enumerate(self.inbox):
                if f[0] in labels:
                    return self.inbox.pop(i)
            left = deadline - time.time()
            if left <= 0:
                raise AssertionError("timeout waiting for %r (inbox %r)" % (labels, [f[0] for f in self.inbox]))
            self.inbox.append(json.loads(await asyncio.wait_for(self.ws.recv(), left)))

    async def call(self, labels, *frame, timeout=20.0):
        await self.send(*frame)
        return await self.recv(*(labels if isinstance(labels, tuple) else (labels,)), timeout=timeout)

    async def login(self, name, password):
        r = await self.call("a", "a", 1, [name, password, None])
        assert r[1] is None, "login %s: %r" % (name, r[1])
        return r


async def register(url, name):
    async with Social(url) as s:
        r = await s.call("a", "a", 0, [name, "econ_pass1", None])
        assert r[1] is None, "register %s: %r" % (name, r[1])
        return r[2]


def op():
    op.n += 1
    return {"op": "t%s-%d" % (STAMP, op.n)}


op.n = 0


async def user(url, name):
    s = Social(url)
    await s.__aenter__()
    await s.login(name, "econ_pass1")
    return s


# ---------------------------------------------------------------- setup helpers

LEVEL = lambda lvl: int((lvl / 0.03) ** 2) + 1  # player_score for a level (Account level formula)


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


GIVEN = [0]  # items created by give() (outside the economy)


def give(db, aid, skin, count=1):
    GIVEN[0] += count
    have = db.one("SELECT count FROM inventory WHERE account_id = $1 AND skin_id = $2", aid, skin)
    if db.q("SELECT 1 FROM inventory WHERE account_id = $1 AND skin_id = $2", aid, skin):
        db.q("UPDATE inventory SET count = $3 WHERE account_id = $1 AND skin_id = $2", aid, skin, have + count)
    else:
        db.q("INSERT INTO inventory (account_id, skin_id, count) VALUES ($1, $2, $3)", aid, skin, count)


def funds(db, aid):
    return db.one("SELECT player_funds FROM players WHERE player_id = $1", aid)


def junk(db, aid):
    """The Junk wallet's balance in hundredths (doc/v10/junk.md)."""
    return db.one("SELECT player_junk FROM players WHERE player_id = $1", aid)


def count(db, aid, skin):
    return db.one("SELECT count FROM inventory WHERE account_id = $1 AND skin_id = $2", aid, skin)


def ledger_ok(db, ids):
    """Per account: the sum of its kr_transactions rows equals its balance."""
    bad = []
    for aid in ids:
        f = funds(db, aid)
        s = db.one("SELECT coalesce(sum(amount), 0) FROM kr_transactions WHERE account_id = $1", aid)
        if f != s:
            bad.append((aid, f, s))
    return bad


def totals(db, ids):
    """(KR held + KR in open escrow + fees paid, items held + listed + in escrow) of the accounts."""
    idl = ",".join(str(i) for i in ids)
    kr = db.one("SELECT coalesce(sum(player_funds), 0) FROM players WHERE player_id IN (%s)" % idl)
    esc = db.one("SELECT coalesce(sum(sender_kr), 0) FROM trades WHERE status = 'open' AND escrow = 1 AND sender_id IN (%s)" % idl)
    fees = db.one("SELECT coalesce(sum(fee), 0) FROM kr_transactions WHERE account_id IN (%s)" % idl)
    items = db.one("SELECT coalesce(sum(count), 0) FROM inventory WHERE account_id IN (%s)" % idl)
    listed = db.one("SELECT count(*) FROM market_listings WHERE sold_at IS NULL AND cancelled_at IS NULL AND seller_id IN (%s)" % idl)
    titems = db.one("SELECT coalesce(sum(i.count), 0) FROM trade_items i JOIN trades t ON t.id = i.trade_id"
                    " WHERE t.status = 'open' AND t.escrow = 1 AND i.side = 's' AND t.sender_id IN (%s)" % idl)
    return kr + esc + fees, items + listed + titems


def po_text(r):
    return r[2] if r and r[0] == "po" and len(r) > 2 else None


# ---------------------------------------------------------------- scenarios

async def blackm_requirements(url, db, c, names, C, D):
    """v10.0.7's checkReq kinds beyond accAge / stats (deob-10.0.7.js:103566-103580; db_economy.c run_blackm_buy):
    accVar (funds / kills / jobRatingPos / followers / timePlayed / trades), rank (peak MMR over the ranked seasons)
    and ownSkins (every reqSkins index owned)."""
    async def buy(item):
        return await c.call(("bmr", "po"), "pbm", item, op())

    def bought(r, item, cf, price):
        return r[0] == "bmr" and r[1] and r[1]["item"] == item and r[1]["f"] == cf - price and count(db, C, item) == 1

    set_player(db, C, funds=50000)
    # accVar: not met (a fresh account has 0 of each; funds 50000 < 1000000)
    for item, what in ((2995, "funds"), (3059, "kills"), (2998, "jobRatingPos"), (3863, "followers"), (3852, "timePlayed"),
                       (2996, "trades")):
        r = await buy(item)
        check(po_text(r) == "Access Denied" and count(db, C, item) == 0, "black market accVar %s not met: %r" % (what, r))
    # accVar: met, each from the players column v10's account reads it from
    for item, col, amount, price, what in ((3059, "player_kills", 50000, 10000, "kills"),
                                            (2998, "jobratingpositive", 150, 5000, "jobRatingPos"),
                                            (3863, "player_followed", 10000, 5000, "followers"),
                                            (3852, "player_timeplayed", 2592000000, 5000, "timePlayed")):
        db.q("UPDATE players SET %s = $2 WHERE player_id = $1" % col, C, amount - 1)
        r = await buy(item)
        check(po_text(r) == "Access Denied", "black market accVar %s one short: %r" % (what, r))
        db.q("UPDATE players SET %s = $2 WHERE player_id = $1" % col, C, amount)
        cf = funds(db, C)
        r = await buy(item)
        check(bought(r, item, cf, price), "black market accVar %s met: %r" % (what, r))
    # accVar trades: completed trades (accepted offers + settled Trade mode trades, db_completed_trades)
    if DRIVER == "mysql":  # MariaDB stops a recursive CTE at max_recursive_iterations (1000 here)
        ins = ("SET STATEMENT max_recursive_iterations = 10000 FOR INSERT INTO live_trades (req_key, account_a, account_b)"
               " WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < $3) SELECT CONCAT('bm', i), $1, $2 FROM n")
    else:
        ins = ("WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < $3)"
               " INSERT INTO live_trades (req_key, account_a, account_b) SELECT 'bm' || i, $1, $2 FROM n")
    db.q(ins, C, D, 4999)
    r = await buy(2996)
    check(po_text(r) == "Access Denied", "black market accVar trades: 4999 trades short: %r" % (r,))
    db.q("INSERT INTO live_trades (req_key, account_a, account_b) VALUES ('bm5000', $1, $2)", D, C)  # either side counts
    cf = funds(db, C)
    r = await buy(2996)
    check(bought(r, 2996, cf, 10000), "black market accVar trades met: %r" % (r,))
    async with Social(url) as s:
        r = await s.login(names["cat"], "econ_pass1")
        check(isinstance(r[6], dict) and r[6].get("trades") == 5000, "account info carries trades (v10 account.trades): %r"
              % (r[6].get("trades") if isinstance(r[6], dict) else r[6],))
    # accVar funds: the balance itself
    set_player(db, C, funds=1000000)
    r = await buy(2995)
    check(bought(r, 2995, 1000000, 10000), "black market accVar funds met: %r" % (r,))
    # rank: Emperor's Blade (1 KR), peakMMR >= 4700, the best over the account's seasons
    r = await buy(4531)
    check(po_text(r) == "Access Denied", "black market rank: no ranked profile: %r" % (r,))
    db.q("INSERT INTO ranked_profiles (account_id, season, region, peak_mmr) VALUES ($1, 1, 'eu', 4699)", C)
    r = await buy(4531)
    check(po_text(r) == "Access Denied", "black market rank: peak 4699 < 4700: %r" % (r,))
    db.q("INSERT INTO ranked_profiles (account_id, season, region, peak_mmr) VALUES ($1, 2, 'na', 4700)", C)
    cf = funds(db, C)
    r = await buy(4531)
    check(bought(r, 4531, cf, 1), "black market rank met (another season's peak): %r" % (r,))
    # ownSkins: Ranked Tamer (100 KR) needs all of 7722..7727 owned
    for skin in (7722, 7723, 7724, 7725, 7726):
        give(db, C, skin)
    r = await buy(7618)
    check(po_text(r) == "Access Denied" and count(db, C, 7618) == 0, "black market ownSkins: one of six missing: %r" % (r,))
    give(db, C, 7727)
    cf = funds(db, C)
    r = await buy(7618)
    check(bought(r, 7618, cf, 100), "black market ownSkins met: %r" % (r,))
    r = await buy(8004)
    check(po_text(r) == "Access Denied", "black market ownSkins: Krunkuku Tamer, none owned: %r" % (r,))


async def scenario(url, db):
    names = {k: "%s%s" % (k, STAMP) for k in ("ann", "bob", "cat", "dan", "eve", "fay", "gus")}
    ids = {}
    for k, n in names.items():
        ids[k] = await register(url, n)
    A, B, C, D, E, F, G = (ids[k] for k in ("ann", "bob", "cat", "dan", "eve", "fay", "gus"))
    for aid in ids.values():
        set_player(db, aid, funds=10000, level=40, premium=0)

    a, b, c = await user(url, names["ann"]), await user(url, names["bob"]), await user(url, names["cat"])
    try:
        # ---- rules
        r = await a.call("ecfg", "ecfg")
        cfg = r[1]
        check(cfg["giftFee"] == 10 and cfg["giftMin"] == 10 and cfg["giftMinLvl"] == 10 and cfg["marketFee"] == 10
              and cfg["marketMinLvl"] == 10 and cfg["tradeMinLvl"] == 10 and cfg["tradeMaxOut"] == 5 and cfg["tradeMaxOutP"] == 20,
              "ecfg reports the v7.1.4 rules: %r" % cfg)

        # ---- KR gifts
        r = await a.call(("gir", "po"), "kr", names["bob"], 105, "hi bob", op())
        check(r[0] == "gir" and r[1]["funds"] == 10000 - 105 - 11 and r[1]["fee"] == 11, "gift: fee = ceil(10%%) on top: %r" % r)
        check(funds(db, B) == 10105, "gift credited the receiver")
        row = db.q("SELECT amount, counterparty_id, fee, source FROM kr_transactions WHERE account_id = $1 AND source = 'gift-out'", A)
        check(row and int(row[0][0]) == -116 and int(row[0][1]) == B and int(row[0][2]) == 11, "gift ledger row: amount, counterparty, fee %r" % row)
        while (mail := await b.recv("gmc"))[1] < 1:  # the login push said 0
            pass
        check(mail[1] >= 1, "receiver's unread mail count pushed")
        note = await b.recv("econN")
        check(note[1] == "gift" and "105 KR" in note[2], "receiver gets the gift note: %r" % note)
        same = op()
        r1 = await a.call(("gir", "po"), "kr", names["bob"], 50, "", same)
        r2 = await a.call(("gir", "po"), "kr", names["bob"], 50, "", same)
        check(r1[0] == "gir" and r2[0] == "gir" and r1[1]["funds"] == r2[1]["funds"] and funds(db, A) == 10000 - 116 - 55,
              "a retried gift (same op) replays the result and moves nothing twice")
        notes = []
        await asyncio.sleep(0.3)
        while True:
            try:
                notes.append(await b.recv("econN", timeout=0.3))
            except (AssertionError, asyncio.TimeoutError):
                break
        check(len(notes) == 1 and "50 KR" in notes[0][2], "the replay notifies nobody again: %r" % notes)
        r = await a.call(("gir", "po"), "kr", names["bob"], 9, "", op())
        check(po_text(r) == "10 KR minimum", "gift below giftMin: %r" % (r,))
        r = await a.call(("gir", "po"), "kr", names["ann"], 50, "", op())
        check(po_text(r) == "Cant gift yourself", "self gift refused")
        r = await a.call(("gir", "po"), "kr", "nobody_" + STAMP, 50, "", op())
        check(po_text(r) == "Player not found", "unknown receiver")
        r = await a.call(("gir", "po"), "kr", names["bob"], 100000, "", op())
        check(po_text(r) == "Not enough KR", "gift over the balance refused: %r" % (r,))
        set_player(db, A, level=9)
        r = await a.call(("gir", "po"), "kr", names["bob"], 50, "", op())
        check(po_text(r) == "Must be level 10", "gift needs level 10 (v10): %r" % (r,))
        set_player(db, A, level=40)
        db.q("INSERT INTO feature_bans (account_id, feature, reason) VALUES ($1, 'gift', 'test')", A)
        r = await a.call(("gir", "po"), "kr", names["bob"], 50, "", op())
        check(po_text(r) == "You are banned from using the KR gifts", "feature ban (gift) enforced: %r" % (r,))
        r = await a.call(("lir", "po"), "li", 3, 3, 1, 100, op())
        db.q("UPDATE feature_bans SET revoked_at = created_at WHERE account_id = $1", A)
        give(db, A, 3, 1)
        r = await a.call(("lir", "po"), "li", 3, 3, 1, 100, op())
        check(r[0] == "lir" and r[1].get("listingId"), "a gift ban leaves the market alone; revoked ban lifts it: %r" % (r,))
        set_player(db, A, notrade=1)
        r = await a.call(("gir", "po"), "kr", names["bob"], 50, "", op())
        check(po_text(r) == "Account is trade locked", "trade lock (player_notrade) blocks gifts")
        set_player(db, A, notrade=0)
        set_player(db, B, notrade=1)
        r = await a.call(("gir", "po"), "kr", names["bob"], 50, "", op())
        check(po_text(r) == "Player can not receive gifts", "a trade-locked receiver gets no gifts")
        set_player(db, B, notrade=0)
        note = "g" * 500
        r = await a.call(("gir", "po"), "kr", names["bob"], 10, note, op())
        check(r[0] == "gir" and db.q("SELECT body FROM mail WHERE account_id = $1 AND subject = 'KR Gift' ORDER BY id DESC LIMIT 1", B)[0][0] == note,
              "a 500-character gift note is kept (deob-10.0.7.js:111703)")
        r = await a.call(("gir", "po"), "kr", names["bob"], 10, "h" * 600, op())
        check(r[0] == "gir" and db.q("SELECT body FROM mail WHERE account_id = $1 AND subject = 'KR Gift' ORDER BY id DESC LIMIT 1", B)[0][0] == "h" * 500,
              "a gift note past 500 characters is cut there")
        r = await a.call(("gir", "po"), "kr", names["bob"], 10, "  padded note  ", op())
        check(r[0] == "gir" and db.q("SELECT body FROM mail WHERE account_id = $1 AND subject = 'KR Gift' ORDER BY id DESC LIMIT 1", B)[0][0] == "padded note",
              "a gift note is trimmed (deob-10.0.7.js:111688)")
        r = await a.call(("gir", "po"), "kr", names["bob"], 10, "   ", op())
        blank = db.q("SELECT body FROM mail WHERE account_id = $1 AND subject = 'KR Gift' ORDER BY id DESC LIMIT 1", B)[0][0]
        check(r[0] == "gir" and not blank, "a blank gift note is stored empty (%r)" % (blank,))

        # ---- market
        give(db, C, 4, 3)    # Moon Dragon, legendary
        give(db, C, 3, 1)
        set_player(db, C, level=9)
        r = await c.call(("lir", "po"), "li", 4, 4, 7, 1000, op())
        check(po_text(r) == "Must be level 10", "listing needs marketMinLVl 10 (v10): %r" % (r,))
        set_player(db, C, level=40)
        before = funds(db, C)
        r = await c.call(("lir", "po"), "li", 4, 4, 7, 1009, op())
        check(r[0] == "lir" and r[2] == 7 and r[1]["fee"] == 101 and r[1]["funds"] == before - 101 and r[1]["cnt"] == 2,
              "listed: fee ceil(10%%) of 1009 is 101, item leaves the inventory: %r" % (r,))
        listing = r[1]["listingId"]
        held, left = funds(db, C), count(db, C, 4)
        r = await c.call(("lir", "po"), "li", 4, 4, 9, 50, op())
        check(po_text(r) == "Price must be at least 100 KR for this rarity" and funds(db, C) == held and count(db, C, 4) == left,
              "a legendary under its sell value is refused (deob-10.0.7.js:109201-109202): %r" % (r,))
        r = await c.call(("lir", "po"), "li", 4, 4, 7, 0, op())
        check(po_text(r) == "Invalid price", "price 0 refused")
        r = await c.call(("lir", "po"), "li", 4, 4, 7, 10000001, op())
        check(po_text(r) == "Invalid price", "price over saleMax refused")
        free = json.load(open(os.path.join(ROOT, "assets", "skins.json")))
        nosale = next(i for i, x in enumerate(free) if x.get("noSale") and not x.get("opensea") and not x.get("blackM"))
        give(db, C, nosale, 1)
        r = await c.call(("lir", "po"), "li", nosale, nosale, 7, 50, op())
        check(po_text(r) == "Item cant be sold.", "noSale item cannot be listed: %r" % (r,))
        # v10's tradable bit wins over the v7.1.4 rule: Gold Plated (an opensea item v10 trades) lists
        gold = next((i for i, x in enumerate(free) if x.get("opensea") and x.get("tradable")), None)
        if gold is not None:
            give(db, C, gold, 1)
            r = await c.call(("lir", "po"), "li", gold, gold, 7, 5000, op())
            check(r[0] == "lir" and r[1].get("listingId"), "a v10 tradable opensea item lists: %r" % (r,))
            if r[0] == "lir" and r[1].get("listingId"):
                await c.call(("ulir", "po"), "uli", gold, r[1]["listingId"], 7, op())
        r = await b.call(("br", "po"), "pi", listing, 3, op())
        check(po_text(r) == "Item not available yet", "saleGrace: not buyable right after posting: %r" % (r,))
        db.q("UPDATE market_listings SET created_at = created_at - interval '10 minutes' WHERE id = $1" if DRIVER == "postgres" else
             ("UPDATE market_listings SET created_at = datetime(created_at, '-10 minutes') WHERE id = $1" if DRIVER == "sqlite" else
              "UPDATE market_listings SET created_at = created_at - INTERVAL 10 MINUTE WHERE id = $1"), listing)
        db.q("INSERT INTO bans (account_id, reason) VALUES ($1, 'test')", C)
        r = await b.call(("br", "po"), "pi", listing, 3, op())
        check(r[0] == "br" and r[1] is None and count(db, B, 4) == 0, "a banned seller's listing does not sell: %r" % (r,))
        db.q("UPDATE bans SET revoked_at = created_at WHERE account_id = $1", C)
        bf, cf = funds(db, B), funds(db, C)
        r = await b.call(("br", "po"), "pi", listing, 3, op())
        check(r[0] == "br" and r[1]["f"] == bf - 1009 and r[1]["i"] == 4 and r[2] == 3, "bought: %r" % (r,))
        check(funds(db, C) == cf + 1009 and count(db, B, 4) == 1, "seller paid the full price, item delivered")
        for _ in range(40):  # written right after the "br" reply
            mails = db.q("SELECT subject, body FROM mail WHERE account_id = $1 AND subject = 'Item sold'", C)
            if mails:
                break
            await asyncio.sleep(0.05)
        check(len(mails) == 1 and "Moon Dragon" in mails[0][1], "seller gets the v7 'Item sold' mail: %r" % mails)
        r = await b.call(("br", "po"), "pi", listing, 3, op())
        check(r[0] == "br" and r[1] is None, "a sold listing is unavailable")
        # the cards' estimate badge (v10 getPrices): each item's mean completed sale price, a whole KR number
        page = await b.call("0", "r", "market", "prices", B, names["bob"], None, -1)
        rows = page[3] if page[1:3] == ["market", "prices"] and isinstance(page[3], list) else []
        est = next((x.get("est") for x in rows if x.get("ski") == 4), None)
        check(isinstance(est, int) and est == 1009 and all(x.get("ski") != 2 for x in rows),
              "prices: Moon Dragon's estimate is its one sale (1009 KR), unsold items have none: %r" % (rows,))
        r = await c.call(("lir", "po"), "li", 4, 4, 8, 500, op())
        l2 = r[1]["listingId"]
        r = await c.call(("ulir", "po"), "uli", 4, l2, 8, op())
        check(r[0] == "ulir" and r[1] == "SUCCESS" and r[2] == 8 and count(db, C, 4) == 2, "unlisted: the item is back")
        r = await c.call(("ulir", "po"), "uli", 4, l2, 8, op())
        check(r[0] == "ulir" and r[1] is None, "unlisting twice refused")
        r = await c.call("ul", "uml", C)
        check(r[1] == [], "profile listings tab (uml -> ul) empty after unlisting")
        # quick sell (v7.1.4 frame): rarity 3 = 100 KR each
        give(db, C, 10, 3)
        cf = funds(db, C)
        r = await c.call("qsr", "qsi", 0, 10, 55, 1, op())
        check(r[1] and r[1]["i"] == 55 and r[1]["c"] == 1 and r[1]["f"] == cf + 200 and r[1]["kr"] == 200,
              "quick sell all but one: %r" % (r,))
        r = await c.call("qsr", "qsi", 1, 10, 55, 0, op())
        check(r[1] is None, "junk mode refused for a Legendary: no scrap value (deob-10.0.3.js:116262-116339)")
        # quick sell for Junk (v10.0.3's Get Junk, action 1): a Contraband gives its scrap, 5.00 Junk each, into the
        # Junk wallet with one quicksell ledger row (doc/v10/junk.md); no KR moves
        give(db, C, 114, 2)  # Flame Tamer, contraband
        cf, cj = funds(db, C), junk(db, C)
        page = await c.call("0", "r", "market", "inventory", C, names["cat"], None, 0)
        row = next((x for x in (page[3] if isinstance(page[3], list) else []) if x.get("i") == 114), None)
        check(row is not None and row.get("qj") == 500 and row.get("qs") == 2500 and "nqv" not in row,
              "inventory row: 5.00 Junk (qj, hundredths) and 2500 KR per unit: %r" % (row,))
        r = await c.call("qsr", "qsi", 1, 114, 56, 2, op())
        check(r[1] and r[1]["i"] == 56 and r[1]["c"] == 0 and r[1]["kr"] == 0 and r[1]["f"] == cf and r[1]["jnk"] == 10
              and abs(r[1]["j"] - (cj + 1000) / 100.0) < 1e-9 and junk(db, C) == cj + 1000 and count(db, C, 114) == 0,
              "quick sell all for Junk: 10.00 Junk (j the balance, jnk the gain), no KR: %r" % (r,))
        check(db.one("SELECT amount FROM junk_transactions WHERE account_id = $1 AND source = 'quicksell'", C) == 1000
              and db.one("SELECT coalesce(sum(amount), 0) FROM junk_transactions WHERE account_id = $1", C) == junk(db, C),
              "one quicksell ledger row of 1000 hundredths; the ledger sums to the balance")
        # Black Market (v7.1.4 pbm -> bmr): blackM items for their store funds, if checkReq holds
        cf = funds(db, C)
        r = await c.call(("bmr", "po"), "pbm", 1642, op())  # Nuke Tamer: 3000 KR, stats n >= 1000
        check(po_text(r) == "Access Denied", "black market: requirement not met: %r" % (r,))
        db.q("UPDATE players SET player_stats = $2 WHERE player_id = $1", C, '{"n": 1200}')
        r = await c.call(("bmr", "po"), "pbm", 1642, op())
        check(r[0] == "bmr" and r[1] and r[1]["item"] == 1642 and r[1]["f"] == cf - 3000 and count(db, C, 1642) == 1,
              "black market purchase: %r" % (r,))
        r = await c.call(("bmr", "po"), "pbm", 1642, op())
        check(po_text(r) == "You already own this item", "black market: once per account")
        r = await c.call(("qsr", "po"), "qsi", 0, 1642, 9, 0, op())
        check(r[0] == "qsr" and r[1] is None and count(db, C, 1642) == 1, "black market items never quick-sell for KR")
        # Nuke Tamer is noSale && blackM: v10.0.3 neither sells nor destroys it ("This item cannot be sold back for
        # currency.", deob-10.0.3.js:122609-122617, 147614)
        cj = junk(db, C)
        r1 = await c.call(("qsr", "po"), "qsi", 1, 1642, 9, 0, op())
        r2 = await c.call(("qsr", "po"), "qsi", 2, 1642, 9, 0, op())
        check(r1[1] is None and r2[1] is None and count(db, C, 1642) == 1 and junk(db, C) == cj,
              "a noSale black market item: no Junk, no destroy: %r %r" % (r1, r2))
        r = await c.call(("bmr", "po"), "pbm", 1483, op())  # 1 Year Cape: account age
        check(po_text(r) == "Access Denied", "black market: account too young: %r" % (r,))
        r = await c.call(("bmr", "po"), "pbm", 1641, op())  # req disabled
        check(r[0] == "bmr" and r[1] is None, "black market: disabled item unavailable")
        # v10.0.3's title reward (deob-10.0.3.js:152763-152806): an item with a `title` grants it, and the reply names it
        r = await c.call("ttl", "ttl")
        check(r[1] == [], "ttl: no titles before: %r" % (r,))
        r = await c.call("gapi", "gapi", 1, "GET", "/players/" + names["cat"], None)
        check(r[2] == 200 and r[3]["data"]["player_titles"] == "", "gapi profile: no titles before: %r" % (r[3],))
        set_player(db, C, funds=funds(db, C) + 10000)
        cf = funds(db, C)
        db.q("UPDATE players SET player_stats = $2 WHERE player_id = $1", C, '{"n": 1200, "tmk": 10000}')
        r = await c.call(("bmr", "po"), "pbm", 1866, op())  # Shuriken: 10000 KR, stats tmk >= 10000, title 91
        check(r[0] == "bmr" and r[1] and r[1]["item"] == 1866 and r[1].get("title") == 91 and r[1]["f"] == cf - 10000
              and count(db, C, 1866) == 1, "black market purchase with a title: %r" % (r,))
        check(db.q("SELECT title_id, source, ref FROM player_titles WHERE account_id = $1", C) == [("91", "blackmarket", "1866")],
              "player_titles row (0361): title 91 from item 1866")
        r = await c.call("ttl", "ttl")
        check(r[1] == [91], "ttl lists the granted title: %r" % (r,))
        r = await c.call("gapi", "gapi", 2, "GET", "/players/" + names["cat"], None)
        check(r[2] == 200 and r[3]["data"]["player_titles"] == "91", "gapi profile player_titles: %r" % (r[3]["data"].get("player_titles"),))
        r = await c.call(("bmr", "po"), "pbm", 1866, op())
        check(po_text(r) == "You already own this item" and db.one("SELECT count(*) FROM player_titles WHERE account_id = $1", C) == 1,
              "a title item twice: refused, the title granted once: %r" % (r,))
        r = await c.call(("bmr", "po"), "pbm", 1642, op())
        check(r[0] == "po" and db.one("SELECT count(*) FROM player_titles WHERE account_id = $1", C) == 1,
              "an item without a title grants none")
        r = await b.call("gapi", "gapi", 3, "GET", "/players/" + names["bob"], None)
        check(r[3]["data"]["player_titles"] == "", "another account's profile: no titles: %r" % (r[3]["data"].get("player_titles"),))
        await blackm_requirements(url, db, c, names, C, D)
        # MAX
        give(db, D, 0, 3)
        d = await user(url, names["dan"])
        try:
            r = await d.call(("lir", "po"), "li", 0, 0, 1, 10, op())
            r = await d.call(("lir", "po"), "li", 0, 0, 1, 10, op())
            r = await d.call(("lir", "po"), "li", 0, 0, 1, 10, op())
            check(r[0] == "lir" and r[1] == {"response": "MAX"} and count(db, D, 0) == 1, "max_listings (2 in the test) -> MAX: %r" % (r,))
            lid = db.one("SELECT max(id) FROM market_listings WHERE seller_id = $1 AND sold_at IS NULL AND cancelled_at IS NULL", D)
            r = await d.call("0", "r", "listing", "market", D, names["dan"], None, 0, lid)
            check(r[1] == "listing" and r[3] and r[3]["id"] == lid and r[3]["sk"] == 0 and r[3]["kr"] == 10 and r[3]["p"] == names["dan"],
                  "listing page (v7 ?p=listing&i=): %r" % (r[3],))
            r = await d.call("ul", "uml", D)
            check(len(r[1]) == 2 and all(x["sk"] == 0 and x["kr"] == 10 for x in r[1]), "profile Listings tab (uml -> ul)")
            r = await d.call("0", "r", "itemsales", "market", D, names["dan"], None, 0, 0)
            check(r[3]["onS"] == 2 and r[3]["inC"] == 3 and r[3]["low"] == 10, "itemsales: on sale / in circulation / low: %r" % (r[3],))
            # rate limit (economy_rate_qsell = 3 in the test)
            give(db, D, 3, 10)
            res = []
            for _ in range(4):
                res.append(await d.call(("qsr", "po"), "qsi", 0, 3, 1, 0, op()))
            check([x[0] for x in res] == ["qsr"] * 4 and res[3][1] is None and count(db, D, 3) == 7,
                  "per-account rate limit: the 4th quick sell in a minute is refused")
        finally:
            await d.__aexit__()

        # ---- trades
        give(db, A, 2, 2)   # epic: fee 5
        give(db, B, 4, 0)
        set_player(db, B, level=9)
        r = await a.call(("trR", "po"), "ctr", B, [{"i": 2, "c": 1}], [{"i": 4, "c": 1}], "deal?", op())
        check(po_text(r) == "Must be level 10", "trade needs level 10 on both sides (v10): %r" % (r,))
        set_player(db, B, level=40)
        r = await a.call(("trR", "po"), "ctr", B, [{"i": 2, "c": 6}], [], "", op())
        check(po_text(r) == "5 items per user", "5 items per side for non-premium: %r" % (r,))
        nosale_t = nosale
        give(db, A, nosale_t, 1)
        r = await a.call(("trR", "po"), "ctr", B, [{"i": nosale_t, "c": 1}], [], "", op())
        check(po_text(r) == "Item cant be traded", "non-tradable item refused in offers")
        af = funds(db, A)
        r = await a.call(("trR", "po"), "ctr", B, [{"i": 2, "c": 1}], [{"i": 4, "c": 1}], "deal?", dict(op(), skr=300, rkr=0))
        fee = 5 + 10 + 30  # rarity fees of both sides + the gift fee on the 300 KR
        check(r[0] == "trR" and r[1]["fee"] == fee and r[1]["funds"] == af - 300 - fee and r[3] == "Offer Sent",
              "offer: escrow of items + KR, fee = rarity fees of both sides + gift fee on KR: %r" % (r,))
        trid = r[1]["trid"]
        check(count(db, A, 2) == 1, "offered item left the sender's inventory (escrow)")
        while (note := await b.recv("econN"))[1] != "trade":  # gift notes came first
            pass
        check(note[2] == "New Trade Offer", "receiver notified (econN push)")
        page = await b.call("0", "r", "market", "trades", B, names["bob"], None, -1)
        offer = next((o for o in page[3][0] if o["trid"] == trid), None)
        check(offer and offer["sksell"] == "2," and offer["skbuy"] == "4," and offer["krsell"] == 300 and offer["dte"] > 0,
              "trades tab row: %r" % offer)
        give(db, A, 1, 1)
        note = "m" * 500
        r = await a.call(("trR", "po"), "ctr", C, [{"i": 1, "c": 1}], [], note, op())
        check(r[0] == "trR" and db.q("SELECT message FROM trades WHERE id = $1", r[1]["trid"])[0][0] == note,
              "a 500-character offer message is kept (deob-10.0.7.js:111332)")
        await a.call(("trR", "po"), "trC", r[1]["trid"], op())
        r = await a.call(("trR", "po"), "ctr", C, [{"i": 1, "c": 1}], [], "n" * 600, op())
        check(r[0] == "trR" and db.q("SELECT message FROM trades WHERE id = $1", r[1]["trid"])[0][0] == "n" * 500,
              "an offer message past 500 characters is cut there")
        await a.call(("trR", "po"), "trC", r[1]["trid"], op())
        # the same item cannot go in a second offer or listing
        r = await a.call(("trR", "po"), "ctr", C, [{"i": 2, "c": 2}], [], "", op())
        check(po_text(r) == "Item not owned", "escrowed item cannot be offered twice: %r" % (r,))
        bf, af = funds(db, B), funds(db, A)
        r = await b.call(("trR", "po"), "trA", trid, op())
        check(r[0] == "trR" and r[2] == trid and r[3] == "Trade Accepted", "accepted: %r" % (r,))
        check(count(db, B, 2) == 1 and count(db, A, 4) == 1 and count(db, B, 4) == 0 and funds(db, B) == bf + 300 and funds(db, A) == af,
              "both sides moved (items + escrowed KR)")
        r = await b.call(("trR", "po"), "trA", trid, op())
        check(po_text(r) == "Trade no longer available", "accepting twice refused")
        # decline returns the escrow (fee kept)
        af = funds(db, A)
        r = await a.call(("trR", "po"), "ctr", C, [{"i": 2, "c": 1}], [], "", dict(op(), skr=50))
        t2 = r[1]["trid"]
        r = await c.call(("trR", "po"), "trC", t2, op())
        check(r[0] == "trR" and r[3] == "Offer Declined" and count(db, A, 2) == 1 and funds(db, A) == af - 10,
              "declined: items and KR back to the sender, fees kept: %r" % (r,))
        # sender removes
        r = await a.call(("trR", "po"), "ctr", C, [{"i": 2, "c": 1}], [], "", op())
        t3 = r[1]["trid"]
        r = await a.call(("trR", "po"), "trC", t3, op())
        check(r[3] == "Offer Removed" and count(db, A, 2) == 1, "removed by the sender")
        # decline all
        r1 = await a.call(("trR", "po"), "ctr", C, [{"i": 2, "c": 1}], [], "", op())
        give(db, B, 0, 1)
        r2 = await b.call(("trR", "po"), "ctr", C, [{"i": 0, "c": 1}], [], "", op())
        c.inbox.clear()
        await c.send("trCA", op())
        while (r := await c.recv("pRes"))[3] != "Declined All":
            pass
        check(r[3] == "Declined All" and count(db, A, 2) == 1 and count(db, B, 0) == 1, "decline all returns every escrow")
        # premium: no fee, 20 items
        set_player(db, A, premium=1)
        give(db, A, 0, 6)
        af = funds(db, A)
        r = await a.call(("trR", "po"), "ctr", C, [{"i": 0, "c": 6}], [], "", op())
        check(r[0] == "trR" and r[1]["fee"] == 0 and funds(db, A) == af, "premium sender: more than 5 items, no fee: %r" % (r,))
        await a.call(("trR", "po"), "trC", r[1]["trid"], op())
        set_player(db, A, premium=0)
        if DRIVER == "postgres":  # 0111 trades_escrow_guard: no closing an escrowed offer around its unwinding
            r = await a.call(("trR", "po"), "ctr", C, [{"i": 2, "c": 1}], [], "", op())
            tg = r[1]["trid"]
            try:
                db.q("UPDATE trades SET status = 'declined' WHERE id = $1", tg)
                guarded = False
            except RuntimeError as e:
                guarded = "Trade no longer available" in str(e)
            check(guarded and db.q("SELECT status FROM trades WHERE id = $1", tg)[0][0] == "open",
                  "a plain UPDATE (an old brain's trCA) cannot close an escrowed offer")
            await a.call(("trR", "po"), "trC", tg, op())
        # KR in offers follows the gift rules: minimum, gift level, and the receiver pays the fee on rkr
        r = await a.call(("trR", "po"), "ctr", C, [{"i": 2, "c": 1}], [], "", dict(op(), skr=9))
        check(po_text(r) == "10 KR minimum", "offer KR below the gift minimum")
        set_player(db, A, level=9)
        r = await a.call(("trR", "po"), "ctr", C, [{"i": 2, "c": 1}], [], "", dict(op(), skr=100))
        check(po_text(r) == "Must be level 10", "offering KR needs the gift level: %r" % (r,))
        set_player(db, A, level=40)
        r = await a.call(("trR", "po"), "ctr", C, [{"i": 2, "c": 1}], [], "", dict(op(), rkr=200))
        tk = r[1]["trid"]
        cf, af = funds(db, C), funds(db, A)
        r = await c.call(("trR", "po"), "trA", tk, op())
        check(r[0] == "trR" and funds(db, C) == cf - 200 - 20 and funds(db, A) == af + 200 and count(db, C, 2) >= 1,
              "the receiver pays the asked KR + the gift fee on it at accept: %r" % (r,))
        give(db, A, 2, 1)
        # receiver KR side: not enough KR at accept refuses everything
        r = await a.call(("trR", "po"), "ctr", C, [{"i": 2, "c": 1}], [], "", dict(op(), rkr=10 ** 6))
        t4 = r[1]["trid"]
        r = await c.call(("trR", "po"), "trA", t4, op())
        check(po_text(r) == "Not enough KR" and count(db, A, 2) == 0 and count(db, C, 2) == 1, "accept without the KR asked refused, nothing moved")
        await a.call(("trR", "po"), "trC", t4, op())
        # expiry (accounts.trade_ttl_s = 3, economy tick 300 ms in the test)
        r = await a.call(("trR", "po"), "ctr", C, [{"i": 2, "c": 1}], [], "", dict(op(), skr=10))
        t5 = r[1]["trid"]
        for _ in range(100):  # TTL 3 s, the economy tick every 300 ms
            await asyncio.sleep(0.1)
            st = db.q("SELECT status, expires_at FROM trades WHERE id = $1", t5)
            if st and st[0][0] != "open":
                break
        check(st and st[0][0] == "expired" and count(db, A, 2) == 1, "expired offer unwound by the timer: %r" % st)
        # a pre-0111 offer (no escrow) still settles from the inventories
        db.q("INSERT INTO trades (sender_id, receiver_id, message) VALUES ($1, $2, 'old')", A, C)
        old = db.one("SELECT max(id) FROM trades WHERE sender_id = $1 AND message = 'old'", A)
        db.q("INSERT INTO trade_items (trade_id, side, skin_id, count) VALUES ($1, 's', 2, 1)", old)
        r = await c.call(("trR", "po"), "trA", old, op())
        check(r[0] == "trR" and count(db, C, 2) == 2 and count(db, A, 2) == 0, "legacy offer accepted: %r" % (r,))
        # KR history
        r = await a.call("krh", "krh", 30)
        srcs = {x["source"] for x in r[1]}
        check({"gift-out", "market-fee", "trade-fee", "trade-escrow", "trade-return"} <= srcs and
              any(x["source"] == "gift-out" and x["cp"] == names["bob"] and x["fee"] == 11 for x in r[1]),
              "krh: own ledger with counterparty names and fees: %r" % sorted(srcs))
    finally:
        for s in (a, b, c):
            await s.__aexit__()

    bad = ledger_ok(db, ids.values())
    check(not bad, "ledger: per account, the sum of its rows equals its balance %r" % bad)
    neg = db.one("SELECT count(*) FROM players WHERE player_funds < 0")
    negi = db.one("SELECT count(*) FROM inventory WHERE count < 0")
    check(neg == 0 and negi == 0, "no negative balance or item count")
    return ids, names


async def concurrency(url, db, ids, names):
    """Parallel actions at the same balance / item from many sockets of the same accounts."""
    E, F, G = ids["eve"], ids["fay"], ids["gus"]
    for aid in (E, F, G):
        set_player(db, aid, funds=1000, level=40, premium=0)
    scope = list(ids.values())
    adm_sql = "SELECT coalesce(sum(amount), 0) FROM kr_transactions WHERE source = 'admin' AND account_id IN (%s)" % ",".join(map(str, scope))
    kr0, items0 = totals(db, scope)
    adm0, given0 = db.one(adm_sql), GIVEN[0]

    async def many(name, n):
        return [await user(url, name) for _ in range(n)]

    eves, fays, guss = await many(names["eve"], 8), await many(names["fay"], 4), await many(names["gus"], 4)
    try:
        # 1. 40 gifts of 100 (+10 fee) from one balance of 1000 over 8 sockets: exactly 9 succeed
        async def gifts(s):
            out = []
            for _ in range(5):
                out.append(await s.call(("gir", "po"), "kr", names["fay"], 100, "", op()))
            return out
        res = sum(await asyncio.gather(*(gifts(s) for s in eves)), [])
        ok = sum(1 for r in res if r[0] == "gir")
        check(ok == 9 and funds(db, E) == 1000 - 9 * 110 and all(po_text(r) == "Not enough KR" for r in res if r[0] != "gir"),
              "parallel gifts: %d succeeded (9 expected), balance %d" % (ok, funds(db, E)))
        # 2. the same op from 8 sockets at once: one movement
        set_player(db, E, funds=1000)
        same = op()
        res = await asyncio.gather(*(s.call(("gir", "po"), "kr", names["gus"], 100, "", same) for s in eves))
        check(all(r[0] == "gir" for r in res) and len({r[1]["funds"] for r in res}) == 1 and funds(db, E) == 890,
              "one op key fired from 8 sockets moves KR once: %r" % sorted({r[0] for r in res}))
        # 3. 8 buyers race for one listing
        give(db, G, 4, 1)
        r = await guss[0].call(("lir", "po"), "li", 4, 4, 1, 300, op())
        lid = r[1]["listingId"]
        set_listing_old(db, lid)
        buyers = eves[:4] + fays
        res = await asyncio.gather(*(s.call(("br", "po"), "pi", lid, i, op()) for i, s in enumerate(buyers)))
        wins = [r for r in res if r[0] == "br" and r[1]]
        check(len(wins) == 1 and count(db, E, 4) + count(db, F, 4) == 1, "8 buyers, one listing: exactly one purchase (%d)" % len(wins))
        # 4. one item offered to two people and listed at the same time: only one wins
        give(db, G, 5, 1)
        acts = [guss[0].call(("trR", "po"), "ctr", E, [{"i": 5, "c": 1}], [], "", op()),
                guss[1].call(("trR", "po"), "ctr", F, [{"i": 5, "c": 1}], [], "", op()),
                guss[2].call(("lir", "po"), "li", 5, 5, 2, 100, op())]
        res = await asyncio.gather(*acts)
        won = [r for r in res if r[0] in ("trR", "lir") and r[1]]
        check(len(won) == 1 and count(db, G, 5) == 0, "one item, two offers + a listing in parallel: one wins (%r)" % [r[0] for r in res])
        # 5. accept from 4 sockets and the sender's removal at once: exactly one outcome
        give(db, G, 3, 2)
        r = await guss[0].call(("trR", "po"), "ctr", E, [{"i": 3, "c": 2}], [], "", dict(op(), skr=50))
        tid = r[1]["trid"]
        acts = [s.call(("trR", "po"), "trA", tid, op()) for s in eves[:4]] + [guss[1].call(("trR", "po"), "trC", tid, op())]
        res = await asyncio.gather(*acts)
        done = [r for r in res if r[0] == "trR"]
        check(len(done) == 1 and count(db, E, 3) + count(db, G, 3) == 2, "accept x4 vs remove: one outcome (%r)" % [r[0] for r in res])
        # 6. opposite gifts between two accounts in parallel (lock order: no deadlock)
        set_player(db, E, funds=5000)
        set_player(db, F, funds=5000)
        acts = [s.call(("gir", "po"), "kr", names["fay"], 10, "", op()) for s in eves] + \
               [s.call(("gir", "po"), "kr", names["eve"], 10, "", op()) for s in fays]
        res = await asyncio.gather(*acts)
        check(all(r[0] == "gir" for r in res), "crossing gifts A->B and B->A all complete: %r" % sorted({str(po_text(r)) for r in res}))
    finally:
        for s in eves + fays + guss:
            await s.__aexit__()
    kr1, items1 = totals(db, scope)
    adm1 = db.one(adm_sql)
    check(kr1 - kr0 == adm1 - adm0, "KR conserved: held + escrowed + fees moved only by the admin top-ups (%d -> %d, admin %+d)"
          % (kr0, kr1, adm1 - adm0))
    check(items1 - items0 == GIVEN[0] - given0, "items conserved: held + listed + escrowed (%d -> %d, given %d)"
          % (items0, items1, GIVEN[0] - given0))
    bad = ledger_ok(db, scope)
    check(not bad, "ledger matches every balance after the races %r" % bad)
    check(db.one("SELECT count(*) FROM players WHERE player_funds < 0") == 0, "no negative balance after the races")


PAINT_RED = 7776       # a colour paint, #dc3c3c (v10_paint.c)
PAINTABLE = 7838       # Susanowo: paintTargets true (assets/skins.json)
NOT_PAINTABLE = 0      # Arctic Hunt: no paintTargets


async def paint_targets(url, db):
    """The social socket's "ip" (the native paint picker's apply; economy.c sh_item_paint): a paint applies only to an
    item with paintTargets, the items v10 offers the Paint button for (deob-10.0.7.js:135288, 147902). The refusal
    takes no paint unit and makes no copy."""
    name = "pnt%s" % STAMP
    aid = await register(url, name)
    set_player(db, aid, level=40)
    give(db, aid, NOT_PAINTABLE, 1)
    give(db, aid, PAINTABLE, 1)
    give(db, aid, PAINT_RED, 2)
    s = await user(url, name)
    try:
        r = await s.call(("ipr", "po"), "ip", NOT_PAINTABLE, NOT_PAINTABLE, PAINT_RED, "#dc3c3c", -1, 41)
        check(po_text(r) == "That item cannot be painted.", "ip: an item without paintTargets is refused: %r" % (r,))
        check(count(db, aid, PAINT_RED) == 2 and db.one("SELECT count(*) FROM skin_copies WHERE account_id = $1", aid) == 0,
              "ip: the refusal takes no paint unit and makes no copy")
        r = await s.call(("ipr", "po"), "ip", PAINTABLE, PAINTABLE, PAINT_RED, "#dc3c3c", -1, 42)
        check(r[0] == "ipr" and isinstance(r[1], dict) and r[1].get("m") == {"paint": "#dc3c3c"} and r[2] == 42,
              "ip: an item with paintTargets is painted (ipr, m, uid): %r" % (r,))
        check(count(db, aid, PAINT_RED) == 1, "ip: the paint takes one unit")
    finally:
        await s.__aexit__()


def give_copy(db, aid, skin, kills):
    """A painted copy (0352) of `skin` with a counter skin's kills on it (skin_stats, 0406); its copy id."""
    give(db, aid, skin, 1)
    db.q("INSERT INTO skin_copies (account_id, skin_id, paint_color, paint_shader) VALUES ($1, $2, '#dc3c3c', NULL)", aid, skin)
    copy = db.one("SELECT max(copy_id) FROM skin_copies WHERE account_id = $1 AND skin_id = $2", aid, skin)
    db.q("INSERT INTO skin_stats (account_id, skin_id, copy_id, stat, count) VALUES ($1, $2, $3, 'kills', $4)", aid, skin, copy, kills)
    return copy


def kills_of(db, skin):
    """[[account, copy, kills], ...] of every skin_stats kills row of `skin`."""
    return sorted([int(v) for v in r] for r in db.q("SELECT account_id, copy_id, count FROM skin_stats WHERE skin_id = $1"
                                                     " AND stat = 'kills'", skin))


def copy_of(db, aid, skin):
    return db.one("SELECT max(copy_id) FROM skin_copies WHERE account_id = $1 AND skin_id = $2", aid, skin)


async def copy_counts(url, db):
    """A counter skin's count goes with its copy, as v10 keeps it in the copy's m[stat] (takeSkinStats,
    deob-10.0.7.js:191932; the item view draws the stats of a listing's m, :106682, :106774): a listing escrows it
    (market_listings.stats, 0407), an unlist gives it back, a sale gives it to the buyer; an offer escrows the sender's
    copy's count (trade_copies.stats), an accept gives both sides' copies' counts across, a cancel gives it back. A stack
    unit's count stays with the seller's stack (a stack listing has no m)."""
    names = {k: "%s%s" % (k, STAMP) for k in ("cks", "ckb", "ckr")}
    ids = {k: await register(url, n) for k, n in names.items()}
    S, B, R = ids["cks"], ids["ckb"], ids["ckr"]
    for aid in (S, B, R):
        set_player(db, aid, funds=50000, level=40)
    c1 = give_copy(db, S, 4, 120)
    give(db, S, 4, 1)  # a stack unit next to the copy, with its own count
    db.q("INSERT INTO skin_stats (account_id, skin_id, copy_id, stat, count) VALUES ($1, 4, 0, 'kills', 9)", S)
    s, b, r = await user(url, names["cks"]), await user(url, names["ckb"]), await user(url, names["ckr"])
    try:
        # ---- the market: list, unlist, list again, buy
        res = await s.call(("lir", "po"), "li", 4, c1, 7, 1000, op())
        lid = res[1]["listingId"] if res[0] == "lir" and isinstance(res[1], dict) else 0
        check(lid and kills_of(db, 4) == [[S, 0, 9]], "a listed copy's 120 kills leave the seller with it: %r %r" % (res, kills_of(db, 4)))
        await s.call(("ulir", "po"), "uli", 4, lid, 7, op())
        c2 = copy_of(db, S, 4)
        check(kills_of(db, 4) == [[S, 0, 9], [S, c2, 120]], "an unlist gives the copy back with its kills: %r" % kills_of(db, 4))
        res = await s.call(("lir", "po"), "li", 4, c2, 7, 1000, op())
        lid = res[1]["listingId"] if res[0] == "lir" and isinstance(res[1], dict) else 0
        set_listing_old(db, lid)
        res = await b.call(("br", "po"), "pi", lid, 4, op())
        bc = copy_of(db, B, 4)
        check(res[0] == "br" and kills_of(db, 4) == [[S, 0, 9], [B, bc, 120]],
              "a sale gives the copy's kills to the buyer's copy; the seller's stack keeps its 9: %r %r" % (res, kills_of(db, 4)))
        check(isinstance(res[1], dict) and (res[1].get("m") or {}).get("kills") == 120, "the buy's m has the copy's kills: %r" % (res,))
        # a stack unit: its listing carries no count, the seller's stack keeps it
        res = await s.call(("lir", "po"), "li", 4, 4, 7, 1000, op())
        lid = res[1]["listingId"] if res[0] == "lir" and isinstance(res[1], dict) else 0
        set_listing_old(db, lid)
        res = await b.call(("br", "po"), "pi", lid, 4, op())
        check(res[0] == "br" and kills_of(db, 4) == [[S, 0, 9], [B, bc, 120]] and not (res[1] or {}).get("m"),
              "a stack unit sold: no count moves (%r, %r)" % (res, kills_of(db, 4)))

        # ---- an offer: the sender's copy escrowed, the receiver's copy read at accept
        sc = give_copy(db, S, 2, 30)
        rc = give_copy(db, R, 3, 5)
        res = await s.call(("trR", "po"), "ctr", R, [{"i": 2, "c": 1, "iids": [sc]}], [{"i": 3, "c": 1, "iids": [rc]}], "", op())
        trid = res[1]["trid"] if res[0] == "trR" and isinstance(res[1], dict) else 0
        check(trid and kills_of(db, 2) == [], "an offered copy's 30 kills leave the sender with it: %r %r" % (res, kills_of(db, 2)))
        res = await r.call(("trR", "po"), "trA", trid, op())
        check(res[0] == "trR" and kills_of(db, 2) == [[R, copy_of(db, R, 2), 30]] and kills_of(db, 3) == [[S, copy_of(db, S, 3), 5]],
              "an accept moves both copies' kills across: %r %r %r" % (res, kills_of(db, 2), kills_of(db, 3)))
        # a cancelled offer gives the copy back with its kills
        rc2 = copy_of(db, R, 2)
        res = await r.call(("trR", "po"), "ctr", S, [{"i": 2, "c": 1, "iids": [rc2]}], [], "", op())
        trid = res[1]["trid"] if res[0] == "trR" and isinstance(res[1], dict) else 0
        res = await r.call(("trR", "po"), "trC", trid, op())
        check(res[0] == "trR" and kills_of(db, 2) == [[R, copy_of(db, R, 2), 30]] and copy_of(db, R, 2) != rc2,
              "a cancel gives the copy back with its kills: %r %r" % (res, kills_of(db, 2)))
    finally:
        for c in (s, b, r):
            await c.__aexit__()


def set_listing_old(db, lid):
    db.q("UPDATE market_listings SET created_at = created_at - interval '10 minutes' WHERE id = $1" if DRIVER == "postgres" else
         ("UPDATE market_listings SET created_at = datetime(created_at, '-10 minutes') WHERE id = $1" if DRIVER == "sqlite" else
          "UPDATE market_listings SET created_at = created_at - INTERVAL 10 MINUTE WHERE id = $1"), lid)


async def main_async(url, db):
    ids, names = await scenario(url, db)
    await concurrency(url, db, ids, names)
    await paint_targets(url, db)
    await copy_counts(url, db)


def main():
    scratch = [sys.executable, "-I", os.path.join(ROOT, "services", "brain", "tests", "db_scratch.py"), ROOT]
    r = subprocess.run(scratch + ["create", DRIVER, NAME], capture_output=True)
    if r.returncode:
        print("%s unavailable (%s): skip" % (DRIVER, r.stderr.decode(errors="replace").strip()))
        return r.returncode
    dbcfg = json.loads(r.stdout)
    tmp = tempfile.mkdtemp(prefix="kn-econ-test-", dir=os.path.expanduser("~/.cache"))
    cfg = {
        "database": dict(dbcfg, pool_size=8, migrations_dir=os.path.join(ROOT, "services", "db", "migrations")),
        "brain": {"host": "127.0.0.1", "port": 0, "internal_secret": "econ-%d" % os.getpid(), "federation": False},
        "accounts": {"signup_rewards": {"enabled": False},  # v10 signup perks off: these count inventories
                    "password_iterations": 1000, "login_throttle_s": 0, "starting_kr": 0, "free_premium": False,
                     "skins_file": os.path.join(ROOT, "assets", "skins.json"), "max_listings": 2, "trade_ttl_s": 3,
                     "economy_tick_ms": 300, "economy_rate_qsell": 3, "economy_rate_gift": 1000, "economy_rate_buy": 1000,
                     "economy_rate_trade": 1000, "economy_rate_accept": 1000, "economy_rate_close": 1000,
                     "economy_rate_list": 1000},
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
        asyncio.run(asyncio.wait_for(main_async("ws://127.0.0.1:%d/social/ws" % port, Db(dbcfg)), 600))
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
            print("brain log errors:\n" + "\n".join(errs[:20]))
        if rc:
            print("---- brain log tail (%s) ----\n%s" % (log_path, text[-4000:]))
        subprocess.run(scratch + ["drop", DRIVER, NAME], capture_output=True)
        if not rc:
            for f in os.listdir(tmp):
                os.unlink(os.path.join(tmp, f))
            os.rmdir(tmp)
    print("economy test (%s): %s" % (DRIVER, "OK" if not rc else "%d failure(s)" % len(FAILS)))
    return rc


if __name__ == "__main__":
    sys.exit(main())
