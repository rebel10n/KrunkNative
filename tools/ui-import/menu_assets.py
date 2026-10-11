"""Krunker v3.1.1 static menu/HUD images (``make ui-import`` step, loaded by import.py).

Copies every file listed in ``menu-assets.txt`` from the v3.1.1 ``public/`` tree into
``ui/`` at the same relative path, byte-identical (``dest = src`` copies a file the
reference serves elsewhere). The manifest is the complete list of images the menu page,
its window templates and the C UI logic (``client/src/ui/``) reference, plus the other
byte-identical site files (stylesheets, fonts, the changelog); ``{a..b[..step]}`` ranges and
``{x,y}`` lists expand like the shell's brace expansion.

A listed file missing from the reference tree is an error (the manifest must only name
files v3.1.1 serves); a missing reference tree is logged and skipped.
"""

import os
import re

KRUNKER_PUBLIC = "/home/user/jails/kru/src/Krunker_v3.1.1/public"
HERE = os.path.dirname(os.path.abspath(__file__))
MANIFEST = os.path.join(HERE, "menu-assets.txt")

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
            if src:
                entries.append((dest, src))
            else:
                entries.extend((p, p) for p in expand(dest))
    seen = set()
    return [e for e in entries if not (e[0] in seen or seen.add(e[0]))]


def manifest_paths(path=MANIFEST):
    return [dest for dest, _ in manifest_entries(path)]


def import_menu_assets(ui_dir, write_if_changed, log, src_root=KRUNKER_PUBLIC):
    """Returns the number of files rewritten."""
    if not os.path.isdir(src_root):
        log("menu assets: %s not found, skipped" % src_root)
        return 0
    entries = manifest_entries()
    missing = [src for _, src in entries if not os.path.isfile(os.path.join(src_root, src))]
    if missing:
        raise SystemExit("menu-assets.txt names files v3.1.1 does not serve:\n  " + "\n  ".join(missing))
    written = 0
    for dest, src in entries:
        with open(os.path.join(src_root, src), "rb") as fh:
            data = fh.read()
        if write_if_changed(os.path.join(ui_dir, dest), data):
            written += 1
            log("  copied %s%s" % (dest, "" if dest == src else " (" + src + ")"))
    log("menu assets: %d files (menu-assets.txt), %d rewritten" % (len(entries), written))
    return written
