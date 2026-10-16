#!/usr/bin/env python3
"""Editor objects end-to-end (doc/v10/editor-social.md 1.3, 1.5): object create, the inspector's
edit round trip, BOT_PATH_NODE numbering, asset map slots, group copy / paste and Quick Add search.

    python3 -I tools/editor/objects_e2e.py --bin <build>/client_bin [--root DIR] [-k NAME] [-v]

Each case is one headless `client_bin --offline --editor new` run (EGL pbuffer, no window on any
display, a private XDG_CONFIG_HOME in a temp dir) driven by KRUNKNATIVE_EDITOR_SCRIPT:

1. bot_path_nodes (and bot_chains: the chain lines of checkConnections, one per node -> the node
   of its group numbered by its Next NodeId, none with Object Connections off): add:41 twice; GroupId / NodeId edits; Fix Node numbers the
   selected node after the highest node of its group (bn 6, bnn 7); Create Next Node adds bn 7
   (bnn 8, bnr from the owner) at the owner's position; Create Next Group starts group 1 with
   node 0, reversible (E10:17833-17885, IDX:15704-15785). Reversible shows only on node 0.
2. map_slots: a TRIGGER with assetID "lnb" (v10 accepts m r p b h v too, E10:7130) shows the
   Lightmap / Normal / Bump folders (IDX:16449-16501) and no Emissive folder; Channel and Flip Y
   of Normal are written as mp {"n": {"ch", "fy"}}; defaults drop the entries again.
3. group_paste: two cubes at one spot, copyobjs copies the one that intersects the selection,
   Object > Group > Paste (group:paste) replaces the selection (v10 createGroup removes it) with
   the copy under a new placeholder owner: two cubes, one placeholder, one group of one member.
4. quick_add: window 4 search "spawn" lists the matches by raw name length; Enter adds the
   first (AI_SPAWNER, the shorter name) and closes the window (windows[3].clickButton(0)).

Exit status 0 = all passed; 77 when no client_bin is given (skip).
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile

BOT_PATH_NODE = 41
CUBE = 0
TRIGGER = 29
AI_SPAWNER = 55


def run_editor(c, script, name):
    """One headless editor run; returns the run's log text."""
    cfg = os.path.join(c.tmp, name + "-cfg")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
    env.update({
        "KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": cfg, "KRUNKNATIVE_WINDOW_SIZE": "1920x937",
        "KRUNKNATIVE_EDITOR_SCRIPT": script + ";wait:0.3",
        "KRUNKNATIVE_SCREENSHOT": os.path.join(c.tmp, name + ".png"),
    })
    if c.egl:
        env["KRUNKNATIVE_EGL_DEVICE"] = c.egl
    p = subprocess.run([c.bin, "--offline", "--editor", "new"], cwd=c.root, env=env,
                       capture_output=True, text=True, timeout=180)
    return p.stdout + p.stderr


def objects(path):
    """objdump lines: [(prefab name, serialized object dict)] and the GROUP lines."""
    objs, groups = [], []
    for line in open(path, encoding="utf-8"):
        line = line.rstrip("\n")
        if line.startswith("GROUP "):
            groups.append(line)
            continue
        name, _, body = line.partition(" ")
        objs.append((name, json.loads(body)))
    return objs, groups


def check(cond, msg):
    if not cond:
        raise AssertionError(msg)


def case_bot_path_nodes(c):
    out = os.path.join(c.tmp, "bot-objs.txt")
    script = ";".join([
        "clear", "add:%d" % BOT_PATH_NODE, "select:0", "set:GroupId=0", "set:NodeId=5", "select:0",
        "add:%d" % BOT_PATH_NODE, "select:1", "press:Fix Node", "select:1",
        "press:Create Next Node", "select:1", "press:Create Next Group", "select:3",
        "dump:right:%s" % os.path.join(c.tmp, "bot-right.txt"), "objdump:" + out,
    ])
    log = run_editor(c, script, "bot")
    check("press: no button" not in log, "a BOT_PATH_NODE button is missing:\n" + log[-800:])
    objs, _ = objects(out)
    check([n for n, _ in objs] == ["BOT_PATH_NODE"] * 4, "expected four path nodes, got %r" % [n for n, _ in objs])
    got = [(d.get("bng", 0), d.get("bn", 0), d.get("bnn", 1), d.get("bnr", 0)) for _, d in objs]
    # A: the node 5 set by hand; B: Fix Node -> 6 (next 7); C: Create Next Node -> 7 (next 8);
    # D: Create Next Group -> group 1, node 0, reversible
    want = [(0, 5, 1, 0), (0, 6, 7, 0), (0, 7, 8, 0), (1, 0, 1, 1)]
    check(got == want, "node numbering (bng, bn, bnn, bnr) %r, want %r" % (got, want))
    check(objs[2][1].get("p") == objs[1][1].get("p"), "Create Next Node is not at the owner's position")
    # the Reversible switch is only shown on node 0 (IDX:15788-15797): the panel of D (node 0)
    right = open(os.path.join(c.tmp, "bot-right.txt"), encoding="utf-8").read()
    check("L[] Reversible : switch=" in right, "Reversible is missing on node 0")
    for label in ("Fix Node", "Create Next Node", "Create Next Group"):
        check("B %s" % label in right, "button %r missing from the BOT_PATH_NODE panel" % label)


def case_bot_chains(c):
    # checkConnections (E10:15577-15584): nodes of one group chain bnn -> bn; another group does not
    chains = os.path.join(c.tmp, "chains.txt")
    off = os.path.join(c.tmp, "chains-off.txt")
    script = ";".join([
        "clear", "add:%d" % BOT_PATH_NODE, "select:0", "add:%d" % BOT_PATH_NODE, "select:1", "set:NodeId=1",
        "add:%d" % BOT_PATH_NODE, "select:2", "set:GroupId=1", "wait:0.5", "chains:" + chains,
        "setting:triggerCons:false", "wait:0.5", "chains:" + off,
    ])
    run_editor(c, script, "chains")
    check(open(chains, encoding="utf-8").read().strip() == "CHAINS 1", "one chain (node 0 -> node 1) expected: %r" %
          open(chains, encoding="utf-8").read())
    check(open(off, encoding="utf-8").read().strip() == "CHAINS 0", "Object Connections off draws no chain")


def case_map_slots(c):
    right = os.path.join(c.tmp, "slot-right.txt")
    out = os.path.join(c.tmp, "slot-objs.txt")
    script = ";".join([
        "clear", "add:%d" % TRIGGER, "select:0", "set:Asset ID#1=lnb", "select:0",
        "dump:right:%s" % right,
        "set:Channel#1=2", "set:Flip Y#1=1", "select:0", "objdump:" + out,
        "set:Channel#1=0", "set:Flip Y#1=0", "select:0", "objdump:" + os.path.join(c.tmp, "slot-reset.txt"),
    ])
    run_editor(c, script, "slots")
    text = open(right, encoding="utf-8").read()
    for folder in ("Fs Lightmap", "Fs Normal", "Fs Bump"):
        check(folder in text, "map-slot folder %r missing for assetID lnb" % folder)
    check("Fs Emissive" not in text and "Fs Roughness" not in text, "a map-slot folder shows for a slot the asset does not have")
    objs, _ = objects(out)
    check(objs and objs[0][1].get("aid") == "lnb", "assetID lnb not kept: %r" % (objs[:1],))
    check(objs[0][1].get("mp") == {"n": {"ch": 2, "fy": 1}}, "mp after the Normal edit: %r" % objs[0][1].get("mp"))
    reset, _ = objects(os.path.join(c.tmp, "slot-reset.txt"))
    check("mp" not in reset[0][1], "defaults did not drop mp: %r" % reset[0][1].get("mp"))


def case_group_paste(c):
    out = os.path.join(c.tmp, "paste-objs.txt")
    run_editor(c, ";".join([
        "clear", "add:%d" % CUBE, "add:%d" % CUBE, "select:0", "copyobjs:0:0:0", "group:paste",
        "objdump:" + out,
    ]), "paste")
    objs, groups = objects(out)
    # v10 replaceObject(selected, copy, true, false, true): createGroup removes the selected cube
    # (removeObject(owner), E10:18671), the copy joins a group under a new placeholder
    names = sorted(n for n, _ in objs)
    check(names == ["CUBE", "CUBE", "PLACEHOLDER"], "paste left %r" % names)
    check(len(groups) == 1 and groups[0].endswith(" members=1"), "paste group lines %r" % groups)


def case_quick_add(c):
    right = os.path.join(c.tmp, "quick.txt")
    out = os.path.join(c.tmp, "quick-objs.txt")
    run_editor(c, ";".join([
        "clear", "window:4", "quick:spawn", "dump:window:" + right, "quickenter", "objdump:" + out,
    ]), "quick")
    labels = [l[2:] for l in open(right, encoding="utf-8").read().splitlines() if l.startswith("X ")]
    # the result cells: "Click to add" and the name; the matches are Ai Spawner then Spawn Point
    names = [l for l in labels if l not in ("Quick Add", "Customize", "Click to add", "delete_forever")]
    check(names[:2] == ["Ai Spawner", "Spawn Point"], "quick add results %r" % names)
    objs, _ = objects(out)
    check(objs and objs[0][0] == "AI_SPAWNER", "Enter did not add the first match: %r" % [n for n, _ in objs])


CASES = [
    ("bot_path_nodes", case_bot_path_nodes),
    ("bot_chains", case_bot_chains),
    ("map_slots", case_map_slots),
    ("group_paste", case_group_paste),
    ("quick_add", case_quick_add),
]


class Ctx:
    pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True, help="client_bin")
    ap.add_argument("--root", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
    ap.add_argument("-k", "--case", action="append", help="run only these cases")
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args()
    c = Ctx()
    c.bin = os.path.abspath(a.bin)
    c.root = os.path.abspath(a.root)
    c.egl = os.environ.get("KRUNKNATIVE_EGL_DEVICE", "")
    c.tmp = tempfile.mkdtemp(prefix="kn-objects-e2e-", dir=os.path.expanduser("~/.cache"))
    failed = 0
    for name, fn in CASES:
        if a.case and name not in a.case:
            continue
        try:
            fn(c)
            print("ok   %s" % name)
        except AssertionError as e:
            failed += 1
            print("FAIL %s: %s" % (name, e))
        except (OSError, subprocess.SubprocessError, ValueError) as e:
            failed += 1
            print("FAIL %s: %s" % (name, e))
    if not failed:
        shutil.rmtree(c.tmp, ignore_errors=True)
    else:
        print("runs kept in %s" % c.tmp)
    print("objects e2e: %d/%d passed" % (len(CASES) - failed if not a.case else sum(1 for n, _ in CASES if n in a.case) - failed,
                                          len(CASES) if not a.case else sum(1 for n, _ in CASES if n in a.case)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
