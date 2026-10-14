"""Krunker v1.5.6 menu page import: ``import.py --ref v156`` (loaded by import.py).

Writes a separate page tree, ``ui/v156/`` (``ui/`` stays v7.1.4, ``ui/v10/`` v10, ``ui/v3/`` v3.1.1:
every Krunker version keeps its own page for the runtime version toggle), from v1.5.6's
``public/`` tree (``/home/user/jails/kru/src/Krunker_v1.5.6/public``). Only the menu page and
what it references are copied: never the game content (models/, textures/ apart from the five
HUD images the page names, sound/, videos/, scares/, scripts/), which is most of the 1.3 GB.

Transformations (the v7 rules of ``index_page.transform_index``, plus what v1.5.6's older page
needs):

- every ``<script>`` (the adsbygoogle push, twitter/google/jQuery/howler/paypal/tween CDN tags,
  oscillator8 / zip / hook.js / game.js, the reCAPTCHA loader), HTML comments (the commented-out
  freestar ad block among them), the ``shortcut icon`` link and the description / keywords /
  Accept-Ranges metas are removed; the ``.g-recaptcha`` placeholder div (``Captcha``, replaced by
  Google's script on the real site) is dropped;
- ``?build=`` / ``&v=`` stripped; site-relative (``/css/main.css``) and ``./`` URLs made
  relative to the page root, in the page and in ``url()``s of the stylesheets;
- external ``<a href>``s neutralised (``href="#" data-ext-href=...``, ``index_page._ext_link``);
- ``<title>`` = the product name, ``<canvas id="canvas">`` (the web build's WebGL target) first in
  ``<body>``, our ``css/krunknative.css`` + ``css/hud.css`` after Krunker's stylesheets and
  ``js/kn-shim.js`` + ``js/loader.js`` before ``</body>``, like ``ui/index.html``;
- the confirm-popup host (``POPUP_HOST``: ``#popupHolder > #popupBack + #confPop``) as ``#uiBase``'s
  first child, styled by ``css/kn-v156.css``: v1.5.6 asked with the browser's ``confirm()``, which
  the native client has not; ``ui_confirm()`` (``client/src/ui/ui_dialog.c``, e.g. the Game version
  restart offer) draws into ``#confPop``. ``css/kn-v156.css`` is generated from v3.1.1's own rules for
  that markup (``V3_POPUP_RULES``, the nearest Krunker version that has it), not written by hand.

Our glue files (``KN_GLUE``) are copied from ``ui/`` (their source of truth): the engine resolves
page URLs inside the page root, so ``ui/v156/`` cannot link ``../css/``. Fonts: GameFont
(``css/fonts/font2.ttf``) as is, Material Icons (``micon.woff2``) plus its ``.ttf`` (import.py; its
ligature table is byte-identical to v7's, so the headers are not regenerated). v1.5.6 has no
window templates (``windows[n].gen()`` builds them at run time and nothing captured them), so only
``windows/versions.txt`` (``docs/versions.txt``, the changelog) is written: the C UI then uses
ui/'s v7.1.4 templates (``client/src/ui/ui_windows.c`` falls back).
"""

import os
import re

import index_page

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
PUBLIC = "/home/user/jails/kru/src/Krunker_v1.5.6/public"
PAGE_DIR = "v156"
STYLESHEETS = ("css/main.css", "css/material_icons.css")
# ours, from ui/ (same list as v10_page.KN_GLUE): linked by the page, added at run time by the C UI
# (devconsole: ui_console.c, ranked-kn: ranked_ui.c) and the web glue
KN_GLUE = ("css/krunknative.css", "css/hud.css", "css/devconsole.css", "css/ranked-kn.css", "js/kn-shim.js", "js/loader.js")
# &v= too: the brief's v1.5.6 strip list (index_page.QUERY_RE only knows ?build= / ?v=)
QUERY_RE = re.compile(r"[?&](?:build|v)=[^\"'\s)&]*")
# v1.5.6 has no popup markup (it used window.confirm): v3.1.1's, for ui_dialog.c's ui_confirm
POPUP_HOST = '<div id="popupHolder"><div id="popupBack" onclick="clearPops()"></div><div id="confPop"></div></div>'
V3_MAIN_CSS = "/home/user/jails/kru/src/Krunker_v3.1.1/public/css/main.css"
V3_POPUP_RULES = ("#popupBack", "#popupHolder", "#confPop", "#confirmBtn", "#confirmBtn:hover", "#declineBtn",
                  "#declineBtn:hover")
KN_V156_CSS = "css/kn-v156.css"
DROP_RE = (
    re.compile(r"<link rel='shortcut icon'[^>]*>\s*"),
    re.compile(r"<meta (?:name=\"(?:description|keywords)\"|http-equiv=\"Accept-Ranges\")[^>]*>\s*"),
    re.compile(r"<div class=\"g-recaptcha\"[^>]*>[^<]*</div>\s*"),  # reCAPTCHA's placeholder (its script is gone)
)


def transform_page(html):
    html = re.sub(r"<script\b[^>]*>.*?</script>\s*", "", html, flags=re.S | re.I)
    html = re.sub(r"<!--.*?-->\s*", "", html, flags=re.S)
    for rx in DROP_RE:
        html = rx.sub("", html)
    html = QUERY_RE.sub("", html)
    # v1.5.6 writes both <link rel="stylesheet" href=...> and <link href=... rel="stylesheet">: one
    # form, so index_page puts our stylesheets after the last of Krunker's
    html = re.sub(r"<link href=\"([^\"]+)\" rel=\"stylesheet\">", r'<link rel="stylesheet" href="\1">', html)
    html = re.sub(r"<link rel=\"stylesheet\" href=\"([^\"]+)\"\s*/>", r'<link rel="stylesheet" href="\1">', html)
    html = re.sub(r"(href|src)=\"\./", r'\1="', html)
    html = index_page.transform_index(html)  # title, root-relative URLs, external links, canvas, our css/js
    html = html.replace('<link rel="stylesheet" href="css/hud.css">\n',
                        '<link rel="stylesheet" href="css/hud.css">\n<link rel="stylesheet" href="%s">\n' % KN_V156_CSS, 1)
    if 'id="popupHolder"' not in html:
        html = html.replace('<div id="uiBase">', '<div id="uiBase">\n\t\t\t' + POPUP_HOST, 1)
    return html


def popup_css(v3_css):
    """css/kn-v156.css: v3.1.1's rules for POPUP_HOST, byte copies in V3_POPUP_RULES order."""
    out = ["/* GENERATED by tools/ui-import/v156_page.py from Krunker v3.1.1's css/main.css: the confirm\n"
           " * popup ui_confirm() draws into (v1.5.6 used the browser's confirm()). DO NOT EDIT. */\n"]
    for sel in V3_POPUP_RULES:
        m = re.search(r"^%s \{[^}]*\}\n" % re.escape(sel), v3_css, re.M)
        if not m:
            raise SystemExit("v156: %s has no `%s {` rule" % (V3_MAIN_CSS, sel))
        out.append(m.group(0))
    return "\n".join(out)


def import_v156(ui_root, write_if_changed, log, public):
    """ui_root = ui/ (the v7 tree, source of our glue files); writes ui/v156/. Returns the page dir."""
    out = os.path.join(ui_root, PAGE_DIR)
    src_html = os.path.join(public, "index.html")
    if not os.path.isfile(src_html):
        raise SystemExit("v156: %s missing" % src_html)
    written = 0
    with open(src_html, encoding="utf-8") as fh:
        written += write_if_changed(os.path.join(out, "index.html"), transform_page(fh.read()))
    for rel in STYLESHEETS:
        with open(os.path.join(public, rel), encoding="utf-8") as fh:
            css = index_page.rewrite_css(fh.read(), os.path.dirname(rel))
        written += write_if_changed(os.path.join(out, rel), QUERY_RE.sub("", css))
    for rel in KN_GLUE:
        with open(os.path.join(ui_root, rel), "rb") as fh:
            written += write_if_changed(os.path.join(out, rel), fh.read())
    with open(V3_MAIN_CSS, encoding="utf-8") as fh:
        written += write_if_changed(os.path.join(out, KN_V156_CSS), popup_css(fh.read()))
    with open(os.path.join(public, "docs", "versions.txt"), "rb") as fh:
        written += write_if_changed(os.path.join(out, "windows", "versions.txt"), fh.read())
    log("v156: %s: index.html, %d stylesheets, %d glue files, %d rewritten" % (
        os.path.relpath(out, REPO), len(STYLESHEETS), len(KN_GLUE), written))
    return out


def import_page_files(out, write_if_changed, log, public, collect_page_refs):
    """Every local file the page and its stylesheets reference (images, fonts, the HUD textures),
    from the reference tree. References v1.5.6 itself 404s on are listed and stay 404s."""
    copied, absent = 0, []
    glue_css = tuple(os.path.basename(g) for g in KN_GLUE + (KN_V156_CSS,) if g.endswith(".css"))
    refs = collect_page_refs(out, pages=("index.html",), css_skip=glue_css)
    for rel, where in refs:
        src = os.path.join(public, rel)
        if not os.path.isfile(src):
            absent.append("%s (%s)" % (rel, where))
            continue
        with open(src, "rb") as fh:
            copied += write_if_changed(os.path.join(out, rel), fh.read())
    for a in absent:
        log("v156: 404 " + a)
    log("v156: page files: %d referenced, %d rewritten, %d missing from v1.5.6 too" % (len(refs), copied, len(absent)))
    return copied
