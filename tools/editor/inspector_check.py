#!/usr/bin/env python3
"""Object inspector and environment panels vs Krunker v7.1.4 (doc/v7/editor.md WP3).

    python3 -I tools/editor/inspector_check.py --bin <build>/client_bin [--root .] [--keep DIR] [-v]

One headless native editor run (EGL, no window on any display), two checks:

1. Panel dumps. For every prefab v7's capture created (doc/ref/krunker-v7.1.4/editor/prefabs/
   <id>.right.dump.txt): a cleared map, KE.createObjects[id](), the right (object) panel dumped
   with the editor script's dump:right (the format of tools/editor/v7ref.js); and the
   Environment / Terrain / Zone / Ocean panels (right-<name>.dump.txt). Reported twice: identical
   lines, and identical once the documented differences below are applied.
2. Edits. Every case of tools/editor/inspector_edits.json: a cleared map, the prefab, each
   [label, value] as a user edit of the right panel's control of that label (set:<label>[#n]=<v>),
   then objdump. Each object must equal the second form of v7's golden pair in
   doc/ref/krunker-v7.1.4/editor/inspector-edits.json (v7ref.js state "edits":
   ObjectInstance.deserialize(edited).serialize(), the form the native editor holds objects in).

Documented differences (doc/v7/editor.md 1.13; editor_inspector.c's header):
- AI_SPAWNER "Offset Shot" shows the object's value (v7: always off, misspelt binding).
- ENVIRONMENT_ZONE "Emissive" shows the zone's colour (v7: "#ffffff", no getter).
- Edits: "sci" is the script id (v7 drops it); "s" is not compared (no case edits a size; model
  sizes come from the loaded model in v7, from the generator's bounds natively).
"""
import argparse
import glob
import json
import os
import shutil
import subprocess
import sys
import tempfile

PANELS = ("environment", "terrain", "zone", "ocean")


def json_eq(a, b):
    if isinstance(a, bool) or isinstance(b, bool):
        return isinstance(a, bool) and isinstance(b, bool) and a == b
    if isinstance(a, (int, float)) and isinstance(b, (int, float)):
        return abs(a - b) <= 1e-9 * max(1.0, abs(a))
    if isinstance(a, list) and isinstance(b, list):
        return len(a) == len(b) and all(json_eq(x, y) for x, y in zip(a, b))
    if isinstance(a, dict) and isinstance(b, dict):
        return a.keys() == b.keys() and all(json_eq(a[k], b[k]) for k in a)
    return a == b


def documented(name, lines):
    if name == "55":
        lines = [l.replace("Offset Shot : switch=0", "Offset Shot : switch=1") for l in lines]
    if name == "79":
        lines = [l.replace("L[subsubsection] Emissive : color=#ffffff", "L[subsubsection] Emissive : color=#000000") for l in lines]
    return lines


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True, help="client_bin")
    ap.add_argument("--root", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
    ap.add_argument("--keep", help="keep the dumps, objdumps and client log in this directory")
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    ref = os.path.join(root, "doc", "ref", "krunker-v7.1.4", "editor")
    cases = json.load(open(os.path.join(root, "tools", "editor", "inspector_edits.json")))["cases"]
    golden = json.load(open(os.path.join(ref, "inspector-edits.json")))
    want = {c["name"]: p[1] for c, p in zip(golden["cases"], golden["maps"]["inspector-edits"])}
    prefabs = sorted(int(os.path.basename(f).split(".")[0]) for f in glob.glob(os.path.join(ref, "prefabs", "*.right.dump.txt")))

    out = a.keep or tempfile.mkdtemp(prefix="kn-inspector-check-", dir=os.path.expanduser("~/.cache"))
    os.makedirs(out, exist_ok=True)
    script = []
    for i in prefabs:
        script += ["clear", "add:%d" % i, "select:0", "dump:right:%s" % os.path.join(out, "prefab-%d.txt" % i)]
    script.append("clear")
    for p in PANELS:
        script += ["panel:%s" % p, "dump:right:%s" % os.path.join(out, "panel-%s.txt" % p)]
    for c in cases:
        script += ["clear", "add:%d" % c["prefab"], "select:0"]
        script += ["set:%s=%s" % (lab, val) for lab, val in c["set"]]
        script.append("objdump:%s" % os.path.join(out, "edit-%s.txt" % c["name"]))
    env = dict(os.environ)
    for k in ("DISPLAY", "WAYLAND_DISPLAY"):
        env.pop(k, None)
    env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": os.path.join(out, "cfg"),
                "KRUNKNATIVE_SCREENSHOT": os.path.join(out, "shot.png"),
                "KRUNKNATIVE_EDITOR_SCRIPT": ";".join(script) + ";wait:0.3"})
    with open(os.path.join(out, "client.log"), "w") as log:
        r = subprocess.run([os.path.abspath(a.bin), "--offline", "--editor", "new"], cwd=root, env=env,
                           stdout=log, stderr=subprocess.STDOUT, timeout=600)
    if r.returncode != 0:
        print("client_bin exited with %d (log: %s)" % (r.returncode, os.path.join(out, "client.log")))
        return 1
    failed = 0

    # 1. panel dumps
    names = [str(i) for i in prefabs] + list(PANELS)
    same = same_doc = 0
    for n in names:
        mine = os.path.join(out, ("prefab-%s.txt" % n) if n.isdigit() else ("panel-%s.txt" % n))
        theirs = os.path.join(ref, "prefabs", n + ".right.dump.txt") if n.isdigit() else os.path.join(ref, "right-%s.dump.txt" % n)
        try:
            got = open(mine).read().splitlines()
        except OSError:
            got = None
        exp = open(theirs).read().splitlines()
        if got == exp:
            same += 1
        if got is not None and documented(n, got) == documented(n, exp):
            same_doc += 1
            if a.verbose:
                print("ok   panel %s" % n)
        else:
            failed += 1
            print("FAIL panel %s (diff %s %s)" % (n, theirs, mine))
    print("panel dumps: %d/%d identical to v7.1.4, %d/%d with the documented differences" % (same, len(names), same_doc, len(names)))

    # 2. edits
    missing = [l.rstrip() for l in open(os.path.join(out, "client.log")) if l.startswith("[editor] set: no control")]
    for l in missing:
        print(l)
    failed += len(missing)
    ok = 0
    for c in cases:
        try:
            line = open(os.path.join(out, "edit-%s.txt" % c["name"])).readline()
            got = json.loads(line.split(" ", 1)[1])
        except (OSError, IndexError, ValueError):
            failed += 1
            print("FAIL edit %s: no object" % c["name"])
            continue
        exp = json.loads(json.dumps(want[c["name"]]))
        got.pop("s", None)
        exp.pop("s", None)
        sets = dict(c["set"])
        if "Script #" in sets and "sci" not in exp:
            exp["sci"] = int(sets["Script #"])
        if json_eq(got, exp):
            ok += 1
            if a.verbose:
                print("ok   edit %s" % c["name"])
        else:
            failed += 1
            print("FAIL edit %s\n  got:  %s\n  want: %s" % (c["name"], json.dumps(got, sort_keys=True), json.dumps(exp, sort_keys=True)))
    print("inspector edits: %d/%d match v7.1.4" % (ok, len(cases)))

    if not a.keep and not failed:
        shutil.rmtree(out, ignore_errors=True)
    elif not a.keep:
        print("dumps and client log kept in %s" % out)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
