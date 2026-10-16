#!/usr/bin/env python3
"""The Ranked popup on a krunker.io account (krunker_ranked.h) end to end, against a local fake of
krunker.io's services, and the routing rule (ranked_route.h) on a real kn_brain.

  python3 -I server/tests/ranked_krunker_e2e.py BUILD_DIR      (run from the repository root)

Nothing leaves the machine. One fake HTTP server serves krunker.io's api (/api: profile, leaderboard,
match history, rejoin lookup, claim) and gapi (/gapi: shop, purchase, the wallet's /player/me). A fake
gamefrontend WebSocket serves /v1/matchmaking/queue (QUEUED; a MATCHED for one account; an ERROR
COOLDOWN for another). The fake records every request: method, path, the Bearer account (by the token
it was issued to; the tokens are fake JWTs, never printed), POST bodies, and the queue's query.
A fake game socket (the MATCHED assignment's connection) records the path and query of each
upgrade and the packets the client sends on it; it sends io-init and "a" (the account row, its
rankedPoints at index 47) and closes after a few seconds.

Four headless client_bin runs (input scripts on the v10 page), one after another:
  1. kio_match:    a krunker.io account only (no KrunkNative account; the same brain and node as the
                   others, whose startup seek the popup is not part of): the Home / Profile / Leaderboard /
                   Match History / Shop tabs read v10's paths with Bearer; the claim and a purchase POST to
                   v10's paths; FIND MATCH queues on the WebSocket with the token and maps=0,2,4,11,14,17,42;
                   QUEUED, then MATCHED shows "Match found!" and joins the match's game socket at its
                   connection with at= the token (v10's join); the RP balance is "--" until the game
                   server's account row arrives, then row[47]; no request to /player/me.
  2. kio_cooldown: the queue answers ERROR COOLDOWN: the cooldown overlay with the time left.
  3. kn_only:      a KrunkNative account only (kn_brain on a scratch database, a server_bin node for the
                   startup seek to join): the popup reads the brain's /mm/ranked/* routes; no krunker.io
                   request is made, no queue socket opens.
  4. both:         both accounts signed in: krunker.io wins (its queue socket opens for the krunker.io
                   account, and its requests carry that account's token).

Needs a headless GL device (KRUNKNATIVE_HEADLESS=1), python websockets, and the scratch database helper
(services/brain/tests/db_scratch.py). Exit 77 when a prerequisite is missing, 0 when every check passed.
"""
import asyncio
import base64
import json
import os
import re
import shutil
import signal
import subprocess
import struct
import sys
import tempfile
import threading
import time
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, os.path.join(os.getcwd(), "server/tests"))
import kn_proto  # noqa: E402

try:
    import websockets
    import websockets.asyncio.server as ws_server
except ImportError:  # pragma: no cover
    print("ranked_krunker_e2e: python websockets missing: skipped")
    sys.exit(77)

BUILD = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug")
ROOT = os.getcwd()
DRIVER = os.environ.get("KN_TEST_DB_DRIVER", "sqlite")
NAME = "kn_test_rkrunker_%d" % os.getpid()
SECRET = "kn-rkrunker-%d" % os.getpid()
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-rkrunker-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
HTTP_PORT, WS_PORT, GAME_PORT, MATCH_PORT, DEAD_PORT = kn_proto.free_ports(5)  # DEAD_PORT: nothing listens on it
MAPS = "0,2,4,11,14,17,42"  # v10's preferredMaps (v10:98372)
MODES = {"kio_match": "match", "kio_cooldown": "cooldown", "kio_both": "match", "kio_garbage": "garbage"}
ACCOUNT_ID = 1234
RP_ROW_INDEX = 47  # v10's Account.setData: row[47] = rankedPoints (v10:112873)
RP_TEST = 4242     # the fake account row's rankedPoints
SCORE_INDEX = 6    # v10's Account.setData: score = row[6] (v10:112832); the level is max(1, floor(0.03 * sqrt(score)))
SCORE_TEST = 450000  # level 20 (0.03 * sqrt(450000) = 20.1)
MILESTONE_REWARD = 10   # the fake profile's first milestone (check_kio_match's claim)
WALLET_TEST = RP_TEST + 100  # the balance a wallet packet ("ub", balances[3].amount) sets after the account row
SHOP_EXPIRY_MS = 2000        # the first shop answer's item expires this long after it is sent (v10's onExpired)
SKIN_PRICE = 300        # the fake shop's skin 7 price
results = []
procs = []
LOG = []  # the fake's requests
LOCK = threading.Lock()
TOKENS = {}  # fake access token -> krunker.io account name (issued by this run)
FAULTS = {"claims": 0, "purchases": 0, "shop": 0}  # the fake's request counts (the faults above)


def check(name, ok, detail=""):
    results.append(bool(ok))
    show = detail and (not ok or len(detail) <= 200)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if show else ''}", flush=True)


def record(entry):
    entry.setdefault("t", time.time())
    with LOCK:
        LOG.append(entry)


def snapshot():
    with LOCK:
        return list(LOG)


def reset_log():
    with LOCK:
        LOG.clear()
    FAULTS.update({"claims": 0, "purchases": 0, "shop": 0})


def b64u(data):
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode()


def fake_jwt(username):
    """a JWT the client reads (its exp and username claims); the signature is not checked by the client"""
    now = int(time.time())
    head = b64u(json.dumps({"alg": "none", "typ": "JWT"}).encode())
    body = b64u(json.dumps({"username": username, "exp": now + 3600, "iat": now}).encode())
    token = head + "." + body + ".e2e"
    TOKENS[token] = username
    return token


def username_of_jwt(token):
    try:
        part = token.split(".")[1]
        part += "=" * (-len(part) % 4)
        return json.loads(base64.urlsafe_b64decode(part)).get("username", "")
    except Exception:  # noqa: BLE001 - a bad token is just unknown
        return ""


def read_log(path):
    try:
        with open(path, errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def wait_for(fn, secs=10.0):
    end = time.time() + secs
    while time.time() < end:
        try:
            v = fn()
            if v:
                return v
        except Exception:  # noqa: BLE001 - polling
            pass
        time.sleep(0.1)
    return None


def http_json(port, path, body=None):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request("http://127.0.0.1:%d%s" % (port, path), data=data,
                                 headers={"Content-Type": "application/json"} if data else {},
                                 method="POST" if data else "GET")
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status, json.loads(r.read() or b"null")
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b"null")


# ---------------------------------------------------------------- msgpack frames (the game socket's subset)

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


# ---------------------------------------------------------------- the fake krunker.io and KrunkNative brain URL

def profile_answer(username):
    return {"profile": {"player_name": username, "rank": 3, "conservative_rating": 1500, "wins": 4, "losses": 2,
                        "total_matches": 6, "total_kills": 40, "total_deaths": 30, "total_assists": 10,
                        "total_score": 1000, "total_objective_score": 100, "total_damage_done": 9000,
                        "total_time_played": 3600000, "total_accuracy": 50, "highest_kill_count": 9,
                        "highest_damage_in_match": 800, "last_milestone_claimed": -1},
            "milestones": [{"matches": 1, "reward": 10}, {"matches": 5, "reward": 20}]}


class Fake(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):  # quiet
        pass

    def _send(self, status, payload=None):
        body = b"" if payload is None else json.dumps(payload).encode()
        self.send_response(status)
        if payload is not None:
            self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if body:
            self.wfile.write(body)

    def _account(self):
        auth = self.headers.get("Authorization", "")
        if not auth.startswith("Bearer "):
            return None
        return TOKENS.get(auth[len("Bearer "):], "?unknown")

    def _handle(self, method):
        parts = urllib.parse.urlsplit(self.path)
        path = parts.path
        length = int(self.headers.get("Content-Length", "0") or 0)
        raw = self.rfile.read(length) if length else b""
        if path == "/__log":
            return self._send(200, snapshot())
        if path == "/__reset":
            reset_log()
            return self._send(200, {"ok": True})
        account = self._account()
        entry = {"m": method, "p": self.path, "account": account, "body": raw.decode(errors="replace") if raw else ""}
        record(entry)
        if path.startswith("/brain/"):
            return self._send(404, {"error": "not in the fake brain"})
        if not (path.startswith("/api/") or path.startswith("/gapi/")):
            return self._send(404, {"error": "unknown route"})
        if account is None:
            return self._send(401, {"data": None, "error": "no bearer"})
        user = account if account != "?unknown" else "kio_match"
        if method == "GET" and re.match(r"^/api/player-profile/me/season/\d+/region/\d+$", path):
            return self._send(200, {"data": profile_answer(user), "error": None})
        if method == "GET" and re.match(r"^/api/leaderboards/rankings/season/\d+/region/\d+\?limit=50&offset=\d+$", self.path):
            return self._send(200, {"data": {"leaderboard": [{"position": 1, "profile": {"player_name": user, "rank": 3,
                                                                                  "conservative_rating": 1500, "total_matches": 6}}],
                                                       "totalPages": 1}, "error": None})
        if method == "GET" and re.match(r"^/api/match-history/me/season/\d+/gameMode/undefined/isRanked/true/region/\d+\?limit=5&offset=\d+$", self.path):
            now_ms = int(time.time() * 1000)
            match = {"date": now_ms, "map": 0, "score_alpha": 3, "score_bravo": 1,
                     "historyEntries": [{"player_name": user, "rank": 3, "team": 1, "kills": 5, "deaths": 2, "assists": 1,
                                         "score": 500, "mmr_change": 12}]}
            return self._send(200, {"data": {"matchHistory": {"matches": [match], "totalPages": 1}}, "error": None})
        if method == "GET" and path == "/api/ranked/me/connection":
            return self._send(204)
        if method == "POST" and re.match(r"^/api/player-profile/me/claim-milestone/region/\d+$", path):
            FAULTS["claims"] += 1
            if FAULTS["claims"] == 1:  # the first claim is refused: v10 shows nothing for it
                return self._send(500, {"data": None, "error": "claim refused"})
            return self._send(200, {"data": {"success": True}, "error": None})
        if method == "GET" and path == "/gapi/ranked/shop":
            FAULTS["shop"] += 1
            expires = int(time.time() * 1000) + SHOP_EXPIRY_MS if FAULTS["shop"] == 1 else None
            return self._send(200, {"data": {"skins": [{"id": 7, "skin_id": 1120, "price": 300, "max_purchase": 0, "purchase_count": 0,
                                                        "restricted_by": None, "expires_at": expires,
                                                        "min_mmr": 9000, "max_mmr": 10000}]}, "error": None})
        if method == "POST" and path == "/gapi/ranked/purchase":
            FAULTS["purchases"] += 1
            if FAULTS["purchases"] == 3:  # the connection drops with no answer: v10's "Network error" (a thrown request)
                self.close_connection = True
                return None
            return self._send(200, {"data": {"success": True}, "error": None})
        if method == "GET" and path == "/gapi/player/me":
            return self._send(200, {"data": {"username": user, "ranked_points": 777}, "error": None})
        return self._send(404, {"data": None, "error": "not in the fake"})

    def do_GET(self):
        self._handle("GET")

    def do_POST(self):
        self._handle("POST")


def start_http():
    httpd = ThreadingHTTPServer(("127.0.0.1", HTTP_PORT), Fake)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    return httpd


# ---------------------------------------------------------------- the fake gamefrontend queue socket

async def queue_handler(ws):
    query = urllib.parse.parse_qs(urllib.parse.urlsplit(ws.request.path).query)
    token = (query.get("token") or [""])[0]
    user = username_of_jwt(token)
    record({"ws": "queue", "path": urllib.parse.urlsplit(ws.request.path).path, "account": user,
            "token_issued": token in TOKENS, "maps": (query.get("maps") or [""])[0], "regions": (query.get("regions") or [""])[0]})
    mode = MODES.get(user, "queue")
    if mode == "garbage":
        await ws.send("this is not json {")
    elif mode == "cooldown":
        await ws.send(json.dumps({"type": "ERROR", "payload": {"code": "COOLDOWN", "message": "cooldown",
                                                               "payload": {"cooldown": 75000}}}))
    else:
        await ws.send(json.dumps({"type": "QUEUE_STATUS", "payload": {"status": "QUEUED"}}))
        if mode == "match":
            await asyncio.sleep(2.0)  # the searching banner shows for a while before MATCHED
            await ws.send(json.dumps({"type": "QUEUE_STATUS", "payload": {"status": "MATCHED", "assignment": {
                "connection": "127.0.0.1:%d" % MATCH_PORT, "gameId": "e2e-game", "clientId": "e2e-ticket", "webUrl": ""}}}))
    try:
        async for _ in ws:
            pass
    except websockets.ConnectionClosed:
        pass


async def match_handler(ws):
    """the match's game socket: records the upgrade (path, query keys, the account of at=) and the client's
    packets; sends io-init and the account row ("a": row[47] = RP_TEST), then closes"""
    url = urllib.parse.urlsplit(ws.request.path)
    query = urllib.parse.parse_qs(url.query)
    token = (query.get("at") or [""])[0]
    user = username_of_jwt(token) if token in TOKENS else "?"
    entry = {"ws": "game", "path": url.path, "query": sorted(query), "token_issued": token in TOKENS, "account": user,
             "frames": []}
    record(entry)
    row = [0] * 55
    row[RP_ROW_INDEX] = RP_TEST
    row[SCORE_INDEX] = SCORE_TEST

    async def reader():
        try:
            async for msg in ws:
                entry["frames"].append(frame_name(msg) if isinstance(msg, bytes) else "text")
        except (websockets.ConnectionClosed, asyncio.CancelledError):
            pass

    rd = asyncio.ensure_future(reader())
    try:
        await ws.send(frame("io-init", "sockME", ah=0x11))
        await ws.send(frame("a", None, ACCOUNT_ID, user, row, "legacy-token", ah=0x22))
        await asyncio.sleep(1.0)
        # a wallet packet (v10's UPDATE_BALANCES, balances[3] = the ranked points, v10:208200): the balance after the row
        await ws.send(frame("ub", [0, 0, 0, {"amount": WALLET_TEST, "change": 100}], ah=0x33))
        await asyncio.sleep(3.0)
        await ws.close(1000, "e2e")
    except (websockets.ConnectionClosed, asyncio.CancelledError):
        pass
    await rd


def start_ws():
    loop = asyncio.new_event_loop()

    async def serve():
        async with ws_server.serve(queue_handler, "127.0.0.1", WS_PORT), ws_server.serve(match_handler, "127.0.0.1", MATCH_PORT):
            await asyncio.Future()

    def run():
        asyncio.set_event_loop(loop)
        loop.run_until_complete(serve())

    threading.Thread(target=run, daemon=True).start()


# ---------------------------------------------------------------- KrunkNative's brain (kn_brain on a scratch database)

class Social:
    def __init__(self, port):
        self.url = "ws://127.0.0.1:%d/social/ws?enc=json" % port
        self.inbox = []

    async def __aenter__(self):
        self.ws = await websockets.connect(self.url)
        return self

    async def __aexit__(self, *a):
        await self.ws.close()

    async def call(self, label, *frame):
        await self.ws.send(json.dumps(list(frame)))
        deadline = time.time() + 10
        while True:
            for i, f in enumerate(self.inbox):
                if f[0] == label:
                    return self.inbox.pop(i)
            self.inbox.append(json.loads(await asyncio.wait_for(self.ws.recv(), deadline - time.time())))


async def register(port, name):
    async with Social(port) as s:
        r = await s.call("a", "a", 0, [name, "pw-rkrunker-%d" % os.getpid(), 0], "captcha")
        assert r[1] is None, r
        return {"name": name, "token": r[5]}


def start_brain():
    """kn_brain on a scratch database: (port, process, scratch command, dbcfg), or None (skipped)"""
    scratch = [sys.executable, "-I", os.path.join(ROOT, "services", "brain", "tests", "db_scratch.py"), ROOT]
    r = subprocess.run(scratch + ["create", DRIVER, NAME], capture_output=True)
    if r.returncode:
        return None, scratch
    dbcfg = json.loads(r.stdout)
    dbenv = {"KN_DATABASE_" + k.upper(): str(v) for k, v in dbcfg.items()}
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=SECRET, **dbenv)
    subprocess.run([f"{BUILD}/services/brain/kn_brain", "--migrate-only"], cwd=ROOT, env=env, check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    log = os.path.join(WORK, "brain.log")
    brain = subprocess.Popen([f"{BUILD}/services/brain/kn_brain", "--brain.host=127.0.0.1", "--brain.port=0",
                              "--accounts.signup_rewards.enabled=false", "--brain.region=local",
                              "--brain.id=rkrunker-%d" % os.getpid(), "--finder.poll_interval_s=1",
                              "--brain.ranked.min_level=0", "--brain.ranked.team_size=1", "--brain.ranked.start_grace_s=60",
                              "--accounts.starting_kr=500"],
                             cwd=ROOT, env=env, stdout=open(log, "w"), stderr=subprocess.STDOUT,
                             stdin=subprocess.DEVNULL, start_new_session=True)
    procs.append(brain)
    m = wait_for(lambda: re.search(r"listening on http://127\.0\.0\.1:(\d+) \(ws", read_log(log)), 20)
    if not m:
        return None, scratch
    return int(m.group(1)), scratch


# ---------------------------------------------------------------- one headless client

SCRIPT_OFFSET_MS = 15000
SCRIPTS = {
    "kio_match": """0 menu
1000 call openRankedMenu
3000 nodes @ranked .rankedMenuModal .start-button
3100 text @region .rankedMenuModal .region-indicator
4000 call knRkTab 1
6000 nodes @profile .profile-container .stat-card
6100 text @profilename .profile-container .username
6200 grep @winrate 66.7% .stat-value
6300 grep @kd 1.33 .stat-value
7000 call knRkTab 2
9000 nodes @board .leaderboard-container .data-table tbody tr
9100 nodes @own .leaderboard-container .highlight-row
10000 call knRkTab 3
12000 nodes @history .match-entry
12100 call knRkMatch 0
12200 nodes @expanded .match-details
13000 call knRkTab 4
15000 nodes @shop .shop-item-wrapper
15100 text @rpbefore .rp-balance .currency-amount
15200 nodes @unlocked .shop-item-wrapper .buy-button:not([disabled])
16000 call knRkTab 0
17000 call knRkClaim
17500 nodes @claimerr .text-error
18000 call knRkTab 4
20000 call knRkBuy 7
21000 call knViewPop 1
22000 call knRkTab 0
23000 call knRkStart
23500 nodes @searching .ranked-matchmaking-status.show
23600 text @searching .ranked-matchmaking-status
23700 text @footerq .footer .status-text
27500 text @overlaytitle .overlay-title
27600 text @overlaytext .overlay-description
31000 call openRankedMenu
31500 call knRkTab 4
32000 text @rpafter .rp-balance .currency-amount
32500 call knRkClaim
33000 call knRkBuy 7
33500 call knViewPop 1
34000 call knRkTab 4
34500 text @rpmoved .rp-balance .currency-amount
35000 text @kiolevel [id=knKioHeaderText]
36000 call knRkBuy 7
36500 call knViewPop 1
37000 text @neterr .text-error
38000 quit
""",
    "kio_cooldown": """0 menu
1000 call openRankedMenu
3000 nodes @ranked .rankedMenuModal .start-button
4000 call knRkStart
8000 nodes @overlay .overlay-container
8100 text @overlaytitle .overlay-title
8200 text @overlaytext .overlay-description
8300 text @banner .ranked-matchmaking-status
9000 quit
""",
    "kn_only": """0 menu
1000 call openRankedMenu
3000 nodes @ranked .rankedMenuModal .start-button
4000 call knRkTab 1
6000 nodes @profile .profile-container .stat-card
6100 text @profilename .profile-container .username
7000 call knRkTab 0
8000 call knRkStart
11000 nodes @searching .ranked-matchmaking-status.show
11500 text @menurp [id=menuRPCount]
12000 call openRankedChallenges
12500 nodes @chal [id=challRankedHeadr]
13000 quit
""",
    "kio_garbage": """0 menu
1000 call openRankedMenu
3000 nodes @ranked .rankedMenuModal .start-button
4000 call knRkStart
7000 text @banner .ranked-matchmaking-status
7100 text @overlaytitle .overlay-title
7200 text @overlaytext .overlay-description
8000 quit
""",
    "kio_down": """0 menu
1000 call openRankedMenu
3000 nodes @ranked .rankedMenuModal .start-button
4000 call knRkStart
7000 text @banner .ranked-matchmaking-status
7100 text @overlaytitle .overlay-title
7200 text @overlaytext .overlay-description
8000 quit
""",
    "both": """0 menu
1000 call openRankedMenu
3000 nodes @ranked .rankedMenuModal .start-button
4000 call knRkStart
4500 text @menurp [id=menuRPCount]
15000 text @menurpafter [id=menuRPCount]
16000 quit
""",
}


def start_node(brain_port):
    """a server_bin linked to the brain: the public lobby the client's startup seek joins (v10 competitive e2e).
    Returns (process, log path); the krunker.io runs start without it (it would be the client's startup server)."""
    log = os.path.join(WORK, "RKR.log")
    gs = os.path.join(WORK, "RKR.json")
    cfg = {"node_id": "RKR", "name": "ranked krunker e2e", "region": "local", "listen_port": GAME_PORT,
           "public_host": "127.0.0.1", "brain_url": "ws://127.0.0.1:%d/mm/internal/ws" % brain_port,
           "brain_secret": SECRET, "modes": ["ffa"], "max_players": 8, "shutdown_grace": 0, "custom_games": True,
           "idle_kick_seconds": 0, "finder_url": "http://127.0.0.1:%d" % brain_port}
    with open(gs, "w") as f:
        json.dump({"gameserver": cfg}, f)
    p = subprocess.Popen([f"{BUILD}/server_bin", "--headless", "--config", gs, "--cfg", "none"], cwd=ROOT,
                         stdout=open(log, "w"), stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, start_new_session=True)
    procs.append(p)
    return p, log


def client(name, brain_url, storage, gamefrontend=None):
    """one headless client_bin on the v10 page; returns (process, log path)"""
    path = os.path.join(WORK, name + ".script")
    # the clock starts when the menu is up, after the client's load (about 10 s): every step is
    # offset so that the steps keep their spacing (a step in the past runs at once, before its answer)
    lines = []
    for line in SCRIPTS[name].splitlines():
        head, _, rest = line.partition(" ")
        if head.isdigit() and int(head) > 0:
            line = "%d %s" % (int(head) + SCRIPT_OFFSET_MS, rest)
        lines.append(line)
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")
    cfg = os.path.join(WORK, name + "-cfg")
    os.makedirs(os.path.join(cfg, "krunknative"), exist_ok=True)
    # no Quickplay seek at launch (Settings > Quickplay > Auto Seek on Launch): the script drives the menu
    storage = dict({"knSettings": json.dumps({"knSeekOnLaunch": False})}, **storage)
    for key, value in storage.items():
        with open(os.path.join(cfg, "krunknative", key + ".json"), "w") as f:
            f.write(value)
    log = os.path.join(WORK, name + ".log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
    env.update({
        "KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": cfg,
        "XDG_STATE_HOME": os.path.join(WORK, name + "-state"), "KRUNKNATIVE_NO_BROWSER": "1",
        "KRUNKNATIVE_INPUT_SCRIPT": path, "KRUNKNATIVE_WINDOW_SIZE": "1280x720",
        "KRUNKNATIVE_BRAIN_URL": brain_url or "0", "KRUNKNATIVE_NET_TRANSPORT": "tcp",
        "KRUNKNATIVE_FINDER_URL": (brain_url or "http://127.0.0.1:%d/brain" % HTTP_PORT) + "/servers",
        "KRUNKNATIVE_KRUNKER_API": "http://127.0.0.1:%d/api" % HTTP_PORT,
        "KRUNKNATIVE_KRUNKER_GAPI": "http://127.0.0.1:%d/gapi" % HTTP_PORT,
        "KRUNKNATIVE_KRUNKER_GAMEFRONTEND": gamefrontend or "http://127.0.0.1:%d" % WS_PORT,
    })
    p = subprocess.Popen(["stdbuf", "-oL", "-eL", f"{BUILD}/client_bin"], cwd=ROOT, stdout=open(log, "w"),
                         stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, start_new_session=True)
    procs.append(p)
    return p, log


def finish(p, name, secs=120):
    try:
        p.wait(secs)
    except subprocess.TimeoutExpired:
        os.killpg(p.pid, signal.SIGTERM)
        try:
            p.wait(5)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
        check(name + ": client finished its script", False, "timed out")


def nodes(text, label):
    m = re.search(r"\[input_script\] nodes t=\d+ @%s elements=\d+ match=(\d+)" % re.escape(label), text)
    return int(m.group(1)) if m else -1


def text_of(text, label):
    m = re.search(r"\[input_script\] text t=\d+ @%s (.*)" % re.escape(label), text)
    return m.group(1) if m else ""


def current_season():
    """v10's _7307(): 0 before August 2025, else the months since then (UTC)"""
    now = time.gmtime()
    if now.tm_year < 2025 or (now.tm_year == 2025 and now.tm_mon < 8):
        return 0
    return 12 * (now.tm_year - 2025) + (now.tm_mon - 8 + 1)


def grep_hits(text, label):
    m = re.search(r"\[input_script\] grep t=\d+ @%s needle=\S+ n=\d+ hits=(\d+)" % re.escape(label), text)
    return int(m.group(1)) if m else -1


def kio_entries(entries):
    """the requests of the krunker.io side (api, gapi and the queue socket)"""
    return [e for e in entries if e.get("p", "").startswith(("/api/", "/gapi/")) or e.get("ws") == "queue"]


def run_kio_client(name, user, brain_url, storage, gamefrontend=None):
    reset_log()
    p, log = client(name, brain_url, storage, gamefrontend)
    finish(p, name)
    return read_log(log), snapshot()


# ---------------------------------------------------------------- the checks

def check_kio_match(text, entries, token):
    api = [e for e in entries if e.get("p", "").startswith(("/api/", "/gapi/"))]
    paths = [e["p"] for e in api]
    check("kio_match: every krunker.io request carries the account's Bearer token", bool(api) and all(e["account"] == "kio_match" for e in api),
          str([(e["p"], e["account"]) for e in api])[:300])
    checks = [
        ("profile: v10's player-profile path with the region index", r"^/api/player-profile/me/season/\d+/region/\d+$"),
        ("leaderboard: v10's rankings path, 50 per page", r"^/api/leaderboards/rankings/season/\d+/region/\d+\?limit=50&offset=0$"),
        ("match history: v10's path (gameMode/undefined, isRanked/true, 5 per page)",
         r"^/api/match-history/me/season/\d+/gameMode/undefined/isRanked/true/region/\d+\?limit=5&offset=0$"),
        ("rejoin lookup: v10's ranked/me/connection", r"^/api/ranked/me/connection$"),
        ("milestone claim: v10's claim-milestone path", None),
        ("shop: gapi /ranked/shop", r"^/gapi/ranked/shop$"),
        ("purchase: gapi /ranked/purchase", r"^/gapi/ranked/purchase$"),
    ]
    for label, pattern in checks:
        if pattern is None:
            hit = [e for e in api if e["p"].startswith("/api/player-profile/me/claim-milestone/region/") and e["m"] == "POST"]
            check("kio_match: " + label, bool(hit), str(paths)[:200])
        else:
            check("kio_match: " + label, any(re.search(pattern, p) for p in paths), str(paths)[:300])
    buys = [e for e in api if e["p"] == "/gapi/ranked/purchase" and e["m"] == "POST"]
    check("kio_match: the purchase body is v10's {skin_id: the shop row's id}", bool(buys) and json.loads(buys[0]["body"]) == {"skin_id": 7},
          str([e["body"] for e in buys]))
    season = str(current_season())
    seasons = {"profile": None, "leaderboard": None, "history": None}
    for e in api:
        m = re.search(r"/season/(\d+)/", e["p"])
        if m and e["m"] == "GET":
            key = "profile" if "player-profile" in e["p"] else "leaderboard" if "leaderboards" in e["p"] else "history"
            seasons[key] = m.group(1)
    check("kio_match: profile, leaderboard and history read the current season (v10's _7307; v10 has no season picker: $selectedSeason is never written)",
          all(seasons[k] == season for k in seasons), str(seasons) + " current " + season)
    queues = [e for e in entries if e.get("ws") == "queue"]
    check("kio_match: FIND MATCH opens the queue socket for the krunker.io account", len(queues) >= 1 and queues[0]["account"] == "kio_match",
          str(queues)[:200])
    if queues:
        q = queues[0]
        check("kio_match: the queue carries the account's token, maps %s and region eu (v10's default, _34041)" % MAPS,
              q["token_issued"] and q["maps"] == MAPS and q["regions"] == "eu", str(q)[:240])
    check("kio_match: QUEUED: the banner says Searching for match...", "Searching for match" in text_of(text, "searching"),
          text_of(text, "searching")[:120])
    check("kio_match: QUEUED: the status line says Looking for match...", "Looking for match" in text_of(text, "footerq"),
          text_of(text, "footerq")[:120])
    check("kio_match: QUEUED then MATCHED: the client's MATCHED frame is handled (match found)", "[ranked] match found" in text,
          "no match-found line in the log")
    game = [e for e in entries if e.get("ws") == "game"]
    check("kio_match: MATCHED joins the match's game socket at its connection (one upgrade, on the match port)",
          len(game) == 1 and "ranked match: opening ws://127.0.0.1:%d/ws?" % MATCH_PORT in text, str(game)[:200])
    if game:
        g = game[0]
        check("kio_match: the game socket is v10's /ws with at= the account's token and no gameId / clientKey",
              g["path"] == "/ws" and g["query"] == ["at"] and g["token_issued"] and g["account"] == "kio_match", str(g)[:200])
        check("kio_match: no joinedRank on the new socket (v10 sends it on a game socket that is already open)",
              "joinedRank" not in g["frames"], str(g["frames"])[:200])
    shown = [l for l in text.splitlines() if l.startswith("[input_script] text") and "not available" in l.lower()]
    check("kio_match: the match's join shows no Not Available popup and no not-available text",
          not shown and "Not Available" not in text_of(text, "overlaytitle"), (shown or [""])[0][:200])
    check("kio_match: the RP balance is unknown (--) before a krunker.io game signs the account in",
          text_of(text, "rpbefore").endswith("text=--"), text_of(text, "rpbefore")[:60])
    check("kio_match: the wallet packet's balance (ub, balances[3].amount = %d) replaces the account row's (v10:208200)" % WALLET_TEST,
          text_of(text, "rpafter").endswith("text=" + "{:,}".format(WALLET_TEST)), text_of(text, "rpafter")[:60])
    check("kio_match: a refused claim shows nothing (no .text-error popup, v10:98757-98764)", nodes(text, "claimerr") == 0,
          str(nodes(text, "claimerr")))
    check("kio_match: a dropped purchase request shows v10's Network error (v10:111062)",
          text_of(text, "neterr").endswith("text=Network error"), text_of(text, "neterr")[:80])
    moved = WALLET_TEST + MILESTONE_REWARD - SKIN_PRICE
    check("kio_match: a claim adds the milestone's reward and a purchase takes the price off the balance (v10:98744, 111042): %d"
          % moved, text_of(text, "rpmoved").endswith("text=" + "{:,}".format(moved)), text_of(text, "rpmoved")[:60])
    check("kio_match: the account row's level is row[6]'s score (level 20): the header's krunker.io bar says Lv 20",
          text_of(text, "kiolevel").endswith("Lv 20"), text_of(text, "kiolevel")[:120])
    check("kio_match: no request to /player/me (the balance is the account row, not gapi)",
          not any(e.get("p", "").startswith("/gapi/player/me") for e in entries))
    check("kio_match: the profile shows the krunker.io account's name", "kio_match" in text_of(text, "profilename"),
          text_of(text, "profilename")[:80])
    check("kio_match: the Ranked popup opened (its start button)", nodes(text, "ranked") == 1, str(nodes(text, "ranked")))
    check("kio_match: the leaderboard lists the account's row", nodes(text, "board") >= 1, str(nodes(text, "board")))
    check("kio_match: the leaderboard row is own when its name is the profile's player_name (v10:111751)", nodes(text, "own") >= 1,
          str(nodes(text, "own")))
    check("kio_match: the profile's win rate is v10's derivation from the totals (4 wins of 6: 66.7%)", grep_hits(text, "winrate") >= 1,
          str(grep_hits(text, "winrate")))
    check("kio_match: the profile's K/D is kills / max(1, deaths) (40 / 30 = 1.33, v10:98337)", grep_hits(text, "kd") >= 1, str(grep_hits(text, "kd")))
    check("kio_match: a history entry expands on click (v10: the entry toggles, v10:108280)", nodes(text, "expanded") >= 1,
          str(nodes(text, "expanded")))
    check("kio_match: an item's expiry drops the shop's answer, so the next visit asks again (v10:112316-112320; the cache would hold 60 s)",
          len(shop_times(entries)) >= 2 and shop_times(entries)[1] >= SHOP_EXPIRY_MS / 1000.0, str(shop_times(entries))[:120])
    check("kio_match: the shop's MMR range does not lock an item (v10 locks on restricted_by only, v10:111314)", nodes(text, "unlocked") >= 1,
          str(nodes(text, "unlocked")))
    check("kio_match: match history lists the match", nodes(text, "history") >= 1, str(nodes(text, "history")))
    check("kio_match: the shop lists the item", nodes(text, "shop") >= 1, str(nodes(text, "shop")))
    check("kio_match: no token in the client's log", token not in text and "Bearer" not in text)


def shop_times(entries):
    """the shop requests' times, from the first one (the expiry refetch's spacing)"""
    ts = sorted(e["t"] for e in entries if e.get("p") == "/gapi/ranked/shop" and e.get("m") == "GET")
    return [t - ts[0] for t in ts] if ts else []


def check_kio_garbage(text, entries):
    check("kio_garbage: an unparseable queue frame sets the banner's message (v10:98489-98497)",
          text_of(text, "banner").endswith("text=Failed to parse server message"), text_of(text, "banner")[:120])
    check("kio_garbage: and the internal error popup (v10 INTERNAL_ERROR, v10:111556-111563)",
          text_of(text, "overlaytitle").endswith("text=Internal Error"), text_of(text, "overlaytitle")[:80])


def check_kio_down(text, entries):
    check("kio_down: a queue socket that never opens sets 'Failed to connect' in the banner (v10:98441-98452)",
          text_of(text, "banner").endswith("text=Failed to connect to matchmaking server"), text_of(text, "banner")[:120])
    check("kio_down: and the internal error popup, not a footer line", text_of(text, "overlaytitle").endswith("text=Internal Error"),
          text_of(text, "overlaytitle")[:80])


def check_kio_cooldown(text, entries):
    queues = [e for e in entries if e.get("ws") == "queue"]
    check("kio_cooldown: the queue socket opened for that account", bool(queues) and queues[0]["account"] == "kio_cooldown", str(queues)[:160])
    check("kio_cooldown: ERROR COOLDOWN shows the cooldown overlay", text_of(text, "overlaytitle").endswith("text=Cooldown"),
          text_of(text, "overlaytitle"))
    check("kio_cooldown: the cooldown text gives the time left (75 s = 1:15)",
          "Cooldown will end in 1:15" in text_of(text, "overlaytext"), text_of(text, "overlaytext")[:200])
    check("kio_cooldown: the server's ERROR message is the banner's text (v10:98476-98486)",
          text_of(text, "banner").endswith("text=cooldown"), text_of(text, "banner")[:120])


def check_kn_only(text, entries, kn_user):
    kio = kio_entries(entries)
    check("kn_only: no krunker.io request (api, gapi or the queue socket)", not kio, str([(e.get("p") or e.get("ws")) for e in kio])[:200])
    check("kn_only: the KrunkNative account's profile shows its name (kn_brain's route)", kn_user in text_of(text, "profilename"),
          text_of(text, "profilename")[:80])
    check("kn_only: the Ranked popup opened", nodes(text, "ranked") == 1, str(nodes(text, "ranked")))
    check("kn_only: FIND MATCH searches through the brain (the searching banner)", nodes(text, "searching") == 1, str(nodes(text, "searching")))
    check("kn_only: no krunker.io client line in the log", "[krunker-ranked]" not in text and "[kio-account]" not in text)
    check("kn_only: the header's RP count is KrunkNative's own value (0)", text_of(text, "menurp").endswith("text=0"), text_of(text, "menurp")[:60])
    check("kn_only: the Ranked Challenges link opens the Challenges window with its ranked block (v10:215670)",
          nodes(text, "chal") >= 1, str(nodes(text, "chal")))


def check_both(text, entries):
    queues = [e for e in entries if e.get("ws") == "queue"]
    check("both: krunker.io wins: the queue socket is the krunker.io account's", bool(queues) and queues[0]["account"] == "kio_both",
          str(queues)[:200])
    game = [e for e in entries if e.get("ws") == "game"]
    check("both: the match joins the krunker.io account's game socket (at= its token)", len(game) == 1 and game[0]["account"] == "kio_both",
          str(game)[:200])
    before = [text_of(text, "menurp")]
    check("both: the header's RP count is -- (unknown) before the krunker.io game sends the balance",
          before and before[0].endswith("text=--"), str(before)[:80])
    check("both: the header's RP count is the account row's rankedPoints (%s), then the wallet packet's (%s)" % (
              "{:,}".format(RP_TEST), "{:,}".format(WALLET_TEST)),
          text_of(text, "menurpafter").endswith("text=" + "{:,}".format(WALLET_TEST)), text_of(text, "menurpafter")[:80])
    api = [e for e in entries if e.get("p", "").startswith(("/api/", "/gapi/"))]
    check("both: the krunker.io requests carry the krunker.io account's Bearer", bool(api) and all(e["account"] == "kio_both" for e in api),
          str([(e["p"], e["account"]) for e in api])[:200])


# ---------------------------------------------------------------- main

def main():
    if not os.access(f"{BUILD}/client_bin", os.X_OK) or not os.access(f"{BUILD}/services/brain/kn_brain", os.X_OK):
        print("ranked_krunker_e2e: client_bin or kn_brain missing in %s: skipped" % BUILD)
        return 77
    httpd = start_http()
    start_ws()
    brain_port, scratch = start_brain()
    try:
        if brain_port is None:
            print("ranked_krunker_e2e: kn_brain did not start (scratch database %s): skipped" % DRIVER)
            return 77
        kn_user = "rkr%d" % (os.getpid() % 100000)
        kn = asyncio.run(register(brain_port, kn_user))
        check("kn_brain registers the KrunkNative account", bool(kn["token"]), str(kn)[:120])
        brain_url = "http://127.0.0.1:%d" % brain_port
        # the node is the startup seek's lobby for every run (the client joins it, as on a real launch)
        node, node_log = start_node(brain_port)
        check("a server_bin node links to the brain (the startup seek's lobby)",
              bool(wait_for(lambda: "welcomed" in read_log(node_log), 20)), read_log(node_log)[-200:])

        # 1 kio_match: a krunker.io account only (no KrunkNative account)
        tok = fake_jwt("kio_match")
        text, entries = run_kio_client("kio_match", "kio_match", brain_url,
                                       {"kio_access_token": tok, "kio_username": "kio_match"})
        check_kio_match(text, entries, tok)

        # 1b kio_garbage: an unparseable queue frame
        tok = fake_jwt("kio_garbage")
        text, entries = run_kio_client("kio_garbage", "kio_garbage", brain_url,
                                       {"kio_access_token": tok, "kio_username": "kio_garbage", "kio_level": "20"})
        check_kio_garbage(text, entries)

        # 1c kio_down: the queue's socket never opens (a port nothing listens on)
        tok = fake_jwt("kio_down")
        text, entries = run_kio_client("kio_down", "kio_down", brain_url,
                                       {"kio_access_token": tok, "kio_username": "kio_down", "kio_level": "20"},
                                       gamefrontend="http://127.0.0.1:%d" % DEAD_PORT)
        check_kio_down(text, entries)

        # 2 kio_cooldown
        tok = fake_jwt("kio_cooldown")
        text, entries = run_kio_client("kio_cooldown", "kio_cooldown", brain_url,
                                       {"kio_access_token": tok, "kio_username": "kio_cooldown", "kio_level": "20"})
        check_kio_cooldown(text, entries)

        # 3 kn_only: the real brain and the node, no krunker.io account
        text, entries = run_kio_client("kn_only", kn_user, brain_url,
                                       {"krunker_username": kn["name"], "krunker_token": kn["token"]})
        check_kn_only(text, entries, kn_user)

        # 4 both: krunker.io wins (the real brain and node for the KrunkNative account beside it)
        tok = fake_jwt("kio_both")
        text, entries = run_kio_client("both", kn_user, brain_url,
                                       {"krunker_username": kn["name"], "krunker_token": kn["token"],
                                        "kio_access_token": tok, "kio_username": "kio_both", "kio_level": "20"})
        check_both(text, entries)
        node.terminate()
    finally:
        for p in procs:
            if p.poll() is None:
                os.killpg(p.pid, signal.SIGTERM)
        for p in procs:
            try:
                p.wait(10)
            except subprocess.TimeoutExpired:
                os.killpg(p.pid, signal.SIGKILL)
                p.wait()
        if brain_port is not None:
            subprocess.run(scratch + ["drop", DRIVER, NAME], capture_output=True)
        httpd.shutdown()
    if all(results):
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print("logs kept in " + WORK)
    print(f"{sum(results)}/{len(results)} passed")
    return 0 if results and all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
