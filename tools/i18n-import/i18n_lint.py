#!/usr/bin/env python3
"""Checks the C UI's localised strings against the tables, and finds keys.

Every user-visible string the C UI writes goes through ``I18N("key", "English")`` or
``I18N_F(buf, "key", "English {0}", args...)`` (client/include/i18n.h). This tool reads those
pairs from client/src and checks them:

  - a Krunker key (anything but ``kn.*``) exists in ui/i18n/en.json, and the English given in the
    C is the table's value (whitespace folded; a mismatch means the C shows other text than v10);
  - a ``kn.*`` key (a string Krunker Box Office added) has one English text everywhere it is used,
    and ui/i18n/kn/en.json has exactly the kn.* keys the C uses, with that text
    (``--write-kn`` regenerates the file from the C);
  - no ``kn.*`` key shadows a Krunker key, and every ui/i18n/kn/<code>.json only has keys of kn/en.json.

Pairs are read anywhere in the source, comments included: a table whose keys are built at run time
(``cs_kn(row, "name", english)`` in ui_client_settings.c, download_ui.c's package descriptions,
ui_controls.c's KrunkNative rows) lists every key it builds as ``// I18N("kn....", "English")`` lines
next to the table, which is how ``--write-kn`` knows them. Keep those lines in step with the table.

    python3 -I tools/i18n-import/i18n_lint.py [--root .]          # check; exit 1 on errors
    python3 -I tools/i18n-import/i18n_lint.py --write-kn          # regenerate ui/i18n/kn/en.json
    python3 -I tools/i18n-import/i18n_lint.py --find "Click to Play"   # keys with that English text
    python3 -I tools/i18n-import/i18n_lint.py --key app.play            # a key in every language
"""
import argparse
import json
import os
import re
import sys

ESC = {"n": "\n", "t": "\t", "r": "\r", "\\": "\\", '"': '"', "'": "'", "0": "\0", "a": "\a", "b": "\b",
       "f": "\f", "v": "\v", "?": "?"}


def c_literals(src, i):
    """adjacent C string literals from src[i] (at a '"'): (value, end index) or (None, i)"""
    out = bytearray()
    n = len(src)
    found = False
    while True:
        j = i
        while j < n and src[j] in " \t\r\n":
            j += 1
        if j >= n or src[j] != '"':
            break
        found = True
        j += 1
        while j < n and src[j] != '"':
            c = src[j]
            if c == "\\":
                e = src[j + 1]
                if e == "x":
                    m = re.match(r"[0-9a-fA-F]+", src[j + 2:])
                    out += bytes([int(m.group(0), 16) & 255])
                    j += 2 + len(m.group(0))
                    continue
                if e in "01234567":
                    m = re.match(r"[0-7]{1,3}", src[j + 1:])
                    out += bytes([int(m.group(0), 8) & 255])
                    j += 1 + len(m.group(0))
                    continue
                if e == "u":
                    out += chr(int(src[j + 2:j + 6], 16)).encode()
                    j += 6
                    continue
                out += ESC.get(e, e).encode()
                j += 2
                continue
            out += c.encode()
            j += 1
        i = j + 1
    return (out.decode("utf-8", "replace"), i) if found else (None, i)


CALL = re.compile(r"\bI18N(_F)?\s*\(|\bi18n_trf\s*\(")


def scan(root):
    """[(file, line, key, english, is_format)]"""
    uses = []
    for base in ("client/src", "client/include", "shared/src"):
        for d, _, files in os.walk(os.path.join(root, base)):
            for f in sorted(files):
                if not f.endswith((".c", ".h")):
                    continue
                path = os.path.join(d, f)
                with open(path, encoding="utf-8", errors="replace") as fh:
                    src = fh.read()
                for m in CALL.finditer(src):
                    i = m.end()
                    # I18N_F(buf, "key", "english", ...) skips the buffer argument; i18n_trf(buf, size, "key", ...)
                    # (the function behind I18N_F, called directly) skips the buffer and its size
                    skip = 2 if m.group(0).startswith("i18n_trf") else (1 if m.group(1) else 0)
                    for _ in range(skip):
                        depth = 0
                        while i < len(src):
                            c = src[i]
                            if c in "([":
                                depth += 1
                            elif c in ")]":
                                depth -= 1
                            elif c == "," and depth == 0:
                                break
                            i += 1
                        i += 1
                    key, i = c_literals(src, i)
                    if key is None:
                        continue  # a macro definition or a computed key
                    while i < len(src) and src[i] in " \t\r\n":
                        i += 1
                    if i >= len(src) or src[i] != ",":
                        continue
                    english, i = c_literals(src, i + 1)
                    if english is None:
                        continue
                    line = src.count("\n", 0, m.start()) + 1
                    uses.append((os.path.relpath(path, root), line, key, english, m.group(0).startswith("i18n_trf") or bool(m.group(1))))
    return uses


def fold(s):
    return re.sub(r"\s+", " ", s).strip()


def load(path):
    with open(path, encoding="utf-8") as fh:
        return json.load(fh)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", default=".")
    ap.add_argument("--write-kn", action="store_true")
    ap.add_argument("--find")
    ap.add_argument("--key")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()
    ui = os.path.join(args.root, "ui", "i18n")
    en = load(os.path.join(ui, "en.json"))
    langs = sorted(f[:-5] for f in os.listdir(ui) if f.endswith(".json") and f != "en.json")
    tables = {c: load(os.path.join(ui, c + ".json")) for c in langs}

    if args.find:
        want = fold(args.find).lower()
        hits = [k for k, v in en.items() if fold(v).lower() == want]
        part = [k for k, v in en.items() if want in fold(v).lower() and k not in hits][:20]
        for k in hits:
            print("%-48s = %r  (%d/%d languages)" % (k, en[k], sum(1 for t in tables.values() if t.get(k)), len(tables)))
        if part:
            print("-- containing it:")
            for k in part:
                print("%-48s = %r" % (k, en[k][:100]))
        return 0 if hits or part else 1
    if args.key:
        print("en   %r" % en.get(args.key))
        for c, t in tables.items():
            print("%-4s %r" % (c, t.get(args.key)))
        return 0

    errors, warnings = [], []
    kn = {}
    for path, line, key, english, _ in scan(args.root):
        where = "%s:%d" % (path, line)
        if key.startswith("kn."):
            if key in en:
                errors.append("%s: %s shadows nothing, but is also a Krunker key" % (where, key))
            prev = kn.get(key)
            if prev and prev[0] != english:
                errors.append("%s: %s is %r here but %r at %s" % (where, key, english, prev[0], prev[1]))
            else:
                kn.setdefault(key, (english, where))
            continue
        if key not in en:
            errors.append("%s: %s is not a key of ui/i18n/en.json" % (where, key))
        elif fold(en[key]) != fold(english):
            errors.append("%s: %s: the C says %r, v10's en says %r" % (where, key, english, en[key]))
    kn_path = os.path.join(ui, "kn", "en.json")
    current = load(kn_path) if os.path.exists(kn_path) else {}
    want = {"_comment": current.get("_comment", "Strings Krunker Box Office added (kn.* keys). Generated: tools/i18n-import/i18n_lint.py --write-kn.")}
    for k in sorted(kn):
        want[k] = kn[k][0]
    if args.write_kn:
        with open(kn_path, "w", encoding="utf-8") as fh:
            fh.write(json.dumps(want, ensure_ascii=False, indent=1) + "\n")
        print("wrote %s: %d kn.* keys" % (kn_path, len(kn)))
    elif want != current:
        missing = sorted(set(want) - set(current))
        extra = sorted(set(current) - set(want))
        changed = sorted(k for k in want if k in current and want[k] != current[k] and k != "_comment")
        errors.append("ui/i18n/kn/en.json is out of date (--write-kn): %d missing %s, %d unused %s, %d changed %s" %
                      (len(missing), missing[:5], len(extra), extra[:5], len(changed), changed[:5]))
    for f in sorted(os.listdir(os.path.join(ui, "kn"))) if os.path.isdir(os.path.join(ui, "kn")) else []:
        if f.endswith(".json") and f != "en.json":
            for k in load(os.path.join(ui, "kn", f)):
                if not k.startswith("_") and k not in want:
                    errors.append("ui/i18n/kn/%s: %s is not a kn.* key the C uses" % (f, k))
    for e in errors:
        print("ERROR " + e)
    for w in warnings:
        print("warn  " + w)
    if not args.quiet:
        uses = scan(args.root)
        print("%d I18N uses, %d Krunker keys, %d kn.* keys; %d errors" %
              (len(uses), len({u[2] for u in uses if not u[2].startswith("kn.")}), len(kn), len(errors)))
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
