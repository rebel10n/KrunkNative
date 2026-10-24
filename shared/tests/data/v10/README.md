# Krunker v10.0.3 table fixtures

What `shared/tests/weapons_v10_test.c` checks KrunkNative's tables against. Each file is one table of the
decoded v10.0.3 bundle (`~/jails/kru/src/Krunker_v10.0.3/deob/game.deob.js`, cited as `deob-10.0.3.js:<line>`),
read statically (never executed) and given the bundle's own post-processing, so it holds what the game holds
at run time.

| File | Table | deob-10.0.3.js | Post-processing applied |
|---|---|---|---|
| `weapons.json` | weaponConfig, 31 entries | 112381-115372 | 115375-115396: `id`; `adSpread` 0, `reloY` 1, `movSpread` 1, `lsMlt` 0.5 when null; `burst` false / `burstC` 0 / `burstR` 50 when `burst` is null |
| `classes.json` | classConfig, 16 entries | 115405-115747 | none: the class-editor defaults (115748-115754) are engine constants the C table leaves at 0; the test applies them |
| `effects.json` | effectConfig, 10 entries | 182046-182169 | 182169-182171: `index` |
| `upgradeWeapons.json` | upgradeWeapons, 29 entries | 199770-202652 | 202652-202673: `id`, `upgraded`, `sound` "weapon_upgraded", then the weapon defaults above |
| `attach.json` | attachments, 2 entries | 202939-202969 | none |
| `streaks.json` | streaks, 7 entries | 203338-203521 | 203521-203523: `index` |
| `aimRecticles.json` | scope overlays, 7 entries | 227658-227679 | none |

Values the dumper cannot resolve are strings: `"<fn>"` for functions (the Combat Knife's `anim`, effect /
streak hooks, the Acog's `req`) and `"<expr:Type>"` for other expressions (the `req` gates, which are
identifiers). `Math.PI / 2` (the Bomb / Build Tool / Boulder `rot`) is computed by the script's second static
pass. The test compares every resolved field it has a C field for. `tools/strike/gen_rules.py` also reads
`weapons.json` (Krunker Strike's weapons are stored as differences from it).

## Regenerating

    python3 -I tools/kref/v10-fixtures.py [--deob ~/jails/kru/src/Krunker_v10.0.3/deob] [shared/tests/data/v10]

The script runs the bundle tree's `tools/extractors/dump2.cjs` (Babel AST walker: object literals,
`x = {...}, ...` sequences and `h(h(obj, "k", v), ...)` helper chains) on each table's opening line above, then
applies the post-processing. One table by hand:

    node --max-old-space-size=8192 deob/tools/extractors/dump2.cjs deob/game.deob.js 112381 weapons.json

If the bundle is decoded again and a line moves, the script stops with the table it could not find; find the
new line with `node deob/tools/extractors/arrays.cjs deob/game.deob.js <a key of the table>`, then update
`TABLES` in the script, this table and the citations in the test.
