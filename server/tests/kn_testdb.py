"""Scratch PostgreSQL databases for the server end-to-end suites (dedicated_e2e.py, ranked_e2e.py).

create() makes the database as config/config.json's database user (CI: the PostgreSQL service's superuser;
a role with CREATEDB works too), and falls back to `sudo -n -u postgres createdb` (a developer machine with
the system cluster). Passwords go through PGPASSWORD, never argv. Returns False when neither works, so the
suite can exit 77 (CTest skip) instead of failing on a missing server.
"""
import os
import subprocess


def _conn(cfg):
    return (["-h", str(cfg.get("host", "127.0.0.1")), "-p", str(cfg.get("port", 5432)), "-U", cfg.get("user", "krunknative")],
            dict(os.environ, PGPASSWORD=str(cfg.get("password", ""))))


def create(cfg, name):
    args, env = _conn(cfg)
    r = subprocess.run(["createdb", *args, "-O", cfg.get("user", "krunknative"), name], env=env, capture_output=True, text=True)
    if r.returncode == 0:
        return True
    r = subprocess.run(["sudo", "-n", "-u", "postgres", "createdb", "-O", cfg.get("user", "krunknative"), name],
                       capture_output=True, text=True)
    return r.returncode == 0


def drop(cfg, name):
    args, env = _conn(cfg)
    r = subprocess.run(["dropdb", *args, "--if-exists", name], env=env, capture_output=True, text=True)
    if r.returncode:
        subprocess.run(["sudo", "-n", "-u", "postgres", "dropdb", "--if-exists", name], capture_output=True, text=True)
