#!/usr/bin/env python3
"""Ranked queue end to end (doc/backend.md 6.5, Krunker v7.1.4 ranked): 2 brains sharing one
database (plus a third with the default level gate and another season), a fake game server on
brain A, 8 accounts registered over the social socket.

  ranked_test.py <kn_brain> <repo root> [database]
Database: argument, else $KN_MM_TEST_DATABASE, else config/config.json's
database.name; every run uses its own schema (kn_test_rq_<pid>, dropped after)
and ephemeral ports, so concurrent runs never interfere. 77 = skip (no PostgreSQL).
KN_TEST_DB_DRIVER=sqlite|mysql runs the brains on that store driver instead
(db_scratch.py: one temp sqlite file, or a scratch MySQL/MariaDB database).

Covers the one "ranked" queue (4v4) on both brains, cross-brain matching + hosting (the node
lives on brain A), parties kept on one team, Join tickets, the ranked "host" config (a comp
lobby: Hardpoint, a comp-pool catalogue map, teamSize 4), AccountAlreadyPlaying, ranked-result
-> per-season ratings (eloK 32 vs the other team's average, floored at 0) on /mm/queue/rating,
abandon -> rankedDeduction + RankedCooldown, parties (join by id, ready, class, kick, promote,
leave), the removed v3.1.1 queues (InvalidQueue, no update-queues), LevelTooLow (minRankedLevel
15), another season's empty rating, InvalidClientKey, and that the brain logs carry no
statement errors.
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
if not DB and os.environ.get("KN_TEST_DB_DRIVER", "postgres") == "postgres":
    print("ranked test: no database configured; skipped")
    sys.exit(77)
SCHEMA = "kn_test_rq_%d" % os.getpid()
SECRET = "kn-ranked-test-%d" % os.getpid()
FAILS = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what, flush=True)
    if not cond:
        FAILS.append(what)


def bound_port(log_path, proc):
    """brain.port=0: the kernel picks the port and the brain logs it."""
    for _ in range(200):
        if proc.poll() is not None:
            raise RuntimeError("kn_brain exited early (%s)" % log_path)
        with open(log_path) as fp:
            m = re.search(r"listening on http://127\.0\.0\.1:(\d+) \(ws", fp.read())
        if m:
            return int(m.group(1))
        time.sleep(0.05)
    raise RuntimeError("kn_brain did not start listening")


def http(port, path, **q):
    url = "http://127.0.0.1:%d%s" % (port, path)
    if q:
        url += "?" + urllib.parse.urlencode(q)
    try:
        with urllib.request.urlopen(url, timeout=10) as r:
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

    async def call(self, label, *frame):
        await self.ws.send(json.dumps(list(frame)))
        deadline = time.time() + 10
        while True:
            for i, f in enumerate(self.inbox):
                if f[0] == label:
                    return self.inbox.pop(i)
            self.inbox.append(json.loads(await asyncio.wait_for(self.ws.recv(), deadline - time.time())))


class FakeNode:
    """Game server on /mm/internal/ws: hosts ranked lobbies, reports results."""

    def __init__(self, port):
        self.url = "ws://127.0.0.1:%d/mm/internal/ws" % port
        self.hosted = []          # (lobbyId, config)
        self.replies = {}
        self.rid = 100
        self.fail_hosts = 0

    async def start(self):
        self.ws = await websockets.connect(self.url, additional_headers={"Authorization": "Bearer " + SECRET})
        await self.ws.send(json.dumps({"type": "hello", "nodeId": "RKD", "region": "us-nj", "publicHost": "127.0.0.1",
                                       "publicPort": 23999, "version": "test", "custom": True, "maxLobbies": 8}))
        self.lobbies = []
        self.task = asyncio.ensure_future(self.loop())

    def tuples(self):
        return [[lid, "us-nj", 0, 8, {"i": "ranked", "cs": True, "pr": True, "v": "test"}] for lid, _ in self.hosted]

    async def loop(self):
        async for raw in self.ws:
            f = json.loads(raw)
            t = f.get("type")
            if t == "welcome":
                await self.ws.send(json.dumps({"type": "snapshot", "lobbies": self.tuples()}))
            elif t == "ping":
                await self.ws.send(json.dumps({"type": "pong", "ts": f.get("ts")}))
            elif t == "host":
                if self.fail_hosts > 0:
                    self.fail_hosts -= 1
                    await self.ws.send(json.dumps({"type": "host-reply", "rid": f["rid"], "ok": False, "err": "Busy"}))
                    continue
                lid = "rk%03d" % len(self.hosted)
                self.hosted.append((lid, f.get("config")))
                await self.ws.send(json.dumps({"type": "host-reply", "rid": f["rid"], "ok": True, "lobbyId": lid}))
                await self.ws.send(json.dumps({"type": "snapshot", "lobbies": self.tuples()}))
            elif t and t.endswith("-reply") and "rid" in f:
                self.replies[f["rid"]] = f

    async def request(self, frame):
        self.rid += 1
        frame["rid"] = self.rid
        await self.ws.send(json.dumps(frame))
        for _ in range(100):
            if self.rid in self.replies:
                return self.replies.pop(self.rid)
            await asyncio.sleep(0.1)
        return None


async def register(port, name):
    async with Social("ws://127.0.0.1:%d/social/ws" % port) as s:
        r = await s.call("a", "a", 0, [name, "rankedpass1", 0], "captcha")
        assert r[1] is None, r
        return {"id": r[2], "name": name, "token": r[5]}


def join(port, acc, ready=1, party="", queues=None, cls=1):
    q = {"token": acc["token"], "region": "us-nj", "party": party, "ready": ready, "classIndex": cls}
    if queues is not None:
        q["queues"] = json.dumps(queues)
    return http(port, "/mm/queue/join", **q)[1]


def rating(port, acc):
    return http(port, "/mm/queue/rating", token=acc["token"])


async def wait_status(port, key, want, secs=15):
    st = None
    for _ in range(secs * 5):
        code, st = http(port, "/mm/queue/poll", clientKey=key)
        if code == 200 and st["status"]["t"] == want:
            return st
        await asyncio.sleep(0.2)
    return st


COMP_MAPS = [0, 2, 4, 5, 11, 12, 14, 17, 23]  # config.hostTypes[1] overDat maps


async def full_match(pa, pb, accs, node, party_of_two=True):
    """8 accounts into one 4v4: the first two as a party on brain A, the rest solo on A / B."""
    keys = []
    first = join(pa, accs[0], ready=0)
    st = await wait_status(pa, first["clientKey"], "Queued")
    second = join(pa, accs[1], ready=1, party=st["partyId"]) if party_of_two else join(pa, accs[1])
    http(pa, "/mm/queue/update-ready", clientKey=first["clientKey"], isReady="true")
    keys += [(pa, first), (pa, second)]
    for i, acc in enumerate(accs[2:]):
        port = pa if i % 2 else pb
        keys.append((port, join(port, acc)))
    states = [await wait_status(port, k["clientKey"], "Join") for port, k in keys]
    return keys, states


async def scenario(pa, pb, pc, node):
    stamp = str(int(time.time()) % 100000)
    accs = [await register(pa, "r%d%s" % (i, stamp)) for i in range(8)]
    spare = await register(pa, "r8%s" % stamp)
    names = [a["name"] for a in accs]
    alice, bob = accs[0], accs[1]

    print("== 4v4 across two brains")
    code, r0 = rating(pa, alice)
    check(code == 200 and r0.get("rating") == 0 and r0.get("rank") == 1 and r0.get("season") == 7 and r0.get("teamSize") == 4
          and r0.get("cooldownMs") == 0, "rating card before any match (%s)" % r0)
    keys, states = await full_match(pa, pb, accs, node)
    games = {st["status"]["c"].get("gameId") for st in states if st and st["status"]["t"] == "Join"}
    check(len(games) == 1 and all(st and st["status"]["t"] == "Join" for st in states),
          "all 8 reach Join in the same game (%s)" % games)
    c = states[0]["status"]["c"] if states[0] else {}
    check(c.get("gameId", "").startswith("RKD:") and c.get("port") == 23999 and len(c.get("clientId", "")) > 100,
          "Join carries gameId/host/port and a per-player ticket")
    cfg = node.hosted[-1][1] if node.hosted else {}
    teams = cfg.get("teams", [])
    roster = sorted(p["name"] for t in teams for p in t)
    party_team = [i for i, t in enumerate(teams) if alice["name"] in [p["name"] for p in t]]
    check(cfg.get("ranked") and cfg.get("queue") == "ranked" and cfg.get("teamSize") == 4 and cfg.get("mode") == 2
          and cfg.get("modes") == [2] and len(cfg.get("maps", [])) == 1 and cfg["maps"][0] in COMP_MAPS
          and roster == sorted(names) and [len(t) for t in teams] == [4, 4],
          "ranked host config: comp Hardpoint (mode 2), a comp map, 4 + 4 (%s)" % {k: cfg.get(k) for k in ("queue", "mode", "maps", "teamSize")})
    check(party_team and bob["name"] in [p["name"] for p in teams[party_team[0]]], "the party plays on one team")
    again = join(pa, alice)
    check(again["status"] == "AccountAlreadyPlaying", "re-queue during the match -> AccountAlreadyPlaying")
    for port, k in keys:
        http(port, "/mm/queue/leave", clientKey=k["clientKey"])
    win = party_team[0]
    r = await node.request({"type": "ranked-result", "matchId": cfg["matchId"], "lobbyId": node.hosted[-1][0],
                            "winner": win, "players": [], "abandoned": []})
    rat = {x["name"]: x for x in (r or {}).get("ratings", [])}
    losers = [p["name"] for p in teams[1 - win]]
    check(r and r.get("ok") and len(rat) == 8 and rat.get(alice["name"], {}).get("delta") == 16
          and rat[alice["name"]]["new"] == 16 and rat[alice["name"]].get("season") == 7
          and all(rat[n]["new"] == 0 for n in losers), "Elo: winners +16, losers floored at 0 (%s)" % rat.get(alice["name"]))
    r2 = await node.request({"type": "ranked-result", "matchId": cfg["matchId"], "winner": 1 - win})
    check(r2 and r2.get("ok") and r2.get("ratings") == [], "a second result for the same match is ignored")
    await asyncio.sleep(0.5)
    code, ra = rating(pb, alice)
    check(code == 200 and ra.get("rating") == 16 and ra.get("games") == 1 and ra.get("wins") == 1 and ra.get("peak") == 16,
          "season rating on the other brain (%s)" % ra)
    code, rl = rating(pa, [a for a in accs if a["name"] == losers[0]][0])
    check(code == 200 and rl.get("rating") == 0 and rl.get("losses") == 1, "loser's card (%s)" % rl)
    code, rc = rating(pc, alice)
    check(code == 200 and rc.get("season") == 8 and rc.get("rating") == 0 and rc.get("games") == 0,
          "another season starts at 0 (%s)" % rc)
    j3 = join(pa, alice, ready=0)
    st = await wait_status(pa, j3["clientKey"], "Queued")
    acc = json.loads(st["partyMembers"][0]["accountData"]) if st else {}
    check(acc.get("e") == 16 and st.get("queueIds") == ["ranked"], "rating visible in accountData.e (%s)" % acc)
    http(pa, "/mm/queue/leave", clientKey=j3["clientKey"])

    print("== abandon -> deduction + cooldown")
    keys, states = await full_match(pa, pb, accs, node, party_of_two=False)
    cfg = node.hosted[-1][1]
    for port, k in keys:
        http(port, "/mm/queue/leave", clientKey=k["clientKey"])
    r = await node.request({"type": "ranked-result", "matchId": cfg["matchId"], "winner": -1, "players": [],
                            "abandoned": [alice["id"]]})
    ra = [x for x in (r or {}).get("ratings", []) if x["name"] == alice["name"]]
    check(ra and ra[0].get("abandoned") and ra[0].get("delta") == -16 and ra[0].get("new") == 0,
          "abandoner: -rankedDeduction, floored at 0 (%s)" % ra)
    jb2 = join(pb, alice)
    check(jb2["status"] == "RankedCooldown" and jb2.get("cooldownMs", 0) > 7000000,
          "abandoner gets RankedCooldown on the other brain (%s)" % jb2)
    code, rb = rating(pb, alice)
    check(code == 200 and rb.get("abandons") == 1 and rb.get("cooldownMs", 0) > 7000000, "rating card: abandons + cooldown (%s)" % rb)

    print("== parties")
    carol, dave = accs[2], accs[3]
    jc = join(pa, carol, ready=0)
    st = await wait_status(pa, jc["clientKey"], "Queued")
    pid = st["partyId"]
    jd = join(pb, dave, ready=0, party=pid, cls=7)
    check(jd["status"] is None, "join a party by id on the other brain")
    code, st = http(pa, "/mm/queue/poll", clientKey=jc["clientKey"])
    leader = st["partyLeaderId"]
    members = st["partyMembers"]
    check(len(members) == 2 and leader == members[0]["clientId"] and st["queueIds"] == ["ranked"],
          "party has 2 members, creator leads")
    check(members[1]["classIndex"] == 0, "a class outside config.rankedClasses (Rocketeer) -> Triggerman")
    dave_id = members[1]["clientId"]
    code, st = http(pb, "/mm/queue/update-ready", clientKey=jd["clientKey"], isReady="true")
    check(st["partyMembers"][1]["isReady"] is True, "update-ready")
    code, st = http(pa, "/mm/queue/update-queues", clientKey=jc["clientKey"], queues='["r4v4"]')
    check(code == 404, "no update-queues: one queue")
    code, st = http(pa, "/mm/queue/update-class", clientKey=jc["clientKey"], classIndex=3)
    check(st["partyMembers"][0]["classIndex"] == 3, "update-class")
    code, st = http(pa, "/mm/queue/promote-party-member", clientKey=jc["clientKey"], promoteClientId=dave_id)
    check(st["partyLeaderId"] == dave_id, "promote")
    code, st = http(pb, "/mm/queue/kick-party-member", clientKey=jd["clientKey"],
                    kickClientId=members[0]["clientId"])
    check(len(st["partyMembers"]) == 1, "kick")
    code, st = http(pa, "/mm/queue/poll", clientKey=jc["clientKey"])
    check(code == 404 and st.get("error") == "InvalidClientKey", "kicked key -> 404 InvalidClientKey")
    code, st = http(pb, "/mm/queue/leave", clientKey=jd["clientKey"])
    check(st == {"left": True}, "leave")

    print("== removed queues, gates")
    jq = join(pa, carol, queues=["r1v1"])
    check(jq["status"] == "InvalidQueue", "v3.1.1 queue ids are gone -> InvalidQueue")
    jl = join(pc, carol)
    check(jl["status"] == "LevelTooLow", "minRankedLevel 15 -> LevelTooLow")
    code, st = http(pa, "/mm/queue/join", region="us-nj")
    check(st["status"] == "LoginRequired", "join without a session -> LoginRequired")
    code, st = http(pa, "/mm/queue/rating")
    check(code == 401, "rating without a session -> 401")

    print("== host failure puts the parties back")
    node.fail_hosts = 1
    others = accs[1:] + [spare]  # alice is on her cooldown
    keys = []
    for i, acc in enumerate(others):
        port = pa if i % 2 else pb
        keys.append((port, join(port, acc)))
    await asyncio.sleep(2.5)
    code, st = http(keys[0][0], "/mm/queue/poll", clientKey=keys[0][1]["clientKey"])
    check(st["status"]["t"] == "Queued", "after a failed host the parties are Queued again")
    st = await wait_status(keys[0][0], keys[0][1]["clientKey"], "Join", secs=20)
    check(st and st["status"]["t"] == "Join", "retried after the back-off and joined")
    for port, k in keys:
        http(port, "/mm/queue/leave", clientKey=k["clientKey"])


def main():
    driver = os.environ.get("KN_TEST_DB_DRIVER", "postgres")
    if driver == "postgres":
        pg = [sys.executable, "-I", os.path.join(ROOT, "services", "brain", "tests", "pg_schema.py"), ROOT, DB]
        rc = subprocess.run(pg + ["create", SCHEMA]).returncode
        if rc:
            sys.exit(rc)
        dbenv = {"KN_DATABASE_NAME": DB, "KN_DATABASE_SEARCH_PATH": SCHEMA}
    else:
        pg = [sys.executable, "-I", os.path.join(ROOT, "services", "brain", "tests", "db_scratch.py"), ROOT]
        r = subprocess.run(pg + ["create", driver, SCHEMA], capture_output=True)
        if r.returncode:
            print("ranked test: %s unavailable: skipped" % driver)
            sys.exit(r.returncode)
        dbenv = {"KN_DATABASE_" + k.upper(): str(v) for k, v in json.loads(r.stdout).items()}
        pg_drop = pg + ["drop", driver, SCHEMA]
    work = tempfile.mkdtemp(prefix="kn-ranked.")
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=SECRET, **dbenv)
    subprocess.run([BRAIN, "--migrate-only"], cwd=ROOT, env=env, check=True, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL)
    procs, logs, ports = [], [], []
    # brain C: the default level gate (minRankedLevel 15) and season 8
    for name, region, extra in (("rank-a-%d" % os.getpid(), "us-nj", ["--brain.ranked.min_level=1", "--accounts.current_season=7"]),
                                ("rank-b-%d" % os.getpid(), "de-fra", ["--brain.ranked.min_level=1", "--accounts.current_season=7"]),
                                ("rank-c-%d" % os.getpid(), "de-fra", ["--accounts.current_season=8"])):
        path = os.path.join(work, name + ".log")
        log = open(path, "w")
        logs.append(path)
        procs.append(subprocess.Popen([BRAIN, "--brain.host=127.0.0.1", "--brain.port=0", "--brain.id=" + name,
                                       "--brain.region=" + region] + extra, cwd=ROOT, env=env,
                                      stdout=log, stderr=subprocess.STDOUT))
    try:
        pa, pb, pc = (bound_port(logs[i], procs[i]) for i in range(3))

        async def run():
            node = FakeNode(pa)
            await node.start()
            await asyncio.sleep(1.5)
            await scenario(pa, pb, pc, node)
            node.task.cancel()
            await node.ws.close()
        asyncio.run(run())
    finally:
        for p in procs:
            p.terminate()
        for p in procs:
            p.wait(5)
        if driver == "postgres":
            subprocess.run(pg + ["drop", SCHEMA])
        else:
            subprocess.run(pg_drop)
    print("== brain logs")
    bad = []
    for path in logs:
        with open(path) as f:
            bad += [l.strip() for l in f if re.search(r"failed:|ERROR|FATAL", l)]
    check(not bad, "no statement errors in the brain logs %s" % bad[:3])
    print("ranked test: " + ("OK" if not FAILS else "%d failure(s)" % len(FAILS)))
    if FAILS:
        print("logs kept in " + work)
    else:
        shutil.rmtree(work, ignore_errors=True)
    sys.exit(1 if FAILS else 0)


main()
