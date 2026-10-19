#!/usr/bin/env python3
"""The menu covers the 3D scene: no scene pixels show at the window's right or bottom edge.

  python3 -I client/tests/menu_bleed_e2e.py BUILD_DIR     (native, run from the repository root)

#uiBase is the menu page, scaled to fit the window (menu_scale_base, client/src/ui/menu_ui.c).
The menu's shade is translucent over the world, so a frame with the page drawn is the world-only
frame (KRUNKNATIVE_HIDE_UI=1) times the page's ratio: menu / world is the shade wherever the page
covers the window, and something else wherever the 3D scene shows through. A page whose scaled box
stops short of the window's right or bottom edge (a scale rounded down, or a fractional box edge
that the engine snaps down to whole CSS px) leaves the last columns or rows at the world's own
brightness: that was the menu's bottom-right strip.

Checks, per window size / UI scale, at startup frames, and across live window resizes:
- the last three columns (the right edge) and rows (the bottom edge) carry no more menu / world
  ratio than the line 4 px inside them, on every position along the edge whose 4 px inside are
  flat (page text, buttons, bars and panel edges are skipped), the scene is at most SHARE of them;
- the frame has the requested size (the resize really happened).

Prints PASS/FAIL lines; exit code 0 only when everything passed.
"""
import os
import shutil
import subprocess
import sys
import tempfile
import warnings

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BUILD = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.path.join(ROOT, "cmake-build-debug")
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-menu-bleed-", dir=CACHE if os.path.isdir(CACHE) else None)
INSIDE = 4             # px from the edge of the reference line the edge lines are compared with
EDGE_LINES = 3         # the last columns / rows checked (the edge lines)
FLAT = 4               # px inside the reference line whose ratio must be flat for a position to count
SELECT = 0.06          # a position is flat when its inside ratio varies less than this (relative)
SHARE = 0.2            # a line whose pixels are this much the 3D scene (median share) is a leak
SCENE_FIT = 6          # a pixel is scene when page-to-world mix fits it within this many levels per channel
MIN_POSITIONS = 20     # flat positions an edge needs to be measured
WORLD_MIN = 150        # world pixels this dim (sum of RGB) have no stable ratio: skipped
CAPTURE_FRAMES = 60
STARTUP_FRAMES = [1, 2, 3, 5, 8, 13, 21, 34]
NATIVE_CASES = [("1280x720", 0.7), ("1366x768", 0.7), ("1920x1080", 0.7), ("1277x713", 0.7),
                ("2560x1440", 0.7), ("1501x847", 0.7), ("1920x1009", 0.7), ("1600x900", 0.73),
                ("1280x720", 0.3), ("1280x720", 0.65), ("1920x1080", 1.0), ("1366x768", None)]
# a window resized after the menu is up: from, to (UI scale 0.7)
RESIZES = [("1280x720", "1920x1080"), ("1920x1080", "1366x768"), ("1280x720", "1277x713"),
           ("1366x768", "2560x1440")]
results = []
warnings.filterwarnings("ignore", category=RuntimeWarning)  # NaN ratios (dark or off-screen world) are expected


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def load_rgb(path):
    import numpy as np
    from PIL import Image
    return np.asarray(Image.open(path).convert("RGB"), dtype=float)


def side_check(menu, world):
    """(measured, shares, detail) for the right edge of two RGB frames (rows x columns x 3): the last
    EDGE_LINES columns against the column INSIDE px in, on the rows where the menu is flat over the
    FLAT columns before the reference and the world is flat over the edge (so the two runs' worlds,
    which may load a few textures apart, agree there). Per edge pixel, the scene's share is how far
    its colour lies from the page's own colour toward the world's, when that mix fits the pixel
    within SCENE_FIT levels per channel (a grey page underlay does not fit, and is not scene). The
    share is the median over the row's pixels in the line; a leak is a line whose share is above
    SHARE. A page element darker than the shade is not a leak."""
    import numpy as np
    H, W, _ = menu.shape
    ref_col = W - INSIDE
    msum, wsum = menu.sum(2), world.sum(2)
    with np.errstate(invalid="ignore", divide="ignore"):
        mprof = msum[:, ref_col - FLAT + 1:ref_col + 1]
        wprof = wsum[:, W - INSIDE - FLAT:W]
        mmed = np.nanmedian(mprof, axis=1, keepdims=True)
        wmed = np.nanmedian(wprof, axis=1, keepdims=True)
        flat = (np.abs(mprof / mmed - 1) < SELECT).all(axis=1) & \
               (np.abs(wprof / wmed - 1) < SELECT).all(axis=1) & (wmed[:, 0] > WORLD_MIN)
    n = int(flat.sum())
    if n < MIN_POSITIONS:
        return False, [], f"{n} flat positions (need {MIN_POSITIONS}): not measured"
    ref = menu[flat, ref_col, :]                       # the page's colour in each row (flat rows)
    shares = []
    for k in range(1, EDGE_LINES + 1):
        mv = menu[flat, W - k, :]
        wv = world[flat, W - k, :]
        d = wv - ref                                   # the world's lift over the page, per channel
        dd = (d * d).sum(1)
        usable = np.sqrt(dd) > 0.25 * wv.sum(1)        # the world is clearly brighter than the page
        with np.errstate(invalid="ignore", divide="ignore"):
            f = ((mv - ref) * d).sum(1) / dd           # scene share of the pixel
            resid = np.abs(mv - (ref + f[:, None] * d)).max(1)
        share = np.where(usable & (resid < SCENE_FIT), np.clip(f, 0.0, 1.0), 0.0)
        shares.append(float(np.median(share[usable])) if usable.any() else 0.0)
    worst = max(shares)
    return True, shares, f"shares {' '.join(f'{x:.2f}' for x in shares)} ({n} positions)"


def edge_check(menu_png, world_png, size):
    """(ok, detail): the window size is right, and no measured edge line is mostly the 3D scene."""
    menu, world = load_rgb(menu_png), load_rgb(world_png)
    H, W, _ = menu.shape
    want_w, want_h = map(int, size.split("x"))
    if (W, H) != (want_w, want_h):
        return False, f"frame is {W}x{H}, not {size}"
    measured, leaks, parts = [], [], []
    for name, (m, w) in (("right", (menu, world)), ("bottom", (menu.transpose(1, 0, 2), world.transpose(1, 0, 2)))):
        ok, shares, detail = side_check(m, w)
        parts.append(f"{name} {detail}")
        if ok:
            measured.append(name)
            leaks += [x for x in shares if x > SHARE]
    if not measured:
        return False, "no edge measured; " + "; ".join(parts)
    return not leaks, "; ".join(parts)


def run(tag, size, scale, hide, frames, script=None):
    """client_bin --offline headless: the frame's PNG path, or None when the client wrote none."""
    base = os.path.join(WORK, f"{tag}-h{hide}-f{frames}")
    cfg = os.path.join(base, "cfg", "krunknative")
    os.makedirs(cfg, exist_ok=True)
    with open(os.path.join(cfg, "knSettings.json"), "w") as f:
        f.write('{"uiScaling": false}' if scale is None else '{"scaleUI": %s}' % scale)
    png = os.path.join(base, "frame.png")
    env = dict(os.environ, KRUNKNATIVE_HEADLESS="1", KRUNKNATIVE_AUDIO_BACKEND="null",
               XDG_CONFIG_HOME=os.path.dirname(cfg), KRUNKNATIVE_WINDOW_SIZE=size,
               KRUNKNATIVE_HIDE_UI=str(hide), KRUNKNATIVE_FIXED_DELTA="16",
               KRUNKNATIVE_CAPTURE_FRAMES=str(frames), KRUNKNATIVE_SCREENSHOT=png)
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    if script:
        path = os.path.join(base, "input.script")
        with open(path, "w") as f:
            f.write(script)
        env["KRUNKNATIVE_INPUT_SCRIPT"] = path
    log = os.path.join(base, "client.log")
    try:
        with open(log, "w") as out:
            subprocess.run([os.path.join(BUILD, "client_bin"), "--offline"], env=env, stdout=out,
                           stderr=subprocess.STDOUT, timeout=180)
    except subprocess.TimeoutExpired:
        return None
    return png if os.path.isfile(png) else None


def pair_check(name, size, scale, frames=CAPTURE_FRAMES, script=None, final_size=None):
    """The menu frame and its world-only frame, checked for an uncovered edge."""
    tag = f"{size}-{'off' if scale is None else scale}-{name}"
    menu = run(tag, size, scale, 0, frames, script)
    world = run(tag, size, scale, 1, frames, script)
    if menu is None or world is None:
        check(name, False, "the client wrote no frame (see " + WORK + ")")
        return
    ok, detail = edge_check(menu, world, final_size or size)
    check(name, ok, detail)


# ---------------------------------------------------------------- native

def native_main():
    for size, scale in NATIVE_CASES:
        label = "uiScaling off" if scale is None else f"scaleUI {scale}"
        pair_check(f"menu covers the window at {size}, {label}", size, scale)
    for frames in STARTUP_FRAMES:
        pair_check(f"startup: the menu covers the window at frame {frames}", "1280x720", 0.7, frames=frames)
    for start, end in RESIZES:
        w, h = end.split("x")
        script = f"0 boot\n200 resize {w} {h}\n"
        # captured well after the resize: the page's bottom bar settles over ~1 s (a 60-frame capture
        # still shows it mid-slide)
        pair_check(f"resize {start} -> {end}: the menu covers the window", start, 0.7, frames=200,
                   script=script, final_size=end)
    return finish("menu_bleed_e2e")


def finish(name):
    if all(results) and not os.environ.get("KN_TEST_KEEP"):
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print(f"files kept in {WORK}")
    print(f"{name}:", "PASS" if results and all(results) else "FAIL")
    return 0 if results and all(results) else 1


if __name__ == "__main__":
    os.chdir(ROOT)
    sys.exit(native_main())
