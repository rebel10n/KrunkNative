#!/usr/bin/env python3
"""Email verification and v7.1.4 2FA through the real web client (doc/backend.md 7.9):
headless Chromium on the Emscripten build (dist/web) against kn_brain (sqlite) and
services/common/tests/fake_smtp.py.

  register in the menu's account window with an email -> the verification mail arrives at the
  fake SMTP server -> its link (fetched like a mail client would) verifies the address -> the open
  menu's Account Security block turns "Verified" (the "emailS" push) -> Enable 2FA shows v7's popup
  with the QR code -> the six code inputs (typed into, as a player would) enable it -> "Disable".

  email_verify_web_e2e.py <native build dir> <repo root> [<web dist dir>]
Web dist: argument, else $KN_WEB_DIST. 77 = skip (no web build, Chromium, websockets or openssl).
Ports from kn_proto.free_ports(); the profile, database and logs live in ~/.cache.
"""
import asyncio
import base64
import email
import email.policy
import functools
import hashlib
import hmac
import http.server
import json
import os
import re
import shutil
import sqlite3
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request

try:
    import websockets
except ImportError:  # pragma: no cover
    print("python websockets missing: skip")
    sys.exit(77)

BUILD, ROOT = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
DIST = os.path.abspath(sys.argv[3] if len(sys.argv) > 3 else os.environ.get("KN_WEB_DIST", ""))
BRAIN = os.path.join(BUILD, "services", "brain", "kn_brain")
CHROME = os.environ.get("KN_CHROME") or shutil.which("chromium-browser") or shutil.which("chromium") or shutil.which("google-chrome")
sys.path.insert(0, os.path.join(ROOT, "services", "common", "tests"))
sys.path.insert(0, os.path.join(ROOT, "server", "tests"))
if not (DIST and os.path.exists(os.path.join(DIST, "client.wasm"))) or not CHROME or not shutil.which("openssl") or not os.path.exists(BRAIN):
    print("needs a web build (KN_WEB_DIST), Chromium, openssl and kn_brain: skip")
    sys.exit(77)
from fake_smtp import FakeSMTP, make_cert  # noqa: E402
from kn_proto import free_ports  # noqa: E402

FAILS = []
SMTP_USER, SMTP_PASS = "mailer@caan.test", "fake-web-pass-%d" % os.getpid()


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what, flush=True)
    if not cond:
        FAILS.append(what)


def totp(key_b32, offset=0):
    key = base64.b32decode(key_b32 + "=" * (-len(key_b32) % 8))
    mac = hmac.new(key, struct.pack(">Q", int(time.time() // 30) + offset), hashlib.sha1).digest()
    o = mac[-1] & 15
    return "%06d" % ((struct.unpack(">I", mac[o:o + 4])[0] & 0x7FFFFFFF) % 1000000)


class Cdp:
    def __init__(self, ws):
        self.ws, self.id, self.pending, self.logs = ws, 0, {}, []

    @classmethod
    async def connect(cls, port):
        for _ in range(120):
            try:
                with urllib.request.urlopen("http://127.0.0.1:%d/json/list" % port, timeout=2) as r:
                    pages = [t for t in json.load(r) if t.get("type") == "page"]
                if pages:
                    break
            except OSError:
                pass
            await asyncio.sleep(0.5)
        c = cls(await websockets.connect(pages[0]["webSocketDebuggerUrl"], max_size=256 << 20))
        asyncio.get_event_loop().create_task(c.reader())
        return c

    async def reader(self):
        async for m in self.ws:
            msg = json.loads(m)
            if "id" in msg and msg["id"] in self.pending:
                self.pending.pop(msg["id"]).set_result(msg)
            elif msg.get("method") == "Runtime.consoleAPICalled":
                self.logs.append(" ".join(str(a.get("value", "")) for a in msg["params"]["args"]))

    async def send(self, method, **params):
        self.id += 1
        fut = asyncio.get_event_loop().create_future()
        self.pending[self.id] = fut
        await self.ws.send(json.dumps({"id": self.id, "method": method, "params": params}))
        msg = await asyncio.wait_for(fut, 60)
        if "error" in msg:
            raise RuntimeError("%s: %s" % (method, msg["error"]))
        return msg["result"]

    async def eval(self, expr):
        r = await self.send("Runtime.evaluate", expression=expr, returnByValue=True, awaitPromise=True, userGesture=True)
        if "exceptionDetails" in r:
            raise RuntimeError("eval %s: %s" % (expr[:60], r["exceptionDetails"]))
        return r["result"].get("value")

    async def wait(self, expr, timeout=60.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                v = await self.eval(expr)
                if v:
                    return v
            except RuntimeError:
                pass
            await asyncio.sleep(0.25)
        return None

    async def shot(self, path):
        r = await self.send("Page.captureScreenshot", format="png")
        with open(path, "wb") as f:
            f.write(base64.b64decode(r["data"]))


def mail_token(msg):
    m = email.message_from_bytes(msg["data"], policy=email.policy.default)
    text = m.get_body(("plain",)).get_content()
    link = re.search(r"(https?://\S+/auth/verify-email\?token=[A-Za-z0-9_-]+)", text)
    return link.group(1) if link else None


async def scenario(http_port, cdp_port, brain_port, smtp, db_path, work):
    c = await Cdp.connect(cdp_port)
    await c.send("Runtime.enable")
    await c.send("Page.enable")
    await c.send("Emulation.setDeviceMetricsOverride", width=1280, height=720, deviceScaleFactor=1, mobile=False)
    name = "web%d" % (os.getpid() % 100000)
    addr = "%s@example.org" % name
    await c.send("Page.navigate", url="http://127.0.0.1:%d/?mode=online&brain=http://127.0.0.1:%d&finder=off" % (http_port, brain_port))
    ok = await c.wait("!!document.querySelector('[onclick*=\"showWindow(5)\"]')", 180)
    check(ok, "menu up")
    await asyncio.sleep(2)
    await c.eval("document.querySelector('[onclick*=\"showWindow(5)\"]').click()")
    ok = await c.wait("!!document.getElementById('accEmail')", 30)
    check(ok, "the account window has the email field")
    await c.eval("""(() => { const set = (id, v) => { const e = document.getElementById(id); e.value = v;
        e.dispatchEvent(new Event('input', {bubbles: true})); };
        set('accName', %s); set('accPass', 'web_pass_1'); set('accEmail', %s);
        document.querySelector('.accBtn.buttonP').click(); })()""" % (json.dumps(name), json.dumps(addr)))
    ok = await c.wait("!!document.querySelector('[onclick*=\"logoutAcc\"]')", 30)
    check(ok, "registered and logged in through the menu")
    mails = smtp.wait_messages(1, 30)
    check(len(mails) == 1 and mails[0]["rcpt"] == [addr], "the verification mail reached the SMTP server")
    link = mail_token(mails[0]) if mails else None
    check(link and link.startswith("http://127.0.0.1:%d/auth/verify-email?token=" % brain_port), "the mail links to this brain")

    # Account Security (windows[4] tab 3): unverified, then the link -> "Verified" without a reload
    await c.eval("document.querySelector('[onclick*=\"switchTab(3)\"]').click()")
    ok = await c.wait("document.getElementById('emailRes') && document.getElementById('menuWindow').innerText.includes('Not verified')", 20)
    check(ok, "Account Security shows the address as not verified")
    await c.shot(os.path.join(work, "email-unverified.png"))
    with urllib.request.urlopen(link, timeout=10) as r:
        body = r.read().decode()
    check(r.status == 200 and "Email verified" in body, "the link verifies the address")
    ok = await c.wait("document.getElementById('menuWindow').innerText.includes('Verified') && "
                      "!document.getElementById('menuWindow').innerText.includes('Not verified')", 20)
    check(ok, "the open menu turns Verified (emailS push)")
    await c.shot(os.path.join(work, "email-verified.png"))

    # v7.1.4 2FA: Enable -> popup with the QR code -> six digits -> enabled
    await c.eval("document.querySelector('[onclick*=\"act2FA()\"]').click()")
    ok = await c.wait("getComputedStyle(document.getElementById('twoFAPop')).display !== 'none' && "
                      "(document.querySelector('#twoFAqr img').getAttribute('src') || '').startsWith('data:image/svg+xml')", 20)
    check(ok, "Enable 2FA opens v7's popup with the QR code")
    n = await c.eval("document.querySelectorAll('.twoFAIn').length")
    check(n == 6, "six code inputs (config.twoFAKeyLen): %r" % n)
    await c.shot(os.path.join(work, "twofa-popup.png"))
    secret = sqlite3.connect(db_path).execute("SELECT twofa_pending FROM accounts WHERE username = ?", (name,)).fetchone()
    code = totp(secret[0] if secret else "AAAA", 0)
    await c.eval("document.querySelector('.twoFAIn').focus()")
    for d in code:
        await c.send("Input.insertText", text=d)
        await asyncio.sleep(0.15)
    ok = await c.wait("getComputedStyle(document.getElementById('twoFAPop')).display === 'none' && "
                      "!!document.querySelector('[onclick*=\"dact2FA()\"]')", 20)
    check(ok, "typing the code enables 2FA (the button turns Disable)")
    on = sqlite3.connect(db_path).execute("SELECT twofa_secret IS NOT NULL FROM accounts WHERE username = ?", (name,)).fetchone()
    check(on and on[0] == 1, "2FA stored on the account")
    await c.ws.close()


def main():
    work = tempfile.mkdtemp(prefix="kn-email-web.", dir=os.path.expanduser("~/.cache"))
    http_port, cdp_port, brain_port = free_ports(3)
    cert, key = make_cert(work)
    smtp = FakeSMTP("starttls", cert, key, users={SMTP_USER: SMTP_PASS}).start()
    class Quiet(http.server.SimpleHTTPRequestHandler):
        def log_message(self, *a):
            pass
    handler = functools.partial(Quiet, directory=DIST)
    httpd = http.server.ThreadingHTTPServer(("127.0.0.1", http_port), handler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    db_path = os.path.join(work, "coordinator.db")
    log = open(os.path.join(work, "brain.log"), "w")
    brain = subprocess.Popen([BRAIN, "--brain.host=127.0.0.1", "--brain.port=%d" % brain_port, "--database.driver=sqlite",
                              "--database.path=" + db_path, "--migrate", "--smtp.host=127.0.0.1", "--smtp.port=%d" % smtp.port,
                              "--smtp.tls=starttls", "--smtp.user=" + SMTP_USER, "--smtp.ca_file=" + cert, "--smtp.retries=-1"],
                             cwd=ROOT, env=dict(os.environ, KN_SMTP_PASSWORD=SMTP_PASS), stdout=log, stderr=subprocess.STDOUT,
                             start_new_session=True)
    prof = os.path.join(work, "chrome")
    chrome = subprocess.Popen([CHROME, "--headless=new", "--no-sandbox", "--use-gl=angle", "--use-angle=swiftshader",
                               "--enable-unsafe-swiftshader", "--mute-audio", "--window-size=1280,720",
                               "--remote-debugging-port=%d" % cdp_port, "--user-data-dir=" + prof, "about:blank"],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
    try:
        asyncio.run(asyncio.wait_for(scenario(http_port, cdp_port, brain_port, smtp, db_path, work), 400))
    except Exception as e:  # noqa: BLE001
        check(False, "exception: %r" % e)
    finally:
        for p in (chrome, brain):
            p.terminate()
            try:
                p.wait(10)
            except subprocess.TimeoutExpired:
                p.kill()
        httpd.shutdown()
        smtp.stop()
    log.close()
    with open(log.name) as f:
        text = f.read()
    check(SMTP_PASS not in text, "no SMTP password in the brain log")
    print("email web e2e: " + ("OK" if not FAILS else "%d failure(s), files in %s" % (len(FAILS), work)))
    if not FAILS and not os.environ.get("KN_KEEP"):
        shutil.rmtree(work, ignore_errors=True)
    sys.exit(1 if FAILS else 0)


main()
