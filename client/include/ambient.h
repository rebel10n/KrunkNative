#pragma once
// Krunker v3.1.1 ambient-occlusion planes (ambient_0 / ambient_1 mesh groups, the
// "ambientShading" setting), generated from the map's collision boxes. See ambient.c.
#include <client.h>

// After the map's object meshes were added: builds the AO planes (none with noTex) into
// two merged meshes owned by ambient.c and adds them to `scene`.
void ambient_load_map(Scene *scene, const Map *map);
// Removes the AO meshes from `scene` and frees them (GL buffers included).
void ambient_unload_map(Scene *scene);

// A v3.1.1 manager.objects collision box: centre and half extents (width, height, length).
typedef struct {
    double center[3];
    double half[3];
} AmbientBox;

// Every collision box the v3.1.1 map builders create for `map` (zones, ramps and ladders
// included; flags, pads and pickups left out). malloc'd, the caller frees it.
AmbientBox *ambient_map_boxes(const Map *map, size_t *count);
