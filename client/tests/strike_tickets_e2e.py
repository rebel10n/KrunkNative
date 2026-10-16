#!/usr/bin/env python3
"""The Krunker Strike buy-tickets popup (client/src/ui/strike_menu.c: Jce / yU in the popup frame) on
kn_brain's Strike economy, end to end through the page:

  kn_brain (sqlite, temp dir) + a headless client_bin --flavor strike --offline signed in with a saved
  session (the account seeded with 1000 KR and no spin tickets):
  1. a KR wheel with no tickets spins (no popup): the spin is sent, the KR drops by the wheel's price
  2. the ticket wheel with no tickets opens the exchange (the spin is NOT sent): the popup holds it
  3. the stepper goes 1 -> 3; the KR button shows 3 x 80 = 240
  4. the KR button buys 3 tickets (POST /shop/purchase, quantity 3): the popup closes, the wallet holds
     3 tickets and 1000 - wheel - 240 KR
  5. the ticket wheel with 3 tickets spins without the popup: the ticket is spent (2 left)
  6. the render scenario tools/uidiff/scenarios/strike/buy-tickets.json renders the exchange

  python3 -I client/tests/strike_tickets_e2e.py BUILD_DIR      (repository root)
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
WORK = tempfile.mkdtemp(prefix="kn-strike-tickets-", dir=CACHE if os.path.isdir(CACHE) else None)
STAMP = os.getpid() % 100000
results = []
procs = []

WHEEL = ".sectioned-scroll:first-child [onclick=\"strikeAction('spin:{}')\"]"
# the wheels in the order the page lists them: KR wheels by price (highest first), then the ticket wheel
SCRIPT = """0 menu
3100 clickid mobMenuStoreH
4000 nodes @wheels .sectioned-scroll:first-child .store-wheel
4100 clicksel {kr_wheel}
4800 nodes @spin .frvr-spin-screen
4900 clickid spinBottomSection
20000 grep @kr-spun {kr_after_spin} .currency-header .krunkies
20050 clicksel [onclick="strikeAction('reward-next')"]
20600 clicksel .frvr-spin-screen .frvr-button-back
20800 clicksel {ticket_wheel}
21500 nodes @spin2 .frvr-spin-screen
21600 clickid spinBottomSection
23000 nodes @popup .popup__holder .buy-tickets
23100 clickid ticketPlus
23500 clickid ticketPlus
24000 grep @amount 3 .store-ticket-exchange .value-input span
24100 grep @price 240 .store-ticket-exchange__buttons .frvr-button-text
24200 clickid ticketBuy
27000 nodes @popup-after .popup__holder .buy-tickets
27100 clickid spinBottomSection
40000 clicksel .frvr-spin-screen .frvr-button-home
41000 quit
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

    stamp = str(STAMP)
    reg = http(brain_port, "/auth/register", {"username": "kst" + stamp, "password": "strike-tickets-" + stamp})
    acc = {"name": "kst" + stamp, "token": reg.get("token", ""), "id": (reg.get("account") or {}).get("id")}
    check("an account registered", bool(acc["token"] and acc["id"]), str(reg))
    aid = acc["id"]
    db(dbpath, "INSERT INTO strike_wallets (account_id, currency, amount) VALUES (?, 'kr', 1000)", (aid,))
    db(dbpath, "INSERT INTO strike_progress (account_id, xp) VALUES (?, 6000)", (aid,))

    script = SCRIPT.format(kr_wheel=WHEEL.format(kr_index), ticket_wheel=WHEEL.format(ticket_index),
                           kr_after_spin=1000 - kr_price)
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

    # the database after each step the client logs (sampled once the brain's reply is stored)
    seen = {}
    marks = [("[strike] account %d:" % aid, "loaded"), ("[strike] spin landed", "landed1"),
             ("[strike] bought tickets_1 x3", "bought"), ("[strike] spin landed", "landed2")]
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
            time.sleep(0.3)  # the brain's reply is in the database before the client logs it
            seen[marks[i][1]] = {"kr": wallet(dbpath, aid, "kr"), "spin_ticket": wallet(dbpath, aid, "spin_ticket")}
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

    check("the Strike account loaded (players/me)", "loaded" in seen, text[-600:])
    check("the Strike wallet loaded with 1000 KR", re.search(r"\[strike\] account %d: level \d+, 1000 KR" % aid, text) is not None,
          text[text.find("[strike] account"):][:120])
    spins = [(m.start(), m.group(0)) for m in re.finditer(r"\[strike\] spin \S+ x\d+", text)]
    bought_at = text.find("[strike] bought tickets_1 x3")
    check("1. a KR wheel with no tickets spins without the popup", "landed1" in seen and seen["landed1"]["kr"] == 1000 - kr_price,
          f"{seen.get('landed1')}")
    check("1. the KR spin is the only spin sent before the purchase",
          [x for _, x in spins if bought_at < 0 or _ < bought_at] == [f"[strike] spin {wheels[kr_index]['id']} x1"], str(spins))
    check("2. the ticket wheel with no tickets opens the exchange", nodes.get("popup") == 1, str(nodes.get("popup")))
    check("3. the stepper goes to 3 tickets", greps.get("amount", 0) >= 1, str(greps.get("amount")))
    check("3. the KR button shows 3 x %d = %d KR" % (ticket_price, 3 * ticket_price), greps.get("price", 0) >= 1, str(greps.get("price")))
    s2 = seen.get("bought", {})
    check("4. the purchase bought 3 tickets for their KR", s2.get("spin_ticket") == 3 and s2.get("kr") == 1000 - kr_price - 3 * ticket_price,
          f"{s2.get('kr')} KR, {s2.get('spin_ticket')} tickets")
    check("4. the popup closed after the purchase", nodes.get("popup-after") == 0, str(nodes.get("popup-after")))
    s3 = seen.get("landed2", {})
    check("5. the ticket wheel with tickets spins without the popup", "landed2" in seen and s3.get("spin_ticket") == 2,
          f"{s3.get('spin_ticket')} tickets left")
    check("no request was refused", "refused" not in text, "; ".join(l for l in text.splitlines() if "refused" in l)[:300])

    # 6. the scenario renders the exchange (a fixture: no account, 1000 KR and no tickets, the stepper at 1)
    scn = os.path.join(WORK, "buy-tickets.png")
    scn_env = {k: v for k, v in env.items() if k != "KRUNKNATIVE_INPUT_SCRIPT"}
    r = subprocess.run([os.path.join(BUILD, "client_bin"), "--render-scenario", "tools/uidiff/scenarios/strike/buy-tickets.json", scn],
                       cwd=ROOT, env=scn_env, capture_output=True, timeout=180)
    rendered = os.path.exists(scn) and os.path.getsize(scn) > 10000
    check("6. the buy-tickets scenario renders", r.returncode == 0 and rendered, r.stderr.decode(errors="replace")[-300:])


def main():
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(1))
    brain_port, = free_ports(1)
    dbpath = os.path.join(WORK, "brain.db")
    brain_log = os.path.join(WORK, "brain.log")
    try:
        procs.append(subprocess.Popen([BRAIN, "--brain.host=127.0.0.1", "--brain.port=%d" % brain_port, "--brain.region=local",
                                       "--brain.id=kst-%d" % os.getpid(), "--accounts.signup_rewards.enabled=false",
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
