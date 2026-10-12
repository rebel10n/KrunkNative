#!/usr/bin/env python3
"""Merge the static inventory, the v7 dynamic probe and the KN runtime misses into missing.tsv.
usage: python3 -I merge.py <cand.tsv> <probe.jsonl> <runs-dir> <out-missing.tsv>"""
import collections
import glob
import json
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
A = os.path.join(REPO, "assets") + "/"
U = os.path.join(REPO, "ui") + "/"
V7 = "/home/user/jails/kru/src/Krunker_v7.1.4/public/"
GAME_DIRS = ("textures/", "models/", "sound/", "videos/", "user-assets/", "scares/", "img/")

cand, probe, runs, out = sys.argv[1:5]
rows = {}  # path -> {cat, sources:set, maps:set, via:set}


def add(path, cat, source, via, mapname=None):
    path = path.lstrip("/").split("?")[0]
    if not path.startswith(GAME_DIRS):
        return
    r = rows.setdefault(path, {"cat": cat, "sources": [], "maps": set(), "via": set()})
    if source and source not in r["sources"] and len(r["sources"]) < 4:
        r["sources"].append(source)
    if mapname:
        r["maps"].add(mapname)
    r["via"].add(via)


for line in open(cand, encoding="utf8"):
    cat, path, src = line.rstrip("\n").split("\t")
    m = None
    if src.startswith("map "):
        m = src.split(" ")[1].replace(".json", "").replace("custom/", "")
    add(path, cat, src, "static", m)

for line in open(probe, encoding="utf8"):
    o = json.loads(line)
    p = o["path"].lstrip("/")
    if o["status"] == 200:
        continue
    cat = "probe-" + p.split("/")[0]
    add(p, rows.get(p, {}).get("cat", cat), "v7 client on map " + o["map"], "v7-probe", o["map"])

for f in glob.glob(os.path.join(runs, "*.miss")):
    m = os.path.basename(f)[:-len(".miss")].split("-", 1)[-1]
    for line in open(f, encoding="utf8"):
        p = line.rstrip("\n").split("\t")[1].split("assets/", 1)[-1]
        if p.endswith(".mtl"):
            continue  # fast_obj looks for the .mtl next to every OBJ; v7's OBJLoader never does
        add(p, rows.get(p, {}).get("cat", "kn-" + p.split("/")[0]), "KN client on map " + m, "kn-run", m)


def exists_kn(p):
    return os.path.exists(A + p) or (p.startswith("img/") and (os.path.exists(U + p) or os.path.exists(A + p)))


missing = []
for p, r in rows.items():
    if exists_kn(p):
        continue
    in_v7 = os.path.exists(V7 + p)
    missing.append((r["cat"], p, "ui" if p.startswith("img/") else "assets", "v7-public" if in_v7 else "live-only?",
                    ",".join(sorted(r["via"])), ",".join(sorted(r["maps"]))[:300], " | ".join(r["sources"])[:300]))
missing.sort()
with open(out, "w", encoding="utf8") as f:
    f.write("# category\tpath\troot\tavailability\tfound_by\tmaps\tsources\n")
    for row in missing:
        f.write("\t".join(row) + "\n")
c = collections.Counter((m[0], m[3]) for m in missing)
for k in sorted(c):
    print("%-22s %-11s %5d" % (k[0], k[1], c[k]))
print("total missing", len(missing), "of", len(rows), "referenced")
