#!/usr/bin/env python3
"""Picking one copy of a stacked item (v10.0.7 "<copyId>#<index>" slots, doc/v10/paint-copy.md section 6).

alice owns two copies of the Topo weapon skin (store 7819) with other paints and wears (#ff0000 wear 0.05, #00ff00 wear
0.45) and two copies of the Flip-Flop melee (store 7857, melee id 182: its wearMap ships) with other wears (0.12, 0.71).
Her loadout first wears both as plain indices. Her client then picks the second copy of each, as v10's owned cards do:
selectSkin(7819, 1, undefined, <copy id>) (the weapon card's call, deob-10.0.7.js:220896), and a click on the second
Flip-Flop card of Select Melee, whose onclick is selectMelee("<copy id>#7857") (:223502). The saved
slots must read "<id>#7819" and "<id>#7857" (iîìîiìï, :24381-24383), and when alice and bob join one brain-linked
server_bin lobby the server must dress alice in the second copies (NET_EXT_KN_SKIN_COPY_IDS before her ENT): bob's
client draws alice's Topo with the second copy's paint ("[paint] ... color=00ff00", never the first copy's ff0000) and
logs the second copies' wear levels ("[wear] weapons/..._wear level=0.4500", "[wear] melee/melee_182_wear level=0.7100
map=1"), and alice's own model uses them too.

  python3 -I client/tests/copy_pick_e2e.py BUILD_DIR      (repository root)

kn_brain and server_bin on a scratch sqlite database, ports from kn_proto.free_ports(), the clients headless
(KRUNKNATIVE_HEADLESS=1, null audio, no DISPLAY, private XDG_CONFIG_HOME). No production: no krunker.io.
77 = skip (no client_bin / server_bin / kn_brain, or the sqlite scratch database is unavailable).
"""
import json
import os
import re
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

BUILD, ROOT = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"), os.getcwd()
if len(sys.argv) > 2:
    ROOT = os.path.abspath(sys.argv[2])
CLIENT = os.path.join(BUILD, "client_bin")
SERVER = os.path.join(BUILD, "server_bin")
BRAIN = os.path.join(BUILD, "services", "brain", "kn_brain")
for b in (CLIENT, SERVER, BRAIN):
    if not os.access(b, os.X_OK):
        print("copy_pick_e2e: no %s: skipped" % b)
        sys.exit(77)
sys.path.insert(0, os.path.join(ROOT, "server", "tests"))
from kn_proto import free_ports  # noqa: E402

NAME = "kn_test_copypick_%d" % os.getpid()
SECRET = "kn-copypick-%d" % os.getpid()
PW = "copypick_pass1"
ALICE, BOB = "copypick_alice", "copypick_bob"
TOPO = 7819        # Topo, a weapon skin (weapon 2: selectSkin's weapon 1), paintable
FLIPFLOP = 7857    # Flip-Flop, a melee (id 182: textures/melee/melee_182_wear ships)
WORK = tempfile.mkdtemp(prefix="kn-copypick.", dir=os.path.expanduser("~/.cache"))
FAILS = []
PROCS = []


def check(what, ok, detail=""):
    print(("  ok   " if ok else "  FAIL ") + what + ("" if ok or not detail else "  [%s]" % detail), flush=True)
    if not ok:
        FAILS.append(what)


def read(path):
    try:
        with open(path, errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def wait_for(fn, secs, step=0.3):
    end = time.time() + secs
    while time.time() < end:
        v = fn()
        if v:
            return v
        time.sleep(step)
    return fn()


def http(port, path, body=None, token=None):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request("http://127.0.0.1:%d%s" % (port, path), data=data,
                                 headers={"Content-Type": "application/json", **({"Authorization": "Bearer " + token} if token else {})} if data or token else {},
                                 method="POST" if data else "GET")
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status, json.loads(r.read() or b"null")
    except urllib.error.HTTPError as e:
        try:
            return e.code, json.loads(e.read() or b"null")
        except ValueError:
            return e.code, None


def db_query(path, sql, args=()):
    con = sqlite3.connect(path, timeout=30)
    try:
        con.execute("PRAGMA busy_timeout = 30000")
        rows = con.execute(sql, args).fetchall()
        con.commit()
        return rows
    finally:
        con.close()


def stop_all():
    for p in PROCS:
        if p.poll() is None:
            try:
                p.terminate()
                p.wait(5)
            except subprocess.TimeoutExpired:
                p.kill()


def main():
    scratch = [sys.executable, "-I", os.path.join(ROOT, "services", "brain", "tests", "db_scratch.py"), ROOT]
    r = subprocess.run(scratch + ["create", "sqlite", NAME], capture_output=True)
    if r.returncode:
        print("copy_pick_e2e: no sqlite scratch database: skipped")
        return 77
    db = json.loads(r.stdout)
    dbpath = db["path"]
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=SECRET, **{"KN_DATABASE_" + k.upper(): str(v) for k, v in db.items()})
    try:
        subprocess.run([BRAIN, "--migrate-only"], cwd=ROOT, env=env, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        brain_log = os.path.join(WORK, "brain.log")
        brain = subprocess.Popen([BRAIN, "--brain.host=127.0.0.1", "--brain.port=0", "--brain.region=local",
                                  "--brain.id=copypick-%d" % os.getpid(), "--finder.poll_interval_s=1"],
                                 cwd=ROOT, env=env, stdout=open(brain_log, "w"), stderr=subprocess.STDOUT,
                                 stdin=subprocess.DEVNULL, start_new_session=True)
        PROCS.append(brain)
        m = wait_for(lambda: re.search(r"listening on http://127\.0\.0\.1:(\d+) \(ws", read(brain_log)), 20)
        check("kn_brain starts on an ephemeral port", bool(m), read(brain_log)[-300:])
        if not m:
            return 1
        port = int(m.group(1))

        (node_port,) = free_ports(1)
        node_log = os.path.join(WORK, "node.log")
        node_cfg = os.path.join(WORK, "node.json")
        gs = {"node_id": "COPYPICK", "name": "copy pick e2e", "listen_port": node_port, "public_host": "127.0.0.1",
              "region": "local", "modes": ["ffa"], "max_players": 8, "shutdown_grace": 0, "idle_kick_seconds": 0,
              "ruleset": "v10", "bans_file": os.path.join(WORK, "bans.json"),
              "brain_url": "ws://127.0.0.1:%d/mm/internal/ws" % port, "brain_secret": SECRET,
              "finder_url": "http://127.0.0.1:%d" % port}
        with open(node_cfg, "w") as f:
            json.dump({"gameserver": gs}, f)
        PROCS.append(subprocess.Popen([SERVER, "--headless", "--config", node_cfg, "--cfg", "none"], cwd=ROOT,
                                      stdout=open(node_log, "w"), stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                                      start_new_session=True))
        check("the node links to the brain", bool(wait_for(lambda: "welcomed" in read(node_log) or "linked" in read(node_log), 30)),
              read(node_log)[-300:])

        tokens = {}
        for name in (ALICE, BOB):
            st, reg = http(port, "/auth/register", {"username": name, "password": PW})
            tok = reg.get("token", "") if isinstance(reg, dict) else ""
            tokens[name] = tok
            check("account %s registers" % name, st == 200 and bool(tok), "HTTP %d" % st)
        if not all(tokens.values()):
            return 1
        st, me = http(port, "/auth/me?token=" + tokens[ALICE])
        acc = me.get("account", me) if isinstance(me, dict) else {}
        a = acc.get("id", 0) if isinstance(acc, dict) else 0

        # alice's two copies of each item (skin_copies: the minted instances of migration 0398)
        db_query(dbpath, "INSERT INTO inventory (account_id, skin_id, count) VALUES (?, ?, 2)", (a, TOPO))
        db_query(dbpath, "INSERT INTO inventory (account_id, skin_id, count) VALUES (?, ?, 2)", (a, FLIPFLOP))
        for skin, paint, wear, serial in ((TOPO, "#ff0000", 0.05, 1), (TOPO, "#00ff00", 0.45, 2),
                                          (FLIPFLOP, None, 0.12, 1), (FLIPFLOP, None, 0.71, 2)):
            db_query(dbpath, "INSERT INTO skin_copies (account_id, skin_id, paint_color, wear, serial) VALUES (?, ?, ?, ?, ?)",
                     (a, skin, paint, wear, serial))
        rows = db_query(dbpath, "SELECT copy_id, skin_id FROM skin_copies WHERE account_id = ? ORDER BY copy_id", (a,))
        topo = [c for c, s in rows if s == TOPO]
        flip = [c for c, s in rows if s == FLIPFLOP]
        check("alice has two copies of each item", len(topo) == 2 and len(flip) == 2, repr(rows))
        if len(topo) != 2 or len(flip) != 2:
            return 1

        def client(name, picks):
            cfg = os.path.join(WORK, name + "-cfg")
            os.makedirs(os.path.join(cfg, "krunknative"), exist_ok=True)
            for k, v in (("krunker_username", name), ("krunker_token", tokens[name]), ("knRuleset", "v10")):
                with open(os.path.join(cfg, "krunknative", k + ".json"), "w") as f:
                    f.write(v)
            # the plain indices first (the first copy of each is the plain-index rule's)
            with open(os.path.join(cfg, "krunknative", "skins.json"), "w") as f:
                json.dump({"1": TOPO}, f)
            with open(os.path.join(cfg, "krunknative", "meleeIndex.json"), "w") as f:
                f.write(str(FLIPFLOP))
            cmds = os.path.join(WORK, name + ".cmds")
            with open(cmds, "w") as f:
                f.write("".join(p + "\n" for p in picks))
            return cfg, cmds

        def launch(name, picks, script_lines):
            cfg, cmds = client(name, picks)
            script = os.path.join(WORK, name + ".script")
            with open(script, "w") as f:
                f.write("".join(l + "\n" for l in script_lines))
            log = os.path.join(WORK, name + ".log")
            cenv = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
            cenv.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": cfg,
                         "XDG_STATE_HOME": os.path.join(WORK, name + "-state"), "KRUNKNATIVE_NO_BROWSER": "1",
                         "KRUNKNATIVE_UPDATES_URL": "off", "KRUNKNATIVE_UI_CMDS": cmds, "KRUNKNATIVE_INPUT_SCRIPT": script,
                         "KRUNKNATIVE_WINDOW_SIZE": "1280x720", "KRUNKNATIVE_BRAIN_URL": "http://127.0.0.1:%d" % port,
                         "KRUNKNATIVE_NET_TRANSPORT": "tcp", "KRUNKNATIVE_FINDER_URL": "http://127.0.0.1:%d/servers" % port})
            p = subprocess.Popen(["stdbuf", "-oL", "-eL", CLIENT], cwd=ROOT, stdout=open(log, "w"), stderr=subprocess.STDOUT,
                                 stdin=subprocess.DEVNULL, env=cenv, start_new_session=True)
            PROCS.append(p)
            return cfg, log

        bob_cfg, bob_log = launch(BOB, [], ["0 play"])
        # the weapon card's call, selectSkin(ind, weapon - 1, undefined, id); then a click on the second Flip-Flop card of
        # Select Melee (windows 26: None, Random, then the owned melees by rarity, Flip-Flop's two copies first), whose
        # onclick is selectMelee("<id>#<ind>"); the menu clock lets them land before the game
        picks = ["ui selectSkin(%d,1,undefined,%d)" % (TOPO, topo[1]), "ui showWindow(26)", "clicksel 3 .skinCard"]
        alice_cfg, alice_log = launch(ALICE, picks, ["0 menu", "6000 play"])

        store = os.path.join(alice_cfg, "krunknative")
        want_skins = {"1": "%d#%d" % (topo[1], TOPO)}
        got = wait_for(lambda: read(os.path.join(store, "skins.json")) if '"%d#' % topo[1] in read(os.path.join(store, "skins.json")) else "", 90, 1.0)
        try:
            saved = json.loads(got) if got else None
        except ValueError:
            saved = None
        check("the weapon slot saves the copy as \"<id>#<index>\"", saved == want_skins, repr(got))
        melee = wait_for(lambda: read(os.path.join(store, "meleeIndex.json")) if "#" in read(os.path.join(store, "meleeIndex.json")) else "", 30, 1.0)
        check("the melee slot saves the copy as \"<id>#<index>\"", melee == "%d#%d" % (flip[1], FLIPFLOP), repr(melee))

        second = r"\[paint\] mesh enabled=1 shader=-?\d+ color=00ff00"
        first = r"\[paint\] mesh enabled=1 shader=-?\d+ color=ff0000"
        # Topo has no mid: v10's key is weapons/weapon_2_wear (deob-10.0.7.js:174185), one of the weapon wear maps
        # krunker.io's asset host has (tools/ui-import/live_assets_v10.lock), so map=1: the wear is drawn
        weapon_wear = r"\[wear\] weapons/weapon_2_wear level=0\.4500 map=1"
        melee_wear = r"\[wear\] melee/melee_182_wear level=0\.7100 map=1"
        ok = wait_for(lambda: re.search(second, read(bob_log)) and re.search(melee_wear, read(bob_log)), 150, 1.0)
        bl = read(bob_log)
        check("bob draws alice's Topo with the second copy's paint (color=00ff00)", bool(re.search(second, bl)), bl[-400:])
        check("bob never draws the first copy's paint (color=ff0000)", not re.search(first, bl))
        check("bob's client gives alice's Topo the second copy's wear (weapon_2_wear level 0.4500, map=1)", bool(re.search(weapon_wear, bl)),
              "\n".join(l for l in bl.splitlines() if l.startswith("[wear]"))[-400:])
        check("bob's client gives alice's melee the second copy's wear (melee_182_wear level 0.7100, map=1)", bool(re.search(melee_wear, bl)),
              "\n".join(l for l in bl.splitlines() if l.startswith("[wear]"))[-400:])
        al = wait_for(lambda: read(alice_log) if re.search(weapon_wear, read(alice_log)) else "", 30, 1.0) or read(alice_log)
        check("alice's own model wears the second Topo copy's paint and wear", bool(re.search(second, al)) and bool(re.search(weapon_wear, al)),
              "\n".join(l for l in al.splitlines() if l.startswith("[wear]") or l.startswith("[paint]"))[-400:])
        check("the two clients joined the same server", "connected to server" in bl and "connected to server" in al, bl[-200:])
        if not ok:
            print(read(node_log)[-600:])
    finally:
        stop_all()
        subprocess.run(scratch + ["drop", "sqlite", NAME], capture_output=True)
    print("copy_pick_e2e: " + ("OK" if not FAILS else "%d failure(s), logs in %s" % (len(FAILS), WORK)))
    if not FAILS:
        shutil.rmtree(WORK, ignore_errors=True)
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
