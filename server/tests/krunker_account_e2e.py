#!/usr/bin/env python3
"""The krunker.io account backend (client/include/krunker_account.h) end to end, against local stubs of
krunker.io's API, FRVR's auth service, the matchmaker and a game server: nothing leaves the machine.

  python3 -I server/tests/krunker_account_e2e.py BUILD_DIR   (repo root)

One HTTP stub (ports from kn_proto.free_ports()) serves three bases:
  /api   krunker.io's API: POST /auth/login/username (checks username, password and the Turnstile token
         "b"; answers check_2fa), POST /auth/2fa/challenge/<id> (the code; answers login_ok with an access
         JWT that expires in 30 s and a refresh token), GET /player/me (Bearer: the account's name)
  /frvr  FRVR's auth: POST /v1/auth/refresh {"refreshToken"} -> 201 {accessToken, refreshToken}
  /mm    the matchmaker: GET /game-list (four lobbies of one mode), GET /seek-game (records accessToken)
and a WebSocket game server (records `at=`, sends io-init, "a" ACCOUNT_RESPONSE, load, init).

There is one account UI (backend_ui.h active_account()): v10's own login modal, header and Account
window, signing in to the ACTIVE BACKEND's account: krunker.io's in an official game, krunk.io's
elsewhere. No separate krunker.io modal, no "krunker.io: Guest (Login)" bar, no Accounts lines.

One headless client_bin (--offline: krunk.io is a guest, an input script) then:
  1. the menu: krunk.io's guest bar (v10's Login / Register), no krunker.io bar, the Servers window
     has no Accounts line; Quick Join joins the fullest Kanji lobby with room (not the fuller Burg one,
     not the full one) as a krunker.io guest (no accessToken, no at=)
  2. in the game, the menu shows the krunker.io guest (v10's guest bar); v10's login modal
     (loginOrRegister) with "(use username instead)": username + password, Login -> the Security
     Challenge (headless: the paste box) -> v10's "Two-Factor Authentication" form (six code boxes,
     not dismissable: a click on its overlay leaves it up) -> signed in (the name from /player/me),
     the modal closes, the header shows the krunker.io account; the open socket logs in with it
     (v10's rpc LOGIN: the server's "a" names it); the Account window is the krunker.io account's
  3. leaving the game switches the menu back to krunk.io's guest at once; the next join refreshes the
     about-to-expire session... (FRVR refresh), seeks with accessToken and opens the socket with at=;
     the game server's "a" names the account and its level, shown in the header
  4. Logout (the Account window's, in the game): back to a krunker.io guest (v10's login modal), the
     next join carries no accessToken / at=
Checks also that no password, code, Turnstile token, access or refresh token reaches the log, the
saved session is gone after the logout, and that no "guest only" text is left in the client's
krunker.io wording. Exit 0 when every check passed, 77 when it cannot run.
"""
import asyncio
import base64
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
# the web build (dist/web, from `make web`): the second run, in headless Chrome, when it is given
DIST = os.path.abspath(ARGS[1]) if len(ARGS) > 1 else None
CHROME = next((c for c in (os.environ.get("KRUNKNATIVE_CHROME"), shutil.which("chromium-browser"), shutil.which("chromium"),
                           shutil.which("google-chrome")) if c), None)
ONLY = next((a for a in sys.argv[1:] if a in ("--native", "--web")), "")
ROOT = os.getcwd()
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-krunker-account-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
ASSETS = os.environ.get("KRUNKNATIVE_ASSETS_DIR") or next(
    (d for d in (os.path.join(ROOT, "assets"), os.path.expanduser("~/src/KrunkNative/assets")) if os.path.isdir(os.path.join(d, "models"))),
    os.path.join(ROOT, "assets"))

LOGIN, PASSWORD, CODE = "kioplayer", "hunter2-Secret!x", "123456"
ACCOUNT_NAME, ACCOUNT_ID, SCORE, LEVEL = "KioPlayer", 777, 40000, 6  # floor(0.03 * sqrt(40000)) = 6
TURNSTILE = "0.kioLoginTurnstile_" + "t" * 48 + ".sig-KiO_123"
CHALLENGE = "chal-2fa-1"
REFRESH1, REFRESH2 = "rt-first-" + "r" * 40, "rt-second-" + "s" * 40
GAME_PICK = "TEST:kanji"
CLIENT_ID = "cid-acc"
KANJI = 10  # map catalogue index (shared/src/map_catalog.c OFFICIAL("kanji", "Kanji", 10, ...))
LOBBIES = [  # [id, region, players, max, data, timeLeft]: one mode (0), so one KRUNKER.IO group
    ["TEST:full", "de-fra", 8, 8, {"i": "Kanji", "g": 0}, 200],   # full
    ["TEST:burg", "de-fra", 7, 8, {"i": "Burg", "g": 0}, 200],    # fuller, but not the seek map
    ["TEST:pw", "de-fra", 6, 8, {"i": "Kanji", "g": 0, "pw": True}, 200],  # password
    [GAME_PICK, "de-fra", 2, 8, {"i": "Kanji", "g": 0}, 200],     # the pick
]

# krunker.io's store (client/src/ui/kio_store.c), in v10's shapes as gapi.svc.krunker.io answers them
# (shop/displays: {id, type, data, children, products}; ranked/shop: {skins}). Skins from the client's table.
SHOP_FUTURE = "2099-01-01T00:00:00.000Z"


def _product(pid, name, contents, price, **kw):
    p = {"id": pid, "name": name, "description": kw.pop("description", ""), "image_url": "", "active": True, "prices": [price],
         "contents": [dict(c, id=pid) for c in contents], "available": True, "display_ids": []}
    p.update(kw)
    return p


def _kr(amount):
    return {"id": 1, "price_ref": "io.krunker.v%d" % amount, "type": "virtual", "available": True,
            "virtual": {"amount": amount, "currency_id": 1, "currency": "KR"}}


def _usd(usd, ref):
    return {"id": 2, "price_ref": ref, "type": "real", "real": {"price_usd": usd}, "available": True}


SHOP_LIMITED = {
    "id": 4, "type": "front", "order": 0, "data": {},
    "children": [
        {"id": 5, "parent_id": 4, "order": 0, "type": "product", "data": {}, "product_id": 12},
        {"id": 6, "parent_id": 4, "order": 1, "type": "product", "data": {}, "product_id": 11},
        {"id": 7, "parent_id": 4, "order": 2, "type": "product", "data": {}, "product_id": 4},
        {"id": 8, "parent_id": 4, "order": 3, "type": "product", "data": {}, "product_id": 15},
        {"id": 9, "parent_id": 4, "order": 4, "type": "product", "data": {}, "product_id": 46},
        {"id": 33, "parent_id": 4, "order": 5, "type": "dynamic", "product_id": 33,
         "data": {"dynamic": {"min_quantity": 10, "max_quantity": 1000, "steps": [{"min_quantity": 500, "price_usd": 0.22}]}}},
        {"id": 40, "parent_id": 4, "order": 6, "type": "section", "data": {"title": "Weekend Picks", "description": "Gone on Monday"},
         "children": [{"id": 41, "parent_id": 40, "order": 0, "type": "product", "data": {}, "product_id": 35, "expires_at": SHOP_FUTURE}]},
    ],
    "products": {
        "12": _product(12, "Eterno Mug", [{"type": "skin", "ref": "7439", "quantity": 1}], _kr(2000),
                       purchase_limit=1, purchase_count=0, purchase_limit_scope="per_user"),
        "11": _product(11, "Selakuku", [{"type": "skin", "ref": "7438", "quantity": 1}], _usd(2.99, "io.krunker.skin7438"),
                       purchase_limit=1, purchase_count=0, purchase_limit_scope="per_user"),
        "4": _product(4, "Company Card", [{"type": "skin", "ref": "7431", "quantity": 1}], _kr(5000),
                      purchase_limit=1, purchase_count=1, purchase_limit_scope="per_user"),
        "15": _product(15, "Junk", [{"type": "currency", "ref": "2", "quantity": 100}], _kr(2000),
                       purchase_limit=1000, purchase_count=37, purchase_limit_scope="global"),
        "46": _product(46, "Season 10 Contributor Badge", [{"type": "badge", "ref": "3", "quantity": 1}], _usd(50, "io.krunker.badge67")),
        "33": _product(33, "Paint Ticket", [{"type": "skin", "ref": "7994", "quantity": 1}], _usd(0.25, "io.krunker.paint"),
                       description="Paint any paintable item"),
        "35": _product(35, "Hovi", [{"type": "skin", "ref": "7994", "quantity": 1}], _kr(100),
                       purchase_limit=1, purchase_count=0, purchase_limit_scope="per_user"),
    },
}
SHOP_ITEM_NAMES = ["Eterno Mug", "Selakuku", "Company Card", "Junk", "Season 10 Contributor Badge", "Paint Ticket", "Weekend Picks", "Hovi"]
RANKED_SHOP = {"skins": [
    {"id": 501, "skin_id": 7613, "price": 1500, "max_purchase": 0, "purchase_count": 0, "expires_at": SHOP_FUTURE},
    {"id": 502, "skin_id": 7578, "price": 800, "max_purchase": 1, "purchase_count": 0},
]}
RANKED_ITEM_NAMES = ["Sand Rhino", "Clan Cape"]

results = []


def check(name, ok, detail=""):
    results.append(bool(ok))
    show = detail and (not ok or len(str(detail)) <= 240)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + str(detail)) if show else ''}", flush=True)


def b64url(data):
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode()


def jwt(payload):
    return "%s.%s.%s" % (b64url(b'{"alg":"HS256","typ":"JWT"}'), b64url(json.dumps(payload).encode()), b64url(b"stub-signature-bytes"))


# ---------------------------------------------------------------- msgpack (the frames' subset)

def mp_encode(v):
    if v is None:
        return b"\xc0"
    if v is True:
        return b"\xc3"
    if v is False:
        return b"\xc2"
    if isinstance(v, int):
        if 0 <= v < 0x80:
            return bytes([v])
        if 0 <= v < 1 << 32:
            return b"\xce" + struct.pack(">I", v)
        return b"\xd3" + struct.pack(">q", v)
    if isinstance(v, float):
        return b"\xcb" + struct.pack(">d", v)
    if isinstance(v, str):
        b = v.encode()
        return (bytes([0xa0 | len(b)]) if len(b) < 32 else b"\xd9" + bytes([len(b)]) if len(b) < 256 else b"\xda" + struct.pack(">H", len(b))) + b
    if isinstance(v, (list, tuple)):
        return (bytes([0x90 | len(v)]) if len(v) < 16 else b"\xdc" + struct.pack(">H", len(v))) + b"".join(mp_encode(x) for x in v)
    if isinstance(v, dict):
        return (bytes([0x80 | len(v)]) if len(v) < 16 else b"\xde" + struct.pack(">H", len(v))) + b"".join(mp_encode(str(k)) + mp_encode(x) for k, x in v.items())
    raise TypeError(type(v))


def frame(name, *args, ah=0):
    return mp_encode([name, *args]) + bytes([ah >> 4 & 0xf, ah & 0xf])


def frame_name(data):  # the packet name of a client frame (fixstr / str8 first item of the array)
    body = data[:-2]
    i = 1 if 0x90 <= body[0] <= 0x9f else 3
    t = body[i]
    if 0xa0 <= t <= 0xbf:
        return body[i + 1:i + 1 + (t & 31)].decode(errors="replace")
    return "?"


# ---------------------------------------------------------------- the stubs

MY_SID = 9  # our player's sid in the stub's "0" (the socket id is io-init's "sockME")


def player_entry(sock, sid, name, x, y, z, level=0):
    """One player of "0" ADD_PLAYERS: 53 (0x35) values, the live client's offsets (game.deob.js:220269-220289,
    players.add :171902): [0] socket id [1] sid [2] x [3] y [4] z [5] name [6] classIndex [7] health (null:
    maxHealth) [8] maxHealth [9] team [10] level [11] clan [16] isHacker [18] xDire. No offset carries a
    player's krunker.io role (isDev / isMod): v10 knows only its own account's rights (its "a")."""
    e = [None] * 53
    e[0], e[1], e[2], e[3], e[4], e[5], e[6], e[8], e[9], e[10], e[18] = sock, sid, x, y, z, name, 0, 100, 0, level, 0.0
    return e


class Stub:
    def __init__(self, http_port, ws_port):
        self.http_port, self.ws_port = http_port, ws_port
        # opt-in (account_unify_e2e.py): players = [(socket id, sid, name)] added with "0" after init, and our
        # own player ("sockME", MY_SID, our_name) added on the client's "en", as a live server spawns it;
        # account_data = the "a" data of each sign-in in turn, merged into the stub account's row
        # (e.g. {"player_role": 2}: v10 isMod; {"player_developer": 1}: isDev)
        self.players = None
        self.our_name = "Guest_%d" % MY_SID
        self.account_data = []
        self.a_sent = []  # the "a" data rows sent, in order
        self.frame_handlers = {}  # extra answers to client frames: {name: async fn(send, raw frame bytes)}
        self.requests = []      # (method, path, query dict, headers dict, body dict | None)
        self.ws_paths = []
        self.ws_frames = []
        self.rpc_logins = 0
        self.access1 = jwt({"exp": int(time.time()) + 30, "sub": str(ACCOUNT_ID)})  # within the 60 s refresh margin
        self.access2 = jwt({"exp": int(time.time()) + 3600, "sub": str(ACCOUNT_ID)})
        self.loop = None
        self.httpd = None

    def start(self):
        stub = self

        class H(BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def reply(self, status, obj):
                body = json.dumps(obj).encode()
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.send_header("Access-Control-Allow-Origin", "*")
                self.end_headers()
                self.wfile.write(body)

            def do_OPTIONS(self):  # CORS preflight (the web build's requests)
                self.send_response(204)
                self.send_header("Access-Control-Allow-Origin", "*")
                self.send_header("Access-Control-Allow-Headers", "*")
                self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
                self.send_header("Content-Length", "0")
                self.end_headers()

            def record(self, method):
                u = urllib.parse.urlsplit(self.path)
                q = dict(urllib.parse.parse_qsl(u.query, keep_blank_values=True))
                body = None
                n = int(self.headers.get("Content-Length") or 0)
                if n:
                    try:
                        body = json.loads(self.rfile.read(n))
                    except ValueError:
                        body = "(not JSON)"
                stub.requests.append((method, u.path, q, {k.lower(): v for k, v in self.headers.items()}, body))
                return u.path, q, body

            def do_GET(self):
                path, q, _ = self.record("GET")
                if path == "/mm/game-list":
                    self.reply(200, {"success": True, "totalPlayerCount": 23, "games": LOBBIES})
                elif path == "/mm/seek-game":
                    self.reply(200, {"host": "127.0.0.1", "port": stub.ws_port, "gameId": q.get("game") or GAME_PICK, "clientId": CLIENT_ID,
                                     "data": {"i": "Kanji", "g": 0}})
                elif path == "/api/shop/displays" or path.startswith("/api/shop/displays/"):  # shop.getDisplayByTag / getDisplay
                    if q.get("tag") == "limited" or path == "/api/shop/displays/%d" % SHOP_LIMITED["id"]:
                        self.reply(200, {"data": SHOP_LIMITED, "error": None})
                    else:
                        self.reply(404, {"data": None, "error": "display not found"})
                elif path == "/api/ranked/shop":  # ranked.getShop: the account's shop
                    auth = self.headers.get("Authorization", "")
                    if auth in ("Bearer " + stub.access1, "Bearer " + stub.access2):
                        self.reply(200, {"data": RANKED_SHOP, "error": None})
                    else:
                        self.reply(401, {"error": "authorization header required"})
                elif path == "/api/player/me":
                    auth = self.headers.get("Authorization", "")
                    if auth == "Bearer " + stub.access1 or auth == "Bearer " + stub.access2:
                        self.reply(200, {"data": {"username": ACCOUNT_NAME, "id": ACCOUNT_ID}, "error": None})
                    else:
                        self.reply(401, {"data": None, "error": "Unauthorized"})
                else:
                    self.reply(404, {"error": "not found"})

            def do_POST(self):
                path, _, body = self.record("POST")
                body = body if isinstance(body, dict) else {}
                if path == "/api/auth/login/username":
                    ok = body.get("username") == LOGIN and body.get("password") == PASSWORD and body.get("b") == TURNSTILE
                    if ok:
                        self.reply(200, {"data": {"type": "check_2fa", "challenge_id": CHALLENGE}, "error": None})
                    else:
                        self.reply(200, {"data": None, "error": "Invalid username or password"})
                elif path == "/api/auth/2fa/challenge/" + CHALLENGE:
                    if body.get("code") == CODE:
                        self.reply(200, {"data": {"type": "login_ok", "access_token": stub.access1, "refresh_token": REFRESH1}, "error": None})
                    else:
                        self.reply(200, {"data": None, "error": "Invalid 2FA code"})
                elif path == "/frvr/v1/auth/refresh":
                    if body.get("refreshToken") in (REFRESH1, REFRESH2):
                        self.reply(201, {"accessToken": stub.access2, "refreshToken": REFRESH2})
                    else:
                        self.reply(401, {"error": "invalid refresh token"})
                else:
                    self.reply(404, {"error": "not found"})

        self.httpd = ThreadingHTTPServer(("127.0.0.1", self.http_port), H)
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

        started = threading.Event()

        async def handler(ws):
            path = ws.request.path
            self.ws_paths.append(path)
            q = dict(urllib.parse.parse_qsl(urllib.parse.urlsplit(path).query))
            ah = [0]

            async def send(name, *args):
                ah[0] = (ah[0] + 0x11) & 0xff
                await ws.send(frame(name, *args, ah=ah[0]))

            async def send_account():
                data = {"player_score": SCORE, "player_kills": 12}
                data.update(self.account_data[len(self.a_sent)] if len(self.a_sent) < len(self.account_data) else {})
                self.a_sent.append(data)
                await send("a", None, ACCOUNT_ID, ACCOUNT_NAME, data, "legacy-token")

            async def reader():
                async for msg in ws:
                    if not isinstance(msg, str):
                        name = frame_name(msg)
                        self.ws_frames.append(name)
                        # v10's rpc LOGIN (["_0", id, "login", accessToken]) on a guest's socket: the server signs it in
                        if name == "_0" and b"login" in msg and (self.access1.encode() in msg or self.access2.encode() in msg):
                            self.rpc_logins += 1
                            await send_account()
                        # ENTER_GAME: the server spawns our player (its "0" with io-init's socket id: isYou, :220268)
                        if name == "en" and self.players is not None:
                            await send("0", player_entry("sockME", MY_SID, self.our_name, -343.0, -143.0, -28.0))
                        handler = self.frame_handlers.get(name)
                        if handler:
                            await handler(send, msg)

            rd = asyncio.ensure_future(reader())
            try:
                await send("io-init", "sockME")
                if q.get("at") == self.access2:  # the account's socket: the server signs it in
                    await send_account()
                await send("load")
                await asyncio.sleep(0.5)
                await send("init", KANJI, 0, 0, None, None, None, {}, None, 0, None, {"gor": True}, None, False, True, False, None, None, 0)
                if self.players is not None:  # the players already in the game (10_kanji.json's spawns 1-3)
                    spots = [(-206.0, -140.0, -395.0), (-188.0, -129.0, -455.0), (191.0, -139.0, -466.0)]
                    entries = []
                    for i, (sock, sid, pname) in enumerate(self.players):
                        x, y, z = spots[i % len(spots)]
                        entries += player_entry(sock, sid, pname, x, y, z, level=10 + i)
                    await send("0", entries)
                await rd
            except (websockets.ConnectionClosed, asyncio.CancelledError):
                pass

        def run():
            self.loop = asyncio.new_event_loop()
            asyncio.set_event_loop(self.loop)

            async def main():
                self.quit = asyncio.Event()
                async with ws_server.serve(handler, "127.0.0.1", self.ws_port, max_size=None):
                    started.set()
                    await self.quit.wait()

            self.loop.run_until_complete(main())
            self.loop.close()

        threading.Thread(target=run, daemon=True).start()
        started.wait(10)

    def stop(self):
        if self.httpd:
            self.httpd.shutdown()
            self.httpd.server_close()
        if self.loop:
            self.loop.call_soon_threadsafe(self.quit.set)

    def find(self, method, path):
        return [r for r in self.requests if r[0] == method and r[1] == path]


# ---------------------------------------------------------------- the client

def probe(text, label):
    m = re.search(r"\[input_script\] (?:text|vis|nodes) t=\d+ @%s (.*)" % re.escape(label), text)
    return m.group(1) if m else ""


def nodes_match(text, label):
    """The match count of a `nodes @label selector` probe (None: no such probe)."""
    m = re.search(r"match=(\d+)", probe(text, label))
    return int(m.group(1)) if m else None


def static_wording():
    """No "guest only" left in the client's krunker.io wording (C sources, the kn i18n file)."""
    hits = []
    for base in ("client/src", "client/include", "ui/i18n/kn"):
        for dirpath, _, files in os.walk(os.path.join(ROOT, base)):
            for f in files:
                if not f.endswith((".c", ".h", ".json")):
                    continue
                with open(os.path.join(dirpath, f), errors="replace") as fh:
                    for n, line in enumerate(fh, 1):
                        if re.search(r"guest[ -]only", line, re.I):
                            hits.append("%s:%d" % (os.path.relpath(os.path.join(dirpath, f), ROOT), n))
    check("wording: no \"guest only\" anywhere in the client or its kn.* strings", not hits, hits[:5])
    with open(os.path.join(ROOT, "ui/i18n/kn/en.json")) as f:
        kn = json.load(f)
    guest = {k: v for k, v in kn.items() if (k.startswith("kn.kio.") or k.startswith("kn.servers.kio") or k.startswith("kn.quickplay.serverType"))
             and "guest" in v.lower() and not k.startswith("kn.kio.account.")}
    check("wording: krunker.io server texts no longer say they are joined as a guest", not guest, guest)
    check("wording: the server type says \"krunker.io servers\"", kn.get("kn.quickplay.serverType.official") == "krunker.io servers",
          kn.get("kn.quickplay.serverType.official"))


def run():
    http_port, ws_port = kn_proto.free_ports(2)
    stub = Stub(http_port, ws_port)
    stub.start()
    base = "http://127.0.0.1:%d" % http_port
    script = "\n".join([
        "0 menu",
        '300 call setSetting "seek_maps" "%d"' % KANJI,
        # 1. the menu (krunk.io: a guest here) and the Servers window
        "800 vis @base signedOutHeaderBar signedInHeaderBar menuBtnQuickMatch",
        "850 nodes @nokiobar [id=knKioHeaderBar]",
        "900 nodes @nokiomodal [id=knKioAccountHolder]",
        "2500 call showWindow 2",
        "5000 nodes @noaccounts [id^=knAccounts]",
        "5050 text @status [id=kioStatus]",
        "5100 text @window [id=menuWindow]",
        "5150 nodes @qj .quickJoin",
        # open each server group in turn and read its first row: the krunker.io group's index is not fixed
        "5200 call setExpandedRegion 0", "5205 text @row0 [onclick*=\"krunkerJoin\"]",
        "5210 call setExpandedRegion 1", "5215 text @row1 [onclick*=\"krunkerJoin\"]",
        "5220 call setExpandedRegion 2", "5225 text @row2 [onclick*=\"krunkerJoin\"]",
        "5230 call setExpandedRegion 3", "5235 text @row3 [onclick*=\"krunkerJoin\"]",
        "5240 call setExpandedRegion 4", "5245 text @row4 [onclick*=\"krunkerJoin\"]",
        "5300 clicksel .quickJoin",
        # 2. in the game as a krunker.io guest: v10's login modal signs in to krunker.io
        "9900 state",  # (the joined game shows the menu until the player clicks in)
        "10000 vis @ingame signedOutHeaderBar signedInHeaderBar",
        "10050 nodes @nokiobar2 [id=knKioHeaderBar]",
        "10100 call loginOrRegister",
        "10600 vis @modal knV10Login accEmail accName",
        "10700 clickid knV10LoginMode",
        "10900 vis @umode accName accPass",
        "11000 clickid accName", "11100 type " + LOGIN,
        "11300 clickid accPass", "11400 type " + PASSWORD,
        "11600 clickid knV10LoginSubmit",
        "13000 vis @cap knCaptchaHolder knCaptchaManual",
        "13100 clickid knCaptchaToken", "13200 type " + TURNSTILE, "13300 clickid knCaptchaUse",
        "15400 vis @code knV10Code knV10Code0 knV10CodeSubmit",
        "15450 text @codehdr [id=knV10Code]",
        "15500 click 20 20",  # the code form's overlay: not dismissable
        "15550 vis @codestays knV10Code knV10Login",
        "15600 clickid knV10Code0", "15660 type 1",
        "15750 clickid knV10Code1", "15810 type 2",
        "15900 clickid knV10Code2", "15960 type 3",
        "16050 clickid knV10Code3", "16110 type 4",
        "16200 clickid knV10Code4", "16260 type 5",
        "16350 clickid knV10Code5", "16410 type 6",
        "18500 nodes @modalgone [id=knV10Login]",
        "18600 vis @signedin signedInHeaderBar signedOutHeaderBar",
        "18700 text @hdrname [id=knV10Name]",
        "19000 call showWindow 5",
        "19800 text @acc5 [id=knKioAccName]",
        "19850 vis @acc5v knKioLogout",
        "19900 call closWind",
        # 3. leaving the game: the menu is krunk.io's again at once; the next join carries the account
        "20000 con disconnect",
        "20200 down ESC", "20250 up ESC",  # (a disconnect leaves the game with the menu closed: Esc opens it)
        # each bar's own inline display (nodes [style*="none"]: 1 = hidden), so the menu being up does not matter
        '20800 nodes @leftout [id=signedOutHeaderBar][style*="none"]',
        '20810 nodes @leftin [id=signedInHeaderBar][style*="none"]',
        "20850 text @lefthdr [id=signedOutHeaderBar]",
        '21500 call krunkerJoin "%s"' % GAME_PICK,
        "27500 text @hdr2 [id=signedInHeaderBar]",
        "27550 vis @hdr2v signedInHeaderBar signedOutHeaderBar",
        # 4. Logout (the krunker.io account's window, in the game): a krunker.io guest, v10's login modal
        "28000 call showWindow 5",
        "28800 clickid knKioLogout",
        "29500 vis @loggedout knV10Login signedOutHeaderBar signedInHeaderBar",
        "29600 call knV10LoginClose",
        "30000 con disconnect",
        '30500 call krunkerJoin "%s"' % GAME_PICK,
        "35000 quit",
    ]) + "\n"
    script_path = os.path.join(WORK, "account.script")
    with open(script_path, "w") as f:
        f.write(script)
    cfg, state = os.path.join(WORK, "cfg"), os.path.join(WORK, "state")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": cfg, "XDG_STATE_HOME": state, "KRUNKNATIVE_NO_BROWSER": "1",
                "KRUNKNATIVE_AUDIO_BACKEND": "null", "KRUNKNATIVE_INPUT_SCRIPT": script_path, "KRUNKNATIVE_WINDOW_SIZE": "1280x720",
                "KRUNKNATIVE_KRUNKER_MATCHMAKER": base + "/mm", "KRUNKNATIVE_KRUNKER_API": base + "/api",
                "KRUNKNATIVE_KRUNKER_AUTH": base + "/frvr/v1/auth", "KRUNKNATIVE_KRUNKER_TRACE": "1",
                "KRUNKNATIVE_ASSETS_DIR": ASSETS, "KRUNKNATIVE_ASSET_FETCH": "off",
                "KRUNKNATIVE_FINDER_URL": "off"})  # no public server list in the test (its groups would shift)
    log_path = os.path.join(WORK, "account.log")
    with open(log_path, "w") as out:
        p = subprocess.Popen(["stdbuf", "-oL", "-eL", f"{BUILD}/client_bin", "--offline"], stdout=out, stderr=subprocess.STDOUT,
                             stdin=subprocess.DEVNULL, env=env, start_new_session=True, cwd=ROOT)
        try:
            p.wait(timeout=300)
        except subprocess.TimeoutExpired:
            check("client_bin finished its script", False, "timeout, log " + log_path)
        finally:
            if p.poll() is None:
                os.killpg(p.pid, signal.SIGKILL)
                p.wait()
    time.sleep(0.5)
    stub.stop()
    with open(log_path, errors="replace") as f:
        lines = f.read().splitlines()
    text = "\n".join(lines)
    client_text = "\n".join(l for l in lines if "[input_script]" not in l)  # (the script's own echo of what it typed)

    # 1. the menu: one account UI
    base = probe(text, "base")
    check("the menu: krunk.io's guest bar (v10's Login / Register), not a signed-in one",
          "signedOutHeaderBar=1" in base and "signedInHeaderBar=1" not in base, base)
    check("no krunker.io account bar and no krunker.io account modal exist", nodes_match(text, "nokiobar") == 0 and
          nodes_match(text, "nokiomodal") == 0 and nodes_match(text, "nokiobar2") == 0,
          (nodes_match(text, "nokiobar"), nodes_match(text, "nokiomodal"), nodes_match(text, "nokiobar2")))
    check("the Servers window has no Accounts line", nodes_match(text, "noaccounts") == 0, probe(text, "noaccounts"))
    window = probe(text, "window")
    check("the Servers window has no \"as a guest\" / \"guest only\" / \"krunker.io servers:\" wording",
          window and not re.search(r"as a guest|guest only|krunker\.io servers:|KrunkNative servers:", window, re.I), window[:300])
    check("no \"krunker.io: Guest\" anywhere in the log's page texts", "krunker.io: Guest" not in text, re.findall(r".{30}krunker\.io: Guest.{10}", text)[:2])
    st = probe(text, "status")
    check("the krunker.io servers line is there and plain (no account, no Login / Logout)",
          st.startswith("n=1") and not re.search(r"guest|login|logout|" + ACCOUNT_NAME, st, re.I), st)
    qj = re.search(r"elements=\d+ match=(\d+)", probe(text, "qj"))
    check("a krunker.io group has a Quick Join button (main's group header; krunker.io groups come last)", qj and int(qj.group(1)) >= 1, probe(text, "qj"))
    check("Quick Join picks the Kanji lobby with room (not the fuller Burg, not the password one or the full one)",
          ("[krunker] official lobby %s: joining in-client" % GAME_PICK) in text, re.findall(r"\[krunker\] official lobby.*", text)[:3])
    seeks = stub.find("GET", "/mm/seek-game")
    check("the first join is a krunker.io guest's (no accessToken, no at=)", seeks and "accessToken" not in seeks[0][2] and
          stub.ws_paths and "at=" not in stub.ws_paths[0], [("accessToken" in x[2]) for x in seeks][:1])
    check("joining switches the active account to krunker.io", "[account] active account backend: krunker.io" in text,
          re.findall(r"\[account\] active.*", text)[:3])

    # 2. in the game: v10's login modal signs in to krunker.io
    ingame = probe(text, "ingame")
    check("in the game the menu shows the krunker.io guest (v10's guest bar)", "signedOutHeaderBar=1" in ingame and "signedInHeaderBar=1" not in ingame, ingame)
    modal = probe(text, "modal")
    check("loginOrRegister opens v10's login modal (Email first)", "knV10Login=1" in modal and "accEmail=1" in modal, modal)
    check("\"(use username instead)\" shows the Username field and the password", "accName=1" in probe(text, "umode") and "accPass=1" in probe(text, "umode"),
          probe(text, "umode"))
    cap = probe(text, "cap")
    check("a username login asks the Security Challenge first (Turnstile, the player solves it)", "knCaptchaHolder=1" in cap, cap)
    logins = stub.find("POST", "/api/auth/login/username")
    body = logins[0][4] if logins else None
    check("POST /auth/login/username {username, password, b: the Turnstile token}", len(logins) == 1 and isinstance(body, dict) and
          body.get("username") == LOGIN and body.get("password") == PASSWORD and body.get("b") == TURNSTILE and "a" not in body,
          {k: ("..." if k != "username" else v) for k, v in (body or {}).items()} if isinstance(body, dict) else body)
    hdr = logins[0][3] if logins else {}
    check("the login POST carries JSON, Accept and an X-Idempotency-Key", hdr.get("content-type") == "application/json" and
          "application/json" in hdr.get("accept", "") and re.fullmatch(r"[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}",
                                                                       hdr.get("x-idempotency-key", "")), hdr)
    code = probe(text, "code")
    check("check_2fa: v10's code form (six boxes, Submit)", "knV10Code=1" in code and "knV10Code0=1" in code and "knV10CodeSubmit=1" in code, code)
    check("... titled \"Two-Factor Authentication\"", "Two-Factor Authentication" in probe(text, "codehdr"), probe(text, "codehdr"))
    check("... not dismissable: a click on its overlay leaves it up", "knV10Code=1" in probe(text, "codestays"), probe(text, "codestays"))
    twofa = stub.find("POST", "/api/auth/2fa/challenge/" + CHALLENGE)
    check("POST /auth/2fa/challenge/<challenge_id> {code}", len(twofa) == 1 and twofa[0][4] == {"code": CODE}, len(twofa))
    me = stub.find("GET", "/api/player/me")
    check("GET /player/me with the account's Bearer token", me and me[0][3].get("authorization") == "Bearer " + stub.access1, len(me))
    check("signed in: the login modal leaves", nodes_match(text, "modalgone") == 0, probe(text, "modalgone"))
    si = probe(text, "signedin")
    check("signed in: the header shows the krunker.io account (v10's signed-in bar)", "signedInHeaderBar=1" in si and "signedOutHeaderBar=0" in si, si)
    check("... named as /player/me says", ACCOUNT_NAME in probe(text, "hdrname"), probe(text, "hdrname"))
    check("the open game socket logs in with the account (v10's rpc LOGIN, the server's \"a\")", stub.rpc_logins >= 1 and
          "[krunker] signed in during the game" in text, (stub.rpc_logins, re.findall(r"\[krunker\] signed in.*", text)[:1]))
    check("the Account window is the krunker.io account's (its name, Logout)", ACCOUNT_NAME in probe(text, "acc5") and
          "knKioLogout=1" in probe(text, "acc5v"), (probe(text, "acc5"), probe(text, "acc5v")))

    # 3. leaving switches back; the next join carries the account
    check("con disconnect leaves the krunker.io server", "[krunker] leaving the official game" in text, re.findall(r"\[krunker\] leaving.*", text)[:1])
    check("leaving switches the active account back to krunk.io", "[account] active account backend: krunk.io" in text,
          re.findall(r"\[account\] active.*", text)[:3])
    check("after leaving, the menu is krunk.io's guest at once (v10's guest bar shown, the signed-in one hidden)",
          nodes_match(text, "leftout") == 0 and nodes_match(text, "leftin") == 1, (nodes_match(text, "leftout"), nodes_match(text, "leftin")))
    check("... with its Login, not the krunker.io account", ACCOUNT_NAME not in probe(text, "lefthdr") and "Login" in probe(text, "lefthdr"), probe(text, "lefthdr"))
    refresh = stub.find("POST", "/frvr/v1/auth/refresh")
    check("the about-to-expire session is refreshed first (FRVR POST /refresh {refreshToken})",
          len(refresh) >= 1 and refresh[0][4] == {"refreshToken": REFRESH1}, [r[4] for r in refresh][:1] and "(body hidden)")
    check("the second join seeks with accessToken = the fresh access token, for the picked lobby",
          len(seeks) >= 2 and seeks[1][2].get("accessToken") == stub.access2 and seeks[1][2].get("game") == GAME_PICK,
          [(x[2].get("game"), "accessToken" in x[2]) for x in seeks])
    check("seek-game: accessToken is matchmakerSeek's last parameter", len(seeks) >= 2 and list(seeks[1][2].keys())[-1] == "accessToken",
          list(seeks[1][2].keys()) if len(seeks) >= 2 else None)
    ws2 = stub.ws_paths[1] if len(stub.ws_paths) >= 2 else ""
    check("the game socket opens with at= the account's token", ws2 == "/ws?gameId=%s&clientKey=%s&at=%s" % (GAME_PICK, CLIENT_ID, stub.access2),
          ws2.split("&at=")[0] + ("&at=(token)" if "&at=" in ws2 else ""))
    check("the game server's \"a\" names the account and its level",
          "[krunker] the game server signed in the krunker.io account %s (level %d)" % (ACCOUNT_NAME, LEVEL) in text,
          re.findall(r"\[krunker\] the game server signed in.*", text))
    hdr2 = probe(text, "hdr2")
    check("in the game the header shows the krunker.io account with the server's level", ACCOUNT_NAME in hdr2 and "Lv %d" % LEVEL in hdr2, hdr2)
    check("... in v10's signed-in bar", "signedInHeaderBar=1" in probe(text, "hdr2v") and "signedOutHeaderBar=0" in probe(text, "hdr2v"), probe(text, "hdr2v"))

    # 4. sign out -> a krunker.io guest
    lo = probe(text, "loggedout")
    check("Logout (the Account window's): a krunker.io guest, v10's login modal", "knV10Login=1" in lo and "signedInHeaderBar=0" in lo, lo)
    check("a join after the logout carries no accessToken", len(seeks) >= 3 and "accessToken" not in seeks[-1][2],
          [("accessToken" in x[2]) for x in seeks])
    check("... and its socket no at=", len(stub.ws_paths) >= 3 and "at=" not in stub.ws_paths[-1], [p.split("&at=")[0] for p in stub.ws_paths])
    saved = []
    for root in (cfg, state):
        for dirpath, _, files in os.walk(root):
            for f in files:
                try:
                    with open(os.path.join(dirpath, f), errors="replace") as fh:
                        data = fh.read()
                except OSError:
                    continue
                if stub.access2 in data or REFRESH2 in data or stub.access1 in data or REFRESH1 in data:
                    saved.append(os.path.relpath(os.path.join(dirpath, f), WORK))
    check("Logout: the saved krunker.io session is gone", not saved, saved)

    # secrets
    leaks = [name for name, secret in (("password", PASSWORD), ("Turnstile token", TURNSTILE), ("access token", stub.access1),
                                       ("refreshed access token", stub.access2), ("refresh token", REFRESH1), ("new refresh token", REFRESH2),
                                       ("2FA code", "\"" + CODE + "\""), ("challenge id", CHALLENGE))
             if secret in client_text]
    check("no password, code, Turnstile token, access or refresh token in the log", not leaks, leaks)
    return log_path


TURNSTILE_URL = "https://challenges.cloudflare.com/turnstile/"


def turnstile_stub():
    """api.js for the web run's browser only: the widget answers at once with the token the login expects."""
    return ("(function(){var tok=%s;window.turnstile={render:function(el,o){var d=document.createElement('div');"
            "d.className='kn-e2e-turnstile';d.textContent='stub turnstile';el.appendChild(d);"
            "setTimeout(function(){o.callback(tok);},400);return 'stub-w1';},remove:function(){},reset:function(){},"
            "getResponse:function(){return tok;}};})();") % json.dumps(TURNSTILE)


class Cdp:
    """The page's DevTools socket: Runtime.evaluate, the console, and Fetch for the Turnstile script."""

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
        self.intercept = None  # (url prefix, JS body)
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        try:
            for raw in self.ws:
                m = json.loads(raw)
                if m.get("method") == "Fetch.requestPaused":
                    url = m["params"]["request"]["url"]
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

    def post(self, method, params=None):
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


class StaticHandler(SimpleHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def translate_path(self, path):  # /assets/* from the asset tree, the rest from the web dist
        p = urllib.parse.unquote(urllib.parse.urlsplit(path).path)
        if p.startswith("/assets/"):
            return os.path.join(ASSETS, os.path.normpath(p[len("/assets/"):]).lstrip("/"))
        return os.path.join(DIST, os.path.normpath(p).lstrip("/"))


def wait_until(fn, secs):
    end = time.time() + secs
    while time.time() < end:
        if fn():
            return True
        time.sleep(0.25)
    return False


def run_web():
    """The same flow in the web build (headless Chrome, the same stub): an official game as a guest,
    v10's login modal signs in to krunker.io (the Turnstile widget answers by itself, v10's code form),
    the header shows the account; out of the game (a reload) krunk.io's guest bar, the session kept:
    the next join carries the account (seek-game's accessToken, the socket's at=); after the in-game
    Logout the next join is a guest's."""
    mh, wsp, web, cdpp = kn_proto.free_ports(4)
    stub = Stub(mh, wsp)
    stub.start()
    base = "http://127.0.0.1:%d" % mh
    httpd = ThreadingHTTPServer(("127.0.0.1", web), StaticHandler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    chrome_log = open(os.path.join(WORK, "web-chrome.log"), "w")
    chrome = subprocess.Popen([CHROME, "--headless=new", "--no-sandbox", "--use-gl=angle", "--use-angle=swiftshader",
                               "--enable-unsafe-swiftshader", "--user-data-dir=" + os.path.join(WORK, "web-chrome"),
                               "--remote-debugging-port=%d" % cdpp, "--window-size=1280,720", "about:blank"],
                              stdout=chrome_log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, start_new_session=True)
    console = []
    try:
        cdp = Cdp(cdpp)
        cdp.send("Runtime.enable")
        cdp.send("Page.enable")
        cdp.intercept = (TURNSTILE_URL, turnstile_stub())
        cdp.send("Fetch.enable", {"patterns": [{"urlPattern": TURNSTILE_URL + "*"}]})

        def page(extra=""):
            q = "&".join("%s=%s" % (k, urllib.parse.quote(v, safe="")) for k, v in (
                ("krunkerApi", base + "/api"), ("krunkerAuth", base + "/frvr/v1/auth"), ("krunkerMM", base + "/mm")))
            return "http://127.0.0.1:%d/?%s%s" % (web, q, extra)

        def wait(js, timeout=90.0):
            end = time.time() + timeout
            while time.time() < end:
                v = cdp.eval(js)
                if v:
                    return v
                time.sleep(0.25)
            return None

        def shown(i):
            return "(() => { const e = document.getElementById(%s); return !!(e && e.offsetParent !== null); })()" % json.dumps(i)

        def text(i):
            return "(() => { const e = document.getElementById(%s); return e ? e.textContent : ''; })()" % json.dumps(i)

        def call(handler, args="[]"):
            """A C handler by name, as an inline onclick would call it (ui/js/kn-shim.js send())."""
            return cdp.eval("(() => { try { return window.Module.ccall('kn_ui_event', 'number', ['string', 'string', 'number', 'string',"
                            " 'number', 'number', 'number', 'number', 'number', 'number'], [%s, %s, 0, '', 0, 0, 0, 0, 0, 0]); }"
                            " catch (e) { return String(e); } })()" % (json.dumps(handler), json.dumps(args)))

        def put(i, v):
            cdp.eval("(() => { const e = document.getElementById(%s); if (!e) return false; e.value = %s;"
                     " e.dispatchEvent(new Event('input', {bubbles: true})); return true; })()" % (json.dumps(i), json.dumps(v)))

        def press(i):
            return cdp.eval("(() => { const e = document.getElementById(%s); if (!e) return false; e.click(); return true; })()" % json.dumps(i))

        def page_text():
            return cdp.eval("(() => (document.body ? document.body.innerText : ''))()") or ""

        def wait_seek(n_before, timeout=120.0):
            end = time.time() + timeout
            while time.time() < end:
                if len(stub.find("GET", "/mm/seek-game")) > n_before and stub.ws_paths:
                    return True
                time.sleep(0.25)
            return False

        def kio_wait_seek(n_seek, n_ws):
            ok = wait_seek(n_seek)
            end = time.time() + 60  # this join's socket (wait_seek returns on an older socket's path)
            while time.time() < end and len(stub.ws_paths) <= n_ws:
                time.sleep(0.25)
            return ok and len(stub.ws_paths) > n_ws

        hdr_js = ("(() => { const o = document.getElementById('signedOutHeaderBar'); const i = document.getElementById('signedInHeaderBar');"
                  " const n = document.getElementById('knV10Name'); const l = document.getElementById('knV10Level');"
                  " return JSON.stringify({out: !!o && o.style.display !== 'none', in: !!i && i.style.display !== 'none',"
                  " name: n ? n.textContent : '', level: l ? l.textContent : '', kio: !!document.getElementById('knKioHeaderBar')}); })()")

        # 1. an official game as a krunker.io guest; v10's login modal signs in to krunker.io
        cdp.send("Page.navigate", {"url": page("&krunker=" + urllib.parse.quote(GAME_PICK, safe=""))})
        check("web: the page loaded", bool(wait("(() => !!document.getElementById('uiBase'))()", 150)), "\n".join(cdp.console[-10:]))
        check("web: the guest's join seeks and opens the socket", kio_wait_seek(0, 0), "\n".join(cdp.console[-15:]))
        seeks = stub.find("GET", "/mm/seek-game")
        check("web: the guest's join carries no accessToken", bool(seeks) and "accessToken" not in seeks[0][2], [sorted(x[2]) for x in seeks])
        opened = None
        for _ in range(60):  # (the page's handlers go in on its first frames)
            call("loginOrRegister")
            opened = wait("(() => !!document.getElementById('knV10Login'))()", 2)  # (position: fixed: no offsetParent)
            if opened:
                break
        check("web: v10's login modal opens in the game (no krunker.io modal, no krunker.io bar)", bool(opened) and
              not cdp.eval("(() => !!document.getElementById('knKioAccountHolder') || !!document.getElementById('knKioHeaderBar'))()"))
        press("knV10LoginMode")
        wait(shown("accName"), 10)
        put("accName", LOGIN)
        put("accPass", PASSWORD)
        press("knV10LoginSubmit")
        end = time.time() + 90
        while time.time() < end and not stub.find("POST", "/api/auth/login/username"):
            time.sleep(0.25)
        logins = stub.find("POST", "/api/auth/login/username")
        check("web: POST /auth/login/username {username, password, b: the Turnstile token}",
              bool(logins) and logins[-1][4] and logins[-1][4].get("username") == LOGIN and logins[-1][4].get("password") == PASSWORD
              and logins[-1][4].get("b") == TURNSTILE, [r[4] and sorted(r[4]) for r in logins])
        check("web: the 2FA step is v10's code form", bool(wait(shown("knV10Code0"), 60)))
        for i, d in enumerate(CODE):
            put("knV10Code%d" % i, d)
        check("web: signed in: the modal leaves", bool(wait("(() => !document.getElementById('knV10Login'))()", 60)))
        hdr_name = wait("(() => { const n = document.getElementById('knV10Name'); return n && /KioPlayer/.test(n.textContent) ? 1 : null; })()", 30)
        h = json.loads(cdp.eval(hdr_js) or "{}")
        check("web: the header is v10's signed-in bar with the krunker.io account", bool(hdr_name) and h.get("in") and not h.get("out"), h)
        check("web: the open socket logs in with the account (rpc LOGIN)", bool(wait("(() => 1)()", 1)) and
              bool(wait_until(lambda: stub.rpc_logins >= 1, 20)), stub.rpc_logins)
        check("web: no \"guest only\" / \"krunker.io: Guest\" text in the page",
              "guest only" not in page_text().lower() and "krunker.io: guest" not in page_text().lower())
        time.sleep(1.0)
        console = list(cdp.console)

        # 2. a reload (the web build's leave): krunk.io's guest bar; the krunker.io session kept for the next join
        cdp.send("Page.navigate", {"url": page()})
        wait("(() => !!document.getElementById('uiBase'))()", 150)
        back = wait("(() => { const o = document.getElementById('signedOutHeaderBar'); return o && o.style.display !== 'none' ? 1 : null; })()", 60)
        h = json.loads(cdp.eval(hdr_js) or "{}")
        check("web: out of the game the header is krunk.io's guest bar, not the krunker.io account", bool(back) and not h.get("in"), h)
        n_seek, n_ws = len(stub.find("GET", "/mm/seek-game")), len(stub.ws_paths)
        cdp.send("Page.navigate", {"url": page("&krunker=" + urllib.parse.quote(GAME_PICK, safe=""))})
        check("web: the next join seeks and opens the socket", kio_wait_seek(n_seek, n_ws), "\n".join(cdp.console[-15:]))
        seeks = stub.find("GET", "/mm/seek-game")[n_seek:]
        check("web: seek-game carries accessToken = the account's fresh token", bool(seeks) and seeks[0][2].get("accessToken") == stub.access2,
              [sorted(x[2]) for x in seeks])
        check("web: the game socket opens with at= the account's token", stub.ws_paths[-1].endswith("at=" + stub.access2), stub.ws_paths[-1:] and "(path)")
        lv = wait("(() => { const l = document.getElementById('knV10Level'); return l && /Lv 6/.test(l.textContent) ? 1 : null; })()", 30)
        check("web: in the game the header shows the account with the server's level", bool(lv), cdp.eval(hdr_js))
        console += list(cdp.console)

        # 3. Logout in the game: a krunker.io guest; the next join carries no token
        call("logoutAcc")
        check("web: Logout: v10's login modal for the krunker.io guest", bool(wait("(() => !!document.getElementById('knV10Login'))()", 30)))
        n_seek, n_ws = len(stub.find("GET", "/mm/seek-game")), len(stub.ws_paths)
        cdp.send("Page.navigate", {"url": page("&krunker=" + urllib.parse.quote(GAME_PICK, safe=""))})
        check("web: after the logout the join seeks again", kio_wait_seek(n_seek, n_ws), "\n".join(cdp.console[-15:]))
        seeks = stub.find("GET", "/mm/seek-game")[n_seek:]
        check("web: after the logout seek-game carries no accessToken", bool(seeks) and "accessToken" not in seeks[0][2],
              [sorted(x[2]) for x in seeks])
        check("web: ... and its socket no at=", "at=" not in stub.ws_paths[-1], "(path)")
        time.sleep(1.0)
        console += list(cdp.console)
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
    stub.stop()
    text_log = "\n".join(console)
    with open(os.path.join(WORK, "web-console.log"), "w") as f:
        f.write(text_log)
    leaks = [name for name, secret in (("password", PASSWORD), ("Turnstile token", TURNSTILE), ("access token", stub.access1),
                                       ("refreshed access token", stub.access2), ("refresh token", REFRESH1), ("new refresh token", REFRESH2),
                                       ("2FA code", "\"" + CODE + "\""), ("challenge id", CHALLENGE)) if secret in text_log]
    check("web: no password, code, Turnstile token, access or refresh token in the console", not leaks, leaks)
    return os.path.join(WORK, "web-console.log")


def main():
    if websockets is None:
        print("SKIP: python websockets is not installed")
        return 77
    if not os.path.exists(f"{BUILD}/client_bin"):
        print(f"SKIP: no {BUILD}/client_bin")
        return 77
    static_wording()
    logs = []
    if ONLY != "--web":
        logs.append(run())
    if ONLY != "--native":
        if DIST and os.path.exists(f"{DIST}/client.wasm") and CHROME:
            logs.append(run_web())
        else:
            print("SKIP: the web run needs a dist/web directory (second argument) and a Chrome")
    failed = results.count(False)
    print(f"{len(results) - failed}/{len(results)} checks passed")
    if failed:
        print("logs kept in " + WORK + " (" + ", ".join(logs) + ")")
        return 1
    shutil.rmtree(WORK, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
