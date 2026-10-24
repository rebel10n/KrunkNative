# Krunker v10.0.3 class models

A class's "Player Asset" settings (the map editor's class config, `cls[id]` of a map's or a host's game config) can
replace the default block body with a sprite or a user-asset model. None of the 16 stock classes uses them
(deob/tables/cls-1003.json); custom games and maps do (the editor's Vehicle, Pirate and Flappy Bird templates,
doc/v7/map-logic.md). Line numbers are `deob-10.0.3.js` (Krunker_v10.0.3/deob/game.deob.js).

KrunkNative: `client/src/player_class_model.c` (`client/include/player_class_model.h`), hooked into
`client/src/player_mesh.c` (build, per-frame pose, free, rebuild check), `client/src/scene.c` (drawn instead of the
body), `client/src/net/s2c_events.c` (`c`, `uai`), `shared/src/player_anim.c` (the clips and the asset swap on both
sides) and the KrunkScript player object (`shared/src/ks_game/ks_hostobj.c`, `client/src/ks/ks_scene.c`).

## The settings (:20255-20334)

| Key | Name | Default | Use |
|---|---|---|---|
| `mdl` | Player Type | 0 | 0 Default, 1 Sprite, 2 Model |
| `aID` | Custom Asset | "" | a user asset id with letters: `g` GLTF, `q` GLB, `f` FBX (all three: animated), `e` emissive map, `n` / `s` normal / specular maps |
| `mdlFP` | Show First Person | true | build anything at all for our own first-person model |
| `animS` | Asset Anim Speed | 1 | the mixer's timeScale |
| `mdlS` | Asset Scale | 1 | the model's scale |
| `mdPS` / `mdPY` | Asset Preview Scale / Y Off | 1 / 0 | the menu / viewer model only |
| `mdlDr` | Asset Dir | 0 | the model's yaw, degrees |
| `mdlYO` | Asset Y Offset | 0 | added to the model's height |

## The build (players.generateMeshes :194228-194266)

- The whole player build sits in `if (config.mdlFP || IÍïíiií && !config.mdlFP)`, IÍïíiií being "seen from outside"
  (thirdPerson, a class camera offset, or another player's model, :194099). Our own first-person model of a class with
  `mdlFP` off gets nothing at all: no class model, and for a Default class no viewmodel arms or weapon either.
- With `mdl` set, the default branch is skipped: no body, arms, weapons or cosmetics. Prop Hunt's prop comes after
  it. The pet still follows (:194849-194852).
- The asset text is `player.assetTxt || config.aID`; `assetTxt` (a `uai` swap) is nulled after the build (:194265).
- Sprite (`mdl` 1): `loadSprite({src: id ? id + "/texture" : "sprite_0", SpriteMaterial, depthWrite, transparent,
  alphaTest 0.02, asset})`, an 11 x 11 camera-facing sprite at y 5.5 (:234089-234099). `id` is
  `parseInt(aID without its letters) || null`.
- Model (`mdl` 2): `loadAnimMesh` for `g` / `q` / `f` (fileExt `q ? glb : g ? gltf : f ? fbx`, startAnim "Idle",
  animS) or `loadMesh` (an OBJ, re-centred on its bounding box's y, :233140-233144), src `<id>/model`, emissive with
  `e`, at `(0, (anim ? 0 : 5.5) + (mdlYO || 0) + (preview && mdPY || 1), 0)`, rotation y `rad(mdlDr || 0)`, scale
  `(mdlS || 1) * (preview && mdPS || 1)`, on objInstances. Note the `|| 1`: every in-game model sits one unit up.
- hasAnims = `config.aID` holds `g`, `q` or `f` (:194877, :195127): the class's asset, never a swap.

## Each frame

- players.updateMsh (:193662-193681): no step bob for any `mdl`, no step sway for `mdl` 2, no turn for a sprite.
- players.update (:191184-191187): an interpolated player or our own plays `crouchVal ? "Crouch" : moved > 0.1 ?
  "Move" : "Idle"` (playClip with no reps: the looping main clip, which waits while a one-shot runs). `moved` is the
  frame's own move (oldX..z to x..z).
- Vehicle.mov (:19145-19152): `if (meshRef)` (a Model, OBJ or GLTF; a sprite has none) the model leans by `angVel *
  vhtL`, angVel as that mov found it before its decay, about x (Euler YXZ) when `mdlDr % 180 != 0`, else about z
  (Euler ZYX; KN: XYZ with z times cos(mdlDr), the same rotation for a yaw of 0 or 180). Only the side running mov
  leans a model: a client its own player, replays included (`Player.scheme.lean_ang_vel`, doc/v10/movement-schemes.md).
- players.shoot (:196426-196427): with `mdl` 2 our own shot has no view recoil, recoil tween or cancelInspect (and v10
  draws no muzzle flash, having no weapon mesh).

## Clips

`players.playClip(player, name, reps)` (:193806-193820) needs an active player with hasAnims. The server sends
`c` PLAYER_ANIM `[sid, animList.indexOf(name)]` (the list :21165) to spectators and to the other active players within
`mode.viewDis || 600`; a client plays it on `meshRef` (`playClip(name, reps)`: reps loops, -1 forever).

| Clip | Where | KrunkNative |
|---|---|---|
| Jump | players.jump, not in a recon (:195271-195273) | `player_jump` -> `player_play_clip`: sent by the authority; the client plays it on its own model (not during `net_reconcile`'s replay) |
| Melee | players.melee (:195909) | `player_melee`, same |
| Shoot | players.shoot, `isYou` only (:196417-196418) | `player_shoot`: our own model only, never sent |
| Idle / Move / Crouch | players.update each frame | `player_class_model_update` |

KrunkScript: `player.playAnim(clip, reps)` (`isNumber(reps) ? reps : 1`, no hasAnims test) and `stopAnim()`
(:208232-208244) work on the drawn GLTF.

## The asset swap: `uai`

`players.updateAssetID(player, txt)` (:197964-197975): `txt += ""`; with `config.mdl` and a non-empty text it sets
`assetTxt`, the server broadcasts `uai [sid, txt]`, and an active player is rebuilt (regenMeshes). It runs from
KrunkScript's `player.assetID = v` setter on either side (:207883-207887); the getter answers `config.aID || ""`.
KrunkNative: `NET_EXT_PLAYER_ASSET_ID` (0x0205), from KN's server and from the official link (`krunker_link_s2c.c`).

## Deliberate differences

- An asset this build cannot draw (an FBX `f` model: no FBX loader; a model file missing or broken) draws the default
  body. v10 draws nothing for it (its loader fails), which here would leave the player invisible for good. While the
  file downloads nothing is drawn, as in v10, and the model is rebuilt once it lands.
- A `uai` swap lasts until the player dies, not until the next model build. v10 nulls `assetTxt` in generateMeshes,
  so a later regenMeshes (an items or loadout update) brings the class's aID back; KrunkNative also rebuilds models
  for its own reasons (a skin download, the Stalker look, the third-person toggle). A swap that reaches a dead player
  waits for its spawn, as in v10, and one for a player the client has not seen yet (KN's server runs `onPlayerSpawn`
  before the SPAWN goes out) waits for that player's first model.
- A client-side `player.assetID` is drawn at the next model refresh (main.c, every 30 frames); a `uai` from the
  server rebuilds at once.
- The server's regenMeshes side effects of a swap (`updateHeadSize`, `swapWeapon(player, 0, true)`) are not repeated.
- The default body, arms and weapons are still built for a class model and kept out of the scene (like Prop Hunt's
  prop), since other client code reads their anchors.

## Not done

- The class camera types (`camType`, `camOffX..Z`, `orthoF`, ...): KrunkNative's camera stays first person, so our own
  model with `mdlFP` on is drawn around the camera, as v10 draws it in first person. Maps that use a class model
  usually pick a third-person camera; KrunkNative's own third person (`thirdperson`) shows the model from behind.
- `player.getAsset()` (an obj3D of `meshRef.children[0]`) stays undefined.
- The normal / specular companion maps (`n`, `s`).
- The first-person muzzle flash of a `mdl` 2 class (v10 has no weapon mesh to flash; vfx.c still flashes).

## Checking it

The input script's `models @label` logs each player's model (`body=model mdl=2 asset=11441g clip=Move`, `*` while a
one-shot runs). A map whose `config.cls` sets models (local user assets: 11441g has Idle / Move / Jump / Melee /
Shoot, 50331q, 29556g, 2153 an OBJ, 10637 a sprite), `bot_place class=N forceClass=1`, `bot_mimic on` (the bots jump
with us: `c` from the sandbox) and a server script setting `player.assetID` cover the build, the clips and `uai`.
Unit tests: `player_anim_test` (the `c` and `uai` sends), `krunker_link_test` (the link's `c` and `uai`).
