#!/usr/bin/env python3
"""Account security on kn_brain (doc/backend.md 7.9; account_security.c, db_security.c, migration
0109_account_security) end to end, with services/common/tests/fake_smtp.py as the mail server
(STARTTLS, a self-signed certificate, AUTH): register with an email -> the verification mail is
captured -> its link verifies the address ("emailS" pushed, verified-gated features open). Plus the
token rules (reuse, expiry, a changed address drops old links), the resend rate limits (cooldown,
per-account day cap), mail_log (statuses, Message-ID, the server's error, no tokens), a rejected
recipient, v7.1.4's TOTP 2FA lock after 10 wrong codes, and KrunkNative's password reset by
emailed code (rpr / rpc / rps, the /auth/reset-password page, no account enumeration).

  account_security_test.py <kn_brain> <repo root> [database]
Database: argument, else $KN_MM_TEST_DATABASE, else config/config.json's database.name; every run
uses its own schema / scratch database, an ephemeral brain port and a free SMTP port.
KN_TEST_DB_DRIVER=sqlite|mysql runs on that store driver (db_scratch.py). 77 = skip (no database,
python websockets or openssl CLI).
"""
import asyncio
import base64
import email
import email.policy
import hashlib
import hmac
import json
import os
import re
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

try:
    import websockets
except ImportError:  # pragma: no cover
    print("python websockets missing: skip")
    sys.exit(77)

BRAIN, ROOT = sys.argv[1], sys.argv[2]
sys.path.insert(0, os.path.join(ROOT, "services", "common", "tests"))
from fake_smtp import FakeSMTP, make_cert  # noqa: E402

DRIVER = os.environ.get("KN_TEST_DB_DRIVER", "postgres")
DB = sys.argv[3] if len(sys.argv) > 3 else os.environ.get("KN_MM_TEST_DATABASE", "")
CONFIG_DB = {}
try:
    with open(os.path.join(ROOT, "config", "config.json")) as fp:
        CONFIG_DB = json.load(fp).get("database", {})
except (OSError, ValueError):
    pass
if not DB:
    DB = CONFIG_DB.get("name", "")
if not DB and DRIVER == "postgres":
    print("account security test: no database configured; skipped")
    sys.exit(77)
if not shutil.which("openssl"):
    print("no openssl CLI: skip")
    sys.exit(77)
SCHEMA = "kn_test_ksec_%d" % os.getpid()
SECRET = "kn-sec-test-%d" % os.getpid()
SMTP_USER, SMTP_PASS = "mailer@caan.test", "fake-smtp-pass-%d" % os.getpid()
FAILS = []
DBCFG = {}


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what, flush=True)
    if not cond:
        FAILS.append(what)


def bound_port(log_path, proc):
    for _ in range(200):
        if proc.poll() is not None:
            raise RuntimeError("kn_brain exited early (%s)" % log_path)
        with open(log_path) as fp:
            m = re.search(r"listening on http://127\.0\.0\.1:(\d+) \(ws", fp.read().split("== start ")[-1])
        if m:
            return int(m.group(1))
        time.sleep(0.05)
    raise RuntimeError("kn_brain did not start listening")


def query(sql):
    """First column of every row of `sql` on the test database, as text (also runs updates)."""
    if DRIVER == "sqlite":
        con = sqlite3.connect(DBCFG["path"])
        try:
            rows = [str(r[0]) for r in con.execute(sql).fetchall()]
            con.commit()
            return rows
        finally:
            con.close()
    if DRIVER == "postgres":
        get = lambda k, d: os.environ.get("KN_DATABASE_" + k.upper(), str(CONFIG_DB.get(k, d)))
        env = dict(os.environ, PGPASSWORD=get("password", ""))
        r = subprocess.run(["psql", "-h", get("host", "127.0.0.1"), "-p", get("port", 5432), "-U", get("user", "krunknative"),
                            "-d", DB, "-v", "ON_ERROR_STOP=1", "-qAt", "-c", "SET search_path TO %s; %s" % (SCHEMA, sql)],
                           env=env, capture_output=True, timeout=30)
        return r.stdout.decode().split("\n")[:-1] if r.returncode == 0 else None
    tool = shutil.which("mariadb") or shutil.which("mysql")
    r = subprocess.run([tool, "--no-defaults", "--protocol=TCP", "-h", DBCFG["host"], "-P", str(DBCFG["port"]), "-u", DBCFG["user"],
                        "-N", "-B", DBCFG["name"], "-e", sql], env=dict(os.environ, MYSQL_PWD=DBCFG.get("password", "")),
                       capture_output=True, timeout=30)
    return r.stdout.decode().split("\n")[:-1] if r.returncode == 0 else None


def totp(key_b32, offset=0):
    key = base64.b32decode(key_b32 + "=" * (-len(key_b32) % 8))
    mac = hmac.new(key, struct.pack(">Q", int(time.time() // 30) + offset), hashlib.sha1).digest()
    o = mac[-1] & 15
    return "%06d" % ((struct.unpack(">I", mac[o:o + 4])[0] & 0x7FFFFFFF) % 1000000)


class Social:
    def __init__(self, url):
        self.url = url + "?enc=json"
        self.inbox = []

    async def __aenter__(self):
        self.ws = await websockets.connect(self.url, max_size=4 << 20)
        return self

    async def __aexit__(self, *a):
        await self.ws.close()

    async def send(self, *frame):
        await self.ws.send(json.dumps(list(frame)))

    async def recv(self, label, timeout=10.0):
        deadline = time.time() + timeout
        while True:
            for i, f in enumerate(self.inbox):
                if f[0] == label:
                    return self.inbox.pop(i)
            left = deadline - time.time()
            if left <= 0:
                raise AssertionError("timeout waiting for %r (inbox %r)" % (label, [f[0] for f in self.inbox]))
            self.inbox.append(json.loads(await asyncio.wait_for(self.ws.recv(), left)))

    async def call(self, label, *frame):
        await self.send(*frame)
        return await self.recv(label)


def log_rows(account_id):
    cols = ("CONCAT(status, '|', coalesce(message_id, ''), '|', to_addr)" if DRIVER == "mysql"
            else "status || '|' || coalesce(message_id, '') || '|' || to_addr")
    return query("SELECT %s FROM mail_log WHERE account_id = %d ORDER BY id" % (cols, account_id))


def mail_parts(msg):
    m = email.message_from_bytes(msg["data"], policy=email.policy.default)
    text = m.get_body(("plain",)).get_content()
    html = m.get_body(("html",)).get_content() if m.get_body(("html",)) else ""
    link = re.search(r"https?://\S+/auth/verify-email\?token=([A-Za-z0-9_-]+)", text)
    return m, text, html, link.group(1) if link else None


def http_get(port, path):
    try:
        with urllib.request.urlopen("http://127.0.0.1:%d%s" % (port, path), timeout=10) as r:
            return r.status, r.read().decode()
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode()


async def scenario(port, smtp):
    ws_url = "ws://127.0.0.1:%d/social/ws" % port
    stamp = os.getpid() % 100000
    name = "sec%d" % stamp
    addr = "player%d@example.org" % stamp
    async with Social(ws_url) as s:
        r = await s.call("a", "a", 0, [name + "x", "pass_word1", None, "not-an-email"], "c")
        check(r[1] == "Invalid Email", "register with an invalid email refused: %r" % r[1])
        r = await s.call("a", "a", 0, [name, "pass_word1", None, "  Player%d@Example.org " % stamp], "c")
        check(r[1] is None and r[3] == name, "register with an email: %r" % r[1])
        my_id = r[2]
        mails = smtp.wait_messages(1)
        check(len(mails) == 1, "verification mail captured by the SMTP server")
        mail = mails[0]
        check(mail["tls"] and mail["auth_user"] == SMTP_USER and mail["rcpt"] == [addr], "over STARTTLS, authenticated, to the lower-cased address")
        check(mail["mail_from"] == "noreply@caan.edu", "envelope sender noreply@caan.edu")
        m, text, html, token = mail_parts(mail)
        check(str(m["From"]) == "Krunker Box Office <noreply@caan.edu>" and str(m["Subject"]) == "Verify your Krunker Box Office email", "From / Subject")
        check(m["Message-ID"] and m["Date"] and m.get_content_type() == "multipart/alternative", "Message-ID, Date, text + HTML")
        check(token and name in text and "expires in 24 hours" in text and token in html, "link + username in both parts")
        st = await s.recv("emailS")
        check(st[1]["email"] == addr and st[1]["verified"] == 0 and st[1]["pending"] == 1, "emailS after register: %r" % st[1])
        rows = None
        for _ in range(50):
            rows = log_rows(my_id)
            if rows and rows[0].startswith("sent|"):
                break
            time.sleep(0.1)
        check(rows and rows[0] == "sent|%s|%s" % (m["Message-ID"], addr), "mail_log: sent with the Message-ID: %r" % rows)
        dump = " ".join(query("SELECT * FROM mail_log") or [])
        check(token not in dump, "no token in mail_log")

        # verified-gated features (accounts.email.require_verified: chat, gift, clan)
        r = await s.call("po", "ict", 1, "hi")
        check(r[2] == "Verify your email to use this feature", "chat needs a verified email: %r" % r)
        r = await s.call("po", "cln", 1, "SEC")
        check(r[2] == "Verify your email to use this feature", "clan create needs a verified email")
        r = await s.call("emailv", "emailv")
        check(r[1] and r[1].startswith("Please wait") and r[3]["pending"] == 1, "resend inside the cooldown refused: %r" % r[1])

        # the link
        status, body = http_get(port, "/auth/verify-email?token=" + token)
        check(status == 200 and "Email verified" in body and addr in body, "verify link -> 200 Email verified")
        st = await s.recv("emailS")
        check(st[1]["verified"] == 1 and st[1]["email"] == addr, "emailS pushed to the open socket: %r" % st[1])
        r = await s.call("chf", "ict", my_id, "note to self")
        check(r[0] == "chf", "chat passes the gate once verified")
        status, body = http_get(port, "/auth/verify-email?token=" + token)
        check(status == 400 and "already verified" in body, "link reuse -> already verified")
        status, body = http_get(port, "/auth/verify-email?token=" + "x" * 43)
        check(status == 400 and "invalid" in body, "unknown token -> invalid")
        r = await s.call("emailv", "emailv")
        check(r[1] == "Email already verified", "resend when verified refused")
        r = await s.call("cemail", "cemail", addr.upper())
        check(r[1] == "Email already verified", "same verified address refused")

    async with Social(ws_url) as s2:  # a fresh login sees the verified state
        r = await s2.call("a", "a", 1, [name, "pass_word1", None], "c")
        st = await s2.recv("emailS")
        check(st[1]["verified"] == 1, "emailS after login: verified")

        # change the address: unverified again, the new link works, older links are void
        await asyncio.sleep(1.1)  # accounts.email.resend_cooldown_s = 1 in this test
        r = await s2.call("cemail", "cemail", "second%d@example.org" % stamp)
        check(r[1] is None and r[2].startswith("Verification email sent") and r[3]["verified"] == 0, "cemail: %r %r" % (r[1:], int(time.time() * 1000)))
        mails = smtp.wait_messages(2)
        _, _, _, token2 = mail_parts(mails[-1])
        await asyncio.sleep(1.1)
        r = await s2.call("cemail", "cemail", "third%d@example.org" % stamp)
        mails = smtp.wait_messages(3)
        _, _, _, token3 = mail_parts(mails[-1])
        status, body = http_get(port, "/auth/verify-email?token=" + token2)
        check(status == 400 and "no longer valid" in body, "a link of a replaced address is void")
        query("UPDATE email_tokens SET expires_at = created_at WHERE used_at IS NULL")
        status, body = http_get(port, "/auth/verify-email?token=" + token3)
        check(status == 400 and "expired" in body, "an expired link is refused")

        # per-account daily cap (accounts.email.max_per_account_day = 3 here)
        await asyncio.sleep(1.1)
        r = await s2.call("emailv", "emailv")
        check(r[1] == "Too many emails today. Please Try Later", "daily cap per account: %r" % r[1])

    # a recipient the server rejects -> mail_log failed with the server's reply
    async with Social(ws_url) as s3:
        r = await s3.call("a", "a", 0, [name + "b", "pass_word1", None, "bounce%d@example.org" % stamp], "c")
        bid = r[2]
        await s3.recv("emailS")
        st = None
        for _ in range(50):
            st = query("SELECT error FROM mail_log WHERE account_id = %d AND status = 'failed'" % bid)
            if st:
                break
            time.sleep(0.1)
        check(st and "550" in st[0] and SMTP_PASS not in st[0], "rejected recipient: mail_log failed with the 550 reply: %r" % st)

        # v7.1.4 2FA lock: 10 wrong codes within 15 minutes
        r = await s3.call("c2fa", "a2FA")
        key = re.search(r"secret=([A-Z2-7]+)", r[3]).group(1)
        r = await s3.call("c2fa", "a2FAc", totp(key, 0))
        check(r[1] == 2 and r[4] is True, "2FA on")
        # the auth class (doc/social-rate-limit.md): a burst of 10 frames, then 0.5 a second per socket.
        # Each code waits for its token, so the lock is reached through the limiter, not around it.
        for i in range(10):
            await asyncio.sleep(2.1)
            r = await s3.call("c2fa", "d2FAc", "%06d" % ((int(totp(key, 0)) + 1 + i) % 1000000))
        check(r[5] == "Invalid Code", "wrong codes counted")
        await asyncio.sleep(2.1)
        r = await s3.call("c2fa", "d2FAc", totp(key, 1))
        check(r[5] == "Too many attempts. Please Try Later", "locked after 10 wrong codes: %r" % r[1:])
        n = query("SELECT twofa_failures FROM accounts WHERE id = %d" % bid)
        check(n == ["10"], "failures recorded: %r" % n)
    return addr


def reset_code(msg):
    """(code as shown, link token) of a password reset mail."""
    m = email.message_from_bytes(msg["data"], policy=email.policy.default)
    text = m.get_body(("plain",)).get_content()
    code = re.search(r"code ([A-Z2-7]{4}-[A-Z2-7]{4}-[A-Z2-7]{4})", text)
    link = re.search(r"/auth/reset-password\?token=([A-Z2-7]{12})", text)
    return m, text, code.group(1) if code else None, link.group(1) if link else None


def http_post_form(port, path, fields):
    data = urllib.parse.urlencode(fields).encode()
    req = urllib.request.Request("http://127.0.0.1:%d%s" % (port, path), data=data, method="POST",
                                 headers={"Content-Type": "application/x-www-form-urlencoded"})
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status, r.read().decode()
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode()


async def reset_scenario(port, smtp):
    """Forgot password: ["rpr"] -> the mailed code -> ["rpc"] / ["rps"] (and the link's page)."""
    ws_url = "ws://127.0.0.1:%d/social/ws" % port
    stamp = os.getpid() % 100000
    name, addr = "rst%d" % stamp, "reset%d@example.org" % stamp
    async with Social(ws_url) as s:
        before = len(smtp.messages)
        r = await s.call("a", "a", 0, [name, "pass_word1", None, addr], "c")
        check(r[1] is None, "reset: register %s" % name)
        await s.recv("emailS")
        mails = smtp.wait_messages(before + 1)  # the verification mail
        _, _, _, vtoken = mail_parts(mails[-1])
        await asyncio.sleep(1.1)
        r = await s.call("rpr", "rpr", name)
        check(r[1] is None and "If that account" in r[2], "rpr for an unverified address: the neutral text")
        await asyncio.sleep(0.5)
        check(len(smtp.messages) == before + 1, "no reset mail while the address is unverified")
        status, _ = http_get(port, "/auth/verify-email?token=" + (vtoken or ""))
        check(status == 200, "reset: address verified")
    async with Social(ws_url) as g:  # a guest socket, as the login window has
        before = len(smtp.messages)
        r = await g.call("rpr", "rpr", "nobody_%d" % os.getpid())
        check(r[1] is None and "If that account has a verified email" in r[2], "rpr for an unknown name: the neutral text")
        r = await g.call("rpr", "rpr", "unknown%d@example.org" % os.getpid())
        check(r[1] is None and "If that account" in r[2], "rpr for an unknown address: the same text")
        await asyncio.sleep(0.5)
        check(len(smtp.messages) == before, "no mail for unknown accounts")
        await asyncio.sleep(1.1)  # resend_cooldown_s = 1
        r = await g.call("rpr", "rpr", addr.upper())
        check(r[1] is None and "If that account" in r[2], "rpr by the verified address")
        mails = smtp.wait_messages(before + 1)
        check(len(mails) == before + 1 and mails[-1]["rcpt"] == [addr], "reset mail to the verified address")
        m, text, code, link_token = reset_code(mails[-1])
        check(str(m["Subject"]) == "Reset your Krunker Box Office password" and name in text, "reset mail subject + account name")
        check(code and link_token and code.replace("-", "") == link_token, "reset mail: code %r and link" % code)
        check("expires in 30 minutes" in text, "reset code lifetime 30 minutes")
        dump = " ".join(query("SELECT * FROM email_tokens") or []) + " ".join(query("SELECT * FROM mail_log") or [])
        check(code and code.replace("-", "") not in dump, "no reset code stored in clear")
        r = await g.call("rpc", "rpc", "AAAA-AAAA-AAAA")
        check(r[1] == "Invalid or expired code.", "rpc with a wrong code refused: %r" % r[1])
        r = await g.call("rpc", "rpc", code.lower())
        check(r[1] is None and r[2] == name, "rpc accepts the code (any case): %r" % r[1:])
        r = await g.call("rps", "rps", code, "new_pass_2", "new_pass_X")
        check(r[1] == "Passwords do not match.", "rps: mismatch refused")
        r = await g.call("rps", "rps", code, "x", "x")
        check(r[1] and r[1].startswith("Password must be"), "rps: too short refused: %r" % r[1])
        r = await g.call("rps", "rps", code, "new_pass_2", "new_pass_2")
        check(r[1] is None and r[3] == name and len(r[4] or "") >= 32, "rps sets the password and returns a session: %r" % r[1:3])
        token = r[4]
        r = await g.call("rps", "rps", code, "new_pass_3", "new_pass_3")
        check(r[1] == "Invalid or expired code.", "a used code is refused")
    async with Social(ws_url) as s:
        r = await s.call("a", "a", 1, [name, "new_pass_2", None], "c")
        check(r[1] is None and r[3] == name, "login with the new password: %r" % r[1:4])
        r2 = await s.call("a", "a", 1, [name, None, token], "c")
        check(r2[1] is None, "the session from rps logs in")

    # the mailed link's page: GET shows the form, POST sets the password
    async with Social(ws_url) as g:
        await asyncio.sleep(1.1)
        before = len(smtp.messages)
        r = await g.call("rpr", "rpr", name)
        check(r[1] is None, "rpr by username")
        mails = smtp.wait_messages(before + 1)
        _, _, code2, tok2 = reset_code(mails[-1])
    status, body = http_get(port, "/auth/reset-password?token=" + (tok2 or ""))
    check(status == 200 and 'name="password"' in body and (tok2 or "?") in body, "the link's page shows the form")
    status, body = http_get(port, "/auth/reset-password?token=%3Cscript%3E")
    check(status == 400 and "<script>" not in body, "a bad link token is refused (escaped)")
    status, body = http_post_form(port, "/auth/reset-password", {"token": tok2, "password": "web_pass_9", "repeat": "web_pass_8"})
    check(status == 400 and "do not match" in body and 'name="password"' in body, "page: mismatch keeps the form")
    status, body = http_post_form(port, "/auth/reset-password", {"token": tok2, "password": "web_pass_9", "repeat": "web_pass_9"})
    check(status == 200 and "Password changed" in body, "page: password changed")
    async with Social(ws_url) as s:
        r = await s.call("a", "a", 1, [name, "web_pass_9", None], "c")
        check(r[1] is None, "login with the password set on the page: %r" % r[1:4])
        r2 = await s.call("a", "a", 1, [name, None, token], "c")
        check(r2[1], "the reset revoked the older sessions: %r" % r2[1])
        r = await s.call("a", "a", 1, [name, "pass_word1", None], "c")
        check(r[1] == "Invalid login.", "the original password no longer works: %r" % r[1])


def main():
    global DBCFG
    if DRIVER == "postgres":
        pg = [sys.executable, "-I", os.path.join(ROOT, "services", "brain", "tests", "pg_schema.py"), ROOT, DB]
        rc = subprocess.run(pg + ["create", SCHEMA]).returncode
        if rc:
            sys.exit(rc)
        dbenv = {"KN_DATABASE_NAME": DB, "KN_DATABASE_SEARCH_PATH": SCHEMA}
        drop = pg + ["drop", SCHEMA]
    else:
        scratch = [sys.executable, "-I", os.path.join(ROOT, "services", "brain", "tests", "db_scratch.py"), ROOT]
        r = subprocess.run(scratch + ["create", DRIVER, SCHEMA], capture_output=True)
        if r.returncode:
            print("account security test: %s unavailable: skipped" % DRIVER)
            sys.exit(r.returncode)
        DBCFG = json.loads(r.stdout)
        dbenv = {"KN_DATABASE_" + k.upper(): str(v) for k, v in DBCFG.items()}
        drop = scratch + ["drop", DRIVER, SCHEMA]
    work = tempfile.mkdtemp(prefix="kn-ksec.", dir=os.path.join(os.path.expanduser("~"), ".cache"))
    cert, key = make_cert(work)
    stamp = os.getpid() % 100000
    smtp = FakeSMTP("starttls", cert, key, users={SMTP_USER: SMTP_PASS}, sender_domains=["caan.edu"],
                    reject_rcpt=["bounce%d@example.org" % stamp]).start()
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=SECRET, KN_SMTP_PASSWORD=SMTP_PASS, **dbenv)
    log_path = os.path.join(work, "brain.log")
    proc = None
    try:
        subprocess.run([BRAIN, "--migrate-only"], cwd=ROOT, env=env, check=True, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)
        extra = ["--smtp.host=127.0.0.1", "--smtp.port=%d" % smtp.port, "--smtp.tls=starttls", "--smtp.user=" + SMTP_USER,
                 "--smtp.ca_file=" + cert, "--smtp.retries=-1", "--accounts.email.resend_cooldown_s=1",
                 "--accounts.email.max_per_account_day=3", "--accounts.email.link_base=http://127.0.0.1:1",
                 "--accounts.email.require_verified=chat,gift,clan"]
        with open(log_path, "a") as log:
            log.write("== start\n")
            proc = subprocess.Popen([BRAIN, "--brain.host=127.0.0.1", "--brain.port=0", "--brain.id=ksec-%d" % os.getpid()] + extra,
                                    cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT)
        port = bound_port(log_path, proc)
        asyncio.run(scenario(port, smtp))
        asyncio.run(reset_scenario(port, smtp))
    except Exception as e:  # noqa: BLE001
        check(False, "exception: %r" % e)
    finally:
        if proc:
            proc.terminate()
            proc.wait(5)
        smtp.stop()
        subprocess.run(drop)
    with open(log_path) as f:
        log = f.read()
    bad = [l.strip() for l in log.splitlines() if re.search(r"ERROR|FATAL|statement", l)]
    check(not bad, "no errors in the brain log %s" % bad[:3])
    check(SMTP_PASS not in log, "the SMTP password never appears in the brain log")
    print("account security test: " + ("OK" if not FAILS else "%d failure(s)" % len(FAILS)))
    if FAILS:
        print("log kept in " + work)
    else:
        shutil.rmtree(work, ignore_errors=True)
    sys.exit(1 if FAILS else 0)


main()
