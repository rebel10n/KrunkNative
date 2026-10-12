"""Krunker map editor page import (v7.1.4 with ``v7=True``, else v3.1.1) (``make ui-import`` step, loaded by import.py).

Copies ``public/editor.html`` and the stylesheets it links from the reference tree (v7.1.4
since editor WP2, doc/v7/editor.md) into ``ui/``, applying the same transformations as the
menu page (``ui/README.md``):

- ``<script>`` elements (hook.js, quicksettings.js, pickr.min.js, editor.js and the
  inline ``onbeforeunload`` script) and ``og:`` metas are removed: the editor's
  behaviour is C (``client/src/editor/``), the QuickSettings panels and the Pickr
  colour picker are rebuilt by it over ``html_dom.h``.
- ``?build=xxxx`` query strings are stripped and root-relative URLs made relative.
- ``css/editor-kn.css`` (KrunkNative's own: transparent 3D area over the GL scene) and
  ``css/editor-script.css`` (the KrunkScript window, from Krunker's scripting.css) are
  linked after Krunker's stylesheets; ``<canvas id="canvas">`` (the web build's WebGL
  target) is the first body child and ``js/kn-shim.js`` + ``js/loader.js`` (web glue)
  load before ``</body>``, as in ``ui/index.html``.

Only the files the editor page references are copied: ``css/editor.css``,
``css/editor_dark.css``, ``css/editor_custom.css``, ``css/pickr.css`` (the fonts it
uses, ``css/fonts/font2.ttf``, Material Icons and Secular One, are already in ``ui/``
for the menu).
"""

import os
import re

KRUNKER_PUBLIC = "/home/user/jails/kru/src/Krunker_v3.1.1/public"

EDITOR_CSS = ("editor.css", "editor_dark.css", "editor_custom.css", "pickr.css")
# v7 also links CodeMirror 5.61 and its monokai theme (the KrunkScript editor)
EDITOR_LIBS_V7 = ("libs/codemirror/5.61.0/codemirror.min.css", "libs/codemirror/5.61.0/theme/monokai.min.css")
SHARED_CSS = ("google-secular-one.css", "material-icons.css")  # imported with the menu


def _transform_html(html):
    html = re.sub(r"<script\b[^>]*>.*?</script>\s*", "", html, flags=re.S | re.I)
    html = re.sub(r"<meta property=\"og:[^>]*>\s*", "", html)
    html = re.sub(r"\?(?:build|v)=[A-Za-z0-9]+", "", html)
    html = re.sub(r"https?://assets\.krunker\.io/", "/", html)
    html = re.sub(r"(href|src)=\"/(?!/)", r'\1="', html)
    html = html.replace('<link rel="shortcut icon" type="image/png" href="img/favicon.png"/>',
                        '<link rel="shortcut icon" type="image/png" href="favicon.png"/>')
    kn_link = '<link rel="stylesheet" href="css/editor-kn.css"/>\n'
    if "css/editor-kn.css" not in html:
        html = html.replace("</head>", kn_link + "</head>", 1)
    # the KrunkScript window (WP5: scripting.html's CSS, client/src/editor/editor_script.c)
    script_link = '<link rel="stylesheet" href="css/editor-script.css"/>\n'
    if "css/editor-script.css" not in html:
        html = html.replace("</head>", script_link + "</head>", 1)
    # web build: the GLFW/WebGL canvas first in <body> (under the page, css/editor-kn.css)
    # and the two glue scripts before </body>, exactly like ui/index.html
    html = re.sub(r'(<body[^>]*>\n?)', r'\1<canvas id="canvas" tabindex="-1"></canvas>\n', html, count=1)
    html = html.replace("</body>", '<script src="js/kn-shim.js"></script>\n<script src="js/loader.js"></script>\n</body>', 1)
    return html


def import_editor(ui_dir, write_if_changed, log, src_root=KRUNKER_PUBLIC, v7=False):
    """Returns the number of files rewritten. Missing reference tree: logged and skipped."""
    src_html = os.path.join(src_root, "editor.html")
    if not os.path.exists(src_html):
        log("editor: %s not found, skipped" % src_html)
        return 0
    written = 0
    with open(src_html, "r", encoding="utf-8") as fh:
        html = _transform_html(fh.read())
    if write_if_changed(os.path.join(ui_dir, "editor.html"), html):
        written += 1
    for name in EDITOR_CSS:
        with open(os.path.join(src_root, "css", name), "rb") as fh:
            data = fh.read()
        if v7:
            import index_page
            data = index_page.rewrite_css(data.decode("utf-8"), "css").encode("utf-8")
        if write_if_changed(os.path.join(ui_dir, "css", name), data):
            written += 1
    for rel in (EDITOR_LIBS_V7 if v7 else ()):
        with open(os.path.join(src_root, rel), "rb") as fh:
            if write_if_changed(os.path.join(ui_dir, rel), fh.read()):
                written += 1
    for name in SHARED_CSS:
        if not os.path.exists(os.path.join(ui_dir, "css", name)):
            log("editor: warning: ui/css/%s missing (menu import)" % name)
    log("editor: editor.html + %d stylesheets, %d rewritten" % (len(EDITOR_CSS), written))
    return written
