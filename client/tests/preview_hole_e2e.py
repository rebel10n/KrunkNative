#!/usr/bin/env python3
"""The class preview is #classPreviewCanvas's own content, under the page's shades with no seam.

  python3 -I client/tests/preview_hole_e2e.py BUILD_DIR             (native, run from the repository root)
  python3 -I client/tests/preview_hole_e2e.py --web WEB_DIST_DIR    (web build, e.g. dist/web)

Like Krunker's transparent WebGL canvas, the preview is the bitmap of the page's canvas
(client/src/ui/class_preview.c renders it, html_dom_canvas_gl shows it), painted at the canvas's
place in the page: #instructionHolder's 50% shade lies below it and dims the world around and
behind the model the same everywhere, and a popup's 75% backdrop (#popupBack) lies above it and
dims the model too. The page has no holes: an earlier version cut them into the shades with spread
box-shadows and dimmed the box itself under the page, which left a visible ring around the box
("this weird small outline that doesn't have a transparent backdrop") and an undimmed box at
startup ("the preview backdrop loads last").

Checks, per window size / UI scale (native) or size / devicePixelRatio (web):
- drawn: the model is drawn inside the canvas box, as the canvas's bitmap (native: the device-px
  rect class_preview.c logs with KRUNKNATIVE_PREVIEW_LOG=1; web: the canvas's bitmap is the box's
  device-px size and has opaque pixels);
- seamless: frame / world (the same run with KRUNKNATIVE_HIDE_UI=1 natively; on the web a
  screenshot with the page hidden, the game's main loop paused around the pair) per line along
  each edge of the box, at every offset from 10 px outside to 10 px inside, is the shade's ratio
  (0.5, within LEVEL_TOLERANCE) and the same within TOLERANCE at every offset up to RING px from
  the edge (the lines farther out set the level and pick the plain positions): a ring of other
  pixels at any edge fails. Lines are taken only at positions where the whole 20 px profile is
  flat (within SELECT), so page text, buttons and the model are skipped; an edge with too few such
  positions is covered by the page (v10's class box over the bottom edge) and one such edge is
  allowed: a ring disqualifies every position it crosses on every edge, so it still fails;
- popup: with a popup open (native: the Exit button's confirm, web: #popupHolder shown) the same
  at 0.5 x 0.25, and nothing inside the box is brighter than 25% (the model is under the backdrop);
- startup: the first frames after the map is ready (native) / while the page loads (web) are
  seamless as well, whatever the shade is while the menu fades in (the lines must agree with each
  other).
--web also reports the per-frame cost of copying the preview into the page's canvas
(window.__knCanvasGl, web_dom.c). Prints PASS/FAIL lines; exit code 0 only when everything passed.
"""
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import warnings

WEB = len(sys.argv) > 2 and sys.argv[1] == "--web"
BUILD = sys.argv[2 if WEB else 1] if len(sys.argv) > (2 if WEB else 1) else "cmake-build-debug"
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-preview-hole-", dir=CACHE if os.path.isdir(CACHE) else None)
REACH = 10          # px on each side of an edge
RING = 4            # px on each side of an edge where a seam / ring is looked for
TOLERANCE = 0.05    # |line median of frame / world over the shade - 1|
SELECT = 0.06       # a position along an edge counts when its whole profile is this flat (relative)
LEVEL_TOLERANCE = 0.1  # |the shade measured at an edge / the expected shade - 1|
MIN_LINES = 20
CAMERA = "0,250,0,-0.15,0.5"  # high over Sandstorm: a bright, detailed world behind the menu
SHADE = 128 / 255   # rgba(0, 0, 0, 0.5): alpha 128
POPUP_SHADE = SHADE * (1 - 191 / 255)  # and rgba(0, 0, 0, 0.75) above it
results = []
warnings.filterwarnings("ignore", category=RuntimeWarning)  # NaN lines (dark or off-screen pixels) are expected

# window size x scaleUI (settings.scaleUI: #uiBase's scale; None = uiScaling off): odd sizes and
# scales put the canvas box on fractional device px
NATIVE_CASES = [("1280x720", 0.7), ("1366x768", 0.7), ("1920x1080", 0.7), ("2560x1440", 0.7), ("1277x713", 0.7),
                ("1501x847", 0.7), ("1920x1009", 0.7), ("1280x720", 0.3), ("1280x720", 0.65), ("1600x900", 0.73),
                ("1920x1080", 1.0), ("1366x768", None)]
NATIVE_POPUP_CASES = [("1280x720", 0.7), ("1501x847", 0.7), ("1920x1080", 0.65)]
NATIVE_STARTUP_FRAMES = [1, 2, 3, 5, 8, 13, 21, 34]
WEB_CASES = [(1280, 720, 1.0), (1277, 713, 1.0), (1920, 1080, 1.0), (1280, 720, 1.25), (1536, 864, 1.25),
             (1280, 720, 1.5), (1366, 768, 1.1), (1280, 720, 2.0)]


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def load_rgb(path):
    from PIL import Image
    import numpy as np
    return np.asarray(Image.open(path).convert("RGB"), dtype=float)


def edge_on_screen(shape, box, edge):
    """The edge's profile (REACH px on both sides) lies inside the image."""
    x, y, w, h = box
    H, W = shape[:2]
    if edge == "left":
        return x - REACH >= 0 and x + REACH <= W
    if edge == "right":
        return x + w - REACH >= 0 and x + w + REACH <= W
    if edge == "top":
        return y - REACH >= 0 and y + REACH <= H
    return y + h - REACH >= 0 and y + h + REACH <= H


def edge_lines(img, box, edge):
    """The lines along `edge` of box (x, y, w, h): a 2D array [position along the edge, offset],
    offsets -REACH..REACH-1 (negative: outside the box, 0: the box's first line inside). Positions
    keep 8 px off the corners; NaN where a line leaves the image."""
    import numpy as np
    x, y, w, h = box
    H, W = img.shape[:2]
    offsets = range(-REACH, REACH)
    if edge in ("left", "right"):
        ks = np.arange(max(0, y + 8), min(H, y + h - 8))
        cols = [x + d if edge == "left" else x + w - 1 - d for d in offsets]
        out = np.full((len(ks), len(cols)), np.nan)
        for j, c in enumerate(cols):
            if 0 <= c < W:
                out[:, j] = img[ks, c]
    else:
        ks = np.arange(max(0, x + 8), min(W, x + w - 8))
        rows = [y + d if edge == "top" else y + h - 1 - d for d in offsets]
        out = np.full((len(ks), len(rows)), np.nan)
        for j, r in enumerate(rows):
            if 0 <= r < H:
                out[:, j] = img[r, ks]
    return out


def seam_check(ratio, box, shade=None):
    """(ok, detail): per edge inside the image, the median of frame / world over the plain
    positions (each position's profile flat within SELECT of its own median and, when `shade` is
    given, at the shade's ratio within LEVEL_TOLERANCE: separate runs may light the world a few
    percent differently, and a panel over the shade is not the shade) must be the same at every
    offset within TOLERANCE."""
    import numpy as np
    bad, covered, detail, measured = [], [], [], 0
    for edge in ("left", "right", "top", "bottom"):
        if not edge_on_screen(ratio.shape, box, edge):
            continue
        lines = edge_lines(ratio, box, edge)
        measured += 1
        with np.errstate(invalid="ignore"):
            ref = np.nanmedian(lines, axis=1, keepdims=True)
            plain = (np.abs(lines / ref - 1) < SELECT).all(axis=1)
            if shade is not None:  # and only the page's shade there (not a panel over it)
                plain &= np.abs(ref[:, 0] / shade - 1) < LEVEL_TOLERANCE
        clean = lines[plain]
        if len(clean) < MIN_LINES:  # page content over this edge (v10's class box, CLICK TO PLAY)
            covered.append(edge)
            detail.append(f"{edge}: {len(clean)} plain positions of {len(lines)}")
            continue
        med = np.median(clean, axis=0)
        level = float(np.median(med))
        # a seam or ring sits at the edge: the worst line within RING px of it (farther out, the
        # world's own gradients and the two runs' lighting differ by a few percent at startup)
        near = np.arange(REACH - RING, REACH + RING)
        worst = int(near[np.argmax(np.abs(med[near] - level))])
        detail.append(f"{edge}: {level:.3f}, worst {med[worst]:.3f} at {worst - REACH:+d} ({len(clean)} positions)")
        if abs(med[worst] / level - 1) > TOLERANCE:
            bad.append(edge)
    if measured < 2:
        return False, "fewer than two edges on screen"
    # an edge the page covers (v10's class box sits over the canvas's bottom edge, CLICK TO PLAY
    # over its top one at some scales) has too few plain positions to measure; a ring runs round
    # every edge, so it shows on the others: all but one on-screen edge must be measured
    if measured - len(covered) < max(2, measured - 1):
        bad += covered
    return not bad, ("BAD " + ", ".join(bad) + " | ") * bool(bad) + "; ".join(detail)


def ring_check(ratio, box):
    """(ok, detail): no ring at the box edges, whatever the levels on both sides are: per edge, the
    positions whose outside and inside halves are each flat, and the line medians at every offset
    equal to their side's level within TOLERANCE (a hole whose shade differs inside the box passes
    when it is sharp; seam_check is the stricter test of the preview's design)."""
    import numpy as np
    bad, covered, detail, measured = [], [], [], 0
    for edge in ("left", "right", "top", "bottom"):
        if not edge_on_screen(ratio.shape, box, edge):
            continue
        lines = edge_lines(ratio, box, edge)
        measured += 1
        with np.errstate(invalid="ignore"):
            out_ref = np.nanmedian(lines[:, :REACH], axis=1, keepdims=True)
            in_ref = np.nanmedian(lines[:, REACH:], axis=1, keepdims=True)
            flat = (np.abs(lines[:, :REACH] / out_ref - 1) < SELECT).all(axis=1) & \
                   (np.abs(lines[:, REACH:] / in_ref - 1) < SELECT).all(axis=1)
        clean = lines[flat]
        if len(clean) < MIN_LINES:  # page content over this edge
            covered.append(edge)
            detail.append(f"{edge}: {len(clean)} flat positions of {len(lines)}")
            continue
        med = np.median(clean, axis=0)
        level = np.concatenate([np.full(REACH, np.median(med[:REACH])), np.full(REACH, np.median(med[REACH:]))])
        dev = np.abs(med / level - 1)
        worst = int(np.argmax(dev))
        detail.append(f"{edge}: {level[0]:.3f} | {level[-1]:.3f}, worst {med[worst]:.3f} at {worst - REACH:+d}")
        if dev[worst] > TOLERANCE:
            bad.append(edge)
    if measured < 2:
        return False, "fewer than two edges on screen"
    # an edge the page covers (v10's class box sits over the canvas's bottom edge, CLICK TO PLAY
    # over its top one at some scales) has too few plain positions to measure; a ring runs round
    # every edge, so it shows on the others: all but one on-screen edge must be measured
    if measured - len(covered) < max(2, measured - 1):
        bad += covered
    return not bad, ("BAD " + ", ".join(bad) + " | ") * bool(bad) + "; ".join(detail)


def model_check(ratio, box, shade):
    """The preview draws something inside the box: pixels far from the shade's ratio."""
    import numpy as np
    x, y, w, h = box
    inner = ratio[max(0, y):y + h, max(0, x):x + w]
    with np.errstate(invalid="ignore"):
        n = int((np.abs(inner - shade) > 0.15).sum())
    return n > 0.01 * w * h, f"{n} px differ from the shade in the {w}x{h} box"


# ---------------------------------------------------------------- native

def native_run(tag, size, scale_ui, hide=0, frames=120, script=None):
    """client_bin --offline headless: (screenshot as float RGB or None, log text)."""
    cfg = os.path.join(WORK, f"cfg-{tag}-{hide}-{frames}", "krunknative")
    os.makedirs(cfg, exist_ok=True)
    with open(os.path.join(cfg, "knSettings.json"), "w") as f:
        f.write('{"uiScaling": false}' if scale_ui is None else '{"scaleUI": %s}' % scale_ui)
    png = os.path.join(WORK, f"{tag}-h{hide}-f{frames}.png")
    env = dict(os.environ, KRUNKNATIVE_HEADLESS="1", KRUNKNATIVE_AUDIO_BACKEND="null",
               XDG_CONFIG_HOME=os.path.dirname(cfg), KRUNKNATIVE_WINDOW_SIZE=size,
               KRUNKNATIVE_CAMERA=CAMERA, KRUNKNATIVE_FIXED_DELTA="0.016", KRUNKNATIVE_HIDE_UI=str(hide),
               KRUNKNATIVE_CAPTURE_FRAMES=str(frames), KRUNKNATIVE_SCREENSHOT=png, KRUNKNATIVE_PREVIEW_LOG="1")
    if script:
        path = os.path.join(WORK, f"{tag}.script")
        with open(path, "w") as f:
            f.write(script)
        env["KRUNKNATIVE_INPUT_SCRIPT"] = path
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    try:
        p = subprocess.run([os.path.join(BUILD, "client_bin"), "--offline", "assets/maps/sandstorm.json"], env=env,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace", timeout=180)
        log = p.stdout
    except subprocess.TimeoutExpired:
        log = ""
    with open(os.path.join(WORK, f"{tag}-h{hide}-f{frames}.log"), "w") as f:
        f.write(log)
    return (load_rgb(png) if os.path.isfile(png) else None), log


def logged_rect(log):
    """The canvas box (device px) class_preview.c logged last."""
    m = re.findall(r"\[preview\] rect (\d+),(\d+) (\d+)x(\d+)", log)
    return tuple(map(int, m[-1])) if m else None


def shade_ratio(frame, world):
    import numpy as np
    f, w = frame.sum(2), world.sum(2)
    return np.where(w > 120, f / np.maximum(w, 1), np.nan)


def native_case(size, scale_ui):
    tag = f"{size}-{'off' if scale_ui is None else scale_ui}"
    frame, log = native_run(tag, size, scale_ui)
    world, _ = native_run(tag, size, scale_ui, hide=1)
    box = logged_rect(log)
    if frame is None or world is None or not box:
        check(f"native {tag}: screenshots and the canvas box", False, f"logs in {WORK}")
        return
    ratio = shade_ratio(frame, world)
    ok, detail = model_check(ratio, box, SHADE)
    check(f"native {tag}: the preview is drawn inside the canvas box {box}", ok, detail)
    ok, detail = seam_check(ratio, box, SHADE)
    check(f"native {tag}: the shade is uniform up to the canvas edges", ok, detail)


def native_popup_case(size, scale_ui):
    import numpy as np
    tag = f"popup-{size}-{scale_ui}"
    # v7's clientExitPopup(): #popupHolder over the menu (1 ms = ~60 frames of the fixed 0.016 ms step)
    script = "0 menu\n1 clickid clientExit\n"
    frame, log = native_run(tag, size, scale_ui, script=script)
    world, _ = native_run(tag, size, scale_ui, hide=1, script=script)
    box = logged_rect(log)
    if frame is None or world is None or not box or "clientExitPopup" not in log:
        check(f"native {tag}: screenshots with the popup open", False, f"logs in {WORK}")
        return
    ok, detail = seam_check(shade_ratio(frame, world), box, POPUP_SHADE)
    check(f"native {tag}: under the popup the shade is uniform up to the canvas edges", ok, detail)
    x, y, w, h = box
    H, W = frame.shape[:2]
    # the box without the screen's last row / column (#uiBase's scaled edge, a fraction of a px
    # short of the window like v7's in Chrome) and without the confirm box in the middle, found by
    # its YES / NO buttons (green, red): their span, from 2.5 button heights above them (the
    # question) to its border below them
    green = (frame[..., 1] > 180) & (frame[..., 0] < 140) & (frame[..., 2] < 140)
    red = (frame[..., 0] > 200) & (frame[..., 1] < 120) & (frame[..., 2] < 120)
    ys, xs_b = np.nonzero(green | red)
    keep = np.ones((H, W), bool)
    if len(xs_b):
        bh = ys.max() - ys.min() + 1
        pad = max(12, int(0.7 * bh))  # the box's padding and border
        keep[max(0, int(ys.min() - 2.5 * bh)):int(ys.max() + pad), max(0, xs_b.min() - pad):xs_b.max() + pad] = False
    region = (slice(max(0, y), min(H - 1, y + h)), slice(max(0, x), min(W - 1, x + w)))
    inner = frame[region].max(axis=2)[keep[region]]
    peak = float(inner.max()) if inner.size else 255.0
    check(f"native {tag}: the model is under the popup's backdrop", peak <= 255 * 0.25 + 4, f"brightest {peak:.0f}")


def native_startup(size, scale_ui):
    tag = f"startup-{size}-{scale_ui}"
    bad, seen = [], 0
    for n in NATIVE_STARTUP_FRAMES:
        frame, log = native_run(tag, size, scale_ui, frames=n)
        world, _ = native_run(tag, size, scale_ui, hide=1, frames=n)
        box = logged_rect(log)  # where the box is (once laid out it stays: checked at every frame)
        if frame is None or world is None or not box:
            bad.append(f"frame {n}: no screenshots or canvas box")
            continue
        seen += 1
        ok, detail = seam_check(shade_ratio(frame, world), box)
        if not ok:
            bad.append(f"frame {n} box {box}: {detail}")
    check(f"native {tag}: the shade is seamless at the canvas edges in the first frames {NATIVE_STARTUP_FRAMES}",
          seen and not bad, "; ".join(bad) or f"{seen} frames")


def native_main():
    try:
        import numpy  # noqa: F401
        from PIL import Image  # noqa: F401
    except ImportError:
        print("preview_hole_e2e: SKIP (needs numpy and pillow)")
        return 0
    if not os.path.isfile(os.path.join(BUILD, "client_bin")):
        check("native: the build has client_bin", False, BUILD)
        return 1
    for size, scale_ui in NATIVE_CASES:
        native_case(size, scale_ui)
    for size, scale_ui in NATIVE_POPUP_CASES:
        native_popup_case(size, scale_ui)
    native_startup("1280x720", 0.7)
    native_startup("1501x847", 0.65)
    return finish("preview_hole_e2e")


# ---------------------------------------------------------------- web

def web_main():
    import asyncio
    import base64
    import json
    import time
    import urllib.request
    try:
        import websockets
        import numpy as np
        from PIL import Image  # noqa: F401
    except ImportError:
        print("preview_hole_e2e --web: SKIP (needs python websockets, numpy and pillow)")
        return 0
    chrome = shutil.which("chromium-browser") or shutil.which("chromium") or shutil.which("google-chrome")
    if not chrome:
        print("preview_hole_e2e --web: SKIP (needs chromium or google-chrome)")
        return 0
    if not os.path.isfile(os.path.join(BUILD, "client.wasm")):
        check("web: the build has client.wasm", False, BUILD)
        return 1
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "server", "tests"))
    from kn_proto import free_ports
    http_port, cdp_port = free_ports(2)
    procs = [subprocess.Popen([sys.executable, "-m", "http.server", "--bind", "127.0.0.1", str(http_port)], cwd=BUILD,
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)]
    procs.append(subprocess.Popen([chrome, "--headless=new", "--no-sandbox", "--use-gl=angle", "--use-angle=swiftshader",
                                   "--enable-unsafe-swiftshader", "--hide-scrollbars", f"--remote-debugging-port={cdp_port}",
                                   f"--user-data-dir={os.path.join(WORK, 'chrome')}", "--window-size=1280,720",
                                   "about:blank"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                  start_new_session=True))
    page_url = f"http://127.0.0.1:{http_port}/?offline=1&map=sandstorm"
    hide_page = ("(() => { const s = document.createElement('style'); s.id = 'knTestHide';"
                 " s.textContent = 'body *:not(#canvas) { visibility: hidden !important; transition: none !important; }';"
                 " document.head.appendChild(s); return 1; })()")
    show_page = "(() => { const s = document.getElementById('knTestHide'); if (s) s.remove(); return 1; })()"
    # The bitmap is the page canvas's own size. Its pixels are not read here: web builds draw the preview
    # into a WebGL canvas of its own (web_dom.c), and getContext('2d') on it would break that; the model is
    # checked on the screenshots (model_check).
    geometry = ("(() => { const c = document.getElementById('classPreviewCanvas'); if (!c) return null;"
                " const b = c.getBoundingClientRect(); const cc = document.getElementById('menuClassContainer');"
                " return {dpr: devicePixelRatio, box: [b.left, b.top, b.width, b.height], bitmap: [c.width, c.height],"
                " opaque: -1, shown: !!(c.offsetParent || c.getClientRects().length) &&"
                " getComputedStyle(cc).visibility !== 'hidden' && document.documentElement.classList.contains('kn-menu-open')}; })()")

    async def session():
        target = None
        for _ in range(100):
            try:
                with urllib.request.urlopen(f"http://127.0.0.1:{cdp_port}/json/list", timeout=2) as r:
                    target = next((t for t in json.load(r) if t.get("type") == "page"), None)
                if target:
                    break
            except OSError:
                pass
            await asyncio.sleep(0.1)
        if not target:
            check("web: Chromium DevTools came up", False)
            return
        async with websockets.connect(target["webSocketDebuggerUrl"], max_size=1 << 28) as ws:
            ids = iter(range(1, 1 << 30))
            console = []
            replies = {}

            async def reader():
                async for raw in ws:
                    msg = json.loads(raw)
                    if msg.get("method") == "Runtime.consoleAPICalled":
                        console.append(" ".join(str(a.get("value", "")) for a in msg["params"].get("args", [])))
                    elif "id" in msg:
                        replies[msg["id"]] = msg

            task = asyncio.create_task(reader())

            async def call(method, **params):
                i = next(ids)
                await ws.send(json.dumps({"id": i, "method": method, "params": params}))
                for _ in range(1200):
                    if i in replies:
                        return replies.pop(i).get("result", {})
                    await asyncio.sleep(0.05)
                return {}

            async def until(pred, seconds):
                for _ in range(int(seconds * 20)):
                    if pred():
                        return True
                    await asyncio.sleep(0.05)
                return False

            async def evaluate(expr):
                r = await call("Runtime.evaluate", returnByValue=True, expression=expr)
                return r.get("result", {}).get("value")

            async def pause(on):
                # the game's frames stop (the WebGL canvas and the preview's canvas keep their last
                # frame), so a page screenshot and a world screenshot see the same world
                await evaluate("(() => { const m = window.Module; if (m && m.%s) m.%s(); return 1; })()"
                               % (("pauseMainLoop",) * 2 if on else ("resumeMainLoop",) * 2))
                await asyncio.sleep(0.15)

            async def pair(name):
                for attempt in range(3):
                    shots = await pair_once(f"{name}-{attempt}")
                    if shots:
                        return shots
                return None

            async def pair_once(name):
                """(frame, world) of one paused game frame: the page, then the page hidden; None when the
                world moved meanwhile (the game's loop can't be paused yet while it starts up: a world
                screenshot before and after the frame must agree) or none is drawn yet."""
                await pause(True)
                geo = await evaluate(geometry)  # the box of this very frame
                await evaluate(hide_page)
                before = await shot(os.path.join(WORK, f"web-{name}-world0.png"))
                await evaluate(show_page)
                frame = await shot(os.path.join(WORK, f"web-{name}-frame.png"))
                await evaluate(hide_page)
                world = await shot(os.path.join(WORK, f"web-{name}-world.png"))
                await evaluate(show_page)
                await pause(False)
                if frame.shape != world.shape or before.shape != world.shape:
                    return None
                if np.abs(before - world).mean() > 1.0 or np.median(world.sum(2)) < 60:
                    return None
                return frame, world, geo

            async def shot(path):
                r = await call("Page.captureScreenshot", format="png")
                with open(path, "wb") as f:
                    f.write(base64.b64decode(r.get("data", "")))
                return load_rgb(path)

            def device_box(geo):
                d = geo["dpr"]
                x, y, w, h = geo["box"]
                return round(x * d), round(y * d), round(w * d), round(h * d)

            async def measure(tag, popup=False):
                geo = await evaluate(geometry)
                if not geo or not geo.get("shown"):
                    check(f"web {tag}: the preview canvas is shown", False, str(geo))
                    return
                box = device_box(geo)
                if popup:
                    await evaluate("(() => { document.getElementById('popupHolder').style.display = 'block'; return 1; })()")
                    await asyncio.sleep(0.5)
                shots = await pair(tag)
                if popup:
                    await evaluate("(() => { document.getElementById('popupHolder').style.display = 'none'; return 1; })()")
                if not shots:
                    check(f"web {tag}: a still frame and its world", False)
                    return
                frame, world, _ = shots
                ratio = shade_ratio(frame, world)
                if popup:
                    ok, detail = seam_check(ratio, box, POPUP_SHADE)
                    check(f"web {tag}: under the popup the shade is uniform up to the canvas edges", ok, detail)
                    x, y, w, h = box
                    H, W = frame.shape[:2]
                    inner = frame[max(0, y):min(H - 1, y + h), max(0, x):min(W - 1, x + w)]
                    peak = float(inner.max()) if inner.size else 255.0
                    check(f"web {tag}: the model is under the popup's backdrop", peak <= 255 * 0.25 + 4,
                          f"brightest {peak:.0f}")
                    return
                # the bitmap is the box, within a device px (the box and the bitmap round the same CSS size apart)
                bitmap_ok = abs(geo["bitmap"][0] - box[2]) <= 1 and abs(geo["bitmap"][1] - box[3]) <= 1
                ok, detail = model_check(ratio, box, SHADE)
                check(f"web {tag}: the preview is the canvas's bitmap, drawn inside its box {box}", bitmap_ok and ok,
                      f"bitmap {geo['bitmap']}; {detail}")
                ok, detail = seam_check(ratio, box, SHADE)
                check(f"web {tag}: the shade is uniform up to the canvas edges", ok, detail)

            await call("Runtime.enable")
            await call("Page.enable")
            for n, (w, h, dpr) in enumerate(WEB_CASES):  # a fresh page at each size / devicePixelRatio
                tag = f"{w}x{h}@{dpr:g}"
                console.clear()
                await call("Emulation.setDeviceMetricsOverride", width=w, height=h, deviceScaleFactor=dpr, mobile=False)
                await call("Page.navigate", url=page_url)
                # startup: frames from the first menu frame on (the preview arrives last)
                start, startup_bad, startup_seen = time.monotonic(), [], 0
                tries = 0
                while time.monotonic() - start < 15.0 and (n in (0, 3) or not startup_seen):
                    geo = await evaluate(geometry)
                    if geo and geo.get("shown"):
                        shots = await pair(f"{tag}-start{tries:02d}")
                        tries += 1
                        if not shots:
                            continue  # no world yet, or it moved
                        frame, world, geo = shots
                        if not geo or not geo.get("shown"):
                            continue
                        startup_seen += 1
                        ok, detail = seam_check(shade_ratio(frame, world), device_box(geo))
                        if not ok:
                            startup_bad.append(f"{time.monotonic() - start:.1f}s: {detail}")
                    else:
                        await asyncio.sleep(0.05)
                menu = await until(lambda: any("[ui] setting" in line for line in console), 240)
                if not menu:
                    check(f"web {tag}: the menu came up", False)
                    continue
                if n in (0, 3):
                    check(f"web {tag}: the shade is seamless at the canvas edges while the menu starts up",
                          startup_seen >= 2 and not startup_bad, "; ".join(startup_bad) or f"{startup_seen} frames")
                await asyncio.sleep(4.0)  # the map, the preview model and #instructionsFadeBG's fade
                await measure(tag)
                if n in (0, 3, 5):
                    await measure(tag + "-popup", popup=True)
            # live resizes: the window changes size and the frames right after it (paused at 0..0.5 s)
            # must show no ring and the uniform shade, while the box moves and the preview catches up
            await call("Emulation.setDeviceMetricsOverride", width=1280, height=720, deviceScaleFactor=1, mobile=False)
            console.clear()
            await call("Page.navigate", url=page_url)
            if await until(lambda: any("[ui] setting" in line for line in console), 240):
                await asyncio.sleep(4.0)
                ring_bad, seam_bad, seen = [], [], 0
                sizes = [(1366, 768, 1.1), (1280, 720, 1.0), (1536, 864, 1.25), (1301, 733, 1.0)]
                for k in range(12):
                    w, h, dpr = sizes[k % len(sizes)]
                    await call("Emulation.setDeviceMetricsOverride", width=w, height=h, deviceScaleFactor=dpr, mobile=False)
                    await asyncio.sleep([0.0, 0.03, 0.1, 0.25, 0.5][k % 5])
                    shots = await pair(f"resize{k:02d}")
                    if not shots or not shots[2] or not shots[2].get("shown"):
                        continue
                    frame, world, geo = shots
                    seen += 1
                    ratio = shade_ratio(frame, world)
                    ok, detail = ring_check(ratio, device_box(geo))
                    if not ok:
                        ring_bad.append(f"#{k} {w}x{h}@{dpr:g}: {detail}")
                    ok, detail = seam_check(ratio, device_box(geo), SHADE)
                    if not ok:
                        seam_bad.append(f"#{k} {w}x{h}@{dpr:g}: {detail}")
                check("web live resizes: no ring at the canvas edges in the frames right after a resize",
                      seen >= 8 and not ring_bad, "; ".join(ring_bad) or f"{seen} frames")
                check("web live resizes: the shade is uniform up to the canvas edges",
                      seen >= 8 and not seam_bad, "; ".join(seam_bad) or f"{seen} frames")
            else:
                check("web live resizes: the menu came up", False)
            # browser zoom on a running page: the CSS viewport shrinks as devicePixelRatio grows, twice
            await call("Emulation.setDeviceMetricsOverride", width=1280, height=720, deviceScaleFactor=1, mobile=False)
            await asyncio.sleep(2.0)
            for w, h, dpr in ((1024, 576, 1.25), (853, 480, 1.5)):
                await call("Emulation.setDeviceMetricsOverride", width=w, height=h, deviceScaleFactor=dpr, mobile=False)
                await asyncio.sleep(2.0)
            await measure("zoom-1280x720@1-1.25-1.5")
            await call("Emulation.setDeviceMetricsOverride", width=1280, height=720, deviceScaleFactor=1, mobile=False)
            stats = None
            for _ in range(60):  # web_dom.c publishes the copy's cost every 30 frames it delivered
                await asyncio.sleep(1.0)
                stats = await evaluate("window.__knCanvasGl || null")
                if stats and stats.get("frames", 0) >= 120:
                    break
            check("web: the preview's copy into the page's canvas runs", stats and stats.get("frames", 0) > 0,
                  (f"{stats['frames']} frames of {stats['width']}x{stats['height']}: mean {stats['meanMs']:.3f} ms, "
                   f"max {stats['maxMs']:.3f} ms per frame") if stats else "no stats")
            task.cancel()
            with open(os.path.join(WORK, "console.log"), "w") as f:
                f.write("\n".join(console) + "\n")

    try:
        asyncio.run(asyncio.wait_for(session(), 1500))
    except asyncio.TimeoutError:
        check("web: finished in time", False)
    finally:
        for proc in procs:
            if proc.poll() is None:
                os.killpg(proc.pid, signal.SIGTERM)
                try:
                    proc.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    os.killpg(proc.pid, signal.SIGKILL)
                    proc.wait()
    return finish("preview_hole_e2e --web")


def finish(name):
    if all(results) and not os.environ.get("KN_TEST_KEEP"):
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print(f"files kept in {WORK}")
    print(f"{name}:", "PASS" if results and all(results) else "FAIL")
    return 0 if results and all(results) else 1


if __name__ == "__main__":
    sys.exit(web_main() if WEB else native_main())
