#!/usr/bin/env python3
"""Krunker v10.0.2 ranked end to end (doc/v10/ranked-impl.md): 2 brains sharing one database (plus a
third with the default level gate), a fake game server on brain A, 9 accounts registered over the
social socket.

  ranked_test.py <kn_brain> <repo root> [database]
Database: argument, else $KN_MM_TEST_DATABASE, else config/config.json's database.name; every run
uses its own schema (kn_test_rq_<pid>, dropped after) and ephemeral ports. 77 = skip (no
PostgreSQL). KN_TEST_DB_DRIVER=sqlite|mysql runs the brains on that store driver instead.

Covers: the live-database upgrade (migrate, add v7 ranked_ratings rows as a v7 brain would, run the
0205 seed: v7 Elo -> the Beta season profile), /mm/ranked/info (calendar season, the 5-map pool),
the solo queue on both brains (QUEUE_STATUS QUEUED -> MATCHED with a v10 assignment
{connection: host:port, gameId, clientId}), the long poll, cross-brain matching + hosting (a 4v4
Hardpoint comp lobby on a season map, teams balanced), ALREADY_IN_MATCH + /mm/ranked/connection
(rejoin), ranked-result -> TrueSkill profiles (season, region), match history with mmr_change,
placement (Unranked for 6 matches, then a rank), the leaderboard (placed players only), Ranked
Points per match + a milestone claim (once), abandon -> COOLDOWN, the level gate
(MIN_LEVEL_REQUIREMENT), LOGIN_REQUIRED, leave, the season close (RP conversion once), the RP shop
purchase, the old /mm/queue routes (ClientOutdated), and that the brain logs carry no statement
errors.
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
    print("ranked test: no database configured; skipped")
    sys.exit(77)
# a statement error ("<what> failed: <pg error>") or a level-tagged ERROR/FATAL line; a WARN that
# quotes a database error text (the v7-era database has no strike catalogue) is not one
LOG_ERROR_RE = r"failed:|\s(?:ERROR|FATAL)\s+\["
SCHEMA = "kn_test_rq_%d" % os.getpid()
SECRET = "kn-ranked-test-%d" % os.getpid()
FAILS = []
SEASON = 7  # brain.ranked.season (the calendar would make results depend on the month)
SEASON_MAPS = {7: None}


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


def http(port, path, method="GET", body=None, **q):
    url = "http://127.0.0.1:%d%s" % (port, path)
    if q:
        url += "?" + urllib.parse.urlencode(q)
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(url, data=data, method=method, headers={"Content-Type": "application/json"} if data else {})
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
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
        self.hosted = []
        self.replies = {}
        self.rid = 100
        self.fail_hosts = 0
        self.finished = []
        self.count = 0

    async def start(self):
        self.ws = await websockets.connect(self.url, additional_headers={"Authorization": "Bearer " + SECRET})
        await self.ws.send(json.dumps({"type": "hello", "nodeId": "RKD", "region": "us-nj", "publicHost": "127.0.0.1",
                                       "publicPort": 23999, "version": "test", "custom": True, "maxLobbies": 8}))
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
                lid = "rk%03d" % self.count
                self.count += 1
                self.hosted.append((lid, f.get("config")))
                await self.ws.send(json.dumps({"type": "host-reply", "rid": f["rid"], "ok": True, "lobbyId": lid}))
                await self.ws.send(json.dumps({"type": "snapshot", "lobbies": self.tuples()}))
            elif t and t.endswith("-reply") and "rid" in f:
                self.replies[f["rid"]] = f

    async def done(self, lobby):
        """the match is over: the lobby goes back to the public rotation (lobby-down)"""
        self.hosted = [(l, c) for l, c in self.hosted if l != lobby]
        self.finished.append(lobby)
        await self.ws.send(json.dumps({"type": "snapshot", "lobbies": self.tuples()}))

    async def request(self, frame):
        if frame.get("type") == "ranked-result" and frame.get("lobbyId") in [l for l, _ in self.hosted]:
            reply = await self._request(frame)
            await self.done(frame["lobbyId"])
            return reply
        return await self._request(frame)

    async def _request(self, frame):
        self.rid += 1
        frame["rid"] = self.rid
        await self.ws.send(json.dumps(frame))
        for _ in range(150):
            if self.rid in self.replies:
                return self.replies.pop(self.rid)
            await asyncio.sleep(0.1)
        return None


async def register(port, name):
    async with Social("ws://127.0.0.1:%d/social/ws" % port) as s:
        r = await s.call("a", "a", 0, [name, "rankedpass1", 0], "captcha")
        assert r[1] is None, r
        return {"id": r[2], "name": name, "token": r[5]}


def queue(port, acc, regions="na"):
    return http(port, "/mm/ranked/queue", token=acc["token"], regions=regions)[1]


async def wait_matched(port, key, secs=40):
    st = None
    end = time.time() + secs
    while time.time() < end:
        code, st = await asyncio.get_event_loop().run_in_executor(None, lambda: http(port, "/mm/ranked/poll", clientKey=key, wait=3))
        if code == 200 and st.get("payload", {}).get("status") == "MATCHED":
            return st
        if code != 200:
            return st
    return st


async def full_match(pa, pb, accs, node):
    keys = []
    for i, acc in enumerate(accs):
        port = pa if i % 2 else pb
        r = queue(port, acc, "na" if i % 3 else "eu,na")
        keys.append((port, r.get("payload", {}).get("clientKey")))
    states = await asyncio.gather(*[wait_matched(port, k) for port, k in keys])
    return keys, states


def profile(port, acc, season=SEASON, region="na"):
    return http(port, "/mm/ranked/profile", token=acc["token"], season=season, region=region)


def result_frame(cfg, lobby, winner, abandoned=(), reward=40):
    players = []
    for t, team in enumerate(cfg["teams"]):
        for p in team:
            # 40 hits of 100 rays (accuracy 40): the profile's total_accuracy
            players.append({"accountId": p["accountId"], "team": t, "score": 1000, "kills": 10 + t, "deaths": 8,
                            "assists": 2, "damage": 1500, "objective": 30, "timeMs": 300000, "reward": reward,
                            "shots": 100, "hits": 40})
    return {"type": "ranked-result", "matchId": cfg["matchId"], "lobbyId": lobby, "winner": winner, "players": players,
            "abandoned": list(abandoned)}


async def scenario(pa, pb, pc, node, v7_seeded):
    stamp = str(int(time.time()) % 100000)
    accs = v7_seeded["accs"] + [await register(pa, "r%d%s" % (i, stamp)) for i in range(len(v7_seeded["accs"]), 8)]
    spare = await register(pa, "r8%s" % stamp)
    alice = accs[0]

    print("== info + the v7 seed (0205)")
    upgraded = bool(v7_seeded["accs"])
    code, info = http(pa, "/mm/ranked/info")
    check(code == 200 and info.get("season") == SEASON and len(info.get("maps", [])) == 5 and
          any(m["id"] == 2 for m in info.get("maps", [])), "info: season %d, Sandstorm + 4 maps (%s)" % (SEASON, [m["id"] for m in info.get("maps", [])]))
    check([r["code"] for r in info.get("regions", [])] == ["as", "eu", "na"] and info.get("minLevel") == 1, "info: regions as/eu/na, min level")
    # Q-MAP: KN plays the pool's maps 0..31 only; Eterno Simulator (39) and Frontier (42) are in the
    # rotation but not playable (catalogue entries now, not hosted), so the list says so and the matcher never hosts them
    unplayable = sorted(m["id"] for m in info.get("maps", []) if not m["available"])
    check(all(m["available"] == (m["id"] < 32) for m in info.get("maps", [])) and all(i in (39, 42) for i in unplayable),
          "maps: the playable ones are listed available, 39 / 42 unavailable (%s)" % unplayable)
    code, p0 = profile(pa, alice, season=0, region="na")
    pr = p0.get("profile", {}) if code == 200 else {}
    check(not upgraded or code == 200 and pr.get("wins") == 3 and pr.get("losses") == 1 and abs(pr.get("conservative_rating", 0) - (25 + 300 / 60.0 - 50 / 3.0)) < 1e-6,
          "a v7 rating 300 (3-1) is the Beta season profile: mu 30, sigma 2/3 sigma0 (%s)" % pr)
    check(p0.get("milestones") == [], "no milestones during the Beta season")

    # Q2 (v10 defaults to eu) and Q17 (the min level comes from the profile response)
    code, dflt = http(pa, "/mm/ranked/profile", token=alice["token"], season=SEASON)
    check(code == 200 and dflt.get("region") == "eu" and dflt.get("minLevel") == 1,
          "profile without a region: eu, min level from the brain (%s, %s)" % (dflt.get("region"), dflt.get("minLevel")))
    print("== a 4v4 across two brains")
    code, r0 = profile(pa, alice)
    check(code == 200 and r0["profile"]["rank"] == 0 and r0["profile"]["placement_left"] == 6 and r0.get("rankedPoints") == 0,
          "season %d profile before any match: Unranked, 6 placements" % SEASON)
    keys, states = await full_match(pa, pb, accs, node)
    games = {st.get("payload", {}).get("assignment", {}).get("gameId") for st in states}
    check(len(games) == 1 and all(st.get("payload", {}).get("status") == "MATCHED" for st in states),
          "all 8 MATCHED in the same game (%s)" % games)
    if len(games) != 1 or None in games:
        print("    states: %s" % [st.get("payload") for st in states][:3])
    a0 = states[0]["payload"]["assignment"] if states[0] else {}
    check(a0.get("connection") == "127.0.0.1:23999" and a0.get("gameId", "").startswith("RKD:") and len(a0.get("clientId", "")) > 100
          and a0.get("region") == "na", "assignment {connection host:port, gameId, per-player ticket, region na}")
    lobby, cfg = node.hosted[-1] if node.hosted else ("", {})
    teams = cfg.get("teams", [])
    roster = sorted(p["name"] for t in teams for p in t)
    check(cfg.get("ranked") and cfg.get("ruleset") == "v10" and cfg.get("teamSize") == 4 and cfg.get("mode") == 2 and cfg.get("modes") == [2]
          and len(cfg.get("maps", [])) == 1 and cfg["maps"][0] in [m["id"] for m in info["maps"] if m["available"]]
          and roster == sorted(a["name"] for a in accs) and [len(t) for t in teams] == [4, 4],
          "host config: Hardpoint on a playable season map, 4 + 4, ruleset v10 (%s)" % {k: cfg.get(k) for k in ("maps", "season", "ruleset")})
    seeded_team = [i for i, t in enumerate(teams) if alice["name"] in [p["name"] for p in t]]
    again = queue(pa, alice)
    check(again.get("type") == "ERROR" and again["payload"]["code"] == "ALREADY_IN_MATCH" and again["payload"]["payload"].get("serverUrl") == "127.0.0.1:23999",
          "re-queue during the match -> ALREADY_IN_MATCH with the serverUrl")
    code, conn = http(pb, "/mm/ranked/connection", token=alice["token"])
    check(code == 200 and conn.get("serverUrl") == "127.0.0.1:23999" and conn["assignment"]["clientId"], "/mm/ranked/connection on the other brain (rejoin)")
    win = seeded_team[0]
    r = await node.request(result_frame(cfg, lobby, win))
    rat = {x["name"]: x for x in (r or {}).get("ratings", [])}
    winners = [p["name"] for p in teams[win]]
    losers = [p["name"] for p in teams[1 - win]]
    check(r and r.get("ok") and len(rat) == 8 and all(rat[n]["delta"] > 0 for n in winners) and all(rat[n]["delta"] < 0 for n in losers)
          and all(rat[n]["rp"] == 20 for n in winners + losers) and rat[alice["name"]]["season"] == SEASON and rat[alice["name"]]["region"] == "na",
          "TrueSkill: winners up, losers down, 20 RP each (reward 40 / 2)")
    r2 = await node.request(result_frame(cfg, lobby, 1 - win))
    check(r2 and r2.get("ok") and r2.get("ratings") == [], "a second result for the same match is ignored")
    code, conn = http(pa, "/mm/ranked/connection", token=alice["token"])
    check(code == 200 and conn.get("serverUrl") is None, "no connection once the match is over")
    code, pa1 = profile(pb, alice)
    pr = pa1.get("profile", {})
    check(code == 200 and pr.get("total_accuracy") == 40.0 and pr.get("wins") == 1 and pr.get("total_matches") == 1 and pr.get("total_kills") in (10, 11) and pr.get("rank") == 0
          and pr.get("placement_left") == 5 and pa1.get("rankedPoints") == 20, "profile on the other brain: 1 win, still placing, 20 RP (%s)" % pr)
    code, h = http(pa, "/mm/ranked/history", token=alice["token"], season=SEASON, region="na")
    m = (h.get("matchHistory") or {}).get("matches", [{}])[0] if code == 200 else {}
    check(code == 200 and m.get("mmr_change", 0) > 0 and m.get("victory") == 1 and m.get("map") == cfg["maps"][0] and m.get("map_name"),
          "match history: the match with its mmr_change (%s)" % {k: m.get(k) for k in ("mmr_change", "victory", "map_name")})
    code, other_region = profile(pa, alice, region="eu")
    check(code == 200 and other_region["profile"]["total_matches"] == 0, "the eu profile is separate")

    print("== placement, leaderboard, milestone")
    for _ in range(5):  # five more: 6 matches, placed
        keys, states = await full_match(pa, pb, accs, node)
        lobby, cfg = node.hosted[-1]
        await node.request(result_frame(cfg, lobby, 0))
    code, p6 = profile(pa, alice)
    pr = p6.get("profile", {})
    check(code == 200 and pr.get("total_matches") == 6 and pr.get("rank", 0) >= 1 and pr.get("placement_left") == 0, "placed after 6 matches (rank %s)" % pr.get("rank"))
    code, lb = http(pc, "/mm/ranked/leaderboard", season=SEASON, region="na", limit=5, offset=0)
    board = lb.get("leaderboard", []) if code == 200 else []
    check(code == 200 and len(board) == 5 and lb.get("total") == 8 and lb.get("totalPages") == 2 and board[0]["position"] == 1
          and board[0]["profile"]["conservative_rating"] >= board[-1]["profile"]["conservative_rating"], "leaderboard: 8 placed, by rating, paged")
    code, cl = http(pa, "/mm/ranked/claim-milestone", method="POST", token=alice["token"], region="na")
    check(code == 400 and "not reached" in cl.get("error", ""), "milestone 10 not reached at 6 matches")
    for k in range(4):
        keys, states = await full_match(pa, pb, accs, node)
        lobby, cfg = node.hosted[-1]
        draw = k == 3  # v10: a draw is Alpha's victory in the match history (team 0), not Bravo's
        await node.request(result_frame(cfg, lobby, -1 if draw else 1))
        if draw:
            by_id = {a["id"]: a for a in accs}
            for t in (0, 1):
                member = by_id[cfg["teams"][t][0]["accountId"]]
                code, h = http(pa, "/mm/ranked/history", token=member["token"], season=SEASON, region="na")
                m = (h.get("matchHistory") or {}).get("matches", [{}])[0] if code == 200 else {}
                check(code == 200 and m.get("victory") == (1 if t == 0 else 0),
                      "a draw in history: team %d victory %s (%s)" % (t + 1, m.get("victory"), m.get("mmr_change")))
    code, cl = http(pa, "/mm/ranked/claim-milestone", method="POST", token=alice["token"], region="na")
    check(code == 200 and cl.get("success") and cl.get("reward") == 100 and cl.get("balance") == 10 * 20 + 100, "milestone 10 claimed: +100 RP (%s)" % cl)
    code, cl = http(pa, "/mm/ranked/claim-milestone", method="POST", token=alice["token"], region="na")
    check(code == 400, "the same milestone only once")

    print("== abandon -> cooldown")
    keys, states = await full_match(pa, pb, accs, node)
    lobby, cfg = node.hosted[-1]
    r = await node.request(result_frame(cfg, lobby, -1, abandoned=[alice["id"]]))
    ra = [x for x in (r or {}).get("ratings", []) if x["name"] == alice["name"]]
    check(ra and ra[0].get("abandoned") and ra[0]["delta"] < 0 and ra[0]["rp"] == 0, "abandoner: rating down, no RP (%s)" % ra)
    jb = queue(pb, alice)
    check(jb.get("type") == "ERROR" and jb["payload"]["code"] == "COOLDOWN" and jb["payload"]["payload"]["cooldown"] > 1700000,
          "COOLDOWN on the other brain (%s)" % jb.get("payload", {}).get("payload"))

    print("== gates, leave")
    gl = queue(pc, accs[2])
    check(gl.get("payload", {}).get("code") == "MIN_LEVEL_REQUIREMENT", "level 10 gate -> MIN_LEVEL_REQUIREMENT")
    if DRIVER == "postgres":  # Q16: a match still creating (no host yet) is seen by the join check and the rejoin lookup alike
        psql("INSERT INTO mm_ranked_matches (match_id, queue_id, region, brain_id, status) VALUES ('rk-creating-test', 'ranked', 'na', 'test', 'creating')")
        psql("INSERT INTO mm_ranked_match_players (match_id, account_id, username, team, rating_key, rating_before)"
             " VALUES ('rk-creating-test', %d, 'creating', 0, 'elo', 1000)" % accs[3]["id"])
        jr = queue(pa, accs[3])
        code, cn = http(pa, "/mm/ranked/connection", token=accs[3]["token"])
        check(jr.get("payload", {}).get("code") == "ALREADY_IN_MATCH" and not jr["payload"]["payload"].get("serverUrl") and code == 200 and cn.get("serverUrl") is None,
              "a creating match: ALREADY_IN_MATCH and /mm/ranked/connection agree (no host yet)")
        psql("DELETE FROM mm_ranked_matches WHERE match_id = 'rk-creating-test'")
    code, st = http(pa, "/mm/ranked/queue", regions="na")
    check(st.get("payload", {}).get("code") == "LOGIN_REQUIRED", "no session -> LOGIN_REQUIRED")
    js = queue(pa, spare, "as")
    key = js["payload"]["clientKey"]
    code, st = http(pa, "/mm/ranked/poll", clientKey=key)
    check(code == 200 and st["payload"]["status"] == "QUEUED" and st["payload"]["queueSize"] >= 1, "queued alone: QUEUED, queueSize")
    code, st = http(pb, "/mm/ranked/leave", clientKey=key)
    check(code == 200 and st.get("left") is True, "leave")
    code, st = http(pa, "/mm/ranked/poll", clientKey=key)
    check(code == 404, "a left key -> 404")
    code, st = http(pa, "/mm/queue/join", token=alice["token"])
    check(code == 410 and st.get("status") == "ClientOutdated", "the v7 /mm/queue routes answer ClientOutdated")

    print("== host failure puts the players back")
    node.fail_hosts = 1
    others = accs[1:] + [spare]
    keys = []
    for i, acc in enumerate(others):
        port = pa if i % 2 else pb
        keys.append((port, queue(port, acc)["payload"]["clientKey"]))
    await asyncio.sleep(2.5)
    code, st = http(keys[0][0], "/mm/ranked/poll", clientKey=keys[0][1])
    check(st["payload"]["status"] == "QUEUED", "after a failed host the players are QUEUED again")
    st = await wait_matched(keys[0][0], keys[0][1], secs=25)
    check(st and st["payload"]["status"] == "MATCHED", "retried after the 10 s hold and matched")
    lobby, cfg = node.hosted[-1]
    await node.request(result_frame(cfg, lobby, 0))
    return accs


def psql(sql):
    db = {}
    try:
        with open(os.path.join(ROOT, "config", "config.json")) as fp:
            db = json.load(fp).get("database", {})
    except (OSError, ValueError):
        pass
    get = lambda k, d: os.environ.get("KN_DATABASE_" + k.upper(), str(db.get(k, d)))
    env = dict(os.environ, PGPASSWORD=get("password", ""), PGOPTIONS="-c search_path=" + SCHEMA)
    r = subprocess.run(["psql", "-h", get("host", "127.0.0.1"), "-p", get("port", 5432), "-U", get("user", "krunknative"), "-d", DB,
                        "-v", "ON_ERROR_STOP=1", "-qAt", "-c", sql], env=env, capture_output=True)
    if r.returncode:
        raise RuntimeError(r.stderr.decode())
    return r.stdout.decode().strip()


def start_brains(work, env, specs):
    procs, logs = [], []
    for name, region, extra in specs:
        path = os.path.join(work, name + ".log")
        log = open(path, "w")
        logs.append(path)
        procs.append(subprocess.Popen([BRAIN, "--brain.host=127.0.0.1", "--brain.port=0", "--brain.id=" + name,
                                       "--brain.region=" + region] + extra, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT))
    return procs, logs


def main():
    if DRIVER == "postgres":
        pg = [sys.executable, "-I", os.path.join(ROOT, "services", "brain", "tests", "pg_schema.py"), ROOT, DB]
        rc = subprocess.run(pg + ["create", SCHEMA]).returncode
        if rc:
            sys.exit(rc)
        dbenv = {"KN_DATABASE_NAME": DB, "KN_DATABASE_SEARCH_PATH": SCHEMA}
    else:
        pg = [sys.executable, "-I", os.path.join(ROOT, "services", "brain", "tests", "db_scratch.py"), ROOT]
        r = subprocess.run(pg + ["create", DRIVER, SCHEMA], capture_output=True)
        if r.returncode:
            print("ranked test: %s unavailable: skipped" % DRIVER)
            sys.exit(r.returncode)
        dbenv = {"KN_DATABASE_" + k.upper(): str(v) for k, v in json.loads(r.stdout).items()}
        pg_drop = pg + ["drop", DRIVER, SCHEMA]
    work = tempfile.mkdtemp(prefix="kn-ranked.")
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=SECRET, **dbenv)
    seeded = {"accs": []}
    # window 400 + 100 / s waited (at most 2000): the seeded v7 player (MMR 2333) matches the new ones
    # (MMR 1000) after ~10 s; brain C keeps the defaults (150 + 10 / s, at most 1000)
    common = ["--brain.ranked.season=%d" % SEASON, "--brain.ranked.poll_timeout_s=60"]
    wide = ["--brain.ranked.rating_window=400", "--brain.ranked.window_growth=100", "--brain.ranked.window_max=2000"]
    procs, logs = [], []
    try:
        if DRIVER == "postgres":
            # the live upgrade: a v7 database (migrations up to 0204) with ranked history, then 0205
            old = os.path.join(work, "migrations-v7")
            src = os.path.join(ROOT, "services", "db", "migrations")
            for dialect in os.listdir(src):
                os.makedirs(os.path.join(old, dialect))
                for f in os.listdir(os.path.join(src, dialect)):
                    if f[:4].isdigit() and int(f[:4]) <= 204:
                        shutil.copy(os.path.join(src, dialect, f), os.path.join(old, dialect, f))
            subprocess.run([BRAIN, "--migrate-only", "--database.migrations_dir=" + old], cwd=ROOT, env=env, check=True,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            procs, logs = start_brains(work, env, [("rank-v7-%d" % os.getpid(), "us-nj", ["--brain.ranked.enabled=false"])])
            port = bound_port(logs[0], procs[0])
            stamp = str(int(time.time()) % 100000)
            seeded["accs"] = [asyncio.run(register(port, "v7r%s" % stamp))]
            procs[0].terminate()
            procs[0].wait(5)
            acc = seeded["accs"][0]["id"]
            psql("INSERT INTO ranked_ratings (account_id, season, rating, peak, games, wins, losses) VALUES (%d, 6, 120, 150, 2, 1, 1),"
                 " (%d, 7, 300, 310, 2, 2, 0)" % (acc, acc))
            with open(logs[0]) as f:
                v7log = f.read()
            if re.search(LOG_ERROR_RE, v7log):
                check(False, "the v7-era brain logged errors")
        subprocess.run([BRAIN, "--migrate-only", "--database.migrations_dir=" + os.path.join(ROOT, "services", "db", "migrations")], cwd=ROOT,
                       env=env, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if DRIVER != "postgres":
            check(True, "fresh %s database migrated to head" % DRIVER)
        procs, logs = start_brains(work, env, [
            ("rank-a-%d" % os.getpid(), "us-nj", ["--brain.ranked.min_level=1"] + common + wide),
            ("rank-b-%d" % os.getpid(), "de-fra", ["--brain.ranked.min_level=1"] + common + wide),
            ("rank-c-%d" % os.getpid(), "de-fra", common)])
        pa, pb, pc = (bound_port(logs[i], procs[i]) for i in range(3))

        async def run():
            node = FakeNode(pa)
            await node.start()
            await asyncio.sleep(1.5)
            if not seeded["accs"]:  # no upgrade path on this driver: the seed check uses a fake profile
                seeded["accs"] = []
            accs = await scenario(pa, pb, pc, node, seeded if DRIVER == "postgres" else {"accs": []})
            node.task.cancel()
            await node.ws.close()
            return accs
        accs = asyncio.run(run())
        # season close: restart brain A one season later; the RP conversion of SEASON runs once
        for p in procs:
            p.terminate()
        for p in procs:
            p.wait(5)
        procs, logs2 = start_brains(work, env, [("rank-d-%d" % os.getpid(), "us-nj", ["--brain.ranked.min_level=1", "--brain.ranked.season=%d" % (SEASON + 1)])])
        logs += logs2
        pd = bound_port(logs2[0], procs[0])
        print("== season close")
        alice = accs[0]
        before = None
        for _ in range(60):
            code, p = profile(pd, alice, season=SEASON)
            if code == 200 and p.get("rankedPoints", 0) > 10 * 20 + 100:
                before = p
                break
            time.sleep(0.5)
        mmr = before["profile"]["mmr"] if before else 0
        # alice: 11 matches (10 rated + 1 abandoned): 10 x 20 RP + the 100 RP milestone, then floor(MMR * 11 / 100)
        check(before is not None and before["rankedPoints"] == 10 * 20 + 100 + (mmr * 11) // 100,
              "season %d closed: floor(MMR %d * 11 / 100) RP added (%s)" % (SEASON, mmr, before and before.get("rankedPoints")))
        code, p8 = profile(pd, alice, season=SEASON + 1)
        check(code == 200 and p8["profile"]["total_matches"] == 0 and p8.get("currentSeason") == SEASON + 1, "the next season starts empty")
        print("== RP shop")
        if DRIVER == "postgres":
            psql("INSERT INTO ranked_shop (skin_id, price, max_purchase) VALUES (55, 100, 1)")
            code, shop = http(pd, "/mm/ranked/shop", token=alice["token"])
            item = shop.get("skins", [{}])[0] if code == 200 else {}
            check(item.get("skin_id") == 55 and item.get("price") == 100 and item.get("purchase_count") == 0, "shop lists the item")
            code, buy = http(pd, "/mm/ranked/purchase", method="POST", body={"skin_id": item.get("id")}, token=alice["token"])
            check(code == 200 and buy.get("success") and buy.get("balance") == before["rankedPoints"] - 100, "purchase: -100 RP (%s)" % buy)
            code, buy = http(pd, "/mm/ranked/purchase", method="POST", body={"skin_id": item.get("id")}, token=alice["token"])
            check(code == 400 and "limit" in buy.get("error", ""), "max_purchase 1")
            check(psql("SELECT count FROM inventory WHERE account_id = %d AND skin_id = 55" % alice["id"]) == "1", "the skin is in the inventory")
            # Q6: restricted_by "mmr" from the purchase's own rating (the account's peak MMR); Q11: max_purchase 0 = no limit
            psql("INSERT INTO ranked_shop (skin_id, price, max_purchase) VALUES (56, 1, 0)")
            psql("INSERT INTO ranked_shop (skin_id, price, max_purchase, min_mmr) VALUES (57, 1, 1, 9000)")
            psql("INSERT INTO ranked_shop (skin_id, price, max_purchase, max_mmr) VALUES (58, 1, 1, 1)")
            psql("INSERT INTO ranked_shop (skin_id, price, max_purchase, min_mmr, max_mmr) VALUES (59, 1, 1, 1, 9999)")
            code, shop = http(pd, "/mm/ranked/shop", token=alice["token"])
            rows = {x.get("skin_id"): x for x in shop.get("skins", [])} if code == 200 else {}
            check(code == 200 and all(k in rows for k in (56, 57, 58, 59)) and rows[57]["restricted_by"] == "mmr" and rows[58]["restricted_by"] == "mmr"
                  and rows[59]["restricted_by"] is None and rows[56]["restricted_by"] is None,
                  "shop restricted_by: mmr outside the peak window only (%s)" % {k: rows.get(k, {}).get("restricted_by") for k in (56, 57, 58, 59)})
            buys = [http(pd, "/mm/ranked/purchase", method="POST", body={"skin_id": rows[56]["id"]}, token=alice["token"])[1] for _ in range(2)]
            check(all(b.get("success") for b in buys), "max_purchase 0 is unlimited: two purchases")
            code, buy = http(pd, "/mm/ranked/purchase", method="POST", body={"skin_id": rows[57]["id"]}, token=alice["token"])
            check(code == 400 and "Rating" in buy.get("error", ""), "a restricted item is refused by the purchase too (%s)" % buy)
    finally:
        for p in procs:
            if p.poll() is None:
                p.terminate()
        for p in procs:
            try:
                p.wait(5)
            except subprocess.TimeoutExpired:
                p.kill()
        if DRIVER == "postgres":
            subprocess.run(pg + ["drop", SCHEMA])
        else:
            subprocess.run(pg_drop)
    print("== brain logs")
    bad = []
    for path in logs:
        with open(path) as f:
            bad += [l.strip() for l in f if re.search(LOG_ERROR_RE, l)]
    check(not bad, "no statement errors in the brain logs %s" % bad[:3])
    print("ranked test: " + ("OK" if not FAILS else "%d failure(s)" % len(FAILS)))
    if FAILS:
        print("logs kept in " + work)
    else:
        shutil.rmtree(work, ignore_errors=True)
    sys.exit(1 if FAILS else 0)


main()
