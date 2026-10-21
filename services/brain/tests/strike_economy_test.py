#!/usr/bin/env python3
"""Krunker Strike's economy on kn_brain (services/brain/strike_econ.c, db_strike.c, migration
0320_strike_economy; doc/v10/krunker-strike.md "Economy"), through the gapi tunnel of the social
socket (/strike/...) and a fake Strike game server on /mm/internal/ws:

  - the catalogue: migration 0320 seeded what tools/strike/gen_economy.py generates from
    strike_data.json (items, pools, wheels, products, missions, track); /item-definitions serves it
  - spins: costs debited (KR / tickets), items granted, odds within tolerance of the bundle's crate
    tiers over many rolls, the 10x Lucky Draw's guaranteed Epic, the golden spin meter, the free wheel
  - the store: tickets for KR, the daily free package once, today's deal, no real money
  - the loadout: equip owned items only, defaults clear a slot
  - progression: a Strike match commit (XP, krunkies, stats, missions, idempotent), the track's
    rewards (a choice slot, level gates, past the last level), daily missions (locked, claim once)
  - Strike and Box Office stay apart: Strike's KR / items never touch players / inventory and a Box
    Office commit never touches strike_progress; a Strike server's auth answers the Strike inventory
  - leaderboards: ordering, ties, ranks, the viewer's own rank

  strike_economy_test.py <kn_brain> <repo root> [database]
Database: argument, else $KN_MM_TEST_DATABASE, else config/config.json's database.name; every
run uses its own schema and an ephemeral port. 77 = skip (no PostgreSQL / python websockets).
KN_TEST_DB_DRIVER=sqlite|mysql runs on that store driver (db_scratch.py).
"""
import asyncio
import json
import math
import os
import re
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import time
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
    with open(os.path.join(ROOT, "config", "config.json")) as fp:
        CONFIG_DB = json.load(fp).get("database", {})
except (OSError, ValueError):
    pass
if not DB:
    DB = CONFIG_DB.get("name", "")
if not DB and DRIVER == "postgres":
    print("strike economy test: no database configured; skipped")
    sys.exit(77)
SCHEMA = "kn_test_kse_%d" % os.getpid()
SECRET = "kn-strike-econ-test-%d" % os.getpid()
FAILS = []
DBCFG = {}
PACE = 0.06  # the social socket allows 20 frames/s (social_ws.c RATE_PER_S)


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


def qint(sql):
    r = query(sql)
    return int(float(r[0])) if r and r[0] not in ("", "None") else 0


class Social:
    def __init__(self, url):
        self.url = url + "?enc=json"
        self.inbox = []
        self.rid = 0
        self.last = 0.0

    async def __aenter__(self):
        self.ws = await websockets.connect(self.url, max_size=16 << 20)
        return self

    async def __aexit__(self, *a):
        await self.ws.close()

    async def call(self, label, *frame):
        if frame:
            await self.ws.send(json.dumps(list(frame)))
        deadline = time.time() + 15
        while True:
            for i, f in enumerate(self.inbox):
                if f[0] == label:
                    return self.inbox.pop(i)
            self.inbox.append(json.loads(await asyncio.wait_for(self.ws.recv(), deadline - time.time())))

    async def gapi(self, method, path, body=None):
        """-> (status, data | error)"""
        wait = self.last + PACE - time.time()
        if wait > 0:
            await asyncio.sleep(wait)
        self.last = time.time()
        self.rid += 1
        rid = self.rid
        await self.ws.send(json.dumps(["gapi", rid, method, path, body]))
        deadline = time.time() + 15
        while True:
            for i, f in enumerate(self.inbox):
                if f[0] == "gapi" and f[1] == rid:
                    self.inbox.pop(i)
                    payload = f[3] or {}
                    return f[2], payload.get("data") if f[2] == 200 else payload.get("error")
            self.inbox.append(json.loads(await asyncio.wait_for(self.ws.recv(), deadline - time.time())))


class Node:
    """A Krunker Strike game server on /mm/internal/ws."""

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

    async def send(self, frame, reply):
        self.rid += 1
        frame = dict(frame, rid=self.rid)
        await self.ws.send(json.dumps(frame))
        while True:
            msg = json.loads(await asyncio.wait_for(self.ws.recv(), 15))
            if msg.get("type") == reply and msg.get("rid") == self.rid:
                return msg

    async def commit(self, match, player, flavor="strike"):
        f = {"type": "commit", "lobbyId": "se1", "matchId": match, "final": True, "map": "Burg", "mode": 0, "custom": False,
             "players": [player]}
        if flavor:
            f["flavor"] = flavor
        return await self.send(f, "commit-reply")

    async def auth(self, token, flavor="strike"):
        f = {"type": "auth", "token": token}
        if flavor:
            f["flavor"] = flavor
        return await self.send(f, "auth-reply")


def generated():
    r = subprocess.run([sys.executable, "-I", os.path.join(ROOT, "tools", "strike", "gen_economy.py"), "--json"], capture_output=True)
    return json.loads(r.stdout)


async def register(sock, name):
    r = await sock.call("a", "a", 0, [name, "strikepass1", 0], "captcha")
    assert r[1] is None, r
    return r[2], r[5]


def wallet(aid, cur):
    return qint("SELECT amount FROM strike_wallets WHERE account_id = %d AND currency = '%s'" % (aid, cur))


def set_wallet(aid, cur, amount):
    query("DELETE FROM strike_wallets WHERE account_id = %d AND currency = '%s'" % (aid, cur))
    query("INSERT INTO strike_wallets (account_id, currency, amount) VALUES (%d, '%s', %d)" % (aid, cur, amount))


def set_progress(aid, **cols):
    """strike_progress columns of an account (the row is created when it has none yet)"""
    if not qint("SELECT count(*) FROM strike_progress WHERE account_id = %d" % aid):
        query("INSERT INTO strike_progress (account_id) VALUES (%d)" % aid)
    query("UPDATE strike_progress SET %s WHERE account_id = %d" % (", ".join("%s = %d" % kv for kv in cols.items()), aid))


def items_total(aid):
    return qint("SELECT coalesce(sum(count), 0) FROM strike_items WHERE account_id = %d AND item_id NOT LIKE 'feature:%%'" % aid)


def box_office(aid):
    """Box Office's own state of an account: (KR, inventory rows, kr_transactions rows, score)"""
    return (qint("SELECT player_funds FROM players WHERE player_id = %d" % aid),
            qint("SELECT count(*) FROM inventory WHERE account_id = %d" % aid),
            qint("SELECT count(*) FROM kr_transactions WHERE account_id = %d" % aid),
            qint("SELECT player_score FROM players WHERE player_id = %d" % aid))


def within(got, n, p, what, sigmas=4.5):
    if n <= 0:
        check(False, "%s: no rolls" % what)
        return
    sd = math.sqrt(p * (1 - p) / n) if 0 < p < 1 else 0.0
    ok = abs(got / n - p) <= sigmas * sd + 1e-9
    check(ok, "%s: %.4f vs %.4f (n=%d, +-%.4f)" % (what, got / n, p, n, sigmas * sd))


async def scenario(port):
    gen = generated()
    stamp = str(int(time.time()) % 100000)
    alice, bob = "ksea" + stamp, "kseb" + stamp
    social = "ws://127.0.0.1:%d/social/ws" % port

    print("== the catalogue: migration 0320 seeded the generated data")
    counts = {t: qint("SELECT count(*) FROM %s" % t) for t in ("strike_item_defs", "strike_pools", "strike_wheels", "strike_products",
                                                                 "strike_missions_def", "strike_track", "strike_pool_items")}
    check(counts["strike_item_defs"] == len(gen["items"]) and counts["strike_item_defs"] > 600, "item definitions: %s" % counts)
    check(counts["strike_pools"] == 7 and counts["strike_pool_items"] == sum(p["count"] for p in gen["pools"]), "pools")
    # the generated products plus the two KR packages migration 0345 adds (the store's KR purchase)
    check(counts["strike_wheels"] == len(gen["wheels"]) and counts["strike_products"] == len(gen["products"]) + 2
          and counts["strike_missions_def"] == len(gen["missions"]) and counts["strike_track"] == len(gen["track"]),
          "wheels / products / missions / track")
    async with Social(social) as g:
        for _ in range(40):  # the catalogue loads at start
            st, defs = await g.gapi("GET", "/strike/v1/item-definitions/all")
            if st == 200:
                break
            await asyncio.sleep(0.25)
        check(st == 200 and [d["_id"] for d in defs] == [i["id"] for i in gen["items"]], "item-definitions/all = the generated catalogue")
        arctic = next(d for d in defs if d["_id"] == "s0")
        check(arctic == {"_id": "s0", "type": 0, "weapon": 0, "name": "Arctic Hunt", "rarity": 1, "id": 0, "default": False,
                         "spinnable": True, "unlockOrigin": "spin_tickets", "data": {}}, "FRVR item definition shape: %s" % arctic)
        st, d = await g.gapi("GET", "/strike/v1/item-definitions/default/3")
        check(st == 200 and d["_id"] == "d3", "default melee")
        st, d = await g.gapi("POST", "/strike/v1/item-definitions/itemList", {"itemIDs": ["s0", "c4"]})
        check(st == 200 and [x["_id"] for x in d] == ["s0", "c4"] and d[1]["classId"] == 4, "itemList")
        st, wl = await g.gapi("GET", "/strike/v2/spin-wheels/list")
        want = {w["id"]: w for w in gen["wheels"]}
        check(st == 200 and {w["_id"] for w in wl} == set(want), "spin-wheels/list")
        for w in wl or []:
            gw = want[w["_id"]]
            check([(r["poolId"], r["weight"]) for r in w["rewards"]] == [(s["pool"], s["weight"]) for s in gw["slices"]]
                  and (w.get("price") or {}).get("amount", 0) == gw["amount"], "wheel %s: the bundle's %s odds, price %s"
                  % (w["_id"], gw["tier"], gw["amount"]))
        st, det = await g.gapi("POST", "/strike/v2/spin-wheels/details", {"ids": ["starter"]})
        check(st == 200 and len(det) == 1 and len(det[0]["rewards"][0]["items"]) == next(p["count"] for p in gen["pools"] if p["id"] == "uncommon"),
              "spin-wheels/details: the pools' items")
        st, err = await g.gapi("GET", "/strike/v1/players/me")
        check(st == 401, "a guest has no Strike account: %s" % st)
        st, cfg = await g.gapi("GET", "/strike/v1/game-config")
        check(st == 200 and cfg["spinWheels"]["spinsToGetGoldenSpin"] == 10 and cfg["xpCurve"] == {"base": 100, "step": 100, "divisor": 3.5}
              and len(cfg["accountProgression"]) == len(gen["track"]), "game-config")

    async with Social(social) as a, Social(social) as b, Node(port) as node:
        alice_id, alice_token = await register(a, alice)
        bob_id, bob_token = await register(b, bob)
        bo0 = box_office(alice_id)

        print("== a new Strike account")
        st, me = await a.gapi("GET", "/strike/v1/players/me")
        check(st == 200 and me["level"]["level"] == 1 and me["wallet"]["kr"] == 0 and me["xp"] == 0, "level 1, no krunkies")
        check([c["classId"] for c in me["classes"]] == [0], "owns Triggerman only: %s" % me["classes"])

        print("== spins: costs, grants, refusals")
        st, err = await a.gapi("POST", "/strike/v2/spin-wheels/starter/spin", {"count": 1})
        check(st == 400 and err["message"] == "Not enough KR" and items_total(alice_id) == 0, "no KR: refused, nothing granted (%s)" % err)
        set_wallet(alice_id, "kr", 200000)
        st, res = await a.gapi("POST", "/strike/v2/spin-wheels/starter/spin", {"count": 1})
        rw = res["rewards"][0] if st == 200 else {}
        check(st == 200 and wallet(alice_id, "kr") == 200000 - 50 and res["wallet"]["kr"] == 200000 - 50, "starter: 50 KR debited")
        item = rw.get("reward", {}).get("item", {})
        pool_rar = {p["id"]: i for i, p in enumerate(gen["pools"])}
        check(item.get("itemId") and item.get("rarity") == pool_rar[rw["poolId"]] and items_total(alice_id) == 1
              and qint("SELECT count FROM strike_items WHERE account_id = %d AND item_id = '%s'" % (alice_id, item["itemId"])) == 1,
              "the won item is granted, from its rarity's pool: %s %s" % (rw.get("poolId"), item.get("itemId")))
        check(qint("SELECT count(*) FROM strike_ledger WHERE account_id = %d AND kind = 'spin' AND delta = -50" % alice_id) == 1, "ledger: the debit")

        print("== odds: the Elite wheel (crate tier Elite 50/30/15/5) over many rolls")
        elite = want["elite"]
        hits = {s["pool"]: 0 for s in elite["slices"]}
        rolls, kr0, it0 = 0, wallet(alice_id, "kr"), items_total(alice_id)
        for _ in range(160):
            st, res = await a.gapi("POST", "/strike/v2/spin-wheels/elite/spin", {"count": 10})
            if st != 200:
                check(False, "elite 10x spin: %s %s" % (st, res))
                break
            for r in res["rewards"]:
                hits[r["poolId"]] += 1
                rolls += 1
        for s in elite["slices"]:
            within(hits[s["pool"]], rolls, s["weight"], "elite %s" % s["pool"])
        check(wallet(alice_id, "kr") == kr0 - 100 * rolls, "elite: 100 KR per roll debited (%d rolls)" % rolls)
        check(items_total(alice_id) == it0 + rolls, "every roll granted one item")

        print("== the golden spin meter (10 paid spins = 1 golden ticket)")
        st, me = await a.gapi("GET", "/strike/v1/players/me")
        paid = 1 + rolls
        check(me["wallet"]["spin_golden_ticket"] == paid // 10 and me["goldenSpinCount"] == paid % 10,
              "golden tickets %s, meter %s after %d paid spins" % (me["wallet"]["spin_golden_ticket"], me["goldenSpinCount"], paid))

        print("== golden spins (crate tier Heroic) with golden tickets")
        set_wallet(alice_id, "spin_golden_ticket", 1000)
        heroic = want["golden_spin"]
        ghits = {s["pool"]: 0 for s in heroic["slices"]}
        grolls = 0
        for _ in range(60):
            st, res = await a.gapi("POST", "/strike/v2/spin-wheels/golden_spin/spin", {"count": 10})
            if st != 200:
                check(False, "golden 10x: %s %s" % (st, res))
                break
            for r in res["rewards"]:
                ghits[r["poolId"]] += 1
                grolls += 1
        check(wallet(alice_id, "spin_golden_ticket") == 1000 - grolls, "a golden ticket per golden roll")
        for s in heroic["slices"]:
            if s["weight"] >= 0.1:
                within(ghits[s["pool"]], grolls, s["weight"], "golden %s" % s["pool"])
        check(ghits.get("uncommon", 0) == 0, "the Heroic tier never gives Uncommon")

        print("== Lucky Draw: tickets, 10x guarantees an Epic or better")
        set_wallet(alice_id, "spin_ticket", 300)
        guaranteed = True
        for _ in range(30):
            st, res = await a.gapi("POST", "/strike/v2/spin-wheels/lucky_draw/spin", {"count": 10})
            if st != 200:
                guaranteed = False
                break
            guaranteed &= any(pool_rar[r["poolId"]] >= 2 for r in res["rewards"])
        check(guaranteed and wallet(alice_id, "spin_ticket") == 0, "30 x 10x Lucky Draw: each held an Epic+, 300 tickets spent")
        st, err = await a.gapi("POST", "/strike/v2/spin-wheels/lucky_draw/spin", {"count": 1})
        check(st == 400 and err["message"] == "Not enough tickets", "no tickets: refused")

        print("== the free wheel: one spin per period")
        st, res = await a.gapi("POST", "/strike/v2/spin-wheels/free/spin", {"count": 1})
        check(st == 200 and len(res["rewards"]) == 1, "the free spin")
        st, err = await a.gapi("POST", "/strike/v2/spin-wheels/free/spin", {"count": 1})
        check(st == 400 and "No free spin" in err["message"], "a second free spin waits: %s" % err)
        st, wl = await a.gapi("GET", "/strike/v2/spin-wheels/list")
        check(any(w["_id"] == "free" and w.get("lastSpinTime") for w in wl), "list: the free wheel's lastSpinTime")

        print("== the store")
        st, shop = await a.gapi("GET", "/strike/v1/shop")
        pids = [p["_id"] for p in shop["products"]] if st == 200 else []
        check(st == 200 and "tickets_1" in pids and any(p.startswith("deal_") for p in pids) and shop["sections"][0]["id"] == "spins",
              "shop: products, today's deals, sections")
        # the KR packages (section currency) are the one real-money price: with no payment provider they are credited
        check(all(p["price"][0]["type"] in ("virtual_currency", "free") or p["_id"].startswith("kr_") for p in shop["products"]),
              "real-money prices only on the KR packages")
        krp = [p for p in shop["products"] if p["_id"].startswith("kr_")]
        check(len(krp) >= 1 and all(p["category"] == "currency" and p["price"][0]["type"] == "real_money"
                                    and p["price"][0]["currency"] == "usd" and p["price"][0]["amount"] > 0 for p in krp)
              and all(r["currency"] == "kr" for p in krp for r in p["products"]),
              "shop: the KR packages sit in the currency section, priced in USD, granting KR: %s" % [p["_id"] for p in krp])
        # front.sections (FRVR's shop v2 front): the golden spin's SpinWheel section; the golden wheel is not in the Spins list
        fsec = [s for s in shop.get("front", {}).get("sections", []) if s.get("id") == "lucky-draw"]
        fcont = fsec[0]["content"][0] if fsec else {}
        gw = [r["spinWheelId"] for r in fcont.get("spinWheels", [])]
        gp = [r["productRef"]["productId"] for r in fcont.get("products", []) if r.get("spinWheelType") == "golden_spin"]
        spins = [r["spinWheelId"] for s in shop["sections"] if s["id"] == "spins" for c in s["content"] for r in c["spinWheels"]]
        check(gw == ["golden_spin"] and gp == ["golden_ticket_1"] and "golden_spin" not in spins,
              f"front: golden spin section {gw} {gp}, not in the Spins list")
        kr = wallet(alice_id, "kr")
        st, res = await a.gapi("POST", "/strike/v1/shop/purchase", {"productId": "tickets_1", "priceId": "tickets_1_p0", "quantity": 2})
        check(st == 200 and wallet(alice_id, "kr") == kr - 160 and wallet(alice_id, "spin_ticket") == 2, "2 x 1 Spin Ticket for 80 KR")
        st, res = await a.gapi("POST", "/strike/v1/shop/purchase", {"productId": "free_daily"})
        check(st == 200 and res["rewards"][0]["currency"] == "kr" and res["rewards"][0]["amount"] == 10, "the daily free KR package: %s" % res)
        st, err = await a.gapi("POST", "/strike/v1/shop/purchase", {"productId": "free_daily"})
        check(st == 400 and err["message"] == "Already purchased", "once a day")
        # buy KR: no payment provider is configured, so the pack is credited (the dev path); the reply is the purchase's
        pack = krp[0] if krp else {"_id": "kr_missing", "products": [{"amount": 0, "currency": "kr"}]}
        pack_kr = pack["products"][0]["amount"]
        kr = wallet(alice_id, "kr")
        st, res = await a.gapi("POST", "/strike/v1/shop/purchase", {"productId": pack["_id"], "priceId": pack["_id"] + "_p0", "quantity": 1})
        check(st == 200 and wallet(alice_id, "kr") == kr + pack_kr and res["wallet"]["kr"] == kr + pack_kr
              and res["rewards"][0]["currency"] == "kr" and res["rewards"][0]["amount"] == pack_kr,
              "buy KR: %s credits %d KR (%d -> %d): %s" % (pack["_id"], pack_kr, kr, wallet(alice_id, "kr"), res))
        st, res = await a.gapi("POST", "/strike/v1/shop/purchase", {"productId": pack["_id"], "priceId": pack["_id"] + "_p0", "quantity": 2})
        check(st == 200 and wallet(alice_id, "kr") == kr + 3 * pack_kr, "buy KR x2 credits 2 packs")
        kr = wallet(alice_id, "kr")
        deal = next(p for p in shop["products"] if p["_id"].startswith("deal_"))
        deal_item = deal["products"][0]["item"]["itemId"]
        before = qint("SELECT coalesce(sum(count), 0) FROM strike_items WHERE account_id = %d AND item_id = '%s'" % (alice_id, deal_item))
        kr = wallet(alice_id, "kr")
        st, res = await a.gapi("POST", "/strike/v1/shop/purchase", {"productId": deal["_id"]})
        check(st == 200 and wallet(alice_id, "kr") == kr - deal["price"][0]["amount"]
              and qint("SELECT count FROM strike_items WHERE account_id = %d AND item_id = '%s'" % (alice_id, deal_item)) == before + 1,
              "today's deal: %s for %d KR" % (deal_item, deal["price"][0]["amount"]))
        st, err = await a.gapi("POST", "/strike/v1/shop/validate", {"validationBlob": {}})
        check(st == 400 and err["code"] == "PAYMENT_UNAVAILABLE", "real-money validation refused")
        st, err = await b.gapi("POST", "/strike/v1/shop/purchase", {"productId": "tickets_10"})
        check(st == 400 and err["message"] == "Not enough KR" and wallet(bob_id, "spin_ticket") == 0, "bob can't afford tickets")

        print("== the loadout")
        owned_skin = query("SELECT i.item_id FROM strike_items i JOIN strike_item_defs d ON d.id = i.item_id"
                           " WHERE i.account_id = %d AND d.item_type = 0 AND d.weapon = 1 ORDER BY i.item_id LIMIT 1" % alice_id)
        if not owned_skin:  # an AR skin for the test, through the DB
            query("INSERT INTO strike_items (account_id, item_id, count) VALUES (%d, 's10', 1)" % alice_id)
            owned_skin = ["s10"]
        st, res = await a.gapi("POST", "/strike/v1/players/me/equip", {"itemId": owned_skin[0]})
        check(st == 200 and res["loadout"] == {"w1": owned_skin[0]}, "equip an owned AR skin: %s" % res)
        unowned_q = ("SELECT d.id FROM strike_item_defs d WHERE d.item_type = 1 AND NOT EXISTS"
                     " (SELECT 1 FROM strike_items i WHERE i.account_id = %d AND i.item_id = d.id) ORDER BY d.id LIMIT 1" % alice_id)
        if not query(unowned_q):  # the random spins above can win every hat: take one back for the refusal
            hat = query("SELECT d.id FROM strike_item_defs d WHERE d.item_type = 1 ORDER BY d.id LIMIT 1")[0]
            query("DELETE FROM strike_items WHERE account_id = %d AND item_id = '%s'" % (alice_id, hat))
        unowned = query(unowned_q)[0]
        st, err = await a.gapi("POST", "/strike/v1/players/me/equip", {"itemId": unowned})
        check(st == 400 and err["message"] == "You don't own this item", "an unowned hat is refused")
        st, err = await a.gapi("POST", "/strike/v1/players/me/equip", {"itemId": "c4"})
        check(st == 400 and err["code"] == "NOT_EQUIPPABLE", "a class is not equipped")
        st, res = await a.gapi("POST", "/strike/v1/players/me/equip", {"itemId": "d0_3"})
        check(st == 200 and res["loadout"] == {"w1": owned_skin[0]}, "a default item clears its slot only")
        st, items = await a.gapi("GET", "/strike/v1/players/me/items")
        check(st == 200 and all(i["item"]["_id"] == i["itemId"] for i in items) and sum(i["count"] for i in items) == items_total(alice_id),
              "players/me/items")

        print("== Strike and Box Office stay apart")
        check(box_office(alice_id) == bo0, "Box Office KR / inventory / KR history / score unchanged: %s vs %s" % (box_office(alice_id), bo0))
        r = await node.auth(alice_token, "strike")
        acc = r.get("account") or {}
        strike_skins = sorted(int(x) for x in query("SELECT d.skin_index FROM strike_items i JOIN strike_item_defs d ON d.id = i.item_id"
                                                     " WHERE i.account_id = %d AND i.count > 0 AND d.skin_index >= 0" % alice_id))
        got = sorted(e["ind"] for e in acc.get("skins", []))
        check(r.get("ok") and got == strike_skins and acc["data"][10] == acc["skins"], "Strike server auth: the Strike inventory (%d skins)" % len(got))
        check(acc.get("funds") == wallet(alice_id, "kr") and acc.get("strike", {}).get("loadout") == {"w1": owned_skin[0]},
              "... its krunkies and loadout")
        r = await node.auth(alice_token, None)
        acc = r.get("account") or {}
        check(r.get("ok") and acc.get("funds") == bo0[0] and len(acc["data"][10]) == bo0[1], "Box Office auth: Box Office's inventory")

        print("== a Strike match: XP, krunkies, stats, missions")
        kr = wallet(alice_id, "kr")
        player = {"accountId": alice_id, "name": alice, "kills": 22, "deaths": 5, "score": 3200, "won": True, "games": 1,
                  "timePlayed": 240000, "kr": 40, "xp": 3200, "prog": [["hs", 6, 1, -1, -1], ["kstk", 3, -1, -1, -1], ["kstk", 6, -1, -1, -1]]}
        r = await node.commit("m1", player)
        res = (r.get("results") or [{}])[0]
        check(r.get("ok") and res.get("xp") == 3200 and res.get("levelUp") is True and res.get("level") > 1, "commit: XP and a level up: %s" % res)
        check(wallet(alice_id, "kr") == kr + 40, "the match's krunkies")
        check(qint("SELECT kills FROM strike_progress WHERE account_id = %d" % alice_id) == 22
              and qint("SELECT wins FROM strike_progress WHERE account_id = %d" % alice_id) == 1, "Strike stats")
        r = await node.commit("m1", player)
        check((r.get("results") or [{}])[0].get("duplicate") is True and wallet(alice_id, "kr") == kr + 40
              and qint("SELECT xp FROM strike_progress WHERE account_id = %d" % alice_id) == 3200, "the same match again: a duplicate")
        check(box_office(alice_id) == bo0, "Box Office's players / inventory unchanged by the Strike match")
        bo_commit = await node.commit("bo1", dict(player, xp=500, score=500, kr=5), flavor=None)
        check(bo_commit.get("ok") and qint("SELECT xp FROM strike_progress WHERE account_id = %d" % alice_id) == 3200
              and box_office(alice_id)[3] > bo0[3], "a Box Office commit: Box Office's score, never Strike's XP")
        bo1 = box_office(alice_id)  # Box Office after its own match

        print("== daily missions")
        st, ms = await a.gapi("GET", "/strike/v1/missions/me")
        dm = ms["dailyMissions"]["missions"] if st == 200 else []
        check(st == 200 and len(dm) == 3 and not ms["locked"] and ms["unlockLevel"] == 3, "3 daily missions, unlocked at level %s"
              % (ms.get("unlockLevel") if st == 200 else "?"))
        expect = {"k": 22, "hs": 6, "w": 1, "gp": 1, "score": 3200, "kstk": 6, "ast": 0, "mk": 0}
        for m in dm:
            key, req = m["details"]["key"], m["details"]["requiredValue"]
            check(m["progress"] == expect[key] and m["completed"] == (expect[key] >= req), "mission %s: progress %d / %d" % (m["details"]["_id"], m["progress"], req))
        done = [m for m in dm if m["completed"]]
        pending = [m for m in dm if not m["completed"]]
        if done:
            m = done[0]
            w0 = dict((await a.gapi("GET", "/strike/v1/players/me"))[1]["wallet"])
            st, res = await a.gapi("POST", "/strike/v1/missions/claim/%s" % m["details"]["_id"])
            rw = m["details"]["rewards"][0]
            check(st == 200 and wallet(alice_id, rw["currency"]) == w0[rw["currency"]] + rw["amount"], "claim %s: +%d %s" % (m["details"]["_id"], rw["amount"], rw["currency"]))
            st, err = await a.gapi("POST", "/strike/v1/missions/claim/%s" % m["details"]["_id"])
            check(st == 400 and err["message"] == "Mission already claimed", "claimed once")
        if pending:
            st, err = await a.gapi("POST", "/strike/v1/missions/claim/%s" % pending[0]["details"]["_id"])
            check(st == 400 and err["message"] == "Mission not completed", "an unfinished mission can't be claimed")
        st, ms = await b.gapi("GET", "/strike/v1/missions/me")
        check(st == 200 and ms["locked"] is True, "bob (level 1): missions locked")

        print("== the progression track")
        st, res = await a.gapi("POST", "/strike/v1/players/me/claim/progression", {"slotId": "1_00"})
        check(st == 400 and res["message"] == "Pick one", "a choice slot needs a choice")
        lvl1 = next(t for t in gen["track"] if t["level"] == 1)
        st, res = await a.gapi("POST", "/strike/v1/players/me/claim/progression", {"slotId": "1_00", "choice": 2})
        pick = lvl1["rewards"][2]["item"]
        check(st == 200 and res["rewards"][0]["item"]["itemId"] == pick and "1_00" in res["progressionClaimedItems"]
              and qint("SELECT count FROM strike_items WHERE account_id = %d AND item_id = '%s'" % (alice_id, pick)) >= 1,
              "level 1: the third choice %s" % pick)
        st, err = await a.gapi("POST", "/strike/v1/players/me/claim/progression", {"slotId": "1_00", "choice": 0})
        check(st == 400 and err["message"] == "Reward already claimed", "claimed once")
        st, me = await a.gapi("GET", "/strike/v1/players/me")
        lvl = me["level"]["level"]
        t2 = await a.gapi("POST", "/strike/v1/players/me/claim/progression", {"slotId": "2_00"})
        check(t2[0] == 200 and t2[1]["rewards"][0]["currency"] == "spin_ticket", "level 2: a spin ticket (level %d)" % lvl)
        st, err = await a.gapi("POST", "/strike/v1/players/me/claim/progression", {"slotId": "%d_00" % (lvl + 1)})
        check(st == 400 and err["message"] == "Level not reached", "level %d not reached" % (lvl + 1))
        st, err = await b.gapi("POST", "/strike/v1/players/me/claim/progression", {"slotId": "2_00"})
        check(st == 400 and err["message"] == "Level not reached", "bob: level 2 not reached")
        # past the track's last level: a spin ticket per level
        check(qint("SELECT count(*) FROM strike_progress WHERE account_id = %d" % bob_id) == 0,
              "bob's refused purchase / claims left no Strike row (rolled back)")
        set_progress(bob_id, xp=1000000)
        st, res = await b.gapi("POST", "/strike/v1/players/me/claim/progression", {"slotId": "31_00"})
        check(st == 200 and res["rewards"] == [{"type": "currency", "currency": "spin_ticket", "amount": 1}], "past level 30: 1 spin ticket")
        # a configured catalogue entry: the level 4 track slot's reward is a Battle Pass ticket (strike_track row)
        query("UPDATE strike_track SET rewards = '[{\"type\": \"battlepass\"}]', player_choice = 0 WHERE slot_id = '4_00'")
        for _ in range(60):
            st, cfg = await b.gapi("GET", "/strike/v1/game-config")
            if st == 200 and cfg["accountProgression"]["4"]["items"] == [{"type": "battlepass"}]:
                break
            await asyncio.sleep(0.25)
        check(st == 200 and cfg["accountProgression"]["4"]["items"] == [{"type": "battlepass"}], "the configured track slot 4_00 is a battle pass reward")
        kr0, tk0 = wallet(bob_id, "kr"), wallet(bob_id, "spin_ticket")
        st, res = await b.gapi("POST", "/strike/v1/players/me/claim/progression", {"slotId": "4_00"})
        check(st == 200 and res["rewards"] == [{"type": "battlepass"}], "a battle pass reward is granted as the reward: %s" % res)
        check(wallet(bob_id, "kr") == kr0 and wallet(bob_id, "spin_ticket") == tk0, "the pass ticket moves no wallet")
        st, err = await b.gapi("POST", "/strike/v1/players/me/claim/progression", {"slotId": "4_00"})
        check(st == 400 and err["message"] == "Reward already claimed", "the battle pass reward is claimed once")
        st, res = await b.gapi("POST", "/strike/v1/players/me/claim/progression", {"slotId": "5_00"})
        check(st == 200 and res["rewards"][0]["type"] == "class", "level 5: a class (Run N Gun)")
        st, cl = await b.gapi("GET", "/strike/v1/players/me/classes")
        check(st == 200 and sorted(c["classId"] for c in cl) == [0, 2], "bob plays Triggerman and Run N Gun: %s" % cl)

        print("== leaderboards")
        set_progress(bob_id, xp=5000, kills=10)
        set_progress(alice_id, xp=9000)
        st, lb = await b.gapi("GET", "/strike/v1/leaderboards/xp?limit=50")
        names = [p["name"] for p in lb["players"]] if st == 200 else []
        check(st == 200 and names.index(alice) < names.index(bob) and [p["rank"] for p in lb["players"]] == list(range(1, len(names) + 1)),
              "xp: alice (9000) above bob (5000), ranks 1..n")
        check(lb["me"]["rank"] == names.index(bob) + 1 and lb["me"]["value"] == 5000, "bob's own rank: %s" % lb["me"])
        st, lb = await a.gapi("GET", "/strike/v1/leaderboards/kills")
        names = [p["name"] for p in lb["players"]]
        check(st == 200 and names.index(alice) < names.index(bob), "kills: alice (22) above bob (10)")
        set_progress(bob_id, kills=22)
        st, lb = await a.gapi("GET", "/strike/v1/leaderboards/kills")
        names = [p["name"] for p in lb["players"]]
        check(st == 200 and names.index(alice) < names.index(bob), "a tie: the older account first")
        st, err = await a.gapi("GET", "/strike/v1/leaderboards/funds")
        check(st == 400, "an unknown board is refused")
        with urllib.request.urlopen("http://127.0.0.1:%d/api/leaderboards/kills?flavor=strike" % port, timeout=10) as r:
            http_rows = json.loads(r.read())["data"]
        with urllib.request.urlopen("http://127.0.0.1:%d/api/strike/leaderboards/xp" % port, timeout=10) as r:
            http_xp = json.loads(r.read())["data"]
        names = [x["player_name"] for x in http_rows]
        check(names.index(alice) < names.index(bob) and http_rows[0]["position"] == 1 and http_rows[names.index(alice)]["value"] == 22,
              "HTTP /api/leaderboards/kills?flavor=strike: Strike's kills (v10's {position, player_name, value})")
        check([(x["player_name"], x["value"]) for x in http_xp] == [(alice, 9000), (bob, 5000)], "HTTP /api/strike/leaderboards/xp: %s" % http_xp)
        with urllib.request.urlopen("http://127.0.0.1:%d/api/leaderboards/kills" % port, timeout=10) as r:
            bo_rows = json.loads(r.read())["data"]
        check(bob not in [x["player_name"] for x in bo_rows], "Box Office's kills board: bob's Strike kills are not there")

        print("== Box Office is untouched by all of it")
        check(box_office(alice_id) == bo1 and box_office(bob_id)[1:3] == (0, 0),
              "Box Office KR / items / KR history / score of both accounts: %s %s" % (box_office(alice_id), box_office(bob_id)))
        check(qint("SELECT count(*) FROM strike_items WHERE item_id NOT LIKE 'feature:%' AND item_id NOT IN (SELECT id FROM strike_item_defs)") == 0,
              "every Strike item is a catalogue item")


def start_brain(env, log_path, extra):
    with open(log_path, "a") as log:
        log.write("== start %s\n" % extra)
        proc = subprocess.Popen([BRAIN, "--brain.host=127.0.0.1", "--brain.port=0", "--brain.id=kse-%d" % os.getpid(),
                                 "--accounts.signup_rewards.enabled=false", "--accounts.login_throttle_s=0"] + extra,
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
            print("strike economy test: %s unavailable: skipped" % DRIVER)
            sys.exit(r.returncode)
        DBCFG = json.loads(r.stdout)
        dbenv = {"KN_DATABASE_" + k.upper(): str(v) for k, v in DBCFG.items()}
        drop = scratch + ["drop", DRIVER, SCHEMA]
    work = tempfile.mkdtemp(prefix="kn-strike-econ.", dir=os.path.join(os.path.expanduser("~"), ".cache"))
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=SECRET, **dbenv)
    log_path = os.path.join(work, "brain.log")
    proc = None
    try:
        subprocess.run([BRAIN, "--migrate-only"], cwd=ROOT, env=env, check=True, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)
        proc, port = start_brain(env, log_path, ["--accounts.store_reload_ms=1500"])  # a configured row reloads in 1.5 s
        asyncio.run(scenario(port))
    finally:
        if proc:
            proc.terminate()
            proc.wait(5)
        subprocess.run(drop)
    with open(log_path) as f:
        bad = [l.strip() for l in f if re.search(r"failed:|ERROR|FATAL", l)]
    check(not bad, "no statement errors in the brain log %s" % bad[:3])
    print("strike economy test: " + ("OK" if not FAILS else "%d failure(s)" % len(FAILS)))
    if FAILS:
        print("log kept in " + work)
    else:
        shutil.rmtree(work, ignore_errors=True)
    sys.exit(1 if FAILS else 0)


main()
