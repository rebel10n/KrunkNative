#!/usr/bin/env python3
"""KrunkNative phone controls, touch only (client/src/ui/ui_mobile.c, client/src/input_script.c `touch`).

  python3 -I client/tests/mobile_touch_e2e.py BUILD_DIR          (run from the repository root)

A phone (KRUNKNATIVE_MOBILE=1: v10's window._isMobile), offline, headless, fixed 16.667 ms frames. The
input script has no keyboard and no mouse: the menu's TAP TO PLAY is a finger tap (`touch tap`), and the
game is played with fingers only, several at once, at the #mobileUI positions v10 lays out:
  1. TAP TO PLAY enters the game; #mobileUI is shown (the touch controls are up)
  2. the joystick (finger 1, held up): the player walks; released, the walk stops
  3. look (finger 2, dragged right on the open screen): the yaw turns, the pitch does not
  4. fire (finger 3, mobileShoot held): the ammo drops; released, it stops
  5. jump (finger 4, mobileJump): the player leaves the ground
  6. reload (finger 5, mobileReload, after the shots): the reload timer runs
Prints PASS/FAIL lines; exit code 0 only when everything passed.
"""
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-mobiletouch-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
results = []

# the #mobileUI centres at 1280x720 (`rect` probes of the native phone layout, uiBase 1279x719)
TAP_PLAY = (640, 300)          # #instructions "TAP TO PLAY" (523..756 x 283..321)
JOY_CENTRE = (256, 497)        # #mobileJoystick: the zone origin, the stick's rest point
JOY_UP = (256, 420)            # 77 px above it: full deflection
LOOK_FROM = (880, 300)         # open screen (no control under it)
LOOK_TO = (1000, 300)
SHOOT = (70, 234)              # #mobileShoot 38..102 x 202..266
JUMP = (1229, 385)             # #mobileJump 1204..1254 x 360..410
RELOAD = (912, 547)            # #mobileReload 890..934 x 525..569


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def touch(verb, *args):
    return f"touch {verb} " + " ".join(str(a) for a in args)


def run_client(tag, args, script, timeout=240):
    events = sorted((l for l in script if l.strip()), key=lambda l: int(l.split()[0]))
    script_path = os.path.join(WORK, tag + ".script")
    with open(script_path, "w") as f:
        f.write("\n".join(events) + "\n")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": os.path.join(WORK, tag + "-cfg"),
                "XDG_STATE_HOME": os.path.join(WORK, tag + "-state"), "KRUNKNATIVE_NO_BROWSER": "1",
                "KRUNKNATIVE_AUDIO_BACKEND": "null", "KRUNKNATIVE_INPUT_SCRIPT": script_path,
                "KRUNKNATIVE_FIXED_DELTA": "16.667", "KRUNKNATIVE_WINDOW_SIZE": "1280x720",
                "KRUNKNATIVE_MOBILE": "1"})
    binary = os.path.join(BUILD, "client_bin")
    proc = subprocess.Popen([binary] + args, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, errors="replace", start_new_session=True)
    try:
        out, _ = proc.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        os.killpg(proc.pid, signal.SIGKILL)
        out, _ = proc.communicate()
        out += "\n[mobile_touch_e2e] TIMEOUT\n"
    return out.splitlines()


STATE = re.compile(r"\[input_script\] state t=(\d+) .*? menu=(\d) .*?pos=([-\d.]+),([-\d.]+),([-\d.]+) ammo=(-?\d+) .*?reload=([-\d.]+) .*?yaw=([-\d.]+) pitch=([-\d.]+)")


def states(lines):
    out = {}
    for l in lines:
        m = STATE.search(l)
        if m:
            t = int(m.group(1))
            out[t] = {"menu": int(m.group(2)), "pos": tuple(float(m.group(i)) for i in (3, 4, 5)),
                      "ammo": int(m.group(6)), "reload": float(m.group(7)), "yaw": float(m.group(8)), "pitch": float(m.group(9))}
    return out


def vis(lines, label):
    for l in lines:
        if l.startswith("[input_script] vis ") and f" {label}=" in l:
            return l.split(f" {label}=", 1)[1].split()[0]
    return None


def at(st, t):
    """the state at the latest sample at or before t"""
    keys = [k for k in st if k <= t]
    return st[max(keys)] if keys else None


def dist(a, b):
    return sum((x - y) ** 2 for x, y in zip(a, b)) ** 0.5


def main():
    script = [
        "0 menu",
        touch("tap", *TAP_PLAY) + "" if False else f"600 touch tap {TAP_PLAY[0]} {TAP_PLAY[1]}",
        "1400 state", "1400 vis @v mobileUI",
        # 2. joystick: finger 1 held up for 600 ms, then lifted
        f"1500 touch start 1 {JOY_CENTRE[0]} {JOY_CENTRE[1]}",
        f"1550 touch move 1 {JOY_UP[0]} {JOY_UP[1]}",
        "1650 state", "2100 state",
        "2150 touch end 1",
        "2400 state", "2700 state",
        # 3. look: finger 2 dragged 120 px right on the open screen
        f"2800 touch start 2 {LOOK_FROM[0]} {LOOK_FROM[1]}",
        f"2850 touch move 2 {LOOK_TO[0]} {LOOK_FROM[1]}",
        "2900 state",
        "3000 touch end 2",
        "3050 state",
        # 4. fire: finger 3 on the shoot button for 500 ms
        f"3200 touch start 3 {SHOOT[0]} {SHOOT[1]}",
        "3200 state",
        "3700 state",
        "3750 touch end 3",
        "3800 state", "4100 state",
        # 5. jump: finger 4
        f"4200 touch start 4 {JUMP[0]} {JUMP[1]}",
        "4230 state",
        "4260 touch end 4",
        "4500 state",
        # 6. reload: finger 5 on the reload button (the magazine is down from the burst)
        f"4600 touch start 5 {RELOAD[0]} {RELOAD[1]}",
        "4620 state",
        "4650 touch end 5",
        "4700 state",
        "4800 quit",
    ]
    lines = run_client("touch", ["--offline", "assets/maps/official/10_kanji.json"], script)
    st = states(lines)
    check("tap: TAP TO PLAY enters the game with a finger tap", at(st, 1400) and at(st, 1400)["menu"] == 0,
          f"menu at 1400 ms = {at(st, 1400) and at(st, 1400)['menu']}")
    check("tap: #mobileUI is up in game", vis(lines, "mobileUI") == "1", f"mobileUI={vis(lines, 'mobileUI')}")

    a, b, c = at(st, 1500), at(st, 2100), at(st, 2700)
    check("joystick: the finger walks the player", a and b and dist(a["pos"], b["pos"]) > 2.0,
          f"moved {dist(a['pos'], b['pos']) if a and b else 'n/a'} units in 600 ms")
    check("joystick: lifted, the walk stops", b and c and dist(at(st, 2400)["pos"], c["pos"]) < 0.5 * dist(a["pos"], b["pos"]) + 0.5,
          f"after release {dist(at(st, 2400)['pos'], c['pos']) if c else 'n/a'} units in 300 ms")

    before, after = at(st, 2800), at(st, 2900)
    # the yaw is not wrapped in the probe: the drag's turn is the difference modulo a full turn. v10's
    # mblInput look: 120 px x 0.0024 (mouse constant) x 4 x sensitivityXMbl 1.4 = 1.613 rad
    turn = ((after["yaw"] - before["yaw"]) + 3.141592653589793) % (2 * 3.141592653589793) - 3.141592653589793 if before and after else 0.0
    check("look: a finger dragged on the screen turns the yaw by the mobile sensitivity (1.61 rad for 120 px)",
          before and after and 1.45 < abs(turn) < 1.8 and abs(after["pitch"] - before["pitch"]) < 0.01,
          f"yaw turn {turn:.3f} rad, pitch {before and before['pitch']} -> {after and after['pitch']}")

    s0, s1, s2 = at(st, 3200), at(st, 3700), at(st, 3800)
    check("fire: the shoot button fires (ammo drops while held)", s0 and s1 and s1["ammo"] < s0["ammo"],
          f"ammo {s0 and s0['ammo']} -> {s1 and s1['ammo']}")
    check("fire: released, it stops", s1 and s2 and at(st, 4100) and at(st, 4100)["ammo"] == s2["ammo"],
          f"ammo at release {s2 and s2['ammo']}, at 4100 {at(st, 4100) and at(st, 4100)['ammo']}")

    j0, j1 = at(st, 4200), at(st, 4230)
    peak = max((v["pos"][1] for k, v in st.items() if 4200 <= k <= 4260), default=0.0)
    check("jump: the jump button leaves the ground", j0 and peak > j0["pos"][1] + 0.3,
          f"y {j0 and j0['pos'][1]} -> peak {peak}")

    r0, r1 = at(st, 4620), at(st, 4700)
    check("reload: the reload button starts a reload", r1 and r1["reload"] > 0.0 or (r0 and r0["reload"] > 0.0),
          f"reload timer {r0 and r0['reload']} / {r1 and r1['reload']}")

    # the script itself: fingers and the menu tap only (a keyboard or mouse event would be a shortcut)
    forbidden = [l for l in script if re.search(r"\s(down|up|mdown|mup|click|clickid|clicksel|cursor|look|pose|tp|play)\b", l)]
    check("touch only: the script has no keyboard, mouse or look events", not forbidden, f"{forbidden[:3]}")
    errors = [l for l in lines if "Traceback" in l or "ASSERT" in l or "[mobile_touch_e2e] TIMEOUT" in l]
    check("the client ran to quit without errors", not errors and any("[input_script] state" in l for l in lines),
          f"{errors[:2]}")

    shutil.rmtree(WORK, ignore_errors=True)
    failed = results.count(False)
    print(f"{len(results) - failed} passed, {failed} failed", flush=True)
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
