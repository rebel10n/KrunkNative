#!/usr/bin/env python3
"""Krunker Strike app icons, made from the Strike logo already in the tree.

Krunker Strike installs as its own app next to Krunker Box Office (platform/README.md "Krunker
Strike app"), so it needs its own home-screen icons. They are the real Krunker Strike FRVR logo,
ui/img/kn-strike-logo.png (1024x348, pinned in tools/ui-import/live_assets_v10.lock), trimmed to
its art and centred on the Strike loading-screen purple (#5736b0, FRVR's
.loading-screen__left-curtain; krunknative.css --kn-flavor-curtain-left). Every icon is opaque:
iOS and Android launchers draw their own mask, and an App Store icon must have no alpha.

  platform/web/strike/icons/     the Strike PWA (pwa.py copies them to <dist>/strike/img/pwa/):
                                 icon-32 (favicon), apple-touch-icon (180), icon-192, icon-512
                                 ("any": logo across 84 % of the width) and icon-512-maskable
                                 (logo inside the 80 % safe circle), og_1200x630 (link preview)
  platform/android/app/src/main/res/mipmap-*/ic_launcher_strike.png
                                 the Strike APK's launcher icon (48 / 72 / 96 / 144 / 192 px)
  platform/ios/KrunkNative/Assets.xcassets/AppIconStrike.appiconset/icon-1024.png
                                 the Strike iOS app's icon (opaque RGB)

usage: python3 -I tools/branding/strike_icons.py [write|check]
  write (default) regenerates every output; check exits 1 when one differs from what write makes.
Needs Pillow.
"""
import io
import os
import sys

from PIL import Image

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
LOGO = os.path.join(ROOT, "ui", "strike", "img", "krunker_logo.png")
BACKGROUND = (0x57, 0x36, 0xB0)  # the Strike loading-screen curtain purple
ANDROID = os.path.join(ROOT, "platform", "android", "app", "src", "main", "res")
WEB = os.path.join(ROOT, "platform", "web", "strike", "icons")
# path -> (width, height, fraction of the width the logo spans)
OUTPUTS = {
    os.path.join(WEB, "icon-32.png"): (32, 32, 0.96),
    os.path.join(WEB, "apple-touch-icon.png"): (180, 180, 0.84),
    os.path.join(WEB, "icon-192.png"): (192, 192, 0.84),
    os.path.join(WEB, "icon-512.png"): (512, 512, 0.84),
    os.path.join(WEB, "icon-512-maskable.png"): (512, 512, 0.72),
    os.path.join(WEB, "og_1200x630.png"): (1200, 630, 0.60),  # the link preview (og:image)
    os.path.join(ANDROID, "mipmap-mdpi", "ic_launcher_strike.png"): (48, 48, 0.84),
    os.path.join(ANDROID, "mipmap-hdpi", "ic_launcher_strike.png"): (72, 72, 0.84),
    os.path.join(ANDROID, "mipmap-xhdpi", "ic_launcher_strike.png"): (96, 96, 0.84),
    os.path.join(ANDROID, "mipmap-xxhdpi", "ic_launcher_strike.png"): (144, 144, 0.84),
    os.path.join(ANDROID, "mipmap-xxxhdpi", "ic_launcher_strike.png"): (192, 192, 0.84),
    os.path.join(ROOT, "platform", "ios", "KrunkNative", "Assets.xcassets", "AppIconStrike.appiconset", "icon-1024.png"): (1024, 1024, 0.80),
}


def icon(art, width, height, span):
    """width x height opaque RGB: art scaled to span * width wide, centred on the background."""
    w = max(1, round(width * span))
    h = max(1, round(art.height * w / art.width))
    logo = art.resize((w, h), Image.LANCZOS)
    out = Image.new("RGB", (width, height), BACKGROUND)
    out.paste(logo, ((width - w) // 2, (height - h) // 2), logo)
    return out


def encode(img):
    buf = io.BytesIO()
    img.save(buf, "PNG", optimize=True)
    return buf.getvalue()


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "write"
    if mode not in ("write", "check"):
        sys.exit(__doc__)
    art = Image.open(LOGO).convert("RGBA")
    art = art.crop(art.getbbox())  # the logo's transparent margins off
    stale = []
    for path, (width, height, span) in sorted(OUTPUTS.items()):
        data = encode(icon(art, width, height, span))
        old = open(path, "rb").read() if os.path.isfile(path) else None
        if old == data:
            continue
        if mode == "check":
            stale.append(os.path.relpath(path, ROOT))
            continue
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(data)
        print("wrote", os.path.relpath(path, ROOT))
    if stale:
        sys.exit("strike_icons.py: out of date (run write): " + ", ".join(stale))


if __name__ == "__main__":
    main()
