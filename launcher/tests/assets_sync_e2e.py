#!/usr/bin/env python3
"""Game content sync end to end (launcher/src/assets_sync.c) against local HTTP/1.1 servers.

    python3 -I launcher/tests/assets_sync_e2e.py <assets_bench binary> [--keep]

Two origins on free ports (kn_proto.free_ports): "own" serves the updates store
(updates/client/latest.json, the content list, objects/<aa>/<sha256>), "cdn" a by-path copy of
the tree (the Pages CDN layout). Both log every request and connection and can inject faults.

  1  fresh sync from both origins: every file right, both origins used, connections kept alive
     (far fewer connections than files) and small files pipelined; duplicate content once
  2  second run: up to date, no object requests
  3  no state file: the hash cache vouches for the tree, nothing hashed or fetched
  4  resume: a cut-off download leaves its .part, the next run asks for a Range and finishes it
  5  hash mismatch on the mirror: the file comes from the other origin, the mirror is counted
  6  origin fallback: the mirror refuses every connection, all files come from the objects
  7  missing everywhere: 404 on both fails the sync with "missing on the update server"
  8  cancel mid-download: the sync stops with "cancelled", nothing half-written in place
  9  content update: changed files fetched, removed ones deleted, a user's own file kept
"""
import argparse
import gzip
import hashlib
import json
import os
import random
import shutil
import socket
import socketserver
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(REPO, "server", "tests"))
import kn_proto  # noqa: E402  (free_ports)

FAILS = []


def check(cond, what):
    print(("  ok    " if cond else "  FAIL  ") + what, flush=True)
    if not cond:
        FAILS.append(what)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


class Origin:
    """A minimal HTTP/1.1 server: keep-alive, pipelining (requests answered in order), Range,
    gzip for .json when asked. files: URL path -> bytes. Faults (set between runs):
      refuse        close every connection right away
      corrupt       set of URL paths answered with different bytes (same length)
      missing       set of URL paths answered 404
      cut           URL path -> bytes after which the body stops and the connection is closed
                    (once: the next request for it is served normally)
      slow_s        sleep per response (lets a cancel land mid-download)
    """

    def __init__(self, name, port):
        self.name, self.port = name, port
        self.files = {}
        self.lock = threading.Lock()
        self.reset_faults()
        self.reset_log()
        origin = self

        class Handler(socketserver.BaseRequestHandler):
            def handle(self):
                origin.serve(self.request)

        class Server(socketserver.ThreadingMixIn, socketserver.TCPServer):
            daemon_threads = True
            allow_reuse_address = True

        self.server = Server(("127.0.0.1", port), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def reset_faults(self):
        self.refuse = False
        self.corrupt, self.missing, self.cut = set(), set(), {}
        self.slow_s = 0.0

    def reset_log(self):
        with self.lock:
            self.requests, self.connections, self.ranges = [], 0, []
            self.max_per_conn = 0

    def close(self):
        self.server.shutdown()
        self.server.server_close()

    def serve(self, sock):
        with self.lock:
            self.connections += 1
        if self.refuse:
            sock.close()
            return
        f = sock.makefile("rb")
        served = 0
        try:
            while True:
                line = f.readline()
                if not line:
                    return
                parts = line.decode("latin-1").split()
                headers = {}
                while True:
                    h = f.readline()
                    if h in (b"\r\n", b"\n", b""):
                        break
                    k, _, v = h.decode("latin-1").partition(":")
                    headers[k.strip().lower()] = v.strip()
                path = parts[1] if len(parts) > 1 else "/"
                served += 1
                with self.lock:
                    self.requests.append(path)
                    self.max_per_conn = max(self.max_per_conn, served)
                if not self.respond(sock, path, headers):
                    return
        except OSError:
            return
        finally:
            try:
                sock.close()
            except OSError:
                pass

    def respond(self, sock, path, headers):
        if self.slow_s:
            time.sleep(self.slow_s)
        body = self.files.get(path.split("?")[0])
        if body is None or path in self.missing:
            sock.sendall(b"HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\n\r\nnot found")
            return True
        if path in self.corrupt:
            body = bytes(b ^ 0x5A for b in body)
        extra = ""
        status = "200 OK"
        if "gzip" in headers.get("accept-encoding", "") and path.endswith(".json"):
            body = gzip.compress(body)
            extra += "Content-Encoding: gzip\r\n"
        rng = headers.get("range", "")
        if rng.startswith("bytes=") and rng.endswith("-"):
            start = int(rng[6:-1])
            with self.lock:
                self.ranges.append((path, start))
            if start < len(body):
                extra += "Content-Range: bytes %d-%d/%d\r\n" % (start, len(body) - 1, len(body))
                body = body[start:]
                status = "206 Partial Content"
        head = ("HTTP/1.1 %s\r\nContent-Length: %d\r\n%sConnection: keep-alive\r\n\r\n" % (status, len(body), extra)).encode()
        if path in self.cut:
            n = self.cut.pop(path)
            sock.sendall(head + body[:n])
            return False  # cut off: the connection closes mid-body
        sock.sendall(head + body)
        return True


def make_tree(rng, n_small=300):
    """path -> content: many small files (pipelined batches), a few large ones (single GETs,
    resumable), two paths sharing one content (downloaded once, copied)."""
    tree = {}
    for i in range(n_small):
        tree["textures/t%03d.png" % i] = rng.randbytes(rng.randint(200, 40000))
    for i in range(4):
        tree["models/big%d.obj" % i] = rng.randbytes(rng.randint(1_500_000, 3_000_000))
    tree["sound/a b.mp3"] = rng.randbytes(5000)  # a space: percent-encoded on the mirror
    tree["css/fonts/font2.ttf"] = b"same content" * 100
    tree["css/fonts/copy.ttf"] = b"same content" * 100
    return tree


def publish(own, cdn, tree, version, mirrors):
    """The updates store on own (objects by sha256) and the tree by path on cdn."""
    files = sorted([p, sha256(d), len(d)] for p, d in tree.items())
    listing = json.dumps({"format": 1, "version": version, "files": files}, separators=(",", ":")).encode()
    assets = {"version": version, "manifest": "../assets/%s.json" % version, "sha256": sha256(listing),
              "size": len(listing), "objects": "../assets/objects/", "files": len(files),
              "bytes": sum(f[2] for f in files), "packaged": [], "mirrors": mirrors}
    manifest = {"format": 1, "channel": "latest",
                "build": {"describe": "v1.0.0-1-gabcdef1", "commit": "abcdef1234567890abcdef1234567890abcdef12",
                          "commit_time": 1760000000, "build_time": "2026-10-13T00:00:00Z", "protocol": "1.3"},
                "outdated_grace_h": 48, "platforms": {}, "assets": assets}
    own.files = {"/updates/client/latest.json": json.dumps(manifest).encode(),
                 "/updates/assets/%s.json" % version: listing}
    for p, d in tree.items():
        h = sha256(d)
        own.files["/updates/assets/objects/%s/%s" % (h[:2], h)] = d
    cdn.files = {"/assets/" + p.replace(" ", "%20"): d for p, d in tree.items()}


def run(bench, dest, manifest_url, *extra, timeout=120):
    p = subprocess.run([bench, dest, "--manifest", manifest_url, *extra], capture_output=True, text=True, timeout=timeout)
    last = p.stdout.strip().splitlines()[-1] if p.stdout.strip() else "{}"
    try:
        out = json.loads(last)
    except ValueError:
        out = {"ok": 0, "err": "bad output: " + last}
    out["stderr"] = p.stderr
    # a sanitizer build (ASan / UBSan / TSan) reports to stderr: any report fails the test
    for marker in ("ERROR: AddressSanitizer", "ERROR: LeakSanitizer", "runtime error:", "WARNING: ThreadSanitizer"):
        if marker in p.stderr:
            check(False, "sanitizer report: " + p.stderr[p.stderr.index(marker):][:1500])
            break
    return out


def tree_ok(dest, tree):
    for p, d in tree.items():
        try:
            with open(os.path.join(dest, p), "rb") as f:
                if f.read() != d:
                    return "%s differs" % p
        except OSError:
            return "%s missing" % p
    return ""


def object_requests(origin):
    return [r for r in origin.requests if "/objects/" in r or (r.startswith("/assets/") and not r.endswith(".json"))]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("bench")
    ap.add_argument("--keep", action="store_true")
    a = ap.parse_args()
    bench = os.path.abspath(a.bench)
    work = tempfile.mkdtemp(prefix="kn-assets-sync-e2e-", dir=os.path.expanduser("~/.cache") if os.path.isdir(os.path.expanduser("~/.cache")) else None)
    p_own, p_cdn = kn_proto.free_ports(2)
    own, cdn = Origin("own", p_own), Origin("cdn", p_cdn)
    try:
        scenarios(bench, work, own, cdn)
    finally:
        own.close()
        cdn.close()
        if not a.keep:
            shutil.rmtree(work, ignore_errors=True)
    print("assets_sync_e2e: %s" % ("FAILED: " + "; ".join(FAILS) if FAILS else "all passed"))
    return 1 if FAILS else 0


def scenarios(bench, work, own, cdn):
    rng = random.Random(11)
    tree = make_tree(rng)
    mirrors = ["http://127.0.0.1:%d/assets/" % cdn.port]
    publish(own, cdn, tree, "v1", mirrors)
    url = "http://127.0.0.1:%d/updates/client/latest.json" % own.port
    dest = os.path.join(work, "assets")
    unique = len({sha256(d) for d in tree.values()})

    print("1  fresh sync from both origins")
    r = run(bench, dest, url, "--threads", "4")
    check(r.get("ok") == 1, "sync ok (%s)" % r.get("err"))
    check(tree_ok(dest, tree) == "", "every file in place and right %s" % tree_ok(dest, tree))
    o_req, c_req = object_requests(own), object_requests(cdn)
    check(len(o_req) > 0 and len(c_req) > 0, "both origins served objects (own %d, cdn %d)" % (len(o_req), len(c_req)))
    check(len(o_req) + len(c_req) == unique, "each unique content requested once (%d requests, %d unique)" % (len(o_req) + len(c_req), unique))
    check(r.get("copied") == 1, "the duplicate content was copied, not downloaded (copied %s)" % r.get("copied"))
    conns = own.connections + cdn.connections
    check(conns <= 2 * 4 + 2, "connections kept alive: %d connections for %d files" % (conns, len(tree)))
    check(max(own.max_per_conn, cdn.max_per_conn) >= 16, "many requests per connection (max %d)" % max(own.max_per_conn, cdn.max_per_conn))
    check(not os.path.exists(os.path.join(dest, ".kn-sync")), "no .kn-sync left after a completed sync")

    print("2  second run: up to date")
    own.reset_log(), cdn.reset_log()
    r = run(bench, dest, url)
    check(r.get("ok") == 1 and r.get("kept") == len(tree), "all kept (%s kept)" % r.get("kept"))
    check(not object_requests(own) and not object_requests(cdn), "no object requests")

    print("3  no state file: the hash cache vouches")
    os.unlink(os.path.join(dest, ".kn-assets.json"))
    r = run(bench, dest, url)
    check(r.get("ok") == 1 and r.get("adopted") == len(tree) and r.get("hashed") == 0,
          "adopted %s, hashed %s (cache hits)" % (r.get("adopted"), r.get("hashed")))
    # a changed file (same size, newer mtime) is hashed again and replaced
    victim = "textures/t007.png"
    with open(os.path.join(dest, victim), "r+b") as f:
        f.write(b"X")
    os.unlink(os.path.join(dest, ".kn-assets.json"))
    r = run(bench, dest, url)
    check(r.get("ok") == 1 and r.get("hashed") == 1 and r.get("fetched") == 1, "the changed file re-hashed and fetched (hashed %s, fetched %s)" % (r.get("hashed"), r.get("fetched")))
    check(tree_ok(dest, tree) == "", "tree right again")

    print("4  resume a cut-off download")
    big = "models/big1.obj"
    os.unlink(os.path.join(dest, big))
    h = sha256(tree[big])
    own_path, cdn_path = "/updates/assets/objects/%s/%s" % (h[:2], h), "/assets/" + big
    own.cut[own_path] = 700_000
    cdn.cut[cdn_path] = 700_000
    own.reset_log(), cdn.reset_log()
    r = run(bench, dest, url, "--threads", "1")
    check(r.get("ok") == 1, "sync ok after the cut (%s)" % r.get("err"))
    check(tree_ok(dest, tree) == "", "the cut file is right")
    ranges = own.ranges + cdn.ranges
    check(any(start == 700_000 for _p, start in ranges), "resumed with Range from the cut (%s)" % ranges)
    check(r.get("resumed", 0) >= 700_000, "resumed bytes reported (%s)" % r.get("resumed"))

    print("5  hash mismatch on the mirror")
    victims = ["textures/t%03d.png" % i for i in range(20, 40)]
    for v in victims:
        os.unlink(os.path.join(dest, v))
    cdn.corrupt = {"/assets/" + v for v in victims}
    own.reset_log(), cdn.reset_log()
    r = run(bench, dest, url, "--threads", "2")
    check(r.get("ok") == 1, "sync ok (%s)" % r.get("err"))
    check(tree_ok(dest, tree) == "", "the corrupted files came right from the other origin")
    cdn_stat = [o for o in r.get("origins", []) if o["url"].startswith("http://127.0.0.1:%d/" % cdn.port)]
    check(cdn_stat and cdn_stat[0]["failures"] > 0, "the mirror's mismatches are counted (%s)" % cdn_stat)
    cdn.reset_faults()

    print("6  origin fallback: the mirror refuses connections")
    for v in victims:
        os.unlink(os.path.join(dest, v))
    cdn.refuse = True
    own.reset_log(), cdn.reset_log()
    r = run(bench, dest, url, "--threads", "2")
    check(r.get("ok") == 1, "sync ok (%s)" % r.get("err"))
    check(tree_ok(dest, tree) == "", "all from the objects")
    check(len(object_requests(own)) >= len(victims), "own served them (%d requests)" % len(object_requests(own)))
    cdn.reset_faults()

    print("7  missing everywhere")
    for v in victims[:1]:
        os.unlink(os.path.join(dest, v))
    hv = sha256(tree[victims[0]])
    own.missing = {"/updates/assets/objects/%s/%s" % (hv[:2], hv)}
    cdn.missing = {"/assets/" + victims[0]}
    t0 = time.time()
    r = run(bench, dest, url)
    check(r.get("ok") == 0 and "missing on the update server" in r.get("err", ""), "fails as missing (%s)" % r.get("err"))
    check(time.time() - t0 < 10, "without retry rounds (%.1f s)" % (time.time() - t0))
    own.reset_faults(), cdn.reset_faults()
    r = run(bench, dest, url)
    check(r.get("ok") == 1 and tree_ok(dest, tree) == "", "the next run completes")

    print("8  cancel mid-download")
    shutil.rmtree(dest)
    own.slow_s = cdn.slow_s = 0.01
    r = run(bench, dest, url, "--threads", "2", "--cancel-after", "2000000")
    check(r.get("ok") == 0 and "cancel" in r.get("err", ""), "stopped as cancelled (%s)" % r.get("err"))
    bad = [p for p, d in tree.items() if os.path.exists(os.path.join(dest, p)) and open(os.path.join(dest, p), "rb").read() != d]
    check(not bad, "nothing wrong in place (%s)" % bad[:3])
    check(not os.path.exists(os.path.join(dest, ".kn-assets.json")), "no state written")
    own.reset_faults(), cdn.reset_faults()
    r = run(bench, dest, url)
    check(r.get("ok") == 1 and tree_ok(dest, tree) == "", "the next run completes")

    print("9  content update")
    tree2 = dict(tree)
    tree2["textures/t001.png"] = rng.randbytes(1234)
    del tree2["textures/t002.png"]
    tree2["textures/new.png"] = rng.randbytes(999)
    with open(os.path.join(dest, "mine.txt"), "wb") as f:
        f.write(b"user file")
    publish(own, cdn, tree2, "v2", mirrors)
    own.reset_log(), cdn.reset_log()
    r = run(bench, dest, url)
    check(r.get("ok") == 1 and tree_ok(dest, tree2) == "", "v2 in place (%s)" % r.get("err"))
    check(r.get("fetched") == 2 and r.get("removed") == 1, "2 fetched, 1 removed (%s, %s)" % (r.get("fetched"), r.get("removed")))
    check(not os.path.exists(os.path.join(dest, "textures/t002.png")), "the removed file is gone")
    check(os.path.exists(os.path.join(dest, "mine.txt")), "the user's own file kept")


if __name__ == "__main__":
    sys.exit(main())
