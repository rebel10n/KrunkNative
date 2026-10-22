#!/usr/bin/env python3
"""A shader paint on another player, two clients (client/src/skin_fx.c, basic.frag paint node).

alice owns the Topo weapon skin (store 7819, paintTargets allowed) and the Spectrum paint (paintShader 0, store
7795). She paints the skin through the brain (POST /items/7819/paint): one painted copy with m = {paintShader: 0}.
Her loadout (the client's skins storage) wears 7819 on every weapon key. alice and bob join one brain-linked
server_bin lobby as two headless client_bin runs (the matchmaker, "0 play"). The server sends alice's SKINS with her
copy's paint (V2); bob's client builds alice's weapon mesh, and skin_fx.c logs "[paint] mesh enabled=1 shader=0" for
every painted mesh it draws. Bob's log must show that line (alice's paint, drawn on bob's screen), with enabled=1
and shader=0. Alice's own log must show it too.

  python3 -I client/tests/paint_shader_two_client_e2e.py BUILD_DIR      (repository root)

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
        print("paint_shader_two_client_e2e: no %s: skipped" % b)
        sys.exit(77)
sys.path.insert(0, os.path.join(ROOT, "server", "tests"))
from kn_proto import free_ports  # noqa: E402

NAME = "kn_test_paint2c_%d" % os.getpid()
SECRET = "kn-paint2c-%d" % os.getpid()
PW = "paint2c_pass1"
ALICE, BOB = "paint2c_alice", "paint2c_bob"
TOPO = 7819        # Topo, a weapon skin (type 0, weapon 2) with VS2_PAINTABLE (viewer_store_table.c)
SPECTRUM = 7795    # Spectrum: paintShader 0 (v10_paint.c)
WORK = tempfile.mkdtemp(prefix="kn-paint2c.", dir=os.path.expanduser("~/.cache"))
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
        print("paint_shader_two_client_e2e: no sqlite scratch database: skipped")
        return 77
    db = json.loads(r.stdout)
    dbpath = db["path"]
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=SECRET, **{"KN_DATABASE_" + k.upper(): str(v) for k, v in db.items()})
    try:
        subprocess.run([BRAIN, "--migrate-only"], cwd=ROOT, env=env, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        brain_log = os.path.join(WORK, "brain.log")
        brain = subprocess.Popen([BRAIN, "--brain.host=127.0.0.1", "--brain.port=0", "--brain.region=local",
                                  "--brain.id=paint2c-%d" % os.getpid(), "--finder.poll_interval_s=1"],
                                 cwd=ROOT, env=env, stdout=open(brain_log, "w"), stderr=subprocess.STDOUT,
                                 stdin=subprocess.DEVNULL, start_new_session=True)
        PROCS.append(brain)
        m = wait_for(lambda: re.search(r"listening on http://127\.0\.0\.1:(\d+) \(ws", read(brain_log)), 20)
        check("kn_brain starts on an ephemeral port", bool(m), read(brain_log)[-300:])
        if not m:
            return 1
        port = int(m.group(1))

        # the node (server_bin) links to the brain: the matchmaker seats the clients on it
        (node_port,) = free_ports(1)
        node_log = os.path.join(WORK, "node.log")
        node_cfg = os.path.join(WORK, "node.json")
        gs = {"node_id": "PAINT2C", "name": "paint two-client e2e", "listen_port": node_port, "public_host": "127.0.0.1",
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

        # the accounts: alice owns the Topo skin and a Spectrum; bob owns nothing
        tokens = {}
        for name in (ALICE, BOB):
            st, reg = http(port, "/auth/register", {"username": name, "password": PW})
            tok = reg.get("token", "") if isinstance(reg, dict) else ""
            tokens[name] = tok
            check("account %s registers" % name, st == 200 and bool(tok), "HTTP %d" % st)
        if not all(tokens.values()):
            return 1
        ids = {}
        for name in (ALICE, BOB):
            st, me = http(port, "/auth/me?token=" + tokens[name])
            acc = me.get("account", me) if isinstance(me, dict) else {}
            ids[name] = acc.get("id", 0) if isinstance(acc, dict) else 0
        a = ids[ALICE]
        db_query(dbpath, "INSERT INTO inventory (account_id, skin_id, count) VALUES (?, ?, 1)", (a, TOPO))
        db_query(dbpath, "INSERT INTO inventory (account_id, skin_id, count) VALUES (?, ?, 1)", (a, SPECTRUM))
        db_query(dbpath, "UPDATE players SET player_score = 2000000, player_funds = 5000 WHERE player_id = ?", (a,))

        # alice paints the Topo skin with the Spectrum: one copy, paintShader 0
        st, reply = http(port, "/items/%d/paint" % TOPO, {"item_id": TOPO, "paint_item_id": SPECTRUM}, tokens[ALICE])
        d = reply.get("data") if isinstance(reply, dict) else None
        check("alice paints Topo with the Spectrum (the brain makes a shader copy)",
              st == 200 and isinstance(d, dict) and d.get("m") == {"paintShader": 0}, "HTTP %d %s" % (st, json.dumps(reply)[:200]))
        rows = db_query(dbpath, "SELECT paint_color, paint_shader FROM skin_copies WHERE account_id = ? AND skin_id = ?", (a, TOPO))
        check("skin_copies: alice's copy has paint_shader 0, no colour", rows == [(None, 0)], repr(rows))

        # clients: the loadout wears Topo on every weapon key (the client's skins storage, read by ui_loadout.c)
        def client(name):
            cfg = os.path.join(WORK, name + "-cfg")
            os.makedirs(os.path.join(cfg, "krunknative"), exist_ok=True)
            for k, v in (("krunker_username", name), ("krunker_token", tokens[name]), ("knRuleset", "v10")):
                with open(os.path.join(cfg, "krunknative", k + ".json"), "w") as f:
                    f.write(v)
            with open(os.path.join(cfg, "krunknative", "skins.json"), "w") as f:
                json.dump({str(w): TOPO for w in range(64)}, f)
            return cfg

        def launch(name):
            cfg = client(name)
            script = os.path.join(WORK, name + ".script")
            cmds = os.path.join(WORK, name + ".cmds")
            open(cmds, "w").close()
            with open(script, "w") as f:
                f.write("0 play\n")
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
            return log

        bob_log = launch(BOB)
        alice_log = launch(ALICE)
        painted = r"\[paint\] mesh enabled=1 shader=0"
        ok_alice = wait_for(lambda: re.search(painted, read(alice_log)), 120, 1.0)
        ok_bob = wait_for(lambda: re.search(painted, read(bob_log)), 120, 1.0)
        check("alice's client draws her shader paint (enabled=1 shader=0)", bool(ok_alice), read(alice_log)[-300:])
        check("bob's client draws alice's shader paint on her weapon (enabled=1 shader=0)", bool(ok_bob), read(bob_log)[-300:])
        # the paint reached bob through the server's SKINS: bob's client is in the same lobby as alice
        check("the two clients joined the same server", "connected to server" in read(bob_log) and "connected to server" in read(alice_log),
              read(bob_log)[-200:])
    finally:
        stop_all()
        subprocess.run(scratch + ["drop", "sqlite", NAME], capture_output=True)
    print("paint_shader_two_client_e2e: " + ("OK" if not FAILS else "%d failure(s), logs in %s" % (len(FAILS), WORK)))
    if not FAILS:
        shutil.rmtree(WORK, ignore_errors=True)
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
