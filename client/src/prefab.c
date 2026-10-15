// Krunker v7.1.4 map-object meshes: data/map.js generate() -> libs/MapManager.js add* builders
// -> libs/render.js addCube / addFixedPlane / addSprite / loadMesh / getMat / loadTexture and the
// libs/geos.js generators. prefab_init() builds one Mesh per map object from the parsed Object
// (shared/src/map.c, the v7 per-object keys) and its raw record; the prefab table (names, model
// sources and scales, variants, flags) is shared/include/prefabs.h (generated from v7 prefabs.js).
//
// Built elsewhere: lights, noise planes, spin, render distance, leaderboard canvases (map_post.c),
// AO planes (ambient.c), terrain / BR zone (terrain_mesh.c / zone_mesh.c), ocean (ocean.c), sky
// dome (sky.c), flags / weapon pickups / showcases / bomb sites (mode_world.c and friends),
// particles (vfx.c), AIs (ai_render.c). Not drawn: PRIZE_WHEEL (v7 paints the store wheel into
// its canvas every frame, store data KN does not have: documented placeholder, no mesh).
#include <gl.h>
#include <client.h>
#include <render_settings.h>
#include <prefabs.h>
#include <canvas2d.h>
#include <billboard_ad.h>
#include <user_asset.h>
#include "viewer/viewer_store.h"
#include <weapons.h>
#include <nettspend.h> // the Drank's cup / the Money Spread's cash stack as pickups
#include <world.h>
#include <map_post.h> // prefab_build_vertices / prefab_build_batch_mesh (the build batch)
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pcg_basic.h>
#include <ruleset.h>

// geos.js generateCube (v7): bottom vertices getColor(colr, (darken || 1) * 0.7) when the
// object has "amb" (v3.1.1 used config.ambientVal 0.5); rungs and ladders take the same.
#define PREFAB_AMBIENT_MULT 0.7f


static int prefab_face_mask(const cJSON *raw_faces) {
    if (!cJSON_IsString(raw_faces)) return 0x3f;

    const long bits = strtol(cJSON_GetStringValue(raw_faces), NULL, 16);
    int mask = 0;

    for (int i = 0; i < 6; i++) {
        if (bits & (1 << (5 - i))) mask |= 1 << i;
    }

    return mask;
}

static void prefab_apply_lighting_controls(BasicMaterial *material, const Object *object) {
    material->ambient_enabled = object->ambient_enabled;
    material->fog_enabled = object->fog_enabled;
    // the lighting setting (0 Basic / 1 Lambert / 2 Phong) is applied per draw in basic.c
}

static float prefab_hue_to_rgb(const float p, const float q, float t) {
    if (t < 0.0f) t += 1.0f;
    if (t > 1.0f) t -= 1.0f;
    if (t < 1.0f / 6.0f) return p + (q - p) * 6.0f * t;
    if (t < 0.5f) return q;
    if (t < 2.0f / 3.0f) return p + (q - p) * 6.0f * (2.0f / 3.0f - t);
    return p;
}

// Krunker 31.js getColor(colr, mult): THREE.Color(colr).multiplyScalar(mult), then the
// HSL saturation is raised by 1.5 (clamped). Used for the vertex colours of generated map
// geometry (cubes, planes, ramps); models use the raw colour as their material colour.
static vec4 prefab_krunker_color(const vec4 color, const float mult) {
    const float r = color.x * mult;
    const float g = color.y * mult;
    const float b = color.z * mult;
    const float max = fmaxf(r, fmaxf(g, b));
    const float min = fminf(r, fminf(g, b));
    const float l = (min + max) * 0.5f;
    float h = 0.0f;
    float s = 0.0f;

    if (max != min) {
        const float delta = max - min;
        s = l <= 0.5f ? delta / (max + min) : delta / (2.0f - max - min);

        if (max == r) h = (g - b) / delta + (g < b ? 6.0f : 0.0f);
        else if (max == g) h = (b - r) / delta + 2.0f;
        else h = (r - g) / delta + 4.0f;

        h /= 6.0f;
    }

    s = fminf(1.0f, s * 1.5f);

    vec4 out = {l, l, l, color.w};

    if (s > 0.0f) {
        const float p = l <= 0.5f ? l * (1.0f + s) : l + s - l * s;
        const float q = 2.0f * l - p;

        out.x = prefab_hue_to_rgb(q, p, h + 1.0f / 3.0f);
        out.y = prefab_hue_to_rgb(q, p, h);
        out.z = prefab_hue_to_rgb(q, p, h - 1.0f / 3.0f);
    }

    return out;
}

static float prefab_json_float(const cJSON *json, const float fallback) {
    if (cJSON_IsNumber(json)) return (float) cJSON_GetNumberValue(json);
    if (cJSON_IsString(json)) return (float) atof(cJSON_GetStringValue(json));
    return fallback;
}

static int prefab_json_present(const cJSON *json) {
    return json && !cJSON_IsNull(json);
}

static Geometry *prefab_upload_geo(const vertex *vertices, const int vertex_count, const unsigned int *indices, const int index_count) {
    Geometry *geometry = calloc(1, sizeof(Geometry));
    if (!geometry) return NULL;

    unsigned int vbo;
    glGenVertexArrays(1, &geometry->vao);
    glGenBuffers(1, &geometry->ebo);
    glGenBuffers(1, &vbo);

    glBindVertexArray(geometry->vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, (long) vertex_count * (long) sizeof(vertex), vertices, GL_STATIC_DRAW);
    geometry_set_bounds(geometry, vertices, (size_t) vertex_count);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, geometry->ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (long) index_count * (long) sizeof(unsigned int), indices, GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), NULL);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *) (3 * sizeof(float)));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);

    geometry->index_count = index_count;
    return geometry;
}

// ---- ladders: 94.js addLadder -> 55.js addCube (two rails) + addPlane (one per rung),
// built as one geometry in the ladder's frame (origin = object->position, the rails' foot:
// x + ladderScale * cos(d), y, z + ladderScale * sin(d)). The map loader passes texture
// scale 0.02 (1 with "tsr") and 31.js w() turns that into u = 0.5 + a * size_u / worldUV *
// scale, v = (b + 0.5) * size_v / worldUV * scale for plane coordinates a, b in
// [-0.5, 0.5]: a few thousandths of the texture, i.e. one or two texels around u = 0.5 of
// the bottom row (nearest filtering), so a v3.1.1 ladder is flat-coloured by its texture.
// The colour comes from the vertex colours: rails getColor(colr) at the top and
// getColor(colr, 0.7) at the bottom (v7 generateCube with amb), rungs getColor(colr): v7
// MapManager.addLadder still sets opts.dark = 0.6 + i / n * 0.4, but generatePlane reads
// opts.darken, so the rungs are not darkened any more (v3.1.1 darkened them).
#define LADDER_RUNG_SPACING 6.0f

typedef struct {
    vertex *vertices;
    vec3 *colors;
    unsigned int *indices;
    int vertex_count;
    int index_count;
} PrefabLadderBuild;

// One THREE.PlaneGeometry(1, 1) quad: corner (a, b) at center + a * axis_u + b * axis_v
// (already scaled), CCW from the axis_u x axis_v side; uv_size = size / worldUV * scale.
static void prefab_ladder_quad(PrefabLadderBuild *b, const vec3 center, const vec3 axis_u, const vec3 axis_v,
                               const vec2 uv_size, const vec3 color_top, const vec3 color_bottom) {
    static const float corners[4][2] = {{-0.5f, 0.5f}, {0.5f, 0.5f}, {-0.5f, -0.5f}, {0.5f, -0.5f}};
    static const unsigned int quad[6] = {0, 2, 1, 2, 3, 1}; // PlaneGeometry faces (a, c, b), (c, d, b)
    const int base = b->vertex_count;

    for (int i = 0; i < 4; i++) {
        const float a = corners[i][0], c = corners[i][1];
        vertex *v = &b->vertices[base + i];
        v->position = (vec3) {
            center.x + axis_u.x * a + axis_v.x * c,
            center.y + axis_u.y * a + axis_v.y * c,
            center.z + axis_u.z * a + axis_v.z * c,
        };
        v->tex_coord = (vec2) {0.5f + a * uv_size.x, (c + 0.5f) * uv_size.y};
        b->colors[base + i] = c > 0.0f ? color_top : color_bottom;
    }
    for (int i = 0; i < 6; i++) b->indices[b->index_count + i] = (unsigned int) base + quad[i];
    b->vertex_count += 4;
    b->index_count += 6;
}

// 31.js generateCube(faces 1,1,1,1,1,1) scaled to size, foot at (x, y, z). Rails are never
// rotated (addCube without xR/yR/zR) and never flipped (flipp needs a cube wider than tall).
static void prefab_ladder_rail(PrefabLadderBuild *b, const float x, const float y, const float z, const vec3 size,
                               const float uv_scale, const vec3 top, const vec3 bottom) {
    const float hx = size.x * 0.5f, hz = size.z * 0.5f;
    const vec3 mid = {x, y + size.y * 0.5f, z};
    const float wu = game_constants.world_uv_scale;
    const vec2 uv_xy = {size.x / wu * uv_scale, size.y / wu * uv_scale};
    const vec2 uv_zy = {size.z / wu * uv_scale, size.y / wu * uv_scale};
    const vec2 uv_xz = {size.x / wu * uv_scale, size.z / wu * uv_scale};

    // sides: top vertices getColor(colr), bottom getColor(colr, amb); top face all top, bottom face all bottom
    prefab_ladder_quad(b, (vec3) {x, mid.y, z + hz}, (vec3) {size.x, 0, 0}, (vec3) {0, size.y, 0}, uv_xy, top, bottom);  // +Z (face 4)
    prefab_ladder_quad(b, (vec3) {x, mid.y, z - hz}, (vec3) {-size.x, 0, 0}, (vec3) {0, size.y, 0}, uv_xy, top, bottom); // -Z (face 5)
    prefab_ladder_quad(b, (vec3) {x - hx, mid.y, z}, (vec3) {0, 0, size.z}, (vec3) {0, size.y, 0}, uv_zy, top, bottom);  // -X (face 1)
    prefab_ladder_quad(b, (vec3) {x + hx, mid.y, z}, (vec3) {0, 0, -size.z}, (vec3) {0, size.y, 0}, uv_zy, top, bottom); // +X (face 0)
    prefab_ladder_quad(b, (vec3) {x, y + size.y, z}, (vec3) {size.x, 0, 0}, (vec3) {0, 0, -size.z}, uv_xz, top, top);    // top (face 2)
    prefab_ladder_quad(b, (vec3) {x, y, z}, (vec3) {size.x, 0, 0}, (vec3) {0, 0, size.z}, uv_xz, bottom, bottom);       // bottom (face 3)
}

static Geometry *prefab_ladder_geo(const float height, const float direction, const float uv_scale, const vec4 color) {
    const float ladder_scale = game_constants.ladder_scale;
    const float ladder_width = game_constants.ladder_width;
    const int rungs = height > 0.0f ? (int) floorf(height / LADDER_RUNG_SPACING) : 0;
    const int vertex_count = 2 * 24 + rungs * 4;
    const int index_count = 2 * 36 + rungs * 6;

    PrefabLadderBuild b = {
        malloc((size_t) vertex_count * sizeof(vertex)),
        malloc((size_t) vertex_count * sizeof(vec3)),
        malloc((size_t) index_count * sizeof(unsigned int)),
        0, 0,
    };
    Geometry *geometry = NULL;
    if (!b.vertices || !b.colors || !b.indices) goto done;

    const vec4 top4 = prefab_krunker_color(color, 1.0f);
    const vec4 bottom4 = prefab_krunker_color(color, PREFAB_AMBIENT_MULT);
    const vec3 top = {top4.x, top4.y, top4.z};
    const vec3 bottom = {bottom4.x, bottom4.y, bottom4.z};

    // addCube(x +- ladderWidth * sin(d), y, z +- ladderWidth * cos(d), ladderScale * 2, h + 2, ladderScale * 2)
    const vec3 rail = {ladder_scale * 2.0f, height + 2.0f, ladder_scale * 2.0f};
    const float sx = ladder_width * sinf(direction), sz = ladder_width * cosf(direction);
    prefab_ladder_rail(&b, sx, 0.0f, sz, rail, uv_scale, top, bottom);
    prefab_ladder_rail(&b, -sx, 0.0f, -sz, rail, uv_scale, top, bottom);

    // addPlane(x, y + (i + 1) * 6 + randFloat(-1, 1), z, ladderWidth, ladderScale, ...,
    // -d + PI/2, PI/2, randFloat(-0.1, 0.1)): rotateY(-d + PI/2), rotateX(0), rotateZ(r),
    // scale(ladderWidth * 2, ladderScale * 2, 1); generatePlane(ladderScale, ladderWidth)
    // scales u by ladderWidth and v by ladderScale. Front side faces (cos d, 0, sin d).
    const float wu = game_constants.world_uv_scale;
    const vec2 rung_uv = {ladder_width / wu * uv_scale, ladder_scale / wu * uv_scale};
    const float yaw = (float) M_PI * 0.5f - direction;
    for (int i = 0; i < rungs; i++) {
        const float displace = ((float) pcg32_random() / (float) UINT32_MAX - 0.5f) * 2.0f;
        const float roll = ((float) pcg32_random() / (float) UINT32_MAX - 0.5f) * 2.0f * 0.1f;
        const vec3 dark = top;

        // Ry(yaw) * Rz(roll) applied to the local x (width) and y (height) axes
        const float ux = cosf(roll), uy = sinf(roll);
        const float vx = -sinf(roll), vy = cosf(roll);
        const vec3 axis_u = {ux * cosf(yaw) * ladder_width * 2.0f, uy * ladder_width * 2.0f, -ux * sinf(yaw) * ladder_width * 2.0f};
        const vec3 axis_v = {vx * cosf(yaw) * ladder_scale * 2.0f, vy * ladder_scale * 2.0f, -vx * sinf(yaw) * ladder_scale * 2.0f};
        const vec3 center = {0.0f, (float) (i + 1) * LADDER_RUNG_SPACING + displace, 0.0f};
        prefab_ladder_quad(&b, center, axis_u, axis_v, rung_uv, dark, dark);
    }

    geometry = prefab_upload_geo(b.vertices, b.vertex_count, b.indices, b.index_count);
    if (geometry) {
        unsigned int color_vbo;
        glBindVertexArray(geometry->vao);
        glGenBuffers(1, &color_vbo);
        glBindBuffer(GL_ARRAY_BUFFER, color_vbo);
        glBufferData(GL_ARRAY_BUFFER, (long) b.vertex_count * (long) sizeof(vec3), b.colors, GL_STATIC_DRAW);
        glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(vec3), NULL);
        glEnableVertexAttribArray(2);
    }

done:
    free(b.vertices);
    free(b.colors);
    free(b.indices);
    return geometry;
}

// THREE.PlaneGeometry(1, 1): the XY unit square facing +Z, CCW from the front. Krunker
// planes, billboards, signs, liquids and ramps all start from it (55.js addPlane/addRamp).
static Geometry *prefab_plane_xy_geo() {
    static Geometry *cached;
    static const vertex vertices[] = {
        {-0.5f, -0.5f, 0.0f, 0.0f, 0.0f},
        {0.5f, 0.5f, 0.0f, 1.0f, 1.0f},
        {-0.5f, 0.5f, 0.0f, 0.0f, 1.0f},
        {0.5f, -0.5f, 0.0f, 1.0f, 0.0f},
    };
    static const unsigned int indices[] = {0, 1, 2, 0, 3, 1};

    if (!cached) cached = prefab_upload_geo(vertices, 4, indices, 6);
    return cached;
}

static Geometry *prefab_cube_geo(const int face_mask) {
    static Geometry *cached[64];
    static const vertex vertices[] = {
        // FRONT (Krunker face 4)
        {-0.5f, 0.0f, 0.5f, 0.0f, 0.0f},
        {0.5f, 1.0f, 0.5f, 1.0f, 1.0f},
        {-0.5f, 1.0f, 0.5f, 0.0f, 1.0f},
        {0.5f, 0.0f, 0.5f, 1.0f, 0.0f},

        // BACK (Krunker face 5)
        {0.5f, 0.0f, -0.5f, 0.0f, 0.0f},
        {-0.5f, 1.0f, -0.5f, 1.0f, 1.0f},
        {0.5f, 1.0f, -0.5f, 0.0f, 1.0f},
        {-0.5f, 0.0f, -0.5f, 1.0f, 0.0f},

        // LEFT (Krunker face 1)
        {-0.5f, 0.0f, -0.5f, 0.0f, 0.0f},
        {-0.5f, 1.0f, 0.5f, 1.0f, 1.0f},
        {-0.5f, 1.0f, -0.5f, 0.0f, 1.0f},
        {-0.5f, 0.0f, 0.5f, 1.0f, 0.0f},

        // RIGHT (Krunker face 0)
        {0.5f, 0.0f, 0.5f, 0.0f, 0.0f},
        {0.5f, 1.0f, -0.5f, 1.0f, 1.0f},
        {0.5f, 1.0f, 0.5f, 0.0f, 1.0f},
        {0.5f, 0.0f, -0.5f, 1.0f, 0.0f},

        // TOP (Krunker face 2)
        {-0.5f, 1.0f, 0.5f, 0.0f, 0.0f},
        {0.5f, 1.0f, -0.5f, 1.0f, 1.0f},
        {-0.5f, 1.0f, -0.5f, 0.0f, 1.0f},
        {0.5f, 1.0f, 0.5f, 1.0f, 0.0f},

        // BOTTOM (Krunker face 3)
        {-0.5f, 0.0f, -0.5f, 0.0f, 0.0f},
        {0.5f, 0.0f, 0.5f, 1.0f, 1.0f},
        {-0.5f, 0.0f, 0.5f, 0.0f, 1.0f},
        {0.5f, 0.0f, -0.5f, 1.0f, 0.0f},
    };
    static const unsigned int face_indices[][6] = {
        {0, 1, 2, 0, 3, 1},
        {4, 5, 6, 4, 7, 5},
        {8, 9, 10, 8, 11, 9},
        {12, 13, 14, 12, 15, 13},
        {16, 17, 18, 16, 19, 17},
        {20, 21, 22, 20, 23, 21},
    };
    static const int krunker_to_native[] = {3, 2, 4, 5, 0, 1};

    if (face_mask == 0x3f) return create_cube_geo();
    if (face_mask >= 0 && face_mask < 64 && cached[face_mask]) return cached[face_mask];

    unsigned int indices[36];
    size_t index_count = 0;

    for (int i = 0; i < 6; i++) {
        if (!(face_mask & (1 << i))) continue;

        const int native_face = krunker_to_native[i];
        memcpy(indices + index_count, face_indices[native_face], sizeof(face_indices[native_face]));
        index_count += 6;
    }

    Geometry *geometry = calloc(1, sizeof(Geometry));
    if (!geometry) return NULL;

    unsigned int vbo;
    glGenVertexArrays(1, &geometry->vao);
    glGenBuffers(1, &geometry->ebo);
    glGenBuffers(1, &vbo);

    glBindVertexArray(geometry->vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    geometry_set_bounds(geometry, vertices, sizeof(vertices) / sizeof(vertices[0]));

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, geometry->ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (int) (index_count * sizeof(unsigned int)), indices, GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), NULL);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *) (3 * sizeof(float)));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);

    geometry->index_count = (int) index_count;
    if (face_mask >= 0 && face_mask < 64) cached[face_mask] = geometry;
    return geometry;
}

static Geometry *prefab_sphere_geo() {
    static Geometry *cached;
    if (cached) return cached;

    // THREE.SphereGeometry(1, 12, 8) (31.js generateSphere): 12 segments, 8 rings, v = 1 at the top
    const int rings = 8;
    const int segments = 12;
    const int vertex_count = (rings + 1) * (segments + 1);
    const int index_count = rings * segments * 6;

    vertex *vertices = calloc(vertex_count, sizeof(vertex));
    unsigned int *indices = calloc(index_count, sizeof(unsigned int));

    if (!vertices || !indices) {
        free(vertices);
        free(indices);
        return NULL;
    }

    int v = 0;
    for (int y = 0; y <= rings; y++) {
        const float vy = (float) y / (float) rings;
        const float phi = vy * (float) M_PI;

        for (int x = 0; x <= segments; x++) {
            const float vx = (float) x / (float) segments;
            const float theta = vx * 2.0f * (float) M_PI;
            vertices[v++] = (vertex) {
                -cosf(theta) * sinf(phi) * 0.5f,
                0.5f + cosf(phi) * 0.5f,
                sinf(theta) * sinf(phi) * 0.5f,
                vx,
                1.0f - vy,
            };
        }
    }

    int idx = 0;
    for (int y = 0; y < rings; y++) {
        for (int x = 0; x < segments; x++) {
            const int a = y * (segments + 1) + x;
            const int b = a + segments + 1;

            indices[idx++] = a;
            indices[idx++] = b;
            indices[idx++] = a + 1;
            indices[idx++] = a + 1;
            indices[idx++] = b;
            indices[idx++] = b + 1;
        }
    }

    Geometry *geometry = calloc(1, sizeof(Geometry));
    if (!geometry) {
        free(vertices);
        free(indices);
        return NULL;
    }

    unsigned int vbo;
    glGenVertexArrays(1, &geometry->vao);
    glGenBuffers(1, &geometry->ebo);
    glGenBuffers(1, &vbo);

    glBindVertexArray(geometry->vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, vertex_count * sizeof(vertex), vertices, GL_STATIC_DRAW);
    geometry_set_bounds(geometry, vertices, (size_t) vertex_count);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, geometry->ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, index_count * sizeof(unsigned int), indices, GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), NULL);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *) (3 * sizeof(float)));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);

    geometry->index_count = index_count;
    cached = geometry;

    free(vertices);
    free(indices);
    return geometry;
}

// THREE.PlaneGeometry(1, 1, tiles_x, tiles_z): (tiles_x + 1) x (tiles_z + 1) vertices,
// row by row from the top (+y) like three.js, so a vertex's index is its gl_VertexID (the
// liquid waves in basic.vert depend on it). Cached per grid size.
static Geometry *prefab_plane_grid_geo(int tiles_x, int tiles_z) {
    static struct { int x, z; Geometry *geometry; } cache[32];
    static int cache_count;

    if (tiles_x < 1) tiles_x = 1;
    if (tiles_z < 1) tiles_z = 1;
    if (tiles_x > 256) tiles_x = 256;
    if (tiles_z > 256) tiles_z = 256;

    for (int i = 0; i < cache_count; i++) {
        if (cache[i].x == tiles_x && cache[i].z == tiles_z) return cache[i].geometry;
    }

    const int columns = tiles_x + 1;
    const int vertex_count = columns * (tiles_z + 1);
    const int index_count = tiles_x * tiles_z * 6;
    vertex *vertices = calloc((size_t) vertex_count, sizeof(vertex));
    unsigned int *indices = calloc((size_t) index_count, sizeof(unsigned int));
    Geometry *geometry = NULL;

    if (vertices && indices) {
        for (int iy = 0; iy <= tiles_z; iy++) {
            for (int ix = 0; ix <= tiles_x; ix++) {
                const float x = (float) ix / (float) tiles_x - 0.5f;
                const float y = 0.5f - (float) iy / (float) tiles_z;
                vertices[iy * columns + ix] = (vertex) {x, y, 0.0f, x + 0.5f, y + 0.5f};
            }
        }

        int k = 0;
        for (int iy = 0; iy < tiles_z; iy++) {
            for (int ix = 0; ix < tiles_x; ix++) {
                const unsigned int a = (unsigned int) (ix + columns * iy);
                const unsigned int b = (unsigned int) (ix + columns * (iy + 1));
                const unsigned int c = (unsigned int) (ix + 1 + columns * (iy + 1));
                const unsigned int d = (unsigned int) (ix + 1 + columns * iy);
                indices[k++] = a; indices[k++] = b; indices[k++] = d;
                indices[k++] = b; indices[k++] = c; indices[k++] = d;
            }
        }

        geometry = prefab_upload_geo(vertices, vertex_count, indices, index_count);
        if (geometry) geometry->bound_radius = 0.0f; // liquid waves displace it in basic.vert: never culled
    }

    free(vertices);
    free(indices);

    if (geometry && cache_count < 32) {
        cache[cache_count].x = tiles_x;
        cache[cache_count].z = tiles_z;
        cache[cache_count++].geometry = geometry;
    }

    return geometry;
}

// ---- textures (render.js loadTexture / getMat)

// render.js missingTexture: the 2x2 checker (#ff00db / black) shown for a texture that fails to
// load (user assets KN does not ship, mostly). Owned here for the process lifetime.
static unsigned int g_prefab_missing_texture;

static unsigned int prefab_missing_texture(void) {
    if (g_prefab_missing_texture) return g_prefab_missing_texture;
    // rows bottom-up (GL): the PNG's palette indices are magenta / black, black / magenta
    static const unsigned char pixels[16] = {0, 0, 0, 253, 255, 0, 219, 255, 255, 0, 219, 252, 0, 0, 0, 255};
    glGenTextures(1, &g_prefab_missing_texture);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_prefab_missing_texture);
    g_active_texture = g_prefab_missing_texture;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    return g_prefab_missing_texture;
}

// A file under assets/ (textures/<src>.png, user-assets/<id>/texture.png): its cached texture,
// the missing-texture checker when it does not load (render.js loadTexture error path).
static unsigned int prefab_load_texture_file(const char *relative) {
    char *path = concat(assets_path(), relative);
    if (!path) return prefab_missing_texture();
    // load_texture caches successes only: remember failures so a missing file is tried once
    static char failed[64][128];
    static int failed_count;
    for (int i = 0; i < failed_count; i++) {
        if (!strcmp(failed[i], relative)) {
            free(path);
            return prefab_missing_texture();
        }
    }
    const unsigned int texture = load_texture(path);
    free(path);
    if (texture) return texture;
    if (failed_count < 64) snprintf(failed[failed_count++], sizeof(failed[0]), "%s", relative);
    return prefab_missing_texture();
}

static unsigned int prefab_load_texture_src(const char *src) {
    char relative[160];
    snprintf(relative, sizeof(relative), "textures/%s.png", src);
    return prefab_load_texture_file(relative);
}

// A src of the v7 map texture table, a user asset ("<id>/texture", the "at" key) or a video asset
// ("<id>/video"): v7 plays user-assets/<id>/video.mp4 on a VideoTexture. KN has no video
// textures: a video asset shows its "<id>/texture.png" when one ships, else stays black like a
// VideoTexture before its first frame (documented placeholder).
static unsigned int prefab_texture(const char *src, const int asset, const int video) {
    if (!src || !src[0] || !strcmp(src, "default")) return 0;
    if (!asset) return prefab_load_texture_src(src);

    char relative[160];
    if (video) {
        char id[32];
        snprintf(id, sizeof(id), "%s", src);
        char *slash = strchr(id, '/');
        if (slash) *slash = 0;
        snprintf(relative, sizeof(relative), "user-assets/%s/texture.png", id);
        char *path = concat(assets_path(), relative);
        FILE *f = path && ensure_asset_file(path) ? fopen(path, "rb") : NULL;
        free(path);
        if (!f) {
            static unsigned int black;
            if (!black) {
                static const unsigned char pixel[4] = {0, 0, 0, 255};
                glGenTextures(1, &black);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, black);
                g_active_texture = black;
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
            }
            return black;
        }
        fclose(f);
        return prefab_load_texture_file(relative);
    }
    snprintf(relative, sizeof(relative), "user-assets/%s.png", src);
    return prefab_load_texture_file(relative);
}

static int prefab_is_trans_tex(const char *src) {
    return src && !strncmp(src, "link_", 5); // utils.js isTransTex
}

// ---- rotation helpers

// Row-major 3x3 -> the XYZ Euler (Rx * Ry * Rz) of MeshTransform ROTATION_ORDER_INTRINSIC
// (three.js Euler.setFromRotationMatrix, order "XYZ").
static vec3 prefab_euler_from_matrix(const float m[9]) {
    vec3 e;
    const float m13 = fminf(fmaxf(m[2], -1.0f), 1.0f);
    e.y = asinf(m13);
    if (fabsf(m13) < 0.9999999f) {
        e.x = atan2f(-m[5], m[8]);
        e.z = atan2f(-m[1], m[0]);
    } else {
        e.x = atan2f(m[7], m[4]);
        e.z = 0.0f;
    }
    return e;
}

static void prefab_mat3_euler_xyz(const vec3 r, float m[9]) {
    const float cx = cosf(r.x), sx = sinf(r.x), cy = cosf(r.y), sy = sinf(r.y), cz = cosf(r.z), sz = sinf(r.z);
    const float out[9] = {
        cy * cz, -cy * sz, sy,
        cx * sz + sx * sy * cz, cx * cz - sx * sy * sz, -sx * cy,
        sx * sz - cx * sy * cz, sx * cz + cx * sy * sz, cx * cy,
    };
    memcpy(m, out, sizeof(out));
}

static void prefab_mat3_mul(const float a[9], const float b[9], float out[9]) {
    float r[9];
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) r[i * 3 + j] = a[i * 3] * b[j] + a[i * 3 + 1] * b[3 + j] + a[i * 3 + 2] * b[6 + j];
    }
    memcpy(out, r, sizeof(r));
}

static vec3 prefab_mat3_apply(const float m[9], const vec3 v) {
    return (vec3) {m[0] * v.x + m[1] * v.y + m[2] * v.z, m[3] * v.x + m[4] * v.y + m[5] * v.z, m[6] * v.x + m[7] * v.y + m[8] * v.z};
}

// The v7 builder rotation of an object (shared/src/map.c map_object_rotation) as the mesh's Euler.
static vec3 prefab_object_euler(const Object *object) {
    float m[9];
    map_object_rotation(object, m);
    return prefab_euler_from_matrix(m);
}

// ---- per-object material inputs (map.js generate option bags)

// map.colors (the "ci"/"ei" palette) of the map being built
static const cJSON *g_prefab_palette;

typedef struct {
    vec4 colr;          // "c" / "ci" (white when absent); alpha = opacity
    int has_colr;
    vec4 emissive;      // "e" / "ei", none = alpha 0 (three.js emissive: e || null)
    float opacity;      // "o" != 1 && o != null ? o : 1
    char src[96];       // texture src, "" = none
    int asset;          // src is a user asset ("<id>/texture" / "<id>/video")
    int video;
} PrefabStyle;

static vec4 prefab_json_color(const cJSON *raw_color, const cJSON *raw_color_idx, const vec4 *colors, const vec4 fallback, int *set);

static void prefab_style_init(PrefabStyle *st, const Object *object, const vec4 *colors, const cJSON *raw_obj) {
    memset(st, 0, sizeof(*st));
    const vec4 white = {1.0f, 1.0f, 1.0f, 1.0f};
    const vec4 none = {0.0f, 0.0f, 0.0f, 0.0f};
    st->colr = prefab_json_color(cJSON_GetObjectItem(raw_obj, "c"), cJSON_GetObjectItem(raw_obj, "ci"), colors, white, &st->has_colr);
    st->emissive = prefab_json_color(cJSON_GetObjectItem(raw_obj, "e"), cJSON_GetObjectItem(raw_obj, "ei"), colors, none, NULL);
    const cJSON *o = cJSON_GetObjectItem(raw_obj, "o");
    st->opacity = prefab_json_present(o) ? prefab_json_float(o, 1.0f) : 1.0f;
    st->colr.w = st->opacity;

    // map.js: at -> "<id>/texture" (or "/video"); otherwise textures[t] (+ "_" + variant id)
    if (object->asset_texture >= 0) {
        snprintf(st->src, sizeof(st->src), "%d/%s", object->asset_texture, object->asset_video ? "video" : "texture");
        st->asset = 1;
        st->video = object->asset_video;
    } else {
        const cJSON *t = cJSON_GetObjectItem(raw_obj, "t");
        int has_t = prefab_json_present(t), value = -1;
        if (cJSON_IsNumber(t)) value = (int) cJSON_GetNumberValue(t);
        else if (cJSON_IsString(t)) {
            char *end = NULL;
            const long n = strtol(cJSON_GetStringValue(t), &end, 10);
            if (end && end != cJSON_GetStringValue(t) && !*end) value = (int) n; // a texture *name* falls back (v7)
        }
        const int texture = prefab_texture_resolve(object->prefab, has_t, value);
        if (!prefab_texture_src(texture, object->tex_variant, st->src, sizeof(st->src))) st->src[0] = 0;
    }
}

// geos.js getColor(colr, mult): THREE.Color(colr) * mult, HSL saturation * 1.5
static vec4 prefab_vertex_color(const PrefabStyle *st, const float mult) {
    vec4 c = prefab_krunker_color(st->colr, mult);
    c.w = st->opacity;
    return c;
}

// texOffX / texOffY: limitMM(tox || 0, 0, 1).round(2) (v3.1.1 rounded to 1 decimal)
static vec2 prefab_texture_offset(const cJSON *raw_obj) {
    vec2 offset = {
        prefab_json_float(cJSON_GetObjectItem(raw_obj, "tox"), 0.0f),
        prefab_json_float(cJSON_GetObjectItem(raw_obj, "toy"), 0.0f),
    };
    offset.x = roundf(fminf(fmaxf(offset.x, 0.0f), 1.0f) * 100.0f) / 100.0f;
    offset.y = roundf(fminf(fmaxf(offset.y, 0.0f), 1.0f) * 100.0f) / 100.0f;
    return offset;
}

static TextureAnimation *prefab_tex_anim(Object *object) {
    if (!object->tex_anim) object->tex_anim = calloc(1, sizeof(TextureAnimation));
    return object->tex_anim;
}

// render.js loadTexture: frames (fct, sheet of min(10, n) x ceil(n / 10)), frameT (ms), movT
// ("ts" / 10000 per ms, scrolled by main.c client_animate_object_texture), texRot ("tro"),
// texEncode ("ten"), texture offset (tox / toy).
static void prefab_apply_texture_keys(BasicMaterial *material, Object *object, const cJSON *raw_obj, const int texture_offset) {
    material->texture_rotation = object->tex_rotation * (float) M_PI / 180.0f;
    material->texture_srgb = object->tex_encoding == 1;
    if (texture_offset) material->texture_base_offset = prefab_texture_offset(raw_obj);

    if (object->frames > 0) {
        TextureAnimation *anim = prefab_tex_anim(object);
        if (anim) {
            anim->frames = object->frames;
            anim->frame_time = object->frame_time / 1000.0f; // 0: the sheet stays on its first frame
        }
    }

    const cJSON *ts = cJSON_GetObjectItem(raw_obj, "ts");
    if (cJSON_IsNumber(ts) && cJSON_GetNumberValue(ts) != 0.0) {
        TextureAnimation *anim = prefab_tex_anim(object);
        if (anim) {
            anim->move = (float) cJSON_GetNumberValue(ts) / 10.0f; // movT = ts / 10000 per ms
            const cJSON *td = cJSON_GetObjectItem(raw_obj, "td");
            anim->move_direction = prefab_json_float(td, 0.0f) != 0.0f; // movD 0 scrolls offset.x
        }
    }
}

// three.js side: rs 0 FrontSide, 1 BackSide, 2 DoubleSide
static void prefab_apply_side(BasicMaterial *material, const int side) {
    material->double_sided = side == 2;
    material->cull_front = side == 1;
}

// MapManager: transparent = fTrans || isTransTex(src) || opacity != 1; link textures (isTransTex)
// write depth only when opaque and alpha-test at 0.02 while transparent.
static void prefab_apply_transparency(BasicMaterial *material, const Object *object, const PrefabStyle *st, const int link_depth_always) {
    const int link = prefab_is_trans_tex(st->src);
    material->base.transparent = object->force_trans || link || st->opacity != 1.0f;
    if (link) {
        material->no_depth_write = link_depth_always ? 0 : st->opacity != 1.0f;
        material->alpha_test = material->base.transparent ? 0.02f : 0.0f;
    }
}

// three.js r150 texture loading for a src honours the noTex setting (getMat("default")) unless forced
static unsigned int prefab_style_texture(const PrefabStyle *st, const int force) {
    if (client_render_settings()->no_textures && !force) return 0;
    return prefab_texture(st->src, st->asset, st->video);
}

// three.js r150 CylinderGeometry(rTop, rBot, 1, sides, 1, open, thetaStart, thetaLength) as
// geos.js generateCylinder / generateCone build it: the torso, then the top and bottom caps, every
// UV through vF23 (u = 0.5 + (u - 0.5) * uv_u, v *= uv_v; uv_u = 0: untouched), translated up
// by 0.5 (base at y = 0). Cached per parameter set for the map's lifetime.
typedef struct {
    float r_top, r_bot, theta_start, theta_length, uv_u, uv_v;
    int sides, open;
    Geometry *geometry;
} PrefabCylinderGeo;

static PrefabCylinderGeo *g_prefab_cylinders;
static size_t g_prefab_cylinder_count;

static Geometry *prefab_cylinder_geo(const float r_top, const float r_bot, int sides, const int open,
                                     const float theta_start, const float theta_length, const float uv_u, const float uv_v) {
    if (sides < 3) sides = 3;
    if (sides > 64) sides = 64;
    for (size_t i = 0; i < g_prefab_cylinder_count; i++) {
        const PrefabCylinderGeo *c = &g_prefab_cylinders[i];
        if (c->r_top == r_top && c->r_bot == r_bot && c->sides == sides && c->open == open && c->theta_start == theta_start &&
            c->theta_length == theta_length && c->uv_u == uv_u && c->uv_v == uv_v) return c->geometry;
    }

    const int caps = open ? 0 : (r_top > 0.0f) + (r_bot > 0.0f);
    const int vertex_count = (sides + 1) * 2 + caps * (sides + sides + 1);
    const int index_count = sides * 6 + caps * sides * 3;
    vertex *v = calloc((size_t) vertex_count, sizeof(vertex));
    unsigned int *idx = calloc((size_t) index_count, sizeof(unsigned int));
    Geometry *geometry = NULL;
    if (!v || !idx) goto done;

    int n = 0, k = 0;
    for (int y = 0; y <= 1; y++) {
        const float radius = (float) y * (r_bot - r_top) + r_top;
        for (int x = 0; x <= sides; x++) {
            const float u = (float) x / (float) sides;
            const float theta = u * theta_length + theta_start;
            v[n++] = (vertex) {{radius * sinf(theta), -(float) y + 0.5f, radius * cosf(theta)}, {u, 1.0f - (float) y}};
        }
    }
    for (int x = 0; x < sides; x++) {
        const unsigned int a = (unsigned int) x, b = (unsigned int) (sides + 1 + x), c = b + 1, d = a + 1;
        idx[k++] = a; idx[k++] = b; idx[k++] = d;
        idx[k++] = b; idx[k++] = c; idx[k++] = d;
    }
    for (int cap = 0; cap < 2 && !open; cap++) {
        const int top = cap == 0;
        const float radius = top ? r_top : r_bot;
        if (radius <= 0.0f) continue;
        const float sign = top ? 1.0f : -1.0f;
        const int center_start = n;
        for (int x = 1; x <= sides; x++) v[n++] = (vertex) {{0.0f, 0.5f * sign, 0.0f}, {0.5f, 0.5f}};
        const int center_end = n;
        for (int x = 0; x <= sides; x++) {
            const float theta = (float) x / (float) sides * theta_length + theta_start;
            v[n++] = (vertex) {{radius * sinf(theta), 0.5f * sign, radius * cosf(theta)}, {cosf(theta) * 0.5f + 0.5f, sinf(theta) * 0.5f * sign + 0.5f}};
        }
        for (int x = 0; x < sides; x++) {
            const unsigned int c = (unsigned int) (center_start + x), i = (unsigned int) (center_end + x);
            if (top) { idx[k++] = i; idx[k++] = i + 1; idx[k++] = c; }
            else { idx[k++] = i + 1; idx[k++] = i; idx[k++] = c; }
        }
    }
    for (int i = 0; i < n; i++) {
        if (uv_u != 0.0f) {
            v[i].tex_coord.x = 0.5f + (v[i].tex_coord.x - 0.5f) * uv_u;
            v[i].tex_coord.y *= uv_v;
        }
        v[i].position.y += 0.5f;
    }

    geometry = prefab_upload_geo(v, n, idx, k);
    if (geometry) {
        PrefabCylinderGeo *grown = realloc(g_prefab_cylinders, (g_prefab_cylinder_count + 1) * sizeof(PrefabCylinderGeo));
        if (grown) {
            g_prefab_cylinders = grown;
            g_prefab_cylinders[g_prefab_cylinder_count++] = (PrefabCylinderGeo) {r_top, r_bot, theta_start, theta_length, uv_u, uv_v, sides, open, geometry};
        }
    }
done:
    free(v);
    free(idx);
    return geometry;
}

// geos.js generateRamps (RAMP "ec"): THREE.ExtrudeGeometry(Shape([(0,0), (0,1), (1,0)]), {depth 1,
// bevelEnabled false}) translated by -0.5, with WorldUVGenerator UVs, then vF23(uv_u, uv_v).
// ExtrudeGeometry: the front (z = 0, reversed) and back (z = depth) caps, then one side wall
// per contour edge. Cached per UV scale.
static Geometry *prefab_ramp_prism_geo(const float uv_u, const float uv_v) {
    static struct { float u, v; Geometry *geometry; } cache[64];
    static int cache_count;
    for (int i = 0; i < cache_count; i++) {
        if (cache[i].u == uv_u && cache[i].v == uv_v) return cache[i].geometry;
    }

    // The outline (0,0) (0,1) (1,0) is clockwise (ShapeUtils.area < 0), so ExtrudeGeometry keeps
    // it. The ramp material is DoubleSide, so the cap winding (earcut order) does not matter.
    static const float contour[3][2] = {{0.0f, 0.0f}, {0.0f, 1.0f}, {1.0f, 0.0f}};
    vertex verts[3 * 2 + 3 * 6];
    int n = 0;
    for (int layer = 0; layer < 2; layer++) {
        for (int i = 0; i < 3; i++) {
            const float *p = contour[layer ? i : 2 - i]; // bottom face reversed
            verts[n++] = (vertex) {{p[0], p[1], (float) layer}, {p[0], p[1]}}; // generateTopUV: (x, y)
        }
    }
    // sidewalls(): i from the last contour point down, j = i, k = i - 1 (wrapping):
    // quad a (j, z 0), b (k, z 0), c (k, z 1), d (j, z 1), generateSideWallUV
    for (int i = 2; i >= 0; i--) {
        const int j = i, kk = (i + 2) % 3;
        const float *pa = contour[j], *pb = contour[kk];
        const float ax = pa[0], ay = pa[1], bx = pb[0], by = pb[1];
        const vertex quad[4] = {
            {{ax, ay, 0.0f}, {0}}, {{bx, by, 0.0f}, {0}}, {{bx, by, 1.0f}, {0}}, {{ax, ay, 1.0f}, {0}},
        };
        vertex q[4];
        memcpy(q, quad, sizeof(q));
        const int along_x = fabsf(ay - by) < fabsf(ax - bx);
        for (int c = 0; c < 4; c++) {
            q[c].tex_coord = along_x ? (vec2) {q[c].position.x, 1.0f - q[c].position.z} : (vec2) {q[c].position.y, 1.0f - q[c].position.z};
        }
        // faces (a, b, d), (b, c, d)
        const int order[6] = {0, 1, 3, 1, 2, 3};
        for (int c = 0; c < 6; c++) verts[n++] = q[order[c]];
    }

    unsigned int idx[24];
    for (int i = 0; i < n; i++) {
        verts[i].position.x -= 0.5f;
        verts[i].position.y -= 0.5f;
        verts[i].position.z -= 0.5f;
        if (uv_u != 0.0f) {
            verts[i].tex_coord.x = 0.5f + (verts[i].tex_coord.x - 0.5f) * uv_u;
            verts[i].tex_coord.y *= uv_v;
        }
        idx[i] = (unsigned int) i;
    }
    Geometry *geometry = prefab_upload_geo(verts, n, idx, n);
    if (geometry && cache_count < 64) {
        cache[cache_count].u = uv_u;
        cache[cache_count].v = uv_v;
        cache[cache_count++].geometry = geometry;
    }
    return geometry;
}

// ---- canvas textures (SIGN: utils.js createCanvasText; LEADERBOARD: createLeaderText, map_post.c)
// drawn with the HTML-canvas port (canvas2d.h, GameFont) into an offscreen layer, read back,
// un-premultiplied (a browser canvas uploads straight alpha to WebGL) and cached by key.

static Canvas2D *g_prefab_canvas;

static void prefab_canvas_color(Canvas2D *c, const char *css, const char *fallback, const int stroke) {
    float rgba[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    if (!css || !c2d_parse_color(css, rgba)) c2d_parse_color(fallback, rgba);
    if (stroke) c2d_set_stroke_color(c, rgba[0], rgba[1], rgba[2], rgba[3]);
    else c2d_set_fill_color(c, rgba[0], rgba[1], rgba[2], rgba[3]);
}

// The canvas of utils.js createCanvasText/createLeaderText: w x h canvas px, 1024 on the
// longer side (JS canvas sizes truncate).
static void prefab_canvas_size(const float w, const float h, int *cw, int *ch) {
    float sx = 1.0f, sy = 1.0f;
    if (h > w) sx = h > 0.0f ? w / h : 1.0f;
    else if (h < w) sy = w > 0.0f ? h / w : 1.0f;
    *cw = (int) (1024.0f * sx);
    *ch = (int) (1024.0f * sy);
}

Canvas2D *prefab_canvas_begin(const int width, const int height) {
    if (!g_prefab_canvas) g_prefab_canvas = c2d_create();
    if (!g_prefab_canvas) return NULL;
    c2d_set_size(g_prefab_canvas, width, height, width, height);
    c2d_reset(g_prefab_canvas);
    return g_prefab_canvas;
}

// Flushes the canvas into a new nearest-filtered, repeating RGBA texture (cached under key).
unsigned int prefab_canvas_end(Canvas2D *c, const char *key) {
    const int width = c2d_width(c), height = c2d_height(c);
    if (width <= 0 || height <= 0) return 0;

    // Leaderboard canvases redraw from a network packet (map_post_leaderboard_rows) before the
    // frame's render_target_begin() clear: framebuffer, viewport and clear colour go back.
    GLint previous_fbo = 0, viewport[4];
    GLfloat clear_color[4];
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous_fbo);
    glGetIntegerv(GL_VIEWPORT, viewport);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, clear_color);

    unsigned int layer = 0, fbo = 0;
    glGenTextures(1, &layer);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, layer);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0); // the composite draws into it: not left on unit 0 (feedback loop)
    g_active_texture = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, layer, 0);
    glViewport(0, 0, width, height);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    c2d_composite(c); // premultiplied source-over onto transparent black

    unsigned char *rgba = malloc((size_t) width * (size_t) height * 4);
    if (rgba) {
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba); // bottom row first = GL texture order
        for (size_t i = 0; i < (size_t) width * (size_t) height; i++) {
            unsigned char *p = rgba + i * 4;
            if (p[3] == 0 || p[3] == 255) continue;
            for (int k = 0; k < 3; k++) {
                const int v = (p[k] * 255 + p[3] / 2) / p[3];
                p[k] = (unsigned char) (v > 255 ? 255 : v);
            }
        }
    }

    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint) previous_fbo);
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    glClearColor(clear_color[0], clear_color[1], clear_color[2], clear_color[3]);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &layer);

    unsigned int texture = 0;
    if (rgba) {
        glGenTextures(1, &texture);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture);
        g_active_texture = texture;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        free(rgba);
        if (key) {
            char *cache_key = malloc(strlen(key) + 1);
            if (cache_key) {
                strcpy(cache_key, key);
                vt_insert(&g_texture_cache, cache_key, texture);
            }
        }
    }
    return texture;
}

// utils.js ctxText: translate(~~x, ~~y), strokeText (outline colour, lineWidth, miterLimit 2,
// round joins) then fillText
void prefab_canvas_text(Canvas2D *c, const char *text, const char *color, const double x, const double y,
                        const char *align, const double font_px, const char *outline, const double outline_width) {
    c2d_save(c);
    c2d_translate(c, (double) (long) x, (double) (long) y);
    if (font_px > 0.0) c2d_set_font(c, font_px, "GameFont");
    prefab_canvas_color(c, color, "#000000", 0);
    if (align) c2d_set_text_align_name(c, align);
    prefab_canvas_color(c, outline, "#626262", 1);
    c2d_set_line_width(c, outline_width > 0.0 ? outline_width : 1.0);
    c2d_set_miter_limit(c, 2.0);
    c2d_set_line_join(c, C2D_JOIN_ROUND);
    c2d_stroke_text(c, text, 0.0, 0.0, 0, 0.0);
    c2d_fill_text(c, text, 0.0, 0.0, 0, 0.0);
    c2d_restore(c);
}

// utils.js getLines: words joined while measureText(line + " " + word) < max_width
static int prefab_canvas_lines(Canvas2D *c, const char *text, const double max_width, char ***out) {
    int count = 0, capacity = 0;
    char **lines = NULL;
    char *copy = strdup(text ? text : "");
    if (!copy) return 0;
    char *line = NULL;
    char *save = copy;
    // split on single spaces like String.split(" ") (empty words kept)
    for (char *word = copy;; ) {
        char *space = strchr(word, ' ');
        if (space) *space = 0;
        if (!line) {
            line = strdup(word);
        } else {
            const size_t n = strlen(line) + 1 + strlen(word) + 1;
            char *joined = malloc(n);
            if (joined) snprintf(joined, n, "%s %s", line, word);
            if (joined && c2d_measure_text(c, joined) < max_width) {
                free(line);
                line = joined;
            } else {
                free(joined);
                if (count == capacity) {
                    capacity = capacity ? capacity * 2 : 8;
                    char **grown = realloc(lines, (size_t) capacity * sizeof(char *));
                    if (!grown) break;
                    lines = grown;
                }
                lines[count++] = line;
                line = strdup(word);
            }
        }
        if (!space) break;
        word = space + 1;
    }
    if (line) {
        char **grown = realloc(lines, (size_t) (count + 1) * sizeof(char *));
        if (grown) {
            lines = grown;
            lines[count++] = line;
        } else {
            free(line);
        }
    }
    free(save);
    *out = lines;
    return count;
}

// utils.js createCanvasText(w, h, text, fontSize, fcolr, bcolr, noBVis, tAlign, ocolr, othick, noSCF)
static unsigned int prefab_sign_texture(const Object *object, const cJSON *raw_obj) {
    const cJSON *raw_text = cJSON_GetObjectItem(raw_obj, "st");
    const char *text = cJSON_IsString(raw_text) && cJSON_GetStringValue(raw_text)[0] ? cJSON_GetStringValue(raw_text) : "Hello World";
    double font = (int) prefab_json_float(cJSON_GetObjectItem(raw_obj, "sf"), 0.0f); // parseInt(sf) || 10
    if (font <= 0.0) font = 10.0;
    const int align = (int) prefab_json_float(cJSON_GetObjectItem(raw_obj, "sa"), 0.0f);
    const cJSON *fg = cJSON_GetObjectItem(raw_obj, "sc"), *bg = cJSON_GetObjectItem(raw_obj, "sb"), *ol = cJSON_GetObjectItem(raw_obj, "so");
    const char *fcolr = cJSON_IsString(fg) && cJSON_GetStringValue(fg)[0] ? cJSON_GetStringValue(fg) : "#000";
    const char *bcolr = cJSON_IsString(bg) && cJSON_GetStringValue(bg)[0] ? cJSON_GetStringValue(bg) : "#fff";
    const char *ocolr = cJSON_IsString(ol) && cJSON_GetStringValue(ol)[0] ? cJSON_GetStringValue(ol) : "#626262";
    double othick = (int) prefab_json_float(cJSON_GetObjectItem(raw_obj, "sot"), 0.0f); // parseInt(sot) || 1
    if (othick <= 0.0) othick = 1.0;
    const cJSON *bv = cJSON_GetObjectItem(raw_obj, "bv");
    const int no_background = bv && !cJSON_IsNull(bv) && !cJSON_IsFalse(bv) && !(cJSON_IsNumber(bv) && cJSON_GetNumberValue(bv) == 0.0) &&
                              !(cJSON_IsString(bv) && !cJSON_GetStringValue(bv)[0]);
    const int scale_font = prefab_json_present(cJSON_GetObjectItem(raw_obj, "nsf")); // noSCF: nsf != null

    const float w = object->scale.x, h = object->scale.z;
    int width, height;
    prefab_canvas_size(w, h, &width, &height);
    if (width <= 0 || height <= 0) return 0;
    if (scale_font) font *= fmin(1024.0 / w, 1024.0 / h);

    char key[2048];
    snprintf(key, sizeof(key), "v7sign:%dx%d:%g:%d:%d:%s|%s|%s|%g|%.1500s", width, height, font, align, no_background, fcolr, bcolr, ocolr, othick, text);
    texture_cache_map_itr cached = vt_get(&g_texture_cache, key);
    if (!vt_is_end(cached)) return cached.data->val;

    Canvas2D *c = prefab_canvas_begin(width, height);
    if (!c) return 0;
    if (!no_background) {
        prefab_canvas_color(c, bcolr, "#ffffff", 0);
        c2d_fill_rect(c, 0.0, 0.0, width, height);
    }
    c2d_set_font(c, font, "GameFont");
    const char *align_name = align == 2 ? "right" : align == 1 ? "center" : "left";
    c2d_set_text_align_name(c, align_name);
    double y = font * 2.0;
    const double m = c2d_measure_text(c, "M");
    const double x = align == 2 ? width - m / 2.0 : align == 1 ? width / 2.0 : m / 2.0;
    const double step = m * 1.2;

    // text.substring(0, signTextLimit - 1).split("\\n") (a literal backslash-n)
    char *copy = strndup(text, 1499);
    if (copy) {
        char *segment = copy;
        for (;;) {
            char *split = strstr(segment, "\\n");
            if (split) *split = 0;
            char **lines = NULL;
            const int count = prefab_canvas_lines(c, segment, width - font, &lines);
            for (int i = 0; i < count; i++) {
                prefab_canvas_text(c, lines[i], fcolr, x, y, NULL, 0.0, ocolr, othick);
                y += step;
                if (y > height) break;
            }
            for (int i = 0; i < count; i++) free(lines[i]);
            free(lines);
            if (!split) break;
            segment = split + 2;
        }
        free(copy);
    }
    return prefab_canvas_end(c, key);
}

// The server's billboard ad on a BILLBOARD (billboard_ad.h): its image (an asset path) when it
// loads, else the text on utils.js createCanvasAD's 2048 x 1024 canvas. 0 = keep the poster.
static unsigned int prefab_billboard_ad_texture(const BillboardAd *ad) {
    if (ad->image[0]) {
        static char failed[NET_BILLBOARD_IMAGE_MAX]; // tried once per path, not once per billboard
        if (strcmp(failed, ad->image)) {
            char *path = concat(assets_path(), ad->image);
            const unsigned int texture = path ? load_texture(path) : 0; // ensure_asset_file + cache
            free(path);
            if (texture) return texture;
            snprintf(failed, sizeof(failed), "%s", ad->image);
        }
    }
    if (!ad->txt[0]) return 0;

    char key[160];
    snprintf(key, sizeof(key), "billboard-ad:%s|%s|%s", ad->txt, ad->txt_col, ad->bg_col);
    texture_cache_map_itr cached = vt_get(&g_texture_cache, key);
    if (!vt_is_end(cached)) return cached.data->val;
    Canvas2D *c = prefab_canvas_begin(BILLBOARD_AD_WIDTH, BILLBOARD_AD_HEIGHT);
    if (!c) return 0;
    billboard_ad_draw(c, ad);
    return prefab_canvas_end(c, key);
}

// ---- models (MapManager.addMesh -> render.js loadMesh)

// loadMesh merges every static instance of a model group (src or variant texture) that is
// queued before the OBJ finishes loading (the whole map) into one mesh with the FIRST
// instance's material: each group renders with the colour / opacity of its first map object.
// Spinning and destructible instances keep their own material.
typedef struct {
    char key[96];
    vec4 color;
} PrefabModelMaterial;

static PrefabModelMaterial *g_prefab_model_materials;
static size_t g_prefab_model_material_count;

static vec4 prefab_model_group_color(const char *key, const vec4 color) {
    for (size_t i = 0; i < g_prefab_model_material_count; i++) {
        if (!strcmp(g_prefab_model_materials[i].key, key)) return g_prefab_model_materials[i].color;
    }
    PrefabModelMaterial *grown = realloc(g_prefab_model_materials, (g_prefab_model_material_count + 1) * sizeof(PrefabModelMaterial));
    if (!grown) return color;
    g_prefab_model_materials = grown;
    PrefabModelMaterial *entry = &g_prefab_model_materials[g_prefab_model_material_count++];
    snprintf(entry->key, sizeof(entry->key), "%s", key);
    entry->color = color;
    return color;
}

// models/<model>.obj with textures/<texture>.png; NULL when the OBJ does not load
static Mesh *prefab_model_mesh(const char *model, const char *texture, const int no_textures) {
    char model_path[160];
    snprintf(model_path, sizeof(model_path), "models/%s.obj", model);
    char *full_model_path = concat(assets_path(), model_path);
    // Krunker keeps the OBJ origin; v7 loadMesh re-centres it (offsetH) in the caller's transform
    Geometry *geometry = full_model_path ? load_obj_model(full_model_path, 0) : NULL;
    free(full_model_path);
    if (!geometry) return NULL;

    BasicMaterial *material = basic_material_init();
    if (!material) return NULL;
    Mesh *mesh = mesh_init(geometry, (Material *) material);
    material->texture = no_textures ? 0 : prefab_load_texture_src(texture);
    return mesh;
}

// map.colors palette: `if (OBJECT.ci != null && ac[OBJECT.ci]) OBJECT.c = ac[OBJECT.ci]`
static int prefab_palette_entry_set(const cJSON *entry) {
    if (cJSON_IsString(entry)) return entry->valuestring[0] != 0;
    if (cJSON_IsNumber(entry)) return cJSON_GetNumberValue(entry) != 0.0;
    return cJSON_IsTrue(entry) || cJSON_IsArray(entry) || cJSON_IsObject(entry);
}

static vec4 prefab_json_color(const cJSON *raw_color, const cJSON *raw_color_idx, const vec4 *colors, const vec4 fallback, int *set) {
    if (set) *set = 0;
    // an index past the palette, or onto an empty entry, keeps the object's own "c"
    if (cJSON_IsNumber(raw_color_idx) && colors && g_prefab_palette) {
        const double index = cJSON_GetNumberValue(raw_color_idx);
        const int count = cJSON_GetArraySize(g_prefab_palette);
        if (index >= 0.0 && index < (double) count && index == (double) (int) index &&
            prefab_palette_entry_set(cJSON_GetArrayItem(g_prefab_palette, (int) index))) {
            if (set) *set = 1;
            return colors[(int) index];
        }
    }

    vec4 color = fallback;
    if (cJSON_IsString(raw_color) && cJSON_GetStringValue(raw_color)[0]) {
        parse_hex_color(cJSON_GetStringValue(raw_color), &color);
        if (set) *set = 1;
    } else if (cJSON_IsNumber(raw_color)) {
        color = hex_to_vec((int) cJSON_GetNumberValue(raw_color));
        if (set) *set = 1;
    }
    return color;
}

// The model and texture sources of a model prefab (MapManager.addMesh): src = "<name>_0"; a
// variant ("tv", limitMM to the variant count) with diffM swaps model and texture to
// "<src>_<id>", without diffM only the texture; noEmis drops the glow; texRange (GIFT) picks
// a random texture suffix 0..texRange when there is no variant.
typedef struct {
    char model[96];
    char texture[96];
    char group[96];
    int glow;
} PrefabModelSrc;

static void prefab_model_src(const PrefabData *data, const Object *object, const int prefab, PrefabModelSrc *out) {
    snprintf(out->model, sizeof(out->model), "%s", data->model ? data->model : data->src);
    snprintf(out->texture, sizeof(out->texture), "%s", out->model);
    out->glow = prefab_has_flag(prefab, PF_EMISS);
    int tv = object->tex_variant;
    if (data->variant_count > 0) {
        if (tv < 0) tv = 0;
        if (tv > data->variant_count - 1) tv = data->variant_count - 1;
    } else {
        tv = 0;
    }
    if (tv && data->variants) {
        const PrefabVariant *variant = &data->variants[tv];
        char base[96];
        snprintf(base, sizeof(base), "%s", out->model);
        if (variant->diff_model) {
            snprintf(out->model, sizeof(out->model), "%s_%d", base, variant->id);
            snprintf(out->texture, sizeof(out->texture), "%s", out->model);
        } else {
            snprintf(out->texture, sizeof(out->texture), "%s_%d", base, variant->id);
        }
        if (variant->no_emissive) out->glow = 0;
    } else if (data->tex_range > 0) {
        const int r = (int) pcg32_boundedrand((uint32_t) data->tex_range + 1u);
        if (r > 0) snprintf(out->texture, sizeof(out->texture), "%s_%d", out->model, r);
    }
    // loadMesh group key: texVar == null ? src : texSrc || src
    snprintf(out->group, sizeof(out->group), "%s|%s", out->model, out->texture);
}

// ---- load-time prefetch (web): the textures and models prefab_init() is about to open,
// collected from the raw objects and downloaded as one parallel batch (asset_prefetch_wait).
// A hint only: whatever this misses is still fetched on demand, extras are just unused, and
// it never draws random numbers (random billboards / texRange are left to prefab_init()).
#ifdef __EMSCRIPTEN__
typedef struct {
    char **paths;
    size_t count;
    size_t capacity;
} PrefabPrefetch;

static void prefab_prefetch_add(PrefabPrefetch *list, const char *format, const char *name) {
    char relative[160];
    snprintf(relative, sizeof(relative), format, name);
    char *path = concat(assets_path(), relative);
    if (!path) return;
    for (size_t i = 0; i < list->count; i++) {
        if (!strcmp(list->paths[i], path)) {
            free(path);
            return;
        }
    }
    if (list->count == list->capacity) {
        const size_t capacity = list->capacity ? list->capacity * 2 : 32;
        char **grown = realloc(list->paths, capacity * sizeof(char *));
        if (!grown) {
            free(path);
            return;
        }
        list->paths = grown;
        list->capacity = capacity;
    }
    list->paths[list->count++] = path;
}

static void prefab_prefetch_object(PrefabPrefetch *list, const cJSON *raw_obj, const ClientRenderSettings *settings) {
    const int prefab = map_raw_object_prefab(raw_obj);
    const PrefabInfo *info = prefab_info(prefab);
    const PrefabData *data = prefab_data(prefab);
    if (!info || !data) return;
    if (!settings->map_details && ((info->roles & PREFAB_R_MAP_DETAIL) || prefab_has_flag(prefab, PF_NODETAIL))) return;

    if (info->geometry == PREFAB_GEO_MODEL && data->model) {
        prefab_prefetch_add(list, "models/%s.obj", data->model);
        if (!settings->no_textures) prefab_prefetch_add(list, "textures/%s.png", data->model);
        if (prefab_has_flag(prefab, PF_EMISS) && !settings->no_textures) prefab_prefetch_add(list, "textures/%s_e.png", data->model);
        return;
    }
    if (prefab == PREFAB_BOOST_PAD) {
        prefab_prefetch_add(list, "models/%s.obj", "pad_0");
        prefab_prefetch_add(list, "textures/%s.png", "pad_0");
        return;
    }
    if (prefab == PREFAB_OBJECTIVE || prefab == PREFAB_DOMINATION_ZONE) {
        prefab_prefetch_add(list, "textures/%s.png", "objective_4");
        return;
    }
    if (settings->no_textures) return;
    if (prefab == PREFAB_LIGHT_CONE) {
        prefab_prefetch_add(list, "textures/%s.png", cJSON_GetObjectItem(raw_obj, "fc") == NULL ? "lightcone_0" : "lightcone_1");
        return;
    }
    if (prefab == PREFAB_LIGHT_BAR) {
        prefab_prefetch_add(list, "textures/%s.png", "lightcone_1");
        return;
    }
    if (prefab == PREFAB_RUNE) {
        prefab_prefetch_add(list, "textures/%s.png", "rune_0");
        return;
    }
    if (info->geometry == PREFAB_GEO_CUBE || info->geometry == PREFAB_GEO_PLANE || info->geometry == PREFAB_GEO_CYLINDER ||
        info->geometry == PREFAB_GEO_SPHERE || info->geometry == PREFAB_GEO_RAMP || info->geometry == PREFAB_GEO_LADDER ||
        info->geometry == PREFAB_GEO_LIQUID || info->geometry == PREFAB_GEO_SPRITE) {
        const cJSON *at = cJSON_GetObjectItem(raw_obj, "at");
        if (at && !cJSON_IsNull(at)) return; // user assets: fetched on demand
        const cJSON *t = cJSON_GetObjectItem(raw_obj, "t");
        const int texture = prefab_texture_resolve(prefab, t && !cJSON_IsNull(t), cJSON_IsNumber(t) ? (int) cJSON_GetNumberValue(t) : -1);
        const cJSON *tv = cJSON_GetObjectItem(raw_obj, "tv");
        char src[96];
        if (prefab_texture_src(texture, cJSON_IsNumber(tv) ? (int) cJSON_GetNumberValue(tv) : 0, src, sizeof(src)) && strcmp(src, "default")) {
            prefab_prefetch_add(list, "textures/%s.png", src);
        }
    }
}

static void prefab_prefetch_map_assets(const cJSON *objects) {
    const ClientRenderSettings *settings = client_render_settings();
    PrefabPrefetch list = {0};
    const cJSON *raw_obj;
    cJSON_ArrayForEach(raw_obj, objects) prefab_prefetch_object(&list, raw_obj, settings);
    // ambientShading planes (ambient.c) load right after the objects
    prefab_prefetch_add(&list, "textures/ambient_%s.png", "0");
    prefab_prefetch_add(&list, "textures/ambient_%s.png", "1");
    asset_prefetch_wait((const char *const *) list.paths, list.count);
    for (size_t i = 0; i < list.count; i++) free(list.paths[i]);
    free(list.paths);
}
#else
static void prefab_prefetch_map_assets(const cJSON *objects) {
    (void) objects; // native files are local: nothing to batch
}
#endif

void prefab_begin_map(const cJSON *objects, const cJSON *colors) {
    free(g_prefab_model_materials);
    g_prefab_model_materials = NULL;
    g_prefab_model_material_count = 0;
    g_prefab_palette = cJSON_IsArray(colors) ? colors : NULL;
    prefab_prefetch_map_assets(objects);
}

// The raw "c" / "ci" colour of a map object (white when absent), for user_asset.c.
vec4 prefab_raw_color(const cJSON *raw_obj, const vec4 *colors) {
    const vec4 white = {1.0f, 1.0f, 1.0f, 1.0f};
    return prefab_json_color(cJSON_GetObjectItem(raw_obj, "c"), cJSON_GetObjectItem(raw_obj, "ci"), colors, white, NULL);
}

// ---- builders

static Mesh *prefab_new_mesh(Geometry *geometry) {
    if (!geometry) return NULL;
    BasicMaterial *material = basic_material_init();
    if (!material) return NULL;
    return mesh_init(geometry, (Material *) material);
}

static int prefab_mesh_visible(const cJSON *raw_obj) {
    const cJSON *v = cJSON_GetObjectItem(raw_obj, "v");
    return !(v && !cJSON_IsNull(v)); // noVis: v != null (v7)
}

// geos.js generateCube via MapManager.addBlock / addGate / addBank / addTrigger / addRopeLadder
// -> render.js addCube: faces, per-face UVs scaled by size / worldUV * (tsm || scale), the
// floor_0 flips, the vertex-colour gradient (getColor(colr, darken) on top, * 0.7 at the bottom
// with "amb"; "ab" (noAmb) colours the whole cube getColor(colr)), rotation XYZ (r0, r1, r2).
static Mesh *prefab_build_cube(Object *object, const cJSON *raw_obj, const PrefabStyle *st, const vec3 size, const int face_mask,
                               const float scale, const int side, const int link_depth_always) {
    Mesh *mesh = prefab_new_mesh(prefab_cube_geo(face_mask));
    if (!mesh) return NULL;
    BasicMaterial *material = (BasicMaterial *) mesh->material;
    const float world_uv = game_constants.world_uv_scale;

    mesh->visible = prefab_mesh_visible(raw_obj);
    mesh->transform.position = object->position;
    mesh->transform.rotation = prefab_object_euler(object);
    mesh->transform.scale = size;

    const int floor_texture = !strcmp(st->src, "floor_0");
    const float m = object->tex_scale_mult != 0.0f ? object->tex_scale_mult : scale;
    material->uv_mode = BASIC_UV_CUBE;
    material->uv_size = m != 0.0f ? (vec3) {size.x / world_uv * m, size.y / world_uv * m, size.z / world_uv * m} : (vec3) {1.0f, 1.0f, 1.0f};
    material->uv_flip_sides = floor_texture && (size.x > size.y || size.z > size.y);
    material->uv_flip_top = floor_texture && size.x > size.z;

    const int no_amb = prefab_json_present(cJSON_GetObjectItem(raw_obj, "ab"));
    const float darken = object->darken != 0.0f ? object->darken : 1.0f;
    material->color = prefab_vertex_color(st, no_amb ? 1.0f : darken);
    material->use_color_gradient = object->ambient_enabled;
    material->color_bottom = object->ambient_enabled ? prefab_vertex_color(st, darken * PREFAB_AMBIENT_MULT) : material->color;

    material->texture = prefab_style_texture(st, 0);
    material->emissive = st->emissive;
    prefab_apply_texture_keys(material, object, raw_obj, 1);
    prefab_apply_transparency(material, object, st, link_depth_always);
    prefab_apply_side(material, side);
    prefab_apply_lighting_controls(material, object);
    return mesh;
}

// Which plane flavour (map.js PLANE / BILLBOARD / RUNE / SPRAY / LIGHT_BAR / SIGN / LIQUID)
typedef enum {
    PLANE_KIND_PLANE,
    PLANE_KIND_BILLBOARD,
    PLANE_KIND_RUNE,
    PLANE_KIND_SPRAY,
    PLANE_KIND_LIGHT_BAR,
    PLANE_KIND_SIGN,
    PLANE_KIND_LIQUID,
} PlaneKind;

// THREE.Sprite for "msp" (mimicSprite) planes and SPRITE: SpriteMaterial (never lit,
// transparent) coloured colr || #FFFFFF, the unit quad scaled to s[0] x s[2] facing the camera.
static void prefab_make_sprite(Mesh *mesh, BasicMaterial *material, const Object *object, const PrefabStyle *st) {
    mesh->geometry = prefab_plane_xy_geo();
    mesh->transform.rotation = (vec3) {0};
    mesh->transform.scale = (vec3) {object->scale.x, object->scale.z, 1.0f};
    material->sprite = 1;
    material->unlit = 1;
    material->uv_mode = BASIC_UV_MESH;
    material->color = st->has_colr ? st->colr : (vec4) {1.0f, 1.0f, 1.0f, st->opacity};
    material->color.w = st->opacity;
    material->use_color_gradient = 0;
    material->emissive = (vec4) {0};
    material->base.transparent = 1;
    material->double_sided = 1;
}

// MapManager.addFixedPlane / addLiquid / addSpray / addLightBar / addCanvas -> render.js
// addFixedPlane: generatePlane(s[2], s[0]) (UVs * s / worldUV * (scaleM || scale), vertex
// colour getColor(colr, darken)), quaternion Rx(r0) Ry(r1) Rz(r2) Rx(-PI/2), scale (s[0], s[2]),
// DoubleSide.
static Mesh *prefab_build_plane(Object *object, const cJSON *raw_obj, PrefabStyle *st, const PlaneKind kind) {
    const int liquid = kind == PLANE_KIND_LIQUID;
    const int tiles_x = liquid ? (int) prefab_json_float(cJSON_GetObjectItem(raw_obj, "tx"), 0.0f) : 1;
    const int tiles_z = liquid ? (int) prefab_json_float(cJSON_GetObjectItem(raw_obj, "tz"), 0.0f) : 1;
    Mesh *mesh = prefab_new_mesh(liquid ? prefab_plane_grid_geo(tiles_x ? tiles_x : 10, tiles_z ? tiles_z : 10) : prefab_plane_xy_geo());
    if (!mesh) return NULL;
    BasicMaterial *material = (BasicMaterial *) mesh->material;
    const float world_uv = game_constants.world_uv_scale;
    const int stretch = prefab_json_present(cJSON_GetObjectItem(raw_obj, "tsr"));

    mesh->visible = prefab_mesh_visible(raw_obj);
    mesh->transform.position = object->position;
    if (kind == PLANE_KIND_RUNE) mesh->transform.position.y += 0.01f;
    mesh->transform.rotation = prefab_object_euler(object);
    mesh->transform.scale = (vec3) {object->scale.x, object->scale.z, 1.0f};

    // scale: PLANE 1 (0 with tsr), LIQUID 2, the others 0 (stretched); tsm (scaleM) wins
    float scale = kind == PLANE_KIND_PLANE ? (stretch ? 0.0f : 1.0f) : liquid ? 2.0f : 0.0f;
    const float m = liquid || kind == PLANE_KIND_SIGN || kind == PLANE_KIND_SPRAY || kind == PLANE_KIND_LIGHT_BAR || object->tex_scale_mult == 0.0f ? scale : object->tex_scale_mult;
    material->uv_mode = BASIC_UV_PLANE;
    material->uv_size = m != 0.0f ? (vec3) {object->scale.x / world_uv * m, object->scale.z / world_uv * m, 1.0f} : (vec3) {1.0f, 1.0f, 1.0f};

    const float darken = kind == PLANE_KIND_PLANE && object->darken != 0.0f ? object->darken : 1.0f;
    material->color = prefab_vertex_color(st, darken);
    material->emissive = st->emissive;
    material->double_sided = 1;

    const int no_textures = client_render_settings()->no_textures;
    unsigned int ad_texture = 0; // the server's billboard ad (BILLBOARD only)
    switch (kind) {
        case PLANE_KIND_BILLBOARD: {
            // bb: isNumber(bb) ? limitMM(parseInt(bb), 1, billboards - 1) : randInt(1, billboards - 1)
            const cJSON *bb = cJSON_GetObjectItem(raw_obj, "bb");
            int id;
            if (cJSON_IsNumber(bb)) {
                id = (int) cJSON_GetNumberValue(bb);
                if (id < 1) id = 1;
                if (id > game_constants.billboard_count) id = game_constants.billboard_count;
            } else {
                id = (int) pcg32_boundedrand((uint32_t) game_constants.billboard_count) + 1;
            }
            snprintf(st->src, sizeof(st->src), "pubs/b_%d", id);
            st->asset = 0;
            // the server's billboard replaces the poster (the random draw above still happens, so
            // the map's other random picks do not move); it survives noTex like a SIGN canvas and
            // shows its own colours (no "col" tint)
            const BillboardAd *ad = billboard_ad_for_object(object);
            ad_texture = ad ? prefab_billboard_ad_texture(ad) : 0;
            if (ad_texture) {
                material->texture = ad_texture;
                material->color.x = material->color.y = material->color.z = 1.0f;
            } else {
                material->texture = prefab_style_texture(st, 0);
            }
            break;
        }
        case PLANE_KIND_RUNE:
            snprintf(st->src, sizeof(st->src), "rune_0");
            material->texture = prefab_style_texture(st, 0);
            break;
        case PLANE_KIND_LIGHT_BAR:
            snprintf(st->src, sizeof(st->src), "lightcone_1");
            material->texture = prefab_style_texture(st, 0);
            break;
        case PLANE_KIND_SIGN:
            // the canvas survives noTex (it is not a "src" texture)
            material->texture = prefab_sign_texture(object, raw_obj);
            material->color = (vec4) {1.0f, 1.0f, 1.0f, 1.0f};
            material->emissive = (vec4) {0};
            break;
        case PLANE_KIND_SPRAY: {
            // MapManager.addSpray: store item spt (2482 when absent or not a spray), opacity *= item opacity
            const cJSON *spt = cJSON_GetObjectItem(raw_obj, "spt");
            int index = cJSON_IsNumber(spt) ? (int) cJSON_GetNumberValue(spt) : 0;
            const ViewerSkin *skin = index > 0 ? viewer_skin(index) : NULL;
            if (!skin || skin->type != 4) skin = viewer_skin(2482);
            snprintf(st->src, sizeof(st->src), "sprays/%d", skin ? skin->id : 0);
            st->asset = 0;
            st->opacity *= skin && skin->opacity > 0.0f ? skin->opacity : 1.0f;
            material->color = prefab_vertex_color(st, 1.0f);
            material->emissive = (vec4) {0};
            material->texture = no_textures ? 0 : prefab_load_texture_src(st->src);
            break;
        }
        default:
            material->texture = prefab_style_texture(st, 0);
            break;
    }

    if (ad_texture) {
        // the whole ad: the map's texture rotation stays, its poster frames / scrolling / offsets do not
        material->texture_rotation = object->tex_rotation * (float) M_PI / 180.0f;
    } else {
        prefab_apply_texture_keys(material, object, raw_obj, kind != PLANE_KIND_SIGN && kind != PLANE_KIND_RUNE);
    }
    prefab_apply_lighting_controls(material, object);

    switch (kind) {
        case PLANE_KIND_LIGHT_BAR:
            // MeshBasicMaterial, AdditiveBlending, depthWrite false, transparent
            material->unlit = 1;
            material->additive_blend = 1;
            material->no_depth_write = 1;
            material->base.transparent = 1;
            break;
        case PLANE_KIND_SPRAY:
            material->no_depth_write = 1;
            material->base.transparent = 1;
            break;
        case PLANE_KIND_SIGN:
            material->base.transparent = 1;
            material->alpha_test = 0.02f;
            break;
        case PLANE_KIND_RUNE:
            // prefabs RUNE alphaTest 0.9. v7 addFixedPlane runs it through parseInt (0.9 -> 0): a v7
            // bug, not reproduced (decisions.md); the same applies to "alt" below.
            material->base.transparent = 1;
            material->alpha_test = 0.9f;
            break;
        case PLANE_KIND_LIQUID: {
            material->base.transparent = st->opacity != 1.0f;
            const float height = prefab_json_float(cJSON_GetObjectItem(raw_obj, "wh"), 0.0f);
            const float speed = prefab_json_float(cJSON_GetObjectItem(raw_obj, "ws"), 0.0f);
            material->wave_height = height ? height : 0.5f;
            material->wave_speed = speed ? speed : 10.0f;
            break;
        }
        default: // PLANE, BILLBOARD
            prefab_apply_transparency(material, object, st, 0);
            if (!prefab_is_trans_tex(st->src) && object->alpha_test > 0.0f) material->alpha_test = object->alpha_test; // "alt" (see RUNE)
            break;
    }

    if (kind == PLANE_KIND_PLANE && prefab_json_float(cJSON_GetObjectItem(raw_obj, "en"), 0.0f) > 0.0f) {
        // noise planes (map_post.c builds the geometry): getColor(colr) flat, getColor(colr, 0.6) on the bumps
        material->color = prefab_vertex_color(st, 1.0f);
        material->use_color_gradient = 1;
        material->color_bottom = prefab_vertex_color(st, 0.6f);
    }

    // mimicSprite: SpriteMaterial instead (PLANE / BILLBOARD / RUNE / LIGHT_BAR / SIGN)
    if (object->sprite && kind != PLANE_KIND_LIQUID && kind != PLANE_KIND_SPRAY) {
        prefab_make_sprite(mesh, material, object, st);
        if (ad_texture) material->color.x = material->color.y = material->color.z = 1.0f; // the ad's own colours
    }
    return mesh;
}

// three.js CylinderGeometry builders: CYLINDER (MapManager.addCylinder -> render.js addCylinder:
// radius min(s[0], s[2]) / 2, rTop / rBot / sides / thetaS / thetaL / open, UV scale
// (scaleM || scale 0) of (radius, height) / worldUV, FrontSide unless open) and LIGHT_CONE
// (addLightCone -> addCone: ConeGeometry(1, 1, sides 12, 1, open, thetaStart PI/4, thetaL),
// scale (s[0] / 2, s[1], s[2] / 2), MeshBasicMaterial, additive, no depth write, DoubleSide).
static Mesh *prefab_build_cylinder(Object *object, const cJSON *raw_obj, PrefabStyle *st, const int cone) {
    if (client_render_settings()->no_textures) return NULL; // addCylinder / addCone: nothing with noTex
    const cJSON *raw = raw_obj;
    int sides = (int) prefab_json_float(cJSON_GetObjectItem(raw, "sds"), 0.0f);
    if (!sides) sides = 12;
    const cJSON *thl = cJSON_GetObjectItem(raw, "thl");
    float theta_length = prefab_json_present(thl) ? prefab_json_float(thl, 0.0f) : 360.0f;
    if (theta_length == 0.0f) theta_length = 0.1f;
    theta_length = fminf(fmaxf(roundf(theta_length * 100.0f) / 100.0f, 0.1f), 360.0f);

    Geometry *geometry;
    const vec3 s = object->scale;
    float radius = fminf(s.x, s.z) / 2.0f;
    if (cone) {
        geometry = prefab_cylinder_geo(0.0f, 1.0f, sides, 1, (float) M_PI / 4.0f, theta_length * (float) M_PI / 180.0f, 0.0f, 0.0f);
    } else {
        const cJSON *rtp = cJSON_GetObjectItem(raw, "rtp"), *rbt = cJSON_GetObjectItem(raw, "rbt"), *ths = cJSON_GetObjectItem(raw, "ths");
        const float r_top = prefab_json_present(rtp) ? fminf(fmaxf(roundf(prefab_json_float(rtp, 0.0f) * 100.0f) / 100.0f, 0.0f), 1.0f) : 1.0f;
        const float r_bot = prefab_json_present(rbt) ? fminf(fmaxf(roundf(prefab_json_float(rbt, 0.0f) * 100.0f) / 100.0f, 0.0f), 1.0f) : 1.0f;
        const float theta_start = fminf(fmaxf(roundf(prefab_json_float(ths, 0.0f) * 100.0f) / 100.0f, 0.0f), 360.0f);
        const int open = prefab_json_present(cJSON_GetObjectItem(raw, "opn"));
        const int stretch = prefab_json_present(cJSON_GetObjectItem(raw, "tsr"));
        const float scale = stretch ? 0.0f : 0.0f; // addCylinder: scale == null ? 0 : scale (tsr: 0)
        const float m = object->tex_scale_mult != 0.0f ? object->tex_scale_mult : scale;
        const float world_uv = game_constants.world_uv_scale;
        geometry = prefab_cylinder_geo(r_top, r_bot, sides, open, theta_start * (float) M_PI / 180.0f, theta_length * (float) M_PI / 180.0f,
                                       m != 0.0f ? radius / world_uv * m : 0.0f, m != 0.0f ? s.y / world_uv * m : 0.0f);
    }
    Mesh *mesh = prefab_new_mesh(geometry);
    if (!mesh) return NULL;
    BasicMaterial *material = (BasicMaterial *) mesh->material;

    mesh->visible = prefab_mesh_visible(raw_obj);
    mesh->transform.position = object->position;
    mesh->transform.rotation = prefab_object_euler(object);
    mesh->transform.scale = cone ? (vec3) {s.x / 2.0f, s.y, s.z / 2.0f} : (vec3) {radius, s.y, radius};

    material->uv_mode = BASIC_UV_MESH;
    material->color = prefab_vertex_color(st, 1.0f);
    prefab_apply_texture_keys(material, object, raw_obj, 1);
    prefab_apply_lighting_controls(material, object);
    if (cone) {
        snprintf(st->src, sizeof(st->src), "%s", cJSON_GetObjectItem(raw_obj, "fc") == NULL ? "lightcone_0" : "lightcone_1");
        st->asset = 0;
        material->texture = prefab_style_texture(st, 0);
        material->emissive = (vec4) {0};
        material->unlit = 1;
        material->additive_blend = 1;
        material->no_depth_write = 1;
        material->double_sided = 1;
        material->base.transparent = 1;
    } else {
        material->texture = prefab_style_texture(st, 0);
        material->emissive = st->emissive;
        prefab_apply_transparency(material, object, st, 0);
        prefab_apply_side(material, prefab_json_present(cJSON_GetObjectItem(raw, "opn")) ? 2 : 0);
    }
    return mesh;
}

// MapManager.addSphere -> render.js addSphere: SphereGeometry(1, 12, 8) scaled s / 2 at y + s[1] / 2
// (the KN unit sphere already spans y 0..1), XYZ rotation, DoubleSide, colour getColor(colr)
static Mesh *prefab_build_sphere(Object *object, const cJSON *raw_obj, const PrefabStyle *st) {
    Mesh *mesh = prefab_new_mesh(prefab_sphere_geo());
    if (!mesh) return NULL;
    BasicMaterial *material = (BasicMaterial *) mesh->material;
    mesh->visible = prefab_mesh_visible(raw_obj);
    mesh->transform.position = object->position;
    mesh->transform.rotation = prefab_object_euler(object);
    mesh->transform.scale = object->scale;
    material->uv_mode = BASIC_UV_MESH;
    material->color = prefab_vertex_color(st, 1.0f);
    material->texture = prefab_style_texture(st, 0);
    material->emissive = st->emissive;
    prefab_apply_texture_keys(material, object, raw_obj, 1);
    prefab_apply_transparency(material, object, st, 0);
    material->double_sided = 1;
    prefab_apply_lighting_controls(material, object);
    return mesh;
}

// MapManager.addRamp -> render.js addRamp: a double-sided plane (generatePlane(length, width)
// UVs * (scaleM || scale 1, 0 with tsr)) rotated rotateY(-PI/2 - d) rotateX(asin(h / slant) -
// PI/2) at half height, or with "ec" (enclose) the extruded triangle of generateRamps scaled
// (w, h, l) (swapped for d 1 / 3) and rotateY(PI - d).
static Mesh *prefab_build_ramp(Object *object, const cJSON *raw_obj, const PrefabStyle *st) {
    const int odd = object->direction % 2;
    const float width = odd ? object->scale.x : object->scale.z;   // p7449
    const float length = odd ? object->scale.z : object->scale.x;  // p7451 * 2
    const float height = object->scale.y;
    const float slant = sqrtf(height * height + length * length);
    const float direction = (float) M_PI * 0.5f * (float) object->direction;
    const float world_uv = game_constants.world_uv_scale;
    const int stretch = prefab_json_present(cJSON_GetObjectItem(raw_obj, "tsr"));
    const float m = object->tex_scale_mult != 0.0f ? object->tex_scale_mult : (stretch ? 0.0f : 1.0f);
    const int enclose = prefab_json_present(cJSON_GetObjectItem(raw_obj, "ec"));

    Mesh *mesh = prefab_new_mesh(enclose ? prefab_ramp_prism_geo(m != 0.0f ? length / 2.0f / world_uv * m : 0.0f, m != 0.0f ? width / world_uv * m : 0.0f)
                                         : prefab_plane_xy_geo());
    if (!mesh) return NULL;
    BasicMaterial *material = (BasicMaterial *) mesh->material;
    mesh->visible = prefab_mesh_visible(raw_obj);
    mesh->transform.position = object->position;
    mesh->transform.position.y += height * 0.5f;
    if (enclose) {
        mesh->transform.rotation = (vec3) {0.0f, (float) M_PI - direction, 0.0f};
        mesh->transform.scale = odd ? (vec3) {object->scale.z, height, object->scale.x} : (vec3) {object->scale.x, height, object->scale.z};
        material->uv_mode = BASIC_UV_MESH;
    } else {
        mesh->transform.rotation_order = ROTATION_ORDER_YXZ;
        mesh->transform.rotation = (vec3) {(slant > 0.0f ? asinf(height / slant) : 0.0f) - (float) M_PI / 2.0f, -(float) M_PI / 2.0f - direction, 0.0f};
        mesh->transform.scale = (vec3) {width, slant, 1.0f};
        material->uv_mode = BASIC_UV_PLANE;
        material->uv_size = m != 0.0f ? (vec3) {width / world_uv * m, length / world_uv * m, 1.0f} : (vec3) {1.0f, 1.0f, 1.0f};
    }
    material->color = prefab_vertex_color(st, 1.0f);
    material->texture = prefab_style_texture(st, 0);
    material->emissive = st->emissive;
    prefab_apply_texture_keys(material, object, raw_obj, 1);
    prefab_apply_transparency(material, object, st, 1);
    material->double_sided = 1;
    prefab_apply_lighting_controls(material, object);
    return mesh;
}

// MapManager.addLadder: two addCube rails and a plane per rung (prefab_ladder_geo)
static Mesh *prefab_build_ladder(Object *object, const cJSON *raw_obj, const PrefabStyle *st) {
    // map.js: scale = tsr == null ? 0.02 : 1 (scaleM "tsm" wins)
    const float uv_scale = object->tex_scale_mult != 0.0f ? object->tex_scale_mult : prefab_json_present(cJSON_GetObjectItem(raw_obj, "tsr")) ? 1.0f : 0.02f;
    const float direction = (float) M_PI * 0.5f * (float) object->direction;
    Mesh *mesh = prefab_new_mesh(prefab_ladder_geo(object->scale.y, direction, uv_scale, st->colr));
    if (!mesh) return NULL;
    BasicMaterial *material = (BasicMaterial *) mesh->material;
    mesh->visible = prefab_mesh_visible(raw_obj);
    mesh->transform.position = object->position; // the direction is in the geometry

    material->uv_mode = BASIC_UV_MESH;
    material->use_vertex_color = 1;
    material->color = (vec4) {1.0f, 1.0f, 1.0f, st->opacity};
    material->texture = prefab_style_texture(st, 0);
    material->emissive = st->emissive;
    prefab_apply_texture_keys(material, object, raw_obj, 1);
    prefab_apply_transparency(material, object, st, 1);
    // v10 MapManager.addPlane sets side = 2 (DoubleSide) on the rungs (the shared option bag, after
    // the two rail addCubes, which stay FrontSide): culled rungs vanished from behind and edge-on.
    // The rails are closed boxes, so drawing their back faces changes nothing while the ladder is
    // opaque; a see-through one keeps single-sided faces (its rails' insides would show).
    material->double_sided = !material->base.transparent;
    prefab_apply_lighting_controls(material, object);
    return mesh;
}

// A model mesh at (x, y, z) with the XYZ rotation of m and uniform scale; when recentre is set
// (loadMesh offsetH / asset), the geometry is shifted so its bounding-box centre is the origin
// and the mesh is placed at p + R * (0, offset_h, 0) (render.js similatePosRot).
static void prefab_place_model(Mesh *mesh, const vec3 p, const float rot[9], const float scale, const int recentre, const float offset_h) {
    vec3 local = {0.0f, 0.0f, 0.0f};
    if (recentre) local.y = offset_h - mesh->geometry->bound_center[1] * scale;
    const vec3 offset = prefab_mat3_apply(rot, local);
    mesh->transform.position = (vec3) {p.x + offset.x, p.y + offset.y, p.z + offset.z};
    mesh->transform.rotation = prefab_euler_from_matrix(rot);
    mesh->transform.scale = (vec3) {scale, scale, scale};
}

static void prefab_model_glow(BasicMaterial *material, const char *emissive_src) {
    if (client_render_settings()->no_textures) return;
    material->emissive = (vec4) {1.0f, 1.0f, 1.0f, 1.0f};
    material->emissive_texture = prefab_load_texture_src(emissive_src);
}

// MapManager.addMesh -> loadMesh
static Mesh *prefab_build_model(Object *object, const cJSON *raw_obj, const PrefabStyle *st) {
    const int prefab = object->prefab;
    const PrefabData *data = prefab_data(prefab);
    if (!data || (!data->model && !data->src)) return NULL;
    const int no_textures = client_render_settings()->no_textures;

    PrefabModelSrc src;
    prefab_model_src(data, object, prefab, &src);
    Mesh *mesh = prefab_model_mesh(src.model, src.texture, no_textures);
    if (!mesh) return NULL;
    BasicMaterial *material = (BasicMaterial *) mesh->material;

    // mSize: limitMM(ms, 0.01, 100).round(2) || prefab scale
    float scale = data->scale;
    const cJSON *ms = cJSON_GetObjectItem(raw_obj, "ms");
    if (prefab_json_float(ms, 0.0f) != 0.0f) scale = fminf(fmaxf(roundf(prefab_json_float(ms, 0.0f) * 100.0f) / 100.0f, 0.01f), 100.0f);

    float rot[9];
    prefab_mat3_euler_xyz(object->rotation, rot);
    prefab_place_model(mesh, object->position, rot, scale, 1, object->scale.y * 0.5f);
    mesh->visible = prefab_mesh_visible(raw_obj);

    // loadMesh colour: colr || #ffffff (no getColor); grouped static instances share the first
    vec4 color = st->has_colr ? st->colr : (vec4) {1.0f, 1.0f, 1.0f, 1.0f};
    color.w = st->opacity;
    const int destructible = prefab_json_float(cJSON_GetObjectItem(raw_obj, "h"), 0.0f) != 0.0f ||
                             prefab_json_float(cJSON_GetObjectItem(raw_obj, "hp"), 0.0f) != 0.0f ||
                             prefab_json_present(cJSON_GetObjectItem(raw_obj, "cdy"));
    if (!destructible && object->spin == 0.0f) color = prefab_model_group_color(src.group, color);
    material->color = color;
    material->base.transparent = prefab_has_flag(prefab, PF_TRANSPARENT) || color.w != 1.0f;
    material->double_sided = prefab_has_flag(prefab, PF_DOUBLESIDE);
    material->texture_srgb = object->tex_encoding == 1;

    if (src.glow) {
        char emissive_src[112];
        snprintf(emissive_src, sizeof(emissive_src), "%s_e", src.model); // getMat: (glowText ? texSrc : src) + "_e"
        prefab_model_glow(material, emissive_src);
    }
    if (prefab == PREFAB_GRASS) {
        // prefabs GRASS: frames 4, frameT 180, alphaTest 0.02
        material->alpha_test = 0.02f;
        TextureAnimation *anim = prefab_tex_anim(object);
        if (anim) {
            anim->frames = 4;
            anim->frame_time = 0.180f;
        }
    }
    prefab_apply_lighting_controls(material, object);
    return mesh;
}

// MapManager.addPad: pad_0 at y + 0.4 (+ s[1] / 2 sideways), scale 3.6 * max(min(w, l) / 10, 0.1)
// in x / z and 3.6 in y, rotation from r (x = r0) then rotateZ(-PI/2) rotateX(PI/2 * (d - 2))
// for the sideways directions 2..5 or rotateX(PI) for d 1 (down)
static Mesh *prefab_build_pad(Object *object, const cJSON *raw_obj) {
    Mesh *mesh = prefab_model_mesh("pad_0", "pad_0", client_render_settings()->no_textures);
    if (!mesh) return NULL;
    BasicMaterial *material = (BasicMaterial *) mesh->material;
    const int d = object->direction;
    const vec3 s = object->scale;
    const float size = d < 2 ? fminf(s.x, s.z) : fminf(s.y, (d & 1) ? s.x : s.z);
    const float k = fmaxf(size / 10.0f, 0.1f);

    float rot[9];
    map_object_rotation(object, rot);
    if (d >= 2) {
        float extra[9];
        prefab_mat3_euler_xyz((vec3) {0.0f, 0.0f, -(float) M_PI / 2.0f}, extra);
        prefab_mat3_mul(rot, extra, rot);
        prefab_mat3_euler_xyz((vec3) {(float) M_PI / 2.0f * (float) (d - 2), 0.0f, 0.0f}, extra);
        prefab_mat3_mul(rot, extra, rot);
    } else if (d == 1) {
        float extra[9];
        prefab_mat3_euler_xyz((vec3) {(float) M_PI, 0.0f, 0.0f}, extra);
        prefab_mat3_mul(rot, extra, rot);
    }
    mesh->transform.position = (vec3) {object->position.x, object->position.y + 0.4f + (d >= 2 ? s.y / 2.0f : 0.0f), object->position.z};
    mesh->transform.rotation = prefab_euler_from_matrix(rot);
    mesh->transform.scale = (vec3) {3.6f * k, 3.6f, 3.6f * k};
    mesh->visible = prefab_mesh_visible(raw_obj);
    material->double_sided = 0;
    prefab_apply_lighting_controls(material, object);
    return mesh;
}

// MapManager.addMysteryBox / addPerkMachine / addKrumMachine: loadMesh without offsetH at
// y + s[1] / 2 (mystery box - 1), scale 5 / 5 / 7, rotation r; the machines glow with their
// "<texture>_e" map. The mystery box starts hidden (switchMysteryBox shows it, zombies WP-H).
static Mesh *prefab_build_machine(Object *object, const cJSON *raw_obj) {
    const int prefab = object->prefab;
    char texture[64];
    const char *model = prefab == PREFAB_MYSTERY_BOX ? "weaponbox_0" : prefab == PREFAB_PERK_MACHINE ? "perkmachine_0" : "krummachine_0";
    if (prefab == PREFAB_PERK_MACHINE && object->zombie_perk > 0) snprintf(texture, sizeof(texture), "perkmachine_0_%d", object->zombie_perk);
    else snprintf(texture, sizeof(texture), "%s", model);
    Mesh *mesh = prefab_model_mesh(model, texture, client_render_settings()->no_textures);
    if (!mesh) return NULL;
    BasicMaterial *material = (BasicMaterial *) mesh->material;
    float rot[9];
    prefab_mat3_euler_xyz(object->rotation, rot);
    const float scale = prefab == PREFAB_KRUM_MACHINE ? 7.0f : 5.0f;
    vec3 p = object->position;
    p.y += object->scale.y * 0.5f - (prefab == PREFAB_MYSTERY_BOX ? 1.0f : 0.0f);
    prefab_place_model(mesh, p, rot, scale, 0, 0.0f);
    mesh->visible = prefab == PREFAB_MYSTERY_BOX ? 0 : prefab_mesh_visible(raw_obj);
    material->double_sided = 0;
    if (prefab != PREFAB_MYSTERY_BOX) {
        char emissive_src[80];
        snprintf(emissive_src, sizeof(emissive_src), "%s_e", texture); // glowText: texSrc + "_e"
        prefab_model_glow(material, emissive_src);
    }
    prefab_apply_lighting_controls(material, object);
    return mesh;
}

// MapManager.addShowcase: a store item's model (mapDet only): src = types[assetType || type] +
// (type ? id : weapon) (+ "_" + mid), the item's texture rules, scale (ms || 1) * (mScl || weapon
// scale || 1), y + s[1] / 2 (charms * 2), rotation r, glow, opacity, side, spin. Sprays, dyes,
// collectibles, wrists, tickets and player cards show nothing. Animated (hasAnim GLTF) items are
// not drawn (documented: no GLTF showcase instancing yet).
static Mesh *prefab_build_showcase(Object *object, const cJSON *raw_obj) {
    const int item_id = (int) prefab_json_float(cJSON_GetObjectItem(raw_obj, "skin"), 0.0f);
    const ViewerSkin *skin = viewer_skin(item_id);
    if (!skin) return NULL;
    const int type = skin->type;
    if (type == 4 || type == 5 || type == 10 || type == 11 || type == 13 || type == 16) return NULL;
    if (skin->flags & VS_HAS_ANIM) return NULL;
    const int type_index = skin->asset_type ? skin->asset_type : type;
    const char *prefix = type_index >= 0 && type_index < g_viewer_type_count ? g_viewer_types[type_index] : g_viewer_types[0];
    char model[128], texture[192];
    const int model_id = type_index ? skin->id : skin->weapon;
    if (skin->mid >= 0) snprintf(model, sizeof(model), "%s%d_%d", prefix, model_id, skin->mid);
    else snprintf(model, sizeof(model), "%s%d", prefix, model_id);
    if (skin->mid >= 0) {
        snprintf(texture, sizeof(texture), "%s%d_%d", prefix, skin->weapon, skin->mid_t >= 0 ? skin->mid_t : skin->mid); // getReModel
    } else if (type == 3) { // melee
        if ((skin->flags & VS_PAT) && skin->tex) snprintf(texture, sizeof(texture), "%s", skin->tex);
        else if (skin->tex) snprintf(texture, sizeof(texture), "melee/melee_%d_%s", skin->id, skin->tex);
        else snprintf(texture, sizeof(texture), "melee/melee_%d", skin->id);
    } else if (type) {
        if (skin->tex) snprintf(texture, sizeof(texture), "%s%d_%s", prefix, skin->id, skin->tex);
        else snprintf(texture, sizeof(texture), "%s", model);
    } else {
        if (skin->tex) snprintf(texture, sizeof(texture), "%s", skin->tex);
        else snprintf(texture, sizeof(texture), "weapons/skins/weapon_%d_%d", skin->weapon, skin->id);
    }
    Mesh *mesh = prefab_model_mesh(model, texture, client_render_settings()->no_textures);
    if (!mesh) return NULL;
    BasicMaterial *material = (BasicMaterial *) mesh->material;

    const Weapon *weapon = skin->weapon >= 1 && skin->weapon <= rules_krunker_weapon_count() ? rules_weapon(skin->weapon - 1) : NULL;
    float ms = prefab_json_float(cJSON_GetObjectItem(raw_obj, "ms"), 0.0f);
    ms = ms != 0.0f ? fminf(fmaxf(ms, 0.01f), 100.0f) : 1.0f;
    const float item_scale = skin->m_scl != 0.0f ? skin->m_scl : weapon && weapon->scale != 0.0f ? weapon->scale : 1.0f;
    float rot[9];
    prefab_mat3_euler_xyz(object->rotation, rot);
    vec3 p = object->position;
    p.y += object->scale.y * 0.5f * (type == 12 ? 2.0f : 1.0f);
    prefab_place_model(mesh, p, rot, ms * item_scale, 0, 0.0f);
    mesh->visible = prefab_mesh_visible(raw_obj);

    const float opacity = skin->opac != 0.0f ? skin->opac : 1.0f;
    material->color = (vec4) {1.0f, 1.0f, 1.0f, opacity};
    material->base.transparent = skin->opac != 0.0f || (skin->flags & VS_TRANS);
    material->double_sided = skin->side == 2;
    material->cull_front = skin->side == 1;
    if (skin->flags & (VS_GLOW | VS_SAME_GLOW)) {
        char glow[200];
        snprintf(glow, sizeof(glow), "%s%s", texture, (skin->flags & VS_SAME_GLOW) ? "" : "_e");
        prefab_model_glow(material, glow);
    }
    prefab_apply_lighting_controls(material, object);
    return mesh;
}

// MapManager.addObjective / addDominationZone: addCube(x, y, z, s[0], 5, s[2]) without top and
// bottom, objective_4 stretched, MeshBasicMaterial, transparent, no depth write, DoubleSide,
// hidden until the mode shows it (objectives glow #ffd542, a MeshBasicMaterial ignores it).
static Mesh *prefab_build_objective(Object *object) {
    Mesh *mesh = prefab_new_mesh(prefab_cube_geo(0x33));
    if (!mesh) return NULL;
    BasicMaterial *material = (BasicMaterial *) mesh->material;
    mesh->visible = 0;
    mesh->transform.position = object->position;
    mesh->transform.scale = (vec3) {object->scale.x, 5.0f, object->scale.z};
    material->uv_mode = BASIC_UV_CUBE;
    material->texture = prefab_load_texture_src("objective_4"); // forceTex: kept with noTex
    material->base.transparent = 1;
    material->double_sided = 1;
    material->no_depth_write = 1;
    material->unlit = 1;
    return mesh;
}

// MapManager.updatePendingLeader: the leaderboard canvas on an addFixedPlane (or, with
// mimicSprite, an addSprite): transparent, depth writes, alphaTest 0.02, DoubleSide, the
// v7 plane rotation, "ts" scrolling. map_post.c owns the mesh (map_post_leaderboard_rows).
Mesh *prefab_canvas_mesh(Object *object, const cJSON *raw_obj, const unsigned int texture) {
    Mesh *mesh = prefab_new_mesh(prefab_plane_xy_geo());
    if (!mesh) return NULL;
    BasicMaterial *material = (BasicMaterial *) mesh->material;
    mesh->visible = prefab_mesh_visible(raw_obj);
    mesh->transform.position = object->position;
    mesh->transform.rotation = prefab_object_euler(object);
    mesh->transform.scale = (vec3) {object->scale.x, object->scale.z, 1.0f};
    material->uv_mode = BASIC_UV_PLANE;
    material->texture = texture;
    material->base.transparent = 1;
    material->alpha_test = 0.02f;
    material->double_sided = 1;
    prefab_apply_texture_keys(material, object, raw_obj, 0);
    prefab_apply_lighting_controls(material, object);
    if (object->sprite) {
        PrefabStyle st;
        prefab_style_init(&st, object, NULL, raw_obj);
        st.has_colr = 0;
        st.opacity = 1.0f;
        prefab_make_sprite(mesh, material, object, &st);
    }
    return mesh;
}

// "weapons/<src>" with the weapon's own texture (no altSkin: KN's weapon table has none), front
// faces only; the scale and pose are the caller's. NULL without a model.
Mesh *prefab_weapon_mesh(const Weapon *weapon, const int no_texture) {
    if (!weapon || !weapon->src) return NULL;
    char model[96];
    snprintf(model, sizeof(model), "weapons/%s", weapon->src);
    Mesh *mesh = prefab_model_mesh(model, model, no_texture || client_render_settings()->no_textures);
    if (!mesh) return NULL;
    ((BasicMaterial *) mesh->material)->double_sided = 0;
    if (weapon->tint) { // KrunkNative weapons (the Tool Gun)
        ((BasicMaterial *) mesh->material)->color = (vec4) {(float) ((weapon->tint >> 16) & 255) / 255.0f,
                                                            (float) ((weapon->tint >> 8) & 255) / 255.0f, (float) (weapon->tint & 255) / 255.0f, 1.0f};
    }
    return mesh;
}

// MapManager.addWeapon -> genWeaponMeshData(weapons[id]): "weapons/<src>" with its own texture
// (noTex "ntx": the default texture), loadMesh scale weapon.scale * (equipment ? 2 : 1). The
// pose (y + pickupOff, rotateY / X / Z) is the caller's (map_post.c). NULL for an unknown id.
// Weapon transparency / glow flags (seeThrough, transp) are not in KN's weapon table (documented).
Mesh *prefab_pickup_mesh(const int weapon_id, const int no_texture, float *scale) {
    if (weapon_id < 0 || weapon_id >= WEAPON_TABLE_COUNT || !rules_weapon(weapon_id)) return NULL;
    const Weapon *weapon = rules_weapon(weapon_id);
    const NettspendWeapon kn_kind = nettspend_weapon_kind(weapon_id); // Nettspend: the cup / cash stack, x2 like the hose
    if (kn_kind) {
        Mesh *mesh = kn_kind == NETTSPEND_WEAPON_DRANK ? nettspend_cup_mesh() : nettspend_cash_mesh();
        if (mesh && scale) *scale = 2.0f; // the pose (rim up) is map_post.c's, like every pickup's
        return mesh;
    }
    if (weapon->stream && !weapon->src) { // KrunkNative Piss: no model, the hose (genHose's mustard default colour) x2
        Mesh *hose = player_hose_mesh(weapon->stream, weapon->stream->color);
        if (hose && scale) *scale = 2.0f;
        return hose;
    }
    Mesh *mesh = prefab_weapon_mesh(weapon, no_texture);
    if (mesh && scale) *scale = (weapon->scale != 0.0f ? weapon->scale : 1.0f) * (weapon->equipment ? 2.0f : 1.0f);
    return mesh;
}

// render.js loadMesh({src, texSrc}) outside the map builder: models/<model>.obj with
// textures/<texture>.png (the noTex setting drops the texture). The caller owns the mesh.
Mesh *prefab_asset_mesh(const char *model, const char *texture) {
    return model ? prefab_model_mesh(model, texture ? texture : model, client_render_settings()->no_textures) : NULL;
}

unsigned int prefab_asset_texture(const char *src) {
    return src ? prefab_load_texture_src(src) : 0;
}

// Player builds (shared/src/build.c, Object.dynamic): v7 draws them through ShaderMesh.addBuild
// (instanced, Lambert, DoubleSide): grain_0 stretched over every face, the build object's colour,
// no ambient gradient. Stairs (custMod addRamp) are drawn as the ramp plane instead of v7's
// 2-unit slab (documented). KrunkNative draws every build in one batch mesh (map_post.c, like v7's
// one instanced draw): prefab_build_vertices bakes a build into world-space vertices with the
// texture coordinates the Krunker cube / plane UV modes of basic.frag give a unit-size build
// (uv_size 1, no flips) and its colour as the vertex colour; prefab_build_batch_mesh is the
// material they share. Build.c frees Objects without knowing about scenes, so builds never keep a
// mesh in Object.mesh.

// The pose the build's own mesh had: a unit cube scaled to the build, or the ramp plane (1)
static int prefab_build_pose(const Object *object, MeshTransform *t) {
    memset(t, 0, sizeof(*t));
    t->position = object->position;
    if (object->prefab == PREFAB_RAMP) {
        const int odd = object->direction % 2;
        const float width = odd ? object->scale.x : object->scale.z, length = odd ? object->scale.z : object->scale.x;
        const float height = object->scale.y, slant = sqrtf(height * height + length * length);
        t->position.y += height * 0.5f;
        t->rotation_order = ROTATION_ORDER_YXZ;
        t->rotation = (vec3) {(slant > 0.0f ? asinf(height / slant) : 0.0f) - (float) M_PI / 2.0f,
                              -(float) M_PI / 2.0f - (float) M_PI * 0.5f * (float) object->direction, 0.0f};
        t->scale = (vec3) {width, slant, 1.0f};
        return 1;
    }
    t->scale = object->scale;
    return 0;
}

// basic.frag krunker_cube_uv with uv_size 1 and no flips, for cube face f (0 +Z, 1 -Z, 2 -X, 3 +X,
// 4 top, 5 bottom) at unit-cube point p
static vec2 prefab_build_cube_uv(const int f, const vec3 p) {
    static const float faces[6][9] = { // center, axis_u, axis_v
        {0.0f, 0.5f, 0.5f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f},
        {0.0f, 0.5f, -0.5f, -1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f},
        {-0.5f, 0.5f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f},
        {0.5f, 0.5f, 0.0f, 0.0f, 0.0f, -1.0f, 0.0f, 1.0f, 0.0f},
        {0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, -1.0f},
        {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f},
    };
    const float *c = faces[f];
    const float dx = p.x - c[0], dy = p.y - c[1], dz = p.z - c[2];
    return (vec2) {0.5f + dx * c[3] + dy * c[4] + dz * c[5], dx * c[6] + dy * c[7] + dz * c[8] + 0.5f};
}

void prefab_build_vertices(const Object *object, const int shown, float out[PREFAB_BUILD_VERTICES * PREFAB_BUILD_FLOATS]) {
    static const vec3 cube[PREFAB_BUILD_VERTICES] = { // prefab_cube_geo's vertex order (face = index / 4)
        {-0.5f, 0.0f, 0.5f}, {0.5f, 1.0f, 0.5f}, {-0.5f, 1.0f, 0.5f}, {0.5f, 0.0f, 0.5f},
        {0.5f, 0.0f, -0.5f}, {-0.5f, 1.0f, -0.5f}, {0.5f, 1.0f, -0.5f}, {-0.5f, 0.0f, -0.5f},
        {-0.5f, 0.0f, -0.5f}, {-0.5f, 1.0f, 0.5f}, {-0.5f, 1.0f, -0.5f}, {-0.5f, 0.0f, 0.5f},
        {0.5f, 0.0f, 0.5f}, {0.5f, 1.0f, -0.5f}, {0.5f, 1.0f, 0.5f}, {0.5f, 0.0f, -0.5f},
        {-0.5f, 1.0f, 0.5f}, {0.5f, 1.0f, -0.5f}, {-0.5f, 1.0f, -0.5f}, {0.5f, 1.0f, 0.5f},
        {-0.5f, 0.0f, -0.5f}, {0.5f, 0.0f, 0.5f}, {-0.5f, 0.0f, 0.5f}, {0.5f, 0.0f, -0.5f},
    };
    static const vec3 plane[4] = {{-0.5f, -0.5f, 0.0f}, {0.5f, 0.5f, 0.0f}, {-0.5f, 0.5f, 0.0f}, {0.5f, -0.5f, 0.0f}}; // prefab_plane_xy_geo
    memset(out, 0, sizeof(float) * PREFAB_BUILD_VERTICES * PREFAB_BUILD_FLOATS);
    if (!shown) return; // every vertex at the origin: zero-area triangles, nothing drawn
    const int index = object->build_index >= 0 && object->build_index < WORLD_BUILD_COUNT ? object->build_index : 0;
    const int rgb = g_build_objects[index].color;
    const float color[3] = {(float) ((rgb >> 16) & 255) / 255.0f, (float) ((rgb >> 8) & 255) / 255.0f, (float) (rgb & 255) / 255.0f};
    Mesh pose;
    memset(&pose, 0, sizeof(pose));
    const int ramp = prefab_build_pose(object, &pose.transform);
    mesh_update_transform_matrix(&pose);
    const float *m = pose.transform_matrix; // row-major
    const int count = ramp ? 4 : PREFAB_BUILD_VERTICES;
    for (int i = 0; i < PREFAB_BUILD_VERTICES; i++) {
        const int k = i < count ? i : 0; // a ramp's 20 spare vertices sit on its first one (degenerate)
        const vec3 p = ramp ? plane[k] : cube[k];
        float *v = out + i * PREFAB_BUILD_FLOATS;
        v[0] = m[0] * p.x + m[1] * p.y + m[2] * p.z + m[3];
        v[1] = m[4] * p.x + m[5] * p.y + m[6] * p.z + m[7];
        v[2] = m[8] * p.x + m[9] * p.y + m[10] * p.z + m[11];
        // basic.frag uv_mode 2 (plane: 0.5 + x, y + 0.5) / 1 (krunker_cube_uv) at uv_size 1
        const vec2 uv = ramp ? (vec2) {0.5f + p.x, p.y + 0.5f} : prefab_build_cube_uv(k / 4, p);
        v[3] = uv.x;
        v[4] = uv.y;
        memcpy(v + 5, color, sizeof(color));
    }
}

Mesh *prefab_build_batch_mesh(Geometry *geometry) {
    Mesh *mesh = prefab_new_mesh(geometry);
    if (!mesh) return NULL;
    BasicMaterial *material = (BasicMaterial *) mesh->material;
    material->uv_mode = BASIC_UV_MESH;
    material->uv_size = (vec3) {1.0f, 1.0f, 1.0f};
    material->color = (vec4) {1.0f, 1.0f, 1.0f, 1.0f}; // times the vertex colour: the build object's colour
    material->color_bottom = material->color;
    material->use_vertex_color = 1;
    material->texture = client_render_settings()->no_textures ? 0 : prefab_load_texture_src("grain_0");
    material->double_sided = 1;
    return mesh;
}

Mesh *prefab_init(Object *object, const vec4 *colors, const cJSON *raw_obj) {
    const int prefab = object->prefab;
    const PrefabInfo *info = prefab_info(prefab);
    if (!info || (info->roles & PREFAB_R_NO_OBJECT)) return NULL;

    // noTex / mapDet are restart settings: read once when the map is built
    const ClientRenderSettings *settings = client_render_settings();
    if (!settings->map_details && ((info->roles & PREFAB_R_MAP_DETAIL) || prefab_has_flag(prefab, PF_NODETAIL))) return NULL;

    PrefabStyle st;
    prefab_style_init(&st, object, colors, raw_obj);
    const int faces = prefab_face_mask(cJSON_GetObjectItem(raw_obj, "f"));
    const int stretch = prefab_json_present(cJSON_GetObjectItem(raw_obj, "tsr"));

    switch (prefab) {
        case PREFAB_CUBE:
        case PREFAB_GATE:
        case PREFAB_DEPOSIT_BOX:
            // addBlock / addGate / addBank: scale == null ? 1 : scale (tsr: 0); rside only on CUBE
            return prefab_build_cube(object, raw_obj, &st, object->scale, faces, stretch ? 0.0f : 1.0f,
                                     prefab == PREFAB_CUBE && object->render_side >= 0 ? object->render_side : 0, 0);
        case PREFAB_TRIGGER: {
            const cJSON *aid = cJSON_GetObjectItem(raw_obj, "aid");
            if (aid && !cJSON_IsNull(aid)) return user_asset_object_mesh(object, raw_obj, colors);
            return prefab_build_cube(object, raw_obj, &st, object->scale, faces, stretch ? 0.0f : 1.0f,
                                     object->render_side >= 0 ? object->render_side : 0, 0);
        }
        case PREFAB_ROPE_LADDER: {
            const float w = fminf(object->scale.x, object->scale.z);
            return prefab_build_cube(object, raw_obj, &st, (vec3) {w, object->scale.y, w}, 0x3f, stretch ? 0.0f : 1.0f, 0, 0);
        }
        case PREFAB_PLANE: return prefab_build_plane(object, raw_obj, &st, PLANE_KIND_PLANE);
        case PREFAB_BILLBOARD: return prefab_build_plane(object, raw_obj, &st, PLANE_KIND_BILLBOARD);
        case PREFAB_RUNE: return prefab_build_plane(object, raw_obj, &st, PLANE_KIND_RUNE);
        case PREFAB_SPRAY: return prefab_build_plane(object, raw_obj, &st, PLANE_KIND_SPRAY);
        case PREFAB_LIGHT_BAR: return prefab_build_plane(object, raw_obj, &st, PLANE_KIND_LIGHT_BAR);
        case PREFAB_SIGN: return prefab_build_plane(object, raw_obj, &st, PLANE_KIND_SIGN);
        case PREFAB_LIQUID: return prefab_build_plane(object, raw_obj, &st, PLANE_KIND_LIQUID);
        case PREFAB_SPRITE: {
            // MapManager.addSprite: SpriteMaterial, the texture keys, colr || #FFFFFF
            Mesh *mesh = prefab_new_mesh(prefab_plane_xy_geo());
            if (!mesh) return NULL;
            BasicMaterial *material = (BasicMaterial *) mesh->material;
            mesh->visible = prefab_mesh_visible(raw_obj);
            mesh->transform.position = object->position;
            material->texture = prefab_style_texture(&st, 0);
            prefab_apply_texture_keys(material, object, raw_obj, 1);
            prefab_apply_lighting_controls(material, object);
            prefab_make_sprite(mesh, material, object, &st);
            return mesh;
        }
        case PREFAB_CYLINDER: return prefab_build_cylinder(object, raw_obj, &st, 0);
        case PREFAB_LIGHT_CONE: return prefab_build_cylinder(object, raw_obj, &st, 1);
        case PREFAB_SPHERE: return prefab_build_sphere(object, raw_obj, &st);
        case PREFAB_RAMP: return prefab_build_ramp(object, raw_obj, &st);
        case PREFAB_LADDER: return prefab_build_ladder(object, raw_obj, &st);
        case PREFAB_BOOST_PAD: return prefab_build_pad(object, raw_obj);
        case PREFAB_MYSTERY_BOX:
        case PREFAB_PERK_MACHINE:
        case PREFAB_KRUM_MACHINE: return prefab_build_machine(object, raw_obj);
        case PREFAB_OBJECTIVE:
        case PREFAB_DOMINATION_ZONE: return prefab_build_objective(object);
        case PREFAB_CUSTOM_ASSET: return user_asset_object_mesh(object, raw_obj, colors);
        case PREFAB_SHOWCASE: return settings->map_details ? prefab_build_showcase(object, raw_obj) : NULL;
        default:
            break;
    }
    if (info->geometry == PREFAB_GEO_MODEL) return prefab_build_model(object, raw_obj, &st);
    return NULL; // tools, zones, lights (map_post.c), canvases without data, special builders elsewhere
}
