#!/usr/bin/env python3
"""The Krunker Strike screens on kn_brain's Strike economy (client/src/ui/strike_menu.c, strike_meta.c on
client/src/ui/strike_econ.c, services/brain/strike_econ.c), end to end through the page:

  kn_brain (sqlite, temp dir) + a headless client_bin --flavor strike --offline signed in with a saved
  session (the account seeded with 1000 KR, a Strike level past the missions unlock and one owned
  Assault Rifle skin):
  1. the account box shows the Strike wallet (1000 KR)
  2. Extras: the Spins section lists the catalogue's wheels and the shop's products load (GET /shop)
  3. a KR wheel, 1x: POST /spin-wheels/:id/spin; the wheel turns onto the result ("spin landed"), the
     wallet drops by the price and the won item is in strike_items; the header shows the new KR
  4. Currency -> "1 Spin Ticket": POST /shop/purchase; 80 KR become a spin ticket
  5. the ticket wheel, 1x: the ticket is spent
  6. Customize -> Weapon -> an owned skin: POST /players/me/equip (strike_loadout w<weapon>); the box
     label follows; Default: the slot is unequipped
  7. a leaderboard (GET /leaderboards/xp) in the popup frame, our row --player
  8. the Missions tab lists today's missions (GET /missions/me); one completed in the database is
     claimed from its card (POST /missions/claim/:id): claimed, its KR paid
  9. Profile's Rewards Track (GET /game-config): Claim Rewards claims every reached level
     (POST /players/me/claim/progression)

  python3 -I client/tests/strike_econ_ui_e2e.py BUILD_DIR      (repository root)
Headless (KRUNKNATIVE_HEADLESS=1, null audio, no DISPLAY, private XDG dirs), the brain's port from
kn_proto.free_ports(). 77 = skip (no kn_brain / client_bin).
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
import threading
import time
import urllib.request

ROOT = os.getcwd()
BUILD = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug")
BRAIN = os.path.join(BUILD, "services", "brain", "kn_brain")
sys.path.insert(0, os.path.join(ROOT, "server", "tests"))
if not (os.access(BRAIN, os.X_OK) and os.access(os.path.join(BUILD, "client_bin"), os.X_OK)):
    print("needs kn_brain and client_bin: skip")
    sys.exit(77)
from kn_proto import free_ports  # noqa: E402

CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-strike-econ-ui-", dir=CACHE if os.path.isdir(CACHE) else None)
STAMP = os.getpid() % 100000
results = []
procs = []

SPIN0 = ".sectioned-scroll:first-child [onclick=\"strikeAction('spin:{}')\"]"
SCRIPT = """0 menu
3000 grep @kr0 1000 .account-currency
3100 clickid mobMenuStoreH
4000 nodes @wheels .sectioned-scroll:first-child .store-wheel
4000 nodes @products .sectioned-scroll:first-child .store-product
4100 clicksel {kr_wheel}
4800 nodes @spin .frvr-spin-screen
4900 clickid spinBottomSection
24000 grep @kr-spun {kr_after_spin} .currency-header .krunkies
24100 clicksel .frvr-spin-screen .frvr-button-back
24800 clicksel .sectioned-scroll:first-child [onclick="strikeAction('store-section:2')"]
25500 clicksel .sectioned-scroll:first-child [data-product="tickets_1"]
26300 grep @kr-bought {kr_after_buy} .currency-header .krunkies
26400 clicksel {ticket_wheel}
27100 nodes @spin2 .frvr-spin-screen
27200 clickid spinBottomSection
46000 clicksel .frvr-spin-screen .frvr-button-home
46700 clicksel [onclick="strikeAction('customize')"]
47400 clicksel [onclick="strikeAction('item-slot:0')"]
48100 nodes @squares .item-selection__item-square
48100 nodes @selected-default .item-selection__item-square--selected:last-child
48200 clicksel .item-selection__item-square[onclick="strikeAction('item-pick:0')"]
49000 nodes @selected-first .item-selection__item-square--selected:first-child
49100 clicksel .item-selection__item-square:last-child
49900 nodes @selected-back .item-selection__item-square--selected:last-child
50000 clicksel .frvr-button-back
50700 grep @weapon-default Default .customization-screen__cat-box
50800 call strikeAction "leaderboard:xp"
51600 nodes @board .popup__holder .frvr-lboard-end-screen__row-wrapper
51600 nodes @board-you .popup__holder .frvr-lboard-end-screen__player-name--player
51700 click 1270 710
52300 call strikeAction "home"
52800 call strikeAction "missions"
56000 call strikeAction "bp-tab:0"
56300 call strikeAction "bp-tab:1"
57500 nodes @cards .frvr-mission-card
57500 nodes @claimable .frvr-mission-card[onclick]
57500 rects @cardrects .frvr-mission-card
57600 clicksel .frvr-bp-missions-tab__missions-list > .frvr-mission-card[onclick]:first-child
58600 call strikeAction "home"
59000 call strikeAction "profile"
61500 nodes @track .frvr-progression-track-rewards__column
61600 clickid claimrewards
64000 quit
"""


def check(name, ok, detail=""):
    results.append(bool(ok))
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def read(path):
    try:
        with open(path, errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def wait_for(fn, secs):
    end = time.time() + secs
    while time.time() < end:
        v = fn()
        if v:
            return v
        time.sleep(0.2)
    return None


def db(path, sql, args=()):
    con = sqlite3.connect(path, timeout=10)
    try:
        rows = con.execute(sql, args).fetchall()
        con.commit()
        return rows
    finally:
        con.close()


def http(port, path, body):
    req = urllib.request.Request("http://127.0.0.1:%d%s" % (port, path), data=json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=10) as r:
        return json.loads(r.read())


def catalogue():
    r = subprocess.run([sys.executable, "-I", os.path.join(ROOT, "tools", "strike", "gen_economy.py"), "--json"], capture_output=True)
    return json.loads(r.stdout)


def wallet(dbpath, aid, cur):
    rows = db(dbpath, "SELECT amount FROM strike_wallets WHERE account_id = ? AND currency = ?", (aid, cur))
    return rows[0][0] if rows else 0


def main_scenario(brain_port, dbpath):
    cat = catalogue()
    wheels = [w for w in cat["wheels"] if w.get("currency") in ("kr", "spin_ticket")]
    wheels.sort(key=lambda w: (w["currency"] == "spin_ticket", -w["amount"]))
    kr_index = next(i for i, w in enumerate(wheels) if w["currency"] == "kr")
    ticket_index = next(i for i, w in enumerate(wheels) if w["currency"] == "spin_ticket")
    kr_price = wheels[kr_index]["amount"]
    ticket_price = next(p for p in cat["products"] if p["id"] == "tickets_1")["amount"]
    skin = next(i for i in cat["items"] if i["type"] == 0 and i["weapon"] == 1 and i["skin"] >= 0)

    stamp = str(STAMP)
    reg = http(brain_port, "/auth/register", {"username": "kse" + stamp, "password": "strike-econ-" + stamp})
    acc = {"name": "kse" + stamp, "token": reg.get("token", ""), "id": (reg.get("account") or {}).get("id")}
    check("an account registered", bool(acc["token"] and acc["id"]), str(reg))
    aid = acc["id"]
    db(dbpath, "INSERT INTO strike_wallets (account_id, currency, amount) VALUES (?, 'kr', 1000)", (aid,))
    db(dbpath, "INSERT INTO strike_progress (account_id, xp) VALUES (?, 6000)", (aid,))
    db(dbpath, "INSERT INTO strike_items (account_id, item_id, count) VALUES (?, ?, 1)", (aid, skin["id"]))

    script = SCRIPT.format(kr_wheel=SPIN0.format(kr_index), ticket_wheel=SPIN0.format(ticket_index),
                           kr_after_spin=1000 - kr_price, kr_after_buy=1000 - kr_price - ticket_price)
    path = os.path.join(WORK, "client.script")
    with open(path, "w") as f:
        f.write(script)
    cfg = os.path.join(WORK, "cfg")
    folder = os.path.join(cfg, "krunknative")
    os.makedirs(folder, exist_ok=True)
    for key, value in (("krunker_username", acc["name"]), ("krunker_token", acc["token"]), ("consent", "1")):
        with open(os.path.join(folder, key + ".json"), "w") as f:
            f.write(value)
    log = os.path.join(WORK, "client.log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KN_FLAVOR", "KRUNKNATIVE_UI_DIR")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": cfg,
                "XDG_STATE_HOME": os.path.join(WORK, "state"), "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_UPDATES_URL": "off",
                "KRUNKNATIVE_INPUT_SCRIPT": path, "KRUNKNATIVE_WINDOW_SIZE": "1280x720",
                "KRUNKNATIVE_BRAIN_URL": "http://127.0.0.1:%d" % brain_port, "KRUNKNATIVE_KRUNKER_MATCHMAKER": "off", "KN_FLAVOR": "strike"})

    # the database between the steps, keyed by what the client logs (sampled as each line appears)
    seen = {}
    marks = [("[strike] account %d:" % aid, "loaded"), ("[strike] spin landed", "landed1"), ("[strike] bought tickets_1", "bought"),
             ("[strike] spin landed", "landed2"), ("[strike] equipped", "equip1"), ("[strike] equipped", "equip2"),
             ("[strike] missions:", "missions"), ("[strike] claimed a mission", "claimed"), ("[strike] Rewards Track: claiming", "track")]
    stop = threading.Event()

    def watcher():
        pos, i = 0, 0
        while not stop.is_set() and i < len(marks):
            text = read(log)
            k = text.find(marks[i][0], pos)
            if k < 0:
                time.sleep(0.1)
                continue
            pos = k + len(marks[i][0])
            label = marks[i][1]
            time.sleep(0.3)  # the brain's reply is in the database before the client logs it
            seen[label] = {"kr": wallet(dbpath, aid, "kr"), "spin_ticket": wallet(dbpath, aid, "spin_ticket"),
                           "items": db(dbpath, "SELECT item_id, count FROM strike_items WHERE account_id = ?", (aid,)),
                           "loadout": db(dbpath, "SELECT slot, item_id FROM strike_loadout WHERE account_id = ?", (aid,))}
            if label == "missions":  # today's missions exist now: complete them all
                db(dbpath, "UPDATE strike_daily_missions SET completed = 1, progress = 999 WHERE account_id = ?", (aid,))
            i += 1

    t = threading.Thread(target=watcher, daemon=True)
    t.start()
    with open(log, "w") as out:
        p = subprocess.Popen(["stdbuf", "-oL", "-eL", os.path.join(BUILD, "client_bin"), "--offline", "--map", "kanji"],
                             cwd=ROOT, stdout=out, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, start_new_session=True)
        try:
            p.wait(timeout=240)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
            check("client_bin finished its script", False, f"timeout, log {log}")
    stop.set()
    t.join(2)
    text = read(log)
    nodes, greps = {}, {}
    for m in re.finditer(r"\[input_script\] nodes t=\d+ @(\S+) elements=\d+ match=(\d+)", text):
        nodes[m.group(1)] = int(m.group(2))
    for m in re.finditer(r"\[input_script\] grep t=\d+ @(\S+) needle=\S+ n=\d+ hits=(\d+)", text):
        greps[m.group(1)] = int(m.group(2))

    check("1. the Strike account loaded (players/me)", "loaded" in seen, text[-600:])
    check("1. the account box shows the Strike wallet's 1000 KR", greps.get("kr0", 0) >= 1, str(greps.get("kr0")))
    check("2. the Spins section lists the catalogue's shop wheels", nodes.get("wheels") == len(wheels), f"{nodes.get('wheels')} vs {len(wheels)}")
    check("2. the shop's products load into Market / Currency", (nodes.get("products") or 0) >= 4, str(nodes.get("products")))
    check("3. a wheel block opens SPIN", nodes.get("spin") == 1)
    s1 = seen.get("landed1", {})
    check("3. the 1x spin landed on the brain's result", "landed1" in seen and re.search(r"\[strike\] spin result 0: pool \S+ rand", text) is not None)
    check("3. the spin cost the wheel's price", s1.get("kr") == 1000 - kr_price, f"{s1.get('kr')} (want {1000 - kr_price})")
    check("3. the won item is the account's", len(s1.get("items", [])) >= 2 or any(c > 1 for _, c in s1.get("items", [])), str(s1.get("items")))
    check("3. the Store header shows the new KR", greps.get("kr-spun", 0) >= 1, str(greps.get("kr-spun")))
    s2 = seen.get("bought", {})
    check("4. \"1 Spin Ticket\": 80 KR became a spin ticket", s2.get("spin_ticket") == 1 and s2.get("kr") == 1000 - kr_price - ticket_price,
          f"{s2.get('kr')} KR, {s2.get('spin_ticket')} tickets")
    check("4. the Store header follows the purchase", greps.get("kr-bought", 0) >= 1, str(greps.get("kr-bought")))
    s3 = seen.get("landed2", {})
    check("5. the ticket wheel spent the ticket", nodes.get("spin2") == 1 and s3.get("spin_ticket") == 0, f"{s3.get('spin_ticket')}")
    check("6. Weapon's items: the owned skin(s) and Default", (nodes.get("squares") or 0) >= 2 and nodes.get("selected-default") == 1,
          f"{nodes.get('squares')} squares, default selected {nodes.get('selected-default')}")
    e1, e2 = seen.get("equip1", {}), seen.get("equip2", {})
    w_rows = [r for r in e1.get("loadout", []) if r[0].startswith("w")]
    check("6. picking a skin equips it (strike_loadout)", len(w_rows) == 1 and nodes.get("selected-first") == 1, f"{e1.get('loadout')}")
    check("6. Default unequips the slot", not [r for r in e2.get("loadout", []) if r[0].startswith("w")] and nodes.get("selected-back") == 1,
          f"{e2.get('loadout')}")
    check("6. the Weapon box shows Default again", greps.get("weapon-default", 0) >= 1, str(greps.get("weapon-default")))
    check("7. the xp leaderboard opens in the popup frame", (nodes.get("board") or 0) >= 2 and nodes.get("board-you") == 1,
          f"{nodes.get('board')} rows, you {nodes.get('board-you')}")
    check("8. the Missions tab lists today's missions", (nodes.get("cards") or 0) >= 1 and "missions" in seen, str(nodes.get("cards")))
    check("8. completed missions are claimable from their cards", (nodes.get("claimable") or 0) >= 1, str(nodes.get("claimable")))
    claimed = db(dbpath, "SELECT count(*) FROM strike_daily_missions WHERE account_id = ? AND claimed = 1", (aid,))[0][0]
    sc = seen.get("claimed", {})
    check("8. a card's click claimed its mission and paid KR", claimed == 1 and sc.get("kr", 0) > s3.get("kr", 0), f"{claimed} claimed, {s3.get('kr')} -> {sc.get('kr')}")
    m = re.search(r"\[strike\] PROGRESSION_TRACK (\d+) levels, level (\d+), (\d+) claimable", text)
    check("9. Profile shows the Rewards Track from the game config", m is not None and int(m.group(1)) > 0 and (nodes.get("track") or 0) > 0,
          m.group(0) if m else "no track")
    want = int(m.group(3)) if m else -1
    time.sleep(0.5)
    rows = db(dbpath, "SELECT count(*) FROM strike_track_claims WHERE account_id = ?", (aid,))[0][0]
    check("9. Claim Rewards claimed every reached level", want > 0 and rows == want, f"{rows} claims, {want} claimable")
    check("no request was refused", "refused" not in text, "; ".join(l for l in text.splitlines() if "refused" in l)[:300])


def main():
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(1))
    brain_port, = free_ports(1)
    dbpath = os.path.join(WORK, "brain.db")
    brain_log = os.path.join(WORK, "brain.log")
    try:
        procs.append(subprocess.Popen([BRAIN, "--brain.host=127.0.0.1", "--brain.port=%d" % brain_port, "--brain.region=local",
                                       "--brain.id=kse-%d" % os.getpid(), "--accounts.signup_rewards.enabled=false",
                                       "--database.driver=sqlite", "--database.path=" + dbpath, "--migrate",
                                       "--brain.public_url=http://127.0.0.1:%d" % brain_port],
                                      cwd=ROOT, stdout=open(brain_log, "w"), stderr=subprocess.STDOUT,
                                      stdin=subprocess.DEVNULL, start_new_session=True))
        if not wait_for(lambda: "Strike catalogue:" in read(brain_log), 30):
            check("kn_brain starts with the Strike catalogue", False, read(brain_log)[-600:])
            return
        main_scenario(brain_port, dbpath)
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
    ok = all(results) and results
    if ok:
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print("logs kept in " + WORK)
    print(f"{sum(results)}/{len(results)} passed")
    sys.exit(0 if ok else 1)


main()
