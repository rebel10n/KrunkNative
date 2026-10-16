#!/usr/bin/env python3
"""A browser reference for a transcribed Krunker Strike screen (one FRVR has no sandbox capture of):
the DOM KrunkNative renders for a scenario, drawn by Chrome with FRVR 1.37.2's own stylesheets only.

  python3 -I tools/strike/static_ref.py BUILD_DIR SCENARIO.json OUT.png [--keep-html OUT.html.gz]

1. client_bin --render-scenario SCENARIO (headless) with KRUNKNATIVE_SCENARIO_HTML: the captured frame's
   document (the page plus the transcribed screen's markup, filled by client/src/ui/strike_*.c)
2. that document without KrunkNative's own parts: no <script> (FRVR's bundle never runs, nor does
   KrunkNative's shim), no css/kn-strike-overrides.css (the native engine's gap fixes), <base> at ui/strike/
   so FRVR's index-CJ_IUjn5.css, icons.css, fonts and images load from the tree
3. Chrome headless screenshots it at the scenario's size; the page carries a Content-Security-Policy of
   script-src 'none' (on top of having no <script> left), so nothing in it can run

So the reference checks the native engine's rendering of FRVR's markup against a browser's rendering of
the same markup with FRVR's CSS; it says nothing about whether the transcription matches FRVR's own
runtime (no capture of these views exists). The canvases (the class preview, a wheel) are empty in it.
"""
import gzip
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CHROMES = ("google-chrome", "chromium-browser", "chromium")


def chrome():
    for c in CHROMES:
        p = shutil.which(c)
        if p:
            return p
    return None


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    keep = None
    if "--keep-html" in sys.argv:
        keep = sys.argv[sys.argv.index("--keep-html") + 1]
        args = [a for a in args if a != keep]
    if len(args) != 3:
        print(__doc__)
        return 2
    build, scenario, out = (os.path.abspath(a) for a in args)
    browser = chrome()
    if not browser:
        print("static_ref: no Chrome / Chromium: skip")
        return 77
    with open(scenario) as f:
        sc = json.load(f)
    w, h = sc.get("size", [1280, 720])
    work = tempfile.mkdtemp(prefix="kn-strike-ref-")
    try:
        doc_path = os.path.join(work, "doc.html")
        env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
        env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": os.path.join(work, "cfg"),
                    "KRUNKNATIVE_SCENARIO_HTML": doc_path})
        r = subprocess.run([os.path.join(build, "client_bin"), "--render-scenario", scenario, os.path.join(work, "native.png")],
                           cwd=ROOT, env=env, stdin=subprocess.DEVNULL, capture_output=True, timeout=180)
        if r.returncode != 0 or not os.path.exists(doc_path):
            sys.stderr.write(r.stdout.decode(errors="replace")[-2000:] + r.stderr.decode(errors="replace")[-2000:])
            print("static_ref: the scenario did not render")
            return 1
        with open(doc_path, encoding="utf-8", errors="replace") as f:
            doc = f.read()
        doc = re.sub(r"<script\b.*?</script>", "", doc, flags=re.S | re.I)
        doc = re.sub(r"<link[^>]*kn-strike-overrides\.css[^>]*>", "", doc, flags=re.I)
        base = "file://" + os.path.join(ROOT, "ui", "strike") + "/"
        # the asset host's paths (store_skin_preview: "/assets/textures/...") from the assets tree
        assets = os.environ.get("KRUNKNATIVE_ASSETS_DIR") or os.path.join(ROOT, "assets")
        doc = doc.replace('src="/assets/', 'src="file://%s/' % assets.rstrip("/"))
        csp = '<meta http-equiv="Content-Security-Policy" content="script-src \'none\'">'
        doc = re.sub(r"<head>", '<head>%s<base href="%s">' % (csp, base), doc, count=1, flags=re.I)
        page = os.path.join(work, "ref.html")
        with open(page, "w", encoding="utf-8") as f:
            f.write(doc)
        if keep:
            with gzip.open(keep, "wt", encoding="utf-8") as f:
                f.write(doc)
        shot = os.path.join(work, "ref.png")
        cmd = [browser, "--headless=new", "--disable-gpu", "--hide-scrollbars", "--no-first-run", "--no-default-browser-check",
               "--user-data-dir=" + os.path.join(work, "chrome"),
               "--allow-file-access-from-files", "--force-device-scale-factor=1", "--window-size=%d,%d" % (w, h),
               "--virtual-time-budget=3000", "--screenshot=" + shot, "file://" + page]
        r = subprocess.run(cmd, stdin=subprocess.DEVNULL, capture_output=True, timeout=120)
        if not os.path.exists(shot):
            sys.stderr.write(r.stderr.decode(errors="replace")[-2000:])
            print("static_ref: Chrome wrote no screenshot")
            return 1
        shutil.copyfile(shot, out)
        print("static_ref: %s -> %s (%dx%d)" % (os.path.relpath(scenario, ROOT), out, w, h))
        return 0
    finally:
        shutil.rmtree(work, ignore_errors=True)


sys.exit(main())
