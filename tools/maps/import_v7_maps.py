#!/usr/bin/env python3
"""Imports the Krunker v7.1.4 official map thumbnails (doc/v7/map-logic.md §4.6, WP-A step A6).

    python3 -I tools/maps/import_v7_maps.py [--check]

Reads the official list of v7 src/data/maps.js (name, fileName; the index is the v7 mapIndex) and
copies, unmodified:

- public/img/maps/map_<i>.png  -> ui/img/maps/map_<i>.png  (the Host Game / Games window
                                                             thumbnails, by v7 mapIndex)

The map JSON is not imported here: the official maps are the v10.0.2 files under
assets/maps/official/ (tools/krunker/fetch_maps.py, pinned in tools/krunker/maps.lock.json), and the
catalogue (shared/src/map_catalog.c) names them. --check only verifies the thumbnails are current.
"""

import filecmp
import os
import re
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
V7 = "/home/user/jails/kru/src/Krunker_v7.1.4"


def official_maps():
    with open(os.path.join(V7, "src", "data", "maps.js"), "r", encoding="utf-8") as fh:
        text = fh.read()
    return re.findall(r'name: "([^"]+)",\s*fileName: "([^"]+)"', text)


def main():
    check = "--check" in sys.argv[1:]
    maps = official_maps()
    if len(maps) != 32:
        print("expected 32 official maps in data/maps.js, found %d" % len(maps))
        return 1
    copies = []
    for index, (name, file_name) in enumerate(maps):
        thumb = os.path.join(V7, "public", "img", "maps", "map_%d.png" % index)
        if os.path.exists(thumb):
            copies.append((thumb, os.path.join(REPO, "ui", "img", "maps", "map_%d.png" % index)))
    stale = 0
    for src, dst in copies:
        if os.path.exists(dst) and filecmp.cmp(src, dst, shallow=False):
            continue
        stale += 1
        if check:
            print("stale: %s" % os.path.relpath(dst, REPO))
            continue
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copyfile(src, dst)
    print("%s %d of %d thumbnails (%d maps)" % ("stale" if check else "copied", stale, len(copies), len(maps)))
    return 1 if check and stale else 0


if __name__ == "__main__":
    sys.exit(main())
