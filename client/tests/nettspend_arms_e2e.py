#!/usr/bin/env python3
"""The Purple Drank's arms: the off arm's rest beside the hip in third and first person, and the elbow bend
(client/src/player_mesh.c player_arm_reach, the pole in nettspend.h NETTSPEND_OFF_ELBOW_POLE_*):

  python3 -I client/tests/nettspend_arms_e2e.py BUILD_DIR      (run from the repository root)

Two offline Kanji sessions (headless client_bin, input script, fixed 60 fps step): the local player holds the
Drank, faces -z (yaw 0) at the feet (-343, -143, -28), and the `arms` verb reads each arm's shoulder, elbow and
wrist as drawn (world space).

Third person (the body's view):
  1. the trigger elbow bends down and back: the elbow's offset from its shoulder-to-wrist line points into
     the down-back half (perpendicular part dotted with (0, -1, +1) > 0);
  2. the off elbow bends the same way (down and back), not forward: a hanging arm's forearm swings forward;
  3. the off hand is beside the body, not behind it: its wrist is not behind the torso's back face (z not more than
     the feet's z plus the torso's half depth, 0.65: chest_scale 1.3 in constants_v10.c; -z is forward at yaw 0);
     3b. the off wrist is in front of the hip line: 1.0 to 1.5 in front of the feet's z (-28 at the pose; the user's
     2026-10-22 "can we move the arm forward more": the rest was 0.26 in front (-0.35), then 0.58 (-0.65), and now
     1.15 (-1.25, z -29.15). That is past the torso's front face (0.65 in front of the hip line): a deliberate
     third-person move, so the old "not ahead of the torso's front face" check is gone, see 3c);
  3c. the off shoulder joint sits on the torso's depth, near its middle: within 0.325 (half the torso's half depth) of
     the body's centre z (the feet's z). The joint was at 0.56 behind it, on the back face, so the upper arm stuck out
     behind the torso and looked detached (the user's 2026-10-22 screenshot; the joint now -28.24 at the pose, moved
     forward with the hand so the elbow stays under the limit in 7);
  4. the trigger hand keeps its forward reach (the cup arm, af38340d): its wrist is over 3 units in front.
  5. the off hand is not below the hip (its wrist at least 0.25 under the pelvis height, 4.55 above the feet);
  6. the off hand is at hip height or a little above: its wrist at least 5.3 above the feet;
  7. the off elbow is no more than 2.25 below the off shoulder: the hand hangs beside the hip at hip height, and
     a bent arm of this length puts its elbow about half the wrist's drop below the shoulder (the user's
     "arms too low" / "sag", 2026-10-21). The rest's elbow is 2.24 below at the 1.15 rest (2.07 at the old 0.58 rest).
First person (the viewmodel, the arms the local player sees; the off arm is hidden except in the refill):
  8. the refill runs: the pour-up reaches full strength (the off hand has the bottle over the cup);
  9. the off hand at rest is beside the hip, as in third person: its wrist is within 0.3 of the third-person
     rest's height above the feet, within 0.4 of the first-person rest's depth (z -28.60, FP_REST_DEPTH: first person
     keeps NETTSPEND_OFF_HAND_FWD; the third-person move does not apply to it), not ahead of the torso's front face, and its elbow
     is no more than 2.25 below the shoulder (the first person's rest was 4.5 above the feet and 1.4 in front of the body, the forward swing);
  10. the off elbow bends down and back at rest (the same pole as third person);
  11. the refill ends where it started: the off wrist after the pour is within 0.1 of its rest on every axis, and
     beside the hip by the same checks as 9 (the refill's end pose is the rest, not a forward swing).
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
HIP_HEIGHT = 4.55               # the Drank's stream pelvis_height (kn_drank.c): the hip, above the feet
OFF_WRIST_MIN = 5.3             # the off hand's rest: at hip height or a little above (above the feet)
OFF_ELBOW_MAX_DROP = 2.25       # the off elbow: no more than this below the off shoulder (the rest sits 2.24 below at the 1.15 rest; 2.07 at the old 0.58 rest)
HIP_LINE_NEAR = 1.0             # the off wrist in front of the hip line: at least this far in front of the feet's z ...
HIP_LINE_FAR = 1.5              # ... and no more than this (2026-10-22 "move the arm forward more": -0.35 was 0.26 in front, -0.65 0.58, -1.25 now 1.15, z -29.15)
SHOULDER_DEPTH_TOL = 0.325      # the off shoulder joint within this of the body's centre z (the torso's middle half; it was 0.56 behind)
FP_REST_DEPTH = -28.60          # the first-person rest's wrist depth (z): unchanged (NETTSPEND_OFF_HAND_FWD -0.35); the third person's move is not first person's
FP_DEPTH_TOL = 0.4              # the first-person rest's wrist depth against FP_REST_DEPTH (the viewmodel's perspective)
REST_HEIGHT_TOL = 0.3           # the first-person rest's wrist height above the feet, against the third person's
REFILL_TOL = 0.1                # the refill's end pose against its rest, per axis
FP_REST_MS = 1400               # first person: the arms at rest (before the sip)
FP_POUR_MS = 3000               # first person: the pour-up mid-way (progress 0.5 of the 1.4 s reload from 2300)
FP_END_MS = 3850                # first person: after the pour-up (the reload ends at 3700)
TP_REST_MS = 1500
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


def parse_arms(text, at):
    """{'trigger': (shoulder, elbow, wrist), 'off': ...} from the `arms` lines drawn at t = at (the first set from then on)."""
    out = {}
    for m in re.finditer(r"\[input_script\] arms t=(\d+) (trigger|off) shoulder=(\S+) elbow=(\S+) wrist=(\S+)", text):
        t = int(m.group(1))
        if not (at <= t <= at + 20) or m.group(2) in out:
            continue
        vals = [tuple(float(v) for v in m.group(i).split(",")) for i in (3, 4, 5)]
        out[m.group(2)] = tuple(vals)
    return out


def parse_pour(text, at):
    """The pour's strength (nettspend_pour_strength) at t = at, or None."""
    for m in re.finditer(r"\[input_script\] pour t=(\d+) strength=(-?[\d.]+)", text):
        if at <= int(m.group(1)) <= at + 20:
            return float(m.group(2))
    return None


def run_client(tag, lines):
    """One headless session of the Drank on the Kanji street; returns its log text."""
    script = os.path.join(WORK, f"{tag}.script")
    with open(script, "w") as f:
        f.write("\n".join(lines) + "\n")
    log = os.path.join(WORK, f"{tag}.log")
    cfg = os.path.join(WORK, f"{tag}-cfg")
    os.makedirs(cfg, exist_ok=True)
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": cfg, "XDG_STATE_HOME": os.path.join(WORK, f"{tag}-state"),
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
        return f.read()


def fmt(p):
    return tuple(round(v, 2) for v in p)


def beside_hip(name, off, tp_height):
    """The off hand at rest or at a refill's end: beside the hip, as in third person (checks 3, 6, 7 and the first-person 9)."""
    shoulder, elbow, wrist = off
    front = FEET[2] - TORSO_HALF_DEPTH
    check(f"{name}: the off hand is not ahead of the torso's front face", wrist[2] >= front,
          f"wrist z {wrist[2]:.2f}, torso front z {front:.2f}")
    check(f"{name}: the off hand's depth is level with the first-person rest ({FP_REST_DEPTH:.2f}, +-{FP_DEPTH_TOL})",
          abs(wrist[2] - FP_REST_DEPTH) <= FP_DEPTH_TOL, f"wrist z {wrist[2]:.2f}")
    height = wrist[1] - FEET[1]
    check(f"{name}: the off hand is at hip height, level with the third-person rest ({tp_height:.2f} above the feet, +-{REST_HEIGHT_TOL})",
          abs(height - tp_height) <= REST_HEIGHT_TOL and height >= OFF_WRIST_MIN,
          f"wrist {height:.2f} above the feet")
    drop = shoulder[1] - elbow[1]
    check(f"{name}: the off elbow is no more than {OFF_ELBOW_MAX_DROP} below its shoulder", drop <= OFF_ELBOW_MAX_DROP,
          f"elbow {drop:.2f} below the shoulder")


def main():
    if not os.path.isfile(f"{BUILD}/client_bin"):
        print(f"no {BUILD}/client_bin", file=sys.stderr)
        return 2
    pose = f"1000 pose {FEET[0]:g} {FEET[1]:g} {FEET[2]:g} 0 0"

    # third person: the rest (the trigger elbow, the off hand, the cup's reach)
    tp_text = run_client("tp", ["0 play", "300 om add 67", "700 con thirdperson", pose, f"{TP_REST_MS} arms", f"{TP_REST_MS + 100} quit"])
    tp = parse_arms(tp_text, TP_REST_MS)
    check("the third-person arms are drawn (trigger and off)", {"trigger", "off"} <= set(tp), ", ".join(sorted(tp)) or "no arms line")
    if {"trigger", "off"} <= set(tp):
        for name in ("trigger", "off"):
            s, e, w = tp[name]
            side = dot(bend(s, e, w), POLE)
            check(f"{name} elbow bends down and back (the same way as the trigger's)", side > 0.0,
                  f"elbow {fmt(e)}, wrist {fmt(w)}, bend side {side:+.2f}")
        off_wrist = tp["off"][2]
        back = FEET[2] + TORSO_HALF_DEPTH
        check("the off hand is not behind the torso's back face", off_wrist[2] <= back,
              f"wrist z {off_wrist[2]:.2f}, torso back z {back:.2f}")
        hip_gap = FEET[2] - off_wrist[2]  # > 0: in front of the hip line (-z is forward at yaw 0)
        check(f"the off hand is in front of the hip line ({HIP_LINE_NEAR} to {HIP_LINE_FAR} in front of the feet's z)",
              HIP_LINE_NEAR <= hip_gap <= HIP_LINE_FAR, f"wrist z {off_wrist[2]:.2f}, {hip_gap:+.2f} in front of the hip line")
        off_shoulder_z = tp["off"][0][2]
        check(f"the off shoulder joint is on the torso's depth (within {SHOULDER_DEPTH_TOL} of the body's centre z, not on its back face)",
              abs(off_shoulder_z - FEET[2]) <= SHOULDER_DEPTH_TOL, f"shoulder z {off_shoulder_z:.2f}, centre z {FEET[2]:.2f}")
        hand_height = off_wrist[1] - FEET[1]
        check("the off hand is not below the hip (about the pelvis height, 4.55 above the feet)", hand_height >= HIP_HEIGHT - 0.25,
              f"wrist {hand_height:.2f} above the feet, pelvis {HIP_HEIGHT:.2f}")
        check("the off hand is at hip height or a little above (its wrist at least 5.3 above the feet)", hand_height >= OFF_WRIST_MIN,
              f"wrist {hand_height:.2f} above the feet")
        off_shoulder, off_elbow, _ = tp["off"]
        elbow_drop = off_shoulder[1] - off_elbow[1]
        check(f"the off elbow is no more than {OFF_ELBOW_MAX_DROP} below its shoulder (the hand hangs beside the hip, elbow bent)",
              elbow_drop <= OFF_ELBOW_MAX_DROP, f"elbow {elbow_drop:.2f} below the shoulder")
        trig_wrist = tp["trigger"][2]
        check("the cup hand keeps its forward reach (over 3 units in front)", FEET[2] - trig_wrist[2] > 3.0,
              f"reach {FEET[2] - trig_wrist[2]:.2f}")
    tp_height = (tp["off"][2][1] - FEET[1]) if "off" in tp else 0.0

    # first person: the rest, the refill (a sip, then R: the pour-up), and the arms after it
    fp_text = run_client("fp", ["0 play", "300 om add 67", pose, f"{FP_REST_MS} arms",
                                "1500 mdown left", "1900 mup left", "2300 down R", "2333 up R",
                                f"{FP_POUR_MS} pour", f"{FP_POUR_MS} arms", f"{FP_END_MS} arms", f"{FP_END_MS + 100} quit"])
    fp = parse_arms(fp_text, FP_REST_MS)
    fp_end = parse_arms(fp_text, FP_END_MS)
    strength = parse_pour(fp_text, FP_POUR_MS)
    check("the first-person arms are drawn at rest and after the refill", "off" in fp and "off" in fp_end,
          f"rest {'off' in fp}, after {'off' in fp_end}")
    check("the first-person refill pours (the bottle over the cup at mid-pour)", strength is not None and strength >= 0.9,
          f"pour strength {strength}")
    if "off" in fp and "off" in fp_end:
        s, e, w = fp["off"]
        side = dot(bend(s, e, w), POLE)
        check("first person: the off elbow bends down and back at rest", side > 0.0,
              f"elbow {fmt(e)}, wrist {fmt(w)}, bend side {side:+.2f}")
        beside_hip("first person at rest", fp["off"], tp_height)
        beside_hip("first person after the refill", fp_end["off"], tp_height)
        spread = max(abs(a - b) for a, b in zip(fp["off"][2], fp_end["off"][2]))
        check(f"the refill ends where it started (the off wrist within {REFILL_TOL} of its rest on every axis)", spread <= REFILL_TOL,
              f"rest {fmt(fp['off'][2])}, after {fmt(fp_end['off'][2])}, largest step {spread:.3f}")

    print(f"{sum(results)}/{len(results)} passed")
    if results and all(results) and not os.environ.get("KN_TEST_KEEP"):
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print(f"logs kept in {WORK}")
    return 0 if results and all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
