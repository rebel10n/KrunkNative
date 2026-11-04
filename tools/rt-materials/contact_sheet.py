#!/usr/bin/env python3
"""Contact sheets + a draft material table for the ray tracer (assets/materials/rt_materials.json).

Renders every map / prop texture (assets/textures/*.png: the v7 map texture table's "<src>_<tv>"
variants and the prefab models' "<model>[_<variant>]" textures), every weapon base texture
(assets/textures/weapons/weapon_<id>.png, one per weapon src) and every melee base texture
(assets/textures/melee/melee_<id>.png) into contact sheets: thumbnail, file name, proposed class.

The proposal is a heuristic (name keywords first, then colour statistics); the shipped
rt_materials.json is hand-corrected from the sheets. See README.md.

Usage:  python3 tools/rt-materials/contact_sheet.py [--out DIR] [--draft FILE] [--assets DIR]
Needs Pillow (python3 -m pip install pillow); falls back to ImageMagick `magick montage` without it
(then the sheets have no proposed-class labels and the draft JSON still gets written).
"""
import argparse
import colorsys
import fnmatch
import json
import os
import re
import subprocess
import sys

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:  # the ImageMagick fallback
    Image = None

# ---------------------------------------------------------------- classes (mirrors rt_materials.json)
CLASSES = {
    "metal":      (0.9, 0.35, 1.0),
    "dark_metal": (0.8, 0.5, 0.6),
    "chrome":     (1.0, 0.1, 2.0),
    "gold":       (1.0, 0.25, 2.0),
    "wood":       (0.0, 0.7, 0.2),
    "cloth":      (0.0, 0.9, 0.0),
    "skin":       (0.0, 0.6, 0.0),
    "plastic":    (0.0, 0.4, 0.3),
    "stone":      (0.0, 0.85, 0.1),
    "glass":      (0.0, 0.05, 1.5),
    "water":      (0.0, 0.05, 1.5),
    "emissive":   (0.0, 1.0, 0.0),
    "foliage":    (0.0, 0.8, 0.0),
}

# name keyword -> class, first match wins (order matters: "sheet_metal" before "sheet")
KEYWORDS = [
    ("chrome", "chrome"), ("gold", "gold"), ("silver", "chrome"),
    ("metal", "metal"), ("steel", "metal"), ("iron", "metal"), ("pipe", "metal"), ("grate", "metal"),
    ("fence", "metal"), ("link", "metal"), ("container", "dark_metal"), ("barrel", "dark_metal"),
    ("dumpster", "dark_metal"), ("ac_unit", "metal"), ("acunit", "metal"), ("antenna", "metal"),
    ("satellite", "metal"), ("water_tower", "metal"), ("watertower", "metal"), ("vehicle", "dark_metal"),
    ("cage", "dark_metal"), ("machine", "dark_metal"), ("servernode", "dark_metal"), ("weaponbox", "dark_metal"),
    ("lamp", "metal"), ("beam", "metal"), ("rail", "metal"), ("door", "wood"),
    ("tile", "stone"), ("roof", "stone"), ("brick", "stone"), ("wall", "stone"), ("stone", "stone"),
    ("concrete", "stone"), ("grey", "stone"), ("dirt", "stone"), ("sand", "stone"), ("snow", "stone"),
    ("grain", "stone"), ("check", "stone"), ("grid", "stone"), ("lines", "stone"), ("pallet", "wood"),
    ("glass", "glass"), ("window", "glass"), ("crystal", "glass"), ("ice", "glass"), ("bottle", "glass"),
    ("water", "water"), ("liquid", "water"),
    ("wood", "wood"), ("plank", "wood"), ("crate", "wood"), ("floor", "wood"), ("cardb", "wood"),
    ("stack", "wood"), ("chair", "wood"), ("cane", "wood"), ("sign", "wood"),
    ("grass", "foliage"), ("tree", "foliage"), ("christmastree", "foliage"), ("pumpkin", "foliage"),
    ("cloth", "cloth"), ("fabric", "cloth"), ("flag", "cloth"), ("leather", "cloth"), ("teddy", "cloth"),
    ("neon", "emissive"), ("light", "emissive"), ("flare", "emissive"), ("screen", "emissive"),
    ("sky", "emissive"), ("sun", "emissive"), ("orb", "emissive"), ("rune", "emissive"), ("beam", "emissive"),
    ("zone", "emissive"), ("objective", "emissive"), ("marker", "emissive"), ("pad", "emissive"),
    ("cone", "plastic"), ("police", "plastic"), ("egg", "plastic"), ("gift", "plastic"), ("event", "plastic"),
    ("skull", "stone"), ("skeleton", "stone"), ("knight", "metal"), ("ghost", "cloth"),
]


def keyword_class(name):
    low = name.lower()
    for key, cls in KEYWORDS:
        if key in low:
            return cls, "name:" + key
    return None, ""


def colour_class(img):
    """Colour statistics of the opaque texels: saturation / luminance / hue -> a class guess."""
    small = img.convert("RGBA").resize((32, 32))
    px = [p for p in small.getdata() if p[3] > 32]
    if not px:
        return "glass", "stats:transparent"
    n = len(px)
    hsv = [colorsys.rgb_to_hsv(r / 255.0, g / 255.0, b / 255.0) for r, g, b, a in px]
    sat = sum(h[1] for h in hsv) / n
    val = sum(h[2] for h in hsv) / n
    hue = sum(h[0] for h in hsv) / n
    alpha = sum(p[3] for p in px) / (255.0 * n)
    tag = "stats:s%.2f v%.2f h%.2f" % (sat, val, hue)
    if alpha < 0.9:
        return "glass", tag
    if sat < 0.12 and 0.3 < val < 0.75:
        return "metal", tag
    if sat < 0.12 and val <= 0.3:
        return "dark_metal", tag
    if sat < 0.15:
        return "stone", tag
    if 0.04 < hue < 0.12 and sat > 0.25 and val < 0.7:
        return "wood", tag
    if 0.2 < hue < 0.45 and sat > 0.3:
        return "foliage", tag
    if sat > 0.6 and val > 0.8:
        return "emissive", tag
    return "stone", tag


def propose(name, img):
    cls, why = keyword_class(name)
    if cls:
        return cls, why
    if img is None:
        return "stone", "default"
    return colour_class(img)


# ---------------------------------------------------------------- sources
def base_name(file_name):
    """"brick_0_7.png" -> family "brick_0" (the map texture table src), "weapon_12.png" -> "weapon_12"."""
    stem = os.path.splitext(file_name)[0]
    m = re.match(r"^(.*?_\d+)(?:_(\d+|e|n|old|next))*$", stem)
    return m.group(1) if m else stem


def collect(assets):
    groups = []
    top = sorted(f for f in os.listdir(os.path.join(assets, "textures")) if f.endswith(".png"))
    groups.append(("map-and-props", "textures", [f for f in top if not f.endswith("_e.png")]))
    weapons = sorted((f for f in os.listdir(os.path.join(assets, "textures", "weapons"))
                      if re.match(r"^weapon_\d+\.png$", f)),
                     key=lambda f: int(re.findall(r"\d+", f)[0]))
    groups.append(("weapons", "textures/weapons", weapons))
    melee = sorted((f for f in os.listdir(os.path.join(assets, "textures", "melee"))
                    if re.match(r"^melee_\d+\.png$", f)),
                   key=lambda f: int(re.findall(r"\d+", f)[0]))
    groups.append(("melee", "textures/melee", melee))
    return groups


# ---------------------------------------------------------------- sheets
THUMB, COLS, CELL_H = 96, 8, 128


def sheet_pillow(title, folder, files, out_png, proposals):
    rows = (len(files) + COLS - 1) // COLS
    img = Image.new("RGB", (COLS * (THUMB + 8) + 8, rows * CELL_H + 24), (24, 24, 28))
    draw = ImageDraw.Draw(img)
    try:
        font = ImageFont.truetype("/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf", 9)
    except (OSError, IOError):
        font = ImageFont.load_default()
    draw.text((8, 6), "%s (%d)" % (title, len(files)), fill=(220, 220, 220), font=font)
    for i, f in enumerate(files):
        x = 8 + (i % COLS) * (THUMB + 8)
        y = 24 + (i // COLS) * CELL_H
        try:
            t = Image.open(os.path.join(folder, f)).convert("RGBA")
            t.thumbnail((THUMB, THUMB))
            bg = Image.new("RGBA", t.size, (90, 90, 96, 255))
            img.paste(Image.alpha_composite(bg, t).convert("RGB"), (x, y))
        except Exception as e:  # noqa: BLE001 - a broken PNG still gets a label
            draw.rectangle((x, y, x + THUMB, y + THUMB), fill=(120, 30, 30))
        cls, why = proposals[f]
        draw.text((x, y + THUMB + 2), os.path.splitext(f)[0][:18], fill=(230, 230, 230), font=font)
        draw.text((x, y + THUMB + 13), cls, fill=(150, 220, 150), font=font)
    img.save(out_png)


def sheet_magick(folder, files, out_png):
    cmd = ["magick", "montage", "-background", "#18181c", "-fill", "white", "-geometry", "96x96+4+4",
           "-tile", "%dx" % COLS, "-label", "%t"] + [os.path.join(folder, f) for f in files] + [out_png]
    subprocess.run(cmd, check=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    ap.add_argument("--assets", default=os.path.join(root, "assets"))
    ap.add_argument("--out", default=os.path.join(root, "tools", "rt-materials", "sheets"))
    ap.add_argument("--draft", default=os.path.join(root, "tools", "rt-materials", "draft.json"))
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)

    draft = {"classes": {k: {"metalness": v[0], "roughness": v[1], "reflect": v[2]} for k, v in CLASSES.items()},
             "textures": {}, "models": {}, "weapons": {}, "parts": {}, "paint": {}, "default": "stone"}
    counts = {}
    for title, rel, files in collect(args.assets):
        folder = os.path.join(args.assets, rel)
        proposals = {}
        families = {}
        for f in files:
            img = None
            if Image is not None:
                try:
                    img = Image.open(os.path.join(folder, f))
                except Exception:  # noqa: BLE001
                    img = None
            cls, why = propose(os.path.splitext(f)[0], img)
            proposals[f] = (cls, why)
            counts[cls] = counts.get(cls, 0) + 1
            fam = base_name(f)
            families.setdefault(fam, []).append(cls)
        out_png = os.path.join(args.out, title + ".png")
        if Image is not None:
            sheet_pillow(title, folder, files, out_png, proposals)
        else:
            sheet_magick(folder, files, out_png)
        print("%s: %d textures -> %s" % (title, len(files), out_png))
        for fam, classes in sorted(families.items()):
            cls = max(set(classes), key=classes.count)  # the family's majority proposal
            key = (rel.replace("textures/", "", 1) + "/" if rel != "textures" else "") + fam + "*"
            draft["textures"][key] = cls
    with open(args.draft, "w") as fp:
        json.dump(draft, fp, indent=1, sort_keys=False)
        fp.write("\n")
    print("draft: %s" % args.draft)
    print("proposed classes:", ", ".join("%s %d" % kv for kv in sorted(counts.items())))


if __name__ == "__main__":
    sys.exit(main())
