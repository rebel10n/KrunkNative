#!/usr/bin/env python3
"""The Social hub, player profiles and clans show the ACTIVE account backend's real data, end to end.

  python3 -I server/tests/krunker_social_e2e.py BUILD_DIR      (run from the repository root)

While krunker.io is the active account backend (an official krunker.io game, backend_ui.h), v10's
Social hub fetches the krunker.io account's data the way v10 does: the friends event bus is v10's own
socket (<EVENT_BUS_URI>/ws?at=<fresh access token>, krunker_eb.h) and the profile / clan / feed reads
are gapi GETs with the account's Bearer token (v10_gapi.c, krunker_account_api(KRUNKER_SVC_API)).
Writes are not ported for that account (follow, gift, report, clan apply, ...): their buttons are
hidden and nothing is sent. Box Office (kn_brain) stays exactly as it was.

A real kn_brain (scratch sqlite database) with one account, krunker_account_e2e.py's krunker.io stubs
(API / FRVR auth / matchmaker / game server) behind a front stub that adds krunker.io's gapi reads
(/api/players/*, /api/clans/*, /api/social/*) and the event bus websocket; one headless client_bin
(--offline: the sandbox is the menu's game; the brain still signs in):
  1. Box Office: the friends tab is kn_brain's (no krunker.io friends), a profile is kn_brain's
  2. an official game, signed in to krunker.io: the friends tab lists the event bus's friends (the
     socket carried the account's fresh token), a conversation loads its history, a profile and a
     clan view show the gapi stub's krunker.io data (Bearer token), without Follow / Gift / Report /
     Request to Join, and a scripted Follow sends nothing
  3. leaving the game: the friends tab and a profile are Box Office's again, no krunker.io request
Exit 0 when every check passed, 77 when it cannot run.
"""
import asyncio
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
import urllib.error
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, os.path.join(os.getcwd(), "server/tests"))
import kn_proto  # noqa: E402
import krunker_account_e2e as kae  # noqa: E402  (its stubs; its own WORK dir is removed below)

shutil.rmtree(kae.WORK, ignore_errors=True)
BUILD = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug")
ROOT = os.getcwd()
DRIVER = os.environ.get("KN_TEST_DB_DRIVER", "sqlite")
NAME = "kn_test_kiosocial_%d" % os.getpid()
SECRET = "kn-kiosocial-%d" % os.getpid()
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-krunker-social-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
DEV = "boxsoc%d" % (os.getpid() % 100000)
PW = "pw-kiosocial-%d" % os.getpid()

FRIEND_A, FRIEND_B = "KioFriendAlpha", "KioFriendBravo"
CLAN, CLAN_MEMBER, CLAN_OWNER = "KIOC", "KioClanMember", "KioClanOwner"
NOW_MS = int(time.time() * 1000)
FRIENDS = [
    {"id": 9001, "name": FRIEND_A, "online": True, "unread": 0, "lastId": 55, "lastText": "my reply on krunker.io",
     "lastTs": NOW_MS - 60000, "lastFromMe": True},
    {"id": 9002, "name": FRIEND_B, "online": False},
]
HISTORY = [{"id": 54, "from": 9001, "text": "history line from alpha", "ts": NOW_MS - 120000},
           {"id": 55, "from": kae.ACCOUNT_ID, "text": "my reply on krunker.io", "ts": NOW_MS - 60000}]
PROFILE = {"player_id": 9001, "player_name": FRIEND_A, "player_clan": CLAN, "player_region": 2, "player_datenew": "2021-10-04 13:04:26",
           "player_featured": 0, "player_hack": 0, "player_funds": 1234, "player_skinvalue": 621, "player_elo4": "13.78",
           "player_kills": 4242, "player_deaths": 1000, "player_wins": 48, "player_games_played": 89, "player_score": 150351,
           "player_timeplayed": 14293208, "player_following": 2, "player_followed": 1, "player_stats": {"s": 10782, "h": 4103},
           "online": 1, "maps": [], "mods": [], "assets": [], "match_history": [], "ranked_rank": 0, "raid_stats": []}
CLAN_DATA = {"clan_id": 31337, "clan_name": CLAN, "clan_score": 26689331, "clan_funds": 200, "clan_rank": 0, "clan_link4": "",
             "creator_name": CLAN_OWNER, "members": [{"p": CLAN_OWNER, "s": 1410582, "r": 3, "h": 0, "pr": 0, "ve": 1, "su": 1, "s7": 0},
                                                     {"p": CLAN_MEMBER, "s": 17062602, "r": 1, "h": 0, "pr": 0, "ve": 1, "su": 1, "s7": 5}]}
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


def http(port, path, body=None):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request("http://127.0.0.1:%d%s" % (port, path), data=data,
                                 headers={"Content-Type": "application/json"} if data else {}, method="POST" if data else "GET")
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            return r.status, json.loads(r.read() or b"null")
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b"null")


def nodes(text, label):
    m = re.search(r"\[input_script\] nodes t=\d+ @%s elements=\d+ match=(\d+)" % re.escape(label), text)
    return int(m.group(1)) if m else -1


def text_of(text, label):
    m = re.search(r"\[input_script\] text t=\d+ @%s n=(\d+) .*?text=(.*)" % re.escape(label), text)
    return m.group(2).strip() if m else ""


class Front:
    """krunker.io's gapi reads + the friends event bus; everything else goes on to kae.Stub."""

    def __init__(self, http_port, ws_port, stub):
        self.http_port, self.ws_port, self.stub = http_port, ws_port, stub
        self.gapi = []        # (method, path, authorization)
        self.eb_paths = []
        self.eb_times = []
        self.eb_in = []       # the client's event bus messages
        self.loop = self.httpd = None

    def gapi_answer(self, path):
        if path == "/api/players/" + FRIEND_A:
            return 200, {"data": PROFILE}
        if path.startswith("/api/players/%s/" % FRIEND_A):
            return 200, {"data": []}
        if path == "/api/clans/by-name/" + CLAN:
            return 200, {"data": CLAN_DATA}
        if path.startswith("/api/social/feed"):
            return 200, {"data": {"posts": []}}
        if path.startswith("/api/players/") or path.startswith("/api/clans/"):
            return 404, {"error": {"code": "PLAYERS_NOT_FOUND", "message": "not found"}}
        return None

    def start(self):
        front = self

        class H(BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def forward(self, method):
                n = int(self.headers.get("Content-Length") or 0)
                body = self.rfile.read(n) if n else None
                path = urllib.parse.urlsplit(self.path).path
                if path.startswith("/api/players/") or path.startswith("/api/clans/") or path.startswith("/api/social/"):
                    front.gapi.append((method, path, self.headers.get("Authorization", ""), time.time()))
                    status, obj = front.gapi_answer(path) if method == "GET" else (405, {"error": "read only stub"})
                    data = json.dumps(obj).encode()
                    self.send_response(status)
                    self.send_header("Content-Type", "application/json")
                    self.send_header("Content-Length", str(len(data)))
                    self.end_headers()
                    self.wfile.write(data)
                    return
                headers = {k: v for k, v in self.headers.items() if k.lower() not in ("host", "content-length", "connection")}
                req = urllib.request.Request("http://127.0.0.1:%d%s" % (front.stub.http_port, self.path), data=body, headers=headers, method=method)
                try:
                    with urllib.request.urlopen(req, timeout=20) as r:
                        status, data = r.status, r.read()
                except urllib.error.HTTPError as e:
                    status, data = e.code, e.read()
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            def do_GET(self):
                self.forward("GET")

            def do_POST(self):
                self.forward("POST")

        self.httpd = ThreadingHTTPServer(("127.0.0.1", self.http_port), H)
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()
        started = threading.Event()

        async def handler(ws):
            path = ws.request.path
            self.eb_paths.append(path)
            self.eb_times.append(time.time())
            q = dict(urllib.parse.parse_qsl(urllib.parse.urlsplit(path).query))
            if q.get("at") not in (self.stub.access1, self.stub.access2):
                await ws.close(4001, "EB_AUTH_INVALID")
                return
            try:
                async for msg in ws:
                    if not isinstance(msg, str):
                        continue
                    o = json.loads(msg)
                    self.eb_in.append(o)
                    t = o.get("type")
                    if t == "friends:list":
                        await ws.send(json.dumps({"type": "friends:list", "offset": o.get("offset", 0), "friends": FRIENDS, "hasMore": False}))
                    elif t == "msg:history":
                        await ws.send(json.dumps({"type": "msg:history:result", "peer": o.get("peer"), "requestBefore": o.get("before"),
                                                  "messages": HISTORY if o.get("peer") == 9001 else [], "hasMore": False}))
                    elif t == "presence:ping":
                        await ws.send(json.dumps({"type": "presence:pong"}))
            except kae.websockets.ConnectionClosed:
                pass

        def run():
            self.loop = asyncio.new_event_loop()
            asyncio.set_event_loop(self.loop)

            async def main():
                self.quit = asyncio.Event()
                async with kae.ws_server.serve(handler, "127.0.0.1", self.ws_port):
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


def hub_probe(t, label, profile):
    """the Friends tab, then a profile (Back pops it), then the hub closes"""
    return [f"{t} call openSocial 1",
            f"{t + 1500} text @{label}friends [id=v10sFriendScroll]",
            f"{t + 1510} text @{label}status [id=v10sEbStatus]",
            f'{t + 1600} call openPlayerProfile "{profile}"',
            f"{t + 3000} text @{label}profile [id=v10sProfile]",
            f"{t + 3005} text @{label}stats [id=v10sProfile] .profile-detail-stat-grid",
            f"{t + 3010} nodes @{label}follow [id=v10sProfileFollow]",
            f"{t + 3020} nodes @{label}gift [id=v10sProfileGift]",
            f"{t + 3030} nodes @{label}report [id=v10sProfileReport]",
            f'{t + 3100} call v10s "back"',
            f"{t + 3200} call clearPops"]


def script():
    s = ["0 play",
         "1500 down ESC", "1520 up ESC",
         # Box Office: the DEV account through v10's login modal
         "2100 clickid knV10LoginBtn",
         "2600 clickid knV10LoginMode",
         "2900 clickid accName", "2950 type " + DEV,
         "3100 clickid accPass", "3150 type " + PW,
         "3300 clickid knV10LoginSubmit"]
    s += hub_probe(7000, "kn", DEV)
    # an official game, signed in to krunker.io from v10's login modal
    s += ["10800 call showWindow 2",
          "13500 call closWind",
          '13600 call krunkerJoin "%s"' % kae.GAME_PICK,
          "18500 call loginOrRegister",
          "19000 clickid knV10LoginMode",
          "19200 clickid accName", "19250 type " + kae.LOGIN,
          "19400 clickid accPass", "19450 type " + kae.PASSWORD,
          "19600 clickid knV10LoginSubmit",
          "21000 clickid knCaptchaToken", "21100 type " + kae.TURNSTILE, "21200 clickid knCaptchaUse"]
    t = 23500
    for i, d in enumerate(kae.CODE):
        s += ["%d clickid knV10Code%d" % (t, i), "%d type %s" % (t + 60, d)]
        t += 150
    s += hub_probe(26500, "kio", FRIEND_A)
    s += ["30000 call openSocial 1",
          "31000 call v10s \"friend\" 9001",
          "32500 text @kioconv [id=v10sMessages]",
          "32510 nodes @kiosent [id=v10sMessages] .bubble-row.sent",
          "32520 nodes @kiorecv [id=v10sMessages] .bubble-row.received",
          "32600 call clearPops",
          '32700 call openClanView "%s"' % CLAN,
          "34200 text @kioclan [id=v10sClanView]",
          "34205 text @kiomembers [id=v10sClanView] .clan-view-members",
          "34210 nodes @kiojoin [id=v10sClanJoin]",
          '34300 call v10s "back"',
          '34400 call openPlayerProfile "%s"' % FRIEND_A,
          '35800 call v10s "follow"',  # scripted: the button is hidden; nothing may be sent
          '36500 call v10s "back"',
          "36600 call clearPops",
          "36700 nodes @markleave body"]
    # leave: Box Office again
    s += ["37000 con disconnect",
          "37200 down ESC", "37250 up ESC"]
    s += hub_probe(38500, "back", DEV)
    s += ["43000 quit"]
    return "\n".join(s) + "\n"


def run_client(brain_port, stub, front):
    path = os.path.join(WORK, "client.script")
    with open(path, "w") as f:
        f.write(script())
    cfg = os.path.join(WORK, "cfg")
    os.makedirs(os.path.join(cfg, "krunknative"), exist_ok=True)
    log = os.path.join(WORK, "client.log")
    base = "http://127.0.0.1:%d" % stub.http_port
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": cfg,
                "XDG_STATE_HOME": os.path.join(WORK, "state"), "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_UPDATES_URL": "off",
                "KRUNKNATIVE_INPUT_SCRIPT": path, "KRUNKNATIVE_WINDOW_SIZE": "1280x720",
                "KRUNKNATIVE_BRAIN_URL": "http://127.0.0.1:%d" % brain_port, "KRUNKNATIVE_FINDER_URL": "off",
                "KRUNKNATIVE_KRUNKER_MATCHMAKER": base + "/mm",
                "KRUNKNATIVE_KRUNKER_API": "http://127.0.0.1:%d/api" % front.http_port,
                "KRUNKNATIVE_KRUNKER_EVENT_BUS": "ws://127.0.0.1:%d" % front.ws_port,
                "KRUNKNATIVE_KRUNKER_AUTH": base + "/frvr/v1/auth", "KRUNKNATIVE_ASSETS_DIR": kae.ASSETS,
                "KRUNKNATIVE_ASSET_FETCH": "off"})
    p = subprocess.Popen(["stdbuf", "-oL", "-eL", f"{BUILD}/client_bin", "--offline"], cwd=ROOT, stdout=open(log, "w"),
                         stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, start_new_session=True)
    procs.append(p)
    marks = {}

    def mark():  # wall clock of the switch to krunker.io and of the script's leave
        while p.poll() is None:
            t = read_log(log)
            if "kio" not in marks and "[account] active account backend: krunker.io" in t:
                marks["kio"] = time.time()
            if "leave" not in marks and "@markleave" in t:
                marks["leave"] = time.time()
            time.sleep(0.05)

    threading.Thread(target=mark, daemon=True).start()
    try:
        p.wait(240)
    except subprocess.TimeoutExpired:
        os.killpg(p.pid, signal.SIGKILL)
        p.wait()
        check("client finished its script", False, "timed out; log " + log)
    return read_log(log), marks


def checks(text, stub, front, marks):
    t_kio, t_leave = marks.get("kio", 0), marks.get("leave", 0)
    fr = text_of(text, "knfriends")
    check("1. Box Office: the friends tab is kn_brain's (no krunker.io friends)", FRIEND_A not in fr and FRIEND_B not in fr, fr[:200])
    prof = text_of(text, "knprofile")
    check("1. Box Office: a profile is kn_brain's (the DEV account)", DEV in prof, prof[:200])
    check("1. Box Office: its actions are unchanged (Follow / Gift / Report are kn_brain's own: the DEV views itself, none)",
          nodes(text, "knfollow") == 0, nodes(text, "knfollow"))
    early = [g[1] for g in front.gapi if g[3] < t_kio - 0.5] + [p[:20] for p, t in zip(front.eb_paths, front.eb_times) if t < t_kio - 0.5]
    check("1. nothing went to krunker.io's gapi / event bus before the official game", t_kio and not early, early[:3])

    check("2. the official game makes krunker.io the active backend", "[account] active account backend: krunker.io" in text,
          re.findall(r"\[account\] active.*", text)[:3])
    check("2. the event bus socket carried the krunker.io account's fresh token (at=)",
          any(("at=" + urllib.parse.quote(stub.access2, safe="")) in p or ("at=" + stub.access2) in p for p in front.eb_paths),
          [p[:40] for p in front.eb_paths])
    check("2. the client asked for the friends list (friends:list {offset: 0})",
          any(o.get("type") == "friends:list" and o.get("offset") == 0 for o in front.eb_in), front.eb_in[:3])
    fr = text_of(text, "kiofriends")
    check("2. the friends tab lists the krunker.io friends", FRIEND_A in fr and FRIEND_B in fr, fr[:200])
    check("2. ... and the bus is connected", "Connected" in text_of(text, "kiostatus") or "connected" in text_of(text, "kiostatus").lower(),
          text_of(text, "kiostatus"))
    conv = text_of(text, "kioconv")
    check("2. a conversation loads its krunker.io history (msg:history)", "history line from alpha" in conv and "my reply on krunker.io" in conv, conv[:200])
    check("2. ... ours on the right, the friend's on the left (from != peer: sent)", nodes(text, "kiosent") == 1 and nodes(text, "kiorecv") == 1,
          (nodes(text, "kiosent"), nodes(text, "kiorecv")))
    prof = text_of(text, "kioprofile")
    stats = text_of(text, "kiostats")
    check("2. a profile shows krunker.io's data (gapi /players/<name>)", FRIEND_A in prof and CLAN in prof and "4,242" in stats, (prof[:120], stats[:160]))
    check("2. ... without Follow / Gift KR / Report (not ported for the krunker.io account)",
          nodes(text, "kiofollow") == 0 and nodes(text, "kiogift") == 0 and nodes(text, "kioreport") == 0,
          (nodes(text, "kiofollow"), nodes(text, "kiogift"), nodes(text, "kioreport")))
    clan = text_of(text, "kioclan")
    members = text_of(text, "kiomembers")
    check("2. the clan view shows krunker.io's clan (gapi /clans/by-name/<tag>)", CLAN_OWNER in clan and CLAN_MEMBER in members, (clan[:120], members[:160]))
    check("2. ... without Request to Join", nodes(text, "kiojoin") == 0, nodes(text, "kiojoin"))
    reads = [g for g in front.gapi if g[1] in ("/api/players/" + FRIEND_A, "/api/clans/by-name/" + CLAN)]
    check("2. the gapi reads carried the account's Bearer token", reads and all(g[2] == "Bearer " + stub.access2 for g in reads),
          [(g[1], g[2][:16]) for g in reads])
    check("2. only GETs went to krunker.io (the scripted Follow sent nothing)", front.gapi and all(g[0] == "GET" for g in front.gapi),
          [g[:2] for g in front.gapi if g[0] != "GET"])

    check("3. leaving switches back to Box Office", "[account] active account backend: krunk.io" in text, re.findall(r"\[account\] active.*", text)[:4])
    fr = text_of(text, "backfriends")
    check("3. the friends tab is Box Office's again", FRIEND_A not in fr and FRIEND_B not in fr, fr[:200])
    prof = text_of(text, "backprofile")
    check("3. a profile is Box Office's again (the DEV account)", DEV in prof, prof[:200])
    late = [g[1] for g in front.gapi if g[3] > t_leave + 0.5] + [p[:20] for p, t in zip(front.eb_paths, front.eb_times) if t > t_leave + 0.5]
    check("3. no krunker.io request after leaving", t_leave and not late, late[:3])


def main():
    if kae.websockets is None:
        print("SKIP: python websockets is not installed")
        return 77
    if not os.access(f"{BUILD}/client_bin", os.X_OK) or not os.access(f"{BUILD}/services/brain/kn_brain", os.X_OK):
        print("krunker_social_e2e: no client_bin / kn_brain in %s: skipped" % BUILD)
        return 77
    scratch = [sys.executable, "-I", os.path.join(ROOT, "services", "brain", "tests", "db_scratch.py"), ROOT]
    r = subprocess.run(scratch + ["create", DRIVER, NAME], capture_output=True)
    if r.returncode:
        print("krunker_social_e2e: %s unavailable: skipped" % DRIVER)
        return r.returncode
    dbcfg = json.loads(r.stdout)
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=SECRET, **{"KN_DATABASE_" + k.upper(): str(v) for k, v in dbcfg.items()})
    brain_log = os.path.join(WORK, "brain.log")
    http_port, ws_port, front_http, front_ws = kn_proto.free_ports(4)
    stub = kae.Stub(http_port, ws_port)
    front = Front(front_http, front_ws, stub)
    try:
        subprocess.run([f"{BUILD}/services/brain/kn_brain", "--migrate-only"], cwd=ROOT, env=env, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        brain = subprocess.Popen([f"{BUILD}/services/brain/kn_brain", "--brain.host=127.0.0.1", "--brain.port=0",
                                  "--accounts.signup_rewards.enabled=false", "--brain.region=local", "--brain.id=kiosocial-%d" % os.getpid(),
                                  "--accounts.email.required=false", "--smtp.host="],
                                 cwd=ROOT, env=env, stdout=open(brain_log, "w"), stderr=subprocess.STDOUT,
                                 stdin=subprocess.DEVNULL, start_new_session=True)
        procs.append(brain)
        m = wait_for(lambda: re.search(r"listening on http://127\.0\.0\.1:(\d+) \(ws", read_log(brain_log)), 15)
        check("kn_brain starts", bool(m), read_log(brain_log)[-300:])
        if not m:
            return 1
        port = int(m.group(1))
        st, reg = http(port, "/auth/register", {"username": DEV, "password": PW})
        check("the Box Office account is registered", st == 200, "%s %s" % (st, str(reg)[:160]))
        stub.start()
        front.start()
        text, marks = run_client(port, stub, front)
        checks(text, stub, front, marks)
    finally:
        front.stop()
        stub.stop()
        for p in procs:
            if p.poll() is None:
                os.killpg(p.pid, signal.SIGTERM)
        for p in procs:
            try:
                p.wait(10)
            except subprocess.TimeoutExpired:
                os.killpg(p.pid, signal.SIGKILL)
                p.wait()
        subprocess.run(scratch + ["drop", DRIVER, NAME], capture_output=True)
    print(f"{sum(results)}/{len(results)} passed")
    if all(results):
        shutil.rmtree(WORK, ignore_errors=True)
        return 0
    print("logs kept in " + WORK)
    return 1


if __name__ == "__main__":
    sys.exit(main())
