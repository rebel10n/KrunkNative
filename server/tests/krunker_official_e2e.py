#!/usr/bin/env python3
"""KN_KRUNKER_OFFICIAL end to end against a local stub of krunker.io (client/include/krunker_net.h):
nothing leaves the machine.

  python3 -I server/tests/krunker_official_e2e.py BUILD_DIR [WEB_DIST_DIR] [--native|--web|--webview|--exclude|--live]   (repo root)

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
    @msgpack/msgpack number rules). It sends io-init, load, init (map 43 = Bastion, mode 0: past v7.1.4's 32 maps,
    shared/include/krunker_maps.h), "0" (a stub
    player), three "cc" (a script, garbage, an undefined result), a "cpt" ALTCHA challenge, "k"
    moving the stub player every 100 ms, "pi" every second; after the client's "en" its own "0",
    "l" and a "ch" line.
Map run "native-hq": init with map 34 (HQ, refused before this client shipped Krunker's whole list); the
client must load assets/maps/official/34_hq.json, stand on the floor at the HQ spawn the stub puts it on
(no "l" corrects it) and walk across it with W while the stub takes its "q" frames.
Live build runs (client/include/krunker_build_fetch.h; part of --native, or --live alone): the stub's
matchmaker also publishes a Krunker build (tools/krunker/publish_build.py's manifest + document,
KRUNKNATIVE_KRUNKER_BUILD_URL) and its game server speaks only that build. "native-live": a build that is
not the compiled one (another buildVersion and AHKEY, renamed packets, moved ADD_PLAYERS / UPDATE_PLAYERS
offsets and strides, KILL_PLAYER's victim and killer swapped, a packet the client does not know): the client
fetches it, seeks with its buildVersion and plays the whole game in it. "native-live-refused": the published
build lost a field the client reads: the join fails on the error screen saying which build and why, before
any seek. "native-live-stale": the matchmaker refuses the published build's buildVersion (InvalidVersion):
the client asks the manifest again, then seeks with the compiled build and joins. "native-live-fallback": the
manifest answers 503: the compiled build joins.
Find New Game runs (--exclude alone): "native-exclude" (Quick Match with Server Type Official, the lobby
list: never the game it leaves, no bouncing between two, the least recent once all are recent) and
"native-exclude-mm" (no list from krunker_lobbies: krunker_net's own list pick, then the live client's
excludeGame seeks around the recent games); the stub then has three lobbies and no captcha.
Lobby runs (the default, no KRUNKNATIVE_KRUNKER_JOIN): "native-lobby" clicks the stub's official lobby
(GET /game-list) in the server browser from the offline game: it is joined in-client (Cancel in the
Security Challenge modal first ends the join saying so, a second click + a pasted token joins);
"native-optout" (KRUNKNATIVE_KRUNKER_JOIN=browser) checks the click opens krunker.io in the browser instead.
Reload runs (--reload alone): "native-reload" (--krunker, a lobby joined, F5: the relaunched client,
whose command line alone would join the matchmaker's pick, rejoins that official game by its id) and
"native-reload-gone" (the game ended meanwhile: InvalidGameId, the client says so and seeks another).
Captcha runs (krunker_captcha.h): native "callback" (the one-time 127.0.0.1 callback: a wrong secret,
a foreign Origin, a body that is not a token, an expired listener, then the token, then a second
token), native "paste" (a bad paste refused in the modal, then a good one), native "webview" (the
challenge window of kn_webview.h, WebKitGTK on a private Xvfb of the run's own whose top-level windows
x11_watch.py records: it loads the test's stub krunker.io page, KRUNKNATIVE_KRUNKER_CAPTCHA_URL, whose
stub `turnstile` widget "is solved" by firing its own callback; silent first: it passes by itself, so
the challenge runs hidden, nothing is ever mapped and the security-challenge modal stays hidden at every
probe of the run; the first token is refused by the matchmaker, the challenge runs again hidden, the
second joins; a frame of another origin tries to post a forged token), native
"webview-interactive" (the stub widget calls before-interactive-callback first: only then is the window
created, mapped once already centred on the work area, never moved, and the modal comes up with it),
web "widget" (the stub widget's
token) and web "domain" (110200: the modal says why and falls back to the paste box). Those native
runs without a display (callback, paste) also cover a webview build falling back. Each
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
Exit 0 when every check passed, 77 when nothing could run (--webview: when this build has no webview,
WebKitGTK / Xvfb are missing; a full run then only says the webview run was skipped); logs stay in
~/.cache/kn-krunker-official-e2e-* on failure.
"""
import asyncio
import base64
import hashlib
import json
import math
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
from x11_watch import PrivateXvfb, X11Watch  # noqa: E402

try:
    import websockets
    import websockets.asyncio.server as ws_server
    import websockets.sync.client as ws_client
except ImportError:  # pragma: no cover
    websockets = None

ARGS = [a for a in sys.argv[1:] if not a.startswith("--")]
BUILD = os.path.abspath(ARGS[0] if ARGS else "cmake-build-debug")
DIST = os.path.abspath(ARGS[1]) if len(ARGS) > 1 else None
ONLY = {"--native": "native", "--web": "web", "--webview": "webview", "--exclude": "exclude", "--reload": "reload", "--ingame": "ingame",
        "--fx": "fx", "--world": "world", "--actions": "actions", "--live": "live",
        "--social": "social", "--rounds": "rounds", "--hud": "hud",
        "--voice": "voice", "--popups": "popups", "--state": "state",
        "--webvoice": "webvoice"}.get(next((a for a in sys.argv[1:] if a.startswith("--")), ""), "")
ROOT = os.getcwd()
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-krunker-official-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
CHROME = shutil.which("chromium-browser") or shutil.which("chromium") or shutil.which("google-chrome")


def asset_tree():
    """The asset tree the client reads: this checkout's when it has the large content (models, textures:
    gitignored, so a worktree lacks them), else KRUNKNATIVE_ASSETS_DIR's (or the main checkout's) content with
    this checkout's tracked files on top (assets/maps/official/ is new in a worktree, and the client must play
    this checkout's official maps), through symlinks in WORK."""
    local = os.path.join(ROOT, "assets")
    main = os.environ.get("KRUNKNATIVE_ASSETS_DIR") or os.path.expanduser("~/src/KrunkNative/assets")
    if os.path.realpath(main) == os.path.realpath(local) or os.path.isdir(os.path.join(local, "models")) or \
            not os.path.isdir(os.path.join(main, "models")):
        return os.environ.get("KRUNKNATIVE_ASSETS_DIR") or local
    out = os.path.join(WORK, "assets")
    merged = ("maps", "sound")  # both trees' files, this checkout's win (sound: the mp3s are gitignored, a few files tracked)
    for name in merged:
        os.makedirs(os.path.join(out, name))
    for name in sorted(set(os.listdir(main)) | set(os.listdir(local))):
        if name not in merged:
            src = os.path.join(local, name) if os.path.lexists(os.path.join(local, name)) else os.path.join(main, name)
            os.symlink(src, os.path.join(out, name))
    for sub in merged:
        for src in (os.path.join(main, sub), os.path.join(local, sub)):
            for name in os.listdir(src) if os.path.isdir(src) else ():
                link = os.path.join(out, sub, name)
                if os.path.lexists(link):
                    os.unlink(link)
                os.symlink(os.path.join(src, name), link)
    return out


ASSETS = asset_tree()


def header_value(name):
    with open(os.path.join(ROOT, "shared/include/krunker_build.h")) as f:
        m = re.search(r"#define %s\s+(\S+)" % name, f.read())
    return m.group(1).strip('"').rstrip("u") if m else None


AHKEY = int(header_value("KRUNKER_AHKEY"), 0)
BUILD_VERSION = header_value("KRUNKER_BUILD_VERSION")
SALT = b"KRUNK3R_AC"
GAME_ID, CLIENT_ID, MY_SID, BOT_SID = "TEST:abc12", "cid-42", 9, 5
OLD_BURG_GAME = "TEST:oldbg"  # a second official lobby in the browser's list, on Old_Burg (never joined)
MAP_INDEX, MAP_NAME = 43, "Bastion"  # past v7.1.4's 32 maps: the map the client used to refuse
# killfeed-name regression: DEAD_SID is a player the stub adds ("0") then void-kills ("3", killer
# -1) so its name must appear in #killFeed; UNKNOWN_SID is a sid the stub never adds, whose "3"
# must be dropped (a join race) and must never render as "unknown committed ...".
DEAD_SID, UNKNOWN_SID = 7, 4242

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
# a "cpt" in a form krunker_altcha.c does not compute (not SHA-256): the client says it failed
ALTCHA_UNSOLVABLE = dict(ALTCHA, algorithm="SHA-512", challenge=hashlib.sha512((ALTCHA_SALT + str(ALTCHA_NUMBER)).encode()).hexdigest())


# ---------------------------------------------------------------- a live Krunker build (krunker_build_fetch.h)

sys.path.insert(0, os.path.join(ROOT, "tools/krunker"))
import publish_build  # noqa: E402

with open(os.path.join(ROOT, "tools/krunker/build.json")) as _f:
    COMMITTED_BUILD = json.load(_f)  # the compiled build (krunker_build.h), the stub's own language
LIVE_VERSION, LIVE_CLIENT, LIVE_AHKEY = "LiveE2EBuildVersion0000000000000", "10.0.9r1", 0x2b


class LiveBuild:
    """A Krunker build (a build.json document) the stub speaks instead of the compiled one: its packets
    are renamed, its per-player arrays and KILL_PLAYER's arguments laid out by Krunker field name."""

    def __init__(self, doc):
        self.doc = doc
        live_s2c = {e["name"]: e["wire"] for e in doc["packets_s2c"]}
        live_c2s = {e["name"]: e["wire"] for e in doc["packets_c2s"]}
        self.s2c = {e["wire"]: live_s2c.get(e["name"], e["wire"]) for e in COMMITTED_BUILD["packets_s2c"]}
        self.c2s_back = {live_c2s[e["name"]]: e["wire"] for e in COMMITTED_BUILD["packets_c2s"] if e["name"] in live_c2s}

    def layout(self, which):
        return COMMITTED_BUILD["layouts"][which], self.doc["layouts"][which]

    def remap(self, values, which, stride_key="stride"):
        base, live = self.layout(which)
        live_off = {f["name"]: f["offset"] for f in live["fields"]}
        bs = base.get(stride_key) or len(values)
        # one state (no stride): as long as the live offsets of the values given
        ls = live.get(stride_key) or (max(live_off[f["name"]] for f in base["fields"] if f["offset"] < len(values) and f["name"] in live_off) + 1)
        out = []
        for i in range(0, len(values), bs):
            chunk, new = values[i:i + bs], [None] * ls
            for f in base["fields"]:
                if f["offset"] < len(chunk) and f["name"] in live_off:
                    new[live_off[f["name"]]] = chunk[f["offset"]]
            out += new
        return out

    def out(self, name, args):
        if name == "0" and args and isinstance(args[0], list):
            args = [self.remap(args[0], "add_players")] + args[1:]
        elif name == "k" and args and isinstance(args[0], list):
            args = [self.remap(args[0], "update_players")] + args[1:]
        elif name == "l" and args and isinstance(args[0], list):
            args = [self.remap(args[0], "sync_player", "none")] + args[1:]  # (one state: no stride)
        elif name == "3":
            base, live = (lay["args"] for lay in self.layout("kill_player"))
            new = [None] * self.doc["layouts"]["kill_player"]["argc"]
            for role, pos in base.items():
                if pos < len(args) and role in live:
                    new[live[role]] = args[pos]
            args = new
        return self.s2c.get(name, name), args

    def back(self, name):
        return self.c2s_back.get(name, name)


def next_build_doc(lose_field=None):
    """The committed build.json as a new Krunker build changes it (LiveBuild); lose_field: (layout, name) renamed
    away, a change the client cannot follow."""
    d = json.loads(json.dumps(COMMITTED_BUILD))
    d.update(ahkey=LIVE_AHKEY, ahkey_hex="0x%x" % LIVE_AHKEY, ahnum_step=LIVE_AHKEY & 0xff, build_version=LIVE_VERSION,
             client_version=LIVE_CLIENT)
    ren = {"packets_s2c": {"ADD_PLAYERS": "P0", "UPDATE_PLAYERS": "Pk", "SYNC_PLAYER": "Pl", "KILL_PLAYER": "P3"},
           "packets_c2s": {"ENTER_GAME": "Pen", "INPUT": "Pq", "PONG": "Pv", "LOAD": "Pld"}}
    for side, names in ren.items():
        for e in d[side]:
            e["wire"] = names.get(e["name"], e["wire"])
    d["packets_s2c"].append({"name": "SOME_NEW_PACKET", "wire": "snp"})
    add, k = d["layouts"]["add_players"], d["layouts"]["update_players"]
    add["stride"], add["server_stride"], k["stride"] = 56, 56, 15
    for lay, moves in ((add, {"sid": 53, "name": 54, "x": 55}), (k, {"sid": 13, "x": 14})):
        for f in lay["fields"]:
            f["offset"] = moves.get(f["name"], f["offset"])
    ka = d["layouts"]["kill_player"]["args"]
    ka["victim"], ka["killer"] = ka["killer"], ka["victim"]
    if lose_field:
        for f in d["layouts"][lose_field[0]]["fields"]:
            if f["name"] == lose_field[1]:
                f["name"] = lose_field[1] + "Renamed"
    return d


def publish(stub, doc, label):
    """Stage doc as tools/krunker/publish_build.py does and serve it from the stub's matchmaker."""
    path = os.path.join(WORK, label + "-build.json")
    with open(path, "w") as f:
        json.dump(doc, f)
    out = os.path.join(WORK, label + "-stage")
    with open(os.devnull, "w") as null:
        old, sys.stdout = sys.stdout, null
        try:
            m = publish_build.stage(path, out)
        finally:
            sys.stdout = old
    for rel in ("client/krunker-build.json", m["path"]):
        with open(os.path.join(out, rel), "rb") as f:
            stub.published["/updates/" + rel] = (200, f.read())
    return m


def build_url(stub):
    return "http://127.0.0.1:%d/updates/client/krunker-build.json" % stub.mm_port


# ---------------------------------------------------------------- the stubs

class Stub:
    def __init__(self, mm_port, ws_port, scenario="default", map_index=None, spawn=None):
        self.mm_port, self.ws_port = mm_port, ws_port
        self.scenario = scenario  # "sync": after "en", play the movement-lock / correction packets (sync_phases);
        #                           "spawn": after "en", our player at `spawn` and nothing correcting it (run_native_hq);
        #                           "hud": a CTF game, the score / kill / objective / end packets (hud_phases)
        #                           "world": a Domination game, the mode-objective / world-object packets (world_phases)
        self.map_index = MAP_INDEX if map_index is None else map_index  # init's mapIndex
        self.spawn = spawn or (4.0, 0.0, 4.0)
        self.frame_t = []         # time.monotonic() of every client frame (parallel to self.frames)
        self.sent = {}            # sync / hud: what the stub sent when (name -> time.monotonic())
        self.isn_restart = 0      # rounds: our respawn after Switching Sides: the client's next "q" may restart its isn
        self.k_paused = False     # rounds: no "k" while the server has every player inactive (Switching Sides)
        self.ended = False        # hud: "end" went out: the clock frames carry state 1 ("Next Round in")
        self.http_log = []      # (path, query list)
        self.ws_paths = []
        self.frames = []        # (name, args, ah) from the client
        self.errors = []
        self.proto_errors = []  # ahNum trailer / "q" sequencing violations seen on the wire
        self.timer_sent = []    # the match-timer ms values the stub broadcast (counts down)
        self.got_en = threading.Event()
        self.closed = threading.Event()
        self.issued = []          # validation tokens handed out
        self.used_captchas = set()
        self.bad_version_seeks = []  # dataQuery.v values refused as InvalidVersion
        self.captcha_ok = threading.Event()
        self.cpt = ALTCHA         # the game socket's "cpt" (ALTCHA_UNSOLVABLE: one this client cannot answer)
        self.loop = None
        self.httpd = None
        # the Find New Game runs (exclude): several lobbies, seeks without a captcha; seek-game answers
        # the asked game, else (like the live matchmaker, which takes ONE excludeGame) the busiest
        # lobby that is not excludeGame; list_fail: /game-list requests answered 503 (-1 = all)
        self.lobbies = None   # [(gameId, players)], busiest first
        self.list_fail = 0
        self.gone_after = {}  # the reload runs: gameId -> seeks answered before it is gone (InvalidGameId)
        self.list_requests = 0
        self.client_closed_t = None  # time.monotonic() when the client closed the game socket
        # the Krunker build the game server speaks and the matchmaker serves (LiveBuild; None = the compiled one),
        # and what /updates/... answers (path -> (status, bytes)): the published manifest + document
        self.live = None
        self.ahkey, self.build_version = AHKEY, BUILD_VERSION
        self.published = {}

    def speak(self, live):
        self.live = live
        self.ahkey, self.build_version = live.doc["ahkey"], live.doc["build_version"]

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
                if u.path in stub.published:  # the published Krunker build (tools/krunker/publish_build.py)
                    status, body = stub.published[u.path]
                    ctype = "application/json"
                elif u.path == "/generate-token":
                    tok = "tok_e2e_%d_0123456789abcdefghijklmnopqrstuvwxyz0123456789" % len(stub.issued)
                    stub.issued.append(tok)
                    body, ctype = tok.encode(), "text/plain"
                elif u.path == "/game-list" and stub.lobbies is not None:  # the exclude runs' lobbies
                    stub.list_requests += 1
                    if stub.list_fail < 0 or stub.list_requests <= stub.list_fail:
                        status, body, ctype = 503, b"unavailable", "text/plain"
                    else:
                        body, ctype = json.dumps({"success": True, "totalPlayerCount": 9, "games": [
                            [g, "de-fra", n, 8, {"i": "Kanji", "g": 0}, 240] for g, n in stub.lobbies]}).encode(), "application/json"
                elif u.path == "/game-list":  # the server browser's official lobbies (krunker_lobbies.c)
                    body, ctype = json.dumps({"success": True, "totalPlayerCount": 5, "games": [
                        [GAME_ID, "de-fra", 3, 8, {"i": MAP_NAME, "g": 0}, 240],
                        # its row must read Krunker's own name: the catalogue's alias would retitle it Burg (1.5.2)
                        [OLD_BURG_GAME, "de-fra", 2, 8, {"i": "Old_Burg", "g": 0}, 240]]}).encode(), "application/json"
                elif u.path == "/seek-game" and stub.lobbies is not None:
                    qd = dict(q)
                    game = qd.get("game") or next((g for g, _ in stub.lobbies if g != qd.get("excludeGame")), None)
                    try:
                        seek_v = json.loads(qd.get("dataQuery") or "{}").get("v")
                    except (ValueError, TypeError):
                        seek_v = None
                    if seek_v != stub.build_version:  # no server runs that build (the version gate below)
                        stub.bad_version_seeks.append(seek_v)
                        status, body = 400, json.dumps({"error": "InvalidVersion"}).encode()
                    elif game in stub.gone_after and stub.gone_after[game] <= 0:
                        status, body = 404, json.dumps({"error": "InvalidGameId"}).encode()  # the game ended
                    else:
                        if game in stub.gone_after:
                            stub.gone_after[game] -= 1
                        body = json.dumps({"host": "127.0.0.1", "port": stub.ws_port, "gameId": game, "clientId": CLIENT_ID,
                                           "data": {"i": MAP_NAME, "g": 0}}).encode()
                    ctype = "application/json"
                elif u.path == "/seek-game":
                    qd = dict(q)
                    cap, val = qd.get("captchaToken"), qd.get("validationToken")
                    # dataQuery is JSON.stringify({v: buildVersion}); the matchmaker/game infra only
                    # serves a build whose buildVersion matches, else the join is kicked as an invalid
                    # version. Parse v out so a client that sends the wrong/stale buildVersion is refused
                    # (this is the regression guard for the stale-krunker_build.h bug).
                    try:
                        seek_v = json.loads(qd.get("dataQuery") or "{}").get("v")
                    except (ValueError, TypeError):
                        seek_v = None
                    if not cap or cap in stub.used_captchas or val not in stub.issued:
                        status, body = 400, json.dumps({"error": "CaptchaRequired"}).encode()  # the live answer
                        if cap:
                            stub.used_captchas.add(cap)
                    elif seek_v != stub.build_version:
                        # captcha accepted, but the build the client presents is not the one we serve
                        stub.used_captchas.add(cap)
                        stub.bad_version_seeks.append(seek_v)
                        status, body = 400, json.dumps({"error": "InvalidVersion"}).encode()
                    else:
                        stub.used_captchas.add(cap)
                        stub.captcha_ok.set()
                        body = json.dumps({"host": "127.0.0.1", "port": stub.ws_port, "gameId": GAME_ID, "clientId": CLIENT_ID,
                                           "data": {"i": MAP_NAME, "g": 0}}).encode()
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
            if self.live:  # the stub's packets are written in the compiled build: say them in the live one
                name, args = self.live.out(name, list(args))
            await ws.send(frame(name, *args, ah=ah[0]))

        def player(sock, sid, name, x, z, y=0, team=0):
            e = [None] * 53
            e[0], e[1], e[2], e[3], e[4], e[5], e[6], e[7], e[8], e[9], e[18] = sock, sid, x, y, z, name, 0, None, 100, team, 0.0
            return e

        hud = self.scenario == "hud"
        ingame = self.scenario == "ingame"
        rounds = self.scenario == "rounds"
        actions = self.scenario == "actions"

        stop = asyncio.Event()

        async def ticker():
            # the match clock: like the live server (game.deob.js:191565) it owns gameTimer in ms,
            # decrements it, and broadcasts UPDATE_TIMER ("t", [formattedString, state0, rawMs]) only
            # when the formatted mm:ss string changes. formatTimer (:23893) floors the components.
            x, n = 0.0, 0
            t0 = time.monotonic()
            base_ms = 300000  # 5:00
            last_fmt = None
            while not stop.is_set():
                if self.scenario != "rounds":  # rounds: the bot stands at 0, 0, 0 in front of us (its Juggernaut marker)
                    x += 0.5
                if not self.k_paused:
                    await send("k", [BOT_SID, x, 0, 0, 90, 0, 0, 1, 0, 0, 0, None, 30], 10, int(time.time() * 1000) & 0xffffff)
                remain = max(0, base_ms - int((time.monotonic() - t0) * 1000))
                fmt = "%d:%02d" % (remain // 60000, (remain // 1000) % 60)
                if fmt != last_fmt:
                    last_fmt = fmt
                    self.timer_sent.append(remain)
                    await send("t", fmt, 1 if self.ended else 0, remain)
                n += 1
                if n % 10 == 0:
                    await send("pi")
                await asyncio.sleep(0.1)

        ah_exp = [0]
        last_q_isn = [None]

        async def reader():
            async for msg in ws:
                if isinstance(msg, str):
                    continue
                try:
                    name, args, trailer = unframe(msg)
                except Exception as e:  # noqa: BLE001
                    self.errors.append("undecodable client frame: %s" % e)
                    continue
                if self.live:  # the live build's wire names, back to the compiled ones the checks use
                    name = self.live.back(name)
                # the server validates the auth-byte stream: every frame except "cptR" advances
                # ahNum by AHKEY (game.deob.js:98214); a wrong trailer is a desync. Checked here on
                # the wire so a broken rotation or a dropped/reordered frame fails the test.
                if name != "cptR":
                    ah_exp[0] = (ah_exp[0] + self.ahkey) & 0xff
                if trailer != ah_exp[0]:
                    self.proto_errors.append("ahNum %s: trailer %d, expected %d (frame %d)" % (name, trailer, ah_exp[0], len(self.frames)))
                # "q" firstIsn must strictly advance (isn increments per input frame, never resets
                # without a respawn; the stub never kills our player) - a stall or rewind is a desync.
                if name == "q" and self.isn_restart:  # a new player of ours (rounds' respawn): a new input sequence
                    self.isn_restart = 0
                    last_q_isn[0] = None
                if name == "q" and len(args) > 1:
                    try:
                        isn = int(args[1])
                    except (TypeError, ValueError):
                        isn = None
                        self.proto_errors.append("q firstIsn not an int: %r" % (args[1],))
                    if isn is not None:
                        if last_q_isn[0] is not None and isn <= last_q_isn[0]:
                            self.proto_errors.append("q firstIsn %d did not advance past %d" % (isn, last_q_isn[0]))
                        last_q_isn[0] = isn
                self.frames.append((name, args, trailer))
                self.frame_t.append(time.monotonic())
                if name == "en":
                    self.got_en.set()
            stop.set()

        async def reader_safe():
            try:
                await reader()
            except websockets.ConnectionClosed:
                stop.set()
            self.client_closed_t = time.monotonic()  # the client closed its socket (the stub never does first)

        rd = asyncio.ensure_future(reader_safe())
        try:
            await send("io-init", "sockME")
            await send("load")
            for _ in range(150):
                if any(f[0] == "load" for f in self.frames):
                    break
                await asyncio.sleep(0.1)
            if self.scenario == "world":  # Domination (25) on Kanji: round wins (arg 3), team scores, syncData doms / spwns / dest
                await send("init", self.map_index, 25, 0, [[1, 1], [2, 0]], [[1, 40], [2, 25]], None, {}, None, 0, None, None,
                           {"gor": 1, "roundC": 2, "lockT": 0, "doms": [0, 2, 1, None, 2, None], "spwns": [[0, 601, -330.0, -143.0, -40.0, 2, {}]],
                            "dest": [4], "obj": [None, 0]},
                           None, False, True, False, None, None, 0)
            elif actions:  # TDM (1), a ranked official game ([14] isOfficial, [16] isRanked): team chat, pings
                await send("init", self.map_index, 1, 0, [[1, 0], [2, 0]], [[1, 0, None], [2, 0, None]], None, {}, None, 0, None, None,
                           {"gor": True}, None, False, True, False, True, None, 0)
            elif hud:  # CTF (3, modes.c MODE_ID_CTF): the joiner's team scores + flag lines, syncData's flags (uids 40 / 41, map order)
                await send("init", self.map_index, 3, 0, [[1, 0], [2, 0]], [[1, 3, "AT BASE"], [2, 1, "AT BASE"]], None, {}, None, 0, None, None,
                           {"gor": True, "flg": [[40, 0.0, 0.0, 0.0, 0, None], [41, 20.0, 0.0, 20.0, 0, None]], "obj": [None, 0]},
                           None, False, True, False, None, None, 0)
            elif rounds:  # Hardpoint (2), arg 14 isOfficial: the end table's "reward" column
                await send("init", self.map_index, 2, 0, [[1, 0], [2, 0]], [[1, 0], [2, 0]], None, {}, None, 0, None, None, {"gor": True},
                           None, False, True, False, None, None, 0)
            elif self.scenario == "popups":  # TDM (1): "rds" names the leading team
                await send("init", self.map_index, 1, 0, [[1, 0], [2, 0]], [[1, 0], [2, 0]], None, {}, None, 0, None, None, {"gor": True},
                           None, False, True, False, None, None, 0)
            elif ingame:  # TDM (1); arg 5: the bot hosts this (custom) lobby; arg 14 isOfficial
                await send("init", self.map_index, 1, 0, [[1, 0], [2, 0]], [[1, 0], [2, 0]], "sockBOT", {}, None, 0, None, None, {"gor": True},
                           None, False, True, False, None, None, 0)
            else:
                await send("init", self.map_index, 0, 0, None, None, None, {}, None, 0, None, {"gor": True}, None, False, True, False, None, None, 0)
            if ingame:  # the bot's account: alias AceBot (its name "acctBot" is never shown), class 1, level 33, clan CLN, featured
                bot = player("sockBOT", BOT_SID, "acctBot", 0.0, 0.0, team=2)
                bot[6], bot[10], bot[11], bot[25], bot[28], bot[31] = 1, 33, "CLN", True, "AceBot", 777
                await send("0", bot)
            else:
                await send("0", player("sockBOT", BOT_SID, "StubBot", 0.0, 0.0, team=2 if hud or actions or rounds else 0))
            for nonce, ct, _ in CC_CASES:
                await send("cc", nonce, None, ct)
            await send("cpt", self.cpt)
            tk = asyncio.ensure_future(ticker())
            for _ in range(900):
                if self.got_en.is_set() or stop.is_set() or self.scenario == "joinend":
                    break
                await asyncio.sleep(0.1)
            if self.scenario == "joinend":
                await self.joinend_phases(send, stop, player)
            elif self.got_en.is_set() and self.scenario == "spawn":
                # the live server's spawn: our player where the map's spawn is (x, y, z); no "l" corrects it,
                # so where the client stands and walks is its own simulation of this map
                await send("frs")
                sx, sy, sz = self.spawn
                await send("0", player("sockME", MY_SID, "Guest_9", sx, sz, sy))
                self.sent["spawn"] = time.monotonic()
                while not stop.is_set():
                    await asyncio.sleep(0.2)
            elif self.got_en.is_set() and self.scenario == "sync":
                await send("frs")
                await send("0", player("sockME", MY_SID, "Guest_9", 4.0, 4.0))
                await self.sync_phases(send, stop)
            elif self.got_en.is_set() and self.scenario == "kick":
                # run_native_kick: in the game, the server's "error" (an idle kick); the socket stays open:
                # the client must close it itself
                await send("frs")
                await send("0", player("sockME", MY_SID, "Guest_9", 4.0, 4.0))
                await asyncio.sleep(3.0)
                self.sent["error"] = time.monotonic()
                await send("error", KICK_TEXT, KICK_CODE)
                for _ in range(150):
                    if stop.is_set():
                        break
                    await asyncio.sleep(0.1)
            elif self.got_en.is_set() and rounds:
                await send("frs")
                await send("0", player("sockME", MY_SID, "Guest_9", 0.0, 20.0, team=1))
                await self.rounds_phases(send, stop, player)
            elif self.got_en.is_set() and self.scenario == "popups":
                await send("frs")
                await send("0", player("sockME", MY_SID, "Guest_9", 4.0, 4.0, team=1))
                await self.popups_phases(send, stop)
            elif self.got_en.is_set() and ingame:
                await send("frs")
                await send("0", player("sockME", MY_SID, "Guest_9", 4.0, 4.0, team=1))
                await self.ingame_phases(send, stop)
            elif self.got_en.is_set() and self.scenario == "fx":
                await send("frs")
                await send("0", player("sockME", MY_SID, "Guest_9", 4.0, 4.0))
                await self.fx_phases(send, stop)
            elif self.got_en.is_set() and self.scenario == "world":
                await send("frs")
                x, y, z = self.spawn
                await send("0", player("sockME", MY_SID, "Guest_9", x, z, y, team=1))
                await self.world_phases(send, stop)
            elif self.got_en.is_set() and self.scenario == "social":
                await self.social_phases(send, stop, player)
            elif self.got_en.is_set() and self.scenario == "voice":
                await send("frs")
                await send("0", player("sockME", MY_SID, "Guest_9", 4.0, 4.0))
                await self.voice_phases(send, stop)
            elif self.got_en.is_set() and actions:
                await send("frs")
                sx, sy, sz = self.spawn
                await send("0", player("sockME", MY_SID, "Guest_9", sx, sz, sy, team=1))
                await self.actions_phases(send, stop)
            elif self.got_en.is_set() and hud:
                await send("frs")
                await send("0", player("sockME", MY_SID, "Guest_9", 4.0, 4.0, team=1))
                await self.hud_phases(send, stop, player)
            elif self.got_en.is_set():
                await send("frs")
                await send("0", player("sockME", MY_SID, "Guest_9", 4.0, 4.0))
                await send("0", player("sockDEAD", DEAD_SID, "DeadBot", 8.0, 8.0))
                await send("ch", -1, "StubBot", "hello from the stub", 0)
                last_isn = 0
                while not stop.is_set():
                    qs = [f for f in self.frames if f[0] == "q"]
                    if qs:
                        last_isn = int(qs[-1][1][1])
                    sync = [last_isn, 42, 4.0, 0, 4.0, 0, 0, 0, 0.0, True, False, False, 0, False, False, 0, 0, 0, 0, True, False, 0, 0, None, 0]
                    await send("l", sync)
                    # killfeed-name regression: frames are processed in order, so DeadBot's "0" lands
                    # before its "3". Re-killing each tick keeps a fresh row in #killFeed throughout
                    # the in-game window. A void death (killer -1) of a synced player shows its name;
                    # a "3" for the never-added UNKNOWN_SID is a join race the client must drop, never
                    # a "<unknown> committed ..." row.
                    await send("3", DEAD_SID, 1, -1, None, {}, False)
                    await send("3", UNKNOWN_SID, 1, -1, None, {}, False)
                    await asyncio.sleep(1.0)
            await rd
            tk.cancel()
        except (websockets.ConnectionClosed, asyncio.CancelledError):
            pass
        finally:
            stop.set()
            self.closed.set()

    async def sync_phases(self, send, stop):
        """The packet-audit clusters A / B against a live client (run_native_sync). Paced by the client's
        own "q" frames: the lock starts once W is held, the teleport once W is let go. Throughout, an
        "l" every 0.4 s puts us where the server has us (pos, its xDire) and acks the last isn."""
        pos = [4.0, 0.0, 4.0]
        yaw = [0.0]

        def q_moves(since):  # the "-4" (moveDir) changes of the "q" frames received after `since`
            out = []
            for (name, args, _), t in zip(list(self.frames), list(self.frame_t)):
                if name == "q" and t >= since and len(args) > 5 and isinstance(args[5], dict):
                    out += [v for k, v in args[5].items() if k.endswith("-4")]
            return out

        async def hold(seconds, until=None):
            end = time.monotonic() + seconds
            while time.monotonic() < end and not stop.is_set() and not (until and until()):
                qs = [f for f in self.frames if f[0] == "q"]
                isn = int(qs[-1][1][1]) if qs else 0
                await send("l", [isn, 42, pos[0], pos[1], pos[2], 0, 0, 0, yaw[0], True, False, False, 0, False, False, 0, 0, 0, 0, True,
                                 False, 0, 0, None, 0])
                await asyncio.sleep(0.4)

        def mark(name):
            self.sent[name] = time.monotonic()

        t0 = time.monotonic()
        await hold(12, until=lambda: any(v >= 0 for v in q_moves(t0)))  # W held
        await hold(0.6)
        pos[:] = [6.0, 0.0, 6.0]
        mark("lm")
        await send("lm", 6.0, 0.0, 6.0, 0.0)  # LOCK_MOVE: moveLock + resetPosition there
        await hold(3.0)
        mark("ulm")
        await send("ulm")
        await hold(10, until=lambda: -1 in q_moves(self.sent["ulm"] + 0.3))  # moving again, then W let go
        await hold(0.8)
        pos[:] = [30.0, 0.0, -12.0]
        yaw[0] = -math.pi / 2  # changePosition's xDire = -(dir + 1) * PI / 2: the server's "l" says so from now on
        mark("chgp")
        await send("chgp", "sockME", 30.0, 0.0, -12.0, True, True, 0)  # findById: the socket id; dir 0 -> yaw -PI/2
        await hold(2.0)
        mark("tm")
        await send("tm", MY_SID, 2)
        await send("wep", 5)
        await send("00", ["sockBOT", "RenamedBot", ""], True)
        await hold(2.0)
        # the rest of clusters A / B, at 30,-12 (the chgp spot, where every "l" keeps us): W is held
        # again (the script's second "down W") for "lock" and "frz", which must hold us still
        t1 = time.monotonic()
        await hold(12, until=lambda: any(v >= 0 for v in q_moves(t1)))
        await hold(0.8)
        mark("lock")
        await send("lock", True)  # LOCK_PLAYER: window.locked, no input
        await hold(2.4)
        mark("unlock")
        await send("lock", False)
        await hold(1.2)
        mark("frz")
        await send("frz", True)  # SET_FROZEN: !defaultMovement / !defaultVelocity
        await hold(2.4)
        mark("unfrz")
        await send("frz", False)
        await hold(1.2)
        mark("buffs")
        await send("noc", True)
        await send("unl", True)
        await send("spb", 2.5, False)
        await send("sic", True)
        await send("jg", MY_SID, True)
        await send("busy", MY_SID, True)
        await send("chrg", MY_SID, 15000)
        await send("gsc", 3)
        await hold(1.5)
        mark("spk")  # SET_PLAYER_KEY undoes three of them, the packets the rest
        await send("spk", "noclip", False)
        await send("spk", "speedBoost", 0)
        await send("spk", "unlimitedAmmo", False)
        await send("jg", MY_SID, False)
        await send("sic", False)
        await send("busy", MY_SID, False)
        await hold(1.5)
        mark("rg")  # REGEN_PLAYER with a class change: Hunter (1), secIndex 2
        await send("rg", MY_SID, True, [-1] * 14 + [2, 1, -1, -1, -1])
        await hold(1.5)
        mark("h")
        await send("h", 2000, None)  # above maxHealth: it rises
        await hold(1.2)
        mark("hreset")
        await send("h", 120, None, None, None, None, True)  # resetMax: it falls to 120
        await hold(1.2)
        mark("rfl")
        await send("rfl")
        await hold(1.0)
        mark("00")
        await send("00", [], False)  # keepOpen false: the client closes its socket
        for _ in range(60):
            if stop.is_set():
                break
            await asyncio.sleep(0.1)
        if stop.is_set():
            mark("closed")


    async def hud_phases(self, send, stop, player):
        """Packet-audit clusters C / D against a live client (run_native_hud): scores, kills, the
        leaderboard, ammo, CTF flags, the action feed, localized chat, then the end of the match. An
        "l" every 0.4 s keeps us where we spawned and acks the last isn."""
        async def hold(seconds):
            end = time.monotonic() + seconds
            while time.monotonic() < end and not stop.is_set():
                qs = [f for f in self.frames if f[0] == "q"]
                isn = int(qs[-1][1][1]) if qs else 0
                await send("l", [isn, 42, 4.0, 0, 4.0, 0, 0, 0, 0.0, True, False, False, 0, False, False, 0, 0, 0, 0, True, False, 0, 0, None, 0])
                await asyncio.sleep(0.4)

        await hold(3.0)
        self.sent["hud"] = time.monotonic()
        # the round popup goes first: the medal queue shows one medal at a time, and the end screen hides the HUD.
        # Switching Sides makes every player inactive (v10): like the live server, ours comes back with a "0"
        # once the popup had its time (a spawn clears the medal queue the popup rides here)
        await send("rds", 2, False, 1, {"1": 2, "2": 1})  # UPDATE_ROUNDS_DIS: Switching Sides, Alpha leads
        await asyncio.sleep(2.5)
        self.isn_restart = 1
        await send("0", player("sockME", MY_SID, "Guest_9", 4.0, 4.0, team=1))
        await send("ts", 1, 5)                        # TEAM_SCORE: team 1 = 5
        await send("5", 120, False, False, 0)         # GET_SCORE: +120
        await send("6", [9, 100, 0, 1], 0, 3, 1)      # GET_KILL: Headshot +100, totalKills 3
        await send("3", BOT_SID, 1, MY_SID, None, {"wId": 0}, False)  # KILL_PLAYER: the bot, by us
        await send("7", [MY_SID, 1, 220, BOT_SID, 0, 40])  # UPDATE_LEADERS, CTF's [caps, score]
        await send("ulb", 1, ["Alice", 500, "Bob", 1.5])   # UPDATE_LEADER_BOARD: a map board's rows
        await send("lv", 3)                           # UPDATE_LIVES
        await send("uamo", [17, None], [])            # UPDATE_AMMO: 17 in the primary
        await send("st", 2, 1)                        # KILL_STREAK: our streak 2, one
        await send("kst", BOT_SID, 5)                 # KILL_STREAK_M: the bot's 5 kill streak
        await send("wstk", 3, 4)                      # UPDATE_WEAPON_STREAK
        await send("10")                              # GET_ASSIST: the Assist medal
        await send("am", [24, 20, 0, 1])              # ADD_MEDAL: Pickup +20
        await send("krt", 3, 0)                       # UPDATE_KR_TAGS: 3 dog tags
        await send("ufi", 2, "Guest_9")               # team 2's flag line: carried by us
        await send("ufl", 40, 4.0, 10.0, 4.0, MY_SID)  # UPDATE_FLAG: team 2's flag (uid 40) carried by us
        await send("ufp", 41, 20.0, 1.0, 20.0)        # UPDATE_FLAG_PING: team 1's flag (uid 41)
        await send("ac", MY_SID, None, "got the Flag")  # ACTION_FEED
        await send("chi", -1, None, "modes.round.win", 2, "#ff0000")
        await send("chi", 0, None, ["player.killstreak", "Somebody", 7], 1)
        await send("obj", 4, 60000, True)             # SET_OBJECTIVE
        await send("uobj", 4, 2)                      # UPDATE_OBJECTIVE: on zone 2
        await send("zn", [0.0, 0.0, 10.0, 10.0])      # UPDATE_ZONE
        await hold(12.0)  # the medal queue shows one medal at a time (about 1.5 s each): the end screen hides the HUD
        self.sent["end"] = time.monotonic()
        self.ended = True
        await send("end", 0, True, {"ed": [MY_SID, ["Guest_9", "Guest_9"], 220, 3, 0, 1, "<span style='color:#F8C55C'>+12</span> KR",
                                           BOT_SID, ["StubBot", "StubBot"], 40, 0, 1, 0, "NONE"],
                                    "tms": [[1, 5, None], [2, 1, None]], "mdls": [], "mts": [], "vo": {}, "modeIndex": 3}, 1, 3, 12, None, {})
        await hold(12.0)
        self.sent["done"] = time.monotonic()

    async def rounds_phases(self, send, stop, player):
        """official-ingame-rounds (run_native_rounds): the bot (sid 5, an enemy at 0, 0, 0 in front of us at 0, 0, 20)
        becomes a Juggernaut (its marker), a "Reward" medal with its "12 KR", then Switching Sides ("rds": every
        player inactive, no "q" from us until our next "0" 2 s later), then the end (Hardpoint's dmg column, a row
        of sid 31 who left before it). An "l" every 0.4 s keeps us at 0, 0, 20 and acks the last isn."""
        async def hold(seconds):
            end = time.monotonic() + seconds
            while time.monotonic() < end and not stop.is_set():
                qs = [f for f in self.frames if f[0] == "q"]
                isn = int(qs[-1][1][1]) if qs else 0
                await send("l", [isn, 42, 0.0, 0, 20.0, 0, 0, 0, 0.0, True, False, False, 0, False, False, 0, 0, 0, 0, True, False, 0, 0, None, 0])
                await asyncio.sleep(0.4)

        await hold(3.0)
        self.sent["jg"] = time.monotonic()
        await send("jg", BOT_SID, True)                  # SET_JUGGERNAUT: the enemy marker (jugg_2)
        await send("am", ["Reward", "12 KR", 0, 1])     # ADD_MEDAL: "Reward" / "12 KR"
        await hold(4.0)
        self.k_paused = True
        self.sent["rds"] = time.monotonic()
        await send("rds", 2, False, 1, {"1": 1, "2": 0})  # Switching Sides: Alpha leads
        await asyncio.sleep(2.0)
        self.sent["respawn"] = time.monotonic()
        self.isn_restart = 1
        await send("0", player("sockME", MY_SID, "Guest_9", 0.0, 20.0, team=2) + player("sockBOT", BOT_SID, "StubBot", 0.0, 0.0, team=1))
        self.k_paused = False
        await hold(3.0)
        self.sent["end"] = time.monotonic()
        self.ended = True
        await send("end", 0, True, {"ed": [MY_SID, ["Guest_9", "Guest_9"], 300, 5, 2, 40, 812, "<span style='color:#F8C55C'>+12</span> KR",
                                           31, ["acct31", "Gone31"], 150, 3, 4, 10, 377, "NONE",
                                           BOT_SID, ["StubBot", "StubBot"], 90, 1, 5, 0, 120, "NONE"],
                                    "tms": [[1, 5, None], [2, 1, None]], "mdls": [], "mts": [], "vo": {}, "modeIndex": 2}, 1, 3, 12, None, {})
        await hold(10.0)
        self.sent["done"] = time.monotonic()

    async def popups_phases(self, send, stop):
        """v10's textPopup / gmsg countdown (run_native_popups): "tx", "stlkpd", "gmsg" with a countdown
        and a [title, sub], then "rds" (Switching Sides + the lead). An "l" every 0.4 s keeps us at 4, 0, 4."""
        async def hold(seconds):
            end = time.monotonic() + seconds
            while time.monotonic() < end and not stop.is_set():
                qs = [f for f in self.frames if f[0] == "q"]
                isn = int(qs[-1][1][1]) if qs else 0
                await send("l", [isn, 42, 4.0, 0, 4.0, 0, 0, 0, 0.0, True, False, False, 0, False, False, 0, 0, 0, 0, True, False, 0, 0, None, 0])
                await asyncio.sleep(0.4)

        await hold(3.0)
        self.sent["tx"] = time.monotonic()
        await send("tx", "Overtime", "+60 Seconds")  # TEXT_POPUP
        await hold(2.6)
        self.sent["stlkpd"] = time.monotonic()
        await send("stlkpd")  # STALKER_PICKED
        await hold(2.6)
        self.sent["gmsg"] = time.monotonic()
        await send("gmsg", "infection in", "00:03", 3, ["Bomb", "Planted"])  # countdown 3 + a text popup
        await hold(2.6)
        self.sent["rds"] = time.monotonic()
        await send("rds", 2, False, 1, {"1": 1, "2": 0})  # Switching Sides, Alpha leads (silent textPopup)
        await hold(3.0)
        self.sent["done"] = time.monotonic()

    async def ingame_phases(self, send, stop):
        """official-ingame (run_native_ingame): names and account fields, the kill feed's v10 icons,
        hideFeed, team chat, the host line, our own death card from deathStats. An "l" every 0.4 s keeps
        us where we spawned and acks the last isn."""
        async def hold(seconds):
            end = time.monotonic() + seconds
            while time.monotonic() < end and not stop.is_set():
                qs = [f for f in self.frames if f[0] == "q"]
                isn = int(qs[-1][1][1]) if qs else 0
                await send("l", [isn, 42, 4.0, 0, 4.0, 0, 0, 0, 0.0, True, False, False, 0, False, False, 0, 0, 0, 0, True, False, 0, 0, None, 0])
                await asyncio.sleep(0.4)

        await hold(3.0)
        self.sent["ingame"] = time.monotonic()
        await send("7", [MY_SID, 10, BOT_SID, 50])  # UPDATE_LEADERS (TDM: score): the board lists the alias and clan
        await send("ch", 1, "AceBot", "team push b", 0, None, None, 777)  # a team-tab line
        await send("ch", 0, "AceBot", "global hello", 0, None, None, 777)
        # kills of the bot by us, each in the feed for 4 s: a wallbang headshot, a hidden one, streak 2, a thrown melee
        await send("3", BOT_SID, 1, MY_SID, None, {"wId": 1, "wb": True, "hs": True}, False)
        await send("3", BOT_SID, 2, MY_SID, None, {"stk": 3}, True)
        await send("3", BOT_SID, 3, MY_SID, None, {"stk": 2}, False)
        await send("3", BOT_SID, 4, MY_SID, None, {"wId": 12, "pj": True}, False)
        await send("4", BOT_SID, 24.1, False, False, False)  # DO_DAMAGE: 25 on the damage number
        await send("uh", "sockME", False)  # UPDATE_HOST: "You are now Host!"
        await hold(3.0)
        self.sent["death"] = time.monotonic()
        # the bot kills us: deathStats [weapon, killer class stat, [pcStat, value], [alive ms, shots, accuracy, kills, score]]
        await send("3", MY_SID, 1, BOT_SID, [1, 1234, None, [61000, 40, "37.5%", 4, 350]], {"wId": 1}, False)
        await hold(6.0)
        self.sent["done"] = time.monotonic()

    async def fx_phases(self, send, stop):
        """Combat visuals / sounds (run_native_fx): the stub bot's shot ("9" with an impact + its "s" gun
        sound by index), a rocket ("pr", then "pre" on impact), an "ex", a 2D "so", an "n", a "shk". An
        "l" every 0.4 s keeps us at 4, 0, 4 and acks the last isn."""
        async def hold(seconds):
            end = time.monotonic() + seconds
            while time.monotonic() < end and not stop.is_set():
                qs = [f for f in self.frames if f[0] == "q"]
                isn = int(qs[-1][1][1]) if qs else 0
                await send("l", [isn, 42, 4.0, 0, 4.0, 0, 0, 0, 0.0, True, False, False, 0, False, False, 0, 0, 0, 0, True, False, 0, 0, None, 0])
                await asyncio.sleep(0.4)

        await hold(3.0)
        self.sent["fx"] = time.monotonic()
        # SHOW_TRACER: the bot's ray hits a wall at 8, 3, 8 (impact yaw / pitch), within 200 of us
        await send("9", [BOT_SID, 8.0, 3.0, 8.0, 1.57, 0.0, 1, 0.0, 0.0, 11.0, 0.0, 0, 0])
        await send("s", 5, BOT_SID, 0, 0, 0, 0.85, 1.0)  # PLAYER_SOUND: sounds[5] = weapon_1, the shot
        # SHOW_PROJECTILE: the bot's rocket (weapon 8), server sid 300, toward +z; its END_PROJECTILE impact
        await send("pr", 0.0, 6.0, 0.0, math.pi, 0.0, 8, 300, None, None, BOT_SID)
        await hold(0.4)
        await send("pre", 300, 0, 0.0, 6.0, 20.0, 1)
        await send("ex", 6, 1, 6, 30, 3, 2)              # EXPLOSION
        await send("so", "cheer_0", 0.5, 1)             # PLAY_SOUND, 2D
        await send("n", 0)                              # NUKE_UPDATE: the siren
        await send("shk", 0.5)                          # SHAKE
        await hold(3.0)
        self.sent["done"] = time.monotonic()
        await hold(2.0)

    async def world_phases(self, send, stop):
        """krunker_link_world.c against a live client (run_native_world): Domination zones, spawnables, a
        destroyed object, game messages, the warmup lockout, a text popup, a loadout, an AI and an airdrop.
        An "l" every 0.4 s keeps us at the spawn and acks the last isn."""
        x, y, z = self.spawn

        async def hold(seconds):
            end = time.monotonic() + seconds
            while time.monotonic() < end and not stop.is_set():
                qs = [f for f in self.frames if f[0] == "q"]
                isn = int(qs[-1][1][1]) if qs else 0
                await send("l", [isn, 42, x, y, z, 0, 0, 0, 0.0, True, False, False, 0, False, False, 0, 0, 0, 0, True, False, 0, 0, None, 0])
                await asyncio.sleep(0.4)

        await hold(3.0)
        self.sent["world"] = time.monotonic()
        await send("gmsg", "wt")                                   # player.waiting
        await hold(2.0)
        await send("gmsg", "wt", "01:00")                          # player.waiting + the suffix
        await hold(2.0)
        await send("lkt", 90500)                                   # UPDATE_LOCKOUT_TIM: "warmup starts in  01:30"
        await hold(2.0)
        self.sent["objects"] = time.monotonic()
        await send("uct", [0, None, 2500, 1, [1], 1, 1, 2, 0, None, [], 1])  # zone A: team 1 securing; zone B: team 2's
        await send("cct", 2, 1)                                    # CAPTURE_CONTROL: zone C to team 1, "Zone C Captured"
        await send("crsp", 0, 602, -320.0, -143.0, -50.0, 1, {})   # CREATE_SPAWNABLE: a second tag
        await send("do", 3)                                        # DESTROY_GAMEOBJECT
        await send("tx", "Overtime", "+60 Seconds")                # TEXT_POPUP
        await send("inat", MY_SID, [3, None, 1], 0, 0, None, None, None, None, [7])  # PLAYER_INTERACTIONS: our loadout
        await send("aai", [700, 0, "Zombie", -330.0, -143.0, -60.0, {"hasAnim": 0, "team": 2, "health": 100, "asset": 0, "src": "zombie_0"}])
        await send("ai", [700, -330.0, -143.0, -60.0, 0, 0, 100, 1, 2])
        await send("ad", [1, 200, -300.0, 300.0, -30.0, 0])        # SYNC_AIRDROP
        await hold(4.0)
        self.sent["dispose"] = time.monotonic()
        await send("dsp", 601)                                     # DISPOSE_SPAWNABLE: the init's tag goes
        await send("rai", 700)                                     # REMOVE_BY_SID
        await send("rad", 1)                                       # REMOVE_AIRDROP_BY_SID
        await send("gmsg")                                         # cleared
        await hold(4.0)
        self.sent["done"] = time.monotonic()

    ACTION_MTS = ["0,1", "1,43,Bastion,0", "3,7"]  # endData.mts: two public options and an official one with its name

    async def actions_phases(self, send, stop):
        """Player actions in an official game (run_native_actions): the client's team chat, streak, spray, class
        pick and ping go out in v10's shapes; the stub's spray and pings show; a "start" with spectating puts the
        client in the spectator cam ("fpSp" back); "end" with mts / vo shows the vote cards, a click sends
        "maVote" with the option's own string and "mv" moves the totals. Paced by the client's own frames."""
        async def hold(seconds, until=None):
            end = time.monotonic() + seconds
            while time.monotonic() < end and not stop.is_set() and not (until and until()):
                qs = [f for f in self.frames if f[0] == "q"]
                isn = int(qs[-1][1][1]) if qs else 0
                await send("l", [isn, 42, sx, sy, sz, 0, 0, 0, 0.0, True, False, False, 0, False, False, 0, 0, 0, 0, True, False, 0, 0, None, 0])
                await asyncio.sleep(0.4)

        sx, sy, sz = self.spawn

        def got(name):
            return any(f[0] == name for f in self.frames)

        await hold(1.0)
        self.sent["actions"] = time.monotonic()
        await send("sp", "%d_0" % BOT_SID, 2490, sx + 4.0, sy + 2.0, sz + 4.0, 0, 0, 0, None)  # ADD_SPRAY: the bot's spray
        # MARK_POSITION: pings around us (one per key) until our own ping comes, so one is in view
        t0 = time.monotonic()
        while not stop.is_set() and not got("mpos") and time.monotonic() - t0 < 7:
            for k, (dx, dz) in enumerate(((0, -40), (40, 0), (0, 40), (-40, 0))):
                await send("mpos", "sockP%d" % k, {"x": sx + dx, "y": sy, "z": sz + dz})
            await hold(1.5)
        # START_GAME with spectating: the client spectates, picks the bot ("1") and goes free cam ("F")
        self.sent["spectate"] = time.monotonic()
        await send("start", "4:00", 0, True, 0, None)
        await hold(10, until=lambda: sum(1 for f in self.frames if f[0] == "fpSp") >= 2)
        await hold(1.0)
        self.sent["end"] = time.monotonic()
        self.ended = True
        vo = {"sockA": ["1,43,Bastion,0", 2], "sockB": ["0,1", 1]}
        await send("end", 0, True, {"ed": [MY_SID, ["Guest_9", "Guest_9"], 0, 0, 0, 0, BOT_SID, ["StubBot", "StubBot"], 0, 0, 0, 0],
                                    "tms": [[1, 0, None], [2, 0, None]], "mdls": [], "mts": self.ACTION_MTS, "vo": vo, "modeIndex": 1},
                   1, 1, 12, None, {})
        await hold(25, until=lambda: got("maVote"))
        votes = [f[1][0] for f in self.frames if f[0] == "maVote" and f[1]]
        if votes:  # updMatchVote: our pick counted (the server echoes it), the others stay
            vo["sockME"] = [votes[-1], 1]
            self.sent["mv"] = time.monotonic()
            await send("mv", vo, self.ACTION_MTS)
        await hold(6.0)
        self.sent["done"] = time.monotonic()

    async def voice_phases(self, send, stop):
        """official-ingame-voice (run_native_voice): the client speaks (a tone as its microphone, V held):
        its clips arrive as "vc" [dataURL, channel, micVolume, micQuality]; the first one goes back to it
        as the bot's clip ("vc" [dataURL, alias, sid, channel, volume, isFinal, accountId]) on channel -1
        (every tab), 0 (ours: global) and 1 (the team tab: not ours, dropped). An "l" every 0.4 s keeps
        us at 4, 0, 4 and acks the last isn."""
        async def hold(seconds, until=None):
            end = time.monotonic() + seconds
            while time.monotonic() < end and not stop.is_set() and not (until and until()):
                qs = [f for f in self.frames if f[0] == "q"]
                isn = int(qs[-1][1][1]) if qs else 0
                await send("l", [isn, 42, 4.0, 0, 4.0, 0, 0, 0, 0.0, True, False, False, 0, False, False, 0, 0, 0, 0, True, False, 0, 0, None, 0])
                await asyncio.sleep(0.4)

        def clips():
            return [f[1] for f in list(self.frames) if f[0] == "vc"]

        await hold(20.0, until=lambda: clips())
        got = clips()
        if not got:
            return
        self.sent["vc_out"] = time.monotonic()
        url = got[0][0]
        await hold(1.0)
        self.sent["vc_in"] = time.monotonic()
        await send("vc", url, "StubBot", BOT_SID, -1, 0.8, False, 0)
        await hold(1.5)
        await send("vc", url, "StubBot", BOT_SID, 0, 0.8, False, 0)
        await hold(1.5)
        await send("vc", url, "StubBot", BOT_SID, 1, 0.8, False, 0)
        await hold(3.0)
        self.sent["done"] = time.monotonic()

    async def social_phases(self, send, stop, player):
        """official-ingame-social (run_native_social): the first "en" is refused ("fe": the menu and a red
        line), the second spawns us; then the account signs in ("a"), a forfeit vote opens / takes our vote /
        ends ("vf"), the bot reconnects under a new socket id ("prc") and finds an item ("skinF"), we find
        one ("skin")."""
        self.sent["fe"] = time.monotonic()
        await send("fe", "Server Full")  # FAILED_TO_ENDER
        for _ in range(300):  # the player plays again (the script's boot `play` retries until spawned)
            if stop.is_set() or sum(1 for f in self.frames if f[0] == "en") >= 2:
                break
            await asyncio.sleep(0.1)
        self.sent["en2"] = time.monotonic()
        await send("frs")
        await send("0", player("sockME", MY_SID, "Guest_9", 4.0, 4.0))
        await send("a", None, 4242, "StubAcct", {"player_score": 100})  # ACCOUNT_RESPONSE: signed in (vf needs it)

        async def hold(seconds):
            end = time.monotonic() + seconds
            while time.monotonic() < end and not stop.is_set():
                qs = [f for f in self.frames if f[0] == "q"]
                isn = int(qs[-1][1][1]) if qs else 0
                await send("l", [isn, 42, 4.0, 0, 4.0, 0, 0, 0, 0.0, True, False, False, 0, False, False, 0, 0, 0, 0, True, False, 0, 0, None, 0])
                await asyncio.sleep(0.4)

        await hold(2.0)
        self.sent["vf"] = time.monotonic()
        await send("vf", None, 1, 3, True, "StubBot")  # opened by the bot
        await hold(3.0)
        await send("vf", "Guest_9", 2, 3)              # our vote: "Vote Submitted - 2/3"
        await hold(2.0)
        self.sent["vfend"] = time.monotonic()
        await send("vf", None)                          # ended
        await send("prc", "sockBOT", "sockBOT2")       # PLAYER_RECONNECTED
        await send("skinF", "sockBOT2", 1)              # the bot found store item 1
        await send("skin", 2)                           # we found store item 2
        await hold(4.0)
        self.sent["done"] = time.monotonic()

    async def joinend_phases(self, send, stop, player):
        """run_native_join_end: the lobby's round is over when we join. Our "en" (Play) enters nothing:
        the server answers it with the joiner's end screen, "end" type 1 (didWin -1, the end table, v10's
        end handler shows it with its results at once, v10.js:219715), and its clock says "Next Round in"
        ("t" state 1). The next round's "init" ends it; then "en" spawns us ("frs")."""
        for _ in range(60):  # Play, or 6 s on the menu
            if self.got_en.is_set() or stop.is_set():
                break
            await asyncio.sleep(0.1)
        self.ended = True
        self.sent["end"] = time.monotonic()
        await send("end", 1, -1, {"ed": [BOT_SID, ["StubBot", "StubBot"], 40, 2, 1, "NONE"], "tms": None, "mdls": [], "mts": [], "vo": {},
                                  "modeIndex": 0}, -1, 1, 0, None, {})
        for _ in range(100):  # the end screen, 10 s
            if stop.is_set():
                return
            await asyncio.sleep(0.1)
        self.ended = False
        self.got_en.clear()
        self.sent["init2"] = time.monotonic()
        await send("init", self.map_index, 0, 0, None, None, None, {}, None, 0, None, {"gor": True}, None, False, True, False, None, None, 0)
        await send("0", player("sockBOT", BOT_SID, "StubBot", 0.0, 0.0))
        for _ in range(300):
            if self.got_en.is_set() or stop.is_set():
                break
            await asyncio.sleep(0.1)
        if not self.got_en.is_set():
            return
        self.sent["frs"] = time.monotonic()
        await send("frs")
        await send("0", player("sockME", MY_SID, "Guest_9", 4.0, 4.0))
        while not stop.is_set():
            await asyncio.sleep(0.2)

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
    # the matchmaker's validation tokens are redacted like the captcha and access tokens: never logged,
    # raw or URL-encoded, while the seek line itself still is
    leaked = [t for t in stub.issued if t in client_text or urllib.parse.quote(t, safe="") in client_text]
    check(f"{label}: no validation token reaches the log",
          stub.issued and not leaked and "validationToken=(redacted)" in client_text, leaked or len(stub.issued))


def check_version_gate():
    """HTTP-only regression guard for the stale-krunker_build.h "invalid version" kick: a fresh stub
    matchmaker must ACCEPT a seek whose dataQuery.v is the build's KRUNKER_BUILD_VERSION and REJECT
    one carrying any other build version. No client / WebSocket: just the matchmaker contract."""
    mm, wsp = kn_proto.free_ports(2)
    stub = Stub(mm, wsp)
    stub.start_http()
    base = f"http://127.0.0.1:{mm}"

    def seek(v):
        tok = urllib.request.urlopen(f"{base}/generate-token", timeout=10).read().decode()
        dq = urllib.parse.quote(json.dumps({"v": v}, separators=(",", ":")))
        url = (f"{base}/seek-game?hostname=krunker.io&region=de-fra&autoChangeGame=false"
               f"&validationToken={tok}&dataQuery={dq}&captchaToken=cap_{v[:6]}")
        try:
            r = urllib.request.urlopen(url, timeout=10)
            return r.status, json.loads(r.read())
        except urllib.error.HTTPError as e:
            return e.code, json.loads(e.read())

    try:
        ok_status, ok_body = seek(BUILD_VERSION)
        check("version gate: the build's buildVersion is accepted (a game is issued)",
              ok_status == 200 and ok_body.get("gameId") == GAME_ID, (ok_status, ok_body))
        bad_status, bad_body = seek("STALE000000000000000000000000000")
        check("version gate: a wrong buildVersion is refused as InvalidVersion",
              bad_status == 400 and bad_body.get("error") == "InvalidVersion", (bad_status, bad_body))
        check("version gate: the refused seek recorded the wrong version",
              stub.bad_version_seeks == ["STALE000000000000000000000000000"], stub.bad_version_seeks)
    finally:
        stub.stop()


def check_run(label, stub, client_lines, cheat_expected):
    check(f"{label}: WebSocket /ws?gameId=&clientKey=", stub.ws_paths[:1] == [f"/ws?gameId={GAME_ID}&clientKey={CLIENT_ID}"], stub.ws_paths)
    check(f"{label}: no undecodable frames", not stub.errors, stub.errors[:3])

    frames = stub.frames
    names = [f[0] for f in frames]
    ah, bad = 0, []
    for name, _, trailer in frames:
        if name != "cptR":
            ah = (ah + stub.ahkey) & 0xff
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
    check(f"{label}: the server saw no ahNum / \"q\"-sequence violations across {len(frames)} frames",
          not stub.proto_errors, stub.proto_errors[:5])
    check(f"{label}: \"q\" stayed in order across many ticks (>= 20 windows, isn strictly ascending)",
          len(qs) >= 20 and all(int(qs[i + 1][1]) > int(qs[i][1]) for i in range(len(qs) - 1)),
          f"{len(qs)} q, isn {isns[:8]}")
    check(f"{label}: the stub broadcast a counting-down match timer (\"t\")",
          len(stub.timer_sent) >= 3 and stub.timer_sent == sorted(stub.timer_sent, reverse=True), stub.timer_sent[:5])
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
    check(f"{label}: init translated (map {MAP_INDEX}, mode 0)", f"init: map {MAP_INDEX}, mode 0" in text)
    check(f"{label}: map {MAP_INDEX} resolved through krunker_maps.h ({MAP_NAME})",
          re.search(r"\[krunker\] map %d: %s \(map slot \d+\)" % (MAP_INDEX, MAP_NAME), text) is not None)
    check(f"{label}: no missing-map error", "could not be loaded" not in text and "cannot be shown" not in text)
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


def launch_native(label, script_text, extra_env, args=("--krunker",), wrap=(), scenario="default", stub_setup=None, display=None,
                  map_index=None, spawn=None):
    mm, wsp = kn_proto.free_ports(2)
    stub = Stub(mm, wsp, scenario, map_index, spawn)
    if stub_setup:
        stub_setup(stub)
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
    if display:
        env["DISPLAY"] = display  # a private Xvfb of the test's own (the challenge window runs; the game stays headless)
    # the live Krunker build's cache stays in the run (krunker_build_fetch.h); a stub publishing a build names it
    env["KRUNKNATIVE_KRUNKER_BUILD_CACHE"] = os.path.join(WORK, label + "-build-cache")
    if stub.published:
        env["KRUNKNATIVE_KRUNKER_BUILD_URL"] = build_url(stub)
    env.update(extra_env)
    out = open(log, "w")
    p = subprocess.Popen([*wrap, "stdbuf", "-oL", "-eL", f"{BUILD}/client_bin", *args], stdout=out, stderr=subprocess.STDOUT,
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
              # the stub void-kills DeadBot and an unknown sid right after we enter (on its "en");
              # the rows live 4 s, so probe #killFeed a few times inside that window.
              "10000 text @feed0 [id=killFeed]\n11000 text @feed1 [id=killFeed]\n12000 text @feed2 [id=killFeed]\n12800 text @feed3 [id=killFeed]\n"
              # the match clock on the HUD (#timerVal): probe it across the in-game window so a stuck
              # timer (the "t" packet never translated) fails - it must count down.
              "10500 text @timer0 [id=timerVal]\n14000 players\n14100 down W\n15500 up W\n15700 players\n"
              "13500 text @timer1 [id=timerVal]\n15900 con say hello official\n16900 players\n17000 state\n"
              "17500 text @timer2 [id=timerVal]\n18700 players\n18900 quit\n")
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
    check_killfeed_names("native", lines)
    check_timer_countdown("native", lines)
    check_sync_trace("native", lines)


SYNC_TRACE_RE = re.compile(r"\[krunker\] sync ([\d.]+)s: q (\d+) \((\d+) inputs\) isn (-?\d+) ack (-?\d+) \((-?\d+) behind\) l (\d+) f (\d+) "
                           r"corr (\d+)/(\d+) max ([\d.]+) pending (\d+) srv (\S+) (\S+) (\S+) me (\S+) (\S+) (\S+) lm (\d+) lock (\d+) paused (\d+)")


def sync_trace_rows(lines):
    rows = []
    for l in lines:
        m = SYNC_TRACE_RE.search(l)
        if m:
            g = m.groups()
            rows.append(dict(dt=float(g[0]), q=int(g[1]), inputs=int(g[2]), isn=int(g[3]), ack=int(g[4]), behind=int(g[5]), l=int(g[6]),
                             f=int(g[7]), corr=int(g[8]), recons=int(g[9]), max=float(g[10]), pending=int(g[11]),
                             srv=tuple(float(v) for v in g[12:15]), me=tuple(float(v) for v in g[15:18]), lm=int(g[18]), lock=int(g[19]),
                             paused=int(g[20])))
    return rows


def check_sync_trace(label, lines):
    """KRUNKNATIVE_KRUNKER_TRACE's sync line (krunker_net.c trace_sync_tick): one a second in the game. The
    "q" cadence is v10's (a window closes on the first input frame past tickRate, 1000 / tf ms: at most
    tf = 30 a second), the isn sent and the stub's acks ("l" inputSN, the last window's firstIsn) advance."""
    rows = sync_trace_rows(lines)
    sending = [r for r in rows if r["q"] > 0]
    check(f"{label}: sync trace: a line a second while in the game", len(sending) >= 3, len(rows))
    rates = sorted(r["q"] / r["dt"] for r in sending)
    med = rates[len(rates) // 2] if rates else 0
    check(f"{label}: sync trace: \"q\" at v10's cadence (tf 30: <= 30 windows a second, median {med:.1f})",
          rates and 10 <= med <= 31.5 and max(rates) <= 33, [round(v, 1) for v in rates])
    isns = [r["isn"] for r in sending]
    acks = [r["ack"] for r in sending if r["ack"] >= 0]
    check(f"{label}: sync trace: the isn sent advances every second", len(isns) >= 3 and all(b > a for a, b in zip(isns, isns[1:])), isns)
    check(f"{label}: sync trace: the server's ack (\"l\" inputSN) advances and keeps up",
          len(acks) >= 2 and acks[-1] > acks[0] and all(0 <= r["behind"] < 400 for r in sending if r["ack"] >= 0),
          [(r["isn"], r["ack"], r["behind"]) for r in sending][:8])
    check(f"{label}: sync trace: \"l\" about once a second, no \"f\"", sum(r["l"] for r in sending) >= len(sending) - 2 and
          not any(r["f"] for r in rows), [(r["l"], r["f"]) for r in rows][:8])


def check_timer_countdown(label, lines):
    """The HUD match clock (#timerVal) must count down: the UPDATE_TIMER ("t") packet is translated
    to our NET_PACKET_TIMER and drives net_round_seconds. A stuck "0:00"/unchanging value means "t"
    was dropped (the regression this guards)."""
    samples = []
    for line in lines:
        m = re.search(r"\[input_script\] text t=\d+ @timer\d+ n=\d+ checked=\d+ value=[^ ]* text=(\d+):(\d+)", line)
        if m:
            samples.append(int(m.group(1)) * 60 + int(m.group(2)))
    ok = len(samples) >= 2 and samples[0] > 0 and all(samples[i] > samples[i + 1] for i in range(len(samples) - 1))
    check(f"{label}: the HUD match timer counts down (#timerVal)", ok, "timerVal seconds over time: %s" % samples)


def check_killfeed_names(label, lines):
    """killfeed-name regression: a void death of a synced player (DeadBot) shows that player's
    name in #killFeed, and a "3" for a sid we never synced is dropped - never a "unknown" row (or
    an empty name, e.g. a leading/doubled " committed")."""
    feeds = []  # (probe label, folded #killFeed text) for every @feedN probe
    for line in lines:
        m = re.search(r"\[input_script\] text t=\d+ @(feed\d+) n=\d+ checked=\d+ value=[^ ]* text=(.*)", line)
        if m:
            # names are wrapped in LRM/RLM marks (fixStr); drop them before matching
            feeds.append((m.group(1), m.group(2).replace("‎", "").replace("‏", "")))
    seen_dead = any("DeadBot committed" in t for _, t in feeds)
    bad = [(lbl, t) for lbl, t in feeds if "unknown" in t.lower() or t.strip().startswith("committed") or "  committed" in t]
    check(f"{label}: a synced player's void death shows its name in #killFeed", seen_dead,
          f"{len(feeds)} #killFeed probes: {[t for _, t in feeds]}")
    check(f"{label}: a kill for a not-yet-synced sid never renders \"unknown\"/empty in #killFeed",
          not bad, bad[:3])


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
              "3000 clickid knCaptchaToken\n3100 type %s\n3200 clickid knCaptchaUse\n3400 vis @used knCaptchaManual knCaptchaCancel knCaptchaHolder\n"
              "3500 text @box [id=knCaptchaToken]\n3800 play\n8000 players\n8200 quit\n"
              % (os.path.join(WORK, "native-modal"), BURNT_TOKEN, PASTE_TOKEN))
    stub, p, out, log = launch_native(label, script, {"KRUNKNATIVE_KRUNKER_TRACE": ""})  # unset: the trace's default
    stub.used_captchas.add(BURNT_TOKEN)
    lines = finish_native(label, stub, p, out, log, timeout=180)
    text = "\n".join(lines)
    check(f"{label}: no krunker trace by default (no frame lines, no sync line)",
          not any("[krunker] sync " in l or "[krunker] <= " in l or "[krunker] => " in l for l in lines))
    bad = re.search(r"\[input_script\] text t=\d+ @bad n=1 checked=0 value= text=(.*)", text)
    check(f"{label}: a paste that is not a token is refused in the modal", bad and "not a Turnstile token" in bad.group(1), bad.group(1) if bad else None)
    check(f"{label}: the modal stays up after the bad paste", "@bad knCaptchaHolder=1" in text)
    again = re.search(r"\[input_script\] text t=\d+ @again n=1 checked=0 value= text=(.*)", text)
    check(f"{label}: a refused token brings the modal back, saying so", again and "refused that token" in again.group(1) and
          "@again knCaptchaManual=1" in text and "attempt 2, the last token was refused" in text, again.group(1) if again else None)
    # (the game socket's "cpt" is answered with nothing on screen: the whole holder is gone)
    check(f"{label}: the good paste closes the modal", "@used knCaptchaManual=0 knCaptchaCancel=0 knCaptchaHolder=0" in text)
    box = re.search(r"\[input_script\] text t=\d+ @box n=1 checked=0 value=(\S*) ", text)
    check(f"{label}: the paste box is emptied", box is not None and box.group(1) == "", box.group(0) if box else None)
    check(f"{label}: the bad paste never reached the matchmaker", not any("not a token" in r for _, _, r in stub.http_log))
    # (the `type` lines of the input script are the test's echo of what it typed: not the client's)
    check_matchmaker(label, stub, [BURNT_TOKEN, PASTE_TOKEN], "\n".join(l for l in lines if "[input_script]" not in l))
    check(f"{label}: the client joined as the stub's sid", f"joined as sid {MY_SID}" in text)


SYNC_RE = re.compile(r"\[input_script\] sync t=(\d+) move_lock=(-?\d+) locked=(-?\d+) frozen=(-?\d+) boost=([-\d.]+) noclip=(-?\d+) "
                     r"team=(-?\d+) hp=([-\d.]+)/(-?\d+) objective=(-?\d+) pos=([-\d.]+),([-\d.]+),([-\d.]+) yaw=([-\d.]+)")


def run_native_sync():
    """Packet-audit clusters A / B (krunker_link.c): the stub (Stub.sync_phases) locks our movement with
    "lm" while W is held, unlocks it ("ulm"), teleports us with "chgp" (found by socket id, dir 0), then
    "tm" (team 2), "wep" (weapon 5), an "00" rename and an "00" without keepOpen. The client's `sync`
    probes show what it did with each; the stub's frames show what reached the wire."""
    label = "native-sync"
    probes = "".join("%d sync\n%d players\n" % (t, t + 10) for t in range(5000, 51000, 300))
    script = ("0 boot\n1500 clickid knCaptchaToken\n1600 type %s\n1700 clickid knCaptchaUse\n3800 play\n5000 down W\n%s"
              "14000 up W\n20000 down W\n32000 up W\n52000 quit\n" % (PASTE_TOKEN, probes))
    # (input script events must be in time order: W's release slots in among the probes)
    events = sorted(script.strip().split("\n"), key=lambda l: int(l.split()[0]))
    stub, p, out, log = launch_native(label, "\n".join(events) + "\n", {}, scenario="sync")
    lines = finish_native(label, stub, p, out, log, timeout=240)
    text = "\n".join(lines)
    check(f"{label}: the client joined as the stub's sid", f"joined as sid {MY_SID}" in text)
    check(f"{label}: the server saw no ahNum / \"q\"-sequence violations", not stub.proto_errors, stub.proto_errors[:5])
    for name in ("lm", "ulm", "chgp", "tm", "lock", "frz", "buffs", "spk", "rg", "h", "hreset", "rfl", "00"):
        check(f"{label}: the stub got to send \"{name}\"", name in stub.sent, sorted(stub.sent))
    sync = [m for m in (SYNC_RE.search(l) for l in lines) if m]
    rows = [(int(m.group(1)), int(m.group(2)), float(m.group(11)), float(m.group(13)), m) for m in sync]

    def near(r, x, z, eps):
        return abs(r[2] - x) <= eps and abs(r[3] - z) <= eps

    locked = [r for r in rows if r[1] == 1]
    check(f"{label}: lm: our client is move-locked (game.moveLock)", len(locked) >= 3, len(locked))
    check(f"{label}: lm: resetPosition put us at 6,6 and W moves nothing while locked",
          locked and all(near(r, 6.0, 6.0, 0.05) for r in locked), [(r[0], r[2], r[3]) for r in locked if not near(r, 6.0, 6.0, 0.05)][:5])
    last_locked = max((r[0] for r in locked), default=0)
    after = [r for r in rows if r[0] > last_locked]
    first_tp = next((r[0] for r in after if near(r, 30.0, -12.0, 3.0)), None)
    free = [r for r in after if first_tp is None or r[0] < first_tp]
    check(f"{label}: ulm: the lock ends and W moves us again", free and all(r[1] == 0 for r in free) and
          any(not near(r, 6.0, 6.0, 0.3) for r in free), [(r[0], r[1], r[2], r[3]) for r in free][:6])

    def q_after(t0, t1=None):
        return [a for (n, a, _), t in zip(stub.frames, stub.frame_t) if n == "q" and t >= t0 and (t1 is None or t < t1)]

    def moves(qs):
        return [v for a in qs if len(a) > 5 and isinstance(a[5], dict) for k, v in a[5].items() if k.endswith("-4")]

    if "lm" in stub.sent and "ulm" in stub.sent:
        lock_moves = moves(q_after(stub.sent["lm"], stub.sent["ulm"]))
        check(f"{label}: \"q\" while lm holds: W goes out as moveDir -1 (never a direction)",
              -1 in lock_moves and not any(v >= 0 for v in moves(q_after(stub.sent["lm"] + 0.3, stub.sent["ulm"]))), lock_moves)
        check(f"{label}: \"q\" after ulm: a direction again", any(v >= 0 for v in moves(q_after(stub.sent["ulm"]))))
    tp = [r for r in rows if first_tp is not None and r[0] >= first_tp]
    yaw_ok = [r for r in tp if abs(math.remainder(float(r[4].group(14)) + math.pi / 2, 2 * math.pi)) < 1e-3]
    check(f"{label}: chgp (by socket id) teleported us to 30,-12", tp and near(tp[0], 30.0, -12.0, 3.0), [(r[0], r[2], r[3]) for r in tp][:3])
    check(f"{label}: chgp dir 0 turned us to yaw -PI/2", yaw_ok, [r[4].group(14) for r in tp][:3])
    if "chgp" in stub.sent:
        yaws = [a[4][1] for a in q_after(stub.sent["chgp"]) if len(a) > 4 and isinstance(a[4], list) and len(a[4]) > 1]
        check(f"{label}: the next \"q\" carries the new yaw (-1571 / 4712)", any(y in (-1571, 4712) for y in yaws), yaws[:6])
    check(f"{label}: tm: our team is 2", any(r[4].group(7) == "2" for r in rows))
    check(f"{label}: wep: the mode's active weapon is 5", any(r[4].group(10) == "5" for r in rows))
    check(f"{label}: 00: the stub player is renamed", re.search(r"players t=\d+ n=\d+.* %d:RenamedBot@" % BOT_SID, text) is not None)
    sync_rest_checks(label, stub, lines, text)
    check(f"{label}: 00 without keepOpen: the client closed the socket", "closed" in stub.sent and
          stub.sent["closed"] - stub.sent.get("00", 0) < 5, sorted(stub.sent))
    check(f"{label}: ... leaving the game cleanly (no reconnect, the script ran on to quit)",
          "the server ended the session (UPDATE_NAMES)" in text and "[krunker] leaving the official game" in text and len(stub.ws_paths) == 1 and
          p.returncode == 0, (p.returncode, stub.ws_paths))


HQ_INDEX, HQ_NAME, HQ_FILE = 34, "HQ", "34_hq.json"  # past v7.1.4's 32 maps: main refused it ("map index 34")


def hq_spawn():
    """A spawn of HQ as the live server has it: from this checkout's v10 file (assets/maps/official/34_hq.json).
    Spawn 4: W (a fresh player's yaw walks -Z) has open floor ahead of it; spawns 0 and 1 face a crate 2.5 away
    (krunker_maps_test walks the same line)."""
    with open(os.path.join(ROOT, "assets", "maps", "official", HQ_FILE)) as f:
        x, y, z = json.load(f)["spawns"][4][:3]
    return float(x), float(y), float(z)


def run_native_hq():
    """An official game on HQ (mapIndex 34, a map v7.1.4 lacked): the client loads 34_hq.json, the stub puts
    our player on one of its spawns and never corrects it, and we stand on the floor there, then walk with W:
    the client's own simulation of the map holds us up (a wrong or missing map drops us into the void, the
    "spawned in the middle of nowhere" desync) and the stub takes the "q" frames (ahNum, sequence)."""
    label = "native-hq"
    sx, sy, sz = hq_spawn()
    probes = "".join("%d sync\n" % t for t in range(5000, 15000, 250))
    script = ("0 boot\n1500 clickid knCaptchaToken\n1600 type %s\n1700 clickid knCaptchaUse\n3800 play\n%s9000 down W\n12000 up W\n15200 quit\n"
              % (PASTE_TOKEN, probes))
    events = sorted(script.strip().split("\n"), key=lambda l: int(l.split()[0]))
    stub, p, out, log = launch_native(label, "\n".join(events) + "\n", {}, scenario="spawn", map_index=HQ_INDEX, spawn=(sx, sy, sz))
    lines = finish_native(label, stub, p, out, log, timeout=180)
    text = "\n".join(lines)
    check(f"{label}: init translated (map {HQ_INDEX})", f"init: map {HQ_INDEX}, mode 0" in text)
    slot = re.search(r"\[krunker\] map %d: %s \(map slot (\d+)\)" % (HQ_INDEX, HQ_NAME), text)
    check(f"{label}: map {HQ_INDEX} resolved through krunker_maps.h ({HQ_NAME}, {HQ_FILE})", slot is not None)
    check(f"{label}: no missing-map error", "could not be loaded" not in text and "cannot be shown" not in text and
          "not in this client's catalogue" not in text)
    check(f"{label}: the client joined as the stub's sid", f"joined as sid {MY_SID}" in text)
    check(f"{label}: the server saw no ahNum / \"q\"-sequence violations", not stub.proto_errors, stub.proto_errors[:5])
    rows = [(int(m.group(1)), float(m.group(11)), float(m.group(12)), float(m.group(13))) for m in (SYNC_RE.search(l) for l in lines) if m]
    spawned = [r for r in rows if abs(r[1] - sx) < 3 and abs(r[3] - sz) < 3]
    before = [r for r in spawned if 7000 <= r[0] < 9000]
    check(f"{label}: placed on the spawn {sx:g},{sy:g},{sz:g} and standing there on the floor (no fall)",
          len(before) >= 4 and all(abs(r[2] - sy) < 2.0 for r in before), [(r[0], r[1], r[2], r[3]) for r in rows if 6000 <= r[0] < 9000][:8])
    walked = [r for r in rows if 9500 <= r[0] <= 15000]
    far = max((math.hypot(r[1] - sx, r[3] - sz) for r in walked), default=0.0)
    check(f"{label}: W walks us across the map (moved {far:.1f})", far > 10.0, [(r[0], r[1], r[2], r[3]) for r in walked][:6])
    check(f"{label}: walking keeps us on the map's ground (never below the spawn by 30)", walked and all(r[2] > sy - 30.0 for r in walked),
          [(r[0], r[2]) for r in walked if r[2] <= sy - 30.0][:5])
    if "spawn" in stub.sent:
        qs = [a for (n, a, _), t in zip(stub.frames, stub.frame_t) if n == "q" and t >= stub.sent["spawn"]]
        mv = [v for a in qs if len(a) > 5 and isinstance(a[5], dict) for k, v in a[5].items() if k.endswith("-4")]
        check(f"{label}: the stub took our \"q\" frames with W's direction", len(qs) > 20 and any(v >= 0 for v in mv), (len(qs), mv[:8]))
    else:
        check(f"{label}: the stub spawned us", False, sorted(stub.sent))


SYNCP_RE = re.compile(r"\[input_script\] sync\+ t=(\d+) unl=(-?\d+) jug=(-?\d+) kranked=([-\d.]+) share=(-?\d+) throw=(-?\d+) loadout=(\S+)")


def sync_rest_checks(label, stub, lines, text):
    """The second half of Stub.sync_phases: lock / frz (W held), the buffs and their undoing (noc unl spb
    sic jg busy chrg gsc, then spk), rg (a class change), h (a raise, then resetMax down) and rfl, read from
    the client's `sync` / `sync+` probes in order (the probes run on the script clock, the stub on its own)."""
    rows = {}
    for line in lines:
        m = SYNC_RE.search(line)
        if m:
            rows.setdefault(int(m.group(1)), {}).update(
                t=int(m.group(1)), locked=int(m.group(3)), frozen=int(m.group(4)), boost=float(m.group(5)), noclip=int(m.group(6)),
                hp=float(m.group(8)), max=int(m.group(9)), x=float(m.group(11)), z=float(m.group(13)))
        m = SYNCP_RE.search(line)
        if m:
            rows.setdefault(int(m.group(1)), {}).update(
                unl=int(m.group(2)), jug=int(m.group(3)), kranked=float(m.group(4)), share=int(m.group(5)), throw=int(m.group(6)),
                loadout=m.group(7))
    seq = [rows[t] for t in sorted(rows) if "x" in rows[t] and "unl" in rows[t]]

    def first(pred, start=0):
        return next((i for i in range(start or 0, len(seq)) if pred(seq[i])), None)

    def at_tp(r, eps=0.05):
        return abs(r["x"] - 30.0) <= eps and abs(r["z"] + 12.0) <= eps

    def held_still(key):  # rows while `key` is set, from 0.5 s after it came (the next "l" undid what W did before it)
        i = first(lambda r: r[key] == 1)
        j = first(lambda r: r[key] == 0, i) if i is not None else None
        if i is None or j is None:
            return i, j, None
        return i, j, [r for r in seq[i:j] if r["t"] >= seq[i]["t"] + 500]

    def xz(rs):
        return [(r["t"], r["x"], r["z"]) for r in rs or []]

    i, j, still = held_still("locked")
    check(f"{label}: lock: lockPlayer set, then cleared", i is not None and j is not None, (i, j))
    before = seq[max(0, i - 4):i] if i is not None else []
    check(f"{label}: lock: W moved us just before it (the hold is real)", any(not at_tp(r, 0.3) for r in before), xz(before))
    check(f"{label}: lock: W moves nothing while locked", still and all(at_tp(r) for r in still), xz(still)[:6])
    if "lock" in stub.sent and "unlock" in stub.sent:
        # v10 builds no input frame while window.locked (gameLoop `!window.locked`): no "q" at all, and the
        # isn picks up where it stopped (the stub's sequencing check above sees no rewind)
        qs = [a for (n, a, _), t in zip(stub.frames, stub.frame_t) if n == "q" and stub.sent["lock"] + 0.3 <= t < stub.sent["unlock"]]
        after = [a for (n, a, _), t in zip(stub.frames, stub.frame_t) if n == "q" and t >= stub.sent["unlock"] + 0.3]
        check(f"{label}: no \"q\" while locked", not qs, [a[:2] for a in qs][:6])
        check(f"{label}: \"q\" again after the lock", len(after) >= 3, len(after))
        trace = sync_trace_rows(lines)
        check(f"{label}: sync trace: paused while locked", any(r["lock"] == 1 and r["paused"] == 1 and r["q"] == 0 for r in trace),
              [(r["lock"], r["paused"], r["q"]) for r in trace if r["lock"]][:6])
    i, j, still = held_still("frozen")
    check(f"{label}: frz: frozen set, then cleared", i is not None and j is not None, (i, j))
    check(f"{label}: frz: W moves nothing while frozen", still and all(at_tp(r) for r in still), xz(still)[:6])
    b = first(lambda r: r["noclip"] == 1 and abs(r["boost"] - 2.5) < 1e-3 and r["unl"] == 1 and r["jug"] == 1 and r["share"] == 1 and r["kranked"] > 0)
    check(f"{label}: noc / unl / spb / sic / jg / chrg: noclip, unlimited ammo, boost 2.5, sharing, juggernaut, kranked", b is not None,
          [(r["t"], r["noclip"], r["boost"], r["unl"], r["jug"], r["share"], r["kranked"]) for r in seq if r["noclip"] or r["boost"]][:4])
    u = first(lambda r: r["noclip"] == 0 and r["boost"] == 0 and r["unl"] == 0 and r["jug"] == 0 and r["share"] == 0, b) if b is not None else None
    check(f"{label}: spk noclip / speedBoost / unlimitedAmmo (and jg / sic off) undo them", u is not None)
    check(f"{label}: busy and gsc reached the link", "busy: sid %d busy" % MY_SID in text and "busy: sid %d back" % MY_SID in text and
          "gsc: game state 3" in text)
    g = first(lambda r: r["hp"] == 60 and r["max"] == 60 and r["loadout"] == "0/2/12", u) if u is not None else None
    check(f"{label}: rg class change: Hunter's loadout (0 / Pistol / Knife) and 60 / 60 health", g is not None,
          [(r["t"], r["hp"], r["max"], r["loadout"]) for r in seq[u or 0:]][:24:3])
    h = first(lambda r: r["hp"] == 2000 and r["max"] == 2000, g) if g is not None else None
    check(f"{label}: h above maxHealth raises it (2000 / 2000)", h is not None)
    hr = first(lambda r: r["hp"] == 120 and r["max"] == 120, h) if h is not None else None
    check(f"{label}: h resetMax lowers maxHealth (120 / 120)", hr is not None)
    ingame = [r for r in seq[hr:] if r["throw"] != -1] if hr is not None else []  # (-1: we left the game)
    check(f"{label}: rfl: the knife is back (canThrow)", ingame and all(r["throw"] == 1 for r in ingame))


def run_native_hud():
    """Packet-audit clusters C / D (krunker_link.c): the stub (Stub.hud_phases) plays a CTF game's score,
    kill, leaderboard, ammo, flag, action-feed and localized-chat packets, then "end" (victory, team 1).
    The HUD probes (#myScoreVal, #killsVal, #ammoVal, #teamScores, #gameMessage, #leaderContainer,
    #chatList, the end screen) show what the client made of them."""
    label = "native-hud"
    # the transient popups (the "+N" score, the medal queue, the round popup) show for about a second each:
    # their own elements are sampled every 700 ms (#inGameUI's text is cut to 160 characters, and the popups
    # are appended at its end); the other values stay until the next write, one probe per 3 s is enough
    # icon medals (the medal's own badge) are #knMedal; text-only ones (the round popup) are #knChalName
    dense = ["text @sc [id=knScoreText]", "text @medal [id=knMedal]", "text @chal [id=knChalName]", "text @info [id=knInfoText]"]
    sparse = ["text @score [id=myScoreVal]", "text @kills [id=killsVal]", "text @ammo [id=ammoVal]",
              "text @teams [id=teamScores]", "text @gmsg [id=gameMessage]", "text @lead [id=leaderContainer]",
              "text @chat [id=chatList]", "vis @end endUI", "attr @vic src [id=victorySub]", "text @endtab [id=endTable]",
              "attr @endmid style [id=endMidHolder]", "text @lives [id=livesDisp]", "text @tags [id=krTagHolder]",
              "text @streak [id=killStreakHolder]", "text @rounds [id=roundsVal]"]
    probes = "".join("%d %s\n" % (t + k, kind) for t in range(5000, 34000, 700) for k, kind in enumerate(dense))
    probes += "".join("%d %s\n" % (t + k, kind) for t in range(5000, 34000, 3000) for k, kind in enumerate(sparse))
    # the end screen opens on the Performance tab; the leaderboard (#endTable) is the first tab
    tab = ("33750 nodes @tabfirst .tabHeader:first-child\n33760 nodes @tabs2 .tabHeader:nth-child(1)\n"
           "33800 clicksel .tabHeader:first-child\n33900 text @endtab [id=endTable]\n"
           "33950 text @tabs [id=tabHolder]\n34100 text @endtab [id=endTable]\n")
    script = "0 boot\n1500 clickid knCaptchaToken\n1600 type %s\n1700 clickid knCaptchaUse\n3800 play\n%s%s34500 quit\n" % (PASTE_TOKEN, probes, tab)
    events = sorted(script.strip().split("\n"), key=lambda l: int(l.split()[0]))
    stub, p, out, log = launch_native(label, "\n".join(events) + "\n", {}, scenario="hud")
    lines = finish_native(label, stub, p, out, log, timeout=240)
    text = "\n".join(lines)
    check(f"{label}: the client joined as the stub's sid", f"joined as sid {MY_SID}" in text)
    for name in ("hud", "end", "done"):
        check(f"{label}: the stub got to send its \"{name}\" packets", name in stub.sent, sorted(stub.sent))
    check(f"{label}: the server saw no ahNum / \"q\"-sequence violations", not stub.proto_errors, stub.proto_errors[:5])

    def probe(name):  # every @name text probe, in order
        return [m.group(1) for m in re.finditer(r"\[input_script\] text t=\d+ @%s n=\d+ checked=\d+ value=\S* text=(.*)" % name, text)]

    clean = lambda t: t.replace("\u200e", "").replace("\u200f", "")  # noqa: E731 (fixStr's LRM / RLM marks)
    score, kills, ammo = probe("score"), probe("kills"), probe("ammo")
    check(f"{label}: GET_SCORE: #myScoreVal shows our 120", "120" in score, score[-3:])
    check(f"{label}: GET_SCORE: the \"+120\" score popup (#knScoreText)", any("+120" in clean(t) for t in probe("sc")), probe("sc")[-1:])
    check(f"{label}: GET_KILL / KILL_PLAYER: #killsVal shows totalKills 3", "3" in kills, kills[-3:])
    check(f"{label}: UPDATE_AMMO: #ammoVal shows the primary's 17", "17" in ammo, ammo[-3:])
    teams = probe("teams")
    check(f"{label}: init + TEAM_SCORE: #teamScores goes from 3 to 5 for team 1", any(t.startswith("3") for t in teams) and
          any(re.match(r"5\s*flag", t) for t in teams), teams[:1] + teams[-1:])
    check(f"{label}: UPDATE_FLAG_INFO: team 2's flag line is our name", any("Guest_9" in t for t in teams), teams[-1:])
    gmsg = probe("gmsg")
    check(f"{label}: UPDATE_FLAG (carrier = us): #gameMessage \"You have the Flag\"", any("You have the Flag" in g for g in gmsg), gmsg[-3:])
    lead = [clean(t) for t in probe("lead")]
    check(f"{label}: UPDATE_LEADERS: the leaderboard lists both players", any("Guest_9" in t and "StubBot" in t for t in lead), lead[-1:])
    chat = [clean(t) for t in probe("chat")]
    check(f"{label}: ADD_CHATI18N: modes.round.win in English", any("Your Team has secured this round" in t for t in chat), chat[-1:])
    check(f"{label}: ADD_CHATI18N with arguments: player.killstreak", any("Somebody on a 7 Kill Streak" in t for t in chat), chat[-1:])
    check(f"{label}: KILL_STREAK_M: StubBot's 5 kill streak", any("StubBot is on a 5 Kill Streak" in t for t in chat), chat[-1:])
    check(f"{label}: ACTION_FEED: \"You got the Flag\"", any("You got the Flag" in t for t in chat), chat[-1:])
    lives, tags = probe("lives"), probe("tags")
    pops = [clean(t).upper() for t in probe("medal") + probe("chal")]  # medal popups are upper-cased
    check(f"{label}: UPDATE_LIVES: #livesDisp shows 3", "3" in lives, lives[-3:])
    check(f"{label}: UPDATE_KR_TAGS: #krTagHolder shows x3", any("x3" in t for t in tags), tags[-1:])
    check(f"{label}: ADD_MEDAL: the Pickup popup (+20)", any("PICKUP+20" in t for t in pops), pops[-1:])
    check(f"{label}: GET_ASSIST: the Assist popup", any("ASSIST" in t for t in pops), pops[-1:])
    info = probe("info")  # v10's textPopup("Switching Sides", lead, true): infoText (hud_popups.c)
    check(f"{label}: UPDATE_ROUNDS_DIS: \"Switching Sides\" popup", "Switching Sides" in info, sorted(set(info)))
    check(f"{label}: KILL_STREAK: #killStreakHolder is written", any(t.strip() for t in probe("streak")), probe("streak")[-1:])
    ends = re.findall(r"\[input_script\] vis t=(\d+) @end endUI=(\d)", text)
    shown = [int(t) for t, v in ends if v == "1"]
    check(f"{label}: END_GAME shows the end screen (#endUI)", shown, ends[-3:])
    vic = re.findall(r"\[input_script\] attr t=\d+ @vic n=\d+ src=(\S+)", text)
    check(f"{label}: END_GAME: a victory (#victorySub vic.png)", "./img/vic.png" in vic, sorted(set(vic)))
    endtab = [clean(t) for t in probe("endtab")]
    check(f"{label}: END_GAME: the results list both players with our reward", any("Guest_9" in t and "StubBot" in t and "12" in t
                                                                                     for t in endtab), endtab[-1:])


def run_native_rounds():
    """official-ingame-rounds: Switching Sides, the Juggernaut marker, a "Reward" medal's text score and an end
    table with a row of a player who left (Stub.rounds_phases)."""
    label = "native-rounds"
    dense = ["text @medal [id=knMedal]", "text @chal [id=knChalName]", "jugg", "players"]
    probes = "".join("%d %s\n" % (t + k, kind) for t in range(5000, 30000, 500) for k, kind in enumerate(dense))
    tab = ("29800 clicksel .tabHeader:first-child\n29900 text @endtab [id=endTable]\n30500 text @endtab [id=endTable]\n")
    script = "0 boot\n1500 clickid knCaptchaToken\n1600 type %s\n1700 clickid knCaptchaUse\n3800 play\n%s%s31000 quit\n" % (PASTE_TOKEN, probes, tab)
    events = sorted(script.strip().split("\n"), key=lambda l: int(l.split()[0]))
    stub, p, out, log = launch_native(label, "\n".join(events) + "\n", {}, scenario="rounds")
    lines = finish_native(label, stub, p, out, log, timeout=240)
    text = "\n".join(lines)
    check(f"{label}: the client joined as the stub's sid", f"joined as sid {MY_SID}" in text)
    for name in ("jg", "rds", "respawn", "end", "done"):
        check(f"{label}: the stub got to send its \"{name}\" packets", name in stub.sent, sorted(stub.sent))
    check(f"{label}: the server saw no ahNum / \"q\"-sequence violations", not stub.proto_errors, stub.proto_errors[:5])
    clean = lambda t: t.replace("‎", "").replace("‏", "")  # noqa: E731

    def probe(name):
        return [clean(m.group(1)) for m in re.finditer(r"\[input_script\] text t=\d+ @%s n=\d+ checked=\d+ value=\S* text=(.*)" % name, text)]

    jugg = [int(v) for v in re.findall(r"\[input_script\] jugg t=\d+ shown=(\d+)", text)]
    check(f"{label}: \"jg\": the Juggernaut marker is drawn over the enemy", jugg and max(jugg) >= 1, jugg[-5:])
    pops = [t.upper() for t in probe("medal") + probe("chal")]
    check(f"{label}: \"am\" Reward: the popup reads 12 KR", any("12 KR" in t for t in pops), sorted(set(pops))[-5:])
    # Switching Sides: no "q" from 0.5 s after "rds" until our "0" (2 s later), and they come back after it
    t_rds, t_spawn = stub.sent.get("rds", 0), stub.sent.get("respawn", 0)
    qs = [t for (name, _, _), t in zip(stub.frames, stub.frame_t) if name == "q"]
    during = [round(t - t_rds, 2) for t in qs if t_rds + 0.5 < t < t_spawn]
    after = [t for t in qs if t > t_spawn + 0.3]
    check(f"{label}: \"rds\": no input frame until our next \"0\"", t_rds and t_spawn and not during, during[:5])
    check(f"{label}: \"rds\": the input frames resume after our \"0\"", after, len(after))
    rows = re.findall(r"\[input_script\] players t=(\d+) n=\d+(.*)", text)
    inactive = [r for t, r in rows if re.search(r" %d:[^ ]*\*-" % MY_SID, r) and re.search(r" %d:[^ ]*-" % BOT_SID, r)]
    check(f"{label}: \"rds\": every player inactive (players probe '-')", inactive, rows[-1:] if rows else "")
    first = next((i for i, (_, r) in enumerate(rows) if r in inactive), None)
    check(f"{label}: after our \"0\" we are active again", first is not None and
          any(re.search(r" %d:[^ ]*\*(?:$| )" % MY_SID, r) for _, r in rows[first + 1:]), rows[-1:] if rows else "")
    endtab = probe("endtab")
    check(f"{label}: \"end\": the row of a player who left (Gone31) and Hardpoint's dmg (812 / 377)",
          any("Gone31" in t and "812" in t and "377" in t for t in endtab), endtab[-1:])


def run_native_popups():
    """v10's textPopup (infoText / infoSub) and the gmsg countdown (timerText): the stub (Stub.popups_phases)
    sends "tx", "stlkpd", "gmsg" [.., 3, [title, sub]] and "rds"; the overlay elements (#knInfoText,
    #knInfoSub, #knTimerText) and the null-audio log show them (pop_3 unless silent, cntd_3)."""
    label = "native-popups"
    probes = "".join("%d text @info [id=knInfoText]\n%d text @sub [id=knInfoSub]\n%d text @timer [id=knTimerText]\n"
                     "%d vis @vis knInfoText knInfoSub knTimerText\n" % (t, t + 1, t + 2, t + 3) for t in range(5000, 20000, 250))
    script = "0 boot\n1500 clickid knCaptchaToken\n1600 type %s\n1700 clickid knCaptchaUse\n3800 play\n%s20500 quit\n" % (PASTE_TOKEN, probes)
    events = sorted(script.strip().split("\n"), key=lambda l: int(l.split()[0]))
    stub, p, out, log = launch_native(label, "\n".join(events) + "\n", {"KRUNKNATIVE_AUDIO_LOG": "1"}, scenario="popups")
    lines = finish_native(label, stub, p, out, log, timeout=180)
    text = "\n".join(lines)
    check(f"{label}: the client joined as the stub's sid", f"joined as sid {MY_SID}" in text)
    for name in ("tx", "stlkpd", "gmsg", "rds", "done"):
        check(f"{label}: the stub got to send its \"{name}\" packets", name in stub.sent, sorted(stub.sent))

    def probe(name):
        return [m.group(1) for m in re.finditer(r"\[input_script\] text t=\d+ @%s n=1 checked=\d+ value=\S* text=(.*)" % name, text)]

    vis = re.findall(r"\[input_script\] vis t=\d+ @vis knInfoText=(\d) knInfoSub=(\d) knTimerText=(\d)", text)
    shown = lambda k: [v for v in vis if v[k] == "1"]  # noqa: E731
    info, sub, timer = probe("info"), probe("sub"), probe("timer")
    check(f"{label}: tx: infoText Overtime / infoSub +60 Seconds", "Overtime" in info and "+60 Seconds" in sub, (sorted(set(info)), sorted(set(sub))))
    check(f"{label}: stlkpd: the whole stalker lines", "You are the Living" in info and "Hunt the stalker and survive the round!" in sub,
          (sorted(set(info)), sorted(set(sub))))
    check(f"{label}: gmsg [title, sub]: Bomb / Planted", "Bomb" in info and "Planted" in sub, (sorted(set(info)), sorted(set(sub))))
    check(f"{label}: gmsg countdown: timerText 3", "3" in timer, sorted(set(timer)))
    check(f"{label}: rds: Switching Sides / the lead", "Switching Sides" in info and any("is in the Lead!" in t for t in sub),
          (sorted(set(info)), sorted(set(sub))))
    check(f"{label}: the popups were drawn (shown, then hidden again)", shown(0) and shown(1) and shown(2) and vis and vis[-1][0] == "0",
          (len(shown(0)), len(shown(1)), len(shown(2)), vis[-1:] if vis else None))
    audio = [l for l in lines if "[audio]" in l and " play" in l]
    pops = sum(1 for l in audio if "play2d pop_3 " in l)
    check(f"{label}: pop_3 for tx, stlkpd and the gmsg popup; none for the silent rds", pops == 3, [l for l in audio if "pop_3" in l])
    check(f"{label}: cntd_3 at 0.2", any("play2d cntd_3 " in l for l in audio), [l for l in audio if "cntd" in l])


def run_native_ingame():
    """official-ingame (krunker_link.c names / kill feed / death card / chat / host): the stub
    (Stub.ingame_phases) plays a TDM lobby whose bot is "AceBot" (alias; its name acctBot), clan CLN,
    level 33, featured, class 1, the lobby's host; we kill it four ways, chat arrives on both tabs, the
    host moves to us, then the bot kills us with deathStats."""
    label = "native-ingame"
    dense = ["nodes @wb [src='./img/wallbang_0.png']", "nodes @hs [src='img/headshot_0.png']",
             "nodes @thrown .thrownChatIcon", "nodes @stk2 [src='img/streaks/2.png']",
             "nodes @stk3 [src='img/streaks/3.png']", "text @feed [id=killFeed]"]
    sparse = ["text @lead [id=leaderContainer]", "nodes @team1 [data-tab='1']", "grep @chatteam push [id=chatList]",
              "grep @chatglob hello [id=chatList]", "grep @host Host! [id=chatList]", "grep @dacc 37.5% [id=deathUIHolder]",
              "grep @dkill AceBot [id=deathUIHolder]", "grep @dname acctBot [id=deathUIHolder]", "vis @death deathUIHolder"]
    probes = "".join("%d %s\n" % (t + k, kind) for t in range(5000, 22000, 600) for k, kind in enumerate(dense))
    probes += "".join("%d %s\n" % (t + k, kind) for t in range(5000, 22000, 1500) for k, kind in enumerate(sparse))
    script = "0 boot\n1500 clickid knCaptchaToken\n1600 type %s\n1700 clickid knCaptchaUse\n3800 play\n%s22500 quit\n" % (PASTE_TOKEN, probes)
    events = sorted(script.strip().split("\n"), key=lambda l: int(l.split()[0]))
    # Settings > Chat "Show Player Messages" (showMessages, v10's default off) on, as a player who reads chat
    store = os.path.join(WORK, label + "-cfg", "krunknative")
    os.makedirs(store, exist_ok=True)
    with open(os.path.join(store, "knSettings.json"), "w") as f:
        json.dump({"showMessages": True}, f)
    stub, p, out, log = launch_native(label, "\n".join(events) + "\n", {}, scenario="ingame")
    lines = finish_native(label, stub, p, out, log, timeout=240)
    text = "\n".join(lines)
    check(f"{label}: the client joined as the stub's sid", f"joined as sid {MY_SID}" in text)
    for name in ("ingame", "death", "done"):
        check(f"{label}: the stub got to send its \"{name}\" packets", name in stub.sent, sorted(stub.sent))
    check(f"{label}: the server saw no ahNum / \"q\"-sequence violations", not stub.proto_errors, stub.proto_errors[:5])

    def nodes(name):
        return [int(m.group(1)) for m in re.finditer(r"\[input_script\] nodes t=\d+ @%s elements=\d+ match=(\d+)" % name, text)]

    def grep(name):
        return [int(m.group(1)) for m in re.finditer(r"\[input_script\] grep t=\d+ @%s needle=\S+ n=\d+ hits=(\d+)" % name, text)]

    def probe(name):
        return [m.group(1) for m in re.finditer(r"\[input_script\] text t=\d+ @%s n=\d+ checked=\d+ value=\S* text=(.*)" % name, text)]

    clean = lambda t: t.replace("\u200e", "").replace("\u200f", "")  # noqa: E731
    feed = [clean(t) for t in probe("feed")]
    check(f"{label}: \"3\" wb + hs: the feed row has the wallbang and headshot icons", max(nodes("wb") or [0]) >= 1 and max(nodes("hs") or [0]) >= 1,
          (nodes("wb")[-3:], nodes("hs")[-3:]))
    check(f"{label}: \"3\" pj: the thrown icon", max(nodes("thrown") or [0]) >= 1, nodes("thrown")[-3:])
    check(f"{label}: \"3\" stk 2: the streak icon", max(nodes("stk2") or [0]) >= 1, nodes("stk2")[-3:])
    check(f"{label}: \"3\" hideFeed: no row for the hidden (stk 3) kill", nodes("stk3") and max(nodes("stk3")) == 0, nodes("stk3")[-3:])
    check(f"{label}: the feed shows the bot's alias, never its name", any("AceBot" in t for t in feed) and not any("acctBot" in t for t in feed),
          feed[-1:])
    lead = [clean(t) for t in probe("lead")]
    check(f"{label}: the leaderboard: alias and clan CLN", any("AceBot" in t and "CLN" in t for t in lead), lead[-1:])
    check(f"{label}: \"ch\" team 1: a team-tab row", max(nodes("team1") or [0]) >= 1, nodes("team1")[-3:])
    check(f"{label}: \"ch\": both lines reached #chatList", max(grep("chatteam") or [0]) >= 1 and max(grep("chatglob") or [0]) >= 1,
          (grep("chatteam")[-2:], grep("chatglob")[-2:]))
    check(f"{label}: \"uh\": You are now Host!", max(grep("host") or [0]) >= 1, grep("host")[-2:])
    shown = re.findall(r"\[input_script\] vis t=\d+ @death deathUIHolder=(\d)", text)
    check(f"{label}: our death shows the death card", "1" in shown, shown[-3:])
    check(f"{label}: the death card: deathStats accuracy and the killer's alias", max(grep("dacc") or [0]) >= 1 and
          max(grep("dkill") or [0]) >= 1 and max(grep("dname") or [0]) == 0, (grep("dacc")[-2:], grep("dkill")[-2:], grep("dname")[-2:]))


KANJI_INDEX, KANJI_SPAWN = 10, (-343.0, -143.0, -28.0)  # Kanji: 3 Domination zones; its first spawn


def run_native_world():
    """krunker_link_world.c: the stub (Stub.world_phases) plays a Domination game's zone, spawnable, object,
    message, lockout, text-popup, loadout, AI and airdrop packets. The `world` probe (input_script.c) shows
    what the client made of them, #gameMessage / #chatList / the popups what the HUD shows."""
    label = "native-world"
    dense = ["world", "text @gmsg [id=gameMessage]", "text @info [id=knInfoText]", "text @sub [id=knInfoSub]", "sync"]
    sparse = ["text @chat [id=chatList]"]
    probes = "".join("%d %s\n" % (t + k, kind) for t in range(5000, 30000, 500) for k, kind in enumerate(dense))
    probes += "".join("%d %s\n" % (t + k, kind) for t in range(5000, 30000, 2000) for k, kind in enumerate(sparse))
    script = "0 boot\n1500 clickid knCaptchaToken\n1600 type %s\n1700 clickid knCaptchaUse\n3800 play\n%s30500 quit\n" % (PASTE_TOKEN, probes)
    events = sorted(script.strip().split("\n"), key=lambda l: int(l.split()[0]))
    stub, p, out, log = launch_native(label, "\n".join(events) + "\n", {}, scenario="world", map_index=KANJI_INDEX, spawn=KANJI_SPAWN)
    lines = finish_native(label, stub, p, out, log, timeout=240)
    text = "\n".join(lines)
    check(f"{label}: the client joined as the stub's sid", f"joined as sid {MY_SID}" in text)
    for name in ("world", "objects", "dispose", "done"):
        check(f"{label}: the stub got to send its \"{name}\" packets", name in stub.sent, sorted(stub.sent))
    worlds = [dict(re.findall(r"(\w+)=(\"[^\"]*\"|\S+)", m.group(1))) for m in re.finditer(r"\[input_script\] world (.*)", text)]
    worlds = [w for w in worlds if "dom" in w]
    check(f"{label}: the world probe ran", len(worlds) > 10, len(worlds))

    def seen(pred):
        return any(pred(w) for w in worlds)

    check(f"{label}: init doms: zone A is team 2's", seen(lambda w: w["dom"].startswith("2/0,")), [w["dom"] for w in worlds[:2]])
    check(f"{label}: init arg 3 / roundC: round wins 1 / 0, 2 rounds left", seen(lambda w: w["wins"] == "1/0" and w["rounds"] == "2"),
          [(w["wins"], w["rounds"]) for w in worlds[:2]])
    check(f"{label}: init spwns: the tag at -330,-40", seen(lambda w: "0@-330,-40" in w["tags"]), [w["tags"] for w in worlds[:2]])
    low = lambda w: w["destroyed"].split(":")[1].split(",")  # noqa: E731 (the destroyed uids below 16)
    check(f"{label}: init dest: object 4 destroyed", seen(lambda w: "4" in low(w)), sorted({w["destroyed"] for w in worlds}))
    check(f"{label}: \"do\": object 3 destroyed too", seen(lambda w: "3" in low(w) and "4" in low(w)), sorted({w["destroyed"] for w in worlds}))

    def securing(w):  # zone A: nobody's, captureT from 2500 ms (the client runs the capture on), zone B team 2's
        z = w["dom"].split(",")
        return len(z) == 3 and z[0].startswith("0/") and int(z[0][2:]) >= 2500 and z[1] == "2/0"

    check(f"{label}: \"uct\": zone A securing (from 2500 ms), zone B team 2", seen(securing), sorted({w["dom"] for w in worlds})[:4])
    check(f"{label}: \"cct\": zone C to team 1", seen(lambda w: w["dom"].endswith(",1/0")), sorted({w["dom"] for w in worlds}))
    check(f"{label}: \"crsp\": the second tag", seen(lambda w: "0@-320,-50" in w["tags"]), sorted({w["tags"] for w in worlds}))
    check(f"{label}: \"dsp\": the init's tag goes, the second stays", seen(lambda w: w["tags"] == "0@-320,-50"), sorted({w["tags"] for w in worlds}))
    check(f"{label}: \"aai\" + \"ai\": an AI, \"rai\" removes it", seen(lambda w: w["ai"] == "1") and worlds[-1]["ai"] == "0",
          sorted({w["ai"] for w in worlds}))
    check(f"{label}: \"ad\": an airdrop, \"rad\" removes it", seen(lambda w: w["drops"] == "1") and worlds[-1]["drops"] == "0",
          sorted({w["drops"] for w in worlds}))
    check(f"{label}: \"lkt\": the lockout (90.5 s)", seen(lambda w: w["lock"].startswith("90") or w["lock"].startswith("89")),
          sorted({w["lock"] for w in worlds})[:5])
    gm = [re.sub(r"\s+", " ", m.group(1)).strip() for m in re.finditer(r"\[input_script\] text t=\d+ @gmsg n=\d+ checked=\d+ value=\S* text=(.*)", text)]
    check(f"{label}: gmsg \"wt\": #gameMessage waiting for players", "waiting for players" in gm, sorted(set(gm)))
    check(f"{label}: gmsg \"wt\" + suffix", "waiting for players 01:00" in gm, sorted(set(gm)))
    check(f"{label}: lkt: #gameMessage warmup starts in 01:30", any(g.startswith("warmup starts in 01:") for g in gm), sorted(set(gm)))
    check(f"{label}: gmsg without args clears it", gm and gm[-1] == "", gm[-3:])
    info = [m.group(1) for m in re.finditer(r"\[input_script\] text t=\d+ @info n=\d+ checked=\d+ value=\S* text=(.*)", text)]
    sub = [m.group(1) for m in re.finditer(r"\[input_script\] text t=\d+ @sub n=\d+ checked=\d+ value=\S* text=(.*)", text)]
    check(f"{label}: \"tx\": the Overtime / +60 Seconds text popup (infoText / infoSub)", "Overtime" in info and "+60 Seconds" in sub,
          (sorted(set(info))[-3:], sorted(set(sub))[-3:]))
    chat = [m.group(1) for m in re.finditer(r"\[input_script\] text t=\d+ @chat n=\d+ checked=\d+ value=\S* text=(.*)", text)]
    check(f"{label}: \"cct\": the \"Zone C Captured\" chat line", any("Zone C Captured" in c for c in chat), chat[-1:])
    loads = re.findall(r"\[input_script\] sync\+ t=\d+ .* loadout=(\S+)", text)
    check(f"{label}: \"inat\": our loadout 3 / 1 (+ the knife)", any(l.startswith("3/1") for l in loads), sorted(set(loads)))


def run_native_voice():
    """official-ingame-voice (krunker_link.c "vc" both ways): our clips (KRUNKNATIVE_VOICE_INPUT tone, V held)
    reach the stub as v10's "vc" frames; the stub's "vc" clips play on channels -1 and 0, not on 1."""
    label = "native-voice"
    script = ("0 boot\n1500 clickid knCaptchaToken\n1600 type %s\n1700 clickid knCaptchaUse\n3800 play\n"
              "7000 down V\n9500 up V\n24000 quit\n") % PASTE_TOKEN
    stub, p, out, log = launch_native(label, script, {"KRUNKNATIVE_VOICE_INPUT": "tone:440", "KRUNKNATIVE_VOICE_LOG": "1"},
                                      scenario="voice")
    lines = finish_native(label, stub, p, out, log, timeout=240)
    text = "\n".join(lines)
    check(f"{label}: the client joined as the stub's sid", f"joined as sid {MY_SID}" in text)
    for name in ("vc_out", "vc_in", "done"):
        check(f"{label}: the stub got to its \"{name}\" step", name in stub.sent, sorted(stub.sent))
    check(f"{label}: the server saw no ahNum / \"q\"-sequence violations", not stub.proto_errors, stub.proto_errors[:5])
    vcs = [f[1] for f in stub.frames if f[0] == "vc"]
    sent = sum(1 for l in lines if "[voice]" in l and " send clip " in l)
    check(f"{label}: every clip the client recorded went out as one \"vc\"", vcs and len(vcs) == sent, (len(vcs), sent))
    ok = all(len(a) == 4 and isinstance(a[0], str) and a[0].startswith("data:audio/ogg; codecs=opus;base64,") and
             a[1] == 0 and isinstance(a[2], (int, float)) and 0 < a[2] <= 1 and a[3] == 6000 for a in vcs)
    check(f"{label}: \"vc\" is [dataURL (v10's ogg/opus label), channel 0, micVolume, micQuality 6000]", ok,
          [(a[0][:40], a[1:]) for a in vcs[:2]])
    recv = re.findall(r"\[voice\] t=[\d.]+ recv v7 StubBot sid=(\d+) ch=(-?\d+)", text)
    chans = sorted({int(c) for _, c in recv})
    check(f"{label}: the bot's clips on channels -1 and 0 reach the player, the team-tab one does not",
          chans == [-1, 0] and all(int(s) == BOT_SID for s, _ in recv), recv)
    check(f"{label}: a received clip is decoded and played", re.search(r"\[voice\] t=[\d.]+ play StubBot ", text) is not None)
    if vcs:  # what an official krunker.io player's browser makes of the clip we sent
        check_clip_in_chromium(label, vcs[0][0], "our recorded clip")


# What headless Chromium makes of one voice clip data URL (official-ingame-webvoice): v10 plays a received
# "vc" clip with Howler (new Howl({src: [dataURL]})), which fetches a data URI and decodes it with WebAudio's
# decodeAudioData, and keeps a non-final clip only when its duration() <= 0.78 s (game.deob.js :209642). The
# <audio> element (Howler's html5 fallback) is probed too. Returns the page's findings, or None without Chromium.
CLIP_PROBE_JS = """(async () => {
  const url = %s, r = {};
  try {
    const buf = await (await fetch(url)).arrayBuffer();
    r.bytes = buf.byteLength;
    r.magic = Array.from(new Uint8Array(buf.slice(0, 4))).map(b => b.toString(16).padStart(2, '0')).join('');
    const audio = await new OfflineAudioContext(1, 1, 48000).decodeAudioData(buf);
    r.duration = audio.duration; r.rate = audio.sampleRate; r.channels = audio.numberOfChannels;
    let peak = 0; const d = audio.getChannelData(0); for (let i = 0; i < d.length; i++) peak = Math.max(peak, Math.abs(d[i]));
    r.peak = peak;
  } catch (e) { r.decodeError = String(e); }
  const el = document.createElement('audio');
  r.canPlay = {ogg: el.canPlayType('audio/ogg; codecs=opus'), webm: el.canPlayType('audio/webm; codecs=opus')};
  r.element = await new Promise(done => {
    const a = new Audio(); let t = setTimeout(() => done({timeout: true}), 8000);
    a.onloadedmetadata = () => { a.play().then(() => setTimeout(() => { clearTimeout(t); done({duration: a.duration, played: a.currentTime}); }, 300),
                                               e => { clearTimeout(t); done({duration: a.duration, playError: String(e)}); }); };
    a.onerror = () => { clearTimeout(t); done({error: a.error ? a.error.code : -1}); };
    a.src = url;
  });
  return r;
})()"""


def chromium_clip_probe(label, url):
    if not CHROME:
        return None
    (cdpp,) = kn_proto.free_ports(1)
    log = open(os.path.join(WORK, f"{label}-chrome.log"), "w")
    chrome = subprocess.Popen([CHROME, "--headless=new", "--no-sandbox", "--autoplay-policy=no-user-gesture-required",
                               "--user-data-dir=" + os.path.join(WORK, label + "-chrome"), "--remote-debugging-port=%d" % cdpp, "about:blank"],
                              stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, start_new_session=True)
    try:
        cdp = Cdp(cdpp)
        r = cdp.send("Runtime.evaluate", {"expression": CLIP_PROBE_JS % json.dumps(url), "awaitPromise": True, "returnByValue": True}, timeout=30)
        return r.get("result", {}).get("result", {}).get("value")
    finally:
        os.killpg(chrome.pid, signal.SIGKILL)
        chrome.wait()
        log.close()


def check_clip_in_chromium(label, url, what, min_peak=0.05):
    """A clip of ours as an official browser player gets it: decodeAudioData takes it (what Howler does with
    a data URI), the clip is short enough for v10's non-final-clip rule (<= 0.78 s) and, with min_peak, not
    silent (a known tone; Chromium's fake microphone beeps only now and then, so the web run checks decoding only)."""
    got = chromium_clip_probe(label, url)
    if got is None:
        print(f"{label}: no Chromium: the browser decode check of {what} is SKIPPED")
        return
    print(f"{label}: Chromium on {what}: {json.dumps(got)}")
    check(f"{label}: Chromium's decodeAudioData takes {what} (Howler's path for a data URI)",
          got.get("duration") and 0 < got["duration"] <= 0.78 and got.get("peak", 0) >= min_peak, got)
    el = got.get("element") or {}
    check(f"{label}: an <audio> element plays {what} too (Howler's html5 fallback)", el.get("duration", 0) > 0 and "error" not in el and
          "playError" not in el, el)


def run_native_social():
    """official-ingame-social: "fe" (a refused enter: the menu again with the red line, a second Play joins),
    "vf" (the ranked forfeit vote box and our "Vote Submitted" line), "prc" + "skinF" (a finding by the
    reconnected bot), "skin" (our "Items Found" card in #skinRewardHolder > #skinRewList)."""
    label = "native-social"
    probes = ["text @vfinfo [id=voteForfeitInfo]", "text @vfnum [id=voteForfeitNum]", "vis @vf voteToForfeit",
              "grep @fe Full [id=chatList]", "grep @sub Submitted [id=chatList]", "grep @found found [id=chatList]",
              "nodes @card .skinRewCard", "vis @rew skinRewardHolder"]
    script = "0 boot\n1500 clickid knCaptchaToken\n1600 type %s\n1700 clickid knCaptchaUse\n3800 play\n" % PASTE_TOKEN
    script += "".join("%d %s\n" % (t + k, kind) for t in range(4500, 26000, 700) for k, kind in enumerate(probes))
    script += "26500 quit\n"
    events = sorted(script.strip().split("\n"), key=lambda l: int(l.split()[0]))
    stub, p, out, log = launch_native(label, "\n".join(events) + "\n", {}, scenario="social")
    lines = finish_native(label, stub, p, out, log, timeout=240)
    text = "\n".join(lines)
    for name in ("fe", "en2", "vf", "vfend", "done"):
        check(f"{label}: the stub got to send its \"{name}\" packets", name in stub.sent, sorted(stub.sent))
    check(f"{label}: the server saw no ahNum / \"q\"-sequence violations", not stub.proto_errors, stub.proto_errors[:5])

    def at(kind, pattern):  # (client ms, value) of every matching probe line
        return [(int(m.group(1)), m.group(2)) for m in re.finditer(r"\[input_script\] %s t=(\d+) %s" % (kind, pattern), text)]

    # the refused enter ends the client's own enter (in_game / spawn_requested): it can enter again (the
    # boot `play` retries at once; without the reset the client would never send a second "en")
    ens = [i for i, f in enumerate(stub.frames) if f[0] == "en"]
    check(f"{label}: \"fe\": the client may enter again (a second \"en\")", len(ens) >= 2, len(ens))
    check(f"{label}: \"fe\": the red line", any(int(v) >= 1 for _, v in at("grep", r"@fe needle=\S+ n=\d+ hits=(\d+)")))
    check(f"{label}: a second Play joins", f"joined as sid {MY_SID}" in text)
    vis = [v for _, v in at("vis", r"@vf voteToForfeit=(\d)")]
    info = [v.strip() for _, v in at("text", r"@vfinfo n=\d+ checked=\d+ value=\S* text=(.*)")]
    num = [v.strip() for _, v in at("text", r"@vfnum n=\d+ checked=\d+ value=\S* text=(.*)")]
    check(f"{label}: \"vf\": the forfeit box opens with the initiator and the count",
          "1" in vis and "StubBot initiated vote" in info and "Votes: 1/3" in num, (sorted(set(vis)), info[-1:], num[-1:]))
    check(f"{label}: \"vf\" null hides it again", vis and vis[-1] == "0", vis[-3:])
    check(f"{label}: \"vf\" with our alias: \"Vote Submitted - 2/3\"", any(int(v) >= 1 for _, v in at("grep", r"@sub needle=\S+ n=\d+ hits=(\d+)")))
    check(f"{label}: \"prc\" + \"skinF\": the bot's finding by its new socket id",
          any(int(v) >= 1 for _, v in at("grep", r"@found needle=\S+ n=\d+ hits=(\d+)")))
    cards = [int(v) for _, v in at("nodes", r"@card elements=\d+ match=(\d+)")]
    rew = [v for _, v in at("vis", r"@rew skinRewardHolder=(\d)")]
    check(f"{label}: \"skin\": our card in #skinRewardHolder > #skinRewList", max(cards or [0]) >= 1 and "1" in rew, (cards[-3:], rew[-3:]))


def run_native_lobby():
    """The default (no KRUNKNATIVE_KRUNKER_JOIN): a click on the official lobby in the server browser
    joins it in this client. From the offline game: the Servers window lists the stub's lobby, the
    click seeks it, the Security Challenge modal asks; Cancel ends the join saying so; a second click
    and a pasted token join the game."""
    label = "native-lobby"
    script = ("0 menu\n300 call showWindow 2\n3000 text @status [id=kioStatus]\n3100 nodes @rows .quickJoin\n"
              # the KRUNKER.IO group (the last one) opened: its rows' titles
              "3120 clicksel .setHed .plusOrMinus\n3160 grep @kioOld Old_Burg .settNameIn\n3170 grep @kio152 1.5.2 .settNameIn\n"
              "3180 grep @kioMap %s .settNameIn\n"
              "3200 call krunkerJoin \"%s\"\n4500 vis @ask1 knCaptchaHolder knCaptchaManual\n4600 clickid knCaptchaCancel\n"
              "5200 vis @cancelled knCaptchaHolder\n5300 call showWindow 2\n5800 call krunkerJoin \"%s\"\n6200 text @toast1 [id=knNotify]\n"
              "7000 vis @ask2 knCaptchaHolder\n"
              "7100 clickid knCaptchaToken\n7200 type %s\n7300 clickid knCaptchaUse\n8000 play\n12800 text @toast2 [id=knNotify]\n13000 players\n"
              # in the official game: the Invite link is krunker.io's, the serverinfo and the menu's region
              # label name the official lobby (its list row: region, map, mode, players)
              "13050 con status\n13100 call copyInviteLink\n13150 text @region [id=menuRegionLabel]\n13200 quit\n"
              % (MAP_NAME, GAME_ID, GAME_ID, PASTE_TOKEN))
    stub, p, out, log = launch_native(label, script, {}, args=("--offline",))
    lines = finish_native(label, stub, p, out, log, timeout=180)
    text = "\n".join(lines)
    st = re.search(r"\[input_script\] text t=\d+ @status n=1 .*?text=(.*)", text)
    check(f"{label}: the KRUNKER.IO section says its lobbies join here", st and "KRUNKER.IO OFFICIAL LOBBIES" in st.group(1),
          st.group(1) if st else None)
    check(f"{label}: no \"can't be joined\" text anywhere", not re.search(r"can.t be joined|cannot be joined|can.t join krunker", text))
    rows = re.search(r"nodes t=\d+ @rows elements=\d+ match=(\d+)", text)
    check(f"{label}: the krunker.io group is listed with its Quick Join (main's group header)", rows and int(rows.group(1)) >= 1, rows.group(0) if rows else None)
    grep = {m.group(1): int(m.group(2)) for m in re.finditer(r"\[input_script\] grep t=\d+ @(\w+) needle=\S+ n=\d+ hits=(\d+)", text)}
    check(f"{label}: official rows read Krunker's map names as sent (Old_Burg, {MAP_NAME}; never Burg (1.5.2))",
          grep.get("kioOld", 0) >= 1 and grep.get("kioMap", 0) >= 1 and grep.get("kio152", -1) == 0, grep)
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
    check(f"{label}: Invite in the official game copies krunker.io's link (?game= the id as it is)",
          f"[party] invite link copied: https://krunker.io/?game={GAME_ID}" in text, text[-300:])
    # the stub's lobby: [GAME_ID, "de-fra", 3, 8, {"i": MAP_NAME, "g": 0}] (krunker_region_name: Frankfurt)
    check(f"{label}: the menu's region label names the official region", re.search(r"@region n=1 .*?text=Frankfurt\b", text) is not None,
          re.search(r"@region.*", text).group(0) if re.search(r"@region.*", text) else None)
    srv = re.search(r"\[console\] Lobby: (.*)", text)
    check(f"{label}: serverinfo names the official lobby, its region, map and mode",
          srv is not None and srv.group(1).startswith(f"{GAME_ID} | region Frankfurt (de-fra) | map {MAP_NAME} | mode ") and srv.group(1).endswith("(0)"),
          srv.group(1) if srv else None)
    check(f"{label}: serverinfo's players are the official list's (3/8)", "[console] Players: 3/8 (krunker.io's list)" in text, "")
    check(f"{label}: serverinfo's endpoint is krunker.io's game server", "[console] Endpoint: krunker.io official game server" in text, "")
    toasts = re.findall(r"\[input_script\] text t=\d+ @toast\d n=\d+ .*?text=(.*)", text)
    check(f"{label}: entering the game shows no toast (no \"Joining\" / \"Joined\" the krunker.io server)",
          len(toasts) == 2 and not any(re.search(r"Join(ing|ed) the krunker\.io", t) for t in toasts), toasts)
    check(f"{label}: no Turnstile token reaches the log", PASTE_TOKEN not in "\n".join(l for l in lines if "[input_script]" not in l))


BAD_MAP_INDEX = 999  # past krunker_maps.h's table, not in the (private) cache, and the maps API does not answer


def run_native_badmap():
    """A lobby whose init names a map index this client lacks: the client closes the krunker.io socket,
    the game stops and v10's Lost Connection screen says which map; no toast, no game left running."""
    label = "native-badmap"
    # the player's path: the offline game, the server browser, a click on the official lobby
    script = ("0 menu\n300 call showWindow 2\n3000 call krunkerJoin \"%s\"\n4500 clickid knCaptchaToken\n4600 type %s\n"
              "4700 clickid knCaptchaUse\n"
              "9000 text @toast [id=knNotify]\n9100 nodes @box .instructionsHeader\n9200 text @screen [id=instructionsUpdate]\n"
              "9300 state\n12000 state\n12100 text @toast2 [id=knNotify]\n12300 quit\n" % (GAME_ID, PASTE_TOKEN))
    dead_api = kn_proto.free_ports(1)[0]  # nothing listens: GET /maps/<index> gets no response
    env = {"KRUNKNATIVE_KRUNKER_MAPS_API": "http://127.0.0.1:%d" % dead_api,
           "KRUNKNATIVE_KRUNKER_MAP_CACHE": os.path.join(WORK, label + "-maps")}
    stub, p, out, log = launch_native(label, script, env, args=("--offline",), map_index=BAD_MAP_INDEX)
    lines = finish_native(label, stub, p, out, log, timeout=180)
    text = "\n".join(lines)
    check(f"{label}: the stub's init reached the client", f"init: map {BAD_MAP_INDEX}," in text, text[-400:])
    check(f"{label}: the client closed the krunker.io socket", stub.client_closed_t is not None and
          any(f"[krunker] leaving the official game" in l for l in lines), stub.client_closed_t)
    check(f"{label}: it never sent \"en\" (no game was entered)", not stub.got_en.is_set())
    screen = [l for l in lines if "[ui log] error screen" in l]
    check(f"{label}: the error screen says which map and why", any(f"Map {BAD_MAP_INDEX} " in l and "no response" in l for l in screen), screen[:2])
    box = re.search(r"nodes t=\d+ @box elements=\d+ match=(\d+)", text)
    check(f"{label}: the Lost Connection box is up", box and int(box.group(1)) >= 1, box.group(0) if box else None)
    shown = re.search(r"\[input_script\] text t=\d+ @screen n=\d+ .*?text=(.*)", text)
    check(f"{label}: the screen's text names the map", shown and f"Map {BAD_MAP_INDEX} " in shown.group(1), shown.group(1) if shown else None)
    toasts = re.findall(r"\[input_script\] text t=\d+ @toast2? n=\d+ .*?text=(.*)", text)
    check(f"{label}: no toast", len(toasts) == 2 and not any(t.strip() for t in toasts), toasts)
    st = re.findall(r"\[input_script\] state t=(\d+) .*", text)
    check(f"{label}: the client stays on the error screen (it does not reconnect by itself)",
          len(stub.ws_paths) == 1, stub.ws_paths)
    check(f"{label}: the menu is back with the box over it, the pointer free (no game left running)",
          len(st) == 2 and all(" menu=1 locked=0 " in l for l in re.findall(r"\[input_script\] state t=\d+ .*", text)), st)


KICK_TEXT, KICK_CODE = "Kicked for inactivity", "000005"


def run_native_actions():
    """Player actions in an official game (Stub.actions_phases): what our client sends for them and what it
    shows of the server's. Out: "ct" on the team tab ([1, text]), "k0" (streak slot 1), "spry" (F), "cd" "c"
    (a class pick in an official game), "mpos" {x, y, z} (M2 in a ranked game), "en" with v10's streak
    fields, "fpSp" [sid] / [] (spectating: "1", then F), "maVote" with the option's own mts string. In: the
    bot's spray decal, the ping markers, the spectator cam, the vote cards and their totals."""
    label = "native-actions"
    script = ["0 boot", "1500 clickid knCaptchaToken", "1600 type %s" % PASTE_TOKEN, "1700 clickid knCaptchaUse", "3800 play",
              "7000 call switchChat", "7300 con say team hello", "7800 down 1", "7900 up 1", "8300 down F", "8400 up F",
              "8800 call selectClass 2"]
    script += ["%d pings" % t for t in range(6000, 10500, 500)]
    script += ["10600 look 0 -1.2", "11100 state", "11200 mdown middle", "11300 mup middle"]
    script += ["%d pings" % t for t in range(12000, 15000, 500)]
    script += ["14500 down 1", "14600 up 1", "15500 down F", "15600 up F"]
    script += ["%d text @votes [id=matchVoteHolder]" % t for t in range(17000, 34000, 1000)]
    script += ["%d vis @end endUI" % t for t in range(17000, 34000, 1000)]
    script += ["24000 clicksel .matchVote", "34500 quit"]
    events = sorted(script, key=lambda l: int(l.split()[0]))
    # on HQ at its spawn 4 (run_native_hq): a floor under us for the ping's ray
    stub, p, out, log = launch_native(label, "\n".join(events) + "\n", {}, scenario="actions", map_index=HQ_INDEX, spawn=hq_spawn())
    lines = finish_native(label, stub, p, out, log, timeout=240)
    text = "\n".join(lines)
    check(f"{label}: the client joined as the stub's sid", f"joined as sid {MY_SID}" in text)
    for name in ("actions", "spectate", "end", "done"):
        check(f"{label}: the stub got to send its \"{name}\" packets", name in stub.sent, sorted(stub.sent))
    check(f"{label}: the server saw no ahNum / \"q\"-sequence violations", not stub.proto_errors, stub.proto_errors[:5])
    frames = [(n, a) for n, a, _ in stub.frames]

    def sent(name):
        return [a for n, a in frames if n == name]

    en = sent("en")
    check(f"{label}: \"en\" carries v10's streak picks (numbers, not NaN) and the pcStat", en and all(
        isinstance(v, int) for v in en[0][0][26:29]) and isinstance(en[0][0][13], int), en[0][0][13:14] + en[0][0][26:29] if en else None)
    ct = sent("ct")
    check(f"{label}: team chat: \"ct\" [1, text] on the team tab", any(a[:2] == [1, "team hello"] for a in ct), ct)
    check(f"{label}: the streak key: \"k0\" with no args", [] in sent("k0"), sent("k0"))
    check(f"{label}: the spray key: \"spry\" with no args", [] in sent("spry"), sent("spry"))
    cd = sent("cd")
    check(f"{label}: a class pick in an official game: \"cd\" \"c\" {{c: 2}}", any(a and a[0] == "c" and isinstance(a[1], dict) and a[1].get("c") == 2
                                                                            for a in cd), cd)
    mpos = sent("mpos")
    check(f"{label}: the ping (M2, ranked): \"mpos\" {{x, y, z}}", any(a and isinstance(a[0], dict) and set(a[0]) == {"x", "y", "z"} for a in mpos), mpos)
    fp = sent("fpSp")
    check(f"{label}: spectating: \"fpSp\" [the bot's sid], then [] (free cam)", [BOT_SID] in fp and [] in fp, fp)
    votes = sent("maVote")
    check(f"{label}: the vote click: \"maVote\" with the option's own mts string", votes and votes[-1][0] in Stub.ACTION_MTS, votes)
    check(f"{label}: the bot's spray decal (\"sp\")", "[sprays] decal %d_0 skin=2490" % BOT_SID in text)
    pings = [int(m.group(1)) for m in re.finditer(r"\[input_script\] pings t=\d+ shown=(\d+) spect=0", text)]
    check(f"{label}: \"mpos\" ping markers on screen", any(n >= 1 for n in pings), pings)
    check(f"{label}: \"start\" spectating: the spectator cam", re.search(r"\[input_script\] pings t=\d+ shown=\d+ spect=1", text) is not None)
    clean = lambda t: t.replace("\u200e", "").replace("\u200f", "")  # noqa: E731
    cards = [clean(m.group(1)) for m in re.finditer(r"\[input_script\] text t=\d+ @votes n=\d+ checked=\d+ value=\S* text=(.*)", text)]
    shown = [c for c in cards if "BASTION" in c.upper() and "LITTLETOWN" in c.upper()]
    check(f"{label}: END: the vote cards (the official list's name for a public option, mts's own for the other)", shown, cards[-1:])
    # the cards' totals: Bastion 2, Littletown 1, then (mv) our pick +1
    check(f"{label}: END: the totals from vo", any(re.search(r"Bastion\D*2(?!\d)", c, re.I) and re.search(r"Littletown\D*1(?!\d)", c, re.I) for c in shown), shown[:1])
    if votes and "mv" in stub.sent:
        pick = votes[-1][0]
        want = {"0,1": r"Littletown\D*2(?!\d)", "1,43,Bastion,0": r"Bastion\D*3(?!\d)", "3,7": r"Lostworld\D*1(?!\d)"}[pick]
        check(f"{label}: \"mv\": our pick's total went up", any(re.search(want, c, re.I) for c in shown), (pick, shown[-1:]))


def run_native_kick():
    """In an official game, the server's "error" (an idle kick) with the socket left open: the client
    closes the socket itself, the game stops, and v10's error screen says why (the server's text, v10's
    wording); no popup over it, no toast."""
    label = "native-kick"
    script = ("0 boot\n1500 clickid knCaptchaToken\n1600 type %s\n1700 clickid knCaptchaUse\n3800 play\n"
              "13000 text @toast [id=knNotify]\n13100 nodes @box .instructionsHeader\n13200 text @screen [id=instructionsUpdate]\n"
              "13300 vis @pop genericPop\n13400 state\n13600 quit\n" % PASTE_TOKEN)
    stub, p, out, log = launch_native(label, script, {}, scenario="kick")
    lines = finish_native(label, stub, p, out, log, timeout=180)
    text = "\n".join(lines)
    check(f"{label}: the client joined as the stub's sid", f"joined as sid {MY_SID}" in text)
    check(f"{label}: the stub sent its \"error\"", "error" in stub.sent, sorted(stub.sent))
    closed = stub.client_closed_t is not None and "error" in stub.sent and stub.client_closed_t - stub.sent["error"] < 5
    check(f"{label}: the client closed the socket itself (the stub kept it open)", closed,
          (stub.client_closed_t, stub.sent.get("error")))
    screen = [l for l in lines if "[ui log] error screen" in l]
    check(f"{label}: the error screen takes the server's reason", any(KICK_TEXT in l for l in screen), screen[:2])
    shown = re.search(r"\[input_script\] text t=\d+ @screen n=\d+ .*?text=(.*)", text)
    check(f"{label}: the Lost Connection box shows it (v10's idle-kick wording)", shown and "Disconnected due to inactivity" in shown.group(1), shown.group(1) if shown else None)
    check(f"{label}: no error popup over it", "@pop genericPop=0" in text, [l for l in lines if "@pop" in l])
    toast = re.search(r"\[input_script\] text t=\d+ @toast n=\d+ .*?text=(.*)", text)
    check(f"{label}: no toast", toast is not None and not toast.group(1).strip(), toast.group(0) if toast else None)
    st = re.findall(r"\[input_script\] state t=\d+ .*", text)
    check(f"{label}: the game stopped: the menu is back, the pointer free", st and " menu=1 locked=0 " in st[-1], st[-1:])
    check(f"{label}: no reconnect by itself", len(stub.ws_paths) == 1, stub.ws_paths)


def run_native_fx():
    """Combat visuals / sounds (krunker_link.c fx): the stub (Stub.fx_phases) has its bot shoot, fire a
    rocket, explode and play sounds; the client's vfx / projectile / audio logs show what it made of them."""
    label = "native-fx"
    script = "0 boot\n1500 clickid knCaptchaToken\n1600 type %s\n1700 clickid knCaptchaUse\n3800 play\n15000 quit\n" % PASTE_TOKEN
    stub, p, out, log = launch_native(label, script, {"KRUNKNATIVE_VFX_LOG": "1", "KRUNKNATIVE_AUDIO_LOG": "1"}, scenario="fx")
    lines = finish_native(label, stub, p, out, log, timeout=180)
    text = "\n".join(lines)
    check(f"{label}: the client joined as the stub's sid", f"joined as sid {MY_SID}" in text)
    for name in ("fx", "done"):
        check(f"{label}: the stub got to send its \"{name}\" packets", name in stub.sent, sorted(stub.sent))
    check(f"{label}: the server saw no ahNum / \"q\"-sequence violations", not stub.proto_errors, stub.proto_errors[:5])
    check(f"{label}: SHOW_TRACER: the bot's tracer to 8, 3, 8", re.search(r"\[vfx\] remote shot from .* to 8\.0 3\.0 8\.0", text),
          [l for l in lines if "[vfx]" in l][:5])
    audio = [l for l in lines if "[audio]" in l and " play" in l]
    check(f"{label}: PLAYER_SOUND: sounds[5] (weapon_1) plays in 3D, once (the tracer adds none)",
          sum(1 for l in audio if "play3d weapon_1 " in l) == 1, audio[:8])
    check(f"{label}: SHOW_PROJECTILE: the rocket (weapon 8) flies", re.search(r"\[proj\] spawn w8 box at 0\.0 6\.0 0\.0 v [-\d.]+ [-\d.]+ 0\.\d+", text),
          [l for l in lines if "[proj]" in l][:5])
    check(f"{label}: END_PROJECTILE: ends it with its impact", re.search(r"\[proj\] end id \d+ at 0\.0 6\.0 20\.0 impact 1", text),
          [l for l in lines if "[proj]" in l][:5])
    check(f"{label}: EXPLOSION: the blast at 6, 1, 6", "[vfx] explosion 6.0 1.0 6.0" in text, [l for l in lines if "explosion" in l][:4])
    check(f"{label}: PLAY_SOUND: cheer_0 in 2D", any("play2d cheer_0 " in l for l in audio), audio[-6:])
    check(f"{label}: NUKE_UPDATE 0: the siren", any("play2d siren_0 " in l for l in audio), audio[-6:])


def run_native_join_end():
    """Play, then the official lobby's round turns out to be over (the user's report: "the camera just
    continues rotating like I'm in the menu, until the game actually starts"): the server's "end" type 1
    shows the end screen with its results at once (v10.js:219715, no victory title for a player who is not
    in the game) and "Next Round in"; the next round's init takes it down and we enter it (like a
    KrunkNative server's next round: the pointer locks again, client_enter_game)."""
    label = "native-join-end"
    script = ("0 boot\n1500 clickid knCaptchaToken\n1600 type %s\n1700 clickid knCaptchaUse\n3800 play\n"
              "9000 vis @end endUI endInfo endMidHolder menuHolder inGameUI instructionHolder\n9100 text @timer [id=endTimer]\n"
              "9200 grep @table StubBot [id=endTable]\n9300 state\n"
              "22000 vis @next endUI menuHolder\n22500 play\n27000 vis @playing inGameUI endUI menuHolder\n27100 players\n27500 quit\n"
              % PASTE_TOKEN)
    stub, p, out, log = launch_native(label, script, {}, scenario="joinend")
    lines = finish_native(label, stub, p, out, log, timeout=180)
    text = "\n".join(lines)
    check(f"{label}: the stub sent its \"end\" type 1, then the next round's init and \"frs\"",
          all(k in stub.sent for k in ("end", "init2", "frs")), sorted(stub.sent))
    end = re.search(r"\[input_script\] vis t=\d+ @end (.*)", text)
    want = {"endUI": "1", "endInfo": "1", "endMidHolder": "1", "menuHolder": "0", "inGameUI": "0", "instructionHolder": "0"}
    got = dict(kv.split("=", 1) for kv in end.group(1).split()) if end else {}
    check(f"{label}: Play during the end screen: the end screen with its results, not the menu camera", got == want, got)
    timer = re.search(r"\[input_script\] text t=\d+ @timer n=\d+ .*?text=(.*)", text)
    check(f"{label}: \"Next Round in\" (t state 1)", timer and "Next Round in" in timer.group(1), timer.group(1) if timer else None)
    table = re.search(r"\[input_script\] grep t=\d+ @table needle=\S+ n=\d+ hits=(\d+)", text)
    check(f"{label}: the end table lists the lobby's players", table and int(table.group(1)) >= 1, table.group(0) if table else None)
    nxt = re.search(r"\[input_script\] vis t=\d+ @next (.*)", text)
    check(f"{label}: the next round's init takes the end screen down", nxt and nxt.group(1).split()[0] == "endUI=0",
          nxt.group(1) if nxt else None)
    playing = re.search(r"\[input_script\] vis t=\d+ @playing (.*)", text)
    check(f"{label}: then Play spawns us (the HUD)", playing and playing.group(1).split() == ["inGameUI=1", "endUI=0", "menuHolder=0"],
          playing.group(1) if playing else None)


def run_native_leave_state():
    """Official -> KrunkNative -> official: leaving a game clears its state (client_leave_game; v10 leaves a
    game by loading a new page): the official lobby's chat line is gone on the KrunkNative server, the
    KrunkNative server's chat line is gone in the next official lobby. That second join starts from inside
    the KrunkNative game with the menu closed: the security challenge must free the pointer and take the
    clicks (it used to show while every click went to the game)."""
    label = "native-leave-state"
    (port,) = kn_proto.free_ports(1)
    slog = open(os.path.join(WORK, label + "-server.log"), "w")
    server = subprocess.Popen([f"{BUILD}/server_bin", "--headless", "--cfg", "none", "--port", str(port)], stdout=slog, stderr=subprocess.STDOUT,
                              stdin=subprocess.DEVNULL, start_new_session=True, cwd=ROOT)
    cfg = os.path.join(WORK, label + "-cfg", "krunknative")
    os.makedirs(cfg, exist_ok=True)
    with open(os.path.join(cfg, "knSettings.json"), "w") as f:
        f.write('{"showMessages":true}')  # player chat on (KrunkNative's default is off; v10's is on)
    token2 = PASTE_TOKEN.replace("Pasted", "Second")
    script = ("0 boot\n1500 clickid knCaptchaToken\n1600 type %s\n1700 clickid knCaptchaUse\n3800 play\n"
              "8000 grep @official hello [id=chatList]\n"
              "8100 con connect 127.0.0.1:%d\n8300 grep @left-official hello [id=chatList]\n"
              "12000 play\n15000 grep @kn hello [id=chatList]\n15000 nodes @kn [id=chatList] .chatItem\n15100 con say zzknmark\n"
              "16500 grep @kn-said zzknmark [id=chatList]\n"
              "16500 state\n17000 call krunkerJoin \"%s\"\n17300 grep @left-kn zzknmark [id=chatList]\n17300 nodes @left-kn [id=chatList] .chatItem\n"
              "18300 vis @challenge knCaptchaHolder menuHolder :cursor\n18400 state\n"
              "18500 clickid knCaptchaToken\n18600 type %s\n18700 clickid knCaptchaUse\n21000 play\n"
              "25000 grep @official2 zzknmark [id=chatList]\n25100 grep @official2-stub hello [id=chatList]\n25200 state\n25500 quit\n"
              % (PASTE_TOKEN, port, GAME_ID, token2))
    try:
        stub, p, out, log = launch_native(label, script, {})
        lines = finish_native(label, stub, p, out, log, timeout=180)
    finally:
        if server.poll() is None:
            os.killpg(server.pid, signal.SIGTERM)
            try:
                server.wait(timeout=10)
            except subprocess.TimeoutExpired:
                os.killpg(server.pid, signal.SIGKILL)
        slog.close()
    text = "\n".join(lines)
    hits = {m.group(1): int(m.group(2)) for m in re.finditer(r"\[input_script\] grep t=\d+ @(\S+) needle=\S+ n=\d+ hits=(\d+)", text)}
    rows = {m.group(1): int(m.group(2)) for m in re.finditer(r"\[input_script\] nodes t=\d+ @(\S+) elements=\d+ match=(\d+)", text)}
    check(f"{label}: the official lobby's chat line showed", hits.get("official") == 1, hits)
    check(f"{label}: official -> KrunkNative: it is gone as the game is left", hits.get("left-official") == 0, hits)
    check(f"{label}: and on the KrunkNative server (only its own join lines)", hits.get("kn") == 0 and 1 <= rows.get("kn", 0) <= 3, (hits, rows))
    check(f"{label}: the KrunkNative server's chat line showed", hits.get("kn-said") == 1, hits)
    check(f"{label}: KrunkNative -> official: it is gone as the game is left", hits.get("left-kn") == 0 and rows.get("left-kn") == 0, (hits, rows))
    check(f"{label}: and in the next official lobby, which shows its own", hits.get("official2") == 0 and hits.get("official2-stub") == 1, hits)
    # the second join starts in a game with the menu closed (pointer locked): the security challenge
    # frees the pointer and takes the clicks and the typing (v10 exits the lock for its modals), the
    # game locks it again once the join is through
    states = {m.group(1): m.group(0) for m in re.finditer(r"\[input_script\] state t=(\d+) .*", text)}
    ordered = [states[k] for k in sorted(states, key=int)]
    check(f"{label}: the second join starts in the game (menu closed, pointer locked)",
          len(ordered) == 3 and " menu=0 locked=1 " in ordered[0], ordered[:1])
    chal = re.search(r"\[input_script\] vis t=\d+ @challenge (.*)", text)
    check(f"{label}: the challenge over the game: shown, no menu, the cursor free",
          chal and chal.group(1).split() == ["knCaptchaHolder=1", "menuHolder=0", ":cursor=1"], chal.group(1) if chal else None)
    check(f"{label}: and the pointer released (no controls, cursor normal)", len(ordered) == 3 and " locked=0 " in ordered[1] and
          "cursor=normal" in ordered[1], ordered[1:2])
    check(f"{label}: the pasted token went through (clicks and typing reached the modal)",
          text.count("[krunker] captcha: a pasted token") == 2, text.count("[krunker] captcha: a pasted token"))
    check(f"{label}: back in the game afterwards: pointer locked, no menu", len(ordered) == 3 and " menu=0 locked=1 " in ordered[2]
          and "cursor=disabled" in ordered[2], ordered[2:])


def run_native_optout():
    """KRUNKNATIVE_KRUNKER_JOIN=browser: the click opens krunker.io/?game=<id> in the system browser
    (KRUNKNATIVE_NO_BROWSER logs it), nothing is sought in-client."""
    label = "native-optout"
    script = ("0 menu\n300 call showWindow 2\n3000 text @status [id=kioStatus]\n3200 call krunkerJoin \"%s\"\n4200 quit\n" % GAME_ID)
    stub, p, out, log = launch_native(label, script, {"KRUNKNATIVE_KRUNKER_JOIN": "browser"}, args=("--offline",))
    lines = finish_native(label, stub, p, out, log, timeout=120)
    text = "\n".join(lines)
    st = re.search(r"\[input_script\] text t=\d+ @status n=1 .*?text=(.*)", text)
    check(f"{label}: the KRUNKER.IO section says its lobbies open in the browser", st and "krunker.io servers open in your browser" in st.group(1),
          st.group(1) if st else None)
    check(f"{label}: the click opens the lobby in the browser", f"open (disabled by KRUNKNATIVE_NO_BROWSER): https://krunker.io/?game={GAME_ID}" in text,
          text[-400:])
    check(f"{label}: nothing is sought in-client", not any(pth == "/seek-game" for pth, _, _ in stub.http_log) and not stub.ws_paths and
          "joining official game" not in text, [pth for pth, _, _ in stub.http_log])


# ---------------------------------------------------------------- native: Find New Game excludes (recent_lobbies.h)

EXCLUDE_LOBBIES = [("FRA:aaaaa", 6), ("FRA:bbbbb", 5), ("FRA:ccccc", 4)]  # busiest first


def find_new_game_script(times, first="0 menu\n", quit_at=None):
    lines = [first, '100 call setSetting "knSeekServers" "official"\n']
    lines += ["%d call backToOrigin\n" % t for t in times]
    lines.append("%d quit\n" % (quit_at or times[-1] + 3500))
    return "".join(lines)


def run_native_exclude():
    """Find New Game (Quick Match / F4, Settings > Quickplay > Official) from the lobby list
    (krunker_lobbies_pick): never the game the player is in, never back to the one before it while
    another has room; with every lobby recent, the least recent one."""
    label = "native-exclude"
    stub, p, out, log = launch_native(label, find_new_game_script([500, 4000, 7500, 11000]), {}, args=("--offline",),
                                      stub_setup=lambda st: setattr(st, "lobbies", EXCLUDE_LOBBIES))
    lines = finish_native(label, stub, p, out, log, timeout=180)
    text = "\n".join(lines)
    games = [dict(q).get("game") for pth, q, _ in stub.http_log if pth == "/seek-game"]
    check(f"{label}: each Find New Game joins another lobby: the busiest, then the next ones, then the least recent "
          "(never the one it leaves, no bouncing between two)", games == ["FRA:aaaaa", "FRA:bbbbb", "FRA:ccccc", "FRA:aaaaa"], games)
    check(f"{label}: no excludeGame needed (the list picks)", not any("excludeGame" in dict(q) for pth, q, _ in stub.http_log if pth == "/seek-game"))
    check(f"{label}: the last pick says it is a recent one", "[krunker] seek: official lobby FRA:aaaaa (a recent one: nothing else has room)" in text,
          [l for l in lines if "[krunker] seek:" in l])
    check(f"{label}: the list was fetched once (cached)", stub.list_requests == 1, stub.list_requests)
    check(f"{label}: each join opened its game's socket", [pth.split("&")[0] for pth in stub.ws_paths] ==
          ["/ws?gameId=" + g for g in games], stub.ws_paths)


def run_native_exclude_mm():
    """Find New Game with no lobby list from krunker_lobbies (its /game-list refused): krunker_net_seek_new
    asks for the list itself and picks a game it was not in lately; once every listed game is recent, the
    live client's way: seek-game with excludeGame (the game it leaves); an answer that is another recent
    game is sought again excluding it, and the least recent answer is joined."""
    label = "native-exclude-mm"

    def setup(st):
        st.lobbies = EXCLUDE_LOBBIES
        st.list_fail = 1  # krunker_lobbies' request; krunker_net's own ones are answered

    stub, p, out, log = launch_native(label, find_new_game_script([3000, 6500, 10000], first="0 boot\n"), {}, args=("--krunker",),
                                      stub_setup=setup)
    lines = finish_native(label, stub, p, out, log, timeout=180)
    text = "\n".join(lines)
    seeks = [dict(q) for pth, q, _ in stub.http_log if pth == "/seek-game"]
    got = [(q.get("game"), q.get("excludeGame")) for q in seeks]
    want = [(None, None),                 # --krunker: the matchmaker picks (aaaaa)
            ("FRA:bbbbb", None),          # F4 1: the list (refused to krunker_lobbies, asked again): not aaaaa
            ("FRA:ccccc", None),          # F4 2: nor bbbbb
            (None, "FRA:ccccc"),          # F4 3: every listed game is recent: excludeGame = the one it leaves -> aaaaa
            (None, "FRA:aaaaa"),          #   aaaaa is recent: again without it -> bbbbb
            (None, "FRA:bbbbb")]          #   bbbbb is recent too: again -> aaaaa, the least recent answer: joined
    check(f"{label}: list picks, then excludeGame seeks around the recent games", got == want, got)
    excl = [q for q in seeks if q.get("excludeGame")]
    check(f"{label}: an excludeGame seek has no game and keeps the live order (excludeGame before dataQuery)",
          excl and all("game" not in q for q in excl) and all(r.index("excludeGame=") < r.index("dataQuery=")
                                                              for pth, _, r in stub.http_log if pth == "/seek-game" and "excludeGame=" in r), excl[:1])
    check(f"{label}: Find New Game's dataQuery carries the build version", excl and all(json.loads(q["dataQuery"]).get("v") == BUILD_VERSION
                                                                                     for q in excl), excl[:1])
    joins = re.findall(r"\[krunker\] seek-game: game (\S+) on", text)
    check(f"{label}: the games joined: aaaaa, bbbbb, ccccc, then the least recent answer (aaaaa)",
          joins == ["FRA:aaaaa", "FRA:bbbbb", "FRA:ccccc", "FRA:aaaaa"], joins)
    check(f"{label}: krunker_lobbies' refused list led to the matchmaker path",
          "[krunker] seek: no lobby list" in text and "[krunker] game-list: FRA:bbbbb" in text, [l for l in lines if "[krunker]" in l][-12:])


def run_native_reload(gone):
    """Reload (F5) in an official game: the client relaunches with the same command line (--krunker, which
    alone joins whatever game the matchmaker picks) and comes back into the official game it was in, by
    its id (never a seek by the Quickplay settings or excludeGame). gone: that game ended meanwhile
    (InvalidGameId): the reload says so and the matchmaker picks another game."""
    label = "native-reload-gone" if gone else "native-reload"

    def setup(st):
        st.lobbies = EXCLUDE_LOBBIES
        if gone:
            st.gone_after = {"FRA:bbbbb": 1}  # the first process joins it; the relaunched one finds it gone

    relaunch = os.path.join(WORK, label + "-relaunch.script")
    with open(relaunch, "w") as f:
        f.write("0 boot\n6000 players\n6500 quit\n")
    stub, p, out, log = launch_native(label, '0 boot\n3000 call krunkerJoin "FRA:bbbbb"\n7000 down F5\n7050 up F5\n90000 quit\n',
                                      {"KRUNKNATIVE_INPUT_SCRIPT_RELAUNCH": relaunch}, args=("--krunker",), stub_setup=setup)
    lines = finish_native(label, stub, p, out, log, timeout=180)
    text = "\n".join(lines)
    after = text[text.find("[app] reload: restarting"):] if "[app] reload: restarting" in text else ""
    seeks = [dict(q) for pth, q, _ in stub.http_log if pth == "/seek-game"]
    got = [(q.get("game"), q.get("excludeGame")) for q in seeks]
    check(f"{label}: F5 relaunched the client", "[reload] restart: F5: relaunch" in text and after, text[-400:])
    check(f"{label}: the session handed over is the official game", "[reload] resume after the restart: krunker FRA:bbbbb region " in after,
          after[:400])
    joins = re.findall(r"\[krunker\] seek-game: game (\S+) on", after)
    if not gone:
        check(f"{label}: --krunker joined the matchmaker's pick, krunkerJoin bbbbb, the reload bbbbb again by its id (no seek "
              "without a game, no excludeGame)", got == [(None, None), ("FRA:bbbbb", None), ("FRA:bbbbb", None)], got)
        check(f"{label}: the relaunched client plays bbbbb", joins == ["FRA:bbbbb"], joins)
    else:
        check(f"{label}: the reload asked for bbbbb, then (gone) one seek with no game and no excludeGame",
              got == [(None, None), ("FRA:bbbbb", None), ("FRA:bbbbb", None), (None, None)], got)
        check(f"{label}: and said so", "[krunker] reload: official game FRA:bbbbb is gone: seeking another game" in after,
              [l for l in after.splitlines() if "[krunker]" in l][-8:])
        check(f"{label}: the relaunched client plays the matchmaker's pick", joins == ["FRA:aaaaa"], joins)
    check(f"{label}: no KrunkNative seek in the relaunched client (no launch seek)",
          "[mm] launch seek" not in after and "[mm] joining" not in after, "")


# ---------------------------------------------------------------- native: the live Krunker build

LIVE_SCRIPT = ("0 boot\n4000 play\n5000 text @timer0 [id=timerVal]\n5500 text @feed0 [id=killFeed]\n6500 text @feed1 [id=killFeed]\n"
               "7500 text @feed2 [id=killFeed]\n8000 players\n8100 down W\n9500 up W\n9700 players\n9900 con say hello official\n"
               "10500 text @timer1 [id=timerVal]\n11000 players\n12500 text @timer2 [id=timerVal]\n13000 players\n13500 quit\n")


def run_native_live():
    """Krunker shipped a build that is not the compiled one: the client takes it from the published manifest
    and plays the whole game in it (another buildVersion and AHKEY, renamed packets, moved offsets)."""
    label = "native-live"
    published = {}

    def setup(st):
        st.lobbies = [(GAME_ID, 3)]  # no captcha: the build is what this run is about
        doc = next_build_doc()
        st.speak(LiveBuild(doc))
        published.update(publish(st, doc, label))

    stub, p, out, log = launch_native(label, LIVE_SCRIPT, {}, stub_setup=setup)
    lines = finish_native(label, stub, p, out, log, timeout=180)
    text = "\n".join(lines)
    paths = [pth for pth, _, _ in stub.http_log if pth != "/game-list"]  # (the server browser's list may come first)
    doc_path = "/updates/" + published.get("path", "?")
    check(f"{label}: the manifest, then the document it names, then seek-game",
          paths[:3] == ["/updates/client/krunker-build.json", doc_path, "/seek-game"], paths[:4])
    check(f"{label}: the client took the published build (live {LIVE_CLIENT}, AHKEY 0x{LIVE_AHKEY:x})",
          f"[krunker] Krunker build: live {LIVE_CLIENT} (buildVersion {LIVE_VERSION}, AHKEY 0x{LIVE_AHKEY:x})" in text,
          [l for l in lines if "Krunker build" in l][:3])
    seeks = [dict(q) for pth, q, _ in stub.http_log if pth == "/seek-game"]
    check(f"{label}: seek-game carries the published buildVersion (the matchmaker's version gate)",
          seeks and all(json.loads(q.get("dataQuery", "{}")).get("v") == LIVE_VERSION for q in seeks) and not stub.bad_version_seeks,
          (seeks[:1], stub.bad_version_seeks))
    text = check_run(label, stub, lines, True)  # every frame in the live build's names, trailers on its AHKEY
    names = [f[0] for f in stub.frames]
    check(f"{label}: \"Pen\" / \"Pq\" / \"Pv\" / \"Pld\" (the renamed packets) arrived as en / q / v / load",
          {"en", "q", "v", "load"} <= set(names), sorted(set(names)))
    rows = [l for l in lines if "[input_script] players" in l]
    xs = [float(m.group(1)) for m in (re.search(r" %d:StubBot@([-\d.]+)," % BOT_SID, r) for r in rows) if m]
    check(f"{label}: the stub player moves (\"Pk\": 15 values a player, sid at 13, x at 14)", len(xs) >= 2 and xs[-1] > xs[0], xs)
    check(f"{label}: our player is in the game (\"P0\": 56 values a player, sid at 53)",
          any(re.search(r" %d:[^ ]*\*" % MY_SID, r) for r in rows), rows[-1:])
    check_killfeed_names(label, lines)  # "P3": victim and killer swapped places
    check_timer_countdown(label, lines)
    cache = os.path.join(WORK, label + "-build-cache", "krunker-build.json")
    check(f"{label}: the published document is cached", os.path.isfile(cache) and
          hashlib.sha256(open(cache, "rb").read()).hexdigest() == published.get("sha256"))


def run_native_live_refused():
    """The published build lost a field the client reads: the join fails on the error screen, naming the
    build and the reason, and never reaches the matchmaker (no silent desync)."""
    label = "native-live-refused"

    def setup(st):
        publish(st, next_build_doc(lose_field=("add_players", "sid")), label)

    stub, p, out, log = launch_native(label, "0 boot\n4000 text @inst [id=instructionsUpdate]\n4300 quit\n", {}, stub_setup=setup)
    lines = finish_native(label, stub, p, out, log, timeout=120)
    text = "\n".join(lines)
    paths = [pth for pth, _, _ in stub.http_log if pth != "/game-list"]  # (the server browser's list may come first)
    check(f"{label}: the manifest and the document, no seek-game", paths[:2] and paths[0] == "/updates/client/krunker-build.json" and
          "/seek-game" not in paths and not stub.ws_paths, paths)
    check(f"{label}: the log says which build was refused and why",
          f'[krunker] Krunker build {LIVE_CLIENT} refused: ADD_PLAYERS no longer has "sid"' in text, [l for l in lines if "refused" in l][:3])
    inst = re.search(r"\[input_script\] text t=\d+ @inst n=\d+ checked=\d+ value=[^ ]* text=(.*)", text)
    check(f"{label}: the error screen says krunker.io updated, the client needs an update, and why",
          inst and f"krunker.io updated to Krunker {LIVE_CLIENT}: this client needs an update to join its games" in inst.group(1) and
          'ADD_PLAYERS no longer has "sid"' in inst.group(1), inst.group(1) if inst else None)


def run_native_live_stale():
    """The matchmaker is the judge: it refuses the published build's buildVersion (InvalidVersion: no server
    runs it), the client asks the manifest again (the same build), then seeks with the compiled build, which
    the servers run, and joins."""
    label = "native-live-stale"

    def setup(st):
        st.lobbies = [(GAME_ID, 3)]
        publish(st, next_build_doc(), label)  # published, but the stub's servers run the compiled build

    stub, p, out, log = launch_native(label, "0 boot\n5000 play\n7000 players\n7300 quit\n", {}, stub_setup=setup)
    lines = finish_native(label, stub, p, out, log, timeout=120)
    text = "\n".join(lines)
    paths = [pth for pth, _, _ in stub.http_log if pth not in ("/game-list",) and not pth.startswith("/updates/krunker/")]
    check(f"{label}: manifest, seek-game (refused), the manifest again (forced), seek-game",
          paths[:4] == ["/updates/client/krunker-build.json", "/seek-game", "/updates/client/krunker-build.json", "/seek-game"], paths[:5])
    vs = [json.loads(dict(q).get("dataQuery", "{}")).get("v") for pth, q, _ in stub.http_log if pth == "/seek-game"]
    check(f"{label}: the published buildVersion, then the compiled one", vs[:2] == [LIVE_VERSION, BUILD_VERSION], vs)
    check(f"{label}: the log says why it seeks again", "seek-game: InvalidVersion for buildVersion %s" % LIVE_VERSION in text and
          "trying the compiled build" in text, [l for l in lines if "[krunker]" in l and ("build" in l.lower())][:6])
    check(f"{label}: the client joined as the stub's sid", f"joined as sid {MY_SID}" in text)


def run_native_live_fallback():
    """The manifest cannot be fetched (503) and nothing is cached: the compiled build joins as before."""
    label = "native-live-fallback"

    def setup(st):
        st.lobbies = [(GAME_ID, 3)]
        st.published["/updates/client/krunker-build.json"] = (503, b"unavailable")

    stub, p, out, log = launch_native(label, "0 boot\n4000 play\n6000 players\n6300 quit\n", {}, stub_setup=setup)
    lines = finish_native(label, stub, p, out, log, timeout=120)
    text = "\n".join(lines)
    paths = [pth for pth, _, _ in stub.http_log if pth != "/game-list"]  # (the server browser's list may come first)
    check(f"{label}: the manifest (503), then seek-game", paths[:2] == ["/updates/client/krunker-build.json", "/seek-game"], paths[:3])
    check(f"{label}: the compiled build, saying why", re.search(r"\[krunker\] Krunker build: compiled \S+ \(buildVersion %s, AHKEY 0x%x\): .*HTTP 503"
                                                               % (re.escape(BUILD_VERSION), AHKEY), text) is not None,
          [l for l in lines if "Krunker build" in l][:2])
    seeks = [dict(q) for pth, q, _ in stub.http_log if pth == "/seek-game"]
    check(f"{label}: seek-game carries the compiled buildVersion", seeks and json.loads(seeks[0].get("dataQuery", "{}")).get("v") == BUILD_VERSION)
    check(f"{label}: the client joined as the stub's sid", f"joined as sid {MY_SID}" in text)


# ---------------------------------------------------------------- native: the challenge window (kn_webview.h)

WEBVIEW_SOLVE_MS = 1500  # the stub widget's "player" solves it this long after it is shown


def webview_token(n):  # the token the stub page's widget gives on its n-th load (1-based)
    return "0.e2eWebviewToken_%d_%s.sig-WvW_789" % (n, "w" * 250)


FORGED_TOKEN = "0.e2eForgedByAnotherOrigin_" + "f" * 120 + ".sig-FoR_000"

# The stand-in for the krunker.io page: a stub of Cloudflare api.js's `turnstile` (render(el, opts) /
# remove(id)), which reports how it was called and then fires the widget's own callback, like a player
# solving it; and a frame of another origin trying to hand the client a forged token.
WEBVIEW_PAGE = """<!doctype html>
<html><head><meta charset="utf-8"><title>krunker.io (e2e stub)</title>
<script>
(function () {
  var probe = function (q) { var i = new Image(); i.src = '/probe?' + q; };
  var h = window.webkit && window.webkit.messageHandlers && window.webkit.messageHandlers.knCaptcha;
  probe('page=1&main_handler=' + (h ? 1 : 0));
  window.turnstile = {
    render: function (el, o) {
      var box = document.createElement('div');
      box.style.cssText = 'border:1px solid #888;padding:14px 18px;background:#f4f4f4;color:#222';
      box.textContent = 'stub challenge: the player is solving it';
      el.appendChild(box);
      probe('render=1&sitekey=' + encodeURIComponent(o.sitekey) + '&appearance=' + encodeURIComponent(o.appearance) +
            '&callback=' + (typeof o.callback) + '&visibility=' + encodeURIComponent(document.visibilityState));
      // interactive: Turnstile's managed mode wants the player first (the window must show)
      if (%(interactive)s && o['before-interactive-callback']) o['before-interactive-callback']();
      setTimeout(function () { box.textContent = 'stub challenge: solved'; o.callback(%(token)s); }, %(delay)d);
      return 'stub-widget';
    },
    remove: function () {}
  };
})();
</script></head>
<body><pre>User-agent: *
Allow: /</pre>
<iframe src="%(forge)s" style="width:40px;height:20px;border:0"></iframe>
</body></html>
"""

FORGE_PAGE = """<!doctype html><html><body><script>
(function () {
  var probe = function (q) { var i = new Image(); i.src = '/probe?' + q; };
  var t = %(token)s;
  var h = window.webkit && window.webkit.messageHandlers && window.webkit.messageHandlers.knCaptcha;
  probe('forge=1&handler=' + (h ? 1 : 0));
  try { if (h) h.postMessage('token:' + t); } catch (e) {}
  try {
    top.document.documentElement.setAttribute('data-kn-captcha', 'token:' + t);
    top.document.dispatchEvent(new Event('kn-captcha'));
    probe('forge=1&top=reached');
  } catch (e) { probe('forge=1&top=blocked'); }
})();
</script></body></html>
"""


class ChallengePage:
    """The test's krunker.io page (KRUNKNATIVE_KRUNKER_CAPTCHA_URL=http://127.0.0.1:<port>/robots.txt) and,
    on the same port as http://localhost:<port>, the frame of another origin."""

    def __init__(self, port, interactive=False):
        self.port, self.loads, self.probes, self.paths = port, [], [], []
        self.interactive = interactive
        self.httpd = None

    def start(self):
        page = self

        class H(BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def do_GET(self):
                u = urllib.parse.urlsplit(self.path)
                page.paths.append(self.path)
                if u.path == "/robots.txt":
                    page.loads.append(self.headers.get("User-Agent", ""))
                    body = WEBVIEW_PAGE % {"token": json.dumps(webview_token(len(page.loads))), "delay": WEBVIEW_SOLVE_MS,
                                           "forge": "http://localhost:%d/forge" % page.port,
                                           "interactive": "true" if page.interactive else "false"}
                    ctype = "text/html; charset=utf-8"
                elif u.path == "/forge":
                    body, ctype = FORGE_PAGE % {"token": json.dumps(FORGED_TOKEN)}, "text/html; charset=utf-8"
                elif u.path == "/probe":
                    page.probes.append(dict(urllib.parse.parse_qsl(u.query)))
                    body, ctype = "", "text/plain"
                else:
                    self.send_response(404)
                    self.end_headers()
                    return
                data = body.encode()
                self.send_response(200)
                self.send_header("Content-Type", ctype)
                self.send_header("Content-Length", str(len(data)))
                self.send_header("Cache-Control", "no-store")
                self.end_headers()
                self.wfile.write(data)

        self.httpd = ThreadingHTTPServer(("127.0.0.1", self.port), H)
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def stop(self):
        if self.httpd:
            self.httpd.shutdown()
            self.httpd.server_close()


def webview_skip_reason():
    if not os.path.isfile(os.path.join(BUILD, "libkn_webview.a")):
        return "this client_bin was built without a webview (KN_WEBVIEW off, or webkit2gtk-4.1 not found)"
    if not shutil.which("Xvfb"):
        return "Xvfb is missing (the challenge window needs an X display of its own)"
    return None


def session_alive(pgid):
    try:
        os.killpg(pgid, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        return True


XVFB_W, XVFB_H = 1280, 800


def webview_session(label, script, page, used_first, stub_setup=None):
    """client_bin (headless game) with the challenge window on a private Xvfb whose top-level windows a
    watcher records (x11_watch.py: no window manager, so a window maps where the client put it)."""
    xvfb = PrivateXvfb("%dx%dx24" % (XVFB_W, XVFB_H))
    watch = X11Watch(xvfb.display)
    url = "http://127.0.0.1:%d/robots.txt" % page.port
    p = None
    try:
        stub, p, out, log = launch_native(label, script, {"KRUNKNATIVE_KRUNKER_CAPTCHA_URL": url}, display=xvfb.display, stub_setup=stub_setup)
        if used_first:
            stub.used_captchas.add(webview_token(1))  # the matchmaker refuses the first one (used / expired)
        lines = finish_native(label, stub, p, out, log, timeout=180)
        left = session_alive(p.pid)
        if left:
            time.sleep(3)
            left = session_alive(p.pid)
        time.sleep(0.2)
        maps = watch.maps(include_override=True)
        moves = {m["window"]: watch.moves_after_map(m["window"]) for m in maps}
    finally:
        if p is not None and session_alive(p.pid):
            os.killpg(p.pid, signal.SIGKILL)
        page.stop()
        watch.stop()
        xvfb.stop()
    return stub, lines, log, left, maps, moves


def run_native_webview():
    """The challenge window when Turnstile passes by itself: it runs hidden and is never mapped; a
    refused first token starts it again (hidden too), the second joins."""
    label = "native-webview"
    (page_port,) = kn_proto.free_ports(1)
    page = ChallengePage(page_port)
    page.start()
    # the modal's probes from the first seek until the game is in: both attempts of the matchmaker's
    # challenge (the stub solves each 1.5 s after it renders, the second starts after the first token is
    # refused), the join, then the game socket's own ALTCHA "cpt", which the client answers itself: it
    # shows no card either (the player sees only the loading state). Every 400 ms: a card would stay
    # longer than that.
    probe_t = list(range(800, 15601, 400))
    probes = "".join("%d vis @silent knCaptchaHolder knCaptchaSolve knCaptchaManual knCaptchaCancel\n" % t for t in probe_t)
    script = "0 boot\n" + probes + "16000 play\n21000 players\n21200 quit\n"
    stub, lines, log, left, maps, moves = webview_session(label, script, page, True)
    text = "\n".join(lines)
    check(f"{label}: the challenge passed by itself: no window was ever mapped on the display", not maps,
          [(m["name"], m["x"], m["y"]) for m in maps])
    check(f"{label}: ... it ran hidden both times and was never shown",
          text.count("[webview] challenge running hidden") == 2 and "[webview] challenge window shown" not in text and
          text.count("[webview] challenge window closed (it was never shown)") == 2,
          [l for l in lines if "[webview]" in l][:12])
    silent = re.findall(r"\[input_script\] vis t=\d+ @silent (.*)", text)
    check(f"{label}: silent first: the security-challenge modal stayed hidden at every probe (no modal, no popup)",
          len(silent) == len(probe_t) and not any(re.search(r"knCaptchaHolder=1|knCaptchaSolve=1|knCaptchaManual=1", s) for s in silent),
          [s for s in silent if "=1" in s][:4] or len(silent))
    probe_lines = [i for i, l in enumerate(lines) if "@silent " in l and "[input_script] vis" in l]
    pow_ok = [i for i, l in enumerate(lines) if "[krunker] captcha: the game server's proof of work passed (nothing shown)" in l]
    check(f"{label}: the game socket's \"cpt\" was answered inside the probed window, with no card",
          pow_ok and probe_lines and pow_ok[0] < probe_lines[-1] and "security challenge answered" in text and
          "proof of work failed" not in text, [l for l in lines if "proof of work" in l][:4])
    check(f"{label}: the modal was never asked to come up (no 'window is up' prompt)",
          "[krunker] captcha: the challenge window is up" not in text)
    vis = [q.get("visibility") for q in page.probes if q.get("render")]
    check(f"{label}: hidden, the page still runs as a visible one (Turnstile is not throttled)", vis and all(v == "visible" for v in vis), vis)
    client_text = "\n".join(l for l in lines if "[input_script] type" not in l)
    check(f"{label}: WebKitGTK came up on the run's own X display", "[webview] WebKitGTK " in text,
          next((l for l in lines if "[webview]" in l), log.path))
    check(f"{label}: the window loaded the challenge page twice (once per attempt), as WebKit",
          len(page.loads) == 2 and all("AppleWebKit" in ua for ua in page.loads), page.loads)
    renders = [q for q in page.probes if q.get("render")]
    check(f"{label}: the widget was rendered with krunker.io's sitekey and the widget's own callback",
          len(renders) == 2 and all(q.get("sitekey") == header_value("KRUNKER_TURNSTILE_SITE_KEY") and q.get("callback") == "function"
                                    for q in renders), renders)
    pages = [q for q in page.probes if q.get("page")]
    check(f"{label}: the page's own scripts cannot reach the client's message handler",
          pages and all(q.get("main_handler") == "0" for q in pages), pages)
    forges = [q for q in page.probes if q.get("forge")]
    refused = "/forge" not in page.paths and re.search(r"\[webview\] a navigation to http://localhost:%d was refused" % page_port, text)
    check(f"{label}: a frame of another origin is refused (or, loaded, reaches neither the handler nor the bridge)",
          refused or (forges and not any(q.get("handler") == "1" or q.get("top") == "reached" for q in forges)),
          "refused by the window" if refused else forges)
    check(f"{label}: the forged token never reached the matchmaker", not any(FORGED_TOKEN in r for _, _, r in stub.http_log))
    check(f"{label}: a token per attempt, solved in the window, then the window closed",
          text.count("[krunker] captcha: solved in the challenge window") == 2 and text.count("[webview] challenge window closed") >= 2,
          [l for l in lines if "[webview]" in l or "captcha:" in l][:16])
    check(f"{label}: the refused token started a second silent run (attempt 2), not a popup", "attempt 2, the last token was refused" in text)
    check(f"{label}: the challenge page never saw a token", not any("e2eWebviewToken" in pth or "e2eForged" in pth for pth in page.paths))
    check_matchmaker(label, stub, [webview_token(1), webview_token(2)], client_text)
    check(f"{label}: the client joined as the stub's sid", f"joined as sid {MY_SID}" in text)
    check(f"{label}: nothing of the run is left (client_bin, WebKit's processes, Xvfb)", not left)


def run_native_webview_interactive():
    """Turnstile needs the player (before-interactive-callback): only then is the window created,
    mapped once, already centred on the work area and above the game, never moved after; the
    player's token joins."""
    label = "native-webview-interactive"
    (page_port,) = kn_proto.free_ports(1)
    page = ChallengePage(page_port, interactive=True)
    page.start()
    # the stub's game server sends a "cpt" this client cannot answer: only that failure shows the
    # modal's passive card (a solved one shows nothing: native-webview)
    probes = "".join("%d vis @pow knCaptchaHolder knCaptchaSolve knCaptchaCancel\n%d text @powst [id=knCaptchaStatus]\n" % (t, t + 50)
                     for t in range(1000, 9801, 400))
    script = "0 boot\n" + probes + "10000 play\n15000 players\n15200 quit\n"

    def unsolvable(stub):
        stub.cpt = ALTCHA_UNSOLVABLE
    stub, lines, log, left, maps, moves = webview_session(label, script, page, False, stub_setup=unsolvable)
    text = "\n".join(lines)
    wins = [m for m in maps if "krunker.io" in m["name"]]
    check(f"{label}: one window, the challenge's, was mapped", len(maps) == 1 and len(wins) == 1,
          [(m["name"], m["x"], m["y"], m["w"], m["h"]) for m in maps] or [l for l in lines if "[webview]" in l][:12])
    want = ((XVFB_W - 520) // 2, (XVFB_H - 600) // 2, 520, 600)
    if wins:
        w = wins[0]
        check(f"{label}: its first mapped position is centred on the work area {want}", (w["x"], w["y"], w["w"], w["h"]) == want,
              (w["x"], w["y"], w["w"], w["h"]))
        check(f"{label}: never moved after it was mapped", not moves[w["window"]], moves[w["window"]])
    check(f"{label}: shown because Turnstile needs the player (it ran hidden until then)",
          "[webview] challenge running hidden" in text and
          "[webview] challenge window shown (Turnstile needs the player) at %d,%d %dx%d" % want in text,
          [l for l in lines if "[webview]" in l][:12])
    up = [i for i, l in enumerate(lines) if "[krunker] captcha: the challenge window is up: the player solves it" in l]
    solved = [i for i, l in enumerate(lines) if "[krunker] captcha: solved in the challenge window" in l]
    check(f"{label}: the modal came up with the window, once, before the token (the player's turn)",
          len(up) == 1 and solved and up[0] < solved[0], [l for l in lines if "captcha:" in l][:8])
    check(f"{label}: the token joins (one attempt), the window closes",
          text.count("[krunker] captcha: solved in the challenge window") == 1 and "[webview] challenge window closed\n" in text + "\n",
          [l for l in lines if "[webview]" in l or "captcha:" in l][:12])
    check(f"{label}: the client joined as the stub's sid", f"joined as sid {MY_SID}" in text)
    fail_i = [i for i, l in enumerate(lines) if "[krunker] captcha: the game server's proof of work failed: the card says so" in l]
    check(f"{label}: a \"cpt\" this client cannot answer is said, after the token (not before it)",
          len(fail_i) == 1 and solved and fail_i[0] > solved[0] and "proof of work passed" not in text, [l for l in lines if "proof of work" in l][:4])
    # each @pow vis line is followed by its @powst text line: the card is the holder shown with the
    # failure line and no Solve / Cancel
    pairs, last_vis = [], None
    for l in lines:
        m = re.search(r"\[input_script\] vis t=\d+ @pow (.*)", l)
        if m:
            last_vis = m.group(1)
        m = re.search(r"\[input_script\] text t=\d+ @powst n=\d+ checked=\d+ value=\S* text=(.*)", l)
        if m and last_vis is not None:
            pairs.append((last_vis, m.group(1)))
            last_vis = None
    card = [v for v, st in pairs if "knCaptchaHolder=1" in v and "could not answer it" in st]
    check(f"{label}: the failed \"cpt\" shows the passive card (no Solve, no Cancel) while it is said",
          card and all("knCaptchaSolve=0" in v and "knCaptchaCancel=0" in v for v in card), pairs[-6:])
    check(f"{label}: nothing of the run is left (client_bin, WebKit's processes)", not left)


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
            # meanwhile the passive "cpt" card is watched for: solved by the client, it must never show
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
            timer_samples = []

            def sample_timer():
                v = cdp.eval("(() => { const e = document.getElementById('timerVal'); const m = e && /^(\\d+):(\\d+)/.exec(e.textContent); return m ? (+m[1]) * 60 + (+m[2]) : null; })()")
                if isinstance(v, int):
                    timer_samples.append(v)
            if stub.got_en.is_set():
                # Settings > Chat "Show Player Messages" is off by default: this check reads the player lines
                cdp.eval("window.setSetting && window.setSetting('showMessages', true)")
                time.sleep(1.0)
                sample_timer()
                cdp.key("rawKeyDown", "w", "KeyW", 87)
                time.sleep(1.5)
                cdp.key("keyUp", "w", "KeyW", 87)
                sample_timer()
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
                sample_timer()
                check("web: the HUD match timer counts down (#timerVal)",
                      len(timer_samples) >= 2 and timer_samples[0] > 0 and timer_samples[-1] < timer_samples[0], timer_samples)
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
    check("web: the \"cpt\" was answered with no card on screen (the player sees only the loading state)",
          not statuses and "security challenge answered" in text and "proof of work failed" not in text, statuses)
    check_run("web", stub, lines, True)
    k = [l for l in lines if "[krunker] <= k " in l]
    check("web: the stub player's \"k\" updates arrived", len(k) >= 5, len(k))


def run_web_voice():
    """official-ingame-webvoice: the web build in an official lobby (?krunker=1, the stub's "voice" scenario). Chromium's
    fake microphone (--use-fake-device-for-media-stream: a beep) is what the page's getUserMedia records; V held: the
    clips go out as v10's "vc" [dataURL, channel, micVolume, micQuality] and decode in this same Chromium; the stub's
    clips back ("vc" on channels -1, 0, 1) play on -1 and 0 (?voicelog=1: the [voice] lines in the console)."""
    label = "web-voice"
    mm, wsp, web, cdpp = kn_proto.free_ports(4)
    stub = Stub(mm, wsp, "voice")
    stub.start_http()
    stub.start_ws()
    httpd = ThreadingHTTPServer(("127.0.0.1", web), StaticHandler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    chrome_log = open(os.path.join(WORK, f"{label}-chrome.log"), "w")
    chrome = subprocess.Popen([CHROME, "--headless=new", "--no-sandbox", "--use-gl=angle", "--use-angle=swiftshader", "--enable-unsafe-swiftshader",
                               "--use-fake-device-for-media-stream", "--use-fake-ui-for-media-stream", "--autoplay-policy=no-user-gesture-required",
                               "--user-data-dir=" + os.path.join(WORK, label + "-chrome"), "--remote-debugging-port=%d" % cdpp, "--window-size=1280,720",
                               "about:blank"], stdout=chrome_log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, start_new_session=True)
    lines = []
    try:
        cdp = Cdp(cdpp)
        cdp.send("Runtime.enable")
        cdp.send("Page.enable")
        cdp.intercept = (TURNSTILE_URL, turnstile_stub("ok"))
        cdp.send("Fetch.enable", {"patterns": [{"urlPattern": TURNSTILE_URL + "*"}]})
        url = "http://127.0.0.1:%d/?krunker=1&krunkerMM=%s&krunkerTrace=1&voicelog=1" % (web, urllib.parse.quote("http://127.0.0.1:%d" % mm, safe=""))
        cdp.send("Page.navigate", {"url": url})
        stub.captcha_ok.wait(120)
        end = time.time() + 150
        while time.time() < end and not stub.got_en.is_set():  # CLICK TO PLAY once the map is in, until the client's "en"
            time.sleep(0.3)
            box = cdp.eval("(() => { const e = document.getElementById('instructions'); if (!e || !/CLICK TO PLAY/.test(e.textContent)) return null;"
                           " const b = e.getBoundingClientRect(); return [b.x + b.width / 2, b.y + b.height / 2]; })()")
            if box:
                cdp.click(box[0], box[1])
        check(f"{label}: the page entered the game (\"en\" sent)", stub.got_en.is_set(), "\n".join(cdp.console[-25:]))
        if stub.got_en.is_set():
            time.sleep(2.0)
            cdp.key("rawKeyDown", "v", "KeyV", 86)  # voiceKey (V): hold to talk
            time.sleep(2.6)
            cdp.key("keyUp", "v", "KeyV", 86)
            end = time.time() + 40
            while time.time() < end and "done" not in stub.sent and not stub.closed.is_set():
                time.sleep(0.3)
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
    for name in ("vc_out", "vc_in", "done"):
        check(f"{label}: the stub got to its \"{name}\" step", name in stub.sent, sorted(stub.sent))
    vcs = [f[1] for f in stub.frames if f[0] == "vc"]
    ok = all(len(a) == 4 and isinstance(a[0], str) and a[0].startswith("data:audio/ogg; codecs=opus;base64,") and a[1] == 0 and
             isinstance(a[2], (int, float)) and 0 < a[2] <= 1 and isinstance(a[3], int) for a in vcs)
    check(f"{label}: the browser-recorded clips go out as v10's \"vc\" [dataURL, channel 0, micVolume, micQuality]", vcs and ok,
          [(a[0][:40], a[1:]) for a in vcs[:2]])
    recv = re.findall(r"\[voice\] t=[\d.]+ recv v7 StubBot sid=(\d+) ch=(-?\d+)", text)
    check(f"{label}: the bot's clips on channels -1 and 0 reach the player, the team-tab one does not",
          sorted({int(c) for _, c in recv}) == [-1, 0], recv)
    check(f"{label}: a received clip is decoded and played on the web", re.search(r"\[voice\] t=[\d.]+ play StubBot ", text) is not None,
          [l for l in lines if "[voice]" in l][-6:])
    if vcs:
        check_clip_in_chromium(label, vcs[0][0], "the web build's recorded clip", min_peak=0.0)


def main():
    if websockets is None:
        print("krunker_official_e2e: needs the Python websockets package: skipped")
        return 77
    # the matchmaker version contract needs no client or web build: always guard it
    check_version_gate()
    ran = True
    if ONLY in ("", "native") and os.path.isfile(f"{BUILD}/client_bin"):
        run_native()
        run_native_paste()
        run_native_sync()
        run_native_hq()
        run_native_hud()
        run_native_ingame()
        run_native_rounds()
        run_native_world()
        run_native_actions()
        run_native_social()
        run_native_voice()
        run_native_popups()
        run_native_lobby()
        run_native_badmap()
        run_native_kick()
        run_native_fx()
        run_native_optout()
        run_native_exclude()
        run_native_exclude_mm()
        run_native_reload(False)
        run_native_reload(True)
        run_native_live()
        run_native_live_refused()
        run_native_live_stale()
        run_native_live_fallback()
        ran = True
    if ONLY == "hud" and os.path.isfile(f"{BUILD}/client_bin"):  # just the score / objective / end HUD run
        run_native_hud()
    if ONLY == "rounds" and os.path.isfile(f"{BUILD}/client_bin"):  # just official-ingame-rounds' run
        run_native_rounds()
    if ONLY == "ingame" and os.path.isfile(f"{BUILD}/client_bin"):  # just official-ingame's run
        run_native_ingame()
    if ONLY == "world" and os.path.isfile(f"{BUILD}/client_bin"):  # just the mode-objective / world packets
        run_native_world()
    if ONLY == "actions" and os.path.isfile(f"{BUILD}/client_bin"):  # just the player actions' run
        run_native_actions()
    if ONLY == "live" and os.path.isfile(f"{BUILD}/client_bin"):  # just the live Krunker build's runs
        run_native_live()
        run_native_live_refused()
        run_native_live_stale()
        run_native_live_fallback()
    if ONLY == "social" and os.path.isfile(f"{BUILD}/client_bin"):  # just the social / account lines' run
        run_native_social()
    if ONLY == "voice" and os.path.isfile(f"{BUILD}/client_bin"):  # just the voice chat run
        run_native_voice()
    if ONLY == "popups" and os.path.isfile(f"{BUILD}/client_bin"):  # just the text popups / countdown run
        run_native_popups()
    if ONLY == "exclude" and os.path.isfile(f"{BUILD}/client_bin"):  # just Find New Game's runs
        run_native_exclude()
        run_native_exclude_mm()
    if ONLY == "fx" and os.path.isfile(f"{BUILD}/client_bin"):  # just the combat visuals / sounds run
        run_native_fx()
    if ONLY == "reload" and os.path.isfile(f"{BUILD}/client_bin"):  # just the reload runs
        run_native_reload(False)
        run_native_reload(True)
    if ONLY in ("", "native", "state") and os.path.isfile(f"{BUILD}/client_bin"):  # joining mid end screen, leaving
        run_native_join_end()
        run_native_leave_state()
        ran = True
    webview_skipped = None
    if ONLY in ("", "native", "webview") and os.path.isfile(f"{BUILD}/client_bin"):
        webview_skipped = webview_skip_reason()
        if webview_skipped:
            print(f"krunker_official_e2e: the native challenge-window run is SKIPPED: {webview_skipped}")
        else:
            run_native_webview()
            run_native_webview_interactive()
            ran = True
    if ONLY in ("", "web") and DIST and CHROME and os.path.isfile(os.path.join(DIST, "client.wasm")):
        run_web("ok")
        run_web("110200")
        run_web_voice()
        ran = True
    if ONLY == "webvoice" and DIST and CHROME and os.path.isfile(os.path.join(DIST, "client.wasm")):  # just the web voice run
        run_web_voice()
        ran = True
    elif ONLY == "web":
        print("krunker_official_e2e: the web run needs Chromium and a web build (WEB_DIST_DIR): skipped")
    if not ran or (ONLY == "webview" and webview_skipped):
        shutil.rmtree(WORK, ignore_errors=True)
        return 77
    if all(results):
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print(f"logs kept in {WORK}")
    print(f"{sum(results)}/{len(results)} passed")
    return 0 if results and all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
