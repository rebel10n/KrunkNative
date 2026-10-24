#!/usr/bin/env python3
"""Krunker v10.0.3 live events on kn_brain (doc/v10/events.md; services/brain/community.c "gameEvents",
internal_ws.c "welcome" / "game-events", GET /data/events).

  live_events_test.py <kn_brain> <repo root>
Runs the brain without a database (--no-db) on an ephemeral port. 77 = skip (no python websockets).

Covers: the game servers' welcome carries the running event with every config.events field 10.0.3 reads (id, name,
startsAt / endsAt, reveal, imgExt, longDesc, noChallenges, rewards {skins, weights}, rewardChance, eligibleMaps,
limitedTimeMode) plus eventObjects and the kill pools; bad values are dropped (a non-integer skin or map, an imgExt
with a path); GET /data/events serves the clients the running event and the one that reveals early (not one past its
reveal window, nor an ended one), without the kill pools; an edit of the feed pushes "game-events" with the new set.
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
import urllib.request

try:
    import websockets
except ImportError:  # pragma: no cover
    print("python websockets missing: skip")
    sys.exit(77)

BRAIN, ROOT = sys.argv[1], os.path.abspath(sys.argv[2])
SECRET = "kn-liveevents-test-%d" % os.getpid()
FAILS = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what, flush=True)
    if not cond:
        FAILS.append(what)


def bound_port(log_path, proc):
    for _ in range(200):
        if proc.poll() is not None:
            raise RuntimeError("kn_brain exited early (%s)" % log_path)
        with open(log_path) as fp:
            m = re.search(r"listening on http://127\.0\.0\.1:(\d+) \(ws", fp.read())
        if m:
            return int(m.group(1))
        time.sleep(0.05)
    raise RuntimeError("kn_brain did not start listening")


def iso(t):
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(t))


def halloween(now):
    return {"id": 8, "name": "Halloween", "startsAt": iso(now - 3600), "endsAt": iso(now + 3600), "reveal": 86400000,
            "imgExt": "webp", "longDesc": ["Hunt the pumpkins", "<b>Collect</b> the skins"], "noChallenges": False,
            "rewards": {"skins": [7540, "x", 7541, 1.5], "weights": [0, 0, 70, 25, 4, 0.1, 0.01, 9]}, "rewardChance": 40,
            "eligibleMaps": [0, 3, "a"], "limitedTimeMode": True, "eventObjects": 1, "killPoolsWeight": 5,
            "killPoolsMode": "dilute", "killPoolsIcon": "gift",
            "killPools": [{"weight": 100, "contents": [{"content_type": "skin", "content_ref": "7540"}]}]}


def feed_events(now, aon_starts):
    return [halloween(now),
            {"id": 4, "name": "All or Nothing", "startsAt": iso(aon_starts), "endsAt": iso(now + 7200), "reveal": 3600000,
             "imgExt": "../x", "longDesc": "<i>One shot</i>", "limitedTimeMode": True},
            {"id": 5, "name": "Mag Dump", "startsAt": iso(now + 7200), "endsAt": iso(now + 9000), "reveal": 60000},
            {"id": 6, "name": "Over", "startsAt": iso(now - 9000), "endsAt": iso(now - 3600)}]


def write_feed(path, events):
    with open(path + ".tmp", "w") as fp:
        json.dump({"news": [], "events": [], "polls": [], "gameEvents": events}, fp)
    os.replace(path + ".tmp", path)


class Node:
    """Game server side of /mm/internal/ws: the welcome and the "game-events" pushes."""

    def __init__(self, port):
        self.url = "ws://127.0.0.1:%d/mm/internal/ws" % port
        self.welcome = None
        self.pushes = []

    async def __aenter__(self):
        self.ws = await websockets.connect(self.url, additional_headers={"Authorization": "Bearer " + SECRET})
        await self.ws.send(json.dumps({"type": "hello", "nodeId": "KLE%d" % os.getpid(), "region": "local", "publicHost": "127.0.0.1",
                                       "publicPort": 1, "version": "test"}))
        while True:
            msg = json.loads(await asyncio.wait_for(self.ws.recv(), 10))
            if msg.get("type") == "welcome":
                self.welcome = msg
                return self

    async def __aexit__(self, *a):
        await self.ws.close()

    async def wait_push(self, seconds):
        deadline = time.time() + seconds
        while time.time() < deadline:
            try:
                msg = json.loads(await asyncio.wait_for(self.ws.recv(), deadline - time.time()))
            except asyncio.TimeoutError:
                break
            if msg.get("type") == "game-events":
                return msg
        return None


def get_events(port):
    with urllib.request.urlopen("http://127.0.0.1:%d/data/events" % port, timeout=10) as r:
        return json.loads(r.read().decode()).get("data", {}).get("events")


async def scenario(port, feed, now):
    async with Node(port) as node:
        got = node.welcome.get("gameEvents")
        ids = [e.get("id") for e in got] if isinstance(got, list) else None
        check(ids == [8], "the welcome carries the running event only (not the revealed, future or ended ones): %s" % ids)
        e = got[0] if ids else {}
        check(e.get("name") == "Halloween" and e.get("startsAt", "").endswith("Z") and e.get("endsAt", "").endswith("Z"),
              "id, name, startsAt, endsAt (config.events start / end): %s" % {k: e.get(k) for k in ("id", "name", "startsAt", "endsAt")})
        check(e.get("reveal") == 86400000 and e.get("imgExt") == "webp" and e.get("longDesc") == ["Hunt the pumpkins", "<b>Collect</b> the skins"],
              "reveal, imgExt, longDesc (a list of text lines, kept as text)")
        check(e.get("noChallenges") is False and e.get("limitedTimeMode") is True and e.get("eventObjects") is True,
              "noChallenges / limitedTimeMode / eventObjects as booleans: %s" % [e.get(k) for k in ("noChallenges", "limitedTimeMode", "eventObjects")])
        rw = e.get("rewards", {})
        check(rw.get("skins") == [7540, 7541] and rw.get("weights") == [0, 0, 70, 25, 4, 0.1, 0.01],
              "rewards: integer skins only, seven rarity weights: %s" % rw)
        check(e.get("rewardChance") == 40 and e.get("eligibleMaps") == [0, 3], "rewardChance, eligibleMaps (integers): %s %s" %
              (e.get("rewardChance"), e.get("eligibleMaps")))
        check(e.get("killPoolsWeight") == 5 and e.get("killPools", [{}])[0].get("weight") == 100, "the kill pools still reach the servers")

        events = get_events(port)
        cids = [x.get("id") for x in events] if isinstance(events, list) else None
        check(cids == [8, 4], "GET /data/events: the running event and the one revealed early, soonest start first: %s" % cids)
        if cids == [8, 4]:
            c8, c4 = events
            check("killPools" not in c8 and "killPoolsWeight" not in c8, "no kill pools for the clients")
            check(c8.get("rewards", {}).get("skins") == [7540, 7541] and c8.get("eventObjects") is True and c8.get("imgExt") == "webp",
                  "the clients get the popup's fields (rewards, imgExt) and eventObjects")
            check("imgExt" not in c4 and c4.get("longDesc") == "<i>One shot</i>" and c4.get("reveal") == 3600000,
                  "an imgExt with a path is dropped; longDesc stays the feed's text (the client escapes it): %s" % c4)

        # All or Nothing starts (the feed is edited): the next ping pushes the new set to the servers
        time.sleep(1.1)  # the feed's mtime has a 1 s grain
        write_feed(feed, feed_events(now, time.time() - 5))
        pids = None
        deadline = time.time() + 8
        push = None
        while time.time() < deadline and pids != [8, 4]:
            push = await node.wait_push(deadline - time.time())
            pids = [x.get("id") for x in push.get("events", [])] if push else pids
        check(pids == [8, 4], "a \"game-events\" push with both running events: %s" % pids)
        if push and pids == [8, 4]:
            check(push["events"][1].get("limitedTimeMode") is True, "the pushed event keeps limitedTimeMode")


def main():
    work = tempfile.mkdtemp(prefix="kn-liveevents.", dir=os.path.join(os.path.expanduser("~"), ".cache"))
    feed = os.path.join(work, "community.json")
    now = time.time()
    write_feed(feed, feed_events(now, now + 1800))
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=SECRET)
    log_path = os.path.join(work, "brain.log")
    proc = None
    try:
        with open(log_path, "w") as log:
            proc = subprocess.Popen([BRAIN, "--no-db", "--brain.host=127.0.0.1", "--brain.port=0", "--brain.id=kle-%d" % os.getpid(),
                                     "--brain.ping_interval_s=1", "--brain.community.feed=" + feed],
                                    cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT)
        port = bound_port(log_path, proc)
        asyncio.run(scenario(port, feed, now))
    finally:
        if proc:
            proc.terminate()
            proc.wait(5)
    print("live events test: " + ("OK" if not FAILS else "%d failure(s)" % len(FAILS)))
    if FAILS:
        print("log kept in " + work)
    else:
        shutil.rmtree(work, ignore_errors=True)
    sys.exit(1 if FAILS else 0)


main()
