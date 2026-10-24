# KrunkScript entities on the wire: replication, the client copy, attachments, LIVE_OBJECTS

How Krunker Box Office replicates v10.0.3's KrunkScript entities, runs `scripts.shared` on the client, attaches
players to entities and adds live assets. Line numbers are `deob-10.0.3.js`, the decoded bundle at
`~/jails/kru/src/Krunker_v10.0.3/deob/game.deob.js`. The entity manager is the class at :212465-214818, the live
objects :212096-212411. The simulation itself (spawn, step, hooks) is in `doc/v10/krunkscript-1003.md` section 5.

## 1. The wire

Every record is one JSON array in `NetExtJsonChunk` slices of an EXT subtype (`shared/include/net_ext.h`):

| subtype | 10.0.3 | record | sent |
|---|---|---|---|
| `NET_EXT_ENTITY_SPAWN` | `"e+"` | `[eid, archId, tick, state, tag, mode, hp]` | spawn (:212720), a joiner's `ent` |
| `NET_EXT_ENTITY_REMOVE` | `"e-"` | `[eid, reason]` | despawn (:212782): `"script"`, `"destroyed"` |
| `NET_EXT_ENTITY_STATE` | `"es"` | `[eid, tick, state]`, one per baseline | below |
| `NET_EXT_ENTITY_EVENT` | `"ee"` | `[eid, name, a, b]` | ctx.emit after an advance (:213922), `"_damage"` [amount, hp] (:214550) |
| `NET_EXT_ENTITY_ATTACH` | `"ea+"` | `[sid, eid, {o, lk, ctl, soft, nd}]` | attachPlayer (:213483), a joiner's `entAtt` |
| `NET_EXT_ENTITY_DETACH` | `"ea-"` | `[sid, eid]` | detachPlayer (:213540) |
| `NET_EXT_ENTITY_CONTROLLED` | `"l"`'s tail | `[eid, tick, state]` | before the controller's RECON (:222404-222405) |
| `NET_EXT_ENTITY_SEED` | `entSeed` | `[seed]` | a joiner's init (:220292) |
| `NET_EXT_KN_OFFICIAL_LIVE_OBJECT` | `"lo"` | `[id, data]` | addBlock / addAsset (:212136, :212161), a joiner's `lo` |
| `NET_EXT_KN_OFFICIAL_LIVE_SYNC` | `"slo"` | `[id, "remove"]` | liveObjects.remove (:212376) |

Baselines go out:

- two per server update, round robin (broadcastBaselines, :214389);
- after every state write: setVar, setState, teleport (applyStateWrite, :214473);
- after damage that leaves the entity standing;
- for a push of more than 0.05 by a controlled entity (displaceEntity, :214376);
- for an entity frozen over the sim budget (freezeSlowest, :213990);
- for every controlled entity a step moved, once per update (broadcastControlled, :213824).

KN's server sends them from `shared/src/ks_game/ks_entities.c` through `game_send`. `server/src/net.c` sends a
joiner's state with `ks_entities_send_state` after its INIT, or after its HELLO for a fresh connection whose INIT
could not carry EXTs yet. It sends the controlled entity with `ks_entities_send_controlled` right before each RECON.
An official lobby's records come through `client/src/net/krunker_link_script.c`: the same EXTs, `entSeed` included.

## 2. The client copy

`client/src/entities.c` hands the records to the shared manager (`shared/include/ks_entities.h`), which keeps
10.0.3's client half:

- **applySpawn** (:212792). A known eid is despawned first. The entity takes the payload's tick, mode and hp, and
  queues `"_spawn"`.
- **applyRemove** (:212826). It queues `"_removed"`, even for an eid the client does not know.
- **applyBaselines** (:214407). It is skipped for an entity our own player controls. The state and tick are the
  server's. A "sim" entity whose baseline is older than the client's tick, by 64 ticks at most, runs the ticks again
  with no events (`replaying`). An uncontrolled entity gets the 100 ms visual blend and a simBuf pose. Another
  player's controlled entity gets a poseBuf entry.
- **applyAttach / applyDetach** (:213548, :213579). A `ctl` attachment makes that player the controller;
  `controlledByLocal` when it is ours.
- **applyControlledSync** (:213849). It sets the state and the tick; the RECON that follows replays our inputs on it.

Every frame, `entities_update` (called by `ks_client_frame` before the client script's `update`) runs game.update's
client half (:222187-222216) once the entity clock has a sample:

- `advance(clock)`: each active, unfrozen, uncontrolled "sim" entity steps to `floor(clock / 33.33)` with the
  client's own `scripts.shared`, the same seed and the same ticks as the server, so it stays where the server
  will put it;
- `drainClientEvents`: `"_spawn"` calls `onEntitySpawn(id, archetype)`, `"_removed"` calls `onEntityRemoved(id)`,
  and the other names call `onEntityEvent(id, name, a, b)`, except the names starting with "_". As in 10.0.3, an
  event both stepped locally and broadcast reaches the client script twice;
- `updateVisuals`:
  - the render poses (getRenderPose, :214562): another player's controlled entity from poseBuf one send interval
    (100 ms) back; an uncontrolled one from simBuf one tick back; otherwise the prevPose lerp plus the blend;
  - our soft ride's carry (softCarryPose);
- the other players' rides (:191173-191183): pinned to their entity's render pose, or glued for a soft ride.

`client/src/ks/ks_client_host.c` loads `scripts.shared` after the client script (prepareGame, :220186-220193).
The entity clock is `entityClock` (:215740-215802), in `client/src/entities.c`:

- samples are a baseline's `tick * 33.33` against the receive time;
- the clock uses the largest offset of the last 2 s;
- it snaps on a jump over 250 ms and otherwise eases by a tenth.

The bodies stay in `client/src/entities.c`: one spawned map object per solid part, the collision boxes at the
committed pose (placeBody, carrying our player standing on a solid part) and the meshes at the render pose.

## 3. Attachments

The entity wrapper's `attachPlayer(playerId, opts)`, `updateAttachment(playerId, opts)` and `detachPlayer(playerId)`
(:209172-209188) are server only.

- **attachPlayer** (:213435) reads these opts:
  - `o` (an array) or `ox` / `oy` / `oz`, and `by`;
  - `lk`, `ctl`, `soft` and `nd`, as flags.

  A `ctl` attachment to an entity someone else controls is refused. A player attached elsewhere is detached first.
  A hard attachment pins the player at once. A `ctl` one makes it the controller. It sends `"ea+"` and queues
  `onEntityAttach(eid, playerId)`.
- **updateAttachment** (:213495) keeps the keys it is not given.
- **detachPlayer** (:213516) only detaches from this entity. It releases a controlled entity at the manager's tick,
  then sends `"ea-"` and queues `onEntityDetach`.
- **pinPlayer** (:213605) places the player at the entity's pose plus the offset turned by the yaw. The player gets
  no velocity, stands on the ground and drops its grapple.
- **pinAllServer** (:213665) runs after each server advance. It runs the soft zones (updateSoftZones, :213718), pins
  every hard rider and detaches a rider that died.
- **procInputs' rider** (:191612-191621, `ks_entities_player_ride` from `shared/src/player.c`) runs on both sides.
  A hard rider:
  - steps the entity it controls (stepControlled, :213790): its inputs come from the input (forward / strafe from
    `config.movDirs`, jump, crouch, shoot, interact, yaw, pitch); ctx.tick is the input number; on the server a
    window of 1.5 s of steps per second; no events on a client or in a replay;
  - is pinned;
  - keeps its old position;
  - skips the acceleration, as on the ground.
- **nd**: the rider's damage to its own entity is refused (:214541-214544). `ks_entities_shot_passes` is
  shotPassesThrough (:214153).

## 4. LIVE_OBJECTS.addAsset

`GAME.LIVE_OBJECTS.addAsset(assetId, x, y, z, scale, rotY, opts)` (:211292) is server only. It reads:

- the id's digits only;
- numbers only, otherwise 0, and a scale of 1;
- the scale clamped to 0.01..100;
- `opts.complexCollision` as `data.cpx`.

The manager (:212144) keeps `{type: "asset", aid, x, y, z, s, ry, data}`, 200 objects at most with the blocks, and
broadcasts it in `"lo"`. The live object wrapper (:209050) has `id` and `delete()`, which sends `"slo"`. A client
draws `<aid>/model.glb` at (x, y, z), turned ry and scaled s. In KN that is a spawned CUSTOM_ASSET map object
whose GLB joins the scene through `user_asset_object_added`.

## 5. Bodies, blocks and complex collision

- **Bodies** (buildBody :212839, placeBody :213050, carryPlayers :213091) are built by the shared manager on both
  sides: a solid entity's parts are map objects on the server too, and a client also builds the non-solid ones (no
  collision). A moved solid part takes along the players standing on it and pushes the ones it runs into, so players
  stand on and collide with entities on the server and the client predicts the same. The server's objects are no
  Object Manager objects (not `spawned`, `spawn_owner` -77) and leave the map at the next round.
- **`LIVE_OBJECTS.addCube(x, y, z, w, h, l, _, opts)`** (:211252-211290) keeps the block with v10's props: src `wall_0`
  or the aid's texture / video, opacity, fTrans and colr. It is a solid server map object (addBlock, :212115); a
  client's `"lo"` builds the same block.
- **complexCollision**: addAsset's cpx loads the model's rest triangles (iîiîïîì :204179, 6000 at most,
  `ai_parts_model_rest_tris`), scaled, turned ry and moved, on both sides, into the player collision
  (`world_player_tris`; loadTris :212187).
- **The `lk` yaw lock** (:260606-260640): `entities_ride_look` (from `client/src/main.c`'s input frame) turns the look
  with a ridden entity and, with `lk`, sets it to the entity's yaw.
- **The v10 bridge** (`server/bridge/v10_bridge.c`) sends official clients `e+` / `e-` / `es` / `ee` / `ea+` / `ea-` /
  `lo` / `slo`, held until START, `entSeed` in the init, and the controlled entity as the `l` array's slot 26.

## 6. Deliberate differences

- **Soft carry at once.** A soft rider's movX / movY / movZ, and carryPlayers' moves, move the player immediately.
  10.0.3 adds them to movX, which pinAllServer or procInputs applies right after. KN has no class hit box pads
  (hbS / hbH) in carryPlayers.
- **onTopOf** is measured (grounded, feet at the top, inside the footprint) for carryPlayers and the soft zones,
  because KN keeps no onTopOf.
- **The match seed** is the shared script's hash with the map, sent as entSeed, not `Math.random`. On the bridge, a
  seed that arrives after the init was built is dropped, as 10.0.3 reads it in the init only.
- **Without `scripts.shared`** a client still places its entities from the baselines. 10.0.3 runs none of the
  entity half of game.update without hooks.
- **The entity clock** runs on the client's frame clock instead of `Date.now()`, starts over at an INIT, and takes
  no samples from the player sync's server time.
- **Model parts** (`aid`) are hidden collision boxes. `playAnim` / `stopAnim` / `setAnimSpeed` on a client
  (playPartClip) return false.
- **Pitch and roll** of a body turn its part meshes but not their offsets.

## 7. Not done

- Shots against entity parts: a shot stops at a solid part on both sides as at any block, but it is not 10.0.3's
  rayVsPart (:214014: the part's yawed box at its part pose, nd riders passing through) and it does not damage the
  entity (`entities.damage` from a hit, :196082).
- The live assets' rayProxy (shots against a cpx asset, :212244).
- On the bridge, the `l` slot 26 assumes a movement scheme without sendVals: the bridge sends none.

## Tests

- `shared/tests/ks_game_v1003_test.c` (`test_entity_replication`) runs a server Game and a client Game. It covers:
  - the joiner's seed and `"e+"`;
  - client steps matching the server's, ctx.rand included, with no baseline;
  - the client hooks;
  - the replay of a baseline the client is 10 ticks ahead of;
  - attachPlayer's pin and pinAllServer, nd, `"ea+"`;
  - the client rider;
  - a controlled step predicted on the client exactly as the server runs it, broadcastControlled, the `"l"` tail;
  - detachPlayer, `"ea-"`;
  - addAsset's `"lo"` / `"slo"`;
  - the despawn's `"e-"` and onEntityRemoved.
- `server/tests/ks_server_test.c` (`test_entities`) covers the server host over the sandbox with EXT clients: the
  joiner's state after its HELLO, the spawn / damage / destroyed / hop records, the baselines, `"ea+"`, the pinned
  rider, the asset's `"lo"`, and a late joiner's seed / lo / e+ / ea+.
- `ks_game_v1003_test` (`test_entity_bodies`) covers a rider carried on the server's body and predicted the same on
  the client, addCube's record and solid block, a player landing on it, delete(), the asset's triangles on both
  sides, and a new round taking the server's objects out. `v10_translate_test` and `v10_bridge_test` cover the bridge.
- Headless offline run: a map with a server script, a shared script and a client script. The server spawns a
  second entity, attaches the player and adds asset 46802. The probe log shows both entities stepping and the
  player pinned at its entity plus (0, 1, 0). It also shows the client's own `"lap"` events and the server's, and the
  live asset drawn.
