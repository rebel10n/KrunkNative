#!/usr/bin/env python3
"""Krunker v10.0.2 translations -> ui/i18n/<code>.json.

v10's i18n object (live bundle game.deob.js:197726-197788) keeps `translations.en` inline
(`parseText("\\nmenu.btn.ranked Ranked\\n...")`, tools/ui-import/v10_bundle.js writes it as
`i18n-en.json`) and loads every other language on first use with a synchronous XHR of
`i18n/<code>.js?v=i41Nu`. Those files are ES modules, ``export default `<lines>`;``, in the same
"key value" line format: the first space ends the key, the rest of the line is the value, empty
lines are skipped, a key seen twice keeps its last value (parseText assigns into an array).

    # 1. fetch (GET only, through the proxy; the live site, not the mirror: it has no .js)
    python3 -I tools/i18n-import/i18n_import.py fetch --out ~/.cache/kn-v10/i18n-live
    # 2. convert (en comes from the bundle's inline table, never from the fetched en.js)
    python3 -I tools/i18n-import/i18n_import.py convert --src ~/.cache/kn-v10/i18n-live \\
        --en ui/i18n/en.json --ui ui

The language list and order are v10's `langNames` (game.deob.js:18773, also
settings.list.lang's options). Output JSON keeps the file's key order, one key per line.

    # 3. the native engine's script fallback fonts (ui/css/fonts/kn-noto-*.otf, ui/i18n/README.md)
    python3 -I tools/i18n-import/i18n_import.py fonts --ui ui

`fonts` subsets Noto Sans CJK (JP / SC / TC / KR faces of the variable collection, instanced at
weight 400) and Noto Sans Devanagari to the characters the shipped tables (ui/i18n/*.json and
ui/i18n/kn/*.json) use in those scripts, plus the kana and CJK punctuation blocks. Re-run it when
a table changes: characters outside the subset fall through to the system's fonts.
"""
import argparse
import json
import os
import subprocess
import sys

# game.deob.js:18773 (iîìíîîï = config langNames): code -> the name the Language select shows
LANGS = [
    ("en", "English"), ("es", "Español"), ("de", "Deutsch"), ("nl", "Nederlands"), ("kr", "한국어"),
    ("pr", "Português"), ("jp", "日本語"), ("fr", "Français"), ("fi", "Suomi"), ("zh", "简体中文"),
    ("zhtr", "繁體中文"), ("pl", "Polski"), ("hi", "हिन्दी"), ("ru", "Pусский"), ("it", "Italiano"),
    ("tg", "Tagalog"), ("tr", "Türkçe"),
]
VERSION = "i41Nu"  # getTranslations' cache buster
BASE = "https://krunker.io/i18n/"


def parse_text(text):
    """v10 parseText over the template literal's body."""
    out = {}
    for line in text.split("\n"):
        if not line:
            continue
        parts = line.split(" ")
        out[parts[0]] = " ".join(parts[1:])  # a repeated key: last value, first position (as JS)
    return out


def parse_module(src):
    a = src.index("`") + 1
    b = src.rindex("`")
    body = src[a:b]
    if "\\" in body or "${" in body:
        raise ValueError("escape sequences in the template literal are not handled")
    return parse_text(body)


def dump(d, path):
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as fh:
        fh.write(json.dumps(d, ensure_ascii=False, indent=1))
        fh.write("\n")
    os.replace(tmp, path)


def cmd_fetch(args):
    os.makedirs(args.out, exist_ok=True)
    proxy = args.proxy or os.environ.get("KN_FETCH_PROXY", "http://127.0.0.1:8888")
    for code, _ in LANGS:
        url = "%s%s.js?v=%s" % (BASE, code, VERSION)
        dst = os.path.join(args.out, code + ".js")
        subprocess.run(["curl", "-sS", "--fail", "-x", proxy, "-o", dst, url], check=True)
        print("fetched", url, os.path.getsize(dst))


def cmd_convert(args):
    with open(args.en, encoding="utf-8") as fh:
        en = json.load(fh)
    out_dir = os.path.join(args.ui, "i18n")
    os.makedirs(out_dir, exist_ok=True)
    report = []
    for code, name in LANGS:
        if code == "en":
            continue
        with open(os.path.join(args.src, code + ".js"), encoding="utf-8") as fh:
            table = parse_module(fh.read())
        dump(table, os.path.join(out_dir, code + ".json"))
        known = sum(1 for k in table if k in en)
        report.append("%-5s %-12s %5d keys, %5d of en's %d (%d only here)" %
                      (code, name, len(table), known, len(en), len(table) - known))
    print("\n".join(report))


# script fallback faces: file, source font (collection index), the tables whose characters it carries
NOTO_CJK = "/usr/share/fonts/google-noto-sans-cjk-vf-fonts/NotoSansCJK-VF.ttc"
NOTO_DEVA = "/usr/share/fonts/google-noto-vf/NotoSansDevanagari[wght].ttf"
FONTS = [
    ("kn-noto-cjk-jp.otf", NOTO_CJK, 0, ["jp"]),
    ("kn-noto-cjk-sc.otf", NOTO_CJK, 2, ["zh"]),
    ("kn-noto-cjk-tc.otf", NOTO_CJK, 3, ["zhtr"]),
    ("kn-noto-cjk-kr.otf", NOTO_CJK, 1, ["kr"]),
    ("kn-noto-devanagari.ttf", NOTO_DEVA, None, ["hi"]),
]


def script_chars(cp, deva):
    """the code points a script face carries (Latin, digits and symbols come from the page's fonts)"""
    if deva:
        return 0x0900 <= cp <= 0x097F or 0xA8E0 <= cp <= 0xA8FF or cp in (0x200C, 0x200D, 0x25CC)
    return (0x2E80 <= cp <= 0x9FFF or 0xAC00 <= cp <= 0xD7AF or 0x1100 <= cp <= 0x11FF or
            0xF900 <= cp <= 0xFAFF or 0xFE30 <= cp <= 0xFE4F or 0xFF00 <= cp <= 0xFFEF)


def table_text(ui, codes):
    text = ""
    for code in codes:
        for path in (os.path.join(ui, "i18n", code + ".json"), os.path.join(ui, "i18n", "kn", code + ".json")):
            if os.path.exists(path):
                with open(path, encoding="utf-8") as fh:
                    text += "".join(v for v in json.load(fh).values() if isinstance(v, str))
    return text


def cmd_fonts(args):
    from fontTools.ttLib import TTCollection, TTFont
    from fontTools.varLib import instancer
    from fontTools import subset
    out_dir = os.path.join(args.ui, "css", "fonts")
    for name, src, index, codes in FONTS:
        deva = index is None
        cps = {ord(c) for c in table_text(args.ui, codes) if script_chars(ord(c), deva)}
        if not deva:
            cps |= set(range(0x3000, 0x3040))  # CJK symbols and punctuation
            cps |= {0xFF01, 0xFF08, 0xFF09, 0xFF0C, 0xFF0E, 0xFF1A, 0xFF1B, 0xFF1F}  # fullwidth punctuation
            if "jp" in codes:
                cps |= set(range(0x3041, 0x3097)) | set(range(0x30A1, 0x30FB)) | {0x30FC}  # kana
        font = TTCollection(src).fonts[index] if index is not None else TTFont(src)
        font = instancer.instantiateVariableFont(font, {"wght": 400}, updateFontNames=False)
        opts = subset.Options()
        opts.layout_features = []  # the engine has no OpenType shaper: cmap + hmtx (+ kern) only
        opts.notdef_outline = True
        opts.hinting = False
        opts.desubroutinize = False
        sub = subset.Subsetter(opts)
        sub.populate(unicodes=sorted(cps))
        sub.subset(font)
        dst = os.path.join(out_dir, name)
        font.save(dst)
        print("%-24s %5d code points  %7d bytes" % (name, len(cps), os.path.getsize(dst)))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    f = sub.add_parser("fetch")
    f.add_argument("--out", required=True)
    f.add_argument("--proxy")
    c = sub.add_parser("convert")
    c.add_argument("--src", required=True)
    c.add_argument("--en", required=True)
    c.add_argument("--ui", required=True)
    fo = sub.add_parser("fonts")
    fo.add_argument("--ui", required=True)
    args = ap.parse_args()
    {"fetch": cmd_fetch, "convert": cmd_convert, "fonts": cmd_fonts}[args.cmd](args)
    return 0


if __name__ == "__main__":
    sys.exit(main())
