#!/usr/bin/env python3
"""The in-game minimap is round (client/src/ui/hud_map.c, client/src/html/engine/css_cascade.c).

  python3 -I client/tests/minimap_round_e2e.py BUILD_DIR      (run from the repository root)

The bug: "sometimes the minimap overlay just appears as a square without having rounded corners".
While playing, hud_map.c puts .roundMap (border-radius: 50%) on #miniMapOverlay and the canvas
inside it has `border-radius: inherit` (main.css #miniMapCanvas). The cascade only restyled a
child when the parent's inherited properties changed, and border-radius is not inherited, so the
canvas kept its old 0px radius: the minimap was square whenever the class arrived while the
overlay's ancestors were already displayed (joining with the HUD up, playing after spectating).
It was round only when the whole HUD subtree happened to be restyled later (shown from
display:none), hence "sometimes".

Offline headless client_bin runs (input script; fixed 60 fps step except for the respawn run) capture page frames (world +
HTML) and check that the four corner pixels of the minimap box show the game behind it, not the
minimap's fill (#3b3a39), while the centre shows the map:
  1. joining a game; 2. after dying and respawning; 3. after a map change (Sandstorm);
  4. playing again after spectating (the spectator minimap is square on purpose, v7 type 0);
  5. at another window size and minimap scale.
Needs a headless GL device (KRUNKNATIVE_HEADLESS=1: EGL; KRUNKNATIVE_EGL_DEVICE picks it), PIL.
Prints PASS/FAIL lines; exit code 0 only when everything passed.
"""
import glob
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-minimap-round-", dir=CACHE if os.path.isdir(CACHE) else None)
FILL = (59, 58, 57)  # mm_update's cc_fill_all: the minimap background
results = []


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def run(name, script, size="1280x720", settings=None, map_name="kanji", fixed=True, timeout=240):
    """Runs one client; returns {label: (png path, minimap rect x,y,w,h in window px)}."""
    cfg = os.path.join(WORK, name, "cfg")
    os.makedirs(os.path.join(cfg, "krunknative"), exist_ok=True)
    if settings:
        with open(os.path.join(cfg, "krunknative", "knSettings.json"), "w") as f:
            json.dump(settings, f)
    path = os.path.join(WORK, name, "script")
    with open(path, "w") as f:
        f.write(script.replace("@OUT", os.path.join(WORK, name)))
    log = os.path.join(WORK, name, "client.log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": cfg,
                "XDG_STATE_HOME": os.path.join(WORK, name, "state"), "KRUNKNATIVE_NO_BROWSER": "1",
                "KRUNKNATIVE_INPUT_SCRIPT": path, "KRUNKNATIVE_WINDOW_SIZE": size})
    if fixed:
        env["KRUNKNATIVE_FIXED_DELTA"] = "16.667"
    with open(log, "w") as out:
        p = subprocess.Popen(["stdbuf", "-oL", f"{BUILD}/client_bin", "--offline", f"assets/maps/{map_name}.json"],
                             stdout=out, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env,
                             start_new_session=True)
        try:
            p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
            check(f"{name}: client_bin finished its script", False, f"timeout, log {log}")
    rects = {}
    with open(log, errors="replace") as f:
        for line in f:
            # "[input_script] rect t=.. @label miniMapCanvas=x,y,w,h,ow,oh" (CSS px = window px here)
            if line.startswith("[input_script] rect") and "miniMapCanvas=" in line:
                label = line.split("@", 1)[1].split()[0]
                v = line.split("miniMapCanvas=", 1)[1].split()[0]
                if v != "-":
                    rects[label] = tuple(float(x) for x in v.split(",")[:4])
    shots = {}
    for label, rect in rects.items():
        pngs = glob.glob(os.path.join(WORK, name, f"{label}_*.png"))  # `frames` numbers on across calls
        if pngs:
            shots[label] = (pngs[0], rect)
    return shots


def near(a, b, tol=3):
    return all(abs(int(x) - int(y)) <= tol for x, y in zip(a, b))


def check_round(name, shot):
    if shot is None:
        check(f"{name}: minimap captured", False, f"no frame / rect, logs in {WORK}")
        return
    from PIL import Image
    png, (x, y, w, h) = shot
    img = Image.open(png).convert("RGB")
    # 4% in from each corner: outside a circle of diameter w (the corner gap is 29% of the radius)
    # and inside the square, on the canvas pixels a square minimap would paint
    inset = max(2.0, 0.04 * w)
    corners = [(x + inset, y + inset), (x + w - inset, y + inset), (x + inset, y + h - inset),
               (x + w - inset, y + h - inset)]
    filled = [c for c in corners if near(img.getpixel((int(c[0]), int(c[1]))), FILL)]
    centre = img.getpixel((int(x + w / 2), int(y + h / 2)))
    check(f"{name}: the minimap shows (centre is not the page behind it)", not near(centre, (0, 0, 0), 2),
          f"centre {centre}")
    check(f"{name}: the minimap's corners are clipped (round)", not filled,
          f"{len(filled)} of 4 corners show the minimap fill {FILL}, frame {png}")


def rect(label):
    return f"rect @{label} miniMapCanvas"


try:
    shots = run("transitions", f"""
0 play
1500 frames @OUT/join 1
1500 {rect('join')}
1600 con kill
9000 respawn
12000 frames @OUT/respawn 1
12000 {rect('respawn')}
12100 con map sandstorm
16000 frames @OUT/mapchange 1
16000 {rect('mapchange')}
16500 quit
""", fixed=False)  # the respawn's death delay does not pass on the fixed step (offline sandbox)
    check_round("joining a game", shots.get("join"))
    check_round("after dying and respawning", shots.get("respawn"))
    check_round("after a map change", shots.get("mapchange"))

    shots = run("spectate", f"""
0 menu
1100 clickid specBtn
1500 play
3000 frames @OUT/spect 1
3000 {rect('spect')}
3500 down ESC
3550 up ESC
4500 clickid specBtn
5000 play
7000 frames @OUT/unspect 1
7000 {rect('unspect')}
7500 quit
""")
    spect = shots.get("spect")
    check("spectating: the minimap captured", spect is not None, WORK)
    check_round("playing again after spectating", shots.get("unspect"))

    shots = run("scaled", f"""
0 play
1500 frames @OUT/scaled 1
1500 {rect('scaled')}
2000 quit
""", size="1600x900", settings={"minimapScale": 1.4})
    check_round("1600x900, minimapScale 1.4", shots.get("scaled"))
finally:
    if all(results) and not os.environ.get("KN_TEST_KEEP"):
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print(f"logs kept in {WORK}")

print("minimap_round_e2e:", "PASS" if all(results) else "FAIL")
sys.exit(0 if all(results) else 1)
