#!/usr/bin/env python3
"""A trade offer's note sends every character it allows (v10_market.c).

  #v10mkCtMsg (M.ct_msg[500*4+1], was 504; maxlength 500, deob-10.0.7.js:111332; oninput v10mk.cmsg(this.value)):
  505 "漢" typed, the box keeps 500, and all 500 (1500 bytes) reach trades.message.

kn_brain on a scratch sqlite database; alice and bob are past the trade level and alice has a stack to offer.
Her client (signed in, v10 ruleset) is driven by an input script: the Store, the exchange's Trading tab, Start a
Trade with bob's name typed, a click on her stack (its row id is the skin index), the note typed into the box, Send and Confirm; the brain's
trades row is read back.

  python3 -I client/tests/offer_note_e2e.py BUILD_DIR [SRC]

Headless (KRUNKNATIVE_HEADLESS=1, null audio, no DISPLAY, private XDG_CONFIG_HOME). 77 = skip (no client_bin /
kn_brain, or the sqlite scratch database is unavailable).
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
import urllib.error
import urllib.request

BUILD = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug")
ROOT = os.path.abspath(sys.argv[2] if len(sys.argv) > 2 else os.getcwd())
CLIENT = os.path.join(BUILD, "client_bin")
BRAIN = os.path.join(BUILD, "services", "brain", "kn_brain")
for b in (CLIENT, BRAIN):
    if not os.access(b, os.X_OK):
        print("offer_note_e2e: no %s: skipped" % b)
        sys.exit(77)

NAME = "kn_test_offernote_%d" % os.getpid()
SECRET = "kn-offernote-%d" % os.getpid()
PW = "offernote_pass1"
ALICE, BOB = "offer_alice", "offer_bob"
WORK = tempfile.mkdtemp(prefix="kn-offernote.", dir=os.path.expanduser("~/.cache"))
FAILS = []
PROCS = []

HAN = "漢"  # 3 bytes, one UTF-16 unit


def check(what, ok, detail=""):
    print(("  ok   " if ok else "  FAIL ") + what + ("" if ok or not detail else "  [%s]" % str(detail)[:300]), flush=True)
    if not ok:
        FAILS.append(what)


def read(path):
    try:
        with open(path, errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def wait_for(fn, secs, step=0.5):
    end = time.time() + secs
    while time.time() < end:
        v = fn()
        if v:
            return v
        time.sleep(step)
    return fn()


def http(port, path, body=None):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request("http://127.0.0.1:%d%s" % (port, path), data=data,
                                 headers={"Content-Type": "application/json"} if data else {}, method="POST" if data else "GET")
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status, json.loads(r.read() or b"null")
    except urllib.error.HTTPError as e:
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


def types(text, chunk):
    """`type` lines of at most `chunk` characters (a script line holds 1023 bytes of text)."""
    return ["type " + text[i:i + chunk] for i in range(0, len(text), chunk)]


def describe(s):
    if s is None:
        return "None"
    return "%d chars, %d bytes: %r...%r" % (len(s), len(s.encode()), s[:3], s[-3:])


def main():
    scratch = [sys.executable, "-I", os.path.join(ROOT, "services", "brain", "tests", "db_scratch.py"), ROOT]
    r = subprocess.run(scratch + ["create", "sqlite", NAME], capture_output=True)
    if r.returncode:
        print("offer_note_e2e: no sqlite scratch database: skipped")
        return 77
    db = json.loads(r.stdout)
    dbpath = db["path"]
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=SECRET, **{"KN_DATABASE_" + k.upper(): str(v) for k, v in db.items()})
    try:
        subprocess.run([BRAIN, "--migrate-only"], cwd=ROOT, env=env, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        brain_log = os.path.join(WORK, "brain.log")
        PROCS.append(subprocess.Popen([BRAIN, "--brain.host=127.0.0.1", "--brain.port=0", "--brain.region=local",
                                       "--brain.id=offernote-%d" % os.getpid()], cwd=ROOT, env=env, stdout=open(brain_log, "w"),
                                      stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, start_new_session=True))
        m = wait_for(lambda: re.search(r"listening on http://127\.0\.0\.1:(\d+) \(ws", read(brain_log)), 20)
        check("kn_brain starts on an ephemeral port", bool(m), read(brain_log)[-300:])
        if not m:
            return 1
        port = int(m.group(1))
        tokens, ids = {}, {}
        for name in (ALICE, BOB):
            st, reg = http(port, "/auth/register", {"username": name, "password": PW})
            tokens[name] = reg.get("token", "") if isinstance(reg, dict) else ""
            check("account %s registers" % name, st == 200 and bool(tokens[name]), "HTTP %s" % st)
            row = db_query(dbpath, "SELECT id FROM accounts WHERE username = ?", (name,))
            ids[name] = row[0][0] if row else 0
        if not all(tokens.values()) or not all(ids.values()):
            return 1
        # both past the trade level; alice has a Flip-Flop stack (7857) to offer
        db_query(dbpath, "UPDATE players SET player_score = 2000000, player_funds = 5000 WHERE player_id IN (?, ?)",
                 (ids[ALICE], ids[BOB]))
        db_query(dbpath, "INSERT INTO inventory (account_id, skin_id, count) VALUES (?, 7857, 2)", (ids[ALICE],))

        note = HAN * 505
        # '#' starts a script comment, so the selectors are [id=...]. The exchange is windows[13].craftMenu(); its
        # sidebar's v10mkSide1 is Trading. Each view gets a second to load before a click.
        lines = ["0 menu", "3000 clickid menuBtnShop", "4000 call windows[13].craftMenu",
                 "5000 waitfor [id=v10mkSide1]", "6000 clickid v10mkSide1", "7000 call v10mk.tcreate",
                 "7000 waitfor [id=v10mkStartName]", "8000 clicksel [id=v10mkStartName]", "8100 type " + BOB,
                 "8300 clickid v10mkStartGo", "8300 waitfor [id=v10mkCtI0_7857]", "10000 clickid v10mkCtI0_7857",
                 "11000 clicksel [id=v10mkCtMsg]"]
        lines += ["11100 " + l for l in types(note, 300)]
        lines += ["11300 text @note [id=v10mkCtMsg]", "11400 clickid v10mkCtSubmit", "11400 waitfor [id=v10mkModalOk]",
                  "12500 clickid v10mkModalOk", "16000 quit"]
        cfg = os.path.join(WORK, "alice-cfg")
        os.makedirs(os.path.join(cfg, "krunknative"), exist_ok=True)
        for k, v in (("krunker_username", ALICE), ("krunker_token", tokens[ALICE]), ("knRuleset", "v10")):
            with open(os.path.join(cfg, "krunknative", k + ".json"), "w") as f:
                f.write(v)
        script = os.path.join(WORK, "alice.script")
        with open(script, "w", encoding="utf-8") as f:
            f.write("".join(l + "\n" for l in lines))
        log = os.path.join(WORK, "alice.log")
        cenv = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
        cenv.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": cfg,
                     "XDG_STATE_HOME": os.path.join(WORK, "alice-state"), "KRUNKNATIVE_NO_BROWSER": "1",
                     "KRUNKNATIVE_UPDATES_URL": "off", "KRUNKNATIVE_INPUT_SCRIPT": script, "KRUNKNATIVE_WINDOW_SIZE": "1280x720",
                     "KRUNKNATIVE_BRAIN_URL": "http://127.0.0.1:%d" % port, "KRUNKNATIVE_NET_TRANSPORT": "tcp",
                     "KRUNKNATIVE_FINDER_URL": "http://127.0.0.1:%d/servers" % port,
                     "KRUNKNATIVE_EGL_DEVICE": os.environ.get("KRUNKNATIVE_EGL_DEVICE", "0"), "LIBGL_ALWAYS_SOFTWARE": "1"})
        p = subprocess.Popen(["stdbuf", "-oL", "-eL", CLIENT, "--offline", "--map", "kanji"], cwd=ROOT, stdout=open(log, "w"),
                             stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=cenv, start_new_session=True)
        PROCS.append(p)
        try:
            rc = p.wait(timeout=300)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
            rc = "timeout"
        text = read(log)
        check("alice's client exits 0", rc == 0, "rc=%s, log %s" % (rc, log))
        gave_up = [l for l in text.splitlines() if "waitfor timeout" in l or "not shown" in l]
        check("every waitfor found its box", not gave_up, gave_up[:2])

        trades = [r[0] for r in db_query(dbpath, "SELECT message FROM trades WHERE sender_id = ? AND receiver_id = ?",
                                         (ids[ALICE], ids[BOB]))]
        check("the offer is sent", len(trades) == 1, trades and [describe(m) for m in trades])
        msg = trades[0] if trades else None
        check("the note box stops at maxlength 500", msg is not None and len(msg) <= 500, describe(msg))
        check("the note keeps all 500 characters (1500 bytes)", msg == HAN * 500, describe(msg))
        if FAILS:
            print("\n".join(l for l in text.splitlines() if l.startswith("[input_script]") and len(l) < 300)[-1500:])
    finally:
        for proc in PROCS:
            if proc.poll() is None:
                try:
                    proc.terminate()
                    proc.wait(5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
        subprocess.run(scratch + ["drop", "sqlite", NAME], capture_output=True)
    if FAILS:
        print("offer_note_e2e: %d failure(s), logs in %s" % (len(FAILS), WORK))
        return 1
    shutil.rmtree(WORK, ignore_errors=True)
    print("offer_note_e2e: passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
