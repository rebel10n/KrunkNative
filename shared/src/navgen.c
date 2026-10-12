// Krunker v7.1.4 generated nav mesh (shared/include/navgen.h): a port of shared/navgen.ts and
// the parts of shared/navmesh.ts it uses. Function names follow the TypeScript (camelCase
// there, snake_case here) so the two read side by side.
//
// Exactness: everything is double, Math.round is js_round (half up), Math.min / Math.max keep
// NaN like JS, and loops visit nodes / areas / boxes in the JS order wherever the order can
// change a result (ground ties, area merge, connection lists, A* ties).
//
// KrunkNative fixes of v7.1.4 bugs (decisions.md "v7 bugs: fix them"), which only change maps
// whose v7 mesh is empty (no official map carries a false collision flag; Industry is the only
// official map with no seed): collision flags by the engine's truthiness (is_non_colliding) and,
// when no spawn seeds, a seed next to a spawn whose snapped grid point is in a wall
// (find_seed_near_spawn).
//
// KrunkNative speed-ups (results unchanged):
//   - NgGrid: the solid boxes in a uniform spatial hash. A point query reads one bucket, whose
//     boxes are in ascending index order, so findGroundForNode's first-wins tie on equal tops
//     is kept; range queries (body clearance, obstacles) only compute an any / min / max, so
//     a box seen in two buckets changes nothing. Maps with a non-finite box fall back to a
//     full scan.
//   - createNavAreasFromNodes: testArea needs every cell of the candidate rectangle to be a
//     closed cell (obstacle-free for areas over 1 x 1) with the anchor's attrs. Those flags are
//     fixed after the flood fill, so their run lengths along the east links are computed once
//     (and shortened west of every node an area covers), and each row of a candidate is
//     checked in O(1) before the cell walk (which still checks coverage and planarity). v7 spends most of its 7 to 22 s on the big maps in these walks.
#include <navgen.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

// DEFAULT_NAVMESH_CONFIG (navmesh.ts) + buildGeneratedNavMesh's defaults (navgen.ts)
#define NG_CELL 12.0
#define NG_PLAYER_HEIGHT 11.0
#define NG_STEP 3.0
#define NG_RADIUS 5.2
#define NG_FLOOR_TOL 1.0
#define NG_PADDING 24.0
#define NG_JUMP 18.0
#define NG_LADDER_REACH 16.0
#define NG_MAX_AREA 50
#define NG_MAX_NODES 50000
#define NG_DEATH_DROP 240.0
#define NG_SLOPE_LIMIT 0.7
#define NG_JUMP_PENALTY 180.0
#define NG_DROP_PENALTY 120.0
#define NG_CROUCH_PENALTY 80.0
#define NG_LADDER_MOUNT 40.0 // LADDER_MOUNT_COST

// prefabs.js indices
#define NG_PREFAB_CUBE 0
#define NG_PREFAB_CRATE 1
#define NG_PREFAB_BARREL 2
#define NG_PREFAB_LADDER 3
#define NG_PREFAB_PLANE 4
#define NG_PREFAB_RAMP 9

static const int k_dir_dx[NAVGEN_DIRS] = {0, 1, 0, -1}; // DIR_DELTA: north (0, -1), east, south, west
static const int k_dir_dz[NAVGEN_DIRS] = {-1, 0, 1, 0};
static const int k_opposite[NAVGEN_DIRS] = {NAVGEN_SOUTH, NAVGEN_WEST, NAVGEN_NORTH, NAVGEN_EAST};

// ------------------------------------------------------------------------- JS number helpers

static double js_round(const double v) { // Math.round: half rounds up
    const double f = floor(v);
    return v - f >= 0.5 ? f + 1.0 : f;
}

static double js_min(const double a, const double b) { return isnan(a) || isnan(b) ? NAN : a < b ? a : b; }
static double js_max(const double a, const double b) { return isnan(a) || isnan(b) ? NAN : a > b ? a : b; }
static double js_clamp01(const double v) { return js_max(0.0, js_min(1.0, v)); }

static int js_present(const cJSON *o, const char *key) { // o.key != null
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return v && !cJSON_IsNull(v);
}

// Number(v) (undefined -> NaN)
static double js_number(const cJSON *v) {
    if (!v) return NAN;
    if (cJSON_IsNumber(v)) return v->valuedouble;
    if (cJSON_IsNull(v) || cJSON_IsFalse(v)) return 0.0;
    if (cJSON_IsTrue(v)) return 1.0;
    if (cJSON_IsString(v)) {
        const char *s = v->valuestring;
        while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
        if (!*s) return 0.0;
        char *end = NULL;
        const double d = strtod(s, &end);
        while (end && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) end++;
        return end && !*end ? d : NAN;
    }
    return NAN;
}

// spawn[i] / p[i] (undefined past the end or on a non-array)
static double js_index(const cJSON *array, const int i) { return cJSON_IsArray(array) ? js_number(cJSON_GetArrayItem(array, i)) : NAN; }

static double js_or0(const double v) { return isnan(v) || v == 0.0 ? 0.0 : v; } // `x || 0` on a number
static double snap_to_grid(const double value, const double step) { return js_round(value / step) * step; }

// ------------------------------------------------------------------------- map extraction

typedef struct {
    double min_x, max_x, min_z, max_z, base_y, top_y;
    unsigned char ramp, surface;
    double ramp_sx, ramp_sz, ramp_ex, ramp_ez;
} NgBox;

typedef struct {
    double min_x, max_x, min_z, max_z, base_y, top_y, dir;
} NgLadder;

// prefabOf: o.i ?? o.id ?? 0, as a number (a non-number prefab equals no prefab id)
static double prefab_of(const cJSON *o) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, "i");
    if (!v || cJSON_IsNull(v)) v = cJSON_GetObjectItemCaseSensitive(o, "id");
    if (!v || cJSON_IsNull(v)) return 0.0;
    return cJSON_IsNumber(v) ? v->valuedouble : NAN;
}

// JS truthiness of o.key
static int js_truthy(const cJSON *o, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!v || cJSON_IsNull(v) || cJSON_IsFalse(v)) return 0;
    if (cJSON_IsNumber(v)) return v->valuedouble != 0.0 && !isnan(v->valuedouble);
    if (cJSON_IsString(v)) return v->valuestring && v->valuestring[0];
    return 1; // true, arrays, objects
}

// isNonColliding. KrunkNative fix of a v7.1.4 bug: navmesh.ts tests `o.l != null || o.col !=
// null || o.nai != null`, so a collidable object written as "col": false (or "l": 0) is dropped,
// while the engine (map.js: noCol: v.l || v.col, noAICol: v.nai != null; KN map.c) keeps it. Older
// editor exports put "col": false on every object: v7's mesh of such a map (Bhop_VIP, ClashRoyale,
// ESP_Parkour, Slide_Neon_Stage, SniperRoyale2, Verb_Arena, zombie_high) is empty. Here l / col
// count by truthiness like the engine, nai by presence. The official maps carry no false flag:
// their meshes are unchanged.
static int is_non_colliding(const cJSON *o) { return js_truthy(o, "l") || js_truthy(o, "col") || js_present(o, "nai"); }

static void size_of(const cJSON *o, const cJSON *xyz, double out[3]) {
    const cJSON *si = cJSON_GetObjectItemCaseSensitive(o, "si");
    if (si && !cJSON_IsNull(si)) {
        const double b = js_number(si) * 3.0;
        for (int k = 0; k < 3; k++) {
            const double index = b + k;
            const cJSON *e = NULL;
            if (index >= 0.0 && index == floor(index) && cJSON_IsArray(xyz) && index < (double) cJSON_GetArraySize(xyz)) e = cJSON_GetArrayItem(xyz, (int) index);
            out[k] = e ? js_or0(js_number(e)) : 0.0; // xyz[b] || 0
        }
        return;
    }
    const cJSON *s = cJSON_GetObjectItemCaseSensitive(o, "s");
    if (cJSON_IsArray(s)) {
        for (int k = 0; k < 3; k++) out[k] = js_index(s, k);
        return;
    }
    out[0] = out[1] = out[2] = 0.0;
}

static void position_of(const cJSON *p, double out[3]) {
    for (int k = 0; k < 3; k++) out[k] = js_index(p, k);
}

// extractBoxes(map)
static NgBox *extract_boxes(const cJSON *map, int *count) {
    const cJSON *xyz = cJSON_GetObjectItemCaseSensitive(map, "xyz");
    const cJSON *objects = cJSON_GetObjectItemCaseSensitive(map, "objects");
    const int n = cJSON_IsArray(objects) ? cJSON_GetArraySize(objects) : 0;
    NgBox *boxes = calloc(n ? (size_t) n : 1, sizeof(NgBox));
    *count = 0;
    if (!boxes) return NULL;
    const cJSON *list = cJSON_IsArray(objects) ? objects : NULL, *o;
    cJSON_ArrayForEach(o, list) {
        const cJSON *p = cJSON_GetObjectItemCaseSensitive(o, "p");
        if (!cJSON_IsArray(p)) continue;
        if (is_non_colliding(o)) continue;
        const double prefab = prefab_of(o);
        const int ramp = prefab == NG_PREFAB_RAMP, plane = prefab == NG_PREFAB_PLANE;
        const int surface = prefab == NG_PREFAB_CUBE || plane || ramp;
        const int blocker_only = prefab == NG_PREFAB_CRATE || prefab == NG_PREFAB_BARREL;
        if (!surface && !blocker_only) continue;
        double s[3], pos[3];
        size_of(o, xyz, s);
        const double half_w = s[0] / 2.0, half_l = s[2] / 2.0;
        if (half_w <= 0.0 || half_l <= 0.0 || (!plane && s[1] <= 0.0)) continue;
        position_of(p, pos);
        const double height = plane ? js_max(s[1], 0.01) : s[1];
        const double dir = js_or0(js_number(cJSON_GetObjectItemCaseSensitive(o, "d"))) * M_PI / 2.0;
        const int rotated = dir != 0.0 && dir != M_PI;
        const double axis_half = (rotated ? s[2] : s[0]) / 2.0;
        NgBox *b = &boxes[(*count)++];
        b->min_x = pos[0] - half_w;
        b->max_x = pos[0] + half_w;
        b->min_z = pos[2] - half_l;
        b->max_z = pos[2] + half_l;
        b->base_y = pos[1];
        b->top_y = pos[1] + height;
        b->ramp = (unsigned char) ramp;
        b->surface = (unsigned char) surface;
        if (ramp) {
            b->ramp_sx = pos[0] - axis_half * cos(dir);
            b->ramp_sz = pos[2] - axis_half * sin(dir);
            b->ramp_ex = pos[0] + axis_half * cos(dir);
            b->ramp_ez = pos[2] + axis_half * sin(dir);
        }
    }
    return boxes;
}

// extractLadders(map, cfg)
static NgLadder *extract_ladders(const cJSON *map, int *count) {
    const cJSON *xyz = cJSON_GetObjectItemCaseSensitive(map, "xyz");
    const cJSON *objects = cJSON_GetObjectItemCaseSensitive(map, "objects");
    const int n = cJSON_IsArray(objects) ? cJSON_GetArraySize(objects) : 0;
    NgLadder *ladders = calloc(n ? (size_t) n : 1, sizeof(NgLadder));
    *count = 0;
    if (!ladders) return NULL;
    const cJSON *list = cJSON_IsArray(objects) ? objects : NULL, *o;
    cJSON_ArrayForEach(o, list) {
        const cJSON *p = cJSON_GetObjectItemCaseSensitive(o, "p");
        if (!cJSON_IsArray(p)) continue;
        if (is_non_colliding(o)) continue;
        if (prefab_of(o) != NG_PREFAB_LADDER) continue;
        double s[3], pos[3];
        size_of(o, xyz, s);
        if (s[0] <= 0.0 || s[2] <= 0.0 || s[1] <= 0.0) continue;
        position_of(p, pos);
        const double dir = js_or0(js_number(cJSON_GetObjectItemCaseSensitive(o, "d"))) * M_PI / 2.0;
        const double cx = pos[0] + 0.5 * cos(dir), cz = pos[2] + 0.5 * sin(dir);
        const double hx = s[0] / 2.0 + NG_LADDER_REACH, hz = s[2] / 2.0 + NG_LADDER_REACH;
        NgLadder *l = &ladders[(*count)++];
        l->min_x = cx - hx;
        l->max_x = cx + hx;
        l->min_z = cz - hz;
        l->max_z = cz + hz;
        l->base_y = pos[1];
        l->top_y = pos[1] + s[1];
        l->dir = dir;
    }
    return ladders;
}

// ------------------------------------------------------------------------- box spatial hash

#define NG_GRID_MAX 256

typedef struct {
    const NgBox *boxes;
    int count;
    int all;                         // full scans (no boxes, or a non-finite box)
    double x0, z0, cell;
    int w, h;
    int32_t *first;                  // w * h + 1 offsets into items
    int32_t *items;
} NgGrid;

static int grid_index(const NgGrid *g, const double v, const double origin, const int size) {
    const double f = floor((v - origin) / g->cell);
    if (!(f > 0.0)) return 0;        // NaN too (callers check finiteness first)
    return f >= (double) (size - 1) ? size - 1 : (int) f;
}

static int grid_init(NgGrid *g, const NgBox *boxes, const int count) {
    memset(g, 0, sizeof(*g));
    g->boxes = boxes;
    g->count = count;
    double x0 = INFINITY, x1 = -INFINITY, z0 = INFINITY, z1 = -INFINITY;
    for (int i = 0; i < count; i++) {
        const NgBox *b = &boxes[i];
        if (!isfinite(b->min_x) || !isfinite(b->max_x) || !isfinite(b->min_z) || !isfinite(b->max_z)) {
            g->all = 1;
            return 1;
        }
        x0 = fmin(x0, b->min_x);
        x1 = fmax(x1, b->max_x);
        z0 = fmin(z0, b->min_z);
        z1 = fmax(z1, b->max_z);
    }
    if (!count) {
        g->all = 1;
        return 1;
    }
    g->x0 = x0;
    g->z0 = z0;
    g->cell = fmax(32.0, fmax(x1 - x0, z1 - z0) / NG_GRID_MAX);
    g->w = (int) floor((x1 - x0) / g->cell) + 1;
    g->h = (int) floor((z1 - z0) / g->cell) + 1;
    if (g->w > NG_GRID_MAX + 1) g->w = NG_GRID_MAX + 1;
    if (g->h > NG_GRID_MAX + 1) g->h = NG_GRID_MAX + 1;
    const size_t cells = (size_t) g->w * (size_t) g->h;
    g->first = calloc(cells + 1, sizeof(int32_t));
    if (!g->first) return 0;
    for (int pass = 0; pass < 2; pass++) {
        int32_t *fill = NULL;
        if (pass == 1) {
            for (size_t k = 0; k < cells; k++) g->first[k + 1] += g->first[k];
            g->items = malloc((g->first[cells] ? (size_t) g->first[cells] : 1) * sizeof(int32_t));
            fill = calloc(cells, sizeof(int32_t));
            if (!g->items || !fill) {
                free(fill);
                return 0;
            }
        }
        for (int i = 0; i < count; i++) { // ascending box index per bucket
            const NgBox *b = &boxes[i];
            const int ax = grid_index(g, b->min_x, g->x0, g->w), bx = grid_index(g, b->max_x, g->x0, g->w);
            const int az = grid_index(g, b->min_z, g->z0, g->h), bz = grid_index(g, b->max_z, g->z0, g->h);
            for (int z = az; z <= bz; z++) {
                for (int x = ax; x <= bx; x++) {
                    const size_t k = (size_t) z * (size_t) g->w + (size_t) x;
                    if (pass == 0) g->first[k + 1]++;
                    else g->items[g->first[k] + fill[k]++] = i;
                }
            }
        }
        free(fill);
    }
    return 1;
}

static void grid_free(NgGrid *g) {
    free(g->first);
    free(g->items);
}

// The bucket range of [x0, x1] x [z0, z1]; 1 = scan every box instead.
static int grid_range(const NgGrid *g, const double x0, const double x1, const double z0, const double z1, int r[4]) {
    if (g->all || !isfinite(x0) || !isfinite(x1) || !isfinite(z0) || !isfinite(z1)) return 1;
    r[0] = grid_index(g, x0, g->x0, g->w);
    r[1] = grid_index(g, x1, g->x0, g->w);
    r[2] = grid_index(g, z0, g->z0, g->h);
    r[3] = grid_index(g, z1, g->z0, g->h);
    return 0;
}

// Runs the body with `box` for every box that may overlap the range (single bucket: ascending
// index order). `continue` skips a box, `return` leaves the enclosing function.
#define NG_FOR_BOXES(g, X0, X1, Z0, Z1, ...) do { \
    int r_[4]; \
    if (grid_range(g, X0, X1, Z0, Z1, r_)) { \
        for (int i_ = 0; i_ < (g)->count; i_++) { const NgBox *box = &(g)->boxes[i_]; __VA_ARGS__ } \
    } else { \
        for (int z_ = r_[2]; z_ <= r_[3]; z_++) for (int x_ = r_[0]; x_ <= r_[1]; x_++) { \
            const size_t k_ = (size_t) z_ * (size_t) (g)->w + (size_t) x_; \
            for (int j_ = (g)->first[k_]; j_ < (g)->first[k_ + 1]; j_++) { const NgBox *box = &(g)->boxes[(g)->items[j_]]; __VA_ARGS__ } \
        } \
    } \
} while (0)

// ------------------------------------------------------------------------- geometry

static double surface_y_at_generated(const NgBox *box, const double x, const double z) {
    if (box->ramp) {
        const double dx = box->ramp_ex - box->ramp_sx, dz = box->ramp_ez - box->ramp_sz;
        const double len_sq = dx * dx + dz * dz;
        const double t = len_sq <= 0.0 ? 0.0 : js_clamp01(((x - box->ramp_sx) * dx + (z - box->ramp_sz) * dz) / len_sq);
        return box->base_y + (box->top_y - box->base_y) * t;
    }
    return box->top_y;
}

static NavGenPoint ramp_normal(const NgBox *box) {
    if (!box->ramp) return (NavGenPoint) {0.0, 1.0, 0.0};
    const double dx = box->ramp_ex - box->ramp_sx, dz = box->ramp_ez - box->ramp_sz, dy = box->top_y - box->base_y;
    const double len = js_max(0.0001, hypot(dx, dz));
    const double slope = dy / len;
    const double nx = -(dx / len) * slope, nz = -(dz / len) * slope, ny = 1.0;
    const double nl = sqrt(nx * nx + ny * ny + nz * nz);
    return (NavGenPoint) {nx / nl, ny / nl, nz / nl};
}

static int is_body_clear_at(const NgGrid *g, const NavGenPoint p, const double height) {
    const double top = p.y + js_max(1.0, height);
    const double r = NG_RADIUS;
    NG_FOR_BOXES(g, p.x - r, p.x + r, p.z - r, p.z + r, {
        if (box->ramp) continue;
        if (p.x < box->min_x - r || p.x > box->max_x + r) continue;
        if (p.z < box->min_z - r || p.z > box->max_z + r) continue;
        if (box->base_y < top - NG_FLOOR_TOL && box->top_y > p.y + NG_FLOOR_TOL) return 0;
    });
    return 1;
}

typedef struct {
    NavGenPoint point, normal;
} NgGround;

// findGroundForNode
static int find_ground_for_node(const NgGrid *g, const NavGenPoint p, NgGround *out) {
    int found = 0;
    double best_y = 0.0;
    NavGenPoint best_normal = {0.0, 1.0, 0.0};
    NG_FOR_BOXES(g, p.x, p.x, p.z, p.z, {
        if (!box->surface) continue;
        if (p.x < box->min_x || p.x > box->max_x || p.z < box->min_z || p.z > box->max_z) continue;
        const double y = surface_y_at_generated(box, p.x, p.z);
        if (y > p.y + NG_JUMP + NG_STEP) continue;
        if (!found || y > best_y) {
            found = 1;
            best_y = y;
            best_normal = ramp_normal(box);
        }
    });
    if (!found) return 0;
    const NavGenPoint grounded = {p.x, best_y, p.z};
    if (!is_body_clear_at(g, grounded, NG_PLAYER_HEIGHT)) return 0;
    if (best_normal.y < NG_SLOPE_LIMIT) return 0;
    out->point = grounded;
    out->normal = best_normal;
    return 1;
}

static int has_clear_body_line(const NgGrid *g, const NavGenPoint from, const NavGenPoint to, const double lift) {
    const int samples = (int) js_max(1.0, ceil(hypot(to.x - from.x, to.z - from.z) / js_max(1.0, NG_CELL / 3.0)));
    for (int i = 0; i <= samples; i++) {
        const double t = (double) i / samples;
        const NavGenPoint p = {from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t + lift, from.z + (to.z - from.z) * t};
        if (!is_body_clear_at(g, p, NG_PLAYER_HEIGHT - lift)) return 0;
    }
    return 1;
}

// clipSegmentAabb2D
static int clip_segment_aabb_2d(const NavGenPoint from, const NavGenPoint to, const double min_x, const double min_z, const double max_x,
                                const double max_z, double *t0_out, double *t1_out) {
    double t0 = 0.0, t1 = 1.0;
    const double axes[2][4] = {{from.x, to.x - from.x, min_x, max_x}, {from.z, to.z - from.z, min_z, max_z}};
    for (int a = 0; a < 2; a++) {
        const double origin = axes[a][0], direction = axes[a][1], mn = axes[a][2], mx = axes[a][3];
        if (fabs(direction) < 1e-8) {
            if (origin < mn || origin > mx) return 0;
            continue;
        }
        double near = (mn - origin) / direction, far = (mx - origin) / direction;
        if (near > far) {
            const double t = near;
            near = far;
            far = t;
        }
        t0 = js_max(t0, near);
        t1 = js_min(t1, far);
        if (t0 > t1) return 0;
    }
    *t0_out = t0;
    *t1_out = t1;
    return 1;
}

typedef struct {
    double height, start, end;
} NgObstacle;

static NgObstacle obstacle_on_segment(const NgGrid *g, const NavGenPoint from, const NavGenPoint to) {
    double height = 0.0, start = NG_CELL, end = 0.0;
    const double dx = to.x - from.x, dz = to.z - from.z;
    const double len = js_max(0.0001, hypot(dx, dz));
    const double head_top = js_max(from.y, to.y) + NG_PLAYER_HEIGHT;
    const double r = NG_RADIUS;
    NG_FOR_BOXES(g, js_min(from.x, to.x) - r, js_max(from.x, to.x) + r, js_min(from.z, to.z) - r, js_max(from.z, to.z) + r, {
        if (box->ramp) continue;
        if (box->base_y > head_top + NG_FLOOR_TOL) continue;
        double t0, t1;
        if (!clip_segment_aabb_2d(from, to, box->min_x - r, box->min_z - r, box->max_x + r, box->max_z + r, &t0, &t1)) continue;
        const double climb = box->top_y - js_min(from.y, to.y);
        if (climb <= NG_STEP) continue;
        height = js_max(height, climb);
        start = js_min(start, t0 * len);
        end = js_max(end, t1 * len);
    });
    const int has = height != 0.0 && !isnan(height); // `height ?`
    return (NgObstacle) {height, has ? start : 0.0, has ? end : NG_CELL};
}

typedef struct {
    int ok;
    NavGenPoint end, normal;
    double obstacle_height, obstacle_start, obstacle_end;
} NgTrace;

static NgTrace trace_adjacent_node(const NgGrid *g, const NavGenPoint from, const NavGenPoint raw_target) {
    NgTrace fail = {0};
    NgGround ground;
    if (!find_ground_for_node(g, raw_target, &ground)) return fail;
    const NavGenPoint target = ground.point;
    const double dy = target.y - from.y;
    if (dy > NG_JUMP + NG_STEP) return fail;
    if (from.y - target.y > NG_DEATH_DROP) return fail;
    const NgObstacle obstacle = obstacle_on_segment(g, from, target);
    if (obstacle.height > NG_JUMP + NG_STEP) return fail;
    if (!has_clear_body_line(g, from, target, js_max(NG_STEP, obstacle.height))) return fail;
    return (NgTrace) {
        1, target, ground.normal,
        dy > NG_STEP ? js_max(dy, obstacle.height) : obstacle.height,
        obstacle.start, obstacle.end,
    };
}

// KrunkNative fix of a v7.1.4 bug: walkableSeeds snaps each spawn to the 12-unit grid and drops
// it when that point has no standing room, so a map whose spawns all sit within the body radius
// (5.2) of a wall after the snap gets no seed and an empty mesh (v7's show_navmeshes reports 0
// areas on Industry, both versions). When no spawn gives a seed, the grid points around each
// spawn (two rings) are tried, nearest to the spawn first, on ground within a jump of the spawn's
// height. Only then: a map where v7 seeds from some spawns (and misses others) keeps v7's mesh.
static int find_seed_near_spawn(const NgGrid *g, const cJSON *spawn, const NavGenPoint snapped, NgGround *out) {
    const double sx = js_index(spawn, 0), sz = js_index(spawn, 2);
    if (!isfinite(sx) || !isfinite(sz) || !isfinite(snapped.y)) return 0;
    int order[24][2];
    double dist[24];
    int n = 0;
    for (int dz = -2; dz <= 2; dz++) {
        for (int dx = -2; dx <= 2; dx++) {
            if (!dx && !dz) continue;
            const double x = snapped.x + dx * NG_CELL, z = snapped.z + dz * NG_CELL;
            const double d = (x - sx) * (x - sx) + (z - sz) * (z - sz);
            int i = n++;
            for (; i > 0 && dist[i - 1] > d; i--) { // insertion sort, stable: ties keep row order
                dist[i] = dist[i - 1];
                order[i][0] = order[i - 1][0];
                order[i][1] = order[i - 1][1];
            }
            dist[i] = d;
            order[i][0] = dx;
            order[i][1] = dz;
        }
    }
    for (int i = 0; i < n; i++) {
        const NavGenPoint p = {snapped.x + order[i][0] * NG_CELL, snapped.y, snapped.z + order[i][1] * NG_CELL};
        if (find_ground_for_node(g, p, out) && out->point.y >= snapped.y - (NG_JUMP + NG_STEP)) return 1;
    }
    return 0;
}

// ------------------------------------------------------------------------- nodes

typedef struct {
    NavGenNode *nodes;
    int count, cap;
    // nodeByKey: open addressing on (round(x / cell), round(y * 10), round(z / cell))
    int64_t *keys;                   // 3 per slot
    int32_t *slots;                  // node id + 1, 0 = empty
    size_t slot_cap;
} NgNodes;

static uint64_t hash3(const int64_t a, const int64_t b, const int64_t c) {
    uint64_t h = 1469598103934665603ULL;
    const int64_t v[3] = {a, b, c};
    for (int i = 0; i < 3; i++) {
        h ^= (uint64_t) v[i];
        h *= 1099511628211ULL;
        h ^= h >> 29;
    }
    return h;
}

// nodeKey: `${snapToGrid(x)},${Math.round(y * 10) / 10},${snapToGrid(z)}` (distinct keys are
// distinct integer triples; non-finite parts all hash to one sentinel like their strings would)
static void node_key(const NavGenPoint p, int64_t out[3]) {
    const double k[3] = {js_round(p.x / NG_CELL), js_round(p.y * 10.0), js_round(p.z / NG_CELL)};
    for (int i = 0; i < 3; i++) {
        if (isnan(k[i])) out[i] = INT64_MIN;
        else if (k[i] > 9e18) out[i] = INT64_MAX;
        else if (k[i] < -9e18) out[i] = INT64_MIN + 1;
        else out[i] = (int64_t) k[i];
    }
}

static int nodes_rehash(NgNodes *n, const size_t cap) {
    int64_t *keys = malloc(cap * 3 * sizeof(int64_t));
    int32_t *slots = calloc(cap, sizeof(int32_t));
    if (!keys || !slots) {
        free(keys);
        free(slots);
        return 0;
    }
    for (size_t i = 0; i < n->slot_cap; i++) {
        if (!n->slots[i]) continue;
        const int64_t *k = &n->keys[i * 3];
        size_t s = hash3(k[0], k[1], k[2]) & (cap - 1);
        while (slots[s]) s = (s + 1) & (cap - 1);
        slots[s] = n->slots[i];
        memcpy(&keys[s * 3], k, 3 * sizeof(int64_t));
    }
    free(n->keys);
    free(n->slots);
    n->keys = keys;
    n->slots = slots;
    n->slot_cap = cap;
    return 1;
}

static void check_node_crouch_and_blocked(const NgGrid *g, NavGenNode *node) {
    for (int c = 0; c < NAVGEN_CORNERS; c++) {
        const double size = NG_CELL / 2.0; // cornerOffset(corner, cellSize / 2)
        const double ox = c == NAVGEN_NE || c == NAVGEN_SE ? size : -size;
        const double oz = c == NAVGEN_SW || c == NAVGEN_SE ? size : -size;
        const NavGenPoint p = {node->x + ox, node->y, node->z + oz};
        if (!is_body_clear_at(g, p, NG_PLAYER_HEIGHT)) {
            const int crouch = is_body_clear_at(g, p, js_max(1.0, NG_PLAYER_HEIGHT * 0.55));
            if (crouch) node->crouch |= (uint8_t) (1u << c);
            else node->blocked |= (uint8_t) (1u << c);
        }
    }
}

// addNode: the node at the point's key (existing or new). -1 on OOM.
static int add_node(NgNodes *n, const NgGrid *g, const NavGenPoint p, const NavGenPoint normal) {
    int64_t key[3];
    node_key(p, key);
    if (n->slot_cap && (size_t) (n->count + 1) * 2 > n->slot_cap && !nodes_rehash(n, n->slot_cap * 2)) return -1;
    size_t s = hash3(key[0], key[1], key[2]) & (n->slot_cap - 1);
    while (n->slots[s]) {
        const int64_t *k = &n->keys[s * 3];
        if (k[0] == key[0] && k[1] == key[1] && k[2] == key[2]) return n->slots[s] - 1;
        s = (s + 1) & (n->slot_cap - 1);
    }
    if (n->count == n->cap) {
        const int cap = n->cap ? n->cap * 2 : 1024;
        NavGenNode *grown = realloc(n->nodes, (size_t) cap * sizeof(NavGenNode));
        if (!grown) return -1;
        n->nodes = grown;
        n->cap = cap;
    }
    NavGenNode *node = &n->nodes[n->count];
    memset(node, 0, sizeof(*node));
    node->x = p.x;
    node->y = p.y;
    node->z = p.z;
    node->normal = normal;
    for (int d = 0; d < NAVGEN_DIRS; d++) node->links[d].to = -1;
    node->area = -1;
    check_node_crouch_and_blocked(g, node);
    n->slots[s] = n->count + 1;
    memcpy(&n->keys[s * 3], key, 3 * sizeof(int64_t));
    return n->count++;
}

static void connect_nodes(NavGenNode *from, const int to, const int dir, const double height, const double start, const double end) {
    from->links[dir].to = to;
    from->links[dir].obstacle_height = js_max(0.0, height);
    from->links[dir].obstacle_start = js_max(0.0, js_min(NG_CELL, start));
    from->links[dir].obstacle_end = js_max(0.0, js_min(NG_CELL, end));
}

static int has_internal_obstacle(const NavGenNode *node) {
    for (int d = 0; d < NAVGEN_DIRS; d++) {
        if (node->links[d].to >= 0 && node->links[d].obstacle_height > NG_STEP) return 1;
    }
    return 0;
}

static int node_attrs(const NavGenNode *node) { return node->crouch ? NAVGEN_ATTR_CROUCH : 0; }

static int walk(const NavGenNode *nodes, int id, const int dir, const int count) {
    for (int i = 0; i < count; i++) {
        if (id < 0 || nodes[id].links[dir].to < 0) return -1;
        id = nodes[id].links[dir].to;
    }
    return id;
}

static int is_closed_cell(const NavGenNode *nodes, const int id) {
    const NavGenNode *node = &nodes[id];
    const int east = node->links[NAVGEN_EAST].to, south = node->links[NAVGEN_SOUTH].to;
    if (east < 0 || south < 0) return 0;
    const int east_south = nodes[east].links[NAVGEN_SOUTH].to, south_east = nodes[south].links[NAVGEN_EAST].to;
    return east_south >= 0 && south_east >= 0 && east_south == south_east && nodes[east].links[NAVGEN_WEST].to == id &&
           nodes[south].links[NAVGEN_NORTH].to == id;
}

// ------------------------------------------------------------------------- areas

static int id_list_push_unique(NavGenIdList *l, const int32_t id) {
    for (int i = 0; i < l->count; i++) if (l->ids[i] == id) return 1;
    if (l->count == l->cap) {
        const int cap = l->cap ? l->cap * 2 : 4;
        int32_t *grown = realloc(l->ids, (size_t) cap * sizeof(int32_t));
        if (!grown) return 0;
        l->ids = grown;
        l->cap = cap;
    }
    l->ids[l->count++] = id;
    return 1;
}

// `(area.connections[dir] ||= []).push(id)` unless included
static int area_connect(NavGenArea *area, const int dir, const int32_t id) {
    int known = 0;
    for (int i = 0; i < area->order_count; i++) known |= area->order[i] == dir;
    if (!known) area->order[area->order_count++] = (uint8_t) dir;
    return id_list_push_unique(&area->connections[dir], id);
}

typedef struct {
    NavGenArea *areas;
    int count, cap;
} NgAreas;

static NavGenArea *areas_push(NgAreas *a) {
    if (a->count == a->cap) {
        const int cap = a->cap ? a->cap * 2 : 256;
        NavGenArea *grown = realloc(a->areas, (size_t) cap * sizeof(NavGenArea));
        if (!grown) return NULL;
        a->areas = grown;
        a->cap = cap;
    }
    NavGenArea *area = &a->areas[a->count];
    memset(area, 0, sizeof(*area));
    area->id = a->count++;
    return area;
}

static NavGenPoint point_of(const NavGenNode *n) { return (NavGenPoint) {n->x, n->y, n->z}; }

// testArea(node, width, height, nodes). `ok` is isClosedCell (closed[]) and, for areas over
// 1 x 1, also !hasInternalObstacle: both are fixed once the flood fill is done.
static int test_area(const NavGenNode *nodes, const uint8_t *ok, const int id, const int width, const int height) {
    const NavGenNode *node = &nodes[id];
    if (node->blocked & (1u << NAVGEN_SE)) return 0;
    const int attrs = node_attrs(node);
    const NavGenPoint normal = node->normal;
    const double d = -(normal.x * node->x + normal.y * node->y + normal.z * node->z);
    for (int y = 0; y <= height; y++) {
        int row = y == 0 ? id : walk(nodes, id, NAVGEN_SOUTH, y);
        if (row < 0) return 0;
        for (int x = 0; x <= width; x++) {
            if (row < 0) return 0;
            const NavGenNode *r = &nodes[row];
            if (x < width && y < height) {
                if (r->covered || !ok[row]) return 0;
                if (node_attrs(r) != attrs) return 0;
                if (fabs(normal.x * r->x + normal.y * r->y + normal.z * r->z + d) > 5.0) return 0;
            }
            row = r->links[NAVGEN_EAST].to;
        }
    }
    return 1;
}

static NavGenArea *build_area(NavGenNode *nodes, NgAreas *areas, const int id, const int width, const int height) {
    NavGenArea *area = areas_push(areas);
    if (!area) return NULL;
    const int ne = walk(nodes, id, NAVGEN_EAST, width);
    const int sw = walk(nodes, id, NAVGEN_SOUTH, height);
    const int se = walk(nodes, ne, NAVGEN_SOUTH, height);
    for (int y = 0; y < height; y++) {
        int row = y == 0 ? id : walk(nodes, id, NAVGEN_SOUTH, y);
        for (int x = 0; x < width; x++) {
            nodes[row].covered = 1;
            nodes[row].area = area->id;
            row = nodes[row].links[NAVGEN_EAST].to;
        }
    }
    area->nw = point_of(&nodes[id]);
    area->ne = point_of(&nodes[ne]);
    area->se = point_of(&nodes[se]);
    area->sw = point_of(&nodes[sw]);
    area->attrs = node_attrs(&nodes[id]) | (has_internal_obstacle(&nodes[id]) ? NAVGEN_ATTR_OBSTACLE : 0);
    area->nodes[NAVGEN_NW] = id;
    area->nodes[NAVGEN_NE] = ne;
    area->nodes[NAVGEN_SE] = se;
    area->nodes[NAVGEN_SW] = sw;
    return area;
}

static NavGenArea *build_point_area(NavGenNode *nodes, NgAreas *areas, const int id) {
    NavGenArea *area = areas_push(areas);
    if (!area) return NULL;
    NavGenNode *node = &nodes[id];
    const double half = NG_CELL * 0.35;
    node->covered = 1;
    node->area = area->id;
    area->nw = (NavGenPoint) {node->x - half, node->y, node->z - half};
    area->ne = (NavGenPoint) {node->x + half, node->y, node->z - half};
    area->se = (NavGenPoint) {node->x + half, node->y, node->z + half};
    area->sw = (NavGenPoint) {node->x - half, node->y, node->z + half};
    area->attrs = node_attrs(node) | (has_internal_obstacle(node) ? NAVGEN_ATTR_OBSTACLE : 0);
    for (int c = 0; c < NAVGEN_CORNERS; c++) area->nodes[c] = id;
    return area;
}

// Consecutive nodes from `id` along the east links that pass `ok` with id's attrs, capped
// at NG_MAX_AREA + 1.
static uint8_t run_east(const NavGenNode *nodes, const uint8_t *ok, int id) {
    const int attrs = node_attrs(&nodes[id]);
    int n = 0;
    while (id >= 0 && ok[id] && node_attrs(&nodes[id]) == attrs && n <= NG_MAX_AREA) {
        n++;
        id = nodes[id].links[NAVGEN_EAST].to;
    }
    return (uint8_t) n;
}

// testArea's necessary condition: every row of the candidate starts with `width` cells that
// pass `ok` with the anchor's attrs (the walk then only checks coverage, planarity and the
// far edge / corner nodes).
static int rows_can_fit(const NavGenNode *nodes, const uint8_t *run, int id, const int width, const int height) {
    const int attrs = node_attrs(&nodes[id]);
    for (int y = 0; y < height; y++) {
        if (id < 0 || run[id] < width || node_attrs(&nodes[id]) != attrs) return 0;
        id = nodes[id].links[NAVGEN_SOUTH].to;
    }
    return 1;
}

// A newly covered node ends every run through it: runs of the nodes west of it (whose east
// link leads here) shrink to their distance from it. Runs stay upper bounds either way.
static void runs_cover(const NavGenNode *nodes, uint8_t *run1, uint8_t *run2, const int id) {
    run1[id] = run2[id] = 0;
    int prev = id, m = nodes[id].links[NAVGEN_WEST].to;
    for (int k = 1; m >= 0 && k <= NG_MAX_AREA && nodes[m].links[NAVGEN_EAST].to == prev; k++) {
        if (run1[m] > k) run1[m] = (uint8_t) k;
        if (run2[m] > k) run2[m] = (uint8_t) k;
        prev = m;
        m = nodes[m].links[NAVGEN_WEST].to;
    }
}

// createNavAreasFromNodes(nodes, cfg)
static int create_nav_areas_from_nodes(NavGenNode *nodes, const int count, NgAreas *areas) {
    const size_t n = count > 0 ? (size_t) (unsigned) count : 1u;
    uint8_t *ok1 = malloc(n), *ok2 = malloc(n), *run1 = malloc(n), *run2 = malloc(n);
    if (!ok1 || !ok2 || !run1 || !run2) {
        free(ok1);
        free(ok2);
        free(run1);
        free(run2);
        return 0;
    }
    for (int i = 0; i < count; i++) {
        ok1[i] = (uint8_t) is_closed_cell(nodes, i);
        ok2[i] = (uint8_t) (ok1[i] && !has_internal_obstacle(&nodes[i]));
    }
    int max_run1 = 0, max_run2 = 0; // upper bounds for the whole merge (runs only shrink)
    for (int i = 0; i < count; i++) {
        run1[i] = run_east(nodes, ok1, i);
        run2[i] = run_east(nodes, ok2, i);
        if (run1[i] > max_run1) max_run1 = run1[i];
        if (run2[i] > max_run2) max_run2 = run2[i];
    }
    int ok = 1;
    long remaining = count;
    for (int w = NG_MAX_AREA, h = NG_MAX_AREA; ok && remaining > 0 && w > 0 && h > 0;) {
        const int big = w > 1 || h > 1;
        const uint8_t *cell_ok = big ? ok2 : ok1, *run = big ? run2 : run1;
        for (int i = 0; i < count && (big ? max_run2 : max_run1) >= w; i++) { // no row this wide: the pass builds nothing
            if (nodes[i].covered) continue;
            if (!rows_can_fit(nodes, run, i, w, h)) continue; // testArea would fail
            if (!test_area(nodes, cell_ok, i, w, h)) continue;
            const NavGenArea *area = build_area(nodes, areas, i, w, h);
            if (!area) {
                ok = 0;
                break;
            }
            for (int y = 0, row = i; y < h; y++, row = nodes[row].links[NAVGEN_SOUTH].to) {
                for (int x = 0, cell = row; x < w; x++, cell = nodes[cell].links[NAVGEN_EAST].to) runs_cover(nodes, run1, run2, cell);
            }
            remaining -= (long) w * h;
        }
        if (w >= h) w--;
        else h--;
    }
    for (int i = 0; ok && i < count; i++) {
        if (nodes[i].covered) continue;
        if (!build_point_area(nodes, areas, i)) ok = 0;
    }
    free(ok1);
    free(ok2);
    free(run1);
    free(run2);
    return ok;
}

// edgeNodeIds(area, dir, nodes): up to `max` ids into out, returns the count
static int edge_node_ids(const NavGenArea *area, const int dir, const NavGenNode *nodes, const int node_count, int32_t *out) {
    const int start = dir == NAVGEN_NORTH || dir == NAVGEN_WEST ? area->nodes[NAVGEN_NW] : dir == NAVGEN_EAST ? area->nodes[NAVGEN_NE] : area->nodes[NAVGEN_SW];
    const int along = dir == NAVGEN_NORTH || dir == NAVGEN_SOUTH ? NAVGEN_EAST : NAVGEN_SOUTH;
    const int end = dir == NAVGEN_NORTH ? area->nodes[NAVGEN_NE] : dir == NAVGEN_SOUTH ? area->nodes[NAVGEN_SE] : dir == NAVGEN_EAST ? area->nodes[NAVGEN_SE] : area->nodes[NAVGEN_SW];
    int n = 0;
    for (int id = start; id >= 0 && n < node_count;) {
        out[n++] = id;
        if (id == end) break;
        id = nodes[id].links[along].to;
    }
    return n;
}

// ------------------------------------------------------------------------- area links

static uint64_t link_key(const int from, const int to) { return (uint64_t) (uint32_t) from << 32 | (uint32_t) to; }

static uint64_t mix64(uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

static int links_rehash(NavGenMesh *m, const size_t cap) {
    uint64_t *keys = malloc(cap * sizeof(uint64_t));
    int32_t *slots = calloc(cap, sizeof(int32_t));
    if (!keys || !slots) {
        free(keys);
        free(slots);
        return 0;
    }
    for (size_t i = 0; i < m->link_slot_cap; i++) {
        if (!m->link_slots[i]) continue;
        size_t s = mix64(m->link_keys[i]) & (cap - 1);
        while (slots[s]) s = (s + 1) & (cap - 1);
        slots[s] = m->link_slots[i];
        keys[s] = m->link_keys[i];
    }
    free(m->link_keys);
    free(m->link_slots);
    m->link_keys = keys;
    m->link_slots = slots;
    m->link_slot_cap = cap;
    return 1;
}

static NavGenAreaLink *link_find(const NavGenMesh *m, const int from, const int to) {
    if (!m->link_slot_cap) return NULL;
    const uint64_t key = link_key(from, to);
    for (size_t s = mix64(key) & (m->link_slot_cap - 1); m->link_slots[s]; s = (s + 1) & (m->link_slot_cap - 1)) {
        if (m->link_keys[s] == key) return &m->links[m->link_slots[s] - 1];
    }
    return NULL;
}

// links[`${from}:${to}`] = link (a new key goes last, an existing one keeps its place)
static int link_set(NavGenMesh *m, const NavGenAreaLink *link, int *cap) {
    NavGenAreaLink *existing = link_find(m, link->from, link->to);
    if (existing) {
        *existing = *link;
        return 1;
    }
    if ((size_t) (m->link_count + 1) * 2 > m->link_slot_cap && !links_rehash(m, m->link_slot_cap ? m->link_slot_cap * 2 : 1024)) return 0;
    if (m->link_count == *cap) {
        const int grown_cap = *cap ? *cap * 2 : 1024;
        NavGenAreaLink *grown = realloc(m->links, (size_t) grown_cap * sizeof(NavGenAreaLink));
        if (!grown) return 0;
        m->links = grown;
        *cap = grown_cap;
    }
    const uint64_t key = link_key(link->from, link->to);
    size_t s = mix64(key) & (m->link_slot_cap - 1);
    while (m->link_slots[s]) s = (s + 1) & (m->link_slot_cap - 1);
    m->links[m->link_count] = *link;
    m->link_keys[s] = key;
    m->link_slots[s] = ++m->link_count;
    return 1;
}

static double dist2d(const NavGenPoint a, const NavGenPoint b) { return hypot(a.x - b.x, a.z - b.z); }

static void area_bounds(const NavGenArea *a, double *min_x, double *max_x, double *min_z, double *max_z) {
    *min_x = js_min(js_min(a->nw.x, a->ne.x), js_min(a->se.x, a->sw.x));
    *max_x = js_max(js_max(a->nw.x, a->ne.x), js_max(a->se.x, a->sw.x));
    *min_z = js_min(js_min(a->nw.z, a->ne.z), js_min(a->se.z, a->sw.z));
    *max_z = js_max(js_max(a->nw.z, a->ne.z), js_max(a->se.z, a->sw.z));
}

static double area_y_at(const NavGenArea *a, const double x, const double z) {
    double min_x, max_x, min_z, max_z;
    area_bounds(a, &min_x, &max_x, &min_z, &max_z);
    const double dx = js_max(0.0001, max_x - min_x), dz = js_max(0.0001, max_z - min_z);
    const double u = js_clamp01((x - min_x) / dx), v = js_clamp01((z - min_z) / dz);
    const double north = a->nw.y + u * (a->ne.y - a->nw.y);
    const double south = a->sw.y + u * (a->se.y - a->sw.y);
    return north + v * (south - north);
}

NavGenPoint navgen_area_center(const NavGenArea *a) {
    const double x = (a->nw.x + a->ne.x + a->se.x + a->sw.x) / 4.0;
    const double z = (a->nw.z + a->ne.z + a->se.z + a->sw.z) / 4.0;
    return (NavGenPoint) {x, area_y_at(a, x, z), z};
}

static NavGenPoint closest_point_on_area(const NavGenArea *a, const NavGenPoint p) {
    double min_x, max_x, min_z, max_z;
    area_bounds(a, &min_x, &max_x, &min_z, &max_z);
    const double x = js_max(min_x, js_min(max_x, p.x)), z = js_max(min_z, js_min(max_z, p.z));
    return (NavGenPoint) {x, area_y_at(a, x, z), z};
}

static NavGenAreaLink make_area_link(const NavGenArea *from, const NavGenArea *to, const NavGenNodeLink *node_link) {
    const NavGenPoint fc = navgen_area_center(from), tc = navgen_area_center(to);
    const double dy = tc.y - fc.y;
    const double distance = sqrt((tc.x - fc.x) * (tc.x - fc.x) + dy * dy + (tc.z - fc.z) * (tc.z - fc.z));
    const double nh = node_link->obstacle_height;
    const double obstacle = js_max(nh != 0.0 && !isnan(nh) ? nh : 0.0, dy > NG_STEP ? dy : 0.0);
    const int crouch = (from->attrs & NAVGEN_ATTR_CROUCH) || (to->attrs & NAVGEN_ATTR_CROUCH);
    const NavGenLinkKind kind = crouch ? NAVGEN_LINK_CROUCH : dy > NG_STEP ? NAVGEN_LINK_JUMP : dy < -NG_STEP ? NAVGEN_LINK_DROP : NAVGEN_LINK_WALK;
    const double jump = dy > NG_STEP ? NG_JUMP_PENALTY + js_max(0.0, dy - NG_STEP) * 6.0 : 0.0;
    const double drop = dy < -NG_STEP ? NG_DROP_PENALTY + js_max(0.0, -dy - NG_STEP) * 3.0 : 0.0;
    const double crouch_cost = crouch ? NG_CROUCH_PENALTY : 0.0;
    const double obstacle_cost = obstacle > NG_STEP ? (obstacle - NG_STEP) * 8.0 : 0.0;
    const double death = dy < -NG_DEATH_DROP ? INFINITY : 0.0;
    const double limit = dy > NG_JUMP + NG_STEP ? INFINITY : 0.0;
    return (NavGenAreaLink) {from->id, to->id, distance, dy, obstacle, distance + jump + drop + crouch_cost + obstacle_cost + death + limit, kind};
}

// connectGeneratedAreas + buildAreaLinks (both walk every area edge; kept as two passes so the
// connection lists fill in the JS order before any link exists)
static int connect_areas_and_links(NavGenMesh *m, int32_t *edge, int *link_cap) {
    for (int a = 0; a < m->area_count; a++) { // connectGeneratedAreas: scanAreaEdge per direction
        NavGenArea *area = &m->areas[a];
        for (int dir = 0; dir < NAVGEN_DIRS; dir++) {
            const int n = edge_node_ids(area, dir, m->nodes, m->node_count, edge);
            for (int i = 0; i < n; i++) {
                const int to = m->nodes[edge[i]].links[dir].to;
                if (to < 0) continue;
                const int target = m->nodes[to].area;
                if (target < 0 || target == area->id || target >= m->area_count) continue;
                if (!area_connect(area, dir, target)) return 0;
            }
        }
    }
    for (int a = 0; a < m->area_count; a++) { // buildAreaLinks
        const NavGenArea *area = &m->areas[a];
        for (int dir = 0; dir < NAVGEN_DIRS; dir++) {
            const int n = edge_node_ids(area, dir, m->nodes, m->node_count, edge);
            for (int i = 0; i < n; i++) {
                const NavGenNodeLink *link = &m->nodes[edge[i]].links[dir];
                if (link->to < 0) continue;
                const int target = m->nodes[link->to].area;
                if (target < 0 || target == area->id || target >= m->area_count) continue;
                const NavGenAreaLink candidate = make_area_link(area, &m->areas[target], link);
                const NavGenAreaLink *existing = link_find(m, area->id, target);
                if ((!existing || candidate.cost < existing->cost) && !link_set(m, &candidate, link_cap)) return 0;
                if (!link_find(m, target, area->id)) {
                    const NavGenAreaLink reverse = make_area_link(&m->areas[target], area, link);
                    if (!link_set(m, &reverse, link_cap)) return 0;
                }
            }
        }
    }
    return 1;
}

static int nearest_area_to_ladder_end(const NavGenMesh *m, const double x, const double z, const double y) {
    const double max_h = NG_CELL * 2.0 + NG_LADDER_REACH, max_v = NG_STEP + NG_JUMP;
    int best = -1;
    double best_score = 0.0;
    for (int a = 0; a < m->area_count; a++) {
        const NavGenPoint c = closest_point_on_area(&m->areas[a], (NavGenPoint) {x, y, z});
        const double h = hypot(c.x - x, c.z - z), v = fabs(c.y - y);
        if (h > max_h || v > max_v) continue;
        const double score = h + v;
        if (best < 0 || score < best_score) {
            best = a;
            best_score = score;
        }
    }
    return best;
}

static int add_ladder_link(NavGenMesh *m, const int from_id, const int to_id, int *link_cap) {
    NavGenArea *from = &m->areas[from_id];
    const NavGenPoint fc = navgen_area_center(from), tc = navgen_area_center(&m->areas[to_id]);
    const double dy = tc.y - fc.y;
    const double distance = sqrt((tc.x - fc.x) * (tc.x - fc.x) + dy * dy + (tc.z - fc.z) * (tc.z - fc.z));
    const NavGenAreaLink link = {from_id, to_id, distance, dy, 0.0, distance + NG_LADDER_MOUNT, NAVGEN_LINK_LADDER};
    if (!link_set(m, &link, link_cap)) return 0;
    const double dx = tc.x - fc.x, dz = tc.z - fc.z; // ladderConnectionDir
    const int dir = fabs(dx) >= fabs(dz) ? (dx >= 0.0 ? NAVGEN_EAST : NAVGEN_WEST) : (dz >= 0.0 ? NAVGEN_SOUTH : NAVGEN_NORTH);
    return area_connect(from, dir, to_id);
}

// ------------------------------------------------------------------------- build

static void summarize(NavGenMesh *m, const int seeds) {
    NavGenStats *s = &m->stats;
    memset(s, 0, sizeof(*s));
    s->seeds = seeds;
    for (int i = 0; i < m->node_count; i++) {
        const NavGenNode *node = &m->nodes[i];
        if (node->area < 0) s->uncovered_nodes++;
        for (int c = 0; c < NAVGEN_CORNERS; c++) {
            if (node->blocked & (1u << c)) s->blocked_quadrants++;
            if (node->crouch & (1u << c)) s->crouch_quadrants++;
        }
        int links = 0;
        for (int d = 0; d < NAVGEN_DIRS; d++) {
            const NavGenNodeLink *link = &node->links[d];
            if (link->to < 0) continue;
            links++;
            s->node_links++;
            if (link->obstacle_height > NG_STEP) s->obstacle_links++;
            const NavGenNodeLink *reverse = &m->nodes[link->to].links[k_opposite[d]];
            if (reverse->to != i) s->one_way_links++;
        }
        if (!links) s->isolated_nodes++;
    }
}

void navgen_free(NavGenMesh *m) {
    if (!m) return;
    for (int a = 0; a < m->area_count; a++) {
        for (int d = 0; d < NAVGEN_DIRS; d++) free(m->areas[a].connections[d].ids);
    }
    free(m->areas);
    free(m->nodes);
    free(m->links);
    free(m->link_keys);
    free(m->link_slots);
    free(m);
}

NavGenMesh *navgen_build(const cJSON *map) {
    if (!cJSON_IsObject(map)) return NULL;
    NavGenMesh *m = calloc(1, sizeof(NavGenMesh));
    if (!m) return NULL;
    m->step = NG_CELL;
    int box_count = 0, ladder_count = 0, ok = 0, link_cap = 0;
    NgBox *boxes = extract_boxes(map, &box_count);
    NgLadder *ladders = extract_ladders(map, &ladder_count);
    NgGrid grid = {0};
    NgNodes nodes = {0};
    NgAreas areas = {0};
    int32_t *stack = NULL, *edge = NULL;
    int stack_n = 0, stack_cap = 0, seeds = 0;
    if (!boxes || !ladders || !grid_init(&grid, boxes, box_count) || !nodes_rehash(&nodes, 4096)) goto done;

    // navBounds(boxes, map, cfg)
    double min_x = INFINITY, max_x = -INFINITY, min_z = INFINITY, max_z = -INFINITY;
    for (int i = 0; i < box_count; i++) {
        min_x = js_min(min_x, boxes[i].min_x);
        max_x = js_max(max_x, boxes[i].max_x);
        min_z = js_min(min_z, boxes[i].min_z);
        max_z = js_max(max_z, boxes[i].max_z);
    }
    const cJSON *spawns = cJSON_GetObjectItemCaseSensitive(map, "spawns");
    if (!cJSON_IsArray(spawns)) spawns = NULL;
    const cJSON *spawn;
    cJSON_ArrayForEach(spawn, spawns) {
        min_x = js_min(min_x, js_index(spawn, 0));
        max_x = js_max(max_x, js_index(spawn, 0));
        min_z = js_min(min_z, js_index(spawn, 2));
        max_z = js_max(max_z, js_index(spawn, 2));
    }
    if (isfinite(min_x)) {
        m->min_x = snap_to_grid(min_x - NG_PADDING, NG_CELL);
        m->max_x = snap_to_grid(max_x + NG_PADDING, NG_CELL);
        m->min_z = snap_to_grid(min_z - NG_PADDING, NG_CELL);
        m->max_z = snap_to_grid(max_z + NG_PADDING, NG_CELL);
    }

    // walkableSeeds: the ground under every spawn (pass 0). Pass 1 (find_seed_near_spawn) only
    // runs when pass 0 found nothing, so every mesh v7 builds stays v7's.
    for (int pass = 0; pass < 2 && !seeds; pass++) {
        cJSON_ArrayForEach(spawn, spawns) {
            const NavGenPoint p = {
                snap_to_grid(js_index(spawn, 0), NG_CELL),
                js_or0(js_index(spawn, 1)),
                snap_to_grid(js_index(spawn, 2), NG_CELL),
            };
            NgGround ground;
            if (pass == 0 ? !find_ground_for_node(&grid, p, &ground) : !find_seed_near_spawn(&grid, spawn, p, &ground)) continue;
            seeds++;
            const int id = add_node(&nodes, &grid, ground.point, ground.normal);
            if (id < 0) goto done;
            if (stack_n == stack_cap) {
                stack_cap = stack_cap ? stack_cap * 2 : 1024;
                int32_t *grown = realloc(stack, (size_t) stack_cap * sizeof(int32_t));
                if (!grown) goto done;
                stack = grown;
            }
            stack[stack_n++] = id;
        }
    }

    // the flood fill
    while (stack_n && nodes.count < NG_MAX_NODES) {
        const int id = stack[--stack_n];
        for (int dir = 0; dir < NAVGEN_DIRS; dir++) {
            if (nodes.nodes[id].links[dir].to >= 0) continue;
            const NavGenPoint from = point_of(&nodes.nodes[id]);
            const NavGenPoint target = {snap_to_grid(from.x + k_dir_dx[dir] * NG_CELL, NG_CELL), from.y, snap_to_grid(from.z + k_dir_dz[dir] * NG_CELL, NG_CELL)};
            // withinBounds
            if (!(target.x >= m->min_x - NG_CELL && target.x <= m->max_x + NG_CELL && target.z >= m->min_z - NG_CELL && target.z <= m->max_z + NG_CELL)) continue;
            const NgTrace trace = trace_adjacent_node(&grid, from, target);
            if (!trace.ok) continue;
            const int next = add_node(&nodes, &grid, trace.end, trace.normal);
            if (next < 0) goto done;
            connect_nodes(&nodes.nodes[id], next, dir, trace.obstacle_height, trace.obstacle_start, trace.obstacle_end);
            const int reverse = fabs(nodes.nodes[id].y - nodes.nodes[next].y) < NG_JUMP + NG_STEP;
            if (reverse && nodes.nodes[next].links[k_opposite[dir]].to < 0) {
                connect_nodes(&nodes.nodes[next], id, k_opposite[dir], trace.obstacle_height, NG_CELL - trace.obstacle_end, NG_CELL - trace.obstacle_start);
            }
            if (!nodes.nodes[next].covered && next == nodes.count - 1) {
                if (stack_n == stack_cap) {
                    stack_cap = stack_cap ? stack_cap * 2 : 1024;
                    int32_t *grown = realloc(stack, (size_t) stack_cap * sizeof(int32_t));
                    if (!grown) goto done;
                    stack = grown;
                }
                stack[stack_n++] = next;
            }
        }
    }
    m->nodes = nodes.nodes;
    m->node_count = nodes.count;
    nodes.nodes = NULL;

    if (!create_nav_areas_from_nodes(m->nodes, m->node_count, &areas)) {
        m->areas = areas.areas;
        m->area_count = areas.count;
        goto done;
    }
    m->areas = areas.areas;
    m->area_count = areas.count;
    edge = malloc((m->node_count > 0 ? (size_t) (unsigned) m->node_count : 1u) * sizeof(int32_t));
    if (!edge || !connect_areas_and_links(m, edge, &link_cap)) goto done;
    for (int i = 0; i < ladder_count; i++) { // connectLadderAreas
        const NgLadder *l = &ladders[i];
        const double cx = (l->min_x + l->max_x) / 2.0, cz = (l->min_z + l->max_z) / 2.0;
        const int bottom = nearest_area_to_ladder_end(m, cx, cz, l->base_y);
        const int top = nearest_area_to_ladder_end(m, cx, cz, l->top_y);
        if (bottom < 0 || top < 0 || bottom == top) continue;
        if (!add_ladder_link(m, bottom, top, &link_cap) || !add_ladder_link(m, top, bottom, &link_cap)) goto done;
    }
    summarize(m, seeds);
    ok = 1;

done:
    free(nodes.nodes);
    free(nodes.keys);
    free(nodes.slots);
    free(stack);
    free(edge);
    grid_free(&grid);
    free(boxes);
    free(ladders);
    if (!ok) {
        navgen_free(m);
        return NULL;
    }
    return m;
}

// ------------------------------------------------------------------------- queries

const NavGenAreaLink *navgen_area_link(const NavGenMesh *m, const int from, const int to) { return m ? link_find(m, from, to) : NULL; }

int navgen_has_connection(const NavGenMesh *m, const int from, const int to) {
    if (!m || from < 0 || from >= m->area_count) return 0;
    const NavGenArea *a = &m->areas[from];
    for (int d = 0; d < NAVGEN_DIRS; d++) {
        for (int i = 0; i < a->connections[d].count; i++) if (a->connections[d].ids[i] == to) return 1;
    }
    return 0;
}

int navgen_resolve_area(const NavGenMesh *m, const NavGenPoint p) {
    if (!m) return -1;
    const double max_h = m->step * 12.0, max_v = NG_JUMP + NG_STEP;
    int best = -1;
    double best_score = 0.0;
    for (int a = 0; a < m->area_count; a++) {
        const NavGenArea *area = &m->areas[a];
        double min_x, max_x, min_z, max_z;
        area_bounds(area, &min_x, &max_x, &min_z, &max_z);
        const int inside = p.x >= min_x && p.x <= max_x && p.z >= min_z && p.z <= max_z;
        const NavGenPoint c = closest_point_on_area(area, p);
        const double v = fabs(c.y - p.y), h = hypot(c.x - p.x, c.z - p.z);
        if (inside && v <= max_v) return area->id;
        const double score = h + v * 2.0;
        if (h <= max_h && v <= max_v && (best < 0 || score < best_score)) {
            best = area->id;
            best_score = score;
        }
    }
    return best;
}

// MinHeap (navgen.ts): same sift order, so equal-f ties pop in the JS order
typedef struct {
    int32_t id;
    double f;
} NgHeapItem;

typedef struct {
    NgHeapItem *items;
    int size, cap;
} NgHeap;

static int heap_push(NgHeap *h, const NgHeapItem item) {
    if (h->size == h->cap) {
        const int cap = h->cap ? h->cap * 2 : 256;
        NgHeapItem *grown = realloc(h->items, (size_t) cap * sizeof(NgHeapItem));
        if (!grown) return 0;
        h->items = grown;
        h->cap = cap;
    }
    int index = h->size++;
    while (index > 0) {
        const int parent = (index - 1) >> 1;
        if (h->items[parent].f <= item.f) break;
        h->items[index] = h->items[parent];
        index = parent;
    }
    h->items[index] = item;
    return 1;
}

static NgHeapItem heap_pop(NgHeap *h) {
    const NgHeapItem top = h->items[0];
    const NgHeapItem last = h->items[--h->size];
    if (h->size) {
        int index = 0;
        for (;;) {
            const int left = index * 2 + 1, right = left + 1;
            if (left >= h->size) break;
            const int child = right < h->size && h->items[right].f < h->items[left].f ? right : left;
            if (h->items[child].f >= last.f) break;
            h->items[index] = h->items[child];
            index = child;
        }
        h->items[index] = last;
    }
    return top;
}

// makeStaticPathCost() with the default options
static double static_path_cost(const NavGenArea *from_area, const NavGenAreaLink *link, const double length_so_far) {
    if (!from_area || !link) return 0.0;
    if (link->height_delta > NG_JUMP + NG_STEP) return -1.0;
    if (link->height_delta < -NG_DEATH_DROP) return -1.0;
    const double baked = js_max(0.0, link->cost - link->distance);
    const double cost = length_so_far + link->distance + baked;
    return isfinite(cost) ? cost : -1.0;
}

// findGeneratedAreaPath (closest = 0) / findClosestGeneratedAreaPath (closest = 1): the area
// ids start..goal (or ..closest) into *out (malloc'd), returns the count, 0 none, -1 OOM
static int find_area_path(const NavGenMesh *m, const int start, const int goal, const int closest, int32_t **out) {
    const int n = m->area_count;
    int32_t *came_from = malloc((size_t) n * sizeof(int32_t));
    double *g = malloc((size_t) n * sizeof(double));
    uint8_t *closed = calloc((size_t) n, 1);
    NgHeap open = {0};
    int result = -1;
    if (!came_from || !g || !closed) goto done;
    for (int i = 0; i < n; i++) {
        came_from[i] = -1;
        g[i] = INFINITY;
    }
    const NavGenPoint goal_center = navgen_area_center(&m->areas[goal]);
    int best = start;
    double best_score = dist2d(navgen_area_center(&m->areas[start]), goal_center);
    g[start] = 0.0; // costFunc(start, null, null, 0)
    if (!heap_push(&open, (NgHeapItem) {start, best_score})) goto done;
    int end = -1;
    while (open.size) {
        const NgHeapItem current = heap_pop(&open);
        if (closed[current.id]) continue;
        if (current.id == goal) {
            end = goal;
            break;
        }
        closed[current.id] = 1;
        if (closest) {
            const double score = dist2d(navgen_area_center(&m->areas[current.id]), goal_center);
            if (score < best_score) {
                best_score = score;
                best = current.id;
            }
        }
        const NavGenArea *area = &m->areas[current.id];
        // generatedAreaNeighbors: the lists in key order, deduplicated
        for (int o = 0; o < area->order_count; o++) {
            const NavGenIdList *list = &area->connections[area->order[o]];
            for (int i = 0; i < list->count; i++) {
                const int next = list->ids[i];
                int seen = 0;
                for (int p = 0; p < o && !seen; p++) {
                    const NavGenIdList *prev = &area->connections[area->order[p]];
                    for (int q = 0; q < prev->count && !seen; q++) seen = prev->ids[q] == next;
                }
                for (int q = 0; q < i && !seen; q++) seen = list->ids[q] == next;
                if (seen || closed[next]) continue;
                const double tentative = static_path_cost(area, link_find(m, current.id, next), g[current.id]);
                if (tentative < 0.0 || !isfinite(tentative)) continue;
                if (tentative < g[next]) {
                    came_from[next] = current.id;
                    g[next] = tentative;
                    if (!heap_push(&open, (NgHeapItem) {next, tentative + dist2d(navgen_area_center(&m->areas[next]), goal_center)})) goto done;
                }
            }
        }
    }
    if (end < 0 && closest && best != start) end = best;
    result = 0;
    if (end >= 0) {
        int count = 0;
        for (int id = end; id >= 0; id = came_from[id]) count++;
        *out = malloc((size_t) count * sizeof(int32_t));
        if (!*out) {
            result = -1;
            goto done;
        }
        int i = count;
        for (int id = end; id >= 0; id = came_from[id]) (*out)[--i] = id;
        result = count;
    }

done:
    free(came_from);
    free(g);
    free(closed);
    free(open.items);
    return result;
}

static NavGenPoint portal_point(const NavGenArea *from, const NavGenArea *to) {
    double ax0, ax1, az0, az1, bx0, bx1, bz0, bz1;
    area_bounds(from, &ax0, &ax1, &az0, &az1);
    area_bounds(to, &bx0, &bx1, &bz0, &bz1);
    const double ox0 = js_max(ax0, bx0), ox1 = js_min(ax1, bx1), oz0 = js_max(az0, bz0), oz1 = js_min(az1, bz1);
    const NavGenPoint ca = navgen_area_center(from), cb = navgen_area_center(to);
    double x, z;
    if (ox0 <= ox1) {
        x = (ox0 + ox1) / 2.0;
        z = cb.z < ca.z ? az0 : az1;
    } else if (oz0 <= oz1) {
        x = cb.x < ca.x ? ax0 : ax1;
        z = (oz0 + oz1) / 2.0;
    } else {
        const NavGenPoint fp = closest_point_on_area(from, cb), tp = closest_point_on_area(to, ca);
        x = (fp.x + tp.x) / 2.0;
        z = (fp.z + tp.z) / 2.0;
    }
    return (NavGenPoint) {x, (area_y_at(from, x, z) + area_y_at(to, x, z)) / 2.0, z};
}

static int kind_significance(const NavGenLinkKind k) {
    switch (k) {
        case NAVGEN_LINK_CROUCH: return 1;
        case NAVGEN_LINK_DROP: return 2;
        case NAVGEN_LINK_JUMP: return 3;
        case NAVGEN_LINK_LADDER: return 4;
        default: return 0;
    }
}

// compactPointsWithKinds, one point at a time (points past NAVGEN_PATH_MAX are cut)
static void path_append(NavGenPath *out, const NavGenPoint p, const NavGenLinkKind kind) {
    if (out->count) {
        const NavGenPoint prev = out->points[out->count - 1];
        const double dx = prev.x - p.x, dy = prev.y - p.y, dz = prev.z - p.z;
        if (sqrt(dx * dx + dy * dy + dz * dz) < 1.0) {
            NavGenLinkKind *last = &out->kinds[out->count - 1];
            if (kind_significance(kind) > kind_significance(*last)) *last = kind; // moreSignificantKind
            return;
        }
    }
    if (out->count == NAVGEN_PATH_MAX) return;
    out->points[out->count] = p;
    out->kinds[out->count] = kind;
    out->count++;
}

int navgen_find_path(const NavGenMesh *m, const NavGenPoint from, const NavGenPoint to, NavGenPath *out) {
    memset(out, 0, sizeof(*out));
    out->start_area = out->goal_area = -1;
    const int start = navgen_resolve_area(m, from), goal = navgen_resolve_area(m, to);
    if (start < 0 || goal < 0) return 0;
    const NavGenPoint projected_goal = closest_point_on_area(&m->areas[goal], to);
    const double ex = projected_goal.x - to.x, ey = projected_goal.y - to.y, ez = projected_goal.z - to.z;
    out->start_area = start;
    out->goal_area = goal;
    out->endpoint_distance = sqrt(ex * ex + ey * ey + ez * ez);
    if (start == goal) {
        out->points[0] = closest_point_on_area(&m->areas[start], from);
        out->points[1] = projected_goal;
        out->kinds[0] = out->kinds[1] = NAVGEN_LINK_WALK;
        out->count = 2;
        out->area_count = 1;
        out->reached_goal = out->endpoint_distance <= m->step * 4.0;
        return 1;
    }
    int32_t *ids = NULL;
    int n = find_area_path(m, start, goal, 0, &ids);
    if (n == 0) n = find_area_path(m, start, goal, 1, &ids);
    if (n < 2) {
        free(ids);
        return 0;
    }
    path_append(out, closest_point_on_area(&m->areas[ids[0]], from), NAVGEN_LINK_WALK);
    for (int i = 1; i < n; i++) {
        const NavGenAreaLink *link = link_find(m, ids[i - 1], ids[i]);
        path_append(out, portal_point(&m->areas[ids[i - 1]], &m->areas[ids[i]]), link ? link->kind : NAVGEN_LINK_WALK);
    }
    path_append(out, closest_point_on_area(&m->areas[ids[n - 1]], to), NAVGEN_LINK_WALK);
    out->area_count = n;
    out->reached_goal = ids[n - 1] == goal && out->endpoint_distance <= m->step * 4.0;
    free(ids);
    return 1;
}

const char *navgen_link_kind_name(const NavGenLinkKind kind) {
    static const char *const NAMES[] = {"walk", "jump", "drop", "crouch", "ladder"};
    return (unsigned) kind < 5 ? NAMES[kind] : "walk";
}
