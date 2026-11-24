#!/usr/bin/env python3
"""Login with Box Office on kn_brain (services/brain/oauth.c, migration 0366_account_links,
doc/v10/oauth-provider.md): Krunker v10.0.7's Login with Krunker and Settings > Connected apps.

  oauth_test.py <kn_brain> <repo root>
env: KN_TEST_DB_DRIVER=sqlite (default) | postgres | mysql (db_scratch.py). 77 = skip.

Covers: staff client registration (validation, the secret shown once); the client's authorize/info and
authorize (PKCE S256 required, unknown scope / redirect refused, min_level); the token endpoint (wrong
verifier, code reuse, a confidential client's secret, refresh rotation); userinfo / stats / inventory /
friends with their scopes; revoke (access, refresh takes the grant's tokens); Connected apps (list,
activity, Disconnect); the consent page (sign-in, signed cookie, CSRF, deny -> access_denied, allow ->
code); write scopes (2FA required, accept + code) and actions (follow.add approved with 2FA once, a wrong
code, deny, the app's poll, a disconnect expiring pending actions).
"""
import asyncio
import base64
import hashlib
import hmac
import http.client
import json
import os
import re
import secrets
import sqlite3
import struct
import subprocess
import sys
import tempfile
import time
import urllib.parse

try:
    import websockets
except ImportError:  # pragma: no cover
    print("python websockets missing: skip")
    sys.exit(77)

BRAIN, ROOT = sys.argv[1], os.path.abspath(sys.argv[2])
DRIVER = os.environ.get("KN_TEST_DB_DRIVER", "sqlite")
SCHEMA = "kn_test_oauth_%d" % os.getpid()
SECRET = "kn-oauth-test-%d" % os.getpid()
FAILS = []
DBCFG = {}
PORT = 0
REDIRECT = "https://app.example/cb"


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what, flush=True)
    if not cond:
        FAILS.append(what)


def bound_port(log_path, proc):
    for _ in range(400):
        if proc.poll() is not None:
            raise RuntimeError("kn_brain exited early (%s)" % log_path)
        with open(log_path) as fp:
            m = re.search(r"listening on http://127\.0\.0\.1:(\d+) \(ws", fp.read())
        if m:
            return int(m.group(1))
        time.sleep(0.05)
    raise RuntimeError("kn_brain did not start listening")


def query(sql):
    if DRIVER == "sqlite":
        con = sqlite3.connect(DBCFG["path"], timeout=30)
        try:
            rows = [str(r[0]) for r in con.execute(sql).fetchall()]
            con.commit()
            return rows
        finally:
            con.close()
    get = lambda k, d: os.environ.get("KN_DATABASE_" + k.upper(), str(DBCFG.get(k, d)))
    env = dict(os.environ, PGPASSWORD=get("password", ""), PGOPTIONS="-c search_path=" + str(DBCFG.get("search_path", "public")))
    r = subprocess.run(["psql", "-h", get("host", "127.0.0.1"), "-p", get("port", 5432), "-U", get("user", "krunknative"),
                        "-d", DBCFG["name"], "-v", "ON_ERROR_STOP=1", "-qAt", "-c", sql],
                       env=env, capture_output=True, timeout=30)
    return r.stdout.decode().split("\n")[:-1] if r.returncode == 0 else None


def totp(key_b32, offset=0):
    key = base64.b32decode(key_b32 + "=" * (-len(key_b32) % 8))
    mac = hmac.new(key, struct.pack(">Q", int(time.time() // 30) + offset), hashlib.sha1).digest()
    o = mac[-1] & 15
    return "%06d" % ((struct.unpack(">I", mac[o:o + 4])[0] & 0x7FFFFFFF) % 1000000)


def req(method, path, body=None, bearer=None, form=False, headers=None):
    """-> (status, headers dict (lower-case), body text)"""
    h = dict(headers or {})
    data = None
    if body is not None:
        if form:
            data = urllib.parse.urlencode(body).encode()
            h["Content-Type"] = "application/x-www-form-urlencoded"
        else:
            data = json.dumps(body).encode()
            h["Content-Type"] = "application/json"
    if bearer:
        h["Authorization"] = "Bearer " + bearer
    c = http.client.HTTPConnection("127.0.0.1", PORT, timeout=15)
    c.request(method, path, body=data, headers=h)
    r = c.getresponse()
    text = r.read().decode("utf-8", "replace")
    hdrs = {k.lower(): v for k, v in r.getheaders()}
    c.close()
    return r.status, hdrs, text


def jreq(*a, **k):
    st, h, text = req(*a, **k)
    try:
        return st, json.loads(text) if text else None
    except ValueError:
        return st, text


def pkce():
    v = secrets.token_urlsafe(48)
    c = base64.urlsafe_b64encode(hashlib.sha256(v.encode()).digest()).decode().rstrip("=")
    return v, c


def code_of(url):
    q = urllib.parse.parse_qs(urllib.parse.urlparse(url).query)
    return q.get("code", [None])[0], q.get("state", [None])[0], q.get("error", [None])[0]


def authorize(session, client, scope, challenge, state="st1", **extra):
    body = {"client_id": client, "redirect_uri": REDIRECT, "scope": scope, "state": state,
            "code_challenge": challenge, "code_challenge_method": "S256"}
    body.update(extra)
    return jreq("POST", "/oauth/authorize", body, bearer=session)


def token(body, form=True, auth=None):
    return jreq("POST", "/oauth/token", body, form=form, headers={"Authorization": auth} if auth else None)


async def register(port, name, password):
    async with websockets.connect("ws://127.0.0.1:%d/social/ws?enc=json" % port) as ws:
        await ws.send(json.dumps(["a", 0, [name, password, 0], "captcha"]))
        while True:
            f = json.loads(await asyncio.wait_for(ws.recv(), 15))
            if f[0] == "a":
                return f[2], f[5]


def new_client(scopes, confidential=False, min_level=0, name="Test App"):
    return jreq("POST", "/mm/admin/oauth/clients", {"name": name, "developer": "Tester", "redirect_uris": [REDIRECT],
                                                     "scopes": scopes, "confidential": confidential, "min_level": min_level,
                                                     "homepage_url": "https://app.example/"}, bearer=SECRET)


def scenario(port, me, my_token, other_name, password, my_name):
    # ---- staff registration
    st, r = jreq("POST", "/mm/admin/oauth/clients", {"name": "x", "redirect_uris": [REDIRECT], "scopes": ["profile"]})
    check(st in (401, 403), "client registration needs the internal secret (%s)" % st)
    st, r = new_client(["profile", "dms:read"])
    check(st == 400, "a scope Box Office does not serve is refused at registration (%s %r)" % (st, r))
    st, r = jreq("POST", "/mm/admin/oauth/clients", {"name": "x", "redirect_uris": ["http://evil.example/cb"], "scopes": ["profile"]},
                 bearer=SECRET)
    check(st == 400, "a plain-http redirect is refused at registration (%s)" % st)
    st, pub = new_client(["profile", "stats:read", "inventory:read", "friends:read"])
    check(st == 201 and pub.get("client_id") and "client_secret" not in pub, "public client registered: %r" % (pub,))
    pub = pub["client_id"]
    st, conf = new_client(["profile"], confidential=True, name="Server App")
    check(st == 201 and conf.get("client_secret", "").startswith("kbo_cs_"), "confidential client gets its secret once")
    st, locked = new_client(["profile"], min_level=99, name="Locked App")
    locked = locked["client_id"]
    st, writer = new_client(["profile", "friends:write"], name="Writer App")
    writer = writer["client_id"]
    st, lst = jreq("GET", "/mm/admin/oauth/clients", bearer=SECRET)
    check(st == 200 and len(lst) == 4 and not any("secret_hash" in c for c in lst), "staff list: 4 clients, no secret hashes")

    # ---- authorize/info
    v, c = pkce()
    q = urllib.parse.urlencode({"client_id": pub, "redirect_uri": REDIRECT, "scope": "profile stats:read", "state": "s",
                                "code_challenge": c, "code_challenge_method": "S256"})
    st, r = jreq("GET", "/oauth/authorize/info?" + q, bearer=my_token)
    d = (r or {}).get("data") or {}
    check(st == 200 and d.get("client", {}).get("name") == "Test App" and [s["scope"] for s in d.get("scopes", [])] == ["profile", "stats:read"]
          and d.get("account", {}).get("name") == my_name and d["account"].get("authorized") is False and d.get("write") is False,
          "authorize/info: app, scopes, caller, not yet authorized: %r" % (d,))
    st, r = jreq("GET", "/oauth/authorize/info?" + q.replace("S256", "plain"), bearer=my_token)
    check(st == 400, "authorize/info: plain PKCE refused")
    st, r = jreq("GET", "/oauth/authorize/info?" + q.replace(urllib.parse.quote(REDIRECT, safe=""), "https%3A%2F%2Fevil.example%2Fcb"),
                 bearer=my_token)
    check(st == 400, "authorize/info: unregistered redirect refused")
    st, r = jreq("GET", "/oauth/authorize/info?" + q.replace("stats%3Aread", "friends%3Awrite"), bearer=my_token)
    check(st == 400, "authorize/info: a scope the app is not registered for is refused")

    # ---- authorize -> code -> token
    st, r = authorize(None, pub, "profile", c)
    check(st == 401, "authorize without a session: LOGIN_REQUIRED (%s)" % st)
    st, r = authorize(my_token, locked, "profile", c)
    check(st == 400 and "level" in json.dumps(r), "min_level app refused: %r" % (r,))
    st, r = authorize(my_token, pub, "profile stats:read inventory:read", c)
    url = ((r or {}).get("data") or {}).get("redirect_url", "")
    code, state, _ = code_of(url)
    check(st == 200 and url.startswith(REDIRECT + "?code=kbo_ac_") and state == "st1", "authorize -> redirect with code and state")
    st, r = token({"grant_type": "authorization_code", "code": code, "redirect_uri": REDIRECT, "client_id": pub, "code_verifier": v + "x"})
    check(st == 400 and r.get("error") == "invalid_grant", "wrong code_verifier: invalid_grant (%r)" % (r,))
    st, r = token({"grant_type": "authorization_code", "code": code, "redirect_uri": REDIRECT, "client_id": pub, "code_verifier": v})
    check(st == 400 and r.get("error") == "invalid_grant", "a code is single use, even after a failed exchange")
    v, c = pkce()
    st, r = authorize(my_token, pub, "profile stats:read inventory:read", c)
    code = code_of(r["data"]["redirect_url"])[0]
    st, r = token({"grant_type": "authorization_code", "code": code, "redirect_uri": REDIRECT + "/x", "client_id": pub, "code_verifier": v})
    check(st == 400 and r.get("error") == "invalid_grant", "redirect_uri mismatch: invalid_grant")
    st, r = authorize(my_token, pub, "profile stats:read inventory:read", c)
    code = code_of(r["data"]["redirect_url"])[0]
    st, tok = token({"grant_type": "authorization_code", "code": code, "redirect_uri": REDIRECT, "client_id": pub, "code_verifier": v})
    check(st == 200 and tok.get("access_token", "").startswith("kbo_at_") and tok.get("refresh_token", "").startswith("kbo_rt_")
          and tok.get("token_type") == "Bearer" and tok.get("scope") == "profile stats:read inventory:read" and tok.get("expires_in") == 3600,
          "token: access + refresh, scope, expiry: %r" % ({k: tok.get(k) for k in ("token_type", "scope", "expires_in")},))
    st, r = token({"grant_type": "authorization_code", "code": code, "redirect_uri": REDIRECT, "client_id": pub, "code_verifier": v})
    check(st == 400 and r.get("error") == "invalid_grant", "code reuse after success: invalid_grant")
    st, r = token({"grant_type": "password", "client_id": pub})
    check(st == 400 and r.get("error") == "unsupported_grant_type", "other grant types refused")
    at, rt = tok["access_token"], tok["refresh_token"]

    # ---- the apps' routes
    st, u = jreq("GET", "/oauth/userinfo", bearer=at)
    check(st == 200 and u.get("id") == me and u.get("username") == my_name and "level" in u, "userinfo: %r" % (u,))
    st, s = jreq("GET", "/oauth/api/stats", bearer=at)
    check(st == 200 and "kills" in s and isinstance(s.get("stats"), dict), "stats:read: %r" % (s,))
    st, inv = jreq("GET", "/oauth/api/inventory", bearer=at)
    check(st == 200 and "funds" in inv and isinstance(inv.get("skins"), list), "inventory:read")
    st, h, _ = req("GET", "/oauth/api/friends", bearer=at)
    check(st == 403 and "insufficient_scope" in h.get("www-authenticate", ""), "friends:read not granted: 403 insufficient_scope")
    st, r = jreq("GET", "/oauth/userinfo", bearer=my_token)
    check(st == 401, "a game session token is not an app token")
    st, r = jreq("GET", "/oauth/userinfo", bearer="kbo_at_nope")
    check(st == 401 and r.get("error") == "invalid_token", "unknown access token: invalid_token")

    # ---- refresh rotation, confidential client, revoke
    st, t2 = token({"grant_type": "refresh_token", "refresh_token": rt, "client_id": pub})
    check(st == 200 and t2.get("refresh_token") != rt and t2.get("scope") == "profile stats:read inventory:read", "refresh rotates")
    st, r = token({"grant_type": "refresh_token", "refresh_token": rt, "client_id": pub})
    check(st == 400 and r.get("error") == "invalid_grant", "the old refresh token is gone")
    st, r = jreq("GET", "/oauth/userinfo", bearer=t2["access_token"])
    check(st == 200, "the refreshed access token works")
    st, r = jreq("POST", "/oauth/revoke", {"token": t2["access_token"], "client_id": pub}, form=True)
    check(st == 200, "revoke an access token: 200")
    st, r = jreq("GET", "/oauth/userinfo", bearer=t2["access_token"])
    check(st == 401, "a revoked access token is refused")
    st, r = jreq("GET", "/oauth/userinfo", bearer=at)
    check(st == 200, "the first access token still works (only one was revoked)")
    st, r = jreq("POST", "/oauth/revoke", {"token": t2["refresh_token"], "client_id": pub}, form=True)
    st, r = jreq("GET", "/oauth/userinfo", bearer=at)
    check(st == 401, "revoking a refresh token ends the grant's access tokens")
    st, r = jreq("POST", "/oauth/revoke", {"token": "kbo_rt_unknown", "client_id": pub}, form=True)
    check(st == 200, "revoking an unknown token is no error (RFC 7009)")

    cid, csec = conf["client_id"], conf["client_secret"]
    v, c = pkce()
    st, r = authorize(my_token, cid, "profile", c)
    code = code_of(r["data"]["redirect_url"])[0]
    st, r = token({"grant_type": "authorization_code", "code": code, "redirect_uri": REDIRECT, "client_id": cid, "code_verifier": v})
    check(st == 401 and r.get("error") == "invalid_client", "a confidential client without its secret: invalid_client")
    st, r = authorize(my_token, cid, "profile", c)
    code = code_of(r["data"]["redirect_url"])[0]
    basic = "Basic " + base64.b64encode(("%s:%s" % (cid, csec)).encode()).decode()
    st, r = token({"grant_type": "authorization_code", "code": code, "redirect_uri": REDIRECT, "code_verifier": v}, auth=basic)
    check(st == 200 and r.get("access_token"), "a confidential client with HTTP Basic credentials")

    # ---- Connected apps
    st, r = jreq("GET", "/account/apps", bearer=my_token)
    apps = (r or {}).get("data") or []
    names = sorted(a.get("name") for a in apps)
    a0 = next((a for a in apps if a.get("client_id") == pub), {})
    check(st == 200 and names == ["Server App", "Test App"] and a0.get("scopes") == ["profile", "stats:read", "inventory:read"]
          and a0.get("developer") == "Tester" and isinstance(a0.get("created_at"), (int, float)) and a0["created_at"] < 1e11,
          "Connected apps list (seconds, scopes, developer): %r" % (apps,))
    st, r = jreq("GET", "/account/apps/%s/activity" % pub, bearer=my_token)
    acts = [x.get("action") for x in (r or {}).get("data") or []]
    check(st == 200 and "authorize" in acts and "token" in acts and "userinfo" in acts and "stats" in acts, "app activity: %r" % (acts,))
    st, r = jreq("GET", "/account/apps", bearer=None)
    check(st == 401, "Connected apps needs a session")

    # ---- the consent page (no script): sign in, cookie, CSRF, deny, allow
    v, c = pkce()
    q = urllib.parse.urlencode({"response_type": "code", "client_id": pub, "redirect_uri": REDIRECT, "scope": "profile friends:read",
                                "state": "pg", "code_challenge": c, "code_challenge_method": "S256"})
    st, h, page = req("GET", "/oauth/authorize?" + q)
    check(st == 200 and "Sign in with your Box Office account" in page and "<script" not in page and "KrunkNative" not in page
          and h.get("x-frame-options") == "DENY", "consent page: the sign-in form, no script, framing denied")
    st, h, page = req("GET", "/oauth/authorize?" + q.replace(urllib.parse.quote(REDIRECT, safe=""), "https%3A%2F%2Fevil.example%2Fcb"))
    check(st == 400 and "not registered" in page, "consent page: an unregistered redirect is shown, not followed")
    fields = dict(urllib.parse.parse_qsl(q))
    st, h, _ = req("POST", "/oauth/authorize/signin", dict(fields, username=my_name, password="wrong-pass"), form=True)
    check(st == 401 or st == 200, "page sign-in with a wrong password stays on the page (%s)" % st)
    st, h, _ = req("POST", "/oauth/authorize/signin", dict(fields, username=my_name, password=password), form=True)
    cookie = (h.get("set-cookie") or "").split(";")[0]
    check(st == 303 and cookie.startswith("kbo_oauth=") and "HttpOnly" in h.get("set-cookie", "") and "SameSite=Lax" in h.get("set-cookie", "")
          and h.get("location", "").startswith("/oauth/authorize?"), "page sign-in: 303 back with the signed cookie")
    st, h, page = req("GET", h.get("location", "/"), headers={"Cookie": cookie})
    m = re.search(r'name="csrf" value="([0-9a-f]{64})"', page)
    check(st == 200 and m and my_name in page and "who you follow" in page, "consent page signed in: the scopes and a CSRF token")
    csrf = m.group(1) if m else ""
    st, h, _ = req("POST", "/oauth/authorize/decide", dict(fields, csrf="0" * 64, decision="allow"), form=True, headers={"Cookie": cookie})
    check(st == 200 and "location" not in h, "decide with a bad CSRF token: no redirect")
    st, h, _ = req("POST", "/oauth/authorize/decide", dict(fields, csrf=csrf, decision="deny"), form=True, headers={"Cookie": cookie})
    _, s_, err = code_of(h.get("location", ""))
    check(st == 303 and err == "access_denied" and s_ == "pg", "deny: access_denied with the state")
    st, h, _ = req("POST", "/oauth/authorize/decide", dict(fields, csrf=csrf, decision="allow"), form=True, headers={"Cookie": cookie})
    pcode, s_, _ = code_of(h.get("location", ""))
    check(st == 303 and pcode and s_ == "pg", "allow: the code, back to the app")
    st, r = token({"grant_type": "authorization_code", "code": pcode, "redirect_uri": REDIRECT, "client_id": pub, "code_verifier": v})
    check(st == 200 and r.get("scope") == "profile friends:read", "the page's code exchanges (scope as asked)")
    st, f = jreq("GET", "/oauth/api/friends", bearer=r.get("access_token"))
    check(st == 200 and f.get("following") == [] and f.get("followers") == [], "friends:read: %r" % (f,))
    st, r2 = jreq("GET", "/account/apps", bearer=my_token)
    a0 = next((a for a in r2["data"] if a.get("client_id") == pub), {})
    check("friends:read" in a0.get("scopes", []) and "stats:read" in a0.get("scopes", []), "a second consent widens the grant")
    check(query("SELECT count(*) FROM sessions WHERE account_id = %d" % me) == ["1"], "the page's sign-in left no game session behind")

    # ---- write scopes: 2FA required, accept + code; actions approved with 2FA
    v, c = pkce()
    st, r = authorize(my_token, writer, "profile friends:write", c, accept=True, twofa_code="000000")
    check(st == 400 and "2FA" in json.dumps(r), "write scope without 2FA on the account: refused (%r)" % (r,))
    key = base64.b32encode(secrets.token_bytes(20)).decode().rstrip("=")
    query("UPDATE accounts SET twofa_secret = '%s' WHERE id = %d" % (key, me))
    st, r = authorize(my_token, writer, "profile friends:write", c, twofa_code=totp(key, -1))
    check(st == 400 and "risk" in json.dumps(r), "write scope without accepting the risks: refused")
    st, r = authorize(my_token, writer, "profile friends:write", c, accept=True, twofa_code="123456" if totp(key, -1) != "123456" else "654321")
    check(st == 400, "write scope with a wrong 2FA code: refused")
    st, r = authorize(my_token, writer, "profile friends:write", c, accept=True, twofa_code=totp(key, -1))
    code = code_of(((r or {}).get("data") or {}).get("redirect_url", ""))[0]
    check(st == 200 and code, "write scope with accept + 2FA: code (%r)" % (r,))
    st, wt = token({"grant_type": "authorization_code", "code": code, "redirect_uri": REDIRECT, "client_id": writer, "code_verifier": v})
    wat = wt.get("access_token")
    st, r = jreq("POST", "/oauth/api/actions", {"kind": "gift.send", "params": {"to": other_name, "amount": 5}}, bearer=wat)
    check(st == 403 and r.get("error") == "insufficient_scope", "an action outside the grant's scopes: insufficient_scope")
    st, gifter = new_client(["profile", "inventory:write:currency"], name="Gift App")
    gifter = gifter.get("client_id", "") if isinstance(gifter, dict) else ""
    gv, gc = pkce()
    st, r = authorize(my_token, gifter, "profile inventory:write:currency", gc, accept=True, twofa_code=totp(key, 0))
    gcode = code_of(((r or {}).get("data") or {}).get("redirect_url", ""))[0]
    st, gt = token({"grant_type": "authorization_code", "code": gcode, "redirect_uri": REDIRECT, "client_id": gifter, "code_verifier": gv})
    gat = gt.get("access_token")
    note = "é" * 500
    st, act = jreq("POST", "/oauth/api/actions", {"kind": "gift.send", "params": {"to": other_name, "amount": 10, "message": "  " + note + "  "}}, bearer=gat)
    check(st == 201 and str(act.get("id", "")).startswith("act_"), "a 500-character gift note is accepted (got %s %r)" % (st, act))
    st, info = jreq("GET", "/oauth/actions/" + act.get("id", ""), bearer=my_token)
    kept = ((info or {}).get("data") or {}).get("params", {}).get("message")
    check(st == 200 and kept == note, "a gift note is trimmed and kept in characters, not stopped at 300 bytes (got %s)" % (len(kept) if isinstance(kept, str) else kept))
    st, r = jreq("POST", "/oauth/api/actions", {"kind": "gift.send", "params": {"to": other_name, "amount": 10, "message": "é" * 501}}, bearer=gat)
    check(st == 400 and "too long" in json.dumps(r), "a gift note past 500 characters is refused: %r" % (r,))
    st, r = jreq("POST", "/oauth/api/actions", {"kind": "follow.add", "params": {}}, bearer=wat)
    check(st == 400, "an action with bad params is refused")
    st, act = jreq("POST", "/oauth/api/actions", {"kind": "follow.add", "params": {"name": other_name}}, bearer=wat)
    aid = act.get("id", "")
    check(st == 201 and aid.startswith("act_") and act.get("approve_url", "").endswith("/oauth/approve/" + aid) and act.get("status") == "pending",
          "follow.add action created: %r" % (act,))
    st, r = jreq("GET", "/oauth/actions/" + aid, bearer=my_token)
    d = (r or {}).get("data") or {}
    check(st == 200 and d.get("kind") == "follow.add" and d.get("params", {}).get("name") == other_name and d.get("app", {}).get("name") == "Writer App",
          "actionInfo: kind, params, app")
    st, r = jreq("POST", "/oauth/actions/%s/approve" % aid, {"twofa_code": "000001" if totp(key, 0) != "000001" else "000002"}, bearer=my_token)
    check(st == 400 and query("SELECT status FROM oauth_actions WHERE action_id = '%s'" % aid) == ["pending"], "approve with a wrong code: still pending")
    check(query("SELECT count(*) FROM follows WHERE follower_id = %d" % me) == ["0"], "nothing ran yet")
    st, r = jreq("POST", "/oauth/actions/%s/approve" % aid, {"twofa_code": totp(key, 1)}, bearer=my_token)
    check(st == 200 and r.get("data", {}).get("status") == "approved", "approve with 2FA: approved (%r)" % (r,))
    check(query("SELECT count(*) FROM follows WHERE follower_id = %d" % me) == ["1"], "the follow ran")
    st, r = jreq("POST", "/oauth/actions/%s/approve" % aid, {"twofa_code": totp(key, 1)}, bearer=my_token)
    check(st == 200 and r.get("data", {}).get("status") == "approved", "a second approve does not run it again")
    st, r = jreq("GET", "/oauth/api/actions/" + aid, bearer=wat)
    check(st == 200 and r.get("status") == "approved" and "params" not in r, "the app polls its action: approved")
    st, act2 = jreq("POST", "/oauth/api/actions", {"kind": "follow.remove", "params": {"name": other_name}}, bearer=wat)
    st, r = jreq("POST", "/oauth/actions/%s/deny" % act2["id"], {}, bearer=my_token)
    check(st == 200 and r.get("data", {}).get("status") == "denied", "deny: denied")
    check(query("SELECT count(*) FROM follows WHERE follower_id = %d" % me) == ["1"], "the denied unfollow did not run")
    st, act3 = jreq("POST", "/oauth/api/actions", {"kind": "follow.remove", "params": {"name": other_name}}, bearer=wat)

    # ---- the approval page (no script)
    st, h, page = req("GET", "/oauth/approve/" + act3["id"], headers={"Cookie": cookie})
    check(st == 200 and "Unfollow " + other_name in page and 'value="approve"' in page, "approval page shows exactly what runs")

    # ---- Disconnect
    st, r = jreq("DELETE", "/account/apps/" + writer, bearer=my_token)
    check(st == 200 and r.get("data", {}).get("revoked") is True, "Disconnect: revoked")
    st, r = jreq("GET", "/oauth/userinfo", bearer=wat)
    check(st == 401, "a disconnected app's token is refused at once")
    check(query("SELECT status FROM oauth_actions WHERE action_id = '%s'" % act3["id"]) == ["expired"], "its pending actions expired")
    st, r = jreq("DELETE", "/account/apps/" + writer, bearer=my_token)
    check(st == 200 and r.get("data", {}).get("revoked") is False, "Disconnect twice: nothing left")
    st, r = jreq("GET", "/account/apps/%s/activity" % writer, bearer=my_token)
    acts = [x.get("action") for x in r.get("data") or []]
    check("disconnect" in acts and "follow.add" in acts, "the activity keeps the disconnect and the actions: %r" % (acts,))


def main():
    global DBCFG, PORT
    scratch = [sys.executable, "-I", os.path.join(ROOT, "services", "brain", "tests", "db_scratch.py"), ROOT]
    r = subprocess.run(scratch + ["create", DRIVER, SCHEMA], capture_output=True)
    if r.returncode:
        print("oauth test: %s unavailable: skipped" % DRIVER)
        sys.exit(r.returncode)
    DBCFG = json.loads(r.stdout)
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=SECRET, **{"KN_DATABASE_" + k.upper(): str(v) for k, v in DBCFG.items()})
    work = tempfile.mkdtemp(prefix="kn-oauth.", dir=os.path.join(os.path.expanduser("~"), ".cache"))
    log_path = os.path.join(work, "brain.log")
    proc = None
    try:
        subprocess.run([BRAIN, "--migrate-only"], cwd=ROOT, env=env, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        with open(log_path, "w") as log:
            proc = subprocess.Popen([BRAIN, "--brain.host=127.0.0.1", "--brain.port=0", "--brain.id=koa-%d" % os.getpid(),
                                     "--accounts.login_throttle_s=0", "--oauth.rate_per_min=1000"],
                                    cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT)
        PORT = bound_port(log_path, proc)
        n = int(time.time()) % 100000
        name, other, password = "oa%d" % n, "ob%d" % n, "oauth_pass1"
        me, tok = asyncio.run(register(PORT, name, password))
        asyncio.run(register(PORT, other, password))
        print("oauth test on %s (account %s)" % (DRIVER, me))
        scenario(PORT, me, tok, other, password, name)
    finally:
        if proc:
            proc.terminate()
            try:
                proc.wait(10)
            except subprocess.TimeoutExpired:
                proc.kill()
        subprocess.run(scratch + ["drop", DRIVER, SCHEMA], capture_output=True)
    if FAILS:
        print("oauth test: %d failure(s); brain log %s" % (len(FAILS), log_path))
        sys.exit(1)
    print("oauth test: all checks passed")


if __name__ == "__main__":
    main()
