#!/usr/bin/env python3
"""References for the v10 Social hub uidiff scenarios (tools/uidiff/scenarios/v10/social-*.json).

  python3 -I tools/uidiff/v10_social_ref.py <client_bin> [scenario names...]

v10's own Social hub is a logged-in Svelte popup: there is no capture of it, and its JS is never run
here (PARITY-BRIEF). The markup v10_social.c emits is rebuilt from the bundle's templates
(doc/v10/social.md), so the reference is that markup rendered by headless Chromium with v10's CSS:
the static v10 page (ui/v10/index.html) with #uiBase scaled like the client and the hub / view stack
the client dumped (KRUNKNATIVE_V10_SOCIAL_DUMP). Writes doc/ref/krunker-v10.0.2/<name>.png and
tools/uidiff/regions/v10/<name>.json (the popup area); `make uidiff SCENARIOS=v10/<name>` compares.
"""
import json
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CLIENT = os.path.abspath(sys.argv[1])
NAMES = sys.argv[2:] or sorted(f[:-5] for f in os.listdir(os.path.join(ROOT, "tools/uidiff/scenarios/v10")) if f.startswith("social-"))
REF_DIR = os.path.join(ROOT, "doc/ref/krunker-v10.0.2")
REG_DIR = os.path.join(ROOT, "tools/uidiff/regions/v10")


def dump(name, work):
    out = os.path.join(work, name + ".json")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": os.path.join(work, "cfg"),
                "KRUNKNATIVE_UI_DIR": "ui/v10", "KRUNKNATIVE_V10_SOCIAL_DUMP": out})
    subprocess.run([CLIENT, "--render-scenario", "tools/uidiff/scenarios/v10/%s.json" % name, os.path.join(work, name + ".png")],
                   cwd=ROOT, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=120, check=True)
    with open(out) as f:
        return json.load(f)


def page(d):
    with open(os.path.join(ROOT, "ui/v10/index.html")) as f:
        html = f.read()
    base = ";".join("%s:%s" % kv for kv in d["uiBase"].items())
    body = ";".join("%s:%s" % kv for kv in d["body"].items())
    style = ("<style>#uiBase{%s;transform-origin:0 0}#popupHolder{display:block!important}"
             "#genericPop{display:block!important}#windowHolder,#consentBlock,#loadingBg,#loadingBg1,#aHider{display:none!important}"
             "body{%s}</style>" % (base, body))
    html = html.replace("<head>", '<head><base href="file://%s/ui/v10/">' % ROOT, 1)  # before the stylesheets
    html = html.replace("</head>", style + "</head>", 1)
    html = html.replace('<div id="genericPop"></div>', '<div id="genericPop" class="socialModal">%s</div>' % d["hub"], 1)
    if d["views"]:
        html = html.replace('<div id="app"></div>', '<div id="app"><div class="view-stack svelte-sdh4ve"><div class="view-stack-overlay svelte-sdh4ve">'
                            '<div class="view-wrapper svelte-sdh4ve" style="display: block">%s</div></div></div></div>' % d["views"], 1)
    return html.replace('<script src="js/kn-shim.js"></script>', "").replace('<script src="js/loader.js"></script>', "")


def main():
    from playwright.sync_api import sync_playwright
    os.makedirs(REF_DIR, exist_ok=True)
    os.makedirs(REG_DIR, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="v10-social-ref.", dir=os.path.expanduser("~/.cache")) as work, sync_playwright() as p:
        browser = p.chromium.launch(args=["--hide-scrollbars", "--force-device-scale-factor=1"])
        try:
            for name in NAMES:
                d = dump(name, work)
                path = os.path.join(work, name + ".html")
                with open(path, "w") as f:
                    f.write(page(d))
                tab = browser.new_page(viewport={"width": 1280, "height": 720})
                tab.goto("file://" + path)
                tab.wait_for_timeout(800)
                tab.screenshot(path=os.path.join(REF_DIR, name + ".png"))
                box = tab.evaluate("""() => { const e = document.querySelector('.view-stack .view-wrapper > *') ||
                                              document.getElementById('genericPop');
                                              const r = (e.querySelector('[role=dialog]') || e).getBoundingClientRect();
                                              return [r.x, r.y, r.width, r.height].map(Math.round); }""")
                tab.close()
                # glyph rasterisation (FreeType vs Skia) is most of the difference: the stat grid of the
                # profile is dense text, so it gets more room
                pct = 9.0 if name == "social-profile" else 6.0
                with open(os.path.join(REG_DIR, name + ".json"), "w") as f:
                    json.dump({"scenario": "v10/" + name, "defaults": {"fuzz": 12, "max_pct": pct},
                               "regions": [{"name": "social", "rect": box, "kind": "text", "fuzz": 12, "max_pct": pct,
                                            "note": "the v10 Social hub popup / view (v10_social.c markup, Chromium + v10 CSS)"}]},
                              f, indent=2)
                    f.write("\n")
                print("%s -> %s %s" % (name, os.path.join(REF_DIR, name + ".png"), box))
        finally:
            browser.close()


main()
