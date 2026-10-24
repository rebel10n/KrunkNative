#!/usr/bin/env python3
"""Krunker v10.0.3 player poses on the models of a headless offline client (client/src/player_pose_render.c).

  python3 -I client/tests/player_pose_e2e.py BUILD_DIR      (run from the repository root)

The offline sandbox plays shared/tests/maps/v10/player_pose.json's scene with a server script of this test: the
local player holds an item in the left hand (holdL), the first bot holds one in both hands (hold), then takes a
pose that names no arm ({legs: 2}), then none. The client script hangs a cube on every player's "hands" and
"handL". The input script's `poses` probe (input_script.c) reads the models (deob-10.0.3.js line numbers):

  P1 hold turns the merged arms by armR's [-0.25, 0, 0] (applyPose :193428-193434): both pivots take it
  P2 a pose naming no arm keeps that turn on the merged arms (mergedArmMeshes is only put at rest by
     resetMergedArms, at the pose's end :195371-195377)
  P3 without a pose the arms are at rest again
  P4 syncHandL copies the first-person arm's layer (:193540-193546): the cube on our own "handL" is drawn by the
     viewmodel pass, the five other cubes ("hands", which syncHands gives no layer, and the bots') by the world's
  P5 the screenshot is written (viewed by hand)

Prints PASS/FAIL lines; exit code 0 only when everything passed. 77 = skipped (no client_bin).
"""
import json
import os
import re
import subprocess
import sys

BUILD = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.path.abspath("cmake-build-debug")
REPO = os.getcwd()
CLIENT = os.path.join(BUILD, "client_bin")
if not os.path.exists(CLIENT):
    print(f"needs client_bin in {BUILD}: skip")
    sys.exit(77)
WORK = os.path.join(BUILD, "player_pose_e2e")
os.makedirs(os.path.join(WORK, "cfg"), exist_ok=True)
results = []


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + str(detail)) if detail else ''}", flush=True)


SERVER = r'''
num n = 0;
str bot = "";
num clock = 0;
public action onPlayerSpawn(str id) {
    obj p = GAME.PLAYERS.findByID(id);
    n += 1;
    if (n == 1) { p.pose = {holdL: 1}; return; }
    p.position.x = -20 + n * 8;
    p.position.z = 0;
    p.position.y = 0;
    if (n == 2) { bot = id; p.pose = {hold: 1}; }
}
public action update(num delta) {
    clock += delta;
    if (bot == "") { return; }
    obj p = GAME.PLAYERS.findByID(bot);
    if (clock > 5000 && clock < 5100) { p.pose = {legs: 2}; }
    if (clock > 7500 && clock < 7600) { p.pose = 0; }
}
'''

INPUT = """0 play
300 con bot_stop on
400 con bot_add 2
3200 poses
5600 poses
5650 shot {shot}
8100 poses
8200 quit
"""


def main():
    with open(os.path.join(REPO, "shared/tests/maps/v10/player_pose.json")) as f:
        m = json.load(f)
    m["scripts"]["server"] = SERVER
    m["scripts"]["client"] = m["scripts"]["client"].replace("lengthOf ps < 5", "lengthOf ps < 3")  # us and two bots
    map_path = os.path.join(WORK, "player_pose_e2e.json")
    with open(map_path, "w") as f:
        json.dump(m, f)
    shot = os.path.join(WORK, "player_pose_e2e.png")
    if os.path.exists(shot):
        os.remove(shot)
    script = os.path.join(WORK, "input.txt")
    with open(script, "w") as f:
        f.write(INPUT.format(shot=shot))
    env = dict(os.environ)
    for k in ("DISPLAY", "WAYLAND_DISPLAY"):
        env.pop(k, None)
    env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": os.path.join(WORK, "cfg"), "KRUNKNATIVE_HIDE_UI": "1",
                "KRUNKNATIVE_INPUT_SCRIPT": script, "KRUNKNATIVE_FIXED_DELTA": "16", "KRUNKNATIVE_CAMERA": "0,8,24,-0.15,0"})
    try:
        out = subprocess.run([CLIENT, "--offline", map_path], cwd=REPO, env=env, capture_output=True, text=True, timeout=180)
        log = out.stdout + out.stderr
    except subprocess.TimeoutExpired as e:
        log = (e.stdout or b"").decode(errors="replace") + (e.stderr or b"").decode(errors="replace")
    with open(os.path.join(WORK, "client.log"), "w") as f:
        f.write(log)
    probes = re.findall(r"\[input_script\] poses t=(\d+) fps=(\d+) world=(\d+)(.*)", log)
    check("three poses probes", len(probes) == 3, f"{len(probes)} (log: {WORK}/client.log)")
    if len(probes) != 3:
        return

    def record(probe, uid):
        m = re.search(r" %d:has=0x([0-9a-f]+) hold=\S+ l=(\S+) r=(\S+) merged=(\S+)" % uid, probe[3])
        if not m:
            return None
        vec = lambda s: [float(v) for v in s.split(",")]
        return int(m.group(1), 16), vec(m.group(2)), vec(m.group(3)), vec(m.group(4))

    bots = [int(u) for u in re.findall(r" (\d+):has=", probes[0][3])]
    bot = next((u for u in bots if record(probes[0], u) and record(probes[0], u)[0] == 0x10), None)  # PLAYER_POSE_HOLD
    check("a bot holds (pose hold)", bot is not None, probes[0][3].strip())
    if bot is None:
        return
    has, l, r, merged = record(probes[0], bot)
    check("P1 hold: both arms turned by armR's -0.25", abs(merged[0] + 0.25) < 0.02 and l == merged and r == merged, f"l={l} r={r} merged={merged}")
    has, l, r, merged = record(probes[1], bot)
    check("P2 legs only: the merged arms keep hold's turn", has == 0x1 and abs(merged[0] + 0.25) < 0.02 and l == merged and r == merged,
          f"has=0x{has:x} l={l} r={r} merged={merged}")
    has, l, r, merged = record(probes[2], bot)
    check("P3 no pose: the arms at rest", has == 0 and all(abs(v) < 1e-4 for v in l + r + merged), f"has=0x{has:x} l={l} r={r} merged={merged}")
    fps, world = int(probes[1][1]), int(probes[1][2])
    check("P4 our own handL cube in the viewmodel pass, the five others in the world's", fps == 1 and world == 5, f"fps={fps} world={world}")
    check("P5 screenshot", os.path.exists(shot) and os.path.getsize(shot) > 10000, shot)


if __name__ == "__main__":
    main()
    print(f"{sum(results)}/{len(results)} passed")
    sys.exit(0 if results and all(results) else 1)
