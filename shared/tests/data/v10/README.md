# Krunker v10.0.7 table fixtures

What `shared/tests/weapons_v10_test.c` checks KrunkNative's tables against. Each file is one table of the
decoded v10.0.7 bundle (`~/jails/kru/src/Krunker_v10.0.7/deob/game.deob.js`, cited as `deob-10.0.7.js:<line>`),
read statically (never executed) and given the bundle's own post-processing, so it holds what the game holds
at run time. The weapon, class, effect, upgrade, attachment, streak and scope tables are byte-identical to
10.0.3's (`deob-10.0.3.js`, the lines in the last column), which the older code comments cite.

| File | Table | deob-10.0.7.js | Post-processing applied | deob-10.0.3.js |
|---|---|---|---|---|
| `weapons.json` | weaponConfig, 31 entries | 99659-102622 | 102626-102634: `id`; `adSpread` 0, `reloY` 1, `movSpread` 1, `lsMlt` 0.5 when null; `burst` false / `burstC` 0 / `burstR` 50 when `burst` is null | 112381 |
| `classes.json` | classConfig, 16 entries | 102638-102980 | none: the class-editor defaults (102981-102985, the list :17276) are engine constants the C table leaves at 0; the test applies them | 115405 |
| `effects.json` | effectConfig, 10 entries | 164539-164662 | 164662-164664: `index` | 182046 |
| `upgradeWeapons.json` | upgradeWeapons, 29 entries | 177550-180402 | 180402-180411: `id`, `upgraded`, `sound` "weapon_upgraded", then the weapon defaults above | 199770 |
| `attach.json` | attachments, 2 entries | 180663-180693 | none | 202939 |
| `streaks.json` | streaks, 7 entries | 180987-181128 | 181128-181130: `index` | 203338 |
| `aimRecticles.json` | scope overlays, 7 entries | 202747-202768 | none | 227658 |
| `modes.json` | the game-mode list, 38 entries in id order | 106124-106681 | none (the list has no loop); identifier entries resolved to their declarations plus every helper chain on that variable | |
| `config.json` | the frozen config object's scalar keys (366) | 19503-20147 | none (`config.cjs` resolves `var _N = <number>` values; the `!0x0` / `!0x1` booleans are read from the object's own lines; arrays, objects and other references are left out) | 22367 |
| `networkSounds.json` | config.networkSounds, 401 names in id order | 18260 (config key 19904) | none. Written with `shared/src/rules/v10/network_sounds_v10.c` by `tools/krunker/extract_sounds.py` (not the script below), checked by `shared/tests/network_sounds_test.c` | 21141 (399 names) |

Values the dumper cannot resolve are strings: `"<fn>"` for functions (the Combat Knife's `anim`, effect /
streak hooks, the Acog's `req`, the mode hooks) and `"<expr:Type>"` for other expressions (the `req` gates, which
are identifiers). `Math.PI / 2` (the Bomb / Build Tool / Boulder `rot`) is computed by the script's second static
pass. The test compares every resolved field it has a C field for; a mode hook is compared by presence. `tools/strike/gen_rules.py` also reads
`weapons.json` (Krunker Strike's weapons are stored as differences from it).

## Regenerating

    python3 -I tools/kref/v10-fixtures.py [--deob ~/jails/kru/src/Krunker_v10.0.7/deob] [--tools TOOLS] [shared/tests/data/v10]

TOOLS is the extractor tree with `node_modules/@babel/parser` and `extractors/config.cjs` (default: the deob
tree's `tools/`, else `~/jails/kru/src/Krunker_v10.0.3/deob/tools`). The script's own Babel walkers dump each
table (the logic of `extractors/dump2.cjs`, but taking the array of the table's entry count that opens on its
line: in 10.0.7 the upgradeWeapons array shares its line with the country list) and the mode list, then apply
the post-processing; `extractors/config.cjs` gives `config.json`.

If the bundle is decoded again and a line moves, the script stops with the table it could not find; find the
new line with `node TOOLS/extractors/arrays.cjs deob/game.deob.js <a key of the table>`, then update `TABLES`
(or `MODES_LINE`) in the script, this table and the citations in the test.
