#!/usr/bin/env python3
"""Krunker v10.0.2 Turf Wars end to end (doc/v10/ranked-impl.md): kn_brain + a fake game server on
the internal WS, 3 clans of accounts registered over the social socket.

  turf_test.py <kn_brain> <repo root> [database]
Same database rules as ranked_test.py (per-run schema / scratch database, 77 = skip).

Covers: "turf-kills" frames (reliable: acknowledged with turf-kills-reply), unknown clans and maps
dropped, the top-20 cap per clan and map, the daily settlement on the next day (brain.turf.day)
once (a second brain does not pay again), the controller in /api/territorial-wars/status, the
60 / 30 / 10 split, the loot configs (clan_bank -> the clan's funds; even / performance -> the
members' KR), the clan / player / leaderboard routes in v10's {data: ...} shapes, PUT loot-config
permissions, recruitment posts (1,000 KR from the clan bank on create, owner / Captain+ only,
validation, the 30 s cooldown, apply, my-applications, delete), and clean logs.
"""
import asyncio
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.parse
import urllib.request

try:
    import websockets
except ImportError:  # pragma: no cover
    print("python websockets missing: skip")
    sys.exit(77)

BRAIN, ROOT = sys.argv[1], sys.argv[2]
DB = sys.argv[3] if len(sys.argv) > 3 else os.environ.get("KN_MM_TEST_DATABASE", "")
if not DB:
    try:
        with open(os.path.join(ROOT, "config", "config.json")) as fp:
            DB = json.load(fp).get("database", {}).get("name", "")
    except (OSError, ValueError):
        DB = ""
DRIVER = os.environ.get("KN_TEST_DB_DRIVER", "postgres")
if not DB and DRIVER == "postgres":
    print("turf test: no database configured; skipped")
    sys.exit(77)
SCHEMA = "kn_test_turf_%d" % os.getpid()
SECRET = "kn-turf-test-%d" % os.getpid()
FAILS = []
DAY = 30


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


def http(port, path, method="GET", body=None, headers=None, **q):
    url = "http://127.0.0.1:%d%s" % (port, path)
    if q:
        url += "?" + urllib.parse.urlencode(q)
    data = json.dumps(body).encode() if body is not None else None
    h = dict(headers or {})
    if data:
        h["Content-Type"] = "application/json"
    req = urllib.request.Request(url, data=data, method=method, headers=h)
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            return r.status, json.loads(r.read())
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b"null")


class Social:
    def __init__(self, url):
        self.url = url + "?enc=json"
        self.inbox = []

    async def __aenter__(self):
        self.ws = await websockets.connect(self.url)
        return self

    async def __aexit__(self, *a):
        await self.ws.close()

    async def send(self, *frame):
        await self.ws.send(json.dumps(list(frame)))

    async def call(self, label, *frame):
        await self.send(*frame)
        deadline = time.time() + 10
        while True:
            for i, f in enumerate(self.inbox):
                if f[0] == label:
                    return self.inbox.pop(i)
            self.inbox.append(json.loads(await asyncio.wait_for(self.ws.recv(), deadline - time.time())))


async def register(port, name):
    async with Social("ws://127.0.0.1:%d/social/ws" % port) as s:
        r = await s.call("a", "a", 0, [name, "turfpass1", 0], "captcha")
        assert r[1] is None, r
        return {"id": r[2], "name": name, "token": r[5]}


async def make_clan(port, owner, tag, members):
    url = "ws://127.0.0.1:%d/social/ws" % port
    async with Social(url) as o:
        await o.call("a", "a", 1, [owner["name"], None, owner["token"]])
        r = await o.call("cln", "cln", 1, tag, None)
        assert r[1] is None, r
        for m in members:
            async with Social(url) as s:
                await s.call("a", "a", 1, [m["name"], None, m["token"]])
                r = await s.call("cln", "cln", tag.lower())
                assert r[1] == "Request Sent", r
            await o.send("clnR", m["id"], 1)
            await asyncio.sleep(0.2)
        r = await o.call("cln", "cln", 3, 1)
        return r[2]


class FakeNode:
    def __init__(self, port):
        self.url = "ws://127.0.0.1:%d/mm/internal/ws" % port
        self.replies = {}
        self.rid = 500

    async def start(self):
        self.ws = await websockets.connect(self.url, additional_headers={"Authorization": "Bearer " + SECRET})
        await self.ws.send(json.dumps({"type": "hello", "nodeId": "TRF", "region": "us-nj", "publicHost": "127.0.0.1",
                                       "publicPort": 23998, "version": "test", "maxLobbies": 1}))
        self.task = asyncio.ensure_future(self.loop())

    async def loop(self):
        async for raw in self.ws:
            f = json.loads(raw)
            if f.get("type") == "ping":
                await self.ws.send(json.dumps({"type": "pong", "ts": f.get("ts")}))
            elif str(f.get("type", "")).endswith("-reply") and "rid" in f:
                self.replies[f["rid"]] = f

    async def kills(self, map_id, records):
        self.rid += 1
        await self.ws.send(json.dumps({"type": "turf-kills", "rid": self.rid, "matchId": "m%d" % self.rid, "mapId": map_id, "mode": 0,
                                       "records": records}))
        for _ in range(100):
            if self.rid in self.replies:
                return self.replies.pop(self.rid)
            await asyncio.sleep(0.05)
        return None


def start(work, env, name, extra):
    path = os.path.join(work, name + ".log")
    log = open(path, "w")
    p = subprocess.Popen([BRAIN, "--brain.host=127.0.0.1", "--brain.port=0", "--brain.id=" + name, "--brain.region=us-nj",
                          "--brain.turf.settle_check_ms=300", "--brain.turf.kr_per_kill=10"] + extra, cwd=ROOT, env=env,
                         stdout=log, stderr=subprocess.STDOUT)
    return p, path, bound_port(path, p)


async def kr_of(port, acc):
    """the account's KR (funds) through the internal accounts op"""
    async with websockets.connect("ws://127.0.0.1:%d/mm/internal/ws" % port, additional_headers={"Authorization": "Bearer " + SECRET}) as ws:
        await ws.send(json.dumps({"type": "hello", "nodeId": "TRQ", "region": "us-nj", "publicHost": "127.0.0.1", "publicPort": 23997,
                                  "version": "test", "maxLobbies": 1}))
        await ws.send(json.dumps({"type": "acct", "rid": 7, "op": "token", "args": {"token": acc["token"]}}))
        while True:
            m = json.loads(await asyncio.wait_for(ws.recv(), 10))
            if m.get("type") == "acct-r" and m.get("rid") == 7:
                return (m.get("data") or {}).get("funds")


async def deposit(port, acc, amount):
    """the clan bank deposit of the clan menu (["cln", 7, amount, {wd: 0}]): the clan gets 90%"""
    async with Social("ws://127.0.0.1:%d/social/ws" % port) as s:
        await s.call("a", "a", 1, [acc["name"], None, acc["token"]])
        return await s.call("cln", "cln", 7, amount, {"wd": 0})


def recruitment(port, accs, clan_a, clan_c):
    """v10's recruitment posts (0208_clan_recruitment): CCC's bank holds 125 KR after the settlement"""
    auth = lambda a: {"Authorization": "Bearer " + a["token"]}
    url = "/api/clans/%d/recruitment" % clan_c
    post = {"region": "EU", "language": "en", "description": "Daily Turf Wars on Sandstorm", "looking_for": "active players",
            "min_level": 0, "min_kd": 1.5}
    code, r = http(port, url)
    check(code == 200 and r["data"] is None, "no post yet: data null (%s %s)" % (code, r))
    code, r = http(port, url, method="POST", body=post)
    check(code == 401, "posting needs a login (%s)" % code)
    code, r = http(port, url, method="POST", headers=auth(accs[7]), body=post)
    check(code == 400 and "owner" in str(r.get("message", r)), "a Recruit cannot post (%s %s)" % (code, r))
    code, r = http(port, url, method="POST", headers=auth(accs[6]), body=dict(post, region="XX"))
    check(code == 400 and "region" in str(r.get("message", r)), "unknown region refused (%s)" % r)
    code, r = http(port, url, method="POST", headers=auth(accs[6]), body=dict(post, min_kd=50))
    check(code == 400 and "K/D" in str(r.get("message", r)), "min K/D out of range refused (%s)" % r)
    code, r = http(port, url, method="POST", headers=auth(accs[6]), body=post)
    check(code == 400 and "clan funds" in str(r.get("message", r)), "125 KR in the bank: not enough for 1,000 (%s)" % r)
    d = asyncio.run(deposit(port, accs[6], 1000))
    check(d[1] is None, "the owner deposits 1,000 KR (the bank gets 900) (%s)" % (d,))
    code, r = http(port, url, method="POST", headers=auth(accs[6]), body=post)
    data = r.get("data") or {} if code == 200 else {}
    check(code == 200 and data.get("charged") is True and data.get("clan_funds") == 125 + 900 - 1000 and data.get("region") == "EU"
          and data.get("clan_name") == "CCC" and abs(data.get("min_kd", 0) - 1.5) < 1e-6 and data.get("is_active") is True,
          "the post is created: 1,000 KR from the clan bank (%s %s)" % (code, r))
    code, r = http(port, url, method="PUT", headers=auth(accs[6]), body=dict(post, description="again"))
    check(code == 400 and "moment" in str(r.get("message", r)), "a second update within 30 s is refused (%s)" % r)
    code, r = http(port, url)
    check(code == 200 and (r.get("data") or {}).get("description") == post["description"], "GET the clan's post")
    code, r = http(port, "/api/clans/recruitment", page=1, limit=10)
    posts = (r.get("data") or {}).get("posts", []) if code == 200 else []
    check([p["clan_name"] for p in posts] == ["CCC"] and r["data"]["total"] == 1, "the recruitment list (%s)" % posts)
    code, r = http(port, "/api/clans/recruitment", region="NA")
    check(code == 200 and r["data"]["posts"] == [], "the list filtered by region")
    code, r = http(port, "/api/clans/%d/apply" % clan_c, method="POST", headers=auth(accs[0]))
    check(code == 400 and "already in a clan" in str(r.get("message", r)), "a clan member cannot apply (%s)" % r)
    code, r = http(port, "/api/clans/%d/apply" % clan_c, method="POST", headers=auth(accs[9]))
    check(code == 200 and r["data"]["message"] == "Application sent!", "a clanless player applies (%s %s)" % (code, r))
    code, r = http(port, "/api/clans/my-applications", headers=auth(accs[9]))
    check(code == 200 and r["data"]["clan_ids"] == [clan_c], "my-applications lists CCC (%s)" % r)
    code, r = http(port, "/api/clans/%d/recruitment" % clan_a, method="DELETE", headers=auth(accs[6]))
    check(code == 400, "another clan's owner cannot remove AAA's post (%s)" % code)
    code, r = http(port, url, method="DELETE", headers=auth(accs[6]))
    check(code == 200 and r["data"]["success"] is True, "the owner removes the post (%s)" % r)
    code, r = http(port, url)
    check(code == 200 and (r.get("data") is None or r["data"].get("is_active") is False), "removed: no active post (%s)" % r)
    code, r = http(port, "/api/clans/recruitment")
    check(code == 200 and r["data"]["posts"] == [], "removed: not listed")


def main():
    if DRIVER == "postgres":
        pg = [sys.executable, "-I", os.path.join(ROOT, "services", "brain", "tests", "pg_schema.py"), ROOT, DB]
        rc = subprocess.run(pg + ["create", SCHEMA]).returncode
        if rc:
            sys.exit(rc)
        dbenv = {"KN_DATABASE_NAME": DB, "KN_DATABASE_SEARCH_PATH": SCHEMA}
        drop = pg + ["drop", SCHEMA]
    else:
        pg = [sys.executable, "-I", os.path.join(ROOT, "services", "brain", "tests", "db_scratch.py"), ROOT]
        r = subprocess.run(pg + ["create", DRIVER, SCHEMA], capture_output=True)
        if r.returncode:
            print("turf test: %s unavailable: skipped" % DRIVER)
            sys.exit(r.returncode)
        dbenv = {"KN_DATABASE_" + k.upper(): str(v) for k, v in json.loads(r.stdout).items()}
        drop = pg + ["drop", DRIVER, SCHEMA]
    work = tempfile.mkdtemp(prefix="kn-turf.")
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=SECRET, **dbenv)
    procs, logs = [], []
    try:
        subprocess.run([BRAIN, "--migrate-only", "--database.migrations_dir=" + os.path.join(ROOT, "services", "db", "migrations")],
                       cwd=ROOT, env=env, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        p, path, port = start(work, env, "turf-a-%d" % os.getpid(), ["--brain.turf.day=%d" % DAY, "--accounts.starting_kr=2000"])
        procs.append(p)
        logs.append(path)
        stamp = str(int(time.time()) % 100000)

        async def setup():
            accs = [await register(port, "t%d%s" % (i, stamp)) for i in range(10)]  # accs[9]: no clan
            await make_clan(port, accs[0], "AAA", accs[1:3])
            await make_clan(port, accs[3], "BBB", accs[4:6])
            await make_clan(port, accs[6], "CCC", accs[7:9])
            return accs
        accs = asyncio.run(setup())

        async def day_one():
            node = FakeNode(port)
            await node.start()
            await asyncio.sleep(0.5)
            rec = lambda a, clan, k, d=0: {"clan_name": clan, "player_id": a["id"], "map_id": 0, "kills": k, "deaths": d}
            # Burg: AAA 30 (+ a stranger clan, an over-large record), BBB 20, CCC 10
            r = await node.kills(0, [rec(accs[0], "AAA", 20), rec(accs[1], "AAA", 10, 4), rec(accs[3], "bbb", 20, 7), rec(accs[6], "CCC", 10),
                                     {"clan_name": "ZZZ", "player_id": accs[2]["id"], "map_id": 0, "kills": 5, "deaths": 0},
                                     rec(accs[2], "AAA", 5000)])
            check(r and r.get("ok") and r.get("recorded") == 4, "turf-kills acknowledged: 4 recorded (unknown clan, absurd count dropped) (%s)" % r)
            r = await node.kills(0, [rec(accs[0], "AAA", 5)])
            check(r and r.get("recorded") == 1, "a second round adds up")
            r = await node.kills(7, [rec(accs[0], "AAA", 5)])
            check(r and r.get("ok") is False, "not a turf map: refused")
            # Sandstorm: CCC only
            r = await node.kills(2, [dict(rec(accs[7], "CCC", 8), map_id=2), dict(rec(accs[8], "CCC", 2), map_id=2)])
            check(r and r.get("recorded") == 2, "Sandstorm records")
            node.task.cancel()
            await node.ws.close()
        asyncio.run(day_one())

        code, st = http(port, "/api/territorial-wars/status")
        burg = [m for m in st.get("data", []) if m["map_id"] == 0][0] if code == 200 else {}
        check(code == 200 and len(st["data"]) == 3 and burg.get("total_kills_today") == 65 and burg.get("guaranteed_reward") == 650
              and burg.get("estimated_reward", 0) >= 650 and burg.get("map_name") == "Burg"
              and burg["leaderboard"][0]["clan_name"] == "AAA" and burg["leaderboard"][0]["kills"] == 35 and burg["leaderboard"][0]["position"] == 1
              and burg.get("clan_id") is None,
              "status today: Burg 65 kills (650 KR guaranteed), AAA leads 35, nobody controls it yet (%s)"
              % {k: burg.get(k) for k in ("total_kills_today", "guaranteed_reward", "estimated_reward", "clan_id")})
        code, lb = http(port, "/api/territorial-wars/leaderboards/top-performing-clans", day=DAY, map_id=0)
        check(code == 200 and [r["clan_name"] for r in lb["data"]] == ["AAA", "BBB", "CCC"] and lb["data"][0]["position"] == 1,
              "top performing clans today")
        code, lb = http(port, "/api/territorial-wars/leaderboards/top-performing-clans", day=DAY, clan_name="bbb")
        check(code == 200 and [r["clan_name"] for r in lb["data"]] == ["BBB"], "top performing clans: search by name")
        code, pl = http(port, "/api/territorial-wars/clan/AAA/players", day=DAY, map_id=0)
        check(code == 200 and pl["data"][0]["kills"] == 25 and pl["data"][0]["games"] == 2 and pl["data"][0]["kd"] == 25.0,
              "clan players (by tag) (%s)" % (pl.get("data") or [None])[0])
        code, cl = http(port, "/api/territorial-wars/clan/AAA")
        maps = {m["map_name"]: m for m in (cl.get("data") or {}).get("maps", [])}
        check(code == 200 and cl["data"]["clan_name"] == "AAA" and cl["data"]["current_day"] == DAY and maps.get("Burg", {}).get("position") == 1
              and maps["Burg"].get("comparison_clan_name") == "BBB", "clan view: today's Burg, #1 ahead of BBB (%s)" % maps.get("Burg"))
        code, nf = http(port, "/api/territorial-wars/clan/NOPE")
        check(code == 404, "unknown clan: 404 (%s)" % code)

        # loot configs: AAA even split with a 20% treasury cut, BBB performance, CCC the bank (default)
        clan_a = http(port, "/api/territorial-wars/clan/AAA")[1]["data"]["clan_id"]
        clan_b = http(port, "/api/territorial-wars/clan/BBB")[1]["data"]["clan_id"]
        clan_c = http(port, "/api/territorial-wars/clan/CCC")[1]["data"]["clan_id"]
        auth = lambda a: {"Authorization": "Bearer " + a["token"]}
        code, r = http(port, "/api/clans/%d/loot-config" % clan_a, method="PUT", headers=auth(accs[1]),
                       body={"distribution_mode": "even", "clan_pct": 20})
        check(code == 403, "a Recruit cannot change the loot config")
        code, r = http(port, "/api/clans/%d/loot-config" % clan_a, method="PUT", headers=auth(accs[0]),
                       body={"distribution_mode": "even", "clan_pct": 20, "min_games_required": 1})
        check(code == 200 and r["data"]["distribution_mode"] == "even" and r["data"]["clan_pct"] == 20
              and r["data"].get("updated_by_name") == accs[0]["name"] and str(r["data"].get("updated_at", "")).endswith("Z"),
              "owner sets AAA: even, 20%% bank; v10's updated_by_name / updated_at (%s)" % r.get("data"))
        code, r = http(port, "/api/clans/%d/loot-config" % clan_b, method="PUT", headers=auth(accs[3]),
                       body={"distribution_mode": "performance", "clan_pct": 150})
        check(code == 200 and r["data"]["clan_pct"] == 100, "values are clamped (clan_pct 150 -> 100)")
        http(port, "/api/clans/%d/loot-config" % clan_b, method="PUT", headers=auth(accs[3]), body={"distribution_mode": "performance", "clan_pct": 0})
        code, r = http(port, "/api/clans/%d/loot-config" % clan_c)
        check(code == 200 and r["data"]["distribution_mode"] == "clan_bank" and r["data"]["top_player_count"] == 20
              and r["data"]["config"][0]["distribution_mode"] == "clan_bank", "CCC: the default config (v10's flat shape + config[0])")

        before = {a["name"]: asyncio.run(kr_of(port, a)) for a in accs}
        # the next day: brain A (and a second brain) settle DAY once
        procs[0].terminate()
        procs[0].wait(5)
        procs = []
        for name in ("turf-b-%d" % os.getpid(), "turf-c-%d" % os.getpid()):
            p, path, pt = start(work, env, name, ["--brain.turf.day=%d" % (DAY + 1)])
            procs.append(p)
            logs.append(path)
            port = pt
        time.sleep(2.5)
        code, st = http(port, "/api/territorial-wars/status")
        burg = [m for m in st.get("data", []) if m["map_id"] == 0][0] if code == 200 else {}
        sand = [m for m in st.get("data", []) if m["map_id"] == 2][0] if code == 200 else {}
        sub = [m for m in st.get("data", []) if m["map_id"] == 3][0] if code == 200 else {}
        check(burg.get("clan_name") == "AAA" and burg.get("kills") == 35 and sand.get("clan_name") == "CCC" and sub.get("clan_id") is None
              and burg.get("total_kills_today") == 0, "next day: AAA controls Burg, CCC Sandstorm, Subzero nobody")
        total = lambda e: sum(r["reward"] for r in (e.get("data") or [])) if isinstance(e, dict) else None
        code, earn = http(port, "/api/territorial-wars/clan/%d/earnings" % clan_a)
        check(code == 200 and total(earn) == 390 and earn["data"][0]["day"] == DAY and earn["data"][0]["map_name"] == "Burg",
              "AAA earned 60%% of 650 = 390 (%s)" % earn.get("data"))
        code, earn = http(port, "/api/territorial-wars/clan/CCC/earnings")
        check(code == 200 and total(earn) == 65 + 60, "CCC: 10%% of Burg + 60%% of Sandstorm's 100 (alone: the 2nd / 3rd shares stay unpaid) (%s)" % earn.get("data"))
        code, pay = http(port, "/api/territorial-wars/clan/%d/payouts" % clan_a, day=DAY)
        rows = pay["data"]["payouts"] if code == 200 else []
        # AAA: 390 - 78 (20%) = 312 over its 2 eligible members (25 and 10 kills) -> 156 each; the bank 78 (top grossing below)
        check(sorted(r["total_kr"] for r in rows) == [156, 156] and pay["data"]["total"] == 312 and all(r["map_name"] == "Burg" for r in rows),
              "AAA even split: 156 + 156 (%s)" % rows)
        code, pay = http(port, "/api/territorial-wars/clan/%d/payouts" % clan_a)
        check(code == 200 and sorted(r["total_kr"] for r in pay["data"]["payouts"]) == [156, 156] and "day" not in pay["data"],
              "AAA payouts, all time (%s)" % pay.get("data"))
        code, pay = http(port, "/api/territorial-wars/clan/%d/payouts" % clan_b, day=DAY)
        rows = pay["data"]["payouts"] if code == 200 else []
        check([r["total_kr"] for r in rows] == [195], "BBB performance: its one contributor gets the whole 30%% (195) (%s)" % rows)
        code, pay = http(port, "/api/territorial-wars/clan/%d/payouts" % clan_c, day=DAY)
        rows = pay["data"]["payouts"] if code == 200 else None
        check(rows == [], "CCC clan bank: no player payouts (%s)" % rows)
        after = {a["name"]: asyncio.run(kr_of(port, a)) for a in accs}
        gain = lambda a: after[a["name"]] - before[a["name"]] if after[a["name"]] is not None and before[a["name"]] is not None else None
        check(gain(accs[0]) == 156 and gain(accs[1]) == 156 and gain(accs[3]) == 195 and gain(accs[6]) == 0,
              "the members' KR (%s)" % [gain(a) for a in (accs[0], accs[1], accs[3], accs[6])])
        code, top = http(port, "/api/territorial-wars/leaderboards/top-grossing-clans")
        rows = top.get("data") or [] if code == 200 else []
        check([r["clan_name"] for r in rows] == ["AAA", "BBB", "CCC"] and [r["total_kr"] for r in rows] == [390, 195, 125]
              and [r["bank"] for r in rows] == [78, 0, 125], "top grossing clans: totals and what stayed in the bank (%s)" % rows)
        code, tp = http(port, "/api/territorial-wars/leaderboards/top-performing-players", map_id=0)
        check(code == 200 and tp["data"][0]["player_name"] == accs[0]["name"] and tp["data"][0]["clan_name"] == "AAA" and tp["data"][0]["earnings"] == 156,
              "top performing players (%s)" % (tp.get("data") or [None])[0])
        code, pp = http(port, "/api/territorial-wars/player/%s" % accs[0]["name"])
        check(code == 200 and pp["data"]["total_kills"] == 25 and pp["data"]["total_kr"] == 156 and pp["data"]["current_clan"] == "AAA",
              "player profile by name (%s)" % pp.get("data"))
        code, pc = http(port, "/api/territorial-wars/player/%d/contributions" % accs[0]["id"], day=DAY)
        check(code == 200 and len(pc["data"]) == 1 and pc["data"][0]["kills"] == 25, "player contributions of the day")
        code, pe = http(port, "/api/territorial-wars/player/%d/earnings" % accs[0]["id"])
        check(code == 200 and [r["amount"] for r in pe["data"]] == [156], "player earnings")
        recruitment(port, accs, clan_a, clan_c)
        # v10's HTTP leaderboards (leaderboards.c): {data: [rows with position]}
        code, lb = http(port, "/api/leaderboards/player_funds")
        vals = [r["value"] for r in lb.get("data", [])] if code == 200 else []
        check(len(vals) >= 10 and vals == sorted(vals, reverse=True) and lb["data"][0]["position"] == 1
              and all(isinstance(r["player_clan"], str) for r in lb["data"]), "leaderboard player_funds (%d rows)" % len(vals))
        code, lb = http(port, "/api/leaderboards/player_clan", sort="members")
        mine = {r["clan_name"]: r["member_count"] for r in lb.get("data", [])} if code == 200 else {}
        check(mine.get("AAA") == 3 and mine.get("BBB") == 3 and mine.get("CCC") == 3, "leaderboard clans by members (%s)" % mine)
        code, lb = http(port, "/api/leaderboards/raids/0")
        check(code == 200 and lb["data"] == [], "raids: no data yet")
        code, lb = http(port, "/api/leaderboards/player_skinvalue")
        check(code == 200 and lb["data"] == [], "inventory value: nothing to rank")
        time.sleep(1.0)
        code, earn = http(port, "/api/territorial-wars/clan/%d/earnings" % clan_a)
        check(code == 200 and total(earn) == 390, "two brains settled the day once")
    finally:
        for p in procs:
            if p.poll() is None:
                p.terminate()
        for p in procs:
            try:
                p.wait(5)
            except subprocess.TimeoutExpired:
                p.kill()
        subprocess.run(drop)
    bad = []
    for path in logs:
        with open(path) as f:
            bad += [l.strip() for l in f if re.search(r"failed:|ERROR|FATAL", l)]
    check(not bad, "no statement errors in the brain logs %s" % bad[:3])
    print("turf test: " + ("OK" if not FAILS else "%d failure(s)" % len(FAILS)))
    if FAILS:
        print("logs kept in " + work)
    else:
        shutil.rmtree(work, ignore_errors=True)
    sys.exit(1 if FAILS else 0)


main()
