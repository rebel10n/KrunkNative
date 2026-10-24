#!/usr/bin/env python3
"""Box Office's captcha on a running kn_brain (services/brain/captcha.h, doc/v10/captcha.md): the challenge route,
and every flow it gates, with captcha.enforce on and off.

  captcha_test.py <kn_brain | build dir> [repo root]
  (CTest passes the kn_brain binary and the source root; a build dir means BUILD/services/brain/kn_brain.)

A sqlite database in the run's directory (~/.cache), kn_brain --migrate-only, then kn_brain on an ephemeral port. The
challenges are solved here the way the ALTCHA 1.0.6 widget's worker does (libs/altcha.1.0.6.min.js: the number
whose SHA-256(salt + number) is the challenge, the payload base64(JSON {algorithm, challenge, number, salt,
signature, took})), which is also what the client's krunker_altcha.c sends (client/tests/kn_captcha_test.c).

  enforce on:   GET /captcha/challenge (shape, CORS, purposes); POST /auth/register and /auth/login {captcha};
                POST /auth/login/username and /auth/login/email {b} (v10.0.3's field); the social socket's
                ["a", action, payload, captcha]; GET /mm/host-game ?captcha=. Missing -> refused with v10.0.3's
                texts / error names, solved -> through, the same payload again (a replay) -> refused, another
                flow's payload -> refused, a forged one -> refused; a saved session's resume needs none
  enforce off:  (the default) the same database, a second brain: every flow goes through without a captcha

Each check prints one PASS or FAIL line; the last line counts them. Exit 0 when every check passes, 1 otherwise,
77 when kn_brain (or its sqlite driver) is unavailable (CTest's skip). The social socket checks need the
python websockets module (skipped, with a line, without it).
"""
import asyncio
import base64
import hashlib
import json
import os
import re
import secrets
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ARG = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
ROOT = os.path.abspath(sys.argv[2]) if len(sys.argv) > 2 else os.path.abspath(os.path.join(HERE, "..", "..", ".."))
BRAIN = os.path.join(ARG, "services", "brain", "kn_brain") if os.path.isdir(ARG) else ARG
SECRET = "kn-captcha-test-secret-%d" % os.getpid()
PASSWORD = "Passw0rd-kn-test!"
MISSING = "Please complete the security challenge to continue."  # deob-10.0.3.js:111601
FAILED = "CAPTCHA FAILED"                                         # deob-10.0.3.js:111430
RESULTS = []

try:
    import websockets  # noqa: F401
except ImportError:
    websockets = None


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
    """(status, json body, headers)"""
    data = json.dumps(body).encode() if body is not None else None
    headers = {"Accept": "application/json", "Origin": "http://127.0.0.1:%d" % port}
    if data is not None:
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request("http://127.0.0.1:%d%s" % (port, path), data=data, method=method, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=15) as r:
            status, raw, hdrs = r.status, r.read(), dict(r.headers)
    except urllib.error.HTTPError as e:
        status, raw, hdrs = e.code, e.read(), dict(e.headers)
    try:
        return status, json.loads(raw or b"null"), hdrs
    except ValueError:
        return status, None, hdrs


def challenge(port, purpose):
    st, body, _ = http(port, "GET", "/captcha/challenge?for=" + purpose)
    return body if st == 200 else None


def solve(ch):
    """the widget worker's search (altcha.1.0.6.min.js b()): the number in [0, maxnumber] whose hash is the challenge"""
    for n in range(int(ch["maxnumber"]) + 1):
        if hashlib.sha256((ch["salt"] + str(n)).encode()).hexdigest() == ch["challenge"]:
            value = {"algorithm": ch["algorithm"], "challenge": ch["challenge"], "number": n, "salt": ch["salt"],
                     "signature": ch["signature"], "took": 5}
            return base64.b64encode(json.dumps(value, separators=(",", ":")).encode()).decode()
    return None


def solved(port, purpose):
    ch = challenge(port, purpose)
    return solve(ch) if ch else None


def forged(port):
    """a payload this brain did not sign: our own challenge, the right number, a made-up signature"""
    ch = challenge(port, "login")
    ch["signature"] = hashlib.sha256(b"not the key").hexdigest()
    return solve(ch)


def fresh_user(tag):
    return "cpt%s%04d" % (tag, secrets.randbelow(10000))


def text_error(body):
    return body.get("error") if isinstance(body, dict) else None


# ---- the checks

def challenge_checks(port):
    st, body, hdrs = http(port, "GET", "/captcha/challenge?for=login")
    keys = sorted(body.keys()) if isinstance(body, dict) else []
    check("GET /captcha/challenge?for=login -> 200 {algorithm, challenge, maxnumber, salt, signature} (altcha-lib v1)",
          st == 200 and keys == ["algorithm", "challenge", "maxnumber", "salt", "signature"] and body["algorithm"] == "SHA-256"
          and re.fullmatch(r"[0-9a-f]{64}", body["challenge"]) and re.fullmatch(r"[0-9a-f]{64}", body["signature"])
          and body["maxnumber"] == 20000, "HTTP %d %s" % (st, json.dumps(body)[:200]))
    m = re.fullmatch(r"[0-9a-f]{24}\?expires=(\d+)&for=login", body.get("salt", "") if isinstance(body, dict) else "")
    check("its salt carries ?expires=<ttl from now>&for=login", m and abs(int(m.group(1)) - (time.time() + 300)) < 30,
          body.get("salt") if isinstance(body, dict) else "")
    check("the route answers CORS and no-store", hdrs.get("Access-Control-Allow-Origin") and "no-store" in hdrs.get("Cache-Control", ""),
          repr({k: v for k, v in hdrs.items() if k.lower().startswith(("access", "cache"))}))
    for purpose in ("register", "hosting"):
        st, body, _ = http(port, "GET", "/captcha/challenge?for=" + purpose)
        check("GET /captcha/challenge?for=%s -> 200" % purpose, st == 200 and body["salt"].endswith("&for=" + purpose))
    for bad in ("", "seek", "login%26for%3Dhosting"):
        st, body, _ = http(port, "GET", "/captcha/challenge?for=" + bad)
        check("GET /captcha/challenge?for=%s -> 400 BadRequest" % (bad or "(none)"), st == 400 and text_error(body) == "BadRequest",
              "HTTP %d %s" % (st, json.dumps(body)[:120]))
    a, b = challenge(port, "login"), challenge(port, "login")
    check("every challenge is a new one", a and b and a["challenge"] != b["challenge"] and a["salt"] != b["salt"])


def http_auth_checks(port, db_path):
    # POST /auth/register {username, password, captcha}
    name = fresh_user("r")
    st, body, _ = http(port, "POST", "/auth/register", {"username": name, "password": PASSWORD})
    check("POST /auth/register without a captcha -> 400 '%s'" % MISSING, st == 400 and text_error(body) == MISSING,
          "HTTP %d %s" % (st, json.dumps(body)[:160]))
    st, body, _ = http(port, "POST", "/auth/register", {"username": name, "password": PASSWORD, "captcha": solved(port, "login")})
    check("POST /auth/register with a login challenge's payload -> 400 '%s'" % FAILED, st == 400 and text_error(body) == FAILED,
          "HTTP %d %s" % (st, json.dumps(body)[:160]))
    reg = solved(port, "register")
    st, body, _ = http(port, "POST", "/auth/register", {"username": name, "password": PASSWORD, "captcha": reg})
    check("POST /auth/register with a solved register challenge -> 200 ok", st == 200 and isinstance(body, dict) and body.get("ok"),
          "HTTP %d %s" % (st, json.dumps(body)[:160]))
    st, body, _ = http(port, "POST", "/auth/register", {"username": fresh_user("x"), "password": PASSWORD, "captcha": reg})
    check("the same payload again (a replay) -> 400 '%s'" % FAILED, st == 400 and text_error(body) == FAILED,
          "HTTP %d %s" % (st, json.dumps(body)[:160]))

    # POST /auth/login {username, password, captcha}
    st, body, _ = http(port, "POST", "/auth/login", {"username": name, "password": PASSWORD})
    check("POST /auth/login without a captcha -> 400", st == 400 and text_error(body) == MISSING, "HTTP %d %s" % (st, json.dumps(body)[:160]))
    st, body, _ = http(port, "POST", "/auth/login", {"username": name, "password": PASSWORD, "captcha": forged(port)})
    check("POST /auth/login with a forged signature -> 400 '%s'" % FAILED, st == 400 and text_error(body) == FAILED,
          "HTTP %d %s" % (st, json.dumps(body)[:160]))
    st, body, _ = http(port, "POST", "/auth/login", {"username": name, "password": PASSWORD, "captcha": solved(port, "login")})
    check("POST /auth/login with a solved login challenge -> 200 ok", st == 200 and isinstance(body, dict) and body.get("ok"),
          "HTTP %d %s" % (st, json.dumps(body)[:160]))
    token = body.get("token") if isinstance(body, dict) else None
    st, body, _ = http(port, "POST", "/auth/refresh", {"token": token})
    check("POST /auth/refresh (a session, no password) needs no captcha", st == 200 and isinstance(body, dict) and body.get("ok"),
          "HTTP %d" % st)

    # v10's POST /auth/login/username {username, password, a?, b?} and /auth/login/email {email, password, b?}
    st, body, _ = http(port, "POST", "/auth/login/username", {"username": name, "password": PASSWORD})
    check("POST /auth/login/username without b -> 400 {data: null, error: '%s'}" % MISSING,
          st == 400 and isinstance(body, dict) and body.get("data") is None and body.get("error") == MISSING,
          "HTTP %d %s" % (st, json.dumps(body)[:160]))
    pl = solved(port, "login")
    st, body, _ = http(port, "POST", "/auth/login/username", {"username": name, "password": PASSWORD, "a": "", "b": pl})
    data = body.get("data") if isinstance(body, dict) else None
    check("POST /auth/login/username with b = a solved challenge -> 200 login_ok",
          st == 200 and isinstance(data, dict) and data.get("type") == "login_ok" and data.get("access_token"),
          "HTTP %d %s" % (st, json.dumps(body)[:160]))
    st, body, _ = http(port, "POST", "/auth/login/username", {"username": name, "password": PASSWORD, "b": pl})
    check("the same b again -> 400 '%s'" % FAILED, st == 400 and isinstance(body, dict) and body.get("error") == FAILED,
          "HTTP %d %s" % (st, json.dumps(body)[:160]))
    st, body, _ = http(port, "POST", "/auth/login/username", {"username": name, "password": "wrong-password-1", "b": solved(port, "login")})
    check("a solved challenge with a wrong password -> 401 (the captcha does not sign in by itself)", st == 401, "HTTP %d" % st)
    email = "%s@example.test" % name.lower()
    con = sqlite3.connect(db_path, timeout=10)
    con.execute("UPDATE accounts SET email = ?, email_verified_at = ? WHERE username = ?",
                (email, time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime()) + ".000", name))
    con.commit()
    con.close()
    st, body, _ = http(port, "POST", "/auth/login/email", {"email": email, "password": PASSWORD})
    check("POST /auth/login/email without b -> 400", st == 400 and isinstance(body, dict) and body.get("error") == MISSING,
          "HTTP %d %s" % (st, json.dumps(body)[:160]))
    st, body, _ = http(port, "POST", "/auth/login/email", {"email": email, "password": PASSWORD, "b": solved(port, "login")})
    data = body.get("data") if isinstance(body, dict) else None
    check("POST /auth/login/email with b -> 200 login_ok", st == 200 and isinstance(data, dict) and data.get("type") == "login_ok",
          "HTTP %d %s" % (st, json.dumps(body)[:160]))
    return name, token


def host_checks(port, token):
    cfg = urllib.parse.quote(json.dumps({"maps": [0], "modes": [0]}))
    base = "/mm/host-game?region=local&config=%s&token=%s" % (cfg, urllib.parse.quote(token or ""))
    st, body, _ = http(port, "GET", base)
    check("GET /mm/host-game without a captcha -> 400 CaptchaRequired (v10.0.3's matchmaker error)",
          st == 400 and text_error(body) == "CaptchaRequired", "HTTP %d %s" % (st, json.dumps(body)[:160]))
    st, body, _ = http(port, "GET", base + "&captcha=" + urllib.parse.quote(solved(port, "login")))
    check("GET /mm/host-game with a login challenge's payload -> 400 RecaptchaFailed",
          st == 400 and text_error(body) == "RecaptchaFailed", "HTTP %d %s" % (st, json.dumps(body)[:160]))
    pl = solved(port, "hosting")
    st, body, _ = http(port, "GET", base + "&captcha=" + urllib.parse.quote(pl))
    check("GET /mm/host-game with a solved hosting challenge gets past the captcha (no server here: its own answer)",
          text_error(body) not in ("CaptchaRequired", "RecaptchaFailed"), "HTTP %d %s" % (st, json.dumps(body)[:160]))
    st, body, _ = http(port, "GET", base + "&captcha=" + urllib.parse.quote(pl))
    check("the same hosting payload again -> 400 RecaptchaFailed", st == 400 and text_error(body) == "RecaptchaFailed",
          "HTTP %d %s" % (st, json.dumps(body)[:160]))
    st, body, _ = http(port, "POST", "/mm/host-game?token=" + urllib.parse.quote(token or ""),
                       {"region": "local", "config": {"maps": [0]}, "captcha": solved(port, "hosting")})
    check("POST /mm/host-game {captcha} gets past the captcha", text_error(body) not in ("CaptchaRequired", "RecaptchaFailed"),
          "HTTP %d %s" % (st, json.dumps(body)[:160]))


async def social_checks(port, name):
    url = "ws://127.0.0.1:%d/social/ws?enc=json" % port

    async def recv(ws, label, timeout=10.0):
        end = time.time() + timeout
        while True:
            left = end - time.time()
            if left <= 0:
                raise AssertionError("timeout waiting for %r" % label)
            f = json.loads(await asyncio.wait_for(ws.recv(), left))
            if f and f[0] == label:
                return f

    async with websockets.connect(url) as ws:
        await ws.send(json.dumps(["a", 1, [name, PASSWORD, None]]))
        f = await recv(ws, "a")
        check("social ['a', 1, [name, pass]] without a captcha -> ['a', '%s']" % MISSING, f[1] == MISSING, repr(f)[:160])
        await ws.send(json.dumps(["a", 1, [name, PASSWORD, None], True]))  # v7.1.4's hub sent true
        f = await recv(ws, "a")
        check("social login with v7's 'true' in the captcha slot -> refused (no payload)", f[1] == MISSING, repr(f)[:160])
        await ws.send(json.dumps(["a", 1, [name, PASSWORD, None], solved(port, "register")]))
        f = await recv(ws, "a")
        check("social login with a register challenge's payload -> ['a', '%s']" % FAILED, f[1] == FAILED, repr(f)[:160])
        await ws.send(json.dumps(["a", 1, [name, PASSWORD, None], solved(port, "login")]))
        f = await recv(ws, "a")
        check("social login with a solved login challenge -> signed in (['a', null, id, name, ...])",
              f[1] is None and f[3] == name and isinstance(f[5], str) and f[5], repr(f)[:160])
        token = f[5] if len(f) > 5 else None
    async with websockets.connect(url) as ws:
        await ws.send(json.dumps(["a", 1, [name, None, token]]))
        f = await recv(ws, "a")
        check("social resume of a saved session ([name, null, token]) needs no captcha", f[1] is None and f[3] == name, repr(f)[:160])
    async with websockets.connect(url) as ws:
        new = fresh_user("s")
        await ws.send(json.dumps(["a", 0, [new, PASSWORD, None]]))
        f = await recv(ws, "a")
        check("social register without a captcha -> refused", f[1] == MISSING, repr(f)[:160])
        await ws.send(json.dumps(["a", 0, [new, PASSWORD, None], solved(port, "register")]))
        f = await recv(ws, "a")
        check("social register with a solved register challenge -> registered and signed in", f[1] is None and f[3] == new, repr(f)[:160])


def enforce_off_checks(port, name, log):
    st, body, _ = http(port, "POST", "/auth/login", {"username": name, "password": PASSWORD})
    check("captcha.enforce off: POST /auth/login without a captcha -> 200", st == 200 and isinstance(body, dict) and body.get("ok"),
          "HTTP %d %s" % (st, json.dumps(body)[:160]))
    st, body, _ = http(port, "POST", "/auth/login/username", {"username": name, "password": PASSWORD, "b": "garbage"})
    data = body.get("data") if isinstance(body, dict) else None
    check("captcha.enforce off: a bad b is let through too", st == 200 and isinstance(data, dict) and data.get("type") == "login_ok",
          "HTTP %d %s" % (st, json.dumps(body)[:160]))
    st, body, _ = http(port, "POST", "/auth/register", {"username": fresh_user("o"), "password": PASSWORD})
    check("captcha.enforce off: POST /auth/register without a captcha -> 200", st == 200 and isinstance(body, dict) and body.get("ok"),
          "HTTP %d" % st)
    check("captcha.enforce off: the challenge route still answers (clients solve before any brain enforces)",
          challenge(port, "login") is not None)
    st, body, _ = http(port, "POST", "/auth/login", {"username": name, "password": PASSWORD, "captcha": solved(port, "login")})
    check("captcha.enforce off: a solved one is accepted as well", st == 200, "HTTP %d" % st)
    check("captcha.enforce off: the brain logs what enforcement would refuse (DEBUG)",
          wait_for(lambda: re.search(r"login from \S+: captcha missing \(captcha\.enforce off: let through\)", read(log)), 5)
          and re.search(r"login from \S+: captcha accepted", read(log)), "; ".join(re.findall(r".*captcha (?:missing|accepted).*", read(log))[:3]))
    check("captcha.enforce off: the startup line says so", "captcha challenges issued, not enforced" in read(log))


def start_brain(env, extra, log):
    """kn_brain on an ephemeral port: (process, port), port None when it did not start"""
    brain = subprocess.Popen([BRAIN, "--brain.host=127.0.0.1", "--brain.port=0", "--brain.region=local",
                              "--brain.id=captcha-test-%d" % os.getpid(), "--finder.poll_interval_s=1"] + extra,
                             cwd=ROOT, env=env, stdout=open(log, "w"), stderr=subprocess.STDOUT,
                             stdin=subprocess.DEVNULL, start_new_session=True)
    m = wait_for(lambda: re.search(r"listening on http://127\.0\.0\.1:(\d+) \(ws", read(log)), 20)
    check("kn_brain starts on an ephemeral port", m is not None, read(log)[-300:])
    return brain, (int(m.group(1)) if m else None)


def stop_brain(brain):
    if brain is not None and brain.poll() is None:
        brain.terminate()
        try:
            brain.wait(timeout=5)
        except subprocess.TimeoutExpired:
            brain.kill()


def main():
    if not os.path.isfile(BRAIN) or not os.access(BRAIN, os.X_OK):
        print("captcha_test: no kn_brain at %s: skipped" % BRAIN)
        return 77
    cache = os.path.expanduser("~/.cache")
    work = tempfile.mkdtemp(prefix="kn-captcha-test.", dir=cache if os.path.isdir(cache) else None)
    db_path = os.path.join(work, "coordinator.db")
    db_args = ["--database.driver=sqlite", "--database.path=" + db_path]
    brain = None
    try:
        env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=SECRET)
        mig = subprocess.run([BRAIN, "--migrate-only"] + db_args, cwd=ROOT, env=env, capture_output=True)
        if mig.returncode and b"sqlite" in mig.stderr.lower() and b"driver" in mig.stderr.lower():
            print("captcha_test: no sqlite driver in this kn_brain: skipped")
            return 77
        check("the migrations apply", mig.returncode == 0, mig.stderr.decode(errors="replace")[-200:])
        if mig.returncode:
            return 1
        extra = db_args + ["--auth.login_per_min=100000", "--auth.register_per_hour=100000", "--auth.join_per_min=100000",
                           "--accounts.login_throttle_s=0", "--captcha.max_number=20000"]
        log = os.path.join(work, "brain-enforced.log")
        brain, port = start_brain(env, extra + ["--captcha.enforce=true", "--debug"], log)  # --debug: the INFO refusals
        if port is None:
            return 1
        check("the startup line says captcha is enforced", "captcha enforced" in read(log), read(log)[-300:])
        challenge_checks(port)
        name, token = http_auth_checks(port, db_path)
        host_checks(port, token)
        if websockets is None:
            print("captcha_test: python websockets missing: the social socket checks are skipped")
        else:
            asyncio.run(social_checks(port, name))
        refused = len(re.findall(r"refused: captcha", read(log)))
        check("each refusal is logged (INFO)", refused >= 8, "%d lines" % refused)
        stop_brain(brain)
        log = os.path.join(work, "brain-off.log")
        brain, port = start_brain(env, extra + ["--debug=2"], log)
        if port is None:
            return 1
        enforce_off_checks(port, name, log)
    except Exception as e:  # noqa: BLE001
        check("the scenario runs", False, "exception: %r" % e)
    finally:
        stop_brain(brain)
    passed = sum(RESULTS)
    failed = len(RESULTS) - passed
    print("captcha_test: %d PASS, %d FAIL" % (passed, failed))
    if failed:
        print("logs kept in " + work)
        return 1
    shutil.rmtree(work, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
