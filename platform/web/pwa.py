#!/usr/bin/env python3
"""Installable web app (PWA) bits of the web build: Add to Home Screen on iOS / Android.

iPhone / iPad players cannot install the native client and an app needs Apple signing, so
the Download window (client/src/ui/download_ui.c) tells them to add krunk.io to the Home
Screen: Safari then opens it fullscreen, without browser bars, like an app.

The ui/ pages stay as imported (tools/ui-import strips v7's own manifest links: the native
HTML engine has no use for them); this runs on the assembled web build (CMake's web_dist
target, after ui/ is copied) and

- copies manifest.webmanifest to <dist>/ and icons/* to <dist>/img/pwa/ (krunker.io's live PWA
  icons, pinned and written by tools/branding/fetch_branding.py);
- puts the manifest link, the apple-touch-icon, the 32 px icon (as krunker.io links it, next to
  the page's 16 px favicon) and mobile-web-app-capable into <head> of <dist>/index.html (the game
  page, the manifest's start_url) and names the home screen app Krunker Box Office (v7's
  apple-mobile-web-app-title is "Krunker FRVR"). v7's other Apple metas (apple-mobile-web-app-
  capable, black-translucent status bar, theme-color #FFD600) stay.

Idempotent: the block between the markers is replaced on every run.

usage: pwa.py <dist dir>
"""
import os
import re
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
# the markers keep their old text: a page written by an older pwa.py is still recognised
BEGIN = "<!-- KrunkNative PWA (platform/web/pwa.py) -->"
END = "<!-- /KrunkNative PWA -->"
BLOCK = BEGIN + """
<link rel="manifest" href="manifest.webmanifest">
<link rel="apple-touch-icon" sizes="180x180" href="img/pwa/apple-touch-icon.png">
<link rel="icon" type="image/png" sizes="32x32" href="img/pwa/icon-32.png">
<meta name="mobile-web-app-capable" content="yes">
""" + END + "\n"
EXISTING_RE = re.compile(re.escape(BEGIN) + r".*?" + re.escape(END) + r"\n?", re.S)
TITLE_RE = re.compile(r'<meta\s+name="apple-mobile-web-app-title"[^>]*>', re.I)
TITLE = '<meta name="apple-mobile-web-app-title" content="Krunker Box Office">'


def inject(page):
    """page with exactly one PWA block, right before </head>, and the Krunker Box Office title."""
    page = EXISTING_RE.sub("", page)
    page = TITLE_RE.sub(TITLE, page) if TITLE_RE.search(page) else page.replace("</head>", TITLE + "\n</head>", 1)
    at = page.find("</head>")
    if at < 0:
        sys.exit("pwa.py: index.html has no </head>")
    return page[:at] + BLOCK + page[at:]


def copy_if_different(src, dst):
    if os.path.exists(dst):
        with open(src, "rb") as a, open(dst, "rb") as b:
            if a.read() == b.read():
                return
    shutil.copyfile(src, dst)


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: pwa.py <dist dir>")
    dist = sys.argv[1]
    index = os.path.join(dist, "index.html")
    if not os.path.isfile(index):
        sys.exit("pwa.py: %s missing" % index)
    copy_if_different(os.path.join(HERE, "manifest.webmanifest"), os.path.join(dist, "manifest.webmanifest"))
    icons = os.path.join(dist, "img", "pwa")
    os.makedirs(icons, exist_ok=True)
    for name in sorted(os.listdir(os.path.join(HERE, "icons"))):
        copy_if_different(os.path.join(HERE, "icons", name), os.path.join(icons, name))
    with open(index, encoding="utf-8") as f:
        page = f.read()
    new = inject(page)
    if new != page:
        with open(index, "w", encoding="utf-8") as f:
            f.write(new)


if __name__ == "__main__":
    main()
