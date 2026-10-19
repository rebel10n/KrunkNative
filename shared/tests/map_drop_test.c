// Every shipped map keeps every solid object it lists. For each map in the catalogue
// (assets/maps: official v10 files, the 311_ / 152_ v3.1.1 maps, custom and the rest) the raw
// "objects" array is read as the loader reads it: a record without a 3-number "p" or a size
// (map_sanitize_json) is dropped, and every other record becomes a Map object except the
// prefabs that build none by design (PREFAB_R_NO_OBJECT: AI / bot spawners, bot path nodes,
// events) and ENTITY instances. A CUBE (a wall) missing from the loaded map is a missing wall.
#include <shared.h>
#include <map_catalog.h>
#include <prefabs.h>
#include <cJSON.h>

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

// 0 when the map keeps every object; prints one line per problem otherwise
static int check_map(const MapCatalogEntry *entry) {
    char *path = concat(assets_path(), entry->path);
    unsigned char *buffer = NULL;
    size_t length = 0;
    int failures = 0;
    if (!path || read_file(path, &buffer, &length)) {
        printf("FAIL %s: cannot read %s\n", entry->key, path ? path : entry->path);
        free(path);
        return 1;
    }
    free(path);
    cJSON *root = cJSON_ParseWithLength((const char *) buffer, length);
    free(buffer);
    if (!root) {
        printf("FAIL %s: invalid JSON\n", entry->key);
        return 1;
    }

    const cJSON *raw_objects = cJSON_GetObjectItem(root, "objects");
    const int raw_count = count_items(raw_objects);
    int expected = 0;
    for (const cJSON *raw = raw_objects ? raw_objects->child : NULL; raw; raw = raw->next) {
        expected += loader_builds_object(raw);
    }

    // map_init sanitises its document in place: the objects it keeps are the copy's children
    cJSON *copy = cJSON_Duplicate(root, 1);
    Map *map = copy ? map_init_ex(copy, 0) : NULL;
    if (!map) {
        printf("FAIL %s: map_init failed\n", entry->key);
        failures++;
    } else {
        const cJSON *kept_objects = cJSON_GetObjectItem(copy, "objects");
        const int kept_records = count_items(kept_objects);
        if (kept_records != raw_count) {
            printf("FAIL %s: %d object record(s) without a valid p or size dropped\n", entry->key, raw_count - kept_records);
            failures++;
        }
        if ((int) map->object_count != expected) {
            printf("FAIL %s: %zu map objects, %d expected\n", entry->key, map->object_count, expected);
            failures++;
        }
        int missing = 0, missing_cubes = 0;
        for (const cJSON *raw = kept_objects ? kept_objects->child : NULL; raw; raw = raw->next) {
            if (!loader_builds_object(raw)) continue;
            int found = 0;
            for (size_t k = 0; k < map->object_count && !found; k++) {
                found = map->objects[k] && map->objects[k]->raw == raw;
            }
            if (found) continue;
            missing++;
            if (map_raw_object_prefab(raw) != PREFAB_CUBE) continue;
            if (missing_cubes++ < 3) {
                const cJSON *p = cJSON_GetObjectItem(raw, "p");
                printf("FAIL %s: CUBE dropped at p=[%g, %g, %g]\n", entry->key,
                       cJSON_GetArrayItem(p, 0)->valuedouble, cJSON_GetArrayItem(p, 1)->valuedouble,
                       cJSON_GetArrayItem(p, 2)->valuedouble);
            }
        }
        if (missing) {
            printf("FAIL %s: %d object(s) dropped by the loader (%d CUBE)\n", entry->key, missing, missing_cubes);
            failures++;
        }
        map_fini(map);
    }
    cJSON_Delete(copy);
    cJSON_Delete(root);
    return failures ? 1 : 0;
}

int main(void) {
    const size_t count = map_catalog_count();
    int failed_maps = 0;
    for (size_t i = 0; i < count; i++) {
        const MapCatalogEntry *entry = map_catalog_at(i);
        if (entry && check_map(entry)) failed_maps++;
    }
    printf("map_drop_test: %zu maps, %d with dropped objects\n", count, failed_maps);
    return failed_maps ? 1 : 0;
}
