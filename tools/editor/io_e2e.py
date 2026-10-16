#!/usr/bin/env python3
"""Map editor file-level round trips (File menu, Ctrl+S, backups, drag and drop; doc/v10/editor-social.md
and doc/v7/editor.md section 1.10): headless `client_bin --editor` runs driven by
KRUNKNATIVE_EDITOR_SCRIPT (file:..., key:..., export:..., dump:...).

    python3 -I tools/editor/io_e2e.py [--root DIR] [--bin client_bin] [-k NAME]

1. export -> import gives the same map: Export (file:export) writes the same text as export:, and
   importing that file and exporting again gives byte-identical text (objects, spawns, camPos,
   botPaths kept).
2. Save Project (file:save) is v10's save(): {map, groups, selected?, cam:{p, r:[pitch, yaw]}, history:[]};
   a save file's camera is read back in the same order (controls.setRotation(r[0], r[1])).
3. Backups: doBackup records, Load Backup (window 15) lists newest first as "date - name", loads the
   chosen entry, and Clear All empties the list.
4. Ctrl+S (key:ctrl+s) writes the project's file, the same map export (E10 save() is a download; the
   native editor saves the project's own file, see editor_project.c).
5. New Project (file:new) clears the map.
6. Drag and drop: several files are imported in turn, the last one shown.
7. File menu: the items before the KrunkNative Projects submenu are v10's File menu (IDX:14434-14458)
   without the dev-only Import Old Project.
8. Export GLTF / Export MTL: <name>.gltf, and <name>.zip with <name>.obj and <name>.mtl.
9. Every map under assets/maps imports, exports, re-imports and exports to the same text.

Every run is headless (EGL pbuffer, no DISPLAY) with private XDG_CONFIG_HOME / XDG_DATA_HOME under a
temp dir in ~/.cache. Exit status 0 = all passed.
"""
import argparse
import glob
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
MAP = os.path.join("assets", "maps", "littletown.json")
SECOND = os.path.join("assets", "maps", "shipyard.json")  # no user-asset references (see all_maps_round_trip)
V10_FILE_MENU = ["Import Project", "New Project", "Save Project", "Load Backup", "-",
                 "Export", "Copy Export", "-", "Export GLTF", "Export MTL"]


class Ctx:
    pass


def check(cond, msg):
    if not cond:
        raise AssertionError(msg)


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def jload(path):
    return json.loads(read(path))


def run_editor(c, name, args, script, cfg=None, data=None, timeout=180):
    """client_bin with KRUNKNATIVE_EDITOR_SCRIPT; cfg/data are shared between runs when given."""
    cfg = cfg or os.path.join(c.tmp, name + "-cfg")
    data = data or os.path.join(c.tmp, name + "-data")
    os.makedirs(cfg, exist_ok=True)
    os.makedirs(data, exist_ok=True)
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
    env.update({
        "KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": cfg, "XDG_DATA_HOME": data,
        "KRUNKNATIVE_WINDOW_SIZE": "1280x720",
        "KRUNKNATIVE_EDITOR_SCRIPT": "closewindow;" + script,
        # the screenshot ends the run once the script is done
        "KRUNKNATIVE_SCREENSHOT": os.path.join(c.tmp, name + ".png"),
    })
    if c.egl:
        env["KRUNKNATIVE_EGL_DEVICE"] = c.egl
    p = subprocess.run([c.bin, "--editor"] + args, cwd=c.root, env=env, capture_output=True, text=True, timeout=timeout)
    log = p.stdout + p.stderr
    check(os.path.exists(env["KRUNKNATIVE_SCREENSHOT"]), "%s: the run did not finish its script (rc %d):\n%s" % (name, p.returncode, log[-800:]))
    return log, data


def out(c, name):
    return os.path.join(c.tmp, name)


def map_copy(c, src, name):
    dst = out(c, name)
    shutil.copy(os.path.join(c.root, src), dst)
    return dst


TESTS = []


def test(fn):
    TESTS.append(fn)
    return fn


@test
def export_import_roundtrip(c):
    src = map_copy(c, MAP, "littletown.json")
    log, data = run_editor(c, "rt-a", [src], "file:export;export:%s" % out(c, "a_cmd.json"))
    exp = os.path.join(data, "krunknative", "maps", "Littletown_export.json")
    check(os.path.exists(exp), "Export wrote no Littletown_export.json (log: %s)" % log[-400:])
    check(read(exp) == read(out(c, "a_cmd.json")), "Export (file:export) and export: differ")
    check("copied to the clipboard" not in log, "a plain Export copied to the clipboard")
    a = jload(out(c, "a_cmd.json"))
    orig = jload(os.path.join(c.root, MAP))
    check(len(a["objects"]) == len(orig["objects"]), "export objects %d != source %d" % (len(a["objects"]), len(orig["objects"])))
    check(len(a.get("spawns", [])) == len(orig["spawns"]), "spawns not exported: %s" % a.get("spawns"))
    check("camPos" in a and "botPaths" in a, "camPos / botPaths missing from the export")
    # import the export file into a fresh editor, export again: the same text
    log, _ = run_editor(c, "rt-b", ["new"], "file:import:%s;export:%s" % (exp, out(c, "b.json")), cfg=os.path.join(c.tmp, "rt-a-cfg"), data=data)
    check(read(out(c, "b.json")) == read(out(c, "a_cmd.json")), "re-import + export differs from the first export")


@test
def copy_export_shift(c):
    src = map_copy(c, MAP, "copy.json")
    log, _ = run_editor(c, "copy", [src], "file:exportcopy;file:copy;export:%s" % out(c, "copy_cmd.json"))
    check("copied to the clipboard" in log, "Shift+Export did not copy (log: %s)" % log[-400:])
    check(read(out(c, "copy_cmd.json")) and jload(out(c, "copy_cmd.json"))["name"] == "Littletown", "copy run exported nothing")


@test
def save_format_and_camera_order(c):
    src = map_copy(c, MAP, "save-src.json")
    log, data = run_editor(c, "save", [src], "select:3;file:save")
    save = os.path.join(data, "krunknative", "maps", "Littletown_save.txt")
    check(os.path.exists(save), "Save Project wrote no _save.txt (log: %s)" % log[-400:])
    s = jload(save)
    check(list(s.keys()) == ["map", "groups", "selected", "cam", "history"], "save keys: %s" % list(s.keys()))
    check(isinstance(s["selected"], int), "selected is not a uuid")
    check(s["history"] == [], "history is not []")
    check(len(s["cam"]["p"]) == 3 and len(s["cam"]["r"]) == 2, "cam shape: %s" % s["cam"])
    check("objects" in s["map"] and len(s["map"]["objects"]) > 1000, "raw map missing objects")
    # a save with a known camera (r = [pitch, yaw], v10's controls.getRotation) comes back the same
    s["cam"] = {"p": [1.5, 2.25, -3], "r": [0.25, 1.5]}
    mod = out(c, "save-mod.txt")
    with open(mod, "w", encoding="utf-8") as f:
        json.dump(s, f)
    run_editor(c, "save2", ["new"], "file:import:%s;file:save" % mod, data=data)
    s2 = jload(save)
    check(s2["cam"] == {"p": [1.5, 2.25, -3], "r": [0.25, 1.5]}, "camera did not round trip: %s" % s2["cam"])
    check(s2["map"]["name"] == "Littletown", "map name after save import: %s" % s2["map"]["name"])


@test
def backups(c):
    src = map_copy(c, MAP, "bk.json")
    cfg = os.path.join(c.tmp, "bk-cfg")
    data = os.path.join(c.tmp, "bk-data")
    win = out(c, "win15.txt")
    log, _ = run_editor(c, "bk", [src], "file:backup;file:new;file:backup;window:15;wait:0.2;dump:window:%s;closewindow;"
                        "file:loadbackup:1;export:%s" % (win, out(c, "bk_lb.json")), cfg=cfg, data=data)
    check(log.count("Backup Successful") == 2, "two backups expected in the log")
    # dump lines: "<tag> <label>" for each entry (the tag is the page element's kind)
    rows = [l[2:] for l in read(win).splitlines() if re.match(r"^[A-Z] \d{1,2}/\d{1,2}/\d{4}, ", l)]
    check(len(rows) == 2, "Load Backup lists %d entries, want 2: %s" % (len(rows), rows))
    pat = re.compile(r"^\d{1,2}/\d{1,2}/\d{4}, \d{1,2}:\d{2}:\d{2} [AP]M - ")
    check(all(pat.match(r) for r in rows), "labels are not 'date - name': %s" % rows)
    check(not rows[0].endswith(" - Littletown"), "Load Backup is not newest first: %s" % rows)
    check(rows[1].endswith(" - Littletown"), "oldest entry is the Littletown backup: %s" % rows)
    lb = jload(out(c, "bk_lb.json"))
    check(lb["name"] == "Littletown" and len(lb["objects"]) > 1000, "Load Backup 1 did not load the Littletown backup")
    # the index and the records persist between runs; Clear All removes them
    log, _ = run_editor(c, "bk2", ["new"], "file:clearbackups;window:15;wait:0.2;dump:window:%s" % win, cfg=cfg, data=data)
    check("No Backups found" in read(win), "Clear All did not empty Load Backup")
    left = [f for f in os.listdir(cfg + "/krunknative") if f.startswith("knEditorBackup")] if os.path.isdir(cfg + "/krunknative") else []
    check(not left, "backup records left after Clear All: %s" % left)


@test
def ctrl_s_saves_project(c):
    src = map_copy(c, MAP, "ctrls.json")
    before = len(jload(src)["objects"])  # the export of the map as it was
    log, _ = run_editor(c, "ctrls", [src], "add:0;key:ctrl+s;wait:0.2")
    after = jload(src)
    check(len(after["objects"]) > 1000, "project file lost its objects")
    check(log.count("Saved ") >= 1, "Ctrl+S did not save (log: %s)" % log[-300:])
    check("spawns" in after and "camPos" in after, "Ctrl+S dropped spawns / camPos")
    check(len(after["objects"]) == before + 1, "Ctrl+S: %d objects, want %d" % (len(after["objects"]), before + 1))


@test
def new_project_clears(c):
    src = map_copy(c, MAP, "new.json")
    log, data = run_editor(c, "new", [src], "file:new;export:%s" % out(c, "new_cmd.json"))
    e = jload(out(c, "new_cmd.json"))
    check(len(e["objects"]) == 0, "New Project kept %d objects" % len(e["objects"]))
    check(len(e.get("spawns", [])) == 0, "New Project kept spawns")


@test
def drop_imports_in_turn(c):
    lt = map_copy(c, MAP, "drop-lt.json")
    sand = os.path.join(c.root, SECOND)
    bad = out(c, "drop-bad.txt")
    with open(bad, "w", encoding="utf-8") as f:
        f.write("not a map")
    log, _ = run_editor(c, "drop", ["new"], "file:drop:%s|%s|%s;export:%s" % (lt, bad, sand, out(c, "drop_cmd.json")))
    e = jload(out(c, "drop_cmd.json"))
    want = jload(sand)["name"]
    check(e["name"] == want, "drop: the last file did not win (%s != %s)" % (e["name"], want))
    check("not valid JSON" in log, "a dropped non-map was not reported")


@test
def file_menu_items(c):
    src = map_copy(c, MAP, "menu.json")
    win = out(c, "toolbar.txt")
    run_editor(c, "menu", [src], "dump:toolbar:%s" % win)
    lines = read(win).splitlines()
    i = lines.index("M File")
    items = []
    for l in lines[i + 1:]:
        if not l.startswith("  "):
            break
        if l.startswith("  M "):
            break
        items.append(l.strip()[2:] if l.strip() != "-" else "-")
    check(items[:len(V10_FILE_MENU)] == V10_FILE_MENU, "File menu %s != v10 %s" % (items, V10_FILE_MENU))


@test
def gltf_and_mtl(c):
    src = map_copy(c, MAP, "gltf.json")
    log, data = run_editor(c, "gltf", [src], "file:gltf;file:mtl")
    d = os.path.join(data, "krunknative", "maps")
    check(os.path.exists(os.path.join(d, "Littletown.gltf")), "no Littletown.gltf (log: %s)" % log[-300:])
    g = jload(os.path.join(d, "Littletown.gltf"))
    check(g.get("asset", {}).get("version") == "2.0", "gltf asset version")
    zp = os.path.join(d, "Littletown.zip")
    check(os.path.exists(zp), "no Littletown.zip")
    names = zipfile.ZipFile(zp).namelist()
    check("Littletown.obj" in names and "Littletown.mtl" in names, "zip members: %s" % names)


@test
def all_maps_round_trip(c):
    # the maps that place Krunker user assets (an "aid") fetch them over the network on load, which
    # takes minutes offline; their model-level round trips are in editor_serialize_test
    maps = []
    for m in sorted(glob.glob(os.path.join(c.root, "assets", "maps", "*.json"))):
        if '"aid"' not in read(m):
            maps.append(m)
    check(len(maps) >= 30, "only %d maps without user assets" % len(maps))
    script = []
    names = []
    for i, m in enumerate(maps):
        a = out(c, "m%02d.json" % i)
        b = out(c, "m%02db.json" % i)
        script.append("file:import:%s;export:%s;file:import:%s;export:%s" % (m, a, a, b))
        names.append((m, a, b))
    run_editor(c, "maps", ["new"], ";".join(script), timeout=900)
    bad = []
    for m, a, b in names:
        if not os.path.exists(a) or not os.path.exists(b):
            bad.append("%s: no export" % os.path.basename(m))
            continue
        if read(a) != read(b):
            bad.append("%s: second export differs" % os.path.basename(m))
        src = jload(m)
        if src.get("name") is not None and jload(a).get("name") != src.get("name"):
            bad.append("%s: name lost" % os.path.basename(m))
    check(not bad, "map round trips: " + "; ".join(bad[:10]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=ROOT)
    ap.add_argument("--bin", default=os.path.join(ROOT, "cmake-build-debug", "client_bin"))
    ap.add_argument("-k", default="", help="run only tests whose name contains this")
    args = ap.parse_args()
    c = Ctx()
    c.root = os.path.abspath(args.root)
    c.bin = os.path.abspath(args.bin)
    c.egl = os.environ.get("KRUNKNATIVE_EGL_DEVICE", "")
    base = os.path.expanduser("~/.cache")
    os.makedirs(base, exist_ok=True)
    c.tmp = tempfile.mkdtemp(prefix="io-e2e-", dir=base)
    failed = ran = 0
    try:
        for fn in TESTS:
            if args.k and args.k not in fn.__name__:
                continue
            ran += 1
            try:
                fn(c)
                print("ok   %s" % fn.__name__)
            except Exception as e:  # report every test
                failed += 1
                print("FAIL %s: %s" % (fn.__name__, e))
    finally:
        shutil.rmtree(c.tmp, ignore_errors=True)
    print("%d/%d passed" % (ran - failed, ran))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
