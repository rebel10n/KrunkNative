"""Krunker v3.1.1 model viewer page import (``make ui-import`` step, loaded by import.py).

Copies ``public/viewer.html`` from the v3.1.1 reference tree into ``ui/`` with the
transformations of the menu page (``ui/README.md``):

- ``<script>`` elements (hook.js, dat.gui.min.js, zip.js, zip-ext.js, viewer.js) are
  removed: the viewer's behaviour is C (``client/src/viewer/``), the dat.gui panel is
  rebuilt by ``client/src/viewer/viewer_gui.c`` over ``html_dom.h`` and the zip import by
  ``viewer_import.c``.
- ``?build=xxxx`` query strings are stripped and root-relative URLs made relative.
- ``css/dat-gui.css`` and ``css/viewer-kn.css`` are linked after the page's own <style>.
- ``<canvas id="canvas">`` is added as the first body child and ``js/kn-shim.js`` +
  ``js/loader.js`` before ``</body>`` (the web build's WebGL target and DOM glue, as in
  ``ui/index.html``; the native engine ignores both).

``css/dat-gui.css`` is the stylesheet dat.gui injects into the page at run time
(``public/libs/dat.gui.min.js``, the string passed to its CSS injector), unchanged.
``css/viewer-kn.css`` is KrunkNative's own (hand-maintained).

When node is available the store tables of the viewer
(``client/src/viewer/viewer_store_table.c``) are regenerated too
(``tools/ui-import/gen_viewer_store.js``, from ``public/js/src/viewer.js.map``).
"""

import os
import re
import shutil
import subprocess

KRUNKER_PUBLIC = "/home/user/jails/kru/src/Krunker_v3.1.1/public"
HERE = os.path.dirname(os.path.abspath(__file__))

DAT_GUI_CSS_HEADER = ("/* dat.gui 0.7 stylesheet, injected at run time by Krunker v3.1.1's "
                      "public/libs/dat.gui.min.js (extracted unchanged by tools/ui-import/viewer_page.py). */\n")


def _transform_html(html):
    html = re.sub(r"<script\b[^>]*>.*?</script>\s*", "", html, flags=re.S | re.I)
    html = re.sub(r"\?build=[A-Za-z0-9]+", "", html)
    html = re.sub(r"(href|src)=\"/(?!/)", r'\1="', html)
    html = html.replace('<link rel="shortcut icon" type="image/png" href="img/favicon.png"/>',
                        '<link rel="shortcut icon" type="image/png" href="favicon.png"/>')
    links = ""
    for css in ("css/dat-gui.css", "css/viewer-kn.css"):
        if css not in html:
            links += '<link rel="stylesheet" href="%s"/>\n' % css
    if links:
        html = html.replace("</head>", links + "</head>", 1)
    # the web build's WebGL target (inert natively) and its DOM glue, as in ui/index.html
    if 'id="canvas"' not in html:
        html = re.sub(r"(<body[^>]*>\n?)", r'\1<canvas id="canvas" tabindex="-1"></canvas>\n', html, count=1)
    if "js/kn-shim.js" not in html:
        html = html.replace("</body>", '<script src="js/kn-shim.js"></script>\n<script src="js/loader.js"></script>\n</body>', 1)
    return html


def _dat_gui_css(src_root):
    with open(os.path.join(src_root, "libs", "dat.gui.min.js"), "r", encoding="utf-8") as fh:
        js = fh.read()
    m = re.search(r'"(\.dg ul\{.*?)"', js, flags=re.S)
    if not m:
        raise SystemExit("viewer: dat.gui stylesheet not found in dat.gui.min.js")
    css = m.group(1).replace("\\n", "\n").replace('\\"', '"')
    return DAT_GUI_CSS_HEADER + css


def import_viewer(ui_dir, write_if_changed, log, src_root=KRUNKER_PUBLIC):
    """Returns the number of files rewritten. Missing reference tree: logged and skipped."""
    src_html = os.path.join(src_root, "viewer.html")
    if not os.path.exists(src_html):
        log("viewer: %s not found, skipped" % src_html)
        return 0
    written = 0
    with open(src_html, "r", encoding="utf-8") as fh:
        html = _transform_html(fh.read())
    if write_if_changed(os.path.join(ui_dir, "viewer.html"), html):
        written += 1
    if write_if_changed(os.path.join(ui_dir, "css", "dat-gui.css"), _dat_gui_css(src_root)):
        written += 1
    if not os.path.exists(os.path.join(ui_dir, "css", "viewer-kn.css")):
        log("viewer: warning: ui/css/viewer-kn.css missing (hand-maintained)")

    source_map = os.path.join(src_root, "js", "src", "viewer.js.map")
    node = shutil.which("node")
    if node and os.path.exists(source_map):
        subprocess.run([node, os.path.join(HERE, "gen_viewer_store.js"), source_map], check=True)
    else:
        log("viewer: node or viewer.js.map missing, viewer_store_table.c not regenerated")
    log("viewer: viewer.html + dat-gui.css, %d rewritten" % written)
    return written
