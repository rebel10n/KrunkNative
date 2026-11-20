#!/usr/bin/env python3
"""Schema parity of the three store drivers (services/db/migrations/{postgres,sqlite,mysql}).

kn_migrate builds a scratch database on every driver that is reachable (db_scratch.py: PostgreSQL from
config/config.json, SQLite always, MySQL/MariaDB on KN_TEST_MYSQL_*), then the test lists every table and
column of each and compares them. The migration files differ per driver (PostgreSQL keeps functions and
triggers in their own files, SQLite and MySQL fold some steps), but the tables and columns the brain reads
and writes are the same everywhere: a column that exists on one driver only is a migration that was not
ported to the others.

Known differences, each with its reason, are listed in ALLOWED below. Fails with the missing columns per
driver; exits 77 (CTest skip) when fewer than two drivers are available.

usage: schema_parity_test.py <build dir>
"""
import json
import os
import shutil
import sqlite3
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
BUILD = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "cmake-build-debug"))
SCRATCH = [sys.executable, "-I", os.path.join(ROOT, "services", "brain", "tests", "db_scratch.py"), ROOT]
NAME = "kn_parity_%d" % os.getpid()

# Driver-internal bookkeeping, never read by the brain as data.
INTERNAL_TABLES = {"sqlite_sequence", "schema_migrations", "kn_schema_migrations"}

# (table, column-or-None) present on some drivers only, on purpose; each with its reason.
ALLOWED = {
    # MySQL has no advisory locks: the ranked matcher and the turf settlement lock this table's row
    # (SELECT ... FOR UPDATE NOWAIT; mysql/0201_ranked.sql, db_ranked.c, db_mm.c, db_turf.c); PostgreSQL uses
    # pg_try_advisory_xact_lock(kn_rq_lock()) and SQLite BEGIN IMMEDIATE
    ("mm_ranked_lock", None),
    # MySQL has no partial unique index: "one live map / mod per name" is a unique index on this stored
    # generated column (NULL once deleted; mysql/0102_items_creations.sql); PostgreSQL and SQLite index
    # lower(name) WHERE deleted_at IS NULL instead
    ("published_maps", "live_name"),
    ("published_mods", "live_name"),
}


def scratch(action, driver):
    r = subprocess.run(SCRATCH + [action, driver, NAME], capture_output=True, text=True, timeout=120)
    if action == "drop":
        return None
    if r.returncode:
        return None
    return json.loads(r.stdout)


def env_for(cfg):
    env = dict(os.environ)
    for k, v in cfg.items():
        env["KN_DATABASE_" + k.upper()] = str(v)
    return env


def migrate(cfg):
    exe = os.path.join(BUILD, "services", "common", "kn_migrate")
    r = subprocess.run([exe], cwd=ROOT, env=env_for(cfg), capture_output=True, text=True, timeout=600)
    return r.returncode, (r.stdout + r.stderr)[-2000:]


def columns_sqlite(cfg):
    con = sqlite3.connect(cfg["path"])
    out = set()
    tables = [t for (t,) in con.execute("SELECT name FROM sqlite_master WHERE type = 'table'")]
    for t in tables:
        for row in con.execute('PRAGMA table_info("%s")' % t.replace('"', '""')):
            out.add((t.lower(), row[1].lower()))
    con.close()
    return out


def columns_postgres(cfg):
    sql = ("SELECT table_name, column_name FROM information_schema.columns c JOIN information_schema.tables t "
           "USING (table_schema, table_name) WHERE c.table_schema = '%s' AND t.table_type = 'BASE TABLE'"
           % cfg["search_path"])
    r = subprocess.run(["psql", "-h", cfg["host"], "-p", str(cfg["port"]), "-U", cfg["user"], "-d", cfg["name"],
                        "-v", "ON_ERROR_STOP=1", "-qAt", "-F", "\t", "-c", sql],
                       env=dict(os.environ, PGPASSWORD=cfg.get("password", "")), capture_output=True, text=True, timeout=60)
    if r.returncode:
        raise RuntimeError(r.stderr)
    return {tuple(x.lower() for x in line.split("\t")) for line in r.stdout.splitlines() if line}


def columns_mysql(cfg):
    tool = shutil.which("mariadb") or shutil.which("mysql")
    sql = ("SELECT c.table_name, c.column_name FROM information_schema.columns c JOIN information_schema.tables t "
           "ON t.table_schema = c.table_schema AND t.table_name = c.table_name "
           "WHERE c.table_schema = '%s' AND t.table_type = 'BASE TABLE'" % cfg["name"])
    r = subprocess.run([tool, "--no-defaults", "--protocol=TCP", "-h", cfg["host"], "-P", str(cfg["port"]), "-u", cfg["user"],
                        "-N", "-B", "-e", sql], env=dict(os.environ, MYSQL_PWD=cfg.get("password", "")),
                       capture_output=True, text=True, timeout=60)
    if r.returncode:
        raise RuntimeError(r.stderr)
    return {tuple(x.lower() for x in line.split("\t")) for line in r.stdout.splitlines() if line}


READERS = {"sqlite": columns_sqlite, "postgres": columns_postgres, "mysql": columns_mysql}


def main():
    schemas = {}
    failed = False
    try:
        for driver in ("postgres", "sqlite", "mysql"):
            cfg = scratch("create", driver)
            if cfg is None:
                print(f"skip   {driver}: unavailable")
                continue
            rc, log = migrate(cfg)
            if rc:
                print(f"FAIL   {driver}: kn_migrate exit {rc}\n{log}")
                failed = True
                continue
            cols = {c for c in READERS[driver](cfg) if c[0] not in INTERNAL_TABLES}
            schemas[driver] = cols
            print(f"ok     {driver}: {len({t for t, _ in cols})} tables, {len(cols)} columns")
    finally:
        for driver in ("postgres", "sqlite", "mysql"):
            scratch("drop", driver)
    if len(schemas) < 2 and not failed:
        print("schema parity: fewer than two drivers available; skipped")
        sys.exit(77)

    every = set().union(*schemas.values()) if schemas else set()
    tables = {d: {t for t, _ in cols} for d, cols in schemas.items()}
    for driver, cols in sorted(schemas.items()):
        missing_tables = sorted(t for t in {t for t, _ in every} - tables[driver] if (t, None) not in ALLOWED)
        missing_cols = sorted((t, c) for t, c in every - cols
                              if t in tables[driver] and (t, c) not in ALLOWED and (t, None) not in ALLOWED)
        for t in missing_tables:
            have = [d for d in schemas if t in tables[d]]
            print(f"FAIL   {driver}: table {t} missing (on {', '.join(sorted(have))})")
            failed = True
        for t, c in missing_cols:
            have = [d for d in schemas if (t, c) in schemas[d]]
            print(f"FAIL   {driver}: column {t}.{c} missing (on {', '.join(sorted(have))})")
            failed = True
    print("schema parity: " + ("FAILED" if failed else "OK (%s)" % ", ".join(sorted(schemas))))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
