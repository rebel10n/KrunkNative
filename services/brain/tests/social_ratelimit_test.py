#!/usr/bin/env python3
"""The social socket's rate limits (services/brain/social_ws.c, doc/social-rate-limit.md) on a
kn_brain with its own database (sqlite, or postgres: KN_TEST_DB_DRIVER, KN_TEST_CONFIG).

  social_ratelimit_test.py <kn_brain> <repo root>

Static: the class table in the code and the one in the doc agree, and every socket label of the
route table has a class (k_label_class), so a new route cannot slip past the limiter unclassed.

Live (JSON frames, one connection per case so each class starts with a full bucket):
  - a flood of fire-and-forget frames: one notice ("Rate limited, retry in N ms"), then after
    retry_ms a frame passes (no notice), and the next flood gets one notice again;
  - a flood of unknown labels: its burst answered "Unknown request.", one notice, recovery;
  - frames without a label (not an array, no string label, not JSON): each answered "Malformed
    request."; an unknown eb type gets EB_UNKNOWN_TYPE;
  - a flood of gapi GETs: every rid gets exactly one answer, 200 or 429 RATE_LIMITED with retry_ms;
    after the retry time the next GET is answered 200;
  - a flood of presence:ping: every ping is answered, pong or EB_RATE_LIMITED of presence:ping;
  - "po" frames (the anti-hack reply) are never limited;
  - a flood of logins: every "a" frame is answered;
  - a friend's presence flapping inside one flush window reaches the friend once, with the newest
    value (coalesced state push).
77 = skip (no kn_brain / websockets).
"""
import asyncio
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

try:
    import websockets
except ImportError:  # pragma: no cover
    print("python websockets missing: skip")
    sys.exit(77)

BRAIN, ROOT = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
if not os.path.exists(BRAIN):
    print("no kn_brain: skip")
    sys.exit(77)
sys.path.insert(0, os.path.join(ROOT, "server", "tests"))
from kn_proto import free_ports  # noqa: E402

FAILS = []
WORK = tempfile.mkdtemp(prefix="kn-social-rl.", dir=os.path.expanduser("~/.cache"))
DB = os.path.join(WORK, "coordinator.db")
DRIVER = os.environ.get("KN_TEST_DB_DRIVER", "sqlite")
PG = {}
RETRY_RE = re.compile(r"retry in (\d+) ms")


def check(cond, what, detail=""):
    print(("  ok   " if cond else "  FAIL ") + what + (("  -- " + str(detail)) if detail and not cond else ""), flush=True)
    if not cond:
        FAILS.append(what)


def static_checks():
    """the limit table of the code and of the doc, and the route classes"""
    src = open(os.path.join(ROOT, "services", "brain", "social_ws.c")).read()
    doc = open(os.path.join(ROOT, "doc", "social-rate-limit.md")).read()
    rate = src[src.index("k_rate[SOCIAL_RATE_CLASSES]"):]
    rate = rate[:rate.index("};")]
    code = {m[2]: (float(m[0]), float(m[1])) for m in re.findall(r"\{([\d.]+),\s*([\d.]+)\},\s*/\* (\w+) \*/", rate)}
    rows = {m[0]: (float(m[1]), float(m[2])) for m in re.findall(r"^\| `(\w+)` \| ([\d.]+) \| ([\d.]+) \|", doc, re.M)}
    check(len(code) == 5 and code == rows, "the limit table of the doc matches k_rate", (code, rows))
    routes = src[src.index("static const SocialRoute k_routes[]"):]
    routes = routes[:routes.index("};")]
    labels = set(re.findall(r'\{"([^"]+)", sh_', routes))
    table = src[src.index("k_label_class[] = {"):]
    table = table[:table.index("};")]
    classed = set(re.findall(r'\{"([^"]+)", RC_', table))
    unclassed = sorted(labels - classed - {"gapi", "eb", "po"})
    check(not unclassed, "every route label has a class (k_label_class)", unclassed)


class Sock:
    def __init__(self, ws):
        self.ws = ws
        self.buf = []
        self.id = 0

    @classmethod
    async def open(cls, port):
        return cls(await websockets.connect("ws://127.0.0.1:%d/social/ws?enc=json" % port, max_size=2 ** 22))

    async def send(self, *frame):
        await self.ws.send(json.dumps(list(frame)))

    async def flood(self, n, *frame):
        """n identical frames back to back, no waiting for answers"""
        text = json.dumps(list(frame))
        for _ in range(n):
            await self.ws.send(text)

    async def _recv(self, timeout):
        """the next frame off the socket, or None after timeout seconds"""
        try:
            return json.loads(await asyncio.wait_for(self.ws.recv(), max(0.01, timeout)))
        except asyncio.TimeoutError:
            return None

    async def read(self, timeout):
        """the next frame (buffered ones first), or None after timeout seconds"""
        if self.buf:
            return self.buf.pop(0)
        return await self._recv(timeout)

    async def collect(self, seconds):
        """every frame that arrives within `seconds`"""
        out = []
        end = time.time() + seconds
        while time.time() < end:
            f = await self.read(end - time.time())
            if f is None:
                break
            out.append(f)
        return out

    async def wait(self, pred, timeout=10.0):
        for i, f in enumerate(self.buf):
            if pred(f):
                return self.buf.pop(i)
        end = time.time() + timeout
        while time.time() < end:
            f = await self._recv(end - time.time())
            if f is None:
                break
            if pred(f):
                return f
            self.buf.append(f)
        return None

    async def auth(self, name, action=0, password="rl_pass_1"):
        await self.send("a", action, [name, password, None])
        f = await self.wait(lambda f: f[0] == "a")
        assert f and f[1] is None, f
        self.id = f[2]
        return f[2]

    async def gapi(self, rid, method, path, body=None):
        await self.send("gapi", rid, method, path, body)
        return await self.wait(lambda f: f[0] == "gapi" and f[1] == rid)

    async def close(self):
        await self.ws.close()


def retry_of(frame):
    m = RETRY_RE.search(frame[2] if frame and len(frame) > 2 and isinstance(frame[2], str) else "")
    return int(m.group(1)) if m else None


def is_notice(f):
    return f[0] == "po" and f[1] is None and isinstance(f[2], str) and f[2].startswith("Rate limited")


async def scenario(port):
    stamp = str(int(time.time()) % 100000)
    names = ["rlA" + stamp, "rlB" + stamp, "rlC" + stamp]

    # --- fire-and-forget: one notice per window, recovery after retry_ms
    n = await Sock.open(port)
    await n.flood(200, "sfkr")
    got = await n.collect(0.6)
    notices = [f for f in got if is_notice(f)]
    check(len(notices) == 1, "a flood of 200 fire-and-forget frames gets exactly one notice", got[:3])
    retry = retry_of(notices[0]) if notices else None
    check(retry and retry > 0, "the notice carries the retry time", notices[:1])
    await asyncio.sleep((retry or 300) / 1000.0 + 0.05)
    await n.send("sfkr")  # the bucket holds a token again: this frame passes and closes the window
    got = await n.collect(0.4)
    check(not [f for f in got if is_notice(f)], "after retry_ms a frame passes (no notice)", got)
    await n.flood(200, "sfkr")
    got = await n.collect(0.6)
    again = [f for f in got if is_notice(f)]
    check(len(again) == 1, "the next flood after the recovery gets one notice", got[:3])
    await n.close()

    # --- unknown labels: each one passing its bucket is answered; a refused one gets the window notice
    u = await Sock.open(port)
    await u.flood(200, "zzNoSuchLabel", 1)
    got = [f for f in await u.collect(0.6) if f[0] == "po"]  # the greeting ("pi", "io-init") is not an answer
    unknown = [f for f in got if f[2] == "Unknown request."]
    notices = [f for f in got if is_notice(f)]
    check(len(notices) == 1, "a flood of 200 unknown labels gets one rate-limit notice", got[:3])
    check(1 <= len(unknown) <= 20 and len(unknown) + len(notices) == len(got),
          "the unknown label flood: its burst answered as \"Unknown request.\", nothing else", (len(unknown), len(got)))
    await asyncio.sleep((retry_of(notices[0]) if notices else 300) / 1000.0 + 0.05)
    await u.send("zzNoSuchLabel", 1)  # recovery: passes and gets its explicit answer
    got = await u.collect(0.4)
    check([f for f in got if f[0] == "po" and f[2] == "Unknown request."] and not [f for f in got if is_notice(f)],
          "after retry_ms an unknown label is answered again", got)
    await u.flood(200, "zzNoSuchLabel", 1)
    got = await u.collect(0.6)
    check(len([f for f in got if is_notice(f)]) == 1, "the next unknown flood gets one notice again", len(got))
    await u.close()

    # --- frames without a label: an explicit answer each; an unknown eb type too
    m = await Sock.open(port)
    await m.ws.send(json.dumps({"no": "label"}))
    await m.ws.send(json.dumps([5, "not a label"]))
    await m.ws.send(json.dumps([]))
    await m.ws.send("this is not json")
    await m.send("eb", {"type": "no:such:type"})
    got = [f for f in await m.collect(0.8) if f[0] != "pi" and f[0] != "io-init"]
    malformed = [f for f in got if f[0] == "po" and f[2] == "Malformed request."]
    check(len(malformed) == 4, "four frames without a label: each answered \"Malformed request.\"", got[:6])
    eb_unknown = [f for f in got if f[0] == "eb" and f[1].get("code") == "EB_UNKNOWN_TYPE"]
    check(len(eb_unknown) == 1 and eb_unknown[0][1]["of"] == "no:such:type",
          "an unknown eb type: EB_UNKNOWN_TYPE of that type", got[-2:])
    await m.close()

    # --- gapi: every rid answered once
    a = await Sock.open(port)
    await a.auth(names[0], 0)
    rids = list(range(1000, 1260))
    for rid in rids:
        await a.ws.send(json.dumps(["gapi", rid, "GET", "/players/search?q=rl&limit=5", None]))
    replies = {}
    end = time.time() + 40
    while len(replies) < len(rids) and time.time() < end:
        f = await a.read(end - time.time())
        if f is None:
            break
        if f[0] == "gapi" and f[1] in set(rids):
            replies.setdefault(f[1], []).append(f)
    once = all(len(replies.get(r, [])) == 1 for r in rids)
    check(once, "a gapi burst: every rid is answered exactly once", {r: len(replies.get(r, [])) for r in rids if len(replies.get(r, [])) != 1})
    refused = [replies[r][0] for r in rids if r in replies and replies[r][0][2] == 429]
    passed = [replies[r][0] for r in rids if r in replies and replies[r][0][2] == 200]
    check(len(refused) >= 1 and len(passed) >= 100, "the burst is partly refused and partly answered", (len(passed), len(refused)))
    check(all(f[3]["error"]["code"] == "RATE_LIMITED" and f[3]["error"]["retry_ms"] >= 1 and "message" in f[3]["error"]
              for f in refused), "each 429 is RATE_LIMITED with retry_ms", refused[:1])
    await asyncio.sleep(0.5)
    st = await a.gapi(9999, "GET", "/players/search?q=rl&limit=5")
    check(st and st[2] == 200, "after the retry time a GET is answered 200", st)

    # --- eb pings: every ping answered
    p = await Sock.open(port)
    await p.flood(100, "eb", {"type": "presence:ping"})
    got = await p.collect(1.0)
    pongs = [f for f in got if f[0] == "eb" and f[1].get("type") == "presence:pong"]
    errs = [f for f in got if f[0] == "eb" and f[1].get("type") == "error"]
    check(len(pongs) + len(errs) == 100, "a flood of 100 pings: every ping answered", (len(pongs), len(errs)))
    check(len(errs) >= 1 and all(e[1]["code"] == "EB_RATE_LIMITED" and e[1]["of"] == "presence:ping" and e[1]["retry_ms"] >= 1
                                 for e in errs), "a refused ping: EB_RATE_LIMITED of presence:ping with retry_ms", errs[:1])
    await p.close()

    # --- "po" is never limited
    q = await Sock.open(port)
    await q.flood(300, "po", None)
    got = await q.collect(0.4)
    check(not [f for f in got if is_notice(f)], "300 \"po\" frames: never limited", got[:2])
    await q.send("eb", {"type": "presence:ping"})
    got = await q.collect(0.4)
    check(any(f[0] == "eb" and f[1].get("type") == "presence:pong" for f in got), "the socket still answers a ping", got[:2])
    await q.close()

    # --- logins: every "a" answered (the auth class)
    lg = await Sock.open(port)
    for _ in range(12):
        await lg.send("a", 1, [names[0], "wrong_pass_1", None])
    replies = []
    end = time.time() + 20
    while len(replies) < 12 and time.time() < end:
        f = await lg.read(end - time.time())
        if f is None:
            break
        if f[0] == "a":
            replies.append(f)
    check(len(replies) == 12, "12 login attempts: every one answered with an \"a\" frame", len(replies))
    check(any(f[1] and "retry in" in f[1] for f in replies), "a refused login says when to retry", replies[-1:])
    await lg.close()

    # --- coalesced presence: a friend's flapping reaches the watcher once, with the newest value
    # names[1] (watcher) and names[2] (flapper) follow each other: friends
    w = await Sock.open(port)
    await w.auth(names[1], 0)
    f0 = await Sock.open(port)
    await f0.auth(names[2], 0)
    st = await w.gapi(7001, "POST", "/players/%s/follow" % names[2], {"follow": True})
    check(st and st[2] == 200, "the watcher follows the flapper", st)
    st = await f0.gapi(7002, "POST", "/players/%s/follow" % names[1], {"follow": True})
    check(st and st[2] == 200, "the flapper follows back: friends", st)
    await f0.close()
    await asyncio.sleep(0.3)
    await w.collect(0.5)
    # the flapper: online, offline, online, offline, online within a few milliseconds
    flapper = None
    for i in range(3):
        s = await Sock.open(port)
        await s.auth(names[2], 1)
        if i < 2:
            await s.close()
        else:
            flapper = s
    flapper_id = flapper.id
    got = await w.collect(1.0)
    pres = [f for f in got if f[0] == "eb" and f[1].get("type", "").startswith("presence:") and f[1].get("id") == flapper_id]
    check(len(pres) == 1 and pres[0][1]["type"] == "presence:online",
          "a flapping friend reaches the watcher once, with the newest value", (len(pres), [f for f in got if f[0] == "eb"][:4]))
    await flapper.close()
    await w.close()


def main():
    static_checks()
    (port,) = free_ports(1)
    log = open(os.path.join(WORK, "brain.log"), "w")
    cfg = os.path.join(WORK, "brain.json")
    dbcfg = {"driver": "sqlite", "path": DB}
    if DRIVER == "postgres":
        with open(os.environ.get("KN_TEST_CONFIG", os.path.join(ROOT, "config", "config.json"))) as fp:
            db = json.load(fp).get("database", {})
        dbname = os.environ.get("KN_TEST_DB_NAME", "krunknative_svcacc")
        PG["schema"] = "kn_test_social_rl_%d" % os.getpid()
        PG["env"] = dict(os.environ, PGPASSWORD=str(db.get("password", "")))
        PG["psql"] = ["psql", "-h", str(db.get("host", "127.0.0.1")), "-p", str(db.get("port", 5432)), "-U",
                      str(db.get("user", "krunknative")), "-d", dbname, "-v", "ON_ERROR_STOP=1", "-qAt"]
        try:
            subprocess.run(PG["psql"] + ["-c", "CREATE SCHEMA %s" % PG["schema"]], env=PG["env"], check=True, capture_output=True, timeout=30)
        except (OSError, subprocess.SubprocessError) as e:
            print("PostgreSQL unavailable (%s): skip" % e)
            sys.exit(77)
        dbcfg = dict(db, name=dbname, search_path=PG["schema"])
    with open(cfg, "w") as f:
        json.dump({"database": dict(dbcfg, migrations_dir=os.path.join(ROOT, "services", "db", "migrations")),
                   "brain": {"host": "127.0.0.1", "port": port, "federation": False, "internal_secret": "social-rl-%d" % os.getpid()},
                   "accounts": {"free_premium": False, "starting_kr": 0, "login_throttle_s": 0, "password_iterations": 1000,
                                "skins_file": os.path.join(ROOT, "assets", "skins.json")}}, f)
    brain = subprocess.Popen([BRAIN, "--config", cfg, "--migrate"], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
    try:
        for _ in range(200):
            try:
                with open(os.path.join(WORK, "brain.log")) as f:
                    if re.search(r"listening on http://127\.0\.0\.1:%d" % port, f.read()):
                        break
            except OSError:
                pass
            if brain.poll() is not None:
                break
            time.sleep(0.1)
        asyncio.run(scenario(port))
    except Exception as e:  # noqa: BLE001
        import traceback
        traceback.print_exc()
        check(False, "exception: %r" % e)
    finally:
        brain.terminate()
        try:
            brain.wait(10)
        except subprocess.TimeoutExpired:
            brain.kill()
        log.close()
        if PG:
            subprocess.run(PG["psql"] + ["-c", "DROP SCHEMA IF EXISTS %s CASCADE" % PG["schema"]], env=PG["env"], capture_output=True)
    print("social ratelimit test: " + ("OK" if not FAILS else "%d failure(s), files in %s" % (len(FAILS), WORK)))
    if not FAILS and not os.environ.get("KN_KEEP"):
        shutil.rmtree(WORK, ignore_errors=True)
    sys.exit(1 if FAILS else 0)


main()
