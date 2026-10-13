#!/usr/bin/env python3
"""Load test of the brain's hot endpoints per backend mode (doc/backend.md 3.11).

  load_test.py <build dir> <repo root> [--modes sqlite,postgres,postgres+redis,sqlite+redis]
               [--seconds 10] [--conns 64] [--threads 2] [--users 200] [--json OUT]

For every mode: a fresh database (db_scratch.py) and, for "+redis", a throwaway
redis-server / valkey-server; one kn_brain with every role (auth, community, finder) built as
it is (use a Release build for numbers); <users> registered accounts. Then kn_loadgen
hammers, one endpoint at a time (closed loop, keep-alive):
  session      GET /auth/me?token=...   (cycling through every user's token)
  login        POST /auth/login         (cycling through the accounts; PBKDF2 at 10000 iterations:
                                         production's 600000 is 60x slower and then dominates)
  finder       GET /servers
  presence     GET /mm/presence
  leaderboard  social WS ["r","leaders","player_score",0,...] with leaderboard_cache_s 1
               (<conns> sockets, one frame per 55 ms each: the socket's 20 frames/s limit)
and the table of req/s and p99 is printed (and written as JSON with --json).
Not a ctest: it takes minutes and its numbers depend on the machine.
"""
import argparse
import http.client
import json
import os
import re
import secrets
import shutil
import socket
import subprocess
import sys
import tempfile
import time


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def find(build, name):
    for d, _, files in os.walk(build):
        if name in files and os.access(os.path.join(d, name), os.X_OK):
            return os.path.join(d, name)
    sys.exit("%s not built in %s" % (name, build))


def post(port, path, body):
    c = http.client.HTTPConnection("127.0.0.1", port, timeout=30)
    c.request("POST", path, body=json.dumps(body), headers={"Content-Type": "application/json"})
    r = c.getresponse()
    data = r.read()
    c.close()
    return r.status, json.loads(data) if data else None


def run_mode(mode, a, root, brain, loadgen, tmp):
    driver = "postgres" if mode.startswith("postgres") else "sqlite"
    use_redis = mode.endswith("+redis")
    name = "kn_load_%s_%d" % (driver, os.getpid())
    scratch = [sys.executable, "-I", os.path.join(root, "services", "brain", "tests", "db_scratch.py"), root]
    r = subprocess.run(scratch + ["create", driver, name], capture_output=True)
    if r.returncode:
        print("%s: database unavailable, skipped" % mode)
        return None
    dbcfg = json.loads(r.stdout)
    procs = []
    try:
        cfg = {
            "database": dict(dbcfg, migrations_dir=os.path.join(root, "services", "db", "migrations")),
            "log": {"level": "warn"},
            "brain": {"host": "127.0.0.1", "port": 0, "federation": False, "id": "load"},
            "auth": {"login_per_min": 100000000, "register_per_hour": 100000000, "join_per_min": 100000000,
                     "has_joined_per_min": 100000000},
            "finder": {"submit_per_min": 100000000},
            "accounts": {"password_iterations": 10000, "login_throttle_s": 0, "leaderboard_cache_s": 1,
                         "skins_file": os.path.join(root, "assets", "skins.json")},
        }
        if use_redis:
            server = os.environ.get("KN_TEST_REDIS_SERVER") or shutil.which("valkey-server") or shutil.which("redis-server")
            if not server:
                print("%s: no redis server, skipped" % mode)
                return None
            port, pw = free_port(), secrets.token_hex(12)
            rp = subprocess.Popen([server, "-"], stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
                                  stderr=subprocess.DEVNULL, cwd=tmp)
            rp.stdin.write(("port %d\nbind 127.0.0.1\nsave \"\"\nappendonly no\nrequirepass %s\n" % (port, pw)).encode())
            rp.stdin.close()
            procs.append(rp)
            for _ in range(200):
                try:
                    socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
                    break
                except OSError:
                    time.sleep(0.02)
            cfg["redis"] = {"url": "redis://127.0.0.1:%d" % port, "password": pw}
        path = os.path.join(tmp, mode.replace("+", "_") + ".json")
        with open(path, "w") as fp:
            json.dump(cfg, fp)
        log_path = path + ".log"
        log = open(log_path, "w")
        env = {k: v for k, v in os.environ.items() if not k.startswith("KN_REDIS_")}
        bp = subprocess.Popen([brain, "--config", path, "--migrate"], stdout=log, stderr=subprocess.STDOUT,
                              env=dict(env, KN_LOG_LEVEL="info"), cwd=root)
        procs.append(bp)
        port = None
        for _ in range(1200):
            m = re.search(r"listening on http://127\.0\.0\.1:(\d+) \(ws", open(log_path).read())
            if m:
                port = int(m.group(1))
                break
            time.sleep(0.05)
        if not port:
            raise RuntimeError("brain did not start: " + open(log_path).read()[-2000:])
        tokens = []
        for i in range(a.users):
            st, r = post(port, "/auth/register", {"username": "load%04d" % i, "password": "hunter22"})
            if st != 200:
                raise RuntimeError("register: %s %r" % (st, r))
            tokens.append(r["token"])
        files = {}
        for key, lines in (("session", ["/auth/me?token=" + t for t in tokens]), ("finder", ["/servers"]),
                           ("presence", ["/mm/presence"]), ("login", ["/auth/login"])):
            files[key] = os.path.join(tmp, "%s_%s.paths" % (mode.replace("+", "_"), key))
            with open(files[key], "w") as fp:
                fp.write("\n".join(lines) + "\n")
        files["bodies"] = os.path.join(tmp, "%s_login.bodies" % mode.replace("+", "_"))
        with open(files["bodies"], "w") as fp:  # every connection logs in other accounts (no row-lock queue)
            fp.write("\n".join(json.dumps({"username": "load%04d" % i, "password": "hunter22"})
                               for i in range(a.users)) + "\n")
        common = ["--port", str(port), "--seconds", str(a.seconds), "--warmup", "2"]
        runs = {
            "session": ["--paths", files["session"], "--threads", str(a.threads), "--conns", str(a.conns)],
            "login": ["--paths", files["login"], "--method", "POST", "--threads", "1", "--conns", "8",
                      "--bodies", files["bodies"]],
            "finder": ["--paths", files["finder"], "--threads", str(a.threads), "--conns", str(a.conns)],
            "presence": ["--paths", files["presence"], "--threads", str(a.threads), "--conns", str(a.conns)],
            "leaderboard": ["--ws", "/social/ws?enc=json", "--frame", '["r","leaders","player_score",0,"",0,0]',
                            "--reply", "0", "--threads", str(a.threads), "--conns", str(max(1, a.conns * 2)),
                            "--interval-ms", "55"],
        }
        res = {}
        for key, args in runs.items():
            out = subprocess.run([loadgen] + common + args, capture_output=True, text=True, timeout=a.seconds + 60)
            try:
                res[key] = json.loads(out.stdout.strip().splitlines()[-1])
            except (ValueError, IndexError):
                res[key] = {"error": out.stderr[-300:]}
            print("  %-15s %-12s %s" % (mode, key, json.dumps(res[key])), flush=True)
        if use_redis and "hot tier on" not in open(log_path).read():
            print("  WARNING: %s ran without Redis" % mode)
        return res
    finally:
        for p in reversed(procs):
            p.terminate()
            try:
                p.wait(10)
            except subprocess.TimeoutExpired:
                p.kill()
        subprocess.run(scratch + ["drop", driver, name], capture_output=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("build")
    ap.add_argument("root")
    ap.add_argument("--modes", default="sqlite,postgres,postgres+redis,sqlite+redis")
    ap.add_argument("--seconds", type=int, default=10)
    ap.add_argument("--conns", type=int, default=64)
    ap.add_argument("--threads", type=int, default=2)
    ap.add_argument("--users", type=int, default=200)
    ap.add_argument("--json")
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    brain, loadgen = find(a.build, "kn_brain"), find(a.build, "kn_loadgen")
    tmp = tempfile.mkdtemp(prefix="kn-load-")
    results = {}
    try:
        for mode in a.modes.split(","):
            print("== %s" % mode, flush=True)
            results[mode] = run_mode(mode, a, root, brain, loadgen, tmp)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    keys = ["session", "login", "finder", "presence", "leaderboard"]
    print("\n%-16s" % "req/s | p99 ms" + "".join("%22s" % k for k in keys))
    for mode, res in results.items():
        if not res:
            continue
        cells = ["%10.0f | %7.2f" % (res[k].get("rps", 0), res[k].get("p99_ms", 0)) for k in keys]
        print("%-16s" % mode + "".join("%22s" % c for c in cells))
    if a.json:
        with open(a.json, "w") as fp:
            json.dump(results, fp, indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
