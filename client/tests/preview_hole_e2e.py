#!/usr/bin/env python3
"""The menu shade covers the class preview's box seamlessly (client/src/ui/class_preview.c, menu_ui.c).

  python3 -I client/tests/preview_hole_e2e.py BUILD_DIR             (native, run from the repository root)
  python3 -I client/tests/preview_hole_e2e.py --web WEB_DIST_DIR    (web build, e.g. dist/web)

The page cuts holes into its shades (#instructionHolder, #instructionsFadeBG) over the preview
canvas and class_preview.c dims that box itself under the page (then draws the model). Both must
cover exactly the same device pixels: a column of the hole the preview doesn't dim shows the game
undimmed ("to the left of the preview there's just a space where there's no transparent backdrop
covering the game. its just a vertical line"), one dimmed by both is darker.

Native: client_bin --offline headless (EGL, KRUNKNATIVE_EGL_DEVICE picks the GPU) with a pinned
camera, once as is and once with KRUNKNATIVE_HIDE_UI=1 (the world alone), at several window sizes
and UI scales. The shade makes frame / world ~0.5 wherever the page is plain shade; the columns
and rows on both sides of each edge of the box (KRUNKNATIVE_PREVIEW_LOG=1 logs it) must be ~0.5.

--web: headless Chromium (DevTools protocol, python websockets), a fresh page at several sizes
and devicePixelRatios, then a browser-zoom case (the ratio changing twice on a running page).
The holes' rect in device px must be whole pixels and the preview's rect. Pixels: a screenshot
of the canvas alone (page hidden) shows the preview's dim as a step down into the box, the
frame / canvas ratio shows the page's hole as a step up (0.5 shade outside, 1 inside); on every
edge both steps must sit exactly at the box edge (within 1 px for the stretched zoom canvas).
Prints PASS/FAIL lines; exit code 0 only when everything passed.
"""
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile

WEB = len(sys.argv) > 2 and sys.argv[1] == "--web"
BUILD = sys.argv[2 if WEB else 1] if len(sys.argv) > (2 if WEB else 1) else "cmake-build-debug"
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-preview-hole-", dir=CACHE if os.path.isdir(CACHE) else None)
TOLERANCE = 0.08  # |ratio - expected| per edge line (median over the line)
results = []

# window sizes (odd ones included) x scaleUI (settings.scaleUI: the #uiBase scale, 0.7 default;
# None = uiScaling off)
NATIVE_CASES = [("1280x720", 0.7), ("1366x768", 0.7), ("1600x900", 0.7), ("1920x1080", 0.7), ("2560x1440", 0.7),
                ("1277x713", 0.7), ("1501x847", 0.7), ("1920x1009", 0.7), ("1280x720", 0.3), ("1920x1080", 1.0),
                ("1366x768", None)]
WEB_CASES = [(1280, 720, 1.0), (1277, 713, 1.0), (1920, 1080, 1.0), (1280, 720, 1.25), (1536, 864, 1.25),
             (1280, 720, 1.5), (1366, 768, 1.1), (1280, 720, 2.0)]


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def load_gray(path):
    from PIL import Image
    import numpy as np
    return np.asarray(Image.open(path).convert("RGB"), dtype=float).sum(2)


def edge_lines(ratio, x, y, w, h, outside=0.5, inside=0.5):
    """Median ratio of the line just outside and the line just inside each edge of the box.
    Only rows (columns) whose context 3..6 px off the edge on both sides is plain shade
    (within TOLERANCE of `outside` / `inside`) count: the model, page text and buttons are
    skipped, and a seam (a 1-2 px line at the edge) still shows. None: too few clean rows."""
    import numpy as np
    H, W = ratio.shape
    out = {}

    def scan(edge, a, b):
        # sample k along the edge: (outside line, inside line, outside ctx, inside ctx)
        vals_o, vals_i = [], []
        for k in range(a, b):
            def at(d):  # d < 0: outside the box, d >= 0: inside
                if edge == "left":
                    c, r = x + d, k
                elif edge == "right":
                    c, r = x + w - 1 - d, k
                elif edge == "top":
                    c, r = k, y + d
                else:
                    c, r = k, y + h - 1 - d
                return ratio[r, c] if 0 <= r < H and 0 <= c < W else np.nan
            co = [at(-d) for d in range(4, 8)]
            ci = [at(d) for d in range(3, 7)]
            if any(np.isnan(v) or abs(v - outside) > TOLERANCE for v in co):
                continue
            if any(np.isnan(v) or abs(v - inside) > TOLERANCE for v in ci):
                continue
            lo, li = at(-1), at(0)
            if not (np.isnan(lo) or np.isnan(li)):
                vals_o.append(lo)
                vals_i.append(li)
        if len(vals_o) < 20:
            return None
        return float(np.median(vals_o)), float(np.median(vals_i)), len(vals_o)

    for edge in ("left", "right"):
        r = scan(edge, max(0, y + 8), min(H, y + h - 8))
        if r:
            out[edge] = r
    for edge in ("top", "bottom"):
        r = scan(edge, max(0, x + 8), min(W, x + w - 8))
        if r:
            out[edge] = r
    return out


def lines_ok(lines, outside, inside):
    bad = [k for k, (o, i, n) in lines.items() if abs(o - outside) > TOLERANCE or abs(i - inside) > TOLERANCE]
    detail = ", ".join(f"{k} {o:.2f}/{i:.2f} ({n} px)" for k, (o, i, n) in lines.items())
    return bool(lines) and not bad, ("BAD " + ", ".join(bad) + " | ") * bool(bad) + (detail or "no clean edge rows")


def edge_steps(img, x, y, w, h):
    """Per edge: {offset: step} for offsets -4..4, step = median over the edge's rows of the log
    ratio between a line and its outer neighbour (dimmer inside: negative). Offset 0 = between
    the last line outside the box and the first line inside it."""
    import numpy as np
    H, W = img.shape
    out = {}
    with np.errstate(divide="ignore", invalid="ignore"):
        L = np.log(img)
    for edge in ("left", "right", "top", "bottom"):
        steps = {}
        for d in range(-4, 5):
            if edge == "left":
                a, b = (L[y + 8:y + h - 8, x + d] if 0 < x + d < W else None), (L[y + 8:y + h - 8, x + d - 1] if 0 < x + d < W else None)
            elif edge == "right":
                c = x + w - 1 - d
                a, b = (L[y + 8:y + h - 8, c] if 0 <= c < W - 1 else None), (L[y + 8:y + h - 8, c + 1] if 0 <= c < W - 1 else None)
            elif edge == "top":
                a, b = (L[y + d, x + 8:min(W, x + w - 8)] if 0 < y + d < H else None), (L[y + d - 1, x + 8:min(W, x + w - 8)] if 0 < y + d < H else None)
            else:
                r = y + h - 1 - d
                a, b = (L[r, x + 8:min(W, x + w - 8)] if 0 <= r < H - 1 else None), (L[r + 1, x + 8:min(W, x + w - 8)] if 0 <= r < H - 1 else None)
            if a is None:
                continue
            with np.errstate(invalid="ignore"):
                v = a - b
            v = v[np.isfinite(v)]
            if len(v) >= 20:
                steps[d] = float(np.median(v))
        if len(steps) == 9:
            out[edge] = steps
    return out


def step_at_edge(steps, tolerance, want):
    """The step across offsets -tolerance..tolerance (a blurred edge spreads over them) must be
    `want` (log ratio, within 0.15) and every other offset flat: (ok, description)."""
    inside = sum(steps[d] for d in range(-tolerance, tolerance + 1))
    rest = max(abs(steps[d]) for d in steps if abs(d) > tolerance)
    peak = max(steps, key=lambda k: abs(steps[k]))
    return abs(inside - want) < 0.15 and rest < 0.15, f"{inside:+.2f} at {peak:+d}, off-edge max {rest:.2f}"


def native_case(size, scale_ui):
    import numpy as np
    tag = f"{size}-{'off' if scale_ui is None else scale_ui}"
    cfg = os.path.join(WORK, f"cfg-{tag}", "krunknative")
    os.makedirs(cfg, exist_ok=True)
    with open(os.path.join(cfg, "knSettings.json"), "w") as f:
        f.write('{"uiScaling": false}' if scale_ui is None else '{"scaleUI": %s}' % scale_ui)
    shots = {}
    rect = None
    for hide in (0, 1):
        png = os.path.join(WORK, f"{tag}-h{hide}.png")
        env = dict(os.environ, KRUNKNATIVE_HEADLESS="1", XDG_CONFIG_HOME=os.path.dirname(cfg),
                   KRUNKNATIVE_WINDOW_SIZE=size, KRUNKNATIVE_CAMERA="0,20,0,-0.1,0.5", KRUNKNATIVE_FIXED_DELTA="0.016",
                   KRUNKNATIVE_HIDE_UI=str(hide), KRUNKNATIVE_CAPTURE_FRAMES="120", KRUNKNATIVE_SCREENSHOT=png,
                   KRUNKNATIVE_PREVIEW_LOG="1")
        env.pop("DISPLAY", None)
        env.pop("WAYLAND_DISPLAY", None)
        try:
            p = subprocess.run([os.path.join(BUILD, "client_bin"), "--offline", "assets/maps/sandstorm.json"], env=env,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace", timeout=180)
            log = p.stdout
        except subprocess.TimeoutExpired:
            log = ""
        with open(os.path.join(WORK, f"{tag}-h{hide}.log"), "w") as f:
            f.write(log)
        if hide == 0:
            m = re.findall(r"\[preview\] rect (\d+),(\d+) (\d+)x(\d+)", log)
            rect = tuple(map(int, m[-1])) if m else None
        if not os.path.isfile(png):
            check(f"native {tag}: screenshot", False, f"log {os.path.join(WORK, f'{tag}-h{hide}.log')}")
            return
        shots[hide] = load_gray(png)
    if not rect:
        check(f"native {tag}: the preview logged its rect", False)
        return
    frame, world = shots[0], shots[1]
    ratio = np.where(world > 60, frame / np.maximum(world, 1), np.nan)
    ok, detail = lines_ok(edge_lines(ratio, *rect), 0.5, 0.5)
    check(f"native {tag}: shade seamless across the box edges (rect {rect[0]},{rect[1]} {rect[2]}x{rect[3]})", ok, detail)


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
    return finish("preview_hole_e2e")


def web_main():
    import asyncio
    import base64
    import json
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

            async def shot(path):
                r = await call("Page.captureScreenshot", format="png")
                with open(path, "wb") as f:
                    f.write(base64.b64decode(r.get("data", "")))
                return load_gray(path)

            async def measure(tag, tolerance):
                geo = await evaluate(
                    "(() => { const r = e => { const el = document.getElementById(e);"
                    " if (!el) return null; const b = el.getBoundingClientRect(); return [b.left, b.top, b.width, b.height]; };"
                    " const c = document.getElementById('canvas');"
                    " return {dpr: devicePixelRatio, hole: r('knPreviewHole'), fade: r('knFadeHole'),"
                    " preview: r('classPreviewCanvas'), canvas: [c.width, c.height]}; })()")
                if not geo or not geo.get("hole") or not geo.get("preview"):
                    check(f"web {tag}: the page has the preview hole", False, str(geo))
                    return
                d = geo["dpr"]
                hx, hy, hw, hh = (v * d for v in geo["hole"])
                px, py, pw, ph = geo["preview"]
                want = (round(px * d), round(py * d), round(pw * d), round(ph * d))
                whole = all(abs(v - round(v)) < 0.02 for v in (hx, hy, hx + hw, hy + hh))
                same = (round(hx), round(hy), round(hw), round(hh)) == want
                check(f"web {tag}: the holes are the preview's whole-pixel rect", whole and same,
                      f"hole x{d:g} = {', '.join(f'{v:.3f}' for v in (hx, hy, hw, hh))}, preview rect {want}, canvas {geo['canvas']}")
                frame = await shot(os.path.join(WORK, f"web-{tag}-frame.png"))
                await evaluate("(() => { const s = document.createElement('style'); s.id = 'knTestHide';"
                               " s.textContent = 'body *:not(#canvas) { visibility: hidden !important; }';"
                               " document.head.appendChild(s); return 1; })()")
                await asyncio.sleep(0.3)
                alone = await shot(os.path.join(WORK, f"web-{tag}-canvas.png"))
                await evaluate("(() => { document.getElementById('knTestHide').remove(); return 1; })()")
                if frame.shape != alone.shape:
                    check(f"web {tag}: screenshots", False, f"{frame.shape} vs {alone.shape}")
                    return
                # the preview's dim in the canvas (canvas alone: the world halves into the box) and the
                # page's hole (frame / canvas: 0.5 outside, 1 inside) must both step at the box edge
                ratio = np.where(alone > 60, frame / np.maximum(alone, 1), np.nan)
                ratio[(ratio < 0.35) | (ratio > 1.25)] = np.nan  # page text, buttons and panels, not shade
                dim = edge_steps(np.where(alone > 30, alone, np.nan), *want)
                hole = edge_steps(ratio, *want)
                bad, detail = [], []
                for edge in dim:
                    if edge not in hole:
                        continue
                    dim_ok, dim_text = step_at_edge(dim[edge], tolerance, float(np.log(0.5)))
                    hole_ok, hole_text = step_at_edge(hole[edge], tolerance, float(np.log(2.0)))
                    detail.append(f"{edge}: dim {dim_text}; hole {hole_text}")
                    if not (dim_ok and hole_ok):
                        bad.append(edge)
                check(f"web {tag}: the preview's dim and the page's hole step exactly at the box edges (+-{tolerance} px)",
                      detail and not bad, ("BAD " + ", ".join(bad) + " | ") * bool(bad) + ("; ".join(detail) or "no edges"))

            await call("Runtime.enable")
            await call("Page.enable")
            for w, h, dpr in WEB_CASES:  # a fresh page at each size / devicePixelRatio
                tag = f"{w}x{h}@{dpr:g}"
                console.clear()
                await call("Emulation.setDeviceMetricsOverride", width=w, height=h, deviceScaleFactor=dpr, mobile=False)
                await call("Page.navigate", url=f"http://127.0.0.1:{http_port}/?offline=1&map=sandstorm")
                menu = await until(lambda: any("[ui] setting" in line for line in console), 240)
                if not menu:
                    check(f"web {tag}: the menu came up", False)
                    continue
                await asyncio.sleep(4.0)  # the map, the preview model and #instructionsFadeBG's fade
                # a canvas of round(CSS px * dpr) px the browser stretches over CSS px * dpr device
                # px (e.g. 1366 x 1.1 = 1502.6) blurs edges over 2 px: 1 px of slack there
                await measure(tag, 0 if (w * dpr).is_integer() and (h * dpr).is_integer() else 1)
            # browser zoom on a running page: the CSS viewport shrinks as devicePixelRatio grows,
            # twice (Emscripten's GLFW keeps the canvas at the first new ratio and the browser
            # stretches it: within 1 px)
            for w, h, dpr in ((1024, 576, 1.25), (853, 480, 1.5)):
                await call("Emulation.setDeviceMetricsOverride", width=w, height=h, deviceScaleFactor=dpr, mobile=False)
                await asyncio.sleep(2.0)
            await measure("zoom-1280x720@1-1.25-1.5", 1)
            task.cancel()
            with open(os.path.join(WORK, "console.log"), "w") as f:
                f.write("\n".join(console) + "\n")

    try:
        asyncio.run(asyncio.wait_for(session(), 1200))
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
