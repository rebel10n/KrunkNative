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
The `uai` handler only finds players this client has added (`findBySid`, :257784). generateMeshes reads `assetTxt ||
aID` and nulls `assetTxt` (:194230, :194265), so a swap lasts until the next generateMeshes: players.add on a spawn
(also for a player alive already), a regenMeshes for items, a loadout, a prop or another swap.

KrunkNative, exactly so: `NET_EXT_PLAYER_ASSET_ID` (0x0205) from KN's server and from the official link
(`krunker_link_s2c.c`); `Player.asset_txt` is assetTxt, taken by the next build of the model. Every model build is a
generateMeshes except KrunkNative's own rebuilds, which keep the model's asset and its getAsset handle
(`player_class_model_keep`: a downloaded skin or model file, the Stalker look, KN's third-person toggle). A SPAWN for
a player alive already (main.c keeps its meshes) rebuilds a model that shows a swap. The client script setter rebuilds
at once (`ks_client_host.c`), the `uai` handler too (`s2c_events.c`); a `uai` for an unknown sid is dropped.
KN's server runs `onPlayerSpawn` inside `player_spawn`, before the SPAWN goes out: a swap made there is held and sent
right after that SPAWN (`player_anim_spawning` / `player_anim_spawn_sent`), so a new player's first model gets it.

## getAsset()

`player.getAsset()` (:207888-207892) is `obj3D(meshRef.children[0])` when the class model's holder has a child: the
GLTF scene root of a loaded `loadAnimMesh` (an OBJ's `loadMesh` mesh has no children, a sprite no meshRef: undefined).
obj3D (:208553-208890) wraps it once (`_protected ||=`) and resets its `data` to `{}` on every call. On a scene root:
position / rotation / scale / move / rotate / lookAt / visible / delete / attachTo / detach and getBone act;
playAnim / stopAnim / updateMorph / animSpeed do nothing (the mixer is the holder's), emissive and opacity find no
material (opacity reads 1), texture only keeps `data.asset`. KrunkNative (`ks_scene.c`, `KSO_PLAYER_ASSET`): an
obj3D whose transform sits between the holder and the GLTF root, bound each frame while that model lives (a
KrunkNative rebuild keeps the binding).

## Companion maps

The asset letters' maps (`applyAssetCompanionMaps` :231687-231752, from getMat :232132 and the GLTF setup :233704):
`n` `model_n.png` as normalMap (normalScale 1), `s` `model_s.png` as specularMap, flipY false (no mapProps). A GLTF's
MeshStandardMaterial takes the normal map (replacing its own) and, with `e`, `model_e.png` as emissiveMap; it has no
specularMap. An OBJ's getMat material (the lighting setting's) takes both, sampled at `1 - v` over its flipped base
map (`BasicMaterial.maps_flip_v`); the specular map's red scales the Phong specular (`specular_texture`, basic.frag).

## Shots

A class model has no weapon mesh, so our own shot draws no muzzle sprites, screen flash or casings
(:196426-196484) and no tracer of its own (`weaponMeshes[loadoutIndex]`, :196958); vfx.c `local_shot`.

## FBX

v10.0.3 draws an `f` asset with three's FBXLoader. KrunkNative has no FBX loader. ufbx (a single-file C99 loader,
MIT / public domain) was weighed against AGENTS.md's rule against large external dependencies: it is C and one file
like cgltf, but about 1.4 MB of source (ufbx.c 1.18 MB + ufbx.h 0.22 MB, five times cgltf.h), and drawing an FBX
class model would need a second model runtime next to the cgltf-based one in gltf.c (skins, baked clips, materials) or
an FBX to glTF converter of several hundred lines, with no FBX class model in KrunkNative's content to test against
(one map object, 30213f, uses an FBX). Not taken: an FBX class model draws the default body instead.

## Deliberate differences

- An FBX class model draws the default body (see FBX), not nothing.
- A missing companion map is not bound (three samples an image-less texture black).
- The server's regenMeshes side effects of a swap (`updateHeadSize`, `swapWeapon(player, 0, true)`) are not repeated.
- The default body, arms and weapons are still built for a class model and kept out of the scene (like Prop Hunt's
  prop), since other client code reads their anchors.

A missing or broken model draws nothing, as v10's loaders leave nothing; while the file downloads nothing is drawn and
the model is rebuilt (kept) once it lands.

## Not done

- The class camera types (`camType`, `camOffX..Z`, `orthoF`, ...): another agent's work. Until then our own model
  with `mdlFP` on is drawn around KrunkNative's first-person camera, as v10 draws it in first person.

## Checking it

The input script's `models @label` logs each player's model (`body=model mdl=2 asset=11441g clip=Move`, `*` while a
one-shot runs, `swap=1`, `maps=nse,flip`, `lean=...`). A map whose `config.cls` sets models (local user assets: 11441g
has Idle / Move / Jump / Melee / Shoot and a Head bone, 50331q, 29556g, 2153 an OBJ, 632 an OBJ with `_n` `_s` `_e`
maps, 10637 a sprite), `bot_place class=N forceClass=1`, `bot_mimic on` (the bots jump with us: `c` from the sandbox),
a server script setting `player.assetID` in `onPlayerSpawn` / `onPlayerUpdate` and a client script calling
`getAsset()` (`KRUNKNATIVE_KS_LOG_STDOUT=1`) cover the build, the clips, `uai` and the handle; `KRUNKNATIVE_VFX_LOG=1`
logs a class model's shot. Unit tests: `player_anim_test` (`c`, `uai`, the spawn deferral), `krunker_link_test` (the
link's `c` and `uai`), `movement_scheme_test` (the lean's angVel).
