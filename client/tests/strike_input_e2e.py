#!/usr/bin/env python3
"""Krunker Strike's inputs, natively (client/src/main.c routing, client/src/ui/strike_touch.c, client/src/gamepad.c).

  python3 -I client/tests/strike_input_e2e.py BUILD_DIR      (run from the repository root)

Three headless offline Strike clients (--flavor strike, kanji), each driven by an input script that
enters the game the way a player does (0 menu, the MAIN_MENU's PLAY, spawn):
  desktop  (no phone): the mouse and keyboard play. Pointer lock on play; mouse motion turns the
           camera; LMB fires, RMB aims (held), the wheel never swaps; W walks; ESC opens the menu
           with the pointer free, and the page's PLAY closes it again.
  pad      (no phone): a scripted standard pad (gpad): the right stick turns the camera, the left
           stick walks, the right trigger (shoot) fires, the left shoulder (aim) aims, button_1
           jumps (the event reaches the machine), START opens and closes the menu.
  phone    (KRUNKNATIVE_MOBILE=1): the touch layer is the input: a finger on the shoot button fires,
           a finger dragged on the move zone walks, one on the aim zone turns the camera; the
           mouse is not the game's (the phone's #mobile detection line is logged).
Each check reads the `[input_script] state` lines (cursor, menu, pos, ammo, aim, yaw) at the times of
its script. Prints PASS/FAIL lines; exit 0 only when everything passed.
Needs a headless GL device (KRUNKNATIVE_HEADLESS=1: EGL). KRUNKNATIVE_ASSETS_DIR is passed through (a
worktree without the untracked asset dirs). KRUNKNATIVE_E2E_KEEP=1 keeps the work directory.
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
WORK = tempfile.mkdtemp(prefix="kn-strike-input-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
results = []

STATE = re.compile(r"\[input_script\] state t=(\d+) slot=(\d+)/\d+ .*?menu=(\d) locked=(\d) pos=([-\d.]+),([-\d.]+),([-\d.]+) "
                   r"ammo=(-?\d+) aim=([-\d.]+) .*?cursor=(\w+) yaw=([-\d.]+) pitch=([-\d.]+)")

DESKTOP = """500 state
600 cam @play
700 cursor 640 360
720 cursor 700 360
740 cursor 760 360
800 state
1000 mdown left
1150 mup left
1300 state
1350 mdown right
1450 state
1500 mup right
1550 state
1600 wheel 1
1650 state
1700 down W
2000 up W
2050 state
2100 down ESC
2120 up ESC
2300 state
2400 clickid mobMenuPlay
2600 state
2700 quit
"""

PAD = """300 state
400 gpad stick right 1 0
700 gpad stick right 0 0
750 state
800 gpad stick left 0 -1
1100 gpad stick left 0 0
1150 state
1200 gpad shoulder_bottom_right down
1400 gpad shoulder_bottom_right up
1450 state
1480 vis @hip knStrikeAimDot knStrikeCross
1500 gpad shoulder_bottom_left down
1650 vis @aimed knStrikeAimDot knStrikeCross
1700 state
1750 gpad shoulder_bottom_left up
1800 gpad button_1 down
1850 gpad button_1 up
1900 gpad start
2050 state
2060 nodes @ring0 [style*=outline]
2100 gpad d_pad_down down
2120 gpad d_pad_down up
2200 nodes @ring1 [style*=outline]
2250 gpad button_2 down
2270 gpad button_2 up
2350 state
2400 gpad start
2500 state
2550 gpad start
2650 state
2700 quit
"""

# window px (1280x720): the shoot button's icon (810..898 x 282..370 with auto-fire off: the centre 854, 326;
# the touch layer drops data-kn-control once built, so the probe finds the icon by its image), the move zone
# (the left half), the aim zone (the right half)
PHONE = """300 call setSetting "autoFireMbl" "false"
350 state
360 rects @shoot img[src$="shoot.svg"]
400 touch start 1 854 326
600 touch end 1
650 state
700 touch start 2 300 500
760 touch move 2 300 420
1200 touch end 2
1250 state
1300 touch start 3 900 300
1340 touch move 3 1000 300
1400 touch end 3
1450 state
1500 quit
"""

# The script clock is absolute: `spawn` holds it at the Play click's time (3000 ms of the menu clock),
# and every later event fires at its own time, so the bodies' times are offsets from that spawn.
HEAD = "0 menu\n3000 clickid mobMenuPlay\n3000 spawn\n"
SPAWN = 3000


def scripted(body):
    # the game's loading screen (strike_ui.c: at least 1500 ms of wall clock) holds the page's pointer, and the
    # script clock runs per frame: the bodies start once it is gone (strike_ftue_e2e.py waits the same way)
    out = [HEAD, f"{SPAWN} waitfor !.frvr-loading-holder .loading-screen\n"]
    for line in body.splitlines():
        t, rest = line.split(" ", 1)
        out.append(f"{int(t) + SPAWN} {rest}\n")
    return "".join(out)


def check(name, ok, detail=""):
    results.append(bool(ok))
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail and not ok else ''}", flush=True)


def run_client(name, script, mobile=False, first_run=False):
    path = os.path.join(WORK, f"{name}.script")
    with open(path, "w") as f:
        f.write(script)
    cfg = os.path.join(WORK, f"{name}-cfg")
    os.makedirs(cfg, exist_ok=True)
    # a returning player (the FTUE gate, client/src/ui/strike_ftue.c) unless first_run: a fresh config runs the
    # controls tutorial in the first run's sandbox game
    if not first_run:
        os.makedirs(os.path.join(cfg, "krunknative-strike"), exist_ok=True)
        with open(os.path.join(cfg, "krunknative-strike", "knFtue.json"), "w") as f:
            f.write('{"guest":{"played":true}}')
    log = os.path.join(WORK, f"{name}.log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL", "KRUNKNATIVE_MOBILE")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": cfg,
                "XDG_STATE_HOME": os.path.join(WORK, f"{name}-state"), "KRUNKNATIVE_NO_BROWSER": "1",
                "KRUNKNATIVE_INPUT_SCRIPT": path, "KRUNKNATIVE_FIXED_DELTA": "16.667",
                "KRUNKNATIVE_WINDOW_SIZE": "1280x720", "KRUNKNATIVE_GAMEPAD_LOG": "1"})
    if mobile:
        env["KRUNKNATIVE_MOBILE"] = "1"
    with open(log, "w") as out:
        p = subprocess.Popen([f"{BUILD}/client_bin", "--flavor", "strike", "--offline", "--map", "kanji"], stdout=out,
                             stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, start_new_session=True)
        try:
            p.wait(timeout=300)
        except subprocess.TimeoutExpired:
            check(f"{name}: client_bin finished its script", False, f"timeout, log {log}")
        finally:
            if p.poll() is None:
                os.killpg(p.pid, signal.SIGKILL)
                p.wait()
    with open(log, errors="replace") as f:
        return f.read()


def states(text):
    out = {}
    for m in STATE.finditer(text):
        out[int(m.group(1))] = {"slot": int(m.group(2)), "menu": int(m.group(3)), "locked": int(m.group(4)),
                                "pos": tuple(float(m.group(i)) for i in (5, 6, 7)), "ammo": int(m.group(8)),
                                "aim": float(m.group(9)), "cursor": m.group(10), "yaw": float(m.group(11)),
                                "pitch": float(m.group(12))}
    return out


NODES = re.compile(r"\[input_script\] nodes t=(\d+) (\S+) elements=\d+ match=(\d+)")
RECTS = re.compile(r"\[input_script\] rects t=(\d+) (\S+) n=(\d+)")


def probe(text, pattern, label):
    """the match count of the nodes / rects probe named `label` (its line's time is the frame it ran in, a few
    ms after the script's, so the probes are looked up by name)"""
    for m in pattern.finditer(text):
        if m.group(2).lstrip("@") == label:
            return int(m.group(3))
    return None


def vis(text, label, ids):
    """the shown flags ("1" / "0", "-" for no such id) of the page elements `ids` in the vis probe named `label`
    (client/src/input_script.c: "[input_script] vis t=100 @label id=1 ...")"""
    for line in text.splitlines():
        if line.startswith("[input_script] vis ") and f" @{label} " in line:
            flags = {}
            for i in ids:
                m = re.search(rf" {re.escape(i)}=(\S+)", line)
                flags[i] = m.group(1) if m else None
            return flags
    return None


def offsets(text):
    """the states after the spawn, keyed by their time from it (the bodies' times)"""
    return {t - SPAWN: v for t, v in states(text).items() if t >= SPAWN}


def desktop(text):
    s = offsets(text)
    if not all(t in s for t in (500, 800, 1300, 1450, 1550, 1650, 2050, 2300, 2600)):
        check("desktop: the probe lines are all there", False, "missing state lines")
        return
    check("desktop: play takes the pointer (cursor disabled, locked)", s[500]["cursor"] == "disabled" and s[500]["locked"] == 1,
          f"cursor={s[500]['cursor']}")
    check("desktop: mouse motion turns the camera (yaw or pitch)", s[800]["yaw"] != s[500]["yaw"] or s[800]["pitch"] != s[500]["pitch"],
          f"yaw {s[500]['yaw']} -> {s[800]['yaw']}")
    check("desktop: LMB (shoot) fires: the magazine drops", s[1300]["ammo"] < s[800]["ammo"], f"ammo {s[800]['ammo']} -> {s[1300]['ammo']}")
    check("desktop: RMB held aims", s[1450]["aim"] > 0.5, f"aim {s[1450]['aim']}")
    check("desktop: RMB released stops aiming", s[1650]["aim"] < 0.1, f"aim {s[1650]['aim']}")
    check("desktop: the wheel does not swap weapons", s[1650]["slot"] == s[800]["slot"], f"slot {s[800]['slot']} -> {s[1650]['slot']}")
    check("desktop: W walks", s[2050]["pos"] != s[1650]["pos"], f"pos {s[1650]['pos']} -> {s[2050]['pos']}")
    check("desktop: ESC opens the menu with the pointer free", s[2300]["menu"] == 1 and s[2300]["cursor"] == "normal",
          f"menu={s[2300]['menu']} cursor={s[2300]['cursor']}")
    check("desktop: the page's PLAY closes the menu again", s[2600]["menu"] == 0, f"menu={s[2600]['menu']}")


def pad(text):
    s = offsets(text)
    if not all(t in s for t in (300, 750, 1150, 1450, 1700, 2050, 2350, 2500, 2650)):
        check("pad: the probe lines are all there", False, "missing state lines")
        return
    check("pad: a scripted pad connects", "[gamepad] connected: script pad" in text)
    check("pad: the right stick turns the camera", s[750]["yaw"] != s[300]["yaw"] or s[750]["pitch"] != s[300]["pitch"],
          f"yaw {s[300]['yaw']} -> {s[750]['yaw']}")
    check("pad: the left stick walks", s[1150]["pos"] != s[300]["pos"], f"pos {s[300]['pos']} -> {s[1150]['pos']}")
    check("pad: the right trigger (shoot) fires", s[1450]["ammo"] < s[300]["ammo"], f"ammo {s[300]['ammo']} -> {s[1450]['ammo']}")
    check("pad: the left shoulder (aim) aims while held", s[1700]["aim"] > 0.5, f"aim {s[1700]['aim']}")
    check("pad: a face button reaches the machine", "[gamepad] press button_1" in text)
    # the sight's reticle (FRVR's aimDot) shows once fully aimed with the Reflex sight, and the Dynamic crosshair
    # hides then (FRVR's updateCrosshair opacity follows aimVal); at the hip it is the other way round
    ids = ("knStrikeAimDot", "knStrikeCross")
    hip, aimed = vis(text, "hip", ids), vis(text, "aimed", ids)
    check("pad: at the hip the crosshair shows and no reticle", hip == {"knStrikeAimDot": "0", "knStrikeCross": "1"}, f"{hip}")
    check("pad: aimed, the sight's reticle shows and the crosshair hides", aimed == {"knStrikeAimDot": "1", "knStrikeCross": "0"},
          f"{aimed}")
    check("pad: START opens the menu", s[2050]["menu"] == 1, f"menu={s[2050]['menu']}")
    # the page marks one element with an outline of its own: the ring adds one more
    before, after = probe(text, NODES, "ring0"), probe(text, NODES, "ring1")
    check("pad: the d-pad puts the focus ring on a button", before is not None and after == before + 1, f"{before} -> {after}")
    check("pad: B (Escape) closes the menu", s[2350]["menu"] == 0, f"menu={s[2350]['menu']}")
    check("pad: START opens the menu again", s[2500]["menu"] == 1, f"menu={s[2500]['menu']}")
    check("pad: START closes it again", s[2650]["menu"] == 0, f"menu={s[2650]['menu']}")


def phone(text):
    s = offsets(text)
    if not all(t in s for t in (350, 650, 1250, 1450)):
        check("phone: the probe lines are all there", False, "missing state lines")
        return
    check("phone: the phone is detected (touch layer on)", "[mobile] mobile device detected" in text)
    check("phone: the shoot button shows with auto-fire off", (probe(text, RECTS, "shoot") or 0) >= 1)
    check("phone: a finger on the shoot button fires", s[650]["ammo"] < s[350]["ammo"], f"ammo {s[350]['ammo']} -> {s[650]['ammo']}")
    check("phone: the move zone walks", s[1250]["pos"] != s[650]["pos"], f"pos {s[650]['pos']} -> {s[1250]['pos']}")
    check("phone: the aim zone turns the camera", s[1450]["yaw"] != s[1250]["yaw"], f"yaw {s[1250]['yaw']} -> {s[1450]['yaw']}")


# A desktop's first run, a fresh config: the boot gate (strike_ui.c) sends it to quick connect, into the controls
# tutorial's sandbox game (menu opened at boot, as the FTUE e2e does). The tutorial's steps are waited for by their
# DOM (waitfor), the clock runs on from each. Taps: a mouse press passes
# a tap step (welcome, basics, hold, move, camera, complete); WASD advance move_controls and move_interaction; the
# mouse look advances camera_controls and camera_interaction; jump (space) and reload (R) their steps.
FIRST_RUN = """0 menu
0 waitfor !.frvr-loading-holder .loading-screen
0 waitfor .ftue-controls__tap-text
200 state
250 mdown left
400 mup left
450 state
700 mdown left
750 mup left
900 mdown left
950 mup left
1100 mdown left
1150 mup left
1200 waitfor .ftue-controls__touch-hand-vertical
1300 down W
1700 up W
1800 waitfor .ftue-controls__tap-text
1900 mdown left
1950 mup left
2000 waitfor .ftue-controls__arrows-horizontal
2100 cursor 640 360
2120 cursor 700 360
2140 cursor 760 360
2400 waitfor .ripples
2500 down SPACE
2520 up SPACE
2600 waitfor .ftue-controls__highlight-reload-animation
2700 down R
2720 up R
2800 waitfor .ftue-controls__tap-text
2900 mdown left
2950 mup left
3000 waitfor .ftue-controls__button
3100 state
3200 quit
"""

TUTORIAL_STEPS = ["welcome", "basics", "hold", "move", "move_controls", "move_interaction", "move_end", "camera",
                  "camera_controls", "camera_interaction", "camera_end", "jump", "jump_end", "reload", "complete",
                  "complete_go"]


def first_run(text):
    s = states(text)
    steps = re.findall(r"\[strike\] ftue: controls step (\w+)", text)
    check("desktop first run: the controls tutorial runs (welcome)", steps[:1] == ["welcome"], f"steps {steps[:3]}")
    check("desktop first run: a mouse press reaches the game during the tutorial (the magazine drops)",
          200 in s and 450 in s and s[450]["ammo"] < s[200]["ammo"],
          f"ammo {s.get(200, {}).get('ammo')} -> {s.get(450, {}).get('ammo')}")
    check("desktop first run: the tutorial runs through to Let's Go by mouse, WASD, look, space and R",
          steps == TUTORIAL_STEPS, f"steps {steps}")


def main():
    if not os.path.exists(f"{BUILD}/client_bin"):
        print(f"no {BUILD}/client_bin")
        return 1
    desktop(run_client("desktop", scripted(DESKTOP)))
    pad(run_client("pad", scripted(PAD)))
    phone(run_client("phone", scripted(PHONE), mobile=True))
    first_run(run_client("firstrun", FIRST_RUN, first_run=True))
    if not os.environ.get("KRUNKNATIVE_E2E_KEEP"):
        shutil.rmtree(WORK, ignore_errors=True)
    failed = results.count(False)
    print(f"{len(results) - failed}/{len(results)} passed")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
