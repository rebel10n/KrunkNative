#include <shared.h>
#include <cJSON.h>
#include <stdio.h>

#ifdef KRUNKNATIVE_CLIENT
#include <client.h>
#endif

Map *map_init(const char *raw) {
    const cJSON *raw_map = cJSON_Parse(raw);
    if (!cJSON_IsObject(raw_map)) return NULL;

    const cJSON *objects = cJSON_GetObjectItem(raw_map, "objects");
    const cJSON *spawns = cJSON_GetObjectItem(raw_map, "spawns");
    const cJSON *colors = cJSON_GetObjectItem(raw_map, "colors");
    const cJSON *cam_pos = cJSON_GetObjectItem(raw_map, "camPos");

    if (!cJSON_IsArray(objects) || !cJSON_IsArray(spawns)) return NULL;

    vec4 *parsed_colors = cJSON_IsArray(colors) ? calloc(cJSON_GetArraySize(colors), sizeof(vec4)) : NULL;
    Map *map = calloc(1, sizeof(Map));

    if (cJSON_IsArray(cam_pos) && cJSON_GetArraySize(cam_pos) >= 3) {
        map->camera_position.x = (float) cJSON_GetNumberValue(cJSON_GetArrayItem(cam_pos, 0));
        map->camera_position.y = (float) cJSON_GetNumberValue(cJSON_GetArrayItem(cam_pos, 1));
        map->camera_position.z = (float) cJSON_GetNumberValue(cJSON_GetArrayItem(cam_pos, 2));
    }

    if (parsed_colors) {
        for (int i = 0; i < cJSON_GetArraySize(colors); i++) {
            const char *hex = cJSON_GetStringValue(cJSON_GetArrayItem(colors, i));
            vec4 color = {1.0f, 1.0f, 1.0f, 1.0f};

            if (hex) parse_hex_color(hex, &color);
            parsed_colors[i] = color;
        }
    }

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
        const cJSON *raw_obj = cJSON_GetArrayItem(objects, i);

        const cJSON *obj_i = cJSON_GetObjectItem(raw_obj, "i");
        const cJSON *obj_id = cJSON_GetObjectItem(raw_obj, "id");
        const cJSON *obj_pos = cJSON_GetObjectItem(raw_obj, "p");
        const cJSON *obj_scale = cJSON_GetObjectItem(raw_obj, "s");

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

        if (prefab_id == PREFAB_RAMP) {
            const cJSON *dir = cJSON_GetObjectItem(raw_obj, "d");
            Ramp *ramp = calloc(1, sizeof(Ramp));

            if (!ramp) continue;

            ramp->direction = cJSON_IsNumber(dir) ? (int) cJSON_GetNumberValue(dir) % 4 : 0;
            ramp->start_x = position.x;
            ramp->start_z = position.z;
            ramp->end_x = position.x;
            ramp->end_z = position.z;

            if (ramp->direction % 2) {
                ramp->start_z -= scale.z * 0.5f * (ramp->direction < 2 ? -1.0f : 1.0f);
                ramp->end_z += scale.z * 0.5f * (ramp->direction < 2 ? -1.0f : 1.0f);
            } else {
                ramp->start_x -= scale.x * 0.5f * (ramp->direction < 2 ? 1.0f : -1.0f);
                ramp->end_x += scale.x * 0.5f * (ramp->direction < 2 ? 1.0f : -1.0f);
            }

            object->ramp = ramp;
        }

#ifdef KRUNKNATIVE_CLIENT
        object->mesh = prefab_init(object, parsed_colors, raw_obj);
        client_animate_object_texture(object, 0.0f);
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
                free(parsed_colors);
                free(map);
                return NULL;
            }

            map->objects[0] = object;
            map->object_count = 1;
        } else {
            Object **new_objects = realloc(map->objects, sizeof(Object) * (map->object_count + 1));

            if (!new_objects) {
                free(object);
                free(parsed_colors);
                free(map);
                return NULL;
            }

            map->objects = new_objects;
            map->objects[map->object_count++] = object;
        }
    }

    free(parsed_colors);
    return map;
}

void map_fini(Map *map) {
    if (map->spawn_count) {
        for (size_t i = 0; i < map->spawn_count; i++) free(map->spawns[i]);

        free(map->spawns);

        map->spawn_count = 0;
        map->spawns = NULL;
    }

    if (map->object_count) {
        for (size_t i = 0; i < map->object_count; i++) free(map->objects[i]);

        free(map->objects);

        map->object_count = 0;
        map->objects = NULL;
    }
}
