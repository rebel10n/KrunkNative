#!/usr/bin/env python3
"""The v10 REST routes with no KrunkNative data behind them (services/brain/v10_empty.c).

  v10_api_empty_test.py <kn_brain | build dir> [repo root]
  (CTest passes the kn_brain binary and the source root; a build dir means BUILD/services/brain/kn_brain.)

A scratch sqlite database (db_scratch.py), kn_brain --migrate-only, then kn_brain on an ephemeral port.
Each v10 route is one plain HTTP request on the brain root, checked against what v10.js's consumer
expects:

  EMPTY     200, {"data": <the empty shape its consumer reads>, "error": null}, exactly that data
  REFUSAL   403, {"error": {"code": "NOT_IMPLEMENTED", "message": ...}} (a write, or an account read)
  NOT_FOUND 404, {"error": {"code": "NOT_FOUND", "message": ...}}

Each check prints one PASS or FAIL line. Exit 0 when every check passes, 1 otherwise, 77 when kn_brain
or the sqlite scratch database is unavailable (CTest's skip).
"""
import json
import os
import re
import shutil
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
NAME = "kn_test_v10empty_%d" % os.getpid()
SECRET = "kn-v10-empty-test-secret-%d" % os.getpid()
RESULTS = []

# the EMPTY shapes (exact: no invented records)
E_MATCH = {"matchHistory": {"matches": [], "totalPages": 0}}   # v10.js 98633: matchHistory.matches / totalPages
E_RANK = {"leaderboard": [], "totalPages": 0}                 # v10.js 98600: leaderboard / totalPages

# (method, path, body, kind, expected data, v10.js line and why)
ROUTES = [
    # ---- api.krunker.io
    ("GET", "/webhooks/general/maps/config?mn=Burg", None, "bare", {},
     "131917, the legacy map preset: no da / fu is the menu's DEFAULT (presetLoaded 212624), no data envelope"),
    ("GET", "/match-history/me/season/1/gameMode/0/isRanked/all", None, "empty", E_MATCH,
     "25940, account match history; the consumer reads an empty history when signed out"),
    ("GET", "/match-history/me/season/1/gameMode/0/isRanked/all/region/us-ca-sv", None, "empty", E_MATCH,
     "25940 with region"),
    ("GET", "/player-profile/me/season/1", None, "refuse", None,
     "25959, the account's ranked profile (consumer 98579 catches the refusal)"),
    ("GET", "/player-profile/me/season/1/region/us-ca-sv", None, "refuse", None, "25959 with region"),
    ("GET", "/leaderboards/rankings/season/1", None, "empty", E_RANK, "25972, public ranking list (consumer 98600)"),
    ("GET", "/leaderboards/rankings/season/1/region/us-ca-sv", None, "empty", E_RANK, "25972 with region"),
    ("POST", "/player-profile/me/claim-milestone/region/us-ca-sv", {}, "refuse", None,
     "25976, claims a ranked milestone for the account"),
    ("GET", "/daily-spin/progress", None, "refuse", None,
     "25999, the account's daily spin (consumer 117993 would show a claimable zero state)"),
    ("POST", "/shop/cancel-subscription", {"subscriptionId": "x"}, "refuse", None, "26006, no payment backend"),
    ("POST", "/iap/checkout", {}, "refuse", None, "26018, no payment backend"),
    ("POST", "/iap/claim", {"channelId": "x", "productId": "x", "purchaseId": "x"}, "refuse", None,
     "26024, no payment backend"),
    ("POST", "/my-api/create-payment/1", {}, "refuse", None, "209123, PayPal, no payment backend"),
    ("POST", "/my-api/execute-payment/1", {}, "refuse", None, "209131, PayPal, no payment backend"),
    ("GET", "/ipinfo", None, "refuse", None,
     "16440, the IP checker URL (getter 19733 only; no v10.js call reads a body, no shape to match)"),
    # ---- gapi.svc.krunker.io: auth (the KN accounts use /auth/login and friends: auth.c, another contract)
    ("POST", "/auth/nonce", {}, "refuse", None, "26172, auth"),
    ("POST", "/auth/login/username", {"username": "x"}, "refuse", None, "26177, auth"),
    ("POST", "/auth/login/email", {"email": "x"}, "refuse", None, "26190, auth"),
    ("POST", "/auth/login/email-code", {"email": "x"}, "refuse", None, "26201, auth"),
    ("POST", "/auth/code-challenge/1", {"code": "x"}, "refuse", None, "26209, auth"),
    ("POST", "/auth/resend-code-challenge/1", {}, "refuse", None, "26217, auth"),
    ("POST", "/auth/migrate/1", {}, "refuse", None, "26222, auth"),
    ("POST", "/auth/2fa/challenge/1", {"code": "x"}, "refuse", None, "26230, auth"),
    ("POST", "/auth/verification/check/1", {}, "refuse", None, "26238, auth"),
    ("POST", "/auth/verification/resend/1", {}, "refuse", None, "26243, auth"),
    ("POST", "/auth/verification/change-email/1", {"email": "x"}, "refuse", None, "26248, auth"),
    ("POST", "/auth/register/email-code", {"email": "x"}, "refuse", None, "26256, auth"),
    ("POST", "/auth/username-challenge/1", {"username": "x"}, "refuse", None, "26264, auth"),
    ("POST", "/auth/password-challenge/1", {"password": "x"}, "refuse", None, "26272, auth"),
    ("POST", "/auth/recover-account", {"email": "x"}, "refuse", None, "26280, auth"),
    ("POST", "/auth/kid/age-challenge/1", {"age": 1}, "refuse", None, "26288, auth"),
    ("POST", "/auth/kid/parental-email/1", {"email": "x"}, "refuse", None, "26296, auth"),
    ("GET", "/auth/kid/consent-status/1", None, "refuse", None, "26304, auth"),
    # ---- gapi: player, account, email, discord, webpush
    ("GET", "/player/me", None, "refuse", None, "26312, signed-out or no mapping"),
    ("GET", "/player/wallet", None, "refuse", None, "26317, signed-out or no mapping"),
    ("POST", "/account/2fa/enable/start", {}, "refuse", None, "26326, account 2FA"),
    ("POST", "/account/2fa/enable/verify", {"code": "x"}, "refuse", None, "26331, account 2FA"),
    ("POST", "/account/2fa/disable", {"code": "x"}, "refuse", None, "26339, account 2FA"),
    ("GET", "/account/usernames", None, "refuse", None, "26347, account usernames"),
    ("POST", "/account/usernames/reserve", {"username": "x"}, "refuse", None, "26352, account usernames"),
    ("POST", "/account/usernames/swap", {"username": "x"}, "refuse", None, "26360, account usernames"),
    ("GET", "/email/preferences", None, "refuse", None,
     "26424, the account's mail preference (consumer 114573 reads null on refusal)"),
    ("POST", "/email/preferences", {"promotional": False, "source": "x"}, "refuse", None, "26429, a write"),
    ("GET", "/discord/status", None, "refuse", None,
     "26439, the account's Discord link (consumer 97112 has no catch; KN has no link)"),
    ("POST", "/discord/link", {}, "refuse", None, "26444, Discord link"),
    ("POST", "/discord/join", {}, "refuse", None, "26452, Discord join"),
    ("POST", "/discord/unlink", {}, "refuse", None, "26457, Discord unlink"),
    ("POST", "/webpush/status", {"endpoint": "x"}, "refuse", None,
     "26408, needs the subscription (consumer 145259 reads is_subscribed; a false would drop the local one)"),
    ("POST", "/webpush/subscription-change", {"old_subscription": {}, "new_subscription": {}}, "refuse", None,
     "26414, needs a stored subscription (consumer 145274 reads success)"),
    # ---- gapi: KrunkCup wristband (no client call site reads it; account-scoped)
    ("GET", "/krunkcup/wristband", None, "refuse", None, "27309, the account's wristband"),
    ("POST", "/krunkcup/wristband/claim", {}, "refuse", None, "27314, a write"),
    # ---- gapi: spins (the spin itself is the social socket's op)
    ("POST", "/spins/execute", {}, "refuse", None, "27299, a spin is the social socket's op"),
]


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
    headers = {"Content-Type": "application/json"} if data is not None else {}
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


def run_route(port, method, path, body, kind, expected, cite):
    label = "%s %s" % (method, path)
    line = cite.split(",")[0].split(" ")[0]  # the v10.js line of the client call
    st, got = http(port, method, path, body)
    if kind == "empty":
        want = {"data": expected, "error": None}
        check("%s -> 200 EMPTY %s (v10.js %s)" % (label, json.dumps(expected, separators=(",", ":")), line),
              st == 200 and got == want, "HTTP %d %s" % (st, json.dumps(got)[:160]))
    elif kind == "bare":
        check("%s -> 200 EMPTY bare %s (v10.js %s)" % (label, json.dumps(expected, separators=(",", ":")), line),
              st == 200 and got == expected, "HTTP %d %s" % (st, json.dumps(got)[:160]))
    elif kind == "refuse":
        ok = st == 403 and error_code(got) == "NOT_IMPLEMENTED" and "data" not in (got or {})
        check("%s -> 403 REFUSAL NOT_IMPLEMENTED (v10.js %s)" % (label, line), ok,
              "HTTP %d %s" % (st, json.dumps(got)[:160]))
    elif kind == "notfound":
        ok = st == 404 and error_code(got) == "NOT_FOUND"
        check("%s -> 404 NOT_FOUND (v10.js %s)" % (label, line), ok, "HTTP %d %s" % (st, json.dumps(got)[:160]))


def scenario(port):
    for method, path, body, kind, expected, cite in ROUTES:
        run_route(port, method, path, body, kind, expected, cite)


def stop(p):
    p.terminate()
    try:
        p.wait(timeout=5)
    except subprocess.TimeoutExpired:
        p.kill()


def main():
    if not os.path.isfile(BRAIN) or not os.access(BRAIN, os.X_OK):
        print("v10_api_empty_test: no kn_brain at %s: skipped" % BRAIN)
        return 77
    r = subprocess.run([sys.executable, "-I", os.path.join(HERE, "db_scratch.py"), ROOT, "create", "sqlite", NAME],
                       capture_output=True)
    if r.returncode:
        print("v10_api_empty_test: no sqlite scratch database: skipped")
        return 77
    cfg = json.loads(r.stdout)
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=SECRET,
               **{"KN_DATABASE_" + k.upper(): str(v) for k, v in cfg.items()})
    work = tempfile.mkdtemp(prefix="kn-v10-empty-test.")
    log = os.path.join(work, "brain.log")
    brain = None
    try:
        mig = subprocess.run([BRAIN, "--migrate-only"], cwd=ROOT, env=env, capture_output=True)
        check("the migrations apply", mig.returncode == 0, mig.stderr.decode(errors="replace")[-200:])
        if mig.returncode:
            return 1
        brain = subprocess.Popen([BRAIN, "--brain.host=127.0.0.1", "--brain.port=0", "--brain.region=local",
                                  "--brain.id=v10-empty-test-%d" % os.getpid(), "--finder.poll_interval_s=1"],
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
    print("v10_api_empty_test: %d PASS, %d FAIL" % (passed, failed))
    if failed:
        print("logs kept in " + work)
        return 1
    shutil.rmtree(work, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
