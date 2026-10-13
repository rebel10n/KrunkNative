// WP-A map lookups of shared/include/world.h (real bodies, not stubs).
#include <world.h>
#include <bots.h> // bot_object_blocks_player: the boxes that stop a player
#include <math.h>

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

// Segment origin -> origin + d against one triangle (Moller-Trumbore, both faces): t in (0, 1], else -1.
static float segment_triangle(const vec3 o, const vec3 d, const float *tri) {
    const vec3 a = {tri[0], tri[1], tri[2]};
    const vec3 e1 = {tri[3] - a.x, tri[4] - a.y, tri[5] - a.z};
    const vec3 e2 = {tri[6] - a.x, tri[7] - a.y, tri[8] - a.z};
    const vec3 p = {d.y * e2.z - d.z * e2.y, d.z * e2.x - d.x * e2.z, d.x * e2.y - d.y * e2.x};
    const float det = e1.x * p.x + e1.y * p.y + e1.z * p.z;
    if (fabsf(det) < 1e-9f) return -1.0f;
    const float inv = 1.0f / det;
    const vec3 s = {o.x - a.x, o.y - a.y, o.z - a.z};
    const float u = (s.x * p.x + s.y * p.y + s.z * p.z) * inv;
    if (u < 0.0f || u > 1.0f) return -1.0f;
    const vec3 q = {s.y * e1.z - s.z * e1.y, s.z * e1.x - s.x * e1.z, s.x * e1.y - s.y * e1.x};
    const float v = (d.x * q.x + d.y * q.y + d.z * q.z) * inv;
    if (v < 0.0f || u + v > 1.0f) return -1.0f;
    const float t = (e2.x * q.x + e2.y * q.y + e2.z * q.z) * inv;
    return t > 0.0f && t <= 1.0f ? t : -1.0f;
}

float world_solid_fraction(const Game *game, const vec3 origin, vec3 seg) {
    const Map *map = game ? game->map : NULL;
    if (!map) return 1.0f;
    const vec3 raw = seg;
    // line_in_rect divides by each component: no exact zero (as airdrop.c)
    if (seg.x == 0.0f) seg.x = 1e-6f;
    if (seg.y == 0.0f) seg.y = 1e-6f;
    if (seg.z == 0.0f) seg.z = 1e-6f;

    float t = 1.0f;
    for (size_t i = 0; i < map->object_count; i++) {
        const Object *o = map->objects[i];
        if (!bot_object_blocks_player(o) || o->complex_coll) continue;
        const float hit = line_in_rect(origin, seg, o->position, object_box_size(o));
        if (hit > 0.0f && hit < t) t = hit;
    }
    for (size_t i = 0; map->tris && i < map->tri_count; i++) {
        const float hit = segment_triangle(origin, raw, map->tris + i * 9);
        if (hit > 0.0f && hit < t) t = hit;
    }
    const float hit = world_terrain_fraction(game, origin, seg);
    if (hit > 0.0f && hit < t) t = hit;
    return t;
}
