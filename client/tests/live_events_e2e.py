#!/usr/bin/env python3
"""Krunker v10.0.3 live events on the client, end to end (doc/v10/events.md; client/src/ui/v10_events.c,
client/src/event_objects.c, v10_rewards.c; server/src/live_events_server.c).

  python3 -I client/tests/live_events_e2e.py BUILD_DIR      (run from the repository root)

  1. --render-scenario, the menu with state.events (config.events at a fixed clock): window.eventPopup for the
     round's event 8 (its img/events/event_8.webp header, the reward tiles), getActiveEvent's fallback to an event
     that reveals early (round event 0), the Challenges window's Event Challenges section and the event's category.
  2. server_bin on loopback with gameserver.force_event (v10 forceEvent) on a scratch copy of Lostworld with one
     EVENT prefab object (both processes read the scratch assets tree): event 3 shows the EVENT mesh at our spawn
     ("start"), event 8 hides it.
  3. offline (the client's sandbox, a custom game): KRUNKNATIVE_EVENT=8 forces the event, KRUNKNATIVE_GAME_EVENTS
     gives the client its list; `call eventPopup` opens event 8's popup; the EVENT mesh stays hidden (v10's
     generate skips EVENT objects in custom games).
Needs a headless GL device (KRUNKNATIVE_HEADLESS=1: EGL). Prints PASS/FAIL lines; exit 0 only when all passed.
"""
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "server", "tests"))
from kn_proto import free_ports  # noqa: E402

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-live-events-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
ASSETS = os.path.abspath(os.environ.get("KRUNKNATIVE_ASSETS_DIR", "assets"))
SPOT = "-19 50.5 140"  # Lostworld's floor; the EVENT object stands 12 units ahead along +x (yaw -PI/2)
NOW_MS = 1793000000000  # 2026-10-26T07:33:20Z: event 8 runs, event 4 starts in 26 minutes
results = []
procs = []


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def read(path):
    try:
        with open(path, errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def client_env(name, extra=None):
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": os.path.join(WORK, f"{name}-cfg"),
                "XDG_STATE_HOME": os.path.join(WORK, f"{name}-state"), "KRUNKNATIVE_NO_BROWSER": "1",
                "KRUNKNATIVE_AUDIO_BACKEND": "null", "KRUNKNATIVE_WINDOW_SIZE": "1280x720"})
    env.update(extra or {})
    return env


def run(name, args, env, timeout=180):
    log = os.path.join(WORK, f"{name}.log")
    with open(log, "w") as out:
        p = subprocess.Popen(["stdbuf", "-oL", f"{BUILD}/client_bin"] + args, stdout=out, stderr=subprocess.STDOUT,
                             stdin=subprocess.DEVNULL, env=env, start_new_session=True)
        procs.append(p)
        try:
            p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
            check(f"{name}: client_bin finished", False, f"timeout, log {log}")
    return read(log)


def run_script(name, args, script, extra=None):
    path = os.path.join(WORK, f"{name}.script")
    with open(path, "w") as f:
        f.write(script.strip() + "\n")
    return run(name, args, client_env(name, dict({"KRUNKNATIVE_INPUT_SCRIPT": path, "KRUNKNATIVE_FIXED_DELTA": "16.667"}, **(extra or {}))))


def stop(p):
    if p.poll() is None:
        try:
            os.killpg(p.pid, signal.SIGTERM)
        except OSError:
            pass
        try:
            p.wait(timeout=5)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()


HALLOWEEN = {"id": 8, "name": "Halloween", "startsAt": "2026-10-25T00:00:00Z", "endsAt": "2026-11-02T00:00:00Z", "imgExt": "webp",
             "longDesc": ["Play public matches for a chance at the event's skins", "Complete the event challenges"],
             "rewards": {"skins": [7540, 7541, 7543, 7545]}, "rewardChance": 100}
AON = {"id": 4, "name": "All or Nothing", "startsAt": "2026-10-26T08:00:00Z", "endsAt": "2026-10-28T00:00:00Z", "reveal": 86400000,
       "longDesc": "One shot, one kill.", "noChallenges": True, "limitedTimeMode": True}


def scenario(name, state):
    base = {"menu": True, "connected": True, "ready": True, "inGame": False, "name": "Player", "classIndex": 0, "region": "...",
            "mapName": "Kanji", "mapFile": "maps/311_kanji.json", "gameName": "ffa on Kanji", "anim_time": 0.8}
    path = os.path.join(WORK, f"{name}.json")
    with open(path, "w") as f:
        json.dump({"size": [1280, 720], "background": "#000000", "state": dict(base, **state), "frames": 3}, f)
    png = os.path.join(WORK, f"{name}.png")
    log = run(name, ["--render-scenario", path, png], client_env(name))
    return log, os.path.exists(png)


def challenge(ci, evt, ky, val, pv, pt, ic):
    return {"CI": ci, "T": "event", "DAT": {"evt": evt, "ky": ky, "val": val}, "REW": {"kr": 50, "sk": [7540]}, "IC": ic, "N": "",
            "PV": pv, "PT": pt}


def scratch_assets():
    """assets/ as symlinks, with Lostworld's file a copy holding one EVENT object near SPOT"""
    root = os.path.join(WORK, "assets")
    os.makedirs(os.path.join(root, "maps", "official"))
    for e in os.listdir(ASSETS):
        if e != "maps":
            os.symlink(os.path.join(ASSETS, e), os.path.join(root, e))
    for e in os.listdir(os.path.join(ASSETS, "maps")):
        if e != "official":
            os.symlink(os.path.join(ASSETS, "maps", e), os.path.join(root, "maps", e))
    for e in os.listdir(os.path.join(ASSETS, "maps", "official")):
        src = os.path.join(ASSETS, "maps", "official", e)
        dst = os.path.join(root, "maps", "official", e)
        if e != "07_lostworld.json":
            os.symlink(src, dst)
            continue
        with open(src) as f:
            m = json.load(f)
        m["objects"].append({"p": [-7, 50.5, 140], "s": [6, 8, 6], "i": 40})
        with open(dst, "w") as f:
            json.dump(m, f)
    return root


EVENT_RE = re.compile(r"\[events\] EVENT objects: (\d+), (\d+) shown \(event (\d+)(, custom game)?\)")


def event_lines(log):
    return [(int(a), int(b), int(c), bool(d)) for a, b, c, d in EVENT_RE.findall(log)]


try:
    # ------------------------------------------------------------------ 1. render scenarios
    events = {"list": [HALLOWEEN, AON], "index": 8, "now": NOW_MS}
    log, shot = scenario("popup", {"events": events, "windowActions": [["eventPopup"]]})
    check("popup: window.eventPopup opens event 8 with its 4 reward skins", shot and
          '[events] eventPopup: event 8 "Halloween" (running, 4 reward skin(s))' in log, "log " + os.path.join(WORK, "popup.log"))
    log, shot = scenario("reveal", {"events": dict(events, index=0), "windowActions": [["eventPopup"]]})
    check("popup: no round event: getActiveEvent's revealed event 4 ('Starts in')",
          '[events] eventPopup: event 4 "All or Nothing" (upcoming' in log, "log " + os.path.join(WORK, "reveal.log"))
    account = {"name": "Player", "score": 50000, "funds": 2500, "premium": 0, "badges": [], "stats": {},
               "challenges": [challenge(900, 8, "k", 50, 12, 0, 1), challenge(901, 8, "hs", 10, 10, 1, 2), challenge(902, 8, "score", 5000, 0, 0, 3)]}
    log, shot = scenario("chall-home", {"events": events, "window": 33, "account": account})
    check("challenges: the Event Challenges section (1 of 3 done) while event 8 runs", "[challenges] Event Challenges: 1/3" in log,
          "log " + os.path.join(WORK, "chall-home.log"))
    log, shot = scenario("chall-off", {"events": dict(events, now=NOW_MS + 30 * 86400000), "window": 33, "account": account})
    check("challenges: no running event, no Event Challenges section", "[challenges] window home" in log and "Event Challenges" not in log)
    log, shot = scenario("chall-event", {"events": events, "window": 33, "account": account,
                                         "windowActions": [["windows.32.openChallengeCat", "event", 8]]})
    check("challenges: the event's category opens", "[challenges] window event" in log)

    # ------------------------------------------------------------------ 2. server_bin, forceEvent 3 / 8
    assets = scratch_assets()
    with open(os.path.join(WORK, "none.json"), "w") as f:
        f.write("{}")
    script = f"""
0 play
300 tp {SPOT} -1.571 -0.1
1500 tp {SPOT} -1.571 -0.1
2000 shot {WORK}/egg-%s.png
2200 quit
"""
    for event, shown in ((3, 1), (8, 0)):
        port = free_ports(1)[0]
        cfg = os.path.join(WORK, f"server-{event}.cfg")
        with open(cfg, "w") as f:
            f.write(f'hostname "Live events e2e"\nport {port}\nregion local\nmaxplayers 8\nmapcycle Lostworld\nmode ffa\nround_seconds 900\n')
        slog = os.path.join(WORK, f"server-{event}.log")
        with open(slog, "w") as out:
            server = subprocess.Popen([f"{BUILD}/server_bin", "--headless", "--cfg", cfg, "--config", os.path.join(WORK, "none.json"),
                                       f"--gameserver.force_event={event}"], stdout=out, stderr=subprocess.STDOUT,
                                      stdin=subprocess.DEVNULL, env=dict(os.environ, KRUNKNATIVE_ASSETS_DIR=assets), start_new_session=True)
        procs.append(server)
        end = time.time() + 20
        while time.time() < end and "Listening on port" not in read(slog):
            time.sleep(0.2)
        check(f"event {event}: server_bin listening", "Listening on port" in read(slog), slog)
        log = run_script(f"online-{event}", ["--connect", f"127.0.0.1:{port}"], script % event, {"KRUNKNATIVE_ASSETS_DIR": assets})
        lines = event_lines(log)
        check(f"event {event}: the map's EVENT object is built (1 mesh, not a map object)", lines and lines[0][0] == 1, str(lines))
        check(f"event {event}: at our spawn it is {'shown' if shown else 'hidden'} (toggleMeshGroup event_0)",
              any(n == 1 and s == shown and e == event and not c for n, s, e, c in lines), str(lines))
        stop(server)

    # ------------------------------------------------------------------ 3. offline: KRUNKNATIVE_EVENT, KRUNKNATIVE_GAME_EVENTS
    feed = os.path.join(WORK, "feed.json")
    now = time.time()
    iso = lambda t: time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(t))
    with open(feed, "w") as f:
        json.dump({"gameEvents": [dict(HALLOWEEN, startsAt=iso(now - 3600), endsAt=iso(now + 3600))]}, f)
    log = run_script("offline", ["--offline", os.path.join(assets, "maps", "official", "07_lostworld.json")], f"""
0 play
1200 call eventPopup
1500 quit
""", {"KRUNKNATIVE_ASSETS_DIR": assets, "KRUNKNATIVE_EVENT": "8", "KRUNKNATIVE_GAME_EVENTS": feed})
    check("offline: the client reads KRUNKNATIVE_GAME_EVENTS", "[events] 1 live event(s) from KRUNKNATIVE_GAME_EVENTS: 8" in log)
    check("offline: KRUNKNATIVE_EVENT=8 is the round's event (forceEvent): its popup opens",
          '[events] eventPopup: event 8 "Halloween" (running' in log, "log " + os.path.join(WORK, "offline.log"))
    lines = event_lines(log)
    check("offline: the sandbox is a custom game: the EVENT mesh stays hidden", lines and lines[-1][1] == 0 and lines[-1][3], str(lines))
finally:
    for p in procs:
        stop(p)

ok = all(results)
print(f"{'OK' if ok else 'FAILED'}: {sum(results)}/{len(results)} checks", flush=True)
if ok:
    shutil.rmtree(WORK, ignore_errors=True)
else:
    print("work dir kept: " + WORK)
sys.exit(0 if ok else 1)
