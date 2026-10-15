#!/usr/bin/env python3
"""Community maps in the Games window (windows[9]) against a local kn_brain: browse, filter, host
pick, download and play (client/src/ui/community_maps.c, servers_ui.c; services/brain/creations.c).

  python3 -I server/tests/community_games_e2e.py BUILD_DIR      (run from the repository root)

kn_brain on a scratch database (KN_TEST_DB_DRIVER, default sqlite: no server needed; postgres uses
config/config.json) on an ephemeral port. An account publishes two maps over the social socket
(uploadM: the brain validates them like v10's editor): GOOD (Parkour, modes FFA + TDM) and BAD,
whose stored data is then made spawnless directly in the database (an old row that would not pass
today's validateMap). One headless client_bin (EGL, no window on any display) on the v10 page:

  1. Custom Games > New: GOOD's card, v10's NEW! badge, the mode tags (the thumbnail's title);
  2. Search (v10 searchMap(val, cat) -> /api/search): by name; Popular Categories' empty name
     with a category (Parkour lists GOOD only, Puzzle BAD only); KrunkNative's Official /
     Community options (the category select's last two): Official hides it and finds Burg,
     Community finds it;
  3. selectHostMap (the Host Game seam, community_pick): GOOD downloads and gets a map slot; BAD
     is refused (-1) with v10's "Missing spawn points.";
  4. playMap BAD: refused before any game starts; playMap GOOD: the offline game plays it.

Exit 77 when the database or python websockets is missing.
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
import time
import urllib.request

try:
    import websockets
except ImportError:  # pragma: no cover
    print("python websockets missing: skip")
    sys.exit(77)

BUILD = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug")
ROOT = os.getcwd()
DRIVER = os.environ.get("KN_TEST_DB_DRIVER", "sqlite")
NAME = "kn_test_cmgames_%d" % os.getpid()
WORK = tempfile.mkdtemp(prefix="kn-community-games-", dir=os.path.expanduser("~/.cache"))
CATEGORY_PARKOUR, CATEGORY_PUZZLE = 1, 2  # v10 config.mapCats
results = []


def check(name, ok, detail=""):
    results.append(bool(ok))
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail and not ok else ''}", flush=True)


def wait_for(path, pattern, secs=15.0):
    end = time.time() + secs
    while time.time() < end:
        with open(path, errors="replace") as f:
            m = re.search(pattern, f.read())
        if m:
            return m
        time.sleep(0.1)
    return None


def api(port, path):
    with urllib.request.urlopen("http://127.0.0.1:%d%s" % (port, path), timeout=20) as r:
        return json.loads(r.read())


def make_map(name):
    return json.dumps({"name": name, "objects": [{"p": [0, 0, 0], "s": [200, 2, 200]}],
                       "spawns": [[0, 2, 0, 0, 0, 0], [20, 2, 20, 0, 0, 0]], "modes": [0, 1]})


async def publish_maps(port, account, maps):
    """Registers the account and publishes each (name, category): the pubMR / upMp replies."""
    out = []
    async with websockets.connect("ws://127.0.0.1:%d/social/ws?enc=json" % port, max_size=4 << 20) as ws:
        async def call(frame, labels):
            await ws.send(json.dumps(frame))
            end = time.time() + 10
            while True:
                f = json.loads(await asyncio.wait_for(ws.recv(), end - time.time()))
                if f[0] in labels:
                    return f
        r = await call(["a", 0, [account, "cmgamespass1", None], None], ("a",))
        check("account registered", r[1] is None and r[5], str(r)[:200])
        out.append(r[5])  # the session token
        for name, cat in maps:
            out.append(await call(["uploadM", make_map(name), None, "e2e", "", cat, False, False], ("pubMR", "upMp")))
    return out


def break_map(dbcfg, name):
    """BAD's stored data loses its spawns (a row published before the brain validated maps)."""
    doc = json.loads(make_map(name))
    doc["spawns"] = []
    data = json.dumps(doc)
    if dbcfg.get("driver") == "sqlite":
        import sqlite3
        con = sqlite3.connect(dbcfg["path"])
        n = con.execute("UPDATE published_maps SET map_data = ? WHERE map_name = ?", (data, name)).rowcount
        con.commit()
        con.close()
        return n == 1
    if dbcfg.get("driver") == "postgres":
        env = dict(os.environ, PGPASSWORD=str(dbcfg.get("password", "")))
        sql = "SET search_path TO %s; UPDATE published_maps SET map_data = '%s' WHERE map_name = '%s'" % (
            dbcfg["search_path"], data.replace("'", "''"), name)
        r = subprocess.run(["psql", "-h", str(dbcfg["host"]), "-p", str(dbcfg["port"]), "-U", str(dbcfg["user"]), "-d",
                            str(dbcfg["name"]), "-v", "ON_ERROR_STOP=1", "-qAt", "-c", sql], env=env, capture_output=True, timeout=30)
        return r.returncode == 0
    return False


def read(path):
    try:
        with open(path, errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def script_line(text, kind, label):
    m = re.search(r"\[input_script\] %s t=\d+ @%s (.*)" % (kind, re.escape(label)), text)
    return m.group(1) if m else ""


def grep_hits(text, label):
    m = re.search(r"hits=(\d+)", script_line(text, "grep", label))
    return int(m.group(1)) if m else -1


def node_matches(text, label):
    m = re.search(r"match=(\d+)", script_line(text, "nodes", label))
    return int(m.group(1)) if m else -1


SCRIPT = """0 boot
1500 call clearPops
1600 call showWindow 10
3000 call windows.9.switchTab 3
6000 grep @new {good} .mapListItem
6050 grep @newbadge NEW! .mapListItem
6100 nodes @newmodes .mapListItem [title*=Deathmatch]
6500 call windows.9.switchTab 6
7000 call windows.9.searchMaps "{good}" -1 ""
9500 grep @byname {good} [id=searchResultsMA] .mapListItem
9600 call windows.9.searchMaps "" {parkour} ""
12000 grep @parkour {good} [id=searchResultsMA] .mapListItem
12050 nodes @parkourn [id=searchResultsMA] .mapListItem
12100 call windows.9.searchMaps "" {puzzle} ""
14500 text @puzzle [id=searchResultsMA]
14600 call windows.9.searchMaps "{good}" -1 "official"
17000 text @official [id=searchResultsMA]
17100 call windows.9.searchMaps "burg" -1 "official"
19500 grep @burg Burg [id=searchResultsMA] .mapListItem
19600 call windows.9.searchMaps "{good}" -1 "community"
22000 grep @community {good} [id=searchResultsMA] .mapListItem
22100 call selectHostMap "{good}" "" "1" "{account}" 0
24000 call selectHostMap "{bad}" "" "2" "{account}" 0
26000 call playMap "{bad}"
29000 call clearPops
29100 call playMap "{good}"
33000 play
38000 players
38100 state
38500 quit
"""


def run_client(port, account, token, good, bad):
    cfg = os.path.join(WORK, "cfg")
    os.makedirs(os.path.join(cfg, "krunknative"), exist_ok=True)
    for k, v in (("krunker_username", account), ("krunker_token", token), ("knRuleset", "v10")):
        with open(os.path.join(cfg, "krunknative", k + ".json"), "w") as f:
            f.write(v)
    path = os.path.join(WORK, "games.script")
    with open(path, "w") as f:
        f.write(SCRIPT.format(good=good, bad=bad, account=account, parkour=CATEGORY_PARKOUR, puzzle=CATEGORY_PUZZLE))
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": cfg,
                "XDG_STATE_HOME": os.path.join(WORK, "state"), "KRUNKNATIVE_NO_BROWSER": "1",
                "KRUNKNATIVE_INPUT_SCRIPT": path, "KRUNKNATIVE_WINDOW_SIZE": "1280x720",
                "KRUNKNATIVE_BRAIN_URL": "http://127.0.0.1:%d" % port, "KRUNKNATIVE_NET_TRANSPORT": "tcp",
                "KRUNKNATIVE_FINDER_URL": "http://127.0.0.1:%d/servers" % port})
    log = os.path.join(WORK, "client.log")
    p = subprocess.Popen(["stdbuf", "-oL", "-eL", os.path.join(BUILD, "client_bin")], cwd=ROOT, stdout=open(log, "w"),
                         stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, start_new_session=True)
    try:
        p.wait(180)
        check("client_bin exits cleanly", p.returncode == 0, "exit %d (%s)" % (p.returncode, log))
    except subprocess.TimeoutExpired:
        os.killpg(p.pid, signal.SIGKILL)
        p.wait()
        check("client_bin finished its script", False, log)
    return read(log)


def main():
    if not os.path.exists(os.path.join(BUILD, "client_bin")) or not os.path.exists(os.path.join(BUILD, "services/brain/kn_brain")):
        print("community_games_e2e: client_bin or kn_brain not built: skipped")
        return 77
    scratch = [sys.executable, "-I", os.path.join(ROOT, "services", "brain", "tests", "db_scratch.py"), ROOT]
    r = subprocess.run(scratch + ["create", DRIVER, NAME], capture_output=True)
    if r.returncode:
        print("community_games_e2e: %s unavailable: skipped" % DRIVER)
        return 77
    dbcfg = json.loads(r.stdout)
    env = dict(os.environ, **{"KN_DATABASE_" + k.upper(): str(v) for k, v in dbcfg.items()})
    brain = None
    brain_log = os.path.join(WORK, "brain.log")
    try:
        subprocess.run([os.path.join(BUILD, "services/brain/kn_brain"), "--migrate-only"], cwd=ROOT, env=env, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        brain = subprocess.Popen([os.path.join(BUILD, "services/brain/kn_brain"), "--brain.host=127.0.0.1", "--brain.port=0",
                                  "--brain.id=cmgames-%d" % os.getpid()], cwd=ROOT, env=env,
                                 stdout=open(brain_log, "w"), stderr=subprocess.STDOUT)
        m = wait_for(brain_log, r"listening on http://127\.0\.0\.1:(\d+) \(ws")
        if not m:
            check("kn_brain starts", False, brain_log)
            return 1
        port = int(m.group(1))
        stamp = str(int(time.time()) % 100000)
        account, good, bad = "cmg" + stamp, "CmGood" + stamp, "CmBad" + stamp
        token, *replies = asyncio.run(publish_maps(port, account, [(good, CATEGORY_PARKOUR), (bad, CATEGORY_PUZZLE)]))
        check("both maps published", all(r[0] == "pubMR" for r in replies), str(replies))
        check("BAD's stored copy loses its spawns", break_map(dbcfg, bad))

        text = run_client(port, account, token, good, bad)
        # 1. the New tab
        check("New: GOOD's card", grep_hits(text, "new") == 1, script_line(text, "grep", "new"))
        check("New: v10's NEW! badge on a map published today", grep_hits(text, "newbadge") >= 1, script_line(text, "grep", "newbadge"))
        check("New: the mode tags (Free for All, Team Deathmatch) on the card", node_matches(text, "newmodes") >= 1,
              script_line(text, "nodes", "newmodes"))
        # 2. Search
        check("Search by name finds GOOD (kn_brain /api/search)", grep_hits(text, "byname") == 1, script_line(text, "grep", "byname"))
        check("Popular Categories: Parkour with no name lists GOOD, not BAD (Puzzle)",
              grep_hits(text, "parkour") == 1 and node_matches(text, "parkourn") == 1,
              script_line(text, "grep", "parkour") + " / " + script_line(text, "nodes", "parkourn"))
        puzzle = script_line(text, "text", "puzzle")
        check("Puzzle: BAD's category lists BAD, not GOOD", bad in puzzle and good not in puzzle, puzzle)
        check("Official: a published map is not official (\"No Games found!\")",
              "No Games found!" in script_line(text, "text", "official"), script_line(text, "text", "official"))
        check("Official: Burg", grep_hits(text, "burg") >= 1, script_line(text, "grep", "burg"))
        check("Community: GOOD", grep_hits(text, "community") == 1, script_line(text, "grep", "community"))
        # 3. the Host Game seam
        m = re.search(r"selectHostMap: %s -> slot (-?\d+)" % re.escape(good), text)
        check("selectHostMap GOOD: downloaded, a map slot", m is not None and int(m.group(1)) >= 0, str(m and m.group(0)))
        m = re.search(r"selectHostMap: %s -> slot (-?\d+)" % re.escape(bad), text)
        check("selectHostMap BAD: refused (-1)", m is not None and int(m.group(1)) == -1, str(m and m.group(0)))
        check("BAD is refused with v10's validateMap message", "[community] %s: Missing spawn points." % bad in text)
        check("GOOD was downloaded once (the session cache serves Play)", text.count("[community] downloaded %s" % good) == 1,
              str(text.count("[community] downloaded %s" % good)))
        # 4. Play
        check("playMap BAD never starts a game", "playing published map %s" % bad not in text)
        check("playMap GOOD: the offline game plays it", "[mm] playing published map %s offline" % good in text,
              "\n".join(l for l in text.splitlines() if "[mm]" in l or "[community]" in l)[-800:])
        m = re.search(r"\[input_script\] state t=\d+ (.*)", text)
        state = m.group(1) if m else ""
        check("in the game on it after Play (the menu closed, spawned)", "menu=0" in state and "pos=0.000,0.000,0.000" not in state, state)
        print("client log: " + os.path.join(WORK, "client.log"))
    finally:
        if brain:
            brain.terminate()
            try:
                brain.wait(10)
            except subprocess.TimeoutExpired:
                brain.kill()
        subprocess.run(scratch + ["drop", DRIVER, NAME], capture_output=True)
    bad_lines = [l for l in read(brain_log).splitlines() if re.search(r"failed:|ERROR|FATAL", l)]
    check("no statement errors in the brain log", not bad_lines, str(bad_lines[:3]))
    if all(results):
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print("logs kept in " + WORK)
    print(f"{sum(results)}/{len(results)} passed")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
