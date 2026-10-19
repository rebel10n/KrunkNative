#!/usr/bin/env python3
"""Window resizes during startup reach the viewport and the HTML layout (client/src/main.c).

  python3 -I client/tests/resize_early_e2e.py BUILD_DIR             (native, run from the repository root)
  python3 -I client/tests/resize_early_e2e.py --web WEB_DIST_DIR    (web build, e.g. dist/web)

The bug: "sometimes resizing the window before it loads the game causes the window resolution to
not update until i resize it again". The viewport and the HTML engine took the window size from
GLFW's framebuffer-size event only, so an event that never arrived, arrived with a stale size or
arrived before the consumer existed left them at the old size until the next resize.
main.c now compares the framebuffer size with the size it last applied every frame.

Runs client_bin --offline (Kanji) in a private Xvfb (never the user's display) with xfwm4 when it is
installed (the reparenting window manager the user runs) and resizes it with xdotool:
  1. at once, while the client is still starting (no menu, no map, no HTML page yet);
  2. in the middle of startup (page and map loading block the main loop);
  3. a burst of resizes ending during startup, like dragging the window border;
  4. a stale ConfigureNotify (a synthetic event with the wrong size, sent through libX11) after the
     menu is up: GLFW reports the stale size, the window keeps its real one. Event-only resizing
     keeps the stale size for good; the per-frame check puts the real size back.
KRUNKNATIVE_RESIZE_LOG=1 makes the client log "[resize] WxH viewport WxH page WxH" (the
GL_VIEWPORT and the HTML layout viewport it applied); the last line must match the window.
Needs Xvfb and xdotool. Prints PASS/FAIL lines; exit code 0 only when everything passed.

--web: the Emscripten build in headless Chromium (DevTools protocol, python websockets). The
viewport shrinks (Emulation.setDeviceMetricsOverride: a window resize event) once main() has
created the canvas, while startup still yields to the browser at its asset fetches; the canvas
must have the new size once the menu is up. The resize callback used to be registered after
startup, so that resize was lost and the canvas kept its first size until the next resize.
"""
import ctypes
import ctypes.util
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time

WEB = len(sys.argv) > 2 and sys.argv[1] == "--web"
BUILD = sys.argv[2 if WEB else 1] if len(sys.argv) > (2 if WEB else 1) else "cmake-build-debug"
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-resize-early-", dir=CACHE if os.path.isdir(CACHE) else None)
results = []


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def start_xvfb():
    """A private Xvfb on a free display number (-displayfd), never DISPLAY :0."""
    r, w = os.pipe()
    p = subprocess.Popen(["Xvfb", "-displayfd", str(w), "-screen", "0", "1920x1080x24", "-nolisten", "tcp"],
                         pass_fds=(w,), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
    os.close(w)
    number = b""
    deadline = time.time() + 15
    while time.time() < deadline and not number.endswith(b"\n"):
        chunk = os.read(r, 16)
        if not chunk:
            break
        number += chunk
    os.close(r)
    if not number.strip():
        p.kill()
        raise RuntimeError("Xvfb did not start")
    return p, ":" + number.decode().strip()


def xdotool(display, *args, timeout=10):
    out = subprocess.run(["xdotool", *args], env=dict(os.environ, DISPLAY=display), capture_output=True, text=True,
                         timeout=timeout)
    return out.stdout.strip()


def find_window(display, pid, deadline):
    while time.time() < deadline:
        ids = xdotool(display, "search", "--all", "--pid", str(pid), "--name", "Krunker Box Office")
        if ids:
            return ids.split()[0]
        time.sleep(0.005)
    return None


def window_size(display, wid):
    m = re.search(r"Geometry: (\d+)x(\d+)", xdotool(display, "getwindowgeometry", wid))
    return (int(m.group(1)), int(m.group(2))) if m else None


class XConfigureEvent(ctypes.Structure):
    _fields_ = [("type", ctypes.c_int), ("serial", ctypes.c_ulong), ("send_event", ctypes.c_int),
                ("display", ctypes.c_void_p), ("event", ctypes.c_ulong), ("window", ctypes.c_ulong),
                ("x", ctypes.c_int), ("y", ctypes.c_int), ("width", ctypes.c_int), ("height", ctypes.c_int),
                ("border_width", ctypes.c_int), ("above", ctypes.c_ulong), ("override_redirect", ctypes.c_int)]


class XEvent(ctypes.Union):
    _fields_ = [("xconfigure", XConfigureEvent), ("pad", ctypes.c_long * 24)]


def send_stale_configure(display, wid, width, height):
    """A synthetic ConfigureNotify claiming the window is width x height (it is not)."""
    x11 = ctypes.CDLL(ctypes.util.find_library("X11") or "libX11.so.6")
    x11.XOpenDisplay.restype = ctypes.c_void_p
    x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
    x11.XSendEvent.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_long, ctypes.POINTER(XEvent)]
    x11.XFlush.argtypes = [ctypes.c_void_p]
    x11.XCloseDisplay.argtypes = [ctypes.c_void_p]
    dpy = x11.XOpenDisplay(display.encode())
    if not dpy:
        return False
    ev = XEvent()
    ev.xconfigure.type = 22  # ConfigureNotify
    ev.xconfigure.send_event = 1
    ev.xconfigure.display = dpy
    ev.xconfigure.event = ev.xconfigure.window = int(wid)
    ev.xconfigure.width, ev.xconfigure.height = width, height
    ok = x11.XSendEvent(dpy, int(wid), 0, 1 << 17, ctypes.byref(ev))  # StructureNotifyMask
    x11.XFlush(dpy)
    x11.XCloseDisplay(dpy)
    return bool(ok)


def resize_lines(log):
    try:
        with open(log, errors="replace") as f:
            text = f.read()
    except OSError:
        return [], ""
    lines = [tuple(map(int, m.groups())) for m in
             re.finditer(r"^\[resize\] (\d+)x(\d+) viewport (\d+)x(\d+) page (\d+)x(\d+)$", text, re.M)]
    return lines, text


def wait_for(log, pattern, deadline):
    while time.time() < deadline:
        _, text = resize_lines(log)
        if re.search(pattern, text, re.M):
            return True
        time.sleep(0.05)
    return False


def run_case(name, display, action):
    log = os.path.join(WORK, f"{name}.log")
    env = {k: v for k, v in os.environ.items() if k not in ("WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL",
                                                            "KRUNKNATIVE_HEADLESS")}
    env.update({"DISPLAY": display, "XDG_CONFIG_HOME": os.path.join(WORK, f"{name}-cfg"),
                "XDG_STATE_HOME": os.path.join(WORK, f"{name}-state"),
                "XDG_RUNTIME_DIR": os.path.join(WORK, f"{name}-run"),  # no Discord RPC socket of the user's
                "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_AUDIO": "0", "KRUNKNATIVE_RESIZE_LOG": "1", "KRUNKNATIVE_WINDOW_SIZE": "1280x720"})
    os.makedirs(env["XDG_RUNTIME_DIR"], mode=0o700, exist_ok=True)
    with open(log, "w") as out:
        p = subprocess.Popen(["stdbuf", "-oL", f"{BUILD}/client_bin", "--offline", "assets/maps/official/10_kanji.json"], stdout=out,
                             stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, start_new_session=True)
    try:
        t0 = time.time()
        wid = find_window(display, p.pid, t0 + 30)
        if not wid:
            check(f"{name}: the client window appeared", False, log)
            return
        expected = action(wid, log, t0)
        # the menu is up (it pushed its settings), then a few more frames
        menu = wait_for(log, r"^\[ui\] setting ", time.time() + 90)
        time.sleep(2.0)
        actual = window_size(display, wid)
        lines, _ = resize_lines(log)
        last = lines[-1] if lines else None
        check(f"{name}: the menu came up", menu, log)
        check(f"{name}: the window has the size the test gave it", actual == expected, f"window {actual}, wanted {expected}")
        check(f"{name}: viewport and HTML layout follow the window",
              last and actual and last[0:2] == actual and last[2:4] == actual and last[4:6] == actual,
              f"window {actual}, last [resize] {last} ({len(lines)} lines), log {log}")
    finally:
        if p.poll() is None:
            os.killpg(p.pid, signal.SIGTERM)
            try:
                p.wait(timeout=10)
            except subprocess.TimeoutExpired:
                os.killpg(p.pid, signal.SIGKILL)
                p.wait()


def main():
    for tool in ("Xvfb", "xdotool"):
        if not shutil.which(tool):
            print(f"resize_early_e2e: SKIP ({tool} not installed)")
            return 0
    xvfb, display = start_xvfb()
    wm = None
    try:
        if shutil.which("xfwm4"):
            wm = subprocess.Popen(["xfwm4", "--compositor=off"], env=dict(os.environ, DISPLAY=display),
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
            time.sleep(1.0)
        print(f"private X display {display}, window manager: {'xfwm4' if wm else 'none'}", flush=True)

        def at_once(wid, log, t0):
            xdotool(display, "windowsize", wid, "900", "500")
            return (900, 500)

        def mid_startup(wid, log, t0):
            time.sleep(max(0.0, t0 + 1.0 - time.time()))
            xdotool(display, "windowsize", wid, "1100", "620")
            return (1100, 620)

        def drag(wid, log, t0):
            i = 0
            while time.time() < t0 + 2.5:
                i += 1
                xdotool(display, "windowsize", wid, str(700 + i * 37 % 500), str(400 + i * 53 % 300))
                time.sleep(0.03)
            xdotool(display, "windowsize", wid, "1000", "560")
            return (1000, 560)

        def stale_event(wid, log, t0):
            xdotool(display, "windowsize", wid, "960", "540")
            wait_for(log, r"^\[ui\] setting ", time.time() + 90)
            time.sleep(1.0)
            send_stale_configure(display, wid, 640, 400)
            time.sleep(0.5)
            lines, _ = resize_lines(log)
            stale = [line for line in lines if line[0:2] == (640, 400)]
            check("stale-event: GLFW reported the stale size (the test reaches the event path)", stale,
                  f"{len(lines)} [resize] lines")
            after = lines[lines.index(stale[-1]) + 1:] if stale else []
            check("stale-event: the next frame put the window's size back (within 0.5 s)",
                  after and after[0] == (960, 540, 960, 540, 960, 540), f"after the stale size: {after[:3]}")
            return (960, 540)

        run_case("at-once", display, at_once)
        run_case("mid-startup", display, mid_startup)
        run_case("drag", display, drag)
        run_case("stale-event", display, stale_event)
    finally:
        for proc in (wm, xvfb):
            if proc and proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
        if all(results) and not os.environ.get("KN_TEST_KEEP"):
            shutil.rmtree(WORK, ignore_errors=True)
        else:
            print(f"logs kept in {WORK}")

    print("resize_early_e2e:", "PASS" if all(results) else "FAIL")
    return 0 if all(results) else 1


def web_main():
    import asyncio
    import json
    import urllib.request
    try:
        import websockets
    except ImportError:
        print("resize_early_e2e --web: SKIP (python websockets not installed)")
        return 0
    chrome = shutil.which("chromium-browser") or shutil.which("chromium") or shutil.which("google-chrome")
    if not chrome:
        print("resize_early_e2e --web: SKIP (needs chromium or google-chrome)")
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
                                   "--enable-unsafe-swiftshader", f"--remote-debugging-port={cdp_port}",
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
        async with websockets.connect(target["webSocketDebuggerUrl"], max_size=1 << 26) as ws:
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
                for _ in range(600):
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

            async def canvas_size():
                r = await call("Runtime.evaluate", returnByValue=True, expression=(
                    "(() => { const c = document.getElementById('canvas');"
                    " return c ? [c.width, c.height, innerWidth, innerHeight, devicePixelRatio] : null; })()"))
                return r.get("result", {}).get("value")

            await call("Runtime.enable")
            await call("Page.enable")
            await call("Page.navigate", url=f"http://127.0.0.1:{http_port}/?offline=1&map=burg")
            started = await until(lambda: any("%c Renderer" in line for line in console), 120)
            check("web: main() created the canvas (startup banner)", started, f"{len(console)} console lines")
            loaded = any("[ui] setting" in line for line in console)
            # the window shrinks while startup is still loading (the page, settings, the map)
            await call("Emulation.setDeviceMetricsOverride", width=900, height=500, deviceScaleFactor=1, mobile=False)
            check("web: the resize came before the menu was up (during startup)", not loaded)
            menu = await until(lambda: any("[ui] setting" in line for line in console), 180)
            check("web: the menu came up", menu)
            await asyncio.sleep(2.0)
            size = await canvas_size()
            check("web: the canvas follows a resize made during startup",
                  size and size[0] == round(size[2] * size[4]) and size[1] == round(size[3] * size[4]) and size[2] == 900,
                  f"canvas {size[0:2] if size else None}, window {size[2:4] if size else None} x{size[4] if size else ''}")
            # and an ordinary resize once it runs
            await call("Emulation.setDeviceMetricsOverride", width=1100, height=600, deviceScaleFactor=1, mobile=False)
            await asyncio.sleep(1.0)
            size = await canvas_size()
            check("web: the canvas follows a resize while running", size and size[0:4] == [1100, 600, 1100, 600],
                  f"canvas {size}")
            task.cancel()
            with open(os.path.join(WORK, "console.log"), "w") as f:
                f.write("\n".join(console) + "\n")

    try:
        asyncio.run(asyncio.wait_for(session(), 420))
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
        if all(results) and not os.environ.get("KN_TEST_KEEP"):
            shutil.rmtree(WORK, ignore_errors=True)
        else:
            print(f"files kept in {WORK}")
    print("resize_early_e2e --web:", "PASS" if all(results) else "FAIL")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(web_main() if WEB else main())
