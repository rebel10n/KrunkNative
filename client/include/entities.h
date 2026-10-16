#pragma once
// v10 KrunkScript entities and runtime map objects on the client (client/src/entities.c): what an
// official krunker.io lobby (krunker_link_script.c) — or any server sending the same EXTs — streams:
//   entities ("e+" / "e-" / "es" / "ea+" / "ea-", init syncData ent / entAtt): bodies built from the
//     map's archetypes (Map.archetypes): one collidable / visible cube per solid part, placed at the
//     entity's pose (x, y, z, yaw + per-part skeleton keys), snapped to every baseline with v10's
//     100 ms visual blend; a hard attachment or standing on a part carries our own player;
//   runtime blocks ("lo" / "slo", init lo: KrunkScript GAME.OBJECTS.addBlock), up to 200;
//   moved dyn objects ("objmv", init chg: game.moveObject), riders carried;
//   the script's player locks ("plk": disableShooting / disableMelee).
// Every body part / block is a spawned map object appended after the map's own (map_post shows it).
#include <client.h>
#include <stdint.h>

// One EXT of those subtypes (NET_EXT_ENTITY_*, NET_EXT_PLAYER_LOCKS, NET_EXT_KN_OFFICIAL_LIVE_*,
// NET_EXT_KN_OFFICIAL_OBJECT_MOVED). 1 = it was one of them (handled or dropped).
int entities_on_ext(Client *client, uint16_t subtype, const uint8_t *body, uint16_t length);
// Per frame: the visual blends.
void entities_frame(Client *client, float delta);
// A new INIT, after the old map was unloaded: every entity, block and attachment is forgotten.
void entities_reset(void);
// One line for the input_script `script` probe: entity / block counts and the first entity's pose.
void entities_describe(const Client *client, char *out, size_t size);
