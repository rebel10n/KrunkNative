#!/usr/bin/env python3
"""KN_KRUNKER_OFFICIAL end to end against a local stub of krunker.io (client/include/krunker_net.h):
nothing leaves the machine.

  python3 -I server/tests/krunker_official_e2e.py BUILD_DIR [WEB_DIST_DIR] [--native|--web]   (repo root)

Stubs (Python, ports from kn_proto.free_ports()):
  - Krunker's matchmaker: GET /generate-token (a token) and GET /seek-game (checks the exact query
    of game.deob.js's matchmakerSeek, answers {host, port, gameId, clientId, data}). Like the live
    one it answers the first seek 400 {"error":"CaptchaRequired"}; a seek with an unused captchaToken
    (+ a validationToken from /generate-token) is let through, a used one is refused again.
  - Cloudflare Turnstile (web runs only): the page's request for challenges.cloudflare.com's api.js is
    answered over the DevTools protocol (Fetch) with a stub that calls the widget's callback with a
    fake token ("ok") or its error-callback with 110200 ("domain", what the real widget answers on a
    domain the sitekey is not for). Nothing is injected outside the test's own browser.
  - a Krunker game server: a WebSocket at /ws?gameId=&clientKey= speaking the official framing,
    msgpack([name, ...args]) + the 2 ahNum trailer bytes, with the stub's own msgpack codec (the
    @msgpack/msgpack number rules). It sends io-init, load, init (map 10, mode 0), "0" (a stub
    player), three "cc" (a script, garbage, an undefined result), a "cpt" ALTCHA challenge, "k"
    moving the stub player every 100 ms, "pi" every second; after the client's "en" its own "0",
    "l" and a "ch" line.
Lobby runs (the default, no KRUNKNATIVE_KRUNKER_JOIN): "native-lobby" clicks the stub's official lobby
(GET /game-list) in the server browser from the offline game: it is joined in-client (Cancel in the
Security Challenge modal first ends the join saying so, a second click + a pasted token joins);
"native-optout" (KRUNKNATIVE_KRUNKER_JOIN=browser) checks the click opens krunker.io in the browser instead.
Captcha runs (krunker_captcha.h): native "callback" (the one-time 127.0.0.1 callback: a wrong secret,
a foreign Origin, a body that is not a token, an expired listener, then the token, then a second
token), native "paste" (a bad paste refused in the modal, then a good one), web "widget" (the stub
widget's token) and web "domain" (110200: the modal says why and falls back to the paste box). Each
checks the live client's order: seek-game (no validationToken), CaptchaRequired, generate-token,
seek-game with validationToken + captchaToken, and that no token reaches the log.
Checks: the seek query, the WebSocket URL, every client frame's ahNum trailer ((prev + AHKEY) & 0xff,
"cptR" without the step), "load" answered, "en" (35-value enter array + 11 flags), "q" windows
(isn ascending, a movement key in the changes), "ct" with the chat text, "po" for "pi", "v" for
"l", the "ccr" (keystream, base64, the JS-double FNV) or no reply where the game sends none, the
"cptR" proof of work; and on the client side: it joined as the stub's sid, sees the stub player
move, and showed the chat line.

The native run is one headless client_bin (--krunker, KRUNKNATIVE_KRUNKER_MATCHMAKER, an input
script: play, W, `say`, `players`); the web run is the web build in headless Chromium (?krunker=1,
?krunkerMM=, clicks and keys over the DevTools protocol). The native cheat check answers only when
client_bin's QuickJS sandbox (krunker_cheat_qjs.c) answers natively, the page's eval on the web.
Exit 0 when every check passed, 77 when nothing could run; logs stay in ~/.cache/kn-krunker-official-e2e-*
on failure.
"""
import asyncio
import base64
import hashlib
import json
import os
import re
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, SimpleHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, os.path.join(os.getcwd(), "server/tests"))
import kn_proto  # noqa: E402

try:
    import websockets
    import websockets.asyncio.server as ws_server
    import websockets.sync.client as ws_client
except ImportError:  # pragma: no cover
    websockets = None

ARGS = [a for a in sys.argv[1:] if not a.startswith("--")]
BUILD = os.path.abspath(ARGS[0] if ARGS else "cmake-build-debug")
DIST = os.path.abspath(ARGS[1]) if len(ARGS) > 1 else None
ONLY = {"--native": "native", "--web": "web"}.get(next((a for a in sys.argv[1:] if a.startswith("--")), ""), "")
ROOT = os.getcwd()
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-krunker-official-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
CHROME = shutil.which("chromium-browser") or shutil.which("chromium") or shutil.which("google-chrome")
# the asset tree: this checkout's, else the main checkout's (a worktree may lack the large content)
ASSETS = os.environ.get("KRUNKNATIVE_ASSETS_DIR") or next(
    (d for d in (os.path.join(ROOT, "assets"), os.path.expanduser("~/src/KrunkNative/assets")) if os.path.isdir(os.path.join(d, "models"))),
    os.path.join(ROOT, "assets"))


def header_value(name):
    with open(os.path.join(ROOT, "shared/include/krunker_build.h")) as f:
        m = re.search(r"#define %s\s+(\S+)" % name, f.read())
    return m.group(1).strip('"').rstrip("u") if m else None


AHKEY = int(header_value("KRUNKER_AHKEY"), 0)
BUILD_VERSION = header_value("KRUNKER_BUILD_VERSION")
SALT = b"KRUNK3R_AC"
GAME_ID, CLIENT_ID, MY_SID, BOT_SID = "TEST:abc12", "cid-42", 9, 5

results = []


def check(name, ok, detail=""):
    results.append(bool(ok))
    show = detail and (not ok or len(str(detail)) <= 240)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + str(detail)) if show else ''}", flush=True)


# ---------------------------------------------------------------- msgpack (@msgpack/msgpack rules)

def mp_encode(v):
    if v is None:
        return b"\xc0"
    if v is True:
        return b"\xc3"
    if v is False:
        return b"\xc2"
    if isinstance(v, float) and v == v and v not in (float("inf"), float("-inf")) and v.is_integer() and abs(v) <= 2 ** 53 - 1:
        v = int(v)
    if isinstance(v, int):
        if v >= 0:
            if v < 0x80:
                return bytes([v])
            for tag, fmt, lim in ((0xcc, ">B", 1 << 8), (0xcd, ">H", 1 << 16), (0xce, ">I", 1 << 32), (0xcf, ">Q", 1 << 64)):
                if v < lim:
                    return bytes([tag]) + struct.pack(fmt, v)
        if v >= -0x20:
            return bytes([0xe0 | (v + 0x20)])
        for tag, fmt, lim in ((0xd0, ">b", 1 << 7), (0xd1, ">h", 1 << 15), (0xd2, ">i", 1 << 31), (0xd3, ">q", 1 << 63)):
            if v >= -lim:
                return bytes([tag]) + struct.pack(fmt, v)
    if isinstance(v, float):
        return b"\xcb" + struct.pack(">d", v)
    if isinstance(v, str):
        b = v.encode()
        n = len(b)
        head = bytes([0xa0 | n]) if n < 32 else bytes([0xd9, n]) if n < 256 else b"\xda" + struct.pack(">H", n) if n < 65536 else b"\xdb" + struct.pack(">I", n)
        return head + b
    if isinstance(v, (list, tuple)):
        n = len(v)
        head = bytes([0x90 | n]) if n < 16 else b"\xdc" + struct.pack(">H", n) if n < 65536 else b"\xdd" + struct.pack(">I", n)
        return head + b"".join(mp_encode(x) for x in v)
    if isinstance(v, dict):
        n = len(v)
        head = bytes([0x80 | n]) if n < 16 else b"\xde" + struct.pack(">H", n)
        return head + b"".join(mp_encode(str(k)) + mp_encode(x) for k, x in v.items())
    raise TypeError(type(v))


def mp_decode(b):
    pos = [0]

    def take(n):
        s = b[pos[0]:pos[0] + n]
        if len(s) != n:
            raise ValueError("truncated")
        pos[0] += n
        return s

    def val():
        t = take(1)[0]
        if t < 0x80:
            return t
        if t >= 0xe0:
            return t - 0x100
        if 0x80 <= t <= 0x8f:
            return {str(val()): val() for _ in range(t & 15)}
        if 0x90 <= t <= 0x9f:
            return [val() for _ in range(t & 15)]
        if 0xa0 <= t <= 0xbf:
            return take(t & 31).decode()
        simple = {0xc0: None, 0xc2: False, 0xc3: True}
        if t in simple:
            return simple[t]
        fmts = {0xca: ">f", 0xcb: ">d", 0xcc: ">B", 0xcd: ">H", 0xce: ">I", 0xcf: ">Q", 0xd0: ">b", 0xd1: ">h", 0xd2: ">i", 0xd3: ">q"}
        if t in fmts:
            return struct.unpack(fmts[t], take(struct.calcsize(fmts[t])))[0]
        if t in (0xd9, 0xda, 0xdb):
            n = struct.unpack({0xd9: ">B", 0xda: ">H", 0xdb: ">I"}[t], take({0xd9: 1, 0xda: 2, 0xdb: 4}[t]))[0]
            return take(n).decode()
        if t in (0xc4, 0xc5, 0xc6):
            n = struct.unpack({0xc4: ">B", 0xc5: ">H", 0xc6: ">I"}[t], take({0xc4: 1, 0xc5: 2, 0xc6: 4}[t]))[0]
            return take(n)
        if t in (0xdc, 0xdd):
            n = struct.unpack(">H" if t == 0xdc else ">I", take(2 if t == 0xdc else 4))[0]
            return [val() for _ in range(n)]
        if t in (0xde, 0xdf):
            n = struct.unpack(">H" if t == 0xde else ">I", take(2 if t == 0xde else 4))[0]
            return {str(val()): val() for _ in range(n)}
        raise ValueError("msgpack type 0x%02x" % t)

    v = val()
    if pos[0] != len(b):
        raise ValueError("extra bytes")
    return v


def frame(name, *args, ah=0):
    return mp_encode([name, *args]) + bytes([ah >> 4 & 0xf, ah & 0xf])


def unframe(data):
    v = mp_decode(data[:-2])
    return v[0], v[1:], (data[-2] << 4) + data[-1]


# ---------------------------------------------------------------- cheat check / captcha (server side)

def keystream(nonce, n):  # game.deob.js iïìiîïì: UTF-16 units of the nonce (ASCII here)
    return bytes(((ord(nonce[i % len(nonce)]) ^ SALT[i % len(SALT)]) + i) & 0xff for i in range(n))


def xor_b64(data, nonce):
    return base64.b64encode(bytes(a ^ b for a, b in zip(data, keystream(nonce, len(data))))).decode()


def fnv_js(s):  # h = 0x1000193 * (h ^= c) | 0 in doubles (the product is rounded to 53 bits)
    h = 0x811c9dc5 - (1 << 32)  # as int32
    for unit in struct.unpack("<%dH" % (len(s.encode("utf-16-le")) // 2), s.encode("utf-16-le")):
        x = (h ^ unit) & 0xffffffff
        x = x - (1 << 32) if x >= 1 << 31 else x
        p = int(float(0x1000193) * float(x))
        p &= 0xffffffff
        h = p - (1 << 32) if p >= 1 << 31 else p
    return "%08x" % (h & 0xffffffff)


assert fnv_js("") == "811c9dc5" and fnv_js("a") == "e40c292c" and fnv_js('abc12345{"r":1}') == "c3639e34"
assert fnv_js('1234567890{"r":null,"e":{"_eval":"x"}}') == "cf36480f" and fnv_js("é€😀") == "0c5e230c"

CC_CASES = [  # nonce, ciphertext, expected (payload, hash) or None = no ccr
    ("n0nceA1", xor_b64(json.dumps("1+1").encode(), "n0nceA1"), (xor_b64(b"2", "n0nceA1"), fnv_js("n0nceA1" + "2"))),
    ("n0nceB2", base64.b64encode(b"\x00garbage, not JSON").decode(), None),
    ("n0nceC3", xor_b64(json.dumps("undefined").encode(), "n0nceC3"), ("", fnv_js("n0nceC3" + "undefined"))),
]
ALTCHA_SALT, ALTCHA_NUMBER = "e2e-salt?expires=1999999999", 4321
ALTCHA = {"algorithm": "SHA-256", "challenge": hashlib.sha256((ALTCHA_SALT + str(ALTCHA_NUMBER)).encode()).hexdigest(),
          "maxnumber": 20000, "salt": ALTCHA_SALT, "signature": "e2esig"}


# ---------------------------------------------------------------- the stubs

class Stub:
    def __init__(self, mm_port, ws_port):
        self.mm_port, self.ws_port = mm_port, ws_port
        self.http_log = []      # (path, query list)
        self.ws_paths = []
        self.frames = []        # (name, args, ah) from the client
        self.errors = []
        self.got_en = threading.Event()
        self.closed = threading.Event()
        self.issued = []          # validation tokens handed out
        self.used_captchas = set()
        self.captcha_ok = threading.Event()
        self.loop = None
        self.httpd = None

    # matchmaker
    def start_http(self):
        stub = self

        class H(BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def do_GET(self):
                u = urllib.parse.urlsplit(self.path)
                q = urllib.parse.parse_qsl(u.query, keep_blank_values=True)
                stub.http_log.append((u.path, q, u.query))
                status = 200
                if u.path == "/generate-token":
                    tok = "tok_e2e_%d_0123456789abcdefghijklmnopqrstuvwxyz0123456789" % len(stub.issued)
                    stub.issued.append(tok)
                    body, ctype = tok.encode(), "text/plain"
                elif u.path == "/game-list":  # the server browser's official lobbies (krunker_lobbies.c)
                    body, ctype = json.dumps({"success": True, "totalPlayerCount": 3, "games": [
                        [GAME_ID, "de-fra", 3, 8, {"i": "Kanji", "g": 0}, 240]]}).encode(), "application/json"
                elif u.path == "/seek-game":
                    qd = dict(q)
                    cap, val = qd.get("captchaToken"), qd.get("validationToken")
                    if not cap or cap in stub.used_captchas or val not in stub.issued:
                        status, body = 400, json.dumps({"error": "CaptchaRequired"}).encode()  # the live answer
                        if cap:
                            stub.used_captchas.add(cap)
                    else:
                        stub.used_captchas.add(cap)
                        stub.captcha_ok.set()
                        body = json.dumps({"host": "127.0.0.1", "port": stub.ws_port, "gameId": GAME_ID, "clientId": CLIENT_ID,
                                           "data": {"i": "Kanji", "g": 0}}).encode()
                    ctype = "application/json"
                else:
                    self.send_response(404)
                    self.end_headers()
                    return
                self.send_response(status)
                self.send_header("Content-Type", ctype)
                self.send_header("Content-Length", str(len(body)))
                self.send_header("Access-Control-Allow-Origin", "*")
                self.end_headers()
                self.wfile.write(body)

        self.httpd = ThreadingHTTPServer(("127.0.0.1", self.mm_port), H)
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    # game server
    async def handler(self, ws):
        self.ws_paths.append(ws.request.path)
        ah = [0]

        async def send(name, *args):
            ah[0] = (ah[0] + 0x11) & 0xff  # the server's own trailer (the client never checks it)
            await ws.send(frame(name, *args, ah=ah[0]))

        def player(sock, sid, name, x, z):
            e = [None] * 53
            e[0], e[1], e[2], e[3], e[4], e[5], e[6], e[7], e[8], e[9], e[18] = sock, sid, x, 0, z, name, 0, None, 100, 0, 0.0
            return e

        stop = asyncio.Event()

        async def ticker():
            x, n = 0.0, 0
            while not stop.is_set():
                x += 0.5
                await send("k", [BOT_SID, x, 0, 0, 90, 0, 0, 1, 0, 0, 0, None, 30], 10, int(time.time() * 1000) & 0xffffff)
                n += 1
                if n % 10 == 0:
                    await send("pi")
                await asyncio.sleep(0.1)

        async def reader():
            async for msg in ws:
                if isinstance(msg, str):
                    continue
                try:
                    name, args, trailer = unframe(msg)
                except Exception as e:  # noqa: BLE001
                    self.errors.append("undecodable client frame: %s" % e)
                    continue
                self.frames.append((name, args, trailer))
                if name == "en":
                    self.got_en.set()
            stop.set()

        async def reader_safe():
            try:
                await reader()
            except websockets.ConnectionClosed:
                stop.set()

        rd = asyncio.ensure_future(reader_safe())
        try:
            await send("io-init", "sockME")
            await send("load")
            for _ in range(150):
                if any(f[0] == "load" for f in self.frames):
                    break
                await asyncio.sleep(0.1)
            await send("init", 10, 0, 0, None, None, None, {}, None, 0, None, {"gor": True}, None, False, True, False, None, None, 0)
            await send("0", player("sockBOT", BOT_SID, "StubBot", 0.0, 0.0))
            for nonce, ct, _ in CC_CASES:
                await send("cc", nonce, None, ct)
            await send("cpt", ALTCHA)
            tk = asyncio.ensure_future(ticker())
            for _ in range(900):
                if self.got_en.is_set() or stop.is_set():
                    break
                await asyncio.sleep(0.1)
            if self.got_en.is_set():
                await send("frs")
                await send("0", player("sockME", MY_SID, "Guest_9", 4.0, 4.0))
                await send("ch", -1, "StubBot", "hello from the stub", 0)
                last_isn = 0
                while not stop.is_set():
                    qs = [f for f in self.frames if f[0] == "q"]
                    if qs:
                        last_isn = int(qs[-1][1][1])
                    sync = [last_isn, 42, 4.0, 0, 4.0, 0, 0, 0, 0.0, True, False, False, 0, False, False, 0, 0, 0, 0, True, False, 0, 0, None, 0]
                    await send("l", sync)
                    await asyncio.sleep(1.0)
            await rd
            tk.cancel()
        except (websockets.ConnectionClosed, asyncio.CancelledError):
            pass
        finally:
            stop.set()
            self.closed.set()

    def start_ws(self):
        started = threading.Event()

        def run():
            self.loop = asyncio.new_event_loop()
            asyncio.set_event_loop(self.loop)

            async def main():
                self.quit = asyncio.Event()
                async with ws_server.serve(self.handler, "127.0.0.1", self.ws_port, max_size=None):
                    started.set()
                    await self.quit.wait()

            self.loop.run_until_complete(main())
            self.loop.close()

        self.thread = threading.Thread(target=run, daemon=True)
        self.thread.start()
        started.wait(10)

    def stop(self):
        if self.httpd:
            self.httpd.shutdown()
            self.httpd.server_close()
        if self.loop:
            self.loop.call_soon_threadsafe(self.quit.set)
            self.thread.join(10)


# ---------------------------------------------------------------- checks of one run

def check_matchmaker(label, stub, tokens, client_text):
    """The live client's order (game.deob.js seek, :147445): seek-game without a validation token,
    CaptchaRequired, the player's Turnstile token, generate-token, seek-game with both; once more for
    each token the matchmaker refused (tokens: the player's, in order; the last one is let through)."""
    paths = [p for p, _, _ in stub.http_log]
    want = ["/seek-game"] + ["/generate-token", "/seek-game"] * len(tokens)
    check(f"{label}: seek-game, CaptchaRequired, generate-token, seek-game (the live order)", paths == want, paths)
    seeks = [q for p, q, _ in stub.http_log if p == "/seek-game"]
    raws = [r for p, _, r in stub.http_log if p == "/seek-game"]
    dq = json.dumps({"v": BUILD_VERSION}, separators=(",", ":"))
    first = seeks[0] if seeks else []
    q = dict(first)
    check(f"{label}: the first seek-game is matchmakerSeek's, without a validationToken",
          [k for k, _ in first] == ["hostname", "region", "autoChangeGame", "dataQuery"] and q.get("hostname") == "krunker.io" and
          q.get("autoChangeGame") == "false" and q.get("dataQuery") == dq and q.get("region"), raws[:1])
    for i, token in enumerate(tokens):
        retry = seeks[i + 1] if len(seeks) > i + 1 else []
        q2 = dict(retry)
        check(f"{label}: retry {i + 1} has a fresh validationToken and the player's captchaToken",
              [k for k, _ in retry] == ["hostname", "region", "autoChangeGame", "validationToken", "dataQuery", "captchaToken"] and
              q2.get("validationToken") == (stub.issued[i] if len(stub.issued) > i else None) and q2.get("captchaToken") == token and
              q2.get("dataQuery") == dq, [k for k, _ in retry])
    check(f"{label}: the matchmaker let the last retry through", stub.captcha_ok.is_set())
    check(f"{label}: no Turnstile token reaches the log", not any(t in client_text for t in tokens))


def check_run(label, stub, client_lines, cheat_expected):
    check(f"{label}: WebSocket /ws?gameId=&clientKey=", stub.ws_paths[:1] == [f"/ws?gameId={GAME_ID}&clientKey={CLIENT_ID}"], stub.ws_paths)
    check(f"{label}: no undecodable frames", not stub.errors, stub.errors[:3])

    frames = stub.frames
    names = [f[0] for f in frames]
    ah, bad = 0, []
    for name, _, trailer in frames:
        if name != "cptR":
            ah = (ah + AHKEY) & 0xff
        if trailer != ah:
            bad.append((name, trailer, ah))
    check(f"{label}: every client frame's ahNum trailer ({len(frames)} frames)", frames and not bad, bad[:5])
    check(f"{label}: \"load\" answered first", names[:1] == ["load"], names[:5])

    en = next((f[1] for f in frames if f[0] == "en"), None)
    check(f"{label}: \"en\" with the enter array and the 11 flags",
          en is not None and len(en) == 12 and isinstance(en[0], list) and len(en[0]) == 35 and isinstance(en[0][0], int) and
          en[1] == 16 and en[2] == 18, en if en is None else en[:3])

    qs = [f[1] for f in frames if f[0] == "q"]
    isns = [int(a[1]) for a in qs if len(a) > 1]
    moved = any(isinstance(a[5] if len(a) > 5 else None, dict) and any(k.endswith("-4") and v >= 0 for k, v in a[5].items()) for a in qs)
    check(f"{label}: \"q\" windows (isn ascending, dts, a movement key)",
          len(qs) >= 5 and isns == sorted(isns) and all(isinstance(a[2], (str, list)) for a in qs) and moved,
          f"{len(qs)} q, isn {isns[:6]}, first {qs[:1]}")
    ct = [f[1] for f in frames if f[0] == "ct"]
    check(f"{label}: \"ct\" [0, text]", any(len(a) == 2 and a[0] == 0 and a[1] == "hello official" for a in ct), ct)
    check(f"{label}: \"po\" answers \"pi\"", "po" in names, names.count("po"))
    check(f"{label}: \"v\" answers \"l\"", "v" in names, names.count("v"))

    ccr = {f[1][0]: f[1] for f in frames if f[0] == "ccr" and f[1]}
    for nonce, _, want in CC_CASES:
        got = ccr.get(nonce)
        if want is None:
            check(f"{label}: cc {nonce} (garbage): no ccr", got is None, got)
        elif cheat_expected or got is not None:
            check(f"{label}: ccr {nonce} = [nonce, xorB64(JSON(R)), fnv]", got == [nonce, want[0], want[1]], f"{got} want {want}")
        else:
            check(f"{label}: cc {nonce}: no evaluator in this build, so no ccr", got is None, got)
    cptr = [f[1] for f in frames if f[0] == "cptR"]
    sol = None
    if cptr and cptr[0]:
        try:
            sol = json.loads(base64.b64decode(cptr[0][0]))
        except Exception:  # noqa: BLE001
            sol = cptr[0]
    check(f"{label}: \"cptR\" solves the ALTCHA proof of work", isinstance(sol, dict) and sol.get("number") == ALTCHA_NUMBER and
          sol.get("challenge") == ALTCHA["challenge"] and sol.get("salt") == ALTCHA_SALT and sol.get("signature") == "e2esig", sol)

    text = "\n".join(client_lines)
    check(f"{label}: the client joined as the stub's sid", f"joined as sid {MY_SID}" in text)
    check(f"{label}: init translated (map 10, mode 0)", "init: map 10, mode 0" in text)
    return text


# ---------------------------------------------------------------- native

NATIVE_TOKEN = "0.e2eNativeCallbackToken_" + "x" * 300 + ".sig-AbC_123"
PASTE_TOKEN = "0.e2ePastedToken_" + "y" * 200 + ".sig-DeF_456"
BURNT_TOKEN = "0.e2eAlreadyUsedToken_" + "u" * 120 + ".sig-MnO_345"  # the stub refuses it (used / expired)
CB_TTL_MS = 4000  # KRUNKNATIVE_KRUNKER_CAPTCHA_TTL_MS: the callback's lifetime in the callback run


class Log:
    """Tails a client's log file while it runs."""

    def __init__(self, path):
        self.path, self.text = path, ""

    def read(self):
        try:
            with open(self.path, errors="replace") as f:
                self.text = f.read()
        except OSError:
            pass
        return self.text

    def wait(self, pattern, timeout, proc=None):
        end = time.time() + timeout
        while time.time() < end:
            m = re.search(pattern, self.read())
            if m:
                return m
            if proc is not None and proc.poll() is not None:
                return None
            time.sleep(0.1)
        return None


def post(url, body, origin="https://krunker.io", ctype="text/plain"):
    """POST to the captcha callback: the HTTP status, or "refused" when nothing listens."""
    headers = {"Content-Type": ctype}
    if origin:
        headers["Origin"] = origin
    req = urllib.request.Request(url, data=body.encode() if isinstance(body, str) else body, headers=headers, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=5) as r:
            return r.status
    except urllib.error.HTTPError as e:
        return e.code
    except (urllib.error.URLError, ConnectionError, OSError):
        return "refused"


def launch_native(label, script_text, extra_env, args=("--krunker",)):
    mm, wsp = kn_proto.free_ports(2)
    stub = Stub(mm, wsp)
    stub.start_http()
    stub.start_ws()
    script = os.path.join(WORK, f"{label}.script")
    with open(script, "w") as f:
        f.write(script_text)
    log = os.path.join(WORK, f"{label}.log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": os.path.join(WORK, label + "-cfg"), "XDG_STATE_HOME": os.path.join(WORK, label + "-state"),
                "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "KRUNKNATIVE_INPUT_SCRIPT": script,
                "KRUNKNATIVE_WINDOW_SIZE": "960x540", "KRUNKNATIVE_KRUNKER_MATCHMAKER": "http://127.0.0.1:%d" % mm,
                "KRUNKNATIVE_KRUNKER_TRACE": "1", "KRUNKNATIVE_ASSETS_DIR": ASSETS, "KRUNKNATIVE_ASSET_FETCH": "off"})
    env.update(extra_env)
    out = open(log, "w")
    p = subprocess.Popen(["stdbuf", "-oL", "-eL", f"{BUILD}/client_bin", *args], stdout=out, stderr=subprocess.STDOUT,
                         stdin=subprocess.DEVNULL, env=env, start_new_session=True, cwd=ROOT)
    return stub, p, out, Log(log)


def finish_native(label, stub, p, out, log, timeout=240):
    try:
        p.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        check(f"{label}: client_bin finished its script", False, f"timeout, log {log.path}")
    finally:
        if p.poll() is None:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
        out.close()
    stub.closed.wait(5)
    stub.stop()
    return log.read().splitlines()


def cb_url_from(m):
    return m.group(1) if m else None


def run_native():
    """The callback path, then the whole game (the original native run)."""
    label = "native"
    script = ("0 boot\n1500 text @cb [id=knCaptchaCallback]\n1600 vis @modal knCaptchaHolder knCaptchaManual knCaptchaCbRow\n"
              "8000 clickid knCaptchaOpen\n8300 text @cb2 [id=knCaptchaCallback]\n9000 play\n"
              "14000 players\n14100 down W\n15500 up W\n15700 players\n15900 con say hello official\n16900 players\n17000 state\n"
              "18700 players\n18900 quit\n")
    stub, p, out, log = launch_native(label, script, {"KRUNKNATIVE_KRUNKER_CAPTCHA_TTL_MS": str(CB_TTL_MS)})
    try:
        url = cb_url_from(log.wait(r"\[input_script\] text t=\d+ @cb n=1 checked=0 value=(http://127\.0\.0\.1:\d+/[0-9a-f]{32}) ", 90, p))
        check(f"{label}: the modal shows a one-time 127.0.0.1 callback", url, log.path)
        vis = log.wait(r"\[input_script\] vis t=\d+ @modal (.*)", 10, p)
        check(f"{label}: the Security Challenge modal is shown (manual path)",
              vis and "knCaptchaHolder=1" in vis.group(1) and "knCaptchaManual=1" in vis.group(1) and "knCaptchaCbRow=1" in vis.group(1),
              vis.group(1) if vis else None)
        if url:
            base, secret = url.rsplit("/", 1)
            wrong = "%s/%s" % (base, ("0" if secret[0] != "0" else "1") + secret[1:])
            check(f"{label}: callback: a wrong secret gets 404", post(wrong, NATIVE_TOKEN) == 404)
            check(f"{label}: callback: a foreign Origin gets 403", post(url, NATIVE_TOKEN, origin="https://evil.example") == 403)
            check(f"{label}: callback: a body that is not a token gets 400", post(url, "not a token!") == 400)
            check(f"{label}: callback: the preflight from krunker.io is answered",
                  post_options(url) == 204)
            expired = log.wait(r"\[krunker\] captcha callback expired", CB_TTL_MS / 1000 + 8, p)
            r = post(url, NATIVE_TOKEN)
            check(f"{label}: callback: after its TTL the token is refused", expired and r in ("refused", 410), r)
        url2 = cb_url_from(log.wait(r"\[input_script\] text t=\d+ @cb2 n=1 checked=0 value=(http://127\.0\.0\.1:\d+/[0-9a-f]{32}) ", 30, p))
        check(f"{label}: \"Open krunker.io\" re-arms a new callback (new secret)", url2 and url2 != url, url2)
        if url2:
            r1 = post(url2, json.dumps({"token": NATIVE_TOKEN}), ctype="application/json")
            r2 = post(url2, NATIVE_TOKEN + "z")
            check(f"{label}: callback: the token is taken (200)", r1 == 200, r1)
            check(f"{label}: callback: a second token is refused", r2 in (410, "refused"), r2)
    finally:
        lines = finish_native(label, stub, p, out, log)
    text = "\n".join(lines)
    check(f"{label}: the browser link carried the callback in its fragment, not in the log",
          "https://krunker.io/#(not logged)" in text and (not url or url.rsplit("/", 1)[1] not in re.sub(r"value=http://127\.0\.0\.1:\d+/[0-9a-f]{32}", "", text)))
    check_matchmaker(label, stub, [NATIVE_TOKEN], text)
    text = check_run(label, stub, lines, True)  # client_bin evaluates cc with QuickJS (krunker_cheat_qjs.c)
    rows = [l for l in lines if "[input_script] players" in l]
    xs = []
    for row in rows:
        m = re.search(r" %d:StubBot@([-\d.]+)," % BOT_SID, row)
        if m:
            xs.append(float(m.group(1)))
    check("native: the stub player is in the game and moves", len(xs) >= 2 and xs[-1] > xs[0], f"x over time {xs}")
    check("native: our own player is in the game (sid 9, marked *)", any(re.search(r" %d:[^ ]*\*" % MY_SID, r) for r in rows), rows[-1:] if rows else "")
    check("native: the stub's chat line reached the client", "hello from the stub" in text)


def post_options(url):
    req = urllib.request.Request(url, method="OPTIONS", headers={"Origin": "https://krunker.io", "Access-Control-Request-Method": "POST",
                                                                  "Access-Control-Request-Private-Network": "true"})
    try:
        with urllib.request.urlopen(req, timeout=5) as r:
            ok = r.headers.get("Access-Control-Allow-Private-Network") == "true" and r.headers.get("Access-Control-Allow-Origin") == "https://krunker.io"
            return r.status if ok else "headers %s" % dict(r.headers)
    except urllib.error.HTTPError as e:
        return e.code
    except (urllib.error.URLError, OSError):
        return "refused"


def run_native_paste():
    """The paste box: a bad paste is refused in the modal; a token the matchmaker refuses (used or
    expired) brings the modal back saying so; the next one is used once."""
    label = "native-paste"
    script = ("0 boot\n1400 rect @r knCaptchaToken knCaptchaUse knCaptchaModal\n1450 frames %s 1\n"
              "1500 clickid knCaptchaToken\n1600 type not a token!\n1700 clickid knCaptchaUse\n1800 text @bad [id=knCaptchaStatus]\n"
              "1900 vis @bad knCaptchaHolder\n1950 clickid knCaptchaToken\n2000 down CTRL+A\n2050 up CTRL+A\n2100 down DELETE\n2150 up DELETE\n"
              "2300 type %s\n2400 clickid knCaptchaUse\n2900 text @again [id=knCaptchaStatus]\n2950 vis @again knCaptchaManual\n"
              "3000 clickid knCaptchaToken\n3100 type %s\n3200 clickid knCaptchaUse\n3400 vis @used knCaptchaManual knCaptchaCancel\n"
              "3500 text @box [id=knCaptchaToken]\n3800 play\n8000 players\n8200 quit\n"
              % (os.path.join(WORK, "native-modal"), BURNT_TOKEN, PASTE_TOKEN))
    stub, p, out, log = launch_native(label, script, {})
    stub.used_captchas.add(BURNT_TOKEN)
    lines = finish_native(label, stub, p, out, log, timeout=180)
    text = "\n".join(lines)
    bad = re.search(r"\[input_script\] text t=\d+ @bad n=1 checked=0 value= text=(.*)", text)
    check(f"{label}: a paste that is not a token is refused in the modal", bad and "not a Turnstile token" in bad.group(1), bad.group(1) if bad else None)
    check(f"{label}: the modal stays up after the bad paste", "@bad knCaptchaHolder=1" in text)
    again = re.search(r"\[input_script\] text t=\d+ @again n=1 checked=0 value= text=(.*)", text)
    check(f"{label}: a refused token brings the modal back, saying so", again and "refused that token" in again.group(1) and
          "@again knCaptchaManual=1" in text and "attempt 2, the last token was refused" in text, again.group(1) if again else None)
    # (the "cpt" proof-of-work state may already show the passive modal: the request's parts are gone)
    check(f"{label}: the good paste closes the modal", "@used knCaptchaManual=0 knCaptchaCancel=0" in text)
    box = re.search(r"\[input_script\] text t=\d+ @box n=1 checked=0 value=(\S*) ", text)
    check(f"{label}: the paste box is emptied", box is not None and box.group(1) == "", box.group(0) if box else None)
    check(f"{label}: the bad paste never reached the matchmaker", not any("not a token" in r for _, _, r in stub.http_log))
    # (the `type` lines of the input script are the test's echo of what it typed: not the client's)
    check_matchmaker(label, stub, [BURNT_TOKEN, PASTE_TOKEN], "\n".join(l for l in lines if "[input_script]" not in l))
    check(f"{label}: the client joined as the stub's sid", f"joined as sid {MY_SID}" in text)


def run_native_lobby():
    """The default (no KRUNKNATIVE_KRUNKER_JOIN): a click on the official lobby in the server browser
    joins it in this client. From the offline game: the Servers window lists the stub's lobby, the
    click seeks it, the Security Challenge modal asks; Cancel ends the join saying so; a second click
    and a pasted token join the game."""
    label = "native-lobby"
    script = ("0 menu\n300 call showWindow 2\n3000 text @status [id=kioStatus]\n3100 nodes @rows .quickJoin[title*=krunker]\n"
              "3200 call krunkerJoin \"%s\"\n4500 vis @ask1 knCaptchaHolder knCaptchaManual\n4600 clickid knCaptchaCancel\n"
              "5200 vis @cancelled knCaptchaHolder\n5300 call showWindow 2\n5800 call krunkerJoin \"%s\"\n7000 vis @ask2 knCaptchaHolder\n"
              "7100 clickid knCaptchaToken\n7200 type %s\n7300 clickid knCaptchaUse\n8000 play\n13000 players\n13200 quit\n"
              % (GAME_ID, GAME_ID, PASTE_TOKEN))
    stub, p, out, log = launch_native(label, script, {}, args=("--offline",))
    lines = finish_native(label, stub, p, out, log, timeout=180)
    text = "\n".join(lines)
    st = re.search(r"\[input_script\] text t=\d+ @status n=1 .*?text=(.*)", text)
    check(f"{label}: the KRUNKER.IO section says its lobbies join here", st and "official lobbies: pick one to join it here" in st.group(1),
          st.group(1) if st else None)
    check(f"{label}: no \"can't be joined\" text anywhere", not re.search(r"can.t be joined|cannot be joined|can.t join krunker", text))
    rows = re.search(r"nodes t=\d+ @rows elements=\d+ match=(\d+)", text)
    check(f"{label}: the stub's lobby has its krunker.io group", rows and rows.group(1) == "1", rows.group(0) if rows else None)
    check(f"{label}: the click joins in-client (no browser)", f"[krunker] official lobby {GAME_ID}: joining in-client" in text and
          "open (disabled by KRUNKNATIVE_NO_BROWSER)" not in text, text[-400:])
    check(f"{label}: the Security Challenge modal asks", "@ask1 knCaptchaHolder=1 knCaptchaManual=1" in text)
    check(f"{label}: Cancel ends the join, saying the challenge was closed",
          "@cancelled knCaptchaHolder=0" in text and "You closed krunker.io's security challenge, so no game was joined" in text)
    check(f"{label}: the second click asks again", "@ask2 knCaptchaHolder=1" in text)
    seeks = [q for pth, q, _ in stub.http_log if pth == "/seek-game"]
    paths = [pth for pth, _, _ in stub.http_log if pth != "/game-list"]
    check(f"{label}: seek, (cancel), seek, generate-token, seek with the token", paths == ["/seek-game", "/seek-game", "/generate-token", "/seek-game"],
          paths)
    check(f"{label}: every seek asks for the clicked lobby (game=, its region)", seeks and all(dict(q).get("game") == GAME_ID and
          dict(q).get("region") == "de-fra" for q in seeks), seeks[:1])
    check(f"{label}: the matchmaker let the token through", stub.captcha_ok.is_set())
    check(f"{label}: WebSocket /ws?gameId=&clientKey=", stub.ws_paths[:1] == [f"/ws?gameId={GAME_ID}&clientKey={CLIENT_ID}"], stub.ws_paths)
    check(f"{label}: the client joined as the stub's sid", f"joined as sid {MY_SID}" in text)
    check(f"{label}: no Turnstile token reaches the log", PASTE_TOKEN not in "\n".join(l for l in lines if "[input_script]" not in l))


def run_native_optout():
    """KRUNKNATIVE_KRUNKER_JOIN=browser: the click opens krunker.io/?game=<id> in the system browser
    (KRUNKNATIVE_NO_BROWSER logs it), nothing is sought in-client."""
    label = "native-optout"
    script = ("0 menu\n300 call showWindow 2\n3000 text @status [id=kioStatus]\n3200 call krunkerJoin \"%s\"\n4200 quit\n" % GAME_ID)
    stub, p, out, log = launch_native(label, script, {"KRUNKNATIVE_KRUNKER_JOIN": "browser"}, args=("--offline",))
    lines = finish_native(label, stub, p, out, log, timeout=120)
    text = "\n".join(lines)
    st = re.search(r"\[input_script\] text t=\d+ @status n=1 .*?text=(.*)", text)
    check(f"{label}: the KRUNKER.IO section says its lobbies open in the browser", st and "official lobbies open in your browser" in st.group(1),
          st.group(1) if st else None)
    check(f"{label}: the click opens the lobby in the browser", f"open (disabled by KRUNKNATIVE_NO_BROWSER): https://krunker.io/?game={GAME_ID}" in text,
          text[-400:])
    check(f"{label}: nothing is sought in-client", not any(pth == "/seek-game" for pth, _, _ in stub.http_log) and not stub.ws_paths and
          "joining official game" not in text, [pth for pth, _, _ in stub.http_log])


class Cdp:
    def __init__(self, port):
        pages = []
        for _ in range(100):
            try:
                pages = json.load(urllib.request.urlopen("http://127.0.0.1:%d/json" % port, timeout=2))
                if any(p.get("type") == "page" for p in pages):
                    break
            except OSError:
                pass
            time.sleep(0.1)
        url = next(p["webSocketDebuggerUrl"] for p in pages if p.get("type") == "page")
        self.ws = ws_client.connect(url, max_size=None, open_timeout=10)
        self.id = 0
        self.console = []
        self.lock = threading.Lock()
        self.replies = {}
        self.cond = threading.Condition()
        self.intercept = None  # (url prefix, JS body): Fetch.requestPaused answers
        self.intercepted = []
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        try:
            for raw in self.ws:
                m = json.loads(raw)
                if m.get("method") == "Fetch.requestPaused":
                    url = m["params"]["request"]["url"]
                    self.intercepted.append(url)
                    rid = m["params"]["requestId"]
                    if self.intercept and url.startswith(self.intercept[0]):
                        self.post("Fetch.fulfillRequest", {"requestId": rid, "responseCode": 200, "responseHeaders": [
                            {"name": "Content-Type", "value": "application/javascript"}, {"name": "Access-Control-Allow-Origin", "value": "*"}],
                            "body": base64.b64encode(self.intercept[1].encode()).decode()})
                    else:
                        self.post("Fetch.continueRequest", {"requestId": rid})
                elif m.get("method") == "Runtime.consoleAPICalled":
                    self.console.append(" ".join(str(a.get("value", a.get("description", ""))) for a in m["params"]["args"]))
                elif "id" in m:
                    with self.cond:
                        self.replies[m["id"]] = m
                        self.cond.notify_all()
        except Exception:  # noqa: BLE001 (closed)
            pass

    def post(self, method, params=None):  # no reply awaited (from the reader thread)
        with self.lock:
            self.id += 1
            i = self.id
        self.ws.send(json.dumps({"id": i, "method": method, "params": params or {}}))

    def send(self, method, params=None, timeout=15):
        with self.lock:
            self.id += 1
            i = self.id
        self.ws.send(json.dumps({"id": i, "method": method, "params": params or {}}))
        end = time.time() + timeout
        with self.cond:
            while i not in self.replies and time.time() < end:
                self.cond.wait(0.2)
            return self.replies.pop(i, {})

    def eval(self, expr):
        r = self.send("Runtime.evaluate", {"expression": expr, "returnByValue": True, "userGesture": True})
        return r.get("result", {}).get("result", {}).get("value")

    def click(self, x, y):
        for t in ("mouseMoved", "mousePressed", "mouseReleased"):
            self.send("Input.dispatchMouseEvent", {"type": t, "x": x, "y": y, "button": "left" if t != "mouseMoved" else "none",
                                                   "buttons": 1 if t == "mousePressed" else 0, "clickCount": 0 if t == "mouseMoved" else 1})
            time.sleep(0.04)

    def key(self, kind, key, code, vk, text=None):
        p = {"type": kind, "key": key, "code": code, "windowsVirtualKeyCode": vk, "nativeVirtualKeyCode": vk}
        if text:
            p.update({"text": text, "unmodifiedText": text})
        self.send("Input.dispatchKeyEvent", p)


class StaticHandler(SimpleHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def translate_path(self, path):  # /assets/* from the asset tree, the rest from the web dist
        p = urllib.parse.unquote(urllib.parse.urlsplit(path).path)
        if p.startswith("/assets/"):
            return os.path.join(ASSETS, os.path.normpath(p[len("/assets/"):]).lstrip("/"))
        return os.path.join(DIST, os.path.normpath(p).lstrip("/"))


WEB_TOKEN = "0.e2eWidgetToken_" + "w" * 300 + ".sig-GhI_789"
WEB_PASTE_TOKEN = "0.e2eWebPastedToken_" + "v" * 250 + ".sig-JkL_012"
TURNSTILE_URL = "https://challenges.cloudflare.com/turnstile/"


def turnstile_stub(mode):
    """api.js for the test's browser only: "ok" calls the widget's callback with WEB_TOKEN, else
    the error-callback with that code (110200: the sitekey is not for this domain)."""
    return ("(function(){var mode=%s,tok=%s;window.turnstile={render:function(el,o){var d=document.createElement('div');"
            "d.className='kn-e2e-turnstile';d.textContent='stub turnstile';el.appendChild(d);setTimeout(function(){"
            "if(mode==='ok')o.callback(tok);else o['error-callback'](mode);},400);return 'stub-w1';},"
            "remove:function(){},reset:function(){},getResponse:function(){return tok;}};})();") % (json.dumps(mode), json.dumps(WEB_TOKEN))


def run_web(mode):
    """mode "ok": the widget's token, then the whole game (the original web run); "110200": the
    domain error, the fallback text, a pasted token."""
    label = "web" if mode == "ok" else "web-domain"
    mm, wsp, web, cdpp = kn_proto.free_ports(4)
    stub = Stub(mm, wsp)
    stub.start_http()
    stub.start_ws()
    httpd = ThreadingHTTPServer(("127.0.0.1", web), StaticHandler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    chrome_log = open(os.path.join(WORK, f"{label}-chrome.log"), "w")
    chrome = subprocess.Popen([CHROME, "--headless=new", "--no-sandbox", "--use-gl=angle", "--use-angle=swiftshader", "--enable-unsafe-swiftshader",
                               "--user-data-dir=" + os.path.join(WORK, label + "-chrome"), "--remote-debugging-port=%d" % cdpp, "--window-size=1280,720",
                               "about:blank"], stdout=chrome_log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, start_new_session=True)
    lines = []
    token = WEB_TOKEN if mode == "ok" else WEB_PASTE_TOKEN
    statuses = set()
    try:
        cdp = Cdp(cdpp)
        cdp.send("Runtime.enable")
        cdp.send("Page.enable")
        cdp.intercept = (TURNSTILE_URL, turnstile_stub(mode))
        cdp.send("Fetch.enable", {"patterns": [{"urlPattern": TURNSTILE_URL + "*"}]})
        url = "http://127.0.0.1:%d/?krunker=1&krunkerMM=%s&krunkerTrace=1" % (web, urllib.parse.quote("http://127.0.0.1:%d" % mm, safe=""))
        cdp.send("Page.navigate", {"url": url})
        modal_js = ("(() => { const h = document.getElementById('knCaptchaHolder'); if (!h) return null;"
                    " const t = id => { const e = document.getElementById(id); return e ? e.textContent : null; };"
                    " const vis = id => { const e = document.getElementById(id); return !!(e && e.offsetParent !== null && getComputedStyle(e).display !== 'none'); };"
                    " return {shown: getComputedStyle(h).display !== 'none', text: t('knCaptchaText'), status: t('knCaptchaStatus'),"
                    " manual: vis('knCaptchaManual'), widget: !!document.querySelector('#knTurnstileHolder .kn-e2e-turnstile'),"
                    " passive: h.classList.contains('kn-captcha-passive')}; })()")
        seen_modal = None
        end = time.time() + 120
        while time.time() < end and not stub.captcha_ok.is_set():
            time.sleep(0.25)
            st = cdp.eval(modal_js)
            if st and st.get("shown") and not st.get("passive"):
                seen_modal = seen_modal or dict(st)
                if mode != "ok" and st.get("manual"):
                    seen_modal = dict(st)
                    break
        check(f"{label}: the Security Challenge modal showed for CaptchaRequired", seen_modal is not None, "\n".join(cdp.console[-25:]))
        check(f"{label}: Turnstile's api.js was requested (and stubbed)", any(u.startswith(TURNSTILE_URL + "v0/api.js") for u in cdp.intercepted),
              cdp.intercepted[:3])
        if mode != "ok":
            text = (seen_modal or {}).get("text") or ""
            check(f"{label}: 110200 is explained (Turnstile keys are bound to their sites) and the paste box offered",
                  bool(seen_modal) and seen_modal.get("manual") and "110200" in text and "only work on the sites" in text, text)
            box = cdp.eval("(() => { const e = document.getElementById('knCaptchaToken'); const b = e.getBoundingClientRect();"
                           " return [b.x + b.width / 2, b.y + b.height / 2]; })()")
            if box:
                cdp.click(box[0], box[1])
                time.sleep(0.3)
                cdp.send("Input.insertText", {"text": token})
                time.sleep(0.3)
                use = cdp.eval("(() => { const b = document.getElementById('knCaptchaUse').getBoundingClientRect(); return [b.x + b.width / 2, b.y + b.height / 2]; })()")
                cdp.click(use[0], use[1])
            stub.captcha_ok.wait(30)
            time.sleep(1.0)
            st = cdp.eval(modal_js) or {}
            # (the "cpt" proof of work may show the passive state by now: no input parts left)
            check(f"{label}: the modal closed after the pasted token", not st.get("shown") or (st.get("passive") and not st.get("manual")), st)
        else:
            check(f"{label}: the stub widget rendered inside the modal", bool(seen_modal) and seen_modal.get("widget"), seen_modal)
            # CLICK TO PLAY once the map is in (menu_ui.c needs a trusted click), until the client's "en";
            # meanwhile the "cpt" proof-of-work modal is watched (passive, "Verifying…" then the result)
            end = time.time() + 150
            while time.time() < end and not stub.got_en.is_set():
                time.sleep(0.3)
                st = cdp.eval(modal_js)
                if st and st.get("shown") and st.get("passive"):
                    statuses.add(st.get("status"))
                box = cdp.eval("(() => { const e = document.getElementById('instructions'); if (!e || !/CLICK TO PLAY/.test(e.textContent)) return null;"
                               " const b = e.getBoundingClientRect(); return [b.x + b.width / 2, b.y + b.height / 2]; })()")
                if box:
                    cdp.click(box[0], box[1])
            check("web: the page entered the game (\"en\" sent)", stub.got_en.is_set(), "\n".join(cdp.console[-25:]))
            if stub.got_en.is_set():
                time.sleep(1.0)
                cdp.key("rawKeyDown", "w", "KeyW", 87)
                time.sleep(1.5)
                cdp.key("keyUp", "w", "KeyW", 87)
                time.sleep(0.5)
                cdp.key("keyDown", "Enter", "Enter", 13, "\r")
                cdp.key("keyUp", "Enter", "Enter", 13)
                time.sleep(0.4)
                cdp.send("Input.insertText", {"text": "hello official"})
                time.sleep(0.2)
                cdp.key("keyDown", "Enter", "Enter", 13, "\r")
                cdp.key("keyUp", "Enter", "Enter", 13)
                time.sleep(2.5)
                chat = cdp.eval("(() => { const c = document.getElementById('chatList'); return c ? c.innerText : ''; })()") or ""
                check("web: the stub's chat line is in #chatList", "hello from the stub" in chat, chat[-300:])
        lines = list(cdp.console)
        cdp.ws.close()
    finally:
        if chrome.poll() is None:
            os.killpg(chrome.pid, signal.SIGTERM)
            try:
                chrome.wait(10)
            except subprocess.TimeoutExpired:
                os.killpg(chrome.pid, signal.SIGKILL)
                chrome.wait()
        chrome_log.close()
        httpd.shutdown()
        httpd.server_close()
    stub.closed.wait(5)
    stub.stop()
    with open(os.path.join(WORK, f"{label}-console.log"), "w") as f:
        f.write("\n".join(lines))
    text = "\n".join(lines)
    check_matchmaker(label, stub, [token], text)
    if mode != "ok":
        check(f"{label}: the client joined as the stub's sid", f"joined as sid {MY_SID}" in text or stub.ws_paths, stub.ws_paths)
        return
    check("web: the \"cpt\" modal showed the proof of work and its result", any(s_ and s_.startswith("Verif") for s_ in statuses), statuses)
    check_run("web", stub, lines, True)
    k = [l for l in lines if "[krunker] <= k " in l]
    check("web: the stub player's \"k\" updates arrived", len(k) >= 5, len(k))


def main():
    if websockets is None:
        print("krunker_official_e2e: needs the Python websockets package: skipped")
        return 77
    ran = False
    if ONLY in ("", "native") and os.path.isfile(f"{BUILD}/client_bin"):
        run_native()
        run_native_paste()
        run_native_lobby()
        run_native_optout()
        ran = True
    if ONLY in ("", "web") and DIST and CHROME and os.path.isfile(os.path.join(DIST, "client.wasm")):
        run_web("ok")
        run_web("110200")
        ran = True
    elif ONLY == "web":
        print("krunker_official_e2e: the web run needs Chromium and a web build (WEB_DIST_DIR): skipped")
    if not ran:
        return 77
    if all(results):
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print(f"logs kept in {WORK}")
    print(f"{sum(results)}/{len(results)} passed")
    return 0 if results and all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
