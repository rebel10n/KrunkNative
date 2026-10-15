#!/usr/bin/env python3
"""Krunker Strike rules generator: tools/strike/strike_data.json -> shared/src/rules/strike/strike_data.c

strike_data.json is extract_strike.py's text-only extraction of FRVR's Krunker Strike client bundle
(build 1.37.2). This script turns the parts the engine plays by into the C tables behind
strike_rules() (shared/include/strike_rules.h), so a new Strike build is re-extracted and
regenerated, never hand-edited:

  classes   the six Strike classes, every stat (classConfig defaults applied: the "effective" row)
  weapons   per weapon, the stats where Strike's entry differs from Krunker v10.0.2's
            (shared/tests/data/v10/weapons.json, what the v10 table is checked against), in
            Krunker's JS units; a key Strike's entry leaves out takes the engine default
  modes     Strike's 35 modes (v7's table without Trade / Impulsed) with their limits; "offered"
            = the modes Strike's menu describes (descriptionLang present in Strike's i18n)
  config    Strike's serverConfig defaults (custom game / lobby settings)
  limits    player cap, private-match capacity, bots, tick rates, timers

Run from anywhere with `python3 -I tools/strike/gen_rules.py` (writes the file) or `--check`
(exit 1 when the checked-in file is stale). Stdlib only; reads JSON, executes nothing.
"""
import json
import math
import re
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DATA = os.path.join(ROOT, "tools", "strike", "strike_data.json")
V10_WEAPONS = os.path.join(ROOT, "shared", "tests", "data", "v10", "weapons.json")
OUT = os.path.join(ROOT, "shared", "src", "rules", "strike", "strike_data.c")

# The value a weaponConfig key reads as when an entry leaves it out (Krunker's weapon code:
# `lsMlt || .5`, `movSpread || 1`, `chrgMlt ?? .9`; booleans are false, the rest 0).
WEAPON_KEY_DEFAULTS = {"lsMlt": 0.5, "movSpread": 1.0, "chrgMlt": 0.9, "burstR": 50.0, "reloY": 1.0}

# Keys whose differences the engine has no field for (cosmetic / inventory). They are listed in the
# generated file and in strike_rules_test, never silently dropped.
WEAPON_KEYS_NOT_PORTED = {
    "pj_color": "projectile tint: a client render table (client/src/projectiles_render.c), not a Weapon field",
    "noSkins": "inventory: whether the weapon takes skins (KN's market, not gameplay)",
    "reloY": "viewmodel reload drop (KN animates reloads from the v7 constants)",
}
# Non-scalar keys: compared, but only reported.
WEAPON_NONSCALAR_NOTES = {
    "cSpread": "Strike keeps one flat pattern of 20 numbers where v10 nests two of 10; the (x, y) pairs "
               "are identical, so KN's flattened custom_spread is unchanged",
    "recoilData": "v10's Assault Rifle recoil pattern; KN does not implement recoilData, so dropping it "
                  "changes nothing",
}


def die(msg):
    sys.stderr.write("gen_rules: %s\n" % msg)
    sys.exit(2)


def is_scalar(v):
    return isinstance(v, bool) or (isinstance(v, (int, float)) and not isinstance(v, bool) and math.isfinite(v))


def num(v):
    return float(v) if not isinstance(v, bool) else (1.0 if v else 0.0)


def c_str(s):
    if s is None:
        return "NULL"
    out = []
    for ch in s:
        if ch == "\\":
            out.append("\\\\")
        elif ch == '"':
            out.append('\\"')
        elif ch == "\n":
            out.append("\\n")
        elif 32 <= ord(ch) < 127:
            out.append(ch)
        else:
            out.extend("\\%03o" % b for b in ch.encode("utf-8"))
    return '"' + "".join(out) + '"'


def c_num(v):
    v = float(v)
    if v == int(v) and abs(v) < 1e15:
        return "%d" % int(v)
    return repr(v)


def c_float(v):
    s = repr(float(v))
    if "e" not in s and "." not in s:
        s += ".0"
    return s + "f"


def norm(v):
    """v10 fixtures keep a few unevaluated expressions ("<expr:Math['PI'] / 2>"): evaluate the
    `Math.PI op number` form, compare numbers with a tolerance; anything else stays as is."""
    if isinstance(v, dict):
        return {k: norm(x) for k, x in v.items()}
    if isinstance(v, list):
        return [norm(x) for x in v]
    if isinstance(v, str):
        m = re.fullmatch(r"<expr:Math\['PI'\]\s*([*/])\s*([\d.]+)>", v)
        if m:
            return math.pi * float(m.group(2)) if m.group(1) == "*" else math.pi / float(m.group(2))
    return v


def same(a, b):
    a, b = norm(a), norm(b)
    if is_scalar(a) and is_scalar(b):
        return abs(num(a) - num(b)) <= 1e-9 * max(1.0, abs(num(b)))
    if isinstance(a, dict) and isinstance(b, dict):
        return set(a) == set(b) and all(same(a[k], b[k]) for k in a)
    if isinstance(a, list) and isinstance(b, list):
        return len(a) == len(b) and all(same(x, y) for x, y in zip(a, b))
    return a == b


def weapon_overrides(strike, v10):
    """[(key, js value)] where Strike's weapon differs from v10's, plus report notes."""
    over, notes, skipped = [], [], []
    for key in sorted(set(strike) | set(v10)):
        sv, vv = strike.get(key), v10.get(key)
        if key in strike and key in v10:
            if same(sv, vv):
                continue
            if is_scalar(sv) and is_scalar(vv):
                (skipped if key in WEAPON_KEYS_NOT_PORTED else over).append((key, num(sv)))
            elif key == "cSpread" and same(flat(sv), flat(vv)):
                notes.append(key)
            else:
                notes.append(key)
        elif key in v10:  # Strike leaves it out: the engine default
            if not is_scalar(vv):
                if key in WEAPON_NONSCALAR_NOTES:
                    notes.append(key)
                continue
            if isinstance(vv, bool):
                default = 0.0
            elif key in WEAPON_KEY_DEFAULTS:
                default = WEAPON_KEY_DEFAULTS[key]
            elif key in ("id",):
                continue
            else:
                default = 0.0
            if abs(num(vv) - default) > 1e-9:
                (skipped if key in WEAPON_KEYS_NOT_PORTED else over).append((key, default))
        # a key only Strike has: a scalar is a difference against the default
        elif is_scalar(sv):
            default = WEAPON_KEY_DEFAULTS.get(key, 0.0)
            if abs(num(sv) - default) > 1e-9:
                (skipped if key in WEAPON_KEYS_NOT_PORTED else over).append((key, num(sv)))
    return over, notes, skipped


def flat(v):
    if isinstance(v, list):
        out = []
        for x in v:
            out.extend(flat(x))
        return out
    return [v]


def main():
    check = "--check" in sys.argv[1:]
    d = json.load(open(DATA, encoding="utf-8"))
    v10_weapons = json.load(open(V10_WEAPONS, encoding="utf-8"))
    meta = d["meta"]
    lines = []
    w = lines.append

    w("// GENERATED by tools/strike/gen_rules.py from tools/strike/strike_data.json: do not edit.")
    w("// Regenerate after re-extracting a new Strike build: python3 -I tools/strike/gen_rules.py")
    w("// Source: FRVR Krunker Strike %s (build %s), %s." % (meta["game_version"], meta["build_version"], meta["primary"]))
    w("// The tables strike_rules() returns (shared/include/strike_rules.h); strike_rules.c builds the")
    w("// engine's weapon / class / mode tables from them, shared/tests/strike_rules_test.c checks them")
    w("// against strike_data.json.")
    w("#include <strike_rules.h>")
    w("#include <modes.h>")
    w("")

    # ---- classes
    classes = d["classes"]
    w("// ---- classes: classConfig as Strike ships it (ids keep Krunker's numbering; 3, 6, 8+ are absent)")
    for c in classes:
        txts = c["effective"].get("txts") or []
        if txts:
            w("static const char *const k_txts_%d[] = {%s};" % (c["id"], ", ".join(c_str(t) for t in txts)))
    w("static const StrikeClass k_classes[] = {")
    for c in classes:
        e = c["effective"]
        loadout = e.get("loadout") or [1]
        colors = (e.get("colors") or [0] * 6)[:6]
        txts = e.get("txts") or []
        w("    {.id = %d, .name = %s, .loadout = %d," % (c["id"], c_str(e["name"]), loadout[0]))
        w("     .health = %d, .segments = %d, .regen = %s, .speed = %s," % (
            e["health"], e.get("segs", 0), c_float(e["regen"]), c_float(e["speed"])))
        w("     .secondary = %d, .wall_jump = %d, .wall_jump_max = %d, .leap = %d, .back_slide = %d," % (
            bool(e.get("secondary")), bool(e.get("wallJ")), int(e.get("wallJM") or 0), bool(e.get("leap")), bool(e.get("bkSlid"))))
        w("     .melee_rate = %s, .slide_time = %s, .slide_accel = %s, .turn_speed = %s, .throw_charge_ms = %d," % (
            c_float(e.get("melRate", 1)), c_float(e.get("slT", 1)), c_float(e.get("slA", 1)), c_float(e.get("trnSpd", 1)),
            int(e.get("throwChrg", 0))))
        w("     .ground_accel = %s, .air_accel = %s, .ground_decel = %s, .air_decel = %s, .crouch_speed = %s," % (
            c_float(e["grA"]), c_float(e["arA"]), c_float(e["grDec"]), c_float(e["airDec"]), c_float(e["crchSpd"])))
        w("     .rarity = %d, .min_rec = %d, .unlockable = %s," % (
            int(e.get("rarity") or 0), int(e.get("minRec") or 0), c_str(e.get("unlockable"))))
        w("     .colors = {%s}," % ", ".join("0x%06X" % x for x in colors))
        w("     .texts = %s, .text_count = %d," % (("k_txts_%d" % c["id"]) if txts else "NULL", len(txts)))
        w("     .description = %s}," % c_str(e.get("description")))
    w("};")
    w("")

    # ---- weapons
    w("// ---- weapons: where each Strike weapon differs from Krunker v10.0.2's (JS units: ms, recoil per ms)")
    report = {}
    weapon_rows = []
    if len(d["weapons"]) > len(v10_weapons):
        die("Strike has %d weapons, v10 %d" % (len(d["weapons"]), len(v10_weapons)))
    for wpn in d["weapons"]:
        i = wpn["index"]
        v10 = v10_weapons[i]
        if v10.get("name") != wpn["name"]:
            die("weapon %d is %s in Strike, %s in v10" % (i, wpn["name"], v10.get("name")))
        over, notes, skipped = weapon_overrides(wpn["data"], v10)
        if over:
            w("static const StrikeStat k_wep_%d[] = { // %s" % (i, wpn["name"]))
            for key, value in over:
                vv = v10.get(key)
                w("    {%s, %s}, // v10 %s" % (c_str(key), c_num(value), "absent" if vv is None else json.dumps(vv)))
            w("};")
        weapon_rows.append((i, wpn["name"], len(over)))
        for key in notes:
            report.setdefault((key, WEAPON_NONSCALAR_NOTES.get(key, "differs (non-scalar), not applied")), []).append(wpn["name"])
        for key, value in skipped:
            report.setdefault((key, "not ported: " + WEAPON_KEYS_NOT_PORTED[key]), []).append("%s %s" % (wpn["name"], c_num(value)))
    w("static const StrikeWeapon k_weapons[] = {")
    for i, name, n in weapon_rows:
        w("    {.id = %d, .name = %s, .overrides = %s, .override_count = %d}," % (i, c_str(name), ("k_wep_%d" % i) if n else "NULL", n))
    w("};")
    if report:
        w("// Differences not applied (non-scalar, or no engine field):")
        for (key, why), where in sorted(report.items()):
            w("//   %s (%s): %s" % (key, ", ".join(where), why))
    w("")

    # ---- modes
    i18n = d.get("i18n") or {}
    modes = d["modes"]["list"]
    w("// ---- modes: Strike's gameModes table; offered = described in Strike's menu (i18n descriptionLang)")
    for m in modes:
        order = m["data"].get("weaponOrder")
        if order:
            w("static const int k_order_%s[] = {%s};" % (m["id"], ", ".join(str(x) for x in order)))
    w("static const StrikeMode k_modes[] = {")
    offered = []
    for m in modes:
        dd = m["data"]
        desc = dd.get("descriptionLang")
        is_offered = bool(desc and desc in i18n)
        if is_offered:
            offered.append(m)
        gt = dd.get("gameTime")
        order = dd.get("weaponOrder")
        w("    {.index = %d, .mode_id = MODE_ID_%s, .alias = %s, .name = %s, .teams = %d, .party = %d, .offered = %d," % (
            m["index"], m["id"].upper(), c_str(m["id"]), c_str(m["name"]), bool(dd.get("teams")), bool(dd.get("party")), is_offered))
        w("     .prevent_custom = %d, .score_limit = %d, .game_time_ms = %d, .lives = %d, .weapon_order = %s, .weapon_order_count = %d}," % (
            bool(dd.get("preventCustomGames")), int(dd.get("scoreLimit") or 0), -1 if gt is None else int(gt), int(dd.get("lives") or 0),
            ("k_order_%s" % m["id"]) if order else "NULL", len(order or [])))
    w("};")
    w("static const int k_rotation[] = {%s};" % ", ".join("MODE_ID_%s" % m["id"].upper() for m in offered))
    w("")

    # ---- serverConfig defaults
    w("// ---- serverConfig defaults (numbers and booleans; Strike's custom-game / lobby defaults)")
    w("static const StrikeConfigDefault k_config[] = {")
    for e in d["config"]["serverConfig"]:
        v, df = e.get("varN"), e.get("def")
        if v and is_scalar(df):
            w("    {%s, %s}," % (c_str(v), c_num(num(df))))
    w("};")
    w("")

    # ---- limits
    k = d["config"]["constants"]
    cap = d["config"]["player_cap"]
    timing = d["config"]["timing"]

    def val(x):
        return x["value"] if isinstance(x, dict) else x

    w("#define COUNT(a) (sizeof(a) / sizeof((a)[0]))")
    w("const StrikeRules g_strike_rules = {")
    w("    .game_version = %s," % c_str(meta["game_version"]))
    w("    .build_version = %s," % c_str(meta["build_version"]))
    w("    .classes = k_classes, .class_count = COUNT(k_classes), .default_class = %d," % classes[0]["id"])
    w("    .weapons = k_weapons, .weapon_count = COUNT(k_weapons),")
    w("    .modes = k_modes, .mode_count = COUNT(k_modes),")
    w("    .rotation_modes = k_rotation, .rotation_mode_count = COUNT(k_rotation),")
    w("    .config_defaults = k_config, .config_default_count = COUNT(k_config),")
    w("    .max_players = %d,        // constants.maxPlayers: Number(MAX_PLAYERS) || 10" % val(k["maxPlayers"]))
    w("    .private_capacity = %d,   // director allocate/private body capacity" % cap["private_match_capacity"])
    w("    .max_bots = %d," % val(k["maxBots"]))
    w("    .max_players_total = %d,  // gamesPerServer x maxPlayers + 40" % val(k["maxPlayersTotal"]))
    w("    .games_per_server = %d," % val(k["gamesPerServer"]))
    w("    .server_tick_ms = %s,     // 1e3 / serverTickFrequency" % c_float(val(k["serverTickRate"])))
    w("    .server_send_frequency = %d," % val(k["serverSendFrequency"]))
    w("    .game_start_delay_ms = %d," % val(k["gameStartDelay"]))
    w("    .kick_timer_ms = %d," % val(k["kickTimer"]))
    w("    .game_time_min = %d,      // serverConfig gameTime def (minutes)" % timing["gameTime"]["def"])
    w("    .intermission_s = %d," % timing["intermTmr"]["def"])
    w("};")

    text = "\n".join(lines) + "\n"
    if check:
        try:
            old = open(OUT, encoding="utf-8").read()
        except OSError:
            old = ""
        if old != text:
            sys.stderr.write("gen_rules: %s is stale; run python3 -I tools/strike/gen_rules.py\n" % os.path.relpath(OUT, ROOT))
            return 1
        print("gen_rules: %s is up to date" % os.path.relpath(OUT, ROOT))
        return 0
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    print("gen_rules: wrote %s (%d classes, %d weapons, %d modes, %d offered)" % (
        os.path.relpath(OUT, ROOT), len(classes), len(weapon_rows), len(modes), len(offered)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
