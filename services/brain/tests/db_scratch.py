#!/usr/bin/env python3
"""Per-run scratch database for the brain integration tests, on any store driver.

  db_scratch.py <repo root> create <driver> <name> [--shell]
  db_scratch.py <repo root> drop   <driver> <name>

create prints the "database" config section to use (JSON), or with --shell
`export KN_DATABASE_*=...` lines for sh. Drivers:
  postgres  schema <name> in config/config.json's database (KN_TEST_DB_NAME
            overrides the database name), like pg_schema.py
  sqlite    file /tmp/kn-dbtest-<name>/coordinator.db (no server needed)
  mysql     database <name> on KN_TEST_MYSQL_HOST/PORT/USER/PASSWORD
            (default 127.0.0.1:3306 root), created with the mariadb/mysql client
Exit 77 (CTest skip) when the server or client tool is unavailable.
Passwords go through the environment (PGPASSWORD / MYSQL_PWD), never argv.
"""
import json
import os
import re
import shlex
import shutil
import subprocess
import sys


def load_db(root):
    try:
        with open(os.path.join(root, "config", "config.json")) as fp:
            return json.load(fp).get("database", {})
    except (OSError, ValueError):
        return {}


def run(cmd, env):
    try:
        return subprocess.run(cmd, env=env, capture_output=True, timeout=60)
    except (OSError, subprocess.SubprocessError) as e:
        return subprocess.CompletedProcess(cmd, 127, b"", str(e).encode())


def postgres(root, action, name):
    db = load_db(root)
    get = lambda k, d: os.environ.get("KN_DATABASE_" + k.upper(), str(db.get(k, d)))
    dbname = os.environ.get("KN_TEST_DB_NAME", get("name", "krunknative"))
    env = dict(os.environ, PGPASSWORD=get("password", ""))
    sql = ("DROP SCHEMA IF EXISTS %s CASCADE; CREATE SCHEMA %s" % (name, name) if action == "create"
           else "DROP SCHEMA IF EXISTS %s CASCADE" % name)
    r = run(["psql", "-h", get("host", "127.0.0.1"), "-p", get("port", 5432), "-U", get("user", "krunknative"),
             "-d", dbname, "-v", "ON_ERROR_STOP=1", "-qAt", "-c", sql], env)
    if r.returncode:
        print("PostgreSQL unavailable: %s" % r.stderr.decode(errors="replace").strip()[:200], file=sys.stderr)
        return 77, None
    return 0, {"driver": "postgres", "host": get("host", "127.0.0.1"), "port": int(get("port", 5432)),
               "user": get("user", "krunknative"), "password": get("password", ""), "name": dbname,
               "search_path": name}


def sqlite(root, action, name):
    d = "/tmp/kn-dbtest-" + name
    if action == "drop":
        shutil.rmtree(d, ignore_errors=True)
        return 0, None
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    return 0, {"driver": "sqlite", "path": os.path.join(d, "coordinator.db")}


def mysql(root, action, name):
    host = os.environ.get("KN_TEST_MYSQL_HOST", "127.0.0.1")
    port = os.environ.get("KN_TEST_MYSQL_PORT", "3306")
    user = os.environ.get("KN_TEST_MYSQL_USER", "root")
    password = os.environ.get("KN_TEST_MYSQL_PASSWORD", "")
    tool = shutil.which("mariadb") or shutil.which("mysql")
    if not tool:
        print("no mariadb/mysql client: skip", file=sys.stderr)
        return 77, None
    sql = ("DROP DATABASE IF EXISTS %s; CREATE DATABASE %s CHARACTER SET utf8mb4" % (name, name) if action == "create"
           else "DROP DATABASE IF EXISTS %s" % name)
    r = run([tool, "--no-defaults", "--protocol=TCP", "-h", host, "-P", port, "-u", user, "-e", sql],
            dict(os.environ, MYSQL_PWD=password))
    if r.returncode:
        print("MySQL/MariaDB unavailable at %s:%s: %s" % (host, port, r.stderr.decode(errors="replace").strip()[:200]),
              file=sys.stderr)
        return 77, None
    return 0, {"driver": "mysql", "host": host, "port": int(port), "user": user, "password": password, "name": name}


def main():
    if len(sys.argv) < 5:
        print(__doc__)
        return 2
    root, action, driver, name = sys.argv[1:5]
    if not re.fullmatch(r"[a-z_][a-z0-9_]{0,62}", name) or action not in ("create", "drop"):
        print("bad arguments")
        return 2
    fn = {"postgres": postgres, "sqlite": sqlite, "mysql": mysql}.get(driver)
    if not fn:
        print("unknown driver %s" % driver)
        return 2
    rc, cfg = fn(root, action, name)
    if rc or action == "drop":
        return rc
    if "--shell" in sys.argv[5:]:
        for k, v in cfg.items():
            print("export KN_DATABASE_%s=%s" % (k.upper(), shlex.quote(str(v))))
    else:
        print(json.dumps(cfg))
    return 0


sys.exit(main())
