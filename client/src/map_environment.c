// STUB (WP-A): client/include/map_environment.h until WP-G implements the v7 environment zones
// (doc/v7/map-logic.md §7 WP-G). WP-G replaces this file.
#include <map_environment.h>
#include <world.h>

int map_environment_update(const Map *map, const vec3 camera_position) {
    return world_environment_zone_at(map, camera_position);
}
