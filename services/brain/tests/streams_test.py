#!/usr/bin/env python3
"""The v10 Live Streams list on kn_brain (services/brain/streams.c, GET /streams/top) against a
stub Twitch (tests/streams_test.py's own http.server on 127.0.0.1; never the live API).

  streams_test.py <kn_brain> <repo root>

Checks: a live list (valid logins only, live rows only, sorted by viewers, thumbnails filled, the
v10 wrapper {success, data: {streams, promo, dropCampaigns}}), one token request for many answers
(the token is kept), the Client-Id and Bearer the upstream sees, the client secret only in the
token request's body and never in an answer or the brain log, the last good list while the upstream
fails, an empty list (the empty state), the cache (a second answer inside streams.cache_s makes no
upstream call), and 503 StreamsUnavailable with no client secret configured.

Database: as ai_test.py (sqlite here). 77 = skip (no database driver).
"""
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


def run_brain(env, work, tw, cache_s, tag):
    log_path = os.path.join(work, "brain-%s.log" % tag)
    extra = ["--brain.host=127.0.0.1", "--brain.port=0", "--brain.id=streams-%d" % os.getpid(),
             "--accounts.email.required=false", "--smtp.host=",
             "--streams.cache_s=%d" % cache_s,
             "--streams.twitch.auth_url=%s/oauth2/token" % tw.url,
             "--streams.twitch.api_url=%s/helix" % tw.url]
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


def scenario_unconfigured(port):
    st, body = http_get(port, "/streams/top?cb=2")
    ans = json.loads(body)
    check(st == 503 and ans.get("error") == "StreamsUnavailable", "no client secret: 503 StreamsUnavailable (HTTP %d)" % st)


def main():
    scratch = [sys.executable, "-I", os.path.join(HERE, "db_scratch.py"), ROOT]
    r = subprocess.run(scratch + ["create", DRIVER, SCHEMA], capture_output=True)
    if r.returncode:
        print("streams test: %s unavailable: skipped" % DRIVER)
        sys.exit(r.returncode)
    dbenv = {"KN_DATABASE_" + k.upper(): str(v) for k, v in json.loads(r.stdout).items()}
    work = tempfile.mkdtemp(prefix="kn-streams.", dir=os.path.join(os.path.expanduser("~"), ".cache"))
    tw = Twitch()
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
    if FAILS:
        print("streams test: %d FAILED" % len(FAILS))
        sys.exit(1)
    print("streams test: ok")


if __name__ == "__main__":
    main()
