#!/usr/bin/env python3
"""The Social hub's text boxes send every character they allow (v10_social.c).

  reply  (post_reply, g_reply[500*4+1]; the box is maxlength 500, deob-10.0.7.js:107368): 505 "漢" typed, the box
         keeps 500, and all 500 (1500 bytes, past the old 1024) reach feed_comments; a reply with spaces around it
         is sent trimmed (commentPost's text.trim()).
  gift   (g_gift_msg, maxlength 500, :111703): 505 "語" typed, 500 land in the recipient's mail.
  report (g_report_text, maxlength 500, :111790): 255 "😀" typed; maxlength counts an emoji as two, so the box
         keeps 250 (1000 bytes, past the old 600) and all of them reach reports.details.

kn_brain on a scratch sqlite database; alice's client (signed in, v10 ruleset) is driven by an input script:
real clicks (clicksel / clickid) and typing through the char callback; the brain's rows are read back.

  python3 -I client/tests/hub_text_e2e.py BUILD_DIR [SRC]

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
        print("hub_text_e2e: no %s: skipped" % b)
        sys.exit(77)

NAME = "kn_test_hubtext_%d" % os.getpid()
SECRET = "kn-hubtext-%d" % os.getpid()
PW = "hubtext_pass1"
ALICE, BOB = "hubtext_alice", "hubtext_bob"
WORK = tempfile.mkdtemp(prefix="kn-hubtext.", dir=os.path.expanduser("~/.cache"))
FAILS = []
PROCS = []

HAN, GO, EMOJI = "漢", "語", "\U0001f600"  # 漢, 語 (3 bytes, one UTF-16 unit), 😀 (4 bytes, two units)


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
        print("hub_text_e2e: no sqlite scratch database: skipped")
        return 77
    db = json.loads(r.stdout)
    dbpath = db["path"]
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=SECRET, **{"KN_DATABASE_" + k.upper(): str(v) for k, v in db.items()})
    try:
        subprocess.run([BRAIN, "--migrate-only"], cwd=ROOT, env=env, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        brain_log = os.path.join(WORK, "brain.log")
        PROCS.append(subprocess.Popen([BRAIN, "--brain.host=127.0.0.1", "--brain.port=0", "--brain.region=local",
                                       "--brain.id=hubtext-%d" % os.getpid()], cwd=ROOT, env=env, stdout=open(brain_log, "w"),
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
        # level 10+ and KR for the gift (100 KR and its 10% fee)
        db_query(dbpath, "UPDATE players SET player_score = 2000000, player_funds = 5000 WHERE player_id IN (?, ?)",
                 (ids[ALICE], ids[BOB]))
        db_query(dbpath, "INSERT INTO feed_posts (author_id, body) VALUES (?, ?)", (ids[BOB], "a post to reply to"))
        post = db_query(dbpath, "SELECT id FROM feed_posts WHERE author_id = ?", (ids[BOB],))[0][0]

        reply = HAN * 505
        gift = GO * 505
        report = EMOJI * 255
        # '#' starts a script comment, so the selectors are [id=...]. Each view gets a second to load before a click:
        # a press and release on either side of its re-render would land on the backdrop.
        lines = ["0 menu",
                 "3000 call openPost %d" % post, "3000 waitfor [id=v10sReply]", "4000 clicksel [id=v10sReply]"]
        lines += ["4100 " + l for l in types(reply, 300)]
        lines += ["4300 text @reply [id=v10sReply]", "4400 clickid v10sReplyBtn",
                  "7000 waitfor [id=v10sReply]", "7100 clicksel [id=v10sReply]", "7200 type    short reply   ", "7300 clickid v10sReplyBtn",
                  "10000 call openPlayerProfile \"%s\"" % BOB, "10000 waitfor [id=v10sProfileReport]", "11000 clickid v10sProfileReport",
                  "11100 waitfor [id=v10sReportText]", "12000 clicksel [id=v10sReportText]"]
        lines += ["12100 " + l for l in types(report, 128)]
        lines += ["12300 text @report [id=v10sReportText]", "12400 clickid v10sReportSend",
                  # Back to the profile: a second openPlayerProfile would stack a hidden copy of its buttons, ids and all
                  "15000 call v10s \"back\"", "15000 waitfor [id=v10sProfileGift]", "16000 clickid v10sProfileGift",
                  "16100 waitfor [id=v10sGiftMsg]", "17000 clicksel [id=v10sGiftMsg]"]
        lines += ["17100 " + l for l in types(gift, 300)]
        lines += ["17300 text @gift [id=v10sGiftMsg]", "17400 clickid v10sGiftSend", "21000 quit"]
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

        comments = [r[0] for r in db_query(dbpath, "SELECT body FROM feed_comments WHERE post_id = ? ORDER BY id", (post,))]
        long_reply = comments[0] if comments else None
        check("reply: the box stops at maxlength 500", long_reply is not None and len(long_reply) <= 500, describe(long_reply))
        check("reply: all 500 characters (1500 bytes) are sent", long_reply == HAN * 500, describe(long_reply))
        check("reply: a reply with spaces around it is sent trimmed", comments[1:2] == ["short reply"], comments[1:2])

        mail = [r[0] for r in db_query(dbpath, "SELECT body FROM mail WHERE account_id = ? AND subject = 'KR Gift'", (ids[BOB],))]
        check("gift: the KR gift is sent", len(mail) == 1, mail)
        check("gift: the note keeps all 500 characters (1500 bytes)", mail[:1] == [GO * 500], describe(mail[0] if mail else None))

        rep = [r[0] for r in db_query(dbpath, "SELECT details FROM reports WHERE reporter_id = ? AND target_id = ?", (ids[ALICE], ids[BOB]))]
        check("report: the report is filed", len(rep) == 1, rep)
        check("report: 250 emoji (500 UTF-16 units, 1000 bytes) are sent", rep[:1] == [EMOJI * 250], describe(rep[0] if rep else None))
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
        print("hub_text_e2e: %d failure(s), logs in %s" % (len(FAILS), WORK))
        return 1
    shutil.rmtree(WORK, ignore_errors=True)
    print("hub_text_e2e: passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
