#!/usr/bin/env python3
"""Krunker v10.0.2 What's New, Guide and Community & Events end to end (client/src/ui/v10_news.c,
services/brain/community.c) on the v10 page (ui/v10) against a real kn_brain and a brain-linked
server_bin node.

  python3 -I server/tests/v10_news_e2e.py BUILD_DIR      (run from the repository root)

kn_brain on a scratch database (KN_TEST_DB_DRIVER, default sqlite: no server needed) with its
community feed file (brain.community.feed) and one staff-featured published map, one v10 server_bin
node, every port from kn_proto.free_ports().

  1. brain: GET /creators/events serves the feed's live and upcoming events soonest first (an
     ended one left out) and the featured map as a "Featured Map" card with Play / Details links;
     its polls with ends_in (closed and link-less ones left out); GET /social/news escapes the
     feed's text and keeps only http(s) links; an edited feed is served without a restart
  2. one headless client_bin, signed in (saved krunker_username / krunker_token), clicking the
     v10 menu like a player:
       - the What's New item shows with " - v<version>"; the small Guide item (client/src/ui/v10_menu.c's
         signed-in nav, level <= 20) opens How to Krunker (12 sections,
         the four level-locked ones marked, the Basics body); a sidebar click switches section
       - Community & Events opens the popup: the 3 event cards (one LIVE), the poll, the socials;
         an event link, a social card and the poll's Vote link open their URLs (logged:
         KRUNKNATIVE_NO_BROWSER); the event's /?play=Kanji link plays Kanji (playMap)
       - What's New opens windows[34]: "Whats New in v<version> ?" with the brain's news cards;
         View All Updates saves lastViewedUpdate, lists every update and hides the menu item
  3. a guest: the big Guide card opens the guide, Community & Events its events; no What's New item
Frames of each popup are written next to the logs (kept on failure).

Needs a headless GL device (KRUNKNATIVE_HEADLESS=1: EGL). Exit 77 when the database is unavailable.
"""
import datetime
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
import urllib.request

sys.path.insert(0, os.path.join(os.getcwd(), "server/tests"))
import kn_proto  # noqa: E402

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
ROOT = os.getcwd()
DRIVER = os.environ.get("KN_TEST_DB_DRIVER", "sqlite")
NAME = "kn_test_v10news_%d" % os.getpid()
SECRET = "kn-v10news-%d" % os.getpid()
(GAME_PORT,) = kn_proto.free_ports(1)
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-v10news-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
FEED = os.path.join(WORK, "community.json")
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


def iso(delta_hours):
    t = datetime.datetime.now(datetime.timezone.utc) + datetime.timedelta(hours=delta_hours)
    return t.strftime("%Y-%m-%dT%H:%M:%SZ")


def write_feed(extra_news=()):
    feed = {
        "news": [{"text": "<b>Season</b> & welcome", "url": "https://krunk.io/", "label": "Play now"},
                 {"text": "No script here", "url": "javascript:alert(1)"}, *extra_news],
        "events": [
            {"id": 1, "player_name": "E2E Host", "title": "Upcoming Cup", "description": "A cup in two days.",
             "start_date": iso(48), "duration_hours": 2, "ended_at": None,
             "links": [{"label": "Join the Discord", "url": "https://discord.gg/krunker"},
                       {"label": "Play Kanji", "url": "/?play=Kanji"}], "tags": ["Tournament", "Stream"]},
            {"id": 2, "player_name": "E2E Host", "title": "Live Stream Now", "description": "On air.",
             "start_date": iso(-1), "duration_hours": 3, "ended_at": None,
             "links": [{"label": "Watch", "url": "https://www.twitch.tv/directory/game/Krunker"}], "tags": ["Stream"]},
            {"id": 3, "player_name": "E2E Host", "title": "Old Event", "description": "Over.",
             "start_date": iso(-30), "duration_hours": 2, "ended_at": None, "links": [], "tags": []},
        ],
        "polls": [
            {"id": 7, "title": "Next ranked map?", "description": "Pick one.", "ends_at": iso(72),
             "url": "https://discord.gg/krunker-poll"},
            {"id": 8, "title": "Closed poll", "ends_at": iso(-2), "url": "https://example.com/p/8"},
            {"id": 9, "title": "No link", "ends_at": iso(72)},
        ],
    }
    with open(FEED, "w") as f:
        json.dump(feed, f)


def node(brain_port):
    log = os.path.join(WORK, "node.log")
    gs = os.path.join(WORK, "node.json")
    cfg = {"node_id": "NWS", "name": "v10 news e2e", "region": "local", "listen_port": GAME_PORT,
           "public_host": "127.0.0.1", "brain_url": "ws://127.0.0.1:%d/mm/internal/ws" % brain_port,
           "brain_secret": SECRET, "modes": ["ffa"], "max_players": 8, "shutdown_grace": 0,
           "idle_kick_seconds": 0, "ruleset": "v10"}
    with open(gs, "w") as f:
        json.dump({"gameserver": cfg}, f)
    p = subprocess.Popen([f"{BUILD}/server_bin", "--headless", "--config", gs, "--cfg", "none"], cwd=ROOT,
                         stdout=open(log, "w"), stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                         start_new_session=True)
    procs.append(p)
    return p


def client(name, brain_port, script, storage, secs=150):
    """One headless client_bin on the v10 page, run to its script's quit; returns (log, config dir)."""
    path = os.path.join(WORK, name + ".script")
    with open(path, "w") as f:
        f.write(script)
    cfg = os.path.join(WORK, name + "-cfg")
    os.makedirs(os.path.join(cfg, "krunknative"), exist_ok=True)
    for key, value in dict({"knRuleset": "v10"}, **storage).items():
        with open(os.path.join(cfg, "krunknative", key + ".json"), "w") as f:
            f.write(value)
    log = os.path.join(WORK, name + ".log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": cfg,
                "XDG_STATE_HOME": os.path.join(WORK, name + "-state"), "KRUNKNATIVE_NO_BROWSER": "1",
                "KRUNKNATIVE_INPUT_SCRIPT": path, "KRUNKNATIVE_WINDOW_SIZE": "1280x720",
                "KRUNKNATIVE_UI_DIR": "ui/v10", "KRUNKNATIVE_BRAIN_URL": "http://127.0.0.1:%d" % brain_port,
                "KRUNKNATIVE_NET_TRANSPORT": "tcp", "KRUNKNATIVE_UPDATES_URL": "off"})
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
    return read_log(log), cfg


def vis(text, label):
    """The `vis @label a=1 b=0` line of an input script log as {id: value}"""
    m = re.search(r"\[input_script\] vis t=\d+ @%s (.*)" % re.escape(label), text)
    return dict(kv.split("=", 1) for kv in m.group(1).split()) if m else {}


def nodes(text, label):
    """The match count of the `nodes @label <selector>` line, -1 without one"""
    m = re.search(r"\[input_script\] nodes t=\d+ @%s elements=\d+ match=(\d+)" % re.escape(label), text)
    return int(m.group(1)) if m else -1


SCRIPT = """0 menu
1500 vis @menu updateAd updateAdVersion menuBtnGuide knV10GuideItem menuBtnSideCommunity
1600 clickid knV10GuideItem
2400 vis @guide genericPop
2400 nodes @guidenav .tutorialPopup .tutorialNavItem
2400 nodes @guidelocked .tutorialNavItem.locked
2400 nodes @guideactive .tutorialNavItem.active
2400 nodes @basics .tutorialSectionBody .tutorialSectionControls
2410 frames {shots}/guide 1
2500 clicksel .tutorialNavItem
3200 nodes @chmode .challengeModeImage
3210 frames {shots}/guide-chmode 1
3300 call clearPops
3600 clickid menuBtnSideCommunity
6000 vis @community genericPop
6000 nodes @events .eventCard
6000 nodes @live .eventLive
6000 nodes @polls .pollCard
6000 nodes @tags .eventTagChip
6000 nodes @socials .socialCard
6000 nodes @empty .eventsEmpty
6010 frames {shots}/community 1
6100 clicksel [data-kn-url="https://discord.gg/krunker"]
6400 clicksel .socialCard
6700 clicksel .pollVote
7000 call clearPops
7300 clicksel [id=updateAd] .menuItem
8800 nodes @whatsnew .logVersionHed
8800 nodes @news .news-item
8810 frames {shots}/whatsnew 1
8850 setscroll menuWindow 99999
8900 clicksel .viewAllUpdates
9600 nodes @all .mailObj
9600 nodes @tabs .bigMenTab
9600 vis @adgone updateAd
9610 frames {shots}/updates 1
9700 call closWind
10000 clickid menuBtnSideCommunity
12000 clicksel [data-kn-url="/?play=Kanji"]
15000 quit
"""


GUEST_SCRIPT = """0 menu
1500 vis @menu updateAd menuBtnGuide knV10GuideItem menuBtnSideCommunity
1600 clickid menuBtnGuide
2400 vis @guide genericPop
2400 nodes @guidelocked .tutorialNavItem.locked
2500 call clearPops
2800 clickid menuBtnSideCommunity
5000 vis @community genericPop
5000 nodes @events .eventCard
5100 quit
"""


def guest_case(port):
    text, _ = client("guest", port, GUEST_SCRIPT, {})
    m = vis(text, "menu")
    check("3: guest: the Guide card, Community & Events, no What's New and no small Guide item",
          m.get("menuBtnGuide") == "1" and m.get("menuBtnSideCommunity") == "1" and m.get("updateAd") == "0"
          and m.get("knV10GuideItem") in ("0", "-"), str(m))
    check("3: guest: the Guide card opens How to Krunker (4 sections locked)", vis(text, "guide").get("genericPop") == "1"
          and nodes(text, "guidelocked") == 4, str(vis(text, "guide")))
    check("3: guest: Community & Events opens with the brain's 3 events", vis(text, "community").get("genericPop") == "1"
          and nodes(text, "events") == 3, str(nodes(text, "events")))


def brain_case(port, version):
    code, body = http(port, "/creators/events")
    data = body.get("data", {}) if isinstance(body, dict) else {}
    ev = data.get("events", [])
    titles = [e.get("title") for e in ev]
    check("1: /creators/events: live + upcoming feed events soonest first, the ended one left out, then the featured map",
          code == 200 and titles == ["Live Stream Now", "Upcoming Cup", "Featured E2E"], str(titles))
    feat = ev[-1] if ev else {}
    check("1: the featured map card: tag, creator, Play / Details links, no LIVE (ended_at = its date)",
          feat.get("tags") == ["Featured Map"] and feat.get("player_name") == "Community" and feat.get("ended_at")
          and [link.get("url") for link in feat.get("links", [])] == ["/?play=Featured%20E2E", "/social.html?p=map&q=Featured%20E2E"],
          json.dumps(feat)[:300])
    up = ev[1] if len(ev) > 1 else {}
    check("1: event fields in v10's shape", up.get("start_date", "").endswith("Z") and up.get("duration_hours") == 2
          and up.get("links", [{}])[0].get("label") == "Join the Discord" and up.get("tags") == ["Tournament", "Stream"],
          json.dumps(up)[:300])
    polls = data.get("polls", [])
    check("1: polls: the open one with ends_in (closed / link-less ones left out)",
          len(polls) == 1 and polls[0].get("id") == 7 and 0 < polls[0].get("ends_in", 0) <= 72 * 3600
          and polls[0].get("url") == "https://discord.gg/krunker-poll", json.dumps(polls)[:300])
    code, body = http(port, "/social/news")
    news = body.get("data", {}).get("news", []) if isinstance(body, dict) else []
    check("1: /social/news: escaped text, the http(s) link kept, the javascript: one dropped",
          code == 200 and news == ['&lt;b&gt;Season&lt;/b&gt; &amp; welcome <a target="_blank" href="https://krunk.io/">Play now</a>',
                                   "No script here"], str(news))
    time.sleep(1.1)  # a new mtime second
    write_feed(extra_news=["Edited feed"])
    news2 = wait_for(lambda: (lambda n: n if len(n) == 3 else None)(http(port, "/social/news")[1]["data"]["news"]), 5) or []
    check("1: an edited feed is served without a restart", news2[-1:] == ["Edited feed"], str(news2))
    return len(news2)


def client_case(port, user, token, version, news_count):
    text, cfg = client("player", port, SCRIPT.format(shots=WORK), {"krunker_username": user, "krunker_token": token})
    m = vis(text, "menu")
    check("2: signed in: the small Guide item (v10-menu, level <= 20), Community & Events and (not viewed) What's New - v" + version,
          m.get("knV10GuideItem") == "1" and m.get("menuBtnGuide") == "0" and m.get("menuBtnSideCommunity") == "1"
          and m.get("updateAd") == "1" and m.get("updateAdVersion") == "1", str(m))
    check("2: Guide opens How to Krunker", vis(text, "guide").get("genericPop") == "1" and "handler openGuide()" in text,
          str(vis(text, "guide")))
    check("2: its 12 sections, 4 level-locked for a new account, Basics active",
          nodes(text, "guidenav") == 12 and nodes(text, "guidelocked") == 4 and nodes(text, "guideactive") == 1,
          "%d %d %d" % (nodes(text, "guidenav"), nodes(text, "guidelocked"), nodes(text, "guideactive")))
    check("2: the Basics body (video + controls)", nodes(text, "basics") == 1, str(nodes(text, "basics")))
    check("2: a sidebar click switches section (Challenge Mode: its images)",
          "knGuideSection('challenge-mode')" in text and nodes(text, "chmode") == 1, str(nodes(text, "chmode")))
    c = vis(text, "community")
    check("2: Community & Events opens its popup", c.get("genericPop") == "1" and "handler openCommunityMenu()" in text, str(c))
    check("2: the brain's events: 3 cards, one LIVE, 4 tags; the poll; no empty state",
          nodes(text, "events") == 3 and nodes(text, "live") == 1 and nodes(text, "tags") == 4
          and nodes(text, "polls") == 1 and nodes(text, "empty") == 0,
          "events %d live %d tags %d polls %d empty %d" % (nodes(text, "events"), nodes(text, "live"), nodes(text, "tags"),
                                                            nodes(text, "polls"), nodes(text, "empty")))
    check("2: the 5 socials", nodes(text, "socials") == 5, str(nodes(text, "socials")))
    for label, url in (("an event link", "https://discord.gg/krunker"), ("the YouTube card",
                       "https://www.youtube.com/channel/UCqH34FGuOae5o-wO2wOa38A"), ("the poll's Vote now", "https://discord.gg/krunker-poll")):
        check("2: %s opens %s" % (label, url), "community link " + url in text
              and "open (disabled by KRUNKNATIVE_NO_BROWSER): " + url in text, url)
    check("2: What's New opens windows[34] with the version header", nodes(text, "whatsnew") == 1
          and "handler showWindow(35)" in text, str(nodes(text, "whatsnew")))
    check("2: the brain's news as cards", nodes(text, "news") == news_count, "%d of %d" % (nodes(text, "news"), news_count))
    check("2: View All Updates: every update, the v1.x tab", nodes(text, "all") == 3 and nodes(text, "tabs") == 1,
          "%d %d" % (nodes(text, "all"), nodes(text, "tabs")))
    check("2: ... and the What's New item is gone", vis(text, "adgone").get("updateAd") == "0", str(vis(text, "adgone")))
    seen = read_log(os.path.join(cfg, "krunknative", "lastViewedUpdate.json"))
    check("2: lastViewedUpdate saved", seen == version, repr(seen))
    check("2: the event's /?play=Kanji link plays Kanji", "community link /?play=Kanji" in text
          and "handler playMap('Kanji')" in text, "")


def main():
    if not os.access(f"{BUILD}/client_bin", os.X_OK):
        print("v10_news_e2e: no client_bin in %s: skipped" % BUILD)
        return 77
    with open(os.path.join(ROOT, "ui", "v10", "windows", "kn-versions.txt")) as f:
        version = re.search(r"== (?:UPDATE|INITIAL RELEASE V?)\s*([\d.]+)", f.read()).group(1)
    scratch = [sys.executable, "-I", os.path.join(ROOT, "services", "brain", "tests", "db_scratch.py"), ROOT]
    r = subprocess.run(scratch + ["create", DRIVER, NAME], capture_output=True)
    if r.returncode:
        print("v10_news_e2e: %s unavailable: skipped" % DRIVER)
        return r.returncode
    dbcfg = json.loads(r.stdout)
    dbenv = {"KN_DATABASE_" + k.upper(): str(v) for k, v in dbcfg.items()}
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=SECRET, **dbenv)
    brain_log = os.path.join(WORK, "brain.log")
    write_feed()
    try:
        subprocess.run([f"{BUILD}/services/brain/kn_brain", "--migrate-only"], cwd=ROOT, env=env, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        sql = ("INSERT INTO published_maps (map_name, map_description, map_featured, map_data) "
               "VALUES ('Featured E2E', 'A staff pick.', 1, '{}')")
        if dbcfg.get("driver") == "sqlite":
            with sqlite3.connect(dbcfg["path"]) as db:
                db.execute(sql)
        else:
            penv = dict(os.environ, PGPASSWORD=dbcfg.get("password", ""))
            subprocess.run(["psql", "-h", dbcfg["host"], "-p", str(dbcfg["port"]), "-U", dbcfg["user"], "-d", dbcfg["name"],
                            "-v", "ON_ERROR_STOP=1", "-qAt", "-c", "SET search_path TO %s; %s" % (dbcfg["search_path"], sql)],
                           env=penv, check=True, capture_output=True)
        brain = subprocess.Popen([f"{BUILD}/services/brain/kn_brain", "--brain.host=127.0.0.1", "--brain.port=0",
                                  "--brain.region=local", "--brain.id=v10news-%d" % os.getpid(),
                                  "--brain.community.feed=" + FEED],
                                 cwd=ROOT, env=env, stdout=open(brain_log, "w"), stderr=subprocess.STDOUT,
                                 stdin=subprocess.DEVNULL, start_new_session=True)
        procs.append(brain)
        m = wait_for(lambda: re.search(r"listening on http://127\.0\.0\.1:(\d+) \(ws", read_log(brain_log)), 15)
        check("kn_brain starts", bool(m), read_log(brain_log)[-300:])
        if not m:
            return 1
        port = int(m.group(1))
        node(port)
        lobby = wait_for(lambda: [g[0] for g in http(port, "/mm/game-list?hostname=krunknative")[1].get("games", [])], 20)
        check("a v10 lobby listed", bool(lobby), str(lobby))
        news_count = brain_case(port, version)
        user = "v10n%d" % (os.getpid() % 100000)
        st, reg = http(port, "/auth/register", {"username": user, "password": "pw-v10news-%d" % os.getpid()})
        token = reg.get("token", "") if isinstance(reg, dict) else ""
        check("account registered", st == 200 and token, str(reg)[:200])
        if lobby and token:
            client_case(port, user, token, version, news_count)
        if lobby:
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
        print("logs and frames kept in " + WORK)
    print(f"{sum(results)}/{len(results)} passed")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
