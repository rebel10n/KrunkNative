# Krunker v10.0.3 gameplay audit

Audit of KrunkNative's gameplay values against the official Krunker v10.0.3 client (2026-10-22). Target: doc/v10/target.md.

Status key: **match** (equal after the unit conversion shown); **mismatch (fixed)** (changed here, with a test that failed before); **mismatch (not fixed: ...)** (a rule or feature rather than a value, left for its own change); **not found** (no 10.0.3 key or reader; the value is dead and left as is) and **placeholder** (a session value, not a constant); the ten former unverified rows are settled in `doc/v10/constants-1003.md`; **covered** (fixed by another owner, not re-audited).

## Reference and method

- Reference: `~/jails/kru/src/Krunker_v10.0.3/game.js` (read only). Its weapon and class names are string-array encoded, so names are not literal there. The audit reads the decoded copy `~/jails/kru/src/Krunker_v10.0.3/deob/game.deob.js` (webcrack of the same `game.js`, same literals). "deob byte N" is the UTF-8 byte offset in that file, with its line.
- Weapon and class tables: extracted with the acorn table extractor of the time (no eval; formerly `~/.cache/kn-v10-tools/extract.js`, not kept: the current dumpers are `deob/tools/extractors/`, output `deob/tables/wpn-1003*.json` and `cls-1003.json`) and written by `tools/kref/v10-fixtures.py`. Since 2026-10-24 that script regenerates every fixture of `shared/tests/data/v10/` (weapons, classes, effects, streaks, attachments, scopes, upgrades, config) straight from `deob/game.deob.js` with `deob/tools/extractors/dump2.cjs` and `config.cjs` (`shared/tests/data/v10/README.md`), and `weapons_v10_test` checks the C tables and constants against them. In 10.0.3 the Minigun is built through a defineProperty chain, so the extractor ran with `DEFP` set to that helper. The 10.0.3 fixtures equal the 10.0.2 fixtures (`shared/tests/data/v10/`) in every numeric and string field. The only differences are the mangled names of the `req` unlock functions and `Math.PI` spelling; the `req` gates were compared by AST shape and are the same boolean function (De Morgan).
- Numeric literals: diffing 10.0.2 and 10.0.3 (`doc/v10/changelog-10.0.3.md`) changed four literals. 1597 was 10.0.2's ahNum rotation key (10.0.3 rotates by 3, deob line 111514; KN's `KRUNKER_AHKEY` is 0x3, row "Native protocol ahNum key"). 0.6075 is a scene-object y offset in a render block (v10.js:200136), not gameplay, and is not audited. 60000000 (asset download cap) and 1179937895 (glTF magic) are not gameplay. So 10.0.3's gameplay numbers are 10.0.2's.
- Movement and physics constants: the frozen config object of the bundle, resolved with acorn; numeric keys are equal between 10.0.2 and 10.0.3.
- Units: our constants are per second (per-millisecond speeds x 1000, times in seconds); weapon `rate` and `reload` are seconds in C and milliseconds in the bundle.

## Summary

| Status | Rows |
|---|---|
| match | 285 |
| mismatch (fixed) | 3 |
| not found (dead in 10.0.3 and v7) | 4 |
| placeholder (not a constant) | 1 |
| covered | 1 |
| **total** | **294** |

Weapons (31 x damage, damage drop, fire rate, reload, clip size) and classes (16 x health, speed, wall jump, wall jumps): 219 rows match, all against the 10.0.3 literal values (then `weapons_v7_test`, 7238 checks, and the class table in the same test; now `weapons_v10_test`).

## Mismatches fixed

1. **Comp warmup spawn spread: 40 to 10.** 10.0.3's `getSpawnPoint` spreads a comp spawn with `getPointSpread(x, z, y, 10, dir)` (deob line 217148). `getPointSpread` offsets by `spread - 10`, so the point is the comp spawn itself, rounded to whole units. KN used 40 (up to 30 units off, at random). Fix: `comp_spawn_spread` in the ruleset (`shared/include/ruleset.h`, `shared/src/rules/ruleset.c:42`), read by `game_get_spawn_point` (`shared/src/modes/mode_round.c:653`). Test: `shared/tests/spawn_test.c` (six comp respawns per team, bound 0.7072), now run by `make test`. Before: 1440 of 7620 checks failed (respawns 12 to 30 units off). After: 7620 OK. `shared/tests/ruleset_test.c` checks the field.

2. **Team round wins outside best-of.** 10.0.3's endGame else branch counts the round's winner in `teamRoundWins` when bestOf is off too (deob 221597-221605); the match is then won by `getWinTeam()` over those round wins. Fix: `shared/src/modes/mode_round.c:373-382`. Test: `shared/tests/modes_test.c` `test_round_wins_outside_best_of` (4 checks failed before, all pass after). Details and the scoreboard: `doc/v10/rules-1003.md` section 1.
3. **Battle Pass points per public match.** The audit's formula left out the gate: 10.0.3 adds the play term only when `!isEmptyLobby` (deob 221768), and `isEmptyLobby` is set true at init and never cleared, so the term never applies (v7.1.4 has the same dead branch). KN sent `min(20, ...) + 30 if half played` for every counted final commit, the +30 included. Fix: `bp_match_points` (`shared/src/season.c`) takes the 10.0.3 inputs and returns 0 unless the match is public; `server/src/accounts.c:1158-1167` sends `bpp` only for public matches, with the play term off. Test: `shared/tests/season_test.c` `test_match_points` (a public 60%-played match: 15, was 45). Details: `doc/v10/rules-1003.md` section 2.

## Mismatches not fixed

- **Winner override in public matches.** 10.0.3 sets `this.winner = -1` for every public, non-raid, non-sandbox match with players (deob 221681-221682), because `isEmptyLobby` is always true. KN gives public matches a winner. Porting it changes who wins and who gets "w" credit: a decision, see `doc/v10/rules-1003.md` open item 1.


## Audit table

| Value | Our value (file:line) | 10.0.3 value (deob byte offset and line, or label) | Status |
|---|---|---|---|
| Sniper Rifle damage | 100 (shared/src/weapons/awp.c:27) | deob byte 4712363 "Sniper Rifle" dmg: 100 | match |
| Sniper Rifle damage drop | 30 (shared/src/weapons/awp.c:32) | deob byte 4712465 "Sniper Rifle" dmgDrop: 30 | match |
| Sniper Rifle fire rate (ms between shots) | 1 s = 1000 ms (shared/src/weapons/awp.c:33) | deob byte 4712826 "Sniper Rifle" rate: 1000 ms | match |
| Sniper Rifle reload | 1.9 s = 1900 ms (shared/src/weapons/awp.c:24) | deob byte 4712410 "Sniper Rifle" reload: 1900 ms | match |
| Sniper Rifle clip size (ammo) | 3 (shared/src/weapons/awp.c:22) | deob byte 4712350 "Sniper Rifle" ammo: 3 | match |
| Assault Rifle damage | 23 (shared/src/rules/v10/weapons_rebalance_v10.c:32) | deob byte 4714240 "Assault Rifle" dmg: 23 | match |
| Assault Rifle damage drop | 8 (shared/src/rules/v10/weapons_rebalance_v10.c:36) | deob byte 4714300 "Assault Rifle" dmgDrop: 8 | match |
| Assault Rifle fire rate (ms between shots) | 0.115 s = 115 ms (shared/src/rules/v10/weapons_rebalance_v10.c:37) | deob byte 4714207 "Assault Rifle" rate: 115 ms | match |
| Assault Rifle reload | 1.5 s = 1500 ms (shared/src/rules/v10/weapons_rebalance_v10.c:29) | deob byte 4714222 "Assault Rifle" reload: 1500 ms | match |
| Assault Rifle clip size (ammo) | 30 (shared/src/rules/v10/weapons_rebalance_v10.c:27) | deob byte 4714193 "Assault Rifle" ammo: 30 | match |
| Pistol damage | 20 (shared/src/rules/v10/weapons_rebalance_v10.c:110) | deob byte 4716324 "Pistol" dmg: 20 | match |
| Pistol damage drop | 10 (shared/src/rules/v10/weapons_rebalance_v10.c:113) | deob byte 4716369 "Pistol" dmgDrop: 10 | match |
| Pistol fire rate (ms between shots) | 0.15 s = 150 ms (shared/src/rules/v10/weapons_rebalance_v10.c:114) | deob byte 4716829 "Pistol" rate: 150 ms | match |
| Pistol reload | 0.7 s = 700 ms (shared/src/rules/v10/weapons_rebalance_v10.c:107) | deob byte 4716307 "Pistol" reload: 700 ms | match |
| Pistol clip size (ammo) | 10 (shared/src/rules/v10/weapons_rebalance_v10.c:105) | deob byte 4716293 "Pistol" ammo: 10 | match |
| Submachine Gun damage | 18 (shared/src/rules/v10/weapons_rebalance_v10.c:180) | deob byte 4718266 "Submachine Gun" dmg: 18 | match |
| Submachine Gun damage drop | 9 (shared/src/rules/v10/weapons_rebalance_v10.c:184) | deob byte 4718326 "Submachine Gun" dmgDrop: 9 | match |
| Submachine Gun fire rate (ms between shots) | 0.095 s = 95 ms (shared/src/rules/v10/weapons_rebalance_v10.c:185) | deob byte 4718740 "Submachine Gun" rate: 95 ms | match |
| Submachine Gun reload | 1 s = 1000 ms (shared/src/rules/v10/weapons_rebalance_v10.c:177) | deob byte 4718248 "Submachine Gun" reload: 1000 ms | match |
| Submachine Gun clip size (ammo) | 24 (shared/src/rules/v10/weapons_rebalance_v10.c:175) | deob byte 4718234 "Submachine Gun" ammo: 24 | match |
| Revolver damage | 66 (shared/src/weapons/rev.c:24) | deob byte 4720215 "Revolver" dmg: 66 | match |
| Revolver damage drop | 10 (shared/src/weapons/rev.c:28) | deob byte 4720262 "Revolver" dmgDrop: 10 | match |
| Revolver fire rate (ms between shots) | 0.45 s = 450 ms (shared/src/weapons/rev.c:29) | deob byte 4720737 "Revolver" rate: 450 ms | match |
| Revolver reload | 0.9 s = 900 ms (shared/src/weapons/rev.c:21) | deob byte 4720198 "Revolver" reload: 900 ms | match |
| Revolver clip size (ammo) | 6 (shared/src/weapons/rev.c:19) | deob byte 4720185 "Revolver" ammo: 6 | match |
| Shotgun damage | 50 (shared/src/weapons/shotgun.c:23) | deob byte 4722282 "Shotgun" dmg: 50 | match |
| Shotgun damage drop | 50 (shared/src/weapons/shotgun.c:26) | deob byte 4722312 "Shotgun" dmgDrop: 50 | match |
| Shotgun fire rate (ms between shots) | 0.45 s = 450 ms (shared/src/weapons/shotgun.c:27) | deob byte 4722723 "Shotgun" rate: 450 ms | match |
| Shotgun reload | 1.1 s = 1100 ms (shared/src/weapons/shotgun.c:20) | deob byte 4722264 "Shotgun" reload: 1100 ms | match |
| Shotgun clip size (ammo) | 2 (shared/src/weapons/shotgun.c:17) | deob byte 4722251 "Shotgun" ammo: 2 | match |
| Machine Gun damage | 17 (shared/src/rules/v10/weapons_rebalance_v10.c:256) | deob byte 4724347 "Machine Gun" dmg: 17 | match |
| Machine Gun damage drop | 10 (shared/src/rules/v10/weapons_rebalance_v10.c:260) | deob byte 4724407 "Machine Gun" dmgDrop: 10 | match |
| Machine Gun fire rate (ms between shots) | 0.125 s = 125 ms (shared/src/rules/v10/weapons_rebalance_v10.c:261) | deob byte 4724925 "Machine Gun" rate: 125 ms | match |
| Machine Gun reload | 3.3 s = 3300 ms (shared/src/rules/v10/weapons_rebalance_v10.c:252) | deob byte 4724329 "Machine Gun" reload: 3300 ms | match |
| Machine Gun clip size (ammo) | 50 (shared/src/rules/v10/weapons_rebalance_v10.c:250) | deob byte 4724315 "Machine Gun" ammo: 50 | match |
| Semi Auto damage | 35 (shared/src/rules/v10/weapons_rebalance_v10.c:332) | deob byte 4727031 "Semi Auto" dmg: 35 | match |
| Semi Auto damage drop | 0 (shared/src/rules/v10/weapons_rebalance_v10.c:336) | deob byte 4727116 "Semi Auto" dmgDrop: 0 | match |
| Semi Auto fire rate (ms between shots) | 0.145 s = 145 ms (shared/src/rules/v10/weapons_rebalance_v10.c:337) | deob byte 4727535 "Semi Auto" rate: 145 ms | match |
| Semi Auto reload | 1.35 s = 1350 ms (shared/src/rules/v10/weapons_rebalance_v10.c:329) | deob byte 4727013 "Semi Auto" reload: 1350 ms | match |
| Semi Auto clip size (ammo) | 8 (shared/src/rules/v10/weapons_rebalance_v10.c:327) | deob byte 4727000 "Semi Auto" ammo: 8 | match |
| Rocket Launcher damage | 25 (shared/src/weapons/rpg.c:51) | deob byte 4729013 "Rocket Launcher" dmg: 25 | match |
| Rocket Launcher damage drop | 0 (absent; shared/src/weapons/rpg.c:6) | absent in "Rocket Launcher" (0) | match |
| Rocket Launcher fire rate (ms between shots) | 0.32 s = 320 ms (shared/src/weapons/rpg.c:26) | deob byte 4729498 "Rocket Launcher" rate: 320 ms | match |
| Rocket Launcher reload | 1.5 s = 1500 ms (shared/src/weapons/rpg.c:22) | deob byte 4729053 "Rocket Launcher" reload: 1500 ms | match |
| Rocket Launcher clip size (ammo) | 3 (shared/src/weapons/rpg.c:19) | deob byte 4729026 "Rocket Launcher" ammo: 3 | match |
| Akimbo Uzi damage | 16 (shared/src/rules/v10/weapons_rebalance_v10.c:406) | deob byte 4730805 "Akimbo Uzi" dmg: 16 | match |
| Akimbo Uzi damage drop | 13 (shared/src/rules/v10/weapons_rebalance_v10.c:410) | deob byte 4730865 "Akimbo Uzi" dmgDrop: 13 | match |
| Akimbo Uzi fire rate (ms between shots) | 0.07 s = 70 ms (shared/src/rules/v10/weapons_rebalance_v10.c:411) | deob byte 4731257 "Akimbo Uzi" rate: 70 ms | match |
| Akimbo Uzi reload | 1.2 s = 1200 ms (shared/src/rules/v10/weapons_rebalance_v10.c:403) | deob byte 4730787 "Akimbo Uzi" reload: 1200 ms | match |
| Akimbo Uzi clip size (ammo) | 28 (shared/src/rules/v10/weapons_rebalance_v10.c:401) | deob byte 4730773 "Akimbo Uzi" ammo: 28 | match |
| Desert Eagle damage | 45 (shared/src/rules/v10/weapons_rebalance_v10.c:468) | deob byte 4732635 "Desert Eagle" dmg: 45 | match |
| Desert Eagle damage drop | 10 (shared/src/rules/v10/weapons_rebalance_v10.c:472) | deob byte 4732682 "Desert Eagle" dmgDrop: 10 | match |
| Desert Eagle fire rate (ms between shots) | 0.45 s = 450 ms (shared/src/rules/v10/weapons_rebalance_v10.c:473) | deob byte 4733028 "Desert Eagle" rate: 450 ms | match |
| Desert Eagle reload | 1 s = 1000 ms (shared/src/rules/v10/weapons_rebalance_v10.c:465) | deob byte 4732617 "Desert Eagle" reload: 1000 ms | match |
| Desert Eagle clip size (ammo) | 7 (shared/src/rules/v10/weapons_rebalance_v10.c:463) | deob byte 4732604 "Desert Eagle" ammo: 7 | match |
| Alien Blaster damage | 45 (shared/src/weapons/alien_blaster.c:22) | deob byte 4734630 "Alien Blaster" dmg: 45 | match |
| Alien Blaster damage drop | 10 (shared/src/weapons/alien_blaster.c:26) | deob byte 4734678 "Alien Blaster" dmgDrop: 10 | match |
| Alien Blaster fire rate (ms between shots) | 0.17 s = 170 ms (shared/src/weapons/alien_blaster.c:27) | deob byte 4735102 "Alien Blaster" rate: 170 ms | match |
| Alien Blaster reload | 1.5 s = 1500 ms (shared/src/weapons/alien_blaster.c:19) | deob byte 4734612 "Alien Blaster" reload: 1500 ms | match |
| Alien Blaster clip size (ammo) | 4 (shared/src/weapons/alien_blaster.c:17) | deob byte 4734599 "Alien Blaster" ammo: 4 | match |
| Combat Knife damage | 50 (shared/src/weapons/knife.c:107) | deob byte 4739936 "Combat Knife" dmg: 50 | match |
| Combat Knife damage drop | 0 (shared/src/weapons/knife.c:109) | deob byte 4739949 "Combat Knife" dmgDrop: 0 | match |
| Combat Knife fire rate (ms between shots) | 0.25 s = 250 ms (shared/src/weapons/knife.c:110) | deob byte 4739921 "Combat Knife" rate: 250 ms | match |
| Combat Knife reload | 0 (absent; shared/src/weapons/knife.c:97) | absent in "Combat Knife" (0) ms | match |
| Combat Knife clip size (ammo) | 0 (absent; shared/src/weapons/knife.c:97) | absent in "Combat Knife" (0) | match |
| Crossbow damage | 100 (shared/src/weapons/crossbow.c:25) | deob byte 4741251 "Crossbow" dmg: 100 | match |
| Crossbow damage drop | 0 (shared/src/weapons/crossbow.c:29) | deob byte 4741308 "Crossbow" dmgDrop: 0 | match |
| Crossbow fire rate (ms between shots) | 0.15 s = 150 ms (shared/src/weapons/crossbow.c:30) | deob byte 4741728 "Crossbow" rate: 150 ms | match |
| Crossbow reload | 0.9 s = 900 ms (shared/src/weapons/crossbow.c:22) | deob byte 4741234 "Crossbow" reload: 900 ms | match |
| Crossbow clip size (ammo) | 1 (shared/src/weapons/crossbow.c:20) | deob byte 4741221 "Crossbow" ammo: 1 | match |
| Famas damage | 28 (shared/src/rules/v10/weapons_rebalance_v10.c:540) | deob byte 4743115 "Famas" dmg: 28 | match |
| Famas damage drop | 8 (shared/src/rules/v10/weapons_rebalance_v10.c:544) | deob byte 4743175 "Famas" dmgDrop: 8 | match |
| Famas fire rate (ms between shots) | 0.33 s = 330 ms (shared/src/rules/v10/weapons_rebalance_v10.c:545) | deob byte 4743082 "Famas" rate: 330 ms | match |
| Famas reload | 1.5 s = 1500 ms (shared/src/rules/v10/weapons_rebalance_v10.c:537) | deob byte 4743097 "Famas" reload: 1500 ms | match |
| Famas clip size (ammo) | 24 (shared/src/rules/v10/weapons_rebalance_v10.c:535) | deob byte 4743068 "Famas" ammo: 24 | match |
| Sawed Off damage | 12 (shared/src/weapons/sawed_off.c:23) | deob byte 4744847 "Sawed Off" dmg: 12 | match |
| Sawed Off damage drop | 12 (shared/src/weapons/sawed_off.c:26) | deob byte 4744876 "Sawed Off" dmgDrop: 12 | match |
| Sawed Off fire rate (ms between shots) | 0.4 s = 400 ms (shared/src/weapons/sawed_off.c:27) | deob byte 4745270 "Sawed Off" rate: 400 ms | match |
| Sawed Off reload | 1.1 s = 1100 ms (shared/src/weapons/sawed_off.c:20) | deob byte 4744829 "Sawed Off" reload: 1100 ms | match |
| Sawed Off clip size (ammo) | 1 (shared/src/weapons/sawed_off.c:17) | deob byte 4744816 "Sawed Off" ammo: 1 | match |
| Auto Pistol damage | 16 (shared/src/weapons/auto_pistol.c:22) | deob byte 4746562 "Auto Pistol" dmg: 16 | match |
| Auto Pistol damage drop | 2 (shared/src/weapons/auto_pistol.c:26) | deob byte 4746609 "Auto Pistol" dmgDrop: 2 | match |
| Auto Pistol fire rate (ms between shots) | 0.1 s = 100 ms (shared/src/weapons/auto_pistol.c:27) | deob byte 4747015 "Auto Pistol" rate: 100 ms | match |
| Auto Pistol reload | 1 s = 1000 ms (shared/src/weapons/auto_pistol.c:19) | deob byte 4746544 "Auto Pistol" reload: 1000 ms | match |
| Auto Pistol clip size (ammo) | 15 (shared/src/weapons/auto_pistol.c:17) | deob byte 4746530 "Auto Pistol" ammo: 15 | match |
| Bomb damage | 0 (absent; shared/src/weapons/bomb.c:6) | absent in "Bomb" (0) | match |
| Bomb damage drop | 0 (absent; shared/src/weapons/bomb.c:6) | absent in "Bomb" (0) | match |
| Bomb fire rate (ms between shots) | 0 (absent; shared/src/weapons/bomb.c:6) | absent in "Bomb" (0) ms | match |
| Bomb reload | 0 (absent; shared/src/weapons/bomb.c:6) | absent in "Bomb" (0) ms | match |
| Bomb clip size (ammo) | 0 (absent; shared/src/weapons/bomb.c:6) | absent in "Bomb" (0) | match |
| Blaster damage | 1 (shared/src/rules/v10/weapons_rebalance_v10.c:614) | deob byte 4749115 "Blaster" dmg: 1 | match |
| Blaster damage drop | 0 (absent; shared/src/rules/v10/weapons_rebalance_v10.c:596) | deob byte 4749413 "Blaster" dmgDrop: 0 | match |
| Blaster fire rate (ms between shots) | 0.16 s = 160 ms (shared/src/rules/v10/weapons_rebalance_v10.c:618) | deob byte 4749082 "Blaster" rate: 160 ms | match |
| Blaster reload | 1.45 s = 1450 ms (shared/src/rules/v10/weapons_rebalance_v10.c:611) | deob byte 4749097 "Blaster" reload: 1450 ms | match |
| Blaster clip size (ammo) | 16 (shared/src/rules/v10/weapons_rebalance_v10.c:609) | deob byte 4749068 "Blaster" ammo: 16 | match |
| Build Tool damage | 0 (absent; shared/src/weapons/v7_build_tool.c:5) | absent in "Build Tool" (0) | match |
| Build Tool damage drop | 0 (absent; shared/src/weapons/v7_build_tool.c:5) | absent in "Build Tool" (0) | match |
| Build Tool fire rate (ms between shots) | 0.3 s = 300 ms (shared/src/weapons/v7_build_tool.c:19) | deob byte 4751091 "Build Tool" rate: 300 ms | match |
| Build Tool reload | 0 (absent; shared/src/weapons/v7_build_tool.c:5) | absent in "Build Tool" (0) ms | match |
| Build Tool clip size (ammo) | 0 (absent; shared/src/weapons/v7_build_tool.c:5) | absent in "Build Tool" (0) | match |
| Grappler damage | 1 (shared/src/weapons/v7_grappler.c:20) | deob byte 4752073 "Grappler" dmg: 1 | match |
| Grappler damage drop | 0 (absent; shared/src/weapons/v7_grappler.c:5) | deob byte 4752119 "Grappler" dmgDrop: 0 | match |
| Grappler fire rate (ms between shots) | 0.3 s = 300 ms (shared/src/weapons/v7_grappler.c:24) | deob byte 4752482 "Grappler" rate: 300 ms | match |
| Grappler reload | 1 s = 1000 ms (shared/src/weapons/v7_grappler.c:17) | deob byte 4752055 "Grappler" reload: 1000 ms | match |
| Grappler clip size (ammo) | 10 (shared/src/weapons/v7_grappler.c:15) | deob byte 4752041 "Grappler" ammo: 10 | match |
| Tehchy-9 damage | 23 (shared/src/rules/v10/weapons_rebalance_v10.c:691) | deob byte 4753585 "Tehchy-9" dmg: 23 | match |
| Tehchy-9 damage drop | 2 (shared/src/rules/v10/weapons_rebalance_v10.c:695) | deob byte 4753633 "Tehchy-9" dmgDrop: 2 | match |
| Tehchy-9 fire rate (ms between shots) | 0.38 s = 380 ms (shared/src/rules/v10/weapons_rebalance_v10.c:696) | deob byte 4754039 "Tehchy-9" rate: 380 ms | match |
| Tehchy-9 reload | 1 s = 1000 ms (shared/src/rules/v10/weapons_rebalance_v10.c:688) | deob byte 4753567 "Tehchy-9" reload: 1000 ms | match |
| Tehchy-9 clip size (ammo) | 15 (shared/src/rules/v10/weapons_rebalance_v10.c:686) | deob byte 4753553 "Tehchy-9" ammo: 15 | match |
| Noob Tube damage | 12 (shared/src/weapons/v7_noob_tube.c:20) | deob byte 4755229 "Noob Tube" dmg: 12 | match |
| Noob Tube damage drop | 12 (shared/src/weapons/v7_noob_tube.c:23) | deob byte 4755270 "Noob Tube" dmgDrop: 12 | match |
| Noob Tube fire rate (ms between shots) | 0.4 s = 400 ms (shared/src/weapons/v7_noob_tube.c:24) | deob byte 4755697 "Noob Tube" rate: 400 ms | match |
| Noob Tube reload | 1.2 s = 1200 ms (shared/src/weapons/v7_noob_tube.c:17) | deob byte 4755211 "Noob Tube" reload: 1200 ms | match |
| Noob Tube clip size (ammo) | 1 (shared/src/weapons/v7_noob_tube.c:15) | deob byte 4755198 "Noob Tube" ammo: 1 | match |
| Slimer damage | 1 (shared/src/weapons/v7_slimer.c:21) | deob byte 4757842 "Slimer" dmg: 1 | match |
| Slimer damage drop | 0 (absent; shared/src/weapons/v7_slimer.c:5) | deob byte 4757888 "Slimer" dmgDrop: 0 | match |
| Slimer fire rate (ms between shots) | 0.3 s = 300 ms (shared/src/weapons/v7_slimer.c:25) | deob byte 4758408 "Slimer" rate: 300 ms | match |
| Slimer reload | 5 s = 5000 ms (shared/src/weapons/v7_slimer.c:18) | deob byte 4757779 "Slimer" reload: 5000 ms | match |
| Slimer clip size (ammo) | 2 (shared/src/weapons/v7_slimer.c:16) | deob byte 4757766 "Slimer" ammo: 2 | match |
| Zapper damage | 10 (shared/src/weapons/v7_zapper.c:20) | deob byte 4759711 "Zapper" dmg: 10 | match |
| Zapper damage drop | 10 (shared/src/weapons/v7_zapper.c:23) | deob byte 4759768 "Zapper" dmgDrop: 10 | match |
| Zapper fire rate (ms between shots) | 0.15 s = 150 ms (shared/src/weapons/v7_zapper.c:24) | deob byte 4760239 "Zapper" rate: 150 ms | match |
| Zapper reload | 1.4 s = 1400 ms (shared/src/weapons/v7_zapper.c:17) | deob byte 4759693 "Zapper" reload: 1400 ms | match |
| Zapper clip size (ammo) | 5 (shared/src/weapons/v7_zapper.c:15) | deob byte 4759680 "Zapper" ammo: 5 | match |
| Minigun damage | 18 (shared/src/weapons/v7_minigun.c:21) | deob byte 4761806 "Minigun" dmg: 18 | match |
| Minigun damage drop | 10 (shared/src/weapons/v7_minigun.c:25) | deob byte 4761866 "Minigun" dmgDrop: 10 | match |
| Minigun fire rate (ms between shots) | 0.07 s = 70 ms (shared/src/weapons/v7_minigun.c:26) | deob byte 4762372 "Minigun" rate: 70 ms | match |
| Minigun reload | 3.3 s = 3300 ms (shared/src/weapons/v7_minigun.c:17) | deob byte 4761743 "Minigun" reload: 3300 ms | match |
| Minigun clip size (ammo) | 250 (shared/src/weapons/v7_minigun.c:15) | deob byte 4761728 "Minigun" ammo: 250 | match |
| War Machine damage | 12 (shared/src/weapons/v7_war_machine.c:21) | deob byte 4763395 "War Machine" dmg: 12 | match |
| War Machine damage drop | 12 (shared/src/weapons/v7_war_machine.c:24) | deob byte 4763436 "War Machine" dmgDrop: 12 | match |
| War Machine fire rate (ms between shots) | 0.3 s = 300 ms (shared/src/weapons/v7_war_machine.c:25) | deob byte 4763879 "War Machine" rate: 300 ms | match |
| War Machine reload | 2 s = 2000 ms (shared/src/weapons/v7_war_machine.c:18) | deob byte 4763332 "War Machine" reload: 2000 ms | match |
| War Machine clip size (ammo) | 6 (shared/src/weapons/v7_war_machine.c:16) | deob byte 4763319 "War Machine" ammo: 6 | match |
| Akimbo Pistol damage | 15 (shared/src/weapons/v7_akimbo_pistol.c:22) | deob byte 4765121 "Akimbo Pistol" dmg: 15 | match |
| Akimbo Pistol damage drop | 13 (shared/src/weapons/v7_akimbo_pistol.c:26) | deob byte 4765181 "Akimbo Pistol" dmgDrop: 13 | match |
| Akimbo Pistol fire rate (ms between shots) | 0.1 s = 100 ms (shared/src/weapons/v7_akimbo_pistol.c:27) | deob byte 4765593 "Akimbo Pistol" rate: 100 ms | match |
| Akimbo Pistol reload | 0.75 s = 750 ms (shared/src/weapons/v7_akimbo_pistol.c:19) | deob byte 4765104 "Akimbo Pistol" reload: 750 ms | match |
| Akimbo Pistol clip size (ammo) | 12 (shared/src/weapons/v7_akimbo_pistol.c:17) | deob byte 4765090 "Akimbo Pistol" ammo: 12 | match |
| Charge Rifle damage | 15 (shared/src/weapons/v7_charge_rifle.c:21) | deob byte 4767117 "Charge Rifle" dmg: 15 | match |
| Charge Rifle damage drop | 5 (shared/src/weapons/v7_charge_rifle.c:25) | deob byte 4767194 "Charge Rifle" dmgDrop: 5 | match |
| Charge Rifle fire rate (ms between shots) | 0.15 s = 150 ms (shared/src/weapons/v7_charge_rifle.c:26) | deob byte 4767583 "Charge Rifle" rate: 150 ms | match |
| Charge Rifle reload | 1.4 s = 1400 ms (shared/src/weapons/v7_charge_rifle.c:18) | deob byte 4767159 "Charge Rifle" reload: 1400 ms | match |
| Charge Rifle clip size (ammo) | 1 (shared/src/weapons/v7_charge_rifle.c:16) | deob byte 4767104 "Charge Rifle" ammo: 1 | match |
| Boulder damage | 0 (absent; shared/src/rules/v10/weapons_new_v10.c:17) | absent in "Boulder" (0) | match |
| Boulder damage drop | 0 (absent; shared/src/rules/v10/weapons_new_v10.c:17) | absent in "Boulder" (0) | match |
| Boulder fire rate (ms between shots) | 0 (absent; shared/src/rules/v10/weapons_new_v10.c:17) | absent in "Boulder" (0) ms | match |
| Boulder reload | 0 (absent; shared/src/rules/v10/weapons_new_v10.c:17) | absent in "Boulder" (0) ms | match |
| Boulder clip size (ammo) | 0 (absent; shared/src/rules/v10/weapons_new_v10.c:17) | absent in "Boulder" (0) | match |
| Compressor damage | 1 (shared/src/rules/v10/weapons_new_v10.c:92) | deob byte 4769810 "Compressor" dmg: 1 | match |
| Compressor damage drop | 10 (shared/src/rules/v10/weapons_new_v10.c:94) | deob byte 4769839 "Compressor" dmgDrop: 10 | match |
| Compressor fire rate (ms between shots) | 0.45 s = 450 ms (shared/src/rules/v10/weapons_new_v10.c:109) | deob byte 4770273 "Compressor" rate: 450 ms | match |
| Compressor reload | 2 s = 2000 ms (shared/src/rules/v10/weapons_new_v10.c:91) | deob byte 4769792 "Compressor" reload: 2000 ms | match |
| Compressor clip size (ammo) | 1 (shared/src/rules/v10/weapons_new_v10.c:90) | deob byte 4769779 "Compressor" ammo: 1 | match |
| Triggerman health | 100 (shared/src/rules/v10/classes_v10.c:27) | deob byte 4772566 "Triggerman" health: 100 | match |
| Triggerman speed | 1.05 (shared/src/rules/v10/classes_v10.c:30) | deob byte 4772616 "Triggerman" speed: 1.05 | match |
| Triggerman wall jump | 0 (shared/src/rules/v10/classes_v10.c:25) | deob byte 4772664 "Triggerman" wallJ: false | match |
| Triggerman wall jumps (max) | 0 (absent; shared/src/rules/v10/classes_v10.c:20) | deob byte 4772682 "Triggerman" wallJM: 0 | match |
| Hunter health | 60 (shared/src/rules/v10/classes_v10.c:43) | deob byte 4772923 "Hunter" health: 60 | match |
| Hunter speed | 1 (shared/src/rules/v10/classes_v10.c:46) | deob byte 4772972 "Hunter" speed: 1 | match |
| Hunter wall jump | 0 (shared/src/rules/v10/classes_v10.c:41) | deob byte 4773017 "Hunter" wallJ: false | match |
| Hunter wall jumps (max) | 0 (absent; shared/src/rules/v10/classes_v10.c:36) | deob byte 4773035 "Hunter" wallJM: 0 | match |
| Run N Gun health | 100 (shared/src/rules/v10/classes_v10.c:61) | deob byte 4773350 "Run N Gun" health: 100 | match |
| Run N Gun speed | 1.18 (shared/src/rules/v10/classes_v10.c:64) | deob byte 4773400 "Run N Gun" speed: 1.18 | match |
| Run N Gun wall jump | 1 (shared/src/rules/v10/classes_v10.c:58) | deob byte 4773249 "Run N Gun" wallJ: true | match |
| Run N Gun wall jumps (max) | 2 (shared/src/rules/v10/classes_v10.c:59) | deob byte 4773266 "Run N Gun" wallJM: 2 | match |
| Spray N Pray health | 170 (shared/src/rules/v10/classes_v10.c:79) | deob byte 4773805 "Spray N Pray" health: 170 | match |
| Spray N Pray speed | 0.9 (shared/src/rules/v10/classes_v10.c:82) | deob byte 4773872 "Spray N Pray" speed: 0.9 | match |
| Spray N Pray wall jump | 0 (shared/src/rules/v10/classes_v10.c:77) | deob byte 4773903 "Spray N Pray" wallJ: false | match |
| Spray N Pray wall jumps (max) | 0 (absent; shared/src/rules/v10/classes_v10.c:71) | deob byte 4773921 "Spray N Pray" wallJM: 0 | match |
| Vince health | 90 (shared/src/rules/v10/classes_v10.c:98) | deob byte 4774268 "Vince" health: 90 | match |
| Vince speed | 1 (shared/src/rules/v10/classes_v10.c:101) | deob byte 4774317 "Vince" speed: 1 | match |
| Vince wall jump | 0 (shared/src/rules/v10/classes_v10.c:96) | deob byte 4774362 "Vince" wallJ: false | match |
| Vince wall jumps (max) | 0 (absent; shared/src/rules/v10/classes_v10.c:90) | deob byte 4774380 "Vince" wallJM: 0 | match |
| Detective health | 100 (shared/src/rules/v10/classes_v10.c:116) | deob byte 4774697 "Detective" health: 100 | match |
| Detective speed | 1 (shared/src/rules/v10/classes_v10.c:119) | deob byte 4774747 "Detective" speed: 1 | match |
| Detective wall jump | 0 (shared/src/rules/v10/classes_v10.c:114) | deob byte 4774792 "Detective" wallJ: false | match |
| Detective wall jumps (max) | 0 (absent; shared/src/rules/v10/classes_v10.c:108) | deob byte 4774810 "Detective" wallJM: 0 | match |
| Marksman health | 90 (shared/src/rules/v10/classes_v10.c:133) | deob byte 4775091 "Marksman" health: 90 | match |
| Marksman speed | 1 (shared/src/rules/v10/classes_v10.c:136) | deob byte 4775140 "Marksman" speed: 1 | match |
| Marksman wall jump | 0 (shared/src/rules/v10/classes_v10.c:131) | deob byte 4775185 "Marksman" wallJ: false | match |
| Marksman wall jumps (max) | 0 (absent; shared/src/rules/v10/classes_v10.c:126) | deob byte 4775203 "Marksman" wallJM: 0 | match |
| Rocketeer health | 130 (shared/src/rules/v10/classes_v10.c:151) | deob byte 4775504 "Rocketeer" health: 130 | match |
| Rocketeer speed | 0.86 (shared/src/rules/v10/classes_v10.c:154) | deob byte 4775554 "Rocketeer" speed: 0.86 | match |
| Rocketeer wall jump | 0 (shared/src/rules/v10/classes_v10.c:149) | deob byte 4775602 "Rocketeer" wallJ: false | match |
| Rocketeer wall jumps (max) | 0 (absent; shared/src/rules/v10/classes_v10.c:143) | deob byte 4775620 "Rocketeer" wallJM: 0 | match |
| Agent health | 105 (shared/src/rules/v10/classes_v10.c:169) | deob byte 4775932 "Agent" health: 105 | match |
| Agent speed | 1.2 (shared/src/rules/v10/classes_v10.c:172) | deob byte 4775982 "Agent" speed: 1.2 | match |
| Agent wall jump | 1 (shared/src/rules/v10/classes_v10.c:166) | deob byte 4775830 "Agent" wallJ: true | match |
| Agent wall jumps (max) | 3 (shared/src/rules/v10/classes_v10.c:167) | deob byte 4775847 "Agent" wallJM: 3 | match |
| Runner health | 120 (shared/src/rules/v10/classes_v10.c:188) | deob byte 4776431 "Runner" health: 120 | match |
| Runner speed | 1 (shared/src/rules/v10/classes_v10.c:191) | deob byte 4776497 "Runner" speed: 1 | match |
| Runner wall jump | 1 (shared/src/rules/v10/classes_v10.c:185) | deob byte 4776328 "Runner" wallJ: true | match |
| Runner wall jumps (max) | 100 (shared/src/rules/v10/classes_v10.c:186) | deob byte 4776345 "Runner" wallJM: 100 | match |
| Deagler health | 60 (shared/src/rules/v10/classes_v10.c:207) | deob byte 4776829 "Deagler" health: 60 | match |
| Deagler speed | 1 (shared/src/rules/v10/classes_v10.c:210) | deob byte 4776878 "Deagler" speed: 1 | match |
| Deagler wall jump | 0 (shared/src/rules/v10/classes_v10.c:203) | deob byte 4776923 "Deagler" wallJ: false | match |
| Deagler wall jumps (max) | 0 (absent; shared/src/rules/v10/classes_v10.c:198) | deob byte 4776941 "Deagler" wallJM: 0 | match |
| Bowman health | 100 (shared/src/rules/v10/classes_v10.c:224) | deob byte 4777225 "Bowman" health: 100 | match |
| Bowman speed | 1 (shared/src/rules/v10/classes_v10.c:227) | deob byte 4777275 "Bowman" speed: 1 | match |
| Bowman wall jump | 0 (shared/src/rules/v10/classes_v10.c:221) | deob byte 4777320 "Bowman" wallJ: false | match |
| Bowman wall jumps (max) | 0 (absent; shared/src/rules/v10/classes_v10.c:216) | deob byte 4777338 "Bowman" wallJM: 0 | match |
| Commando health | 100 (shared/src/rules/v10/classes_v10.c:241) | deob byte 4777621 "Commando" health: 100 | match |
| Commando speed | 1 (shared/src/rules/v10/classes_v10.c:244) | deob byte 4777671 "Commando" speed: 1 | match |
| Commando wall jump | 0 (shared/src/rules/v10/classes_v10.c:239) | deob byte 4777716 "Commando" wallJ: false | match |
| Commando wall jumps (max) | 0 (absent; shared/src/rules/v10/classes_v10.c:234) | deob byte 4777734 "Commando" wallJM: 0 | match |
| Trooper health | 100 (shared/src/rules/v10/classes_v10.c:259) | deob byte 4778050 "Trooper" health: 100 | match |
| Trooper speed | 0.9 (shared/src/rules/v10/classes_v10.c:262) | deob byte 4778100 "Trooper" speed: 0.9 | match |
| Trooper wall jump | 1 (shared/src/rules/v10/classes_v10.c:256) | deob byte 4777906 "Trooper" wallJ: true | match |
| Trooper wall jumps (max) | 3 (shared/src/rules/v10/classes_v10.c:257) | deob byte 4777923 "Trooper" wallJM: 3 | match |
| Survivor health | 150 (shared/src/rules/v10/classes_v10.c:281) | deob byte 4778489 "Survivor" health: 150 | match |
| Survivor speed | 0.8 (shared/src/rules/v10/classes_v10.c:284) | deob byte 4778539 "Survivor" speed: 0.8 | match |
| Survivor wall jump | 1 (shared/src/rules/v10/classes_v10.c:275) | deob byte 4778307 "Survivor" wallJ: true | match |
| Survivor wall jumps (max) | 100 (shared/src/rules/v10/classes_v10.c:276) | deob byte 4778324 "Survivor" wallJM: 100 | match |
| Infiltrator health | 90 (shared/src/rules/v10/classes_v10.c:297) | deob byte 4778855 "Infiltrator" health: 90 | match |
| Infiltrator speed | 1 (shared/src/rules/v10/classes_v10.c:300) | deob byte 4778904 "Infiltrator" speed: 1 | match |
| Infiltrator wall jump | 0 (shared/src/rules/v10/classes_v10.c:295) | deob byte 4778949 "Infiltrator" wallJ: false | match |
| Infiltrator wall jumps (max) | 0 (absent; shared/src/rules/v10/classes_v10.c:290) | deob byte 4778967 "Infiltrator" wallJM: 0 | match |
| Gravity (velocity.y step) (gravity) | 0.15 (shared/src/rules/v10/constants_v10.c:49) | deob byte 828608 (line 22609) gravity: 0.00015 = 0.15 per s | match |
| Jump velocity (jump_velocity) | 0.072 (shared/src/rules/v10/constants_v10.c:28) | deob byte 829622 (line 22648) jumpVel: 0.072 | match |
| Jump push (jump_push) | 0.1 (shared/src/rules/v10/constants_v10.c:30) | deob byte 829603 (line 22647) jumpPush: 0.1 | match |
| Jump height factor when not aiming (0.85) (jump_aim_slow) | 0.85 (shared/src/rules/v10/constants_v10.c:29) | deob byte 822446 (line 22383) aimJumpSlow: 0.85 | match |
| Crouch cut of the jump (crouch_jump) | 0.25 (shared/src/rules/v10/constants_v10.c:31) | deob byte 825238 (line 22491) crouchJump: 0.25 | match |
| Crouch speed (crouch_speed) | 7 (shared/src/rules/v10/constants_v10.c:32) | deob byte 825279 (line 22493) crouchSpd: 0.007 = 7 per s | match |
| Crouch height distance (crouch_distance) | 3 (shared/src/rules/v10/constants_v10.c:48) | deob byte 825220 (line 22490) crouchDst: 3 | match |
| Crouch lean (crouch_lean) | 0 (shared/src/rules/v10/constants_v10.c:33) | deob byte 825260 (line 22492) crouchLean: 0 | match |
| Crouch animation (crouch_anim) | 0.8 (shared/src/rules/v10/constants_v10.c:74) | deob byte 825166 (line 22488) crouchAnim: 0.0008 = 0.8 per s | match |
| Slide time (ms to s) (slide_time) | 0.35 (shared/src/rules/v10/constants_v10.c:60) | deob byte 835377 (line 22855) slideTime: 350 = 0.35 s | match |
| Slide velocity multiplier (player_slide_velocity_mlt) | 1.2 (shared/src/rules/v10/constants_v10.c:61) | deob byte 833651 (line 22798) playerSlideVelMlt: 1.2 | match |
| Terrain slide velocity multiplier (player_terrain_slide_velocity_mlt) | 0.4 (shared/src/rules/v10/constants_v10.c:62) | deob byte 833679 (line 22799) playerTerrainSlideVelMlt: 0.4 | match |
| Player height (player_height) | 11 (shared/src/rules/v10/constants_v10.c:8) | deob byte 833590 (line 22796) playerHeight: 11 | match |
| Leg height (leg_height) | 4.2 (shared/src/rules/v10/constants_v10.c:9) | deob byte 830612 (line 22677) legHeight: 4.2 | match |
| Head scale (head_scale) | 2 (shared/src/rules/v10/constants_v10.c:10) | deob byte 828801 (line 22616) headScale: 2 | match |
| Chest width (chest_width) | 2.6 (shared/src/rules/v10/constants_v10.c:6) | deob byte 824780 (line 22472) chestWidth: 2.6 | match |
| Chest scale (chest_scale) | 1.3 (shared/src/rules/v10/constants_v10.c:53) | deob byte 824759 (line 22471) chestScale: 1.3 | match |
| Leg scale (leg_scale) | 1.3 (shared/src/rules/v10/constants_v10.c:54) | deob byte 830641 (line 22678) legScale: 1.3 | match |
| Camera height (camera_height) | 1.5 (shared/src/rules/v10/constants_v10.c:24) | deob byte 824604 (line 22465) cameraHeight: 1.5 | match |
| Border height (border_height) | 500 (shared/src/rules/v10/constants_v10.c:23) | deob byte 824315 (line 22454) borderH: 500 | match |
| Ladder speed (ladder_speed) | 35 (shared/src/rules/v10/constants_v10.c:77) | deob byte 830073 (line 22665) ladderSpeed: 0.035 = 35 per s | match |
| Ladder decel (ladder_decel) | 0.97 (shared/src/rules/v10/constants_v10.c:47) | deob byte 830020 (line 22663) ladderDecel: 0.97 | match |
| Ground decel (ground_decel) | 0.99 (shared/src/rules/v10/constants_v10.c:46) | deob byte 828635 (line 22610) groundDecel: 0.99 | match |
| Air decel (air_decel) | 0.9996 (shared/src/rules/v10/constants_v10.c:45) | deob byte 822506 (line 22386) airDecel: 0.9996 | match |
| Slide decel (slide_decel) | 0.999 (shared/src/rules/v10/constants_v10.c:44) | deob byte 835354 (line 22854) slideDecel: 0.999 | match |
| Terrain decel (terrain_decel) | 0.99 (shared/src/rules/v10/constants_v10.c:43) | deob byte 836624 (line 22895) terrainDecel: 0.99 | match |
| Terrain slide decel (terrain_slide_decel) | 0.9996 (shared/src/rules/v10/constants_v10.c:42) | deob byte 836668 (line 22897) terrainSlideDecel: 0.9996 | match |
| Minimum decel (min_decel) | 0.0001 (shared/src/rules/v10/constants_v10.c:59) | deob byte 825738 (line 22507) decelMin: 0.0001 | match |
| Interpolation (interpolation) | 1.1 (shared/src/rules/v10/constants_v10.c:64) | deob byte 829270 (line 22631) interpolation: 1.1 | match |
| Aim slow (aim_slow) | 0.55 (shared/src/rules/v10/constants_v10.c:37) | deob byte 822469 (line 22384) aimSlow: 0.55 | match |
| Max frame delta (ms to s) (max_delta) | 0.033 (shared/src/rules/v10/constants_v10.c:27) | deob byte 825995 (line 22516) (Math.round(1000 / 30) = 33 ms) = 0.033 s | match |
| Ground acceleration (grA 0.042) x10 per s (player_speed 0.42) | 0.042 x 10 = 0.42 per s (shared/src/player.c:1752) | deob byte 771064 (setting def 0.042; line 20415); movement reads config.grA / 100 per ms (deob byte 9213746 (line 191624)) | match |
| Air acceleration (arA 0.0047) x10 per s (air_speed 0.047) | 0.0047 x 10 = 0.047 per s (shared/src/player.c:1753) | deob byte 771382 (line 20435); movement reads config.arA / 100 per ms (deob byte 9213820 (line 191624)) | match |
| Booster speed (2 per s) | 2.0 (shared/src/rules/v10/constants_v10.c:76; used at shared/src/player.c:1142) | deob byte 9237488 (line 192003) (0.002 per ms = 2 per s) | match |
| Player speed constant (player_speed 0.42) | 0.42 (shared/src/rules/v10/constants_v10.c:34); read by Strike and bots only | derived from grA 0.042 / 100 per ms x 1000 (row above) | match |
| Air speed constant (air_speed 0.047) | 0.047 (shared/src/rules/v10/constants_v10.c:36); read by bots only | derived from arA 0.0047 / 100 per ms x 1000 (row above) | match |
| Hit-box broad-phase padding (5) | 5 (shared/src/hitboxes.c:331, 333) | deob byte 828857 (line 22618) hitBoxPad: 5 | match |
| Speed cap on movement | none (shared/src/player.c has no speed clamp) | deob byte 831987 (line 22733) is the speedrun score-zone display limit (custUI), not a movement cap | match |
| Slipping speed (slipping_speed 0.3) | 0.3 (shared/src/rules/v10/constants_v10.c:35) | no 10.0.3 key (no slippingSpeed; the only "slipp" text is a tip); v7 config.js:2117 has 0.0003 per ms and no v7 code reads it | not found (dead in 10.0.3 and v7; doc/v10/constants-1003.md 1) |
| Terrain slide threshold (1.9) | 1.9 (shared/src/rules/v10/constants_v10.c:40) | no 10.0.3 key; the terrain test is y <= height (deob 192234), no slope threshold | not found (dead; constants-1003.md 2) |
| Terrain slip decel (0.99) | 0.99 (shared/src/rules/v10/constants_v10.c:41) | no 10.0.3 key; the terrain decel that is read is 0.99 (deob 191631) = terrain_decel (constants_v10.c:43) | not found (dead; constants-1003.md 3) |
| Slipping jump cooldown (0.5 s) | 0.5 (shared/src/rules/v10/constants_v10.c:39) | no 10.0.3 key; the jump sets jumpCooldown = jpC * 0 (deob 195251) = 0, as shared/src/world.c:415 | not found (dead; the behaviour matches; constants-1003.md 4) |
| Crouch slow (0.3) | 0.3 (shared/src/rules/v10/constants_v10.c:38) | 10.0.3 setting "Crouch Speed" crchSpd, def 0.3 (deob 20462), read at deob 191627 | match (constants-1003.md 5) |
| Collision padding (1.1) | 1.1 (shared/src/rules/v10/constants_v10.c:75) | deob 21171 var = 1.1, read as the config key collPad (deob 22480) | match (constants-1003.md 6) |
| Jump velocity formula: 0.072 x jumpMlt x attributes.jump x mode.jumpMlt (x mode.jumpVel) | shared/src/player.c:1286 | deob byte 9415374 (line 195256); mode.jumpVel: no 10.0.3 mode sets it (no `jumpVel:` in the mode table), so the factor is 1 | match |
| Real-movement jump factor 0.92 | shared/src/player.c:1287 | deob byte 9415660 (line 195258) | match |
| Jump push 0.1 with the same multipliers | shared/src/player.c:1289 | deob byte 9415816 (line 195261) | match |
| Jump height: crouch 0.25 and weapon jumMlt \|\| spdMlt, aim 0.85 | shared/src/player.c:1291-1293 | deob byte 9416022 (line 195262); aimVal ? 1 : 0.85 | match |
| Gravity step: gravity x config.gravMlt x weapon.gravMlt x mode.gravMlt (no gravity while stuck to a wall) | shared/src/player.c:1827 | deob byte 9217568 (line 191697) | match |
| Ranked end-game KR multiplier x2 | shared/src/rules/ruleset.c:37 (.kr_mlt = 2.0f) | deob byte 10695300 (line 221760) reward x 2 (not in a challenge) | match |
| Ranked needAll timer 60 s | shared/src/rules/ruleset.c:37 (.need_all_ms = 60000) | deob byte 10614043 (line 219964) | match |
| Ranked shutdown 120 s | shared/src/rules/ruleset.c:37 (.shutdown_ms = 120000) | deob byte 10698357 (line 221815) | match |
| Ranked: no kill streaks, no class XP | shared/src/rules/ruleset.c:37; rules_ranked_no_streaks / no_class_xp (ruleset.c:78-79) | deob 198604 (!isRanked on the killer's streak); deob 197794 (saveClassScores: !isRanked) | match (constants-1003.md 7) |
| Trigger action limit 8 | shared/include/triggers.h:187; shared/src/rules/ruleset.c:38 | deob byte 838025 (line 22957) triggerActionLimit: 8 | match |
| Entity limit 64 | shared/src/rules/ruleset.c:39 | deob byte 826706 (line 22543) entityLimit: 64 | match |
| Archetype limit 32 | shared/src/rules/ruleset.c:40 | deob byte 822759 (line 22397) archetypeLimit: 32 | match |
| Light unit factor (lightIntensityScaling = Math.PI) | shared/src/rules/ruleset.c:41 (M_PI) | deob byte 830752 (line 22682) Math.PI | match |
| End screen timer 25 s | shared/include/shared.h:1980 (GAME_END_TIMER 25.0f); ruleset.c:22 | deob byte 826625 (line 22540) endTimer: 25000 | match |
| End round slow motion 5 s | shared/src/rules/ruleset.c:23 (end_anim 5.0) | deob byte 826543 (line 22537) endAnim: 5000 | match |
| Vote forfeit timer 60 s | shared/include/shared.h:1807 (FORFEIT_TIMER_S 60) | deob byte 839024 (line 22997) | match |
| Vote forfeit disconnect grace 30 s | shared/include/shared.h:1808 (FORFEIT_GRACE_S 30) | deob byte 750382 (line 19428) (voteForfeitDisconnectGrace) | match |
| Raid time limit 25 min | shared/src/modes/modes.c:251 (game_time 1500.0f) | deob byte 4893830 (line 118456) | match |
| Stalker HP = floor(200 + 50 x (active - 1)) | shared/src/modes/mode_rules.c:659 | deob byte 10506695 (line 217943) (pickStalker) | match |
| Comp warmup spawn spread (getPointSpread(.., 10, ..): the spawn itself, rounded) | FIXED: shared/src/rules/ruleset.c:42 (.comp_spawn_spread = 10.0f), used at shared/src/modes/mode_round.c:653 (was SPAWN_SPREAD_RADIUS 40, mode_round.c:564) | deob byte 10471759 (line 217148) (spread 10 less 10 = no offset) | mismatch (fixed) |
| Non-comp spawn spread (spread + 7) | shared/src/modes/mode_round.c:719 (best->spread + 7.0f) | deob byte 10471910 (line 217150) | match |
| Team round wins counted outside best-of (teamRoundWins[getWinTeam(true)]++) | shared/src/modes/mode_round.c:373-382 (FIXED: the else branch of mode_end_game; was counted only under best_of, mode_round.c:353-368) | deob byte 10688117 (line 221603) (else branch of bestOf) | mismatch (fixed) |
| Battle Pass points per public match: base min(20, floor(score x rewardMlt x 0.01)) for a public match; play term 30 x min(1, floor(playTime / (gameTime - max(0, gameTimerP)) x 10) / 10) only when !isEmptyLobby and not raid (never, as isEmptyLobby is never false) | FIXED: bp_match_points (shared/src/season.c), sent for public matches only by server/src/accounts.c:1158-1167; was base + binary +30 for every counted commit | deob byte 10696099 (line 221771) (x 30, gated by !isEmptyLobby at byte 10695730, line 221768) | mismatch (fixed) |
| Mode index default 0 = Free for All | net-bridge/krunkerProxy.ts:902; v10Proxy.ts:633; v10Translate.ts:815 (MODE_ID_FFA = 0) | deob byte 4925147 (line 119361) (first entry of the mode table) | match |
| Map index default 0 (Burg) | net-bridge/krunkerProxy.ts:901; shared/include/krunker_maps.h:23 (X(0, "Burg")) | deob 160271 (filter value 0 = Burg); deob 188614 and 257421 (index 0 is Burg) | match (constants-1003.md 8) |
| Tick rate default 30 Hz | net-bridge/krunkerProxy.ts:16 (DEFAULT_TICK_RATE = 30); proxy.ts:15 | deob byte 10397678 (line 215719) | match |
| INIT_GAME arguments: eventIndex 0, isOfficial false, isComp false, accType 1, sync gor 1 (init 4th argument true) | net-bridge/v10Translate.ts:1227-1250 (buildInitArgs) | positions read by the 10.0.3 handler (deob 256748-256856) and the create-game broadcast (deob 261893); isOfficial false is right for Box Office (not a krunker.io official game); the audit's old "isOfficial true" misread index 13 | match (constants-1003.md 9) |
| Session placeholders gameTimer 0, challMode 0 | net-bridge/v10Proxy.ts:644-645 | not a 10.0.3 constant: TIMER (v10Proxy.ts:461) and ENTER (593) overwrite them; the START_GAME order matches (deob 242075) | placeholder (constants-1003.md 10) |
| Native protocol ahNum key (KRUNKER_AHKEY 0x3) | shared/include/krunker_build.h:19; net-bridge/krunkerProxy.ts:352 | deob byte 4658927 (line 111514) (10.0.2: 1597, v10.js:98200) | match |
| Weapon trail (bullet trail setting and tracer) | covered upstream: commit 60217454 ("Bullet trail: the server sends the official shot trail"; Sniper Rifle .trail, Charge Rifle .trail_charge, checked by weapons_v10_test) and 2d7113ac (the trail setting) | not re-audited here (another owner) | covered |
| Class leap (all 16 classes) | no leap set in shared/src/rules/v10/classes_v10.c (default false) | all 16 are leap: false in the 10.0.3 class table | match |

## Not audited here

- Render-side and UI values (lean, bob, view-model offsets, HUD, skins) and the scene offset noted above.
- The ten former unverified rows are settled in `doc/v10/constants-1003.md`. The bridge's live INIT is exercised by `make playable-check`, which this audit did not run.
