# Player features and viewer: Krunker v3.1.1 -> v7.1.4 (spec and gap analysis)

Status: survey only, written 2026-10-12 by the spec-player agent. No code was changed.
Scope: everything player-facing (movement, classes, weapons, cosmetics, HUD, controls,
settings, account) plus the model viewer. The map editor is out of scope; it gets its
own survey.

**Policy (user decision, 2026-10-12).** Krunker Box Office upgrades **in place** to Krunker
v7.1.4. v7.1.4 movement constants, weapon and class stats, settings and player
features are adopted directly, with no v3.1.1 option. v3.1.1 is preserved on the
`v3.1.1` branch/tag, and AGENTS.md's parity section now names v7.1.4. This document
still marks every change that alters v3.1.1 gameplay (**[GAMEPLAY]**), so implementers
know which tests break and which movement recordings must be re-recorded against
v7.1.4. All `shared/tests/inputs/v3_*` files and the older live v3 recordings are
replaced by v7 recordings (1.1.8, WP-A).

## Sources

| Name | Path | Notes |
|---|---|---|
| v7 | `/home/user/jails/kru/src/Krunker_v7.1.4` | Readable source in `src/` (`src/data/*.js` is the source of truth for numbers). `public/js/*.js.map` holds the original module names. `skins.json` = readable `public/skins.jspck` |
| v3 | `/home/user/jails/kru/src/Krunker_v3.1.1` | `src/data/players.js` holds both physics and the player mesh. `public/viewer.html` exists (same page as v7 plus `zip-ext.js`) |
| KN | `/home/user/src/KrunkNative` | This repo, at commit `90cdea1` |

Refs look like `v7 src/data/player.js:123`, `v3 src/data/players.js:456` and
`KN shared/src/player.c:789`. Line numbers are from the trees above as of this survey.

## Legend

- **v3 -> v7 status**: NEW, CHANGED (old -> new), SAME, REMOVED.
- **Scope tag**: **[GAMEPLAY]** alters shared physics, combat or authoritative state, or a
  packet other peers rely on, so it changes v3.1.1 parity. **[CLIENT]** is
  presentation or input only. **[BACKEND]** needs kn_brain / `services/` support.
- **KN status**: **present** (matches v7), **partial** (the v3 version exists, or the
  key is stored but nothing reads it), **missing**. The owning KN file is named.

## Contents

- 0 Executive summary
- 1 Every player-facing change, v3.1.1 -> v7.1.4
  - 1.1 Movement and physics (includes the input array and the recordings plan)
  - 1.2 Classes, weapons, attachments, reticles, projectiles, killstreaks
  - 1.3 Cosmetics, item types, skin effects, player mesh
  - 1.4 Pets, badges, medals, challenges, account
  - 1.5 Controls, HUD, killcam, spectate, emotes/sprays/voice
  - 1.6 Status effects, perks and pickups
  - 1.7 Settings: every key in settings.js
- 2 The viewer: v7.1.4, v3.1.1 and Krunker Box Office
- 3 Gap analysis against Krunker Box Office (consolidated)
- 4 Implementation plan (six work packages)
- 5 Appendix: bugs found, open questions

## 0. Executive summary

1. **Movement core is unchanged.** Gravity, jump, acceleration/friction, crouch,
   slide decay, ladder speed, step-up, ramps, and the jump-pad and wall-jump impulses
   match. v7 moves the code into `movement.js` with per-class knobs whose defaults equal
   the v3 constants.
2. **20 movement changes alter v3 gameplay (1.1.7).** Among them: per-class wall-jump
   caps (`wallJM`: Run N Gun 2, Agent 3, Trooper 3, Runner 100), a slide only after
   an airborne landing plus a 100 ms re-slide lock, `slidCont` removed (slides always
   follow the look direction), directional and map-sized jump pads, border 250 -> 500,
   no ladder bottom clamp, every solid object wall-jumpable, and a 0.1 ms delta floor.
3. **The input array changes.** v7 swaps yaw/pitch (indices 2/3) and crouch/reload
   (8/9), sends jump while held, and adds reset (13) and interact (14). The server
   appends lag-comp fields 15/16. Input is sampled at `netRate` (30-360 Hz, default
   144), not once per frame. KN's own `Input` struct only needs the new fields and
   semantics; the recording converter must undo the swaps.
4. **New v7 movement mechanics:** grapple (weapon 20), 7 movement schemes (SRM with dash,
   Fly, Vehicle, ...), triangle-mesh collision, player-player collision, fall damage,
   head-clip fix, "fixed movement" integration and rope ladders. All are off by default or
   custom-game only.
5. **Classes: 14 -> 16** (Survivor, hidden; Infiltrator with the Charge Rifle). Changed
   stats: Hunter speed 1.05->1.0, Run N Gun HP 100->90, Spray N Pray HP 170->180 and
   speed 0.95->0.9, Agent HP 100->110, Trooper speed 1.0->0.9. Rocketeer gains a
   secondary.
6. **Weapons: 19 -> 29 (ids 19-28 new; ids 0-18 keep their meaning).** 16 of the 19
   existing weapons changed: 14 have stat changes, and Knife and Crossbow change through the
   charge system. Examples: Sniper rate 900->1000 and reload 1500->1900; AR
   ammo 30->28 and rate 110->120. The new **charge system** is on by default: a hip-fired
   crossbow bolt does 20 and a full 1.3 s charge does 200; the knife throw charges
   and its damage drops 200->180.
7. **Hit registration:** hitbox pad 1 -> **0.6** (0.1 on comp servers) plus per-class
   `hbS/hbH`, a per-weapon leg multiplier, and `headMlt`/`adjustedHeight` in the hit AABB.
8. **Attachments and reticles:** the 12 sight reticles are identical. New are the Acog
   Scope (zoom x1.35) and an `aimRecticles.js` scope-overlay picker (visual only).
   Killstreaks gain a 3-slot loadout plus Airdrop, Slimer, War Machine and Juggernaut.
   The default pick is Nuke only.
9. **Cosmetic slots 8 -> 17.** New slots: shoe, pet, wrist, charm, back (split from
   body), head (split from hat), player card, plus inventory-only collectible and
   ticket. The skin table grows 1917 -> 6448; the first 1914 indices are stable. Non-premium
   accounts cannot wear hat+head or body+back together.
10. **Skin effects:** v3 already had glow, pulse, frames, rgb, wireframe and 4 shaders.
    v7 adds env maps, video and projected textures, animated GLTF (22 items), 8
    renumbered shaders, per-skin sounds and item particles. KN animates skins only in
    the viewer and has no GLTF loader.
11. **Pets** (13, client-side followers), **badges** (29, 11 auto-awarded at match end),
    **data-driven daily/weekly challenges** (which replace the 26-level ladder),
    per-account medal counts, junk currency, Battle Pass. Most need kn_brain work.
12. **Controls:** every bind gets an alternate key, LMB/RMB become rebindable (`shoot1Key`
    10001 / `aim1Key` 10003), about 30 new actions are added, streak slots drop 5 -> 3,
    and the controls window has 5 tabs. Also new: per-weapon toggle-ADS and sensitivity,
    and hold/toggle triggers for the scoreboard and voice.
13. **HUD:** a full rebuild (bottom-left player card, Tab table scoreboard, K/D, TPS,
    rounds and perks displays, minimap, death markers, icon medals). The XP bar is
    removed. A new head-to-head death screen arrives with per-life stats.
14. **v7.1.4 has no emote wheel, no team pings and no replay killcam.** The "kill cam"
    is still the 2800 ms chase camera, now switchable per game (`kCams`). Emote-like
    features are prop-hunt taunts and 6 quick-chat binds.
15. **Social:** multi-spray (1/2/3 active by tier), a premium spray wheel, sprays from the
    store (default 2482), team/global chat and voice channels, proximity voice and
    per-player mute. The `ct`, `ch`, `vc`, `sp` and `spry` packets change shape.
16. **Settings:** 287 v7 keys (101 NEW, 59 CHANGED, 127 SAME) plus 7 removed v3 keys. In
    KN, 120 are present, 40 need a v7 default/range/per-weapon change, 28 are partial
    (stored, no-op), 94 are missing and 5 are n/a. KN's settings page carries exactly the
    193 v3 keys.
17. **Viewer:** KN's viewer is a faithful port of the **v3.1.1** viewer. v7 adds head,
    body, shoes, pet and wrist slots with `hide*` params, `anim`/`crouch` params, an
    auto-rotating showcase embedded as an iframe in the store item window, GLTF/FBX models,
    material effects, and dev config editors with Export Config.
18. **KN v3 parity bugs found along the way:** Auto Pistol `noHeadShot` is ignored
    (`player.c:2258`), weapon `movSpread` is not modelled (`player.c:1359`), jump-pad
    extents come from the map scale, and the challenges window hangs on "Loading"
    (kn_brain has no `chg` handler).
19. **Plan:** six parallel work packages (section 4): A movement plus v7 recordings,
    B combat data and mechanics, C protocol/input/server plumbing, D settings and
    controls, E HUD/death/spectate/social/progression, F cosmetics/player mesh/skin
    effects/pets/viewer. Hot files are partitioned by function (`player.c`: A owns
    movement, B owns combat; `shared.h`: C owns the wire structs).
20. **Open questions for the user (section 5):** v7 locks Hunter, Vince, Detective and
    Bowman for guests (KN offline play has no account), whether to re-import `ui/` from
    v7 `public/`, whether to add a GLTF loader (cgltf), the v7 model download, KrunkScript
    scope, and whether to replicate or fix the v7 reference bugs.

## 1. Every player-facing change, v3.1.1 -> v7.1.4

Each subsection lists the change with source refs and exact values. It tags the change
[GAMEPLAY] or [CLIENT]/[BACKEND], and ends with that area's KN gap table. Section 3
consolidates the gaps.


## 1.1 Movement and physics

Scope: everything `procInputs` and its helpers do to a player's position, velocity and
movement state, plus the settings, class, mode, weapon and effect multipliers that feed it.
Sources: v7 `src/data/player.js` (procInputs 859-2012), `src/data/players.js` (helpers),
`src/data/movement.js` (new movement-scheme table), `src/config.js`, `src/data/classes.js`;
v3 `src/data/players.js` (procInputs 570-1601, helpers 3051-3235), `src/config.js`,
`src/data/classes.js`; KN `shared/src/player.c`.

Units: Krunker works in map units and milliseconds. KN keeps delta in seconds and scales
its constants by 1000 (KN `shared/src/constants.c`: `player_speed 0.42` = 0.00042/ms,
`gravity 0.15` = 0.00015/ms²). All numbers below are Krunker (per-ms) values.

Policy (decided by the user): KN upgrades in place to v7.1.4 and adopts v7.1.4 movement
constants and physics directly, with no v3.1.1 option. v3.1.1 stays on the `v3.1.1`
branch/tag. Every gameplay-altering item below is therefore "adopt v7; re-record".

### 1.1.1 Summary

- The core integrator is the same in both versions: gravity, jump velocity/push,
  accel/decel constants, crouch, slide decay, ladder speed, step-up, ramps, jump-pad
  impulse, wall-jump impulse, and the lean/bob constants. v7 moved the default movement
  code into `movement.js[0]` ("Default": `dir`/`mov`/`vel`) and made most knobs
  per-class (custom-game class config). The defaults reproduce the v3 constants.
- Real default-gameplay changes: per-class **wall-jump limit** (`wallJM`), **class speed**
  changes (Hunter, Spray N Pray, Trooper) and slide stats, stricter **slide trigger** (must
  land from the air, plus a 100 ms re-slide lock), **ladder** bottom clamp removed,
  **jump pads** sized from the map and directional, **border height** 250 -> 500, all solid
  objects **wall-jumpable** unless flagged, a **0.1 ms delta floor**, the `slidCont`
  "Non FPS Sliding" setting removed (always on), and stronger **air strafe** (custom option).
- New v7 mechanics: grappling hook (Grappler weapon), 7 alternative movement schemes
  (SRM Quake-style bhop with dash/wall-dash, Fly, Vehicle, MMORPG, Follow Mouse, Dir Only,
  Move Right), triangle-mesh capsule collision, player-player collision, fall damage, head-clip
  fix, "fixed movement" exact integration, rope ladders, speed/jump effects and perks.
- Input array: **indices 2/3 (yaw/pitch) and 8/9 (crouch/reload) are swapped** in v7, 4 new
  fields (13-16), jump is sent held instead of on the press frame, and inputs are sampled
  at the `netRate` setting (30-360 Hz, default 144) instead of once per render frame.
  `procInputs(input, game, recon, lock)` has the same signature.
- Every existing recording in `shared/tests/inputs/` is v3.1.1 data. All of it has to be
  replaced with v7.1.4 recordings (1.1.8).

### 1.1.2 Unchanged (SAME) constants and mechanics

| Item | Value (both) | v3 ref | v7 ref |
|---|---|---|---|
| Gravity | 0.00015 /ms² × `gravMlt` | config.js:1071, players.js:788 | config.js:2021, player.js:1224 |
| Upward-velocity extra decay / terminal fall | `velY -= 0.000032·dt` while > 0; clamp at -0.3 | players.js:845-849 | movement.js:86-90 |
| Jump velocity / push | 0.072 / 0.1 (× `jumpMlt`); push is along -yaw, scaled by horizontal speed | config.js:1218-1219, players.js:3173-3184 | config.js:2129-2130, players.js:1699-1713 |
| Crouch jump / aimed jump | `×(1 - 0.25·crouchVal)`, `×0.85` when fully aimed; × `weapon.jumMlt \|\| spdMlt` | config.js:1197,1262 | config.js:2108,2167 |
| realMov factors | air accel ×0.72, jump ×0.92, wall jump ×0.7 horizontal, ×0.9 vertical | players.js:727,3175,1589-1590 | player.js:1167,1970-1971; players.js:1701 |
| Ground accel | 0.00042 × class speed (v7: `grA 0.042 / 100`) | config.js:1206, players.js:723 | config.js:1220 (grA def), player.js:1167 |
| Air accel | 0.000047 (v7: `arA 0.0047 / 100`) | config.js:1216 | config.js:1240 |
| Decel per ms | ground 0.99, air 0.9996, ladder 0.97, terrain 0.99, slide 0.999, terrain slide 0.9996 | config.js:1209-1217 | config.js:2119-2128; class grDec 0.99 / airDec 0.9996 (config.js:1230,1250) |
| decelMin crop / dltMx | 0.0001 / round(1000/30) = 33 ms | config.js:1220, 74 | config.js:2131, 40 |
| Aim slow / crouch slow | ×0.55 when fully aimed / ×0.3 crouched (v7 class `crchSpd` def 0.3) | config.js:1196,1260 | config.js:2107; 1267 |
| Diagonal strafe | `strafeSpd` 1.2 on diagonal movDirs unless realMov | config.js:355 | config.js:576; movement.js:45-47 |
| Crouch | 0.007 /ms toggling `crouchVal`; airborne crouch moves y by the same amount; height = 11 - 3·crouchVal | config.js:1238,1259,1261; players.js:692-718 | config.js:2149,2165-2166; player.js:1131-1161 |
| Slide decay and boost | slide timer 350 ms × `slidTime` × slT × crouchVal; speed ×1.2 (terrain 0.4) × slA; accel ×0.25 while sliding | config.js:1211,1226-1227; players.js:3104-3122 | config.js:2121,2137-2138; players.js:1640-1648 |
| Ladder climb | 0.035 /ms × ldA × weapon spdMlt × direction factor; no crouch on ladders | config.js:1208; players.js:1003-1036 | config.js:2118; player.js:1500-1509 |
| Step-up (climb) | climbHeight 3; blocks rise 0.03/ms; cylinders 30 % of the gap per tick | config.js:1221; players.js:1146-1157,1192-1205 | config.js:2132; player.js:1581-1585,1606-1612 |
| Ramps and rampFix snap | same progress-on-line height and `rampFix` within climbHeight | players.js:1071-1127,1360-1373 | player.js:1537-1564,1763-1772 |
| Booster ramps | boost × 0.002 × dt along ramp dir/boostDr | config.js:1400 | config.js:2295; player.js:1546-1551 |
| Spinning disks | `spin·dt·0.0002`, carry 0.07 × xVelC/zVelC in the air | config.js:1087; players.js:1170-1191,826-835 | config.js:2037; player.js:1596-1605,1246-1255 |
| Wall slide gravity | ×0.3 when falling, crouched, on a wall, class can wall-jump (v7 `wallJD` def 0.3) | players.js:780-791 | player.js:1220-1225; config.js:1529 |
| Wall-jump impulse | vy = 0.058, horizontal 0.03 × `wallJP` away from wall (v7 `wallJV` def 0.03) | players.js:1589-1599 | player.js:1970-1980; config.js:1538 |
| Jump pad impulse | vy = bounce × 0.1, halved when fully crouched on a `crouch` pad | players.js:944-953 | player.js:1416,1448 |
| Knockback (physPow) | `(1-dst)·physPow·impulseMlt` opposite the shot; re-applied on recon via `input.velObj` | players.js:4208-4228,1546-1553 | players.js:3020-3040, player.js:1937-1945 |
| Lean / bob / step constants | leanSens 0.05, leanMax 0.16, leanPull 0.99, leanMltZ 0.03, bobMltY 0.024, bobMltZ 0.02, bobPullY 0.985, stepMlt 0.25, stepPull 0.995, crouchAnim 0.0008 | config.js:1183-1200,1264 | config.js:2094-2111,2169 |
| Landing / jump camera tweens | landBobY = velY·6 (100 ms, then elastic 1100 ms), jumpBobY 0.22, wall jump 0.26 | players.js:3051-3080,3165-3222 | players.js:1612-1631,1717-1736 |
| Movement spread | spreadMove 25, spreadFall 30, recover 0.985, crouchSpread 0.55 (v7 defaults missing `movSpread` to 1) | config.js:1263-1270 | config.js:2168-2175; weapons.js tail |
| Weapon spdMlt / jumMlt, ids 0-18 | identical (Sniper .95, AR .95, Pistol 1.05, SMG 1.04, Revolver 1.04, Shotgun 1, LMG .79/jum .85, Semi 1, RL .9, Uzi 1.04, Deagle 1, Alien 1, Knife 1.1, Crossbow 1, Famas .95, Sawed Off 1, Auto Pistol 1, Bomb .65, Blaster .95) | weapons.js | weapons.js |
| No swimming/water, no explosion knockback, no melee leap (`leap` unused) | none in either version | | |

### 1.1.3 Changes, new mechanics and removals

Tags: [GAMEPLAY] = shared physics or parity; [CLIENT] = client-only visuals/UX.

#### Integration, delta and acceleration

| # | Item | v3.1.1 | v7.1.4 | Status | Tag |
|---|---|---|---|---|---|
| A1 | Delta clamp | `max(0, min(input[1], 33))` (players.js:575) | `max(0.1, min(input[1], 33))`; NaN/0 -> 0.1 ms (player.js:858,865-868) | CHANGED | GAMEPLAY |
| A2 | Code structure | inline in procInputs | `game.movement[class.movSch]` `.dir/.mov/.vel`, falls back to `movement[0]` (player.js:864,969-975,1239-1270; movement.js:3-98) | CHANGED (same default math) | GAMEPLAY |
| A3 | Speed X/Y/Z, Air Accel, Ladder Accel, Slide Time/Accel, Jump Cooldown, Infinite Jump | map `game` block (multipliers, def 1) -> `game.map.config` (v3 config.js:607-679, map.js:200-225) | per-class config `speedX/Y/Z`, `arA` (absolute, def 0.0047), `ldA`, `slT`, `slA`, `jpC`, `infJ` (config.js:1160-1296,1390-1407), set from map JSON `config.cls`; the map `game` block is ignored for physics (map.js:173 only defaults it) | CHANGED (moved) | GAMEPLAY (custom maps only; no KN map sets these) |
| A4 | New class knobs | none | `grA` 0.042, `grDec` 0.99, `airDec` 0.9996, `crchSpd` 0.3, `canCrch`, `trnSp` (turn-rate limit, `movement.js:5-38`), `movDrP` (move-dir offset ×π, player.js:890-893), `lckCmYR` (lock pitch, player.js:871), `hbS`/`hbH` (collision radius/height pad), `disJump`, `ovrJump`, `stpFr`, `stpSD`, `pColl`, `wallJ/wallJM/wallJD/wallJV`, `frSlid`, `bkSlid` (config.js:1143-1538) | NEW | GAMEPLAY (custom games) |
| A5 | Accel multiplier chain | aim, team `spdMlt`, crouch, weapon spdMlt, `1-input[12]` (players.js:722-731,793) | adds `mode.groundAccel` (applies to ground **and** air), `shootChrg && lMouse ×0.35`, noclip ×2, `speedBoost` (admin), `attributes.speed`, `attributes.rooted`, Kranked `bonuses.speed`, perk Swift ×1.15, `speedMlt` (bots) (player.js:1167-1238) | NEW | GAMEPLAY |
| A6 | Fixed Movement | none | `fixMov` (server option, def false): exact exponential integral `x += (v·d^t - v)/ln d · speedX`, vy decays ×0.9993^dt (movement.js:57-75; config.js:533) | NEW | GAMEPLAY |
| A7 | Camera-type movement basis | camType 1/2 -> yaw 0, 3 -> -π/2 (players.js:794-799) | only camType 1 with `!camRot`: basis from camOffX/Z (movement.js:40-44) | CHANGED | GAMEPLAY (custom) |
| A8 | Terrain slipping | computed then forced false (players.js:1397-1398) | removed entirely; terrain always sets vy 0 (player.js:1779-1797) | REMOVED (no-op) | GAMEPLAY (none) |

#### Jump

| # | Item | v3.1.1 | v7.1.4 | Status | Tag |
|---|---|---|---|---|---|
| J1 | Jump multipliers | `jumpMlt` only | × `attributes.jump` (Jump Boost effect) × `mode.jumpMlt` × `mode.jumpVel` (vel only) × `attributes.rooted` (players.js:1699-1704) | NEW | GAMEPLAY |
| J2 | Jump blockers / override | none | `weapon.noJump` (Minigun), `wasStopCrouch` (head-clip fix), class `disJump`, class `ovrJump` sets vy instead of adding (players.js:1693,1706-1710; player.js:1217) | NEW | GAMEPLAY |
| J3 | Jump cooldown | `jumpTimer = (slipping ? 500 : 0) × map jpC` (players.js:3167) | `jumpCooldown = jpC × 0` (players.js:1695); always 0 | SAME in effect | — |
| J4 | Jump input | sent on the key-down frame only (`CONTROLS.didPressed`, index.js:12829) | sent while held (`isPressed`, index.js:16833). `tryJump` still needs a release, so no difference unless `autoJump` is on: v7 then jumps on every landing while held | CHANGED | CLIENT (input) / GAMEPLAY with autoJump |

#### Crouch and slide

| # | Item | v3.1.1 | v7.1.4 | Status | Tag |
|---|---|---|---|---|---|
| S1 | When a landing starts a slide | `resetStep` called whenever `oldY > y` on a top/ramp/cylinder contact, even while grounded; slides if crouched and `canSlide` (players.js:1112-1114,1216-1218,3104-3122) | `resetStep` guard: `!wasOnGround && oldY > y` (players.js:1613). Walking down a ramp or step while grounded no longer slides | CHANGED | GAMEPLAY |
| S2 | Re-slide lock | none | `canSlideT = 100` ms after each slide (players.js:1642; player.js:1175-1180) | NEW | GAMEPLAY |
| S3 | Slide speed setting | none | `× game.config.slidSpd` (def 1, 0-2; `\|\| 0` so 0 kills horizontal speed) (players.js:1645; config.js:549) | NEW | GAMEPLAY |
| S4 | Mode disables | `realMov` only | `mode.noSliding` unless perk 4 (Power Slide) or 5 (Drift Fuze) (players.js:1632) | NEW | GAMEPLAY |
| S5 | Slide steering | player setting `slidCont` (default on: velocity snaps to the look direction; off: steers 18 %/tick toward it) (players.js:760-768; settings.js:1471) | always look direction (setting removed); `frSlid` (class or mode) steers by the held movement key, `bkSlid` slides backwards on S keys (player.js:1204-1213) | REMOVED setting / NEW options | GAMEPLAY |
| S6 | Class slide stats | none (map-level) | Spray N Pray `slA 0.98` (classes.js:82), Trooper `slT 1.1` (classes.js:283) | NEW | GAMEPLAY |
| S7 | Drift Fuze perk | none | explosion every 750 ms while sliding (player.js:1189-1198) | NEW | GAMEPLAY (zombies) |
| S8 | Crouch while grappling | n/a | uses the grounded branch (bob, no y shift) (player.js:1139,1151) | NEW | GAMEPLAY |

#### Wall jump

| # | Item | v3.1.1 | v7.1.4 | Status | Tag |
|---|---|---|---|---|---|
| W1 | Jumps per airtime | unlimited (players.js:1554-1600) | `wallJumpC < wallJumpMax`, reset on landing in resetStep (player.js:1955-1957; players.js:1615). Max per class: Run N Gun 2, Agent 3, Trooper 3, Runner 100, Survivor 100 (classes.js:51,173,271,193,290) | CHANGED | GAMEPLAY |
| W2 | Wall-jumpable objects | only objects built with `wallJumpable: wj == null`: block, sphere, gate, trigger, terminal (map.js:436-582) and tree/stump/rock (objects.js:823,863,1027). Crates, barrels, containers, planes, banks, assets are not | every solid object unless `notWallJ` (`wj` set) (player.js:1523; map.js:358-1266) | CHANGED | GAMEPLAY |
| W3 | Per-class/mode values | constants 0.3 / 0.03 / 0.058 | `wallJD`, `wallJV` per class; mode overrides `wallJ`, `wallJM`, `wallJD`, `wallJV`, `wallJYV` (vertical ×), `noWallJ` (player.js:694-695,1220-1221,1970-1971) | NEW | GAMEPLAY |
| W4 | Jump sound | jump_0/1 | shoe-specific (players.js:1596-1602) | CHANGED | CLIENT |

#### Ladders, jump pads, collisions

| # | Item | v3.1.1 | v7.1.4 | Status | Tag |
|---|---|---|---|---|---|
| L1 | Ladder bottom clamp | y clamped to [bottom, top] (players.js:1030-1034) | only clamped to the top (player.js:1505-1507): climbing down continues below the ladder | CHANGED | GAMEPLAY |
| L2 | Ladder while grappling | n/a | ignored while `grapple` (player.js:1501) | NEW | GAMEPLAY |
| L3 | Rope ladders | none | `ropeLadder` objects: climb relative to the look direction (player.js:1513-1522) | NEW | GAMEPLAY (custom) |
| P1 | Pad hitbox | fixed half-extents 5 × 5 × 1 (10×10 footprint) regardless of object scale (objects.js:330-338) | half-extents = scale/2, height `max(sy, 3)` (MapManager.js:294-307) | CHANGED | GAMEPLAY |
| P2 | Pad direction | always up | `d` 0 up, 1 down, 2/3/4/5 horizontal (pads set vx/vz instead of vy) (player.js:1417-1449; map.js:788-797) | NEW | GAMEPLAY |
| P3 | Pad bounce default | `parseFloat(bm) \|\| 1` (0 -> 1) (map.js:665-672) | `bm == null ? 1 : parseFloat(bm) \|\| 0` (0 allowed) (map.js:789-790) | CHANGED | GAMEPLAY |
| P4 | Pad vs surface order | a top/cylinder contact processed after the pad zeroes vy | `onPad`/`wasOnPad`: top, cylinder and head contacts keep vy on the tick after a pad; pad and trigger-type objects run once per tick per sid (player.js:1316,1375-1380,1457,1570-1580,1617) | CHANGED | GAMEPLAY |
| C1 | Border height | `borderH` 250 (config.js:1592) | 500 (config.js:2403) | CHANGED | GAMEPLAY |
| C2 | Cylinder sides | push out only (players.js:1158-1169) | a second side push in the same tick also zeroes vx/vz (player.js:1586-1595) | CHANGED | GAMEPLAY |
| C3 | Cylinder step-up epsilon | `+0.00001` only if `config.isComp` (players.js:1154) | always `+0.00001` (player.js:1582) | CHANGED | GAMEPLAY (tiny) |
| C4 | Head contact / head-clip fix | snap below, vy 0 | sets `stopCrouch`; with `headClipFix` (server option, def off, **on in comp**: defC true) crouch and jump are blocked for one tick and the pre-crouch height is used (player.js:1125-1127,1634-1646; config.js:862) | NEW | GAMEPLAY |
| C5 | Triangle-mesh collision | none | capsule vs triangle (`isTri`, complex-collision assets): push out along the normal, remove the normal velocity, ground if normal.y > 0.7 (player.js:1337-1351; collisions.js:13-64) | NEW | GAMEPLAY (custom assets) |
| C6 | Player collision | none | class `pColl` (def off): players are solid boxes (players.js:33-68; player.js:1326) | NEW | GAMEPLAY (custom) |
| C7 | Fall damage | none | server only, on landing: if `\|vy\| > fallDmgThr`, damage = `\|vy\|·10·fallDmg` (def 0 = off, max 10) (players.js:1649-1658; config.js:509-517) | NEW | GAMEPLAY |
| C8 | Hitbox pads | none | `hbS`/`hbH` added to every collision test (player.js:804-826) | NEW (def 0) | GAMEPLAY (custom) |
| C9 | Third-person height | client: `height` only updated for the first-person local player; third-person self and remote players keep 11 (players.js:1830) | updated for every rendered-self (`renderYou`) and on the server; adds `heightOld`, `adjustedHeight` (head scale) (players.js:254-262) | CHANGED (fixes a v3 prediction mismatch) | GAMEPLAY |
| C10 | `onGround` bookkeeping | flag | `setOnGround` also records `lastGround` (players.js:24-32) | NEW | GAMEPLAY (used by modes) |

#### Air strafe and knockback

| # | Item | v3.1.1 | v7.1.4 | Status | Tag |
|---|---|---|---|---|---|
| R1 | Air strafe (`airStrf`, def off) | rotate air velocity by `yawDelta × 0.73`, only on pure right/left (movDir 3/7) (players.js:818-825) | rotate by `yawDelta × 1.0`, also on forward diagonals (movDir 2 with right turns, 0 with left turns); gated by `currentSeason >= 7` (=7) (player.js:1256-1263) | CHANGED | GAMEPLAY |
| K1 | Knockback modifiers | `impulseMlt` | adds `mode.impulseMlt` (Classic 0, Impulsed 3), `mode.noImpulseDist`, `mode.nonObjImpulse` (fixed 0.04 impulse without a surface), object `noImpulse`/`noPCol` and `dst >= 0` checks (players.js:2884-2905,3020-3040) | NEW | GAMEPLAY |

#### New movement mechanics

| # | Mechanic | Behaviour (exact values) | Refs | Tag |
|---|---|---|---|---|
| N1 | Grappling hook | Weapon 20 "Grappler" (`grapRang` 500, spdMlt 1). Fire: ray vs objects (skips `noShoot`/`noGrapple`), anchor rounded to 0.1, `range = hitDist + height - 1.5 + 0.001`. Each tick: reel `range -= 0.04·dt` down to 35; if farther than range, project onto the sphere and set `v = (new - old)·decel^dt / dt`. Jump ends it; it also breaks when `dist - range >= 0.2` after collisions. Replays use `input.grapObj` | weapons.js:2056; players.js:2509-2585; player.js:1271-1294,1756-1762,1946-1953 | GAMEPLAY |
| N2 | SRM scheme (movSch 5) | Quake-style: friction 0.006, baseSpeed 0.07 (+dt/1000 on ground), groundAccel 0.014, groundDecel floor 0.01, air accel/decel 0.00085, side strafe accel 0.05 capped at 0.01, no friction during the first 33 ms grounded (bhop window), holding jump auto-bhops, ramp bounce (`vy·min(1,angle/45)·0.75`), minRampSlideVelocity 0.125, dash on input[6] (buffered 300 ms; speed 0.099995 + dt/1500, height 0.035, cooldown 1000 ms), wall-dash (reflect off the wall normal; speed 0.0525, height 0.07, cooldown 1150 ms, 0.7× on ramps), onWallMaxFrames 116, airDecel 1, `noThrow`, `noWallJ`, crouch halves wish speed, strafe-angle HUD overlay | movement.js:130-470 | GAMEPLAY (+ CLIENT overlay) |
| N3 | Fly (3), Follow Mouse (1), Dir Only (4), Move Right (2), MMORPG (7) | alternative `mov`/`dir` functions (fly along look pitch, move toward mouse yaw, absolute dirs, constant +x, click-to-turn at 0.012 rad/ms) | movement.js:99-129,629-649 | GAMEPLAY (custom) |
| N4 | Vehicle (6) | forward/reverse power with drag 0.97, angular drag 0.94, max power 0.12, power force 0.0012, max reverse 0.0375, reverse force 0.0005, turn threshold 0.0025, turn lean -2.7 (class `vh*` vals) | movement.js:471-628; classes.js tail | GAMEPLAY (custom) |
| N5 | Effects / perks | Boosted (speed +0.1/stack), Rooted (×, mlt 0.25, 25 s), Jump Boost (+0.1/stack), Dizzy (drunk); zombie perks Swift ×1.15, Power Slide, Drift Fuze | effects.js:2-93; config.js:944; player.js:1234-1237 | GAMEPLAY |
| N6 | KrunkScript hooks | `onPlayerUpdate`, `disabledInputs.*`, `defaultMovement/Velocity/Rotation` toggles, `game.noDefaultBehav` | player.js:894-948,1127,1239,1264,969 | GAMEPLAY (scripting) |
| N7 | Charge weapons | `shootChrg` weapon held ×0.35 accel (Charge Rifle) | player.js:1172; weapons.js:2825 | GAMEPLAY |

#### Settings, modes and other modifiers

| Item | v3.1.1 | v7.1.4 | Tag |
|---|---|---|---|
| Server (custom game) options | gravMlt 0.1-2 (step 0.1), jumpMlt, slidTime, wallJP, strafeSpd, canSlide, airStrf, autoJump, disableB, impulseMlt (config.js:302-451) | gravMlt **0-2 step 0.001**, jumpMlt step 0.001, + `fallDmg`, `fallDmgThr`, `fixMov`, `slidSpd`, `headClipFix` (config.js:491-862) | GAMEPLAY |
| Mode overrides used by official v7 modes | `realMov`, team `spdMlt` | + `gravMlt`, `jumpMlt`, `groundAccel`, `frSlid`, `noSliding`, `impulseMlt`, `wallJ*`, `forceClass` (e.g. Chaos Snipers: grav 0.92, jump 1.5, groundAccel 2, wallJM 100, wallJD 0.05, wallJV 0.06, wallJYV 2; Classic FFA: realMov, noSliding, impulse 0) (modes/chs.js:15-25, modes/imp.js:16-28, modes/clas.js:9-16). `airAccel` in those modes is never read | GAMEPLAY (modes section) |
| Class speeds | Hunter 1.05, Spray N Pray 0.95, Trooper 1.0 (v3 classes.js:18,41,140) | Hunter **1.0**, SNP **0.9**, Trooper **0.9** (v7 classes.js:33,75,278); new Survivor 0.8 (hidden), Infiltrator 1.0. Others unchanged (Trig 1.05, RnG 1.18, Vince/Det/Marks 1, Rocket 0.86, Agent 1.2, Runner/Deagler/Bowman/Commando 1) | GAMEPLAY |
| New weapons affecting movement | — | Build Tool spdMlt 0.9, Minigun spdMlt 0.5 / jumMlt 0.2 / `noJump`, Zapper 1.05, Akimbo Pistol 1.04, Charge Rifle 0.95 + `shootChrg`, Grappler | GAMEPLAY (weapons section) |
| Player settings | `slidCont` (Non FPS Sliding, default on) | removed; new `netRate` (30/60/120/144/240/360 Hz, default 144, mobile 60), `toggleADS` (input[6] is the toggle state), `wallLean`/`wallLeanMlt` (camera roll 0.15·mlt while crouched on a wall), `rawMouse`, `mouseAccel` (settings.js:55-72,3346,3474-3496; index.js:16899-16912) | CLIENT (netRate also sets the physics step size) |

### 1.1.4 Input array and procInputs

`procInputs(input, game, recon, lock)` is unchanged (v3 players.js:570, v7 player.js:859).
v7 also passes the frame through KrunkScript and reads per-class state (`this.config`).

| Index | v3.1.1 (index.js:12822-12838) | v7.1.4 (index.js:16833) |
|---|---|---|
| 0 | input sequence | same |
| 1 | `round(deltaMs × deltaMlt)`, one frame per render frame | same formula, sampled at `SETTINGS.netRate` (30-360 Hz, default 144); floored to 0.1 ms in procInputs |
| 2 | **yaw** `round(xDr.round(3)·1000)` | **pitch** `round(yDr.round(3)·1000)` |
| 3 | **pitch** `round(yDr.round(3)·1000)` | **yaw** `round(cameraYaw.round(3)·1000)` |
| 4 | movDir index or -1 | same (+ class `movDrP`·π) |
| 5 | shoot | same |
| 6 | aim held | `adsToggled` (ADS toggle aware); SRM: dash |
| 7 | jump, press frame only | jump held; SRM: auto-bhop |
| 8 | **crouch** | **reload** |
| 9 | **reload** | **crouch** |
| 10 | scroll × scrollDirM | scroll × per-weapon dir, normalized to -1/0/1 |
| 11 | swap action (1 sec, 2 melee, 3 primary, 4 equipment) | same codes |
| 12 | `1 - speedLmt.round(1)` (gradual speed) | same |
| 13 | — | reset: 1 = reset to start, 2 = reset to last checkpoint |
| 14 | — | interact key (air drops, `onInteractKey`) |
| 15 | — | `inputSTime`: server time the frame saw (from the `q` server-times map), used by `fetchState` lag compensation |
| 16 | — | `inputSTPrg`: interpolation progress 0..1.2 between snapshots |

The `q` packet changed with it: v3 `q(firstSeq, aimDeltas|0, changedFields, deltaStr, deltaLen)`
(v3 index.js:12979-12986), v7 `q(resyncFlag, firstSeq, deltaStr, deltaLen, [aimDeltas|0],
[changedFields], [serverTimes])` (v7 index.js:17165-17180). KN's native wire uses its own
`Input` struct (KN shared/include/shared.h:357-396), so the swap only matters for recording
converters and any JS-compatible transport.

State renames that matter for recordings: `velX/velY/velZ` -> `velocity.{x,y,z}` (THREE.Vector3),
`yaw/pitch` -> `xDire/yDire`, `jumpTimer` -> `jumpCooldown`. New state: `wallJumpC`,
`canSlideT`, `onPad`, `wasOnPad`, `wasOnGround`, `stopCrouch`, `wasStopCrouch`, `heightOld`,
`grapple`, `tmpVel`, `onGroundFrames`, `lastGround`, `overlayInfo`.

### 1.1.5 Client-only changes

| Item | v7 behaviour | Ref | Tag |
|---|---|---|---|
| Step cadence | `playerStep` runs for any grounded player that moved ≥ 0.1 units this tick (v3: local player with a held movement key). At high input rates (≥ ~230 Hz walking) steps stop counting; `stepFr` (class) scales the period | player.js:1814-1816; players.js:1661-1690 | CLIENT |
| Surface step sounds | `stepSrc` picks Default/Wood/Water/Sand/Snow; shoes override step/jump sounds | config.js:2045-2060; players.js:1596-1611 | CLIENT |
| Wall lean camera roll | setting `wallLean` | index.js:16899-16912 | CLIENT |
| Animation clips | Idle/Move/Crouch/Jump clips for asset player models | player.js:798-801; players.js:1715 | CLIENT |
| Remote step interpolation | `stepVal` lerped from the position buffer (`s`), step sounds on sign change | player.js:788-795 | CLIENT |
| SRM strafe overlay | `overlayInfo` angles for the local player | movement.js:434-470 | CLIENT |
| Shot direction drag | `shtDrg` class option (aim chase) | player.js:976-1000 | CLIENT/GAMEPLAY (custom) |

Lean/bob math in procInputs is identical (v3 players.js:581-602, v7 player.js:959-968).

### 1.1.6 Gap vs Krunker Box Office

| v7 item | KN status | Owning KN code |
|---|---|---|
| Core integrator, constants (1.1.2) | present (v3 values equal v7 defaults) | `player_proc_input` KN shared/src/player.c:1001; `game_constants` shared/src/constants.c |
| A1 delta floor 0.1 ms | missing (floors to 0) | player.c:1015 |
| A2/A4 per-class movement config (grA, arA, grDec, airDec, crchSpd, ldA, slT, slA, speedX/Y/Z, jpC, infJ, disJump, ovrJump, hbS/hbH, canCrch, movDrP, trnSp, wallJ*/frSlid/bkSlid) | missing: `ClassConfig` only has `speed`, `wall_jump`; slide/ladder/air/speed knobs live in `MapConfig` (defaults only, never parsed) | `ClassConfig` shared/include/shared.h:327-339; `g_default_map_config` shared/src/map.c:15-27; shared/src/classes.c |
| A5 multiplier chain | partial: aim, crouch, team spdMlt, weapon, slowdown, noclip ×2 present (noclip ×2 is already a v7-ism, v3 has none); groundAccel, effects, perks, Kranked, speedBoost, shootChrg missing | player.c:1173-1180 |
| A6 fixMov | missing | player.c:1273-1294 (integration block) |
| A7 camType basis | missing (TODO in code) | player.c:1243 |
| J1/J2 jump multipliers and blockers | missing | `player_jump` player.c:846-862; jump gate player.c:1221-1227 |
| J4 held jump / autoJump | KN sends press frame (v3) | client/src/ui/ui_controls.c:1111 |
| S1 slide needs airborne landing | missing (`last_position.y > position.y` guards only) | `player_reset_step` player.c:529-553; callers player.c:741,765,818 |
| S2 canSlideT 100 ms | missing | player_reset_step |
| S3/S4 slidSpd, noSliding | missing | player_reset_step; `GameConfig` shared.h:779-820 |
| S5 slide steering | present (both branches via `slid_cont`); v7 needs only the `slid_cont = 1` branch; frSlid/bkSlid missing | player.c:1196-1215; net.c:446 |
| S6 class slide stats | missing | classes.c |
| W1 wall-jump limit | missing (unlimited) | player.c:1430-1460 |
| W2 wall-jumpable set | partial: KN already marks crates/barrels/planes/assets wall-jumpable when `wj` is absent (v7-like, not v3); check the exclusion list against v7's `notWallJ` constructors | map.c:578-600 |
| W3 per-class/mode wall values | missing (constants 0.3/0.03/0.058) | player.c:1230,1437-1438 |
| L1 ladder bottom clamp | KN clamps both ends (v3) | player.c:693 |
| L2/L3 grapple gate, rope ladders | missing | player_do_map_collisions player.c:580 |
| P1-P4 jump pads (size, direction, bounce 0, wasOnPad, once per tick) | missing (vertical only, map scale used for the box, bounce 0 -> 1). KN's box comes from the map `s` scale, which already differs from v3's fixed 5/5/1 extents; check against both | player.c:634-640; map.c:468-477 |
| C1 borderH 500 | missing (250) | constants.c:17 |
| C2/C3 cylinder tweaks | missing | player.c:761-790 |
| C4 headClipFix | missing | player.c:836-838 (head contact) |
| C5 triangle collision | missing | (new) collision module |
| C6 player collision | missing (TODO) | player.c:1462 |
| C7 fall damage | missing | player_reset_step + `player_change_health` player.c:1994 |
| C9 height update | present (KN always updates height) | `player_update_height` player.c:51-53 |
| R1 air strafe ×1.0 + diagonals | missing (×0.73, pure strafe only) | player.c:1251-1259 |
| K1 knockback modifiers | partial (`impulse_mlt` only) | combat shot code player.c:2211+ |
| N1 grapple | missing | new weapon + player.c |
| N2-N4 movement schemes | missing | new `movement` table (shared) |
| N5 effects/perks | missing | — |
| N6 KrunkScript hooks | missing | — |
| Terrain raycast | missing in KN (TODO), used by both versions | player.c:1324 |
| Input layout (1.1.4) | N/A for the KN wire; recording converter must change | shared.h:357; shared/tests/proc_input_test.c parse_tick |

### 1.1.7 Gameplay-altering changes (policy: adopt v7)

Each item changes v3.1.1 parity. Every one is **adopt v7; re-record**.

1. Delta floor 0.1 ms (A1). Adopt v7; re-record.
2. Wall-jump count limit per airtime with per-class `wallJM` (RnG 2, Agent 3, Trooper 3, Runner 100), reset on landing (W1). Adopt v7; re-record.
3. Class speed changes: Hunter 1.05 -> 1.0, Spray N Pray 0.95 -> 0.9, Trooper 1.0 -> 0.9; slide stats SNP slA 0.98, Trooper slT 1.1 (S6). Adopt v7; re-record.
4. Slide only from an airborne landing (`!wasOnGround`), 100 ms re-slide lock, `slidSpd`, `noSliding` (S1-S4). Adopt v7; re-record.
5. Slide always follows the look direction (`slidCont` setting removed); `frSlid`/`bkSlid` options (S5). Adopt v7; re-record.
6. Air strafe ×1.0 and on forward diagonals (R1, only with `airStrf`). Adopt v7; re-record.
7. Ladder: no bottom clamp, disabled while grappling; rope ladders (L1-L3). Adopt v7; re-record.
8. Jump pads: map-scaled hitbox, 6 directions, bounce 0 allowed, `wasOnPad`, once per tick (P1-P4). Adopt v7; re-record.
9. Border height 250 -> 500 (C1). Adopt v7; re-record.
10. All solid objects wall-jumpable unless `wj` (W2). Adopt v7; re-record.
11. Cylinder side double-hit zeroes horizontal velocity; step-up epsilon always (C2, C3). Adopt v7; re-record.
12. Height updated for third-person self (C9; KN already does this). Adopt v7; re-record.
13. Jump modifiers/blockers: Jump Boost, Rooted, mode jumpMlt/jumpVel, Minigun `noJump`, `ovrJump`, `disJump` (J1, J2). Adopt v7; re-record.
14. Accel modifiers: `mode.groundAccel` (ground and air), effects, perks, Kranked, `shootChrg` ×0.35 (A5, N5, N7). Adopt v7; re-record.
15. Per-class movement config replaces map-level `game` multipliers; custom maps carry them in `config.cls` (A3, A4). Adopt v7; re-record.
16. Knockback modifiers (mode impulseMlt, nonObjImpulse, noImpulse objects) (K1). Adopt v7; re-record.
17. Held jump input (J4; changes behaviour with `autoJump`). Adopt v7; re-record.
18. New options, default off but part of v7 parity: fixMov, fallDmg/fallDmgThr, headClipFix (default **on** in competitive), pColl, hbS/hbH, gravity minimum 0 (A6, C4, C6-C8). Adopt v7; re-record.
19. New mechanics: grapple, movement schemes (SRM, Fly, Vehicle, MMORPG, Follow Mouse, Dir Only, Move Right), triangle collision, KrunkScript movement hooks (N1-N6, C5). Adopt v7; re-record.
20. Input layout and `q` packet shape (1.1.4): KN keeps its own wire, but the recording converter and any JS-compatible transport follow v7. Adopt v7; re-record.

### 1.1.8 Recordings

#### How the v3.1.1 recordings were made

- `tools/kref/movement-synth.js <scenario> <delta_ms> [out]` (KN): connects over CDP
  (`tools/kref/cdp.js`, `ws` from `net-bridge/node_modules`) to a headless Chromium on
  `KREF_CDP` (default 7780) whose page URL contains `KREF_PORT` (default 6780, the v3.1.1
  server started with `PORT=6780 npm run server`). The page is the v3.1.1 client in the
  offline sandbox with a custom map (localStorage `custMap` = the map JSON;
  `tools/kref/world-ref.js` shows the launch: `chromium-browser --headless=new
  --remote-debugging-port=… --use-angle=swiftshader`, `tools/kref/shim.js` injected with
  `Page.addScriptToEvaluateOnNewDocument` so pointer lock succeeds).
- Inside one `Runtime.evaluate` (the game loop cannot interleave) it takes
  `p = Game.players.list.find(p => p.isYou)`. `Game` is a page global because v3 bundles
  `src/index.js` as **cjs** (v3 server/compiler.ts:69), so `var Game` (index.js:687,704) is
  top-level. It saves all primitive fields, places the player, runs 30 idle settle frames,
  then calls `proc.call(p, input, Game, false, false)` once per tick with a 13-field v3
  input (`[isn, delta, yaw·1000, pitch·1000, mv, 0, aim, jumpPress, crouch, 0, 0, 0, 0]`),
  records state before/after (x, y, z, velX/Y/Z, onGround, crouchVal, aimVal, height,
  slideTimer, rampFix, stepVal, stepChase, bob/lean anims), and restores the player.
- It writes `test-synth-<scenario>-<delta>.json` in the `proc_input_test` format:
  `[[delta, yaw, pitch, movDir, jump, crouch, aim], [x, y, z, vx, vy, vz, rampFix, stepVal,
  stepChase, bobAnimY, bobAnimZ, leanAnimX, leanAnimY, leanAnimZ]]` per tick. These became
  `shared/tests/inputs/v3_*_{30,60,144}fps.json` (deltas 33 / 17 / 7 ms).
- `tools/kref/movement-rec.js` records real key presses (CDP key events) by wrapping
  `p.procInputs`; it expects a singleplayer match set up by `movement-sp.js`, which is not in
  the repo. The older files (`walk_forwards.json`, `jump.json`, `slide.json`, `wall_jump.json`,
  `walk_ramp.json`, ...) came from `shared/tests/input_recorder.js`, a userscript that wraps
  procInputs during live play. `proc_input_test` replays them on Sandstorm, Kanji and Soul
  Sanctum (shared/tests/proc_input_test.c:422-513).

#### Feasibility for v7.1.4 (read from code only)

- Server: `npm run server` = `node -r ts-node/register/transpile-only server/index.ts`
  (package.json:35). Port `PORT` (default 6767, server/index.ts:62), lobbies `LOBBY_COUNT`
  (:65). It compiles the client bundle at startup (:3604) and logs
  `[server] krunker v7.1.4 listening on http://HOST:PORT/` (:4365). The tree has a
  `server.pid`, and its AGENTS.md says a `npm run watch` may already be running: reuse that
  server's port if there is one.
- Client globals: the bundle is IIFE (server/compiler.ts:136), but `index.js:19125` does
  `Object.assign(window, {Game, sandboxGame, CONTROLS, RENDER, config, UTILS, SETTINGS,
  player, sandboxPlayer, …})`, and `player.js:301-304` sets `window.player` (client) or
  `window.sandboxPlayer` (sandbox authority) when an `isYou` player is created.
  `Game.players.list.find(p => p.isYou)` works as in v3. `mod/hooks.ts` only patches level
  locks, config and UTILS; nothing is needed from it.
- Offline sandbox: localStorage `custMap` (or `?sandbox`) -> `loadSandbox` (index.js:17799-17805)
  -> `initSandboxGame` builds `sandboxGame`/`sandboxPlayer` in-page (index.js:18280-18292). Each
  input frame runs `tmpPlayer.procInputs(frame, Game, …)` and
  `sandboxPlayer.procInputs(frame, sandboxGame, …)` (index.js:16842-16845). `shim.js`'s
  `krk_force_offline_sandbox` key is the same key v7 uses (mod/mods.ts:7).
- Signature: same `(input, game, recon, lock)`. Differences the synth must handle: 15-field
  input with the 2/3 and 8/9 swaps, held jump, `velocity` is a Vector3 (the primitive-only
  save/restore misses it), renamed state (`xDire/yDire/jumpCooldown`), new state to reset
  (`wallJumpC`, `canSlideT`, `onPad`, `wasOnPad`, `stopCrouch`, `grapple`, `tmpVel`), and
  per-class config (`p.config`, `p.classIndex`, `p.speed`).

#### Recipe (v7 variant: `tools/kref/movement-synth-v7.js`)

1. `cd /home/user/jails/kru/src/Krunker_v7.1.4 && PORT=6781 LOBBY_COUNT=1 npm run server`
   (or reuse a running server). Wait for the `listening on` line.
2. Start Chromium: `chromium-browser --headless=new --no-sandbox --use-gl=angle
   --use-angle=swiftshader --enable-unsafe-swiftshader --window-size=1280,720
   --remote-debugging-port=7781 --user-data-dir=/tmp/kn-move-v7/chrome --mute-audio
   --disable-background-timer-throttling --disable-renderer-backgrounding about:blank`.
3. Over CDP: `Page.addScriptToEvaluateOnNewDocument(shim.js)`, navigate to
   `http://127.0.0.1:6781/`, set `localStorage.custMap` to KN's `assets/maps/<map>.json`
   (keeps the geometry identical to what `proc_input_test` loads, so only physics differs),
   reload, then wait for `Game.players.list.some(p => p.isYou && p.active)`.
4. Pin the run: class 0 (Triggerman, speed 1.05; check `p.classIndex`), primary weapon
   (check `p.weapon.id` / `spdMlt`), `Game.config` defaults (`canSlide`, `slidSpd 1`,
   `strafeSpd 1.2`, `airStrf false`, `autoJump false`, `fixMov false`, `headClipFix false`).
   Write them into the output header.
5. `KREF_PORT=6781 KREF_CDP=7781 node tools/kref/movement-synth-v7.js <scenario> <delta>`.
   The v7 evaluate body, compared with movement-synth.js:
   - `const p = Game.players.list.find(p => p.isYou)`; save primitives plus
     `p.velocity.clone()`, `{...p.tmpVel}`, `p.grapple`.
   - Reset: `p.velocity.set(0,0,0)`, `p.xDire = yaw`, `p.yDire = 0`, `p.jumpCooldown = 0`,
     `p.wallJumpC = 0`, `p.canSlideT = 0`, `p.onPad = p.wasOnPad = false`, `p.didJump =
     p.didJumpW = false`, `p.stopCrouch = false`, `p.grapple = null`, plus the v3 resets.
   - Input: `[isn, delta, round(pitch·1000), round(yaw·1000), mv, 0, aim, jumpHeld, 0,
     crouch, 0, 0, 0, 0, 0]`; call `p.procInputs(input, Game, false, false)`. Use the
     sandbox pair (`sandboxPlayer`, `sandboxGame`) for a second pass that covers
     server-only paths (fall damage).
   - State: `{x, y, z, vx: p.velocity.x, vy: p.velocity.y, vz: p.velocity.z, g: onGround,
     lad: onLadder, cr: crouchVal, aim: aimVal, h: height, sl: slideTimer, cst: canSlideT,
     wjc: wallJumpC, wall: onWall, pad: onPad, rampFix, stepVal, stepChase, bob/lean anims,
     grapple}`.
   - Convert to the test format as `[in[1], in[3], in[2], in[4], in[7], in[9], in[6]]` (undo
     the swaps). Extend the state row with the v7 fields so `proc_input_test` can check them.
6. Do not modify the v7 tree: the scripts only evaluate code in the page.

#### What to re-record and add

All of `shared/tests/inputs/` is v3 data and will stop matching once v7 physics lands.

Replace (same scenario, same delta, plus 7 ms because v7's default netRate is 144 Hz):

| Existing file | Map | v7 deltas |
|---|---|---|
| v3_walk_diag_60fps | Sandstorm | 17, 7 |
| v3_aim_walk_60fps | Sandstorm | 17, 7 |
| v3_crouch_slide_144fps | Sandstorm | 7, 17, 33 |
| v3_sprint_jump_30fps | Sandstorm | 33, 7 |
| v3_look_60fps | Sandstorm | 17 |
| v3_ladder_60fps | Sandstorm | 17, 7 (add climb-down past the bottom) |
| v3_turn_jump_144fps | Sandstorm | 7 |
| v3_crouch_walk_30fps | Sandstorm | 33, 7 |
| v3_kanji_jump_pad_30fps | Kanji | 33, 7 |
| v3_soul_spawn_30fps / _60fps | Soul Sanctum | 33, 17 |
| collide, crouch, jump, scope, slide, spawn, walk_forwards, walk_ladder, walk_ramp, wall_jump, kanji_jump_pad (live v3 recordings) | Sandstorm/Kanji | re-record as synth scenarios at 17 and 7 (add `collide`, `ramp`, `wall_jump`, `scope`, `spawn` to the synth table), then retire the live files |

New v7 scenarios (17 and 7 ms unless noted):

| Scenario | Purpose | Setup |
|---|---|---|
| wall_jump_limit | W1: 3+ presses airborne, only `wallJM` succeed; counter resets on landing | Run N Gun (2), Agent (3); Sandstorm wall |
| wall_slide | wall-slide gravity ×wallJD while crouched | Run N Gun |
| ramp_crouch_descend | S1: crouch-walk down a ramp; v3 slid, v7 must not | Sandstorm ramp (KN `walk_ramp` spot) |
| slide_relock | S2: land-slide, stand to crouchVal ≤ 0.5, jump-land-crouch inside 100 ms | Sandstorm |
| slide_steer | S5: turn during a slide (always look direction) | Sandstorm |
| slide_class | S6: Trooper (slT 1.1), Spray N Pray (slA 0.98) | class switch |
| class_walk | class speed table: Hunter, Spray N Pray, Trooper walks | class switch |
| air_strafe_v7 | R1: `Game.config.airStrf = true`, right/left and forward-diagonal turns | Sandstorm |
| auto_jump_hold | J4: `autoJump = true`, hold jump across several landings | Sandstorm |
| jump_pad_edge | P1: walk onto the pad edge (hitbox from map scale) | Kanji |
| jump_pad_dir | P2-P4: pads with `d` 1-5, `bm 0`, pad on top of a block | small custom map JSON |
| border_jump | C1: jump onto an `isBorder` wall (500 extension) | map with borders |
| cylinder_side | C2: graze two cylinders in one tick | Soul Sanctum or custom |
| head_clip | C4: jump into a low ceiling with `headClipFix` off and on | custom |
| delta_zero | A1: frames with delta 0 integrate as 0.1 ms | any, 1-tick inputs |
| minigun_nojump | J2: jump with Minigun (no jump) | class/loadout with weapon 25 |
| fall_damage (33) | C7: server path, `fallDmg 1`, `fallDmgThr 0.1`, drop from height | sandboxPlayer/sandboxGame |
| fixmov_walk | A6: `fixMov = true` walk and jump | Sandstorm |
| grapple (if ported) | N1: fire Grappler, swing, reel to 35, jump release | custom class loadout with weapon 20 |
| srm_bhop / srm_dash (if ported) | N2: class `movSch 5`, auto-bhop, dash, wall-dash, ramp bounce | custom class config |

## 1.2 Classes, weapons, attachments, reticles

Scope: `src/data/classes.js`, `weapons.js`, `upgradeWeapons.js`, `weaponTypes.js`,
`attach.js`, `reticles.js`, `aimRecticles.js`, `projectile(s).js`, `streaks.js` in
v3.1.1 and v7.1.4, plus the client and server code that reads them. Method: both
`weapons.js` tables were loaded under Node and diffed field by field (v3's nested
`projectile: {..}` is flattened to v7's `pj_*` names). Every value below comes
from that diff or from the cited line. Times are in ms in JS. Krunker Box Office (KN)
stores the same values in seconds (`rate 900` -> `.rate = 0.9f`).

Tags: **[GAMEPLAY]** = changes shared stats or v3.1.1 behaviour; **[CLIENT]** =
visual/UI/audio/menu only. KN status: **present / partial / missing**.

---

### 1.2.1 Classes

v3 has 14 classes (`v3 src/data/classes.js:1-142`). v7 has 16: the same 14 in the
same order, plus Survivor (id 14) and Infiltrator (id 15) (`v7 src/data/classes.js:7-342`).
Class ids stay the same, so v3 loadouts and packets still line up.

Defaults: v3 omits `regen`, so `config.regenVal` 0.1 applies (`v3 src/data/players.js:396-401`,
`v3 src/config.js:1346`). v7 writes `regen: 0.1` explicitly, so the values are the same.

| id | Class | v3 (HP/segs, speed, regen, loadout, sec, wallJ) | v7 changes | Status | Tag | KN (`shared/src/classes.c`) |
|---|---|---|---|---|---|---|
| 0 | Triggerman | 100/6, 1.05, 0.1, [1] AR, sec yes | none (adds face 4544) | same | - | present :5 |
| 1 | Hunter | 60/5, **1.05**, 0.1, [0] Sniper, sec yes | **speed 1.05 -> 1.0**; `minRec 1` + `req` (needs an account, see below) | CHANGED | [GAMEPLAY] speed; [CLIENT] gating | present (v3 values) :19 |
| 2 | Run N Gun | **100**/6, 1.18, 0.1, [3] SMG, wallJ | **health 100 -> 90**; **wallJM 2** | CHANGED | [GAMEPLAY] | present (v3) :33 |
| 3 | Spray N Pray | **170**/7, **0.95**, 0.05, [6] LMG | **health 170 -> 180**, **speed 0.95 -> 0.9**, **slA 0.98** (slide accel x0.98) | CHANGED | [GAMEPLAY] | present (v3) :47 |
| 4 | Vince | 90/6, 1.0, 0.1, [5] Shotgun, sec | `txts` + 2 lines; `minRec 1`/`req` | CHANGED | [CLIENT] | present :61 |
| 5 | Detective | 100/6, 1.0, 0.1, [4] Revolver | `minRec 1`/`req` | CHANGED | [CLIENT] | present :75 |
| 6 | Marksman | 90/6, 1.0, 0.1, [7] Semi, sec | colors `[..,5793865,4806204,2631720,2631720,2699298]` -> `[..,4605741,2763291,1842204,2631720,2763291]` | CHANGED | [CLIENT] | present :89 |
| 7 | Rocketeer | 130/7, 0.86, 0.1, [8] RL, **no secondary** | **`secondary: true`** (`v7 classes.js:149`) | CHANGED | [GAMEPLAY] | present (v3: `.secondary = 0`) :103 |
| 8 | Agent | **100**/6, 1.2, 0.1, [9] Uzi, wallJ | **health 100 -> 110**; **wallJM 3** | CHANGED | [GAMEPLAY] | present (v3) :117 |
| 9 | Runner | 120/6, 1.0, 0.2, [12] Knife, wallJ | **wallJM 100**; **throwChrg 600** (others 800) | CHANGED | [GAMEPLAY] | present (v3) :131 |
| 10 | Deagler (hide) | 60/5, 1.0, [10] Deagle | `noMobile` | same | [CLIENT] | present :145 |
| 11 | Bowman | 100/6, 1.0, [13] Crossbow, sec | `minRec 1`/`req`, `noMobile` | CHANGED | [CLIENT] | present :159 |
| 12 | Commando | 100/6, 1.0, [14] Famas, sec | none | same | - | present :173 |
| 13 | Trooper | 100/6, **1.0**, [18] Blaster, wallJ | **speed 1.0 -> 0.9**, **wallJM 3**, **slT 1.1** (slide time x1.1) | CHANGED | [GAMEPLAY] | present (v3) :187 |
| 14 | **Survivor** (hide) | - | NEW: loadout [19] Build Tool, HP 150/6, speed 0.8, wallJ, wallJM 100, `noSleeve`, `noMobile`, no secondary | NEW (hidden, mode-only) | [GAMEPLAY] additive | missing |
| 15 | **Infiltrator** | - | NEW: loadout [28] Charge Rifle, secondary yes, HP 90/5 segs, speed 1.0, regen 0.1, colors `[11635832,14983351,14983351,10066329,15452519,10066329,10066329]`, face 5234 | NEW | [GAMEPLAY] additive | missing (`g_classes[14]`, `shared/include/shared.h:338`) |

New per-class fields in v7. Every class sets them, and the class-editor defaults
come from `config.classConfig` (`v7 src/config.js:961-1585`):

| Field | Meaning | Values | Code | Effect vs v3 |
|---|---|---|---|---|
| `wallJM` | Max wall jumps per airborne period. The counter `wallJumpC` resets on landing. | RnG 2, Agent 3, Trooper 3, Runner 100, Survivor 100, others 0 (no wallJ) | `v7 src/data/player.js:695,1955-1957`, `players.js:1615` | **[GAMEPLAY] NEW CAP.** v3 has no limit (`v3 players.js:1555`). Modes can override it (`mode.wallJM`). |
| `throwChrg` | Knife-throw charge time (ms) | 800 for all, Runner 600. Global default `config.throwCharge` 900 (v3 config 800, unused) | `v7 player.js:692,1069`; `players.js:2245` | [GAMEPLAY] Only applies through the new charge system (see 1.2.2c). |
| `slT` / `slA` | Per-class slide time / slide accel multipliers | Trooper slT 1.1; Spray slA 0.98; others 1 | `v7 players.js:1643-1644` | [GAMEPLAY]. v3 only has map-level `slT`/`slA` (`v3 players.js:3114-3119`). |
| `trnSpd`, `melRate`, `leap`, `bkSlid` | Turn speed mult, melee-rate mult, melee leap, backwards slide | 1, 1, false, false for all | `v7 movement.js:7`, `players.js:2239`, `player.js:1208` | Neutral at these values. They exist for custom classes. |
| `minRec` + `req` (`f791`) | Account level gate. With no account, `req` fails, so guests cannot pick the class (except in sandbox or a custom game). | Hunter, Vince, Detective, Bowman `minRec: 1` | `v7 classes.js:3-5`, `index.js:4988` | [CLIENT] menu gating |
| `noMobile`, `noSleeve`, `face` | Mobile picker hiding, arm sleeves, class face icon | - | - | [CLIENT] |
| `hbS`/`hbH` (classConfig) | Per-class hitbox pad added to `game.config.hitBoxPad` | default 0 | `v7 player.js:270-277` | see hitbox note below |

**Hitbox padding [GAMEPLAY].** v3 hitscan and melee pad player boxes by
`game.mode.hitBoxPad || 1` (`v3 players.js:3697-3702, 4132-4137`). v3 projectiles
use `config.hitBoxPad` = 1 (`v3 config.js:1254`, `projectile.js:167-172`). v7 uses
`game.config.hitBoxPad + class.hbS/hbH`, where the game-setting default is **0.6**
(`defC` 0.1) (`v7 config.js:622-631`, `player.js:270-277`, `players.js:2345,2937`,
`projectiles.js:191`). Players are therefore about 0.4 units narrower and shorter
to hit in v7 pubs. KN: v3 rule present (`shared/src/player.c:1809`, `modes.h:164`).

---

### 1.2.2 Weapons

#### a) Roster (index = weapon id; `type` follows `weaponTypes.js`)

| id | Weapon | type | v3 | v7 | Notes |
|---|---|---|---|---|---|
| 0 | Sniper Rifle | 0 | yes | yes | CHANGED |
| 1 | Assault Rifle | 0 | yes | yes | CHANGED |
| 2 | Pistol | 1 | yes | yes | stat-identical |
| 3 | Submachine Gun | 0 | yes | yes | CHANGED |
| 4 | Revolver | 0 | yes | yes | CHANGED |
| 5 | Shotgun | 0 | yes | yes | CHANGED |
| 6 | Machine Gun | 0 | yes | yes | CHANGED |
| 7 | Semi Auto | 0 | yes | yes | CHANGED |
| 8 | Rocket Launcher | 0 | yes | yes | CHANGED |
| 9 | Akimbo Uzi | 0 | yes | yes | CHANGED |
| 10 | Desert Eagle | 1 | yes | yes | CHANGED |
| 11 | Alien Blaster | 1 | yes | yes | CHANGED |
| 12 | Combat Knife | 2 | yes | yes | CHANGED (throw charge) |
| 13 | Crossbow | 0 | yes | yes | CHANGED (charge) |
| 14 | Famas | 0 | yes | yes | data reshaped only |
| 15 | Sawed Off | 1 | yes | yes | CHANGED |
| 16 | Auto Pistol | 1 | yes | yes | CHANGED |
| 17 | Bomb | 3 | yes | yes | identical (v7 adds `equipment: true`) |
| 18 | Blaster | 0 | yes | yes | CHANGED |
| 19 | Build Tool | 3 | - | NEW | equipment; Survivor class / build modes |
| 20 | Grappler | 1 | - | NEW | `customOnly` |
| 21 | Tehchy-9 | 1 | - | NEW | secondary, minRec 40 |
| 22 | Noob Tube | 1 | - | NEW | secondary, minRec 25, `noComp` |
| 23 | Slimer | 3 | - | NEW | Slimer killstreak equipment |
| 24 | Zapper | 1 | - | NEW | secondary, minRec 35, `noComp` |
| 25 | Minigun | 3 | - | NEW | Juggernaut killstreak equipment |
| 26 | War Machine | 3 | - | NEW | War Machine killstreak equipment, `noComp` |
| 27 | Akimbo Pistol | 1 | - | NEW | secondary, minRec 10 |
| 28 | Charge Rifle | 0 | - | NEW | Infiltrator primary |

No weapon was removed. Ids 0-18 are unchanged, so v3 ids remain valid in v7.
KN: ids 0-18 present (`shared/src/weapons/weapons.c:5-11`, `WEAPON_COUNT 19` in
`shared/include/weapons.h`); ids 19-28 missing.

#### b) Numeric diff of shared weapons (gameplay-relevant fields)

Fields left out because their only difference is a v7 default equal to the v3
behaviour: `adSpread` 0, `movSpread` 1, `lsMlt` 0.5, `reloY` 1, `burst*`,
`chrgTime` 0, `ovrChrg` false. The [CLIENT] fields are listed separately below.

| Weapon | Field | v3 | v7 | Tag | v3 ref | v7 ref | KN ref (v3 value) |
|---|---|---|---|---|---|---|---|
| Sniper Rifle | rate | 900 | **1000** | [GAMEPLAY] | weapons.js:70 | weapons.js:78 | awp.c:31 |
| Sniper Rifle | reload | 1500 | **1900** | [GAMEPLAY] | :47 | :55 | awp.c:22 |
| Assault Rifle | ammo | 30 | **28** | [GAMEPLAY] | :126 | :151 | ak47.c:19 |
| Assault Rifle | rate | 110 | **120** | [GAMEPLAY] | :127 | :152 | ak47.c:29 |
| Assault Rifle | reload | 1400 | **1500** | [GAMEPLAY] | :128 | :153 | ak47.c:21 |
| Assault Rifle | recoilYM | 0.6 | **1.1** | [CLIENT] viewmodel rise | :170 | :200 | ak47.c:40 |
| Assault Rifle | recoilData {dirs [0.05,-0.1,0.1,0.05,0.3,0.1,-0.05], pow 0.1, tim 150} | - | new | none (nothing in `src/` reads it) | - | :195 | - |
| Submachine Gun | ammo | 34 | **30** | [GAMEPLAY] | :305 | :369 | smg.c:19 |
| Submachine Gun | rate | 90 | **95** | [GAMEPLAY] | :333 | :397 | smg.c:29 |
| Submachine Gun | recoilZ | 3.4 | **4.2** | [CLIENT] viewmodel kick | :346 | :410 | smg.c:42 |
| Revolver | rate | 390 | **450** | [GAMEPLAY] | :424 | :505 | rev.c:28 |
| Shotgun | hsMlt | 1.5 | **1.25** | [GAMEPLAY] | :484 | :590 | shotgun.c:23 |
| Shotgun | cSpread | flat 20 values | `[[first 10],[last 10]]`, same numbers | none (v3 index `ammo*shots*2` == v7 `ammo%2` for 2 shells) | :512 | :618 | shotgun.c:58 |
| Machine Gun | forceAttach | true | **removed** (player's sight choice applies) | [GAMEPLAY] via sight zoom; [CLIENT] | :630 | - | lmg.c:15 |
| Machine Gun | reload | 3500 | **3300** | [GAMEPLAY] | :573 | :693 | lmg.c:22 |
| Machine Gun | swapTime | 700 | **600** | [GAMEPLAY] | :568 | :686 | lmg.c:21 |
| Semi Auto | dmg | 32 | **35** | [GAMEPLAY] | :679 | :823 | semi.c:24 |
| Semi Auto | hsMlt | 1.5625 (head 50) | **1.4571** (head 51.0) | [GAMEPLAY] | :680 | :824 | semi.c:25 |
| Semi Auto | lsMlt | 0.5 fixed (leg 16) | **0.5429** (leg 19.0) | [GAMEPLAY] | - | :825 | `player.c` fixed 0.5 |
| Semi Auto | rate | 120 | **150** | [GAMEPLAY] | :706 | :851 | semi.c:29 |
| Rocket Launcher | pj_spd | 0.72 | **0.75** | [GAMEPLAY] | :750 | :912 | projectile.c:16 |
| Rocket Launcher | rate | 350 | **320** | [GAMEPLAY] | :791 | :956 | rpg.c:25 |
| Rocket Launcher | swapTime | 400 | **350** | [GAMEPLAY] | :759 | :920 | rpg.c:20 |
| Rocket Launcher | kill (score) | - (base 50) | **["",75]** | [GAMEPLAY] score | - | :899 | `player.c:1859` table |
| Rocket Launcher | dmg | - | 25 | none (projectile weapons skip hitscan, `v7 players.js:2819`) | - | :928 | - |
| Rocket Launcher | hsMlt / lsMlt | 1.5 / - | 1 / 1 | none (see note 1) | :768 | :932-933 | rpg.c:24 |
| Akimbo Uzi | ammo | 18 | **28** | [GAMEPLAY] | :842 | :1024 | akimbo.c:17 |
| Akimbo Uzi | dmg | 14 | **15** | [GAMEPLAY] | :844 | :1026 | akimbo.c:22 |
| Akimbo Uzi | hsMlt | 1.5 | **1** | [GAMEPLAY] | :845 | :1027 | akimbo.c:23 |
| Akimbo Uzi | range | 700 | **500** | [GAMEPLAY] | :847 | :1029 | akimbo.c:25 |
| Akimbo Uzi | spread | 40 | **0** (minSpread 10 unchanged, so spread is clamped to 10) | [GAMEPLAY] | :871 | :1053 | akimbo.c:28 |
| Akimbo Uzi | movSpread | 0.1 | **0.01** | [GAMEPLAY] | :873 | :1055 | not modelled (note 3) |
| Desert Eagle | ammo | 6 | **7** | [GAMEPLAY] | :924 | :1124 | deagle.c:18 |
| Desert Eagle | dmg | 50 | **45** | [GAMEPLAY] | :926 | :1126 | deagle.c:23 |
| Desert Eagle | hsMlt | 1.5 (head 75) | **1.2** (head 54) | [GAMEPLAY] | :927 | :1127 | deagle.c:24 |
| Desert Eagle | rate | 400 | **450** | [GAMEPLAY] | :949 | :1149 | deagle.c:28 |
| Alien Blaster | dmg | 50 | **45** | [GAMEPLAY] | :995 | :1235 | alien_blaster.c:21 |
| Alien Blaster | hsMlt | 1.5 (head 75) | **1.25** (head 56.25) | [GAMEPLAY] | :996 | :1236 | alien_blaster.c:22 |
| Alien Blaster | noComp | - | true | [CLIENT] (comp lobbies) | - | :1224 | - |
| Combat Knife | ovrChrg | - | true | [GAMEPLAY] (throw charge, 1.2.2c) | - | :1423 | knife.c |
| Combat Knife | hsMlt / lsMlt | - | 1 / 1 | none (thrown knife had no head/leg mult in v3) | - | :1308-1309 | - |
| Crossbow | chrgTime | - | **1300** | [GAMEPLAY] | - | :1491 | crossbow.c |
| Crossbow | chrgMlt | - | **0.9** | [GAMEPLAY] | - | :1492 | - |
| Crossbow | ovrChrg | - | **true** | [GAMEPLAY] | - | :1493 | - |
| Crossbow | hsMlt / lsMlt | 1.5 / - | 1 / 1 | none (note 1) | :1270 | :1498-1499 | crossbow.c:25 |
| Famas | burst | `{c: 3, r: 90}` | `burst: true, burstC 3, burstR 90` | none (reshaped) | :1339 | :1583-1585 | famas.c |
| Sawed Off | cSpread | 10 values (5 pellets) | `[[0.03,0.05,-0.27,-0.02,-0.03,0.31,0.3,-0.01,0.01,-0.28],[v3 values]]`. v7 picks `cSpread[ammo%2]`; with 1 shell that is index 0, the **new** pattern | [GAMEPLAY] pellet pattern | :1460 | :1724 | sawed_off.c:58 |
| Sawed Off | sound | "weapon_6" | "weapon_16" | [CLIENT] | :1411 | :1668 | - |
| Sawed Off | noComp | - | true | [CLIENT] | - | :1669 | - |
| Auto Pistol | noHeadShot | true | **removed** | [GAMEPLAY] | :1495 | - | auto_pistol.c:15 |
| Auto Pistol | hsMlt | 1.5 (unused) | **1.1** (head 17.6) | [GAMEPLAY] | :1504 | :1785 | auto_pistol.c:23 |
| Blaster | reload | 1600 | **1450** | [GAMEPLAY] | :1620 | :1922 | blaster.c:20 |
| Blaster | dmgDrop | 5 | 0 | none (projectile weapon) | :1637 | :1940 | blaster.c:27 |
| Blaster | hsMlt / lsMlt | 1.5 / - | 1 / 1 | none (note 1) | :1634 | :1936-1937 | blaster.c:24 |

Unchanged on every shared weapon: zoom, aimSpd, spdMlt, range (except Uzi),
dmgDrop (except Blaster), pierce, spread/minSpread (except Uzi), recoil/recover,
`minRec` (Deagle 15, Alien Blaster 50, Sawed Off 20, Auto Pistol 30), and the
`kill` scores of the other weapons.

Notes:
1. v7 now applies head/leg multipliers to direct **projectile** hits
   (`v7 projectiles.js:296-307`). v3 projectiles had none (`v3 projectile.js:225`).
   v7 sets `hsMlt 1, lsMlt 1` on Rocket, Crossbow, Blaster and Knife, so the net
   result for v3 weapons is unchanged.
2. v7 hitscan damage (`v7 players.js:3036-3062`) differs from v3 (`v3 players.js:4233-4250`) in four ways:
   - **leg multiplier is per weapon** (`lsMlt`, default 0.5), not a fixed 0.5;
   - the leg zone shrinks while crouched: `legHeight - crouchDst*crouchVal` [GAMEPLAY];
   - a leg multiplier only applies when the hit is not a headshot;
   - damage after falloff is clamped to >= 0. The head zone becomes `headScale*headMlt`, but `headMlt` is 1 by default, so that part is neutral.
3. KN parity bugs found while surveying (v3.1.1, not v7):
   - `shared/src/player.c:2258` multiplies by `headshot_mlt` without checking
     `no_headshot`, so the Auto Pistol deals 1.5x headshots where v3 deals 1x
     (`v3 players.js:4246`).
   - KN's spread update (`shared/src/player.c:1359`) ignores `movSpread`. The
     Weapon struct has no such field, so the v3 Akimbo Uzi (`movSpread 0.1`)
     gains 10x too much movement spread.

New v7 fields on every weapon that do **not** change gameplay [CLIENT]: `charm*`
(weapon charms), `assetIcon`, `refDist`/`rollOff` (3D audio), `reloY` (reload
anim), `mysteryBoxR` (mystery-box rarity), `pj_asset`, `canUpgrade` (upgrade
eligibility, see 1.2.2e), `magReserve` (reserve mags, read only in `finiteAmmo`
modes, `v7 game.js:2195`, `player.js:1837-1866`), `req` functions (menu gating),
`noComp`.

#### c) New v7 combat mechanics touching shared weapons

| Mechanic | Exact behaviour | Refs | Tag |
|---|---|---|---|
| Charge system (`game.config.chrgWeps`, default **true**, `dontChange`) | While charging (`shootChrg` weapons charge with the trigger held, others while aimVal < 1, i.e. while ADS), `chargeTime += delta`. Past `chrgTime` it grows at `delta*0.1/(160/(chrgTime*(1.2-1)))`. With `ovrChrg`, reaching `chrgTime*1.2` resets the charge to 0 and sets `overCharged` until you un-aim. Without `ovrChrg` it clamps at `chrgTime` and sets `fullCharge`. `chrgDec` weapons decay at `chrgDec*delta` when released. | `v7 config.js:707-712, 2066`; `player.js:1067-1100` | [GAMEPLAY] |
| Crossbow charge | Projectile `dmgMlt = 1 - 0.9*(1 - min(1, charge/1300))`. A hip shot does 0.1 x 200 = **20**. A full ADS charge (1.3 s) does **200**. Holding past full charge overcharges after about 1.6 s more and resets the charge. v3: always 200. | `v7 weapons.js:1491-1493`; `players.js:2676-2685, 2832` | [GAMEPLAY] |
| Knife throw charge | The throw (ADS + fire) uses `d = clamp(chargeTime/throwChrg, 0.1, 1)`. Projectile dmg is `ceil(180*d)` (stored dmg was **200 -> 180**) and speed is `0.38*max(0.3, d)`. `throwChrg` is 800 (Runner 600). The knife has `ovrChrg`, so holding too long resets. `mode.forceCharge` forces d = 1. v3: instant 200-damage throw. | `v7 players.js:2236-2300`; `projectiles.js:10-24`; `v3 players.js:3615` | [GAMEPLAY] |
| Hitbox pad 1 -> 0.6 | See 1.2.1 | | [GAMEPLAY] |
| Upgraded weapons | `player.upgrades[slot]` swaps `weaponConfig[id]` for `upgradeWeapons[id]` | 1.2.2e | [GAMEPLAY] additive (map objects) |
| Per-player multipliers | `attributes.fRate/reload`, `perks` (Rapid Fire 0.66 rate, reload 0.5, ...), `isKranked` bonuses, `spreadMlt`, `rangeMlt`, `dmgMlt`, status effects (`effects.js`). All are 1/none in plain pub play. | `v7 players.js:2239, 1822`; `effects.js` | [GAMEPLAY] mode-dependent |

KN: none of this exists. The knife throw is instant at 200 (`shared/src/projectile.c:21`,
`player.c:2123-2157`) and the crossbow always does 200 (`projectile.c:17`).

#### d) New v7 weapons (ids 19-28), summarized (`v7 src/data/weapons.js:2003-2977`)

| id | Weapon | Role | Key stats |
|---|---|---|---|
| 19 | Build Tool | equipment, `build: true` | rate 300, range 50, spdMlt 0.9, swap 200, zoom 1.1, spread 120, `noCross` |
| 20 | Grappler | secondary, `customOnly`, `grapRang 500` | ammo 10, reload 1000, rate 300, dmg 1, hs 1.5, range 700, `noSpread`, zoom 1.2, kill 50 |
| 21 | Tehchy-9 | secondary, minRec 40, 3-round burst (burstR 90) | ammo 15, reload 1000, dmg 23, hs 1.25, dmgDrop 2, pierce 0.95, range 700, rate 380, spread 150, spdMlt 1, zoom 1.3, kill 50 |
| 22 | Noob Tube | secondary, minRec 25, `noComp`, grenade | ammo 1, reload 1200, rate 400, projectile spd 0.75, grav 0.001, dmg 45, explode 40 / eDmg 35, range 1000, `nImp`, hs/ls 1, `innac` 110, zoom 1.25 |
| 23 | Slimer | equipment (streak), `customOnly`, `deployable 5` | ammo 2, `noReload`, `removeOnEmpty`, deployRang 250, `noAim`: places slime boost pads |
| 24 | Zapper | secondary, minRec 35, `noComp`, chargeable | ammo 5, reload 1400, chrgTime 1500 (`ovrChrg`), projectile spd 0.5, grav 0, dmg 5, range 150, `effectOnHit 8` ("Zapped": 2 hp/tick, stacks = floor(charge*10), max 10), spdMlt 1.05, kill 75 |
| 25 | Minigun | equipment (Juggernaut) | ammo 250, `noReload`, `noSwap`, `noJump`, spdMlt 0.5, dmg 18, hs 1.5, dmgDrop 10, range 700, rate 70, spread 300 / min 10, zoom 1.3 |
| 26 | War Machine | equipment (streak), `noComp` | ammo 6, `noReload`, `noSwap`, rate 300, projectile spd 0.75, grav 0.001, dmg 60, explode 70 / eDmg 65, range 1000, zoom 1.25 |
| 27 | Akimbo Pistol | secondary, minRec 10, akimbo | ammo 12, reload 750, dmg 15, hs 1.4, dmgDrop 13, range 700, rate 100, spread 40, movSpread 0.1, minSpread 10, spdMlt 1.04, zoom 1.5 |
| 28 | Charge Rifle | Infiltrator primary, `scope`, hitscan | ammo 1, reload 1400, dmg 15, hs 1, dmgDrop 5, range 1000, rate 150, spread 150 / min 5, spdMlt 0.95, zoom 2. Charges while the trigger is held (`shootChrg`, chrgTime 700, `chrgDec 0.8`). Normal shots keep firing, cost **no ammo** (`fullChrgSht`) and do not reset the charge (`bypassChrg`). The interval shrinks as the charge builds: `rate * max(0.7, (1-charge)*1.5)` (`rateChrg`). At full charge, the next shot does dmg x `fullChrgMlt` 50 = **750** and uses the 1 round. Refs: `v7 players.js:2664-2698, 2996`. |

All ten are missing in KN. The ones with a non-default `type`/secondary role also
need the v7 secondary picker list (`v7 index.js:8033`) and the `minRec` gating KN
already uses for v3 secondaries.

#### e) `weaponTypes.js` and `upgradeWeapons.js`

- `weaponTypes.js` (`v7 src/data/weaponTypes.js:1-6`): an enum
  `{PRIMARY: 0, SECONDARY: 1, MELEE: 2, EQUIPMENT: 3}` naming the existing `type`
  field. The values match v3's `type`. No gameplay effect. KN: `Weapon` has no
  `type`; it uses `secondary`/`melee`/`equipment` bits instead (fine).
- `upgradeWeapons.js` (3084 lines): a second 29-entry table indexed like
  `weapons.js`, with `upgraded: true`. Which table applies is decided per
  loadout slot by `player.upgrades[slot]` (`v7 players.js:2286, 2831, 3621, 4901`).
  A slot gets upgraded by buying at a **`krumMachine`** map object
  (`v7 player.js:642, 1352-1353`; `config.krumMachineAnim 5000`), which works like
  "pack-a-punch" in zombies-style custom maps. The upgrade gates on `canUpgrade`
  (true for all firearms). It is **[GAMEPLAY]** but only on maps that place a
  krumMachine. Diff vs the base v7 weapon:

| id | Upgraded name | Changes |
|---|---|---|
| 0 | AWP Blaster | rate 1000->280, reload 1900->1200, ammo 3->12, minSpread 20, burst 2 @100 |
| 1 | Zombie Assaulter | dmg 23->28, rate 120->90, ammo 28->35 |
| 2 | Pistol | rate 150->100, ammo 10->16 |
| 3 | UMPire | dmg 18->24, rate 95->80, ammo 30->44 |
| 4 | Revolver | ammo 6->12, burst 3 @90 |
| 5 | Shotgun | ammo 2->6, burst 3 @60 |
| 6 | NORU | dmg 18->22, rate 130->100 |
| 7 | Semi Auto | rate 150->100, ammo 8->12 |
| 8 | Dream Wrecker | ammo 3->9, spdMlt 0.9->0.95, pj_dmg 50->60 |
| 9 | Akimbo Uzi | dmg 15->18, ammo 28->30 |
| 10 | Desert Eagle | dmg 45->80, rate 450->350, ammo 7->3 |
| 11 | Cupids Blaster | name only |
| 13 | Crossbow | ammo 1->3, pj_spd 0.68->0.75, pj_dmg 200->100 + explode 100 / eDmg 100, grav 0.00008, nImp |
| 14 | Famas | ammo 30->36, burstC 3->4 |
| 15 | Sawed Off | ammo 1->3, burst 3 @100 |
| 16 | Auto Pistol | rate 100->70, ammo 15->18 |
| 18 | Blaster | hsMlt 1->1.5, rate 160->110, reload 1450->1100 |
| 21 | Tehchy-9 | ammo 15->18, burstR 90->80 |
| 22 | Noob Tube | ammo 1->2, pj_dmg 45->50 |
| 24 | Zapper | dmg 10->12, ammo 5->10 |
| 27 | Akimbo Pistol | rate 100->90, ammo 12->15 |
| 28 | Charge Rifle | shots 3 with cSpread |

Ids 12, 17, 19, 20, 23, 25 and 26 change only cosmetics (`altSkin` projection
shader, `muzID 3`), and the magReserve values also change. KN: missing (no
krumMachine object, no `upgrades[]`).

---

### 1.2.3 Attachments and reticles

#### Attachments (`attach.js`)

| id | Name | v3 (`v3 src/data/attach.js:1-9`) | v7 (`v7 src/data/attach.js:2-31`) | Tag |
|---|---|---|---|---|
| 0 | Reflex Sight | src attach_0, dotSrc dot_0, aimOffY 0.16, scale 1.2 | same + `expScale 12` | [CLIENT] |
| 1 | **Acog Scope** | - | NEW: src attach_1, `zoomMlt 1.35`, `zRec 0.9`, aimOffY 0.22, zOff 0.35, `yOff {1: 0.01, 14: -0.01}`, scale 1.1, expScale 8, `noVertM` (no reader), `minRec 20` + `req`. The req is per-class rank >= 20, from `stats["c"+classIndex]` via `config.rankVar` (`v7 index.js:8632`). | **[GAMEPLAY]** zoom |

Which weapons take a sight (`attach: 0`) is the same in both versions: AR, SMG,
MG, Semi, Crossbow, Famas, Blaster. v3 forces the Reflex Sight on the MG; v7 drops
`forceAttach` and adds a skin-level `attachBypass`/`noAttach`/`forceAttach`.

How the client uses them:
- **v3**: `attachIndex` is saved locally and sent at spawn. The sight mesh uses
  `aimOffY` for the ADS height (`v3 players.js:2023-2026`). When fully aimed with
  a sight, the `#aimDot` reticle is shown (`v3 players.js:3265-3274`). It is
  visual only.
- **v7**: same, plus the Acog multiplies the ADS zoom:
  `RENDER.zoom(1 + (weapon.zoom*zoomMlt - 1)*aim*adsFov)` (`v7 players.js:1790-1794`).
  That is FOV only; spread is untouched. `zRec` replaces `weapon.recoilZM` in the
  viewmodel recoil (`v7 players.js:345`, visual). The server takes the client's
  `attachIndex` (`v7 server/index.ts:1966, 3525`).
- KN: **partial**. The data is `g_attachments[1]` (`shared/src/weapons/weapons.c:13-22`)
  and the loadout UI offers Reflex/None (`client/src/ui/ui_loadout.c:450-478, 592-600`).
  The rendering always uses `DEFAULT_ATTACHMENT_INDEX` (`client/src/player_mesh.c:753, 925`),
  so the selection never reaches the mesh, aim or network. Acog: missing.

#### Sight reticles (`reticles.js`), the `#aimDot` inside a sight

Identical in both versions: 12 entries, ids 0-11 (Red Dot, Green Dot, Pyramid,
Tau, Ray, Spread, Marine, Underline, Precision, Quantum, Triad, Nuclear)
(`v3 src/data/reticles.js:1-50`, `v7 src/data/reticles.js:1-36`). Each loads
`textures/reticles/reticle_<id>.png`, or a custom image with -2, or none with -1
(`v3 index.js:993-1020`, `v7 index.js:1199-1221`). [CLIENT], visual only.
KN: **present** (`client/src/ui/ui_loadout.c:79-80, 603-612`; `ui_render_settings.c:731-733`).

#### Scope overlays (`aimRecticles.js`), NEW in v7

v3 has one sniper overlay, `textures/recticle.png` (`v3 libs/render.js:146`,
`#recticleImg`). The `customScope` setting can replace it. v7 adds a selectable
**scope** list: Default 0, Pro 1, OG 2, Waspy 4, MoonJuice 5, Swixie 6, Sorable 7
(id 3 skipped) (`v7 src/data/aimRecticles.js:1-21`). It loads
`textures/scopes/scope_<id>.png`, with `scopeIndex` -1 for none and -2 for custom
(`v7 index.js:1224-1245, 3246-3248`, saved in `scopeIndex`). New settings
`scopeBorderCol`, `scopeBorderOpac`, `scopeWidth`, `scopeHeight`, `scopeOpac`
(`v7 libs/settings.js:4011-4110`). The `scope_0..7.png` textures already ship in
the v3 public tree. [CLIENT], visual only. KN: **missing** (only `customScope` /
`scopeBorders`, `client/src/ui/ui_render_settings.c:422-423, 730`).

---

### 1.2.4 Projectiles

`projectiles.stored` (`v3 src/data/projectile.js:62-115`, `v7 src/data/projectiles.js`):

| idx | Use | v3 | v7 | Tag |
|---|---|---|---|---|
| 0 | thrown melee | spd 0.38, grav 0.0002, linger 5000, dmg **200**, range 1500 | dmg **180** (then x charge d, speed x max(0.3, d)) | [GAMEPLAY] |
| 1 | AI orb | spd 0.05, hitB 10, scale 6.1, dmg 60, range 500 | spd 0.1, hitB 4, dmg 1, life 24000 | [GAMEPLAY] AI (see AI section) |
| 2 | AI (projType 1) / v7: Bombardment bomb | spd 0.25, scale 4, dmg 20 | `streak_3` bomb: spd 0.4, explode 150, dmg 150, eDmg 150, range 1500, `dontPred`, info.stk 3 | v3 AI kinds 2-4 removed; [GAMEPLAY] dev-only streak |
| 3, 4 | AI (projType 2, 3) | dmg 200 / dmg 10 | removed | AI section |

`Projectile.init` changes (`v7 projectiles.js:4-60`):
- speed `x v x pjSp` and damage `ceil(dmg x d) x dmgMlt` (charge and skins);
- `rangeMlt`;
- custom projectile assets (`customProj` pjA/pjSp/pjG/pjSC);
- skin-driven explode (`ex`/`expS`);
- `effectInd`/`effectC` (status effects);
- `streak` (killstreak bombs);
- `noObjC`;
- `travelTime`;
- `life` counts down instead of range.

Direct hits now apply `hsMlt`/`lsMlt` (note 1 above).

Per-weapon projectile values: only the Rocket `pj_spd` changed (0.72 -> 0.75). The
Crossbow (spd 0.68, dmg 200), Blaster and Bomb are identical. KN: v3 table present
(`shared/src/projectile.c:11-21`). Charge, effect and streak fields are missing.

---

### 1.2.5 Killstreaks (`streaks.js`)

v3 (`v3 src/data/streaks.js:1-11`) has Nuke only: 25 kills. It is earned at
`realKillStreak == kills`, and the counter resets at `maxStreak`
(`v3 players.js:5556-5562`). It is activated with the streak key and has a fixed
10 s timer (`v3 game.js:1282-1286`). It kills every active enemy and gives +50
score each (`v3 game.js:3118-3156`).

v7 (`v7 src/data/streaks.js`) adds **streak loadouts**: the player picks one
streak per `slot` (`streakIndex1..3` saved locally and sent in `enterData.streakIndexes`,
`v7 index.js:3279-3284`, `server/index.ts:3549`). The defaults are slot 0 none,
slot 1 none, slot 2 = Nuke, which matches v3. Earning checks only the selected
streaks. The counter resets at the highest selected `kills` (`v7 players.js:4588-4600`).
Modes can disable streaks (`mode.disabledStreaks`).

| idx | Streak | slot | kills | Mechanics | Status | Tag |
|---|---|---|---|---|---|---|
| 0 | Nuke | 2 | 25 | Same kill/score as v3. **New:** `reqAccount` (needs `player.account`; v7.1.4's server gives every player one, guests via `createGuestAccount`, so in practice everyone earns it). Timer is `gameTimer-1` when under 10 s remain in untimed modes, otherwise 10000 (`streaks.js:17-23`). Custom-map triggers `onNukeStart`/`onNukeEnd`. 1/140 chance of skin 4040 in pubs. | CHANGED | [GAMEPLAY] minor |
| 1 | Airdrop | 1 | 12 | Needs open sky (500 up). Drops an `airDrop` package: KR/Junk/Trapped/Skin/Juggernaut/War Machine/Nuke/Slimer packages (`airDropPackages.js`). `noCustoms`. | NEW | [GAMEPLAY] |
| 2 | Refill | 0 | 5 | devOnly: refills the thrown knife | NEW (dev) | - |
| 3 | Bombardment | 2 | 20 | devOnly: carpet-bombs a grid over the spawn bounds, a bomb every 500 ms (stored[2]). Hits apply Rooted + Dizzy, and kills give +100. | NEW (dev) | - |
| 4 | Slimer | 0 | 7 | Gives weapon 23 (Slimer) | NEW | [GAMEPLAY] |
| 5 | Juggernaut | 2 | 30 | `setJuggernaut`: maxHealth/health 2000, Minigun (25), `jug_0` loop sound, no regen while juggernaut (`v7 game.js:1607-1632`, `players.js:4181`) | NEW | [GAMEPLAY] |
| 6 | War Machine | 1 | 15 | Gives weapon 26 | NEW | [GAMEPLAY] |

KN: **partial**. Nuke detonation is present (`shared/src/game.c:587-606`), but
earning, the `st` packet, use-streak input and `startNuke` are missing (nothing
sets `nuke_timer`; `shared/src/player.c:1485` says "Not ported yet: kill streak
rewards"). The kill-streak `kst` messages are present (`player.c:2053-2116`). All
v7 streaks, slots and airdrops are missing.

---

### 1.2.6 Gap summary vs Krunker Box Office

| Item | KN status | Owning file(s) |
|---|---|---|
| v3 classes 0-13 | present (v3 values) | `shared/src/classes.c` |
| v7 class stat changes, `wallJM`, `throwChrg`, per-class `slT`/`slA` | missing | `shared/src/classes.c`, `shared/include/shared.h:324-336` (ClassConfig), `shared/src/player.c:1430` (wall jump) |
| Survivor / Infiltrator | missing | `shared/src/classes.c` (+ `g_classes[14]` size) |
| v3 weapons 0-18 | present | `shared/src/weapons/*.c` |
| v3 `noHeadShot` in hitscan | **bug** (ignored) | `shared/src/player.c:2258` |
| v3 `movSpread` (Akimbo Uzi) | **bug** (not modelled) | `shared/include/weapons.h`, `shared/src/player.c:1359` |
| v7 stat changes on 0-18 | missing | `shared/src/weapons/*.c` |
| Per-weapon `lsMlt`, crouch leg zone, damage clamp | missing | `shared/src/player.c:2253-2258` |
| Projectile head/leg mult | missing (net-neutral for v3 weapons) | `shared/src/projectile.c:253` |
| Charge system (crossbow, knife throw, zapper, charge rifle) | missing | `shared/src/player.c`, `shared/src/projectile.c` |
| Hitbox pad 0.6 game setting | missing (v3 pad 1 present) | `shared/src/player.c:1809`, `shared/include/modes.h:164` |
| Weapons 19-28 | missing | `shared/src/weapons/`, `weapons.h` (`WEAPON_COUNT`) |
| upgradeWeapons / krumMachine | missing | new table + map objects |
| Attachments: selection wiring | partial | `client/src/player_mesh.c:753,925`, `client/src/ui/ui_loadout.c` |
| Acog Scope | missing | `shared/src/weapons/weapons.c`, `client/src/camera_feel.c:66-72` (zoom) |
| Sight reticles 0-11 | present | `client/src/ui/ui_loadout.c` |
| Scope overlay picker (aimRecticles) + scope settings | missing | `client/src/ui/ui_render_settings.c`, `ui_loadout.c`, `hud_ui.c` |
| Thrown-knife dmg 200 | present (v3) | `shared/src/projectile.c:21` |
| Nuke streak | partial (detonation only) | `shared/src/game.c`, `shared/src/player.c` |
| v7 streaks / slots / airdrop / juggernaut | missing | new |

---

### 1.2.7 Gameplay-altering v7 changes (policy: adopt v7.1.4 in place)

Policy (user decision, 2026-10-12): Krunker Box Office upgrades in place to v7.1.4.
Every row below is adopted as v7 defines it, with no v3.1.1 option; v3.1.1
stays on the `v3.1.1` branch/tag. The list says which changes alter v3.1.1
gameplay, so implementers know which tests and recordings to redo.

| # | Change | Alters v3.1.1 gameplay? | Re-verify |
|---|---|---|---|
| 1 | Class stats: Hunter speed 1.05->1.0; RnG HP 100->90; Spray HP 170->180, speed 0.95->0.9; Agent HP 100->110; Trooper speed 1.0->0.9 | yes | `class_spawn_test`; movement recordings use class 0 (Triggerman, unchanged) |
| 2 | Rocketeer gets a secondary | yes | loadout/spawn test |
| 3 | `wallJM` wall-jump caps (RnG 2, Agent 3, Trooper 3, Runner 100) | yes (movement) | new v7 wall-jump recording |
| 4 | Per-class slide `slT`/`slA` (Trooper 1.1, Spray 0.98) | yes (movement) | slide recording with a non-default class |
| 5 | Shared weapon stat changes (Sniper, AR, SMG, Revolver, Shotgun hs, MG, Semi, Rocket, Uzi, Deagle, Alien Blaster, Auto Pistol hs, Blaster reload; Sawed Off pellet pattern) | yes | weapon table unit test against v7 `weapons.js` dump; `v3_aim_walk` recording (aim speed) |
| 6 | MG `forceAttach` removed | yes (minor) | loadout |
| 7 | Rocket kill score 50->75 | yes (score) | `modes_test` / `match_test` expectations |
| 8 | Per-weapon `lsMlt`, crouch-aware leg zone, no leg mult on headshots, damage clamp | yes | `lagcomp_test` / hit tests |
| 9 | Projectile head/leg multipliers | neutral for v3 weapons | needed for new weapons |
| 10 | Charge system: Crossbow charge (20 hip / 200 full), knife-throw charge + dmg 200->180, overcharge (`chrgWeps` game setting, on by default) | yes | new charge test; input packet carries charge state (see 1.1) |
| 11 | Hitbox pad 1 -> 0.6 (`hitBoxPad` game setting) | yes | `lagcomp_test` hit/miss boundaries |
| 12 | Acog Scope (zoom x1.35) | no (opt-in attachment; FOV only) | - |
| 13 | New classes Survivor (hidden), Infiltrator | additive | `class_spawn_test` (ids 14/15) |
| 14 | New weapons 19-28 (Tehchy-9, Noob Tube, Zapper, Akimbo Pistol, Charge Rifle, Grappler, Build Tool, Slimer, Minigun, War Machine) | additive (respect `minRec`/`noComp`/`customOnly`) | per-weapon fire/projectile tests |
| 15 | upgradeWeapons + krumMachine | additive (maps that place a krumMachine) | map object test |
| 16 | Streak loadouts + Airdrop, Slimer, War Machine, Juggernaut | additive (default pick = Nuke only, as v3) | streak test |
| 17 | Nuke `reqAccount` and end-of-round timer clamp | yes (guests lose the nuke) | streak test |
| 18 | Class/weapon `req` gating for guests (Hunter/Vince/Detective/Bowman need an account) | yes (menu/spawn) | **Open question (5.1 item 1):** KN offline/guest play has no account; adopting v7 literally locks these classes for guests and offline. Suggest treating the local/offline player as "has account" (v7 itself waives `req` in sandbox and custom games). |
| 19 | Projectile stored[1] AI orb (dmg 60 -> 1 etc.) | map AI only | AI section, not player combat |

Visual-only items (adopt as-is): `aimRecticles` scope picker and scope
settings, charms, `reloY`, `recoilYM`/`recoilZ` viewmodel tweaks, Sawed Off sound
`weapon_16`, Marksman colors, `noMobile`, class faces.

Fix the two v3 parity bugs (`noHeadShot`, `movSpread`) as part of the v7
weapon work: both fields still exist in v7 with the same meaning.

## 1.3 Cosmetics, item types, skin effects, player mesh

Sources: v3 = `/home/user/jails/kru/src/Krunker_v3.1.1/src`, v7 = `/home/user/jails/kru/src/Krunker_v7.1.4/src` (skin table `Krunker_v7.1.4/skins.json`, which the client loads as `skins.jspck`, `src/data/skins.js:1-12`), KN = `/home/user/src/KrunkNative`.
Tags: [GAMEPLAY] changes shared or authoritative state (hitbox, packets the server must honour). [CLIENT] is renderer or UI only. [BACKEND] needs kn_brain (services/) support: inventory, account fields, or server-side tracking.

### 1.3.1 Item types (slots)

v3 `store.types` has 8 entries (`v3 src/data/store.js:94-103`). v7 `itemTypes` has 17 (`v7 src/data/itemTypes.js:1-18`), with asset folders in `v7 src/data/store.js:106`. KN models only the v3 player-dressing subset: `KN client/include/cosmetics.h:6-12` (types 1, 2, 5, 6, 7). The per-player net struct is `PlayerCosmetics` = skin colour, hair colour, hat, back, waist, face and dye (`KN shared/include/shared.h:507-518`). The loadout slot list for the v7 Customize tab is at `v7 src/index.js:5169`.

| id | v7 name | v3 | asset path (v7) | skins v3 / v7 | v7 attach point and offset (`v7 src/data/players.js`) | Tag | KN |
|---|---|---|---|---|---|---|---|
| 0 | WEAPON | yes | `weapons/weapon_` | 1521 / 3904 | weapon mesh (not this section) | [CLIENT][BACKEND] | present for v3 skins (`KN client/src/player_mesh.c:707-737`) |
| 1 | HAT | yes | `hats/hat_` | 184 / 270 | `hatMesh = genObj3D(0, playerHeight-legHeight-sitOff, -sitOffZ)` on lowerBody, loadMesh rot PI/2, scale `sclMlt*2.1` (:933-935). Same as v3 (`v3 src/data/players.js:2376`) | [CLIENT] | present (`KN client/src/player_mesh.c:557`) |
| 2 | BODY | yes ("back") | `body/body_` | 82 / 131 | `bodyMesh = genObj3D(0, (playerHeight-legHeight-headScale)/2-sitOff, -sitOffZ)`, scale `sclMlt*2.1` (:913-916). `noBody` hides the shirt cube | [CLIENT] | partial. KN calls it BACK (`COSMETIC_BACK`, `player_mesh.c:547`), the v3 name. v3's `player.backIndex` holds type 2 (`v3 players.js:2285`) |
| 3 | MELEE | yes | `melee/melee_` | 48 / 280 | weapon slot | [CLIENT][BACKEND] | present for v3 skins |
| 4 | SPRAY | yes | `sprays/` | 48 (+53 base `sprays.js`) / 466 | decal. In v7 every spray is a store item: base `game.sprays` is gone, default is skin 2482 "Krunker", 66 are `free`. **NEW: premium spray wheel** (`v7 src/index.js:16461`, bind `sprayWheel` F, `v7 src/libs/controls.js:207`) and `spraySnd` (`players.js:2007`) | [CLIENT][BACKEND] | v3 only: 53 base + type 4 (`KN client/src/sprays.c`, `SPRAYS_BASE_COUNT`). No wheel |
| 5 | DYE | yes | `dyes/` | 13 / 30 | recolours body cubes (:637-690). v7 adds `waistCol`, `hairCol`, `skinCol`, `useSkin`, array colours (random pick), and `tex`/`rgb`/`shaderNum`/`video`/`projection`/`tileS` on every body part (:855-905) | [CLIENT] | partial: colours only (`player_mesh.c:490-504`, TODO tex/opac/glow) |
| 6 | WAIST | yes | `waist/waist_` | 11 / 71 | `(0, (playerHeight-legHeight-headScale-2)/2-sitOff, -sitOffZ)`, `sclMlt*2.1`, `noLegs` (:924-931) | [CLIENT] | present (`player_mesh.c:552`) |
| 7 | FACE | yes | `faces/face_` | 10 / 544 | `(0, playerHeight-legHeight-sitOff, -sitOffZ)`, scale `sclMlt*1` (:943-947). Scaled by `headMlt` (:1449) | [CLIENT] | present for v3 faces (`player_mesh.c:561`) |
| 8 | **SHOE** | NEW | `shoes/shoe_` | - / 59 | Per leg i<4 on `meshHoldObj`: `genObj3D(±legScale/2, legHeight(-crouchDst-0.5 for crouch legs)-sitOff-0.01, -sitOffZ)`, mesh y `-legH`, scale `sclMlt*1`. Crouch legs get rotX -PI/6 or PI/8 (:951-968). Visibility follows the leg (:275-281, :416-419). `legLen` changes the leg's colour split (`v7 src/data/playerMesh.js:129`). `randomLeg`. `stepSnd`/`jumpSnd` replace footsteps (`players.js:1597-1636`). Hidden on lowSpec | [CLIENT][BACKEND] | missing (bots ignore `shoes`: `KN shared/src/bots.c:314`) |
| 9 | **PET** | NEW | `pets/pet_` | - / 13 | Separate follower object. `petHolder = genObj3D(3, player.height, 0)` on objInstances (:1334). See 1.4.1 | [CLIENT][BACKEND] | missing (`bots.c:302`) |
| 10 | **COLLECTIBLE** | NEW | `collectibles/collect_` | - / 26 | none: inventory-only crafting materials (Krystal, Wood, Cloth, ...) | [BACKEND] | missing |
| 11 | **WRIST** | NEW | `wrist/wrist_` | - / 1 (Krönix Gold, NFT) | First person only (`isYou`). On the merged arm: a marker cube from `genArms(..., hasWrist)` sets the centre and quaternion, rot [-PI/2,-PI/2,0], scale `sclMlt` (:1082-1089). Melee path: `armMeshes[0]` at `(-1.55-sitOffX, -1.4-sitOff, 1-sitOffZ)`, rot `[sxRot,syRot,szRot]` (:1127-1130). `noLeftSleeve` | [CLIENT][BACKEND] | missing (`bots.c:328`) |
| 12 | **CHARM** | NEW | `charms/charm_` | - / 7 | On weapon meshes, two slots (primary, secondary: `selectedCharms`, `v7 index.js:3261`). Mount `charms/charm_mount_0` at weapon `charmM{X,Y,Z}Off`, scale `charmMScl`. The charm sits at `charm{X,Y,Z}Off` (+0.17 y), rot -PI, scale `charmScl` (`players.js:1266-1291`). The weapon needs `charm` (weapons.js, 52 refs); skin `noCharm` blocks it. Sway anim (:501-504) | [CLIENT][BACKEND] | missing |
| 13 | **TICKET** | NEW | `tickets/ticket_` | - / 9 | none: an inventory voucher for a `spinWheel` (season wheels, Ultra `ticketOnly`) | [BACKEND] | missing |
| 14 | **BACK** | NEW slot | `back/back_` (meshes load from `body/body_` via `assetType=2`, `store.js:512-515`) | - / 229 | same anchor as BODY (:918-921) | [CLIENT][BACKEND] | missing as a separate slot. 74 v3 type-2 items were retyped 2->14 |
| 15 | **HEAD** | NEW slot | `head/head_` (meshes load from `hats/hat_`, `assetType=1`, `store.js:516-519`) | - / 362 | same anchor as HAT (:937-940). Scaled by `headMlt` | [CLIENT][BACKEND] | missing as a separate slot. 111 v3 type-1 "keyW Head" items were retyped 1->15 |
| 16 | **PLAYER_CARD** | NEW | `playercards/card_` (`.webp`, `store.js` post-loop) | - / 46 | 2D only: the death-UI banner background (`v7 index.js:15291`) plus a chosen `pcStatIndex` stat (13 stats, `v7 src/config.js:2883`; v3 had the 10-stat kill card, `v3 src/config.js:2053`) | [CLIENT][BACKEND] | missing. KN has the v3 kill card (`KN client/src/ui/hud_ui.c:1825`, `ui_loadout.c:112` kcStatIndex) |

Notes:
- **Premium slot rule (new).** Non-premium accounts cannot equip HAT+HEAD or BODY+BACK together. The Customize card shows "Premium Required to use Both" (`v7 src/index.js:5169-5209`, the 5th tuple element points at the conflicting slot). [CLIENT][BACKEND]
- **Same skin index, new type.** The first 1914 of v3's 1917 `skins` keep their index in v7, and only types 1->15 and 2->14 changed. KN's `assets/skins.json` (1917) and the inventory ids (`KN services/db/migrations/postgres/0102_items_creations.sql:5`) stay valid, but the slot of 185 items changes.
- **Spawn packet.** The v7 per-player spawn packet carries `shoeIndex, waistIndex, faceIndex, petIndex, badgeIndex, wristIndex, headMlt, charms, backIndex, headIndex, playerCardIndex, isBot` (`v7 src/data/players.js:5002`). The live re-dress broadcast is `rg` (`v7 src/data/player.js:535`, pet at :553). KN `PlayerCosmetics` has 7 int32 and needs extending (shared packet change, `KN shared/include/shared.h:507`).
- **Cosmetic overrides.** The custom-game config `noCosm` "Hide Cosmetics" (`v7 src/config.js:822`) clears every slot (`players.js:599-611`). Mode `modelOverrides` per team (`noCosmetics`, forced indices and colours: :697-773). Juggernaut forces items 4908-4911 (:774-779). [CLIENT]
- **Data tables.** v7 `store.templates` (`store.js:474-508`) are field presets merged into skins. `store.tags` (`store.js:76-105`, 30 tags: map rewards, Craftable, NFT, Ranked, FRVR gradient) replace v3 `events` (8, `v3 store.js:84`). [CLIENT]

### 1.3.2 Rarities, tags, prices

| Item | v3 | v7 | Tag | KN |
|---|---|---|---|---|
| Rarities | 7: Uncommon..Unobtainable (`v3 store.js:192-265`) | 8: adds **NFT** (7, `#f3b8ff`, noSpin). Contraband and Unobtainable gain `scrap` 5 and 50 (junk) (`v7 store.js:233-309`, `v7 src/data/rarities.js:1-10`) | [CLIENT][BACKEND] | v3 7 (`KN client/src/viewer/viewer_store_table.c`) |
| Unobtainable rainbow | `animate:true` + `rainbowText` when `RENDER.rarityAnim` | same. Setting `disableRarityAnim` (`v7 src/libs/settings.js:803`) | [CLIENT] | present (`disableRarityAnim` in KN settings) |
| Item req gate | none | `checkReq` (`accAge` / `stats` / `accVar`, e.g. "USS Krunk" needs stats c9 >= 2667778) (`v7 store.js:310-323`) | [BACKEND] | missing |
| Prices | static 1917-entry array (`v3 src/data/prices.js`) | empty table filled by the API (`update/change`, `v7 src/data/prices.js:1-15`) | [BACKEND] | v3 static |
| Wheels | 7 (Starter, Elite, Heroic, Valour, Free, Free KR, Twitch) (`v3 store.js:1-83`) | 5 (Starter/Elite/Heroic `multi:10`, Ultra `ticketOnly`, Free KR) + `ticketIndex` (`v7 store.js:4-63`). `lootRars` changed (:232) | [CLIENT][BACKEND] | v3 wheels (`KN client/src/ui/store_ui.c:29-37`, `services/brain/store.c`) |
| Premium | 30/60/90 days for 7500/14000/19000 KR (`v3 store.js:170`) | real money 7/30/60/90 days, $1.49-$12.99 (`v7 store.js:209-231`). Perks: zero trade fee (`calcTradeFee`, :339), both hat+head / body+back, spray wheel, alias | [BACKEND] | v3 premium flag only |

### 1.3.3 Skin effects (rendering)

v3 already has `ShaderToyMaterial`, pulse, frame, rgb and glow (`v3 src/libs/render.js:77-80, 431, 756-757`). v7 adds the entries marked NEW. Item materials go through `generateMeshData` (`v7 src/data/players.js:553-592`) -> `RENDER.getMat` (`v7 src/libs/render.js:1194-1360`). Counts are v7 skins using the field, with v3 in brackets.

| Field | v7 (v3) | Render behaviour (v7) | Tag | KN |
|---|---|---|---|---|
| `glow`/`sameGlow`/`emiss`/`noEmis` | 2150 (426) | `emissive = emiss \|\| #fff`, emissiveMap `<texSrc\|\|src>_e` (or the texture itself with sameGlow) (`render.js:1253-1254`) | [CLIENT] | weapons/melee only (`player_mesh.c:443-466`). Cosmetic meshes have no glow (TODO `player_mesh.c:508`) |
| `glowInt` NEW | 1 | `emissiveIntensity = emissInt` (`render.js:1256`) | [CLIENT] | missing |
| `pulsT`/`minGlo` | 750 (104) | `emissiveIntensity = (sin(pulsVal)/2+0.5)*1.5 + minGlo`, `pulsVal += dt*0.003`. pulsT itself is only a flag/cache key (`render.js:842-846`) | [CLIENT] | viewer only (`KN client/src/viewer/viewer_scene.c:172-186`). In game: TODO |
| `frames`/`frameT` | 141 (33) | sprite sheet: `map.offset.x = cur%numX/numX` every frameT ms (`render.js:847-861`) | [CLIENT] | viewer only. In game the repeat is set but frame 0 is static (`player_mesh.c:458`) |
| `rgb`/`rgbS`/`rgbT` | 23 (7) | hue cycle on emissive (or colour for Basic) `hexFromHue(dlt)`, `dlt += dt*rgbS(0.3)` (`render.js:863-878`) | [CLIENT] | missing |
| `movT`/`movD`, `rotT` NEW | 40 (25), rotT 0 | texture offset scroll / texture rotation (`render.js:817-826`) | [CLIENT] | viewer only |
| `rAnim`/`rAnimA`/`rAnimT`/`rAnimM` | 50 (13) | `movMeshes`: spin about an axis, or bob `sin*0.55` (`render.js:37-43, 827-836`) | [CLIENT] | viewer only |
| `wire` | 22 (6) | `useWireframe` | [CLIENT] | present (`player_mesh.c:456`; GLES has no polygon mode, see AGENTS.md) |
| `trans`/`opac`/`alpha`/`side` | 665/15/164/493 | transparent, opacity, alphaTest, DoubleSide | [CLIENT] | present |
| `shaderNum` (v3 `shaderId`) | 13 (5) | `THREE.ShaderToyMaterial(shaders[n].code)`: a RawShaderMaterial wrapping Shadertoy `mainImage`, uniforms `iTime, iTimeDelta, iResolution (1500 x 1500/aspect), iFrame, iChannel0-3, iChannelResolution, iDate, iMouse`, UV varying `vUv` (`v7 src/133.js:269-340`). 8 shaders, ids 0-7 (`v7 src/data/shaders.js`). v3 had 4 without ids ("Glob" was removed, so indices shifted: v3 Purple Plasma = 3, v7 = 2) | [CLIENT] | missing |
| `env`/`sameEnv` NEW | 21 | envMap `<tex>_env` (mapping 3), `envMapIntensity = env\|\|3` (`render.js:121-131, 1290-1293`) | [CLIENT] | missing |
| `video` NEW | 2 (Deep Space dye, Mysterious Orb melee) | `<video>` texture `videos/video_<n>.mp4` (`render.js:1180-1209`) | [CLIENT] | missing |
| `projection` NEW | 2 | screen-space UVs: patches map/emissive fragment chunks with `vTexCoords` (`render.js:1314-1318`) | [CLIENT] | missing |
| `hasAnim`/`animS`, `fileExt` gltf/glb NEW | 22 (types 0, 3, 11, 14) | `loadAnimMesh` + `AnimationMixer`, `timeScale = animS`, morph targets (`render.js:63-175, 2231`) | [CLIENT] | missing (OBJ only) |
| `snd`/`killSnd`/`stepSnd`/`jumpSnd`/`reloadSnd`/`spraySnd` NEW (beyond 3 `snd`) | 10/6/5/... | per-skin sounds (`players.js:1597-1636, 2007, 4924, 4937`). Setting `skinVolume` (`settings.js:2945`) | [CLIENT] | missing (`KN client/src/sound_events.c` has no skin sounds) |
| `explode`/`particle` NEW | 2/2 | weapon-skin kill VFX | [CLIENT] | missing |
| `texExt` (webp) | 46 | texture extension | [CLIENT] | missing |

`effects.js` in v7 (`v7 src/data/effects.js:1-126`) is **not** skin effects. It holds 10 timed status effects / perks (Boosted, Sleight of Hand, Rapid Fire, Siphon, Rooted, Dizzy, Poisoned, Jump Boost, Zapped, Strength). They change `player.attributes` and are [GAMEPLAY], so they belong in the weapons/perks section.

### 1.3.4 Player mesh

| Item | v3 | v7 | Tag | KN |
|---|---|---|---|---|
| Dimensions | playerHeight 11, chestWidth 2.6, chestScale 1.3, armScale 1.2, legScale 1.3, u/lArmLength 2.7, headScale 2, legHeight 4.2, crouchDst 3 (`v3 src/config.js:1238-1259`) | identical (`v7 src/config.js:2149-2165`) | - | present |
| Mesh generator | `RENDER.genBody/genLeg/genArms` in render.js, with the head merged into the body (`v3 src/libs/render.js:885-1115`) | moved to the shared `src/data/playerMesh.js` (runs on the server too). **Head split** into `genHead` (`playerMesh.js:138-163`) so it can scale. `genBody` adds `noBody`, waist band; `genLeg` adds `legLen` (:84-136); `genArms` adds `hasWrist` marker, `noSleeve`, `noLeftSleeve`, `noRightHand`, `armS`, per-skin `left/rightHoldX/Y/Z` (:193-322) | [CLIENT] | partial: v3 shape (`KN client/src/player_mesh.c`), head already separate (`player_mesh.c:611`) |
| Head multiplier `headMlt` | none | the head, face, hat and head meshes scale by `headMlt`, and `adjustedHeight = playerHeight-headScale+headScale*headMlt` (`players.js:260-261, 1440-1466`). `ah` feeds the hit AABB top and the headshot line (`players.js:2228, 2345`; `projectiles.js:295`). Sent in the spawn packet; nothing in 7.1.4 sets it above 1 except bots/sandbox (`server/NextBotManager.ts:164`) | **[GAMEPLAY]** | missing |
| Hitbox pad | `game.mode.hitBoxPad \|\| 1` (one mode 0.5) (`v3 players.js:3697-3702`, `v3 modes.js:800`) | `hitboxPadW/H = game.config.hitBoxPad + class hbS/hbH` (`v7 src/data/player.js:270-279`). The server-config default is **0.6** public / **0.1** comp host (`v7 config.js:621-630`, applied `game.js:522`). Custom class `hbS`/`hbH` 0-20 (`config.js:1368-1387`) | **[GAMEPLAY]** | v3 rule (`KN shared/src/player.c:1793`, `shared/src/projectile.c:193`, `shared/include/modes.h:164`) |
| Mesh hitboxes | none | the server builds padded meshes (`hitBoxPadP` 0.5) and raycasts them if `player.newHitboxes` (`players.js:2344-2362`). That flag is **never set** in 7.1.4, so the path is dead. Constant `hitBoxPad = 5` (`config.js:334`) is used only there | [GAMEPLAY] (inert) | n/a |
| Model files | 124 in `public/models` (props + cosmetic dirs) | cosmetic dirs: hats 510, body 305, back 171 (byte-identical copies of `body/`, unused), waist 92, faces 7 (+565 textures), shoes 65, pets 17, charms 39 (+mount), wrist 6, collectibles 25, tickets 1. `.gltf` appears (e.g. `back_103.gltf`) | [CLIENT] | v3 models only (hats 199, body 102, waist 15, faces 2, shoes 3). Textures for pets/charms/back already exist under `KN assets/textures/`; models missing |

## 1.4 Pets, badges, medals, challenges, account

### 1.4.1 Pets

| Aspect | v7 | Tag | KN |
|---|---|---|---|
| Data | 13 type-9 skins: Ghostie, Pumpie, Brainie, Pay Piggie, Flying Dutchman, Assault Drone, Bruisegg, Forbidden Tome, Overlord, Attack Drone, Festive Flyer, Paper Airplane, Brainrotie. Optional `spd`, `sclMlt` (0.7 on drones), the usual glow/frames/env/shaderNum/hasAnim | [BACKEND] inventory | missing |
| Follow AI (`v7 src/data/pets.js:3-97`) | Target is `owner.petHolder` world position (3 units right, at player height). Speed `spd \|\| 0.12`/ms, scaled down linearly within 40 units. 3D move toward the target. Bob `sin(bobY)*0.2` with `bobSpd 0.002`. Yaw turns toward travel at <= `0.01*dt` rad. Default scale `sclMlt \|\| 0.8`. Added to the scene root (or the preview parent). Client-only: no server state, created when RENDER exists | [CLIENT] | missing |
| Lifecycle | `game.pets.add/remove` on mesh regen and dispose (`players.js:1349-1350, 1391, 1579`). Class preview shows it (`v7 index.js:3955-3993`) | [CLIENT] | missing |
| Hiding | no dedicated setting. Hidden by the custom config `noCosm` (`players.js:608`), by the Prop team in Prop Hunt (:612-614), and by `game.disableMeshes`. Loadout "None" (-1) | [CLIENT] | n/a |
| Selection | `petIndex` localStorage + the `en` packet (`v7 index.js:3256, 1391`), spawn packet field | [CLIENT] + shared packet | missing |

### 1.4.2 Badges (new in v7)

`v7 src/data/badges.js:1-202`: **29 entries** over 17 icon ids, URL `textures/badges/badge_<id>[_<tex>].png` (:199-202).

| Group | Entries | Unlock | Server tracking |
|---|---|---|---|
| Legacy Challenger | 1 | "Completed Old Challenge Lvl 30" (v3 ladder) | granted (DB) |
| Skin Creator I-IV | 4 | 50/100/200/300 skins added | granted |
| KPD ranks: Deputy, Officer, Sergeant, Lieutenant, Captain, Major | 6 | staff | granted |
| Translators, Gratitude I-III, Map Maker I-III | 7 | staff | granted |
| Nuclear I-IV | 4 | `stats.n` >= 500/750/1000/1500 nukes, `claimable` | auto |
| S6 Founder, S6 Map Master, S6 BP Master, S6 Badge Master | 4 | BP bought before the end date; own all 17 S6 map rewards (5237-5253); `BP.bpp >= 15000`; badges 22+23+24 | auto |
| S7 Founder, S7 BP Master, S7 Badge Master | 3 | same rules for season 7 | auto |

- **Award.** The server runs `checkBadges` at match end for public games and calls DB proc 181 (`v7 src/data/game.js:4385-4392`). The client claims via the menu (`v7 index.js:3720-3721`). The account holds `badges` from `player_badge` CSV (`v7 src/data/account.js:56`). The selected badge is `stats.bdg`, gated by the `customBadge` setting (`v7 settings.js`, gameprivacy).
- **Where it shows.** Nametag image (`v7 src/libs/game-overlay.js:1010-1011`), side leaderboard (`v7 index.js:15425`), Tab/centre leaderboard (:15473), menu account header (:3935), Profile/Customize tab next to the flag picker (:5639-5650). The spawn packet carries `badgeIndex`.
- **Tags.** [CLIENT] display + [BACKEND] badge table, stats.bdg, claim logic.
- **KN.** Missing. KN only has the v3 icon badges (partner `verified`, premium `beenhere`, featured `check_circle`: `KN client/src/nametags.c:50-58, 427-460`), which v7 keeps alongside. v7 adds the privacy toggles `verifiedBadge`, `premiumBadge`, `partnerBadge`, `customBadge`, `showRegion` (`v7 src/libs/settings.js`, cat `gameprivacy`), all missing in KN.

### 1.4.3 Medals

- **`v7 src/data/medals.js` is not kill medals.** It holds the **25 pickup/collectible objects**: Tag, Gift, Golden Egg, KR Tag, Diamond Egg, Slime Pad, Ammo Pack, Weapon Box, Melee Pickup, Ammo Cache, Nuke, Minigun, Insta-kill, Discount, Double Points, Grenade Launcher, Joe Tag, Frog, Krunkanium, Perk Bottle, Zombie Portal, Skull EE, Free Upgrade, Mega Slime Pad, Loot Drop. Each has model, scale, timer and onPickup. These are [GAMEPLAY] and belong in the modes/pickups section.
- **Kill medals.** The names are identical in v3 and v7: 26 in `kills.js` (First Blood, Headshot, Wallbang, Double..Super Kill, Driftkill, Mid Air, ...). New in v7:
  - Per-account medal counting: `game.incMedal(name, player)` for pub games and sandbox (`v7 game.js:1125-1132`), 31 call names including mode medals (Confirmed, Denied, Capture, Bomb Planted, ...). Sent to the end screen as `endData.mdls` (`game.js:4693-4694`). [BACKEND] for persistence, [CLIENT] for the end screen.
  - Medal settings `classicMedals`, `playMedals`, `medalScale`, `medalOffX/Y`, `medalSat`, `medalHue`. v3 had only `showMedals`. [CLIENT]
- **KN.** Kill medal popups present (`KN client/src/ui/hud_popups.c`). `showMedals` present. Per-account counts, the end-screen medal list and the new medal settings are missing.

### 1.4.4 Challenges

| | v3 | v7 |
|---|---|---|
| Model | static ladder: **26 levels**, each 2-4 of **21 templates** (Hitman, Headhunter, Assassin, Streakster, ..., Speed Demon), rewards `funds` (KR) per level (`v3 src/data/challenges.js:17-552`). Level stored in `account.chal`; name-box colour `getChallCol` (>20 red, >15 purple, >10 orange) (`v3 src/data/colors.js`) | **data-driven** rows from the DB: `activeChallenges[] = {CI, T (daily/weekly/ranked/season), DAT{ky,val,wep,cls,mp,mid,md,stk,item,lesser,sm,nem,evt,seas}, REW{kr,jnk,sk,bpp}, PV, PT}` (`v7 src/data/challenges.js:4-15`). 55 tracked keys (47 additive, 5 set, 3 max: :16-18). Meta "chall" = complete N other challenges. Rewards KR, junk, skins and Battle Pass points via DB procs 175/176 (:158-213). `getChallCol` removed |
| Min level | `minChalLevel 30` (`v3 config.js:184`) | 1 (`v7 config.js:181`) |
| Packets | `chg` | `uchp` (progress), `chg` (results), `chgC` (completed), `chlR` (rewards) |
| Tag | [BACKEND] | [BACKEND] (server-side progress per stat key) + [CLIENT] UI |

KN: **partial, v3 only.** The window sends `["chg"]` (`KN client/src/ui/account_ui.c:1171-1176`), but kn_brain has no `chg` handler, so the window shows "Loading". A `challenge` op only sets `player_chal` + KR (`KN services/brain/accounts.c:1114-1121`, `db_accounts.c:414-433`). The challenge colour level box is in `KN client/src/ui/menu_ui.c:670-674`. The game server commits only kills/deaths/score/won/time/games/kr/xp/class (`KN server/src/accounts.c:439-461`), so no per-key stats (hs, n, wb, ...) reach the brain. Both v7 challenges and v7 badges need those keys.

### 1.4.5 Account and profile fields that affect the client

Compare `v3 src/data/account.js:1-153` with `v7 src/data/account.js:1-206`. KN schema: `KN services/db/migrations/postgres/0100_accounts.sql:38-73`. KN client: `KN client/src/ui/account_ui.c`.

| Field / feature | v3 | v7 | Tag | KN |
|---|---|---|---|---|
| Level | `max(1, floor(0.03*sqrt(score)))`; levelProg = fractional part *100 (linear in sqrt) | same level. `getLevelProg` is **linear in score** between `(L/0.03)^2` and `((L+1)/0.03)^2` (:117-127). maxLevel 102 icons, unchanged | [CLIENT] | v3 formula (`account_ui.c:172-176`) |
| KR (`funds`), `junk` NEW | funds | funds + `junk` (`player_jnk`, KR scrap from Contraband/Unobtainable `scrap`, crafting) | [BACKEND] | funds only |
| Premium | `premiumT`, alias | same + perks (1.3.2) + `premiumBadge` toggle | [BACKEND] | premiumT/alias present |
| Verified/featured, partner | `featured`, `partnerApp` | same; nametag `partner == 2` (`game-overlay.js:1007-1009`) | [CLIENT] | present (`nametags.c:58`) |
| Roles | `isMod`, `isDev`, `isCreationMod` | `role` (2 mod, 3 deputy), `isAdmin`, `isSkinMaker`, perms `canBadge`, `canSilentKick`, `canRehost`, `canUseKRTrigger` (:128-149) | [BACKEND] | v3 dev/mod |
| Clan | `clan`, `clanRank` (rank colours: 1 rainbow, 2-5 red, 6-15 gold, 16-25 pink, 26-50 blue), `clanCon`, `clanRegion` | `clanRole`, `clanTime`. Rank colours **removed**: `getClanCol` keeps only DEV/VIP/CMD (`v7 src/data/colors.js:7-18`) | [CLIENT][BACKEND] | v3 rank colours (`nametags.c:163-186`) |
| Badges, BP, challenges | - | `badges[]`, `BP{tier,bpp}` (season pass, `bppCapDay 300`, `v7 config.js:169`), `activeChallenges` | [BACKEND] | missing |
| Inventory | `skins` index list | `skins[{ind,cnt,a}]` with counts; `invVal`, `trades`, `metamask` (NFT/opensea items) (:150-202) | [BACKEND] | count-based inventory present (`0102_items_creations.sql:5`) |
| Removed in v7 | `elo/elo2/elo4`, `chal`, `virus`, `followInd`, `clanCon/Region/Rank`, `has2FA`, `isMod` | - | - | KN keeps the v3 set |
| Flag | `stats.flg`, 240 flags | identical list (`v7 src/data/flags.js`) | [CLIENT] | present (`KN client/src/ui/account_flags.h`) |
| Region | `regionInd` | same + `showRegion` privacy toggle | [CLIENT] | toggle missing |
| Class levels | `stats["c"+class]` | shown on the death UI ("lvl N") and in the class menu | [BACKEND] | stats JSON present |
| Name colour | `chatCol` (premium) | same | [CLIENT] | present (`ui_loadout.c:107`) |
| Menu backgrounds | - | `backgrounds.js`: 1 entry ("Midnight Snow"), used by the `selectImage` setting picker (`v7 index.js:916, 953`) | [CLIENT] | missing (trivial) |
| Profile picture | `showPfp` | same | [CLIENT] | - |
| Death UI | v3 kill card + stat | player-card banner, class level, killer stat, KD vs (`v7 index.js:15291`; server packet `3`, `players.js:4860`) | [CLIENT] + packet | v3 kill card |

### 1.4.6 Gap summary (KN owner files)

Policy: adopt v7. KN upgrades in place to the v7.1.4 player mesh, hitbox, cosmetics and account model, with no v3.1.1 option (v3.1.1 is preserved on the `v3.1.1` branch/tag). The [GAMEPLAY] items below still change shared state.

- **Slots.**
  - Add HEAD (15), BACK (14, split from BODY), SHOE, PET, WRIST, CHARM, PLAYER_CARD (`client/include/cosmetics.h`, `client/src/cosmetics_table.c`, `client/src/player_mesh.c`, `client/include/client.h:327`).
  - Extend the `PlayerCosmetics` packet struct (`shared/include/shared.h:507`) and the ENT/SPAWN packets.
  - Port the v7 skins table: 6448 vs 1917 (`client/src/viewer/viewer_store_table.c`, generator `tools/ui-import/gen_viewer_store.js`, `assets/skins.json`).
- **Skin effects.** In-game glow on cosmetics; pulse/frames/movT/rAnim/rgb at runtime (they exist only in `client/src/viewer/viewer_scene.c`); ShaderToy material (8 shaders); envMap, video, projection, GLTF animation, skin sounds.
- **[GAMEPLAY] items.**
  - adopt the v7 hitbox: pad 1 -> 0.6 (public) / 0.1 (comp) plus class `hbS`/`hbH` (`shared/src/player.c:1793`, `shared/src/projectile.c:193`)
  - `headMlt`/`adjustedHeight` affecting hit AABB and headshots
  - The mesh-hitbox path is dead in 7.1.4 and does not need porting.
- **[BACKEND].** badges, junk, BP, data-driven challenges and per-stat commit keys, `checkReq` gates, NFT rarity, tickets/collectibles/crafting, real-money premium perks (`services/brain/accounts.c`, `store.c`, `db/migrations/postgres/0100_accounts.sql`, `server/src/accounts.c`).

## 1.5 Controls, HUD, killcam, spectate, emotes/sprays/voice

Scope: keybinds and input mechanics (keyboard/mouse, gamepad, touch), the in-game HUD, the death screen / kill camera / spectator mode, and the social in-match features: sprays, taunts, quick chat, text chat and voice chat. The full settings list is in section 1.7; only settings that change how input or these features behave are repeated here.

Sources: v7 = `/home/user/jails/kru/src/Krunker_v7.1.4`, v3 = `/home/user/jails/kru/src/Krunker_v3.1.1`, KN = `/home/user/src/KrunkNative`.

Tags: **[GAMEPLAY]** = touches shared state, or a packet other clients or the server depend on. **[CLIENT]** = local presentation or input only.

Status per item: v3 -> v7 is **NEW / CHANGED / SAME / REMOVED**. KN is **present / partial / missing**, with the KN file that owns it.

Headline facts:
- **v7.1.4 has no emote wheel and no team ping/marker system.** No `emote` identifier and no ping-marker packet appears anywhere in v7 `src/`. The only emote-like features are the Prop Hunt taunts (`prpt`) and the quick chat messages.
- **v7 has no replay killcam.** "Kill cam" in both versions is the chase camera that turns toward the killer during the 2800 ms `deathDelay`. v7 only makes it switchable per game (`kCams`).
- KN is a faithful port of the **v3.1.1** controls/HUD/spectate/voice/spray layer: 38 bind ids (`client/src/ui/ui_controls.c:137-170`) and the v3 HUD DOM in `ui/index.html`. None of the v7 additions below exist in KN yet.
- Policy: KN upgrades in place to v7.1.4, and v3.1.1 is kept only on the `v3.1.1` branch/tag. Every CHANGED or REMOVED row below therefore means **adopt v7**: v7 defaults, v7 storage keys and v7 packet shapes, with no v3 option. Elements v7 removed are deleted from KN.

### 1.5.1 Keybind model

| Aspect | v3.1.1 | v7.1.4 | Tag | KN |
|---|---|---|---|---|
| Bind object | Plain `{def, val}` per bind; arrays for move/streak/toggle/premium (`v3 src/libs/controls.js:914-1035`) | `ControlBind(name, def, altDef)` with `val` **and `altVal`**, `isKey()` matching either, `reset/resetAll/unbind` (`v7 src/libs/controlBind.js:1-41`). **Every action gets a second (alternate) key.** | [CLIENT] | **missing**: single key per id (`KN client/src/ui/ui_controls.c:177`) |
| Storage | `localStorage cont_<name>` / `cont_<arrayId>` | Same, plus `cont_<name>_alt`. Shoot and aim **renamed**: `cont_shootKey` -> `cont_shoot1Key` and `cont_aimKey` -> `cont_aim1Key`, so saved v3 values for those two do not carry over (`v7 src/libs/controls.js:210-211`) | [CLIENT] | v3 keys (`KN ui_controls.c:154-158`) |
| Mouse buttons | LMB/RMB are hard-wired to shoot/aim (`controls.isShooting` / `isAiming`). Only middle/back/forward reach the bind table as `10000+which` (`v3 src/libs/controls.js:396-433`). `shootKey` 220 and `aimKey` 221 are keyboard extras | **All** mouse buttons go through binds as `10000+which` (`v7 src/libs/controls.js:580-618`). Shoot defaults to **10001 (LMB), alt 220 (`\`)**; aim to **10003 (RMB), alt 221 (`]`)**, so LMB/RMB can be rebound. The controls window draws `img/mouse/<code>.png` for 10001/10002/10003/20000 (`v7 src/index.js:6226-6231`) | [CLIENT] | **partial**: `KN ui_controls.c:268-276` maps all GLFW buttons to 10000+n, but defaults follow v3 (shoot 220, aim 221, LMB/RMB fixed) |
| Wheel as key (20000) | Every wheel event fires key 20000 (`v3 src/libs/controls.js:514`) | Throttled to one key 20000 per **50 ms**. The spray and prop wheels take the scroll when open (`v7 src/libs/controls.js:660-708`) | [CLIENT] | v3 behaviour (`KN ui_controls.c:31`) |
| Controls window | One list with section headers (`v3 src/index.js:4200-4370`) | 5 tabs: **General / Chat & Toggles / Interaction / Spectate & Vote / Modes**. Collapsible groups, a reset (refresh) button and an unbind button per key and per alt key, an inline Toggle/Hold `<select>` for binds that have a `<bind>Trigger` setting, and quick-chat `<input>` rows (`v7 src/index.js:6156-6235`) | [CLIENT] | **partial**: v3 list (`KN ui_controls.c:463-497`) |
| Bind labels in the HUD | Hard-coded `[1]`/`[2]` etc. | `updateKeybindUI` writes the current key into `#speedRunRstKey`, `#kickVoteY/N`, `#specObjCam`, `#specFreeCam`, `#specNamesF`, `#specFirstP`, `#specFocus`, `#kpdVision`, `#kpdVoteY/N`, `#taunt0-5`, `#pickRandProp`, `#pickProp`, `#rotProp`, `#rotRProp`, `#specMinimap` (`v7 src/index.js:2578-2650`) | [CLIENT] | **missing** |

### 1.5.2 Every v7 bindable action

Key codes are DOM `keyCode`s; `-1` means unbound. "v3" is the v3.1.1 default, or "hard-coded N" when v3 handled the key in `pressButton` without a bind. v7 binds are defined at `v7 src/libs/controls.js:195-245` and dispatched in `v7 src/index.js:16316-16591` (`pressButton`/`releaseButton`). v3 dispatch is at `v3 src/index.js:12259-12447`. KN dispatch is `press_button` (`KN client/src/ui/ui_controls.c:768-813`).

| v7 bind (storage key) | Label (en) | v7 default (alt) | v3 | v3->v7 | Effect / packet | Tag | KN |
|---|---|---|---|---|---|---|---|
| move[0..3] (`cont_0..3`) | Forward/Backward/Left/Right | 87 W, 83 S, 65 A, 68 D | same | SAME | input frame `moveDir` | [GAMEPLAY] | present `ui_controls.c:162` |
| jump (`jumpKey`) | Jump | 32 Space | 32 | SAME | input bit | [GAMEPLAY] | present |
| crouch (`crouchKey`) | Crouch | 16 Shift | 16 | SAME | input bit | [GAMEPLAY] | present |
| reload (`reloadKey`) | Reload | 82 R | 82 | SAME | input bit | [GAMEPLAY] | present |
| aim (`aim1Key`) | Aim | **10003 RMB (alt 221)** | RMB hard-wired, `aimKey` 221 | CHANGED | input bit = `adsToggled`; per-weapon **toggle ADS** (see 1.5.3) | [GAMEPLAY] | partial (v3 model) |
| shoot (`shoot1Key`) | Shoot | **10001 LMB (alt 220)** | LMB hard-wired, `shootKey` 220 | CHANGED | input bit | [GAMEPLAY] | partial (v3 model) |
| primary (`primKey`) | Primary Weapon | 84 T | 84 | SAME | `wSwap=3` | [GAMEPLAY] | present `ui_controls.c:822` |
| melee (`meleeKey`) | Melee Weapon | 81 Q | 81 | SAME | `wSwap=2` | [GAMEPLAY] | present |
| swap (`swapKey`) | Secondary Weapon | 69 E | 69 | SAME | `wSwap=1` | [GAMEPLAY] | present |
| equip (`equipKey`) | Equipment | 67 C | 67 | SAME | `wSwap=4` | [GAMEPLAY] | present |
| inspect (`inspKey`) | Inspect | 88 X | 88 | SAME | `Game.players.wInspect` (local anim) | [CLIENT] | present `ui_controls.c:790` |
| spray (`sprayKey`) | Spray | 70 F | 70 | CHANGED packet | v3 `io.send("int",0)`, v7 **`"spry"`** (no index) | [GAMEPLAY] | present (v3 `int 0`) `ui_controls.c:783` |
| sprayWheel (`sprayWheelKey`) | Spray Wheel (Hold) | 70 F | — | **NEW** | premium/sandbox only: hold opens the wheel after 200 ms, release sends **`"spry", index`** (`v7 src/index.js:16450-16461,16573-16580`) | [GAMEPLAY] | missing |
| playerList (`pListKey`) | Player List | 18 Alt | 18 | SAME | window 23 | [CLIENT] | present `ui_controls.c:773` |
| scoreBoard (`sBoardKey`) | InGame Leaderboard | 9 Tab | 9 | CHANGED | shows `#centerLeaderDisplay`; **`scoreBoardTrigger`** 0 Press (toggle) / 1 Hold (default) (`v7 src/index.js:15498-15505`, `v7 src/libs/settings.js:4265`) | [CLIENT] | partial (hold only) |
| interact (`interactKey`) | Interact | 71 G | 71 | CHANGED packet | v3 `"int",1`; v7 **`"int", type, id`** from `lastInteractData`; also held in input frame field 14 | [GAMEPLAY] | partial: `ui_terminal_interact` (`ui_controls.c:787`) |
| secondaryInteract (`interactSecKey`) | Secondary Interact | 72 H | 72 | CHANGED packet | v3 `"int",1,true`; v7 **`"intSec", type, id`** | [GAMEPLAY] | partial `ui_controls.c:788` |
| confirm (`confirmKey`) | Confirm Interact | 75 K | — | **NEW** | `"cnfm"` for a `pendingTrigger`; `"cnfm",2` for a `pendingSpin` | [GAMEPLAY] | missing |
| drop (`dropKey`) | Drop | 90 Z | 90 | CHANGED packet | v3 `"int",2`; v7 **`"drpw"`** | [GAMEPLAY] | **missing**: logs "not available" (`ui_controls.c:789`) |
| chat (`chatKey`) | Chat | 13 Enter | 13 | SAME | focus/send `#chatInput` | [CLIENT] | present `ui_controls.c:671` |
| voiceChat (`voiceKey`) | Voice | 86 V | 86 | CHANGED | push-to-talk, or toggle via **`voiceChatTrigger`** 0 Press / 1 Hold (default) (`v7 src/index.js:2292-2300`) | [CLIENT] | partial: hold only (`ui_controls.c:733`) |
| wepVis (`wepVisKey`) | Toggle Weapon Visibility | -1 | -1 | SAME | flips `showWeapon`/`Sec`/`Mel` | [CLIENT] | present `ui_controls.c:794` |
| streak[0..2] (`cont_18..20`) | Streak Slot 1-3 | 49, 50, 51 | **5 slots** 49-53 (`cont_18..22`) | CHANGED | v3 `"k", i`; v7 **`"k0".."k2"`** event names (`callInStreak`, `v7 src/index.js:15311-15318`; server `v7 server/index.ts:3329`). Slot -> streak through `player.streakIndexs` (streak loadout) | [GAMEPLAY] | **missing**: logs "not available" (`ui_controls.c:791-793`) |
| toggle[0..5] (`cont_27..32`, `_set`) | Toggle <setting> | -1 x6 | same | SAME | flips a chosen `toggle:true` setting | [CLIENT] | present `ui_controls.c:799` |
| premium[0..3] (`cont_34..37`) | Noclip / Godmode / Kill / Smite | -1 x4 | same | SAME | `"prmA", i` | [GAMEPLAY] | present `ui_controls.c:805-812` |
| message[0..5] (`cont_43..48`, text `cont_43_set..`) | Chat Messages | -1 x6 | — | **NEW** | quick chat: sends the stored text via `"ct"`; 1500 ms cooldown; `filterCM` strips `<>'"` (`v7 src/index.js:16222-16244`, `:2525-2529`) | [GAMEPLAY] (chat) | missing |
| hidePlayers (`hidePlayersKey`) | Hide Players | -1 | — | **NEW** | `Game.hidePlayers` (all non-bot players hidden locally, `v7 src/index.js:14814,15104`) | [CLIENT] | missing |
| kickVoteY / kickVoteN | Vote Kick [Yes]/[No] | 49 / 50 | hard-coded 49/50 | CHANGED (rebindable) | `"v2k", alias, 1` / `"v2k", alias` | [GAMEPLAY] | missing (no vote-kick UI; `KN client/src/ui/hud_ui.c:495-501`) |
| specFree | Toggle Free Cam | 70 F | hard-coded 70 | CHANGED (rebindable) | `spectMode()` | [CLIENT] | present, hard-coded (`KN client/src/spectate.c:194-232`) |
| specObj | Toggle Objective Cam | 72 H | hard-coded 72 | CHANGED (rebindable) | objective spectate cams | [CLIENT] | missing (H is a no-op, `spectate.c:202`) |
| specFirst | Toggle First Person | 82 R | hard-coded 82 | CHANGED (rebindable) | 1st/3rd person | [CLIENT] | present, hard-coded (`spectate.c:216`) |
| specNames | Toggle Forced Names | 77 M | — | **NEW** | `showSpectatorNameTags` | [CLIENT] | missing |
| specMiniMap (`cont_specMiniMap`) | Toggle Spectator Minimap | 189 `-` | — | **NEW** | `setSetting("specMiniMap")`; that setting does not exist in v7 settings.js (broken in the reference) | [CLIENT] | missing |
| specFocus | Toggle Focus Player | 190 `.` | — | **NEW** | `focusPlayer` / `clearFocus` | [CLIENT] | missing |
| kpdVoteY / kpdVoteN | KPD Vote [Yes]/[No] | 89 Y / 78 N | hard-coded 89/78 | CHANGED | two-step verdict, then `"rkpd", 0/1` | [GAMEPLAY] | missing (no KPD) |
| kpdVision | Toggle KPD Vision | 187 `=` | — | **NEW** | mods/deputies: `kpdVision` setting | [CLIENT] | missing |
| prop | Open Prop Selection (Hold) | 80 P | — | **NEW** (Prop Hunt) | prop wheel; release sends `"prp", id` | [GAMEPLAY] | missing |
| propRand | Select Random Prop | 77 M | — | **NEW** | `"prp", randomId` | [GAMEPLAY] | missing |
| propRot / propRotR | Toggle / Reset Rotation | 82 R / 78 N | — | **NEW** | `"prpR"` / `"prpR", true` | [GAMEPLAY] | missing |
| taunt[0..5] | Taunt (Psst/Whistle/Fart/Meme/WTF is This?/Right Now!) | 49-54 (1-6) | — | **NEW** | `"prpt", i` (Prop team only); see 1.5.8 | [GAMEPLAY] | missing |
| reset (`resetKey`) | Reset Position (Parkour etc) | 66 B | — | **NEW** | input frame field 13 = 1 | [GAMEPLAY] | missing |
| resetLast (`resetLastKey`) | Reset To Last Checkpoint | 78 N | — | **NEW** | input frame field 13 = 2 | [GAMEPLAY] | missing |
| noclipSB / godModeSB / killSB (`sandbox*`) | Sandbox Noclip/Godmode/Kill | -1 | — | **NEW** (offline sandbox / bot FTUE) | local sandbox game | [CLIENT] (local authority) | missing |
| spawnBot / clearBots | Spawn / Clear Bots | 107 numpad `+` / 109 numpad `-` | — | **NEW** (sandbox) | `sandboxGame.bots.add({dmgMlt:0.01})` / `reset()` (`v7 src/index.js:16316-16327`) | [CLIENT] (local authority) | missing (KN has `bot_*` console commands instead) |
| — | Escape | 27 | 27 | SAME | `UI.toggleEndScreen` / unlock | [CLIENT] | present |
| — | Ctrl+`/` | — | — | **NEW** | clears the active mod (`setLastMod("")`) (`v7 src/libs/controls.js:170-172`) | [CLIENT] | n/a |
| — | Spectate number keys 1-9, 0 | 49-57, 48 hard-coded | same | SAME (still hard-coded) | pick player (`"fpSp", sid`) / objective cam | [CLIENT]/[GAMEPLAY] (`fpSp`) | present for players (`spectate.c:204-214`) |

Removed in v7: streak slots 4-5 (`cont_21`, `cont_22`), and the v3 LMB/RMB hard-wiring.

### 1.5.3 Input mechanics and input-related settings

| Mechanic | v3.1.1 | v7.1.4 | Tag | KN |
|---|---|---|---|---|
| Toggle ADS | none (aim is held) | Per-weapon `toggleADS` (viewmodel, `perWep`, default false) (`v7 src/libs/settings.js:3345-3357`). Aim press flips `CONTROLS.adsToggled`; release clears it unless the weapon has toggle on (`v7 src/index.js:16331-16337,16587-16589`). Switching to a non-toggle weapon drops it (`v7 src/data/players.js:4461-4465`). Input-frame aim bit = `adsToggled` | [CLIENT] (same wire bit) | missing |
| Hold vs toggle: leaderboard / voice | hold only | `scoreBoardTrigger`, `voiceChatTrigger`: 0 Press (Toggle), 1 Continuous (Hold, default) (`v7 src/libs/settings.js:4265-4294`) | [CLIENT] | missing |
| Per-weapon sensitivity | global | `gpSetts` 0 All / 1 Per Weapon. `sensitivityX/Y`, `aimSensitivityX/Y`, `scrollDir`, `invertY` are `perWepG`. `getSetByWep` reads `kbmInput[key][weaponId+1]` (`v7 src/libs/settings.js:2396-2500`, `v7 src/libs/controls.js:514-520`) | [CLIENT] | missing |
| Mouse input | `flickClamp` 200 + `mouseFlick` false, `aimFreezeFix` | Same, plus `rawMouse` (false, `toggleMouseInputs`) and `mouseAccel` (true). With `mouseAccel` off, pointer lock is requested with `{unadjustedMovement:false}` (`v7 src/libs/controls.js:7-22`, `v7 src/libs/settings.js:137-206`). `mouseFlick` defaults to `isMobile` | [CLIENT] | partial: flick clamp present (`KN client/src/ui/ui_mods.c:257`); KN has its own `rawInput` client setting |
| Input frame ("q") | 13 fields: `isn, dt, yaw, pitch, moveDir, shoot, aim, jump(press frame), crouch, reload, scroll, wSwap, speedLmt` (`v3 src/index.js:12821-12835`) | 15 client fields: `isn, dt, **pitch, yaw** (2/3 swapped vs v3), moveDir, shoot, adsToggled, jump (held), **reload, crouch** (8/9 swapped vs v3), scroll*per-weapon scroll dir, wSwap, speedLmt, **reset (0/1/2)**, **interact held**`, sampled at `netRate` (`v7 src/index.js:16830`). The server appends `[15]` input server time and `[16]` interpolation progress for lag compensation (`v7 src/data/player.js:944-957`). Full table in 1.1.4 | [GAMEPLAY] | v3 layout (`KN shared/include/shared.h:1186-1190,1277-1323`); no reset/interact fields |
| KrunkScript input hooks | none | `onKeyPress`, `onKeyUp`, `onKeyHeld`, `onMouseClick`, `onMouseUp`, `onMouseScroll`, `onControllerPress/Held/Up` (`v7 src/libs/controls.js:155-161,601,1137-1141`, `v7 src/index.js:16328,16566`) | [CLIENT] (custom-map scripts) | missing (no KrunkScript) |
| Idle timer | `controls.idleTimer` | same | [CLIENT] | present |

### 1.5.4 Gamepad

| Item | v3.1.1 (`src/libs/gamepad.js`, `controls.js:1163-1290`) | v7.1.4 (`src/libs/gamepadInput.js`, `controls.js:1014-1170`) | v3->v7 | Tag | KN (`client/src/gamepad.c`) |
|---|---|---|---|---|---|
| Button map | button_1 jump, button_2 crouch, button_3 reload, button_4 swap, L1 swap, R1 melee, L2 aim, R2 shoot (`THRES`, mouse), select player list, d-pad up voice / down spray / left interact / right drop, start = toggle controls | Same buttons. R2 is now `{THRES, KEY:"shoot"}` (goes through the shoot bind, not a fake mouse) (`v7 src/libs/controls.js:1074-1114`) | SAME (R2 path CHANGED) | [CLIENT] | present (v3), `gamepad.c:82-89` |
| Left stick -> move keys | 4 independent axis thresholds at `deadZoneL` (`v3 src/libs/controls.js:1226-1260`) | Radial deadzone rescale (`(v - dz) / (1 - dz)`, clamped to the unit circle), then **8 sectors of 45°** with one or two move keys each (`v7 src/libs/controls.js:1036-1069,1145-1200`) | CHANGED | [CLIENT] (sent as moveDir) | v3 model (`gamepad.c:337`) |
| Rumble | `vibration` on/off | `rumble(dur = weapon.rumbleDur or 200, mag = (weapon.rumble or 0.5) * vibrationMlt)`, dual-rumble weak = 0.5 x strong (`v7 src/libs/gamepadInput.js:234-244`, `v7 src/data/players.js:2257`). New `vibrationMlt` setting (default 1) | CHANGED | [CLIENT] | partial (on/off only) |
| Controller settings | sens X/Y 1/1, aim 1/1, deadZoneL 0.3, deadZoneR 0.25, triggerThres 0.1 | **sens 4/4, aim 3/3** (`v7 src/libs/settings.js:2529-2571`), same deadzones/threshold, **NEW `disableCons`** (false) = ignore controllers | CHANGED | [CLIENT] | v3 defaults (`gamepad.c:206`) |
| Script hooks | — | `onControllerPress/Held/Up(button, bindKey)` | NEW | [CLIENT] | missing |

### 1.5.5 Mobile / touch

| Item | v3.1.1 | v7.1.4 | Tag | KN (`client/src/ui/ui_mobile.c`) |
|---|---|---|---|---|
| Buttons | `#mobileUI`: `mobileJoystick`, `mobileJump`, `mobileCrouch`, `mobileEsc`, `mobileShoot`, `mobileAimShoot`, `mobileScope`, `mobileReload`, `mobileSwap` (`v3 public/index.html:620-629`) | Adds **`mobileKSwap`** (melee) at `{left:0, top:0.65, right:0}` (`v7 public/index.html:1127-1137`, `v7 src/config.js:3120-3175`) | [CLIENT] | partial: v3 set (`ui_mobile.c:58-72`), no `mobileKSwap` |
| Layout | fixed fractions `config.mobileControlPositions` | Same fractions, plus a per-button `scale` (`controls.scaleMap`), an override via `window.mobileControlPositions`, and responsive sizing with min/max px clamps per element (`v7 src/libs/mobileControls.js:7-49,73-160`). Joystick: nipplejs `size 120`, static | [CLIENT] | v3 |
| Loadout-aware buttons | all shown | Hides buttons based on the class loadout: `HAS_MELEE_ONLY` hides aim-shoot/scope/reload/melee/swap; `HAS_PRIMARY_OR_SECONDARY` hides swap (`v7 src/libs/mobileControls.js:2-5,164-216`). `mobileShoot` vs `mobileAimShoot` follow scope state (`updateScopeStatus`) | [CLIENT] | missing |
| Aim-shoot logic | aim, then shoot after `aimSpd+50` ms; auto-refire on `nAuto` every `rate` | Same logic, using the bind table (`v7 src/libs/controls.js:754-807`) | [CLIENT] | present (v3) |
| Mobile settings | sens 1/1/1/1 | **1.4/1.4/1.4/1.4**; `scaleUI` 0.9 on mobile; `oldScoreboard` true on mobile (`v7 src/libs/settings.js:747,791,2678-2723`) | [CLIENT] | v3 defaults |
| Streak tray | — | `.killStreakItem` tappable (`callInStreak`) | [GAMEPLAY] | missing |

### 1.5.6 HUD

KN's `ui/index.html` is the v3.1.1 HUD. None of the new v7 HUD ids exist in it, and every v3 id that v7 dropped is still there. The C HUD lives in `client/src/ui/hud_ui.c`, `hud_popups.c`, `hud_modes.c` and `client/src/nametags.c`. `client/src/ui/hud.c` only draws "CLICK TO PLAY", and `client/src/ui/overlay.c` is the no-HTML fallback.

| Element | v3.1.1 | v7.1.4 (DOM id / code ref) | Tag | KN |
|---|---|---|---|---|
| Layout | `#inGameUI` -> `#topRight`, `#botRightHider`, `#topLeftHolder`, `#bottomLeftHolder` (`v3 public/index.html:454-610`) | `#gameUI > #gameUIHider > #inGameUI > #midGameUIHider`, with `#centerLeaderDisplay`, `#topMidHold`, `#topLeftMain` (minimap + match data), `#topLeftData`, `#topLeftBottom` (`v7 public/index.html:850-1110`) | [CLIENT] | missing (v3 DOM) |
| Health | `#healthValue` "hp \| max", segmented `#healthBar` `.hpBSeg`, `hudHealthLow` at ≤3 segments | `#bottomLeftPlayer` card: `#bottomLeftClassImg`, `#bottomLeftTeamType`, `#bottomLeftTeamName`, `#bottomLeftHealthBar` (`.nHpBSeg`), `#bottomLeftHealth` = `ceil(hp)`, `#bottomLeftChallIcon`. Damage adds `RENDER.shake(0.02)` + `takedmg_0/1` at 0.45, at most once per 500 ms (`v7 public/index.html:1088-1104`, `v7 src/index.js:15887-15935`) | [CLIENT] | partial: v3 (`hud_ui.c:288,1617`) |
| Ammo | `#ammoDisplay` (`#ammoVal/#ammoMax/#ammoIcon`) | `#ammoHolder > .greyInner`, same children (`v7 public/index.html:962`) | [CLIENT] | present (v3 markup) `hud_ui.c:1631` |
| Weapon slots + weapon streak | `#weapDisplay .weapItem` | `#weapHolder .weapItem` + `.weapStreakHolder` per-weapon kill counter (`io_updateWeaponStreak`, `v7 src/index.js:15344`) | [GAMEPLAY] | partial (`hud_ui.c:1695`); weapon streak missing |
| Counters | kills, deaths, streak, score (`.countIcon`) | `.statIcon`; **NEW `#kdCount`/`#kdVal`** = kills/max(deaths,1), 1 decimal, setting `showKD` false (`v7 src/index.js:15773,15190`); `#scoreZoneVal` flag icon, lives heart icon | [CLIENT] | partial (`hud_ui.c:2448-2450`); K/D missing |
| Kill feed | `#killFeed .kfItem`, life 4000 ms | Same; headshot/wallbang icons also on melee kills; streak icon may be `img/streaks/package/<pkg>.png` (`v7 src/index.js:16150-16170`) | [CLIENT] | present (`hud_ui.c:1452,1480`) |
| Call-in streak tray | `#killStreakHolder` | `.killStreakItem`: key name, `x<cnt>`, `img/streaks/<i>.png` (`v7 src/index.js:15333-15340`); packet `"st", slot, cnt` | [GAMEPLAY] | missing (empty DOM) |
| Perks / power-ups / tags / big-head | — | `#perkHolder` (`io_effect`), `#zPerksHolder`, `#powerUpHolder` (3 timers), `#krTagHolder`/`#giftTagHolder` (red at `mode.tagHoldLimit`), `#bhHolder` (`headMlt`) (`v7 src/index.js:13783,15353-15395,15869`) | [GAMEPLAY] | missing |
| Rounds | — | `#roundsDisplay` (`#roundTSc1/2`, `#roundsVal`, `#roundSub`) when `gamRounds > 1`; `#roundMessage` (`v7 src/index.js:13207,15624`) | [GAMEPLAY] | missing |
| Timer / match info | `#timerDisplay`, `#curGameInfo` | `#timerHolder > #timerInner > #timerVal`, `#matchInfo` next to the minimap | [CLIENT] | partial (`hud_ui.c:1654,1726`) |
| Ping / FPS / net | `#pingText` + bar icon; `showNetStat` | `#pingText` + "PING"; **NEW `#tickDisplay > #tickPacketCount`** "N TPS" (`v7 src/index.js:2746`); `fpsRate` 100; `showNetStat` removed | [CLIENT] | partial (`hud_ui.c:767,1685`) |
| Top-right leaderboard | `#leaderDisplay .leaderItem`, ranked elo icon | `#leaderboardHolder`; `Game.badges[badgeIndex]` image, no elo icon. `"7"` = `[sid, ...mode.leaderStats]` (default `["score"]`), max 16 (`v7 src/data/players.js:3946-3965`, `v7 src/index.js:15409-15441`) | [GAMEPLAY] (stride) | present (v3) `hud_ui.c:467` |
| Tab scoreboard | `#newLeaderDisplay` only with `leaderType==1`; name + score | `#centerLeaderDisplay`: per-team `.newLeaderTeam` header with team score + `<table id='ingameTable'>`, one column per `leaderStats` (`v7 src/index.js:15442-15496`); toggle/hold via `scoreBoardTrigger` | [CLIENT] | partial (`hud_ui.c:1740,1763`) |
| In-game XP bar | `#igXPBar > #igXPBarProg`, `xpBarCol`, `xpBarOpac` | **REMOVED** (XP only on the end screen) | [CLIENT] | present (v3 only) `hud_ui.c:1811`. Adopt v7: remove it |
| Medals | text banner on `#chalName`; `"am"` = `[name, score]` pairs; `medalAnim` 1000, `medalDelay` 900 (`v3 src/index.js:11762-11806`) | Icon medals: 56 names (`v7 src/libs/game-overlay.js:136`), `img/medals/medal_<i>.png`, sound `medal_<snd>` at 0.08, repeat count. **`"am"` = quads `[index, score, sound, count]`** (assist = `[31,25,0,1]`); `medalAnim` **1800**; hidden in comp; `medals` settings category (`showMedals`, `classicMedals` false, `playMedals` true, `medalScale` 1, `medalOffX` 5, `medalOffY` 1, `medalSat` 1, `medalHue` 0) (`v7 src/index.js:15778-15825`, `v7 src/libs/settings.js:1440-1546`) | [GAMEPLAY] (`am` shape) | partial: v3 banner (`KN client/src/ui/hud_popups.c:290`; `NetMedalPacket` `{text, score}` at `KN shared/include/shared.h:1765`) |
| Score popup / checkpoint | `overlay.scoreText`, `checkText` | same; size `scoreScale*100` | [CLIENT] | present (`hud_popups.c:254,263`) |
| Damage numbers | life 1000 ms, rise 0.022/ms, drift 0.013/ms, crit colour | Same motion; **heal numbers** (negative, `healColor`); **`dmgType`** 0 int / 1 float (default 1); hit sound `crit_0` on crit, else `hit_0` (`v7 src/libs/settings.js:946`, `v7 src/index.js:16176-16189`) | [CLIENT] | partial: integer only (`hud_ui.c:1252`) |
| Hit direction / hitmarker | `overlay.hits` (`hitLife` 2000, `#FF3F3F`), `#hitmarker` | same, same settings | [CLIENT] | present (`hud_ui.c:1060,1199`) |
| Death markers | — | **NEW** `overlay.addDeathMarker` for self, teammates, or anyone while spectating (`v7 src/index.js:15180-15182`); `img/death_0_1.png` (self) / `death_2_1.png`; size `max(0.3, 1-d/600)*70`, hidden within 10 units, `deathMarkerAdsOpac` near the crosshair while aiming (`v7 src/libs/game-overlay.js:679-697,2177-2203`). Settings `deathMarkers` 0 All/1 Self/2 Team/3 None, `deathMarkerOpac` 1, `deathMarkerAdsOpac` 1, `deathMarkerDur` 7 (`v7 src/libs/settings.js:874-945`). Note: life 7 is decremented by delta in ms, so it lasts about one frame as reconstructed; a port should use 7 s | [CLIENT] | missing |
| Minimap | debug-only `drawMinimap=false` | **NEW** `src/libs/Minimap.js` on `#miniMapCanvas` (`#topLeftMap > #miniMapOverlay`): map objects (unless `noMinimap`), flags, bomb sites, dom zones by owner, teammate blips with direction; enemies show for `playerBlipTimer` 2000 ms after firing (`v7 src/index.js:15506-15513,15532,15584`); spectator full-map mode with cardinal directions; off when `mode.noMinimap` or map `minimaps` (`v7 src/libs/Minimap.js:300-357`). Settings `showMinimap` true, `minimapScale`, `minimapZoom`, `miniMapHudScale` (1, range 0.5-1.5) | [CLIENT] | missing |
| Nametags | `nametagStyle` 0-3, `hideNames`, `nametagOpac`, `healthNum`, `dynamicHP`, level/clan/badge icons, voice icon (`v3 src/libs/game-overlay.js:640-760`) | Adds `nametagScale` (1), `Game.badges[badgeIndex]` 23 px image, "busy" hourglass (alternates every 800 ms), `mode.noHPbars`, KPD-vision colour `kpdVisionCol` `#00ffff`, height offset for hats/`headMlt`, level hidden in comp (`v7 src/libs/game-overlay.js:946-1100`) | [CLIENT] | partial (`KN client/src/nametags.c:381,392,993`) |
| Objective markers | `textures/objective_1.png` (active only) | `textures/marker_0.png` (current) + `marker_0_next.png` (next), swappable at runtime; dom-zone markers; `objOpac` 1, `adsObjOpac` 0.5 (`v7 src/libs/game-overlay.js:30-44,1251,2119-2140`) | [GAMEPLAY] (next-objective state) | partial (`nametags.c:46,107-109`) |
| Flag / juggernaut | `flag_r`, `flag_b` | + `flag_w` (one-flag); off-screen `jugg_1.png`/`jugg_2.png` (`v7 src/libs/game-overlay.js:113-124,1102-1110`) | [GAMEPLAY] | partial |
| Strafe meter | — | `overlay.drawSRM` from `player.overlayInfo` (`v7 src/libs/game-overlay.js:423`, `v7 src/data/movement.js:461`) | [CLIENT] | missing |
| Speedometer / speed lines / blood | `showSpeed` (5, 5.4, 2.4, `#ffea78`/`#ff0000`), `showDMGOverlay` | same defaults; `shoSpdLines` (`speedLineMlt` 500); `useDamageOverlay` rename | [CLIENT] | present (`hud_ui.c:1353`) |
| Spray / prop wheel | — | radial wheel drawn on `#game-overlay`, opens after 200 ms hold (`v7 src/libs/game-overlay.js:46-111,1941-2060`) | [CLIENT] | missing |
| Voice indicator | `#voiceDisplay` (opacity 0.5/0.9) + `#recTimer` `[V]` (`v3 public/index.html:399`) | `#voiceIcon.material-icons` with `voiceOn`/`voiceOff` classes (`v7 public/index.html:786`); speaker icon over nametags alternates `volume_down/up` every 300 ms (`v7 src/libs/game-overlay.js:703,814,1062`) | [CLIENT] | present (v3) `KN client/src/voice.c` |
| Mode panels | `#propInstruct` | `#propControlHolder` (prop + taunt key labels), `#speedRunHolder` (`#speedRunRstKey`, `#speedRunRstLKey`, `#speedRunTimer`, `#speedRunList`) | [GAMEPLAY] | missing |
| Vote UI | `#voteToKick` | + `#voteHeader`, bind-driven key labels, `#voteToForfeit` (`"vf"`) | [GAMEPLAY] | missing |
| End screen | Leaderboard / Performance | `GUI.endScreen` tabs Leaderboard / Performance (mode icon, funds, battle-pass `bpp % mx`, 1000 ms XP animation) / **Medals** / **Rewards** (`v7 src/ui/game.js:36-200`) | [CLIENT] (BP data [GAMEPLAY]) | partial (`hud_ui.c:2084,2119,2207`) |
| Removed in v7 | `#hitHolder`, `#scoreText`, `#checkTextHolder`, `#texts3DHolder`, `#interactProg`, `#terminalHolder`, `#queueStatus`, `#curGameInfo`, `#teamName`, `#hudClassIcon`, `#killCardHolder`, `#killReactHolder`, `#igXPBar` | — | [CLIENT] | KN still has them. Adopt v7: remove them |

### 1.5.7 Death screen, kill camera and spectate

| Feature | v3.1.1 | v7.1.4 | Tag | KN |
|---|---|---|---|---|
| Kill camera | Always on: `CONTROLS.camLookAt(killer.x, killer.y + killer.height/1.5, killer.z)`, killer forced visible (`v3 src/index.js:11062`) | Same, but only when **`Game.config.kCams`** is set (custom-game option "Kill Cams", default true) (`v7 src/index.js:15197`, `v7 src/config.js:789`) | [GAMEPLAY] (game config) | present, always on (`KN client/src/menu_view.c:51`, from `client/src/net/game_events.c:122`); `kCams` missing |
| Kill-cam motion | chase: turn `camChaseTrn` 0.0022/ms, glide `camChaseSpd` 0.0012/ms, stop `camChaseDst` 24 short; `camChaseSen` 0.2 (`v3 src/config.js:1172-1175`) | identical (`v7 src/libs/controls.js:971-996`, `v7 src/config.js:2081-2084`). **No replay.** | [CLIENT] | present (`menu_view.c`) |
| Death delay | `deathDelay` 2800 ms, then the menu (`v3 src/index.js:11213`) | 2800 ms (`v7 src/config.js:2255`). The call is written as `requestAnimationFrame(fn, deathDelay)`, which looks like a deobfuscation slip for `setTimeout` (`v7 src/index.js:15214-15226`) | [GAMEPLAY] | present (`KN shared/include/shared.h:914`, `shared/src/game.c:219`) |
| Auto-respawn countdown | "Auto Respawning in N", ticks every 1000 ms (`v3 src/index.js:11196-11211`) | same, after the delay; config `forceSpawn` 0-15 | [CLIENT] | server respawn present; subtitle missing |
| Kill packet `"3"` | `[sid, deaths, killerSid, [weaponId, score, classStat, [kcStat, val]], hs, fists]` (`v3 src/data/players.js:5733-5753`) | `[sid, deaths, killerSid, [weaponId, classStat, [pcStatIdx, val] or null, **[timeAlive ms, shots, "acc%", kills, score]**], ...]`; non-victims get `[]` (`v7 src/data/players.js:4860-4862`) | [GAMEPLAY] | v3 shape (`game_events.c:100-138`) |
| Death screen | `#killCardHolder` (`#killCard`, `#killCardBy`, `#killCardMsg`, `#killCardKick` "[pList] to Report" for killers ≤ lvl 10); `#killReactHolder` | **`#deathUIHolder`** (`v7 public/index.html:1108`, built by `f1975`, `v7 src/index.js:15240-15296`): VS layout with both player cards, class + level, weapon icon `img/death-ui/<icon>.png` (fists `icon_0`, melee `icon_0_1`, "Thrown"), skin name in rarity colour, head-to-head counts (`player.killedBy[killer.id]++`, `:15177`), "[key] to Report", `.death-stats` Time Survived / Shots Fired / Accuracy / Kills / Score; "You Died" when there is no killer; `uiBase.onDeathScrn`; closes the spray/prop wheels | [CLIENT] (data [GAMEPLAY]) | v3 card present (`KN client/src/ui/hud_ui.c:1827-1859`); report prompt hidden (`:1850`); v7 screen missing |
| Death markers | — | see 1.5.6 | [CLIENT] | missing |
| Spectate entry | `toggleSpect` / `#spectButton` | + `#specBtn`, competitive `#cmpSpectBtn`/`#cmpSpectTgl`; game config `allowSpect`, "Designated Spectators" (`v7 src/index.js:2092-2103`, `v7 src/config.js:761,800`) | [GAMEPLAY] | v3 present (`KN client/src/spectate.c:51,104`); comp/designated missing |
| Next/prev target | LMB `spectMode(-1)`, RMB `spectMode(1)` | same; skips bots when `mode.botsAsAI` (`v7 src/libs/controls.js:609-615`, `v7 src/index.js:2122-2165`) | [CLIENT] | present (`spectate.c:145,224`); bot filter missing |
| Free cam | F (70) hard-coded; wheel = speed 1..10, or follow distance ±3 within `specMinD` 10..`specMaxD` 160; `mode.noFreeCam` | rebindable `specFree` 70; same wheel logic (`v7 src/libs/controls.js:660-680`) | [CLIENT] | present, hard-coded (`spectate.c:197,232`) |
| First/third person | R (82); `#specIns` text | rebindable `specFirst` 82 (`f1764`, `v7 src/index.js:2104-2116`); POV eases crouch at 0.0085/ms, aim at 0.009/ms | [CLIENT] | present (`spectate.c:216,281`) |
| Objective cams | H (72) / 0 (48) / 1-9 | `specObj` 72 rebindable; 48/49-57 still hard-coded (`v7 src/index.js:16366-16400`) | [CLIENT] | missing (also missing vs v3) |
| `"fpSp"` | `fpSp(sid)` | same; server keeps `spectateTargetSid` (`v7 server/index.ts:3389`) | [GAMEPLAY] | present (`spectate.c:67`, `KN server/src/net.c:1035`) |
| Focus player | — | `specFocus` 190: `focusPlayer` / `clearFocus` with chat "Focusing - name" / "Focus Cleared"; re-follows the focused player on respawn; auto-focuses the KPD suspect; also from window 23 (`v7 src/index.js:11136-11172,16424-16430,14689`) | [CLIENT] | missing |
| Forced nametags | always shown while spectating | `specNames` 77 toggles `showSpectatorNameTags` (default true) (`v7 src/index.js:2091,16362,14819`) | [CLIENT] | missing (always on, `nametags.c:596-611`) |
| Spectator HUD | `#spectateUI`, `#spectateHUD`, `#spec0/#spec1`, `#specTeam0/1`, `#specTimer`, `#specGameInfo`, `#specGMessage`, `#specNames`, `#specStats`, `#specSUS`, `#specContr` (`v3 public/index.html:408-439`) | + `#specHeaderTm0/1`, `#specStatsInner` (class icon, flag, level, featured badge, `spectHPV`/`spectHPBI` HP bar, low colour ≤30%); `#specContr` -> `#specControlHolder` with `.specControl` rows labelled from binds (`v7 public/index.html:796-844`, `v7 src/index.js:2167-2206`) | [CLIENT] | v3 present (`KN client/src/ui/hud_spectate.c`); v7 holder missing |
| KPD review | `#specKPDContr`, N/Y -> `"rkpd"` 0/1 (`v3 src/index.js:12209,12335`) | two-step verdict ("Is X hacking?" -> "Are you 100% sure?") -> `"rkpd"` 0/1, reset by `"kpdf"`, copies the profile URL; `kpdVision` + `kpdVisionCol` (`v7 src/index.js:16404-16422,16593-16625`) | [GAMEPLAY] | missing (`hud_spectate.c:258,377` hide it) |
| Mute spectators | — | `muteSpec` (false) drops voice when you or the speaker is not active (`v7 src/index.js:2222`) | [CLIENT] | missing |

### 1.5.8 Sprays, taunts ("emotes"), chat, voice

| Feature | v3.1.1 | v7.1.4 | Tag | KN |
|---|---|---|---|---|
| Spray catalogue | `src/data/sprays.js`: **53** base sprays (`{name, id, opacity, frames?, frameT?}`) + 48 store skins of `type: 4` (`v3 src/data/store.js`) | No `sprays.js`. Sprays are store skins with `type == itemTypes.SPRAY (4)` from the runtime `skins.jspck` (`v7 src/data/skins.js`, `v7 src/data/itemTypes.js:6`); **default spray id 2482**; 472 files in `public/textures/sprays/`. Skins can carry `spraySnd`, `sclW/sclH`, `rgb*`, `glow`, `pulsT`, `rAnim`, `video`, `projection`, and a spray dye (`v7 src/libs/render.js:1844-1889`) | [GAMEPLAY] (ids on the wire) | v3 table (`KN client/src/sprays.c:25`, `SPRAYS_BASE_COUNT 53`) |
| Spray rules | `players.spray`: not in comp, `sprayTimer` 1000 ms, `sprayRange` 25, `sprayScale` 15 (or `sizeReq`), view ray vs map boxes, decal pushed inside the face (`v3 src/data/players.js:3436-3566`, `v3 src/config.js:1347-1349`) | Same ray, timer, range and scale (`v7 src/config.js:2252-2254`), with scale `sprayScale*sclW/sclH`. **Several live sprays per player**: `maxSprays` 1, `maxSpraysP` 2 (premium), `maxSpraysF` 3 (featured/sandbox), `unlimitedSprays` -> 10000; ids `sid_<n>` cycle (`v7 src/data/players.js:2011-2022`, `v7 src/config.js:2654-2656`). The chosen index is honoured only for premium/sandbox owners (`canUseSkin`); otherwise the equipped `sprayIndex`, falling back to 2482 (`v7 src/data/players.js:2032-2072`). Stat `spry` + challenge `spry` | [GAMEPLAY] | v3 rules present (`KN shared/src/interact.c:120-160`); one decal per player (`sprays.c:113`) |
| Spray packets | C->S `"int", 0`; S->C `"sp", sid, sprayIndex, x, y, z, rotY, rotX` (`v3 src/data/players.js:3556`) | C->S **`"spry"[, wheelIndex]`** (`v7 server/index.ts:3290-3294`); S->C **`"sp", "<sid>_<n>", sprayDefIndex, x, y, z, rotY, rotX, rotZ, skinId`**; sound `spray` or `spray_<spraySnd>` at 0.4 (`v7 src/data/players.js:2007-2025`, `v7 src/data/game.js:3343-3349`) | [GAMEPLAY] | v3 (`NET_PACKET_SPRAY` `KN shared/include/shared.h:1131,1584`) |
| Spray wheel | — | Premium/sandbox. Hold `sprayWheelKey` (F) for 200 ms; mouse wheel cycles up to **15** owned sprays (favourites first) + the equipped one (`sprayWheelMax` 15); release sends `"spry", index` (`v7 src/libs/game-overlay.js:46-77`, `v7 src/index.js:14501-14512,16573-16580`) | [GAMEPLAY] | missing |
| Taunts (the only "emotes") | — (taunt sounds exist but have no bind) | Prop Hunt, Prop team only: `"prpt", i` (and `"stlkrt"` for the stalker mode). `config.taunts`: 0 `taunt_0` 500 ms, 1 `taunt_1` 1000, 2 `taunt_2` 2300, 3 `taunt_3` 3200, 4 `taunt_4` (sound 28) 3200, 5 `taunt_5` (sound 104) 1000; cooldown = `lastTaunt + tm`; played as a network sound at 0.3, pitch 0.9-1.0 (`v7 src/config.js:2258-2281`, `v7 src/data/players.js:4552-4557`, `v7 server/index.ts:3284`) | [GAMEPLAY] | missing |
| Emote wheel / gestures | — | **none in v7.1.4** | — | n/a |
| Team ping / map markers | — | **none in v7.1.4** (closest: the minimap enemy blip for 2000 ms after firing, and death markers) | — | n/a |
| Text chat send | `"ct", msg[, captchaKey]` (captcha on the first message) (`v3 src/index.js:10130-10145`) | **`"ct", channel, msg`**: channel 0 = global, 1 = team (`v7 src/index.js:13986-13988`) | [GAMEPLAY] | v3 (`NET_PACKET_CHAT_SEND` `KN shared/include/shared.h:1078`) |
| Team / global channel | — | **`#chatSwitch`** (`data-tab` public/groups) in `#chatSwitchHolder` toggles text **and** voice channel; forced to global in non-team modes, to team in comp; system line "Team/Global Text & Voice Chat" (`v7 public/index.html:778-779`, `v7 src/index.js:15955-15974,13766-13771`). Each chat row is tagged `data-tab` and filtered | [GAMEPLAY] | missing |
| Chat receive | `"ch", sender, msg, type, color, senderColor` -> `addChat` (`v3 src/index.js:11978`) | **`"ch", channel, sender, msg, type, color, senderColor, accountId`** -> `io_addChat`; channel forced to -1 outside team modes; dropped if `accountId` is muted (`v7 src/index.js:16065-16098`) | [GAMEPLAY] | v3 (`NetChatPacket` `shared.h:1331-1336`) |
| Per-player mute | — | Player list (window 23) toggles chat mute (`mute_chat`) and voice mute (`mute_voice`) by account id, saved in localStorage (`v7 src/index.js:803-810,11085-11107`). The reference saves the voice list under `mute_chat` (bug) | [CLIENT] | missing |
| Quick chat | — | 6 bindable messages, see 1.5.2 | [GAMEPLAY] | missing |
| Chat limits | `chatMaxLength` 70, `chatListLength` 35 | same (`v7 src/config.js:2071,2074`) | [GAMEPLAY] | present |
| Chat settings | `shwChat`, `chatFilter`, `showMessages`, `showUnboxings`, `chatOp`, `chatBGOp`, `chatTextOutline`, `chatHeight`, `chatTimer` | same 9 keys and defaults (`v7 src/libs/settings.js:1546-1655`) | [CLIENT] | present |
| Voice capture | push-to-talk on `voiceKey`; MediaRecorder `bitsPerSecond` 6000, restarted every `voiceRate` 500 ms; each chunk becomes a data URL; `#voiceDisplay` (`v3 src/index.js:1661-1720`) | Hold **or toggle** (`voiceChatTrigger`); bitrate = **`micQuality`** setting (default 6000); `audioInput` device; `#voiceIcon` on/off (`v7 src/index.js:2292-2358`, `v7 src/libs/settings.js:2822-2866`) | [CLIENT] | v3 present (`KN client/src/voice.c:1355`) |
| Voice packets | C->S `"vc", dataUrl`; S->C `"vc", dataUrl, alias, sid`; server drops it if there is no player or it is over 256 KiB, then relays to everyone (`KN shared/include/voice.h:3-17`) | C->S **`"vc", dataUrl, channel, micVolume, micQuality`** (`v7 src/index.js:2288-2291`); S->C **`"vc", dataUrl, name, sid, channel, volume, isFinal, accountId`** (`v7 server/index.ts:4241-4263`: must start with `data:audio/`, ≤ 750 000 chars, volume clamped 0-1). The receiver plays it only if `channel == -1` or matches its own channel, and scales by `micVolume` (`v7 src/index.js:2275-2287`) | [GAMEPLAY] | v3 (`KN shared/src/voice.c`, `server/src/net.c:868-877`) |
| Voice playback | Howl queue per speaker; clips over `voiceChatMaxLength` 0.65 s x 1.2 are dropped (`v3 src/index.js:1610-1648`) | Same queue and limits (`v7 src/config.js:2075-2076`); **spatial/proximity** mode `voiceChatType` 1 places the Howl at the speaker's head with `refDistance = voiceDistance*5` (slider 1-10, default 5 -> 25), rolloff 1; `muteSpec`; mute list; `dialogueVolume`, `micVolume` settings (`v7 src/index.js:2221-2273`, `v7 src/libs/settings.js:2807-2914,2990`) | [CLIENT] | v3 present (`KN client/src/voice.c:989`); proximity, channels and mute missing |

### 1.5.9 Gap summary for Krunker Box Office

Biggest gaps, in rough priority order. Each one is adopted v7 behaviour; KN keeps no v3 fallback.

1. **[GAMEPLAY] wire shapes**: input frame (v7 swaps yaw/pitch and reload/crouch, adds reset + interact-held; see 1.1.4), `"am"` medal quads, the `"3"` kill packet with the death-stats array, `"ct"`/`"ch"` with a channel and account id, `"vc"` with channel/volume/quality, `"sp"` multi-spray ids with skin id, and the action packets `spry` / `intSec` / `drpw` / `cnfm` / `k0..k2` / `prp*` / `prpt`. These belong with the protocol section, but every v7 control in 1.5.2 depends on them.
2. **Bind table**: KN has the 38 v3 ids (`client/src/ui/ui_controls.c`). v7 needs alt keys, rebindable LMB/RMB, about 30 new actions (spray wheel, confirm, quick chat, hide players, kick/KPD/spectator keys, prop hunt, parkour reset, sandbox), 3 streak slots, and the 5-tab window.
3. **HUD rebuild**: the v7 DOM (bottom-left player card, `#centerLeaderDisplay` table, K/D, TPS, rounds, perks/power-ups, weapon streak) plus the canvas features (minimap, death markers, icon medals, spray/prop wheels). KN `ui/index.html` is v3 and would need a v7 HUD import.
4. **Death UI**: `#deathUIHolder` VS screen + death stats + `kCams` toggle. The kill camera itself already matches (no replay in either version).
5. **Spectate**: rebindable keys, focus player, forced nametags, the `#specControlHolder` UI and bot filtering. Objective cams are missing even against v3.
6. **Social**: team/global chat and voice channels, proximity voice, per-player mute, prop-hunt taunts. Emotes and team pings do not exist in v7.1.4, so there is nothing to port for them.
7. **Input options**: per-weapon toggle ADS and sensitivity, press/hold triggers, the gamepad 8-way stick and rumble multiplier, new controller/mobile sensitivity defaults, and `mobileKSwap` with the loadout-aware mobile layout.

## 1.6 Status effects, perks and pickups

These are new v7 gameplay systems. The other sections touch them only in passing:
`effects.js` is not about skin effects, and `medals.js` is not about kill medals.
All of it is **[GAMEPLAY]** and server-authoritative. Most of it only shows up in modes,
custom games or map triggers, so a default public FFA match on a stock map never uses it.

### 1.6.1 Status effects (`v7 src/data/effects.js`, NEW)

There are 10 stackable effects. `player.effects[i]` holds the stack count, and
`execute()` writes a multiplier into `player.attributes[key]`. Grants go through
`game.players.giveEffect(player, index, count)` (`v7 src/data/players.js:4628`).

| idx | Name | key | stacks (`max`) | per stack | timer | Read at |
|---|---|---|---|---|---|---|
| 0 | Boosted | `speed` | 5 | +10% move speed | 0 | `v7 src/data/player.js:1234` |
| 1 | Sleight of Hand | `reload` | 5 | -10% reload time | 0 | `player.js:1847`, `players.js:373` |
| 2 | Rapid Fire | `fRate` | 5 | -5% fire interval | 0 | `players.js:2239,2707,2739` |
| 3 | Siphon | `siphon` | 5 | killer heals 10 HP per stack on a player or AI kill | 0 | `players.js:4873-4874`, `AI.js:1132-1133` |
| 4 | Rooted | `rooted` | 2 | -25% move speed | 25 | `player.js:1235` |
| 5 | Dizzy | `drunk` | 4 | +0.25 camera sway | 35 | `players.js:527-529` (client sway) |
| 6 | Poisoned | `enraged` | 1 | 10 damage every 4 s (every 8th 500 ms tick), can kill | 40 | `players.js:4181` |
| 7 | Jump Boost | `jump` | 5 | +10% jump velocity and push | 0 | `players.js:1699,1704` (1.1 J1) |
| 8 | Zapped | `zap` | 10 | 1 damage per stack every 1 s (every 2nd 500 ms tick), credited to `zappedBy` | 5 | `players.js:4181-4192`, `AI.js:425-427` |
| 9 | Strength | `dmg` | 10 | +10% damage | 0 | `game.js:893,927`, `projectiles.js:236` |

Effects are granted by map triggers (`v7 src/data/triggers.js:1482,1514`), airdrop
packages (`airDropPackages.js:232,245-246`), custom-game perk config `cfg.prks`
(`game.js:485-497`) and AIs (`AI.js:983-1026`). v3 has none of this. KN: **missing**.
There is no effects table and no `attributes` on `Player` (`shared/include/shared.h`).

### 1.6.2 Perks (NEW)

`player.perks` is a list of perk ids. They are bought at `PERK_MACHINE` map objects
(`v7 src/data/prefabs.js:2485-2501`; interaction at `player.js:1352-1385`, purchase at
`players.js:3420-3428`, at most `config.maxZombiePerks = 4`, `v7 src/config.js:945`)
or from the Perk Bottle pickup (`medals.js:539-566`). Their effects, as read in code:

| perk | Effect | Ref |
|---|---|---|
| 1 | fire interval x0.66 | `v7 src/data/players.js:2239` |
| 2 | reload time x0.5 | `v7 src/data/player.js:1847` |
| 3 | move speed x1.15 | `player.js:1237` |
| 4, 5 | may slide in `noSliding` modes | `players.js:1632` |
| 5 | while sliding, an AI-only explosion every 750 ms (radius 50, damage 300) | `player.js:1189-1196` |
| 6 | weapon limit 2 -> 3 | `player.js:291`, `players.js:3638` |

KN: **missing**. Perks belong with the zombie/wave modes and the krumMachine/upgrade
weapons (1.2.2e).

### 1.6.3 Pickups (`v7 src/data/medals.js`, CHANGED)

v3 `src/data/spawnables.js` has a single entry, "Tag" (Kill Confirmed). v7 `medals.js`
has **25 pickups**, each with a model, scale, timer and `onPickup`: Tag, Gift, Golden Egg,
KR Tag, Diamond Egg, Slime Pad, Ammo Pack, Weapon Box, Melee Pickup, Ammo Cache, Nuke,
Minigun, Insta-kill, Discount, Double Points, Grenade Launcher, Joe Tag, Frog,
Krunkanium, Perk Bottle, Zombie Portal, Skull EE, Free Upgrade, Mega Slime Pad and
Loot Drop (`v7 src/data/medals.js:18-730`). KN: Tag is present through the modes code
(`shared/src/modes/mode_objectives.c`, `shared/src/interact.c`); the other 24 are
**missing**. Mode-specific pickups belong to the modes survey; the player-relevant ones
(Ammo Pack, Weapon Box, Melee Pickup, Ammo Cache, Slime Pad) go with WP-B.

### 1.6.4 Kranked / Krank mode bonuses

`player.isKranked` with `game.mode.bonuses` scales reload, fire rate and speed
(`player.js:402-403,1847`; `players.js:2239`). This is mode-only (`src/modes/krank.js`)
and is tracked in the modes survey.

## 1.7 Settings (settings.js)

Scope: every entry of `this.list` in v7 `src/libs/settings.js` (287 keys, lines 18-4295), plus the 7 v3.1.1 keys with no v7 entry. The list was pulled out mechanically: a script split `this.list` at its top-level `key: {` entries and read `name`/`cat`/`val`/`min`/`max`/`step`/`type`/`options`/`set`. v3 `html:` helpers were mapped to their types. Labels come from `src/i18n/en.js` (v7) and the v3 `en.js`. The v3/v7 diff was also done by script. Defaults, ranges and `set` hooks for the tricky rows (paired toggles, mobile-only defaults, colour constants, per-weapon keys) were then checked by hand against v7 `src/index.js` and `src/libs/game-overlay.js`.

How to read the table:
- **Grouping**: rows are grouped under v7's *advanced* settings tabs (v7 `src/index.js:4036-4058`): General = local/network/serverb/experimental/kpd/dev; Controls = gameplay/controller/mobile; Display = interface/minimap/medals/crosshair/crosshairm/hitmarker/chat; Render = quality/post/editing; Game = gameprivacy/viewmodel/mods; Sound = audio. The `controls` cat (2 hidden keys) is listed under Controls. Keybinds (`binds`) come from `libs/controls.js`, not settings.js, so they are not covered here.
- **Type**: `checkbox (paired, hidden)` is a `hide: true` toggle that v7 renders as the switch in front of another row's slider/color/select (for example `mouseFlick` + `flickClamp`). `checkbox+slider` etc. is the row that owns it.
- **Default**: `X (mobile Y)` means v7 uses `isMobile ? Y : X`. `fov` is `config.fov + 20` with `config.fov = 80`.
- **v3 -> v7**: NEW, SAME, CHANGED (the changes are listed) or REMOVED. "mobile default" means only the `isMobile` default changed and the desktop default is the same as v3. "per-weapon" means v7 marks the key `perWep`/`perWepG`. v7 `init()` (settings.js:4296) then clones it into hidden `key_<weaponIndex>` entries, edited through `gpSetts`/`vmSetts` = "Per Weapon". KN has no per-weapon storage.
- **Scope**: [GAMEPLAY] means the value leaves the client: it goes into the `en` (enter) packet, the `vc` voice packet or the `prmA` dev packet, or it changes the input stream the server simulates (`netRate`). Everything else is [CLIENT].
- **Krunker Box Office**: *present* = the key is in KN's settings UI/store (`ui/windows/settings-tab-*.html`, `client/src/ui/ui_settings.c`) and the engine uses it. The owning consumer is named; most render/HUD keys go through the `RS_TABLE` in `client/src/ui/ui_render_settings.c`. *present (needs v7 change)* = it works, but KN still has the v3 default/range/step/options or only a global (not per-weapon) value. *partial* = KN stores and shows the key but nothing reads it (a no-op), or a KN-specific setting covers it. *missing* = no key and no behaviour. *n/a* = staff/dev only. "(needs: X)" marks a missing setting that only makes sense once feature X exists.
- KN's settings UI is the v3.1.1 page. Its 193 `setSetting("...")` keys match the 193 v3 settings.js keys exactly, so no v7-only key exists in KN under its v7 name. KN also has its own Client tab (`client/src/ui/ui_client_settings.c`: `rawInput`, `vsync`, `windowMode`, `discordRPC`, `audioBackend`, ...), which is not part of settings.js.
- v7 also adds a *basic/advanced* settings view: 85 keys carry `sType: "advanced"` and are hidden in basic mode, which has fewer tabs (v7 `src/index.js:4020-4035`). KN only has the v3 single (advanced-style) view.

### General tab

#### `local`

| Key | Label (en) | Type | Default | Range / options | Effect | v3 -> v7 | Scope | Krunker Box Office | Ref |
|---|---|---|---|---|---|---|---|---|---|
| `defaultRegion` | Default Region | select | regionCache | region list | region used by matchmaking/server list | SAME | [CLIENT] | present: servers_ui.c, ui_client_settings.c | v7 src/libs/settings.js:19; v3 src/libs/settings.js:35 |
| `lang` | Language | select | browser locale | locale list | UI locale (i18n tables) | SAME | [CLIENT] | done (v10 rule: `en` unless `krk_lang`; 17 tables, live switch: ui/i18n/README.md) | v7 src/libs/settings.js:37; v3 src/libs/settings.js:55 |

#### `network`

| Key | Label (en) | Type | Default | Range / options | Effect | v3 -> v7 | Scope | Krunker Box Office | Ref |
|---|---|---|---|---|---|---|---|---|---|
| `netRate` | Network Rate (Hz) | select | 3 (mobile 1) | 0=30, 1=60, 2=120, 3=144, 4=240, 5=360 | client input sample/send rate (Hz) in the game loop | NEW | [GAMEPLAY] | missing: recon.c samples one input per frame (v3 behaviour) | v7 src/libs/settings.js:55 |
| `lagComp` | Lag Compensation | slider | 1 | 0..1 step 0.1 | sent in `en` packet; server lag-comp amount | SAME | [GAMEPLAY] | present: ui_render_settings.c -> main.c ENT lag_comp | v7 src/libs/settings.js:74; v3 src/libs/settings.js:75 |
| `netRerouting` | Auto Routing (Affects Ping) | checkbox | false |  | route ip_ hosts through CloudFlare | SAME | [CLIENT] | partial: stored only (N/A natively) | v7 src/libs/settings.js:87; v3 src/libs/settings.js:87 |
| `showNetStat` | Show Network Stats | checkbox | false |  | packet stats / ingress display | CHANGED: cat interface->network | [CLIENT] | present: ui_render_settings.c -> hud_ui.c | v7 src/libs/settings.js:99; v3 src/libs/settings.js:722 |

#### `serverb`

| Key | Label (en) | Type | Default | Range / options | Effect | v3 -> v7 | Scope | Krunker Box Office | Ref |
|---|---|---|---|---|---|---|---|---|---|
| `oldBrowser` | Legacy Browser | checkbox | true |  | legacy server browser layout | CHANGED: default false->true; cat interface->serverb; label "Use Old Browser"->"Legacy Browser" | [CLIENT] | partial: stored only | v7 src/libs/settings.js:115; v3 src/libs/settings.js:547 |
| `defaultRegionOnly` | Default Region Only | checkbox | false |  | server browser lists default region only | NEW | [CLIENT] | missing: servers_ui.c | v7 src/libs/settings.js:126 |

#### `experimental`

| Key | Label (en) | Type | Default | Range / options | Effect | v3 -> v7 | Scope | Krunker Box Office | Ref |
|---|---|---|---|---|---|---|---|---|---|
| `rawMouse` | Raw Mouse Input | checkbox | false |  | raw/unaccelerated mouse input | NEW | [CLIENT] | partial: KN Client tab `rawInput` (ui_client_settings.c) | v7 src/libs/settings.js:137 |
| `flickClamp` | Mouse Flick Fix | checkbox+slider | 200 | 0..500 step 10 | max per-event mouse delta when flick fix on | SAME | [CLIENT] | partial: stored only | v7 src/libs/settings.js:148; v3 src/libs/settings.js:118 |
| `mouseFlick` | Mouse Flick Fix | checkbox (paired, hidden) | false (mobile true) |  | enable mouse flick clamp | CHANGED: mobile default | [CLIENT] | partial: stored only | v7 src/libs/settings.js:164; v3 src/libs/settings.js:137 |
| `aimFreezeFix` | Aim Freeze Fix | checkbox | false |  | use rAF while shooting (frame pacing) | SAME | [CLIENT] | partial: stored only | v7 src/libs/settings.js:176; v3 src/libs/settings.js:109 |
| `shaderRendering` | Instance Rendering | checkbox | false |  | instanced rendering (no-op set in v3) | CHANGED: label "Shader Based Rendering"->"Instance Rendering" | [CLIENT] | present: ui_render_settings.c (no-op, as v3) | v7 src/libs/settings.js:186; v3 src/libs/settings.js:149 |
| `mouseAccel` | Mouse Acceleration | checkbox | true |  | pointer lock without unadjustedMovement | NEW | [CLIENT] | partial: covered by Client `rawInput` | v7 src/libs/settings.js:197 |

#### `kpd`

| Key | Label (en) | Type | Default | Range / options | Effect | v3 -> v7 | Scope | Krunker Box Office | Ref |
|---|---|---|---|---|---|---|---|---|---|
| `kpdVision` | KPD Vision | checkbox (paired, hidden) | true |  | KPD (staff) wallhack vision toggle | NEW | [CLIENT] | n/a (needs: KPD): staff only | v7 src/libs/settings.js:208 |
| `kpdVisionCol` | KPD Vision | checkbox+color | #00ffff |  | KPD vision color | NEW | [CLIENT] | n/a (needs: KPD): staff only | v7 src/libs/settings.js:222 |

#### `dev`

| Key | Label (en) | Type | Default | Range / options | Effect | v3 -> v7 | Scope | Krunker Box Office | Ref |
|---|---|---|---|---|---|---|---|---|---|
| `forceUpdate` | Force Update Skins | checkbox | true |  | dev: push skin changes live (prmA) | NEW | [GAMEPLAY] | n/a (needs: dev): dev only | v7 src/libs/settings.js:237 |
| `forceRegen` | Always Regenerate Player | checkbox | false |  | dev: regenerate player on skin update | NEW | [GAMEPLAY] | n/a (needs: dev): dev only | v7 src/libs/settings.js:246 |
| `extraBan` | Extra Ban Info | checkbox | false |  | dev: extra ban info | NEW | [CLIENT] | n/a (needs: dev): dev only | v7 src/libs/settings.js:255 |

### Controls tab

#### `gameplay`

| Key | Label (en) | Type | Default | Range / options | Effect | v3 -> v7 | Scope | Krunker Box Office | Ref |
|---|---|---|---|---|---|---|---|---|---|
| `gpSetts` | Gameplay Settings | select | 0 | 0=All, 1=Per Weapon | edit sensitivities for all / per weapon | NEW | [CLIENT] | missing (needs: per-weapon settings): no per-weapon settings | v7 src/libs/settings.js:2396 |
| `sensitivityX` | X Sensitivity | slider | 1 (mobile 1.4) | 0.1..15 step 0.1 | mouse X sens (per weapon) | CHANGED: mobile default; per-weapon | [CLIENT] | present (needs v7 change): ui_settings.c; global only | v7 src/libs/settings.js:2414; v3 src/libs/settings.js:1421 |
| `sensitivityY` | Y Sensitivity | slider | 1 (mobile 1.4) | 0.1..15 step 0.1 | mouse Y sens (per weapon) | CHANGED: mobile default; per-weapon | [CLIENT] | present (needs v7 change): ui_settings.c; global only | v7 src/libs/settings.js:2429; v3 src/libs/settings.js:1433 |
| `aimSensitivityX` | Aim X Sensitivity | slider | 1 (mobile 1.4) | 0.1..15 step 0.1 | ADS X sens (per weapon) | CHANGED: mobile default; per-weapon | [CLIENT] | present (needs v7 change): ui_settings.c; global only | v7 src/libs/settings.js:2444; v3 src/libs/settings.js:1445 |
| `aimSensitivityY` | Aim Y Sensitivity | slider | 1 (mobile 1.4) | 0.1..15 step 0.1 | ADS Y sens (per weapon) | CHANGED: mobile default; per-weapon | [CLIENT] | present (needs v7 change): ui_settings.c; global only | v7 src/libs/settings.js:2460; v3 src/libs/settings.js:1458 |
| `scrollDir` | Scroll Direction | checkbox | true |  | scroll weapon direction (per weapon) | CHANGED: per-weapon | [CLIENT] | present (needs v7 change): ui_controls.c; global only | v7 src/libs/settings.js:2476; v3 src/libs/settings.js:1482 |
| `invertY` | Invert Y-Axis | checkbox | false |  | invert mouse Y (per weapon) | CHANGED: per-weapon | [CLIENT] | present (needs v7 change): ui_controls.c; global only | v7 src/libs/settings.js:2490; v3 src/libs/settings.js:1502 |
| `challMode` | Challenge Mode | checkbox | false |  | challenge mode flag in `en` (half HP, no regen) | SAME | [GAMEPLAY] | partial: stored only; not in ENT | v7 src/libs/settings.js:2504; v3 src/libs/settings.js:1492 |

#### `controller`

| Key | Label (en) | Type | Default | Range / options | Effect | v3 -> v7 | Scope | Krunker Box Office | Ref |
|---|---|---|---|---|---|---|---|---|---|
| `disableCons` | Disable Controllers | checkbox | false |  | ignore gamepads | NEW | [CLIENT] | missing: gamepad.c | v7 src/libs/settings.js:2517 |
| `sensitivityXCntrl` | X Sensitivity | slider | 4 | 0.1..15 step 0.1 | stick X sens (*5) | CHANGED: default 1->4 | [CLIENT] | present (needs v7 change): ui_settings.c -> gamepad.c | v7 src/libs/settings.js:2529; v3 src/libs/settings.js:1512 |
| `sensitivityYCntrl` | Y Sensitivity | slider | 4 | 0.1..15 step 0.1 | stick Y sens (*5) | CHANGED: default 1->4 | [CLIENT] | present (needs v7 change): ui_settings.c -> gamepad.c | v7 src/libs/settings.js:2543; v3 src/libs/settings.js:1524 |
| `aimSensitivityXCntrl` | Aim X Sensitivity | slider | 3 | 0.1..15 step 0.1 | stick ADS X sens (*5) | CHANGED: default 1->3 | [CLIENT] | present (needs v7 change): ui_settings.c -> gamepad.c | v7 src/libs/settings.js:2557; v3 src/libs/settings.js:1536 |
| `aimSensitivityYCntrl` | Aim Y Sensitivity | slider | 3 | 0.1..15 step 0.1 | stick ADS Y sens (*5) | CHANGED: default 1->3 | [CLIENT] | present (needs v7 change): ui_settings.c -> gamepad.c | v7 src/libs/settings.js:2571; v3 src/libs/settings.js:1548 |
| `deadZoneL` | Dead Zone (Left) | slider | 0.3 | 0..1 step 0.05 | left stick dead zone | SAME | [CLIENT] | present: ui_settings.c -> gamepad.c | v7 src/libs/settings.js:2585; v3 src/libs/settings.js:1560 |
| `deadZoneR` | Dead Zone (Right) | slider | 0.25 | 0..1 step 0.05 | right stick dead zone | SAME | [CLIENT] | present: ui_settings.c -> gamepad.c | v7 src/libs/settings.js:2599; v3 src/libs/settings.js:1572 |
| `triggerThres` | Trigger Threshold | slider | 0.1 | 0..1 step 0.05 | trigger threshold | SAME | [CLIENT] | present: ui_settings.c -> gamepad.c | v7 src/libs/settings.js:2613; v3 src/libs/settings.js:1584 |
| `invertYCntrl` | Invert Y-Axis | checkbox | false |  | invert stick Y | SAME | [CLIENT] | present: ui_settings.c -> gamepad.c | v7 src/libs/settings.js:2627; v3 src/libs/settings.js:1596 |
| `vibrationMlt` | Vibration | checkbox+slider | 1 | 0..1 step 0.1 | vibration strength | NEW | [CLIENT] | missing: gamepad.c on/off only | v7 src/libs/settings.js:2638 |
| `vibration` | Vibration | checkbox (paired, hidden) | true |  | vibration on/off | CHANGED: now paired toggle of vibrationMlt | [CLIENT] | present (needs v7 change): ui_settings.c (now paired with vibrationMlt) | v7 src/libs/settings.js:2655; v3 src/libs/settings.js:1605 |
| `gradSpeed` | Gradual Speed | checkbox | false |  | analog gradual move speed | SAME | [CLIENT] | present: ui_settings.c | v7 src/libs/settings.js:2667; v3 src/libs/settings.js:1614 |

#### `mobile`

| Key | Label (en) | Type | Default | Range / options | Effect | v3 -> v7 | Scope | Krunker Box Office | Ref |
|---|---|---|---|---|---|---|---|---|---|
| `sensitivityXMbl` | X Sensitivity | slider | 1.4 | 0.1..15 step 0.1 | touch X sens (*4) | CHANGED: default 1->1.4 | [CLIENT] | present (needs v7 change): ui_mobile.c | v7 src/libs/settings.js:2678; v3 src/libs/settings.js:1624 |
| `sensitivityYMbl` | Y Sensitivity | slider | 1.4 | 0.1..15 step 0.1 | touch Y sens (*4) | CHANGED: default 1->1.4 | [CLIENT] | present (needs v7 change): ui_mobile.c | v7 src/libs/settings.js:2693; v3 src/libs/settings.js:1637 |
| `aimSensitivityXMbl` | Aim X Sensitivity | slider | 1.4 | 0.1..15 step 0.1 | touch ADS X sens | CHANGED: default 1->1.4 | [CLIENT] | present (needs v7 change): ui_mobile.c | v7 src/libs/settings.js:2708; v3 src/libs/settings.js:1650 |
| `aimSensitivityYMbl` | Aim Y Sensitivity | slider | 1.4 | 0.1..15 step 0.1 | touch ADS Y sens | CHANGED: default 1->1.4 | [CLIENT] | present (needs v7 change): ui_mobile.c | v7 src/libs/settings.js:2723; v3 src/libs/settings.js:1663 |
| `invertYMbl` | Invert Y-Axis | checkbox | false |  | touch invert Y | CHANGED: type slider->switch | [CLIENT] | present (needs v7 change): ui_mobile.c | v7 src/libs/settings.js:2738; v3 src/libs/settings.js:1676 |
| `vibrationMbl` | Vibration | checkbox | true |  | touch vibration | SAME | [CLIENT] | partial: stored only | v7 src/libs/settings.js:2750; v3 src/libs/settings.js:1686 |
| `gradSpeedMbl` | Gradual Speed | checkbox | true |  | joystick gradual speed | SAME | [CLIENT] | present: ui_mobile.c | v7 src/libs/settings.js:2762; v3 src/libs/settings.js:1696 |

#### `controls` (hidden in UI)

| Key | Label (en) | Type | Default | Range / options | Effect | v3 -> v7 | Scope | Krunker Box Office | Ref |
|---|---|---|---|---|---|---|---|---|---|
| `scoreBoardTrigger` | InGame Leaderboard Trigger Type | select | 1 | 0=Press (Toggle), 1=Continuous (Hold) | leaderboard key toggle vs hold | NEW | [CLIENT] | missing: ui_controls.c hold only | v7 src/libs/settings.js:4265 |
| `voiceChatTrigger` | Voicechat Trigger Type | select | 1 | 0=Press (Toggle), 1=Continuous (Hold) | voice key toggle vs hold | NEW | [CLIENT] | missing (needs: voice v7): voice.c | v7 src/libs/settings.js:4280 |

### Display tab

#### `interface`

| Key | Label (en) | Type | Default | Range / options | Effect | v3 -> v7 | Scope | Krunker Box Office | Ref |
|---|---|---|---|---|---|---|---|---|---|
| `scaleUI` | UI Scale | checkbox+slider | 0.7 (mobile 0.9) | 0.1..1 step 0.1 | menu/HUD scale | CHANGED: default 0.8->0.7 (mobile 0.9) | [CLIENT] | present (needs v7 change): menu_ui.c, nametags.c, vfx.c | v7 src/libs/settings.js:747; v3 src/libs/settings.js:487 |
| `uiScaling` | UI Scale | checkbox (paired, hidden) | true |  | enable UI scaling | SAME | [CLIENT] | present: menu_ui.c, social_ui.c | v7 src/libs/settings.js:765; v3 src/libs/settings.js:507 |
| `showUI` | Show UI | checkbox | true |  | hide game HUD + chat | SAME | [CLIENT] | present: hud_ui.c | v7 src/libs/settings.js:777; v3 src/libs/settings.js:518 |
| `oldScoreboard` | Use Old Scoreboard | checkbox | false (mobile true) |  | classic io scoreboard | CHANGED: mobile default | [CLIENT] | present: hud_ui.c | v7 src/libs/settings.js:791; v3 src/libs/settings.js:534 |
| `disableRarityAnim` | Disable Rarity Animations | checkbox | false |  | static rarity colors | SAME | [CLIENT] | present: ui_loadout.c | v7 src/libs/settings.js:804; v3 src/libs/settings.js:557 |
| `showPfp` | Show Profile Pictures | checkbox | true (mobile false) |  | profile pictures | CHANGED: mobile default | [CLIENT] | present: social_ui.c | v7 src/libs/settings.js:818; v3 src/libs/settings.js:568 |
| `dynamicHP` | Dynamic HP Bars | checkbox | true |  | dynamic nametag HP bars | SAME | [CLIENT] | present: nametags.c | v7 src/libs/settings.js:830; v3 src/libs/settings.js:578 |
| `showHitInd` | Show Hit Indicators | checkbox | true |  | hit direction indicators | SAME | [CLIENT] | present: hud_ui.c | v7 src/libs/settings.js:843; v3 src/libs/settings.js:589 |
| `hitColor` | Hit Indicator Color | color | #FF3F3F |  | hit indicator color | SAME | [CLIENT] | present: hud_ui.c | v7 src/libs/settings.js:861; v3 src/libs/settings.js:604 |
| `deathMarkerOpac` | Show Death Markers | select+slider | 1 | 0..1 step 0.1 | death marker opacity | NEW | [CLIENT] | missing (needs: death markers): no death markers | v7 src/libs/settings.js:874 |
| `deathMarkers` | Show Death Markers | select | 0 | 0=All, 1=Self Only, 2=Team Only, 3=None | death marker filter (all/self/team/none) | NEW | [CLIENT] | missing (needs: death markers): no death markers | v7 src/libs/settings.js:892 |
| `deathMarkerAdsOpac` | Death Marker ADS Opacity | slider | 1 | 0..1 step 0.1 | death marker opacity while ADS | NEW | [CLIENT] | missing (needs: death markers): no death markers | v7 src/libs/settings.js:916 |
| `deathMarkerDur` | Death Marker Duration (s) | slider | 7 | 0..10 step 1 | death marker lifetime | NEW | [CLIENT] | missing (needs: death markers): no death markers | v7 src/libs/settings.js:931 |
| `dmgType` | Show Damage | checkbox+select | 1 | 0=Rounded, 1=Float | damage number style (rounded/float) | NEW | [CLIENT] | missing: hud_ui.c rounded only | v7 src/libs/settings.js:946 |
| `showDMG` | Show Damage | checkbox (paired, hidden) | true |  | damage numbers on hit | CHANGED: now paired toggle of dmgType | [CLIENT] | present: hud_ui.c | v7 src/libs/settings.js:964; v3 src/libs/settings.js:614 |
| `dmgColor` | Damage Color | color | #DE292F |  | damage number color | CHANGED: type fixed switch->color | [CLIENT] | present: hud_ui.c | v7 src/libs/settings.js:980; v3 src/libs/settings.js:626 |
| `critColor` | Crit Color | color | #FFD542 |  | crit damage color | SAME | [CLIENT] | present: hud_ui.c | v7 src/libs/settings.js:993; v3 src/libs/settings.js:636 |
| `dmgScale` | Damage Scale | slider | 1 | 0.1..2 step 0.1 | damage number size (*30) | SAME | [CLIENT] | present: hud_ui.c | v7 src/libs/settings.js:1006; v3 src/libs/settings.js:646 |
| `showKills` | Show Kill Feed | checkbox | true |  | kill feed | SAME | [CLIENT] | present: hud_ui.c | v7 src/libs/settings.js:1022; v3 src/libs/settings.js:659 |
| `showKillC` | Show Kill Counter | checkbox | false |  | kill counter | SAME | [CLIENT] | present: hud_ui.c | v7 src/libs/settings.js:1035; v3 src/libs/settings.js:670 |
| `showDeaths` | Show Death Counter | checkbox | false |  | death counter | SAME | [CLIENT] | present: hud_ui.c | v7 src/libs/settings.js:1048; v3 src/libs/settings.js:680 |
| `showKD` | Show K/D Counter | checkbox | false |  | K/D counter (#kdCount) | NEW | [CLIENT] | missing: no kdCount element | v7 src/libs/settings.js:1062 |
| `showScore` | Show Score Counter | checkbox | false |  | score counter | SAME | [CLIENT] | present: hud_ui.c | v7 src/libs/settings.js:1075; v3 src/libs/settings.js:691 |
| `showStreak` | Show Streak Counter | checkbox | false |  | streak counter | SAME | [CLIENT] | present: hud_ui.c | v7 src/libs/settings.js:1088; v3 src/libs/settings.js:701 |
| `showPing` | Show Ping | checkbox | true |  | ping display | SAME | [CLIENT] | present: hud_ui.c | v7 src/libs/settings.js:1102; v3 src/libs/settings.js:712 |
| `showFPS` | Show FPS | checkbox | false |  | FPS display (game + menu) | SAME | [CLIENT] | present: ui_settings.c, hud_ui.c | v7 src/libs/settings.js:1114; v3 src/libs/settings.js:737 |
| `fpsRate` | FPS Update Rate (ms) | slider | 100 | 0..1000 step 10 | FPS display refresh interval (ms) | NEW | [CLIENT] | missing: hud_ui.c fixed rate | v7 src/libs/settings.js:1128 |
| `showSpeed` | Show Movement Speed | checkbox | false |  | speedometer | SAME | [CLIENT] | present: hud_ui.c | v7 src/libs/settings.js:1141; v3 src/libs/settings.js:751 |
| `speedOffX` | Speed X Offset | slider | 5 | 0..10 step 0.1 | speedometer X | SAME | [CLIENT] | present: hud_ui.c | v7 src/libs/settings.js:1153; v3 src/libs/settings.js:761 |
| `speedOffY` | Speed Y Offset | slider | 5.4 | 0..10 step 0.1 | speedometer Y | SAME | [CLIENT] | present: hud_ui.c | v7 src/libs/settings.js:1168; v3 src/libs/settings.js:773 |
| `speedScale` | Speed Scale | slider | 2.4 | 0.5..5 step 0.1 | speedometer size | SAME | [CLIENT] | present: hud_ui.c | v7 src/libs/settings.js:1183; v3 src/libs/settings.js:785 |
| `speedOpacity` | Speed Opacity | slider | 1 | 0..1 step 0.1 | speedometer opacity | SAME | [CLIENT] | present: hud_ui.c | v7 src/libs/settings.js:1198; v3 src/libs/settings.js:797 |
| `speedColor` | Speed Color (Current) | color | #ffea78 |  | speedometer color | SAME | [CLIENT] | present: hud_ui.c | v7 src/libs/settings.js:1213; v3 src/libs/settings.js:809 |
| `speedColorMax` | Speed Color (Max) | color | #ff0000 |  | speedometer max color | SAME | [CLIENT] | present: hud_ui.c | v7 src/libs/settings.js:1226; v3 src/libs/settings.js:819 |
| `hideNames` | Nametag Display | select | 0 | 0=Everyone, 1=Team Only, 2=Enemy Only, 3=Off | nametag filter | SAME | [CLIENT] | present: nametags.c | v7 src/libs/settings.js:1239; v3 src/libs/settings.js:840 |
| `nametagOpac` | Nametag Opacity | slider | 1 | 0..1 step 0.1 | nametag opacity | SAME | [CLIENT] | present: nametags.c | v7 src/libs/settings.js:1256; v3 src/libs/settings.js:854 |
| `nametagScale` | Nametag Scale | slider | 1 | 0..1 step 0.1 | nametag size | NEW | [CLIENT] | missing: nametags.c | v7 src/libs/settings.js:1271 |
| `nametagStyle` | Nametag Style | select | 0 | 0=Everything, 1=Name Only, 2=Name & Level Only, 3=Health Only | nametag content | SAME | [CLIENT] | present: nametags.c | v7 src/libs/settings.js:1286; v3 src/libs/settings.js:866 |
| `healthNum` | Nametag Health Number | checkbox | false |  | HP number on nametag | SAME | [CLIENT] | present: nametags.c | v7 src/libs/settings.js:1304; v3 src/libs/settings.js:882 |
| `healthColT` | Nametag Health Color Team | color | #9eeb56 |  | team HP bar color | SAME | [CLIENT] | present: nametags.c | v7 src/libs/settings.js:1317; v3 src/libs/settings.js:892 |
| `healthColE` | Nametag Health Color Enemy | color | #eb5656 |  | enemy HP bar color | SAME | [CLIENT] | present: nametags.c | v7 src/libs/settings.js:1330; v3 src/libs/settings.js:902 |
| `objOpac` | Objective Opacity | slider | 1 | 0..1 step 0.1 | objective info opacity | NEW | [CLIENT] | missing: hud_modes.c | v7 src/libs/settings.js:1343 |
| `adsObjOpac` | ADS Objective Opacity | slider | 0.5 | 0..1 step 0.1 | objective info opacity while ADS | NEW | [CLIENT] | missing: hud_modes.c | v7 src/libs/settings.js:1358 |
| `hideNonTrade` | Hide Non-Tradable Skins | checkbox | false |  | hide non-tradable skins in inventory | NEW | [CLIENT] | missing (needs: trading/market): no v7 trading/market | v7 src/libs/settings.js:1373 |

#### `minimap`

| Key | Label (en) | Type | Default | Range / options | Effect | v3 -> v7 | Scope | Krunker Box Office | Ref |
|---|---|---|---|---|---|---|---|---|---|
| `showMinimap` | Show Minimap | checkbox | true |  | minimap on/off | NEW | [CLIENT] | missing (needs: minimap): no minimap | v7 src/libs/settings.js:1384 |
| `minimapScale` | Minimap Scale | slider | 1 | 0.5..1.5 step 0.1 | minimap size | NEW | [CLIENT] | missing (needs: minimap): no minimap | v7 src/libs/settings.js:1398 |
| `minimapZoom` | Minimap Zoom | slider | 1 | 0.5..1.5 step 0.1 | minimap zoom | NEW | [CLIENT] | missing (needs: minimap): no minimap | v7 src/libs/settings.js:1412 |
| `miniMapHudScale` | Minimap Hud Scale | slider | 1 | 0.5..1.5 step 0.1 | minimap HUD icon scale | NEW | [CLIENT] | missing (needs: minimap): no minimap | v7 src/libs/settings.js:1426 |

#### `medals`

| Key | Label (en) | Type | Default | Range / options | Effect | v3 -> v7 | Scope | Krunker Box Office | Ref |
|---|---|---|---|---|---|---|---|---|---|
| `showMedals` | Show Medals | checkbox | true |  | medal popups | CHANGED: cat interface->medals | [CLIENT] | present: hud_popups.c | v7 src/libs/settings.js:1440; v3 src/libs/settings.js:829 |
| `classicMedals` | Classic Medals | checkbox | false |  | classic (v3-style) medal display | NEW | [CLIENT] | missing (needs: v7 medals): KN only has classic medals | v7 src/libs/settings.js:1451 |
| `playMedals` | Play Medal Sounds | checkbox | true |  | medal sounds | NEW | [CLIENT] | missing (needs: v7 medals): hud_popups.c | v7 src/libs/settings.js:1461 |
| `medalScale` | Medal Scale | slider | 1 | 0..2 step 0.1 | medal size | NEW | [CLIENT] | missing (needs: v7 medals): hud_popups.c | v7 src/libs/settings.js:1474 |
| `medalOffX` | Medal X Offset | slider | 5 | 0..10 step 0.1 | medal X | NEW | [CLIENT] | missing (needs: v7 medals): hud_popups.c | v7 src/libs/settings.js:1488 |
| `medalOffY` | Medal Y Offset | slider | 1 | 0..10 step 0.1 | medal Y | NEW | [CLIENT] | missing (needs: v7 medals): hud_popups.c | v7 src/libs/settings.js:1502 |
| `medalSat` | Medal Saturation | slider | 1 | 0..3 step 0.1 | medal saturation filter | NEW | [CLIENT] | missing (needs: v7 medals): hud_popups.c | v7 src/libs/settings.js:1516 |
| `medalHue` | Medal Color Hue | slider | 0 | 0..100 step 5 | medal hue filter | NEW | [CLIENT] | missing (needs: v7 medals): hud_popups.c | v7 src/libs/settings.js:1532 |

#### `crosshair`

| Key | Label (en) | Type | Default | Range / options | Effect | v3 -> v7 | Scope | Krunker Box Office | Ref |
|---|---|---|---|---|---|---|---|---|---|
| `crosshairSho` | Type | select | 1 | 0=Off, 1=Dynamic, 2=Shapes, 3=Layered, 4=Image, 5=Precision | crosshair type | CHANGED: labels 1 Default->Dynamic, 2 Custom->Shapes | [CLIENT] | present: hud_ui.c | v7 src/libs/settings.js:1669; v3 src/libs/settings.js:1049 |
| `crosshairStyle` | Style | select | 0 | 0=Cross, 1=Hollow Circle, 2=Solid Circle, 3=Hollow Square, 4=Solid Square | crosshair preset shape | SAME | [CLIENT] | present: hud_ui.c | v7 src/libs/settings.js:1727; v3 src/libs/settings.js:1116 |
| `crosshairImage` | Image | url | (empty) |  | custom crosshair image | SAME | [CLIENT] | present: hud_ui.c | v7 src/libs/settings.js:1746; v3 src/libs/settings.js:1134 |
| `crosshairImageW` | Image Width | slider | 0 | 0..1000 step 1 | crosshair image width | NEW | [CLIENT] | missing: hud_ui.c natural size | v7 src/libs/settings.js:1783 |
| `crosshairImageH` | Image Height | slider | 0 | 0..1000 step 1 | crosshair image height | NEW | [CLIENT] | missing: hud_ui.c natural size | v7 src/libs/settings.js:1798 |
| `crosshairOpacity` | Custom Opacity | checkbox+slider | 1 | 0..1 step 0.1 | custom crosshair opacity | SAME | [CLIENT] | present: hud_ui.c | v7 src/libs/settings.js:1813; v3 src/libs/settings.js:1150 |
| `crosshairUseOpacity` | Use Custom Opacity | checkbox (paired, hidden) | false |  | enable custom opacity | SAME | [CLIENT] | present: hud_ui.c | v7 src/libs/settings.js:1831; v3 src/libs/settings.js:1169 |
| `crosshairAlways` | Always Show | checkbox | false |  | crosshair while ADS | SAME | [CLIENT] | present: hud_ui.c | v7 src/libs/settings.js:1843; v3 src/libs/settings.js:1180 |
| `crosshairColor` | Color | color | #ffffff |  | crosshair color | SAME | [CLIENT] | present: hud_ui.c | v7 src/libs/settings.js:1857; v3 src/libs/settings.js:1192 |
| `crosshairShadow` | Shadow Color | color | #000000 |  | crosshair shadow color | SAME | [CLIENT] | present: hud_ui.c | v7 src/libs/settings.js:1869; v3 src/libs/settings.js:1202 |
| `crosshairShadowThickess` | Shadow Thickness | slider | 1 | 0..3 step 0.1 | crosshair shadow width | CHANGED: step 1->0.1 | [CLIENT] | present (needs v7 change): hud_ui.c | v7 src/libs/settings.js:1881; v3 src/libs/settings.js:1212 |
| `crosshairThick` | Thickness | slider | 2 | 0..100 step 0.5 | crosshair thickness | SAME | [CLIENT] | present: hud_ui.c | v7 src/libs/settings.js:1896; v3 src/libs/settings.js:1225 |
| `crosshairLen` | Size | slider | 16 | 0..100 step 0.5 | crosshair length | SAME | [CLIENT] | present: hud_ui.c | v7 src/libs/settings.js:1911; v3 src/libs/settings.js:1238 |
| `crosshairGap` | Gap | slider | 0 | -100..100 step 1 | crosshair gap | SAME | [CLIENT] | present: hud_ui.c | v7 src/libs/settings.js:1925; v3 src/libs/settings.js:1251 |
| `crosshairDot` | Dot | checkbox | true |  | center dot | CHANGED: default false->true | [CLIENT] | present (needs v7 change): hud_ui.c | v7 src/libs/settings.js:1940; v3 src/libs/settings.js:1264 |

#### `crosshairm`

| Key | Label (en) | Type | Default | Range / options | Effect | v3 -> v7 | Scope | Krunker Box Office | Ref |
|---|---|---|---|---|---|---|---|---|---|
| `crosshairShoM` | Type | select | 3 | 0=Off, 1=Shapes, 2=Image, 3=Precision | throw crosshair type (melee canThrow, not ADS) | NEW | [CLIENT] | missing (needs: throw crosshair): no throw crosshair (knife can throw) | v7 src/libs/settings.js:1953 |
| `crosshairStyleM` | Style | select | 0 | 0=Cross, 1=Hollow Circle, 2=Solid Circle, 3=Hollow Square, 4=Solid Square | throw crosshair preset | NEW | [CLIENT] | missing (needs: throw crosshair): no throw crosshair (knife can throw) | v7 src/libs/settings.js:2009 |
| `crosshairImageM` | Image | url | (empty) |  | throw crosshair image | NEW | [CLIENT] | missing (needs: throw crosshair): no throw crosshair (knife can throw) | v7 src/libs/settings.js:2028 |
| `crosshairImageWM` | Image Width | slider | 0 | 0..1000 step 1 | throw crosshair image W | NEW | [CLIENT] | missing (needs: throw crosshair): no throw crosshair (knife can throw) | v7 src/libs/settings.js:2064 |
| `crosshairImageHM` | Image Height | slider | 0 | 0..1000 step 1 | throw crosshair image H | NEW | [CLIENT] | missing (needs: throw crosshair): no throw crosshair (knife can throw) | v7 src/libs/settings.js:2079 |
| `crosshairOpacityM` | Custom Opacity | checkbox+slider | 1 | 0..1 step 0.1 | throw crosshair opacity | NEW | [CLIENT] | missing (needs: throw crosshair): no throw crosshair (knife can throw) | v7 src/libs/settings.js:2094 |
| `crosshairUseOpacityM` | Use Custom Opacity | checkbox (paired, hidden) | false |  | enable throw opacity | NEW | [CLIENT] | missing (needs: throw crosshair): no throw crosshair (knife can throw) | v7 src/libs/settings.js:2112 |
| `crosshairAlwaysM` | Always Show | checkbox | false |  | throw crosshair always | NEW | [CLIENT] | missing (needs: throw crosshair): no throw crosshair (knife can throw) | v7 src/libs/settings.js:2124 |
| `crosshairColorM` | Color | color | #ffffff |  | throw crosshair color | NEW | [CLIENT] | missing (needs: throw crosshair): no throw crosshair (knife can throw) | v7 src/libs/settings.js:2138 |
| `crosshairShadowM` | Shadow Color | color | #000000 |  | throw crosshair shadow color | NEW | [CLIENT] | missing (needs: throw crosshair): no throw crosshair (knife can throw) | v7 src/libs/settings.js:2150 |
| `crosshairShadowThickessM` | Shadow Thickness | slider | 1 | 0..3 step 0.1 | throw crosshair shadow width | NEW | [CLIENT] | missing (needs: throw crosshair): no throw crosshair (knife can throw) | v7 src/libs/settings.js:2162 |
| `crosshairThickM` | Thickness | slider | 3 | 0..100 step 0.5 | throw crosshair thickness | NEW | [CLIENT] | missing (needs: throw crosshair): no throw crosshair (knife can throw) | v7 src/libs/settings.js:2176 |
| `crosshairLenM` | Size | slider | 16 | 0..100 step 0.5 | throw crosshair size | NEW | [CLIENT] | missing (needs: throw crosshair): no throw crosshair (knife can throw) | v7 src/libs/settings.js:2191 |
| `crosshairGapM` | Gap | slider | 14 | 0..100 step 1 | throw crosshair gap | NEW | [CLIENT] | missing (needs: throw crosshair): no throw crosshair (knife can throw) | v7 src/libs/settings.js:2204 |
| `crosshairDotM` | Dot | checkbox | true |  | throw crosshair dot | NEW | [CLIENT] | missing (needs: throw crosshair): no throw crosshair (knife can throw) | v7 src/libs/settings.js:2218 |

#### `hitmarker`

| Key | Label (en) | Type | Default | Range / options | Effect | v3 -> v7 | Scope | Krunker Box Office | Ref |
|---|---|---|---|---|---|---|---|---|---|
| `hitm` | Show | checkbox | true |  | hitmarker on/off | SAME | [CLIENT] | present: hud_ui.c | v7 src/libs/settings.js:2231; v3 src/libs/settings.js:1275 |
| `hitmCol` | Color | color | #FFFFFF |  | hitmarker color | SAME | [CLIENT] | present: hud_ui.c | v7 src/libs/settings.js:2243; v3 src/libs/settings.js:1285 |
| `hitmKCol` | Kill Color | color | #FF0000 |  | kill hitmarker color | SAME | [CLIENT] | present: hud_ui.c | v7 src/libs/settings.js:2255; v3 src/libs/settings.js:1298 |
| `hitOpac` | Opacity | slider | 1 | 0..1 step 0.1 | hitmarker opacity | SAME | [CLIENT] | present: hud_ui.c | v7 src/libs/settings.js:2267; v3 src/libs/settings.js:1311 |
| `hitmLen` | Length | slider | 25 | 1..50 step 1 | hitmarker length | SAME | [CLIENT] | present: hud_ui.c | v7 src/libs/settings.js:2281; v3 src/libs/settings.js:1323 |
| `hitmThick` | Thickness | slider | 7 | 1..20 step 1 | hitmarker thickness | SAME | [CLIENT] | present: hud_ui.c | v7 src/libs/settings.js:2295; v3 src/libs/settings.js:1335 |
| `hitmSpac` | Spacing | slider | 37 | 5..60 step 1 | hitmarker spacing | SAME | [CLIENT] | present: hud_ui.c | v7 src/libs/settings.js:2309; v3 src/libs/settings.js:1347 |
| `hitmAnimD` | Anim Size | slider | 0.14 | 0.1..0.5 step 0.01 | hitmarker anim size | SAME | [CLIENT] | present: hud_ui.c | v7 src/libs/settings.js:2323; v3 src/libs/settings.js:1359 |
| `hitmAnimS` | Anim Speed | slider | 0.008 | 0.001..0.01 step 0.001 | hitmarker anim speed | SAME | [CLIENT] | partial: stored only | v7 src/libs/settings.js:2337; v3 src/libs/settings.js:1371 |
| `hitmFad` | Fade Speed | slider | 0.0062 | 0.0001..0.01 step 0.0001 | hitmarker fade speed | SAME | [CLIENT] | present: hud_ui.c | v7 src/libs/settings.js:2351; v3 src/libs/settings.js:1383 |
| `customHitmarker` | Hitmarker Image | url | (empty) |  | custom hitmarker image | SAME | [CLIENT] | partial: stored only | v7 src/libs/settings.js:2364; v3 src/libs/settings.js:1394 |

#### `chat`

| Key | Label (en) | Type | Default | Range / options | Effect | v3 -> v7 | Scope | Krunker Box Office | Ref |
|---|---|---|---|---|---|---|---|---|---|
| `shwChat` | Show Chat Box | select | 0 | 0=Always, 1=While Focused, 2=Off | chat box visibility | SAME | [CLIENT] | present: ui_controls.c | v7 src/libs/settings.js:1546; v3 src/libs/settings.js:934 |
| `chatFilter` | Profanity Filter | checkbox | false |  | profanity filter | SAME | [CLIENT] | partial: stored only | v7 src/libs/settings.js:1562; v3 src/libs/settings.js:949 |
| `showMessages` | Show Player Messages | checkbox | true |  | show player chat | SAME | [CLIENT] | partial: stored only | v7 src/libs/settings.js:1573; v3 src/libs/settings.js:959 |
| `showUnboxings` | Show Unboxings | checkbox | true |  | show unboxing messages | SAME | [CLIENT] | partial: stored only | v7 src/libs/settings.js:1584; v3 src/libs/settings.js:969 |
| `chatOp` | Chat Opacity | slider | 1 | 0..1 step 0.1 | chat opacity | SAME | [CLIENT] | present: hud_ui.c | v7 src/libs/settings.js:1596; v3 src/libs/settings.js:980 |
| `chatBGOp` | Chat BG Opacity | slider | 0.2 | 0..1 step 0.1 | chat row background | SAME | [CLIENT] | present: hud_ui.c | v7 src/libs/settings.js:1610; v3 src/libs/settings.js:992 |
| `chatTextOutline` | Chat Text Outline | checkbox | false |  | chat text outline | SAME | [CLIENT] | present: hud_ui.c | v7 src/libs/settings.js:1626; v3 src/libs/settings.js:1009 |
| `chatHeight` | Chat Height | slider | 2.5 | 0..5 step 0.1 | chat max height | SAME | [CLIENT] | present: hud_ui.c | v7 src/libs/settings.js:1640; v3 src/libs/settings.js:1024 |
| `chatTimer` | Chat Timer (s) | slider | 0 | 0..10 step 0.5 | chat message lifetime | SAME | [CLIENT] | present: hud_ui.c | v7 src/libs/settings.js:1655; v3 src/libs/settings.js:1037 |

### Render tab

#### `quality`

| Key | Label (en) | Type | Default | Range / options | Effect | v3 -> v7 | Scope | Krunker Box Office | Ref |
|---|---|---|---|---|---|---|---|---|---|
| `resolution` | Resolution | slider | 0.6 (mobile 0.4) | 0.1..2 step 0.1 | render scale (setResMlt) | CHANGED: mobile default | [CLIENT] | present: ui_render_settings.c -> render_target.c | v7 src/libs/settings.js:264; v3 src/libs/settings.js:158 |
| `updateRate` | Frame Cap | slider | 0 | 0..1200 step 10 | frame cap (0 = unlimited, min 10) | SAME | [CLIENT] | present: ui_render_settings.c | v7 src/libs/settings.js:284; v3 src/libs/settings.js:172 |
| `aspectRatio` | Aspect Ratio | text | (empty) |  | force WxH aspect | SAME | [CLIENT] | present: ui_render_settings.c -> render_target.c | v7 src/libs/settings.js:301; v3 src/libs/settings.js:184 |
| `antiAlias` | Antialiasing | checkbox | false |  | MSAA (restart) | SAME | [CLIENT] | present: ui_render_settings.c -> render_target.c | v7 src/libs/settings.js:325; v3 src/libs/settings.js:206 |
| `lowSpec` | Low Spec | checkbox | false |  | low-spec renderer path | SAME | [CLIENT] | partial: stored only | v7 src/libs/settings.js:341; v3 src/libs/settings.js:228 |
| `noTex` | No Textures | checkbox | false |  | no map textures | SAME | [CLIENT] | present: prefab.c, ambient.c | v7 src/libs/settings.js:355; v3 src/libs/settings.js:239 |
| `mapDet` | Map Details | checkbox | true (mobile false) |  | map details (grass etc.) | CHANGED: mobile default | [CLIENT] | present: prefab.c, ambient.c | v7 src/libs/settings.js:371; v3 src/libs/settings.js:252 |
| `particles` | Particles | checkbox | true |  | particle effects on/off | SAME | [CLIENT] | present: vfx.c | v7 src/libs/settings.js:386; v3 src/libs/settings.js:264 |
| `particlesDist` | Particle Distance | slider | 160 | 10..1000 step 10 | particle cull distance | SAME | [CLIENT] | present: vfx.c | v7 src/libs/settings.js:401; v3 src/libs/settings.js:276 |
| `renderDist` | Render Distance | slider | 100 | 1..100 step 1 | camera far plane = v*60 | SAME | [CLIENT] | present: ui_render_settings.c | v7 src/libs/settings.js:416; v3 src/libs/settings.js:288 |
| `reflection` | Reflection Quality | select | 0 (mobile 2) | 0=Map Dependent, 1=32, 2=64, 3=128, 4=256, 5=1024, 6=2048 | water (Ocean) reflection RT size | NEW | [CLIENT] | missing (needs: water/Ocean): no water/Ocean renderer | v7 src/libs/settings.js:431 |
| `shadows` | Shadows | checkbox | true (mobile false) |  | shadow map on/off | CHANGED: mobile default | [CLIENT] | present: shadow.c | v7 src/libs/settings.js:453; v3 src/libs/settings.js:300 |
| `softShad` | Soft Shadows | checkbox | false |  | PCF soft shadows | SAME | [CLIENT] | partial: shadow.c: not ported | v7 src/libs/settings.js:467; v3 src/libs/settings.js:312 |
| `highResShad` | High-Res Shadows | checkbox | false |  | 4096 shadow map | SAME | [CLIENT] | present: shadow.c | v7 src/libs/settings.js:482; v3 src/libs/settings.js:324 |
| `shadowsDynamic` | Dynamic Shadows | checkbox | false |  | real-time shadow updates (players cast) | SAME | [CLIENT] | partial: shadow.c: not ported | v7 src/libs/settings.js:496; v3 src/libs/settings.js:336 |
| `ambientShading` | Old Shading (When Possible) | checkbox | true |  | old ambient shading mesh groups | CHANGED: label "Ambient Shading"->"Old Shading (When Possible)" | [CLIENT] | present: basic.c, ambient.c | v7 src/libs/settings.js:511; v3 src/libs/settings.js:348 |
| `showTrails` | Bullet Trails | checkbox | true |  | bullet trails | SAME | [CLIENT] | present: vfx.c | v7 src/libs/settings.js:526; v3 src/libs/settings.js:361 |
| `yourTrails` | Your Trails | checkbox | true |  | own bullet trails | SAME | [CLIENT] | present: vfx.c | v7 src/libs/settings.js:541; v3 src/libs/settings.js:373 |
| `muzzleFlash` | Show Muzzle Flash | checkbox | true (mobile false) |  | muzzle flash | CHANGED: mobile default; label "Muzzle Flash"->"Show Muzzle Flash" | [CLIENT] | present: vfx.c | v7 src/libs/settings.js:555; v3 src/libs/settings.js:384 |
| `bulletCasings` | Show Bullet Casings | checkbox | true |  | ejected casings on/off | NEW | [CLIENT] | missing: vfx.c has casings, no toggle | v7 src/libs/settings.js:570 |
| `impactHoles` | Show Bullet Impact Holes | checkbox | true |  | bullet hole decals on/off | NEW | [CLIENT] | missing: vfx.c has holes, no toggle | v7 src/libs/settings.js:585 |
| `sniperFlap` | Show Sniper Flap | checkbox | false |  | animated sniper scope flap | CHANGED: label "Sniper Flap"->"Show Sniper Flap" | [CLIENT] | partial: stored only (flap not rendered) | v7 src/libs/settings.js:600; v3 src/libs/settings.js:396 |
| `sniperFlapAnim` | Sniper Flap Animation | checkbox | true |  | sniper flap animation | NEW | [CLIENT] | missing (needs: sniper flap): needs sniperFlap | v7 src/libs/settings.js:613 |
| `textureAnim` | Texture Animations | checkbox | true |  | animated textures | SAME | [CLIENT] | present: main.c, sky.c, sprays.c | v7 src/libs/settings.js:626; v3 src/libs/settings.js:406 |
| `objectAnim` | Object Animations | checkbox | true |  | animated map objects | SAME | [CLIENT] | present: basic.c | v7 src/libs/settings.js:660; v3 src/libs/settings.js:434 |
| `screenShake` | Screen Shake | checkbox | true |  | explosion screen shake | SAME | [CLIENT] | present: camera_feel.c | v7 src/libs/settings.js:674; v3 src/libs/settings.js:445 |
| `weaponShine` | Weapons Shine | checkbox | false |  | weapon skin shine | SAME | [CLIENT] | partial: stored only | v7 src/libs/settings.js:690; v3 src/libs/settings.js:458 |
| `lighting` | Lighting | select | 1 (mobile 0) | 0=Low, 1=Normal, 2=High, 3=Physical | lighting quality (restart) | CHANGED: mobile default; adds 3=Physical | [CLIENT] | present (needs v7 change): basic.c, main.c; no 3 "Physical" | v7 src/libs/settings.js:704; v3 src/libs/settings.js:469 |
| `showExplo` | Show Explosions | checkbox | true |  | explosion animation/particles | NEW | [CLIENT] | missing: vfx.c explosions always on | v7 src/libs/settings.js:724 |

#### `post`

| Key | Label (en) | Type | Default | Range / options | Effect | v3 -> v7 | Scope | Krunker Box Office | Ref |
|---|---|---|---|---|---|---|---|---|---|
| `postProcessing` | Post Processing | checkbox | true (mobile false) |  | post FX composer | CHANGED: mobile default; cat quality->post | [CLIENT] | present: ui_render_settings.c (no visible pass, as v3) | v7 src/libs/settings.js:736; v3 src/libs/settings.js:219 |

#### `editing`

| Key | Label (en) | Type | Default | Range / options | Effect | v3 -> v7 | Scope | Krunker Box Office | Ref |
|---|---|---|---|---|---|---|---|---|---|
| `hudHealthHigh` | HUD Health High | color | #9eeb56 |  | HUD HP bar color (full) | CHANGED: type fixed switch->color | [CLIENT] | present: hud_spectate.c | v7 src/libs/settings.js:3500; v3 src/libs/settings.js:2059 |
| `hudHealthLow` | HUD Health Low | color | #eb5656 |  | HUD HP bar color (damaged) | SAME | [CLIENT] | present: hud_spectate.c | v7 src/libs/settings.js:3511; v3 src/libs/settings.js:2067 |
| `showSpdLines` | Speed Lines | slider | 0 | 0..1 step 0.1 | speed lines opacity | CHANGED: default 0.6->0 | [CLIENT] | present (needs v7 change): vfx.c | v7 src/libs/settings.js:3522; v3 src/libs/settings.js:2075 |
| `spdLinesCol` | Speed Lines Color | color | #e6e6e6 |  | speed lines color | SAME | [CLIENT] | present: vfx.c | v7 src/libs/settings.js:3537; v3 src/libs/settings.js:2088 |
| `showPScore` | Show Popup Score | checkbox | true |  | popup score | SAME | [CLIENT] | present: hud_popups.c | v7 src/libs/settings.js:3549; v3 src/libs/settings.js:2098 |
| `scoreColor` | Popup Score Color | color | #ffea78 |  | popup score color | SAME | [CLIENT] | present: hud_popups.c | v7 src/libs/settings.js:3561; v3 src/libs/settings.js:2108 |
| `scoreShadow` | Popup Score Shadow | checkbox | false |  | popup score shadow | SAME | [CLIENT] | present: hud_popups.c | v7 src/libs/settings.js:3573; v3 src/libs/settings.js:2118 |
| `scoreScale` | Popup Score Scale | slider | 1 | 0.1..2 step 0.1 | popup score size | SAME | [CLIENT] | present: hud_popups.c | v7 src/libs/settings.js:3585; v3 src/libs/settings.js:2128 |
| `scoreOffX` | Popup Score X Offset | slider | 5 | 0..10 step 0.1 | popup score X | SAME | [CLIENT] | present: hud_popups.c | v7 src/libs/settings.js:3599; v3 src/libs/settings.js:2141 |
| `scoreOffY` | Popup Score Y Offset | slider | 3.7 | 0..10 step 0.1 | popup score Y | SAME | [CLIENT] | present: hud_popups.c | v7 src/libs/settings.js:3613; v3 src/libs/settings.js:2153 |
| `progColor` | Progress Bar Color | color | #ffffff |  | interact progress color | NEW | [CLIENT] | missing: hud_ui.c (#interactProg fixed) | v7 src/libs/settings.js:3628 |
| `progOpac` | Progress Bar Opacity | slider | 1 | 0..1 step 0.1 | interact progress opacity | NEW | [CLIENT] | missing: hud_ui.c (#interactProg fixed) | v7 src/libs/settings.js:3639 |
| `progShadow` | Progress Bar Shadow | checkbox | true |  | interact progress shadow | NEW | [CLIENT] | missing: hud_ui.c (#interactProg fixed) | v7 src/libs/settings.js:3652 |
| `progScale` | Progress Bar Scale | slider | 1 | 0.1..2 step 0.1 | interact progress size | NEW | [CLIENT] | missing: hud_ui.c (#interactProg fixed) | v7 src/libs/settings.js:3663 |
| `progOffX` | Progress Bar X Offset | slider | 5 | 0..10 step 0.1 | interact progress X | NEW | [CLIENT] | missing: hud_ui.c (#interactProg fixed) | v7 src/libs/settings.js:3676 |
| `progOffY` | Progress Bar Y Offset | slider | 7.2 | 0..10 step 0.1 | interact progress Y | NEW | [CLIENT] | missing: hud_ui.c (#interactProg fixed) | v7 src/libs/settings.js:3689 |
| `progRot` | Progress Bar Rotation | slider | 0 | 0..360 step 1 | interact progress rotation | NEW | [CLIENT] | missing: hud_ui.c (#interactProg fixed) | v7 src/libs/settings.js:3702 |
| `progMColor` | Charge Bar Color | color | #ffffff |  | charge bar color | NEW | [CLIENT] | missing (needs: weapon charge): no weapon charge bar | v7 src/libs/settings.js:3715 |
| `progMOColor` | Overcharge Color | color | #eb2f2f |  | overcharge color | NEW | [CLIENT] | missing (needs: weapon charge): no weapon charge bar | v7 src/libs/settings.js:3727 |
| `progMOpac` | Charge Bar Opacity | slider | 1 | 0..1 step 0.1 | charge bar opacity | NEW | [CLIENT] | missing (needs: weapon charge): no weapon charge bar | v7 src/libs/settings.js:3739 |
| `progMShadow` | Charge Bar Shadow | checkbox | true |  | charge bar shadow | NEW | [CLIENT] | missing (needs: weapon charge): no weapon charge bar | v7 src/libs/settings.js:3753 |
| `progMScale` | Charge Bar Scale | slider | 0.5 | 0.1..2 step 0.1 | charge bar size | NEW | [CLIENT] | missing (needs: weapon charge): no weapon charge bar | v7 src/libs/settings.js:3765 |
| `progMOffX` | Charge Bar X Offset | slider | 5.6 | 0..10 step 0.1 | charge bar X | NEW | [CLIENT] | missing (needs: weapon charge): no weapon charge bar | v7 src/libs/settings.js:3779 |
| `progMOffY` | Charge Bar Y Offset | slider | 5 | 0..10 step 0.1 | charge bar Y | NEW | [CLIENT] | missing (needs: weapon charge): no weapon charge bar | v7 src/libs/settings.js:3793 |
| `progMRot` | Charge Bar Rotation | slider | 270 | 0..360 step 1 | charge bar rotation | NEW | [CLIENT] | missing (needs: weapon charge): no weapon charge bar | v7 src/libs/settings.js:3807 |
| `saturationn` | Saturation (In-game) | slider | 1 | 0..3 step 0.1 | in-game saturation filter | SAME | [CLIENT] | present: render_target.c | v7 src/libs/settings.js:3821; v3 src/libs/settings.js:2166 |
| `menuSaturation` | Saturation (UI) | slider | 1 | 0..3 step 0.1 | menu saturation filter | SAME | [CLIENT] | present: ui_render_settings.c | v7 src/libs/settings.js:3837; v3 src/libs/settings.js:2179 |
| `hue` | Color Hue (In-game) | slider | 0 | 0..100 step 5 | in-game hue filter | SAME | [CLIENT] | present: render_target.c | v7 src/libs/settings.js:3852; v3 src/libs/settings.js:2192 |
| `menuHue` | Color Hue (UI) | slider | 0 | 0..100 step 5 | menu hue filter | SAME | [CLIENT] | present: ui_render_settings.c | v7 src/libs/settings.js:3866; v3 src/libs/settings.js:2204 |
| `vignette` | Vignette | slider | 0 | 0..1 step 0.1 | vignette opacity | SAME | [CLIENT] | present: ui_render_settings.c | v7 src/libs/settings.js:3880; v3 src/libs/settings.js:2216 |
| `feedLimit` | Killfeed Limit | slider | 8 | 1..10 step 1 | kill feed rows | SAME | [CLIENT] | present: hud_ui.c | v7 src/libs/settings.js:3898; v3 src/libs/settings.js:2232 |
| `bulletTracers` | Bullet Tracers | checkbox (paired, hidden) | true |  | tracers on/off | SAME | [CLIENT] | present: vfx.c | v7 src/libs/settings.js:3913; v3 src/libs/settings.js:2260 |
| `bulletTracerCol` | Bullet Tracers | checkbox+color | #ffffdb |  | tracer color | SAME | [CLIENT] | present: vfx.c | v7 src/libs/settings.js:3927; v3 src/libs/settings.js:2245 |
| `tracerOffset` | Tracer Offset | slider | 0 | 0..1 step 0.01 | tracer start offset | NEW | [CLIENT] | missing: vfx.c | v7 src/libs/settings.js:3941 |
| `trailCol` | Bullet Trail Color | color | #ededed |  | bullet trail color | NEW | [CLIENT] | missing: vfx.c fixed color | v7 src/libs/settings.js:3954 |
| `ropeCol` | Grapple Rope Color | color | #b3b3b3 |  | grapple rope color | NEW | [CLIENT] | missing (needs: grapple): no grapple | v7 src/libs/settings.js:3966 |

### Game tab

#### `gameprivacy`

| Key | Label (en) | Type | Default | Range / options | Effect | v3 -> v7 | Scope | Krunker Box Office | Ref |
|---|---|---|---|---|---|---|---|---|---|
| `streamMode` | Streamer Mode | checkbox | false |  | streamer mode (hide names/ids) | SAME | [CLIENT] | present: nametags.c, streamer.c | v7 src/libs/settings.js:3002; v3 src/libs/settings.js:1813 |
| `anonMode` | Anonymous Mode | checkbox | false |  | anonymous flag in `en` | SAME | [GAMEPLAY] | partial: stored only; not sent | v7 src/libs/settings.js:3017; v3 src/libs/settings.js:1825 |
| `hideGameP` | Hide Game Title | text | (empty) |  | hide game title (no consumer in index.js) | NEW | [CLIENT] | missing: - | v7 src/libs/settings.js:3030 |
| `showRegion` | Show Region Info | checkbox | true |  | menu region label | NEW | [CLIENT] | missing: menu_ui.c | v7 src/libs/settings.js:3040 |
| `showServerTimer` | Show Server Timers | checkbox | true |  | server timers in browser/menu | NEW | [CLIENT] | missing: servers_ui.c | v7 src/libs/settings.js:3053 |
| `verifiedBadge` | Show Verified Badge | checkbox | true |  | verified badge flag in `en` | NEW | [GAMEPLAY] | missing (needs: badges): no badges | v7 src/libs/settings.js:3065 |
| `premiumBadge` | Show Premium Badge | checkbox | true |  | premium badge flag in `en` | NEW | [GAMEPLAY] | missing (needs: badges): no badges | v7 src/libs/settings.js:3081 |
| `partnerBadge` | Show Partner Badge | checkbox | true |  | partner badge flag in `en` | NEW | [GAMEPLAY] | missing (needs: badges): no badges | v7 src/libs/settings.js:3097 |
| `customBadge` | Show Custom Badge | checkbox | true |  | custom badge flag in `en` | NEW | [GAMEPLAY] | missing (needs: badges): no badges | v7 src/libs/settings.js:3113 |

#### `viewmodel`

| Key | Label (en) | Type | Default | Range / options | Effect | v3 -> v7 | Scope | Krunker Box Office | Ref |
|---|---|---|---|---|---|---|---|---|---|
| `vmSetts` | View Model Settings | select | 0 | 0=All, 1=Per Weapon | edit viewmodel for all / per weapon | NEW | [CLIENT] | missing (needs: per-weapon settings): no per-weapon settings | v7 src/libs/settings.js:3127 |
| `fov` | Field of View (FOV) | slider | 100 | 60..150 step 5 | camera FOV (per weapon) | CHANGED: default 90->100; label "Field of View"->"Field of View (FOV)"; per-weapon | [CLIENT] | present (needs v7 change): ui_settings.c; global only | v7 src/libs/settings.js:3145; v3 src/libs/settings.js:1837 |
| `fpsFOV` | Weapon FOV | slider | 95 | 60..150 step 5 | weapon model FOV (per weapon) | CHANGED: default 90->95; per-weapon | [CLIENT] | present (needs v7 change): ui_controls.c; global only | v7 src/libs/settings.js:3161; v3 src/libs/settings.js:1850 |
| `weaponBob` | Weapon Bobbing | slider | 0.7 | 0..3 step 0.1 | weapon bob | CHANGED: default 0.9->0.7; per-weapon | [CLIENT] | present (needs v7 change): player_mesh.c; global only | v7 src/libs/settings.js:3177; v3 src/libs/settings.js:1863 |
| `weaponLean` | Weapon Leaning | slider | 0.6 | 0..6 step 0.1 | weapon lean | CHANGED: default 1->0.6; per-weapon | [CLIENT] | present (needs v7 change): player_mesh.c; global only | v7 src/libs/settings.js:3195; v3 src/libs/settings.js:1877 |
| `weaponRot` | Weapon Rotation | slider | 0 | -2..2 step 0.1 | weapon roll | CHANGED: min -1->-2; max 1->2; per-weapon | [CLIENT] | present (needs v7 change): player_mesh.c; global only | v7 src/libs/settings.js:3213; v3 src/libs/settings.js:1891 |
| `weaponOffX` | Weapon X Offset | slider | 1 | 0..4 step 0.1 | weapon X offset | CHANGED: max 2->4; per-weapon | [CLIENT] | present (needs v7 change): player_mesh.c; global only | v7 src/libs/settings.js:3230; v3 src/libs/settings.js:1904 |
| `weaponOffY` | Weapon Y Offset | slider | 1.3 | 0..4 step 0.1 | weapon Y offset | CHANGED: default 1->1.3; max 2->4; per-weapon | [CLIENT] | present (needs v7 change): player_mesh.c; global only | v7 src/libs/settings.js:3247; v3 src/libs/settings.js:1917 |
| `weaponOffZ` | Weapon Z Offset | slider | 1 | 0..4 step 0.1 | weapon Z offset | CHANGED: max 2->4; per-weapon | [CLIENT] | present (needs v7 change): player_mesh.c; global only | v7 src/libs/settings.js:3264; v3 src/libs/settings.js:1930 |
| `weaponADSOffY` | Weapon ADS Y Offset | slider | 1 | 0..2 step 0.1 | weapon ADS Y offset | CHANGED: per-weapon | [CLIENT] | present (needs v7 change): player_mesh.c; global only | v7 src/libs/settings.js:3281; v3 src/libs/settings.js:1943 |
| `weapSwapY` | Weapon Swap Y | slider | 1 | -2..2 step 0.1 | swap animation Y travel | NEW | [CLIENT] | missing: player_mesh.c | v7 src/libs/settings.js:3297 |
| `weapReloadY` | Weapon Reload Y | slider | 1 | 0..1 step 0.1 | reload dip amount | NEW | [CLIENT] | missing: player_mesh.c | v7 src/libs/settings.js:3313 |
| `adsFovMlt` | Weapon ADS FOV Power | slider | 1 | 0.3..1 step 0.1 | ADS FOV multiplier (per weapon) | CHANGED: min 0.2->0.3; label "ADS FOV Power"->"Weapon ADS FOV Power"; per-weapon | [CLIENT] | present (needs v7 change): ui_settings.c; global only | v7 src/libs/settings.js:3329; v3 src/libs/settings.js:1965 |
| `toggleADS` | Toggle Weapon ADS | checkbox | false |  | aim key toggles instead of hold (per weapon) | NEW | [CLIENT] | missing: ui_controls.c | v7 src/libs/settings.js:3345 |
| `aimAnim` | Weapon Aim Animation | checkbox | true |  | ADS weapon animation | CHANGED: per-weapon | [CLIENT] | present (needs v7 change): player_mesh.c; global only | v7 src/libs/settings.js:3358; v3 src/libs/settings.js:1977 |
| `hideADS` | Hide Weapon on ADS | checkbox | false |  | hide weapon when fully ADS | CHANGED: per-weapon | [CLIENT] | present (needs v7 change): ui_render_settings.c; global only | v7 src/libs/settings.js:3373; v3 src/libs/settings.js:1988 |
| `showHands` | Show Hands | checkbox | true |  | hand models | SAME | [CLIENT] | present: player_mesh.c | v7 src/libs/settings.js:3389; v3 src/libs/settings.js:2000 |
| `showWeapon` | Show Primary | checkbox | true |  | primary model visible | SAME | [CLIENT] | present: ui_controls.c | v7 src/libs/settings.js:3403; v3 src/libs/settings.js:2011 |
| `showWeaponSec` | Show Secondary | checkbox | true |  | secondary model visible | SAME | [CLIENT] | present: ui_controls.c | v7 src/libs/settings.js:3417; v3 src/libs/settings.js:2023 |
| `showWeaponMel` | Show Melee | checkbox | true |  | melee model visible | SAME | [CLIENT] | present: ui_controls.c | v7 src/libs/settings.js:3431; v3 src/libs/settings.js:2035 |
| `leftHand` | Left Handed | checkbox | false |  | left-handed viewmodel | SAME | [CLIENT] | partial: stored only | v7 src/libs/settings.js:3445; v3 src/libs/settings.js:1955 |
| `roundArms` | Rounded Arms | checkbox | false |  | rounded arm models | SAME | [CLIENT] | partial: stored only | v7 src/libs/settings.js:3458; v3 src/libs/settings.js:2047 |
| `wallLeanMlt` | Wall Slide/Jump Lean | checkbox+slider | 0.5 | 0..1 step 0.1 | camera roll on wall slide/jump (*0.15) | NEW | [CLIENT] | missing: main.c camera | v7 src/libs/settings.js:3473 |
| `wallLean` | Wall Slide/Jump Lean | checkbox (paired, hidden) | false |  | enable wall lean | NEW | [CLIENT] | missing: main.c camera | v7 src/libs/settings.js:3488 |

#### `mods`

| Key | Label (en) | Type | Default | Range / options | Effect | v3 -> v7 | Scope | Krunker Box Office | Ref |
|---|---|---|---|---|---|---|---|---|---|
| `canLoadMods` | Load Mods | checkbox | true |  | allow mods | SAME | [CLIENT] | partial: stored only | v7 src/libs/settings.js:3978; v3 src/libs/settings.js:2272 |
| `canChangeLogo` | Allow Logo Changes | checkbox | true |  | mods may change logo | SAME | [CLIENT] | partial: stored only | v7 src/libs/settings.js:3989; v3 src/libs/settings.js:2281 |
| `autoLoadLast` | Auto-Load Mod | checkbox | false |  | auto-load last mod | SAME | [CLIENT] | partial: stored only | v7 src/libs/settings.js:4000; v3 src/libs/settings.js:2290 |
| `scopeBorderCol` | Scope Borders | checkbox+color | #000000 |  | scope border color | NEW | [CLIENT] | missing: ui_render_settings.c (black only) | v7 src/libs/settings.js:4011 |
| `scopeBorders` | Scope Borders | checkbox (paired, hidden) | true |  | scope side borders | CHANGED: now paired toggle of scopeBorderCol | [CLIENT] | present: ui_render_settings.c | v7 src/libs/settings.js:4026; v3 src/libs/settings.js:2297 |
| `scopeBorderOpac` | Scope Borders Opacity | slider | 1 | 0..1 step 0.1 | scope border opacity | NEW | [CLIENT] | missing: ui_render_settings.c | v7 src/libs/settings.js:4041 |
| `scopeWidth` | Scope Image Width | slider | 6.8 | 0.1..10 step 0.1 | scope image width (*100 px) | NEW | [CLIENT] | missing: fixed scope size | v7 src/libs/settings.js:4056 |
| `scopeHeight` | Scope Image Height | slider | 6.8 | 0.1..10 step 0.1 | scope image height (*100 px) | NEW | [CLIENT] | missing: fixed scope size | v7 src/libs/settings.js:4076 |
| `scopeOpac` | Scope Opacity | slider | 1 | 0..1 step 0.1 | scope image opacity mult | NEW | [CLIENT] | missing: fixed | v7 src/libs/settings.js:4096 |
| `reticleWidth` | Reticle Image Width | slider | 2 | 0.1..10 step 0.1 | ADS dot width | SAME | [CLIENT] | present: ui_render_settings.c | v7 src/libs/settings.js:4110; v3 src/libs/settings.js:2353 |
| `reticleHeight` | Reticle Image Height | slider | 2 | 0.1..10 step 0.1 | ADS dot height | SAME | [CLIENT] | present: ui_render_settings.c | v7 src/libs/settings.js:4125; v3 src/libs/settings.js:2366 |
| `endMessage` | Match End Message | text | (empty) |  | auto chat message at match end | SAME | [CLIENT] | partial: stored only | v7 src/libs/settings.js:4140; v3 src/libs/settings.js:2379 |
| `customAmmo` | Ammo Icon Image | url | (empty) |  | ammo icon URL | SAME | [CLIENT] | present: ui_render_settings.c | v7 src/libs/settings.js:4152; v3 src/libs/settings.js:2388 |
| `customKills` | Kills Icon Image | url | (empty) |  | kills icon URL | SAME | [CLIENT] | present: ui_render_settings.c | v7 src/libs/settings.js:4166; v3 src/libs/settings.js:2400 |
| `customDeaths` | Deaths Icon Image | url | (empty) |  | deaths icon URL | SAME | [CLIENT] | present: ui_render_settings.c | v7 src/libs/settings.js:4180; v3 src/libs/settings.js:2412 |
| `customStreak` | Streak Counter Icon Image | url | (empty) |  | streak icon URL | SAME | [CLIENT] | present: ui_render_settings.c | v7 src/libs/settings.js:4194; v3 src/libs/settings.js:2425 |
| `useDamageOverlay` | Use Damage Overlay | checkbox | true |  | damage blood overlay | SAME | [CLIENT] | present: vfx.c | v7 src/libs/settings.js:4208; v3 src/libs/settings.js:2438 |
| `customDamage` | Damage Overlay Image | url | (empty) |  | damage overlay image | SAME | [CLIENT] | partial: stored only | v7 src/libs/settings.js:4226; v3 src/libs/settings.js:2455 |
| `customGameOverlay` | Game Overlay Image | url | (empty) |  | game overlay image | SAME | [CLIENT] | present: ui_render_settings.c | v7 src/libs/settings.js:4251; v3 src/libs/settings.js:2491 |

### Sound tab

#### `audio`

| Key | Label (en) | Type | Default | Range / options | Effect | v3 -> v7 | Scope | Krunker Box Office | Ref |
|---|---|---|---|---|---|---|---|---|---|
| `sound` | Master Volume | slider | 0.8 | 0..1 step 0.01 | master volume | CHANGED: step 0.1->0.01 | [CLIENT] | present (needs v7 change): audio.c | v7 src/libs/settings.js:2775; v3 src/libs/settings.js:1707 |
| `ambientVolume` | Ambient Volume | slider | 1 | 0..1 step 0.01 | ambient volume | CHANGED: step 0.1->0.01 | [CLIENT] | present (needs v7 change): audio.c | v7 src/libs/settings.js:2792; v3 src/libs/settings.js:1721 |
| `dialogueVolume` | Action Volume | slider | 1 | 0..1 step 0.01 | "action" sound category 7 | NEW | [CLIENT] | missing (needs: action sounds): audio.c (no category) | v7 src/libs/settings.js:2807 |
| `audioInput` | Audio/Mic Input | select | default | default=Default | mic device | SAME | [CLIENT] | present: voice.c | v7 src/libs/settings.js:2822; v3 src/libs/settings.js:1733 |
| `micQuality` | Mic Quality | select | 6000 | 3000=Low, 6000=Medium, 9000=High, 12000=Epic, 15000=Ultra | voice bitrate, sent in `vc` | NEW | [GAMEPLAY] | missing (needs: voice v7): voice.c fixed | v7 src/libs/settings.js:2836 |
| `micVolume` | Mic Volume | slider | 1 | 0..1 step 0.01 | outgoing voice gain, sent in `vc` | NEW | [GAMEPLAY] | missing (needs: voice v7): voice.c | v7 src/libs/settings.js:2853 |
| `voiceVolume` | Voice Volume | slider | 1 | 0..1 step 0.01 | incoming voice volume | CHANGED: step 0.1->0.01 | [CLIENT] | present (needs v7 change): audio.c | v7 src/libs/settings.js:2867; v3 src/libs/settings.js:1752 |
| `voiceChatType` | Voice Chat Type | select | 0 | 0=Default, 1=Spacial / Proximity | default vs proximity (spatial) voice | NEW | [CLIENT] | missing (needs: proximity voice): voice.c non-spatial | v7 src/libs/settings.js:2882 |
| `voiceDistance` | Voice Chat Distance | slider | 5 | 1..10 step 1 | proximity voice refDistance (*5) | NEW | [CLIENT] | missing (needs: proximity voice): voice.c | v7 src/libs/settings.js:2901 |
| `gunsVolume` | Weapon Volume | slider | 0.3 | 0..1 step 0.01 | weapon volume | CHANGED: default 0.5->0.3; step 0.1->0.01 | [CLIENT] | present (needs v7 change): audio.c | v7 src/libs/settings.js:2915; v3 src/libs/settings.js:1765 |
| `playerVolume` | Player Volume | slider | 1 | 0..1 step 0.01 | player sounds volume | CHANGED: step 0.1->0.01 | [CLIENT] | present (needs v7 change): audio.c | v7 src/libs/settings.js:2930; v3 src/libs/settings.js:1777 |
| `skinVolume` | Cosmetics Volume | slider | 1 | 0..1 step 0.01 | cosmetic sounds category 8 | NEW | [CLIENT] | missing (needs: cosmetic sounds): audio.c (no category) | v7 src/libs/settings.js:2945 |
| `uiVolume` | UI Volume | slider | 1 | 0..1 step 0.01 | UI volume | CHANGED: step 0.1->0.01 | [CLIENT] | present (needs v7 change): audio.c | v7 src/libs/settings.js:2960; v3 src/libs/settings.js:1789 |
| `assetVolume` | Asset Volume | slider | 1 | 0..1 step 0.01 | map asset volume | CHANGED: step 0.1->0.01 | [CLIENT] | present (needs v7 change): audio.c | v7 src/libs/settings.js:2975; v3 src/libs/settings.js:1801 |
| `muteSpec` | Voice Mute Spectators | checkbox | false |  | mute voice from spectators/dead | NEW | [CLIENT] | missing (needs: voice v7): voice.c | v7 src/libs/settings.js:2990 |

### Removed in v7 (v3.1.1 settings with no v7 entry)

| Key | Label (en) | Type | Default | Range / options | Effect | v3 -> v7 | Scope | Krunker Box Office | Ref |
|---|---|---|---|---|---|---|---|---|---|
| `servSendR` | High Tickrate | checkbox | false |  | "High Tickrate" flag in ENT | REMOVED | [GAMEPLAY] | partial: stored only; not sent | v3 src/libs/settings.js:98 |
| `xpBarCol` | XP Bar Color | color | #FBC02D |  | in-game XP bar color | REMOVED | [CLIENT] | present: hud_ui.c | v3 src/libs/settings.js:912 |
| `xpBarOpac` | XP Bar Opacity | slider | 1 | 0..1 step 0.1 | in-game XP bar opacity | REMOVED | [CLIENT] | present: hud_ui.c | v3 src/libs/settings.js:922 |
| `slidCont` | Non FPS Sliding | checkbox | true |  | "Non FPS Sliding" flag in ENT | REMOVED | [GAMEPLAY] | present: main.c ENT slid_cont | v3 src/libs/settings.js:1471 |
| `customScope` | Scope Image | url | (empty) |  | scope image URL (v7: custom scope list krk_custScps) | REMOVED | [CLIENT] | present: ui_render_settings.c | v3 src/libs/settings.js:2311 |
| `customADSDot` | Reticle Image | url | (empty) |  | ADS dot image URL (v7: reticle picker) | REMOVED | [CLIENT] | present: ui_render_settings.c | v3 src/libs/settings.js:2338 |
| `customTimer` | Timer Icon Image | url | (empty) |  | timer icon URL | REMOVED | [CLIENT] | present: ui_render_settings.c | v3 src/libs/settings.js:2479 |

### Summary

**Counts (v7 `this.list`, before per-weapon cloning)**

| | count |
|---|---|
| v7 settings total | 287 |
| NEW in v7 | 101 |
| CHANGED (default/range/step/options/type/category/label/per-weapon) | 59 |
| SAME | 127 |
| REMOVED (v3 only) | 7 (`servSendR`, `xpBarCol`, `xpBarOpac`, `slidCont`, `customScope`, `customADSDot`, `customTimer`) |
| KN present (works as v7) | 120 |
| KN present but needs a v7 change (default/range/step/options/per-weapon) | 40 |
| KN partial (stored, shown in UI, but a no-op, or covered by a KN Client-tab option) | 28 |
| KN missing | 94 |
| n/a (KPD/dev only) | 5 |

All 7 removed keys still exist in KN, because KN's UI is the v3 page. Six of them work and `servSendR` is a no-op. For v7 parity they should be dropped or hidden. Two need a migration rather than a plain drop: v7 moves the old `customScope` URL into the custom-scope list (`krk_custScps`, v7 `src/index.js:873-879`) and replaces `customADSDot` with the reticle picker. `slidCont` and `servSendR` were also fields of the v3 ENT packet, and v7's `en` packet no longer carries them.

The 40 "needs v7 change" rows break down as:
- 18 of the 21 per-weapon keys (`perWep`/`perWepG`); the other 3 (`toggleADS`, `weapSwapY`, `weapReloadY`) are missing.
- New v7 desktop defaults: `fov` 90->100, `fpsFOV` 90->95, `weaponBob` 0.9->0.7, `weaponLean` 1->0.6, `weaponOffY` 1->1.3, `scaleUI` 0.8->0.7, `gunsVolume` 0.5->0.3, `crosshairDot` false->true, `showSpdLines` 0.6->0, controller sens 1->4/3, mobile sens 1->1.4.
- Wider ranges: `weaponRot` +-2, `weaponOff*` max 4, `adsFovMlt` min 0.3.
- Volume steps 0.1->0.01.
- `lighting` option 3 "Physical".
- `invertYMbl` slider->switch.

**Most important missing or partial settings, by player impact**

1. **Per-weapon settings**: `gpSetts`, `vmSetts` and the 21 per-weapon keys (sensitivity, aim sensitivity, scroll, invert, FOV, weapon FOV, viewmodel offsets/bob/lean/rot, ADS FOV, `toggleADS`, `aimAnim`, `hideADS`). KN keeps one global value each and needs a weapon-indexed store.
2. **`toggleADS`**: aim as toggle instead of hold, per weapon. A core input option.
3. **v7 defaults**: `fov` 100, `fpsFOV` 95, viewmodel bob/lean/offset, `scaleUI`, `gunsVolume`. These are what every new player sees first.
4. **`netRate`** [GAMEPLAY]: input sample rate, 144 Hz on desktop. KN samples one input per frame like v3 (`client/src/net/recon.c`). This changes what the server simulates.
5. **ENT/`en` flags KN never sends** [GAMEPLAY]: `challMode` (half HP, no regen) and `anonMode` are stored but not put in the ENT packet. The four badge flags (`verifiedBadge`/`premiumBadge`/`partnerBadge`/`customBadge`) are missing.
6. **Effect toggles for systems KN already renders**: `bulletCasings`, `impactHoles`, `showExplo`, `trailCol`, `tracerOffset`, plus the partial `softShad`/`shadowsDynamic` (not ported in `client/src/shadow.c`), `sniperFlap`, `weaponShine`, `lowSpec`.
7. **Viewmodel/camera**: `leftHand` and `roundArms` are no-ops. `weapSwapY`, `weapReloadY` and `wallLean`/`wallLeanMlt` (camera roll on walls; `player->on_wall` already exists) are missing.
8. **Crosshair/HUD**: the throwing crosshair (`crosshair*M`, 15 keys; KN's Combat Knife already has `can_throw`), `crosshairImageW/H`, `dmgType` (float damage), `showKD`, `nametagScale`, `objOpac`/`adsObjOpac`, `fpsRate`, the `prog*` interact-bar customisation (7 keys), the scope image options (`scopeWidth`/`scopeHeight`/`scopeOpac`/`scopeBorderCol`/`scopeBorderOpac`), and the partial `customHitmarker`, `hitmAnimS`, `customDamage`.
9. **Chat**: `chatFilter`, `showMessages` and `showUnboxings` are no-ops in KN.
10. **Input/controller**: `disableCons`, `vibrationMlt`, `scoreBoardTrigger` (toggle vs hold leaderboard), and the partial `flickClamp`/`mouseFlick`, `aimFreezeFix`, `rawMouse`/`mouseAccel` (KN has its own Client-tab `rawInput`).
11. **Privacy/browser**: `showRegion`, `showServerTimer`, `defaultRegionOnly`, `hideGameP` (v7 never reads it in `index.js`, so it can be skipped). `oldBrowser` is a no-op and its v7 default is now true.

**Settings that belong to feature work packages** (only meaningful once KN has the feature; group them with it):
- Minimap: `showMinimap`, `minimapScale`, `minimapZoom`, `miniMapHudScale`.
- Death markers: `deathMarkers`, `deathMarkerOpac`, `deathMarkerAdsOpac`, `deathMarkerDur`.
- v7 medal display: `classicMedals` (KN only has the classic style), `playMedals`, `medalScale`, `medalOffX/Y`, `medalSat`, `medalHue`.
- Voice chat v7: `voiceChatType` + `voiceDistance` (proximity/spatial voice), `micQuality`/`micVolume` (sent in `vc`), `muteSpec`, `voiceChatTrigger`.
- Sound categories: `dialogueVolume` (category 7, "Action") and `skinVolume` (category 8, cosmetic sounds).
- Weapon charge bar (crossbow/melee charge): `progMColor`, `progMOColor`, `progMOpac`, `progMShadow`, `progMScale`, `progMOffX/Y`, `progMRot`.
- Grapple: `ropeCol`. Water/Ocean reflections: `reflection`. Sniper flap model: `sniperFlap` (partial) + `sniperFlapAnim`. Lighting "Physical" mode: `lighting` option 3.
- Trading/market: `hideNonTrade`. Account badges: the four `*Badge` keys. Mods system: `canLoadMods`, `canChangeLogo`, `autoLoadLast` (partial).
- Per-weapon settings framework: `gpSetts`, `vmSetts` + the 21 per-weapon keys. Basic/advanced settings view: `sType` (85 keys).
- KPD/dev, not needed for players: `kpdVision`, `kpdVisionCol`, `forceUpdate`, `forceRegen`, `extraBan`.

## 2. The viewer: v7.1.4, v3.1.1 and Krunker Box Office

Sections 2.1-2.6 describe the v7.1.4 viewer, 2.7 the v3.1.1 viewer and the v3 -> v7 diff, 2.8 Krunker Box Office's viewer and its gaps.

### 2.1 Page, launch and data sources

| Aspect | v7.1.4 | Ref |
|---|---|---|
| Page | `public/viewer.html` (title "Krunker Texture Viewer"): one `<div id="container">` plus scripts `js/hook.js`, `libs/dat.gui.min.js`, `libs/zip.js`, `libs/fflate.min.js`, `js/viewer.js`. Inline CSS: `body{margin:0;overflow:hidden}`, `#container{position:absolute;width:100%;height:100%}` | v7 public/viewer.html:1-19 |
| Bundle | `src/viewer.js` is an esbuild entry point (with index/editor/social), output `public/js/viewer.js` + linked `viewer.js.map` | v7 server/compiler.ts:124-133 |
| Modules pulled in (source map) | `data/game.js` (the whole `Game`: store, weapons, classes, attach, players, pets, effects, all gameModes), `data/player.js` (Player), `data/players.js`, `data/playerMesh.js`, `data/pets.js`, `data/effects.js`, `data/store.js`, `data/skins.js`, `data/rarities.js`, `data/itemTypes.js`, `libs/render.js` (+ OBJ/FBX/GLTF loaders, MeshoptDecoder, SkeletonUtils), `libs/particles.js`, `libs/OrbitControls.js`, `config.js`, `previews.json` | v7 public/js/viewer.js.map `sources` |
| Electron launch | `npm run viewer` = `electron . --viewer`. `createWindow` sends `--viewer` to `handleNavigation(getBase()+"/viewer.html", true)`. `createViewerWindow` opens an 800x600 `subWin`; with `isSingle`, closing it quits the app | v7 package.json:43, main/main.js:403-404, 504-505, 530-532, 563-569 |
| In-game links | Mods window: `<a href='/viewer.html'>Model Viewer</a>`. **Item view**: `<iframe style='width:100%;height:450px' src='/viewer.html?nobg&showcase=<index>'>` for every non-player-card item | v7 src/index.js:5423, 10622 |
| Skin table | `data/skins.js` uses `window.skinfx` when it is set. Otherwise it does a **synchronous XHR `GET skins.jspck?v=AD9yU`**, decoded with `shim/jspck.js` (`public/skins.jspck`, 517 KB). `skins.json` at the repo root is the same data in readable form (6448 entries) and is also read by the server (`server/DBClient.ts:62`) | v7 src/data/skins.js:1-12 |
| Item type enum | `previews.json`: WEAPON 0, HAT 1, BODY 2, MELEE 3, SPRAY 4, DYE 5, WAIST 6, FACE 7, SHOE 8, PET 9, COLLECTIBLE 10, WRIST 11, CHARM 12, TICKET 13, BACK 14, HEAD 15, PLAYER_CARD 16 | v7 previews.json |
| Asset dirs | `store.types` = `weapons/weapon_, hats/hat_, body/body_, melee/melee_, sprays/, dyes/, waist/waist_, faces/face_, shoes/shoe_, pets/pet_, collectibles/collect_, wrist/wrist_, charms/charm_, tickets/ticket_, back/back_, head/head_, playercards/card_` | v7 src/data/store.js:106 |
| Model/texture URLs | `RENDER.loadMesh` uses `fileExt` (default `obj`; `loadAnimMesh` defaults to `gltf`) and loads `UTILS.assetsUrl("/models/<src>.<ext>")`, or `userAssetsUrl` when `asset` is set. `assetsUrl` gives `https://assets.krunker.io/...`, which `hook.js` rewrites to a relative local path (`public/models`, `public/textures`). `user-assets.krunker.io` is rewritten to `user-assets/`, which the server mirrors with a cache-on-miss download (`/user-assets/*`) | v7 src/libs/render.js:2068-2085, 2233-2261; src/libs/utils.js:672-699; public/js/hook.js:6025-6030; server/index.ts:645-662, 3815 |
| Other data | `store.rarities` (8: Uncommon..Unobtainable, NFT), `store.previews` (per-weapon xOff/yOff/zRota/scl), `store.templates` (dev export), `config.skinColors`, `config.currentSeason = 7`, `lightDistance 500`, `shadowRes 1024`, `idleAnimS`, `headScale 2`. NFT items are stripped unless `window.canShowNFTs` (`UTILS.removeNFT`) | v7 src/data/store.js:233-372; src/config.js:164, 2025-2027, 2092, 2157, 2854; v7 src/viewer.js:24-26 |
| Content volume (skins.json) | 6448 items: weapon skins 3904, face 544, spray 466, head 362, melee 280, hat 270, back 229, body 131, waist 71, shoe 59, card 46, dye 30, collectible 26, pet 13, ticket 9, charm 7, wrist 1. Seasons 1-7. 2207 glow, 900 remodels (`mid`), 750 `pulsT`, 657 `template`, 591 `assetType`, 141 `frames`, 50 `rAnim`, 23 `rgb`, 22 `hasAnim` (animated GLTF), 21 `env`/`sameEnv`, 13 `shaderNum`, 2 `particle`, 2 `projection` | v7 skins.json |
| Server routes | None specific to the viewer. `viewer.html`, `skins.jspck`, `models/`, `textures/` are plain `fastifyStatic` files from `public/`. The only dynamic route it can hit is `/user-assets/*` | v7 server/index.ts:3815-3830 |

### 2.2 Page modes (dispatch at the bottom of src/viewer.js)

`init()` runs only when `window.dat` exists (`v5632`), which is always the case on viewer.html. `v5633 = !window.dat` is a dead **embedded mode** in this build (`exports.init` is commented out). Embedded mode has no zoom or pan, a translucent shadow disc and a `showShadow` toggle. It also exposes `applyOptions/updatePlayer/updateSize/updateTargetContainer/minimiseCanvas/clearGPUCache/destroy` (v7 src/viewer.js:611-707).

| Mode | Trigger | What it shows | Ref |
|---|---|---|---|
| Showcase | `showcase=<store index>` | One item alone, no GUI. Each item type has its own build. **DYE**: a class-9 preview Player wearing the dye, init(0,-7,0). **SPRAY**: a 9x9 plane plus a `brick_0_3` backing cube 0.1 deep, full material options (frames, movT, rgb, glow, pulsT, rAnim, video, projection). **FACE**: a head `genColorCube(headScale*2)` with the face mesh, no auto-rotate. **HAT** and **HEAD**: a head cube (unless `noHead`) with `hats/hat_<id>`, sitOff/sitOffZ/sclMlt applied. **Everything else** (weapons, remodels, melee, body, back, waist, shoes, pets, wrist, collectibles, charms, tickets): `loadMesh`/`loadAnimMesh` from `store.types[assetType||type]`. Camera at (12,0,0) looking at the item. `OrbitControls.autoRotate = true` (except FACE), driven by a 120 Hz `setInterval` | v7 src/viewer.js:2055-2358 |
| Cosmetic render | `cosmetic=<store index>` | A 512x512 canvas, `PerspectiveCamera(45,1,.1,100)` at x 30. `f447` builds one item. Weapon skins (with remodels and per-weapon/per-melee-id offset and scale fixups) and TICKET/CHARM/PET/COLLECTIBLE/MELEE/WRIST use a mesh holder. HAT/HEAD/FACE/DYE/SHOE use a class-9 preview Player wearing only that slot, posed by `vO203` (slot field, camera x, xDire, y offset, z rotation, light x). GUI has just "Render" (`renderCosmetic`, f463). Skips non-renderable items while `auto` is on | v7 src/viewer.js:630-640, 758-961, 1013-1014, 1969-2018, 2359-2361 |
| No GUI | `nogui` | Player rebuilt (`f458`) and the camera on the weapon (`f452`) | v7 src/viewer.js:2362-2364 |
| Full viewer | default | dat.gui panel (f449), player preview, OrbitControls, drag-and-drop import | v7 src/viewer.js:662-694, 2365-2367 |

### 2.3 URL parameters (every `params.has/get` read)

`params = new URLSearchParams(window.location.search)` (v7 src/viewer.js:45). All index params are `parseInt(x) || default`, so `0` falls back to the default.

| Param | Meaning | Default | Ref |
|---|---|---|---|
| `swap` | `params.has("swap") && params.get("swap")`. `-1` starts on the secondary, `1` on the melee (loose `==`) | false (primary) | :46, 1843-1848 |
| `skinIdP` / `skinIdS` | primary / secondary weapon skin (store index) | -1 | :50-51 |
| `shoe` | shoes index (type 8) **new** | -1 | :52 |
| `face` / `hat` | face (7) / hat (1) | -1 | :53-54 |
| `head` | head (15) **new** | -1 | :55 |
| `body` | body (2) **new** (v3 used type 2 as "Back") | -1 | :56 |
| `back` | back (14). v3's `back` was type 2 | -1 | :57 |
| `waist` | waist (6) | -1 | :58 |
| `pet` | pet (9) **new** | -1 | :59 |
| `wrist` | wrist (11) **new** | -1 | :60 |
| `melee` | melee skin (3) | -1 | :61 |
| `skinCol` | `config.skinColors` id | -1 | :62 |
| `dye` | dye (5) | -1 | :63 |
| `secIndex` | secondary weapon index | 2 | :64 |
| `class` | class index | 0 | :65 |
| `attach` | attachment index | -1 | :66 |
| `hidePlayer` `hideBody` `hideBack` `hideWaist` `hideFace` `hideHat` `hideHead` `hidePet` `hideWrist` `hideArms` `hideLegs` `hideWep` | start with that "Show ..." box unticked (`hideBody`, `hideHead`, `hidePet`, `hideWrist` are **new**) | shown | :92-103 |
| `season` | season filter for every dropdown (0 = All) | `config.currentSeason` (7) | :106 |
| `size` | export PNG edge for the Render buttons | 256 | :191, 233, 260, ... 1993 |
| `anim` | start with Animate ticked **new** | false | :465 |
| `crouch` | start crouched **new** | false | :466 |
| `nobg` | no `#container` gradient background (used by the item-view iframe) | gradient | :467, 556-559 |
| `nogui` | no panel, weapon close-up | panel | :468, 2362 |
| `cosmetic` | cosmetic render page (index) | NaN = off | :475 |
| `showcase` | showcase page (index) | NaN = off | :476 |
| `dev` | dev mode. Lists sort by store index. Adds the Render Face/Hat/Head/Body/Back/Waist/Shoes/Dye/Pet buttons and the live **"<Slot> Config"** editor folders. With a value it also forces highp + antialias + 2x pixel ratio | off | :478, 482 |
| `auto` | with `cosmetic`: render and download every item from the index on | off | :479 |
| `nosup` | keep `skinIdP/S` through the first Weapons rebuild | off | :480, 1249-1253 |
| `noTex` | load models without textures (cosmetic page) | off | :481, 918 |

Non-URL options: `vO201.width/height` can be set through `applyOptions` (embedded mode only).

### 2.4 GUI (dat.gui, width 322; f449/f450/f451)

The page's own DOM is just `#container` and the WebGL canvas the script appends to it (`f438`). On drag-over the container gets `box-shadow: #666666 0 0 100px inset`. Pickers are a transient `<input type=file id="jsonInput">` (f461). Downloads go through a transient `<a download>`. Everything else is dat.gui's generated markup (`.dg.ac > .dg.main`).

Root (non-cosmetic): **Season** (`{All:0, 1..7}`) > folder **Player** > folder **Weapons** > **Animate** > **Crouch** > **Lighting** (`Viewer` / `In-game`) > **Screenshot Gradient** > **Take a Screenshot**. Cosmetic page: only **Render** (v7 src/viewer.js:1009-1038).

Player folder (v7 src/viewer.js:1039-1231), in order:

| Control | Type | Notes |
|---|---|---|
| Show Player | bool | renamed from v3 "Show Model". Moves the camera to the model or to the weapon |
| Show Arms, Show Legs | bool | |
| Class | options | `Game.classes[*].name`. Resets swap and rebuilds Weapons |
| Face / Load Custom Face / [dev] Face Config folder + Render Face / Show Face | options, fn, folder, bool | |
| Hat / Load Custom Hat / [dev] Hat Config + Render Hat / Show Hat | | |
| Head / Load Custom Head / [dev] Head Config + Render Head / Show Head | | **new** |
| Body / Load Custom Body / [dev] Body Config + Render Body / Show Body | | **new**. `showBody` is never applied: the loop uses `vO201.bodyMesh` (undefined), so a body cosmetic mesh is always hidden (v7 bug, :1743-1745) |
| Back / Load Custom Back / [dev] Back Config + Render Back / Show Back | | type 14 (v3: type 2) |
| Waist / Load Custom Waist / [dev] Waist Config + Render Waist / Show Waist | | |
| Shoes / Load Custom Shoes / [dev] Shoes Config + Render Shoes | | **new** (no Show toggle) |
| Skin Color | options | `Default:-1` + `config.skinColors` |
| Dye / [dev] Dye Config (Hair/Skin/Shirt/Sleeves/Waist/Pants/Shoe colour pickers) + Render Dye | | colour editor is **new** |
| Pet / Load Custom Pet / [dev] Render Pet / Show Pet | | **new**. Pet spawned via `Game.pets.add(player, true)`. `showPet` is never applied (no-op toggle) |
| Wrist / Show Wrist | options, bool | **new**, list `f460(11)` |

Weapons folder (v7 src/viewer.js:1232-1344): **Show Weapon**, **Primary Skin** (+[dev] Primary Config), **Attachments** (`None` + `Game.attach`, only if the primary has `attach`), [class has secondary] **Secondary** + **Secondary Skin** (+[dev] Secondary Config), **Melee** (+[dev] Melee Config), [secondary] **Swap Secondary**, **Swap Melee**, **Load Skin**, **Reset Skins** (`RENDER.reset()`).

Dropdown contents (f459 weapon skins, f460 by type, inline for melee). Filter: `seas` matches the Season control (or All) and the item is not `blocked`. Sort: by id, then **dev**: by store index, **else** by rarity, then season, then reversed. Labels: `"<name> [<rarity name> | Free]"` with value `skin.index`. v3 used `findIndex` and had no "Free" label (v7 src/viewer.js:1858-1890).

Dev config editors (f453 cosmetics, f454 weapons, **new**, :1355-1705). They edit the **store skin object in place** (`vA353[type] = Game.store.skins[i]`) and rebuild:
- Cosmetics: `sitOff`, `sitOffZ`, `sclMlt`, Transparency (`trans`), AlphaTest, Frames, Frame Time, Side (Front/Back/Double), Glow, `noEmis`, `sameGlow`, Pulse (`pulsT`); plus `noHead` (head), `noBody` (body), `noLegs` (waist), `randomLeg`/`legLen` (shoes).
- Dye: seven colour pickers.
- Weapons: Transparency, AlphaTest, Side, Glow, Pulse.
- Each editor has **Export Config**: strips store-only keys and template defaults (`store.templates`), then copies unquoted-key JSON to the clipboard.

### 2.5 Scene, camera, lighting, animation

| Feature | v7 behaviour | Ref |
|---|---|---|
| Renderer | `WebGLRenderer({preserveDrawingBuffer, alpha, antialias: dev/cosmetic, precision highp/mediump})`, pixel ratio `dpr*(hi?2:1)`, BasicShadowMap with `autoUpdate=false` | :619-634 |
| Background | `#container` `background:#1b1c1e; background-image: linear-gradient(#7d8fa3,#1b1c1e)` unless `nobg` | :553-560 |
| Camera / controls | `PerspectiveCamera(60, aspect, .01, 50)` at (15,5,0) looking at (0,5,0). three.js OrbitControls with default rotate, **zoom (wheel) and pan** on `#container`, and arrow keys. Re-renders on `change` | :662-672 |
| Lighting | `AmbientLight(0x97A0A8)` always. **In-game**: `DirectionalLight(0xF2F7FC,1.2)` at lightDistance, pitch -0.3π, yaw -0.5π, castShadow. **Viewer**: 6 white SpotLights at ±150 on each axis. Cosmetic/showcase: extra `DirectionalLight(#fff,.5)` at (3,1,0) and shadows off | :615-617, 962-1008 |
| Player build (f458) | Removes the old pet and meshes. `new Player(-1,this,Game)`, `init(...,"preview")`, sets all 13 slot indices, `setClass`, `swapWeapon(0)`, `crouchVal`, `generateMeshes(player,false,true,vA353)` (4th arg truthy, so full third-person meshes), swapSecondary/swapMelee, `updateMshClient`, `updateHeight`, then `Game.pets.add(player,true)` when there is a pet | :1796-1857 |
| Per-frame loop (f456) | Visibility of legs (standing/crouched), arms, back, body, waist, hat, head, face, wrist[0..1], weapon per loadout slot. **Animate**: `playerStep(player, dt*0.015, true)` and `idleAnim += idleAnimS*dt`. **Every frame**: `Game.players.itemEffects(player, scene, dt)`, which spawns the `particle` effects of body/back/dye/shoes cosmetics every 500 ms, and `Game.update(dt, null, player)`, which drives pets, particles and animated materials | :1712-1789; v7 src/data/players.js:533-551 |
| Material effects shown | Everything `RENDER.loadMesh/getMat` supports: glow/sameGlow/emissInt/noEmis, pulsT, frames/frameT, movT/movE/movD, rAnim/rAnimA/rAnimT/rAnimM, rgb/rgbS/rgbT, env/sameEnv (env maps), shaderNum, video textures, projection, wireframe, side, opac/trans/alpha, tileS, `hasAnim` (animated GLTF, `animS`, `inspectAnim`), fileExt obj/gltf/glb/fbx | :910-958, 2087-2338 |

### 2.6 Export and import

| Feature | v7 behaviour | Ref |
|---|---|---|
| Take a Screenshot | canvas `toDataURL`, optionally painted over the gradient, downloaded as `render.png` | :107-128 |
| Render (cosmetic page) | 750 ms after building, crops `550*dpr` squares at a per-weapon (`vO205`, weapons 0-29) or per-type (`vO206`) origin, scales to `size`, downloads `getPreviewExport(skin)` (file name of `skin.thumbnail`). `auto` walks the whole store | :1969-2018 |
| Render Hat/Head/Face/Body/Back/Waist/Shoes/Dye/Pet (dev) | camera (0,10/11/6,-15/-11) looking at (0,6,0), model rotated (`prevRot`, π/6.5, π/4), primary weapon hidden. After 500 ms crops and downloads `<type>_default.png` or the thumbnail name | :217-464 |
| Load Skin / Load Custom <slot> | file picker (1 file for weapons, up to 3 for cosmetics). `.zip` gives a mod pack. `.obj/.fbx/.gltf/.glb` gives `RENDER.updateMesh(key, data, ext)`. Anything else is an image for `RENDER.updateTexture(key, canvas)`. The key is the slot's `store.types[assetType||type]+id[_tex]`, the melee texture, or the weapon skin / remodel texture. Slots: face 7, hat 1, head 15, pet 9, body 2, back 14, waist 6, shoes 8 (`vO204`) | :67-75, 491-502, 1891-1968 |
| Drag and drop | drop on `#container` runs the same import as Load Skin (weapon target) | :673-691 |
| Mod zip | zip.js `ZipReader`. `textures/*.png` go to updateTexture, `models/*.(obj|gltf|glb|fbx)` to updateMesh, then a rebuild after 1 s | :2373-2420 |
| Removed from v3 | root **Config** folder (Face/Hat/Back/Waist Config JSON `prompt` + Reset Config) | v3 src/viewer.js:584-605 |

### 2.7 The v3.1.1 viewer

#### 2.7.1 Summary

- **Page**: `public/viewer.html` **does exist** in the v3.1.1 tree, with the same markup as v7 plus `/libs/zip-ext.js` and root-absolute script paths. It is served statically and bundled from `src/viewer.js` (v3 server/compiler.ts:61). It is opened by `npm run viewer` (`electron . --viewer`, v3 package.json:33; main/main.js:405-406, 499-500, 525-527, 559-562, 800x600) and by the Mods window's "Model Viewer" link (v3 src/index.js:3907). There is **no** item-view iframe and v3 never uses `showcase` itself.
- **Data**: `store.skins` is embedded in `src/data/store.js:418` (1917 items: weapon 1521, hat 184, body 82, melee 48, spray 48, dye 13, waist 11, face 10). `store.types` has 8 entries (store.js:94-102), 7 rarities, `currentSeason = 3`. Models are OBJ only (`OBJLoader`, `GLTFLoader` is imported but unused by the viewer).
- **Modes**: showcase (dye player / 9x7.2 spray plane / generic mesh, holder `rotateX(-π/9)`, **no auto-rotate**, v3 src/viewer.js:1324-1423), cosmetic (weapon skins, melee, `is3D` sprays only, F :348-512), nogui, full.
- **URL params**: `swap skinIdP skinIdS face hat back waist melee skinCol dye secIndex class attach hidePlayer hideBack hideWaist hideFace hideHat hideArms hideLegs hideWep season size cosmetic showcase dev auto nosup noTex nobg nogui` (v3 src/viewer.js:50-111, 339-346, 693, 1181, 1427). `back` means **type 2 (body/body_)**.
- **GUI** (H :559-621, G :622-765, W :766-894): Season. Player: Show Model, Show Arms, Show Legs, Class, Face + Load Custom Face [+dev Render Face], Hat + Load [+Render], Back + Load [+Render], Waist + Load [+Render], Skin Color, Dye [+Render Dye]. No Show Face/Hat/Back/Waist controls (only URL `hide*`). Weapons: same as v7. **Config** folder: Face/Hat/Back/Waist Config (a `prompt("Import Skin Json")` stored into `z[type]`, which v3 `generateMeshes(player,U,V)` ignores, players.js:2123) and Reset Config. Then Animate, Crouch, Lighting, Screenshot Gradient, Take a Screenshot.
- **Loop**: visibility through `lowerBody.children[n]` indices, `playerStep` + `idleAnim` when animating, `RENDER.render(dt,true)` for material animation (:1433-1489).
- **Import**: `.zip` (old zip.js callback API, `textures/` + `models/*.obj`), `.obj`, image. Custom targets: face, hat, back(2), waist (:1023-1136, 1514-1552).

#### 2.7.2 Diff v3.1.1 to v7.1.4

| Area | v3.1.1 | v7.1.4 |
|---|---|---|
| Item types previewable | 8 (weapon, hat, body-as-"Back", melee, spray, dye, waist, face) | 17 (adds shoe, pet, collectible, wrist, charm, ticket, back 14, head 15, player card; body 2 becomes a real "Body") |
| Store size / seasons | 1917 items, seasons 1-3, embedded in store.js | 6448 items, seasons 1-7, `skins.jspck` loaded by sync XHR |
| Player slots in panel | face, hat, back(2), waist, skinCol, dye | face, hat, **head**, **body**, back(14), waist, **shoes**, skinCol, dye, **pet**, **wrist** |
| Show toggles | Model, Arms, Legs, Weapon | Player, Arms, Legs, **Face, Hat, Head, Body (broken), Back, Waist, Pet (no-op), Wrist**, Weapon |
| New URL params | none | `shoe head body pet wrist hideBody hideHead hidePet hideWrist anim crouch` |
| Dropdown order / labels | id, rarity, seas, reversed; `[Rarity]`; value `findIndex` | dev: store index. Else rarity, seas, reversed; `[Rarity|Free]`; value `skin.index`. NFT items removed |
| Config | root Config folder, JSON `prompt` (no effect) | dev-only live per-slot property editors + Export Config to clipboard; root Config folder removed |
| Cosmetic page items | weapons, melee, 3D sprays | + ticket, charm, pet, collectible, wrist, hat, head, face, dye, shoe (preview Player poses `vO203`), larger crop tables (`vO205`/`vO206`), more per-id fixups |
| Dev Render buttons | Face, Hat, Back, Waist, Dye | + Head, Body, Shoes, Pet |
| Showcase | dye / spray plane 9x7.2 / generic; static | + face-on-head cube, hat/head on head cube, spray with brick backing and full material options; **auto-rotate** |
| Showcase consumers | none | store item view iframe `viewer.html?nobg&showcase=<i>` |
| Effects in preview | material animation via `RENDER.render` | + `itemEffects` particles, pets (`Game.pets`), `Game.update` per frame, env maps, shaderNum, video, projection, rgbT, rAnimT/M, emissInt, animated GLTF (`hasAnim`) |
| Model formats | OBJ | OBJ, GLTF, GLB, FBX (import and store) |
| Zip | zip.js callback API + zip-ext.js | zip.js `ZipReader` promises + fflate; models of any of the 4 formats |
| Embedded API | none | `init()` returns applyOptions/updatePlayer/... (dead in this build) |

### 2.8 Krunker Box Office's viewer

#### 2.8.1 What it implements

Krunker Box Office's viewer is a **faithful port of the v3.1.1 viewer**, not v7. The data, GUI and behaviour are v3's, and the code comments quote v3 identifiers (`E`, `Z`, `F`, `G`, `H`, `W`, `U`, `$`, `K`, `J`, `Q`).

| Aspect | Krunker Box Office | Ref |
|---|---|---|
| Launch (native) | `client_bin --viewer [query]` (query optional, without `?`). `app_mode_parse_args` strips `--viewer` from argv; a `viewer.html` link inside the game (Mods window "Model Viewer", `ui/windows/4.html`) is routed through `parse_page_url` to `APP_MODE_VIEWER`. No Makefile or CMake target mentions the viewer | KN client/src/app_mode.c:115-130, 245-275; client/src/main.c:213, 375, 602 |
| Launch (web) | `viewer.html?<query>`: `web_start_page` takes the mode from `location.pathname` and the query from `location.search`. (The `index.html?page=viewer` route in the viewer.c header comment is not implemented in app_mode.c; the comment looks stale) | KN client/src/app_mode.c:318-345; client/src/viewer/viewer.c:3 |
| Page | `ui/viewer.html` has the v3 head/style, `css/dat-gui.css`, `css/viewer-kn.css`, `<canvas id="canvas">` (web GL target), `#container` (pointer-events none), `js/kn-shim.js`, `js/loader.js`. Modal `#knViewerDialog` (+`Text/Input/Ok/Cancel`) replaces `alert`/`prompt` | KN ui/viewer.html; ui/css/viewer-kn.css; client/src/viewer/viewer_import.c:41-117 |
| Page hooks | `viewer_app_page()`: enter/leave/tick/mouse/cursor/scroll/key/resize. Uses a private `Game`/`Map`/`Player`, never the client's game. No game server needed | KN client/src/viewer/viewer.c:675-828; viewer_scene.c:1-17 |
| Data | `viewer_store_table.c`, generated by `tools/ui-import/gen_viewer_store.js` from the **v3.1.1** viewer bundle source map: 1917 skins, 7 rarities, 8 types, 6 skin colours, `currentSeason 3`, 88 pre-sorted option lists (v3 sort order and labels). Also used by `client/src/ui/store_ui.c` (store previews, spin wheel) | KN client/src/viewer/viewer_store.h; viewer_store_table.c:1-2, 1924, 1936, 1940, 1970, 1972, 2154 |
| URL params | v3 set exactly: `skinIdP skinIdS face hat back waist melee skinCol dye secIndex class attach season hidePlayer hideBack hideWaist hideFace hideHat hideArms hideLegs hideWep swap cosmetic showcase dev auto noTex nobg nogui nosup size`, with v3 `parseInt(x)||default` semantics | KN client/src/viewer/viewer.c:5-15, 92-138, 611-663 |
| GUI | C port of dat.gui 0.7 (folders, bool/options/function controllers, listen, remove, the H key to hide, resize). v3 tree: Season, Player (G), Weapons (W), Config (JSON prompt, stored in `g_ui.configs` but unused, as in v3), Animate, Crouch, Lighting, Screenshot Gradient, Take a Screenshot. Cosmetic page: Render | KN client/src/viewer/viewer_gui.h; viewer_gui.c:130-422; viewer.c:465-607 |
| Scene | Z: preview Player through `player_generate_meshes` with 4 cosmetic slots (hat/back/waist/face) + dye + skin colour, weapon skins incl. remodels and glow, melee skins, sight attachment, swap secondary/melee, crouch. F: weapon/melee/3D-spray item. Showcase: dye player / spray plane / generic. Visibility per Show toggle | KN viewer_scene.c:735-829 (`player_cosmetic_extras`, `vscene_rebuild_player`), 845-900 (`player_visibility`), 964-1013 (`vscene_build_cosmetic`), 1015-1091 (`vscene_build_showcase`) |
| Lighting | `U()`: ambient 0x97A0A8 + In-game sun / Viewer 6 spot lights / cosmetic fill light, through a `basic.frag` variant patched at load time | KN viewer_scene.c:30-44, 333-380 (`viewer_program_create`), 402-443 |
| Camera / input | OrbitControls port: rotate, dolly (wheel), pan, arrow keys. The panel hit-test keeps the mouse off the scene. The cosmetic page has no orbit | KN viewer_scene.c:444-590; viewer.c:747-806 |
| Animation | Animate: walk cycle `step_val` + idle anim. Material anims: frames/frameT, movT, pulsT, rAnim | KN viewer_scene.c:170-188 (`anim_add`), 1123-1177 (`anims_tick`, `vscene_tick`) |
| Material options | glow/sameGlow/emiss/noEmis, opac, side 2, alpha, wire, tileS, colour. **No `rgb` cycling** (`VS_RGB` flag is generated but unused), no env maps, shaders, video or projection | KN viewer_scene.c:190-212 (`material_apply_skin`) |
| Background | gradient drawn in GL under the scene unless `nobg` | KN viewer_scene.c:382-400, 1201-1227 |
| Export | Take a Screenshot (`render.png`, optional gradient). Cosmetic Render with v3 crop table and `auto` walk. Dev Render Face/Hat/Back/Waist/Dye. PNG goes to `$XDG_DOWNLOAD_DIR`/`~/Downloads`/cwd natively, a browser download on the web | KN viewer.c:299-458; viewer_scene.c:1229-1279 (`vscene_capture`); viewer_import.c:119-155 |
| Import | `.zip` (own reader over stb inflate: `textures/*.png`, `models/*.obj`), `.obj`, images. Targets: weapon, face, hat, back(2), waist. Native picker via zenity/kdialog, GLFW drop, web `<input type=file>` queue. Test hook `KRUNKNATIVE_VIEWER_IMPORT=<file>[,<target>]`. Overrides cleared by Reset Skins | KN viewer_import.c:1-14, 226-449; viewer_scene.c:1281-1430; viewer.c:707-716 |
| History | viewer files arrived in `ce396af its kruger`. Later touched by `81fa451`, `1dac812`, `476770f` (store previews/spin wheel reuse the viewer store) | `git log -- client/src/viewer ui/viewer.html` |

#### 2.8.2 Gap table vs v7.1.4

Legend: **present** = matches v7; **partial** = the v3 version exists but v7 changed or extended it; **missing** = absent.

| Feature (v7) | KN status | Owning file / function |
|---|---|---|
| `--viewer` launch, 800x600 page window, Mods-window link | present | client/src/app_mode.c `app_mode_parse_args`, `parse_page_url` |
| Item-view iframe `viewer.html?nobg&showcase=<i>` embedded in the store item window | missing (KN has no embedded 3D item view; store_ui.c shows 2D previews) | client/src/ui/store_ui.c; would need an in-page viewer surface from viewer_scene.c |
| v7 skin table (6448 items, 17 types, seasons 1-7, `assetType`, `template`, `free`, NFT flags) | missing (v3 table, 1917 items, 8 types, season 3) | tools/ui-import/gen_viewer_store.js, client/src/viewer/viewer_store_table.c, viewer_store.h `ViewerSkin` |
| `skins.jspck` decode / `store.types` with 17 dirs / `previews.json` enum | missing | viewer_store.h, generator |
| v7 dropdown order and labels (`[Free]`, dev sorts by index, value = `skin.index`) | partial (v3 order and labels) | generator `g_viewer_lists`, viewer_store.c `viewer_option_label` |
| NFT removal (`UTILS.removeNFT`) | missing (no NFT items in the v3 data) | generator |
| Params `shoe head body pet wrist` | missing | viewer.c `state_from_params`, `ViewerState` (viewer_internal.h) |
| Params `hideBody hideHead hidePet hideWrist` | missing | viewer.c `state_from_params` |
| Params `anim`, `crouch` | missing (always start off) | viewer.c `state_from_params` |
| `back` = type 14 vs body type 2 | partial (KN `back` = v3 type 2) | viewer.c `G`, viewer_scene.c `player_cosmetic_extras` |
| Other v3/v7 shared params (`skinIdP/S face hat waist melee skinCol dye secIndex class attach season size cosmetic showcase dev auto nosup noTex nobg nogui swap hidePlayer/Back/Waist/Face/Hat/Arms/Legs/Wep`) | present | viewer.c `state_from_params` |
| Season control (All + 1..current) | partial (1..3) | viewer.c `H` |
| Player folder: Show Player label | partial ("Show Model") | viewer.c `G` |
| Head / Body / Shoes / Pet / Wrist dropdowns + Load Custom Head/Body/Shoes/Pet | missing | viewer.c `G`, viewer_import.c `cosmetic_key` |
| Show Face / Show Hat / Show Back / Show Waist GUI toggles | partial (state and URL params exist, no GUI controllers) | viewer.c `G` |
| Show Head / Show Body / Show Pet / Show Wrist | missing (note: v7 Show Body is broken and Show Pet is a no-op) | viewer.c `G`, viewer_scene.c `player_visibility` |
| Weapons folder (Show Weapon, skins, Attachments, Secondary, Melee, swaps, Load Skin, Reset) | present | viewer.c `W` |
| Root Animate / Crouch / Lighting / Screenshot Gradient / Take a Screenshot | present | viewer.c `H`, `fn_screenshot` |
| Root Config folder (v3 only, removed in v7) | present (v3 extra; would be removed for v7 parity) | viewer.c `H`, `config_prompt` |
| Dev live config editors (sitOff, sitOffZ, sclMlt, trans, alpha, frames, frameT, side, glow, noEmis, sameGlow, pulsT, noHead/noBody/noLegs/randomLeg/legLen; weapon trans/alpha/side/glow/pulsT; dye colour pickers) + Export Config to clipboard | missing (needs number sliders, steps, colour controllers in the dat.gui port and a mutable skin table) | viewer_gui.c (no number or colour controllers), viewer.c, viewer_store_table.c (const) |
| dat.gui number / colour controllers | missing (bool, options, function only) | viewer_gui.h |
| Dev Render Head / Body / Shoes / Pet | missing (Face/Hat/Back/Waist/Dye present) | viewer.c `render_pose`, `pose_capture`; viewer_scene.c `vscene_camera_render_pose` |
| Cosmetic page: weapons, melee | present (v3 crop table weapons 0-19; v7 has 21-29 and more melee-id fixups) | viewer.c `render_weapon_crop`; viewer_scene.c `vscene_build_cosmetic` |
| Cosmetic page: ticket, charm, pet, collectible, wrist, hat, head, face, dye, shoe (`vO203` preview-Player poses, `vO206` crops) | missing | viewer.c `cosmetic_shown`, viewer_scene.c `vscene_build_cosmetic` |
| Showcase: dye player, generic mesh | present (v3 holder rotation `-π/9`, no auto-rotate) | viewer_scene.c `vscene_build_showcase` |
| Showcase: face-on-head, hat/head-on-head (sitOff/sclMlt/noHead), spray with brick backing 9x9 | missing (spray is the v3 9x7.2 plane) | viewer_scene.c `vscene_build_showcase` |
| Showcase auto-rotate (OrbitControls.autoRotate, 120 Hz) | missing | viewer_scene.c orbit (no autoRotate) |
| Cosmetic/showcase fill light + shadows off for showcase | partial (fill light only for cosmetic) | viewer_scene.c `vscene_update_lights` |
| OrbitControls rotate/zoom/pan/keys | present | viewer_scene.c `vscene_orbit_*` |
| Lighting Viewer / In-game | present | viewer_scene.c `vscene_update_lights`, `lights_apply` |
| Animate (walk + idle) / Crouch | present | viewer_scene.c `vscene_tick` |
| Pets (`Game.pets.add`, follow/bob, animated pet meshes) | missing | none (no pet system in shared/ or client/) |
| `itemEffects` particles for body/back/dye/shoes (`particle` field) | missing | none (viewer has no particle system hook) |
| Material: glow, sameGlow, noEmis, opac, side, alpha, wire, tileS, colour, frames, movT, pulsT, rAnim | present | viewer_scene.c `material_apply_skin`, `anim_add`, `anims_tick` |
| Material: `rgb/rgbS/rgbT` colour cycling | missing (even vs v3) | viewer_scene.c (`VS_RGB` unused) |
| Material: `env`/`sameEnv`, `shaderNum`, `video`, `projection`, `emissInt`, `rAnimT/rAnimM`, `movE/movD`, `minGlo` | missing | viewer_scene.c, client/src/materials/basic.c, assets/shaders/basic.frag |
| Model formats GLTF/GLB/FBX + animated GLTF (`hasAnim`, `animS`) | missing (OBJ only via fast_obj; no gltf/fbx loader anywhere in client/) | viewer_scene.c `model_for`, `vscene_update_mesh_obj` |
| Import: zip (textures/png, models/obj), obj, image; drop; picker | present | viewer_import.c `import_zip`, `viewer_import_path`, `viewer_import_pick`, `drop_callback` |
| Import: zip/models in gltf/glb/fbx | missing | viewer_import.c `import_zip_entry` |
| Load Custom targets head 15 / pet 9 / body 2 / back 14 / shoes 8 (`vO204`) | missing (targets 0, 1, 2, 6, 7) | viewer_import.c `cosmetic_key`; viewer.c `fn_custom_*` |
| Screenshot / Render downloads, `size`, `auto` | present | viewer.c `fn_screenshot`, `render_capture`; viewer_import.c `viewer_download_png` |
| Embedded-mode API (applyOptions, updatePlayer, minimiseCanvas, shadow disc) | missing (dead code in v7 too) | n/a |

#### 2.8.3 Dependencies on other work

1. **v7 item data**: a v7 store table (skins.json/skins.jspck, 17 types, `assetType`, `template`, `free`, seasons 1-7, `store.previews`, `rarities` incl. NFT, `config.skinColors`). Everything else depends on it. It overlaps with the store/inventory survey sections, because `store_ui.c` and `cosmetics_table.c` share the v3 table today. KN's `ViewerSkin.type` comment and the per-type `g_viewer_types` assume 8 types.
2. **Player cosmetic slots**: KN `PlayerCosmetics` (shared/include/shared.h:509-516) and `PLAYER_COSMETIC_SLOTS` (client/include/client.h:327) have only hat/back/waist/face/dye/colours. v7 previews need **head, body, shoes, wrist, pet** slots and their `playerMesh.js` attachment (shoes per leg, wrist on arms, head replaces the head cube with `noHead`, body with `noBody`, waist with `noLegs`, `randomLeg`/`legLen`). This is the same work the in-game v7 player mesh needs, so it should land in `client/src/player_mesh.c` rather than in the viewer.
3. **Pets** (`data/pets.js`, `Game.pets.add/remove`, preview flag, speed/scale/bob) and **item particle effects** (`players.itemEffects` + `libs/particles.js` `effect`): KN has neither.
4. **Model formats**: v7 assets use GLTF/GLB/FBX (pets, `hasAnim` items, `loadAnimMesh` defaults to gltf). KN has OBJ only. A GLTF loader (and skeletal or node animation for `hasAnim`) is a renderer-level dependency.
5. **Material features**: env maps (`env`), `shaderNum` custom shaders, video textures, `projection`, `rgb` cycling, `rAnimT/M`, `emissInt`. Shared with in-game weapon and cosmetic rendering (`client/src/materials/basic.c`, `basic.frag`).
6. **Assets**: KN `assets/models` lacks the v7 dirs `back/` (v7 171 models), `pets/` (17), `wrist/` (6), `charms/` (39), `collectibles/` (25), `tickets/`, `head/`. It has only 3 shoe, 199 hat, 102 body, 15 waist and 26 melee models against v7's 65/510/305/92/165. KN does have some v3-era textures under `assets/textures/{pets,charms,collectibles,tickets,back,shoes}`. Fetching the v7 asset set is a prerequisite (also for the web build's on-demand `ensure_asset_file`).
7. **Classes/weapons**: v7 dropdowns iterate `Game.classes`, `Game.weapons` (secondaries), `Game.attach`, and the cosmetic poses use class 9. Weapon remodel ids up to 29 need the v7 weapon list (other sections).
8. **dat.gui port**: the v7 dev editors need number (slider/step/min/max) and colour controllers plus `addColor` in `client/src/viewer/viewer_gui.c`. A clipboard write is also needed for Export Config.
9. **Store item view**: v7's item window embeds the showcase page in an iframe. KN would need either an embedded viewer surface in the game page or a separate scene pass, which depends on the store/inventory UI work.

## 3. Gap analysis against Krunker Box Office (consolidated)

The per-item tables live in each section: 1.1.6 (movement), 1.2.6 (combat), 1.3.1
and 1.4.6 (cosmetics and account), 1.5 and 1.5.9 (controls and HUD), 1.6 (effects),
1.7 (settings, every key) and 2.8.2 (viewer). This section rolls them up by area, gives
the KN owner and the work package (section 4).

Overall: KN is a faithful **v3.1.1** port across the board. Almost every v7 item is
*partial* (the v3 version exists) or *missing*. A few places are already v7-like by
accident: noclip x2 speed, crates/barrels/planes wall-jumpable, and player height updated
in third person (1.1.6).

| Area | Item | KN status | Owning KN file(s) | WP |
|---|---|---|---|---|
| Movement | Core integrator and constants | present (v3 = v7 defaults) | `shared/src/player.c` `player_proc_input`, `shared/src/constants.c` | A |
| Movement | Delta floor 0.1 ms | missing | `player.c:1015` | A |
| Movement | Wall-jump cap `wallJM`, per-class wall values | missing | `player.c:1430-1460`; `ClassConfig` `shared/include/shared.h:327-339` | A (+B schema) |
| Movement | Slide trigger (airborne landing), 100 ms re-slide lock, `slidSpd`, `noSliding`, always-look-direction slide, `frSlid`/`bkSlid` | missing / partial (`slid_cont` both branches) | `player_reset_step` `player.c:529-553`, `player.c:1196-1215` | A |
| Movement | Per-class movement config (`slT`, `slA`, `grA`, `arA`, `ldA`, `jpC`, `infJ`, `disJump`, ...) | missing (`MapConfig` defaults only) | `shared/src/classes.c`, `shared/src/map.c:15-27` | A + B |
| Movement | Ladders (no bottom clamp, rope ladders), jump pads (map-sized, 6 directions, `bm 0`, `wasOnPad`), border 500, cylinder tweaks, head-clip fix | missing | `player.c:580-840`, `map.c:468-600`, `constants.c:17` | A |
| Movement | Air strafe x1.0 + diagonals | missing (x0.73) | `player.c:1251-1259` | A |
| Movement | Grapple, movement schemes, triangle collision, player collision, fall damage, fixMov | missing | new code in `shared/src/` | A (phase 2) |
| Input | v7 input fields (reset, interact), held jump, `netRate` sampling | missing (v3 frame, one input per render frame) | `Input` `shared/include/shared.h:357-399`, `client/src/net/recon.c`, `client/src/ui/ui_controls.c:1111` | C |
| Tests | Movement recordings | v3 only (all must be replaced) | `shared/tests/inputs/`, `shared/tests/proc_input_test.c`, `tools/kref/movement-synth.js` | A |
| Classes | v7 stats (5 classes), Rocketeer secondary, Survivor and Infiltrator (ids 14/15) | missing | `shared/src/classes.c`, `g_classes[14]` | B |
| Weapons | v7 stats on ids 0-18, weapons 19-28, per-weapon `lsMlt`, crouch leg zone | missing | `shared/src/weapons/*.c`, `shared/include/weapons.h` (`WEAPON_COUNT 19`), `player.c:2253-2258` | B |
| Weapons | Charge system (crossbow, knife throw, Zapper, Charge Rifle) | missing | `player.c`, `shared/src/projectile.c` | B |
| Weapons | v3 bugs: `noHeadShot` ignored, `movSpread` not modelled | bug | `player.c:2258`, `player.c:1359` | B |
| Hit reg | Hitbox pad 0.6 / 0.1 comp + class `hbS/hbH`; `headMlt`/`adjustedHeight` | missing (v3 pad 1) | `player.c:1793-1809`, `projectile.c:193`, `shared/include/modes.h:164` | B |
| Attachments | Selection wiring | partial (menu offers it, render uses the default sight) | `client/src/player_mesh.c:753,925`, `client/src/ui/ui_loadout.c` | B |
| Attachments | Acog Scope (zoom x1.35), `aimRecticles` scope picker | missing | `shared/src/weapons/weapons.c`, `client/src/camera_feel.c:66-72`, `client/src/ui/hud_ui.c` | B (data) + D (picker settings) |
| Streaks | Nuke trigger (detonation exists, nothing starts it); 3-slot streak loadout, Airdrop/Slimer/War Machine/Juggernaut | partial / missing | `shared/src/game.c`, `player.c` | B |
| Effects | 10 status effects, perks, perk machines, 24 new pickups, upgradeWeapons/krumMachine | missing | new `shared/src/effects.c`, `map.c` objects | B |
| Cosmetics | v7 skin table (6448, 17 types, `assetType`, templates, tags, NFT) | missing (v3 1917 / 8 types) | `tools/ui-import/gen_viewer_store.js`, `client/src/viewer/viewer_store_table.c`, `client/src/cosmetics_table.c`, `assets/skins.json` | F |
| Cosmetics | New slots head, back (split), shoe, pet, wrist, charm, player card; premium hat+head / body+back rule | missing | `client/include/cosmetics.h`, `client/src/player_mesh.c`, `client/include/client.h:327`; packet `PlayerCosmetics` `shared/include/shared.h:507` | F (+C packet) |
| Cosmetics | Runtime skin animation in game (pulse, frames, movT, rAnim, rgb), cosmetic glow | missing in game (viewer only) | `client/src/player_mesh.c`, `client/src/materials/basic.c`, `assets/shaders/basic.frag` | F |
| Cosmetics | env maps, `shaderNum` (8), video, projection, GLTF/animated models, skin sounds, item particles | missing | `basic.c`/`basic.frag`, new GLTF loader, `client/src/vfx.c`, `client/src/audio.c` | F |
| Cosmetics | Sprays from the store, 1/2/3 active, spray wheel | partial (v3 sprays) | `client/src/sprays.c` | E |
| Player mesh | v7 `playerMesh.js` (separate head, `legLen`, `noSleeve`, wrist marker, `armS`, hold offsets) | partial (v3 shape, head already split) | `client/src/player_mesh.c` | F |
| Pets | follow AI, lifecycle, class-preview pet | missing | new `client/src/pets.c` | F |
| Badges | 29 badges, nametag/leaderboard display, privacy toggles | missing (v3 icon badges only) | `client/src/nametags.c`, `services/brain/accounts.c` | E |
| Medals | Kill-medal popups | present | `client/src/ui/hud_popups.c` | - |
| Medals | v7 icon medals, per-account counts, end-screen list, 7 medal settings | missing | `hud_popups.c`, `server/src/accounts.c` | E |
| Challenges | v7 data-driven challenges; the v3 window hangs on "Loading" | partial / bug | `client/src/ui/account_ui.c:1171-1176`, `services/brain/accounts.c` | E |
| Account | Level progress curve, junk, Battle Pass, roles, clan colours, backgrounds, per-stat commit keys | partial (v3 set) | `account_ui.c`, `nametags.c:163-186`, `services/`, `server/src/accounts.c:439-461` | E |
| Controls | Alt keys, rebindable LMB/RMB (`shoot1Key`/`aim1Key`), ~30 new actions, 3 streak slots, 5-tab window, wheel throttle | partial (38 v3 ids) | `client/src/ui/ui_controls.c` | D |
| Controls | Per-weapon toggle-ADS and sensitivity, hold/toggle triggers | missing | `ui_controls.c`, `ui_settings.c` | D |
| Controls | Gamepad 8-way stick, rumble multiplier, `disableCons`; mobile melee button, loadout-aware layout | partial / missing | `client/src/gamepad.c`, `client/src/ui/ui_mobile.c` | D |
| HUD | v7 HUD DOM (player card, Tab table, K/D, TPS, rounds, perks), XP bar removal | missing (v3 HUD) | `ui/index.html`, `client/src/ui/hud.c`, `hud_ui.c` | E |
| HUD | Minimap, death markers, spray/prop wheels | missing | new `client/src/ui/minimap.c` (or `hud.c`) | E |
| Death | Head-to-head death screen, per-life stats, `kCams` toggle | missing (v3 kill card) | `hud_ui.c:1825`, `ui_loadout.c:112` | E (+C packet "3") |
| Spectate | Rebindable keys, focus player, forced nametags, `#specControlHolder`; objective cams (missing even against v3) | partial | `client/src/ui/hud_spectate.c`, `client/src/spectate.c` | E |
| Social | Team/global chat and voice channels, proximity voice, per-player mute, mic quality; prop taunts, quick chat | missing | `client/src/voice.c`, `shared/src/voice.c`, chat UI in `hud_ui.c` | E (+C packets) |
| Settings | 287 v7 keys: 120 present, 40 need a v7 change, 28 partial, 94 missing, 5 n/a; per-weapon storage; basic/advanced view; 7 removed keys (2 need migration) | see 1.7 | `client/src/ui/ui_settings.c`, `ui_render_settings.c`, `ui_client_settings.c`, `ui/windows/settings-tab-*.html` | D |
| ENT packet | `challMode`, `anonMode` stored but never sent; 4 badge flags; `slidCont`/`servSendR` dropped | partial | `client/src/main.c` (ENT), `shared/include/shared.h` | C |
| Viewer | v3 viewer complete (params, GUI, import, export) | present (v3) | `client/src/viewer/*` | - |
| Viewer | v7 params (`shoe head body pet wrist hide* anim crouch`), slots, Season 1-7, dev config editors + Export Config, dat.gui number/colour controllers, showcase auto-rotate, head/hat/face showcase, store item iframe | missing | `client/src/viewer/viewer.c`, `viewer_gui.c`, `viewer_scene.c`, `client/src/ui/store_ui.c` | F |
| Viewer | `rgb` colour cycling (missing even against v3), GLTF/FBX import, v7 model sets | missing | `viewer_scene.c`, `viewer_import.c`, `assets/models/` | F |

## 4. Implementation plan

Six work packages (WP-A to WP-F) that parallel agents can run. Policy: adopt v7.1.4
in place, with no v3 option (header). Each WP lists its owned files, its dependencies,
its tests, and the [GAMEPLAY] items that break existing tests.

### 4.0 Ground rules for all WPs

- **Hot-file partition.** Agents may edit a hot file only in their own region:
  - `shared/src/player.c`: WP-A owns the movement functions (`player_proc_input`
    integration, `player_reset_step`, `player_do_map_collisions`, `player_jump`, wall
    jump, ladder, jump pad, air strafe). WP-B owns the combat functions (shooting, hit
    tests, damage, hitbox pad, reload/fire timers, charge, streaks). Each adds new
    functions in its own region.
  - `shared/include/shared.h`: WP-C owns the wire structs (`Input`, `PlayerCosmetics`,
    ENT/spawn/kill/chat/voice/spray packets, `NET_PACKET_*`). WP-B owns `ClassConfig`
    (and `weapons.h`). WP-A owns the movement fields of `GameConfig`/`MapConfig`.
  - Settings rows (`ui_settings.c`, `ui_render_settings.c`, `ui/windows/settings-*`):
    WP-D owns every row and default. Feature WPs read values through the existing
    getters and ask WP-D for new keys (1.7 lists which keys belong to which feature).
  - `ui/index.html` and the HUD CSS: WP-E. `ui/windows/*`: WP-D (settings, controls)
    and WP-F (store/item windows).
- **Reference data dumps.** WP-B delivers `tools/kref/v7-dump.js` first (day 1): Node
  with the polyfill stub from the v7 AGENTS.md. It writes JSON fixtures of v7
  `weapons.js`, `classes.js`, `upgradeWeapons.js`, `streaks.js`, `effects.js`,
  `settings.js` (key/default/min/max/step/options), `controls.js` binds and
  `itemTypes.js` into `shared/tests/data/v7/` (settings and binds into
  `client/tests/data/v7/` or similar). Every WP's table test compares C tables against
  these fixtures.
- **Reference renders.** UI and viewer parity needs v7 captures in
  `doc/ref/krunker-v7.1.4/`, taken with the existing `tools/kref` CDP tooling against
  the v7 server (`PORT=6781 npm run server` in the v7 tree; never modify that tree).
  WP-E owns the in-game and menu captures and WP-F the viewer captures.
- Verification follows AGENTS.md: `make`, `make test`, `make check`, and for UI work
  `make smoke-native`, `make uidiff UIDIFF_HEADLESS=1`, plus headless screenshots of
  Sandstorm and Kanji for renderer work.

### WP-A: Movement and physics, plus v7 recordings (shared)

- **Owns:** the movement region of `shared/src/player.c`, `shared/src/constants.c`,
  the movement and jump-pad/ladder/wall-jump/border parts of `shared/src/map.c`, the
  movement fields of `GameConfig`/`MapConfig`, new `shared/src/movement.c` (the
  movement-scheme table, phase 2), `tools/kref/movement-synth-v7.js` (new),
  `shared/tests/inputs/v7_*.json` (new), `shared/tests/proc_input_test.c`.
- **Scope (1.1.7 items 1-20):**
  - **Phase 1** (default-gameplay changes): delta floor; `wallJM` caps; slide trigger,
    re-slide lock, `slidSpd`/`noSliding`; always-look slide (drop `slid_cont`); air
    strafe; ladder clamp; jump pads; border 500; wall-jumpable set; cylinder tweaks; jump
    and accel modifier hooks (they read `attributes`/perks from WP-B, default 1).
  - **Phase 2** (off by default): fixMov, headClipFix, fall damage, player collision,
    grapple physics (weapon 20 data from WP-B), movement schemes, triangle collision.
- **Depends on:** WP-B's `ClassConfig` v7 schema and class table (speed, `wallJM`,
  `slT`, `slA`, movement knobs) for the class-dependent scenarios. Everything else in
  phase 1 can start at once. WP-C for held-jump/`netRate` input semantics; the physics
  itself does not wait on the wire.
- **Tests:**
  - Re-record every scenario in the 1.1.8 table at the listed deltas (7/17/33 ms) with
    `movement-synth-v7.js` (recipe in 1.1.8: v7 server on 6781, Chromium CDP 7781,
    `custMap` = KN's own map JSON, class 0). Store them as `shared/tests/inputs/v7_<scenario>_<fps>fps.json`.
  - Delete the `v3_*` files and the live v3 recordings (`collide`, `crouch`, `jump`,
    `scope`, `slide`, `spawn`, `walk_*`, `wall_jump`, `kanji_jump_pad`) once their v7
    synth equivalents pass.
  - Add the ~19 new v7 scenarios (`wall_jump_limit`, `ramp_crouch_descend`,
    `slide_relock`, `jump_pad_dir`, `border_jump`, `delta_zero`, `fall_damage`, ...).
  - Extend the state row with `wallJumpC`, `canSlideT`, `onPad` so `proc_input_test`
    checks them.
  - `make test` on Sandstorm, Kanji and Soul Sanctum.
- **Breaks:** all existing movement recordings (expected; replaced).
  `class_spawn_test` if it asserts class speeds.

### WP-B: Classes, weapons, combat, effects (shared)

- **Owns:** `shared/src/classes.c`, `ClassConfig` in `shared/include/shared.h`,
  `shared/include/weapons.h`, `shared/src/weapons/*.c` (plus new files for ids 19-28),
  `shared/src/projectile.c`, the combat region of `shared/src/player.c`, the
  streak/nuke/airdrop code in `shared/src/game.c`, new `shared/src/effects.c` (status
  effects, perks, 1.6), krumMachine/perkMachine/pickup objects (in their own
  `map.c` parse function), `client/src/projectiles_render.c`, the attachment/Acog
  parts of `client/src/player_mesh.c` and `client/src/camera_feel.c`, and
  `tools/kref/v7-dump.js`.
- **Scope (1.2, 1.6):**
  - **Day 1:** the `ClassConfig` v7 schema and the v7 class table (unblocks WP-A).
  - v7 stats for weapons 0-18; per-weapon `lsMlt` and crouch leg zone; hitbox pad
    0.6/0.1 and `hbS/hbH`; `headMlt`/`adjustedHeight` in the hit AABB (field from WP-C).
  - The charge system; Survivor and Infiltrator; weapons 19-28.
  - Acog Scope; killstreak slots and the new streaks; upgradeWeapons/krumMachine;
    effects/perks/pickups.
  - Fix `noHeadShot` and `movSpread`.
  - Guest gating (`minRec`/`req`): see 5.1 item 1.
- **Depends on:** WP-C for packet fields (charge state in the input or weapon state,
  streak slot selection, `headMlt` in spawn, kill packet). WP-D for the streak-slot
  binds and the Acog/scope picker settings rows.
- **Tests:**
  - New `shared/tests/weapons_v7_test.c`: every class and weapon field against the
    `v7-dump.js` fixtures. Times converted ms -> s, as KN already does.
  - `class_spawn_test` (ids 14/15, health, secondaries).
  - `lagcomp_test` (hit/miss at the new 0.6 pad, leg-zone and headshot lines).
  - New `charge_test` (crossbow 20 hip / 200 at 1.3 s, knife throw 180) and
    `streak_test` (default Nuke-only pick, slot selection).
  - `match_test`/`modes_test` expectations (rocket kill score 50 -> 75).
  - `make test`.
- **Breaks:** `lagcomp_test` and any hard-coded damage/health expectations; v3 movement
  recordings with class speed (handled by WP-A).

### WP-C: Protocol, input and server plumbing

- **Owns:** the wire structs in `shared/include/shared.h`, `shared/src/net.c`,
  `shared/src/net_channel.c`, `shared/include/serde.h`/serde code, `client/src/net/*`
  (notably `recon.c`, `events.c`, `game_events.c`), `server/src/net.c`,
  `server/src/server_core.c`, and the ENT build in `client/src/main.c`.
- **Scope (KN keeps its own binary wire, but must carry every v7 field):**
  - Input: `reset` (0/1/2), `interact`, held jump, `netRate` sampling decoupled from
    the render rate. KN already has exact lag compensation from the shooter's input
    clock, so v7's fields 15/16 map to it.
  - ENT/enter: `challMode`, `anonMode`, the 4 badge flags, `petIndex` and the new slot
    indices; drop `slidCont` and `servSendR`.
  - Spawn packet: `PlayerCosmetics` + shoe, pet, wrist, head, back, charms[2],
    playerCard, badgeIndex, headMlt (v7 `players.js:5002`); the re-dress broadcast `rg`.
  - Kill packet "3" with the death-stats array.
  - Chat `ct`/`ch` with a channel and account id; voice `vc` with
    channel/volume/quality and proximity.
  - Sprays `sp` (multi ids + skin id) and `spry`.
  - Medals `am`.
  - Action packets: `intSec`, `drpw`, `cnfm`, `k0..k2`, `prp*`, `prpt`.
  - Challenge packets `uchp`/`chg`/`chgC`/`chlR`.
- **Depends on:** nothing to start. It is the hub: land struct changes early as small
  commits with defaults, so WP-B, WP-E and WP-F can fill them in.
- **Tests:**
  - `knet_test`, `recon_test` (`netRate` 144 sampling at 30/60/144/240 render fps,
    held jump, reset/interact).
  - A new serde round-trip test per changed packet.
  - `make test-net` (2-client UDP end-to-end); `make check` (bridge typecheck,
    since net-bridge passes frames through); `mm-parity` scripts if the matchmaker
    payload changes.
- **Breaks:** cross-version play between old and new KN builds (expected with in-place
  adoption; bump the protocol version).

### WP-D: Settings and controls (client)

- **Owns:** `client/src/ui/ui_settings.c`, `ui_render_settings.c`,
  `ui_client_settings.c`, `ui_controls.c`, `ui_mod_keys.c`, `ui_bhop.c`,
  `ui_mobile.c`, `client/src/gamepad.c`, `client/include/client_settings.h`,
  `client/include/render_settings.h`, the `ui/windows/settings-tab-*.html` and controls
  window templates, and the v7 mode of `tools/ui-import` for those windows.
- **Scope (1.7, 1.5.1-1.5.5):**
  - All 287 v7 keys with v7 defaults/ranges/steps/options.
  - Per-weapon storage for the 21 `perWep`/`perWepG` keys, behind `gpSetts`/`vmSetts`.
  - `toggleADS`, `netRate` (UI; the sampling is WP-C), the basic/advanced view
    (`sType`), and removal of the 7 v3 keys, with migration for `customScope` ->
    `krk_custScps` and `customADSDot` -> the reticle picker.
  - Wire the no-op keys to their consumers. Most consumers already exist: `leftHand`,
    `bulletCasings`, `impactHoles`, `showExplo`, `trailCol`, `tracerOffset`, `softShad`,
    `chatFilter`, ...
  - Bind table: `ControlBind` with alt keys, rebindable LMB/RMB (`shoot1Key` 10001 alt
    220, `aim1Key` 10003 alt 221), ~30 new actions, 3 streak slots, the 5-tab controls
    window, the 50 ms wheel throttle, and `updateKeybindUI` labels.
  - Gamepad 8-way stick with radial deadzone, `vibrationMlt`, `disableCons`, sensitivity
    defaults 4/3; mobile `mobileKSwap` and loadout-aware buttons.
  - Feature-gated rows (minimap, death markers, medals, voice, pets, charge bar,
    grapple rope) land as storage plus UI rows. Their consumers come from WP-E/B/F.
- **Depends on:** WP-C for the reset/interact input fields; WP-E's v7 `ui/` import if
  it re-imports from v7 `public/` (5.1 item 2).
- **Tests:**
  - New `settings_v7_test`: KN's table of keys, defaults and ranges against the
    `v7-dump.js` settings fixture. Assert the counts: 287 keys, no v3-only key left.
  - The same for binds (ids, defaults, alt defaults).
  - `make uidiff` for the hub-settings and controls scenarios against the new v7
    captures; `make smoke-native`; `make web smoke-web` if `web_dom.c`/`ui/js` change.
- **Breaks:** saved user settings (`fov` 90 -> 100 and so on). Add a one-time migration
  that keeps user-changed values and adopts the v7 defaults for untouched keys.

### WP-E: HUD, death screen, spectate, social, progression (client + backend)

- **Owns:**
  - `ui/index.html` and the HUD CSS.
  - `client/src/ui/hud.c`, `hud_ui.c`, `hud_popups.c`, `hud_modes.c`, `hud_spectate.c`.
  - `client/src/spectate.c`, `client/src/nametags.c`, `client/src/sprays.c`,
    `client/src/voice.c`, `client/src/ui/account_ui.c`, a new minimap/death-marker
    module.
  - `server/src/accounts.c` (per-stat commit keys).
  - `services/brain` and `services/db/migrations` for badges, v7 challenges, medal
    counts, junk and Battle Pass.
  - The in-game/menu capture scripts for `doc/ref/krunker-v7.1.4/`.
- **Scope (1.4.2-1.4.5, 1.5.6-1.5.9):**
  - The v7 HUD DOM and logic: player card, Tab table scoreboard, K/D, TPS, rounds,
    perks; remove the XP bar.
  - Minimap; death markers (decide on the v7 one-frame bug, 5.1 item 3); icon medals and the
    v7 medal settings.
  - The head-to-head death screen with per-life stats, the player card banner and
    `kCams`.
  - Spectator focus, forced nametags, `#specControlHolder`, objective cams.
  - Chat/voice channels, proximity voice, per-player mute; multi-spray, spray wheel,
    store sprays; prop taunts and quick chat.
  - Badges on nametags/leaderboards; v7 challenges (and fix the hung `chg`); level
    progress curve; clan colour removal.
- **Depends on:** WP-C packets (kill "3", `ct`/`ch`/`vc`, `sp`/`spry`, `am`, challenge
  packets); WP-D bind ids and settings rows; WP-F for the player-card and badge assets.
- **Tests:**
  - `make uidiff UIDIFF_HEADLESS=1` with the `ingame-*`, `win-challenges` and
    `menu-*` scenarios re-baselined against v7 captures, plus new scenarios
    (`ingame-death-v7`, `ingame-spectate`, `ingame-minimap`).
  - `make services-test` for the badge/challenge procs.
  - `make test-net` for chat/voice channels.
  - `make smoke-native` / `make web smoke-web`.
- **Breaks:** every existing uidiff baseline that shows the HUD. They are regenerated
  from v7 references; the v3 baselines stay on the `v3.1.1` tag.
- **Part 2c (landed):** badges on nametags and the menu class tag (privacy settings), the badge
  collection popup, select / claim and the auto-awarded Nuclear badges; v7 challenges (window 33
  with Krunker Box Office's Daily / Weekly sections, map / weapon / nightmare categories, the end
  screen's completed cards and reward skins); v7's getLevelProg; v7's getClanCol (no rank /
  challenge colours); per-match medal counts (`NET_PACKET_END_MEDALS` 243, the end screen's
  Medals tab) and per-account medal counts (profile "Medals" stat tab). The game server tallies
  v7's incStat / challenge events (`GameHooks.progress`, `server/src/accounts.c`) into the stats
  commit; kn_brain evaluates the challenges at the commit (`shared/src/progression.c`,
  `services/brain/db_progress.c`, migration `0107_progression`), so a challenge completes at
  the end of the match (no in-match "Finish the game to complete" line). Not ported: the KPD
  review flow (no KPD system), item / store-item challenge contexts, `uwk` and `kjg` / `jgk` /
  `crc` events, the season badges (Battle Pass rules).

### WP-F: Cosmetics, player mesh, skin effects, pets, viewer (client)

- **Owns:**
  - `client/src/player_mesh.c` (except the attachment/Acog part, WP-B),
    `client/src/cosmetics.c`, `client/src/cosmetics_table.c`,
    `client/include/cosmetics.h`, `client/src/class_preview.c`.
  - `client/src/materials/basic.c` and `assets/shaders/basic.{vert,frag}` (skin-effect
    uniforms). Keep them valid GLSL ES 3.00 and coordinate with any shadow work.
  - A new GLTF loader (`client/src/gltf.c`), new `client/src/pets.c`, the item
    particles in `client/src/vfx.c`.
  - `client/src/viewer/*`, `ui/viewer.html`, `tools/ui-import/gen_viewer_store.js`
    plus the v7 asset fetch (`tools/ui-import/game_assets.py` v7 mode), and the store
    item 3D showcase in `client/src/ui/store_ui.c`.
- **Scope (1.3, 1.4.1, 2):**
  - The v7 skin table (6448, 17 types, `assetType`, templates, tags, NFT removal); the
    first 1914 indices stay stable, and KN inventory ids remain valid but 185 items
    change slot.
  - New slots and their mesh attachment per `playerMesh.js`; the premium hat+head /
    body+back rule.
  - Runtime skin animation in game (pulse, frames, movT, rAnim, rgb) and cosmetic glow;
    env maps, `shaderNum` 0-7, projection, video (optional), animated GLTF, skin sounds,
    item particles.
  - Pets (follow AI from `v7 src/data/pets.js:3-97`).
  - Viewer: v7 params, the Head/Body/Shoes/Pet/Wrist folders and Load Custom targets,
    Season 1-7, dat.gui number/colour controllers, dev config editors + Export Config,
    showcase types and auto-rotate, the cosmetic render page for all types, GLTF import,
    and the embedded store item view.
- **Depends on:** WP-C (`PlayerCosmetics`/spawn fields, `rg`); WP-B (weapons 19-28 for
  remodels and charms, class 15 face); WP-D (pet/skin-sound settings rows); 5.1 item 4
  (v7 assets and the GLTF dependency).
- **Tests:**
  - A generator test (6448 items, 17 types, index stability for 0-1913, the
    `assetType` remap).
  - Viewer render scenarios: `client_bin --viewer "<params>"` headless with
    `KRUNKNATIVE_SCREENSHOT`, diffed against v7 `viewer.html?<params>` captures from a
    new `tools/kref/viewer-ref.js` for showcase hat/head/face/dye/spray/pet and a
    player with all slots.
  - A GLTF loader test on a v7 pet model.
  - `make uidiff` for the store/inventory window scenarios; headless Sandstorm/Kanji
    screenshots with dressed bots (player mesh in game).
- **Breaks:** viewer and store baselines (re-baselined against v7).

### 4.1 Order and critical path

1. **Day 1, unblocking:** WP-B `v7-dump.js` and the `ClassConfig` schema; WP-C struct
   skeletons (new fields with defaults, protocol version bump); WP-E/WP-F v7 reference
   captures; WP-A recording tool (`movement-synth-v7.js`) and the first v7 recordings
   of the existing scenarios.
2. **Parallel phase:** all six WPs in their owned files.
3. **Integration:** WP-A phase 2 (grapple needs WP-B weapon 20); WP-E death screen and
   chat (needs WP-C packets); WP-F store showcase (needs the WP-E/WP-D window import).
4. Done when `make check`, `make test` (v7 recordings only), `make uidiff` (v7
   baselines) and `make smoke` all pass, and headless Sandstorm/Kanji screenshots show
   the v7 HUD and dressed players.

## 5. Appendix

### 5.1 Open questions for the user (via the coordinator)

1. **Guest/offline class and weapon gating.** v7 locks Hunter, Vince, Detective and
   Bowman (`minRec 1` + `req`), the Nuke (`reqAccount`) and the higher-rank secondaries
   for players without an account. KN offline play has no account. Recommendation:
   treat the local/offline player as "has account", as v7 itself waives `req` in
   sandbox and custom games.
2. **Re-import `ui/` from v7 `public/`.** The v7 HUD, settings and controls windows are
   large DOM changes. The recommendation is a v7 mode of `tools/ui-import`
   (index.html, CSS, window templates), with `ui/README.md` provenance updated, rather
   than hand-porting.
3. **v7 reference bugs: replicate or fix?** Found in v7 (details in the sections):
   - Viewer "Show Body" never applies (`v7 src/viewer.js:1744`) and "Show Pet" is a
     no-op.
   - Death markers last about one frame (seconds subtracted as ms).
   - The spectator minimap key toggles a setting that does not exist.
   - The voice mute list is saved under the chat mute key.

   Recommendation: fix them and document each deviation in code (AGENTS.md allows
   intentional, documented differences).

   Decided (WP-E part 2a, fixed and documented in `client/src/ui/hud_map.c` and
   `client/src/spectate.c`):
   - Death markers: `deathMarkerDur` is read as seconds ("Death Marker Duration (s)"): a
     marker lasts `deathMarkerDur * 1000` ms of overlay time, v7's 7 s default included.
     Other players' markers use `img/death_0_1.png` too: v7's `death_2_1.png` is not in the
     v7.1.4 public tree, so v7 never draws them.
   - `specMiniMap` hides / shows the minimap while spectating (a session toggle like
     `showSpectatorNameTags`) instead of setting the missing `specMiniMap` setting.
   - Related, also fixed in part 2a: v7's `io_addMedal` queues a repeated medal with count 1
     whatever count it came with; Krunker Box Office keeps the count (`client/src/ui/hud_popups.c`).
4. **GLTF dependency and v7 assets.** 22 animated items and the pets are GLTF/GLB/FBX.
   Recommendation: adopt `cgltf` (single header, MIT) for GLTF/GLB only, and skip FBX
   unless an item needs it. The v7 model set (hats 510, body 305, back 171, waist 92,
   shoes 65, pets 17, charms 39, ...) must be fetched into `assets/`, which affects
   repo/web asset size.
5. **KrunkScript.** v7 movement and input hooks (`onKeyPress`, movement scheme hooks)
   only matter for custom maps with scripts. Recommendation: out of scope for this
   upgrade.
6. **Monetization/back-office features.** Battle Pass, junk/crafting, NFT/metamask
   items, trading and tickets need kn_brain work. They only matter for the client
   through badges, challenges and inventory. Recommendation: the backend ports what the
   client displays; the economy is a separate project.
7. **Alternative movement schemes and triangle collision** are custom-game only
   (default off). Recommendation: WP-A phase 2, after the default-gameplay changes and
   recordings land.

### 5.2 KN bugs found during the survey (fix in the owning WP)

| Bug | Where | WP |
|---|---|---|
| Auto Pistol `noHeadShot` ignored: headshots do 1.5x instead of 1x | `shared/src/player.c:2258` | B |
| Weapon `movSpread` not modelled: Akimbo Uzi moving spread is wrong | `shared/src/player.c:1359`, `shared/include/weapons.h` | B |
| Jump-pad box sized from the map scale (matches neither v3's fixed extents nor v7 exactly) | `shared/src/player.c:634-640`, `shared/src/map.c:468-477` | A |
| Challenges window hangs on "Loading": kn_brain has no `chg` handler (fixed, part 2c: `["chg"]` -> `["chgL", list]`) | `client/src/ui/account_ui.c:1171-1176`, `services/brain/accounts.c` | E |
| Game server commits no per-stat keys (hs, n, wb, ...), so v7 challenges and badges cannot work (fixed, part 2c: `stats` / `prog` / `medals`) | `server/src/accounts.c:439-461` | E |
| `challMode`/`anonMode` stored but never sent in ENT | `client/src/main.c` ENT build | C |
| Nuke can detonate but nothing starts it | `shared/src/game.c`, `shared/src/player.c` | B |
| Viewer header comment names an `index.html?page=viewer` route that `app_mode.c` lacks | `client/src/viewer/viewer.c:3` | F |
| `tools/kref/movement-rec.js` needs `movement-sp.js`, which is not in the repo | `tools/kref/` | A |

