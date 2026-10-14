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
  client 1: a headless client_bin gets a token over HTTP, joins with the ticket; F4 = Find New
    Game: another lobby, same process (no relaunch)
  client 2: F5 relaunches the client (the same pid: exec), which comes back into the same lobby
  client 3: Settings > General (advanced view) > Quickplay: the rows render; seek mode tdm set through setSetting persists
    (knSettings) and the next run's startup seek lands on the tdm node
  client 4: server type Official: F4 opens the matching official lobby in the browser (logged with
    KRUNKNATIVE_NO_BROWSER), the Servers window shows the krunker.io section after ours (the server
    type only steers seeks), a row opens its lobby; the stub was asked once (cache)
  (the proxy for krunker.io, its block page and the default server type: client_proxy_e2e.py)
  client 5: server type Both, a mode only krunker.io plays: F4 falls back to the official lobby

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


def client_f4(port, game_a, game_b):
    p, log = client("f4", port, "0 play\n1000 state\n2000 down F4\n2050 up F4\n9000 respawn\n15000 state\n16000 quit\n")
    done = finish(p, 90)
    text = read_log(log)
    js = joined(text)
    check("C1: the client fetched a validation token over HTTP", "[mm] validation token (" in text, text[-500:])
    check("C1: and joined a lobby with the ticket", len(js) >= 1 and js[0] in (game_a[0], game_b[0]), str(js))
    check("C1: F4 seeks a new game", "[controls] F4: find new game" in text and "[mm] quick match: seeking a lobby other than" in text,
          text[-500:])
    check("C1: ... another lobby", len(js) == 2 and js[1] != js[0], str(js))
    check("C1: ... in the same process (no relaunch)", done and p.returncode == 0 and "[app] reload: restarting" not in text,
          "exit %s" % p.returncode)
    check("C1: in the game after F4", in_game(states(text)), str(states(text)))


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
    p, log = client("prefs1", port, '0 play\n500 call setSetting "seek_modes" "%d"\n800 call showWindow 1\n2500 text @mode [id=knSeekMode]\n'
                    '2600 text @type [id=knSeekServers]\n2700 nodes @hed [id=setHed_quickplay]\n3000 quit\n' % mode_b, cfg=cfg)
    finish(p, 60)
    text = read_log(log)
    m = re.search(r"\[input_script\] text t=\d+ @mode n=(\d+) checked=\d value=(\S*)", text)
    check("C3: Settings > General shows the Quickplay section", re.search(r"nodes t=\d+ @hed elements=\d+ match=1", text), text[-400:])
    check("C3: its Seek Mode row holds the saved mode", m and m.group(1) == "1" and m.group(2) == str(mode_b), m.group(0) if m else "")
    check("C3: the Server Type row (native)", re.search(r"text t=\d+ @type n=1 checked=\d value=kn", text), "")
    stored = read_log(os.path.join(cfg, "krunknative", "knSettings.json"))
    check("C3: seek_modes persisted in knSettings", '"seek_modes":"%d"' % mode_b in stored, stored[:200])
    p, log = client("prefs2", port, "0 play\n1000 state\n1500 quit\n", cfg=cfg)
    finish(p, 60)
    text = read_log(log)
    check("C3: the next run's seek lands on the node playing the saved mode", joined(text)[:1] == [game_b[0]], str(joined(text)))


def client_official(port):
    """The browser opt-out (KRUNKNATIVE_KRUNKER_JOIN=browser): official lobbies open on krunker.io.
    The default, in-client, is client_both's and krunker_official_e2e.py's."""
    hits = len(stub_hits)
    p, log = client("official", port, '0 play\n300 call setSetting "knSeekServers" "official"\n'
                    '400 call setSetting "seek_modes" "%d"\n500 call setSetting "seek_maps" "%d"\n'
                    "1000 down F4\n1050 up F4\n4000 call showWindow 2\n6000 text @status [id=kioStatus]\n"
                    "6100 text @head .setHed\n6150 nodes @kgroups .quickJoin[title*=krunker]\n6200 call setExpandedRegion 1\n"
                    "6500 nodes @rows .kioTag\n"
                    "6550 nodes @wired [onclick*=krunkerJoin]\n6600 call krunkerJoin \"NY:bbbbb\"\n7500 quit\n" % (MODES["b"], SANDSTORM),
                    extra_env={"KRUNKNATIVE_KRUNKER_JOIN": "browser"})
    finish(p, 60)
    text = read_log(log)
    check("C4: opt-out: F4 with Official opens the matching official lobby in the browser",
          "[krunker] official lobby NY:bbbbb: opening https://krunker.io/?game=NY:bbbbb" in text
          and "open (disabled by KRUNKNATIVE_NO_BROWSER): https://krunker.io/?game=NY:bbbbb" in text, text[-600:])
    check("C4: ... and stays in its KrunkNative game (no new seek)", len(joined(text)) == 1, str(joined(text)))
    check("C4: the Servers window explains the krunker.io section", re.search(r"@status n=1 .*text=KRUNKER\.IO official lobbies open in your browser", text),
          "")
    m = re.search(r"@head n=\d+ checked=\d value=\S* text=(.*)", text)
    check("C4: the server type only steers the seek: our (tdm) group is still listed, first",
          m and m.group(1) and "krunker.io" not in m.group(1), m.group(1) if m else "")
    m = re.search(r"nodes t=\d+ @kgroups elements=\d+ match=(\d+)", text)
    check("C4: then one krunker.io group (New York: tdm on Sandstorm)", m and m.group(1) == "1", m.group(0) if m else "")
    m = re.search(r"nodes t=\d+ @rows elements=\d+ match=(\d+)", text)
    check("C4: its rows (the open and the full lobby)", m and m.group(1) == "2", m.group(0) if m else "")
    m = re.search(r"nodes t=\d+ @wired elements=\d+ match=(\d+)", text)
    check("C4: the rows open krunkerJoin(id) (the browser), not a seek", m and m.group(1) == "2", m.group(0) if m else "")
    check("C4: krunkerJoin opens its lobby in the browser", text.count("open (disabled by KRUNKNATIVE_NO_BROWSER): https://krunker.io/?game=NY:bbbbb") == 2,
          "")
    check("C4: the official list was fetched once (cached >= 15 s)", len(stub_hits) - hits == 1, str(stub_hits[hits:]))


def client_both(port):
    p, log = client("both", port, '0 play\n300 call setSetting "knSeekServers" "both"\n400 call setSetting "seek_modes" "%d"\n'
                    "1000 down F4\n1050 up F4\n5000 quit\n" % MODES["kio"])
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
        client_f4(port, node_a[1], node_b[1])
        client_f5(port)
        client_settings(port, node_b[1])
        client_official(port)
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
