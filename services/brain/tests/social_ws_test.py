#!/usr/bin/env python3
"""End-to-end test of the brain's accounts + social socket (svc-accounts).

Starts kn_brain against an isolated PostgreSQL schema, then drives the social
WebSocket like Krunker v3.1.1's social page / menu would (msgpack frames with the
2-byte trailer, and JSON text frames), plus the internal game-server ops over
/mm/internal/ws ("acct" frames).

usage: social_ws_test.py <kn_brain> <repo_root>
env:   KN_TEST_DB_NAME (default krunknative_svcacc), KN_TEST_SCHEMA (default
       kn_test_accounts_<pid>, dropped afterwards), config/config.json for host/user/password.
       KN_TEST_DB_DRIVER=sqlite|mysql runs on that store driver instead (db_scratch.py:
       a temp sqlite file, or a scratch database on KN_TEST_MYSQL_HOST/PORT/USER/PASSWORD).
Exit 77 (CTest skip) when the database or python websockets is unavailable.
"""
import asyncio
import json
import os
import re
import socket
import struct
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
import base64
import hashlib
import hmac

try:
    import websockets
except ImportError:  # pragma: no cover
    print("python websockets missing: skip")
    sys.exit(77)

# ---------------------------------------------------------------- tiny msgpack (independent of knsvc)


def mp_pack(v, out=None):
    out = bytearray() if out is None else out
    if v is None:
        out.append(0xC0)
    elif v is True:
        out.append(0xC3)
    elif v is False:
        out.append(0xC2)
    elif isinstance(v, int):
        if 0 <= v < 128:
            out.append(v)
        elif -32 <= v < 0:
            out.append(v & 0xFF)
        elif 0 <= v < 1 << 32:
            out += b"\xce" + struct.pack(">I", v)
        else:
            out += b"\xd3" + struct.pack(">q", v)
    elif isinstance(v, float):
        out += b"\xcb" + struct.pack(">d", v)
    elif isinstance(v, str):
        b = v.encode()
        if len(b) < 32:
            out.append(0xA0 | len(b))
        else:
            out += b"\xdb" + struct.pack(">I", len(b))
        out += b
    elif isinstance(v, (list, tuple)):
        out += b"\xdd" + struct.pack(">I", len(v))
        for x in v:
            mp_pack(x, out)
    elif isinstance(v, dict):
        out += b"\xdf" + struct.pack(">I", len(v))
        for k, x in v.items():
            mp_pack(str(k), out)
            mp_pack(x, out)
    else:
        raise TypeError(type(v))
    return out


def mp_unpack(b, i=0):
    t = b[i]
    i += 1
    if t < 0x80:
        return t, i
    if t >= 0xE0:
        return t - 256, i
    if 0x80 <= t <= 0x8F:
        return _map(b, i, t & 15)
    if 0x90 <= t <= 0x9F:
        return _arr(b, i, t & 15)
    if 0xA0 <= t <= 0xBF:
        n = t & 31
        return b[i:i + n].decode(), i + n
    if t == 0xC0:
        return None, i
    if t == 0xC2:
        return False, i
    if t == 0xC3:
        return True, i
    fmt = {0xCC: ">B", 0xCD: ">H", 0xCE: ">I", 0xCF: ">Q", 0xD0: ">b", 0xD1: ">h", 0xD2: ">i", 0xD3: ">q",
           0xCA: ">f", 0xCB: ">d"}
    if t in fmt:
        n = struct.calcsize(fmt[t])
        return struct.unpack(fmt[t], b[i:i + n])[0], i + n
    if t in (0xD9, 0xDA, 0xDB, 0xC4, 0xC5, 0xC6):
        ln = {0xD9: 1, 0xDA: 2, 0xDB: 4, 0xC4: 1, 0xC5: 2, 0xC6: 4}[t]
        n = int.from_bytes(b[i:i + ln], "big")
        i += ln
        raw = b[i:i + n]
        return (raw.decode() if t in (0xD9, 0xDA, 0xDB) else bytes(raw)), i + n
    if t in (0xDC, 0xDD):
        ln = 2 if t == 0xDC else 4
        return _arr(b, i + ln, int.from_bytes(b[i:i + ln], "big"))
    if t in (0xDE, 0xDF):
        ln = 2 if t == 0xDE else 4
        return _map(b, i + ln, int.from_bytes(b[i:i + ln], "big"))
    raise ValueError("msgpack type 0x%x" % t)


def _arr(b, i, n):
    out = []
    for _ in range(n):
        v, i = mp_unpack(b, i)
        out.append(v)
    return out, i


def _map(b, i, n):
    out = {}
    for _ in range(n):
        k, i = mp_unpack(b, i)
        v, i = mp_unpack(b, i)
        out[k] = v
    return out, i


# ---------------------------------------------------------------- client



def totp(key_b32, offset=0):
    """RFC 6238 code of the current 30 s step + offset (SHA-1, 6 digits)."""
    key = base64.b32decode(key_b32 + "=" * (-len(key_b32) % 8))
    mac = hmac.new(key, struct.pack(">Q", int(time.time() // 30) + offset), hashlib.sha1).digest()
    o = mac[-1] & 15
    return "%06d" % ((struct.unpack(">I", mac[o:o + 4])[0] & 0x7FFFFFFF) % 1000000)


class Social:
    """One social socket. msgpack (Krunker framing) unless json=True."""

    def __init__(self, url, json_mode=False):
        self.url = url + ("?enc=json" if json_mode else "")
        self.json = json_mode
        self.inbox = []
        self.ws = None

    async def __aenter__(self):
        self.ws = await websockets.connect(self.url, max_size=4 << 20)
        return self

    async def __aexit__(self, *a):
        await self.ws.close()

    async def send(self, *frame):
        if self.json:
            await self.ws.send(json.dumps(list(frame)))
        else:
            await self.ws.send(bytes(mp_pack(list(frame))) + b"\x0a\x05")  # client ahNum nibbles

    async def recv(self, label, timeout=10.0):
        deadline = time.time() + timeout
        while True:
            for i, f in enumerate(self.inbox):
                if f[0] == label:
                    return self.inbox.pop(i)
            left = deadline - time.time()
            if left <= 0:
                raise AssertionError("timeout waiting for %r (inbox %r)" % (label, [f[0] for f in self.inbox]))
            msg = await asyncio.wait_for(self.ws.recv(), left)
            if isinstance(msg, str):
                assert self.json, "text frame on a msgpack socket"
                self.inbox.append(json.loads(msg))
            else:
                assert not self.json, "binary frame on a JSON socket"
                assert msg[-2:] == b"\x00\x00", "missing Krunker trailer"
                v, n = mp_unpack(msg[:-2])
                assert n == len(msg) - 2
                self.inbox.append(v)

    async def call(self, reply, *frame, timeout=10.0):
        await self.send(*frame)
        return await self.recv(reply, timeout)


class Internal:
    """Game-server side of /mm/internal/ws: only the "acct" ops."""

    def __init__(self, url, secret):
        self.url, self.secret, self.rid = url, secret, 0

    async def __aenter__(self):
        self.ws = await websockets.connect(self.url, additional_headers={"Authorization": "Bearer " + self.secret})
        await self.ws.send(json.dumps({"type": "hello", "nodeId": "svcacc-test-node", "region": "local",
                                       "publicHost": "127.0.0.1", "publicPort": 1, "version": "test"}))
        while json.loads(await asyncio.wait_for(self.ws.recv(), 10)).get("type") != "welcome":
            pass
        return self

    async def __aexit__(self, *a):
        await self.ws.close()

    async def op(self, op, **args):
        self.rid += 1
        rid = self.rid
        await self.ws.send(json.dumps({"type": "acct", "rid": rid, "op": op, "args": args}))
        while True:
            msg = json.loads(await asyncio.wait_for(self.ws.recv(), 15))
            if msg.get("type") == "acct-r" and msg.get("rid") == rid:
                return msg.get("err"), msg.get("data")


# ---------------------------------------------------------------- the test

FAILS = []


ROOT = "."  # the repository root (argv[2]): assets/skins.json for the wheel checks


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what, flush=True)
    if not cond:
        FAILS.append(what)


async def scenario(base, internal_url, secret):
    ws_url = base + "/social/ws"
    stamp = str(int(time.time()) % 100000)
    alice, bob = "alice" + stamp, "bob_" + stamp

    async with Social(ws_url) as a:
        pi = await a.recv("pi")
        init = await a.recv("io-init")
        check(pi == ["pi", None] and isinstance(init[1], str) and len(init[1]) == 16, "greeting pi + io-init")
        await a.send("po")

        # register validation (v3.1.1 accounts.ts strings)
        r = await a.call("a", "a", 0, ["ab", "secret1", 0], "captcha")
        check(r[1] == "Username must be 3–16 characters.", "short username rejected: %r" % r[1])
        r = await a.call("a", "a", 0, ["bad name!", "secret1", 0], "captcha")
        check(r[1] == "Username may only contain letters, digits, and underscores.", "bad chars rejected")
        r = await a.call("a", "a", 0, [alice, "123", 0], "captcha")
        check(r[1] == "Password must be ≥ 6 characters.", "short password rejected: %r" % r[1])

        r = await a.call("a", "a", 0, [alice, "hunter22", 0], "captcha")
        check(r[1] is None and isinstance(r[2], int) and r[3] == alice, "register ok: %r" % r[1:4])
        check(isinstance(r[4], list) and len(r[4]) == 40, "data is 40-slot Account.getData()")
        check(isinstance(r[5], str) and len(r[5]) == 32, "32-hex session token")
        check(isinstance(r[6], dict) and r[6].get("level") == 1, "info object with level")
        alice_id, alice_token = r[2], r[5]

        r = await a.call("a", "a", 0, [alice.upper(), "hunter22", 0], "captcha")
        check(r[1] == "Username already taken.", "duplicate name (case-insensitive) rejected")

    async with Social(ws_url) as a:
        r = await a.call("a", "a", 1, [alice, "wrongpass", None], "captcha")
        check(r[1] == "Invalid login.", "wrong password: %r" % r[1])
        r = await a.call("a", "a", 1, [alice, "hunter22", None], "captcha")
        check(r[1] == "Please Try Later", "per (name, ip) throttle after a failure: %r" % r[1])
        await asyncio.sleep(1.5)  # accounts.login_throttle_s = 1 in the test config
        r = await a.call("a", "a", 1, [alice, "hunter22", None], "captcha")
        check(r[1] is None and r[2] == alice_id, "password login ok")
        r = await a.call("a", "a", 1, [alice, None, alice_token])
        check(r[1] is None and r[5] == alice_token, "token resume keeps the token")
        r = await a.call("a", "a", 1, ["someone_else", None, alice_token])
        check(r[1] == "Invalid login.", "token with another name refused")

    # JSON text client (KrunkNative native/web)
    async with Social(ws_url, json_mode=True) as b:
        await b.recv("io-init")
        r = await b.call("a", "a", 0, [bob, "pass_word", None])
        check(r[1] is None and r[3] == bob, "JSON register ok")
        bob_id, bob_token = r[2], r[5]
        gmc = await b.recv("gmc")
        check(gmc[1] == 0, "unread mail count pushed after login (JSON clients)")

        r = await b.call("0", "r", "profile", alice, bob_id, bob, None, 0)
        row = r[3]
        check(r[1] == "profile" and r[2] == alice and row and row["player_name"] == alice, "profile row")
        check(isinstance(row["player_stats"], str), "player_stats is a JSON string (JSON.parse in io_0)")
        check(r[6] == [{"f": 0}], "follow state not following")
        await b.send("f", alice, 1)  # no reply; the socket runs requests in order
        r = await b.call("0", "r", "profile", alice, bob_id, bob, None, 0)
        check(r[6] == [{"f": 1}] and r[3]["player_followed"] == 1, "follow counted")
        r = await b.call("fwsr", "gfs", alice, alice_id, 1, "c")
        check(len(r[1]) == 1 and r[1][0]["friendname"] == bob and r[2] == alice, "followers list")
        r = await b.call("fwr", "gfg", bob, bob_id, 1, "c")
        check(len(r[1]) == 1 and r[1][0]["friendname"] == alice, "following list")

        r = await b.call("spR", "sp", alice[:4], "c")
        check(r[1] and any(x["player_name"] == alice for x in r[1]), "player search")

        # feed
        r = await b.call("psr", "ps", {"txt": "<b>hello</b> krunker"})
        check(r[1] == 1, "post created")
        r = await b.call("0", "r", "feed", None, bob_id, bob, None, 0)
        posts = r[3]
        check(r[2] is None and posts and posts[0]["txt"] == "&lt;b&gt;hello&lt;/b&gt; krunker", "feed post html-escaped")
        pid = posts[0]["pid"]
        r = await b.call("psr", "com", pid, {"txt": "first"})
        check(r[1] == 1 and r[3] == 2, "comment ack")
        await b.send("lps", pid, 1)
        r = await b.call("psr", "gps", pid)
        check(r[1]["p"]["vot"] == 1 and r[1]["p"]["dlk"] == 1 and r[1]["c"][0]["txt"] == "first", "open post")
        r = await b.call("uf", "guf", bob_id)
        check(len(r[1]) == 1, "profile activity tab")

        # settings sync, display name
        r = await b.call("pRes", "uploadS", '{"fov":90}')
        check(r[1] == "SUCCESS", "settings uploaded")
        r = await b.call("loadS", "getS")
        check(r[1] == '{"fov":90}', "settings downloaded")
        r = await b.call("nmeR", "nme", "Bob Display")
        check(r[1] is None and r[2] == "Bob Display", "display name set")
        r = await b.call("nmeR", "nme", "x")
        check(r[1] == "Display name must be 3–16 characters.", "short display name refused")

        # clans (menu window protocol)
        r = await b.call("cln", "cln", 1, "BOB", None)
        check(r[1] is None and r[2]["clan_name"] == "BOB" and r[3] == 1, "clan created")
        async with Social(ws_url) as a:
            await a.call("a", "a", 1, [alice, None, alice_token])
            r = await a.call("cln", "cln", "bob")
            check(r[1] == "Request Sent" and r[2] is True, "social page join request")
            r = await b.call("cln", "cln", 3, 1)
            check(r[2]["requests"][0]["player_name"] == alice, "request visible to leader")
            await b.send("clnR", alice_id, 1)
            r = await b.call("cln", "cln", 3, 1)
            check(len(r[2]["members"]) == 2, "request accepted")
            r = await a.call("0", "r", "clan", "BOB", alice_id, alice, None, 0)
            check(r[3]["clan_name"] == "BOB" and len(r[3]["members"]) == 2, "clan page")

            # chat
            r = await a.call("chf", "ict", bob_id, "hey bob")
            check(r[1] is True, "chat sent")
            push = await b.recv("chtb")
            check(push[1][0]["fid"] == alice_id, "chat push to the recipient")
            r = await b.call("chtb", "chtb")
            check(r[1][0]["fna"] == alice and r[1][0]["cht"] == 1, "chat blobs with unread")
            r = await b.call("chtl", "ctd", alice_id)
            check(r[1][0]["txt"] == "hey bob", "chat thread")

    # game server side (internal WS); alice's JSON menu socket sees the live update
    async with Internal(internal_url, secret) as g, Social(ws_url, json_mode=True) as menu:
        await menu.call("a", "a", 1, [alice, None, alice_token])
        err, acc = await g.op("token", token=alice_token)
        check(err is None and acc["id"] == alice_id and acc["level"] == 1, "internal token lookup: %r" % err)
        err, acc = await g.op("stats", id=alice_id, match="m-%s" % stamp, kills=10, deaths=4, score=50000,
                              won=1, games=1, time_ms=120000, kr=40, **{"class": 3})
        check(err is None and acc["score"] == 50000 and acc["level"] == 6 and acc["funds"] == 1040,
              "stats commit: %r %r" % (err, acc and (acc.get("score"), acc.get("level"), acc.get("funds"))))
        err, acc = await g.op("stats", id=alice_id, match="m-%s" % stamp, kills=10, score=50000, kr=40)
        check(err is None and acc["duplicate"] == 1 and acc["score"] == 50000, "stats commit idempotent")
        check(acc["data"][16].get("c3") == 50000, "per-class xp in stats")
        ua = await menu.recv("ua")
        check(ua[1][6] == 50000 and ua[2]["level"] == 6 and ua[2]["funds"] == 1040, "ua push after the commit")
        err, data = await g.op("give_skin", id=alice_id, skin=5)
        check(err is None and data["count"] == 1, "give skin")
        err, data = await g.op("ban_check", ip="203.0.113.9")
        check(err is None and data["banned"] == 0, "ban check")
        err, data = await g.op("token", token="0" * 32)
        check(err == "Invalid login.", "bad token refused")

    async with Social(ws_url) as a:
        await a.call("a", "a", 1, [alice, None, alice_token])
        r = await a.call("0", "r", "leaders", "player_score", alice_id, alice, None, 0)
        check(r[2] == "player_score" and r[3][0]["player_name"] == alice, "score leaderboard")
        r = await a.call("0", "r", "leaders", "player_clan", alice_id, alice, None, 0)
        check(r[3] and r[3][0]["clan_name"] == "BOB" and r[3][0]["clan_score"] == 50000, "clan leaderboard")
        # KR gift (fee 0 in the test config) -> bob gets mail
        r = await a.call("gir", "kr", bob, 100, "gg", "c")
        check(r[1]["funds"] == 940, "gift sent: %r" % r[1])
        # market: list the skin, unlisted by nobody, bob buys it
        r = await a.call("0", "r", "market", "inventory", alice_id, alice, None, 0)
        check(r[3] == [{"i": 5, "s": 5, "c": 1}], "inventory page: %r" % r[3])
        r = await a.call("lir", "li", 5, 5, 77, 500)
        check(r[1]["cnt"] == 0 and r[1]["funds"] == 915 and r[2] == 77, "listed (5%% fee): %r" % r[1])
        async with Social(ws_url) as b:
            await b.call("a", "a", 1, [bob, None, bob_token])
            r = await b.call("kml", "kml", 0)
            check(r[2] and r[2][0]["km_subject"] == "KR Gift", "gift mail")
            r = await b.call("0", "r", "market", "market", bob_id, bob, None, 0)
            listing = next(x for x in r[3] if x["i"] == 5)
            r = await b.call("br", "pi", listing["m"], 3)
            check(r[1] and r[1]["f"] == 600 and r[1]["i"] == 5 and r[2] == 3, "market purchase: %r" % r[1:])
            r = await b.call("br", "pi", listing["m"], 3)
            check(r[1] is None, "sold listing unavailable")
            r = await b.call("0", "r", "market", "inventory", bob_id, bob, None, 0)
            check(r[3] == [{"i": 5, "s": 5, "c": 1}], "bought skin in inventory")
            r = await b.call("qsr", "qsi", 0, 5, 9, 2)  # v7.1.4: mode KR, skin, uid, all
            check(r[1] and r[1]["c"] == 0 and r[1]["i"] == 9, "quick sell: %r" % r[1])
        # alice trades with herself is refused
        r = await a.call("po", "ctr", alice_id, [1], [], "", "c")
        check(r[2] == "Cant trade with yourself", "self trade refused: %r" % r)
        await a.send("a", -1)
        r = await a.call("po", "kr", bob, 1, "", "c")
        check(r[2] == "Must be logged in", "logout drops the account")

    async with Internal(internal_url, secret) as g:
        err, _ = await g.op("token", token=alice_token)
        check(err == "Invalid login.", "logout revoked the session token")
        err, data = await g.op("password", id=bob_id, old="pass_word", new="new_pass1")
        check(err is None and len(data["token"]) == 32, "password change")
        err, _ = await g.op("token", token=bob_token)
        check(err == "Invalid login.", "password change revokes other sessions")
        err, acc = await g.op("login", name=bob, **{"pass": "new_pass1"})
        check(err is None and acc["alias"] == "Bob Display", "internal login with the new password")
        bob_token = acc["token"]

    # publishing maps (social.js uploadGameAsset "uploadM") + the hub catalogue
    http_base = base.replace("ws://", "http://")
    async with Social(ws_url, json_mode=True) as a:
        await a.call("a", "a", 1, [alice, "hunter22", None])
        mapname = "Arena_" + stamp
        mapjson = json.dumps({"name": mapname, "objects": [{"p": [0, 0, 0], "s": [10, 1, 10]}], "spawns": []})
        r = await a.call("pubMR", "uploadM", mapjson, None, "my <i>map</i>", "", 2)
        check(r[1] == "Map Published!", "map published: %r" % r)
        r = await a.call("pubMR", "uploadM", mapjson, None, "v2", "", 2)
        check(r[1] == "Map Updated!", "republish updates: %r" % r)
        r = await a.call("upMp", "uploadM", json.dumps({"name": "Burg", "objects": []}), None, "", "", 0)
        check(r[1] == "Map name taken", "catalogue names reserved")
        # catalogue (shared/include/map_catalog.h, since 83d680d): v7.1.4's 32 official maps at their
        # v7 indices (0 = Burg), then 12 v3.1.1 community maps, its 15 official maps as 311_* ("Burg
        # (3.1.1)") and its 9 old_* maps as 152_* ("Burg (1.5.2)") = 68. Popular / New list the 32
        # officials first, then the rest in the v3.1.1 tab order; 42 rows per page (TAB_PAGE_SIZE)
        page = json.loads(urllib.request.urlopen(http_base + "/api/maps?index=2&pos=0").read())
        page1 = json.loads(urllib.request.urlopen(http_base + "/api/maps?index=2&pos=1").read())
        rows = page["data"] + page1["data"]
        check(rows[0]["map_name"] == mapname and rows[0]["creatorname"] == alice and rows[1]["map_name"] == "Burg"
              and len(page["data"]) == 42 and len(rows) == 1 + 68,
              "/api/maps New tab: published map first + 68 catalogue maps (%d)" % len(rows))
        page = json.loads(urllib.request.urlopen(http_base + "/api/maps?index=0&pos=0").read())
        check(page["data"][0]["map_name"] == "Burg" and page["data"][32]["map_name"] == "Soul Sanctum (3.1.1)"
              and page["error"] is None, "/api/maps Popular = v7 official maps, then the v3.1.1 order")
        found = json.loads(urllib.request.urlopen(http_base + "/api/search?type=map&val=arena_" + stamp).read())
        check(len(found["data"]) == 1, "/api/search finds the published map")
        data = urllib.request.urlopen(http_base + "/api/map-data?name=" + mapname).read()
        check(json.loads(data)["name"] == mapname, "/api/map-data serves the map JSON")
        try:
            urllib.request.urlopen(http_base + "/api/map-data?name=nope_" + stamp)
            check(False, "/api/map-data unknown map 404")
        except urllib.error.HTTPError as e:
            check(e.code == 404 and json.loads(e.read())["error"] == "not_found", "/api/map-data unknown map 404")
        r = await a.call("edMp", "edMp", mapname)
        check(json.loads(r[1])["name"] == mapname, "edMp returns the map for the editor")
        r = await a.call("0", "r", "map", mapname, None, None, None, 0)
        check(r[3]["map_creator"] == alice and r[3]["map_description"] == "v2", "map page")
        r = await a.call("0", "r", "profile", alice, None, None, None, 0)
        check(len(r[4]) == 1 and r[4][0]["map_name"] == mapname, "profile games tab")
        r = await a.call("po", "delC", r[4][0]["map_id"], 0)
        check(r[1] == "SUCCESS", "creator deletes the map")

    # store: v7.1.4 prize wheels (single, 10x multi, tickets), spin history, Free KR wheel,
    # daily reward; ranked elo deltas
    carol = "carol" + stamp
    with open(os.path.join(ROOT, "assets", "skins.json")) as fp:
        skins = json.load(fp)
    season = 7  # accounts.current_season (default): store wheels roll this season's items

    def slice_of(rarities, roll):  # the wheel slice a roll in [0, 100) lands on
        for i, share in enumerate(rarities):
            if roll < share:
                return i
            roll -= share
        return max(i for i, share in enumerate(rarities) if share > 0)

    def check_rolls(rolls, items, rarities, big_rar, what):
        ok = len(rolls) == len(items) and all(0 <= n < 100 for n in rolls)
        for n, i in zip(rolls, items):
            k, r = skins[i], slice_of(rarities, n)
            ok = ok and (k.get("rarity", 0) == r or (big_rar and r >= big_rar and k.get("rarity", 0) >= big_rar))
            ok = ok and (k.get("mSeas") or k.get("seas", 1) == season) and not k.get("noSpin") and not k.get("limT")
        check(ok, what + ": every item has the rarity of its roll's slice, this season's pool: %r" % list(zip(rolls, items)))

    starter, heroic = [75, 22, 3, 0, 0, 0], [0, 48, 35, 14, 2.49, 0.5, 0.01]
    async with Social(ws_url, json_mode=True) as c:
        r = await c.call("a", "a", 0, [carol, "carol_pw1", None])
        carol_id = r[2]
        r = await c.call("spin", "spin", 0, None, False)
        check(r[1] is None and isinstance(r[3], int) and r[4] == 950 and r[5] is None, "Starter wheel spin: %r" % r)
        check_rolls([r[2]], [r[3]], starter, 0, "Starter spin")
        won = [r[3]]
        r = await c.call("spin", "spin", 0, None, True)
        check(r[1] is None and len(r[2]) == 10 and len(r[3]) == 10 and r[4] == 450, "Starter 10x spin costs 10 x 50 KR: %r" % r)
        check_rolls(r[2], r[3], starter, 0, "Starter 10x")
        won += r[3]
        r = await c.call("0", "r", "market", "inventory", carol_id, carol, None, 0)
        inv = {x["i"]: x.get("c", 1) for x in r[3]}
        check(all(i in inv for i in won), "won skins in the inventory")
        r = await c.call("spin", "spin", 2, None, True)
        check(r[1] == "Not enough KR", "Heroic 10x needs 5000 KR: %r" % r)
        for wheel, err in ((3, "Wheel unavailable"), (4, "Wheel unavailable"), (9, "Invalid spin")):
            r = await c.call("spin", "spin", wheel, None, False)
            check(r[1] == err, "wheel %d refused (%s): %r" % (wheel, err, r))
        r = await c.call("spin", "spin", None, 5, False)
        check(r[1] == "Invalid spin", "only TICKET items redeem: %r" % r)
        async with Internal(internal_url, secret) as g:
            err, _ = await g.op("give_skin", id=carol_id, skin=6426, source="test")  # Season 7 Ultra Ticket
            check(err is None, "give an Ultra ticket")
            for _ in range(12):  # 6000 KR (one reward is capped at 500)
                err, _ = await g.op("rewards", items=[[carol_id, 500]])
        r = await c.call("spin", "spin", None, 6426, False)
        check(r[1] is None and isinstance(r[3], int) and r[4] == 6450 and r[5] == 6426, "Ultra ticket redeemed, no KR: %r" % r)
        check_rolls([r[2]], [r[3]], [0, 28, 53, 13.5, 4.46, 1, 0.04], 5, "Ultra ticket")
        r = await c.call("spin", "spin", None, 6426, False)
        check(r[1] == "Item not owned", "a ticket redeems once: %r" % r)
        r = await c.call("spin", "spin", 2, None, True)
        check(r[1] is None and len(r[3]) == 10 and r[4] == 1450, "Heroic 10x: %r" % r[1:])
        check_rolls(r[2], r[3], heroic, 5, "Heroic 10x")
        r = await c.call("spinH", "spinH", 0)
        h = r[1]
        check(isinstance(h, list) and len(h) == 22 and all(isinstance(x["dte"], (int, float)) and x["dte"] > 0 for x in h),
              "spin history: 22 won items: %r" % (h and h[:2]))
        check(sum(x["fds"] for x in h) == 50 * 11 + 500 * 10 and h[0]["fds"] == 500, "spin history KR spent per item")
        check(h[10]["fds"] == 0 and h[11]["fds"] == 50, "the ticket's item cost no KR: %r" % h[10])
        r = await c.call("spinH", "spinH", 20)
        check(len(r[1]) == 2, "spin history paging (offset 20)")
        r = await c.call("spinKR", "fkr")
        check(r[1] > 0 and 0 <= r[3] <= 3 and r[4] - 1450 == [5, 10, 20, 500][r[3]], "Free KR wheel: %r" % r)
        r = await c.call("spinKR", "fkr")
        check(r[5] == "Reward not ready yet", "Free KR once per rewardTime")
        r = await c.call("pRes", "clm")
        check(r[2] == "Must be level 5", "daily reward needs level 5: %r" % r)
    async with Internal(internal_url, secret) as g:
        err, data = await g.op("elo", id=carol_id, mode=2, delta=25)
        check(err is None and data["elo"] == 25, "elo delta")
        err, data = await g.op("elo", id=carol_id, mode=2, delta=-100)
        check(err is None and data["elo"] == 0, "elo delta clamps at 0")

    # v7.1.4 2FA: authenticator app (TOTP), index.js act2FA / sub2FA / twoFAres
    async with Social(ws_url, json_mode=True) as b:
        await b.call("a", "a", 1, [bob, None, bob_token])
        r = await b.call("c2fa", "a2FA")
        uri = r[3] or ""
        m = re.match(r"otpauth://totp/Krunker%20Box%20Office:(\w+)\?secret=([A-Z2-7]+)&issuer=Krunker%20Box%20Office", uri)
        check(r[1] == 0 and r[2] == "a2FAc" and m and m.group(1) == bob, "a2FA offers an otpauth URI: %r" % r[1:])
        key = m.group(2) if m else "AAAA"
        r = await b.call("c2fa", "a2FAc", totp(key, 5))
        check(r[1] == 0 and r[2] == "a2FAc" and r[5] == "Invalid Code", "wrong code asks again: %r" % r[1:])
        r = await b.call("c2fa", "a2FAc", totp(key, -1))
        check(r[1] == 2 and r[4] is True, "2FA enabled with the first code: %r" % r[1:])
        r = await b.call("c2fa", "a2FA")
        check(r[1] == 2 and r[4] is True, "a2FA when enabled only refreshes the state")
        async with Social(ws_url, json_mode=True) as b2:  # b stays logged in by token
            r = await b2.call("c2fa", "a", 1, [bob, "new_pass1", None], "c")
            check(r[1] == 0 and r[2] == "2fa", "password login asks for the code: %r" % r[1:])
            r = await b2.call("c2fa", "2fa", "12345x")
            check(r[1] == 0 and r[5] == "Invalid Code", "wrong code refused: %r" % r[1:])
            code = totp(key, 0)
            await b2.send("2fa", code)  # no (name, ip) throttle after a wrong code: the password was right
            r = await b2.recv("c2fa")
            check(r[1] == 1, "popup hidden after the right code")
            r = await b2.recv("a")
            check(r[1] is None and r[3] == bob, "2FA login completes")
            check(r[4][28] == 1, "has2FA slot set")
            await b2.send("a", -1)
            await b2.call("c2fa", "a", 1, [bob, "new_pass1", None], "c")
            r = await b2.call("c2fa", "2fa", code)
            check(r[1] == 0 and r[5] == "Invalid Code", "a used code is never accepted again")
        async with Internal(internal_url, secret) as g:
            err, _ = await g.op("login", name=bob, **{"pass": "new_pass1"})
            check(err == "2FA required.", "internal password login refused for 2FA accounts")
        r = await b.call("c2fa", "d2FA")
        check(r[1] == 0 and r[2] == "d2FAc" and r[3] is None, "d2FA asks for a code")
        r = await b.call("c2fa", "d2FAc", totp(key, 1))
        check(r[1] == 2 and r[4] is False, "2FA disabled with a code: %r" % r[1:])
        await b.send("a", -1)
        r = await b.call("a", "a", 1, [bob, "new_pass1", None], "c")
        check(r[1] is None and r[4][28] == 0, "password login without 2FA again")


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    global ROOT
    brain, root = sys.argv[1], os.path.abspath(sys.argv[2])
    ROOT = root
    with open(os.path.join(root, "config", "config.json")) as fp:
        db = json.load(fp).get("database", {})
    dbname = os.environ.get("KN_TEST_DB_NAME", "krunknative_svcacc")
    # one schema per run: concurrent runs (several build dirs / agents) never share tables
    schema = os.environ.get("KN_TEST_SCHEMA", "kn_test_accounts_%d" % os.getpid())
    driver = os.environ.get("KN_TEST_DB_DRIVER", "postgres")
    if driver == "postgres":
        env = dict(os.environ, PGPASSWORD=str(db.get("password", "")))
        psql = ["psql", "-h", str(db.get("host", "127.0.0.1")), "-p", str(db.get("port", 5432)), "-U",
                str(db.get("user", "krunknative")), "-d", dbname, "-v", "ON_ERROR_STOP=1", "-qAt"]
        try:
            subprocess.run(psql + ["-c", "DROP SCHEMA IF EXISTS %s CASCADE; CREATE SCHEMA %s" % (schema, schema)],
                           env=env, check=True, capture_output=True, timeout=30)
        except (OSError, subprocess.SubprocessError) as e:
            print("PostgreSQL unavailable (%s): skip" % e)
            return 77
        dbcfg = dict(db, name=dbname, search_path=schema)
        drop_db = lambda: subprocess.run(psql + ["-c", "DROP SCHEMA IF EXISTS %s CASCADE" % schema], env=env,
                                         capture_output=True)
    else:
        scratch = [sys.executable, "-I", os.path.join(root, "services", "brain", "tests", "db_scratch.py"), root]
        r = subprocess.run(scratch + ["create", driver, schema], capture_output=True)
        if r.returncode:
            print("%s unavailable (%s): skip" % (driver, r.stderr.decode(errors="replace").strip()))
            return r.returncode
        dbcfg = json.loads(r.stdout)
        drop_db = lambda: subprocess.run(scratch + ["drop", driver, schema], capture_output=True)

    secret = "test-secret-" + str(os.getpid())
    cfg = {
        "database": dict(dbcfg, migrations_dir=os.path.join(root, "services", "db", "migrations")),
        "brain": {"host": "127.0.0.1", "port": 0, "internal_secret": secret, "federation": False},
        "accounts": {"password_iterations": 10000, "login_throttle_s": 1, "starting_kr": 1000,
                     "gift_fee_percent": 0, "gift_min_level": 0, "market_fee_bypass": 100,
                     "market_fee_percent": 5, "market_min_level": 0, "sale_grace_s": 0, "trade_min_level": 0, "skins_file": os.path.join(root, "assets", "skins.json"),
                     "leaderboard_cache_s": 0},
    }
    tmp = tempfile.mkdtemp(prefix="kn-svcacc-test-")
    cfg_path = os.path.join(tmp, "config.json")
    with open(cfg_path, "w") as fp:
        json.dump(cfg, fp)
    log_path = os.path.join(tmp, "brain.log")
    log = open(log_path, "w")
    proc = subprocess.Popen([brain, "--config", cfg_path, "--migrate"], stdout=log, stderr=subprocess.STDOUT,
                            env=dict(os.environ, KN_LOG_LEVEL="debug"), cwd=root)
    rc = 1
    try:
        # brain.port 0: the kernel picks a free port, the brain logs it (no bind/close race).
        # 60 s: --migrate runs every migration first (MySQL DDL alone takes ~15 s on a cold server)
        port = None
        for _ in range(1200):
            if proc.poll() is not None:
                raise RuntimeError("kn_brain exited early")
            with open(log_path) as fp:
                m = re.search(r"listening on http://127\.0\.0\.1:(\d+) \(ws", fp.read())
            if m:
                port = int(m.group(1))
                break
            time.sleep(0.05)
        if not port:
            raise RuntimeError("kn_brain did not start listening")
        base = "ws://127.0.0.1:%d" % port
        asyncio.run(scenario(base, base + "/mm/internal/ws", secret))
        rc = 1 if FAILS else 0
    except Exception as e:  # noqa: BLE001
        print("FAIL exception: %r" % (e,))
        rc = 1
    finally:
        proc.terminate()
        try:
            proc.wait(10)
        except subprocess.TimeoutExpired:
            proc.kill()
        log.close()
        if rc:
            print("---- brain log (%s) ----" % log_path)
            with open(log_path) as fp:
                print(fp.read()[-6000:])
        drop_db()
        if not rc:
            for f in os.listdir(tmp):
                os.unlink(os.path.join(tmp, f))
            os.rmdir(tmp)
    print("%d failure(s)" % len(FAILS))
    return rc


if __name__ == "__main__":
    sys.exit(main())
