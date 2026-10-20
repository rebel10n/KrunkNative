// Every shipped map keeps every solid object it lists. For each map in the catalogue
// (assets/maps: official v10 files, the 311_ / 152_ v3.1.1 maps, custom and the rest) and each of
// Krunker's 46 official maps (krunker_maps.h, the v10 files under assets/maps/official/) the raw
// "objects" array is read as the loader reads it: a record without a 3-number "p" or a size
// (map_sanitize_json) is dropped, and every other record becomes a Map object except the
// prefabs that build none by design (PREFAB_R_NO_OBJECT: AI / bot spawners, bot path nodes,
// events) and ENTITY instances. A CUBE (a wall) missing from the loaded map is a missing wall, and a CUBE
// the record does not switch off (l / col) must load solid (COLLISION_TYPE_BOX) at its record's size, the
// pooled "si" sizes included. An official map's record count must also be the one krunker_maps.h records.
#include <shared.h>
#include <map_catalog.h>
#include <krunker_maps.h>
#include <prefabs.h>
#include <cJSON.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// the raw record of an object the loader keeps as a Map object
static int loader_builds_object(const cJSON *raw) {
    const int prefab = map_raw_object_prefab(raw);
    return prefab != PREFAB_ENTITY && !prefab_has_role(prefab, PREFAB_R_NO_OBJECT);
}

static int count_items(const cJSON *array) {
    int n = 0;
    for (const cJSON *item = array ? array->child : NULL; item; item = item->next) n++;
    return n;
}

// JS truthiness of a record field ("l", "col"): the loader's map_truthy
static int record_flag(const cJSON *item) {
    if (!item || cJSON_IsNull(item) || cJSON_IsFalse(item)) return 0;
    if (cJSON_IsNumber(item)) return cJSON_GetNumberValue(item) != 0.0;
    if (cJSON_IsString(item)) return cJSON_GetStringValue(item)[0] != 0;
    return 1;
}

// the object's scale is its size in the file as written: the record's "s", or a pooled "si" size
// (xyz[si * 3 .. si * 3 + 2] of the file's own pool). `source` is the record as the file has it, not
// the sanitised copy (map_sanitize_json writes the resolved "s" into the copy).
static int scale_matches_source(const Object *object, const cJSON *source, const cJSON *xyz) {
    double want[3] = {0.0, 0.0, 0.0};
    const cJSON *s = cJSON_GetObjectItem(source, "s");
    if (cJSON_IsArray(s) && cJSON_GetArraySize(s) >= 3) {
        for (int i = 0; i < 3; i++) want[i] = cJSON_GetNumberValue(cJSON_GetArrayItem(s, i));
    } else {
        const cJSON *si = cJSON_GetObjectItem(source, "si");
        const int base = cJSON_IsNumber(si) ? (int) cJSON_GetNumberValue(si) * 3 : -1;
        if (!cJSON_IsArray(xyz) || base < 0 || base + 2 >= cJSON_GetArraySize(xyz)) return 0;
        for (int i = 0; i < 3; i++) want[i] = cJSON_GetNumberValue(cJSON_GetArrayItem(xyz, base + i));
    }
    const float scale[3] = {object->scale.x, object->scale.y, object->scale.z};
    for (int i = 0; i < 3; i++) {
        if (fabs((double) scale[i] - want[i]) > 1e-3) return 0;
    }
    return 1;
}

// 0 when the map keeps every object; prints one line per problem otherwise. `file` is relative
// to the assets directory; `records` < 0 skips the record-count check (catalogue maps).
static int check_map_file(const char *key, const char *file, const int records) {
    char *path = concat(assets_path(), file);
    unsigned char *buffer = NULL;
    size_t length = 0;
    int failures = 0;
    if (!path || read_file(path, &buffer, &length)) {
        printf("FAIL %s: cannot read %s\n", key, path ? path : file);
        free(path);
        return 1;
    }
    free(path);
    cJSON *root = cJSON_ParseWithLength((const char *) buffer, length);
    free(buffer);
    if (!root) {
        printf("FAIL %s: invalid JSON\n", key);
        return 1;
    }

    const cJSON *raw_objects = cJSON_GetObjectItem(root, "objects");
    const int raw_count = count_items(raw_objects);
    if (records >= 0 && raw_count != records) {
        printf("FAIL %s: %d object records, %d expected\n", key, raw_count, records);
        failures++;
    }
    int expected = 0;
    for (const cJSON *raw = raw_objects ? raw_objects->child : NULL; raw; raw = raw->next) {
        expected += loader_builds_object(raw);
    }

    // map_init sanitises its document in place: the objects it keeps are the copy's children
    cJSON *copy = cJSON_Duplicate(root, 1);
    Map *map = copy ? map_init_ex(copy, 0) : NULL;
    if (!map) {
        printf("FAIL %s: map_init failed\n", key);
        failures++;
    } else {
        const cJSON *kept_objects = cJSON_GetObjectItem(copy, "objects");
        const int kept_records = count_items(kept_objects);
        if (kept_records != raw_count) {
            printf("FAIL %s: %d object record(s) without a valid p or size dropped\n", key, raw_count - kept_records);
            failures++;
        }
        if ((int) map->object_count != expected) {
            printf("FAIL %s: %zu map objects, %d expected\n", key, map->object_count, expected);
            failures++;
        }
        int missing = 0, missing_cubes = 0, unsolid = 0, resized = 0;
        // the file's own records, in step with the kept copies when the loader dropped none (aligned)
        const cJSON *source = raw_objects ? raw_objects->child : NULL;
        const cJSON *pool = cJSON_GetObjectItem(root, "xyz");
        const int aligned = kept_records == raw_count;
        for (const cJSON *raw = kept_objects ? kept_objects->child : NULL; raw;
             raw = raw->next, source = source ? source->next : NULL) {
            if (!loader_builds_object(raw)) continue;
            const Object *object = NULL;
            for (size_t k = 0; k < map->object_count && !object; k++) {
                if (map->objects[k] && map->objects[k]->raw == raw) object = map->objects[k];
            }
            const cJSON *p = cJSON_GetObjectItem(raw, "p");
            if (!object) {
                missing++;
                if (map_raw_object_prefab(raw) != PREFAB_CUBE) continue;
                if (missing_cubes++ < 3) {
                    printf("FAIL %s: CUBE dropped at p=[%g, %g, %g]\n", key,
                           cJSON_GetArrayItem(p, 0)->valuedouble, cJSON_GetArrayItem(p, 1)->valuedouble,
                           cJSON_GetArrayItem(p, 2)->valuedouble);
                }
                continue;
            }
            // a CUBE the record does not switch off (l / col) is a solid box at its record's size. A wall that
            // loads walk-through, or at another entry's size from the pool, is a wrong wall (Undergrowth's wall at
            // p=[84, 80, 44] reads pool entry 410)
            if (map_raw_object_prefab(raw) != PREFAB_CUBE) continue;
            if (!record_flag(cJSON_GetObjectItem(raw, "l")) && !record_flag(cJSON_GetObjectItem(raw, "col")) &&
                object->collision_type != COLLISION_TYPE_BOX) {
                if (unsolid++ < 3) {
                    printf("FAIL %s: CUBE at p=[%g, %g, %g] is not solid (collision type %d)\n", key,
                           cJSON_GetArrayItem(p, 0)->valuedouble, cJSON_GetArrayItem(p, 1)->valuedouble,
                           cJSON_GetArrayItem(p, 2)->valuedouble, (int) object->collision_type);
                }
            }
            if (aligned && source && !scale_matches_source(object, source, pool)) {
                if (resized++ < 3) {
                    printf("FAIL %s: CUBE at p=[%g, %g, %g] has scale %g,%g,%g, not its record's size\n", key,
                           cJSON_GetArrayItem(p, 0)->valuedouble, cJSON_GetArrayItem(p, 1)->valuedouble,
                           cJSON_GetArrayItem(p, 2)->valuedouble, object->scale.x, object->scale.y, object->scale.z);
                }
            }
        }
        if (missing) {
            printf("FAIL %s: %d object(s) dropped by the loader (%d CUBE)\n", key, missing, missing_cubes);
            failures++;
        }
        if (unsolid) {
            printf("FAIL %s: %d CUBE(s) loaded without collision\n", key, unsolid);
            failures++;
        }
        if (resized) {
            printf("FAIL %s: %d CUBE(s) loaded at the wrong size\n", key, resized);
            failures++;
        }
        map_fini(map);
    }
    cJSON_Delete(copy);
    cJSON_Delete(root);
    return failures ? 1 : 0;
}

static int check_catalog_map(const MapCatalogEntry *entry) {
    return check_map_file(entry->key, entry->path, -1);
}

#define CHECK_OFFICIAL(index_, name_, file_, objects_) \
    if (check_map_file(name_, file_, objects_)) official_failed++;

int main(void) {
    const size_t count = map_catalog_count();
    int failed_maps = 0;
    for (size_t i = 0; i < count; i++) {
        const MapCatalogEntry *entry = map_catalog_at(i);
        if (entry && check_catalog_map(entry)) failed_maps++;
    }
    int official_failed = 0;
    KRUNKER_OFFICIAL_MAPS(CHECK_OFFICIAL)
    printf("map_drop_test: %zu catalogue maps, %d with dropped objects; %d official maps, %d with dropped objects\n",
           count, failed_maps, KRUNKER_OFFICIAL_MAP_COUNT, official_failed);
    return failed_maps || official_failed ? 1 : 0;
}
