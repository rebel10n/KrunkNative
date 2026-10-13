#!/usr/bin/env python3
"""KrunkNative UI import tool.

Run with ``python3 -I tools/ui-import/import.py --ui ui`` from the repository
root (the Makefile target ``make ui-import`` does exactly that). ``--ref v7`` (the
default) imports Krunker v7.1.4's pages; ``--ref v3`` the former Krunker v3.1.1 import
(``ui/index.html`` hand-maintained, ``krunker-windows.js`` window templates).

With ``--ref v7`` the steps are: the menu page and its stylesheets (``index_page.py``),
the window templates (``windows_v7.py``, from ``krunker-windows-v7.json.gz`` captured by
``capture_v7_windows.js``), the social hub, editor and viewer pages, the fonts and
ligature tables below, and every file the pages reference plus the C UI's own image
list (``menu_assets.py``, ``menu-assets-v7.txt``).

What it does (all steps are idempotent; files are rewritten only when their
content changes):

1. Converts every ``.woff2`` referenced by an ``@font-face`` rule in
   ``<ui>/css/*.css`` that exists on disk to a ``.ttf`` next to it. The output
   name is derived from the ``font-family`` of the rule (``material icons`` ->
   ``material-icons.ttf``). An existing, loadable ``.ttf`` is kept as is
   unless ``--force`` is given, so a file produced by another tool is never
   silently replaced.
2. Writes ``client/src/html/engine/material_icons_liga.h``: a C table of every
   ligature substitution of the Material Icons font (GSUB ligature lookups; the
   woff2 Google serves keeps them under ``rlig``): ligature component glyph
   names -> ligature glyph -> its codepoint through ``cmap``, sorted by name in
   byte order so a ``strcmp`` based binary search works. The table format
   (``MaterialIconLiga material_icons_liga[MATERIAL_ICONS_LIGA_COUNT]`` with
   ``name``, ``codepoint``, ``glyph_index``) is the one ``gen_liga.py`` in this
   directory produces and ``client/src/html/engine/text.c`` consumes; when
   ``gen_liga.py`` is present its collector/writer are used directly so both
   tools emit byte-identical headers.
3. Splits ``<ui>/js/krunker-windows.js`` (``window.KRUNKER_WINDOWS = {...}``)
   into static files under ``<ui>/windows/``: ``<id>.html`` for each window
   body, ``<id>-card.html`` / ``<id>-item.html`` / ``<id>-mapcard.html`` for
   the per-item templates, ``settings-head.html`` and ``settings-tab-N.html``
   for the settings window, and ``windows.json`` as the index. The HTML is
   written byte-identically (UTF-8, no reformatting).
4. Keeps a copy of the source JS at ``tools/ui-import/krunker-windows.js`` so
   that re-runs keep working after ``<ui>/js/krunker-windows.js`` is deleted.

Dependencies: ``fonttools`` and ``brotli`` (``pip install fonttools brotli``).
"""

import argparse
import json
import os
import re
import sys

sys.dont_write_bytecode = True  # no __pycache__ in the tree when gen_liga.py is loaded

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))

FONT_FACE_RE = re.compile(r"@font-face\s*\{(.*?)\}", re.S)
FAMILY_RE = re.compile(r"font-family\s*:\s*(['\"]?)([^;'\"]+)\1\s*;", re.I)
URL_RE = re.compile(r"url\(\s*(['\"]?)([^)'\"]+)\1\s*\)", re.I)

JS_PREFIX_RE = re.compile(r"window\.KRUNKER_WINDOWS\s*=\s*", re.S)

LIGA_HEADER_REL = os.path.join("client", "src", "html", "engine", "material_icons_liga.h")
LIGA_OUTLINED_REL = os.path.join("client", "src", "html", "engine", "material_icons_outlined_liga.h")

REFS = {
    "v3": {"public": "/home/user/jails/kru/src/Krunker_v3.1.1/public", "manifest": "menu-assets.txt"},
    "v7": {"public": "/home/user/jails/kru/src/Krunker_v7.1.4/public", "manifest": "menu-assets-v7.txt"},
    # the live krunker.io site (Krunker v10), mirrored by fetch_live_site.py; written to <ui>/v10/
    "v10": {"public": os.path.expanduser("~/.cache/kn-v10/live/site/public"), "manifest": "menu-assets-v7.txt",
            "client_js": os.path.expanduser("~/Downloads/v10.0.2.js")},
}


def log(msg):
    print("[ui-import] " + msg)


def write_if_changed(path, data):
    """Write bytes to path only when the content differs. Returns True if written."""
    if isinstance(data, str):
        data = data.encode("utf-8")
    try:
        with open(path, "rb") as fh:
            if fh.read() == data:
                return False
    except OSError:
        pass
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "wb") as fh:
        fh.write(data)
    os.replace(tmp, path)
    return True


def slug(family):
    s = re.sub(r"[^a-z0-9]+", "-", family.strip().lower()).strip("-")
    return s or "font"


# ---------------------------------------------------------------------------
# 1. @font-face woff2 -> ttf
# ---------------------------------------------------------------------------

def font_face_woff2s(ui_dir):
    """Yield (family, woff2_path) for each @font-face woff2 found in <ui>/css/*.css."""
    css_dir = os.path.join(ui_dir, "css")
    out = []
    seen = set()
    for name in sorted(os.listdir(css_dir)):
        if not name.endswith(".css"):
            continue
        css_path = os.path.join(css_dir, name)
        with open(css_path, "r", encoding="utf-8", errors="replace") as fh:
            text = fh.read()
        for block in FONT_FACE_RE.finditer(text):
            body = block.group(1)
            fam = FAMILY_RE.search(body)
            family = fam.group(2).strip() if fam else "font"
            for url in URL_RE.finditer(body):
                rel = url.group(2).strip()
                if not rel.lower().endswith(".woff2"):
                    continue
                path = os.path.normpath(os.path.join(os.path.dirname(css_path), rel))
                if path in seen:
                    continue
                seen.add(path)
                out.append((family, path))
    return out


def load_font(path):
    from fontTools.ttLib import TTFont

    return TTFont(path)


def convert_woff2(family, woff2_path, force):
    """Convert one woff2 to a .ttf next to it. Returns the ttf path or None."""
    if not os.path.exists(woff2_path):
        log("skip %s: %s does not exist" % (family, os.path.relpath(woff2_path, REPO)))
        return None
    # Material icon fonts get their family's name (the engine's fallback for their opaque
    # Google file names); other faces (several unicode-range subsets per family, e.g. v7's
    # "play") keep the woff2's own name, which the engine tries first.
    name = slug(family) if slug(family).startswith("material-") else os.path.splitext(os.path.basename(woff2_path))[0]
    ttf_path = os.path.join(os.path.dirname(woff2_path), name + ".ttf")
    if os.path.exists(ttf_path) and not force:
        try:
            existing = load_font(ttf_path)
            n = existing["maxp"].numGlyphs
            existing.close()
            log("keep %s (%d glyphs, already present)" % (os.path.relpath(ttf_path, REPO), n))
            return ttf_path
        except Exception as exc:  # noqa: BLE001 - any unreadable file is regenerated
            log("existing %s is not loadable (%s); regenerating" % (ttf_path, exc))
    font = load_font(woff2_path)
    font.flavor = None  # plain sfnt (TrueType/OpenType), no WOFF wrapper
    tmp = ttf_path + ".tmp"
    font.save(tmp)
    font.close()
    os.replace(tmp, ttf_path)
    log("wrote %s (%d bytes) from %s" % (
        os.path.relpath(ttf_path, REPO), os.path.getsize(ttf_path),
        os.path.relpath(woff2_path, REPO)))
    return ttf_path


# ---------------------------------------------------------------------------
# 2. Material Icons ligature table
# ---------------------------------------------------------------------------

def _lookup_subtables(gsub, index):
    """Return the LigatureSubst subtables of lookup `index`, unwrapping extensions."""
    lookup = gsub.LookupList.Lookup[index]
    subs = []
    for st in lookup.SubTable:
        if lookup.LookupType == 7:  # Extension
            st = st.ExtSubTable
        if getattr(st, "LookupType", lookup.LookupType) == 4 and hasattr(st, "ligatures"):
            subs.append(st)
    return subs


def _load_gen_liga():
    """Load tools/ui-import/gen_liga.py (same directory) as a module, if present."""
    path = os.path.join(HERE, "gen_liga.py")
    if not os.path.exists(path):
        return None
    import importlib.util

    spec = importlib.util.spec_from_file_location("kn_gen_liga", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _lookup_subtables(gsub, index):
    """Return the LigatureSubst subtables of lookup `index`, unwrapping extensions."""
    lookup = gsub.LookupList.Lookup[index]
    subs = []
    for st in lookup.SubTable:
        if lookup.LookupType == 7:  # Extension
            st = st.ExtSubTable
        if getattr(st, "ligatures", None) is not None:
            subs.append(st)
    return subs


def collect_ligatures(font):
    """Return {name: (codepoint, glyph_index)} for every ligature substitution.

    Fallback used when gen_liga.py is absent; mirrors its behaviour: every
    ligature lookup of the font, lowest codepoint per glyph through cmap, first
    name wins unless it had no codepoint.
    """
    rev = {}
    for cp, gname in font.getBestCmap().items():
        if gname not in rev or cp < rev[gname]:
            rev[gname] = cp
    order = font.getGlyphOrder()
    ligs = {}
    if "GSUB" not in font:
        return ligs
    gsub = font["GSUB"].table
    for li in range(len(gsub.LookupList.Lookup)):
        for st in _lookup_subtables(gsub, li):
            for first, entries in st.ligatures.items():
                for lig in entries:
                    comps = [first] + list(lig.Component)
                    try:
                        name = "".join(chr(rev[g]) for g in comps)
                    except KeyError:
                        continue  # component glyph without a codepoint
                    cp = rev.get(lig.LigGlyph, 0)
                    gid = order.index(lig.LigGlyph)
                    if name not in ligs or (cp and not ligs[name][0]):
                        ligs[name] = (cp, gid)
    return ligs


def render_header(ligs, font_name, generator="tools/ui-import/import.py"):
    """Same layout as gen_liga.write_header (kept in sync by hand)."""
    names = sorted(ligs)
    out = []
    out.append("/* GENERATED by %s from %s. DO NOT EDIT. */\n" % (generator, font_name))
    out.append("#pragma once\n#include <stdint.h>\n\n")
    out.append("/* Material Icons ligature names -> PUA codepoint (0 if the ligature glyph is\n"
               " * unmapped; use glyph_index then).  Sorted by name (strcmp order). */\n")
    out.append("typedef struct MaterialIconLiga {\n    const char *name;\n    uint32_t codepoint;\n"
               "    uint32_t glyph_index;\n} MaterialIconLiga;\n\n")
    out.append("#define MATERIAL_ICONS_LIGA_COUNT %d\n" % len(names))
    out.append("#define MATERIAL_ICONS_LIGA_MAX_NAME %d\n\n" % max(len(n) for n in names))
    out.append("static const MaterialIconLiga material_icons_liga[MATERIAL_ICONS_LIGA_COUNT] = {\n")
    for n in names:
        cp, gid = ligs[n]
        out.append('    {"%s", 0x%04x, %d},\n' % (n.replace("\\", "\\\\").replace('"', '\\"'), cp, gid))
    out.append("};\n")
    return "".join(out)


def rename_liga_header(text, prefix):
    """The Material Icons header with its table renamed to `prefix` (``material_icons_outlined``):
    the struct typedef comes from material_icons_liga.h."""
    upper = prefix.upper()
    text = re.sub(r"typedef struct MaterialIconLiga \{.*?\} MaterialIconLiga;\n\n",
                  '#include "material_icons_liga.h" /* MaterialIconLiga */\n\n', text, flags=re.S)
    text = text.replace("MATERIAL_ICONS_LIGA_", upper + "_LIGA_").replace("material_icons_liga[", prefix + "_liga[")
    return text.replace("/* Material Icons ligature names", "/* %s ligature names" % prefix.replace("_", " ").title())


def generate_liga_header(font_path, header_path, prefix=None):
    """Write the ligature header from `font_path` (the woff2). Returns (count, changed)."""
    gen = _load_gen_liga()
    font = load_font(font_path)
    font_name = os.path.basename(font_path)
    if gen is not None:
        ligs = gen.collect_ligatures(font)
        tmp = header_path + ".gen.tmp"
        os.makedirs(os.path.dirname(header_path) or ".", exist_ok=True)
        gen.write_header(tmp, ligs, font_name)
        with open(tmp, "rb") as fh:
            text = fh.read()
        os.remove(tmp)
    else:
        ligs = collect_ligatures(font)
        text = render_header(ligs, font_name).encode("utf-8")
    font.close()
    if not ligs:
        raise SystemExit("no ligatures found in %s" % font_path)
    if prefix:
        text = rename_liga_header(text.decode("utf-8"), prefix).encode("utf-8")
    return len(ligs), write_if_changed(header_path, text)


# ---------------------------------------------------------------------------
# 3. krunker-windows.js -> ui/windows/
# ---------------------------------------------------------------------------

def parse_windows_js(text):
    m = JS_PREFIX_RE.search(text)
    if not m:
        raise SystemExit("krunker-windows.js: `window.KRUNKER_WINDOWS =` not found")
    body = text[m.end():].rstrip()
    if body.endswith(";"):
        body = body[:-1].rstrip()
    # The object literal is strict JSON (generated); keep key order.
    return json.loads(body)


TEMPLATE_KEYS = (("card", "card"), ("item", "item"), ("mapCard", "mapcard"))


def split_windows(data, out_dir):
    index = {"windows": {}, "settings": {}}
    written = 0
    files = []

    def emit(name, html):
        nonlocal written
        path = os.path.join(out_dir, name)
        if write_if_changed(path, html):
            written += 1
        files.append((name, len(html.encode("utf-8"))))

    for wid, win in data.get("windows", {}).items():
        entry = {}
        entry["header"] = win.get("header", "")
        entry["sticky"] = bool(win.get("sticky", False))
        html_name = "%s.html" % wid
        emit(html_name, win.get("html", ""))
        entry["file"] = html_name
        for key, suffix in TEMPLATE_KEYS:
            if key in win:
                tname = "%s-%s.html" % (wid, suffix)
                emit(tname, win[key])
                entry[key] = tname
        for key, value in win.items():
            if key in ("header", "sticky", "html", "card", "item", "mapCard"):
                continue
            if isinstance(value, (int, float, bool, str)) or value is None:
                entry[key] = value  # pass-through scalars such as itemCount
        index["windows"][wid] = entry

    settings = data.get("settings")
    if settings is not None:
        s = {}
        if "header" in settings:
            s["header"] = settings["header"]
        if "sticky" in settings:
            s["sticky"] = bool(settings["sticky"])
        emit("settings-head.html", settings.get("head", ""))
        s["head"] = "settings-head.html"
        tabs = []
        for i, tab in enumerate(settings.get("tabs", [])):
            tname = "settings-tab-%d.html" % i
            emit(tname, tab)
            tabs.append(tname)
        s["tabs"] = tabs
        index["settings"] = s

    index_text = json.dumps(index, indent=2, ensure_ascii=False) + "\n"
    if write_if_changed(os.path.join(out_dir, "windows.json"), index_text):
        written += 1
    files.append(("windows.json", len(index_text.encode("utf-8"))))
    return index, files, written


def resolve_windows_source(ui_dir):
    live = os.path.join(ui_dir, "js", "krunker-windows.js")
    copy = os.path.join(HERE, "krunker-windows.js")
    if os.path.exists(live):
        with open(live, "rb") as fh:
            data = fh.read()
        if write_if_changed(copy, data):
            log("copied %s -> %s" % (os.path.relpath(live, REPO), os.path.relpath(copy, REPO)))
        return live, data
    if os.path.exists(copy):
        log("using archived source %s (%s is gone)" % (os.path.relpath(copy, REPO), os.path.relpath(live, REPO)))
        with open(copy, "rb") as fh:
            return copy, fh.read()
    raise SystemExit("no krunker-windows.js found at %s or %s" % (live, copy))


# ---------------------------------------------------------------------------

def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ui", default=os.path.join(REPO, "ui"), help="ui directory (default: <repo>/ui)")
    ap.add_argument("--ref", choices=sorted(REFS), default="v7", help="Krunker reference to import (default v7; v10 = the live site into <ui>/v10/)")
    ap.add_argument("--public", default=None, help="the reference's public/ tree (default per --ref)")
    ap.add_argument("--client-js", default=None,
                    help="v10: the live client script, parsed (never run) by v10_bundle.js for the Svelte menu")
    ap.add_argument("--viewer-store", action="store_true",
                    help="also regenerate client/src/viewer/viewer_store_table.c (v3 always does when node is there)")
    ap.add_argument("--windows-out", default=None, help="output dir for window files (default: <ui>/windows)")
    ap.add_argument("--liga-header", default=os.path.join(REPO, LIGA_HEADER_REL),
                    help="generated C header path (default: %s)" % LIGA_HEADER_REL)
    ap.add_argument("--force", action="store_true", help="regenerate .ttf files even if present")
    ap.add_argument("--no-fonts", action="store_true", help="skip font conversion and the liga table")
    ap.add_argument("--no-windows", action="store_true", help="skip the window split")
    ap.add_argument("--no-editor", action="store_true", help="skip the map editor page (editor_page.py)")
    ap.add_argument("--editor-page", action="store_true",
                    help="(no-op, kept for old scripts: the v7 editor page is imported by default since editor WP2)")
    ap.add_argument("--no-social", action="store_true", help="skip the social hub page (social_page.py)")
    ap.add_argument("--no-viewer", action="store_true", help="skip the model viewer page (viewer_page.py)")
    ap.add_argument("--no-assets", action="store_true", help="skip the menu/HUD images (menu_assets.py, menu-assets.txt)")
    args = ap.parse_args(argv)

    ui_dir = os.path.abspath(args.ui)
    if not os.path.isdir(ui_dir):
        raise SystemExit("ui dir not found: " + ui_dir)
    windows_out = os.path.abspath(args.windows_out or os.path.join(ui_dir, "windows"))
    ref = REFS[args.ref]
    public = os.path.abspath(args.public or ref["public"])
    v7 = args.ref == "v7"
    sys.path.insert(0, HERE)
    if args.ref == "v10":
        return import_v10(args, ui_dir, public, ref)
    if v7:
        # the page first: fonts, windows and assets read what it links
        import index_page  # tools/ui-import/index_page.py
        index_page.import_index(ui_dir, write_if_changed, log, public)
        index_page.import_shared_css(ui_dir, write_if_changed, log, public)

    if not args.no_fonts:
        try:
            import fontTools  # noqa: F401
            import brotli  # noqa: F401
        except ImportError as exc:
            raise SystemExit("missing dependency (%s): pip install fonttools brotli" % exc)
        os.environ.setdefault("SOURCE_DATE_EPOCH", "0")  # deterministic head.modified in saved TTFs
        if v7:
            # every font file an imported stylesheet names (the woff2s are converted below)
            for family, woff2 in font_face_woff2s(ui_dir):
                rel = os.path.relpath(woff2, ui_dir)
                src = os.path.join(public, rel)
                if os.path.isfile(src):
                    with open(src, "rb") as fh:
                        write_if_changed(woff2, fh.read())
        material_woff2 = outlined_woff2 = None
        for family, woff2 in font_face_woff2s(ui_dir):
            ttf = convert_woff2(family, woff2, args.force)
            if ttf and slug(family) == "material-icons":
                material_woff2 = woff2
            if ttf and slug(family) == "material-icons-outlined":
                outlined_woff2 = woff2
        if material_woff2 is None:
            log("warning: no Material Icons font found; liga header not generated")
        else:
            count, changed = generate_liga_header(material_woff2, os.path.abspath(args.liga_header))
            log("%s %s: %d ligatures" % ("wrote" if changed else "unchanged",
                                         os.path.relpath(args.liga_header, REPO), count))
        if outlined_woff2 is not None:
            header = os.path.join(REPO, LIGA_OUTLINED_REL)
            count, changed = generate_liga_header(outlined_woff2, header, "material_icons_outlined")
            log("%s %s: %d ligatures" % ("wrote" if changed else "unchanged", LIGA_OUTLINED_REL, count))

    if not args.no_windows and v7:
        import windows_v7  # tools/ui-import/windows_v7.py
        windows_v7.split_windows_v7(windows_out, write_if_changed, log)
        with open(os.path.join(public, "docs", "versions.txt"), "rb") as fh:  # the Changelog source
            write_if_changed(os.path.join(windows_out, "versions.txt"), fh.read())
    elif not args.no_windows:
        src, data = resolve_windows_source(ui_dir)
        parsed = parse_windows_js(data.decode("utf-8"))
        index, files, written = split_windows(parsed, windows_out)
        total = sum(size for _, size in files)
        log("%s -> %s: %d files (%d bytes), %d rewritten" % (
            os.path.relpath(src, REPO), os.path.relpath(windows_out, REPO), len(files), total, written))
        for name, size in files:
            log("  %-24s %8d bytes" % (name, size))

    if not args.no_editor:
        import editor_page  # Krunker's editor.html + its stylesheets (tools/ui-import/editor_page.py)
        editor_page.import_editor(ui_dir, write_if_changed, log, public, v7)
    if not args.no_social:
        import social_page  # Krunker's social.html + its stylesheets/images (tools/ui-import/social_page.py)
        social_page.import_social(ui_dir, write_if_changed, log, public, v7)
    if not args.no_viewer:
        import viewer_page  # Krunker's viewer.html, dat.gui's stylesheet, the viewer store tables (tools/ui-import/viewer_page.py)
        viewer_page.import_viewer(ui_dir, write_if_changed, log, public, store_table=args.viewer_store or not v7)
    if not args.no_assets:
        import menu_assets  # every image the pages and the C UI reference (tools/ui-import/menu-assets*.txt)
        menu_assets.import_menu_assets(ui_dir, write_if_changed, log, public, os.path.join(HERE, ref["manifest"]), auto=v7)
    return 0


def import_v10(args, ui_dir, public, ref):
    """--ref v10: the live page into <ui>/v10/ (v10_page.py), its fonts (+ .ttf) and every file the
    page, its stylesheets and the C UI's image list reference, from the mirror. The material icon
    fonts are byte-identical to v7's, so the ligature headers are not regenerated."""
    import menu_assets
    import v10_page
    client_js = args.client_js or ref.get("client_js")
    if client_js and not os.path.isfile(client_js):
        log("v10: %s not found, the Svelte mounts stay empty" % client_js)
        client_js = None
    out = v10_page.import_v10(ui_dir, write_if_changed, log, public, client_js)
    if not args.no_fonts:
        try:
            import fontTools  # noqa: F401
            import brotli  # noqa: F401
        except ImportError as exc:
            raise SystemExit("missing dependency (%s): pip install fonttools brotli" % exc)
        os.environ.setdefault("SOURCE_DATE_EPOCH", "0")
        for family, woff2 in font_face_woff2s(out):
            rel = os.path.relpath(woff2, out)
            src = os.path.join(public, rel)
            if not os.path.isfile(src):
                src = os.path.join(ui_dir, rel)  # the live site 404s on it (e.g. Play subsets): v7's copy
            if os.path.isfile(src):
                with open(src, "rb") as fh:
                    write_if_changed(woff2, fh.read())
            convert_woff2(family, woff2, args.force)
    if not args.no_assets:
        known = set()
        for dest, _ in menu_assets.manifest_entries(os.path.join(HERE, ref["manifest"])):
            known.add(dest)
        entries = [(rel, rel) for rel, _ in menu_assets.collect_page_refs(out, pages=v10_page.PAGES)]
        entries += [(d, d) for d in sorted(known)]
        copied = missing = 0
        seen = set()
        for dest, src in entries:
            if dest in seen:
                continue
            seen.add(dest)
            path = os.path.join(public, src)
            if not os.path.isfile(path):
                fallback = os.path.join(ui_dir, src)  # the live hosts no longer serve it: v7's copy
                if not os.path.isfile(fallback):
                    missing += 1
                    continue
                path = fallback
            with open(path, "rb") as fh:
                copied += write_if_changed(os.path.join(out, dest), fh.read())
        log("v10: page files: %d referenced, %d rewritten, %d on neither host nor in ui/" % (len(seen), copied, missing))
        # the native engine has no WebP decoder: <name>.webp.png sidecars (as game_assets.py --v7 does)
        import fetch_live_assets
        sidecars = 0
        for root, _, names in os.walk(out):
            for name in names:
                if name.endswith(".webp") and fetch_live_assets.webp_sidecar(os.path.join(root, name)):
                    sidecars += 1
        if sidecars:
            log("v10: %d .webp.png sidecars written" % sidecars)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
