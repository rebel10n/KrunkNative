#!/usr/bin/env python3
"""The Easter egg hunt's finds on kn_brain (Krunker v10.0.3 live event 3, doc/v10/events.md; services/brain/db_eggs.c
"egg_found", migration 0363_event_eggs; game.foundEventItem's proc 68, deob-10.0.3.js:220700-220728).

  eggs_test.py <kn_brain> <repo root>
Driver: KN_TEST_DB_DRIVER (default sqlite: no server needed; postgres / mysql through db_scratch.py). Ephemeral port.
77 = skip (no python websockets, the driver unavailable).

Covers: a game server's "egg_found" for a new egg answers re "found" with the account's count (1, then 2); the same
egg again answers cnt 0 (found before); the find that makes `total` answers re "success"; another account counts its
own finds; a bad key or account is refused; the rows are in event_eggs.
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

BRAIN, ROOT = sys.argv[1], os.path.abspath(sys.argv[2])
DRIVER = os.environ.get("KN_TEST_DB_DRIVER", "sqlite")
SCHEMA = "kn_test_eggs_%d" % os.getpid()
SECRET = "kn-eggs-test-%d" % os.getpid()
FAILS = []
DBCFG = {}


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what, flush=True)
    if not cond:
        FAILS.append(what)


def bound_port(log_path, proc):
    for _ in range(200):
        if proc.poll() is not None:
            raise RuntimeError("kn_brain exited early (%s)" % log_path)
        with open(log_path) as fp:
            m = re.search(r"listening on http://127\.0\.0\.1:(\d+) \(ws", fp.read())
        if m:
            return int(m.group(1))
        time.sleep(0.05)
    raise RuntimeError("kn_brain did not start listening")


class Social:
    def __init__(self, url):
        self.url = url + "?enc=json"
        self.inbox = []

    async def __aenter__(self):
        self.ws = await websockets.connect(self.url, max_size=4 << 20)
        return self

    async def __aexit__(self, *a):
        await self.ws.close()

    async def call(self, label, *frame):
        if frame:
            await self.ws.send(json.dumps(list(frame)))
        deadline = time.time() + 10
        while True:
            for i, f in enumerate(self.inbox):
                if f[0] == label:
                    return self.inbox.pop(i)
            self.inbox.append(json.loads(await asyncio.wait_for(self.ws.recv(), deadline - time.time())))


class Node:
    """Game server side of /mm/internal/ws: the "acct" ops."""

    def __init__(self, port):
        self.url = "ws://127.0.0.1:%d/mm/internal/ws" % port
        self.rid = 0

    async def __aenter__(self):
        self.ws = await websockets.connect(self.url, additional_headers={"Authorization": "Bearer " + SECRET})
        await self.ws.send(json.dumps({"type": "hello", "nodeId": "KEG%d" % os.getpid(), "region": "local", "publicHost": "127.0.0.1",
                                       "publicPort": 1, "version": "test"}))
        while json.loads(await asyncio.wait_for(self.ws.recv(), 10)).get("type") != "welcome":
            pass
        return self

    async def __aexit__(self, *a):
        await self.ws.close()

    async def op(self, op, **args):
        self.rid += 1
        rid = self.rid
        await self.ws.send(json.dumps({"type": "acct", "rid": rid, "op": op, "args": args}))
        while True:
            msg = json.loads(await asyncio.wait_for(self.ws.recv(), 15))
            if msg.get("type") == "acct-r" and msg.get("rid") == rid:
                return msg.get("err"), msg.get("data")


def rows(account):
    if DRIVER != "sqlite":
        return None
    con = sqlite3.connect(DBCFG["path"])
    try:
        return sorted(r[0] for r in con.execute("SELECT egg_key FROM event_eggs WHERE account_id = ?", (account,)))
    finally:
        con.close()


async def scenario(port):
    async with Social("ws://127.0.0.1:%d/social/ws" % port) as sock:
        accounts = []
        for k in range(2):
            name = "eggs%d_%d" % (k, os.getpid() % 100000)
            r = await sock.call("a", "a", 0, [name, "eggpass12", 0], "captcha")
            accounts.append(r[2])
        check(all(isinstance(a, int) and a > 0 for a in accounts), "two accounts registered: %s" % accounts)
    a, b = accounts
    async with Node(port) as node:
        err, d = await node.op("egg_found", id=a, key="10-37", total=3)
        check(err is None and d == {"re": "found", "cnt": 1}, "a new egg: found, count 1: %s %s" % (err, d))
        err, d = await node.op("egg_found", id=a, key="10-37", total=3)
        check(err is None and d.get("cnt") == 0 and d.get("re") != "success", "the same egg again: count 0 (found before): %s" % d)
        err, d = await node.op("egg_found", id=a, key="10-52", total=3)
        check(err is None and d == {"re": "found", "cnt": 2}, "a second egg: count 2: %s" % d)
        err, d = await node.op("egg_found", id=a, key="3-8", total=3)
        check(err is None and d == {"re": "success", "cnt": 3}, "the third of three: success (the hunt is complete): %s" % d)
        err, d = await node.op("egg_found", id=b, key="10-37", total=3)
        check(err is None and d == {"re": "found", "cnt": 1}, "another account counts its own finds: %s" % d)
        err, d = await node.op("egg_found", id=a, key="", total=3)
        check(err, "an empty key is refused: %s" % err)
        err, d = await node.op("egg_found", id=0, key="1-1", total=3)
        check(err, "no account is refused: %s" % err)
        got = rows(a)
        check(got is None or got == ["10-37", "10-52", "3-8"], "event_eggs holds the account's three eggs: %s" % got)


def main():
    global DBCFG
    scratch = [sys.executable, "-I", os.path.join(ROOT, "services", "brain", "tests", "db_scratch.py"), ROOT]
    r = subprocess.run(scratch + ["create", DRIVER, SCHEMA], capture_output=True)
    if r.returncode:
        print("eggs test: %s unavailable: skipped" % DRIVER)
        sys.exit(r.returncode)
    DBCFG = json.loads(r.stdout)
    dbenv = {"KN_DATABASE_" + k.upper(): str(v) for k, v in DBCFG.items()}
    drop = scratch + ["drop", DRIVER, SCHEMA]
    work = tempfile.mkdtemp(prefix="kn-eggs.", dir=os.path.join(os.path.expanduser("~"), ".cache"))
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=SECRET, **dbenv)
    log_path = os.path.join(work, "brain.log")
    proc = None
    try:
        subprocess.run([BRAIN, "--migrate-only"], cwd=ROOT, env=env, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        with open(log_path, "w") as log:
            proc = subprocess.Popen([BRAIN, "--brain.host=127.0.0.1", "--brain.port=0", "--brain.id=keg-%d" % os.getpid(),
                                     "--accounts.login_throttle_s=0"], cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT)
        asyncio.run(scenario(bound_port(log_path, proc)))
    finally:
        if proc:
            proc.terminate()
            proc.wait(5)
        subprocess.run(drop)
    with open(log_path) as f:
        bad = [l.strip() for l in f if re.search(r"failed:|FATAL", l)]
    check(not bad, "no statement errors in the brain log %s" % bad[:3])
    print("eggs test: " + ("OK" if not FAILS else "%d failure(s)" % len(FAILS)))
    if FAILS:
        print("log kept in " + work)
    else:
        shutil.rmtree(work, ignore_errors=True)
    sys.exit(1 if FAILS else 0)


main()
