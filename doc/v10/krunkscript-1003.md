# KrunkScript: the v10.0.3 additions in Krunker Box Office

What v10.0.3 added to KrunkScript, how Box Office implements it, and what is still missing. Line numbers
are `deob-10.0.3.js`, the decoded bundle at `~/jails/kru/src/Krunker_v10.0.3/deob/game.deob.js`.

## 1. AI parts (`preciseHits`)

**10.0.3.** `AI.spawn(..., {preciseHits: true})` sets `prc` (:211720). On the server, the AI loads its asset's
`model.glb` (`loadModel`, :206431). It fetches the file from the user-assets host and reads it with its own small GLB
reader, `iîìïiìî` (:204349). The reader builds two things:

- a rig: the nodes with their names, parents and rest TRS, the centre of each node's mesh bounding box, the clips, and
  `byName`;
- a hit mesh (`iíìíííï`, :204466): each triangle goes to the skin joint that weighs most on its first vertex, in that
  joint's bind space through the inverse bind matrix. A triangle with no skin goes to its mesh node. Each group is
  sorted along its longest axis and split into chunks of 48 triangles with boxes.

`usesHitTest()` replaces the box test for hitscan, melee and projectiles (:196017, :196677, :206849). The ray goes into
model space (xD, mROff, `modelScale = (mSize || 4) * hbw`, mYOff) and is tested against the posed mesh: the clip at
`clipAt(now)`, hidden nodes skipped, a slab test per group and chunk, then Möller-Trumbore (:204603-204687). A hit
returns `{dst, part: {n, m}}`. `m` comes from `partMult`: the nearest `setPartDamage` value on the part or an ancestor,
otherwise `bodyDamage`.

Damage on a part hit is `dmg * m`. `m > 1` makes the hit marker a crit, and the crit band is not used. `m == 0` is an
armour hit (`armourHit`, :206223): `onAIPartDamage(sid, part, playerId, 0)` and no damage, and a bullet stops there.
Otherwise `dmgPart` is set and `changeHealth` calls `onAIDamage(sid, playerId, dmg)` and then
`onAIPartDamage(sid, part, playerId, dmg)` (:206243-206249).

**Box Office.**

- `shared/src/ai_parts.c` (`shared/include/ai_parts.h`) ports the reader, the pose (`iíîìíïî`), the ray, `partMult`,
  `clipAt`, `partPosition`, `setSolid` / `applySolid` and the `"aip"` record.
- `game_ai_ray_hit` (`shared/src/ai.c`) is the `usesHitTest() ? hitTest : box` choice. It is used in `combat_trace`
  (`shared/src/player.c`: shots and melee) and in the projectile AI loop (`shared/src/projectile.c`).
- `game_ai_damage_part` applies the multiplier, the armour hit and `dmgPart`.
- The KrunkScript hooks are `ks_game_on_ai_damage`, `ks_game_on_ai_part_damage` and `ks_game_on_ai_death`
  (`shared/src/ks_game/ks_game.c`). `onAIDeath(sid, name, killerId)` runs from `AI.kill` (:206338).
- The script setters are in `shared/src/ks_game/ks_hostobj.c`. They are server only, validated as in :208461-208507:
  - part names must match `/^[\w.\-]{1,48}$/`;
  - colours must match `#rgb` or `#rrggbb`;
  - multipliers are clamped to 0..20;
  - glow intensity is clamped to 0..1 and rounded with `toFixed(2)`.

**Parts on the clients.** `"aip"` `[sid, partGlow, hiddenNodes, heldClip, solidPose]` is sent from `AI.sync` to the
AI's viewers when `partsDirty` (:206561). In Box Office it is EXT subtype `AI_PARTS_EXT_SUBTYPE` (0x0c00): a JSON
record in `NetExtJsonChunk` slices, sent by `ai_parts_send` and applied by `ai_parts_on_ext` / `ai_parts_apply_json`.

`client/src/ai_render.c` ports `updateParts` (:205465-205560):

- A hidden node hides every mesh under it.
- A glowing node's meshes get the emissive colour and `max(ei, .25) * (1 + i * 6 * pulse)`.
- A glowing node also gets an additive halo sprite (a 64 px radial gradient) at its origin.
- The held clip plays once and holds its last frame, with no Idle / Move.

The node names come from the asset's own file: `model.glb`, or `model.gltf` for "g" assets.

On an official lobby, the link (`client/src/net/krunker_link_world.c`) turns the official `"aip"` into the same EXT
record. It also turns an `"aai"` whose getData carries `pg` / `hn` / `hc` / `sp` into that record, sent after the
add.

**Deliberate differences, documented in the code.**

- The model is read from `assets_path()`'s `user-assets/<id>/model.glb` through `asset_prefetch`, not fetched from
  user-assets.krunker.io.
- No pose cache. The JS reuses the first pose of each 1/30 s.
- No lag-compensated AI state. KN keeps no AI position history.
- `setPartDamage` keeps 32 parts.
- A malformed accessor drops its data instead of failing the whole model.
- 10.0.3 sends `pg` / `hn` / `hc` / `sp` inside `getData`. Box Office's fixed `"aai"` has no room for them, so a late
  viewer gets the `"aip"` right after the `"aai"`.
- The halo is sized from the meshes' bounding spheres, not a `Box3`.
- `heldLate`'s jump to the clip's end is not done.

The 10.0.3 reader's own limits are kept:

- `EXT_meshopt_compression` views are read raw.
- Signed position types give no triangles, but the model still counts as the hit mesh, so nothing can hit that AI.

## 2. `ai.solid` and the `playAnim` hold

**Solid.** `ai.solid = true` works only on a precise AI. It snapshots the current clip pose, position, xD, scale, mROff
and mYOff. `applySolid` (:205390) then puts the posed hit triangles into the map's quad tree:

- only triangles with an area above 0.01 are kept;
- at most 6000 are kept, the largest first;
- hidden nodes are left out.

Players collide with these triangles like with `cpx` triangles. In Box Office, `world_player_tris` (`shared/src/world.c`)
walks `ai_parts_solid_tris` after the map's own triangles. A model that is still loading is retried every tick
(`solidWait`). Clients rebuild the triangles from the `"aip"` `solidPose` for their prediction.

**Hold.** The `hold` option is real code, not "no behaviour": `ai.playAnim(clip, reps, hold)` (:208455) reaches
`AI.playAnim(ai, clip, reps, true, !!hold)` (:206452). On the server, a raw clip sets
`clipState = {n, t0, reps: max(1, reps || 1), hold}`. With `hold`, `clipAt` keeps posing the clip's last frame after it
ends; without it, the AI goes back to Idle / Move. A held clip also sets `heldClip`, which marks the parts dirty, and
clients show it with `showHeld`. In Box Office this is `game_ai_play_anim_hold` and `ai_parts_play_anim`.

## 3. The AI wrapper and `AI.spawn` options

**The AI wrapper.** It now has every 10.0.3 member (:208323-208510): `active`, a `pathIndex` setter, `maxHealth`,
`speed` / `turnSpeed` (per ms, over 0.031 / 0.002, clamped to 0..10), `team`, `autoTarget` / `target` (`scriptTarget`:
no automatic targeting, and no aggro on damage), `moveTo` / `clearMoveTo` (`goal`), `solid`, `bodyDamage`,
`setPartDamage`, `glowPart`, `setNodeVisible` and `getPartPosition`. `behaviour` reads and writes `ai.behavior`, which
is not `behavType`. It is undefined until a script sets it, and `0` freezes the AI's server update (:205618).

**`AI.spawn`.** It reads every key of :211612-211758 with the bundle's rules:

- numbers only (a string `"5"` reads as 0);
- `parseInt` or `parseFloat` as the bundle uses them, then the clamps;
- `modelScale` only when it is truthy, else `mSize || 4`;
- `health` 0 when absent;
- the asset keeps its letters, so a `"46802q"` AI has its clips.

`reflections` (:211721) is `limitMM(parseFloat, 0, 4)` of a number. It reaches clients in EXT `AI_REFL_EXT_SUBTYPE`
(0x0c01), sent after the `"aai"` of an AI with reflections, because getData's `refl` (:205147) does not fit KN's fixed
`"aai"`. The official link emits the same packet from `dat.refl`. The client gives the model's metallic materials
without an extras `env` the scene env map at that intensity (`air_apply_refl`, :233665-233669).

These keys are not read, because nothing in Box Office uses them yet: `hideTag`, `canContactHit`, `boss`,
`challengeKey` and the reward keys.

## 4. Other hooks and `getMatch`

- `onObjectDamage(sid, interface || 0, playerId, dmg)` and `onObjectDestroyed(sid, interface || 0, playerId)` come from
  `damageObject` (:220473-220481), through `game_object_hit` in `shared/src/triggers.c`.
- `CONFIG.getMatch().started` is `!game.waitTimers` (:212017). It is false while any wait timer runs, contTime ones
  included ("Zone closing in", "race ends in"). The earlier port excluded contTime timers, which was wrong.
- `alwaysSync` (`aSync`, :204917) is checked twice in 10.0.3: in `AI.sync` (:206576) and in `playAnim`'s `"ana"` loop
  (:206471). Box Office had only the first; the `"ana"` range now honours it too.

- `onPlayerUpdate`'s `inputs.movDir` is `config.movDirs[input[4]] + (movDrP || 0) * PI` (:191302-191305), with the
  player's class `movDrP`. Our move key k (0 forward, clockwise) is the client's input[4] `(k + 1) % 8`, so forward is
  -PI / 2 (:236111-236121). Before this fix KN passed `-PI + (k + 1) * PI / 4`, which was a quarter turn off, with no
  `movDrP`.

## 5. `scripts.shared`

**10.0.3.** `prepareGame` parses the main script for its side and then `scripts.shared` (`addScripts`, :216431). On
both sides, `entities.setup(sharedScript, matchSeed)` and `loadMapData` run before `mainScript.start()`. The server
also runs `spawnPending` (:220192). A client takes the seed from its init (`entSeed`, :256978).

Only the entity manager calls the shared script, and only through `onEntityInit` (server spawns, :212696) and
`simulateEntity`. The shared script gets no `start` / `update` calls.

**The server in Box Office.** It matches: `ks_server_round_init` loads the server script and then the shared one, and
`ks_game_start` runs `spawnPending` before `start()` (`shared/src/ks_game/ks_entities.c`).

**The client in Box Office.** It is missing. A 10.0.3 client steps `"sim"` entities locally between baselines and runs
controlled ones (`advance` / `stepControlled`, :213790-213990). That needs entity replication, which Box Office servers
do not send yet, and a client entity manager. `client/src/entities.c` snaps to baselines.

## 6. GAME.NETWORK on official lobbies (`sd`)

A KrunkScript message is `["sd", id, data]` in both directions: the client's C2S `CUSTOM_NETWORK_DATA` (:209466) and the
server's S2C `CUSTOM_NETWORK_MESSAGE` (:241493, `onNetworkMessage(id, data)`). `data` is the script's own JS value,
whose object keys are compiled as `"V_" + name` (`iíiìiîî.prefix`, :189986).

`shared/src/ks/ks_net_official.c` converts between that msgpack value and Box Office's `NET_PACKET_SCRIPT_MSG` body:

- incoming keys lose `V_`; outgoing keys gain it;
- a number id or key becomes its `String()`;
- nil, `Uint8Array` and `Date` become null.

`client/src/net/krunker_link_s2c.c` turns an S2C `sd` into a `SCRIPT_MSG`. `krunker_link.c` turns our `SCRIPT_MSG`
into a C2S `sd`. Box Office's own server / client path already matched 10.0.3: the 2000-character JSON rule, the
`c_broad` 10 and `n_send<id>` 20 limits, and `send` needing a player id on the server.

## 7. The rest of the API surface (diffed against :209382-212023)

Added here:

- `RAYCAST.fromPlayer` returns `obj` for the type checker, as in 10.0.3's type table (:207528). It was `any`.

- On the map object wrapper (obj3DServ, :208959-209045): `interface`, `health` (only for destructibles: set on the
  server, clamped to 1..1e6), `destroyed` and `toggleDestructible`. A destroyed object respawns (`"ro"`, onRespawn
  with no player); a standing one is destroyed (`"do"`, onDestroy / onDamage with no player).
- The map object wrapper's `position` and `scale` read the GameObject's own values: y is the centre and the scale is
  the half size (:208981-209012). Box Office returned the bottom and the full size. The `position` setters (server)
  call `game.moveObject` (:216882), which is `world_move_dyn_object` in `shared/src/world.c`:
  - only a `dyn` object moves;
  - a solid object carries the players standing on it and pushes the ones it runs into by their overlap;
  - `"objmv"` goes to every connection, using the client-handled `NET_EXT_KN_OFFICIAL_OBJECT_MOVED` that the official
    link already emits, and to a joiner later;
  - the round start puts the object back (`resetChangedObjs`).
- On the player wrapper: `godMode`, `speed` and `aiKills` (:207846-207866, :208133). `speed` reads
  `speedBoost || 1`; a server write clamps it to 0.1..10 and sends the player `"spb"`.
- On the client scene object (obj3D, :208565-208600): `intensity` (lights, 0..15), `distance` (point and spot lights,
  0..1000) and `color` (lights, `#rrggbb`), in `client/src/ks/ks_scene.c`. They read 0 / "" on other objects, and
  a write re-syncs the light. Compile-checked only: the client has no KrunkScript scene test harness.

Missing, outside this package:

- `LIVE_OBJECTS.addAsset` (:211292) and the live object wrapper (`id`, `delete`).
- On the player wrapper: `currentSkinIndex`, `giveSkin`, `setSkin` and `getSkin` (store skins, :207943, :208302-208320).
  `player.pose` belongs to `doc/v10/player-pose.md`.
- The entity wrapper's `attachPlayer`, `updateAttachment` and `detachPlayer`, and the ctx's `peer` / `peers`, all in the
  entities package.

## Tests

- `shared/tests/ks_game_v1003_test.c` (`ks_game_v1003_test`) covers:
  - the GLB reader on asset 46802;
  - model and box hits;
  - multipliers, armour and the hook order;
  - hidden parts;
  - the glow record and the `"aip"` send and apply;
  - the hold;
  - solid triangles pushing a player;
  - `onAIDeath`;
  - the wrapper members;
  - the `AI.spawn` clamps;
  - `onObjectDamage` / `onObjectDestroyed`;
  - `started` with a contTime timer.
- `shared/tests/ks_vm_test.c` (`test_official_net`) covers the `sd` conversions.
- `client/tests/krunker_link_test.c` covers `sd` in and out on the official link.
