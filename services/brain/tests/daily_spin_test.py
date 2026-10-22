#!/usr/bin/env python3
"""Krunker v10's Daily Spin on kn_brain's social socket (services/brain/daily_spin.c): every request is
answered, so the client's item never waits on a frame that will not come.

  daily_spin_test.py <kn_brain> <repo root> [database]

  ["dsp"] as a guest             -> ["dsp", null, "Must be logged in"]        (was: no answer)
  ["dsl"] as a guest             -> ["dslr", null, "Must be logged in"]       (was: ["po", ...], which the
                                                                                client's claim never reads)
  ["dsp"] signed in              -> ["dsp", {score, claimed, stage, maxScore, timeRemaining}] (maxScore = the
                                 configured accounts.daily_spin_score; the test sets it to 0)
  ["dsl"] signed in, score 0 >= accounts.daily_spin_score 0 -> ["dslr", {ticketId}] (the Starter ticket,
                                 a grant in skin_transactions); a second claim -> ["dslr", null, "Daily spin
                                 already claimed"]
The database is sqlite (db_scratch.py, no server needed); KN_TEST_DB_DRIVER=postgres|mysql runs there instead.
Exit 77 = skip (no websockets, or the store driver is unavailable).
"""
import asyncio
import json
import os
import re
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
SECRET = "kn-daily-spin-test-%d" % os.getpid()
NAME = "kndspin%d" % os.getpid()
FAILS = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what, flush=True)
    if not cond:
        FAILS.append(what)


def bound_port(log_path, proc):
    for _ in range(300):
        if proc.poll() is not None:
            raise RuntimeError("kn_brain exited early (%s)" % log_path)
        with open(log_path) as fp:
            m = re.search(r"listening on http://127\.0\.0\.1:(\d+) \(ws", fp.read().split("== start ")[-1])
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

    async def send(self, *frame):
        await self.ws.send(json.dumps(list(frame)))

    async def wait(self, label, seconds=5.0):
        """The first frame labelled `label` (others are kept for later), or None after `seconds`."""
        for i, f in enumerate(self.inbox):
            if f[0] == label:
                return self.inbox.pop(i)
        deadline = time.time() + seconds
        while time.time() < deadline:
            try:
                f = json.loads(await asyncio.wait_for(self.ws.recv(), deadline - time.time()))
            except asyncio.TimeoutError:
                break
            if f[0] == label:
                return f
            self.inbox.append(f)
        return None

    async def call(self, label, *frame, seconds=5.0):
        await self.send(*frame)
        return await self.wait(label, seconds)


async def register(sock, name):
    await sock.send("a", 0, [name, "dailyspin1", 0], "captcha")
    r = await sock.wait("a")
    assert r and r[1] is None, r
    return r[2]


async def scenario(port):
    stamp = str(int(time.time()) % 100000)
    social = "ws://127.0.0.1:%d/social/ws" % port
    print("== as a guest")
    async with Social(social) as g:
        r = await g.call("dsp", "dsp")
        check(r is not None and r[1] is None and r[2] == "Must be logged in", "dsp as a guest is answered: %s" % (r,))
        r = await g.call("dslr", "dsl")
        check(r is not None and r[1] is None and r[2] == "Must be logged in", "dsl as a guest is answered dslr, not po: %s" % (r,))

    print("== signed in")
    async with Social(social) as s:
        acct = await register(s, "kdsa" + stamp)
        check(acct is not None, "an account registers")
        r = await s.call("dsp", "dsp")
        d = r[1] if r else None
        check(isinstance(d, dict) and d.get("score") == 0 and d.get("claimed") is False and d.get("maxScore") == 0,
              "dsp for a new account: score 0, unclaimed, maxScore = accounts.daily_spin_score (0 here): %s" % (r,))
        r = await s.call("dslr", "dsl")
        check(r is not None and isinstance(r[1], dict) and isinstance(r[1].get("ticketId"), int),
              "dsl with the score at accounts.daily_spin_score (0): a ticket is granted: %s" % (r,))
        ticket = r[1]["ticketId"] if r and isinstance(r[1], dict) else None
        r = await s.call("dslr", "dsl")
        check(r is not None and r[1] is None and r[2] == "Daily spin already claimed",
              "a second claim the same day is refused in dslr: %s" % (r,))
        r = await s.call("dsp", "dsp")
        d = r[1] if r else None
        check(isinstance(d, dict) and d.get("claimed") is True and d.get("stage") == 1, "dsp after the claim: claimed, stage 1: %s" % (r,))
        check(ticket is not None and ticket > 0, "the ticket is a wheel ticket of the store: %s" % ticket)


def main():
    scratch = [sys.executable, "-I", os.path.join(ROOT, "services", "brain", "tests", "db_scratch.py"), ROOT]
    r = subprocess.run(scratch + ["create", DRIVER, NAME], capture_output=True)
    if r.returncode:
        print("daily spin test: %s unavailable: skipped" % DRIVER)
        return r.returncode
    dbcfg = json.loads(r.stdout)
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=SECRET, **{"KN_DATABASE_" + k.upper(): str(v) for k, v in dbcfg.items()})
    work = tempfile.mkdtemp(prefix="kn-dspin.")
    log_path = os.path.join(work, "brain.log")
    proc = None
    try:
        subprocess.run([BRAIN, "--migrate-only"], cwd=ROOT, env=env, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        with open(log_path, "a") as log:
            log.write("== start\n")
            proc = subprocess.Popen([BRAIN, "--brain.host=127.0.0.1", "--brain.port=0", "--brain.id=dspin-%d" % os.getpid(),
                                     "--accounts.signup_rewards.enabled=false", "--accounts.email.required=false", "--smtp.host=",
                                     "--accounts.daily_spin_score=0"],
                                    cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT)
        port = bound_port(log_path, proc)
        asyncio.run(scenario(port))
    finally:
        if proc is not None and proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(10)
            except subprocess.TimeoutExpired:
                proc.kill()
        subprocess.run(scratch + ["drop", DRIVER, NAME], capture_output=True)
    if FAILS:
        print("daily spin test: %d failed" % len(FAILS))
        return 1
    print("daily spin test: all passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
