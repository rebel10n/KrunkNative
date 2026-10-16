#!/usr/bin/env python3
"""Krunker Strike as its own installable app next to Krunker Box Office (platform/web/pwa.py,
platform/README.md "Krunker Strike app"), on a web build in headless Chromium.

  python3 -I client/tests/strike_app_e2e.py DIST_WEB      (run from the repository root; make strike-app-test)

The dist is served by a local HTTP server (game content from KRUNKNATIVE_ASSETS_DIR, else the dist's
own assets/), one headless Chromium drives it over CDP.
  1. files: both manifests parse, have their own name / short_name / id / start_url / scope (Box
     Office ./ at the root, Strike ./ in strike/), theme colours, and every icon is a PNG of the
     size it declares; the Strike icons are not Box Office's.
  2. Box Office (/): Chrome's own manifest parser (Page.getAppManifest) and app id (Page.getAppId)
     see /manifest.webmanifest, id /, scope /; the client starts as Krunker Box Office (its banner on
     the console, no kn-flavor-strike class) and writes no Strike storage keys.
  3. Strike (/strike/, the manifest's start_url, no flag): Chrome sees /strike/manifest.webmanifest,
     id /strike/, scope /strike/ (a different app), no installability errors beyond Box Office's;
     the client starts as Krunker Strike (banner, FRVR's page up in #frvrMenuMobileHolder, or on
     the generated page body.kn-flavor-strike; "Krunker Strike" title);
     its own storage keys all start with "strike." and the Box Office keys are untouched.
  4. Strike offline (/strike/?offline=1&map=burg): the address bar the client writes stays in
     /strike/ (a reload stays in the Strike app).
  The Download window (showWindow(25)) lists the Strike app (APK, .ipa, page, logo) after Box
  Office's packages, and first in the Strike app.
Prints PASS/FAIL lines; exit 0 only when everything passed, 77 when Chromium or websockets is missing.
"""
import asyncio
import functools
import http.server
import json
import os
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import threading
import urllib.parse
import urllib.request

try:
    import websockets
except ImportError:
    print("SKIP  needs python websockets")
    sys.exit(77)

sys.path.insert(0, os.path.join(os.getcwd(), "server/tests"))
import kn_proto  # noqa: E402

DIST = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "dist/web")
ASSETS = os.environ.get("KRUNKNATIVE_ASSETS_DIR", "")
WORK = tempfile.mkdtemp(prefix="kn-strike-app-e2e-", dir=os.environ.get("TMPDIR") or None)
results = []


def check(name, ok, detail=""):
    results.append(bool(ok))
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + str(detail)) if detail else ''}", flush=True)


def png_size(path):
    with open(path, "rb") as f:
        head = f.read(24)
    if head[:8] != b"\x89PNG\r\n\x1a\n":
        return None
    return struct.unpack(">II", head[16:24])


def check_files():
    apps = {}
    for label, rel in (("Box Office", ""), ("Strike", "strike/")):
        path = os.path.join(DIST, rel, "manifest.webmanifest")
        try:
            with open(path) as f:
                m = json.load(f)
        except (OSError, ValueError) as e:
            check(f"{label}: {rel}manifest.webmanifest parses", False, e)
            continue
        check(f"{label}: {rel}manifest.webmanifest parses", True)
        apps[label] = m
        base = "https://site/" + rel
        start = urllib.parse.urljoin(base, m.get("start_url", ""))
        scope = urllib.parse.urljoin(base, m.get("scope", ""))
        check(f"{label}: start_url {start} inside scope {scope}", start.startswith(scope))
        # the manifest spec resolves id against start_url's ORIGIN (not the manifest's URL): "./" in
        # strike/ would be the root, Box Office's app; Chrome agrees (Page.getAppId below)
        m["_id"] = urllib.parse.urljoin("https://site/", m["id"]) if "id" in m else start
        m["_scope"] = scope
        for icon in m.get("icons", []):
            file = os.path.join(DIST, rel, icon["src"])
            want = tuple(int(v) for v in icon["sizes"].split("x"))
            got = png_size(file) if os.path.isfile(file) else None
            check(f"{label}: icon {icon['src']} ({icon.get('purpose', 'any')}) is a {icon['sizes']} PNG", got == want, got)
    bo, st = apps.get("Box Office"), apps.get("Strike")
    if not (bo and st):
        return
    check("names: Krunker Box Office / Krunker Strike", (bo["name"], st["name"]) == ("Krunker Box Office", "Krunker Strike"),
          (bo["name"], st["name"]))
    check("short names differ", bo["short_name"] != st["short_name"], (bo["short_name"], st["short_name"]))
    check("ids differ (/ and /strike/)", (bo["_id"], st["_id"]) == ("https://site/", "https://site/strike/"), (bo["_id"], st["_id"]))
    check("scopes differ (/ and /strike/)", (bo["_scope"], st["_scope"]) == ("https://site/", "https://site/strike/"))
    check("Strike theme colour is the Strike gold", st.get("theme_color", "").lower() == "#f9c80e", st.get("theme_color"))
    with open(os.path.join(DIST, "img/pwa/icon-192.png"), "rb") as a, open(os.path.join(DIST, "strike/img/pwa/icon-192.png"), "rb") as b:
        check("Strike icons are its own", a.read() != b.read())


class Handler(http.server.SimpleHTTPRequestHandler):
    def translate_path(self, path):
        rel = urllib.parse.unquote(urllib.parse.urlsplit(path).path)
        if ASSETS and rel.startswith("/assets/"):
            return os.path.join(ASSETS, rel[len("/assets/"):])
        return super().translate_path(path)

    def log_message(self, *args):
        pass


async def browser_checks(origin, cdp_port):
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
        check("Chromium came up", False)
        return
    async with websockets.connect(target["webSocketDebuggerUrl"], max_size=1 << 28) as ws:
        ids, replies, console = iter(range(1, 1 << 30)), {}, []

        async def reader():
            async for raw in ws:
                msg = json.loads(raw)
                if "id" in msg:
                    replies[msg["id"]] = msg
                elif msg.get("method") == "Runtime.consoleAPICalled":
                    console.append(" ".join(str(a.get("value", "")) for a in msg["params"].get("args", [])))

        task = asyncio.create_task(reader())

        async def call(method, **params):
            i = next(ids)
            await ws.send(json.dumps({"id": i, "method": method, "params": params}))
            for _ in range(2400):
                if i in replies:
                    return replies.pop(i).get("result", {})
                await asyncio.sleep(0.05)
            return {}

        async def evaluate(expr):
            r = await call("Runtime.evaluate", returnByValue=True, awaitPromise=True, expression=expr)
            return r.get("result", {}).get("value")

        async def until(expr, seconds):
            for _ in range(int(seconds * 4)):
                if await evaluate(expr):
                    return True
                await asyncio.sleep(0.25)
            return False

        async def banner(seconds=120):
            for _ in range(int(seconds * 4)):
                hit = next((c for c in console if "(KrunkNative client)" in c), None)
                if hit:
                    return hit
                await asyncio.sleep(0.25)
            return None

        async def open_page(path):
            console.clear()
            await call("Page.navigate", url=origin + path)
            line = await banner()
            # the menu: flavor_ui_apply runs from its setup (menu_ui.c), so wait for the main menu
            await until("!!document.getElementById('gameNameHolder') && !!window.__krunknative_wasm_ready", 60)
            await asyncio.sleep(4.0)
            return line

        async def app_info():
            manifest = await call("Page.getAppManifest")
            app_id = await call("Page.getAppId")
            installability = await call("Page.getInstallabilityErrors")
            return manifest, app_id, [e.get("errorId") for e in installability.get("installabilityErrors", [])]

        # the Download window (download_ui.c): the Strike app's section and where it sits
        downloads_js = ("(() => { const s = document.querySelector('.knDlStrike'); if (!s) return null;"
                        " const all = Array.from(document.querySelectorAll('.knDlStrike, .knDlRec, .knDlOther')).map(e => e.className);"
                        " return JSON.stringify({first: all[0], links: Array.from(s.querySelectorAll('a')).map(a => a.href),"
                        " logo: (s.querySelector('img') || {}).complete && (s.querySelector('img') || {}).naturalWidth > 0}); })()")

        async def downloads():
            await evaluate("showWindow(25), 1")
            await until("!!document.querySelector('.knDlStrike')", 20)
            await asyncio.sleep(1.0)
            return json.loads(await evaluate(downloads_js) or "null")

        def check_downloads(label, dl, first):
            check(f"{label}: the Download window lists the Krunker Strike app", bool(dl), dl)
            if not dl:
                return
            links = dl.get("links", [])
            check(f"{label}: Strike APK, .ipa and page links",
                  any(u.endswith("/krunknative-strike-android.apk") for u in links)
                  and any(u.endswith("/krunknative-strike-ios-unsigned.ipa") for u in links)
                  and origin + "/strike/" in links, links)
            check(f"{label}: the Strike logo shows", dl.get("logo") is True)
            check(f"{label}: the Strike section {'first' if first else 'after Box Office'}",
                  (dl.get("first") == "knDlStrike") == first, dl.get("first"))

        keys_js = "JSON.stringify(Object.keys(localStorage).sort())"
        await call("Page.enable")
        await call("Runtime.enable")
        await call("Page.navigate", url=origin + "/img/pwa/icon-32.png")  # the origin, for localStorage
        await asyncio.sleep(0.5)
        await evaluate("localStorage.clear(), 1")

        # 2. Krunker Box Office
        line = await open_page("/")
        check("Box Office: the client starts as Krunker Box Office", bool(line) and line.startswith("Krunker Box Office"), line)
        check("Box Office: no Strike branding on the page", await evaluate("document.body.classList.contains('kn-flavor-strike')") is False)
        manifest, app_id, bo_errors = await app_info()
        check("Box Office: Chrome finds /manifest.webmanifest", manifest.get("url") == origin + "/manifest.webmanifest", manifest.get("url"))
        check("Box Office: Chrome parses it without errors", manifest.get("errors") == [], manifest.get("errors"))
        check("Box Office: app id /", app_id.get("appId") == origin + "/", app_id)
        # a setting each app stores (html_storage_set): the language, picked in Settings
        async def pick_language(code):
            await evaluate("showWindow(1), 1")
            await until("!!document.getElementById('knLangSelect')", 20)
            await evaluate("(s => { s.value = %s; s.dispatchEvent(new Event('change')); return 1; })"
                           "(document.getElementById('knLangSelect'))" % json.dumps(code))
            await asyncio.sleep(3.0)
            await evaluate("typeof closWind === 'function' && closWind(), 1")

        await pick_language("de")
        bo_keys = json.loads(await evaluate(keys_js) or "[]")
        check("Box Office: stores its settings under its own keys (Deutsch: krk_lang), none in the Strike namespace",
              await evaluate("localStorage.getItem('krk_lang')") == "de" and not any(k.startswith("strike.") for k in bo_keys), bo_keys)
        bo_settings = {k: await evaluate("localStorage.getItem(%s)" % json.dumps(k)) for k in bo_keys}
        check_downloads("Box Office", await downloads(), False)

        # 3. Krunker Strike from its start_url
        line = await open_page("/strike/")
        check("Strike: /strike/ (the start_url, no flag) starts the client as Krunker Strike",
              bool(line) and line.startswith("Krunker Strike"), line)
        # FRVR's Strike page (ui/strike/index.html, strike_ui.c) once the tree has it, else the game
        # page pwa.py generates (Krunker's menu with the Strike branding of flavor_ui.c)
        frvr = await until("!!document.getElementById('frvrMenuMobileHolder')", 10)
        if frvr:
            check("Strike: FRVR's Strike page is up, driven by the client (screens in #frvrMenuMobileHolder)",
                  await until("document.getElementById('frvrMenuMobileHolder').children.length > 0", 30))
        else:
            check("Strike: the page is branded (body.kn-flavor-strike)",
                  await until("document.body.classList.contains('kn-flavor-strike')", 30))
        title = await evaluate("document.title")
        check("Strike: the page title names Krunker Strike", (title or "").startswith("Krunker Strike"), title)
        check("Strike: the address stays /strike/", await evaluate("location.pathname") == "/strike/", await evaluate("location.pathname"))
        manifest, app_id, st_errors = await app_info()
        check("Strike: Chrome finds /strike/manifest.webmanifest", manifest.get("url") == origin + "/strike/manifest.webmanifest", manifest.get("url"))
        check("Strike: Chrome parses it without errors", manifest.get("errors") == [], manifest.get("errors"))
        parsed = json.loads(manifest.get("data") or "{}")
        check("Strike: the manifest says Krunker Strike", parsed.get("name") == "Krunker Strike", parsed.get("name"))
        check("Strike: app id /strike/, not Box Office's", app_id.get("appId") == origin + "/strike/", app_id)
        check("Strike: installable like Box Office (the same installability errors, if any)", sorted(st_errors) == sorted(bo_errors),
              {"strike": st_errors, "box office": bo_errors})
        icons = await evaluate("Promise.all(['link[rel=apple-touch-icon]', 'link[rel=icon][sizes=\"32x32\"]', 'link[rel=manifest]']"
                               ".map(s => { const l = document.querySelector(s); return l ? fetch(l.href).then(r => l.href + ' ' + r.status) : s + ' missing'; }))")
        check("Strike: its manifest and home screen icons load from /strike/",
              bool(icons) and all(" 200" in i and "/strike/" in i for i in icons), icons)
        if not frvr:  # Krunker's Settings and Download windows: only on the generated page
            lang = await evaluate("document.documentElement.lang")
            check("Strike: does not inherit Box Office's language (its own settings)", lang != "de", lang)
            await pick_language("ru")
        keys = json.loads(await evaluate(keys_js) or "[]")
        new = [k for k in keys if k not in bo_keys]
        if not frvr:
            check("Strike: stores its settings under strike. (strike.krk_lang = ru), nothing else new",
                  await evaluate("localStorage.getItem('strike.krk_lang')") == "ru" and bool(new)
                  and all(k.startswith("strike.") for k in new), new)
        else:
            check("Strike: anything it stores is under strike.", all(k.startswith("strike.") for k in new), new)
        now = {k: await evaluate("localStorage.getItem(%s)" % json.dumps(k)) for k in bo_keys}
        check("Strike: Box Office's keys untouched (krk_lang still de)", now == bo_settings,
              {k: (bo_settings[k], now[k]) for k in bo_keys if now[k] != bo_settings[k]})
        if not frvr:
            check_downloads("Strike", await downloads(), True)

        # 4. Strike offline: the client rewrites the address (servers_ui.c page_write_offline)
        await open_page("/strike/?offline=1&map=burg")
        stayed = await until("location.pathname === '/strike/' && /offline=1/.test(location.search) && /map=/.test(location.search)", 60)
        check("Strike offline: the address the client writes stays in /strike/", stayed,
              await evaluate("location.pathname + location.search"))

        # 2b. Box Office again: still Box Office, the address it writes stays at the root
        line = await open_page("/?offline=1&map=burg")
        check("Box Office after Strike: still Krunker Box Office", bool(line) and line.startswith("Krunker Box Office"), line)
        lang = await evaluate("document.documentElement.lang")
        check("Box Office after Strike: still in its own language (de)", lang == "de", lang)
        stayed = await until("location.pathname === '/' && /offline=1/.test(location.search)", 60)
        check("Box Office offline: the address stays at /", stayed, await evaluate("location.pathname + location.search"))
        task.cancel()


def main():
    if not os.path.isfile(os.path.join(DIST, "client.wasm")):
        print(f"SKIP  no web build in {DIST}: run make web")
        return 77
    check_files()
    chrome = shutil.which("chromium-browser") or shutil.which("chromium") or shutil.which("google-chrome")
    if not chrome:
        print("SKIP  browser checks: needs chromium or google-chrome")
        return 77
    http_port, cdp_port = kn_proto.free_ports(2)
    server = http.server.ThreadingHTTPServer(("127.0.0.1", http_port), functools.partial(Handler, directory=DIST))
    threading.Thread(target=server.serve_forever, daemon=True).start()
    proc = subprocess.Popen([chrome, "--headless=new", "--no-sandbox", "--use-gl=angle", "--use-angle=swiftshader",
                             "--enable-unsafe-swiftshader", f"--remote-debugging-port={cdp_port}",
                             f"--user-data-dir={os.path.join(WORK, 'chrome')}", "--window-size=1280,720", "about:blank"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
    try:
        asyncio.run(asyncio.wait_for(browser_checks(f"http://127.0.0.1:{http_port}", cdp_port), 900))
    except Exception as e:  # noqa: BLE001 - reported as a failed check
        check("browser session", False, repr(e))
    finally:
        if proc.poll() is None:
            os.killpg(proc.pid, signal.SIGKILL)
            proc.wait()
        server.shutdown()
        shutil.rmtree(WORK, ignore_errors=True)
    print(f"{sum(results)}/{len(results)} passed")
    return 0 if results and all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
