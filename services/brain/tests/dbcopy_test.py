#!/usr/bin/env python3
"""kn_dbcopy: a coordinator SQLite database into PostgreSQL (doc/backend.md 3.11).

  dbcopy_test.py <build dir> <repo root>

A kn_brain on a scratch SQLite file registers accounts (accounts, players, sessions,
login_audit, ...); the test then adds what production data has and a naive copy gets wrong:
a premium time beyond int32 (0110_premium_bigint), follows (the follows_count trigger must not
count them twice), an AUTOINCREMENT counter above max(id), JSON settings, fractional UTC
timestamps. Then: a schema migrated without 0110_premium_bigint is refused before anything is
copied (the column named); kn_migrate a scratch PostgreSQL schema with this tree's migrations
(0110 applied: bigint), kn_dbcopy (identical checksums),
the refusal without --replace, an idempotent --replace, a tampered row caught by
--verify-only (exit 3), sequences past the copied ids, and kn_brain serving the copied
accounts from PostgreSQL (/auth/login of a copied user). Skips (77) without PostgreSQL.
"""
import http.client
import json
import os
import re
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import time

FAILS = []


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what, flush=True)
    if not cond:
        FAILS.append(what)


def find(build, name):
    for d, _, files in os.walk(build):
        if name in files and os.access(os.path.join(d, name), os.X_OK):
            return os.path.join(d, name)
    return None


def start_brain(exe, root, args, log_path):
    log = open(log_path, "w")
    proc = subprocess.Popen([exe, "--brain.port=0", "--brain.host=127.0.0.1", "--brain.federation=false",
                             "--accounts.password_iterations=10000",
                             "--database.migrations_dir=" + os.path.join(root, "services", "db", "migrations")] + args,
                            stdout=log, stderr=subprocess.STDOUT, cwd=root)
    for _ in range(600):
        if proc.poll() is not None:
            break
        with open(log_path) as fp:
            m = re.search(r"listening on http://127\.0\.0\.1:(\d+) \(ws", fp.read())
        if m:
            return proc, int(m.group(1))
        time.sleep(0.05)
    proc.kill()
    raise RuntimeError("kn_brain did not start: " + open(log_path).read()[-2000:])


def post(port, path, body):
    c = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
    c.request("POST", path, body=json.dumps(body), headers={"Content-Type": "application/json"})
    r = c.getresponse()
    data = r.read()
    c.close()
    return r.status, json.loads(data) if data else None


def main():
    build, root = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
    brain, dbcopy, migrate = find(build, "kn_brain"), find(build, "kn_dbcopy"), find(build, "kn_migrate")
    if not (brain and dbcopy and migrate):
        print("kn_brain / kn_dbcopy / kn_migrate not built: skip")
        return 77
    schema = "kn_test_dbcopy_%d" % os.getpid()
    scratch = [sys.executable, "-I", os.path.join(root, "services", "brain", "tests", "db_scratch.py"), root]
    r = subprocess.run(scratch + ["create", "postgres", schema], capture_output=True)
    if r.returncode:
        print("PostgreSQL unavailable: skip")
        return 77
    pg = json.loads(r.stdout)
    # this tree's migrations, always: a config.json symlinked from another checkout would make the
    # tools resolve services/db/migrations next to *that* file (kn_config_root follows symlinks)
    migs = os.path.join(root, "services", "db", "migrations")
    pgset = ["--set", "database.driver=postgres", "--set", "database.search_path=" + schema,
             "--set", "database.migrations_dir=" + migs]
    tmp = tempfile.mkdtemp(prefix="kn-dbcopy-test-")
    lite = os.path.join(tmp, "coord.db")
    procs = []
    try:
        # ---- source: a real brain on SQLite
        p, port = start_brain(brain, root, ["--migrate", "--database.driver=sqlite", "--database.path=" + lite],
                              os.path.join(tmp, "lite.log"))
        procs.append(p)
        users = ["dbc_a%d" % i for i in range(5)]
        for u in users:
            st, _ = post(port, "/auth/register", {"username": u, "password": "hunter22"})
            check(st == 200, "register %s on sqlite" % u)
        p.terminate()
        p.wait(10)
        procs.remove(p)
        db = sqlite3.connect(lite)
        db.execute("UPDATE players SET player_premium = 315360000000 WHERE player_id = 2")
        db.execute("INSERT INTO follows (follower_id, followee_id) VALUES (1, 2), (3, 2), (2, 1)")
        db.execute("UPDATE accounts SET settings = '{\"b\": [1, 2.5, {\"z\": null, \"a\": true}], \"a\": \"x\"}' WHERE id = 1")
        db.execute("INSERT INTO accounts (username, password_hash) VALUES ('dbc_gone', 'x')")
        db.execute("DELETE FROM accounts WHERE username = 'dbc_gone'")  # sqlite_sequence now above max(id)
        db.commit()
        followed = db.execute("SELECT player_followed FROM players WHERE player_id = 2").fetchone()[0]
        seq = db.execute("SELECT seq FROM sqlite_sequence WHERE name = 'accounts'").fetchone()[0]
        db.close()
        check(followed == 2 and seq == 6, "sqlite source prepared (followed %d, seq %d)" % (followed, seq))
        before = open(lite, "rb").read()

        # ---- destination
        env = dict(os.environ, PGPASSWORD=str(pg.get("password", "")))
        psql = ["psql", "-h", str(pg["host"]), "-p", str(pg["port"]), "-U", pg["user"], "-d", pg["name"], "-qAt",
                "-v", "ON_ERROR_STOP=1"]

        def q(sql, sch=schema):
            return subprocess.run(psql + ["-c", "SET search_path = %s; %s" % (sch, sql)], env=env,
                                  capture_output=True, text=True).stdout.strip()

        # a destination migrated without 0110_premium_bigint: refused before copying, column named
        old_migs = os.path.join(tmp, "migrations-before-0110")
        shutil.copytree(migs, old_migs)
        for f in os.listdir(os.path.join(old_migs, "postgres")):
            if f.startswith("0110_"):  # the schema as it was before this fix
                os.unlink(os.path.join(old_migs, "postgres", f))
        old_schema = schema + "_old"
        r = subprocess.run(scratch + ["create", "postgres", old_schema], capture_output=True)
        oldset = ["--set", "database.driver=postgres", "--set", "database.search_path=" + old_schema,
                  "--set", "database.migrations_dir=" + old_migs]
        r = subprocess.run([migrate] + oldset, capture_output=True, text=True, cwd=root)
        check(r.returncode == 0 and q("SELECT data_type FROM information_schema.columns WHERE table_schema = "
                                      "current_schema() AND table_name = 'players' AND column_name = 'player_premium'",
                                      old_schema) == "integer", "an older schema (before 0110) has an integer player_premium")
        r = subprocess.run([dbcopy, "--sqlite", lite] + oldset, capture_output=True, text=True, cwd=root)
        check(r.returncode == 1 and "players.player_premium holds 315360000000" in r.stderr
              and q("SELECT count(*) FROM accounts", old_schema) == "0",
              "kn_dbcopy refuses it before copying, naming the column: %r" % r.stderr.strip()[-200:])
        subprocess.run(scratch + ["drop", "postgres", old_schema], capture_output=True)

        r = subprocess.run([migrate] + pgset, capture_output=True, text=True, cwd=root)
        check(r.returncode == 0, "kn_migrate the postgres schema")
        check(q("SELECT count(*) FROM kn_migrations WHERE version = 110") == "1" and
              q("SELECT data_type FROM information_schema.columns WHERE table_schema = current_schema() AND "
                "table_name = 'players' AND column_name = 'player_premium'") == "bigint",
              "0110_premium_bigint applied: player_premium is bigint")
        r = subprocess.run([dbcopy, "--sqlite", lite] + pgset, capture_output=True, text=True, cwd=root)
        print(r.stdout[-1500:], r.stderr[-1500:])
        check(r.returncode == 0 and "verify: identical" in r.stdout, "kn_dbcopy copies and verifies")
        check(open(lite, "rb").read() == before, "the SQLite file is not modified")
        check(q("SELECT player_followed FROM players WHERE player_id = 2") == "2", "follower counter not doubled")
        check(q("SELECT player_premium FROM players WHERE player_id = 2") == "315360000000", "premium beyond int32")
        check(q("SELECT settings::jsonb->'b'->2->>'a' FROM accounts WHERE id = 1") == "true", "JSON settings as jsonb")
        check(q("SELECT nextval('accounts_id_seq')") == "7", "accounts sequence continues after sqlite_sequence")
        r = subprocess.run([dbcopy, "--sqlite", lite] + pgset, capture_output=True, text=True, cwd=root)
        check(r.returncode == 1 and "refusing" in r.stderr, "refuses a populated destination without --replace")
        r = subprocess.run([dbcopy, "--sqlite", lite, "--replace"] + pgset, capture_output=True, text=True, cwd=root)
        check(r.returncode == 0 and "verify: identical" in r.stdout, "--replace is idempotent")
        q("UPDATE players SET player_kills = player_kills + 1 WHERE player_id = 3")
        r = subprocess.run([dbcopy, "--sqlite", lite, "--verify-only"] + pgset, capture_output=True, text=True, cwd=root)
        check(r.returncode == 3 and re.search(r"players .* MISMATCH", r.stdout), "--verify-only catches a changed row")
        r = subprocess.run([dbcopy, "--sqlite", lite, "--replace"] + pgset, capture_output=True, text=True, cwd=root)
        check(r.returncode == 0, "re-copy repairs it")

        # ---- the brain on the copied PostgreSQL data
        args = ["--database.driver=postgres", "--database.search_path=" + schema]
        p, port = start_brain(brain, root, args, os.path.join(tmp, "pg.log"))
        procs.append(p)
        st, r = post(port, "/auth/login", {"username": users[1], "password": "hunter22"})
        check(st == 200 and r["account"]["id"] == 2 and r["account"]["premium"] == 315360000000,
              "kn_brain on postgres logs in a copied account")
        st, r = post(port, "/auth/register", {"username": "dbc_new", "password": "hunter22"})
        check(st == 200 and r["account"]["id"] == 7, "new accounts continue after the copied counter (%s)" % (r or {}).get("account", {}).get("id"))
    except Exception as e:  # noqa: BLE001
        print("FAIL exception: %r" % (e,))
        FAILS.append("exception")
    finally:
        for p in procs:
            p.kill()
        subprocess.run(scratch + ["drop", "postgres", schema], capture_output=True)
        subprocess.run(scratch + ["drop", "postgres", schema + "_old"], capture_output=True)
        if not FAILS:
            subprocess.run(["rm", "-rf", tmp])
        else:
            print("logs kept in", tmp)
    print("%d failure(s)" % len(FAILS))
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
