#!/usr/bin/env python3
"""Krunker game content for ``assets/`` (``make assets-import``, ``make assets-import-v7``).

v3.1.1 mode (default, ``make assets-import``): ``assets/`` is unpacked from Krunker's
``mod.zip`` (assets/README.md), which has no ``textures/previews/``: the item preview images
v3.1.1 serves from its asset host (``UTILS.getPreview(skin, store)`` ->
``assetsUrl("/textures/previews/...")``). This copies that directory from the v3.1.1
``public/`` tree.

v7.1.4 mode (``--v7``, ``make assets-import-v7``): the in-place v7 upgrade
(doc/v7/decisions.md "GLTF and assets"). Copies every directory the v7 client loads game
content from (``V7_ASSET_DIRS``: OBJ/GLTF/GLB models of props, weapons and the 17 cosmetic
item types, their textures and previews, sounds, video textures, user-assets, scares) from the
v7.1.4 ``public/`` tree, writes ``assets/skins.json`` (the v7 store table, 6448 skins, unpacked
from ``public/skins.jspck`` by ``v7_skins.js`` exactly as the v7 client does) and
``assets/previews.json`` (v7's item type enum, ``src/viewer.js``). The table is ``store.skins``
after v7's ``store.js`` loaded (templates and load-time flags applied), so kn_brain, the bots
and the client read v7's own noSpin / noSale / limited / limT values. The skin table write is
refused when it would move an existing store index (the array position is the inventory
``skin_id``): every index of the current ``assets/skins.json`` must keep its ``id``, ``weapon``
and item type (v7 retyped HAT 1 -> HEAD 15 and BODY 2 -> BACK 14; those keep their slot).

v7 serves the player card banners (``textures/playercards/card_<id>.webp``) as WebP, which
the native HTML engine can't decode (stb_image has none; client/src/html/engine/image.c): the
v7 import also writes ``<name>.webp.png`` next to every real WebP it copied (Pillow, the first
frame of animated ones, RGBA), and the engine reads that sidecar for a ``.webp`` URL. The page
and the C UI keep v7's ``.webp`` URLs, which the web build's browser decodes itself.

Both modes copy byte-identical, leave identical files alone and delete nothing; v7 files
replace the v3 files of the same name. The client builds asset URLs with
``html_dom_asset_url()`` (client/include/html_dom.h) and opens files through
``ensure_asset_file()``; deploy/ansible role kn_web syncs ``assets/`` to the asset host and
rehashes ``assets/version.txt``, so the ``?v=`` URLs pick the new files up.

usage: python3 -I tools/ui-import/game_assets.py [--v7] [--assets DIR] [--src PUBLIC] [--check] [--size]
"""

import argparse
import json
import os
import shutil
import subprocess
import sys

KRUNKER_PUBLIC = "/home/user/jails/kru/src/Krunker_v3.1.1/public"
KRUNKER_V7_PUBLIC = "/home/user/jails/kru/src/Krunker_v7.1.4/public"
REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
HERE = os.path.dirname(os.path.abspath(__file__))

# Directories of public/ copied whole (relative paths, '/'-separated).
GAME_ASSET_DIRS = (
    "textures/previews",  # UTILS.getPreview: attach/, cosmetics/, melee/, weapons/
)

# v7.1.4: what the v7 client loads from its asset host (UTILS.assetsUrl / userAssetsUrl).
# Not ui/ material: the page's own images (img/, css/) belong to tools/ui-import's menu
# import (ui/img), maps/ to the map catalogue import.
V7_ASSET_DIRS = (
    "models",       # props + weapons/ melee/ attach/ and the cosmetic dirs (hats body back waist faces
                    # shoes pets charms wrist collectibles tickets); .obj, .gltf (render.js loadAnimMesh)
    "textures",     # model textures, previews/, badges/, playercards/ (.webp), classes/, ...
    "sound",        # weapon/class/skin sounds (snd, killSnd, stepSnd, jumpSnd, reloadSnd, spraySnd)
    "videos",       # skin `video` textures (render.js:1180, videos/video_<n>.mp4)
    "user-assets",  # map/AI user assets (UTILS.userAssetsUrl): model.gltf/.glb, sound.mp3, ...
    "scares",       # jump-scare trigger media
)
V7_SKIN_RETYPES = {15: 1, 14: 2}  # v7 HEAD -> HAT, BACK -> BODY (store.js assetType)


def files_under(root, rel):
    base = os.path.join(root, rel)
    for dirpath, dirnames, filenames in os.walk(base):
        dirnames.sort()
        for name in sorted(filenames):
            yield os.path.relpath(os.path.join(dirpath, name), root).replace(os.sep, "/")


def same_file(dest, src):
    """Byte-identical without reading both files whole when the sizes already differ."""
    try:
        if os.path.getsize(dest) != os.path.getsize(src):
            return False
        with open(dest, "rb") as a, open(src, "rb") as b:
            while True:
                x = a.read(1 << 20)
                if x != b.read(1 << 20):
                    return False
                if not x:
                    return True
    except OSError:
        return False


def same_content(path, data):
    try:
        if os.path.getsize(path) != len(data):
            return False
        with open(path, "rb") as fh:
            return fh.read() == data
    except OSError:
        return False


def write_atomic(dest, data=None, src=None):
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    tmp = dest + ".tmp"
    if src is not None:
        shutil.copyfile(src, tmp)
    else:
        with open(tmp, "wb") as fh:
            fh.write(data)
    os.replace(tmp, dest)


def mib(n):
    return "%.1f MiB" % (n / 1048576.0)


def sync_dirs(src_root, assets, dirs, check, quiet=False):
    """Copy dirs of src_root into assets. Returns (total, total_bytes, differ, differ_bytes, new)."""
    total = total_bytes = differ = differ_bytes = new = 0
    for rel_dir in dirs:
        if not os.path.isdir(os.path.join(src_root, rel_dir)):
            sys.exit("game_assets.py: %s/%s not found" % (src_root, rel_dir))
        for rel in files_under(src_root, rel_dir):
            src = os.path.join(src_root, rel)
            size = os.path.getsize(src)
            total += 1
            total_bytes += size
            dest = os.path.join(assets, rel)
            if same_file(dest, src):
                continue
            differ += 1
            differ_bytes += size
            if not os.path.exists(dest):
                new += 1
            if check:
                if not quiet:
                    print("%s: %s" % ("missing" if not os.path.exists(dest) else "differs", rel))
                continue
            write_atomic(dest, src=src)
    return total, total_bytes, differ, differ_bytes, new


# ---------------------------------------------------------------- WebP sidecars

def is_webp(path):
    with open(path, "rb") as fh:
        head = fh.read(12)
    return head[:4] == b"RIFF" and head[8:12] == b"WEBP"


def webp_sidecars(assets, dirs, check, quiet=False):
    """<name>.webp.png for every real WebP under dirs of assets (a .webp holding PNG data needs
    none: stb_image decodes it). Returns (sidecars, differ)."""
    try:
        import io
        from PIL import Image
    except ImportError:
        sys.exit("game_assets.py: Pillow (python3-pillow) is required to decode the .webp player cards for the native client")
    count = differ = 0
    for rel_dir in dirs:
        for rel in files_under(assets, rel_dir):
            if not rel.endswith(".webp") or not is_webp(os.path.join(assets, rel)):
                continue
            count += 1
            with Image.open(os.path.join(assets, rel)) as im:
                im.seek(0)
                out = io.BytesIO()
                im.convert("RGBA").save(out, "PNG")
            dest = os.path.join(assets, rel + ".png")
            if same_content(dest, out.getvalue()):
                continue
            differ += 1
            if check:
                if not quiet:
                    print("%s: %s.png" % ("missing" if not os.path.exists(dest) else "differs", rel))
                continue
            write_atomic(dest, data=out.getvalue())
    return count, differ


# ---------------------------------------------------------------- v7 skin table

def v7_skins(v7_root):
    node = shutil.which("node")
    if not node:
        sys.exit("game_assets.py: node is required to unpack skins.jspck (tools/ui-import/v7_skins.js)")
    out = subprocess.run([node, os.path.join(HERE, "v7_skins.js"), v7_root], check=True, stdout=subprocess.PIPE).stdout
    skins = json.loads(out)
    # Cross-check against the reference's readable dump (skins.json = the same table after
    # store.js loaded, plus `thumbnail`): every entry must be identical.
    ref_path = os.path.join(v7_root, "skins.json")
    if os.path.exists(ref_path):
        with open(ref_path, encoding="utf-8") as fh:
            ref = json.load(fh)
        if len(ref) != len(skins):
            sys.exit("game_assets.py: skins.jspck has %d skins, skins.json %d" % (len(skins), len(ref)))
        for i, (a, b) in enumerate(zip(skins, ref)):
            b = dict(b)
            b.pop("thumbnail", None)
            if a != b:
                sys.exit("game_assets.py: skins.jspck (store.js applied) and skins.json disagree at index %d" % i)
    return out, skins


def base_type(t):
    t = t or 0
    return V7_SKIN_RETYPES.get(t, t)


def skin_index_report(old, new):
    """Index stability of the store table (array position = inventory skin_id).

    Returns (errors, retyped, renamed): errors are indices whose item changed (id, weapon or
    item type other than the v7 HAT->HEAD / BODY->BACK retype) or disappeared."""
    errors, retyped, renamed = [], 0, []
    if len(new) < len(old):
        errors.append("the new table is shorter (%d < %d)" % (len(new), len(old)))
    for i in range(min(len(old), len(new))):
        a, b = old[i], new[i]
        if a.get("id") != b.get("id") or a.get("weapon") != b.get("weapon") or base_type(a.get("type")) != base_type(b.get("type")):
            errors.append("index %d: %r (type %s) -> %r (type %s)" % (i, a.get("name"), a.get("type", 0), b.get("name"), b.get("type", 0)))
            continue
        if (a.get("type") or 0) != (b.get("type") or 0):
            retyped += 1
        if a.get("name") != b.get("name"):
            renamed.append((i, a.get("name"), b.get("name")))
    return errors, retyped, renamed


def import_v7_tables(v7_root, assets, check):
    """assets/skins.json + assets/previews.json. Returns the number of differing files."""
    raw, skins = v7_skins(v7_root)
    differ = 0
    dest = os.path.join(assets, "skins.json")
    old = []
    if os.path.exists(dest):
        with open(dest, encoding="utf-8") as fh:
            old = json.load(fh)
    errors, retyped, renamed = skin_index_report(old, skins)
    types = {}
    for s in skins:
        types[s.get("type", 0)] = types.get(s.get("type", 0), 0) + 1
    print("skins.json: %d skins (types %s), %d existing indices checked, %d retyped HAT/BODY -> HEAD/BACK, %d renamed"
          % (len(skins), ",".join("%d:%d" % kv for kv in sorted(types.items())), min(len(old), len(skins)), retyped, len(renamed)))
    for i, a, b in renamed:
        print("  renamed %d: %r -> %r" % (i, a, b))
    if errors:
        for e in errors[:40]:
            print("  INDEX SHIFT %s" % e, file=sys.stderr)
        sys.exit("game_assets.py: refusing to write assets/skins.json: %d store indices would change (inventory skin_id)" % len(errors))
    if not same_content(dest, raw):
        differ += 1
        if check:
            print("differs: skins.json")
        else:
            write_atomic(dest, data=raw)

    with open(os.path.join(v7_root, "previews.json"), "rb") as fh:
        previews = fh.read()
    dest = os.path.join(assets, "previews.json")
    if not same_content(dest, previews):
        differ += 1
        if check:
            print("differs: previews.json")
        else:
            write_atomic(dest, data=previews)
    return differ


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--assets", default=os.path.join(REPO, "assets"), help="asset tree (default <repo>/assets)")
    ap.add_argument("--src", default=None, help="Krunker public/ tree (default: v3.1.1, or v7.1.4 with --v7)")
    ap.add_argument("--v7", action="store_true", help="import the Krunker v7.1.4 content set (models, textures, sounds, skins.json, ...)")
    ap.add_argument("--check", action="store_true", help="only report missing/different files (exit 1 if any)")
    ap.add_argument("--size", action="store_true", help="with --check: only print the size summary, not every file")
    args = ap.parse_args()

    src = args.src or (KRUNKER_V7_PUBLIC if args.v7 else KRUNKER_PUBLIC)
    if not os.path.isdir(src):
        sys.exit("game_assets.py: %s not found" % src)
    dirs = V7_ASSET_DIRS if args.v7 else GAME_ASSET_DIRS

    total, total_bytes, written, written_bytes, new = sync_dirs(src, args.assets, dirs, args.check, quiet=args.size)
    tables = import_v7_tables(os.path.dirname(src.rstrip("/")), args.assets, args.check) if args.v7 else 0
    sidecars, sidecars_differ = webp_sidecars(args.assets, dirs, args.check, quiet=args.size) if args.v7 else (0, 0)

    verb = "differ" if args.check else "copied"
    print("game assets%s: %d files, %s (%s); %d %s (%d new), %s"
          % (" v7" if args.v7 else "", total, mib(total_bytes), ", ".join(dirs), written, verb, new, mib(written_bytes)))
    if args.v7:
        print("tables: %d %s (skins.json, previews.json)" % (tables, verb))
        print("webp sidecars: %d .webp.png, %d %s" % (sidecars, sidecars_differ, "differ" if args.check else "written"))
    if args.check and (written or tables or sidecars_differ):
        sys.exit(1)


if __name__ == "__main__":
    main()
