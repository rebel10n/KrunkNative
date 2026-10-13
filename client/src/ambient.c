// Krunker v3.1.1 ambient-occlusion planes ("ambientShading", the ambient_0 / ambient_1 mesh
// groups): dark, alpha-textured strips generated along the edges and corners where two map
// boxes touch. Straight port of map.js generate() (Krunker.Unpacked-v3.1.1/93.js, the
// "aoOpac" block) with its helpers from objects.js (94.js addAmbient, limitAmb, collision,
// groupsMatch, clearAOGroup, addPendingAOs) and utils.js (2.js boxIntersection,
// getIntersection, limitRect, getMaxRect, limitRectVal, pointInBox, pointInBox3D).
//
// The collision boxes are rebuilt from the map JSON in map order exactly like the v3.1.1
// builders push them (addBlock, addMesh, addPlane; every other builder's box is
// complexMesh/gate or has none), with the same rounding, so the float comparisons the JS
// does (corner equality, angle equality) behave the same. Doubles throughout, like JS.
//
// Every generated plane is 55.js addPlane: THREE.PlaneGeometry(1, 1) scaled (2M, 2L),
// rotateY(I) rotateX(J) rotateZ(K), at (F, G, H), texture ambient_N stretched, Lambert,
// transparent, DoubleSide, depthWrite false. All planes of one texture are merged into a
// single world-space mesh (Krunker's meshGroup), so there are two draws per map.
#include <gl.h>
#include <ambient.h>
#include <map_catalog.h>
#include <prefabs.h>
#include <render_settings.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AO_SCALE 8.0          // config.js ambScale
#define AO_OFFSET 0.09        // config.js ambOff
#define AO_POSITION_RANGE 500000.0

enum { AX_X = 0, AX_Y = 1, AX_Z = 2 };

typedef struct {
    double c[3];    // centre x, y, z
    double h[3];    // half extents: width (x), height (y), length (z) (utils.js cdv)
    int eligible;   // !noVis && !complexMesh && !health && !canDestroy && !noAmb && !gate
    int sid;
} AoBox;

typedef struct {
    double v[3];    // only the two intersection axes are set
    double d;
    int dont_use;
} AoCorner;

typedef struct {
    double c[3];
    double h[3];
} AoRect;

// One plane: addPlane(F, G, H, L, M, {src: ambient_N}, I, J, K)
typedef struct {
    double pos[3];
    double rot[3];  // I (rotateY), J (rotateX), K (rotateZ)
    double half_l;  // L: half size along the plane's local y
    double half_m;  // M: half size along the plane's local x
    int texture;    // N
} AoPlane;

// a pending plane of objects.js aosToAdd (vals + group)
typedef struct {
    AoPlane plane;
    char group[16];
    int dont_add;
} AoPending;

typedef struct {
    char key[32];   // "<sidA>-<sidB>"
    AoPending *items;
    size_t count, capacity;
} AoPendingGroup;

typedef struct {
    AoPlane *planes;
    size_t plane_count, plane_capacity;
    AoPendingGroup *groups;
    size_t group_count, group_capacity;
} AoBuild;

static Mesh *g_ambient_meshes[2];
static unsigned int g_ambient_vbos[2];

// ---------------------------------------------------------------- JS helpers

static double ao_round2(const double v) {
    return floor(v * 100.0 + 0.5) / 100.0; // Number.prototype.round(2): Math.round(v * 100) / 100
}

static double ao_json_number(const cJSON *json) {
    if (cJSON_IsNumber(json)) return cJSON_GetNumberValue(json);
    if (cJSON_IsString(json)) return atof(cJSON_GetStringValue(json));
    return 0.0;
}

static int ao_json_truthy(const cJSON *json) {
    if (!json || cJSON_IsNull(json) || cJSON_IsFalse(json)) return 0;
    if (cJSON_IsNumber(json)) return cJSON_GetNumberValue(json) != 0.0;
    if (cJSON_IsString(json)) return cJSON_GetStringValue(json)[0] != 0;
    return 1;
}

static int ao_json_present(const cJSON *json) {
    return json && !cJSON_IsNull(json);
}

// utils.js pointInBox (strict unless inclusive)
static int ao_point_in_box(const double l, const double m, const double p, const double q, const double u, const double v, const int inclusive) {
    if (inclusive) return l >= p && l <= u && m >= q && m <= v;
    return l > p && l < u && m > q && m < v;
}

static int ao_point_in_box_3d(const double x, const double y, const double z, const AoBox *b, const double pad) {
    return x >= b->c[0] - b->h[0] - pad && x <= b->c[0] + b->h[0] + pad &&
           y >= b->c[1] - b->h[1] - pad && y <= b->c[1] + b->h[1] + pad &&
           z >= b->c[2] - b->h[2] - pad && z <= b->c[2] + b->h[2] + pad;
}

// objects.js collision(F, G, 0)
static int ao_collision(const AoBox *f, const AoBox *g) {
    return f->c[0] - f->h[0] <= g->c[0] + g->h[0] && f->c[0] + f->h[0] >= g->c[0] - g->h[0] &&
           f->c[2] - f->h[2] <= g->c[2] + g->h[2] && f->c[2] + f->h[2] >= g->c[2] - g->h[2] &&
           f->c[1] - f->h[1] <= g->c[1] + g->h[1] && f->c[1] + f->h[1] >= g->c[1] - g->h[1];
}

// utils.js boxIntersection(q, z, A, B, C): the 4 corners of the overlap of the two boxes'
// footprints on axes a/b (grown by 0.1), with corners on a shared edge or inside one
// footprint marked dontUse. Returns 0 when the footprints do not overlap.
static int ao_box_intersection(const AoBox *q, const AoBox *z, const int a, const int b, const double dirs[4], AoCorner out[4]) {
    const double f = q->c[a] - q->h[a] - 0.1;
    const double g = z->c[a] - z->h[a] - 0.1;
    const double h = q->c[a] + q->h[a] + 0.1;
    const double i = z->c[a] + z->h[a] + 0.1;
    const double j = q->c[b] - q->h[b] - 0.1;
    const double k = z->c[b] - z->h[b] - 0.1;
    const double l = q->c[b] + q->h[b] + 0.1;
    const double m = z->c[b] + z->h[b] + 0.1;
    const double n = fmax(f, g);
    const double o = fmin(h, i);
    if (!(o >= n)) return 0;

    const double p = fmax(j, k);
    const double qq = fmin(l, m);
    if (!(qq >= p)) return 0;

    const double corner_a[4] = {n, o, n, o};
    const double corner_b[4] = {p, qq, qq, p};

    for (int s = 0; s < 4; s++) {
        AoCorner *r = &out[s];
        memset(r, 0, sizeof(*r));
        r->v[a] = corner_a[s];
        r->v[b] = corner_b[s];
        r->d = dirs[s];

        const double ra = r->v[a];
        const double rb = r->v[b];
        r->dont_use = (ra == h && ra == i) || (ra == f && ra == g) || (rb == l && rb == m) || (rb == j && rb == k) ||
                      ao_point_in_box(ra, rb, f, j, h, l, 0) || ao_point_in_box(ra, rb, g, k, i, m, 0);
    }

    return 1;
}

// utils.js getIntersection(m, q, u): centre and half size of the boxes' overlap on axis u
static int ao_get_intersection(const AoBox *m, const AoBox *q, const int u, double *center, double *half) {
    const double w = q->c[u] - q->h[u];
    const double x = m->c[u] - m->h[u];
    const double y = m->c[u] + m->h[u];
    const double z = q->c[u] + q->h[u];
    const double a = fmax(x, w);
    const double b = fmin(y, z);
    if (!(b >= a)) return 0;

    const double c = (b - a) / 2.0;
    *center = a + c;
    *half = c;
    return 1;
}

// objects.js limitAmb(F, G, H, I, J, K, L)
static double ao_limit_amb(const double f, const AoCorner *g, const double angle, const int ai, const int aj, const AoBox *k, const AoBox *l) {
    if (angle == -M_PI / 2.0 || angle == M_PI + M_PI / 2.0) {
        return fmin(f, (g->v[aj] - fmin(k->c[aj] - k->h[aj], l->c[aj] - l->h[aj])) / 2.0);
    }
    if (angle == M_PI / 2.0) {
        return fmin(f, (fmax(k->c[aj] + k->h[aj], l->c[aj] + l->h[aj]) - g->v[aj]) / 2.0);
    }
    if (angle == 0.0) {
        return fmin(f, (fmax(k->c[ai] + k->h[ai], l->c[ai] + l->h[ai]) - g->v[ai]) / 2.0);
    }
    return fmin(f, (g->v[ai] - fmin(k->c[ai] - k->h[ai], l->c[ai] - l->h[ai])) / 2.0);
}

// utils.js limitRectVal(l, m, p)
static void ao_limit_rect_val(AoRect *l, const AoRect *m, const int p) {
    if (l->c[p] - l->h[p] < m->c[p] - m->h[p]) {
        const double u = (m->c[p] - m->h[p] - (l->c[p] - l->h[p])) / 2.0;
        l->h[p] -= u;
        l->c[p] += u;
    }
    if (l->c[p] + l->h[p] > m->c[p] + m->h[p]) {
        const double u = (l->c[p] + l->h[p] - (m->c[p] + m->h[p])) / 2.0;
        l->c[p] -= u;
        l->h[p] -= u;
    }
}

// utils.js limitRect(m, q, u, v, w, [T, U], y, z): a rect centred (m, q) on axes (y, z)
// with half sizes (u, v), clipped to the two boxes' bounding rect, halves swapped for w 0/PI
static AoRect ao_limit_rect(const double m, const double q, const double u, const double v, const double w, const AoBox *t, const AoBox *uu, const int y, const int z) {
    AoRect max = {0};
    max.c[y] = (fmin(t->c[y] - t->h[y], uu->c[y] - uu->h[y]) + fmax(t->c[y] + t->h[y], uu->c[y] + uu->h[y])) / 2.0;
    max.c[z] = (fmin(t->c[z] - t->h[z], uu->c[z] - uu->h[z]) + fmax(t->c[z] + t->h[z], uu->c[z] + uu->h[z])) / 2.0;
    max.h[y] = fabs(fmax(t->c[y] + t->h[y], uu->c[y] + uu->h[y]) - fmin(t->c[y] - t->h[y], uu->c[y] - uu->h[y])) / 2.0;
    max.h[z] = fabs(fmax(t->c[z] + t->h[z], uu->c[z] + uu->h[z]) - fmin(t->c[z] - t->h[z], uu->c[z] - uu->h[z])) / 2.0;

    AoRect d = {0};
    // the axis not in (y, z) stays NaN like the JS object's missing key
    for (int i = 0; i < 3; i++) d.c[i] = d.h[i] = NAN;
    d.c[y] = m;
    d.c[z] = q;
    d.h[z] = v;
    d.h[y] = u;

    ao_limit_rect_val(&d, &max, y);
    ao_limit_rect_val(&d, &max, z);

    if (w == 0.0 || w == M_PI) {
        const double e = d.h[y];
        d.h[y] = d.h[z];
        d.h[z] = e;
    }

    return d;
}

// objects.js groupsMatch: the 24 corner-piece pairs that cover the same spot
static const char *AO_GROUP_PAIRS[24][2] = {
    {"g6-1-2", "g4-1-2"}, {"g5-1-2", "g4-0-1"}, {"g6-0-1", "g3-1-2"}, {"g5-0-1", "g3-0-1"},
    {"g6-0-3", "g2-0-3"}, {"g5-0-3", "g2-1-1"}, {"g6-1-1", "g1-0-3"}, {"g5-1-1", "g1-1-1"},
    {"g6-0-2", "g1-1-0"}, {"g5-0-2", "g1-0-2"}, {"g6-1-0", "g2-1-0"}, {"g5-1-0", "g2-0-2"},
    {"g4-0-2", "g1-0-0"}, {"g3-0-2", "g1-1-3"}, {"g4-1-0", "g2-0-0"}, {"g3-1-0", "g2-1-3"},
    {"g4-0-3", "g2-1-2"}, {"g3-0-3", "g2-0-1"}, {"g4-1-1", "g1-1-2"}, {"g3-1-1", "g1-0-1"},
    {"g6-1-3", "g3-0-0"}, {"g5-1-3", "g3-1-3"}, {"g6-0-0", "g4-0-0"}, {"g5-0-0", "g4-1-3"},
};

static int ao_groups_match(const char *f, const char *g) {
    for (int i = 0; i < 24; i++) {
        const int has_f = !strcmp(AO_GROUP_PAIRS[i][0], f) || !strcmp(AO_GROUP_PAIRS[i][1], f);
        const int has_g = !strcmp(AO_GROUP_PAIRS[i][0], g) || !strcmp(AO_GROUP_PAIRS[i][1], g);
        if (has_f && has_g) return 1;
    }
    return 0;
}

// ---------------------------------------------------------------- plane collection

static void ao_push_plane(AoBuild *build, const AoPlane *plane) {
    if (build->plane_count == build->plane_capacity) {
        const size_t capacity = build->plane_capacity ? build->plane_capacity * 2 : 1024;
        AoPlane *planes = realloc(build->planes, capacity * sizeof(AoPlane));
        if (!planes) return;
        build->planes = planes;
        build->plane_capacity = capacity;
    }
    build->planes[build->plane_count++] = *plane;
}

static AoPendingGroup *ao_pending_group(AoBuild *build, const char *key) {
    for (size_t i = 0; i < build->group_count; i++) {
        if (!strcmp(build->groups[i].key, key)) return &build->groups[i];
    }

    if (build->group_count == build->group_capacity) {
        const size_t capacity = build->group_capacity ? build->group_capacity * 2 : 256;
        AoPendingGroup *groups = realloc(build->groups, capacity * sizeof(AoPendingGroup));
        if (!groups) return NULL;
        build->groups = groups;
        build->group_capacity = capacity;
    }

    AoPendingGroup *group = &build->groups[build->group_count++];
    memset(group, 0, sizeof(*group));
    snprintf(group->key, sizeof(group->key), "%s", key);
    return group;
}

// objects.js addAmbient(F, G, H, I, J, K, L, M, N, O, P). Without boxes the plane is added
// now; with boxes it is kept only when (F, G, H) lies within ambOff * 2 of one of them and
// queued in aosToAdd["sidA-sidB"] (sizes grown by ambOff) for addPendingAOs.
static void ao_add_ambient(AoBuild *build, const double f, const double g, const double h, const double i, const double j, const double k,
                           const double l, const double m, const int n, const AoBox *t, const AoBox *u, const char *group) {
    AoPlane plane = {{f, g, h}, {i, j, k}, l, m, n};

    if (!t) {
        ao_push_plane(build, &plane);
        return;
    }

    if (!ao_point_in_box_3d(f, g, h, t, AO_OFFSET * 2.0) && !ao_point_in_box_3d(f, g, h, u, AO_OFFSET * 2.0)) return;

    char key[32];
    snprintf(key, sizeof(key), "%d-%d", t->sid, u->sid);
    AoPendingGroup *pending = ao_pending_group(build, key);
    if (!pending) return;

    if (pending->count == pending->capacity) {
        const size_t capacity = pending->capacity ? pending->capacity * 2 : 16;
        AoPending *items = realloc(pending->items, capacity * sizeof(AoPending));
        if (!items) return;
        pending->items = items;
        pending->capacity = capacity;
    }

    AoPending *item = &pending->items[pending->count++];
    memset(item, 0, sizeof(*item));
    item->plane = plane;
    item->plane.half_l = l + AO_OFFSET;
    item->plane.half_m = m + AO_OFFSET;
    snprintf(item->group, sizeof(item->group), "%s", group);
}

// objects.js clearAOGroup + addPendingAOs: of two corner pieces covering the same spot
// keep the larger one (vals[6] + vals[7]), then add the survivors.
static void ao_add_pending(AoBuild *build) {
    for (size_t gi = 0; gi < build->group_count; gi++) {
        AoPendingGroup *group = &build->groups[gi];

        for (size_t a = group->count; a-- > 0;) {
            AoPending *x = &group->items[a];

            for (size_t b = group->count; b-- > 0;) {
                AoPending *y = &group->items[b];

                if (!x->dont_add && !y->dont_add && x != y && ao_groups_match(x->group, y->group)) {
                    if (x->plane.half_l + x->plane.half_m >= y->plane.half_l + y->plane.half_m) y->dont_add = 1;
                    else x->dont_add = 1;
                    break;
                }
            }
        }

        for (size_t a = 0; a < group->count; a++) {
            if (!group->items[a].dont_add) ao_push_plane(build, &group->items[a].plane);
        }

        free(group->items);
    }

    free(build->groups);
    build->groups = NULL;
    build->group_count = build->group_capacity = 0;
}

// ---------------------------------------------------------------- collision boxes

// v7.1.4 prefabs.js "complex" (addMesh / addGate / addTrigger ... boxes are complexMesh),
// the prefabs that fall through to manager.addMesh (models) and "noDetail" (map-G: from the
// generated v7 prefab table, shared/include/prefabs.h, instead of the v3.1.1 lists)
static int ao_prefab_complex(const int prefab) {
    return prefab_has_flag(prefab, PF_COMPLEX) || prefab == PREFAB_GATE || prefab == PREFAB_TRIGGER || prefab == PREFAB_DEPOSIT_BOX ||
           prefab == PREFAB_LIGHT_CONE || prefab == PREFAB_CUSTOM_ASSET || prefab == PREFAB_SHOWCASE || prefab == PREFAB_BOT || prefab == PREFAB_RUNE;
}

static int ao_prefab_is_mesh(const int prefab) {
    const PrefabInfo *info = prefab_info(prefab);
    return info && info->geometry == PREFAB_GEO_MODEL && prefab != PREFAB_EVENT;
}

static int ao_prefab_no_detail(const int prefab) {
    return prefab_has_flag(prefab, PF_NODETAIL);
}

// The manager.objects boxes that can take part (others are skipped by the loop anyway).
// all = 0: the AO candidates only; all = 1: every box the v3.1.1 builders push to
// manager.objects (zones, ramps, ladders, ... included), for the noise planes.
static AoBox *ao_collect_boxes(const Map *map, const cJSON *objects, const int map_details, const int all, size_t *out_count) {
    const int total = cJSON_GetArraySize(objects);
    size_t object_index = 0; // map.c keeps the raw objects map_raw_object_kept() accepts, in order
    AoBox *boxes = calloc(total > 0 ? (size_t) total : 1, sizeof(AoBox));
    size_t count = 0;
    if (!boxes) return NULL;

    for (int i = 0; i < total; i++) {
        const cJSON *t = cJSON_GetArrayItem(objects, i);
        const cJSON *p = cJSON_GetObjectItem(t, "p");
        const cJSON *s = cJSON_GetObjectItem(t, "s");
        if (!map_raw_object_kept(t)) continue;
        if (object_index >= map->object_count) break;

        // the prefab id as map.c resolved it (numeric or prefab name)
        const int prefab = map->objects[object_index++]->prefab;

        const int no_col = ao_json_truthy(cJSON_GetObjectItem(t, "l")) || ao_json_truthy(cJSON_GetObjectItem(t, "col"));

        const double af = ao_round2(fmax(fmin(ao_json_number(cJSON_GetArrayItem(p, 0)), AO_POSITION_RANGE), -AO_POSITION_RANGE));
        const double ag = ao_round2(fmax(fmin(ao_json_number(cJSON_GetArrayItem(p, 1)), AO_POSITION_RANGE), -AO_POSITION_RANGE));
        const double ah = ao_round2(fmax(fmin(ao_json_number(cJSON_GetArrayItem(p, 2)), AO_POSITION_RANGE), -AO_POSITION_RANGE));
        const double ai = ao_round2(ao_json_number(cJSON_GetArrayItem(s, 0)));
        const double aj = ao_round2(ao_json_number(cJSON_GetArrayItem(s, 1)));
        const double ak = ao_round2(ao_json_number(cJSON_GetArrayItem(s, 2)));

        const int no_vis = ao_json_truthy(cJSON_GetObjectItem(t, "v"));
        // v10: a dyn object forces noAmb (GameObject: this.dyn && (this.noAmb = true), L163775)
        const int no_amb = ao_json_present(cJSON_GetObjectItem(t, "ab")) || map->objects[object_index - 1]->dyn;
        const cJSON *raw_h = cJSON_GetObjectItem(t, "h");
        const int health = (int) ao_json_number(ao_json_truthy(raw_h) ? raw_h : cJSON_GetObjectItem(t, "hp"));
        const int can_destroy = ao_json_present(cJSON_GetObjectItem(t, "cdy"));

        AoBox box = {0};
        box.sid = (int) count;
        box.c[0] = af;
        box.c[2] = ah;
        int complex = 0;

        // boxes the builders only push without noCol (l / col)
        const int collidable_kind = prefab == PREFAB_CUBE || ao_prefab_is_mesh(prefab) || prefab == PREFAB_PLANE ||
                                    prefab == PREFAB_BILLBOARD || prefab == PREFAB_RUNE || prefab == PREFAB_CYLINDER;
        if (no_col && collidable_kind) continue;

        if (prefab == PREFAB_CUBE) {
            // 94.js addBlock: y = p.y + s.y / 2, halves s / 2; complexMesh = rotated or link_0
            const cJSON *r = cJSON_GetObjectItem(t, "r");
            const cJSON *tex = cJSON_GetObjectItem(t, "t");
            complex = (cJSON_IsArray(r) && (ao_json_number(cJSON_GetArrayItem(r, 0)) != 0.0 || ao_json_number(cJSON_GetArrayItem(r, 1)) != 0.0 || ao_json_number(cJSON_GetArrayItem(r, 2)) != 0.0)) ||
                      (cJSON_IsNumber(tex) && (int) cJSON_GetNumberValue(tex) == 12) ||
                      (cJSON_IsString(tex) && !strcmp(cJSON_GetStringValue(tex), "LINK"));
            box.c[1] = ag + aj / 2.0;
            box.h[0] = ai / 2.0;
            box.h[1] = aj / 2.0;
            box.h[2] = ak / 2.0;
        } else if (ao_prefab_is_mesh(prefab)) {
            // 94.js addMesh: y = p.y + s.y / 2 (G += K), halves s / 2; mapDet skips noDetail props
            if (!map_details && ao_prefab_no_detail(prefab)) continue;
            complex = ao_prefab_complex(prefab);
            box.c[1] = ag + aj / 2.0;
            box.h[0] = ai / 2.0;
            box.h[1] = aj / 2.0;
            box.h[2] = ak / 2.0;
        } else if (prefab == PREFAB_PLANE || prefab == PREFAB_BILLBOARD || prefab == PREFAB_RUNE) {
            // 94.js addPlane: flat box (h 0) at p.y (runes + 0.01), w s[0] / 2, l s[2] / 2;
            // these boxes are never complexMesh
            box.c[1] = prefab == PREFAB_RUNE ? ag + 0.01 : ag;
            box.h[0] = ai / 2.0;
            box.h[1] = 0.0;
            box.h[2] = ak / 2.0;
        } else if (!all) {
            // ramps, ladders, zones, flags, pads, pickups, cylinders, gates: complexMesh or
            // gate; signs, spheres, liquids, light cones: no box
            continue;
        } else if (prefab == PREFAB_LADDER) {
            // 94.js addLadder: offset by ladderScale along d, ladderWidth across it
            const cJSON *raw_d = cJSON_GetObjectItem(t, "d");
            const double j = (cJSON_IsNumber(raw_d) ? cJSON_GetNumberValue(raw_d) : 0.0) * M_PI / 2.0;
            double l = game_constants.ladder_scale, m = game_constants.ladder_scale;
            if (j == 0.0 || j == M_PI) m = game_constants.ladder_width;
            else l = game_constants.ladder_width;
            box.c[0] = af + game_constants.ladder_scale * cos(j);
            box.c[2] = ah + game_constants.ladder_scale * sin(j);
            box.c[1] = ag + aj / 2.0;
            box.h[0] = l;
            box.h[1] = aj / 2.0;
            box.h[2] = m;
        } else if (prefab == PREFAB_CYLINDER) {
            box.c[1] = ag + aj / 2.0;
            box.h[0] = box.h[2] = fmin(ai, ak) / 2.0;
            box.h[1] = aj / 2.0;
        } else if (prefab == PREFAB_RAMP || prefab == PREFAB_DEATH_ZONE || prefab == PREFAB_SCORE_ZONE ||
                   prefab == PREFAB_PREMIUM_ZONE || prefab == PREFAB_VERIFIED_ZONE || prefab == PREFAB_TEAM_ZONE ||
                   prefab == PREFAB_CHECK_POINT || prefab == PREFAB_TELEPORTER || prefab == PREFAB_GATE ||
                   prefab == PREFAB_TRIGGER || prefab == PREFAB_TERMINAL || prefab == PREFAB_DEPOSIT_BOX) {
            // 94.js addRamp/zones/gate/trigger/terminal/bank: y + s.y / 2, halves s / 2
            box.c[1] = ag + aj / 2.0;
            box.h[0] = ai / 2.0;
            box.h[1] = aj / 2.0;
            box.h[2] = ak / 2.0;
        } else {
            continue; // flags/pads/pickups (small fixed boxes) and builders without a box
        }

        box.eligible = !no_vis && !complex && !health && !can_destroy && !no_amb;
        if (all || box.eligible) boxes[count++] = box;
    }

    *out_count = count;
    return boxes;
}

// ---------------------------------------------------------------- map.js generate()

static void ao_generate(AoBuild *build, const AoBox *boxes, const size_t count) {
    const double ao = AO_SCALE;
    static const double dirs_xz[4] = {M_PI, 0.0, M_PI / 2.0, -M_PI / 2.0};
    static const double dirs_v[4] = {M_PI, 0.0, -M_PI / 2.0, M_PI / 2.0};
    AoCorner an[4];

    for (size_t a3 = 0; a3 < count; a3++) {
        const AoBox *t = &boxes[a3];

        for (size_t al = a3 + 1; al < count; al++) {
            const AoBox *u = &boxes[al];
            if (!ao_collision(t, u)) continue;

            // vertical edges (footprint corners on x/z)
            if (ao_box_intersection(t, u, AX_X, AX_Z, dirs_xz, an)) {
                for (int ap = 0; ap < 4; ap++) {
                    if (an[ap].dont_use) continue;

                    double qy, qh;
                    if (!ao_get_intersection(t, u, AX_Y, &qy, &qh)) continue;
                    double ar = ao;

                    for (int as = 0; as < 2; as++) {
                        const double a4 = fmod(an[ap].d + M_PI / 2.0 * as, M_PI * 2.0);
                        ar = fmin(ar, ao_limit_amb(ao, &an[ap], a4, AX_X, AX_Z, t, u));
                    }

                    for (int as = 0; as < 2; as++) {
                        const double a4 = fmod(an[ap].d + M_PI / 2.0 * as, M_PI * 2.0);
                        char g1[16], g2[16];
                        snprintf(g1, sizeof(g1), "g1-%d-%d", as, ap);
                        snprintf(g2, sizeof(g2), "g2-%d-%d", as, ap);

                        ao_add_ambient(build, an[ap].v[0] + ar * cos(a4), qy, an[ap].v[2] + ar * sin(a4), -a4 + M_PI, 0.0, 0.0, qh, ar + AO_OFFSET, 1, NULL, NULL, NULL);

                        if (a4 == M_PI / 2.0 || a4 == -M_PI / 2.0 || a4 == M_PI * 1.5) {
                            AoRect at = ao_limit_rect(an[ap].v[2] + ar * sin(a4), qy + ar + qh + AO_OFFSET, ar, ar, 0.0, t, u, AX_Z, AX_Y);
                            ao_add_ambient(build, an[ap].v[0] + ar * cos(a4), at.c[1], at.c[2], -a4 + M_PI, M_PI, 0.0, at.h[2], at.h[1], 0, t, u, g1);
                            at = ao_limit_rect(an[ap].v[2] + ar * sin(a4), qy - ar - qh - AO_OFFSET, ar, ar, 0.0, t, u, AX_Z, AX_Y);
                            ao_add_ambient(build, an[ap].v[0] + ar * cos(a4), at.c[1], at.c[2], -a4 + M_PI, 0.0, 0.0, at.h[2], at.h[1], 0, t, u, g2);
                        } else {
                            AoRect at = ao_limit_rect(an[ap].v[0] + ar * cos(a4), qy + ar + qh + AO_OFFSET, ar, ar, 0.0, t, u, AX_X, AX_Y);
                            ao_add_ambient(build, at.c[0], at.c[1], an[ap].v[2] + ar * sin(a4), -a4 + M_PI, M_PI, 0.0, at.h[0], at.h[1], 0, t, u, g1);
                            at = ao_limit_rect(an[ap].v[0] + ar * cos(a4), qy - ar - qh - AO_OFFSET, ar, ar, 0.0, t, u, AX_X, AX_Y);
                            ao_add_ambient(build, at.c[0], at.c[1], an[ap].v[2] + ar * sin(a4), -a4 + M_PI, 0.0, 0.0, at.h[0], at.h[1], 0, t, u, g2);
                        }
                    }
                }
            }

            // edges along x (corners on z/y)
            if (ao_box_intersection(t, u, AX_Z, AX_Y, dirs_v, an)) {
                for (int ap = 0; ap < 4; ap++) {
                    if (an[ap].dont_use) continue;

                    double qx, qw;
                    if (!ao_get_intersection(t, u, AX_X, &qx, &qw)) continue;
                    double ar = ao;

                    for (int as = 0; as < 2; as++) {
                        const double a4 = an[ap].d + M_PI / 2.0 * (as ? 0 : 1);
                        ar = fmin(ar, ao_limit_amb(ao, &an[ap], -a4 + M_PI / 2.0, AX_Z, AX_Y, t, u));
                    }

                    for (int as = 0; as < 2; as++) {
                        const double a4 = an[ap].d + M_PI / 2.0 * (as ? 0 : 1);
                        const int quarter = a4 == M_PI * 1.5;
                        const int half_or_quarter = a4 == M_PI || a4 == M_PI * 1.5;
                        char g3[16], g4[16];
                        snprintf(g3, sizeof(g3), "g3-%d-%d", as, ap);
                        snprintf(g4, sizeof(g4), "g4-%d-%d", as, ap);

                        ao_add_ambient(build, qx, an[ap].v[1] + ar * cos(a4), an[ap].v[2] + ar * sin(a4), 0.0, a4, -M_PI / 2.0, qw, ar + AO_OFFSET, 1, NULL, NULL, NULL);

                        AoRect at = ao_limit_rect(qx + qw + ar + AO_OFFSET, an[ap].v[2] + ar * sin(a4), ar, ar, 0.0, t, u, AX_X, AX_Z);
                        ao_add_ambient(build, at.c[0], an[ap].v[1] + ar * cos(a4), an[ap].v[2] + ar * sin(a4), M_PI,
                                       a4 == M_PI ? a4 : fmod(-a4, M_PI), quarter ? 0.0 : -M_PI / 2.0,
                                       quarter ? ar : at.h[2], quarter ? at.h[2] : ar, 0, t, u, g3);

                        at = ao_limit_rect(qx - qw - ar - AO_OFFSET, an[ap].v[2] + ar * sin(a4), ar, ar, M_PI / 2.0, t, u, AX_X, AX_Z);
                        ao_add_ambient(build, at.c[0], an[ap].v[1] + ar * cos(a4), an[ap].v[2] + ar * sin(a4), M_PI,
                                       fmod(-a4, M_PI), half_or_quarter ? M_PI / 2.0 : M_PI,
                                       half_or_quarter ? at.h[0] : ar, half_or_quarter ? ar : at.h[0], 0, t, u, g4);
                    }
                }
            }

            // edges along z (corners on x/y)
            if (ao_box_intersection(t, u, AX_X, AX_Y, dirs_v, an)) {
                for (int ap = 0; ap < 4; ap++) {
                    if (an[ap].dont_use) continue;

                    double qz, ql;
                    if (!ao_get_intersection(t, u, AX_Z, &qz, &ql)) continue;
                    double ar = ao;

                    for (int as = 0; as < 2; as++) {
                        const double a4 = an[ap].d + M_PI / 2.0 * (as ? 0 : 1);
                        ar = fmin(ar, ao_limit_amb(ao, &an[ap], -a4 + M_PI / 2.0, AX_X, AX_Y, t, u));
                    }

                    for (int as = 0; as < 2; as++) {
                        const double a4 = an[ap].d + M_PI / 2.0 * (as ? 0 : 1);
                        char g5[16], g6[16];
                        snprintf(g5, sizeof(g5), "g5-%d-%d", as, ap);
                        snprintf(g6, sizeof(g6), "g6-%d-%d", as, ap);

                        ao_add_ambient(build, an[ap].v[0] + ar * sin(a4), an[ap].v[1] + ar * cos(a4), qz, M_PI / 2.0, a4, -M_PI / 2.0, ql, ar + AO_OFFSET, 1, NULL, NULL, NULL);

                        if (a4 == M_PI || a4 == 0.0) {
                            // the JS reads an[ap].z here, which the x/y corners do not have (NaN)
                            AoRect at = ao_limit_rect(NAN, qz + ql + ar + AO_OFFSET, ar, ar, M_PI / 2.0, t, u, AX_X, AX_Z);
                            ao_add_ambient(build, an[ap].v[0] + ar * sin(a4), an[ap].v[1] + ar * cos(a4), at.c[2], M_PI / 2.0, a4, -M_PI / 2.0, at.h[2], at.h[0], 0, t, u, g5);
                            at = ao_limit_rect(NAN, qz - ql - ar - AO_OFFSET, ar, ar, M_PI / 2.0, t, u, AX_X, AX_Z);
                            ao_add_ambient(build, an[ap].v[0] + ar * sin(a4), an[ap].v[1] + ar * cos(a4), at.c[2], M_PI / 2.0, a4, M_PI, at.h[0], at.h[2], 0, t, u, g6);
                        } else {
                            AoRect at = ao_limit_rect(an[ap].v[0] + ar * sin(a4), qz + ql + ar + AO_OFFSET, ar, ar, M_PI / 2.0, t, u, AX_X, AX_Z);
                            ao_add_ambient(build, at.c[0], an[ap].v[1] + ar * cos(a4), at.c[2], M_PI / 2.0, a4, -M_PI / 2.0, at.h[2], at.h[0], 0, t, u, g5);
                            at = ao_limit_rect(an[ap].v[0] + ar * sin(a4), qz - ql - ar - AO_OFFSET, ar, ar, M_PI / 2.0, t, u, AX_X, AX_Z);
                            ao_add_ambient(build, at.c[0], an[ap].v[1] + ar * cos(a4), at.c[2], M_PI / 2.0, a4, M_PI, at.h[0], at.h[2], 0, t, u, g6);
                        }
                    }
                }
            }
        }
    }

    ao_add_pending(build);
}

// ---------------------------------------------------------------- meshes

// T(F, G, H) * Ry(I) * Rx(J) * Rz(K) * S(2M, 2L, 1) applied to the XY unit plane
static void ao_plane_vertices(const AoPlane *plane, vertex out[4]) {
    static const float corners[4][2] = {{-0.5f, -0.5f}, {0.5f, 0.5f}, {-0.5f, 0.5f}, {0.5f, -0.5f}};
    const double cy = cos(plane->rot[0]), sy = sin(plane->rot[0]);
    const double cx = cos(plane->rot[1]), sx = sin(plane->rot[1]);
    const double cz = cos(plane->rot[2]), sz = sin(plane->rot[2]);

    for (int i = 0; i < 4; i++) {
        double x = corners[i][0] * plane->half_m * 2.0;
        double y = corners[i][1] * plane->half_l * 2.0;
        double z = 0.0;

        double t = x * cz - y * sz; // Rz
        y = x * sz + y * cz;
        x = t;

        t = y * cx - z * sx;        // Rx
        z = y * sx + z * cx;
        y = t;

        t = x * cy + z * sy;        // Ry
        z = -x * sy + z * cy;
        x = t;

        out[i].position = (vec3) {(float) (x + plane->pos[0]), (float) (y + plane->pos[1]), (float) (z + plane->pos[2])};
        out[i].tex_coord = (vec2) {corners[i][0] + 0.5f, corners[i][1] + 0.5f}; // PlaneGeometry UVs
    }
}

static Mesh *ao_build_mesh(const AoBuild *build, const int texture, unsigned int *vbo) {
    size_t quads = 0;
    for (size_t i = 0; i < build->plane_count; i++) quads += build->planes[i].texture == texture;
    if (!quads) return NULL;

    vertex *vertices = malloc(quads * 4 * sizeof(vertex));
    unsigned int *indices = malloc(quads * 6 * sizeof(unsigned int));
    Geometry *geometry = calloc(1, sizeof(Geometry));
    BasicMaterial *material = basic_material_init();

    if (!vertices || !indices || !geometry || !material) {
        free(vertices);
        free(indices);
        free(geometry);
        if (material) material_fini((Material *) material);
        return NULL;
    }

    size_t q = 0;
    for (size_t i = 0; i < build->plane_count; i++) {
        if (build->planes[i].texture != texture) continue;

        ao_plane_vertices(&build->planes[i], vertices + q * 4);
        const unsigned int base = (unsigned int) (q * 4);
        const unsigned int quad[6] = {base, base + 1, base + 2, base, base + 3, base + 1};
        memcpy(indices + q * 6, quad, sizeof(quad));
        q++;
    }

    glGenVertexArrays(1, &geometry->vao);
    glGenBuffers(1, &geometry->ebo);
    glGenBuffers(1, vbo);

    glBindVertexArray(geometry->vao);
    glBindBuffer(GL_ARRAY_BUFFER, *vbo);
    glBufferData(GL_ARRAY_BUFFER, (long) (quads * 4 * sizeof(vertex)), vertices, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, geometry->ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (long) (quads * 6 * sizeof(unsigned int)), indices, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), NULL);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *) (3 * sizeof(float)));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    geometry->index_count = (int) (quads * 6);

    free(vertices);
    free(indices);

    char src[64];
    snprintf(src, sizeof(src), "textures/ambient_%d.png", texture);
    char *path = concat(assets_path(), src);
    material->texture = path ? load_texture(path) : 0;
    free(path);

    // 94.js addPlane for addAmbient: transparent, DoubleSide, depthWrite false; Lambert
    // with a black map, so only the fog tints it
    material->base.transparent = 1;
    material->double_sided = 1;
    material->no_depth_write = 1;
    material->ambient_group = 1;

    return mesh_init(geometry, (Material *) material);
}

void ambient_load_map(Scene *scene, const Map *map) {
    ambient_unload_map(scene);
    if (!scene || !map || !map->raw_data) return;

    const ClientRenderSettings *settings = client_render_settings();
    if (settings->no_textures) return; // objects.js addAmbient: nothing with noTex

    // v7.1.4 map.js generate: AO planes only when oldAO || groupData.official (the map's
    // "official" key) || (!hasLM && custom), custom = a map outside the official list or
    // mapIndex 0 not named "Burg" (a custom game's maps = [map]). The v7 official maps ship
    // baked lightmaps and get none unless they set oldAO (freight, lagoon); the v3.1.1 maps,
    // now community maps, keep them. (map-logic.md decision 11 predates the v7 catalogue:
    // KN's official maps are v7's now, so the v7 gate applies as is.)
    const int index = map_catalog_index_of_json(map->raw_data);
    const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(map->raw_data, "name"));
    const int custom = index < 0 || index >= MAP_CATALOG_OFFICIAL_COUNT || (index == 0 && (!name || strcmp(name, "Burg")));
    const int official_key = cJSON_IsTrue(cJSON_GetObjectItem(map->raw_data, "official"));
    if (!map->old_ao && !official_key && !(!map->has_lightmaps && custom)) return;

    const cJSON *objects = cJSON_GetObjectItem(map->raw_data, "objects");
    if (!cJSON_IsArray(objects)) return;

    size_t box_count = 0;
    AoBox *boxes = ao_collect_boxes(map, objects, settings->map_details, 0, &box_count);
    if (!boxes) return;

    AoBuild build = {0};
    ao_generate(&build, boxes, box_count);
    free(boxes);

    for (int texture = 0; texture < 2; texture++) {
        g_ambient_meshes[texture] = ao_build_mesh(&build, texture, &g_ambient_vbos[texture]);
        if (g_ambient_meshes[texture]) scene_add_mesh(scene, g_ambient_meshes[texture]);
    }

    free(build.planes);
}

void ambient_unload_map(Scene *scene) {
    for (int i = 0; i < 2; i++) {
        Mesh *mesh = g_ambient_meshes[i];
        if (!mesh) continue;

        if (scene) scene_remove_mesh(scene, mesh);

        Geometry *geometry = mesh->geometry;
        glDeleteBuffers(1, &g_ambient_vbos[i]);
        g_ambient_vbos[i] = 0;
        glDeleteBuffers(1, &geometry->ebo);
        glDeleteVertexArrays(1, &geometry->vao);
        free(geometry);

        mesh_fini(mesh);
        g_ambient_meshes[i] = NULL;
    }
}

AmbientBox *ambient_map_boxes(const Map *map, size_t *count) {
    *count = 0;
    if (!map || !map->raw_data) return NULL;

    const cJSON *objects = cJSON_GetObjectItem(map->raw_data, "objects");
    if (!cJSON_IsArray(objects)) return NULL;

    size_t box_count = 0;
    AoBox *boxes = ao_collect_boxes(map, objects, client_render_settings()->map_details, 1, &box_count);
    if (!boxes) return NULL;

    AmbientBox *out = calloc(box_count ? box_count : 1, sizeof(AmbientBox));
    if (out) {
        for (size_t i = 0; i < box_count; i++) {
            memcpy(out[i].center, boxes[i].c, sizeof(out[i].center));
            memcpy(out[i].half, boxes[i].h, sizeof(out[i].half));
        }
        *count = box_count;
    }

    free(boxes);
    return out;
}
