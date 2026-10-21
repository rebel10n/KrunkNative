#!/usr/bin/env python3
"""The Krunker v10 REST routes answered at the brain's root paths (services/brain/turf.c, lottery.c,
shop.c, ranked.c, leaderboards.c, creations.c, push.c, public_routes.c, streams.c, guest.c, community.c).

  v10_api_alias_test.py <kn_brain | build dir> [repo root]
  (a build dir means BUILD/services/brain/kn_brain; the repo root holds db_scratch.py.)

A scratch sqlite database (db_scratch.py), kn_brain --migrate-only, then kn_brain on an ephemeral port.
Every check is one HTTP request, one (method, path) of the v10 inventory (`node` over
/home/user/.cache/kn-api-inv/inv-data.mjs), with the shape its v10.js consumer reads. The consumer's line
is in the check name. Stdlib only: no websockets, no scripted game server.

A route is FAIL when the brain does not route it (the 404 fallback {"error": "not_found"}) or routes it
with the wrong method (405). Routes that need a login (no token here) pass when the brain answers with the
v10 error shape {"error": ...} at a 4xx status: that proves the route and its gate, not the success
shape, which the consumer reads only with a session. The streams route passes at 503 StreamsUnavailable
when no Twitch client is configured (the test brain has none).

Each check prints one PASS or FAIL line; the last line is the count. Exit 0 when every check passes,
1 otherwise, 77 when kn_brain or the sqlite scratch database is unavailable (CTest's skip).
"""
import json
import os
import re
import secrets
import shutil
import signal
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
NAME = "kn_test_v10alias_%d" % os.getpid()
SECRET = "kn-v10alias-test-secret-%d" % os.getpid()
DRIVER = os.environ.get("KN_TEST_DB_DRIVER", "sqlite")
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


def wait_for(fn, secs):
    end = time.time() + secs
    while time.time() < end:
        v = fn()
        if v:
            return v
        time.sleep(0.1)
    return None


def call(port, method, path, body=None):
    """(status, parsed JSON or None)"""
    data = json.dumps(body).encode() if body is not None else None
    headers = {"Accept": "application/json"}
    if data is not None:
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request("http://127.0.0.1:%d%s" % (port, path), data=data, method=method, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=15) as r:
            status, raw = r.status, r.read()
    except urllib.error.HTTPError as e:
        status, raw = e.code, e.read()
    try:
        return status, json.loads(raw or b"null")
    except ValueError:
        return status, None


def unrouted(status, body):
    """the brain's 404 fallback (public_routes.c h_not_found) or a wrong method (405)"""
    return status == 405 or (status == 404 and isinstance(body, dict) and body.get("error") == "not_found")


def is_list(v):
    return isinstance(v, list)


# ---- predicates: (status, body) -> (ok, what it saw)

def p_data_list(status, body):
    """200 with data a list (v10 gapi: body {"data": [...]}, the consumer reads data || [])"""
    ok = status == 200 and isinstance(body, dict) and is_list(body.get("data"))
    return ok, "data is a list" if ok else "HTTP %d, data not a list" % status


def p_data_key_list(key):
    def f(status, body):
        d = body.get("data") if isinstance(body, dict) else None
        ok = status == 200 and isinstance(d, dict) and is_list(d.get(key))
        return ok, "data.%s is a list" % key if ok else "HTTP %d, data.%s not a list" % (status, key)
    return f


def p_data_keys(keys):
    def f(status, body):
        d = body.get("data") if isinstance(body, dict) else None
        ok = status == 200 and isinstance(d, dict) and all(k in d for k in keys)
        return ok, "data has %s" % ", ".join(keys) if ok else "HTTP %d, data lacks %s" % (status, ", ".join(keys))
    return f


def p_data(status, body):
    """200 with a data key (any value: the consumer tests data itself)"""
    ok = status == 200 and isinstance(body, dict) and "data" in body
    return ok, "data present" if ok else "HTTP %d, no data key" % status


def p_data_or_error(status, body):
    """200 with data, or the v10 error shape at 4xx/5xx (an unknown clan or lottery id answers the error)"""
    if status == 200 and isinstance(body, dict) and "data" in body:
        return True, "data present"
    ok = status >= 400 and isinstance(body, dict) and "error" in body
    return ok, "error %s" % (body.get("error") if ok else "")


def p_auth_error(status, body):
    """4xx with the v10 error key: the login gate answers (no token in this test)"""
    ok = 400 <= status < 500 and isinstance(body, dict) and "error" in body
    return ok, "login gate answers %d" % status if ok else "HTTP %d, no error key" % status


def p_bare_object_ok(status, body):
    ok = status == 200 and isinstance(body, dict)
    return ok, "JSON object" if ok else "HTTP %d, not an object" % status


def p_player_count(status, body):
    ok = status == 200 and isinstance(body, dict) and isinstance(body.get("playerCount"), int)
    return ok, "playerCount is a number" if ok else "HTTP %d, no playerCount" % status


def p_streams(status, body):
    """the Twitch list under data.streams; 503 StreamsUnavailable when this brain has no Twitch client"""
    if status == 503 and isinstance(body, dict) and body.get("error") == "StreamsUnavailable":
        return True, "route answers 503 StreamsUnavailable (no Twitch client configured)"
    d = body.get("data") if isinstance(body, dict) else None
    ok = status == 200 and isinstance(d, dict) and is_list(d.get("streams"))
    return ok, "data.streams is a list" if ok else "HTTP %d, no data.streams" % status


def p_guest(status, body):
    d = body.get("data") if isinstance(body, dict) else None
    p = d.get("profile") if isinstance(d, dict) else None
    ok = status == 200 and isinstance(p, dict) and "funds" in p and "score" in p
    return ok, "data.profile has funds, score" if ok else "HTTP %d, no data.profile" % status


def p_news(status, body):
    d = body.get("data") if isinstance(body, dict) else None
    ok = status == 200 and isinstance(d, dict) and is_list(d.get("news"))
    return ok, "data.news is a list" if ok else "HTTP %d, no data.news" % status


def p_events(status, body):
    d = body.get("data") if isinstance(body, dict) else None
    ok = status == 200 and isinstance(d, dict) and is_list(d.get("events")) and is_list(d.get("polls"))
    return ok, "data.events and data.polls are lists" if ok else "HTTP %d, no data.events/polls" % status


def p_wheels(status, body):
    d = body.get("data") if isinstance(body, dict) else None
    ok = status == 200 and isinstance(d, dict) and is_list(d.get("wheels")) and isinstance(d.get("itemPools"), dict)
    return ok, "data.wheels list, data.itemPools object" if ok else "HTTP %d, no data.wheels/itemPools" % status


# ---- the checks: (method, path, body, predicate, v10.js citation)
# the path is the root path the v10 client builds (inv-data.mjs); {id} values are fixed here

def checks(guest_id):
    return [
        ("GET", "/ranked/shop", None, p_data_key_list("skins"), "v10.js 111025-111026, def 26381"),
        ("POST", "/ranked/purchase", {"skin_id": 1}, p_auth_error, "v10.js 111039-111044, def 26386"),
        ("GET", "/ranked/me/connection", None, p_auth_error, "v10.js 98725-98726, def 25985"),
        ("GET", "/leaderboards/player_score", None, p_data_list, "v10.js 138453-138456, def 26995"),
        ("GET", "/leaderboards/raids/map1", None, p_data_list, "v10.js 138946-138948, def 27005"),
        ("GET", "/leaderboards/raids/map1/monthly?month=2026-10", None, p_data_list, "v10.js 138953-138955, def 27016"),
        ("GET", "/leaderboards/krunkcup/teams", None, p_data_list, "v10.js 139396-139398, def 27021"),
        ("GET", "/leaderboards/krunkcup/teams/1/contributors", None, p_data_list, "v10.js 139378-139380, def 27026"),
        ("GET", "/leaderboards/krunkcup/players?limit=5", None, p_data_list, "v10.js def 27032 (no bundled caller)"),
        ("GET", "/territorial-wars/status", None, p_data, "v10.js 142468-142472, def 26673"),
        ("GET", "/territorial-wars/clan/1", None, p_data_or_error, "v10.js 141733-141736, def 26678"),
        ("GET", "/territorial-wars/clan/1/players", None, p_data_or_error, "v10.js 141740-141744, def 26688"),
        ("GET", "/territorial-wars/clan/1/earnings", None, p_data_or_error, "v10.js 141747-141749, def 26693"),
        ("GET", "/territorial-wars/clan/1/payouts", None, p_data_or_error, "v10.js 141754, def 26722"),
        ("GET", "/territorial-wars/leaderboards/top-performing-clans", None, p_data_list, "v10.js 143288-143292, def 26704"),
        ("GET", "/territorial-wars/leaderboards/top-grossing-clans", None, p_data_list, "v10.js 143454-143458, def 26713"),
        ("GET", "/territorial-wars/leaderboards/top-performing-players", None, p_data_list, "v10.js 143606-143610, def 26732"),
        ("GET", "/territorial-wars/player/1", None, p_data_or_error, "v10.js 141761-141764, def 26737"),
        ("GET", "/territorial-wars/player/1/contributions", None, p_data_or_error, "v10.js 141768, def 26743"),
        ("GET", "/territorial-wars/player/1/earnings", None, p_data_or_error, "v10.js 141775, def 26746"),
        ("GET", "/clans/recruitment", None, p_data_keys(("posts", "total")), "v10.js 143826-143832, def 26803"),
        ("GET", "/clans/1/recruitment", None, p_data_or_error, "v10.js 96683-96686, def 26806"),
        ("POST", "/clans/1/recruitment", {"description": "x"}, p_auth_error, "v10.js 27661, def 26809"),
        ("DELETE", "/clans/1/recruitment", None, p_auth_error, "v10.js 27648, def 26815"),
        ("GET", "/clans/my-applications", None, p_auth_error, "v10.js 143875-143877, def 26820"),
        ("POST", "/clans/1/apply", None, p_auth_error, "v10.js 144072-144077, def 26823"),
        ("GET", "/clans/1/loot-config", None, p_data_or_error, "v10.js 27602-27604, def 26764"),
        ("PUT", "/clans/1/loot-config", {"clan_pct": 10}, p_auth_error, "v10.js 96456, def 26767"),
        ("GET", "/lottery/1", None, p_data_or_error, "v10.js 118082-118083, def 26566"),
        ("POST", "/lottery/1/participate", {"cost_type": "kr", "cost_ref": "x", "cost_quantity": 1}, p_data_or_error,
         "v10.js 125504-125506, def 26571"),
        ("GET", "/shop/displays/limited", None, p_data_or_error, "v10.js 113916-113920, def 26520"),
        ("GET", "/shop/displays?tag=limited", None, p_data_or_error, "v10.js def 26525 (getDisplayByTag)"),
        ("POST", "/shop/purchase", {"product_id": 1, "quantity": 1}, p_auth_error, "v10.js 113943-113948, def 26530"),
        ("GET", "/webhooks/general/maps/list?index=0&pos=0", None, p_data_list, "v10.js 221943-221946, def 131894"),
        ("GET", "/webhooks/general/maps/search?val=x&cat=-1", None, p_data_list, "v10.js def 131908 (searchMap)"),
        ("GET", "/webhooks/general/mods/list?index=0&pos=0", None, p_data_list, "v10.js 211797, def 131921"),
        ("GET", "/webhooks/general/mods/search?val=x", None, p_data_list, "v10.js 219061-219063, def 131928"),
        ("GET", "/webhooks/general/assets/list?index=0&cat=-1", None, p_data_list, "v10.js def 131933 (no bundled caller)"),
        ("GET", "/webhooks/general/assets/search?index=2&cat=-1&val=x", None, p_data_list,
         "v10.js def 131940 (no bundled caller)"),
        ("GET", "/webhooks/general/items/prices", None, p_bare_object_ok, "v10.js 219262-219265, def 131947"),
        ("POST", "/webpush/subscribe", {"subscription": {"endpoint": "https://push.example/e", "keys": {"p256dh": "", "auth": ""}}},
         p_auth_error, "v10.js 146596-146606, def 26396"),
        ("POST", "/webpush/unsubscribe", {"registration_id": None, "endpoint": "https://push.example/e"}, p_auth_error,
         "v10.js 146632-146636, def 26402"),
        ("GET", "/spins/wheels", None, p_wheels, "v10.js def 27294 (no bundled caller); packed /data/spins reads wheels, itemPools 102769-102776"),
        ("GET", "/player-count", None, p_player_count, "v10.js 211170-211172 (social host)"),
        ("GET", "/streams/top", None, p_streams, "v10.js 145057-145059, 25991-25993"),
        ("GET", "/guest/profile?guest=" + guest_id, None, p_guest, "v10.js 117249-117252, def 26372"),
        ("GET", "/creators/events", None, p_events, "v10.js 196818-196820, def 27107"),
        ("GET", "/social/news", None, p_news, "v10.js 140724-140727, def 27285"),
    ]


def run_checks(port):
    guest_id = secrets.token_hex(16)  # a random id: not printed
    for method, path, body, pred, cite in checks(guest_id):
        status, data = call(port, method, path, body)
        label = "%s %s  HTTP %d  [%s]" % (method, path.replace(guest_id, "<guest>"), status, cite)
        if unrouted(status, data):
            check(label, False, "not routed (%s)" % ("405 wrong method" if status == 405 else "404 fallback"))
            continue
        ok, seen = pred(status, data)
        check(label, ok, seen)


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
        brain = subprocess.Popen([BRAIN, "--brain.host=127.0.0.1", "--brain.port=0", "--brain.region=local",
                                  "--brain.id=v10alias-test-%d" % os.getpid(), "--finder.poll_interval_s=1"],
                                 cwd=ROOT, env=env, stdout=open(log, "w"), stderr=subprocess.STDOUT,
                                 stdin=subprocess.DEVNULL, start_new_session=True)
        procs.append(brain)
        m = wait_for(lambda: re.search(r"listening on http://127\.0\.0\.1:(\d+) \(ws", read(log)), 20)
        check("kn_brain starts on an ephemeral port", m is not None, read(log)[-300:])
        if m is None:
            return 1
        run_checks(int(m.group(1)))
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
