#!/usr/bin/env python3
"""The Junk counter on the v10 page follows the wallet (doc/v10/junk.md), end to end.

  python3 -I server/tests/junk_wallet_e2e.py BUILD_DIR      (run from the repository root)

A real kn_brain on a scratch database (KN_TEST_DB_DRIVER, default sqlite) with one account whose
wallet holds 12.34 Junk (players.player_junk = 1234, set directly). A headless client_bin (v10 page,
--offline like v10_menu_e2e: the brain still signs in) signs in with the account's saved session:
  1. the navbar's Junk counter (menuJNKCount) reads the account's balance, "12.34" (the account
     info's "junk" text of the social "a" frame, read into AccountInfo.junk)
  2. while the client is signed in, a game server (a scripted "acct" node, as a Reward Junk pickup
     makes) credits 0.05 Junk: the brain pushes ["uj", "12.39"] to the client's social socket, and the
     counter reads "12.39" without a restart or a sign-in
Prints PASS/FAIL lines; exit code 0 only when everything passed. 77 = skipped (no client_bin, no
database tool, no websockets).
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
import urllib.error
import urllib.request

try:
    import websockets
except ImportError:  # pragma: no cover
    print("junk_wallet_e2e: python websockets missing: skipped")
    sys.exit(77)

ROOT = os.getcwd()
BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
DRIVER = os.environ.get("KN_TEST_DB_DRIVER", "sqlite")
NAME = "kn_test_junk_e2e_%d" % os.getpid()
SECRET = "junk-wallet-e2e-%d" % os.getpid()
WORK = tempfile.mkdtemp(prefix="kn-junk-e2e.", dir=os.path.expanduser("~/.cache"))
results = []
procs = []


def check(name, ok, detail=""):
    results.append(bool(ok))
    show = detail and (not ok or len(detail) <= 160)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if show else ''}", flush=True)


def read_log(path):
    try:
        with open(path, errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def wait_for(fn, secs=10.0):
    end = time.time() + secs
    while time.time() < end:
        try:
            v = fn()
            if v:
                return v
        except Exception:  # noqa: BLE001 - polling
            pass
        time.sleep(0.1)
    return None


def http(port, path, body=None):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request("http://127.0.0.1:%d%s" % (port, path), data=data,
                                 headers={"Content-Type": "application/json"} if data else {},
                                 method="POST" if data else "GET")
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status, json.loads(r.read() or b"null")
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b"null")


def scratch_db(action):
    """db_scratch.py: create / drop the test database (sqlite file or the PostgreSQL schema)."""
    scratch = [sys.executable, "-I", os.path.join(ROOT, "services", "brain", "tests", "db_scratch.py"), ROOT]
    if action == "drop":
        subprocess.run(scratch + ["drop", DRIVER, NAME], capture_output=True)
        return None
    r = subprocess.run(scratch + ["create", DRIVER, NAME], capture_output=True)
    return json.loads(r.stdout) if r.returncode == 0 else r.returncode


def db_exec(cfg, sql, args=()):
    """One statement on the scratch database (sqlite only; the postgres path uses psql via db_scratch)."""
    con = sqlite3.connect(cfg["path"])
    try:
        cur = con.execute(sql, args)
        rows = cur.fetchall()
        con.commit()
        return rows
    finally:
        con.close()


class Node:
    """A scripted game server on /mm/internal/ws: one "acct" op (junk_credit)."""

    def __init__(self, port):
        self.url = "ws://127.0.0.1:%d/mm/internal/ws" % port

    async def credit(self, account_id, hundredths):
        async with websockets.connect(self.url, additional_headers={"Authorization": "Bearer " + SECRET}) as ws:
            await ws.send(json.dumps({"type": "hello", "nodeId": "JWE1", "region": "local", "publicHost": "127.0.0.1",
                                      "publicPort": 1, "version": "test"}))
            while json.loads(await asyncio.wait_for(ws.recv(), 10)).get("type") != "welcome":
                pass
            await ws.send(json.dumps({"type": "acct", "rid": 1, "op": "junk_credit",
                                      "args": {"id": account_id, "amount": hundredths, "reason": "trigger"}}))
            while True:
                msg = json.loads(await asyncio.wait_for(ws.recv(), 15))
                if msg.get("type") == "acct-r" and msg.get("rid") == 1:
                    return msg.get("err"), msg.get("data")


def client_env(brain_port, cfg, name):
    base = os.path.join(WORK, name)
    os.makedirs(os.path.join(base, "cfg", "krunknative"), exist_ok=True)
    return base, dict({k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}, **{
        "KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": os.path.join(base, "cfg"),
        "XDG_STATE_HOME": os.path.join(base, "state"), "KRUNKNATIVE_NO_BROWSER": "1",
        "KRUNKNATIVE_WINDOW_SIZE": "1280x720", "KRUNKNATIVE_BRAIN_URL": "http://127.0.0.1:%d" % brain_port,
        "KRUNKNATIVE_NET_TRANSPORT": "tcp",
        "KRUNKNATIVE_FINDER_URL": "http://127.0.0.1:%d/servers" % brain_port})


def junk_text(log, label):
    """The text of the last `text @label` probe of the input script log, or None."""
    m = None
    for m in re.finditer(r"\[input_script\] text t=\d+ @%s n=\d+ .*? text=(.*)" % re.escape(label), log):
        pass
    return m.group(1).strip() if m else None


def main():
    if not os.access(f"{BUILD}/client_bin", os.X_OK):
        print("junk_wallet_e2e: no client_bin in %s: skipped" % BUILD)
        return 77
    if DRIVER != "sqlite":
        print("junk_wallet_e2e: this check runs on sqlite (its wallet row is set directly): skipped")
        return 77
    cfg = scratch_db("create")
    if not isinstance(cfg, dict):
        print("junk_wallet_e2e: %s unavailable: skipped" % DRIVER)
        return 77
    dbenv = {"KN_DATABASE_" + k.upper(): str(v) for k, v in cfg.items()}
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=SECRET, **dbenv)
    brain_log = os.path.join(WORK, "brain.log")
    try:
        subprocess.run([f"{BUILD}/services/brain/kn_brain", "--migrate-only"], cwd=ROOT, env=env, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        brain = subprocess.Popen([f"{BUILD}/services/brain/kn_brain", "--brain.host=127.0.0.1", "--brain.port=0",
                                  "--accounts.signup_rewards.enabled=false", "--brain.region=local",
                                  "--brain.id=junk-e2e-%d" % os.getpid(), "--finder.poll_interval_s=1"],
                                 cwd=ROOT, env=env, stdout=open(brain_log, "w"), stderr=subprocess.STDOUT,
                                 stdin=subprocess.DEVNULL, start_new_session=True)
        procs.append(brain)
        m = wait_for(lambda: re.search(r"listening on http://127\.0\.0\.1:(\d+) \(ws", read_log(brain_log)), 15)
        check("kn_brain starts", bool(m), read_log(brain_log)[-300:])
        if not m:
            return 1
        port = int(m.group(1))

        user = "jwe%d" % (os.getpid() % 100000)
        st, reg = http(port, "/auth/register", {"username": user, "password": "pw-junk-e2e-%d" % os.getpid()})
        token = reg.get("token", "") if isinstance(reg, dict) else ""
        check("an account registers and signs in with a session token", st == 200 and token, "HTTP %d" % st) # (no reply: it holds the token)
        if not token:
            return 1
        rows = db_exec(cfg, "SELECT id FROM accounts WHERE username = ?", (user,))
        account_id = int(rows[0][0])
        db_exec(cfg, "UPDATE players SET player_junk = 1234 WHERE player_id = ?", (account_id,))
        check("the wallet holds 12.34 Junk (1234 hundredths)", db_exec(cfg, "SELECT player_junk FROM players WHERE player_id = ?",
                                                                       (account_id,))[0][0] == 1234)

        script = os.path.join(WORK, "client.script")
        with open(script, "w") as f:
            f.write("0 menu\n"
                    "2000 text @jnk1 [id=menuJNKCount]\n"
                    "9000 text @jnk2 [id=menuJNKCount]\n"
                    "9500 quit\n")
        base, cenv = client_env(port, cfg, "client")
        for key, value in (("krunker_username", user), ("krunker_token", token), ("knRuleset", "v10")):
            with open(os.path.join(base, "cfg", "krunknative", key + ".json"), "w") as f:
                f.write(value) # the stored values are raw text (v10_menu_e2e.py client())
        # the UI settings file: no Quick Play seek at launch (it would hold the script's clock)
        with open(os.path.join(base, "cfg", "krunknative", "knSettings.json"), "w") as f:
            f.write(json.dumps({"knSeekOnLaunch": False}))
        log = os.path.join(base, "client.log")
        cenv["KRUNKNATIVE_INPUT_SCRIPT"] = script
        # --offline: the menu's game is the sandbox (no matchmaking, no game server); the brain still signs in
        client = subprocess.Popen(["stdbuf", "-oL", "-eL", f"{BUILD}/client_bin", "--offline"], cwd=ROOT, stdout=open(log, "w"),
                                  stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=cenv, start_new_session=True)
        procs.append(client)

        first = wait_for(lambda: junk_text(read_log(log), "jnk1"), 60)
        check("1: the navbar's Junk counter reads the account's balance (12.34)", first == "12.34", str(first))
        if first is not None:
            err, data = asyncio.run(Node(port).credit(account_id, 5))
            check("2: a game server's credit of 0.05 is accepted (balance 12.39)", err is None and data == {"junk": "12.39", "credited": 5},
                  "%s %s" % (err, data))
        try:
            client.wait(90)
        except subprocess.TimeoutExpired:
            os.killpg(client.pid, signal.SIGTERM)
            client.wait(5)
            check("the client finished its script", False, "timed out; log " + log)
        second = junk_text(read_log(log), "jnk2")
        check("2: the counter follows the brain's push while signed in (12.39, no sign-in again)", second == "12.39", str(second))
    finally:
        for p in procs:
            if p.poll() is None:
                try:
                    os.killpg(p.pid, signal.SIGTERM)
                except OSError:
                    p.terminate()
                try:
                    p.wait(5)
                except subprocess.TimeoutExpired:
                    p.kill()
        scratch_db("drop")
    ok = bool(results) and all(results)
    print("junk_wallet_e2e: " + ("OK" if ok else "FAILED") + " (%d checks)" % len(results))
    if ok:
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print("logs kept in " + WORK)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
