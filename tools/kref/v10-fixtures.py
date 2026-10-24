#!/usr/bin/env python3
"""Write the Krunker v10.0.3 table and config fixtures shared/tests/data/v10/*.json from the decoded bundle.

    python3 -I tools/kref/v10-fixtures.py [--deob DIR] [OUT_DIR]

DIR is the decoded v10.0.3 tree (default ~/jails/kru/src/Krunker_v10.0.3/deob: game.deob.js plus
tools/extractors/dump2.cjs, see its README.md); OUT_DIR defaults to shared/tests/data/v10. The
bundle is never executed: dump2.cjs parses game.deob.js with @babel/parser and resolves each
table's object literals, `x = {...}, ...` sequences and h(h(obj, "k", v), ...) helper chains
statically (node --max-old-space-size=8192 DIR/tools/extractors/dump2.cjs DIR/game.deob.js LINE
OUT.json, LINE = the table's opening line below).

Each fixture gets the bundle's own post-processing loop (the deob-10.0.3.js lines in TABLES), so it
holds what the game holds at run time. `Math.PI / 2`-style values (which dump2.cjs leaves unresolved)
are computed by a second static pass (PI_EXPRS_JS); other values the dumper cannot resolve (functions,
identifiers such as the `req` gates) become "<fn>" / "<expr:Type>" strings, and the dumper's "$..."
bookkeeping keys are dropped. shared/tests/weapons_v10_test.c checks KN's tables against the
output; shared/tests/data/v10/README.md lists the sources.

config.json holds the frozen config object's scalar keys (deob-10.0.3.js:22367, through
DIR/tools/extractors/config.cjs): the physics constants weapons_v10_test checks GameConstants against.

Rerun after the bundle is decoded again (deob/README.md): when a table's opening line moved, the
first-entry check below fails; find the new line with
    node DIR/tools/extractors/arrays.cjs DIR/game.deob.js <a key of the table>
and update TABLES (and the README and the citations).
"""
import json
import math
import os
import subprocess
import sys
import tempfile


def weapons_loop(items):
    # deob-10.0.3.js:115375-115396: id = index; adSpread 0, reloY 1, movSpread 1, lsMlt 0.5 when
    # null; burst false / burstC 0 / burstR 50 when burst is null
    for i, w in enumerate(items):
        w["id"] = i
        for key, default in (("adSpread", 0), ("reloY", 1), ("movSpread", 1), ("lsMlt", 0.5)):
            if w.get(key) is None:
                w[key] = default
        if w.get("burst") is None:
            w["burst"], w["burstC"], w["burstR"] = False, 0, 50


def upgrades_loop(items):
    # deob-10.0.3.js:202652-202673: id, upgraded true, sound "weapon_upgraded", then the weapons' defaults
    for i, w in enumerate(items):
        w["id"] = i
        w["upgraded"] = True
        w["sound"] = "weapon_upgraded"
    weapons_loop(items)


def index_loop(items):
    # effects deob-10.0.3.js:182169-182171, streaks deob-10.0.3.js:203521-203523: index = i
    for i, e in enumerate(items):
        e["index"] = i


# fixture name: (opening line of the array in game.deob.js, entries, first entry's name, post-processing)
# The class table is kept as literal: its classConfig knob defaults (deob-10.0.3.js:115748-115754) are
# the engine constants the C table leaves at 0, and weapons_v10_test applies the same defaults.
TABLES = {
    "weapons": (112381, 31, "Sniper Rifle", weapons_loop),        # weaponConfig, ends 115372
    "classes": (115405, 16, "Triggerman", None),                   # classConfig, ends 115747
    "effects": (182046, 10, "Boosted", index_loop),                # effectConfig, ends 182169
    "upgradeWeapons": (199770, 29, "AWP Blaster", upgrades_loop),  # upgradeWeapons, ends 202652
    "attach": (202939, 2, "Reflex Sight", None),                   # attachments, ends 202969
    "streaks": (203338, 7, "Nuke", index_loop),                    # streaks, ends 203521
    "aimRecticles": (227658, 7, "Default", None),                  # scope overlays, ends 227679
}


def clean(value):
    if isinstance(value, dict):
        if "$unresolved" in value and len(value) == 1:
            kind = value["$unresolved"]
            return "<fn>" if kind in ("FunctionExpression", "ArrowFunctionExpression") else "<expr:%s>" % kind
        return {k: clean(v) for k, v in value.items() if not k.startswith("$")}
    if isinstance(value, list):
        return [clean(v) for v in value]
    return value


def dump(deob, line, out):
    script = os.path.join(deob, "tools", "extractors", "dump2.cjs")
    subprocess.run(["node", "--max-old-space-size=8192", script, os.path.join(deob, "game.deob.js"), str(line), out],
                   check=True, stdout=subprocess.DEVNULL)
    with open(out) as f:
        return json.load(f)


# dump2.cjs leaves `Math.PI / 2` (the Bomb / Build Tool / Boulder `rot`) unresolved. This static pass (the
# same @babel/parser, nothing evaluated but the arithmetic below) lists, for the table's plain object
# entries, every property value or array element of the form `Math.PI op number`: [entry, key, index, text].
PI_EXPRS_JS = r"""
const [parserPath, file, line] = process.argv.slice(1);
const parser = require(parserPath);
const ast = parser.parse(require('fs').readFileSync(file, 'utf8'), { errorRecovery: true });
let target = null;
(function find(n) { if (!n || typeof n.type !== 'string' || target) return;
  if (n.type === 'ArrayExpression' && n.loc.start.line === +line) { target = n; return; }
  for (const k of Object.keys(n)) { if (k === 'loc') continue; const c = n[k];
    if (Array.isArray(c)) c.forEach(find); else if (c && typeof c.type === 'string') find(c); } })(ast.program);
const isPi = n => n && n.type === 'MemberExpression' && n.object.name === 'Math' && (n.property.name === 'PI' || n.property.value === 'PI');
const piExpr = n => n && n.type === 'BinaryExpression' && isPi(n.left) && n.right.type === 'NumericLiteral' && '*/'.includes(n.operator);
const out = [];
(target ? target.elements : []).forEach((e, i) => { if (!e || e.type !== 'ObjectExpression') return;
  for (const p of e.properties) { const key = p.key && (p.key.name || p.key.value); if (p.type !== 'ObjectProperty') continue;
    if (piExpr(p.value)) out.push([i, key, -1, p.value.operator, p.value.right.value]);
    if (p.value.type === 'ArrayExpression') p.value.elements.forEach((x, k) => { if (piExpr(x)) out.push([i, key, k, x.operator, x.right.value]); }); } });
console.log(JSON.stringify(out));
"""


def resolve_pi(deob, line, items):
    parser = os.path.join(deob, "tools", "node_modules", "@babel", "parser")
    found = json.loads(subprocess.run(["node", "--max-old-space-size=8192", "-e", PI_EXPRS_JS, parser,
                                       os.path.join(deob, "game.deob.js"), str(line)],
                                      check=True, capture_output=True, text=True).stdout)
    for i, key, k, op, operand in found:
        value = math.pi * operand if op == "*" else math.pi / operand
        if k < 0 and items[i].get(key) == "<expr:BinaryExpression>":
            items[i][key] = value
        elif k >= 0 and isinstance(items[i].get(key), list) and items[i][key][k] == "<expr:BinaryExpression>":
            items[i][key][k] = value


CONFIG_LINE = 22367  # the frozen config object (deob-10.0.3.js:22367-23014)


def dump_config(deob):
    """config.json: the config object's scalar keys, from DIR/tools/extractors/config.cjs (it resolves a
    `var _N = <number expression>` value; other references, arrays and objects are left out)."""
    script = os.path.join(deob, "tools", "extractors", "config.cjs")
    lines = subprocess.run(["node", "--max-old-space-size=8192", script, os.path.join(deob, "game.deob.js")],
                           check=True, capture_output=True, text=True).stdout.splitlines()
    if not lines or not lines[0].startswith("config object line %d " % CONFIG_LINE):
        sys.exit("v10-fixtures: config.cjs found %r, not the config object at deob line %d: update CONFIG_LINE" %
                 (lines[0] if lines else None, CONFIG_LINE))
    config = {}
    for row in lines[1:]:
        key, _, value = row.partition("=")
        if value.startswith("<"):
            continue
        if value in ("true", "false"):
            config[key] = value == "true"
        elif value.startswith('"'):
            config[key] = json.loads(value)
        else:
            number = float(value)
            config[key] = int(number) if number.is_integer() and "e" not in value and "." not in value else number
    return config


def main():
    args = sys.argv[1:]
    deob = os.path.expanduser("~/jails/kru/src/Krunker_v10.0.3/deob")
    if args[:1] == ["--deob"] and len(args) >= 2:
        deob, args = args[1], args[2:]
    if len(args) > 1 or (args and args[0].startswith("-")):
        sys.exit(__doc__)
    out_dir = args[0] if args else "shared/tests/data/v10"
    if not os.path.isfile(os.path.join(deob, "game.deob.js")):
        sys.exit("v10-fixtures: no game.deob.js in %s (--deob DIR)" % deob)
    os.makedirs(out_dir, exist_ok=True)
    scratch = os.path.expanduser("~/.cache/krunknative")
    os.makedirs(scratch, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=scratch, prefix="v10-fixtures-") as tmp:
        for name, (line, count, first, post) in TABLES.items():
            items = clean(dump(deob, line, os.path.join(tmp, name + ".json")))
            resolve_pi(deob, line, items)
            if len(items) != count or not items or items[0].get("name") != first:
                sys.exit("v10-fixtures: %s at deob line %d is not the %d-entry table starting with %r (got %d, %r):"
                         " the bundle was decoded again, update TABLES" %
                         (name, line, count, first, len(items), items[0].get("name") if items else None))
            if post:
                post(items)
            with open(os.path.join(out_dir, name + ".json"), "w") as f:
                json.dump(items, f, indent=1, ensure_ascii=False)
                f.write("\n")
            print("v10-fixtures: %s.json, %d entries (deob-10.0.3.js:%d)" % (name, len(items), line))
    config = dump_config(deob)
    with open(os.path.join(out_dir, "config.json"), "w") as f:
        json.dump(config, f, indent=1, ensure_ascii=False, sort_keys=True)
        f.write("\n")
    print("v10-fixtures: config.json, %d keys (deob-10.0.3.js:%d)" % (len(config), CONFIG_LINE))


if __name__ == "__main__":
    main()
