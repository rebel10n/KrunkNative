// map_vivo_gap_test: the objects of Vivo (assets/maps/official/37_vivo.json) around the wall-top
// gap at (-357.726, 35, -96.280), as the real loader (map_init_ex) makes them.
//
// Result of the investigation (doc in the commit message): both objects that touch the spot are
// prefab-less CUBEs (no "i", so PREFAB_CUBE, map.c map_raw_object_prefab), solid (COLLISION_TYPE_BOX),
// visible and active. The face masks are the official ones: the lower slab (y 10..35) has its top
// face switched off because the upper slab (y 35..37) sits exactly on it (the editor's "Optimize
// Faces"), and the upper slab's top face (y 37) is on. So the surface a player walks on is at y 37.
// Nothing is dropped, and the loader keeps the record's size and height as written.
//
// Run from the repository root: ./<build>/map_vivo_gap_test
#include <shared.h>
#include <map_catalog.h>
#include <prefabs.h>
#include <cJSON.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks, g_failures;
#define CHECK(cond, ...) do { g_checks++; if (!(cond)) { g_failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define VIVO_MAP "assets/maps/official/37_vivo.json"
#define VIVO_OBJECTS 1558

// the gap and the two loader objects that touch it (position = bottom centre, scale = full size)
static const float GAP[3] = {-357.726f, 35.0f, -96.280f};
static const float LOWER_POS[3] = {-359.0f, 10.0f, -153.0f}, LOWER_SIZE[3] = {42.0f, 25.0f, 170.0f};
static const float UPPER_POS[3] = {-359.0f, 35.0f, -153.0f}, UPPER_SIZE[3] = {42.0f, 2.0f, 170.0f};

static int feq(const float a, const float b) { return fabsf(a - b) < 1e-3f; }

// distance from a point to an object's box (bottom centre position, full scale)
static float box_distance(const Object *o, const float g[3]) {
    float d2 = 0.0f;
    for (int k = 0; k < 3; k++) {
        const float c = (&o->position.x)[k];
        const float lo = c - (k == 1 ? 0.0f : (&o->scale.x)[k] * 0.5f);
        const float hi = k == 1 ? c + (&o->scale.x)[k] : c + (&o->scale.x)[k] * 0.5f;
        const float d = g[k] < lo ? lo - g[k] : g[k] > hi ? g[k] - hi : 0.0f;
        d2 += d * d;
    }
    return sqrtf(d2);
}

// the record's "f" face switches as the editor reads them: a hex string, bit 0x08 = Top
// (UTILS.convertFaces order "Right Left Top Bottom Back Front", MSB first)
static int record_top_face(const cJSON *raw) {
    const cJSON *f = cJSON_GetObjectItem(raw, "f");
    if (!cJSON_IsString(f)) return 1; // no "f": all faces on
    return (int) (strtol(cJSON_GetStringValue(f), NULL, 16) & 0x08) != 0;
}

static const Object *object_at(const Map *map, const float pos[3], const float size[3]) {
    for (size_t i = 0; i < map->object_count; i++) {
        const Object *o = map->objects[i];
        if (feq(o->position.x, pos[0]) && feq(o->position.y, pos[1]) && feq(o->position.z, pos[2]) &&
            feq(o->scale.x, size[0]) && feq(o->scale.y, size[1]) && feq(o->scale.z, size[2])) return o;
    }
    return NULL;
}

int main(void) {
    printf("Running Vivo wall-top gap...\n");
    cJSON *json = NULL;
    CHECK(!load_json_file(VIVO_MAP, &json) && json, "load %s (run from the repository root)", VIVO_MAP);
    if (!json) return 1;

    const cJSON *records = cJSON_GetObjectItem(json, "objects");
    CHECK(cJSON_GetArraySize(records) == VIVO_OBJECTS, "Vivo lists %d objects", cJSON_GetArraySize(records));

    Map *map = map_init_ex(json, 0);
    CHECK(map != NULL, "map_init_ex");
    if (!map) {
        cJSON_Delete(json);
        printf("%d checks, %d failures\n", g_checks, g_failures);
        return 1;
    }

    // every record with p and s is kept (no object dropped at the gap or anywhere else)
    CHECK((int) map->object_count == VIVO_OBJECTS, "loader keeps %d objects, Vivo lists %d", (int) map->object_count, VIVO_OBJECTS);

    // exactly the two slabs touch the gap: nothing else is within 12 units of it
    int near = 0;
    for (size_t i = 0; i < map->object_count; i++) {
        const Object *o = map->objects[i];
        if (box_distance(o, GAP) <= 12.0f) near++;
    }
    CHECK(near == 2, "%d loader objects within 12 units of the gap, expected the 2 slabs", near);

    const Object *lower = object_at(map, LOWER_POS, LOWER_SIZE);
    const Object *upper = object_at(map, UPPER_POS, UPPER_SIZE);
    CHECK(lower != NULL, "lower slab 42x25x170 at (-359, 10, -153) is loaded");
    CHECK(upper != NULL, "upper slab 42x2x170 at (-359, 35, -153) is loaded");
    if (lower) {
        CHECK(lower->prefab == PREFAB_CUBE, "lower slab is a CUBE (no prefab key)");
        CHECK(lower->collision_type == COLLISION_TYPE_BOX, "lower slab collides (BOX)");
        CHECK(!lower->no_vis && lower->active, "lower slab is visible and active");
        CHECK(!record_top_face(lower->raw), "lower slab top face is off in its record (\"f\": \"31\"): the upper slab covers it");
        CHECK(box_distance(lower, GAP) == 0.0f, "the gap is on the lower slab's box boundary (its top at 35)");
    }
    if (upper) {
        CHECK(upper->prefab == PREFAB_CUBE, "upper slab is a CUBE (no prefab key)");
        CHECK(upper->collision_type == COLLISION_TYPE_BOX, "upper slab collides (BOX)");
        CHECK(!upper->no_vis && upper->active, "upper slab is visible and active");
        CHECK(record_top_face(upper->raw), "upper slab top face is on in its record (\"f\": \"38\"): the wall top is at y 37");
    }

    map_fini(map);
    free(map);
    cJSON_Delete(json);

    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
