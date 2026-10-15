#!/usr/bin/env python3
"""Profile pictures on kn_brain (services/brain/avatar.c, avatar_image.c, avatar_verdict.c, db_avatar.c,
migration 0123_account_avatars) end to end: upload, the AI review through a STUBBED gateway (never the
live /cpa provider), approval / rejection / failure / malformed answers, "not shown before review",
the upload quota, delete, and the rule that the upstream key never appears in a response or the log.
Then the moderation provider profile (ai_provider.h): ai.moderation.provider routes the review to its
own stubbed upstream and key (the default one sees no review), the profile's model wins, its rpm
refuses a review beyond the limit (fail closed) and /ai/status counts it; a moderation profile that
does not exist, or ai.enabled=0, turns uploads off (503 ModerationUnavailable).

  avatar_test.py <kn_brain> <repo root> [database]
Database: argument, else $KN_MM_TEST_DATABASE, else config/config.json's database.name (PostgreSQL,
its own schema). 77 = skip (no database or python websockets).
"""
import asyncio
import base64
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request
import zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

try:
    import websockets
except ImportError:  # pragma: no cover
    print("python websockets missing: skip")
    sys.exit(77)

BRAIN, ROOT = sys.argv[1], sys.argv[2]
DB = sys.argv[3] if len(sys.argv) > 3 else os.environ.get("KN_MM_TEST_DATABASE", "")
CONFIG_DB = {}
try:
    with open(os.path.join(ROOT, "config", "config.json")) as fp:
        CONFIG_DB = json.load(fp).get("database", {})
except (OSError, ValueError):
    pass
if not DB:
    DB = CONFIG_DB.get("name", "")
if not DB:
    print("avatar test: no database configured; skipped")
    sys.exit(77)

try:
    from PIL import Image  # optional: JPEG / WebP inputs and the output's size
except ImportError:  # pragma: no cover
    Image = None

SCHEMA = "kn_test_avatar_%d" % os.getpid()
KEY = "sk-avatar-test-key-%d" % os.getpid()  # the upstream key: must never show up anywhere the brain writes
MOD_KEY = "sk-avatar-mod-key-%d" % os.getpid()  # the moderation profile's own key (phase 2)
MOD_MODEL = "gemini-test-vision-low"  # ai.providers.mod.model: wins over avatar.moderation_model
MOD_RPM = 2
SECRET = "kn-avatar-test-%d" % os.getpid()
PER_HOUR = 14
FAILS = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what, flush=True)
    if not cond:
        FAILS.append(what)


# ---- fixtures: PNG by hand (zlib), JPEG / WebP by Pillow when it is there

def png_bytes(w, h):
    """A w x h PNG, left half red, right half blue (the square crop keeps both)."""
    rows = []
    for _y in range(h):
        row = bytearray(b"\x00")
        for x in range(w):
            row += bytes((255, 0, 0, 255)) if x < w // 2 else bytes((0, 0, 255, 255))
        rows.append(bytes(row))
    raw = b"".join(rows)

    def chunk(kind, data):
        return (len(data).to_bytes(4, "big") + kind + data +
                (zlib.crc32(kind + data) & 0xFFFFFFFF).to_bytes(4, "big"))

    ihdr = w.to_bytes(4, "big") + h.to_bytes(4, "big") + bytes((8, 6, 0, 0, 0))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(raw)) +
            chunk(b"IEND", b""))


def pil_bytes(fmt, w=64, h=48):
    buf = io.BytesIO()
    img = Image.new("RGB", (w, h), (30, 200, 90))
    img.save(buf, format=fmt, quality=90) if fmt == "JPEG" else img.save(buf, format=fmt)
    return buf.getvalue()


def b64(data):
    return base64.b64encode(data).decode()


# ---- the stub gateway: OpenAI-compatible /cpa/v1/chat/completions, scripted per test

class Gateway:
    def __init__(self, key=KEY):
        self.key = key
        self.mode = "approve"
        self.calls = []        # moderation requests: {auth, body}
        self.probes = 0
        gw = self

        class H(BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def _send(self, code, obj):
                data = json.dumps(obj).encode()
                self.send_response(code)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            def do_POST(self):
                length = int(self.headers.get("Content-Length") or 0)
                body = json.loads(self.rfile.read(length) or b"{}")
                if self.path != "/cpa/v1/chat/completions":
                    return self._send(404, {"error": {"message": "no route"}})
                if self.headers.get("Authorization") != "Bearer " + gw.key:
                    return self._send(401, {"error": {"message": "bad key"}})
                msgs = body.get("messages", [])
                parts = msgs[-1].get("content") if msgs else None
                if isinstance(parts, list) and any(p.get("type") == "image_url" for p in parts):
                    return gw.moderate(self, body, msgs[0].get("content", ""))
                gw.probes += 1  # the brain's health probe: echo its token
                text = msgs[-1].get("content", "") if msgs else ""
                token = re.search(r"KN-PROBE-\w+", text if isinstance(text, str) else "")
                self._answer(token.group(0) if token else "Hello")

            def _answer(self, content):
                self._send(200, {"choices": [{"index": 0, "finish_reason": "stop",
                                              "message": {"role": "assistant", "content": content}}],
                                 "usage": {"prompt_tokens": 7, "completion_tokens": 2, "total_tokens": 9}})

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), H)
        self.server.daemon_threads = True
        self.url = "http://127.0.0.1:%d/cpa/v1" % self.server.server_address[1]
        self._handler = H

        # the moderation answers are written by the handler through these helpers
        def moderate(handler, body, system):
            self.calls.append({"auth": handler.headers.get("Authorization"), "body": body})
            nonce = re.search(r'"nonce":"([^"]+)"', system)
            nonce = nonce.group(1) if nonce else ""
            mode = self.mode
            if mode == "fail":
                return handler._send(500, {"error": {"message": "stub failure"}})
            if mode == "slow":
                time.sleep(3)
                mode_out = "approve"
            else:
                mode_out = mode
            if mode_out == "prose":
                return handler._answer("Sure, this picture looks fine to me.")
            if mode_out == "reject":
                return handler._answer(json.dumps({"nonce": nonce, "allowed": False, "categories": ["gore", "bogus"],
                                                   "reason": "Shows blood."}))
            if mode_out == "wrong":
                return handler._answer(json.dumps({"nonce": "not-the-nonce", "allowed": True, "categories": [],
                                                   "reason": "Fine."}))
            return handler._answer(json.dumps({"nonce": nonce, "allowed": True, "categories": [],
                                               "reason": "Looks fine."}))

        self.moderate = moderate

    def start(self):
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        return self

    def stop(self):
        self.server.shutdown()
        self.server.server_close()


# ---- HTTP to the brain's public listener

def http(port, method, path, body=None, token=None):
    headers = {"Content-Type": "application/json"}
    if token:
        headers["Authorization"] = "Bearer " + token
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request("http://127.0.0.1:%d%s" % (port, path), data=data, method=method, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            return r.status, dict(r.headers), r.read()
    except urllib.error.HTTPError as e:
        return e.code, dict(e.headers), e.read()


def jbody(raw):
    try:
        return json.loads(raw)
    except ValueError:
        return {}


def status(port, token):
    code, _, raw = http(port, "GET", "/api/account/avatar", token=token)
    return code, jbody(raw).get("data", {})


# uploaded_ms of the row before each upload: a wait only counts the row written by THAT upload
# (the 202 is answered before the pending row is stored, so the old row can still be read)
PREV_UPLOADED = {}


def upload(port, token, data):
    _, before = status(port, token)
    PREV_UPLOADED[token] = before.get("uploaded_ms")
    return http(port, "POST", "/api/account/avatar", {"image": b64(data), "token": token})


def wait_uploaded(port, token, timeout=20.0):
    """The status of the picture uploaded last, once its row is written (pending or reviewed)."""
    deadline = time.time() + timeout
    last = {}
    while time.time() < deadline:
        _, last = status(port, token)
        if last.get("uploaded_ms") not in (None, PREV_UPLOADED.get(token)):
            return last
        time.sleep(0.1)
    return last


def wait_status(port, token, wanted, timeout=20.0):
    deadline = time.time() + timeout
    last = {}
    while time.time() < deadline:
        last = wait_uploaded(port, token, timeout=max(0.1, deadline - time.time()))
        if last.get("status") in wanted:
            return last
        time.sleep(0.2)
    return last


def public(port, account_id):
    return http(port, "GET", "/api/avatars/%d" % account_id)


# ---- the brain

def pg(*args):
    cmd = [sys.executable, "-I", os.path.join(ROOT, "services", "brain", "tests", "pg_schema.py"), ROOT, DB] + list(args)
    return subprocess.run(cmd).returncode


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


def register(port_ws, name):
    """Registers an account over the social socket (as social_ws_test does): the greeting, then "po",
    then the register frame. Returns (account id, session token); (None, None) with the reason printed."""
    async def go():
        async with websockets.connect("ws://127.0.0.1:%d/social/ws?enc=json" % port_ws, max_size=4 << 20) as ws:
            seen = []
            while True:  # the greeting: pi, io-init
                frame = json.loads(await asyncio.wait_for(ws.recv(), 15))
                seen.append(frame[0])
                if frame[0] == "io-init":
                    break
            await ws.send(json.dumps(["po"]))
            await ws.send(json.dumps(["a", 0, [name, "pass_word1", 0], "captcha"]))  # social_ws_test: call("a", "a", 0, ...)
            while True:
                frame = json.loads(await asyncio.wait_for(ws.recv(), 15))
                if frame[0] == "a":
                    if frame[1] is not None or frame[2] is None:
                        print("  register %s refused: %r" % (name, frame[1]), flush=True)
                        return None, None
                    return frame[2], frame[5]
    return asyncio.run(go())


def scenario(port, gw):
    stamp = os.getpid() % 100000
    alice_id, alice = register(port, "avtr%d" % stamp)
    bob_id, bob = register(port, "avtb%d" % stamp)
    check(bool(alice) and bool(bob), "two accounts registered")

    # ---- intake: the refusals come before any review, and never store a picture
    code, _, raw = http(port, "POST", "/api/account/avatar", {"image": b64(png_bytes(40, 40))})
    check(code == 401 and jbody(raw).get("error") == "LoginRequired", "no token -> 401 LoginRequired")
    code, _, raw = upload(port, alice, b"GIF89a\x01\x00\x01\x00\x80\x00\x00\x00\x00\xff\xff\xff!\xf9\x04\x01\x00\x00\x00\x00,\x00\x00\x00\x00\x01\x00\x01\x00\x00\x02\x02D\x01\x00;")
    check(code == 415 and jbody(raw).get("error") == "UnsupportedType", "a GIF -> 415 UnsupportedType: %s" % code)
    code, _, raw = upload(port, alice, b'<svg xmlns="http://www.w3.org/2000/svg"><script>alert(1)</script></svg>')
    check(code == 415 and jbody(raw).get("error") == "UnsupportedType", "an SVG (named .png in the client, by bytes) -> 415")
    code, _, raw = http(port, "POST", "/api/account/avatar", {"image": "!!!not base64!!!", "token": alice})
    check(code == 422 and jbody(raw).get("error") == "BadImage", "undecodable base64 -> 422 BadImage: %s" % code)
    code, _, raw = upload(port, alice, png_bytes(20, 20))
    check(code == 422 and jbody(raw).get("error") == "ImageTooSmall", "a 20 px picture -> 422 ImageTooSmall")
    code, _, raw = upload(port, alice, png_bytes(2100, 40))
    check(code == 413 and jbody(raw).get("error") == "ImageTooLarge", "a 2100 px wide picture -> 413 ImageTooLarge: %s" % code)
    code, _, raw = http(port, "POST", "/api/account/avatar", {"image": b64(b"\x89PNG\r\n\x1a\n" + b"\x00" * (600 * 1024)), "token": alice})
    check(code == 413 and jbody(raw).get("error") == "FileTooLarge", "a 600 KB upload -> 413 FileTooLarge")
    code, st = status(port, alice)
    check(code == 200 and st.get("status") == "none" and st.get("shown") is False, "nothing stored after refusals: %r" % st)

    # ---- approval: 202 pending, the review approves, the picture is served as WebP
    gw.mode = "approve"
    code, _, raw = upload(port, alice, png_bytes(300, 200))
    data = jbody(raw).get("data", {})
    check(code == 202 and data.get("status") == "pending" and data.get("width") == 256, "a 300x200 PNG -> 202 pending (256 px): %s" % code)
    check(data.get("source_width") == 300 and data.get("source_height") == 200, "the answer reports the upload's own size")
    st = wait_status(port, alice, ("approved", "rejected", "error"))
    check(st.get("status") == "approved" and st.get("shown") is True, "the review approves: %r" % st)
    check(st.get("model") == "gemini-3.8-flash-high", "the review model is recorded")
    check(st.get("categories") == [] and st.get("reason") == "Looks fine.", "the approval's reason is shown")
    code, headers, img = public(port, alice_id)
    check(code == 200 and headers.get("Content-Type") == "image/webp", "an approved picture is served as image/webp: %s" % code)
    check(img[:4] == b"RIFF" and img[8:12] == b"WEBP", "the served bytes are a WebP file")
    check(headers.get("ETag") and "max-age=300" in (headers.get("Cache-Control") or ""), "ETag and Cache-Control on the picture")
    code, _, _ = http(port, "GET", "/api/avatars/%d" % alice_id, None)
    check(code == 200, "the picture is public (no token)")
    if Image is not None:
        size = Image.open(io.BytesIO(img)).size
        check(size == (256, 256), "stored picture is 256 x 256: %r" % (size,))

    # the review request: the vision model, the gateway's key, an image part, the reasoning effort of "-high"
    call = gw.calls[-1]
    body = call["body"]
    check(call["auth"] == "Bearer " + KEY, "the review carries the gateway key (server side only)")
    check(body.get("model") == "gemini-3.8-flash-high" and body.get("reasoning_effort") == "high", "model gemini-3.8-flash-high, reasoning_effort high")
    check(body.get("stream") is False, "the review is one non-streamed answer")
    parts = body["messages"][1]["content"]
    check(any(p.get("type") == "image_url" and p["image_url"]["url"].startswith("data:image/webp;base64,") for p in parts),
          "the image goes to the model as a WebP data URL (never the upload's bytes)")

    # a JPEG and a WebP are accepted as inputs when Pillow can make them
    if Image is not None:
        code, _, raw = upload(port, alice, pil_bytes("JPEG"))
        check(code == 202, "a JPEG upload -> 202")
        st = wait_status(port, alice, ("approved", "rejected", "error"))
        check(st.get("status") == "approved", "the JPEG is approved: %r" % st.get("status"))
        code, _, raw = upload(port, alice, pil_bytes("WEBP"))
        check(code == 202, "a WebP upload -> 202")
        st = wait_status(port, alice, ("approved", "rejected", "error"))
        check(st.get("status") == "approved", "the WebP is approved")

    # ---- rejection: reason and categories shown, the picture stays hidden
    gw.mode = "reject"
    code, _, raw = upload(port, alice, png_bytes(128, 128))
    check(code == 202, "upload for the rejection case -> 202")
    st = wait_status(port, alice, ("approved", "rejected", "error"))
    check(st.get("status") == "rejected" and st.get("shown") is False, "a rejected picture is not shown: %r" % st.get("status"))
    check(st.get("categories") == ["gore"], "rejection lists the known categories only: %r" % st.get("categories"))
    check(st.get("reason") == "Shows blood.", "rejection reason shown to the owner")
    code, _, _ = public(port, alice_id)
    check(code == 404, "a rejected picture is not served (404)")

    # ---- failures: each hides the picture and says why
    gw.mode = "fail"
    upload(port, alice, png_bytes(128, 128))
    st = wait_status(port, alice, ("approved", "rejected", "error"))
    check(st.get("status") == "error" and st.get("shown") is False, "an upstream HTTP 500 -> error, hidden: %r" % st.get("status"))
    check("Automatic review failed" in (st.get("reason") or ""), "the owner is told the review failed")
    code, _, _ = public(port, alice_id)
    check(code == 404, "an errored review is not served")

    gw.mode = "prose"
    upload(port, alice, png_bytes(128, 128))
    st = wait_status(port, alice, ("approved", "rejected", "error"))
    check(st.get("status") == "error" and st.get("shown") is False, "a prose answer (no JSON) -> error, hidden")

    gw.mode = "wrong"
    upload(port, alice, png_bytes(128, 128))
    st = wait_status(port, alice, ("approved", "rejected", "error"))
    check(st.get("status") == "error" and st.get("shown") is False, "an approval with the wrong nonce -> error, hidden")

    # ---- not shown before the review: the review is slow, the picture is pending and unreachable
    gw.mode = "slow"
    code, _, _ = upload(port, alice, png_bytes(128, 128))
    check(code == 202, "upload for the slow review -> 202")
    st_now = wait_uploaded(port, alice)
    code_pub, _, _ = public(port, alice_id)
    check(st_now.get("status") == "pending" and st_now.get("shown") is False, "while under review the status is pending, not shown")
    check(code_pub == 404, "while under review the picture is NOT served (404)")
    st = wait_status(port, alice, ("approved", "rejected", "error"), timeout=30)
    check(st.get("status") == "approved", "after the slow review it is approved")
    check(public(port, alice_id)[0] == 200, "and then served")

    # ---- delete
    code, _, raw = http(port, "DELETE", "/api/account/avatar", None, token=alice)
    check(code == 200 and jbody(raw).get("data", {}).get("deleted") == 1, "DELETE removes the picture")
    code, st = status(port, alice)
    check(st.get("status") == "none" and st.get("shown") is False, "after delete the status is none")
    check(public(port, alice_id)[0] == 404, "after delete the picture is gone (404)")

    # ---- quota: uploads that reach the account count, a rolling hour per account
    last = None
    for _ in range(PER_HOUR):
        last, _, raw = upload(port, bob, b"GIF89a\x01\x00\x01\x00")
    check(last == 415, "bob's first %d uploads are counted (refused by type)" % PER_HOUR)
    code, _, raw = upload(port, bob, b"GIF89a\x01\x00\x01\x00")
    check(code == 429 and jbody(raw).get("error") == "TooManyRequests", "upload %d -> 429 TooManyRequests" % (PER_HOUR + 1))

    return alice_id


def scenario_profiles(port, gw, gw_mod):
    """ai.moderation.provider=mod: the review goes to the mod upstream with its key and model, within its rpm."""
    stamp = os.getpid() % 100000
    carol_id, carol = register(port, "avtc%d" % stamp)
    check(bool(carol), "profiles: an account registered")
    before = len(gw.calls)
    gw_mod.mode = "approve"
    code, _, _ = upload(port, carol, png_bytes(128, 128))
    check(code == 202, "profiles: upload -> 202")
    st = wait_status(port, carol, ("approved", "rejected", "error"))
    check(st.get("status") == "approved" and st.get("shown") is True, "profiles: approved through the moderation profile: %r" % st.get("status"))
    check(len(gw_mod.calls) == 1 and gw_mod.calls[-1]["auth"] == "Bearer " + MOD_KEY,
          "profiles: the review went to the moderation upstream with the moderation key")
    check(len(gw.calls) == before, "profiles: the default (editor) upstream saw no review")
    body = gw_mod.calls[-1]["body"] if gw_mod.calls else {}
    check(body.get("model") == MOD_MODEL and body.get("reasoning_effort") == "low",
          "profiles: the profile's model (%s) and its -low effort are sent: %r" % (MOD_MODEL, body.get("model")))
    check(st.get("model") == MOD_MODEL, "profiles: the verdict records the profile's model: %r" % st.get("model"))
    code, _, _ = upload(port, carol, png_bytes(96, 96))
    st = wait_status(port, carol, ("approved", "rejected", "error"))
    check(code == 202 and st.get("status") == "approved", "profiles: a second review within the rpm is approved")
    # beyond ai.providers.mod.rpm: refused before any request, the picture fails closed (hidden)
    code, _, _ = upload(port, carol, png_bytes(80, 80))
    st = wait_status(port, carol, ("approved", "rejected", "error"))
    check(code == 202 and st.get("status") == "error" and st.get("shown") is False,
          "profiles: review %d in a minute (rpm %d) -> error, hidden: %r" % (MOD_RPM + 1, MOD_RPM, st.get("status")))
    check(len(gw_mod.calls) == MOD_RPM, "profiles: the refused review never reached the upstream (%d calls)" % len(gw_mod.calls))
    check(public(port, carol_id)[0] == 404, "profiles: the refused picture is not served")
    code, _, raw = http(port, "GET", "/ai/status")
    text = raw.decode(errors="replace")
    s = jbody(raw)
    c = s.get("consumers", {}).get("moderation", {})
    pm = s.get("providers", {}).get("mod", {})
    check(code == 200 and c.get("provider") == "mod" and c.get("configured") is True, "profiles: /ai/status routes moderation to mod: %r" % c)
    check(c.get("requests_today") == MOD_RPM and c.get("tokens_today") == 9 * MOD_RPM,
          "profiles: /ai/status counts the moderation requests and tokens: %r" % c)
    check(pm.get("rpm") == MOD_RPM and pm.get("requests_today") == MOD_RPM, "profiles: /ai/status mod profile: %r" % pm)
    check(MOD_KEY not in text and KEY not in text and gw_mod.url not in text, "profiles: no key or upstream URL in /ai/status")


def start_brain(log_path, env, extra, tag):
    with open(log_path, "a") as log:
        log.write("== start %s\n" % tag)
        proc = subprocess.Popen([BRAIN, "--brain.host=127.0.0.1", "--brain.port=0", "--brain.id=avatar-%s-%d" % (tag, os.getpid())] + extra,
                                cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT)
    port = bound_port(log_path, proc)
    time.sleep(0.2)
    return proc, port


def stop_brain(proc):
    if proc and proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()


def main():
    if shutil.which("psql") is None:
        print("psql unavailable: skip")
        return 77
    work = tempfile.mkdtemp(prefix="kn-avatar.", dir=os.path.join(os.path.expanduser("~"), ".cache"))
    log_path = os.path.join(work, "brain.log")
    gw = Gateway().start()
    gw_mod = Gateway(MOD_KEY).start()
    proc = None
    if pg("create", SCHEMA):
        print("PostgreSQL unavailable: skip")
        gw.stop()
        gw_mod.stop()
        return 77
    # no KN_AI_* from the caller's environment: the profiles are exactly the stubs below
    base = {k: v for k, v in os.environ.items() if not k.startswith("KN_AI_")}
    env = dict(base, KN_BRAIN_INTERNAL_SECRET=SECRET, KN_DATABASE_NAME=DB, KN_DATABASE_SEARCH_PATH=SCHEMA,
               KN_AI_UPSTREAM_URL=gw.url, KN_AI_UPSTREAM_KEY=KEY)
    extra = ["--avatar.uploads_per_hour=%d" % PER_HOUR, "--avatar.moderation_timeout_ms=20000",
             "--ai.health_interval_s=0"]
    try:
        subprocess.run([BRAIN, "--migrate-only"], cwd=ROOT, env=env, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        # phase 1: the legacy single upstream (the "default" profile) serves the review
        proc, port = start_brain(log_path, env, extra, "default")
        scenario(port, gw)
        stop_brain(proc)
        proc = None
        # phase 2: moderation on its own profile, with its own key, model and rpm
        env_mod = dict(env, KN_AI_MOD_URL=gw_mod.url, KN_AI_MOD_KEY=MOD_KEY)
        proc, port = start_brain(log_path, env_mod, extra + ["--ai.moderation.provider=mod", "--ai.providers.mod.model=" + MOD_MODEL,
                                                             "--ai.providers.mod.rpm=%d" % MOD_RPM], "profiles")
        scenario_profiles(port, gw, gw_mod)
        stop_brain(proc)
        proc = None
        # phase 3: a moderation profile that does not exist: uploads are off, nothing is reviewed
        proc, port = start_brain(log_path, env, extra + ["--ai.moderation.provider=nosuch"], "missing")
        calls = len(gw.calls) + len(gw_mod.calls)
        code, _, raw = http(port, "POST", "/api/account/avatar", {"image": b64(png_bytes(64, 64)), "token": "x"})
        check(code == 503 and jbody(raw).get("error") == "ModerationUnavailable",
              "no moderation profile -> 503 ModerationUnavailable: %s" % code)
        check(len(gw.calls) + len(gw_mod.calls) == calls, "no moderation profile: no review request")
        stop_brain(proc)
        proc = None
        # phase 4: the AI proxy switched off (ai.enabled=0): uploads are off as well
        proc, port = start_brain(log_path, env, extra + ["--ai.enabled=0"], "disabled")
        code, _, raw = http(port, "POST", "/api/account/avatar", {"image": b64(png_bytes(64, 64)), "token": "x"})
        check(code == 503 and jbody(raw).get("error") == "ModerationUnavailable",
              "ai.enabled=0 -> 503 ModerationUnavailable: %s" % code)
        check(len(gw.calls) + len(gw_mod.calls) == calls, "ai.enabled=0: no review request")
    except Exception as e:  # noqa: BLE001
        check(False, "exception: %r" % e)
    finally:
        stop_brain(proc)
        gw.stop()
        gw_mod.stop()
        pg("drop", SCHEMA)
    with open(log_path) as f:
        log = f.read()
    check(KEY not in log and MOD_KEY not in log, "the gateway keys never appear in the brain log")
    check(SECRET not in log, "the internal secret never appears in the brain log")
    check(not [l for l in log.splitlines() if re.search(r"\bFATAL\b|statement", l)], "no fatal or SQL errors in the brain log")
    print("avatar test: " + ("OK" if not FAILS else "%d failure(s)" % len(FAILS)))
    if FAILS:
        print("log kept in " + work)
    else:
        shutil.rmtree(work, ignore_errors=True)
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
