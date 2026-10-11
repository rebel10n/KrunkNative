#!/usr/bin/env python3
"""Create or drop a per-run PostgreSQL schema for the brain integration tests.

  pg_schema.py <repo root> <database> create|drop <schema>

Credentials: config/config.json "database" (+ KN_DATABASE_HOST/PORT/USER/PASSWORD
env overrides, like knsvc). Exit 77 when PostgreSQL is unreachable (CTest skip).
The password is passed through PGPASSWORD and never printed.
"""
import json
import os
import re
import subprocess
import sys


def main():
    root, dbname, action, schema = sys.argv[1:5]
    if not re.fullmatch(r"[a-z_][a-z0-9_]{0,62}", schema):
        print("bad schema name %r" % schema)
        return 2
    db = {}
    try:
        with open(os.path.join(root, "config", "config.json")) as fp:
            db = json.load(fp).get("database", {})
    except (OSError, ValueError):
        pass
    get = lambda k, d: os.environ.get("KN_DATABASE_" + k.upper(), str(db.get(k, d)))
    env = dict(os.environ, PGPASSWORD=get("password", ""))
    sql = ("DROP SCHEMA IF EXISTS %s CASCADE; CREATE SCHEMA %s" % (schema, schema) if action == "create"
           else "DROP SCHEMA IF EXISTS %s CASCADE" % schema)
    cmd = ["psql", "-h", get("host", "127.0.0.1"), "-p", get("port", 5432), "-U", get("user", "krunknative"),
           "-d", dbname, "-v", "ON_ERROR_STOP=1", "-qAt", "-c", sql]
    try:
        r = subprocess.run(cmd, env=env, capture_output=True, timeout=30)
    except (OSError, subprocess.SubprocessError) as e:
        print("PostgreSQL unavailable (%s)" % e)
        return 77
    if r.returncode:
        print("PostgreSQL unavailable: %s" % r.stderr.decode(errors="replace").strip()[:200])
        return 77
    return 0


sys.exit(main())
