#!/usr/bin/env python3
"""The Skin Manager's Capture Thumbnail on the web build (client/src/ui/v10_skin_manager.c, client/src/viewer/viewer.c),
in headless Chromium against kn_brain (sqlite).

  python3 -I client/tests/v10_skin_capture_web_e2e.py <native build dir> <repo root> [<web dist dir>]

Web dist: the argument, else $KN_WEB_DIST. 77 = skip (no web build, Chromium, websockets or kn_brain).

As in v10.0.7, the editor's preview is the viewer page in an iframe (#knSmPreview). Capture Thumbnail posts
{type: 'capturePreview', size: 256} to it (deob-10.0.7.js:124565-124568), and the viewer page answers its parent with
{type: 'previewCaptured', dataURL} (:124764). The page turns the data URL into thumbnail.png, uploads it, sets
itemConfig.customThumbnail and saves the skin (:124765-124790). Checks:
- the sandbox page (?sandbox) keeps the account service, as the native --offline --sandbox does: the Skin Manager
  lists and creates skins;
- Capture Thumbnail: the brain stores a 256 x 256 PNG with the item drawn on a transparent background, the saved
  itemConfig has customThumbnail, and the button reads Captured;
- a capturePreview the viewer page gets from anything but its parent is not answered.
The profile, database and screenshots live in ~/.cache (removed when everything passed, unless KN_KEEP is set).
"""
import asyncio
import base64
import functools
import http.server
import json
import os
import re
import shutil
import sqlite3
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request
import zlib

try:
    import websockets
except ImportError:  # pragma: no cover
    print("python websockets missing: skip")
    sys.exit(77)

BUILD, ROOT = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
DIST = os.path.abspath(sys.argv[3] if len(sys.argv) > 3 else os.environ.get("KN_WEB_DIST", ""))
BRAIN = os.path.join(BUILD, "services", "brain", "kn_brain")
CHROME = os.environ.get("KN_CHROME") or shutil.which("chromium-browser") or shutil.which("chromium") or shutil.which("google-chrome")
sys.path.insert(0, os.path.join(ROOT, "server", "tests"))
if not (DIST and os.path.exists(os.path.join(DIST, "client.wasm"))) or not CHROME or not os.path.exists(BRAIN):
    print("needs a web build (KN_WEB_DIST), Chromium and kn_brain: skip")
    sys.exit(77)
from kn_proto import free_ports  # noqa: E402

FAILS = []
WORK = tempfile.mkdtemp(prefix="kn-skin-capture-web.", dir=os.path.expanduser("~/.cache"))
DB = os.path.join(WORK, "coordinator.db")
UPLOADS = os.path.join(WORK, "uploads")
PROFILE = os.path.join(WORK, "chrome")


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what, flush=True)
    if not cond:
        FAILS.append(what)


def q(sql, *args):
    con = sqlite3.connect(DB, timeout=30)
    try:
        rows = con.execute(sql, args).fetchall()
        con.commit()
        return rows
    finally:
        con.close()


def cfg_of(sid):
    rows = q("SELECT item_config FROM custom_skins WHERE id = ?", sid)
    try:
        return json.loads(rows[0][0]) if rows and rows[0][0] else {}
    except ValueError:
        return {}


async def poll(fn, timeout=20.0):
    end = time.time() + timeout
    while time.time() < end:
        v = fn()
        if v:
            return v
        await asyncio.sleep(0.25)
    return fn()


def png_rgba(data):
    """(w, h, rows of RGBA bytes) of an 8-bit RGBA, non-interlaced PNG; None for anything else"""
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        return None
    pos, idat, w = 8, b"", 0
    while pos + 8 <= len(data):
        n, t = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if t == b"IHDR":
            w, h, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", body)
            if depth != 8 or ctype != 6 or interlace:
                return None
        elif t == b"IDAT":
            idat += body
        pos += 12 + n
    raw, rows, prev, stride = zlib.decompress(idat), [], bytearray(w * 4), w * 4
    for y in range(h):
        f, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - 4] if i >= 4 else 0
            b = prev[i]
            c = prev[i - 4] if i >= 4 else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(bytes(line))
        prev = line
    return w, h, rows


class Cdp:
    def __init__(self, ws):
        self.ws, self.id, self.pending = ws, 0, {}

    @classmethod
    async def connect(cls, port):
        pages = []
        for _ in range(120):
            try:
                with urllib.request.urlopen("http://127.0.0.1:%d/json/list" % port, timeout=2) as r:
                    pages = [t for t in json.load(r) if t.get("type") == "page"]
                if pages:
                    break
            except OSError:
                pass
            await asyncio.sleep(0.5)
        c = cls(await websockets.connect(pages[0]["webSocketDebuggerUrl"], max_size=256 << 20))
        asyncio.get_event_loop().create_task(c.reader())
        return c

    async def reader(self):
        with open(os.path.join(WORK, "console.log"), "a") as log:
            async for m in self.ws:
                msg = json.loads(m)
                if "id" in msg and msg["id"] in self.pending:
                    self.pending.pop(msg["id"]).set_result(msg)
                elif msg.get("method") == "Runtime.consoleAPICalled":  # the page's and the iframe's stderr
                    args = msg["params"].get("args", [])
                    log.write(" ".join(str(a.get("value", a.get("description", ""))) for a in args) + "\n")
                    log.flush()

    async def send(self, method, **params):
        self.id += 1
        fut = asyncio.get_event_loop().create_future()
        self.pending[self.id] = fut
        await self.ws.send(json.dumps({"id": self.id, "method": method, "params": params}))
        msg = await asyncio.wait_for(fut, 60)
        if "error" in msg:
            raise RuntimeError("%s: %s" % (method, msg["error"]))
        return msg["result"]

    async def eval(self, expr):
        r = await self.send("Runtime.evaluate", expression=expr, returnByValue=True, awaitPromise=True, userGesture=True)
        if "exceptionDetails" in r:
            raise RuntimeError("eval %s: %s" % (expr[:80], r["exceptionDetails"]))
        return r["result"].get("value")

    async def wait(self, expr, timeout=30.0):
        end = time.time() + timeout
        while time.time() < end:
            try:
                v = await self.eval(expr)
                if v:
                    return v
            except RuntimeError:
                pass
            await asyncio.sleep(0.25)
        return None

    async def visible(self, sel, timeout=30.0):
        return await self.wait("(() => { const e = document.querySelector(%s); return !!e && e.getClientRects().length > 0; })()"
                               % json.dumps(sel), timeout)

    async def click(self, sel):
        """a real mouse click at the element's centre (element.click() has no position, and the page's view stack
        takes a click at 0, 0 for one outside the window)"""
        r = await self.eval("(() => { const e = document.querySelector(%s); if (!e) return null; e.scrollIntoView({block: 'nearest'});"
                            " const r = e.getBoundingClientRect(); return [r.x + r.width / 2, r.y + r.height / 2]; })()" % json.dumps(sel))
        if not r:
            raise RuntimeError("no element %s" % sel)
        for kind in ("mouseMoved", "mousePressed", "mouseReleased"):
            await self.send("Input.dispatchMouseEvent", type=kind, x=r[0], y=r[1], button="none" if kind == "mouseMoved" else "left",
                            buttons=1 if kind == "mousePressed" else 0, clickCount=0 if kind == "mouseMoved" else 1)
            await asyncio.sleep(0.05)
        await asyncio.sleep(0.4)

    async def text(self, sel):
        return await self.eval("(() => { const e = document.querySelector(%s); return e ? e.textContent : ''; })()" % json.dumps(sel))

    async def shot(self, name):
        r = await self.send("Page.captureScreenshot", format="png")
        with open(os.path.join(WORK, name + ".png"), "wb") as f:
            f.write(base64.b64decode(r["data"]))


async def register(port, name, studio):
    async with websockets.connect("ws://127.0.0.1:%d/social/ws?enc=json" % port) as ws:
        await ws.send(json.dumps(["a", 0, [name, "capture_pass_1", None]]))
        while True:
            f = json.loads(await asyncio.wait_for(ws.recv(), 20))
            if f[0] == "a":
                assert f[1] is None, f
                aid, tok = f[2], f[5]
                break
        await ws.send(json.dumps(["gapi", 7, "POST", "/studios", {"name": studio}]))
        while True:
            f = json.loads(await asyncio.wait_for(ws.recv(), 20))
            if f[0] == "gapi" and f[1] == 7:
                return aid, tok


async def scenario(http_port, cdp_port, brain_port, aid, name, token):
    c = await Cdp.connect(cdp_port)
    await c.send("Runtime.enable")
    await c.send("Page.enable")
    await c.send("Emulation.setDeviceMetricsOverride", width=1280, height=720, deviceScaleFactor=1, mobile=False)
    await c.send("Page.navigate", url="http://127.0.0.1:%d/favicon.png" % http_port)
    await asyncio.sleep(1.0)
    await c.eval("localStorage.setItem('krunker_username', %s); localStorage.setItem('krunker_token', %s)" % (json.dumps(name), json.dumps(token)))
    await c.send("Page.navigate", url="http://127.0.0.1:%d/index.html?sandbox&brain=http://127.0.0.1:%d" % (http_port, brain_port))

    check(await c.visible("#menuBtnSkinManager", 240), "the sandbox page shows the Skin Manager item")
    await asyncio.sleep(1.0)
    await c.click("#menuBtnSkinManager")
    check(await c.visible("#knSmWindow", 20), "Skin Manager opens its window")
    check(await c.wait("document.getElementById('knSmWindow').textContent.includes('No data available')", 30),
          "the table: the account's skins (none yet) from kn_brain")
    await c.click("#knSmAdd")
    rows = await poll(lambda: q("SELECT id FROM custom_skins WHERE owner_id = ?", aid), 20)
    check(len(rows) == 1, "Add new skin: \"New Skin\" is in kn_brain")
    sid = rows[0][0] if rows else 0
    check(await c.wait("((document.getElementById('skin-id') || {}).textContent || '').includes('%s')" % sid, 20),
          "the editor shows the new skin's id")
    await c.shot("web-editor")
    await c.click("#knSmTab1")
    check(await c.visible("#knSmPreview", 20), "the Appearance tab's 3D preview iframe")
    ready = await c.wait("(() => { const f = document.getElementById('knSmPreview'); const m = f && f.contentWindow && f.contentWindow.Module;"
                         " return !!(m && typeof m.ccall === 'function' && f.contentDocument.readyState === 'complete'); })()", 180)
    await c.eval("window.__knFrame = document.getElementById('knSmPreview')")  # the same element after the updates below
    check(ready, "the iframe runs the viewer page")
    await asyncio.sleep(6.0)  # the showcase's model and textures over HTTP
    await c.shot("web-editor-preview")

    # a capturePreview the viewer page posts to itself (its source is not the parent) gets no answer
    await c.eval("window.__knCaps = 0; window.addEventListener('message', e => { if (e.data && e.data.type === 'previewCaptured') window.__knCaps++; })")
    await c.eval("document.getElementById('knSmPreview').contentWindow.eval(\"window.postMessage({type: 'capturePreview', size: 64}, '*')\")")
    await asyncio.sleep(2.0)
    check(await c.eval("window.__knCaps") == 0, "a capturePreview the viewer's own window posts is not answered (only its parent's)")

    await c.click("#knSmCapture")
    check(await c.eval("window.__knFrame === document.getElementById('knSmPreview') && window.__knFrame.isConnected"),
          "the window's update keeps the preview iframe (its viewer page is not loaded again, as Svelte keeps it)")
    label = await c.text("#knSmCapture")
    check("Capturing" in (label or "") or "Captured" in (label or ""), "Capture Thumbnail: the button reads Capturing... (%r)" % label)
    thumb = os.path.join(UPLOADS, "skins", str(sid), "thumbnail.png")
    ok = await poll(lambda: os.path.isfile(thumb) and cfg_of(sid).get("customThumbnail") is True, 30)
    check(ok, "the brain stores thumbnail.png and the saved itemConfig has customThumbnail")
    check(await c.eval("window.__knCaps") == 1, "the viewer page answered the page's capturePreview with one previewCaptured")
    check(await c.wait("document.getElementById('knSmCapture').textContent.includes('Captured')", 15), "the button reads Captured")
    img = png_rgba(open(thumb, "rb").read()) if os.path.isfile(thumb) else None
    check(img is not None and img[:2] == (256, 256), "thumbnail.png is a 256 x 256 RGBA PNG (capturePreview's size)")
    if img:
        _, _, rows = img
        alpha = [r[i] for r in rows for i in range(3, len(r), 4)]
        opaque = sum(1 for a in alpha if a > 200)
        check(rows[0][3] == 0 and rows[-1][-1] == 0, "the capture's background is transparent (its corners)")
        check(opaque > 256 * 256 // 50, "the item is drawn in it (%d opaque pixels)" % opaque)
    await c.shot("web-captured")
    await c.ws.close()


def main():
    http_port, cdp_port, brain_port = free_ports(3)

    class Quiet(http.server.SimpleHTTPRequestHandler):
        def log_message(self, *a):
            pass
    httpd = http.server.ThreadingHTTPServer(("127.0.0.1", http_port), functools.partial(Quiet, directory=DIST))
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    cfg = os.path.join(WORK, "brain.json")
    with open(cfg, "w") as f:
        json.dump({"database": {"driver": "sqlite", "path": DB, "migrations_dir": os.path.join(ROOT, "services", "db", "migrations")},
                   "brain": {"host": "127.0.0.1", "port": brain_port, "federation": False, "internal_secret": "capture-web-%d" % os.getpid()},
                   "uploads": {"dir": UPLOADS},
                   "accounts": {"signup_rewards": {"enabled": False}, "free_premium": False, "starting_kr": 0, "login_throttle_s": 0,
                                "password_iterations": 1000, "skins_file": os.path.join(ROOT, "assets", "skins.json")}}, f)
    log = open(os.path.join(WORK, "brain.log"), "w")
    brain = subprocess.Popen([BRAIN, "--config", cfg, "--migrate"], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
    chrome = subprocess.Popen([CHROME, "--headless=new", "--no-sandbox", "--use-gl=angle", "--use-angle=swiftshader",
                               "--enable-unsafe-swiftshader", "--mute-audio", "--window-size=1280,720",
                               "--remote-debugging-port=%d" % cdp_port, "--user-data-dir=" + PROFILE, "about:blank"],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
    try:
        for _ in range(200):
            with open(os.path.join(WORK, "brain.log")) as f:
                if re.search(r"listening on http://127\.0\.0\.1:%d" % brain_port, f.read()):
                    break
            time.sleep(0.1)
        stamp = str(int(time.time()) % 100000)
        name = "skW%s" % stamp
        aid, tok = asyncio.run(register(brain_port, name, "WebStudio" + stamp))
        asyncio.run(asyncio.wait_for(scenario(http_port, cdp_port, brain_port, aid, name, tok), 600))
    except Exception as e:  # noqa: BLE001
        import traceback
        traceback.print_exc()
        check(False, "exception: %r" % e)
    finally:
        for p in (chrome, brain):
            p.terminate()
            try:
                p.wait(10)
            except subprocess.TimeoutExpired:
                p.kill()
        # Chromium's GPU / zygote children leave its process group: end whatever still runs on this profile
        subprocess.run(["pkill", "-f", "--", "--user-data-dir=" + PROFILE], check=False)
        httpd.shutdown()
        log.close()
    print("skin capture web e2e: " + ("OK" if not FAILS else "%d failure(s), files in %s" % (len(FAILS), WORK)))
    if not FAILS and not os.environ.get("KN_KEEP"):
        shutil.rmtree(WORK, ignore_errors=True)
    sys.exit(1 if FAILS else 0)


main()
