#!/usr/bin/env python3
"""kn_brain's mirror of Krunker's user-asset host (services/brain/user_assets.c,
GET /api/user-assets/<id>/<file>) against a stub host (this file's http.server on 127.0.0.1;
never user-assets.krunker.io).

  user_assets_test.py <kn_brain> <repo root>

Checks: a file of the brain's asset tree (accounts.assets_dir/user-assets/) is answered without asking
the host; a missing one is fetched from user_assets.upstream, answered byte for byte with its content
type and Cache-Control, written into user_assets.cache_dir, and answered from there afterwards (also by
a restarted brain: no second request); /user-assets/ (no /api) answers the same; concurrent requests for
one file make one upstream request; paths outside shared/include/user_asset_path.h's rules (an id with
letters, an unknown file name, dot segments, an encoded slash) answer 404 without reaching the host;
an upstream 404 answers 404 and is remembered (no second request); an HTML page answered 200 (a CDN's
block page), a body over the size cap and a redirect (never followed) answer 502 and nothing is cached;
user_assets.enabled=false answers 503. Proxy: with user_assets.upstream on a host name only a stub
forward proxy knows, KN_USER_ASSETS_PROXY (what the deploy writes from the krunkbox .env) and, without
it, KN_FETCH_PROXY (the local-run convention) send the fetch through the proxy (absolute-URI GET);
with neither the fetch fails (502).

Database: as streams_test.py (sqlite here). 77 = skip (no database driver).
"""
import concurrent.futures
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

BRAIN, ROOT = sys.argv[1], sys.argv[2]
HERE = os.path.dirname(os.path.abspath(__file__))
DRIVER = os.environ.get("KN_TEST_DB_DRIVER", "sqlite")
SCHEMA = "kn_test_uassets_%d" % os.getpid()
FAILS = []

PNG_1x1 = bytes.fromhex(
    "89504e470d0a1a0a0000000d4948445200000001000000010806000000"
    "1f15c4890000000d49444154789c63f8cfc0f01f0005000201a1e8e4e50000000049454e44ae426082")
OBJ = b"v 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 0\nvt 1 0\nvt 0 1\nf 1/1 2/2 3/3\n"


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what, flush=True)
    if not cond:
        FAILS.append(what)


class Host:
    """The stub user-asset host. Counts every request by path."""

    def __init__(self):
        self.hits = {}
        self.lock = threading.Lock()
        self.files = {
            "/9356/texture.png": ("image/png", PNG_1x1 + b"\0" * 32),
            "/7282/model.obj": ("application/x-tgif", OBJ),
            "/md43867/mod.zip": ("application/zip", b"PK\x05\x06" + b"\0" * 18),
            "/5000/texture.png": ("text/html", b"<!DOCTYPE html><html><title>Krunker.io</title>Access denied</html>"),
            "/5001/texture.png": ("image/png", PNG_1x1 + b"\0" * (33 << 20)),
            "/5003/texture.png": ("image/png", PNG_1x1),  # slow: concurrent requests wait for one fetch
        }
        self.srv = ThreadingHTTPServer(("127.0.0.1", 0), self.handler())
        self.url = "http://127.0.0.1:%d/" % self.srv.server_address[1]
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()

    def count(self, path):
        with self.lock:
            return self.hits.get(path, 0)

    def total(self):
        with self.lock:
            return sum(self.hits.values())

    def handler(self):
        host = self

        class H(BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def do_GET(self):
                with host.lock:
                    host.hits[self.path] = host.hits.get(self.path, 0) + 1
                if self.path == "/5002/texture.png":
                    self.send_response(302)
                    self.send_header("Location", host.url + "9356/texture.png")
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    return
                if self.path == "/5003/texture.png":
                    time.sleep(0.6)
                f = host.files.get(self.path)
                if not f:
                    self.send_response(404)
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    return
                self.send_response(200)
                self.send_header("Content-Type", f[0])
                self.send_header("Content-Length", str(len(f[1])))
                self.end_headers()
                try:
                    self.wfile.write(f[1])
                except OSError:
                    pass

        return H


class Proxy:
    """A stub HTTP forward proxy: GET http://upstream.invalid/<path> is answered from `host` (the stub
    user-asset host). Records the absolute URIs it saw."""

    def __init__(self, host):
        self.seen = []
        proxy = self

        class H(BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def do_GET(self):
                proxy.seen.append(self.path)
                prefix = "http://upstream.invalid/"
                if not self.path.startswith(prefix):
                    self.send_response(502)
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    return
                try:
                    with urllib.request.urlopen(host.url + self.path[len(prefix):], timeout=10) as r:
                        status, body = r.status, r.read()
                except urllib.error.HTTPError as e:
                    status, body = e.code, e.read()
                self.send_response(status)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

        self.srv = ThreadingHTTPServer(("127.0.0.1", 0), H)
        self.url = "http://127.0.0.1:%d" % self.srv.server_address[1]
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()


def bound_port(log_path, proc):
    for _ in range(300):
        if proc.poll() is not None:
            raise RuntimeError("kn_brain exited early (%s)" % log_path)
        with open(log_path) as fp:
            m = re.search(r"listening on http://127\.0\.0\.1:(\d+) \(ws", fp.read())
        if m:
            return int(m.group(1))
        time.sleep(0.05)
    raise RuntimeError("kn_brain did not start listening")


def get(port, path, timeout=20):
    req = urllib.request.Request("http://127.0.0.1:%d%s" % (port, path))
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, r.read(), dict(r.headers)
    except urllib.error.HTTPError as e:
        return e.code, e.read(), dict(e.headers)


def run_brain(env, work, host, tag, more=(), upstream=None, cache="cache"):
    log_path = os.path.join(work, "brain-%s.log" % tag)
    extra = ["--brain.host=127.0.0.1", "--brain.port=0", "--brain.id=uassets-%d" % os.getpid(),
             "--accounts.email.required=false", "--smtp.host=",
             "--accounts.assets_dir=%s" % os.path.join(work, "tree"),
             "--user_assets.upstream=%s" % (upstream or host.url),
             "--user_assets.cache_dir=%s" % os.path.join(work, cache)] + list(more)
    with open(log_path, "a") as log:
        proc = subprocess.Popen([BRAIN] + extra, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT)
    return proc, log_path


def scenario_mirror(port, host, work):
    st, body, hd = get(port, "/api/user-assets/2154/model.png")
    check(st == 200 and body == b"tree-copy", "a file of the asset tree is answered from there")
    check(host.total() == 0, "the tree's file never reaches the host")

    st, body, hd = get(port, "/api/user-assets/9356/texture.png")
    check(st == 200 and body == host.files["/9356/texture.png"][1], "a missing texture is fetched and answered byte for byte")
    check(hd.get("Content-Type", "").startswith("image/png"), "image/png (%s)" % hd.get("Content-Type"))
    check("public" in hd.get("Cache-Control", ""), "Cache-Control public (%s)" % hd.get("Cache-Control"))
    cached = os.path.join(work, "cache", "9356", "texture.png")
    check(os.path.exists(cached) and open(cached, "rb").read() == body, "written into user_assets.cache_dir")
    st2, body2, _ = get(port, "/api/user-assets/9356/texture.png")
    check(st2 == 200 and body2 == body and host.count("/9356/texture.png") == 1, "the second answer comes from the cache")
    st, body, hd = get(port, "/user-assets/7282/model.obj")
    check(st == 200 and body == OBJ, "/user-assets/ (no /api) answers too")
    st, body, _ = get(port, "/api/user-assets/md43867/mod.zip")
    check(st == 200 and body.startswith(b"PK"), "a resource pack (md<id>/mod.zip)")

    with concurrent.futures.ThreadPoolExecutor(6) as ex:
        res = list(ex.map(lambda _: get(port, "/api/user-assets/5003/texture.png"), range(6)))
    check(all(r[0] == 200 and r[1] == PNG_1x1 for r in res), "six concurrent requests all answered")
    check(host.count("/5003/texture.png") == 1, "... with one upstream request (%d)" % host.count("/5003/texture.png"))

    before = host.total()
    for bad in ("/api/user-assets/12a/texture.png", "/api/user-assets/9356/passwd", "/api/user-assets/../etc/passwd",
                "/api/user-assets/9356/..%2Ftexture.png", "/api/user-assets/9356%2F..%2F9357/texture.png",
                "/api/user-assets/p123/profile.png", "/api/user-assets/9356/texture.png.html"):
        st, _, _ = get(port, bad)
        check(st == 404, "%s: 404 (%d)" % (bad, st))
    check(host.total() == before, "refused paths never reach the host")

    st, _, _ = get(port, "/api/user-assets/4040/texture.png")
    check(st == 404, "an upstream 404 answers 404")
    st, _, _ = get(port, "/api/user-assets/4040/texture.png")
    check(st == 404 and host.count("/4040/texture.png") == 1, "... and is remembered (one upstream request)")

    for path, what in (("5000", "an HTML page answered 200"), ("5001", "a body over the cap"), ("5002", "a redirect")):
        st, _, _ = get(port, "/api/user-assets/%s/texture.png" % path)
        check(st == 502, "%s: 502 (%d)" % (what, st))
        check(not os.path.exists(os.path.join(work, "cache", path, "texture.png")), "%s: nothing cached" % what)
    check(host.count("/9356/texture.png") == 1, "the redirect was not followed")


def main():
    scratch = [sys.executable, "-I", os.path.join(HERE, "db_scratch.py"), ROOT]
    r = subprocess.run(scratch + ["create", DRIVER, SCHEMA], capture_output=True)
    if r.returncode:
        print("user assets test: %s unavailable: skipped" % DRIVER)
        sys.exit(r.returncode)
    dbenv = {"KN_DATABASE_" + k.upper(): str(v) for k, v in json.loads(r.stdout).items()}
    work = tempfile.mkdtemp(prefix="kn-uassets.", dir=os.path.join(os.path.expanduser("~"), ".cache"))
    os.makedirs(os.path.join(work, "tree", "user-assets", "2154"))
    with open(os.path.join(work, "tree", "user-assets", "2154", "model.png"), "wb") as f:
        f.write(b"tree-copy")
    host = Host()
    procs = []
    try:
        env = {k: v for k, v in os.environ.items()
               if not k.startswith("KN_USER_ASSETS") and k != "KN_FETCH_PROXY" and not k.lower().endswith("_proxy")}
        env.update(dbenv, KN_BRAIN_INTERNAL_SECRET="kn-uassets-test-%d" % os.getpid())
        subprocess.run([BRAIN, "--migrate-only"], cwd=ROOT, env=env, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

        print("mirror (stub host):")
        proc, log_path = run_brain(env, work, host, "mirror")
        procs.append(proc)
        scenario_mirror(bound_port(log_path, proc), host, work)
        proc.terminate()
        proc.wait(10)

        print("restarted brain:")
        proc, log_path = run_brain(env, work, host, "restart")
        procs.append(proc)
        port = bound_port(log_path, proc)
        st, body, _ = get(port, "/api/user-assets/9356/texture.png")
        check(st == 200 and host.count("/9356/texture.png") == 1, "the disk cache answers a restarted brain")
        proc.terminate()
        proc.wait(10)

        proxy = Proxy(host)
        for tag, var in (("proxy", "KN_USER_ASSETS_PROXY"), ("fetchproxy", "KN_FETCH_PROXY")):
            print("through a proxy (%s):" % var)
            proc, log_path = run_brain(dict(env, **{var: proxy.url}), work, host, tag, upstream="http://upstream.invalid/",
                                       cache="cache-" + tag)
            procs.append(proc)
            before = len(proxy.seen)
            st, body, _ = get(bound_port(log_path, proc), "/api/user-assets/7282/model.obj")
            check(st == 200 and body == OBJ, "%s: fetched through the proxy" % var)
            check(proxy.seen[before:] == ["http://upstream.invalid/7282/model.obj"], "%s: the proxy saw the request (%s)" % (var, proxy.seen[before:]))
            proc.terminate()
            proc.wait(10)
        print("no proxy, the same unreachable upstream:")
        proc, log_path = run_brain(env, work, host, "noproxy", upstream="http://upstream.invalid/", cache="cache-noproxy")
        procs.append(proc)
        before = len(proxy.seen)
        st, _, _ = get(bound_port(log_path, proc), "/api/user-assets/7282/model.obj")
        check(st == 502 and len(proxy.seen) == before, "502, the proxy not asked (%d)" % st)
        proc.terminate()
        proc.wait(10)
        proxy.srv.shutdown()

        print("off (user_assets.enabled=false):")
        proc, log_path = run_brain(env, work, host, "off", ["--user_assets.enabled=false"])
        procs.append(proc)
        st, _, _ = get(bound_port(log_path, proc), "/api/user-assets/9356/texture.png")
        check(st == 503, "503 (%d)" % st)
        proc.terminate()
    except Exception as e:  # noqa: BLE001
        import traceback
        traceback.print_exc()
        check(False, "exception: %r" % e)
    finally:
        for p in procs:
            if p.poll() is None:
                p.terminate()
        host.srv.shutdown()
        subprocess.run(scratch + ["drop", DRIVER, SCHEMA], capture_output=True)
    if FAILS:
        print("user assets test: %d FAILED (logs in %s)" % (len(FAILS), work))
        sys.exit(1)
    shutil.rmtree(work, ignore_errors=True)
    print("user assets test: ok")


if __name__ == "__main__":
    main()
