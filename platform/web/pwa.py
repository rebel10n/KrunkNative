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

- gives every page krunker.io's link-preview metadata under our name (the live krunker.io <head>,
  checked 2026-10-13): title, description and keywords, canonical + hreflang, Open Graph and
  Twitter card (krunker.io's og_1200x630.jpg, pinned by tools/branding), and on the game page a
  schema.org VideoGame JSON-LD. krunker.io's Organization block (FRVR) and its aggregateRating are
  left out: they describe FRVR's game, not this one. Absolute URLs use KN_SITE_URL (default
  https://krunk.io/, the GitHub Pages custom domain); embeds need absolute og:image URLs.

Idempotent: the block between the markers is replaced on every run.

usage: pwa.py <dist dir>
"""
import html
import json
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


SITE = os.environ.get("KN_SITE_URL", "https://krunk.io/").rstrip("/") + "/"
NAME = "Krunker Box Office"
DESCRIPTION = ("Play Krunker Box Office: the Krunker FPS rebuilt as a native engine. Free fast-paced multiplayer "
               "in your browser or as a desktop client. No download required.")
KEYWORDS = ("krunker,krunker box office,free fps game,online multiplayer,io game,browser fps,"
            "first person shooter,competitive gaming,no download games")
OG_IMAGE = "img/pwa/og_1200x630.jpg"
OG_ALT = "Krunker characters mid-fight on a desert map beside the Krunker logo"
# page: (title, og title, path); the game page is the only one with the JSON-LD
PAGES = {
    "index.html": (NAME + " - Free Online Multiplayer FPS Game | Play Now", NAME + " - Free Online Multiplayer FPS Game", ""),
    "social.html": (NAME + " Hub", NAME + " Hub: profiles, clans, leaderboards and the market", "social.html"),
    "editor.html": (NAME + " Editor", NAME + " Editor: build Krunker maps in your browser", "editor.html"),
    "viewer.html": (NAME + " Viewer", NAME + " Viewer: skins, models and textures", "viewer.html"),
}
META_BEGIN = "<!-- Krunker Box Office meta (platform/web/pwa.py) -->"
META_END = "<!-- /Krunker Box Office meta -->"
META_RE = re.compile(re.escape(META_BEGIN) + r".*?" + re.escape(META_END) + r"\n?", re.S)
# the imported page's own copies, replaced by the block
OLD_META_RE = re.compile(r'<meta\s+(?:name="(?:description|keywords|twitter:[^"]*)"|property="og:[^"]*")[^>]*>\s*'
                         r'|<link\s+rel="(?:canonical|alternate)"[^>]*>\s*', re.I)
PAGE_TITLE_RE = re.compile(r"<title>.*?</title>", re.S | re.I)


def meta_block(name):
    title, og_title, path = PAGES[name]
    url = SITE + path
    image = SITE + OG_IMAGE
    a = lambda v: html.escape(v, quote=True)
    lines = [
        '<meta name="description" content="%s">' % a(DESCRIPTION),
        '<meta name="keywords" content="%s">' % a(KEYWORDS),
        '<link rel="canonical" href="%s">' % a(url),
        '<link rel="alternate" hreflang="en" href="%s">' % a(url),
        '<link rel="alternate" hreflang="x-default" href="%s">' % a(url),
        '<meta property="og:site_name" content="%s">' % a(NAME),
        '<meta property="og:title" content="%s">' % a(og_title),
        '<meta property="og:description" content="%s">' % a(DESCRIPTION),
        '<meta property="og:type" content="website">',
        '<meta property="og:url" content="%s">' % a(url),
        '<meta property="og:locale" content="en">',
        '<meta property="og:image" content="%s">' % a(image),
        '<meta property="og:image:type" content="image/jpeg">',
        '<meta property="og:image:width" content="1200">',
        '<meta property="og:image:height" content="630">',
        '<meta property="og:image:alt" content="%s">' % a(OG_ALT),
        '<meta name="twitter:card" content="summary_large_image">',
        '<meta name="twitter:title" content="%s">' % a(og_title),
        '<meta name="twitter:description" content="%s">' % a(DESCRIPTION),
        '<meta name="twitter:image" content="%s">' % a(image),
        '<meta name="twitter:image:alt" content="%s">' % a(OG_ALT),
    ]
    if name == "index.html":
        ld = {"@context": "https://schema.org", "@type": "VideoGame", "name": NAME,
              "description": "Free online multiplayer FPS game", "url": SITE, "image": image,
              "genre": ["Action", "Shooter", "Multiplayer"],
              "gamePlatform": ["Web Browser", "PC", "Mobile"],
              "operatingSystem": ["Windows", "macOS", "Linux", "iOS", "Android"],
              "offers": {"@type": "Offer", "price": "0", "priceCurrency": "USD"}}
        lines.append('<script type="application/ld+json">%s</script>' % json.dumps(ld).replace("</", "<\\/"))
    return META_BEGIN + "\n" + "\n".join(lines) + "\n" + META_END + "\n"


def inject_meta(page, name):
    """page with its title, one metadata block after <title> and none of the imported copies."""
    page = OLD_META_RE.sub("", META_RE.sub("", page))
    title = "<title>%s</title>" % html.escape(PAGES[name][0])
    m = PAGE_TITLE_RE.search(page)
    if m:
        page = page[:m.start()] + title + "\n" + meta_block(name) + page[m.end():].lstrip("\n")
    else:
        at = page.find("</head>")
        if at < 0:
            sys.exit("pwa.py: %s has no </head>" % name)
        page = page[:at] + title + "\n" + meta_block(name) + page[at:]
    return page


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
    for name in PAGES:
        path = os.path.join(dist, name)
        if not os.path.isfile(path):
            continue
        with open(path, encoding="utf-8") as f:
            page = f.read()
        new = inject_meta(page, name)
        if name == "index.html":
            new = inject(new)
        if new != page:
            with open(path, "w", encoding="utf-8") as f:
                f.write(new)


if __name__ == "__main__":
    main()
