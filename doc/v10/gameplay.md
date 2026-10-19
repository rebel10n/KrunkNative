# v10.0.2 gameplay changes vs v7.1.4

Scope: physics constants, movement, classes, weapons, attachments, streaks/effects, modes, scoring, spawns, hit
detection. "v10:N" = line N of the renamed bundle `~/.cache/kn-v10/v10.js` (made by
`~/.cache/kn-v10-tools/rename.py` from `~/Downloads/v10.0.2.js`; the renamer keeps line numbers, so the
original file has the same lines). "v7 f:N" = `/home/user/jails/kru/src/Krunker_v7.1.4/src/f:N`.

How it was compared (repeatable):

| tool (`~/.cache/kn-v10-tools/`) | what it does |
|---|---|
| `extract.js <file> <anchor names...>` | acorn parse (no eval). Prints every array literal whose elements have `name: <anchor>`. Resolves identifiers, babel `_69(obj,k,v)` defineProperty chains (expression and statement level) |
| `v7modes.js <v7 src>` | the 37 `src/modes/*.js` `module.exports` objects, in `data/modes.js` order |
| `fielddiff.js a.json i b.json j key [skipRe]` | field-by-field diff of two extracted tables |
| `fnnums2.py v7.js v10.js a b v7config.js` | per `this.X = function`: numeric literals and property names that differ (inlines v7 `config.X` and v10 top-level `var _N = <num>`) |
| `bodycmp.py` | the same for one pair of line ranges (used for v7 `procInputs` vs v10 obfuscated `Wjm9aj`) |
| `settingsdiff.py` | custom-game setting entries `{name, varN, def, defC, min, max}` |

Raw outputs are in `~/.cache/kn-v10/ext/` (`diff-weapons.txt`, `diff-classes.txt`, `diff-modes.txt`,
`player-fn.txt`, `players-fn.txt`, `game-fn.txt`).

Summary: v10's core movement code and physics constants are the same as v7.1.4 (but `crouchLean`, 0 in
v10). The gameplay changes that matter are:

1. Server hit detection now always uses posed oriented-box hitboxes ("complex hitboxes"), with a hurtbox scale.
2. A weapon balance pass (10 weapons).
3. Two new secondaries: Boulder (id 29) and Compressor (id 30).
4. Three class HP changes, plus a level-1 unlock gate on every class except Triggerman.
5. Stalker mode rework.
6. Raid time limit raised to 25 min.
7. Ranked-only rules (x2 KR, no streaks, 60 s join window, 120 s shutdown).
8. Comp-warmup spawn spread cut from 40 to 10.

## 1. Physics constants

v10 inlines most v7 `config.*` movement constants as top-level `var _N = <value>` (v10:16404-19476) and then
re-exports them in the frozen config object (v10:19477-20114). Every movement value is unchanged. A whole-object
numeric diff of that config against v7's `config.js` finds one gameplay constant that differs: `crouchLean`
(v10:19602 `0`, v7 config.js:2164 `-(Math.PI * 0.1)`): v10 neither leans a crouched player's model nor its hit
boxes (`shared/src/rules/v10/constants_v10.c`). The other differing keys are menu / economy values
(bppCapDay, giftMinLVl, marketMinLVl, tradeMinLvl, minKDFCall, rewardTime, hostTransferLimit, triggerActionLimit).

| constant | v7 (config.js) | v10 | evidence |
|---|---|---|---|
| jumpVel | 0.072 (config.js:2129) | 0.072 | `'jumpVel': 0.072` v10:19748; jump() inlines `0.072 *` v10:172018 |
| jumpPush | 0.1 | 0.1 | `var IIíîíïì = 0.1 * (...jumpMlt...)` v10:172021 |
| crouchJump / aimJumpSlow | 0.25 / 0.85 | 0.25 / 0.85 | `0.25 * crouchVal`, `aimVal ? 1 : 0.85` v10:172022 |
| gravity | 0.00015 | 0.00015 | `var _7387 = 0.00015` |
| crouchSpd | 0.007 | 0.007 | `var _7422 = 0.007`, `'crouchSpd': _7422` v10:19603 |
| crouchDst | 3 | 3 | `'crouchDst': 3` v10:19600; `updateHeight` inlines `3 * crouchVal` v10:170860 |
| playerHeight / legHeight / headScale | 11 / 4.2 / 2 | 11 / 4.2 / 2 | `_7418 = 11`, `_7420 = 4.2`; `9 + 2 * headMlt` v10:170866 (= 11 - 2 + 2*headMlt) |
| slideTime / playerSlideVelMlt / playerTerrainSlideVelMlt | 350 / 1.2 / 0.4 | 350 / 1.2 / 0.4 | `350 * (slidTime...)`, `(onTerrain ? 0.4 : 1.2) * slA` v10:171975-171976; `this` is the players manager (onTerrain undefined), so the 0.4 never applies: KN uses 1.2 on terrain too, as v10 and the official servers do |
| delta floor (procInputs input[1], ms) | `max(0.1, min(input[1], 33))` (v7.1.4: 0.1 ms; NaN, 0 and negatives too) | `max(1 * deltaMlt, min(input[1], 33))` (1 ms at deltaMlt 1; NaN, 0 and negatives too) | `Math.max(1 * (config.deltaMlt \|\| 1), Math.min(input[1], dltMx))` v10:169581-169582; input[1] = `Math.round(frame ms * deltaMlt)` game.deob.js:221748 (the client scales it, main.c) |
| airDecel, groundDecel, slideDecel, ladderSpeed, decelMin | v7 values | same | `_7416 = 0.9996`, `_7415 = 0.99`, `_7413 = 0.035`, `_7081 = 0.0001` |
| hitBoxPad (game setting) | def 0.6, defC 0.1 (config.js:623) | def 0.6, defC 0.1 | v10:16921-16930 |

Keys missing from v10's config object (playerSpeed 0.00042, airSpeed 0.000047, EPM, cornerGlide,
terrainGravityMlt, terrainSlideThreshold, terrainSlipDecel, landPull, landPullV, slippingSpeed,
playerSlippingJumpCooldown, wpnSpin, eloK, eloPer, rankedCooldown, rankedDeduction, minRankedLevel, ...):
no v7 game code reads any of them (`grep -rn` over v7 `src/` finds only the `module.exports.X =` lines;
EPM is read only in v7 `index.js:8264` editor-earnings UI). They are dead constants, not physics changes.
The ranked ones moved to the ranked service (see ranked.md).

`movement.js` (the 17 movement/vehicle presets) matches v7 numerically (`bodycmp.py` over v7
`data/movement.js:1-649` vs v10:15899-16400; the only differences are constant-folded `0.07 * 1.4285` /
`0.07 * 0.75` = v7 `0.099995` / `0.0525`).

## 2. Movement code (player.procInputs, players.*)

v7 `Player.procInputs` (player.js:859-2012) is v10's `this['Wjm9aj']` (v10:169577-170182; the method name is
obfuscated). Numeric diff: only differences are the Stalker block below plus a crouch-bob constant.

| change | v10 evidence | v7 |
|---|---|---|
| crouch bob while grounded: `bobAnimY -= 0.00112 * dt` (v7: `config.crouchAnim * 1.4 * dt` = 0.0008*1.4 = 0.00112, same value constant-folded) | v10:169705 | player.js:1141 |
| `isStalker` getter (`mode.teamOptions[team-1] == "Stalker"`) replaces inline team checks in step/jump sounds | v10:168995 | players.js:1617, 1663 |
| Stalker leap: the aim input (`input[6]`, rMouse) while grounded / stuck to a wall / within the 150 ms coyote time, not in replays and not while attached to an entity, cooldown 3000 ms, impulse 0.15 along the look vector (x,z,y); the client keeps it as the input's `velObj` | v10:169664-169685, coyote `stalkerCoyoteTime = 150` v10:170078 | none (new) |
| Stalker wall stick: pressing jump against a wall in the air sticks (`stuckToWall`, `stuckToWallDir` 1-4): no gravity and velocity 0 while stuck (v10:169754-169755), no movement (`defaultMovement && !stuckToWall`, v10:169764), no wall jump (v10:170155); the next jump press releases with a 0.03 push off the wall | v10:170153-170154 | none |
| Stalker: wall-jump allowed on `notWallJ` objects (`!obj.notWallJ \|\| this.isStalker`) | v10:169909 | player.js wall-jump collision |
| Stalker: hidden from render after 300 ms without moving (`isStalker && noMovT >= 300`) | v10:169790 | none |
| gravity also scaled by `weapon.gravMlt` (new weapon field, used by Boulder 1.7) | v10:169754 `(this.weapon.gravMlt \|\| 1)` | player.js:1224 (no weapon term) |
| weapon `rShoot`: primary fire also triggered by the right/aim input (used by Compressor); `shoot()` gets the aim input as its last argument (v10:170139) for `physPull` | v10:170122 `_59781 \|\| this.weapon.rShoot && _59782` | none |
| `jump()` sets `didStickToWall = true`, counts a skin stat `jumps` | v10:172012-172015 | players.js:1692 |
| player-player interact range: `getD3D(..y+5.5.., ..y+5.5..) <= 13.2` (v7 used playerHeight/2 and 2.4*scale, same values) | v10:170175 | player.js:1993 |
| `poseData.bodyY` lowers the eye/hitbox height (emotes/poses) | v10:170861 | none |
| `markPosition` (ranked/comp key bind: ping a world position for the team) | v10:170189-170610, bind v10:221125 | none |

Mode fields that are new in v10 movement code: `mode.canSlide(player)` overrides `noSliding` (v10:171971),
used by Stalker (`canSlide: p => p.isStalker`).

Zombie perks (`perks.includes(1..6)`, max 4) and effects (`attributes.rooted` etc.) are unchanged from v7. The
effects table is the same 10 entries (Boosted ... Strength, v10:162141 vs v7 data/effects.js:1).

## 3. Hit detection (largest gameplay change)

v7: by default players are an axis-aligned box: x/z +-(scale + hitboxPadW), y feet .. adjustedHeight +
hitboxPadH. Headshot if hit y > top - headScale*headMlt; legshot below legHeight - crouchDst*crouchVal
(players.js:2936-2960, 3054-3065). The raycast-on-mesh path is only used when `player.newHitboxes` is set.

v10 (players.shoot v10:172884-172906, melee v10:172468, projectiles v10:180106-180191):

| step | detail | evidence |
|---|---|---|
| broad phase | lineInRect against an AABB x/z +-(scale + 5), y .. y + ah + 5 (fixed padding 5, not hitBoxPad) | v10:172895 |
| pose | `_40376(game, player, cfg, state)` poses a per-player THREE hierarchy `complexHitboxes` from the lag-comp state (x,y,z,xD,yD,sV,cV,stepChase,weapon) | v10:117728-117831 |
| hierarchy | `hitboxHoldObj` at (x, y + \|6 sin(sV) stepAnim\|, z) (no bob for a class `mdl`), turned y by xD - 2 sin(sV) stepAnim > `upperBodyHoldObj` at legHeight-crouchDst*cV, pitched by crouchLean*cV + 0.5*yD, containing `headHitbox` (headScale cube at playerHeight - headScale/2 - legHeight + headScale(headMlt-1)/2, scale hurtboxScale*headMlt), `torsoHitbox` (chestWidth x (playerHeight-headScale-legHeight) x chestScale, scale hurtboxScale) and `armHoldObj` (pitched another 0.5*yD, at playerHeight - cameraHeight - legHeight + recoilAnimY*recoilYM*animMult) > `arms` (the viewmodel sway pose) > two arm holders (rotation order YXZ, two-bone IK to the weapon's hold offsets, `_40318` v10:117609) with upper arm / elbow connector / lower arm boxes; `legsHoldObj` with 4 holders in the order right standing, left standing, right crouched, left crouched (`userData.active` by cV), each pitched by `sin(sV)*stepAnim*(1 - cV*crouchAnimMlt) * (1, 3: +1; 0, 2: -1) * 9 (+ -0.6 crouched)`; every box mesh scaled by hurtboxScale | v10:169443-169498, 117609-117831 |
| narrow phase | `_40442`: ray from the eye (y + height - cameraHeight) against OBBs (`_40272`, center/halfSize/rotation; a box's centre is its mesh's world position). Head first, then torso; the six arm boxes only when the torso missed (stop at the first hit); then the active leg pair (stop at the first hit); the nearest of those. If the target's live velocity is not zero, every box's half size grows by +1 before its scale | v10:117833-117946 |
| result | `[(distance/range).toFixed(4), hitType, hitboxName]`; `[null, "nearmiss"]` if no box. Every arm box carries hitType `torsoshot`: nothing in 10.0.2 produces `armshot` (it only appears in the damage checks) | v10:117609-117663, 117943-117946 |
| damage | headshot x `weapon.hsMlt \|\| 1.5` (unless `noHeadShot`); `legshot` (and the unused `armshot`) x `weapon.lsMlt \|\| 0.5`; `config.headshotOnly` zeroes the rest. Arms are body damage | v10:172979 |
| projectiles | the same boxes along each tick's step (`_40442(..., true)`: from the projectile, along its xD/yD); a `hitBox` projectile first gates on its sphere around the body centre instead of the padded AABB, then still needs a box hit | v10:180115-180143 |
| prop mode | keeps the v7 AABB path (scale+hitboxPadW / ah+hitboxPadH) | v10:172886-172892 |
| hurtboxScale | new game setting (`sub: "Hitboxes"`, def 1, 1..2, v10:16931) times new class/player config (`sub: "Hitbox"`, cat 1, v10:17699); getter v10:169065 | none in v7 |

Port: `shared/src/hitboxes.c` (`shared/include/hitboxes.h`) is the hierarchy and the ray in plain C over
the lag-comp state (PlayerStoredState carries sV, stepChase, xD, yD, cV and the weapon, as v10's storeState),
used by hitscan (`combat_trace`), projectiles (`projectile_cast`) and the Piss stream in every mode but
Prop Hunt (`hitboxes_active`). The broad phase is v10's lineInRect (`!(tmax < 0) && !(tmin > tmax) && tmin`,
then `<= 1`): a ray or step that starts within 5 of the player passes it. Melee keeps the padded AABB in
v10 too (v10:172468). Arms are torsoshot, so an arm hit is body damage. Not ported: the arms' viewmodel sway
reads client animation values off the live player (inspect, lean, swap, jump / land / idle bob, recoil tweens):
taken as 0, they move the arm boxes by tenths of a unit; and v10 keeps the last weapon's arm boxes per player id
(a player whose states never held a weapon is unhittable there), KrunkNative poses the arms from the state's
weapon only.

## 4. Classes (v10:101961-102300 vs v7 data/classes.js)

| class | field | v7 | v10 |
|---|---|---|---|
| Run N Gun | health | 90 (classes.js:53) | 100 (v10:102001) |
| Spray N Pray | health | 180 (classes.js:72) | 170 (v10:102022) |
| Agent | health | 110 (classes.js:175) | 105 (v10:102131) |
| all except Triggerman | `minRec: 1`, `req: _34270` (unlock gate: level >= 1; customOnly/premiumOnly logic as v7 `f791`) | only Hunter, Vince, Detective, Bowman | v10:101980-102300 |

The 16 classes keep their order, loadouts, speeds and wall-jump values. Level 1 is every account's starting
level, so the gate only matters for guests without an account (`p && p.level >= minRec`) (guess: this is how
v10 locks guests into Triggerman, consistent with the new `guestKRCap`/`guestScoreCap` in config).

## 5. Weapons (v10:98982-101959 vs v7 data/weapons.js)

31 weapons (v7: 29). Ids 0-28 unchanged in order and meaning. Only the stats below changed. Every other field
of the 29 old weapons is identical; `req` became a shared function `_34256` that adds `noComp` handling
equal to v7 `f701`.

| id | weapon | field | v7 (weapons.js line) | v10 (v10 line) |
|---|---|---|---|---|
| 1 | Assault Rifle | ammo | 28 | 30 |
| 1 | Assault Rifle | rate | 120 | 115 |
| 1 | Assault Rifle | dmgDrop | 5 | 8 |
| 2 | Pistol | swapTime | 260 | 350 |
| 3 | Submachine Gun | ammo | 30 | 24 |
| 3 | Submachine Gun | dmgDrop | 12 | 9 |
| 6 | Machine Gun | ammo | 60 | 50 |
| 6 | Machine Gun | dmg | 18 | 17 |
| 6 | Machine Gun | rate | 130 | 125 |
| 7 | Semi Auto | reload | 1500 | 1350 |
| 7 | Semi Auto | rate | 150 | 145 |
| 9 | Akimbo Uzi | reload | 1300 | 1200 |
| 9 | Akimbo Uzi | dmg | 15 | 16 |
| 9 | Akimbo Uzi | hsMlt | 1 | 1.5 |
| 10 | Desert Eagle | swapTime | 200 | 350 |
| 10 | Desert Eagle | hsMlt | 1.2 | 1.4 |
| 14 | Famas | pj_dmg | 28 | 26 |
| 14 | Famas | burstR | 90 | 85 |
| 14 | Famas | ammo | 30 | 24 |
| 14 | Famas | rate | 280 | 330 |
| 14 | Famas | reload | 1200 | 1500 |
| 14 | Famas | dmgDrop | 5 | 8 |
| 18 | Blaster | pj_dmg | 35 | 45 |
| 21 | Tehchy-9 | swapTime | 200 | 350 |

Weapon entry lines: v7 weapons.js 111 (AR), 225 (Pistol), 328 (SMG), 651 (LMG), 763 (Semi), 992 (Uzi),
1085 (Deagle), 1554 (Famas), 1891 (Blaster), 2140 (Tehchy); v10 99088, 99202, 99305, 99628, 99720, 99949,
100042, 100483, 100820, 101069.

### New weapons

| id | name | key fields (v10) | role |
|---|---|---|---|
| 29 | Boulder (v10:101808) | `src weapon_30`, `type 3`, `secondary`, `noAim`, `canThrow false`, `transp`, `spdMlt 0.85`, `gravMlt 1.7` (new field: heavier gravity while held), `jumMlt 0.88`, `swapTime 200`, `spread 120`, no damage fields | carry object. No `req`, not referenced by any class/mode in the bundle (guess: given by map logic/KrunkScript `giveWeapon`). |
| 30 | Compressor (v10:101859) | `src weapon_31`, `type 1`, `secondary`, `minRec 45`, `req`, `noDmg`, `noComp`, `dmg 1`, `ammo 1`, `reload 2000`, `rate 450`, `range 40`, `physRang 50`, `physPow 0.1`, `physPull` (new: with the aim input held the impulse sign is -1), `nonObjImpulse` (new weapon-level flag, v7 was mode-only), `rShoot` (new: the aim input fires it), `innac 110`, `spdMlt 1.05` | selectable secondary (level 45), a self-impulse: `noDmg` traces nothing (no objects, players, AI or terrain, no "s" stat), and `nonObjImpulse` makes the physPow ray a fixed push of the shooter along the shot, physPow x impulseMlt (`noImpulseDist`: 1), whatever it points at; the left click pushes the shooter back, the right click (rShoot + physPull) pulls it forward. Nobody else and no object is moved (the object push is skipped under nonObjImpulse) (v10:172807-172840, 172853). `noDmg` keeps it out of `onlyWeapons`. |

Table post-processing is unchanged except `onlyWeapons` also skips `noDmg` (v10:101957 vs weapons.js:2972), and a
new `mysteryBoxRarities = [35, 30, 20, 10, 5]` (v10:101948) for zombies' mystery box tiers (the matching weapon
fields `magReserve`, `mysteryBoxR`, `canUpgrade` already exist in v7).

### Nettspend balance (KrunkNative, not v10.0.2)

The Nettspend (class 16) and its weapons are KrunkNative additions (doc/v7/decisions.md "Nettspend class"), so
v10.0.2 has no value to compare with. This is a KN balance change, made from the bot bench (below) and written in
the weapon entry (`shared/src/weapons/kn_money_spread.c`, with the class note in `classes_v10.c`). Only the Money
Spread's own entry changed. The Drank (`kn_drank.c`), the class table and the shared weapon table are untouched.
The balance went through three versions: "first" (the KN values before 2026-10-18), "second" (the first tune
of this change, a flat falloff, not in main) and "now" (this one).

Bench: the bots play Kanji FFA with 2 bots per class (14 classes), 6 seeds x 300 s for the field; each
Nettspend-vs-class duel gets 3 seeds x 180 s (13 classes, the Nettspend mirror left out). The harness is a scratch
file (not in the repo).

Money Spread values (the other fields are unchanged: the 8 bills a throw, `no_auto`, the 180 ms swap, the 1.2 s
reload, the fan's pitch, no knockback, and every Drank value: 40 sips, 4 a sip, 60 ms, 1.4 s pour-up, puddles at
2.5 a 250 ms tick):

| Money Spread field | first | second | now |
|---|---|---|---|
| `damage` (a bill) | 7 | 18 | **14** |
| `damage_drop` (fall over `range`) | 7 | 4 | **8** |
| `range` (units) | 60 | 120 | 120 |
| `ammo` (throws a magazine) | 5 | 8 | 8 |
| `rate` (ms between throws) | 420 | 250 | 250 |
| `custom_spread` yaw (pattern x) | +-1.1 .. +-0.12 (about +-20 degrees) | x0.4: +-0.44 .. +-0.048 (about +-8) | x0.24 of first: +-0.264 .. +-0.0288 (about +-5) |
| point blank, all 8 bills in (8 units) | 56 | 144 | **112** |
| one bill at 8 / 47 / 120 units | 6.1 / 1.5 / 0 | 17.7 / 16.4 / 14 | 13.5 / 10.9 / 6 |

Field results, Nettspend (class 16), 6 seeds x 300 s (seeds 7-12 for the second and now):

| field | first | second | now |
|---|---|---|---|
| kills / deaths | 18 / 534 | 473 / 491 | 446 / 489 |
| KDR | 0.03 | 0.96 | 0.91 |
| KDR, seeds 7-12 | not run | 0.94 | 0.93 |
| damage per death | 6.6 | 99.8 | 91.8 |
| rank of the 14 classes by KDR | 14th | 9th | 9th |

Duels, Nettspend vs each of 13 classes, 3 seeds x 180 s:

| duels | first | second | now |
|---|---|---|---|
| Nettspend kills / other side's kills | 7 / 50 | 176 / 177 | 173 / 181 |
| runs where Nettspend got 0 kills (of 39) | 35 | 0 | 0 |
| classes where one side got 0 kills in all 3 runs | 10 (Nettspend) | 0 | 0 |

What the bench showed. The Drank's cup was not the limit: bots pour about 2.6 sips a life, so a 60-sip cup gave the
same results to the unit, and so did a 0.8 s Money Spread reload. Most of the output was the Money Spread, and most
of its hits came from 60 units or more (a bot engages a weapon inside its `range`, here up to 120). The second
version's flat falloff kept about 15 a bill at 60+ units, so its reach did the work. A full fan of 144 at point blank
and the flat falloff were the two faults. The now-values cut the bill to 14 and steepen the fall to 8 over 120: a bill
at 60+ units averages 8.6 in the bench (15.2 before), and a bill at 8 units keeps 13.5.

Why the fan narrowed. With the falloff steepened, the fan at the second version's width gave a field KDR under the
band, and raising the bill to hold KDR pushed the point-blank total past the cap. Narrowing the yaw brings more bills
onto a target at range (Money Spread hits in the six field runs rose from 3025 to 4350 on about the same throws, 1222 and 1261), so the KDR comes back at the same damage
numbers. Variants tried (field KDR, 6 seeds; "yaw" is the fan width against the second version, 1.0 = +-8 degrees):

| bill / drop over 120 | yaw | field KDR | point blank |
|---|---|---|---|
| 13 / 9 | 1.0 | 0.47 | 104 |
| 14 / 6 | 1.0 | 0.68 | 112 |
| 14 / 8 | 1.0 | 0.64 | 112 |
| 15 / 6 | 1.0 | 0.71 | 120 |
| 15 / 8 | 1.0 | 0.74 | 120 |
| 16 / 8 | 1.0 | 0.78 | 128 |
| 16 / 9 | 1.0 | 0.77 | 128 |
| 14 / 8 | 0.8 | 0.67 | 112 |
| 14 / 9 | 0.7 | 0.82 | 112 |
| 14 / 9 | 0.6 | 0.84 | 112 |
| **14 / 8** | **0.6** | **0.91** (seeds 7-12: 0.93) | **112** |
| 15 / 9 | 0.6 | 0.89 | 120 |
| 15 / 8 | 0.6 | 0.96 | 120 |
| 14 / 8 | 0.5 | 0.92 | 112 |
| 16 / 8 | 0.5 | 1.04 | 128 |
| 14 / 8, rate 200 ms | 0.6 | 0.93 | 112 |

Risk to check in play. The bench has no human players: a human's aim lands bills differently from a bot's, and the
hit rate is the lever that set the fan width. Check the point-blank full fan (112, down from 144) and the 60+ unit
bills (about 8.6, down from 15) in a real match. The client's bill burst (`VFX_BILL_CONE` in `client/src/vfx.c`) is
its own cone and was not changed.

### Attachments, streaks, effects

| table | v10 | result |
|---|---|---|
| attachments (Reflex Sight, Acog Scope) | v10:178097 | identical to v7 data/attach.js |
| killstreaks (7: Nuke ... War Machine) | v10:178405 | identical except Bombardment lost `devOnly: true` (v7 streaks.js:82, now usable) |
| effects (10) | v10:162141 | identical |

## 6. Modes (v10:104079-105960, list at v10:105413 vs v7 data/modes.js)

Same 37 modes, same order and ids (0 ffa ... 36 imp); no new mode objects. Changes:

| idx | mode | change | evidence |
|---|---|---|---|
| 10 | gun (Gun Game) | `weaponOrder` adds 28 (Charge Rifle) after 0 (Sniper): `[2,27,16,10,4,11,21,5,3,9,1,14,6,0,28,18,7,8,13,22,12]` | v10:105580 |
| 15 | stalk (Stalker) | now `event: true`, `tag: Event`; `canSlide: p => p.isStalker` (only the stalker slides); `wallJ true, wallJM 100, wallJD 0.2, wallJV 0.04, wallJYV 1.2`; Stalker model `opacity 0.1`; `onKill`: a kill by the stalker sends JUMP_SCARE to the victim and heals the stalker 50 (changeHealth -50); `onMelee` is now a damage factor (v10:172543): the stalker's knife on someone else heals it 10 and deals x0.6, any other knife hit x0.8 (v7: the stalker's swing took the victim's whole health and healed 40); `update`: once no wait timer is left, a Living player who stays within 200 units of their camp origin for 20 s gets effect 5 (Dizzy) until 2 s outside, coming back within 10 s re-applies it, origin resets after 10 s away; pickStalker: Stalker HP = floor(200 + 50 * (active players - 1)) (v7 fixed 400), packet STALKER_PICKED ("You are the Living" popup) to others; plus the stalker's own moves in section 2 (leap, wall stick, coyote time, any wall for wall jumps) | v10:104350-104437, 172543, 188459-188474 (v7 modes/stalk.js, game.js:1537) |
| 23 | raid | `gameTime` 360000 -> 1500000 (25 min); `noProfileStats`; tracks `runPeakPlayers`; onEnd raid challenges (rnodmg, r1hp, rnogun, rsolo, rsolofull; map 43 needs `runProg.bthrone`); `onAIDeath` -> challenge `aik` | v10:104536-104613 |
| 27 | krank | `party: true` and Party tag removed (now a normal mode) | v10:104640 |

v7 has two near-identical mode tables (`src/modes/` for social/editor and `src/gameModes/` for the game). v10
ships one. The diff above is against `src/modes/`; KrunkNative ports `gameModes/`, which matches `modes/` in
the fields above (checked: v7 `gameModes/stalk.js` also lacks the v10 wall-jump/canSlide fields).

Custom-game settings: same set as v7 plus `hurtboxScale` (game, "Hitboxes") and per-class `hurtboxScale`
("Hitbox"), and `webhook` (string, host type 1, only kept when `isComp`, v10:17192, 187258). The vehicle
settings dr/aDr/tL/... are the v7 Vehicle movement preset (movement.js:472), not new.

## 7. Scoring and rewards

| item | v7 | v10 | evidence |
|---|---|---|---|
| players.score() | as v10 | unchanged; level-up medal uses `_7340 = 0.03 * sqrt(score)` (v7 `config.levelDiv` path, same) | v10:173662-173704 vs players.js:4085 |
| pub score cap | 13000 | 13000 | v10:173675 |
| end-game KR | min(premium ? 40 : 30, floor(score*rewardMlt/100)), x1.5 challenge mode | same, plus floor(2 x reward) when `isRanked` (after the cap, not in challenge mode) | v10:191170-191171 vs game.js:4584-4587 |
| battle-pass points | +30 if playTime >= half the game | +30 * min(1, floor(playTime/gameTime*10)/10) (proportional in 10% steps), not in raid | v10:191174-191179 vs game.js:4593-4596 |
| team round wins | only counted for best-of games | also counted outside best-of when a team wins (`teamRoundWins[winTeam]++`) | v10:191057-191059 |
| killstreaks | `mode.noStreaks \|\| config.noStreaks` | also off when `isRanked` (the killer's streak count only; deathStreak still counts, v10:174051) | v10:174076 |
| class XP | saved in pub games | not saved in ranked (`isPubType() && !isRanked`) | v10:173631 |
| kill rewards | none | event `killPools` (weighted skin drops during live events, `getKillReward`) | v10:190436-190539 |
| end screen | — | `showRaidPromo` 50% in non-raid pubs (END_GAME extra field) | v10:191274 |

## 8. Spawns and match flow

| item | v7 | v10 | evidence |
|---|---|---|---|
| getSpawnPoint comp warmup spread | getPointSpread(..., 40, ...) | 10 | v10:188049 vs game.js:1024 |
| getSpawnPoint otherwise | — | identical algorithm (visibility count, then max summed distance, flag/domzone terms, `spread + 7`) | v10:188000-188050 |
| game state machine | implicit | explicit enum INITIALIZING 0, IDLE 1, WARMUP 2, IN_PROGRESS 3, POST_MATCH 4, ENDED 5 (`setGameState`) | v10:186518-186525, 191761 |
| needAll timer | afkTimeout or 300000 | 60000 when ranked; if lobby players with a chosen class (`lobbyPlayers[*].c`) < minPlayers: everyone is kicked with "Ranked Game: ..." and the match ends (endIndex 3); players who never picked a class get `onRankedPlayerFailedToJoin` | v10:189820, 191376-191410 |
| shutdown after end | config.shutdownTimer | 120000 when ranked | v10:191217 |
| reconnection | — | `allowsReconnection()` = ranked && WARMUP..POST_MATCH | v10:191796 |
| vote forfeit | exists (game.js:4435, 4880) | voters = team members not bypassed and disconnected <= 30 s (`voteForfeitDisconnectGrace 30000`); `voteForfeitTimer 60000`; win = ceil(bestOf/2) rounds to the other team | v10:186459-186470, 190928-191004 |
| `freezeOthers` | — | new `applyFreezeOthers` (guess: KrunkScript/host command) | v10:191006 |

## 9. Port status (gameplay)

Krunker v10.0.2 is the only version (doc/v7/decisions.md, user 2026-10-13): everything below is the
game's one ruleset (`shared/include/ruleset.h`); the v7.1.4 tables and rules are gone.

| item | where | tests |
|---|---|---|
| weapon balance (section 5), Boulder 29, Compressor 30, class HP and minRec (section 4), crouchLean 0 (section 1) | `shared/src/rules/v10/` (weapons_rebalance_v10.c, weapons_new_v10.c, classes_v10.c, constants_v10.c) | `weapons_v7_test` (the whole table against `shared/tests/data/v10/`, `tools/kref/v10-fixtures.py`), ruleset_test |
| Boulder gravMlt, Compressor (noDmg, nonObjImpulse, physPull, rShoot) | `shared/src/player.c` (procInputs gravity / fire input, `combat_fire_ray`) | v10_gameplay_test |
| posed hit boxes, hurtboxScale (game + class options) | `shared/src/hitboxes.c` (every mode but Prop Hunt), `shared/src/game_config.c` (hurtboxScale rows) | hitboxes_test, combat_test, ruleset_test |
| modes: Gun Game order, Stalker rework, Raid 25 min, Krank not a party mode | `shared/src/modes/modes.c` (the table), `shared/src/modes/mode_rules.c` / `mode_round.c` (Stalker hooks), `shared/src/player.c` (`movement_stalker`) | ruleset_test, v10_gameplay_test |
| ranked: x2 KR, no kill streaks, no class XP, needAll 60 s, shutdown 120 s | `Ruleset.ranked`, `Game.is_ranked` (`server/src/accounts.c`), `game_is_ranked`, `rules_reward_mlt` | ruleset_test, v10_gameplay_test |
| a v10 match on server_bin | `server_bin` (no version choice) | `server/tests/v10_match_e2e.py` |

Not ported yet: Bombardment without devOnly (KrunkNative has no carpet bomb), the comp-warmup spawn spread 10,
the proportional battle-pass points and team round wins outside best-of (section 7), raid challenges /
`noProfileStats`, the Stalker's render-side changes (model opacity 0.1, hidden after 300 ms still), its HUD
(leap timer, STALKER_PICKED popup, taunts), event kill pools, `poseData.bodyY`, `markPosition`, and the match-flow
items of section 8 other than the ranked timers (their own work: doc/v10/ranked-impl.md).
