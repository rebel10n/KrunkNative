#!/usr/bin/env python3
"""The Krunker v10 REST routes answered at the brain's root paths (services/brain/turf.c, lottery.c,
shop.c, ranked.c, leaderboards.c, creations.c, push.c, public_routes.c, streams.c, guest.c, community.c,
v10_api.c). Family f5 of the v10 API inventory (doc/v10/api-inventory.md).

  v10_api_alias_test.py <kn_brain | build dir> [repo root]
  (a build dir means BUILD/services/brain/kn_brain; the repo root holds db_scratch.py.)

A scratch sqlite database (db_scratch.py), kn_brain --migrate-only, then kn_brain on an ephemeral port.
One shop product is seeded into the scratch database ("limited" display, the table starts empty), one
account is registered over POST /auth/register (its Bearer token the logged-in checks send), and three
completed market sales are seeded for it (the items/prices map is the mean per skin: 400 for skin 7, 120 for 9).
Every check is one HTTP request, one (method, path) of the v10 inventory, with the JSON shape its consumer
reads. The label names the v10.0.3 decoded line (grep-able in /tmp/api-full-http-ref/v10.0.3.decoded.js,
the gapi client table is on line 5161) and the v10.0.2 line (~/jails/kru/src/Krunker_v10.0.3/deob/v10.0.2/game.js). Stdlib only.

A route is FAIL when the brain does not route it (the 404 fallback {"error": "not_found"}), routes it with
the wrong method (405), or answers 5xx (the v10 gapi wrapper throws on any status >= 500). Login routes
pass on the v10 error shape {"error": ...} at a 4xx status when no token is sent (the gate); with a token
they are checked for the success shape the consumer reads. The v10 data routes /data/skins and /data/spins
are checked by decoding the packed text with the port of v10.js unpack in v10_api_data_test.py.

Each check prints one PASS or FAIL line; the last line is the count. Exit 0 when every check passes,
1 otherwise, 77 when kn_brain or the sqlite scratch database is unavailable (CTest's skip).
"""
import json
import os
import re
import secrets
import shutil
import signal
import sqlite3
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from v10_api_data_test import skins_as_sent, v10_unpack  # noqa: E402 - the packed-format port (its main is guarded)

ARG = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
ROOT = os.path.abspath(sys.argv[2]) if len(sys.argv) > 2 else os.path.abspath(os.path.join(HERE, "..", "..", ".."))
BRAIN = os.path.join(ARG, "services", "brain", "kn_brain") if os.path.isdir(ARG) else ARG
NAME = "kn_test_v10alias_%d" % os.getpid()
SECRET = "kn-v10alias-test-secret-%d" % os.getpid()
DRIVER = os.environ.get("KN_TEST_DB_DRIVER", "sqlite")
PASSWORD = "hunter22"
RESULTS = []


def check(name, ok, detail=""):
    RESULTS.append(bool(ok))
    print(("PASS  " if ok else "FAIL  ") + name + ("" if ok or not detail else "  -- " + detail), flush=True)


def read(path):
    try:
        with open(path, errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def load_json(rel):
    with open(os.path.join(ROOT, rel), "rb") as fp:
        return json.loads(fp.read())


def wait_for(fn, secs):
    end = time.time() + secs
    while time.time() < end:
        v = fn()
        if v:
            return v
        time.sleep(0.1)
    return None


def call_raw(port, method, path, body=None, token=None):
    """(status, content type, body bytes)"""
    data = json.dumps(body).encode() if body is not None else None
    headers = {"Accept": "application/json"}
    if data is not None:
        headers["Content-Type"] = "application/json"
    if token:
        headers["Authorization"] = "Bearer " + token
    req = urllib.request.Request("http://127.0.0.1:%d%s" % (port, path), data=data, method=method, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=15) as r:
            return r.status, r.headers.get("Content-Type", ""), r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.headers.get("Content-Type", ""), e.read()


def call(port, method, path, body=None, token=None):
    """(status, parsed JSON or None)"""
    status, _ctype, raw = call_raw(port, method, path, body, token)
    try:
        return status, json.loads(raw or b"null")
    except ValueError:
        return status, None


def unrouted(status, body):
    """the brain's 404 fallback (public_routes.c h_not_found) or a wrong method (405)"""
    return status == 405 or (status == 404 and isinstance(body, dict) and body.get("error") == "not_found")


# ---- value kinds the consumers test

KINDS = {
    "any": lambda v: True,
    "list": lambda v: isinstance(v, list),
    "object": lambda v: isinstance(v, dict),
    "int": lambda v: isinstance(v, int) and not isinstance(v, bool),
    "str": lambda v: isinstance(v, str),
    "bool": lambda v: isinstance(v, bool),
    "null_or_str": lambda v: v is None or isinstance(v, str),
}


def is_error(status, body):
    return 400 <= status < 500 and isinstance(body, dict) and "error" in body


# ---- predicates: (status, body) -> (ok, what it saw)

def p_auth_error(status, body):
    """4xx with the v10 error key: the login gate answers (no token sent)"""
    ok = is_error(status, body)
    return ok, "login gate answers %d" % status if ok else "HTTP %d, no error key" % status


def p_data(kind="any", fields=None, or_error=False):
    """200 with {data: <kind>}; with fields, data is an object whose fields have those kinds.
    or_error: a 4xx v10 error also passes (an unknown id, or a login gate with no token)."""
    def f(status, body):
        if status == 200 and isinstance(body, dict) and "data" in body:
            d = body["data"]
            if not KINDS[kind](d):
                return False, "data is not %s" % kind
            for k, kk in (fields or {}).items():
                if not isinstance(d, dict) or k not in d:
                    return False, "data lacks %s" % k
                if not KINDS[kk](d[k]):
                    return False, "data.%s is not %s" % (k, kk)
            return True, "data is %s%s" % (kind, " with " + ", ".join(fields) if fields else "")
        if or_error and is_error(status, body):
            return True, "error answer %d (gate or lookup)" % status
        return False, "HTTP %d, no data %s" % (status, kind)
    return f


def p_root(fields=None):
    """200 with a JSON object whose top-level fields have those kinds (no envelope: the legacy webhooks,
    the matchmaker, the social host)"""
    def f(status, body):
        ok = status == 200 and isinstance(body, dict)
        for k, kk in (fields or {}).items():
            ok = ok and k in body and KINDS[kk](body[k])
        return ok, "JSON object with " + (", ".join(fields) if fields else "no fields") if ok else "HTTP %d, shape" % status
    return f


def p_search(window):
    """a legacy search answer {data: [...], index}: index is the window whose searchResp takes it
    (windows[resp.index].searchResp, deob-10.0.3.js:255448-255451)"""
    def f(status, body):
        ok = status == 200 and isinstance(body, dict) and isinstance(body.get("data"), list) and body.get("index") == window
        return ok, "data list, index %d" % window if ok else "HTTP %d, %s" % (status, json.dumps(body)[:120])
    return f


def p_json(status, body):
    """a JSON answer from a routed matchmaker path: the object on 200, or the matchmaker's named refusal
    ({success: 0, error: name}, e.g. NoAvailableServers 503 when no game server is up; the menu catches it)"""
    ok = body is not None and (status == 200 or (isinstance(body, dict) and "error" in body))
    return ok, "JSON answer %d" % status if ok else "HTTP %d, no JSON" % status


def p_ok(status, body):
    """200 (a text or JSON body the consumer reads by its own rules)"""
    return status == 200, "HTTP 200" if status == 200 else "HTTP %d" % status


def p_purchase(status, body):
    """POST /ranked/purchase: data.success is a bool on success; a 4xx v10 error for a refusal"""
    d = body.get("data") if isinstance(body, dict) else None
    if status == 200 and isinstance(d, dict) and isinstance(d.get("success"), bool):
        return True, "data.success is a bool"
    # a refusal is a text error: v10.0.3 shows the top-level string error (deob-10.0.3.js:127095-127101, 127277), and
    # refetches the shop on exactly "Skin not found or not available" (127096)
    if 400 <= status < 500 and isinstance(body, dict) and body.get("error") == "Skin not found or not available":
        return True, "refusal %d, the text error v10.0.3 refetches on (no such skin in the shop)" % status
    return False, "HTTP %d %s" % (status, json.dumps(body)[:120])


def p_participate(status, body):
    """POST /lottery/:id/participate: data.player_entries on success; the v10 error otherwise"""
    d = body.get("data") if isinstance(body, dict) else None
    if status == 200 and isinstance(d, dict) and "player_entries" in d:
        return True, "data.player_entries present"
    if is_error(status, body):
        return True, "refusal %d (no such lottery or no login)" % status
    return False, "HTTP %d, no data.player_entries" % status


def p_guest(status, body):
    d = body.get("data") if isinstance(body, dict) else None
    p = d.get("profile") if isinstance(d, dict) else None
    ok = status == 200 and isinstance(p, dict) and "funds" in p and "score" in p
    return ok, "data.profile has funds, score" if ok else "HTTP %d, no data.profile" % status


def p_streams(status, body):
    """the Twitch list under data.streams; empty without a Twitch client (no 503: the menu reads the list)"""
    d = body.get("data") if isinstance(body, dict) else None
    ok = status == 200 and isinstance(d, dict) and isinstance(d.get("streams"), list)
    return ok, "data.streams is a list" if ok else "HTTP %d, no data.streams" % status


def p_player_count(status, body):
    ok = status == 200 and isinstance(body, dict) and isinstance(body.get("playerCount"), int)
    return ok, "playerCount is a number" if ok else "HTTP %d, no playerCount" % status


def p_preset(expect):
    """GET /webhooks/general/maps/config: the legacy body {da, fu}. da is the map's host preset as JSON text
    (presetLoaded parses it, v10.js 212624) and holds the map's own config; fu (funds) is 0: no server cost."""
    def f(status, body):
        if status != 200 or not isinstance(body, dict) or not isinstance(body.get("da"), str):
            return False, "HTTP %d, no da text" % status
        try:
            da = json.loads(body["da"])
        except ValueError:
            return False, "da is not JSON"
        ok = da == expect and body.get("fu") == 0
        return ok, "da is the map's config %s, fu 0" % json.dumps(expect) if ok else "da %s, fu %r" % (da, body.get("fu"))
    return f


def p_empty_object(status, body):
    """{} from the legacy webhook: no preset (the menu shows DEFAULT), for a name with no catalogue map"""
    ok = status == 200 and body == {}
    return ok, "the empty object (no preset)" if ok else "HTTP %d, %s" % (status, json.dumps(body)[:120])


def p_prices(expect):
    """GET /webhooks/general/items/prices: the price map itself, keyed by skin index (v10.js 108363-108365)"""
    def f(status, body):
        ok = status == 200 and body == expect
        return ok, "the price map %s" % json.dumps(expect) if ok else "HTTP %d, %s" % (status, json.dumps(body)[:120])
    return f


def p_shop_purchase(status, body):
    """POST /shop/purchase with a login: data.success on a purchase; the v10 error otherwise (the seeded
    product costs more Junk than the test account has)"""
    d = body.get("data") if isinstance(body, dict) else None
    if status == 200 and isinstance(d, dict) and isinstance(d.get("success"), bool):
        return True, "data.success is a bool"
    if is_error(status, body):
        return True, "refusal %d (no Junk)" % status
    return False, "HTTP %d, no data.success" % status


def push_keys():
    """a subscription's keys: base64url, unpadded, a 65-byte P-256 point and a 16-byte auth secret (push_rules.h)"""
    import base64
    def b64(n):
        return base64.urlsafe_b64encode(n).rstrip(b"=").decode()
    return {"p256dh": b64(b"\x04" + secrets.token_bytes(64)), "auth": b64(secrets.token_bytes(16))}


def official_config(stem):
    """the config block of an official map file (assets/maps/official/<stem>): the map's own modes"""
    return load_json("assets/maps/official/" + stem)["config"]


# ---- the checks: (method, path, body, predicate, cite, who); who "alice" sends alice's Bearer token

def checks(guest_id):
    return [
        # ranked.c: the root aliases of the ranked shop, purchase and connection (v10.0.3 L5161 is the gapi table)
        ("GET", "/ranked/shop", None, p_data("object", {"skins": "list"}),
         "v10.0.3 L5161 getShop; v10.js 111025-111026, def 26381", None),
        ("POST", "/ranked/purchase", {"skin_id": 1}, p_auth_error,
         "v10.0.3 L5161 purchaseSkin; v10.js 111039-111044, def 26386", None),
        ("POST", "/ranked/purchase", {"skin_id": 999999}, p_purchase,
         "v10.0.3 L5161 purchaseSkin; reads data.success, v10.js 111039-111044", "alice"),
        ("GET", "/ranked/me/connection", None, p_auth_error,
         "v10.0.3 L5161 getMyConnection; v10.js 98725-98726, def 25985", None),
        ("GET", "/ranked/me/connection", None, p_data("object", {"serverUrl": "null_or_str"}),
         "v10.0.3 L5161 getMyConnection; reads data.serverUrl, v10.js 98725", "alice"),
        # leaderboards.c
        ("GET", "/leaderboards/player_score", None, p_data("list"),
         "v10.0.3 L5161 getLeaderboard; v10.js 138453-138456, def 26995", None),
        ("GET", "/leaderboards/raids/map1", None, p_data("list"),
         "v10.0.3 L5161 getRaidLeaderboard; v10.js 138946-138948, def 27005", None),
        ("GET", "/leaderboards/raids/map1/monthly?month=2026-10", None, p_data("list"),
         "v10.0.3 L5161 getMonthlyRaidLeaderboard; v10.js 138953-138955, def 27016", None),
        ("GET", "/leaderboards/krunkcup/teams", None, p_data("list"),
         "v10.0.3 L5161 getKrunkCupTeams; v10.js 139396-139398, def 27021", None),
        ("GET", "/leaderboards/krunkcup/teams/1/contributors", None, p_data("list"),
         "v10.0.3 L5161 getKrunkCupContributors; v10.js 139378-139380, def 27026", None),
        ("GET", "/leaderboards/krunkcup/players?limit=5", None, p_data("list"),
         "v10.0.3 L5161 getKrunkCupTopPlayers; v10.js def 27032 (no bundled caller)", None),
        # turf.c: territorial wars (the consumer reads data or the error's message)
        ("GET", "/territorial-wars/status", None, p_data("any"),
         "v10.0.3 L5161 getStatus; v10.js 142468-142472, def 26673", None),
        ("GET", "/territorial-wars/clan/1", None, p_data("any", or_error=True),
         "v10.0.3 L5161 getClanStats; v10.js 141733-141737, def 26678", None),
        ("GET", "/territorial-wars/clan/1/players", None, p_data("list", or_error=True),
         "v10.0.3 L5161 getClanPlayers; v10.js 141740-141744, def 26688", None),
        ("GET", "/territorial-wars/clan/1/earnings", None, p_data("any", or_error=True),
         "v10.0.3 L5161 getClanEarnings; v10.js 141747-141749, def 26693", None),
        ("GET", "/territorial-wars/clan/1/payouts", None, p_data("any", or_error=True),
         "v10.0.3 L5161 getClanPayouts; v10.js 141754, def 26722", None),
        ("GET", "/territorial-wars/leaderboards/top-performing-clans", None, p_data("list"),
         "v10.0.3 L5161 getTopPerformingClans; v10.js 143288-143292, def 26704", None),
        ("GET", "/territorial-wars/leaderboards/top-grossing-clans", None, p_data("list"),
         "v10.0.3 L5161 getTopGrossingClans; v10.js 143454-143458, def 26713", None),
        ("GET", "/territorial-wars/leaderboards/top-performing-players", None, p_data("list"),
         "v10.0.3 L5161 getTopPerformingPlayers; v10.js 143606-143610, def 26732", None),
        ("GET", "/territorial-wars/player/1", None, p_data("object", or_error=True),
         "v10.0.3 L5161 getPlayerProfile; v10.js 141761-141764, def 26737", None),
        ("GET", "/territorial-wars/player/1/contributions", None, p_data("list", or_error=True),
         "v10.0.3 L5161 getPlayerContributions; v10.js 141768, def 26743", None),
        ("GET", "/territorial-wars/player/1/earnings", None, p_data("list", or_error=True),
         "v10.0.3 L5161 getPlayerEarnings; v10.js 141775-141780, def 26746", None),
        # turf.c: clans (loot config, recruitment, applications)
        ("GET", "/clans/recruitment", None, p_data("object", {"posts": "list", "total": "int"}),
         "v10.0.3 L5161 listRecruitment; v10.js 143826-143832, def 26803", None),
        ("GET", "/clans/1/recruitment", None, p_data("any", or_error=True),
         "v10.0.3 L5161 getRecruitment; v10.js 96683-96686, def 26806", None),
        ("POST", "/clans/1/recruitment", {"description": "x"}, p_auth_error,
         "v10.0.3 L5161 createOrUpdateRecruitment; v10.js 27661, def 26809", None),
        ("DELETE", "/clans/1/recruitment", None, p_auth_error,
         "v10.0.3 L5161 removeRecruitment; v10.js 27648, def 26815", None),
        ("GET", "/clans/my-applications", None, p_auth_error,
         "v10.0.3 L5161 getMyApplications; v10.js 143875-143877, def 26820", None),
        ("GET", "/clans/my-applications", None, p_data("object", {"clan_ids": "list"}),
         "v10.0.3 L5161 getMyApplications; reads data.clan_ids, v10.js 143875-143877", "alice"),
        ("POST", "/clans/1/apply", None, p_auth_error,
         "v10.0.3 L5161 applyToClan; v10.js 144072-144077, def 26823", None),
        ("GET", "/clans/1/loot-config", None, p_data("any", or_error=True),
         "v10.0.3 L5161 getLootConfig; v10.js 27602-27604, def 26764", None),
        ("PUT", "/clans/1/loot-config", {"clan_pct": 10}, p_auth_error,
         "v10.0.3 L5161 updateLootConfig; v10.js 96456, def 26767", None),
        # lottery.c and shop.c
        ("GET", "/lottery/1", None, p_data("any", or_error=True),
         "v10.0.3 L5161 getLottery; v10.js 118082-118083, def 26566", None),
        ("POST", "/lottery/1/participate", {"cost_type": "kr", "cost_ref": "x", "cost_quantity": 1}, p_participate,
         "v10.0.3 L5161 participateInLottery; v10.js 125504-125506, def 26571", None),
        ("GET", "/shop/displays/limited", None,
         p_data("object", {"type": "str", "children": "list", "products": "object"}),
         "v10.0.3 L5161 getDisplayByTag; v10.js 113916-113920, def 26520 (seeded product)", None),
        ("GET", "/shop/displays?tag=limited", None,
         p_data("object", {"type": "str", "children": "list", "products": "object"}),
         "v10.0.3 L5161 getDisplayByTag; v10.js def 26525 (seeded product)", None),
        ("POST", "/shop/purchase", {"product_id": 1, "quantity": 1}, p_auth_error,
         "v10.0.3 L5161 purchaseProduct; v10.js 113943-113948, def 26530", None),
        # creations.c: the legacy webhooks, read from the raw body (apiRequest, v10.js 131894-131950)
        ("GET", "/webhooks/general/maps/list?index=2&pos=0", None, p_root({"data": "list"}),
         "v10.0.3 L5161 listMaps (legacy apiRequest); v10.js 221943-221946, def 131894", None),
        ("GET", "/webhooks/general/maps/search?val=x&cat=-1", None, p_search(9),
         "searchMap (legacy apiRequest, deob-10.0.3.js:148015): index 9, the Maps window (searchResp 249327)", None),
        ("GET", "/webhooks/general/maps/config?mn=Sandstorm", None, p_root(),
         "v10.0.3 L292 maps/config; v10.js def 131914 (no preset: the empty {}, source none)", None),
        ("GET", "/webhooks/general/mods/list?index=0&pos=0", None, p_root({"data": "list"}),
         "v10.0.3 L5161 listMods (legacy apiRequest); v10.js 211797, def 131921", None),
        ("GET", "/webhooks/general/mods/search?val=x", None, p_search(3),
         "searchMod (legacy apiRequest, deob-10.0.3.js:148035): index 3, the Mods window (searchResp 247232)", None),
        ("GET", "/webhooks/general/assets/list?index=0&cat=-1", None, p_root({"data": "list"}),
         "v10.0.3 L5161 listAssets (legacy apiRequest); v10.js def 131933", None),
        ("GET", "/webhooks/general/assets/search?index=2&cat=-1&val=x", None, p_root({"data": "list"}),
         "v10.0.3 L5161 searchAsset (legacy apiRequest); v10.js def 131940", None),
        ("GET", "/webhooks/general/items/prices", None, p_root(),
         "v10.0.3 L415 getPrices (legacy apiRequest); v10.js 219262-219265, def 131947", None),
        # push.c: the two handlers this family owns (subscription status and change are f1a's)
        ("POST", "/webpush/subscribe",
         {"subscription": {"endpoint": "https://push.example/e", "keys": {"p256dh": "", "auth": ""}}},
         p_auth_error, "v10.0.3 L4093 webpush subscribe; v10.js 146596-146606, def 26396", None),
        ("POST", "/webpush/unsubscribe", {"registration_id": None, "endpoint": "https://push.example/e"},
         p_auth_error, "v10.0.3 L368 webpush unsubscribe; v10.js 146632-146636, def 26402", None),
        # v10_api.c: the official maps
        ("GET", "/maps", None, p_data("list"),
         "v10.0.3 L5161 getMaps; v10.js 26581, 222889", None),
        ("GET", "/maps/0", None, p_data("object"),
         "v10.0.3 L5161 getMap; v10.js 26586", None),
        ("GET", "/maps/by-name/Sandstorm", None, p_data("object"),
         "v10.0.3 L5161 getMapByName; v10.js 26592", None),
        # streams.c, guest.c, community.c, v10_api.c (titles)
        ("GET", "/streams/top", None, p_streams,
         "v10.0.3 L4171 /streams/top, L5161 getTopStreams; v10.js 145057-145059, 25991-25993", None),
        ("GET", "/guest/profile?guest=" + guest_id, None, p_guest,
         "v10.0.3 L117 /guest/profile, L5161 getProfile; v10.js 117249-117252, def 26372", None),
        ("GET", "/creators/events", None, p_data("object", {"events": "list", "polls": "list"}),
         "v10.0.3 L155 /creators/events, L6504 getUpcomingEvents; v10.js 196818-196820, def 27107", None),
        ("GET", "/social/news", None, p_data("object", {"news": "list"}),
         "v10.0.3 L4455 /social/news, L4675 getNews; v10.js 140724-140727, def 27285", None),
        ("GET", "/spins/wheels", None, p_data("object", {"wheels": "list", "itemPools": "object"}),
         "v10.0.3 L3836 /spins/wheels, L5161 getWheels; v10.js def 27294 (no bundled caller)", None),
        ("GET", "/titles", None, p_data("list"),
         "v10.0.3 L3989 getTitles, L5161; v10.js 27518", None),
        # the matchmaker (public_routes.c, raw JSON: the consumer reads the bare object)
        ("GET", "/ping", None, p_ok,
         "v10.0.3 L4842 '/ping'; v10.js 147377 (pingFetch resolves on 200)", None),
        ("GET", "/ping-list?hostname=localhost", None, p_root(),
         "v10.0.3 L3020 /ping-list; v10.js 147570-147573 (fetchPingList, 147478 reads it by region)", None),
        ("GET", "/game-list?hostname=localhost", None, p_root({"games": "list"}),
         "v10.0.3 L4033 /game-list; v10.js 147578-147581, 218190 reads data.games (raw)", None),
        ("GET", "/game-info?game=x&hostname=localhost", None, p_json,
         "v10.0.3 L4296 /game-info; v10.js 147586-147589, 197742 reads a 5-field tuple", None),
        ("GET", "/seek-game?region=na&hostname=localhost&autoChangeGame=true", None, p_json,
         "v10.0.3 L4838 /seek-game; v10.js 147591-147612", None),
        ("GET", "/generate-token", None, p_ok,
         "v10.0.3 L5965 /generate-token; v10.js 147556-147561 (fetchValidationToken reads text)", None),
        ("GET", "/player-count", None, p_player_count,
         "v10.0.3 L31 /player-count (social host), L6557 (editor host); v10.js 211170-211178", None),
        # creations.c: the legacy map preset and the item prices, from the catalogue and the market sales
        ("GET", "/webhooks/general/maps/config?mn=Burg", None, p_preset(official_config("00_burg.json")),
         "v10.0.3 L292 maps/config; v10.js 131917, presetLoaded 212624 (the map's config as da, fu 0)", None),
        ("GET", "/webhooks/general/maps/config?mn=NoSuchMap", None, p_empty_object,
         "v10.0.3 L292 maps/config; v10.js 131917, no catalogue map: {} (source none)", None),
        ("GET", "/webhooks/general/items/prices", None, p_prices({"7": 400, "9": 120}),
         "v10.0.3 L415 getPrices; v10.js 131947, 108363-108365 (seeded sales: the mean per skin)", None),
        # push.c: the two v10 handlers this family owns (the status and subscription-change rows are f1a's)
        ("POST", "/webpush/subscribe",
         {"subscription": {"endpoint": "https://push.example/e", "keys": push_keys()}},
         p_data("object", {"success": "bool"}),
         "v10.0.3 L4093 webpush subscribe; reads data.success, v10.js 146596-146606", "alice"),
        ("POST", "/webpush/unsubscribe", {"registration_id": None, "endpoint": "https://push.example/e"},
         p_data("object", {"success": "bool"}),
         "v10.0.3 L368 webpush unsubscribe; v10.js 146632-146636 (the response is not read)", "alice"),
        # shop.c: the purchase with a login (the seeded product is above the test account's Junk)
        ("POST", "/shop/purchase", {"product_id": 1, "quantity": 1}, p_shop_purchase,
         "v10.0.3 L5161 purchaseProduct; reads data.success, v10.js 113943-113948", "alice"),
    ]


def run_checks(port, token):
    guest_id = secrets.token_hex(16)  # a random id: not printed
    for method, path, body, pred, cite, who in checks(guest_id):
        status, data = call(port, method, path, body, token if who == "alice" else None)
        label = "%s %s  HTTP %d  [%s]" % (method, path.replace(guest_id, "<guest>"), status, cite)
        if unrouted(status, data):
            check(label, False, "not routed (%s)" % ("405 wrong method" if status == 405 else "404 fallback"))
            continue
        ok, seen = pred(status, data)
        check(label, ok, seen)


def run_raw_checks(port):
    """the packed data routes (v10.js reads responseText and unpacks it, 27906-27985)"""
    status, ctype, raw = call_raw(port, "GET", "/data/skins")
    skins_file = load_json("assets/skins.json")
    check("GET /data/skins  HTTP %d  [v10.0.3 L4171; v10.js 28035 reads responseText]" % status,
          status == 200 and ctype.startswith("text/plain"), "status %s type %s" % (status, ctype))
    try:
        decoded = v10_unpack(raw.decode("utf-8"))
        ok = decoded == skins_as_sent(skins_file)
        detail = "decoded %d rows, expected %d" % (len(decoded), len(skins_file)) if not ok else ""
    except Exception as e:  # noqa: BLE001 - a broken pack is a failed check
        ok, detail = False, repr(e)[:200]
    check("GET /data/skins decodes (v10 unpack) to assets/skins.json (video skins as stills), in order  [v10.0.3 L4171; v10.js 28035]", ok, detail)

    status, ctype, raw = call_raw(port, "GET", "/data/spins")
    jstatus, spins_json = call(port, "GET", "/api/data/spins")
    check("GET /data/spins  HTTP %d  [v10.0.3 L4381; v10.js 102764 reads responseText]" % status,
          status == 200 and ctype.startswith("text/plain"), "status %s" % status)
    try:
        spins_decoded = v10_unpack(raw.decode("utf-8"))
        ok = jstatus == 200 and spins_decoded == spins_json
        detail = "" if ok else "decoded keys %s" % (sorted(spins_decoded) if isinstance(spins_decoded, dict) else type(spins_decoded).__name__)
    except Exception as e:  # noqa: BLE001
        ok, detail = False, repr(e)[:200]
    check("GET /data/spins decodes to the wheels JSON of GET /api/data/spins  [v10.0.3 L4381; v10.js 102764]", ok, detail)


def seed_shop(db_path):
    """one Junk product on the 'limited' display (the table starts empty; the brain publishes none)"""
    con = sqlite3.connect(db_path)
    try:
        cur = con.execute("INSERT INTO shop_products (display_tag, name, price_amount, active, sort_order)"
                          " VALUES ('limited', 'Alias test pack', 500, 1, 0)")
        con.execute("INSERT INTO shop_product_contents (product_id, ord, content_type, skin_id, quantity)"
                    " VALUES (?, 0, 'skin', 1, 1)", (cur.lastrowid,))
        con.commit()
    finally:
        con.close()


def seed_sales(db_path):
    """three completed market sales by the test account: skin 7 at 300 and 500 (mean 400), skin 9 at 120"""
    con = sqlite3.connect(db_path)
    try:
        seller = con.execute("SELECT id FROM accounts ORDER BY id LIMIT 1").fetchone()[0]
        con.executemany("INSERT INTO market_listings (seller_id, skin_id, price, sold_at)"
                        " VALUES (?, ?, ?, '2026-10-02 12:00:00')", [(seller, 7, 300), (seller, 7, 500), (seller, 9, 120)])
        con.commit()
    finally:
        con.close()


def register(port, name):
    status, body = call(port, "POST", "/auth/register", {"username": name, "password": PASSWORD})
    token = body.get("token") if isinstance(body, dict) else None
    return token if status == 200 and token else None


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
        print("v10_api_alias_test: no kn_brain at %s: skipped" % BRAIN)
        return 77
    if DRIVER != "sqlite":
        print("v10_api_alias_test: runs on sqlite: skipped")
        return 77
    r = subprocess.run([sys.executable, "-I", os.path.join(HERE, "db_scratch.py"), ROOT, "create", "sqlite", NAME],
                       capture_output=True)
    if r.returncode:
        print("v10_api_alias_test: no sqlite scratch database: skipped")
        return 77
    cfg = json.loads(r.stdout)
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=SECRET,
               **{"KN_DATABASE_" + k.upper(): str(v) for k, v in cfg.items()})
    work = tempfile.mkdtemp(prefix="kn-v10alias-test.")
    log = os.path.join(work, "brain.log")
    procs = []
    try:
        mig = subprocess.run([BRAIN, "--migrate-only"], cwd=ROOT, env=env, capture_output=True)
        check("the migrations apply", mig.returncode == 0, mig.stderr.decode(errors="replace")[-200:])
        if mig.returncode:
            return 1
        seed_shop(cfg["path"])
        brain = subprocess.Popen([BRAIN, "--brain.host=127.0.0.1", "--brain.port=0", "--brain.region=local",
                                  "--brain.id=v10alias-test-%d" % os.getpid(), "--finder.poll_interval_s=1"],
                                 cwd=ROOT, env=env, stdout=open(log, "w"), stderr=subprocess.STDOUT,
                                 stdin=subprocess.DEVNULL, start_new_session=True)
        procs.append(brain)
        m = wait_for(lambda: re.search(r"listening on http://127\.0\.0\.1:(\d+) \(ws", read(log)), 20)
        check("kn_brain starts on an ephemeral port", m is not None, read(log)[-300:])
        if m is None:
            return 1
        port = int(m.group(1))
        token = register(port, "f5al" + secrets.token_hex(4))  # at most 14 characters: a new name's cap (v10.0.3 maxNameLength, deob-10.0.3.js:22725)
        check("a test account registers (POST /auth/register) for the logged-in checks", token is not None)
        seed_sales(cfg["path"])
        run_checks(port, token)
        run_raw_checks(port)
    finally:
        for p in procs:
            stop(p)
        subprocess.run([sys.executable, "-I", os.path.join(HERE, "db_scratch.py"), ROOT, "drop", "sqlite", NAME],
                       capture_output=True)
    passed = sum(RESULTS)
    failed = len(RESULTS) - passed
    print("v10_api_alias_test: %d PASS, %d FAIL" % (passed, failed))
    if failed:
        print("logs kept in " + work)
    else:
        shutil.rmtree(work, ignore_errors=True)
    return 0 if RESULTS and not failed else 1


if __name__ == "__main__":
    sys.exit(main())
