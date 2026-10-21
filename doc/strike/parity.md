# Krunker Strike gameplay parity (FRVR Krunker Strike 1.37.2)

What Krunker Strike plays by, item by item: FRVR's value with where it comes from in the bundle, what
Krunker Box Office did before this pass and what it does now under `FLAVOR_STRIKE`. The base game
(`FLAVOR_BASE`, Krunker v10.0.3) is unchanged by every row: each Strike value is reached through the
flavor's rules (`shared/include/strike_rules.h` -> `strike_ruleset()`, `flavor_current()->rules`).

Sources. The bundle is `~/src/KrunkerStrike_v1.37.2/public/assets/index-Cz2uYXJQ.js` (minified, build
`b97uTkCEJUQwVJG6JDIu7mxPv40kqAA0`), read as text only. "@N" is the byte offset of the quoted anchor in
that file. Every number below is extracted by `tools/strike/extract_strike.py` into
`tools/strike/strike_data.json` (sections `physics`, `combat`, `input`, `classes`, `weapons`, `modes`,
`config`), turned into `shared/src/rules/strike/strike_data.c` by `tools/strike/gen_rules.py`, and checked
by `strike_rules_test` (data <-> tables), `strike_assist_test` (aim assist / auto-fire) and
`strike_sandbox_test` (the server core playing it, base vs Strike).

Legend: "v10" = Krunker v10 (what KN plays as the base game; the target is v10.0.3, the values compared are v10.0.2's). "= main" = already Strike's value
on main before this pass (8271e3e5 data-driven rules).

## 1. Inputs

| item | Strike 1.37.2 (source) | ours before | ours after |
|---|---|---|---|
| shoot bind | LMB (10001), alt `\` (220) (`this.binds.shoot=new li("shoot1Key",10001,220)` @2872481..) | same | same |
| aim bind | RMB (10003), **alt Shift (16)** (`this.binds.aim=new li("aim1Key",10003,16)` @2873040) | alt `]` (221, v7 / v10) | alt Shift (16) (ui_controls.c `bind_def`) |
| binds Strike has no ControlBind for | swap, reset, resetLast, kickVoteY/N, kpdVoteY/N, kpdVision, specMiniMap, taunts, stalkerTaunts, smiteSB, unlimitedSB, spawnBot, clearBots, markPosition (`this.binds.*` list @2872481, 56 binds) | bound (E, B, N, 1/2, Y/N, =, -, 1-6, Numpad +/-, M2) | unbound by default (KrunkNative's own binds: Reload Game, Object Manager, Third Person keep theirs) |
| other default binds (move WASD, jump Space, crouch Shift, reload R, melee Q, primary T, ...) | `this.binds.*` @2872481 | same | same (checked bind by bind in strike_rules_test) |
| mouse wheel swaps weapons | never: `checkScroll` ends `r.scrollToSwap=!1` on every path (@2874542) | yes unless a bind uses the wheel (v10) | never |
| look formula | `mouseSens * sens * (camera.fov / fov) * fw()` (`updateMouseInp` @2880872); mouseSens 0.0024 | same (fw = 1) | same, fw() from the aim assist (row below) |
| pitch limit | +-PI/2 (`var A=Math.PI/2` @2880494, `Math.max(-A,Math.min(A,...))`) | +-PI/2 | +-PI/2 |
| flick clamp | mouseSkipFix false, flickClamp 500 (@2880179): off by default | not implemented (off) | not implemented (off: equal by default) |
| kbm sensitivity defaults | sensitivityX/Y, aimSensitivityX/Y 1 on every device (`sensitivityX:{...val:1` settings) | v10: 1.4 on phones, 1 desktop | 1 (Strike settings defaults, ui_settings.c) |
| mobile look sensitivity | sensitivityXMbl/YMbl, aimSensitivity*Mbl 1.4, stored x4 (`sensitivityXMbl:{...val:1.4` @2931781, `h*4`) | 1.4 x 4 (v10) | same |
| mobile gradual speed | gradSpeedMbl **false** | true (v10) | false |
| controller sensitivity | sensitivityXCntrl 4 / aim 3, stored x5; deadZoneL .3, deadZoneR .25, triggerThres .1 (`this.cntrlInput={...}` @2879718) | same | same |
| auto-fire (mobile) | autoFireMbl **on** (`autoFireMbl:{...val:!0` @2931482); gameLoop: an automatic weapon holds shoot while a target is in the box, released 260 ms after (`this.autoFireBuffer=260` @2877354); a semi-auto weapon toggles shoot every frame while the base box holds | none (v10's #mobileUI aim/shoot buttons only) | `client/src/strike_assist.c` (core) + `strike_assist_client.c`, on a phone under Strike |
| aim assist (mobile) | aimAssist **on**; box = playerScale*.5/dist*zoom x playerHeight*.5/dist*zoom around the centre, grown x1.5 / x1.1 for "on target" (`const dbe=1.1,fbe=1.5` @2869909); pull = aimAssistDist * innerWidth|Height * GE(dt) / fw, GE 2 until \|dist.x\| < .005 with no vertical pull, then 3 (vertical x.3 unless the target is airborne); vertical deadband playerHeight*.4 / *.3 per dist; fw() = .75 while locked (`const wbe=2,ybe=3,bbe=.005,_be=.75,xbe=.3,Sbe=.4,Cbe=.3` @2871653); aim point y + (playerHeight - crouchVal*crouchDst)/2; runs while a joystick is held or a target is tracked | none | same, `strike_assist.c`; 30 checks in strike_assist_test |
| touch layout | FRVR's control table (`const Gw={crouch:...}` @1891297): 640x360 base; reload (r190,b16), melee toggle (r100,b16), shoot (r190,t140, hidden while aiming / with auto-fire), aim joystick 88 (r97,t74: aim + shoot unless auto-fire), menu 32 (l24,t24), jump only with "disable tap jump"; crouch and swap disabled; dynamic move joystick 106 on the left half (`g(tF,{size:106` @1898250); the right half drags the look, a tap moving <= 9 px jumps (`t<=9&&tc.jump.down()` @1894892) | v10's #mobileUI (static nipple 120, v10 buttons) | FRVR's controls (client/src/ui/strike_touch.c, the Strike UI port) laid out from `StrikeRules.touch`: positions, sizes, 640x360 base, the 9 px tap, the move angles; auto-fire read as the switch it is (on: no shoot button, the aim stick does not fire) |
| move joystick directions | \|deg - 180\| in [0,15] or [165,195]: no forward/back; [75,105]: no strafe (@2883902) | same thresholds (ui_mobile.c) | same |

## 2. Rotations, recoil and spread

| item | Strike (source) | ours before | ours after |
|---|---|---|---|
| default FOV | config.fov **50**: settings fov = 50 + 20 = **70**, fpsFOV = 50 + 15 = **65** (`fov:{name:"settings.viewmodel.fov"...val:e.fov+20` @2937500) | 100 / 95 (v10) | 70 / 65 |
| sensitivity x FOV | x camera.fov / fov (zoom) | same | same |
| recoil / spread per weapon | weaponConfig (`{name:"Sniper Rifle",src:"weapon_1"` @861434), e.g. AR recoil .002 (v10 .003), spread 40 (v10 100), movSpread .35 | = main (100 overrides of v10 over 29 weapons) | = main; AR checked live in strike_sandbox_test (kick 2.0/s, spread 40) |
| recoilMlt, spreadAdj, spreadMove/Fall/Recover/Mlt, crouchSpread, aimSlow | .3, .00063, 25, 30, .985, 240, .55, .55 (config object @90796) | v10's (equal) | Strike's constants table (equal to v10's; strike_rules_test checks every one) |
| crouch lean | crouchLean **-(PI x .1)** (v7's; `crouchLean` in config @90796) | 0 (v10) | -PI x .1 (`strike_constants()`; the third-person model, player_mesh.c) |

## 3. Classes and loadouts

| item | Strike (source) | ours before | ours after |
|---|---|---|---|
| class list | Triggerman 0, Hunter 1, Run N Gun 2, Shotgunner 4, Detective 5, Rocketeer 7 (`{id:0,name:"Triggerman"` @854568) | = main | = main |
| health / speed / regen | 180/1/.15, 110/.85/.15, 160/1.2/.15, 160/1/.15, 180/1.05/.15, 230/.8/.15 | = main | = main (Hunter 110 HP and the Run N Gun / Triggerman walk ratio checked live) |
| loadouts | one primary each (AR, Sniper, SMG, Shotgun, Revolver, Rocket Launcher), no secondary | = main | = main |
| class pick | starter Triggerman; any class Strike lacks plays Triggerman | = main | = main (sandbox: class 3 -> Triggerman, 180 HP, AR) |
| class movement defaults | classConfig defaults applied to every class (`bs[mv][xe.classConfig[_p].varN]==null&&(...=def)` @857211): **grA .048** (v10 .042), arA .0047, grDec .99, airDec .9996, crchSpd .3; ground accel = config.grA/100 * speed (`he?G=this.config.grA/100` @1268445) | = main | = main (sandbox: Triggerman walks 1.146x v10's) |
| per-class perks / attachments | none in classConfig; no attachment slots in Strike's loadout (skins only: economy module) | n/a | n/a |

## 4. Movement and physics

| item | Strike (source) | ours before | ours after |
|---|---|---|---|
| jumpVel, jumpPush, crouchJump, aimJumpSlow | .072, .1, .25, .85 (`this.jump=function(c)` @1332264) | equal (v10) | equal (Strike constants) |
| gravity | .00015 x config.gravMlt x mode.gravMlt, no weapon term (`velocity.y-=v*(s.gravity*` @1270023) | v10 adds weapon.gravMlt (Boulder only) | equal: Strike has no Boulder |
| playerSpeed, airSpeed, groundDecel, airDecel, slideDecel, decelMin | .00042, .000047, .99, .9996, .999, .0001 | equal | equal |
| slide | slideTime 350 x slidTime x slT x crouchVal, velocity x playerSlideVelMlt 1.2 x slA x slidSpd (`c.slideTimer=r.slideTime` @1331243) | equal | equal |
| crouch | crouchDst 3, crouchSpd .007, crouchAnim .0008 | equal | equal |
| procInputs delta floor | `Math.max(.1, Math.min(input[1], dltMx))` ms, no deltaMlt term (`var A=.1,...` @1262843) | 1 ms x deltaMlt (v10) | 0.1 ms, unscaled (`Ruleset.delta_floor_scaled`) |
| dltMx | 33 ms | 33 | 33 |
| bhop / autoJump, fall damage | serverConfig autoJump false, fallDmg 0 | = main | = main |
| hit test | v7's padded AABB: x/z +-(scale + hitBoxPad + hbS), y .. ah + hitBoxPad + hbH (`Object.defineProperty(this,"hitboxPadW"` @1250123; no complexHitboxes / hurtboxScale in the bundle) | v10 posed hit boxes | padded AABB (`Ruleset.posed_hitboxes` 0) |
| hit zones | head: top - hitY < headScale x headMlt, legs: below legHeight - crouchDst x crouchVal, measured on the **live** target (`Zt=Ct<r.headScale*d.obj.headMlt` @1356237); hsMlt default 1.5, lsMlt .5 | rewound (lag-compensated) box | live target (`Ruleset.hit_zones_live`) |
| other config constants | the whole engine config object (282 numeric keys, @90796) | - | 69 GameConstants keys from it; every one equal to v10's but crouchLean and the delta floor; billboardCnt 4 (v10 6) |

## 5. Game rules

| item | Strike (source) | ours before | ours after |
|---|---|---|---|
| modes | 35 (v7's without Trade / Impulsed) (`FREE_FOR_ALL:0,TEAM_DEATHMATCH:1` @2044628) | = main | = main |
| public rotation | FFA, TDM, Hardpoint, CTF, Gun Game, Domination | = main | = main |
| score limits / time | FFA 1500, TDM 2500, ...; gameTime 3 min (serverConfig) | = main | = main |
| map pool | the 7 weighted pool maps | = main | = main |
| respawn | deathDelay 2800 ms | 2.8 s | 2.8 s |
| end screen | endTimer **30000** ms | 25 s (v10) | 30 s (`Ruleset.end_timer`) |
| trigger action limit | triggerActionLimit **5** | 8 (v10) | 5 |
| lobby cap, bots, tick | 10, 7, 30 Hz | = main | = main |

## 6. In-game feedback: damage numbers and the crosshair (strike-hit-feedback)

FRVR draws both on its game canvas (`ue.render`), under the DOM HUD, so the 1.37.2 DOM captures
(`doc/ref/krunker-strike-1.37.2/strike-ingame*.png`, black where the canvas was) show neither; the
rules are the bundle's. Krunker Box Office lays them out as DOM under `#InGameUIMobile`
(`#knStrikeFx`: `.kn-strike-dmg` numbers and `#knStrikeCross`, `client/src/ui/strike_hud.c`), the
numbers and geometry in `client/src/ui/strike_hud_fx.c` (`strike_hud_fx_test`), end to end in
`server/tests/strike_damage_numbers_e2e.py`. The base game's own HUD (`hud_ui.c`, `#inGameUI`) never
mounts on the Strike page, which is why Strike showed no number and no reticle before this.

| item | Strike 1.37.2 (source) | ours before | ours after |
|---|---|---|---|
| damage number on a hit | `doDamage`: `dmgType == 0 && (e = Math.ceil(e))`, `e && showDMG && ue.addDamageNumber(id, x, y, z, dmg, crit, heal)` (AI: id undefined) | none | `strike_hud_damage_number` from game_events.c's HIT (players by uid, bots share `STRIKE_DMG_KEY_AI`); dmgType / showDMG / dmgScale / critColor are the shared settings (FRVR defaults: Float, on, 1, #FFD542) |
| Damage Number Display | settings `damageNumberDisplayMode` val **0** (Stacking), options 0 Stacking / 1 Individual / 2 Legacy (`settings.interface.damageNumberDisplayMode.*`), Display tab's dropdown; but the feature `damage_numbers_display_mode` sets `default_damage_numbers_mode` **2** (Legacy) when the player has no saved choice (`FRVR.features.getFeature(...)` in the bundle; the 1.37.2 config in doc/ref/krunker-strike-1.37.2 has it as 2), so a new player sees Legacy | row rendered with the template's "Select", no handler | the dropdown (header + the three options) in strike_menu.c, default **Legacy**, stored as `damageNumberDisplayMode` with the Strike settings (knSettings, `krunknative-strike`), read by `client_render_settings()->damage_number_mode` |
| Stacking | `newDmgVal` with mode 0: the victim's live number by id (`r.val += abs(dmg)`), life **1000** ms, at feet + **1.25 x playerHeight** | | same (`strike_damage_add`) |
| Individual | mode 1: a number per hit, life **750** ms, at feet + playerHeight; numbers within **15** units get `needsHeightBoost` (lifted `lerp(2, 15, dist * (zoom > 1 ? 1.1 : 1) / 300)` once) | | same |
| Legacy | mode 2: `dmgVal`: life **1000** ms, y + **7**, rises **.022**/ms, drifts **.013**/ms at `randFloat(-PI, PI)`, no fade, white (`dmgColor`) / `critColor`, `#2e2e2e` stroke | | same (v10's number) |
| step and draw (Stacking / Individual) | rise `eye = .001`/ms for the first half of life, `tye = .025`/ms after; `globalAlpha = life / (life / 2)`; screen nudge `offsetX = min(.03, playerScale * .5 / (75 / d * 100) * zoom * 2)`, `offsetY` likewise (`playerHeight * .2`, `50 / d`) past 30 units; font `dmgScale * (crit ? 1.1 : 1) px GameFont`, `lineWidth 8` stroke; colours `interpolateColors` from the hit's (`#f87c7c` / `#8b0000`, crit `#FFD542` / `#D40B0B`) to white / black (crit `#ffeaa0` / `#5f0000`) over the first half; off the frustum: not drawn | | same: left / top as page fractions, opacity, colour and an eight-way text-shadow for the stroke; an off-screen or dead number has no element |
| crosshair | `updateCrosshair(max(58, spread * .85 * UH), aimVal * (inspecting ? 0 : 1) * (noCross ? 0 : 1) * (reloadTimer > 0 ? 0 : 1) * ...)` each frame (UH = page height / 693); `ue.render`: for `crosshairType > 0` (default **1**, Dynamic; Strike's settings have no crosshair rows) with the mini scoreboard down and not spectating, type 1 / 3: four `Hr(te, 6, 23, scale / 2 - 2, shadowThickness 1, angle)` lines (fill `crosshairColor` white, stroke `crosshairShadow` black) plus the 6 px dot; drawn on mobile too (no mobile guard) | none on the Strike page | `#knStrikeCross`: the four 6 x 23 px lines from scale / 2 - 22 to scale / 2 + 1 of the centre and the dot, opacity `strike_crosshair_opacity` (1 at the hip, fading as `aim_val` rises; 0 inspecting / noCross / reloading), hidden on the scoreboard, the kill card and while spectating; a uidiff scenario shows it only with `state.strike.crosshair` (FRVR's DOM captures have none) |
| aim reticles | `toggleAim` (bundle ~L72820): `aimDot.opacity = 1` when aimed (aimVal 0) with the weapon's sight (`attach` present and `i.attach[attachIndex]`: Reflex, index 0, by default), first person; `aimRecticle.opacity = 1` for a `scope` weapon when aimed (its viewmodel hides). aimDot draws `textures/reticles/reticle_0.png` 20 x 20 at the centre; aimRecticle draws `textures/scopes/scope_0.png` 680 x 680 with black borders (`scopeBorders`: #000000, opacity 1). Both are skipped with the kill card or spectating (`p`) | none: the crosshair faded and nothing took its place | `strike_aim_dot_shown` / `strike_scope_shown` (strike_hud_fx.c) drive `#knStrikeAimDot` and `#knStrikeScope` (strike_hud.c `aim_tick`); the art is `ui/strike/cdn/textures/{reticles/reticle_0,scopes/scope_0}.png` (sha256 in ui/strike/README.md). The dot is checked live in strike_input_e2e.py (pad); the scope is unit-tested only |

Not ported (not reachable in Strike's settings): crosshair types 2, 4, 5 (shapes, image, precision), the
throwing-knife crosshair (`crosshairTypeM`), `crosshairAlways` / custom opacity (type > 1 only),
`mapCross`; heal numbers (no healing in Strike's modes).

## Formerly open rows (closed in strike-gameplay3)

| id | item | now |
|---|---|---|
| O1 | FRVR touch HUD and its touch semantics | the Strike UI port's `strike_touch.c` (dynamic move stick, right-half look with the tap-to-jump, aim stick = aim + shoot unless auto-fire) reads its layout numbers from `StrikeRules.touch`; `autoFireMbl` is read as a switch (it was read as a number, so the default "on" counted as off: the shoot button showed and the aim stick fired, unlike FRVR's capture). The aim assist's "joystick held" is `strike_touch_sticks_active()` (joystick.active \|\| moveJoystick.active) under the Strike page |
| O2 | aim assist visibility | `cnBSeen \|\| !canSee(camera, point, dist < 20 ? .4 : .5)`: the eye line of sight or `game_can_see_point` from the bare camera position (the ray starts cameraHeight below it, boxes shrunk by the pad, as v7 canSee does with a point without height). cnBSeen is evaluated every frame here (Strike: every nameVisRate 200 ms) |
| O3 | Stalker mode | `GameModeConfig.stalker_v7` under Strike (strike_rules.c): pickStalker 400 HP, knife, no regen; onMelee: the stalker's swing jump-scares the target, heals 40 and deals the target's whole health; no v10 wall jumps / canSlide / onKill / camping / leap / wall stick. Checked live in strike_sandbox_test (base: 250 HP, x0.6, +10) |
| O4 | projectile head zone | `Ruleset.projectile_head_plain`: `top - hitY < headScale` (no headMlt) for projectiles under Strike (`V<xe.headScale`) |
| O5 | end-screen animation | `Ruleset.end_anim` 2.5 s under Strike (config.endAnim 2500; v10 5 s): menu_view.c's slow motion |
| O6 | Auto Fire / Aim Assist rows | in the Strike settings markup (ui/strike/screens/SETTINGS-game.html, the UI port); their defaults are Strike's (on). strike_menu.c's FOV / Weapon FOV fallbacks are 1.37.2's 70 / 65 (`config.fov 50 + 20 / + 15`; they were 90 / 80 from a capture), the same values ui_settings.c sets as the defaults under Strike |
| O7 | weapon keys without an engine field | pj_color: carried (`StrikeRules.projectile_colors`, the Rocket Launcher's 0xFDEBD0 in projectiles_render.c). reloY: Strike leaves it out (default 1) and v10's entries are 1: no difference. noSkins: the Combat Knife takes skins in Strike (v10: none): a store / market rule, not gameplay (the Strike economy module's) |
| O8 | projectile hit height | checked equal, no change: Strike's `a.y + T * sin(a.yD)` with T = spd * dt * dst after the update (yS -= grav * dt, yD = asin(yS / spd)) is yS * dt * dst; KrunkNative's `position.y + velocity.y * dt * t` runs after the same update (projectile_tick: gravity, then the move, then projectile_cast from the new position), so it is the same number for dropping projectiles too (a hitBox projectile's t is its distance / hitBox in both) |

No row is open.
