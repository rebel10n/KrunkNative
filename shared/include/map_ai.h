#pragma once
// WP-F (doc/v7/map-logic.md §2.8, §7 WP-F): v7.1.4 map AIs (AI_SPAWNER, 40 per map) and map bots
// (BOT_SPAWNER -> TFUE bots, 16 per map). Map.ai_spawners / Map.bot_spawners hold the raw records
// (key bags); Map.path_nodes / world_path_node() the PATH_NODEs; AI_REDIRECTION objects carry
// Object.redirection. Declared by WP-A; the bodies in shared/src/map_ai.c are stubs until WP-F
// replaces them (today shared/src/ai.c still spawns the v3-shaped AIs itself).
#include <shared.h>

void map_ai_spawn_pending(Game*);       // MapManager.addPendingAIs (sd / rsc honoured)
void map_bots_spawn_pending(Game*);     // addPendingBots (server, sandbox and pubs)
// trigger action 36 "Spawn AI" / zombies templates: an AI from a key bag at a position; its sid or -1.
int map_ai_spawn(Game*, const cJSON *data, vec3 position, int team);
void map_ai_force_target(Game*, int interface_id, Player *target); // action 70 "Force AI Target"
int map_bot_spawn_at_node(Game*, int node_id, int count);          // action 69 "Spawn Bot at Node"
