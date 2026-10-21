#!/usr/bin/env python3
"""The Purple Drank's arms in third person: the off arm's elbow bends the same way as the trigger arm's
(client/src/player_mesh.c player_arm_reach, the pole in nettspend.h NETTSPEND_OFF_ELBOW_POLE_*):

  python3 -I client/tests/nettspend_arms_e2e.py BUILD_DIR      (run from the repository root)

An offline Kanji session (headless client_bin, input script, fixed 60 fps step): the local player holds the
Drank in third person, faces -z (yaw 0) at the feet (-343, -143, -28), and the `arms` verb reads each arm's
shoulder, elbow and wrist as drawn (world space). Checks:
  1. the trigger elbow bends down and back: the elbow's offset from its shoulder-to-wrist line points into
     the down-back half (perpendicular part dotted with (0, -1, +1) > 0);
  2. the off elbow bends the same way (down and back), not forward: a hanging arm's forearm swings forward;
  3. the off hand is forward of the hip: its wrist is ahead of the torso's front face (z less than the
     feet's z minus the torso's half depth, 0.65: chest_scale 1.3 in constants_v10.c; -z is forward at yaw 0);
  4. the trigger hand keeps its forward reach (the cup arm, af38340d): its wrist is over 3 units in front.
Needs a headless GL device (KRUNKNATIVE_HEADLESS=1: EGL; KRUNKNATIVE_EGL_DEVICE picks it).
Prints PASS/FAIL lines; exit code 0 only when everything passed.
"""
import math
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-nettspend-arms-", dir=CACHE if os.path.isdir(CACHE) else None)
FRAME_MS = 16.667
FEET = (-343.0, -143.0, -28.0)  # the pose: feet x / y / z (yaw 0 faces -z)
TORSO_HALF_DEPTH = 0.65         # the body cube's half depth (chest_scale 1.3, constants_v10.c; the cube is centred on the body)
results = []


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def sub(a, b):
    return tuple(x - y for x, y in zip(a, b))


def dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def unit(a):
    n = math.sqrt(dot(a, a))
    return tuple(x / n for x in a)


def bend(shoulder, elbow, wrist):
    """The elbow's offset from the shoulder-to-wrist line (perpendicular to it), as a vector."""
    chord = unit(sub(wrist, shoulder))
    arm = sub(elbow, shoulder)
    along = dot(arm, chord)
    return tuple(a - along * c for a, c in zip(arm, chord))


POLE = unit((0.0, -1.0, 1.0))  # down and back: the body faces -z, so back is +z


def parse_arms(text):
    """{'trigger': (shoulder, elbow, wrist), 'off': ...} from the first `arms` line set (t = 1500)."""
    out = {}
    for m in re.finditer(r"\[input_script\] arms t=(\d+) (trigger|off) shoulder=(\S+) elbow=(\S+) wrist=(\S+)", text):
        if m.group(1) != "1500" or m.group(2) in out:
            continue
        vals = [tuple(float(v) for v in m.group(i).split(",")) for i in (3, 4, 5)]
        out[m.group(2)] = tuple(vals)
    return out


def main():
    if not os.path.isfile(f"{BUILD}/client_bin"):
        print(f"no {BUILD}/client_bin", file=sys.stderr)
        return 2
    script = os.path.join(WORK, "arms.script")
    with open(script, "w") as f:
        f.write("0 play\n300 om add 67\n700 con thirdperson\n"
                f"1000 pose {FEET[0]:g} {FEET[1]:g} {FEET[2]:g} 0 0\n"
                "1500 arms\n1600 quit\n")
    log = os.path.join(WORK, "client.log")
    cfg = os.path.join(WORK, "cfg")
    os.makedirs(cfg, exist_ok=True)
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": cfg, "XDG_STATE_HOME": os.path.join(WORK, "state"),
                "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_INPUT_SCRIPT": script, "KRUNKNATIVE_FIXED_DELTA": str(FRAME_MS),
                "KRUNKNATIVE_WINDOW_SIZE": "640x360", "KRUNKNATIVE_AUDIO_BACKEND": "null"})
    with open(log, "w") as out:
        proc = subprocess.Popen(["stdbuf", "-oL", f"{BUILD}/client_bin", "--offline", "assets/maps/official/10_kanji.json"], stdout=out,
                                stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, start_new_session=True)
    try:
        end = time.time() + 240.0
        while time.time() < end and proc.poll() is None:
            time.sleep(0.5)
    finally:
        if proc.poll() is None:
            os.killpg(proc.pid, signal.SIGKILL)
        proc.wait()
    with open(log, errors="replace") as f:
        text = f.read()
    arms = parse_arms(text)
    check("the third-person arms are drawn (trigger and off)", {"trigger", "off"} <= set(arms), ", ".join(sorted(arms)) or "no arms line")
    if {"trigger", "off"} <= set(arms):
        for name in ("trigger", "off"):
            s, e, w = arms[name]
            b = bend(s, e, w)
            side = dot(b, POLE)
            check(f"{name} elbow bends down and back (the same way as the trigger's)", side > 0.0,
                  f"elbow {tuple(round(v, 2) for v in e)}, wrist {tuple(round(v, 2) for v in w)}, bend side {side:+.2f}")
        off_wrist = arms["off"][2]
        front = FEET[2] - TORSO_HALF_DEPTH
        check("the off hand is forward of the hip (ahead of the torso's front face)", off_wrist[2] < front,
              f"wrist z {off_wrist[2]:.2f}, torso front z {front:.2f}")
        trig_wrist = arms["trigger"][2]
        check("the cup hand keeps its forward reach (over 3 units in front)", FEET[2] - trig_wrist[2] > 3.0,
              f"reach {FEET[2] - trig_wrist[2]:.2f}")
    print(f"{sum(results)}/{len(results)} passed")
    if results and all(results) and not os.environ.get("KN_TEST_KEEP"):
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print(f"logs kept in {WORK}")
    return 0 if results and all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
