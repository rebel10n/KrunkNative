#!/usr/bin/env python3
"""Krunker Box Office branding: the app icons, taken from https://krunker.io's live PWA set.

Krunker Box Office (formerly KrunkNative) wears Krunker's current branding: the PWA icons
krunker.io links from its <head> (``<link rel="apple-touch-icon">``, ``<link rel="icon">``) and
its ``manifest.json`` (``img/pwa/icon<N>x<N>.png``), plus the page favicon and the menu logo,
which ui/ already carries byte-identical. ``branding.lock`` (next to this script) pins every
file by URL, sha256 and size; ``install`` downloads them (or reuses a verified staged copy)
and writes every icon the builds use (``OUTPUTS`` below):

  platform/web/icons/       the PWA (pwa.py copies them to <dist>/img/pwa/; the manifest and
                            <head> links point there): 32 px favicon, 180 px apple-touch-icon,
                            192 / 512 px manifest icons (the 512 one doubles as maskable: the art
                            is full-bleed and its centre circle holds the face and the badge),
                            and og_1200x630.jpg, krunker.io's link-preview image (og:image /
                            twitter:image, pwa.py)
  assets/img/icon.png       the native window icon (client/src/main.c and the launcher,
                            glfwSetWindowIcon) and the Linux desktop entry's icon (256 px)
  client/icon.ico           the Windows executables' icon (client/win.rc, client_bin and the
                            launcher)
  platform/ios/.../icon-1024.png
                            the iOS app icon and the macOS .icns source (tools/release/
                            package-client.sh: sips + iconutil); opaque RGB as App Store wants
  platform/android/.../mipmap-*/ic_launcher.png
                            the Android launcher icons (48 / 72 / 96 / 144 / 192 px)

Live values that are not files (checked 2026-10-12): manifest.json theme_color / background_color
#000000 (platform/web/manifest.webmanifest uses the same), <meta name="theme-color"> #FFD600
(already in ui/index.html from the v7 page). krunker.io names the app "Krunker FRVR"; we say
"Krunker Box Office" (pwa.py, the manifest).

usage:
  python3 -I tools/branding/fetch_branding.py install [--stage DIR]   fetch, verify, write OUTPUTS
  python3 -I tools/branding/fetch_branding.py check                   verbatim OUTPUTS match the lock
  python3 -I tools/branding/fetch_branding.py repin [--stage DIR]     re-download and rewrite the lock

environment: KN_FETCH_PROXY (e.g. http://127.0.0.1:8888), KN_FETCH_UA (default: a current
desktop Chrome, the same as tools/ui-import/fetch_live_assets.py).
"""

import argparse
import hashlib
import io
import os
import sys
import tempfile
import time
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
LOCK = os.path.join(HERE, "branding.lock")
DEFAULT_STAGE = os.path.join(os.path.expanduser("~"), ".cache", "krunknative", "branding")
DEFAULT_UA = ("Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) "
              "Chrome/141.0.0.0 Safari/537.36")
LOCK_HEADER = ("# krunker.io branding files (tools/branding/fetch_branding.py).\n"
               "# name\turl\tsha256\tsize\n")
# Live URLs, in the order krunker.io links them (the lock carries the pinned hashes)
SOURCES = (
    ("favicon.png", "https://krunker.io/img/favicon.png"),
    ("logo_6.png", "https://krunker.io/img/logo_6.png"),
    ("icon32x32.png", "https://krunker.io/img/pwa/icon32x32.png"),
    ("icon60x60.png", "https://krunker.io/img/pwa/icon60x60.png"),
    ("icon76x76.png", "https://krunker.io/img/pwa/icon76x76.png"),
    ("icon120x120.png", "https://krunker.io/img/pwa/icon120x120.png"),
    ("icon192x192.png", "https://krunker.io/img/pwa/icon192x192.png"),
    ("icon196x196.png", "https://krunker.io/img/pwa/icon196x196.png"),
    ("icon256x256.png", "https://krunker.io/img/pwa/icon256x256.png"),
    ("icon512x512.png", "https://krunker.io/img/pwa/icon512x512.png"),
    ("icon1024x1024.png", "https://krunker.io/img/pwa/icon1024x1024.png"),
    ("og_1200x630.jpg", "https://assets.krunker.io/promo/og_1200x630.jpg"),
)
ANDROID_RES = "platform/android/app/src/main/res"
IOS_ICON = "platform/ios/KrunkNative/Assets.xcassets/AppIcon.appiconset/icon-1024.png"
# (repo path, source, how): "copy" = the live bytes; ("png", size[, mode]) = resized from the
# source with Lanczos; ("ico", sizes) = a multi-size Windows icon
OUTPUTS = (
    ("ui/img/favicon.png", "favicon.png", "copy"),
    ("ui/img/logo_6.png", "logo_6.png", "copy"),
    ("platform/web/icons/icon-32.png", "icon32x32.png", "copy"),
    ("platform/web/icons/icon-192.png", "icon192x192.png", "copy"),
    ("platform/web/icons/icon-512.png", "icon512x512.png", "copy"),
    ("platform/web/icons/apple-touch-icon.png", "icon1024x1024.png", ("png", 180)),
    ("platform/web/icons/og_1200x630.jpg", "og_1200x630.jpg", "copy"),
    ("assets/img/icon.png", "icon256x256.png", "copy"),
    ("client/icon.ico", "icon1024x1024.png", ("ico", (16, 24, 32, 48, 64, 128, 256))),
    (IOS_ICON, "icon1024x1024.png", ("png", 1024, "RGB")),
    (ANDROID_RES + "/mipmap-mdpi/ic_launcher.png", "icon1024x1024.png", ("png", 48)),
    (ANDROID_RES + "/mipmap-hdpi/ic_launcher.png", "icon1024x1024.png", ("png", 72)),
    (ANDROID_RES + "/mipmap-xhdpi/ic_launcher.png", "icon1024x1024.png", ("png", 96)),
    (ANDROID_RES + "/mipmap-xxhdpi/ic_launcher.png", "icon1024x1024.png", ("png", 144)),
    (ANDROID_RES + "/mipmap-xxxhdpi/ic_launcher.png", "icon1024x1024.png", ("png", 192)),
)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def read_lock():
    pins = {}
    with open(LOCK, encoding="utf8") as f:
        for line in f:
            if line.strip() and not line.startswith("#"):
                name, url, digest, size = line.rstrip("\n").split("\t")
                pins[name] = {"url": url, "sha256": digest, "size": int(size)}
    return pins


def write_lock(pins):
    with open(LOCK, "w", encoding="utf8") as f:
        f.write(LOCK_HEADER)
        for name, _ in SOURCES:
            p = pins[name]
            f.write("%s\t%s\t%s\t%d\n" % (name, p["url"], p["sha256"], p["size"]))


def download(url):
    proxy = os.environ.get("KN_FETCH_PROXY", "")
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({"http": proxy, "https": proxy} if proxy else {}))
    headers = {"User-Agent": os.environ.get("KN_FETCH_UA", DEFAULT_UA),
               "Accept": "image/avif,image/webp,image/apng,image/svg+xml,image/*,*/*;q=0.8",
               "Accept-Language": "en-US,en;q=0.9", "Referer": "https://krunker.io/",
               "Sec-Fetch-Site": "same-origin", "Sec-Fetch-Mode": "no-cors", "Sec-Fetch-Dest": "image"}
    for attempt in range(4):
        try:
            with opener.open(urllib.request.Request(url, headers=headers), timeout=60) as r:
                data = r.read()
            if not data.startswith((b"\x89PNG\r\n\x1a\n", b"\xff\xd8\xff")):
                raise SystemExit("%s: not a PNG or JPEG" % url)
            return data
        except urllib.error.HTTPError as e:
            if e.code not in (429, 500, 502, 503, 504) or attempt == 3:
                raise SystemExit("%s: HTTP %d" % (url, e.code))
        except (urllib.error.URLError, TimeoutError, OSError) as e:
            if attempt == 3:
                raise SystemExit("%s: %s" % (url, e))
        time.sleep(2 ** attempt)
    raise SystemExit("%s: failed" % url)


def fetch_all(stage, pins, verify=True):
    """{name: bytes}: staged copies when their hash matches, else downloaded (and staged)."""
    os.makedirs(stage, exist_ok=True)
    out = {}
    for name, url in SOURCES:
        path = os.path.join(stage, name)
        data = None
        if verify and os.path.isfile(path):
            with open(path, "rb") as f:
                data = f.read()
            if sha256(data) != pins[name]["sha256"]:
                data = None
        if data is None:
            data = download(url)
            time.sleep(0.3)
            if verify and sha256(data) != pins[name]["sha256"]:
                raise SystemExit("%s: sha256 %s != pinned %s (krunker.io changed it: `repin` after a look)"
                                 % (url, sha256(data), pins[name]["sha256"]))
            write_atomic(path, data)
        out[name] = data
    return out


def write_atomic(dest, data):
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    fd, tmp = tempfile.mkstemp(prefix=".kn-brand-", dir=os.path.dirname(dest))
    with os.fdopen(fd, "wb") as f:
        f.write(data)
    os.chmod(tmp, 0o644)
    os.replace(tmp, dest)


def render(data, how):
    from PIL import Image  # only install needs Pillow
    with Image.open(io.BytesIO(data)) as src:
        im = src.convert("RGBA")
    buf = io.BytesIO()
    if how[0] == "ico":
        big = max(how[1])
        im.resize((big, big), Image.LANCZOS).save(buf, "ICO", sizes=[(s, s) for s in how[1]])
    else:
        size = how[1]
        out = im if im.width == size else im.resize((size, size), Image.LANCZOS)
        if len(how) > 2:
            out = out.convert(how[2])
        out.save(buf, "PNG", optimize=True)
    return buf.getvalue()


def cmd_install(a):
    files = fetch_all(a.stage, read_lock())
    for dest, name, how in OUTPUTS:
        data = files[name] if how == "copy" else render(files[name], how)
        path = os.path.join(REPO, dest)
        if os.path.isfile(path):
            with open(path, "rb") as f:
                if f.read() == data:
                    continue
        write_atomic(path, data)
        print("wrote %s (%d bytes, from %s)" % (dest, len(data), name))
    return 0


def cmd_check(_a):
    pins, bad = read_lock(), 0
    for dest, name, how in OUTPUTS:
        if how != "copy":
            continue
        path = os.path.join(REPO, dest)
        if not os.path.isfile(path):
            print("missing %s" % dest)
            bad = 1
            continue
        with open(path, "rb") as f:
            if sha256(f.read()) != pins[name]["sha256"]:
                print("differs %s (not %s)" % (dest, pins[name]["url"]))
                bad = 1
    if not bad:
        print("branding ok")
    return bad


def cmd_repin(a):
    files = fetch_all(a.stage, {}, verify=False)
    write_lock({name: {"url": url, "sha256": sha256(files[name]), "size": len(files[name])} for name, url in SOURCES})
    print("pinned %d files in %s" % (len(SOURCES), os.path.relpath(LOCK, REPO)))
    return 0


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = p.add_subparsers(dest="cmd", required=True)
    for name in ("install", "repin"):
        s = sub.add_parser(name)
        s.add_argument("--stage", default=DEFAULT_STAGE, help="download cache (default %(default)s)")
    sub.add_parser("check")
    a = p.parse_args()
    return {"install": cmd_install, "check": cmd_check, "repin": cmd_repin}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main())
