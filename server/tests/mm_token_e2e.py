#!/usr/bin/env python3
"""The HTTP matchmaker token flow, seek preferences, official krunker.io lobbies and the F4 / F5
keys, end to end (doc/v10/matchmaker-token.md).

  python3 -I server/tests/mm_token_e2e.py BUILD_DIR      (run from the repository root)

kn_brain on a scratch database (KN_TEST_DB_DRIVER, default sqlite: no server needed), two
brain-linked server_bin nodes (MTA: ffa on Burg, MTB: tdm on Sandstorm), a stub of Krunker's
matchmaker (GET /game-list, what the native client lists as official lobbies), every port from
kn_proto.free_ports().

  protocol (no client): GET /mm/generate-token is plain text; seek-game with it -> a join ticket the
    game server admits; the token is single use (InvalidClientValidationToken), garbage is refused,
    an expired one is ExpiredClientValidationToken; no token is still served (rollout: optional);
    a second brain with brain.matchmaking.require_validation_token refuses a seek without one and
    serves one with one; dataQuery dsmo / dsma pick the matching node, NoMatchingServers when none;
    server_bin refuses a forged and an expired ticket, and a reused ticket replaces the first
    connection (one seat per ticket)
  client 1: a headless client_bin gets a token over HTTP, joins with the ticket; F4 leaves the game for
    the main menu at once (no session, no error screen), and Play starts a game again (a fresh seek)
  client 2: F5 relaunches the client (the same pid: exec), which comes back into the same lobby
  client 3: Settings > General (advanced view) > Quickplay: the rows render; seek mode tdm set through setSetting persists
    (knSettings) and the next run's startup seek lands on the tdm node
  client 4: server type Official: Find New Game opens the matching official lobby in the browser (logged with
    KRUNKNATIVE_NO_BROWSER), the Servers window shows the krunker.io section after ours (the server
    type only steers seeks), a row opens its lobby; the stub was asked once (cache)
  (the proxy for krunker.io, its block page and the default server type: client_proxy_e2e.py)
  client 5: server type Both, a mode only krunker.io plays: Find New Game falls back to the official lobby
  client 6: Quick Join on the krunker.io group joins its first lobby in-client (not a seek)
  client 7: krunker.io's official customs / custom games are their own krunker.io groups, never Box Office's
    (the tabs: krunk.io holds Box Office's groups, krunker.io's own tab holds krunker.io's Custom Games first)
  client 8: Quickplay's seek mode and map narrow the seek, not the server list: the krunker.io tab lists the same
    rows in the same groups with a restricted seek as with none picked, and no 'Showing ...' line

Needs a headless GL device (KRUNKNATIVE_HEADLESS=1: EGL). Exit 77 when the database is unavailable.
"""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
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

sys.path.insert(0, os.path.join(os.getcwd(), "server/tests"))
import kn_proto  # noqa: E402

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
ROOT = os.getcwd()
DRIVER = os.environ.get("KN_TEST_DB_DRIVER", "sqlite")
NAME = "kn_test_mmtok_%d" % os.getpid()
SECRET = "kn-mmtoken-e2e-secret-%d" % os.getpid()
PORT_A, PORT_B, STUB_PORT = kn_proto.free_ports(3)
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-mm-token-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
results = []
procs = []

# Krunker's /game-list shape (v10.js:211417-211436): [id, region, players, max, data, timeLeft]
STUB_GAMES = {"success": True, "totalPlayerCount": 21, "games": [
    ["FRA:aaaaa", "de-fra", 5, 8, {"i": "Burg", "g": 0, "c": 0, "v": "x"}, 120],
    ["NY:bbbbb", "us-nj", 3, 8, {"i": "Sandstorm", "g": 1, "c": 0, "v": "x"}, 200],
    ["NY:ccccc", "us-nj", 8, 8, {"i": "Sandstorm", "g": 1, "c": 0, "v": "x"}, 10],
    ["SV:cust1", "us-ca-sv", 2, 8, {"i": "Burg", "g": 0, "c": 1, "h": "Somebody's game"}, 0],
    ["TOK:ddddd", "jb-hnd", 3, 8, {"i": "Littletown", "g": 3, "c": 0, "v": "x"}, 50],
]}
stub_hits = []


class Stub(BaseHTTPRequestHandler):
    def do_GET(self):  # noqa: N802
        stub_hits.append(self.path)
        if self.path.startswith("/game-list"):
            body = json.dumps(STUB_GAMES).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        else:
            self.send_error(404)

    def log_message(self, *args):
        pass


def check(name, ok, detail=""):
    results.append(bool(ok))
    show = detail and (not ok or len(detail) <= 160)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if show else ''}", flush=True)


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


def post(port, path, body):
    req = urllib.request.Request("http://127.0.0.1:%d%s" % (port, path), data=json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json"}, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status, json.loads(r.read() or b"null")
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b"null")


def account_cfg(port, name):
    """a client config dir signed in as a new account (the saved session the client restores at launch)"""
    user = "f4acct%d" % (os.getpid() % 100000)
    st, reg = post(port, "/auth/register", {"username": user, "password": "pw-f4-%d" % os.getpid()})
    token = reg.get("token", "") if isinstance(reg, dict) else ""
    if st != 200 or not token:
        return None
    cfg = os.path.join(WORK, name + "-cfg")
    os.makedirs(os.path.join(cfg, "krunknative"), exist_ok=True)
    for key, value in (("krunker_username", user), ("krunker_token", token)):
        with open(os.path.join(cfg, "krunknative", key + ".json"), "w") as f:
            f.write(value)
    return cfg


def http(port, path, raw=False):
    try:
        with urllib.request.urlopen("http://127.0.0.1:%d%s" % (port, path), timeout=10) as r:
            data = r.read()
            return r.status, (data.decode() if raw else json.loads(data))
    except urllib.error.HTTPError as e:
        data = e.read() or b"null"
        try:
            return e.code, json.loads(data)
        except ValueError:
            return e.code, data.decode(errors="replace")


def token(port):
    code, text = http(port, "/mm/generate-token", raw=True)
    return text.strip() if code == 200 else None


def seek(port, tok=None, extra=""):
    q = "/mm/seek-game?hostname=krunknative&region=local&autoChangeGame=true" + extra
    if tok is not None:
        q += "&validationToken=" + urllib.parse.quote(tok, safe="")
    return http(port, q)


def lobbies(port):
    code, body = http(port, "/mm/game-list")
    return body.get("games", []) if code == 200 else []


def start_brain(env, log, extra=()):
    p = subprocess.Popen([f"{BUILD}/services/brain/kn_brain", "--brain.host=127.0.0.1", "--brain.port=0", "--brain.region=local",
                          "--brain.join_ttl_s=5", "--brain.matchmaking.validation_ttl_s=5",
                          "--brain.id=mmtok-%d-%d" % (os.getpid(), len(procs))] + list(extra),
                         cwd=ROOT, env=env, stdout=open(log, "w"), stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                         start_new_session=True)
    procs.append(p)
    m = wait_for(lambda: re.search(r"listening on http://127\.0\.0\.1:(\d+) \(ws", read_log(log)), 15)
    return int(m.group(1)) if m else None


def start_node(brain_port, node_id, port, mode, map_name):
    log = os.path.join(WORK, node_id + ".log")
    cfg = os.path.join(WORK, node_id + ".json")
    with open(cfg, "w") as f:
        json.dump({"gameserver": {"node_id": node_id, "name": "mm token e2e " + node_id, "region": "local", "listen_port": port,
                                  "public_host": "127.0.0.1", "brain_url": "ws://127.0.0.1:%d/mm/internal/ws" % brain_port,
                                  "brain_secret": SECRET, "modes": [mode], "maps": [map_name], "max_players": 6,
                                  "idle_kick_seconds": 0, "idle_menu_kick_seconds": 0}}, f)
    p = subprocess.Popen([f"{BUILD}/server_bin", "--headless", "--config", cfg, "--cfg", "none"], cwd=ROOT,
                         stdout=open(log, "w"), stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, start_new_session=True)
    procs.append(p)
    linked = wait_for(lambda: "welcomed" in read_log(log), 20)
    game = wait_for(lambda: next((g for g in lobbies(brain_port) if g[0].startswith(node_id + ":")), None), 15)
    return log, game


def connect(rep):
    """JOIN with a seek reply's ticket; (conn, spawned, error)"""
    c = kn_proto.Conn("127.0.0.1", int(rep["port"]))
    c.hello()
    c.join(client_key=rep.get("clientId", ""), game_id=rep.get("gameId", ""))
    c.ent()
    end, seen = time.time() + 8, 0
    while time.time() < end:
        c.read(timeout=0.3)
        for t, data in c.packets[seen:]:
            if t == kn_proto.ERROR:
                return c, False, kn_proto.error_info(data)
            sp = kn_proto.spawn_info(data) if t == kn_proto.SPAWN else None
            if sp and sp["is_you"]:
                return c, True, None
        seen = len(c.packets)
    return c, False, None


def protocol(port, env, node_a, node_b):
    a_log, game_a = node_a
    _, game_b = node_b
    mode_b = game_b[4].get("mode")
    map_b = game_b[4].get("mapIndex")
    check("lobbies announce mode and mapIndex", isinstance(mode_b, int) and isinstance(map_b, int), str(game_b[4]))
    t = token(port)
    check("P1: GET /mm/generate-token answers a token (plain text)", t and t.count(".") == 2 and len(t) < 400, str(t)[:80])
    code, rep = seek(port, t)
    check("P2: seek-game with the token -> a join ticket", code == 200 and rep.get("clientId"), "%s %s" % (code, rep))
    c, ok, err = connect(rep) if code == 200 else (None, False, None)
    check("P2: the game server admits the ticket", ok, str(err))
    code2, rep2 = seek(port, t)
    check("P3: the token is single use", code2 == 403 and rep2.get("error") == "InvalidClientValidationToken", "%s %s" % (code2, rep2))
    code3, rep3 = seek(port, "not.a.token")
    check("P4: a forged token is refused", code3 == 403 and rep3.get("error") == "InvalidClientValidationToken", "%s %s" % (code3, rep3))
    stale = token(port)
    time.sleep(6.2)  # validation_ttl_s 5
    code4, rep4 = seek(port, stale)
    check("P5: an expired token is refused", code4 == 403 and rep4.get("error") == "ExpiredClientValidationToken", "%s %s" % (code4, rep4))
    code5, rep5 = seek(port)
    check("P6: no token: still served while optional (old clients mid-deploy)", code5 == 200 and rep5.get("clientId"), str(code5))
    dq = urllib.parse.quote(json.dumps({"dsmo": [mode_b]}))
    code6, rep6 = seek(port, token(port), "&dataQuery=" + dq)
    check("P7: dataQuery dsmo picks the node playing that mode", code6 == 200 and rep6.get("gameId") == game_b[0], "%s %s" % (code6, rep6))
    dq = urllib.parse.quote(json.dumps({"dsma": [map_b]}))
    code6, rep6 = seek(port, token(port), "&dataQuery=" + dq)
    check("P7: dataQuery dsma picks the node playing that map", code6 == 200 and rep6.get("gameId") == game_b[0], "%s %s" % (code6, rep6))
    dq = urllib.parse.quote(json.dumps({"dsmo": [mode_b], "dsma": [map_b + 100]}))
    code7, rep7 = seek(port, token(port), "&dataQuery=" + dq)
    check("P7: nothing plays it: NoMatchingServers", code7 == 503 and rep7.get("error") == "NoMatchingServers", "%s %s" % (code7, rep7))

    # the game server's side of the ticket
    forged = dict(rep5, clientId=rep5["clientId"][:-4] + "AAAA")
    c2, ok2, err2 = connect(forged)
    check("P8: server_bin refuses a forged ticket", not ok2 and err2 and "Invalid join key" in err2[0], str(err2))
    c2.close()
    code8, rep8 = seek(port, token(port), "&game=" + urllib.parse.quote(game_a[0]))
    time.sleep(6.2)  # join_ttl_s 5
    c3, ok3, err3 = connect(rep8) if code8 == 200 else (None, False, None)
    check("P8: server_bin refuses an expired ticket", not ok3 and err3 and "expired" in err3[0], str(err3))
    if c3:
        c3.close()
    code9, rep9 = seek(port, token(port), "&game=" + urllib.parse.quote(game_a[0]))
    first, ok_first, _ = connect(rep9)
    second, ok_second, _ = connect(rep9)
    kicked = first.read(until=kn_proto.ERROR, timeout=5)
    info = kn_proto.error_info(kicked) if kicked is not None else None
    check("P8: a reused ticket replaces the first connection (one seat per ticket)",
          ok_first and ok_second and info and info[1] == "004002", str(info))
    for conn in (c, first, second):
        if conn:
            conn.close()

    # a brain that requires the token (rollout step 2), over the same database (federation)
    port2 = start_brain(env, os.path.join(WORK, "brain2.log"), ["--brain.matchmaking.require_validation_token=true"])
    check("P9: a second brain (tokens required) starts", port2, read_log(os.path.join(WORK, "brain2.log"))[-300:])
    if port2:
        wait_for(lambda: len(lobbies(port2)) >= 2, 10)
        code10, rep10 = seek(port2)
        check("P9: required: a seek without a token is refused", code10 == 403 and rep10.get("error") == "InvalidClientValidationToken",
              "%s %s" % (code10, rep10))
        code11, rep11 = seek(port2, token(port))  # issued by the other brain: same network key
        check("P9: required: a token of another brain of the network is accepted", code11 == 200, "%s %s" % (code11, rep11))
        if code11 == 200:
            c4, ok4, err4 = connect(rep11)
            check("P9: and its ticket admitted", ok4, str(err4))
            c4.close()


def client(name, brain_port, script, relaunch_script=None, cfg=None, extra_env=None):
    path = os.path.join(WORK, name + ".script")
    with open(path, "w") as f:
        f.write(script)
    log = os.path.join(WORK, name + ".log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null",
                "XDG_CONFIG_HOME": cfg or os.path.join(WORK, name + "-cfg"),
                "XDG_STATE_HOME": os.path.join(WORK, name + "-state"), "KRUNKNATIVE_NO_BROWSER": "1",
                "KRUNKNATIVE_INPUT_SCRIPT": path, "KRUNKNATIVE_WINDOW_SIZE": "640x360",
                "KRUNKNATIVE_BRAIN_URL": "http://127.0.0.1:%d" % brain_port, "KRUNKNATIVE_NET_TRANSPORT": "tcp",
                "KRUNKNATIVE_KRUNKER_MATCHMAKER": "http://127.0.0.1:%d" % STUB_PORT,
                "KRUNKNATIVE_KRUNKER_URL": "https://krunker.io"})
    if relaunch_script:
        rpath = os.path.join(WORK, name + "-relaunch.script")
        with open(rpath, "w") as f:
            f.write(relaunch_script)
        env["KRUNKNATIVE_INPUT_SCRIPT_RELAUNCH"] = rpath
    env.update(extra_env or {})
    p = subprocess.Popen(["stdbuf", "-oL", "-eL", f"{BUILD}/client_bin"], cwd=ROOT, stdout=open(log, "w"),
                         stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, start_new_session=True)
    procs.append(p)
    return p, log


def finish(p, secs):
    try:
        p.wait(secs)
        return True
    except subprocess.TimeoutExpired:
        os.killpg(p.pid, signal.SIGTERM)
        try:
            p.wait(5)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
        return False


def joined(text):
    return re.findall(r"\[mm\] joining (\S+) at ", text)


def states(text):
    return re.findall(r"\[input_script\] state t=\d+ slot=-?\d+/(\d+) weapon=(.+?) menu=(\d)", text)


def in_game(st):
    return bool(st) and st[-1][2] == "0" and int(st[-1][0]) > 0 and st[-1][1] != "-"


# the probe times after F4: every 20 ms through the leave, then every 100 ms through the wait for Play,
# the seek and the join (Play is clicked at 6000, 9000 and 12000). A probe after another input event
# runs in its own frame (input_script.c); a no-op `look 0 0` between two probes keeps them apart, so
# each probe samples a frame of its own instead of all of them sharing one frame after a stall.
F4_TIMES = list(range(2060, 2501, 20)) + list(range(5000, 12001, 100))


def f4_probes():
    """vis probes after F4, one per frame: the menu, the game HUD, the chat holder and the chat box."""
    out = []
    for t in F4_TIMES:
        out.append("%d look 0 0\n" % (t - 1))
        out.append("%d vis @f4-%d inGameUI menuHolder chatUIHolder chatUI chatHolder chatList endUI\n" % (t, t))
    return "".join(out)


def f4_page(text):
    """[(script-time, {element: '0'|'1'|'-'})] of the F4 probes, in script order."""
    out = []
    for t, rest in re.findall(r"vis t=\S+ @f4-(\d+) ([^\n]*)", text):
        out.append((int(t), dict(kv.split("=", 1) for kv in rest.split() if "=" in kv)))
    return out


def client_f4(port, game_a, game_b, cfg=None):
    # both nodes' modes picked (Settings > Quickplay): with none picked a seek takes only the public rotation
    # (seek_prefs.h), which the tdm node is not in, so Play's seek could not find the other lobby
    cfg = cfg or os.path.join(WORK, "f4-cfg")
    os.makedirs(os.path.join(cfg, "krunknative"), exist_ok=True)
    with open(os.path.join(cfg, "krunknative", "knSettings.json"), "w") as f:
        json.dump({"seek_modes": "%d,%d" % (game_a[4]["mode"], game_b[4]["mode"])}, f)
    # F4 in game leaves the game for the main menu at once: the menu is up (no game HUD, no chat, no
    # spinner) and offers Play. Nothing joins by itself; Play starts a game again (a fresh seek).
    # The join lands late on a loaded machine, so Play is pressed three times (a click on the
    # spinner does nothing; once the lobby is up, the first one that lands enters it).
    # signed in as an account (cfg): an account sees the chat on the menu (v10 hideChatIfGuestUser), so
    # the chat must stay hidden for it too until the game is in game
    p, log = client("f4", port, "0 play\n1000 state\n2000 down F4\n2050 up F4\n" + f4_probes() +
                    "2600 nodes @f4-chat [id=chatList] .chatItem\n"
                    "2600 grep @f4-play CLICK [id=instructions]\n"
                    + "".join("%d clickid instructionHolder\n" % t for t in range(6000, 20001, 1000)) +
                    "21000 call copyInviteLink\n23000 state\n24000 quit\n", cfg=cfg)
    done = finish(p, 90)
    text = read_log(log)
    js = joined(text)
    page = f4_page(text)
    check("C1: F4 leaves the game: the main menu is up at once (menuHolder=1, inGameUI=0)",
          bool(page) and page[0][1].get("menuHolder") == "1" and page[0][1].get("inGameUI") == "0", str(page[:2]))
    waiting = [(t, el) for t, el in page if t < 6000]  # F4 until Play is clicked
    bad = [t for t, el in waiting if el.get("inGameUI") != "0" or el.get("menuHolder") != "1"]
    check("C1: and stays so until Play (no game HUD, no grey page)", bool(page) and not bad,
          "not the menu at %s" % bad[:5])
    shown = [t for t, el in page if el.get("inGameUI") != "1" and any(el.get(k) == "1" for k in ("chatUIHolder", "chatUI", "chatHolder"))]
    check("C1: no chat bar from F4 until the game is in game (menu, Play, the seek and the join)",
          bool(page) and not shown, "chat shown at script times %s" % shown[:8])
    chat = re.search(r"\[input_script\] nodes t=\d+ @f4-chat elements=\d+ match=(\d+)", text)
    signed = "[account] logged in as" in text if cfg else True
    check("C1: the client ran signed in as an account" if cfg else "C1: (guest run)", signed, "")
    check("C1: the game's chat is cleared (no chat rows left)", bool(chat) and chat.group(1) == "0",
          chat.group(0) if chat else "no probe")
    cta = re.search(r"\[input_script\] grep t=\d+ @f4-play needle=CLICK n=\d+ hits=(\d+)", text)
    check("C1: the menu offers Play (CLICK TO PLAY, no spinner)", bool(cta) and cta.group(1) == "1",
          cta.group(0) if cta else "no probe")
    left = text.find("[mm] F4: left the game for the menu")
    check("C1: F4 left the game (the session ended, no error screen)", left >= 0 and "[client] left the game" in text
          and "Lost Connection" not in text and "error screen:" not in text[left:], str(left))
    between = text[left:text.find("@f4-chat")] if left >= 0 and "@f4-chat" in text else ""
    check("C1: and joined nothing by itself (only Play does)", left >= 0 and "[mm] joining" not in between,
          between[:200])
    after = [m.start() for m in re.finditer(r"\[mm\] joining (\S+) at ", text) if left >= 0 and m.start() > left]
    check("C1: Play starts a game again (a fresh seek and a join)", len(after) >= 1, str(js))
    # a KrunkNative match: the Invite link is ours (the public origin, the lobby's short ?game= id)
    inv = re.search(r"\[party\] invite link copied: (\S+)", text)
    check("C1: Invite in a KrunkNative match copies our link", inv is not None and inv.group(1).startswith("https://krunk.io/?game=")
          and "krunker.io/?game=" not in inv.group(1), inv.group(1) if inv else text[-300:])
    check("C1: the client fetched a validation token over HTTP", "[mm] validation token (" in text, text[-500:])
    check("C1: in the game after Play", in_game(states(text)), str(states(text)))
    check("C1: in the same process (no relaunch)", done and p.returncode == 0 and "[app] reload: restarting" not in text,
          "exit %s" % p.returncode)


def client_launch(port, game_b):
    """Auto Seek on Launch (Settings > Quickplay, on by default): the launch seeks once, the way Find New Game does,
    after the menu is up; a launch that names a lobby (KRUNKNATIVE_GAME, ?game=) does not seek."""
    p, log = client("launch", port, "0 play\n6000 state\n9000 quit\n")
    finish(p, 60)
    text = read_log(log)
    js = joined(text)
    check("L1: the launch seeks once (the option is on by default)", text.count("[mm] launch seek: Quickplay KrunkNative") == 1 and len(js) == 1,
          "%s %s" % (text.count("[mm] launch seek"), str(js)))
    check("L1: and plays the lobby it found", in_game(states(text)) and js and "[mm] joining %s at" % js[0] in text, str(states(text)))
    check("L1: nothing picked: the launch seek takes the public rotation's lobby (ffa), never the tdm node (v10 rotationModes)",
          js and js[0] != game_b[0], str(js))
    p, log = client("launch_link", port, "0 play\n6000 state\n9000 quit\n", extra_env={"KRUNKNATIVE_GAME": game_b[0]})
    finish(p, 60)
    text = read_log(log)
    check("L2: a launch that names a lobby does not seek (no launch seek line)", "[mm] launch seek" not in text, text[-300:])
    check("L2: it joins the lobby it names, once", joined(text) == [game_b[0]], str(joined(text)))
    # the switch saved off (knSettings.json, as Settings > Quickplay writes it): the launch is the startup seek
    cfg = os.path.join(WORK, "launch-off-cfg")
    os.makedirs(os.path.join(cfg, "krunknative"), exist_ok=True)
    with open(os.path.join(cfg, "krunknative", "knSettings.json"), "w") as f:
        json.dump({"knSeekOnLaunch": False}, f)
    p, log = client("launch_off", port, "0 play\n6000 state\n9000 quit\n", cfg=cfg)
    finish(p, 60)
    text = read_log(log)
    check("L3: Auto Seek on Launch switched off: the launch seeks no more (the startup seek as before)",
          "[mm] launch seek" not in text and len(joined(text)) == 1, "%s %s" % (text.count("[mm] launch seek"), str(joined(text))))


def client_f5(port):
    p, log = client("f5", port, "0 play\n1000 state\n2000 down F5\n2050 up F5\n60000 quit\n",
                    relaunch_script="0 play\n1000 state\n2000 quit\n")
    pid = p.pid
    done = finish(p, 90)
    text = read_log(log)
    js = joined(text)
    check("C2: F5 relaunches the client", "[reload] restart: F5: relaunch" in text and "[app] reload: restarting" in text, text[-500:])
    check("C2: the same process image replaced (pid %d, exit 0)" % pid, done and p.returncode == 0, "exit %s" % p.returncode)
    check("C2: the relaunched client comes back into the same lobby",
          "[reload] resume after the restart: game" in text and len(js) == 2 and js[0] == js[1], str(js))
    check("C2: and plays", in_game(states(text)), str(states(text)))


def client_settings(port, game_b):
    cfg = os.path.join(WORK, "prefs-cfg")
    os.makedirs(os.path.join(cfg, "krunknative"), exist_ok=True)
    with open(os.path.join(cfg, "krunknative", "krk_advanced.json"), "w") as f:
        f.write("1")  # Settings' advanced view (Quickplay is not in the basic one)
    mode_b = game_b[4]["mode"]
    p, log = client("prefs1", port, '0 play\n500 call setSetting "seek_modes" "%d"\n800 call showWindow 1\n2500 text @mode [id=knSeekMode%d]\n'
                    '2550 text @ffa [id=knSeekMode0]\n2600 text @type [id=knSeekServers]\n2700 nodes @hed [id=setHed_quickplay]\n3000 quit\n'
                    % (mode_b, mode_b), cfg=cfg)
    finish(p, 60)
    text = read_log(log)
    m = re.search(r"\[input_script\] text t=\d+ @mode n=(\d+) checked=(\d)", text)
    f = re.search(r"\[input_script\] text t=\d+ @ffa n=(\d+) checked=(\d)", text)
    check("C3: Settings > General shows the Quickplay section", re.search(r"nodes t=\d+ @hed elements=\d+ match=1", text), text[-400:])
    check("C3: its Select Modes picker shows the saved mode picked (its tile checked, Free for All not)",
          m and m.group(1) == "1" and m.group(2) == "1" and f and f.group(2) == "0", "%s / %s" % (m.group(0) if m else "", f.group(0) if f else ""))
    check("C3: the Server Type row (native)", re.search(r"text t=\d+ @type n=1 checked=\d value=kn", text), "")
    stored = read_log(os.path.join(cfg, "krunknative", "knSettings.json"))
    check("C3: seek_modes persisted in knSettings", '"seek_modes":"%d"' % mode_b in stored, stored[:200])
    p, log = client("prefs2", port, "0 play\n1000 state\n1500 quit\n", cfg=cfg)
    finish(p, 60)
    text = read_log(log)
    check("C3: the next run's seek lands on the node playing the saved mode", joined(text)[:1] == [game_b[0]], str(joined(text)))


def client_list_all(port):
    """Settings > Quickplay narrows what a seek picks, never the server list (the list is the objective view
    of every server): on krunker.io's tab, with a seek mode and map restricted (TDM on Sandstorm), the list shows
    the same krunker.io rows as with nothing picked, and no 'Showing ...' line. Each group is opened in turn."""
    reads = ""
    for g in range(10):
        t = 6000 + 300 * g
        reads += ("%d call setExpandedRegion %d\n%d nodes @kio%d_%d [id=serverHolder] .kioRow\n"
                  "%d nodes @ny%d [onclick*=bbbbb]\n") % (t, g, t + 60, 1, g, t + 120, g)
    end = "9100 nodes @shown [id=knSeekFilter]\n9200 quit\n"
    restricted = '300 call setSetting "seek_modes" "%d"\n400 call setSetting "seek_maps" "%d"\n' % (MODES["b"], SANDSTORM)
    seen, ny_at = {}, {}
    for label, setup in (("list-none", ""), ("list-restricted", restricted)):
        p, log = client(label, port, "0 play\n" + setup + "4000 call showWindow 2\n4100 call windows.1.switchTab 1\n"
                        "5800 text @head .setHed\n" + reads + end)
        finish(p, 90)
        text = read_log(log)
        rows = [int(x) for x in re.findall(r"nodes t=\d+ @kio1_\d+ elements=\d+ match=(\d+)", text)]
        shown = re.search(r"nodes t=\d+ @shown elements=\d+ match=(\d+)", text)
        seen[label] = (sum(rows), len(rows), int(shown.group(1)) if shown else -1)
        # the NY read of group g is at 6000 + 300 g + 120
        hits = [(int(t) - 6120) // 300 for t, n in re.findall(r"nodes t=(\d+) @ny\d+ elements=\d+ match=(\d+)", text) if int(n) > 0]
        ny_at[label] = hits[0] if len(hits) == 1 else -1
    none, restricted_seen = seen["list-none"], seen["list-restricted"]
    want = len(STUB_GAMES["games"])
    check("Q1: with nothing picked the krunker.io tab lists every krunker.io lobby the matchmaker sent (%d)" % want,
          none[0] == want, str(seen))
    check("Q1: with a seek mode and map picked the krunker.io tab lists the same rows (not only the TDM on Sandstorm ones)",
          restricted_seen[0] == none[0] and restricted_seen[1] == none[1], str(seen))
    check("Q1: ... and no 'Showing ...' line for the Quickplay switches", restricted_seen[2] == 0, str(seen))
    check("Q1: NY:bbbbb is in one group, the same one in both runs", ny_at["list-none"] >= 0 and ny_at["list-none"] == ny_at["list-restricted"], str(ny_at))
    return ny_at["list-none"]


def client_official(port, ny):
    """The browser opt-out (KRUNKNATIVE_KRUNKER_JOIN=browser): official lobbies open on krunker.io.
    The default, in-client, is client_both's and krunker_official_e2e.py's."""
    hits = len(stub_hits)
    p, log = client("official", port, '0 play\n300 call setSetting "knSeekServers" "official"\n'
                    '400 call setSetting "seek_modes" "%d"\n500 call setSetting "seek_maps" "%d"\n'
                    "1000 call backToOrigin\n4000 call showWindow 2\n4100 call windows.1.switchTab 1\n6000 text @status [id=kioStatus]\n"
                    "6100 text @head .setHed\n6150 nodes @kgroups .quickJoin[onclick*=\"quickJoinRegion(%d,\"]\n6200 call setExpandedRegion %d\n"
                    "6500 nodes @rows .kioRow\n"
                    "6550 nodes @wired [onclick*=krunkerJoin]\n6600 call krunkerJoin \"NY:bbbbb\"\n"
                    "6700 call quickJoinRegion %d\n7500 quit\n" % (MODES["b"], SANDSTORM, ny, ny, ny),
                    extra_env={"KRUNKNATIVE_KRUNKER_JOIN": "browser"})
    finish(p, 60)
    text = read_log(log)
    check("C4: opt-out: Find New Game with Official opens the matching official lobby in the browser",
          "[krunker] official lobby NY:bbbbb: opening https://krunker.io/?game=NY:bbbbb" in text
          and "open (disabled by KRUNKNATIVE_NO_BROWSER): https://krunker.io/?game=NY:bbbbb" in text, text[-600:])
    check("C4: ... and stays in its KrunkNative game (no new seek)", len(joined(text)) == 1, str(joined(text)))
    check("C4: the Servers window's krunker.io servers line says they open in the browser",
          re.search(r"@status n=1 .*text=.*KRUNKER\.IO OFFICIAL LOBBIES\s*krunker\.io servers open in your browser", text),
          "")
    m = re.search(r"@head n=\d+ checked=\d value=\S* text=(.*)", text)
    check("C4: the server type only steers the seek: the krunker.io tab is not narrowed (NY's group is found)", ny >= 0, str(ny))
    m = re.search(r"nodes t=\d+ @kgroups elements=\d+ match=(\d+)", text)
    check("C4: then one krunker.io region group (New York: tdm on Sandstorm)", m and m.group(1) == "1", m.group(0) if m else "")
    m = re.search(r"nodes t=\d+ @rows elements=\d+ match=(\d+)", text)
    check("C4: its rows (the open and the full lobby)", m and m.group(1) == "2", m.group(0) if m else "")
    m = re.search(r"nodes t=\d+ @wired elements=\d+ match=(\d+)", text)
    check("C4: the rows open krunkerJoin(id) (the browser), not a seek", m and m.group(1) == "2", m.group(0) if m else "")
    # Find New Game, the row and Quick Join each open the first lobby in the browser (3); none of them seeks
    check("C4: krunkerJoin and Quick Join open their lobby in the browser (Find New Game too: 3 opens)",
          text.count("open (disabled by KRUNKNATIVE_NO_BROWSER): https://krunker.io/?game=NY:bbbbb") == 3, "")
    # (the seek's region first: /ping-list, the ping race of region_ping.h; this stub lists none)
    lists = [h for h in stub_hits[hits:] if h.startswith("/game-list")]
    check("C4: the official list was fetched once (cached >= 15 s)", len(lists) == 1, str(stub_hits[hits:]))


def client_quick_join(port, ny):
    """Quick Join on the official krunker.io group, in-client (the default): its first joinable lobby
    (NY:bbbbb; the full NY:ccccc is skipped) is joined through krunker_net, not in the browser."""
    p, log = client("quickjoin", port, '0 play\n300 call setSetting "knSeekServers" "official"\n'
                    '400 call setSetting "seek_modes" "%d"\n500 call setSetting "seek_maps" "%d"\n'
                    "4000 call showWindow 2\n4100 call windows.1.switchTab 1\n6000 text @head .setHed\n6100 call setExpandedRegion %d\n"
                    "6300 nodes @rows .kioRow\n6400 call quickJoinRegion %d\n7500 quit\n" % (MODES["b"], SANDSTORM, ny, ny))
    finish(p, 60)
    text = read_log(log)
    check("C6: Quick Join on the krunker.io group joins its first lobby in-client",
          "[krunker] official lobby NY:bbbbb: joining in-client" in text, text[-600:])
    check("C6: ... not in the browser", "https://krunker.io/?game=NY:bbbbb" not in text, "")


def client_hoist(port):
    """The tabs (servers_ui.c tab_lists): krunker.io's official customs and custom games sit in krunker.io's own tab,
    in its groups (kio_group: Official Customs, Custom Games, then the regions), and never on krunk.io's tab, which
    holds Box Office's groups only. SV:cust1, a krunker.io custom game, is in 'Custom Games' on krunker.io's tab."""
    # krunk.io's tab (the default: no official game): every group opened in turn, no krunker.io row in any
    steps = "0 play\n4000 call showWindow 2\n6000 text @head0 .setHed\n"
    for i in range(4):  # Box Office's regions (the test list has 3)
        t = 6100 + 200 * i
        steps += "%d call setExpandedRegion %d\n%d nodes @krows0_%d [id=serverHolder] .kioRow\n" % (t, i, t + 60, i)
    p, log = client("hoist-krunk", port, steps + "7100 quit\n")
    finish(p, 60)
    text = read_log(log)
    m = re.search(r"@head0 n=\d+ checked=\d value=\S* text=(.*)", text)
    check("C7: krunk.io's tab: its first group is Box Office's, no krunker.io group",
          m and "krunker.io" not in m.group(1) and "Official Customs" not in m.group(1), m.group(1) if m else "")
    krows = [int(x) for x in re.findall(r"nodes t=\d+ @krows0_\d+ elements=\d+ match=(\d+)", text)]
    check("C7: krunk.io's tab: no krunker.io row in its groups", krows and sum(krows) == 0, str(krows))
    # krunker.io's tab: its groups in list order; the custom game is in one group, which holds only krunker.io rows
    steps = "0 play\n4000 call showWindow 2\n5800 call windows.1.switchTab 1\n6000 text @head .setHed\n"
    groups = 8
    for i in range(groups):
        t = 6100 + 200 * i
        steps += ("%d call setExpandedRegion %d\n%d grep @cg%d Somebody [id=serverHolder] .settNameIn\n"
                  "%d nodes @rows%d [id=serverHolder] .settNameIn\n%d nodes @krows%d [id=serverHolder] .kioRow\n"
                  % (t, i, t + 40, i, t + 60, i, t + 80, i))
    p, log = client("hoist-old", port, steps + "%d quit\n" % (6100 + 200 * groups + 100))
    finish(p, 60)
    text = read_log(log)
    m = re.search(r"@head n=\d+ checked=\d value=\S* text=(.*)", text)
    check("C7: krunker.io's tab: the first group is its Custom Games (no Box Office group before it)",
          m and "Custom Games" in m.group(1) and "Official Customs" not in m.group(1), m.group(1) if m else "")
    custom = [i for i in range(groups) if re.search(r"grep t=\d+ @cg%d needle=\S+ n=\d+ hits=1\b" % i, text)]
    check("C7: krunker.io's tab: the krunker.io custom game is in one group", len(custom) == 1, str(custom))
    if custom:
        i = custom[0]
        rows = re.search(r"nodes t=\d+ @rows%d elements=\d+ match=(\d+)" % i, text)
        krows = re.search(r"nodes t=\d+ @krows%d elements=\d+ match=(\d+)" % i, text)
        check("C7: krunker.io's tab: that group holds only krunker.io rows (no Box Office game)",
              rows and krows and rows.group(1) == krows.group(1) == "1", "%s / %s" % (rows.group(0) if rows else "", krows.group(0) if krows else ""))
    # the Legacy Browser setting no longer changes the groups: the krunker.io tab reads the same with it off
    p, log = client("hoist-modes", port, '0 play\n300 call setSetting "oldBrowser" "false"\n4000 call showWindow 2\n'
                    "6000 call windows.1.switchTab 1\n6100 text @head .setHed\n6200 call setExpandedRegion 0\n"
                    "6300 nodes @rows .kioRow\n7500 quit\n")
    finish(p, 60)
    text = read_log(log)
    m = re.search(r"@head n=\d+ checked=\d value=\S* text=(.*)", text)
    check("C7: Legacy Browser off, krunker.io's tab: the first group is still its Custom Games",
          m and "Custom Games" in m.group(1), m.group(1) if m else "")
    m = re.search(r"nodes t=\d+ @rows elements=\d+ match=(\d+)", text)
    check("C7: ... and that group holds the krunker.io custom game", m and m.group(1) == "1", m.group(0) if m else "")


def client_both(port):
    p, log = client("both", port, '0 play\n300 call setSetting "knSeekServers" "both"\n400 call setSetting "seek_modes" "%d"\n'
                    "1000 call backToOrigin\n5000 quit\n" % MODES["kio"])
    finish(p, 60)
    text = read_log(log)
    check("C5: Both: no KrunkNative server plays it -> the official lobby, joined in-client (the default)",
          "no KrunkNative server matches, trying krunker.io" in text and "[krunker] official lobby TOK:ddddd: joining in-client" in text
          and "[krunker] joining official game TOK:ddddd" in text and "https://krunker.io/?game=TOK:ddddd" not in text, text[-600:])


SANDSTORM = -1
MODES = {}


def main():
    global SANDSTORM  # noqa: PLW0603
    if not os.access(f"{BUILD}/client_bin", os.X_OK):
        print("mm_token_e2e: no client_bin in %s: skipped" % BUILD)
        return 77
    scratch = [sys.executable, "-I", os.path.join(ROOT, "services", "brain", "tests", "db_scratch.py"), ROOT]
    r = subprocess.run(scratch + ["create", DRIVER, NAME], capture_output=True)
    if r.returncode:
        print("mm_token_e2e: %s unavailable: skipped" % DRIVER)
        return r.returncode
    dbenv = {"KN_DATABASE_" + k.upper(): str(v) for k, v in json.loads(r.stdout).items()}
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=SECRET, **dbenv)
    stub = ThreadingHTTPServer(("127.0.0.1", STUB_PORT), Stub)
    threading.Thread(target=stub.serve_forever, daemon=True).start()
    try:
        subprocess.run([f"{BUILD}/services/brain/kn_brain", "--migrate-only"], cwd=ROOT, env=env, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        port = start_brain(env, os.path.join(WORK, "brain.log"))
        check("kn_brain starts", port, read_log(os.path.join(WORK, "brain.log"))[-300:])
        if not port:
            return 1
        node_a = start_node(port, "MTA", PORT_A, "ffa", "Burg")
        node_b = start_node(port, "MTB", PORT_B, "tdm", "Sandstorm")
        check("nodes up", node_a[1] and node_b[1], "%s %s" % (node_a[1], node_b[1]))
        if not (node_a[1] and node_b[1]):
            return 1
        SANDSTORM = node_b[1][4].get("mapIndex", -1)
        # the stub's official lobbies play our nodes' modes (Krunker's g = our mode ids), and one
        # mode no KrunkNative node plays
        MODES.update(a=node_a[1][4]["mode"], b=node_b[1][4]["mode"])
        MODES["kio"] = next(m for m in range(2, 20) if m not in MODES.values())
        for g in STUB_GAMES["games"]:
            g[4]["g"] = {"Burg": MODES["a"], "Sandstorm": MODES["b"]}.get(g[4]["i"], MODES["kio"])
        protocol(port, env, node_a, node_b)
        client_f4(port, node_a[1], node_b[1], account_cfg(port, "f4"))
        client_launch(port, node_b[1])
        client_f5(port)
        client_settings(port, node_b[1])
        ny = client_list_all(port)
        client_official(port, ny)
        client_quick_join(port, ny)
        client_hoist(port)
        client_both(port)
    finally:
        stub.shutdown()
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
    if all(results):
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print("logs kept in " + WORK)
    print(f"{sum(results)}/{len(results)} passed")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
