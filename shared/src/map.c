#include <shared.h>
#include <cJSON.h>
#include <math.h>
#include <stdio.h>

#ifdef KRUNKNATIVE_CLIENT
#include <client.h>
#elif defined(KRUNKNATIVE_SERVER)
#include <server.h>
#endif

const MapConfig g_default_map_config = {
    .cam_type = CAMERA_TYPE_NORMAL,
    .cam_rotation = 1,
    .cam_offset = {},
    .model = MODEL_TYPE_NORMAL,
    .speed = {1.0f, 1.0f, 1.0f},
    .ladder_accel = 1.0f,
    .air_accel = 1.0f,
    .slide_time = 1.0f,
    .slide_accel = 1.0f,
    .jump_cooldown = 1.0f,
    .infinite_jump = 0,
};

const char *default_map_names[] = {
    "burg",
    "littletown",
    "sandstorm",
    "subzero",
    "undergrowth",
    "shipyard",
    "freight",
    "lostworld",
    "citadel",
    "oasis",
    "kanji",
    "newtown",
    "industry",
    "soul_sanctum",
    "evacuation",
};

const cJSON *g_maps[sizeof(default_map_names) / sizeof(default_map_names[0])];
const int g_rotation_maps[] = {0, 1, 2, 3, 4, 6, 7, 8, 9, 10, 11, 12, 14};

void load_default_maps() {
    for (size_t i = 0; i < sizeof(default_map_names) / sizeof(default_map_names[0]); i++) {
        if (g_maps[i]) continue;

        const size_t path_length = snprintf(NULL, 0, "maps/%s.json", default_map_names[i]);
        char *path = malloc(path_length + 1);
        if (!path) continue;

        snprintf(path, path_length + 1, "maps/%s.json", default_map_names[i]);
        char *full_path = concat(assets_path(), path);

        size_t map_length;
        unsigned char *map_buffer;

        if (read_file(full_path, &map_buffer, &map_length)) continue;

        g_maps[i] = cJSON_Parse((char *) map_buffer);

        free(map_buffer);
        free(full_path);
        free(path);
    }
}

Map *map_init(const cJSON *raw_data) {
    if (!cJSON_IsObject(raw_data)) return NULL;

    const cJSON *objects = cJSON_GetObjectItem(raw_data, "objects");
    const cJSON *spawns = cJSON_GetObjectItem(raw_data, "spawns");
    const cJSON *colors = cJSON_GetObjectItem(raw_data, "colors");
    const cJSON *cam_pos = cJSON_GetObjectItem(raw_data, "camPos");
    const cJSON *death_y = cJSON_GetObjectItem(raw_data, "dthY");

    if (!cJSON_IsArray(objects) || !cJSON_IsArray(spawns)) return NULL;

    vec4 *parsed_colors = cJSON_IsArray(colors) ? calloc(cJSON_GetArraySize(colors), sizeof(vec4)) : NULL;
    Map *map = calloc(1, sizeof(Map));

    if (!map) return NULL;

    map->raw_data = raw_data;
    map->config = g_default_map_config;
    map->death_y = cJSON_IsNumber(death_y) ? (float) cJSON_GetNumberValue(death_y) : game_constants.death_y;

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
                free(parsed_colors);
                return NULL;
            }

            map->spawns[0] = spawn;
            map->spawn_count = 1;
        } else {
            Spawn **new_spawns = realloc(map->spawns, (map->spawn_count + 1) * sizeof(Spawn*));

            if (!new_spawns) {
                free(spawn);
                free(map);
                free(parsed_colors);
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

        object->active = 1;
        object->position = position;
        object->scale = scale;
        object->prefab = prefab_id;

        if (prefab_id == PREFAB_BOOST_PAD) {
            const cJSON *bm = cJSON_GetObjectItem(raw_obj, "bm");
            const cJSON *cr = cJSON_GetObjectItem(raw_obj, "cr");

            object->jump_pad = 1;
            object->bounce = cJSON_IsNumber(bm) ? (float) cJSON_GetNumberValue(bm) : 0.0f;
            object->crouch = !cr || cJSON_IsNull(cr);

            if (!object->bounce) object->bounce = 1.0f;
        }

        const cJSON *dir = cJSON_GetObjectItem(raw_obj, "d");

        if (prefab_id == PREFAB_LADDER) {
            object->ladder = 1;
            object->direction = cJSON_IsNumber(dir) ? (int) cJSON_GetNumberValue(dir) % 4 : 0;

            const float direction = (float) M_PI * 0.5f * (float) object->direction;

            object->position.x += game_constants.ladder_scale * cosf(direction);
            object->position.z += game_constants.ladder_scale * sinf(direction);

            object->scale.x = (object->direction % 2 ? game_constants.ladder_width : game_constants.ladder_scale) * 2.0f;
            object->scale.z = (object->direction % 2 ? game_constants.ladder_scale : game_constants.ladder_width) * 2.0f;
        }

        if (prefab_id == PREFAB_RAMP) {
            const cJSON *boost = cJSON_GetObjectItem(raw_obj, "b");

            Ramp *ramp = calloc(1, sizeof(Ramp));
            if (!ramp) continue;

            object->direction = cJSON_IsNumber(dir) ? (int) cJSON_GetNumberValue(dir) % 4 : 0;

            ramp->boost = cJSON_IsNumber(boost) ? (float) cJSON_GetNumberValue(boost) : 0.0f;
            ramp->start.x = position.x;
            ramp->start.y = position.z;
            ramp->end.x = position.x;
            ramp->end.y = position.z;

            if (object->direction % 2) {
                ramp->start.y -= scale.z * 0.5f * (object->direction < 2 ? 1.0f : -1.0f);
                ramp->end.y += scale.z * 0.5f * (object->direction < 2 ? 1.0f : -1.0f);
            } else {
                ramp->start.x -= scale.x * 0.5f * (object->direction < 2 ? 1.0f : -1.0f);
                ramp->end.x += scale.x * 0.5f * (object->direction < 2 ? 1.0f : -1.0f);
            }

            object->ramp = ramp;
        }

        const cJSON *obj_l = cJSON_GetObjectItem(raw_obj, "l");
        const cJSON *obj_col = cJSON_GetObjectItem(raw_obj, "col");
        const cJSON *obj_bo = cJSON_GetObjectItem(raw_obj, "bo");

        const int no_collisions = obj_l && !cJSON_IsNull(obj_l) && (!cJSON_IsNumber(obj_l) || cJSON_GetNumberValue(obj_l) != 0) || obj_col && !cJSON_IsNull(obj_col) && (!cJSON_IsNumber(obj_col) || cJSON_GetNumberValue(obj_col) != 0);
        const int is_border = obj_bo && !cJSON_IsNull(obj_bo) && (!cJSON_IsNumber(obj_bo) || cJSON_GetNumberValue(obj_bo) != 0);

        switch (prefab_id) {
            case PREFAB_CUBE:
            case PREFAB_GATE:
            case PREFAB_DEPOSIT_BOX:
            case PREFAB_PREMIUM_ZONE:
            case PREFAB_VERIFIED_ZONE:
            case PREFAB_TEAM_ZONE:
                object->is_border = is_border;
                break;
            default:
                break;
        }

        switch (prefab_id) {
            case PREFAB_SPHERE:
            case PREFAB_POINT_LIGHT:
            case PREFAB_LIGHT_CONE:
            case PREFAB_SOUND_EMITTER:
            case PREFAB_PARTICLES:
            case PREFAB_LIQUID:
            case PREFAB_SPECTATE_CAM:
            case PREFAB_EVENT:
                object->collision_type = COLLISION_TYPE_NONE;
                break;
            case PREFAB_GATE:
            case PREFAB_TRIGGER:
            case PREFAB_TERMINAL:
            case PREFAB_DEPOSIT_BOX:
            case PREFAB_OBJECTIVE:
            case PREFAB_BOMB_SITE:
            case PREFAB_FLAG:
            case PREFAB_WEAPON_PICKUP:
            case PREFAB_SHOWCASE:
            case PREFAB_RAMP:
            case PREFAB_PREMIUM_ZONE:
            case PREFAB_VERIFIED_ZONE:
            case PREFAB_SCORE_ZONE:
            case PREFAB_DEATH_ZONE:
            case PREFAB_CHECK_POINT:
            case PREFAB_TELEPORTER:
            case PREFAB_LADDER:
                object->collision_type = COLLISION_TYPE_BOX;
                break;
            default:
                object->collision_type = no_collisions ? COLLISION_TYPE_NONE : prefab_id == PREFAB_CYLINDER ? COLLISION_TYPE_CYLINDER : COLLISION_TYPE_BOX;
                break;
        }

        switch (prefab_id) {
            case PREFAB_BOT:
            case PREFAB_TELEPORTER:
            case PREFAB_CHECK_POINT:
            case PREFAB_DEATH_ZONE:
            case PREFAB_SCORE_ZONE:
            case PREFAB_TEAM_ZONE:
            case PREFAB_VERIFIED_ZONE:
            case PREFAB_RAMP:
            case PREFAB_SPECTATE_CAM:
            case PREFAB_SIGN:
            case PREFAB_LIQUID:
            case PREFAB_PARTICLES:
            case PREFAB_SHOWCASE:
            case PREFAB_WEAPON_PICKUP:
            case PREFAB_FLAG:
            case PREFAB_BOMB_SITE:
            case PREFAB_OBJECTIVE:
            case PREFAB_SOUND_EMITTER:
            case PREFAB_LIGHT_CONE:
            case PREFAB_POINT_LIGHT:
                object->wall_jumpable = 0;
                break;
            default: {
                const cJSON *wj = cJSON_GetObjectItem(raw_obj, "wj");
                object->wall_jumpable = !wj || cJSON_IsNull(wj);
                break;
            }
        }

        // TODO: parse any additional metadata
        switch (prefab_id) {
            case PREFAB_VERIFIED_ZONE:
                object->verified = 1;
                break;
            case PREFAB_PREMIUM_ZONE:
                object->premium = 1;
                break;
            case PREFAB_SCORE_ZONE:
                object->score_zone = 1;
                break;
            case PREFAB_TELEPORTER:
                object->teleporter = 1;
                break;
            case PREFAB_CHECK_POINT:
                object->checkpoint = 1;
                break;
            case PREFAB_WEAPON_PICKUP:
                object->pickup = 1;
                break;
            case PREFAB_FLAG:
                object->flag = 1;
                break;
            case PREFAB_TRIGGER:
                object->trigger = 1;
                break;
            case PREFAB_DEATH_ZONE:
                object->kill = 1;
                break;
            case PREFAB_OBJECTIVE:
                object->objective = 1;
                break;
            case PREFAB_BOMB_SITE:
                object->bomb_site = 1;
                break;
            default:
                break;
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

    if (parsed_colors) free(parsed_colors);
    return map;
}

void map_reset(Map *map) {

}

void map_fini(Map *map) {
    if (map->spawn_count) {
        for (size_t i = 0; i < map->spawn_count; i++) free(map->spawns[i]);

        free(map->spawns);

        map->spawn_count = 0;
        map->spawns = NULL;
    }

    if (map->object_count) {
        for (size_t i = 0; i < map->object_count; i++) {
            Object *object = map->objects[i];

            if (object->tex_anim) free(object->tex_anim);
            if (object->ramp) free(object->ramp);

#ifdef KRUNKNATIVE_CLIENT
            if (object->mesh) mesh_fini(object->mesh);
#endif

            free(object);
        }

        free(map->objects);

        map->object_count = 0;
        map->objects = NULL;
    }
}
