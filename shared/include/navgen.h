#pragma once
// Krunker v7.1.4 generated nav mesh: a C port of shared/navgen.ts (buildGeneratedNavMesh,
// findGeneratedNavPath, resolveGeneratedNavArea) and the map extraction it uses from
// shared/navmesh.ts (extractBoxes, extractLadders, DEFAULT_NAVMESH_CONFIG).
//
// Source-engine style: walkable sample nodes flood-filled on a 12-unit grid from the spawn
// points (traceAdjacentNode: target ground, step / jump / death-drop limits, obstacle height,
// body clearance), merged greedily into rectangular nav areas (largest first, 50 x 50 nodes
// down to 1 x 1, then point areas for the rest), area connections from the node links, area
// links with a traversal kind and cost (walk / jump / drop / crouch / ladder), and ladder
// links between the areas at a ladder's foot and top.
//
// Pure data: input is the map JSON exactly as v7 reads it (objects p / i / si / s / d / l /
// col / nai, the xyz size pool, spawns), no Game or engine state, so the server, the offline
// game and the client (the show_navmeshes overlay, client/src/nav_overlay.c) build the same
// mesh. Arithmetic is in double like the JS, and the result (node, area and link order
// included) matches v7's for the same map; shared/tests/navgen_test.c checks the counts
// against v7's own output.
//
// KrunkNative differences (documented in navgen.c): a spatial hash over the boxes and run
// length pre-checks in the area merge make the build fast enough for the big maps (v7 takes
// 7 to 22 s in the browser on Lagoon / Little Town / Shipwrecked); both are exact, they only
// skip work whose answer is known.
#include <cJSON.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    double x, y, z;
} NavGenPoint;

typedef enum { NAVGEN_NORTH, NAVGEN_EAST, NAVGEN_SOUTH, NAVGEN_WEST, NAVGEN_DIRS } NavGenDir;
typedef enum { NAVGEN_NW, NAVGEN_NE, NAVGEN_SE, NAVGEN_SW, NAVGEN_CORNERS } NavGenCorner;
typedef enum { NAVGEN_LINK_WALK, NAVGEN_LINK_JUMP, NAVGEN_LINK_DROP, NAVGEN_LINK_CROUCH, NAVGEN_LINK_LADDER } NavGenLinkKind;

// GeneratedNavArea.attrs bits
#define NAVGEN_ATTR_CROUCH 1
#define NAVGEN_ATTR_OBSTACLE 4

typedef struct {
    int32_t to;                     // node id, -1 = no link this way
    double obstacle_height, obstacle_start, obstacle_end;
} NavGenNodeLink;

typedef struct {
    double x, y, z;
    NavGenPoint normal;
    NavGenNodeLink links[NAVGEN_DIRS];
    uint8_t crouch;                 // bit per NavGenCorner
    uint8_t blocked;                // bit per NavGenCorner
    uint8_t covered;
    int32_t area;                   // area id, -1 = none
} NavGenNode;

typedef struct {
    int32_t *ids;
    int count, cap;
} NavGenIdList;

typedef struct {
    int id;
    NavGenPoint nw, ne, se, sw;
    int attrs;
    // connections[dir]: neighbouring area ids. order[] is the JS object's key order
    // (Object.values(area.connections) walks the directions in first-insertion order).
    NavGenIdList connections[NAVGEN_DIRS];
    uint8_t order[NAVGEN_DIRS];
    uint8_t order_count;
    int32_t nodes[NAVGEN_CORNERS];
} NavGenArea;

typedef struct {
    int32_t from, to;
    double distance, height_delta, obstacle_height, cost; // cost may be +INFINITY (forbidden)
    NavGenLinkKind kind;
} NavGenAreaLink;

typedef struct {
    int seeds, node_links, one_way_links, blocked_quadrants, crouch_quadrants, obstacle_links, isolated_nodes, uncovered_nodes;
} NavGenStats;

typedef struct NavGenMesh {
    NavGenNode *nodes;
    int node_count;
    NavGenArea *areas;
    int area_count;
    NavGenAreaLink *links;          // areaLinks, in insertion order
    int link_count;
    uint64_t *link_keys;            // open-addressing (from, to) -> links index + 1
    int32_t *link_slots;
    size_t link_slot_cap;
    double step;                    // generationStepSize (cellSize)
    double min_x, max_x, min_z, max_z; // bounds
    NavGenStats stats;
} NavGenMesh;

// buildGeneratedNavMesh(map) with the default config. NULL on OOM or when `map` is not an
// object; a map without walkable spawns gives an empty mesh (0 nodes, 0 areas).
NavGenMesh *navgen_build(const cJSON *map);
void navgen_free(NavGenMesh*);

// areaLinks[`${from}:${to}`], NULL when absent
const NavGenAreaLink *navgen_area_link(const NavGenMesh*, int from, int to);
int navgen_has_connection(const NavGenMesh*, int from, int to); // `to` is in one of from's lists
NavGenPoint navgen_area_center(const NavGenArea*);                // areaCenter (y on the area plane)

// resolveGeneratedNavArea: the area under / nearest to `p`, -1 none
int navgen_resolve_area(const NavGenMesh*, NavGenPoint p);

#define NAVGEN_PATH_MAX 256
typedef struct {
    int start_area, goal_area;
    NavGenPoint points[NAVGEN_PATH_MAX];
    NavGenLinkKind kinds[NAVGEN_PATH_MAX]; // segmentKinds: the kind used to reach points[i]
    int count;
    int area_count;                 // areaIds.length (the ids themselves are not kept)
    int reached_goal;
    double endpoint_distance;
} NavGenPath;

// findGeneratedNavPath(mesh, from, to) with the default cost (makeStaticPathCost()) and no
// blocked edges: 1 = a path (possibly not reaching the goal, see reached_goal), 0 = none.
// Paths longer than NAVGEN_PATH_MAX points are cut (count is the kept part).
int navgen_find_path(const NavGenMesh*, NavGenPoint from, NavGenPoint to, NavGenPath *out);

const char *navgen_link_kind_name(NavGenLinkKind);
