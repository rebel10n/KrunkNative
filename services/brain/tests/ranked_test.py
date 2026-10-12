#!/usr/bin/env python3
"""Ranked queue end to end (doc/backend.md 6.5): 2 brains sharing one database,
a fake game server on brain A, 4 accounts registered over the social socket.

  ranked_test.py <kn_brain> <repo root> [database]
Database: argument, else $KN_MM_TEST_DATABASE, else config/config.json's
database.name; every run uses its own schema (kn_test_rq_<pid>, dropped after)
and ephemeral ports, so concurrent runs never interfere. 77 = skip (no PostgreSQL).
KN_TEST_DB_DRIVER=sqlite|mysql runs both brains on that store driver instead
(db_scratch.py: one temp sqlite file, or a scratch MySQL/MariaDB database).

Covers /mm/queue/join on both brains, cross-brain matching + hosting (the node
lives on brain A), Join tickets, the ranked "host" config, AccountAlreadyPlaying,
ranked-result -> Elo through svc-accounts' "elo" op, abandon -> RankedCooldown,
parties (join by id, ready, queues, kick, promote, leave), InvalidClientKey, and
that the brain logs carry no statement errors.
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


def join(port, acc, queues, ready=1, party=""):
    return http(port, "/mm/queue/join", token=acc["token"], region="us-nj", queues=json.dumps(queues),
                party=party, ready=ready, classIndex=1)[1]


async def wait_status(port, key, want, secs=15):
    st = None
    for _ in range(secs * 5):
        code, st = http(port, "/mm/queue/poll", clientKey=key)
        if code == 200 and st["status"]["t"] == want:
            return st
        await asyncio.sleep(0.2)
    return st


async def scenario(pa, pb, node):
    stamp = str(int(time.time()) % 100000)
    alice, bob, carol, dave = [await register(pa, n + stamp) for n in ("ral", "rbo", "rca", "rda")]

    print("== 1v1 across two brains")
    ja, jb = join(pa, alice, ["r1v1"]), join(pb, bob, ["r1v1"])
    check(ja["status"] is None and ja["clientKey"] and jb["status"] is None, "join on brain A and brain B")
    st = await wait_status(pa, ja["clientKey"], "Join")
    stb = await wait_status(pb, jb["clientKey"], "Join")
    ok = st and stb and st["status"]["t"] == "Join" and st["status"]["c"]["gameId"] == stb["status"]["c"]["gameId"]
    check(ok, "both reach Join in the same game (%s)" % (st and st["status"]["c"].get("gameId")))
    c = st["status"]["c"] if st else {}
    check(c.get("gameId", "").startswith("RKD:") and c.get("port") == 23999 and len(c.get("clientId", "")) > 100,
          "Join carries gameId/host/port and a per-player ticket")
    cfg = node.hosted[-1][1] if node.hosted else {}
    teams = cfg.get("teams", [])
    names = sorted(p["name"] for t in teams for p in t)
    check(cfg.get("ranked") and cfg.get("queue") == "r1v1" and cfg.get("maxPlayers") == 2 and cfg.get("mode") == 38
          and names == sorted([alice["name"], bob["name"]]), "ranked host config (queue, teams, mode 38 = KN Ranked on the v7.1.4 table)")
    again = join(pa, alice, ["r1v1"])
    check(again["status"] == "AccountAlreadyPlaying", "re-queue during the match -> AccountAlreadyPlaying")
    http(pa, "/mm/queue/leave", clientKey=ja["clientKey"])
    http(pb, "/mm/queue/leave", clientKey=jb["clientKey"])
    team_a = 0 if teams[0][0]["name"] == alice["name"] else 1
    r = await node.request({"type": "ranked-result", "matchId": cfg["matchId"], "lobbyId": node.hosted[-1][0],
                            "winner": team_a, "players": [], "abandoned": []})
    rat = {x["name"]: x for x in (r or {}).get("ratings", [])}
    check(r and r.get("ok") and rat.get(alice["name"], {}).get("delta") == 16 and rat[alice["name"]]["new"] == 16
          and rat.get(bob["name"], {}).get("new") == 0, "Elo: winner +16, loser clamped at 0 (%s)" % rat)
    r2 = await node.request({"type": "ranked-result", "matchId": cfg["matchId"], "winner": 1 - team_a})
    check(r2 and r2.get("ok") and r2.get("ratings") == [], "a second result for the same match is ignored")
    j3 = join(pa, alice, ["r1v1"], ready=0)
    st = await wait_status(pa, j3["clientKey"], "Queued")
    acc = json.loads(st["partyMembers"][0]["accountData"]) if st else {}
    check(acc.get("e") == 16, "rating visible in accountData.e (%s)" % acc)
    http(pa, "/mm/queue/leave", clientKey=j3["clientKey"])

    print("== abandon -> cooldown")
    ja, jb = join(pa, alice, ["r1v1"]), join(pa, bob, ["r1v1"])
    st = await wait_status(pa, ja["clientKey"], "Join")
    cfg = node.hosted[-1][1]
    for k in (ja, jb):
        http(pa, "/mm/queue/leave", clientKey=k["clientKey"])
    r = await node.request({"type": "ranked-result", "matchId": cfg["matchId"], "winner": -1, "players": [],
                            "abandoned": [bob["id"]]})
    rb = [x for x in (r or {}).get("ratings", []) if x["name"] == bob["name"]]
    check(rb and rb[0].get("abandoned"), "abandoner flagged")
    jb2 = join(pb, bob, ["r1v1"])
    check(jb2["status"] == "RankedCooldown" and jb2.get("cooldownMs", 0) > 7000000,
          "abandoner gets RankedCooldown on the other brain (%s)" % jb2)

    print("== parties")
    jc = join(pa, carol, ["r2v2"], ready=0)
    st = await wait_status(pa, jc["clientKey"], "Queued")
    pid = st["partyId"]
    jd = join(pb, dave, ["r2v2"], ready=0, party=pid)
    check(jd["status"] is None, "join a party by id on the other brain")
    code, st = http(pa, "/mm/queue/poll", clientKey=jc["clientKey"])
    leader = st["partyLeaderId"]
    members = st["partyMembers"]
    check(len(members) == 2 and leader == members[0]["clientId"] and st["queueIds"] == ["r2v2"],
          "party has 2 members, creator leads")
    dave_id = members[1]["clientId"]
    code, st = http(pb, "/mm/queue/update-ready", clientKey=jd["clientKey"], isReady="true")
    check(st["partyMembers"][1]["isReady"] is True, "update-ready")
    code, st = http(pb, "/mm/queue/update-queues", clientKey=jd["clientKey"], queues='["r4v4"]')
    check(st["queueIds"] == ["r2v2"], "non-leader cannot change queues")
    code, st = http(pa, "/mm/queue/update-queues", clientKey=jc["clientKey"], queues='["r4v4","bogus"]')
    check(st["queueIds"] == ["r4v4"], "leader changes queues (invalid ids dropped)")
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

    print("== host failure puts the parties back")
    node.fail_hosts = 1
    ja, jc = join(pa, alice, ["r1v1"]), join(pa, carol, ["r1v1"])
    await asyncio.sleep(2.5)
    code, st = http(pa, "/mm/queue/poll", clientKey=ja["clientKey"])
    check(st["status"]["t"] == "Queued", "after a failed host the party is Queued again")
    st = await wait_status(pa, ja["clientKey"], "Join", secs=20)
    check(st and st["status"]["t"] == "Join", "retried after the back-off and joined")
    code, st = http(pa, "/mm/queue/join", region="us-nj", queues="[]")
    check(st["status"] == "LoginRequired", "join without a session -> LoginRequired")


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
    for name, region in (("rank-a-%d" % os.getpid(), "us-nj"), ("rank-b-%d" % os.getpid(), "de-fra")):
        path = os.path.join(work, name + ".log")
        log = open(path, "w")
        logs.append(path)
        procs.append(subprocess.Popen([BRAIN, "--brain.host=127.0.0.1", "--brain.port=0", "--brain.id=" + name,
                                       "--brain.region=" + region, "--brain.ranked.min_level=1"], cwd=ROOT, env=env,
                                      stdout=log, stderr=subprocess.STDOUT))
    try:
        pa, pb = bound_port(logs[0], procs[0]), bound_port(logs[1], procs[1])

        async def run():
            node = FakeNode(pa)
            await node.start()
            await asyncio.sleep(1.5)
            await scenario(pa, pb, node)
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
