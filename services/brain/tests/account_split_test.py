#!/usr/bin/env python3
"""The account split on kn_brain (doc/v10/account-split.md): a krunker.io token is no Box Office session.

  account_split_test.py <kn_brain | build dir> [repo root]
  (CTest passes the kn_brain binary and the source root; a build dir means BUILD/services/brain/kn_brain.)

A scratch sqlite database (db_scratch.py), kn_brain --migrate-only, then kn_brain on an ephemeral port. A Box Office
account is registered over POST /auth/register: its session token is the control (it works on /auth/me in every
transport). A krunker.io access token is a JWT (krunker_account.h: three base64url segments, the FRVR issuer), so the
test builds one of that shape with the same issuer and sends it in each transport the brain reads a session from:
the Authorization Bearer header, ?token= and the JSON body "token" (auth.c req_token). The session routes refuse it:
/auth/me, /auth/refresh and /auth/join (the same lookup serves the others: accounts.c token_lookup_done).

Each check prints one PASS or FAIL line. Exit 0 when every check passes, 1 otherwise, 77 when kn_brain or the scratch
database is unavailable (CTest's skip).
"""
import base64
import json
import os
import re
import secrets
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
NAME = "kns%d" % (os.getpid() % 1000000)
PASS = "split_pass_word1"  # a fixture password, sent by this test only
SECRET = "kn-split-test-secret-%d" % os.getpid()
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


def b64url(data):
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode()


def krunker_token():
    """A krunker.io access token's shape: a JWT (header.payload.signature) from FRVR's auth issuer, signed by no key
    the brain holds. Nothing here is a credential: it is built for this test."""
    head = b64url(json.dumps({"alg": "RS256", "typ": "JWT"}).encode())
    body = b64url(json.dumps({"iss": "https://crucible.frvr.com/v1/auth", "sub": "krunker-%d" % os.getpid(),
                              "exp": int(time.time()) + 3600, "scope": "krunker"}).encode())
    return "%s.%s.%s" % (head, body, b64url(secrets.token_bytes(64)))


def http(port, method, path, body=None, headers=None):
    data = json.dumps(body).encode() if body is not None else None
    hdrs = dict(headers or {})
    if data is not None:
        hdrs["Content-Type"] = "application/json"
    req = urllib.request.Request("http://127.0.0.1:%d%s" % (port, path), data=data, method=method, headers=hdrs)
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            status, raw = r.status, r.read()
    except urllib.error.HTTPError as e:
        status, raw = e.code, e.read()
    try:
        return status, json.loads(raw or b"null")
    except ValueError:
        return status, None


def accepted(reply):
    """A session route accepted the token: 200 with ok true."""
    status, body = reply
    return status == 200 and isinstance(body, dict) and body.get("ok") is True


def routes_for(token, port):
    """The session routes, each with the token in one transport: the Bearer header, ?token=, the JSON body."""
    bearer = {"Authorization": "Bearer " + token}
    return [
        ("GET /auth/me (Bearer)", lambda: http(port, "GET", "/auth/me", headers=bearer)),
        ("GET /auth/me (?token=)", lambda: http(port, "GET", "/auth/me?token=" + token)),
        ("POST /auth/refresh (JSON)", lambda: http(port, "POST", "/auth/refresh", {"token": token})),
        ("POST /auth/refresh (Bearer)", lambda: http(port, "POST", "/auth/refresh", headers=bearer)),
        ("POST /auth/join (JSON)", lambda: http(port, "POST", "/auth/join", {"token": token, "serverHash": "x" * 16})),
    ]


def main():
    if not os.access(BRAIN, os.X_OK):
        print("account_split_test: no kn_brain at %s: skipped" % BRAIN)
        return 77
    if DRIVER != "sqlite":
        print("account_split_test: runs on sqlite (the scratch database is a file): skipped")
        return 77
    r = subprocess.run([sys.executable, "-I", os.path.join(HERE, "db_scratch.py"), ROOT, "create", "sqlite", NAME],
                       capture_output=True)
    if r.returncode:
        print("account_split_test: no sqlite scratch database: skipped")
        return 77
    dbinfo = json.loads(r.stdout)
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=SECRET, **{"KN_DATABASE_" + k.upper(): str(v) for k, v in dbinfo.items()})
    work = tempfile.mkdtemp(prefix="kn-split-test.")
    log = os.path.join(work, "brain.log")
    proc = None
    try:
        mig = subprocess.run([BRAIN, "--migrate-only"], cwd=ROOT, env=env, capture_output=True)
        check("the migrations apply", mig.returncode == 0, mig.stderr.decode(errors="replace")[-200:])
        if mig.returncode:
            return 1
        proc = subprocess.Popen([BRAIN, "--brain.host=127.0.0.1", "--brain.port=0", "--brain.region=local",
                                 "--brain.id=split-test-%d" % os.getpid(), "--finder.poll_interval_s=1"],
                                cwd=ROOT, env=env, stdout=open(log, "w"), stderr=subprocess.STDOUT,
                                stdin=subprocess.DEVNULL, start_new_session=True)
        m = None
        for _ in range(200):
            m = re.search(r"listening on http://127\.0\.0\.1:(\d+) \(ws", read(log))
            if m or proc.poll() is not None:
                break
            time.sleep(0.1)
        check("kn_brain starts on an ephemeral port", m is not None, read(log)[-300:])
        if m is None:
            return 1
        port = int(m.group(1))

        status, body = http(port, "POST", "/auth/register", {"username": NAME, "password": PASS})
        token = body.get("token") if isinstance(body, dict) else None
        check("a Box Office account registers (the control's session)", status == 200 and bool(token), "status %s %s" % (status, body))
        if not token:
            return 1
        check("the Box Office session token is accepted on /auth/me (control)",
              accepted(http(port, "GET", "/auth/me", headers={"Authorization": "Bearer " + token})))

        kt = krunker_token()
        for label, call in routes_for(kt, port):
            check("a krunker.io token is refused: %s" % label, not accepted(call()), repr(call()[:1]))

        # the Box Office session survives the krunker.io token's attempts: the same token still works
        check("the Box Office session still works after the krunker.io attempts",
              accepted(http(port, "GET", "/auth/me", headers={"Authorization": "Bearer " + token})))
    finally:
        if proc is not None:
            proc.terminate()
            try:
                proc.wait(10)
            except subprocess.TimeoutExpired:
                proc.kill()
        subprocess.run([sys.executable, "-I", os.path.join(HERE, "db_scratch.py"), ROOT, "drop", "sqlite", NAME],
                       capture_output=True)
    ok = bool(RESULTS) and all(RESULTS)
    print("account_split_test: " + ("OK" if ok else "FAILED") + " (%d checks)" % len(RESULTS))
    if ok:
        shutil.rmtree(work, ignore_errors=True)
    else:
        print("logs kept in " + work)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
