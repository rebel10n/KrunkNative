// STUB (WP-A): shared/include/map_ai.h bodies until WP-F implements the v7 map AI and map bots
// (doc/v7/map-logic.md §7 WP-F). WP-F replaces this file.
#include <map_ai.h>

void map_ai_spawn_pending(Game *game) { (void) game; }

void map_bots_spawn_pending(Game *game) { (void) game; }

int map_ai_spawn(Game *game, const cJSON *data, const vec3 position, const int team) {
    (void) game;
    (void) data;
    (void) position;
    (void) team;
    return -1;
}

void map_ai_force_target(Game *game, const int interface_id, Player *target) {
    (void) game;
    (void) interface_id;
    (void) target;
}

int map_bot_spawn_at_node(Game *game, const int node_id, const int count) {
    (void) game;
    (void) node_id;
    (void) count;
    return 0;
}
