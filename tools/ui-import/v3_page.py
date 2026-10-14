#!/usr/bin/env python3
"""Krunker v3.1.1 page tree import: ``ui/v3/`` from the ``v3.1.1`` tag's ``ui/`` (``make ui-import-v3``).

The v3.1.1 port kept Krunker v3.1.1's menu page in ``ui/`` (branch / tag ``v3.1.1``, ``v3.1.1-parity``);
the version toggle (doc/v10/version-toggle.md, client/include/ruleset_ui.h) shows it from ``ui/v3/``
next to the v7.1.4 page in ``ui/``. The files come out of git (``git archive <ref> ui``), never from
another checkout, and get the rules ``tools/ui-import`` applies to the v7 page:

- every ``<script>`` is removed (the native engine ignores them; ``js/kn-shim.js`` and
  ``js/loader.js`` are the web build's glue and the web build does not serve ``ui/v3``), so ``js/``
  is not copied;
- ``?build=`` / ``?v=`` query strings are stripped and root-relative or web-archive
  ``https://krunker.io/`` URLs are made relative (the v3.1.1 tree already did all of this for the
  files kept here);
- ``windows/`` keeps ``versions.txt`` (the v3.1.1 Changelog, ui_loadout.c reads it from the page tree)
  only: v3.1.1's window templates have v3.1.1's ``windows.json`` shape (no Settings / Controls tab
  lists) and markup for v3.1.1's C UI, while the C UI logic on main writes v7.1.4's window markup, so
  the page borrows ``ui/windows/`` (ui_windows.c) like ``ui/v10/``;
- ``css/v7-windows.css`` (generated, linked after the page's stylesheets): those borrowed v7.1.4
  windows are styled by v7's sheets, so this copies, from ``ui/css/`` (main.css, the bundled sheet,
  main_custom.css and our krunknative.css / hud.css), every selector that names a class the v7 window
  templates (``ui/windows/*.html``) use and v3.1.1's own sheets do not define, plus the @keyframes
  those rules animate. Nothing of v3.1.1's own markup changes (its classes are left out by
  construction), and hides ``#propInstruct`` (v3.1.1's prop hunt hint, which the C UI never
  hides); rerun after ``make ui-import`` changes ``ui/``;
- ``social.html``, ``editor.html``, ``viewer.html`` and their stylesheets are left out: the hub,
  editor and viewer C code is written for v7.1.4's pages, so the engine shows ``ui/``'s for a page
  this tree lacks (client/src/html/engine/engine.c ``fallback_dir``; it also reads ui/'s copy of a
  stylesheet or image the tree lacks, e.g. the images of the borrowed v7.1.4 windows);
- the branch's ``README.md`` (it describes the tree as ``ui/``) is replaced by ``ui/v3/README.md``,
  which this script leaves alone.

Usage: python3 -I tools/ui-import/v3_page.py [--ref v3.1.1] [--ui ui]   (from the repository root)
"""

import argparse
import io
import os
import re
import shutil
import subprocess
import sys
import tarfile

SCRIPT_RE = re.compile(r"[ \t]*<script\b[^>]*>.*?</script>[ \t]*\n?", re.S | re.I)
QUERY_RE = re.compile(r"\?(?:build|v)=[^\"'\s)&]*")
# web-archive copies of krunker.io URLs (css/social.css): /web/<stamp>im_/https://krunker.io/<path>
ARCHIVE_RE = re.compile(r"/web/\d+[a-z_]*/https?://(?:assets\.)?krunker\.io/")
ASSETS_HOST_RE = re.compile(r"https?://assets\.krunker\.io/")
KEEP_OWN = ("README.md",)  # ui/v3 files of ours (not from the tag)
# the hub / editor / viewer pages and the sheets only they link: ui/'s are shown (engine fallback_dir)
OTHER_PAGES = ("social.html", "editor.html", "viewer.html", "css/social.css", "css/social_custom.css", "css/editor.css",
               "css/editor_custom.css", "css/editor_dark.css", "css/editor-kn.css", "css/pickr.css", "css/dat-gui.css",
               "css/viewer-kn.css")


def transform_html(text):
    text = SCRIPT_RE.sub("", text)
    text = QUERY_RE.sub("", text)
    text = ASSETS_HOST_RE.sub("", text)
    return re.sub(r"(href|src)=\"/(?!/)", r'\1="', text)


def transform_css(text, rel_dir):
    up = "../" * len([p for p in rel_dir.split("/") if p])

    def fix(m):
        quote, url = m.group(1), m.group(2)
        url = QUERY_RE.sub("", url)
        url = ARCHIVE_RE.sub("/", url)
        url = ASSETS_HOST_RE.sub("/", url)
        if url.startswith("/") and not url.startswith("//"):
            url = up + url[1:]
        return "url(%s%s%s)" % (quote, url, quote)

    return re.sub(r"url\(\s*(['\"]?)([^)'\"]+)\1\s*\)", fix, text)


BRIDGE = "css/v7-windows.css"
BRIDGE_SOURCES = ("css/main.css", "css/bundled/bundledStyles.min.css", "css/main_custom.css", "css/krunknative.css", "css/hud.css")
CLASS_RE = re.compile(r"\.([A-Za-z_][\w-]*)")


def css_blocks(text):
    """Top-level ``prelude { body }`` pairs of a stylesheet (comments removed)."""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    out, i, n = [], 0, len(text)
    while i < n:
        j = text.find("{", i)
        if j < 0:
            break
        depth, k = 0, j
        while k < n:
            depth += text[k] == "{"
            depth -= text[k] == "}"
            if depth == 0:
                break
            k += 1
        out.append((text[i:j].strip(), text[j + 1:k]))
        i = k + 1
    return out


def selector_classes(selector):
    return set(CLASS_RE.findall(re.sub(r"url\([^)]*\)", "", selector)))


def defined_classes(texts):
    out = set()
    for text in texts:
        for pre, body in css_blocks(text):
            if pre.startswith(("@media", "@supports")):
                for inner, _ in css_blocks(body):
                    out |= selector_classes(inner)
            elif not pre.startswith("@"):
                out |= selector_classes(pre)
    return out


def bridge_css(ui_dir, v3_sheets):
    """css/v7-windows.css: v7's rules for the v7 window templates' classes v3.1.1's sheets lack."""
    sources = []
    for rel in BRIDGE_SOURCES:
        with open(os.path.join(ui_dir, rel), encoding="utf-8") as fh:
            sources.append((rel, fh.read()))
    used = set()
    windows = os.path.join(ui_dir, "windows")
    for name in sorted(os.listdir(windows)):
        if name.endswith(".html"):
            with open(os.path.join(windows, name), encoding="utf-8") as fh:
                for value in re.findall(r'class="([^"]*)"', fh.read()):
                    used |= set(value.split())
    v3 = defined_classes(v3_sheets)
    missing = (defined_classes(text for _, text in sources) - v3) & used

    def keep(prelude):
        return ", ".join(sel.strip() for sel in prelude.split(",") if selector_classes(sel) & missing)

    rules, keyframes = [], {}
    for rel, text in sources:
        rules.append("/* %s */" % rel)
        for pre, body in css_blocks(text):
            if pre.startswith("@keyframes") or pre.startswith("@-webkit-keyframes"):
                keyframes.setdefault(pre.split()[-1], "%s {%s}" % (pre, body))
            elif pre.startswith(("@media", "@supports")):
                inner = ["  %s {%s}" % (k, b.strip()) for k, b in ((keep(p), b) for p, b in css_blocks(body)) if k]
                if inner:
                    rules.append("%s {\n%s\n}" % (pre, "\n".join(inner)))
            elif not pre.startswith("@"):
                k = keep(pre)
                if k:
                    rules.append("%s {%s}" % (k, body.strip()))
        if rules[-1].startswith("/* "):
            rules.pop()
    css = "\n".join(rules)
    v3_text = "\n".join(v3_sheets)
    for name in sorted(set(re.findall(r"animation(?:-name)?\s*:\s*([\w-]+)", css))):
        if name in keyframes and not re.search(r"@(?:-webkit-)?keyframes\s+%s\b" % re.escape(name), v3_text):
            css += "\n" + keyframes[name]
    # v3.1.1 HUD markup its own JS hid, which the v7.1.4 C UI does not know: kept hidden
    css += "\n/* v3.1.1 HUD elements the C UI does not drive (v3.1.1's JS showed them in prop hunt only) */\n#propInstruct {display: none !important;}"
    header = ("/* Generated by tools/ui-import/v3_page.py (make ui-import-v3): Krunker v7.1.4's rules (ui/css/) for\n"
              "   the classes of the v7.1.4 window templates (ui/windows/) that this v3.1.1 page's sheets lack.\n"
              "   The C UI writes v7.1.4 window markup on every page (ui_windows.c); do not edit. */\n")
    return header + css + "\n", len(missing)


def link_bridge(html):
    if BRIDGE in html or "</head>" not in html:
        return html
    last = list(re.finditer(r"<link rel=\"stylesheet\"[^>]*>\n", html))
    at = last[-1].end() if last else html.index("</head>")
    return html[:at] + '<link rel="stylesheet" href="%s"/>\n' % BRIDGE + html[at:]


def wanted(rel):
    if rel in KEEP_OWN or rel in OTHER_PAGES or rel.startswith("js/"):
        return False
    if rel.startswith("windows/"):
        return rel == "windows/versions.txt"
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--ref", default="v3.1.1", help="git ref holding the v3.1.1 ui/ tree (default v3.1.1)")
    ap.add_argument("--ui", default="ui", help="the ui/ directory (default ui)")
    args = ap.parse_args()

    repo = os.getcwd()
    data = subprocess.run(["git", "-C", repo, "archive", "--format=tar", args.ref, "ui"], check=True, stdout=subprocess.PIPE).stdout
    out = os.path.join(args.ui, "v3")
    own = {}
    for name in KEEP_OWN:
        path = os.path.join(out, name)
        if os.path.exists(path):
            with open(path, "rb") as fh:
                own[name] = fh.read()
    shutil.rmtree(out, ignore_errors=True)
    files = rewritten = 0
    with tarfile.open(fileobj=io.BytesIO(data)) as tar:
        for member in tar.getmembers():
            if not member.isfile() or not member.name.startswith("ui/"):
                continue
            rel = member.name[len("ui/"):]
            if ".." in rel.split("/") or not wanted(rel):
                continue
            blob = tar.extractfile(member).read()
            if rel == "index.html":
                new = link_bridge(transform_html(blob.decode("utf-8"))).encode("utf-8")
            elif rel.endswith(".html"):
                new = transform_html(blob.decode("utf-8")).encode("utf-8")
            elif rel.endswith(".css"):
                new = transform_css(blob.decode("utf-8"), os.path.dirname(rel)).encode("utf-8")
            else:
                new = blob
            rewritten += new != blob
            dest = os.path.join(out, rel)
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            with open(dest, "wb") as fh:
                fh.write(new)
            files += 1
    sheets = []
    for dirpath, _, names in os.walk(os.path.join(out, "css")):
        for name in sorted(names):
            if name.endswith(".css"):
                with open(os.path.join(dirpath, name), encoding="utf-8") as fh:
                    sheets.append(fh.read())
    css, classes = bridge_css(args.ui, sheets)
    with open(os.path.join(out, BRIDGE), "w", encoding="utf-8") as fh:
        fh.write(css)
    print("v3: %s/%s: %d bytes for %d window classes" % (out, BRIDGE, len(css), classes))
    for name, blob in own.items():
        with open(os.path.join(out, name), "wb") as fh:
            fh.write(blob)
    print("v3: %s/ from %s:ui: %d files, %d rewritten" % (out, args.ref, files, rewritten))
    return 0


if __name__ == "__main__":
    sys.exit(main())
