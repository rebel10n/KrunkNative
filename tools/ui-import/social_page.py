"""Krunker social hub page import (v7.1.4 with ``v7=True``, else v3.1.1) (``make ui-import`` step, loaded by import.py).

Copies ``public/social.html`` and the stylesheets/images it uses from the v3.1.1
reference tree into ``ui/``, with the same transformations as the menu page
(``ui/README.md``):

- ``<script>`` elements (hook.js, recaptcha, social.js, howler, hcaptcha, Chart.js and
  the inline captcha callbacks), HTML comments and ``og:``/description metas are
  removed: the hub's behaviour is C (``client/src/ui/social_ui.c``).
- ``?build=xxxx`` query strings are stripped and root-relative URLs made relative.
- ``<canvas id="canvas" style="display:none">`` (first body child) and the web build's
  ``js/kn-shim.js`` + ``js/loader.js`` (end of body) are added, as on ``index.html``: the
  page is its own URL on the web and boots the same ``client.js`` in hub mode.
- The asset-server profile icon (``https://totallynotio.krunker.zip/textures/classes/
  icon_0.png``) points at the local copy in ``ui/textures/classes/``.
- The app badges (``#androidDB`` / ``#iosDB``: Krunker Hub on Google Play / the App Store)
  open KrunkNative's Download window (``/?download``, ``client/src/ui/download_ui.c``: the
  Android APK, Add to Home Screen on iOS) instead of Krunker's store pages (``APP_BADGES``);
  ``#steamDB`` (Krunker on Steam) stays hidden (``social_ui.c``).

``css/social.css`` and ``css/social_custom.css`` are copied byte-identical (social.css
keeps its dead web-archive ``url()``s, which fail in the real offline page too);
``css/flag.css``, ``css/google-secular-one.css`` and ``css/material-icons.css`` are the
menu's copies. Images: only those the page and the hub's C logic reference.
"""

import os
import re

KRUNKER_PUBLIC = "/home/user/jails/kru/src/Krunker_v3.1.1/public"

SOCIAL_CSS = ("social.css", "social_custom.css")
SHARED_CSS = ("flag.css", "google-secular-one.css", "material-icons.css")  # imported with the menu
SOCIAL_IMG = ("steam.png", "android.png", "ios.png", "editor.png", "black_m.png", "crossed_swords_0.svg",
              "shaking_hands.svg", "favicon.png", "noimg.png", "backgrounds/bg_test.png")
# Clan Wars region rewards (store.skins with limT "Clan Wars"), shown by the region cards
SOCIAL_TEXTURES = ("previews/cosmetics/6_9.png", "previews/melee/melee_18.png", "previews/cosmetics/2_66.png",
                   "previews/cosmetics/1_128.png", "previews/cosmetics/6_8.png", "previews/cosmetics/6_10.png",
                   "previews/cosmetics/2_65.png", "previews/cosmetics/1_129.png")


# Krunker's store links on the hub's app badges -> KrunkNative's Download window
APP_BADGES = (("https://play.google.com/store/apps/details?id=ch.yendis.krunkerhub", "/?download"),
              ("https://apps.apple.com/app/id1515679115", "/?download"))


def _transform_html(html, v7=False):
    html = re.sub(r"<script\b[^>]*>.*?</script>\s*", "", html, flags=re.S | re.I)
    if v7:
        html = re.sub(r"\?(?:build|v)=[A-Za-z0-9]+", "", html)
        html = re.sub(r"https?://assets\.krunker\.io/", "/", html)
        html = re.sub(r"<meta (property=\"og:|name=\"(description|keywords)\")[^>]*>\s*", "", html)
    html = re.sub(r"<!--.*?-->\s*", "", html, flags=re.S)
    html = re.sub(r"<meta (property=\"og:|name=\"(description|keywords)\")[^>]*>\s*", "", html)
    html = re.sub(r"\?build=[A-Za-z0-9]+", "", html)
    html = re.sub(r"(href|src)=\"/(?!/)", r'\1="', html)
    html = html.replace("https://totallynotio.krunker.zip/textures/classes/icon_0.png", "textures/classes/icon_0.png")
    for store, ours in APP_BADGES:
        html = html.replace(store, ours)
    # Web build: the page boots the same client.js as index.html (in hub mode, from its
    # file name). The WebGL canvas GLFW needs stays hidden: the hub draws no 3D.
    if 'id="canvas"' not in html:
        html = re.sub(r"(<body[^>]*>\n?)", r'\1\n<canvas id="canvas" tabindex="-1" style="display:none"></canvas>\n', html, count=1)
    if "js/kn-shim.js" not in html:
        html = html.replace("</body>", '<script src="js/kn-shim.js"></script>\n<script src="js/loader.js"></script>\n</body>', 1)
    return html


def import_social(ui_dir, write_if_changed, log, src_root=KRUNKER_PUBLIC, v7=False):
    """Returns the number of files rewritten. Missing reference tree: logged and skipped."""
    src_html = os.path.join(src_root, "social.html")
    if not os.path.exists(src_html):
        log("social: %s not found, skipped" % src_html)
        return 0
    written = 0
    with open(src_html, "r", encoding="utf-8") as fh:
        html = _transform_html(fh.read(), v7)
    if write_if_changed(os.path.join(ui_dir, "social.html"), html):
        written += 1
    if v7:
        # v7: stylesheets with the menu's URL rewriting; images come from the page's own
        # references (menu_assets.collect_page_refs) and menu-assets-v7.txt
        import index_page
        for name in SOCIAL_CSS:
            with open(os.path.join(src_root, "css", name), "r", encoding="utf-8") as fh:
                css = index_page.rewrite_css(fh.read(), "css")
            if write_if_changed(os.path.join(ui_dir, "css", name), css):
                written += 1
        log("social: social.html + %d stylesheets, %d rewritten" % (len(SOCIAL_CSS), written))
        return written
    for name in SOCIAL_CSS:
        with open(os.path.join(src_root, "css", name), "rb") as fh:
            data = fh.read()
        if write_if_changed(os.path.join(ui_dir, "css", name), data):
            written += 1
    for name in SOCIAL_IMG:
        with open(os.path.join(src_root, "img", name), "rb") as fh:
            data = fh.read()
        if write_if_changed(os.path.join(ui_dir, "img", name), data):
            written += 1
    for name in SOCIAL_TEXTURES:
        with open(os.path.join(src_root, "textures", name), "rb") as fh:
            data = fh.read()
        dst = os.path.join(ui_dir, "textures", name)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        if write_if_changed(dst, data):
            written += 1
    for name in SHARED_CSS:
        if not os.path.exists(os.path.join(ui_dir, "css", name)):
            log("social: warning: ui/css/%s missing (menu import)" % name)
    log("social: social.html + %d stylesheets + %d images + %d textures, %d rewritten" % (
        len(SOCIAL_CSS), len(SOCIAL_IMG), len(SOCIAL_TEXTURES), written))
    return written
