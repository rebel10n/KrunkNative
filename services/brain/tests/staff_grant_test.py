#!/usr/bin/env python3
"""The startup staff grant on kn_brain: accounts.developers / accounts.moderators (db_accounts.c).

  python3 -I services/brain/tests/staff_grant_test.py <kn_brain> <repo root>

At every brain start a developer named in accounts.developers gets player_developer and
player_moderator, the verified tick (player_featured), lifetime VIP (player_premium 315360000000 ms)
and the creator badge (creator_badge). A moderator gets player_moderator only; a plain account
gets none of them. The grant runs at each start, so a restart after the flags were wiped (a
database restore from an older copy) grants them again.

KN_TEST_DB_DRIVER=postgres runs the same on a scratch schema of the local PostgreSQL
(KN_TEST_CONFIG, default <repo>/config/config.json). 77 = skip (no kn_brain / websockets / database).
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

BRAIN, ROOT = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
if not os.path.exists(BRAIN):
    print("needs kn_brain at %s: skip" % BRAIN)
    sys.exit(77)
sys.path.insert(0, os.path.join(ROOT, "server", "tests"))
from kn_proto import free_ports  # noqa: E402

LIFETIME_PREMIUM = 315360000000  # the developers' premium (ms, ten years); migration 0110 widens it on PostgreSQL
PASSWORD = "social_pass_1"
FAILS = []
WORK = tempfile.mkdtemp(prefix="kn-staff-grant.", dir=os.path.expanduser("~/.cache"))
DB = os.path.join(WORK, "coordinator.db")
DRIVER = os.environ.get("KN_TEST_DB_DRIVER", "sqlite")
PG = {}
PORT = 0


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


async def register(name):
    """One social-socket registration (the same frame the clients send); returns the reply's error field."""
    ws = await websockets.connect("ws://127.0.0.1:%d/social/ws?enc=json" % PORT, max_size=None)
    try:
        await ws.send(json.dumps(["a", 0, [name, PASSWORD, None]]))
        for _ in range(100):
            f = json.loads(await asyncio.wait_for(ws.recv(), 15))
            if f and f[0] == "pi":
                await ws.send(json.dumps(["po"]))
                continue
            if f and f[0] == "a":
                return f[1]
        return "no reply"
    finally:
        await ws.close()


def flags(name):
    rows = q("SELECT p.player_developer, p.player_moderator, p.player_featured, p.player_premium, p.creator_badge "
             "FROM players p JOIN accounts a ON a.id = p.player_id WHERE lower(a.username) = ?", name.lower())
    return rows[0] if rows else None


def player_id(name):
    rows = q("SELECT id FROM accounts WHERE lower(username) = ?", name.lower())
    return rows[0][0] if rows else None


def wipe(name):
    pid = player_id(name)
    q("UPDATE players SET player_developer = 0, player_moderator = 0, player_featured = 0, player_premium = 0,"
      " creator_badge = 0 WHERE player_id = ?", pid)


def start_brain(tag, developers, moderators, migrate):
    """Start kn_brain with the staff lists; True once it listens. Each start has its own config and log."""
    global PORT
    PORT = free_ports(1)[0]
    dbcfg = {"driver": "sqlite", "path": DB} if DRIVER != "postgres" else PG["dbcfg"]
    cfg = os.path.join(WORK, "brain-%s.json" % tag)
    log_path = os.path.join(WORK, "brain-%s.log" % tag)
    with open(cfg, "w") as f:
        json.dump({"database": dict(dbcfg, migrations_dir=os.path.join(ROOT, "services", "db", "migrations")),
                   "brain": {"host": "127.0.0.1", "port": PORT, "federation": False,
                             "internal_secret": "staffgrant-%d" % os.getpid()},
                   "streams": {"enabled": False},
                   "accounts": {"signup_rewards": {"enabled": False}, "free_premium": False, "starting_kr": 0,
                                "login_throttle_s": 0, "password_iterations": 1000,
                                "email": {"required": False},
                                "developers": developers, "moderators": moderators,
                                "skins_file": os.path.join(ROOT, "assets", "skins.json")}}, f)
    log = open(log_path, "w")
    args = [BRAIN, "--config", cfg] + (["--migrate"] if migrate else [])
    proc = subprocess.Popen(args, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
    proc.log = log
    for _ in range(300):
        try:
            with open(log_path) as f:
                if re.search(r"listening on http://127\.0\.0\.1:%d" % PORT, f.read()):
                    return proc
        except OSError:
            pass
        if proc.poll() is not None:
            break
        time.sleep(0.1)
    return proc


def stop_brain(proc):
    proc.terminate()
    try:
        proc.wait(10)
    except subprocess.TimeoutExpired:
        proc.kill()
    proc.log.close()


def main():
    global PG
    if DRIVER == "postgres":
        cfg_path = os.environ.get("KN_TEST_CONFIG", os.path.join(ROOT, "config", "config.json"))
        try:
            with open(cfg_path) as fp:
                db = json.load(fp).get("database", {})
        except (OSError, ValueError):
            print("no database config for postgres: skip")
            sys.exit(77)
        dbname = os.environ.get("KN_TEST_DB_NAME", "krunknative_svcacc")
        PG["schema"] = "kn_test_staff_%d" % os.getpid()
        PG["env"] = dict(os.environ, PGPASSWORD=str(db.get("password", "")))
        PG["psql"] = ["psql", "-h", str(db.get("host", "127.0.0.1")), "-p", str(db.get("port", 5432)), "-U",
                      str(db.get("user", "krunknative")), "-d", dbname, "-v", "ON_ERROR_STOP=1", "-qAt"]
        try:
            subprocess.run(PG["psql"] + ["-c", "CREATE SCHEMA %s" % PG["schema"]], env=PG["env"], check=True,
                           capture_output=True, timeout=30)
        except (OSError, subprocess.SubprocessError) as e:
            print("PostgreSQL unavailable (%s): skip" % type(e).__name__)
            sys.exit(77)
        PG["dbcfg"] = dict(db, name=dbname, search_path=PG["schema"])

    stamp = str(int(time.time()) % 100000)
    dev_n, mod_n, plain_n = "sgdev%s" % stamp, "sgmod%s" % stamp, "sgplain%s" % stamp
    brain = None
    try:
        print("baseline (no staff lists):")
        brain = start_brain("1", [], [], migrate=True)
        check(brain.poll() is None, "kn_brain is up (%s)" % DRIVER)
        for n in (dev_n, mod_n, plain_n):
            check(asyncio.run(register(n)) is None, "registered %s" % n)
        check(flags(dev_n) == (0, 0, 0, 0, 0), "a new account has no staff flags, featured, premium or creator badge")
        stop_brain(brain)

        print("startup grant (developers and moderators named in the config):")
        brain = start_brain("2", [dev_n], [mod_n], migrate=False)
        check(brain.poll() is None, "kn_brain restarts with the staff lists")
        check(flags(dev_n) == (1, 1, 1, LIFETIME_PREMIUM, 1),
              "developer: developer, moderator, featured, lifetime premium, creator badge (%r)" % (flags(dev_n),))
        check(flags(mod_n) == (0, 1, 0, 0, 0), "moderator: moderator flag only (%r)" % (flags(mod_n),))
        check(flags(plain_n) == (0, 0, 0, 0, 0), "a plain account gets no staff grant (%r)" % (flags(plain_n),))
        stop_brain(brain)

        print("restore (flags wiped, then a start):")
        wipe(dev_n)
        check(flags(dev_n) == (0, 0, 0, 0, 0), "the developer's flags are wiped")
        brain = start_brain("3", [dev_n], [mod_n], migrate=False)
        check(brain.poll() is None, "kn_brain restarts again")
        check(flags(dev_n) == (1, 1, 1, LIFETIME_PREMIUM, 1),
              "the next start grants the developer again (%r)" % (flags(dev_n),))
        check(flags(mod_n) == (0, 1, 0, 0, 0), "the moderator is unchanged (%r)" % (flags(mod_n),))
    except Exception as e:  # noqa: BLE001
        import traceback
        traceback.print_exc()
        check(False, "exception: %r" % e)
    finally:
        if brain is not None and brain.poll() is None:
            stop_brain(brain)
        if PG:
            subprocess.run(PG["psql"] + ["-c", "DROP SCHEMA IF EXISTS %s CASCADE" % PG["schema"]], env=PG["env"],
                           capture_output=True)
    print("staff grant test (%s): " % DRIVER + ("OK" if not FAILS else "%d failure(s), files in %s" % (len(FAILS), WORK)))
    if not FAILS and not os.environ.get("KN_KEEP"):
        shutil.rmtree(WORK, ignore_errors=True)
    sys.exit(1 if FAILS else 0)


main()
