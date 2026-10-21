#!/usr/bin/env python3
"""Krunker v10.0.2 Social hub backend (services/brain/social_eb.c, social_gapi.c, db_social_v10.c):
kn_brain on sqlite with its own config, two (three) accounts over the social socket (JSON frames).

  social_v10_test.py <native build dir> <repo root>

Friends event bus ["eb", {...}]: follow + follow back = friends (both get a fresh friends:list), a follow not
followed back is a Followers entry (kml kind 0) and a gmc count,
presence:online / offline, msg:send -> msg:sent + msg:recv, EB_NOT_FRIEND / EB_TARGET_OFFLINE /
EB_TEXT_TOO_LONG, msg:history paging (oldest first, hasMore), msg:read + unread:summary on login, a switch
on a signed-in socket (restore of another account, no logout) takes the account left offline for its friends.
gapi tunnel ["gapi", rid, method, path, body]: /players/search, /players/<name> (follow_state,
online), /players/<name>/follow, followers / following, /social/post (level gate, giveaway funds
and fee, draw), /social/feed tabs, /social/post/<id> (+ comments), vote, comment, /social/news,
/clans/by-name/<tag>, /clans/<id>/profile, /clans/<id>/apply; the mail of a giveaway win (kml rows as
Inbox mail); reports by v10's reason index or name (rpt, grp, GET /players/<name>/reports, POST .../report).
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
    gm = await B.recv_until(lambda f: f[0] == "gmc" and f[1] == 1)
    check(gm is not None, "B's unread count: gmc 1 (A's Followers entry)")
    await B.send(["kml", 0])
    m = await B.recv_until(lambda f: f[0] == "kml")
    fol = [x for x in (m[2] if m else []) if x.get("km_notificationtype") == 0]
    check(len(fol) == 1 and fol[0]["km_link"] == an and fol[0]["km_playerfrom"] == an and fol[0]["km_archive"] == 0,
          "B's Followers tab: one unread entry, A has followed you (km_link A)")
    st, r = await B.gapi("GET", "/players/%s" % an)
    check(st == 200 and r["data"]["player_name"] == an and r["data"]["follow_state"] == 0 and r["data"]["online"] == 1,
          "B opens A's profile: not followed, online")
    st, r = await B.gapi("POST", "/players/%s/follow" % an, {"follow": True})
    check(st == 200 and r["data"]["following"] is True, "B follows A back")
    check(await B.recv_until(lambda f: f[0] == "gmc" and f[1] >= 2, timeout=1.0) is None,
          "a follow back makes no Followers entry and no gmc (v10 has no notice for it)")
    await B.send(["kml", 0])
    m = await B.recv_until(lambda f: f[0] == "kml")
    check(len([x for x in (m[2] if m else []) if x.get("km_notificationtype") == 0]) == 1,
          "B's Followers tab still holds just A's entry after the follow back")
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
    check(h and h["peer"] == b_id and len(h["messages"]) == 15 and h["hasMore"] is True and h.get("requestBefore") == 0
          and h["messages"][-1]["text"] == "m19" and h["messages"][0]["text"] == "m5",
          "history: newest 15, oldest first, hasMore; requestBefore echoes before 0")
    # v10 files the pending request under "<peer>:<requestBefore>" (10.0.3 msg:history:result), so the echo
    # must match the request: no before sent -> no requestBefore (v10's key is "<peer>:undefined")
    await A.send(["eb", {"type": "msg:history", "peer": b_id, "limit": 5}])
    h0 = await A.eb("msg:history:result")
    check(h0 and "requestBefore" not in h0 and len(h0["messages"]) == 5 and h0["messages"][-1]["text"] == "m19",
          "history without before: no requestBefore, newest 5")
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

    # --- chat ban (feature_bans 'chat', the staff row): the DM and the social chat line are refused
    before = q("SELECT count(*) FROM chat_messages WHERE sender_id = ?", a_id)[0][0]
    q("INSERT INTO feature_bans (account_id, feature, reason) VALUES (?, 'chat', 'test ban')", a_id)
    await A.send(["eb", {"type": "msg:send", "to": b_id, "text": "banned hello"}])
    e = await A.eb("error")
    check(e and e["code"] == "EB_MSG_INVALID" and "banned" in e["message"], "chat ban: msg:send is refused with EB_MSG_INVALID")
    check(await B.eb("msg:recv", timeout=1.0) is None, "chat ban: B gets no msg:recv")
    await A.send(["ict", b_id, "banned chat line"])
    f = await A.recv_until(lambda f: f[0] == "chf")
    check(f is not None and f[1] is False, "chat ban: the social chat line (ict) is refused with chf false")
    check(q("SELECT count(*) FROM chat_messages WHERE sender_id = ?", a_id)[0][0] == before,
          "chat ban: no chat_messages row was written for A")
    q("UPDATE feature_bans SET revoked_at = ? WHERE account_id = ?", "2000-01-01 00:00:00", a_id)
    await A.send(["eb", {"type": "msg:send", "to": b_id, "text": "unbanned hello"}])
    check(await A.eb("msg:sent") is not None, "chat ban revoked: msg:send goes through again")

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
    # --- reports: v10's page sends the reason's index (0..6), v7's hub the name; the Reports tab lists them
    await B.send(["rpt", an, 0, "They are using an aimbot in the lobby"])
    f = await B.recv_until(lambda f: f[0] == "rptr")
    check(f and f[1] is True, "report by index 0 (Hacking): rptr true")
    await C.send(["rpt", an, "Scamming", "asked me for my KR, detail here"])
    f = await C.recv_until(lambda f: f[0] == "rptr")
    check(f and f[1] is True, "report by name (v7 hub): rptr true")
    await B.send(["rpt", an, 9, "a reason that is not in the list"])
    f = await B.recv_until(lambda f: f[0] == "rptr")
    check(f and f[1] is None, "reason index 9: refused (rptr null)")
    st, r = await B.gapi("GET", "/players/%s/reports" % an)
    check(st == 200 and r["data"] == [], "a plain account sees no reports")
    q("UPDATE players SET player_moderator = 1 WHERE player_id = ?", c_id)
    st, r = await C.gapi("GET", "/players/%s/reports" % an)
    rows = r["data"] if st == 200 else []
    check([x["rp_index"] for x in rows] == [2, 0] and rows[0]["rp_reportername"] == cn and rows[1]["rp_text"] == "They are using an aimbot in the lobby"
          and rows[1]["rp_reportername"] == bn, "moderator: newest first, rp_index is v10's reason index")
    await C.send(["grp", an])
    f = await C.recv_until(lambda f: f[0] == "rps")
    check(f and [x["rp_index"] for x in f[1]] == [2, 0], "the socket's grp lists the same rows")
    st, r = await B.gapi("POST", "/players/%s/report" % an, {"reason": 3, "text": "a second report, by B"})
    check(st == 200 and r["data"] == {}, "POST /players/<name>/report: filed")
    st, r = await B.gapi("POST", "/players/%s/report" % an, {"reason": 7, "text": "unknown reason"})
    check(st == 400 and r["error"]["code"] == "PLAYERS_REPORT_INVALID", "gapi report: reason 7 is PLAYERS_REPORT_INVALID")
    st, r = await A.gapi("POST", "/players/%s/report" % an, {"reason": 0, "text": "reporting myself"})
    check(st == 400 and r["error"]["code"] == "PLAYERS_REPORT_FAILED", "a self report is refused (PLAYERS_REPORT_FAILED)")
    st, r = await C.gapi("GET", "/players/%s/reports" % an)
    check(st == 200 and [x["rp_index"] for x in r["data"]] == [3, 2, 0], "the gapi report is in the moderator's list")
    q("UPDATE players SET player_moderator = 0 WHERE player_id = ?", c_id)

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


async def friends_extras(stamp):
    """friends:list paging (50 a page, offset, hasMore), presence:ping -> presence:pong (also signed out),
    a signed-out socket's friends:list -> EB_AUTH_REQUIRED, an unknown type -> EB_UNKNOWN_TYPE, the
    message length in UTF-16 units (a 4-byte emoji is 2: 256 of them fit, 257 do not), and Remove
    Friend (unfollow) empties the friends list on both sides."""
    X = await Sock().open()
    xid, _ = await X.register("pgx" + stamp)
    peers = []  # (id, name, socket): 52 friends, followed both ways
    for i in range(52):
        s = await Sock().open()
        name = "pg%s%02d" % (stamp, i)
        pid, _ = await s.register(name)
        peers.append((pid, name, s))
    rows = []
    for pid, _, _ in peers:
        rows += [(xid, pid), (pid, xid)]
    q("INSERT INTO follows (follower_id, followee_id) VALUES " + ", ".join(["(?, ?)"] * len(rows)), *[v for r in rows for v in r])

    await X.send(["eb", {"type": "friends:list", "offset": 0}])
    p1 = await X.eb("friends:list", pred=lambda o: o["offset"] == 0)
    await X.send(["eb", {"type": "friends:list", "offset": 50}])
    p2 = await X.eb("friends:list", pred=lambda o: o["offset"] == 50)
    got = [f["id"] for f in (p1 or {}).get("friends", [])] + [f["id"] for f in (p2 or {}).get("friends", [])]
    check(p1 and len(p1["friends"]) == 50 and p1["hasMore"] is True, "friends:list page 1: 50 friends, hasMore")
    check(p2 and len(p2["friends"]) == 2 and p2["hasMore"] is False, "friends:list offset 50: the last 2, no more")
    check(sorted(got) == sorted(pid for pid, _, _ in peers), "the two pages hold every friend once")

    await X.send(["eb", {"type": "presence:ping"}])
    check(await X.eb("presence:pong") is not None, "presence:ping: presence:pong")
    D = await Sock().open()
    await D.send(["eb", {"type": "presence:ping"}])
    check(await D.eb("presence:pong") is not None, "a signed-out socket still gets presence:pong")
    await D.send(["eb", {"type": "friends:list", "offset": 0}])
    e = await D.eb("error")
    check(e and e["code"] == "EB_AUTH_REQUIRED" and e["message"] == "You must be signed in to use chat.",
          "signed out: friends:list -> EB_AUTH_REQUIRED")
    await D.close()
    await X.send(["eb", {"type": "msg:shout", "to": xid, "text": "x"}])
    e = await X.eb("error")
    check(e and e["code"] == "EB_UNKNOWN_TYPE", "an unknown event-bus type: EB_UNKNOWN_TYPE")

    first_id, _, first = peers[0]
    await X.send(["eb", {"type": "msg:send", "to": first_id, "text": "\U0001F600" * 257}])  # 514 UTF-16 units
    e = await X.eb("error")
    check(e and e["code"] == "EB_TEXT_TOO_LONG", "257 emoji (514 UTF-16 units): EB_TEXT_TOO_LONG")
    await X.send(["eb", {"type": "msg:send", "to": first_id, "text": "\U0001F600" * 256}])  # 512 units
    sent = await X.eb("msg:sent")
    check(sent and sent["to"] == first_id, "256 emoji (512 UTF-16 units): sent")

    # Remove Friend (window 5: deleteFollower -> unfollow): both friends lists drop the pair
    gone_id, gone_name, gone = peers[1]
    st, r = await X.gapi("POST", "/players/%s/follow" % gone_name, {"follow": False})
    check(st == 200 and r["data"]["following"] is False, "Remove Friend: unfollow")
    fx = await X.eb("friends:list", pred=lambda o: o["offset"] == 0 and all(f["id"] != gone_id for f in o["friends"]))
    check(fx and len(fx["friends"]) == 50 and fx["hasMore"] is True, "unfollow: X's friends list (51 left, page 1) drops the peer")
    fg = await gone.eb("friends:list", pred=lambda o: o["offset"] == 0 and all(f["id"] != xid for f in o["friends"]))
    check(fg is not None and fg["friends"] == [], "unfollow: the peer's friends list drops X (none left)")
    for _, _, s in peers:
        await s.close()
    await X.close()


async def switch_presence(stamp):
    """The Account Manager's Use on a signed-in social socket: the restore frame of another account, with no
    logout first. The socket becomes that account, and the account it leaves goes offline for its friends
    (presence:offline, as a logout does) while its session token stays good (it signs in again with it). A
    refused switch leaves the account signed in and online for its friends."""
    L = await Sock().open()
    lid, ltok = await L.register("swl" + stamp)
    N = await Sock().open()
    nid, ntok = await N.register("swn" + stamp)
    F = await Sock().open()
    fid, _ = await F.register("swf" + stamp)
    await L.gapi("POST", "/players/swf%s/follow" % stamp, {"follow": True})
    await F.gapi("POST", "/players/swl%s/follow" % stamp, {"follow": True})
    await F.send(["eb", {"type": "friends:list", "offset": 0}])
    f = await F.eb("friends:list", pred=lambda o: any(x["id"] == lid and x["online"] is True for x in o["friends"]))
    check(f is not None, "switch: F's friends list shows L online")

    await L.send(["a", 1, ["swn" + stamp, None, "not-a-session-token"]])
    f = await L.recv_until(lambda f: f[0] == "a")
    check(f and f[1] is not None, "a switch with a refused token is refused")
    off = await F.eb("presence:offline", pred=lambda o: o["id"] == lid, timeout=2.0)
    check(off is None, "a refused switch: L stays online for F (no presence:offline)")

    await L.send(["a", 1, ["swn" + stamp, None, ntok]])
    f = await L.recv_until(lambda f: f[0] == "a")
    check(f and f[1] is None and f[2] == nid, "the switch: L's socket is signed in as N")
    off = await F.eb("presence:offline", pred=lambda o: o["id"] == lid, timeout=4.0)
    check(off is not None, "the switch: the account left (L) goes offline for F (presence:offline)")

    Lx = await Sock().open()
    await Lx.send(["a", 1, ["swl" + stamp, None, ltok]])
    f = await Lx.recv_until(lambda f: f[0] == "a")
    check(f and f[1] is None and f[2] == lid, "L's session token is kept: L signs in again with it")
    on = await F.eb("presence:online", pred=lambda o: o["id"] == lid)
    check(on is not None, "and F sees L online again")
    for s in (L, N, F, Lx):
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
        asyncio.run(friends_extras(stamp))
        asyncio.run(switch_presence(stamp))
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
