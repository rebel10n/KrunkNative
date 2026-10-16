#!/usr/bin/env python3
"""Generates tools/editor-mcp/data/schema.json, the data kn_editor_mcp validates maps with.

    python3 -I tools/editor-mcp/gen/gen_schema.py [--v7 /path/to/Krunker_v7.1.4/src]

Inputs:
  - client/src/editor/editor_data.c (generated from the v3.1.1 editor by
    tools/editor/gen_editor_data.py): the v3 prefab rows (model bounds for prefabs without
    a defaultSize) and the editor configuration lists (gameConfig, serverConfig, ...).
  - the Krunker v7.1.4 reference (read-only), through dump_v7.js under node: the v7 prefab
    table (97 prefabs), textures (20), trigger events / actions / conditions, serverConfig and
    the limits.
  - PROPERTIES / MAP_KEYS below: the object property table (friendly name, map key, type,
    range, which prefabs may carry it) and the top-level map keys. They are hand-maintained
    from ObjectInstance.js / map.js / editor.js (v3.1.1 and v7.1.4) and from KrunkNative's
    client editor (client/src/editor/editor_objects.c PROPS, editor_panels.c).

The output is data only: kn_editor_mcp embeds it at build time and an edited copy under
--root (tools/editor-mcp/data/schema.json) or --schema overrides it at run time, so new v7
fields drop in without touching C.
"""

import argparse
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
EDITOR_DATA = os.path.join(REPO, "client", "src", "editor", "editor_data.c")
OUT = os.path.join(os.path.dirname(HERE), "data", "schema.json")

# v3.1.1 names of renamed prefabs (map.c prefab_names, string "i" values of old maps)
V3_ALIASES = {33: ["SPECTATE_CAM"], 55: ["BOT"]}

# The native engine (shared/src/map.c, shared/src/triggers.c) implements the v10.0.2 trigger set
# minus the entity actions: every event (32, trigger_v7_data.c; the runtime dispatches all of them),
# actions 0-71 (triggers_v7.h "actions 0-71"); 72-74 (Spawn / Despawn Entity, Set Entity Value) wait
# for the entity runtime and do nothing (TriggerScriptHooks TODO).
NATIVE_PREFABS = 60
NATIVE_EVENTS = 32
NATIVE_ACTIONS = 72
V3_EVENT_NAMES = {3: "onEnter", 10: "onInteract"}

# when: which prefabs may carry a property. A prefab qualifies when its name is in
# "prefabs" or it has any flag of "flags" (no flags and no prefabs = every prefab), and
# none of "not_flags". since: first Krunker version with the key.
def P(prop, key, typ, default=None, lo=None, hi=None, flags=None, prefabs=None, not_flags=None,
      since="v3", desc="", alt=None, dec=None, maxlen=None, values=None, palette=None):
    d = {"prop": prop, "key": key, "type": typ}
    if default is not None: d["default"] = default
    if lo is not None: d["min"] = lo
    if hi is not None: d["max"] = hi
    if dec is not None: d["decimals"] = dec
    if maxlen is not None: d["max_length"] = maxlen
    if values is not None: d["values"] = values
    if palette is not None: d["palette_key"] = palette
    if alt: d["alt_keys"] = alt
    w = {}
    if flags: w["flags"] = flags
    if prefabs: w["prefabs"] = prefabs
    if not_flags: w["not_flags"] = not_flags
    if w: d["when"] = w
    d["since"] = since
    if desc: d["desc"] = desc
    return d

# types: vec3, num, int, bool_present (true <=> key present, written 1), bool_absent (true <=>
# key absent, false writes 1), str, color ("#rrggbb", folded into the "colors" palette as
# <key>i when the map has one), texture (index or name), faces (6 booleans, hex string), any.
PROPERTIES = [
    P("position", "p", "vec3", desc="bottom-centre position (y is the bottom face)"),
    P("size", "s", "vec3", desc="size; pooled maps store an index into the top-level xyz array as si", alt=["si"]),
    P("rotation", "r", "vec3", default=[0, 0, 0], desc="rotation in radians (x, y, z); omitted when zero"),
    P("prefab", "i", "int", default=0, lo=0, desc="prefab id (omitted for CUBE); legacy maps use id"),
    P("visible", "v", "bool_absent", desc="false writes v:1 (noVis)"),
    P("collidable", "l", "bool_absent", alt=["col"], not_flags=["tool", "forceCollision", "noCollision"],
      desc="false writes l:1 (no collision)"),
    P("penetrable", "pe", "bool_present", flags=["editPen"], desc="bullets pass through (wallbang)"),
    P("wall_jumpable", "wj", "bool_absent", flags=["canWallJump"]),
    P("boost", "b", "num", 0, -10, 10, flags=["boostable"], dec=2),
    P("bounce", "bm", "num", 1, 0.1, 3, flags=["bounce"], dec=2),
    P("crouch_reduces", "cr", "bool_absent", flags=["bounce"]),
    P("edge_noise", "en", "num", 0, -5, 5, flags=["edgeNoise"], dec=1),
    P("border", "bo", "bool_present", flags=["hasBorder"]),
    P("spin", "spn", "num", 0, -20, 20, flags=["hasSpin"]),
    P("open_ended", "opn", "bool_present", prefabs=["CYLINDER"]),
    P("direction", "d", "int", 0, 0, 3, flags=["customDirection"],
      prefabs=["CHECK_POINT", "TELEPORTER", "SPAWN_POINT"]),
    P("force_direction", "fd", "bool_present", flags=["optionalDirection"], prefabs=["TELEPORTER"]),
    P("team", "tm", "int", 0, 0, 2, flags=["teamable"],
      prefabs=["TRIGGER", "TERMINAL", "GATE", "DEPOSIT_BOX", "CHECK_POINT", "TELEPORTER", "SPAWN_POINT"],
      desc="0 default/any, 1 team 1, 2 team 2 (spawns: team id)"),
    P("model_size", "ms", "num", None, 0.1, 100, flags=["gen"], dec=1),
    P("item_id", "skin", "int", 0, 0, None, prefabs=["SHOWCASE"]),
    P("objective_id", "obi", "int", 0, -1, 10, flags=["objectiveLink"]),
    P("texture", "t", "texture", 0, flags=["texturable"], desc="texture index or name (krunker://textures)"),
    P("texture_variant", "tv", "int", 0, 0, 20, flags=["texturable"], since="v7"),
    P("asset_texture", "at", "any", flags=["assetTextures"], desc="custom texture asset id (not available offline)"),
    P("texture_stretch", "tsr", "bool_present", flags=["canStretch"]),
    P("texture_offset_x", "tox", "num", 0, 0, 1, flags=["texturable"], dec=1),
    P("texture_offset_y", "toy", "num", 0, 0, 1, flags=["texturable"], dec=1),
    P("texture_rotation", "tro", "num", 0, flags=["texturable"], since="v7", alt=["tr"]),
    P("texture_scale", "tsm", "num", 1, flags=["texturable"], since="v7"),
    P("moving_speed", "ts", "num", 0, -20, 20, flags=["movingTexture"], dec=1),
    P("moving_axis", "td", "int", 0, 0, 1, flags=["movingTexture"]),
    P("color", "c", "color", "#ffffff", flags=["editColor"], palette="ci"),
    P("emissive", "e", "color", "#000000", flags=["editEmissive"], palette="ei"),
    P("opacity", "o", "num", 1, 0, 1, flags=["editOpac"], dec=1),
    P("texture_smoothing", "txs", "bool_absent", flags=["custom", "texturable"]),
    P("shading", "ab", "bool_absent", not_flags=["complex"], desc="ambient shading; false writes ab:1"),
    P("fog", "nf", "bool_absent", flags=["editFog"], desc="false writes nf:1 (ignores fog)"),
    P("full_cone", "fc", "bool_present", prefabs=["LIGHT_CONE"]),
    P("faces", "f", "faces", flags=["editFaces"], desc="6 booleans Right Left Top Bottom Back Front, stored as hex"),
    P("light_intensity", "li", "num", 1, 0, 2, flags=["light"], dec=1),
    P("light_distance", "lds", "num", 0, 0, 1000, flags=["light"], dec=1),
    P("light_decay", "ld", "num", 1, 0, 2, flags=["light"], dec=1),
    P("light_angle", "la", "num", None, flags=["light"], since="v7"),
    P("light_penumbra", "lp", "num", None, flags=["light"], since="v7"),
    P("light_target", "lt", "vec3", flags=["light"], prefabs=["DIRECTIONAL_LIGHT", "SPOT_LIGHT"], since="v7"),
    P("withdraw", "wd", "int", 1000, 0, 100000, prefabs=["DEPOSIT_BOX"]),
    P("deposit", "dp", "int", 1000, 0, 100000, prefabs=["DEPOSIT_BOX"]),
    P("score", "sp", "int", None, 0, 10000, flags=["customScore", "interact"], prefabs=["GATE"]),
    P("single_use", "su", "bool_present", flags=["ai"], prefabs=["CHECK_POINT", "TRIGGER"]),
    P("single_use_player", "sup", "bool_present", prefabs=["TRIGGER"], since="v7"),
    P("method", "md", "int", 0, 0, 2, flags=["interact"], desc="gate: 0 cost score, 1 require score, 2 require kills"),
    P("closeable", "cl", "bool_present", flags=["canUndo"]),
    P("start_closed", "stc", "bool_present", flags=["canUndo"]),
    P("no_popup", "nm", "bool_present", flags=["interact", "interact2"], prefabs=["TERMINAL"]),
    P("destructible", "cdy", "bool_present", flags=["hasHealth"]),
    P("start_destroyed", "sd", "bool_present", flags=["hasHealth"]),
    P("self_destruct", "sfd", "bool_present", prefabs=["TRIGGER"]),
    P("health", "h", "int", 0, 0, 20000, flags=["hasHealth"], alt=["hp"]),
    P("respawn_time", "rt", "num", 0, 0, 300, flags=["hasHealth"], dec=1),
    P("respawn_random", "rr", "bool_present", flags=["hasHealth"]),
    P("spawn_chance", "rsc", "num", None, 0, 100, flags=["hasHealth"], since="v7"),
    P("weapon", "w", "int", 0, 0, 40, prefabs=["WEAPON_PICKUP"], alt=["wid"]),
    P("weapon_replaceable", "wr", "bool_absent", prefabs=["WEAPON_PICKUP"]),
    P("random_weapon", "rw", "bool_present", prefabs=["WEAPON_PICKUP"]),
    P("random_weapon_group", "rwg", "any", prefabs=["WEAPON_PICKUP"], since="v7"),
    P("hide_pickup_icon", "wpi", "bool_present", prefabs=["WEAPON_PICKUP"], since="v7"),
    P("event", "te", "int", 0, 0, 31, flags=["ai"], prefabs=["TRIGGER"], desc="trigger event (krunker://triggers)"),
    P("event_value", "tce", "int", 0, 0, 10000, flags=["ai"], prefabs=["TRIGGER"]),
    P("action", "ta", "int", 0, 0, 71, flags=["ai"], prefabs=["TRIGGER"], desc="trigger action (legacy single action)"),
    P("value", "tc", "int", 0, -10000, 10000, flags=["ai"], prefabs=["TRIGGER"]),
    P("value_text", "tct", "str", "", flags=["ai"], prefabs=["TRIGGER"], maxlen=70),
    P("chance", "cha", "int", 0, 0, 100, flags=["ai"], prefabs=["TRIGGER"]),
    P("actions", "tas", "any", prefabs=["TRIGGER"], since="v7",
      desc="v7 action list: up to 5 {ta, tc, tc2, tce, tct, tcc, tad, cd, cv0, cv1, cv2}"),
    P("action_cost", "tac", "int", None, prefabs=["TRIGGER"], since="v7"),
    P("action_method", "tam", "int", None, prefabs=["TRIGGER"], since="v7"),
    P("interface_id", "in", "int", 0, 0, 1000, flags=["canInterface"], prefabs=["TRIGGER"]),
    P("target_interface", "tin", "int", 0, 0, 1000, flags=["canTargetInterface"], prefabs=["TRIGGER"]),
    P("node", "n", "int", 0, 0, 2, prefabs=["TELEPORTER"], desc="0 sending, 1 receiving, 2 both"),
    P("channel", "ch", "int", 0, 0, 100, prefabs=["TELEPORTER"]),
    P("stop_momentum", "m", "bool_present", prefabs=["TELEPORTER"]),
    P("checkpoint", "tcp", "bool_present", prefabs=["TELEPORTER"], since="v7"),
    P("dynamic", "dyn", "bool_present", flags=["canBeDynamic"]),
    P("gravity", "gy", "num", 0, 0, 5, flags=["canBeDynamic", "ai"], dec=1),
    P("poster", "bb", "int", 0, 0, 6, prefabs=["BILLBOARD"]),
    P("show_ad", "sad", "bool_present", prefabs=["BILLBOARD"]),
    P("particle_texture", "pr", "int", 0, 0, 3, prefabs=["PARTICLES"]),
    P("particle_blending", "pb", "int", 0, 0, 4, prefabs=["PARTICLES"]),
    P("particle_count", "pc", "int", 0, 0, 300, prefabs=["PARTICLES"]),
    P("particle_size", "ps", "num", 0, 0, 30, prefabs=["PARTICLES"], dec=1),
    P("particle_scale", "psc", "num", 0, -10, 10, prefabs=["PARTICLES"], dec=1),
    P("particle_speed", "psp", "num", 0, -10, 10, prefabs=["PARTICLES"], dec=1),
    P("particle_decel", "pd", "num", 0, -1, 1, prefabs=["PARTICLES"], dec=1),
    P("particle_gravity", "pg", "num", 0, -10, 10, prefabs=["PARTICLES"], dec=1),
    P("text", "st", "str", "", flags=["hasText"], maxlen=1500),
    P("text_align", "sa", "int", 0, 0, 2, flags=["hasText"]),
    P("font_size", "sf", "int", 10, 10, 120, flags=["hasText"]),
    P("font_color", "sc", "color", "#000000", flags=["hasText"]),
    P("background_color", "sb", "color", "#ffffff", flags=["hasText"]),
    P("background_visible", "bv", "bool_absent", flags=["hasText"]),
    P("competitive_spawn", "cp", "bool_present", prefabs=["SPAWN_POINT"]),
    P("wave_speed", "ws", "num", 10, 0, 25, prefabs=["LIQUID"]),
    P("wave_height", "wh", "num", 0.5, 0, 5, prefabs=["LIQUID"]),
    P("tiles_x", "tx", "int", 10, 2, 100, prefabs=["LIQUID"]),
    P("tiles_z", "tz", "int", 10, 2, 100, prefabs=["LIQUID"]),
    P("sound", "es", "int", 0, 0, 50, flags=["hasSound"], prefabs=["SOUND_EMITTER", "TRIGGER"],
      desc="emitter sound index (krunker://triggers emitter_sounds)"),
    P("asset_sound", "esa", "any", prefabs=["SOUND_EMITTER", "TRIGGER"]),
    P("volume", "ev", "num", 1, 0.05, 2, prefabs=["SOUND_EMITTER", "TRIGGER"], dec=2),
    P("rate", "er", "num", 1, 0.5, 4, prefabs=["SOUND_EMITTER", "TRIGGER"], dec=1),
    P("reference_distance", "ed", "int", 0, 0, 5000, prefabs=["SOUND_EMITTER"]),
    P("rolloff", "ero", "num", 0, 0, 1, prefabs=["SOUND_EMITTER"], dec=2),
    P("emit_from", "ef", "int", 0, 0, 1, prefabs=["TRIGGER"]),
    P("asset_id", "aid", "str", "", flags=["ai", "custom"], prefabs=["CUSTOM_ASSET"]),
    P("class_index", "ci", "int", 0, flags=["ai"], desc="AI class (ci on non-AI objects is the colour palette index)"),
    P("skill", "sk", "int", 0, flags=["ai"]),
    P("hp_multiplier", "hm", "num", 1, flags=["ai"]),
    P("can_trigger", "ct", "bool_present", flags=["ai"]),
    # v7.1.4 ObjectInstance keys (any prefab unless noted; ranges are not enforced)
    P("no_player_collision", "pl", "bool_present", since="v7"),
    P("no_bullet_collision", "ns", "bool_present", flags=["editBulletCol"], since="v7"),
    P("no_ai_collision", "nai", "bool_present", flags=["editAICol"], since="v7"),
    P("no_grapple", "gp", "bool_present", flags=["canGrapple"], since="v7"),
    P("no_impulse", "ip", "bool_present", flags=["canImpulse"], since="v7"),
    P("complex_collision", "cpx", "bool_present", flags=["canComplexColl"], since="v7"),
    P("frame_count", "fct", "int", None, flags=["frameTexture"], since="v7"),
    P("frame_speed", "fs", "num", None, flags=["frameTexture"], since="v7"),
    P("texture_encoding", "ten", "int", None, since="v7"),
    P("darken", "da", "num", None, since="v7"),
    P("force_transparent", "ft", "bool_present", since="v7"),
    P("render_side", "rs", "int", None, flags=["editRenderSide"], since="v7"),
    P("alpha_test", "alt", "num", None, since="v7"),
    P("mimic_sprite", "msp", "bool_present", since="v7"),
    P("no_cast_shadow", "cst", "bool_present", flags=["castShadow"], since="v7"),
    P("no_receive_shadow", "rcv", "bool_present", flags=["receiveShadow"], since="v7"),
    P("force_render", "fcr", "bool_present", since="v7"),
    P("hide_on_minimap", "smm", "bool_present", flags=["showMinimap"], since="v7"),
    P("step_sound", "sat", "any", flags=["stepSrc"], since="v7"),
    P("spin_axis", "spa", "int", None, flags=["hasSpin"], since="v7"),
    P("wheel_type", "sty", "int", None, prefabs=["PRIZE_WHEEL"], since="v7"),
    P("kr_minimum", "krs", "int", None, prefabs=["KR_ZONE"], since="v7"),
    P("perk", "zp", "int", None, prefabs=["PERK_MACHINE"], since="v7"),
    P("node_tag", "nt", "any", prefabs=["SERVER_NODE"], since="v7"),
    P("spawn_spread", "ssp", "num", None, prefabs=["SPAWN_POINT"], since="v7"),
    P("anchor_type", "ant", "int", None, prefabs=["MAP_ANCHOR"], since="v7"),
    P("node_id", "nid", "int", None, prefabs=["PATH_NODE"], since="v7"),
    P("camera_type", "camt", "int", None, prefabs=["CAMERA"], since="v7"),
    P("camera_force", "frc", "bool_present", prefabs=["CAMERA"], since="v7"),
    P("follow_path", "fp", "any", prefabs=["CAMERA"], since="v7"),
    P("camera_static", "cstc", "bool_present", prefabs=["CAMERA_POSITION"], since="v7"),
    P("leaderboard_type", "ldt", "any", prefabs=["LEADERBOARD"], since="v7"),
    P("leaderboard_count", "ldc", "any", prefabs=["LEADERBOARD"], since="v7"),
    P("leaderboard_order", "ldo", "any", prefabs=["LEADERBOARD"], since="v7"),
    P("leaderboard_reset", "ldr", "any", prefabs=["LEADERBOARD"], since="v7"),
    P("no_pickup_texture", "ntx", "bool_present", prefabs=["WEAPON_PICKUP"], since="v7"),
    P("no_shadow_font", "nsf", "bool_present", flags=["hasText"], since="v7"),
    P("outline_thickness", "sot", "num", None, flags=["hasText"], since="v7"),
    P("anim_speed", "as", "num", None, since="v7"),
    P("start_anim", "ast", "any", since="v7"),
    P("script_id", "sci", "any", flags=["canScript"], since="v7"),
    P("hitbox_height", "hbh", "num", None, since="v7"),
    P("hitbox_width", "hbw", "num", None, since="v7"),
    P("enclose", "ec", "bool_present", since="v7"),
    P("interact_message", "inm", "str", None, since="v7", maxlen=70),
    P("spray_texture", "spt", "any", prefabs=["SPRAY"], since="v7"),
]

# Top-level map keys (editor.js mapConfig / getMapExport). type as above plus "string",
# "array", "object", "hex_or_int" (colour as "#rrggbb" or a number).
def K(key, typ, default=None, desc="", since="v3", lo=None, hi=None, group="general"):
    d = {"key": key, "type": typ, "group": group, "since": since}
    if default is not None: d["default"] = default
    if lo is not None: d["min"] = lo
    if hi is not None: d["max"] = hi
    if desc: d["desc"] = desc
    return d

MAP_KEYS = [
    K("name", "string", "New Krunker Map", "map name (v7 publish limit 16 chars)"),
    K("modURL", "string", "", "resource pack URL"),
    K("welMsg", "string", "", "welcome message (<= 70 chars)"),
    K("ambInd", "number", 1, "ambient sound index", group="sound"),
    K("ambIndC", "number", None, since="v7", group="sound"),
    K("ambFadeTime", "number", 0, "ambient fade time in seconds (0..10; v10 IDX:20052)", since="v10", lo=0, hi=10, group="sound"),
    K("sky", "hex_or_int", "#dce8ed", "sky colour", group="sky"),
    K("skyDome", "bool", False, group="sky"),
    K("skyDomeCol0", "hex_or_int", "#74a4b9", group="sky"),
    K("skyDomeCol1", "hex_or_int", "#dce8ed", group="sky"),
    K("skyDomeCol2", "hex_or_int", "#dce8ed", group="sky"),
    K("skyDomeEmis", "hex_or_int", "#000000", group="sky"),
    K("skyDomeTex", "bool", False, group="sky"),
    K("skyDomeEmisTex", "any", None, since="v7", group="sky"),
    K("skyDomeTexA", "any", None, since="v7", group="sky"),
    K("skyDomeMovD", "number", 0, group="sky"),
    K("skyDomeMovT", "number", 0, group="sky"),
    K("fog", "hex_or_int", "#8d9aa0", "fog colour", group="fog"),
    K("fogD", "number", 2000, "fog distance", lo=0, group="fog"),
    K("ambient", "hex_or_int", "#97a0a8", "ambient light colour", group="light"),
    K("ambientI", "number", 1, "ambient intensity", lo=0, hi=5, group="light"),
    K("light", "hex_or_int", "#f2f8fc", "sun colour", group="light"),
    K("lightI", "number", 1.3, "sun intensity", lo=0, hi=5, group="light"),
    K("lightD", "number", 500, "sun distance", group="light"),
    K("lightP", "any", None, "sun position (legacy)", group="light"),
    K("sunAngX", "number", 90, "sun angle X (deg)", group="light"),
    K("sunAngY", "number", 54, "sun angle Y (deg)", group="light"),
    K("shadowR", "number", 1024, "shadow map resolution", group="light"),
    K("shadowD", "number", 1200, "shadow distance", group="light"),
    K("nAmb", "any", None, "no ambient gradient on cubes", group="light"),
    K("ambO", "number", None, "legacy ambient occlusion opacity", group="light"),
    K("ambS", "number", None, "legacy ambient occlusion size", group="light"),
    K("oldAO", "bool", None, since="v7", group="light"),
    K("correctLights", "bool", None, since="v7", group="light"),
    K("toneMapping", "number", None, since="v7", group="light"),
    K("toneMappingExposure", "number", None, since="v7", group="light"),
    K("outputEncoding", "number", None, since="v7", group="light"),
    K("dthY", "number", -100, "death height (falling below kills)", group="physics"),
    K("deathHeight", "number", None, "legacy death height", group="physics"),
    K("zone", "bool", False, "battle royale zone", group="zone"),
    K("zoneSize", "number", 500, group="zone"),
    K("zoneSpeed", "number", 1, group="zone"),
    K("zoneRnd", "bool", False, group="zone"),
    K("zoneCol0", "hex_or_int", "#c542d9", group="zone"),
    K("zoneCol1", "hex_or_int", "#c542d9", group="zone"),
    K("zoneCol2", "hex_or_int", "#c542d9", group="zone"),
    K("zoneHeight", "number", None, since="v7", group="zone"),
    K("zoneStages", "any", None, since="v7", group="zone"),
    K("zoneWait", "number", None, since="v7", group="zone"),
    K("terrainSeed", "string", "", group="terrain"),
    K("terrainTSeed", "string", "", group="terrain"),
    K("terrainWidth", "number", 3000, group="terrain"),
    K("terrainHeight", "number", 3000, group="terrain"),
    K("terrainMntMlt", "number", 1, group="terrain"),
    K("terrainMntCol", "hex_or_int", "#707070", group="terrain"),
    K("terrainSnwCol", "hex_or_int", "#e8fdff", group="terrain"),
    K("terrainGrsCol", "hex_or_int", "#5e692f", group="terrain"),
    K("terrainDrtCol", "hex_or_int", "#7f6238", group="terrain"),
    K("terrainSndCol", "hex_or_int", "#967f51", group="terrain"),
    K("terrainEase", "number", 12, group="terrain"),
    K("sizeMlt", "number", 2, group="terrain"),
    K("ocean", "bool", None, since="v7", group="ocean"),
    K("ocCol", "hex_or_int", None, since="v7", group="ocean"),
    K("ocOpac", "number", None, since="v7", group="ocean"),
    K("ocDist", "number", None, since="v7", group="ocean"),
    K("ocSpd", "number", None, since="v7", group="ocean"),
    K("ocYOff", "number", None, since="v7", group="ocean"),
    K("ocRefRes", "number", None, since="v7", group="ocean"),
    K("ocW", "number", None, since="v7", group="ocean"),
    K("ocL", "number", None, since="v7", group="ocean"),
    K("mmId", "any", None, "minimap id", since="v7"),
    K("modes", "array", None, "legacy mode list (prefer config.modes)", group="game"),
    K("shadScale", "number", None, "legacy", group="light"),
    K("shadLength", "number", None, "legacy", group="light"),
    K("shadWidth", "number", None, "legacy", group="light"),
    K("skyDomeSize", "number", None, "legacy", group="sky"),
    K("biomes", "any", None, "legacy"),
    K("creator", "string", None, "creator name"),
    K("objects", "array", None, "map objects", group="structure"),
    K("colors", "array", None, "colour palette referenced by ci / ei", group="structure"),
    K("xyz", "array", None, "pooled sizes referenced by si (v7)", group="structure"),
    K("spawns", "array", None, "[x, y, z, team, dir, comp, teamOnly?, spread?]", group="structure"),
    K("camPos", "array", None, "menu camera [x, y, z] (v7: [x,y,z,static,rx,ry,rz])", group="structure"),
    K("config", "object", None, "{modes, classes, settings, weps, cls?, prks?}", group="game"),
    K("game", "object", None, "v3 game config (camera / model / speed overrides)", group="game"),
    K("gui", "array", None, "custom GUI list (v7)", since="v7", group="structure"),
    K("scripts", "object", None, "KrunkScript {client, server} (base64 in v7 exports)", since="v7", group="structure"),
    K("botPaths", "array", None, "bot paths [{p:[[x,y,z]...], r}] (v7)", since="v7", group="structure"),
    K("tris", "array", None, "baked complex-collision triangles (v7)", since="v7", group="structure"),
]

CATEGORY_RULES = [
    ("spawns", lambda n, f: n in ("SPAWN_POINT",)),
    ("camera", lambda n, f: n in ("CAMERA_POSITION", "CAMERA", "PLACEHOLDER")),
    ("lights", lambda n, f: "LIGHT" in n or f.get("light")),
    ("zones", lambda n, f: n.endswith("_ZONE") or n in ("BOMB_SITE", "OBJECTIVE")),
    ("ai", lambda n, f: f.get("ai") or f.get("bot") or n in ("AI_REDIRECTION", "BOT_SPAWNER", "PATH_NODE")),
    ("logic", lambda n, f: n in ("TRIGGER", "TELEPORTER", "CHECK_POINT", "GATE", "DEPOSIT_BOX", "TERMINAL", "EVENT",
                                 "SOUND_EMITTER", "BOOST_PAD", "WEAPON_PICKUP", "FLAG", "MAP_ANCHOR", "SERVER_NODE",
                                 "PRIZE_WHEEL", "LEADERBOARD", "MYSTERY_BOX", "PERK_MACHINE", "KRUM_MACHINE")),
    ("effects", lambda n, f: n in ("PARTICLES", "LIQUID", "GRASS", "SPRITE", "SPRAY", "RUNE", "SIGN", "SHOWCASE")),
    ("geometry", lambda n, f: n in ("CUBE", "PLANE", "RAMP", "CYLINDER", "SPHERE", "LADDER", "ROPE_LADDER", "BILLBOARD",
                                    "CUSTOM_ASSET", "WINDOW", "DOOR")),
    ("props", lambda n, f: True),
]

V7_ONLY_FLAGS = None  # every boolean of the prefab row is kept


def c_string_concat(text):
    out = []
    for m in re.finditer(r'"((?:[^"\\]|\\.)*)"', text):
        out.append(bytes(m.group(1), "utf-8").decode("unicode_escape"))
    return "".join(out)


def read_editor_data():
    src = open(EDITOR_DATA, encoding="utf-8").read()
    rows = {}
    for m in re.finditer(r'\{"([A-Z_]+)", "([^"]*)", \{([-0-9.e]+)f, ([-0-9.e]+)f, ([-0-9.e]+)f\}, (\d+), ([-0-9.e]+)f', src):
        rows[m.group(1)] = {"label": m.group(2), "size": [float(m.group(3)), float(m.group(4)), float(m.group(5))],
                            "has_size": int(m.group(6)), "scale": float(m.group(7))}
    text_start = src.index("const char editor_data_text[] =")
    # the literal ends at the ";" on its own line (the JSON itself may hold ";", e.g. the KrunkScript templates)
    data = json.loads(c_string_concat(src[text_start:src.index("\n;\n", text_start)]))
    return rows, data


def label_of(name):
    return " ".join(w.capitalize() for w in name.split("_"))


def num(v):
    return int(v) if isinstance(v, float) and v.is_integer() else v


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--v7", default="/home/user/jails/kru/src/Krunker_v7.1.4/src")
    ap.add_argument("--out", default=OUT)
    args = ap.parse_args()

    rows, edata = read_editor_data()
    dump = json.loads(subprocess.check_output(["node", os.path.join(HERE, "dump_v7.js"), args.v7]))

    prefabs = []
    for i, pf in enumerate(dump["prefabs"]):
        name = pf.get("name")
        flags = sorted(k for k, v in pf.items() if v is True)
        v3 = None
        for alias in [name] + V3_ALIASES.get(i, []):
            v3 = v3 or rows.get(alias)
        size = pf.get("defaultSize")
        size_source = "defaultSize"
        if not size and v3:
            size, size_source = v3["size"], "model bounds (v3 editor)"
        if not size:
            s = pf.get("scale", 1)
            size, size_source = [10 * s, 10 * s, 10 * s], "estimate (model scale)"
        entry = {
            "id": i, "name": name, "label": v3["label"] if v3 and i < NATIVE_PREFABS else label_of(name),
            "aliases": V3_ALIASES.get(i, []), "default_size": [num(round(float(x), 3)) for x in size],
            "size_source": size_source, "scale": num(pf.get("scale", 1)), "flags": flags,
            "native": i < NATIVE_PREFABS, "since": "v3" if i < NATIVE_PREFABS else "v7",
        }
        if "lineCol" in pf: entry["line_color"] = "#%06x" % (int(pf["lineCol"]) & 0xffffff)
        if "src" in pf: entry["model"] = pf["src"]
        f = {k: True for k in flags}
        entry["category"] = next(c for c, rule in CATEGORY_RULES if rule(name, f))
        prefabs.append(entry)

    textures = [{"id": i, "name": t["name"], "src": t["src"], "variants": t["vars"], "since": "v3" if i < 17 else "v7"}
                for i, t in enumerate(dump["textures"])]

    # the trigger tables are v10.0.2's (editor_data: 32 events, 13 conditions, 75 actions, with the
    # entity actions 72-74 and condition 12 "Custom Action"); the v7 dump lacks them
    trig = edata.get("triggers") or {}
    events = []
    for i, ev in enumerate(trig.get("events") or [{"name": n} for n in dump["events"]]):
        e = {"id": i, "name": ev["name"], "native": i < NATIVE_EVENTS}
        if i in V3_EVENT_NAMES: e["v3_name"] = V3_EVENT_NAMES[i]
        events.append(e)
    actions = []
    for i, a in enumerate(trig.get("actions") or dump["actions"]):
        kind = "sound" if a.get("custom") else ("text" if a.get("type") == "Text" else ("both" if a.get("type") == "Both" else "number"))
        actions.append({"id": i, "name": a["name"], "value": kind, "native": i < NATIVE_ACTIONS})
    conditions = [{"id": i, "name": c["name"] if isinstance(c, dict) else c}
                  for i, c in enumerate(trig.get("conditions") or dump["conditions"])]

    limits = dict(edata.get("limits", {}))
    limits.update(dump["limits"])
    limits["publishMaxChars"] = 500000
    limits["nameMaxChars"] = 16
    limits["welcomeMaxChars"] = 70
    limits["triggerTextMax"] = 70

    schema = {
        "generated_by": "tools/editor-mcp/gen/gen_schema.py",
        "format": "Krunker map JSON (v3.1.1 native, v7.1.4 fields)",
        "native_prefab_count": NATIVE_PREFABS,
        "prefabs": prefabs,
        "textures": textures,
        "properties": PROPERTIES,
        "map_keys": MAP_KEYS,
        "triggers": {"events": events, "actions": actions, "conditions": conditions,
                     "emitter_sounds": dump.get("emitterSounds") or edata.get("emitterSounds", [])},
        "game_config": edata.get("gameConfig", []),
        # editor_data's list is v7's plus the v10 entries (tools/editor/gen_editor_data.py V10_SERVER_ADDS)
        "server_config": edata.get("serverConfig") or dump.get("serverConfig", []),
        "modes": edata.get("modes", []),
        "classes": edata.get("classes", []),
        "limits": limits,
        "defaults": edata.get("defaults", {}),
    }
    with open(args.out, "w", encoding="utf-8") as fh:
        json.dump(schema, fh, indent=1, sort_keys=False)
        fh.write("\n")
    print("wrote %s (%d prefabs, %d textures, %d properties, %d events, %d actions)" %
          (args.out, len(prefabs), len(textures), len(PROPERTIES), len(events), len(actions)))


if __name__ == "__main__":
    sys.exit(main())
