#!/usr/bin/env python3
"""The Dedicated Server window (client/src/ui/dedicated_server_ui.c), opened from the Host Game flow.

  python3 -I client/tests/dedicated_server_window_e2e.py BUILD_DIR      (from the repository root)
  KN_WEB_DIST=<web dist dir> also runs the web build in headless Chromium (else that part is skipped)

The Host Game flow's last step has "Get the dedicated server". It used to open a krunk.io web page
(a 404); now it opens a window in the client, built like the Download window (showWindow(25)):
  - native: a headless offline client_bin driven by an input script goes Host Game (window 42) ->
    the flow (openHostWindow) -> a map -> the Server & command step, clicks the button, and the window
    shows: its head, this computer's package with its .sha256, every server_bin release asset of
    `server-latest` (release.yml's platforms), quick start, server.cfg, port forwarding (TCP and UDP
    21015), the coordinator listing (finder_url) and the dated VPS prices; Back returns to the flow's
    Server step;
  - web (KN_WEB_DIST): the same clicks in the browser over web_dom.c, the same content.
Also checks that no krunk.io page URL is left in the Host Game flow's code.
Exit code 0 when every check passed; logs are kept in ~/.cache/kn-dsw-e2e-* on failure.
"""
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
WORK = tempfile.mkdtemp(prefix="kn-dsw-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
failures = []

RELEASE = "https://github.com/rebel10n/KrunkNative/releases/download/server-latest/"
# .github/workflows/release.yml's release-notes rows (tools/release/package-server.sh names)
ASSETS = ["krunknative-server-linux-x86_64.tar.gz", "krunknative-server-linux-x86_64-bullseye.tar.gz",
          "krunknative-server-windows-x86_64.zip", "krunknative-server-macos-arm64.tar.gz"]
# what each section must say (case-insensitive, whitespace folded)
NEEDLES = ["21015", "TCP", "UDP", "TCP and UDP", "Port forwarding", "server.cfg", "public_ip", "finder_url",
           "matchmaker.krunk.io", "Get listed", "Host on a VPS", "DigitalOcean", "Hetzner", "Gcore",
           "as of October 2026", "Quick start"]


def check(ok, what):
    print(("PASS " if ok else "FAIL ") + what)
    if not ok:
        failures.append(what)


def fold(s):
    return re.sub(r"\s+", " ", s or "").strip()


# ---------------------------------------------------------------- source: no web page left
with open("client/src/ui/host_flow_ui.c") as f:
    flow_src = f.read()
with open("client/include/host_flow_ui.h") as f:
    flow_src += f.read()
check("krunk.io/host" not in flow_src and "HOST_FLOW_SERVER_DOWNLOAD_URL" not in flow_src,
      "the Host Game flow no longer links the krunk.io/host/dedicated-server page")
with open(".github/workflows/release.yml") as f:
    release = f.read()
check(all(a in release for a in ASSETS), "every linked asset is one release.yml publishes")


# ---------------------------------------------------------------- native
def run_native(script, timeout=240):
    path = os.path.join(WORK, "native.script")
    with open(path, "w") as f:
        f.write(script.strip() + "\n")
    log = os.path.join(WORK, "native.log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": os.path.join(WORK, "cfg"),
                "XDG_STATE_HOME": os.path.join(WORK, "state"), "KRUNKNATIVE_NO_BROWSER": "1",
                "KRUNKNATIVE_UPDATES_URL": "off", "KRUNKNATIVE_INPUT_SCRIPT": path, "KRUNKNATIVE_WINDOW_SIZE": "1280x720"})
    with open(log, "w") as out:
        p = subprocess.Popen([f"{BUILD}/client_bin", "--offline", "--map", "kanji"], stdout=out, stderr=subprocess.STDOUT,
                             stdin=subprocess.DEVNULL, env=env, start_new_session=True)
        try:
            p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            check(False, f"client_bin finished within {timeout} s")
        finally:
            if p.poll() is None:
                os.killpg(p.pid, signal.SIGKILL)
                p.wait()
    with open(log, errors="replace") as f:
        return f.read()


def vis(log, label):
    m = re.search(r"\[input_script\] vis t=\d+ " + re.escape(label) + r" (.*)", log)
    return dict(kv.split("=", 1) for kv in m.group(1).split()) if m else {}


def nodes(log, label):
    m = re.search(r"\[input_script\] nodes t=\d+ " + re.escape(label) + r" elements=\d+ match=(\d+)", log)
    return int(m.group(1)) if m else -1


def text(log, label):
    m = re.search(r"\[input_script\] text t=\d+ " + re.escape(label) + r" n=(\d+) checked=\d value=.*? text=(.*)", log)
    return m.group(2) if m else None


def grep(log, label):
    m = re.search(r"\[input_script\] grep t=\d+ " + re.escape(label) + r" needle=\S+ n=(\d+) hits=(\d+)", log)
    return int(m.group(2)) if m else -1


# '#' starts a comment in input scripts: ids are [id=...]
script = [
    "0 menu",
    "300 call showWindow 42",
    "800 call openHostWindow 0 0",
    '1200 call knHostFlow "map" "kanji"',
    '1500 call knHostFlow "go" 3',
    "2000 vis @flow hostFlowCommand hostFlowGetServer knDsHead",
    "2100 clickid hostFlowGetServer",
    "2700 vis @ds knDsHead knDsRec knDsOther knDsBack knDsQuick knDsCfg knDsPorts knDsList knDsVps knDsPrices hostFlowCommand",
    "2700 text @head [id=knDsHead]",
    "2700 nodes @sha a[href$='.sha256']",
    "2700 nodes @release a[href^='" + RELEASE + "']",
]
for i, a in enumerate(ASSETS):
    script.append(f"2700 nodes @asset{i} a[href='{RELEASE}{a}']")
for i, n in enumerate(NEEDLES):
    if " " not in n:  # grep takes one word
        script.append(f"2700 grep @n{i} {n} [id=menuWindow]")
script += ["2800 clickid knDsBack", "3400 vis @back hostFlowCommand knDsHead", "3500 quit"]
log = run_native("\n".join(script))

flow = vis(log, "@flow")
check(flow.get("hostFlowCommand") == "1", f"native: the Host Game flow's Server & command step is up ({flow})")
ds = vis(log, "@ds")
for key in ("knDsHead", "knDsRec", "knDsOther", "knDsBack", "knDsQuick", "knDsCfg", "knDsPorts", "knDsList", "knDsVps", "knDsPrices"):
    check(ds.get(key) == "1", f"native: Get the dedicated server -> the window shows #{key} ({ds.get(key)})")
check(ds.get("hostFlowCommand") in ("0", "-"), f"native: the flow's window was replaced ({ds.get('hostFlowCommand')})")
check(fold(text(log, "@head")) == "Dedicated Server", f"native: the head says Dedicated Server ({text(log, '@head')!r})")
check(nodes(log, "@sha") == len(ASSETS), f"native: one .sha256 link per package ({nodes(log, '@sha')})")
check(nodes(log, "@release") == 2 * len(ASSETS), f"native: every download is a server-latest release asset ({nodes(log, '@release')})")
for i, a in enumerate(ASSETS):
    check(nodes(log, f"@asset{i}") >= 1, f"native: {a} linked ({nodes(log, f'@asset{i}')})")
for i, n in enumerate(NEEDLES):
    if " " not in n:
        check(grep(log, f"@n{i}") >= 1, f"native: the window mentions {n!r}")
back = vis(log, "@back")
check(back.get("hostFlowCommand") == "1" and back.get("knDsHead") in ("0", "-"),
      f"native: Back to Host Game returns to the flow's Server step ({back})")


# ---------------------------------------------------------------- web build (KN_WEB_DIST)
def web_part():
    import asyncio
    import time  # noqa: F401
    import urllib.request
    try:
        import websockets
    except ImportError:
        print("SKIP  web: needs python websockets")
        return
    chrome = shutil.which("chromium-browser") or shutil.which("chromium") or shutil.which("google-chrome")
    dist = os.environ.get("KN_WEB_DIST", "")
    if not chrome or not os.path.isfile(os.path.join(dist, "client.wasm")):
        print("SKIP  web: needs chromium and KN_WEB_DIST (a web build's dist dir)")
        return
    sys.path.insert(0, os.path.join(os.getcwd(), "server", "tests"))
    from kn_proto import free_ports
    http_port, cdp_port = free_ports(2)
    own = [subprocess.Popen([sys.executable, "-m", "http.server", "--bind", "127.0.0.1", str(http_port)], cwd=dist,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True),
           subprocess.Popen([chrome, "--headless=new", "--no-sandbox", "--use-gl=angle", "--use-angle=swiftshader",
                             "--enable-unsafe-swiftshader", f"--remote-debugging-port={cdp_port}",
                             f"--user-data-dir={os.path.join(WORK, 'chrome')}", "--window-size=1280,720", "about:blank"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)]
    origin = f"http://127.0.0.1:{http_port}"
    snap_js = ("JSON.stringify({head: (document.getElementById('knDsHead') || {}).textContent || null, "
               "flow: !!document.getElementById('hostFlowCommand'), back: !!document.getElementById('knDsBack'), "
               "sections: ['knDsRec','knDsOther','knDsQuick','knDsCfg','knDsPorts','knDsList','knDsVps','knDsPrices']"
               ".filter(function (i) { return !!document.getElementById(i); }), "
               "hrefs: Array.prototype.map.call(document.querySelectorAll('#menuWindow a[href]'), function (a) { return a.getAttribute('href'); }), "
               "text: ((document.getElementById('menuWindow') || {}).textContent || '')})")
    result = {}

    async def session():
        target = None
        for _ in range(200):
            try:
                with urllib.request.urlopen(f"http://127.0.0.1:{cdp_port}/json/list", timeout=2) as r:
                    target = next((t for t in json.load(r) if t.get("type") == "page"), None)
                if target:
                    break
            except OSError:
                pass
            await asyncio.sleep(0.1)
        if not target:
            return
        async with websockets.connect(target["webSocketDebuggerUrl"], max_size=1 << 28) as ws:
            ids, replies = iter(range(1, 1 << 30)), {}

            async def reader():
                async for raw in ws:
                    msg = json.loads(raw)
                    if "id" in msg:
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

            async def evaluate(expr):
                r = await call("Runtime.evaluate", returnByValue=True, expression=expr)
                return r.get("result", {}).get("value")

            async def until(expr, seconds):
                for _ in range(int(seconds * 4)):
                    if await evaluate(expr):
                        return True
                    await asyncio.sleep(0.25)
                return False

            await call("Page.enable")
            await call("Page.navigate", url=origin + "/?offline=1&map=kanji")
            ready = await until("!!(window.windows && document.querySelector('#menuBtnHost .cardLabel'))", 120)
            result["ready"] = ready
            if ready:
                await asyncio.sleep(1.0)
                await evaluate("showWindow(42), 1")
                await until("typeof openHostWindow === 'function'", 10)
                await evaluate("openHostWindow(false, 0), 1")
                await until("!!document.querySelector('#menuWindow [onclick*=\"knHostFlow\"]')", 20)
                await evaluate("knHostFlow('map', 'kanji'), 1")
                await asyncio.sleep(0.5)
                await evaluate("knHostFlow('go', 3), 1")
                result["flow"] = await until("!!document.getElementById('hostFlowCommand')", 20)
                # the button itself, as a player clicks it
                await evaluate("(b => { if (b) b.click(); return !!b; })"
                               "(document.querySelector('#hostFlowGetServer [onclick]'))")
                await until("!!document.getElementById('knDsHead')", 20)
                result["ds"] = json.loads(await evaluate(snap_js) or "{}")
                await evaluate("(b => { if (b) b.click(); return !!b; })(document.getElementById('knDsBack'))")
                await until("!!document.getElementById('hostFlowCommand')", 20)
                result["back"] = json.loads(await evaluate(snap_js) or "{}")
            task.cancel()

    try:
        asyncio.run(asyncio.wait_for(session(), 300))
    except Exception as e:  # noqa: BLE001 - reported as a failed check
        result["error"] = repr(e)
    finally:
        for p in own:
            if p.poll() is None:
                os.killpg(p.pid, signal.SIGKILL)
                p.wait()
    check(result.get("ready") is True, f"web: the page and the client came up ({result.get('error', '')})")
    if not result.get("ready"):
        return
    check(result.get("flow") is True, "web: the Host Game flow's Server & command step is up")
    ds = result.get("ds", {})
    check(fold(ds.get("head")) == "Dedicated Server" and not ds.get("flow") and ds.get("back"),
          f"web: Get the dedicated server -> the window, with Back ({ds.get('head')!r})")
    check(len(ds.get("sections", [])) == 8, f"web: every section is there ({ds.get('sections')})")
    hrefs = ds.get("hrefs", [])
    for a in ASSETS:
        check(RELEASE + a in hrefs and RELEASE + a + ".sha256" in hrefs, f"web: {a} and its .sha256 linked")
    check(not any("krunk.io/host" in h for h in hrefs), "web: no krunk.io page link")
    body = fold(ds.get("text", "")).lower()
    for n in NEEDLES:
        check(n.lower() in body, f"web: the window mentions {n!r}")
    back = result.get("back", {})
    check(back.get("flow") is True and not back.get("head"), "web: Back to Host Game returns to the flow's Server step")


web_part()

if failures:
    print(f"{len(failures)} FAILED (logs: {WORK})")
    sys.exit(1)
shutil.rmtree(WORK, ignore_errors=True)
print("all passed")
