#pragma once
// map-logic WP-B (doc/v7/map-logic.md §1.8.1): the procedural terrain of v7.1.4 libs/Terrain.js
// (simplex noise seeded by alea, the four biomes, the colour / AO rules and the terrainTSeed
// objects), generated identically by the server, the offline sandbox, the client (prediction and
// rendering, client/src/terrain_mesh.c) and the map editor (client/src/editor/editor_terrain.c).
//
// Terrain space is Terrain.js's: x, y = -world z (both / sizeMlt 5.7), z = height. The mesh is
// rotated -PI/2 about x and scaled 5.7 in x and y, so a terrain vertex (x, y, h) is the world
// point (x * 5.7, h, -y * 5.7).
#include <shared.h>

#define TERRAIN_SIZE_MLT 5.7    // Terrain.js: sizeMlt is forced to 5.7
#define TERRAIN_SEGMENT 25.0    // cells are 25 terrain units
#define TERRAIN_EDGE_SIZE 500.0 // edgeSize

enum { TERRAIN_BIOME_GRASS, TERRAIN_BIOME_MOUNTAIN, TERRAIN_BIOME_SNOW, TERRAIN_BIOME_HILLS, TERRAIN_BIOME_COUNT };
enum { TERRAIN_COLOR_DIRT, TERRAIN_COLOR_GRASS, TERRAIN_COLOR_MOUNTAIN, TERRAIN_COLOR_SAND, TERRAIN_COLOR_SNOW, TERRAIN_COLOR_COUNT };
// MAP_OBJS. BUSH and GRASS are client-only in v7 (the server consumes their noise samples but does
// not keep them); TREE / STUMP / LOOT / ROCK are map objects on both sides (MapManager.addTerrObj).
enum { TERRAIN_OBJ_TREE, TERRAIN_OBJ_STUMP, TERRAIN_OBJ_LOOT, TERRAIN_OBJ_ROCK, TERRAIN_OBJ_BUSH, TERRAIN_OBJ_GRASS, TERRAIN_OBJ_COUNT };

typedef struct {
    float x, y, z; // world space: [x * 5.7, height, -y * 5.7]
    int biome;     // TERRAIN_BIOME_*
} TerrainObject;

typedef struct Terrain_t {
    double size_x, size_y;     // limitMM(terrainWidth / terrainHeight, 0, 8000)
    double mnt_mlt, size_mlt, edge_size;
    int ease;                  // index into Terrain.EASE_LIST (libs/easing.js order), 12 easeInOutQuint
    int gen_objs;              // terrainTSeed set
    uint8_t colors[TERRAIN_COLOR_COUNT][3];
    int seg_x, seg_y;          // xSegmentCount / ySegmentCount (floor(size / 25))
    double seg_w, seg_h;       // xSegmentSize / ySegmentSize
    // terrainHeights: (seg_x + 1) * (seg_y + 1), index x + y * (seg_x + 1)
    float *heights;
    // the non-indexed mesh: seg_x * seg_y cells * 6 vertices (2 triangles: a b c, c d a with
    // a = (x, y), b = (x + 1, y), c = (x + 1, y + 1), d = (x, y + 1)), 3 floats each, terrain space.
    // Counter-clockwise seen from +z (above) in terrain space, so front faces point up in the world.
    // Stable after generation.
    float *positions;
    uint8_t *vcolors;          // per vertex rgb: biome colour * (1 - calcAOWeight) truncated to bytes
    TerrainObject *objs[TERRAIN_OBJ_COUNT];
    int obj_count[TERRAIN_OBJ_COUNT], obj_cap[TERRAIN_OBJ_COUNT];
    // internal (generation state)
    uint8_t perm[2][512];
    int sample_index, sample_index_3d;
} Terrain;

// Terrain.fromConfig(map) from the raw map JSON (terrainSeed, terrainTSeed, terrainWidth,
// terrainHeight, terrainMntMlt, terrain*Col, terrainEase). Generates even with an empty seed (the
// editor shows nothing then); NULL when out of memory. map.js only builds a terrain when
// terrainSeed, terrainWidth and terrainHeight are all truthy: terrain_enabled_json().
Terrain *terrain_generate_json(const cJSON *map_json);
int terrain_enabled_json(const cJSON *map_json);
void terrain_free(Terrain*);

// Terrain.raycast(x, y, z, dx, dy, dz, normal) in terrain space (x, y not yet divided by 5.7, as
// the callers pass them): the closest hit within |d| along the cells the ray crosses, hit[] as
// v7 returns it (x, y multiplied back by 5.7, z = height); normal (nullable) is the hit triangle's
// normal in terrain space. 0 = no hit.
int terrain_raycast_local(const Terrain*, double x, double y, double z, double dx, double dy, double dz, double hit[3], double normal[3]);

// The terrain of a map (map.terrain), generated once and cached on the Map (freed by map_fini).
// NULL when the map has no terrain (map.js: terrainSeed && terrainWidth && terrainHeight).
const Terrain *map_terrain(const Map*);
void map_terrain_free(Map*);

// "#rgb" / "#rrggbb" -> bytes (Terrain.js vF83), `fallback` for an empty / NULL string.
void terrain_hex(const char *s, const char *fallback, uint8_t out[3]);
