# Player poses and freezeOthers (Krunker v10.0.3)

KrunkScript `player.pose`, the `pz` PLAYER_POSE packet, the pose on player models (`hold` / "hands",
v10.0.3's `holdL` / "handL" and split arms, `bodyY`) and `game.applyFreezeOthers`. Line numbers are
`deob-10.0.3.js` (`Krunker_v10.0.3/deob/game.deob.js`).

| Part | v10.0.3 | Krunker Box Office |
|---|---|---|
| Script setter | `set pose(v)` :208220-208224, server script only (the classIndex guard) | `ks_player_pose_set` (`shared/src/ks_game/ks_pose.c`) |
| Sanitize | `game.sanitizePose` :220978-221034 | `ks_pose_from_value` (same keys, order, clamps, rounding) |
| Script getter | `get pose()` :208225-208231, `sanitize(poseData)` or null, both sides | `ks_player_pose_get` |
| Authority | `game.setPlayerPose` :221035-221042, broadcast `pz` sid, pose \|\| 0 | `game_set_player_pose` (`shared/src/player_pose.c`) |
| Round end | endGame :221901-221903, every posed player's pose cleared | `game_clear_player_poses` from `game_end_round` |
| Joiners | init `syncData.plrPose` :220294-220302, client :256989-256993 | `NET_EXT_PLAYER_POSE` after each posed player's SPAWN (`server/src/net.c`) |
| Wire | `pz` [sid, pose \| 0] | `NET_EXT_PLAYER_POSE` 0x0200, `NetPlayerPosePacket` (48 bytes, `has` 0 = pose 0) |
| Official lobbies | | `pz` and init `plrPose` translated (`client/src/net/krunker_link_script.c`) |
| Client | `players.setPose` :195354-195382, `pendingPoses` (applied at players.add :195135-195139) | `player_pose_on_ext` / `player_pose_client_set` (`client/src/player_pose_render.c`) |
| Render | `players.applyPose` :193340-193436 | `player_pose_render`, called from `player_update_meshes` |
| Weapon hidden | `isWeaponModelHidden` :195350 (weapon 0 or hold), `refreshWeaponVisibility` :195386-195411 | `pose_weapons` |
| "hands" | `getHands` / `syncHands` :193452-193476 | `PlayerMesh.hands_anchor`, `attachTo(p, x, y, z, keep, "hands")` (`client/src/ks/ks_scene.c`) |
| "handL" (10.0.3) | `getHandL` / `syncHandL` :193515-193549 | `PlayerMesh.hand_l_anchor`, `attachTo(..., "handL")` |
| Split arms (10.0.3) | `setSplitArms` :193477-193514 | the two arm pivots turned apart (below) |
| bodyY | `meshHoldObj.position.y` :193400-193402; `updateHeight` :193560-193566 | `PlayerMesh.hold`; `player_update_height` (render_you only) |
| faceRide | updateMsh :193675-193681 | `player_pose_face_ride_yaw` (every player's hard attachment, `client/src/entities.c`) |
| freezeOthers | `game.applyFreezeOthers` :221547-221573, reset at startGame :219811 | `game_apply_freeze_others`; `Game.freeze_others` reset in `game_start_round`; `/freezeothers` |

## The pose object

`sanitizePose` copies only these keys, each when `!= null`, and an object with none of them is no
pose (null):

| Key | Rule | Effect (third person unless noted) |
|---|---|---|
| `legs` | `max(0, min(2, round(parseFloat(v) \|\| 0)))` | 0 no legs, 1 the standing pair, 2 the crouched pair |
| `weapon` | `v ? 1 : 0` | 0 hides the weapon model (every weapon mesh, not the arms) |
| `bob` | `v ? 1 : 0` | 0 takes the step swing back off the per-arm (melee) meshes |
| `faceRide` | `v ? 1 : 0` | the model faces the yaw of the entity it is hard-attached to |
| `hold` | `v ? 1 : 0` | weapon model hidden, both arms to [-0.25, 0, 0], "hands" holds the item |
| `holdL` | `v ? 1 : 0` | the left arm to [-1.2, 0, -0.6] (first person: the melee left arm moved by [-0.5, 0.35, 0.5]), the weapon stays, "handL" holds the item |
| `legX` | parseFloat, finite, rounded to 1/1000, +-3.2 | every visible leg's (and shoe's) x angle |
| `bodyY` | the same, +-12 | the body, legs and shoes moved up / down; the local first-person eye too |
| `torsoX` | the same, +-1.6 | added to the upper body's x angle |
| `armL`, `armR` | arrays only: items 0..2 the same rule, +-3.2, a bad item 0 | the arm turned by Euler XYZ about the shoulder line (0, 0, 0.6 - weapon z) |

The client eases `legX`, `bodyY`, `torsoX` and both arm turns 0.25 of the way per rendered frame
(`poseLerp`, reset when a pose starts or ends).

## Arms

v10 builds a third-person model's arms as one merged mesh per weapon. A pose that names `armL`,
`armR` or `holdL` splits them (10.0.3: "player.pose armL and armR now move only their own arm in third
person"); otherwise `armR` (or `hold`) turns the merged mesh. KN's arms are two meshes with a pivot
each already (`PlayerArmMesh.pivot`, v10's `armMeshes[i]`), so a split is the two pivots turned apart
and a merged turn is both pivots turned by `armR`. `armL` is genArms' side 1 (the left hold), KN's off
arm; under the leftHand setting v10 mirrors the whole upper body, KN the arms only, so the turn is
applied as [x, -y, -z] and a holdL offset's x is negated.

The merged mesh keeps its own transform: with `armR` / `hold` it is put at rest and turned by armR
(:193428-193434), without either it keeps the turn a previous pose gave it until `resetMergedArms` at the
pose's end (:195371-195377). KN keeps that transform per weapon slot (`PlayerArms.pose_merged_*`) and puts
both pivots on it while the arms are not split; the split arms are put at rest and turned every frame.

## Attach points and layers

`attachTo(player, x, y, z, keepWorld, "hands" | "handL")` (:208838-208842) hangs the object on the
model's hands anchor (in the held weapon at (0, -1.5, 0), turned with armR under a third-person hold; on
the first-person knife arm at (0.9, -0.95, 0.72)) or its left-hand anchor (the "l" arm's hand, the arm's
turn undone). `syncHandL` also copies the arm's `layers.mask` onto the anchor and everything under it
(:193540-193546): on the local first-person model that is layer 1, which only the fpsCamera draws, so an
object on our own "handL" (or attached to one that is, or to a bone of a model that is) draws in KN's
viewmodel pass (`client->fps_scene`, `ks_scene.c` `obj_first_person`). `syncHands` copies no layer: a
"hands" object stays in the world pass, also on the first-person model.

## Freeze flags

`applyFreezeOthers` (:221558-221564) and the client's "frz" (:258510-258518) write `player.freeze` and the
script's own flags: `defaultMovement` / `defaultVelocity` = !freeze, `disableShooting` / `disableMelee` =
freeze. KN does the same (`player_apply_freeze`): `Player.frozen` is the freeze marker, the four flags go
into the player's script flags (`KsRef.allow`, created when the game has no script), which
`ks_player_allows` reads. So, as in v10: the script reads the freeze back (`player.defaultMovement` is
false), a script may turn a flag on again while frozen, an unfreeze turns all four on whatever the script
had set, and a respawn's reset turns the movement flags on and keeps `disableShooting` / `disableMelee` (`player.init`
:190737-190740: `ks_ref_respawn_player`); a leave clears them all, so the next player of the uid starts fresh.

## Differences (deliberate)

- `applyFreezeOthers` has no caller in the bundle (its caller is server-only code); KN reaches it with
  the host / dev chat command `/freezeothers [on|off]` (the caller is the exception).
- Poses are stored as floats; the getter rounds them back to the 1/1000 `sanitizePose` rounded to.
- The KN server sends a posed player's pose after every SPAWN of it (a joiner's `plrPose`); the client
  takes a repeat as the same pose (no new easing).

## Tests

- `player_pose_test` (`shared/tests/player_pose_test.c`): the setter's sanitize (every key, clamps,
  rounding, a string number, NaN items, unknown keys), the getter, clears, the client-side refusal,
  the packet, endGame's clear, the arm targets, bodyY on the rendering client only, applyFreezeOthers and
  the flags it writes (read back by the script, turned on by a script, a respawn's reset, an unfreeze, "frz").
- `client/tests/player_pose_e2e.py BUILD_DIR`: a headless offline client and the input script's `poses`
  probe: hold turns the merged arms by -0.25, a pose naming no arm keeps that turn, no pose puts them at
  rest, our own "handL" cube is in the viewmodel pass and the other five in the world pass.
- A headless offline render: `shared/tests/maps/v10/player_pose.json`, whose server script poses the
  first four bots (hold, holdL, a seat, an emote's lowering) and whose client script hangs a cyan cube
  on each player's "hands" and a red one on "handL". With an input script `0 play`, `300 con bot_stop
  on`, `400 con bot_add 5`, `2500 con freezeothers on`, `2550 con bot_stop off` (the frozen bots stay
  put), `4500 shot <png>`, `4600 quit`:

  ```sh
  env -u DISPLAY -u WAYLAND_DISPLAY KRUNKNATIVE_HEADLESS=1 XDG_CONFIG_HOME=<private> KRUNKNATIVE_HIDE_UI=1 \
    KRUNKNATIVE_CAMERA="0,8,20,-0.15,0" KRUNKNATIVE_FIXED_DELTA=16 KRUNKNATIVE_INPUT_SCRIPT=<script> \
    ./cmake-build-debug/client_bin --offline shared/tests/maps/v10/player_pose.json
  ```
