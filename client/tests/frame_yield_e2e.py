#!/usr/bin/env python3
"""Mid-frame asset loads never show a frame without the world (client/src/frame_target.c).

  python3 -I client/tests/frame_yield_e2e.py BUILD_DIR      (run from the repository root)

On the web every blocking asset fetch (a spray texture, a joining player's models and
textures) yields to the browser, which then composites the canvas as far as the frame had got:
the sky-colour clear and no world, the "white / fog-only flash". KRUNKNATIVE_YIELD_CHECK=1
emulates that natively: at each ensure_asset_file() of a running game it inspects framebuffer 0
(what the browser would show) and logs "[yield-check] ok|NO WORLD <path>".

An offline Kanji game (headless client_bin, input script, fixed 60 fps step) where three bots
join (`bot_add 3`: their bodies, hats, weapons and skins load) and the player sprays twice
through the spray wheel (the chosen spray's texture loads):
  1. KRUNKNATIVE_FRAME_TARGET=0 (drawing straight into the window, the old web behaviour):
     the check must see frames without the world, so it can catch the bug at all;
  2. KRUNKNATIVE_FRAME_TARGET=1 (the web default): every yield, spray and player assets
     included, still finds the last complete frame with the world.
Needs a headless GL device (KRUNKNATIVE_HEADLESS=1: EGL; KRUNKNATIVE_EGL_DEVICE picks it).
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
WORK = tempfile.mkdtemp(prefix="kn-frame-yield-", dir=CACHE if os.path.isdir(CACHE) else None)
results = []

# times from `play`: bots join, then the player looks down and sprays twice (holding F opens
# the spray wheel, releasing it sprays the chosen spray: its decal texture loads)
SCRIPT = """0 play
1000 con bot_add 3
1500 look 0 -0.6
1600 down F
1900 up F
2600 down F
2900 up F
4000 quit
"""


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def run(name, frame_target, timeout=120):
    script = os.path.join(WORK, f"{name}.script")
    with open(script, "w") as f:
        f.write(SCRIPT)
    log = os.path.join(WORK, f"{name}.log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": os.path.join(WORK, f"{name}-cfg"),
                "XDG_STATE_HOME": os.path.join(WORK, f"{name}-state"), "KRUNKNATIVE_NO_BROWSER": "1",
                "KRUNKNATIVE_INPUT_SCRIPT": script, "KRUNKNATIVE_FIXED_DELTA": "16.667",
                "KRUNKNATIVE_WINDOW_SIZE": "640x360", "KRUNKNATIVE_YIELD_CHECK": "1",
                "KRUNKNATIVE_FRAME_TARGET": "1" if frame_target else "0"})
    with open(log, "w") as out:
        p = subprocess.Popen(["stdbuf", "-oL", f"{BUILD}/client_bin", "--offline", "assets/maps/kanji.json"], stdout=out,
                             stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, start_new_session=True)
        try:
            p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
            check(f"{name}: client_bin finished its script", False, f"timeout, log {log}")
    with open(log, errors="replace") as f:
        lines = f.read().splitlines()
    yields = [m.groups() for m in (re.match(r"\[yield-check\] (ok|NO WORLD) (\S+)", line) for line in lines) if m]
    summary = next((line for line in lines if re.match(r"\[yield-check\] \d+ asset yields", line)), None)
    return yields, summary, log


try:
    old, old_summary, old_log = run("direct", frame_target=False)
    check("direct drawing: the yield check ran to the end", old_summary, old_log)
    old_bad = [path for state, path in old if state == "NO WORLD"]
    check("direct drawing: a mid-frame load shows a frame without the world (the check sees the bug)", old_bad,
          f"{len(old_bad)} of {len(old)} yields")

    new, new_summary, new_log = run("frame-target", frame_target=True)
    check("frame target: the yield check ran to the end", new_summary, new_log)
    sprays = [path for _, path in new if "/textures/sprays/" in path]
    models = [path for _, path in new if "/models/" in path]
    check("frame target: a spray texture loaded mid-game", sprays, ", ".join(sprays[:3]) or new_log)
    check("frame target: joining players' models loaded mid-game", models, f"{len(models)} models" if models else new_log)
    new_bad = [path for state, path in new if state == "NO WORLD"]
    check("frame target: every mid-frame load still shows the world", new and not new_bad,
          f"{len(new)} yields" + (", without the world at " + ", ".join(new_bad[:5]) if new_bad else ""))
finally:
    if all(results) and not os.environ.get("KN_TEST_KEEP"):
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print(f"logs kept in {WORK}")

print("frame_yield_e2e:", "PASS" if all(results) else "FAIL")
sys.exit(0 if all(results) else 1)
