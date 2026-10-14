#!/usr/bin/env python3
"""Write the Krunker v10.0.2 table fixtures shared/tests/data/v10/{weapons,classes}.json.

The v10 client bundle is untrusted and never executed: its weaponConfig and classConfig arrays
are read statically by the acorn extractor (~/.cache/kn-v10-tools/extract.js, no eval; resolves
identifiers and babel defineProperty chains) into one JSON file, e.g.

    node ~/.cache/kn-v10-tools/extract.js ~/.cache/kn-v10/v10.js "Sniper Rifle" "Triggerman" > v10.json
    python3 -I tools/kref/v10-fixtures.py v10.json shared/tests/data/v10

This applies v10's weaponConfig post-processing loop (v10:101949-101956: id, adSpread 0, reloY 1,
movSpread 1, lsMlt 0.5, burst false / burstC 0 / burstR 50 when burst is unset), drops the
extractor's "__line" keys, and writes the same shape as tools/kref/v7-dump.js does for v7, so
shared/tests/weapons_v7_test.c checks both (its "v10" argument).
"""
import json
import os
import sys


def tables(path):
    data = json.load(open(path))
    weapons = classes = None
    for table in data:
        items = table.get("items", [])
        if items and items[0].get("name") == "Sniper Rifle":
            weapons = items
        if items and items[0].get("name") == "Triggerman":
            classes = items
    if weapons is None or classes is None:
        sys.exit("v10-fixtures: the extractor output has no weaponConfig / classConfig array")
    return weapons, classes


def clean(entry):
    return {k: v for k, v in entry.items() if k != "__line"}


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    weapons, classes = tables(sys.argv[1])
    out = []
    for i, w in enumerate(weapons):
        w = clean(w)
        w["id"] = i
        w.setdefault("adSpread", 0)
        w.setdefault("reloY", 1)
        w.setdefault("movSpread", 1)
        w.setdefault("lsMlt", 0.5)
        if w.get("burst") is None:
            w["burst"], w["burstC"], w["burstR"] = False, 0, 50
        out.append(w)
    os.makedirs(sys.argv[2], exist_ok=True)
    with open(os.path.join(sys.argv[2], "weapons.json"), "w") as f:
        json.dump(out, f, indent=1)
        f.write("\n")
    with open(os.path.join(sys.argv[2], "classes.json"), "w") as f:
        json.dump([clean(c) for c in classes], f, indent=1)
        f.write("\n")
    print("v10-fixtures: %d weapons, %d classes" % (len(out), len(classes)))


if __name__ == "__main__":
    main()
