#!/usr/bin/env python3
"""v10.0.3's account lifecycle and raid record on kn_brain (services/brain/account_life.c, raids.c;
migrations 0359_account_lifecycle / 0360_raid_stats).

  account_life_test.py <kn_brain> <repo root>
env: KN_TEST_DB_DRIVER=sqlite (default) | postgres | mysql (db_scratch.py). 77 = skip.

Covers: the account info's reqDel / twitchId / twitch / raidStats extras; the v10.0.3 password change
["upp", new, repeat, sessionToken] (30 minute clock, other sessions revoked, old password refuses) and
the v7 old-password shape; the deletion request / cancel ("rqd", sessions ended, reqDel); the Twitch
handlers with no Twitch app configured (unavailable, empty drops, unlink); a game server's "raid" run
in the stats commit (attempts, completions, best time, AI / boss kills, deaths, once per match) and the
raid leaderboards; and the grace-period deletion sweep (sqlite).
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
import urllib.request

try:
    import websockets
except ImportError:  # pragma: no cover
    print("python websockets missing: skip")
    sys.exit(77)

BRAIN, ROOT = sys.argv[1], os.path.abspath(sys.argv[2])
DRIVER = os.environ.get("KN_TEST_DB_DRIVER", "sqlite")
SCHEMA = "kn_test_acctlife_%d" % os.getpid()
SECRET = "kn-acctlife-test-%d" % os.getpid()
FAILS = []
DBCFG = {}


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what, flush=True)
    if not cond:
        FAILS.append(what)


def bound_port(log_path, proc):
    for _ in range(400):
        if proc.poll() is not None:
            raise RuntimeError("kn_brain exited early (%s)" % log_path)
        with open(log_path) as fp:
            m = re.search(r"listening on http://127\.0\.0\.1:(\d+) \(ws", fp.read())
        if m:
            return int(m.group(1))
        time.sleep(0.05)
    raise RuntimeError("kn_brain did not start listening")


def query(sql):
    """First column of every row of `sql` on the test database, as text."""
    if DRIVER == "sqlite":
        con = sqlite3.connect(DBCFG["path"], timeout=30)
        try:
            rows = [str(r[0]) for r in con.execute(sql).fetchall()]
            con.commit()
            return rows
        finally:
            con.close()
    get = lambda k, d: os.environ.get("KN_DATABASE_" + k.upper(), str(DBCFG.get(k, d)))
    # the scratch database is a schema (db_scratch.py): without its search_path every statement hit the wrong one
    env = dict(os.environ, PGPASSWORD=get("password", ""), PGOPTIONS="-c search_path=" + str(DBCFG.get("search_path", "public")))
    r = subprocess.run(["psql", "-h", get("host", "127.0.0.1"), "-p", get("port", 5432), "-U", get("user", "krunknative"),
                        "-d", DBCFG["name"], "-v", "ON_ERROR_STOP=1", "-qAt", "-c", sql],
                       env=env, capture_output=True, timeout=30)
    return r.stdout.decode().split("\n")[:-1] if r.returncode == 0 else None


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
        deadline = time.time() + 15
        while True:
            for i, f in enumerate(self.inbox):
                if f[0] == label:
                    return self.inbox.pop(i)
            self.inbox.append(json.loads(await asyncio.wait_for(self.ws.recv(), deadline - time.time())))

    async def login(self, name, password):
        return await self.call("a", "a", 1, [name, password, None], "captcha")


class Node:
    """Game server side of /mm/internal/ws (the stats commit's "raid" run)."""

    def __init__(self, port):
        self.url = "ws://127.0.0.1:%d/mm/internal/ws" % port
        self.rid = 0

    async def __aenter__(self):
        self.ws = await websockets.connect(self.url, additional_headers={"Authorization": "Bearer " + SECRET})
        await self.ws.send(json.dumps({"type": "hello", "nodeId": "KAL%d" % os.getpid(), "region": "local", "publicHost": "127.0.0.1",
                                       "publicPort": 1, "version": "test"}))
        while True:
            msg = json.loads(await asyncio.wait_for(self.ws.recv(), 10))
            if msg.get("type") == "welcome":
                return self

    async def __aexit__(self, *a):
        await self.ws.close()

    async def commit(self, match, player):
        self.rid += 1
        rid = self.rid
        await self.ws.send(json.dumps({"type": "commit", "rid": rid, "lobbyId": "al1", "matchId": match, "final": True,
                                       "map": "Tortuga", "mode": 0, "custom": False, "players": [player]}))
        while True:
            msg = json.loads(await asyncio.wait_for(self.ws.recv(), 15))
            if msg.get("type") == "commit-reply" and msg.get("rid") == rid:
                return msg


def raid_rows(account_id):
    return query("SELECT map_id || ':' || completions || ':' || attempts || ':' || ifnull(best_time, 0) || ':' ||"
                 " ai_kills || ':' || boss_kills || ':' || deaths FROM raid_stats WHERE account_id = %d" % account_id
                 if DRIVER == "sqlite" else
                 "SELECT map_id || ':' || completions || ':' || attempts || ':' || coalesce(best_time, 0) || ':' ||"
                 " ai_kills || ':' || boss_kills || ':' || deaths FROM raid_stats WHERE account_id = %d" % account_id)


def board(port, path):
    try:
        with urllib.request.urlopen("http://127.0.0.1:%d%s" % (port, path), timeout=10) as r:
            return r.read().decode()
    except Exception as e:  # noqa: BLE001
        return "ERR %r" % e


async def scenario(port):
    now = time.time()
    name = "alife" + str(int(now) % 100000)
    password = "alife_pass1"
    async with Social("ws://127.0.0.1:%d/social/ws" % port) as sock:
        r = await sock.call("a", "a", 0, [name, password, 0], "captcha")
        check(r[1] is None and isinstance(r[2], int), "an account registered: %r" % (r[:3],))
        account, token = r[2], r[5]
        info = r[6] if len(r) > 6 else {}
        check(info.get("reqDel") is False and info.get("raidStats") == [] and info.get("twitchId") is None,
              "account info extras: reqDel false, empty raidStats, no twitchId: %r" % ({k: info.get(k) for k in ("reqDel", "raidStats", "twitchId")},))

        # Twitch without a configured app: v10's unavailable state, empty drops, unlink still answers
        r = await sock.call("twitchAuth", "twitchAuth")
        check(r[1] is None and isinstance(r[2], str) and "unavailable" in r[2], "twitchAuth with no Twitch app: %r" % (r,))
        r = await sock.call("twitchDropsResponse", "getTwitchDrops")
        check(r[1] == [], "getTwitchDrops: no campaigns: %r" % (r,))
        r = await sock.call("twitchRemoveResponse", "removeTwitchLink")
        check(r[0] == "twitchRemoveResponse", "removeTwitchLink answers")
        r = await sock.call("twitchVerifyResponse", "verifyTwitch", "some-code")
        check(isinstance(r[1], str) and "unavailable" in r[1], "verifyTwitch with no Twitch app: %r" % (r,))

        # v10.0.3's password change: the new password, its repeat and the session token (no old password)
        r = await sock.call("upp", "upp", "alife_pass2", "alife_pass2", token)
        check(r[1] == "Password updated." and isinstance(r[2], str) and r[2], "upp (v10 shape): %r" % (r[:2],))
        token = r[2] if len(r) > 2 and r[2] else token
        r = await sock.call("upp", "upp", "alife_pass3", "alife_pass3", token)
        check(isinstance(r[1], str) and "30 minutes" in r[1], "the 30 minute password clock: %r" % (r,))
        r = await sock.login(name, password)
        check(isinstance(r[1], str), "the old password no longer signs in: %r" % (r[1],))
        r = await sock.login(name, "alife_pass2")
        check(r[1] is None and r[2] == account, "the new password signs in")
        token = r[5]
        # the v7 shape still checks the current password (the clock was set back for the test)
        query("UPDATE accounts SET password_changed_at = NULL WHERE id = %d" % account)
        r = await sock.call("upp", "upp", "alife_pass2", "alife_pass3", "alife_pass3")
        check(r[1] == "Password updated.", "upp (old-password shape): %r" % (r[:2],))
        r = await sock.login(name, "alife_pass3")
        check(r[1] is None and r[2] == account, "the third password signs in")

        # deletion: request, cancel. The auth bucket is 0.5/s with a burst of 10 (social_ws.c k_rate);
        # the calls above spent it, so let it refill instead of tripping the limiter.
        await asyncio.sleep(6)
        r = await sock.call("rqd", "rqd", 1)
        check(r[1] is True and r[4] == 1, "rqd 1 accepted: %r" % (r,))
        for _ in range(50):
            if query("SELECT count(*) FROM accounts WHERE id = %d AND delete_requested_at IS NOT NULL" % account) == ["1"]:
                break
            await asyncio.sleep(0.1)
        check(query("SELECT count(*) FROM accounts WHERE id = %d AND delete_requested_at IS NOT NULL" % account) == ["1"],
              "delete_requested_at is set")
        check(query("SELECT count(*) FROM sessions WHERE account_id = %d" % account) == ["0"], "every session ended")
        r = await sock.login(name, "alife_pass3")
        check(r[1] is None and r[6].get("reqDel") is True, "a password login comes back to reqDel true: %r" % (r[6].get("reqDel"),))
        r = await sock.call("rqd", "rqd", 0)
        check(r[1] is True and r[4] == 0, "rqd 0 cancels: %r" % (r,))
        for _ in range(50):
            if query("SELECT count(*) FROM accounts WHERE id = %d AND delete_requested_at IS NOT NULL" % account) == ["0"]:
                break
            await asyncio.sleep(0.1)
        check(query("SELECT count(*) FROM accounts WHERE id = %d AND delete_requested_at IS NOT NULL" % account) == ["0"],
              "the request is cancelled")

        # the raid record: a clear, a duplicate commit, a failed run, a faster clear
        async with Node(port) as node:
            player = {"accountId": account, "name": name, "kills": 4, "deaths": 2, "score": 400, "won": True,
                      "timePlayed": 60000, "games": 1, "kr": 0, "xp": 400, "classIndex": 0,
                      "raid": {"map": 18, "cleared": True, "time": 120000, "aiKills": 10, "bossKills": 1, "deaths": 2}}
            reply = await node.commit("r1", player)
            check(reply.get("ok"), "the raid commit is accepted: %s" % reply)
            for _ in range(60):
                if raid_rows(account):
                    break
                await asyncio.sleep(0.1)
            check(raid_rows(account) == ["18:1:1:120000:10:1:2"], "the first run: %s" % raid_rows(account))
            await node.commit("r1", player)
            await asyncio.sleep(0.8)
            check(raid_rows(account) == ["18:1:1:120000:10:1:2"], "the same match again adds nothing: %s" % raid_rows(account))
            player["raid"] = {"map": 18, "cleared": False, "time": 0, "aiKills": 5, "bossKills": 0, "deaths": 3}
            await node.commit("r2", player)
            for _ in range(60):
                if raid_rows(account) and raid_rows(account)[0].split(":")[2] == "2":
                    break
                await asyncio.sleep(0.1)
            check(raid_rows(account) == ["18:1:2:120000:15:1:5"], "a failed run: an attempt, no clear, no time: %s" % raid_rows(account))
            player["raid"] = {"map": 18, "cleared": True, "time": 90000, "aiKills": 2, "bossKills": 2, "deaths": 0}
            await node.commit("r3", player)
            for _ in range(60):
                if raid_rows(account) and raid_rows(account)[0].split(":")[3] == "90000":
                    break
                await asyncio.sleep(0.1)
            check(raid_rows(account) == ["18:2:3:90000:17:3:5"], "a faster clear: best time and boss kills: %s" % raid_rows(account))

        await asyncio.sleep(4)  # the auth bucket again (raidStats' login below)
        r = await sock.login(name, "alife_pass3")
        stats = r[6].get("raidStats")
        check(isinstance(stats, list) and len(stats) == 1 and stats[0].get("map_id") == 18 and stats[0].get("best_time") == 90000
              and stats[0].get("completions") == 2 and stats[0].get("attempts") == 3,
              "account info raidStats: %r" % (stats,))
        text = board(port, "/leaderboards/raids/18")
        check(name in text and "90000" in text, "the all-time raid leaderboard: %.200s" % text)
        text = board(port, "/leaderboards/raids/18/monthly?month=" + time.strftime("%Y-%m", time.gmtime()))
        check(name in text and "90000" in text, "the monthly raid leaderboard: %.200s" % text)
        text = board(port, "/leaderboards/raids/notamap")
        check(name not in text, "a bad map gives no rows: %.120s" % text)

        # the grace-period sweep (sqlite: timestamps edited directly)
        if DRIVER == "sqlite":
            query("UPDATE accounts SET delete_requested_at = datetime('now', '-20 days'), last_login_at = datetime('now', '-21 days')"
                  " WHERE id = %d" % account)
            gone = False
            for _ in range(600):  # the sweep's first run is 30 s after startup
                if query("SELECT count(*) FROM accounts WHERE id = %d" % account) == ["0"]:
                    gone = True
                    break
                await asyncio.sleep(0.1)
            check(gone, "the grace period ran out: the account and its data are deleted")
            check(query("SELECT count(*) FROM raid_stats WHERE account_id = %d" % account) == ["0"], "the raid rows went with it")


def main():
    global DBCFG
    scratch = [sys.executable, "-I", os.path.join(ROOT, "services", "brain", "tests", "db_scratch.py"), ROOT]
    r = subprocess.run(scratch + ["create", DRIVER, SCHEMA], capture_output=True)
    if r.returncode:
        print("account life test: %s unavailable: skipped" % DRIVER)
        sys.exit(r.returncode)
    DBCFG = json.loads(r.stdout)
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=SECRET, **{"KN_DATABASE_" + k.upper(): str(v) for k, v in DBCFG.items()})
    work = tempfile.mkdtemp(prefix="kn-acctlife.", dir=os.path.join(os.path.expanduser("~"), ".cache"))
    log_path = os.path.join(work, "brain.log")
    proc = None
    try:
        subprocess.run([BRAIN, "--migrate-only"], cwd=ROOT, env=env, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        with open(log_path, "w") as log:
            proc = subprocess.Popen([BRAIN, "--brain.host=127.0.0.1", "--brain.port=0", "--brain.id=kal-%d" % os.getpid(),
                                     "--accounts.login_throttle_s=0", "--accounts.deletion_sweep_s=1"],
                                    cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT)
        port = bound_port(log_path, proc)
        asyncio.run(asyncio.wait_for(scenario(port), 240))
    except Exception as e:  # noqa: BLE001
        import traceback
        traceback.print_exc()
        FAILS.append("exception: %r" % (e,))
    finally:
        if proc:
            proc.terminate()
            try:
                proc.wait(10)
            except subprocess.TimeoutExpired:
                proc.kill()
        subprocess.run(scratch + ["drop", DRIVER, SCHEMA], capture_output=True)
    with open(log_path) as f:
        bad = [l.strip() for l in f if re.search(r"failed:|ERROR|FATAL", l)]
    check(not bad, "no statement errors in the brain log %s" % bad[:3])
    print("account life test: " + ("OK" if not FAILS else "%d failure(s)" % len(FAILS)))
    if FAILS:
        print("log kept in " + work)
    else:
        shutil.rmtree(work, ignore_errors=True)
    sys.exit(1 if FAILS else 0)


main()
