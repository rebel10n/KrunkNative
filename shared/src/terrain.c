// map-logic WP-B: v7.1.4 libs/Terrain.js (doc/v7/map-logic.md §1.8.1), shared by the server, the
// offline sandbox, the client and the map editor (moved here from client/src/editor/editor_terrain.c).
// v7 builds the noise with createNoise3D(alea(seed + "")) (libs/noise.js, shim/alea.js); v3.1.1
// used simplex-noise 2.4 seeded the same way. Both build the same permutation table (Fisher-Yates
// over 256 with alea) and the same 0.6-falloff x32 noise3D, so this port matches either.
#include <terrain.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- alea (Johannes Baagoe) seeded from the seed's JS string

typedef struct {
    double n;
} TerrMash;

static double terr_mash(TerrMash *m, const char *data) {
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

// noise.js buildPermutationTable(alea(seed))
static void terr_perm_init(uint8_t perm[512], const char *seed) {
    TerrAlea a;
    terr_alea_init(&a, seed);
    for (int i = 0; i < 256; i++) perm[i] = (uint8_t) i;
    for (int i = 0; i < 255; i++) {
        const int r = i + (int) (terr_alea_next(&a) * (256 - i));
        const uint8_t aux = perm[i];
        perm[i] = perm[r];
        perm[r] = aux;
    }
    for (int i = 256; i < 512; i++) perm[i] = perm[i - 256];
}

static const double k_grad3[36] = {1, 1, 0, -1, 1, 0, 1, -1, 0, -1, -1, 0, 1, 0, 1, -1, 0, 1,
                                   1, 0, -1, -1, 0, -1, 0, 1, 1, 0, -1, 1, 0, 1, -1, 0, -1, -1};

// noise.js createNoise3D (permGrad3x/y/z = grad3[perm % 12 * 3 + k])
static double terr_noise3d(const uint8_t *perm, const double xin, const double yin, const double zin) {
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
    double n0 = 0, n1 = 0, n2 = 0, n3 = 0;
    double t0 = 0.6 - x0 * x0 - y0 * y0 - z0 * z0;
    if (t0 >= 0) {
        const int g = perm[ii + perm[jj + perm[kk]]] % 12 * 3;
        t0 *= t0;
        n0 = t0 * t0 * (k_grad3[g] * x0 + k_grad3[g + 1] * y0 + k_grad3[g + 2] * z0);
    }
    double t1 = 0.6 - x1 * x1 - y1 * y1 - z1 * z1;
    if (t1 >= 0) {
        const int g = perm[ii + i1 + perm[jj + j1 + perm[kk + k1]]] % 12 * 3;
        t1 *= t1;
        n1 = t1 * t1 * (k_grad3[g] * x1 + k_grad3[g + 1] * y1 + k_grad3[g + 2] * z1);
    }
    double t2 = 0.6 - x2 * x2 - y2 * y2 - z2 * z2;
    if (t2 >= 0) {
        const int g = perm[ii + i2 + perm[jj + j2 + perm[kk + k2]]] % 12 * 3;
        t2 *= t2;
        n2 = t2 * t2 * (k_grad3[g] * x2 + k_grad3[g + 1] * y2 + k_grad3[g + 2] * z2);
    }
    double t3 = 0.6 - x3 * x3 - y3 * y3 - z3 * z3;
    if (t3 >= 0) {
        const int g = perm[ii + 1 + perm[jj + 1 + perm[kk + 1]]] % 12 * 3;
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

static const double k_biome_power[TERRAIN_BIOME_COUNT] = {2100, 1900, 1000, 2100};
static const double k_biome_slope[TERRAIN_BIOME_COUNT] = {0.8, 0.85, 0.75, 0.7}; // slopeSton || 0.85
static const int k_biome_force[TERRAIN_BIOME_COUNT] = {-1, -1, TERRAIN_COLOR_SNOW, TERRAIN_COLOR_GRASS};

typedef struct {
    int max;          // 0 = unlimited
    double sample, max_height, rate; // max_height 0 = none; rate < 0 = none (GRASS has no rate)
} TerrObjDef;
static const TerrObjDef k_objs[TERRAIN_OBJ_COUNT] = {
    {2000, 300, 230, 0.18}, {200, 300, 600, 0.02}, {50, 300, 0, 0.02},
    {500, 300, 400, 0.075}, {0, 400, 200, 0.05}, {0, 400, 200, -1},
};

static double terr_sample(Terrain *g, const double x, const double y, const double s) {
    g->sample_index++;
    return terr_noise3d(g->perm[0], x / s, y / s, g->sample_index * 10000.0) / 2 + 0.5;
}

static double terr_sample_3d(Terrain *g, const double x, const double y, const double s) {
    g->sample_index_3d++;
    return terr_noise3d(g->perm[1], x / s, y / s, g->sample_index_3d * 10000.0) / 2 + 0.5;
}

static double terr_ease2(const Terrain *g, const double q) { return terr_ease(g->ease, terr_ease(g->ease, q)); }

static double terr_edge_fade(const Terrain *g, const double q, const double r) {
    if (fabs(q) >= r / 2) return 0;
    double s = (fabs(q) - r / 2 + g->edge_size) / g->edge_size;
    s = 1 - fmax(s, 0);
    return terr_ease(11, s); // easeOutQuint
}

// evaluate(Q, R): height and the per-biome weights (sampleIndex restarts: call order matters)
static double terr_evaluate(Terrain *g, const double q, const double r, double w[TERRAIN_BIOME_COUNT]) {
    g->sample_index = 0;
    double t[TERRAIN_BIOME_COUNT], sum = 0;
    for (int i = 0; i < TERRAIN_BIOME_COUNT; i++) {
        t[i] = terr_sample(g, q, r, k_biome_power[i]);
        sum += t[i];
    }
    int idx[TERRAIN_BIOME_COUNT];
    double val[TERRAIN_BIOME_COUNT];
    for (int i = 0; i < TERRAIN_BIOME_COUNT; i++) {
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
    for (int i = 1; i < TERRAIN_BIOME_COUNT; i++) w[idx[i]] = terr_ease2(g, val[i] / (y0 + val[i]));

    double s = 0, u, v, ww;
    // GRASS
    u = terr_sample(g, q, r, 300);
    u = floor(u * 4) / 4;
    s += u * 18 * w[TERRAIN_BIOME_GRASS] * g->size_mlt + w[TERRAIN_BIOME_GRASS] * 1;
    // MOUNTAIN
    u = 0.4 + terr_sample(g, q, r, 200) * 0.3;
    v = 1 - fabs(u - terr_sample(g, q, r, 500)) * 2;
    ww = 0.4 + terr_sample(g, q, r, 400) * 1.4;
    s += fabs(v * ww * (g->mnt_mlt * 220) * w[TERRAIN_BIOME_MOUNTAIN]) * g->size_mlt;
    // SNOW
    u = terr_sample(g, q, r, 300);
    u = floor(u * 3) / 3;
    s += u * 15 * w[TERRAIN_BIOME_SNOW] * g->size_mlt + w[TERRAIN_BIOME_SNOW] * 90;
    // HILLS
    u = 1 - terr_sample(g, q, r, 2000) * 2;
    v = 0.4 + terr_sample(g, q, r, 400) * 0.4;
    s += fabs(u * v * (g->mnt_mlt * -100) * w[TERRAIN_BIOME_HILLS]) * g->size_mlt;

    const double a0 = terr_edge_fade(g, q, g->size_x) * terr_edge_fade(g, r, g->size_y);
    s *= a0;
    s += (a0 - 1) * 2000;
    if (fabs(q) >= g->size_x / 2 || fabs(r) >= g->size_y / 2) s = -1000;
    return s;
}

// evaluateColor(centroid, normal, biome)
static int terr_color(const double *c, const double *n, const int biome) {
    const double u = c[2];
    const double v = atan2(fabs(n[0]), fabs(n[2]));
    const double w = atan2(fabs(n[1]), fabs(n[2]));
    const double x = 1 - v / (M_PI * 2), y = 1 - w / (M_PI * 2);
    int t = u > 100 ? TERRAIN_COLOR_MOUNTAIN : u > 5 ? TERRAIN_COLOR_GRASS : TERRAIN_COLOR_DIRT;
    if (k_biome_force[biome] >= 0) t = k_biome_force[biome];
    if (u > 950) t = TERRAIN_COLOR_SNOW;
    if (u <= -10) t = TERRAIN_COLOR_SAND;
    else if (u > 250 || x * y <= k_biome_slope[biome]) t = TERRAIN_COLOR_MOUNTAIN;
    return t;
}

// terrainHeight: out-of-range indexes read index 0
static float terr_height(const Terrain *g, const int q, const int r) {
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

// calcAOWeight (its second angle uses xSegmentSize for the lower neighbour, as in v7)
static double terr_ao(const Terrain *g, const int q, const int r) {
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

static void terr_push_obj(Terrain *g, const int type, const float x, const float y, const float z, const int biome) {
    if (g->obj_count[type] == g->obj_cap[type]) {
        const int cap = g->obj_cap[type] ? g->obj_cap[type] * 2 : 64;
        TerrainObject *n = realloc(g->objs[type], (size_t) cap * sizeof(TerrainObject));
        if (!n) return;
        g->objs[type] = n;
        g->obj_cap[type] = cap;
    }
    g->objs[type][g->obj_count[type]++] = (TerrainObject) {x, y, z, biome};
}

// evaluateObj(centroid, triangle, biome, name)
static void terr_eval_obj(Terrain *g, const double *q, const double *r, const double *s, const double *t, const int biome, const int type) {
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

void terrain_hex(const char *s, const char *fallback, uint8_t out[3]) {
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

// ---- raw map JSON (map.js extractProperties(map, Terrain.CONFIG_PROPS))

static int tj_truthy(const cJSON *v) {
    if (!v || cJSON_IsNull(v) || cJSON_IsFalse(v)) return 0;
    if (cJSON_IsNumber(v)) return cJSON_GetNumberValue(v) != 0.0 && !isnan(cJSON_GetNumberValue(v));
    if (cJSON_IsString(v)) return v->valuestring[0] != 0;
    return 1;
}

// limitMM(raw, lo, hi) of a number (or numeric string, which JS compares as a number)
static double tj_num(const cJSON *map, const char *key, const double fallback) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(map, key);
    if (cJSON_IsNumber(v)) return cJSON_GetNumberValue(v);
    if (cJSON_IsBool(v)) return cJSON_IsTrue(v) ? 1.0 : 0.0;
    if (cJSON_IsString(v) && v->valuestring[0]) {
        char *end = NULL;
        const double d = strtod(v->valuestring, &end);
        if (end && end != v->valuestring) return d;
    }
    return fallback;
}

static const char *tj_str(const cJSON *map, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(map, key);
    return cJSON_IsString(v) ? v->valuestring : "";
}

// `seed + ""`: a number as JS prints it (shortest round trip; integers without a fraction)
static void tj_seed(const cJSON *map, const char *key, char *out, const size_t size) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(map, key);
    out[0] = 0;
    if (cJSON_IsString(v)) {
        snprintf(out, size, "%s", v->valuestring);
    } else if (cJSON_IsNumber(v)) {
        const double d = cJSON_GetNumberValue(v);
        if (d == trunc(d) && fabs(d) < 1e21) {
            snprintf(out, size, "%.0f", d);
        } else {
            for (int p = 1; p <= 17; p++) {
                snprintf(out, size, "%.*g", p, d);
                if (strtod(out, NULL) == d) break;
            }
        }
    } else if (cJSON_IsBool(v)) {
        snprintf(out, size, "%s", cJSON_IsTrue(v) ? "true" : "false");
    }
}

int terrain_enabled_json(const cJSON *map_json) {
    return map_json && tj_truthy(cJSON_GetObjectItemCaseSensitive(map_json, "terrainSeed")) &&
           tj_truthy(cJSON_GetObjectItemCaseSensitive(map_json, "terrainWidth")) &&
           tj_truthy(cJSON_GetObjectItemCaseSensitive(map_json, "terrainHeight"));
}

void terrain_free(Terrain *g) {
    if (!g) return;
    free(g->heights);
    free(g->positions);
    free(g->vcolors);
    for (int i = 0; i < TERRAIN_OBJ_COUNT; i++) free(g->objs[i]);
    free(g);
}

Terrain *terrain_generate_json(const cJSON *mc) {
    Terrain *g = calloc(1, sizeof(Terrain));
    if (!g) return NULL;
    g->size_mlt = TERRAIN_SIZE_MLT;
    g->size_x = clampd(tj_num(mc, "terrainWidth", 0), 0, 8000);
    g->size_y = clampd(tj_num(mc, "terrainHeight", 0), 0, 8000);
    g->edge_size = TERRAIN_EDGE_SIZE;
    // KrunkNative fix: v7 limitMM(undefined, 0, 5) keeps undefined and the mountain biome turns
    // every height into NaN; a missing terrainMntMlt takes the editor default 1 instead.
    g->mnt_mlt = clampd(tj_num(mc, "terrainMntMlt", 1), 0, 5);
    terrain_hex(tj_str(mc, "terrainDrtCol"), "#8c6835", g->colors[TERRAIN_COLOR_DIRT]);
    terrain_hex(tj_str(mc, "terrainGrsCol"), "#4a6904", g->colors[TERRAIN_COLOR_GRASS]);
    terrain_hex(tj_str(mc, "terrainMntCol"), "#a0a0a0", g->colors[TERRAIN_COLOR_MOUNTAIN]);
    terrain_hex(tj_str(mc, "terrainSndCol"), "#967f51", g->colors[TERRAIN_COLOR_SAND]);
    terrain_hex(tj_str(mc, "terrainSnwCol"), "#e8fdff", g->colors[TERRAIN_COLOR_SNOW]);
    // easing[EASE_LIST[terrainEase] || "easeInOutQuint"]
    const cJSON *ease = cJSON_GetObjectItemCaseSensitive(mc, "terrainEase");
    const double e = cJSON_IsNumber(ease) ? cJSON_GetNumberValue(ease) : cJSON_IsString(ease) ? tj_num(mc, "terrainEase", 12) : 12;
    g->ease = e == trunc(e) && e >= 0 && e < 13 ? (int) e : 12;
    char seed[64], tseed[64];
    tj_seed(mc, "terrainSeed", seed, sizeof(seed));
    tj_seed(mc, "terrainTSeed", tseed, sizeof(tseed));
    terr_perm_init(g->perm[0], seed);
    g->gen_objs = tj_truthy(cJSON_GetObjectItemCaseSensitive(mc, "terrainTSeed"));
    if (g->gen_objs) terr_perm_init(g->perm[1], tseed);

    const int a7 = (int) floor(g->size_x / TERRAIN_SEGMENT), a8 = (int) floor(g->size_y / TERRAIN_SEGMENT);
    g->seg_x = a7;
    g->seg_y = a8;
    if (a7 <= 0 || a8 <= 0) {
        g->seg_x = g->seg_y = 0; // an empty terrain (size < 25)
        return g;
    }
    g->seg_w = g->size_x / a7;
    g->seg_h = g->size_y / a8;
    const size_t verts = (size_t) a7 * (size_t) a8 * 6;
    g->heights = calloc((size_t) (a7 + 1) * (size_t) (a8 + 1), sizeof(float));
    g->positions = calloc(verts * 3, sizeof(float));
    g->vcolors = calloc(verts * 3, 1);
    uint8_t *biome = calloc((size_t) (a7 + 1) * (size_t) (a8 + 1), 1);
    if (!g->heights || !g->positions || !g->vcolors || !biome) {
        free(biome);
        terrain_free(g);
        return NULL;
    }
    for (int ap = 0; ap < a7 + 1; ap++) {
        for (int aq = 0; aq < a8 + 1; aq++) {
            const int ar = ap + aq * (a7 + 1);
            double w[TERRAIN_BIOME_COUNT];
            g->heights[ar] = (float) terr_evaluate(g, terr_vert_pos(ap, g->seg_w, g->size_x), terr_vert_pos(aq, g->seg_h, g->size_y), w);
            int af = 0;
            double ag = 0;
            for (int i = 0; i < TERRAIN_BIOME_COUNT; i++) {
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
            // positions are a Float32Array: the corners are stored (and read back) as floats
            const double aB[3] = {(float) az, (float) aa, terr_height(g, aw, ax)};
            const double aC[3] = {(float) (az + g->seg_w), (float) aa, terr_height(g, aw + 1, ax)};
            const double aD[3] = {(float) (az + g->seg_w), (float) (aa + g->seg_h), terr_height(g, aw + 1, ax + 1)};
            const double aE[3] = {(float) az, (float) (aa + g->seg_h), terr_height(g, aw, ax + 1)};
            const double *tri[6] = {aB, aC, aD, aD, aE, aB};
            for (int k = 0; k < 6; k++)
                for (int i = 0; i < 3; i++) g->positions[(ay + (size_t) k) * 3 + (size_t) i] = (float) tri[k][i];
            // the colour / object rules see the unrounded corners (JS arrays of doubles)
            const double cB[3] = {az, aa, aB[2]}, cC[3] = {az + g->seg_w, aa, aC[2]};
            const double cD[3] = {az + g->seg_w, aa + g->seg_h, aD[2]}, cE[3] = {az, aa + g->seg_h, aE[2]};
            const int bi = biome[aw + ax * (a7 + 1)];
            double aF[3], aG[3];
            for (int i = 0; i < 3; i++) {
                aF[i] = (cB[i] + cC[i] + cD[i]) / 3;
                aG[i] = (cD[i] + cE[i] + cB[i]) / 3;
            }
            // _normal3D(aB, aC, aD), including its sqrt(Z*Z + a0*a0 + a1 + a1) typo (Appendix D)
            const double T = cD[0] - cC[0], U = cD[1] - cC[1], V = cD[2] - cC[2];
            const double W = cB[0] - cC[0], X = cB[1] - cC[1], Y = cB[2] - cC[2];
            const double nz = U * Y - V * X, n0 = V * W - T * Y, n1 = T * X - U * W;
            const double n2 = sqrt(nz * nz + n0 * n0 + n1 + n1);
            const double aH[3] = {nz / n2, n0 / n2, n1 / n2};
            const int col = terr_color(aF, aH, bi);
            const double ak = fmax(aF[2], aG[2]);
            if (g->gen_objs) {
                for (int o = 0; o < TERRAIN_OBJ_COUNT; o++) {
                    // biomes: GRASS, SNOW, HILLS for every MAP_OBJS entry but GRASS (GRASS, HILLS)
                    const int in_biome = bi == TERRAIN_BIOME_GRASS || bi == TERRAIN_BIOME_HILLS || (bi == TERRAIN_BIOME_SNOW && o != TERRAIN_OBJ_GRASS);
                    if (!in_biome) continue;
                    if (k_objs[o].max_height > 0 && k_objs[o].max_height <= ak) continue;
                    const int ok = o == TERRAIN_OBJ_TREE ? col != TERRAIN_COLOR_MOUNTAIN && col != TERRAIN_COLOR_DIRT && col != TERRAIN_COLOR_SAND
                                 : o == TERRAIN_OBJ_ROCK ? 1 : o == TERRAIN_OBJ_GRASS ? 0 : col != TERRAIN_COLOR_MOUNTAIN;
                    if (!ok) continue;
                    terr_eval_obj(g, aF, cB, cC, cD, bi, o);
                    terr_eval_obj(g, aG, cD, cE, cB, bi, o);
                }
            }
            // per-vertex colour: the biome colour darkened by the corner's AO weight
            const double ao[4] = {terr_ao(g, aw, ax), terr_ao(g, aw + 1, ax), terr_ao(g, aw + 1, ax + 1), terr_ao(g, aw, ax + 1)};
            static const int corner[6] = {0, 1, 2, 2, 3, 0};
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

// ---- raycast (Terrain.raycast / _raycastVisit, three.js Ray.intersectTriangle with backface culling)

typedef struct {
    double x, y, z;
} TerrVec;

static TerrVec tv_sub(const TerrVec a, const TerrVec b) { return (TerrVec) {a.x - b.x, a.y - b.y, a.z - b.z}; }
static TerrVec tv_cross(const TerrVec a, const TerrVec b) { return (TerrVec) {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
static double tv_dot(const TerrVec a, const TerrVec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

static int terr_ray_triangle(const TerrVec o, const TerrVec d, const TerrVec a, const TerrVec b, const TerrVec c, TerrVec *out) {
    const TerrVec e1 = tv_sub(b, a), e2 = tv_sub(c, a), n = tv_cross(e1, e2);
    double ddn = tv_dot(d, n), sign;
    if (ddn > 0) return 0; // backfaceCulling
    if (ddn < 0) {
        sign = -1;
        ddn = -ddn;
    } else {
        return 0;
    }
    const TerrVec diff = tv_sub(o, a);
    const double ddqxe2 = sign * tv_dot(d, tv_cross(diff, e2));
    if (ddqxe2 < 0) return 0;
    const double dde1xq = sign * tv_dot(d, tv_cross(e1, diff));
    if (dde1xq < 0) return 0;
    if (ddqxe2 + dde1xq > ddn) return 0;
    const double qdn = -sign * tv_dot(diff, n);
    if (qdn < 0) return 0;
    const double t = qdn / ddn;
    *out = (TerrVec) {o.x + d.x * t, o.y + d.y * t, o.z + d.z * t};
    return 1;
}

static int terr_visit(const Terrain *g, const int cx, const int cy, const TerrVec o, const TerrVec d, const double max, TerrVec *hit, TerrVec *normal) {
    // v7 indexes vertPositions with x + y * xSegmentCount and no per-axis bounds: a column past
    // the edge reads the next row, an index outside the buffer reads undefined (no hit)
    const long cell = (long) cx + (long) cy * g->seg_x;
    if (cell < 0 || cell >= (long) g->seg_x * g->seg_y) return 0;
    double best = INFINITY;
    int found = 0;
    for (int k = 0; k < 2; k++) {
        const float *p = &g->positions[((size_t) cell * 6 + (size_t) k * 3) * 3];
        const TerrVec a = {p[0], p[1], p[2]}, b = {p[3], p[4], p[5]}, c = {p[6], p[7], p[8]};
        TerrVec at;
        if (!terr_ray_triangle(o, d, a, b, c, &at)) continue;
        const TerrVec rel = tv_sub(at, o);
        const double dist = sqrt(tv_dot(rel, rel));
        if (dist < best && dist < max) {
            best = dist;
            found = 1;
            *hit = at;
            if (normal) {
                // Triangle.getNormal: (c - b) x (a - b), normalised
                TerrVec nn = tv_cross(tv_sub(c, b), tv_sub(a, b));
                const double len = sqrt(tv_dot(nn, nn));
                *normal = len > 0 ? (TerrVec) {nn.x / len, nn.y / len, nn.z / len} : (TerrVec) {0, 0, 0};
            }
        }
    }
    return found;
}

int terrain_raycast_local(const Terrain *g, double x, double y, const double z, double dx, double dy, const double dz, double hit[3], double normal[3]) {
    if (!g || !g->positions || g->seg_x <= 0 || g->seg_y <= 0) return 0;
    x /= g->size_mlt;
    y /= g->size_mlt;
    dx /= g->size_mlt;
    dy /= g->size_mlt;
    const double max = sqrt(dx * dx + dy * dy + dz * dz);
    const TerrVec o = {x, y, z}, d = {dx, dy, dz};
    const double x0 = (x + g->size_x / 2) / g->seg_w, y0 = (y + g->size_y / 2) / g->seg_h;
    const double x1 = x0 + dx / g->seg_w, y1 = y0 + dy / g->seg_h;
    const double adx = fabs(x1 - x0), ady = fabs(y1 - y0);
    long cx = (long) floor(x0), cy = (long) floor(y0);
    int sx, sy;
    long n = 1;
    double err;
    if (adx == 0) {
        sx = 0;
        err = INFINITY;
    } else if (x1 > x0) {
        sx = 1;
        n += (long) floor(x1) - cx;
        err = (floor(x0) + 1 - x0) * ady;
    } else {
        sx = -1;
        n += cx - (long) floor(x1);
        err = (x0 - floor(x0)) * ady;
    }
    if (ady == 0) {
        sy = 0;
        err -= INFINITY;
    } else if (y1 > y0) {
        sy = 1;
        n += (long) floor(y1) - cy;
        err -= (floor(y0) + 1 - y0) * adx;
    } else {
        sy = -1;
        n += cy - (long) floor(y1);
        err -= (y0 - floor(y0)) * adx;
    }
    for (; n > 0; n--) {
        TerrVec at, nn;
        if (cx >= -0x7fffffffL && cx <= 0x7fffffffL && cy >= -0x7fffffffL && cy <= 0x7fffffffL &&
            terr_visit(g, (int) cx, (int) cy, o, d, max, &at, normal ? &nn : NULL)) {
            hit[0] = at.x * g->size_mlt;
            hit[1] = at.y * g->size_mlt;
            hit[2] = at.z;
            if (normal) {
                normal[0] = nn.x;
                normal[1] = nn.y;
                normal[2] = nn.z;
            }
            return 1;
        }
        // NaN err (Infinity - Infinity: a vertical ray) steps x, which is 0: the loop runs once
        if (err > 0) {
            cy += sy;
            err -= adx;
        } else {
            cx += sx;
            err += ady;
        }
    }
    return 0;
}

// ---- the map's terrain (map.terrain), cached on the Map

const Terrain *map_terrain(const Map *map) {
    if (!map || !map->terrain.enabled || !map->raw_data) return NULL;
    Map *m = (Map *) map; // a lazily filled cache of immutable map data
    if (!m->world_terrain && !m->world_terrain_failed) {
        m->world_terrain = terrain_generate_json(map->raw_data);
        if (!m->world_terrain) m->world_terrain_failed = 1;
    }
    return m->world_terrain;
}

void map_terrain_free(Map *map) {
    if (!map) return;
    terrain_free(map->world_terrain);
    map->world_terrain = NULL;
    map->world_terrain_failed = 0;
}
