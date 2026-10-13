#!/usr/bin/env python3
"""The in-game minimap is round from the first frame it shows (client/src/ui/hud_map.c,
client/src/html/engine/css_cascade.c).

  python3 -I client/tests/minimap_round_e2e.py BUILD_DIR      (run from the repository root)

The bugs: "sometimes the minimap overlay just appears as a square without having rounded corners",
then "it shows as a square for a few seconds then it goes back to being a normal circle shape".
While playing, hud_map.c puts .roundMap (border-radius: 50%) on #miniMapOverlay and the canvas
inside it has `border-radius: inherit` (main.css #miniMapCanvas).
  - The cascade only restyled a child when the parent's inherited properties changed, and
    border-radius is not inherited, so the canvas kept its old 0px radius (3011c33). A style
    query of the overlay between the class change and the next restyle still lost it: the query
    recomputed the overlay outside the restyle walk, which then skipped its children.
  - .roundMap was only toggled inside the minimap's draw, which runs only while it is drawn, but
    the overlay shows whenever the HUD does: after Play from spectating it came up with the
    spectator's square shape until our SPAWN (online: a round trip plus the server's join
    checks). The shape is now set from the state the minimap is drawn in, before it shows.

Offline headless client_bin runs (input script) capture every frame (world + HTML) across each
transition and find the first frame the minimap is visible (its fill #3b3a39 covers part of its
box); on that frame and on every later captured frame the corner regions of the minimap box
must show the game behind it, not the minimap's fill, and the centre must show the map:
  1. joining a game from the menu; 2. a map change (Sandstorm); 3. dying and respawning;
  4. playing again after spectating (while spectating the minimap is square on purpose, v7
     type 0: checked too); 5. at another window size and minimapScale.
Needs a headless GL device (KRUNKNATIVE_HEADLESS=1: EGL; KRUNKNATIVE_EGL_DEVICE picks it), PIL.
Prints PASS/FAIL lines; exit code 0 only when everything passed. KN_TEST_KEEP=1 keeps the frames.
"""
import glob
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-minimap-round-", dir=CACHE if os.path.isdir(CACHE) else None)
KEEP = bool(os.environ.get("KN_TEST_KEEP"))
FILL = (59, 58, 57)  # mm_update's cc_fill_all: the minimap background
results = []


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def run(name, script, size="1280x720", settings=None, fixed=True, timeout=300):
    """Runs one client; returns ({label: [png, ...] in frame order}, minimap rect x,y,w,h in window px)."""
    out = os.path.join(WORK, name)
    cfg = os.path.join(out, "cfg")
    os.makedirs(os.path.join(cfg, "krunknative"), exist_ok=True)
    if settings:
        with open(os.path.join(cfg, "krunknative", "knSettings.json"), "w") as f:
            json.dump(settings, f)
    path = os.path.join(out, "script")
    with open(path, "w") as f:
        f.write(script.replace("@OUT", out))
    log = os.path.join(out, "client.log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": cfg,
                "XDG_STATE_HOME": os.path.join(out, "state"), "KRUNKNATIVE_NO_BROWSER": "1",
                "KRUNKNATIVE_INPUT_SCRIPT": path, "KRUNKNATIVE_WINDOW_SIZE": size})
    if fixed:
        env["KRUNKNATIVE_FIXED_DELTA"] = "16.667"
    with open(log, "w") as f:
        p = subprocess.Popen(["stdbuf", "-oL", f"{BUILD}/client_bin", "--offline", "assets/maps/kanji.json"],
                             stdout=f, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env,
                             start_new_session=True)
        try:
            p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            check(f"{name}: client_bin finished its script", False, f"timeout, log {log}")
        finally:
            if p.poll() is None:
                os.killpg(p.pid, signal.SIGKILL)
                p.wait()
    rect = None
    with open(log, errors="replace") as f:
        for line in f:
            # "[input_script] rect t=.. @final miniMapCanvas=x,y,w,h,ow,oh" (CSS px = window px here):
            # asked once at the end, so no style query runs during the transitions
            if line.startswith("[input_script] rect") and "miniMapCanvas=" in line:
                v = line.split("miniMapCanvas=", 1)[1].split()[0]
                if v != "-":
                    rect = tuple(float(x) for x in v.split(",")[:4])
    frames = {}
    for png in glob.glob(os.path.join(out, "*.png")):
        m = re.match(r"(.+)_(\d+)\.png$", os.path.basename(png))  # `frames` numbers on across calls
        if m:
            frames.setdefault(m.group(1), []).append((int(m.group(2)), png))
    if rect is None:
        check(f"{name}: the minimap's box logged", False, f"log {log}")
    return {k: [p for _, p in sorted(v)] for k, v in frames.items()}, rect


def near(a, b, tol=3):
    return all(abs(int(x) - int(y)) <= tol for x, y in zip(a, b))


def corner_points(rect):
    # 2..8% in from each corner: outside a circle of diameter w (the corner gap is 29% of the
    # radius) and inside the square, on the canvas pixels a square minimap would paint
    x, y, w, h = rect
    pts = []
    for fx in (0.02, 0.05, 0.08):
        for fy in (0.02, 0.05, 0.08):
            for cx, sx in ((x, 1), (x + w, -1)):
                for cy, sy in ((y, 1), (y + h, -1)):
                    pts.append((cx + sx * max(2.0, fx * w), cy + sy * max(2.0, fy * h)))
    return pts


def analyse(png, rect):
    """(visible, filled corner points, centre pixel) of one frame."""
    from PIL import Image
    img = Image.open(png).convert("RGB")
    x, y, w, h = rect
    n = total = 0
    for yy in range(int(y), int(y + h), 3):
        for xx in range(int(x), int(x + w), 3):
            total += 1
            n += near(img.getpixel((xx, yy)), FILL, 2)
    filled = sum(near(img.getpixel((int(px), int(py))), FILL) for px, py in corner_points(rect))
    return n >= 0.03 * total, filled, img.getpixel((int(x + w / 2), int(y + h / 2)))


def check_first_round(name, pngs, rect, expect_round=True):
    """The first captured frame showing the minimap, and every later frame showing it, have the
    expected shape."""
    if not pngs or rect is None:
        check(f"{name}: frames captured", False, f"logs in {WORK}")
        return
    first = None
    bad = []
    for i, png in enumerate(pngs):
        visible, filled, centre = analyse(png, rect)
        if not visible:  # the menu, a loading map: no minimap
            continue
        if first is None:
            first = i
            check(f"{name}: the minimap shows (centre is not the page behind it)", not near(centre, (0, 0, 0), 2),
                  f"centre {centre}, frame {png}")
        if bool(filled) == expect_round:
            bad.append(f"frame {i} ({filled} of 36 corner points show the minimap fill {FILL}): {png}")
    if first is None:
        check(f"{name}: the minimap shows in {len(pngs)} frames", False, f"frames {pngs[0]} ...")
        return
    shape = "round (corners clipped)" if expect_round else "square (the spectator's map)"
    check(f"{name}: {shape} from its first visible frame ({first} of {len(pngs)}) on", not bad,
          "; ".join(bad[:3]))


def done_with(name):
    if not KEEP and all(results):
        for png in glob.glob(os.path.join(WORK, name, "*.png")):
            os.remove(png)


try:
    # Wall-clock steps: the respawn's death delay does not pass on the fixed step (offline sandbox).
    # `0 menu` starts the clock on the menu, so the frames before Play show no minimap.
    frames, rect = run("transitions", """
0 menu
250 frames @OUT/join 30
300 play
2000 frames @OUT/mapchange 30
2000 con map sandstorm
5000 con kill
9000 frames @OUT/respawn 30
9020 respawn
12500 rect @final miniMapCanvas
12600 quit
""", fixed=False)
    check_first_round("joining a game", frames.get("join"), rect)
    check_first_round("after a map change", frames.get("mapchange"), rect)
    check_first_round("after dying and respawning", frames.get("respawn"), rect)
    done_with("transitions")

    frames, rect = run("spectate", """
0 menu
1100 clickid specBtn
1500 play
2900 frames @OUT/spect 10
3500 down ESC
3550 up ESC
4500 clickid specBtn
4950 frames @OUT/unspect 30
5000 play
6500 rect @final miniMapCanvas
6600 quit
""")
    check_first_round("spectating", frames.get("spect"), rect, expect_round=False)
    check_first_round("playing again after spectating", frames.get("unspect"), rect)
    done_with("spectate")

    frames, rect = run("scaled", """
0 menu
250 frames @OUT/scaled 30
300 play
1500 rect @final miniMapCanvas
1600 quit
""", size="1600x900", settings={"minimapScale": 1.4})
    check_first_round("1600x900, minimapScale 1.4", frames.get("scaled"), rect)
    done_with("scaled")
finally:
    if all(results) and not KEEP:
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print(f"logs kept in {WORK}")

print("minimap_round_e2e:", "PASS" if all(results) else "FAIL")
sys.exit(0 if all(results) else 1)
