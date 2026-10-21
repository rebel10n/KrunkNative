#!/usr/bin/env python3
"""The v10 /auth, /account 2FA and usernames, /email, /discord routes (services/brain/v10_auth.c,
account_security.c). Family f1b of doc/v10/api-inventory.md, rows 32-49,
52-57, 65-70.

  v10_api_auth_test.py <kn_brain | build dir> [repo root]
  (CTest passes the kn_brain binary and the source root; a build dir means BUILD/services/brain/kn_brain.)

A scratch sqlite database (db_scratch.py), kn_brain --migrate-only, then kn_brain on an ephemeral port
with the fake SMTP server of services/common/tests/fake_smtp.py (the recover-account mail). Each check
is one HTTP request on the brain root, checked against what the official v10.0.3 consumer reads
(line numbers: v10.0.3.decoded.js, the decoded bundle; v10.js, the v10.0.2 bundle the inventory uses):

  LOGIN   200 {"data": {"type": "login_ok", "access_token", "refresh_token"}, "error": null}, the token is
          a real session (GET /auth/me answers it)
  CHECK   200 {"data": {"type": "check_2fa", "challenge_id"}, "error": null}: the 2FA prompt
  ERROR   a 4xx status, {"data": null, "error": "<text>"} (the auth consumers throw the text)
  NONE    404 {"data": null, "error": "<text>"}: a flow Box Office does not have (no challenge exists)
  EMPTY   200 with the empty shape the consumer reads
  REFUSAL 403 {"error": {"code": "NOT_IMPLEMENTED", ...}}: a write Box Office cannot do

Each check prints one PASS or FAIL line; the last line counts them. Exit 0 when every check passes,
1 otherwise, 77 when kn_brain, the sqlite scratch database or openssl is unavailable (CTest's skip).
"""
import base64
import hashlib
import hmac
import json
import os
import re
import secrets
import shutil
import sqlite3
import struct
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
NAME = "kn_test_v10auth_%d" % os.getpid()
SECRET = "kn-v10auth-test-secret-%d" % os.getpid()
SMTP_USER, SMTP_PASS = "mailer@caan.test", "fake-smtp-pass-%d" % os.getpid()
PASSWORD = "Passw0rd-kn-test!"
RESULTS = []
HEX = "0" * 32   # a challenge id no sign-in ever issued

sys.path.insert(0, os.path.join(ROOT, "services", "common", "tests"))


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
    headers = {"Accept": "application/json"}
    if data is not None:
        headers["Content-Type"] = "application/json"
    if token:
        headers["Authorization"] = "Bearer " + token
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


def error_code(body):
    err = body.get("error") if isinstance(body, dict) else None
    return err.get("code") if isinstance(err, dict) else None


def is_text_error(body):
    """{"data": null, "error": "<text>"}: the auth shape the consumers throw"""
    return isinstance(body, dict) and body.get("data") is None and isinstance(body.get("error"), str) and body["error"] != ""


def is_refusal(status, body):
    return status == 403 and error_code(body) == "NOT_IMPLEMENTED" and "data" not in (body or {})


def totp(secret_b32, step):
    """RFC 6238, SHA-1, 6 digits: the code of one 30 s step (the brain's kn_totp_code)"""
    pad = "=" * (-len(secret_b32) % 8)
    key = base64.b32decode(secret_b32.upper() + pad)
    h = hmac.new(key, struct.pack(">Q", step), hashlib.sha1).digest()
    o = h[-1] & 0x0F
    return "%06d" % ((struct.unpack(">I", h[o:o + 4])[0] & 0x7FFFFFFF) % 1000000)


def now_step():
    return int(time.time()) // 30


def b32_secret():
    return base64.b32encode(secrets.token_bytes(20)).decode().rstrip("=")


def sql_time():
    return time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime()) + ".000"


def fresh_user(tag):
    return "f1b%s%04d" % (tag, secrets.randbelow(10000))


# ---- the checks

def run_checks(port, db_path, smtp):
    import sqlite3

    def db(sql, args=()):
        con = sqlite3.connect(db_path, timeout=10)
        try:
            con.execute(sql, args)
            con.commit()
        finally:
            con.close()

    # accounts: A plain (login, usernames, recover), B wrong password, C with 2FA (seeded secret),
    # E the 2FA setup and disable flow
    A, B, C, E = fresh_user("a"), fresh_user("b"), fresh_user("c"), fresh_user("e")
    email = "%s@example.test" % A.lower()
    for name in (A, B, C, E):
        st, body = http(port, "POST", "/auth/register", {"username": name, "password": PASSWORD})
        check("setup: register %s over /auth/register" % name, st == 200 and isinstance(body, dict) and body.get("ok"),
              "HTTP %d" % st)
    db("UPDATE accounts SET email = ?, email_verified_at = ? WHERE username = ?", (email, sql_time(), A))
    c_secret = b32_secret()
    db("UPDATE accounts SET twofa_secret = ? WHERE username = ?", (c_secret, C))

    # ---- gapi POST /auth/nonce (row 32): the nonce is the attestation payload; none -> no payload
    st, body = http(port, "POST", "/auth/nonce", {})
    check("POST /auth/nonce -> 200 EMPTY {data: null} (v10.0.3 decoded 4381, 5161; v10.js 26172, 115004 reads data.nonce)",
          st == 200 and body == {"data": None, "error": None}, "HTTP %d %s" % (st, json.dumps(body)[:160]))

    # ---- rows 33-35: password login, through acct_auth
    st, body = http(port, "POST", "/auth/login/username", {"username": B, "password": "wrong-password-1"})
    check("POST /auth/login/username wrong password -> 401 ERROR text, no token (v10.0.3 decoded 3905, 5161; v10.js 26177, 114475)",
          st == 401 and is_text_error(body) and "access_token" not in json.dumps(body), "HTTP %d %s" % (st, json.dumps(body)[:160]))

    st, body = http(port, "POST", "/auth/login/username", {"username": A, "password": PASSWORD, "a": "", "b": ""})
    data = body.get("data") if isinstance(body, dict) else None
    token_a = data.get("access_token") if isinstance(data, dict) else None
    check("POST /auth/login/username -> 200 LOGIN {type: login_ok, access_token, refresh_token} (v10.0.3 decoded 3905, 5161; v10.js 26177, 114475 reads data.type, access_token, refresh_token)",
          st == 200 and isinstance(data, dict) and data.get("type") == "login_ok" and isinstance(token_a, str) and token_a
          and isinstance(data.get("refresh_token"), str), "HTTP %d %s" % (st, json.dumps(body)[:200]))
    st, me = http(port, "GET", "/auth/me", token=token_a)
    check("the token of POST /auth/login/username is a real session: GET /auth/me answers it (KN auth.c)",
          st == 200 and isinstance(me, dict) and (me.get("account") or {}).get("username") == A, "HTTP %d" % st)

    st, body = http(port, "POST", "/auth/login/email", {"email": email.upper(), "password": PASSWORD})
    data = body.get("data") if isinstance(body, dict) else None
    check("POST /auth/login/email -> 200 LOGIN (email, any case) (v10.0.3 decoded 3633, 5161; v10.js 26190, 114478)",
          st == 200 and isinstance(data, dict) and data.get("type") == "login_ok" and isinstance(data.get("access_token"), str),
          "HTTP %d %s" % (st, json.dumps(body)[:160]))

    # ---- rows 36-38, 40-45, 47-49: challenge flows Box Office never issues: no challenge exists
    for label, method, path, req_body, cite in [
        ("POST /auth/code-challenge/:id", "POST", "/auth/code-challenge/" + HEX, {"code": "123456"},
         "3921 / 5161; v10.js 26209, 114161 (enter_code: no email code)"),
        ("POST /auth/resend-code-challenge/:id", "POST", "/auth/resend-code-challenge/" + HEX, {},
         "5161; v10.js 26217"),
        ("POST /auth/migrate/:id", "POST", "/auth/migrate/" + HEX, {"email": email},
         "282 / 5161; v10.js 26222, 114182 (ensure_migrated: no legacy migration)"),
        ("POST /auth/verification/check/:id", "POST", "/auth/verification/check/" + HEX, {},
         "2703 / 5161; v10.js 26238, 114141 (ensure_verified: login never asks)"),
        ("POST /auth/verification/resend/:id", "POST", "/auth/verification/resend/" + HEX, {},
         "5161; v10.js 26243, 113497"),
        ("POST /auth/verification/change-email/:id", "POST", "/auth/verification/change-email/" + HEX,
         {"newEmail": "x@example.test"}, "4863 / 5161; v10.js 26248, 113570"),
        ("POST /auth/register/email-code", "POST", "/auth/register/email-code", {"email": email},
         "4652 / 5161; v10.js 26256, 114434 (sign-up is username and password)"),
        ("POST /auth/username-challenge/:id", "POST", "/auth/username-challenge/" + HEX, {"username": "x"},
         "3900 / 5161; v10.js 26264, 114209 (select_username: no sign-up challenge)"),
        ("POST /auth/password-challenge/:id", "POST", "/auth/password-challenge/" + HEX, {"password": "x"},
         "3245 / 5161; v10.js 26272, 114245 (update_password: no sign-up challenge)"),
        ("POST /auth/kid/age-challenge/:id", "POST", "/auth/kid/age-challenge/" + HEX, {"age": 18},
         "5161; v10.js 26288, 114292 (kid_age_gate: no K-ID gate)"),
        ("POST /auth/kid/parental-email/:id", "POST", "/auth/kid/parental-email/" + HEX, {"email": email},
         "5161; v10.js 26296, 114358 (kid_parental_consent: no K-ID consent)"),
        ("GET /auth/kid/consent-status/:id", "GET", "/auth/kid/consent-status/" + HEX, None,
         "4415 / 5161; v10.js 26304, 114370"),
    ]:
        st, body = http(port, method, path, req_body)
        check("%s -> 404 NONE text (v10.0.3 decoded %s)" % (label, cite), st == 404 and is_text_error(body),
              "HTTP %d %s" % (st, json.dumps(body)[:160]))

    # ---- rows 39 (2FA challenge): the 2FA prompt of a password login, then its code
    st, body = http(port, "POST", "/auth/login/username", {"username": C, "password": PASSWORD})
    data = body.get("data") if isinstance(body, dict) else None
    cid = data.get("challenge_id") if isinstance(data, dict) else None
    check("POST /auth/login/username on a 2FA account -> 200 CHECK {type: check_2fa, challenge_id} (v10.0.3 decoded 3905, 2704, 5161; v10.js 26177, 114129 reads challenge_id)",
          st == 200 and isinstance(data, dict) and data.get("type") == "check_2fa" and isinstance(cid, str) and cid != ""
          and "access_token" not in json.dumps(body), "HTTP %d %s" % (st, json.dumps(body)[:160]))
    cid = cid or HEX

    st, body = http(port, "POST", "/auth/2fa/challenge/" + cid, {"code": "000000"})
    data = body.get("data") if isinstance(body, dict) else None
    check("POST /auth/2fa/challenge/:id wrong code -> 200 CHECK again (the consumer asks again) (v10.0.3 decoded 3954, 5161; v10.js 26230, 114129)",
          st == 200 and isinstance(data, dict) and data.get("type") == "check_2fa" and data.get("challenge_id") == cid,
          "HTTP %d %s" % (st, json.dumps(body)[:160]))

    st, body = http(port, "POST", "/auth/2fa/challenge/" + cid, {"code": totp(c_secret, now_step())})
    data = body.get("data") if isinstance(body, dict) else None
    token_c = data.get("access_token") if isinstance(data, dict) else None
    check("POST /auth/2fa/challenge/:id right code -> 200 LOGIN {type: login_ok, access_token} (v10.0.3 decoded 3954, 5161; v10.js 26230, 114129)",
          st == 200 and isinstance(data, dict) and data.get("type") == "login_ok" and isinstance(token_c, str) and token_c != "",
          "HTTP %d %s" % (st, json.dumps(body)[:160]))
    st, me = http(port, "GET", "/auth/me", token=token_c)
    check("the 2FA login token is a real session: GET /auth/me answers it", st == 200 and (me or {}).get("account", {}).get("username") == C,
          "HTTP %d" % st)

    st, body = http(port, "POST", "/auth/2fa/challenge/" + HEX, {"code": "000000"})
    check("POST /auth/2fa/challenge/:id unknown challenge -> 404 NONE text (v10.0.3 decoded 3954, 5161; v10.js 26230)",
          st == 404 and is_text_error(body), "HTTP %d %s" % (st, json.dumps(body)[:160]))

    # ---- rows 46: recover-account, the reset mail (rpr) to the verified address
    before = len(smtp.messages)
    st, body = http(port, "POST", "/auth/recover-account", {"email": email})
    data = body.get("data") if isinstance(body, dict) else None
    mails = smtp.wait_messages(before + 1, timeout=10.0)
    check("POST /auth/recover-account -> 200 with a message, and the reset mail reaches the verified address (v10.0.3 decoded 3259, 5161; v10.js 26280, 114750 reads no field, the success text follows)",
          st == 200 and isinstance(data, dict) and isinstance(data.get("message"), str) and len(mails) >= before + 1
          and any(email in [r.lower() for r in m.get("rcpt", [])] for m in mails[before:]),
          "HTTP %d %s, mails %d" % (st, json.dumps(body)[:120], len(mails) - before))
    st, body = http(port, "POST", "/auth/recover-account", {"email": "not-an-address"})
    check("POST /auth/recover-account invalid email -> 400 ERROR text (v10.0.3 decoded 3259; the rpr validation)",
          st == 400 and is_text_error(body), "HTTP %d %s" % (st, json.dumps(body)[:160]))

    # ---- rows 52-54: the account's 2FA (setup, verify, disable), E
    st, body = http(port, "POST", "/account/2fa/enable/start", {})
    check("POST /account/2fa/enable/start without a token -> 401 ERROR text (v10.0.3 decoded 52 string, 5161 client; v10.js 26325, 209974)",
          st == 401 and is_text_error(body), "HTTP %d %s" % (st, json.dumps(body)[:160]))

    st_e, sess_e = http(port, "POST", "/auth/login/username", {"username": E, "password": PASSWORD})
    token_e = (sess_e.get("data") or {}).get("access_token") if isinstance(sess_e, dict) else None

    st, body = http(port, "POST", "/account/2fa/enable/start", {}, token=token_e)
    uri = (body.get("data") or {}).get("qr_code_url") if isinstance(body, dict) and isinstance(body.get("data"), dict) else None
    m = re.search(r"[?&]secret=([A-Za-z2-7]+)", uri or "")
    e_secret = m.group(1) if m else None
    check("POST /account/2fa/enable/start -> 200 {data: {qr_code_url: otpauth URI with a secret}} (v10.0.3 decoded 3331, 3474, 5161; v10.js 26325, 209974 reads data.qr_code_url)",
          st == 200 and uri is not None and uri.startswith("otpauth://totp/") and e_secret is not None,
          "HTTP %d %s" % (st, json.dumps(body)[:160]))
    e_secret = e_secret or "A" * 16

    st, body = http(port, "POST", "/account/2fa/enable/verify", {"code": "000000"}, token=token_e)
    check("POST /account/2fa/enable/verify wrong code -> 400 ERROR 'Invalid Code' (the consumer retries on 'invalid code') (v10.0.3 decoded 3335, 5161; v10.js 26331, 209986)",
          st == 400 and is_text_error(body) and "invalid code" in body["error"].lower(), "HTTP %d %s" % (st, json.dumps(body)[:160]))

    enable_step = now_step()
    st, body = http(port, "POST", "/account/2fa/enable/verify", {"code": totp(e_secret, enable_step)}, token=token_e)
    check("POST /account/2fa/enable/verify right code -> 200 {data: {enabled: true}} (v10.0.3 decoded 3335, 5161; v10.js 26331, 210007)",
          st == 200 and isinstance(body, dict) and (body.get("data") or {}).get("enabled") is True and body.get("error") is None,
          "HTTP %d %s" % (st, json.dumps(body)[:160]))

    st, body = http(port, "POST", "/account/2fa/disable", {"code": "000000"}, token=token_e)
    check("POST /account/2fa/disable wrong code -> 400 ERROR 'Invalid Code' (v10.0.3 decoded 469, 5161; v10.js 26339, 210081)",
          st == 400 and is_text_error(body) and "invalid code" in body["error"].lower(), "HTTP %d %s" % (st, json.dumps(body)[:160]))

    st, body = http(port, "POST", "/account/2fa/disable", {"code": totp(e_secret, now_step() + 1)}, token=token_e)
    check("POST /account/2fa/disable right code -> 200 {data: {enabled: false}} (v10.0.3 decoded 469, 5161; v10.js 26339, 210081)",
          st == 200 and isinstance(body, dict) and (body.get("data") or {}).get("enabled") is False and body.get("error") is None,
          "HTTP %d %s" % (st, json.dumps(body)[:160]))

    # ---- rows 55-57: usernames. Box Office's one name is the account's login username: active, never expires
    st, body = http(port, "GET", "/account/usernames", token=token_a)
    data = body.get("data") if isinstance(body, dict) else None
    names = data.get("names") if isinstance(data, dict) else None
    check("GET /account/usernames -> 200 {data: {names: [{name, state: active}], cooldown_secs: 0}} (v10.0.3 decoded 3319, 5161; v10.js 26347, 113173 reads names[].name/state, cooldown_secs)",
          st == 200 and isinstance(names, list) and len(names) == 1 and names[0].get("name") == A and names[0].get("state") == "active"
          and data.get("cooldown_secs") == 0 and data.get("hold_secs") == 0 and data.get("held") in (None, ""),
          "HTTP %d %s" % (st, json.dumps(body)[:200]))
    st, body = http(port, "GET", "/account/usernames")
    check("GET /account/usernames without a token -> 401 ERROR text (v10.0.3 decoded 3319; v10.js 26347)",
          st == 401 and is_text_error(body), "HTTP %d %s" % (st, json.dumps(body)[:160]))

    st, body = http(port, "POST", "/account/usernames/reserve", {"username": fresh_user("x")}, token=token_a)
    check("POST /account/usernames/reserve -> 403 REFUSAL (a paid purchase Box Office does not take) (v10.0.3 decoded 468, 5161; v10.js 26352, 113186)",
          is_refusal(st, body), "HTTP %d %s" % (st, json.dumps(body)[:160]))

    st, body = http(port, "POST", "/account/usernames/swap", {"username": fresh_user("y")}, token=token_a)
    check("POST /account/usernames/swap to a name not owned -> 403 REFUSAL (v10.0.3 decoded 4805, 5161; v10.js 26360, 113199)",
          is_refusal(st, body), "HTTP %d %s" % (st, json.dumps(body)[:160]))

    st, body = http(port, "POST", "/account/usernames/swap", {"username": A}, token=token_a)
    data = body.get("data") if isinstance(body, dict) else None
    check("POST /account/usernames/swap to the active name -> 200 {data: {active, names, cooldown_secs}} (v10.0.3 decoded 4805; v10.js 26360, 113199 reads data.active)",
          st == 200 and isinstance(data, dict) and data.get("active") == A and isinstance(data.get("names"), list),
          "HTTP %d %s" % (st, json.dumps(body)[:160]))

    # ---- rows 65-66: the mail preference. Box Office stores none: the consumer's 'not chosen' is data null
    st, body = http(port, "GET", "/email/preferences", token=token_a)
    check("GET /email/preferences -> 200 EMPTY {data: null} (no stored preference; the consumer shows 'not chosen') (v10.0.3 decoded 5511, 5161; v10.js 26424, 114573)",
          st == 200 and body == {"data": None, "error": None}, "HTTP %d %s" % (st, json.dumps(body)[:160]))
    st, body = http(port, "POST", "/email/preferences", {"promotional": False, "source": "settings"}, token=token_a)
    check("POST /email/preferences -> 403 REFUSAL (no preference store) (v10.0.3 decoded 5161; v10.js 26429, 114583)",
          is_refusal(st, body), "HTTP %d %s" % (st, json.dumps(body)[:160]))

    # ---- rows 67-70: Discord. No link exists: the status is the not-linked shape
    st, body = http(port, "GET", "/discord/status", token=token_a)
    check("GET /discord/status -> 200 EMPTY {data: {linked: false}} (no Discord link) (v10.0.3 decoded 3336, 5161; v10.js 26439, 97112 reads data.linked)",
          st == 200 and body == {"data": {"linked": False}, "error": None}, "HTTP %d %s" % (st, json.dumps(body)[:160]))
    for label, path, req_body, cite in [
        ("POST /discord/link", "/discord/link", {"returnTo": "x"}, "4655 / 5161; v10.js 26444, 97215"),
        ("POST /discord/join", "/discord/join", {}, "5161; v10.js 26452"),
        ("POST /discord/unlink", "/discord/unlink", {}, "4403 / 5161; v10.js 26457, 97215"),
    ]:
        st, body = http(port, "POST", path, req_body, token=token_a)
        check("%s -> 403 REFUSAL (Box Office has no Discord link) (v10.0.3 decoded %s)" % (label, cite), is_refusal(st, body),
              "HTTP %d %s" % (st, json.dumps(body)[:160]))
    email_code_checks(port, smtp, db_path)
    no_challenge_checks(port)


# ---- rows 35-37 (the emailed sign-in code) and the 'no such challenge' text of the other challenge routes

EXPIRED = "This sign-in has expired. Sign in again."
SIGNUP = "Sign up with a username and password on Box Office"
CODE_RE = re.compile(r"sign-in code for Krunker Box Office is (\d{6})")


def db_exec(db_path, sql, args=()):
    con = sqlite3.connect(db_path, timeout=10)
    try:
        con.execute(sql, args)
        con.commit()
    finally:
        con.close()


def enter_code_id(st, body):
    """the challenge id of a 200 enter_code state ({data: {type, challenge_id}, error: null}), else None"""
    data = body.get("data") if isinstance(body, dict) else None
    cid = data.get("challenge_id") if isinstance(data, dict) else None
    shape = st == 200 and body == {"data": {"type": "enter_code", "challenge_id": cid}, "error": None}
    return cid if shape and isinstance(cid, str) and re.fullmatch(r"[0-9a-f]{32}", cid) else None


def mails_to(smtp, addr, before):
    """the fake SMTP's messages to addr, from message number `before` on"""
    return [m for m in list(smtp.messages)[before:] if addr.lower() in [r.lower() for r in m.get("rcpt", [])]]


def next_code(smtp, addr, before, timeout=10.0):
    """the six-digit sign-in code of the first code mail to addr from message `before` on (None: no mail came)"""
    end = time.time() + timeout
    while True:
        for m in mails_to(smtp, addr, before):
            hit = CODE_RE.search(m["data"].decode("utf-8", "replace"))
            if hit:
                return hit.group(1)
        if time.time() >= end:
            return None
        time.sleep(0.1)


def email_code_checks(port, smtp, db_path):
    """rows 35-37 on the default config (accounts.login.email_code true). The consumer (v10.js 114109-114200)
    gets enter_code, asks for the code and sends respondCodeChallenge; a wrong code is enter_code again, the
    right one login_ok. The mail is the fake SMTP's; the code is read from it."""
    G = fresh_user("g")
    st, body = http(port, "POST", "/auth/register", {"username": G, "password": PASSWORD})
    check("setup: register %s for the email-code sign-in" % G, st == 200 and isinstance(body, dict) and body.get("ok"),
          "HTTP %d" % st)
    g_email = "%s@example.test" % G.lower()
    db_exec(db_path, "UPDATE accounts SET email = ?, email_verified_at = ? WHERE username = ?", (g_email, sql_time(), G))
    ghost = "ghost%d@example.test" % secrets.randbelow(10 ** 9)

    # row 35: the state, the mail, and the same answer for an address with no account
    before = len(smtp.messages)
    st, body = http(port, "POST", "/auth/login/email-code", {"email": g_email})
    cid = enter_code_id(st, body)
    check("[35] POST /auth/login/email-code for an account's email -> 200 {data: {type: enter_code, challenge_id}, error: null} (v10.0.3 decoded 3633, 5161; v10.js 26201, 114109 enter_code branch)",
          cid is not None, "HTTP %d %s" % (st, json.dumps(body)[:160]))
    st, body = http(port, "POST", "/auth/login/email-code", {"email": ghost})
    check("[35] POST /auth/login/email-code for an address with no account -> the same 200 enter_code shape (no account enumeration)",
          enter_code_id(st, body) is not None, "HTTP %d %s" % (st, json.dumps(body)[:160]))
    code1 = next_code(smtp, g_email, before) if cid else None
    check("[35] the sign-in code is mailed to the account's email: six digits, through the fake SMTP",
          code1 is not None and len(code1) == 6, "no code mail to %s" % g_email)
    check("[35] no mail goes to an address with no account (the same answer, nothing sent)", not mails_to(smtp, ghost, before))

    # row 36: a wrong code asks again, the right code signs in, and the used challenge is gone
    wrong = "000000" if code1 != "000000" else "111111"
    st, body = http(port, "POST", "/auth/code-challenge/" + (cid or HEX), {"code": wrong})
    check("[36] POST /auth/code-challenge/:id with a wrong code -> 200 enter_code again, same challenge (v10.js 26209, 114161: the loop asks again)",
          cid is not None and enter_code_id(st, body) == cid, "HTTP %d %s" % (st, json.dumps(body)[:160]))
    st, body = http(port, "POST", "/auth/code-challenge/" + (cid or HEX), {"code": code1 or wrong})
    data = body.get("data") if isinstance(body, dict) else None
    token = data.get("access_token") if isinstance(data, dict) else None
    check("[36] the right code -> 200 LOGIN {type: login_ok, access_token, refresh_token}, error null (v10.0.3 decoded 3633, 5161; v10.js 26209, 114161 ends the loop on login_ok)",
          st == 200 and isinstance(data, dict) and data.get("type") == "login_ok" and isinstance(token, str) and token != ""
          and isinstance(data.get("refresh_token"), str) and body.get("error") is None, "HTTP %d %s" % (st, json.dumps(body)[:160]))
    st, me = http(port, "GET", "/account/usernames", token=token)
    names = ((me or {}).get("data") or {}).get("names") if isinstance(me, dict) else None
    check("[36] the email-code login token is a real session: GET /account/usernames answers the account",
          st == 200 and isinstance(names, list) and len(names) == 1 and names[0].get("name") == G, "HTTP %d" % st)
    st, body = http(port, "POST", "/auth/code-challenge/" + (cid or HEX), {"code": code1 or wrong})
    check("[36] the used challenge again -> 404 with the expired text, data null (v10.js 114161 throws the text)",
          st == 404 and is_text_error(body) and body["error"] == EXPIRED, "HTTP %d %s" % (st, json.dumps(body)[:160]))

    # row 37: a resend keeps the challenge, mails a new code and voids the old one
    before = len(smtp.messages)
    st, body = http(port, "POST", "/auth/login/email-code", {"email": g_email})
    cid2 = enter_code_id(st, body)
    code2a = next_code(smtp, g_email, before) if cid2 else None
    before = len(smtp.messages)
    st, body = http(port, "POST", "/auth/resend-code-challenge/" + (cid2 or HEX))
    check("[37] POST /auth/resend-code-challenge/:id -> 200 enter_code for the same challenge (v10.js 26217)",
          cid2 is not None and enter_code_id(st, body) == cid2, "HTTP %d %s" % (st, json.dumps(body)[:160]))
    code2b = next_code(smtp, g_email, before) if cid2 else None
    check("[37] the resend mails a new six-digit code to the account's email", code2b is not None and len(code2b) == 6,
          "no code mail")
    st, body = http(port, "POST", "/auth/code-challenge/" + (cid2 or HEX), {"code": code2a or "000000"})
    check("[37] the code the resend replaced no longer signs in (the loop asks again)",
          cid2 is not None and enter_code_id(st, body) == cid2, "HTTP %d %s" % (st, json.dumps(body)[:160]))
    st, body = http(port, "POST", "/auth/code-challenge/" + (cid2 or HEX), {"code": code2b or "000000"})
    data = body.get("data") if isinstance(body, dict) else None
    check("[36] the resent code signs in: 200 login_ok", st == 200 and isinstance(data, dict) and data.get("type") == "login_ok",
          "HTTP %d %s" % (st, json.dumps(body)[:160]))

    # row 36: five wrong codes end the sign-in (the login throttle's answer), so the right code is expired
    before = len(smtp.messages)
    st, body = http(port, "POST", "/auth/login/email-code", {"email": g_email})
    cid3 = enter_code_id(st, body)
    code3 = next_code(smtp, g_email, before) if cid3 else None
    bad = [c for c in ("123401", "123402", "123403", "123404", "123405", "123406") if c != code3][:5]
    replies = [http(port, "POST", "/auth/code-challenge/" + (cid3 or HEX), {"code": c}) for c in bad[:4]]
    check("[36] four wrong codes -> 200 enter_code each time (the challenge holds)",
          cid3 is not None and all(enter_code_id(s, b) == cid3 for s, b in replies), "%s" % [s for s, _ in replies])
    st, body = http(port, "POST", "/auth/code-challenge/" + (cid3 or HEX), {"code": bad[4]})
    check("[36] the fifth wrong code ends the sign-in: 429 with the login throttle text 'Please Try Later'",
          st == 429 and is_text_error(body) and body["error"] == "Please Try Later", "HTTP %d %s" % (st, json.dumps(body)[:160]))
    st, body = http(port, "POST", "/auth/code-challenge/" + (cid3 or HEX), {"code": code3 or "123400"})
    check("[36] after the lock the right code is refused as expired (404)",
          st == 404 and is_text_error(body) and body["error"] == EXPIRED, "HTTP %d %s" % (st, json.dumps(body)[:160]))

    # a 2FA account: the same answer, and no mail (its sign-in is the password and the authenticator code)
    H = fresh_user("h")
    st, body = http(port, "POST", "/auth/register", {"username": H, "password": PASSWORD})
    h_email = "%s@example.test" % H.lower()
    db_exec(db_path, "UPDATE accounts SET email = ?, email_verified_at = ?, twofa_secret = ? WHERE username = ?",
            (h_email, sql_time(), b32_secret(), H))
    before = len(smtp.messages)
    st, body = http(port, "POST", "/auth/login/email-code", {"email": h_email})
    check("[35] POST /auth/login/email-code for a 2FA account -> the same 200 enter_code shape (v10.js 114109)",
          enter_code_id(st, body) is not None, "HTTP %d %s" % (st, json.dumps(body)[:160]))
    time.sleep(1.5)
    check("[35] no sign-in mail goes to a 2FA account (the policy: such an account signs in with its password and code)",
          not mails_to(smtp, h_email, before))


def no_challenge_checks(port):
    """rows 36-37 (unknown challenge), 38, 43-45, 47-49: the text each consumer throws. None of these
    challenges is ever issued, so the answer is 404 with data null and the expired text (challenge ids),
    or the sign-up text (row 43: sign-up is username and password). v10.js 114109-114440, 26209-26304."""
    for label, method, path, req_body, want, row in [
        ("POST /auth/code-challenge/:id", "POST", "/auth/code-challenge/" + HEX, {"code": "123456"}, EXPIRED, "36"),
        ("POST /auth/resend-code-challenge/:id", "POST", "/auth/resend-code-challenge/" + HEX, {}, EXPIRED, "37"),
        ("POST /auth/migrate/:id", "POST", "/auth/migrate/" + HEX, {"email": "x@example.test"}, EXPIRED, "38"),
        ("POST /auth/register/email-code", "POST", "/auth/register/email-code", {"email": "x@example.test"}, SIGNUP, "43"),
        ("POST /auth/username-challenge/:id", "POST", "/auth/username-challenge/" + HEX, {"username": "x"}, EXPIRED, "44"),
        ("POST /auth/password-challenge/:id", "POST", "/auth/password-challenge/" + HEX, {"password": "x"}, EXPIRED, "45"),
        ("POST /auth/kid/age-challenge/:id", "POST", "/auth/kid/age-challenge/" + HEX, {"age": 18}, EXPIRED, "47"),
        ("POST /auth/kid/parental-email/:id", "POST", "/auth/kid/parental-email/" + HEX, {"email": "x@example.test"}, EXPIRED, "48"),
        ("GET /auth/kid/consent-status/:id", "GET", "/auth/kid/consent-status/" + HEX, None, EXPIRED, "49"),
    ]:
        st, body = http(port, method, path, req_body)
        check("[%s] %s -> 404 data null, error %r (v10.js consumer throws the text)" % (row, label, want),
              st == 404 and is_text_error(body) and body["error"] == want, "HTTP %d %s" % (st, json.dumps(body)[:160]))


def email_code_off_checks(port):
    """accounts.login.email_code false: rows 35-37 answer the not-offered reply, as before this change"""
    st, body = http(port, "POST", "/auth/login/email-code", {"email": "off@example.test"})
    check("POST /auth/login/email-code -> 404 NONE text: Box Office has no email-code sign-in when accounts.login.email_code is false (v10.0.3 decoded 3633, 5161; v10.js 26201, 114483)",
          st == 404 and is_text_error(body), "HTTP %d %s" % (st, json.dumps(body)[:160]))
    st, body = http(port, "POST", "/auth/code-challenge/" + HEX, {"code": "123456"})
    check("POST /auth/code-challenge/:id -> 404 NONE text when accounts.login.email_code is false",
          st == 404 and is_text_error(body), "HTTP %d %s" % (st, json.dumps(body)[:160]))
    st, body = http(port, "POST", "/auth/resend-code-challenge/" + HEX, {})
    check("POST /auth/resend-code-challenge/:id -> 404 NONE text when accounts.login.email_code is false",
          st == 404 and is_text_error(body), "HTTP %d %s" % (st, json.dumps(body)[:160]))


def start_brain(env, extra, log):
    """kn_brain on an ephemeral port: (process, port), port None when it did not start"""
    brain = subprocess.Popen([BRAIN, "--brain.host=127.0.0.1", "--brain.port=0", "--brain.region=local",
                              "--brain.id=v10-auth-test-%d" % os.getpid(), "--finder.poll_interval_s=1"] + extra,
                             cwd=ROOT, env=env, stdout=open(log, "w"), stderr=subprocess.STDOUT,
                             stdin=subprocess.DEVNULL, start_new_session=True)
    m = wait_for(lambda: re.search(r"listening on http://127\.0\.0\.1:(\d+) \(ws", read(log)), 20)
    check("kn_brain starts on an ephemeral port", m is not None, read(log)[-300:])
    return brain, (int(m.group(1)) if m else None)


def stop_brain(brain):
    if brain is not None:
        brain.terminate()
        try:
            brain.wait(timeout=5)
        except subprocess.TimeoutExpired:
            brain.kill()
    return None


def scenario_env(work):
    """the fake SMTP server and its certificate (services/common/tests/fake_smtp.py)"""
    from fake_smtp import FakeSMTP, make_cert  # noqa: E402

    cert, key = make_cert(work)
    smtp = FakeSMTP("starttls", cert, key, users={SMTP_USER: SMTP_PASS}, sender_domains=["caan.edu"]).start()
    return smtp, cert


def main():
    if not os.path.isfile(BRAIN) or not os.access(BRAIN, os.X_OK):
        print("v10_api_auth_test: no kn_brain at %s: skipped" % BRAIN)
        return 77
    if not shutil.which("openssl"):
        print("v10_api_auth_test: no openssl CLI (the fake SMTP certificate): skipped")
        return 77
    r = subprocess.run([sys.executable, "-I", os.path.join(HERE, "db_scratch.py"), ROOT, "create", "sqlite", NAME],
                       capture_output=True)
    if r.returncode:
        print("v10_api_auth_test: no sqlite scratch database: skipped")
        return 77
    cfg = json.loads(r.stdout)
    db_path = cfg["path"]
    work = tempfile.mkdtemp(prefix="kn-v10-auth-test.")
    log = os.path.join(work, "brain.log")
    smtp = None
    brain = None
    try:
        smtp, cert = scenario_env(work)
        env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=SECRET, KN_SMTP_PASSWORD=SMTP_PASS,
                   **{"KN_DATABASE_" + k.upper(): str(v) for k, v in cfg.items()})
        mig = subprocess.run([BRAIN, "--migrate-only"], cwd=ROOT, env=env, capture_output=True)
        check("the migrations apply", mig.returncode == 0, mig.stderr.decode(errors="replace")[-200:])
        if mig.returncode:
            return 1
        extra = ["--smtp.host=127.0.0.1", "--smtp.port=%d" % smtp.port, "--smtp.tls=starttls",
                 "--smtp.user=" + SMTP_USER, "--smtp.ca_file=" + cert, "--smtp.retries=-1",
                 "--accounts.email.link_base=http://127.0.0.1:1",
                 "--auth.login_per_min=100000", "--auth.register_per_hour=100000", "--auth.join_per_min=100000",
                 "--accounts.email.resend_cooldown_s=0", "--accounts.email.max_per_account_day=1000",
                 "--accounts.email.max_per_address_day=1000", "--accounts.email.max_per_ip_hour=1000"]
        brain, port = start_brain(env, extra, log)
        if port is None:
            return 1
        run_checks(port, db_path, smtp)
        brain = stop_brain(brain)
        # the same database with accounts.login.email_code off: rows 35-37 answer the not-offered reply
        brain, port = start_brain(env, extra + ["--accounts.login.email_code=false"], log + ".off")
        if port is None:
            return 1
        email_code_off_checks(port)
    except Exception as e:  # noqa: BLE001
        check("the scenario runs", False, "exception: %r" % e)
    finally:
        if brain is not None:
            brain.terminate()
            try:
                brain.wait(timeout=5)
            except subprocess.TimeoutExpired:
                brain.kill()
        if smtp is not None:
            smtp.stop()
        subprocess.run([sys.executable, "-I", os.path.join(HERE, "db_scratch.py"), ROOT, "drop", "sqlite", NAME],
                       capture_output=True)
    passed = sum(RESULTS)
    failed = len(RESULTS) - passed
    print("v10_api_auth_test: %d PASS, %d FAIL" % (passed, failed))
    if failed:
        print("logs kept in " + work)
        return 1
    shutil.rmtree(work, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
