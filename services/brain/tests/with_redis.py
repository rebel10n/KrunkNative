#!/usr/bin/env python3
"""Run a brain test with the Redis hot tier on (doc/backend.md 3.11).

  with_redis.py <command> [args...]

Starts a throwaway redis-server / valkey-server (KN_TEST_REDIS_SERVER, else valkey-server or
redis-server on PATH) on a free loopback port with a random password and no persistence,
runs the command with KN_REDIS_URL / KN_REDIS_PASSWORD (every kn_brain it starts then uses
the hot tier), and stops the server. The run fails when the brains never talked to Redis
(so a test cannot silently pass on the database path alone). Exit 77 (CTest skip) without
a server binary; otherwise the command's exit code.
"""
import os
import secrets
import shutil
import socket
import subprocess
import sys
import tempfile
import time


def find_server():
    env = os.environ.get("KN_TEST_REDIS_SERVER")
    if env:
        return env
    return shutil.which("valkey-server") or shutil.which("redis-server")


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def resp(port, password, *cmd):
    """One RESP command on a fresh connection (AUTH first); returns the raw reply text."""
    with socket.create_connection(("127.0.0.1", port), timeout=5) as s:
        def send(*args):
            out = b"*%d\r\n" % len(args)
            for a in args:
                a = a.encode() if isinstance(a, str) else a
                out += b"$%d\r\n%s\r\n" % (len(a), a)
            s.sendall(out)
        send("AUTH", password)
        send(*cmd)
        data = b""
        deadline = time.time() + 5
        while time.time() < deadline:
            chunk = s.recv(65536)
            if not chunk:
                break
            data += chunk
            if data.startswith(b"+OK\r\n") and len(data) > 5:
                body = data[5:]
                if body[:1] in (b"+", b"-", b":") and body.endswith(b"\r\n"):
                    break
                if body[:1] == b"$":
                    n = int(body[1:body.index(b"\r\n")])
                    if n < 0 or len(body) >= body.index(b"\r\n") + 2 + n + 2:
                        break
        return data[5:].decode(errors="replace") if data.startswith(b"+OK\r\n") else data.decode(errors="replace")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    server = find_server()
    if not server:
        print("no redis-server / valkey-server (KN_TEST_REDIS_SERVER): skip")
        return 77
    port = free_port()
    password = secrets.token_hex(16)
    tmp = tempfile.mkdtemp(prefix="kn-redis-test-")
    log = open(os.path.join(tmp, "redis.log"), "w")
    # the password goes through a config file on stdin, never argv (ps)
    proc = subprocess.Popen([server, "-"], stdin=subprocess.PIPE, stdout=log, stderr=subprocess.STDOUT, cwd=tmp)
    proc.stdin.write(("port %d\nbind 127.0.0.1\nsave \"\"\nappendonly no\ndir %s\nrequirepass %s\n"
                      % (port, tmp, password)).encode())
    proc.stdin.close()
    for _ in range(300):
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
            break
        except OSError:
            if proc.poll() is not None:
                break
            time.sleep(0.02)
    else:
        proc.kill()
        print("redis server did not start: skip")
        return 77
    if proc.poll() is not None:
        print("redis server exited: skip")
        return 77
    env = dict(os.environ, KN_REDIS_URL="redis://127.0.0.1:%d" % port, KN_REDIS_PASSWORD=password,
               KN_TEST_REDIS_PORT=str(port))
    rc = 1
    try:
        rc = subprocess.call(sys.argv[1:], env=env)
        if rc == 0:
            info = resp(port, password, "INFO", "commandstats")
            evals = sum(int(line.split("calls=")[1].split(",")[0]) for line in info.splitlines()
                        if line.startswith(("cmdstat_eval:", "cmdstat_publish:", "cmdstat_set:")))
            print("redis: %d EVAL/PUBLISH/SET calls from the brains" % evals)
            if evals == 0:
                print("FAIL: the brains never used the Redis hot tier")
                rc = 1
    finally:
        proc.terminate()
        try:
            proc.wait(10)
        except subprocess.TimeoutExpired:
            proc.kill()
        log.close()
        shutil.rmtree(tmp, ignore_errors=True)
    return rc


if __name__ == "__main__":
    sys.exit(main())
