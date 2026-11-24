#!/usr/bin/env python3
"""The v10 skins, wild offer and map routes on kn_brain's root paths (services/brain/v10_catalog.c;
route rows 71-77, 81-83 and 89-98 of doc/v10/api-inventory.md).

  v10_api_catalog_test.py <kn_brain | build dir> [repo root]
  (CTest passes the kn_brain binary and the source root; a build dir means BUILD/services/brain/kn_brain.)

A scratch sqlite database (db_scratch.py), kn_brain --migrate-only, then kn_brain on an ephemeral port.
Accounts register over POST /auth/register (the token is the Bearer of every call). Each check is one
HTTP request and the JSON fields the v10.0.3 consumer reads; the label names the v10.0.3 wrapper line
(decoded bundle /tmp/api-full-http-ref/v10.0.3.decoded.js, wrapper L5161) and the v10.js consumer line.

  DATA      200, {"data": ..., "error": null} with the fields the consumer reads
  REFUSAL   403, {"error": {"code": "NOT_IMPLEMENTED", ...}} (a real write KrunkNative cannot do)
  NOT_FOUND 404, {"error": {"code": "NOT_FOUND" | "MAP_NOT_FOUND", ...}}
  LOGIN     401, {"error": {"code": "UNAUTHORIZED", ...}} (a write with no session)

Each section runs on its own: a section that raises records one FAIL and the next one still runs.
Exit 0 when every check passes, 1 otherwise, 77 when kn_brain or the sqlite scratch database is
unavailable (CTest's skip).
"""
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

HERE = os.path.dirname(os.path.abspath(__file__))
ARG = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
ROOT = os.path.abspath(sys.argv[2]) if len(sys.argv) > 2 else os.path.abspath(os.path.join(HERE, "..", "..", ".."))
BRAIN = os.path.join(ARG, "services", "brain", "kn_brain") if os.path.isdir(ARG) else ARG
NAME = "kn_test_v10catalog_%d" % os.getpid()
SECRET = "kn-v10-catalog-test-secret-%d" % os.getpid()
PASSWORD = "hunter22"
DAY_MS = 86400000
PREFAB_LEADERBOARD = 72  # shared/include/shared.h
RESULTS = []
DB = None  # the sqlite file of the scratch database


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
    headers = {}
    if data is not None:
        headers["Content-Type"] = "application/json"
    if token:
        headers["Authorization"] = "Bearer " + token
    req = urllib.request.Request("http://127.0.0.1:%d%s" % (port, path), data=data, method=method, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            status, raw = r.status, r.read()
    except urllib.error.HTTPError as e:
        status, raw = e.code, e.read()
    try:
        return status, json.loads(raw or b"null")
    except ValueError:
        return status, None


def dig(obj, *keys):
    """obj[k1][k2]... or None when any step is missing (a failing reply must not stop a check)."""
    for k in keys:
        if isinstance(obj, dict) and k in obj:
            obj = obj[k]
        elif isinstance(obj, list) and isinstance(k, int) and 0 <= k < len(obj):
            obj = obj[k]
        else:
            return None
    return obj


def sql(query, args=()):
    con = sqlite3.connect(DB)
    try:
        rows = con.execute(query, args).fetchall()
        con.commit()
        return rows
    finally:
        con.close()


def one(query, args=()):
    rows = sql(query, args)
    return rows[0][0] if rows else None


def error_code(body):
    return dig(body, "error", "code")


def error_message(body):
    msg = dig(body, "error", "message")
    return msg if isinstance(msg, str) else ""


def is_data(status, body, want):
    return status == 200 and isinstance(body, dict) and body.get("error") is None and body.get("data") == want


def register(port, name):
    st, body = http(port, "POST", "/auth/register", {"username": name, "password": PASSWORD})
    token = dig(body, "token")
    acc = one("SELECT id FROM accounts WHERE username = ?", (name,))
    return token, acc


def insert_map(name, creator, objects):
    data = json.dumps({"name": name, "modes": [0], "objects": objects})
    con = sqlite3.connect(DB)
    try:
        cur = con.execute("INSERT INTO published_maps (map_name, creator_id, map_data, map_info) VALUES (?, ?, ?, '{}')",
                          (name, creator, data))
        con.commit()
        return cur.lastrowid
    finally:
        con.close()


def map_json(map_id):
    raw = one("SELECT map_data FROM published_maps WHERE map_id = ?", (map_id,))
    try:
        return json.loads(raw) if raw else None
    except ValueError:
        return None


def lb_object(map_id):
    for o in dig(map_json(map_id), "objects") or []:
        if isinstance(o, dict) and o.get("i") == PREFAB_LEADERBOARD:
            return o
    return None


class Ctx:
    """the accounts and ids the sections share"""

    def __init__(self, port, stamp):
        self.port = port
        self.stamp = stamp
        self.day = int(time.time() * 1000) // DAY_MS


def catalogue_index():
    """the index of a store skin of assets/skins.json (the catalogue the store and /data/skins read), or None"""
    try:
        with open(os.path.join(ROOT, "assets", "skins.json")) as f:
            skins = json.load(f)
    except (OSError, ValueError):
        return None
    if not isinstance(skins, list) or not skins:
        return None
    entry = skins[len(skins) // 2]
    return entry.get("index") if isinstance(entry, dict) and isinstance(entry.get("index"), int) else None


def text_error(st, body, status):
    """a refusal the Skin Manager service throws as new Error(res.error) (deob-10.0.7.js:124374-124426): a text error"""
    return st == status and isinstance(body, dict) and body.get("data") is None and isinstance(body.get("error"), str) and body["error"]


def section_skins(c):
    """rows 71-77: v10.0.7's Skin Manager, the caller's custom skins (skins.c P_CUSTOM_SKIN, 0394_custom_skins); the
    full suite is brain_skins (skins_test.py), these are the HTTP replies the window's service reads"""
    p = c.port
    st, body = http(p, "GET", "/skins")
    check("[71] GET /skins signed out -> 401 UNAUTHORIZED {code, message} (the list throws res.error.message, deob-10.0.7.js:124361)",
          st == 401 and error_code(body) == "UNAUTHORIZED" and error_message(body), "HTTP %d %s" % (st, json.dumps(body)[:160]))
    st, body = http(p, "GET", "/skins?page=1&page_size=10", token=c.tok_a)
    check("[71] GET /skins signed in, none yet -> 200 {skins: [], total: 0} (consumer deob-10.0.7.js:124359 returns data: {skins, total})",
          is_data(st, body, {"skins": [], "total": 0, "page": 1, "page_size": 10}), "HTTP %d %s" % (st, json.dumps(body)[:160]))
    st, body = http(p, "POST", "/skins", {"skinId": None, "name": "New Skin", "type": 0, "weapon": 1, "rarity": 0, "itemConfig": {}}, token=c.tok_a)
    skin = dig(body, "data", "skin") or {}
    sid = skin.get("skinId")
    check("[73] POST /skins -> 200 {skin} with its skinId (consumer deob-10.0.7.js:124385 reads data.skin)",
          st == 200 and isinstance(sid, int) and skin.get("name") == "New Skin" and skin.get("weapon") == 1 and skin.get("itemConfig") == {},
          "HTTP %d %s" % (st, json.dumps(body)[:160]))
    st, body = http(p, "POST", "/skins", {"name": ""}, token=c.tok_a)
    check("[73] POST /skins with an empty name -> 400, a text error", text_error(st, body, 400), "HTTP %d %s" % (st, json.dumps(body)[:160]))
    st, body = http(p, "POST", "/skins", {"name": "x"})
    check("[73] POST /skins signed out -> 401, a text error", text_error(st, body, 401), "HTTP %d %s" % (st, json.dumps(body)[:160]))
    st, body = http(p, "GET", "/skins", token=c.tok_a)
    check("[71] GET /skins lists it", st == 200 and [s.get("skinId") for s in dig(body, "data", "skins") or []] == [sid]
          and dig(body, "data", "total") == 1, "HTTP %d %s" % (st, json.dumps(body)[:160]))
    st, body = http(p, "GET", "/skins/%d" % sid, token=c.tok_a)
    check("[72] GET /skins/:id -> 200 {skin} (consumer deob-10.0.7.js:124372 reads data.skin)",
          st == 200 and (dig(body, "data", "skin") or {}).get("skinId") == sid, "HTTP %d %s" % (st, json.dumps(body)[:160]))
    st, body = http(p, "GET", "/skins/%d" % sid, token=c.tok_b)
    check("[72] GET another account's skin -> 404, a text error", text_error(st, body, 404), "HTTP %d %s" % (st, json.dumps(body)[:160]))
    # a store skin has an index in assets/skins.json (GET /data/skins) but is no Skin Manager record
    idx = catalogue_index()
    if idx is not None:
        st, body = http(p, "GET", "/skins/%d" % (idx + 1000000), token=c.tok_b)
        check("[72] GET /skins/<a store skin index + 1e6> -> 404: no custom skin has that id", text_error(st, body, 404),
              "HTTP %d %s" % (st, json.dumps(body)[:160]))
    st, body = http(p, "GET", "/skins/abc", token=c.tok_a)
    check("[72] GET /skins/abc -> 404 (no skin has that id)", text_error(st, body, 404), "HTTP %d %s" % (st, json.dumps(body)[:160]))
    st, body = http(p, "PUT", "/skins/%d" % sid, {"name": "Renamed", "rarity": 3}, token=c.tok_a)
    check("[74] PUT /skins/:id -> 200 {skin} (consumer deob-10.0.7.js:124398 reads data.skin)",
          st == 200 and (dig(body, "data", "skin") or {}).get("name") == "Renamed" and (dig(body, "data", "skin") or {}).get("rarity") == 3,
          "HTTP %d %s" % (st, json.dumps(body)[:160]))
    st, body = http(p, "POST", "/skins/%d/duplicate" % sid, {}, token=c.tok_a)
    dup = (dig(body, "data", "skin") or {}).get("skinId")
    check("[77] POST /skins/:id/duplicate -> 200 {skin} \"<name> (Copy)\" (consumer deob-10.0.7.js:124424 reads data.skin)",
          st == 200 and isinstance(dup, int) and dup != sid and (dig(body, "data", "skin") or {}).get("name") == "Renamed (Copy)",
          "HTTP %d %s" % (st, json.dumps(body)[:160]))
    st, body = http(p, "POST", "/skins/%d/upload" % sid, {"type": "thumbnail"}, token=c.tok_a)
    check("[76] POST /skins/:id/upload without its FormData file -> 400 text error \"Missing File\" (the upload itself: skins_test.py)",
          text_error(st, body, 400) and body["error"] == "Missing File", "HTTP %d %s" % (st, json.dumps(body)[:160]))
    st, body = http(p, "DELETE", "/skins/%d" % sid, token=c.tok_b)
    check("[75] DELETE another account's skin -> 404", text_error(st, body, 404), "HTTP %d %s" % (st, json.dumps(body)[:160]))
    for k in (sid, dup):
        st, body = http(p, "DELETE", "/skins/%d" % k, token=c.tok_a)
        check("[75] DELETE /skins/:id -> 200 {deleted, skinId} (consumer deob-10.0.7.js:124411 reads data)",
              is_data(st, body, {"deleted": True, "skinId": k}), "HTTP %d %s" % (st, json.dumps(body)[:160]))
    st, body = http(p, "GET", "/skins", token=c.tok_a)
    check("[71] GET /skins after the deletes -> {skins: [], total: 0}", st == 200 and dig(body, "data", "total") == 0,
          "HTTP %d %s" % (st, json.dumps(body)[:160]))


def section_wild(c):
    """rows 81-83: the wild offer (wild_offer.c's ledgers, KR only); the consumer is v10.js 126583-126704"""
    p = c.port
    st, body = http(p, "GET", "/shop/wild-offer")
    check("[81] GET /shop/wild-offer signed out -> 200 {offer: null} (consumer v10.js 126666 reads data.offer)",
          is_data(st, body, {"offer": None}), "HTTP %d %s" % (st, json.dumps(body)[:160]))
    sql("UPDATE players SET player_funds = 1000 WHERE player_id = ?", (c.a,))
    st, body = http(p, "GET", "/shop/wild-offer", token=c.tok_a)
    offer = dig(body, "data", "offer")
    check("[81] GET /shop/wild-offer signed in -> 200 with the day's offer (consumer v10.js 126666 reads data.offer)",
          st == 200 and isinstance(offer, dict), "HTTP %d %s" % (st, json.dumps(body)[:200]))
    offer = offer if isinstance(offer, dict) else {}
    oid = offer.get("id", c.day * 100)
    check("[81] the offer id is the UTC day's first (day * 100)", offer.get("id") == c.day * 100, "id %r" % offer.get("id"))
    check("[81] offer shape: status null, price_type virtual, KR prices, one skin content, refresh_cost 300",
          offer.get("status") is None and offer.get("price_type") == "virtual"
          and dig(offer, "price", "virtual", "currency") == "KR" and dig(offer, "original_price", "virtual", "currency") == "KR"
          and len(offer.get("contents") or []) == 1 and dig(offer, "contents", 0, "type") == "skin"
          and offer.get("refresh_cost") == 300, json.dumps(offer)[:200])
    st, body = http(p, "GET", "/shop/wild-offer", token=c.tok_a)
    check("[81] the same offer when asked again", dig(body, "data", "offer") == offer, "HTTP %d" % st)

    # row 83: a stale id is refused; the current one costs 300 KR and brings the next offer (consumer 126704)
    st, body = http(p, "POST", "/shop/wild-offer/refresh", {"offer_id": oid + 1}, token=c.tok_a)
    check("[83] refresh of another offer id -> 200 {success: false, message: expired} (consumer v10.js 126704 reads data.success, data.message)",
          st == 200 and dig(body, "data", "success") is False and "expired" in str(dig(body, "data", "message")),
          "HTTP %d %s" % (st, json.dumps(body)[:160]))
    funds_before = one("SELECT player_funds FROM players WHERE player_id = ?", (c.a,))
    st, body = http(p, "POST", "/shop/wild-offer/refresh", {"offer_id": oid}, token=c.tok_a)
    got = dig(body, "data") if isinstance(dig(body, "data"), dict) else {}
    nxt = got.get("offer") if isinstance(got.get("offer"), dict) else {}
    check("[83] refresh of the current offer -> 200 {success: true, refresh_cost: 300, offer: next} (consumer v10.js 126704)",
          st == 200 and got.get("success") is True and got.get("refresh_cost") == 300 and nxt.get("id") == oid + 1,
          "HTTP %d %s" % (st, json.dumps(body)[:200]))
    check("[83] the refresh charged 300 KR", one("SELECT player_funds FROM players WHERE player_id = ?", (c.a,)) == (funds_before or 0) - 300)
    st, body = http(p, "GET", "/shop/wild-offer", token=c.tok_a)
    check("[81] after the refresh the next offer is the one shown (consumer v10.js 126666)",
          st == 200 and dig(body, "data", "offer") == nxt and bool(nxt), "HTTP %d %s" % (st, json.dumps(body)[:160]))

    # row 82: a purchase; stale, signed out, too little KR, then bought once (consumer v10.js 126583)
    skin = int(dig(nxt, "contents", 0, "ref") or 0)
    price = dig(nxt, "price", "virtual", "amount") or 0
    st, body = http(p, "POST", "/shop/wild-offer/purchase", {"offer_id": oid}, token=c.tok_a)
    check("[82] purchase of a stale offer id -> 200 {success: false, message} (consumer v10.js 126583 reads data.success, data.message)",
          st == 200 and dig(body, "data", "success") is False and "expired" in str(dig(body, "data", "message")),
          "HTTP %d %s" % (st, json.dumps(body)[:160]))
    st, body = http(p, "POST", "/shop/wild-offer/purchase", {"offer_id": oid + 1})
    check("[82] purchase signed out -> 200 {success: false, message} (consumer reads data.success)",
          st == 200 and dig(body, "data", "success") is False and bool(dig(body, "data", "message")),
          "HTTP %d %s" % (st, json.dumps(body)[:160]))
    sql("UPDATE players SET player_funds = ? WHERE player_id = ?", (max(price - 1, 0), c.a))
    st, body = http(p, "POST", "/shop/wild-offer/purchase", {"offer_id": oid + 1}, token=c.tok_a)
    check("[82] purchase with too little KR -> 200 {success: false, message: Not enough KR}",
          st == 200 and dig(body, "data", "success") is False and "Not enough KR" in str(dig(body, "data", "message")),
          "HTTP %d %s" % (st, json.dumps(body)[:160]))
    check("[82] nothing charged or granted when refused",
          one("SELECT player_funds FROM players WHERE player_id = ?", (c.a,)) == max(price - 1, 0)
          and one("SELECT coalesce(sum(delta), 0) FROM skin_transactions WHERE account_id = ? AND skin_id = ?", (c.a, skin)) == 0)
    sql("UPDATE players SET player_funds = ? WHERE player_id = ?", (price + 5, c.a))
    st, body = http(p, "POST", "/shop/wild-offer/purchase", {"offer_id": oid + 1}, token=c.tok_a)
    got = dig(body, "data") if isinstance(dig(body, "data"), dict) else {}
    check("[82] purchase -> 200 {success: true, rewards: [{type: skin, skin_id}], funds: 5} (consumer v10.js 126583 reads rewards)",
          st == 200 and got.get("success") is True and got.get("rewards") == [{"type": "skin", "skin_id": skin}]
          and got.get("funds") == 5, "HTTP %d %s" % (st, json.dumps(body)[:200]))
    check("[82] the skin granted once and the price charged",
          one("SELECT coalesce(sum(delta), 0) FROM skin_transactions WHERE account_id = ? AND skin_id = ?", (c.a, skin)) == 1
          and one("SELECT player_funds FROM players WHERE player_id = ?", (c.a,)) == 5)
    st, body = http(p, "POST", "/shop/wild-offer/purchase", {"offer_id": oid + 1}, token=c.tok_a)
    check("[82] a second purchase of the same offer refused (success false)",
          st == 200 and dig(body, "data", "success") is False, "HTTP %d %s" % (st, json.dumps(body)[:160]))
    st, body = http(p, "POST", "/shop/wild-offer/purchase", {"offer_id": "x"}, token=c.tok_a)
    check("[82] a non-numeric offer id -> 200 {success: false, message: Invalid wild offer}",
          st == 200 and dig(body, "data", "success") is False, "HTTP %d %s" % (st, json.dumps(body)[:160]))
    st, body = http(p, "GET", "/shop/wild-offer", token=c.tok_a)
    check("[81] no offer after the purchase (until the next day or refresh)", st == 200 and dig(body, "data", "offer") is None,
          "HTTP %d %s" % (st, json.dumps(body)[:160]))
    st, body = http(p, "POST", "/shop/wild-offer/refresh", {"offer_id": oid + 1}, token=c.tok_a)
    check("[83] refresh of a bought offer -> 200 {success: false}", st == 200 and dig(body, "data", "success") is False,
          "HTTP %d %s" % (st, json.dumps(body)[:160]))


def section_maps(c):
    """rows 89-98: the published maps (creations.c), the map's leaderboard, votes, staff feature, the KR and data routes"""
    p = c.port
    tok_a, tok_b, tok_d, tok_m, a, b = c.tok_a, c.tok_b, c.tok_d, c.tok_m, c.a, c.b
    bname = "kncB%05d" % c.stamp
    aname = "kncA%05d" % c.stamp
    sql("UPDATE players SET player_funds = 1000 WHERE player_id = ?", (a,))  # the wild offer section spent it
    board = "Kn Test Board %05d" % c.stamp
    map1 = insert_map(board, a, [{"i": PREFAB_LEADERBOARD, "p": [0, 0, 0], "s": [1, 1, 1], "ldt": 0, "ldo": 1}])
    # the match results on the map: the score board (DESC) and the time board (ASC)
    for mid, acc, score, t in (("kn-m1-%d" % c.stamp, a, 100, 60000), ("kn-m2-%d" % c.stamp, b, 50, 45000),
                               ("kn-m3-%d" % c.stamp, b, 75, 30000)):
        sql("INSERT INTO match_results (match_id, account_id, server, region, map, mode, class_id, kills, deaths, score,"
            " won, games, time_ms, kr, stats) VALUES (?, ?, 'local', 'local', ?, 'ffa', 0, 1, 0, ?, 1, 1, ?, 0, '{}')",
            (mid, acc, board, score, t))
    path = "/maps/by-name/%s/leaderboard?page=1" % board.replace(" ", "%20")

    # row 88 GET /maps/by-name/{name} for a published map: the v10.0.3 Map Detail record
    # (deob-10.0.3.js:121106-121532: map_id, creator_name, category, featured, votes, gameplays, created_at,
    # updated_at, description, funds, leaderboard_type, leaderboard_order)
    sql("UPDATE published_maps SET map_category = 3, map_votes = 4, map_plays = 9, map_funds = 25,"
        " map_description = 'Fish & chips', map_initialdate = '2026-03-04 05:06:07.000',"
        " map_date = '2026-04-05 06:07:08.000' WHERE map_id = ?", (map1,))
    st, body = http(p, "GET", "/maps/by-name/%s" % board.lower().replace(" ", "%20"))
    check("[88] GET /maps/by-name/<published map> (any case) -> the Map Detail record (deob-10.0.3.js:121118)",
          is_data(st, body, {"map_id": map1, "name": board, "creator_name": aname, "category": 3, "featured": False,
                             "votes": 4, "gameplays": 9, "funds": 25, "description": "Fish & chips",
                             "created_at": "2026-03-04T05:06:07.000Z", "updated_at": "2026-04-05T06:07:08.000Z",
                             "leaderboard_type": "score", "leaderboard_order": 1}),
          "HTTP %d %s" % (st, json.dumps(body)[:300]))
    sql("UPDATE published_maps SET map_funds = 0 WHERE map_id = ?", (map1,))  # row 95's deposits start from 0
    st, body = http(p, "GET", "/maps/by-name/no%20such%20map%20here")
    check("[88] GET /maps/by-name/<neither official nor published> -> 404 MAP_NOT_FOUND (the panel's 'Map not found', 121141)",
          st == 404 and error_code(body) == "MAP_NOT_FOUND", "HTTP %d %s" % (st, json.dumps(body)[:160]))

    # row 89 GET /maps/by-name/{name}/leaderboard (consumer v10.js 106970 reads entries, total)
    st, body = http(p, "GET", path)
    check("[89] GET by-name leaderboard, score DESC: the best score per player (wrapper L5161; consumer v10.js 106970 reads entries, total)",
          is_data(st, body, {"entries": [{"position": 1, "player_name": aname, "value": 100},
                                         {"position": 2, "player_name": bname, "value": 75}], "total": 2}),
          "HTTP %d %s" % (st, json.dumps(body)[:240]))
    st, body = http(p, "GET", "/maps/by-name/Sandstorm/leaderboard?page=1")
    check("[89] GET by-name leaderboard of an official map (no published row): {entries: [], total: 0}",
          is_data(st, body, {"entries": [], "total": 0}), "HTTP %d %s" % (st, json.dumps(body)[:160]))

    # row 90 POST /maps/{id}/vote (wrapper L5161; no HTTP caller in v10.0.3: the in-game vote is the socket's MAP_VOTE)
    st, body = http(p, "POST", "/maps/%d/vote" % map1, {"vote": 1})
    check("[90] POST vote signed out -> 401 UNAUTHORIZED (wrapper L5161; no HTTP caller in v10.0.3)",
          st == 401 and error_code(body) == "UNAUTHORIZED", "HTTP %d %s" % (st, json.dumps(body)[:160]))
    st, body = http(p, "POST", "/maps/%d/vote" % map1, {"vote": 1}, token=tok_b)
    check("[90] POST vote +1 by a visitor -> 200 {vote: 1, map_votes: 1}", is_data(st, body, {"vote": 1, "map_votes": 1}),
          "HTTP %d %s" % (st, json.dumps(body)[:160]))
    st, body = http(p, "POST", "/maps/%d/vote" % map1, {"vote": 1}, token=tok_a)
    check("[90] POST vote +1 by the owner -> map_votes 2", is_data(st, body, {"vote": 1, "map_votes": 2}),
          "HTTP %d %s" % (st, json.dumps(body)[:160]))
    st, body = http(p, "POST", "/maps/%d/vote" % map1, {"vote": -1}, token=tok_b)
    check("[90] the visitor changes the vote to -1 (one vote each) -> map_votes 0", is_data(st, body, {"vote": -1, "map_votes": 0}),
          "HTTP %d %s" % (st, json.dumps(body)[:160]))
    st, body = http(p, "POST", "/maps/%d/vote" % map1, {"vote": 0}, token=tok_b)
    check("[90] vote 0 removes the visitor's vote -> map_votes 1", is_data(st, body, {"vote": 0, "map_votes": 1}),
          "HTTP %d %s" % (st, json.dumps(body)[:160]))
    check("[90] published_maps.map_votes stores the sum", one("SELECT map_votes FROM published_maps WHERE map_id = ?", (map1,)) == 1)
    st, body = http(p, "POST", "/maps/%d/vote" % map1, {"vote": "x"}, token=tok_b)
    check("[90] a non-numeric vote -> 400 INVALID_VOTE", st == 400 and error_code(body) == "INVALID_VOTE",
          "HTTP %d %s" % (st, json.dumps(body)[:160]))
    st, body = http(p, "POST", "/maps/999999/vote", {"vote": 1}, token=tok_b)
    check("[90] a vote on an unknown map -> 404 MAP_NOT_FOUND", st == 404 and error_code(body) == "MAP_NOT_FOUND",
          "HTTP %d %s" % (st, json.dumps(body)[:160]))

    # row 91 POST /maps/{id}/feature: developers (creations.c ftC); anyone else sees the map as not found
    st, body = http(p, "POST", "/maps/%d/feature" % map1, {"featured": True}, token=tok_b)
    check("[91] feature by a visitor (not staff) -> 404 MAP_NOT_FOUND (wrapper L5161; consumer v10.js 106766 reads error)",
          st == 404 and error_code(body) == "MAP_NOT_FOUND", "HTTP %d %s" % (st, json.dumps(body)[:160]))
    st, body = http(p, "POST", "/maps/%d/feature" % map1, {"featured": True}, token=tok_d)
    check("[91] feature by a developer -> 200 {featured: true, map_featured: 1}",
          is_data(st, body, {"featured": True, "map_featured": 1}), "HTTP %d %s" % (st, json.dumps(body)[:160]))
    st, body = http(p, "POST", "/maps/%d/feature" % map1, {"featured": False}, token=tok_d)
    check("[91] unfeature by a developer -> 200 {featured: false, map_featured: 0}",
          is_data(st, body, {"featured": False, "map_featured": 0}), "HTTP %d %s" % (st, json.dumps(body)[:160]))

    # row 93 GET /maps/{id}/versions: the managers' list (map_versions, migration 0392; deob-10.0.7.js:26713-26717,
    # the Versions tab 107794-107811). This map was written straight into the table: no version stored yet.
    st, body = http(p, "GET", "/maps/%d/versions" % map1)
    check("[93] GET versions signed out -> 401", st == 401, "HTTP %d %s" % (st, json.dumps(body)[:160]))
    st, body = http(p, "GET", "/maps/%d/versions" % map1, token=tok_b)
    check("[93] GET versions by a non-manager -> 404 MAP_NOT_FOUND (the tab is the owner's, deob-10.0.7.js:107651-107652)",
          st == 404 and error_code(body) == "MAP_NOT_FOUND", "HTTP %d %s" % (st, json.dumps(body)[:160]))
    st, body = http(p, "GET", "/maps/%d/versions" % map1, token=tok_a)
    check("[93] GET versions by the owner before any write -> 200 []", is_data(st, body, []), "HTTP %d %s" % (st, json.dumps(body)[:160]))
    # row 94 POST revert: a version that was never stored
    st, body = http(p, "POST", "/maps/%d/versions/2/revert" % map1, {}, token=tok_a)
    check("[94] POST revert of an unknown version -> 400 Version not found (deob-10.0.7.js:107823-107826 reads error.message)",
          st == 400 and "Version not found" in error_message(body), "HTTP %d %s" % (st, json.dumps(body)[:160]))

    # row 95 POST /maps/{id}/funds: the creator moves KR between the account and the map.
    # The Funds tab shows floor(10%) as "Deposit fee" (deob-10.0.7.js:107627) and sends the signed
    # amount (updateMapFunds, :107777). The fee comes out of what arrives, same as the hub's mfnds.
    # |amount| under 10 never leaves the tab (:107768-107771).
    st, body = http(p, "POST", "/maps/%d/funds" % map1, {"amount": 100}, token=tok_b)
    check("[95] funds by a non-creator -> 404 MAP_NOT_FOUND", st == 404 and error_code(body) == "MAP_NOT_FOUND",
          "HTTP %d %s" % (st, json.dumps(body)[:160]))
    st, body = http(p, "POST", "/maps/%d/funds" % map1, {"amount": 9}, token=tok_a)
    check("[95] deposit under 10 KR -> 400 Minimum is 10 KR, and nothing moves",
          st == 400 and error_message(body) == "Minimum is 10 KR"
          and one("SELECT map_funds FROM published_maps WHERE map_id = ?", (map1,)) == 0
          and one("SELECT player_funds FROM players WHERE player_id = ?", (a,)) == 1000,
          "HTTP %d %s" % (st, json.dumps(body)[:160]))
    st, body = http(p, "POST", "/maps/%d/funds" % map1, {"amount": 100}, token=tok_a)
    check("[95] deposit 100 KR -> 200 {map_funds: 90, player_funds: 900} (the account pays 100, the map gets 90)",
          is_data(st, body, {"map_funds": 90, "player_funds": 900}), "HTTP %d %s" % (st, json.dumps(body)[:160]))
    st, body = http(p, "POST", "/maps/%d/funds" % map1, {"amount": -40}, token=tok_a)
    check("[95] withdraw 40 KR -> 200 {map_funds: 50, player_funds: 936} (the map pays 40, the account gets 36)",
          is_data(st, body, {"map_funds": 50, "player_funds": 936}), "HTTP %d %s" % (st, json.dumps(body)[:160]))
    check("[95] the ledger records the fee the window shows",
          sql("SELECT amount, fee FROM kr_transactions WHERE account_id = ? AND source = 'map_funds' ORDER BY id", (a,))
          == [(-100, 10), (36, 4)])
    st, body = http(p, "POST", "/maps/%d/funds" % map1, {"amount": -1000}, token=tok_a)
    check("[95] withdraw more than the map holds -> 400 and nothing moves",
          st == 400 and "Not enough map funds" in error_message(body)
          and one("SELECT map_funds FROM published_maps WHERE map_id = ?", (map1,)) == 50, "HTTP %d %s" % (st, json.dumps(body)[:160]))
    st, body = http(p, "POST", "/maps/%d/funds" % map1, {"amount": 5000}, token=tok_a)
    check("[95] deposit more than the account holds -> 400 Not enough KR",
          st == 400 and "Not enough KR" in error_message(body), "HTTP %d %s" % (st, json.dumps(body)[:160]))

    # row 96 POST /maps/{id}/leaderboard: the Leaderboard Display (consumer v10.js 106782 reads leaderboard_type, order)
    st, body = http(p, "POST", "/maps/%d/leaderboard" % map1, {"type": 1, "custom_type": "", "order": 0}, token=tok_b)
    check("[96] leaderboard settings by a non-owner -> 404 MAP_NOT_FOUND", st == 404 and error_code(body) == "MAP_NOT_FOUND",
          "HTTP %d %s" % (st, json.dumps(body)[:160]))
    st, body = http(p, "POST", "/maps/%d/leaderboard" % map1, {"type": 1, "custom_type": "", "order": 0}, token=tok_a)
    check("[96] leaderboard Time, ASC by the owner -> 200 {leaderboard_type: \"time\", order: 0} (deob-10.0.3.js:120948 stores it, 121127 compares it with \"time\")",
          st == 200 and dig(body, "data", "leaderboard_type") == "time" and dig(body, "data", "order") == 0,
          "HTTP %d %s" % (st, json.dumps(body)[:160]))
    check("[96] the LEADERBOARD object carries ldt 1, and the zero order is left out (creations.c ulb)",
          lb_object(map1) == {"i": PREFAB_LEADERBOARD, "p": [0, 0, 0], "s": [1, 1, 1], "ldt": 1}, json.dumps(lb_object(map1)))
    check("[96] the map is republished (map_updatecounter + 1)",
          one("SELECT map_updatecounter FROM published_maps WHERE map_id = ?", (map1,)) == 2)
    st, body = http(p, "GET", path)
    check("[89] by-name leaderboard after the write: Time, the best (lowest) time per player",
          is_data(st, body, {"entries": [{"position": 1, "player_name": bname, "value": 30000},
                                         {"position": 2, "player_name": aname, "value": 60000}], "total": 2}),
          "HTTP %d %s" % (st, json.dumps(body)[:240]))
    st, body = http(p, "POST", "/maps/%d/leaderboard" % map1, {"type": 0, "custom_type": "kills", "order": 1}, token=tok_a)
    check("[96] a custom type (kills) is stored, and is the leaderboard_type",
          st == 200 and dig(body, "data", "custom_type") == "kills" and dig(body, "data", "leaderboard_type") == "kills",
          "HTTP %d %s" % (st, json.dumps(body)[:160]))
    st, body = http(p, "GET", path)
    check("[89] a custom leaderboard has no KrunkNative source: empty board", is_data(st, body, {"entries": [], "total": 0}),
          "HTTP %d %s" % (st, json.dumps(body)[:160]))
    st, body = http(p, "POST", "/maps/%d/leaderboard" % map1, {"type": 1, "custom_type": "", "order": 0}, token=tok_a)
    check("[96] back to Time, ASC (custom removed)", st == 200 and "ldc" not in (lb_object(map1) or {}), json.dumps(lb_object(map1)))

    # rows 93-94 again: the three Leaderboard Display writes above each stored a version (2, 3, 4)
    st, body = http(p, "GET", "/maps/%d/versions" % map1, token=tok_a)
    rows = dig(body, "data") or []
    live_size = len(one("SELECT map_data FROM published_maps WHERE map_id = ?", (map1,)).encode())
    check("[93] versions newest first, each {version, created, data_size, current_version} (deob-10.0.7.js:107649, 108235-108266)",
          st == 200 and [r.get("version") for r in rows] == [4, 3, 2] and all(r.get("current_version") == 4 for r in rows)
          and rows[0].get("data_size") == live_size and re.match(r"^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d", rows[0].get("created") or ""),
          "HTTP %d %s" % (st, json.dumps(body)[:300]))
    st, body = http(p, "POST", "/maps/%d/versions/3/revert" % map1, {}, token=tok_b)
    check("[94] revert by a non-manager -> 404 MAP_NOT_FOUND", st == 404 and error_code(body) == "MAP_NOT_FOUND",
          "HTTP %d %s" % (st, json.dumps(body)[:160]))
    st, body = http(p, "POST", "/maps/%d/versions/4/revert" % map1, {}, token=tok_a)
    check("[94] revert to the live version -> 400 REFUSED", st == 400 and error_code(body) == "REFUSED", "HTTP %d %s" % (st, json.dumps(body)[:160]))
    st, body = http(p, "POST", "/maps/%d/versions/3/revert" % map1, {}, token=tok_a)
    check("[94] revert to v3 -> 200 {version: 5} (\"Live version is now v{0}\" reads data.version, deob-10.0.7.js:107827)",
          is_data(st, body, {"version": 5}), "HTTP %d %s" % (st, json.dumps(body)[:160]))
    check("[94] v3's data is live again (its custom type kills), as a new version 5",
          (lb_object(map1) or {}).get("ldc") == "kills" and one("SELECT map_updatecounter FROM published_maps WHERE map_id = ?", (map1,)) == 5,
          json.dumps(lb_object(map1)))
    st, body = http(p, "GET", "/maps/%d/versions" % map1, token=tok_a)
    check("[93] the version that was live (4) stays listed: [5, 4, 3, 2], current 5",
          [r.get("version") for r in dig(body, "data") or []] == [5, 4, 3, 2] and dig(body, "data")[0].get("current_version") == 5,
          "HTTP %d %s" % (st, json.dumps(body)[:300]))
    st, body = http(p, "POST", "/maps/%d/versions/4/revert" % map1, {}, token=tok_a)
    check("[94] the revert undone (v4 back live as v6, no custom type)",
          is_data(st, body, {"version": 6}) and "ldc" not in (lb_object(map1) or {}), "HTTP %d %s" % (st, json.dumps(body)[:160]))
    for i in range(20):  # a map keeps its newest 20 versions (MAP_VERSIONS_KEEP)
        http(p, "POST", "/maps/%d/leaderboard" % map1, {"type": 1, "custom_type": "", "order": i % 2}, token=tok_a)
    kept = sql("SELECT min(version), max(version), count(*) FROM map_versions WHERE map_id = ?", (map1,))
    check("[93] the newest 20 versions are kept (7..26 after 20 more writes): %r" % (kept,), kept == [(7, 26, 20)])
    http(p, "POST", "/maps/%d/leaderboard" % map1, {"type": 1, "custom_type": "", "order": 0}, token=tok_a)
    st, body = http(p, "POST", "/maps/%d/leaderboard" % map1, {"type": 0, "custom_type": "Hello World!xyz", "order": 1}, token=tok_a)
    check("[96] a custom type is trimmed to [A-Za-z0-9_] and 10 characters (deob-10.0.7.js:107493)",
          st == 200 and dig(body, "data", "custom_type") == "HelloWorld" and dig(body, "data", "leaderboard_type") == "HelloWorld"
          and (lb_object(map1) or {}).get("ldc") == "HelloWorld",
          "HTTP %d %s" % (st, json.dumps(body)[:160]))

    # rows 97-98: the map's key/value data per player (ks_storage; consumer v10.js 106987 reads data, mk, kv)
    sql("INSERT INTO ks_storage (account_id, map_id, skey, svalue, scope) VALUES (?, ?, 'coins', '5', 0)", (b, map1))
    sql("INSERT INTO ks_storage (account_id, map_id, skey, svalue, scope) VALUES (?, ?, 'score', '9', 0)", (b, map1))
    sql("INSERT INTO ks_storage (account_id, map_id, skey, svalue, scope) VALUES (?, ?, 'coins', '7', 0)", (a, map1))
    st, body = http(p, "GET", "/maps/%d/data?player=%s" % (map1, bname), token=tok_a)
    check("[97] GET data of a player by the owner -> rows {mk: key, kv: value} (wrapper L5161; consumer v10.js 106987 reads mk, kv)",
          is_data(st, body, [{"mk": "coins", "kv": "5"}, {"mk": "score", "kv": "9"}]), "HTTP %d %s" % (st, json.dumps(body)[:200]))
    st, body = http(p, "GET", "/maps/%d/data?player=%s" % (map1, bname), token=tok_b)
    check("[97] GET data by a non-owner -> 200 [] (none of the map's data is shown)", is_data(st, body, []),
          "HTTP %d %s" % (st, json.dumps(body)[:160]))
    st, body = http(p, "GET", "/maps/%d/data?player=%s" % (map1, bname))
    check("[97] GET data signed out -> 200 []", is_data(st, body, []), "HTTP %d %s" % (st, json.dumps(body)[:160]))
    st, body = http(p, "GET", "/maps/%d/data?player=nobody%05d" % (map1, c.stamp), token=tok_a)
    check("[97] GET data of an unknown player -> 200 []", is_data(st, body, []), "HTTP %d %s" % (st, json.dumps(body)[:160]))
    st, body = http(p, "DELETE", "/maps/%d/data?key=coins&player=%s" % (map1, bname), token=tok_b)
    check("[98] DELETE data by a non-owner -> 404 MAP_NOT_FOUND", st == 404 and error_code(body) == "MAP_NOT_FOUND",
          "HTTP %d %s" % (st, json.dumps(body)[:160]))
    st, body = http(p, "DELETE", "/maps/%d/data?key=coins&player=%s" % (map1, bname), token=tok_a)
    check("[98] DELETE one key of one player by the owner -> 200 {deleted: 1} (wrapper L5161; consumer v10.js 107010 reads error)",
          is_data(st, body, {"deleted": 1}), "HTTP %d %s" % (st, json.dumps(body)[:160]))
    check("[98] only that player's key is gone (the owner's own coins stay)",
          one("SELECT count(*) FROM ks_storage WHERE map_id = ? AND account_id = ?", (map1, b)) == 1
          and one("SELECT count(*) FROM ks_storage WHERE map_id = ? AND account_id = ?", (map1, a)) == 1)
    st, body = http(p, "DELETE", "/maps/%d/data?key=coins" % map1, token=tok_a)
    check("[98] DELETE a key across every player (no player=) -> 200 {deleted: 1} (consumer v10.js 107035)",
          is_data(st, body, {"deleted": 1}), "HTTP %d %s" % (st, json.dumps(body)[:160]))
    st, body = http(p, "DELETE", "/maps/%d/data" % map1, token=tok_a)
    check("[98] DELETE with no key -> 400 INVALID_KEY", st == 400 and error_code(body) == "INVALID_KEY",
          "HTTP %d %s" % (st, json.dumps(body)[:160]))

    # row 92 DELETE /maps/{id}: the creator, developers and map mods (creations.c delC); others see not found
    map2 = insert_map("Kn Test Doomed %05d" % c.stamp, a, [])
    st, body = http(p, "DELETE", "/maps/%d" % map2, token=tok_b)
    check("[92] DELETE by a visitor -> 404 MAP_NOT_FOUND (wrapper L5161; consumer v10.js 106802 catches)",
          st == 404 and error_code(body) == "MAP_NOT_FOUND", "HTTP %d %s" % (st, json.dumps(body)[:160]))
    st, body = http(p, "DELETE", "/maps/%d" % map2, token=tok_a)
    check("[92] DELETE by the creator -> 200 {deleted: true}", is_data(st, body, {"deleted": True}),
          "HTTP %d %s" % (st, json.dumps(body)[:160]))
    check("[92] the map is soft-deleted (deleted_at set)",
          one("SELECT deleted_at IS NOT NULL FROM published_maps WHERE map_id = ?", (map2,)) == 1)
    st, body = http(p, "DELETE", "/maps/%d" % map2, token=tok_a)
    check("[92] a second DELETE of the same map -> 404 MAP_NOT_FOUND", st == 404 and error_code(body) == "MAP_NOT_FOUND",
          "HTTP %d %s" % (st, json.dumps(body)[:160]))
    map3 = insert_map("Kn Test Mod Delete %05d" % c.stamp, a, [])
    st, body = http(p, "DELETE", "/maps/%d" % map3, token=tok_m)
    check("[92] DELETE by a map mod (not the creator) -> 200 {deleted: true}", is_data(st, body, {"deleted": True}),
          "HTTP %d %s" % (st, json.dumps(body)[:160]))
    st, body = http(p, "DELETE", "/maps/%d" % map1)
    check("[92] DELETE signed out -> 401 UNAUTHORIZED", st == 401 and error_code(body) == "UNAUTHORIZED",
          "HTTP %d %s" % (st, json.dumps(body)[:160]))


def scenario(port):
    stamp = int(time.time()) % 100000
    c = Ctx(port, stamp)
    c.tok_a, c.a = register(port, "kncA%05d" % stamp)
    c.tok_b, c.b = register(port, "kncB%05d" % stamp)
    c.tok_d, c.d = register(port, "kncD%05d" % stamp)
    c.tok_m, c.m = register(port, "kncM%05d" % stamp)
    check("four accounts registered for the catalogue checks (owner, visitor, developer, map mod)",
          all((c.tok_a, c.tok_b, c.tok_d, c.tok_m)) and all((c.a, c.b, c.d, c.m)),
          "ids %r" % ((c.a, c.b, c.d, c.m),))
    if not all((c.tok_a, c.tok_b, c.tok_d, c.tok_m)):
        return
    sql("UPDATE players SET player_developer = 1 WHERE player_id = ?", (c.d,))
    sql("UPDATE players SET player_mapmod = 1 WHERE player_id = ?", (c.m,))
    sql("UPDATE players SET player_funds = 1000 WHERE player_id IN (?, ?)", (c.a, c.b))
    for section in (section_skins, section_wild, section_maps):
        try:
            section(c)
        except Exception as e:  # one section's crash is one FAIL; the rest still run
            check("%s ran to the end" % section.__name__, False, "%s: %s" % (type(e).__name__, e))


def main():
    global DB
    if not os.path.isfile(BRAIN) or not os.access(BRAIN, os.X_OK):
        print("v10_api_catalog_test: no kn_brain at %s: skipped" % BRAIN)
        return 77
    r = subprocess.run([sys.executable, "-I", os.path.join(HERE, "db_scratch.py"), ROOT, "create", "sqlite", NAME],
                       capture_output=True)
    if r.returncode:
        print("v10_api_catalog_test: no sqlite scratch database: skipped")
        return 77
    cfg = json.loads(r.stdout)
    DB = cfg["path"]
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=SECRET,
               **{"KN_DATABASE_" + k.upper(): str(v) for k, v in cfg.items()})
    work = tempfile.mkdtemp(prefix="kn-v10-catalog-test.")
    log = os.path.join(work, "brain.log")
    brain = None
    try:
        mig = subprocess.run([BRAIN, "--migrate-only"], cwd=ROOT, env=env, capture_output=True)
        check("the migrations apply", mig.returncode == 0, mig.stderr.decode(errors="replace")[-200:])
        if mig.returncode:
            return 1
        brain = subprocess.Popen([BRAIN, "--brain.host=127.0.0.1", "--brain.port=0", "--brain.region=local",
                                  "--brain.id=v10-catalog-test-%d" % os.getpid(), "--finder.poll_interval_s=1"],
                                 cwd=ROOT, env=env, stdout=open(log, "w"), stderr=subprocess.STDOUT,
                                 stdin=subprocess.DEVNULL, start_new_session=True)
        m = wait_for(lambda: re.search(r"listening on http://127\.0\.0\.1:(\d+) \(ws", read(log)), 20)
        check("kn_brain starts on an ephemeral port", m is not None, read(log)[-300:])
        if m is None:
            return 1
        scenario(int(m.group(1)))
    finally:
        if brain is not None:
            brain.terminate()
            try:
                brain.wait(timeout=5)
            except subprocess.TimeoutExpired:
                brain.kill()
        subprocess.run([sys.executable, "-I", os.path.join(HERE, "db_scratch.py"), ROOT, "drop", "sqlite", NAME],
                       capture_output=True)
    passed = sum(RESULTS)
    failed = len(RESULTS) - passed
    print("v10_api_catalog_test: %d PASS, %d FAIL" % (passed, failed))
    if failed:
        print("logs kept in " + work)
        return 1
    shutil.rmtree(work, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
