#!/usr/bin/env python3
"""An equipped Krunker Strike skin renders in game (client/src/ui/strike_econ.c, ui_loadout.c,
services/brain/strike_econ.c, server/src/accounts.c's flavored auth):

  kn_brain (sqlite, temp dir) + a brain-linked server_bin --flavor strike + a headless
  client_bin --flavor strike signed in with a saved session:
  1. the account owns one Strike AR skin (strike_items) and nothing in Box Office's inventory
  2. the client loads its Strike state ([strike] account ...) and equips the skin through the page
     handler knStrikeEquip -> POST /strike/v1/players/me/equip (strike_loadout row)
  3. the menu's class preview wears it (cosm / wsel @menu)
  4. after Play the Strike server keeps it (the brain answered its auth with the Strike inventory:
     an unowned Box Office skin would be stripped) and our player's weapon mesh is built with that
     skin (wsel @game: skin=<store index>)
  5. a second account that equipped nothing wears the plain Assault Rifle

  python3 -I client/tests/strike_skin_e2e.py BUILD_DIR      (repository root)
Headless (KRUNKNATIVE_HEADLESS=1, null audio, no DISPLAY, private XDG dirs), ports from
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
WORK = tempfile.mkdtemp(prefix="kn-strike-skin-", dir=CACHE if os.path.isdir(CACHE) else None)
SECRET = "kn-strike-skin-%d" % os.getpid()
STAMP = os.getpid() % 100000
results = []
procs = []


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
    con = sqlite3.connect(path)
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


def ar_skin():
    """the first Assault Rifle skin of the generated catalogue (weapon enum 1)"""
    r = subprocess.run([sys.executable, "-I", os.path.join(ROOT, "tools", "strike", "gen_economy.py"), "--json"], capture_output=True)
    items = json.loads(r.stdout)["items"]
    return next(i for i in items if i["type"] == 0 and i["weapon"] == 1 and i["skin"] >= 0)


def run_client(name, account, script, brain_port, game_port, timeout=150):
    path = os.path.join(WORK, name + ".script")
    with open(path, "w") as f:
        f.write(script)
    cfg = os.path.join(WORK, name + "-cfg")
    folder = os.path.join(cfg, "krunknative")
    os.makedirs(folder, exist_ok=True)
    for key, value in (("krunker_username", account["name"]), ("krunker_token", account["token"]), ("consent", "1")):
        with open(os.path.join(folder, key + ".json"), "w") as f:
            f.write(value)
    log = os.path.join(WORK, name + ".log")
    # through the coordinator, as the Strike app plays: seek-game hands out the Strike lobby and a join
    # ticket, the JOIN carries the session, the Strike server verifies it with kn_brain
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KN_FLAVOR", "KRUNKNATIVE_UI_DIR")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": cfg,
                "XDG_STATE_HOME": os.path.join(WORK, name + "-state"), "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_UPDATES_URL": "off",
                "KRUNKNATIVE_INPUT_SCRIPT": path, "KRUNKNATIVE_WINDOW_SIZE": "1280x720", "KRUNKNATIVE_NET_TRANSPORT": "tcp",
                "KRUNKNATIVE_BRAIN_URL": "http://127.0.0.1:%d" % brain_port, "KRUNKNATIVE_FINDER_URL": "http://127.0.0.1:%d/servers" % brain_port,
                "KRUNKNATIVE_KRUNKER_MATCHMAKER": "off", "KN_FLAVOR": "strike"})
    with open(log, "w") as out:
        p = subprocess.Popen(["stdbuf", "-oL", "-eL", os.path.join(BUILD, "client_bin")],
                             cwd=ROOT, stdout=out, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, start_new_session=True)
        try:
            p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
            check(f"{name}: client_bin finished its script", False, f"timeout, log {log}")
    return read(log)


SCRIPT = """0 menu
{equip}
9000 cosm @menu
9100 wsel @menu
9500 play
16000 cosm @game
16100 wsel @game
18000 quit
"""


def scenario(brain_port, game_port, dbpath, server_log):
    skin = ar_skin()
    stamp = str(STAMP)
    alice = http(brain_port, "/auth/register", {"username": "ksk" + stamp, "password": "strike-skin-" + stamp})
    bob = http(brain_port, "/auth/register", {"username": "ksb" + stamp, "password": "strike-skin-" + stamp})
    a = {"name": "ksk" + stamp, "token": alice.get("token", ""), "id": (alice.get("account") or {}).get("id")}
    b = {"name": "ksb" + stamp, "token": bob.get("token", ""), "id": (bob.get("account") or {}).get("id")}
    check("two accounts registered", bool(a["token"] and b["token"] and a["id"] and b["id"]), "%s %s" % (alice, bob))
    db(dbpath, "INSERT INTO strike_items (account_id, item_id, count) VALUES (?, ?, 1)", (a["id"], skin["id"]))
    check("1. alice owns Strike %s (%s, store skin %d), no Box Office item" % (skin["id"], skin["name"], skin["skin"]),
          db(dbpath, "SELECT count(*) FROM inventory WHERE account_id = ?", (a["id"],))[0][0] == 0)

    log = run_client("alice", a, SCRIPT.format(equip='5000 call knStrikeEquip "%s"' % skin["id"]), brain_port, game_port)
    check("2. the client loaded its Strike account", "[strike] account %d:" % a["id"] in log, log[-600:])
    check("2. knStrikeEquip equipped it on the brain", '[strike] loadout {"w1":"%s"}' % skin["id"] in log
          and db(dbpath, "SELECT item_id FROM strike_loadout WHERE account_id = ? AND slot = 'w1'", (a["id"],)) == [(skin["id"],)],
          str(db(dbpath, "SELECT slot, item_id FROM strike_loadout WHERE account_id = ?", (a["id"],))))
    m = re.search(r"\[input_script\] cosm t=\d+ @menu who=preview .*primary=(-?\d+)", log)
    check("3. the class preview wears it", bool(m) and int(m.group(1)) == skin["skin"], m.group(0) if m else "no cosm @menu")
    m = re.search(r"\[input_script\] cosm t=\d+ @game who=me .*primary=(-?\d+)", log)
    check("4. our spawned player keeps it on the Strike server", bool(m) and int(m.group(1)) == skin["skin"],
          m.group(0) if m else "no cosm @game (not spawned?) " + log[-800:])
    m = re.search(r"\[input_script\] wsel t=\d+ @game who=me slot=0 tp=\d+ w=(\d+) skin=(-?\d+) model=(\S+) tex=(\S+)", log)
    check("4. the first-person weapon is built with the Strike skin", bool(m) and int(m.group(1)) == 1 and int(m.group(2)) == skin["skin"]
          and m.group(4) != "-", m.group(0) if m else "no wsel @game")
    slog = read(server_log)
    check("4. alice joined the Strike server as her account, through the coordinator", re.search(r"\[mm\] joining KSS:\S+ at", log)
          and ("ksk" + stamp) in slog, str([l for l in log.splitlines() if "[mm]" in l][:4]))
    check("4. the Strike server knows the Strike inventory (kn_brain's flavored auth)",
          re.search(r"ksk%s owns 1 skin\(s\) on this network" % stamp, slog) is not None and "ksk%s: removed" % stamp not in slog,
          slog[-600:])

    log = run_client("bob", b, SCRIPT.format(equip="5000 call knStrikeRefresh"), brain_port, game_port)
    m = re.search(r"\[input_script\] wsel t=\d+ @game who=me slot=0 tp=\d+ w=(\d+) skin=(-?\d+)", log)
    check("5. an account without Strike items wears the plain Assault Rifle", bool(m) and int(m.group(2)) == -1,
          m.group(0) if m else "no wsel @game")


def main():
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(1))
    brain_port, game_port = free_ports(2)
    dbpath = os.path.join(WORK, "coordinator.db")
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=SECRET)
    brain_log, server_log = os.path.join(WORK, "brain.log"), os.path.join(WORK, "server.log")
    try:
        procs.append(subprocess.Popen([BRAIN, "--brain.host=127.0.0.1", "--brain.port=%d" % brain_port, "--brain.region=local",
                                       "--brain.id=kss-%d" % os.getpid(), "--accounts.signup_rewards.enabled=false",
                                       "--database.driver=sqlite", "--database.path=" + dbpath, "--migrate",
                                       "--brain.public_url=http://127.0.0.1:%d" % brain_port],
                                      cwd=ROOT, env=env, stdout=open(brain_log, "w"), stderr=subprocess.STDOUT,
                                      stdin=subprocess.DEVNULL, start_new_session=True))
        if not wait_for(lambda: "Strike catalogue:" in read(brain_log), 30):
            check("kn_brain starts with the Strike catalogue", False, read(brain_log)[-600:])
            return
        gs = os.path.join(WORK, "node.json")
        with open(gs, "w") as f:
            json.dump({"gameserver": {"node_id": "KSS", "name": "strike skin e2e", "region": "local", "listen_port": game_port,
                                      "public_host": "127.0.0.1", "brain_url": "ws://127.0.0.1:%d/mm/internal/ws" % brain_port,
                                      "brain_secret": SECRET, "flavor": "strike", "modes": ["ffa"], "max_players": 8, "shutdown_grace": 1,
                                      "idle_kick_seconds": 0}}, f)
        procs.append(subprocess.Popen([os.path.join(BUILD, "server_bin"), "--headless", "--flavor", "strike", "--config", gs,
                                       "--cfg", "none"], cwd=ROOT, stdout=open(server_log, "w"), stderr=subprocess.STDOUT,
                                      stdin=subprocess.DEVNULL, start_new_session=True))
        check("the Strike server links to kn_brain", bool(wait_for(lambda: "welcomed" in read(server_log), 30)), read(server_log)[-400:])
        scenario(brain_port, game_port, dbpath, server_log)
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
