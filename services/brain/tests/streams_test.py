#!/usr/bin/env python3
"""The v10 Live Streams list on kn_brain (services/brain/streams.c, GET /streams/top) against a
stub Twitch (tests/streams_test.py's own http.server on 127.0.0.1; never the live API).

  streams_test.py <kn_brain> <repo root>

Checks: a live list (valid logins only, live rows only, sorted by viewers, thumbnails filled, the
v10 wrapper {success, data: {streams, promo, dropCampaigns}}), one token request for many answers
(the token is kept), the Client-Id and Bearer the upstream sees, the client secret only in the
token request's body and never in an answer or the brain log, the last good list while the upstream
fails, an empty list (the empty state), the cache (a second answer inside streams.cache_s makes no
upstream call), and the empty list {success, data: {streams: []}} with no client secret configured (no source: not a 503).

Thumbnails (stream_thumbs.c, GET /streams/thumb?url=) against a stub CDN (streams.thumbs.cdn_url):
an allow-listed https://static-cdn.jtvnw.net/... preview comes back as its data: URL, byte for byte;
a second ask is served from the cache; other hosts, http, user info, ports, queries and dot
segments are refused (400) without reaching the CDN; a body over streams.thumbs.max_bytes, a body
that is not an image, a redirect (never followed) and a CDN error answer 502.

Database: as ai_test.py (sqlite here). 77 = skip (no database driver).
"""
import base64
import json
import os
import re
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

BRAIN, ROOT = sys.argv[1], sys.argv[2]
HERE = os.path.dirname(os.path.abspath(__file__))
DRIVER = os.environ.get("KN_TEST_DB_DRIVER", "sqlite")
SCHEMA = "kn_test_streams_%d" % os.getpid()
SECRET = "stub-client-secret-%d" % os.getpid()
CLIENT_ID = "stub-client-id-%d" % os.getpid()
FAILS = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what, flush=True)
    if not cond:
        FAILS.append(what)


LIVE = [
    {"id": "1", "user_login": "zeta_one", "title": "Zombie Labs", "viewer_count": 12, "type": "live",
     "language": "EN", "thumbnail_url": "https://static-cdn.jtvnw.net/zeta-{width}x{height}.jpg"},
    {"id": "2", "user_login": "replay_guy", "title": "A VOD", "viewer_count": 900, "type": "vod",
     "thumbnail_url": "https://static-cdn.jtvnw.net/vod-{width}x{height}.jpg"},
    {"id": "3", "user_login": "bad login!", "title": "Injected", "viewer_count": 5000, "type": "live",
     "thumbnail_url": "https://static-cdn.jtvnw.net/bad-{width}x{height}.jpg"},
    {"id": "4", "user_login": "big_streamer", "title": "Parkour", "viewer_count": 300, "type": "live",
     "language": "de", "thumbnail_url": "https://static-cdn.jtvnw.net/big-{width}x{height}.jpg"},
]


# two small JPEGs (88x50 gradients), the stub CDN's previews
THUMB_A = base64.b64decode(
    "/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAoHBwgHBgoICAgLCgoLDhgQDg0NDh0VFhEYIx8lJCIfIiEmKzcvJik0KSEiMEExNDk7Pj4+JS5ESUM8SDc9Pjv/"
    "2wBDAQoLCw4NDhwQEBw7KCIoOzs7Ozs7Ozs7Ozs7Ozs7Ozs7Ozs7Ozs7Ozs7Ozs7Ozs7Ozs7Ozs7Ozs7Ozs7Ozs7Ozv/wAARCAAyAFgDASIAAhEBAxEB/8QAFwAB"
    "AQEBAAAAAAAAAAAAAAAAAAMCBf/EABUQAQEAAAAAAAAAAAAAAAAAAAAS/8QAFgEBAQEAAAAAAAAAAAAAAAAAAAUE/8QAFREBAQAAAAAAAAAAAAAAAAAAABP/"
    "2gAMAwEAAhEDEQA/AOBJKkkrVVKKckqSSVIpySpJJUinJKkklSKckqSSVIpyKSFSKskqSSl1V4pySpJJUinJKkklSKckqSSVIpySpJJUinIpIVIqSSpJKXVX"
    "inJKkklSKckqSSVIpySpJJUinJKkklSKcikhUi2AxtgAAAAAAAAAD//Z")
THUMB_B = base64.b64decode(
    "/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAoHBwgHBgoICAgLCgoLDhgQDg0NDh0VFhEYIx8lJCIfIiEmKzcvJik0KSEiMEExNDk7Pj4+JS5ESUM8SDc9Pjv/"
    "2wBDAQoLCw4NDhwQEBw7KCIoOzs7Ozs7Ozs7Ozs7Ozs7Ozs7Ozs7Ozs7Ozs7Ozs7Ozs7Ozs7Ozs7Ozs7Ozs7Ozs7Ozv/wAARCAAyAFgDASIAAhEBAxEB/8QAFQAB"
    "AQAAAAAAAAAAAAAAAAAAAAH/xAAUEAEAAAAAAAAAAAAAAAAAAAAA/8QAGQEBAAIDAAAAAAAAAAAAAAAAAAEFAwQG/8QAFREBAQAAAAAAAAAAAAAAAAAAABP/2gAM"
    "AwEAAhEDEQA/AIIMEHEKIEBRAgKIEBRAgKIEAEFzBKiBAUQICiBAUQICiBABBdwSogQFECAogQFECAogQABvAAAAAAAAAAD/2Q==")


class Cdn:
    """The stub Twitch CDN (static-cdn.jtvnw.net's previews). Counts every request by path."""

    def __init__(self, max_bytes):
        self.hits = {}
        self.lock = threading.Lock()
        self.files = {
            "/previews-ttv/live_user_big_streamer-440x248.jpg": ("image/jpeg", THUMB_A),
            "/previews-ttv/live_user_zeta_one-440x248.jpg": ("image/jpeg", THUMB_B),
            "/previews-ttv/huge-440x248.jpg": ("image/jpeg", b"\xff\xd8\xff\xe0" + b"\0" * (max_bytes + 4096)),
            "/previews-ttv/page-440x248.jpg": ("text/html", b"<html><body>not an image</body></html>"),
        }
        self.srv = ThreadingHTTPServer(("127.0.0.1", 0), self.handler())
        self.url = "http://127.0.0.1:%d" % self.srv.server_address[1]
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()

    def count(self, path):
        with self.lock:
            return self.hits.get(path, 0)

    def handler(self):
        cdn = self

        class H(BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def do_GET(self):
                with cdn.lock:
                    cdn.hits[self.path] = cdn.hits.get(self.path, 0) + 1
                if self.path == "/previews-ttv/moved-440x248.jpg":
                    self.send_response(302)
                    self.send_header("Location", "%s/previews-ttv/live_user_big_streamer-440x248.jpg" % cdn.url)
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    return
                f = cdn.files.get(self.path)
                if not f:
                    self.send_response(404)
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    return
                self.send_response(200)
                self.send_header("Content-Type", f[0])
                self.send_header("Content-Length", str(len(f[1])))
                self.end_headers()
                self.wfile.write(f[1])

        return H


class Twitch:
    """The stub: token (client credentials), games, streams. Records every request."""

    def __init__(self):
        self.data = LIVE
        self.fail = False
        self.tokens = 0
        self.streams_calls = 0
        self.seen = []  # (method, path, headers dict, body)
        self.lock = threading.Lock()
        self.srv = ThreadingHTTPServer(("127.0.0.1", 0), self.handler())
        self.url = "http://127.0.0.1:%d" % self.srv.server_address[1]
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()

    def handler(self):
        twitch = self

        class H(BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def send_json(self, code, obj):
                body = json.dumps(obj).encode()
                self.send_response(code)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def do_POST(self):
                n = int(self.headers.get("Content-Length") or 0)
                body = self.rfile.read(n).decode()
                with twitch.lock:
                    twitch.seen.append(("POST", self.path, dict(self.headers), body))
                form = urllib.parse.parse_qs(body)
                if self.path != "/oauth2/token" or form.get("client_secret") != [SECRET] or form.get("grant_type") != ["client_credentials"]:
                    return self.send_json(400, {"status": 400, "message": "invalid client"})
                with twitch.lock:
                    twitch.tokens += 1
                    tok = "tok-%d" % twitch.tokens
                self.send_json(200, {"access_token": tok, "expires_in": 3600, "token_type": "bearer"})

            def do_GET(self):
                u = urllib.parse.urlsplit(self.path)
                q = urllib.parse.parse_qs(u.query)
                with twitch.lock:
                    twitch.seen.append(("GET", self.path, dict(self.headers), ""))
                if self.headers.get("Client-Id") != CLIENT_ID or not self.headers.get("Authorization", "").startswith("Bearer tok-"):
                    return self.send_json(401, {"status": 401, "message": "invalid token"})
                if u.path == "/helix/games":
                    return self.send_json(200, {"data": [{"id": "512345", "name": "Krunker"}]})
                if u.path == "/helix/streams":
                    with twitch.lock:
                        twitch.streams_calls += 1
                        fail, data = twitch.fail, list(twitch.data)
                    if q.get("game_id") != ["512345"]:
                        return self.send_json(400, {"message": "wrong game"})
                    if fail:
                        return self.send_json(500, {"message": "stub outage"})
                    return self.send_json(200, {"data": data})
                return self.send_json(404, {"message": "not found"})

        return H


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


def http_get(port, path, timeout=20):
    req = urllib.request.Request("http://127.0.0.1:%d%s" % (port, path))
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, r.read().decode()
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode()


def run_brain(env, work, tw, cache_s, tag, more=()):
    log_path = os.path.join(work, "brain-%s.log" % tag)
    extra = ["--brain.host=127.0.0.1", "--brain.port=0", "--brain.id=streams-%d" % os.getpid(),
             "--accounts.email.required=false", "--smtp.host=",
             "--streams.cache_s=%d" % cache_s,
             "--streams.twitch.auth_url=%s/oauth2/token" % tw.url,
             "--streams.twitch.api_url=%s/helix" % tw.url] + list(more)
    with open(log_path, "a") as log:
        proc = subprocess.Popen([BRAIN] + extra, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT)
    return proc, log_path


def base_env(dbenv, secret):
    env = {k: v for k, v in os.environ.items() if not k.startswith("KN_STREAMS_") and not k.startswith("KN_AI_")}
    env.update(dbenv, KN_BRAIN_INTERNAL_SECRET="kn-streams-test-%d" % os.getpid())
    if secret:
        env.update(KN_STREAMS_TWITCH_CLIENT_ID=CLIENT_ID, KN_STREAMS_TWITCH_CLIENT_SECRET=SECRET)
    return env


def scenario_live(port, tw, log_path):
    st, body = http_get(port, "/streams/top?cb=2")
    ans = json.loads(body)
    check(st == 200 and ans.get("success") is True, "GET /streams/top answers success (HTTP %d)" % st)
    data = ans.get("data") or {}
    streams = data.get("streams") or []
    check([s["username"] for s in streams] == ["big_streamer", "zeta_one"],
          "live rows only, valid logins, sorted by viewers (%s)" % [s["username"] for s in streams])
    if streams:
        s0 = streams[0]
        check(s0["viewerCount"] == 300 and s0["title"] == "Parkour", "the viewer count and title come through")
        check(s0["thumbnailUrl"] == "https://static-cdn.jtvnw.net/big-440x248.jpg", "the thumbnail template is filled (%s)" % s0["thumbnailUrl"])
        check(s0["languageCode"] == "de", "the language is lower-cased")
    check(data.get("promo") is False and data.get("dropCampaigns") == [], "the v10 wrapper has promo and dropCampaigns")
    check(SECRET not in body, "the client secret is not in the answer")
    posts = [s for s in tw.seen if s[0] == "POST"]
    check(len(posts) == 1, "one token request so far (%d)" % len(posts))
    check(tw.tokens == 1, "the access token was fetched once")

    # the token is kept: more answers, no new token request
    for _ in range(3):
        http_get(port, "/streams/top?cb=2")
    check(tw.tokens == 1, "the token is kept across answers (%d tokens)" % tw.tokens)

    # Twitch down: the last good list
    tw.fail = True
    st, body = http_get(port, "/streams/top?cb=2")
    ans = json.loads(body)
    check(st == 200 and ans.get("success") is True and len((ans.get("data") or {}).get("streams") or []) == 2,
          "while the upstream fails, the last good list is served (HTTP %d)" % st)
    tw.fail = False

    # no live streams: the empty state
    tw.data = []
    st, body = http_get(port, "/streams/top?cb=2")
    ans = json.loads(body)
    check(st == 200 and ans.get("success") is True and (ans.get("data") or {}).get("streams") == [],
          "no live streams: an empty list (HTTP %d)" % st)
    tw.data = LIVE

    with open(log_path) as fp:
        log = fp.read()
    check(SECRET not in log, "the client secret is not in the brain log")
    check("Live Streams on" in log, "the brain logs that Live Streams is on")


def scenario_cache(port, tw):
    before = tw.streams_calls
    st1, b1 = http_get(port, "/streams/top?cb=2")
    st2, b2 = http_get(port, "/streams/top?cb=2")
    check(st1 == 200 and st2 == 200 and b1 == b2, "two answers inside streams.cache_s are the same")
    check(tw.streams_calls - before <= 1, "a cached answer makes no upstream call (%d calls)" % (tw.streams_calls - before))


def thumb(port, url):
    st, body = http_get(port, "/streams/thumb?url=" + urllib.parse.quote(url, safe=""))
    try:
        return st, json.loads(body)
    except ValueError:
        return st, {}


def scenario_thumbs(port, cdn):
    cdn_host = "https://static-cdn.jtvnw.net"
    big = "/previews-ttv/live_user_big_streamer-440x248.jpg"
    st, ans = thumb(port, cdn_host + big)
    img = (ans.get("data") or {}).get("image") or ""
    check(st == 200 and ans.get("success") is True, "an allow-listed preview is answered (HTTP %d)" % st)
    check(img.startswith("data:image/jpeg;base64,") and base64.b64decode(img.split(",", 1)[1]) == THUMB_A,
          "the answer is the preview's data: URL, byte for byte")
    check((ans.get("data") or {}).get("url") == cdn_host + big, "the answer names the URL it is for")
    check(cdn.count(big) == 1, "the CDN was asked once (%d)" % cdn.count(big))
    for _ in range(3):
        st2, ans2 = thumb(port, cdn_host + big)
    check(st2 == 200 and ans2 == ans and cdn.count(big) == 1, "asked again: served from the cache (CDN hits %d)" % cdn.count(big))
    st, ans = thumb(port, cdn_host + "/previews-ttv/live_user_zeta_one-440x248.jpg")
    check(st == 200 and base64.b64decode(((ans.get("data") or {}).get("image") or ",").split(",", 1)[1]) == THUMB_B,
          "a second stream's preview is its own")

    before = sum(cdn.hits.values())
    refused = [
        "https://evil.example/previews-ttv/live_user_big_streamer-440x248.jpg",
        "https://static-cdn.jtvnw.net.evil.example/a.jpg",
        "http://static-cdn.jtvnw.net" + big,
        "https://user@static-cdn.jtvnw.net" + big,
        "https://static-cdn.jtvnw.net:444" + big,
        "https://static-cdn.jtvnw.net" + big + "?x=1",
        "https://static-cdn.jtvnw.net/../etc/passwd.jpg",
        "https://static-cdn.jtvnw.net/previews-ttv/a.gif",
        "file:///etc/passwd",
        "",
    ]
    for u in refused:
        st, ans = thumb(port, u)
        check(st == 400 and ans.get("error") == "InvalidThumbnail", "refused: %r (HTTP %d)" % (u, st))
    st, body = http_get(port, "/streams/thumb")
    check(st == 400, "no url: 400 (HTTP %d)" % st)
    check(sum(cdn.hits.values()) == before, "the refused URLs never reached the CDN")

    st, ans = thumb(port, cdn_host + "/previews-ttv/huge-440x248.jpg")
    check(st == 502 and ans.get("error") == "ThumbnailTooLarge", "a body over streams.thumbs.max_bytes: 502 ThumbnailTooLarge (HTTP %d %s)" % (st, ans.get("error")))
    st, ans = thumb(port, cdn_host + "/previews-ttv/page-440x248.jpg")
    check(st == 502 and ans.get("error") == "ThumbnailNotImage", "a body that is not a JPEG / PNG: 502 ThumbnailNotImage (HTTP %d)" % st)
    hits_target = cdn.count(big)
    st, ans = thumb(port, cdn_host + "/previews-ttv/moved-440x248.jpg")
    check(st == 502 and cdn.count(big) == hits_target, "a redirect is not followed: 502 (HTTP %d)" % st)
    st, ans = thumb(port, cdn_host + "/previews-ttv/gone-440x248.jpg")
    check(st == 502 and ans.get("error") == "ThumbnailUnavailable", "a CDN 404: 502 ThumbnailUnavailable (HTTP %d)" % st)
    gone = cdn.count("/previews-ttv/gone-440x248.jpg")
    thumb(port, cdn_host + "/previews-ttv/gone-440x248.jpg")
    check(cdn.count("/previews-ttv/gone-440x248.jpg") == gone, "a failure is cached too (the CDN is not asked again at once)")


def scenario_unconfigured(port):
    st, body = http_get(port, "/streams/top?cb=2")
    ans = json.loads(body)
    check(st == 200 and ans.get("success") is True and ans.get("data") == {"streams": []},
          "no client secret: the empty list {success, data: {streams: []}} (HTTP %d)" % st)


def main():
    scratch = [sys.executable, "-I", os.path.join(HERE, "db_scratch.py"), ROOT]
    r = subprocess.run(scratch + ["create", DRIVER, SCHEMA], capture_output=True)
    if r.returncode:
        print("streams test: %s unavailable: skipped" % DRIVER)
        sys.exit(r.returncode)
    dbenv = {"KN_DATABASE_" + k.upper(): str(v) for k, v in json.loads(r.stdout).items()}
    work = tempfile.mkdtemp(prefix="kn-streams.", dir=os.path.join(os.path.expanduser("~"), ".cache"))
    tw = Twitch()
    cdn = Cdn(4096)
    procs = []
    try:
        env = base_env(dbenv, SECRET)
        subprocess.run([BRAIN, "--migrate-only"], cwd=ROOT, env=env, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

        print("live list, no cache (streams.cache_s=0):")
        proc, log_path = run_brain(env, work, tw, 0, "live")
        procs.append(proc)
        scenario_live(bound_port(log_path, proc), tw, log_path)
        proc.terminate()

        print("cache (streams.cache_s=30):")
        proc, log_path = run_brain(env, work, tw, 30, "cache")
        procs.append(proc)
        scenario_cache(bound_port(log_path, proc), tw)
        proc.terminate()

        print("thumbnails (stub CDN, streams.thumbs.max_bytes=4096):")
        proc, log_path = run_brain(env, work, tw, 30, "thumbs",
                                   ["--streams.thumbs.cdn_url=%s" % cdn.url, "--streams.thumbs.max_bytes=4096"])
        procs.append(proc)
        scenario_thumbs(bound_port(log_path, proc), cdn)
        proc.terminate()

        print("no client secret:")
        proc, log_path = run_brain(base_env(dbenv, None), work, tw, 0, "unconfigured")
        procs.append(proc)
        scenario_unconfigured(bound_port(log_path, proc))
        proc.terminate()
    except Exception as e:  # noqa: BLE001
        import traceback
        traceback.print_exc()
        check(False, "exception: %r" % e)
    finally:
        for p in procs:
            if p.poll() is None:
                p.terminate()
        tw.srv.shutdown()
        cdn.srv.shutdown()
    if FAILS:
        print("streams test: %d FAILED" % len(FAILS))
        sys.exit(1)
    print("streams test: ok")


if __name__ == "__main__":
    main()
