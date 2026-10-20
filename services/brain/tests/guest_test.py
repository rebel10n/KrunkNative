#!/usr/bin/env python3
"""The signed-out guest's balance on kn_brain (services/brain/guest.c, db_guest.c; the accounts ops
guest_credit / guest_claim; migration 0350_guest_profiles).

  guest_test.py <kn_brain | build dir> [repo root]
  (CTest passes the kn_brain binary and the source root; a build dir means BUILD/services/brain/kn_brain.)

A scratch sqlite database (db_scratch.py), kn_brain --migrate-only, then kn_brain on an ephemeral port.
A scripted game server sends the "acct" frames of the guest ops over /mm/internal/ws (hello, welcome,
then the frames); the two routes are plain HTTP. Each check prints one PASS or FAIL line. No guest id is
printed: the ids are random 32-hex strings, and the brain's log must not contain one (check 11).

Covers: an unknown valid id reads 200 zeros and creates no row; uppercase, short, non-hex and missing ids
are 400 on GET and on POST /guest/claim; a credit (KR 12, XP 800) lands; the same match again is a duplicate
and moves nothing; a new match is capped (KR 20, score 2500); a claim zeroes the balance and keeps the credit
rows; claiming an unknown id creates no row; a credit after the claim counts again, and a replay of an old
match after the claim is still a duplicate; a bad guest id or an empty match is refused and writes nothing.

Exit 0 when every check passes, 1 otherwise, 77 when kn_brain, the websockets module or the sqlite scratch
database is unavailable (CTest's skip).
"""
import asyncio
import json
import os
import re
import secrets
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
    print("guest_test: python websockets missing: skipped")
    sys.exit(77)

HERE = os.path.dirname(os.path.abspath(__file__))
ARG = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
ROOT = os.path.abspath(sys.argv[2]) if len(sys.argv) > 2 else os.path.abspath(os.path.join(HERE, "..", "..", ".."))
BRAIN = os.path.join(ARG, "services", "brain", "kn_brain") if os.path.isdir(ARG) else ARG
NAME = "kn_test_guest_%d" % os.getpid()
SECRET = "kn-guest-test-secret-%d" % os.getpid()
DRIVER = os.environ.get("KN_TEST_DB_DRIVER", "sqlite")
DBPATH = ""
RESULTS = []
ZERO = {"funds": 0, "score": 0}
M1, M2, M3, M4 = "match-1", "match-2", "match-3", "match-4"


def check(name, ok, detail=""):
    RESULTS.append(bool(ok))
    print(("PASS  " if ok else "FAIL  ") + name + ("" if ok or not detail else "  -- " + detail), flush=True)


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
        time.sleep(0.1)
    return None


def new_guest():
    return secrets.token_hex(16)  # 32 lowercase hex digits, the client's guest id


def rows(sql, args=()):
    con = sqlite3.connect(DBPATH)
    try:
        return con.execute(sql, args).fetchall()
    finally:
        con.close()


def guest_row_count(gid):
    return rows("SELECT count(*) FROM guest_profiles WHERE guest_id = ?", (gid,))[0][0]


def profile_rows():
    return rows("SELECT count(*) FROM guest_profiles")[0][0]


def credit_count(gid):
    return rows("SELECT count(*) FROM guest_credits WHERE guest_id = ?", (gid,))[0][0]


def stored_balance(gid):
    found = rows("SELECT funds, score FROM guest_profiles WHERE guest_id = ?", (gid,))
    return {"funds": found[0][0], "score": found[0][1]} if found else None


def http(port, method, path, body=None):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request("http://127.0.0.1:%d%s" % (port, path), data=data, method=method,
                                 headers={"Content-Type": "application/json"} if data is not None else {})
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            status, raw = r.status, r.read()
    except urllib.error.HTTPError as e:
        status, raw = e.code, e.read()
    try:
        return status, json.loads(raw or b"null")
    except ValueError:
        return status, None


def profile_of(body):
    """data.profile of a 200 reply (None for any other shape)"""
    if not isinstance(body, dict) or not isinstance(body.get("data"), dict):
        return None
    return body["data"].get("profile")


def is_refusal(status, body):
    return status == 400 and isinstance(body, dict) and bool(body.get("error")) and body.get("message") == body.get("error")


class Node:
    """A scripted game server on /mm/internal/ws: hello, welcome, then "acct" frames."""

    def __init__(self, port):
        self.url = "ws://127.0.0.1:%d/mm/internal/ws" % port
        self.rid = 0
        self.ws = None

    async def __aenter__(self):
        self.ws = await websockets.connect(self.url, additional_headers={"Authorization": "Bearer " + SECRET})
        await self.ws.send(json.dumps({"type": "hello", "nodeId": "GBT1", "region": "local", "publicHost": "127.0.0.1",
                                       "publicPort": 1, "version": "test"}))
        while json.loads(await asyncio.wait_for(self.ws.recv(), 10)).get("type") != "welcome":
            pass
        return self

    async def __aexit__(self, *exc):
        await self.ws.close()

    async def op(self, op, args):
        """(err, data) of the acct-r reply to this frame"""
        self.rid += 1
        rid = self.rid
        await self.ws.send(json.dumps({"type": "acct", "rid": rid, "op": op, "args": args}))
        while True:
            msg = json.loads(await asyncio.wait_for(self.ws.recv(), 15))
            if msg.get("type") == "acct-r" and msg.get("rid") == rid:
                return msg.get("err"), msg.get("data")


async def scenario(port, brain_log):
    g = new_guest()              # credited, claimed, credited again
    unknown = new_guest()        # never credited: reads zeros, creates no row
    claim_unknown = new_guest()  # claimed, never created
    ids = [g, unknown, claim_unknown]
    async with Node(port) as node:
        # 1. an unknown valid id reads zeros and creates nothing
        st, body = http(port, "GET", "/guest/profile?guest=" + unknown)
        check("1: GET /guest/profile of an unknown valid id is 200 with zeros", st == 200 and profile_of(body) == ZERO,
              "HTTP %d" % st)
        check("1b: that read created no row", guest_row_count(unknown) == 0)

        # 2. bad ids are 400 with {error, message}
        bad = {
            "uppercase": "ABCDEF0123456789abcdef0123456789",
            "short": "0123456789abcdef0123456789abcde",
            "non-hex": "g123456789abcdef0123456789abcdef",
        }
        for label, gid in bad.items():
            st, body = http(port, "GET", "/guest/profile?guest=" + gid)
            check("2: GET with a %s id is 400" % label, is_refusal(st, body), "HTTP %d" % st)
        st, body = http(port, "GET", "/guest/profile")
        check("2: GET without a guest is 400", is_refusal(st, body), "HTTP %d" % st)
        for label, payload in (("uppercase", {"guest": bad["uppercase"]}), ("short", {"guest": bad["short"]}),
                               ("non-hex", {"guest": bad["non-hex"]}), ("missing", {})):
            st, body = http(port, "POST", "/guest/claim", payload)
            check("2: POST /guest/claim with a %s id is 400" % label, is_refusal(st, body), "HTTP %d" % st)

        # 3. the first match's credit lands
        err, data = await node.op("guest_credit", {"guest": g, "match": M1, "kr": 12, "xp": 800})
        check("3: guest_credit of KR 12 and XP 800 is accepted (funds 12, score 800, not a duplicate)",
              err is None and data == {"funds": 12, "score": 800, "duplicate": 0}, "%s %s" % (err, data))
        st, body = http(port, "GET", "/guest/profile?guest=" + g)
        check("3b: GET reads funds 12 and score 800", st == 200 and profile_of(body) == {"funds": 12, "score": 800},
              "HTTP %d" % st)

        # 4. the same match again is a duplicate and moves nothing
        err, data = await node.op("guest_credit", {"guest": g, "match": M1, "kr": 12, "xp": 800})
        check("4: the same match again is a duplicate and moves nothing",
              err is None and data == {"funds": 12, "score": 800, "duplicate": 1}, "%s %s" % (err, data))

        # 5. a new match is capped: KR at 20, score at 2500
        err, data = await node.op("guest_credit", {"guest": g, "match": M2, "kr": 15, "xp": 2000})
        check("5: a new match is capped (funds 20, score 2500)",
              err is None and data == {"funds": 20, "score": 2500, "duplicate": 0}, "%s %s" % (err, data))
        # huge amounts (2**62) stay at the cap and do not overflow the sums
        err, data = await node.op("guest_credit", {"guest": g, "match": M3, "kr": 1 << 62, "xp": 1 << 62})
        check("5b: a further match with huge amounts stays at the cap (no overflow)",
              err is None and data == {"funds": 20, "score": 2500, "duplicate": 0}, "%s %s" % (err, data))

        # 6. a login (claim) zeroes the balance
        err, data = await node.op("guest_claim", {"guest": g})
        check("6: guest_claim zeroes the balance", err is None and data == ZERO, "%s %s" % (err, data))
        st, body = http(port, "GET", "/guest/profile?guest=" + g)
        check("6b: GET after the claim reads zeros", st == 200 and profile_of(body) == ZERO, "HTTP %d" % st)

        # 7. claiming an unknown id creates no row
        before = profile_rows()
        err, data = await node.op("guest_claim", {"guest": claim_unknown})
        check("7: guest_claim of an unknown id answers zeros", err is None and data == ZERO, "%s %s" % (err, data))
        st, body = http(port, "POST", "/guest/claim", {"guest": claim_unknown})
        check("7b: POST /guest/claim of an unknown id is 200 with zeros", st == 200 and profile_of(body) == ZERO,
              "HTTP %d" % st)
        check("7c: claiming an unknown id created no row", guest_row_count(claim_unknown) == 0 and profile_rows() == before,
              "rows %d -> %d" % (before, profile_rows()))

        # 8. a credit after the claim counts again; a replay of an old match after the claim is still a duplicate
        err, data = await node.op("guest_credit", {"guest": g, "match": M4, "kr": 5, "xp": 100})
        check("8: a credit after the claim counts again (funds 5, score 100)",
              err is None and data == {"funds": 5, "score": 100, "duplicate": 0}, "%s %s" % (err, data))
        err, data = await node.op("guest_credit", {"guest": g, "match": M1, "kr": 12, "xp": 800})
        check("8b: a replay of an old match after the claim is a duplicate",
              err is None and data == {"funds": 5, "score": 100, "duplicate": 1}, "%s %s" % (err, data))
        st, body = http(port, "POST", "/guest/claim", {"guest": g})
        check("8c: POST /guest/claim zeroes the balance again", st == 200 and profile_of(body) == ZERO, "HTTP %d" % st)

        # 9. one credit row per credited match (M1, M2, M3, M4)
        check("9: guest_credits holds one row per credited match", credit_count(g) == 4, "rows %d" % credit_count(g))

        # 10. refusals write nothing
        err, data = await node.op("guest_credit", {"guest": "not-a-guest-id", "match": M1, "kr": 1, "xp": 1})
        check("10: guest_credit with a bad guest id is refused", err is not None and data is None, "%s %s" % (err, data))
        err, data = await node.op("guest_credit", {"guest": g, "match": "", "kr": 1, "xp": 1})
        check("10b: guest_credit with an empty match is refused", err is not None and data is None, "%s %s" % (err, data))
        err, data = await node.op("guest_claim", {"guest": "ABCDEF0123456789abcdef0123456789"})
        check("10c: guest_claim with an uppercase id is refused", err is not None and data is None, "%s %s" % (err, data))
        check("10d: the refused calls wrote nothing", credit_count(g) == 4 and stored_balance(g) == ZERO,
              "rows %d" % credit_count(g))

    # 11. the brain's log holds no guest id
    log = read(brain_log)
    check("11: the brain's log holds none of the guest ids", not any(gid in log for gid in ids))


def stop(proc):
    if proc.poll() is None:
        try:
            os.killpg(proc.pid, signal.SIGTERM)
        except OSError:
            proc.terminate()
        try:
            proc.wait(5)
        except subprocess.TimeoutExpired:
            proc.kill()


def main():
    global DBPATH
    if not os.access(BRAIN, os.X_OK):
        print("guest_test: no kn_brain at %s: skipped" % BRAIN)
        return 77
    if DRIVER != "sqlite":
        print("guest_test: runs on sqlite (it reads the rows from the file): skipped")
        return 77
    r = subprocess.run([sys.executable, "-I", os.path.join(HERE, "db_scratch.py"), ROOT, "create", "sqlite", NAME],
                       capture_output=True)
    if r.returncode:
        print("guest_test: no sqlite scratch database: skipped")
        return 77
    DBPATH = json.loads(r.stdout)["path"]
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=SECRET,
               **{"KN_DATABASE_" + k.upper(): str(v) for k, v in json.loads(r.stdout).items()})
    work = tempfile.mkdtemp(prefix="kn-guest-test.")
    log = os.path.join(work, "brain.log")
    procs = []
    try:
        mig = subprocess.run([BRAIN, "--migrate-only"], cwd=ROOT, env=env, capture_output=True)
        tables = rows("SELECT count(*) FROM sqlite_master WHERE type = 'table' AND name IN ('guest_profiles', 'guest_credits')")
        check("the migrations apply and create guest_profiles and guest_credits",
              mig.returncode == 0 and tables[0][0] == 2, mig.stderr.decode(errors="replace")[-200:])
        if mig.returncode or tables[0][0] != 2:
            return 1
        brain = subprocess.Popen([BRAIN, "--brain.host=127.0.0.1", "--brain.port=0", "--brain.region=local",
                                  "--brain.id=guest-test-%d" % os.getpid(), "--finder.poll_interval_s=1"],
                                 cwd=ROOT, env=env, stdout=open(log, "w"), stderr=subprocess.STDOUT,
                                 stdin=subprocess.DEVNULL, start_new_session=True)
        procs.append(brain)
        m = wait_for(lambda: re.search(r"listening on http://127\.0\.0\.1:(\d+) \(ws", read(log)), 20)
        check("kn_brain starts on an ephemeral port", m is not None, read(log)[-300:])
        if m is None:
            return 1
        try:
            asyncio.run(scenario(int(m.group(1)), log))
        except Exception as e:  # noqa: BLE001 - a broken scripted server is a failed run, not a crash
            check("the scripted game server completes its checks", False, repr(e)[:200])
    finally:
        for p in procs:
            stop(p)
        subprocess.run([sys.executable, "-I", os.path.join(HERE, "db_scratch.py"), ROOT, "drop", "sqlite", NAME],
                       capture_output=True)
    ok = bool(RESULTS) and all(RESULTS)
    print("guest_test: " + ("OK" if ok else "FAILED") + " (%d checks)" % len(RESULTS))
    if ok:
        shutil.rmtree(work, ignore_errors=True)
    else:
        print("logs kept in " + work)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
