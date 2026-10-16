#!/usr/bin/env python3
"""Installable web apps (PWA) of the web build: Add to Home Screen on iOS / Android.

iPhone / iPad players cannot install the native client and an app needs Apple signing, so
the Download window (client/src/ui/download_ui.c) tells them to add krunk.io to the Home
Screen: Safari then opens it fullscreen, without browser bars, like an app.

The one web build carries two apps, each with its own manifest (id, start_url and scope), name,
icons and theme colour, so a phone can install both side by side (platform/README.md):

  Krunker Box Office  /          manifest.webmanifest, img/pwa/         (this directory)
  Krunker Strike      /strike/   strike/manifest.webmanifest, strike/img/pwa/   (strike/)

The same client.wasm runs both: the Strike page carries <meta name="kn-flavor" content="strike">
and the client starts in the Strike flavor from it (client/src/main.c, no flag needed).

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

- assembles the Krunker Strike app in <dist>/strike/: strike/manifest.webmanifest, strike/icons/*
  as <dist>/strike/img/pwa/ (tools/branding/strike_icons.py: the Strike logo on the Strike purple)
  and the same <head> blocks under the Strike name, gold theme colour and kn-flavor meta in
  <dist>/strike/index.html. That page is ui/strike/index.html when the tree has one (FRVR's Strike
  page, copied with ui/); otherwise it is generated here from the game page with
  <base href="../">, so every relative URL (css/, img/, js/, the client files, assets/,
  kn-config.json) resolves exactly as on the root page while the address stays /strike/.

Idempotent: the blocks between the markers are replaced on every run.

usage: pwa.py <dist dir>
"""
import html
import json
import os
import re
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SITE = os.environ.get("KN_SITE_URL", "https://krunk.io/").rstrip("/") + "/"

# the markers keep their old text: a page written by an older pwa.py is still recognised
BEGIN = "<!-- KrunkNative PWA (platform/web/pwa.py) -->"
END = "<!-- /KrunkNative PWA -->"
EXISTING_RE = re.compile(re.escape(BEGIN) + r".*?" + re.escape(END) + r"\n?", re.S)
TITLE_RE = re.compile(r'<meta\s+name="apple-mobile-web-app-title"[^>]*>', re.I)
THEME_RE = re.compile(r'<meta\s+name="theme-color"[^>]*>', re.I)
MANIFEST_RE = re.compile(r'<link\s+rel="manifest"[^>]*>\s*', re.I)
FLAVOR_RE = re.compile(r'<meta\s+name="kn-flavor"[^>]*>\s*', re.I)
BASE_RE = re.compile(r'<base\s[^>]*>\s*', re.I)
HEAD_RE = re.compile(r"<head[^>]*>", re.I)


def pwa_block(prefix):
    """The <head> links of an app whose PWA files sit at <prefix>manifest.webmanifest / img/pwa/."""
    return BEGIN + """
<link rel="manifest" href="{p}manifest.webmanifest">
<link rel="apple-touch-icon" sizes="180x180" href="{p}img/pwa/apple-touch-icon.png">
<link rel="icon" type="image/png" sizes="32x32" href="{p}img/pwa/icon-32.png">
<meta name="mobile-web-app-capable" content="yes">
""".format(p=prefix) + END + "\n"


# One installable app: its name, the page's metadata and where its files live in <dist>.
class App:
    def __init__(self, name, description, keywords, og_image, og_type, og_alt, path, src, theme=None, flavor=None):
        self.name = name
        self.description = description
        self.keywords = keywords
        self.og_image = og_image  # relative to SITE
        self.og_type = og_type
        self.og_alt = og_alt
        self.path = path      # "" (root) or "strike/": the app's directory in <dist> and on the site
        self.src = src        # this script's directory holding its manifest.webmanifest and icons/
        self.theme = theme    # <meta name="theme-color">; None keeps the page's
        self.flavor = flavor  # <meta name="kn-flavor">: the client's flavor on this page


BOX_OFFICE = App(
    "Krunker Box Office",
    "Play Krunker Box Office: the Krunker FPS rebuilt as a native engine. Free fast-paced multiplayer "
    "in your browser or as a desktop client. No download required.",
    "krunker,krunker box office,free fps game,online multiplayer,io game,browser fps,"
    "first person shooter,competitive gaming,no download games",
    "img/pwa/og_1200x630.jpg", "image/jpeg",
    "Krunker characters mid-fight on a desert map beside the Krunker logo",
    "", HERE)
STRIKE = App(
    "Krunker Strike",
    "Play Krunker Strike: the competitive Krunker FPS, free in your browser and as a phone app. "
    "Fast rounds, touch controls, no download required.",
    "krunker,krunker strike,free fps game,online multiplayer,mobile fps,browser fps,"
    "first person shooter,competitive gaming,no download games",
    "strike/img/pwa/og_1200x630.png", "image/png",
    "The Krunker Strike logo on purple",
    "strike/", os.path.join(HERE, "strike"), theme="#f9c80e", flavor="strike")

# page: (title, og title, path); the game page is the only one with the JSON-LD
PAGES = {
    "index.html": (BOX_OFFICE.name + " - Free Online Multiplayer FPS Game | Play Now", BOX_OFFICE.name + " - Free Online Multiplayer FPS Game", ""),
    "social.html": (BOX_OFFICE.name + " Hub", BOX_OFFICE.name + " Hub: profiles, clans, leaderboards and the market", "social.html"),
    "editor.html": (BOX_OFFICE.name + " Editor", BOX_OFFICE.name + " Editor: build Krunker maps in your browser", "editor.html"),
    "viewer.html": (BOX_OFFICE.name + " Viewer", BOX_OFFICE.name + " Viewer: skins, models and textures", "viewer.html"),
}
STRIKE_PAGE = (STRIKE.name + " - Free Online Multiplayer FPS Game | Play Now", STRIKE.name + " - Free Online Multiplayer FPS Game", "strike/")
# the first line of a Strike page this script generated (ui/ has no strike/index.html yet)
STRIKE_MARK = "<!-- Krunker Strike page: generated by platform/web/pwa.py from index.html -->"

META_BEGIN = "<!-- Krunker Box Office meta (platform/web/pwa.py) -->"
META_END = "<!-- /Krunker Box Office meta -->"
META_RE = re.compile(re.escape(META_BEGIN) + r".*?" + re.escape(META_END) + r"\n?", re.S)
# the imported page's own copies, replaced by the block
OLD_META_RE = re.compile(r'<meta\s+(?:name="(?:description|keywords|twitter:[^"]*)"|property="og:[^"]*")[^>]*>\s*'
                         r'|<link\s+rel="(?:canonical|alternate)"[^>]*>\s*', re.I)
PAGE_TITLE_RE = re.compile(r"<title>.*?</title>", re.S | re.I)


def inject(page, app=BOX_OFFICE, prefix=""):
    """page with exactly one PWA block, right before </head>, and the app's home screen title,
    theme colour and flavor."""
    page = EXISTING_RE.sub("", page)
    title = '<meta name="apple-mobile-web-app-title" content="%s">' % html.escape(app.name, quote=True)
    page = TITLE_RE.sub(lambda _m: title, page) if TITLE_RE.search(page) else page.replace("</head>", title + "\n</head>", 1)
    if app.theme:
        theme = '<meta name="theme-color" content="%s">' % app.theme
        page = THEME_RE.sub(lambda _m: theme, page) if THEME_RE.search(page) else page.replace("</head>", theme + "\n</head>", 1)
    if app is not BOX_OFFICE:
        page = MANIFEST_RE.sub("", page)  # FRVR's own manifest.json link ("Krunker FRVR")
    if app.flavor:
        page = FLAVOR_RE.sub("", page)
        page = page.replace("</head>", '<meta name="kn-flavor" content="%s">\n</head>' % app.flavor, 1)
    at = page.find("</head>")
    if at < 0:
        sys.exit("pwa.py: index.html has no </head>")
    return page[:at] + pwa_block(prefix) + page[at:]


def meta_block(name, app=BOX_OFFICE, page_info=None):
    title, og_title, path = page_info or PAGES[name]
    url = SITE + path
    image = SITE + app.og_image
    a = lambda v: html.escape(v, quote=True)
    lines = [
        '<meta name="description" content="%s">' % a(app.description),
        '<meta name="keywords" content="%s">' % a(app.keywords),
        '<link rel="canonical" href="%s">' % a(url),
        '<link rel="alternate" hreflang="en" href="%s">' % a(url),
        '<link rel="alternate" hreflang="x-default" href="%s">' % a(url),
        '<meta property="og:site_name" content="%s">' % a(app.name),
        '<meta property="og:title" content="%s">' % a(og_title),
        '<meta property="og:description" content="%s">' % a(app.description),
        '<meta property="og:type" content="website">',
        '<meta property="og:url" content="%s">' % a(url),
        '<meta property="og:locale" content="en">',
        '<meta property="og:image" content="%s">' % a(image),
        '<meta property="og:image:type" content="%s">' % app.og_type,
        '<meta property="og:image:width" content="1200">',
        '<meta property="og:image:height" content="630">',
        '<meta property="og:image:alt" content="%s">' % a(app.og_alt),
        '<meta name="twitter:card" content="summary_large_image">',
        '<meta name="twitter:title" content="%s">' % a(og_title),
        '<meta name="twitter:description" content="%s">' % a(app.description),
        '<meta name="twitter:image" content="%s">' % a(image),
        '<meta name="twitter:image:alt" content="%s">' % a(app.og_alt),
    ]
    if name == "index.html":
        ld = {"@context": "https://schema.org", "@type": "VideoGame", "name": app.name,
              "description": "Free online multiplayer FPS game", "url": url, "image": image,
              "genre": ["Action", "Shooter", "Multiplayer"],
              "gamePlatform": ["Web Browser", "PC", "Mobile"],
              "operatingSystem": ["Windows", "macOS", "Linux", "iOS", "Android"],
              "offers": {"@type": "Offer", "price": "0", "priceCurrency": "USD"}}
        lines.append('<script type="application/ld+json">%s</script>' % json.dumps(ld).replace("</", "<\\/"))
    return META_BEGIN + "\n" + "\n".join(lines) + "\n" + META_END + "\n"


def inject_meta(page, name, app=BOX_OFFICE, page_info=None):
    """page with its title, one metadata block after <title> and none of the imported copies."""
    page_info = page_info or PAGES[name]
    page = OLD_META_RE.sub("", META_RE.sub("", page))
    title = "<title>%s</title>" % html.escape(page_info[0])
    block = meta_block(name, app, page_info)
    m = PAGE_TITLE_RE.search(page)
    if m:
        page = page[:m.start()] + title + "\n" + block + page[m.end():].lstrip("\n")
    else:
        at = page.find("</head>")
        if at < 0:
            sys.exit("pwa.py: %s has no </head>" % name)
        page = page[:at] + title + "\n" + block + page[at:]
    return page


def strike_page_from(game_page):
    """The interim Strike page: the game page one directory down. <base href="../"> keeps every
    relative URL on the root (the page, the client, assets, kn-config.json); the address stays
    /strike/, which keeps the page's own history writes (servers_ui.c) and the manifest scope there."""
    page = EXISTING_RE.sub("", META_RE.sub("", game_page))
    page = BASE_RE.sub("", page)
    m = HEAD_RE.search(page)
    if not m:
        sys.exit("pwa.py: index.html has no <head>")
    return page[:m.end()] + "\n" + STRIKE_MARK + '\n<base href="../">' + page[m.end():]


def copy_if_different(src, dst):
    if os.path.exists(dst):
        with open(src, "rb") as a, open(dst, "rb") as b:
            if a.read() == b.read():
                return
    shutil.copyfile(src, dst)


def install_app_files(dist, app):
    """<dist>/<app.path>manifest.webmanifest and <dist>/<app.path>img/pwa/<icons>."""
    root = os.path.join(dist, app.path)
    os.makedirs(os.path.join(root, "img", "pwa"), exist_ok=True)
    copy_if_different(os.path.join(app.src, "manifest.webmanifest"), os.path.join(root, "manifest.webmanifest"))
    for name in sorted(os.listdir(os.path.join(app.src, "icons"))):
        copy_if_different(os.path.join(app.src, "icons", name), os.path.join(root, "img", "pwa", name))


def write_if_changed(path, old, new):
    if new != old:
        with open(path, "w", encoding="utf-8") as f:
            f.write(new)


def build_strike(dist, game_page):
    """The Krunker Strike app in <dist>/strike/ (game_page: the root index.html as ui/ has it)."""
    install_app_files(dist, STRIKE)
    path = os.path.join(dist, STRIKE.path, "index.html")
    old = None
    if os.path.isfile(path):
        with open(path, encoding="utf-8") as f:
            old = f.read()
    # ui/strike/index.html (copied with ui/) is the real Strike page; else (re)generate ours
    page = old if old is not None and STRIKE_MARK not in old else strike_page_from(game_page)
    # under <base href="../"> the app's files are strike/...; the real page sits in strike/ itself
    prefix = STRIKE.path if STRIKE_MARK in page else ""
    new = inject(inject_meta(page, "index.html", STRIKE, STRIKE_PAGE), STRIKE, prefix)
    write_if_changed(path, old, new)


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: pwa.py <dist dir>")
    dist = sys.argv[1]
    index = os.path.join(dist, "index.html")
    if not os.path.isfile(index):
        sys.exit("pwa.py: %s missing" % index)
    install_app_files(dist, BOX_OFFICE)
    game_page = None
    for name in PAGES:
        path = os.path.join(dist, name)
        if not os.path.isfile(path):
            continue
        with open(path, encoding="utf-8") as f:
            page = f.read()
        if name == "index.html":
            game_page = page
        new = inject_meta(page, name)
        if name == "index.html":
            new = inject(new)
        write_if_changed(path, page, new)
    build_strike(dist, game_page)


if __name__ == "__main__":
    main()
