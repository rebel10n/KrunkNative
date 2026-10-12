#!/usr/bin/env python3
"""Tools and Prefabs panels vs Krunker v7.1.4 (doc/v7/editor.md WP6, client/src/editor/editor_tools.c).

    python3 -I tools/editor/tools_check.py --bin <build>/client_bin [--root .] [--keep DIR] [-v]

One headless native editor run (EGL, no window on any display), three checks:

1. Panel dumps: Component > Tools and Object > Prefab > Tools (dump:right) against
   doc/ref/krunker-v7.1.4/editor/right-tools.dump.txt / right-prefab.dump.txt.
2. Optimize Faces: every optimizeFaces case of tools/editor/tools_cases.json imported into a fresh
   map (tool:import), the tool run (tool:optimizeFaces), the alert ("N% Face Reduction") and the
   objects compared with v7's (doc/ref/krunker-v7.1.4/editor/tools/<case>.json, tools/editor/v7ref.js
   state "tools").
3. Color Merging: v7 reuses its object loop's index for the palette loops (objects are skipped or
   visited again). The check models mergeColors here twice: with v7's loop it must reproduce v7's
   capture exactly (alert and objects), which validates the model; without it (every object once,
   the native fix) it gives the native expectation (alert and objects).

Documented differences (doc/v7/editor.md 1.13; editor_tools.c's header):
- Tools "Has Ceiling" shows / edits frameCeiling (v7 binds frameThickness, so it shows on).
- Color Merging visits every object once (above).
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile


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


def documented_tools(lines):
    return [l.replace("L[] Has Ceiling : switch=1", "L[] Has Ceiling : switch=0") for l in lines]


def js_parse_hex(s):
    m = re.match(r"\s*([0-9a-fA-F]+)", s)
    return int(m.group(1), 16) if m else None


def color_of(o, key, default):
    v = o.get(key, default)
    if isinstance(v, (int, float)) and not isinstance(v, bool):
        v = "#" + ("000000" + format(int(v), "x"))[-6:]
    return v


def merge_colors(objects, rng, v7_loop):
    """Editor.mergeColors over serialize() objects; v7_loop = v7's shared loop index."""
    objs = json.loads(json.dumps(objects))
    pal = ["#FFFFFF", "#000000"]
    count = 0
    i = 0
    steps = 0

    def one(o, key, skip, idx):
        nonlocal count
        col = color_of(o, key, skip)
        if col == skip or col is None:
            return idx
        col = col.upper()
        o[key] = col
        if col not in pal:
            x = js_parse_hex(col[1:])
            found = False
            k = 0
            while k < len(pal):
                y = js_parse_hex(pal[k][1:])
                if x is not None and y is not None and abs(x - y) <= rng:
                    count += 1
                    o[key] = pal[k]
                    found = True
                    break
                k += 1
            if not found:
                pal.append(col)
            if v7_loop:
                idx = k
        return idx

    while i < len(objs):
        steps += 1
        if steps > 100000:
            raise RuntimeError("mergeColors model does not terminate")
        o = objs[i]
        i = one(o, "c", "#FFFFFF", i)
        i = one(o, "e", "#000000", i)
        i += 1
    for o in objs:  # serialize(): the default colours are not written
        if o.get("c") == "#FFFFFF":
            del o["c"]
        if o.get("e") == "#000000":
            del o["e"]
    return "%d Colors Merged" % count, objs


def read_objdump(path):
    out = []
    for line in open(path):
        if line.startswith("GROUP "):
            continue
        out.append(json.loads(line.split(" ", 1)[1]))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True, help="client_bin")
    ap.add_argument("--root", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
    ap.add_argument("--keep", help="keep the dumps, objdumps and client log in this directory")
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    ref = os.path.join(root, "doc", "ref", "krunker-v7.1.4", "editor")
    cases = json.load(open(os.path.join(root, "tools", "editor", "tools_cases.json")))["cases"]

    out = a.keep or tempfile.mkdtemp(prefix="kn-tools-check-", dir=os.path.expanduser("~/.cache"))
    os.makedirs(out, exist_ok=True)
    script = ["panel:tools", "dump:right:%s" % os.path.join(out, "panel-tools.txt"),
              "panel:prefab", "dump:right:%s" % os.path.join(out, "panel-prefab.txt")]
    for c in cases:
        mp = os.path.join(out, "map-%s.json" % c["name"])
        json.dump(c["map"], open(mp, "w"))
        tool = "mergeColors:%s" % c.get("range", 1500) if c["tool"] == "mergeColors" else c["tool"]
        script += ["clear", "tool:import:%s" % mp, "objdump:%s" % os.path.join(out, "before-%s.txt" % c["name"]),
                   "tool:%s" % tool, "objdump:%s" % os.path.join(out, "after-%s.txt" % c["name"])]
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
    alerts = [l.split("] ", 1)[1].strip() for l in open(os.path.join(out, "client.log"))
              if l.startswith("[editor] ") and re.search(r"(Face Reduction|Colors Merged)$", l.strip())]
    failed = 0

    # 1. panels
    for name, doc in (("tools", documented_tools), ("prefab", lambda x: x)):
        try:
            got = open(os.path.join(out, "panel-%s.txt" % name)).read().splitlines()
        except OSError:
            got = None
        exp = open(os.path.join(ref, "right-%s.dump.txt" % name)).read().splitlines()
        if got is not None and doc(got) == doc(exp):
            print("panel %s: %s v7.1.4%s" % (name, "identical to" if got == exp else "matches",
                                             "" if got == exp else " with the documented differences"))
        else:
            failed += 1
            print("FAIL panel %s (diff %s %s)" % (name, os.path.join(ref, "right-%s.dump.txt" % name),
                                                  os.path.join(out, "panel-%s.txt" % name)))

    # 2. / 3. the tools
    if len(alerts) != len(cases):
        print("FAIL: %d tool alerts in the log for %d cases" % (len(alerts), len(cases)))
        failed += 1
    for i, c in enumerate(cases):
        name = c["name"]
        v7 = json.load(open(os.path.join(ref, "tools", name + ".json")))
        try:
            before = read_objdump(os.path.join(out, "before-%s.txt" % name))
            after = read_objdump(os.path.join(out, "after-%s.txt" % name))
        except (OSError, ValueError, IndexError) as e:
            failed += 1
            print("FAIL %s: no objdump (%s)" % (name, e))
            continue
        alert = alerts[i] if i < len(alerts) else None
        problems = []
        if not json_eq(before, v7["before"]):
            problems.append("imported objects differ from v7's\n  got:  %s\n  want: %s" % (json.dumps(before), json.dumps(v7["before"])))
        if c["tool"] == "mergeColors":
            m_alert, m_objs = merge_colors(v7["before"], c.get("range", 1500), True)
            if m_alert != v7["alert"] or not json_eq(m_objs, v7["after"]):
                problems.append("the v7-loop model does not reproduce v7 (%s vs %s)" % (m_alert, v7["alert"]))
            want_alert, want = merge_colors(v7["before"], c.get("range", 1500), False)
        else:
            want_alert, want = v7["alert"], v7["after"]
        if alert != want_alert:
            problems.append("alert %r, want %r" % (alert, want_alert))
        if not json_eq(after, want):
            problems.append("objects differ\n  got:  %s\n  want: %s" % (json.dumps(after), json.dumps(want)))
        if problems:
            failed += 1
            print("FAIL %s: %s" % (name, "; ".join(problems)))
        else:
            print("ok   %s: %s%s" % (name, alert, "" if alert == v7["alert"] else " (v7: %s)" % v7["alert"]))

    if not a.keep and not failed:
        shutil.rmtree(out, ignore_errors=True)
    elif not a.keep:
        print("dumps and client log kept in %s" % out)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
