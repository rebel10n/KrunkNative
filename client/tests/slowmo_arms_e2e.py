#!/usr/bin/env python3
"""The first-person arms stay locked to the camera through the round-end slow motion (client/src/main.c).

  python3 -I client/tests/slowmo_arms_e2e.py BUILD_DIR      (run from the repository root)

The bug: "the player arms go out of sync with a camera for a certain period during/after the slow
mo effect". An input frame is only sampled (predicted, sent) once the summed frame delta reaches
half a netRate period, and the round-end slow motion scales that delta down to 0 (endAnim). Between
input frames the camera already showed the mouse look, but the player's direction (which the
viewmodel is posed from) kept the look of the last input frame: the slower the slow motion, the
more frames without an input frame and the further the arms fell behind; once the delta reached 0
no input frame came at all and the arms stayed off for the whole results screen. v7.1.4's gameLoop
turns the player with the camera on such frames (yDire = CONTROLS.yDr, xDire = cameraYaw).

An offline Kanji game (headless client_bin, input script, fixed 60 fps step): the player walks and
turns the mouse steadily, the console `end` command ends the round (slow motion, then the frozen
results), and KRUNKNATIVE_INPUT_LOG records per frame the viewmodel's look against the camera's
(arm_yaw_off / arm_pitch_off, input_script.c) and the arm sway (wr_x / wr_y). Checks:
  1. the slow motion ran (the world delta fell from the frame step to 0) while the look kept turning;
  2. the arms' look matches the camera's in normal play, during the slow motion and after it;
  3. the arm sway during and after the slow motion stays within its normal-play range.
Needs a headless GL device (KRUNKNATIVE_HEADLESS=1: EGL; KRUNKNATIVE_EGL_DEVICE picks it).
Prints PASS/FAIL lines; exit code 0 only when everything passed.
"""
import csv
import os
import shutil
import signal
import subprocess
import sys
import tempfile

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-slowmo-arms-", dir=CACHE if os.path.isdir(CACHE) else None)
FRAME_MS = 16.667
LOCK_TOLERANCE = 1e-3  # rad: the arms' look is the camera's (sway is on the arm anchor, not on the look)
results = []


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def script_text():
    # walk forward, `end` sets the match timer to 3 s (the round ends ~4 s in, endAnim is 5 s),
    # and turn 0.02 rad yaw / 0.004 rad pitch every other frame until the results are up
    lines = ["0 play", "300 down W", "800 con end"]
    t = 300.0
    while t < 9800.0:
        lines.append(f"{t:.3f} look 0.02 0.004")
        t += 2 * FRAME_MS
    lines.append("11000 quit")
    return "\n".join(sorted(lines, key=lambda line: float(line.split()[0]))) + "\n"


def run(timeout=240):
    script = os.path.join(WORK, "slowmo.script")
    with open(script, "w") as f:
        f.write(script_text())
    log = os.path.join(WORK, "client.log")
    csv_path = os.path.join(WORK, "input.csv")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": os.path.join(WORK, "cfg"),
                "XDG_STATE_HOME": os.path.join(WORK, "state"), "KRUNKNATIVE_NO_BROWSER": "1",
                "KRUNKNATIVE_INPUT_SCRIPT": script, "KRUNKNATIVE_FIXED_DELTA": str(FRAME_MS),
                "KRUNKNATIVE_INPUT_LOG": csv_path, "KRUNKNATIVE_WINDOW_SIZE": "640x360"})
    with open(log, "w") as out:
        p = subprocess.Popen(["stdbuf", "-oL", f"{BUILD}/client_bin", "--offline", "assets/maps/kanji.json"], stdout=out,
                             stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, start_new_session=True)
        try:
            p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
            check("client_bin finished its script", False, f"timeout, log {log}")
            return []
    if not os.path.exists(csv_path):
        return []
    with open(csv_path, newline="") as f:
        return [{k: float(v) for k, v in row.items()} for row in csv.DictReader(f)]


def peak(rows, key):
    return max((abs(r[key]) for r in rows), default=0.0)


try:
    rows = run()
    check("the input log has frames", rows, WORK)
    if rows:
        slow = [r for r in rows if 0.0 < r["delta"] < FRAME_MS - 0.5]
        frozen = [r for r in rows if r["delta"] == 0.0]
        start = slow[0]["t"] if slow else float("inf")
        normal = [r for r in rows if 300.0 <= r["t"] < start]
        check("normal play before the round end", len(normal) > 60, f"{len(normal)} frames")
        check("the round-end slow motion ran down to a frozen world", slow and frozen,
              f"{len(slow)} slow frames, {len(frozen)} frozen")
        turning = [r for r in slow + frozen if r["t"] < 9800.0]
        yaws = [r["cam_yaw"] for r in turning]
        check("the look kept turning through the slow motion", yaws and max(yaws) - min(yaws) > 0.5,
              f"camera yaw range {max(yaws) - min(yaws):.3f} rad" if yaws else "")
        for name, part in (("normal play", normal), ("the slow motion", slow), ("after the slow motion", frozen)):
            yaw, pitch = peak(part, "arm_yaw_off"), peak(part, "arm_pitch_off")
            check(f"{name}: the arms' look is the camera's", part and yaw <= LOCK_TOLERANCE and pitch <= LOCK_TOLERANCE,
                  f"max |yaw off| {yaw:.5f}, |pitch off| {pitch:.5f} rad over {len(part)} frames")
        for key in ("wr_x", "wr_y"):
            base = peak(normal, key)
            worst = max(peak(slow, key), peak(frozen, key))
            check(f"arm sway {key} stays in its normal-play range", worst <= base * 1.5 + 0.01,
                  f"normal {base:.4f}, slow motion / after {worst:.4f} rad")
finally:
    if all(results) and not os.environ.get("KN_TEST_KEEP"):
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print(f"logs kept in {WORK}")

print("slowmo_arms_e2e:", "PASS" if all(results) else "FAIL")
sys.exit(0 if all(results) else 1)
