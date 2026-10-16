#!/usr/bin/env python3
"""The Krunker Strike golden spin (the shop's front SpinWheel section, FRVR's Lucky Draw golden block and
the golden screen; services/brain/strike_econ.c front_json, client/src/ui/strike_menu.c golden_*), end to end:

  kn_brain (sqlite, temp dir) + a headless client_bin --flavor strike --offline signed in with a saved
  session (1000 KR, one golden spin ticket, the golden meter at 4 of 10):
  1. the Spins section shows the golden block from the shop's front (#luckyDrawGolden), its meter reads
     4/10, and the golden wheel is not one of the Spins list's blocks (the list has the shop wheels only)
  2. the golden block's button opens the golden screen: the wheel is "Golden", its meter is on the screen
  3. the 1x golden spin: POST /spin-wheels/golden_spin/spin; the wallet drops by one golden ticket
  4. back in the store with no golden ticket, the block's button buys one (POST /shop/purchase of the
     front's golden product, 500 KR) and the wallet holds one golden ticket again

  python3 -I client/tests/strike_golden_spin_e2e.py BUILD_DIR      (repository root)
Headless (KRUNKNATIVE_HEADLESS=1, null audio, no DISPLAY, private XDG dirs). 77 = skip (no kn_brain / client_bin).
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
WORK = tempfile.mkdtemp(prefix="kn-strike-golden-", dir=CACHE if os.path.isdir(CACHE) else None)
STAMP = os.getpid() % 100000
results = []
procs = []

SCRIPT = """0 menu
3100 clickid mobMenuStoreH
4000 nodes @golden .sectioned-scroll:first-child [id="luckyDrawGolden"]
4000 nodes @wheels .sectioned-scroll:first-child .store-wheel
4000 grep @meter 4/10 .golden-spin-meter__value
4100 clicksel .sectioned-scroll:first-child [id="luckyDrawGolden"] .golden-spin-block__button
4800 nodes @spin .frvr-spin-screen
4850 nodes @spin-meter .frvr-spin-screen .golden-spin-meter
4900 grep @title Golden .spinner-title
4950 clickid spinBottomSection
24000 clicksel .frvr-spin-screen .frvr-button-back
24800 nodes @buy .sectioned-scroll:first-child [id="luckyDrawGolden"] .golden-spin-block__button
24900 clicksel .sectioned-scroll:first-child [id="luckyDrawGolden"] .golden-spin-block__button
26500 quit
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
    golden = next(w for w in cat["wheels"] if w.get("currency") == "spin_golden_ticket")
    golden_product = next(p for p in cat["products"] if p["id"] == "golden_ticket_1")

    stamp = str(STAMP)
    reg = http(brain_port, "/auth/register", {"username": "kgs" + stamp, "password": "strike-golden-" + stamp})
    acc = {"name": "kgs" + stamp, "token": reg.get("token", ""), "id": (reg.get("account") or {}).get("id")}
    check("an account registered", bool(acc["token"] and acc["id"]), str(reg))
    aid = acc["id"]
    db(dbpath, "INSERT INTO strike_wallets (account_id, currency, amount) VALUES (?, 'kr', 1000)", (aid,))
    db(dbpath, "INSERT INTO strike_wallets (account_id, currency, amount) VALUES (?, 'spin_golden_ticket', 1)", (aid,))
    db(dbpath, "INSERT INTO strike_progress (account_id, xp, golden_spin_count) VALUES (?, 6000, 4)", (aid,))

    cfg = os.path.join(WORK, "cfg")
    folder = os.path.join(cfg, "krunknative")
    os.makedirs(folder, exist_ok=True)
    for key, value in (("krunker_username", acc["name"]), ("krunker_token", acc["token"]), ("consent", "1")):
        with open(os.path.join(folder, key + ".json"), "w") as f:
            f.write(value)
    path = os.path.join(WORK, "client.script")
    with open(path, "w") as f:
        f.write(SCRIPT)
    log = os.path.join(WORK, "client.log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KN_FLAVOR", "KRUNKNATIVE_UI_DIR")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": cfg,
                "XDG_STATE_HOME": os.path.join(WORK, "state"), "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_UPDATES_URL": "off",
                "KRUNKNATIVE_INPUT_SCRIPT": path, "KRUNKNATIVE_WINDOW_SIZE": "1280x720",
                "KRUNKNATIVE_BRAIN_URL": "http://127.0.0.1:%d" % brain_port, "KRUNKNATIVE_KRUNKER_MATCHMAKER": "off", "KN_FLAVOR": "strike"})
    with open(log, "w") as out:
        p = subprocess.Popen(["stdbuf", "-oL", "-eL", os.path.join(BUILD, "client_bin"), "--offline", "--map", "kanji"],
                             cwd=ROOT, stdout=out, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, start_new_session=True)
        try:
            p.wait(timeout=240)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
            check("client_bin finished its script", False, f"timeout, log {log}")
    text = read(log)
    nodes, greps = {}, {}
    for m in re.finditer(r"\[input_script\] nodes t=\d+ @(\S+) elements=\d+ match=(\d+)", text):
        nodes[m.group(1)] = int(m.group(2))
    for m in re.finditer(r"\[input_script\] grep t=\d+ @(\S+) needle=\S+ n=\d+ hits=(\d+)", text):
        greps[m.group(1)] = int(m.group(2))

    check("1. the golden block comes from the shop's front", nodes.get("golden") == 1, str(nodes.get("golden")))
    check("1. the Spins list shows the shop's wheels, not the golden wheel", nodes.get("wheels") == len(wheels),
          f"{nodes.get('wheels')} vs {len(wheels)}")
    check("1. the meter reads 4/10", greps.get("meter", 0) >= 1, str(greps.get("meter")))
    check("2. the block's button opens the golden screen", nodes.get("spin") == 1, str(nodes.get("spin")))
    check("2. the golden screen shows the golden meter", nodes.get("spin-meter") == 1, str(nodes.get("spin-meter")))
    check("2. the screen is the golden wheel", greps.get("title", 0) >= 1, str(greps.get("title")))
    # the client's wallet after each refresh (its account line: "... N golden"): one ticket at the start, none after the spin
    golden_seen = [int(v) for v in re.findall(r"\[strike\] account \d+: level \d+, \d+ KR, \d+ spin tickets, (\d+) golden", text)]
    spent = golden_seen[:1] == [1] and 0 in golden_seen[1:]
    m = re.search(r"\[strike\] spin golden_spin x1", text)
    check("3. the golden spin was sent and landed", m is not None and re.search(r"\[strike\] spin result 0: pool", text) is not None,
          "no spin line" if m is None else "")
    check("3. the golden spin cost the one golden ticket", spent, str(golden_seen))
    check("4. back in the store with no ticket, the block offers the purchase", (nodes.get("buy") or 0) >= 1, str(nodes.get("buy")))
    bought = "[strike] bought golden_ticket_1" in text
    kr = wallet(dbpath, aid, "kr")
    gt = db(dbpath, "SELECT amount FROM strike_wallets WHERE account_id = ? AND currency = 'spin_golden_ticket'", (aid,))
    check("4. the purchase paid 500 KR for a golden spin ticket", bought and kr == 1000 - golden_product["amount"] and gt and gt[0][0] == 1,
          f"KR {kr}, tickets {gt}")
    check("no request was refused", "refused" not in text, "; ".join(l for l in text.splitlines() if "refused" in l)[:300])


def main():
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(1))
    brain_port, = free_ports(1)
    dbpath = os.path.join(WORK, "brain.db")
    brain_log = os.path.join(WORK, "brain.log")
    try:
        procs.append(subprocess.Popen([BRAIN, "--brain.host=127.0.0.1", "--brain.port=%d" % brain_port, "--brain.region=local",
                                       "--brain.id=kgs-%d" % os.getpid(), "--accounts.signup_rewards.enabled=false",
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
