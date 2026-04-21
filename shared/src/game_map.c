#include <shared.h>
#include <cJSON.h>
#include <stdio.h>

#ifdef KRUNKNATIVE_CLIENT
#include <client.h>
#endif

GameMap *game_map_init(const char *raw) {
    const cJSON *raw_map = cJSON_Parse(raw);
    if (!cJSON_IsObject(raw_map)) return NULL;

    const cJSON *objects = cJSON_GetObjectItem(raw_map, "objects");
    const cJSON *spawns = cJSON_GetObjectItem(raw_map, "spawns");
    const cJSON *colors = cJSON_GetObjectItem(raw_map, "colors");

    if (!cJSON_IsArray(objects) || !cJSON_IsArray(spawns)) return NULL;

    GameMap *map = calloc(1, sizeof(GameMap));

    for (int i  = 0;  i < cJSON_GetArraySize(spawns); i++) {
        const cJSON *raw_spawn = cJSON_GetArrayItem(spawns, i);
        if (!cJSON_IsArray(raw_spawn) || cJSON_GetArraySize(raw_spawn) < 3) continue;

        const vec3 position = {
            (float) cJSON_GetNumberValue(cJSON_GetArrayItem(raw_spawn, 0)),
            (float) cJSON_GetNumberValue(cJSON_GetArrayItem(raw_spawn, 1)),
            (float) cJSON_GetNumberValue(cJSON_GetArrayItem(raw_spawn, 2)),
        };

        const int team = (int) cJSON_GetNumberValue(cJSON_GetArrayItem(raw_spawn, 3));
        const int dir = cJSON_IsNumber(cJSON_GetArrayItem(raw_spawn, 4)) ? (int) cJSON_GetNumberValue(cJSON_GetArrayItem(raw_spawn, 4)) : 0;
        const int comp = cJSON_GetArrayItem(raw_spawn, 5) != NULL && cJSON_GetNumberValue(cJSON_GetArrayItem(raw_spawn, 5)) != 0;

        Spawn *spawn = calloc(1, sizeof(Spawn));
        if (!spawn) continue;

        spawn->position = position;
        spawn->team = team;
        spawn->direction = dir;
        spawn->comp = comp;

        if (!map->spawns) {
            map->spawns = calloc(1, sizeof(Spawn*));

            if (!map->spawns) {
                free(spawn);
                free(map);
                return NULL;
            }

            map->spawns[0] = spawn;
            map->spawn_count = 1;
        } else {
            Spawn **new_spawns = realloc(map->spawns, (map->spawn_count + 1) * sizeof(Spawn*));

            if (!new_spawns) {
                free(spawn);
                free(map);
                return NULL;
            }

            map->spawns = new_spawns;
            map->spawns[map->spawn_count++] = spawn;
        }
    }

    for (int i = 0; i < cJSON_GetArraySize(objects); i++) {
        cJSON *raw_obj = cJSON_GetArrayItem(objects, i);

        const cJSON *obj_i = cJSON_GetObjectItem(raw_obj, "i");
        const cJSON *obj_id = cJSON_GetObjectItem(raw_obj, "id");
        const cJSON *obj_pos = cJSON_GetObjectItem(raw_obj, "p");
        const cJSON *obj_scale = cJSON_GetObjectItem(raw_obj, "s");
        const cJSON *obj_color = cJSON_GetObjectItem(raw_obj, "c");
        const cJSON *obj_color_idx = cJSON_GetObjectItem(raw_obj, "ci");

        // TODO: parse color

        int prefab_id = 0;

        if (cJSON_IsNumber(obj_i)) {
            prefab_id = (int) cJSON_GetNumberValue(obj_i);
        } else if (cJSON_IsNumber(obj_id)) {
            prefab_id = (int) cJSON_GetNumberValue(obj_id);
        }

        if (!cJSON_IsArray(obj_pos) || cJSON_GetArraySize(obj_pos) < 3 || cJSON_GetArraySize(obj_scale) < 3) continue;

        Object *object = calloc(1, sizeof(Object));
        if (!object) continue;

        const vec3 position = {
            (float) cJSON_GetNumberValue(cJSON_GetArrayItem(obj_pos, 0)),
            (float) cJSON_GetNumberValue(cJSON_GetArrayItem(obj_pos, 1)),
            (float) cJSON_GetNumberValue(cJSON_GetArrayItem(obj_pos, 2))
        };

        const vec3 scale = {
            (float) cJSON_GetNumberValue(cJSON_GetArrayItem(obj_scale, 0)),
            (float) cJSON_GetNumberValue(cJSON_GetArrayItem(obj_scale, 1)),
            (float) cJSON_GetNumberValue(cJSON_GetArrayItem(obj_scale, 2)),
        };

        object->position = position;
        object->scale = scale;
        object->prefab = prefab_id;

#ifdef KRUNKNATIVE_CLIENT
        object->mesh = prefab_init(object, raw_obj);
#endif

        if (position.x - scale.x * 0.5f < map->dimensions.min.x) {
            map->dimensions.min.x = position.x - scale.x * 0.5f;
        } else if (position.x + scale.x * 0.5f > map->dimensions.max.x) {
            map->dimensions.max.x = position.x + scale.x * 0.5f;
        }

        if (position.y - scale.y * 0.5f < map->dimensions.min.y) {
            map->dimensions.min.y = position.y - scale.y * 0.5f;
        } else if (position.y + scale.y * 0.5f > map->dimensions.max.y) {
            map->dimensions.max.y = position.y + scale.y * 0.5f;
        }

        if (position.z - scale.z * 0.5f < map->dimensions.min.z) {
            map->dimensions.min.z = position.z - scale.z * 0.5f;
        } else if (position.z + scale.z * 0.5f > map->dimensions.max.z) {
            map->dimensions.max.z = position.z + scale.z * 0.5f;
        }

        if (!map->objects) {
            map->objects = calloc(1, sizeof(Object));

            if (!map->objects) {
                free(object);
                free(map);
                return NULL;
            }

            map->objects[0] = object;
            map->object_count = 1;
        } else {
            Object **new_objects = realloc(map->objects, sizeof(Object) * (map->object_count + 1));

            if (!new_objects) {
                free(object);
                free(map);
                return NULL;
            }

            map->objects = new_objects;
            map->objects[map->object_count++] = object;
        }
    }

    return map;
}
