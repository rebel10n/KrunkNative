#!/usr/bin/env python3
"""Generates client/src/editor/editor_data.c from the Krunker v7.1.4 sources.

    python3 -I tools/editor/gen_editor_data.py

tools/maps/dump_v7.js loads the readable v7.1.4 data/prefabs.js, config.js, data/triggers.js,
data/gameModes.js, data/classes.js, data/weapons.js and data/effects.js under node (the same
dump tools/maps/gen_prefab_data.py builds the shared prefab / trigger tables from), and
tools/editor/dump_editor_v7.js the editor-only data (class / effect values, KrunkScript syntax,
templates, map / asset categories); this script writes

- the prefab table (name, label, default size, model scale, line colour, opacity, model variant
  count, fakeScale mode, editor menu category, every boolean flag the editor GUI reads) as C
  structs (client/src/editor/editor_prefab_flags.h has the EP_* bits: the v3.1.1 flag bits keep
  their positions, the v7 flags follow from bit 64, so EditorPrefabFlags is 128 bits wide), and
  the texture names and variant counts,
- the configuration lists the editor panels are built from (serverConfig, weaponConfig,
  classConfig, effectConfig, modes, classes, weapons, effects, textures, particles, emitter /
  step sounds, trigger events / conditions / actions, limits, mapCats, assetCat, custom GUI lists,
  perks, KrunkScript templates / autocomplete / aliases) as one JSON string parsed once at run time
  (editor_data_json()), and
- assets/editor/templates.json: the Templates window data (18 script templates and the game
  templates as map JSON strings, the default map list). It is ~250 KB, so it is loaded at run
  time (ed_templates_get) instead of being compiled in.

Krunker v10 (the live editor; doc/v10/editor-social.md): the prefab table gets the v10 appendix of
tools/maps/gen_prefab_data.py (97 SCRAPCRATE, 98 ENTITY, canDynamic on CUBE / TRIGGER /
CUSTOM_ASSET, condition 12 and actions 72-74), id 41 is v10's BOT_PATH_NODE (V10_BOT_PATH_NODE; v7
TERMINAL was localOnly, so the public editor never offered it), triggerActionLimit is 8 and the
entity limits are added (V10_LIMITS; webcrack shared/config/renamed.js:7866-7871).

"gameConfig" is the v3.1.1 map-wide "game" panel (tools/editor/v3_game_config.json). v7 replaced it
with the per-class "cls" config; the editor imports a "game" block into every class and its left
panel's Game folder writes every class until the v7 Class Config panel lands (doc/v7/editor.md WP4).
"""

import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
OUT = os.path.join(REPO, "client", "src", "editor", "editor_data.c")
TEMPLATES_OUT = os.path.join(REPO, "assets", "editor", "templates.json")
V7_PUBLIC = "/home/user/jails/kru/src/Krunker_v7.1.4/public"
# KrunkScript autocomplete and templates of Krunker v10.0.2 (E10:11795-12624, see the file's "source")
KSS_V10 = os.path.join(HERE, "kss_v10.json")
# Krunker v10.0.2's editor bundle, webcracked and renamed (doc/v10/editor-social.md): the Templates
# window's KrunkScript templates (const _2189, 29 entries) replace v7's 18. Read as text; the array
# literal is parsed without running it. Missing bundle: the v7 list stays.
V10_EDITOR_BUNDLE = os.environ.get("KN_V10_EDITOR_BUNDLE",
                                   os.path.expanduser("~/.cache/kn-v10/webcrack/editor/renamed.js"))

# The v3.1.1 editor flags, bits 0..63 (their positions are kept: code may store the low word).
FLAGS_V3 = [
    "editFaces", "editFog", "movingTexture", "hasHealth", "scalable", "editAmb", "editColor",
    "editEmissive", "editOpac", "editPen", "texturable", "assetTextures", "hasBorder",
    "canInterface", "canStretch", "canBeDynamic", "canWallJump", "gen", "genGeo", "editorGen",
    "tool", "tool2", "ai", "notTechnical", "notStyleable", "teamable", "customDirection",
    "optionalDirection", "boostable", "bounce", "edgeNoise", "hasSpin", "customScore",
    "interact", "interact2", "canUndo", "canTargetInterface", "objectiveLink", "light",
    "hasText", "complex", "custom", "noExport", "localOnly", "noCol", "noCollision",
    "forceCollision", "hideBoundingBox", "dontRound", "alwaysSee", "isCam", "fakeScale",
    "wireframe", "noDefault", "hasSound", "canTerrain", "isProp", "scaleWithSize",
    "doubleSide", "transparent", "emiss", "noTexture", "shootable",
]

# defaultSize of genGeo prefabs without one: the bounds of the geometry they generate
GENGEO_SIZES = {
    "SPAWN_POINT": [7, 11, 7],  # generateSpawn: generateCube(..., 7, 11, 7)
}

NON_FLAGS = {"id", "name", "src", "defaultSize", "scale", "lineCol", "opacity", "edCat", "texRange",
             "variants", "gen", "CP", "CPY", "mlt", "alphaTest", "blending", "frameT", "frames", "width"}


def run_node(script):
    out = subprocess.run(["node", script], check=True, capture_output=True, text=True, cwd="/tmp").stdout
    return json.loads(out)


# v10 L155465 (the client bundle, read, never run): BOT_PATH_NODE replaces TERMINAL at 41
V10_BOT_PATH_NODE = {"id": 41, "name": "BOT_PATH_NODE", "src": "cube_0", "noCol": True, "has_genGeo": True,
                     "notTechnical": True, "notStyleable": True, "defaultSize": [3, 5, 3], "lineCol": 65535,
                     "noTexture": True, "hideBoundingBox": False, "noCollision": True, "tool": True}
V10_LIMITS = {"triggerActionLimit": 8, "entityLimit": 64, "archetypeLimit": 32, "archetypeObjectLimit": 40}


# v10 configuration lists (webcrack shared/config/renamed.js and shared/classes/renamed.js; read, never
# run). Each entry is inserted after the v7 key it follows in v10's list (v10 keeps v7's order).
# serverConfig: hurtboxScale (CFG:8370s, "Hitboxes"), noPvp (CFG:7641, "Teams"), transferHostOnLeave
# (CFG:7719, "Hosting"), webhook (CFG:7796, comp hosts; an input). classConfig: hurtboxScale under
# "Hitbox" (CFG:8334). Weapons 29 Boulder and 30 Compressor (classes/renamed.js:2969-3102): the
# weaponConfig fields their records set, then the defaults v10 fills in for every weapon
# (classes/renamed.js:3109-3120: adSpread 0, reloY 1, movSpread 1, lsMlt 0.5, burst false / burstC 0 /
# burstR 50). The editor shows only the fields a record has, as v10's Weapon Config does.
V10_SERVER_ADDS = [
    ("hitBoxPad", {"name": "Hurtbox Scale", "varN": "hurtboxScale", "sub": "Hitboxes", "dontChange": True,
                   "def": 1, "min": 1, "max": 2, "step": 0.1}),
    ("frFire", {"name": "Player Damage Off", "varN": "noPvp", "sub": "Teams", "dontChange": True,
                "def": False, "bool": True}),
    ("aAnon", {"name": "Transfer Host on Leave", "varN": "transferHostOnLeave", "sub": "Hosting",
               "def": True, "bool": True}),
    ("headClipFix", {"name": "Webhook", "varN": "webhook", "sub": "Other", "def": "", "input": True}),
]
V10_CLASS_ADDS = [
    ("hbH", {"name": "Hurtbox Scale", "varN": "hurtboxScale", "sub": "Hitbox", "cat": 1, "def": 1, "min": 1,
             "max": 2, "step": 0.1}),
]
V10_WEAPON_DEFAULTS = {"adSpread": 0, "reloY": 1, "movSpread": 1, "lsMlt": 0.5, "burst": False, "burstC": 0, "burstR": 50}
V10_WEAPONS = [
    {"name": "Boulder", "src": "weapon_30", "assetIcon": "", "refDist": 25, "rollOff": 1, "secondary": True,
     "equipment": False, "noAim": True, "aimSpd": 140, "chrgTime": 0, "ovrChrg": False, "swapTime": 200,
     "spdMlt": 0.85, "spread": 120, "zoom": 1},
    {"name": "Compressor", "src": "weapon_31", "assetIcon": "", "refDist": 25, "rollOff": 1, "secondary": True,
     "equipment": False, "noSpread": False, "noAim": True, "akimbo": False, "nAuto": True, "ovrChrg": False,
     "swapTime": 350, "aimSpd": 100, "chrgTime": 0, "spdMlt": 1.05, "ammo": 1, "reload": 2000, "dmg": 1,
     "hsMlt": 1.25, "dmgDrop": 10, "range": 40, "rate": 450, "spread": 180, "minSpread": 20, "zoom": 0,
     "recoil": 0.02, "recoilYM": 0.65, "recoilZ": 2.1, "recoilZM": 0.2, "jumpYM": 0.5, "physRang": 50,
     "physPow": 0.1},
]


def add_v10_config(data):
    """The v10 serverConfig / classConfig / weapon entries the v7 lists lack (see V10_*)."""
    config = data["config"]
    for lst, adds in (("serverConfig", V10_SERVER_ADDS), ("classConfig", V10_CLASS_ADDS)):
        entries = config[lst]
        for after, entry in adds:
            if any(e.get("varN") == entry["varN"] for e in entries):
                continue
            idx = next(i for i, e in enumerate(entries) if e.get("varN") == after)
            entries.insert(idx + 1, dict(entry))
    if len(data["weapons"]) != 29:
        raise SystemExit("v7 weapon list is not 29 entries: re-check V10_WEAPONS")
    for w in V10_WEAPONS:
        rec = dict(w)
        for k, v in V10_WEAPON_DEFAULTS.items():
            rec.setdefault(k, v)
        data["weapons"].append(rec)
    # the weaponConfig fields: v7's list has every v10 field (checked against classes/renamed.js)


def dump_v7():
    data = run_node(os.path.join(REPO, "tools", "maps", "dump_v7.js"))
    sys.path.insert(0, os.path.join(REPO, "tools", "maps"))
    import gen_prefab_data  # the v10 appendix the shared tables use (tools/maps/gen_prefab_data.py)
    gen_prefab_data.add_v10(data)
    if data["prefabs"][41]["name"] != "TERMINAL":
        raise SystemExit("v7 prefab 41 is not TERMINAL: re-check V10_BOT_PATH_NODE")
    data["prefabs"][41] = dict(V10_BOT_PATH_NODE)
    data["config"].update(V10_LIMITS)
    add_v10_config(data)
    return data


def dump_editor_v7():
    return run_node(os.path.join(HERE, "dump_editor_v7.js"))


def format_const_name(name):
    return " ".join(p[:1].upper() + p[1:].lower() for p in name.lower().split("_"))


def c_str(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def c_float(v):
    r = repr(float(v))
    return r + "f" if ("." in r or "e" in r) else r + ".0f"


def model_size(prefab):
    """defaultSize of a model prefab: the bounds of its OBJ at prefab.scale (ObjectInstance.promises)."""
    m = re.search(r'"models/([a-z_0-9]+)\.obj"', prefab.get("gen") or "")
    if not m:
        return None
    for root in (os.path.join(REPO, "assets", "models"), os.path.join(V7_PUBLIC, "models")):
        path = os.path.join(root, m.group(1) + ".obj")
        if not os.path.exists(path):
            continue
        lo, hi = [1e9] * 3, [-1e9] * 3
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            for line in fh:
                if line.startswith("v "):
                    v = [float(x) for x in line.split()[1:4]]
                    lo = [min(a, b) for a, b in zip(lo, v)]
                    hi = [max(a, b) for a, b in zip(hi, v)]
        scale = prefab.get("scale") or 1
        return [round((b - a) * scale, 3) for a, b in zip(lo, hi)]
    return None


def flag_set(prefab, flag):
    if flag in ("gen", "genGeo", "editorGen"):
        return bool(prefab.get("has_" + flag))
    return bool(prefab.get(flag))


def v10_script_templates(path):
    """The v10 KrunkScript templates (E10 const _2189) as {name, icon, data} dicts, or None."""
    import re
    try:
        with open(path, encoding="utf-8") as fh:
            src = fh.read()
    except OSError:
        return None
    m = re.search(r"const _2189 = \[", src)
    if not m:
        return None
    end = src.find("\n}];", m.end())
    if end < 0:
        return None
    body = src[m.end() - 1:end + 3]
    out = []
    for e in re.finditer(r'name: ("(?:[^"\\]|\\.)*"),\s*icon: ([^,\n]+),\s*data: ("(?:[^"\\]|\\.)*")', body):
        icon = e.group(2).strip()
        out.append({"name": json.loads(e.group(1)), "icon": int(icon) if icon.isdigit() else "",
                    "data": json.loads(e.group(3))})
    return out or None


def main():
    data = dump_v7()
    ed = dump_editor_v7()
    with open(KSS_V10, "r", encoding="utf-8") as fh:
        kss_v10 = json.load(fh)
    # v10 replaced the v7 autocomplete (211 client/server entries) with 227 and added the shared
    # tab's 48 entries and template; the templates' other keys stay v7's
    kss = dict(ed["kss"])
    kss["autoComplete"] = kss_v10["autoComplete"]
    kss["sharedAutoComplete"] = kss_v10["sharedAutoComplete"]
    kss["templates"] = dict(ed["kss"]["templates"], **kss_v10["templates"])
    prefabs, textures, config = data["prefabs"], data["textures"], data["config"]
    # v10 fills every classConfig default into each class record (classes/renamed.js:4632-4636), so the
    # Class Config panel shows the v10 keys on every class
    for c in ed["classes"]:
        for _, entry in V10_CLASS_ADDS:
            c.setdefault(entry["varN"], entry["def"])

    v7_flags = set()
    for p in prefabs:
        for k, v in p.items():
            if not k.startswith("has_") and k not in NON_FLAGS and isinstance(v, bool):
                v7_flags.add(k)
    flags = FLAGS_V3 + sorted((f for f in v7_flags if f not in FLAGS_V3), key=str.lower)
    if len(flags) > 128:
        raise SystemExit("more than 128 editor flags")

    lines = [
        "// GENERATED by tools/editor/gen_editor_data.py from the Krunker v7.1.4 sources",
        "// (tools/maps/dump_v7.js: data/prefabs.js, config.js, data/*.js) + the Krunker v10 appendix. Do not edit.",
        "#include \"editor_data.h\"",
        "",
        "const EditorPrefab editor_prefabs[EDITOR_PREFAB_COUNT] = {",
    ]
    for p in prefabs:
        on = [("EP_" + f.upper()) for f in flags if flag_set(p, f)]
        size = p.get("defaultSize")
        computed = 0
        if not size and p["name"] in GENGEO_SIZES:
            size = GENGEO_SIZES[p["name"]]  # bounds of the genGeo cube (ObjectInstance.promises)
            computed = 2
        if not size:
            size = model_size(p)
            computed = 2 if size else 0
        line_col = p.get("lineCol")
        fake = p.get("fakeScale")
        lines.append("    {%s, %s, {%s}, %d, %s, %d, %s, %d, %d, %s, %s}," % (
            c_str(p["name"]), c_str(format_const_name(p["name"]) if p["name"] else ""),
            ", ".join(c_float(v) for v in (size or [1, 1, 1])), (computed or 1) if size else 0,
            c_float(p.get("scale", 1) or 1),
            line_col if line_col is not None else -1,
            c_float(p.get("opacity", 1) if p.get("opacity") is not None else 1),
            len(p.get("variants") or []),
            (2 if fake == 2 else 1) if fake else 0,
            c_str(p["edCat"]) if p.get("edCat") else "NULL",
            " | ".join(on) if on else "0",
        ))
    lines.append("};")
    lines.append("")
    lines.append("const char *const editor_textures[EDITOR_TEXTURE_COUNT] = {")
    lines.append("    " + ", ".join(c_str(t["name"]) for t in textures) + ",")
    lines.append("};")
    lines.append("")
    lines.append("const unsigned char editor_texture_variants[EDITOR_TEXTURE_COUNT] = {")
    lines.append("    " + ", ".join(str(len(t.get("variants") or [])) for t in textures) + ",")
    lines.append("};")
    lines.append("")

    with open(os.path.join(HERE, "v3_game_config.json"), "r", encoding="utf-8") as fh:
        game_config = json.load(fh)
    weapon_keys = ["name", "src"] + [c["varN"] for c in config["weaponConfig"] or []]
    trig = data["triggers"]
    blob = {
        "gameConfig": game_config,
        "serverConfig": config["serverConfig"],
        "weaponConfig": config["weaponConfig"],
        "classConfig": config["classConfig"],
        "effectConfig": config["effectConfig"],
        "interfaceIDMax": config["interfaceIDMax"],
        "modes": [{k: m.get(k) for k in ("name", "id", "cost") if m.get(k) is not None} |
                  ({"pcg": True} if m.get("preventCustomGames") else {}) for m in data["modes"]],
        # every Class / Effect Config value (the export drops values equal to them)
        "classes": ed["classes"],
        "weapons": [{k: w.get(k) for k in weapon_keys if w.get(k) is not None} for w in data["weapons"]],
        "effects": ed["effects"],
        "textures": [{k: t.get(k) for k in ("name", "alias", "src") if t.get(k)} |
                     ({"variants": [v["name"] for v in t["variants"]]} if t.get("variants") else {}) for t in textures],
        "particleIDS": config["particleIDS"],
        "emitterSounds": config["emitterSounds"],
        "particleBlending": config["particleBlending"],
        "stepSounds": config["stepSounds"],
        "encodingTypes": config["encodingTypes"],
        "inputTypes": config["inputTypes"],
        "cameraTypes": config["cameraTypes"],
        "pathTypes": config["pathTypes"],
        "soundScapes": config["soundScapes"],
        "triggers": {
            "events": trig["events"],
            "conditions": trig["conditions"],
            # "custom" (the v3.1.1 Broadcast Sound flag) = v7 hasSound
            "actions": [{k: a[k] for k in ("name", "type", "hasCust", "hasExtra", "devOnly", "actionValue", "actionValue2",
                                           "actionValueTxt", "actionOptions", "actionOptions2", "bypassType") if k in a} |
                        {"custom": bool(a.get("hasSound"))} for a in trig["actions"]],
        },
        "limits": {k: config[k] for k in ("spawnLimit", "signLimit", "objectLimit", "objectColLimit", "objectLimitF",
                                          "objectColLimitF", "aiLimit", "botLimit", "lbLimit", "wheelLimit", "anchorLimit",
                                          "triangleLimit", "interfaceLimit", "triggerActionLimit", "guiLimit",
                                          "signFontMax", "signTextLimit", "entityLimit", "archetypeLimit",
                                          "archetypeObjectLimit")} | ed["limits"],
        "mapCats": ed["mapCats"],
        "assetCat": ed["assetCat"],
        "custGUITypes": ed["custGUITypes"],
        "perks": ed["perks"],  # PERK_MACHINE "zp" options (config.zombiePerks)
        "inputTypesGUI": ed["inputTypesGUI"],
        "zoneColors": ed["zoneColors"],
        "kss": kss,
        "defaults": {k: config[k] for k in ("shadowRes", "shadowDst", "lightDistance", "lightIntensity",
                                            "ambIntensity", "sunAngleX", "sunAngleY")},
        # object inspector (WP3): model variant names by prefab id (variant id = index) and the
        # model's src (the T-key list's img/editor-models/<src>[_<id>].png)
        "prefabVariants": {str(i): {"src": p.get("src") or "", "names": [v["name"] for v in p["variants"]]}
                           for i, p in enumerate(prefabs) if p.get("variants")},
    }
    text = json.dumps(blob, separators=(",", ":"), ensure_ascii=True)
    lines.append("// the editor's configuration lists, parsed once by editor_data_json()")
    lines.append("const char editor_data_text[] =")
    for i in range(0, len(text), 100):
        lines.append("    " + c_str(text[i:i + 100]))
    lines.append(";")
    lines.append("")

    header_flags = []
    for i, f in enumerate(flags):
        header_flags.append("#define EP_%-22s ((EditorPrefabFlags) 1 << %d)" % (f.upper(), i))
    flags_path = os.path.join(REPO, "client", "src", "editor", "editor_prefab_flags.h")
    with open(flags_path, "w", encoding="utf-8") as fh:
        fh.write("// GENERATED by tools/editor/gen_editor_data.py: Krunker v7.1.4 data/prefabs.js flags (+ v10's)\n"
                 "// (bits 0-63: the v3.1.1 editor flags, kept in place; 64+: the flags v7 added).\n#pragma once\n\n"
                 "__extension__ typedef unsigned __int128 EditorPrefabFlags;\n\n")
        fh.write("#define EDITOR_PREFAB_COUNT %d\n#define EDITOR_TEXTURE_COUNT %d\n\n" % (len(prefabs), len(textures)))
        fh.write("\n".join(header_flags) + "\n")
    with open(OUT, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines))
    templates = ed["templates"]
    v10_scripts = v10_script_templates(V10_EDITOR_BUNDLE)
    if v10_scripts:
        templates["script"] = v10_scripts
    os.makedirs(os.path.dirname(TEMPLATES_OUT), exist_ok=True)
    with open(TEMPLATES_OUT, "w", encoding="utf-8") as fh:
        json.dump(templates, fh, separators=(",", ":"), ensure_ascii=False)
        fh.write("\n")
    print("wrote %s (%d prefabs, %d textures, %d flags, %d bytes of config)" % (
        os.path.relpath(OUT, REPO), len(prefabs), len(textures), len(flags), len(text)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
