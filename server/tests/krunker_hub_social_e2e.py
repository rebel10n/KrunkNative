#!/usr/bin/env python3
"""The Krunker Hub page (social.html) of an official krunker.io game talks to krunker.io's own social socket, end to end
against a stub of social.krunker.io.

  python3 -I server/tests/krunker_hub_social_e2e.py BUILD_DIR      (run from the repository root)

The hub page in krunker.io mode (client/src/ui/social_ui.c while krunker.io is the active account backend) opens the
socket the hub bundle opens (hub-10.0.7.js:7020-7110, client/include/krunker_social.h): it fetches SOCIAL_URI/ws/ping
for the pre-auth token, connects to SOCIAL_URI/ws?tc=&at=&pat= with the account's fresh FRVR token, speaks the game
socket's framing (msgpack + the ahNum trailer, AHKEY 0x43e1, cptR exempt), answers the ALTCHA "cpt" with "cptR", asks
for its view with "r", draws the server's frames ("a", "0", "news") and sends PAT_REFRESH ("patR" + a fresh /ws/ping
token) every 20 s. A socket the server closes shows DISCONNECTED. Nothing goes to kn_brain.

The stub (one websockets server: /ws/ping over plain HTTP, /ws the socket) stands in for social.krunker.io and a small
HTTP stub for FRVR's auth refresh. client_bin runs as a hub window of a router in an official game: --hub with
KRUNKNATIVE_APP_BACKEND=krunker and a router pipe pair (KRUNKNATIVE_APP_IPC, app_window.c), signed in from a stored
krunker.io session (kio_refresh_token). KRUNKNATIVE_BRAIN_URL points at a recorder that must see no request.

The LOGIN rpc (an account that signs in while the page is open) is not driven here: a krunker.io sign-in from the hub
page needs the login card's Security Challenge (krunker_account_e2e.py covers that card).
Exit 0 when every check passed, 77 when it cannot run.
"""
import asyncio
import base64
import hashlib
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, os.path.join(os.getcwd(), "server/tests"))
import kn_proto  # noqa: E402
import krunker_account_e2e as kae  # noqa: E402  (msgpack helpers and the JWT; its own WORK dir is removed below)

shutil.rmtree(kae.WORK, ignore_errors=True)
BUILD = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug")
ROOT = os.getcwd()
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-hub-social-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)

ACCOUNT_ID, ACCOUNT_NAME = 52011, "KioHubber"
FRIEND = "KioHubFriend"
REFRESH1, REFRESH2 = "rt-hub-one-" + "h" * 40, "rt-hub-two-" + "u" * 40
ACCESS = kae.jwt({"exp": int(time.time()) + 3600, "sub": str(ACCOUNT_ID)})
TEST_CODE = "tc-hub-e2e"
SALT = "hubsalt?expires=%d" % (int(time.time()) + 600)
NUMBER = 31337
CHALLENGE = hashlib.sha256((SALT + str(NUMBER)).encode()).hexdigest()
NEWS = "Hub news from the krunker.io stub"
PROFILE = {"player_id": 9101, "player_name": FRIEND, "player_clan": "KHUB", "player_score": 250000, "player_kills": 777,
           "player_deaths": 100, "player_wins": 12, "player_games_played": 30, "player_funds": 10, "player_stats": {"c": 1},
           "player_following": 3, "player_followed": 4, "player_datenew": "2022-02-02 10:00:00", "maps": [], "mods": [],
           "assets": []}
results = []
procs = []


def check(name, ok, detail=""):
    results.append(bool(ok))
    detail = str(detail)
    show = detail and (not ok or len(detail) <= 160)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail[:400]) if show else ''}", flush=True)


def read_log(path):
    try:
        with open(path, errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def nodes(text, label):
    m = re.search(r"\[input_script\] nodes t=\d+ @%s elements=\d+ match=(\d+)" % re.escape(label), text)
    return int(m.group(1)) if m else -1


def text_of(text, label):
    m = re.search(r"\[input_script\] text t=\d+ @%s n=(\d+) .*?text=(.*)" % re.escape(label), text)
    return m.group(2).strip() if m else ""


class Http(ThreadingHTTPServer):
    """FRVR's auth (the stored session's refresh) and the kn_brain recorder: one handler, two ports."""

    def __init__(self, port, kind, log):
        self.kind, self.log = kind, log

        class H(BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def reply(self, status, obj):
                data = json.dumps(obj).encode()
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            def handle_any(self, method):
                n = int(self.headers.get("Content-Length") or 0)
                body = self.rfile.read(n) if n else b""
                srv = self.server
                srv.log.append((method, self.path))
                if srv.kind == "brain":
                    self.reply(404, {"error": "not here"})
                    return
                try:
                    obj = json.loads(body or b"{}")
                except ValueError:
                    obj = {}
                if method == "POST" and self.path == "/frvr/v1/auth/refresh" and obj.get("refreshToken") in (REFRESH1, REFRESH2):
                    self.reply(201, {"accessToken": ACCESS, "refreshToken": REFRESH2})
                else:
                    self.reply(404, {"error": "not found"})

            def do_GET(self):
                self.handle_any("GET")

            def do_POST(self):
                self.handle_any("POST")

        super().__init__(("127.0.0.1", port), H)
        threading.Thread(target=self.serve_forever, daemon=True).start()


class Social:
    """social.krunker.io: GET /ws/ping {token}, the /ws socket in the game socket's framing."""

    def __init__(self, port):
        self.port = port
        self.pings = []        # the /ws/ping fetches (time)
        self.paths = []        # the socket's request paths
        self.frames = []       # the client's frames: (time, [name, args...], trailer ok)
        self.cpt_answer = None  # the decoded cptR payload
        self.loop = None
        self.open_at = None

    def start(self):
        social = self
        started = threading.Event()

        def process_request(connection, request):
            path = urllib.parse.urlsplit(request.path).path
            if path == "/ws/ping":
                social.pings.append(time.time())
                resp = connection.respond(HTTPStatus.OK, json.dumps({"token": "pat-%d" % len(social.pings)}) + "\n")
                resp.headers["Content-Type"] = "application/json"
                return resp
            if path != "/ws":
                return connection.respond(HTTPStatus.NOT_FOUND, "not found\n")
            return None

        async def handler(ws):
            self.paths.append(ws.request.path)
            self.open_at = time.time()
            ah = [0]
            got_cpt = asyncio.Event()
            got_pat = asyncio.Event()

            async def send(name, *args):
                await ws.send(kae.frame(name, *args, ah=0x5))

            async def reader():
                async for msg in ws:
                    if isinstance(msg, str):
                        continue
                    f = kae.frame_args(msg)
                    name = f[0] if f else "?"
                    if name != "cptR":  # ahNum = (ahNum + 0x43e1) & 0xff before every frame but cptR
                        ah[0] = (ah[0] + 0x43e1) & 0xff
                    ok = msg[-2] == ah[0] >> 4 and msg[-1] == ah[0] & 0xf
                    self.frames.append((time.time(), f, ok))
                    if name == "cptR":
                        try:
                            self.cpt_answer = json.loads(base64.b64decode(f[1]))
                        except (ValueError, IndexError, TypeError):
                            self.cpt_answer = {"bad": repr(f)[:80]}
                        got_cpt.set()
                    elif name == "r" and len(f) > 2 and f[1] == "profile":
                        await send("0", "profile", f[2], PROFILE)
                    elif name == "patR":
                        got_pat.set()

            rd = asyncio.ensure_future(reader())
            try:
                # the security challenge first; the modal stays up until our cptR, 3 s after the answer
                await send("cpt", {"algorithm": "SHA-256", "challenge": CHALLENGE, "maxnumber": 100000, "salt": SALT, "signature": "s1g"})
                await asyncio.wait_for(got_cpt.wait(), 30)
                await asyncio.sleep(3)
                await send("cptR")
                await send("io-init", 4)
                q = dict(urllib.parse.parse_qsl(urllib.parse.urlsplit(ws.request.path).query))
                if q.get("at") == ACCESS:  # the account's socket: the server signs it in
                    await send("a", None, ACCOUNT_ID, ACCOUNT_NAME, {"player_funds": 4321, "player_score": 90000, "stats": {"c": 2}}, None)
                await send("news", {"data": [NEWS]})
                try:
                    await asyncio.wait_for(got_pat.wait(), 40)
                except asyncio.TimeoutError:
                    pass
                await asyncio.sleep(1)
                await ws.close(1000, "stub done")
                await rd
            except (kae.websockets.ConnectionClosed, asyncio.CancelledError, asyncio.TimeoutError):
                pass

        def run():
            self.loop = asyncio.new_event_loop()
            asyncio.set_event_loop(self.loop)

            async def main():
                self.quit = asyncio.Event()
                async with kae.ws_server.serve(handler, "127.0.0.1", self.port, process_request=process_request, max_size=None):
                    started.set()
                    await self.quit.wait()

            self.loop.run_until_complete(main())
            self.loop.close()

        threading.Thread(target=run, daemon=True).start()
        started.wait(10)

    def stop(self):
        if self.loop:
            self.loop.call_soon_threadsafe(self.quit.set)


def script():
    s = []
    for i, t in enumerate((1500, 2500, 3500, 4500)):  # the Security Challenge modal while the challenge is open
        s.append(f"{t} nodes @cap{i} [id=captcha-holder]")
    s += ["9000 nodes @capgone [id=captcha-holder]",
          "9100 text @header [id=profileName]",
          "9200 text @profile [id=profileHolder]",
          "9300 text @news [id=leftABox]",
          "9400 text @errbefore [id=errorDisplay]",
          "29500 text @errtitle [id=errorTitle]",
          "29600 nodes @errshown [id=errorDisplay]",
          "30000 quit"]
    return "\n".join(s) + "\n"


def run_client(social, auth, brain):
    path = os.path.join(WORK, "client.script")
    with open(path, "w") as f:
        f.write(script())
    cfg = os.path.join(WORK, "cfg")
    os.makedirs(os.path.join(cfg, "krunknative"), exist_ok=True)
    # a stored krunker.io session (as kio_pfp_e2e.py) and the page's saved testCode (saveVal("testCode"))
    for key, value in (("kio_refresh_token", REFRESH1), ("kio_username", ACCOUNT_NAME), ("testCode", TEST_CODE)):
        with open(os.path.join(cfg, "krunknative", key + ".json"), "w") as f:
            f.write(value)
    log = os.path.join(WORK, "client.log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
    to_child_r, to_child_w = os.pipe()
    from_child_r, from_child_w = os.pipe()
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": cfg,
                "XDG_STATE_HOME": os.path.join(WORK, "state"), "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_UPDATES_URL": "off",
                "KRUNKNATIVE_INPUT_SCRIPT": path, "KRUNKNATIVE_WINDOW_SIZE": "1280x720",
                "KRUNKNATIVE_BRAIN_URL": "http://127.0.0.1:%d" % brain.server_port, "KRUNKNATIVE_FINDER_URL": "off",
                # a hub window of a router in an official krunker.io game (app_window.c)
                "KRUNKNATIVE_APP_BACKEND": "krunker", "KRUNKNATIVE_APP_IPC": "%d,%d" % (to_child_r, from_child_w),
                "KRUNKNATIVE_KRUNKER_SOCIAL": "http://127.0.0.1:%d" % social.port,
                "KRUNKNATIVE_KRUNKER_AUTH": "http://127.0.0.1:%d/frvr/v1/auth" % auth.server_port,
                "KRUNKNATIVE_KRUNKER_API": "http://127.0.0.1:%d/api" % auth.server_port,
                "KRUNKNATIVE_KRUNKER_MATCHMAKER": "http://127.0.0.1:9/mm",
                "KRUNKNATIVE_KRUNKER_EVENT_BUS": "ws://127.0.0.1:9", "KRUNKNATIVE_KRUNKER_EDITOR": "http://127.0.0.1:9",
                "KRUNKNATIVE_ASSETS_DIR": kae.ASSETS, "KRUNKNATIVE_ASSET_FETCH": "off"})
    p = subprocess.Popen(["stdbuf", "-oL", "-eL", f"{BUILD}/client_bin", "--hub", "p=profile&q=" + FRIEND], cwd=ROOT,
                         stdout=open(log, "w"), stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env,
                         start_new_session=True, pass_fds=(to_child_r, from_child_w))
    procs.append(p)
    os.close(to_child_r)
    os.close(from_child_w)

    def drain():  # the router's side: read what the window says, say nothing
        while os.read(from_child_r, 4096):
            pass

    threading.Thread(target=drain, daemon=True).start()
    try:
        p.wait(120)
    except subprocess.TimeoutExpired:
        os.killpg(p.pid, signal.SIGKILL)
        p.wait()
        check("client finished its script", False, "timed out; log " + log)
    os.close(to_child_w)
    return read_log(log)


def checks(text, social, brain):
    check("the hub window runs in krunker.io mode", "[account] active account backend: krunker.io" in text or social.paths,
          re.findall(r"\[account\] active.*", text)[:2])
    check("GET SOCIAL_URI/ws/ping before the socket (the pre-auth token)", social.pings and social.open_at and social.pings[0] < social.open_at,
          (social.pings[:1], social.open_at))
    want = "/ws?tc=%s&at=%s&pat=pat-1" % (TEST_CODE, ACCESS)
    check("the socket URL is SOCIAL_URI/ws?tc=&at=<fresh token>&pat=<ping token>", social.paths[:1] == [want],
          [p[:60] for p in social.paths])
    check("one socket only", len(social.paths) == 1, len(social.paths))
    names = [f[1][0] for f in social.frames]
    check("every frame has the right ahNum trailer (+0x43e1 per frame, cptR exempt)", social.frames and all(f[2] for f in social.frames),
          [(f[1][0], f[2]) for f in social.frames][:8])
    ans = social.cpt_answer or {}
    check("cpt answered with cptR: the widget's payload with the number", ans.get("number") == NUMBER and ans.get("challenge") == CHALLENGE
          and ans.get("salt") == SALT and ans.get("signature") == "s1g" and ans.get("algorithm") == "SHA-256", ans)
    # onopen runs the connect callback at once (deob-10.0.7.js:98829-98832): the view request goes before the
    # challenge's answer, which waits for the proof of work
    check("\"r\" goes at the open, cptR after it", names[:2] == ["r", "cptR"], names[:4])
    shown = [nodes(text, "cap%d" % i) for i in range(4)]
    check("the Security Challenge modal shows while the challenge is open", 1 in shown, shown)
    check("... and goes with the server's cptR", nodes(text, "capgone") == 0, nodes(text, "capgone"))
    rs = [f[1] for f in social.frames if f[1][0] == "r"]
    check('the view request "r" [page, query, ...] for ?p=profile&q=<name>', rs and rs[0][1:3] == ["profile", FRIEND], rs[:2])
    header = text_of(text, "header")
    check('the server\'s "a" signs the page in (the account\'s name and KR in the header)', ACCOUNT_NAME in header and "4,321" in header, header)
    prof = text_of(text, "profile")
    check('the "0" profile reply draws the profile', FRIEND in prof and "KHUB" in prof, prof[:200])
    news = text_of(text, "news")
    check('the "news" push fills the Krunker News box', NEWS in news, news[:160])
    pats = [f for f in social.frames if f[1][0] == "patR"]
    check("PAT_REFRESH: patR with a fresh /ws/ping token, about 20 s after the open",
          pats and pats[0][1][1:] == ["pat-2"] and 18 <= pats[0][0] - social.open_at <= 26 and len(social.pings) >= 2,
          [(round(f[0] - social.open_at, 1), f[1]) for f in pats][:2])
    check("no LOGIN rpc (the socket was signed in from its at=)", "_0" not in names, names)
    check("no DISCONNECTED while the socket is up", "DISCONNECTED" not in text_of(text, "errbefore"), text_of(text, "errbefore"))
    check("the server's close shows DISCONNECTED", text_of(text, "errtitle") == "DISCONNECTED", text_of(text, "errtitle"))
    check("nothing went to kn_brain", not brain.log, brain.log[:4])
    leaked = ACCESS in text or REFRESH1 in text or REFRESH2 in text
    check("no token in the client log", not leaked)


def main():
    if kae.websockets is None:
        print("SKIP: python websockets is not installed")
        return 77
    if not os.access(f"{BUILD}/client_bin", os.X_OK):
        print("krunker_hub_social_e2e: no client_bin in %s: skipped" % BUILD)
        return 77
    social_port, auth_port, brain_port = kn_proto.free_ports(3)
    social = Social(social_port)
    auth = Http(auth_port, "auth", [])
    brain = Http(brain_port, "brain", [])
    try:
        social.start()
        text = run_client(social, auth, brain)
        checks(text, social, brain)
    finally:
        social.stop()
        for srv in (auth, brain):
            srv.shutdown()
            srv.server_close()
        for p in procs:
            if p.poll() is None:
                os.killpg(p.pid, signal.SIGTERM)
                try:
                    p.wait(10)
                except subprocess.TimeoutExpired:
                    os.killpg(p.pid, signal.SIGKILL)
                    p.wait()
    print(f"{sum(results)}/{len(results)} passed")
    if all(results):
        shutil.rmtree(WORK, ignore_errors=True)
        return 0
    print("logs kept in " + WORK)
    return 1


if __name__ == "__main__":
    sys.exit(main())
