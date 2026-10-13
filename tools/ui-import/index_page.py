"""Krunker v7.1.4 menu page import (``make ui-import`` step, loaded by import.py).

Writes ``ui/index.html`` from v7's ``public/index.html`` and copies the stylesheets it
links, with the transformations documented in ``ui/README.md``:

- every ``<script>`` (hook.js, mithril, frvr, index.js, the libs, the inline scripts:
  ``_isMobile``, the captcha callbacks, ``randInt``, the early ``scaleBase`` copy, the
  ``#mainLogoFace`` pick and the FTUE / gradual-menu block), HTML comments, ``og:`` metas,
  the PWA manifest / apple-touch-icon links are removed: the page's behaviour is C
  (``client/src/ui/``). KrunkNative shows the full menu of a returning player (v7's
  ``fullMenu()`` state); the FTUE gradual menu is not ported.
- ``?build=`` / ``?v=`` query strings are stripped; ``https://assets.krunker.io/`` URLs
  (rewritten to the local tree at run time by v7's hook.js) and root-relative URLs are
  made relative, so the page works from ``file://`` and from any subpath.
- external ``<a href>``s are neutralised (``href="#" data-ext-href=...``): the page never
  navigates (``client/src/ui/ui_handlers.c`` opens them in a browser).
- ``<canvas id="canvas">`` (the web build's WebGL target) becomes the first body child,
  ``css/krunknative.css`` + ``css/hud.css`` (ours) are linked after Krunker's stylesheets
  and ``js/kn-shim.js`` + ``js/loader.js`` (web glue) load before ``</body>``.
- the page ``<title>`` is the product name, ``Krunker Box Office`` (v7: ``Krunker``; the web
  build's tab and bookmarks).
- the "Experiments" footer link (``abTestsPopup()``, v7's A/B test opt-in popup) and the
  separator after it are dropped from ``#termsInfo`` (intentional difference: KrunkNative
  runs no A/B tests), so Contact / Terms / Changelog stay evenly spaced.

Stylesheets (``STYLESHEETS``) are copied with the same URL rewriting (assets host and
root-relative ``url()``s made relative, query strings dropped); nothing else changes.
"""

import os
import re

ASSETS_HOST_RE = re.compile(r"https?://assets\.krunker\.io/")
# the whole query up to the URL's delimiter: v7 sometimes writes past it ("other.png?build=X.png",
# which its server answers with other.png)
QUERY_RE = re.compile(r"\?(?:build|v)=[^\"'\s)&]*")

# index.html <link rel="stylesheet">s, in page order (v7.1.4)
STYLESHEETS = ("main.css", "bundled/bundledStyles.min.css", "main_custom.css", "flag.css", "material-icons.css",
               "material-icons-outlined.css", "material-symbols-outlined.css", "google-play.css")
KN_STYLESHEETS = ("css/krunknative.css", "css/hud.css")


def rewrite_css(css, css_rel_dir):
    """url()s of a stylesheet at ui/<css_rel_dir>/: assets host and root-relative -> relative."""
    depth = len([p for p in css_rel_dir.split("/") if p])
    up = "../" * depth

    def fix(m):
        quote, url = m.group(1), m.group(2)
        url = QUERY_RE.sub("", url)
        url = ASSETS_HOST_RE.sub("/", url)
        if url.startswith("/") and not url.startswith("//"):
            url = up + url[1:]
        return "url(%s%s%s)" % (quote, url, quote)

    return re.sub(r"url\(\s*(['\"]?)([^)'\"]+)\1\s*\)", fix, css)


def _ext_link(m):
    attrs = m.group(1)
    href = m.group(2)
    attrs = re.sub(r'\s*target="_blank"', "", attrs)
    return '<a%s href="#" data-ext-href="%s"' % (attrs, href)


# v7's "Experiments" footer link plus the separator after it (KN has no A/B tests)
AB_TESTS_LINK_RE = re.compile(r'<span class="terms"[^>]*onclick="abTestsPopup\(\)"[^>]*>[^<]*</span>\s*'
                              r'(?:<span class="verticalSeparatorInline"></span>\s*)?')


def transform_index(html):
    html = AB_TESTS_LINK_RE.sub("", html)
    html = re.sub(r"<title>[^<]*</title>", "<title>Krunker Box Office</title>", html, count=1)
    html = re.sub(r"<script\b[^>]*>.*?</script>\s*", "", html, flags=re.S | re.I)
    html = re.sub(r"<!--.*?-->\s*", "", html, flags=re.S)
    html = re.sub(r"<meta property=\"og:[^>]*>\s*", "", html)
    html = re.sub(r"<link rel=\"(manifest|apple-touch-icon|icon)\"[^>]*>\s*", "", html)
    html = QUERY_RE.sub("", html)
    html = ASSETS_HOST_RE.sub("/", html)
    html = re.sub(r"(href|src)=\"/(?!/)", r'\1="', html)
    html = re.sub(r"url\(/(?!/)", "url(", html)
    # external links never navigate the page (ui_handlers.c l_document_click)
    html = re.sub(r"<a([^>]*?) href=\"(https?://[^\"]*)\"", _ext_link, html)
    links = "".join('<link rel="stylesheet" href="%s">\n' % css for css in KN_STYLESHEETS if css not in html)
    if links:
        # after Krunker's last stylesheet link, before its inline <style>
        last = list(re.finditer(r"<link rel=\"stylesheet\"[^>]*>\n", html))
        at = last[-1].end() if last else html.index("</head>")
        html = html[:at] + links + html[at:]
    if 'id="canvas"' not in html:
        html = re.sub(r"(<body[^>]*>\n?)", r'\1<canvas id="canvas" tabindex="-1"></canvas>\n', html, count=1)
    if "js/kn-shim.js" not in html:
        html = html.replace("</body>", '<script src="js/kn-shim.js"></script>\n<script src="js/loader.js"></script>\n</body>', 1)
    return html


# linked by the other pages (editor.html, social.html) but not by index.html
SHARED_STYLESHEETS = ("google-secular-one.css",)


def import_shared_css(ui_dir, write_if_changed, log, src_root):
    written = 0
    for name in SHARED_STYLESHEETS:
        with open(os.path.join(src_root, "css", name), "r", encoding="utf-8") as fh:
            css = rewrite_css(fh.read(), "css")
        if write_if_changed(os.path.join(ui_dir, "css", name), css):
            written += 1
    return written


def import_index(ui_dir, write_if_changed, log, src_root):
    """Returns the number of files rewritten."""
    src_html = os.path.join(src_root, "index.html")
    if not os.path.exists(src_html):
        log("index: %s not found, skipped" % src_html)
        return 0
    written = 0
    with open(src_html, "r", encoding="utf-8") as fh:
        html = transform_index(fh.read())
    if write_if_changed(os.path.join(ui_dir, "index.html"), html):
        written += 1
    for name in STYLESHEETS:
        with open(os.path.join(src_root, "css", name), "r", encoding="utf-8") as fh:
            css = rewrite_css(fh.read(), os.path.dirname(os.path.join("css", name)))
        if write_if_changed(os.path.join(ui_dir, "css", name), css):
            written += 1
    for css in KN_STYLESHEETS:
        if not os.path.exists(os.path.join(ui_dir, css)):
            log("index: warning: ui/%s missing (hand-maintained)" % css)
    log("index: index.html + %d stylesheets, %d rewritten" % (len(STYLESHEETS), written))
    return written
