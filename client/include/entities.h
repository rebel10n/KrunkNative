#pragma once
// v10 KrunkScript entities and runtime map objects on the client (client/src/entities.c): what an
// official krunker.io lobby (krunker_link_script.c) — or any server sending the same EXTs — streams:
//   entities ("e+" / "e-" / "es" / "ee" / "ea+" / "ea-", the "l" tail, init syncData entSeed / ent / entAtt):
//     the shared manager's replicated copy (shared/include/ks_entities.h), stepped between baselines by the
//     map's scripts.shared, and their bodies built from the map's archetypes (Map.archetypes): one collidable
//     / visible cube per solid part at the entity's pose (x, y, z, yaw + per-part skeleton keys), the meshes
//     at its render pose; standing on a part carries our own player;
//   runtime objects ("lo" / "slo", init lo: KrunkScript LIVE_OBJECTS addCube / addAsset), up to 200;
//   moved dyn objects ("objmv", init chg: game.moveObject), riders carried;
//   the script's player locks ("plk": disableShooting / disableMelee);
//   player poses ("pz", handed to player_pose_render.c) and every player's hard attachment (faceRide).
// Every body part / block is a spawned map object appended after the map's own (map_post shows it).
#include <client.h>
#include <stdint.h>

// One EXT of those subtypes (NET_EXT_ENTITY_*, NET_EXT_PLAYER_LOCKS, NET_EXT_PLAYER_POSE,
// NET_EXT_KN_OFFICIAL_LIVE_*, NET_EXT_KN_OFFICIAL_OBJECT_MOVED). 1 = it was one of them (handled or dropped).
// KN's own server sends the same records (shared/src/ks_game/ks_entities.c).
int entities_on_ext(Client *client, uint16_t subtype, const uint8_t *body, uint16_t length);
// Per frame, before the client script's update (ks_client_frame): v10 game.update's entity half on the shared
// manager's copy (ks_entities_client_frame: advance on the entity clock, the client script's entity hooks, the
// render poses, the other players' rides). `delta` is the frame's, in seconds.
void entities_update(Client *client, float delta);
// Per frame, before map_post: the bodies at their entities' poses (collision) and render poses (meshes).
void entities_frame(Client *client, float delta);
// A new INIT, after the old map was unloaded: every entity, block and attachment is forgotten (and the
// pending player poses, player_pose_reset).
void entities_reset(void);
// v10 player.attachedEnt: the yaw of the entity player `uid` is hard-attached to; 0 when it rides none
int entities_player_ride_yaw(int uid, float *yaw);
// One line for the input_script `script` probe: entity / block counts and the first entity's pose.
void entities_describe(const Client *client, char *out, size_t size);
