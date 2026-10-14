#!/usr/bin/env python3
"""Krunker v10.0.2 clan window backend (client/src/ui/clan_ui.c): the social socket's clan ops and
the settings tab's gapi routes (services/brain/clan_v10.c, social_data.c, db_social.c,
0113_clan_v10), on a kn_brain with its own sqlite database (never config/config.json).

  clan_v10_test.py <kn_brain> <repo root>
  KN_TEST_DB_DRIVER=postgres: a scratch schema in krunknative_svcacc (config/config.json credentials,
  or the file KN_TEST_CONFIG names)

Covers: create clan, join request + accept, the owner making a Captain, a Captain's rights (role
change but no Captains, Decline All, no Discord), the Discord link (cln 9), s7, MOTD set / get /
delete (members read, only role >= 2 writes), banner colour from a seeded paint (consumed; none
left = refused), ownership transfer initiate / cancel / respond, and the loot config read.
77 = skip (no kn_brain / websockets).
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
import urllib.request

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
WORK = tempfile.mkdtemp(prefix="kn-clan-v10.", dir=os.path.expanduser("~/.cache"))
DB = os.path.join(WORK, "coordinator.db")
RID = itertools.count(1)


def check(cond, what, detail=""):
    print(("  ok   " if cond else "  FAIL ") + what + (("  -- " + str(detail)) if detail and not cond else ""), flush=True)
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
        if not line or line == "SET":
            continue
        rows.append(tuple(None if c == "" else int(c) if re.fullmatch(r"-?\d+", c) else c for c in line.split("|")))
    return rows


NOW_PLUS_1S = "now() + interval '1 second'" if DRIVER == "postgres" else "strftime('%Y-%m-%d %H:%M:%f', 'now', '+1 second')"


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
        await self.send("a", 0, [name, "clan_pass_1", None])
        f = await self.wait(lambda f: f[0] == "a")
        assert f and f[1] is None, f
        self.id = f[2]
        return f[2]

    async def cln(self, action, value=1, extra=None):
        frame = ["cln", action, value] + ([extra] if extra is not None else [])
        await self.send(*frame)
        return await self.wait(lambda f: f[0] == "cln" and len(f) >= 5)

    async def gapi(self, method, path, body=None):
        rid = next(RID)
        await self.send("gapi", rid, method, path, body)
        f = await self.wait(lambda f: f[0] == "gapi" and f[1] == rid)
        return (f[2], (f[3] or {}).get("data"), (f[3] or {}).get("error")) if f else (0, None, None)

    async def close(self):
        await self.ws.close()


async def scenario(port):
    stamp = str(int(time.time()) % 100000)
    a, b, c, d = [await Sock.open(port) for _ in range(4)]
    names = ["clA" + stamp, "clB" + stamp, "clC" + stamp, "clD" + stamp]
    for s, n in zip((a, b, c, d), names):
        await s.auth(n)
    tag = "T" + stamp[-3:]

    f = await a.cln(1, tag)
    check(f and f[3] == 1 and f[2] and f[2]["clan_name"] == tag, "create clan", f)
    clan = f[2]["clan_id"] if f and f[2] else 0
    check(f and f[2] and "clan_link4" in f[2] and "clan_banner_color" in f[2], "clanData has the v10 fields", f and f[2])

    for s in (b, c):
        f = await s.cln(2, tag)
        check(f and f[1] == "Request Sent", "join request", f)
    await a.send("clnR", b.id, 1)
    f = await a.wait(lambda f: f[0] == "cln" and len(f) >= 5)
    check(f and f[3] == 1 and any(m["i"] == b.id for m in f[2]["members"]), "owner accepts the request", f)
    await a.send("clnR", c.id, 1)
    await a.wait(lambda f: f[0] == "cln" and len(f) >= 5)

    f = await a.cln(6, b.id, {"r": 2})
    check(f and f[3] == 1 and f[2] == {"i": b.id, "r": 2}, "owner makes a Captain", f)
    f = await a.cln(6, c.id, {"r": 3})
    check(f and f[3] == 0, "nobody is made Commander", f)
    f = await b.cln(6, c.id, {"r": 1})
    check(f and f[3] == 1 and f[2]["r"] == 1, "Captain changes a Soldier's role", f)
    f = await b.cln(6, c.id, {"r": 2})
    check(f and f[3] == 0 and f[1] == "No Permission", "Captain cannot make Captains", f)
    f = await c.cln(6, b.id, {"r": 0})
    check(f and f[3] == 0, "a Soldier manages nobody", f)

    f = await d.cln(2, tag)
    check(f and f[1] == "Request Sent", "a third request", f)
    await b.send("clnRA")
    await asyncio.sleep(0.5)
    check(q("SELECT count(*) FROM clan_requests WHERE clan_id = ?", clan)[0][0] == 0, "Captain declines all requests")

    f = await b.cln(9, "https://discord.gg/abcDEF")
    check(f and f[3] == 0, "a Captain cannot set the Discord link", f)
    f = await a.cln(9, "not a link")
    check(f and f[3] == 0 and f[1] == "Invalid Discord", "invalid Discord link refused", f)
    f = await a.cln(9, "https://discord.com/invite/abcDEF-12")
    check(f and f[3] == 1 and f[2]["clan_link4"] == "abcDEF-12", "owner sets the Discord link", f)

    q("INSERT INTO match_results (match_id, account_id, score, at) VALUES ('m1', ?, 1234, " + NOW_PLUS_1S + ")", b.id)
    f = await a.cln(3)
    sb = [m for m in (f[2]["members"] if f and f[2] else []) if m["i"] == b.id]
    check(sb and sb[0]["s7"] == 1234, "s7 = the member's score of the last 7 days", sb)

    st, data, err = await a.gapi("PUT", "/clans/%d/motd" % clan, {"message": "  Hello clan  "})
    check(st == 200 and data and data["message"] == "Hello clan" and data["set_by_name"] == names[0], "owner sets the MOTD", (st, data, err))
    st, data, err = await c.gapi("GET", "/clans/%d/motd" % clan)
    check(st == 200 and data and data["message"] == "Hello clan" and data["set_at"], "a member reads the MOTD", (st, data, err))
    st, data, err = await c.gapi("PUT", "/clans/%d/motd" % clan, {"message": "nope"})
    check(st != 200 and err, "a Soldier cannot set the MOTD", (st, data, err))
    st, data, err = await d.gapi("GET", "/clans/%d/motd" % clan)
    check(st != 200, "an outsider cannot read the MOTD", (st, data, err))
    st, data, err = await b.gapi("DELETE", "/clans/%d/motd" % clan)
    check(st == 200 and data and data["message"] is None, "a Captain removes the MOTD", (st, data, err))

    q("INSERT INTO inventory (account_id, skin_id, count) VALUES (?, 7776, 1)", a.id)
    st, data, err = await a.gapi("POST", "/clans/%d/banner-color" % clan, {"paint_item_id": 7776})
    check(st == 200 and data and data["clan_banner_color"] == "#dc3c3c", "banner colour from a paint", (st, data, err))
    check(q("SELECT count FROM inventory WHERE account_id = ? AND skin_id = 7776", a.id)[0][0] == 0, "the paint is consumed")
    st, data, err = await a.gapi("POST", "/clans/%d/banner-color" % clan, {"paint_item_id": 7776})
    check(st != 200 and err and "paint" in err.get("message", ""), "no paint left: refused", (st, data, err))
    st, data, err = await a.gapi("POST", "/clans/%d/banner-color" % clan, {"paint_item_id": 5})
    check(st != 200, "a non-paint item is refused", (st, data, err))
    f = await a.cln(3)
    check(f and f[2]["clan_banner_color"] == "#dc3c3c", "clanData carries the banner colour", f and f[2])

    st, data, err = await b.gapi("POST", "/clans/%d/transfer" % clan, {"name": names[2]})
    check(st != 200, "only the owner transfers", (st, data, err))
    st, data, err = await a.gapi("POST", "/clans/%d/transfer" % clan, {"name": names[3]})
    check(st != 200, "transfer to a non-member refused", (st, data, err))
    st, data, err = await a.gapi("POST", "/clans/%d/transfer" % clan, {"name": names[2]})
    check(st == 200 and data and data["to_player_id"] == c.id and data["from_player_name"] == names[0], "owner starts a transfer", (st, data, err))
    st, data, err = await a.gapi("DELETE", "/clans/%d/transfer" % clan)
    check(st == 200, "owner cancels the transfer", (st, data, err))
    st, data, err = await c.gapi("POST", "/clans/%d/transfer/respond" % clan, {"accept": True})
    check(st != 200, "no pending transfer: respond refused", (st, data, err))
    await a.gapi("POST", "/clans/%d/transfer" % clan, {"name": names[2]})
    f = await c.cln(3)
    ot = f[2].get("ownership_transfer") if f and f[2] else None
    check(ot and ot["to_player_id"] == c.id and ot["clan_name"] == tag, "the target sees the pending transfer", f and f[2])
    st, data, err = await b.gapi("POST", "/clans/%d/transfer/respond" % clan, {"accept": True})
    check(st != 200, "someone else cannot accept", (st, data, err))
    st, data, err = await c.gapi("POST", "/clans/%d/transfer/respond" % clan, {"accept": True})
    check(st == 200 and data and data["accepted"], "the target accepts", (st, data, err))
    row = q("SELECT clan_playerid, clan_transfer_to FROM clans WHERE clan_id = ?", clan)[0]
    check(row[0] == c.id and row[1] is None, "the clan changed hands", row)
    ranks = dict(q("SELECT account_id, rank FROM clan_members WHERE clan_id = ?", clan))
    check(ranks.get(c.id) == 3 and ranks.get(a.id) == 2, "new owner Commander, old owner Captain", ranks)

    st, data, err = await a.gapi("POST", "/clans/%d/logo" % clan, {})
    check(st != 200 and err, "logo upload refused (no upload pipeline)", (st, data, err))
    st, data, err = await a.gapi("DELETE", "/clans/%d/logo" % clan)
    check(st == 200, "a Captain removes the logo", (st, data, err))

    with urllib.request.urlopen("http://127.0.0.1:%d/api/clans/%d/loot-config" % (port, clan), timeout=10) as r:
        body = json.loads(r.read())
    data = body.get("data") or {}
    cfg = (data.get("config") or [data])[0]
    check(r.status == 200 and cfg.get("distribution_mode") == "clan_bank", "loot config read", body)

    f = await c.cln(4, 1)
    await asyncio.sleep(0.3)
    check(q("SELECT count(*) FROM clans WHERE clan_id = ?", clan)[0][0] == 0, "the owner deletes the clan")
    for s in (a, b, c, d):
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
        PG["schema"] = "kn_test_clan_v10_%d" % os.getpid()
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
                   "brain": {"host": "127.0.0.1", "port": port, "federation": False, "internal_secret": "clan-v10-%d" % os.getpid()},
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
    print("clan v10 test: " + ("OK" if not FAILS else "%d failure(s), files in %s" % (len(FAILS), WORK)))
    if not FAILS and not os.environ.get("KN_KEEP"):
        shutil.rmtree(WORK, ignore_errors=True)
    sys.exit(1 if FAILS else 0)


main()
