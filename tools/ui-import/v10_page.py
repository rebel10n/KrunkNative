"""Krunker v10 (the live krunker.io client) menu page import: ``import.py --ref v10`` (loaded by import.py).

Writes a separate page tree, ``ui/v10/`` (the v7.1.4 page in ``ui/`` is left alone: every Krunker
version keeps its own page for the runtime version toggle), from two inputs:

- the live site mirror (``fetch_live_site.py --stage S``; ``--public S/public``): ``index.html``,
  ``social.html``, their stylesheets, fonts and images, byte copies of what krunker.io /
  assets.krunker.io serve;
- the live client script (``--client-js``; v10.0.2): v10's menu is a Svelte 5 app, so the live
  ``index.html`` is a shell with empty mount points (``#mainMenuUIMount``, ``#notificationCenter``,
  ...). ``v10_bundle.js`` reconstructs, without running any of it, the markup each boot component
  mounts (its initial, logged-out state), every component stylesheet and the English strings.

Transformations (same rules as the v7 import, ``index_page.py``): every ``<script>``, comments,
og/PWA/preload links removed; ``?build=`` / ``?v=`` stripped; asset-host and root-relative URLs
made relative; external links neutralised (``data-ext-href``); ``<canvas id="canvas">``, our
``css/krunknative.css`` + ``css/hud.css`` and ``js/kn-shim.js`` + ``js/loader.js`` added. v10
additions: the reconstructed mounts are written into their (empty) targets, ``css/svelte.css``
(the component stylesheets, which the Svelte runtime injects as ``<style>`` at mount) is linked
after Krunker's stylesheets, ``i18n-en.json`` holds ``translations.en``.

Our glue files (``css/krunknative.css``, ``css/hud.css``, ``js/kn-shim.js``, ``js/loader.js``) are
copied from ``ui/`` (their source of truth): the engine resolves page URLs inside the page root,
so ``ui/v10/`` cannot link ``../css/``. Window templates (``windows/``) are not reconstructed
statically (``window.windows[n].gen()`` builds them at run time): only ``windows/versions.txt``
(the changelog) is written; the rendered windows come from the Chromium capture
(``capture_v10_live.js``), once it has been run.
"""

import json
import os
import re
import shutil
import subprocess
import tempfile

import index_page
import social_page

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
PAGES = ("index.html", "social.html")
# ours: linked by the page (krunknative/hud), added at run time by the C UI (devconsole: ui_console.c,
# ranked-kn: ranked_ui.c) and the web glue
KN_GLUE = ("css/krunknative.css", "css/hud.css", "css/devconsole.css", "css/ranked-kn.css", "js/kn-shim.js", "js/loader.js")
DROP_LINKS_RE = re.compile(r"<link rel=\"(?:alternate|preload|dns-prefetch|preconnect|modulepreload|shortcut icon)\"[^>]*>\s*")
STYLESHEET_RE = re.compile(r"<link rel=\"stylesheet\"[^>]*href=\"([^\"]+)\"[^>]*>")


def run_bundle(client_js, out_dir, log):
    """node v10_bundle.js (static extraction; the client script is parsed, never run)."""
    node = shutil.which("node")
    if not node:
        raise SystemExit("v10: node is needed for v10_bundle.js")
    env = dict(os.environ)
    if "KN_NODE_MODULES" not in env and not os.path.isdir(os.path.join(REPO, "net-bridge", "node_modules", "acorn")):
        main = "/home/user/src/KrunkNative/net-bridge/node_modules"  # the shared checkout (worktrees have none)
        if os.path.isdir(os.path.join(main, "acorn")):
            env["KN_NODE_MODULES"] = main
    r = subprocess.run([node, "--max-old-space-size=8192", os.path.join(HERE, "v10_bundle.js"), client_js, out_dir],
                       capture_output=True, text=True, env=env, cwd=out_dir)
    if r.returncode != 0:
        raise SystemExit("v10_bundle.js failed:\n" + r.stderr[-4000:])
    for line in r.stdout.splitlines()[:3]:
        log("v10: " + line)


def svelte_css(text):
    """The component CSS lives in <style> at the document; as css/svelte.css its URLs move one level down."""
    def fix(m):
        quote, url = m.group(1), m.group(2)
        url = index_page.QUERY_RE.sub("", index_page.ASSETS_HOST_RE.sub("/", url))
        if url.startswith(("data:", "http:", "https:", "//", "#")):
            return m.group(0)
        url = "../" + (url[1:] if url.startswith("/") else url[2:] if url.startswith("./") else url)
        return "url(%s%s%s)" % (quote, url, quote)
    return re.sub(r"url\(\s*(['\"]?)([^)'\"]+)\1\s*\)", fix, text)


def inject_mounts(html, mounts):
    done = []
    for target, inner in mounts.items():
        if not inner:
            continue
        pat = re.compile(r"(<div id=\"%s\"[^>]*>)(</div>)" % re.escape(target))
        if pat.search(html):
            html = pat.sub(lambda m: m.group(1) + inner + m.group(2), html, count=1)
            done.append(target)
    return html, done


# description / keywords / og: / twitter: / apple-* metas: platform/web/pwa.py writes our own at web-dist time
DROP_META_RE = re.compile(r"<meta (?:property=\"og:|name=\"(?:twitter:|description|keywords|apple-mobile-web-app-))[^>]*>\s*")


def transform_page(name, html, mounts):
    html = DROP_LINKS_RE.sub("", html)
    html = DROP_META_RE.sub("", html)
    html = re.sub(r"<script\b[^>]*>.*?</script>\s*", "", html, flags=re.S | re.I)
    done = []
    if name == "index.html":
        html, done = inject_mounts(html, mounts)
        html = index_page.transform_index(html)
        html = html.replace('<link rel="stylesheet" href="css/krunknative.css">',
                            '<link rel="stylesheet" href="css/svelte.css">\n<link rel="stylesheet" href="css/krunknative.css">', 1)
    else:
        html = social_page._transform_html(html, True)
        html = re.sub(r"https?://assets\.krunker\.io/", "", html)
    html = re.sub(r"(href|src)=\"\./", r'\1="', html)
    return html, done


def import_v10(ui_root, write_if_changed, log, public, client_js):
    """ui_root = ui/ (the v7 tree, source of our glue files); writes ui/v10/. Returns the page dir."""
    out = os.path.join(ui_root, "v10")
    if not os.path.isfile(os.path.join(public, "index.html")):
        raise SystemExit("v10: %s/index.html missing (run fetch_live_site.py --stage <dir> first)" % public)
    mounts, written = {}, 0
    if client_js:
        with tempfile.TemporaryDirectory(prefix="kn-v10-bundle-") as tmp:
            run_bundle(os.path.abspath(client_js), tmp, log)
            with open(os.path.join(tmp, "mounts.json"), encoding="utf-8") as fh:
                mounts = json.load(fh)
            with open(os.path.join(tmp, "svelte.css"), encoding="utf-8") as fh:
                written += write_if_changed(os.path.join(out, "css", "svelte.css"), svelte_css(fh.read()))
            with open(os.path.join(tmp, "i18n-en.json"), "rb") as fh:
                written += write_if_changed(os.path.join(out, "i18n-en.json"), fh.read())
            with open(os.path.join(tmp, "report.txt"), encoding="utf-8") as fh:
                for line in fh.read().splitlines()[3:]:
                    if line.startswith(("mount #main", "render ")):
                        log("v10: " + line)
    else:
        log("v10: no --client-js: the Svelte mounts stay empty, no css/svelte.css / i18n-en.json")
    sheets = []
    for name in PAGES:
        with open(os.path.join(public, name), encoding="utf-8") as fh:
            raw = fh.read()
        for href in STYLESHEET_RE.findall(raw):
            rel = index_page.QUERY_RE.sub("", href).split("?")[0]
            rel = rel[2:] if rel.startswith("./") else rel.lstrip("/")
            if "://" not in rel and not rel.startswith("//") and rel not in sheets:
                sheets.append(rel)
        html, done = transform_page(name, raw, mounts)
        if done:
            log("v10: %s mounts filled: %s" % (name, ", ".join("#" + d for d in done)))
        written += write_if_changed(os.path.join(out, name), html)
    for rel in sheets:
        src = os.path.join(public, rel)
        if not os.path.isfile(src):
            log("v10: stylesheet %s not in the mirror, skipped" % rel)
            continue
        with open(src, encoding="utf-8") as fh:
            css = index_page.rewrite_css(fh.read(), os.path.dirname(rel))
        css = re.sub(r"url\((['\"]?)https?://assets\.krunker\.io/", r"url(\1" + "../" * rel.count("/"), css)
        written += write_if_changed(os.path.join(out, rel), css)
    for rel in KN_GLUE:
        with open(os.path.join(ui_root, rel), "rb") as fh:
            written += write_if_changed(os.path.join(out, rel), fh.read())
    with open(os.path.join(public, "docs", "versions.txt"), "rb") as fh:
        written += write_if_changed(os.path.join(out, "windows", "versions.txt"), fh.read())
    log("v10: %s: %d pages, %d stylesheets, %d glue files, %d rewritten" % (
        os.path.relpath(out, REPO), len(PAGES), len(sheets), len(KN_GLUE), written))
    return out
