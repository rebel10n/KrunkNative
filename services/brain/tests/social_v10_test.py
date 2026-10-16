#!/usr/bin/env python3
"""Krunker v10.0.2 Social hub backend (services/brain/social_eb.c, social_gapi.c, db_social_v10.c):
kn_brain on sqlite with its own config, two (three) accounts over the social socket (JSON frames).

  social_v10_test.py <native build dir> <repo root>

Friends event bus ["eb", {...}]: follow + follow back = friends (both get a fresh friends:list),
presence:online / offline, msg:send -> msg:sent + msg:recv, EB_NOT_FRIEND / EB_TARGET_OFFLINE /
EB_TEXT_TOO_LONG, msg:history paging (oldest first, hasMore), msg:read + unread:summary on login.
gapi tunnel ["gapi", rid, method, path, body]: /players/search, /players/<name> (follow_state,
online), /players/<name>/follow, followers / following, /social/post (level gate, giveaway funds
and fee, draw), /social/feed tabs, /social/post/<id> (+ comments), vote, comment, /social/news,
/clans/by-name/<tag>, /clans/<id>/profile, /clans/<id>/apply.
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
WORK = tempfile.mkdtemp(prefix="kn-social-v10.", dir=os.path.expanduser("~/.cache"))
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


async def scenario(names):
    A, B, C = [await Sock().open() for _ in range(3)]
    ids, tokens = [], []
    for s, n in zip((A, B, C), names):
        i, t = await s.register(n)
        ids.append(i)
        tokens.append(t)
    a_id, b_id, c_id = ids
    an, bn, cn = names

    # --- friends: follow, follow back
    await A.send(["eb", {"type": "friends:list", "offset": 0}])
    f = await A.eb("friends:list")
    check(f and f["friends"] == [] and f["offset"] == 0 and f["hasMore"] is False, "no friends yet: empty friends:list")
    st, r = await A.gapi("POST", "/players/%s/follow" % bn, {"follow": True})
    check(st == 200 and r["data"]["following"] is True, "A follows B (gapi follow)")
    st, r = await B.gapi("GET", "/players/%s" % an)
    check(st == 200 and r["data"]["player_name"] == an and r["data"]["follow_state"] == 0 and r["data"]["online"] == 1,
          "B opens A's profile: not followed, online")
    st, r = await B.gapi("POST", "/players/%s/follow" % an, {"follow": True})
    check(st == 200 and r["data"]["following"] is True, "B follows A back")
    fa = await A.eb("friends:list", pred=lambda o: len(o["friends"]) == 1)
    fb = await B.eb("friends:list", pred=lambda o: len(o["friends"]) == 1)
    check(fa and fa["friends"][0]["id"] == b_id and fa["friends"][0]["name"] == bn and fa["friends"][0]["online"] is True,
          "the follow back pushes A's friends list: B online")
    check(fb and fb["friends"][0]["id"] == a_id, "and B's: A")
    st, r = await B.gapi("GET", "/players/%s" % an)
    check(st == 200 and r["data"]["follow_state"] == 1, "A's profile now shows Unfollow (follow_state 1)")
    st, r = await A.gapi("GET", "/players/%s/followers" % an)
    check(st == 200 and [x["player_name"] for x in r["data"]] == [bn] and r["data"][0]["follow_state"] == 1,
          "A's followers: B, followed back")

    # --- DMs
    await A.send(["eb", {"type": "msg:send", "to": b_id, "text": "  hello b  "}])
    sent = await A.eb("msg:sent")
    recv = await B.eb("msg:recv")
    check(sent and sent["to"] == b_id and sent["id"] > 0 and sent["ts"] > 0, "A: msg:sent with the server id")
    check(recv and recv["from"] == a_id and recv["text"] == "hello b" and recv["id"] == sent["id"], "B: msg:recv (trimmed)")
    await A.send(["eb", {"type": "msg:send", "to": c_id, "text": "hi c"}])
    e = await A.eb("error")
    check(e and e["code"] == "EB_NOT_FRIEND", "messaging a non-friend: EB_NOT_FRIEND")
    await A.send(["eb", {"type": "msg:send", "to": b_id, "text": "x" * 513}])
    e = await A.eb("error")
    check(e and e["code"] == "EB_TEXT_TOO_LONG", "513 characters: EB_TEXT_TOO_LONG")
    for i in range(20):
        await B.send(["eb", {"type": "msg:send", "to": a_id, "text": "m%d" % i}])
        await B.eb("msg:sent")
    await A.send(["eb", {"type": "msg:history", "peer": b_id, "before": 0, "limit": 15}])
    h = await A.eb("msg:history:result")
    check(h and h["peer"] == b_id and len(h["messages"]) == 15 and h["hasMore"] is True and h["requestBefore"] is None
          and h["messages"][-1]["text"] == "m19" and h["messages"][0]["text"] == "m5", "history: newest 15, oldest first, hasMore")
    await A.send(["eb", {"type": "msg:history", "peer": b_id, "before": h["messages"][0]["id"], "limit": 25}])
    h2 = await A.eb("msg:history:result")
    check(h2 and len(h2["messages"]) == 6 and h2["hasMore"] is False and h2["messages"][0]["text"] == "hello b"
          and h2["requestBefore"] == h["messages"][0]["id"], "history: load earlier reaches the start")
    # unread: A reconnects -> unread:summary {B: 20}; msg:read clears it
    await A.close()
    off = await B.eb("presence:offline", pred=lambda o: o["id"] == a_id)
    check(off is not None, "A closes its socket: B gets presence:offline")
    A = await Sock().open()
    await A.login(an, tokens[0])
    u = await A.eb("unread:summary")
    check(u and u["counts"].get(str(b_id), {}).get("count") == 20 and u["counts"][str(b_id)]["name"] == bn,
          "A signs in again: unread:summary 20 from B")
    on = await B.eb("presence:online", pred=lambda o: o["id"] == a_id)
    check(on is not None, "B gets presence:online")
    await A.send(["eb", {"type": "msg:read", "peer": b_id}])
    await A.send(["eb", {"type": "friends:list", "offset": 0}])
    f = await A.eb("friends:list")
    check(f and f["friends"][0]["unread"] == 0 and f["friends"][0]["lastText"] == "m19" and f["friends"][0]["lastFromMe"] is False,
          "after msg:read: unread 0, last message m19 from B")
    await B.close()
    await A.eb("presence:offline", pred=lambda o: o["id"] == b_id)
    await A.send(["eb", {"type": "msg:send", "to": b_id, "text": "you there?"}])
    e = await A.eb("error")
    check(e and e["code"] == "EB_TARGET_OFFLINE", "B offline: EB_TARGET_OFFLINE")
    B = await Sock().open()
    await B.login(bn, tokens[1])

    # --- search
    st, r = await C.gapi("GET", "/players/search?q=%s&limit=20" % an[:6])
    check(st == 200 and any(x["player_name"] == an for x in r["data"]["results"]), "search finds A")
    st, r = await C.gapi("GET", "/players/search?q=x&limit=20")
    check(st == 400 and r["error"]["code"] == "PLAYERS_SEARCH_INVALID", "one character: PLAYERS_SEARCH_INVALID")
    st, r = await C.gapi("GET", "/players/nobody_here_zz")
    check(st == 404 and r["error"]["code"] == "PLAYERS_NOT_FOUND", "unknown profile: PLAYERS_NOT_FOUND")

    # --- posts
    st, r = await A.gapi("POST", "/social/post", {"type": 0, "text": "low level"})
    check(st == 400 and r["error"]["code"] == "SOCIAL_LEVEL_TOO_LOW", "level 1: SOCIAL_LEVEL_TOO_LOW")
    q("UPDATE players SET player_score = 2000000, player_funds = 1000 WHERE player_id IN (?, ?)", a_id, b_id)
    st, r = await A.gapi("POST", "/social/post", {"type": 0, "text": "hello world"})
    check(st == 200 and r["data"]["post_id"] > 0, "level 42: post created")
    pid = r["data"]["post_id"] if st == 200 else 0
    st, r = await A.gapi("POST", "/social/post", {"type": 9, "text": "giveaway!", "funds": 100})
    check(st == 200 and r["data"]["player_funds"] == 1000 - 110, "giveaway of 100 KR: 110 KR charged (10 % fee)")
    gid = r["data"]["post_id"] if st == 200 else 0
    st, r = await A.gapi("POST", "/social/post", {"type": 9, "text": "too big", "funds": 5000})
    check(st == 400 and r["error"]["code"] == "SOCIAL_INSUFFICIENT_FUNDS", "giveaway above the balance: SOCIAL_INSUFFICIENT_FUNDS")
    st, r = await B.gapi("GET", "/social/feed?tab=global")
    posts = r["data"]["posts"] if st == 200 else []
    check([p["pid"] for p in posts][:2] == [gid, pid] and posts[0]["typ"] == 9 and posts[0]["fds"] == 100
          and 0 < posts[0]["exp"] <= 24 * 3600 * 1000 and posts[1]["txt"] == "hello world" and posts[1]["pln"] == an,
          "global feed: the giveaway (fds, exp) and the post")
    st, r = await B.gapi("GET", "/social/feed?tab=giveaways")
    check(st == 200 and [p["pid"] for p in r["data"]["posts"]] == [gid], "giveaways tab: the running giveaway")
    st, r = await C.gapi("GET", "/social/feed?tab=following")
    check(st == 200 and r["data"]["posts"] == [], "C follows nobody: empty Following tab")
    st, r = await B.gapi("POST", "/social/post/%d/vote" % pid, {"value": 1})
    check(st == 200, "B likes the post")
    st, r = await B.gapi("POST", "/social/post/%d/comment" % pid, {"text": "nice"})
    check(st == 200, "B comments")
    st, r = await A.gapi("GET", "/social/post/%d" % pid)
    check(st == 200 and r["data"]["post"]["vot"] == 1 and r["data"]["post"]["cnt"] == 1 and r["data"]["post"]["dlk"] == 0
          and r["data"]["comments"][0]["txt"] == "nice" and r["data"]["comments"][0]["pln"] == bn, "post detail: 1 like, B's comment")
    st, r = await B.gapi("GET", "/social/feed?tab=hot")
    check(st == 200 and r["data"]["posts"][0]["pid"] == pid and r["data"]["posts"][0]["dlk"] == 1, "hot tab: the liked post first")
    st, r = await B.gapi("POST", "/social/post/%d/vote" % pid, {"value": 0})
    st, r = await A.gapi("GET", "/social/post/%d" % pid)
    check(st == 200 and r["data"]["post"]["vot"] == 0, "unlike")
    st, r = await A.gapi("GET", "/social/news")
    check(st == 200 and r["data"]["news"] == ["<b>KN news</b>"], "news from accounts.news")
    # the giveaway draw: B (follows A) likes it; end it now and wait for the timer
    await B.gapi("POST", "/social/post/%d/vote" % gid, {"value": 1})
    q("UPDATE feed_posts SET ends_at = " + AGO_1S + " WHERE id = ?", gid)
    end = time.time() + 10
    while time.time() < end and not q("SELECT settled_at FROM feed_posts WHERE id = ? AND settled_at IS NOT NULL", gid):
        time.sleep(0.2)
    won = q("SELECT winner_id FROM feed_posts WHERE id = ?", gid)
    check(won and won[0][0] == b_id and q("SELECT player_funds FROM players WHERE player_id = ?", b_id)[0][0] == 1100,
          "giveaway ends: B (liked + follows) wins 100 KR")

    # --- mail: the giveaway's mail is Inbox mail in v10's notification window (km_notificationtype 3)
    await B.send(["kml", 0])
    m = await B.recv_until(lambda f: f[0] == "kml")
    rows = m[2] if m else []
    check(any(r.get("km_subject") == "Giveaway Won" and r.get("km_notificationtype") == 3 and r.get("km_link") == ""
              for r in rows), "mail: the giveaway win is Inbox mail (km_notificationtype 3)")

    # --- clans: the clan view and Request to Join
    cid = q("INSERT INTO clans (clan_name, clan_playerid, clan_score, clan_discord) VALUES ('TST', ?, 4321, 'abc') RETURNING clan_id", a_id)[0][0]
    q("INSERT INTO clan_members (account_id, clan_id, rank, score) VALUES (?, ?, 3, 99)", a_id, cid)
    st, r = await C.gapi("GET", "/clans/by-name/tst")
    check(st == 200 and r["data"]["clan_id"] == cid and r["data"]["creator_name"] == an and r["data"]["clan_score"] == 4321
          and r["data"]["clan_link4"] == "abc" and r["data"]["members"][0]["p"] == an and r["data"]["members"][0]["r"] == 3,
          "clan view by name: creator, score, Discord, members")
    st, r = await C.gapi("GET", "/clans/%d/profile" % cid)
    check(st == 200 and r["data"]["clan_name"] == "TST", "clan view by id")
    st, r = await C.gapi("POST", "/clans/%d/apply" % cid)
    check(st == 200 and q("SELECT 1 FROM clan_requests WHERE clan_id = ? AND account_id = ?", cid, c_id),
          "Request to Join: a clan request row")
    st, r = await C.gapi("GET", "/clans/by-name/nope")
    check(st == 404, "unknown clan: 404")
    st, r = await C.gapi("GET", "/nothing/here")
    check(st == 404 and r["error"]["code"] == "NOT_FOUND", "unknown path: 404")
    for s in (A, B, C):
        await s.close()


def main():
    log = open(os.path.join(WORK, "brain.log"), "w")
    cfg = os.path.join(WORK, "brain.json")
    dbcfg = {"driver": "sqlite", "path": DB}
    if DRIVER == "postgres":  # a scratch schema (config/config.json credentials, or KN_TEST_CONFIG)
        with open(os.environ.get("KN_TEST_CONFIG", os.path.join(ROOT, "config", "config.json"))) as fp:
            db = json.load(fp).get("database", {})
        dbname = os.environ.get("KN_TEST_DB_NAME", "krunknative_svcacc")
        PG["schema"] = "kn_test_social_v10_%d" % os.getpid()
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
                   "brain": {"host": "127.0.0.1", "port": PORT, "federation": False, "internal_secret": "sv10-%d" % os.getpid()},
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
        asyncio.run(scenario(["sva%s" % stamp, "svb%s" % stamp, "svc%s" % stamp]))
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
    print("social v10 backend: " + ("OK" if not FAILS else "%d failure(s), files in %s" % (len(FAILS), WORK)))
    if not FAILS and not os.environ.get("KN_KEEP"):
        shutil.rmtree(WORK, ignore_errors=True)
    sys.exit(1 if FAILS else 0)


main()
