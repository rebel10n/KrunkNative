// STUB (WP-A): shared/include/world.h bodies until WP-B implements the v7 world runtime
// (doc/v7/map-logic.md §7 WP-B). WP-B replaces this file; the other packages only call it.
#include <world.h>

void zone_reset(Game *game) { (void) game; }

void zone_update(Game *game, const float delta) {
    (void) game;
    (void) delta;
}

int zone_contains(const Game *game, const vec3 position) {
    (void) game;
    (void) position;
    return 1;
}

int terrain_raycast(const Game *game, const vec3 origin, const vec3 direction, const float max_distance, float *distance) {
    (void) game;
    (void) origin;
    (void) direction;
    (void) max_distance;
    (void) distance;
    return 0;
}

int world_teleport_object_to_node(Game *game, Object *object, const int node_id) {
    (void) game;
    (void) object;
    (void) node_id;
    return 0;
}

void world_reset_object_position(Game *game, Object *object) {
    (void) game;
    (void) object;
}

void world_play_anim(Game *game, Object *object, const char *clip, const int raw) {
    (void) game;
    (void) object;
    (void) clip;
    (void) raw;
}
