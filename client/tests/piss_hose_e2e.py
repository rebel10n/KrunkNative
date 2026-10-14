#!/usr/bin/env python3
"""The Piss's first-person hose is held in the trigger hand and its spray leaves from the hose's tip.

  python3 -I client/tests/piss_hose_e2e.py BUILD_DIR      (run from the repository root)

The bug: "the cylinder object thing [...] rn its above the hand and it should be inside the hand".
The Piss (WEAPON_ID_PISS) has no model: its weapon mesh is the procedural hose (player_hose_mesh),
which sat at a fixed spot in the weapon anchor ahead of and above the hand, and the spray's source
was worked out separately from the same numbers. Now the hose is placed from the trigger hand's
centre (player_mesh.c player_trigger_hand + WeaponStream.hose_view / hose_view_rot) and the spray
leaves from the drawn hose's own tip (player_hose_points, the branch's hoseTip.getWorldPosition()).

An offline Kanji game (headless client_bin, input script, fixed 60 fps step): the Object Manager
hands the Piss out, the player stands still and sprays; the input script's `hose` command logs the hose as drawn (tip, back end, the hand's
centre, their window positions) and KRUNKNATIVE_VFX_LOG the spray's source per shot. Checks:
  1. the hose is held: the hand's centre lies on the hose's axis (within the fist) and the back end
     sits inside the fist, the tip ahead of it;
  2. the nozzle shows: the tip is in the window, above the hand's centre;
  3. every local spray starts at the drawn hose's tip (the frame's probe), and there were sprays.
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

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-piss-hose-", dir=CACHE if os.path.isdir(CACHE) else None)
FRAME_MS = 16.667
FIST = 0.41          # half the first-person hand's thickness (arm_scale x 0.68 / 2)
HAND_HALF = 0.27     # half the hand's length along the arm (lower_arm_length 2.7 x the skin segment 0.2 / 2)
# world units between the spray's source and the probe's tip: the probe runs before the frame's arm
# pose update (idle sway / recoil move the tip ~0.01); the old separate source was ~1 unit off
SOURCE_TOLERANCE = 0.05
results = []

VEC = r"(-?[\d.]+),(-?[\d.]+),(-?[\d.]+)"
HOSE_RE = re.compile(rf"\[input_script\] hose t=(\d+) tip={VEC} back={VEC} hand={VEC} cam={VEC} "
                     r"screen_tip=(-?[\d.]+),(-?[\d.]+) screen_hand=(-?[\d.]+),(-?[\d.]+)")
SOURCE_RE = re.compile(r"\[vfx\] stream source (-?[\d.]+) (-?[\d.]+) (-?[\d.]+)")


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def script_text():
    # the Piss from the Object Manager's Weapons tab, stand facing down Kanji's open street, then
    # spray for ~0.5 s with a hose probe every frame (the probe runs before the frame's shot)
    lines = ["0 play", "400 om add 65", "1000 pose -343 -143 -28 0 0", "1490 hose", "1600 mdown left"]
    t = 1600.0
    while t < 2100.0:
        lines.append(f"{t:.3f} hose")
        t += FRAME_MS
    lines += ["2100 mup left", "2300 quit"]
    return "\n".join(sorted(lines, key=lambda line: float(line.split()[0]))) + "\n"


def run(tag, timeout=180):
    work = os.path.join(WORK, tag)
    cfg = os.path.join(work, "cfg")
    os.makedirs(cfg)
    script = os.path.join(work, "hose.script")
    with open(script, "w") as f:
        f.write(script_text())
    log = os.path.join(work, "client.log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": cfg, "XDG_STATE_HOME": os.path.join(work, "state"),
                "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_INPUT_SCRIPT": script, "KRUNKNATIVE_FIXED_DELTA": str(FRAME_MS),
                "KRUNKNATIVE_VFX_LOG": "1", "KRUNKNATIVE_WINDOW_SIZE": "640x360"})
    with open(log, "w") as out:
        p = subprocess.Popen(["stdbuf", "-oL", f"{BUILD}/client_bin", "--offline", "assets/maps/kanji.json"], stdout=out,
                             stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, start_new_session=True)
        try:
            p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
            check(f"{tag}: client_bin finished its script", False, f"timeout, log {log}")
            return []
    events = []  # ("hose", dict) / ("source", vec) in log order
    with open(log, errors="replace") as f:
        for line in f:
            m = HOSE_RE.search(line)
            if m:
                g = [float(x) for x in m.groups()]
                events.append(("hose", {"t": g[0], "tip": g[1:4], "back": g[4:7], "hand": g[7:10], "cam": g[10:13],
                                        "screen_tip": g[13:15], "screen_hand": g[15:17]}))
                continue
            m = SOURCE_RE.search(line)
            if m:
                events.append(("source", [float(x) for x in m.groups()]))
    return events


def sub(a, b):
    return [a[i] - b[i] for i in range(3)]


def dot(a, b):
    return sum(a[i] * b[i] for i in range(3))


def length(a):
    return math.sqrt(dot(a, a))


def held(probe):
    """(distance of the hand's centre from the hose's axis, its place along the axis from the back end)"""
    axis = sub(probe["tip"], probe["back"])
    rel = sub(probe["hand"], probe["back"])
    along = dot(rel, axis) / length(axis)
    side = math.sqrt(max(0.0, dot(rel, rel) - along * along))
    return side, along, length(axis)


def verify(tag, events):
    probes = [e for kind, e in events if kind == "hose"]
    check(f"{tag}: the hose probes ran", probes, WORK)
    if not probes:
        return None
    still = probes[0]
    side, along, hose_len = held(still)
    check(f"{tag}: the hand's centre is on the hose's axis (in the fist)", side < FIST * 0.5,
          f"{side:.3f} off the axis (fist half-thickness {FIST})")
    check(f"{tag}: the back end sits in the fist, the tip ahead of it", abs(along) <= HAND_HALF and along < hose_len,
          f"hand {along:.3f} along the {hose_len:.3f} hose from the back end (hand half-length {HAND_HALF})")
    cam, tip, hand = still["cam"], still["tip"], still["hand"]
    check(f"{tag}: the tip is ahead of the hand (from the camera)", length(sub(tip, cam)) > length(sub(hand, cam)),
          f"tip {length(sub(tip, cam)):.3f}, hand {length(sub(hand, cam)):.3f} from the eye")
    u, v = still["screen_tip"]
    check(f"{tag}: the nozzle shows (the tip in the window, above the hand)", 0.0 < u < 1.0 and 0.0 < v < 1.0 and v < still["screen_hand"][1],
          f"tip at {u:.3f},{v:.3f}, hand at {still['screen_hand'][0]:.3f},{still['screen_hand'][1]:.3f}")

    # each spray against the probe of its frame (the last probe logged before it)
    last, worst, sprays, unmatched = None, 0.0, 0, 0
    for kind, e in events:
        if kind == "hose":
            last = e
        elif kind == "source":
            sprays += 1
            if last is None:
                unmatched += 1
                continue
            worst = max(worst, length(sub(e, last["tip"])))
    check(f"{tag}: the local player sprayed", sprays >= 5, f"{sprays} spray sources logged")
    check(f"{tag}: every spray starts at the drawn hose's tip", sprays and not unmatched and worst <= SOURCE_TOLERANCE,
          f"worst {worst:.4f} units from the tip, {unmatched} without a probe")
    return still


try:
    verify("first person", run("fp"))
finally:
    if all(results) and not os.environ.get("KN_TEST_KEEP"):
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print(f"logs kept in {WORK}")

print("piss_hose_e2e:", "PASS" if all(results) else "FAIL")
sys.exit(0 if all(results) else 1)
