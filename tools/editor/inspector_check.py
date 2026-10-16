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

The goldens are v7.1.4's (doc/ref/krunker-v7.1.4/editor/, never rewritten). The expectation is
v10.0.2's: the v7 golden with the v10 deltas of V10_SUBS / V10_INSERTS applied (doc/v10/
editor-social.md 1.3 and 1.4: Dynamic, Spin Axis Z, Boss, Can Contact Hit, Crate Type, Bounce
-10..10, Fade Time, the 13 conditions and 75 actions, the v10 spray list, and the v10 object
keys that v7's dumps do not show: ambFadeTime is written as 0 on ENVIRONMENT_ZONE, E10:8720).
Every v10 delta is listed here once; a line that differs otherwise fails the check.
"""
import argparse
import difflib
import glob
import json
import os
import re
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


# v10.0.2 deltas on v7.1.4's dump lines: (regex, replacement) substitutions, then insertions of a
# new line after the first golden line matching an anchor (only for the listed dump names).
V10_SUBS = [
    (r"L\[\] Spin Axis : select#2\[Y\.\.X\]=Y$", "L[] Spin Axis : select#3[Y..Z]=Y"),                     # IDX:15442
    (r"Action : select#66\[Give Player Score\.\.Mode Specific\]", "Action : select#69[Give Player Score..Set Entity Value]"),  # 75 actions
    (r"Condition : select#12\[None\.\.Player Inventory Value\]", "Condition : select#13[None..Custom Action]"),  # 13 conditions
    (r"Bounce : sval=1 range\[-3,3,0\.1\]=1$", "Bounce : sval=1 range[-10,10,0.1]=1"),                       # IDX:15348
]
V10_INSERTS = [
    # Dynamic (CUBE, TRIGGER; IDX:15953-15962), after the Step Sound switch
    (("0", "29"), r"L\[\] Step Sound ", 0, "L[] Dynamic : switch=0"),
    # AI Boss and Can Contact Hit (IDX:17040, 17226)
    (("55",), r"L\[subsection\] Target other AI ", 0, "L[subsection] Boss : switch=0"),
    (("55",), r"L\[subsection\] Can Melee ", 0, "L[subsection] Can Contact Hit : switch=0"),
    # Crate Type (IDX:16786-16829), the first line of Technical & Logic
    (("90",), r"F Technical & Logic", 1, "L[] Crate Type : select#4[Random..Power-ups]=Random"),
    # Environment Fade Time (IDX:18990, 20052): the ENVIRONMENT_ZONE dump and the Environment panel
    (("79", "environment"), r"B Select Sound", 0, "L[] Fade Time : sval=0 range[0,10,0.1]=0"),
]


def v10_expected(name, lines):
    """The v10.0.2 expectation for dump `name` (prefab id or panel name), from v7.1.4's lines."""
    out = []
    for l in lines:
        for pat, rep in V10_SUBS:
            l = re.sub(pat, rep, l)
        out.append(l)
    for names, anchor, offset, new in V10_INSERTS:
        if name not in names:
            continue
        for i, l in enumerate(out):
            if re.search(anchor, l):
                out.insert(i + 1 + offset, new)
                break
        else:
            out.append("!! anchor %r missing for %s" % (anchor, name))
    return out


def documented(name, lines):
    # the spray list is v10's own (556 options, shipped in the skins data, not v7's 466): only the
    # select's shape is compared, on both sides
    lines = [re.sub(r"Spray : select#\d+\[.*\]=", "Spray : select#*[*]=", l) for l in lines]
    # the weapon list ends at Charge Rifle (v7.1.4) or Compressor (v10: weapons 29 Boulder, 30
    # Compressor, the game config's data in editor_data.c): the Weaponbox "Type" select's shape only
    lines = [re.sub(r"Type : select#(28|30)\[Sniper Rifle\.\.(Charge Rifle|Compressor)\]", "Type : select#*[Sniper Rifle..*]", l) for l in lines]
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

    # 1. panel dumps (v10.0.2's expectation: the v7.1.4 golden with the V10_SUBS / V10_INSERTS deltas)
    names = [str(i) for i in prefabs] + list(PANELS)
    same = same_doc = 0
    for n in names:
        mine = os.path.join(out, ("prefab-%s.txt" % n) if n.isdigit() else ("panel-%s.txt" % n))
        theirs = os.path.join(ref, "prefabs", n + ".right.dump.txt") if n.isdigit() else os.path.join(ref, "right-%s.dump.txt" % n)
        try:
            got = open(mine).read().splitlines()
        except OSError:
            got = None
        exp = v10_expected(n, open(theirs).read().splitlines())
        if got is not None and got == exp:
            same += 1
            if a.verbose:
                print("ok   panel %s" % n)
        elif got is not None and documented(n, got) == documented(n, exp):
            same_doc += 1
            if a.verbose:
                print("ok   panel %s (documented differences)" % n)
        else:
            failed += 1
            print("FAIL panel %s (expected v10 %s, got %s)" % (n, theirs, mine))
            if got is not None:
                for l in difflib.unified_diff(exp, got, "v10 expected", "got", lineterm="", n=0):
                    print("    " + l)
    print("panel dumps: %d/%d identical to the v10.0.2 expectation (v7.1.4 + the v10 deltas), %d more with the documented differences (Offset Shot, Emissive, spray list, weapon list)" % (same, len(names), same_doc))

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
        if c["prefab"] == 79:
            exp.setdefault("ambFadeTime", 0)  # v10 writes ambFadeTime on ENVIRONMENT_ZONE (E10:8720)
        if json_eq(got, exp):
            ok += 1
            if a.verbose:
                print("ok   edit %s" % c["name"])
        else:
            failed += 1
            print("FAIL edit %s\n  got:  %s\n  want: %s" % (c["name"], json.dumps(got, sort_keys=True), json.dumps(exp, sort_keys=True)))
    print("inspector edits: %d/%d match the v10.0.2 expectation (v7.1.4 golden + ambFadeTime on ENVIRONMENT_ZONE)" % (ok, len(cases)))

    if not a.keep and not failed:
        shutil.rmtree(out, ignore_errors=True)
    elif not a.keep:
        print("dumps and client log kept in %s" % out)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
