#!/usr/bin/env python3
"""The v10 market, items, trades, moderation, mods, KPD and players routes on kn_brain's root paths
(services/brain/v10_market.c; route inventory doc/v10/api-inventory.md, v10.js 26857-27365).

  v10_api_market_test.py <kn_brain | build dir> [repo root]
  (CTest passes the kn_brain binary and the source root; a build dir means BUILD/services/brain/kn_brain.)

A scratch sqlite database (db_scratch.py), kn_brain --migrate-only, then kn_brain on an ephemeral port.
One check per route, stdlib only. Each check names the method, the path, the kind of answer and the
v10.js line of the v10 caller that reads it:

  EMPTY      200, {"data": ..., "error": null} with exactly the shape v10 reads (its own default)
  REFUSAL    501, {"error": {"code": "NOT_IMPLEMENTED", "message": ...}} (writes, and reads of an
             account KrunkNative does not map over HTTP)
  NOT_FOUND  404, {"error": {"code": "NOT_FOUND", ...}} (a read of an item id that is not in skins.json)

Exit 0 when every check passes, 1 otherwise, 77 when kn_brain or the sqlite scratch database is
unavailable (CTest's skip).
"""
import json
import os
import re
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
NAME = "kn_test_v10mkt_%d" % os.getpid()
SECRET = "kn-v10-market-test-secret-%d" % os.getpid()
RESULTS = []

# an item index that is in assets/skins.json (the first row), and one that is not
ITEM = 0
NO_ITEM = 999999
# any player name: the account-bound routes refuse without looking the name up
PLAYER = "kn_test_player"


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


def http(port, method, path, body=None):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request("http://127.0.0.1:%d%s" % (port, path), data=data, method=method,
                                 headers={"Content-Type": "application/json"} if data is not None else {})
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            status, raw = r.status, r.read()
    except urllib.error.HTTPError as e:
        status, raw = e.code, e.read()
    try:
        return status, json.loads(raw or b"null")
    except ValueError:
        return status, None


def eq(value):
    return lambda data: data == value


# (method, path, body, kind, expected data predicate or None, v10.js reference)
ROUTES = [
    # ---- market (v10.js 26857-26948)
    ("GET", "/market/listings", None, "EMPTY",
     eq({"listings": [], "has_more": False}), "26857, read at 128405"),
    ("GET", "/market/inventory", None, "REFUSAL", None,
     "26862, read at 128416: the caller's own inventory"),
    ("GET", "/market/sales", None, "REFUSAL", None,
     "26867, read at 134079: the caller's own sales"),
    ("GET", "/market/stats?limit=20", None, "EMPTY",
     eq({"stats": [], "has_more": False}), "26882, read at 128424"),
    ("GET", "/market/items/%d/sales" % ITEM, None, "EMPTY",
     eq([]), "26887, no v10 caller"),
    ("GET", "/market/items/%d/sales-statistics?daysback=7" % ITEM, None, "EMPTY",
     eq([]), "26895, read at 134931"),
    ("POST", "/market/list", {"item_id": ITEM, "price": 1}, "REFUSAL", None,
     "26900, write, read at 128440"),
    ("POST", "/market/purchase", {"market_id": 1}, "REFUSAL", None,
     "26906, write, read at 132836"),
    ("POST", "/market/unlist", {"market_id": 1}, "REFUSAL", None,
     "26912, write, read at 132978"),
    ("POST", "/market/quick-sell", {"item_id": ITEM, "action": "kr", "amount": 1}, "REFUSAL", None,
     "26918, write, read at 131580"),
    ("GET", "/market/black-market", None, "EMPTY",
     eq([]), "26924, read at 128486"),
    ("GET", "/market/black-market/rank", None, "EMPTY",
     lambda data: data is None, "26929, read at 128494: null = no rank here (Promise.all with the list)"),
    ("POST", "/market/black-market/purchase", {"item_index": 1}, "REFUSAL", None,
     "26934, write, read at 136215"),
    ("GET", "/market/history?sort=newest&limit=50", None, "REFUSAL", None,
     "26948, read at 131476: the caller's own history (\"Please log in to view your history\")"),

    # ---- items (v10.js 26957-26980)
    ("GET", "/items/%d/view" % ITEM, None, "EMPTY",
     eq({"stats": None, "owners": [], "sales": [], "listings": [], "stackable": False, "wearable": False}),
     "26957, read at 128558 (its own default shape)"),
    ("GET", "/items/%d/sales?range=0" % ITEM, None, "EMPTY",
     eq([]), "26963, read at 129914"),
    ("GET", "/items/%d/instances?sort=mint" % ITEM, None, "EMPTY",
     eq([]), "26969, read at 128573"),
    ("POST", "/items/%d/paint" % ITEM, {"item_id": 1, "paint_item_id": 2}, "REFUSAL", None,
     "26974, write, read at 128724"),
    ("POST", "/items/%d/style" % ITEM, {"item_id": 1, "style": 1}, "REFUSAL", None,
     "26980, write, read at 128924"),

    # ---- trades (v10.js 27047-27078)
    ("GET", "/trades/history?offset=0&limit=30", None, "REFUSAL", None,
     "27047, read at 108327: the caller's own trades"),
    ("POST", "/trades/create", {"target_player_id": 1, "my_offer": [], "their_offer": [], "message": ""}, "REFUSAL", None,
     "27052, write, read at 109194"),
    ("POST", "/trades/1/accept", {}, "REFUSAL", None,
     "27058, write, read at 135709"),
    ("POST", "/trades/1/cancel", {}, "REFUSAL", None,
     "27063, write, read at 108339"),
    ("POST", "/trades/decline-all", {}, "REFUSAL", None,
     "27068, write, read at 135779"),
    ("GET", "/trades/inventory/%s" % PLAYER, None, "REFUSAL", None,
     "27073, read at 109226: another account's inventory"),
    ("GET", "/trades/player-search?name=%s" % PLAYER, None, "REFUSAL", None,
     "27078, read at 108349: an account lookup (the social tunnel's /players/search)"),

    # ---- moderation (v10.js 27087-27098)
    ("GET", "/moderation/chat/%s" % PLAYER, None, "REFUSAL", None,
     "27087, read at 110003 and 217008: another account's chat ban"),
    ("POST", "/moderation/chat", {"name": PLAYER, "chat_banned": 1}, "REFUSAL", None,
     "27092, write, read at 109549"),
    ("POST", "/moderation/flag", {"name": PLAYER, "reason": "x"}, "REFUSAL", None,
     "27098, write, read at 109570"),

    # ---- mods (v10.js 27195-27229)
    ("POST", "/mods/publish/key", {"name": "mod", "assetSizes": {"mod": 1}}, "REFUSAL", None,
     "27195, write, read at 213947"),
    ("GET", "/mods/by-name/example", None, "REFUSAL", None,
     "27201, read at 107663: a published mod record (community catalogue, not on the v10 root)"),
    ("POST", "/mods/1/vote", {"vote": 1}, "REFUSAL", None,
     "27204, write, no direct v10 caller"),
    ("POST", "/mods/1/feature", {"featured": True}, "REFUSAL", None,
     "27212, write, read at 107604"),
    ("DELETE", "/mods/1", None, "REFUSAL", None,
     "27220, write, read at 107616"),
    ("POST", "/mods/publish", None, "REFUSAL", None,
     "27229, write (multipart upload), no direct v10 caller"),

    # ---- KPD (v10.js 27329-27354)
    ("GET", "/kpd/demos?off=0&q=", None, "EMPTY",
     eq({"rows": [], "more": False}), "27329, read at 216006 (policeRes code 4)"),
    ("GET", "/kpd/demos/1", None, "REFUSAL", None,
     "27335, read at 222638: an officer's demo view (needs the ct token and the officer account)"),
    ("POST", "/kpd/demos/1/vote", {"ct": "x", "verdict": 1}, "REFUSAL", None,
     "27345, write, no direct v10 caller"),
    ("GET", "/kpd/reviewers", None, "EMPTY",
     eq({"month": [], "all": []}), "27354, read at 216013 (policeRes code 5)"),

    # ---- players, HTTP only (v10.js 27123-27182)
    ("GET", "/players/%s/sales?range=0" % PLAYER, None, "REFUSAL", None,
     "27123, read at 109908: another account's sales"),
    ("GET", "/players/%s/listings" % PLAYER, None, "REFUSAL", None,
     "27128, read at 110044: another account's listings"),
    ("POST", "/players/%s/verify" % PLAYER, {"featured": True}, "REFUSAL", None,
     "27160, write, read at 109590"),
    ("POST", "/players/%s/gift" % PLAYER, {"amount": 1, "message": ""}, "REFUSAL", None,
     "27182, write, read at 109361"),
]

# item reads that name an item the skins table does not have: 404 (a real source says it is absent)
NOT_FOUND_ROUTES = [
    ("GET", "/items/%d/view" % NO_ITEM, None, "NOT_FOUND", None, "26957: unknown item index"),
    ("GET", "/market/items/%d/sales" % NO_ITEM, None, "NOT_FOUND", None, "26887: unknown item index"),
]


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
        ok = status == 501 and err.get("code") == "NOT_IMPLEMENTED" and bool(err.get("message")) and "data" not in reply
        check(name, ok, "HTTP %d %s" % (status, json.dumps(reply)[:160]))
    else:  # NOT_FOUND
        err = reply.get("error") if isinstance(reply.get("error"), dict) else {}
        ok = status == 404 and err.get("code") == "NOT_FOUND" and "data" not in reply
        check(name, ok, "HTTP %d %s" % (status, json.dumps(reply)[:160]))


def scenario(port):
    for method, path, body, kind, pred, ref in ROUTES:
        run_route(port, method, path, body, kind, pred, ref)
    for method, path, body, kind, pred, ref in NOT_FOUND_ROUTES:
        run_route(port, method, path, body, kind, pred, ref)


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
            scenario(int(m.group(1)))
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
