// Map editor: the Terrain and Zone right panels (ui/editor.js panel.right.terrain / .zone)
// and what they generate in the 3D view: Editor.regenTerrain (libs/Terrain.js, the
// simplex-noise 2.4.0 heightmap with biome colours and the terrainTSeed clutter of
// data/prefabs.js addTerrObj) and Editor.regenZone (libs/Zone.js, the battle-royale
// cylinder). Both live in g_ed.scene as ordinary BasicMaterial meshes owned here.
//
// Parity notes (intentional differences from v3.1.1):
// - The terrain is lit with per-face normals (basic.frag dFdx/dFdy); three.js merged the
//   vertices and Gouraud-lit smooth vertex normals.
// - The clutter (trees, stumps, loot, rocks, bushes) is one merged geometry. v3.1.1 added
//   every piece as a Mesh without a material: white unlit MeshBasicMaterial, fogged,
//   which is what is drawn here too.
#include "editor_internal.h"
#include <gl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- generator (pure): simplex-noise 2.4.0 seeded by alea, Terrain.js evaluate
// (kept free of GL so it can be checked against the real editor's terrainHeights)

typedef struct {
    double n;
} TerrMash;

static double terr_mash(TerrMash *m, const char *data) {
    // simplex-noise masher(): Johannes Baagoe's Mash on data.toString()
    double n = m->n;
    for (size_t i = 0; data[i]; i++) {
        n += (unsigned char) data[i];
        double h = 0.02519603282416938 * n;
        n = fmod(trunc(h), 4294967296.0); // h >>> 0
        h -= n;
        h *= n;
        n = fmod(trunc(h), 4294967296.0);
        h -= n;
        n += h * 4294967296.0;
    }
    m->n = n;
    return fmod(trunc(n), 4294967296.0) * 2.3283064365386963e-10;
}

typedef struct {
    double s0, s1, s2, c;
} TerrAlea;

static void terr_alea_init(TerrAlea *a, const char *seed) {
    TerrMash m = {4022871197.0}; // 0xefc8249d
    a->c = 1;
    a->s0 = terr_mash(&m, " ");
    a->s1 = terr_mash(&m, " ");
    a->s2 = terr_mash(&m, " ");
    a->s0 -= terr_mash(&m, seed);
    if (a->s0 < 0) a->s0 += 1;
    a->s1 -= terr_mash(&m, seed);
    if (a->s1 < 0) a->s1 += 1;
    a->s2 -= terr_mash(&m, seed);
    if (a->s2 < 0) a->s2 += 1;
}

static double terr_alea_next(TerrAlea *a) {
    const double t = 2091639 * a->s0 + a->c * 2.3283064365386963e-10;
    a->s0 = a->s1;
    a->s1 = a->s2;
    a->c = trunc(t); // t | 0 (t < 2^31)
    return a->s2 = t - a->c;
}

typedef struct {
    uint8_t perm[512], perm_mod12[512];
} TerrSimplex;

static void terr_simplex_init(TerrSimplex *s, const char *seed) {
    // new SimplexNoise(seed): alea(seed); a falsy seed uses Math.random
    TerrAlea a;
    const int seeded = seed && seed[0];
    if (seeded) terr_alea_init(&a, seed);
    uint8_t p[256];
    for (int i = 0; i < 256; i++) p[i] = (uint8_t) i;
    for (int i = 0; i < 255; i++) {
        const double r01 = seeded ? terr_alea_next(&a) : (double) rand() / ((double) RAND_MAX + 1.0);
        const int r = i + (int) (r01 * (256 - i));
        const uint8_t aux = p[i];
        p[i] = p[r];
        p[r] = aux;
    }
    for (int i = 0; i < 512; i++) {
        s->perm[i] = p[i & 255];
        s->perm_mod12[i] = (uint8_t) (s->perm[i] % 12);
    }
}

static const double k_grad3[36] = {1, 1, 0, -1, 1, 0, 1, -1, 0, -1, -1, 0, 1, 0, 1, -1, 0, 1,
                                   1, 0, -1, -1, 0, -1, 0, 1, 1, 0, -1, 1, 0, 1, -1, 0, -1, -1};

static double terr_noise3d(const TerrSimplex *s, const double xin, const double yin, const double zin) {
    const double F3 = 1.0 / 3.0, G3 = 1.0 / 6.0;
    const double sk = (xin + yin + zin) * F3;
    const double fi = floor(xin + sk), fj = floor(yin + sk), fk = floor(zin + sk);
    const double t = (fi + fj + fk) * G3;
    const double x0 = xin - (fi - t), y0 = yin - (fj - t), z0 = zin - (fk - t);
    int i1, j1, k1, i2, j2, k2;
    if (x0 >= y0) {
        if (y0 >= z0) { i1 = 1; j1 = 0; k1 = 0; i2 = 1; j2 = 1; k2 = 0; }
        else if (x0 >= z0) { i1 = 1; j1 = 0; k1 = 0; i2 = 1; j2 = 0; k2 = 1; }
        else { i1 = 0; j1 = 0; k1 = 1; i2 = 1; j2 = 0; k2 = 1; }
    } else {
        if (y0 < z0) { i1 = 0; j1 = 0; k1 = 1; i2 = 0; j2 = 1; k2 = 1; }
        else if (x0 < z0) { i1 = 0; j1 = 1; k1 = 0; i2 = 0; j2 = 1; k2 = 1; }
        else { i1 = 0; j1 = 1; k1 = 0; i2 = 1; j2 = 1; k2 = 0; }
    }
    const double x1 = x0 - i1 + G3, y1 = y0 - j1 + G3, z1 = z0 - k1 + G3;
    const double x2 = x0 - i2 + 2.0 * G3, y2 = y0 - j2 + 2.0 * G3, z2 = z0 - k2 + 2.0 * G3;
    const double x3 = x0 - 1.0 + 3.0 * G3, y3 = y0 - 1.0 + 3.0 * G3, z3 = z0 - 1.0 + 3.0 * G3;
    const int ii = (int) (int64_t) fi & 255, jj = (int) (int64_t) fj & 255, kk = (int) (int64_t) fk & 255;
    const uint8_t *perm = s->perm, *pm = s->perm_mod12;
    double n0 = 0, n1 = 0, n2 = 0, n3 = 0;
    double t0 = 0.6 - x0 * x0 - y0 * y0 - z0 * z0;
    if (t0 >= 0) {
        const int g = pm[ii + perm[jj + perm[kk]]] * 3;
        t0 *= t0;
        n0 = t0 * t0 * (k_grad3[g] * x0 + k_grad3[g + 1] * y0 + k_grad3[g + 2] * z0);
    }
    double t1 = 0.6 - x1 * x1 - y1 * y1 - z1 * z1;
    if (t1 >= 0) {
        const int g = pm[ii + i1 + perm[jj + j1 + perm[kk + k1]]] * 3;
        t1 *= t1;
        n1 = t1 * t1 * (k_grad3[g] * x1 + k_grad3[g + 1] * y1 + k_grad3[g + 2] * z1);
    }
    double t2 = 0.6 - x2 * x2 - y2 * y2 - z2 * z2;
    if (t2 >= 0) {
        const int g = pm[ii + i2 + perm[jj + j2 + perm[kk + k2]]] * 3;
        t2 *= t2;
        n2 = t2 * t2 * (k_grad3[g] * x2 + k_grad3[g + 1] * y2 + k_grad3[g + 2] * z2);
    }
    double t3 = 0.6 - x3 * x3 - y3 * y3 - z3 * z3;
    if (t3 >= 0) {
        const int g = pm[ii + 1 + perm[jj + 1 + perm[kk + 1]]] * 3;
        t3 *= t3;
        n3 = t3 * t3 * (k_grad3[g] * x3 + k_grad3[g + 1] * y3 + k_grad3[g + 2] * z3);
    }
    return 32.0 * (n0 + n1 + n2 + n3);
}

// libs/easing.js, in Terrain.EASE_LIST (Object.keys) order
static double terr_ease(const int f, double t) {
    switch (f) {
        case 0: return t;
        case 1: return t * t;
        case 2: return t * (2 - t);
        case 3: return t < 0.5 ? 2 * t * t : -1 + (4 - 2 * t) * t;
        case 4: return t * t * t;
        case 5: t -= 1; return t * t * t + 1;
        case 6: return t < 0.5 ? 4 * t * t * t : (t - 1) * (2 * t - 2) * (2 * t - 2) + 1;
        case 7: return t * t * t * t;
        case 8: t -= 1; return 1 - t * t * t * t;
        case 9: if (t < 0.5) return 8 * t * t * t * t; t -= 1; return 1 - 8 * t * t * t * t;
        case 10: return t * t * t * t * t;
        case 11: t -= 1; return 1 + t * t * t * t * t;
        default: if (t < 0.5) return 16 * t * t * t * t * t; t -= 1; return 1 + 16 * t * t * t * t * t;
    }
}

static const char *const k_ease_names[13] = {"linear", "easeInQuad", "easeOutQuad", "easeInOutQuad", "easeInCubic", "easeOutCubic",
                                             "easeInOutCubic", "easeInQuart", "easeOutQuart", "easeInOutQuart", "easeInQuint",
                                             "easeOutQuint", "easeInOutQuint"};

// BIOMES (GRASS, MOUNTAIN, SNOW, HILLS) and the colour names evaluateColor picks
enum { B_GRASS, B_MOUNTAIN, B_SNOW, B_HILLS, B_COUNT };
enum { C_DIRT, C_GRASS, C_MOUNTAIN, C_SAND, C_SNOW, C_COUNT };
static const double k_biome_power[B_COUNT] = {2100, 1900, 1000, 2100};
static const double k_biome_slope[B_COUNT] = {0.8, 0.85, 0.75, 0.7}; // slopeSton || 0.85
static const int k_biome_force[B_COUNT] = {-1, -1, C_SNOW, C_GRASS};

// MAP_OBJS (TREE, STUMP, LOOT, ROCK, BUSH, GRASS)
enum { O_TREE, O_STUMP, O_LOOT, O_ROCK, O_BUSH, O_GRASS, O_COUNT };
typedef struct {
    int max;          // 0 = unlimited
    double sample, max_height, rate; // max_height 0 = none; rate < 0 = none
} TerrObjDef;
static const TerrObjDef k_objs[O_COUNT] = {
    {2000, 300, 230, 0.18}, {200, 300, 600, 0.02}, {50, 300, 0, 0.02},
    {500, 300, 400, 0.075}, {0, 400, 200, 0.05}, {0, 400, 200, -1},
};

typedef struct {
    float x, y, z;
    int biome;
} TerrObj;

typedef struct {
    // config
    double size_x, size_y, mnt_mlt, size_mlt, edge_size;
    int ease;
    int gen_objs;
    uint8_t colors[C_COUNT][3];
    TerrSimplex simplex, simplex_objs;
    int sample_index, sample_index_3d;
    // output
    int seg_x, seg_y;          // xSegmentCount / ySegmentCount
    double seg_w, seg_h;       // xSegmentSize / ySegmentSize
    float *heights;            // (seg_x + 1) * (seg_y + 1)
    float *positions;          // seg_x * seg_y * 6 vertices (terrain-local x, y, height)
    uint8_t *vcolors;          // per vertex rgb (Uint8, normalized)
    TerrObj *objs[O_COUNT];
    int obj_count[O_COUNT], obj_cap[O_COUNT];
} TerrGen;

static double terr_sample(TerrGen *g, const double x, const double y, const double s) {
    g->sample_index++;
    return terr_noise3d(&g->simplex, x / s, y / s, g->sample_index * 10000.0) / 2 + 0.5;
}

static double terr_sample_3d(TerrGen *g, const double x, const double y, const double s) {
    g->sample_index_3d++;
    return terr_noise3d(&g->simplex_objs, x / s, y / s, g->sample_index_3d * 10000.0) / 2 + 0.5;
}

static double terr_ease2(const TerrGen *g, const double q) { return terr_ease(g->ease, terr_ease(g->ease, q)); }

static double terr_edge_fade(const TerrGen *g, const double q, const double r) {
    if (fabs(q) >= r / 2) return 0;
    double s = (fabs(q) - r / 2 + g->edge_size) / g->edge_size;
    s = 1 - fmax(s, 0);
    return terr_ease(11, s); // easeOutQuint
}

// evaluate(Q, R): height and the per-biome weights
static double terr_evaluate(TerrGen *g, const double q, const double r, double w[B_COUNT]) {
    g->sample_index = 0;
    double t[B_COUNT], sum = 0;
    for (int i = 0; i < B_COUNT; i++) {
        t[i] = terr_sample(g, q, r, k_biome_power[i]);
        sum += t[i];
    }
    int idx[B_COUNT];
    double val[B_COUNT];
    for (int i = 0; i < B_COUNT; i++) {
        // stable insertion sort, descending (Array.prototype.sort((a, b) => b[1] - a[1]))
        const double v = t[i] / sum;
        int k = i;
        while (k > 0 && val[k - 1] - v < 0) {
            val[k] = val[k - 1];
            idx[k] = idx[k - 1];
            k--;
        }
        val[k] = v;
        idx[k] = i;
    }
    const double y0 = val[0];
    w[idx[0]] = terr_ease2(g, y0 / (y0 + val[1]));
    for (int i = 1; i < B_COUNT; i++) w[idx[i]] = terr_ease2(g, val[i] / (y0 + val[i]));

    double s = 0, u, v, ww;
    // GRASS
    u = terr_sample(g, q, r, 300);
    u = floor(u * 4) / 4;
    s += u * 18 * w[B_GRASS] * g->size_mlt + w[B_GRASS] * 1;
    // MOUNTAIN
    u = 0.4 + terr_sample(g, q, r, 200) * 0.3;
    v = 1 - fabs(u - terr_sample(g, q, r, 500)) * 2;
    ww = 0.4 + terr_sample(g, q, r, 400) * 1.4;
    s += fabs(v * ww * (g->mnt_mlt * 220) * w[B_MOUNTAIN]) * g->size_mlt;
    // SNOW
    u = terr_sample(g, q, r, 300);
    u = floor(u * 3) / 3;
    s += u * 15 * w[B_SNOW] * g->size_mlt + w[B_SNOW] * 90;
    // HILLS
    u = 1 - terr_sample(g, q, r, 2000) * 2;
    v = 0.4 + terr_sample(g, q, r, 400) * 0.4;
    s += fabs(u * v * (g->mnt_mlt * -100) * w[B_HILLS]) * g->size_mlt;

    const double a0 = terr_edge_fade(g, q, g->size_x) * terr_edge_fade(g, r, g->size_y);
    s *= a0;
    s += (a0 - 1) * 2000;
    if (fabs(q) >= g->size_x / 2 || fabs(r) >= g->size_y / 2) s = -1000;
    return s;
}

static int terr_color(const double *c, const double *n, const int biome) {
    // evaluateColor(centroid, normal, biome)
    const double u = c[2];
    const double v = atan2(fabs(n[0]), fabs(n[2]));
    const double w = atan2(fabs(n[1]), fabs(n[2]));
    const double x = 1 - v / (M_PI * 2), y = 1 - w / (M_PI * 2);
    int t = u > 100 ? C_MOUNTAIN : u > 5 ? C_GRASS : C_DIRT;
    if (k_biome_force[biome] >= 0) t = k_biome_force[biome];
    if (u > 950) t = C_SNOW;
    if (u <= -10) t = C_SAND;
    else if (u > 250 || x * y <= k_biome_slope[biome]) t = C_MOUNTAIN;
    return t;
}

static float terr_height(const TerrGen *g, const int q, const int r) {
    // terrainHeight: out-of-range indexes read index 0
    const int i = q + r * (g->seg_x + 1);
    return g->heights[i < 0 || i >= (g->seg_x + 1) * (g->seg_y + 1) ? 0 : i];
}

static double terr_vert_pos(const int q, const double r, const double s) { return q * r - s / 2; }

static double terr_top_angle(const double q, const double r, const double s, const double t, const double u, const double v) {
    double w = atan2(r - t, q - s);
    const double x = atan2(v - t, u - s);
    if (w < 0) w += M_PI * 2;
    return w - x;
}

static double terr_ao(const TerrGen *g, const int q, const int r) {
    // calcAOWeight (its second angle uses xSegmentSize for the lower neighbour, as in v3.1.1)
    const double s = terr_vert_pos(q, g->seg_w, g->size_x), t = terr_vert_pos(r, g->seg_h, g->size_y);
    const double u = terr_top_angle(s - g->seg_w, terr_height(g, q - 1, r), s, terr_height(g, q, r), s + g->seg_w, terr_height(g, q + 1, r));
    const double v = terr_top_angle(t - g->seg_w, terr_height(g, q, r - 1), t, terr_height(g, q, r), t + g->seg_h, terr_height(g, q, r + 1));
    const double w = M_PI;
    double x = 1 - (1 - fmax(w - u, 0) / w) * (1 - fmax(w - v, 0) / w);
    if (x >= 0.5) x = 0.65;
    else if (x >= 0.2) x = 0.3;
    else if (x >= 0.1) x = 0.25;
    return x;
}

static void terr_push_obj(TerrGen *g, const int type, const float x, const float y, const float z, const int biome) {
    if (g->obj_count[type] == g->obj_cap[type]) {
        const int cap = g->obj_cap[type] ? g->obj_cap[type] * 2 : 64;
        TerrObj *n = realloc(g->objs[type], (size_t) cap * sizeof(TerrObj));
        if (!n) return;
        g->objs[type] = n;
        g->obj_cap[type] = cap;
    }
    g->objs[type][g->obj_count[type]++] = (TerrObj) {x, y, z, biome};
}

static void terr_eval_obj(TerrGen *g, const double *q, const double *r, const double *s, const double *t, const int biome, const int type) {
    // evaluateObj(centroid, triangle, biome, name)
    if (q[2] <= -120) return;
    const TerrObjDef *w = &k_objs[type];
    if (w->max && g->obj_count[type] >= w->max) return;
    const double a1 = terr_sample_3d(g, q[0], q[1], w->sample);
    const double x = w->rate;
    if (!(x < 0 || a1 > 1 - x || a1 < x)) return;
    int z = (int) floor(a1); // floor(a1 * (countM || 1)) || 1
    if (!z) z = 1;
    for (int k = 0; k < z; k++) {
        // randomInTriangle(R, S, T, centroid.x, centroid.y)
        const double v = terr_sample_3d(g, q[0], q[1], 400);
        const double ww = sqrt(terr_sample_3d(g, q[0], q[1], 500));
        double y[3];
        for (int i = 0; i < 3; i++) y[i] = r[i] * (1 - ww) + s[i] * (ww * (1 - v)) + t[i] * (v * ww);
        terr_push_obj(g, type, (float) (y[0] * g->size_mlt), (float) y[2], (float) (-y[1] * g->size_mlt), biome);
    }
}

static void terr_hex(const char *s, const char *fallback, uint8_t out[3]) {
    // J(color): "#rgb" / "#rrggbb" -> [r, g, b]
    if (!s || !s[0]) s = fallback;
    if (*s == '#') s++;
    char full[7];
    if (strlen(s) == 3) snprintf(full, sizeof(full), "%c%c%c%c%c%c", s[0], s[0], s[1], s[1], s[2], s[2]);
    else snprintf(full, sizeof(full), "%.6s", s);
    const long v = strtol(full, NULL, 16);
    out[0] = (uint8_t) (v >> 16);
    out[1] = (uint8_t) (v >> 8);
    out[2] = (uint8_t) v;
}

static double clampd(const double v, const double lo, const double hi) { return v < lo ? lo : v > hi ? hi : v; }

static void terr_gen_free(TerrGen *g) {
    free(g->heights);
    free(g->positions);
    free(g->vcolors);
    for (int i = 0; i < O_COUNT; i++) free(g->objs[i]);
    free(g);
}

// new Terrain(...) from mapConfig (Terrain.fromConfig); NULL when out of memory
static TerrGen *terr_generate(const cJSON *mc) {
    TerrGen *g = calloc(1, sizeof(TerrGen));
    if (!g) return NULL;
    g->size_mlt = 5.7;
    g->size_x = clampd(ed_json_num(mc, "terrainWidth", 0), 0, 8000);
    g->size_y = clampd(ed_json_num(mc, "terrainHeight", 0), 0, 8000);
    g->edge_size = 500;
    g->mnt_mlt = clampd(ed_json_num(mc, "terrainMntMlt", 0), 0, 5);
    terr_hex(ed_json_str(mc, "terrainDrtCol", ""), "#8c6835", g->colors[C_DIRT]);
    terr_hex(ed_json_str(mc, "terrainGrsCol", ""), "#4a6904", g->colors[C_GRASS]);
    terr_hex(ed_json_str(mc, "terrainMntCol", ""), "#a0a0a0", g->colors[C_MOUNTAIN]);
    terr_hex(ed_json_str(mc, "terrainSndCol", ""), "#967f51", g->colors[C_SAND]);
    terr_hex(ed_json_str(mc, "terrainSnwCol", ""), "#e8fdff", g->colors[C_SNOW]);
    const int ease = (int) ed_json_num(mc, "terrainEase", 12);
    g->ease = ease >= 0 && ease < 13 ? ease : 12;
    const char *tseed = ed_json_str(mc, "terrainTSeed", "");
    terr_simplex_init(&g->simplex, ed_json_str(mc, "terrainSeed", ""));
    terr_simplex_init(&g->simplex_objs, tseed);
    g->gen_objs = tseed[0] != 0;

    const int a7 = (int) floor(g->size_x / 25), a8 = (int) floor(g->size_y / 25);
    g->seg_x = a7;
    g->seg_y = a8;
    if (a7 <= 0 || a8 <= 0) return g; // an empty terrain (size 0)
    g->seg_w = g->size_x / a7;
    g->seg_h = g->size_y / a8;
    const size_t verts = (size_t) a7 * (size_t) a8 * 6;
    g->heights = calloc((size_t) (a7 + 1) * (size_t) (a8 + 1), sizeof(float));
    g->positions = calloc(verts * 3, sizeof(float));
    g->vcolors = calloc(verts * 3, 1);
    uint8_t *biome = calloc((size_t) (a7 + 1) * (size_t) (a8 + 1), 1);
    if (!g->heights || !g->positions || !g->vcolors || !biome) {
        free(biome);
        terr_gen_free(g);
        return NULL;
    }
    for (int ap = 0; ap < a7 + 1; ap++) {
        for (int aq = 0; aq < a8 + 1; aq++) {
            const int ar = ap + aq * (a7 + 1);
            double w[B_COUNT];
            g->heights[ar] = (float) terr_evaluate(g, terr_vert_pos(ap, g->seg_w, g->size_x), terr_vert_pos(aq, g->seg_h, g->size_y), w);
            int af = 0;
            double ag = 0;
            for (int i = 0; i < B_COUNT; i++) {
                if (w[i] > ag) {
                    ag = w[i];
                    af = i;
                }
            }
            biome[ar] = (uint8_t) af;
        }
    }
    for (int aw = 0; aw < a7; aw++) {
        for (int ax = 0; ax < a8; ax++) {
            const size_t ay = ((size_t) aw + (size_t) ax * (size_t) a7) * 6;
            const double az = terr_vert_pos(aw, g->seg_w, g->size_x), aa = terr_vert_pos(ax, g->seg_h, g->size_y);
            const double aB[3] = {az, aa, terr_height(g, aw, ax)};
            const double aC[3] = {az + g->seg_w, aa, terr_height(g, aw + 1, ax)};
            const double aD[3] = {az + g->seg_w, aa + g->seg_h, terr_height(g, aw + 1, ax + 1)};
            const double aE[3] = {az, aa + g->seg_h, terr_height(g, aw, ax + 1)};
            const double *tri[6] = {aB, aC, aD, aD, aE, aB};
            for (int k = 0; k < 6; k++)
                for (int i = 0; i < 3; i++) g->positions[(ay + (size_t) k) * 3 + (size_t) i] = (float) tri[k][i];
            const int bi = biome[aw + ax * (a7 + 1)];
            double aF[3], aG[3];
            for (int i = 0; i < 3; i++) {
                aF[i] = (aB[i] + aC[i] + aD[i]) / 3;
                aG[i] = (aD[i] + aE[i] + aB[i]) / 3;
            }
            // _normal3D(aB, aC, aD), including its sqrt(Z*Z + a0*a0 + a1 + a1) typo
            const double T = aD[0] - aC[0], U = aD[1] - aC[1], V = aD[2] - aC[2];
            const double W = aB[0] - aC[0], X = aB[1] - aC[1], Y = aB[2] - aC[2];
            const double nz = U * Y - V * X, n0 = V * W - T * Y, n1 = T * X - U * W;
            const double n2 = sqrt(nz * nz + n0 * n0 + n1 + n1);
            const double aH[3] = {nz / n2, n0 / n2, n1 / n2};
            const int col = terr_color(aF, aH, bi);
            const double ak = fmax(aF[2], aG[2]);
            if (g->gen_objs) {
                for (int o = 0; o < O_COUNT; o++) {
                    // biomes: GRASS, SNOW, HILLS for every MAP_OBJS entry but GRASS (GRASS, HILLS)
                    const int in_biome = bi == B_GRASS || bi == B_HILLS || (bi == B_SNOW && o != O_GRASS);
                    if (!in_biome) continue;
                    if (k_objs[o].max_height > 0 && k_objs[o].max_height <= ak) continue;
                    const int ok = o == O_TREE ? col != C_MOUNTAIN && col != C_DIRT && col != C_SAND
                                 : o == O_ROCK ? 1 : o == O_GRASS ? 0 : col != C_MOUNTAIN;
                    if (!ok) continue;
                    terr_eval_obj(g, aF, aB, aC, aD, bi, o);
                    terr_eval_obj(g, aG, aD, aE, aB, bi, o);
                }
            }
            // per-vertex colour: the biome colour darkened by the corner's AO weight
            const double ao[4] = {terr_ao(g, aw, ax), terr_ao(g, aw + 1, ax), terr_ao(g, aw + 1, ax + 1), terr_ao(g, aw, ax + 1)};
            const int corner[6] = {0, 1, 2, 2, 3, 0};
            for (int k = 0; k < 6; k++)
                for (int i = 0; i < 3; i++) {
                    // Uint8Array stores the truncated (wrapped) value of c * (1 - weight)
                    g->vcolors[(ay + (size_t) k) * 3 + (size_t) i] = (uint8_t) (int) (g->colors[col][i] * (1 - ao[corner[k]]));
                }
        }
    }
    free(biome);
    return g;
}

// ---------------------------------------------------------------------------
// geometry building (CPU, world space) and upload

typedef struct {
    vertex *v;
    float *c;          // rgb per vertex
    unsigned int *i;
    size_t nv, ni, cap_v, cap_i;
} TerrBuf;

static int tb_reserve(TerrBuf *b, const size_t nv, const size_t ni) {
    if (b->nv + nv > b->cap_v) {
        size_t cap = b->cap_v ? b->cap_v : 4096;
        while (cap < b->nv + nv) cap *= 2;
        vertex *v = realloc(b->v, cap * sizeof(vertex));
        if (!v) return 0;
        b->v = v;
        float *c = realloc(b->c, cap * 3 * sizeof(float));
        if (!c) return 0;
        b->c = c;
        b->cap_v = cap;
    }
    if (b->ni + ni > b->cap_i) {
        size_t cap = b->cap_i ? b->cap_i : 8192;
        while (cap < b->ni + ni) cap *= 2;
        unsigned int *i = realloc(b->i, cap * sizeof(unsigned int));
        if (!i) return 0;
        b->i = i;
        b->cap_i = cap;
    }
    return 1;
}

static void tb_vertex(TerrBuf *b, const float x, const float y, const float z, const float u, const float v, const float r, const float g, const float bl) {
    b->v[b->nv] = (vertex) {{x, y, z}, {u, v}};
    b->c[b->nv * 3] = r;
    b->c[b->nv * 3 + 1] = g;
    b->c[b->nv * 3 + 2] = bl;
    b->nv++;
}

static void tb_free(TerrBuf *b) {
    free(b->v);
    free(b->c);
    free(b->i);
    memset(b, 0, sizeof(*b));
}

// a world transform for the clutter helpers: p + R(xyz Euler) * (scale * local)
typedef struct {
    float m[9];
    vec3 p, s;
} TerrXf;

static TerrXf xf_make(const float x, const float y, const float z, const vec3 rot, const vec3 scale) {
    TerrXf t;
    ed_euler_matrix(rot, t.m);
    t.p = (vec3) {x, y, z};
    t.s = scale;
    return t;
}

static vec3 xf_apply(const TerrXf *t, const vec3 l) {
    const vec3 a = {l.x * t->s.x, l.y * t->s.y, l.z * t->s.z};
    return (vec3) {t->p.x + t->m[0] * a.x + t->m[1] * a.y + t->m[2] * a.z,
                   t->p.y + t->m[3] * a.x + t->m[4] * a.y + t->m[5] * a.z,
                   t->p.z + t->m[6] * a.x + t->m[7] * a.y + t->m[8] * a.z};
}

static void tb_quad(TerrBuf *b, const TerrXf *t, const vec3 q[4]) {
    if (!tb_reserve(b, 4, 6)) return;
    const unsigned int base = (unsigned int) b->nv;
    for (int k = 0; k < 4; k++) {
        const vec3 w = xf_apply(t, q[k]);
        tb_vertex(b, w.x, w.y, w.z, 0, 0, 1, 1, 1);
    }
    static const unsigned int idx[6] = {0, 1, 2, 0, 2, 3};
    for (int k = 0; k < 6; k++) b->i[b->ni++] = base + idx[k];
}

// J(): generateCube(faces) scaled to (w, h, d), foot at (x, y, z); faces +x -x +y -y +z -z
static void tb_cube(TerrBuf *b, const float x, const float y, const float z, const float w, const float h, const float d,
                    const int faces[6], const vec3 rot) {
    const TerrXf t = xf_make(x, y, z, rot, (vec3) {w, h, d});
    static const vec3 q[6][4] = {
        {{0.5f, 0, 0.5f}, {0.5f, 0, -0.5f}, {0.5f, 1, -0.5f}, {0.5f, 1, 0.5f}},
        {{-0.5f, 0, -0.5f}, {-0.5f, 0, 0.5f}, {-0.5f, 1, 0.5f}, {-0.5f, 1, -0.5f}},
        {{-0.5f, 1, 0.5f}, {0.5f, 1, 0.5f}, {0.5f, 1, -0.5f}, {-0.5f, 1, -0.5f}},
        {{-0.5f, 0, -0.5f}, {0.5f, 0, -0.5f}, {0.5f, 0, 0.5f}, {-0.5f, 0, 0.5f}},
        {{-0.5f, 0, 0.5f}, {0.5f, 0, 0.5f}, {0.5f, 1, 0.5f}, {-0.5f, 1, 0.5f}},
        {{0.5f, 0, -0.5f}, {-0.5f, 0, -0.5f}, {-0.5f, 1, -0.5f}, {0.5f, 1, -0.5f}},
    };
    for (int f = 0; f < 6; f++) if (faces[f]) tb_quad(b, &t, q[f]);
}

// generateSphere: SphereGeometry(1, 12, 8), every vertex moved by random() * vNoise on all
// three axes; geos.js caches the noisy geometry per option set, so one per vNoise here too
#define SPHERE_W 12
#define SPHERE_H 8
typedef struct {
    float noise;
    vec3 v[(SPHERE_W + 1) * (SPHERE_H + 1)];
} TerrSphere;

static TerrSphere g_spheres[8];
static int g_sphere_count;

static const TerrSphere *sphere_for(const float noise) {
    for (int i = 0; i < g_sphere_count; i++) if (g_spheres[i].noise == noise) return &g_spheres[i];
    TerrSphere *s = &g_spheres[g_sphere_count < 8 ? g_sphere_count++ : 7];
    s->noise = noise;
    for (int iy = 0; iy <= SPHERE_H; iy++) {
        const float v = (float) iy / SPHERE_H;
        for (int ix = 0; ix <= SPHERE_W; ix++) {
            const float u = (float) ix / SPHERE_W;
            const float j = (float) ((double) rand() / ((double) RAND_MAX + 1.0)) * noise;
            s->v[iy * (SPHERE_W + 1) + ix] = (vec3) {-cosf(u * 2 * (float) M_PI) * sinf(v * (float) M_PI) + j, cosf(v * (float) M_PI) + j,
                                                      sinf(u * 2 * (float) M_PI) * sinf(v * (float) M_PI) + j};
        }
    }
    return s;
}

// K(): the sphere at (x, y + h / 2, z) scaled to (w / 2, h / 2, d / 2), rotation (yR, xR, zR)
static void tb_sphere(TerrBuf *b, const float x, const float y, const float z, const float w, const float h, const float d,
                      const float noise, const vec3 rot) {
    const TerrSphere *s = sphere_for(noise);
    const TerrXf t = xf_make(x, y + h / 2, z, rot, (vec3) {w / 2, h / 2, d / 2});
    const size_t n = (SPHERE_W + 1) * (SPHERE_H + 1);
    if (!tb_reserve(b, n, SPHERE_W * SPHERE_H * 6)) return;
    const unsigned int base = (unsigned int) b->nv;
    for (size_t k = 0; k < n; k++) {
        const vec3 p = xf_apply(&t, s->v[k]);
        tb_vertex(b, p.x, p.y, p.z, 0, 0, 1, 1, 1);
    }
    for (int iy = 0; iy < SPHERE_H; iy++)
        for (int ix = 0; ix < SPHERE_W; ix++) {
            const unsigned int a = base + (unsigned int) (iy * (SPHERE_W + 1) + ix + 1), bb = base + (unsigned int) (iy * (SPHERE_W + 1) + ix);
            const unsigned int c = base + (unsigned int) ((iy + 1) * (SPHERE_W + 1) + ix), dd = base + (unsigned int) ((iy + 1) * (SPHERE_W + 1) + ix + 1);
            if (iy != 0) { b->i[b->ni++] = a; b->i[b->ni++] = bb; b->i[b->ni++] = dd; }
            if (iy != SPHERE_H - 1) { b->i[b->ni++] = bb; b->i[b->ni++] = c; b->i[b->ni++] = dd; }
        }
}

// L(): CylinderGeometry(1, 1, 1, 12, 1, false) translated up 0.5, scale (r, h, r), r = min(w, d) / 2
static void tb_cylinder(TerrBuf *b, const float x, const float y, const float z, const float w, const float h, const float d) {
    const float r = fminf(w, d) / 2;
    const TerrXf t = xf_make(x, y, z, (vec3) {0, 0, 0}, (vec3) {r, h, r});
    const int seg = 12;
    if (!tb_reserve(b, (size_t) seg * 4 + 2, (size_t) seg * 12)) return;
    const unsigned int base = (unsigned int) b->nv;
    for (int k = 0; k < seg; k++) {
        const float th = (float) k / seg * 2 * (float) M_PI;
        const vec3 top = xf_apply(&t, (vec3) {sinf(th), 1, cosf(th)}), bot = xf_apply(&t, (vec3) {sinf(th), 0, cosf(th)});
        tb_vertex(b, top.x, top.y, top.z, 0, 0, 1, 1, 1);
        tb_vertex(b, bot.x, bot.y, bot.z, 0, 0, 1, 1, 1);
    }
    const vec3 ct = xf_apply(&t, (vec3) {0, 1, 0}), cb = xf_apply(&t, (vec3) {0, 0, 0});
    tb_vertex(b, ct.x, ct.y, ct.z, 0, 0, 1, 1, 1);
    tb_vertex(b, cb.x, cb.y, cb.z, 0, 0, 1, 1, 1);
    const unsigned int c_top = base + (unsigned int) seg * 2, c_bot = c_top + 1;
    for (int k = 0; k < seg; k++) {
        const unsigned int t0 = base + (unsigned int) k * 2, b0 = t0 + 1;
        const unsigned int t1 = base + (unsigned int) ((k + 1) % seg) * 2, b1 = t1 + 1;
        const unsigned int tri[12] = {t0, b0, t1, b0, b1, t1, c_top, t0, t1, c_bot, b1, b0};
        for (int q = 0; q < 12; q++) b->i[b->ni++] = tri[q];
    }
}

// data/prefabs.js addTerrObj -> addTREE / addSTUMP / addLOOT / addROCK / addBUSH (GRASS adds nothing)
static int mod_round(const float a, const float b, const int m) { return (int) lround(fabs(fmod((double) a + b, m))); }

static void clutter_add(TerrBuf *b, const int type, const TerrObj *n) {
    static const int all[6] = {1, 1, 1, 1, 1, 1}, trunk[6] = {1, 1, 0, 0, 1, 1}, open_bottom[6] = {1, 1, 1, 0, 1, 1};
    const vec3 none = {0, 0, 0};
    switch (type) {
        case O_TREE: {
            // B: Square Oak (rar 6..10), Big Oak (5), Tall Italian (1..4), Honey Oak (0)
            const int q = mod_round(n->x, n->z, 10);
            const int kind = q >= 6 ? 0 : q == 5 ? 1 : q >= 1 ? 2 : 3;
            static const float heights[4] = {58, 70, 37, 100};
            static const float scales[4][2] = {{4, 5}, {6, 6.2f}, {3, 3.2f}, {4, 5}};
            const float s = heights[kind] - (float) q * 2, t = scales[kind][q % 2];
            tb_cube(b, n->x, n->y, n->z, t, s + 20, t, trunk, none);
            const vec3 yaw = {0, (float) q, 0}; // xR: P -> rotation.y
            if (kind == 0) tb_sphere(b, n->x, n->y + s - 1, n->z, 90, 100, 90, 0.36f, yaw);
            else if (kind == 1) tb_sphere(b, n->x, n->y + s - 1, n->z, 130, 100, 130, 0.3f, yaw);
            else if (kind == 2) tb_sphere(b, n->x, n->y + s - 6, n->z, 33, 80, 33, 0.3f, yaw);
            else {
                tb_sphere(b, n->x, n->y + s - 1, n->z, 55, 56, 55, 0.35f, yaw);
                tb_cube(b, n->x + 10, n->y + s / 1.5f + 5, n->z, 22, 3, 2, all, (vec3) {0, 0, 0.4f});
                tb_sphere(b, n->x + 20, n->y + s / 1.5f, n->z, 22, 20, 22, 0.25f, none);
            }
            break;
        }
        case O_STUMP:
            tb_cube(b, n->x, n->y, n->z, 4, 80, 4, open_bottom, none);
            tb_cube(b, n->x, n->y + 30, n->z + 10, 2, 3, 22, all, (vec3) {-0.5f, 0, 0});
            tb_cube(b, n->x + 10, n->y + 50, n->z, 22, 3, 2, all, (vec3) {0, 0, 0.4f});
            break;
        case O_LOOT: {
            const int q = mod_round(n->x, n->z, 3);
            tb_cube(b, n->x + 6, n->y, n->z + 15, 18, 1, 3, open_bottom, (vec3) {0, 0.86f, 0});
            if (q == 0 || q == 1) tb_cylinder(b, n->x, n->y, n->z, 7, 7, 9);
            if (q == 2 || q == 1) tb_cylinder(b, n->x - 7, n->y, n->z + 12, 7, 7, 9);
            break;
        }
        case O_ROCK: {
            static const float rocks[4][3] = {{25, 20, 0.2f}, {50, 30, 0.3f}, {13, 10, 0.15f}, {10, 10, 0.1f}}; // scl, hei, nois
            const float *s = rocks[mod_round(n->x, n->z, 10) % 4];
            tb_sphere(b, n->x, n->y - 7, n->z, s[0], s[1], s[0], s[2], none);
            break;
        }
        case O_BUSH: {
            const int r = mod_round(n->x, n->z, 10);
            const float s = n->biome == B_SNOW ? 0.5f : 1.0f;
            tb_sphere(b, n->x, n->y - s * 6, n->z, s * 29, s * 21, s * 28, 0.3f, (vec3) {0, (float) r, 0});
            break;
        }
        default: break;
    }
}

typedef struct {
    Geometry *geometry;
    unsigned int vbo, cbo;
    Mesh *mesh;
} TerrPart;

static TerrPart g_terrain, g_clutter, g_zone;

static Geometry *upload(const TerrBuf *b, unsigned int *vbo, unsigned int *cbo) {
    if (!b->nv || !b->ni) return NULL;
    Geometry *g = calloc(1, sizeof(Geometry));
    if (!g) return NULL;
    glGenVertexArrays(1, &g->vao);
    glGenBuffers(1, &g->ebo);
    glGenBuffers(1, vbo);
    glGenBuffers(1, cbo);
    glBindVertexArray(g->vao);
    glBindBuffer(GL_ARRAY_BUFFER, *vbo);
    glBufferData(GL_ARRAY_BUFFER, (long) (b->nv * sizeof(vertex)), b->v, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(vertex), NULL);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(vertex), (void *) (3 * sizeof(float)));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glBindBuffer(GL_ARRAY_BUFFER, *cbo);
    glBufferData(GL_ARRAY_BUFFER, (long) (b->nv * 3 * sizeof(float)), b->c, GL_STATIC_DRAW);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), NULL);
    glEnableVertexAttribArray(2);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g->ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (long) (b->ni * sizeof(unsigned int)), b->i, GL_STATIC_DRAW);
    glBindVertexArray(0);
    geometry_set_bounds(g, b->v, b->nv);
    g->index_count = (int) b->ni;
    return g;
}

static void part_drop(TerrPart *p) {
    if (p->mesh) {
        if (g_ed.scene) scene_remove_mesh(g_ed.scene, p->mesh);
        mesh_fini(p->mesh); // frees the material
    }
    if (p->geometry) {
        glDeleteVertexArrays(1, &p->geometry->vao);
        glDeleteBuffers(1, &p->geometry->ebo);
        glDeleteBuffers(1, &p->vbo);
        glDeleteBuffers(1, &p->cbo);
        free(p->geometry);
    }
    memset(p, 0, sizeof(*p));
}

static void part_set(TerrPart *p, const TerrBuf *b, BasicMaterial *m) {
    p->geometry = upload(b, &p->vbo, &p->cbo);
    if (!p->geometry || !m) {
        free(m);
        part_drop(p);
        return;
    }
    p->mesh = mesh_init(p->geometry, (Material *) m);
    p->mesh->no_shadow_receive = 1;
    if (p->mesh && g_ed.scene) scene_add_mesh(g_ed.scene, p->mesh);
}

// Editor.regenTerrain: remove the old terrain; mapConfig.terrainSeed set -> a new one
void ed_terrain_regen(void) {
    part_drop(&g_terrain);
    part_drop(&g_clutter);
    const cJSON *mc = g_ed.map_config;
    if (!mc || !ed_json_str(mc, "terrainSeed", "")[0]) return;
    TerrGen *g = terr_generate(mc);
    if (!g) return;
    TerrBuf b = {0};
    const size_t verts = (size_t) g->seg_x * (size_t) g->seg_y * 6;
    if (verts && tb_reserve(&b, verts, verts)) {
        // baseMesh.scale (5.7, 5.7, 1) inside terrain.rotation.x = -PI/2: (x, y, h) -> (5.7x, h, -5.7y)
        for (size_t k = 0; k < verts; k++) {
            const float *p = &g->positions[k * 3];
            const uint8_t *c = &g->vcolors[k * 3];
            tb_vertex(&b, p[0] * 5.7f, p[2], -p[1] * 5.7f, 0, 0, c[0] / 255.0f, c[1] / 255.0f, c[2] / 255.0f);
            b.i[b.ni++] = (unsigned int) k;
        }
        BasicMaterial *m = basic_material_init(); // StrippedLambertMaterial(color 0x8c8c8c, vertexColors)
        if (m) {
            m->color = (vec4) {0x8c / 255.0f, 0x8c / 255.0f, 0x8c / 255.0f, 1};
            m->use_vertex_color = 1;
        }
        part_set(&g_terrain, &b, m);
    }
    tb_free(&b);
    if (g->gen_objs) {
        for (int o = 0; o < O_COUNT; o++)
            for (int k = 0; k < g->obj_count[o]; k++) clutter_add(&b, o, &g->objs[o][k]);
        BasicMaterial *m = basic_material_init(); // new THREE.Mesh(geometry): white MeshBasicMaterial
        if (m) m->unlit = 1;
        part_set(&g_clutter, &b, m);
        tb_free(&b);
    }
    terr_gen_free(g);
}

static void zone_color(const char *s, const char *fallback, float out[3]) {
    uint8_t c[3];
    terr_hex(s, fallback, c);
    for (int i = 0; i < 3; i++) out[i] = c[i] / 255.0f;
}

// Editor.regenZone: Zone.fromConfig(mapConfig, true) when mapConfig.zone
void ed_zone_regen(void) {
    part_drop(&g_zone);
    const cJSON *mc = g_ed.map_config;
    if (!mc || !ed_json_truthy(cJSON_GetObjectItemCaseSensitive(mc, "zone"))) return;
    double size = ed_json_num(mc, "zoneSize", 0);
    if (!size) size = 50; // zoneSize || 50
    size = clampd(size, 0, 10000);
    // colour stops 0 / 0.5 / 1 from the top: zoneCol0 || COLORS.zones.BR[0], ...
    float cols[3][3];
    zone_color(ed_json_str(mc, "zoneCol0", ""), "#c542d9", cols[0]);
    zone_color(ed_json_str(mc, "zoneCol1", ""), "#c542d9", cols[1]);
    zone_color(ed_json_str(mc, "zoneCol2", ""), "#c542d9", cols[2]);
    // CylinderGeometry(1, 1, 1, 32, 32, true), scale (size, 1000, size), at the origin
    const int rs = 32, hs = 32;
    TerrBuf b = {0};
    if (tb_reserve(&b, (size_t) (rs + 1) * (hs + 1), (size_t) rs * hs * 6)) {
        for (int y = 0; y <= hs; y++) {
            const float v = (float) y / hs; // 0 at the top
            const float t = v <= 0.5f ? v / 0.5f : (v - 0.5f) / 0.5f;
            const float *c0 = v <= 0.5f ? cols[0] : cols[1], *c1 = v <= 0.5f ? cols[1] : cols[2];
            for (int x = 0; x <= rs; x++) {
                const float u = (float) x / rs, th = u * 2 * (float) M_PI;
                tb_vertex(&b, (float) size * sinf(th), (0.5f - v) * 1000.0f, (float) size * cosf(th), u, 1 - v,
                          c0[0] + (c1[0] - c0[0]) * t, c0[1] + (c1[1] - c0[1]) * t, c0[2] + (c1[2] - c0[2]) * t);
            }
        }
        for (int x = 0; x < rs; x++)
            for (int y = 0; y < hs; y++) {
                const unsigned int a = (unsigned int) (y * (rs + 1) + x), bb = (unsigned int) ((y + 1) * (rs + 1) + x);
                const unsigned int c = bb + 1, d = a + 1;
                const unsigned int tri[6] = {a, bb, d, bb, c, d};
                for (int k = 0; k < 6; k++) b.i[b.ni++] = tri[k];
            }
        BasicMaterial *m = basic_material_init(); // MeshBasicMaterial(map zone_0, vertexColors, DoubleSide, opacity 0.5)
        if (m) {
            char *path = concat(assets_path(), "textures/zone_0.png");
            m->texture = path ? load_texture(path) : 0;
            free(path);
            m->texture_repeat = (vec2) {20, 20};
            m->color.w = 0.5f;
            m->unlit = 1;
            m->use_vertex_color = 1;
            m->double_sided = 1;
            m->no_depth_write = 1;
            m->base.transparent = 1;
        }
        part_set(&g_zone, &b, m);
    }
    tb_free(&b);
}

void ed_terrain_fini(void) {
    part_drop(&g_terrain);
    part_drop(&g_clutter);
    part_drop(&g_zone);
}

// ---------------------------------------------------------------------------
// panels (ui/editor.js panel.right.terrain / panel.right.zone)

static EdCtl *bound(EdBuild *b, EdCtl *c, const char *key, const EdChangeFn fn) {
    ed_ctl_bind_json(c, g_ed.map_config, key);
    c->on_change = fn;
    ed_gui_emit_value(b, c);
    return c;
}

static void on_terrain(EdCtl *c, const cJSON *v) { (void) c; (void) v; ed_terrain_regen(); }
static void on_zone(EdCtl *c, const cJSON *v) { (void) c; (void) v; ed_zone_regen(); }

static void on_ease(EdCtl *c, const cJSON *v) {
    (void) c; (void) v;
    ed_panel_right_show("terrain"); // y.panel.state.refresh("panel_right_terrain")
    ed_terrain_regen();
}

static void random_seed(const char *key) {
    // Math.random().toString(36).substring(4): base-36 digits of a random fraction minus "0.xx"
    char s[16];
    double f = (double) rand() / ((double) RAND_MAX + 1.0);
    int n = 0;
    for (int i = 0; i < 13 && n + 1 < (int) sizeof(s); i++) {
        f *= 36;
        const int d = (int) f;
        f -= d;
        if (i >= 2) s[n++] = "0123456789abcdefghijklmnopqrstuvwxyz"[d];
    }
    s[n] = 0;
    ed_json_set_str(g_ed.map_config, key, s);
    g_ed.dirty_storage = 1;
    ed_panel_right_show("terrain");
    ed_terrain_regen();
}

static void act_seed(EdCtl *c) { (void) c; random_seed("terrainSeed"); }
static void act_tseed(EdCtl *c) { (void) c; random_seed("terrainTSeed"); }

static void panel_terrain(void) {
    EdBuild b;
    if (!ed_panel_right_begin(&b, "terrain")) return;
    ed_gui_panel_buttons(&b, 1, "Terrain", 0);
    bound(&b, ed_gui_text(&b, "Seed", "Seed", 0), "terrainSeed", on_terrain);
    ed_gui_button_line(&b, "Random", "Generate", act_seed, 1);
    bound(&b, ed_gui_text(&b, "Tree Seed", "Seed", 0), "terrainTSeed", on_terrain);
    ed_gui_button_line(&b, "Random", "Generate", act_tseed, 1);
    static EdOption eases[13];
    for (int i = 0; i < 13; i++) eases[i] = (EdOption) {k_ease_names[i], i, NULL};
    bound(&b, ed_gui_select(&b, "Easing", eases, 13, 1), "terrainEase", on_ease);
    ed_gui_rule(&b);
    ed_gui_folder_begin(&b, "Dimensions", 0, 0, 0, "terrain.dimensions");
    bound(&b, ed_gui_slider(&b, "Width", 0, 8000, 1, 0), "terrainWidth", on_terrain);
    bound(&b, ed_gui_slider(&b, "Length", 0, 8000, 1, 0), "terrainHeight", on_terrain);
    bound(&b, ed_gui_slider(&b, "Height", 0, 5, 0.05, 0), "terrainMntMlt", on_terrain);
    ed_gui_folder_end(&b, 0);
    ed_gui_folder_begin(&b, "Style", 0, 0, 0, "terrain.style");
    ed_gui_folder_begin(&b, "Color", 1, 0, 0, "terrain.style.color");
    bound(&b, ed_gui_color(&b, "Snow", 1), "terrainSnwCol", on_terrain);
    bound(&b, ed_gui_color(&b, "Mountain", 1), "terrainMntCol", on_terrain);
    bound(&b, ed_gui_color(&b, "Grass", 1), "terrainGrsCol", on_terrain);
    bound(&b, ed_gui_color(&b, "Dirt", 1), "terrainDrtCol", on_terrain);
    bound(&b, ed_gui_color(&b, "Sand", 1), "terrainSndCol", on_terrain);
    ed_gui_folder_end(&b, 1);
    ed_gui_folder_end(&b, 0);
    ed_gui_end(&b, g_ed.el_right_list);
}

static void panel_zone(void) {
    EdBuild b;
    if (!ed_panel_right_begin(&b, "zone")) return;
    ed_gui_panel_buttons(&b, 1, "Zone", 0);
    bound(&b, ed_gui_switch(&b, "Enabled", 0), "zone", on_zone);
    bound(&b, ed_gui_slider(&b, "Speed", 1, 10, 0.1, 0), "zoneSpeed", NULL);
    bound(&b, ed_gui_switch(&b, "Random Endpoint", 0), "zoneRnd", on_zone);
    ed_gui_rule(&b);
    ed_gui_folder_begin(&b, "Dimensions", 0, 0, 0, "zone.dimensions");
    bound(&b, ed_gui_slider(&b, "Size", 0, 10000, 10, 0), "zoneSize", on_zone);
    ed_gui_folder_end(&b, 0);
    ed_gui_folder_begin(&b, "Style", 0, 0, 0, "zone.style");
    ed_gui_folder_begin(&b, "Color", 1, 0, 0, "zone.style.color");
    bound(&b, ed_gui_color(&b, "1", 1), "zoneCol0", on_zone);
    bound(&b, ed_gui_color(&b, "2", 1), "zoneCol1", on_zone);
    bound(&b, ed_gui_color(&b, "3", 1), "zoneCol2", on_zone);
    ed_gui_folder_end(&b, 1);
    ed_gui_folder_end(&b, 0);
    ed_gui_end(&b, g_ed.el_right_list);
}

int ed_terrain_panel_show(const char *name) {
    if (!strcmp(name, "terrain")) panel_terrain();
    else if (!strcmp(name, "zone")) panel_zone();
    else return 0;
    return 1;
}
