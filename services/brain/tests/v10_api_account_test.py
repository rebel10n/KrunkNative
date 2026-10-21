#!/usr/bin/env python3
"""The v10 account, player, ranked, daily spin, web push, spin and KrunkCup routes on kn_brain's root
paths (services/brain/v10_account.c, ranked.c, daily_spin.c, push.c, store.c; route inventory
doc/v10/api-inventory.md, rows 1-4, 7-13, 13, 50, 51, 63, 64, 187-189).

  v10_api_account_test.py <kn_brain | build dir> [repo root]
  (CTest passes the kn_brain binary and the source root; a build dir means BUILD/services/brain/kn_brain.)

A scratch sqlite database (db_scratch.py), kn_brain --migrate-only, the accounts seeded into that
database (accounts, players, sessions: the bearer tokens are the sha256 the brain stores), then kn_brain
on an ephemeral port with the ranked season pinned to 7. One check per route, stdlib only. Each check
names the method, the path, the caller and the v10.0.3 decoded line of the path literal, and the v10.js
line of the v10.0.2 caller the inventory cites:

  DATA     200, {"data": <what the v10.0.3 consumer reads>, "error": null}
  NONE     200, {"data": null, "error": null}: no KrunkNative source (the v10 'none' value)
  REFUSAL  403, {"error": {"code": "NOT_IMPLEMENTED", ...}}: a real write KrunkNative cannot do
  SIGNEDOUT 401, {"error": {"code": "LOGIN_REQUIRED", ...}}: the route needs the caller's session
  BAD      400, {"error": {...}}: a request the route refuses (an index that is not a wheel)

Exit 0 when every check passes, 1 otherwise, 77 when kn_brain or the sqlite scratch database is
unavailable (CTest's skip).
"""
import datetime
import hashlib
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
NAME = "kn_test_v10acct_%d" % os.getpid()
SECRET = "kn-v10-account-test-secret-%d" % os.getpid()
SEASON = 7          # --brain.ranked.season: the ranked season the seeded profiles belong to
REGION = "3"        # v10 regionIndex 3 (eu): the consumers pass the index, the path carries it
RESULTS = []

NOW = datetime.datetime.now(datetime.timezone.utc)
STAMP = NOW.strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]
FAR = "2099-01-01 00:00:00.000"

# the seeded accounts: name -> (funds, bearer token)
ACCOUNTS = {
    "alice": (2500, "kn-v10-acct-alice-%d" % os.getpid()),   # ranked: 10 matches, a milestone to claim
    "bob": (100, "kn-v10-acct-bob-%d" % os.getpid()),       # ranked: 8 matches
    "carol": (0, "kn-v10-acct-carol-%d" % os.getpid()),     # match_results today: 1200 score
    "dave": (5000, "kn-v10-acct-dave-%d" % os.getpid()),    # the store spin (Starter costs 50 KR)
}
IDS = {}
EP_ALICE = "https://push.example.test/sub/alice-1"
EP_ALICE2 = "https://push.example.test/sub/alice-2"
EP_UNKNOWN = "https://push.example.test/sub/nobody"
SAMPLE_KEYS = {}


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
        with urllib.request.urlopen(req, timeout=10) as r:
            status, raw = r.status, r.read()
    except urllib.error.HTTPError as e:
        status, raw = e.code, e.read()
    try:
        return status, json.loads(raw or b"null")
    except ValueError:
        return status, None


def error_code(body):
    err = body.get("error") if isinstance(body, dict) else None
    return err.get("code") if isinstance(err, dict) else None


def b64url(nbytes):
    import base64
    return base64.urlsafe_b64encode(os.urandom(nbytes)).decode().rstrip("=")


def token_hash(token):
    return hashlib.sha256(token.encode()).hexdigest()


# ---- the seeded data (the tables the routes read; migrations 0100, 0107, 0201, 0205, 0344)

def seed(db_path):
    con = sqlite3.connect(db_path)
    try:
        for name, (funds, token) in ACCOUNTS.items():
            cur = con.execute("INSERT INTO accounts (username, password_hash) VALUES (?, 'x')", (name,))
            aid = cur.lastrowid
            IDS[name] = aid
            con.execute("INSERT INTO players (player_id, player_alias, player_funds) VALUES (?, ?, ?)", (aid, name, funds))
            con.execute("INSERT INTO sessions (token_hash, account_id, expires_at, origin) VALUES (?, ?, ?, 'test')",
                        (token_hash(token), aid, FAR))
        a, b = IDS["alice"], IDS["bob"]
        # ranked profiles of season SEASON, region eu (the code of index REGION)
        con.execute("INSERT INTO ranked_profiles (account_id, season, region, cr, wins, losses, draws, total_kills,"
                    " total_deaths, total_score, last_milestone_claimed) VALUES (?, ?, 'eu', 25.0, 7, 3, 0, 90, 60, 9000, -1)",
                    (a, SEASON))
        con.execute("INSERT INTO ranked_profiles (account_id, season, region, cr, wins, losses, draws, total_kills,"
                    " total_deaths, total_score, last_milestone_claimed) VALUES (?, ?, 'eu', 18.0, 6, 2, 0, 70, 50, 7000, -1)",
                    (b, SEASON))
        # one done ranked match, alice (team 0, won 3 - 1) and bob (team 1)
        result = json.dumps({"winner": 0, "teamScores": [3, 1], "players": [{"accountId": a, "objective": 4}]})
        con.execute("INSERT INTO mm_ranked_matches (match_id, queue_id, region, brain_id, status, result, season,"
                    " map_index, mode_index, finished_at) VALUES ('kn-test-m1', 'ranked', 'eu', 'test', 'done', ?, ?, 3, 0, ?)",
                    (result, SEASON, STAMP))
        for acc, nm, team, before, after, victory in ((a, "alice", 0, 1000, 1020, 1), (b, "bob", 1, 1010, 990, 0)):
            con.execute("INSERT INTO mm_ranked_match_players (match_id, account_id, username, team, rating_key,"
                        " rating_before, rating_after, kills, deaths, assists, score, damage_done, victory)"
                        " VALUES ('kn-test-m1', ?, ?, ?, 'eu', ?, ?, 9, 3, 1, 1500, 4000, ?)",
                        (acc, nm, team, before, after, victory))
        # carol's score today (match_results: the stats commits of the game servers)
        con.execute("INSERT INTO match_results (match_id, account_id, score, at) VALUES ('kn-test-ds1', ?, 1200, ?)",
                    (IDS["carol"], STAMP))
        # the web push subscription alice holds
        con.execute("INSERT INTO push_subscriptions (endpoint, account_id, p256dh, auth, created_at) VALUES (?, ?, ?, ?, 1)",
                    (EP_ALICE, a, SAMPLE_KEYS["p256dh"], SAMPLE_KEYS["auth"]))
        con.commit()
    finally:
        con.close()


def token(name):
    return ACCOUNTS[name][1]


# ---- predicates on the data the consumer reads

def is_profile(d):
    p = d.get("profile") if isinstance(d, dict) else None
    ms = d.get("milestones") if isinstance(d, dict) else None
    return (isinstance(p, dict) and p.get("wins") == 7 and p.get("losses") == 3 and p.get("total_matches") == 10
            and p.get("conservative_rating") == 25.0 and isinstance(ms, list) and len(ms) == 5
            and ms[0].get("matches") == 10 and ms[0].get("reward") == 100 and d.get("season") == SEASON)


def is_board(d):
    rows = d.get("leaderboard") if isinstance(d, dict) else None
    return (isinstance(rows, list) and [r.get("profile", {}).get("player_name") for r in rows] == ["alice", "bob"]
            and [r.get("position") for r in rows] == [1, 2] and d.get("totalPages") == 1)


def is_history(d):
    mh = d.get("matchHistory") if isinstance(d, dict) else None
    ms = mh.get("matches") if isinstance(mh, dict) else None
    if not isinstance(ms, list) or len(ms) != 1 or mh.get("totalPages") != 1:
        return False
    m = ms[0]
    entries = m.get("historyEntries") or []
    return (m.get("map") == 3 and m.get("score_alpha") == 3 and m.get("score_bravo") == 1 and m.get("mmr_change") == 20
            and sorted(e.get("player_name") for e in entries) == ["alice", "bob"])


def is_empty_history(d):
    return d == {"matchHistory": {"matches": [], "totalPages": 0}}


def is_claim(d):
    return isinstance(d, dict) and d.get("success") is True and d.get("reward") == 100 and d.get("balance") == 100


def is_claim_again(d):
    return isinstance(d, dict) and d.get("success") is False and d.get("error") == "Milestone not reached"


def is_progress(d):
    return (isinstance(d, dict) and d.get("score") == 1200 and d.get("maxScore") == 5000 and d.get("claimed") is False
            and d.get("stage") == 0 and isinstance(d.get("timeRemaining"), int) and 0 < d["timeRemaining"] <= 86400000)


def is_me(d):
    return isinstance(d, dict) and d.get("id") == IDS["alice"] and d.get("username") == "alice" and "token" not in d


def is_wallet(d):
    return isinstance(d, dict) and d.get("funds") == ACCOUNTS["alice"][0] and isinstance(d.get("junk"), str)


def is_ip(d):
    return isinstance(d, dict) and d.get("ip") == "127.0.0.1"


def subscribed(v):
    return lambda d: isinstance(d, dict) and d.get("is_subscribed") is v


def is_change_ok(d):
    return isinstance(d, dict) and d.get("success") is True and d.get("registration_id") is None


def is_change_unknown(d):
    return isinstance(d, dict) and d.get("success") is False


def is_spin(d):
    return (isinstance(d, dict) and isinstance(d.get("itemIDs"), list) and len(d["itemIDs"]) == 1
            and d.get("funds") == ACCOUNTS["dave"][0] - 50 and d.get("redeemedTicket") is None
            and isinstance(d.get("randNums"), list) and len(d["randNums"]) == 1)


def is_none(d):
    return d is None


def change_body(old_endpoint):
    """a v10 subscriptionChange body (v10.js 145274): the PushSubscription JSON of each side"""
    return lambda: {"old_subscription": {"endpoint": old_endpoint, "keys": SAMPLE_KEYS},
                    "new_subscription": {"endpoint": EP_ALICE2, "keys": SAMPLE_KEYS}}


# (method, path, body, caller, kind, predicate, cite)
#   cite: "v10.0.3 decoded line; v10.js line of the v10.0.2 caller (inventory row)"
ROUTES = [
    # ---- ranked: the account's match history, profile, leaderboard, milestone (v10.0.3 3734, 4921, 5160)
    ("GET", "/match-history/me/season/%d/gameMode/all/isRanked/true/region/%s?limit=5&offset=0" % (SEASON, REGION),
     None, "alice", "DATA", is_history,
     "row 1: match history, isRanked true (decoded 4921; v10.js 25940, 98633)"),
    ("GET", "/match-history/me/season/%d/gameMode/all/isRanked/true/region/%s?limit=5&offset=0" % (SEASON, REGION),
     None, None, "SIGNEDOUT", None,
     "row 1: signed out (decoded 4921; v10.js 98628 returns the empty history before the call)"),
    ("GET", "/match-history/me/season/%d/gameMode/all/isRanked/all/region/%s?limit=5&offset=0" % (SEASON, REGION),
     None, "alice", "DATA", is_empty_history,
     "row 1: isRanked all has no KrunkNative source in the v10 shape (decoded 4921; no v10 caller asks it)"),
    ("GET", "/player-profile/me/season/%d/region/%s" % (SEASON, REGION), None, "alice", "DATA", is_profile,
     "row 2: ranked profile of the caller (decoded 3734; v10.js 25959, 98579)"),
    ("GET", "/player-profile/me/season/%d/region/%s" % (SEASON, REGION), None, None, "SIGNEDOUT", None,
     "row 2: signed out (decoded 3734)"),
    ("GET", "/leaderboards/rankings/season/%d/region/%s?limit=10&offset=0" % (SEASON, REGION), None, None, "DATA",
     is_board, "row 3: ranked leaderboard, placed players by conservative rating (decoded 5160; v10.js 25972, 98600)"),
    ("POST", "/player-profile/me/claim-milestone/region/%s" % REGION, {}, "alice", "DATA", is_claim,
     "row 4: claim the next ranked milestone, 100 RP (decoded 5160; v10.js 25976, 98741)"),
    ("POST", "/player-profile/me/claim-milestone/region/%s" % REGION, {}, "alice", "DATA", is_claim_again,
     "row 4: the next milestone is not reached yet: success false with the reason (decoded 5160; v10.js 98741)"),
    ("POST", "/player-profile/me/claim-milestone/region/%s" % REGION, {}, None, "SIGNEDOUT", None,
     "row 4: signed out (decoded 5160)"),

    # ---- daily spin: today's score from match_results (decoded 3573; v10.js 25999, 117993)
    ("GET", "/daily-spin/progress", None, "carol", "DATA", is_progress,
     "row 7: score since the UTC day began, the maximum, the time left, claimed, stage (decoded 3573)"),
    ("GET", "/daily-spin/progress", None, None, "SIGNEDOUT", None, "row 7: signed out (decoded 3573)"),

    # ---- payments: no payment backend (decoded 5160, 3546, 2704, 3576; v10.js 26006-209131)
    ("POST", "/shop/cancel-subscription", {"subscriptionId": "x"}, None, "REFUSAL", None,
     "row 8: no payment backend (decoded 5160; v10.js 26006)"),
    ("POST", "/iap/checkout", {}, None, "REFUSAL", None, "row 9: no payment backend (decoded 5160; v10.js 26018)"),
    ("POST", "/iap/claim", {"channelId": "x", "productId": "x", "purchaseId": "x"}, None, "REFUSAL", None,
     "row 10: no payment backend (decoded 3546; v10.js 26024)"),
    ("POST", "/my-api/create-payment/1", {}, None, "REFUSAL", None,
     "row 11: PayPal, no payment backend (decoded 2704; v10.js 209123)"),
    ("POST", "/my-api/execute-payment/1", {}, None, "REFUSAL", None,
     "row 12: PayPal, no payment backend (decoded 3576; v10.js 209131)"),

    # ---- IP checker: no consumer reads the body; the peer address is the one field KrunkNative has
    ("GET", "/ipinfo", None, None, "DATA", is_ip,
     "row 13: the caller's peer address (decoded 117; v10.js 16440; the URL getter 19733 is the only reader)"),

    # ---- player and wallet (decoded 5160, 4527; v10.js 26312, 26317: no v10 caller reads them)
    ("GET", "/player/me", None, "alice", "DATA", is_me,
     "row 50: the signed-in account (decoded 5160; v10.js 26312)"),
    ("GET", "/player/me", None, None, "SIGNEDOUT", None, "row 50: signed out (decoded 5160)"),
    ("GET", "/player/wallet", None, "alice", "DATA", is_wallet,
     "row 51: the KR balance (players.player_funds) and junk (decoded 4527; v10.js 26317)"),
    ("GET", "/player/wallet", None, None, "SIGNEDOUT", None, "row 51: signed out (decoded 4527)"),

    # ---- web push: push_subscriptions (decoded 62, 3323; v10.js 26408, 26414, consumers 145259, 145274)
    ("POST", "/webpush/status", {"endpoint": EP_ALICE}, "alice", "DATA", subscribed(True),
     "row 63: a stored endpoint of the caller (decoded 62; v10.js 26408, 145259)"),
    ("POST", "/webpush/status", {"endpoint": EP_UNKNOWN}, "alice", "DATA", subscribed(False),
     "row 63: an endpoint the caller did not store (decoded 62)"),
    ("POST", "/webpush/status", {"endpoint": EP_ALICE}, None, "SIGNEDOUT", None, "row 63: signed out (decoded 62)"),
    ("POST", "/webpush/subscription-change", change_body(EP_ALICE), "alice", "DATA", is_change_ok,
     "row 64: the stored endpoint moves to the new one (decoded 3323; v10.js 26414, 145274)"),
    ("POST", "/webpush/status", {"endpoint": EP_ALICE2}, "alice", "DATA", subscribed(True),
     "row 64: after the change the new endpoint is stored (decoded 62)"),
    ("POST", "/webpush/status", {"endpoint": EP_ALICE}, "alice", "DATA", subscribed(False),
     "row 64: after the change the old endpoint is gone (decoded 62)"),
    ("POST", "/webpush/subscription-change", change_body(EP_UNKNOWN), "alice", "DATA", is_change_unknown,
     "row 64: an old endpoint the caller does not hold: success false (decoded 3323; v10.js 145274)"),
    ("POST", "/webpush/subscription-change", change_body(EP_ALICE2), None, "SIGNEDOUT", None,
     "row 64: signed out (decoded 3323)"),

    # ---- store spin: POST /spins/execute {index, ticket, multi} (decoded 5160; v10.js 27299, no v10 caller)
    ("POST", "/spins/execute", {"index": 0, "ticket": -1, "multi": False}, "dave", "DATA", is_spin,
     "row 187: the store spin of wheel 0 (Starter, 50 KR), the socket's spin op (decoded 5160; v10.js 27299)"),
    ("POST", "/spins/execute", {"index": 99999, "ticket": -1, "multi": False}, "dave", "BAD", None,
     "row 187: an index that is not a wheel (decoded 5160)"),
    ("POST", "/spins/execute", {"index": 0, "ticket": -1, "multi": False}, None, "SIGNEDOUT", None,
     "row 187: signed out (decoded 5160)"),

    # ---- KrunkCup wristband: KrunkNative has no wristband (decoded 4919, 4105; v10.js 27309, 27314)
    ("GET", "/krunkcup/wristband", None, None, "NONE", is_none,
     "row 188: no wristband in KrunkNative: the v10 none value (decoded 4919; v10.js 27309)"),
    ("POST", "/krunkcup/wristband/claim", {}, "alice", "REFUSAL", None,
     "row 189: a wristband claim KrunkNative cannot do (decoded 4105; v10.js 27314)"),
]



def run_route(port, method, path, body, caller, kind, pred, cite):
    label = "%s %s" % (method, path)
    if callable(body):
        body = body()
    tok = token(caller) if caller else None
    st, got = http(port, method, path, body, tok)
    if kind == "DATA":
        ok = st == 200 and isinstance(got, dict) and got.get("error") is None and pred(got.get("data"))
        check("%s -> 200 DATA (%s)" % (label, cite), ok, "HTTP %d %s" % (st, json.dumps(got)[:200]))
    elif kind == "NONE":
        ok = st == 200 and got == {"data": None, "error": None}
        check("%s -> 200 NONE (%s)" % (label, cite), ok, "HTTP %d %s" % (st, json.dumps(got)[:200]))
    elif kind == "REFUSAL":
        ok = st == 403 and error_code(got) == "NOT_IMPLEMENTED" and "data" not in (got or {})
        check("%s -> 403 REFUSAL NOT_IMPLEMENTED (%s)" % (label, cite), ok, "HTTP %d %s" % (st, json.dumps(got)[:200]))
    elif kind == "SIGNEDOUT":
        ok = st == 401 and error_code(got) == "LOGIN_REQUIRED"
        check("%s -> 401 LOGIN_REQUIRED (%s)" % (label, cite), ok, "HTTP %d %s" % (st, json.dumps(got)[:200]))
    elif kind == "BAD":
        ok = st == 400 and error_code(got) is not None and "data" not in (got or {})
        check("%s -> 400 BAD (%s)" % (label, cite), ok, "HTTP %d %s" % (st, json.dumps(got)[:200]))


def scenario(port):
    for method, path, body, caller, kind, pred, cite in ROUTES:
        run_route(port, method, path, body, caller, kind, pred, cite)


def stop(p):
    p.terminate()
    try:
        p.wait(timeout=5)
    except subprocess.TimeoutExpired:
        p.kill()


def main():
    if not os.path.isfile(BRAIN) or not os.access(BRAIN, os.X_OK):
        print("v10_api_account_test: no kn_brain at %s: skipped" % BRAIN)
        return 77
    r = subprocess.run([sys.executable, "-I", os.path.join(HERE, "db_scratch.py"), ROOT, "create", "sqlite", NAME],
                       capture_output=True)
    if r.returncode:
        print("v10_api_account_test: no sqlite scratch database: skipped")
        return 77
    cfg = json.loads(r.stdout)
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=SECRET,
               **{"KN_DATABASE_" + k.upper(): str(v) for k, v in cfg.items()})
    SAMPLE_KEYS["p256dh"] = b64url(65)
    SAMPLE_KEYS["auth"] = b64url(16)
    work = tempfile.mkdtemp(prefix="kn-v10-account-test.")
    log = os.path.join(work, "brain.log")
    brain = None
    try:
        mig = subprocess.run([BRAIN, "--migrate-only"], cwd=ROOT, env=env, capture_output=True)
        check("the migrations apply", mig.returncode == 0, mig.stderr.decode(errors="replace")[-200:])
        if mig.returncode:
            return 1
        seed(cfg["path"])
        check("the accounts, ranked, match, push and stats rows are seeded", True)
        brain = subprocess.Popen([BRAIN, "--brain.host=127.0.0.1", "--brain.port=0", "--brain.region=local",
                                  "--brain.id=v10-account-test-%d" % os.getpid(), "--finder.poll_interval_s=1",
                                  "--brain.ranked.season=%d" % SEASON],
                                 cwd=ROOT, env=env, stdout=open(log, "w"), stderr=subprocess.STDOUT,
                                 stdin=subprocess.DEVNULL, start_new_session=True)
        m = wait_for(lambda: re.search(r"listening on http://127\.0\.0\.1:(\d+) \(ws", read(log)), 20)
        check("kn_brain starts on an ephemeral port", m is not None, read(log)[-300:])
        if m is None:
            return 1
        scenario(int(m.group(1)))
    finally:
        if brain is not None:
            stop(brain)
        subprocess.run([sys.executable, "-I", os.path.join(HERE, "db_scratch.py"), ROOT, "drop", "sqlite", NAME],
                       capture_output=True)
    passed = sum(RESULTS)
    failed = len(RESULTS) - passed
    print("v10_api_account_test: %d PASS, %d FAIL" % (passed, failed))
    if failed:
        print("logs kept in " + work)
        return 1
    shutil.rmtree(work, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
