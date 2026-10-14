"""Krunker static menu/HUD images (``make ui-import`` step, loaded by import.py).

Copies every file listed in the manifest (``menu-assets-v7.txt``) from the reference
``public/`` tree into ``ui/`` at the
same relative path, byte-identical (``dest = src`` copies a file the reference serves
elsewhere). With ``--ref v7`` every local file the imported page itself references
(``src``/``href``/``url()`` of ``ui/index.html``, ``ui/social.html``, the stylesheets and
the window templates) is collected automatically too (``collect_page_refs``); the
manifest then only lists what the C UI generates. The manifest is the complete list of images the menu page,
its window templates and the C UI logic (``client/src/ui/``) reference, plus the other
byte-identical site files (stylesheets, fonts, the changelog); ``{a..b[..step]}`` ranges and
``{x,y}`` lists expand like the shell's brace expansion.

A listed file missing from the reference tree is an error (the manifest must only name
files the reference serves); a missing reference tree is logged and skipped.
"""

import os
import re

KRUNKER_PUBLIC = "/home/user/jails/kru/src/Krunker_v7.1.4/public"
HERE = os.path.dirname(os.path.abspath(__file__))
MANIFEST = os.path.join(HERE, "menu-assets-v7.txt")

_BRACE = re.compile(r"\{([^{}]*)\}")


def expand(pattern):
    """Shell-style brace expansion: {0..14}, {1..99..2} (integers, may be negative) and {a,b,c}."""
    m = _BRACE.search(pattern)
    if not m:
        return [pattern]
    body = m.group(1)
    rng = re.fullmatch(r"(-?\d+)\.\.(-?\d+)(?:\.\.(\d+))?", body)
    if rng:
        lo, hi = int(rng.group(1)), int(rng.group(2))
        step = int(rng.group(3) or 1) * (1 if hi >= lo else -1)
        items = [str(i) for i in range(lo, hi + (1 if step > 0 else -1), step)]
    else:
        items = body.split(",")
    out = []
    for item in items:
        out.extend(expand(pattern[:m.start()] + item + pattern[m.end():]))
    return out


def manifest_entries(path=MANIFEST):
    """[(dest, src)] relative to ui/ and public/, in manifest order, without duplicates."""
    entries = []
    with open(path, "r", encoding="utf-8") as fh:
        for line in fh:
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            dest, _, src = (part.strip() for part in line.partition("="))
            if src == "v3:":  # `pattern = v3:`: every expanded path from the v3.1.1 tree
                entries.extend((p, "v3:" + p) for p in expand(dest))
            elif src:
                entries.append((dest, src))
            else:
                entries.extend((p, p) for p in expand(dest))
    seen = set()
    return [e for e in entries if not (e[0] in seen or seen.add(e[0]))]


def manifest_paths(path=MANIFEST):
    return [dest for dest, _ in manifest_entries(path)]


_REF_RES = (re.compile(r"""(?:src|href)\s*=\s*(?:"([^"]*)"|'([^']*)')""", re.I),
            re.compile(r"""url\(\s*(?:&quot;|["'])?([^)"'&]+?)(?:&quot;|["'])?\s*\)""", re.I))
_IMAGE_EXT = (".png", ".jpg", ".jpeg", ".gif", ".svg", ".webp", ".ttf", ".woff", ".woff2", ".otf", ".ico", ".mp4", ".webm")


def collect_page_refs(ui_dir, pages=("index.html", "social.html", "viewer.html"), css_skip=("krunknative.css", "hud.css")):
    """Local files referenced by the imported pages, their stylesheets and ui/windows/*.html:
    [(rel path under ui/, referencing file)], deduplicated, in discovery order."""
    found, seen = [], set()

    def scan(path, base_dir):
        try:
            with open(path, "r", encoding="utf-8", errors="replace") as fh:
                text = fh.read()
        except OSError:
            return
        for rx in _REF_RES:
            for m in rx.finditer(text):
                url = next((g for g in m.groups() if g), "").strip()
                url = url.split("?", 1)[0].split("#", 1)[0]
                if not url or ":" in url or url.startswith("//") or "${" in url or "+" in url:
                    continue
                if not url.lower().endswith(_IMAGE_EXT):
                    continue
                rel = os.path.normpath(os.path.join(base_dir, url.lstrip("/")) if not url.startswith("/") else url.lstrip("/"))
                if rel.startswith(".."):
                    continue
                if rel not in seen:
                    seen.add(rel)
                    found.append((rel, os.path.relpath(path, ui_dir)))

    for page in pages:
        scan(os.path.join(ui_dir, page), "")
    css_dir = os.path.join(ui_dir, "css")
    for root, _, names in os.walk(css_dir):
        for name in sorted(names):
            if name.endswith(".css") and name not in css_skip:
                scan(os.path.join(root, name), os.path.relpath(root, ui_dir))
    win_dir = os.path.join(ui_dir, "windows")
    if os.path.isdir(win_dir):
        for name in sorted(os.listdir(win_dir)):
            if name.endswith(".html"):
                scan(os.path.join(win_dir, name), "")  # window markup lives in index.html's #menuWindow
    return found


V3_PUBLIC = "/home/user/jails/kru/src/Krunker_v3.1.1/public"


def _src_path(src_root, src):
    """`v3:<path>` names a file of the v3.1.1 tree (one the v7.1.4 reconstruction lacks)."""
    return os.path.join(V3_PUBLIC, src[3:]) if src.startswith("v3:") else os.path.join(src_root, src)


def import_menu_assets(ui_dir, write_if_changed, log, src_root=KRUNKER_PUBLIC, manifest=MANIFEST, auto=False,
                       known_missing=()):
    """Returns the number of files rewritten."""
    if not os.path.isdir(src_root):
        log("menu assets: %s not found, skipped" % src_root)
        return 0
    entries = manifest_entries(manifest)
    missing = [src for _, src in entries if not os.path.isfile(_src_path(src_root, src))]
    if missing:
        raise SystemExit("%s names files the reference does not serve:\n  " % os.path.basename(manifest) + "\n  ".join(missing))
    if auto:
        listed = {dest for dest, _ in entries}
        absent = []
        for rel, where in collect_page_refs(ui_dir):
            if rel in listed:
                continue
            if os.path.isfile(os.path.join(src_root, rel)):
                entries.append((rel, rel))
                listed.add(rel)
            elif not os.path.isfile(os.path.join(ui_dir, rel)) and rel not in known_missing:
                absent.append("%s (%s)" % (rel, where))
        if absent:
            log("menu assets: %d page references the reference does not serve either (404 there too):" % len(absent))
            by_dir = {}
            for a in absent:
                by_dir.setdefault(os.path.dirname(a.split(" ")[0]), []).append(a)
            for d, items in sorted(by_dir.items()):
                if len(items) > 3:
                    log("  404 %s/ x%d (e.g. %s)" % (d, len(items), items[0]))
                else:
                    for a in items:
                        log("  404 " + a)
    written = 0
    for dest, src in entries:
        with open(_src_path(src_root, src), "rb") as fh:
            data = fh.read()
        if write_if_changed(os.path.join(ui_dir, dest), data):
            written += 1
            log("  copied %s%s" % (dest, "" if dest == src else " (" + src + ")"))
    log("menu assets: %d files (%s%s), %d rewritten" % (len(entries), os.path.basename(manifest),
                                                         " + page references" if auto else "", written))
    return written
