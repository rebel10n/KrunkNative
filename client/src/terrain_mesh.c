// Krunker v7.1.4 terrain rendering (client/include/terrain_mesh.h): the procedural terrain of
// shared/src/terrain.c (map_terrain, libs/Terrain.js) and its terrainTSeed objects
// (MapManager.addTerrObj -> addTREE / addSTUMP / addBUSH / addGRASS / addLOOT / addROCK).
//
// Terrain.js: the mesh is StrippedLambertMaterial(color #8c8c8c, vertexColors) with the vertex
// colours of the generator, rotated -PI/2 about x and scaled 5.7, receiveShadow.
// Parity notes (documented differences):
// - The terrain is lit with per-face normals (basic.frag dFdx/dFdy); three.js merged the vertices
//   and lit smooth vertex normals.
// - The objects are merged into one Lambert mesh (vertex colours) plus one unlit mesh for the
//   grass / flower planes. Their random parts (grass placement, flower rotation, rung jitter)
//   use KN's random generator, not v7's Math.random sequence.
// - geos.js generateSphere / generateCone with vNoise set every vertex to (n, n, n), n random:
//   the canopies, rocks and bushes collapse into degenerate triangles and v7 draws nothing of
//   them (a v7 bug, fixed here: each vertex is pushed out radially by 1 + n instead).
#include <gl.h>
#include <terrain_mesh.h>
#include <terrain.h>
#include <math.h>
#include <pcg_basic.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    vertex *v;
    float *c;
    unsigned int *i;
    size_t nv, ni, cap_v, cap_i;
} TerrainBuf;

typedef struct {
    Mesh *mesh;
    unsigned int vbo, cbo;
} TerrainPart;

static TerrainPart g_terrain, g_terrain_objects, g_terrain_grass;
static Scene *g_terrain_scene;

static int tbuf_reserve(TerrainBuf *b, const size_t nv, const size_t ni) {
    if (b->nv + nv > b->cap_v) {
        size_t cap = b->cap_v ? b->cap_v : 1024;
        while (cap < b->nv + nv) cap *= 2;
        vertex *v = realloc(b->v, cap * sizeof(vertex));
        float *c = realloc(b->c, cap * 3 * sizeof(float));
        if (v) b->v = v;
        if (c) b->c = c;
        if (!v || !c) return 0;
        b->cap_v = cap;
    }
    if (b->ni + ni > b->cap_i) {
        size_t cap = b->cap_i ? b->cap_i : 2048;
        while (cap < b->ni + ni) cap *= 2;
        unsigned int *i = realloc(b->i, cap * sizeof(unsigned int));
        if (!i) return 0;
        b->i = i;
        b->cap_i = cap;
    }
    return 1;
}

static void tbuf_vertex(TerrainBuf *b, const vec3 p, const vec3 c) {
    b->v[b->nv] = (vertex) {p, {0.0f, 0.0f}};
    b->c[b->nv * 3] = c.x;
    b->c[b->nv * 3 + 1] = c.y;
    b->c[b->nv * 3 + 2] = c.z;
    b->nv++;
}

static void tbuf_free(TerrainBuf *b) {
    free(b->v);
    free(b->c);
    free(b->i);
    memset(b, 0, sizeof(*b));
}

static float terrain_random(void) {
    return (float) pcg32_random() / 4294967296.0f;
}

// geos.js getColor(colr, mult): colour * mult with the HSL saturation * 1.5
static vec3 terrain_color(const char *hex, const float mult) {
    vec4 c = {1.0f, 1.0f, 1.0f, 1.0f};
    parse_hex_color(hex, &c);
    float r = c.x * mult, g = c.y * mult, bl = c.z * mult;
    const float max = fmaxf(r, fmaxf(g, bl)), min = fminf(r, fminf(g, bl));
    const float l = (max + min) * 0.5f;
    float h = 0.0f, s = 0.0f;
    if (max != min) {
        const float d = max - min;
        s = l <= 0.5f ? d / (max + min) : d / (2.0f - max - min);
        if (max == r) h = (g - bl) / d + (g < bl ? 6.0f : 0.0f);
        else if (max == g) h = (bl - r) / d + 2.0f;
        else h = (r - g) / d + 4.0f;
        h /= 6.0f;
    }
    s = fminf(1.0f, s * 1.5f);
    if (s <= 0.0f) return (vec3) {l, l, l};
    const float p = l <= 0.5f ? l * (1.0f + s) : l + s - l * s, q = 2.0f * l - p;
    float out[3];
    for (int k = 0; k < 3; k++) {
        float t = h + (1.0f / 3.0f) * (float) (1 - k);
        if (t < 0.0f) t += 1.0f;
        if (t > 1.0f) t -= 1.0f;
        out[k] = t < 1.0f / 6.0f ? q + (p - q) * 6.0f * t : t < 0.5f ? p : t < 2.0f / 3.0f ? q + (p - q) * 6.0f * (2.0f / 3.0f - t) : q;
    }
    return (vec3) {out[0], out[1], out[2]};
}

// rotation.set(a, b, c) (XYZ) then scale, then position
static vec3 terrain_xf(const vec3 p, const vec3 rot, const vec3 scale, const vec3 pos) {
    vec3 v = {p.x * scale.x, p.y * scale.y, p.z * scale.z};
    // R = Rx(a) Ry(b) Rz(c) applied to v
    float x = v.x * cosf(rot.z) - v.y * sinf(rot.z), y = v.x * sinf(rot.z) + v.y * cosf(rot.z), z = v.z;
    float x2 = x * cosf(rot.y) + z * sinf(rot.y), z2 = -x * sinf(rot.y) + z * cosf(rot.y);
    float y3 = y * cosf(rot.x) - z2 * sinf(rot.x), z3 = y * sinf(rot.x) + z2 * cosf(rot.x);
    return (vec3) {x2 + pos.x, y3 + pos.y, z3 + pos.z};
}

// render.js addCube(x, y, z, w, h, d, faces, {colr, amb 0.5}): geos.js generateCube (bottom
// vertices getColor(colr, 0.7)), rotation (yR, xR, zR), scale (w, h, d), base at y
static void tbuf_cube(TerrainBuf *b, const float x, const float y, const float z, const float w, const float h, const float d,
                      const int faces, const vec3 rot, const char *colr) {
    static const float quads[6][4][3] = {
        {{0.5f, 0, 0.5f}, {0.5f, 0, -0.5f}, {0.5f, 1, -0.5f}, {0.5f, 1, 0.5f}},     // +x
        {{-0.5f, 0, -0.5f}, {-0.5f, 0, 0.5f}, {-0.5f, 1, 0.5f}, {-0.5f, 1, -0.5f}}, // -x
        {{-0.5f, 1, 0.5f}, {0.5f, 1, 0.5f}, {0.5f, 1, -0.5f}, {-0.5f, 1, -0.5f}},   // top
        {{-0.5f, 0, -0.5f}, {0.5f, 0, -0.5f}, {0.5f, 0, 0.5f}, {-0.5f, 0, 0.5f}},   // bottom
        {{-0.5f, 0, 0.5f}, {0.5f, 0, 0.5f}, {0.5f, 1, 0.5f}, {-0.5f, 1, 0.5f}},     // +z
        {{0.5f, 0, -0.5f}, {-0.5f, 0, -0.5f}, {-0.5f, 1, -0.5f}, {0.5f, 1, -0.5f}}, // -z
    };
    const vec3 top = terrain_color(colr, 1.0f), bottom = terrain_color(colr, 0.7f);
    for (int f = 0; f < 6; f++) {
        if (!(faces & (1 << f)) || !tbuf_reserve(b, 4, 6)) continue;
        const unsigned int base = (unsigned int) b->nv;
        for (int k = 0; k < 4; k++) {
            const vec3 p = terrain_xf((vec3) {quads[f][k][0], quads[f][k][1], quads[f][k][2]}, rot, (vec3) {w, h, d}, (vec3) {x, y, z});
            const int up = quads[f][k][1] > 0.5f && f != 3;
            tbuf_vertex(b, p, f == 2 ? top : f == 3 ? bottom : up ? top : bottom);
        }
        const unsigned int tri[6] = {base, base + 1, base + 2, base, base + 2, base + 3};
        memcpy(b->i + b->ni, tri, sizeof(tri));
        b->ni += 6;
    }
}

// CylinderGeometry(rTop, rBot, 1, sides, 1, open) around (x, y .. y + h, z) with radius r
static void tbuf_cylinder(TerrainBuf *b, const float x, const float y, const float z, const float r_top, const float r_bot, const float h,
                          const int sides, const int open, const vec3 rot, const char *colr, const float noise) {
    const vec3 color = terrain_color(colr, 1.0f);
    if (!tbuf_reserve(b, (size_t) sides * 4 + 2, (size_t) sides * 12)) return;
    const unsigned int base = (unsigned int) b->nv;
    for (int k = 0; k < sides; k++) {
        const float th = (float) k / (float) sides * 2.0f * (float) M_PI;
        const float n = noise ? 1.0f + terrain_random() * noise : 1.0f;
        tbuf_vertex(b, terrain_xf((vec3) {sinf(th) * r_top * n, 1, cosf(th) * r_top * n}, rot, (vec3) {1, h, 1}, (vec3) {x, y, z}), color);
        tbuf_vertex(b, terrain_xf((vec3) {sinf(th) * r_bot * n, 0, cosf(th) * r_bot * n}, rot, (vec3) {1, h, 1}, (vec3) {x, y, z}), color);
    }
    tbuf_vertex(b, terrain_xf((vec3) {0, 1, 0}, rot, (vec3) {1, h, 1}, (vec3) {x, y, z}), color);
    tbuf_vertex(b, terrain_xf((vec3) {0, 0, 0}, rot, (vec3) {1, h, 1}, (vec3) {x, y, z}), color);
    const unsigned int c_top = base + (unsigned int) sides * 2, c_bot = c_top + 1;
    for (int k = 0; k < sides; k++) {
        const unsigned int t0 = base + (unsigned int) k * 2, b0 = t0 + 1;
        const unsigned int t1 = base + (unsigned int) ((k + 1) % sides) * 2, b1 = t1 + 1;
        const unsigned int side[6] = {t0, b0, t1, b0, b1, t1};
        memcpy(b->i + b->ni, side, sizeof(side));
        b->ni += 6;
        if (!open) {
            const unsigned int caps[6] = {c_top, t0, t1, c_bot, b1, b0};
            memcpy(b->i + b->ni, caps, sizeof(caps));
            b->ni += 6;
        }
    }
}

// render.js addSphere(x, y, z, w, h, d, {vNoise, xR}): SphereGeometry(1, 12, 8) scaled (w, h, d) / 2
// at y + h / 2, rotation (yR, xR, zR)
static void tbuf_sphere(TerrainBuf *b, const float x, const float y, const float z, const float w, const float h, const float d,
                        const float noise, const float x_rot, const char *colr) {
    const int rings = 8, segments = 12;
    const vec3 color = terrain_color(colr, 1.0f);
    if (!tbuf_reserve(b, (size_t) (rings + 1) * (segments + 1), (size_t) rings * segments * 6)) return;
    const unsigned int base = (unsigned int) b->nv;
    for (int iy = 0; iy <= rings; iy++) {
        const float phi = (float) iy / (float) rings * (float) M_PI;
        for (int ix = 0; ix <= segments; ix++) {
            const float th = (float) ix / (float) segments * 2.0f * (float) M_PI;
            const float n = 1.0f + terrain_random() * noise;
            const vec3 p = {-cosf(th) * sinf(phi) * n, cosf(phi) * n, sinf(th) * sinf(phi) * n};
            tbuf_vertex(b, terrain_xf(p, (vec3) {0.0f, x_rot, 0.0f}, (vec3) {w / 2.0f, h / 2.0f, d / 2.0f}, (vec3) {x, y + h / 2.0f, z}), color);
        }
    }
    for (int iy = 0; iy < rings; iy++) {
        for (int ix = 0; ix < segments; ix++) {
            const unsigned int a = base + (unsigned int) (iy * (segments + 1) + ix), c = a + (unsigned int) segments + 1;
            const unsigned int tri[6] = {a, c, a + 1, a + 1, c, c + 1};
            memcpy(b->i + b->ni, tri, sizeof(tri));
            b->ni += 6;
        }
    }
}

// render.js addPlane(x, y, z, w, l, ..., yaw, pitch, roll): a vertical / tilted quad of w x l
static void tbuf_plane(TerrainBuf *b, const float x, const float y, const float z, const float w, const float l,
                       const float yaw, const float pitch, const float roll, const char *colr) {
    if (!tbuf_reserve(b, 4, 6)) return;
    const vec3 color = terrain_color(colr, 1.0f);
    static const float corners[4][2] = {{-0.5f, -0.5f}, {0.5f, -0.5f}, {0.5f, 0.5f}, {-0.5f, 0.5f}};
    const unsigned int base = (unsigned int) b->nv;
    for (int k = 0; k < 4; k++) {
        // rotateY(yaw) rotateX(pitch - PI/2) rotateZ(roll), scale (w * 2, l * 2): YXZ
        vec3 p = {corners[k][0] * w * 2.0f, corners[k][1] * l * 2.0f, 0.0f};
        const float cr = cosf(roll), sr = sinf(roll);
        p = (vec3) {p.x * cr - p.y * sr, p.x * sr + p.y * cr, p.z};
        const float px = pitch - (float) M_PI / 2.0f, cx = cosf(px), sx = sinf(px);
        p = (vec3) {p.x, p.y * cx - p.z * sx, p.y * sx + p.z * cx};
        const float cy = cosf(yaw), sy = sinf(yaw);
        p = (vec3) {p.x * cy + p.z * sy, p.y, -p.x * sy + p.z * cy};
        tbuf_vertex(b, (vec3) {p.x + x, p.y + y, p.z + z}, color);
    }
    const unsigned int tri[6] = {base, base + 1, base + 2, base, base + 2, base + 3};
    memcpy(b->i + b->ni, tri, sizeof(tri));
    b->ni += 6;
}

static const char *const k_tree_colors[4] = {"#444532", "#484a2f", "#4b4d2e", "#55573b"}; // MapManager vA700

// Terrain.js MAP_OBJS biomeCols (BIOME_INDEXES: the TerrainObject biome)
static const char *terrain_object_color(const int type, const int biome) {
    static const char *const tree_snow[3] = {"#484d35", "#4d4b35", "#5c5a3c"};
    if (type == TERRAIN_OBJ_TREE && biome == TERRAIN_BIOME_SNOW) return tree_snow[pcg32_boundedrand(3)];
    if (type == TERRAIN_OBJ_BUSH && biome == TERRAIN_BIOME_SNOW) return "#484d35";
    if (type == TERRAIN_OBJ_GRASS && (biome == TERRAIN_BIOME_GRASS || biome == TERRAIN_BIOME_HILLS)) return "#4d5928";
    return NULL;
}

static int terrain_round_mod(const float a, const float b, const int m) {
    return (int) lroundf(fabsf(fmodf(a + b, (float) m))); // Math.round(Math.abs((x + z) % m))
}

static void terrain_add_object(TerrainBuf *b, TerrainBuf *grass, const int type, const TerrainObject *o) {
    const char *col = terrain_object_color(type, o->biome);
    const int all = 0x3f, no_bottom = 0x3f & ~(1 << 3);
    switch (type) {
        case TERRAIN_OBJ_TREE: {
            // MapManager vA701: Tiny Oak, Square Oak, Pine, Tall Italian, Honey Oak by rar
            const int q = terrain_round_mod(o->x, o->z, 10);
            const int kind = q == 10 ? 0 : (q == 4 || q == 8 || q == 9) ? 1 : (q >= 5 && q <= 7) ? 2 : (q >= 1 && q <= 3) ? 3 : q == 0 ? 4 : -1;
            if (kind < 0) break;
            static const float heights[5][3] = {{30}, {65, 50}, {90, 120, 140}, {37}, {100, 120}};
            static const int height_count[5] = {1, 2, 3, 1, 2};
            static const float scales[5][2] = {{1.5f, 1.6f}, {4, 5}, {3, 3.2f}, {3, 3.2f}, {4, 5}};
            const float height = heights[kind][q % height_count[kind]] - (float) q * 2.0f;
            const float scale = scales[kind][q % 2];
            // trunk: addCylinder(x, y, z, scale * 1.5, height + 20, scale * 1.5, {sides 4, open, #524435})
            const float r = scale * 1.5f / 2.0f;
            tbuf_cylinder(b, o->x, o->y, o->z, r, r, height + 20.0f, 4, 1, (vec3) {0}, "#524435", 0.0f);
            const char *leaf = col ? col : k_tree_colors[q % 4];
            switch (kind) {
                case 0: tbuf_sphere(b, o->x, o->y + height - 1, o->z, 25, 34, 25, 0.3f, (float) q, leaf); break;
                case 1: tbuf_sphere(b, o->x, o->y + height - 1, o->z, 110, 120, 110, 0.36f, (float) q, leaf); break;
                case 2: {
                    float y = o->y + 40.0f, radius = 30.0f;
                    const int count = height >= 130.0f ? 5 : 4;
                    for (int k = 0; k < count; k++) {
                        // addCone(x, y, z, r * 2, 40, r * 2, {closC, sides 6}): ConeGeometry radius 1 scaled (r, 40, r)
                        tbuf_cylinder(b, o->x, y, o->z, 0.0f, radius, 40.0f, 6, 0, (vec3) {0, (float) q, 0}, leaf, 0.12f);
                        radius -= 5.0f;
                        y += height / (float) count;
                    }
                    break;
                }
                case 3: tbuf_sphere(b, o->x, o->y + height - 6, o->z, 39, 80, 39, 0.3f, (float) q, leaf); break;
                default:
                    tbuf_sphere(b, o->x, o->y + height - 1, o->z, 70, 80, 70, 0.35f, (float) q, col ? col : "#616037");
                    tbuf_cube(b, o->x + 10, o->y + height / 1.5f + 5, o->z, 22, 3, 2, all, (vec3) {0, 0, 0.4f}, "#665547");
                    tbuf_sphere(b, o->x + 20, o->y + height / 1.5f, o->z, 30, 28, 30, 0.25f, 0.0f, col ? col : "#616037");
                    break;
            }
            break;
        }
        case TERRAIN_OBJ_STUMP:
            tbuf_cube(b, o->x, o->y, o->z, 4, 80, 4, no_bottom, (vec3) {0}, "#665547");
            tbuf_cube(b, o->x, o->y + 30, o->z + 10, 2, 3, 22, all, (vec3) {-0.5f, 0, 0}, "#665547");
            tbuf_cube(b, o->x + 10, o->y + 50, o->z, 22, 3, 2, all, (vec3) {0, 0, 0.4f}, "#665547");
            break;
        case TERRAIN_OBJ_LOOT: {
            const int q = terrain_round_mod(o->x, o->z, 3);
            tbuf_cube(b, o->x + 6, o->y, o->z + 15, 18, 1, 3, no_bottom, (vec3) {0, 0.86f, 0}, "#665547");
            // manager.addCylinder(x, y, z, 7, 7, 9): radius min(7, 9) / 2, height 7
            if (q == 0 || q == 1) tbuf_cylinder(b, o->x, o->y, o->z, 3.5f, 3.5f, 7, 12, 0, (vec3) {0}, "#9e2b2b", 0.0f);
            if (q == 2 || q == 1) tbuf_cylinder(b, o->x - 7, o->y, o->z + 12, 3.5f, 3.5f, 7, 12, 0, (vec3) {0}, "#2b769e", 0.0f);
            break;
        }
        case TERRAIN_OBJ_ROCK: {
            static const struct { float scl, hei, nois; const char *col; } rocks[4] = {
                {25, 20, 0.2f, "#707070"}, {50, 30, 0.3f, "#545454"}, {13, 10, 0.15f, "#707070"}, {10, 10, 0.1f, "#545454"},
            };
            const int k = terrain_round_mod(o->x, o->z, 10) % 4;
            tbuf_sphere(b, o->x, o->y - 7, o->z, rocks[k].scl, rocks[k].hei, rocks[k].scl, rocks[k].nois, 0.0f, rocks[k].col);
            break;
        }
        case TERRAIN_OBJ_BUSH: {
            const int q = terrain_round_mod(o->x, o->z, 10);
            const float s = o->biome == TERRAIN_BIOME_SNOW ? 0.5f : 1.0f;
            tbuf_sphere(b, o->x, o->y - s * 6, o->z, s * 29, s * 21, s * 28, 0.3f, (float) q, col ? col : "#646639");
            break;
        }
        case TERRAIN_OBJ_GRASS:
            // addGRASS: 4 random blades (Grass 1.2 x 5..6, Flower 0.3 x 2..2.3 with a red top),
            // MeshBasicMaterial, DoubleSide
            for (int k = 0; k < 4; k++) {
                const int flower = (int) pcg32_boundedrand(2);
                const float gx = o->x + (float) ((int) pcg32_boundedrand(31) - 15), gz = o->z + (float) ((int) pcg32_boundedrand(31) - 15);
                const float h = flower ? (pcg32_boundedrand(2) ? 2.3f : 2.0f) : (pcg32_boundedrand(2) ? 6.0f : 5.0f);
                const float yaw = (terrain_random() * 2.0f - 1.0f) * (float) M_PI;
                tbuf_plane(grass, gx, o->y, gz, flower ? 0.3f : 1.2f, h, yaw, (float) M_PI / 2.0f, 0.0f, col ? col : "#4d5928");
                if (flower) tbuf_plane(grass, gx, o->y + h + 0.05f, gz, 1.2f, 1.2f, (terrain_random() * 2.0f - 1.0f) * (float) M_PI, 0.5f, 0.0f, "#993d36");
            }
            break;
        default:
            break;
    }
}

static Mesh *terrain_upload(TerrainPart *part, const TerrainBuf *b) {
    if (!b->nv || !b->ni) return NULL;
    Geometry *g = calloc(1, sizeof(Geometry));
    if (!g) return NULL;
    glGenVertexArrays(1, &g->vao);
    glGenBuffers(1, &g->ebo);
    glGenBuffers(1, &part->vbo);
    glGenBuffers(1, &part->cbo);
    glBindVertexArray(g->vao);
    glBindBuffer(GL_ARRAY_BUFFER, part->vbo);
    glBufferData(GL_ARRAY_BUFFER, (long) (b->nv * sizeof(vertex)), b->v, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(vertex), NULL);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(vertex), (void *) (3 * sizeof(float)));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glBindBuffer(GL_ARRAY_BUFFER, part->cbo);
    glBufferData(GL_ARRAY_BUFFER, (long) (b->nv * 3 * sizeof(float)), b->c, GL_STATIC_DRAW);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), NULL);
    glEnableVertexAttribArray(2);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g->ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (long) (b->ni * sizeof(unsigned int)), b->i, GL_STATIC_DRAW);
    glBindVertexArray(0);
    geometry_set_bounds(g, b->v, b->nv);
    g->index_count = (int) b->ni;
    BasicMaterial *m = basic_material_init();
    if (!m) {
        glDeleteVertexArrays(1, &g->vao);
        free(g);
        return NULL;
    }
    m->use_vertex_color = 1;
    m->double_sided = 0;
    part->mesh = mesh_init(g, (Material *) m);
    return part->mesh;
}

static void terrain_part_drop(TerrainPart *part) {
    if (!part->mesh) return;
    if (g_terrain_scene) scene_remove_mesh(g_terrain_scene, part->mesh);
    Geometry *g = part->mesh->geometry;
    glDeleteVertexArrays(1, &g->vao);
    glDeleteBuffers(1, &g->ebo);
    glDeleteBuffers(1, &part->vbo);
    glDeleteBuffers(1, &part->cbo);
    free(g);
    mesh_fini(part->mesh);
    memset(part, 0, sizeof(*part));
}

void terrain_mesh_load(Scene *scene, const Map *map) {
    terrain_mesh_unload();
    const Terrain *terrain = map_terrain(map);
    if (!terrain || !scene) return;
    g_terrain_scene = scene;

    TerrainBuf b = {0};
    const size_t verts = (size_t) terrain->seg_x * (size_t) terrain->seg_y * 6;
    if (verts && tbuf_reserve(&b, verts, verts)) {
        // baseMesh.scale (5.7, 5.7, 1) inside rotation.x = -PI/2: (x, y, h) -> (5.7x, h, -5.7y)
        for (size_t k = 0; k < verts; k++) {
            const float *p = &terrain->positions[k * 3];
            const uint8_t *c = &terrain->vcolors[k * 3];
            tbuf_vertex(&b, (vec3) {p[0] * (float) TERRAIN_SIZE_MLT, p[2], -p[1] * (float) TERRAIN_SIZE_MLT},
                        (vec3) {c[0] / 255.0f, c[1] / 255.0f, c[2] / 255.0f});
            b.i[b.ni++] = (unsigned int) k;
        }
        Mesh *mesh = terrain_upload(&g_terrain, &b);
        if (mesh) {
            BasicMaterial *m = (BasicMaterial *) mesh->material;
            m->color = (vec4) {0x8c / 255.0f, 0x8c / 255.0f, 0x8c / 255.0f, 1.0f}; // StrippedLambertMaterial color 9211020
            scene_add_mesh(scene, mesh);
        }
    }
    tbuf_free(&b);

    if (terrain->gen_objs) {
        TerrainBuf grass = {0};
        for (int type = 0; type < TERRAIN_OBJ_COUNT; type++) {
            for (int k = 0; k < terrain->obj_count[type]; k++) terrain_add_object(&b, &grass, type, &terrain->objs[type][k]);
        }
        Mesh *mesh = terrain_upload(&g_terrain_objects, &b);
        if (mesh) scene_add_mesh(scene, mesh);
        mesh = terrain_upload(&g_terrain_grass, &grass);
        if (mesh) {
            BasicMaterial *m = (BasicMaterial *) mesh->material;
            m->unlit = 1;         // MeshBasicMaterial
            m->double_sided = 1;  // DoubleSide
            scene_add_mesh(scene, mesh);
        }
        tbuf_free(&b);
        tbuf_free(&grass);
    }
}

void terrain_mesh_unload(void) {
    terrain_part_drop(&g_terrain);
    terrain_part_drop(&g_terrain_objects);
    terrain_part_drop(&g_terrain_grass);
    g_terrain_scene = NULL;
}
