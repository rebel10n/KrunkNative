#!/usr/bin/env python3
"""The v10 market, items, trades and players routes on kn_brain's root paths (services/brain/v10_market.c;
route inventory doc/v10/api-inventory.md; the consumer is v10.0.3, decoded 5160 for the REST client, and
v10.js 26857-27182 is the readable v10.0.2 copy the inventory cites).

  v10_api_market_test.py <kn_brain | build dir> [repo root]
  (CTest passes the kn_brain binary and the source root; a build dir means BUILD/services/brain/kn_brain.)

A scratch sqlite database (db_scratch.py), kn_brain --migrate-only, then kn_brain on an ephemeral port.
Two accounts register over POST /auth/register (their Bearer tokens are the callers); the rows they need
(levels, funds, inventories, one old listing, a black-market purchase, a ranked peak) are seeded with
sqlite3, then every route runs through HTTP: one check per route, the status and the JSON fields v10 reads.

Exit 0 when every check passes, 1 otherwise, 77 when kn_brain or the sqlite scratch database is
unavailable (CTest's skip).
"""
import json
import os
import re
import shutil
import signal
import sqlite3
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ARG = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
ROOT = os.path.abspath(sys.argv[2]) if len(sys.argv) > 2 else os.path.abspath(os.path.join(HERE, "..", "..", ".."))
BRAIN = os.path.join(ARG, "services", "brain", "kn_brain") if os.path.isdir(ARG) else ARG
NAME = "kn_test_v10mkt_%d" % os.getpid()
SECRET = "kn-v10-market-test-secret-%d" % os.getpid()
RESULTS = []

# an item index that is tradable in assets/skins.json, and one that is not in it
ITEM = 0
ITEM_B = 1
NO_ITEM = 999999
# the black-market item the buyer buys (Nuke Tamer, req "stats" on player_stats "n" >= 1000, 3000 funds) and
# one it already owns (1 Year Cape: bought from the Black Market, never again)
BM_BUY = 1642
BM_OWNED = 1483
SELLER = "mkt_seller"
BUYER = "mkt_buyer"
PASSWORD = "marketpass1"
# any player name: the account-bound routes refuse without looking it up
PLAYER = "kn_test_player"
# an old listing (not fresh: a buy is refused for the grace period after posting)
OLD = "2000-01-01 00:00:00"


def check(name, ok, detail=""):
    RESULTS.append(bool(ok))
    print(("PASS  " if ok else "FAIL  ") + name + ("" if ok or not detail else "  -- " + detail), flush=True)


def read(path):
    try:
        with open(path, errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def wait_for(fn, secs):
    end = time.time() + secs
    while time.time() < end:
        v = fn()
        if v:
            return v
        time.sleep(0.1)
    return None


def http(port, method, path, body=None, token=None):
    data = json.dumps(body).encode() if body is not None else None
    headers = {"Content-Type": "application/json"} if data is not None else {}
    if token:
        headers["Authorization"] = "Bearer " + token
    req = urllib.request.Request("http://127.0.0.1:%d%s" % (port, path), data=data, method=method, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            status, raw = r.status, r.read()
    except urllib.error.HTTPError as e:
        status, raw = e.code, e.read()
    try:
        return status, json.loads(raw or b"null")
    except ValueError:
        return status, None


def err_of(reply):
    return reply.get("error") if isinstance(reply, dict) and isinstance(reply.get("error"), dict) else {}


def data_of(reply):
    return reply.get("data") if isinstance(reply, dict) else None


def is_int(v):
    return isinstance(v, int) and not isinstance(v, bool)


# ---- the v10 REST answers (v10.0.3 decoded 5160 is the REST client; v10.js lines in each label)

def expect_data(label, status, reply, pred, want="200 {data}"):
    ok = status == 200 and reply.get("error") is None and "data" in reply and bool(pred(reply.get("data")))
    check("%s -> %s" % (label, want), ok, "HTTP %d %s" % (status, json.dumps(reply)[:200]))
    return reply.get("data") if isinstance(reply, dict) else None


def expect_error(label, status, reply, code, http_status, message=None):
    err = err_of(reply)
    ok = status == http_status and err.get("code") == code and bool(err.get("message")) and "data" not in reply
    if message is not None:
        ok = ok and err.get("message") == message
    check("%s -> %d %s" % (label, http_status, code), ok, "HTTP %d %s" % (status, json.dumps(reply)[:200]))


def expect_refusal(label, status, reply):
    expect_error(label, status, reply, "NOT_IMPLEMENTED", 403)


def expect_not_found(label, status, reply):
    expect_error(label, status, reply, "NOT_FOUND", 404)


def eq(value):
    return lambda data: data == value

def expect_name(method, path, kind, ref):
    return "%s %s -> %s (v10.js %s)" % (method, path, kind, ref)


def run_route(port, method, path, body, kind, pred, ref):
    status, reply = http(port, method, path, body if body is not None else ({} if method == "POST" else None))
    name = expect_name(method, path, kind, ref)
    if not isinstance(reply, dict):
        check(name, False, "HTTP %d, body not an object" % status)
        return
    if kind == "EMPTY":
        ok = status == 200 and reply.get("error") is None and "data" in reply and pred(reply.get("data"))
        check(name, ok, "HTTP %d %s" % (status, json.dumps(reply)[:160]))
    elif kind == "REFUSAL":
        err = reply.get("error") if isinstance(reply.get("error"), dict) else {}
        ok = status == 403 and err.get("code") == "NOT_IMPLEMENTED" and bool(err.get("message")) and "data" not in reply
        check(name, ok, "HTTP %d %s" % (status, json.dumps(reply)[:160]))
    else:  # NOT_FOUND
        err = reply.get("error") if isinstance(reply.get("error"), dict) else {}
        ok = status == 404 and err.get("code") == "NOT_FOUND" and "data" not in reply
        check(name, ok, "HTTP %d %s" % (status, json.dumps(reply)[:160]))



def seed(path, stmts):
    con = sqlite3.connect(path, timeout=30)
    try:
        for sql, args in stmts:
            con.execute(sql, args)
        con.commit()
    finally:
        con.close()


def query(path, sql, args=()):
    con = sqlite3.connect(path, timeout=30)
    try:
        return con.execute(sql, args).fetchall()
    finally:
        con.close()


def account_id(path, name):
    rows = query(path, "SELECT id FROM accounts WHERE username = ?", (name,))
    return rows[0][0] if rows else 0


def register(port, name):
    status, reply = http(port, "POST", "/auth/register", {"username": name, "password": PASSWORD})
    token = reply.get("token") if isinstance(reply, dict) else None
    check("POST /auth/register %s (the caller's Bearer token)" % name, status == 200 and bool(token), "HTTP %d" % status)
    return token


def market_scenario(port, path):
    """Every market, items, trades and players route this module answers (inventory rows 126-144, 151-157,
    163, 164, 169, 172)."""
    s_tok = register(port, SELLER)
    b_tok = register(port, BUYER)
    if not s_tok or not b_tok:
        return
    s_id, b_id = account_id(path, SELLER), account_id(path, BUYER)
    seed(path, [
        # level 10 (market_min_level) and funds; the buyer's player_stats "n" is the Nuke Tamer requirement
        ("UPDATE players SET player_score = 200000, player_funds = 1000 WHERE player_id = ?", (s_id,)),
        ("UPDATE players SET player_score = 200000, player_funds = 100000, player_stats = ? WHERE player_id = ?",
         ('{"n": 2000}', b_id)),
        # the seller holds 2 of skin 0 and 2 of skin 1; one skin 0 is in an old listing (escrowed, not in inventory)
        ("INSERT INTO inventory (account_id, skin_id, count) VALUES (?, ?, 2)", (s_id, ITEM)),
        ("INSERT INTO inventory (account_id, skin_id, count) VALUES (?, ?, 2)", (s_id, ITEM_B)),
        ("INSERT INTO market_listings (seller_id, skin_id, price, created_at) VALUES (?, ?, 500, ?)", (s_id, ITEM, OLD)),
        # the buyer owns a black-market item already, and has the Nuke Tamer's rating
        ("INSERT INTO skin_transactions (account_id, skin_id, delta, source) VALUES (?, ?, 1, 'blackmarket')",
         (b_id, BM_OWNED)),
        ("INSERT INTO ranked_profiles (account_id, season, region, peak_mmr) VALUES (?, 1, 'na', 1234)", (b_id,)),
    ])
    listing_a = query(path, "SELECT id FROM market_listings WHERE seller_id = ? AND price = 500", (s_id,))[0][0]

    # ---- guests: the caller-owned reads answer their empty shape, the writes need a login
    st, r = http(port, "GET", "/market/inventory")
    expect_data("126/127 GET /market/inventory (guest)", st, r, lambda d: d == [], "[] (v10.0.3 5160; v10.js 128416)")
    st, r = http(port, "GET", "/market/sales")
    expect_data("128 GET /market/sales (guest)", st, r, lambda d: d == [], "[] (v10.0.3 5160; v10.js 134079)")
    st, r = http(port, "GET", "/market/history?sort=newest&limit=50")
    expect_data("139 GET /market/history (guest)", st, r,
                lambda d: isinstance(d, dict) and d.get("history") == [] and d.get("has_more") is False,
                "{history: [], has_more} (v10.0.3 5930; v10.js 131476)")
    st, r = http(port, "GET", "/market/black-market")
    expect_data("136 GET /market/black-market (guest)", st, r, lambda d: d == [], "[] (v10.0.3 5160; v10.js 128486)")
    st, r = http(port, "GET", "/market/black-market/rank")
    expect_data("137 GET /market/black-market/rank (guest)", st, r, lambda d: d is None,
                "null: no rank (v10.0.3 5160; v10.js 128494)")
    st, r = http(port, "GET", "/trades/history?offset=0&limit=30&includeOpen=true")
    expect_data("151 GET /trades/history (guest)", st, r,
                lambda d: isinstance(d, dict) and d.get("open") == [] and d.get("completed") == []
                and d.get("completed_has_more") is False, "{open, completed, completed_has_more} (v10.js 108327)")
    st, r = http(port, "POST", "/market/list", {"item_id": ITEM, "price": 10})
    expect_error("132 POST /market/list (guest)", st, r, "LOGIN_REQUIRED", 401, "Must be logged in")
    st, r = http(port, "POST", "/trades/create", {"target_player_id": b_id, "my_offer": [], "their_offer": [], "message": ""})
    expect_error("152 POST /trades/create (guest)", st, r, "LOGIN_REQUIRED", 401)
    st, r = http(port, "GET", "/trades/inventory/%d" % b_id)
    expect_error("156 GET /trades/inventory/{id} (guest)", st, r, "LOGIN_REQUIRED", 401)
    st, r = http(port, "POST", "/players/%s/gift" % BUYER, {"amount": 10, "message": ""})
    expect_error("172 POST /players/{name}/gift (guest)", st, r, "LOGIN_REQUIRED", 401)

    # ---- writes, as the seller and the buyer
    st, r = http(port, "POST", "/market/list", {"item_id": ITEM, "price": 0}, b_tok)
    expect_error("132 POST /market/list (price 0)", st, r, "BAD_REQUEST", 400, "Invalid price")
    st, r = http(port, "POST", "/market/list", {"item_id": ITEM_B, "price": 700}, s_tok)
    d = expect_data("132 POST /market/list (skin %d at 700, to unlist)" % ITEM_B, st, r,
                    lambda d: isinstance(d, dict) and is_int(d.get("listingId")) and is_int(d.get("fee")),
                    "{cnt, funds, listingId, fee} (v10.0.3 5160; v10.js 128440)")
    unlist_id = d.get("listingId") if isinstance(d, dict) else 0
    st, r = http(port, "POST", "/market/unlist", {"market_id": unlist_id}, s_tok)
    expect_data("134 POST /market/unlist (own listing)", st, r, lambda d: isinstance(d, dict) and d.get("response") == "SUCCESS",
                "{response: SUCCESS} (v10.0.3 5160; v10.js 132978)")
    st, r = http(port, "POST", "/market/list", {"item_id": ITEM_B, "price": 700}, s_tok)
    d = expect_data("132 POST /market/list (skin %d at 700, kept)" % ITEM_B, st, r,
                    lambda d: isinstance(d, dict) and is_int(d.get("listingId")), "{listingId}")
    listing_b = d.get("listingId") if isinstance(d, dict) else 0

    st, r = http(port, "POST", "/market/purchase", {"market_id": listing_a}, s_tok)
    expect_error("133 POST /market/purchase (own listing)", st, r, "BAD_REQUEST", 400, "Cant buy your own item")
    st, r = http(port, "POST", "/market/purchase", {"market_id": NO_ITEM}, b_tok)
    expect_error("133 POST /market/purchase (unknown listing)", st, r, "BAD_REQUEST", 400, "Item Unavailable")
    st, r = http(port, "POST", "/market/purchase", {"market_id": listing_a}, b_tok)
    expect_data("133 POST /market/purchase (old listing, buyer)", st, r,
                lambda d: isinstance(d, dict) and d.get("i") == ITEM and d.get("price") == 500,
                "{f, i, sid, m, price} (v10.0.3 5160; v10.js 132836)")

    st, r = http(port, "POST", "/trades/create",
                 {"target_player_id": b_id, "my_offer": [{"i": ITEM, "c": 1}], "their_offer": [], "message": "t1"}, s_tok)
    d = expect_data("152 POST /trades/create (seller offers 1 x skin %d)" % ITEM, st, r,
                    lambda d: isinstance(d, dict) and is_int(d.get("trid")) and "funds" in d,
                    "{trid, funds} (v10.0.3 5160; v10.js 109194)")
    t1 = d.get("trid") if isinstance(d, dict) else 0
    st, r = http(port, "POST", "/trades/%d/accept" % t1, {}, b_tok)
    expect_data("153 POST /trades/{id}/accept (buyer)", st, r,
                lambda d: isinstance(d, dict) and d.get("trid") == t1 and "funds" in d,
                "{trid, funds} (v10.0.3 5160; v10.js 135709)")
    st, r = http(port, "POST", "/trades/create",
                 {"target_player_id": b_id, "my_offer": [{"i": ITEM, "c": 1}], "their_offer": [], "message": "t2"}, s_tok)
    expect_data("152 POST /trades/create (declined later)", st, r, lambda d: isinstance(d, dict) and is_int(d.get("trid")))
    st, r = http(port, "POST", "/trades/decline-all", {}, b_tok)
    expect_data("155 POST /trades/decline-all (buyer)", st, r, lambda d: isinstance(d, dict) and d.get("success") is True,
                "{success} (v10.0.3 5160; v10.js 135779)")
    st, r = http(port, "POST", "/trades/create",
                 {"target_player_id": b_id, "my_offer": [{"i": ITEM_B, "c": 1}], "their_offer": [], "message": "t3"}, s_tok)
    d = expect_data("152 POST /trades/create (cancelled later)", st, r, lambda d: isinstance(d, dict) and is_int(d.get("trid")))
    t3 = d.get("trid") if isinstance(d, dict) else 0
    st, r = http(port, "POST", "/trades/%d/cancel" % t3, {}, s_tok)
    expect_data("154 POST /trades/{id}/cancel (sender)", st, r, lambda d: isinstance(d, dict) and d.get("success") is True,
                "{success} (v10.0.3 5160; v10.js 108339)")
    st, r = http(port, "POST", "/trades/create",
                 {"target_player_id": s_id, "my_offer": [], "their_offer": [], "message": ""}, s_tok)
    check("152 POST /trades/create (to yourself) -> 400 error", st == 400 and bool(err_of(r).get("message")) and "data" not in r,
          "HTTP %d %s" % (st, json.dumps(r)[:160]))

    st, r = http(port, "GET", "/trades/history?offset=0&limit=30&includeOpen=true", None, s_tok)
    expect_data("151 GET /trades/history (seller)", st, r,
                lambda d: isinstance(d, dict) and d.get("open") == [] and d.get("completed_has_more") is False
                and len(d.get("completed") or []) == 3 and all(
                    "sksell" in t and "skbuy" in t and "csell" in t and "cbuy" in t and "trid" in t for t in d["completed"])
                and any(t.get("trid") == t1 and t.get("pl") == s_id and t.get("player") == SELLER
                        and t.get("buyer") == BUYER and t.get("sksell") == [{"ind": ITEM}] and t.get("skbuy") == []
                        and t.get("csell") == [] and t.get("cbuy") == [] for t in d["completed"]),
                "{open, completed: [{trid, pl, player, buyer, sksell, skbuy, csell, cbuy, status}]} (v10.js 108327)")
    st, r = http(port, "GET", "/trades/inventory/%d" % b_id, None, s_tok)
    expect_data("156 GET /trades/inventory/{id} (seller reads the buyer)", st, r,
                lambda d: isinstance(d, dict) and isinstance(d.get("my_inventory"), list)
                and any(it.get("i") == ITEM and it.get("c") == 2 for it in d.get("their_inventory") or []),
                "{my_inventory, their_inventory} (v10.0.3 5160; v10.js 109226)")
    st, r = http(port, "GET", "/trades/inventory/%s" % BUYER, None, s_tok)
    expect_data("156 GET /trades/inventory/{name} (seller reads the buyer)", st, r,
                lambda d: isinstance(d, dict) and isinstance(d.get("their_inventory"), list), "{my_inventory, their_inventory}")
    st, r = http(port, "GET", "/trades/player-search?name=%s" % BUYER, None, s_tok)
    expect_data("157 GET /trades/player-search (buyer)", st, r,
                lambda d: isinstance(d, dict) and d.get("player_id") == b_id and d.get("player_name") == BUYER
                and "player_score" in d, "{player_id, player_name, player_score} (v10.js 108349)")
    st, r = http(port, "GET", "/trades/player-search?name=nobody_here_x", None, s_tok)
    expect_not_found("157 GET /trades/player-search (unknown)", st, r)

    st, r = http(port, "POST", "/players/%s/gift" % BUYER, {"amount": 50, "message": "hi"}, s_tok)
    expect_data("172 POST /players/{name}/gift (50 KR)", st, r,
                lambda d: isinstance(d, dict) and d.get("amount") == 50 and is_int(d.get("funds")),
                "{funds, amount, fee} (v10.0.3 5160; v10.js 109361)")
    st, r = http(port, "POST", "/players/%s/gift" % BUYER, {"amount": 0, "message": ""}, s_tok)
    expect_error("172 POST /players/{name}/gift (amount 0)", st, r, "BAD_REQUEST", 400, "Invalid amount")
    st, r = http(port, "POST", "/players/%s/verify" % SELLER, {"featured": True}, b_tok)
    expect_data("169 POST /players/{name}/verify (not a developer: no change)", st, r, lambda d: d is None,
                "null (v10.0.3 5160; v10.js 109590; the socket answers null)")

    st, r = http(port, "GET", "/players/%s/sales?range=0" % SELLER)
    expect_data("163 GET /players/{name}/sales (seller, today)", st, r,
                lambda d: isinstance(d, list) and sum(int(x.get("f", 0)) for x in d) == 500 and all(
                    "d" in x and "f" in x for x in d), "[{d, f}] daily KR (v10.js 109908)")
    st, r = http(port, "GET", "/players/%s/listings" % SELLER)
    expect_data("164 GET /players/{name}/listings (seller)", st, r,
                lambda d: isinstance(d, list) and any(x.get("marketid") == listing_b and x.get("skinindex") == ITEM_B
                                                      and x.get("funds") == 700 for x in d),
                "[{marketid, skinindex, funds}] (v10.0.3 5160; v10.js 110044)")
    st, r = http(port, "GET", "/players/nobody_here_x/listings")
    expect_data("164 GET /players/{name}/listings (unknown name)", st, r, lambda d: d == [], "[]")

    # ---- public reads
    st, r = http(port, "GET", "/market/listings?items=%d" % ITEM_B)
    expect_data("126 GET /market/listings?items= (skin %d)" % ITEM_B, st, r,
                lambda d: isinstance(d, dict) and d.get("has_more") is False and any(
                    x.get("marketid") == listing_b and x.get("skinindex") == ITEM_B and x.get("funds") == 700
                    for x in d.get("listings") or []),
                "{listings: [{marketid, skinindex, funds}], has_more, next_cursor_id} (v10.0.3 5160; v10.js 128405)")
    st, r = http(port, "GET", "/market/listings?items=%d" % ITEM)
    expect_data("126 GET /market/listings?items= (sold skin %d: none open)" % ITEM, st, r,
                lambda d: isinstance(d, dict) and d.get("listings") == [], "{listings: []}")
    # paging: a second open listing of skin 1 (900), read two rows at a time by price (highest first)
    st, r = http(port, "POST", "/market/list", {"item_id": ITEM_B, "price": 900}, s_tok)
    d = expect_data("132 POST /market/list (skin %d at 900, for paging)" % ITEM_B, st, r,
                    lambda d: isinstance(d, dict) and is_int(d.get("listingId")), "{listingId}")
    listing_c = d.get("listingId") if isinstance(d, dict) else 0
    st, r = http(port, "GET", "/market/listings?items=%d&sort=price_desc&limit=1" % ITEM_B)
    d = expect_data("126 GET /market/listings (page 1: limit 1, price_desc)", st, r,
                    lambda d: isinstance(d, dict) and [x.get("funds") for x in d.get("listings") or []] == [900]
                    and d.get("has_more") is True and d.get("next_cursor_id") == listing_c,
                    "{listings: [900], has_more: true, next_cursor_id} (v10.0.3 5160; v10.js 132798)")
    st, r = http(port, "GET", "/market/listings?items=%d&sort=price_desc&limit=1&cursor_id=%d" % (ITEM_B, listing_c))
    expect_data("126 GET /market/listings (page 2: after cursor_id)", st, r,
                lambda d: isinstance(d, dict) and [x.get("funds") for x in d.get("listings") or []] == [700]
                and d.get("has_more") is False and d.get("next_cursor_id") is None,
                "{listings: [700], has_more: false}")
    st, r = http(port, "POST", "/market/unlist", {"market_id": listing_c}, s_tok)
    expect_data("134 POST /market/unlist (the paging listing)", st, r,
                lambda d: isinstance(d, dict) and d.get("response") == "SUCCESS", "{response: SUCCESS}")
    st, r = http(port, "GET", "/market/stats?limit=20")
    expect_data("129 GET /market/stats", st, r,
                lambda d: isinstance(d, dict) and d.get("has_more") is False and any(
                    x.get("ski") == ITEM and is_int(x.get("onS")) for x in d.get("stats") or []),
                "{stats: [{ski, onS, low, high}], has_more} (v10.0.3 5160; v10.js 128424)")
    st, r = http(port, "GET", "/market/items/%d/sales" % ITEM)
    expect_data("130 GET /market/items/{id}/sales", st, r,
                lambda d: isinstance(d, dict) and d.get("ind") == ITEM and is_int(d.get("onS")) and any(
                    o.get("nm") == BUYER for o in d.get("owners") or []),
                "{ind, onS, inC, high, low, owners: [{nm, ct, su, ft}]} (v10.js 26887)")
    st, r = http(port, "GET", "/market/items/%d/sales" % NO_ITEM)
    expect_not_found("130 GET /market/items/{id}/sales (unknown)", st, r)
    st, r = http(port, "GET", "/market/items/%d/sales-statistics?daysback=7" % ITEM)
    expect_data("131 GET /market/items/{id}/sales-statistics", st, r,
                lambda d: isinstance(d, list) and sum(int(x.get("t", 0)) for x in d) >= 1 and all(
                    "d" in x and "f" in x and "t" in x for x in d), "[{d, f, t}] (v10.0.3 5160; v10.js 134931)")
    st, r = http(port, "GET", "/market/sales", None, s_tok)
    expect_data("128 GET /market/sales (seller's open listings)", st, r,
                lambda d: isinstance(d, list) and any(x.get("marketid") == listing_b and x.get("skinindex") == ITEM_B
                                                      and "seller_id" not in x for x in d),
                "[{marketid, skinindex, funds}] (v10.0.3 5160; v10.js 134079)")
    st, r = http(port, "GET", "/market/inventory", None, s_tok)
    expect_data("127 GET /market/inventory (seller)", st, r,
                lambda d: isinstance(d, list) and any(is_int(x.get("i")) and is_int(x.get("cnt")) and x["cnt"] > 0 for x in d),
                "[{i, cnt}] (v10.0.3 5160; v10.js 128416)")
    st, r = http(port, "GET", "/market/history?sort=newest&limit=50", None, b_tok)
    expect_data("139 GET /market/history (buyer)", st, r,
                lambda d: isinstance(d, dict) and d.get("has_more") is False and any(
                    h.get("type") == "buy" and h.get("skinindex") == ITEM and h.get("amount") == 500
                    and h.get("other_party_name") == SELLER and h.get("status") == "completed" for h in d.get("history") or []),
                "{history: [{type, date, skinindex, amount, other_party_name, status}]} (v10.0.3 5930; v10.js 131476)")
    st, r = http(port, "GET", "/market/history?sort=newest&limit=50", None, s_tok)
    expect_data("139 GET /market/history (seller)", st, r,
                lambda d: isinstance(d, dict) and any(h.get("type") == "sell" and h.get("other_party_name") == BUYER
                                                      for h in d.get("history") or []), "a sell row")

    # ---- the black market
    st, r = http(port, "GET", "/market/black-market", None, b_tok)
    expect_data("136 GET /market/black-market (buyer's owned items)", st, r,
                lambda d: isinstance(d, list) and BM_OWNED in d, "[skin index] (v10.0.3 5160; v10.js 128486)")
    st, r = http(port, "GET", "/market/black-market/rank", None, b_tok)
    expect_data("137 GET /market/black-market/rank (buyer)", st, r,
                lambda d: isinstance(d, dict) and d.get("peak_mmr") == 1234, "{peak_mmr} (v10.0.3 5160; v10.js 128494)")
    st, r = http(port, "POST", "/market/black-market/purchase", {"item_index": ITEM}, b_tok)
    expect_error("138 POST /market/black-market/purchase (not a black-market item)", st, r, "BAD_REQUEST", 400,
                 "Item Unavailable")
    st, r = http(port, "POST", "/market/black-market/purchase", {"item_index": BM_BUY}, b_tok)
    expect_data("138 POST /market/black-market/purchase (stats requirement met)", st, r,
                lambda d: isinstance(d, dict) and d.get("item") == BM_BUY and d.get("kr") == 3000 and is_int(d.get("funds")),
                "{item, kr, funds} (v10.0.3 5160; v10.js 136215)")
    st, r = http(port, "GET", "/market/black-market", None, b_tok)
    expect_data("136 GET /market/black-market (after the purchase)", st, r,
                lambda d: isinstance(d, list) and BM_OWNED in d and BM_BUY in d, "owned includes the bought item")
    st, r = http(port, "POST", "/market/black-market/purchase", {"item_index": BM_BUY})
    expect_error("138 POST /market/black-market/purchase (guest)", st, r, "LOGIN_REQUIRED", 401)

    # ---- items
    st, r = http(port, "GET", "/items/%d/view" % ITEM)
    expect_data("140 GET /items/{id}/view", st, r,
                lambda d: isinstance(d, dict) and isinstance(d.get("stats"), dict) and is_int(d["stats"].get("onS"))
                and any(o.get("nm") == BUYER for o in d.get("owners") or []) and isinstance(d.get("sales"), list)
                and isinstance(d.get("listings"), list) and d.get("stackable") is False and d.get("wearable") is False,
                "{stats, owners, sales, listings, stackable, wearable} (v10.0.3 5160; v10.js 128558)")
    st, r = http(port, "GET", "/items/%d/view" % NO_ITEM)
    expect_not_found("140 GET /items/{id}/view (unknown)", st, r)
    st, r = http(port, "GET", "/items/%d/sales?range=0" % ITEM)
    expect_data("141 GET /items/{id}/sales?range=0", st, r,
                lambda d: isinstance(d, list) and sum(int(x.get("t", 0)) for x in d) >= 1, "[{d, f, t}] (v10.js 129914)")
    st, r = http(port, "GET", "/items/%d/instances?sort=mint" % ITEM)
    expect_data("142 GET /items/{id}/instances?sort=mint (no per-copy instances)", st, r, lambda d: d == [],
                "[] (v10.0.3 5160; v10.js 128573)")
    st, r = http(port, "POST", "/items/%d/paint" % ITEM, {"item_id": ITEM, "paint_item_id": 2}, s_tok)
    expect_refusal("143 POST /items/{id}/paint", st, r)
    st, r = http(port, "POST", "/items/%d/style" % ITEM, {"item_id": ITEM, "style": 1}, s_tok)
    expect_refusal("144 POST /items/{id}/style", st, r)

    # ---- quick sell (after the trades: the seller holds one skin 0 again)
    st, r = http(port, "POST", "/market/quick-sell", {"item_id": ITEM, "action": 1, "amount": 0}, s_tok)
    expect_refusal("135 POST /market/quick-sell (junk mode: not built)", st, r)
    st, r = http(port, "POST", "/market/quick-sell", {"item_id": ITEM, "action": 0, "amount": 0}, s_tok)
    expect_data("135 POST /market/quick-sell (KR, one)", st, r,
                lambda d: isinstance(d, dict) and d.get("count") == 0 and is_int(d.get("funds")),
                "{funds, junk, count} (v10.0.3 5160; v10.js 131580)")
    st, r = http(port, "POST", "/market/quick-sell", {"item_id": ITEM_B, "action": 2, "amount": 2}, s_tok)
    expect_data("135 POST /market/quick-sell (destroy, all)", st, r,
                lambda d: isinstance(d, dict) and d.get("count") == 0, "{count: 0}")


def run_all(port, path):
    market_scenario(port, path)


def stop(proc):
    if proc.poll() is None:
        try:
            os.killpg(proc.pid, signal.SIGTERM)
        except OSError:
            proc.terminate()
        try:
            proc.wait(5)
        except subprocess.TimeoutExpired:
            proc.kill()


def main():
    if not os.access(BRAIN, os.X_OK):
        print("v10_api_market_test: no kn_brain at %s: skipped" % BRAIN)
        return 77
    r = subprocess.run([sys.executable, "-I", os.path.join(HERE, "db_scratch.py"), ROOT, "create", "sqlite", NAME],
                       capture_output=True)
    if r.returncode:
        print("v10_api_market_test: no sqlite scratch database: skipped")
        return 77
    db = json.loads(r.stdout)
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=SECRET,
               **{"KN_DATABASE_" + k.upper(): str(v) for k, v in db.items()})
    work = tempfile.mkdtemp(prefix="kn-v10-market-test.")
    log = os.path.join(work, "brain.log")
    procs = []
    try:
        mig = subprocess.run([BRAIN, "--migrate-only"], cwd=ROOT, env=env, capture_output=True)
        check("the migrations apply", mig.returncode == 0, mig.stderr.decode(errors="replace")[-200:])
        if mig.returncode:
            return 1
        brain = subprocess.Popen([BRAIN, "--brain.host=127.0.0.1", "--brain.port=0", "--brain.region=local",
                                  "--brain.id=v10-market-test-%d" % os.getpid(), "--finder.poll_interval_s=1"],
                                 cwd=ROOT, env=env, stdout=open(log, "w"), stderr=subprocess.STDOUT,
                                 stdin=subprocess.DEVNULL, start_new_session=True)
        procs.append(brain)
        m = wait_for(lambda: re.search(r"listening on http://127\.0\.0\.1:(\d+) \(ws", read(log)), 20)
        check("kn_brain starts on an ephemeral port", m is not None, read(log)[-300:])
        if m is None:
            return 1
        try:
            run_all(int(m.group(1)), db["path"])
        except Exception as e:  # noqa: BLE001 - a crashed probe is a failed run, not a crash of the test
            check("the probes complete", False, repr(e)[:200])
    finally:
        for p in procs:
            stop(p)
        subprocess.run([sys.executable, "-I", os.path.join(HERE, "db_scratch.py"), ROOT, "drop", "sqlite", NAME],
                       capture_output=True)
    ok = bool(RESULTS) and all(RESULTS)
    print("v10_api_market_test: %d PASS, %d FAIL" % (sum(RESULTS), len(RESULTS) - sum(RESULTS)))
    if ok:
        shutil.rmtree(work, ignore_errors=True)
    else:
        print("logs kept in " + work)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
