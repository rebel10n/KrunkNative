#include <shared.h>
#include <map_catalog.h>
#include <cJSON.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h> // atoi
#include <string.h>

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

// The official maps (catalogue indices 0..14 = v3.1.1 mapIndex, shared/src/map_catalog.c)
const cJSON *g_maps[MAP_CATALOG_OFFICIAL_COUNT];
const int g_rotation_maps[] = {0, 1, 2, 3, 4, 6, 7, 8, 9, 10, 11, 12, 14};

static const char *prefab_names[] = {
    "CUBE",
    "CRATE",
    "BARREL",
    "LADDER",
    "PLANE",
    "SPAWN_POINT",
    "CAMERA_POSITION",
    "VEHICLE",
    "STACK",
    "RAMP",
    "SCORE_ZONE",
    "BILLBOARD",
    "DEATH_ZONE",
    "PARTICLES",
    "OBJECTIVE",
    "TREE",
    "CONE",
    "CONTAINER",
    "GRASS",
    "CONTAINERR",
    "ACIDBARREL",
    "DOOR",
    "WINDOW",
    "FLAG",
    "GATE",
    "CHECK_POINT",
    "WEAPON_PICKUP",
    "TELEPORTER",
    "TEDDY",
    "TRIGGER",
    "SIGN",
    "DEPOSIT_BOX",
    "LIGHT_CONE",
    "SPECTATE_CAM",
    "SPHERE",
    "PLACEHOLDER",
    "CARDB",
    "PALLET",
    "LIQUID",
    "SOUND_EMITTER",
    "EVENT",
    "TERMINAL",
    "PREMIUM_ZONE",
    "VERIFIED_ZONE",
    "CUSTOM_ASSET",
    "BOMB_SITE",
    "BOOST_PAD",
    "TEAM_ZONE",
    "CYLINDER",
    "POLICE",
    "CAGE",
    "EBARREL",
    "SHOWCASE",
    "POINT_LIGHT",
    "GHOST",
    "BOT",
    "PUMPKIN",
    "RUNE",
    "SKELETON",
    "KNIGHT",
};

static int prefab_id_from_string(const char *name) {
    if (!name) return 0;

    for (size_t i = 0; i < sizeof(prefab_names) / sizeof(prefab_names[0]); i++) {
        if (!strcmp(name, prefab_names[i])) return (int) i;
    }

    return 0;
}

int map_raw_object_prefab(const cJSON *raw_obj) {
    const cJSON *obj_i = cJSON_GetObjectItem(raw_obj, "i");
    const cJSON *obj_id = cJSON_GetObjectItem(raw_obj, "id");
    int prefab_id = 0;

    if (cJSON_IsNumber(obj_i)) {
        prefab_id = (int) cJSON_GetNumberValue(obj_i);
    } else if (cJSON_IsString(obj_i)) {
        prefab_id = prefab_id_from_string(cJSON_GetStringValue(obj_i));
    } else if (cJSON_IsNumber(obj_id)) {
        prefab_id = (int) cJSON_GetNumberValue(obj_id);
    } else if (cJSON_IsString(obj_id)) {
        prefab_id = prefab_id_from_string(cJSON_GetStringValue(obj_id));
    }

    // game.js customMap: prefabs.prefabs[i || id || 0] missing -> prefab 0 (CUBE)
    // (Krunk_Plaza, Slide_Neon_Stage, zombie_high... carry ids from newer editors)
    if (prefab_id < 0 || prefab_id >= (int) (sizeof(prefab_names) / sizeof(prefab_names[0]))) prefab_id = PREFAB_CUBE;
    return prefab_id;
}

int map_raw_object_kept(const cJSON *raw_obj) {
    const cJSON *obj_pos = cJSON_GetObjectItem(raw_obj, "p");
    const cJSON *obj_scale = cJSON_GetObjectItem(raw_obj, "s");
    if (!cJSON_IsArray(obj_pos) || cJSON_GetArraySize(obj_pos) < 3 || cJSON_GetArraySize(obj_scale) < 3) return 0;
    // map.js "BOT" -> objects.js addAI: a pending AI (shared/src/ai.c spawns it at game init),
    // never a map object (no mesh, no collision box, no object uid).
    return map_raw_object_prefab(raw_obj) != PREFAB_BOT;
}

static int map_flag_is_set(const cJSON *item) {
    if (!item || cJSON_IsNull(item) || cJSON_IsFalse(item)) return 0;
    if (cJSON_IsNumber(item)) return cJSON_GetNumberValue(item) != 0;
    return 1;
}

// parseInt(value) || 0 for a number or numeric string field
static int map_int(const cJSON *obj, const char *key) {
    const cJSON *item = cJSON_GetObjectItem(obj, key);
    if (cJSON_IsNumber(item)) return (int) cJSON_GetNumberValue(item);
    if (cJSON_IsString(item)) return atoi(cJSON_GetStringValue(item));
    return 0;
}

void load_default_maps() {
    map_catalog_load_official();
    for (size_t i = 0; i < MAP_CATALOG_OFFICIAL_COUNT; i++) g_maps[i] = map_catalog_json(i);
}

const cJSON **map_list_with_custom(const cJSON *custom_map, size_t *map_count) {
    if (map_count) *map_count = 0;
    if (!custom_map) return NULL;

    load_default_maps();

    const size_t default_count = sizeof(g_rotation_maps) / sizeof(g_rotation_maps[0]);
    const cJSON **maps = calloc(default_count + 1, sizeof(cJSON *));
    if (!maps) return NULL;

    maps[0] = custom_map;

    for (size_t i = 0; i < default_count; i++) {
        maps[i + 1] = g_maps[g_rotation_maps[i]];
    }

    if (map_count) *map_count = default_count + 1;
    return maps;
}

// parseFloat(value) || fallback
static float map_float(const cJSON *obj, const char *key, const float fallback) {
    const cJSON *item = cJSON_GetObjectItem(obj, key);
    float value = 0.0f;
    if (cJSON_IsNumber(item)) value = (float) cJSON_GetNumberValue(item);
    else if (cJSON_IsString(item)) value = strtof(cJSON_GetStringValue(item), NULL);
    return value == value && value != 0.0f ? value : fallback;
}

static int map_has(const cJSON *obj, const char *key) { // OBJECT.key != null
    const cJSON *item = cJSON_GetObjectItem(obj, key);
    return item && !cJSON_IsNull(item);
}

static int map_clamp(const int value, const int min, const int max) { // UTILS.limitMM
    return value < min ? min : value > max ? max : value;
}

// map.js addTrigger options -> objects.js addTrigger / GameObject (shared/include/triggers.h).
static TriggerData *map_parse_trigger(const cJSON *raw_obj, const int can_destroy) {
    TriggerData *t = calloc(1, sizeof(TriggerData));
    if (!t) return NULL;

    const int te = map_int(raw_obj, "te");
    // triggers.events[te || 0]; addTrigger drops the onAI events, unknown indexes name nothing
    t->event = te >= 0 && te < TRIGGER_EVENT_COUNT && te != TRIGGER_EVENT_ON_AI_DEATH ? te : TRIGGER_EVENT_NONE;
    t->action = map_int(raw_obj, "ta");
    t->chance = map_clamp(map_int(raw_obj, "cha"), 0, 100);
    t->constant = map_int(raw_obj, "tc");
    t->constant_event = map_clamp(map_int(raw_obj, "tce"), 0, TRIGGER_CONSTANT_EVENT_MAX);
    t->interface_target = map_int(raw_obj, "tin");
    const cJSON *tct = cJSON_GetObjectItem(raw_obj, "tct");
    if (cJSON_IsString(tct) && trigger_text_valid(cJSON_GetStringValue(tct))) snprintf(t->text, sizeof(t->text), "%s", cJSON_GetStringValue(tct));

    // snd: parseInt(esa || es) || 0; assetSnd = esa != null; else emitterSounds[snd] || emitterSounds[0]
    t->asset_sound = (unsigned char) map_has(raw_obj, "esa");
    t->sound = map_flag_is_set(cJSON_GetObjectItem(raw_obj, "esa")) ? map_int(raw_obj, "esa") : map_int(raw_obj, "es");
    if (!t->asset_sound && (t->sound < 0 || t->sound >= TRIGGER_EMITTER_SOUND_COUNT)) t->sound = 0;
    t->volume = fminf(TRIGGER_EMITTER_VOLUME_MAX, fmaxf(TRIGGER_EMITTER_VOLUME_MIN, map_float(raw_obj, "ev", 1.0f)));
    t->rate = fminf(TRIGGER_EMITTER_RATE_MAX, fmaxf(TRIGGER_EMITTER_RATE_MIN, map_float(raw_obj, "er", 1.0f)));
    t->emit_from = map_int(raw_obj, "ef");

    t->single_use = (unsigned char) map_has(raw_obj, "su");
    t->self_destruct = (unsigned char) (map_has(raw_obj, "sfd") && t->single_use && can_destroy);
    t->timer = (float) (t->constant_event ? t->constant_event : 1) * 1000.0f; // triggerTimer = (constantEvent || 1) * 1000
    return t;
}

// The map.js / objects.js fields of the interface objects (Krunker GameObject health,
// canDestroy, interface, gate, bank, check point, teleporter and trigger data).
static void map_parse_interface(Object *object, const cJSON *raw_obj, const int prefab_id) {
    int has_health = 0, has_interface = 0, clamp_interface = 0;

    switch (prefab_id) {
        case PREFAB_SPHERE: case PREFAB_POINT_LIGHT: case PREFAB_FLAG: case PREFAB_BOOST_PAD: case PREFAB_WEAPON_PICKUP:
        case PREFAB_SHOWCASE: case PREFAB_PARTICLES: case PREFAB_LIQUID: case PREFAB_SIGN: case PREFAB_SPECTATE_CAM:
        case PREFAB_RAMP: case PREFAB_PREMIUM_ZONE: case PREFAB_VERIFIED_ZONE: case PREFAB_TEAM_ZONE: case PREFAB_SCORE_ZONE:
        case PREFAB_DEATH_ZONE: case PREFAB_LADDER: case PREFAB_RUNE: case PREFAB_LIGHT_CONE: case PREFAB_SOUND_EMITTER:
        case PREFAB_OBJECTIVE: case PREFAB_BOMB_SITE: case PREFAB_SPAWN_POINT: case PREFAB_CAMERA_POSITION: case PREFAB_EVENT:
            break;
        case PREFAB_GATE:
        case PREFAB_DEPOSIT_BOX:
            has_interface = clamp_interface = 1;
            break;
        case PREFAB_CHECK_POINT:
        case PREFAB_TELEPORTER:
            break;
        case PREFAB_CUBE: case PREFAB_CYLINDER: case PREFAB_PLANE: case PREFAB_BILLBOARD: case PREFAB_CUSTOM_ASSET:
            has_health = has_interface = 1;
            break;
        default: // TRIGGER, TERMINAL and the addMesh models (crates, barrels, ...)
            has_health = has_interface = clamp_interface = 1;
            break;
    }

    // objects.js addBlock / addCylinder / addMesh / addAsset / addPlane make no GameObject for
    // a noCol ("l" / "col") object: no health, no interface.
    if (object->collision_type == COLLISION_TYPE_NONE) has_health = has_interface = 0;

    if (has_health) {
        // health: parseInt(h || hp) || 0; a health makes it destructible (canDestroy)
        const int h = map_flag_is_set(cJSON_GetObjectItem(raw_obj, "h")) ? map_int(raw_obj, "h") : map_int(raw_obj, "hp");
        object->health = object->start_health = (float) h;
        object->can_destroy = (unsigned char) (map_has(raw_obj, "cdy") || h);
        object->start_destroyed = (unsigned char) (object->can_destroy && map_has(raw_obj, "sd"));
        // GameObject: respawnT under 100 ms (but set) becomes 100
        float respawn = map_float(raw_obj, "rt", 0.0f) * 1000.0f;
        if (respawn != 0.0f && respawn < 100.0f) respawn = 100.0f;
        object->respawn_time = object->respawn_time_start = respawn;
        object->respawn_random = (unsigned char) map_has(raw_obj, "rr");
    }
    if (has_interface) {
        const int id = map_int(raw_obj, "in"); // parseInt(OBJECT.in) || null
        object->interface_id = id && clamp_interface ? map_clamp(id, 0, TRIGGER_INTERFACE_MAX) : id;
    }
    object->destroyed_by = -1;

    switch (prefab_id) {
        case PREFAB_TRIGGER:
            object->trig = map_parse_trigger(raw_obj, object->can_destroy);
            if (object->trig && object->trig->event == TRIGGER_EVENT_ON_ENTER) object->no_shoot = 1; // noShoot: eventN == "onEnter"
            break;
        case PREFAB_GATE:
            object->gate = 1;
            object->score_points = (unsigned int) map_int(raw_obj, "sp");
            object->no_msg = (unsigned char) map_has(raw_obj, "nm");
            object->gate_method = map_int(raw_obj, "md");
            object->closeable = (unsigned char) map_has(raw_obj, "cl");
            object->start_closed = (unsigned char) (map_has(raw_obj, "stc") && object->closeable);
            object->team = (unsigned int) map_int(raw_obj, "tm");
            break;
        case PREFAB_DEPOSIT_BOX: // withdraw / deposit: wd == null ? 1000 : parseInt(wd) || 0, then Math.abs
            object->bank = 1;
            object->withdraw_amount = abs(map_has(raw_obj, "wd") ? map_int(raw_obj, "wd") : 1000);
            object->deposit_amount = abs(map_has(raw_obj, "dp") ? map_int(raw_obj, "dp") : 1000);
            object->team = (unsigned int) map_int(raw_obj, "tm");
            break;
        case PREFAB_CHECK_POINT:
            object->single_use = (unsigned char) map_has(raw_obj, "su");
            object->direction = (unsigned char) (((map_int(raw_obj, "d") % 4) + 4) % 4);
            object->team = (unsigned int) map_int(raw_obj, "tm");
            break;
        case PREFAB_TELEPORTER:
            object->direction = (unsigned char) (((map_int(raw_obj, "d") % 4) + 4) % 4);
            object->force_dir = (unsigned char) map_has(raw_obj, "fd");
            object->channel = map_clamp(map_int(raw_obj, "ch"), 0, 100);
            object->node = map_int(raw_obj, "n");
            object->stop_momentum = (unsigned char) map_has(raw_obj, "m");
            object->team = (unsigned int) map_int(raw_obj, "tm");
            break;
        default:
            break;
    }

    // GameObject active = !startClosed && !startDestroyed (objects.js resetAll)
    object->destroyed = object->start_destroyed;
    if (object->start_closed || object->start_destroyed) object->active = 0;
}

Map *map_init(const cJSON *raw_data) {
    if (!cJSON_IsObject(raw_data)) return NULL;
    // server/game.ts sanitizeCustomMapJson runs on every map before generate(): pooled
    // "si" sizes become "s", objects without p / s go. Idempotent, and it has to edit the
    // document itself because the renderer, shadows and modes read the raw objects too.
    map_sanitize_json((cJSON *) raw_data);

    const cJSON *objects = cJSON_GetObjectItem(raw_data, "objects");
    const cJSON *spawns = cJSON_GetObjectItem(raw_data, "spawns");
    const cJSON *colors = cJSON_GetObjectItem(raw_data, "colors");
    const cJSON *cam_pos = cJSON_GetObjectItem(raw_data, "camPos");
    const cJSON *death_y = cJSON_GetObjectItem(raw_data, "dthY");
    // Krunker 93.js: amb = map.nAmb || obj.ab != null ? null : ambientVal (cube shading gradient)
    const int map_no_ambient = map_flag_is_set(cJSON_GetObjectItem(raw_data, "nAmb"));

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

#ifdef KRUNKNATIVE_CLIENT
    prefab_begin_map(objects, colors);
#endif

    for (int i = 0; i < cJSON_GetArraySize(objects); i++) {
        const cJSON *raw_obj = cJSON_GetArrayItem(objects, i);

        const cJSON *obj_pos = cJSON_GetObjectItem(raw_obj, "p");
        const cJSON *obj_scale = cJSON_GetObjectItem(raw_obj, "s");
        const int prefab_id = map_raw_object_prefab(raw_obj);

        if (!map_raw_object_kept(raw_obj)) continue;

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
        const cJSON *obj_ab = cJSON_GetObjectItem(raw_obj, "ab"); // "ab != null", so ab: 0 counts too
        object->ambient_enabled = !map_no_ambient && (!obj_ab || cJSON_IsNull(obj_ab));
        object->fog_enabled = !map_flag_is_set(cJSON_GetObjectItem(raw_obj, "nf"));
        object->penetrable = map_flag_is_set(cJSON_GetObjectItem(raw_obj, "pe")); // 93.js penetrable: T.pe

        // objects.js noShoot helpers (94.js addLadder/addRamp set noShoot: true as well)
        switch (prefab_id) {
            case PREFAB_FLAG:
            case PREFAB_WEAPON_PICKUP:
            case PREFAB_DEATH_ZONE:
            case PREFAB_SCORE_ZONE:
            case PREFAB_PREMIUM_ZONE:
            case PREFAB_VERIFIED_ZONE:
            case PREFAB_TEAM_ZONE:
            case PREFAB_CHECK_POINT:
            case PREFAB_TELEPORTER:
            case PREFAB_LADDER:
            case PREFAB_RAMP:
                object->no_shoot = 1;
                break;
            default:
                break;
        }

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
                object->team = (unsigned int) map_int(raw_obj, "tm");
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
            case PREFAB_TERMINAL: {
                // map.js addTerminal: noMsg = OBJECT.nm != null, team = parseInt(OBJECT.tm || 0)
                object->terminal = 1;
                const cJSON *nm = cJSON_GetObjectItem(raw_obj, "nm");
                object->no_msg = nm && !cJSON_IsNull(nm);
                object->team = (unsigned int) map_int(raw_obj, "tm");
                break;
            }
            default:
                break;
        }

        map_parse_interface(object, raw_obj, prefab_id); // destructibles, interfaces, gates, banks, triggers, check points, teleporters

#ifdef KRUNKNATIVE_CLIENT
        object->mesh = prefab_init(object, parsed_colors, raw_obj);
        client_animate_object_texture(object, 0.0f);
        object->no_vis = object->mesh && !((Mesh *) object->mesh)->visible; // noVis (v), hidden objectives
        if (object->mesh && !object->active && !object->no_vis) ((Mesh *) object->mesh)->visible = 0; // meshRef.visible = active
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

// objects.js resetAll (+ game.js resetGates / resetBanks) for the next round on the same
// map: start state of the destructibles, gates and banks, trigger storage / triggered.
void map_reset(Map *map) {
    for (size_t i = 0; map && i < map->object_count; i++) {
        Object *object = map->objects[i];
        if (!object->can_destroy && !object->start_health && !object->gate && !object->bank && !object->trig &&
            !object->teleporter) continue;

        object->active = !object->start_closed && !object->start_destroyed;
        object->destroyed = object->start_destroyed;
        object->destroyed_by = -1;
        if (object->start_health) object->health = object->start_health;
        object->respawn_time = object->respawn_time_start;
        object->deposited = 0;
        object->tel_cooldown = 0.0f;
        if (object->trig) {
            object->trig->storage = 0;
            object->trig->triggered = 0;
        }
#ifdef KRUNKNATIVE_CLIENT
        if (object->mesh && !object->no_vis) ((Mesh *) object->mesh)->visible = object->active;
#endif
    }
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
            free(object->trig);

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
