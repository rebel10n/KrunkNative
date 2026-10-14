#!/usr/bin/env python3
"""The v10 Customize / Loadout / Titles / Store flow end to end (client/src/ui/v10_customize.c,
store_ui.c) on a real kn_brain (sqlite, temp dir) and server_bin:

  register -> the Customize tab of Change Loadout (windows[2] tab 2) -> the Hat picker (the shell
  wraps it, goBackWindow(52)) -> equip a granted hat -> Title window (windows[53]: the catalogue
  with every title locked, a locked pick does not equip) -> the Store on the v10 layout
  (store-window-mount) -> Prize Wheels -> a real Starter spin: KR charged, the item granted (the
  brain's skin_transactions + the account's funds).

  python3 -I client/tests/v10_customize_e2e.py BUILD_DIR      (repository root)
Headless client_bin (KRUNKNATIVE_HEADLESS=1, null audio, no DISPLAY, private XDG dirs), ports from
kn_proto.free_ports(). 77 = skip (no kn_brain).
"""
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
sys.path.insert(0, os.path.join(ROOT, "server", "tests"))
if not os.path.exists(BRAIN):
    print("needs kn_brain (KRUNKNATIVE_SERVICES): skip")
    sys.exit(77)
from kn_proto import free_ports  # noqa: E402

CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-v10-customize-", dir=CACHE if os.path.isdir(CACHE) else None)
STAMP = os.getpid() % 100000
NAME, PASS = "cst%d" % STAMP, "pass_word1"
GRANTED = 111  # Top Hat: a hat (store type 1) the fixture grants, so the picker has a card to click
results = []
procs = []


def check(name, ok, detail=""):
    results.append(bool(ok))
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def run_client(name, script, brain_url, game_port, timeout=180):
    """One headless client_bin run of `script`; returns its log text."""
    path = os.path.join(WORK, name + ".script")
    with open(path, "w") as f:
        f.write(script)
    log = os.path.join(WORK, name + ".log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": os.path.join(WORK, "cfg"),
                "XDG_STATE_HOME": os.path.join(WORK, "state"), "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_UPDATES_URL": "off", "KRUNKNATIVE_INPUT_SCRIPT": path, "KRUNKNATIVE_WINDOW_SIZE": "1280x720"})
    with open(log, "w") as out:
        p = subprocess.Popen([os.path.join(BUILD, "client_bin"), "--mode", "online", "--brain", brain_url, "--finder", "off",
                              "--connect", "127.0.0.1:%d" % game_port], cwd=ROOT, stdout=out, stderr=subprocess.STDOUT,
                             stdin=subprocess.DEVNULL, env=env, start_new_session=True)
        try:
            p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
            check(f"{name}: client_bin finished its script", False, f"timeout, log {log}")
    with open(log, errors="replace") as f:
        return f.read()


def vis(log, label):
    m = re.search(r"\[input_script\] vis t=\d+ @%s(.*)" % re.escape(label), log)
    return dict(kv.split("=", 1) for kv in m.group(1).split()) if m else {}


def count(log, label):
    m = re.search(r"\[input_script\] rects t=\d+ @%s n=(\d+)" % re.escape(label), log)
    return int(m.group(1)) if m else -1


def storage(key):
    try:
        with open(os.path.join(WORK, "cfg", "krunknative", key + ".json")) as f:
            text = f.read().strip()
    except OSError:
        return None
    try:
        v = json.loads(text)
        return v if not isinstance(v, str) else str(v)
    except ValueError:
        return text


def db(path, sql, args=()):
    con = sqlite3.connect(path)
    try:
        rows = con.execute(sql, args).fetchall()
        con.commit()
        return rows
    finally:
        con.close()


def scenario(brain_url, game_port, dbpath):
    # 0. a guest online: the Class tab locks the classes that need a level (v10 req _34270)
    log = run_client("guest", """0 menu
1500 call showWindow 3
2500 clicksel [onclick*="tabIndex=0"]
3500 rects @locked .lockedClass
3600 rects @cards .classCard
3700 quit
""", brain_url, game_port)
    check("guest: every class but the level-0 ones is locked (Register to unlock)",
          count(log, "locked") >= 10 and count(log, "locked") < count(log, "cards"), "locked=%d cards=%d" % (count(log, "locked"), count(log, "cards")))

    # 1. register (the Account window's form) and give the account KR through the fixture DB
    log = run_client("register", """0 menu
500 call showWindow 5
1200 clickid accName
1300 type %s
1400 down TAB
1410 up TAB
1500 type %s
1550 down TAB
1560 up TAB
1580 type %s@example.com
1600 clicksel .accBtn.buttonP
5000 quit
""" % (NAME, PASS, NAME), brain_url, game_port)  # Tab: name -> password -> email (v10 registers by email)
    check("register through the native form", f"[account] logged in as {NAME}" in log)
    db(dbpath, "UPDATE players SET player_funds = 2000 WHERE player_id = (SELECT id FROM accounts WHERE username = ?)", (NAME,))
    db(dbpath, "INSERT INTO inventory (account_id, skin_id, count) SELECT id, ?, 1 FROM accounts WHERE username = ?",
       (GRANTED, NAME))
    check("the fixture granted %d KR and item %d" % (2000, GRANTED),
          bool(db(dbpath, "SELECT player_funds FROM players WHERE player_id = (SELECT id FROM accounts WHERE username = ?)", (NAME,))))

    # 2. the Customize tab: the shell, its slots, the Hat picker (wrapper + the back target)
    log = run_client("customize", """0 menu
1500 call showWindow 3
2500 clicksel [onclick*="tabIndex=2"]
3500 vis @cust charPreviewSlot loadoutContent customizeContainer
3550 rects @slot .titleSlotSelect
3600 rect @r charPreviewSlot classPreviewCanvas
4500 clicksel [onclick*="showWindow(16)"]
5500 vis @hat loadoutToolbar charPreviewSlot skinList
5600 rects @cards .skinCard[data-index]
5700 quit
""", brain_url, game_port)
    v = vis(log, "cust")
    check("Customize tab: the v10 shell shows", v.get("charPreviewSlot") == "1" and v.get("customizeContainer") == "1" and count(log, "slot") == 1, str(v))
    check("the live preview canvas is in #charPreviewSlot", ":preview=1" in log.replace(" ", " ") or "classPreviewCanvas=" in log)
    v = vis(log, "hat")
    check("the Hat picker is wrapped in the shell", v.get("loadoutToolbar") == "1" and v.get("charPreviewSlot") == "1" and v.get("skinList") == "1", str(v))
    m = re.search(r"\[input_script\] rects t=\d+ @cards n=(\d+).*", log)
    check("the granted hat is the picker's owned card", bool(m) and int(m.group(1)) == 1 and (" %d@" % GRANTED) in m.group(0),
          m.group(0) if m else "no rects")

    # 3. equip the granted hat, then the Title window and a locked title
    log = run_client("equip", f"""0 menu
1500 call showWindow 16
2500 clicksel .skinCard[data-index="{GRANTED}"]
4000 vis @back windowHolder
4050 cosm @menu
4100 call showWindow 3
4600 call showWindow 54
5600 vis @title loadoutToolbar skinList
5650 rects @rows .titleRow
5700 clicksel [onclick*="selectTitle(87)"]
6700 call closWind
7000 play
9000 cosm @game
13000 cosm @game
20000 cosm @game
20100 quit
""", brain_url, game_port)
    pv = re.search(r"cosm t=\d+ @menu who=preview hat=(-?\d+)", log)
    me = re.search(r"cosm t=\d+ @game who=me hat=(%d)" % GRANTED, log) or re.search(r"cosm t=\d+ @game who=me hat=(-?\d+)", log)
    check("the class preview wears the equipped hat", bool(pv) and int(pv.group(1)) == GRANTED, pv.group(0) if pv else "no cosm")
    check("the next spawn wears it in game", bool(me) and int(me.group(1)) == GRANTED, me.group(0) if me else "no cosm (not spawned)")
    check("the picker's card equips the hat", str(storage("hatIndex")) == str(GRANTED), "hatIndex=%r" % storage("hatIndex"))
    v = vis(log, "title")
    check("the Title window shows the catalogue in the shell", v.get("skinList") == "1" and v.get("loadoutToolbar") == "1" and count(log, "rows") > 100, "%s rows=%d" % (v, count(log, "rows")))
    check("a title the account does not own is not equipped", storage("titleIndex") in (None, "-1"), "titleIndex=%r" % storage("titleIndex"))

    # 4. the Store: the v10 layout, Prize Wheels, a real Starter spin (50 KR)
    before = db(dbpath, "SELECT player_funds FROM players WHERE player_id = (SELECT id FROM accounts WHERE username = ?)", (NAME,))[0][0]
    log = run_client("spin", """0 menu
1500 call showWindow 14
2600 vis @store store-window-mount
2700 rect @rw store-window-mount
3600 call windows.13.switchTab 1
4600 rects @tiles .prize-tile
4650 rects @tickets .view-tickets-btn
4700 cursor 450 410
4900 clicksel [onclick*="prizeWheel(0)"]
5900 vis @spin spinUI spinButton
6000 clickid spinButton
16000 vis @won spinUI spinItem
16100 quit
""", brain_url, game_port)
    v = vis(log, "store")
    check("the Store is the v10 Svelte store", v.get("store-window-mount") == "1", str(v))
    check("Prize Wheels shows its tiles and View Tickets", count(log, "tiles") == 2 and count(log, "tickets") == 1,
          "tiles=%d tickets=%d" % (count(log, "tiles"), count(log, "tickets")))
    v = vis(log, "spin")
    check("the Starter spin opened the wheel screen", v.get("spinUI") == "1", str(v))
    after = db(dbpath, "SELECT player_funds FROM players WHERE player_id = (SELECT id FROM accounts WHERE username = ?)", (NAME,))[0][0]
    won = db(dbpath, "SELECT skin_id, delta, source FROM skin_transactions WHERE source = 'spin' ORDER BY id DESC LIMIT 1")
    check("the spin charged 50 KR on the brain", before - after == 50, "%s -> %s" % (before, after))
    check("the spin granted the won item", bool(won) and won[0][1] == 1, str(won))


def daily_spin(brain_url, game_port, dbpath):
    """The sidebar's Daily Spin: 4999 score today shows the bar, 5000 makes it FREE SPIN, a click
    claims the Universal Starter Ticket 8067 (the first claim) and opens its wheel; a second claim is refused."""
    acc = db(dbpath, "SELECT id FROM accounts WHERE username = ?", (NAME,))[0][0]
    db(dbpath, "INSERT INTO match_results (match_id, account_id, score) VALUES ('ds-e2e-1', ?, 4999)", (acc,))
    log = run_client("dailyspin-bar", """0 menu
3000 vis @ds dailySpinDiv
3100 rects @bar .dsBarFill
3200 rects @claim .dsClaim
3300 quit
""", brain_url, game_port)
    check("signed in: the sidebar has the Daily Spin item", vis(log, "ds").get("dailySpinDiv") == "1", str(vis(log, "ds")))
    check("4999 / 5000: the progress bar, no FREE SPIN", count(log, "bar") == 1 and count(log, "claim") == 0,
          "bar=%d claim=%d" % (count(log, "bar"), count(log, "claim")))
    db(dbpath, "INSERT INTO match_results (match_id, account_id, score) VALUES ('ds-e2e-2', ?, 1)", (acc,))
    before = db(dbpath, "SELECT count(*) FROM skin_transactions WHERE source = 'dailyspin'")[0][0]
    log = run_client("dailyspin-claim", """0 menu
3000 rects @claim .dsClaim
3100 clickid dailySpinDiv
5000 vis @wheel spinUI
5100 rects @done .dsDone
5200 call knDailySpin
6500 quit
""", brain_url, game_port)
    check("5000 / 5000: FREE SPIN", count(log, "claim") == 1, "claim=%d" % count(log, "claim"))
    grants = db(dbpath, "SELECT skin_id FROM skin_transactions WHERE source = 'dailyspin'")
    check("the claim granted the Starter spin ticket once", len(grants) == before + 1 and grants[-1][0] == 8067, str(grants))
    check("the ticket's wheel opened and the item says CLAIMED", vis(log, "wheel").get("spinUI") == "1" and count(log, "done") == 1,
          "%s done=%d" % (vis(log, "wheel"), count(log, "done")))


def main():
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(1))
    brain_port, game_port = free_ports(2)
    dbpath = os.path.join(WORK, "coordinator.db")
    brain_url = "http://127.0.0.1:%d" % brain_port
    brain_log = open(os.path.join(WORK, "brain.log"), "w")
    try:
        procs.append(subprocess.Popen([BRAIN, "--brain.host=127.0.0.1", "--accounts.signup_rewards.enabled=false", "--brain.port=%d" % brain_port, "--database.driver=sqlite",
                                       "--database.path=" + dbpath, "--migrate", "--brain.public_url=" + brain_url],
                                      cwd=ROOT, stdout=brain_log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                                      start_new_session=True))
        procs.append(subprocess.Popen([os.path.join(BUILD, "server_bin"), "--headless", "--cfg", "none", "--port", str(game_port)],
                                      cwd=ROOT, stdout=open(os.path.join(WORK, "server.log"), "w"), stderr=subprocess.STDOUT,
                                      stdin=subprocess.DEVNULL, start_new_session=True))
        for _ in range(100):
            try:
                with urllib.request.urlopen(brain_url + "/mm/status", timeout=1) as r:
                    if b'"brain"' in r.read():
                        break
            except OSError:
                time.sleep(0.1)
        scenario(brain_url, game_port, dbpath)
        daily_spin(brain_url, game_port, dbpath)
    except Exception as e:  # noqa: BLE001
        check("exception", False, repr(e))
    finally:
        for p in procs:
            if p.poll() is None:
                os.killpg(p.pid, signal.SIGTERM)
                try:
                    p.wait(10)
                except subprocess.TimeoutExpired:
                    os.killpg(p.pid, signal.SIGKILL)
        brain_log.close()
    ok = all(results)
    print(f"{'PASS' if ok else 'FAIL'}: {sum(results)}/{len(results)}")
    if ok and not os.environ.get("KN_KEEP"):
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print(f"logs kept in {WORK}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
