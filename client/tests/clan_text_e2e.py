#!/usr/bin/env python3
"""The Clan window's Settings boxes send every character they allow (clan_ui.c).

  MOTD        (motd_text[500*4+1]; #clanMotdInput is maxlength 500, deob-10.0.7.js:96354-96520): 505 "漢" typed,
              the box keeps 500, and all 500 (1500 bytes, past the old 1024) reach clans.clan_motd.
  description (r_desc[2000*4+1]; #recruit-desc is maxlength 2000): 2005 "語" typed, 2000 (6000 bytes, past the old
              2048) reach clan_recruitment.description.
  looking for (r_looking[500*4+1]; #recruit-looking is maxlength 500): 505 "漢" typed, 500 (past the old 512)
              reach clan_recruitment.looking_for.

kn_brain on a scratch sqlite database; alice leads a clan of her own. Her client (signed in, v10 ruleset) is
driven by an input script: real clicks into the boxes and typing through the char callback, then the Save
handlers; the brain's rows are read back.

  python3 -I client/tests/clan_text_e2e.py BUILD_DIR [SRC]

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
        print("clan_text_e2e: no %s: skipped" % b)
        sys.exit(77)

NAME = "kn_test_clantext_%d" % os.getpid()
SECRET = "kn-clantext-%d" % os.getpid()
PW = "clantext_pass1"
ALICE = "clantext_alice"
WORK = tempfile.mkdtemp(prefix="kn-clantext.", dir=os.path.expanduser("~/.cache"))
FAILS = []
PROCS = []

HAN, GO = "漢", "語"  # 3 bytes each, one UTF-16 unit


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
        print("clan_text_e2e: no sqlite scratch database: skipped")
        return 77
    db = json.loads(r.stdout)
    dbpath = db["path"]
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=SECRET, **{"KN_DATABASE_" + k.upper(): str(v) for k, v in db.items()})
    try:
        subprocess.run([BRAIN, "--migrate-only"], cwd=ROOT, env=env, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        brain_log = os.path.join(WORK, "brain.log")
        PROCS.append(subprocess.Popen([BRAIN, "--brain.host=127.0.0.1", "--brain.port=0", "--brain.region=local",
                                       "--brain.id=clantext-%d" % os.getpid()], cwd=ROOT, env=env, stdout=open(brain_log, "w"),
                                      stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, start_new_session=True))
        m = wait_for(lambda: re.search(r"listening on http://127\.0\.0\.1:(\d+) \(ws", read(brain_log)), 20)
        check("kn_brain starts on an ephemeral port", bool(m), read(brain_log)[-300:])
        if not m:
            return 1
        port = int(m.group(1))
        tokens, ids = {}, {}
        for name in (ALICE,):
            st, reg = http(port, "/auth/register", {"username": name, "password": PW})
            tokens[name] = reg.get("token", "") if isinstance(reg, dict) else ""
            check("account %s registers" % name, st == 200 and bool(tokens[name]), "HTTP %s" % st)
            row = db_query(dbpath, "SELECT id FROM accounts WHERE username = ?", (name,))
            ids[name] = row[0][0] if row else 0
        if not all(tokens.values()) or not all(ids.values()):
            return 1
        # alice owns the clan (clan_playerid) and has KR in its bank for the recruitment post (1,000 KR)
        db_query(dbpath, "UPDATE players SET player_score = 2000000, player_funds = 5000 WHERE player_id = ?", (ids[ALICE],))
        db_query(dbpath, "INSERT INTO clans (clan_name, clan_playerid, clan_funds) VALUES ('TextClan', ?, 50000)", (ids[ALICE],))
        clan = db_query(dbpath, "SELECT clan_id FROM clans WHERE clan_name = 'TextClan'")[0][0]
        db_query(dbpath, "INSERT INTO clan_members (account_id, clan_id, rank) VALUES (?, ?, 0)", (ids[ALICE], clan))

        motd, desc, looking = HAN * 505, GO * 2005, HAN * 505
        # '#' starts a script comment, so the selectors are [id=...]. The Clan window is windows[12]; tab 4 is Settings.
        lines = ["0 menu", "3000 call showWindow 13", "6000 call windows[12].switchTab 4",
                 "6000 waitfor [id=clanMotdInput]", "8000 clicksel [id=clanMotdInput]"]
        lines += ["8100 " + l for l in types(motd, 300)]
        lines += ["8300 text @motd [id=clanMotdInput]", "8400 call windows[12].motdSave",
                  # the recruitment form is below the fold of the 1280x720 window
                  "9500 waitfor [id=recruit-desc]", "9500 setscroll menuWindow 1000", "10000 clicksel [id=recruit-desc]"]
        # the description is typed before the MOTD's "Saved!" goes (3 s after the answer) and saved after it:
        # that re-render must keep it. Looking For is typed once it has gone (the re-render takes the focus).
        lines += ["10100 " + l for l in types(desc, 300)]
        lines += ["13000 clicksel [id=recruit-looking]"]
        lines += ["13100 " + l for l in types(looking, 300)]
        lines += ["13300 text @desc [id=recruit-desc]", "13300 text @looking [id=recruit-looking]",
                  "13400 call windows[12].recruitSave", "17000 quit"]
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

        saved = db_query(dbpath, "SELECT clan_motd FROM clans WHERE clan_id = ?", (clan,))
        saved = saved[0][0] if saved else None
        check("MOTD: the box stops at maxlength 500", saved is not None and len(saved) <= 500, describe(saved))
        check("MOTD: all 500 characters (1500 bytes) are saved", saved == HAN * 500, describe(saved))

        post = db_query(dbpath, "SELECT description, looking_for FROM clan_recruitment WHERE clan_id = ?", (clan,))
        check("recruitment: the post is created", len(post) == 1, post and [describe(c) for c in post[0]])
        d, lf = post[0] if post else (None, None)
        check("recruitment: the description keeps all 2000 characters (6000 bytes)", d == GO * 2000, describe(d))
        check("recruitment: Looking For keeps all 500 characters (1500 bytes)", lf == HAN * 500, describe(lf))
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
        print("clan_text_e2e: %d failure(s), logs in %s" % (len(FAILS), WORK))
        return 1
    shutil.rmtree(WORK, ignore_errors=True)
    print("clan_text_e2e: passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
