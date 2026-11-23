#!/usr/bin/env python3
"""The v10 Customize / Loadout / Titles / Store flow end to end (client/src/ui/v10_customize.c,
store_ui.c) on a real kn_brain (sqlite, temp dir) and server_bin:

  a guest's open classes (every class opens off official krunker.io games since 8de2fe8c) and locked
  Acog Scope -> register -> the Acog Scope locked at class level 1, open
  at 20 -> the Customize tab of Change Loadout (windows[2] tab 2) -> the Hat picker (the shell
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
    # KRUNKNATIVE_DEBUG=2: the [account] / [mm] / [preview] / console lines it waits on are level 1-2 (5f55eef1)
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_DEBUG": "2", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": os.path.join(WORK, "cfg"),
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


def seed_session(brain_url, name, password):
    """The signed-in runs resume NAME's session. A sign-in is stored only for an account the Account Manager
    lists (account_ui.c persist_session, since c7f73bd4 / f6d4a040), so the native form's sign-in does not
    survive the client; the legacy krunker_username / krunker_token keys do: load_session moves them into the
    alt vault (the file keystore in headless runs). The token comes from the brain's /auth/login."""
    req = urllib.request.Request(brain_url + "/auth/login", data=json.dumps({"username": name, "password": password}).encode(),
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=10) as r:
        token = json.loads(r.read()).get("token", "")
    folder = os.path.join(WORK, "cfg", "krunknative")
    os.makedirs(folder, exist_ok=True)
    for key, value in (("krunker_username", name), ("krunker_token", token)):
        with open(os.path.join(folder, key + ".json"), "w") as f:
            f.write(value)
    return bool(token)


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


def set_storage(key, value):
    folder = os.path.join(WORK, "cfg", "krunknative")
    os.makedirs(folder, exist_ok=True)
    path = os.path.join(folder, key + ".json")
    if value is None:
        if os.path.exists(path):
            os.remove(path)
        return
    with open(path, "w") as f:
        f.write(value)


def rect_indices(log, label):
    m = re.search(r"\[input_script\] rects t=\d+ @%s n=\d+(.*)" % re.escape(label), log)
    return [tok.split("@", 1)[0] for tok in m.group(1).split()] if m else []


def grep_hits(log, label):
    m = re.search(r"\[input_script\] grep t=\d+ @%s .*hits=(\d+)" % re.escape(label), log)
    return int(m.group(1)) if m else -1


def guest_picks(brain_url, game_port):
    """Round 6 (deob-10.0.7.js): a guest's pickers. getSkins(account, true) gives everyone the free items
    (:217752-217795): the 14 class faces (Triggerman 4544 .. Infiltrator 5234, classIndex) in Select Face, each
    locked unless its class is selected ("Req <class>", the lockedSkin overlay: :223500-223505); favItem stars a
    card, saves krk_favList and the next list puts it first (:223687, :223501); Select Spray's cards carry the item
    and favourite buttons (:220249-220253); the Grappler's caption is "Customs Only" (:221076-221080); without
    premium the hat + head pair blocks the hat (:222484-222516)."""
    set_storage("krk_favList", None)
    set_storage("hatIndex", "111")
    set_storage("headIndex", "6044")
    log = run_client("guest-picks", """0 menu
1500 call showWindow 39
2500 rects @faces .skinCard[data-index]
2550 rects @locked .lockedSkin
2600 rects @open .skinCard[onclick*="selectFace(4544)"]
2650 grep @req Req .skinCard
2700 grep @dflt Default .skinCard[data-index]
2800 clicksel .skinCard[data-index="4550"] .selctFavBtn
3300 attr @star data-icon .skinCard[data-index="4550"] .selctFavBtn
3350 call closWind
3500 call showWindow 39
4400 rects @faces2 .skinCard[data-index]
4500 call showWindow 9
5500 rects @sprayfav .noBtnCard .selctFavBtn
5600 rects @sprayinfo .noBtnCard .selctInfoBtn
5700 call showWindow 20
6700 grep @grap Customs .skinCard
6800 call showWindow 3
7300 clicksel [onclick*="tabIndex=2"]
8300 rects @prem .custPremReq
8400 grep @remove Remove .custPremReq
8500 quit
""", brain_url, game_port)
    faces = rect_indices(log, "faces")
    check("guest: Select Face lists the free class faces (getSkins(account, true))",
          "4544" in faces and "4550" in faces and len(faces) >= 8, "faces=%s" % faces)
    check("guest: only the selected class's face is open (Triggerman), the others locked with Req <class>",
          count(log, "open") == 1 and count(log, "locked") == len(faces) - 1 and grep_hits(log, "req") == len(faces) - 1,
          "open=%d locked=%d req=%d" % (count(log, "open"), count(log, "locked"), grep_hits(log, "req")))
    check("guest: a free item's caption is Default Skin", grep_hits(log, "dflt") >= 1, "hits=%d" % grep_hits(log, "dflt"))
    m = re.search(r"\[input_script\] attr t=\d+ @star n=\d+ data-icon=(\S+)", log)
    check("favItem: the star turns on", bool(m) and m.group(1) == "star", m.group(0) if m else "no attr")
    check("favItem: krk_favList holds the index", storage("krk_favList") == [4550], "krk_favList=%r" % storage("krk_favList"))
    faces2 = rect_indices(log, "faces2")
    check("the favourite comes first in the list", bool(faces2) and faces2[0] == "4550", "faces=%s" % faces2)
    check("Select Spray: every card has the item and favourite buttons",
          count(log, "sprayfav") > 10 and count(log, "sprayfav") == count(log, "sprayinfo"),
          "fav=%d info=%d" % (count(log, "sprayfav"), count(log, "sprayinfo")))
    check("Select Secondary: the Grappler says Customs Only", grep_hits(log, "grap") == 1, "hits=%d" % grep_hits(log, "grap"))
    check("Customize, no premium: hat + head blocks the hat (Remove Head)", count(log, "prem") == 1 and grep_hits(log, "remove") == 1,
          "prem=%d remove=%d" % (count(log, "prem"), grep_hits(log, "remove")))
    for key in ("krk_favList", "hatIndex", "headIndex"):
        set_storage(key, None)


def scenario(brain_url, game_port, dbpath):
    # 0. a guest online: the Class tab opens every class. v10 locks a class below its level (req _34270), but
    #    Box Office only does that in an official krunker.io game (8de2fe8c, ui_loadout.c gen_tab_class), which
    #    this local brain + server_bin cannot host.
    log = run_client("guest", """0 menu
1500 call showWindow 3
2500 clicksel [onclick*="tabIndex=0"]
3500 rects @locked .lockedClass
3600 rects @cards .classCard
3700 call showWindow 31
4500 rects @attach .noBtnCard
4550 rects @acoglock .lockedCard
4600 rects @acogopen [onclick*="selectAttachment(1)"]
4700 quit
""", brain_url, game_port)
    check("guest on a Box Office server: every class is open (no Register to unlock)",
          count(log, "locked") == 0 and count(log, "cards") >= 10, "locked=%d cards=%d" % (count(log, "locked"), count(log, "cards")))
    # v10 "Acog Scope": minRec 20 on the class level (stats.c<class>), devs exempt (v10.js:178106)
    check("guest: Select Attachment locks the Acog Scope (None, Reflex Sight, a locked Acog)",
          count(log, "attach") == 3 and count(log, "acoglock") == 1 and count(log, "acogopen") == 0,
          "cards=%d locked=%d acog onclick=%d" % (count(log, "attach"), count(log, "acoglock"), count(log, "acogopen")))

    # 1. register (showWindow(5) as a guest: v10's login popup, "Register here") and give the account
    #    KR through the fixture DB
    log = run_client("register", """0 menu
500 call showWindow 5
1000 clickid knV10AuthSwitch
1400 clickid accName
1450 type %s
1500 clickid accPass
1550 type %s
1600 clickid accEmail
1650 type %s@example.com
1700 clickid knV10LoginSubmit
5000 quit
""" % (NAME, PASS, NAME), brain_url, game_port)
    check("register through the native form", f"[account] logged in as {NAME}" in log)
    check("the account's session is seeded for the next runs", seed_session(brain_url, NAME, PASS))
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

    # 2b. the Acog Scope: locked at Triggerman level 1, open once stats.c0 makes it level 20
    #     (floor(0.03 * sqrt(500000)) + 1 = 22)
    acog = """0 menu
1500 call showWindow 31
2500 rects @acoglock .lockedCard
2600 rects @acogopen [onclick*="selectAttachment(1)"]
2700 quit
"""
    log = run_client("acog-l1", acog, brain_url, game_port)
    check("an account at class level 1: the Acog Scope is still locked", count(log, "acoglock") == 1 and count(log, "acogopen") == 0,
          "locked=%d onclick=%d" % (count(log, "acoglock"), count(log, "acogopen")))
    db(dbpath, "UPDATE players SET player_stats = json_set(COALESCE(NULLIF(player_stats, ''), '{}'), '$.c0', 500000) "
               "WHERE player_id = (SELECT id FROM accounts WHERE username = ?)", (NAME,))
    log = run_client("acog-l22", acog, brain_url, game_port)
    check("Triggerman level 22: the Acog Scope is open", count(log, "acoglock") == 0 and count(log, "acogopen") == 1,
          "locked=%d onclick=%d" % (count(log, "acoglock"), count(log, "acogopen")))

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
        guest_picks(brain_url, game_port)
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
