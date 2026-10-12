// Krunker v7.1.4 generated nav mesh (shared/src/navgen.c, v7 shared/navgen.ts) against v7's own
// output: node / area / connection / link counts, link kinds, stats and a spawn-to-spawn
// findGeneratedNavPath on Sandstorm and Kanji (the v3.1.1 maps and v7's), and on small maps for
// ladders, ramps, crouch space, crates, pooled sizes (xyz / si) and noCol objects; connectivity;
// determinism (two builds are identical); the build time of every official map. Regressions of
// the two v7 bugs KrunkNative fixes (navgen.c): "col": false objects and spawns snapped into walls
// gave empty meshes (show_navmeshes: "0 areas, 0 nodes"), checked over the whole catalogue.
//
// The expected numbers come from v7.1.4's navgen.ts run under node on the same JSON (esbuild
// bundle of buildGeneratedNavMesh + findGeneratedNavPath; the recipe is in doc/console.md
// "show_navmeshes"). Run from the repository root: ./<build>/navgen_test
#include <navgen.h>
#include <map_catalog.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int g_failures;

#define CHECK(cond, ...) do { \
    if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_failures++; } \
} while (0)

typedef struct {
    const char *name;
    int nodes, areas, conns, links, obstacle_links, node_links, blocked, crouch_quadrants;
    int walk, jump, drop, crouch, ladder;
    // spawns[0] -> spawns[last]: start / goal area, areaIds.length, points, reachedGoal, last point
    int has_path, start, goal, path_areas, points, reached;
    double last_x, last_y, last_z;
    const char *kinds;          // segmentKinds, comma separated
} Expect;

static double now_ms(void) { return (double) clock() * 1000.0 / CLOCKS_PER_SEC; }

static void kinds_string(const NavGenPath *p, char *out, const size_t n) {
    size_t len = 0;
    out[0] = 0;
    for (int i = 0; i < p->count && len < n; i++) len += (size_t) snprintf(out + len, n - len, "%s%s", i ? "," : "", navgen_link_kind_name(p->kinds[i]));
}

static int count_connections(const NavGenMesh *m) {
    int n = 0;
    for (int a = 0; a < m->area_count; a++) {
        for (int d = 0; d < NAVGEN_DIRS; d++) n += m->areas[a].connections[d].count;
    }
    return n;
}

// The largest set of areas joined by connections (either way)
static int largest_component(const NavGenMesh *m) {
    int *component = malloc((size_t) m->area_count * sizeof(int)), *stack = malloc((size_t) m->area_count * sizeof(int));
    int best = 0;
    for (int i = 0; i < m->area_count; i++) component[i] = -1;
    // undirected: connections plus their reverses, through the area links (every connection has one)
    for (int s = 0, c = 0; s < m->area_count; s++) {
        if (component[s] >= 0) continue;
        int top = 0, size = 0;
        stack[top++] = s;
        component[s] = c;
        while (top) {
            const int a = stack[--top];
            size++;
            for (int k = 0; k < m->link_count; k++) {
                const NavGenAreaLink *l = &m->links[k];
                const int other = l->from == a ? l->to : l->to == a ? l->from : -1;
                if (other < 0 || component[other] >= 0) continue;
                component[other] = c;
                stack[top++] = other;
            }
        }
        if (size > best) best = size;
        c++;
    }
    free(component);
    free(stack);
    return best;
}

static int meshes_equal(const NavGenMesh *a, const NavGenMesh *b) {
    if (a->node_count != b->node_count || a->area_count != b->area_count || a->link_count != b->link_count) return 0;
    for (int i = 0; i < a->node_count; i++) {
        const NavGenNode *x = &a->nodes[i], *y = &b->nodes[i];
        if (x->x != y->x || x->y != y->y || x->z != y->z || x->area != y->area || x->crouch != y->crouch || x->blocked != y->blocked) return 0;
        for (int d = 0; d < NAVGEN_DIRS; d++) {
            if (x->links[d].to != y->links[d].to || x->links[d].obstacle_height != y->links[d].obstacle_height) return 0;
        }
    }
    for (int i = 0; i < a->area_count; i++) {
        const NavGenArea *x = &a->areas[i], *y = &b->areas[i];
        if (memcmp(&x->nw, &y->nw, sizeof(NavGenPoint)) || memcmp(&x->se, &y->se, sizeof(NavGenPoint)) || x->attrs != y->attrs) return 0;
        if (x->order_count != y->order_count || memcmp(x->order, y->order, sizeof(x->order))) return 0;
        for (int d = 0; d < NAVGEN_DIRS; d++) {
            if (x->connections[d].count != y->connections[d].count) return 0;
            if (x->connections[d].count && memcmp(x->connections[d].ids, y->connections[d].ids, (size_t) x->connections[d].count * sizeof(int32_t))) return 0;
        }
    }
    for (int i = 0; i < a->link_count; i++) {
        const NavGenAreaLink *x = &a->links[i], *y = &b->links[i];
        if (x->from != y->from || x->to != y->to || x->kind != y->kind || (x->cost != y->cost && !(isinf(x->cost) && isinf(y->cost)))) return 0;
    }
    return 1;
}

static void check_map(const cJSON *json, const Expect *e) {
    printf("Running navgen test on %s...\n", e->name);
    CHECK(json, "%s: map JSON", e->name);
    if (!json) return;
    const double t0 = now_ms();
    NavGenMesh *m = navgen_build(json);
    const double ms = now_ms() - t0;
    CHECK(m, "%s: built", e->name);
    if (!m) return;
    printf("  %d nodes, %d areas, %d links in %.1f ms\n", m->node_count, m->area_count, m->link_count, ms);
    CHECK(m->node_count == e->nodes, "%s: nodes %d (v7 %d)", e->name, m->node_count, e->nodes);
    CHECK(m->area_count == e->areas, "%s: areas %d (v7 %d)", e->name, m->area_count, e->areas);
    CHECK(count_connections(m) == e->conns, "%s: connections %d (v7 %d)", e->name, count_connections(m), e->conns);
    CHECK(m->link_count == e->links, "%s: area links %d (v7 %d)", e->name, m->link_count, e->links);
    CHECK(m->stats.obstacle_links == e->obstacle_links, "%s: obstacle links %d (v7 %d)", e->name, m->stats.obstacle_links, e->obstacle_links);
    CHECK(m->stats.node_links == e->node_links, "%s: node links %d (v7 %d)", e->name, m->stats.node_links, e->node_links);
    CHECK(m->stats.blocked_quadrants == e->blocked, "%s: blocked quadrants %d (v7 %d)", e->name, m->stats.blocked_quadrants, e->blocked);
    CHECK(m->stats.crouch_quadrants == e->crouch_quadrants, "%s: crouch quadrants %d (v7 %d)", e->name, m->stats.crouch_quadrants, e->crouch_quadrants);
    CHECK(m->stats.uncovered_nodes == 0 && m->stats.one_way_links == 0, "%s: every node in an area, no one-way node link", e->name);
    int kinds[5] = {0};
    for (int i = 0; i < m->link_count; i++) kinds[m->links[i].kind]++;
    CHECK(kinds[NAVGEN_LINK_WALK] == e->walk && kinds[NAVGEN_LINK_JUMP] == e->jump && kinds[NAVGEN_LINK_DROP] == e->drop &&
          kinds[NAVGEN_LINK_CROUCH] == e->crouch && kinds[NAVGEN_LINK_LADDER] == e->ladder,
          "%s: link kinds walk %d jump %d drop %d crouch %d ladder %d (v7 %d %d %d %d %d)", e->name, kinds[0], kinds[1], kinds[2], kinds[3], kinds[4],
          e->walk, e->jump, e->drop, e->crouch, e->ladder);
    // every connection has an area link, and links point at real areas
    int missing = 0;
    for (int a = 0; a < m->area_count; a++) {
        for (int d = 0; d < NAVGEN_DIRS; d++) {
            for (int i = 0; i < m->areas[a].connections[d].count; i++) missing += !navgen_area_link(m, a, m->areas[a].connections[d].ids[i]);
        }
    }
    CHECK(!missing, "%s: %d connections without an area link", e->name, missing);
    const int largest = largest_component(m);
    printf("  largest connected set: %d of %d areas\n", largest, m->area_count);
    CHECK(m->area_count == 0 || largest * 2 > m->area_count, "%s: most areas connected (%d / %d)", e->name, largest, m->area_count);

    const cJSON *spawns = cJSON_GetObjectItem(json, "spawns");
    const int spawn_count = cJSON_GetArraySize(spawns);
    if (e->has_path && spawn_count >= 2) {
        const cJSON *a = cJSON_GetArrayItem(spawns, 0), *b = cJSON_GetArrayItem(spawns, spawn_count - 1);
        const NavGenPoint from = {cJSON_GetArrayItem(a, 0)->valuedouble, cJSON_GetArrayItem(a, 1)->valuedouble, cJSON_GetArrayItem(a, 2)->valuedouble};
        const NavGenPoint to = {cJSON_GetArrayItem(b, 0)->valuedouble, cJSON_GetArrayItem(b, 1)->valuedouble, cJSON_GetArrayItem(b, 2)->valuedouble};
        static NavGenPath path;
        const int found = navgen_find_path(m, from, to, &path);
        char kinds_text[2048];
        kinds_string(&path, kinds_text, sizeof(kinds_text));
        CHECK(found, "%s: a path between the first and last spawn", e->name);
        CHECK(path.start_area == e->start && path.goal_area == e->goal, "%s: path areas %d -> %d (v7 %d -> %d)", e->name, path.start_area, path.goal_area, e->start, e->goal);
        CHECK(path.area_count == e->path_areas && path.count == e->points, "%s: path %d areas, %d points (v7 %d, %d)", e->name, path.area_count, path.count, e->path_areas, e->points);
        CHECK(path.reached_goal == e->reached, "%s: reachedGoal %d (v7 %d)", e->name, path.reached_goal, e->reached);
        if (path.count) {
            const NavGenPoint last = path.points[path.count - 1];
            CHECK(fabs(last.x - e->last_x) < 1e-9 && fabs(last.y - e->last_y) < 1e-9 && fabs(last.z - e->last_z) < 1e-9, "%s: last point %.3f %.3f %.3f", e->name, last.x, last.y, last.z);
        }
        CHECK(!strcmp(kinds_text, e->kinds), "%s: segment kinds %s\n  (v7 %s)", e->name, kinds_text, e->kinds);
    }

    NavGenMesh *again = navgen_build(json);
    CHECK(again && meshes_equal(m, again), "%s: a second build is identical", e->name);
    navgen_free(again);
    navgen_free(m);
}

static const char *const SANDSTORM_KINDS = "walk,walk,walk,walk,walk,jump,walk,jump,walk,walk,jump,walk,jump,walk,walk,walk,walk,walk,walk,walk,walk,walk,walk,walk,walk,walk,walk,walk,walk,walk,walk,walk,walk,walk,walk,walk,walk,walk,walk";

static const Expect k_catalog[] = {
    {"311_sandstorm", 768, 497, 1678, 2444, 98, 2424, 569, 0, 2136, 154, 154, 0, 0,
     1, 141, 67, 38, 39, 1, -234, 48, 150, NULL},
    {"311_kanji", 1169, 638, 2672, 3858, 238, 4016, 736, 0, 3076, 389, 389, 0, 4,
     1, 284, 99, 14, 15, 1, -334, -140, -279, "walk,walk,walk,walk,walk,walk,ladder,walk,walk,walk,jump,walk,jump,walk,walk"},
    {"sandstorm_v3", 774, 507, 1736, 2532, 120, 2446, 579, 0, 2164, 184, 184, 0, 0,
     1, 77, 65, 40, 41, 1, -234, 48, 150, "walk,walk,walk,walk,walk,jump,walk,jump,walk,walk,jump,walk,jump,walk,walk,walk,walk,walk,walk,walk,walk,walk,walk,walk,walk,walk,walk,walk,walk,walk,walk,walk,walk,walk,walk,walk,walk,walk,walk,walk,walk"},
    {"kanji", 1170, 638, 2678, 3872, 240, 4022, 735, 0, 3084, 392, 392, 0, 4,
     1, 285, 99, 13, 14, 1, -334, -140, -279, "walk,walk,walk,walk,walk,ladder,walk,walk,walk,jump,walk,jump,walk,walk"},
};

// Small maps (shared/tests: generated by hand, numbers from v7's navgen.ts on the same JSON)
static const char k_floor[] = "{\"name\":\"nav_floor\",\"spawns\":[[0,0,0,0,0,0]],\"objects\":[{\"p\":[0,-10,0],\"s\":[240,10,240]}]}";
// ground, an upper floor reached by a ladder (3), a ramp (9) up to a ledge, a slab 7..11 over the
// ground (crouch space) and a crate (1, blocker only)
static const char k_layers[] =
    "{\"name\":\"nav_layers\",\"spawns\":[[0,0,0,0,0,0],[100,40,0,0,0,0]],\"objects\":["
    "{\"p\":[0,-10,0],\"s\":[200,10,120]},{\"p\":[100,30,0],\"s\":[60,10,60]},{\"i\":3,\"p\":[66,0,0],\"s\":[4,40,8],\"d\":1},"
    "{\"i\":9,\"p\":[-50,0,40],\"s\":[40,12,20],\"d\":0},{\"p\":[-20,0,40],\"s\":[20,12,20]},{\"p\":[0,7,-40],\"s\":[30,4,30]},"
    "{\"i\":1,\"p\":[40,0,-30],\"s\":[6,6,6]}]}";
// v7 pooled sizes (xyz + si), a noCol ("l") pillar and a d = 2 ramp
static const char k_pooled[] =
    "{\"name\":\"nav_pooled\",\"xyz\":[240,10,240,20,30,20,40,12,20],\"spawns\":[[0,0,0,0,0,0]],\"objects\":["
    "{\"p\":[0,-10,0],\"si\":0},{\"p\":[30,0,30],\"si\":1},{\"p\":[-30,0,-30],\"si\":1,\"l\":1},{\"i\":9,\"p\":[40,0,-60],\"si\":2,\"d\":2}]}";

static const Expect k_small[] = {
    {"floor", 441, 42, 120, 160, 0, 1680, 0, 0, 160, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, NULL},
    {"layers", 202, 84, 341, 488, 24, 714, 20, 12, 324, 46, 46, 70, 2,
     1, 4, 2, 5, 6, 0, 100, 0, 0, "walk,walk,walk,walk,walk,walk"},
    {"pooled", 437, 74, 318, 408, 10, 1656, 20, 0, 382, 13, 13, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, NULL},
};

int main(void) {
    for (size_t i = 0; i < sizeof(k_catalog) / sizeof(k_catalog[0]); i++) {
        Expect e = k_catalog[i];
        if (!e.kinds) e.kinds = SANDSTORM_KINDS;
        const int index = map_catalog_index_of_key(e.name);
        CHECK(index >= 0, "catalogue map %s (run from the repository root)", e.name);
        if (index >= 0) check_map(map_catalog_json((size_t) index), &e);
    }
    const char *const small[] = {k_floor, k_layers, k_pooled};
    for (size_t i = 0; i < sizeof(small) / sizeof(small[0]); i++) {
        cJSON *json = cJSON_Parse(small[i]);
        check_map(json, &k_small[i]);
        cJSON_Delete(json);
    }

    // Regressions (show_navmeshes printed "0 areas, 0 nodes" and drew nothing):
    // 1. collision flags by the engine's truthiness: "col": false / "l": 0 objects collide (map.js
    //    noCol: v.l || v.col), "nai" counts when present (noAICol: v.nai != null)
    static const char *const flag_maps[][2] = {
        {"{\"spawns\":[[0,0,0,0,0,0]],\"objects\":[{\"p\":[0,-10,0],\"s\":[240,10,240],\"col\":false}]}", "441"},
        {"{\"spawns\":[[0,0,0,0,0,0]],\"objects\":[{\"p\":[0,-10,0],\"s\":[240,10,240],\"l\":0,\"col\":\"\"}]}", "441"},
        {"{\"spawns\":[[0,0,0,0,0,0]],\"objects\":[{\"p\":[0,-10,0],\"s\":[240,10,240],\"col\":true}]}", "0"},
        {"{\"spawns\":[[0,0,0,0,0,0]],\"objects\":[{\"p\":[0,-10,0],\"s\":[240,10,240],\"l\":1}]}", "0"},
        {"{\"spawns\":[[0,0,0,0,0,0]],\"objects\":[{\"p\":[0,-10,0],\"s\":[240,10,240],\"nai\":0}]}", "0"},
    };
    for (size_t i = 0; i < sizeof(flag_maps) / sizeof(flag_maps[0]); i++) {
        cJSON *json = cJSON_Parse(flag_maps[i][0]);
        NavGenMesh *fm = navgen_build(json);
        CHECK(fm && fm->node_count == atoi(flag_maps[i][1]), "collision flags %zu: %d nodes (want %s)", i, fm ? fm->node_count : -1, flag_maps[i][1]);
        navgen_free(fm);
        cJSON_Delete(json);
    }
    // 2. a spawn whose snapped grid point is inside a wall's body radius still seeds the mesh
    //    (v7: no seed, an empty mesh on Industry); the wall at x 4..8 is within 5.2 of x = 0
    cJSON *walled = cJSON_Parse("{\"spawns\":[[2,0,0,0,0,0]],\"objects\":[{\"p\":[0,-10,0],\"s\":[240,10,240]},{\"p\":[6,0,0],\"s\":[4,20,240]}]}");
    NavGenMesh *wm = navgen_build(walled);
    CHECK(wm && wm->node_count > 100 && wm->area_count > 0, "walled spawn seeds (%d nodes)", wm ? wm->node_count : -1);
    navgen_free(wm);
    cJSON_Delete(walled);
    // 3. every catalogue map (what the client builds from: Map.raw_data is the catalogue JSON) has
    //    a mesh, but ESP_Parkour, whose spawn stands on non-colliding ("l": 1) objects only
    int unexpected = 0;
    for (size_t i = 0; i < map_catalog_count(); i++) {
        const cJSON *json = map_catalog_json(i);
        const MapCatalogEntry *entry = map_catalog_at(i);
        if (!json || !entry) continue;
        NavGenMesh *cm = navgen_build(json);
        const int nodes = cm ? cm->node_count : -1;
        navgen_free(cm);
        if (nodes > 0) continue;
        if (!strcmp(entry->key, "ESP_Parkour")) continue;
        CHECK(0, "%s: empty nav mesh", entry->key);
        unexpected++;
    }
    printf("Catalogue: %zu maps, %d unexpectedly empty\n", map_catalog_count(), unexpected);
    static const Expect k_industry[] = {
        {"industry", 1061, 564, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, NULL},
        {"311_industry", 1061, 564, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, NULL},
    };
    for (size_t i = 0; i < 2; i++) {
        const int index = map_catalog_index_of_key(k_industry[i].name);
        CHECK(index >= 0, "catalogue map %s", k_industry[i].name);
        NavGenMesh *im = index >= 0 ? navgen_build(map_catalog_json((size_t) index)) : NULL;
        CHECK(im && im->node_count == k_industry[i].nodes && im->area_count == k_industry[i].areas, "%s: %d nodes, %d areas (v7: 0, 0)",
              k_industry[i].name, im ? im->node_count : -1, im ? im->area_count : -1);
        navgen_free(im);
    }

    // floor: one 20 x 20 area covers the middle (createNavAreasFromNodes, largest first)
    cJSON *floor = cJSON_Parse(k_floor);
    NavGenMesh *m = navgen_build(floor);
    CHECK(m && m->area_count && m->areas[0].nw.x == -120.0 && m->areas[0].se.x == 120.0 && m->areas[0].se.z == 120.0, "floor: area 0 spans -120..120");
    CHECK(m && navgen_resolve_area(m, (NavGenPoint) {0, 0, 0}) == 0, "floor: the centre resolves to area 0");
    CHECK(m && navgen_resolve_area(m, (NavGenPoint) {0, 500, 0}) < 0, "floor: nothing 500 above it");
    navgen_free(m);
    cJSON_Delete(floor);

    // empty / malformed maps
    cJSON *empty = cJSON_Parse("{\"objects\":[],\"spawns\":[]}");
    m = navgen_build(empty);
    CHECK(m && m->node_count == 0 && m->area_count == 0, "empty map: an empty mesh");
    navgen_free(m);
    cJSON_Delete(empty);
    cJSON *odd = cJSON_Parse("{\"objects\":[{\"p\":[0,\"x\"]},{\"p\":5,\"s\":[1,1,1]},{\"i\":\"9\",\"p\":[0,0,0],\"s\":[10,1,10]}],\"spawns\":[[0],{}]}");
    m = navgen_build(odd);
    CHECK(m && m->area_count == 0, "malformed objects / spawns are skipped");
    navgen_free(m);
    cJSON_Delete(odd);
    CHECK(!navgen_build(NULL), "no map: NULL");

    // build time of every official map (the show_navmeshes overlay builds on a worker thread)
    double worst = 0.0;
    const char *worst_name = "";
    for (size_t i = 0; i < MAP_CATALOG_OFFICIAL_COUNT; i++) {
        const cJSON *json = map_catalog_json(i);
        if (!json) continue;
        const double t0 = now_ms();
        NavGenMesh *mesh = navgen_build(json);
        const double ms = now_ms() - t0;
        if (ms > worst) {
            worst = ms;
            worst_name = map_catalog_at(i)->key;
        }
        CHECK(mesh, "%s: built", map_catalog_at(i)->key);
        navgen_free(mesh);
    }
    printf("Slowest official map: %s, %.1f ms\n", worst_name, worst);
    CHECK(worst < 5000.0, "every official map builds in under 5 s (%s: %.1f ms)", worst_name, worst);

    if (g_failures) {
        printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("Test OK\n");
    return 0;
}
