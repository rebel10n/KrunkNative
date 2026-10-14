#!/usr/bin/env python3
"""Reference for the v10 login modal's Register card (tools/uidiff/scenarios/login-register.json).

  python3 -E -P tools/uidiff/v10_login_ref.py <web dist dir>      (needs playwright + its Chromium)

The offline v10 sandbox capture (doc/ref/krunker-v10.0.2/ranked-guest.png) only has the Login card,
and v10's JS is never run here. v10_login.c emits the Register card from v10's template (_39035 with
defaultToRegister, the sign-up bonuses _39197), so the reference is that markup laid out by headless
Chromium with v10's CSS: the web build (make web, same C UI, the browser's DOM and CSS) opened
offline at 1280x720, the 3D canvas hidden over the scenario's flat background, then
loginOrRegister(true). Writes doc/ref/krunker-v10.0.2/login-register.png and prints the rects of
the popup and its buttons (tools/uidiff/regions/login-register.json takes them).
"""
import json
import os
import socket
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DIST = os.path.abspath(sys.argv[1])
OUT = os.path.join(ROOT, "doc/ref/krunker-v10.0.2/login-register.png")
RECTS = """() => { const r = s => { const e = document.querySelector(s); if (!e) return null;
                     const b = e.getBoundingClientRect(); return [b.x, b.y, b.width, b.height].map(Math.round); };
                   return {popup: r('#knV10Login .popup'), submit: r('#knV10LoginSubmit'), switch: r('#knV10AuthSwitch'),
                           bonuses: r('#knV10Login .sign-up-bonuses')}; }"""


def main():
    from playwright.sync_api import sync_playwright
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    srv = subprocess.Popen([sys.executable, "-m", "http.server", str(port), "--bind", "127.0.0.1", "--directory", DIST],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        with sync_playwright() as p:
            browser = p.chromium.launch(args=["--hide-scrollbars", "--force-device-scale-factor=1"])
            try:
                tab = browser.new_page(viewport={"width": 1280, "height": 720})
                tab.goto("http://127.0.0.1:%d/index.html?offline=1" % port)
                tab.wait_for_function("window.__krunknative_wasm_ready === true", timeout=120000)
                tab.add_style_tag(content="#canvas{visibility:hidden!important}html,body{background:#222222!important}")
                tab.wait_for_timeout(4000)
                tab.evaluate("knCall('loginOrRegister', [true])")
                tab.wait_for_timeout(1000)
                tab.screenshot(path=OUT)
                print(json.dumps(tab.evaluate(RECTS)))
            finally:
                browser.close()
    finally:
        srv.terminate()
        srv.wait()
    print("-> " + OUT)


main()
