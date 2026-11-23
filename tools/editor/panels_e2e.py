#!/usr/bin/env python3
"""Editor panels, settings and the T-key picker against Krunker v10.0.7's editor (round 12).

    python3 -I tools/editor/panels_e2e.py --bin <build>/client_bin [--root DIR] [-k NAME] [-v]

Each case is one headless `client_bin --offline --editor new` run (EGL pbuffer, no window on any
display, a private XDG_CONFIG_HOME) driven by KRUNKNATIVE_EDITOR_SCRIPT; "edstate:PATH" writes the
settings, the Recent Actions markup, the open prompt, the picker's visible items, each object's drawn
colour / emissive and the 3D view's target size (editor.c dump_state).

1. defaults: historyLimit is 100 (initSettings, editor-10.0.7.js:14628).
2. resets: Reset Grid / Barrier / Snapping / All ask first with v10's texts; Cancel keeps the values,
   OK resets them (snapping to 1, 1, 1) (editor-10.0.7.js:14853-14901).
3. history: Recent Actions names an object by formatConstName(prefab.name) ("Added Cube", :7134,
   :15505-15545); Disable History empties the undo list (toggleHistory, :15363-15372).
4. grid: Size stores Math.abs(v), Spacing Math.abs(v) || the default (index-10.0.7.js:13308-13328).
5. textures: the search matches either way round (the query holding a texture's name shows it) and by
   variant name (searchItems, editor-10.0.7.js:20922-20968, editor-config-10.0.7.js:4406); selectTex
   clears the asset texture (:20899-20906).
6. skins: the SHOWCASE picker lists no emotes or paints (:20982) and finds an item by its creator.
7. rewards: AI Rewards' Rarity Chances is stored as "rewc" when it holds digits, dots and commas only
   (index-10.0.7.js:9462-9472, editor-10.0.7.js:7357, :8467).
8. visuals: Object Border Visuals draws a bordered CUBE #ff0000 (:6943, :7033, :15958); Highlight gives
   the object under the mouse an emissive and drops it when off (:15848-15862, :17607-17621); the
   view draws at resMltV (0.6 by default) of its size (setResMlt, :20071-20074).
9. guide: the Publishing guide ends at step 4 (:13494).
10. tools: Frame's "Has Ceiling" is bound to frameThickness (index-10.0.7.js:12189): it shows on, off
   refuses Frame It, on frames with walls 1 thick and no ceiling (:19863-19907); Colorize > Gold
   changes nothing (:14824, :19106).

Exit status 0 = all passed; 77 when no client_bin is given (skip).
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile

CUBE = 0
SHOWCASE = 52
AI_SPAWNER = 55


def run_editor(c, script, name):
    cfg = os.path.join(c.tmp, name + "-cfg")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
    env.update({
        "KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": cfg, "KRUNKNATIVE_WINDOW_SIZE": "1920x937",
        "KRUNKNATIVE_EDITOR_SCRIPT": script + ";wait:0.3",
        "KRUNKNATIVE_SCREENSHOT": os.path.join(c.tmp, name + ".png"),  # the run ends after the script and this shot
    })
    if c.egl:
        env["KRUNKNATIVE_EGL_DEVICE"] = c.egl
    p = subprocess.run([c.bin, "--offline", "--editor", "new"], cwd=c.root, env=env,
                       capture_output=True, text=True, timeout=180)
    return p.stdout + p.stderr


def state(c, name):
    return os.path.join(c.tmp, name + ".json")


def load(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def objects(path):
    out = []
    for line in open(path, encoding="utf-8"):
        line = line.rstrip("\n")
        if not line or line.startswith("GROUP "):
            continue
        name, _, body = line.partition(" ")
        out.append((name, json.loads(body)))
    return out


class Failed(Exception):
    pass


def check(cond, msg):
    if not cond:
        raise Failed(msg)


def test_defaults(c):
    s = state(c, "defaults")
    run_editor(c, "wait:0.3;edstate:" + s, "defaults")
    st = load(s)["settings"]
    check(st.get("historyLimit") == 100, "historyLimit default %r, want 100" % st.get("historyLimit"))
    check(st.get("resMltV") == 0.6, "resMltV default %r" % st.get("resMltV"))


def test_resets(c):
    a, b, d, e, f, g = (state(c, "reset-%s" % k) for k in "abdefg")
    script = ";".join([
        "setting:gridSize:5", "setting:translationSnapping:7", "setting:dbOpacity:0.2",
        "ui:showWindow(3)", "ui:windows[2].switchTab(1)", "wait:0.2",
        "wpress:Reset#0", "edstate:" + a, "ui:KE.kn.promptCancel()", "edstate:" + b,
        "wpress:Reset#0", "ui:KE.kn.promptOk()", "edstate:" + d,
        "wpress:Reset#1", "edstate:" + e, "ui:KE.kn.promptOk()",
        "ui:windows[2].switchTab(2)", "wait:0.2", "wpress:Reset", "edstate:" + f, "ui:KE.kn.promptOk()", "edstate:" + g,
    ])
    log = run_editor(c, script, "resets")
    check("no control" not in log, "a Reset button is missing:\n" + log[-600:])
    check("reset Grid settings?" in (load(a)["prompt"] or ""), "Reset Grid asked %r" % load(a)["prompt"])
    check(load(a)["settings"]["gridSize"] == 5, "Reset Grid reset before the answer")
    check(load(b)["prompt"] is None and load(b)["settings"]["gridSize"] == 5, "Cancel still reset the grid")
    check(load(d)["settings"]["gridSize"] != 5, "OK did not reset the grid")
    check("reset Barrier settings?" in (load(e)["prompt"] or ""), "Reset Barrier asked %r" % load(e)["prompt"])
    check("reset Snapping settings?" in (load(f)["prompt"] or ""), "Reset Snapping asked %r" % load(f)["prompt"])
    check(load(g)["settings"]["translationSnapping"] == 1, "snapping not back to 1: %r" % load(g)["settings"]["translationSnapping"])
    # Controls > Speed's Reset (v10 asks about "All settings" there too)
    k = state(c, "reset-speed")
    run_editor(c, ";".join(["setting:speedNormal:5", "ui:showWindow(1)", "wait:0.2", "wpress:Reset", "edstate:" + k,
                            "ui:KE.kn.promptOk()", "edstate:" + k + ".2"]), "reset-speed2")
    check("reset All settings?" in (load(k)["prompt"] or ""), "Reset Speed asked %r" % load(k)["prompt"])
    check(load(k)["settings"]["speedNormal"] == 5, "Reset Speed reset before the answer")
    check(load(k + ".2")["settings"]["speedNormal"] == 70, "Reset Speed OK: speedNormal %r" % load(k + ".2")["settings"]["speedNormal"])


def test_history(c):
    a, b = state(c, "hist-a"), state(c, "hist-b")
    script = ";".join([
        "clear", "add:%d" % CUBE, "wait:1.3", "edstate:" + a,
        "ui:showWindow(3)", "ui:windows[2].switchTab(0)", "wait:0.2", "wset:Disable=1", "edstate:" + b,
    ])
    log = run_editor(c, script, "history")
    check("no control" not in log, "Disable History missing:\n" + log[-600:])
    hist = load(a)["history"]
    check("Added Cube" in hist, "Recent Actions: %r" % hist[:300])
    check("CUBE" not in hist, "Recent Actions shows the raw prefab name: %r" % hist[:300])
    check("Added" not in load(b)["history"], "Disable History kept the undo list: %r" % load(b)["history"][:300])


def test_grid(c):
    a = state(c, "grid")
    script = ";".join(["ui:showWindow(3)", "ui:windows[2].switchTab(1)", "wait:0.2", "wset:Size=-7", "wset:Spacing=0", "edstate:" + a])
    log = run_editor(c, script, "grid")
    check("no control" not in log, "grid rows missing:\n" + log[-600:])
    st = load(a)["settings"]
    check(st["gridSize"] == 7, "gridSize %r, want 7" % st["gridSize"])
    check(st["gridSpacing"] == 10, "gridSpacing %r, want the default 10" % st["gridSpacing"])


def test_textures(c):
    a, b, d = state(c, "tex-a"), state(c, "tex-b"), state(c, "tex-c")
    out = os.path.join(c.tmp, "tex-objs.txt")
    script = ";".join([
        "clear", "add:%d" % CUBE, "select:0", "set:Asset ID=12345", "select:0", "texlist", "wait:0.2",
        "search:my old brick wall", "edstate:" + a, "search:", "edstate:" + b,
        "ui:KE.selectTex(11,0)", "select:0", "objdump:" + out,
    ])
    run_editor(c, script, "textures")
    vis = load(a)["popup"]
    names = [v.split("|")[0].lower() for v in vis]
    # WALL shows as its alias "Stone" (formatConstName(alias || name))
    check("brick" in names and "stone" in names, "a query holding the names (BRICK, WALL) shows them: %r" % vis[:20])
    check("grass" not in names, "a texture the query does not name is shown: %r" % vis[:20])
    check(len(load(b)["popup"]) > len(vis), "clearing the search does not show every item")
    objs = objects(out)
    check(objs and objs[0][1].get("t") == "BRICK" or objs and objs[0][1].get("t") == 11, "selectTex did not set BRICK: %r" % (objs[:1],))
    check("at" not in objs[0][1], "selectTex kept the asset texture: %r" % objs[0][1].get("at"))


def test_skins(c):
    skins = load(os.path.join(c.root, "assets", "skins.json"))
    excluded = {k["name"] for k in skins if k.get("type") in (17, 18)}
    listed = {k["name"] for k in skins if k.get("type") not in (4, 5, 10, 11, 13, 16, 17, 18)}
    only_excluded = sorted(excluded - listed)
    target = None
    for k in skins:
        if k.get("type") in (4, 5, 10, 11, 13, 16, 17, 18) or not k.get("creators"):
            continue
        cr = k["creators"][0]
        if isinstance(cr, str) and len(cr) > 3 and cr.lower() not in k["name"].lower() and k["name"].lower() not in cr.lower():
            target = (k["name"], cr)
            break
    a, b = state(c, "skin-a"), state(c, "skin-b")
    script = ";".join(["clear", "add:%d" % SHOWCASE, "select:0", "texlist", "wait:0.3", "edstate:" + a,
                       "search:" + (target[1] if target else "zzz"), "edstate:" + b])
    run_editor(c, script, "skins")
    names = set(load(a)["popup"])
    check(len(names) > 100, "the skin list is nearly empty: %d" % len(names))
    leaked = [n for n in only_excluded if n in names]
    check(not leaked, "emotes / paints listed: %r" % leaked[:10])
    if target:
        check(target[0] in load(b)["popup"], "searching creator %r does not find %r" % (target[1], target[0]))


def test_rewards(c):
    a, b = os.path.join(c.tmp, "rew-a.txt"), os.path.join(c.tmp, "rew-b.txt")
    script = ";".join([
        "clear", "add:%d" % AI_SPAWNER, "select:0", "set:Rarity Chances=40,30,20,10", "select:0", "objdump:" + a,
        "set:Rarity Chances=4a", "select:0", "objdump:" + b,
    ])
    log = run_editor(c, script, "rewards")
    check("no control" not in log, "Rarity Chances missing:\n" + log[-600:])
    check(objects(a)[0][1].get("rewc") == "40,30,20,10", "rewc %r" % objects(a)[0][1].get("rewc"))
    check("rewc" not in objects(b)[0][1], "an invalid Rarity Chances was written: %r" % objects(b)[0][1].get("rewc"))


def test_visuals(c):
    a, b, d, e, f = (state(c, "vis-%s" % k) for k in "abdef")
    script = ";".join([
        "clear", "add:%d" % CUBE, "select:0", "set:Border=1", "wait:0.2", "edstate:" + a,
        "ui:showWindow(3)", "ui:windows[2].switchTab(2)", "wait:0.2", "wset:Object Border Visuals=0", "closewindow", "edstate:" + b,
        "setting:objectHighlight:true", "hover:obj:0", "edstate:" + d,
        "setting:objectHighlight:false", "hover:861:471", "edstate:" + e,
        "setting:resMltV:1", "edstate:" + f,
    ])
    log = run_editor(c, script, "visuals")
    check("no control" not in log, "a Settings row is missing:\n" + log[-600:])
    check(load(a)["render"][0]["c"].lower() == "#ff0000", "bordered cube not red: %r" % load(a)["render"])
    check(load(b)["render"][0]["c"].lower() != "#ff0000", "Object Border Visuals off still red: %r" % load(b)["render"])
    check(load(d)["highlight"] == 1 and load(d)["render"][0]["e"], "Highlight: no emissive on the cube under the mouse: %r" % load(d))
    check(load(e)["highlight"] == 0 and not load(e)["render"][0]["e"], "Highlight off kept the emissive: %r" % load(e)["render"])
    sa = load(a)
    check(sa["scaled"] and abs(sa["target_w"] - round(sa["view_w"] * 0.6)) <= 1,
          "resMltV 0.6: target %r for view %r" % (sa["target_w"], sa["view_w"]))
    check(not load(f)["scaled"], "resMltV 1 still draws through a target")


def test_guide(c):
    out = os.path.join(c.tmp, "guide.txt")
    run_editor(c, ";".join(["ui:showWindow(11)", "wait:0.2", "dump:window:" + out]), "guide")
    text = open(out, encoding="utf-8").read()
    check("Publishing Guide" in text and "4." in text, "the guide did not open: %r" % text[:200])
    check("50,000" not in text and "Partner" not in text, "the guide has v7's step 5")


def test_tools(c):
    # Tools > Frame: "Has Ceiling" is bound to frameThickness (index-10.0.7.js:12189), so it shows on at 10;
    # off makes the thickness false (refused), on makes it true (walls 1 thick); frameCeiling stays false, so
    # no ceiling. Colorize > Gold changes nothing (editor-10.0.7.js:14824, 19106).
    p, a, b, d = (os.path.join(c.tmp, "tools-%s.txt" % k) for k in "pabd")
    script = ";".join([
        "clear", "panel:tools", "dump:right:" + p,
        "add:%d" % CUBE, "select:0", "panel:tools", "set:Has Ceiling=0", "press:Frame It", "objdump:" + a,
        "panel:tools", "set:Has Ceiling=1", "press:Frame It", "objdump:" + b,
        "panel:tools", "press:Execute#7", "objdump:" + d,  # Gold: the 8th Execute (Optimize, Turn On, 3 tools, Random)
    ])
    log = run_editor(c, script, "tools")
    check("no control" not in log and "no button" not in log, "a Tools control is missing:\n" + log[-600:])
    check("L[] Has Ceiling : switch=1" in open(p, encoding="utf-8").read(), "Has Ceiling does not show the thickness")
    check(len(objects(a)) == 1 and "Wall Thickness must be 1 or greator" in log, "Has Ceiling off: %d objects" % len(objects(a)))
    walls = objects(b)
    check(len(walls) == 5 and walls[1][1]["s"][2] == 1, "Has Ceiling on: %r" % walls)
    check([o[1].get("c") for o in objects(d)] == [o[1].get("c") for o in walls], "Gold changed colours: %r" % objects(d))


TESTS = [test_defaults, test_resets, test_history, test_grid, test_textures, test_skins, test_rewards, test_visuals, test_guide,
         test_tools]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin")
    ap.add_argument("--root", default=os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
    ap.add_argument("-k")
    ap.add_argument("-v", action="store_true")
    ap.add_argument("--keep")
    c = ap.parse_args()
    if not c.bin or not os.path.exists(c.bin):
        print("panels_e2e: no client_bin, skipping")
        return 77
    c.bin = os.path.abspath(c.bin)
    c.egl = os.environ.get("KRUNKNATIVE_EGL_DEVICE", "")
    c.tmp = c.keep or tempfile.mkdtemp(prefix="kn-panels-e2e-")
    os.makedirs(c.tmp, exist_ok=True)
    failed = 0
    try:
        for t in TESTS:
            if c.k and c.k not in t.__name__:
                continue
            try:
                t(c)
                print("PASS", t.__name__)
            except (Failed, OSError, ValueError, KeyError, IndexError, TypeError) as e:
                failed += 1
                print("FAIL", t.__name__, e)
    finally:
        if not c.keep:
            shutil.rmtree(c.tmp, ignore_errors=True)
    print("%d failed" % failed)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
