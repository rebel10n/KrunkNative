#!/usr/bin/env python3
"""The editor's AI panel in the real web client: headless Chromium on the Emscripten build (the
page's fetch() + ReadableStream stream, CORS against kn_brain) with kn_brain (sqlite) talking to the
stub upstream and stub MCP of tests/ai_stub.py (never the live provider).

  ai_web_e2e.py <native build dir> <repo root> [<web dist dir>]

A signed-in session (registered over the social socket, put into the page's localStorage) opens
editor.html, the canvas "AI" button opens the panel (the provider's 31 models smartest first,
glm-5.3-flash selected, no account indicator, the header row on one centre line, Tools / Web / Ask /
Stop one size, readable select and option colours), a prompt streams a reply, the stub then asks for
objects_create, which runs in the web editor against the open map, and its result goes back. Then
the checkpoints: Restore checkpoint (the object undone, Redo brings it back), Edit + resend (undo
the later edits), Rewind (+ Redo). Frames in the work directory (KN_KEEP=1 keeps it).
Web dist: argument, else $KN_WEB_DIST. 77 = skip (no web build, Chromium or websockets).
Ports from kn_proto.free_ports(); the profile, database and logs live in ~/.cache.
"""
import asyncio
import base64
import functools
import http.server
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request

try:
    import websockets
except ImportError:  # pragma: no cover
    print("python websockets missing: skip")
    sys.exit(77)

BUILD, ROOT = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
DIST = os.path.abspath(sys.argv[3] if len(sys.argv) > 3 else os.environ.get("KN_WEB_DIST", ""))
BRAIN = os.path.join(BUILD, "services", "brain", "kn_brain")
CHROME = os.environ.get("KN_CHROME") or shutil.which("chromium-browser") or shutil.which("chromium") or shutil.which("google-chrome")
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "server", "tests"))
if not (DIST and os.path.exists(os.path.join(DIST, "client.wasm"))) or not CHROME or not os.path.exists(BRAIN):
    print("needs a web build (KN_WEB_DIST), Chromium and kn_brain: skip")
    sys.exit(77)
from ai_stub import REAL_SMART_ORDER, Upstream, real_models  # noqa: E402
from kn_proto import free_ports  # noqa: E402

FAILS = []
KEY = "sk-web-%d" % os.getpid()


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what, flush=True)
    if not cond:
        FAILS.append(what)


class Cdp:
    def __init__(self, ws):
        self.ws, self.id, self.pending, self.logs = ws, 0, {}, []

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
        async for m in self.ws:
            msg = json.loads(m)
            if "id" in msg and msg["id"] in self.pending:
                self.pending.pop(msg["id"]).set_result(msg)
            elif msg.get("method") == "Runtime.consoleAPICalled":
                self.logs.append(" ".join(str(a.get("value", "")) for a in msg["params"]["args"]))

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
            raise RuntimeError("eval %s: %s" % (expr[:60], r["exceptionDetails"]))
        return r["result"].get("value")

    async def wait(self, expr, timeout=60.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                v = await self.eval(expr)
                if v:
                    return v
            except RuntimeError:
                pass
            await asyncio.sleep(0.25)
        return None

    async def shot(self, path):
        r = await self.send("Page.captureScreenshot", format="png")
        with open(path, "wb") as f:
            f.write(base64.b64decode(r["data"]))


async def register(port, name):
    async with websockets.connect("ws://127.0.0.1:%d/social/ws?enc=json" % port, max_size=4 << 20) as ws:
        await ws.send(json.dumps(["a", 0, [name, "webpass_1", None], None]))
        end = time.time() + 15
        while True:
            f = json.loads(await asyncio.wait_for(ws.recv(), end - time.time()))
            if f[0] == "a":
                return f[2], f[5]


async def ask(c, text):
    await c.eval("(function(){ var t = document.getElementById('knAiInput'); t.value = %s; "
                 "document.getElementById('knAiSend').click(); })()" % json.dumps(text))


async def scenario(http_port, cdp_port, brain_port, up, work):
    name = "aiweb%d" % (os.getpid() % 100000)
    acc_id, token = await register(brain_port, name)
    check(bool(token), "account registered")
    c = await Cdp.connect(cdp_port)
    await c.send("Runtime.enable")
    await c.send("Page.enable")
    await c.send("Emulation.setDeviceMetricsOverride", width=1280, height=720, deviceScaleFactor=1, mobile=False)
    origin = "http://127.0.0.1:%d" % http_port
    # the session in this origin's storage (what a sign-in in the hub leaves behind)
    await c.send("Page.navigate", url=origin + "/favicon.png")
    await asyncio.sleep(1)
    await c.eval("localStorage.setItem('krunker_username', %s); localStorage.setItem('krunker_token', %s); "
                 "localStorage.setItem('krunker_id', %s); true" % (json.dumps(name), json.dumps(token), json.dumps(str(acc_id))))
    await c.send("Page.navigate", url="%s/editor.html?new&mode=online&brain=http://127.0.0.1:%d&finder=off" % (origin, brain_port))
    ok = await c.wait("!!document.getElementById('knAiOption') && !!document.querySelector('#_toolbar .toolbarItem')", 180)
    check(ok, "the web editor is up")
    await asyncio.sleep(2)
    await c.eval("document.getElementById('knAiOption').click()")
    ok = await c.wait("(function(){ var p = document.getElementById('knAi'); return p && p.style.display !== 'none' && "
                      "document.querySelectorAll('#knAiModel option').length >= 3; })()", 30)
    check(ok, "the panel opens and lists the models")
    ids = await c.wait("(function(){ var o = document.querySelectorAll('#knAiModel option'); if (o.length < 31) return null; "
                       "return Array.prototype.map.call(o, function(x){ return x.value; }); })()", 30)
    check(ids == REAL_SMART_ORDER, "the models smartest first (%s)" % (ids or [])[:8])
    sel = await c.wait("document.getElementById('knAiModel').value === 'glm-5.3-flash'", 15)
    check(sel, "glm-5.3-flash selected by default")
    check(await c.eval("!document.getElementById('knAiAccount') && document.getElementById('knAi').textContent.indexOf(%s) < 0" % json.dumps(name)),
          "no account indicator in the panel")
    geo = await c.eval("""(function(){ var r = {}; ['knAi','knAiHead','knAiTitle','knAiModel','knAiNewBtn','knAiHistBtn','knAiSetBtn','knAiCloseBtn',
        'knAiTools','knAiWeb','knAiSend','knAiStop'].forEach(function(id){ var e = document.getElementById(id); if (!e) return;
        var hidden = e.style.display === 'none'; if (hidden) e.style.display = ''; var b = e.getBoundingClientRect();
        r[id] = [b.x, b.y, b.width, b.height]; if (hidden) e.style.display = 'none'; }); return r; })()""")
    k = geo["knAi"][2] / 380.0 if geo.get("knAi") else 1.0
    head = [geo.get(i) for i in ("knAiTitle", "knAiModel", "knAiNewBtn", "knAiHistBtn", "knAiSetBtn", "knAiCloseBtn")]
    cy = [r[1] + r[3] / 2 for r in head if r]
    check(len(cy) == 6 and max(cy) - min(cy) <= 0.5 and all(abs(r[3] / k - 24) < 0.6 for r in head),
          "header: title, model picker and icons 24 px on one centre line (%s)" % [round(v, 2) for v in cy])
    btn = [geo.get(i) for i in ("knAiTools", "knAiWeb", "knAiSend", "knAiStop")]
    check(all(btn) and all(abs(r[3] / k - 24) < 0.6 for r in btn) and max(r[1] + r[3] / 2 for r in btn) - min(r[1] + r[3] / 2 for r in btn) <= 0.5,
          "composer: Tools / Web / Ask / Stop one height (%s)" % [round(r[3] / k, 2) for r in btn if r])
    cols = await c.eval("""(function(){ function rgb(s){ var m = s.match(/[\\d.]+/g); return [+m[0], +m[1], +m[2]]; }
        var s = getComputedStyle(document.getElementById('knAiModel')), o = getComputedStyle(document.querySelector('#knAiModel option'));
        return [rgb(s.color), rgb(s.backgroundColor), rgb(o.color), rgb(o.backgroundColor)]; })()""")

    def lum(c):
        f = [v / 255.0 for v in c]
        f = [v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4 for v in f]
        return 0.2126 * f[0] + 0.7152 * f[1] + 0.0722 * f[2]

    def contrast(a, b):
        return (max(lum(a), lum(b)) + 0.05) / (min(lum(a), lum(b)) + 0.05)
    check(contrast(cols[0], cols[1]) >= 4.5 and contrast(cols[2], cols[3]) >= 4.5,
          "model picker and its options readable (select %.1f, option %.1f)" % (contrast(cols[0], cols[1]), contrast(cols[2], cols[3])))
    await ask(c, "hello")
    got = await c.wait("(document.getElementById('knAiList') || {}).textContent.indexOf('Hello from the stub: hello') >= 0", 30)
    check(got, "a streamed reply in the browser (fetch + ReadableStream through CORS)")
    req = up.chat_requests()[-1]
    check(req["body"]["messages"][-1] == {"role": "user", "content": "hello"} and req["body"].get("stream") is True, "the upstream got the prompt")
    create = json.dumps({"objects": [{"prefab": "CUBE", "p": [0, 0, 0], "s": [10, 10, 10]}]})
    await ask(c, "call tool objects_create " + create)
    got = await c.wait("(document.getElementById('knAiList') || {}).textContent.indexOf('Tool said: {\"created\"') >= 0", 40)
    text = await c.eval("(document.getElementById('knAiList') || {}).textContent")
    check(got, "objects_create ran in the web editor and its result went back (%s)" % (text or "")[-200:])
    count = await c.eval("(document.getElementById('objectCount') || {}).textContent")
    check(count and count.strip().startswith("1"), "the map has the new object (Total Objects %s)" % count)
    await c.shot(os.path.join(work, "web-ai.png"))
    hist = await c.eval("localStorage.getItem('knAiChats') || ''")
    check("call tool objects_create" in hist, "the chat is in localStorage (knAiChats)")

    # checkpoints (the hover controls are clicked directly)
    objs = "(document.getElementById('objectCount') || {}).textContent.trim()"
    listed = "(document.getElementById('knAiList') || {}).textContent"
    await c.eval("document.querySelector('#knAiU2 .knAiCpBtn').click()")
    ok = await c.wait("%s.indexOf('Restore this checkpoint?') >= 0" % listed, 10)
    await c.shot(os.path.join(work, "web-restore-confirm.png"))
    check(ok, "restore checkpoint asks first")
    await c.eval("document.querySelector('.knAiConfirm .knAiSend').click()")
    ok = await c.wait("%s.indexOf('0') === 0 && !document.getElementById('knAiU2') && !!document.getElementById('knAiRedoBar')" % objs, 10)
    await c.shot(os.path.join(work, "web-restored.png"))
    check(ok, "restore checkpoint: the object undone, the messages out, Redo offered (%s)" % await c.eval(objs))
    await c.eval("document.querySelector('#knAiRedoBar .knAiBtnS').click()")
    ok = await c.wait("%s.indexOf('1') === 0 && !!document.getElementById('knAiU2')" % objs, 10)
    check(ok, "redo: the object and the messages are back")
    await c.eval("document.querySelector('#knAiU0 .knAiAct').click()")
    ok = await c.wait("!!document.getElementById('knAiEditInput')", 10)
    await c.eval("document.getElementById('knAiEditInput').value = 'hello again'; document.querySelector('.knAiEditBar .knAiSend').click(); true")
    ok = ok and await c.wait("%s.indexOf('Undo the editor changes made after this message?') >= 0" % listed, 10)
    await c.shot(os.path.join(work, "web-edit-confirm.png"))
    check(ok, "edit: Send asks about the later editor changes")
    await c.eval("document.querySelector('.knAiConfirm .knAiSend').click()")
    ok = await c.wait("%s.indexOf('Hello from the stub: hello again') >= 0 && !document.getElementById('knAiU2') && %s.indexOf('0') === 0" % (listed, objs), 30)
    check(ok, "edit + resend: the edited message answered, the later edit undone")
    await c.eval("document.querySelectorAll('#knAiU0 .knAiAct')[1].click()")
    ok = await c.wait("!document.getElementById('knAiU0') && document.getElementById('knAiInput').value === 'hello again'", 10)
    await c.shot(os.path.join(work, "web-rewound.png"))
    check(ok, "rewind: the chat empties, the text back in the composer")
    await c.eval("document.querySelector('#knAiRedoBar .knAiBtnS').click()")
    ok = await c.wait("!!document.getElementById('knAiU0') && document.getElementById('knAiInput').value === ''", 10)
    check(ok, "rewind + redo: the messages back, the composer cleared")
    leaked = [l for l in c.logs if KEY in l or token in l]
    check(not leaked, "no key or session token in the console")


def main():
    work = tempfile.mkdtemp(prefix="kn-ai-web.", dir=os.path.expanduser("~/.cache"))
    http_port, cdp_port, brain_port = free_ports(3)

    class Quiet(http.server.SimpleHTTPRequestHandler):
        def log_message(self, *a):
            pass
    handler = functools.partial(Quiet, directory=DIST)
    httpd = http.server.ThreadingHTTPServer(("127.0.0.1", http_port), handler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    up = Upstream(KEY, models=real_models()).start()
    db_path = os.path.join(work, "coordinator.db")
    log = open(os.path.join(work, "brain.log"), "w")
    env = {k: v for k, v in os.environ.items() if not k.startswith("KN_AI_")}
    env.update(KN_AI_UPSTREAM_URL=up.url, KN_AI_UPSTREAM_KEY=KEY)
    brain = subprocess.Popen([BRAIN, "--brain.host=127.0.0.1", "--brain.port=%d" % brain_port, "--database.driver=sqlite",
                              "--database.path=" + db_path, "--migrate", "--accounts.email.required=false", "--smtp.host=",
                              "--ai.search.command=%s -I %s mcp" % (sys.executable, os.path.join(HERE, "ai_stub.py"))],
                             cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
    prof = os.path.join(work, "chrome")
    chrome = subprocess.Popen([CHROME, "--headless=new", "--no-sandbox", "--use-gl=angle", "--use-angle=swiftshader",
                               "--enable-unsafe-swiftshader", "--mute-audio", "--window-size=1280,720",
                               "--remote-debugging-port=%d" % cdp_port, "--user-data-dir=" + prof, "about:blank"],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
    try:
        for _ in range(100):
            try:
                urllib.request.urlopen("http://127.0.0.1:%d/ai/status" % brain_port, timeout=1).read()
                break
            except OSError:
                time.sleep(0.1)
        asyncio.run(asyncio.wait_for(scenario(http_port, cdp_port, brain_port, up, work), 400))
    except Exception as e:  # noqa: BLE001
        check(False, "exception: %r" % e)
    finally:
        for p in (chrome, brain):
            p.terminate()
            try:
                p.wait(10)
            except subprocess.TimeoutExpired:
                p.kill()
        httpd.shutdown()
        up.stop()
    log.close()
    with open(log.name) as f:
        text = f.read()
    check(KEY not in text, "no upstream key in the brain log")
    print("ai web e2e: " + ("OK" if not FAILS else "%d failure(s), files in %s" % (len(FAILS), work)))
    if not FAILS and not os.environ.get("KN_KEEP"):
        shutil.rmtree(work, ignore_errors=True)
    sys.exit(1 if FAILS else 0)


main()
