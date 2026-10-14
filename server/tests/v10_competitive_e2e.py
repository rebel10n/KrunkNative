#!/usr/bin/env python3
"""Krunker v10.0.2 Ranked / Leaderboards / Turf Wars end to end from the v10 page (ui/v10;
client/src/ui/ranked_ui.c, v10_competitive.c, turf_ui.c) against a real kn_brain and server_bin.

  python3 -I server/tests/v10_competitive_e2e.py BUILD_DIR      (run from the repository root)

kn_brain on a scratch database (KN_TEST_DB_DRIVER, default sqlite: no server needed; postgres
through config/config.json) with 1v1 ranked (brain.ranked.team_size=1), brain-linked server_bin nodes
(V10C: the public lobby the clients start in; V10R: started once they are in, the free host of the
ranked match, since a node hosts one lobby at a time), every port from kn_proto.free_ports(). Setup: two level-20 accounts in clan AAA, a third in
clan BBB, Burg Turf Wars kills for both clans (a fake game node's "turf-kills", like a round end).
Then two headless client_bin runs AT ONCE (signed in by their saved token), each clicking through:

  1. Turf Wars (openTurfWars) -> Leaderboards tab -> Top Performing Clans: AAA first, BBB second
  2. Leaderboards (openLeaderboards) -> Players -> KR: the accounts' KR, highest first
  3. Ranked (openRankedMenu): the rankedMenuModal popup with FIND MATCH; FIND MATCH -> the
     "Searching for match..." banner; both are matched into ONE ranked game and join it
  4. the match is hosted on V10R as a ranked lobby for the 2 players

Needs a headless GL device (KRUNKNATIVE_HEADLESS=1: EGL) and python websockets. Exit 77 when the
database or websockets is unavailable.
"""
import asyncio
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
import urllib.request

sys.path.insert(0, os.path.join(os.getcwd(), "server/tests"))
import kn_proto  # noqa: E402

try:
    import websockets
except ImportError:  # pragma: no cover
    print("v10_competitive_e2e: python websockets missing: skipped")
    sys.exit(77)

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
ROOT = os.getcwd()
DRIVER = os.environ.get("KN_TEST_DB_DRIVER", "sqlite")
NAME = "kn_test_v10comp_%d" % os.getpid()
SECRET = "kn-v10comp-%d" % os.getpid()
GAME_PORT, RANKED_PORT = kn_proto.free_ports(2)
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-v10comp-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
STAMP = os.getpid() % 100000
results = []
procs = []


def check(name, ok, detail=""):
    results.append(bool(ok))
    show = detail and (not ok or len(detail) <= 160)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if show else ''}", flush=True)


def read_log(path):
    try:
        with open(path, errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def wait_for(fn, secs=10.0):
    end = time.time() + secs
    while time.time() < end:
        try:
            v = fn()
            if v:
                return v
        except Exception:  # noqa: BLE001 - polling
            pass
        time.sleep(0.1)
    return None


def http(port, path, body=None):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request("http://127.0.0.1:%d%s" % (port, path), data=data,
                                 headers={"Content-Type": "application/json"} if data else {},
                                 method="POST" if data else "GET")
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status, json.loads(r.read() or b"null")
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b"null")


def sql(dbcfg, text, *args):
    """direct database access for the test setup only (levels): sqlite or postgres"""
    if dbcfg.get("driver") == "sqlite":
        con = sqlite3.connect(dbcfg["path"], timeout=30)
        try:
            con.execute(text.replace("$1", "?").replace("$2", "?"), args)
            con.commit()
        finally:
            con.close()
        return
    lit = lambda a: str(a) if isinstance(a, int) else "'" + str(a).replace("'", "''") + "'"
    text = re.sub(r"\$(\d+)", lambda m: lit(args[int(m.group(1)) - 1]), text)
    env = dict(os.environ, PGPASSWORD=str(dbcfg.get("password", "")), PGOPTIONS="-c search_path=" + dbcfg.get("search_path", "public"))
    subprocess.run(["psql", "-h", str(dbcfg.get("host", "127.0.0.1")), "-p", str(dbcfg.get("port", 5432)), "-U", str(dbcfg.get("user")),
                    "-d", dbcfg["name"], "-v", "ON_ERROR_STOP=1", "-qAtX", "-c", text], env=env, check=True, capture_output=True)


class Social:
    def __init__(self, port):
        self.url = "ws://127.0.0.1:%d/social/ws?enc=json" % port
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


async def register(port, name):
    async with Social(port) as s:
        r = await s.call("a", "a", 0, [name, "pw-v10comp-%d" % os.getpid(), 0], "captcha")
        assert r[1] is None, r
        return {"id": r[2], "name": name, "token": r[5]}


async def make_clan(port, owner, tag, members):
    async with Social(port) as o:
        await o.call("a", "a", 1, [owner["name"], None, owner["token"]])
        r = await o.call("cln", "cln", 1, tag, None)
        assert r[1] is None, r
        for m in members:
            async with Social(port) as s:
                await s.call("a", "a", 1, [m["name"], None, m["token"]])
                await s.call("cln", "cln", tag.lower())
            await o.ws.send(json.dumps(["clnR", m["id"], 1]))
            await asyncio.sleep(0.3)


async def turf_kills(port, records):
    """a fake game node's round end: ["turf-kills"] on the internal socket, acknowledged"""
    async with websockets.connect("ws://127.0.0.1:%d/mm/internal/ws" % port, additional_headers={"Authorization": "Bearer " + SECRET}) as ws:
        await ws.send(json.dumps({"type": "hello", "nodeId": "TWF", "region": "local", "publicHost": "127.0.0.1", "publicPort": 23996,
                                  "version": "test", "maxLobbies": 0}))
        await ws.send(json.dumps({"type": "turf-kills", "rid": 9, "matchId": "m9", "mapId": 0, "mode": 0, "records": records}))
        while True:
            m = json.loads(await asyncio.wait_for(ws.recv(), 10))
            if m.get("type") == "turf-kills-reply" and m.get("rid") == 9:
                return m


def node(brain_port, node_id, game_port):
    log = os.path.join(WORK, node_id + ".log")
    gs = os.path.join(WORK, node_id + ".json")
    cfg = {"node_id": node_id, "name": "v10 competitive e2e " + node_id, "region": "local", "listen_port": game_port,
           "public_host": "127.0.0.1", "brain_url": "ws://127.0.0.1:%d/mm/internal/ws" % brain_port,
           "brain_secret": SECRET, "modes": ["ffa"], "max_players": 8, "shutdown_grace": 0, "custom_games": True,
           "idle_kick_seconds": 0, "finder_url": "http://127.0.0.1:%d" % brain_port}
    with open(gs, "w") as f:
        json.dump({"gameserver": cfg}, f)
    p = subprocess.Popen([f"{BUILD}/server_bin", "--headless", "--config", gs, "--cfg", "none"], cwd=ROOT,
                         stdout=open(log, "w"), stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                         start_new_session=True)
    procs.append(p)
    return p, log


SCRIPT = """0 menu
1000 call openTurfWars
2500 call knTwTab 2
2700 call knTwBoard 0
5000 nodes @turfrows .data-table tbody tr
5050 text @turffirst .data-table tbody tr
5100 frames {shots}/turf-{n} 1
5500 call clearPops
6000 call openLeaderboards
7000 call knLbCategory 0
7200 call knLbTab 4
9500 nodes @lbrows .data-table tbody tr
9550 text @lbfirst .data-table tbody tr
9600 frames {shots}/leaderboards-{n} 1
10000 call clearPops
11000 call openRankedMenu
13000 nodes @ranked .rankedMenuModal .start-button
13100 frames {shots}/ranked-{n} 1
13500 call knRkStart
14500 nodes @searching .ranked-matchmaking-status.show
14600 frames {shots}/searching-{n} 1
45000 quit
"""


def client(name, brain_port, storage):
    """One headless client_bin on the v10 page; returns its Popen and log path (the caller waits)."""
    path = os.path.join(WORK, name + ".script")
    with open(path, "w") as f:
        f.write(SCRIPT.format(shots=WORK, n=name))
    cfg = os.path.join(WORK, name + "-cfg")
    os.makedirs(os.path.join(cfg, "krunknative"), exist_ok=True)
    for key, value in storage.items():
        with open(os.path.join(cfg, "krunknative", key + ".json"), "w") as f:
            f.write(value)
    log = os.path.join(WORK, name + ".log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": cfg,
                "XDG_STATE_HOME": os.path.join(WORK, name + "-state"), "KRUNKNATIVE_NO_BROWSER": "1",
                "KRUNKNATIVE_INPUT_SCRIPT": path, "KRUNKNATIVE_WINDOW_SIZE": "1280x720",
                "KRUNKNATIVE_UI_DIR": "ui/v10", "KRUNKNATIVE_BRAIN_URL": "http://127.0.0.1:%d" % brain_port,
                "KRUNKNATIVE_NET_TRANSPORT": "tcp", "KRUNKNATIVE_FINDER_URL": "http://127.0.0.1:%d/servers" % brain_port})
    p = subprocess.Popen(["stdbuf", "-oL", "-eL", f"{BUILD}/client_bin"], cwd=ROOT, stdout=open(log, "w"),
                         stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, start_new_session=True)
    procs.append(p)
    return p, log


def finish(p, name, secs=150):
    try:
        p.wait(secs)
    except subprocess.TimeoutExpired:
        os.killpg(p.pid, signal.SIGTERM)
        try:
            p.wait(5)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
        check(name + ": client finished its script", False, "timed out")


def nodes(text, label):
    m = re.search(r"\[input_script\] nodes t=\d+ @%s elements=\d+ match=(\d+)" % re.escape(label), text)
    return int(m.group(1)) if m else -1


def first_text(text, label):
    m = re.search(r"\[input_script\] text t=\d+ @%s (.*)" % re.escape(label), text)
    return m.group(1) if m else ""


def main():
    if not os.access(f"{BUILD}/client_bin", os.X_OK):
        print("v10_competitive_e2e: no client_bin in %s: skipped" % BUILD)
        return 77
    scratch = [sys.executable, "-I", os.path.join(ROOT, "services", "brain", "tests", "db_scratch.py"), ROOT]
    r = subprocess.run(scratch + ["create", DRIVER, NAME], capture_output=True)
    if r.returncode:
        print("v10_competitive_e2e: %s unavailable: skipped" % DRIVER)
        return r.returncode
    dbcfg = json.loads(r.stdout)
    dbenv = {"KN_DATABASE_" + k.upper(): str(v) for k, v in dbcfg.items()}
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=SECRET, **dbenv)
    brain_log = os.path.join(WORK, "brain.log")
    try:
        subprocess.run([f"{BUILD}/services/brain/kn_brain", "--migrate-only"], cwd=ROOT, env=env, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        brain = subprocess.Popen([f"{BUILD}/services/brain/kn_brain", "--brain.host=127.0.0.1", "--brain.port=0", "--accounts.signup_rewards.enabled=false",
                                  "--brain.region=local", "--brain.id=v10comp-%d" % os.getpid(), "--finder.poll_interval_s=1",
                                  "--brain.ranked.min_level=0", "--brain.ranked.team_size=1", "--brain.ranked.start_grace_s=60",
                                  "--accounts.starting_kr=500"],
                                 cwd=ROOT, env=env, stdout=open(brain_log, "w"), stderr=subprocess.STDOUT,
                                 stdin=subprocess.DEVNULL, start_new_session=True)
        procs.append(brain)
        m = wait_for(lambda: re.search(r"listening on http://127\.0\.0\.1:(\d+) \(ws", read_log(brain_log)), 15)
        check("kn_brain starts", bool(m), read_log(brain_log)[-300:])
        if not m:
            return 1
        port = int(m.group(1))
        _, node_log = node(port, "V10C", GAME_PORT)
        check("the game node links to the brain", bool(wait_for(lambda: "welcomed" in read_log(node_log), 20)), read_log(node_log)[-300:])

        accs = [asyncio.run(register(port, "v10c%d_%d" % (i, STAMP))) for i in range(3)]
        for a in accs:  # level 20 (v10's ranked needs level 10): player_score for a level (the Account level formula)
            sql(dbcfg, "UPDATE players SET player_score = $1 WHERE player_id = $2", int((20 / 0.03) ** 2) + 1, a["id"])
        check("3 accounts registered", all(a["token"] and a["id"] for a in accs), str(accs))
        asyncio.run(make_clan(port, accs[0], "AAA", [accs[1]]))
        asyncio.run(make_clan(port, accs[2], "BBB", []))
        rec = lambda a, clan, k: {"clan_name": clan, "player_id": a["id"], "map_id": 0, "kills": k, "deaths": 1}
        rep = asyncio.run(turf_kills(port, [rec(accs[0], "AAA", 30), rec(accs[1], "AAA", 12), rec(accs[2], "BBB", 20)]))
        check("Burg turf kills recorded for AAA and BBB", rep.get("ok") and rep.get("recorded") == 3, str(rep))
        code, st = http(port, "/api/territorial-wars/status")
        burg = [m for m in (st or {}).get("data", []) if m.get("map_id") == 0]
        check("brain: AAA leads Burg today", code == 200 and burg and burg[0]["leaderboard"][0]["clan_name"] == "AAA",
              json.dumps(burg)[:200])

        runs = []
        for i in range(2):
            storage = {"krunker_username": accs[i]["name"], "krunker_token": accs[i]["token"]}
            runs.append((("player%d" % i),) + client("player%d" % i, port, storage))
        # the clients join V10C's public lobby on start (a node hosts one lobby at a time): a second,
        # empty node comes up once they are in, so the ranked match has a free host
        both_in = wait_for(lambda: all("[mm] joining" in read_log(log) for _, _, log in runs), 60)
        check("both clients start in the public lobby", bool(both_in))
        _, rk_log = node(port, "V10R", RANKED_PORT)
        check("a free node for the ranked match links", bool(wait_for(lambda: "welcomed" in read_log(rk_log), 20)), read_log(rk_log)[-300:])
        for name, p, _ in runs:
            finish(p, name)
        joined = []
        for name, _, log in runs:
            text = read_log(log)
            rows = nodes(text, "turfrows")
            first = first_text(text, "turffirst")
            check("%s: Turf Wars > Leaderboards > Top Performing Clans lists both clans, AAA first" % name,
                  rows == 2 and "AAA" in first and "42" in first, "%d rows, first: %s" % (rows, first[:120]))
            rows = nodes(text, "lbrows")
            first = first_text(text, "lbfirst")
            check("%s: Leaderboards > Players > KR lists the accounts" % name, rows >= 3 and "500" in first,
                  "%d rows, first: %s" % (rows, first[:120]))
            check("%s: Ranked opens v10's popup with its start button" % name, nodes(text, "ranked") == 1, str(nodes(text, "ranked")))
            check("%s: FIND MATCH: the Searching for match banner" % name, nodes(text, "searching") == 1, str(nodes(text, "searching")))
            j = re.findall(r"\[ranked\] joining (\S+)", text)
            check("%s: matched and joining the ranked game" % name, bool(j), str(j))
            joined += j[:1]
        check("both players joined ONE ranked match", len(joined) == 2 and joined[0] == joined[1], str(joined))
        check("V10R hosted it as a ranked match for 2 players", bool(joined) and joined[0].startswith("V10R:")
              and re.search(r"\[ranked\] match \S+ \(ranked\): 2 player", read_log(rk_log)), read_log(rk_log)[-300:])
    finally:
        for p in procs:
            if p.poll() is None:
                os.killpg(p.pid, signal.SIGTERM)
        for p in procs:
            try:
                p.wait(10)
            except subprocess.TimeoutExpired:
                os.killpg(p.pid, signal.SIGKILL)
                p.wait()
        subprocess.run(scratch + ["drop", DRIVER, NAME], capture_output=True)
    if all(results):
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print("logs kept in " + WORK)
    print(f"{sum(results)}/{len(results)} passed")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
