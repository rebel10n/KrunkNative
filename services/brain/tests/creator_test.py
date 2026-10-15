#!/usr/bin/env python3
"""Creators on kn_brain: v10's creator badge and the maps' YouTube videos (social socket + HTTP).

  python3 -I services/brain/tests/creator_test.py <native build dir> <repo root>

Creator badge (players.creator_badge, migration 0210; profiles.c "crt", db_social.c P_SET_CREATOR):
a plain account's ["crt", name, 1] is refused (crtr null, nothing changes); a developer's gives it
(crtr true, moderation_log 'creator') and takes it back ('uncreator'). The badge is sent as
creator_badge in the account info of the sign-in ("a" frame, its 7th field), in v10's profile
(gapi GET /players/<name>) and in the hub's profile row (["r", "profile", name]).

Map video (creations.c uploadM, shared/src/youtube_link.c): a published map keeps the YouTube id of
its link (map_video), not the text typed; a link that is not a YouTube video stores none; /api/maps
lists it as map_videolink and the map page (["r", "map", name]) as vl.

KN_TEST_DB_DRIVER=postgres runs the same against a scratch schema of the local PostgreSQL
(config/config.json credentials or KN_TEST_CONFIG), which checks the PostgreSQL functions
(kn_account_json, the profile query). 77 = skip (no kn_brain / websockets / database).
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
import urllib.request

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
WORK = tempfile.mkdtemp(prefix="kn-creator.", dir=os.path.expanduser("~/.cache"))
DB = os.path.join(WORK, "coordinator.db")
PORT = free_ports(1)[0]
DRIVER = os.environ.get("KN_TEST_DB_DRIVER", "sqlite")
PG = {}
VIDEO = "dQw4w9WgXcQ"


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what, flush=True)
    if not cond:
        FAILS.append(what)


def lit(v):
    if v is None:
        return "NULL"
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


def http_json(path):
    with urllib.request.urlopen("http://127.0.0.1:%d%s" % (PORT, path), timeout=20) as r:
        return json.loads(r.read().decode())


class Sock:
    def __init__(self):
        self.ws = None
        self.inbox = []
        self.rid = 0

    async def open(self):
        self.ws = await websockets.connect("ws://127.0.0.1:%d/social/ws?enc=json" % PORT, max_size=None)
        return self

    async def send(self, frame):
        await self.ws.send(json.dumps(frame))

    async def recv_until(self, pred, timeout=10.0):
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
        return f

    async def ask(self, frame, label):
        await self.send(frame)
        return await self.recv_until(lambda f: f[0] == label)

    async def close(self):
        await self.ws.close()


def info_of(a_frame):
    info = a_frame[6] if a_frame and len(a_frame) > 6 else None
    return info if isinstance(info, dict) else {}


async def creator_badge(names):
    dev_n, tgt_n, plain_n = names
    D, T, P = await Sock().open(), await Sock().open(), await Sock().open()
    dev, _ = await D.register(dev_n)
    tgt, tgt_tok = await T.register(tgt_n)
    await P.register(plain_n)
    q("UPDATE players SET player_developer = 1 WHERE player_id = ?", dev)
    # the developer flag is read per request: sign in again so the session is fresh either way
    check(q("SELECT creator_badge FROM players WHERE player_id = ?", tgt)[0][0] == 0, "a new account has no creator badge")
    f = await T.login(tgt_n, tgt_tok)
    check(info_of(f).get("creator_badge") == 0, "the sign-in's account info says creator_badge 0")

    r = await P.ask(["crt", tgt_n, 1], "crtr")
    check(r is not None and r[1] is None, "a plain account's crt is refused (crtr null)")
    check(q("SELECT creator_badge FROM players WHERE player_id = ?", tgt)[0][0] == 0, "the refused crt changed nothing")

    r = await D.ask(["crt", tgt_n, 1], "crtr")
    check(r is not None and r[1] is True, "a developer's crt gives the badge (crtr true)")
    check(q("SELECT creator_badge FROM players WHERE player_id = ?", tgt)[0][0] == 1, "players.creator_badge = 1")
    check(bool(q("SELECT 1 FROM moderation_log WHERE actor_id = ? AND target_id = ? AND action = 'creator'", dev, tgt)),
          "moderation_log has the developer's 'creator' row")

    f = await T.login(tgt_n, tgt_tok)
    check(info_of(f).get("creator_badge") == 1, "the creator's sign-in carries creator_badge 1 (account info)")
    st, r = await P.gapi("GET", "/players/%s" % tgt_n)
    check(st == 200 and r and r["data"].get("creator_badge") == 1, "v10 profile (gapi /players/<name>): creator_badge 1")
    st, r = await P.gapi("GET", "/players/%s" % plain_n)
    check(st == 200 and r and r["data"].get("creator_badge") == 0, "another profile: creator_badge 0")
    page = await P.ask(["r", "profile", tgt_n], "0")
    row = page[3] if page and len(page) > 3 else None
    check(isinstance(row, dict) and row.get("creator_badge") == 1, "the hub's profile row: creator_badge 1")

    r = await D.ask(["crt", tgt_n, 0], "crtr")
    check(r is not None and r[1] is True, "a developer's crt 0 takes the badge back")
    check(q("SELECT creator_badge FROM players WHERE player_id = ?", tgt)[0][0] == 0, "players.creator_badge = 0 again")
    check(bool(q("SELECT 1 FROM moderation_log WHERE target_id = ? AND action = 'uncreator'", tgt)), "logged as 'uncreator'")
    st, r = await P.gapi("GET", "/players/%s" % tgt_n)
    check(st == 200 and r["data"].get("creator_badge") == 0, "the profile no longer has the badge")
    r = await D.ask(["crt", "nobody_zz_%s" % dev_n[-5:], 1], "crtr")
    check(r is not None and r[1] is None, "crt on an unknown name: crtr null")
    for s in (D, T, P):
        await s.close()


def map_json(name):
    return json.dumps({"name": name, "objects": [{"p": [0, 0, 0], "s": [10, 10, 10]}], "spawns": [[0, 10, 0, 0, 0, 0]]})


async def map_video(names):
    pub_n = names[0]
    S = await Sock().open()
    await S.register(pub_n)
    cases = [
        ("cv%s" % pub_n[-5:], "https://youtu.be/%s?t=42" % VIDEO, VIDEO),
        ("cw%s" % pub_n[-5:], "https://www.youtube.com/watch?feature=share&v=%s" % VIDEO, VIDEO),
        ("cx%s" % pub_n[-5:], "https://evil.example/watch?v=%s" % VIDEO, None),
        ("cy%s" % pub_n[-5:], "<script>alert(1)</script>", None),
        ("cz%s" % pub_n[-5:], "", None),
    ]
    for name, link, want in cases:
        r = await S.ask(["uploadM", map_json(name), None, "desc", link, 0], "pubMR")
        check(r is not None and r[1] == "Map Published!", "publish %s (%r)" % (name, link))
        got = q("SELECT 'v:' || coalesce(map_video, '') FROM published_maps WHERE map_name = ?", name)
        stored = (got[0][0][2:] or None) if got else "missing"
        check(stored == want,
              "%r is stored as %r (map_video %r)" % (link, want, stored))
    # an update with a new link replaces the id; one without a video clears it
    name0 = cases[0][0]
    r = await S.ask(["uploadM", map_json(name0), None, "desc", "https://youtube.com/shorts/abcdefghijk", 0], "pubMR")
    check(r is not None and r[1] == "Map Updated!", "republish %s" % name0)
    check(q("SELECT map_video FROM published_maps WHERE map_name = ?", name0)[0][0] == "abcdefghijk", "the update keeps the new id")
    r = await S.ask(["uploadM", map_json(name0), None, "desc", VIDEO, 0], "pubMR")
    check(q("SELECT map_video FROM published_maps WHERE map_name = ?", name0)[0][0] == VIDEO, "a bare id is kept as is")

    # the lists (map_videolink) and the map page (vl)
    lst = http_json("/api/maps?index=2&pos=0&val=")
    rows = {r.get("map_name"): r for r in lst.get("data") or []}
    check(rows.get(name0, {}).get("map_videolink") == VIDEO, "/api/maps lists map_videolink for the map with a video")
    check(rows.get(cases[2][0], {}).get("map_videolink") == "", "/api/maps: map_videolink is empty without a video")
    check(all("map_videolink" in r for r in lst.get("data") or []), "every /api/maps row has map_videolink (catalogue rows too)")
    feat = http_json("/api/maps?index=1&pos=0&val=")
    check(all("map_videolink" in r for r in feat.get("data") or []), "the featured list's rows have map_videolink")
    page = await S.ask(["r", "map", name0], "0")
    row = page[3] if page and len(page) > 3 else None
    check(isinstance(row, dict) and row.get("vl") == VIDEO, "the map page's vl is the id (%r)" % (row or {}).get("vl"))
    await S.close()


def main():
    log = open(os.path.join(WORK, "brain.log"), "w")
    cfg = os.path.join(WORK, "brain.json")
    dbcfg = {"driver": "sqlite", "path": DB}
    if DRIVER == "postgres":
        with open(os.environ.get("KN_TEST_CONFIG", os.path.join(ROOT, "config", "config.json"))) as fp:
            db = json.load(fp).get("database", {})
        dbname = os.environ.get("KN_TEST_DB_NAME", "krunknative_svcacc")
        PG["schema"] = "kn_test_creator_%d" % os.getpid()
        PG["env"] = dict(os.environ, PGPASSWORD=str(db.get("password", "")))
        PG["psql"] = ["psql", "-h", str(db.get("host", "127.0.0.1")), "-p", str(db.get("port", 5432)), "-U",
                      str(db.get("user", "krunknative")), "-d", dbname, "-v", "ON_ERROR_STOP=1", "-qAt"]
        try:
            subprocess.run(PG["psql"] + ["-c", "CREATE SCHEMA %s" % PG["schema"]], env=PG["env"], check=True, capture_output=True, timeout=30)
        except (OSError, subprocess.SubprocessError) as e:
            print("PostgreSQL unavailable (%s): skip" % type(e).__name__)
            sys.exit(77)
        dbcfg = dict(db, name=dbname, search_path=PG["schema"])
    with open(cfg, "w") as f:
        json.dump({"database": dict(dbcfg, migrations_dir=os.path.join(ROOT, "services", "db", "migrations")),
                   "brain": {"host": "127.0.0.1", "port": PORT, "federation": False, "internal_secret": "crt-%d" % os.getpid()},
                   "streams": {"enabled": False},
                   "accounts": {"free_premium": False, "starting_kr": 0, "login_throttle_s": 0, "password_iterations": 1000,
                                "email": {"required": False},
                                "skins_file": os.path.join(ROOT, "assets", "skins.json")}}, f)
    brain = subprocess.Popen([BRAIN, "--config", cfg, "--migrate"], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
    try:
        up = False
        for _ in range(300):
            try:
                with open(os.path.join(WORK, "brain.log")) as f:
                    if re.search(r"listening on http://127\.0\.0\.1:%d" % PORT, f.read()):
                        up = True
                        break
            except OSError:
                pass
            if brain.poll() is not None:
                break
            time.sleep(0.1)
        check(up, "kn_brain is up (%s)" % DRIVER)
        stamp = str(int(time.time()) % 100000)
        print("creator badge:")
        asyncio.run(creator_badge(["crdev%s" % stamp, "crtgt%s" % stamp, "crplain%s" % stamp]))
        print("map YouTube video:")
        asyncio.run(map_video(["crpub%s" % stamp]))
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
    print("creator test (%s): " % DRIVER + ("OK" if not FAILS else "%d failure(s), files in %s" % (len(FAILS), WORK)))
    if not FAILS and not os.environ.get("KN_KEEP"):
        shutil.rmtree(WORK, ignore_errors=True)
    sys.exit(1 if FAILS else 0)


main()
