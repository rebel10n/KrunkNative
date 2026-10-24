# v10.0.2 map format: changes from v7.1.4

Sources: v10 = `~/jails/kru/src/Krunker_v10.0.3/deob/v10.0.2/game.js` (`Lnnnnnn` = its line; the citations were taken
from an identifier-renamed copy with the same line layout). v7 = `/home/user/jails/kru/src/Krunker_v7.1.4/src/` (`map.js` = `data/map.js`, `MM` =
`libs/MapManager.js`, `PF` = `data/prefabs.js`, `TRG` = `data/triggers.js`, `KSA` =
`libs/createKrunkScriptApiContext.js`, `R` = `libs/render.js`, `CGC` = `data/customGameConfig.js`).
The parser `this.generate` is L167328-168492 (v7: `map.js:170-1490`).

Summary: the map JSON is a strict superset of v7.1.4. There is no version field and no migration
code. Two things break id-level compatibility: prefab 41 changes from TERMINAL to BOT_PATH_NODE, and
the lights move to THREE r186 units (intensities x PI). Everything else is additive. The largest
addition is the **Entities** system: map-level `archetypes`, the `ENTITY` prefab, a third
KrunkScript `shared` script, and `GAME.ENTITIES`.

## 1. Prefab table (PF vs L154335-156758)

v7 has 97 prefabs (0-96). v10 has 99 (0-98). Textures 0-19 (WALL..SNOW, L154019) are unchanged.

| id | v7.1.4 | v10.0.2 | v10 evidence | notes | implemented (KN) |
|---|---|---|---|---|---|
| 0-40 | same | same | | flag sets identical except `canDynamic` below | yes (`canDynamic` flag: PF_CANDYNAMIC) |
| **41** | TERMINAL (`PF:1386`, `map.js:646` builds nothing) | **BOT_PATH_NODE** | L155465 `'name': "BOT_PATH_NODE"`, `noCol`, `noCollision`, `tool`, `defaultSize [3,5,3]`, `lineCol 65535`, octahedron geo `#00ffff` | v10 drops TERMINAL from the table. The loader still skips both names (L167396 `"BOT_PATH_NODE" != _ && "TERMINAL" != _`), so either way an id-41 object builds nothing. Only the editor meaning changes. v7 editor already emitted BOT_PATH_NODE via `getPrefabIdByName` (returned 0 there, see doc/v7/editor.md:470). | yes: `PREFAB_BOT_PATH_NODE` (= `PREFAB_TERMINAL`). `prefab_data.c` row 41 is 10.0.3's BOT_PATH_NODE (deob-10.0.3.js:174150: `noCol`, `noCollision`, `noTexture`, `notStyleable`, `notTechnical`, `tool`, `genGeo`, 3x5x3, lineCol 65535; `tools/maps/gen_prefab_data.py` replaces v7's TERMINAL cube row), `TERMINAL` stays a name alias; maps store the id, so old map JSON loads unchanged; builds nothing |
| 42-96 | same | same | | | yes |
| **97** | - | **SCRAPCRATE** | L156724: crate_0.obj/crate_0.png, `scale 5`, `hasHealth`, `canInterface`, `canWallJump`, `canGrapple`, `canImpulse`, `editBulletCol`, `editAICol`, `stepSrc`, shadows | Loader has no special branch, so it is a generic prop (same path as CRATE, `scale` 5 vs CRATE 6). Scrap is also a currency (L102865 `'scrap': 5`). Guess: tied to the scrap/quick-sell economy, not gameplay. | yes: `PREFAB_SCRAPCRATE`, a MODEL_PROP (crate_0, scale 5), v10 dialect only |
| **98** | - | **ENTITY** | L156744: `isEntity`, `scalable`, `tool`, `transparent`, `opacity 0.3`, `lineCol 65484`, `defaultSize [10,10,10]` | An entity instance placement (section 4). No map object is built (L167403 `if ("ENTITY" != _) {...} else { entityInstances.push }`). | yes: `PREFAB_ENTITY`, no map object; kept in `Map.entity_instances` for the entities package (TODO) |

New prefab flag `canDynamic: true` on CUBE (L154373), TRIGGER (L155203) and CUSTOM_ASSET (L155540).
It is editor metadata that allows the per-object `dyn` key (section 2).

v3.1.1 ids 0-59 keep their meaning (v7 kept them; v10 keeps v7's). The only change is 41: v3.1.1
named it TERMINAL too (it was already a no-op in v7, `shared/include/shared.h:133`). No map under
`assets/maps/` uses id 41; `krunk_plaza_v2.json` has two id-42 (PREMIUM_ZONE) objects, unaffected.

## 2. Object keys (per entry in `objects[]`)

Key extraction over the parser (`_59474[...]` L167380-168500 vs `v12622.*` `map.js:250-1250`):

| key | v10 meaning | evidence | v7 | implemented (KN) |
|---|---|---|---|---|
| `dyn` (bool) | dynamic object: `obj.dyn = !!dyn`; forces `noAmb` (L163775), always gets a meshRef, stays in collision even with `noCol` (L165943, L166187). `GAME.OBJECTS` position setters call `game.moveObject(uid, x,y,z)` (L181706); `moveObject` only acts when `obj.dyn` (L187877), re-quads the object, updates shadows, pushes riding players (`onTopOf` -> `movX/Y/Z`, `movedWithObject`) and broadcasts `mo` | L167488 `'dyn': void 0 !== _["dyn"] ? !!_["dyn"] : void 0` (CUBE), L167732 (CUSTOM_ASSET), L168320 (TRIGGER) | new (v7 has no runtime object move with player carry) | parsed (`Object.dyn`, canDynamic prefabs, v10 dialect): noAmb (ambient.c) and keeps health / interface with noCol. Runtime move (`moveObject`, rider carry, `mo`) is TODO for the KS / entities package |
| `mp` (object) | per-map-object "mapProps" for custom-asset companion maps: `mp[<code>] = {ch: uvChannel, fy: flipY, i: intensity/scale}` where code is one of `n e s a m r l p b` (normal, emissive, spec, alpha, metalness, roughness, lightMap, displacement, bump) | L167718 `'mp': _['mp']`, L165531 `mapProps`, applied in `applyAssetCompanionMaps` L199452-199489 (table L199443) | new (v7 had only the `asset` letter flags, `MM:1863-1874`) | parsed and stored (`Object.mp`, CUSTOM_ASSET / asset TRIGGER). Not rendered: BasicMaterial has no normal / metal / rough / bump maps (client/src/user_asset.c) |
| `eid`, `ar`, `tag`, `as`, `sp` | ENTITY instance: entity id 1..9999, archetype id, tag (<=32 chars), `as` (0 = don't auto-spawn), `sp` spawn params (object). `p`, `r` reused (only `r[1]` = yaw is applied) | L168363-168371; spawnPending L184314-184331 | new | yes: `MapEntityInstance` (eid, archetype, position, rotation, tag, auto_spawn, spawn_params); 64 max, unknown archetypes dropped like loadMapData |
| `aoM` -> `aoMlt` | ambient-occlusion size multiplier per object (`8 * aoMlt` AO plane size) | L163750 `void 0 !== _["aoM"] && (this["aoMlt"] = ...)`, L168387 | v7 had none (8 fixed). Guess: set by the editor on objects; not seen in parsed keys, so possibly only on built objects | no (not a parsed key in v10.0.2) |
| `cv` | gone from the parsed set; `cv0/cv1/cv2` remain (L167662) | key diff | `map.js:553-555` reads `cv0..cv2` only; `cv` was a false hit, so treat as unchanged | n/a |
| everything else | identical key set and the same defaults (spot-checked CUBE, PLANE, SPRITE, LIGHT_*, ZONE branches) | | | unchanged |

Texture-id character whitelist (custom texture ids such as `skyDomeTexA`, `skyDomeEmisTex`, `mmId`):
v7 `/[^\dtlnesacdgxufq]+/` -> v10 `/[^\dtlnesacdgxufqmrpbhv]+/` (L164733, L159097; v7 `MM:891`).
This adds `m r p b h v`: metalness, roughness, displacement, bump, (h?), video. A port that
validates these ids must widen the set, or v10 maps lose sky domes and minimap images.

## 3. Map-level keys

Keys read off the map root in `generate` (L167200-168560):

| key | change | evidence | v7 | implemented (KN) |
|---|---|---|---|---|
| `archetypes` | **new**: entity archetype list (section 4) | L167386 | - | yes: `Map.archetypes` (id, name, coll, hp, data, parts, footprint, soft zones), `map_archetype()`; validation in map_validate.c |
| `botPaths` | now consumed by bots at runtime: `[{p: [[x,y,z],...], r: reversible}]` -> `map.botPaths[{pathNodes, reversible}]`; bots with `config.usePathing` (on for the newer bot type, L174979) walk them (L174550-174600) | L168471-168486 | v7 runtime ignored it (editor only, `editor.js:3581,3937`); KN already keeps `map->bot_paths` (`shared/src/map.c:673`) | yes: `Map.bot_path_list` (all dialects); tfue bots with `usePathing` walk them (v10 setupLocal turns it on), NextBots too (KN addition) |
| `scripts.shared` | **new third script** (besides `client`/`server`), compiled into `game.sharedScript` and run on both sides for entity simulation (L187094-187097, L189962) | `'shared' == _ ? this.sharedScript = ... : this.mainScript = ...` | v7 has `client`/`server` only | stored (`Map.shared_script`, v10 dialect); not run (entities package) |
| `shadowNB` | **new**: sky-light shadow `normalBias`, clamp 0..5, default 0 | L201758 | - (`R:2738-2772` sets only `bias = -0.0004`) | yes: `MapEnvironment.shadow_nb` 0..5, `shadow_normal_bias` in basic.frag (world units along the normal) |
| `correctLights` | **removed as a toggle**: THREE r186 has no legacy lights, so lights are always physical. Still parsed per environment zone (L164748), with no effect seen | renderer `init` L199044-199061 has no `useLegacyLights` | `R:408` `useLegacyLights = !correctLights` | yes: the v10 dialect always renders physical lights (map_environment.c) |
| `outputEncoding` | now maps to `renderer.outputColorSpace`; table `Default->NoColorSpace, sRGB->SRGBColorSpace, Linear->LinearSRGBColorSpace`; **index 0 is forced to 2 (Linear)** (`0 == _ && (_ = 2)`). With ColorManagement off (`deob-10.0.3.js:230030`) the output conversion is a no-op, so no value changes the image | L17259, L199057-199058; `deob-10.0.3.js:231105-231109`, `63117-63136` | `config.js:948` (Default = LinearEncoding), `R:407` | yes: parsed, no output encoding (basic.frag / sky.frag; doc/v10/render.md) |
| `ambientI`, `lightI` and all light intensities | **multiplied by `lightIntensityScaling = Math.PI`** (r155+ physical units) | `_7479 = Math.PI` L18383; L201738, L201743; point/spot/dir/rect L200274-200296; KS lights L183152-183205; lightMap L199446 | unscaled (`R:2744,2754,1718-1762`) | yes: `Ruleset.light_unit_factor` (`ruleset_current()->light_unit_factor`) multiplies ambient, sun, point / spot / directional and lightMap intensities in basic.c; rect-area unscaled like v10 |
| everything else (`ambient light sky fog fogD lightD sunAngX/Y shadowR/D skyDome* terrain* zone* ocean oc* tris xyz colors spawns camPos dthY nAmb oldAO hasLM welMsg gui config modes`) | unchanged | | | unchanged |

Lighting note for KN (implemented, verified): the x PI does not brighten v7 maps. v7 maps use legacy
lights (`useLegacyLights = !correctLights`), which three r150 already multiplies by PI; v10 (r186) has
only physical lights (x 1) and multiplies the intensities by `lightIntensityScaling = PI` instead, so
ambient, sun and directional light come out the same. KN renders physical lights (`correct_lights`
forced on) with intensities x `Ruleset.light_unit_factor` (PI): headless captures of Sandstorm and
Kanji under v7.1.4's legacy lights and v10's differed no more than two runs of the same build
(animated content). What does change in v10: point / spot lights fall off with the physical
inverse-square curve instead of the legacy linear one, and a map with `correctLights: true` (none under
assets/maps) renders PI times brighter than in v7. `outputEncoding` has no effect in v10.0.3 (ColorManagement
off: no output conversion), and KN renders it so (doc/v10/render.md).

Spawns (`spawns[i] = [x,y,z,team,dir,comp,teamOnly,spread]`, L168458-168469) are identical to
`map.js:1469-1478`. Terrain (`Terrain.CONFIG_PROPS` L161995) and zone (`Zone.CONFIG_PROPS`
L162140) are identical to `libs/Terrain.js:606` and `libs/Zone.js:178`. Ocean
(`CONFIG_PROPS` L192277) is identical. The terrain class gained exportState/importState and
raycast helpers (runtime only, no new JSON keys).

KrunkNative render fix (not a v10 rule, render only): the top-only face mask `"f": "8"` omits the
bottom face, so a collidable CUBE with that mask is invisible from below. On Atomic (`mmId` 52269,
`assets/maps/official/23_atomic.json`) the 14 collidable CUBEs of at most 4 units high with the mask
`"8"` also draw their bottom face (the mask becomes `"c"` in the client only). Collision, the top face
and every other map are unchanged; the rule is `shared/src/map.c` (`MAP_SLAB_UNDERSIDE_MMID`) and
`client/src/prefab.c`. The other 55 maps with top-only cubes (`assets/maps/`) have a different `mmId`
and render as before.

## 4. Entities (new subsystem)

Entities are server-authoritative, scriptable rigid groups of cubes/assets. Archetypes are stored
in the map, instances come from ENTITY objects or script spawns, and behaviour is a deterministic
`simulateEntity` hook in the shared script. Manager class L184033-185900 (`this.entities`).
It is a different system from v7's `LIVE_OBJECTS`-style `liveObjects` (that one also exists in
v10, L183000).

### 4.1 JSON

```
"archetypes": [{
  "id": 1..9999,             // unique
  "name": "<=24 chars [\\w -]",
  "coll": 0|1|2,             // 0 none (server skips bodies), 1 solid, 2 = noPCol (bullets only)
  "hp": >=0,
  "data": {...},             // JSON.stringify < 15000 chars
  "objects": [               // 1..40 parts, any prefab except ENTITY
    {"i": prefab, "p": [x,y,z], "s": [w,h,l], "c": color, "o": opacity, "aid": assetId,
     "n": "<=12 \\w chars (named part: script pose keys n_x n_y n_z n_yaw n_pitch n_roll n_clip n_clipS)",
     "soft": 0|1,            // 1 = soft zone (rider glue AABB), not a body
     "as": animSpeed, ...}
  ]
}]
objects[] ENTITY entry: {"i": 98, "p": [...], "r": [_, yaw, _], "eid": 1..9999, "ar": archetypeId,
                         "tag": "<=32", "as": 0 to not auto-spawn, "sp": {spawn params}}
```

Validation (map upload/host, L187585-187745): <=32 archetypes ("Too many archetypes."), <=40
objects per archetype, archetypes cannot contain entities, <=64 ENTITY objects ("Too many
entities."), unique `eid`, `ar` must exist, `tag` is a string <=32. Config: `archetypeLimit 32`,
`archetypeObjectLimit 40`, `entityLimit 64`, `entityReplayCap 64`, `entityStateKeys 32`,
`entityStateKeyLen 20`, `entityStateBytes 512`, `entitySimBudgetMs 2`, `entityCtrlNetRate 60`,
`entityCorrectionsPerTick 2` (config L19507-19660). The other map limits are unchanged:
objects 5000/7500, collidable 4500/6000, signs 100, prize wheels 10, leaderboards 2, AI 40,
`gui` 80 (`CGC:202-260`).

### 4.2 Runtime

- `loadMapData(archetypes, instances)` L184118 computes a per-archetype footprint (`footCX/CZ/HX/HZ/H`)
  from the non-soft parts and collects the soft zones. `spawnPending()` (server, at game start
  L189967) spawns instances with `as != 0`. eids from scripts start at 10000.
- `spawn(arch, x,y,z, params, {eid, tag})` L184284: calls the shared-script hook
  `onEntityInit(eid, archName, CTX params)`, which returns a state object. State must be
  num/bool/str values, <=32 keys, key <=20 chars, string <=64 chars, total <=512 bytes
  (`validateState` L184237). Then `buildBody` (parts become map blocks with `dyn:1`,
  `entityEid`) and a broadcast of `ENTITY_SPAWN` `e+`.
- Per tick (`_63176 = 33.33 ms`): `simulateEntity(eid, archName, state, ctx, dt)` (L184925, L185233).
  ctx keys (prefixed): `tick rand inputs id dist`; controlled-entity inputs
  `active forward strafe jump crouch shoot interact yaw pitch` (L184049-184060). Pose keys in state:
  `x y z yaw pitch roll`. Players riding an entity are carried (`carryPlayers`, `glueSoftRider`);
  players can be attached (`attachPlayer` / `detachPlayer`).
- Server hooks are queued and then drained into the main (server) script: `onEntitySpawn(eid, arch)`,
  `onEntityAttach(eid, playerId)`, `onEntityDetach(eid, playerId)`,
  `onEntityEvent(eid, name, a, b)`, `onEntityDamage(eid, dmg, by)`,
  `onEntityDestroyed(eid, by)` (hp <= 0 then despawn), `onEntityRemoved` (L184311, L184966,
  L185005, L185323, L185861-185863, L191471).
- Packets: `e+` ENTITY_SPAWN, `e-` ENTITY_REMOVE, `es` ENTITY_STATE, `ee` ENTITY_EVENT,
  `ea+` / `ea-` ATTACH/DETACH (L98080-98085). Shapes belong to net.md.

### 4.3 KrunkScript surface

`GAME.ENTITIES` (L183661): `spawn(archNameOrId, x, y, z, params{tag})`, `clone(entityOrId)`,
`despawn(id)`, `list()`, `findById(id)`, `findByTag(tag)`. Entity object (L181744): `id`
`archetype` `tag` `hp` `active` `sid`, `position.{x,y,z}` (setter teleports), `rotation`,
`getVar` `setVar` `setState` `playAnim` `stopAnim` `setAnimSpeed` `damage` `attachPlayer`
`updateAttachment` `detachPlayer` `despawn`.

## 5. Triggers (TRG vs L162268-163696)

| list | v7 | v10 | change | implemented (KN) |
|---|---|---|---|---|
| events | 32 (`TRG:7-103`) | 32 (L162268) | identical names and order; descriptions are now real text instead of "REPLACE" | unchanged |
| conditions | 12 (`TRG:104-320`) | **13** (L162365) | new **12 "Custom Action"**: `mainScript.shouldTrigger(playerId or "NONE", triggerSid, customParam)`; true passes (L162580-162597). Condition vals shape is unchanged | yes: condition 12 calls `TriggerScriptHooks.should_trigger` -> KrunkScript `shouldTrigger(playerId, sid, param)` (server hook); v7 dialect: 12 conditions |
| actions | 72 (`TRG:321-1609`) | **75** (L162600) | indices 0-71 are unchanged (same names, order, value shapes). New: **72 "Spawn Entity"** (Text, `trigConstTx = "arch[|tag]"`, spawns at the trigger object's position), **73 "Despawn Entity"** (Text tag), **74 "Set Entity Value"** (Both, `"tag|key"`, number = `getActionValue(...trigConst)`, `entities.setVar`) (L163648-163700) | yes: 72-74 parse and run through `TriggerScriptHooks.spawn_entity / despawn_entities / set_entity_value` (NULL until the entities package: no-ops); v7 dialect: 72 actions |

`triggerActionLimit` 5 -> **8** (actions per trigger object; config L20057, v7 `config.js`).

## 6. KrunkScript API changes (KSA vs L181990-183900)

| namespace | added | removed/renamed |
|---|---|---|
| LIVE_OBJECTS | new namespace with `addCube(x,y,z,w,h,l,_,opts{opacity, forceTransparency, aid (number = texture, "Nv" = video), color})` -> liveObject (L183345). v7 had `SCENE.addCube` only (KSA:2705, still present in v10 L182990) | |
| ENTITIES | whole namespace (section 4.3) | |
| SCENE | `addParticles(x,y,z,opts{texture: id or dust/glow/star/feather, glow, color, ...})`, rate-limited `p_parts` 30 (L182830) | |
| PAYMENTS | `giveCurrency`, `takeCurrency`, `getCurrencyBalance`, `giveSkin` (skin id >= `customSkinBase` 10000000), `authorizeSpend`; `transact` now returns "Transactions disabled for now" (L182653) | |
| AI | `removeBySid(sid)` (L183649) | `remove` |
| COOKIES | `removeByKey(key)` (L182677) | `remove` |
| TRIGGERS / OBJECTS | `getByInterface(id)` (L183707, L183746) | |
| OBJECTS | position setters now move `dyn` objects at runtime (section 2) | |
| hooks | `shouldTrigger(playerId, triggerSid, param)` -> bool (condition 12); shared script: `onEntityInit`, `simulateEntity`; server script: `onEntitySpawn/Attach/Detach/Event/Damage/Destroyed/Removed` | |

Compiler: v10 compiles scripts in the client (`_59600.parse` = `new Function(src).bind(ctx)()`,
L168737; `minify` L168757 still knows the `public`, `bool|num|str|obj` and `action` keywords). The
language itself shows no new types or keywords in the bundle. **Guess:** the KS->JS transpiler is
server-side, as in v7, so language changes cannot be confirmed from this file.

Not part of the map JSON: the editor is no longer bundled (no autocomplete table; `editorURL`
getter points to editor.krunker.io), so editor-only fields cannot be verified from v10.0.2.js.

## 7. Game-config (`config` / custom-game) keys (varN tables L16900-17200 vs `config.js` classConfig/gameConfig)

| varN | v10 | evidence |
|---|---|---|
| `hurtboxScale` | new, "Hurtbox Scale", Hitboxes, def 1, 1..2 step 0.1, hostTypes [0,1]; scales hit boxes and head (`hurtboxScale * headMlt`) | L16933, L117655-117773 |
| `noPvp` | new, "Player Damage Off" (Teams); names and minimap treat everyone as teammates | L17040, L159971 |
| `transferHostOnLeave` | new, Hosting, def true | L17119 |
| `webhook` | new, Other, string input, hostTypes [1] | L17194 |
| others | unchanged defaults/ranges (`classLim` was a parse artefact) | |

## 8. Compatibility verdict

| map source | loads in v10? | caveats |
|---|---|---|
| v7.1.4 maps | yes, unchanged | lights look the same (legacy x PI in v7 = physical x lightIntensityScaling PI in v10, checked on Sandstorm / Kanji, see the lighting note); point / spot falloff is physical; `outputEncoding 0` now means Linear; `correctLights` ignored |
| v3.1.1 maps | yes (v7 already loaded them; v10 adds no removals) | id 41 builds nothing in all three versions |
| v10 maps in KN | loaded (shared/src/map.c): prefabs 97 / 98, `dyn`, `mp`, `shadowNB`, `archetypes`, entity instances, `scripts.shared`, `botPaths` and triggers are read (section tables, "implemented"); Krunker v10.0.3 is the only version KN plays, so every map is read this way | entities do not run yet (entities package) |

## 9. Port checklist (map side, in order)

Status (v10-maps package): items 1-4 and the map-data part of 5 (archetype parse and validation,
ENTITY instances, `scripts.shared` stored) are done, and the renderer constant of 7 (x PI) is in
`Ruleset.light_unit_factor`; the entity manager and KS (6) are open (`outputEncoding`: no effect in v10.0.3, doc/v10/render.md).

1. `PREFAB_SCRAPCRATE = 97` (generic prop like CRATE, scale 5) and `PREFAB_ENTITY = 98` (no object;
   collect instance), plus `PREFAB_BOT_PATH_NODE` as an alias of 41. Raise `PREFAB_COUNT`.
2. Parse `dyn` and `mp` on CUBE/TRIGGER/CUSTOM_ASSET; parse `shadowNB`; widen the texture-id
   whitelist to `\dtlnesacdgxufqmrpbhv`.
3. Bots: use `botPaths` when the bot config has `usePathing` (cross-check gameplay.md).
4. Triggers: condition 12, actions 72-74, `triggerActionLimit` 8.
5. Entities: archetype parse and validation (shared/), the `entities` manager (shared/ simulation,
   server authority), `scripts.shared`, the `GAME.ENTITIES` KS namespace, and the packets in net.md.
   This is the largest item.
6. KS additions in section 6 (LIVE_OBJECTS.addCube, SCENE.addParticles, AI.removeBySid,
   COOKIES.removeByKey, getByInterface, currency functions as stubs).
7. Renderer: decide on the light-unit change (x PI) and the linear default output. Treat it as a
   documented, per-version renderer constant, not a map edit.
