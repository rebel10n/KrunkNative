#!/usr/bin/env python3
"""Krunker v10.0.2 Ranked popup and menu end to end (client/src/ui/ranked_ui.c, v10_menu.c,
v10_rewards.c) against a real kn_brain, from the v10 page (ui/), one headless client_bin:

  python3 -I server/tests/ranked_menu_e2e.py BUILD_DIR      (run from the repository root)

kn_brain on a scratch database (KN_TEST_DB_DRIVER, default sqlite), one signed-in level-20 account.
The client's script (KRUNKNATIVE_INPUT_SCRIPT, text checks from the [input_script] log lines):

  0. a plain home load opens no Ranked popup: the rejoin check (fetchIsPlayerInRankedMatch on
     the home, v10:209847) finds no match for this player
  1. "Ranked Challenges" on the home (openRankedChallenges, v10:215670): the Challenges window
     opens at home, its Ranked block (#challRankedHeadr) is scrolled into the window
  2. Ranked (openRankedMenu): the popup with its start button
  2b. FIND MATCH with the queue's answer held back (a byte relay in front of the brain, QUEUE_DELAY_S): the button
     reads LOADING... and the footer Ready text until the QUEUED frame, then the search starts (v10:98459, 98420)
  3. the brain is killed by this harness while the popup is up; FIND MATCH then fails to reach the
     matchmaking server: v10 shows the "Internal Error" popup (v10:98443-98450, 37958) and keeps the banner's
     "Failed to connect to matchmaking server" text

  H. the Home tab's map carousel is v10's markup (ranked_ui.c gen_home, v10:111965-111969): every pool map
     is a plain .map-card with an unlabelled .map-image, none greyed, even one the brain marks unavailable.
     A --render-scenario run of tools/uidiff/scenarios/ranked-home.json (no brain) dumps the document
     (KRUNKNATIVE_SCENARIO_HTML) and checks it.

  I. the Shop's item view (ranked_ui.c item_view, v10's ShareButton, v10:103485-103520): Share copies the
     item's link and the button reads Copied! (title, the check icon, .share-copied) for 1.5 s, then
     Copy link again
  J. the game announcement card with a ranked-queue action (v10:116880-117013): the node's console
     `announce ranked-queue <label>` shows the card to a joining player; the action starts the search
     (the button reads In Queue while it runs); an account under level 10 sees Requires Level 10, disabled

Needs a headless GL device (KRUNKNATIVE_HEADLESS=1: EGL) and python websockets. Exit 77 when the
database or websockets is unavailable.
"""
import asyncio
import json
import os
import re
import shutil
import signal
import sqlite3
import socket
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.join(os.getcwd(), "server/tests"))
import kn_proto  # noqa: E402

try:
    import websockets
except ImportError:  # pragma: no cover
    print("ranked_menu_e2e: python websockets missing: skipped")
    sys.exit(77)

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
ROOT = os.getcwd()
DRIVER = os.environ.get("KN_TEST_DB_DRIVER", "sqlite")
NAME = "kn_test_rkmenu_%d" % os.getpid()
SECRET = "kn-rkmenu-%d" % os.getpid()
GAME_PORT = kn_proto.free_ports(1)[0]
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-rkmenu-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
STAMP = os.getpid() % 100000
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


def sql(dbcfg, text, *args):
    """direct database access for the test setup only (the player's level): sqlite"""
    con = sqlite3.connect(dbcfg["path"], timeout=30)
    try:
        con.execute(text.replace("$1", "?").replace("$2", "?"), args)
        con.commit()
    finally:
        con.close()



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
        r = await s.call("a", "a", 0, [name, "pw-rkmenu-%d" % os.getpid(), 0], "captcha")
        assert r[1] is None, r
        return {"id": r[2], "name": name, "token": r[5]}


SCRIPT = """0 menu
12000 nodes @idle .rankedMenuModal
13100 call openRankedChallenges
15000 nodes @chal [id="challRankedHeadr"]
15050 rect challRankedHeadr menuWindow
15100 scroll menuWindow
15500 call openRankedMenu
17000 nodes @rk .rankedMenuModal .start-button
20000 call knRkStart
22000 text @err .overlay-title
22050 text @errtext .overlay-description
22100 nodes @still .rankedMenuModal .start-button
22200 text @banner .ranked-matchmaking-status
23500 quit
"""

# the queue answers late (QUEUE_DELAY_S, behind delayed_relay): v10 keeps the state Idle, LOADING... on the button and
# no banner until the QUEUED frame (v10 handleQueueStatus, v10:98459); then the search starts
QUEUE_DELAY_S = 3.0
SLOW_SCRIPT = """0 menu
12000 nodes @idle .rankedMenuModal
13000 call openRankedMenu
15000 nodes @rk .rankedMenuModal .start-button
16000 call knRkStart
17000 nodes @early .ranked-matchmaking-status.show
17100 text @early_btn .start-button
17200 text @early_foot .footer .status-text
21000 nodes @late .ranked-matchmaking-status.show
21100 text @late_foot .footer .status-text
22000 quit
"""


# I: the Shop's item (its shop row is ITEM) opens its view; Share copies the link and the button reads Copied!
# for 1.5 s (v10's ShareButton: copied = true, setTimeout 1500), then Copy link again
SHARE_SCRIPT = """0 menu
12000 call openRankedMenu
13000 call knRkTab 4
15000 call knRkItem ITEM
16000 nodes @view .item-showcase-container
16050 attr @before title .item-showcase-btns .share-btn
16100 clickid knRkShareBtn
16300 attr @copied title .item-showcase-btns .share-btn
16350 text @copied_text .item-showcase-btns .share-btn
16400 nodes @copied_cls .item-showcase-btns .share-btn.share-copied
18000 attr @later title .item-showcase-btns .share-btn
18050 text @later_text .item-showcase-btns .share-btn
18100 nodes @later_cls .item-showcase-btns .share-btn.share-copied
19000 quit
"""

# J: the card of the node's `announce ranked-queue` (the account is level 20): Join Ranked, the action starts the
# search (In Queue, the banner shows); nothing happens before the click
ANN_SCRIPT = """0 menu
8000 nodes @shown .game-announcement.show
8050 text @label .game-announcement .ga-action-btn
8100 nodes @before .ranked-matchmaking-status.show
8200 clicksel .game-announcement .ga-action-btn
9500 text @queued .game-announcement .ga-action-btn
9600 nodes @banner .ranked-matchmaking-status.show
9700 quit
"""

# J: an account under level 10 (v10:116880, $account.level < 10): Requires Level 10, disabled, the click does nothing
LOW_SCRIPT = """0 menu
8000 nodes @shown .game-announcement.show
8050 text @label .game-announcement .ga-action-btn
8100 nodes @disabled .game-announcement .ga-action-btn.disabled
8200 clicksel .game-announcement .ga-action-btn
9000 nodes @banner .ranked-matchmaking-status.show
9100 quit
"""


def delayed_relay(upstream_port, relay_port):
    """the brain behind a byte relay: a ranked queue request (its first bytes) is held for QUEUE_DELAY_S before it is
    sent on; every other byte passes at once (the login's WebSocket included). Returns the listening socket: close()"""
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", relay_port))
    srv.listen(32)

    def pump(src, dst, held):
        try:
            while True:
                data = src.recv(65536)
                if not data:
                    break
                if held and data.startswith(b"GET /mm/ranked/queue"):
                    time.sleep(QUEUE_DELAY_S)
                dst.sendall(data)
        except OSError:
            pass
        finally:
            try:
                dst.shutdown(socket.SHUT_WR)
            except OSError:
                pass

    def accept_loop():
        while True:
            try:
                client, _ = srv.accept()
            except OSError:
                return
            up = socket.create_connection(("127.0.0.1", upstream_port))
            threading.Thread(target=pump, args=(client, up, True), daemon=True).start()
            threading.Thread(target=pump, args=(up, client, False), daemon=True).start()

    threading.Thread(target=accept_loop, daemon=True).start()
    return srv


def node(brain_port, node_id, game_port):
    """a server_bin node linked to the brain: the public lobby the client starts in"""
    log = os.path.join(WORK, node_id + ".log")
    gs = os.path.join(WORK, node_id + ".json")
    cfg = {"node_id": node_id, "name": "ranked menu e2e " + node_id, "region": "local", "listen_port": game_port,
           "public_host": "127.0.0.1", "brain_url": "ws://127.0.0.1:%d/mm/internal/ws" % brain_port,
           "brain_secret": SECRET, "modes": ["ffa"], "max_players": 8, "shutdown_grace": 0, "custom_games": True,
           "idle_kick_seconds": 0, "finder_url": "http://127.0.0.1:%d" % brain_port}
    with open(gs, "w") as f:
        json.dump({"gameserver": cfg}, f)
    p = subprocess.Popen([f"{BUILD}/server_bin", "--headless", "--config", gs, "--cfg", "none"], cwd=ROOT,
                         stdout=open(log, "w"), stderr=subprocess.STDOUT, stdin=subprocess.PIPE, text=True,
                         start_new_session=True)  # p.stdin: the node's console (announce)
    procs.append(p)
    return p, log


def start_client(name, brain_port, storage, script=None):
    path = os.path.join(WORK, name + ".script")
    with open(path, "w") as f:
        f.write(script or SCRIPT)
    cfg = os.path.join(WORK, name + "-cfg")
    os.makedirs(os.path.join(cfg, "krunknative"), exist_ok=True)
    for key, value in storage.items():
        with open(os.path.join(cfg, "krunknative", key + ".json"), "w") as f:
            f.write(value)
    log = os.path.join(WORK, name + ".log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": cfg,
                "XDG_STATE_HOME": os.path.join(WORK, name + "-state"), "KRUNKNATIVE_NO_BROWSER": "1",
                "KRUNKNATIVE_INPUT_SCRIPT": path, "KRUNKNATIVE_WINDOW_SIZE": "1280x720",
                "KRUNKNATIVE_BRAIN_URL": "http://127.0.0.1:%d" % brain_port, "KRUNKNATIVE_NET_TRANSPORT": "tcp",
                "KRUNKNATIVE_FINDER_URL": "http://127.0.0.1:%d/servers" % brain_port, "KRUNKNATIVE_UPDATES_URL": "off"})
    p = subprocess.Popen(["stdbuf", "-oL", "-eL", f"{BUILD}/client_bin"], cwd=ROOT, stdout=open(log, "w"),
                         stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, start_new_session=True)
    procs.append(p)
    return p, log


def nodes(text, label):
    m = re.search(r"\[input_script\] nodes t=\d+ @%s elements=\d+ match=(\d+)" % re.escape(label), text)
    return int(m.group(1)) if m else -1


def text_of(text, label):
    m = re.search(r"\[input_script\] text t=\d+ @%s n=(\d+) checked=\d+ value=(.*?) text=(.*)" % re.escape(label), text)
    return (int(m.group(1)), m.group(3).strip()) if m else (-1, None)


def rect_of(text, element):
    m = re.search(r"\[input_script\] rect t=\d+ (.*)", text)
    if not m:
        return None
    for part in m.group(1).split():
        k, _, v = part.partition("=")
        if k == element and v != "-":
            return [float(x) for x in v.split(",")[:4]]
    return None


def scroll_of(text, element):
    m = re.search(r"\[input_script\] scroll t=\d+ (.*)", text)
    if not m:
        return None
    for part in m.group(1).split():
        k, _, v = part.partition("=")
        if k == element and v != "-":
            return float(v)
    return None


def attr_of(text, label, name):
    """[input_script] attr t=N @label n=K name=value (the first match's attribute, "-" when absent)"""
    m = re.search(r"\[input_script\] attr t=\d+ @%s n=(\d+) %s=(.*)" % (re.escape(label), re.escape(name)), text)
    return (int(m.group(1)), m.group(2).strip()) if m else (-1, None)


def run_client(name, port, storage, script):
    """one headless client on the brain at port, its script run to the end; returns its log"""
    p, log = start_client(name, port, storage, script=script)
    try:
        p.wait(120)
    except subprocess.TimeoutExpired:
        os.killpg(p.pid, signal.SIGTERM)
        p.wait(10)
    return read_log(log)


def home_map_cards(build):
    """H: the Home tab's carousel from a scenario (no brain). The scenario's season maps say one map is
    unavailable; v10 still lists it as a plain card (its carousel: _38145-_38147, v10:111965-111969)."""
    with open(os.path.join(ROOT, "tools", "uidiff", "scenarios", "ranked-home.json"), encoding="utf-8") as f:
        scenario = json.load(f)
    profile = scenario["state"]["ranked"]["profile"]
    profile["maps"] = [{"id": 2, "available": True}, {"id": 4, "available": True}, {"id": 11, "available": False},
                       {"id": 14, "available": True}, {"id": 17, "available": True}]
    work = tempfile.mkdtemp(prefix="kn-rkhome-", dir=CACHE if os.path.isdir(CACHE) else None)
    scn, out, html = (os.path.join(work, n) for n in ("scenario.json", "home.png", "home.html"))
    with open(scn, "w", encoding="utf-8") as f:
        json.dump(scenario, f)
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": os.path.join(work, "cfg"),
                "KRUNKNATIVE_SCENARIO_HTML": html})
    r = subprocess.run([f"{BUILD}/client_bin", "--render-scenario", scn, out], cwd=ROOT, env=env, capture_output=True, text=True,
                       timeout=240)
    doc = read_log(html) if os.path.exists(html) else ""
    check("the Home scenario renders the popup's document", r.returncode == 0 and "map-carousel-container" in doc,
          (r.stdout + r.stderr)[-300:])
    cards = re.findall(r'<div class="map-card[^"]*"([^>]*)><img class="map-image[^"]*"([^>]*)>', doc)
    check("the carousel lists every pool map twice (10 cards, v10 [...maps, ...maps])", len(cards) == 10, str(len(cards)))
    greyed = [c for c in cards if "style" in c[0]]
    check("no map card is greyed, even the one the brain marks unavailable (v10 has no greying)", not greyed,
          "%d greyed" % len(greyed))
    labelled = [c for c in cards if "alt=" in c[1]]
    check("the map images carry no alt text (v10's markup: a broken image shows nothing)", not labelled, "%d with alt" % len(labelled))
    if all(results):
        shutil.rmtree(work, ignore_errors=True)
    else:
        print("logs kept in " + work)


def main():
    if not os.access(f"{BUILD}/client_bin", os.X_OK):
        print("ranked_menu_e2e: no client_bin in %s: skipped" % BUILD)
        return 77
    home_map_cards(BUILD)
    scratch = [sys.executable, "-I", os.path.join(ROOT, "services", "brain", "tests", "db_scratch.py"), ROOT]
    r = subprocess.run(scratch + ["create", DRIVER, NAME], capture_output=True)
    if r.returncode:
        print("ranked_menu_e2e: %s unavailable: skipped" % DRIVER)
        return r.returncode
    dbcfg = json.loads(r.stdout)
    dbenv = {"KN_DATABASE_" + k.upper(): str(v) for k, v in dbcfg.items()}
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=SECRET, **dbenv)
    brain_log = os.path.join(WORK, "brain.log")
    brain = None
    try:
        subprocess.run([f"{BUILD}/services/brain/kn_brain", "--migrate-only"], cwd=ROOT, env=env, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        brain = subprocess.Popen([f"{BUILD}/services/brain/kn_brain", "--brain.host=127.0.0.1", "--brain.port=0",
                                  "--accounts.signup_rewards.enabled=false", "--brain.region=local",
                                  "--brain.id=rkmenu-%d" % os.getpid(), "--finder.poll_interval_s=1",
                                  "--brain.ranked.min_level=0", "--brain.ranked.team_size=1", "--brain.ranked.start_grace_s=60",
                                  "--accounts.starting_kr=500"],
                                 cwd=ROOT, env=env, stdout=open(brain_log, "w"), stderr=subprocess.STDOUT,
                                 stdin=subprocess.DEVNULL, start_new_session=True)
        procs.append(brain)
        m = wait_for(lambda: re.search(r"listening on http://127\.0\.0\.1:(\d+) \(ws", read_log(brain_log)), 15)
        check("kn_brain starts", bool(m), read_log(brain_log)[-300:])
        if not m:
            return 1
        port = int(m.group(1))
        node_proc, node_log = node(port, "RKM", GAME_PORT)
        check("the game node links to the brain", bool(wait_for(lambda: "welcomed" in read_log(node_log), 20)), read_log(node_log)[-300:])
        acc = asyncio.run(register(port, "rkmenu%d" % STAMP))
        # level 20 (the popup's own level check, v10:111566-111580): the player_score of that level
        sql(dbcfg, "UPDATE players SET player_score = $1 WHERE player_id = $2", int((20 / 0.03) ** 2) + 1, acc["id"])
        check("a level-20 account registered", bool(acc["token"]), str(acc))
        low = asyncio.run(register(port, "rkmlow%d" % STAMP))
        check("a level-0 account registered", bool(low["token"]), str(low))
        login = {"krunker_username": acc["name"], "krunker_token": acc["token"]}

        # I: a Shop item (skin 55, Blaze LMG, in the store table) for the item view's Share
        con = sqlite3.connect(dbcfg["path"], timeout=30)
        con.execute("INSERT INTO ranked_shop (skin_id, price, max_purchase) VALUES (55, 100, 1)")
        con.commit()
        item = con.execute("SELECT id FROM ranked_shop WHERE skin_id = 55").fetchone()[0]
        con.close()
        # J: the node's console sets the ranked-queue card; it goes to each player who joins from now on
        node_proc.stdin.write("announce ranked-queue Join Ranked\n")
        node_proc.stdin.flush()
        check("the node's console sets the ranked-queue card", bool(wait_for(lambda: "Ranked queue action" in read_log(node_log), 10)),
              read_log(node_log)[-200:])

        stext = run_client("share", port, login, SHARE_SCRIPT.replace("ITEM", str(item)))
        before = attr_of(stext, "before", "title")[1]
        check("the item view opens from the Shop (v10 item view, its Share button)", nodes(stext, "view") == 1, str(nodes(stext, "view")))
        check("before a copy the Share button reads Copy link (v10 ShareButton title)", before == "Copy link", repr(before))
        copied = attr_of(stext, "copied", "title")[1]
        check("after Share the button's title is Copied! (v10 ShareButton, v10:103485-103520)", copied == "Copied!", repr(copied))
        ctext = text_of(stext, "copied_text")[1] or ""
        check("after Share the button reads Copied! with the check icon (v10)", "Copied!" in ctext and "check" in ctext, repr(ctext)[:80])
        check("after Share the button has .share-copied (v10's green state)", nodes(stext, "copied_cls") == 1, str(nodes(stext, "copied_cls")))
        later = attr_of(stext, "later", "title")[1]
        check("1.5 s after Share the button reads Copy link again (v10's setTimeout 1500)", later == "Copy link", repr(later))
        check("1.5 s after Share the state is gone (v10 removes .share-copied)", nodes(stext, "later_cls") == 0, str(nodes(stext, "later_cls")))

        ann = run_client("announce", port, login, ANN_SCRIPT)
        check("the ranked-queue card shows in the joining client (v10 .game-announcement.show)", nodes(ann, "shown") == 1, str(nodes(ann, "shown")))
        label = text_of(ann, "label")[1] or ""
        check("the card's action reads the server's label before a search", label == "Join Ranked", repr(label)[:80])
        check("nothing is queued before the action is clicked", nodes(ann, "before") == 0, str(nodes(ann, "before")))
        queued = text_of(ann, "queued")[1] or ""
        check("the action starts the ranked queue: the button reads In Queue (v10 inRankedQueue)", queued == "In Queue", repr(queued))
        check("the action starts the ranked queue: the search banner shows", nodes(ann, "banner") == 1, str(nodes(ann, "banner")))

        lows = run_client("lowlevel", port, {"krunker_username": low["name"], "krunker_token": low["token"]}, LOW_SCRIPT)
        ltext = text_of(lows, "label")[1] or ""
        check("an account under level 10 sees Requires Level 10 (v10 announcement action label)", ltext == "Requires Level 10", repr(ltext))
        check("an account under level 10: the action is disabled (v10 .disabled)", nodes(lows, "disabled") == 1, str(nodes(lows, "disabled")))
        check("an account under level 10: the action queues nothing", nodes(lows, "banner") == 0, str(nodes(lows, "banner")))

        # FIND MATCH with a slow matchmaker (the brain behind a relay that holds the queue's answer)
        relay_port = kn_proto.free_ports(1)[0]
        relay = delayed_relay(port, relay_port)
        s, slog = start_client("slow", relay_port, {"krunker_username": acc["name"], "krunker_token": acc["token"]},
                               script=SLOW_SCRIPT)
        try:
            s.wait(120)
        except subprocess.TimeoutExpired:
            os.killpg(s.pid, signal.SIGTERM)
            s.wait(10)
        relay.close()
        stext = read_log(slog)
        early_btn = text_of(stext, "early_btn")[1] or ""
        early_foot = text_of(stext, "early_foot")[1] or ""
        check("before QUEUED the start button reads LOADING... (v10 keeps the state Idle while it connects, v10:98420)",
              early_btn == "LOADING...", repr(early_btn)[:80])
        check("before QUEUED no search has started: no banner (v10 updateGlobalStatus only while Searching)",
              nodes(stext, "early") == 0, str(nodes(stext, "early")))
        check("before QUEUED the footer keeps v10's Ready text (no Looking for match yet)",
              early_foot == "Ready to compete? Join the ranked queue!", repr(early_foot)[:80])
        check("after QUEUED the banner shows the search (v10 handleQueueStatus QUEUED, v10:98459)",
              nodes(stext, "late") == 1, str(nodes(stext, "late")))
        check("after QUEUED the footer reads Looking for match...",
              "Looking for match" in (text_of(stext, "late_foot")[1] or ""), repr(text_of(stext, "late_foot")[1])[:80])

        p, log = start_client("player", port, {"krunker_username": acc["name"], "krunker_token": acc["token"]})
        # the harness takes the brain away once the popup is up: FIND MATCH cannot reach the queue
        up = wait_for(lambda: re.search(r"@rk elements=\d+ match=1", read_log(log)), 90)
        check("the Ranked popup is up before the brain goes away", bool(up), read_log(log)[-300:])
        if brain.poll() is None:
            os.killpg(brain.pid, signal.SIGKILL)
            brain.wait()
        try:
            p.wait(120)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGTERM)
            p.wait(10)
            check("client finished its script", False, "timed out")
        text = read_log(log)

        check("a plain home load opens no Ranked popup (the rejoin check finds no match, v10:209847)", nodes(text, "idle") == 0,
              str(nodes(text, "idle")))
        check("Ranked Challenges opens the Challenges window with its Ranked block", nodes(text, "chal") == 1, str(nodes(text, "chal")))
        mw = rect_of(text, "menuWindow")
        hd = rect_of(text, "challRankedHeadr")
        sc = scroll_of(text, "menuWindow")
        visible = bool(mw and hd and mw[1] <= hd[1] < mw[1] + mw[3])
        check("the Challenges window is scrolled to the Ranked header (it is inside the window)", visible and sc is not None and sc > 0,
              "window %s header %s scroll %s" % (mw, hd, sc))
        check("Ranked opens v10's popup with its start button", nodes(text, "rk") == 1, str(nodes(text, "rk")))
        title = text_of(text, "err")[1]
        check("FIND MATCH with no matchmaking server: the Internal Error popup (v10:98443-98450, 37958)", title == "Internal Error",
              "title: %r" % title)
        desc = text_of(text, "errtext")[1]
        check("the Internal Error text is v10's", desc == "An internal error occurred. Please try again later.", repr(desc))
        check("the Ranked popup stays up behind the error", nodes(text, "still") == 1, str(nodes(text, "still")))
        banner = text_of(text, "banner")[1] or ""
        check("an unreachable queue keeps v10's banner text (v10 socket onerror: Failed to connect to matchmaking server)",
              banner == "Failed to connect to matchmaking server", repr(banner)[:120])
    finally:
        for q in procs:
            if q.poll() is None:
                os.killpg(q.pid, signal.SIGTERM)
        for q in procs:
            try:
                q.wait(10)
            except subprocess.TimeoutExpired:
                os.killpg(q.pid, signal.SIGKILL)
                q.wait()
        subprocess.run(scratch + ["drop", DRIVER, NAME], capture_output=True)
    if all(results):
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print("logs kept in " + WORK)
    print(f"{sum(results)}/{len(results)} passed")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
