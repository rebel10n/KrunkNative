#!/usr/bin/env python3
"""The Junk-priced shop on kn_brain (services/brain/db_shop.c, shop.c; migration 0349_shop_junk).

  shop_test.py <kn_brain> <repo root> [database]
Database: argument, else $KN_MM_TEST_DATABASE, else config/config.json's database.name; every
run uses its own schema and an ephemeral port. 77 = skip (no PostgreSQL / python websockets).
KN_TEST_DB_DRIVER=sqlite|mysql runs on that store driver (db_scratch.py).

Covers: a tag with no product is 404 "No display found with this tag"; the display lists the active
products only, in v10's shape (the Junk price as a "virtual" block of hundredths, the contents, the
purchase limit, the account's purchase_count); a purchase needs a token; a purchase the balance cannot
pay is refused "Not enough Junk" and writes nothing (no skin, no purchase row, no ledger row); a purchase
spends the price through junk_spend (reason "shop") and grants the contents (skin ledger rows); the
per-account purchase limit counts quantities across purchases; an inactive product, a bad quantity and
an unknown product are refused; the account's ledger rows sum to its balance.
"""
import asyncio
import concurrent.futures
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
    print("shop test: no database configured; skipped")
    sys.exit(77)
SCHEMA = "kn_test_shop_%d" % os.getpid()
SECRET = "kn-shop-test-%d" % os.getpid()
FAILS = []
JUNK_BASE = 100000  # db_shop.c SHOP_JUNK_BASE: a Junk product's display id is shop_products.id + this
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
    """Every row of `sql` on the test database, the first column as text (writes commit too)."""
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


def one(sql, default=0):
    rows = query(sql)
    return int(rows[0]) if rows else default


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
    """Game server side of /mm/internal/ws: the "acct" ops (give_skin, junk_credit)."""

    NODES = [0]

    def __init__(self, port):
        self.url = "ws://127.0.0.1:%d/mm/internal/ws" % port
        self.rid = 0
        Node.NODES[0] += 1
        self.node_id = "KSP%d" % Node.NODES[0]

    async def __aenter__(self):
        self.ws = await websockets.connect(self.url, additional_headers={"Authorization": "Bearer " + SECRET})
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
            msg = json.loads(await asyncio.wait_for(self.ws.recv(), 15))
            if msg.get("type") == "acct-r" and msg.get("rid") == rid:
                return msg.get("err"), msg.get("data")


async def register(sock, name):
    r = await sock.call("a", "a", 0, [name, "shoppass1", 0], "captcha")
    assert r[1] is None, r
    return r[2], r[5]


def http(port, method, path, body=None, token=None):
    """(status, json) of one request to the brain's HTTP port."""
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
        raw = e.read()
        try:
            return e.code, json.loads(raw or b"null")
        except ValueError:
            return e.code, raw.decode(errors="replace")


def err_of(body):
    return body.get("error") if isinstance(body, dict) else None


async def scenario(port):
    stamp = str(int(time.time()) % 100000)
    alice, bob = "shopa" + stamp, "shopb" + stamp
    social = "ws://127.0.0.1:%d/social/ws" % port
    async with Social(social) as sock_a, Social(social) as sock_b, Node(port) as node:
        a_id, a_tok = await register(sock_a, alice)
        b_id, b_tok = await register(sock_b, bob)
        check(a_id > 0 and b_id > 0, "two accounts registered")

        status, body = http(port, "GET", "/api/shop/displays?tag=nothing")
        check(status == 404 and err_of(body) == "No display found with this tag", "a tag with no product is 404: %s %s" % (status, body))

        query("INSERT INTO shop_products (display_tag, name, description, image_url, price_currency_id, price_amount, purchase_limit, active, sort_order)"
              " VALUES ('limited', 'Shop Test Crate', 'A test crate', '', 2, 250, 2, 1, 1)")
        query("INSERT INTO shop_products (display_tag, name, description, image_url, price_currency_id, price_amount, purchase_limit, active, sort_order)"
              " VALUES ('limited', 'Shop Test Pack', 'Two of one, one of another', '', 2, 1000, NULL, 1, 2)")
        query("INSERT INTO shop_products (display_tag, name, description, image_url, price_currency_id, price_amount, purchase_limit, active, sort_order)"
              " VALUES ('limited', 'Shop Test Retired', 'Not sold', '', 2, 100, NULL, 0, 3)")
        a = one("SELECT id FROM shop_products WHERE name = 'Shop Test Crate'")
        b = one("SELECT id FROM shop_products WHERE name = 'Shop Test Pack'")
        c = one("SELECT id FROM shop_products WHERE name = 'Shop Test Retired'")
        ja, jb, jc = a + JUNK_BASE, b + JUNK_BASE, c + JUNK_BASE  # the display and purchase ids
        query("INSERT INTO shop_product_contents (product_id, ord, content_type, skin_id, quantity) VALUES (%d, 0, 'skin', 7438, 1)" % a)
        query("INSERT INTO shop_product_contents (product_id, ord, content_type, skin_id, quantity) VALUES (%d, 0, 'skin', 7439, 2)" % b)
        query("INSERT INTO shop_product_contents (product_id, ord, content_type, skin_id, quantity) VALUES (%d, 1, 'skin', 7440, 1)" % b)
        query("INSERT INTO shop_product_contents (product_id, ord, content_type, skin_id, quantity) VALUES (%d, 0, 'skin', 7441, 1)" % c)

        status, body = http(port, "GET", "/api/shop/displays?tag=limited")
        data = body.get("data", {}) if isinstance(body, dict) else {}
        products = data.get("products", {})
        junk = {k: v for k, v in products.items() if int(k) >= JUNK_BASE}
        official = {k: v for k, v in products.items() if int(k) < JUNK_BASE}
        check(status == 200 and set(junk) == {str(ja), str(jb)} and len(official) == 17,
              "the display lists the two active Junk products (not the retired one) and the 17 official KR ones: %s %s"
              % (status, sorted(products)))
        pa = products.get(str(ja), {})
        pb = products.get(str(jb), {})
        price = (pa.get("prices") or [{}])[0]
        check(price.get("type") == "virtual" and price.get("virtual") == {"amount": 250, "currency_id": 2, "currency": "JNK"},
              "a Junk price is v10's virtual block in hundredths: %s" % price.get("virtual"))
        check(pa.get("contents") == [{"id": 1, "type": "skin", "ref": "7438", "quantity": 1}] and pa.get("purchase_limit") == 2
              and pa.get("purchase_count") == 0 and pa.get("purchase_limit_scope") == "per_user",
              "the product carries its contents, its per-user limit and a zero purchase count")
        check(pb.get("purchase_limit") is None and len(pb.get("contents", [])) == 2,
              "a product with no limit has purchase_limit null and both contents")
        junk_nodes = [ch for ch in data.get("children", []) if ch.get("product_id", 0) >= JUNK_BASE]
        children = [ch.get("product_id") for ch in junk_nodes]
        check(children == [ja, jb] and all(ch.get("type") == "product" for ch in junk_nodes),
              "the Junk products are the display's last children, in sort order: %s" % children)

        status, body = http(port, "POST", "/api/shop/purchase", {"product_id": ja, "quantity": 1})
        check(status == 401 and err_of(body) == "LoginRequired", "a purchase without a token is refused: %s %s" % (status, body))
        status, body = http(port, "POST", "/api/shop/purchase", {"product_id": ja, "quantity": 1}, a_tok)
        check(status == 400 and err_of(body) == "Not enough Junk", "a purchase with no Junk is refused: %s %s" % (status, body))
        check(one("SELECT COUNT(*) FROM shop_purchases WHERE account_id = %d" % a_id) == 0
              and one("SELECT COUNT(*) FROM skin_transactions WHERE account_id = %d AND skin_id = 7438" % a_id) == 0,
              "a refused purchase writes no purchase row and grants no skin")

        err, _ = await node.op("junk_credit", id=a_id, amount=1000, reason="trigger")
        assert err is None, err
        # v10.0.3's body: product_id is the price's price_ref string (deob-10.0.3.js:132049), "kn.shop.<display id>"
        status, body = http(port, "POST", "/api/shop/purchase", {"display_id": ja, "product_id": "kn.shop.%d" % ja, "quantity": 2}, a_tok)
        d = body.get("data", {}) if isinstance(body, dict) else {}
        check(status == 200 and d.get("success") is True and d.get("price_paid") == 500 and d.get("currency_id") == 2
              and d.get("junk") == "5.00" and d.get("rewards") == [{"type": "skin", "skin_id": 7438, "amount": 2}],
              "two crates cost 5.00 Junk and grant two skins: %s %s" % (status, body))
        check(one("SELECT player_junk FROM players WHERE player_id = %d" % a_id) == 500, "the balance is 5.00 (500 hundredths)")
        check(one("SELECT COUNT(*) FROM junk_transactions WHERE account_id = %d AND source = 'shop' AND amount = -500" % a_id) == 1,
              "one ledger row of -500 with reason 'shop'")
        check(one("SELECT COALESCE(SUM(delta), 0) FROM skin_transactions WHERE account_id = %d AND skin_id = 7438" % a_id) == 2,
              "two skins of 7438 are in the account (skin ledger)")

        status, body = http(port, "POST", "/api/shop/purchase", {"product_id": ja, "quantity": 1}, a_tok)
        check(status == 400 and err_of(body) == "Purchase limit reached", "the per-account limit of 2 refuses a third: %s" % body)
        status, body = http(port, "GET", "/api/shop/displays?tag=limited", token=a_tok)
        pa2 = body["data"]["products"][str(ja)] if status == 200 else {}
        check(pa2.get("purchase_count") == 2, "the display counts the account's two crates: %s" % pa2.get("purchase_count"))

        status, body = http(port, "POST", "/api/shop/purchase", {"product_id": jb, "quantity": 1}, a_tok)
        check(status == 400 and err_of(body) == "Not enough Junk", "a 10.00 pack with 5.00 Junk is refused: %s" % body)
        check(one("SELECT player_junk FROM players WHERE player_id = %d" % a_id) == 500
              and one("SELECT COUNT(*) FROM shop_purchases WHERE account_id = %d AND product_id = %d" % (a_id, b)) == 0,
              "the refused pack writes nothing")

        err, _ = await node.op("junk_credit", id=a_id, amount=600, reason="trigger")
        assert err is None, err
        status, body = http(port, "POST", "/api/shop/purchase", {"product_id": jb, "quantity": 1}, a_tok)
        d = body.get("data", {}) if isinstance(body, dict) else {}
        check(status == 200 and d.get("junk") == "1.00" and d.get("rewards") == [
            {"type": "skin", "skin_id": 7439, "amount": 2}, {"type": "skin", "skin_id": 7440, "amount": 1}],
              "the pack costs 10.00 and grants both of its contents: %s %s" % (status, body))

        status, body = http(port, "POST", "/api/shop/purchase", {"product_id": jc, "quantity": 1}, a_tok)
        check(status == 400 and err_of(body) == "Product not found", "a retired product is not sold: %s" % body)
        status, body = http(port, "POST", "/api/shop/purchase", {"product_id": 999999, "quantity": 1}, a_tok)
        check(status == 400 and err_of(body) == "Product not found", "an unknown product is refused: %s" % body)
        status, body = http(port, "POST", "/api/shop/purchase", {"product_id": "kn.shop.%d" % jc, "quantity": 1}, a_tok)
        check(status == 400 and err_of(body) == "Product not found", "a retired product by its price_ref is not sold: %s" % body)
        status, body = http(port, "POST", "/api/shop/purchase", {"product_id": "io.krunker.no-such-ref", "quantity": 1}, a_tok)
        check(status == 400 and err_of(body) == "Product not found", "an unknown price_ref is refused: %s" % body)
        status, body = http(port, "POST", "/api/shop/purchase", {"product_id": jb, "quantity": 0}, a_tok)
        check(status == 400 and err_of(body) == "Invalid quantity.", "a zero quantity is refused: %s" % body)

        check(one("SELECT COALESCE(SUM(amount), 0) FROM junk_transactions WHERE account_id = %d" % a_id)
              == one("SELECT player_junk FROM players WHERE player_id = %d" % a_id) == 100,
              "the ledger of the account sums to its balance (1.00)")
        status, body = http(port, "GET", "/api/shop/displays?tag=limited", token=b_tok)
        check(status == 200 and body["data"]["products"][str(jb)]["purchase_count"] == 0,
              "another account's purchase count is its own (zero)")

        # the official "limited" products (0354_store_catalog: krunker.io's display, verbatim), read and refused
        hovi = official.get("35", {})
        check(hovi.get("name") == "Hovi" and hovi.get("prices") == [{"id": 36, "price_ref": "io.krunker.skin7994", "type": "virtual",
              "virtual": {"amount": 100, "currency_id": 1, "currency": "KR"}, "available": True}]
              and hovi.get("contents") == [{"id": 34, "type": "skin", "ref": "7994", "quantity": 1}]
              and hovi.get("purchase_limit") == 1 and hovi.get("purchase_limit_scope") == "per_user"
              and hovi.get("display_ids") == [37] and hovi.get("purchase_count") == 0,
              "Hovi (35) is krunker.io's product as the display has it: %s" % hovi)
        # nothing listed is unbuyable: the USD products (Groza 26, Paint Ticket 33, Hanami 38...) and the badge (46) are not shown
        check("26" not in official and "33" not in official and "38" not in official and "46" not in official,
              "the USD products and the badge are not listed (Box Office cannot sell them): %s" % sorted(official))
        check(all((p.get("prices") or [{}])[0].get("virtual", {}).get("currency_id") == 1 for p in official.values()),
              "every listed official product is priced in KR, so each one can be bought")
        dyn = [ch for ch in data.get("children", []) if ch.get("id") == 33]
        check(not dyn, "the dynamic USD node (Paint Ticket) is not listed: %s" % dyn)
        query("UPDATE players SET player_funds = 0 WHERE player_id = %d" % a_id)
        status, body = http(port, "POST", "/api/shop/purchase", {"product_id": 35, "quantity": 1}, a_tok)
        check(status == 400 and err_of(body) == "Not enough KR", "an official KR product with no KR is refused: %s %s" % (status, body))
        status, body = http(port, "POST", "/api/shop/purchase", {"product_id": "io.krunker.skin7994", "quantity": 1}, a_tok)
        check(status == 400 and err_of(body) == "Not enough KR",
              "an official product by its price_ref (v10.0.3's product_id) is Hovi (35): %s %s" % (status, body))
        status, body = http(port, "POST", "/api/shop/purchase", {"product_id": 999, "quantity": 1}, a_tok)
        check(status == 400 and err_of(body) == "Product not found", "an unknown low id is refused: %s" % body)
        check(one("SELECT COUNT(*) FROM shop_official_purchases WHERE account_id = %d" % a_id) == 0,
              "a refused official purchase writes no purchase row")

        # the store home's bundle grid (store_bundles; v10's ALL_BUNDLE_DATA keys). Nothing ships: the grid stays empty
        status, body = http(port, "GET", "/api/shop/bundles")
        check(status == 200 and body.get("data") == [], "no bundle is published, so the grid is empty: %s %s" % (status, body))
        query("INSERT INTO store_bundles (id, name, price, original_price, discount_pct, skins, expiry, info, funds, junk, premium,"
              " event_name, event_ends_at, active, sort_order)"
              " VALUES (900001, 'Shop Test Bundle', 4.99, 9.99, 50, '7438,7439', NULL, 'Test info', 0, 0, 0, NULL, NULL, 1, 1)")
        query("INSERT INTO store_bundles (id, name, price, original_price, discount_pct, skins, expiry, info, funds, junk, premium,"
              " event_name, event_ends_at, active, sort_order)"
              " VALUES (900002, 'Shop Test Retired Bundle', 1.0, NULL, NULL, '', NULL, '', 0, 0, 0, NULL, NULL, 0, 2)")
        status, body = http(port, "GET", "/api/shop/bundles")
        want = [{"ID": 900001, "NAME": "Shop Test Bundle", "PRICE": 4.99, "ORIGINAL_PRICE": 9.99, "DISCOUNT_PCT": 50,
                 "SKINS": "7438,7439", "EXPIRY": None, "INFO": "Test info", "FUNDS": 0, "JUNK": 0, "PREMIUM": 0,
                 "EVENT_NAME": None, "EVENT_ENDS_AT": None, "DATE": "", "OWNED": False}]
        check(status == 200 and body.get("data") == want,
              "a published bundle is listed in v10's ALL_BUNDLE_DATA keys and an inactive one is not: %s" % body)

        # Eterno KR purchases: the KR ledger debits the price, the skin is granted, the limit counts the purchase rows
        query("UPDATE players SET player_funds = 250 WHERE player_id = %d" % a_id)
        query("INSERT INTO kr_transactions (account_id, amount, source, ref) VALUES (%d, 250, 'trigger', 'test')" % a_id)
        status, body = http(port, "POST", "/api/shop/purchase", {"product_id": 35, "quantity": 1}, a_tok)
        d = body.get("data", {}) if isinstance(body, dict) else {}
        check(status == 200 and d.get("success") is True and d.get("price_paid") == 100 and d.get("kr") == 150
              and d.get("rewards") == [{"type": "skin", "skin_id": 7994, "amount": 1}],
              "Hovi costs 100 KR and grants its skin: %s %s" % (status, body))
        check(one("SELECT player_funds FROM players WHERE player_id = %d" % a_id) == 150, "the KR balance is 150")
        check(one("SELECT COUNT(*) FROM kr_transactions WHERE account_id = %d AND source = 'shop' AND amount = -100" % a_id) == 1,
              "one KR ledger row of -100 with reason 'shop'")
        check(one("SELECT COALESCE(SUM(delta), 0) FROM skin_transactions WHERE account_id = %d AND skin_id = 7994" % a_id) == 1,
              "one skin of 7994 is in the account (skin ledger)")
        check(one("SELECT COUNT(*) FROM shop_official_purchases WHERE account_id = %d AND product_id = 35" % a_id) == 1,
              "the purchase row is written")
        status, body = http(port, "POST", "/api/shop/purchase", {"product_id": 35, "quantity": 1}, a_tok)
        check(status == 400 and err_of(body) == "Purchase limit reached", "Hovi's per-user limit of 1 refuses a second: %s" % body)
        status, body = http(port, "GET", "/api/shop/displays?tag=limited", token=a_tok)
        check(status == 200 and body["data"]["products"]["35"]["purchase_count"] == 1, "the display counts the one Hovi")

        # v10.0.6 "Fixed limited items sometimes being purchasable more than once": purchases of a limit-1 product sent
        # at the same time queue on the product row, so exactly one passes the limit and the KR is debited once
        query("UPDATE players SET player_funds = 1000 WHERE player_id = %d" % b_id)
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            racing = list(pool.map(lambda _: http(port, "POST", "/api/shop/purchase", {"product_id": 35, "quantity": 1}, b_tok), range(4)))
        won = [r for r in racing if r[0] == 200 and isinstance(r[1], dict) and r[1].get("data", {}).get("success") is True]
        lost = [r for r in racing if r[0] == 400 and err_of(r[1]) == "Purchase limit reached"]
        check(len(won) == 1 and len(lost) == 3, "four simultaneous purchases of a limit-1 product: one passes, three hit the limit: %s" % racing)
        check(one("SELECT COUNT(*) FROM shop_official_purchases WHERE account_id = %d AND product_id = 35" % b_id) == 1
              and one("SELECT player_funds FROM players WHERE player_id = %d" % b_id) == 900,
              "one purchase row and one 100 KR debit for the race")
        status, body = http(port, "POST", "/api/shop/purchase", {"product_id": 26, "quantity": 1}, a_tok)
        check(status == 400 and err_of(body) == "Payment is not available on Box Office",
              "a USD product (Sandstorm OTs-14 Groza, 2.99, not listed) is refused, nothing written: %s %s" % (status, body))

        # the Junk product of krunker.io's display: 2000 KR for 100 Junk (currency 2, hundredths), limit 1000 globally
        query("UPDATE players SET player_funds = 4000 WHERE player_id = %d" % a_id)
        jbefore = one("SELECT player_junk FROM players WHERE player_id = %d" % a_id)
        status, body = http(port, "POST", "/api/shop/purchase", {"product_id": 15, "quantity": 2}, a_tok)
        d = body.get("data", {}) if isinstance(body, dict) else {}
        check(status == 200 and d.get("price_paid") == 4000 and d.get("rewards") == [{"type": "currency", "currency_id": 2, "currency": 2, "amount": 20000}],
              "two Junk products cost 4000 KR and grant 200 Junk (20000 hundredths): %s %s" % (status, body))
        check(one("SELECT player_junk FROM players WHERE player_id = %d" % a_id) == jbefore + 20000
              and one("SELECT player_funds FROM players WHERE player_id = %d" % a_id) == 0,
              "the Junk is credited and the KR spent")

        # Eterno (45): 200000 KR and skin 8003, the same grant path as Hovi
        query("UPDATE players SET player_funds = 200000 WHERE player_id = %d" % a_id)
        status, body = http(port, "POST", "/api/shop/purchase", {"product_id": 45, "quantity": 1}, a_tok)
        d = body.get("data", {}) if isinstance(body, dict) else {}
        check(status == 200 and d.get("price_paid") == 200000 and d.get("rewards") == [{"type": "skin", "skin_id": 8003, "amount": 1}]
              and one("SELECT player_funds FROM players WHERE player_id = %d" % a_id) == 0
              and one("SELECT COALESCE(SUM(delta), 0) FROM skin_transactions WHERE account_id = %d AND skin_id = 8003" % a_id) == 1,
              "Eterno costs 200000 KR, grants skin 8003 and the KR is spent: %s %s" % (status, body))

        # bundles: v10's order (owned last, price low first, DATE newest first) and OWNED per account
        query("INSERT INTO store_bundles (id, name, price, original_price, discount_pct, skins, expiry, info, funds, junk, premium,"
              " event_name, event_ends_at, active, sort_order, bundle_date)"
              " VALUES (900003, 'Shop Test Bundle C', 3.0, NULL, NULL, '', NULL, '', 0, 0, 0, NULL, NULL, 1, 3, '2026-02-01T00:00:00Z')")
        query("INSERT INTO store_bundles (id, name, price, original_price, discount_pct, skins, expiry, info, funds, junk, premium,"
              " event_name, event_ends_at, active, sort_order, bundle_date)"
              " VALUES (900004, 'Shop Test Bundle D', 9.0, NULL, NULL, '', NULL, '', 0, 0, 0, NULL, NULL, 1, 4, '2026-01-01T00:00:00Z')")
        query("INSERT INTO store_bundles (id, name, price, original_price, discount_pct, skins, expiry, info, funds, junk, premium,"
              " event_name, event_ends_at, active, sort_order, bundle_date)"
              " VALUES (900005, 'Shop Test Bundle E', 3.0, NULL, NULL, '', NULL, '', 0, 0, 0, NULL, NULL, 1, 5, '2026-03-01T00:00:00Z')")
        status, body = http(port, "GET", "/api/shop/bundles", token=a_tok)
        rows = body.get("data", []) if isinstance(body, dict) else []
        check(status == 200 and [r["ID"] for r in rows] == [900005, 900003, 900001, 900004]
              and all(r["OWNED"] is False for r in rows),
              "the bundles are in v10's order (price low first, then DATE newest first): %s"
              % [r["ID"] for r in rows])
        query("INSERT INTO store_bundle_purchases (account_id, bundle_id) VALUES (%d, 900003)" % a_id)
        status, body = http(port, "GET", "/api/shop/bundles", token=a_tok)
        rows = body.get("data", []) if isinstance(body, dict) else []
        owned = {r["ID"]: r["OWNED"] for r in rows}
        check(status == 200 and [r["ID"] for r in rows] == [900005, 900001, 900004, 900003] and owned[900003] is True
              and owned[900005] is False, "an owned bundle is last, the others keep their order: %s" % owned)
        status, body = http(port, "GET", "/api/shop/bundles", token=b_tok)
        rows = body.get("data", []) if isinstance(body, dict) else []
        check(status == 200 and all(r["OWNED"] is False for r in rows), "another account owns none of them")

        # a bundle is USD: its purchase is refused (no payment), an owned one and an unknown one with their own reasons
        status, body = http(port, "POST", "/api/shop/bundles/purchase", {"bundle_id": 900001})
        check(status == 401 and err_of(body) == "LoginRequired", "a bundle purchase needs a token: %s %s" % (status, body))
        status, body = http(port, "POST", "/api/shop/bundles/purchase", {"bundle_id": 900001}, a_tok)
        check(status == 400 and err_of(body) == "Payment is not available on Box Office", "a USD bundle is refused: %s %s" % (status, body))
        status, body = http(port, "POST", "/api/shop/bundles/purchase", {"bundle_id": 900003}, a_tok)
        check(status == 400 and err_of(body) == "Already purchased", "an owned bundle is already purchased: %s" % body)
        status, body = http(port, "POST", "/api/shop/bundles/purchase", {"bundle_id": 900002}, a_tok)
        check(status == 400 and err_of(body) == "Bundle not found", "an inactive bundle is not found: %s" % body)
        check(one("SELECT COUNT(*) FROM store_bundle_purchases WHERE account_id = %d" % a_id) == 1,
              "the refused bundle purchases wrote nothing")


def start_brain(env, log_path, extra):
    with open(log_path, "a") as log:
        log.write("== start %s\n" % extra)
        proc = subprocess.Popen([BRAIN, "--brain.host=127.0.0.1", "--brain.port=0", "--brain.id=klt-%d" % os.getpid(),
                                 "--accounts.signup_rewards.sweep_s=1", "--accounts.login_throttle_s=0"] + extra,
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
            print("shop test: %s unavailable: skipped" % DRIVER)
            sys.exit(r.returncode)
        DBCFG = json.loads(r.stdout)
        dbenv = {"KN_DATABASE_" + k.upper(): str(v) for k, v in DBCFG.items()}
        drop = scratch + ["drop", DRIVER, SCHEMA]
    work = tempfile.mkdtemp(prefix="kn-shop.", dir=os.path.join(os.path.expanduser("~"), ".cache"))
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=SECRET, **dbenv)
    log_path = os.path.join(work, "brain.log")
    proc = None
    try:
        subprocess.run([BRAIN, "--migrate-only"], cwd=ROOT, env=env, check=True, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)
        proc, port = start_brain(env, log_path, [])
        asyncio.run(scenario(port))
    finally:
        if proc:
            proc.terminate()
            proc.wait(5)
        subprocess.run(drop)
    with open(log_path) as f:
        bad = [l.strip() for l in f if re.search(r"failed:|ERROR|FATAL", l)]
    check(not bad, "no statement errors in the brain log %s" % bad[:3])
    print("shop test: " + ("OK" if not FAILS else "%d failure(s)" % len(FAILS)))
    if FAILS:
        print("log kept in " + work)
    else:
        shutil.rmtree(work, ignore_errors=True)
    sys.exit(1 if FAILS else 0)


main()
