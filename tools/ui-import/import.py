#!/usr/bin/env python3
"""KrunkNative UI import tool.

Run with ``python3 -I tools/ui-import/import.py --ui ui`` from the repository
root (the Makefile target ``make ui-import`` does exactly that).

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
    ttf_path = os.path.join(os.path.dirname(woff2_path), slug(family) + ".ttf")
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


def generate_liga_header(font_path, header_path):
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
    ap.add_argument("--windows-out", default=None, help="output dir for window files (default: <ui>/windows)")
    ap.add_argument("--liga-header", default=os.path.join(REPO, LIGA_HEADER_REL),
                    help="generated C header path (default: %s)" % LIGA_HEADER_REL)
    ap.add_argument("--force", action="store_true", help="regenerate .ttf files even if present")
    ap.add_argument("--no-fonts", action="store_true", help="skip font conversion and the liga table")
    ap.add_argument("--no-windows", action="store_true", help="skip the window split")
    ap.add_argument("--no-editor", action="store_true", help="skip the map editor page (editor_page.py)")
    ap.add_argument("--no-social", action="store_true", help="skip the social hub page (social_page.py)")
    ap.add_argument("--no-viewer", action="store_true", help="skip the model viewer page (viewer_page.py)")
    ap.add_argument("--no-assets", action="store_true", help="skip the menu/HUD images (menu_assets.py, menu-assets.txt)")
    args = ap.parse_args(argv)

    ui_dir = os.path.abspath(args.ui)
    if not os.path.isdir(ui_dir):
        raise SystemExit("ui dir not found: " + ui_dir)
    windows_out = os.path.abspath(args.windows_out or os.path.join(ui_dir, "windows"))

    if not args.no_fonts:
        try:
            import fontTools  # noqa: F401
            import brotli  # noqa: F401
        except ImportError as exc:
            raise SystemExit("missing dependency (%s): pip install fonttools brotli" % exc)
        os.environ.setdefault("SOURCE_DATE_EPOCH", "0")  # deterministic head.modified in saved TTFs
        material_woff2 = None
        for family, woff2 in font_face_woff2s(ui_dir):
            ttf = convert_woff2(family, woff2, args.force)
            if ttf and slug(family) == "material-icons":
                material_woff2 = woff2
        if material_woff2 is None:
            log("warning: no Material Icons font found; liga header not generated")
        else:
            count, changed = generate_liga_header(material_woff2, os.path.abspath(args.liga_header))
            log("%s %s: %d ligatures" % ("wrote" if changed else "unchanged",
                                         os.path.relpath(args.liga_header, REPO), count))

    if not args.no_windows:
        src, data = resolve_windows_source(ui_dir)
        parsed = parse_windows_js(data.decode("utf-8"))
        index, files, written = split_windows(parsed, windows_out)
        total = sum(size for _, size in files)
        log("%s -> %s: %d files (%d bytes), %d rewritten" % (
            os.path.relpath(src, REPO), os.path.relpath(windows_out, REPO), len(files), total, written))
        for name, size in files:
            log("  %-24s %8d bytes" % (name, size))

    if not args.no_assets:
        sys.path.insert(0, HERE)
        import menu_assets  # every image the menu/HUD references (tools/ui-import/menu-assets.txt)
        menu_assets.import_menu_assets(ui_dir, write_if_changed, log)
    if not args.no_editor:
        sys.path.insert(0, HERE)
        import editor_page  # Krunker's editor.html + its stylesheets (tools/ui-import/editor_page.py)
        editor_page.import_editor(ui_dir, write_if_changed, log)
    if not args.no_social:
        sys.path.insert(0, HERE)
        import social_page  # Krunker's social.html + its stylesheets/images (tools/ui-import/social_page.py)
        social_page.import_social(ui_dir, write_if_changed, log)
    if not args.no_viewer:
        sys.path.insert(0, HERE)
        import viewer_page  # Krunker's viewer.html, dat.gui's stylesheet, the viewer store tables (tools/ui-import/viewer_page.py)
        viewer_page.import_viewer(ui_dir, write_if_changed, log)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
