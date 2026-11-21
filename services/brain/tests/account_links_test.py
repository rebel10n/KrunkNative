#!/usr/bin/env python3
"""Krunker v10.0.7's account links on kn_brain against a fake Discord and Twitch (services/brain/discord.c,
account_life.c; migration 0366_account_links).

  account_links_test.py <kn_brain> <repo root>
env: KN_TEST_DB_DRIVER=sqlite (default) | postgres | mysql (db_scratch.py). 77 = skip.

Discord: status (configured, guildUrl), link (session, the state, returnTo only to an allowed origin), the
callback (cancelled, expired / single-use state, linked with sealed tokens, in_use), Join Server (joined,
already a member, the expired token refreshed, DISCORD_NEEDS_RELINK), unlink (DISCORD_DELETION_PENDING while
the account is being deleted, then unlinked and the token revoked at Discord).
Twitch: the link keeps sealed tokens; the drops answer's reconnect flag (fresh: false; due and refused: the
token refreshed, false; refresh refused too: true and 'expired'; Twitch down: no verdict, false; a legacy link
without tokens: true); ["twitchAuth", true] relinks a linked account.
"""
import asyncio
import json
import os
import re
import secrets
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse
import http.client
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

try:
    import websockets
except ImportError:  # pragma: no cover
    print("python websockets missing: skip")
    sys.exit(77)

BRAIN, ROOT = sys.argv[1], os.path.abspath(sys.argv[2])
DRIVER = os.environ.get("KN_TEST_DB_DRIVER", "sqlite")
SCHEMA = "kn_test_links_%d" % os.getpid()
SECRET = "kn-links-test-%d" % os.getpid()
FAILS = []
DBCFG = {}
PORT = 0

# ---- the fake Discord / Twitch
FAKE = {"discord_users": {}, "members": set(), "revoked": [], "refreshes": 0, "join_status": None,
        "tw_valid": set(), "tw_refresh_ok": True, "tw_validate_status": None, "tw_refreshes": 0}


class Fake(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def body(self):
        n = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(n).decode() if n else ""

    def reply(self, status, obj=None):
        data = json.dumps(obj).encode() if obj is not None else b""
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_POST(self):
        form = dict(urllib.parse.parse_qsl(self.body()))
        if self.path == "/discord/oauth2/token":
            if form.get("client_id") != "dc-id" or form.get("client_secret") != "dc-secret":
                return self.reply(401, {"error": "invalid_client"})
            if form.get("grant_type") == "authorization_code" and form.get("redirect_uri") != BORROWED:
                return self.reply(400, {"error": "invalid_grant", "error_description": "Invalid \"redirect_uri\" in request."})
            if form.get("grant_type") == "authorization_code" and form.get("code", "").startswith("good-"):
                uid = form["code"][5:]
                tok = "dat-%s-%s" % (uid, secrets.token_hex(4))
                FAKE["discord_users"][tok] = uid
                return self.reply(200, {"access_token": tok, "refresh_token": "drt-" + uid, "expires_in": 604800, "token_type": "Bearer"})
            if form.get("grant_type") == "refresh_token" and form.get("refresh_token", "").startswith("drt-"):
                FAKE["refreshes"] += 1
                uid = form["refresh_token"][4:]
                tok = "dat-%s-%s" % (uid, secrets.token_hex(4))
                FAKE["discord_users"][tok] = uid
                return self.reply(200, {"access_token": tok, "refresh_token": "drt-" + uid, "expires_in": 604800})
            return self.reply(400, {"error": "invalid_grant"})
        if self.path == "/discord/oauth2/token/revoke":
            FAKE["revoked"].append(form.get("token"))
            return self.reply(200, {})
        if self.path == "/twitch/token":
            if form.get("grant_type") == "authorization_code" and form.get("code") == "tw-good":
                FAKE["tw_valid"].add("twa1")
                return self.reply(200, {"access_token": "twa1", "refresh_token": "twr1", "expires_in": 14000})
            if form.get("grant_type") == "refresh_token" and FAKE["tw_refresh_ok"]:
                FAKE["tw_refreshes"] += 1
                tok = "twa-r%d" % FAKE["tw_refreshes"]
                FAKE["tw_valid"].add(tok)
                return self.reply(200, {"access_token": tok, "refresh_token": "twr-r%d" % FAKE["tw_refreshes"], "expires_in": 14000})
            return self.reply(400, {"status": 400, "message": "Invalid refresh token"})
        self.reply(404, {})

    def do_GET(self):
        auth = self.headers.get("Authorization", "")
        if self.path == "/discord/users/@me":
            uid = FAKE["discord_users"].get(auth[7:]) if auth.startswith("Bearer ") else None
            if not uid:
                return self.reply(401, {"message": "401: Unauthorized"})
            return self.reply(200, {"id": uid, "username": "user" + uid, "global_name": "User " + uid, "avatar": "av" + uid})
        if self.path == "/twitch/helix/users":
            if auth[7:] not in FAKE["tw_valid"]:
                return self.reply(401, {})
            return self.reply(200, {"data": [{"id": "777", "login": "kbo_streamer"}]})
        if self.path == "/twitch/validate":
            if FAKE["tw_validate_status"]:
                return self.reply(FAKE["tw_validate_status"], {})
            ok = auth.startswith("OAuth ") and auth[6:] in FAKE["tw_valid"]
            return self.reply(200 if ok else 401, {"client_id": "tw-id"} if ok else {"status": 401})
        self.reply(404, {})

    def do_PUT(self):
        m = re.match(r"^/discord/guilds/G1/members/(\w+)$", self.path)
        data = json.loads(self.body() or "{}")
        if not m or self.headers.get("Authorization") != "Bot bot-tok":
            return self.reply(401, {})
        if FAKE["join_status"]:
            return self.reply(FAKE["join_status"], {"message": "Unknown"})
        if FAKE["discord_users"].get(data.get("access_token")) != m.group(1):
            return self.reply(403, {"message": "Invalid OAuth2 access token"})
        if m.group(1) in FAKE["members"]:
            return self.reply(204)
        FAKE["members"].add(m.group(1))
        return self.reply(201, {"user": {"id": m.group(1)}})


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what, flush=True)
    if not cond:
        FAILS.append(what)


def free_port():
    import socket
    with socket.socket() as so:
        so.bind(("127.0.0.1", 0))
        return so.getsockname()[1]


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


def past(seconds):
    return ("datetime('now', '-%d seconds')" % seconds) if DRIVER == "sqlite" else ("now() - interval '%d seconds'" % seconds)


def req(method, path, body=None, bearer=None):
    h = {}
    data = None
    if body is not None:
        data = json.dumps(body).encode()
        h["Content-Type"] = "application/json"
    if bearer:
        h["Authorization"] = "Bearer " + bearer
    c = http.client.HTTPConnection("127.0.0.1", PORT, timeout=20)
    c.request(method, path, body=data, headers=h)
    r = c.getresponse()
    text = r.read().decode("utf-8", "replace")
    hdrs = {k.lower(): v for k, v in r.getheaders()}
    c.close()
    try:
        obj = json.loads(text) if text else None
    except ValueError:
        obj = text
    return r.status, hdrs, obj


def err_code(r):
    return ((r or {}).get("error") or {}).get("code") if isinstance(r, dict) else None


class Social:
    def __init__(self, port):
        self.url = "ws://127.0.0.1:%d/social/ws?enc=json" % port
        self.inbox = []

    async def __aenter__(self):
        self.ws = await websockets.connect(self.url, max_size=4 << 20)
        return self

    async def __aexit__(self, *a):
        await self.ws.close()

    async def call(self, label, *frame):
        if frame:
            await self.ws.send(json.dumps(list(frame)))
        deadline = time.time() + 15
        while True:
            for i, f in enumerate(self.inbox):
                if f[0] == label:
                    return self.inbox.pop(i)
            self.inbox.append(json.loads(await asyncio.wait_for(self.ws.recv(), deadline - time.time())))


async def register(name, password):
    async with Social(PORT) as s:
        r = await s.call("a", "a", 0, [name, password, 0], "captcha")
        return r[2], r[5]


# the production setup (deploy/ansible kn_brain_discord_*): another site's registered callback, whose edge sends a
# state with the prefix on to /discord/callback; the token exchange must name that same redirect_uri
BORROWED = "https://abulxt.example/api/account/discord/callback"
PREFIX = "kbo-"


def state_of(url):
    return urllib.parse.parse_qs(urllib.parse.urlparse(url).query).get("state", [""])[0]


def discord(me, tok, other_tok):
    st, _, r = req("GET", "/discord/status", bearer=tok)
    d = (r or {}).get("data") or {}
    check(st == 200 and d.get("configured") is True and d.get("linked") is False and d.get("guildUrl") == "https://discord.gg/kbo-test",
          "status: configured, not linked, guildUrl: %r" % (d,))
    st, _, r = req("POST", "/discord/link", {"returnTo": None})
    check(st == 401, "link needs a session")

    # cancelled at Discord, then the state is spent
    st, _, r = req("POST", "/discord/link", {"returnTo": "https://evil.example"}, bearer=tok)
    url = ((r or {}).get("data") or {}).get("url", "")
    q = urllib.parse.parse_qs(urllib.parse.urlparse(url).query)
    check(st == 200 and url.startswith("http://127.0.0.1:%d/discord/oauth2/authorize?" % FAKE_PORT) and q.get("client_id") == ["dc-id"]
          and q.get("scope") == ["identify guilds.join"] and q.get("redirect_uri") == [BORROWED]
          and q.get("state", [""])[0].startswith(PREFIX) and len(q.get("state", [""])[0]) >= 40 + len(PREFIX),
          "link: the authorize URL (client, scopes, the configured redirect, the prefixed state)")
    state = state_of(url)
    st, h, page = req("GET", "/discord/callback?error=access_denied&state=" + state)
    check(st == 400 and "cancelled" in str(page) and "location" not in h, "callback error=access_denied: cancelled, no redirect to a foreign returnTo")
    st, h, page = req("GET", "/discord/callback?code=good-1001&state=" + state)
    check(st == 400 and "expired" in str(page), "the state is single use: expired")
    st, h, page = req("GET", "/discord/callback?code=good-1001&state=nope")
    check(st == 400 and "expired" in str(page), "an unknown state: expired")

    # linked, back to the allowed returnTo
    back = "http://127.0.0.1:%d/game" % PORT
    st, _, r = req("POST", "/discord/link", {"returnTo": back}, bearer=tok)
    state = state_of(r["data"]["url"])
    st, h, _ = req("GET", "/discord/callback?code=good-1001&state=" + state)
    check(st == 303 and h.get("location") == back + "?discord_link=linked", "callback: linked, 303 to returnTo?discord_link=linked (%s %r)"
          % (st, h.get("location")))
    st, _, r = req("GET", "/discord/status", bearer=tok)
    d = (r or {}).get("data") or {}
    check(d.get("linked") is True and d.get("id") == "1001" and d.get("username") == "user1001" and d.get("globalName") == "User 1001"
          and d.get("avatar") == "av1001" and d.get("inGuild") is False, "status after the link: %r" % (d,))
    enc = query("SELECT token_enc FROM discord_links WHERE account_id = %d" % me)
    check(enc and enc[0].startswith("v1.") and "dat-" not in enc[0], "the Discord token is sealed at rest")

    # the same Discord user on another account: in_use
    st, _, r = req("POST", "/discord/link", {}, bearer=other_tok)
    st, h, page = req("GET", "/discord/callback?code=good-1001&state=" + state_of(r["data"]["url"]))
    check(st == 400 and "another Box Office account" in str(page), "the same Discord user on another account: in_use")

    # Join Server
    st, _, r = req("POST", "/discord/join", bearer=tok)
    check(st == 200 and r.get("data", {}).get("joined") is True, "join: joined (%s %r)" % (st, r))
    st, _, r = req("POST", "/discord/join", bearer=tok)
    check(st == 200 and r.get("data", {}).get("joined") is False, "join again: already a member")
    st, _, r = req("GET", "/discord/status", bearer=tok)
    check(r["data"].get("inGuild") is True, "status: inGuild")
    query("UPDATE discord_links SET token_expires_at = %s WHERE account_id = %d" % (past(10), me))
    before = query("SELECT token_enc FROM discord_links WHERE account_id = %d" % me)
    st, _, r = req("POST", "/discord/join", bearer=tok)
    time.sleep(0.5)
    after = query("SELECT token_enc FROM discord_links WHERE account_id = %d" % me)
    check(st == 200 and FAKE["refreshes"] == 1 and before != after, "an expired token is refreshed (and resealed) before the join")
    FAKE["join_status"] = 403
    st, _, r = req("POST", "/discord/join", bearer=tok)
    check(st == 409 and err_code(r) == "DISCORD_NEEDS_RELINK", "Discord refuses the user's token: DISCORD_NEEDS_RELINK")
    FAKE["join_status"] = 500
    st, _, r = req("POST", "/discord/join", bearer=tok)
    check(st == 502 and err_code(r) == "DISCORD_JOIN_FAILED", "Discord fails: DISCORD_JOIN_FAILED")
    FAKE["join_status"] = None
    st, _, r = req("POST", "/discord/join", bearer=other_tok)
    check(st == 400 and err_code(r) == "DISCORD_NOT_LINKED", "join without a link: DISCORD_NOT_LINKED")

    # deletion pending: unlink and relink refused, then the unlink
    query("UPDATE accounts SET delete_requested_at = %s WHERE id = %d" % (past(1), me))
    st, _, r = req("POST", "/discord/unlink", bearer=tok)
    check(st == 409 and err_code(r) == "DISCORD_DELETION_PENDING", "unlink while the account is being deleted: DISCORD_DELETION_PENDING")
    st, _, r = req("POST", "/discord/link", {}, bearer=tok)
    check(st == 409 and err_code(r) == "DISCORD_DELETION_PENDING", "link while the account is being deleted: DISCORD_DELETION_PENDING")
    check(query("SELECT count(*) FROM discord_links WHERE account_id = %d" % me) == ["1"], "the link is kept")
    query("UPDATE accounts SET delete_requested_at = NULL WHERE id = %d" % me)
    st, _, r = req("POST", "/discord/unlink", bearer=tok)
    check(st == 200 and r.get("data", {}).get("unlinked") is True, "unlink: unlinked")
    for _ in range(50):
        if FAKE["revoked"]:
            break
        time.sleep(0.1)
    check(len(FAKE["revoked"]) == 1 and FAKE["revoked"][0].startswith("dat-1001-"), "the token is revoked at Discord")
    st, _, r = req("GET", "/discord/status", bearer=tok)
    check(r["data"].get("linked") is False, "status after the unlink: not linked")
    st, _, r = req("POST", "/discord/unlink", bearer=tok)
    check(st == 200 and r.get("data", {}).get("unlinked") is False, "unlink again: nothing to unlink")


async def drops(s):
    r = await s.call("twitchDropsResponse", "getTwitchDrops")
    return r[2] if len(r) > 2 else None


async def twitch(me, name, password):
    async with Social(PORT) as s:
        await s.call("a", "a", 1, [name, password, None], "captcha")
        r = await s.call("twitchAuth", "twitchAuth")
        url = r[1] if len(r) > 1 else None
        check(isinstance(url, str) and "state=" in url, "twitchAuth: the authorize URL")
        st, h, page = req("GET", "/twitch/callback?code=tw-good&state=" + state_of(url))
        check(st == 200 and "kbo_streamer" in str(page), "Twitch callback: linked")
        await s.call("twitchVerifyResponse")
        row = query("SELECT token_enc || '|' || coalesce(refresh_enc, '') || '|' || coalesce(token_state, '') FROM twitch_links WHERE account_id = %d" % me)
        parts = row[0].split("|") if row else ["", "", ""]
        check(parts[0].startswith("v1.") and parts[1].startswith("v1.") and "twa1" not in row[0] and parts[2] == "ok",
              "the Twitch tokens are sealed at rest, state ok")
        check(await drops(s) is False, "drops of a fresh link: reconnect false")

        query("UPDATE twitch_links SET token_checked_at = %s WHERE account_id = %d" % (past(7200), me))
        FAKE["tw_valid"].discard("twa1")  # Twitch expired the access token
        check(await drops(s) is False, "due, access refused, refresh works: reconnect false")
        for _ in range(50):
            if FAKE["tw_refreshes"] == 1:
                break
            await asyncio.sleep(0.1)
        check(FAKE["tw_refreshes"] == 1, "the token was refreshed")

        query("UPDATE twitch_links SET token_checked_at = %s WHERE account_id = %d" % (past(7200), me))
        FAKE["tw_validate_status"] = 503
        check(await drops(s) is False, "Twitch down: no verdict, reconnect false")
        FAKE["tw_validate_status"] = None

        query("UPDATE twitch_links SET token_checked_at = %s WHERE account_id = %d" % (past(7200), me))
        FAKE["tw_valid"].clear()
        FAKE["tw_refresh_ok"] = False
        first = await drops(s)
        for _ in range(50):
            if query("SELECT token_state FROM twitch_links WHERE account_id = %d" % me) == ["expired"]:
                break
            await asyncio.sleep(0.1)
        second = await drops(s)
        check(first is True and second is True, "access and refresh refused: reconnect true (%r, %r)" % (first, second))
        check(query("SELECT token_state FROM twitch_links WHERE account_id = %d" % me) == ["expired"], "token_state expired")

        query("UPDATE twitch_links SET token_enc = NULL, refresh_enc = NULL, token_state = NULL WHERE account_id = %d" % me)
        check(await drops(s) is True, "a link without tokens (made before 0366): reconnect true")

        r = await s.call("twitchAuth", "twitchAuth")
        check(len(r) > 2 and r[1] is None, "twitchAuth of a linked account without force: refused (%r)" % (r,))
        r = await s.call("twitchAuth", "twitchAuth", True)
        check(isinstance(r[1], str) and "state=" in r[1], "[\"twitchAuth\", true]: the reconnect gets a new authorize URL")
        FAKE["tw_refresh_ok"] = True
        st, h, page = req("GET", "/twitch/callback?code=tw-good&state=" + state_of(r[1]))
        await s.call("twitchVerifyResponse")
        check(st == 200 and query("SELECT token_state FROM twitch_links WHERE account_id = %d" % me) == ["ok"], "reconnected: token_state ok")
        check(await drops(s) is False, "after the reconnect: reconnect false")


FAKE_PORT = 0


def main():
    global DBCFG, PORT, FAKE_PORT
    scratch = [sys.executable, "-I", os.path.join(ROOT, "services", "brain", "tests", "db_scratch.py"), ROOT]
    r = subprocess.run(scratch + ["create", DRIVER, SCHEMA], capture_output=True)
    if r.returncode:
        print("account links test: %s unavailable: skipped" % DRIVER)
        sys.exit(r.returncode)
    DBCFG = json.loads(r.stdout)
    fake = ThreadingHTTPServer(("127.0.0.1", 0), Fake)
    FAKE_PORT = fake.server_address[1]
    threading.Thread(target=fake.serve_forever, daemon=True).start()
    f = "http://127.0.0.1:%d" % FAKE_PORT
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=SECRET, **{"KN_DATABASE_" + k.upper(): str(v) for k, v in DBCFG.items()})
    work = tempfile.mkdtemp(prefix="kn-links.", dir=os.path.join(os.path.expanduser("~"), ".cache"))
    log_path = os.path.join(work, "brain.log")
    proc = None
    try:
        subprocess.run([BRAIN, "--migrate-only"], cwd=ROOT, env=env, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        with open(log_path, "w") as log:
            # its own port and public_url: the redirect URI and returnTo origin come from brain.public_url, and a
            # config.json copied from config.example.json (CI) sets it to :5050, which port 0 would keep
            port = free_port()
            proc = subprocess.Popen([BRAIN, "--brain.host=127.0.0.1", "--brain.port=%d" % port,
                                     "--brain.public_url=http://127.0.0.1:%d" % port, "--brain.id=kln-%d" % os.getpid(),
                                     "--accounts.login_throttle_s=0", "--accounts.secrets_key=links-test-key",
                                     "--accounts.discord.client_id=dc-id", "--accounts.discord.client_secret=dc-secret",
                                     "--accounts.discord.redirect_uri=" + BORROWED, "--accounts.discord.state_prefix=" + PREFIX,
                                     "--accounts.discord.bot_token=bot-tok", "--accounts.discord.guild_id=G1",
                                     "--accounts.discord.guild_url=https://discord.gg/kbo-test",
                                     "--accounts.discord.api_url=" + f + "/discord", "--accounts.discord.authorize_url=" + f + "/discord/oauth2/authorize",
                                     "--streams.twitch.client_id=tw-id", "--streams.twitch.client_secret=tw-secret",
                                     "--streams.twitch.auth_url=" + f + "/twitch/token", "--streams.twitch.api_url=" + f + "/twitch/helix",
                                     "--streams.twitch.validate_url=" + f + "/twitch/validate",
                                     "--streams.twitch.authorize_url=" + f + "/twitch/authorize"],
                                    cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT)
        PORT = bound_port(log_path, proc)
        n = int(time.time()) % 100000
        name, password = "la%d" % n, "links_pass1"
        me, tok = asyncio.run(register(name, password))
        _, other_tok = asyncio.run(register("lb%d" % n, password))
        print("account links test on %s (account %s)" % (DRIVER, me))
        discord(me, tok, other_tok)
        asyncio.run(twitch(me, name, password))
    finally:
        if proc:
            proc.terminate()
            try:
                proc.wait(10)
            except subprocess.TimeoutExpired:
                proc.kill()
        fake.shutdown()
        subprocess.run(scratch + ["drop", DRIVER, SCHEMA], capture_output=True)
    if FAILS:
        print("account links test: %d failure(s); brain log %s" % (len(FAILS), log_path))
        sys.exit(1)
    print("account links test: all checks passed")


if __name__ == "__main__":
    main()
