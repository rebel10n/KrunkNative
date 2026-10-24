# Movement schemes (class movSch)

Krunker v10.0.3 lets a map or a hosted game give a class one of eight movement schemes: the class
config key `movSch` ("Move Scheme", sub "Movement", deob-10.0.3.js:20337-20344). `procInputs` runs
`game.movement[this.config.movSch || 0]` (deob-10.0.3.js:191275), a list of scheme objects
(deob-10.0.3.js:18553-19199). KrunkNative ports all eight in `shared/src/player.c` ("v10.0.3 movement
schemes"); `shared/tests/movement_scheme_test.c` drives each one.

## How procInputs uses a scheme

A scheme has up to three functions; one it lacks falls back to Default's (`movement[0]`):

| Step | v10.0.3 | Gate | KrunkNative |
| --- | --- | --- | --- |
| `dir` (the look) | deob-10.0.3.js:191385-191391 | KrunkScript `defaultRotation` | `movement_scheme_dir` |
| `mov` (the acceleration) | :191716-191722 | `defaultMovement` and not stuck to a wall | `movement_scheme_mov` |
| `vel` (the integration) | :191741-191747 | `defaultVelocity` | Vehicle: `scheme_vehicle_vel`, else Default's block |

`mov` gets the tick's acceleration (ground / air accel with every multiplier: aim slow, crouch,
weapon, mode, perks, attributes), the move angle `movDir` = `config.movDirs[input[4]] + movDrP * PI`
(undefined without a key or while KrunkScript disables `movDir`), the input yaw (`mouseX`), the raw
input array and the frame in ms. Three scheme keys reach outside the functions: `airDecel` (the air
decel, SRM 1, :191631), `noThrow` (no aiming, SRM, :191450) and `noWallJ` (no wall jumps, SRM,
:192436). An out-of-range `movSch` runs Default (v10 would throw).

## The schemes

| movSch | Name | dir | mov | vel |
| --- | --- | --- | --- | --- |
| 0 | Default | look = input; below a turn speed (`trnSp` x `trnSpd`) of 1, turns at most speed x 0.01 rad/ms | keys relative to the look (`xDire`); diagonals x `strafeSpd` unless `realMov`; noclip flies with the pitch; a `camType` 1 class camera without `camRot` moves relative to the fixed offset, `-getDir(camOffX, camOffZ, 0, 0) % PI` | exponential decay (`fixMov`: exact integral) |
| 1 | Follow Mouse | Default | always runs toward the look's yaw (`-xDire - PI/2`), keys unused | Default |
| 2 | Move Right | Default | `velocity.x = 0.01`, `velocity.z = 0` every tick | Default |
| 3 | Fly | Default | always along the look, pitch included (`velocity.y += accel * sin(yDire)`) | Default (gravity still pulls) |
| 4 | Dir Only | Default | the keys along the world axes (W -z, D +x), the look ignored, no `strafeSpd` | Default |
| 5 | SRM | Default | Quake-style strafe movement, below | Default (air decel 1) |
| 6 | Vehicle | throttle / reverse / steering on the ground, below | push along the heading, drag, turn | y only (0.9993/ms, exact integral) |
| 7 | MMORPG | with a key, the body (`xDire`) turns toward the move direction at 0.012 rad/ms; the pitch is never set | keys relative to the input look (the camera), not the body; no `strafeSpd` | Default |

### SRM (deob-10.0.3.js:18680-19021)

Config: friction 0.006, baseSpeed 0.07, ground accel 0.014, ground decel 0.01, air accel / decel
0.00085, side strafe accel 0.05 / speed 0.01, minRampSlideVelocity 0.125, onWallMaxFrames 116 ms;
dash {accel 1000, cooldown 1000 ms, speed 0.099995, height 0.035}, walldash {1000, 1150 ms,
0.0525, 0.07}.

Each tick: `onGroundFrames` / `onWallFrames` count the ms on the ground / a wall; the velocity
`vel()` decayed is put back from `tmpVel` (while on a wall at most 116 ms), so only SRM's own
friction slows it. A grounded player's ramp (the first ramp of its grid cell it touches) decides:
a slow landing on it bounces down the slope (`tmpVel.y * min(1, slope / 45) * 0.75`); else the held
jump key auto-hops (`players.jump`) when grounded at rest; a fast slide over a ramp keeps its speed
(a wall-dash off it adds the ramp speed). The aim key (`input[6]`) wants a dash for 300 ms: on the
ground it dashes along the keys (along the look without keys), in the air against a wall it
wall-dashes along `tmpVel` reflected off the wall (straight off it when standing still). Then the
Quake part: keys `cmd` from `input[4]`, `wishdir` = (right, 0, forward) turned by `xDire`; on the
ground friction, no move for the first 33 ms after landing (a held jump keeps the hop's speed),
then Accelerate toward `baseSpeed + delta / 1000`; in the air Accelerate with the air accel, side
keys alone capped at 0.01 with accel 0.05. Accelerate measures against the pre-friction `tmpVel`,
so the ground speed settles at 0.087 / 1.102 = 0.0789 per ms at 60 fps.

The local player's `overlayInfo` (SetAccelerationValues: current / optimal / minimum / maximum
strafe angles, airborne with keys) drives the HUD's strafe helper, v10's `drawSRM`
(deob-10.0.3.js:178673-178719; `client/src/vfx.c batch_srm_overlay`): a 24-high bar at the screen's
middle, the gain zone green, the neutral zone grey, a centre marker. A side strafe (A or D alone)
always draws it. On a diagonal (W+D) the minimum angle divides by `2 - |wishdir|^2`, which is only a
rounding residue; when it is negative the angles are NaN and nothing draws (about two yaws in three).
That is v10's own arithmetic: `wishdir` is built from the same quaternion matrix (half-angle sines,
iíîìïïi, deob-10.0.3.js:28407-28434) so the residue has v10's bits.

### Vehicle (deob-10.0.3.js:19022-19175)

The "Vehicle Config" class keys (prefix `vh`, inserted after `movSch`, deob-10.0.3.js:20771-20786;
`ClassOverrides.vehicle_*`): Drag `vhdr` 0.97, Angular Drag `vhaDr` 0.94, Turn Lean `vhtL` -2.7,
Turn Threshold `vhtrsh` 0.0025, Turn Conv `vhtrsv` 0, Max Power `vhmP` 0.12, Power Force `vhpF`
0.0012, Max Reverse `vhmR` 0.0375, Reverse Force `vhrF` 0.0005. Per 60 fps frame
(`f = delta / 16.67`), on the ground only: W keys (`input[4]` 0..2) add `vhpF` to `forwardV` up to
`vhmP`, S keys (4..6) add `vhrF` to `reverseV` up to `vhmR`, both bleed off without their keys;
above `vhtrsh` (or in reverse) D keys (2..4) and A keys (0, 6, 7) change `angVel` by
`(trnSp || 1) [* vhtrsv * 10 * (forwardV + reverseV)] * (trnSpd || 1) * 0.004`, mirrored in
reverse. `mov` pushes `accel * 120 * (forwardV - reverseV)` along the heading, decays x / z with
`vhdr^f` (position by the exact integral, x `speedX` / `speedZ`), and turns `xDire` by angVel decayed
with `vhaDr^f`. The mouse never turns a vehicle. Before all that, `mov` leans the class "Model" asset
(`mdl` 2, `if (meshRef)`, deob-10.0.3.js:19145-19152) by the angVel it finds: `mdlDr % 180 != 0` sets
Euler "YXZ" and `rotation.x = angVel * vhtL`, otherwise Euler "ZYX" and `rotation.z`. KrunkNative keeps
that angVel in `Player.scheme.lean_ang_vel` (shared/src/player.c) and the client draws the lean on the
class model (client/src/player_class_model.c, doc/v10/class-models.md; ZYX as the default XYZ with
`z * cos(mdlDr)`, the same rotation for a yaw of 0 or 180 degrees). Like v10 only the side running `mov`
leans a model: a client its own player (reconciliation replays included), never the others, whose angVel
reaches only their owner.

## Sync (sendVals)

SRM's and Vehicle's state lives on the player (`Player.scheme`) and is part of the prediction:
SYNC_PLAYER appends the scheme's `sendVals` (SRM: onGroundFrames, onWallFrames, wishDash, dashTimer,
walldashTimer, tmpVel; Vehicle: forwardV, reverseV, angVel; deob-10.0.3.js:222398-222403) and the
owner writes them back before replaying (:258289-258295). KrunkNative's server sends them as
`NET_EXT_PLAYER_MOVE_VALS` (`NetPlayerMoveValsPacket`, shared/include/net_ext.h) right before each
RECON of the same ack; the client applies them with that RECON (`client/src/net/recon.c`). Peers
without EXT keep their own predicted values. On an official krunker.io server the link does the same
with "l"'s own values (`krunker_link.c` sync_tail): the six SRM values (tmpVel an `{x, y, z}` object,
three floats here) or Vehicle's three, read between custSync and the controlled-entity tail, go out as
the same `NET_EXT_PLAYER_MOVE_VALS` ahead of the RECON (scheme from their count; the client applies
them only when its class has that scheme). `resetMovVals` (spawn and class change,
deob-10.0.3.js:195029) zeroes them (`player_movement_spawn`).

## Camera, third person, KrunkScript

- **Camera.** v10's camera is always CONTROLS (the mouse); `xDire` / `yDire` only pose the player.
  Default with a turn speed of 1 copies the look into the player, so the two agree; between input
  frames the client copies the camera into the player only for Default with `trnSp` 1
  (deob-10.0.3.js:260853, `player_look_follows_camera`). For Vehicle, MMORPG and turn speeds below
  1 the body turns on its own: the client keeps the mouse look for the camera every frame and does
  not resync it from the body (`player_direction_is_look`, client/src/main.c); only a spawn resets
  it. In first person the arms (the player mesh) follow the body, as in v10.
- **Camera type.** `camType` 0..4 are FPS, 3rd Person, Platformer (X), Platformer (Z), Orthographic
  (deob-10.0.3.js:20162-20168; controls.camType isNormal / isTop / isSideX / isSideZ / isOrto,
  :257869-257873). Only Default's movement reads it (3rd Person without `camRot`, above). The class
  camera itself (offsets, top / side / orthographic views, the free mouse aim of a camera offset
  without pointer lock, `lockCamP`, `lckCmYR` / `yCmLim` pitch locks) is not drawn by KrunkNative's
  client yet; the game's `thirdPerson` setting and KrunkNative's own third person ride behind the
  shoulder with the mouse look, so MMORPG shows the body turning toward its move there.
- **KrunkScript.** v10's `onPlayerUpdate` sees `movDir` after `movDrP` (deob-10.0.3.js:191302-191305), and so does
  KrunkNative's (`ks_move_dir_value` in `shared/src/ks_game/ks_game.c`, which also maps our move key to input[4]
  as `(k + 1) % 8`: forward is -PI / 2);
  `disabledInputs.movDir` makes it
  undefined: Default, Dir Only and MMORPG stop, Follow Mouse, Move Right and Fly keep going (they do
  not read it), SRM keeps its keys (`input[4]`) but gets no `wishdir`, and a Vehicle keeps driving
  (it reads the raw `input[4]`). Free-range / backward sliding and air strafe also read the raw
  `input[4]`. `defaultRotation`, `defaultMovement` and `defaultVelocity` = false skip the scheme's
  `dir`, `mov` and `vel`.

## Deliberate differences

- SRM keeps `this.cmd` (the last keys a dash at the start of the next tick reads) on the shared scheme
  object, so on a v10 server a dash reads whichever player ran last; KrunkNative keeps each player's.
- SRM's ramp wall-dash scales the ramp speed by `iíïiïíí(player) / 45`, the slope of the *player*
  (no `dir`: undefined, so `velocity.y` became NaN); KrunkNative uses the ramp's slope.
- SetAccelerationValues also zeroes `tmpVel.y` for the local player; the next UpdateVelocity
  always overwrites it before anything reads it, so it is left out.
- drawSRM's `clearRect` of the bar's band (it erases the speed lines there) is not mirrored.

## Mode physics keys

Three keys of a game mode reach the same physics (shared/src/player.c, the accessors in
shared/src/modes/modes.c; `movement_scheme_test` checks them in Chaos Snipers and Impulsed):

| Key | v10.0.3 | Modes | KrunkNative |
| --- | --- | --- | --- |
| `groundAccel` | `(mode.groundAccel ?? 1)` on `grA` and on `arA` (deob-10.0.3.js:191624) | Chaos Snipers, Impulsed 2 | `mode_ground_accel` |
| `gravMlt` | gravity x `config.gravMlt` x `(weapon.gravMlt \|\| 1)` x `(mode.gravMlt ?? 1)` (:191697) | Chaos Snipers 0.92, Impulsed 1.05 | `mode_grav_mlt` |
| `impulseMlt` | a shot's knockback x `config.impulseMlt` x `(mode.impulseMlt != null ? it : 1)` (:196585 the shooter, :196732 a hit) | Impulsed 3, Zombies and Classic FFA 0 | `GameModeConfig.has_impulse_mlt` / `impulse_mlt` |

A 0 `gravMlt` or `groundAccel` would count in v10 (`??`); no v10.0.3 mode sets one, so KrunkNative
reads 0 as unset there. `impulseMlt` keeps its 0 (`has_impulse_mlt`).
