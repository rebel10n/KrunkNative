// WP-A map lookups of shared/include/world.h (real bodies, not stubs).
#include <world.h>

const Object *world_path_node(const Map *map, const int node_id) {
    for (size_t i = 0; map && i < map->path_node_count; i++) {
        if (map->path_nodes[i].nid == node_id && map->path_nodes[i].object < map->object_count) {
            return map->objects[map->path_nodes[i].object];
        }
    }
    return NULL;
}

int world_environment_zone_at(const Map *map, const vec3 p) {
    for (size_t i = 0; map && i < map->env_zone_count; i++) {
        const MapEnvironmentZone *z = &map->env_zones[i];
        // MapManager.addEnvironmentZone: y = p.y + h / 2, half extents s / 2 (pointInBox3D)
        if (p.x >= z->position.x - z->size.x * 0.5f && p.x <= z->position.x + z->size.x * 0.5f &&
            p.y >= z->position.y && p.y <= z->position.y + z->size.y &&
            p.z >= z->position.z - z->size.z * 0.5f && p.z <= z->position.z + z->size.z * 0.5f) return (int) i;
    }
    return -1;
}
