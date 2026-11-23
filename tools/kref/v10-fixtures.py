#!/usr/bin/env python3
"""Write the Krunker v10.0.7 table and config fixtures shared/tests/data/v10/*.json from the decoded bundle.

    python3 -I tools/kref/v10-fixtures.py [--deob DIR] [--tools TOOLS] [OUT_DIR]

DIR is the decoded v10.0.7 tree (default ~/jails/kru/src/Krunker_v10.0.7/deob: game.deob.js). TOOLS is the
extractor tree (default DIR/tools when it exists, else ~/jails/kru/src/Krunker_v10.0.3/deob/tools, see its
README.md): its node_modules/@babel/parser and extractors/config.cjs. OUT_DIR defaults to shared/tests/data/v10.
The bundle is never executed: DUMP_JS below (TOOLS/extractors/dump2.cjs with a stricter array match) parses
game.deob.js with @babel/parser and resolves each table's object literals, `x = {...}, ...` sequences and
h(h(obj, "k", v), ...) helper chains statically, taking the array of the table's entry count that opens on
the table's line below.

Each fixture gets the bundle's own post-processing loop (the deob-10.0.7.js lines in TABLES), so it
holds what the game holds at run time. `Math.PI / 2`-style values (which the dumper leaves unresolved)
are computed by a second static pass (PI_EXPRS_JS); other values the dumper cannot resolve (functions,
identifiers such as the `req` gates) become "<fn>" / "<expr:Type>" strings, and the dumper's "$..."
bookkeeping keys are dropped. shared/tests/weapons_v10_test.c checks KN's tables against the
output; shared/tests/data/v10/README.md lists the sources.

modes.json is the game-mode list (MODES_JS below); config.json holds the frozen config object's scalar keys (deob-10.0.7.js:19503, through
TOOLS/extractors/config.cjs): the physics constants weapons_v10_test checks GameConstants against.

Rerun after the bundle is decoded again (deob/README.md): when a table's opening line moved, the
first-entry check below fails; find the new line with
    node TOOLS/extractors/arrays.cjs DIR/game.deob.js <a key of the table>
and update TABLES (and the README and the citations).
"""
import json
import math
import os
import re
import subprocess
import sys
import tempfile


def weapons_loop(items):
    # deob-10.0.7.js:102626-102634: id = index; adSpread 0, reloY 1, movSpread 1, lsMlt 0.5 when
    # null; burst false / burstC 0 / burstR 50 when burst is null
    for i, w in enumerate(items):
        w["id"] = i
        for key, default in (("adSpread", 0), ("reloY", 1), ("movSpread", 1), ("lsMlt", 0.5)):
            if w.get(key) is None:
                w[key] = default
        if w.get("burst") is None:
            w["burst"], w["burstC"], w["burstR"] = False, 0, 50


def upgrades_loop(items):
    # deob-10.0.7.js:180402-180411: id, upgraded true, sound "weapon_upgraded", then the weapons' defaults
    for i, w in enumerate(items):
        w["id"] = i
        w["upgraded"] = True
        w["sound"] = "weapon_upgraded"
    weapons_loop(items)


def index_loop(items):
    # effects deob-10.0.7.js:164662-164664, streaks deob-10.0.7.js:181128-181130: index = i
    for i, e in enumerate(items):
        e["index"] = i


# fixture name: (opening line of the array in game.deob.js, entries, first entry's name, post-processing)
# The class table is kept as literal: its classConfig knob defaults (deob-10.0.7.js:102981-102985) are
# the engine constants the C table leaves at 0, and weapons_v10_test applies the same defaults.
TABLES = {
    "weapons": (99659, 31, "Sniper Rifle", weapons_loop),         # weaponConfig, ends 102622
    "classes": (102638, 16, "Triggerman", None),                   # classConfig, ends 102980
    "effects": (164539, 10, "Boosted", index_loop),                # effectConfig, ends 164662
    "upgradeWeapons": (177550, 29, "AWP Blaster", upgrades_loop),  # upgradeWeapons, ends 180402
    "attach": (180663, 2, "Reflex Sight", None),                   # attachments, ends 180693
    "streaks": (180987, 7, "Nuke", index_loop),                    # streaks, ends 181128
    "aimRecticles": (202747, 7, "Default", None),                  # scope overlays, ends 202768
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


# The table dumper: TOOLS/extractors/dump2.cjs (object literals, `x = {...}, ...` sequences and
# h(h(obj, "k", v), ...) helper chains, anything else {$unresolved}), except that it takes the first array
# opening on LINE that has COUNT elements: in 10.0.7 the upgradeWeapons array shares its opening line with
# the country list (deob-10.0.7.js:177550), which dump2.cjs (first array on the line) picks instead.
DUMP_JS = r"""
const [parserPath, file, line, count, outFile] = process.argv.slice(1);
const fs = require('fs');
const parser = require(parserPath);
const ast = parser.parse(fs.readFileSync(file, 'utf8'), { errorRecovery: true });
let target = null;
(function find(n) { if (!n || typeof n.type !== 'string' || target) return;
  if (n.type === 'ArrayExpression' && n.loc.start.line === +line && n.elements.length === +count) { target = n; return; }
  for (const k of Object.keys(n)) { if (k === 'loc') continue; const c = n[k];
    if (Array.isArray(c)) c.forEach(find); else if (c && typeof c.type === 'string') find(c); } })(ast.program);
const keyName = p => p.key && (p.key.name || p.key.value);
function lit(n) {
  if (!n) return { $unresolved: 'null' };
  switch (n.type) {
    case 'NumericLiteral': case 'StringLiteral': case 'BooleanLiteral': return n.value;
    case 'NullLiteral': return null;
    case 'UnaryExpression':
      if (n.operator === '-' && n.argument.type === 'NumericLiteral') return -n.argument.value;
      if (n.operator === '!' && n.argument.type === 'NumericLiteral') return !n.argument.value;
      return { $unresolved: 'unary' + n.operator };
    case 'ArrayExpression': return n.elements.map(lit);
    case 'ObjectExpression': return resolveObj(n, {});
    default: return { $unresolved: n.type };
  }
}
function resolveObj(n, o) {
  if (n.type === 'ObjectExpression') {
    for (const p of n.properties) { const k = keyName(p); if (k !== undefined && p.type === 'ObjectProperty') o[k] = lit(p.value); }
    return o;
  }
  if (n.type === 'SequenceExpression') {
    let first = true;
    for (const e of n.expressions) {
      if (first) { first = false; if (e.type === 'AssignmentExpression') resolveObj(e.right, o); else resolveObj(e, o); continue; }
      applyStep(e, o);
    }
    return o;
  }
  if (n.type === 'AssignmentExpression') return resolveObj(n.right, o);
  if (n.type === 'CallExpression') { resolveObj(n.arguments[0], o); applyStep(n, o); return o; }
  o.$unresolved = n.type; return o;
}
function applyStep(e, o) {
  if (e.type === 'AssignmentExpression' && e.left.type === 'MemberExpression') {
    o[e.left.property.name || e.left.property.value] = lit(e.right); return;
  }
  if (e.type === 'CallExpression') {
    const args = e.arguments;
    if (args.length >= 1 && (args[0].type === 'CallExpression' || args[0].type === 'SequenceExpression')) resolveObj(args[0], o);
    if (args.length === 3 && args[1].type === 'StringLiteral') o[args[1].value] = lit(args[2]);
    else if (args.length !== 1) o.$unresolved_step = e.type;
    return;
  }
  o.$unresolved_step = e.type;
}
if (!target) { console.error('no ' + count + '-element array opens on line ' + line); process.exit(1); }
fs.writeFileSync(outFile, JSON.stringify(target.elements.map(e => e ? resolveObj(e, {}) : null), null, 1));
"""


def dump(deob, tools, line, count, out):
    parser = os.path.join(tools, "node_modules", "@babel", "parser")
    subprocess.run(["node", "--max-old-space-size=8192", "-e", DUMP_JS, parser, os.path.join(deob, "game.deob.js"),
                    str(line), str(count), out], check=True, stdout=subprocess.DEVNULL)
    with open(out) as f:
        return json.load(f)


# The dumper leaves `Math.PI / 2` (the Bomb / Build Tool / Boulder `rot`) unresolved. This static pass (the
# same @babel/parser, nothing evaluated but the arithmetic below) lists, for the table's plain object
# entries, every property value or array element of the form `Math.PI op number`: [entry, key, index, text].
PI_EXPRS_JS = r"""
const [parserPath, file, line, count] = process.argv.slice(1);
const parser = require(parserPath);
const ast = parser.parse(require('fs').readFileSync(file, 'utf8'), { errorRecovery: true });
let target = null;
(function find(n) { if (!n || typeof n.type !== 'string' || target) return;
  if (n.type === 'ArrayExpression' && n.loc.start.line === +line && n.elements.length === +count) { target = n; return; }
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


def resolve_pi(deob, tools, line, count, items):
    parser = os.path.join(tools, "node_modules", "@babel", "parser")
    found = json.loads(subprocess.run(["node", "--max-old-space-size=8192", "-e", PI_EXPRS_JS, parser,
                                       os.path.join(deob, "game.deob.js"), str(line), str(count)],
                                      check=True, capture_output=True, text=True).stdout)
    for i, key, k, op, operand in found:
        value = math.pi * operand if op == "*" else math.pi / operand
        if k < 0 and items[i].get(key) == "<expr:BinaryExpression>":
            items[i][key] = value
        elif k >= 0 and isinstance(items[i].get(key), list) and items[i][key][k] == "<expr:BinaryExpression>":
            items[i][key][k] = value


CONFIG_LINE, CONFIG_END = 19503, 20147  # the frozen config object (deob-10.0.7.js:19503-20147)


def dump_config(deob, tools):
    """config.json: the config object's scalar keys, from TOOLS/extractors/config.cjs (it resolves a
    `var _N = <number expression>` value; other references, arrays and objects are left out). The 10.0.7
    decode writes booleans as `!0x0` / `!0x1`, which config.cjs reports as <UnaryExpression>: those keys are
    read from the object's own `'key': !0xN,` line (a top-level property, 4-space indent)."""
    script = os.path.join(tools, "extractors", "config.cjs")
    lines = subprocess.run(["node", "--max-old-space-size=8192", script, os.path.join(deob, "game.deob.js")],
                           check=True, capture_output=True, text=True).stdout.splitlines()
    if not lines or not lines[0].startswith("config object line %d " % CONFIG_LINE):
        sys.exit("v10-fixtures: config.cjs found %r, not the config object at deob line %d: update CONFIG_LINE" %
                 (lines[0] if lines else None, CONFIG_LINE))
    with open(os.path.join(deob, "game.deob.js")) as f:
        span = [next(f) for _ in range(CONFIG_END)][CONFIG_LINE - 1:]
    bools = {}
    for text in span:
        m = re.match(r"^    '([A-Za-z0-9_]+)': !0x([01]),?$", text.rstrip("\n"))
        if m:
            bools[m.group(1)] = m.group(2) == "0"
    config = {}
    for row in lines[1:]:
        key, _, value = row.partition("=")
        if value == "<UnaryExpression>" and key in bools:
            config[key] = bools[key]
            continue
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

# The game-mode list (deob-10.0.7.js:106124-106681, 38 entries, id order). Half its entries are identifiers: a
# mode declared earlier as `var m = {...}` or as a helper chain h(h(m = {...}, "k", v), ...), often continued by
# further standalone chains on the same variable (Zombies: :105887-105989, Candy Heist :106098). MODES_JS
# resolves an identifier to its declaration and then applies every h(<chain on that variable>, "key", value)
# call in the bundle (object literals, chains and `"key": function` as "<fn>"). Nothing is executed. The list has
# no post-processing loop.
MODES_LINE, MODES_COUNT, MODES_FIRST = 106124, 38, "ffa"
MODES_JS = r"""
const [parserPath, file, line, count, outFile] = process.argv.slice(1);
const fs = require('fs');
const parser = require(parserPath);
const ast = parser.parse(fs.readFileSync(file, 'utf8'), { errorRecovery: true });
const decl = new Map(), chains = new Map();
const keyName = p => p.key && (p.key.name || p.key.value);
function baseOf(n) {
  while (n) {
    if (n.type === 'Identifier') return n.name;
    if (n.type === 'AssignmentExpression' && n.left.type === 'Identifier') return n.left.name;
    if (n.type === 'CallExpression' && n.callee.type === 'Identifier') { n = n.arguments[0]; continue; }
    return null;
  }
  return null;
}
let target = null;
(function walk(n) {
  if (!n || typeof n.type !== 'string') return;
  if (n.type === 'VariableDeclarator' && n.id.type === 'Identifier' && n.init) decl.set(n.id.name, n.init);
  if (n.type === 'AssignmentExpression' && n.left.type === 'Identifier' && n.operator === '=' &&
      (!decl.has(n.left.name) || n.right.type === 'ObjectExpression')) decl.set(n.left.name, n.right);
  if (n.type === 'CallExpression' && n.callee.type === 'Identifier' && n.arguments.length === 3 && n.arguments[1].type === 'StringLiteral') {
    const b = baseOf(n.arguments[0]);
    if (b) { if (!chains.has(b)) chains.set(b, []); chains.get(b).push([n.arguments[1].value, n.arguments[2]]); }
  }
  if (n.type === 'ArrayExpression' && n.loc.start.line === +line && n.elements.length === +count && !target) target = n;
  for (const k of Object.keys(n)) { if (k === 'loc') continue; const c = n[k];
    if (Array.isArray(c)) c.forEach(walk); else if (c && typeof c.type === 'string') walk(c); }
})(ast.program);
function lit(n) {
  switch (n.type) {
    case 'NumericLiteral': case 'StringLiteral': case 'BooleanLiteral': return n.value;
    case 'NullLiteral': return null;
    case 'UnaryExpression':
      if (n.operator === '-' && n.argument.type === 'NumericLiteral') return -n.argument.value;
      if (n.operator === '!' && n.argument.type === 'NumericLiteral') return !n.argument.value;
      return '<expr:unary' + n.operator + '>';
    case 'ArrayExpression': return n.elements.map(lit);
    case 'ObjectExpression': return resolveObj(n, {});
    case 'FunctionExpression': case 'ArrowFunctionExpression': return '<fn>';
    default: return '<expr:' + n.type + '>';
  }
}
function resolveObj(n, o) {
  if (n.type === 'ObjectExpression') {
    for (const p of n.properties) { const k = keyName(p); if (k !== undefined) o[k] = p.type === 'ObjectMethod' ? '<fn>' : lit(p.value); }
    return o;
  }
  if (n.type === 'AssignmentExpression') return resolveObj(n.right, o);
  if (n.type === 'CallExpression') {
    resolveObj(n.arguments[0], o);
    if (n.arguments.length === 3 && n.arguments[1].type === 'StringLiteral') o[n.arguments[1].value] = lit(n.arguments[2]);
    return o;
  }
  if (n.type === 'Identifier' && decl.has(n.name)) return resolveObj(decl.get(n.name), o);
  o['<unresolved>'] = n.type;
  return o;
}
if (!target) { console.error('no ' + count + '-element array opens on line ' + line); process.exit(1); }
fs.writeFileSync(outFile, JSON.stringify(target.elements.map(e => {
  const o = resolveObj(e, {});
  if (e.type === 'Identifier') {
    const d = decl.get(e.name), b = d ? baseOf(d) : null;
    for (const name of [b, e.name]) for (const [k, v] of (name && chains.get(name)) || []) if (!(k in o)) o[k] = lit(v);
  }
  return o;
}), null, 1));
"""


def dump_modes(deob, tools, out):
    parser = os.path.join(tools, "node_modules", "@babel", "parser")
    subprocess.run(["node", "--max-old-space-size=8192", "-e", MODES_JS, parser, os.path.join(deob, "game.deob.js"),
                    str(MODES_LINE), str(MODES_COUNT), out], check=True, stdout=subprocess.DEVNULL)
    with open(out) as f:
        modes = json.load(f)
    if len(modes) != MODES_COUNT or modes[0].get("id") != MODES_FIRST:
        sys.exit("v10-fixtures: the mode list at deob line %d is not the %d-entry list starting with %r: update MODES_LINE" %
                 (MODES_LINE, MODES_COUNT, MODES_FIRST))
    return modes


def main():
    args = sys.argv[1:]
    deob = os.path.expanduser("~/jails/kru/src/Krunker_v10.0.7/deob")
    tools = None
    while args[:1] in (["--deob"], ["--tools"]) and len(args) >= 2:
        if args[0] == "--deob":
            deob = args[1]
        else:
            tools = args[1]
        args = args[2:]
    if tools is None:
        tools = os.path.join(deob, "tools")
        if not os.path.isdir(tools):
            tools = os.path.expanduser("~/jails/kru/src/Krunker_v10.0.3/deob/tools")
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
            items = clean(dump(deob, tools, line, count, os.path.join(tmp, name + ".json")))
            resolve_pi(deob, tools, line, count, items)
            if len(items) != count or not items or items[0].get("name") != first:
                sys.exit("v10-fixtures: %s at deob line %d is not the %d-entry table starting with %r (got %d, %r):"
                         " the bundle was decoded again, update TABLES" %
                         (name, line, count, first, len(items), items[0].get("name") if items else None))
            if post:
                post(items)
            with open(os.path.join(out_dir, name + ".json"), "w") as f:
                json.dump(items, f, indent=1, ensure_ascii=False)
                f.write("\n")
            print("v10-fixtures: %s.json, %d entries (deob-10.0.7.js:%d)" % (name, len(items), line))
        modes = dump_modes(deob, tools, os.path.join(tmp, "modes.json"))
        with open(os.path.join(out_dir, "modes.json"), "w") as f:
            json.dump(modes, f, indent=1, ensure_ascii=False)
            f.write("\n")
        print("v10-fixtures: modes.json, %d entries (deob-10.0.7.js:%d)" % (len(modes), MODES_LINE))
    config = dump_config(deob, tools)
    with open(os.path.join(out_dir, "config.json"), "w") as f:
        json.dump(config, f, indent=1, ensure_ascii=False, sort_keys=True)
        f.write("\n")
    print("v10-fixtures: config.json, %d keys (deob-10.0.7.js:%d)" % (len(config), CONFIG_LINE))


if __name__ == "__main__":
    main()
