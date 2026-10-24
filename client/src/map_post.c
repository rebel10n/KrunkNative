// Map render work that needs every object first (map.js generate() runs it after the object
// loop) or every frame: the ambient-occlusion planes (ambient.c), the noise planes
// (MapManager.addNoisePlanes -> geos.js generatePlane with `noise`), the v7 light objects
// (MapManager.addPointLight / addSpotLight / addDirectionalLight / addRectAreaLight -> the
// basic.frag light list), spinning meshes (render.js movMeshes, "spn"), the canvas render
// distance (renderVisD 400: signs, leaderboards), leaderboard canvases (updatePendingLeader)
// and GLTF user assets (user_asset.c). Owned here: the noise-plane geometries, the light
// handles, the spin / distance lists and the leaderboard meshes.
#include <gl.h>
#include <map_post.h>
#include <shadow.h> // map edits: the casters follow the edited objects
#include <ambient.h>
#include <user_asset.h>
#include <terrain_mesh.h>
#include <world_events.h>
#include <prefabs.h>
#include <render_settings.h>
#include <canvas2d.h>
#include <weapons.h>
#include <nettspend.h> // the Drank's cup / the Money Spread's cash stack as pickups
#include <math.h>
#include <pcg_basic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ruleset.h>

#define NOISE_MARGIN 5.0 // 94.js addPlane: K.margin = 5

typedef struct {
    Mesh *mesh;
    Geometry *original;
    Geometry *noise;
    unsigned int vbo;
} NoisePlane;

static NoisePlane *g_noise_planes;
static size_t g_noise_plane_count;

static double post_json_number(const cJSON *json) {
    if (cJSON_IsNumber(json)) return cJSON_GetNumberValue(json);
    if (cJSON_IsString(json)) return atof(cJSON_GetStringValue(json));
    return 0.0;
}

static double post_random(void) {
    return (double) pcg32_random() / 4294967296.0; // Math.random(): [0, 1)
}

// 31.js generatePlane(E = s[2] / 2, F = s[0] / 2, {noise, pinEdges, margin 5, tilesX, tilesZ,
// objects}, H = x, I = y, J = z): inner grid vertices above the footprint (+ margin) of an
// object whose base is at most 0.1 above the plane and whose top is more than `noise`
// above it are raised by random * noise + 1 (getColor(colr, 0.6), see basic.frag), the
// rest stay at 0; faces whose three vertices all stay at 0 are dropped. The test uses the
// unrotated plane (world z = J - v * 2E, world x = H + u * 2F) like the JS.
static Geometry *post_noise_geometry(const Object *object, const double noise, const AmbientBox *boxes, const size_t box_count, unsigned int *vbo) {
    const double sx = object->scale.x;
    const double sz = object->scale.z;
    const double e = sz / 2.0;
    const double f = sx / 2.0;
    // v7 MapManager.addFixedPlane: tilesX = round(l / 5), tilesZ = round(w / 5) with
    // w = s[0] / 2, l = s[2] / 2 (v3.1.1 had them the other way round)
    int tiles_x = (int) floor(e / 5.0 + 0.5);
    int tiles_z = (int) floor(f / 5.0 + 0.5);
    if (tiles_x < 1) tiles_x = 1;
    if (tiles_z < 1) tiles_z = 1;
    if (tiles_x > 512) tiles_x = 512;
    if (tiles_z > 512) tiles_z = 512;

    const int columns = tiles_x + 1;
    const int vertex_count = columns * (tiles_z + 1);
    vertex *vertices = calloc((size_t) vertex_count, sizeof(vertex));
    unsigned int *indices = calloc((size_t) tiles_x * (size_t) tiles_z * 6, sizeof(unsigned int));

    if (!vertices || !indices) {
        free(vertices);
        free(indices);
        return NULL;
    }

    // the raw positions as doubles (map.js limit(p).round(2)): with floats, a plane at -0.1
    // misses every object standing at 0 (-0.1f + 0.1 < 0) (map-G fix)
    const cJSON *raw_p = cJSON_GetObjectItem(object->raw, "p");
    const double plane_x = round(fmin(fmax(post_json_number(cJSON_GetArrayItem(raw_p, 0)), -500000.0), 500000.0) * 100.0) / 100.0;
    const double plane_y = round(fmin(fmax(post_json_number(cJSON_GetArrayItem(raw_p, 1)), -500000.0), 500000.0) * 100.0) / 100.0;
    const double plane_z = round(fmin(fmax(post_json_number(cJSON_GetArrayItem(raw_p, 2)), -500000.0), 500000.0) * 100.0) / 100.0;

    for (int iy = 0; iy <= tiles_z; iy++) {
        for (int ix = 0; ix <= tiles_x; ix++) {
            // THREE.PlaneGeometry: x = ix * segW - 0.5, y = -(iy * segH - 0.5)
            const double p = ix * (1.0 / tiles_x) - 0.5;
            const double q = -(iy * (1.0 / tiles_z) - 0.5);
            double z = 0.0;

            if (p != -0.5 && p != 0.5 && q != -0.5 && q != 0.5) {
                const double world_z = plane_z + -q * e * 2.0;
                const double world_x = plane_x + p * f * 2.0;

                for (size_t r = 0; r < box_count; r++) {
                    const AmbientBox *box = &boxes[r];
                    if (box->center[1] - box->half[1] <= plane_y + 0.1 && box->center[1] + box->half[1] > plane_y + noise &&
                        world_z >= box->center[2] - (box->half[2] + NOISE_MARGIN) && world_z <= box->center[2] + (box->half[2] + NOISE_MARGIN) &&
                        world_x >= box->center[0] - (box->half[0] + NOISE_MARGIN) && world_x <= box->center[0] + (box->half[0] + NOISE_MARGIN)) {
                        z = post_random() * noise + 1.0;
                        break;
                    }
                }
            }

            vertices[iy * columns + ix] = (vertex) {(float) p, (float) q, (float) z, (float) (p + 0.5), (float) (q + 0.5)};
        }
    }

    int index_count = 0;
    for (int iy = 0; iy < tiles_z; iy++) {
        for (int ix = 0; ix < tiles_x; ix++) {
            const unsigned int a = (unsigned int) (ix + columns * iy);
            const unsigned int b = (unsigned int) (ix + columns * (iy + 1));
            const unsigned int c = (unsigned int) (ix + 1 + columns * (iy + 1));
            const unsigned int d = (unsigned int) (ix + 1 + columns * iy);
            const unsigned int faces[2][3] = {{a, b, d}, {b, c, d}};

            for (int k = 0; k < 2; k++) {
                if (vertices[faces[k][0]].position.z <= 0.0f && vertices[faces[k][1]].position.z <= 0.0f && vertices[faces[k][2]].position.z <= 0.0f) continue;
                memcpy(indices + index_count, faces[k], sizeof(faces[k]));
                index_count += 3;
            }
        }
    }

    Geometry *geometry = calloc(1, sizeof(Geometry));
    if (geometry) {
        glGenVertexArrays(1, &geometry->vao);
        glGenBuffers(1, &geometry->ebo);
        glGenBuffers(1, vbo);

        glBindVertexArray(geometry->vao);
        glBindBuffer(GL_ARRAY_BUFFER, *vbo);
        glBufferData(GL_ARRAY_BUFFER, (long) vertex_count * (long) sizeof(vertex), vertices, GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, geometry->ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, (long) (index_count ? index_count : 1) * (long) sizeof(unsigned int), indices, GL_STATIC_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), NULL);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *) (3 * sizeof(float)));
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
        geometry->index_count = index_count;
    }

    free(vertices);
    free(indices);
    return geometry;
}

static void post_noise_planes(const Map *map) {
    const cJSON *objects = cJSON_GetObjectItem(map->raw_data, "objects");
    if (!cJSON_IsArray(objects)) return;

    AmbientBox *boxes = NULL;
    size_t box_count = 0;
    size_t object_index = 0;

    const cJSON *raw;
    cJSON_ArrayForEach(raw, objects) {
        if (!map_raw_object_kept(raw)) continue; // the same raw objects map.c skips
        if (object_index >= map->object_count) break;

        Object *object = map->objects[object_index++];
        const double noise = post_json_number(cJSON_GetObjectItem(raw, "en")); // noise: T.en || 0
        if (object->prefab != PREFAB_PLANE || noise <= 0.0 || !object->mesh) continue;

        if (!boxes) boxes = ambient_map_boxes(map, &box_count); // addNoisePlanes: objects = this.objects

        NoisePlane *planes = realloc(g_noise_planes, (g_noise_plane_count + 1) * sizeof(NoisePlane));
        if (!planes) break;
        g_noise_planes = planes;

        NoisePlane *plane = &g_noise_planes[g_noise_plane_count];
        memset(plane, 0, sizeof(*plane));
        plane->noise = post_noise_geometry(object, noise, boxes, box_count, &plane->vbo);
        if (!plane->noise) continue;

        plane->mesh = (Mesh *) object->mesh; // shared Object keeps the client mesh as void *
        plane->original = plane->mesh->geometry;
        plane->mesh->geometry = plane->noise;
        g_noise_plane_count++;
    }

    free(boxes);
}

// ---- lights: three.js lights in the forward light list (client.h BasicLight)
static int g_light_handles[BASIC_MAX_LIGHTS];
static int g_light_handle_count;

static float post_clamp(const float v, const float lo, const float hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

// limit(v, positionRange 500000).round(2)
static float post_position(const cJSON *json) {
    return roundf(post_clamp((float) post_json_number(json), -500000.0f, 500000.0f) * 100.0f) / 100.0f;
}

static void post_lights(const Map *map) {
    const ClientRenderSettings *settings = client_render_settings();
    const cJSON *raw_colors = cJSON_GetObjectItem(map->raw_data, "colors");
    const cJSON *objects = cJSON_GetObjectItem(map->raw_data, "objects");
    size_t object_index = 0;
    // MapManager.addPointLight & co: only with mapDet; render.js addPointLight / addSpotLight
    // need lighting >= 2, addDirectionalLight / addRectAreaLight lighting 3 (Physical)
    if (!settings->map_details || settings->lighting < 2) return;

    const cJSON *raw;
    cJSON_ArrayForEach(raw, objects) {
        if (!map_raw_object_kept(raw)) continue; // the same raw objects map.c skips
        if (object_index >= map->object_count) break;
        const Object *object = map->objects[object_index++];
        const int prefab = object->prefab;
        if (prefab != PREFAB_POINT_LIGHT && prefab != PREFAB_SPOT_LIGHT && prefab != PREFAB_DIRECTIONAL_LIGHT && prefab != PREFAB_RECTAREA_LIGHT) continue;
        if ((prefab == PREFAB_DIRECTIONAL_LIGHT || prefab == PREFAB_RECTAREA_LIGHT) && settings->lighting != 3) continue;
        if (g_light_handle_count >= BASIC_MAX_LIGHTS) break;

        // colr || "#f2f8fc" (map.js resolves "ci" into "c" first)
        vec4 color = {0.949f, 0.973f, 0.988f, 1.0f};
        const cJSON *ci = cJSON_GetObjectItem(raw, "ci");
        const cJSON *c = cJSON_GetObjectItem(raw, "c");
        const cJSON *palette = cJSON_IsNumber(ci) ? cJSON_GetArrayItem(raw_colors, (int) cJSON_GetNumberValue(ci)) : NULL;
        if (cJSON_IsString(palette) && cJSON_GetStringValue(palette)[0]) parse_hex_color(cJSON_GetStringValue(palette), &color);
        else if (cJSON_IsString(c) && cJSON_GetStringValue(c)[0]) parse_hex_color(cJSON_GetStringValue(c), &color);
        else if (cJSON_IsNumber(c) && cJSON_GetNumberValue(c) != 0.0) color = hex_to_vec((int) cJSON_GetNumberValue(c));

        const double li = post_json_number(cJSON_GetObjectItem(raw, "li"));
        BasicLight light = {
            .visible = 1,
            .color = {color.x, color.y, color.z},
            .intensity = post_clamp(li ? (float) li : 1.0f, 0.0f, 15.0f), // parseFloat(li) || 1, limitMM 0..15
            .position = object->position,
        };
        const cJSON *lt = cJSON_GetObjectItem(raw, "lt");
        const int has_target = cJSON_GetArraySize(lt) >= 3;
        if (has_target) light.target = (vec3) {post_position(cJSON_GetArrayItem(lt, 0)), post_position(cJSON_GetArrayItem(lt, 1)), post_position(cJSON_GetArrayItem(lt, 2))};

        if (prefab == PREFAB_POINT_LIGHT || prefab == PREFAB_SPOT_LIGHT) {
            const cJSON *lds = cJSON_GetObjectItem(raw, "lds");
            const double ld = post_json_number(cJSON_GetObjectItem(raw, "ld"));
            // point: parseFloat(lds) || 0; spot: lds == null ? 50 : parseFloat(lds) || 0; limitMM 0..1000
            float distance = (float) post_json_number(lds);
            if (prefab == PREFAB_SPOT_LIGHT && (!lds || cJSON_IsNull(lds))) distance = 50.0f;
            light.distance = post_clamp(distance, 0.0f, 1000.0f);
            light.decay = post_clamp(ld ? (float) ld : 1.0f, 0.1f, 2.0f);
        }
        switch (prefab) {
            case PREFAB_POINT_LIGHT:
                light.type = BASIC_LIGHT_POINT;
                light.position.y += 2.5f; // addPointLight: y += 5 / 2
                break;
            case PREFAB_SPOT_LIGHT: {
                if (!has_target) continue; // map.js reads lt[0] (a map without it fails to load in v7)
                light.type = BASIC_LIGHT_SPOT;
                light.position.y += 2.0f;
                const double la = post_json_number(cJSON_GetObjectItem(raw, "la"));
                const double lp = post_json_number(cJSON_GetObjectItem(raw, "lp"));
                const int angle = (int) la; // parseInt(la) || 45, limitMM 1..90, toRad
                light.angle = post_clamp(angle ? (float) angle : 45.0f, 1.0f, 90.0f) * (float) M_PI / 180.0f;
                light.penumbra = post_clamp((float) lp, 0.0f, 1.0f);
                break;
            }
            case PREFAB_DIRECTIONAL_LIGHT:
                if (!has_target) continue;
                light.type = BASIC_LIGHT_DIRECTIONAL;
                light.position.y += 2.0f;
                break;
            default: { // RECTAREA_LIGHT: addRectAreaLight(x, y, z, s[0] / 2, s[1] / 2, opts, r[2], r[0], r[1])
                light.type = BASIC_LIGHT_RECT;
                light.width = object->scale.x;
                light.height = object->scale.y;
                light.position.y += object->scale.y / 2.0f; // y += s[1] / 2
                light.rotation = (vec3) {object->rotation.x, object->rotation.z, object->rotation.y}; // rotateY(r2) rotateX(r0) rotateZ(r1)
                break;
            }
        }
        const int handle = basic_lights_add(&light);
        if (handle) g_light_handles[g_light_handle_count++] = handle;
    }
}

static void post_lights_unload(void) {
    for (int i = 0; i < g_light_handle_count; i++) basic_lights_remove(g_light_handles[i]);
    g_light_handle_count = 0;
}

// ---- spinning meshes (render.js movMeshes: mesh.rotateY / rotateX(rAnim * delta) while the
// textureAnim setting is on; rAnim = spn * config.diskSpin, axis "x" with spa 1 on models / assets)
typedef struct {
    Mesh *mesh;
    float base[9];   // the builder rotation (row-major)
    float rate;      // radians per ms
    int axis_x;
    float angle;
} SpinMesh;

static SpinMesh *g_spins;
static size_t g_spin_count;

// ---- canvas render distance (render.js rendDisObjs: visible within renderVisD of the camera,
// re-checked every 500 ms)
typedef struct {
    Mesh *mesh;
    int visible;     // the visibility the builder gave it (noVis)
} DistanceMesh;

static DistanceMesh *g_distance_meshes;
static size_t g_distance_count;
static float g_distance_timer;

#define POST_CANVAS_VIEW_DISTANCE 400.0f
#define POST_DISK_SPIN 0.0002f

static void post_mat3_xyz(const vec3 r, float m[9]) {
    const float cx = cosf(r.x), sx = sinf(r.x), cy = cosf(r.y), sy = sinf(r.y), cz = cosf(r.z), sz = sinf(r.z);
    const float out[9] = {
        cy * cz, -cy * sz, sy,
        cx * sz + sx * sy * cz, cx * cz - sx * sy * sz, -sx * cy,
        sx * sz - cx * sy * cz, sx * cz + cx * sy * sz, cx * cy,
    };
    memcpy(m, out, sizeof(out));
}

static void post_track_meshes(const Map *map) {
    for (size_t i = 0; i < map->object_count; i++) {
        Object *object = map->objects[i];
        Mesh *mesh = object->mesh;
        if (!mesh) continue;
        if (object->prefab == PREFAB_SIGN) {
            DistanceMesh *grown = realloc(g_distance_meshes, (g_distance_count + 1) * sizeof(DistanceMesh));
            if (grown) {
                g_distance_meshes = grown;
                g_distance_meshes[g_distance_count++] = (DistanceMesh) {mesh, mesh->visible};
            }
        }
        const float spin = fmaxf(fminf(object->spin, 20.0f), -20.0f);
        const int spinner = object->prefab == PREFAB_CYLINDER || object->prefab == PREFAB_CUSTOM_ASSET || object->prefab == PREFAB_TRIGGER || object->prefab == PREFAB_SHOWCASE ||
                            prefab_info(object->prefab)->geometry == PREFAB_GEO_MODEL;
        if (spin == 0.0f || !spinner || mesh->transform.rotation_order != ROTATION_ORDER_INTRINSIC) continue;
        SpinMesh *grown = realloc(g_spins, (g_spin_count + 1) * sizeof(SpinMesh));
        if (!grown) continue;
        g_spins = grown;
        SpinMesh *s = &g_spins[g_spin_count++];
        memset(s, 0, sizeof(*s));
        s->mesh = mesh;
        post_mat3_xyz(mesh->transform.rotation, s->base);
        s->rate = spin * POST_DISK_SPIN;
        s->axis_x = object->prefab != PREFAB_CYLINDER && object->spin_axis == 1;
    }
}

// ---- an open-addressing index from an Object pointer to its entry in one of the arrays below
// (builds, spawned objects), so a frame looks each object up in O(1) instead of scanning the
// array per object (O(n^2) with a thousand builds). A slot holds entry index + 1 (0 empty); the
// arrays swap-remove, so a removal marks the index dirty and the next lookup rebuilds it.
typedef struct {
    uint32_t *slots;
    size_t mask;   // capacity - 1 (a power of two), 0 = no table yet
    int dirty;
} PostIndex;

static size_t post_index_hash(const void *key, const size_t mask) {
    uint64_t h = (uint64_t) (uintptr_t) key;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    return (size_t) h & mask;
}

static void post_index_free(PostIndex *index) {
    free(index->slots);
    *index = (PostIndex) {0};
}

static void post_index_put(PostIndex *index, const void *key, const size_t entry) {
    size_t h = post_index_hash(key, index->mask);
    while (index->slots[h]) h = (h + 1) & index->mask;
    index->slots[h] = (uint32_t) (entry + 1);
}

// Entry `count - 1` (just appended, under `key`) joins the index, or the next lookup rebuilds it
static void post_index_added(PostIndex *index, const void *key, const size_t count) {
    if (!index->dirty && index->mask && count * 2 <= index->mask + 1) post_index_put(index, key, count - 1);
    else index->dirty = 1;
}

// Rebuilt for `count` entries (keys read through key_of) when dirty or more than half full with one
// more: 0 when out of memory (callers fall back to a scan).
static int post_index_ready(PostIndex *index, const size_t count, const void *(*key_of)(size_t)) {
    if (!index->dirty && index->mask && (count + 1) * 2 <= index->mask + 1) return 1;
    size_t cap = 64;
    while (cap < (count + 1) * 2) cap *= 2;
    if (cap != index->mask + 1 || !index->slots) {
        uint32_t *slots = malloc(cap * sizeof(uint32_t));
        if (!slots) {
            post_index_free(index);
            return 0;
        }
        free(index->slots);
        index->slots = slots;
        index->mask = cap - 1;
    }
    memset(index->slots, 0, cap * sizeof(uint32_t));
    for (size_t i = 0; i < count; i++) post_index_put(index, key_of(i), i);
    index->dirty = 0;
    return 1;
}

// ---- player builds (shared/src/build.c), drawn in one batch: one mesh whose vertex buffer holds a
// slot of PREFAB_BUILD_VERTICES world-space vertices per build (prefab_build_vertices), one draw call
// for them all (v7 draws its builds as one instanced ShaderMesh). Entry i of g_builds owns slot i;
// entries are keyed by the Object (build.c frees Objects on removeBuilds / replacement without a
// scene) plus its build index and position. A new build appends a slot, a build that goes moves the
// last slot into its place, a build destroyed (or back) rewrites its slot: each frame uploads only
// the slots that changed (one glBufferSubData over their range), so a placement never rebuilds the
// whole batch. The index buffer repeats prefab_cube_geo's 36 indices per slot.
typedef struct {
    const Object *object;
    int build_index;
    vec3 position;
    int shown;
    int seen;
} BuildMesh;

#define BUILD_SLOT_FLOATS (PREFAB_BUILD_VERTICES * PREFAB_BUILD_FLOATS)
#define BUILD_SLOT_INDICES 36

static BuildMesh *g_builds;
static size_t g_build_count;
static Scene *g_build_scene;
static PostIndex g_build_index;

static struct {
    Geometry geometry;  // vao + ebo (owned here), index_count = 36 per build
    unsigned int vbo;
    Mesh *mesh;         // in g_build_scene while it exists
    float *vertices;    // CPU copy of every slot
    size_t capacity;    // slots the GL buffers hold
    size_t dirty_lo, dirty_hi; // slots to upload [lo, hi)
    int realloc;        // the GL buffers must be (re)created at `capacity`
} g_batch;

static const void *post_build_key(const size_t i) { return g_builds[i].object; }

static void post_builds_clear(void) {
    if (g_batch.mesh) {
        if (g_build_scene) scene_remove_mesh(g_build_scene, g_batch.mesh);
        mesh_fini(g_batch.mesh);
    }
    if (g_batch.geometry.vao) glDeleteVertexArrays(1, &g_batch.geometry.vao);
    if (g_batch.geometry.ebo) glDeleteBuffers(1, &g_batch.geometry.ebo);
    if (g_batch.vbo) glDeleteBuffers(1, &g_batch.vbo);
    free(g_batch.vertices);
    memset(&g_batch, 0, sizeof(g_batch));
    free(g_builds);
    g_builds = NULL;
    g_build_count = 0;
    g_build_scene = NULL;
    post_index_free(&g_build_index);
}

static int post_build_matches(const BuildMesh *b, const Object *object) {
    return b->object == object && b->build_index == object->build_index && b->position.x == object->position.x &&
           b->position.y == object->position.y && b->position.z == object->position.z;
}

// The entry of a build: same Object, build index and position (a freed Object's address can come
// back for another build); NULL = none
static BuildMesh *post_build_find(const Object *object) {
    if (post_index_ready(&g_build_index, g_build_count, post_build_key)) {
        for (size_t h = post_index_hash(object, g_build_index.mask); g_build_index.slots[h]; h = (h + 1) & g_build_index.mask) {
            BuildMesh *b = &g_builds[g_build_index.slots[h] - 1];
            if (post_build_matches(b, object)) return b;
        }
        return NULL;
    }
    for (size_t k = 0; k < g_build_count; k++) {
        if (post_build_matches(&g_builds[k], object)) return &g_builds[k];
    }
    return NULL;
}

static void post_batch_dirty(const size_t slot) {
    if (g_batch.dirty_lo >= g_batch.dirty_hi) {
        g_batch.dirty_lo = slot;
        g_batch.dirty_hi = slot + 1;
    } else {
        if (slot < g_batch.dirty_lo) g_batch.dirty_lo = slot;
        if (slot + 1 > g_batch.dirty_hi) g_batch.dirty_hi = slot + 1;
    }
}

// Room for `count` slots in the CPU copy (doubling); 0 when out of memory
static int post_batch_reserve(const size_t count) {
    if (count <= g_batch.capacity) return 1;
    size_t capacity = g_batch.capacity ? g_batch.capacity : 64;
    while (capacity < count) capacity *= 2;
    float *grown = realloc(g_batch.vertices, capacity * BUILD_SLOT_FLOATS * sizeof(float));
    if (!grown) return 0;
    g_batch.vertices = grown;
    g_batch.capacity = capacity;
    g_batch.realloc = 1;
    return 1;
}

static void post_batch_write(const size_t slot, const Object *object, const int shown) {
    prefab_build_vertices(object, shown, g_batch.vertices + slot * BUILD_SLOT_FLOATS);
    post_batch_dirty(slot);
}

// The frame's changes to the GL buffers (new buffers after a growth, else the changed range), and
// the draw's index count
static void post_batch_upload(Scene *scene) {
    if (!g_build_count && !g_batch.mesh) return;
    if (!g_batch.geometry.vao) {
        glGenVertexArrays(1, &g_batch.geometry.vao);
        glGenBuffers(1, &g_batch.geometry.ebo);
        glGenBuffers(1, &g_batch.vbo);
        glBindVertexArray(g_batch.geometry.vao);
        glBindBuffer(GL_ARRAY_BUFFER, g_batch.vbo);
        const GLsizei stride = PREFAB_BUILD_FLOATS * sizeof(float);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, NULL);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, (void *) (3 * sizeof(float)));
        glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, stride, (void *) (5 * sizeof(float)));
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
        glEnableVertexAttribArray(2);
        g_batch.realloc = 1;
    }
    if (!g_batch.mesh) {
        g_batch.mesh = prefab_build_batch_mesh(&g_batch.geometry); // bound_radius 0: never frustum-culled
        if (!g_batch.mesh) return;
        scene_add_mesh(scene, g_batch.mesh);
    }
    glBindVertexArray(g_batch.geometry.vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_batch.vbo);
    if (g_batch.realloc) {
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr) (g_batch.capacity * BUILD_SLOT_FLOATS * sizeof(float)), g_batch.vertices, GL_DYNAMIC_DRAW);
        static const unsigned int pattern[BUILD_SLOT_INDICES] = {
            0, 1, 2, 0, 3, 1, 4, 5, 6, 4, 7, 5, 8, 9, 10, 8, 11, 9, 12, 13, 14, 12, 15, 13, 16, 17, 18, 16, 19, 17, 20, 21, 22, 20, 23, 21,
        };
        unsigned int *indices = malloc(g_batch.capacity * BUILD_SLOT_INDICES * sizeof(unsigned int));
        if (indices) {
            for (size_t s = 0; s < g_batch.capacity; s++) {
                for (int k = 0; k < BUILD_SLOT_INDICES; k++) indices[s * BUILD_SLOT_INDICES + k] = (unsigned int) (s * PREFAB_BUILD_VERTICES) + pattern[k];
            }
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_batch.geometry.ebo);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr) (g_batch.capacity * BUILD_SLOT_INDICES * sizeof(unsigned int)), indices, GL_STATIC_DRAW);
            free(indices);
        }
        g_batch.realloc = 0;
    } else if (g_batch.dirty_lo < g_batch.dirty_hi) {
        const size_t bytes = BUILD_SLOT_FLOATS * sizeof(float);
        glBufferSubData(GL_ARRAY_BUFFER, (GLintptr) (g_batch.dirty_lo * bytes), (GLsizeiptr) ((g_batch.dirty_hi - g_batch.dirty_lo) * bytes),
                        g_batch.vertices + g_batch.dirty_lo * BUILD_SLOT_FLOATS);
    }
    glBindVertexArray(0); // nothing else edits this VAO's element buffer
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    g_batch.dirty_lo = g_batch.dirty_hi = 0;
    g_batch.geometry.index_count = (int) (g_build_count * BUILD_SLOT_INDICES);
    g_batch.mesh->visible = g_build_count > 0;
}

// Every frame: a slot for each build (a new one when a build is new or was replaced at its uid),
// filled while the build stands; the slots of builds that are gone leave the batch. Linear in the
// map's objects (the index above), whatever the number of builds.
static void post_builds_frame(Scene *scene, const Map *map) {
    if (!scene || !map) return;
    if (g_build_scene && g_build_scene != scene) post_builds_clear();
    g_build_scene = scene;
    for (size_t i = 0; i < g_build_count; i++) g_builds[i].seen = 0;
    for (size_t i = 0; i < map->object_count; i++) {
        const Object *object = map->objects[i];
        if (!object->dynamic) continue;
        const int shown = object->active && !object->destroyed;
        BuildMesh *entry = post_build_find(object);
        if (!entry) {
            BuildMesh *grown = realloc(g_builds, (g_build_count + 1) * sizeof(BuildMesh));
            if (!grown) continue;
            g_builds = grown;
            if (!post_batch_reserve(g_build_count + 1)) continue;
            entry = &g_builds[g_build_count++];
            *entry = (BuildMesh) {object, object->build_index, object->position, shown, 0};
            post_index_added(&g_build_index, object, g_build_count);
            post_batch_write(g_build_count - 1, object, shown);
        } else if (entry->shown != shown) {
            entry->shown = shown;
            post_batch_write((size_t) (entry - g_builds), object, shown);
        }
        entry->seen = 1;
    }
    for (size_t k = 0; k < g_build_count;) {
        if (g_builds[k].seen) {
            k++;
            continue;
        }
        const size_t last = --g_build_count;
        if (k != last) {
            g_builds[k] = g_builds[last];
            memcpy(g_batch.vertices + k * BUILD_SLOT_FLOATS, g_batch.vertices + last * BUILD_SLOT_FLOATS, BUILD_SLOT_FLOATS * sizeof(float));
            post_batch_dirty(k);
        }
        g_build_index.dirty = 1;
    }
    post_batch_upload(scene);
}

int map_post_build_meshes(int *visible) {
    int shown = 0;
    for (size_t i = 0; i < g_build_count; i++) shown += g_builds[i].shown ? 1 : 0;
    if (visible) *visible = shown;
    return (int) g_build_count;
}

int map_post_build_draws(void) {
    return g_batch.mesh && g_batch.mesh->visible && g_batch.geometry.index_count > 0 ? 1 : 0;
}

// ---- weapon pickups (MapManager.addWeapon mesh, game.resetPickups / updatePickup): one mesh per
// WEAPON_PICKUP, rebuilt when the weapon it holds changes, shown while the pickup is active and
// holds a weapon, at its (moved) position + pickupOff 1, rotateY(r2) rotateX(r0 - PI/2)
// rotateZ(r1) (equipment: rotateX(r0) rotateZ(r1 - PI/2)), the state from map-C's
// game_pickup_state (client mirror of "inat" / "pkups"). v10.0.3's genWeaponMeshData sets noFog on the
// pickup model: it is the only weapon material with noFog (the player's viewmodel and third-person
// weapons are fogged, see main.c's viewmodel pass).
typedef struct {
    int uid;
    const Object *object;
    int weapon;
    Mesh *mesh;
} PickupMesh;

static PickupMesh *g_pickups;
static size_t g_pickup_count;
static Scene *g_pickup_scene;

static void post_pickups_load(Scene *scene, const Map *map) {
    g_pickup_scene = scene;
    for (size_t i = 0; i < map->object_count; i++) {
        const Object *object = map->objects[i];
        if (object->prefab != PREFAB_WEAPON_PICKUP) continue;
        PickupMesh *grown = realloc(g_pickups, (g_pickup_count + 1) * sizeof(PickupMesh));
        if (!grown) break;
        g_pickups = grown;
        g_pickups[g_pickup_count++] = (PickupMesh) {(int) i, object, -2, NULL};
    }
}

static void post_pickups_unload(void) {
    for (size_t i = 0; i < g_pickup_count; i++) {
        if (!g_pickups[i].mesh) continue;
        if (g_pickup_scene) scene_remove_mesh(g_pickup_scene, g_pickups[i].mesh);
        mesh_fini(g_pickups[i].mesh);
    }
    free(g_pickups);
    g_pickups = NULL;
    g_pickup_count = 0;
    g_pickup_scene = NULL;
}

static void post_pickups_frame(const Game *game) {
    if (!game || !g_pickup_scene) return;
    for (size_t i = 0; i < g_pickup_count; i++) {
        PickupMesh *p = &g_pickups[i];
        GamePickup state;
        if (!game_pickup_state(game, p->uid, &state)) continue;
        if (state.weapon != p->weapon) {
            if (p->mesh) {
                scene_remove_mesh(g_pickup_scene, p->mesh);
                mesh_fini(p->mesh);
                p->mesh = NULL;
            }
            p->weapon = state.weapon;
            float scale = 1.0f;
            const cJSON *ntx = p->object->raw ? cJSON_GetObjectItem(p->object->raw, "ntx") : NULL;
            p->mesh = state.weapon >= 0 ? prefab_pickup_mesh(state.weapon, ntx && !cJSON_IsNull(ntx), &scale) : NULL;
            if (p->mesh) {
                const int equipment = state.weapon < rules_krunker_weapon_count() && rules_weapon(state.weapon) && rules_weapon(state.weapon)->equipment;
                const vec3 r = p->object->rotation;
                p->mesh->transform.rotation_order = ROTATION_ORDER_YXZ;
                p->mesh->transform.rotation = (vec3) {r.x - (equipment ? 0.0f : (float) M_PI / 2.0f), r.z, r.y - (equipment ? (float) M_PI / 2.0f : 0.0f)};
                // Nettspend (nettspend.h): the cup's rim / the cash stack's top is the local -z, which the
                // weapon pose above would point down; turned over so they stand on the pickup's spot
                if (nettspend_weapon_kind(state.weapon)) p->mesh->transform.rotation.x += (float) M_PI;
                p->mesh->transform.scale = (vec3) {scale, scale, scale};
                p->mesh->no_shadow_receive = 0;
                // genWeaponMeshData (deob-10.0.3.js:184768-184783): shadowsR, noShadow and noFog: pickups are never fogged
                if (p->mesh->material && p->mesh->material->vtable == &basic_material_vtable) ((BasicMaterial *) p->mesh->material)->fog_enabled = 0;
                scene_add_mesh(g_pickup_scene, p->mesh);
            }
        }
        if (!p->mesh) continue;
        p->mesh->transform.position = (vec3) {state.position.x, state.position.y + 1.0f, state.position.z}; // config.pickupOff
        // Defuse's planted bomb is an inactive pickup that stays in sight: updatePickup shows a pickup that holds
        // a weapon, active or not (`(!bomb || mode.site) && pickup != null`, deob-10.0.3.js:218972)
        const Weapon *held = state.weapon >= 0 && state.weapon < rules_krunker_weapon_count() ? rules_weapon(state.weapon) : NULL;
        const int planted = held && held->bomb && game->mode && game->mode->config.site;
        p->mesh->visible = (state.active || planted) && state.weapon >= 0;
    }
}

// ---- KrunkNative Object Manager: objects spawned at run time (shared/src/object_manager.c). Their
// meshes come from map_object_from_json like the map's own; they join the scene here (the map's
// were added by client_load_map), show while active, and leave it when the object is freed
// (map_post_object_freed, from map_object_free). A spawned WEAPON_PICKUP joins the pickup meshes.
typedef struct {
    const Object *object;
    int in_scene; // its mesh was added to g_spawned_scene here
} SpawnedMesh;

static SpawnedMesh *g_spawned;
static size_t g_spawned_count;
static Scene *g_spawned_scene;

static PostIndex g_spawned_index; // object -> g_spawned entry (PostIndex above)

static const void *post_spawned_key(const size_t i) { return g_spawned[i].object; }

static SpawnedMesh *post_spawned_find(const Object *object) {
    if (post_index_ready(&g_spawned_index, g_spawned_count, post_spawned_key)) {
        for (size_t h = post_index_hash(object, g_spawned_index.mask); g_spawned_index.slots[h]; h = (h + 1) & g_spawned_index.mask) {
            SpawnedMesh *entry = &g_spawned[g_spawned_index.slots[h] - 1];
            if (entry->object == object) return entry;
        }
        return NULL;
    }
    for (size_t i = 0; i < g_spawned_count; i++) {
        if (g_spawned[i].object == object) return &g_spawned[i];
    }
    return NULL;
}


// Object Manager map edits changed what stands in the map: the shadow casters and the light
// objects are rebuilt on the next frame (map_post_frame)
static int g_map_edits_dirty;

typedef struct {
    const Object *object; // a detached map object
    int visible;          // its mesh's visibility before
} DetachedMesh;
static DetachedMesh *g_detached;
static size_t g_detached_count;

static void post_spawned_frame(Scene *scene, const Map *map) {
    if (!scene || !map) return;
    g_spawned_scene = scene;
    for (size_t i = 0; i < map->object_count; i++) {
        const Object *object = map->objects[i];
        if (!object->spawned && !object->map_edit) continue; // (map_edit: a map object's stand-in)
        SpawnedMesh *entry = post_spawned_find(object);
        if (!entry) {
            SpawnedMesh *grown = realloc(g_spawned, (g_spawned_count + 1) * sizeof(SpawnedMesh));
            if (!grown) continue;
            g_spawned = grown;
            entry = &g_spawned[g_spawned_count++];
            *entry = (SpawnedMesh) {object, 0};
            post_index_added(&g_spawned_index, object, g_spawned_count);
            if (object->map_edit) g_map_edits_dirty = 1;
            if (object->mesh) {
                scene_add_mesh(scene, object->mesh);
                entry->in_scene = 1;
            }
            if (object->prefab == PREFAB_WEAPON_PICKUP && g_pickup_scene) {
                PickupMesh *pickups = realloc(g_pickups, (g_pickup_count + 1) * sizeof(PickupMesh));
                if (pickups) {
                    g_pickups = pickups;
                    g_pickups[g_pickup_count++] = (PickupMesh) {(int) i, object, -2, NULL};
                }
            }
        }
        if (object->mesh) ((Mesh *) object->mesh)->visible = object->active && !object->no_vis;
    }
}

int map_post_spawned_meshes(void) {
    int shown = 0;
    for (size_t i = 0; i < g_spawned_count; i++) {
        const Mesh *mesh = g_spawned[i].object->mesh;
        shown += g_spawned[i].in_scene && mesh && mesh->visible;
    }
    return shown;
}

void map_post_object_freed(const Object *object) {
    if (object->map_edit) g_map_edits_dirty = 1;
    for (size_t i = 0; i < g_spawned_count; i++) {
        if (g_spawned[i].object != object) continue;
        if (g_spawned[i].in_scene && g_spawned_scene && object->mesh) scene_remove_mesh(g_spawned_scene, object->mesh);
        g_spawned[i] = g_spawned[--g_spawned_count];
        g_spawned_index.dirty = 1;
        break;
    }
    for (size_t i = 0; i < g_pickup_count; i++) {
        if (g_pickups[i].object != object) continue;
        if (g_pickups[i].mesh) {
            if (g_pickup_scene) scene_remove_mesh(g_pickup_scene, g_pickups[i].mesh);
            mesh_fini(g_pickups[i].mesh);
        }
        g_pickups[i] = g_pickups[--g_pickup_count];
        break;
    }
}

static void post_pickup_forget(const Object *object) {
    for (size_t i = 0; i < g_pickup_count; i++) {
        if (g_pickups[i].object != object) continue;
        if (g_pickups[i].mesh) {
            if (g_pickup_scene) scene_remove_mesh(g_pickup_scene, g_pickups[i].mesh);
            mesh_fini(g_pickups[i].mesh);
        }
        g_pickups[i] = g_pickups[--g_pickup_count];
        return;
    }
}

void map_post_object_detach(const Object *object, const int uid) {
    (void) uid;
    g_map_edits_dirty = 1;
    Mesh *mesh = object->mesh;
    DetachedMesh *grown = realloc(g_detached, (g_detached_count + 1) * sizeof(DetachedMesh));
    if (grown) {
        g_detached = grown;
        g_detached[g_detached_count++] = (DetachedMesh) {object, mesh ? mesh->visible : 0};
    }
    if (mesh) mesh->visible = 0;
    for (size_t i = 0; mesh && i < g_distance_count; i++) { // a sign's render distance would show it again
        if (g_distance_meshes[i].mesh == mesh) g_distance_meshes[i].visible = 0;
    }
    post_pickup_forget(object); // the stand-in's pickup takes the uid's model
}

void map_post_object_attach(const Object *object, const int uid) {
    g_map_edits_dirty = 1;
    for (size_t i = 0; i < g_detached_count; i++) {
        if (g_detached[i].object != object) continue;
        if (object->mesh) ((Mesh *) object->mesh)->visible = g_detached[i].visible;
        for (size_t k = 0; object->mesh && k < g_distance_count; k++) {
            if (g_distance_meshes[k].mesh == object->mesh) g_distance_meshes[k].visible = g_detached[i].visible;
        }
        g_detached[i] = g_detached[--g_detached_count];
        break;
    }
    if (object->prefab == PREFAB_WEAPON_PICKUP && g_pickup_scene) {
        PickupMesh *pickups = realloc(g_pickups, (g_pickup_count + 1) * sizeof(PickupMesh));
        if (pickups) {
            g_pickups = pickups;
            g_pickups[g_pickup_count++] = (PickupMesh) {uid, object, -2, NULL};
        }
    }
}

// Spawned objects that exist when a map loads were added by client_load_map: track them as in the
// scene already. map_post_unload only forgets them (client_unload_map removes every object mesh).
static void post_spawned_load(Scene *scene, const Map *map) {
    g_spawned_scene = scene;
    for (size_t i = 0; map && i < map->object_count; i++) {
        const Object *object = map->objects[i];
        if (!object->spawned && !object->map_edit) continue;
        SpawnedMesh *grown = realloc(g_spawned, (g_spawned_count + 1) * sizeof(SpawnedMesh));
        if (!grown) break;
        g_spawned = grown;
        g_spawned[g_spawned_count++] = (SpawnedMesh) {object, object->mesh != NULL};
        post_index_added(&g_spawned_index, object, g_spawned_count);
    }
}

static void post_spawned_unload(void) {
    free(g_spawned);
    g_spawned = NULL;
    g_spawned_count = 0;
    post_index_free(&g_spawned_index);
    g_spawned_scene = NULL;
    free(g_detached); // (client_unload_map restored the map edits first)
    g_detached = NULL;
    g_detached_count = 0;
    g_map_edits_dirty = 0;
}

// "ana" on a map object (world_events.c): Game.playAnim(obj, clip, 1) on its GLTF user asset
static void post_object_anim(Client *client, Object *object, const char *clip, const int raw) {
    (void) client;
    (void) raw;
    if (object && clip) user_asset_play_clip(object->mesh, clip, 1);
}

void map_post_frame(Scene *scene, const Game *game, const Camera *camera, const float delta_ms) {
    user_asset_frame();
    post_builds_frame(scene, game ? game->map : NULL);
    post_spawned_frame(scene, game ? game->map : NULL); // Object Manager objects (before the pickups: they add theirs)
    // map edits: the casters and the light objects where the objects are now, at most 4 times a second
    // (a carried prop sends up to 60 edits a second; the static shadow map is the whole map's)
    static float since_rebuild = 1000.0f;
    since_rebuild += delta_ms;
    if (g_map_edits_dirty && game && game->map && since_rebuild >= 250.0f) {
        since_rebuild = 0.0f;
        g_map_edits_dirty = 0;
        shadow_load_map(game->map);
        post_lights_unload();
        post_lights(game->map);
    }
    post_pickups_frame(game);
    zone_mesh_frame(game, delta_ms);

    if (client_render_settings()->texture_anim) {
        for (size_t i = 0; i < g_spin_count; i++) {
            SpinMesh *s = &g_spins[i];
            s->angle = fmodf(s->angle + s->rate * delta_ms, 2.0f * (float) M_PI);
            float axis[9], m[9];
            post_mat3_xyz(s->axis_x ? (vec3) {s->angle, 0.0f, 0.0f} : (vec3) {0.0f, s->angle, 0.0f}, axis);
            for (int r = 0; r < 3; r++) {
                for (int c = 0; c < 3; c++) m[r * 3 + c] = s->base[r * 3] * axis[c] + s->base[r * 3 + 1] * axis[3 + c] + s->base[r * 3 + 2] * axis[6 + c];
            }
            // Euler XYZ of m (three.js setFromRotationMatrix)
            const float m13 = fminf(fmaxf(m[2], -1.0f), 1.0f);
            vec3 e = {0.0f, asinf(m13), 0.0f};
            if (fabsf(m13) < 0.9999999f) {
                e.x = atan2f(-m[5], m[8]);
                e.z = atan2f(-m[1], m[0]);
            } else {
                e.x = atan2f(m[7], m[4]);
            }
            s->mesh->transform.rotation = e;
        }
    }

    g_distance_timer -= delta_ms;
    if (g_distance_timer <= 0.0f && camera) {
        g_distance_timer = 500.0f;
        for (size_t i = 0; i < g_distance_count; i++) {
            const DistanceMesh *d = &g_distance_meshes[i];
            const vec3 p = d->mesh->transform.position;
            const float dx = p.x - camera->position.x, dy = p.y - camera->position.y, dz = p.z - camera->position.z;
            d->mesh->visible = d->visible && sqrtf(dx * dx + dy * dy + dz * dz) <= POST_CANVAS_VIEW_DISTANCE;
        }
    }
}

// ---- leaderboards (MapManager.addLeaderBoard pendingLeaders / updatePendingLeader)
typedef struct {
    Object *object;
    Mesh *mesh;
} LeaderboardMesh;

static LeaderboardMesh *g_leaderboards;
static size_t g_leaderboard_count;
static Scene *g_leaderboard_scene;

// utils.js getTime(ms, true, ms >= 3600000, 1, ms >= 86400000)
static void post_leader_time(const double ms, char *out, const size_t size) {
    const long long v = (long long) ms;
    const long long milli = v % 1000;
    const int seconds = (int) (v / 1000 % 60), minutes = (int) (v / 60000 % 60);
    const int hours = (int) (v / 3600000 % 24), days = (int) (v / 86400000);
    char milli_text[16] = "0";
    if (milli) snprintf(milli_text, sizeof(milli_text), "%lld", milli);
    char head[32] = "";
    if (v >= 86400000 && days) snprintf(head, sizeof(head), "%02d:", days);
    if (v >= 3600000 && (hours || days)) {
        const size_t n = strlen(head);
        if (v >= 86400000 && days) snprintf(head + n, sizeof(head) - n, "%02d:", hours);
        else snprintf(head + n, sizeof(head) - n, "%d:", hours);
    }
    snprintf(out, size, "%s%02d:%02d.%s", head, minutes, seconds, milli_text);
}

static const char *post_json_text(const cJSON *raw, const char *key, const char *fallback) {
    const cJSON *item = cJSON_GetObjectItem(raw, key);
    return cJSON_IsString(item) && cJSON_GetStringValue(item)[0] ? cJSON_GetStringValue(item) : fallback;
}

int map_post_leaderboard_rows(Scene *scene, const Map *map, const int index, const char *const *cells, const int cell_count) {
    if (!map || !cells || cell_count <= 0) return 0;
    // the index-th LEADERBOARD map object
    Object *object = NULL;
    for (size_t i = 0, k = 0; i < map->object_count; i++) {
        if (map->objects[i]->prefab != PREFAB_LEADERBOARD) continue;
        if ((int) k++ == index) {
            object = map->objects[i];
            break;
        }
    }
    if (!object || !object->raw) return 0;
    const cJSON *raw = object->raw;

    // addLeaderBoard: leaderType ["score", "time"][limitMM(ldt, 0, 1)], or the custom key ldc
    // (a-z0-9_, <= 10); fontSize parseInt(sf) || 10 (* min(1024 / w, 1024 / h) with nsf)
    char type[16];
    const cJSON *ldc = cJSON_GetObjectItem(raw, "ldc");
    size_t n = 0;
    if (cJSON_IsString(ldc)) {
        for (const char *p = cJSON_GetStringValue(ldc); *p && n < 10; p++) {
            if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_') type[n++] = *p;
        }
    }
    type[n] = 0;
    if (!n) snprintf(type, sizeof(type), "%s", post_json_number(cJSON_GetObjectItem(raw, "ldt")) >= 1.0 ? "time" : "score");
    const int is_time = !strcmp(type, "time") || !strcmp(type, "Time") || !strcmp(type, "TIME");
    const float w = object->scale.x, h = object->scale.z;
    int cw, ch;
    {
        float sx = 1.0f, sy = 1.0f;
        if (h > w) sx = h > 0.0f ? w / h : 1.0f;
        else if (h < w) sy = w > 0.0f ? h / w : 1.0f;
        cw = (int) (1024.0f * sx);
        ch = (int) (1024.0f * sy);
    }
    if (cw <= 0 || ch <= 0) return 0;
    double font = (int) post_json_number(cJSON_GetObjectItem(raw, "sf"));
    if (font <= 0.0) font = 10.0;
    if (cJSON_GetObjectItem(raw, "nsf") && !cJSON_IsNull(cJSON_GetObjectItem(raw, "nsf"))) font *= fmin(1024.0 / w, 1024.0 / h);
    const char *fcolr = post_json_text(raw, "sc", "#000");
    const char *bcolr = post_json_text(raw, "sb", "#fff");
    const cJSON *bv = cJSON_GetObjectItem(raw, "bv");
    const int no_background = bv && !cJSON_IsNull(bv) && !cJSON_IsFalse(bv) && !(cJSON_IsNumber(bv) && cJSON_GetNumberValue(bv) == 0.0);

    Canvas2D *c = prefab_canvas_begin(cw, ch);
    if (!c) return 0;
    if (!no_background) {
        float rgba[4] = {1, 1, 1, 1};
        c2d_parse_color(bcolr, rgba);
        c2d_set_fill_color(c, rgba[0], rgba[1], rgba[2], rgba[3]);
        c2d_fill_rect(c, 0.0, 0.0, cw, ch);
    }
    c2d_set_font(c, font, "GameFont");
    c2d_set_text_align_name(c, "left");
    double y = font * 2.0;
    const double m = c2d_measure_text(c, "M");
    const double x = m / 2.0, step = m * 1.2;
    char header[16];
    snprintf(header, sizeof(header), "%s", type);
    if (header[0] >= 'a' && header[0] <= 'z') header[0] = (char) (header[0] - 32); // capFirst
    prefab_canvas_text(c, "Player", fcolr, x, y, "left", font * 1.25, NULL, 0.0);
    prefab_canvas_text(c, header, fcolr, cw - x, y, "right", font * 1.25, NULL, 0.0);
    y = font * 1.25 * 2.0;
    float rgba[4] = {0, 0, 0, 1};
    c2d_parse_color(fcolr, rgba);
    c2d_set_fill_color(c, rgba[0], rgba[1], rgba[2], rgba[3]);
    c2d_fill_rect(c, 0.0, y, cw, font / 3.0);
    y += font * 2.0;
    for (int i = 0; i + 1 < cell_count; i += 2) {
        char line[160], value[64];
        snprintf(line, sizeof(line), "%d. %s", 1 + i / 2, cells[i] ? cells[i] : "");
        prefab_canvas_text(c, line, fcolr, x, y, "left", 0.0, NULL, 0.0);
        if (is_time) post_leader_time(atof(cells[i + 1] ? cells[i + 1] : "0"), value, sizeof(value));
        else snprintf(value, sizeof(value), "%s", cells[i + 1] ? cells[i + 1] : "");
        prefab_canvas_text(c, value, fcolr, cw - x, y, "right", 0.0, NULL, 0.0);
        y += step;
        if (y > ch) break;
    }
    const unsigned int texture = prefab_canvas_end(c, NULL);
    if (!texture) return 0;

    for (size_t i = 0; i < g_leaderboard_count; i++) {
        if (g_leaderboards[i].object != object) continue;
        BasicMaterial *material = (BasicMaterial *) g_leaderboards[i].mesh->material;
        if (material->texture) glDeleteTextures(1, &material->texture);
        material->texture = texture;
        return 1;
    }
    Mesh *mesh = prefab_canvas_mesh(object, raw, texture);
    if (!mesh) return 0;
    LeaderboardMesh *grown = realloc(g_leaderboards, (g_leaderboard_count + 1) * sizeof(LeaderboardMesh));
    if (!grown) {
        mesh_fini(mesh);
        return 0;
    }
    g_leaderboards = grown;
    g_leaderboards[g_leaderboard_count++] = (LeaderboardMesh) {object, mesh};
    if (scene) scene_add_mesh(scene, mesh);
    g_leaderboard_scene = scene;
    DistanceMesh *distance = realloc(g_distance_meshes, (g_distance_count + 1) * sizeof(DistanceMesh));
    if (distance) {
        g_distance_meshes = distance;
        g_distance_meshes[g_distance_count++] = (DistanceMesh) {mesh, mesh->visible};
    }
    return 1;
}

static void post_leaderboards_unload(Scene *scene) {
    for (size_t i = 0; i < g_leaderboard_count; i++) {
        Mesh *mesh = g_leaderboards[i].mesh;
        if (scene) scene_remove_mesh(scene, mesh);
        else if (g_leaderboard_scene) scene_remove_mesh(g_leaderboard_scene, mesh);
        BasicMaterial *material = (BasicMaterial *) mesh->material;
        if (material->texture) glDeleteTextures(1, &material->texture);
        mesh_fini(mesh);
    }
    free(g_leaderboards);
    g_leaderboards = NULL;
    g_leaderboard_count = 0;
    g_leaderboard_scene = NULL;
}

void map_post_load(Scene *scene, const Map *map) {
    map_post_unload(scene);
    if (!map || !map->raw_data) return;

    post_noise_planes(map);
    post_lights(map);
    post_track_meshes(map);
    g_distance_timer = 0.0f;
    user_asset_map_load(scene);
    terrain_mesh_load(scene, map);
    post_pickups_load(scene, map);
    post_spawned_load(scene, map);
    zone_mesh_load(scene, map);
    client_world_set_anim_handler(post_object_anim);
    ambient_load_map(scene, map);
}

void map_post_unload(Scene *scene) {
    ambient_unload_map(scene);
    post_lights_unload();
    post_builds_clear();
    post_spawned_unload();
    post_pickups_unload();
    terrain_mesh_unload();
    zone_mesh_unload();
    client_world_set_anim_handler(NULL);
    post_leaderboards_unload(scene);
    user_asset_map_unload(scene);
    free(g_spins);
    g_spins = NULL;
    g_spin_count = 0;
    free(g_distance_meshes);
    g_distance_meshes = NULL;
    g_distance_count = 0;

    for (size_t i = 0; i < g_noise_plane_count; i++) {
        NoisePlane *plane = &g_noise_planes[i];
        if (plane->mesh && plane->mesh->geometry == plane->noise) plane->mesh->geometry = plane->original;

        glDeleteBuffers(1, &plane->vbo);
        glDeleteBuffers(1, &plane->noise->ebo);
        glDeleteVertexArrays(1, &plane->noise->vao);
        free(plane->noise);
    }

    free(g_noise_planes);
    g_noise_planes = NULL;
    g_noise_plane_count = 0;
}
