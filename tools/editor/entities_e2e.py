#!/usr/bin/env python3
"""Editor entities and archetypes end-to-end (Krunker v10.0.7, editor-10.0.7.js:18213-18851,
index-10.0.7.js:6337-6343 and 7006-7020; client/src/editor/editor_entities.c).

    python3 -I tools/editor/entities_e2e.py --bin <build>/client_bin [--root DIR] [-k NAME] [-v]

Each case is one headless `client_bin --offline --editor new` run (EGL pbuffer, a private
XDG_CONFIG_HOME in a temp dir) driven by KRUNKNATIVE_EDITOR_SCRIPT. A two-cube group comes from
Object > Group > Prefab > Import > JSON with assetAutoGroup on; "entity:menu:<label>" clicks a
toolbar option, "entity:prompt:<text>" answers the in-page prompt, "entity:val:/check:" fill the
popup, "entity:popup:<file>" dumps it and "entity:state:<file>" the stage / part-edit state.

1. save_archetype: Save as Archetype without a group says noGroup; with the group it prompts, keeps
   the name's [\\w \\-] characters, centres the parts and opens the archetype popup; the map export
   carries {id 1, name, objects, coll 1, hp 0, data {}}; undo / redo of the "archetypes" kind.
2. place_entity: Entity > Place lists the archetypes; Place adds an ENTITY (eid 1, ar 1) and opens
   the entity popup; Save writes the tag ([^\\w-] removed), autoSpawn off (as 0) and spawn params;
   invalid JSON is refused; the ENTITY panel has Edit Entity / Edit Archetype; a second one gets eid 2.
3. archetype_popup: name / collision / health (at least 0) / part names (\\w{1,12}) / soft zones /
   data JSON saved; Pick point adds pt1 rounded to 0.1; Delete is refused while an entity uses it.
4. browser_stage: the browser rows (parts / placed counts), Duplicate (" copy"), Delete; the stage
   (plate at y 5000, camera at (55, 5055, 115)) for a new archetype: the bar, its parts out of the
   export, Save names it; Edit Parts of an existing one and Cancel leave it unchanged.
5. part_edit: Edit Parts swaps the entity for its parts as a group; Apply Parts writes them back (the
   entity returns with its eid); Cancel Edit restores the entity; Unpack Parts leaves the parts
   (pn / sfz records).
6. prefab_entities: importing a prefab with an entity whose archetype is missing drops it (with the
   message); exporting a group holding an entity leaves it out (prefabStripped message).

Exit status 0 = all passed.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile

CUBE = 0
ENTITY = 98
GROUP_JSON = '[{"p":[0,0,0],"s":[4,4,4],"c":"#ff0000"},{"p":[0,4,0],"s":[2,2,2]}]'


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
                       capture_output=True, text=True, timeout=240)
    if c.verbose:
        print(p.stdout + p.stderr)
    return p.stdout + p.stderr


def objects(path):
    objs, groups = [], []
    for line in open(path, encoding="utf-8"):
        line = line.rstrip("\n")
        if line.startswith("GROUP "):
            groups.append(line)
            continue
        name, _, body = line.partition(" ")
        objs.append((name, json.loads(body)))
    return objs, groups


def popup(path):
    """{"shown": bool, "cls": str, "text": str, "fields": {key: (checked, value)}}"""
    out = {"shown": False, "cls": "", "text": "", "prompt": "", "fields": {}}
    for line in open(path, encoding="utf-8"):
        line = line.rstrip("\n")
        if line.startswith("SHOWN "):
            parts = line.split(" ")
            out["shown"] = parts[1] == "1"
            out["cls"] = parts[2] if len(parts) > 2 else ""
        elif line.startswith("TEXT "):
            out["text"] = line[5:]
        elif line.startswith("PROMPT "):
            out["prompt"] = line[7:]
        elif line.startswith("F "):
            key, checked, value = (line[2:].split(" ", 2) + ["", ""])[:3]
            out["fields"][key] = (checked == "1", value)
    return out


def state(path):
    return dict(kv.split("=") for kv in open(path, encoding="utf-8").read().split())


def export(path):
    return json.load(open(path, encoding="utf-8"))


def check(cond, msg):
    if not cond:
        raise AssertionError(msg)


def group_setup():
    """Two cubes grouped under a placeholder (objects 0, 1; the placeholder 2), the placeholder selected."""
    return ["clear", "add:%d" % CUBE, "select:0", "setting:assetAutoGroup:true", "entity:menu:JSON",
            "entity:prompt:" + GROUP_JSON, "select:2"]


def archetype_setup(name="Crate"):
    return group_setup() + ["entity:menu:Save as Archetype", "entity:prompt:" + name]


def f(c, name):
    return os.path.join(c.tmp, name)


def case_save_archetype(c):
    s = ["clear", "add:%d" % CUBE, "select:0", "entity:menu:Save as Archetype", "entity:popup:" + f(c, "sa-nogroup.txt")]
    s += group_setup() + ["objdump:" + f(c, "sa-objs.txt"), "entity:menu:Save as Archetype", "entity:popup:" + f(c, "sa-prompt.txt"),
                          "entity:prompt:Crate <Box>!", "entity:popup:" + f(c, "sa-pop.txt"), "export:" + f(c, "sa-e1.json"),
                          "entity:menu:Undo (Ctrl + Z)", "export:" + f(c, "sa-e2.json"), "entity:menu:Redo (Ctrl + Y)", "export:" + f(c, "sa-e3.json")]
    log = run_editor(c, ";".join(s), "save")
    check("no menu option" not in log, "a menu option is missing:\n" + log[-600:])
    p = popup(f(c, "sa-nogroup.txt"))
    check(p["cls"] == "responsePop" and "Select a group to save as an archetype." in p["text"], "noGroup popup: %r" % p)
    objs, groups = objects(f(c, "sa-objs.txt"))
    check([n for n, _ in objs] == ["CUBE", "CUBE", "PLACEHOLDER"] and len(groups) == 1, "group setup: %r %r" % ([n for n, _ in objs], groups))
    p = popup(f(c, "sa-prompt.txt"))
    check(p["prompt"] == "Name your Archetype", "name prompt: %r" % p["prompt"])
    p = popup(f(c, "sa-pop.txt"))
    check(p["cls"] == "entityPop" and p["fields"].get("archetypeName", (0, ""))[1] == "Crate Box" and p["fields"]["archetypeId"][1] == "1",
          "archetype popup after saving: %r" % p)
    check("archetypePartName#0" in p["fields"] and "archetypePartSoft#1" in p["fields"], "part rows: %r" % p["fields"])
    check("Edit Parts" in p["text"] and "Unpack Parts" in p["text"] and "Delete" in p["text"], "archetype popup buttons: %r" % p["text"])
    ar = export(f(c, "sa-e1.json")).get("archetypes")
    check(ar and len(ar) == 1, "export archetypes %r" % ar)
    a = ar[0]
    check(a["id"] == 1 and a["name"] == "Crate Box" and a["coll"] == 1 and a["hp"] == 0 and a["data"] == {} and len(a["objects"]) == 2,
          "archetype record %r" % a)
    ps = sorted(o["p"] for o in a["objects"])
    check(ps == [[0, 0, 0], [0, 4, 0]], "parts centred on (x, minY, z): %r" % ps)
    check("archetypes" not in export(f(c, "sa-e2.json")), "undo did not remove the archetype")
    check(len(export(f(c, "sa-e3.json")).get("archetypes", [])) == 1, "redo did not restore the archetype")


def case_place_entity(c):
    s = ["clear", "entity:menu:Place", "entity:popup:" + f(c, "pl-none.txt")]
    s += archetype_setup() + ["entity:menu:Place", "entity:popup:" + f(c, "pl-place.txt"), "ui:KE.createEntity()",
                              "entity:popup:" + f(c, "pl-ent.txt"), "objdump:" + f(c, "pl-o1.txt"),
                              "entity:val:entityTag=boss 1!", "entity:check:entityAuto=0", 'entity:val:entitySp={"hp":5}',
                              "ui:KE.saveEntity()", "entity:popup:" + f(c, "pl-saved.txt"), "objdump:" + f(c, "pl-o2.txt"),
                              "entity:menu:Edit Selected", "entity:val:entitySp=[1]", "ui:KE.saveEntity()",
                              "entity:popup:" + f(c, "pl-bad.txt"), "objdump:" + f(c, "pl-o3.txt"),
                              "select:3", "dump:right:" + f(c, "pl-right.txt"), "press:Edit Entity", "entity:popup:" + f(c, "pl-press.txt"),
                              "press:Edit Archetype", "entity:popup:" + f(c, "pl-press2.txt"),
                              "add:%d" % ENTITY, "ui:KE.createEntity()", "objdump:" + f(c, "pl-o4.txt"), "export:" + f(c, "pl-e.json")]
    log = run_editor(c, ";".join(s), "place")
    check("no menu option" not in log and "no element" not in log, "missing menu option / element:\n" + log[-600:])
    p = popup(f(c, "pl-none.txt"))
    check("No archetypes yet. Group some objects and use Save as Archetype." in p["text"], "noArchetypes: %r" % p["text"])
    p = popup(f(c, "pl-place.txt"))
    check(p["text"].startswith("Place Entity") and p["fields"].get("entityArPick", (0, ""))[1] == "1", "place popup %r" % p)
    p = popup(f(c, "pl-ent.txt"))
    for label in ("Entity", "Archetype", "Entity ID", "Tag", "Spawn on map load", "Spawn Params (JSON)", "Pick point", "Save", "Edit Archetype"):
        check(label in p["text"], "entity popup lacks %r: %r" % (label, p["text"]))
    check(p["fields"]["entityEid"][1] == "1" and p["fields"]["entityAuto"][0] and p["fields"]["entitySp"][1] == "{}", "entity popup fields %r" % p["fields"])
    objs, _ = objects(f(c, "pl-o1.txt"))
    ents = [d for n, d in objs if n == "ENTITY"]
    check(len(ents) == 1 and ents[0].get("eid") == 1 and ents[0].get("ar") == 1, "placed entity %r" % ents)
    check(not popup(f(c, "pl-saved.txt"))["shown"], "Save did not close the popup")
    ent = [d for n, d in objects(f(c, "pl-o2.txt"))[0] if n == "ENTITY"][0]
    check(ent.get("tag") == "boss1" and ent.get("as") == 0 and ent.get("sp") == {"hp": 5}, "saved entity %r" % ent)
    p = popup(f(c, "pl-bad.txt"))
    check("That JSON is not a valid object." in p["text"], "bad JSON popup %r" % p["text"])
    ent = [d for n, d in objects(f(c, "pl-o3.txt"))[0] if n == "ENTITY"][0]
    check(ent.get("sp") == {"hp": 5}, "bad JSON changed the entity %r" % ent)
    right = open(f(c, "pl-right.txt"), encoding="utf-8").read()
    check("B Edit Entity" in right and "B Edit Archetype" in right, "ENTITY panel buttons missing:\n" + right[:600])
    check(popup(f(c, "pl-press.txt"))["fields"].get("entityTag", (0, ""))[1] == "boss1", "Edit Entity did not open the entity popup")
    check(popup(f(c, "pl-press2.txt"))["fields"].get("archetypeName", (0, ""))[1] == "Crate", "Edit Archetype did not open the archetype popup")
    ents = [d for n, d in objects(f(c, "pl-o4.txt"))[0] if n == "ENTITY"]
    check(sorted(e.get("eid") for e in ents) == [1, 2], "second entity id %r" % ents)
    e = export(f(c, "pl-e.json"))
    check(len([o for o in e["objects"] if o.get("i") == ENTITY]) == 2, "export lacks the entities")


def case_archetype_popup(c):
    s = archetype_setup() + ["ui:KE.createEntity(1)", "ui:KE.archetypePopup()", "entity:val:archetypeName=Big Crate#",
                             "entity:val:archetypeColl=2", "entity:val:archetypeHp=-5", "entity:val:.archetypePartName#0=lid",
                             "entity:val:.archetypePartName#1=bad name", "entity:check:.archetypePartSoft#1=1",
                             'entity:val:archetypeData={"k":1}', "ui:KE.saveArchetypeData()", "export:" + f(c, "ap-e1.json"),
                             "ui:KE.archetypePopup(1)", "entity:popup:" + f(c, "ap-pop.txt"),
                             'ui:KE.pickPoint("data")', "entity:state:" + f(c, "ap-s1.txt"), "entity:pickat:1.26,2.04,-3.35",
                             "entity:popup:" + f(c, "ap-pick.txt"), "entity:state:" + f(c, "ap-s2.txt"),
                             "ui:KE.removeArchetype()", "entity:popup:" + f(c, "ap-inuse.txt"), "export:" + f(c, "ap-e2.json")]
    run_editor(c, ";".join(s), "arpop")
    a = export(f(c, "ap-e1.json"))["archetypes"][0]
    check(a["name"] == "Big Crate" and a["coll"] == 2 and a["hp"] == 0 and a["data"] == {"k": 1}, "saved archetype %r" % a)
    names = [o.get("n") for o in a["objects"]]
    softs = [o.get("soft") for o in a["objects"]]
    check(sorted(map(str, names)) == ["None", "lid"] and softs.count(1) == 1, "part names / soft %r %r" % (names, softs))
    p = popup(f(c, "ap-pop.txt"))
    check(p["fields"]["archetypeColl"][1] == "2" and p["fields"]["archetypeHp"][1] == "0", "reopened popup %r" % p["fields"])
    check(state(f(c, "ap-s1.txt"))["pick"] == "1", "pickPoint did not wait for a click")
    check(state(f(c, "ap-s2.txt"))["pick"] == "0", "the picked point did not end pickPoint")
    data = json.loads(popup(f(c, "ap-pick.txt"))["fields"]["archetypeData"][1])
    check(data == {"k": 1, "pt1": [1.3, 2, -3.4]} or data == {"k": 1, "pt1": [1.3, 2, -3.3]}, "picked data %r" % data)
    check("1 entities still use this archetype." in popup(f(c, "ap-inuse.txt"))["text"], "Delete of a used archetype not refused")
    check(len(export(f(c, "ap-e2.json"))["archetypes"]) == 1, "the used archetype was deleted")


def case_browser_stage(c):
    s = archetype_setup() + ["ui:KE.createEntity(1)", "entity:menu:Archetypes", "entity:popup:" + f(c, "bs-browser.txt"),
                             "ui:KE.duplicateArchetype(1)", "entity:popup:" + f(c, "bs-dup.txt"), "export:" + f(c, "bs-e1.json"),
                             "ui:KE.removeArchetypeFromBrowser(2)", "export:" + f(c, "bs-e2.json"),
                             "objdump:" + f(c, "bs-o0.txt"), "setting:createObjectNear:true",
                             "ui:KE.enterArchetypeStage()", "entity:state:" + f(c, "bs-s1.txt"), "objdump:" + f(c, "bs-o1.txt"),
                             "add:%d" % CUBE, "export:" + f(c, "bs-e3.json"), "ui:KE.saveArchetypeStage()", "entity:prompt:Stage One",
                             "entity:state:" + f(c, "bs-s2.txt"), "entity:popup:" + f(c, "bs-saved.txt"), "export:" + f(c, "bs-e4.json"),
                             "objdump:" + f(c, "bs-o2.txt"),
                             "ui:KE.enterArchetypeStage(1)", "objdump:" + f(c, "bs-o3.txt"), "entity:menu:Archetypes",
                             "entity:popup:" + f(c, "bs-active.txt"), "ui:KE.cancelArchetypeStage()", "entity:state:" + f(c, "bs-s3.txt"),
                             "entity:popup:" + f(c, "bs-cancel.txt"), "export:" + f(c, "bs-e5.json"), "objdump:" + f(c, "bs-o4.txt")]
    log = run_editor(c, ";".join(s), "stage")
    p = popup(f(c, "bs-browser.txt"))
    check(p["text"].startswith("Archetypes") and "Crate" in p["text"] and "#1 · 2 parts · 1 placed" in p["text"], "browser %r" % p["text"])
    for label in ("Edit Parts", "Properties", "Duplicate", "Delete", "New Archetype"):
        check(label in p["text"], "browser lacks %r" % label)
    names = [a["name"] for a in export(f(c, "bs-e1.json"))["archetypes"]]
    check(names == ["Crate", "Crate copy"], "duplicate %r" % names)
    check([a["name"] for a in export(f(c, "bs-e2.json"))["archetypes"]] == ["Crate"], "browser delete")
    before = len(objects(f(c, "bs-o0.txt"))[0])
    st = state(f(c, "bs-s1.txt"))
    check(st["archEdit"] == "1" and st["bar"] == "1" and st["arId"] == "0", "stage state %r" % st)
    plate = [d for _, d in objects(f(c, "bs-o1.txt"))[0] if d.get("s") == [280, 1, 280]]
    check(plate and plate[0]["p"] == [0, 4999, 0] and plate[0].get("c") == "#23202a", "stage plate %r" % plate)
    e2, e3 = export(f(c, "bs-e2.json")), export(f(c, "bs-e3.json"))
    check(len(e3["objects"]) == len(e2["objects"]), "the stage objects are in the export (%d vs %d)" % (len(e3["objects"]), len(e2["objects"])))
    st = state(f(c, "bs-s2.txt"))
    check(st["archEdit"] == "0" and st["bar"] == "0", "stage not left after Save %r" % st)
    check(popup(f(c, "bs-saved.txt"))["fields"].get("archetypeName", (0, ""))[1] == "Stage One", "new archetype popup after the stage")
    ar = export(f(c, "bs-e4.json"))["archetypes"]
    check([a["name"] for a in ar] == ["Crate", "Stage One"] and len(ar[1]["objects"]) == 1 and ar[1]["id"] == 2, "stage archetype %r" % ar)
    check(len(objects(f(c, "bs-o2.txt"))[0]) == before, "stage objects left behind")
    o3 = objects(f(c, "bs-o3.txt"))[0]
    check(len(o3) == before + 3, "Edit Parts stage: plate + 2 parts expected (%d -> %d)" % (before, len(o3)))
    check("Finish the archetype stage first (Save or Cancel)." in popup(f(c, "bs-active.txt"))["text"], "browser opened during the stage")
    st = state(f(c, "bs-s3.txt"))
    check(st["archEdit"] == "0", "Cancel did not leave the stage")
    check(popup(f(c, "bs-cancel.txt"))["text"].startswith("Archetypes"), "Cancel did not return to the browser")
    check(export(f(c, "bs-e5.json"))["archetypes"] == ar, "Cancel changed the archetypes")
    check(len(objects(f(c, "bs-o4.txt"))[0]) == before, "Cancel left stage objects")


def case_part_edit(c):
    s = archetype_setup() + ["ui:KE.createEntity(1)", "objdump:" + f(c, "pe-o0.txt"), "ui:KE.archetypePopup()",
                             "entity:val:.archetypePartName#0=lid", "entity:check:.archetypePartSoft#1=1", "ui:KE.saveArchetypeData()",
                             "ui:KE.archetypePopup()", "ui:KE.editEntityParts()", "objdump:" + f(c, "pe-o1.txt"),
                             "entity:state:" + f(c, "pe-s1.txt"), "entity:popup:" + f(c, "pe-msg.txt"),
                             "entity:menu:Edit Selected", "entity:popup:" + f(c, "pe-pop.txt"),
                             "ui:KE.applyEntityParts()", "objdump:" + f(c, "pe-o2.txt"), "entity:state:" + f(c, "pe-s2.txt"),
                             "export:" + f(c, "pe-e1.json")]
    run_editor(c, ";".join(s), "parts")
    o0 = objects(f(c, "pe-o0.txt"))[0]
    ent0 = [d for n, d in o0 if n == "ENTITY"][0]
    objs, groups = objects(f(c, "pe-o1.txt"))
    names = [n for n, _ in objs]
    check("ENTITY" not in names and names.count("CUBE") == 4 and len(groups) == 2, "Edit Parts objects %r %r" % (names, groups))
    parts = [d for n, d in objs if n == "CUBE"][2:]
    check(any(d.get("pn") == "lid" for d in parts) and any(d.get("sfz") == 1 for d in parts), "part records %r" % parts)
    st = state(f(c, "pe-s1.txt"))
    check(st["partEdit"] == "1" and st["partArId"] == "1", "part edit state %r" % st)
    check("Editing parts" in popup(f(c, "pe-msg.txt"))["text"], "editingParts message")
    p = popup(f(c, "pe-pop.txt"))
    check("Apply Parts" in p["text"] and "Cancel Edit" in p["text"], "archetype popup during the part edit %r" % p["text"])
    objs, groups = objects(f(c, "pe-o2.txt"))
    ents = [d for n, d in objs if n == "ENTITY"]
    check(len(ents) == 1 and ents[0]["eid"] == ent0["eid"] and ents[0]["ar"] == 1, "entity after Apply %r" % ents)
    check(state(f(c, "pe-s2.txt"))["partEdit"] == "0", "Apply did not end the part edit")
    a = export(f(c, "pe-e1.json"))["archetypes"][0]
    check(len(a["objects"]) == 2 and any(o.get("n") == "lid" for o in a["objects"]) and any(o.get("soft") == 1 for o in a["objects"]),
          "archetype after Apply %r" % a)
    # Cancel Edit and Unpack Parts
    s = archetype_setup() + ["ui:KE.createEntity(1)", "ui:KE.editEntityParts()", "ui:KE.cancelEntityParts()", "objdump:" + f(c, "pc-o1.txt"),
                             "entity:state:" + f(c, "pc-s1.txt"), "select:3", "ui:KE.archetypePopup()", "ui:KE.unpackArchetype()",
                             "objdump:" + f(c, "pc-o2.txt")]
    run_editor(c, ";".join(s), "cancel")
    names = [n for n, _ in objects(f(c, "pc-o1.txt"))[0]]
    check(names.count("ENTITY") == 1 and names.count("CUBE") == 2, "Cancel Edit objects %r" % names)
    check(state(f(c, "pc-s1.txt"))["partEdit"] == "0", "Cancel did not end the part edit")
    names = [n for n, _ in objects(f(c, "pc-o2.txt"))[0]]
    check("ENTITY" not in names and names.count("CUBE") == 4, "Unpack Parts objects %r" % names)


def case_prefab_entities(c):
    dangling = '[{"p":[0,0,0],"s":[2,2,2]},{"i":98,"p":[0,0,0],"eid":5,"ar":9}]'
    with_entity = '[{"p":[0,0,0],"s":[2,2,2]},{"i":98,"p":[3,0,0],"eid":5,"ar":1}]'
    s = ["clear", "add:%d" % CUBE, "select:0", "entity:menu:JSON", "entity:prompt:" + dangling, "entity:popup:" + f(c, "pf-dang.txt"),
         "objdump:" + f(c, "pf-o1.txt")]
    # archetype 1 exists: the entity of the second prefab is kept, in a group with a cube (objects 3..5)
    s += archetype_setup() + ["add:%d" % CUBE, "select:3", "entity:menu:JSON", "entity:prompt:" + with_entity, "objdump:" + f(c, "pf-o2.txt"),
                              "select:5", "entity:menu:Export#1", "entity:popup:" + f(c, "pf-strip.txt")]
    run_editor(c, ";".join(s), "prefab")
    p = popup(f(c, "pf-dang.txt"))
    check("1 imported entity instance(s) were dropped" in p["text"], "dangling popup %r" % p["text"])
    names = [n for n, _ in objects(f(c, "pf-o1.txt"))[0]]
    check("ENTITY" not in names and "CUBE" in names, "dangling import objects %r" % names)
    objs, groups = objects(f(c, "pf-o2.txt"))
    names = [n for n, _ in objs]
    check(names[3:] == ["CUBE", "ENTITY", "PLACEHOLDER"] and len(groups) == 2, "prefab with an entity: %r %r" % (names, groups))
    p = popup(f(c, "pf-strip.txt"))
    check("1 entity instance(s) were left out of the prefab (prefabs cannot carry archetypes)." in p["text"], "prefabStripped popup %r" % p["text"])
    check(p["prompt"] == "Name your Prefab", "the prefab name prompt %r" % p["prompt"])


CASES = [
    ("save_archetype", case_save_archetype),
    ("place_entity", case_place_entity),
    ("archetype_popup", case_archetype_popup),
    ("browser_stage", case_browser_stage),
    ("part_edit", case_part_edit),
    ("prefab_entities", case_prefab_entities),
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
    c.verbose = a.verbose
    c.egl = os.environ.get("KRUNKNATIVE_EGL_DEVICE", "")
    c.tmp = tempfile.mkdtemp(prefix="kn-entities-e2e-", dir=os.path.expanduser("~/.cache"))
    failed = ran = 0
    for name, fn in CASES:
        if a.case and name not in a.case:
            continue
        ran += 1
        try:
            fn(c)
            print("ok   %s" % name)
        except (AssertionError, OSError, subprocess.SubprocessError, ValueError, KeyError, IndexError) as e:
            failed += 1
            print("FAIL %s: %s: %s" % (name, type(e).__name__, e))
    if not failed:
        shutil.rmtree(c.tmp, ignore_errors=True)
    else:
        print("runs kept in %s" % c.tmp)
    print("entities e2e: %d/%d passed" % (ran - failed, ran))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
