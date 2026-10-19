#!/usr/bin/env python3
"""Offline play: a dead local player gets no live-world combat packets, as online (v7.1.4).

  python3 -I client/tests/offline_dead_filter_e2e.py BUILD_DIR      (run from the repository root)

Offline play is the sandbox server (server/include/server_sandbox.h) inside client_bin, joined
over the in-memory loopback, so the server's live-world filter (server/src/net.c
net_live_only_packet / net_client_in_live_world) is what keeps the death screen quiet there too.
sandbox_test drives that core directly; this drives the real client: an offline Kanji game
(headless client_bin, input script, fixed 60 fps step) with eight bots, the player dies
(`con kill`), stays on the death screen, then plays again (`respawn`). KRUNKNATIVE_PACKET_LOG=1
logs every packet the client receives; between the player's own KILL and its next own SPAWN:
  - no STATE, SHOT, PROJECTILE, AI_SHOT, EXPLOSION, SOUND, OBJECT_ANIM or AI_ANIM;
  - still the kill feed (KILL), health (HEALTH) and projectile ends (PROJECTILE_END);
and after the respawn the player sync and the bots' tracers come back.
Needs a headless GL device (KRUNKNATIVE_HEADLESS=1: EGL; KRUNKNATIVE_EGL_DEVICE picks it).
Prints PASS/FAIL lines; exit code 0 only when everything passed.
"""
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-offline-dead-", dir=CACHE if os.path.isdir(CACHE) else None)
results = []

# NetPacketType ids (shared/include) of the live-world packets
LIVE_ONLY = {6: "STATE", 21: "SHOT", 22: "PROJECTILE", 164: "AI_SHOT", 24: "EXPLOSION", 102: "SOUND",
             133: "OBJECT_ANIM", 163: "AI_ANIM"}
KILL, HEALTH, PROJECTILE_END = 10, 12, 23

# times from `play`: eight bots fight, the player dies at 10 s and plays again at 22 s
SCRIPT = """0 play
200 con bot_add 8
10000 con kill
22000 respawn
32000 quit
"""


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def run(timeout=240):
    script = os.path.join(WORK, "dead.script")
    with open(script, "w") as f:
        f.write(SCRIPT)
    log = os.path.join(WORK, "dead.log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": os.path.join(WORK, "cfg"),
                "XDG_STATE_HOME": os.path.join(WORK, "state"), "KRUNKNATIVE_NO_BROWSER": "1",
                "KRUNKNATIVE_AUDIO_BACKEND": "null", "KRUNKNATIVE_PACKET_LOG": "1",
                "KRUNKNATIVE_INPUT_SCRIPT": script, "KRUNKNATIVE_FIXED_DELTA": "16.667",
                "KRUNKNATIVE_WINDOW_SIZE": "640x360"})
    with open(log, "w") as out:
        p = subprocess.Popen(["stdbuf", "-oL", f"{BUILD}/client_bin", "--offline", "assets/maps/official/10_kanji.json"], stdout=out,
                             stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, start_new_session=True)
        try:
            p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
            check("client_bin finished its script", False, f"timeout, log {log}")
    return log


# received packets ("<=" lines of client_net_log_packet) by phase: alive, dead, respawned
def phases(log):
    me, phase, counts = None, "menu", {}
    with open(log, errors="replace") as f:
        for line in f:
            if "<=" not in line or "{" not in line:
                continue
            try:
                p = json.loads(line[line.index("{"):].strip())
            except ValueError:
                continue
            if p.get("type") == "spawn" and p.get("is_you") and p.get("active"):
                me = p["uid"]
                phase = "respawned" if phase == "dead" else "alive" if phase == "menu" else phase
            elif p.get("type") == "kill" and me is not None and p.get("victim") == me and phase == "alive":
                phase = "dead"
            bucket = counts.setdefault(phase, {})
            bucket[p.get("id")] = bucket.get(p.get("id"), 0) + 1
    return counts


try:
    log = run()
    counts = phases(log)
    alive, dead, back = counts.get("alive", {}), counts.get("dead", {}), counts.get("respawned", {})
    check("the player spawned, died and spawned again", alive and dead and back, log)
    check("alive: the bots' tracers reach the player (the check can see a leak)", alive.get(21, 0) > 0,
          f"{alive.get(21, 0)} SHOT")
    leaked = {LIVE_ONLY[t]: n for t, n in dead.items() if t in LIVE_ONLY}
    check("dead: no live-world packet", dead and not leaked, json.dumps(leaked) if leaked else f"{sum(dead.values())} packets")
    check("dead: the kill feed still arrives", dead.get(KILL, 0) > 0, f"{dead.get(KILL, 0)} KILL")
    check("dead: health still arrives (v7 \"h\" to all; projectile ends too)", dead.get(HEALTH, 0) > 0,
          f"{dead.get(HEALTH, 0)} HEALTH, {dead.get(PROJECTILE_END, 0)} PROJECTILE_END")
    check("respawned: the player sync and the tracers come back", back.get(6, 0) > 0 and back.get(21, 0) > 0,
          f"{back.get(6, 0)} STATE, {back.get(21, 0)} SHOT")
finally:
    if all(results) and not os.environ.get("KN_TEST_KEEP"):
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print(f"logs kept in {WORK}")

print("offline_dead_filter_e2e:", "PASS" if all(results) else "FAIL")
sys.exit(0 if all(results) else 1)
