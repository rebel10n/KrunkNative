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

One headless client_bin (--offline, an input script) then:
  1. opens the krunker.io account modal (kioAccount()), types a username + password, Login: the Security
     Challenge modal asks (headless: the paste box), the pasted token goes with the login; the 2FA step
     asks for the code; signed in (the name from /player/me)
  2. the Servers window: the Accounts line names both backends (KrunkNative: Guest, krunker.io: the
     account), the KRUNKER.IO group has a Quick Join; with Settings > Quickplay's map set to Kanji it
     joins the fullest Kanji lobby with room and no password (not the fuller Burg one, not the full one)
  3. the join refreshes the about-to-expire session first (FRVR refresh), seeks with accessToken and
     opens the socket with at= the fresh token; the game server's "a" names the account and its level,
     shown in the Accounts line
  4. Logout (the Accounts line's): back to a guest; the next join carries no accessToken / at=
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
        return bytes([0x80 | len(v)]) + b"".join(mp_encode(str(k)) + mp_encode(x) for k, x in v.items())
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

class Stub:
    def __init__(self, http_port, ws_port):
        self.http_port, self.ws_port = http_port, ws_port
        self.requests = []      # (method, path, query dict, headers dict, body dict | None)
        self.ws_paths = []
        self.ws_frames = []
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
                    if body.get("refreshToken") == REFRESH1:
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

            async def reader():
                async for msg in ws:
                    if not isinstance(msg, str):
                        self.ws_frames.append(frame_name(msg))

            rd = asyncio.ensure_future(reader())
            try:
                await send("io-init", "sockME")
                if q.get("at") == self.access2:  # the account's socket: the server signs it in
                    await send("a", None, ACCOUNT_ID, ACCOUNT_NAME, {"player_score": SCORE, "player_kills": 12}, "legacy-token")
                await send("load")
                await asyncio.sleep(0.5)
                await send("init", KANJI, 0, 0, None, None, None, {}, None, 0, None, {"gor": True}, None, False, True, False, None, None, 0)
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
        "600 call kioAccount",
        "1400 vis @modal knKioAccountHolder knKioAccountForm knKioSignIn knKioSignOut",
        "1500 clickid knKioLogin", "1600 type " + LOGIN,
        "1700 clickid knKioPassword", "1800 type " + PASSWORD,
        "1900 clickid knKioSignIn",
        "3200 vis @cap knCaptchaHolder knCaptchaManual",
        "3300 clickid knCaptchaToken", "3400 type " + TURNSTILE, "3500 clickid knCaptchaUse",
        "5500 vis @code knKioAccountCodeRow knKioSubmitCode knKioAccountForm",
        "5600 clickid knKioCode", "5700 type " + CODE, "5800 clickid knKioSubmitCode",
        "8000 text @signed [id=knKioAccountText]",
        "8100 vis @signedvis knKioSignOut knKioAccountForm",
        "8200 clickid knKioClose",
        "8500 call showWindow 2",
        "11000 text @kn [id=knAccountsKn]",
        "11050 text @kio [id=knAccountsKio]",
        "11100 text @status [id=kioStatus]",
        "11150 text @window [id=menuWindow]",
        "11200 nodes @qj .quickJoin",
        "8900 vis @base signedOutHeaderBar knKioHeaderBar menuBtnQuickMatch",
        # open each server group in turn and read its first row: the krunker.io group's index is not fixed
        "11250 call setExpandedRegion 0", "11255 text @row0 [onclick*=\"krunkerJoin\"]",
        "11260 call setExpandedRegion 1", "11265 text @row1 [onclick*=\"krunkerJoin\"]",
        "11270 call setExpandedRegion 2", "11275 text @row2 [onclick*=\"krunkerJoin\"]",
        "11280 call setExpandedRegion 3", "11285 text @row3 [onclick*=\"krunkerJoin\"]",
        "11290 call setExpandedRegion 4", "11295 text @row4 [onclick*=\"krunkerJoin\"]",
        "11300 clicksel .quickJoin",
        "17000 call showWindow 2",
        "19500 text @kio2 [id=knAccountsKio]",
        "19550 text @hdr [id=knKioHeaderText]",
        '19560 nodes @kioshown [id=knKioHeaderBar][style*="none"]',
        '19561 nodes @knhidden [id=signedOutHeaderBar][style*="none"]',
        "19600 clickid knAccountsKioOut",
        "19650 text @kio3 [id=knAccountsKio]",
        "19700 con disconnect",
        "19750 down ESC", "19800 up ESC",  # (a disconnect leaves the game with the menu closed: Esc opens it)
        '19900 nodes @kiohidden [id=knKioHeaderBar][style*="none"]',
        '19901 nodes @knshown [id=signedOutHeaderBar][style*="none"]',
        "19950 text @knhdr [id=signedOutHeaderBar]",
        "19960 vis @menuhdr menuBtnQuickMatch playerHeaderEl",
        '21000 nodes @kiohidden2 [id=knKioHeaderBar][style*="none"]',
        '21001 nodes @knshown2 [id=signedOutHeaderBar][style*="none"]',
        "21050 text @knhdr2 [id=signedOutHeaderBar]",
        '25000 call krunkerJoin "%s"' % GAME_PICK,
        "29000 quit",
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
            p.wait(timeout=240)
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

    # 1. sign in
    modal = probe(text, "modal")
    check("the krunker.io account modal opens with the login form", "knKioAccountHolder=1" in modal and "knKioAccountForm=1" in modal and
          "knKioSignIn=1" in modal and "knKioSignOut=0" in modal, modal)
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
    check("check_2fa: the modal asks for the code", "knKioAccountCodeRow=1" in code and "knKioSubmitCode=1" in code and "knKioAccountForm=0" in code, code)
    twofa = stub.find("POST", "/api/auth/2fa/challenge/" + CHALLENGE)
    check("POST /auth/2fa/challenge/<challenge_id> {code}", len(twofa) == 1 and twofa[0][4] == {"code": CODE}, len(twofa))
    me = stub.find("GET", "/api/player/me")
    check("GET /player/me with the account's Bearer token", me and me[0][3].get("authorization") == "Bearer " + stub.access1, len(me))
    signed = probe(text, "signed")
    check("the modal says who is signed in (the name /player/me gave)", "Signed in as %s" % ACCOUNT_NAME in signed, signed)
    sv = probe(text, "signedvis")
    check("signed in: Logout instead of the form", "knKioSignOut=1" in sv and "knKioAccountForm=0" in sv, sv)

    # 2. the Servers window's krunker.io servers line (the account they see) + Quick Join
    kn, kio = probe(text, "kn"), probe(text, "kio")
    check("the Servers window has no KrunkNative accounts row (the menu header shows that account, as v10's does)", kn.startswith("n=0 "), kn)
    check("the krunker.io servers line shows the krunker.io account with Logout", ACCOUNT_NAME in kio and "Logout" in kio, kio)
    window = probe(text, "window")
    check("the Servers window has no \"as a guest\" / \"guest only\" wording", window and not re.search(r"as a guest|guest only", window, re.I),
          re.findall(r".{40}as a guest.{20}", window)[:2])
    st = probe(text, "status")
    check("the KRUNKER.IO section speaks of krunker.io servers", "krunker.io servers" in st and "guest" not in st.lower(), st)
    qj = re.search(r"elements=\d+ match=(\d+)", probe(text, "qj"))
    check("a krunker.io group has a Quick Join button (main's group header; krunker.io groups come last)", qj and int(qj.group(1)) >= 1, probe(text, "qj"))
    check("Quick Join picks the Kanji lobby with room (not the fuller Burg, not the password one or the full one)",
          ("[krunker] official lobby %s: joining in-client" % GAME_PICK) in text, re.findall(r"\[krunker\] official lobby.*", text)[:3])

    # 3. the join: refresh, accessToken, at=, "a"
    refresh = stub.find("POST", "/frvr/v1/auth/refresh")
    check("the about-to-expire session is refreshed first (FRVR POST /refresh {refreshToken})",
          len(refresh) >= 1 and refresh[0][4] == {"refreshToken": REFRESH1}, [r[4] for r in refresh][:1] and "(body hidden)")
    seeks = stub.find("GET", "/mm/seek-game")
    check("seek-game carries accessToken = the refreshed access token, for the picked lobby",
          seeks and seeks[0][2].get("accessToken") == stub.access2 and seeks[0][2].get("game") == GAME_PICK,
          [(s[2].get("game"), "accessToken" in s[2]) for s in seeks])
    check("seek-game: accessToken is matchmakerSeek's last parameter", seeks and list(seeks[0][2].keys())[-1] == "accessToken",
          list(seeks[0][2].keys()) if seeks else None)
    ws1 = stub.ws_paths[0] if stub.ws_paths else ""
    check("the game socket opens with at= the account's token", ws1 == "/ws?gameId=%s&clientKey=%s&at=%s" % (GAME_PICK, CLIENT_ID, stub.access2),
          ws1.split("&at=")[0] + ("&at=(token)" if "&at=" in ws1 else ""))
    check("the game server's \"a\" names the account and its level",
          "[kio-account] the krunker.io game server names the account %s (level %d)" % (ACCOUNT_NAME, LEVEL) in text or
          "[krunker] the game server signed in the krunker.io account %s (level %d)" % (ACCOUNT_NAME, LEVEL) in text,
          re.findall(r"\[krunker\] the game server signed in.*", text))
    kio2 = probe(text, "kio2")
    check("the Accounts line shows the account's level from the game server", ACCOUNT_NAME in kio2 and "Level %d" % LEVEL in kio2, kio2)

    # 3b. the header box: in a krunker.io server it shows the krunker.io account alone; leaving brings
    # the KrunkNative account's bar back (the KrunkNative account is a guest in this test)
    hdr = probe(text, "hdr")
    check("in a krunker.io server the header shows the krunker.io account (name and level)", ACCOUNT_NAME in hdr and "Lv %d" % LEVEL in hdr, hdr)
    # each bar's own inline display (nodes [style*="none"]: 1 = hidden), so the menu being up does not matter
    check("in a krunker.io server the krunker.io bar shows and the KrunkNative bar is hidden",
          nodes_match(text, "kioshown") == 0 and nodes_match(text, "knhidden") == 1,
          (nodes_match(text, "kioshown"), nodes_match(text, "knhidden")))
    check("con disconnect leaves the krunker.io server", "[krunker] leaving the official game" in text, re.findall(r"\[krunker\] leaving.*", text)[:1])
    base = probe(text, "base")
    check("baseline: before any join the KrunkNative guest's bar shows (the menu is up)", "signedOutHeaderBar=1" in base, base)
    check("leaving the krunker.io server restores the KrunkNative bar (its guest's Login)",
          nodes_match(text, "kiohidden") == 1 and nodes_match(text, "knshown") == 0, (nodes_match(text, "kiohidden"), nodes_match(text, "knshown")))
    check("... and still after a few seconds (no krunker.io join in between)",
          nodes_match(text, "kiohidden2") == 1 and nodes_match(text, "knshown2") == 0,
          (nodes_match(text, "kiohidden2"), nodes_match(text, "knshown2")))
    knhdr = probe(text, "knhdr2")
    check("... with the KrunkNative guest's Login or Register", "Login" in knhdr and ACCOUNT_NAME not in knhdr, knhdr)

    # 4. sign out -> a guest again
    kio3 = probe(text, "kio3")
    check("Logout: krunker.io servers are joined as a guest again", "Guest" in kio3 and "Login" in kio3 and ACCOUNT_NAME not in kio3, kio3)
    check("a join after the logout carries no accessToken", len(seeks) >= 2 and "accessToken" not in seeks[-1][2],
          [("accessToken" in s[2]) for s in seeks])
    check("... and its socket no at=", len(stub.ws_paths) >= 2 and "at=" not in stub.ws_paths[-1], [p.split("&at=")[0] for p in stub.ws_paths])
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


def run_web():
    """The same flow in the web build (headless Chrome, the same stub): sign in (the Turnstile widget
    answers by itself), the 2FA code, the account kept in the page's storage across a reload; a
    krunker.io join carries the account (seek-game's accessToken, the socket's at=) and the game
    server's name shows; after the sign-out the next join is a guest's."""
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

        # 1. sign in in the page
        cdp.send("Page.navigate", {"url": page()})
        check("web: the page loaded with the account's handler", bool(wait("(() => !!document.getElementById('uiBase'))()", 150)),
              "\n".join(cdp.console[-10:]))
        # the page's handlers go in on the first frames after it is ready (krunker_account_ui_tick): retry the open
        opened = None
        for _ in range(60):
            call("kioAccount")
            opened = wait(shown("knKioAccountForm"), 2)
            if opened:
                break
        check("web: the krunker.io account modal opens with the login form", bool(opened),
              cdp.eval("(() => { const h = document.getElementById('knKioAccountHolder'); return h ? h.outerHTML.slice(0, 200) : 'no modal'; })()"))
        put("knKioLogin", LOGIN)
        put("knKioPassword", PASSWORD)
        press("knKioSignIn")
        end = time.time() + 90
        while time.time() < end and not stub.find("POST", "/api/auth/login/username"):
            time.sleep(0.25)
        logins = stub.find("POST", "/api/auth/login/username")
        check("web: POST /auth/login/username {username, password, b: the Turnstile token}",
              bool(logins) and logins[-1][4] and logins[-1][4].get("username") == LOGIN and logins[-1][4].get("password") == PASSWORD
              and logins[-1][4].get("b") == TURNSTILE, [r[4] and sorted(r[4]) for r in logins])
        check("web: the 2FA step asks for the code", bool(wait(shown("knKioAccountCodeRow"), 60)))
        put("knKioCode", CODE)
        press("knKioSubmitCode")
        check("web: signed in: the modal says who (the name from /player/me)", bool(wait(shown("knKioSignOut"), 60)) and
              "KioPlayer" in (cdp.eval(text("knKioAccountText")) or ""), cdp.eval(text("knKioAccountText")))
        press("knKioClose")
        check("web: no \"guest only\" text in the page", "guest only" not in page_text().lower())

        # 2. a krunker.io join: the account's token (refreshed first) on seek-game and on the socket
        n_seek = len(stub.find("GET", "/mm/seek-game"))
        cdp.send("Page.navigate", {"url": page("&krunker=" + urllib.parse.quote(GAME_PICK, safe=""))})
        check("web: the join seeks and opens the socket", wait_seek(n_seek), "\n".join(cdp.console[-15:]))
        seeks = stub.find("GET", "/mm/seek-game")[n_seek:]
        check("web: the about-to-expire session was refreshed first (FRVR POST /refresh)",
              bool(stub.find("POST", "/frvr/v1/auth/refresh")), stub.requests[-3:] and [r[1] for r in stub.requests[-3:]])
        check("web: seek-game carries accessToken = the refreshed access token",
              bool(seeks) and seeks[0][2].get("accessToken") == stub.access2, [sorted(s[2]) for s in seeks])
        check("web: the game socket opens with at= the account's token",
              any(p.endswith("at=" + stub.access2) for p in stub.ws_paths), stub.ws_paths[-2:])
        check("web: the game server's \"a\" names the account and its level", bool(wait(
            "(() => { const c = document.body ? document.body.innerText : ''; return /KioPlayer/.test(c) || null; })()", 10)) or
              any("KioPlayer (level 6)" in l for l in cdp.console), "\n".join(cdp.console[-8:]))
        # the header box (the krunker.io account alone in the server; each bar's own inline display)
        hdr_js = ("(() => { const k = document.getElementById('knKioHeaderBar'); const t = document.getElementById('knKioHeaderText');"
                  " const o = document.getElementById('signedOutHeaderBar'); const i = document.getElementById('signedInHeaderBar');"
                  " return JSON.stringify({kio: !!k && k.style.display !== 'none', text: t ? t.textContent : '',"
                  " knOut: !!o && o.style.display === 'none', knIn: !!i && i.style.display !== 'none'}); })()")
        hdr_lv = wait("(() => { const t = document.getElementById('knKioHeaderText'); return t && /Lv 6/.test(t.textContent) ? 1 : null; })()", 20)
        h = json.loads(cdp.eval(hdr_js) or "{}")
        check("web: in a krunker.io server the header shows the krunker.io account (name and level), the KrunkNative bars hidden",
              bool(hdr_lv) and h.get("kio") and h.get("knOut") and not h.get("knIn"), h)
        time.sleep(1.0)
        console = list(cdp.console)

        # 3. the session is kept across a reload; sign out; the next join is a guest's
        cdp.send("Page.navigate", {"url": page()})
        check("web: the signed-in session is restored after a reload (its storage)", bool(wait(
            "(() => { const e = document.getElementById('uiBase'); return !!e; })()", 150)), "")
        # the reloaded page's handlers go in on its first frames (as at step 1): retry the open
        reopened = None
        for _ in range(60):
            call("kioAccount")
            reopened = wait(shown("knKioSignOut"), 2)
            if reopened:
                break
        check("web: ... and the krunker.io account modal says it is signed in (the name from the storage)",
              bool(reopened) and "KioPlayer" in (cdp.eval(text("knKioAccountText")) or ""), cdp.eval(text("knKioAccountText")))
        # the reload is the web build's leave of the server: the KrunkNative bar is back, the krunker.io bar gone
        h = json.loads(cdp.eval(hdr_js) or "{}")
        check("web: after leaving the server the KrunkNative bar is back and the krunker.io bar is hidden",
              not h.get("kio") and not h.get("knOut") and "KioPlayer" not in h.get("text", ""), h)
        press("knKioSignOut")
        check("web: Logout: the modal shows the login form again", bool(wait(shown("knKioAccountForm"), 30)))
        press("knKioClose")
        n_seek = len(stub.find("GET", "/mm/seek-game"))
        n_ws = len(stub.ws_paths)
        cdp.send("Page.navigate", {"url": page("&krunker=" + urllib.parse.quote(GAME_PICK, safe=""))})
        check("web: after the logout the join seeks again", wait_seek(n_seek), "\n".join(cdp.console[-15:]))
        seeks = stub.find("GET", "/mm/seek-game")[n_seek:]
        check("web: after the logout seek-game carries no accessToken", bool(seeks) and "accessToken" not in seeks[0][2],
              [sorted(s[2]) for s in seeks])
        end = time.time() + 60  # the socket of this join: wait_seek returns on an older socket's path
        while time.time() < end and len(stub.ws_paths) <= n_ws:
            time.sleep(0.25)
        check("web: ... and its socket no at=", len(stub.ws_paths) > n_ws and "at=" not in stub.ws_paths[-1], stub.ws_paths[n_ws:])
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
