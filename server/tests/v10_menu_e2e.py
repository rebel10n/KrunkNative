#!/usr/bin/env python3
"""Krunker v10.0.2 main menu shell end to end (client/src/ui/v10_menu.c, v10_login.c) on the v10
page (ui/v10) against a real kn_brain and two brain-linked server_bin nodes.

  python3 -I server/tests/v10_menu_e2e.py BUILD_DIR      (run from the repository root)

kn_brain on a scratch database (KN_TEST_DB_DRIVER, default sqlite: no server needed), two v10
server_bin nodes (V10A, V10B: one public FFA lobby each), every port from kn_proto.free_ports().
Headless client_bin runs (one at a time, input scripts clicking the page by element id):

  1. a signed-in player (saved krunker_username / krunker_token): the header is v10's signed-in
     bar (name, level, KR), the nav shows v10's signed-in items (Turf Wars, Market & Trading,
     Leaderboards, the small Guide entry), the class preview shows, Host Game shows and the
     old Connect button is gone;
     Settings opens Game Settings; Find Game opens v10's "Select a Server" chooser and its Public
     tile the server list with both lobbies; Custom Games opens the Custom Games window with its
     map cards; Host Game opens v10's host chooser; More Krunker opens its popup;
     Quick Match seeks again with excludeGame and joins the OTHER lobby
  2. a guest: "Login or Register" opens v10's modal; Register here -> username, password, email
     -> Register: the account exists in the brain, the modal closes and the header is the
     signed-in bar without a restart; Logout -> the guest bar again; the modal again -> "(use
     username instead)" -> Login with that account's username -> signed in again
     (signing in with the email: v10_email_login_e2e.py)
  3. brain: /mm/seek-game?excludeGame= never returns the excluded lobby while another has room

Needs a headless GL device (KRUNKNATIVE_HEADLESS=1: EGL). Exit 77 when the database is unavailable.
"""
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import urllib.request

sys.path.insert(0, os.path.join(os.getcwd(), "server/tests"))
import kn_proto  # noqa: E402

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
ROOT = os.getcwd()
DRIVER = os.environ.get("KN_TEST_DB_DRIVER", "sqlite")
NAME = "kn_test_v10menu_%d" % os.getpid()
SECRET = "kn-v10menu-%d" % os.getpid()
PORT_A, PORT_B = kn_proto.free_ports(2)
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-v10menu-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
results = []
procs = []


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


def http(port, path, body=None):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request("http://127.0.0.1:%d%s" % (port, path), data=data,
                                 headers={"Content-Type": "application/json"} if data else {},
                                 method="POST" if data else "GET")
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status, json.loads(r.read() or b"null")
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b"null")


def lobbies(port):
    code, body = http(port, "/mm/game-list?hostname=krunknative")
    return sorted(g[0] for g in body.get("games", [])) if code == 200 else []


def node(brain_port, node_id, game_port):
    log = os.path.join(WORK, node_id + ".log")
    gs = os.path.join(WORK, node_id + ".json")
    cfg = {"node_id": node_id, "name": "v10 menu e2e " + node_id, "region": "local", "listen_port": game_port,
           "public_host": "127.0.0.1", "brain_url": "ws://127.0.0.1:%d/mm/internal/ws" % brain_port,
           "brain_secret": SECRET, "modes": ["ffa"], "max_players": 8, "shutdown_grace": 0,
           "idle_kick_seconds": 0, "ruleset": "v10", "finder_url": "http://127.0.0.1:%d" % brain_port}
    with open(gs, "w") as f:
        json.dump({"gameserver": cfg}, f)
    p = subprocess.Popen([f"{BUILD}/server_bin", "--headless", "--config", gs, "--cfg", "none"], cwd=ROOT,
                         stdout=open(log, "w"), stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                         start_new_session=True)
    procs.append(p)
    return p, log


def client(name, brain_port, script, storage=None, secs=120):
    """One headless client_bin on the v10 page, run to its script's quit; returns its log."""
    path = os.path.join(WORK, name + ".script")
    with open(path, "w") as f:
        f.write(script)
    cfg = os.path.join(WORK, name + "-cfg")
    os.makedirs(os.path.join(cfg, "krunknative"), exist_ok=True)
    for key, value in dict({"knRuleset": "v10"}, **(storage or {})).items():
        with open(os.path.join(cfg, "krunknative", key + ".json"), "w") as f:
            f.write(value)
    log = os.path.join(WORK, name + ".log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": cfg,
                "XDG_STATE_HOME": os.path.join(WORK, name + "-state"), "KRUNKNATIVE_NO_BROWSER": "1",
                "KRUNKNATIVE_INPUT_SCRIPT": path, "KRUNKNATIVE_WINDOW_SIZE": "1280x720",
                "KRUNKNATIVE_UI_DIR": "ui/v10", "KRUNKNATIVE_BRAIN_URL": "http://127.0.0.1:%d" % brain_port,
                "KRUNKNATIVE_NET_TRANSPORT": "tcp",
                # the server finder is this brain's (finder role): the nodes heartbeat to it
                "KRUNKNATIVE_FINDER_URL": "http://127.0.0.1:%d/servers" % brain_port})
    p = subprocess.Popen(["stdbuf", "-oL", "-eL", f"{BUILD}/client_bin"], cwd=ROOT, stdout=open(log, "w"),
                         stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, start_new_session=True)
    procs.append(p)
    try:
        p.wait(secs)
    except subprocess.TimeoutExpired:
        os.killpg(p.pid, signal.SIGTERM)
        try:
            p.wait(5)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
        check(name + ": client finished its script", False, "timed out; log " + log)
    return read_log(log)


def vis(text, label):
    """The `vis @label a=1 b=0` line of an input script log as {id: value}"""
    m = re.search(r"\[input_script\] vis t=\d+ @%s (.*)" % re.escape(label), text)
    return dict(kv.split("=", 1) for kv in m.group(1).split()) if m else {}


def nodes(text, label):
    """The match count of the `nodes @label <selector>` line, -1 without one"""
    m = re.search(r"\[input_script\] nodes t=\d+ @%s elements=\d+ match=(\d+)" % re.escape(label), text)
    return int(m.group(1)) if m else -1


SIGNED_IN_SCRIPT = """0 menu
500 vis @header signedInHeaderBar signedOutHeaderBar knV10Name menuKRCount menuBtnConnect menuBtnHost menuBtnQuickMatch :preview
500 vis @nav menuBtnGuide knV10GuideItem menuBtnTurfWars menuBtnMarket menuBtnLeaderboards menuBtnShop menuBtnChall menuBtnSocial menuBtnSideCommunity
600 clickid knV10Settings
1500 vis @settings windowHolder settHolder
1600 down ESC
1620 up ESC
2000 clickid menuBtnBrowser
3000 nodes @chooser .serverHostOp
3100 call openServerWindow 0
5000 call setExpandedRegion 0
6000 nodes @servers .settNameIn
6100 frames {shots}/servers 1
6500 down ESC
6520 up ESC
7000 clickid menuBtnCustomGames
8000 frames {shots}/games 1
11000 nodes @games .mapListItem
11100 frames {shots}/games2 1
11500 down ESC
11520 up ESC
12000 clickid menuBtnHost
13000 nodes @host .serverHostOp
13100 down ESC
13120 up ESC
13500 clickid knV10MoreKrunker
14500 nodes @more .moreKrunkerOption
14600 call clearPops
15000 clickid menuBtnQuickMatch
21000 vis @after menuBtnQuickMatch :preview
21100 quit
"""

GUEST_SCRIPT = """0 menu
500 vis @guest signedInHeaderBar signedOutHeaderBar
600 clickid knV10LoginBtn
1000 vis @modal knV10Login accEmail accName
1100 clickid knV10AuthSwitch
1400 clickid accName
1450 type {user}
1500 clickid accPass
1550 type {pw}
1600 clickid accEmail
1650 type {user}@example.com
1700 clickid knV10LoginSubmit
6000 vis @registered knV10Login signedInHeaderBar signedOutHeaderBar knV10Name
6100 call logoutAcc
6800 down ESC
6820 up ESC
7000 vis @loggedout signedInHeaderBar signedOutHeaderBar
7100 clickid knV10LoginBtn
7400 clickid knV10LoginMode
7500 clickid accName
7550 type {user}
7600 clickid accPass
7650 type {pw}
7700 clickid knV10LoginSubmit
12000 vis @loggedin knV10Login signedInHeaderBar signedOutHeaderBar
12100 quit
"""


def signed_in_case(brain_port, games):
    user = "v10m%d" % (os.getpid() % 100000)
    st, reg = http(brain_port, "/auth/register", {"username": user, "password": "pw-v10menu-%d" % os.getpid()})
    token = reg.get("token", "") if isinstance(reg, dict) else ""
    check("1: account registered", st == 200 and token, str(reg)[:200])
    text = client("signedin", brain_port, SIGNED_IN_SCRIPT.format(shots=WORK), {"krunker_username": user, "krunker_token": token})
    joins = re.findall(r"\[mm\] joining (\S+) at", text)
    h = vis(text, "header")
    check("1: signed-in header bar (no guest bar)", h.get("signedInHeaderBar") == "1" and h.get("signedOutHeaderBar") == "0", str(h))
    check("1: name and KR in the header", h.get("knV10Name") == "1" and h.get("menuKRCount") == "1", str(h))
    check("1: no Connect button, Host Game and Quick Match shown", h.get("menuBtnConnect") == "-" and h.get("menuBtnHost") == "1"
          and h.get("menuBtnQuickMatch") == "1", str(h))
    check("1: the class preview is drawn", h.get(":preview") == "1", str(h))
    n = vis(text, "nav")
    check("1: signed-in nav (Turf Wars, Market, Leaderboards, small Guide; no Guide card)",
          n.get("menuBtnTurfWars") == "1" and n.get("menuBtnMarket") == "1" and n.get("menuBtnLeaderboards") == "1"
          and n.get("knV10GuideItem") == "1" and n.get("menuBtnGuide") == "0", str(n))
    s = vis(text, "settings")
    check("1: Settings opens Game Settings", s.get("windowHolder") == "1" and s.get("settHolder") == "1", str(s))
    check("1: Find Game opens the Select a Server chooser", nodes(text, "chooser") == 5, str(nodes(text, "chooser")))
    rows = nodes(text, "servers")
    check("1: its Public list shows both lobbies", rows >= 2, "%d rows" % rows)
    cards = nodes(text, "games")
    check("1: Custom Games lists games", cards > 0, "%d cards" % cards)
    check("1: Host Game opens the host chooser", nodes(text, "host") == 5, str(nodes(text, "host")))
    check("1: More Krunker opens its popup", nodes(text, "more") == 4, str(nodes(text, "more")))
    qm = re.search(r"\[mm\] quick match: seeking a lobby other than (\S+)", text)
    check("1: Quick Match seeks with the current lobby excluded", qm and joins and qm.group(1) == joins[0],
          "%s %s" % (qm.group(1) if qm else None, joins))
    check("1: Quick Match joins the other lobby", len(joins) >= 2 and joins[-1] != joins[0] and joins[-1] in games,
          str(joins))
    a = vis(text, "after")
    check("1: back on the menu with the new game", a.get("menuBtnQuickMatch") == "1", str(a))


def guest_case(brain_port):
    user = "v10g%d" % (os.getpid() % 100000)
    pw = "pw-v10guest-%d" % os.getpid()
    text = client("guest", brain_port, GUEST_SCRIPT.format(user=user, pw=pw))
    g = vis(text, "guest")
    check("2: guest header bar", g.get("signedOutHeaderBar") == "1" and g.get("signedInHeaderBar") in ("-", "0"), str(g))
    m = vis(text, "modal")
    # a guest with no saved username: v10's Email field first (the "(use username instead)" toggle -> Username)
    check("2: Login or Register opens the modal", m.get("knV10Login") == "1" and m.get("accEmail") == "1", str(m))
    r = vis(text, "registered")
    check("2: Register signs in: modal closed, signed-in header", r.get("knV10Login") == "-" and r.get("signedInHeaderBar") == "1"
          and r.get("signedOutHeaderBar") == "0" and r.get("knV10Name") == "1", str(r))
    st, body = http(brain_port, "/auth/login", {"username": user, "password": pw})
    check("2: the account exists in the brain", st == 200 and isinstance(body, dict) and body.get("token"), str(body)[:200])
    o = vis(text, "loggedout")
    check("2: Logout: the guest header again", o.get("signedOutHeaderBar") == "1" and o.get("signedInHeaderBar") == "0", str(o))
    li = vis(text, "loggedin")
    check("2: Login from the modal: signed in again", li.get("knV10Login") == "-" and li.get("signedInHeaderBar") == "1", str(li))


def brain_case(brain_port, games):
    for g in games:
        picked = set()
        for _ in range(4):
            code, rep = http(brain_port, "/mm/seek-game?hostname=krunknative&region=local&ruleset=v10&excludeGame=" + g)
            if code == 200:
                picked.add(rep.get("gameId"))
        check("3: excludeGame=%s never picks it" % g, picked and g not in picked, str(picked))


def main():
    if not os.access(f"{BUILD}/client_bin", os.X_OK):
        print("v10_menu_e2e: no client_bin in %s: skipped" % BUILD)
        return 77
    scratch = [sys.executable, "-I", os.path.join(ROOT, "services", "brain", "tests", "db_scratch.py"), ROOT]
    r = subprocess.run(scratch + ["create", DRIVER, NAME], capture_output=True)
    if r.returncode:
        print("v10_menu_e2e: %s unavailable: skipped" % DRIVER)
        return r.returncode
    dbenv = {"KN_DATABASE_" + k.upper(): str(v) for k, v in json.loads(r.stdout).items()}
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=SECRET, **dbenv)
    brain_log = os.path.join(WORK, "brain.log")
    try:
        subprocess.run([f"{BUILD}/services/brain/kn_brain", "--migrate-only"], cwd=ROOT, env=env, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        brain = subprocess.Popen([f"{BUILD}/services/brain/kn_brain", "--brain.host=127.0.0.1", "--brain.port=0",
                                  "--brain.region=local", "--brain.id=v10menu-%d" % os.getpid(),
                                  "--finder.poll_interval_s=1"],
                                 cwd=ROOT, env=env, stdout=open(brain_log, "w"), stderr=subprocess.STDOUT,
                                 stdin=subprocess.DEVNULL, start_new_session=True)
        procs.append(brain)
        m = wait_for(lambda: re.search(r"listening on http://127\.0\.0\.1:(\d+) \(ws", read_log(brain_log)), 15)
        check("kn_brain starts", bool(m), read_log(brain_log)[-300:])
        if not m:
            return 1
        port = int(m.group(1))
        node(port, "V10A", PORT_A)
        node(port, "V10B", PORT_B)
        games = wait_for(lambda: (lambda l: l if len(l) == 2 else None)(lobbies(port)), 20) or []
        check("two v10 lobbies listed", len(games) == 2, str(lobbies(port)))
        if len(games) != 2:
            return 1
        listed = wait_for(lambda: len(http(port, "/servers")[1].get("servers", [])) == 2, 20)
        check("both nodes in the server finder", listed, str(http(port, "/servers")[1])[:300])
        brain_case(port, games)
        signed_in_case(port, games)
        guest_case(port)
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
        subprocess.run(scratch + ["drop", DRIVER, NAME], capture_output=True)
    if all(results):
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print("logs kept in " + WORK)
    print(f"{sum(results)}/{len(results)} passed")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
