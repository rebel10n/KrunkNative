#!/usr/bin/env python3
"""Redis hot tier end to end, including Redis dying mid-run (doc/backend.md 3.11).

  redis_failover_test.py <kn_brain> <repo root>

Two brains (auth + community) share one database (KN_TEST_DB_DRIVER: sqlite default,
postgres, mysql; services/brain/tests/db_scratch.py) and one throwaway redis-server /
valkey-server of their own:

  * session cache: /auth/me fills <p>sess:<sha256(token)>; the other brain answers from it;
    logout and a password change drop it (the old token is refused on both brains at once);
  * join records: /auth/join on brain A, /auth/hasJoined on brain B;
  * rate limits: auth.login_per_min counts the hits of both brains together;
  * presence + pushes: a social socket on A counts in B's /mm/presence, and a chat sent by a
    player on B reaches the recipient's socket on A;
  * kill -9 of Redis while both brains are hammered: no request fails, both log "down: serving
    from the database", sessions / logins / chat keep working; Redis restarted: both reconnect,
    presence is re-registered and the cache fills again.
Skips (77) without a redis server binary, python websockets or the database.
"""
import asyncio
import hashlib
import http.client
import json
import os
import re
import secrets
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time

try:
    import websockets
except ImportError:
    print("python websockets missing: skip")
    sys.exit(77)

FAILS = []


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what, flush=True)
    if not cond:
        FAILS.append(what)


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Redis:
    def __init__(self, server, tmp):
        self.server, self.tmp = server, tmp
        self.port, self.password, self.proc = free_port(), secrets.token_hex(12), None

    def start(self):
        self.log = open(os.path.join(self.tmp, "redis.log"), "a")
        self.proc = subprocess.Popen([self.server, "-"], stdin=subprocess.PIPE, stdout=self.log,
                                     stderr=subprocess.STDOUT, cwd=self.tmp)
        self.proc.stdin.write(("port %d\nbind 127.0.0.1\nsave \"\"\nappendonly no\ndir %s\nrequirepass %s\n"
                               % (self.port, self.tmp, self.password)).encode())
        self.proc.stdin.close()
        for _ in range(300):
            try:
                socket.create_connection(("127.0.0.1", self.port), timeout=0.2).close()
                return True
            except OSError:
                time.sleep(0.02)
        return False

    def kill(self):
        self.proc.send_signal(signal.SIGKILL)
        self.proc.wait(10)
        self.log.close()

    def cmd(self, *args):
        """Tiny RESP client: AUTH + one command; returns the decoded reply."""
        with socket.create_connection(("127.0.0.1", self.port), timeout=5) as s:
            f = s.makefile("rb")
            for c in (("AUTH", self.password), args):
                out = b"*%d\r\n" % len(c)
                for a in c:
                    a = str(a).encode()
                    out += b"$%d\r\n%s\r\n" % (len(a), a)
                s.sendall(out)
            def read():
                line = f.readline().rstrip(b"\r\n")
                t, rest = line[:1], line[1:]
                if t in (b"+", b"-"):
                    return rest.decode()
                if t == b":":
                    return int(rest)
                if t == b"$":
                    n = int(rest)
                    if n < 0:
                        return None
                    v = f.read(n + 2)[:-2]
                    return v.decode(errors="replace")
                if t == b"*":
                    return [read() for _ in range(int(rest))]
                raise ValueError(line)
            read()
            return read()


class Brain:
    def __init__(self, exe, root, tmp, name, dbcfg, redis, secret):
        self.name, self.log_path = name, os.path.join(tmp, name + ".log")
        cfg = {
            "database": dict(dbcfg, migrations_dir=os.path.join(root, "services", "db", "migrations")),
            "brain": {"host": "127.0.0.1", "port": 0, "id": name, "internal_secret": secret, "federation": True},
            "auth": {"login_per_min": 4},
            "accounts": {"password_iterations": 10000, "login_throttle_s": 0, "starting_kr": 100,
                         "skins_file": os.path.join(root, "assets", "skins.json")},
            "redis": {"url": "redis://127.0.0.1:%d" % redis.port, "password": redis.password, "prefix": "knt:",
                      "presence_ttl_s": 9},
        }
        path = os.path.join(tmp, name + ".json")
        with open(path, "w") as fp:
            json.dump(cfg, fp)
        self.log = open(self.log_path, "w")
        env = {k: v for k, v in os.environ.items() if not k.startswith("KN_REDIS_")}
        self.proc = subprocess.Popen([exe, "--config", path, "--migrate", "--role", "auth,community"],
                                     stdout=self.log, stderr=subprocess.STDOUT, env=env, cwd=root)
        self.port = None
        for _ in range(1200):
            if self.proc.poll() is not None:
                raise RuntimeError(name + " exited early")
            m = re.search(r"listening on http://127\.0\.0\.1:(\d+) \(ws", self.text())
            if m:
                self.port = int(m.group(1))
                break
            time.sleep(0.05)
        if not self.port:
            raise RuntimeError(name + " did not start")

    def text(self):
        with open(self.log_path) as fp:
            return fp.read()

    def http(self, method, path, body=None, timeout=10):
        c = http.client.HTTPConnection("127.0.0.1", self.port, timeout=timeout)
        data = json.dumps(body) if body is not None else None
        c.request(method, path, body=data, headers={"Content-Type": "application/json"} if data else {})
        r = c.getresponse()
        raw = r.read()
        c.close()
        try:
            return r.status, json.loads(raw) if raw else None
        except ValueError:
            return r.status, raw

    def stop(self):
        self.proc.terminate()
        try:
            self.proc.wait(10)
        except subprocess.TimeoutExpired:
            self.proc.kill()
        self.log.close()


class Social:
    def __init__(self, url):
        self.url, self.inbox = url + "?enc=json", []

    async def __aenter__(self):
        self.ws = await websockets.connect(self.url)
        return self

    async def __aexit__(self, *a):
        await self.ws.close()

    async def recv(self, label, timeout=10.0):
        deadline = time.time() + timeout
        while True:
            for i, f in enumerate(self.inbox):
                if f[0] == label:
                    return self.inbox.pop(i)
            left = deadline - time.time()
            if left <= 0:
                raise AssertionError("timeout waiting for %r" % label)
            self.inbox.append(json.loads(await asyncio.wait_for(self.ws.recv(), left)))

    async def call(self, reply, *frame):
        await self.ws.send(json.dumps(list(frame)))
        return await self.recv(reply)


def wait_for(fn, timeout=10.0, step=0.1):
    end = time.time() + timeout
    while time.time() < end:
        v = fn()
        if v:
            return v
        time.sleep(step)
    return fn()


def sess_key(token):
    return "knt:sess:" + hashlib.sha256(token.encode()).hexdigest()


async def scenario(a, b, redis):
    u1, u2 = "rt_%s" % secrets.token_hex(3), "rt_%s" % secrets.token_hex(3)
    st, r = a.http("POST", "/auth/register", {"username": u1, "password": "hunter22"})
    check(st == 200 and r.get("ok"), "register on A")
    tok = r["token"]
    st, r = b.http("POST", "/auth/register", {"username": u2, "password": "hunter22"})
    check(st == 200, "register on B")
    tok2 = r["token"]

    # ---- session cache
    st, _ = a.http("GET", "/auth/me?token=" + tok)
    check(st == 200, "/auth/me on A (database, fills the cache)")
    check(wait_for(lambda: redis.cmd("EXISTS", sess_key(tok)) == 1, 3), "session cached in Redis (hash, not token)")
    keys = redis.cmd("KEYS", "knt:*")
    check(not any(tok in k for k in keys), "the raw token is in no Redis key")
    st, me = b.http("GET", "/auth/me?token=" + tok)
    check(st == 200 and me["account"]["username"] == u1, "/auth/me on B (served from the shared cache)")

    # ---- join record across brains
    sh = hashlib.sha256(b"server-" + tok.encode()).hexdigest()
    st, _ = a.http("POST", "/auth/join", {"token": tok, "serverHash": sh})
    check(st == 200, "/auth/join on A")
    st, r = b.http("GET", "/auth/hasJoined?username=%s&serverHash=%s" % (u1, sh))
    check(st == 200 and r.get("username") == u1, "/auth/hasJoined on B finds A's join record")
    st, _ = b.http("GET", "/auth/hasJoined?username=%s&serverHash=%s" % (u2, sh))
    check(st == 204, "hasJoined with another username: 204")

    # ---- presence + push across brains
    async with Social("ws://127.0.0.1:%d/social/ws" % a.port) as sa, \
            Social("ws://127.0.0.1:%d/social/ws" % b.port) as sb:
        r = await sa.call("a", "a", 1, [u1, None, tok])
        check(r[1] is None, "social login on A")
        r = await sb.call("a", "a", 1, [u2, None, tok2])
        check(r[1] is None, "social login on B")
        st, p = b.http("GET", "/mm/presence")
        check(st == 200 and p["redis"] == "up" and p["local"] == 1, "/mm/presence on B: one local socket, redis up")
        check(wait_for(lambda: b.http("GET", "/mm/presence")[1]["global"] == 2, 3), "global presence 2 (both brains)")
        st, me = a.http("GET", "/auth/me?token=" + tok)
        u1_id = me["account"]["id"]
        check(redis.cmd("GET", sess_key(tok)) == str(u1_id), "cached value is the account id")
        r = await sb.call("chf", "ict", u1_id, "hello across brains")
        check(r[1] is True, "chat sent on B")
        push = await sa.recv("chtb", 5)
        check(push[1][0]["fna"] == u2, "chat push reached the recipient's socket on A")

        # ---- rate limit: 4 logins per minute per IP over both brains
        codes = []
        for i in range(3):
            codes.append(a.http("POST", "/auth/login", {"username": u1, "password": "wrong!"})[0])
        time.sleep(0.3)  # the shared count is updated asynchronously
        for i in range(3):
            codes.append(b.http("POST", "/auth/login", {"username": u1, "password": "wrong!"})[0])
            time.sleep(0.1)
        check(codes[:3] == [401, 401, 401] and 429 in codes[3:], "login limit shared by both brains %r" % codes)

        # ---- kill -9 Redis while both brains are hammered
        stop, errors, done = threading.Event(), [], [0]

        def hammer(brain):
            while not stop.is_set():
                for path in ("/auth/me?token=" + tok2, "/mm/presence", "/mm/status"):
                    try:
                        st, _ = brain.http("GET", path, timeout=5)
                        if st != 200:
                            errors.append((brain.name, path, st))
                    except Exception as e:  # noqa: BLE001
                        errors.append((brain.name, path, repr(e)))
                    done[0] += 1
        threads = [threading.Thread(target=hammer, args=(x,)) for x in (a, b) for _ in range(2)]
        for t in threads:
            t.start()
        time.sleep(1.0)
        before = done[0]
        redis.kill()
        t_kill = time.time()
        time.sleep(3.0)
        stop.set()
        for t in threads:
            t.join()
        check(not errors, "no request failed while Redis died (%d requests, %d after the kill) %r"
              % (done[0], done[0] - before, errors[:3]))
        check(all("down: serving from the database" in x.text() for x in (a, b)), "both brains noticed Redis down")
        st, p = a.http("GET", "/mm/presence")
        check(st == 200 and p["redis"] == "down" and p["global"] is None, "/mm/presence without Redis")
        st, _ = b.http("GET", "/auth/me?token=" + tok)
        check(st == 200, "session check from the database while Redis is down")
        st, r = b.http("POST", "/auth/login", {"username": u2, "password": "hunter22"})
        check(st == 200, "password login while Redis is down")
        st, me2 = b.http("GET", "/auth/me?token=" + tok2)
        r = await sa.call("chf", "ict", me2["account"]["id"], "still here")
        check(r[1] is True, "chat while Redis is down")

        # ---- restart: both reconnect, presence comes back, the cache fills again
        check(redis.start(), "redis restarted")
        check(wait_for(lambda: all(x.text().count("hot tier on") >= 2 for x in (a, b)), 10), "both brains reconnected")
        print("     reconnected %.1f s after the kill" % (time.time() - t_kill))
        check(wait_for(lambda: a.http("GET", "/mm/presence")[1].get("global") == 2, 5),
              "presence re-registered after the restart")
        a.http("GET", "/auth/me?token=" + tok)
        check(wait_for(lambda: redis.cmd("EXISTS", sess_key(tok)) == 1, 3), "session cache fills again")
        r = await sb.call("chf", "ict", u1_id, "after restart")
        push = await sa.recv("chtb", 5)
        check(push[1][0]["fna"] == u2, "cross-brain push works again")

    # ---- revocation: logout on A drops the cached session for B too
    st, _ = a.http("POST", "/auth/logout", {"token": tok})
    check(wait_for(lambda: redis.cmd("EXISTS", sess_key(tok)) == 0, 3), "logout drops the cached session")
    st, _ = b.http("GET", "/auth/me?token=" + tok)
    check(st == 401, "logged-out token refused on B at once")
    # password change revokes every older session (social "upp")
    st, r = a.http("POST", "/auth/login", {"username": u2, "password": "hunter22"})
    if st == 429:
        time.sleep(1)
    async with Social("ws://127.0.0.1:%d/social/ws" % a.port) as sa:
        r = await sa.call("a", "a", 1, [u2, None, tok2])
        b.http("GET", "/auth/me?token=" + tok2)
        check(wait_for(lambda: redis.cmd("EXISTS", sess_key(tok2)) == 1, 3), "second session cached")
        r = await sa.call("upp", "upp", "hunter22", "hunter33", "hunter33")
        check(r[1] == "Password updated.", "password changed")
    st, _ = b.http("GET", "/auth/me?token=" + tok2)
    check(st == 401, "token from before the password change refused on B at once")


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    exe, root = sys.argv[1], os.path.abspath(sys.argv[2])
    server = os.environ.get("KN_TEST_REDIS_SERVER") or shutil.which("valkey-server") or shutil.which("redis-server")
    if not server:
        print("no redis-server / valkey-server: skip")
        return 77
    driver = os.environ.get("KN_TEST_DB_DRIVER", "sqlite")
    name = "kn_test_rf_%d" % os.getpid()
    scratch = [sys.executable, "-I", os.path.join(root, "services", "brain", "tests", "db_scratch.py"), root]
    r = subprocess.run(scratch + ["create", driver, name], capture_output=True)
    if r.returncode:
        print("%s unavailable: skip" % driver)
        return r.returncode
    dbcfg = json.loads(r.stdout)
    tmp = tempfile.mkdtemp(prefix="kn-redis-failover-")
    redis = Redis(server, tmp)
    brains, rc = [], 1
    try:
        if not redis.start():
            print("redis did not start: skip")
            return 77
        secret = "rf-" + secrets.token_hex(8)
        brains.append(Brain(exe, root, tmp, "rfa", dbcfg, redis, secret))
        brains.append(Brain(exe, root, tmp, "rfb", dbcfg, redis, secret))
        asyncio.run(scenario(brains[0], brains[1], redis))
        for x in brains:
            errs = [l for l in x.text().splitlines() if " ERROR " in l or "FATAL" in l]
            check(not errs, "%s logged no errors %r" % (x.name, errs[:3]))
        rc = 1 if FAILS else 0
    except Exception as e:  # noqa: BLE001
        print("FAIL exception: %r" % (e,))
    finally:
        for x in brains:
            x.stop()
        try:
            redis.proc.kill()
        except Exception:  # noqa: BLE001
            pass
        if rc:
            for x in brains:
                print("---- %s log ----\n%s" % (x.name, x.text()[-5000:]))
        subprocess.run(scratch + ["drop", driver, name], capture_output=True)
        shutil.rmtree(tmp, ignore_errors=True)
    print("%d failure(s)" % len(FAILS))
    return rc


if __name__ == "__main__":
    sys.exit(main())
