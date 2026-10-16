#!/usr/bin/env python3
"""A battle pass reward from the brain, shown and opened by the client (strike_reward.c, strike_meta.c,
services/brain/db_strike.c, services/brain/strike_econ.c), end to end through the page:

  kn_brain (sqlite, temp dir) + a headless client_bin --flavor strike --offline signed in with a saved
  session (a fresh Strike account: level 1, no wallet). The catalogue's level 1 track slot (1_00) is
  configured as a Battle Pass ticket: its reward is {type: battlepass}, no pick-one (a strike_track row,
  reloaded by the brain every second):
  1. Profile's Rewards Track shows that level; Claim Rewards claims it (POST /players/me/claim/progression)
  2. the brain grants nothing for the ticket (no wallet row) and records the claim
  3. the reward screen (REWARD) shows the ticket (reward-image--bp) and its button reads Awesome!
  4. Awesome! ends the reward and opens BATTLEPASS_SCREEN (FRVR's kr(_C))

  python3 -I client/tests/strike_bp_source_e2e.py BUILD_DIR      (repository root)
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
WORK = tempfile.mkdtemp(prefix="kn-strike-bp-source-", dir=CACHE if os.path.isdir(CACHE) else None)
STAMP = os.getpid() % 100000
results = []
procs = []

SCRIPT = """0 menu
3000 call strikeAction "profile"
9000 nodes @track .frvr-progression-track-rewards__column
9100 clickid claimrewards
13000 nodes @bp-reward .frvr-reward-screen .reward-image--bp
13000 text @bp-title .frvr-reward-screen .reward-title
13000 text @bp-button .frvr-reward-screen .frvr-button-text
13100 clicksel .frvr-reward-screen .buttons .frvr-button
16000 nodes @bp-screen .frvr-bp-screen
16000 nodes @bp-reward-gone .frvr-reward-screen
16200 quit
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


def scenario(brain_port, dbpath):
    stamp = str(STAMP)
    reg = http(brain_port, "/auth/register", {"username": "kbp" + stamp, "password": "strike-bp-" + stamp})
    acc = {"name": "kbp" + stamp, "token": reg.get("token", ""), "id": (reg.get("account") or {}).get("id")}
    check("an account registered", bool(acc["token"] and acc["id"]), str(reg))
    aid = acc["id"]
    db(dbpath, "INSERT INTO strike_progress (account_id, xp) VALUES (?, 0)", (aid,))
    # the level 1 track slot is the Battle Pass ticket: a configured catalogue entry (no pick-one)
    db(dbpath, "UPDATE strike_track SET rewards = ?, player_choice = 0 WHERE slot_id = '1_00'",
       ('[{"type": "battlepass"}]',))
    time.sleep(3.5)  # the brain reloads its catalogue every 1.5 s (--accounts.store_reload_ms=1500)

    path = os.path.join(WORK, "client.script")
    with open(path, "w") as f:
        f.write(SCRIPT)
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
                "KRUNKNATIVE_INPUT_SCRIPT": path, "KRUNKNATIVE_WINDOW_SIZE": "1280x720", "KRUNKNATIVE_FIXED_DELTA": "16.667",
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
    texts = {m.group(1): m.group(2) for m in re.finditer(r"\[input_script\] text t=\d+ @(\S+) n=\d+ checked=\d+ value=\S* text=(.*)", text)}

    check("the Profile's Rewards Track shows its levels", (nodes.get("track") or 0) >= 1, str(nodes.get("track")))
    bp_text = text.split("[strike] REWARD battlepass", 1)[-1]
    check("Claim Rewards claims the battle pass ticket and shows its reward screen", "[strike] REWARD battlepass" in text
          and nodes.get("bp-reward") == 1, str(nodes.get("bp-reward")))
    check("the ticket's reward screen says Awesome!", texts.get("bp-button") == "Awesome!", repr(texts.get("bp-button")))
    check("the ticket's reward screen has no title (FRVR's battlepass reward)", texts.get("bp-title", "") == "", repr(texts.get("bp-title")))
    claims = db(dbpath, "SELECT slot_id FROM strike_track_claims WHERE account_id = ?", (aid,))
    check("the brain recorded the claim of slot 1_00", claims == [("1_00",)], str(claims))
    wallet = db(dbpath, "SELECT currency, amount FROM strike_wallets WHERE account_id = ?", (aid,))
    check("the ticket moves no wallet (the pass is the Battle Pass system's)", not wallet or all(a == 0 for _, a in wallet), str(wallet))
    check("Awesome! opens BATTLEPASS_SCREEN and ends the reward", "[strike] screen BATTLEPASS_SCREEN" in bp_text
          and nodes.get("bp-screen") == 1 and nodes.get("bp-reward-gone") == 0,
          f"screen nodes {nodes.get('bp-screen')}, reward left {nodes.get('bp-reward-gone')}")
    check("no request was refused", "refused" not in text, "; ".join(l for l in text.splitlines() if "refused" in l)[:300])
    missing = [l for l in text.splitlines() if "file not found" in l and "img/" in l]
    check("the ticket art is shipped (no \"file not found\" for img/)", not missing, "; ".join(missing)[:300])


def main():
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(1))
    brain_port, = free_ports(1)
    dbpath = os.path.join(WORK, "brain.db")
    brain_log = os.path.join(WORK, "brain.log")
    try:
        procs.append(subprocess.Popen([BRAIN, "--brain.host=127.0.0.1", "--brain.port=%d" % brain_port, "--brain.region=local",
                                       "--brain.id=kbp-%d" % os.getpid(), "--accounts.signup_rewards.enabled=false",
                                       "--accounts.store_reload_ms=1500",
                                       "--database.driver=sqlite", "--database.path=" + dbpath, "--migrate",
                                       "--brain.public_url=http://127.0.0.1:%d" % brain_port],
                                      cwd=ROOT, stdout=open(brain_log, "w"), stderr=subprocess.STDOUT,
                                      stdin=subprocess.DEVNULL, start_new_session=True))
        if not wait_for(lambda: "Strike catalogue:" in read(brain_log), 30):
            check("kn_brain starts with the Strike catalogue", False, read(brain_log)[-600:])
            return
        scenario(brain_port, dbpath)
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
