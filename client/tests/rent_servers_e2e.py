#!/usr/bin/env python3
"""Rent a Server end to end (doc/v10/rented-servers.md): the native client's Rent Dedicated Server window
(client/src/ui/rent_servers_ui.c) against a local kn_brain (sqlite, services/brain/rentals.c), kn_rentd
(server/src/rentd.c) and the server_bin it starts.

  python3 -I client/tests/rent_servers_e2e.py BUILD_DIR      (repository root)

  1. a signed-in player (30,000 KR) opens Host Game -> Rent a Server (the chooser's card), New York, the free
     Ashburn_1, picks Kanji through the Games window (showGames(2) -> selectHostMap), 30 days, Rent Server:
     the window says it is rented and My Servers lists it; the brain charged 7,500 KR (dediPrices[0]);
  2. kn_rentd (host e2ehost, polling the brain's agent API) starts a server_bin for the slot with `dedicated 1`;
     it links to the brain and its lobby is listed by /mm/game-list with ds (24/7) and h = Ashburn_1, and the
     client's server browser shows the row with the 24/7 tag;
  3. the rental lapses (expires_s in the past): the brain frees the slot, kn_rentd stops the server and the
     lobby leaves the list.
Headless client_bin (KRUNKNATIVE_HEADLESS=1, null audio, no DISPLAY, private XDG dirs). 77 = skip (no kn_brain,
kn_rentd or python websockets). Logs are kept in ~/.cache/kn-rent-e2e-* on failure.
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
import urllib.request

ROOT = os.getcwd()
BUILD = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug")
BRAIN = os.path.join(BUILD, "services", "brain", "kn_brain")
RENTD = os.path.join(BUILD, "kn_rentd")
SERVER = os.path.join(BUILD, "server_bin")
CLIENT = os.path.join(BUILD, "client_bin")
for b in (BRAIN, RENTD, SERVER, CLIENT):
    if not os.path.exists(b):
        print("rent_servers_e2e: %s missing: skip" % b)
        sys.exit(77)
try:
    import websockets
except ImportError:
    print("rent_servers_e2e: python websockets missing: skip")
    sys.exit(77)
sys.path.insert(0, os.path.join(ROOT, "server", "tests"))
from kn_proto import free_ports  # noqa: E402

CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-rent-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
SECRET = "kn-rent-e2e-%d-xxxxxxxxxxxxxxxxxxxxxxxx" % os.getpid()
NAME, PASS = "rent%d" % (os.getpid() % 100000), "rentpass1"
ACC = [0]
DB = os.path.join(WORK, "brain.db")
results = []
procs = []


def check(name, ok, detail=""):
    results.append(bool(ok))
    print("%s  %s%s" % ("PASS" if ok else "FAIL", name, ("  -- " + str(detail)) if detail and not ok else ""), flush=True)


def sql(q, *a):
    con = sqlite3.connect(DB, timeout=10)
    try:
        rows = con.execute(q, a).fetchall()
        con.commit()
        return rows
    finally:
        con.close()


def wait(cond, timeout=30, step=0.25):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            v = cond()
        except Exception:  # noqa: BLE001
            v = None
        if v:
            return v
        time.sleep(step)
    return None


def run_client(tag, script, brain_url, game_port, timeout=240):
    path = os.path.join(WORK, tag + ".script")
    with open(path, "w") as f:
        f.write(script.strip() + "\n")
    log = os.path.join(WORK, tag + ".log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY") and (not k.startswith("KRUNKNATIVE_") or k == "KRUNKNATIVE_EGL_DEVICE")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": os.path.join(WORK, "cfg-" + tag),
                "XDG_STATE_HOME": os.path.join(WORK, "state-" + tag), "KRUNKNATIVE_NO_BROWSER": "1",
                "KRUNKNATIVE_UPDATES_URL": "off", "KRUNKNATIVE_INPUT_SCRIPT": path, "KRUNKNATIVE_WINDOW_SIZE": "1280x720",
                "KRUNKNATIVE_DEBUG": "1", "KRUNKNATIVE_BRAIN_URL": brain_url,
                # nothing reaches production: no server finder, krunker.io's hosts are a dead local port
                "KRUNKNATIVE_FINDER_URL": "off", "KRUNKNATIVE_KRUNKER_MATCHMAKER": "http://127.0.0.1:9",
                "KRUNKNATIVE_KRUNKER_SOCIAL": "http://127.0.0.1:9/social", "KRUNKNATIVE_KRUNKER_EDITOR": "http://127.0.0.1:9/editor"})
    with open(log, "w") as out:
        p = subprocess.Popen([CLIENT, "--mode", "online", "--brain", brain_url, "--finder", "off",
                              "--connect", "127.0.0.1:%d" % game_port],
                             cwd=ROOT, stdout=out, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env,
                             start_new_session=True)
        try:
            p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
            check(tag + ": client_bin finished its script", False, "timeout, log " + log)
    with open(log, errors="replace") as f:
        return f.read()


def text(log, label):
    m = re.search(r"\[input_script\] text t=\d+ " + re.escape(label) + r" n=(\d+) checked=\d value=.*? text=(.*)", log)
    return re.sub(r"\s+", " ", m.group(2)).strip() if m else None


def grep(log, label):
    m = re.search(r"\[input_script\] grep t=\d+ " + re.escape(label) + r" needle=\S+ n=(\d+) hits=(\d+)", log)
    return int(m.group(2)) if m else -1


def game_list(brain_url):
    with urllib.request.urlopen(brain_url + "/mm/game-list?hostname=127.0.0.1", timeout=5) as r:
        data = json.loads(r.read())
    games = data.get("games", data) if isinstance(data, dict) else data
    return [g for g in games if isinstance(g, list) and len(g) > 4 and isinstance(g[4], dict)]


def rented_lobby(brain_url):
    return next((g for g in game_list(brain_url) if g[4].get("ds") is True and g[4].get("h") == "Ashburn_1"), None)


async def register(port):
    async with websockets.connect("ws://127.0.0.1:%d/social/ws?enc=json" % port) as ws:
        await ws.send(json.dumps(["a", 0, [NAME, PASS, 0], "captcha"]))
        while True:
            f = json.loads(await asyncio.wait_for(ws.recv(), 15))
            if f[0] == "a":
                return f


LOGIN = """0 menu
500 clickid knV10LoginBtn
800 clickid knV10LoginMode
1000 clickid accName
1100 type {name}
1200 clickid accPass
1300 type {pw}
1400 down ENTER
1410 up ENTER
"""


def scenario(brain_url, game_port):
    # 1. the window: the chooser's card, a region, a server, the game, the duration, Rent Server
    log = run_client("rent", LOGIN.format(name=NAME, pw=PASS) + """
5000 call showWindow 42
5600 grep @card Rent [id=menuWindow]
5700 clicksel [onclick*="openHostWindow(false, 2)"]
6500 grep @regions Available [id=menuWindow]
6600 text @tabs [id=menuWindow]
6700 call windows.7.servInd= 0
6710 call windows.7.switchTab2 2
7200 grep @ashburn Ashburn_1 [id=menuWindow]
7300 call windows.7.servInd= 0
7310 call windows.7.switchTab2 3
7800 call showGames 2
8800 call selectHostMap "Kanji" 0 "" ""
9500 grep @game Kanji [id=menuWindow]
9600 call windows.7.dediCst 0
9700 text @cost [id=dediCostI]
9800 clickid rentServBtn
12500 text @msg [id=dediHostErr]
12600 call windows.7.switchTab2 1
13200 grep @mine Ashburn_1 [id=menuWindow]
13300 quit
""", brain_url, game_port)
    check("signed in", "[account] logged in as %s" % NAME in log, "log in " + WORK)
    check("Host Game has the Rent a Server card", grep(log, "@card") >= 1)
    check("the card opens the rental window's regions (Available counts)", grep(log, "@regions") >= 1, text(log, "@tabs"))
    check("New York lists Ashburn_1", grep(log, "@ashburn") >= 1)
    check("the Games window's pick comes back as the server's game", grep(log, "@game") >= 1)
    check("30 days cost 7,500 KR (dediPrices[0])", text(log, "@cost") == "7,500", text(log, "@cost"))
    msg = text(log, "@msg") or ""
    check("Rent Server: the brain's answer in #dediHostErr", "rented" in msg.lower(), msg)
    check("My Servers lists the rented server", grep(log, "@mine") >= 1)
    acc = [(ACC[0],)]
    funds = sql("SELECT player_funds FROM players WHERE player_id = ?", ACC[0])
    check("the brain charged 7,500 KR", funds and funds[0][0] == 22500, funds)
    row = sql("SELECT owner_id, conf FROM rented_servers WHERE id = 1")
    check("the slot is the player's, with Kanji", row and acc and row[0][0] == acc[0][0] and "Kanji" in (row[0][1] or ""), row)

    # 2. kn_rentd starts it; the brain lists its lobby as 24/7 under its own name; the client's browser shows it
    state = wait(lambda: sql("SELECT state FROM rented_servers WHERE id = 1 AND state = 'running'"), 60)
    check("kn_rentd started the server (agent status running)", state, sql("SELECT state, running_gen FROM rented_servers"))
    lobby = wait(lambda: rented_lobby(brain_url), 60)
    check("/mm/game-list has the rented lobby: ds (24/7), h Ashburn_1", lobby, game_list(brain_url) if not lobby else "")
    if lobby:
        check("the lobby earns no KR (isDedicated)", lobby[4].get("earnKR") is False, lobby[4])
    # the list comes once the menu's game is joined: waitfor holds the script until its region is listed, then opens it
    # (the only region: the rented lobby's)
    log = run_client("browse", """0 menu
1500 call showWindow 2
1600 waitfor [onclick*="setExpandedRegion"]
1700 clicksel [onclick*="setExpandedRegion"]
2200 grep @row Ashburn_1 [id=menuWindow]
2300 grep @tag 24/7 [id=menuWindow]
2400 quit
""", brain_url, game_port)
    check("the server browser lists Ashburn_1", grep(log, "@row") >= 1, "log in " + WORK)
    check("with the 24/7 tag", grep(log, "@tag") >= 1)

    # 3. the rental lapses: freed, stopped, unlisted
    sql("UPDATE rented_servers SET expires_s = 1 WHERE id = 1")
    check("the lapsed rental is freed", wait(lambda: sql("SELECT 1 FROM rented_servers WHERE id = 1 AND owner_id IS NULL"), 30))
    check("kn_rentd stops it (agent status stopped)", wait(lambda: sql("SELECT 1 FROM rented_servers WHERE id = 1 AND state = 'stopped'"), 90))
    check("the lobby leaves the list", wait(lambda: not rented_lobby(brain_url), 60))


def main():
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(1))
    brain_port, game_port, rent_port = free_ports(3)
    brain_url = "http://127.0.0.1:%d" % brain_port
    pool = [{"id": 1, "host": "e2ehost", "region": 0, "name": "Ashburn", "sid": 1, "port": rent_port, "public_host": "127.0.0.1"}]
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=SECRET)
    try:
        procs.append(subprocess.Popen([BRAIN, "--brain.host=127.0.0.1", "--brain.port=%d" % brain_port, "--database.driver=sqlite",
                                       "--database.path=" + DB, "--migrate", "--brain.public_url=" + brain_url,
                                       "--accounts.login_throttle_s=0", "--brain.rentals.pool=" + json.dumps(pool),
                                       "--brain.rentals.check_ms=500", "--brain.rentals.start_timeout_s=300"],
                                      cwd=ROOT, env=env, stdout=open(os.path.join(WORK, "brain.log"), "w"),
                                      stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, start_new_session=True))
        up = wait(lambda: b'"brain"' in urllib.request.urlopen(brain_url + "/mm/status", timeout=1).read(), 20, 0.1)
        check("kn_brain is up", up)
        reg = asyncio.run(register(brain_port))
        check("registered " + NAME, reg and reg[1] is None, reg)
        ACC[0] = reg[2]
        sql("UPDATE players SET player_funds = 30000 WHERE player_id = ?", ACC[0])
        # the menu's own game (the client's --connect)
        procs.append(subprocess.Popen([SERVER, "--headless", "--cfg", "none", "--port", str(game_port)], cwd=ROOT,
                                      stdout=open(os.path.join(WORK, "server.log"), "w"), stderr=subprocess.STDOUT,
                                      stdin=subprocess.DEVNULL, start_new_session=True))
        cfg = os.path.join(WORK, "rentd.cfg")
        with open(cfg, "w") as f:
            f.write('host_id "e2ehost"\nbrain_url "%s"\nserver_bin "%s"\nworkdir "%s"\nstate_dir "%s"\ncoordinator_secret "%s"\n'
                    'coordinator_url "ws://127.0.0.1:%d/mm/internal/ws"\nlisten_host "127.0.0.1"\npublic_host "127.0.0.1"\n'
                    'poll_s 3\nstop_grace_s 10\nchild_mem_mb 0\n'
                    % (brain_url, SERVER, ROOT, os.path.join(WORK, "rentd"), SECRET, brain_port))
        procs.append(subprocess.Popen([RENTD, "--cfg", cfg], cwd=ROOT, stdout=open(os.path.join(WORK, "rentd.log"), "w"),
                                      stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, start_new_session=True))
        scenario(brain_url, game_port)
    except Exception as e:  # noqa: BLE001
        check("exception", False, repr(e))
    finally:
        for p in reversed(procs):
            if p.poll() is None:
                os.killpg(p.pid, signal.SIGTERM)
                try:
                    p.wait(20)
                except subprocess.TimeoutExpired:
                    os.killpg(p.pid, signal.SIGKILL)
    ok = results and all(results)
    print("%s: %d/%d" % ("PASS" if ok else "FAIL", sum(results), len(results)))
    if ok and not os.environ.get("KN_KEEP"):
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print("logs kept in " + WORK)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
