#!/usr/bin/env python3
"""Krunker Strike's friends over kn_brain's event bus (services/brain/social_eb.c game:invite,
game_presence.c game_presence_describe, registry.c's flavor pools (strike-backend)): kn_brain on sqlite with its own
config, three accounts on social sockets (JSON frames) and two fake game-server nodes on
/mm/internal/ws (a Strike node, hello.flavor "strike", and a base node).

  strike_social_test.py <native build dir> <repo root>

  - A and B are friends (mutual follows), C only follows A.
  - A plays in the Strike node's lobby: B's presence:game and friends:list rows carry gameFlavor
    "strike", gameMode / gameMap / gamePlayers / gameMaxPlayers of the lobby; A moves to the base
    node: gameFlavor "base"; A leaves: gameInstanceId null.
  - game:invite {to: B, gameId}: B gets game:invite {from: A, name, gameId}, A game:invite:sent;
    to C (not a friend): EB_NOT_FRIEND; to an offline friend: EB_TARGET_OFFLINE; a bad game id or
    oneself: EB_INVITE_INVALID; past 20 a minute: EB_RATE_LIMITED; a guest socket: EB_AUTH_REQUIRED (v10's code).
77 = skip (no kn_brain / websockets).
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

BUILD, ROOT = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
BRAIN = os.path.join(BUILD, "services", "brain", "kn_brain")
if not os.path.exists(BRAIN):
    print("needs kn_brain in %s: skip" % BUILD)
    sys.exit(77)
sys.path.insert(0, os.path.join(ROOT, "server", "tests"))
from kn_proto import free_ports  # noqa: E402

FAILS = []
WORK = tempfile.mkdtemp(prefix="kn-strike-social.", dir=os.path.expanduser("~/.cache"))
DB = os.path.join(WORK, "coordinator.db")
PORT = free_ports(1)[0]


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what, flush=True)
    if not cond:
        FAILS.append(what)


DRIVER = os.environ.get("KN_TEST_DB_DRIVER", "sqlite")
PG = {}


def lit(v):
    return str(int(v)) if isinstance(v, (int, bool)) else "'" + str(v).replace("'", "''") + "'"


def q(sql, *args):
    parts = sql.split("?")
    text = "".join(p + (lit(args[i]) if i < len(args) else "") for i, p in enumerate(parts))
    if DRIVER != "postgres":
        con = sqlite3.connect(DB, timeout=30)
        try:
            rows = con.execute(text).fetchall()
            con.commit()
            return rows
        finally:
            con.close()
    out = subprocess.run(PG["psql"] + ["-c", "SET search_path TO %s; %s" % (PG["schema"], text)], env=PG["env"],
                         capture_output=True, text=True, check=True).stdout
    rows = []
    for line in out.splitlines():
        if not line or line == "SET" or line.startswith("UPDATE ") or line.startswith("INSERT "):
            continue
        rows.append(tuple(None if c == "" else int(c) if re.fullmatch(r"-?\d+", c) else c for c in line.split("|")))
    return rows


AGO_1S = "now() - interval '1 second'" if DRIVER == "postgres" else "strftime('%Y-%m-%d %H:%M:%f', 'now', '-1 seconds')"


class Sock:
    """One social socket (JSON frames), signed in as `name`."""

    def __init__(self):
        self.ws = None
        self.inbox = []
        self.rid = 0

    async def open(self):
        self.ws = await websockets.connect("ws://127.0.0.1:%d/social/ws?enc=json" % PORT)
        return self

    async def send(self, frame):
        await self.ws.send(json.dumps(frame))

    async def recv_until(self, pred, timeout=8.0):
        for i, f in enumerate(self.inbox):
            if pred(f):
                return self.inbox.pop(i)
        end = time.time() + timeout
        while time.time() < end:
            try:
                f = json.loads(await asyncio.wait_for(self.ws.recv(), max(0.05, end - time.time())))
            except asyncio.TimeoutError:
                break
            if f and f[0] == "pi":
                await self.send(["po"])
                continue
            if pred(f):
                return f
            self.inbox.append(f)
        return None

    async def eb(self, typ, timeout=8.0, pred=None):
        f = await self.recv_until(lambda f: f[0] == "eb" and f[1].get("type") == typ and (pred is None or pred(f[1])), timeout)
        return f[1] if f else None

    async def gapi(self, method, path, body=None):
        self.rid += 1
        rid = self.rid
        await self.send(["gapi", rid, method, path, body])
        f = await self.recv_until(lambda f: f[0] == "gapi" and f[1] == rid)
        return (f[2], f[3]) if f else (0, None)

    async def register(self, name):
        await self.send(["a", 0, [name, "social_pass_1", None]])
        f = await self.recv_until(lambda f: f[0] == "a")
        assert f and f[1] is None, f
        return f[2], f[5]

    async def login(self, name, token):
        await self.send(["a", 1, [name, None, token]])
        f = await self.recv_until(lambda f: f[0] == "a")
        assert f and f[1] is None, f
        return f[2]

    async def close(self):
        await self.ws.close()


SECRET = "strike-social-secret-%d" % os.getpid()


async def node(node_id, flavor):
    ws = await websockets.connect("ws://127.0.0.1:%d/mm/internal/ws" % PORT,
                                  additional_headers={"Authorization": "Bearer " + SECRET})
    hello = {"type": "hello", "nodeId": node_id, "region": "local", "name": node_id, "publicHost": "127.0.0.1",
             "publicPort": 4000, "version": "test", "maxPlayers": 12}
    if flavor:
        hello["flavor"] = flavor
    await ws.send(json.dumps(hello))
    await ws.send(json.dumps({"type": "lobby-up", "lobby": ["l1", "local", 3, 12, {"modeName": "Free for All", "map": "Burg"}]}))
    return ws


async def scenario(names):
    A, B, C = [await Sock().open() for _ in range(3)]
    ids = []
    for s, n in zip((A, B, C), names):
        i, _ = await s.register(n)
        ids.append(i)
    a_id, b_id, c_id = ids
    an, bn, cn = names
    await A.gapi("POST", "/players/%s/follow" % bn, {"follow": True})
    await B.gapi("POST", "/players/%s/follow" % an, {"follow": True})
    await C.gapi("POST", "/players/%s/follow" % an, {"follow": True})
    fb = await B.eb("friends:list", pred=lambda o: len(o["friends"]) == 1)
    check(fb and fb["friends"][0]["id"] == a_id and "gameFlavor" not in fb["friends"][0], "A and B are friends; A in no game")

    strike, base = await node("STK", "strike"), await node("BAS", None)
    await asyncio.sleep(0.3)
    await strike.send(json.dumps({"type": "player-join", "lobbyId": "l1", "accountId": a_id}))
    p = await B.eb("presence:game", pred=lambda o: o.get("id") == a_id and o.get("gameInstanceId"))
    check(p and p["gameInstanceId"] == "STK:l1" and p.get("gameFlavor") == "strike" and p.get("gameMode") == "Free for All"
          and p.get("gameMap") == "Burg" and p.get("gamePlayers") == 3 and p.get("gameMaxPlayers") == 12,
          "A in the Strike node: B's presence:game says gameFlavor strike + the lobby's mode / map / players  -- %s" % p)
    await B.send(["eb", {"type": "friends:list", "offset": 0}])
    fb = await B.eb("friends:list", pred=lambda o: o["friends"] and o["friends"][0].get("gameInstanceId"))
    row = fb["friends"][0] if fb and fb["friends"] else {}
    check(row.get("gameInstanceId") == "STK:l1" and row.get("gameFlavor") == "strike" and row.get("gamePlayers") == 3,
          "B's friends:list row: A in STK:l1, gameFlavor strike  -- %s" % row)
    c_push = await C.eb("presence:game", timeout=1.0)
    check(c_push is None, "C (not a friend) gets no presence:game")
    await strike.send(json.dumps({"type": "player-leave", "lobbyId": "l1", "accountId": a_id}))
    await base.send(json.dumps({"type": "player-join", "lobbyId": "l1", "accountId": a_id}))
    p = await B.eb("presence:game", pred=lambda o: o.get("id") == a_id and o.get("gameInstanceId") == "BAS:l1")
    check(p and p.get("gameFlavor") == "base", "A in the base node: gameFlavor base (a Strike client shows A as not in Strike)  -- %s" % p)
    await base.send(json.dumps({"type": "player-leave", "lobbyId": "l1", "accountId": a_id}))
    p = await B.eb("presence:game", pred=lambda o: o.get("id") == a_id and o.get("gameInstanceId") is None)
    check(p is not None and "gameFlavor" not in p, "A leaves: gameInstanceId null, no lobby fields")

    # --- invites
    await A.send(["eb", {"type": "game:invite", "to": b_id, "gameId": "STK:l1"}])
    inv = await B.eb("game:invite")
    sent = await A.eb("game:invite:sent")
    check(inv and inv["from"] == a_id and inv["name"] == an and inv["gameId"] == "STK:l1", "B gets A's game:invite  -- %s" % inv)
    check(sent and sent["to"] == b_id and sent["gameId"] == "STK:l1", "A gets game:invite:sent")
    await B.send(["eb", {"type": "game:invite", "to": a_id, "gameId": "AB12CD"}])
    inv = await A.eb("game:invite")
    check(inv and inv["from"] == b_id and inv["gameId"] == "AB12CD", "a private lobby code travels as the game id")
    await C.send(["eb", {"type": "game:invite", "to": a_id, "gameId": "STK:l1"}])
    e = await C.eb("error")
    check(e and e["code"] == "EB_NOT_FRIEND", "C (follows A, not followed back) invites A: EB_NOT_FRIEND")
    # an invite refusal names its request: a msg:send's EB_NOT_FRIEND / EB_TARGET_OFFLINE fails a pending message
    # in v10.0.3 (deob-10.0.3.js:156838), an invite's must not
    check(e and e.get("of") == "game:invite", "an invite refusal carries of: game:invite")
    for bad in ({"to": b_id, "gameId": "x y"}, {"to": b_id, "gameId": ""}, {"to": a_id, "gameId": "STK:l1"}, {"to": 0, "gameId": "STK:l1"}):
        await A.send(["eb", dict(type="game:invite", **bad)])
        e = await A.eb("error")
        check(e and e["code"] == "EB_INVITE_INVALID", "invalid invite %s: EB_INVITE_INVALID" % bad)
    await B.close()
    await asyncio.sleep(0.5)
    await A.send(["eb", {"type": "game:invite", "to": b_id, "gameId": "STK:l1"}])
    e = await A.eb("error")
    check(e and e["code"] == "EB_TARGET_OFFLINE" and e.get("of") == "game:invite", "B offline: EB_TARGET_OFFLINE, of game:invite")
    codes = []
    for i in range(20):
        await A.send(["eb", {"type": "game:invite", "to": b_id, "gameId": "STK:l1"}])
        e = await A.eb("error")
        codes.append(e and e["code"])
    check(codes[-1] == "EB_RATE_LIMITED" and "EB_RATE_LIMITED" not in codes[:13],
          "past 20 invites a minute: EB_RATE_LIMITED  -- %s" % codes)
    G = await Sock().open()
    await G.send(["eb", {"type": "game:invite", "to": a_id, "gameId": "STK:l1"}])
    e = await G.eb("error")
    check(e and e["code"] == "EB_AUTH_REQUIRED", "a guest socket: EB_AUTH_REQUIRED")
    for s in (A, C, G):
        await s.close()
    await strike.close()
    await base.close()


def main():
    log = open(os.path.join(WORK, "brain.log"), "w")
    cfg = os.path.join(WORK, "brain.json")
    dbcfg = {"driver": "sqlite", "path": DB}
    if DRIVER == "postgres":  # a scratch schema (config/config.json credentials, or KN_TEST_CONFIG)
        with open(os.environ.get("KN_TEST_CONFIG", os.path.join(ROOT, "config", "config.json"))) as fp:
            db = json.load(fp).get("database", {})
        dbname = os.environ.get("KN_TEST_DB_NAME", "krunknative_svcacc")
        PG["schema"] = "kn_test_strike_social_%d" % os.getpid()
        PG["env"] = dict(os.environ, PGPASSWORD=str(db.get("password", "")))
        PG["psql"] = ["psql", "-h", str(db.get("host", "127.0.0.1")), "-p", str(db.get("port", 5432)), "-U",
                      str(db.get("user", "krunknative")), "-d", dbname, "-v", "ON_ERROR_STOP=1", "-qAt"]
        try:
            subprocess.run(PG["psql"] + ["-c", "CREATE SCHEMA %s" % PG["schema"]], env=PG["env"], check=True, capture_output=True, timeout=30)
        except (OSError, subprocess.SubprocessError) as e:
            print("PostgreSQL unavailable (%s): skip" % e)
            sys.exit(77)
        dbcfg = dict(db, name=dbname, search_path=PG["schema"])
    with open(cfg, "w") as f:
        json.dump({"database": dict(dbcfg, migrations_dir=os.path.join(ROOT, "services", "db", "migrations")),
                   "brain": {"host": "127.0.0.1", "port": PORT, "federation": False, "internal_secret": SECRET},
                   "accounts": {"free_premium": False, "starting_kr": 0, "login_throttle_s": 0, "password_iterations": 1000,
                                "news": ["<b>KN news</b>", ""], "giveaway_check_ms": 300,
                                "skins_file": os.path.join(ROOT, "assets", "skins.json")}}, f)
    brain = subprocess.Popen([BRAIN, "--config", cfg, "--migrate"], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
    try:
        for _ in range(200):
            try:
                with open(os.path.join(WORK, "brain.log")) as f:
                    if re.search(r"listening on http://127\.0\.0\.1:%d" % PORT, f.read()):
                        break
            except OSError:
                pass
            time.sleep(0.1)
        stamp = str(int(time.time()) % 100000)
        asyncio.run(scenario(["ssa%s" % stamp, "ssb%s" % stamp, "ssc%s" % stamp]))
    except Exception as e:  # noqa: BLE001
        import traceback
        traceback.print_exc()
        check(False, "exception: %r" % e)
    finally:
        brain.terminate()
        try:
            brain.wait(10)
        except subprocess.TimeoutExpired:
            brain.kill()
        log.close()
        if PG:
            subprocess.run(PG["psql"] + ["-c", "DROP SCHEMA IF EXISTS %s CASCADE" % PG["schema"]], env=PG["env"], capture_output=True)
    print("strike social backend: " + ("OK" if not FAILS else "%d failure(s), files in %s" % (len(FAILS), WORK)))
    if not FAILS and not os.environ.get("KN_KEEP"):
        shutil.rmtree(WORK, ignore_errors=True)
    sys.exit(1 if FAILS else 0)


main()
