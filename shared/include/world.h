#pragma once
// WP-B (doc/v7/map-logic.md §7 WP-B): the v7.1.4 world runtime: triangle collision, terrain,
// battle-royale zone, path-node object moves, object animations, builds. Declared by WP-A so the
// other packages can call it; the bodies in shared/src/world.c are stubs until WP-B lands
// (WP-B replaces them; nobody else edits them).
#include <shared.h>

// ---- zone (libs/Zone.js; Map.zone): server tick, "zn" sync, 10 damage/s outside, reset per round
void zone_reset(Game*);                          // resetZone (round start)
void zone_update(Game*, float delta);            // game.update #24: shrink, stages, damage
int zone_contains(const Game*, vec3 position);   // inside the current circle (1 when there is no zone)

// ---- terrain (libs/Terrain.js; Map.terrain)
// Downward / any-direction ray against the generated terrain: 1 and *distance on a hit.
int terrain_raycast(const Game*, vec3 origin, vec3 direction, float max_distance, float *distance);

// ---- object moves / animation (trigger actions 40-42, 39 / 63; "mo" / "mf" / "ana")
int world_teleport_object_to_node(Game*, Object*, int node_id); // 0 = no such node
void world_reset_object_position(Game*, Object*);
void world_play_anim(Game*, Object*, const char *clip, int raw);

// ---- implemented by WP-A (shared/src/world_lookup.c): pure map lookups
// manager.pathNodes[nid]: the first PATH_NODE with that "nid", NULL none.
const Object *world_path_node(const Map*, int node_id);
// The ENVIRONMENT_ZONE containing `position` (client camera; pointInBox3D over envZones, the
// first match wins), -1 = the map's own environment.
int world_environment_zone_at(const Map*, vec3 position);
