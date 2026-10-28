#!/usr/bin/env python3
"""Krunker v10.0.2 Studios backend: the /studios gapi routes over the social socket
(services/brain/studios.c, db_studios.c, 0330_studios), on a kn_brain with its own sqlite database
(never config/config.json).

  studios_test.py <kn_brain> <repo root>
  KN_TEST_DB_DRIVER=postgres: a scratch schema in krunknative_svcacc (config/config.json credentials,
  or the file KN_TEST_CONFIG names); mysql: a scratch database on KN_TEST_MYSQL_HOST/PORT/USER/PASSWORD
  (default 127.0.0.1:3306 root)

Covers every route v10's Studios window and studio view call: create (name rule, unique, 3 owned),
mine, get (members / invites / games / currencies, member-only), invites (send, list, cancel,
accept, decline, expiry, 20 seats), transfer, remove / leave, games (own published maps only, one
studio per game, the map's video id), currencies (rules, 20 in total, 5 published, publish / unpublish), analytics,
skins, the monetization application (requirements, apply, pending, withdraw, rejection wait) and
delete. 77 = skip (no kn_brain / websockets / database server).
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
WORK = tempfile.mkdtemp(prefix="kn-studios.", dir=os.path.expanduser("~/.cache"))
DB = os.path.join(WORK, "coordinator.db")
RID = itertools.count(1)
DRIVER = os.environ.get("KN_TEST_DB_DRIVER", "sqlite")
PG = {}
MY = {}


def check(cond, what, detail=""):
    print(("  ok   " if cond else "  FAIL ") + what + (("  -- " + str(detail)) if detail and not cond else ""), flush=True)
    if not cond:
        FAILS.append(what)


def lit(v):
    return str(int(v)) if isinstance(v, (int, bool)) else "'" + str(v).replace("'", "''") + "'"


def q(sql, *args):
    parts = sql.split("?")
    text = "".join(p + (lit(args[i]) if i < len(args) else "") for i, p in enumerate(parts))
    if DRIVER == "mysql":
        out = subprocess.run(MY["cmd"] + ["-N", "-B", "-e", text], env=MY["env"], capture_output=True, text=True, check=True).stdout
        return [tuple(None if c == "NULL" else int(c) if re.fullmatch(r"-?\d+", c) else c for c in line.split("\t"))
                for line in out.splitlines() if line]
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
        if not line or line == "SET" or line.startswith("INSERT ") or line.startswith("UPDATE ") or line.startswith("DELETE "):
            continue
        rows.append(tuple(None if c == "" else int(c) if re.fullmatch(r"-?\d+", c) else c for c in line.split("|")))
    return rows


def ts(days):
    """SQL for now + days (negative: the past)"""
    if DRIVER == "postgres":
        return "now() + interval '%d days'" % days
    if DRIVER == "mysql":
        return "NOW(3) + INTERVAL %d DAY" % days
    return "strftime('%%Y-%%m-%%d %%H:%%M:%%f', 'now', '%+d days')" % days


class Sock:
    def __init__(self, ws):
        self.ws = ws
        self.buf = []

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
        await self.send("a", 0, [name, "studio_pass_1", None])
        f = await self.wait(lambda f: f[0] == "a")
        assert f and f[1] is None, f
        self.id, self.name = f[2], name
        return f[2]

    async def gapi(self, method, path, body=None):
        rid = next(RID)
        await self.send("gapi", rid, method, path, body)
        f = await self.wait(lambda f: f[0] == "gapi" and f[1] == rid)
        return (f[2], (f[3] or {}).get("data"), (f[3] or {}).get("error")) if f else (0, None, None)

    async def close(self):
        await self.ws.close()


def code(err):
    return (err or {}).get("code")


async def scenario(port):
    stamp = str(int(time.time()) % 100000)
    anon = await Sock.open(port)
    st, data, err = await anon.gapi("GET", "/studios/mine")
    check(st == 200 and data == [] and err is None, "a guest gets the empty studio list (the Studios window's read)", (st, data, err))
    st, data, err = await anon.gapi("GET", "/studios/invites")
    check(st == 200 and data == [] and err is None, "a guest gets the empty invite list", (st, data, err))
    st, data, err = await anon.gapi("POST", "/studios", {"name": "GuestStudio"})
    check(st == 401 and code(err) == "UNAUTHORIZED", "a guest may not create a studio: 401 UNAUTHORIZED", (st, err))
    await anon.close()

    own, mem, out, ext = [await Sock.open(port) for _ in range(4)]
    for s, n in zip((own, mem, out, ext), ("stO", "stM", "stX", "stE")):
        await s.auth(n + stamp)
    m1 = q("INSERT INTO published_maps (map_name, creator_id, map_video) VALUES (?, ?, 'dQw4w9WgXcQ') RETURNING map_id", "StMapA" + stamp, own.id)[0][0]
    m2 = q("INSERT INTO published_maps (map_name, creator_id) VALUES (?, ?) RETURNING map_id", "StMapB" + stamp, mem.id)[0][0]

    # ---- create
    st, data, err = await own.gapi("POST", "/studios", {"name": "a b"})
    check(st == 400 and code(err) == "STUDIO_NAME_INVALID", "create: a name with a space is refused", err)
    name = "KnStudio_" + stamp
    st, data, err = await own.gapi("POST", "/studios", {"name": name})
    sid = (data or {}).get("id")
    check(st == 200 and isinstance(sid, int) and sid > 0 and data.get("name") == name, "create: {id, name}", (st, data, err))
    st, data, err = await mem.gapi("POST", "/studios", {"name": name.lower()})
    check(st == 409 and code(err) == "STUDIO_NAME_TAKEN", "create: the name is unique in any letter case", err)
    st, data, err = await own.gapi("GET", "/studios/mine")
    check(st == 200 and len(data) == 1 and data[0]["id"] == sid and data[0]["role"] == 1 and data[0]["member_count"] == 1
          and data[0]["owner"] == own.name, "mine: the owner's row (role 1, 1 member, owner name)", data)
    st, data, err = await out.gapi("GET", "/studios/%d" % sid)
    check(st == 404 and code(err) == "STUDIO_NOT_FOUND", "get: an outsider gets 404", err)
    st, data, err = await own.gapi("GET", "/studios/abc")
    check(st == 404, "get: a non-numeric id is 404", err)

    # ---- invites
    st, data, err = await own.gapi("POST", "/studios/%d/members" % sid, {"player_name": mem.name.upper()})
    inv = (data or {}).get("invite_id")
    check(st == 200 and inv and data.get("name") == mem.name and re.fullmatch(r"\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ", data.get("expires_at") or ""),
          "invite: by name in any case -> {invite_id, name, expires_at}", (st, data, err))
    days = q("SELECT %s FROM studio_invites WHERE id = ?" % {
        "postgres": "extract(epoch FROM expires_at - created_at) / 86400",
        "mysql": "TIMESTAMPDIFF(SECOND, created_at, expires_at) / 86400"}.get(DRIVER, "julianday(expires_at) - julianday(created_at)"), inv)
    check(days and abs(float(days[0][0]) - 14) < 0.01, "invite: expires after 14 days", days)
    st, data, err = await own.gapi("POST", "/studios/%d/members" % sid, {"player_name": mem.name})
    check(st == 409 and code(err) == "STUDIO_ALREADY_INVITED", "invite: twice is refused", err)
    st, data, err = await own.gapi("POST", "/studios/%d/members" % sid, {"player_name": "nobody_" + stamp})
    check(st == 404 and code(err) == "STUDIO_PLAYER_NOT_FOUND", "invite: an unknown player", err)
    st, data, err = await own.gapi("POST", "/studios/%d/members" % sid, {"player_name": own.name})
    check(st == 400 and code(err) == "STUDIO_INVITE_SELF", "invite: yourself", err)
    st, data, err = await mem.gapi("POST", "/studios/%d/members" % sid, {"player_name": out.name})
    check(st == 404, "invite: a non-member cannot invite", err)
    st, data, err = await mem.gapi("GET", "/studios/invites")
    check(st == 200 and len(data) == 1 and data[0]["invite_id"] == inv and data[0]["studio_id"] == sid and data[0]["studio_name"] == name
          and data[0]["invited_by"] == own.name and data[0]["expires_at"], "invites: the invitee's list", data)
    st, data, err = await own.gapi("POST", "/studios/%d/members" % sid, {"player_name": out.name})
    inv_out = (data or {}).get("invite_id")
    st, data, err = await own.gapi("GET", "/studios/%d" % sid)
    check(st == 200 and sorted(i["name"] for i in data["invites"]) == sorted([mem.name, out.name]) and all(i["invited_by"] == own.name for i in data["invites"]),
          "get: the pending invites with invited_by", data and data.get("invites"))
    st, data, err = await own.gapi("DELETE", "/studios/%d/invites/%d" % (sid, inv_out))
    check(st == 200, "cancel invite", err)
    st, data, err = await out.gapi("GET", "/studios/invites")
    check(st == 200 and data == [], "a cancelled invite is gone from the invitee's list", data)
    st, data, err = await own.gapi("DELETE", "/studios/%d/invites/%d" % (sid, inv_out))
    check(st == 404 and code(err) == "STUDIO_INVITE_NOT_FOUND", "cancel invite: twice is 404", err)
    st, data, err = await out.gapi("POST", "/studios/invites/%d/accept" % inv)
    check(st == 404, "accept: someone else's invite is 404", err)
    st, data, err = await mem.gapi("POST", "/studios/invites/%d/accept" % inv)
    check(st == 200 and data.get("accepted") is True and data.get("studio_id") == sid and data.get("studio_name") == name, "accept", (st, data, err))
    st, data, err = await mem.gapi("GET", "/studios/mine")
    check(st == 200 and len(data) == 1 and data[0]["role"] == 2 and data[0]["member_count"] == 2, "mine: the member's row (role 2, 2 members)", data)
    st, data, err = await mem.gapi("GET", "/studios/%d" % sid)
    members = {m["name"]: m["role"] for m in (data or {}).get("members", [])}
    check(st == 200 and data["my_role"] == 2 and members == {own.name: 1, mem.name: 2} and data["invites"] == [],
          "get: a member sees my_role 2 and both members", data)
    check(all("creator_badge" in m for m in data["members"]), "get: members carry creator_badge (0210)", data["members"])
    st, data, err = await mem.gapi("POST", "/studios/%d/members" % sid, {"player_name": out.name})
    check(st == 403 and code(err) == "STUDIO_FORBIDDEN", "invite: a member is not the owner", err)
    # decline + expiry
    st, data, err = await own.gapi("POST", "/studios/%d/members" % sid, {"player_name": ext.name})
    st, data, err = await ext.gapi("POST", "/studios/invites/%d/decline" % data["invite_id"])
    check(st == 200 and data.get("accepted") is False, "decline", (st, data, err))
    st, data, err = await ext.gapi("GET", "/studios/invites")
    check(st == 200 and data == [], "a declined invite is gone", data)
    st, data, err = await own.gapi("POST", "/studios/%d/members" % sid, {"player_name": ext.name})
    q("UPDATE studio_invites SET expires_at = %s WHERE id = ?" % ts(-1), data["invite_id"])
    st, data, err = await ext.gapi("GET", "/studios/invites")
    check(st == 200 and data == [], "an expired invite is not listed", data)
    st, data, err = await own.gapi("POST", "/studios/%d/members" % sid, {"player_name": ext.name})
    check(st == 200, "an expired invite can be sent again", err)
    # seats: 2 members + 1 invite + 17 more = 20
    fillers = []
    for i in range(17):
        aid = q("INSERT INTO accounts (username, password_hash) VALUES (?, 'x') RETURNING id", "stF%d_%s" % (i, stamp))[0][0]
        fillers.append(aid)
        q("INSERT INTO studio_invites (studio_id, account_id, invited_by, expires_at) VALUES (?, ?, ?, %s)" % ts(5), sid, aid, own.id)
    st, data, err = await own.gapi("POST", "/studios/%d/members" % sid, {"player_name": out.name})
    check(st == 400 and code(err) == "STUDIO_FULL", "invite: 20 seats counting pending invites", err)
    q("DELETE FROM studio_invites WHERE studio_id = ? AND account_id <> ?", sid, ext.id)

    # ---- games
    st, data, err = await own.gapi("POST", "/studios/%d/maps" % sid, {"map_id": m2})
    check(st == 403 and code(err) == "STUDIO_MAP_NOT_YOURS", "assign game: someone else's map", err)
    st, data, err = await own.gapi("POST", "/studios/%d/maps" % sid, {"map_id": 99999999})
    check(st == 404 and code(err) == "STUDIO_MAP_NOT_FOUND", "assign game: an unknown map", err)
    st, data, err = await mem.gapi("POST", "/studios/%d/maps" % sid, {"map_id": m2})
    check(st == 403 and code(err) == "STUDIO_FORBIDDEN", "assign game: only the owner", err)
    st, data, err = await own.gapi("POST", "/studios/%d/maps" % sid, {"map_id": m1})
    check(st == 200 and data == {"map_id": m1, "map_name": "StMapA" + stamp}, "assign game", (st, data, err))
    st, data, err = await own.gapi("POST", "/studios/%d/maps" % sid, {"map_id": m1})
    check(st == 409 and code(err) == "STUDIO_MAP_TAKEN", "assign game: one studio per game", err)
    st, data, err = await mem.gapi("GET", "/studios/%d" % sid)
    check(st == 200 and len(data["maps"]) == 1 and data["maps"][0]["map_id"] == m1 and data["maps"][0]["map_name"] == "StMapA" + stamp
          and data["maps"][0]["map_videolink"] == "dQw4w9WgXcQ", "get: maps with the video id (creator data)", data and data.get("maps"))

    # ---- currencies
    for body, what in (({"name": "Gold", "symbol": "KR", "decimals": 0}, "a reserved symbol"),
                       ({"name": "Gold", "symbol": "gld", "decimals": 0}, "a lower-case symbol"),
                       ({"name": "Go", "symbol": "GLD", "decimals": 0}, "a 2-letter name"),
                       ({"name": "Gold", "symbol": "GLD", "decimals": 3}, "3 decimals")):
        st, data, err = await own.gapi("POST", "/studios/%d/currencies" % sid, body)
        check(st == 400 and code(err) == "STUDIO_CURRENCY_INVALID", "currency: " + what + " is refused", err)
    st, data, err = await own.gapi("POST", "/studios/%d/currencies" % sid, {"name": "Gold Coins", "symbol": "GLD", "decimals": 2})
    cid = (data or {}).get("currency_id")
    check(st == 200 and cid and data["symbol"] == "GLD" and data["decimals"] == 2 and data["active"] is True, "currency: create", (st, data, err))
    st, data, err = await own.gapi("POST", "/studios/%d/currencies" % sid, {"name": "Golden", "symbol": "GLD", "decimals": 0})
    check(st == 409 and code(err) == "STUDIO_CURRENCY_TAKEN", "currency: the symbol is unique in the studio", err)
    # v10.0.4: 20 in total, 5 published at once (studios.currencies.hint / publishCap, deob-10.0.4.js:139268, :139320)
    # 2..7 through the API (the 6th and 7th past the publish cap), 8..19 in the database (the owner's 60 writes a
    # minute, studios.c write_ok, would run out), the 20th through the API again, then the 21st is refused
    made = []
    for i, sym in enumerate(("AA", "AB", "AC", "AD", "BA", "BB")):
        st, data, err = await own.gapi("POST", "/studios/%d/currencies" % sid, {"name": "Coin %d" % i, "symbol": sym, "decimals": 0})
        made.append((st, data))
    for i, sym in enumerate([a + b for a in "CDE" for b in "ABCD"]):
        q("INSERT INTO studio_currencies (studio_id, name, symbol, decimals, active) VALUES (?, ?, ?, 0, 0)", sid, "Coin db %d" % i, sym)
    st, data, err = await own.gapi("POST", "/studios/%d/currencies" % sid, {"name": "Coin twenty", "symbol": "FA", "decimals": 0})
    made.append((st, data))
    check(all(st == 200 for st, _ in made), "currency: 20 per studio can be created", [m[0] for m in made])
    check([d["active"] for _, d in made[:4]] == [True] * 4 and not any(d["active"] for _, d in made[4:]),
          "currency: the first 5 are published, the rest start unpublished", [d.get("active") for _, d in made])
    st, data, err = await own.gapi("POST", "/studios/%d/currencies" % sid, {"name": "Coin twenty one", "symbol": "ZZ", "decimals": 0})
    check(st == 400 and code(err) == "STUDIO_CURRENCY_LIMIT", "currency: 20 per studio", err)
    sixth = made[4][1]["currency_id"]
    st, data, err = await own.gapi("POST", "/studios/%d/currencies/%d/publish" % (sid, sixth))
    check(st == 400 and code(err) == "STUDIO_CURRENCY_PUBLISH_CAP", "currency: 5 published at once", err)
    st, data, err = await own.gapi("POST", "/studios/%d/currencies/%d/publish" % (sid, cid))
    check(st == 200 and data["active"] is True, "currency: re-publishing a published one is not a sixth", (st, data, err))
    st, data, err = await own.gapi("POST", "/studios/%d/currencies/%d/unpublish" % (sid, cid))
    check(st == 200 and data == {"currency_id": cid, "active": False}, "currency: unpublish", (st, data, err))
    st, data, err = await mem.gapi("GET", "/studios/%d" % sid)
    cur = [c for c in data["currencies"] if c["currency_id"] == cid]
    check(len(data["currencies"]) == 20 and cur and cur[0]["active"] is False and cur[0]["name"] == "Gold Coins", "get: the currencies", data["currencies"])
    st, data, err = await own.gapi("POST", "/studios/%d/currencies/%d/publish" % (sid, sixth))
    check(st == 200 and data["active"] is True, "currency: publish once one was unpublished", (st, data, err))
    st, data, err = await own.gapi("POST", "/studios/%d/currencies/%d/publish" % (sid, cid))
    check(st == 400 and code(err) == "STUDIO_CURRENCY_PUBLISH_CAP", "currency: publish again at the cap", err)
    st, data, err = await own.gapi("POST", "/studios/%d/currencies/%d/unpublish" % (sid, sixth))
    st, data, err = await own.gapi("POST", "/studios/%d/currencies/%d/publish" % (sid, cid))
    check(st == 200 and data["active"] is True, "currency: publish", (st, data, err))
    st, data, err = await mem.gapi("POST", "/studios/%d/currencies/%d/publish" % (sid, cid))
    check(st == 403, "currency: only the owner publishes", err)

    # ---- analytics, skins
    st, data, err = await mem.gapi("GET", "/studios/%d/analytics" % sid)
    check(st == 200 and data["kr_earned"] == 0 and data["kr_charged"] == 0 and len(data["currencies"]) == 20
          and all(c["circulating"] == 0 and c["holders"] == 0 for c in data["currencies"]), "analytics: KR and the currencies", data)
    st, data, err = await mem.gapi("GET", "/studios/%d/skins" % sid)
    check(st == 200 and data == [], "skins: none", (st, data, err))
    st, data, err = await own.gapi("POST", "/studios/%d/skins/import" % sid, {"custom_skin_id": 10000001})
    check(st == 404 and code(err) == "STUDIO_SKIN_NOT_FOUND", "skins: import refused (no custom skins)", err)

    # ---- monetization
    st, mon, err = await own.gapi("GET", "/studios/%d/monetization" % sid)
    keys = [r["key"] for r in (mon or {}).get("requirements", [])]
    check(st == 200 and keys == ["account_age", "level", "twofa", "no_recent_ban", "has_map"], "monetization: the five requirements", mon)
    reqs = {r["key"]: r for r in mon["requirements"]}
    check(mon["enabled"] is False and mon["blocked"] is False and mon["request"] is None and mon["next_apply_at"] is None
          and mon["eligible"] is False and mon["can_apply"] is False, "monetization: a new account is not eligible", mon)
    check(reqs["has_map"]["ok"] and reqs["no_recent_ban"]["ok"] and not reqs["twofa"]["ok"] and not reqs["account_age"]["ok"]
          and reqs["level"]["required"] == 20 and reqs["level"]["value"] >= 1 and not reqs["level"]["ok"], "monetization: requirement states", reqs)
    st, data, err = await own.gapi("POST", "/studios/%d/monetization" % sid, {"description": "A cosmetic shop in our map.", "map_ids": [m1]})
    check(st == 400 and code(err) == "MONETIZATION_NOT_ELIGIBLE", "apply: not eligible", err)
    q("UPDATE accounts SET created_at = %s, twofa_secret = 'JBSWY3DPEHPK3PXP' WHERE id = ?" % ts(-100), own.id)
    q("UPDATE players SET player_score = 50000000 WHERE player_id = ?", own.id)
    st, mon, err = await mem.gapi("GET", "/studios/%d/monetization" % sid)
    check(st == 200 and mon["eligible"] is True and mon["can_apply"] is False, "monetization: eligible, but a member cannot apply", mon)
    st, mon, err = await own.gapi("GET", "/studios/%d/monetization" % sid)
    check(st == 200 and mon["eligible"] is True and mon["can_apply"] is True, "monetization: the owner can apply", mon)
    st, data, err = await own.gapi("POST", "/studios/%d/monetization" % sid, {"description": "short", "map_ids": []})
    check(st == 400 and code(err) == "MONETIZATION_INVALID", "apply: under 10 characters", err)
    st, data, err = await own.gapi("POST", "/studios/%d/monetization" % sid, {"description": "A cosmetic shop in our map.", "map_ids": [m2]})
    check(st == 400 and code(err) == "MONETIZATION_MAPS_INVALID", "apply: an example game outside the studio", err)
    st, mon, err = await own.gapi("POST", "/studios/%d/monetization" % sid, {"description": "  A cosmetic shop in our map.  ", "map_ids": [m1]})
    check(st == 200 and mon["request"] and mon["request"]["status"] == "pending" and mon["request"]["description"] == "A cosmetic shop in our map."
          and mon["request"]["map_ids"] == [m1] and mon["can_apply"] is False, "apply: pending, the trimmed description and games", (st, mon, err))
    st, data, err = await own.gapi("POST", "/studios/%d/monetization" % sid, {"description": "A cosmetic shop in our map.", "map_ids": []})
    check(st == 409 and code(err) == "MONETIZATION_PENDING", "apply: twice while pending", err)
    row = q("SELECT applicant_id, status FROM studio_monetization WHERE studio_id = ?", sid)
    check(row == [(own.id, "pending")], "the application record", row)
    st, mon, err = await own.gapi("DELETE", "/studios/%d/monetization" % sid)
    check(st == 200 and mon["request"]["status"] == "cancelled" and mon["can_apply"] is True, "withdraw", (st, mon, err))
    st, data, err = await own.gapi("DELETE", "/studios/%d/monetization" % sid)
    check(st == 404 and code(err) == "MONETIZATION_NONE", "withdraw: nothing pending", err)
    await own.gapi("POST", "/studios/%d/monetization" % sid, {"description": "A cosmetic shop in our map.", "map_ids": []})
    q("UPDATE studio_monetization SET status = 'rejected', review_note = 'Not yet', reviewed_at = %s WHERE studio_id = ? AND status = 'pending'" % ts(0), sid)
    st, mon, err = await own.gapi("GET", "/studios/%d/monetization" % sid)
    check(st == 200 and mon["request"]["status"] == "rejected" and mon["request"]["review_note"] == "Not yet" and mon["next_apply_at"]
          and mon["can_apply"] is False, "rejected: the note and the next date", mon)
    q("UPDATE studios SET monetization = 1 WHERE id = ?", sid)
    st, data, err = await own.gapi("GET", "/studios/%d" % sid)
    check(st == 200 and data["monetization"] is True, "get: monetization true once approved", data and data.get("monetization"))

    # ---- transfer, leave
    st, data, err = await own.gapi("POST", "/studios/%d/transfer" % sid, {"player_name": out.name})
    check(st == 404 and code(err) == "STUDIO_NOT_MEMBER", "transfer: to a non-member", err)
    st, data, err = await mem.gapi("POST", "/studios/%d/transfer" % sid, {"player_name": mem.name})
    check(st == 403, "transfer: only the owner", err)
    st, data, err = await own.gapi("POST", "/studios/%d/transfer" % sid, {"player_name": mem.name})
    check(st == 200 and data.get("owner") == mem.name, "transfer", (st, data, err))
    st, data, err = await own.gapi("GET", "/studios/%d" % sid)
    members = {m["name"]: m["role"] for m in data["members"]}
    check(data["my_role"] == 2 and data["owner"] == mem.name and members == {own.name: 2, mem.name: 1}, "transfer: roles swapped", data)
    st, data, err = await mem.gapi("DELETE", "/studios/%d/members/%s" % (sid, mem.name))
    check(st == 400 and code(err) == "STUDIO_OWNER_LEAVE", "leave: the owner cannot", err)
    st, data, err = await own.gapi("DELETE", "/studios/%d/members/%s" % (sid, own.name))
    check(st == 200, "leave: the old owner leaves", err)
    st, data, err = await own.gapi("GET", "/studios/mine")
    check(st == 200 and data == [], "leave: the studio is gone from mine", data)
    st, data, err = await ext.gapi("POST", "/studios/invites/%d/accept" % q("SELECT id FROM studio_invites WHERE studio_id = ? AND account_id = ?", sid, ext.id)[0][0])
    st, data, err = await mem.gapi("DELETE", "/studios/%d/members/%s" % (sid, ext.name))
    check(st == 200, "remove member: the owner removes", err)
    check(q("SELECT count(*) FROM studio_members WHERE studio_id = ?", sid)[0][0] == 1, "remove member: one member left", None)

    # ---- the 3-studio limit, delete
    for i in range(2):
        st, data, err = await mem.gapi("POST", "/studios", {"name": "KnMore%d_%s" % (i, stamp)})
        check(st == 200, "create #%d" % (i + 2), err)
    st, data, err = await mem.gapi("POST", "/studios", {"name": "KnFour_" + stamp})
    check(st == 400 and code(err) == "STUDIO_LIMIT", "create: 3 owned studios at most", err)
    st, data, err = await out.gapi("DELETE", "/studios/%d" % sid)
    check(st == 404, "delete: an outsider", err)
    st, data, err = await mem.gapi("DELETE", "/studios/%d" % sid)
    check(st == 200, "delete", err)
    left = [q("SELECT count(*) FROM %s WHERE studio_id = ?" % t, sid)[0][0]
            for t in ("studio_members", "studio_invites", "studio_maps", "studio_currencies", "studio_monetization")]
    check(left == [0, 0, 0, 0, 0] and q("SELECT count(*) FROM studios WHERE id = ?", sid)[0][0] == 0, "delete: every row is gone", left)
    st, data, err = await mem.gapi("GET", "/studios/%d" % sid)
    check(st == 404, "delete: the studio is 404", err)
    st, data, err = await mem.gapi("POST", "/studios", {"name": "KnFour_" + stamp})
    check(st == 200, "delete: frees an owned slot", err)
    for s in (own, mem, out, ext):
        await s.close()


def main():
    (port,) = free_ports(1)
    log = open(os.path.join(WORK, "brain.log"), "w")
    cfg = os.path.join(WORK, "brain.json")
    dbcfg = {"driver": "sqlite", "path": DB}
    if DRIVER == "postgres":
        with open(os.environ.get("KN_TEST_CONFIG", os.path.join(ROOT, "config", "config.json"))) as fp:
            db = json.load(fp).get("database", {})
        dbname = os.environ.get("KN_TEST_DB_NAME", "krunknative_svcacc")
        PG["schema"] = "kn_test_studios_%d" % os.getpid()
        PG["env"] = dict(os.environ, PGPASSWORD=str(db.get("password", "")))
        PG["psql"] = ["psql", "-h", str(db.get("host", "127.0.0.1")), "-p", str(db.get("port", 5432)), "-U",
                      str(db.get("user", "krunknative")), "-d", dbname, "-v", "ON_ERROR_STOP=1", "-qAt"]
        try:
            subprocess.run(PG["psql"] + ["-c", "CREATE SCHEMA %s" % PG["schema"]], env=PG["env"], check=True, capture_output=True, timeout=30)
        except (OSError, subprocess.SubprocessError) as e:
            print("PostgreSQL unavailable (%s): skip" % e)
            sys.exit(77)
        dbcfg = dict(db, name=dbname, search_path=PG["schema"])
    elif DRIVER == "mysql":
        tool = shutil.which("mariadb") or shutil.which("mysql")
        host, mport = os.environ.get("KN_TEST_MYSQL_HOST", "127.0.0.1"), os.environ.get("KN_TEST_MYSQL_PORT", "3306")
        user, password = os.environ.get("KN_TEST_MYSQL_USER", "root"), os.environ.get("KN_TEST_MYSQL_PASSWORD", "")
        MY["name"] = "kn_test_studios_%d" % os.getpid()
        MY["env"] = dict(os.environ, MYSQL_PWD=password)
        base = [tool or "mariadb", "--no-defaults", "--protocol=TCP", "-h", host, "-P", mport, "-u", user]
        try:
            subprocess.run(base + ["-e", "CREATE DATABASE %s CHARACTER SET utf8mb4" % MY["name"]], env=MY["env"], check=True,
                           capture_output=True, timeout=30)
        except (OSError, subprocess.SubprocessError) as e:
            print("MySQL/MariaDB unavailable (%s): skip" % e)
            sys.exit(77)
        MY["base"], MY["cmd"] = base, base + [MY["name"]]
        dbcfg = {"driver": "mysql", "host": host, "port": int(mport), "user": user, "password": password, "name": MY["name"]}
    with open(cfg, "w") as f:
        json.dump({"database": dict(dbcfg, migrations_dir=os.path.join(ROOT, "services", "db", "migrations")),
                   "brain": {"host": "127.0.0.1", "port": port, "federation": False, "internal_secret": "studios-%d" % os.getpid()},
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
        if PG:
            subprocess.run(PG["psql"] + ["-c", "DROP SCHEMA IF EXISTS %s CASCADE" % PG["schema"]], env=PG["env"], capture_output=True)
        if MY:
            subprocess.run(MY["base"] + ["-e", "DROP DATABASE IF EXISTS %s" % MY["name"]], env=MY["env"], capture_output=True)
    print("studios test: " + ("OK" if not FAILS else "%d failure(s), files in %s" % (len(FAILS), WORK)))
    if not FAILS and not os.environ.get("KN_KEEP"):
        shutil.rmtree(WORK, ignore_errors=True)
    sys.exit(1 if FAILS else 0)


main()
