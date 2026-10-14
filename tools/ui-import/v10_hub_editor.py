#!/usr/bin/env python3
"""Krunker v10 (live krunker.io) hub and editor pages: ``ui/social.html``, ``ui/editor.html``.

Run by ``import.py`` (``make ui-import``) in place of the v7.1.4 ``social_page.py`` /
``editor_page.py`` steps, or alone::

    python3 -I tools/ui-import/v10_hub_editor.py --ui ui [--public ~/.cache/kn-v10/live/site/public]

Input: the live site mirror (``fetch_live_site.py --stage S``; ``--public S/public``), byte copies of
what krunker.io serves: ``social.html``, ``editor.html`` and the stylesheets they link. The v10
hub and editor are static pages over a Vite bundle (``static/social-*.js`` / ``static/editor-*.js``,
never fetched by the mirror and never run here); their behaviour is C (``client/src/ui/social_ui.c``,
``client/src/editor/``). Parity notes: ``doc/v10/editor-social.md``.

Transformations (the rules of the v7.1.4 import, ``social_page.py`` / ``editor_page.py``):

- every ``<script>`` (FRVR SDK + channel, the inline FRVR config / error reporter, gtag, the
  ld+json blocks, the module entry, altcha, CodeMirror, howler, Chart.js, ...), HTML comments,
  ``<link rel="modulepreload">``, and the description / keywords / ``og:`` / ``twitter:`` metas
  are removed;
- ``?build=`` / ``?v=`` stripped; ``https://assets.krunker.io/``, root-relative and ``./`` URLs
  made relative to ``ui/``;
- the shared Vite stylesheet ``static/index-<hash>.css`` (the ``io-button`` / popup / CSS variable
  rules both pages link) is written as ``static/index.css`` (the hash changes per deploy);
- CodeMirror 5.61.0 and its monokai theme come from cdnjs on v10: linked from the local copies in
  ``ui/libs/codemirror/5.61.0/`` (same files, v7.1.4 served them itself);
- KrunkNative additions, kept on every import: the web build's ``<canvas id="canvas">`` (hidden
  on the hub) and ``js/kn-shim.js`` + ``js/loader.js``; the hub's app badges open our Download
  window (``social_page.APP_BADGES``); the editor links ``css/editor-kn.css`` (transparent 3D area
  over the GL scene), ``css/editor-script.css`` (the KrunkScript window) and ``css/editor-ai.css``
  and has the AI panel button ``#knAiOption`` after ``#snappingOption`` (``EDITOR_KN_TOOLBAR``,
  ``client/src/editor/editor_ai.c``).
- v10 dropped ``#twoFAPop`` (the 2-Factor Authentication code popup) from both pages and its rules
  from ``editor.css`` / ``social.css`` (its ``twoFAres`` handler is still in the hub bundle, dead).
  KrunkNative keeps 2FA logins (``client/src/ui/account_ui.c``, brain ``account_security.c``), so
  v7.1.4's popup markup (``KN_TWOFA``) is put back at the start of ``#uiBase`` and its rules are
  written as ``css/kn-twofa.css`` from v7.1.4's ``editor.css`` (linked by both pages).

- the hub's login is v10's Svelte LoginModal on the view stack (``#viewStack``; C:
  ``social_ui.c``): its component stylesheets (``HUB_SVELTE``: the view stack, the popup frame, the
  modal), which the Svelte runtime injects at mount, are written as ``css/hub-svelte.css`` from the
  component CSS ``v10_bundle.js`` extracts from the v10 client (``svelte.css``, ``SVELTE_CSS``) and
  linked after ``static/index.css``.

Stylesheets (``social.css``, ``social_custom.css``, ``editor.css``, ``editor_dark.css``,
``editor_custom.css``, ``pickr.css``) get the v7 import's ``url()`` rewriting. Images the pages and
these stylesheets reference are copied from the mirror when ``ui/`` lacks them; a file ``ui/``
already has with other content is reported, not replaced (the menu page owns those).
"""

import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)

import index_page  # noqa: E402  (rewrite_css, QUERY_RE)
import social_page  # noqa: E402  (APP_BADGES)

DEFAULT_PUBLIC = os.path.expanduser("~/.cache/kn-v10/live/site/public")
PAGES = ("social.html", "editor.html")
PAGE_CSS = {
    "social.html": ("css/social.css", "css/social_custom.css"),
    "editor.html": ("css/editor.css", "css/editor_dark.css", "css/editor_custom.css", "css/pickr.css"),
}
STATIC_CSS_RE = re.compile(r"static/index-[A-Za-z0-9_-]+\.css")
STATIC_CSS = "static/index.css"
CODEMIRROR = (("https://cdnjs.cloudflare.com/ajax/libs/codemirror/5.61.0/codemirror.min.css",
               "libs/codemirror/5.61.0/codemirror.min.css"),
              ("https://cdnjs.cloudflare.com/ajax/libs/codemirror/5.61.0/theme/monokai.min.css",
               "libs/codemirror/5.61.0/theme/monokai.min.css"))
EDITOR_KN_CSS = ("css/editor-kn.css", "css/editor-script.css", "css/editor-ai.css")
EDITOR_KN_TOOLBAR = ('<div id="knAiOption" class="eButton" title="AI assistant"><i class="material-icons" '
                     'style="font-size:15px;vertical-align:-3px;margin-right:3px">auto_awesome</i>AI</div>')
KN_TWOFA = ('<div id="twoFAPop" style="display:none"><div id="twoFAWindow"><div class="twoFAHeader">2-Factor Authentication</div>'
            '<div id="twoFAmsg">Enter Verification Code</div><form id="twoFAForm"></form>'
            '<div id="twoFAbtn" onclick="sub2FA()">Submit</div></div></div>')
KN_TWOFA_CSS = "css/kn-twofa.css"
V7_EDITOR_CSS = "/home/user/jails/kru/src/Krunker_v7.1.4/public/css/editor.css"
HUB_SVELTE = ("sdh4ve", "1wz7ymp", "16z1lxi")  # view stack, popup frame, LoginModal
HUB_SVELTE_CSS = "css/hub-svelte.css"
# v10_bundle.js output, first that exists: the page tree's copy, then the bundle cache
SVELTE_CSS = ("css/svelte.css", "v10/css/svelte.css", os.path.expanduser("~/.cache/kn-v10/bundle/svelte.css"))
DROP_META_RE = re.compile(r"<meta (?:property=\"og:|name=\"(?:twitter:|description|keywords))[^>]*>\s*")
DROP_LINK_RE = re.compile(r"<link rel=\"(?:modulepreload|preload|dns-prefetch|preconnect)\"[^>]*>\s*")


def _relative_urls(html):
    html = index_page.QUERY_RE.sub("", html)
    html = re.sub(r"https?://assets\.krunker\.io/", "", html)
    html = re.sub(r"(href|src)=\"\./", r'\1="', html)
    html = re.sub(r"(href|src)=\"/(?!/)", r'\1="', html)
    return html


def _kn_glue(html, hidden_canvas):
    style = ' style="display:none"' if hidden_canvas else ""
    if 'id="canvas"' not in html:
        html = re.sub(r"(<body[^>]*>)", r'\1\n<canvas id="canvas" tabindex="-1"%s></canvas>' % style, html, count=1)
    if "js/kn-shim.js" not in html:
        html = html.replace("</body>", '<script src="js/kn-shim.js"></script>\n<script src="js/loader.js"></script>\n</body>', 1)
    return html


def transform_page(name, html):
    """v10 page markup -> ui/<name> (module docstring)."""
    html = re.sub(r"<script\b[^>]*>.*?</script>\s*", "", html, flags=re.S | re.I)
    html = re.sub(r"<!--.*?-->\s*", "", html, flags=re.S)
    html = DROP_META_RE.sub("", html)
    html = DROP_LINK_RE.sub("", html)
    html = re.sub(r"\s+crossorigin(?==|\s|>)(?:=\"[^\"]*\")?", "", html)
    for cdn, local in CODEMIRROR:
        html = html.replace(cdn, local)
    html = _relative_urls(html)
    html = STATIC_CSS_RE.sub(STATIC_CSS, html)
    if 'id="twoFAPop"' not in html:
        html, n = re.subn(r'(<div id="uiBase"[^>]*>)', lambda m: m.group(1) + KN_TWOFA, html, count=1)
        if not n:
            raise SystemExit("v10 %s: #uiBase not found (the 2FA popup goes into it)" % name)
        html = html.replace("</head>", '<link rel="stylesheet" href="%s"></head>' % KN_TWOFA_CSS, 1)
    if name == "social.html":
        html = html.replace('<link rel="stylesheet" href="%s">' % STATIC_CSS,
                            '<link rel="stylesheet" href="%s"><link rel="stylesheet" href="%s">' % (STATIC_CSS, HUB_SVELTE_CSS), 1)
        for store, ours in social_page.APP_BADGES:
            html = html.replace(store, ours)
        html = _kn_glue(html, True)
    else:
        links = "".join('<link rel="stylesheet" href="%s">' % css for css in EDITOR_KN_CSS if css not in html)
        html = html.replace("</head>", links + "</head>", 1)
        if 'id="knAiOption"' not in html:
            html, n = re.subn(r'(<div id="snappingOption"[^>]*>[^<]*</div>)', lambda m: m.group(1) + EDITOR_KN_TOOLBAR, html, count=1)
            if not n:
                raise SystemExit("v10 editor.html: #snappingOption not found (the AI panel button goes after it)")
        html = _kn_glue(html, False)
    return html


def hub_svelte_css(ui_dir):
    """The HUB_SVELTE component blocks of v10_bundle.js's svelte.css ("/* svelte-<hash> */" + rules)."""
    for cand in SVELTE_CSS:
        path = cand if os.path.isabs(cand) else os.path.join(ui_dir, cand)
        if os.path.isfile(path):
            break
    else:
        raise SystemExit("v10 hub: no svelte.css (%s): run import.py --ref v10 (v10_bundle.js) first" % ", ".join(SVELTE_CSS))
    with open(path, encoding="utf-8") as fh:
        text = fh.read()
    blocks = re.split(r"(?m)^/\* (svelte-[a-z0-9]+) \*/\n", text)
    found = {blocks[i]: blocks[i + 1].strip() for i in range(1, len(blocks) - 1, 2)}
    out = ["/* KrunkNative: Krunker v10's Svelte component stylesheets for the hub's view stack and LoginModal\n"
           "   (svelte.css of tools/ui-import/v10_bundle.js; tools/ui-import/v10_hub_editor.py HUB_SVELTE) */"]
    for h in HUB_SVELTE:
        css = found.get("svelte-" + h)
        if css is None:
            raise SystemExit("v10 hub: svelte-%s not in %s" % (h, path))
        out.append("/* svelte-%s */\n%s" % (h, css))
    return "\n".join(out) + "\n"


def twofa_css():
    """v7.1.4's #twoFAPop rules (editor.css; social.css has the same eight), for css/kn-twofa.css."""
    with open(V7_EDITOR_CSS, encoding="utf-8") as fh:
        css = fh.read()
    rules = [m.group(0).strip() for m in re.finditer(r"[^{}]*twoFA[^{}]*\{[^}]*\}", css)]
    if len(rules) != 8:
        raise SystemExit("v7.1.4 editor.css: %d #twoFAPop rules (8 expected)" % len(rules))
    return ("/* KrunkNative: Krunker v7.1.4's 2-Factor Authentication popup (css/editor.css), which v10 dropped:\n"
            "   tools/ui-import/v10_hub_editor.py (KN_TWOFA), client/src/ui/account_ui.c */\n" + "\n\n".join(rules) + "\n")


IMG_URL_RES = (re.compile(r"(?:src|href)=[\"']([^\"']+)[\"']"), re.compile(r"url\(\s*['\"]?([^)'\"]+)['\"]?\s*\)"))
IMG_EXT = (".png", ".jpg", ".jpeg", ".gif", ".svg", ".webp", ".ico", ".woff2", ".woff", ".ttf")


def page_refs(ui_dir, files):
    """Local image/font files the given ui-relative files reference (ui-relative, sorted)."""
    found = set()
    for rel in files:
        try:
            with open(os.path.join(ui_dir, rel), encoding="utf-8", errors="replace") as fh:
                text = fh.read()
        except OSError:
            continue
        base = os.path.dirname(rel)
        for rx in IMG_URL_RES:
            for m in rx.finditer(text):
                url = m.group(1).split("?", 1)[0].split("#", 1)[0].strip()
                if not url or ":" in url or url.startswith("//") or not url.lower().endswith(IMG_EXT):
                    continue
                path = os.path.normpath(os.path.join(base, url))
                if not path.startswith(".."):
                    found.add(path)
    return sorted(found)


def import_pages(ui_dir, write_if_changed, log, public=DEFAULT_PUBLIC, pages=PAGES):
    """Writes ui/social.html, ui/editor.html (``pages``), their stylesheets and missing images. Returns files rewritten."""
    if not all(os.path.isfile(os.path.join(public, p)) for p in pages):
        log("v10 hub/editor: %s has no social.html / editor.html (fetch_live_site.py --stage first), skipped" % public)
        return 0
    written, sheets, static_css = 0, [], None
    for name in pages:
        with open(os.path.join(public, name), encoding="utf-8") as fh:
            raw = fh.read()
        m = STATIC_CSS_RE.search(raw)
        if m:
            static_css = m.group(0)
        written += write_if_changed(os.path.join(ui_dir, name), transform_page(name, raw))
        sheets += [s for s in PAGE_CSS[name] if s not in sheets]
    for rel in sheets:
        with open(os.path.join(public, rel), encoding="utf-8") as fh:
            written += write_if_changed(os.path.join(ui_dir, rel), index_page.rewrite_css(fh.read(), os.path.dirname(rel)))
    if static_css and os.path.isfile(os.path.join(public, static_css)):
        with open(os.path.join(public, static_css), encoding="utf-8") as fh:
            written += write_if_changed(os.path.join(ui_dir, STATIC_CSS), index_page.rewrite_css(fh.read(), "static"))
        sheets.append(STATIC_CSS)
    else:
        log("v10 hub/editor: %s not in the mirror, %s not written" % (static_css, STATIC_CSS))
    written += write_if_changed(os.path.join(ui_dir, KN_TWOFA_CSS), twofa_css())
    if "social.html" in pages:
        written += write_if_changed(os.path.join(ui_dir, HUB_SVELTE_CSS), hub_svelte_css(ui_dir))
    for _, local in CODEMIRROR:
        if not os.path.isfile(os.path.join(ui_dir, local)):
            log("v10 hub/editor: warning: ui/%s missing" % local)
    copied, differ, missing = 0, [], []
    for rel in page_refs(ui_dir, list(pages) + sheets):
        dst, src = os.path.join(ui_dir, rel), os.path.join(public, rel)
        if not os.path.isfile(src):
            if not os.path.isfile(dst):
                missing.append(rel)
            continue
        with open(src, "rb") as fh:
            data = fh.read()
        if os.path.isfile(dst):
            with open(dst, "rb") as fh:
                if fh.read() != data:
                    differ.append(rel)
            continue
        copied += write_if_changed(dst, data)
    written += copied
    log("v10 hub/editor: %s + %d stylesheets, %d images copied, %d rewritten" % (" + ".join(pages), len(sheets), copied, written))
    if differ:
        log("v10 hub/editor: kept ui/'s copy of %d file(s) that differ on krunker.io: %s" % (len(differ), ", ".join(differ)))
    if missing:
        log("v10 hub/editor: %d referenced file(s) on neither the mirror nor ui/: %s" % (len(missing), ", ".join(missing)))
    return written


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ui", default=os.path.join(REPO, "ui"))
    ap.add_argument("--public", default=DEFAULT_PUBLIC, help="the live site mirror's public/ (fetch_live_site.py --stage)")
    args = ap.parse_args(argv)
    import importlib.util
    spec = importlib.util.spec_from_file_location("kn_ui_import", os.path.join(HERE, "import.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    import_pages(os.path.abspath(args.ui), mod.write_if_changed, mod.log, os.path.abspath(os.path.expanduser(args.public)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
