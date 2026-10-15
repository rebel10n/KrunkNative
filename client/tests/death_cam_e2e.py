#!/usr/bin/env python3
"""KrunkNative death cam end-to-end (client/src/menu_view.c; v10 CONTROLS.camLookAt / update and
CONTROLS.updateMouseInp, game.deob.js 10186446 / 10603817 / 10587124).

  python3 -I client/tests/death_cam_e2e.py BUILD_DIR          (run from the repository root)

Offline Kanji, a bot frozen with `con bot_stop on` (the killer holds still), then at 2 s a KILL by
that bot (the input script's `killed`). The per-frame KRUNKNATIVE_INPUT_LOG rows of the death:
  1. the camera's yaw turns toward the killer's bearing, monotonically, never past it, and is
     within 1 degree of it before the mouse moves (v10: exp(-0.0022 t), ~2.1 s for a 120 degree turn)
  2. from 4.5 s a real mouse (`cursor` moves, 12 px a frame for 0.5 s, the look leaves the killer)
     turns the rig: the yaw follows the model below every frame, mouse included
  3. released, the chase brings the camera back within 1 degree of the killer
The model replays CONTROLS.updateMouseInp (yaw -= dx * 0.0024: mouseSens at sensitivity 1) and
CONTROLS.update (yaw += angleDistance * ms * 0.0022 per frame) from the logged frame deltas, the
cursor positions of the script and the killer's position at the kill (`players`).
Needs a headless GL device (KRUNKNATIVE_HEADLESS=1: EGL; KRUNKNATIVE_EGL_DEVICE picks it).
Prints PASS/FAIL lines; exit code 0 only when everything passed.
"""
import csv
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
WORK = tempfile.mkdtemp(prefix="kn-dc-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
results = []
procs = []

CHASE_TRN = 0.0022   # camChaseTrn per ms
KILL_MS = 2000
MOUSE_MS = 4500      # the look starts once the chase has turned (1 degree at ~2.14 s after the kill)
MOUSE_STEP = 0.0288  # rad of look per event: 12 px at mouseSens 0.0024 (CONTROLS.updateMouseInp: game_constants.mouse_sensitivity)
MOUSE_EVENTS = 24    # one event every 20 ms: 0.48 s of look
RELEASE_MS = MOUSE_MS + 20 * MOUSE_EVENTS
END_MS = 8000


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


def angle_between(frm, to):
    """the signed shortest turn from frm to to (UTILS.angleDistance)"""
    return math.atan2(math.sin(to - frm), math.cos(frm - to))


def script_lines():
    # the look is the input script's `look` (the look channel the mouse feeds): a real cursor needs
    # window focus, which a headless run does not have, so the pointer path is dropped before the look
    lines = ["0 play", "1000 con bot_add 1", "1500 con bot_stop on"]
    lines.append(f"{KILL_MS} players")
    lines.append(f"{KILL_MS} killed")
    for k in range(MOUSE_EVENTS):
        lines.append(f"{MOUSE_MS + 20 * k} look {-MOUSE_STEP:.4f} 0")
    lines.append(f"{END_MS} quit")
    return lines


def run_client(args, script, timeout=240):
    """client_bin headless with the input script, its log lines and the per-frame CSV rows"""
    events = [l for l in script if l.strip()]
    events.sort(key=lambda l: int(l.split()[0]))  # stable: the script's order at equal times
    script_path = os.path.join(WORK, "death.script")
    with open(script_path, "w") as f:
        f.write("\n".join(events) + "\n")
    log = os.path.join(WORK, "death.log")
    csv_path = os.path.join(WORK, "death.csv")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": os.path.join(WORK, "cfg"),
                "XDG_STATE_HOME": os.path.join(WORK, "state"), "KRUNKNATIVE_NO_BROWSER": "1",
                "KRUNKNATIVE_AUDIO_BACKEND": "null", "KRUNKNATIVE_INPUT_SCRIPT": script_path,
                "KRUNKNATIVE_INPUT_LOG": csv_path, "KRUNKNATIVE_FIXED_DELTA": "16.667",
                "KRUNKNATIVE_WINDOW_SIZE": "1280x720"})
    # a worktree lacks the untracked font / asset dirs: the main checkout's assets (RELAUNCH notes)
    env.setdefault("KRUNKNATIVE_ASSETS_DIR", "/home/user/src/KrunkNative/assets")
    with open(log, "w") as out:
        p = subprocess.Popen(["stdbuf", "-oL", f"{BUILD}/client_bin"] + args, stdout=out, stderr=subprocess.STDOUT,
                             stdin=subprocess.DEVNULL, env=env, start_new_session=True)
        procs.append(p)
        try:
            p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
            check("client_bin finished its script", False, f"timeout, log {log}")
    rows = []
    if os.path.exists(csv_path):
        with open(csv_path, newline="") as f:
            for r in csv.DictReader(f):
                rows.append({k: float(v) for k, v in r.items()})
    return read(log).splitlines(), rows


def main():
    if not os.path.exists(os.path.join(BUILD, "client_bin")):
        print(f"no client_bin in {BUILD}")
        return 2
    lines, rows = run_client(["--offline", "assets/maps/kanji.json"], script_lines())

    kill = [l for l in lines if "[input_script] killed" in l]
    killer = None
    if kill:
        m = re.search(r"by=(-?\d+)", kill[0])
        killer = int(m.group(1)) if m else -1
    check("the KILL by a bot happened", killer is not None and killer >= 0, kill[0] if kill else "no killed line")
    if killer is None or killer < 0:
        return finish()

    # the killer's feet at the kill ('players' is logged in the same frame, before the KILL)
    players = [l for l in lines if "[input_script] players t=2000" in l]
    kpos = None
    for m in re.finditer(r"(\d+):(.*?)@(-?\d+\.\d+),(-?\d+\.\d+),(-?\d+\.\d+)", players[0] if players else ""):
        if int(m.group(1)) == killer:
            kpos = (float(m.group(3)), float(m.group(4)), float(m.group(5)))
    check("the killer's position at the kill is logged", kpos is not None, players[0] if players else "no players line")
    if kpos is None:
        return finish()

    before = [r for r in rows if r["t"] < KILL_MS]
    dead = [r for r in rows if r["t"] >= KILL_MS]
    check("the death frames are logged", len(dead) > 200 and len(before) > 10, f"{len(dead)} dead rows")
    if len(dead) < 200 or not before:
        return finish()

    # the death eye (the first-person camera of the frame before the kill) and the chase's yaw goal:
    # camLookAt(killer): direction2D(eye -> killer) = atan2(eye.x - kx, eye.z - kz)
    eye = before[-1]
    target = math.atan2(eye["cam_x"] - kpos[0], eye["cam_z"] - kpos[2])

    # the kill frame's camera is already the chase's first step; the model starts from the first
    # dead row's observed yaw (the death yaw itself is the player's look, not logged)
    start = dead[1]

    # the chase-only turn: monotonic, no overshoot, within 1 degree before the look starts
    pre = [r for r in dead if KILL_MS <= r["t"] < MOUSE_MS]
    errs = [angle_between(r["cam_yaw"], target) for r in pre]
    mono = all(abs(errs[i + 1]) <= abs(errs[i]) + 2e-3 for i in range(len(errs) - 1))
    overshoot = any(errs[i] * errs[i + 1] < 0.0 for i in range(len(errs) - 1))
    first_err = abs(errs[0]) if errs else -1.0
    deg1 = next((r["t"] - KILL_MS for r in pre if abs(angle_between(r["cam_yaw"], target)) <= math.radians(1.0)), None)
    check("the yaw turns toward the killer's bearing every frame (no pass of it)", mono and not overshoot and first_err > 0.3,
          f"first dead frame {math.degrees(first_err):.1f} degrees off the bearing {math.degrees(target):.1f}")
    check("within 1 degree of the killer before the look moves it (v10 ~2.1 s for 120 degrees)", deg1 is not None and deg1 <= 2600,
          f"1 degree after {None if deg1 is None else deg1 / 1000:.2f} s")

    # the model: the look (updateMouseInp: yDr += look, the input script's `look` as the mouse's
    # Input look) then the chase (CONTROLS.update), frame by frame. An event fires in the first frame
    # whose time reaches it (events run at the frame's start), so frame r gets the events with
    # previous_t < event_t <= t_r.
    events = [(MOUSE_MS + 20 * k, -MOUSE_STEP) for k in range(MOUSE_EVENTS)]
    model = start["cam_yaw"]
    model_nomouse = start["cam_yaw"]
    prev_t = start["t"]
    worst = 0.0
    nomouse_at_release = None
    for r in dead[2:]:
        look = sum(v for (et, v) in events if prev_t < et <= r["t"])
        prev_t = r["t"]
        ms = min(r["delta"], 33.0)
        model += look
        model += angle_between(model, target) * ms * CHASE_TRN
        model_nomouse += angle_between(model_nomouse, target) * ms * CHASE_TRN
        worst = max(worst, abs(model - r["cam_yaw"]))
        if int(r["t"]) >= RELEASE_MS and nomouse_at_release is None:
            nomouse_at_release = (model_nomouse, r["cam_yaw"], model)
    check("every frame matches the model: chase + look (updateMouseInp, camChaseTrn)", worst < 0.002,
          f"worst {worst:.5f} rad over {len(dead) - 2} frames")
    # the look is a real effect: without it the camera would have kept turning toward the killer
    mouse_effect = abs(nomouse_at_release[1] - nomouse_at_release[0]) if nomouse_at_release else 0.0
    check("the look moved the dead camera off the chase's path", mouse_effect > 0.2,
          f"{math.degrees(mouse_effect):.1f} degrees off the chase-only camera at the release")
    # released: the chase pulls the camera back
    last = dead[-1]
    back = abs(angle_between(last["cam_yaw"], target))
    check("released: back within 1 degree of the killer by the end", back <= math.radians(1.0),
          f"{math.degrees(back):.2f} degrees at {last['t'] / 1000:.2f} s")
    return finish()


def finish():
    print(f"{sum(results)} of {len(results)} checks passed")
    return 0 if results and all(results) else 1


if __name__ == "__main__":
    try:
        code = main()
    finally:
        for p in procs:
            try:
                os.killpg(p.pid, signal.SIGKILL)
            except OSError:
                pass
        shutil.rmtree(WORK, ignore_errors=True) if os.environ.get("KN_KEEP_E2E") is None and results and all(results) else None
    sys.exit(code)
