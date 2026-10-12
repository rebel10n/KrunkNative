// Map editor: what the Terrain and Zone right panels (editor_env.c) generate in the 3D view: Editor.regenTerrain (libs/Terrain.js, the
// simplex-noise 2.4.0 heightmap with biome colours and the terrainTSeed clutter of
// data/prefabs.js addTerrObj) and Editor.regenZone (libs/Zone.js, the battle-royale
// cylinder). Both live in g_ed.proj->scene as ordinary BasicMaterial meshes owned here.
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

// ---- generator: shared/src/terrain.c (map-logic WP-B moved it to shared/ so the server, the
// sandbox and the client's prediction raycast the same terrain); the editor draws its output.
#include <terrain.h>

typedef Terrain TerrGen;
typedef TerrainObject TerrObj;
enum { B_SNOW = TERRAIN_BIOME_SNOW };
enum { O_TREE = TERRAIN_OBJ_TREE, O_STUMP = TERRAIN_OBJ_STUMP, O_LOOT = TERRAIN_OBJ_LOOT, O_ROCK = TERRAIN_OBJ_ROCK,
       O_BUSH = TERRAIN_OBJ_BUSH, O_COUNT = TERRAIN_OBJ_COUNT };
#define terr_generate terrain_generate_json
#define terr_gen_free terrain_free
#define terr_hex terrain_hex

static double clampd(const double v, const double lo, const double hi) { return v < lo ? lo : v > hi ? hi : v; }

// Terrain.EASE_LIST (libs/easing.js key order), the terrainEase options

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

typedef EdTerrPart TerrPart; // per project: g_ed.proj->terrain / clutter / zone

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
        if (g_ed.proj->scene) scene_remove_mesh(g_ed.proj->scene, p->mesh);
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
    if (p->mesh && g_ed.proj->scene) scene_add_mesh(g_ed.proj->scene, p->mesh);
}

// Editor.regenTerrain: remove the old terrain; mapConfig.terrainSeed set -> a new one
void ed_terrain_regen(void) {
    part_drop(&g_ed.proj->terrain);
    part_drop(&g_ed.proj->clutter);
    const cJSON *mc = g_ed.proj->map_config;
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
        part_set(&g_ed.proj->terrain, &b, m);
    }
    tb_free(&b);
    if (g->gen_objs) {
        for (int o = 0; o < O_COUNT; o++)
            for (int k = 0; k < g->obj_count[o]; k++) clutter_add(&b, o, &g->objs[o][k]);
        BasicMaterial *m = basic_material_init(); // new THREE.Mesh(geometry): white MeshBasicMaterial
        if (m) m->unlit = 1;
        part_set(&g_ed.proj->clutter, &b, m);
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
    part_drop(&g_ed.proj->zone);
    const cJSON *mc = g_ed.proj->map_config;
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
        part_set(&g_ed.proj->zone, &b, m);
    }
    tb_free(&b);
}

void ed_terrain_fini(void) {
    part_drop(&g_ed.proj->terrain);
    part_drop(&g_ed.proj->clutter);
    part_drop(&g_ed.proj->zone);
}
