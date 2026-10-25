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
# a black-market item with a v10.0.3 title (Shuriken: stats "tmk" >= 10000, 10000 funds, title 91; deob-10.0.3.js:152803)
BM_TITLE, BM_TITLE_ID = 1866, 91
JUNK_ITEM = 114          # Flame Tamer: Contraband, 5 Junk per quick-sold unit (deob-10.0.3.js:116317, 122619-122632)
SELLER = "mkt_seller"
BUYER = "mkt_buyer"
PAINTER = "mkt_painter"
PAINT_OTHER = "mkt_paint_oth"  # at most 14 characters: a new name's cap (v10.0.3 maxNameLength, deob-10.0.3.js:22725)
TRADE_A = "mkt_pt_a"
TRADE_B = "mkt_pt_b"
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


def expect_not_found(label, status, reply, code="NOT_FOUND"):
    expect_error(label, status, reply, code, 404)


def expect_text_error(label, status, reply, http_status, message):
    """{"data": null, "error": "<text>"}: a message with no v10.0.3 code, which its translator shows as it is
    (deob-10.0.3.js:117409); an unknown code would show "An unexpected error occurred." """
    ok = status == http_status and isinstance(reply, dict) and reply.get("error") == message and reply.get("data") is None
    check("%s -> %d text error %r" % (label, http_status, message), ok, "HTTP %d %s" % (status, json.dumps(reply)[:200]))


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
         ('{"n": 2000, "tmk": 10000}', b_id)),
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
    expect_text_error("132 POST /market/list (price 0)", st, r, 400, "Invalid price")
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
    expect_error("133 POST /market/purchase (own listing; v10.0.3's code, deob-10.0.3.js:117343)", st, r, "MARKET_SELF_PURCHASE", 400,
                 "Cant buy your own item")
    st, r = http(port, "POST", "/market/purchase", {"market_id": NO_ITEM}, b_tok)
    expect_error("133 POST /market/purchase (unknown listing; v10.0.3's code, deob-10.0.3.js:117342)", st, r, "MARKET_LISTING_GONE", 400,
                 "Item Unavailable")
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
    # "Cant trade with yourself" has no v10.0.3 code: a text error, shown as it is (deob-10.0.3.js:117409)
    check("152 POST /trades/create (to yourself) -> 400 text error", st == 400 and r.get("error") == "Cant trade with yourself",
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
    # a stack row carries st 1: v10.0.3's trade picker offers a stack only when st === 1, else one copy by its sid
    # (deob-10.0.3.js:125012, 124726, 125069-125075)
    expect_data("156 GET /trades/inventory/{id}: the stacks carry st 1", st, r,
                lambda d: isinstance(d, dict) and all(it.get("st") == 1 for it in d.get("their_inventory") or [] if "sid" not in it)
                and any(it.get("i") == ITEM and it.get("st") == 1 for it in d.get("their_inventory") or []), "st 1 on every stack row")
    st, r = http(port, "GET", "/trades/inventory/%s" % BUYER, None, s_tok)
    expect_data("156 GET /trades/inventory/{name} (seller reads the buyer)", st, r,
                lambda d: isinstance(d, dict) and isinstance(d.get("their_inventory"), list), "{my_inventory, their_inventory}")
    st, r = http(port, "GET", "/trades/player-search?name=%s" % BUYER, None, s_tok)
    expect_data("157 GET /trades/player-search (buyer)", st, r,
                lambda d: isinstance(d, dict) and d.get("player_id") == b_id and d.get("player_name") == BUYER
                and "player_score" in d, "{player_id, player_name, player_score} (v10.js 108349)")
    st, r = http(port, "GET", "/trades/player-search?name=nobody_here_x", None, s_tok)
    expect_not_found("157 GET /trades/player-search (unknown; deob-10.0.3.js:117356)", st, r, "TRADE_PLAYER_NOT_FOUND")

    st, r = http(port, "POST", "/players/%s/gift" % BUYER, {"amount": 50, "message": "hi"}, s_tok)
    expect_data("172 POST /players/{name}/gift (50 KR)", st, r,
                lambda d: isinstance(d, dict) and d.get("amount") == 50 and is_int(d.get("funds")),
                "{funds, amount, fee} (v10.0.3 5160; v10.js 109361)")
    st, r = http(port, "POST", "/players/%s/gift" % BUYER, {"amount": 0, "message": ""}, s_tok)
    expect_text_error("172 POST /players/{name}/gift (amount 0)", st, r, 400, "Invalid amount")
    st, r = http(port, "POST", "/players/%s/verify" % SELLER, {"featured": True}, b_tok)
    # an error, not 200 null: v10.0.3 would flip the badge on a null (deob-10.0.3.js:125561-125570)
    expect_text_error("169 POST /players/{name}/verify (not a developer: no change)", st, r, 403, "Access Denied")

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
    # item_id: one item's listings (the item sidebar, deob-10.0.3.js:30822-30823, 148938); the card reads listed_time
    # and seller_id (123698-123701, 151762-151768)
    st, r = http(port, "GET", "/market/listings?item_id=%d" % ITEM_B)
    expect_data("126 GET /market/listings?item_id= (skin %d only, listed_time, seller_id)" % ITEM_B, st, r,
                lambda d: isinstance(d, dict) and d.get("listings") and all(x.get("skinindex") == ITEM_B for x in d["listings"])
                and all(is_int(x.get("listed_time")) and x["listed_time"] > 0 and x.get("seller_id") == s_id for x in d["listings"]),
                "{listings: [{skinindex %d, listed_time, seller_id}]}" % ITEM_B)
    st, r = http(port, "GET", "/market/listings?item_id=%d" % ITEM)
    expect_data("126 GET /market/listings?item_id= (sold skin %d: none open, not every item's)" % ITEM, st, r,
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
    # paging: every owned item is a row (no 50-row cut before the filters), sorted as the grid sorts (old: store
    # index), one page after another by next_cursor (deob-10.0.3.js:30873-30900, 150895-150918, 151236-151262)
    st, r = http(port, "GET", "/market/stats?sort=old&limit=1")
    p1 = data_of(r) or {}
    st2, r2 = http(port, "GET", "/market/stats?sort=old&limit=1&cursor=%s" % p1.get("next_cursor"))
    p2 = data_of(r2) or {}
    st3, r3 = http(port, "GET", "/market/stats?sort=old&limit=100")
    every = [x.get("ski") for x in (data_of(r3) or {}).get("stats", [])]
    check("129 GET /market/stats paged: limit 1 gives one row, has_more and next_cursor; the next page follows; sort old is the store order",
          st == 200 and st2 == 200 and len(p1.get("stats", [])) == 1 and p1.get("has_more") is True and p1.get("next_cursor") == 1
          and len(every) >= 2 and every == sorted(every) and [x.get("ski") for x in p1["stats"] + p2.get("stats", [])] == every[:2]
          and (data_of(r3) or {}).get("has_more") is False and (data_of(r3) or {}).get("next_cursor") is None,
          "p1 %s p2 %s every %s" % (json.dumps(p1)[:160], json.dumps(p2)[:120], every))
    # the owners rows' creator badge: cr from players.creator_badge (deob-10.0.3.js:151912, 123737)
    seed(path, [("UPDATE players SET creator_badge = 1 WHERE player_id = ?", (b_id,))])
    st, r = http(port, "GET", "/market/items/%d/sales" % ITEM)
    expect_data("130 GET /market/items/{id}/sales", st, r,
                lambda d: isinstance(d, dict) and d.get("ind") == ITEM and is_int(d.get("onS")) and any(
                    o.get("nm") == BUYER and o.get("cr") == 1 for o in d.get("owners") or []) and all(
                    o.get("cr") == 0 for o in d.get("owners") or [] if o.get("nm") != BUYER),
                "{ind, onS, inC, high, low, owners: [{nm, ct, su, ft, cr}]}: the creator's row cr 1, the others 0 (v10.js 26887)")
    st, r = http(port, "GET", "/market/items/%d/sales" % NO_ITEM)
    expect_not_found("130 GET /market/items/{id}/sales (unknown; deob-10.0.3.js:117384)", st, r, "ITEMS_INVALID_ID")
    st, r = http(port, "GET", "/market/items/%d/sales-statistics?daysback=7" % ITEM)
    expect_data("131 GET /market/items/{id}/sales-statistics", st, r,
                lambda d: isinstance(d, list) and sum(int(x.get("t", 0)) for x in d) >= 1 and all(
                    "d" in x and "f" in x and "t" in x for x in d), "[{d, f, t}] (v10.0.3 5160; v10.js 134931)")
    st, r = http(port, "GET", "/market/sales", None, s_tok)
    expect_data("128 GET /market/sales (seller's open listings)", st, r,
                lambda d: isinstance(d, list) and any(x.get("marketid") == listing_b and x.get("skinindex") == ITEM_B
                                                      and x.get("seller_id") == s_id and is_int(x.get("listed_time")) for x in d),
                "[{marketid, skinindex, funds, listed_time, seller_id}] (the card reads listed_time and seller_id, deob-10.0.3.js:146404-146473)")
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
    expect_text_error("138 POST /market/black-market/purchase (not a black-market item)", st, r, 400, "Item Unavailable")
    st, r = http(port, "POST", "/market/black-market/purchase", {"item_index": BM_BUY}, b_tok)
    expect_data("138 POST /market/black-market/purchase (stats requirement met)", st, r,
                lambda d: isinstance(d, dict) and d.get("item") == BM_BUY and d.get("kr") == 3000 and is_int(d.get("funds"))
                and "title" not in d, "{item, kr, funds} (v10.0.3 5160; v10.js 136215)")
    st, r = http(port, "POST", "/market/black-market/purchase", {"item_index": BM_TITLE}, b_tok)
    expect_data("138 POST /market/black-market/purchase (an item with a title)", st, r,
                lambda d: isinstance(d, dict) and d.get("item") == BM_TITLE and d.get("title") == BM_TITLE_ID,
                "{item, kr, funds, title} (deob-10.0.3.js:152803-152806)")
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
                and isinstance(d.get("listings"), list) and d.get("stackable") is True and d.get("wearable") is False,
                "{stats, owners, sales, listings, stackable true (no Mints tab: 10.0.3 adds it for stackable === false, deob-10.0.3.js:123271), wearable}")
    st, r = http(port, "GET", "/items/%d/view" % NO_ITEM)
    expect_not_found("140 GET /items/{id}/view (unknown)", st, r, "ITEMS_INVALID_ID")
    st, r = http(port, "GET", "/items/%d/sales?range=0" % ITEM)
    expect_data("141 GET /items/{id}/sales?range=0", st, r,
                lambda d: isinstance(d, list) and sum(int(x.get("t", 0)) for x in d) >= 1, "[{d, f, t}] (v10.js 129914)")
    st, r = http(port, "GET", "/items/%d/instances?sort=mint" % ITEM)
    expect_data("142 GET /items/{id}/instances?sort=mint (no per-copy instances)", st, r, lambda d: d == [],
                "[] (v10.0.3 5160; v10.js 128573)")
    st, r = http(port, "POST", "/items/%d/paint" % ITEM, {"item_id": ITEM, "paint_item_id": 2}, s_tok)
    expect_error("143 POST /items/{id}/paint (not a paint)", st, r, "ITEM_PAINT_INVALID", 400, "That item is not a paint.")
    st, r = http(port, "POST", "/items/%d/style" % ITEM, {"item_id": ITEM, "style": 1}, s_tok)
    expect_refusal("144 POST /items/{id}/style", st, r)

    # ---- quick sell (after the trades: the seller holds one skin 0 again)
    st, r = http(port, "POST", "/market/quick-sell", {"item_id": ITEM, "action": 1, "amount": 0}, s_tok)
    expect_error("135 POST /market/quick-sell (Junk for an Uncommon: no scrap value)", st, r,
                 "MARKET_QUICK_SELL_NOT_SELLABLE", 400)
    # Get Junk (action 1): 2 x 5.00 Junk into the wallet, reason quicksell; data.junk is the balance (deob-10.0.3.js:147736)
    seed(path, [("INSERT INTO inventory (account_id, skin_id, count) VALUES (?, ?, 2)", (s_id, JUNK_ITEM)),
                ("UPDATE players SET player_junk = 25 WHERE player_id = ?", (s_id,)),
                ("INSERT INTO junk_transactions (account_id, amount, source) VALUES (?, 25, 'test')", (s_id,))])
    kr0 = query(path, "SELECT player_funds FROM players WHERE player_id = ?", (s_id,))[0][0]
    st, r = http(port, "POST", "/market/quick-sell", {"item_id": JUNK_ITEM, "action": 1, "amount": 2}, s_tok)
    expect_data("135 POST /market/quick-sell (Junk, all)", st, r,
                lambda d: isinstance(d, dict) and d.get("count") == 0 and abs(d.get("junk", 0) - 10.25) < 1e-9
                and d.get("funds") == kr0 and d.get("kr") == 0 and abs(d.get("jnk", 0) - 10.0) < 1e-9,
                "{funds unchanged, junk 10.25, count 0, jnk 10}")
    left = query(path, "SELECT count FROM inventory WHERE account_id = ? AND skin_id = ?", (s_id, JUNK_ITEM))
    bal = query(path, "SELECT player_junk FROM players WHERE player_id = ?", (s_id,))[0][0]
    rows = query(path, "SELECT amount, source FROM junk_transactions WHERE account_id = ? ORDER BY id", (s_id,))
    check("135 the Junk quick sell: both units gone, 1025 hundredths, one quicksell ledger row of 1000 (the sum is the balance)",
          (not left or left[0][0] == 0) and bal == 1025 and rows[-1] == (1000, "quicksell") and sum(a for a, _ in rows) == bal,
          "left %r balance %r rows %r" % (left, bal, rows))
    st, r = http(port, "POST", "/market/quick-sell", {"item_id": JUNK_ITEM, "action": 1, "amount": 0}, s_tok)
    check("135 the Junk quick sell of an item no longer owned is refused, nothing credited",
          st == 400 and query(path, "SELECT player_junk FROM players WHERE player_id = ?", (s_id,))[0][0] == 1025,
          "HTTP %d %s" % (st, json.dumps(r)[:160]))
    st, r = http(port, "POST", "/market/quick-sell", {"item_id": ITEM, "action": 0, "amount": 0}, s_tok)
    expect_data("135 POST /market/quick-sell (KR, one)", st, r,
                lambda d: isinstance(d, dict) and d.get("count") == 0 and is_int(d.get("funds")),
                "{funds, junk, count} (v10.0.3 5160; v10.js 131580)")
    st, r = http(port, "POST", "/market/quick-sell", {"item_id": ITEM_B, "action": 2, "amount": 2}, s_tok)
    expect_data("135 POST /market/quick-sell (destroy, all)", st, r,
                lambda d: isinstance(d, dict) and d.get("count") == 0, "{count: 0}")


PAINT_RED = 7776         # a colour paint, #dc3c3c
PAINT_SHADER = 7795      # a shader paint, shader 0
PAINT_SHADER_B = 7796    # a shader paint, shader 1


def paint_scenario(port, path):
    """POST /items/{id}/paint (v10.0.3's setPaint) and the painted copies on GET /market/inventory
    (doc/v10/paint-copy.md): a paint is set on the copy it names, and on no other copy of the skin."""
    p_tok = register(port, PAINTER)
    o_tok = register(port, PAINT_OTHER)
    if not p_tok or not o_tok:
        return
    p_id, o_id = account_id(path, PAINTER), account_id(path, PAINT_OTHER)
    seed(path, [
        ("UPDATE players SET player_funds = 1000 WHERE player_id = ?", (p_id,)),
        # three units of skin 0, and the paints: two red, two shader 0, one shader 1 (the caller)
        ("INSERT INTO inventory (account_id, skin_id, count) VALUES (?, ?, 3)", (p_id, ITEM)),
        ("INSERT INTO inventory (account_id, skin_id, count) VALUES (?, ?, 2)", (p_id, PAINT_RED)),
        ("INSERT INTO inventory (account_id, skin_id, count) VALUES (?, ?, 2)", (p_id, PAINT_SHADER)),
        ("INSERT INTO inventory (account_id, skin_id, count) VALUES (?, ?, 1)", (p_id, PAINT_SHADER_B)),
        # the other account owns one unit of skin 0 and one shader 0 (its own copy is never the caller's)
        ("INSERT INTO inventory (account_id, skin_id, count) VALUES (?, ?, 1)", (o_id, ITEM)),
        ("INSERT INTO inventory (account_id, skin_id, count) VALUES (?, ?, 1)", (o_id, PAINT_SHADER)),
    ])

    def owned_rows():
        st, r = http(port, "GET", "/market/inventory", None, p_tok)
        return st, data_of(r) or []

    def copy_row(rows, copy_id):
        return next((x for x in rows if x.get("sid") == copy_id), None)

    def stack_row(rows):
        return next((x for x in rows if x.get("i") == ITEM and "m" not in x and x.get("sid") == ITEM), None)

    # the refusals first: a non-paint, an unknown item, a guest, and a paint the caller does not own
    st, r = http(port, "POST", "/items/%d/paint" % ITEM, {"item_id": ITEM, "paint_item_id": ITEM_B}, p_tok)
    expect_error("paint: a non-paint is refused", st, r, "ITEM_PAINT_INVALID", 400, "That item is not a paint.")
    st, r = http(port, "POST", "/items/%d/paint" % NO_ITEM, {"item_id": NO_ITEM, "paint_item_id": PAINT_RED}, p_tok)
    expect_not_found("paint: an unknown item", st, r, "ITEMS_INVALID_ID")
    st, r = http(port, "POST", "/items/%d/paint" % ITEM, {"item_id": ITEM, "paint_item_id": PAINT_RED})
    check("paint: a guest is refused (401)", st == 401, "HTTP %d" % st)

    # 1. an unpainted unit becomes a copy, painted red: the reply names the copy and its m
    st, r = http(port, "POST", "/items/%d/paint" % ITEM, {"item_id": ITEM, "paint_item_id": PAINT_RED}, p_tok)
    d = data_of(r) or {}
    copy_a = d.get("copy_id")
    check("paint: an unpainted unit becomes a copy painted red (copy id, m, paints left)",
          st == 200 and is_int(copy_a) and copy_a >= 1000000 and d.get("m") == {"paint": "#dc3c3c"}
          and d.get("count") == 1 and d.get("item_id") == ITEM, "HTTP %d %s" % (st, json.dumps(r)[:200]))
    st, rows = owned_rows()
    row = copy_row(rows, copy_a)
    stack = stack_row(rows)
    check("market inventory: the painted copy is its own row, painted red, with no count",
          row is not None and row.get("i") == ITEM and row.get("m") == {"paint": "#dc3c3c"} and "cnt" not in row,
          json.dumps(rows)[:300])
    check("market inventory: the other copies of the skin keep no paint (the stack has 2)",
          stack is not None and stack.get("cnt") == 2 and "m" not in stack, json.dumps(rows)[:300])

    # 2. another unit becomes a second copy, painted shader 0
    st, r = http(port, "POST", "/items/%d/paint" % ITEM, {"item_id": ITEM, "paint_item_id": PAINT_SHADER}, p_tok)
    copy_b = (data_of(r) or {}).get("copy_id")
    check("paint: a second unit becomes a copy painted with shader 0",
          st == 200 and is_int(copy_b) and copy_b != copy_a and data_of(r).get("m") == {"paintShader": 0},
          "HTTP %d %s" % (st, json.dumps(r)[:200]))

    # 3. repaint the first copy (a shader 1): only that copy changes
    st, r = http(port, "POST", "/items/%d/paint" % copy_a, {"item_id": ITEM, "paint_item_id": PAINT_SHADER_B}, p_tok)
    check("paint: a copy is repainted in place (same copy id)",
          st == 200 and data_of(r).get("copy_id") == copy_a and data_of(r).get("m") == {"paintShader": 1},
          "HTTP %d %s" % (st, json.dumps(r)[:200]))
    st, rows = owned_rows()
    row_a, row_b = copy_row(rows, copy_a), copy_row(rows, copy_b)
    check("a paint set on one copy is returned for that copy and not for another copy of the same skin",
          row_a is not None and row_a.get("m") == {"paintShader": 1}
          and row_b is not None and row_b.get("m") == {"paintShader": 0}
          and stack_row(rows) is not None and stack_row(rows).get("cnt") == 1,
          json.dumps(rows)[:400])

    # 4. the last unpainted unit is painted; no unpainted unit is left for another paint
    st, r = http(port, "POST", "/items/%d/paint" % ITEM, {"item_id": ITEM, "paint_item_id": PAINT_SHADER}, p_tok)
    check("paint: the last unpainted unit is painted", st == 200 and is_int((data_of(r) or {}).get("copy_id")), "HTTP %d" % st)
    st, r = http(port, "POST", "/items/%d/paint" % ITEM, {"item_id": ITEM, "paint_item_id": PAINT_RED}, p_tok)
    check("paint: no unpainted unit left is refused (400)",
          st == 400 and "unpainted" in str(err_of(r).get("message", "")), "HTTP %d %s" % (st, json.dumps(r)[:200]))
    st, rows = owned_rows()
    check("the three painted copies are the caller's three units (no stack row left)",
          stack_row(rows) is None and len([x for x in rows if x.get("i") == ITEM and "sid" in x and "m" in x]) == 3,
          json.dumps(rows)[:400])

    # 4b. the stack / copy flag: v10.0.3 reads st === 1 for a stack and st: 0 on a painted copy (official addSkin /
    # setSkin write st: 1 and st: 0, deob-10.0.3.js:129099-129153; the trade and item-card code tests it,
    # deob-10.0.3.js:124726, 125012). The market rows and the owned list (account data slot 10) carry it.
    st, rows = owned_rows()
    stack_rows = [x for x in rows if "m" not in x]
    check("market inventory: a stack row has st 1 and a painted copy has st 0",
          stack_rows and all(x.get("st") == 1 for x in stack_rows)
          and copy_row(rows, copy_a) is not None and copy_row(rows, copy_a).get("st") == 0, json.dumps(rows)[:400])

    # 5. a copy is the caller's: another account's paint of it is refused, and the copy keeps its paint
    st, r = http(port, "POST", "/items/%d/paint" % copy_b, {"item_id": ITEM, "paint_item_id": PAINT_SHADER}, o_tok)
    check("paint: another account cannot paint the caller's copy (400)",
          st == 400 and err_of(r).get("message") == "You don't own that copy.", "HTTP %d %s" % (st, json.dumps(r)[:200]))
    st, rows = owned_rows()
    check("the copy keeps its paint after the refusal", copy_row(rows, copy_b) is not None
          and copy_row(rows, copy_b).get("m") == {"paintShader": 0}, json.dumps(rows)[:300])

    # 6. painted units are not sold through the stack: the destroy-all of the skin takes none
    st, r = http(port, "POST", "/market/quick-sell", {"item_id": ITEM, "action": 2, "amount": 3}, p_tok)
    left = query(path, "SELECT count FROM inventory WHERE account_id = ? AND skin_id = ?", (p_id, ITEM))
    check("quick sell takes no painted unit (the three units stay)", st != 200 and left and left[0][0] == 3,
          "HTTP %d %s" % (st, json.dumps(r)[:200]))


def paint_trade_scenario(port, path):
    """Painted copies in the market, trades and quick sell (doc/v10/paint-copy.md): a copy keeps its paint through a
    listing (and its unlist, a buy), a trade offer (sent, accepted, cancelled), and a quick sell; a stack sells and
    lists only its unpainted units. The copy ids are the caller's own (the owned list's id)."""
    a_tok = register(port, TRADE_A)
    b_tok = register(port, TRADE_B)
    if not a_tok or not b_tok:
        return
    a_id, b_id = account_id(path, TRADE_A), account_id(path, TRADE_B)
    seed(path, [
        ("UPDATE players SET player_score = 200000, player_funds = 1000 WHERE player_id = ?", (a_id,)),
        ("UPDATE players SET player_score = 200000, player_funds = 1000 WHERE player_id = ?", (b_id,)),
        # A: three units of ITEM (a red copy, a shader 0 copy, one unpainted) and two unpainted units of ITEM_B
        ("INSERT INTO inventory (account_id, skin_id, count) VALUES (?, ?, 3)", (a_id, ITEM)),
        ("INSERT INTO inventory (account_id, skin_id, count) VALUES (?, ?, 2)", (a_id, ITEM_B)),
        ("INSERT INTO skin_copies (account_id, skin_id, paint_color, paint_shader) VALUES (?, ?, '#dc3c3c', NULL)", (a_id, ITEM)),
        ("INSERT INTO skin_copies (account_id, skin_id, paint_color, paint_shader) VALUES (?, ?, NULL, 0)", (a_id, ITEM)),
        # B: two units of ITEM (a shader 1 copy and one unpainted), one unpainted unit of ITEM_B
        ("INSERT INTO inventory (account_id, skin_id, count) VALUES (?, ?, 2)", (b_id, ITEM)),
        ("INSERT INTO skin_copies (account_id, skin_id, paint_color, paint_shader) VALUES (?, ?, NULL, 1)", (b_id, ITEM)),
        ("INSERT INTO inventory (account_id, skin_id, count) VALUES (?, ?, 1)", (b_id, ITEM_B)),
    ])

    def copies(aid):
        """the account's painted copies: copy id -> (skin, paint_color, paint_shader)"""
        return {r[0]: (r[1], r[2], r[3]) for r in query(
            path, "SELECT copy_id, skin_id, paint_color, paint_shader FROM skin_copies WHERE account_id = ?", (aid,))}

    def count(aid, skin):
        rows = query(path, "SELECT count FROM inventory WHERE account_id = ? AND skin_id = ?", (aid, skin))
        return rows[0][0] if rows else 0

    def copy_of(aid, paint):
        """the copy id of the account's copy with that paint (a colour, or ("shader", n))"""
        for cid, (skin, color, shader) in copies(aid).items():
            if (paint[0] == "color" and color == paint[1]) or (paint[0] == "shader" and shader == paint[1]):
                return cid
        return 0

    def listing_of(lid):
        return query(path, "SELECT skin_id, paint_color, paint_shader, sold_at IS NULL AND cancelled_at IS NULL FROM market_listings WHERE id = ?", (lid,))

    red_a = copy_of(a_id, ("color", "#dc3c3c"))
    shader0_a = copy_of(a_id, ("shader", 0))
    seed_ok = bool(red_a and shader0_a)
    check("paint trade: the seeded copies exist (A: red, shader 0)", seed_ok, "%r %r" % (red_a, shader0_a))
    if not seed_ok:
        return

    # 1. a painted copy is listed by its copy id (skin_id): its paint moves to the listing, the copy leaves the owner
    st, r = http(port, "POST", "/market/list", {"item_id": ITEM, "skin_id": red_a, "price": 300}, a_tok)
    d = data_of(r) or {}
    lid_red = d.get("listingId")
    check("market list: a painted copy (its copy id) is listed (listing id, unpainted units left)",
          st == 200 and is_int(lid_red) and d.get("cnt") == 1, "HTTP %d %s" % (st, json.dumps(r)[:200]))
    rows = listing_of(lid_red)
    check("market list: the listing keeps the copy's paint, and the copy is gone from the owner",
          rows and rows[0][0] == ITEM and rows[0][1] == "#dc3c3c" and rows[0][2] is None and red_a not in copies(a_id),
          "%r" % (rows,))

    st, r = http(port, "GET", "/market/sales", None, a_tok)
    mine = next((x for x in (data_of(r) or []) if x.get("marketid") == lid_red), None)
    check("market sales: the painted listing carries its paint (m), as the official card reads it",
          st == 200 and mine is not None and mine.get("m") == {"paint": "#dc3c3c"}, "HTTP %d %s" % (st, json.dumps(r)[:240]))

    # 2. the last unpainted unit is listed as a stack (its store index); no unpainted unit is left for another
    st, r = http(port, "POST", "/market/list", {"item_id": ITEM, "price": 300}, a_tok)
    check("market list: a stack lists its last unpainted unit", st == 200 and is_int((data_of(r) or {}).get("listingId")),
          "HTTP %d %s" % (st, json.dumps(r)[:200]))
    st, r = http(port, "POST", "/market/list", {"item_id": ITEM, "price": 300}, a_tok)
    check("market list: no unpainted unit left is refused (painted copies are not listed as a stack)",
          st == 400 and r.get("error") and count(a_id, ITEM) == 1, "HTTP %d %s" % (st, json.dumps(r)[:200]))

    # 3. the buy: the buyer gets a copy with the paint (the old listing: a buy is refused for the grace period)
    seed(path, [("UPDATE market_listings SET created_at = ? WHERE id = ?", (OLD, lid_red))])
    st, r = http(port, "POST", "/market/purchase", {"market_id": lid_red}, b_tok)
    red_b = copy_of(b_id, ("color", "#dc3c3c"))
    check("market buy: the buyer receives the copy with its paint (a new copy of the same paint)",
          st == 200 and red_b != 0 and count(b_id, ITEM) == 3, "HTTP %d %s" % (st, json.dumps(r)[:200]))

    # 4. unlist returns the painted copy to its seller with the paint (the buyer keeps theirs)
    st, r = http(port, "POST", "/market/list", {"item_id": ITEM, "skin_id": shader0_a, "price": 300}, a_tok)
    lid_sh = (data_of(r) or {}).get("listingId")
    st2, r2 = http(port, "POST", "/market/unlist", {"market_id": lid_sh}, a_tok)
    back_a = copy_of(a_id, ("shader", 0))
    check("market unlist: a painted copy comes back to its seller with its paint (shader 0)",
          st == 200 and st2 == 200 and back_a != 0 and count(a_id, ITEM) == 1, "HTTP %d/%d" % (st, st2))

    # 5. a trade offer: A offers its shader 0 copy for B's red copy; the offer lists both copies with their paints
    st, r = http(port, "POST", "/trades/create", {
        "target_player_id": b_id, "my_offer": [{"i": ITEM, "c": 1, "iids": [back_a, back_a]}],
        "their_offer": [{"i": ITEM, "c": 1, "iids": [red_b]}], "message": ""}, a_tok)
    check("trade offer: a copy offered twice in one side is refused (400), and the copy stays the sender's",
          st == 400 and err_of(r).get("message") == "Invalid offer" and back_a in copies(a_id), "HTTP %d %s" % (st, json.dumps(r)[:200]))
    st, r = http(port, "POST", "/trades/create", {
        "target_player_id": b_id,
        "my_offer": [{"i": ITEM, "c": 1, "iids": [back_a]}],
        "their_offer": [{"i": ITEM, "c": 1, "iids": [red_b]}],
        "message": "paint"}, a_tok)
    trid = (data_of(r) or {}).get("trid")
    check("trade offer: painted copies are offered by their copy ids (iids)",
          st == 200 and is_int(trid), "HTTP %d %s" % (st, json.dumps(r)[:240]))
    check("trade offer: the sender's copy is escrowed (gone from the owner until the offer closes)",
          back_a not in copies(a_id) and count(a_id, ITEM) == 0, "%r" % (copies(a_id),))
    # the open offer is in the history's open list, unless v10.0.3's include_open=0 (deob-10.0.3.js:31078) asks without it
    st, r = http(port, "GET", "/trades/history?offset=0&limit=30&include_open=1", None, a_tok)
    check("trade history include_open=1: the open offer is listed",
          st == 200 and any(x.get("trid") == trid for x in (data_of(r) or {}).get("open", [])), json.dumps(r)[:200])
    st, r = http(port, "GET", "/trades/history?offset=0&limit=30&include_open=0", None, a_tok)
    check("trade history include_open=0: no open list (v10.0.3's query key)",
          st == 200 and (data_of(r) or {}).get("open") == [], json.dumps(r)[:200])
    st, r = http(port, "GET", "/trades/inventory/%d" % b_id, None, a_tok)
    theirs = (data_of(r) or {}).get("their_inventory") or []
    row = next((x for x in theirs if x.get("sid") == red_b), None)
    check("trade inventory: the other player's painted copy is a row by its copy id, with its paint",
          st == 200 and row is not None and row.get("i") == ITEM and row.get("m") == {"paint": "#dc3c3c"}, json.dumps(theirs)[:300])
    st, r = http(port, "POST", "/trades/%d/accept" % (trid or 0), None, b_tok)
    a_now, b_now = copies(a_id), copies(b_id)
    check("trade accept: the sender's paint arrives on the receiver's copy, and the receiver's on the sender's",
          st == 200 and any(v == (ITEM, None, 0) for v in b_now.values())
          and any(v == (ITEM, "#dc3c3c", None) for v in a_now.values()), "HTTP %d %s; A %r B %r" % (st, json.dumps(r)[:160], a_now, b_now))
    st, r = http(port, "GET", "/trades/history?offset=0&limit=30&includeOpen=true", None, a_tok)
    done = next((x for x in (data_of(r) or {}).get("completed", []) if x.get("trid") == trid), None)
    units = (done or {}).get("sksell") or []
    check("trade history: the accepted offer shows the sender's copy with its paint",
          done is not None and any(u.get("m") == {"paintShader": 0} for u in units), json.dumps(done)[:300])

    # 6. a cancelled offer returns the copy with its paint
    red_a2 = copy_of(a_id, ("color", "#dc3c3c"))
    st, r = http(port, "POST", "/trades/create", {
        "target_player_id": b_id, "my_offer": [{"i": ITEM, "c": 1, "iids": [red_a2]}], "their_offer": [], "message": ""}, a_tok)
    trid2 = (data_of(r) or {}).get("trid")
    st2, r2 = http(port, "POST", "/trades/%d/cancel" % (trid2 or 0), None, a_tok)
    check("trade cancel: the offered painted copy comes back to its owner with its paint",
          st == 200 and st2 == 200 and copy_of(a_id, ("color", "#dc3c3c")) != 0, "HTTP %d/%d %s" % (st, st2, json.dumps(r)[:160]))

    # 7. quick sell: a painted copy sells for its KR (one unit, the copy); a stack sells its unpainted units only
    red_a3 = copy_of(a_id, ("color", "#dc3c3c"))
    funds_before = query(path, "SELECT player_funds FROM players WHERE player_id = ?", (a_id,))[0][0]
    st, r = http(port, "POST", "/market/quick-sell", {"item_id": ITEM, "action": 0, "amount": 0, "skin_id": red_a3}, a_tok)
    funds_after = query(path, "SELECT player_funds FROM players WHERE player_id = ?", (a_id,))[0][0]
    check("quick sell: a painted copy sells for its KR and leaves with its paint",
          st == 200 and (data_of(r) or {}).get("sold") == 1 and funds_after > funds_before and red_a3 not in copies(a_id),
          "HTTP %d %s" % (st, json.dumps(r)[:200]))
    st, r = http(port, "POST", "/market/quick-sell", {"item_id": ITEM, "action": 0, "amount": 2}, b_tok)
    check("quick sell: a stack sells its unpainted units only (the shader 1 copy stays)",
          st == 200 and (data_of(r) or {}).get("sold") == 1 and count(b_id, ITEM) == 2
          and any(v == (ITEM, None, 1) for v in copies(b_id).values()), "HTTP %d %s" % (st, json.dumps(r)[:200]))


def run_all(port, path):
    market_scenario(port, path)
    paint_scenario(port, path)
    paint_trade_scenario(port, path)


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
                                  "--brain.id=v10-market-test-%d" % os.getpid(), "--finder.poll_interval_s=1",
                                  "--auth.register_per_hour=100000"],
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
