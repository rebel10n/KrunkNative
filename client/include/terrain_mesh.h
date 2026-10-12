#pragma once
// Krunker v7.1.4 client meshes of the map-B world data (client/src/terrain_mesh.c,
// client/src/zone_mesh.c): the procedural terrain and its objects (map_terrain, terrain.h) and
// the battle-royale zone cylinder (zone_circle, world.h).
#include <client.h>

void terrain_mesh_load(Scene *scene, const Map *map); // after the map's objects (map_post.c)
void terrain_mesh_unload(void);

void zone_mesh_load(Scene *scene, const Map *map);    // Zone.fromConfig(map, render) when map.zone
void zone_mesh_unload(void);
void zone_mesh_frame(const Game *game, float delta_ms); // the circle of zone_circle, texture scroll
