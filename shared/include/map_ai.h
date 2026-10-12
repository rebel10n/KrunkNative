#pragma once
// WP-F (doc/v7/map-logic.md §2.8, §7 WP-F): v7.1.4 map AIs (AI_SPAWNER, 40 per map) and map bots
// (BOT_SPAWNER -> TFUE / NextBot players of shared/src/bots.c, 16 per map). Map.ai_spawners /
// Map.bot_spawners hold the raw records (key bags); Map.path_nodes the PATH_NODEs; AI_REDIRECTION
// objects carry Object.redirection. The AI half is in shared/src/ai.c (with the AI runtime), the
// bot half in shared/src/map_ai.c.
#include <shared.h>

void map_ai_spawn_pending(Game*);       // MapManager.addAI + addPendingAIs at game.init (sd / rsc honoured)
void map_bots_spawn_pending(Game*);     // addBot + addPendingBots (authority): last round's map bots leave first
// Zombies templates / KrunkScript: an AI from a key bag (AI_SPAWNER keys) at a position, past the
// 40 limit (zom.js addAI(..., true)). team >= 0 overrides "tm". Its sid or -1.
int map_ai_spawn(Game*, const cJSON *data, vec3 position, int team);
void map_ai_force_target(Game*, int interface_id, Player *target); // WP-A stub: forces the target's position

// Trigger actions (WP-C, shared/src/triggers_v7.c):
#define MAP_AI_HAVE_TRIGGER_API 1
// action 36 "Spawn AI": every pending map AI with in == interfaceT and != the source's interface.
int map_ai_spawn_interface(Game*, int source_interface, int interface_target);
// action 70 "Force AI Target": every AI chases `point` for 30 s (AI.forceTarget / forceTargetTimer).
void map_ai_force_target_point(Game*, vec3 point);
// action 39 "Play Animation", AI half: active AIs with in == interface_id play `clip` once ("ana" raw).
void map_ai_play_anim_interface(Game*, int interface_id, const char *clip);
// action 69 "Spawn Bot at Node": `count` "Uncle Joe" bots (class 4, 1000 HP, weapon 5) at PATH_NODE
// node_id. Returns how many were added.
int map_bot_spawn_at_node(Game*, int node_id, int count);
