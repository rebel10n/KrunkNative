#!/usr/bin/env python3
"""Krunker Strike's feedback form on kn_brain (services/brain/strike_econ.c r_feedback, db_strike.c
P_STRIKE_FEEDBACK, migration 0340_strike_feedback), through the gapi tunnel of the social socket
(POST /strike/v1/feedback = FRVR's POST /v1/feedback {text, email, frvrId, channelId}):

  - a guest is refused (401 UNAUTHORIZED) and nothing is stored; GET on the path is 404
  - the text: 5 to 1000 bytes (400 FEEDBACK_TEXT); a missing body is refused the same way
  - the e-mail is optional, trimmed, at most 160 bytes and a plausible address (400 FEEDBACK_EMAIL)
  - a signed-in account's feedback answers {id}; the row keeps the account, the text verbatim (quotes,
    unicode), the trimmed e-mail ('' when none) and a created_at; refused submissions store nothing

  strike_feedback_test.py <kn_brain> <repo root>
Database: KN_TEST_DB_DRIVER=sqlite (default: a scratch file from db_scratch.py) or postgres (the
schema of pg_schema.py, as strike_economy_test.py). 77 = skip (no database / python websockets).
"""
import asyncio
import json
import os
import re
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import time

try:
    import websockets
except ImportError:  # pragma: no cover
    print("python websockets missing: skip")
    sys.exit(77)

BRAIN, ROOT = sys.argv[1], sys.argv[2]
DRIVER = os.environ.get("KN_TEST_DB_DRIVER", "sqlite")
SCHEMA = "kn_test_fb_%d" % os.getpid()
SECRET = "kn-strike-feedback-test-%d" % os.getpid()
FAILS = []
DBCFG = {}
PACE = 0.06  # the social socket allows 20 frames/s (social_ws.c RATE_PER_S)


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what, flush=True)
    if not cond:
        FAILS.append(what)


def bound_port(log_path, proc):
    for _ in range(200):
        if proc.poll() is not None:
            raise RuntimeError("kn_brain exited early (%s)" % log_path)
        with open(log_path) as fp:
            m = re.search(r"listening on http://127\.0\.0\.1:(\d+) \(ws", fp.read().split("== start ")[-1])
        if m:
            return int(m.group(1))
        time.sleep(0.05)
    raise RuntimeError("kn_brain did not start listening")


def rows(sql, args=()):
    """Every row of `sql` on the test database (sqlite; postgres through psql like strike_economy_test)."""
    if DRIVER == "sqlite":
        con = sqlite3.connect(DBCFG["path"])
        try:
            return [tuple(r) for r in con.execute(sql, args).fetchall()]
        finally:
            con.close()
    raise RuntimeError("feedback test: only the sqlite driver reads rows here (KN_TEST_DB_DRIVER=sqlite)")


def count():
    return rows("SELECT count(*) FROM strike_feedback")[0][0]


class Social:
    def __init__(self, url):
        self.url = url + "?enc=json"
        self.inbox = []
        self.rid = 0
        self.last = 0.0

    async def __aenter__(self):
        self.ws = await websockets.connect(self.url, max_size=16 << 20)
        return self

    async def __aexit__(self, *a):
        await self.ws.close()

    async def call(self, label, *frame):
        if frame:
            await self.ws.send(json.dumps(list(frame)))
        deadline = time.time() + 15
        while True:
            for i, f in enumerate(self.inbox):
                if f[0] == label:
                    return self.inbox.pop(i)
            self.inbox.append(json.loads(await asyncio.wait_for(self.ws.recv(), deadline - time.time())))

    async def gapi(self, method, path, body=None):
        """-> (status, data | error)"""
        wait = self.last + PACE - time.time()
        if wait > 0:
            await asyncio.sleep(wait)
        self.last = time.time()
        self.rid += 1
        rid = self.rid
        await self.ws.send(json.dumps(["gapi", rid, method, path, body]))
        deadline = time.time() + 15
        while True:
            for i, f in enumerate(self.inbox):
                if f[0] == "gapi" and f[1] == rid:
                    self.inbox.pop(i)
                    payload = f[3] or {}
                    return f[2], payload.get("data") if f[2] == 200 else payload.get("error")
            self.inbox.append(json.loads(await asyncio.wait_for(self.ws.recv(), deadline - time.time())))


async def register(sock, name):
    r = await sock.call("a", "a", 0, [name, "strikepass1", 0], "captcha")
    assert r[1] is None, r
    return r[2]


async def scenario(port):
    social = "ws://127.0.0.1:%d/social/ws" % port
    stamp = str(int(time.time()) % 100000)

    print("== a guest")
    async with Social(social) as g:
        before = count()
        st, err = await g.gapi("POST", "/strike/v1/feedback", {"text": "a guest writes this"})
        check(st == 401 and err["code"] == "UNAUTHORIZED", "a guest is refused: %s %s" % (st, err))
        st, _ = await g.gapi("GET", "/strike/v1/feedback")
        check(st == 404, "only POST is routed (GET: %s)" % st)
        check(count() == before, "nothing stored for a guest")

    async with Social(social) as a:
        aid = await register(a, "ksefb" + stamp)
        before = count()
        print("== the text and the e-mail are checked; a refusal stores nothing")
        for text, what in (("abcd", "4 characters"), ("", "an empty text"), ("x" * 1001, "1001 characters")):
            st, err = await a.gapi("POST", "/strike/v1/feedback", {"text": text})
            check(st == 400 and err["code"] == "FEEDBACK_TEXT", "%s is refused: %s %s" % (what, st, err))
        st, err = await a.gapi("POST", "/strike/v1/feedback", None)
        check(st == 400 and err["code"] == "FEEDBACK_TEXT", "a missing body is refused: %s %s" % (st, err))
        for mail, what in (("nope", "no @"), ("a@b", "no dot after the @"), ("a b@example.com", "a space"),
                           ("x" * 150 + "@example.com", "162 bytes")):
            st, err = await a.gapi("POST", "/strike/v1/feedback", {"text": "a fine message", "email": mail})
            check(st == 400 and err["code"] == "FEEDBACK_EMAIL", "e-mail with %s is refused: %s %s" % (what, st, err))
        check(count() == before, "no refused submission is stored (%d rows)" % (count() - before))

        print("== valid feedback is stored")
        text1 = "x" * 1000
        st, res = await a.gapi("POST", "/strike/v1/feedback", {"text": text1})
        check(st == 200 and isinstance(res.get("id"), int) and res["id"] > 0, "1000 characters: 200 with an id (%s %s)" % (st, res))
        id1 = res["id"] if st == 200 else 0
        text2 = "Quote ' and \"double\" and café – 5/5"
        st, res = await a.gapi("POST", "/strike/v1/feedback", {"text": text2, "email": "  player@example.com  ", "frvrId": "f", "channelId": "c"})
        id2 = res["id"] if st == 200 else 0
        check(st == 200 and id2 > id1 > 0, "a quoted text with an e-mail: 200, the next id (%s, %s)" % (id1, id2))
        st, res = await a.gapi("POST", "/strike/v1/feedback", {"text": "Thanks for the game", "email": "   "})
        id3 = res["id"] if st == 200 else 0
        check(st == 200 and id3 > id2, "a blank e-mail is no e-mail: 200 (%s)" % st)

        got = {r[0]: r for r in rows("SELECT id, account_id, message, email, created_at FROM strike_feedback WHERE id IN (?, ?, ?)",
                                     (id1, id2, id3))}
        check(len(got) == 3, "three rows stored")
        r1, r2, r3 = got.get(id1, (0, 0, "", "", "")), got.get(id2, (0, 0, "", "", "")), got.get(id3, (0, 0, "", "", ""))
        check(r1[1] == aid and r1[2] == text1 and r1[3] == "", "row 1: the account, the text, no e-mail (%d chars)" % len(r1[2]))
        check(r2[1] == aid and r2[2] == text2 and r2[3] == "player@example.com", "row 2: the text verbatim, the e-mail trimmed: %r" % r2[3])
        check(r3[1] == aid and r3[2] == "Thanks for the game" and r3[3] == "", "row 3: a blank e-mail stored as ''")
        check(all(r[4] for r in (r1, r2, r3)), "every row has its created_at")
        check(count() == before + 3, "exactly the three valid submissions are stored (%d)" % (count() - before))


def main():
    global DBCFG
    if DRIVER != "sqlite":
        print("strike feedback test: only KN_TEST_DB_DRIVER=sqlite is supported here: skipped")
        return 77
    scratch = [sys.executable, "-I", os.path.join(ROOT, "services", "brain", "tests", "db_scratch.py"), ROOT]
    r = subprocess.run(scratch + ["create", DRIVER, SCHEMA], capture_output=True)
    if r.returncode:
        print("strike feedback test: %s unavailable: skipped" % DRIVER)
        return r.returncode
    DBCFG = json.loads(r.stdout)
    dbenv = {"KN_DATABASE_" + k.upper(): str(v) for k, v in DBCFG.items()}
    work = tempfile.mkdtemp(prefix="kn-strike-feedback.", dir=os.path.join(os.path.expanduser("~"), ".cache"))
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=SECRET, **dbenv)
    log_path = os.path.join(work, "brain.log")
    proc = None
    try:
        subprocess.run([BRAIN, "--migrate-only"], cwd=ROOT, env=env, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        with open(log_path, "a") as log:
            log.write("== start \n")
            proc = subprocess.Popen([BRAIN, "--brain.host=127.0.0.1", "--brain.port=0", "--brain.id=kfb-%d" % os.getpid(),
                                     "--accounts.signup_rewards.enabled=false", "--accounts.login_throttle_s=0"],
                                    cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT)
        port = bound_port(log_path, proc)
        asyncio.run(scenario(port))
    finally:
        if proc:
            proc.terminate()
            proc.wait(5)
        subprocess.run(scratch + ["drop", DRIVER, SCHEMA], capture_output=True)
    with open(log_path) as f:
        bad = [l.strip() for l in f if re.search(r"failed:|ERROR|FATAL", l)]
    check(not bad, "no statement errors in the brain log %s" % bad[:3])
    print("strike feedback test: " + ("OK" if not FAILS else "%d failure(s)" % len(FAILS)))
    if FAILS:
        print("log kept in " + work)
    else:
        shutil.rmtree(work, ignore_errors=True)
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
