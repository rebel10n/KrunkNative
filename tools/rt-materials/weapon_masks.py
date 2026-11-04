#!/usr/bin/env python3
"""Per-weapon metal / roughness masks for the ray-traced material pass (doc/raytracing.md).

For every weapon and melee texture (assets/textures/weapons/weapon_*.png, assets/textures/melee/melee_*.png)
writes `<base>_mr.png` next to it: a glTF-style metallicRoughness texture (R unused = 255, G = roughness,
B = metalness) that client/src/rt/rt_masks.c binds as the model's `metal_rough_texture` while ray tracing is
on, so one gun has a metal barrel and receiver and a wood / polymer grip and stock.

The client builds the same masks itself at run time (client/src/rt/rt_masks.c ports this classifier to the GPU,
assets/shaders/rt_mask_classify.frag + rt_mask_filter.frag, and to C as its fallback: keep the rules, the filter and
the values of all three in step; KRUNKNATIVE_RT_MASK_DUMP=<dir> dumps the client's masks, which must be identical to
`--only <base> --max-size 0`); a `_mr.png` next to the texture overrides the built one, so this tool
is for hand-tuning and for the sheets. The parts are classified per texel from the texture's own colour (HSV), then cleaned with a 5x5 majority
filter (2 px) so the mask follows parts, not pixel noise:

  class    rule (defaults; weapon_masks.json overrides them per texture)        G rough  B metal
  wood     hue 8..50 deg, sat >= 0.30, val >= 0.18 (orange / brown)               190      0
  polymer  val < 0.05 (true black: grips, rubber; dark gunmetal stays metal)      110      0
  plastic  sat >= 0.55, hue outside 5..50 (saturated non-wood accents)            100      0
  painted  0.14 <= sat < 0.55, val >= 0.22, not wood (olive / khaki / blue paint) 140      100
  metal    everything else (desaturated greys / blues)                             90      200

Metalness tops out at 200 (0.78), not 1: a metal texel keeps a fifth of its diffuse, so a gun whose pixels get no
traced reflection (the viewmodel before its RT pass, rtMode < 3, a shadowed barrel) reads as dark worn metal
instead of black. The class factors of rt_materials.json scale it further.

Overrides (assets/materials/weapon_masks.json, shipped: client/src/rt/rt_masks.c applies the same file at run time): per texture base name (or "*" for all),
  "rules":   [{"class": "wood", "hue": [12, 45], "sat_min": 0.3, "val_min": 0.22}, ...]  evaluated first, in order
  "rects":   [{"class": "metal", "uv": [x0, y0, x1, y1]}, ...]   forced class inside a UV rectangle (0..1, top-left origin)
  "default": "metal"                                             class for texels no rule matched (and transparent ones)
  "values":  {"wood": [190, 0], ...}                             per-class (G, B) overrides

Usage:
  tools/rt-materials/weapon_masks.py [--assets assets] [--only weapon_1,weapon_2,melee_0] [--max-size 512]
                                     [--sheets DIR] [--overrides assets/materials/weapon_masks.json] [--dry-run]
Sheets (texture | mask, one PNG per texture) go to --sheets (default ~/.cache/kn-rt-masks/sheets); masks are
written at most --max-size wide (nearest resample of the class map: parts stay crisp, files stay small).
"""
import argparse
import fnmatch
import json
import os
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

CLASSES = ["metal", "painted", "wood", "polymer", "plastic"]  # index = class id in the mask map
VALUES = {"metal": (90, 200), "painted": (140, 100), "wood": (190, 0), "polymer": (110, 0), "plastic": (100, 0)}  # (G rough, B metal)
SHEET_COLORS = {"metal": (150, 160, 175), "painted": (120, 150, 90), "wood": (190, 110, 50), "polymer": (40, 40, 40), "plastic": (220, 60, 160)}

DEFAULT_RULES = [
    {"class": "wood", "hue": [8, 50], "sat_min": 0.30, "val_min": 0.18},
    {"class": "polymer", "val_max": 0.05},
    {"class": "plastic", "hue": [50, 5], "sat_min": 0.55, "val_min": 0.18},
    {"class": "painted", "sat_min": 0.14, "sat_max": 0.55, "val_min": 0.22},
]


def load_overrides(path):
    if not path or not os.path.exists(path):
        return {}
    with open(path) as f:
        return json.load(f)


def overrides_for(overrides, base):
    merged = {"rules": None, "rects": [], "default": "metal", "values": {}}
    for key in ("*", base):
        o = overrides.get(key)
        if not o:
            continue
        if "rules" in o:
            merged["rules"] = o["rules"]
        merged["rects"] += o.get("rects", [])
        merged["default"] = o.get("default", merged["default"])
        merged["values"].update(o.get("values", {}))
    for pat, o in overrides.items():  # glob keys other than "*" and the exact name
        if pat in ("*", base) or not fnmatch.fnmatchcase(base, pat):
            continue
        if "rules" in o:
            merged["rules"] = o["rules"]
        merged["rects"] += o.get("rects", [])
        merged["default"] = o.get("default", merged["default"])
        merged["values"].update(o.get("values", {}))
    if merged["rules"] is None:
        merged["rules"] = DEFAULT_RULES
    return merged


def classify(img, ov):
    """Returns the class-id map (uint8, H x W) of an RGBA image."""
    rgba = np.asarray(img.convert("RGBA"))
    hsv = np.asarray(img.convert("RGB").convert("HSV")).astype(np.float32)
    hue = hsv[..., 0] * (360.0 / 255.0)
    sat = hsv[..., 1] / 255.0
    val = hsv[..., 2] / 255.0
    alpha = rgba[..., 3]
    cls = np.full(hue.shape, CLASSES.index(ov["default"]), dtype=np.uint8)
    assigned = alpha < 8  # transparent texels keep the default
    for rule in ov["rules"]:
        m = ~assigned
        if "hue" in rule:
            h0, h1 = rule["hue"]
            m &= (hue >= h0) & (hue <= h1) if h0 <= h1 else (hue >= h0) | (hue <= h1)
        if "sat_min" in rule:
            m &= sat >= rule["sat_min"]
        if "sat_max" in rule:
            m &= sat < rule["sat_max"]
        if "val_min" in rule:
            m &= val >= rule["val_min"]
        if "val_max" in rule:
            m &= val < rule["val_max"]
        cls[m] = CLASSES.index(rule["class"])
        assigned |= m
    # 5x5 majority filter (2 px): parts, not noise
    cls = np.array(Image.fromarray(cls, "L").filter(ImageFilter.ModeFilter(5)), dtype=np.uint8)  # a writable copy (rects below)
    h, w = cls.shape
    for rect in ov["rects"]:
        x0, y0, x1, y1 = rect["uv"]
        cls[int(y0 * h):int(y1 * h), int(x0 * w):int(x1 * w)] = CLASSES.index(rect["class"])
    return cls


def mask_image(cls, ov):
    values = dict(VALUES)
    values.update({k: tuple(v) for k, v in ov["values"].items()})
    lut_g = np.array([values[c][0] for c in CLASSES], dtype=np.uint8)
    lut_b = np.array([values[c][1] for c in CLASSES], dtype=np.uint8)
    out = np.stack([np.full(cls.shape, 255, np.uint8), lut_g[cls], lut_b[cls]], axis=-1)
    return Image.fromarray(out, "RGB")


def sheet_image(img, cls, base, stats):
    lut = np.array([SHEET_COLORS[c] for c in CLASSES], dtype=np.uint8)
    colored = Image.fromarray(lut[cls], "RGB")
    size = 384
    left = img.convert("RGB").resize((size, size), Image.NEAREST)
    right = colored.resize((size, size), Image.NEAREST)
    sheet = Image.new("RGB", (size * 2 + 12, size + 34), (20, 20, 24))
    sheet.paste(left, (4, 30))
    sheet.paste(right, (size + 8, 30))
    d = ImageDraw.Draw(sheet)
    d.text((6, 4), base, fill=(230, 230, 230))
    d.text((size + 10, 4), "  ".join("%s %d%%" % (c, stats[c]) for c in CLASSES if stats[c]), fill=(200, 200, 200))
    d.text((6, 18), "mask colours: metal grey, painted green, wood orange, polymer black, plastic magenta", fill=(130, 130, 140))
    return sheet


def process(path, ov, max_size, sheets_dir, dry_run):
    base = os.path.splitext(os.path.basename(path))[0]
    img = Image.open(path)
    cls = classify(img, ov)
    total = cls.size
    stats = {c: int(round(100.0 * np.count_nonzero(cls == i) / total)) for i, c in enumerate(CLASSES)}
    out_path = os.path.join(os.path.dirname(path), base + "_mr.png")
    if not dry_run:
        m = mask_image(cls, ov)
        if max_size and max(m.size) > max_size:
            scale = max_size / float(max(m.size))
            m = m.resize((max(1, int(m.size[0] * scale)), max(1, int(m.size[1] * scale))), Image.NEAREST)
        m.convert("P", palette=Image.ADAPTIVE, colors=8).save(out_path, optimize=True)
        if sheets_dir:
            os.makedirs(sheets_dir, exist_ok=True)
            sheet_image(img, cls, base, stats).save(os.path.join(sheets_dir, base + ".png"))
    return base, stats, out_path


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    repo = os.path.dirname(os.path.dirname(here))
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--assets", default=os.path.join(repo, "assets"))
    ap.add_argument("--only", default="", help="comma-separated texture base names (default: all weapon_* and melee_*)")
    ap.add_argument("--max-size", type=int, default=512)
    ap.add_argument("--sheets", default=os.path.expanduser("~/.cache/kn-rt-masks/sheets"))
    ap.add_argument("--overrides", default=os.path.join(repo, "assets", "materials", "weapon_masks.json"))
    ap.add_argument("--dry-run", action="store_true", help="classify and print stats, write nothing")
    args = ap.parse_args()

    only = set(s for s in args.only.split(",") if s)
    files = []
    for sub, prefix in (("weapons", "weapon_"), ("melee", "melee_")):
        d = os.path.join(args.assets, "textures", sub)
        if not os.path.isdir(d):
            continue
        for name in sorted(os.listdir(d)):
            base, ext = os.path.splitext(name)
            if ext != ".png" or not base.startswith(prefix) or base.endswith("_mr"):
                continue
            if not all(ch.isdigit() or ch == "_" for ch in base[len(prefix):]):
                continue  # weapon_12_old, icons, ...
            if only and base not in only:
                continue
            files.append(os.path.join(d, name))
    if not files:
        print("no textures found", file=sys.stderr)
        return 1
    overrides = load_overrides(args.overrides)
    totals = {c: 0 for c in CLASSES}
    for i, path in enumerate(files):
        base = os.path.splitext(os.path.basename(path))[0]
        try:
            base, stats, out_path = process(path, overrides_for(overrides, base), args.max_size, args.sheets, args.dry_run)
        except Exception as e:  # a corrupt texture must not stop the batch
            print("%s: %s" % (path, e), file=sys.stderr)
            continue
        for c in CLASSES:
            totals[c] += stats[c]
        if only or len(files) <= 40:
            print("%-16s %s" % (base, "  ".join("%s %d%%" % (c, stats[c]) for c in CLASSES if stats[c])))
    n = len(files)
    print("%d textures, mean coverage: %s" % (n, "  ".join("%s %.0f%%" % (c, totals[c] / float(n)) for c in CLASSES)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
