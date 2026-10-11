// Map render work that needs every object first (v3.1.1 map.js generate() runs it after
// the object loop): the ambient-occlusion planes (ambient.c), the noise planes
// (94.js addNoisePlanes -> 31.js generatePlane with `noise`) and the POINT_LIGHT objects
// (94.js addPointLight). Owned here: the noise-plane geometries.
#include <gl.h>
#include <map_post.h>
#include <ambient.h>
#include <math.h>
#include <pcg_basic.h>
#include <stdlib.h>
#include <string.h>

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
    int tiles_x = (int) floor(f / 5.0 + 0.5); // Math.round(J / 5), J = s[0] / 2
    int tiles_z = (int) floor(e / 5.0 + 0.5);
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

    const double plane_x = object->position.x;
    const double plane_y = object->position.y;
    const double plane_z = object->position.z;

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
        const cJSON *p = cJSON_GetObjectItem(raw, "p");
        const cJSON *s = cJSON_GetObjectItem(raw, "s");
        if (!cJSON_IsArray(p) || cJSON_GetArraySize(p) < 3 || cJSON_GetArraySize(s) < 3) continue;
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

static void post_point_lights(const Map *map) {
    BasicPointLight lights[BASIC_MAX_POINT_LIGHTS];
    int count = 0;

    const cJSON *raw_colors = cJSON_GetObjectItem(map->raw_data, "colors");
    const cJSON *objects = cJSON_GetObjectItem(map->raw_data, "objects");
    size_t object_index = 0;

    const cJSON *raw;
    cJSON_ArrayForEach(raw, objects) {
        const cJSON *p = cJSON_GetObjectItem(raw, "p");
        const cJSON *s = cJSON_GetObjectItem(raw, "s");
        if (!cJSON_IsArray(p) || cJSON_GetArraySize(p) < 3 || cJSON_GetArraySize(s) < 3) continue;
        if (object_index >= map->object_count) break;

        const Object *object = map->objects[object_index++];
        if (object->prefab != PREFAB_POINT_LIGHT || count >= BASIC_MAX_POINT_LIGHTS) continue;

        // PointLight(colr || "#f2f8fc", parseFloat(li) || 1, parseFloat(lds) || 0, parseFloat(ld) || 1)
        vec4 color = {0.949f, 0.973f, 0.988f, 1.0f};
        const cJSON *ci = cJSON_GetObjectItem(raw, "ci");
        const cJSON *c = cJSON_GetObjectItem(raw, "c");
        const cJSON *palette = cJSON_IsNumber(ci) ? cJSON_GetArrayItem(raw_colors, (int) cJSON_GetNumberValue(ci)) : NULL;
        if (cJSON_IsString(palette)) parse_hex_color(cJSON_GetStringValue(palette), &color);
        else if (cJSON_IsString(c)) parse_hex_color(cJSON_GetStringValue(c), &color);
        else if (cJSON_IsNumber(c)) color = hex_to_vec((int) cJSON_GetNumberValue(c));

        const double intensity = post_json_number(cJSON_GetObjectItem(raw, "li"));
        const double distance = post_json_number(cJSON_GetObjectItem(raw, "lds"));
        const double decay = post_json_number(cJSON_GetObjectItem(raw, "ld"));
        const float i = intensity ? (float) intensity : 1.0f;

        lights[count++] = (BasicPointLight) {
            .position = {
                (float) post_json_number(cJSON_GetArrayItem(p, 0)),
                (float) post_json_number(cJSON_GetArrayItem(p, 1)),
                (float) post_json_number(cJSON_GetArrayItem(p, 2)),
            },
            .color = {color.x * i, color.y * i, color.z * i},
            .distance = (float) distance,
            .decay = decay ? (float) decay : 1.0f,
        };
    }

    basic_material_set_point_lights(lights, count);
}

void map_post_load(Scene *scene, const Map *map) {
    map_post_unload(scene);
    if (!map || !map->raw_data) return;

    post_noise_planes(map);
    post_point_lights(map);
    ambient_load_map(scene, map);
}

void map_post_unload(Scene *scene) {
    ambient_unload_map(scene);
    basic_material_set_point_lights(NULL, 0);

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
