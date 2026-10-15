#!/usr/bin/env python3
"""Draws the Nettspend class's KrunkNative-only icon art (client/include/nettspend.h):

  python3 -I tools/nettspend/icons.py            (run from the repository root; needs Pillow)

  ui/textures/weapons/icon_kn_drank.png, icon_kn_money.png   the Purple Drank's / Money Spread's HUD rack
                                                        and kill-feed icons (Weapon.icon stems), 256x128
                                                        white silhouettes on transparency like Krunker's
                                                        textures/weapons/icon_<n>.png
  ui/img/death-ui/icon_kn_drank.png, icon_kn_money.png  the same for the death card (./img/death-ui/<stem>.png)
  ui/textures/classes/icon_16.png                        the Nettspend class icon (ClassConfig.icon_index 16),
                                                        8x8 pixel art like textures/classes/icon_<n>.png
  ui/img/kn/preview_drank.png                            the Drank's loadout-tile preview: the white styrofoam
                                                        double cup in colour, 256x256 (the 3D cup's look)

They sit next to Krunker's own icons in the page tree (ui/textures/weapons, ui/textures/classes,
ui/img/death-ui), so every page URL built from Weapon.icon / ClassConfig.icon_index finds them.
"""
import os
import sys

from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SS = 4  # supersampling
W, H = 256, 128
WHITE = (255, 255, 255, 255)
CLEAR = (0, 0, 0, 0)


def canvas():
    im = Image.new("RGBA", (W * SS, H * SS), CLEAR)
    return im, ImageDraw.Draw(im)


def finish(im, path):
    out = im.resize((W, H), Image.LANCZOS)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    out.save(path, optimize=True)
    print(f"wrote {os.path.relpath(path, ROOT)}")


def s(v):
    return v * SS


def drank():
    """A lean double cup, tilted a little, with a straw: the outer cup's rim a step below the inner's."""
    im, d = canvas()
    # the inner cup (tall trapezoid), then the outer cup drawn over its lower part with a gap line
    cx = 118
    top_w, bot_w, top_y, bot_y = 46, 34, 22, 112
    d.polygon([(s(cx - top_w), s(top_y)), (s(cx + top_w), s(top_y)), (s(cx + bot_w), s(bot_y)), (s(cx - bot_w), s(bot_y))], fill=WHITE)
    # the inner cup's lip
    d.rounded_rectangle([s(cx - top_w - 4), s(top_y - 4), s(cx + top_w + 4), s(top_y + 8)], radius=s(4), fill=WHITE)
    # the outer cup: its rim 14 below the top, a transparent seam above it so the two cups read
    rim_y = top_y + 16
    rim_w = top_w - (top_w - bot_w) * (rim_y - top_y) / (bot_y - top_y)
    d.polygon([(s(cx - rim_w - 5), s(rim_y - 3)), (s(cx + rim_w + 5), s(rim_y - 3)), (s(cx + rim_w + 5), s(rim_y + 1)),
               (s(cx - rim_w - 5), s(rim_y + 1))], fill=CLEAR)
    d.rounded_rectangle([s(cx - rim_w - 6), s(rim_y + 1), s(cx + rim_w + 6), s(rim_y + 11)], radius=s(4), fill=WHITE)
    # the straw, leaning out of the cup to the right
    d.line([(s(cx + 14), s(top_y + 2)), (s(cx + 52), s(6))], fill=WHITE, width=s(7))
    d.ellipse([s(cx + 52 - 4), s(6 - 4), s(cx + 52 + 4), s(6 + 4)], fill=WHITE)
    # the liquid's surface: a thin dark seam just under the top lip
    d.rectangle([s(cx - top_w + 6), s(top_y + 10), s(cx + top_w - 6), s(top_y + 12)], fill=CLEAR)
    # a few drops spilling left
    for x, y, r in ((cx - 62, 40, 4), (cx - 72, 58, 3), (cx - 58, 72, 3)):
        d.ellipse([s(x - r), s(y - r), s(x + r), s(y + r)], fill=WHITE)
    return im


def money():
    """A banded brick of bills with three bills fanned off its top."""
    im, d = canvas()
    cx, cy = 118, 80
    # the brick
    d.rounded_rectangle([s(cx - 70), s(cy - 22), s(cx + 70), s(cy + 30)], radius=s(6), fill=WHITE)
    # its paper band (a transparent seam each side of a narrower strip)
    d.rectangle([s(cx - 16), s(cy - 24), s(cx + 16), s(cy + 32)], fill=CLEAR)
    d.rectangle([s(cx - 11), s(cy - 24), s(cx + 11), s(cy + 32)], fill=WHITE)
    # the bills' edges: thin seams across the brick
    for y in (cy - 8, cy + 4, cy + 16):
        d.rectangle([s(cx - 70), s(y), s(cx + 70), s(y + 2)], fill=CLEAR)
    # three bills fanned off the top, each turned a little more
    for i, angle in enumerate((8, 20, 34)):
        bill = Image.new("RGBA", (s(150), s(60)), CLEAR)
        bd = ImageDraw.Draw(bill)
        bd.rounded_rectangle([s(4), s(10), s(146), s(50)], radius=s(5), fill=WHITE)
        bd.rounded_rectangle([s(12), s(18), s(138), s(42)], radius=s(4), fill=CLEAR)  # the bill's printed frame
        bd.ellipse([s(62), s(22), s(88), s(38)], fill=WHITE)                         # the portrait
        bill = bill.rotate(angle, resample=Image.BICUBIC, expand=True)
        px, py = s(cx + 10 - 8 * i), s(cy - 50 - 14 * i)
        im.alpha_composite(bill, (px - bill.width // 2, py - bill.height // 2))
    return im


def drank_preview():
    """The Drank's weapon-tile preview (ui/img/kn/preview_drank.png, 256x256 in colour): the white
    styrofoam double cup of the 3D model (client/src/nettspend_vfx.c cup_geometry): two stacked matte
    off-white cups, the outer one's rolled lip a step under the inner one's, the purple lean just under
    the top rim, as seen a little from above."""
    S = 256
    im = Image.new("RGBA", (S * SS, S * SS), CLEAR)
    d = ImageDraw.Draw(im)
    foam, shade, lip = (246, 245, 241, 255), (222, 220, 214, 255), (255, 255, 253, 255)
    seam = (190, 187, 180, 255)
    lean, lean_edge, lean_shine = (123, 44, 191, 255), (74, 26, 122, 255), (155, 91, 214, 255)
    cx = 128
    top_y, base_y = 52, 226
    top_w, base_w = 98, 66                 # half widths at the rim and at the base
    def half(y):
        return base_w + (top_w - base_w) * (y - base_y) / (top_y - base_y)
    seam_y = top_y + 34                   # where the inner cup's wall meets the outer cup's rim
    # the body: a trapezoid with moulded ridges (every 22 units a slightly darker band)
    d.polygon([(s(cx - half(top_y)), s(top_y)), (s(cx + half(top_y)), s(top_y)), (s(cx + half(base_y)), s(base_y)), (s(cx - half(base_y)), s(base_y))], fill=foam)
    for y in range(base_y - 26, top_y, -34):
        d.rectangle([s(cx - half(y)), s(y), s(cx + half(y)), s(y + 2)], fill=shade)
    # the outer cup's rolled lip under the inner cup's rim, a seam above it
    d.rounded_rectangle([s(cx - half(seam_y) - 4), s(seam_y - 4), s(cx + half(seam_y) + 4), s(seam_y + 8)], radius=s(4), fill=lip)
    d.rectangle([s(cx - half(seam_y) - 2), s(seam_y + 8), s(cx + half(seam_y) + 2), s(seam_y + 11)], fill=seam)
    # the inner cup's rolled rim at the top
    d.rounded_rectangle([s(cx - top_w - 5), s(top_y - 9), s(cx + top_w + 5), s(top_y + 6)], radius=s(6), fill=lip)
    # the lean: a dark edge ellipse, the purple surface, a soft highlight off-centre
    d.ellipse([s(cx - top_w + 4), s(top_y - 2), s(cx + top_w - 4), s(top_y + 26)], fill=lean_edge)
    d.ellipse([s(cx - top_w + 9), s(top_y + 1), s(cx + top_w - 9), s(top_y + 22)], fill=lean)
    d.ellipse([s(cx - 46), s(top_y + 6), s(cx - 14), s(top_y + 12)], fill=lean_shine)
    # the body's shadow edge on the right, so the cup reads as a solid
    d.polygon([(s(cx + half(top_y) - 14), s(top_y + 10)), (s(cx + half(top_y) - 2), s(top_y + 10)), (s(cx + half(base_y) - 2), s(base_y - 4)),
               (s(cx + half(base_y) - 12), s(base_y - 4))], fill=shade)
    return im.resize((S, S), Image.LANCZOS)


def class_icon():
    """8x8: the lean cup (purple) with its straw, a cash-green base pixel row."""
    P = (123, 44, 191, 255)   # #7b2cbf, the lean
    L = (244, 242, 248, 255)  # the styrofoam
    Y = (241, 230, 184, 255)  # the straw
    G = (63, 175, 90, 255)    # the cash
    _ = CLEAR
    rows = [
        [_, _, _, _, _, _, Y, _],
        [_, L, L, L, L, L, Y, _],
        [_, L, P, P, P, L, _, _],
        [_, L, P, P, P, L, _, _],
        [_, _, L, P, P, L, _, _],
        [_, _, L, P, L, _, _, _],
        [_, _, L, L, L, _, _, _],
        [_, G, G, G, G, G, _, _],
    ]
    im = Image.new("RGBA", (8, 8), CLEAR)
    for y, row in enumerate(rows):
        for x, c in enumerate(row):
            im.putpixel((x, y), c)
    return im


def main():
    for stem, draw in (("icon_kn_drank", drank), ("icon_kn_money", money)):
        im = draw()
        finish(im, os.path.join(ROOT, "ui", "textures", "weapons", stem + ".png"))
        finish(im, os.path.join(ROOT, "ui", "img", "death-ui", stem + ".png"))
    path = os.path.join(ROOT, "ui", "textures", "classes", "icon_16.png")
    class_icon().save(path, optimize=True)
    print(f"wrote {os.path.relpath(path, ROOT)}")
    path = os.path.join(ROOT, "ui", "img", "kn", "preview_drank.png")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    drank_preview().save(path, optimize=True)
    print(f"wrote {os.path.relpath(path, ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
