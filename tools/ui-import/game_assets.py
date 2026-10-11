#!/usr/bin/env python3
"""Krunker v3.1.1 game content that mod.zip lacks (``make assets-import``).

``assets/`` is unpacked from Krunker's ``mod.zip`` (assets/README.md), which has no
``textures/previews/``: the item preview images v3.1.1 serves from its asset host
(``UTILS.getPreview(skin, store)`` -> ``assetsUrl("/textures/previews/...")``) for the
store, inventory, Change Loadout skin pickers, market, trades, spin rewards, profiles
and item popups. This copies the directories below from the v3.1.1 ``public/`` tree into
``assets/`` at the same relative path, byte-identical; files already identical are left
alone, nothing is deleted. The client builds their URLs with ``html_dom_asset_url()``
(client/include/html_dom.h); deploy/ansible role kn_web syncs ``assets/`` to the asset
host and rehashes ``assets/version.txt``, so the ``?v=`` URLs pick the new files up.

usage: python3 -I tools/ui-import/game_assets.py [--assets DIR] [--src PUBLIC] [--check]
"""

import argparse
import os
import sys

KRUNKER_PUBLIC = "/home/user/jails/kru/src/Krunker_v3.1.1/public"
REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# Directories of public/ copied whole (relative paths, '/'-separated).
GAME_ASSET_DIRS = (
    "textures/previews",  # UTILS.getPreview: attach/, cosmetics/, melee/, weapons/
)


def files_under(root, rel):
    base = os.path.join(root, rel)
    for dirpath, dirnames, filenames in os.walk(base):
        dirnames.sort()
        for name in sorted(filenames):
            yield os.path.relpath(os.path.join(dirpath, name), root).replace(os.sep, "/")


def same_content(path, data):
    try:
        if os.path.getsize(path) != len(data):
            return False
        with open(path, "rb") as fh:
            return fh.read() == data
    except OSError:
        return False


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--assets", default=os.path.join(REPO, "assets"), help="asset tree (default <repo>/assets)")
    ap.add_argument("--src", default=KRUNKER_PUBLIC, help="Krunker v3.1.1 public/ tree")
    ap.add_argument("--check", action="store_true", help="only report missing/different files (exit 1 if any)")
    args = ap.parse_args()

    if not os.path.isdir(args.src):
        sys.exit("game_assets.py: %s not found" % args.src)
    total = written = 0
    for rel_dir in GAME_ASSET_DIRS:
        if not os.path.isdir(os.path.join(args.src, rel_dir)):
            sys.exit("game_assets.py: %s/%s not found" % (args.src, rel_dir))
        for rel in files_under(args.src, rel_dir):
            total += 1
            with open(os.path.join(args.src, rel), "rb") as fh:
                data = fh.read()
            dest = os.path.join(args.assets, rel)
            if same_content(dest, data):
                continue
            written += 1
            if args.check:
                print("differs: %s" % rel)
                continue
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            tmp = dest + ".tmp"
            with open(tmp, "wb") as fh:
                fh.write(data)
            os.replace(tmp, dest)
    verb = "differ" if args.check else "copied"
    print("game assets: %d files (%s), %d %s" % (total, ", ".join(GAME_ASSET_DIRS), written, verb))
    if args.check and written:
        sys.exit(1)


if __name__ == "__main__":
    main()
