#!/usr/bin/env python3
"""The 45 v10 gapi routes the official client sends over the social socket (the "tunnel" rows of
doc/v10/api-inventory.md): a gapi frame ["gapi", rid, method, path, body] must get the reply the
v10.0.3 consumer reads, {"data": ...} on success or {"error": {"code", "message"}} on failure, with
the field names and the meaning of each value the consumer uses (game.js, decrypted; the same paths
and field names are in ~/jails/kru/src/Krunker_v10.0.3/deob/v10.0.2/game.js). Each check is labelled with its route number.

  gapi_tunnel_test.py <kn_brain> <repo root>

Runs on a kn_brain with its own sqlite database (never config/config.json). 77 = skip (no kn_brain
or python websockets).
"""
import asyncio
import itertools
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

BRAIN, ROOT = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
if not os.path.exists(BRAIN):
    print("no kn_brain: skip")
    sys.exit(77)
sys.path.insert(0, os.path.join(ROOT, "server", "tests"))
from kn_proto import free_ports  # noqa: E402

FAILS = []
WORK = tempfile.mkdtemp(prefix="kn-gapi-tunnel.")
DB = os.path.join(WORK, "coordinator.db")
RID = itertools.count(1)
PASSWORD = "gapi_pass_1"


def check(cond, what, detail=""):
    print(("  ok   " if cond else "  FAIL ") + what + (("  -- " + str(detail)[:300]) if detail and not cond else ""), flush=True)
    if not cond:
        FAILS.append(what)


def lit(v):
    return str(int(v)) if isinstance(v, (int, bool)) else "'" + str(v).replace("'", "''") + "'"


def q(sql, *args):
    parts = sql.split("?")
    text = "".join(p + (lit(args[i]) if i < len(args) else "") for i, p in enumerate(parts))
    con = sqlite3.connect(DB, timeout=30)
    try:
        rows = con.execute(text).fetchall()
        con.commit()
        return rows
    finally:
        con.close()


def err_ok(err):
    """the consumer shows error.message: a refusal must carry a code and a non-empty message"""
    return isinstance(err, dict) and isinstance(err.get("code"), str) and isinstance(err.get("message"), str) and err["message"] != ""


class Sock:
    def __init__(self, ws):
        self.ws = ws
        self.buf = []
        self.id = 0
        self.name = ""

    @classmethod
    async def open(cls, port):
        return cls(await websockets.connect("ws://127.0.0.1:%d/social/ws?enc=json" % port))

    async def send(self, *frame):
        await self.ws.send(json.dumps(list(frame)))

    async def wait(self, pred, timeout=10.0):
        for i, f in enumerate(self.buf):
            if pred(f):
                return self.buf.pop(i)
        end = time.time() + timeout
        while time.time() < end:
            try:
                f = json.loads(await asyncio.wait_for(self.ws.recv(), max(0.05, end - time.time())))
            except asyncio.TimeoutError:
                break
            if pred(f):
                return f
            self.buf.append(f)
        return None

    async def auth(self, name):
        await self.send("a", 0, [name, PASSWORD, None])
        f = await self.wait(lambda f: f[0] == "a")
        assert f and f[1] is None, f
        self.id, self.name = f[2], name
        return f[2]

    async def gapi(self, method, path, body=None):
        rid = next(RID)
        await self.send("gapi", rid, method, path, body)
        f = await self.wait(lambda f: f[0] == "gapi" and f[1] == rid)
        return (f[2], (f[3] or {}).get("data"), (f[3] or {}).get("error")) if f else (0, None, None)

    async def cln(self, action, value=1, extra=None):
        frame = ["cln", action, value] + ([extra] if extra is not None else [])
        await self.send(*frame)
        return await self.wait(lambda f: f[0] == "cln" and len(f) >= 5)

    async def close(self):
        await self.ws.close()


async def scenario(port):
    stamp = str(int(time.time()) % 100000)
    guest = await Sock.open(port)
    A, B, C, D = [await Sock.open(port) for _ in range(4)]
    for s, n in zip((A, B, C, D), ("gta", "gtb", "gtc", "gtd")):
        await s.auth(n + stamp)
    q("UPDATE players SET player_score = 2000000, player_funds = 1000 WHERE player_id IN (?, ?, ?, ?)", A.id, B.id, C.id, D.id)

    # ---- fixtures: a clan (A leads, B joins), a post by A, studios S1 (A, B invited), S2 (A, D), a map
    tag = "T" + "".join("abcdefghij"[int(c)] for c in stamp[-3:])  # letters only: a clan tag rule
    f = await A.cln(1, tag)
    clan = f[2]["clan_id"] if f and f[2] else 0
    check(clan > 0, "fixture: A creates the clan", f)
    f = await B.cln(2, tag)
    await A.send("clnR", B.id, 1)
    await A.wait(lambda f: f[0] == "cln" and len(f) >= 5)
    st, post, perr = await A.gapi("POST", "/social/post", {"type": 0, "text": "gapi tunnel probe"})
    pid = (post or {}).get("post_id", 0)
    st, s1, _ = await A.gapi("POST", "/studios", {"name": "GtS1" + stamp})
    sid = (s1 or {}).get("id", 0)
    st, tmp, _ = await A.gapi("POST", "/studios", {"name": "GtDel" + stamp})
    tmp_id = (tmp or {}).get("id", 0)
    check(sid > 0 and tmp_id > 0, "fixture: A creates studios S1 and a temporary one", (s1, tmp))
    st, s2, _ = await A.gapi("POST", "/studios", {"name": "GtS2" + stamp})
    s2_id = (s2 or {}).get("id", 0)
    st, inv_d, _ = await A.gapi("POST", "/studios/%d/members" % s2_id, {"player_name": D.name})
    await D.gapi("POST", "/studios/invites/%d/accept" % (inv_d or {}).get("invite_id", 0))
    mid = q("INSERT INTO published_maps (map_name, creator_id) VALUES (?, ?) RETURNING map_id", "GtMap" + stamp, A.id)[0][0]

    # ---- guest: the Studios window reads these on open, so a guest gets the empty list
    st, d, e = await guest.gapi("GET", "/studios/mine")
    check(st == 200 and d == [] and e is None, "[195] GET /studios/mine, guest: the empty list (the window reads data || [])", (st, d, e))
    st, d, e = await guest.gapi("GET", "/studios/invites")
    check(st == 200 and d == [] and e is None, "[200] GET /studios/invites, guest: the empty list", (st, d, e))

    # ---- clans (8 routes)
    st, d, e = await A.gapi("POST", "/clans/%d/logo" % clan, {"file": ""})
    check(st >= 400 and err_ok(e), "[110] POST /clans/{id}/logo: a refusal with error.message (no logo source)", (st, e))
    st, d, e = await A.gapi("DELETE", "/clans/%d/logo" % clan)
    check(st == 200 and e is None, "[111] DELETE /clans/{id}/logo: the owner's removal -> data", (st, e))
    st, d, e = await B.gapi("GET", "/clans/%d/motd" % clan)
    check(st == 200 and e is None and isinstance(d, dict) and "message" in d and "set_by_name" in d and "set_at" in d
          and isinstance(d.get("message") or "", str), "[114] GET /clans/{id}/motd: {message, set_by_name, set_at}", (st, d, e))
    st, d, e = await A.gapi("PUT", "/clans/%d/motd" % clan, {"message": "Hello clan"})
    check(st == 200 and e is None, "[115] PUT /clans/{id}/motd by the owner -> data", (st, e))
    st, d, e = await B.gapi("GET", "/clans/%d/motd" % clan)
    check(st == 200 and (d or {}).get("message") == "Hello clan", "[114] GET /clans/{id}/motd reads back the saved message", (st, d))
    st, d, e = await A.gapi("DELETE", "/clans/%d/motd" % clan)
    check(st == 200 and e is None, "[116] DELETE /clans/{id}/motd -> data", (st, e))
    q("INSERT INTO inventory (account_id, skin_id, count) VALUES (?, 7776, 1)", A.id)
    st, d, e = await A.gapi("POST", "/clans/%d/banner-color" % clan, {"paint_item_id": 7776})
    check(st == 200 and e is None and (d or {}).get("clan_banner_color") == "#dc3c3c", "[117] POST /clans/{id}/banner-color: a seeded paint -> the clan data", (st, d, e))
    st, d, e = await C.gapi("GET", "/clans/%d/profile" % clan)
    check(st == 200 and e is None and d.get("clan_name") == tag and isinstance(d.get("members"), list), "[124] GET /clans/{id}/profile: clan_name + members list", (st, d, e))
    st, d, e = await C.gapi("GET", "/clans/by-name/%s" % tag)
    check(st == 200 and e is None and d.get("clan_id") == clan and isinstance(d.get("members"), list), "[125] GET /clans/by-name/{name}: the clan profile", (st, e))

    # ---- players and social (13 routes)
    st, d, e = await C.gapi("GET", "/players/%s" % A.name)
    check(st == 200 and e is None and d.get("player_name") == A.name, "[162] GET /players/{name}: the profile object", (st, e))
    st, d, e = await C.gapi("GET", "/players/search?q=%s&limit=20" % A.name[:6])
    check(st == 200 and any(x.get("player_name") == A.name for x in (d or {}).get("results", [])), "[165] GET /players/search: {results: [...]}", (st, d, e))
    st, d, e = await B.gapi("POST", "/players/%s/follow" % A.name, {"follow": True})
    check(st == 200 and (d or {}).get("following") is True, "[166] POST /players/{name}/follow -> {following: true}", (st, d, e))
    st, d, e = await C.gapi("GET", "/players/%s/followers" % A.name)
    check(st == 200 and isinstance(d, list) and any(x.get("player_name") == B.name for x in d), "[167] GET followers: a list of player rows", (st, d, e))
    st, d, e = await C.gapi("GET", "/players/%s/following" % B.name)
    check(st == 200 and isinstance(d, list) and any(x.get("player_name") == A.name for x in d), "[168] GET following: a list of player rows", (st, d, e))
    st, d, e = await B.gapi("POST", "/players/%s/report" % A.name, {"reason": 0, "text": "probe"})
    check(st == 200 and d == {} and e is None, "[170] POST /players/{name}/report -> {}", (st, d, e))
    st, d, e = await B.gapi("GET", "/players/%s/reports" % A.name)
    check(st == 200 and d == [] and e is None, "[171] GET reports, not a moderator: the empty list", (st, d, e))
    st, d, e = await C.gapi("GET", "/social/feed?tab=global")
    check(st == 200 and any(p.get("pid") == pid for p in (d or {}).get("posts", [])), "[179] GET /social/feed?tab=: {posts: [...]}", (st, e))
    st, d, e = await C.gapi("GET", "/social/feed?player_id=%d" % A.id)
    check(st == 200 and any(p.get("pid") == pid for p in (d or {}).get("posts", [])), "[180] GET /social/feed?player_id=: {posts: [...]}", (st, e))
    st, d, e = await C.gapi("GET", "/social/post/%d" % pid)
    check(st == 200 and (d or {}).get("post", {}).get("pid") == pid and isinstance(d.get("comments"), list), "[181] GET /social/post/{id}: {post, comments}", (st, e))
    check(bool(pid) and perr is None and (post or {}).get("post_id", 0) > 0 and "player_funds" in (post or {}), "[182] POST /social/post: {post_id, player_funds}", (post, perr))
    st, d, e = await B.gapi("POST", "/social/post/%d/vote" % pid, {"value": 1})
    check(st == 200 and (d or {}).get("value") == 1 and e is None, "[183] POST /social/post/{id}/vote {value: 1} -> {value: 1}", (st, d, e))
    st, d, e = await B.gapi("POST", "/social/post/%d/vote" % pid, {"value": 0})
    check(st == 200 and (d or {}).get("value") == 0, "[183] the same route with value 0 unlikes -> {value: 0}", (st, d, e))
    st, d, e = await B.gapi("POST", "/social/post/999999/vote", {"value": 1})
    check(st == 404 and (e or {}).get("code") == "SOCIAL_POST_NOT_FOUND", "[183] a vote on a missing post: SOCIAL_POST_NOT_FOUND", (st, d, e))
    st, d, e = await B.gapi("POST", "/social/post/%d/comment" % pid, {"text": "nice"})
    check(st == 200 and d == {} and e is None, "[184] POST /social/post/{id}/comment -> {}", (st, d, e))
    st, d, e = await C.gapi("GET", "/social/post/%d" % pid)
    check(st == 200 and len(d.get("comments", [])) == 1, "[181] the comment shows in the post's comments", (st, d))

    # ---- studios (24 routes)
    st, d, e = await A.gapi("DELETE", "/studios/%d" % tmp_id)
    check(st == 200 and e is None, "[197] DELETE /studios/{id} -> data", (st, e))
    st, d, e = await A.gapi("POST", "/studios", {"name": "GtNew" + stamp})
    check(st == 200 and (d or {}).get("id", 0) > 0 and d.get("role") == 1, "[194] POST /studios -> {id, name, role: 1}", (st, d, e))
    st, d, e = await A.gapi("GET", "/studios/mine")
    check(st == 200 and any(x.get("id") == sid and x.get("role") == 1 for x in d or []), "[195] GET /studios/mine: the owner's rows", (st, d, e))
    st, d, e = await A.gapi("GET", "/studios/%d" % sid)
    check(st == 200 and d.get("id") == sid and isinstance(d.get("members"), list) and isinstance(d.get("invites"), list), "[196] GET /studios/{id}: members and invites lists", (st, e))
    st, inv_c, e = await A.gapi("POST", "/studios/%d/members" % sid, {"player_name": C.name})
    inv_c1 = (inv_c or {}).get("invite_id", 0)
    check(st == 200 and inv_c1 > 0 and (inv_c or {}).get("name") == C.name and (inv_c or {}).get("expires_at"),
          "[198] POST /studios/{id}/members -> {invite_id, name, expires_at}", (st, inv_c, e))
    st, d, e = await A.gapi("DELETE", "/studios/%d/invites/%d" % (sid, inv_c1))
    check(st == 200 and e is None, "[199] DELETE /studios/{id}/invites/{invite} -> data", (st, e))
    st, inv_b, e = await A.gapi("POST", "/studios/%d/members" % sid, {"player_name": B.name})
    inv_b_id = (inv_b or {}).get("invite_id", 0)
    st, d, e = await B.gapi("GET", "/studios/invites")
    check(st == 200 and any(x.get("invite_id") == inv_b_id and x.get("studio_id") == sid for x in d or []), "[200] GET /studios/invites: the invitee's rows", (st, d, e))
    st, d, e = await B.gapi("POST", "/studios/invites/%d/accept" % inv_b_id)
    check(st == 200 and (d or {}).get("accepted") is True and (d or {}).get("studio_id") == sid, "[201] POST /studios/invites/{id}/accept -> {accepted, studio_id}", (st, d, e))
    st, inv_c, e = await A.gapi("POST", "/studios/%d/members" % sid, {"player_name": C.name})
    st, d, e = await C.gapi("POST", "/studios/invites/%d/decline" % (inv_c or {}).get("invite_id", 0))
    check(st == 200 and e is None, "[202] POST /studios/invites/{id}/decline -> data", (st, e))
    st, d, e = await A.gapi("POST", "/studios/%d/transfer" % s2_id, {"player_name": D.name})
    check(st == 200 and (d or {}).get("owner") == D.name, "[203] POST /studios/{id}/transfer -> {owner}", (st, d, e))
    st, d, e = await A.gapi("GET", "/studios/%d/monetization" % sid)
    check(st == 200 and isinstance(d, dict) and isinstance(d.get("eligible"), bool) and isinstance(d.get("requirements"), list),
          "[204] GET monetization: eligible, can_apply, requirements", (st, d, e))
    st, d, e = await A.gapi("POST", "/studios/%d/monetization" % sid, {"description": "A probe shop.", "map_ids": []})
    check(st >= 400 and err_ok(e), "[205] POST monetization by an ineligible account: a refusal with error.message", (st, d, e))
    st, d, e = await A.gapi("DELETE", "/studios/%d/monetization" % sid)
    check(st == 200 or err_ok(e), "[206] DELETE monetization: data, or a refusal with error.message", (st, d, e))
    st, d, e = await A.gapi("DELETE", "/studios/%d/members/%s" % (sid, B.name))
    check(st == 200 and e is None, "[207] DELETE /studios/{id}/members/{name} -> data", (st, e))
    st, d, e = await A.gapi("POST", "/studios/%d/maps" % sid, {"map_id": mid})
    check(st == 200 and e is None, "[208] POST /studios/{id}/maps -> data", (st, e))
    st, d, e = await A.gapi("DELETE", "/studios/%d/maps/%d" % (sid, mid))
    check(st == 200 and e is None, "[209] DELETE /studios/{id}/maps/{map} -> data", (st, e))
    st, d, e = await A.gapi("POST", "/studios/%d/currencies" % sid, {"name": "Gold Coins", "symbol": "GLD", "decimals": 0})
    cid = (d or {}).get("currency_id", 0)
    check(st == 200 and cid > 0, "[210] POST /studios/{id}/currencies -> {currency_id}", (st, d, e))
    st, d, e = await A.gapi("GET", "/studios/%d/analytics" % sid)
    check(st == 200 and (d or {}).get("kr_earned") == 0 and isinstance(d.get("currencies"), list), "[211] GET analytics: kr_earned and currencies", (st, e))
    st, d, e = await A.gapi("GET", "/studios/%d/skins" % sid)
    check(st == 200 and d == [] and e is None, "[212] GET /studios/{id}/skins: the empty list (no custom skins)", (st, d, e))
    st, d, e = await A.gapi("POST", "/studios/%d/skins/import" % sid, {"custom_skin_id": 1})
    check(st >= 400 and err_ok(e), "[213] POST skins/import: a refusal with error.message (no custom skins)", (st, e))
    st, d, e = await A.gapi("POST", "/studios/%d/skins/1/unpublish" % sid)
    check(st >= 400 and err_ok(e), "[214] POST skins/{sid}/unpublish: a refusal with error.message", (st, e))
    st, d, e = await A.gapi("POST", "/studios/%d/skins/1/publish" % sid)
    check(st >= 400 and err_ok(e), "[215] POST skins/{sid}/publish: a refusal with error.message", (st, e))
    st, d, e = await A.gapi("POST", "/studios/%d/currencies/%d/unpublish" % (sid, cid))
    check(st == 200 and e is None, "[216] POST currencies/{cid}/unpublish -> data", (st, e))
    st, d, e = await A.gapi("POST", "/studios/%d/currencies/%d/publish" % (sid, cid))
    check(st == 200 and e is None, "[217] POST currencies/{cid}/publish -> data", (st, e))

    for s in (guest, A, B, C, D):
        await s.close()


def main():
    (port,) = free_ports(1)
    log = open(os.path.join(WORK, "brain.log"), "w")
    cfg = os.path.join(WORK, "brain.json")
    with open(cfg, "w") as f:
        json.dump({"database": {"driver": "sqlite", "path": DB, "migrations_dir": os.path.join(ROOT, "services", "db", "migrations")},
                   "brain": {"host": "127.0.0.1", "port": port, "federation": False, "internal_secret": "gapi-%d" % os.getpid()},
                   "accounts": {"free_premium": False, "starting_kr": 0, "login_throttle_s": 0, "password_iterations": 1000,
                                "skins_file": os.path.join(ROOT, "assets", "skins.json")}}, f)
    brain = subprocess.Popen([BRAIN, "--config", cfg, "--migrate"], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
    try:
        for _ in range(200):
            try:
                with open(os.path.join(WORK, "brain.log")) as f:
                    if re.search(r"listening on http://127\.0\.0\.1:%d" % port, f.read()):
                        break
            except OSError:
                pass
            if brain.poll() is not None:
                break
            time.sleep(0.1)
        asyncio.run(scenario(port))
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
    print("gapi tunnel test: " + ("OK" if not FAILS else "%d failure(s), files in %s" % (len(FAILS), WORK)))
    if not FAILS and not os.environ.get("KN_KEEP"):
        shutil.rmtree(WORK, ignore_errors=True)
    sys.exit(1 if FAILS else 0)


main()
