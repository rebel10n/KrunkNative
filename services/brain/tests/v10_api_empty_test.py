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
E_SKINS = {"skins": []}                                       # v10.js 137427: skins
E_OFFER = {"offer": None}                                     # v10.js 126666: offer (null = no offer)
E_LIST = []                                                   # v10.js 107087, 106987: data || []
E_MAPLB = {"entries": [], "total": 0}                         # v10.js 106970: entries / total

# (method, path, body, kind, expected data, v10.js line and why)
ROUTES = [
    # ---- api.krunker.io
    ("GET", "/webhooks/general/maps/config?mn=Burg", None, "bare", {},
     "131917, the legacy map preset: no da / fu is the menu's DEFAULT (presetLoaded 212624), no data envelope"),
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
    # ---- gapi: skins
    ("GET", "/skins", None, "empty", E_SKINS, "26474, skin list (consumer 137427 reads skins)"),
    ("GET", "/skins/1", None, "notfound", None, "26479, KN holds no skin record for this id"),
    ("POST", "/skins", {}, "refuse", None, "26484, a write"),
    ("PUT", "/skins/1", {}, "refuse", None, "26490, a write"),
    ("DELETE", "/skins/1", None, "refuse", None, "26496, a write"),
    ("POST", "/skins/1/upload", {}, "refuse", None, "26505, a write"),
    ("POST", "/skins/1/duplicate", {}, "refuse", None, "26511, a write"),
    # ---- gapi: shop wild offer
    ("GET", "/shop/wild-offer", None, "empty", E_OFFER, "26539, no offer (consumer 126666 reads offer)"),
    ("POST", "/shop/wild-offer/purchase", {}, "refuse", None, "26544, a write"),
    ("POST", "/shop/wild-offer/refresh", {}, "refuse", None, "26553, a write"),
    # ---- gapi: maps (GET /maps, /maps/:id and /maps/by-name/:name are v10_api.c's)
    ("GET", "/maps/1/versions", None, "empty", E_LIST, "26626, version list (consumer 107087 reads data || [])"),
    ("POST", "/maps/1/versions/2/revert", {}, "refuse", None, "26632, a write"),
    ("POST", "/maps/1/funds", {"amount": 1}, "refuse", None, "26637, a write"),
    ("POST", "/maps/1/leaderboard", {"type": "score"}, "refuse", None, "26645, a write"),
    ("GET", "/maps/1/data?player=someone", None, "empty", E_LIST,
     "26655, the player's map data (consumer 106987 reads data || [])"),
    ("DELETE", "/maps/1/data?key=k", None, "refuse", None, "26664, a write"),
    ("GET", "/maps/by-name/Sandstorm/leaderboard?page=1", None, "empty", E_MAPLB,
     "26602, map leaderboard (consumer 106970 reads entries / total)"),
    ("POST", "/maps/1/vote", {"vote": 1}, "refuse", None, "26605, a write"),
    ("POST", "/maps/1/feature", {}, "refuse", None, "26613, a write"),
    ("DELETE", "/maps/1", None, "refuse", None, "26621, a write"),
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
