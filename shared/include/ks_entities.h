// v10.0.3 KrunkScript entities beyond the authority's simulation (shared/src/ks_game/ks_entities.c): the
// replication, the client's copy that runs scripts.shared between baselines, the player attachments and
// LIVE_OBJECTS. Line numbers are deob-10.0.3.js (~/jails/kru/src/Krunker_v10.0.3/deob/game.deob.js); the
// entity manager is the class at :212465-214818, the live objects :212096-212411.
//
// Wire (NET_PACKET_EXT, NetExtJsonChunk + one JSON record each, shared/include/net_ext.h):
//   NET_EXT_ENTITY_SPAWN      "e+"  [eid, archId, tick, state, tag, mode, hp] (spawn, and a joiner's syncData.ent)
//   NET_EXT_ENTITY_REMOVE     "e-"  [eid, reason]
//   NET_EXT_ENTITY_STATE      "es"  [eid, tick, state] (one baseline each)
//   NET_EXT_ENTITY_EVENT      "ee"  [eid, name, a, b]
//   NET_EXT_ENTITY_ATTACH     "ea+" [sid, eid, {o, lk, ctl, soft, nd}] (and syncData.entAtt)
//   NET_EXT_ENTITY_DETACH     "ea-" [sid, eid]
//   NET_EXT_ENTITY_CONTROLLED "l"'s tail [eid, tick, state]: the entity the player controls, before its RECON
//   NET_EXT_ENTITY_SEED       syncData.entSeed [seed]: the match seed of ctx.rand
//   NET_EXT_KN_OFFICIAL_LIVE_OBJECT "lo" [id, data], NET_EXT_KN_OFFICIAL_LIVE_SYNC "slo" [id, "remove"]
// KN's server and an official lobby (client/src/net/krunker_link_script.c) send the same records; the
// client hands them to the functions below (client/src/entities.c), which keep the bodies.
#ifndef KS_ENTITIES_H
#define KS_ENTITIES_H

#include <shared.h>
#include <cJSON.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    double x, y, z, yaw, pitch, roll;
} KsEntityPose;

typedef struct {
    int eid;
    const MapArchetype *arch;
    const cJSON *state;         // the entity's state (wire keys)
    KsEntityPose pose;          // the committed pose (placeBody: the collision boxes)
    KsEntityPose render;        // updateVisuals' renderPose on a client (the meshes); the committed pose until then
    int frozen;
    int controller;             // the controlling player's sid, -1 none
    int part_count;             // solid parts (colObjs), in the archetype's order without the soft ones
} KsEntityView;

// ---------------------------------------------------------------- the authority (server, offline sandbox)

// What a joiner needs after its INIT (getSyncData's entSeed / lo / ent / entAtt, deob-10.0.3.js:220283-220292,
// :220401): the seed, every live object, every entity and every attachment, to `uid`.
void ks_entities_send_state(Game*, int uid);
// SYNC_PLAYER's tail (:222404-222405): the entity `player` controls as NET_EXT_ENTITY_CONTROLLED to `uid`, right
// before its RECON; nothing when it controls none.
void ks_entities_send_controlled(Game*, const Player *player, int uid);

// ---------------------------------------------------------------- a client's copy

void ks_entities_net_seed(Game*, const cJSON *rec);                    // entSeed: setup(sharedScript, seed)
void ks_entities_net_spawn(Game*, const cJSON *rec);                   // applySpawn (:212792)
void ks_entities_net_remove(Game*, const cJSON *rec);                  // applyRemove (:212826)
// applyBaselines (:214407) for one [eid, tick, state]; local_ms is the client's own clock (poseBuf times)
void ks_entities_net_baseline(Game*, const cJSON *rec, double local_ms);
void ks_entities_net_controlled(Game*, const cJSON *rec);              // applyControlledSync (:213849)
void ks_entities_net_event(Game*, const cJSON *rec);                   // "ee" -> clientEventQueue (:261732)
void ks_entities_net_attach(Game*, const cJSON *rec, int local_sid);   // applyAttach (:213548); local_sid: our uid, -1 none
void ks_entities_net_detach(Game*, const cJSON *rec);                  // applyDetach (:213579)
// game.update's client half (:222187-222216): advance(clock_ms) with the shared script, the client events into the
// client script (onEntitySpawn / onEntityRemoved / onEntityEvent), updateVisuals, then the riders of
// players.update (:191173-191183): the other players pinned to their entity's renderPose, soft riders glued.
// clock_ms is the entity clock (the server's round time), local_ms the client's own clock, delta_ms the frame.
void ks_entities_client_frame(Game*, double clock_ms, double local_ms, double delta_ms);
// A new INIT (client/src/entities.c entities_reset): the copy, its attachments and its queued client events go.
void ks_entities_client_reset(Game*);

// ---------------------------------------------------------------- both sides

int ks_entities_count(const Game*);
int ks_entities_view(const Game*, int index, KsEntityView *out);        // 0 past the end
int ks_entities_find(const Game*, int eid, KsEntityView *out);          // 0 when it is not there
// partPose (:212975) of solid part `part` (the archetype offset plus its skeleton keys): x y z yaw pitch roll
int ks_entities_part_pose(const Game*, int eid, int part, KsEntityPose *out, double size[3], int *is_model);
// The attachment of player `sid` (v10 attachments.get): eid, soft, ctl, lk, nd. 0 none.
int ks_entities_attachment(const Game*, int sid, int *eid, int *soft, int *ctl, int *lk, int *nd);
// procInputs' rider (:191612-191621): a hard-attached player steps the entity it controls (stepControlled with
// the input; `recon` on a replay), is pinned to it (pinPlayer) and keeps its old position. 1 when it rides one:
// the caller skips the acceleration and treats it as on the ground.
int ks_entities_player_ride(Game*, Player*, const Input*, int recon);
// shotPassesThrough (:214153): `shooter` rides entity `eid` with nd (no damage to it)
int ks_entities_shot_passes(const Game*, const Player *shooter, int eid);

#ifdef __cplusplus
}
#endif

#endif
