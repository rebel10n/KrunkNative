#!/usr/bin/env python3
"""Krunker v10.0.2 main menu shell end to end (client/src/ui/v10_menu.c, v10_login.c) on the v10
page (ui/) against a real kn_brain and two brain-linked server_bin nodes.

  python3 -I server/tests/v10_menu_e2e.py BUILD_DIR      (run from the repository root)

kn_brain on a scratch database (KN_TEST_DB_DRIVER, default sqlite: no server needed), two client v10
server_bin nodes (V10A / V10B with this run's pid suffix: one public FFA lobby each) and, after those nodes
are retired, two more (V10P / V10Q) for the direct brain probes, every port from kn_proto.free_ports().
Nothing is shared with another test: the ports are taken just before the nodes bind them, node and
brain ids carry this run's pid, and every process (brain, nodes, clients) has its own HOME and XDG config,
state, data and cache dirs under this run's scratch dir (a saved session, settings or recent lobby
of an earlier e2e is never read).
Headless client_bin runs (one at a time, input scripts clicking the page by element id):

  1. a signed-in player (saved krunker_username / krunker_token): the header is v10's signed-in
     bar (name, level, KR), the nav shows v10's signed-in items (Turf Wars, Market & Trading,
     Leaderboards, the small Guide entry), 120 unread mails read "99" on the Inbox badge, #newUserGuide is set to
     block (level <= 20), the class preview shows, Host Game shows and the
     Connect link is on the match bar after Join (it opens the Connect window);
     Settings opens Game Settings; Find Game opens v10's "Select a Server" chooser and its Public
     tile the server list with both lobbies, and no frame from the click to the list takes over
     500 ms (`ft`: the main thread never freezes); Custom Games opens the Custom Games window with its
     map cards; Host Game opens v10's host chooser; More Krunker opens its popup;
     the header's Wallet opens the Wallet and Inventory modal (not the Account window): KR / Junk / RP
     cards and the empty grid; Escape closes it; its Catalog closes it and opens the exchange's Catalog
     Quick Match seeks again with excludeGame and joins the OTHER lobby; a second one sends the first
     lobby as excludeRecent too and, the two being the only ones, goes back to it (never the current)
  2. a guest: the header has Login or Register, KrunkNative's Account Manager, then Get Signup
     Rewards (in that order), and the Account Manager opens the saved accounts window (knAltTitle);
     "Login or Register" opens v10's modal; Register here -> username, password, email
     -> Register: the account exists in the brain, the modal closes and the header is the
     signed-in bar without a restart; Logout -> the guest bar again; the modal again -> "(use
     username instead)" -> Login with that account's username -> signed in again
     (signing in with the email: v10_email_login_e2e.py)
  3. brain, on its own node pair so these probes' join-ticket reservations cannot fill the clients' lobbies:
     /mm/seek-game?excludeGame= never returns the excluded lobby while another has room;
     excludeRecent covering the rest falls back to the recent lobby before excludeGame's

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
import urllib.parse
import urllib.request

sys.path.insert(0, os.path.join(os.getcwd(), "server/tests"))
import kn_proto  # noqa: E402

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
ROOT = os.getcwd()
DRIVER = os.environ.get("KN_TEST_DB_DRIVER", "sqlite")
NAME = "kn_test_v10menu_%d" % os.getpid()
SECRET = "kn-v10menu-%d" % os.getpid()
RUN = os.getpid() % 100000  # node ids, account names: unique to this run
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-v10menu-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
results = []
procs = []
DB = {}  # the scratch database's config section (db_scratch.py create)


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


def xdg_dirs(name):
    """This process's own HOME and XDG config / state / data / cache dirs: nothing it reads or writes
    (localStorage, KrunkScript storage, server_history, recent lobbies) outlives the run"""
    home = os.path.join(WORK, name + "-home")
    dirs = {"HOME": home, "XDG_CONFIG_HOME": os.path.join(WORK, name + "-cfg"),
            "XDG_STATE_HOME": os.path.join(WORK, name + "-state"), "XDG_DATA_HOME": os.path.join(WORK, name + "-data"),
            "XDG_CACHE_HOME": os.path.join(WORK, name + "-cache")}
    for d in dirs.values():
        os.makedirs(d, exist_ok=True)
    return dirs


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
                         env=dict(os.environ, **xdg_dirs(node_id)), start_new_session=True)
    procs.append(p)
    return p, log


def client(name, brain_port, script, storage=None, secs=120):
    """One headless client_bin on the v10 page, run to its script's quit; returns its log."""
    path = os.path.join(WORK, name + ".script")
    with open(path, "w") as f:
        f.write(script)
    dirs = xdg_dirs(name)
    cfg = dirs["XDG_CONFIG_HOME"]
    os.makedirs(os.path.join(cfg, "krunknative"), exist_ok=True)
    for key, value in dict({"knRuleset": "v10"}, **(storage or {})).items():
        with open(os.path.join(cfg, "krunknative", key + ".json"), "w") as f:
            f.write(value)
    log = os.path.join(WORK, name + ".log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
    env.update(dirs)
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "KRUNKNATIVE_NO_BROWSER": "1",
                "KRUNKNATIVE_INPUT_SCRIPT": path, "KRUNKNATIVE_WINDOW_SIZE": "1280x720", "KRUNKNATIVE_BRAIN_URL": "http://127.0.0.1:%d" % brain_port,
                "KRUNKNATIVE_NET_TRANSPORT": "tcp", "KRUNKNATIVE_DEBUG": "2",  # the [mm] join / seek lines (klog.h level 2)
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


# Find Game (the chooser, the Public list, its region) must never freeze the main thread: every
# wall-clock frame from the click to the list's screenshot stays under this (`ft`, input_script.c).
# A stalled frame's delta fast-forwards the script clock, so a later step can run in the same
# breath as the click that asked for it (the list, before /servers answers). `waitfor` stops
# the clock until the rows exist. The ft window is wall-clock frame time, not that jump.
FIND_GAME_MAX_FRAME_MS = 500

SIGNED_IN_SCRIPT = """0 menu
450 waitfor [id=knV10Title]
460 text @title [id=knV10Title]
470 attr @titlestyle style [id=knV10Title]
480 nodes @titlerow [id=signedInHeaderBar] .ph-name-row .ph-title
490 text @mail [id=mailCount]
490 attr @mailstyle style [id=mailCount]
490 attr @guide style [id=newUserGuide]
500 vis @header signedInHeaderBar signedOutHeaderBar knV10Name menuKRCount menuBtnConnect menuBtnHost menuBtnQuickMatch :preview spectButtonHolder chatUIHolder knV10AccountsBtn knV10AccountsSignedIn
500 vis @nav menuBtnGuide knV10GuideItem menuBtnTurfWars menuBtnMarket menuBtnLeaderboards menuBtnShop menuBtnChall menuBtnSocial menuBtnSideCommunity
600 clickid knV10Settings
1500 vis @settings windowHolder settHolder
1600 down ESC
1620 up ESC
1700 clicksel [title="Wallet"]
1800 vis @wallet knV10WalletRoot windowHolder knV10WalletKR knV10WalletJunk knV10WalletRP knV10WalletEmpty
1850 frames {shots}/wallet-open 1
1860 down ESC
1880 up ESC
1890 vis @walletclosed knV10WalletRoot windowHolder
1900 clicksel [title="Wallet"]
2000 vis @walletagain knV10WalletRoot knV10WalletCatalogBtn
2050 clicksel .wallet-catalog-btn
2150 vis @catalog v10mkRoot knV10WalletRoot windowHolder
2200 call clearPops
2400 ft @find start
2500 clickid menuBtnBrowser
3500 nodes @chooser .serverHostOp
3600 call openServerWindow 0
5500 call setExpandedRegion 0
5600 waitfor .settNameIn
6500 nodes @servers .settNameIn
6600 frames {shots}/servers 1
6900 ft @find stop
7000 down ESC
7020 up ESC
7500 clickid menuBtnCustomGames
7600 waitfor .mapListItem
8500 frames {shots}/games 1
11500 nodes @games .mapListItem
11600 frames {shots}/games2 1
12000 down ESC
12020 up ESC
12500 clickid menuBtnHost
13500 nodes @host .serverHostOp
13600 down ESC
13620 up ESC
14000 clickid knV10MoreKrunker
15000 nodes @more .moreKrunkerOption
15050 grep @more Accounts .moreKrunkerOption
15100 call clearPops
15500 clickid menuBtnQuickMatch
21500 vis @after menuBtnQuickMatch :preview
22000 clickid menuBtnQuickMatch
27500 quit
"""

GUEST_SCRIPT = """0 menu
500 vis @guest signedInHeaderBar signedOutHeaderBar spectButtonHolder chatUIHolder
500 attr @guideguest style [id=newUserGuide]
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
1730 vis @logging knV10LoggingInBar signedOutHeaderBar signedInHeaderBar
1740 waitfor [id=signedInHeaderBar]
6000 vis @registered knV10Login signedInHeaderBar signedOutHeaderBar knV10Name spectButtonHolder chatUIHolder
6000 attr @guidereg style [id=newUserGuide]
6100 call logoutAcc
6700 attr @guideout style [id=newUserGuide]
6800 down ESC
6820 up ESC
6900 down ESC
6920 up ESC
7000 vis @loggedout signedInHeaderBar signedOutHeaderBar spectButtonHolder chatUIHolder
7100 clickid knV10LoginBtn
7400 clickid knV10LoginMode
7500 clickid accName
7550 type {user}
7600 clickid accPass
7650 type {pw}
7700 clickid knV10LoginSubmit
7710 waitfor ![id=knV10Login]
12000 vis @loggedin knV10Login signedInHeaderBar signedOutHeaderBar
12100 quit
"""


# A guest's name in the class box (menu_ui.c menu_guest_name, deob-10.0.7.js:217898): "Guest" on the menu before any
# spawn, "Guest_<sid>" once the game has our player (back on the menu with Escape)
GUEST_NAME_SCRIPT = """0 menu
500 text @tag0 [id=menuClassNameTag]
600 play
700 spawn
3000 down ESC
3020 up ESC
4000 text @tag1 [id=menuClassNameTag]
4100 quit
"""


def text_of(text, label):
    m = re.search(r"\[input_script\] text t=\d+ @%s n=\d+ .* text=(.*)" % re.escape(label), text)
    return m.group(1).strip() if m else None


def guest_name_case(brain_port):
    text = client("guestname", brain_port, GUEST_NAME_SCRIPT)
    t0, t1 = text_of(text, "tag0"), text_of(text, "tag1")
    check("2: a guest's class box name on the menu before any spawn is Guest", t0 == "Guest", str(t0))
    check("2: ... and Guest_<sid> once the game has our player", bool(t1 and re.fullmatch(r"Guest_\d+", t1)), str(t1))


# The guest header's Account Manager (v10_menu.c header_setup): between Login or Register and Get
# Signup Rewards, and its click opens the saved accounts window (ALT_MANAGER_WINDOW, alt_manager_ui.c)
ACCOUNTS_SCRIPT = """0 menu
500 rect @hdr knV10LoginBtn knV10AccountsBtn signupRewardsButton
600 clickid knV10AccountsBtn
1200 nodes @window .knAltTitle
1250 text @title .knAltTitle
1300 quit
"""


def rects_of(text, label):
    """The `rect @label id ...` line of an input script log as {id: (x, y, w, h)} (None = absent)"""
    m = re.search(r"\[input_script\] rect t=\d+ @%s (.*)" % re.escape(label), text)
    out = {}
    for kv in (m.group(1).split() if m else []):
        k, _, v = kv.partition("=")
        parts = v.split(",")
        out[k] = tuple(float(x) for x in parts[:4]) if len(parts) >= 4 else None
    return out


# The signed-in bar's Account Manager (the same item, after the profile): before KR, and it opens the window
SIGNED_ACCOUNTS_SCRIPT = """0 menu
500 rect @hdr knV10Profile knV10AccountsSignedIn menuKRCount
600 clickid knV10AccountsSignedIn
1200 nodes @window .knAltTitle
1250 text @title .knAltTitle
1300 quit
"""


def accounts_window_checks(text, prefix):
    check(prefix + ": the Account Manager opens the saved accounts window", nodes(text, "window") == 1, str(nodes(text, "window")))
    t = re.search(r"\[input_script\] text t=\d+ @title n=\d+ .* text=(.*)", text)
    check(prefix + ": ... titled Accounts", t and t.group(1).strip() == "Accounts", t.group(0) if t else "no text line")


def accounts_case(brain_port):
    text = client("accounts", brain_port, ACCOUNTS_SCRIPT)
    r = rects_of(text, "hdr")
    login, acc, rewards = (r.get(i) for i in ("knV10LoginBtn", "knV10AccountsBtn", "signupRewardsButton"))
    check("2: the guest header has the Account Manager button", acc is not None, str(r))
    check("2: Account Manager sits between Login or Register and Get Signup Rewards",
          login and acc and rewards and login[0] + login[2] <= acc[0] and acc[0] + acc[2] <= rewards[0],
          "login %s account %s rewards %s" % (login, acc, rewards))
    accounts_window_checks(text, "2")


def signed_accounts_case(brain_port, user, token):
    text = client("accounts-in", brain_port, SIGNED_ACCOUNTS_SCRIPT, {"krunker_username": user, "krunker_token": token})
    r = rects_of(text, "hdr")
    prof, acc, kr = (r.get(i) for i in ("knV10Profile", "knV10AccountsSignedIn", "menuKRCount"))
    check("1: the signed-in header has the Account Manager button", acc is not None, str(r))
    check("1: Account Manager sits after the profile and before KR",
          prof and acc and kr and prof[0] + prof[2] <= acc[0] and acc[0] + acc[2] <= kr[0],
          "profile %s account %s KR %s" % (prof, acc, kr))
    accounts_window_checks(text, "1")


def signed_in_case(brain_port, games):
    user = "v10m%d" % RUN
    st, reg = http(brain_port, "/auth/register", {"username": user, "password": "pw-v10menu-%d" % os.getpid()})
    token = reg.get("token", "") if isinstance(reg, dict) else ""
    check("1: account registered", st == 200 and token, str(reg)[:200])
    # the equipped title in the header (.ph-title, deob-10.0.7.js:148780-148797): the account owns title 91
    # ("Pinpoint", player_titles: "ttl") and wears it (titleIndex)
    titled = False
    if DB.get("driver") == "sqlite" and DB.get("path"):
        import sqlite3
        con = sqlite3.connect(DB["path"])
        con.execute("INSERT INTO player_titles (account_id, title_id, source, ref) SELECT id, 91, 'blackmarket', 1866 "
                    "FROM accounts WHERE lower(username) = lower(?)", (user,))
        con.commit()
        # 120 unread mails: the Inbox badge reads "99" (windows[29].mailCount, deob-10.0.7.js:221231-221235)
        con.executemany("INSERT INTO mail (account_id, subject, body) SELECT id, ?, '' FROM accounts WHERE lower(username) = lower(?)",
                        [("mail %d" % i, user) for i in range(120)])
        con.commit()
        titled = con.execute("SELECT count(*) FROM player_titles pt JOIN accounts a ON a.id = pt.account_id "
                             "WHERE lower(a.username) = lower(?)", (user,)).fetchone()[0] == 1
        con.close()
    text = client("signedin", brain_port, SIGNED_IN_SCRIPT.format(shots=WORK),
                  {"krunker_username": user, "krunker_token": token, "titleIndex": "91"})
    if titled:
        title = re.search(r"\[input_script\] text t=\d+ @title n=(\d+) .* text=(.*)", text)
        style = re.search(r"\[input_script\] attr t=\d+ @titlestyle n=\d+ style=(.*)", text)
        check("1: the header shows the equipped title after the name", title and title.group(1) == "1" and title.group(2).strip() == "Pinpoint"
              and nodes(text, "titlerow") == 1, title.group(0) if title else "no text line")
        check("1: ... in its rarity colour", style and style.group(1).startswith("color:#"), style.group(0) if style else "no attr line")
    signed_accounts_case(brain_port, user, token)
    if titled:
        mail = re.search(r"\[input_script\] text t=\d+ @mail n=(\d+) .* text=(.*)", text)
        mstyle = attr_of(text, "mailstyle")
        check("1: 120 unread mails: the Inbox badge reads 99 (> 99 -> \"99\"), shown inline-block",
              mail and mail.group(2).strip() == "99" and mstyle and "inline-block" in mstyle, "%s / %s" % (mail.group(0) if mail else None, mstyle))
    check("1: #newUserGuide: a login up to level 20 sets it to block (deob-10.0.7.js:216887)",
          "block" in (attr_of(text, "guide") or ""), str(attr_of(text, "guide")))
    joins = re.findall(r"\[mm\] joining (\S+) at", text)
    h = vis(text, "header")
    check("1: signed-in header bar (no guest bar)", h.get("signedInHeaderBar") == "1" and h.get("signedOutHeaderBar") == "0", str(h))
    check("1: name and KR in the header", h.get("knV10Name") == "1" and h.get("menuKRCount") == "1", str(h))
    check("1: Connect link, Host Game and Quick Match shown", h.get("menuBtnConnect") == "1" and h.get("menuBtnHost") == "1"
          and h.get("menuBtnQuickMatch") == "1", str(h))
    check("1: the class preview is drawn", h.get(":preview") == "1", str(h))
    # an account gets the Spectate switch on the menu (hideSpectateIfGuestUser v10:208579) and the chat
    # (hideChatIfGuestUser, deob-10.0.7.js:215587; hud_v10.c hud_v10_tick)
    check("1: chat and Spectate switch on the menu for an account", h.get("chatUIHolder") == "1" and h.get("spectButtonHolder") == "1", str(h))
    n = vis(text, "nav")
    check("1: signed-in nav (Turf Wars, Market, Leaderboards, small Guide; no Guide card)",
          n.get("menuBtnTurfWars") == "1" and n.get("menuBtnMarket") == "1" and n.get("menuBtnLeaderboards") == "1"
          and n.get("knV10GuideItem") == "1" and n.get("menuBtnGuide") == "0", str(n))
    s = vis(text, "settings")
    check("1: Settings opens Game Settings", s.get("windowHolder") == "1" and s.get("settHolder") == "1", str(s))
    # the header's Wallet (v10's Wallet and Inventory modal, v10_wallet.c), not the Account window (windowHolder)
    w = vis(text, "wallet")
    check("1: the header Wallet opens the Wallet and Inventory modal, not the Account window",
          w.get("knV10WalletRoot") == "1" and w.get("windowHolder") in ("0", "-"), str(w))
    check("1: the modal shows the KR, Junk and RP cards and, with no skins, its empty grid",
          w.get("knV10WalletKR") == "1" and w.get("knV10WalletJunk") == "1" and w.get("knV10WalletRP") == "1"
          and w.get("knV10WalletEmpty") == "1", str(w))
    wc = vis(text, "walletclosed")
    check("1: Escape closes the Wallet modal", wc.get("knV10WalletRoot") in ("-", "0"), str(wc))
    cat = vis(text, "catalog")
    check("1: the modal's Catalog closes it and opens the exchange (openItems: the Catalog tab)",
          cat.get("v10mkRoot") == "1" and cat.get("knV10WalletRoot") in ("-", "0") and cat.get("windowHolder") in ("0", "-"), str(cat))
    check("1: Find Game opens the Select a Server chooser", nodes(text, "chooser") == 5, str(nodes(text, "chooser")))
    rows = nodes(text, "servers")
    check("1: its Public list shows both lobbies", rows >= 2, "%d rows" % rows)
    ft = re.search(r"\[input_script\] ft t=\d+ @find frames=(\d+) .* max=([\d.]+) ms", text)
    check("1: no frame of Find Game takes over %d ms" % FIND_GAME_MAX_FRAME_MS,
          ft and int(ft.group(1)) > 0 and float(ft.group(2)) <= FIND_GAME_MAX_FRAME_MS, ft.group(0) if ft else "no ft line")
    cards = nodes(text, "games")
    check("1: Custom Games lists games", cards > 0, "%d cards" % cards)
    # v10's five cards and Box Office's Rent a Server card (rent_servers_ui_chooser_card)
    check("1: Host Game opens the host chooser", nodes(text, "host") == 6, str(nodes(text, "host")))
    # v10's popup has Mods, Sandbox, Editor and KPD; the Account Manager is the guest header's button
    check("1: More Krunker opens its popup with v10's four options", nodes(text, "more") == 4, str(nodes(text, "more")))
    check("1: the signed-in header has the Account Manager button, the guest one hidden",
          h.get("knV10AccountsSignedIn") == "1" and h.get("knV10AccountsBtn") in ("0", "-"), str(h))
    qm = re.search(r"\[mm\] quick match: seeking a lobby other than (\S+)", text)
    check("1: Quick Match seeks with the current lobby excluded", qm and joins and qm.group(1) == joins[0],
          "%s %s" % (qm.group(1) if qm else None, joins))
    ok = len(joins) >= 2 and joins[1] != joins[0] and joins[1] in games
    # on a failure, the brain's view of the lobbies (players, max, meta) for the kept log
    check("1: Quick Match joins the other lobby", ok,
          str(joins) + ("" if ok else " game-list " + json.dumps(http(brain_port, "/mm/game-list?hostname=krunknative")[1])[:1500]))
    # the second Quick Match: excludeGame = this lobby, excludeRecent = the one before it (recent_lobbies.h);
    # with only those two, the brain falls back to the one before, never the one it leaves
    qm2 = re.search(r"\[mm\] seek: excluding (\S+), then the recent (\S+)", text)
    check("1: a second Quick Match also excludes the lobby before (excludeRecent)",
          qm2 and len(joins) >= 2 and (qm2.group(1), qm2.group(2)) == (joins[1], joins[0]), "%s %s" % (qm2.groups() if qm2 else None, joins))
    check("1: ... and with nothing else, joins the one before, not the one it leaves", len(joins) == 3 and joins[2] == joins[0],
          str(joins))
    a = vis(text, "after")
    check("1: back on the menu with the new game", a.get("menuBtnQuickMatch") == "1", str(a))


def attr_of(text, label):
    """The value of an `attr @label <name> <selector>` line, None without one"""
    m = re.search(r"\[input_script\] attr t=\d+ @%s n=\d+ \S+?=(.*)" % re.escape(label), text)
    return m.group(1).strip() if m else None


def guest_case(brain_port):
    user = "v10g%d" % RUN
    pw = "pw-v10guest-%d" % os.getpid()
    text = client("guest", brain_port, GUEST_SCRIPT.format(user=user, pw=pw))
    g = vis(text, "guest")
    check("2: guest header bar", g.get("signedOutHeaderBar") == "1" and g.get("signedInHeaderBar") in ("-", "0"), str(g))
    check("2: a guest has no chat (hideChatIfGuestUser, deob-10.0.7.js:215587) and no Spectate switch on the menu (v10:208579)",
          g.get("chatUIHolder") == "0" and g.get("spectButtonHolder") == "0", str(g))
    lg = vis(text, "logging")
    # $loggingIn (deob-10.0.7.js:148818-148821): while the registration waits for its answer the guest bar is
    # "Logging in..." (v10_login_logging_in)
    check("2: a sent registration: the header says Logging in... in place of the guest bar",
          lg.get("knV10LoggingInBar") == "1" and lg.get("signedOutHeaderBar") == "0" and lg.get("signedInHeaderBar") in ("-", "0"), str(lg))
    m = vis(text, "modal")
    # a guest with no saved username: v10's Email field first (the "(use username instead)" toggle -> Username)
    check("2: Login or Register opens the modal", m.get("knV10Login") == "1" and m.get("accEmail") == "1", str(m))
    r = vis(text, "registered")
    check("2: Register signs in: modal closed, signed-in header", r.get("knV10Login") == "-" and r.get("signedInHeaderBar") == "1"
          and r.get("signedOutHeaderBar") == "0" and r.get("knV10Name") == "1", str(r))
    check("2: signed in: the chat is up on the idle menu, Spectate switch on the menu", r.get("chatUIHolder") == "1" and r.get("spectButtonHolder") == "1", str(r))
    st, body = http(brain_port, "/auth/login", {"username": user, "password": pw})
    check("2: the account exists in the brain", st == 200 and isinstance(body, dict) and body.get("token"), str(body)[:200])
    o = vis(text, "loggedout")
    check("2: Logout: the guest header again", o.get("signedOutHeaderBar") == "1" and o.get("signedInHeaderBar") == "0", str(o))
    check("2: Logout: the chat goes from the menu (deob-10.0.7.js:216910), no Spectate switch", o.get("chatUIHolder") == "0" and o.get("spectButtonHolder") == "0", str(o))
    # #newUserGuide: the markup's display:none for a fresh guest, block after a level-1 login and after logoutAcc
    # (deob-10.0.7.js:148225, :216887, :216910)
    gg, gr, go = attr_of(text, "guideguest"), attr_of(text, "guidereg"), attr_of(text, "guideout")
    check("2: #newUserGuide: none for a fresh guest, block after the login (level 1) and after the logout",
          gg is not None and "none" in gg and gr is not None and "block" in gr and go is not None and "block" in go, "%s / %s / %s" % (gg, gr, go))
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
    if len(games) >= 2:
        # excludeRecent: skipped too; with every lobby excluded, the recent one comes back before excludeGame's
        a, b = games[0], games[1]
        code, rep = http(brain_port, "/mm/seek-game?hostname=krunknative&region=local&ruleset=v10&excludeGame=%s&excludeRecent=%s" % (a, b))
        check("3: excludeGame + excludeRecent covering every lobby: the recent one, not excludeGame's",
              code == 200 and rep.get("gameId") == b, "%s %s" % (code, rep))
        # a dsma past 32 entries (the rotation has 34) with the lobby's map last: the brain once kept the first 32
        # only, so a Quick Match never found a lobby on Barnyard (45) or Candy Heist (46)
        lst = http(brain_port, "/mm/game-list?hostname=krunknative")[1]
        meta = {g[0]: g[4] for g in (lst.get("games") or []) if len(g) > 4} if isinstance(lst, dict) else {}
        m = (meta.get(b) or {}).get("mapIndex", -1)
        if isinstance(m, int) and m >= 0:
            dq = json.dumps({"dsma": [i for i in range(47) if i != m][:40] + [m]}, separators=(",", ":"))
            code, rep = http(brain_port, "/mm/seek-game?hostname=krunknative&region=local&ruleset=v10&excludeGame=%s&dataQuery=%s"
                             % (a, urllib.parse.quote(dq)))
            check("3: a 41-map dsma with the lobby's map (%d) last still finds it" % m, code == 200 and rep.get("gameId") == b,
                  "%s %s" % (code, rep))
        else:
            check("3: the other lobby lists its mapIndex", False, str(meta.get(b)))


def main():
    if not os.access(f"{BUILD}/client_bin", os.X_OK):
        print("v10_menu_e2e: no client_bin in %s: skipped" % BUILD)
        return 77
    scratch = [sys.executable, "-I", os.path.join(ROOT, "services", "brain", "tests", "db_scratch.py"), ROOT]
    r = subprocess.run(scratch + ["create", DRIVER, NAME], capture_output=True)
    if r.returncode:
        print("v10_menu_e2e: %s unavailable: skipped" % DRIVER)
        return r.returncode
    DB.update(json.loads(r.stdout))
    dbenv = {"KN_DATABASE_" + k.upper(): str(v) for k, v in DB.items()}
    # debug: a kept failure log has every seek's answer (routes)
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=SECRET, KN_LOG_LEVEL="debug", **dbenv, **xdg_dirs("brain"))
    brain_log = os.path.join(WORK, "brain.log")
    try:
        subprocess.run([f"{BUILD}/services/brain/kn_brain", "--migrate-only"], cwd=ROOT, env=env, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        brain = subprocess.Popen([f"{BUILD}/services/brain/kn_brain", "--brain.host=127.0.0.1", "--brain.port=0", "--accounts.signup_rewards.enabled=false",
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
        # the ports now, just before the nodes bind them (not at import, minutes before)
        port_a, port_b = kn_proto.free_ports(2)
        node_a, _ = node(port, "V10A%d" % RUN, port_a)
        node_b, _ = node(port, "V10B%d" % RUN, port_b)
        games = wait_for(lambda: (lambda l: l if len(l) == 2 else None)(lobbies(port)), 20) or []
        check("two v10 lobbies listed", len(games) == 2, str(lobbies(port)))
        if len(games) != 2:
            return 1
        listed = wait_for(lambda: len(http(port, "/servers")[1].get("servers", [])) == 2, 20)
        check("both nodes in the server finder", listed, str(http(port, "/servers")[1])[:300])
        signed_in_case(port, games)
        guest_case(port)
        guest_name_case(port)
        accounts_case(port)
        # The direct seek probes reserve join tickets. Retire the client nodes and wait for their lobbies to go,
        # then give the probes their own pair: neither side's reservations can fill the other side's lobbies,
        # and excludeRecent really does cover every remaining lobby.
        for p in (node_a, node_b):
            if p.poll() is None:
                os.killpg(p.pid, signal.SIGTERM)
        for p in (node_a, node_b):
            try:
                p.wait(10)
            except subprocess.TimeoutExpired:
                os.killpg(p.pid, signal.SIGKILL)
                p.wait()
        gone = wait_for(lambda: len(lobbies(port)) == 0, 10)
        check("client lobbies retired before isolated brain probes", bool(gone), str(lobbies(port)))
        if not gone:
            return 1
        probe_a, probe_b = kn_proto.free_ports(2)
        node(port, "V10P%d" % RUN, probe_a)
        node(port, "V10Q%d" % RUN, probe_b)
        prefixes = ("V10P%d:" % RUN, "V10Q%d:" % RUN)
        probes = wait_for(lambda: (lambda l: l if len(l) == 2 else None)([g for g in lobbies(port) if g.startswith(prefixes)]), 20) or []
        check("two isolated brain-probe lobbies listed", len(probes) == 2, str(probes))
        if len(probes) != 2:
            return 1
        brain_case(port, probes)
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
    if all(results) and not os.environ.get("KN_KEEP"):
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print("logs kept in " + WORK)
    print(f"{sum(results)}/{len(results)} passed")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
