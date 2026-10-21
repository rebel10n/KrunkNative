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
  3. the brain is killed by this harness while the popup is up; FIND MATCH then fails to reach the
     matchmaking server: v10 shows the "Internal Error" popup (v10:98443-98450, 37958), not a line

  H. the Home tab's map carousel is v10's markup (ranked_ui.c gen_home, v10:111965-111969): every pool map
     is a plain .map-card with an unlabelled .map-image, none greyed, even one the brain marks unavailable.
     A --render-scenario run of tools/uidiff/scenarios/ranked-home.json (no brain) dumps the document
     (KRUNKNATIVE_SCENARIO_HTML) and checks it.

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
import subprocess
import sys
import tempfile
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
23500 quit
"""


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
                         stdout=open(log, "w"), stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                         start_new_session=True)
    procs.append(p)
    return p, log


def start_client(name, brain_port, storage):
    path = os.path.join(WORK, name + ".script")
    with open(path, "w") as f:
        f.write(SCRIPT)
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
        _, node_log = node(port, "RKM", GAME_PORT)
        check("the game node links to the brain", bool(wait_for(lambda: "welcomed" in read_log(node_log), 20)), read_log(node_log)[-300:])
        acc = asyncio.run(register(port, "rkmenu%d" % STAMP))
        # level 20 (the popup's own level check, v10:111566-111580): the player_score of that level
        sql(dbcfg, "UPDATE players SET player_score = $1 WHERE player_id = $2", int((20 / 0.03) ** 2) + 1, acc["id"])
        check("a level-20 account registered", bool(acc["token"]), str(acc))

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
