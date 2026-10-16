#!/usr/bin/env python3
"""The mouse wheel's console binds and the weapon swap it keeps (client/src/ui/ui_console.c
ui_console_wheel, client/src/ui/ui_controls.c ui_controls_scroll_event, client/src/main.c scroll_callback).

  python3 -I client/tests/wheel_bind_e2e.py BUILD_DIR      (run from the repository root)

The bug: `bind mwheelup +jump` was stored and never fired: the console had no wheel names, `+jump` was not a
command (an unknown command is silent), and the wheel always swaps weapons with a live player, so a SCROLL
bind never reached a control. The fix: mwheelup / mwheeldown are bind names (tab completion offers them); a
bound direction runs its command and does not swap, the other direction swaps as before; '+name' presses the
control (jump, shoot, ...) and '-name' lets go, a wheel tap lets go at the end of its frame and a key's hold
ends with its release; and a control bound to the wheel key (Settings > Controls, v10's SCROLL) fires with a
live player and stops the swap, as v10's checkScroll does.

Two offline Kanji runs (headless client_bin, input script, fixed 60 fps step, KRUNKNATIVE_INPUT_LOG per frame,
`state` lines for the held slot):
  A (console binds):
    1. `bind mwheelup +jump`: a wheel-up jumps (airborne frames after it), and the held weapon does not change;
    2. mwheeldown is unbound: a wheel-down still swaps weapons (the per-direction rule);
    3. `unbind mwheelup`: the wheel-up swaps again and does not jump;
    4. `bind u +jump`: holding U jumps, and releasing it lets go: the player lands and does not jump again;
    5. tab completion: `bind mw` + Tab completes the wheel name (the console's input line, conin).
  B (Settings wheel bind, the control SCROLL pre-set in the config: cont_jumpKey = 20000):
    6. a wheel-up fires the control (a jump) and the weapon does not swap; a wheel-down does not swap either
       (v10: the wheel key bound means no swap, both directions).
Needs a headless GL device (KRUNKNATIVE_HEADLESS=1: EGL; KRUNKNATIVE_EGL_DEVICE picks it).
Prints PASS/FAIL lines; exit code 0 only when everything passed.
"""
import csv
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-wheel-bind-", dir=CACHE if os.path.isdir(CACHE) else None)
FRAME_MS = 16.667
results = []
STATE_RE = re.compile(r"\[input_script\] state t=(\d+) slot=(\d+)/")
CONIN_RE = re.compile(r"\[input_script\] conin t=\d+ @tab value=\[(.*?)\]")


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def run(name, script, seed_settings=None, timeout=240):
    """One headless offline game. Returns (input rows, {t: held slot}, conin value or None, log path)."""
    base = os.path.join(WORK, name)
    cfg = os.path.join(base, "cfg")
    store = os.path.join(cfg, "krunknative")
    os.makedirs(store, exist_ok=True)
    for key, value in (seed_settings or {}).items():
        with open(os.path.join(store, key + ".json"), "w") as f:
            f.write(value)
    script_path = os.path.join(base, "script.txt")
    with open(script_path, "w") as f:
        f.write(script)
    log = os.path.join(base, "client.log")
    csv_path = os.path.join(base, "input.csv")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": cfg, "XDG_STATE_HOME": os.path.join(base, "state"),
                "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null",
                "KRUNKNATIVE_INPUT_SCRIPT": script_path, "KRUNKNATIVE_FIXED_DELTA": str(FRAME_MS),
                "KRUNKNATIVE_INPUT_LOG": csv_path, "KRUNKNATIVE_WINDOW_SIZE": "640x360"})
    with open(log, "w") as out:
        p = subprocess.Popen(["stdbuf", "-oL", f"{BUILD}/client_bin", "--offline", "assets/maps/kanji.json"], stdout=out,
                             stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, start_new_session=True)
        try:
            p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
            check(f"{name}: client_bin finished its script", False, f"timeout, log {log}")
            return [], {}, None, log
    with open(log, errors="replace") as f:
        text = f.read()
    slots = {int(m.group(1)): int(m.group(2)) for m in STATE_RE.finditer(text)}
    conin = CONIN_RE.search(text)
    rows = []
    if os.path.exists(csv_path):
        with open(csv_path, newline="") as f:
            rows = [{k: float(v) for k, v in row.items()} for row in csv.DictReader(f)]
    return rows, slots, (conin.group(1) if conin else None), log


def airborne(rows, t0, t1):
    return sum(1 for r in rows if t0 <= r["t"] < t1 and r["on_ground"] == 0.0)


def grounded(rows, t0, t1):
    return sum(1 for r in rows if t0 <= r["t"] < t1 and r["on_ground"] == 1.0)


def slot_at(slots, t):
    """The held slot of the last `state` probe at or before t (the probes are at the script's state times)."""
    before = [k for k in slots if k <= t]
    return slots[max(before)] if before else None


SCRIPT_A = "\n".join([
    "0 play",
    "300 con bind mwheelup +jump",       # the console's bind (the rest of the line is the command)
    "400 state",                          # grounded, slot 0 (Assault Rifle)
    "600 wheel 1",                        # mwheelup: the bound jump, no swap
    "800 state",
    "1000 wheel -1",                      # mwheeldown: unbound, swaps as always
    "1200 state",
    "1400 con unbind mwheelup",
    "1500 wheel 1",                       # unbound again: swaps, no jump
    "1700 state",
    "1800 con bind u +jump",
    "1900 down U",                        # '+jump' holds the jump while U is down
    "1950 up U",                          # '-jump' lets go
    "3000 down GRAVE", "3040 up GRAVE",  # open the developer console
    "3100 type bind mw",
    "3140 down TAB", "3180 up TAB",       # the wheel name completes
    "3260 conin @tab",
    "3600 quit",
]) + "\n"

SCRIPT_B = "\n".join([
    "0 play",
    "400 state",
    "600 wheel 1",                        # the Settings bind on the wheel key: jump, no swap
    "800 state",
    "1000 wheel -1",                      # v10: the wheel key bound, no swap either way
    "1200 state",
    "1400 quit",
]) + "\n"

try:
    # ---- A: console binds
    rows, slots, conin, log = run("console", SCRIPT_A)
    check("A: the input log has frames", rows, log)
    if rows:
        check("A: grounded before the bound wheel-up", grounded(rows, 450, 590) > 0)
        jumped = airborne(rows, 600, 800)
        check("A: bind mwheelup +jump: a wheel-up jumps", jumped > 0, f"{jumped} airborne frames after the wheel-up")
        s0, s1 = slot_at(slots, 400), slot_at(slots, 800)
        check("A: the bound wheel-up does not swap weapons", s0 is not None and s0 == s1, f"slot {s0} -> {s1}")
        s2 = slot_at(slots, 1200)
        check("A: an unbound wheel-down still swaps weapons", s1 is not None and s2 is not None and s1 != s2, f"slot {s1} -> {s2}")
        check("A: unbind mwheelup: the wheel-up swaps again", slot_at(slots, 1700) != s2, f"slot {s2} -> {slot_at(slots, 1700)}")
        check("A: unbound, the wheel-up does not jump", airborne(rows, 1500, 1700) == 0)
        landing = [r["t"] for r in rows if r["t"] > 1960 and r["on_ground"] == 1.0]
        check("A: bind u +jump: holding U jumps", airborne(rows, 1900, 2300) > 0, f"{airborne(rows, 1900, 2300)} airborne frames")
        if landing:
            after = landing[0] + 100
            check("A: releasing U lets go: no second jump after the landing", airborne(rows, after, after + 600) == 0,
                  f"landed at t={landing[0]:.0f}")
        else:
            check("A: releasing U lets go: the player lands", False, "no grounded frame after the jump")
    check("A: tab completes `bind mw` to the wheel name", conin is not None and "mwheelup" in conin, f"input line [{conin}]")

    # ---- B: a control bound to the wheel key in the Settings (v10's SCROLL)
    rows, slots, _, log = run("settings", SCRIPT_B, seed_settings={"cont_jumpKey": "20000"})
    check("B: the input log has frames", rows, log)
    if rows:
        check("B: grounded before the wheel-up", grounded(rows, 450, 590) > 0)
        jumped = airborne(rows, 600, 800)
        check("B: the wheel key bound to jump: a wheel-up jumps", jumped > 0, f"{jumped} airborne frames")
        s0, s1, s2 = slot_at(slots, 400), slot_at(slots, 800), slot_at(slots, 1200)
        check("B: the wheel key bound: no weapon swap in either direction", s0 is not None and s0 == s1 == s2,
              f"slot {s0} -> {s1} -> {s2}")
finally:
    if all(results) and not os.environ.get("KN_TEST_KEEP"):
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print(f"logs kept in {WORK}")

print("wheel_bind_e2e:", "PASS" if all(results) else "FAIL")
sys.exit(0 if all(results) else 1)
