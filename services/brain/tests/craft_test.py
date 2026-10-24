#!/usr/bin/env python3
"""The Junkyard's crafting on the social socket (services/brain/craft.c): ["recps"] and ["crft", SID]
against kn_brain on a scratch database.

  craft_test.py <kn_brain> <repo root>
env: KN_TEST_DB_DRIVER=sqlite (default) | postgres | mysql (db_scratch.py). 77 = skip.

The recipes are fixtures from the config (accounts.junkyard_recipes): the real table
(assets/junkyard_recipes.json) is empty until v10's rows are copied in. Covered: the recipe list
(craftable results only, the world limit CNT), a craft that takes its materials and grants the
result, missing materials (nothing taken), a paid recipe while no junk wallet is linked ("Not
enough Junk", nothing taken), the world limit ("Sold out"), a non-craftable result, an unknown
recipe and a logged-out request. Junk is the wallet's (doc/v10/junk.md): these checks need no
junk, so they run before the wallet lands.
"""
import asyncio
import json
import os
import re
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

BRAIN, ROOT = sys.argv[1], os.path.abspath(sys.argv[2])
DRIVER = os.environ.get("KN_TEST_DB_DRIVER", "sqlite")
NAME = "kn_test_craft_%d" % os.getpid()
FAILS = []

# skins.json store indices (assets/skins.json): 1 Wood and 3 Wires are collectible materials,
# 3283 Krystallizer and 3921 Plasma Cutter are craftable results, 0 (Krystal) is not craftable
WOOD, WIRES = 1, 3
RESULT, RESULT2 = 3283, 3921
NOT_CRAFTABLE = 0
RECIPES = [
    {"SID": 1, "SOUT": RESULT, "SIN": "%d,2,%d,1" % (WOOD, WIRES), "JUNK": 0, "DUR": 0, "CNT": -1, "DAT": 1700000000000},
    {"SID": 2, "SOUT": RESULT, "SIN": "%d,1" % WOOD, "JUNK": 0, "DUR": 0, "CNT": 1, "DAT": 1700000000000},
    {"SID": 3, "SOUT": RESULT2, "SIN": "%d,1" % WOOD, "JUNK": 5, "DUR": 0, "CNT": -1, "DAT": 1700000000000},
    # not craftable: skipped when the table loads
    {"SID": 4, "SOUT": NOT_CRAFTABLE, "SIN": "%d,1" % WOOD, "JUNK": 0, "DUR": 0, "CNT": -1, "DAT": 0},
]


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what, flush=True)
    if not cond:
        FAILS.append(what)


# ---------------------------------------------------------------- database (setup + invariants)

class Db:
    def __init__(self, cfg):
        self.cfg = cfg

    def q(self, sql, *args):
        def lit(a):
            if a is None:
                return "NULL"
            if isinstance(a, int):
                return str(a)
            return "'" + str(a).replace("'", "''") + "'"
        text = re.sub(r"\$(\d+)", lambda m: lit(args[int(m.group(1)) - 1]), sql)
        d = self.cfg["driver"]
        if d == "sqlite":
            con = sqlite3.connect(self.cfg["path"], timeout=30)
            try:
                rows = con.execute(text).fetchall()
                con.commit()
            finally:
                con.close()
            return [tuple(None if v is None else str(v) for v in r) for r in rows]
        if d == "postgres":
            env = dict(os.environ, PGPASSWORD=str(self.cfg.get("password", "")), PGOPTIONS="-c search_path=" + self.cfg["search_path"])
            cmd = ["psql", "-h", str(self.cfg.get("host", "127.0.0.1")), "-p", str(self.cfg.get("port", 5432)), "-U",
                   str(self.cfg.get("user", "krunknative")), "-d", self.cfg["name"], "-v", "ON_ERROR_STOP=1", "-qAtX",
                   "-F", "\x1f", "-c", text]
        else:
            env = dict(os.environ, MYSQL_PWD=str(self.cfg.get("password", "")))
            cmd = [os.environ.get("KN_TEST_MYSQL_CLIENT", "mariadb"), "--no-defaults", "--protocol=TCP", "-h", str(self.cfg.get("host")),
                   "-P", str(self.cfg.get("port")), "-u", str(self.cfg.get("user")), "-D", self.cfg["name"], "-N", "-B", "-e", text]
        r = subprocess.run(cmd, env=env, capture_output=True, timeout=60)
        if r.returncode:
            raise RuntimeError("sql failed: %s: %s" % (text[:120], r.stderr.decode(errors="replace")))
        sep = "\x1f" if d == "postgres" else "\t"
        out = []
        for line in r.stdout.decode().splitlines():
            out.append(tuple(None if v in ("", "NULL") and d != "postgres" else v for v in line.split(sep)))
        return out

    def one(self, sql, *args):
        rows = self.q(sql, *args)
        return int(rows[0][0]) if rows and rows[0][0] not in (None, "") else 0


def give(db, aid, skin, count):
    have = db.one("SELECT count FROM inventory WHERE account_id = $1 AND skin_id = $2", aid, skin)
    if db.q("SELECT 1 FROM inventory WHERE account_id = $1 AND skin_id = $2", aid, skin):
        db.q("UPDATE inventory SET count = $3 WHERE account_id = $1 AND skin_id = $2", aid, skin, have + count)
    else:
        db.q("INSERT INTO inventory (account_id, skin_id, count) VALUES ($1, $2, $3)", aid, skin, count)


def owned(db, aid, skin):
    return db.one("SELECT count FROM inventory WHERE account_id = $1 AND skin_id = $2", aid, skin)


# ---------------------------------------------------------------- social socket (JSON mode)

class Social:
    def __init__(self, url):
        self.url = url + "?enc=json"
        self.inbox = []

    async def __aenter__(self):
        self.ws = await websockets.connect(self.url, max_size=8 << 20)
        return self

    async def __aexit__(self, *a):
        await self.ws.close()

    async def send(self, *frame):
        await self.ws.send(json.dumps(list(frame)))

    async def recv(self, label, timeout=20.0):
        deadline = time.time() + timeout
        while True:
            for i, f in enumerate(self.inbox):
                if f[0] == label:
                    return self.inbox.pop(i)
            left = deadline - time.time()
            if left <= 0:
                raise AssertionError("timeout waiting for %r (inbox %r)" % (label, [f[0] for f in self.inbox]))
            self.inbox.append(json.loads(await asyncio.wait_for(self.ws.recv(), left)))

    async def call(self, reply, *frame, timeout=20.0):
        await self.send(*frame)
        return await self.recv(reply, timeout=timeout)

    async def login(self, name, password):
        r = await self.call("a", "a", 1, [name, password, None])
        assert r[1] is None, "login %s: %r" % (name, r[1])
        return r


PASS = "craft_pass1"


async def register(url, name):
    async with Social(url) as s:
        r = await s.call("a", "a", 0, [name, PASS, None])
        assert r[1] is None, "register %s: %r" % (name, r[1])
        return r[2]


async def user(url, name):
    s = Social(url)
    await s.__aenter__()
    await s.login(name, PASS)
    return s


async def craft(s, sid):
    """The crftr answer for one ["crft", SID]: (ok, message)."""
    r = await s.call("crftr", "crft", sid)
    assert r[1] == sid, "crftr for another recipe: %r" % (r,)
    return r[2] == 1, r[3]


async def scenario(url, db):
    name_a, name_b, name_out = "craft_a%d" % os.getpid(), "craft_b%d" % os.getpid(), "craft_o%d" % os.getpid()  # at most 14 characters: a new name's cap (v10.0.3 maxNameLength, deob-10.0.3.js:22725)
    aid = await register(url, name_a)
    bid = await register(url, name_b)
    await register(url, name_out)
    outsider = Social(url)
    await outsider.__aenter__()
    try:
        # logged out: no recipes, no craft
        r = await outsider.call("recps", "recps")
        check(r[1] is None and r[2] is None, "recps logged out: no recipe list")
        ok, msg = await craft(outsider, 1)
        check(not ok and msg == "Must be logged in", "crft logged out: refused (%r)" % msg)
    finally:
        await outsider.__aexit__()

    sa = await user(url, name_a)
    sb = await user(url, name_b)
    try:
        # the recipe list: the non-craftable recipe is not offered, CNT is -1 (unlimited) or the copies left
        r = await sa.call("recps", "recps")
        rows = {row["SID"]: row for row in (r[1] or [])}
        check(sorted(rows) == [1, 2, 3], "recps lists the craftable recipes (%r)" % sorted(rows))
        check(rows.get(1, {}).get("SIN") == "%d,2,%d,1" % (WOOD, WIRES) and rows.get(1, {}).get("SOUT") == RESULT,
              "recps row keeps SIN and SOUT as the table has them")
        check(rows.get(1, {}).get("CNT") == -1 and rows.get(2, {}).get("CNT") == 1, "recps CNT: -1 unlimited, 1 copy left")
        check(r[2] == 0, "recps reports the junk balance (0: no wallet linked)")

        # missing materials: nothing is taken
        give(db, aid, WOOD, 3)
        ok, msg = await craft(sa, 1)
        check(not ok and msg == "Missing materials", "crft without Wires: Missing materials (%r)" % msg)
        check(owned(db, aid, WOOD) == 3, "missing materials: no Wood taken")

        # a paid recipe with no junk wallet linked is refused, and nothing is taken
        ok, msg = await craft(sa, 3)
        check(not ok and msg == "Not enough Junk", "crft a paid recipe without junk: Not enough Junk (%r)" % msg)
        check(owned(db, aid, WOOD) == 3, "insufficient junk: no material taken")

        # a craft that has its materials: taken, the result granted
        give(db, aid, WIRES, 1)
        ok, msg = await craft(sa, 1)
        check(ok and msg == "Crafted.", "crft with its materials: crafted (%r)" % msg)
        check(owned(db, aid, WOOD) == 1 and owned(db, aid, WIRES) == 0, "craft takes 2 Wood and 1 Wires")
        check(owned(db, aid, RESULT) == 1, "craft grants the result skin")
        check(db.one("SELECT COUNT(*) FROM skin_transactions WHERE account_id = $1 AND source = 'craft' AND delta > 0", aid) == 1,
              "the grant is one craft row in the ledger")

        # the world limit: recipe 2 has one copy; the second craft is sold out and takes nothing
        give(db, aid, WOOD, 1)
        ok, msg = await craft(sa, 2)
        check(ok, "crft recipe 2 (the one copy): crafted (%r)" % msg)
        give(db, bid, WOOD, 1)
        r = await sb.call("recps", "recps")
        rows = {row["SID"]: row for row in (r[1] or [])}
        check(rows.get(2, {}).get("CNT") == 0, "recps shows recipe 2 sold out (CNT 0)")
        ok, msg = await craft(sb, 2)
        check(not ok and msg == "Sold out", "crft a sold out recipe: Sold out (%r)" % msg)
        check(owned(db, bid, WOOD) == 1 and owned(db, bid, RESULT) == 0, "sold out: nothing taken, nothing granted")

        # painted copies are materials too (doc/v10/paint-copy.md): recipe 1 takes 2 Wood and 1 Wires; one Wood is a
        # painted copy and the Wires is a painted copy. The unpainted Wood goes first; the copy's paint goes with it.
        name_p = "cpaint%d" % (os.getpid() % 1000000)
        pid = await register(url, name_p)
        sp = await user(url, name_p)
        try:
            give(db, pid, WOOD, 2)
            give(db, pid, WIRES, 1)
            db.q("INSERT INTO skin_copies (account_id, skin_id, paint_color, paint_shader) VALUES ($1, $2, '#dc3c3c', NULL)", pid, WOOD)
            db.q("INSERT INTO skin_copies (account_id, skin_id, paint_color, paint_shader) VALUES ($1, $2, NULL, 0)", pid, WIRES)
            ok, msg = await craft(sp, 1)
            check(ok and msg == "Crafted.", "crft with painted copies as materials: crafted (%r)" % msg)
            check(owned(db, pid, WOOD) == 0 and owned(db, pid, WIRES) == 0 and owned(db, pid, RESULT) == 1,
                  "painted materials: 2 Wood and 1 Wires taken, the result granted")
            check(db.one("SELECT COUNT(*) FROM skin_copies WHERE account_id = $1", pid) == 0,
                  "painted materials: the copies that were taken are gone (the Wood copy with its paint)")
        finally:
            await sp.__aexit__()

        # a result that is not craftable, an unknown recipe
        ok, msg = await craft(sa, 4)
        check(not ok and msg == "Not craftable.", "crft a non-craftable recipe: Not craftable (%r)" % msg)
        ok, msg = await craft(sa, 99)
        check(not ok and msg == "Not craftable.", "crft an unknown recipe: Not craftable (%r)" % msg)
    finally:
        await sa.__aexit__()
        await sb.__aexit__()


async def main_async(url, db):
    await scenario(url, db)


def recipes_config(skins):
    return {"accounts": {"signup_rewards": {"enabled": False}, "password_iterations": 1000, "login_throttle_s": 0,
                         "starting_kr": 0, "free_premium": False, "skins_file": skins,
                         "junkyard_recipes": {"recipes": RECIPES}}}


def main():
    scratch = [sys.executable, "-I", os.path.join(ROOT, "services", "brain", "tests", "db_scratch.py"), ROOT]
    r = subprocess.run(scratch + ["create", DRIVER, NAME], capture_output=True)
    if r.returncode:
        print("%s unavailable (%s): skip" % (DRIVER, r.stderr.decode(errors="replace").strip()))
        return 77
    dbcfg = json.loads(r.stdout)
    tmp = tempfile.mkdtemp(prefix="kn-craft-test-", dir=os.path.expanduser("~/.cache"))
    cfg = {
        "database": dict(dbcfg, pool_size=4, migrations_dir=os.path.join(ROOT, "services", "db", "migrations")),
        "brain": {"host": "127.0.0.1", "port": 0, "internal_secret": "craft-%d" % os.getpid(), "federation": False},
    }
    cfg.update(recipes_config(os.path.join(ROOT, "assets", "skins.json")))
    with open(os.path.join(tmp, "config.json"), "w") as fp:
        json.dump(cfg, fp)
    log_path = os.path.join(tmp, "brain.log")
    log = open(log_path, "w")
    proc = subprocess.Popen([BRAIN, "--config", os.path.join(tmp, "config.json"), "--migrate"], stdout=log, stderr=subprocess.STDOUT,
                            cwd=ROOT, start_new_session=True)
    rc = 1
    try:
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
        asyncio.run(asyncio.wait_for(main_async("ws://127.0.0.1:%d/social/ws" % port, Db(dbcfg)), 300))
        rc = 1 if FAILS else 0
    except Exception as e:  # noqa: BLE001
        import traceback
        traceback.print_exc()
        print("FAIL exception: %r" % (e,))
        rc = 1
    finally:
        proc.terminate()
        try:
            proc.wait(10)
        except subprocess.TimeoutExpired:
            proc.kill()
        log.close()
        with open(log_path) as fp:
            text = fp.read()
        if rc:
            print("---- brain log tail (%s) ----\n%s" % (log_path, text[-4000:]))
        subprocess.run(scratch + ["drop", DRIVER, NAME], capture_output=True)
        if not rc:
            for f in os.listdir(tmp):
                os.unlink(os.path.join(tmp, f))
            os.rmdir(tmp)
    print("craft test (%s): %s" % (DRIVER, "OK" if not rc else "%d failure(s)" % len(FAILS)))
    return rc


if __name__ == "__main__":
    sys.exit(main())
