#include <shared.h>
#include <map_catalog.h>
#include <flavor.h> // map_list_with_custom: Krunker Strike rotates its own pool
#include <prefabs.h>
#include <terrain.h> // map_terrain_free (WP-B)
#include <cJSON.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h> // atoi
#include <string.h>

#ifdef KRUNKNATIVE_CLIENT
#include <client.h>
#include <map_post.h> // map_post_object_freed (Object Manager)
#include <user_asset.h> // user_asset_mesh_freed
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

// The official maps by Krunker's mapIndex (the v10.0.2 list: a catalogue index is a position, not a mapIndex;
// shared/src/map_catalog.c). g_rotation_maps and the strike pool are mapIndex lists.
const cJSON *g_maps[MAP_CATALOG_OFFICIAL_COUNT];
// v7.1.4 data/map.js rotationMaps
// v10.0.2 rotationMaps (v10.js:167210): the public rotation, as official mapIndexes.
const int g_rotation_maps[] = {0, 1, 2, 3, 4, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 19, 21, 22, 23, 24, 25, 26, 28,
    32, 33, 34, 36, 37, 39, 40, 42, 44, 45};

// prefabs.prefabs[i || id || 0] || prefabs.prefabs[0] (v7 map.js generate): a JS array index
// is a number or its canonical string; anything else (a prefab *name*, 5.5, true) is undefined.
// The table is v10.0.2's: ids 0-98 (v7.1.4 ended at 96).
static int map_prefab_index(const cJSON *item) {
    const int count = PREFAB_COUNT;
    if (cJSON_IsNumber(item)) {
        const double v = cJSON_GetNumberValue(item);
        return v >= 0.0 && v < count && v == (double) (int) v ? (int) v : -1;
    }
    if (cJSON_IsString(item)) {
        const char *text = cJSON_GetStringValue(item);
        if (!text[0] || (text[0] == '0' && text[1])) return -1;
        int value = 0;
        for (const char *c = text; *c; c++) {
            if (*c < '0' || *c > '9' || value > count) return -1;
            value = value * 10 + (*c - '0');
        }
        return value < count ? value : -1;
    }
    return -1;
}

// JS truthiness of a parsed value (undefined / null / false / 0 / NaN / "" are falsy)
static int map_truthy(const cJSON *item) {
    if (!item || cJSON_IsNull(item) || cJSON_IsFalse(item)) return 0;
    if (cJSON_IsNumber(item)) return cJSON_GetNumberValue(item) != 0.0;
    if (cJSON_IsString(item)) return cJSON_GetStringValue(item)[0] != 0;
    return 1;
}

// The CUBE face switches of a raw map object as v10 reads them (deob-10.0.3.js: the map loader
// at the CUBE addBlock call, `f[i] && f[i][k] != null ? f[k] : 1`, and UTILS.convertFaces for
// strings). Bit i of the result is Krunker face i (0 +X, 1 -X, 2 top, 3 bottom, 4 +Z, 5 -Z).
//  - a hex string (convertFaces): the binary digits are read MSB first, face i = digit i. Values of
//    7 or 8 bits take the top digits. "" is falsy in JS and keeps every face.
//  - a list [1, 1, 1, 1, 1, 0] (4541 objects in the custom maps): element i is the switch of face
//    i; a missing or null element is on, a falsy one (0, false, "") is off.
//  - no "f", a number or any other value: every face (generateCube's default [1,1,1,1,1,1]).
int map_face_mask(const cJSON *raw_faces) {
    if (cJSON_IsString(raw_faces)) {
        const char *s = cJSON_GetStringValue(raw_faces);
        if (!s || !*s) return 0x3f;
        const unsigned long bits = strtoul(s, NULL, 16);
        int length = 6;
        while (length < 64 && (bits >> length) != 0) length++;
        int mask = 0;
        for (int i = 0; i < 6; i++) {
            if ((bits >> (length - 1 - i)) & 1ul) mask |= 1 << i;
        }
        return mask;
    }
    if (cJSON_IsArray(raw_faces)) {
        int mask = 0;
        for (int i = 0; i < 6; i++) {
            const cJSON *item = cJSON_GetArrayItem(raw_faces, i);
            if (!item || cJSON_IsNull(item) || map_truthy(item)) mask |= 1 << i;
        }
        return mask;
    }
    return 0x3f;
}

int map_raw_object_prefab(const cJSON *raw_obj) {
    const cJSON *obj_i = cJSON_GetObjectItem(raw_obj, "i");
    const cJSON *obj_id = cJSON_GetObjectItem(raw_obj, "id");
    // i || id || 0; validation (map_validate.h) rejects custom maps with unknown ids first
    const cJSON *pick = map_truthy(obj_i) ? obj_i : map_truthy(obj_id) ? obj_id : NULL;
    const int prefab_id = pick ? map_prefab_index(pick) : PREFAB_CUBE;
    return prefab_id < 0 ? PREFAB_CUBE : prefab_id;
}

int map_raw_object_kept(const cJSON *raw_obj) {
    const cJSON *obj_pos = cJSON_GetObjectItem(raw_obj, "p");
    const cJSON *obj_scale = cJSON_GetObjectItem(raw_obj, "s");
    if (!cJSON_IsArray(obj_pos) || cJSON_GetArraySize(obj_pos) < 3 || cJSON_GetArraySize(obj_scale) < 3) return 0;
    // TERMINAL (v7: nothing), AI_SPAWNER (addAI: a pending AI, shared/src/ai.c spawns it at game
    // init), BOT_SPAWNER (addBot: a pending map bot), EVENT (client-only official event objects)
    return !prefab_has_role(map_raw_object_prefab(raw_obj), PREFAB_R_NO_OBJECT);
}

// JS truthiness of OBJECT.key ("l", "col", "esa", "h" ...)
static int map_flag_is_set(const cJSON *item) {
    return map_truthy(item);
}

// A JSON value as an int: numbers truncated (NaN / out of range -> 0, never UB), numeric strings
// atoi'd, anything else (missing, bool, null) 0.
static int map_item_int(const cJSON *item) {
    if (cJSON_IsNumber(item)) {
        const double v = cJSON_GetNumberValue(item);
        return v == v && v > -2147483648.0 && v < 2147483648.0 ? (int) v : 0;
    }
    if (cJSON_IsString(item)) return atoi(cJSON_GetStringValue(item));
    return 0;
}

// parseInt(value) || 0 for a number or numeric string field
static int map_int(const cJSON *obj, const char *key) { return map_item_int(cJSON_GetObjectItem(obj, key)); }

void load_default_maps() {
    map_catalog_load_official();
    for (int i = 0; i < MAP_CATALOG_OFFICIAL_COUNT; i++) g_maps[i] = map_catalog_official_json(i);
}

const cJSON **map_list_with_custom(const cJSON *custom_map, size_t *map_count) {
    if (map_count) *map_count = 0;
    if (!custom_map) return NULL;

    load_default_maps();

    // Krunker Strike: the custom map, then FRVR's weighted pool (map_catalog_rotation), not v7's list
    const int strike = flavor_current()->rules != NULL;
    const size_t strike_total = strike ? map_catalog_rotation(NULL, 0) : 0;
    int *strike_pool = strike_total ? malloc(strike_total * sizeof(int)) : NULL;
    const size_t strike_count = strike_pool ? map_catalog_rotation(strike_pool, strike_total) : 0;
    const size_t default_count = strike ? strike_count : sizeof(g_rotation_maps) / sizeof(g_rotation_maps[0]);
    const cJSON **maps = calloc(default_count + 1, sizeof(cJSON *));
    if (!maps) {
        free(strike_pool);
        return NULL;
    }

    maps[0] = custom_map;

    size_t n = 1;
    for (size_t i = 0; i < default_count; i++) {
        const int index = strike ? strike_pool[i] : g_rotation_maps[i];
        if (strike && (index < 0 || index >= MAP_CATALOG_OFFICIAL_COUNT || !g_maps[index])) continue;
        maps[n++] = g_maps[index];
    }
    free(strike_pool);

    if (map_count) *map_count = n;
    return maps;
}

// parseFloat(value) of a number or numeric string, NaN when it is neither
static float map_number(const cJSON *item) {
    if (cJSON_IsNumber(item)) return (float) cJSON_GetNumberValue(item);
    if (cJSON_IsString(item)) {
        const char *text = cJSON_GetStringValue(item);
        char *end = NULL;
        const float value = strtof(text, &end);
        return end != text ? value : NAN;
    }
    return NAN;
}

// parseFloat(value) || fallback
static float map_float(const cJSON *obj, const char *key, const float fallback) {
    const float value = map_number(cJSON_GetObjectItem(obj, key));
    return value == value && value != 0.0f ? value : fallback;
}

static int map_has(const cJSON *obj, const char *key) { // OBJECT.key != null
    const cJSON *item = cJSON_GetObjectItem(obj, key);
    return item && !cJSON_IsNull(item);
}

static int map_clamp(const int value, const int min, const int max) { // UTILS.limitMM
    return value < min ? min : value > max ? max : value;
}

static float map_clampf(const float value, const float min, const float max) { // UTILS.limitMM
    return value < min ? min : value > max ? max : value;
}

static float map_round(const float value, const float scale) { // Number.prototype.round(n): 10^n
    return roundf(value * scale) / scale;
}

// The trigger text check of v7 (tct, inm, ast, nt, welMsg): <= max chars of [\w:;()?,.|[] -+*$@=^!{}].
static int map_text_ok(const char *text, const size_t max) {
    if (!text || !text[0]) return 0;
    size_t length = 0;
    for (const unsigned char *c = (const unsigned char *) text; *c; c++, length++) {
        const int word = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '_';
        if (!word && !strchr(":;()?,.|[] -+*$@=^!{}", *c)) return 0;
    }
    return length <= max;
}

// The raw string of OBJECT.key when it passes map_text_ok, else NULL (points into raw_data).
static const char *map_text(const cJSON *obj, const char *key, const size_t max) {
    const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(obj, key));
    return map_text_ok(text, max) ? text : NULL;
}

// "[a-z0-9_]{0,max}" of a value (v7 .replace(/[^a-z0-9_]+/gmi, "").substring(0, max); the "i"
// flag keeps upper case letters too)
static void map_key_text(const cJSON *item, char *out, const size_t max) {
    size_t n = 0;
    out[0] = 0;
    char number[32];
    const char *text = cJSON_GetStringValue(item);
    if (!text && cJSON_IsNumber(item)) {
        snprintf(number, sizeof(number), "%g", cJSON_GetNumberValue(item));
        text = number;
    }
    for (; text && *text && n < max; text++) {
        const char c = *text;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_') out[n++] = c;
    }
    out[n] = 0;
}

// parseInt of the digits of an asset id ("123e" -> 123, the "at" / "aid" / "skyDomeTexA" letter
// flags stripped); 0 none.
static int map_asset_id(const cJSON *item) {
    if (cJSON_IsNumber(item)) return (int) cJSON_GetNumberValue(item);
    const char *text = cJSON_GetStringValue(item);
    long value = 0;
    for (; text && *text; text++) {
        if (*text >= '0' && *text <= '9' && value < 100000000) value = value * 10 + (*text - '0');
    }
    return (int) value;
}

int map_texture_id(const cJSON *item) {
    // p && !/[^\dtlnesacdgxufqmrpbhv]+/gim.test(p) ? p : 0 (v10 L164733, L159097; v7 MapManager.js:889
    // had /[^\dtlnesacdgxufq]+/)
    if (cJSON_IsNumber(item)) return (int) cJSON_GetNumberValue(item);
    const char *text = cJSON_GetStringValue(item);
    if (!text || !text[0]) return 0;
    static const char letters[] = "tlnesacdgxufqmrpbhv";
    for (const char *c = text; *c; c++) {
        const char lower = (char) (*c >= 'A' && *c <= 'Z' ? *c + 32 : *c);
        if (!(*c >= '0' && *c <= '9') && !strchr(letters, lower)) return 0;
    }
    return map_asset_id(item);
}

// UTILS.isHexColor ? value : fallback
static void map_color(const cJSON *obj, const char *key, const char *fallback, char out[MAP_COLOR_SIZE]) {
    const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(obj, key));
    int ok = text && text[0] == '#' && (strlen(text) == 7 || strlen(text) == 4);
    for (const char *c = text ? text + 1 : NULL; ok && *c; c++) {
        if (!((*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'f') || (*c >= 'A' && *c <= 'F'))) ok = 0;
    }
    snprintf(out, MAP_COLOR_SIZE, "%s", ok ? text : fallback);
}

// key == null ? fallback : parseFloat(key || 0) (the "== null ? default" fields of v7 builders)
static float map_float_or(const cJSON *obj, const char *key, const float fallback) {
    if (!map_has(obj, key)) return fallback;
    const float value = map_number(cJSON_GetObjectItem(obj, key));
    return value == value ? value : 0.0f;
}

// One "tas" entry (MapManager.addTrigger, MapManager.js:1938-2041). The condition values stay raw:
// their sanitize needs triggers.conditions[cd].vals (WP-C).
static void map_parse_trigger_action(TriggerActionData *a, const cJSON *raw) {
    a->action = map_int(raw, "ta");
    a->constant = map_int(raw, "tc");
    a->constant2 = map_int(raw, "tc2");
    a->value_type = map_int(raw, "tce");
    a->condition = map_int(raw, "cd");
    for (int i = 0; i < 3; i++) {
        char key[4] = {'c', 'v', (char) ('0' + i), 0};
        const cJSON *cv = cJSON_GetObjectItem(raw, key);
        a->condition_values[i] = cv && !cJSON_IsNull(cv) ? cv : NULL;
    }
    a->delay = map_round(map_clampf(map_float(raw, "tad", 0.0f), 0.0f, TRIGGER_V7_DELAY_MAX), 10.0f);

    const char *text = map_text(raw, "tct", TRIGGER_TEXT_MAX);
    if (!text && cJSON_IsNumber(cJSON_GetObjectItem(raw, "tct"))) text = NULL; // isString check: numbers drop
    if (text) {
        a->has_text = 1;
        if (a->action == 37 || a->action == 38) map_key_text(cJSON_GetObjectItem(raw, "tct"), a->text, TRIGGER_V7_KEY_MAX);
        else snprintf(a->text, sizeof(a->text), "%s", text);
    }
    // tcc: isString ? [a-z0-9_]{0,10} : undefined
    if (cJSON_IsString(cJSON_GetObjectItem(raw, "tcc"))) map_key_text(cJSON_GetObjectItem(raw, "tcc"), a->value_key, TRIGGER_V7_KEY_MAX);

    // snd = parseInt(esa || es) || 0; assetSnd = esa != null; else emitterSounds[snd] || emitterSounds[0]
    a->asset_sound = (unsigned char) map_has(raw, "esa");
    a->sound = map_truthy(cJSON_GetObjectItem(raw, "esa")) ? map_int(raw, "esa") : map_int(raw, "es");
    if (!a->asset_sound && (a->sound < 0 || a->sound >= TRIGGER_V7_EMITTER_SOUND_COUNT)) a->sound = 0;
    a->volume = map_clampf(map_float(raw, "ev", 1.0f), TRIGGER_EMITTER_VOLUME_MIN, TRIGGER_EMITTER_VOLUME_MAX);
    a->rate = map_clampf(map_float(raw, "er", 1.0f), TRIGGER_V7_EMITTER_RATE_MIN, TRIGGER_EMITTER_RATE_MAX);
    a->emit_from = map_clamp(map_int(raw, "ef"), 0, 2);
}

// map.js TRIGGER -> MapManager.addTrigger / GameObject (shared/include/triggers.h).
static TriggerData *map_parse_trigger(Object *object, const cJSON *raw_obj, const int can_destroy) {
    TriggerData *t = calloc(1, sizeof(TriggerData));
    if (!t) return NULL;

    const int te = map_int(raw_obj, "te");
    // eventN = triggers.events[parseInt(te) || 0]?.name; addTrigger drops the onAI events
    t->event_index = te >= 0 && te < TRIGGER_V7_EVENT_COUNT && !trigger_v7_event_is_ai(te) ? te : -1;
    t->event = t->event_index; // the v7 runtime (shared/src/triggers_v7.c) runs events 0..31
    t->chance = map_clamp(map_int(raw_obj, "cha"), 0, 100);
    t->constant_event = map_clamp(map_int(raw_obj, "tce"), 0, TRIGGER_CONSTANT_EVENT_MAX);
    t->interface_target = map_int(raw_obj, "tin") ? map_clamp(map_int(raw_obj, "tin"), 0, TRIGGER_INTERFACE_MAX) : 0;
    t->action_method = map_int(raw_obj, "tam");
    t->action_cost = map_clamp(map_int(raw_obj, "tac"), 0, TRIGGER_V7_COST_MAX);
    if (t->action_cost && t->event_index != TRIGGER_V7_EVENT_ON_INTERACT_KEY && !t->action_method) t->action_cost = 0;
    t->single_use = (unsigned char) map_has(raw_obj, "su");
    t->single_use_player = (unsigned char) map_has(raw_obj, "sup");
    // v7: selfDestruct needs canDest only (MapManager.js:2059; v3.1.1 also needed singleUse)
    t->self_destruct = (unsigned char) (map_has(raw_obj, "sfd") && can_destroy);
    t->timer = (float) (t->constant_event ? t->constant_event : 1) * 1000.0f; // triggerTimer = (constantEvent || 1) * 1000

    // tas, or the legacy single-action keys wrapped into tas[0] (map.js:538-557, map-logic.md §3.4)
    const cJSON *tas = cJSON_GetObjectItem(raw_obj, "tas");
    const int has_tas = cJSON_IsArray(tas) && cJSON_GetArraySize(tas) > 0;
    const int count = has_tas ? cJSON_GetArraySize(tas) : 1;
    t->actions = calloc((size_t) count, sizeof(TriggerActionData));
    if (t->actions) {
        if (has_tas) {
            const cJSON *entry;
            cJSON_ArrayForEach(entry, tas) {
                if (!cJSON_IsObject(entry)) continue; // if (actions[i]): falsy entries are skipped
                map_parse_trigger_action(&t->actions[t->action_count++], entry);
            }
        } else {
            map_parse_trigger_action(&t->actions[t->action_count++], raw_obj);
            // v7.1.4 copies the object-level "tce" (Event Value) into tas[0].tce, the action's value
            // type, so a v3 trigger with an Event Value and a hasCust action reads a trigger value that
            // does not exist and skips the action. Fixed (doc/v7/decisions.md "v7 bugs"): a wrapped v3
            // action takes its value from "tc" (value type 0, Input Value). No shipped map is affected.
            t->actions[0].value_type = 0;
            object->legacy_trigger = 1;
        }
    }

    // the single-action fields of the v3.1.1-shaped runtime mirror the first action
    if (t->action_count) {
        const TriggerActionData *a = &t->actions[0];
        t->action = a->action;
        t->constant = a->constant;
        if (a->has_text) snprintf(t->text, sizeof(t->text), "%s", a->text);
        t->asset_sound = a->asset_sound;
        t->sound = a->sound;
        t->volume = a->volume;
        t->rate = a->rate;
        t->emit_from = a->emit_from;
    } else {
        t->action = -1; // an empty / all-falsy "tas": no action
        t->volume = t->rate = 1.0f;
    }
    return t;
}

// The map.js / MapManager fields of the interface objects (GameObject health, canDestroy,
// interface, gate, bank, check point, teleporter and trigger data).
static void map_parse_interface(Object *object, const cJSON *raw_obj, const int prefab_id) {
    const PrefabInfo *info = prefab_info(prefab_id);
    const unsigned roles = info ? info->roles : 0;
    int has_health = (roles & PREFAB_R_HEALTH) != 0;
    int has_interface = (roles & PREFAB_R_INTERFACE) != 0;
    const int clamp_interface = (roles & PREFAB_R_INTERFACE_CLAMP) != 0;

    // the builders make no GameObject for a noCol ("l" / "col") object: no health, no interface
    // (the non-solid v7 volumes that are GameObjects keep theirs: PREFAB_R_KEEP_NONSOLID; v10:
    // so does a dyn object, `noCol && !dyn || new GameObject`, L165943 / L166187)
    if (object->collision_type == COLLISION_TYPE_NONE && !(roles & PREFAB_R_KEEP_NONSOLID) && !object->dyn) has_health = has_interface = 0;

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
        // spawnChance: limitMM(parseFloat(rsc) || 0, 0, 100).round(2)
        object->spawn_chance = map_round(map_clampf(map_float(raw_obj, "rsc", 0.0f), 0.0f, 100.0f), 100.0f);
    }
    if (has_interface) {
        const int id = map_int(raw_obj, "in"); // parseInt(OBJECT.in) || null
        object->interface_id = id && clamp_interface ? map_clamp(id, 0, TRIGGER_INTERFACE_MAX) : id;
    }
    object->destroyed_by = -1;
    // complexColl is off for destructibles (addBlock:2489, addCylinder:2350, addAsset, addFixedPlane)
    if (object->can_destroy) object->complex_coll = 0;

    switch (prefab_id) {
        case PREFAB_TRIGGER:
            object->trig = map_parse_trigger(object, raw_obj, object->can_destroy);
            // noShoot: eventN == "onEnterObject" || "onExitObject" (MapManager.js:2075)
            if (object->trig && (object->trig->event_index == TRIGGER_EVENT_ON_ENTER ||
                                 object->trig->event_index == TRIGGER_V7_EVENT_ON_EXIT_OBJECT)) object->no_shoot = 1;
            object->no_player_col = (unsigned char) map_has(raw_obj, "pl");
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
            object->tel_checkpoint = (unsigned char) map_has(raw_obj, "tcp"); // WP-B: telCheckPoint = tcp != null
            break;
        default:
            break;
    }

    // GameObject active = !startClosed && !startDestroyed (MapManager.resetAll)
    object->destroyed = object->start_destroyed;
    if (object->start_closed || object->start_destroyed || object->start_inactive) object->active = 0;
}

// The generic v7 object keys (map-logic.md §1.5.2): map.js generate passes them to every builder
// that reads them; parsing them for every prefab is harmless (the others ignore them).
static void map_parse_generic(Object *object, const cJSON *raw_obj) {
    const cJSON *r = cJSON_GetObjectItem(raw_obj, "r");
    if (cJSON_IsArray(r)) {
        object->rotation.x = map_number(cJSON_GetArrayItem(r, 0));
        object->rotation.y = map_number(cJSON_GetArrayItem(r, 1));
        object->rotation.z = map_number(cJSON_GetArrayItem(r, 2));
        if (object->rotation.x != object->rotation.x) object->rotation.x = 0.0f;
        if (object->rotation.y != object->rotation.y) object->rotation.y = 0.0f;
        if (object->rotation.z != object->rotation.z) object->rotation.z = 0.0f;
    }

    // pe: v7 penetrable = parseFloat(pe) || 0, a 0..1 wallbang fraction (v3: a truthy flag)
    object->penetration = map_clampf(map_float(raw_obj, "pe", 0.0f), 0.0f, 1.0f);
    object->penetrable = object->penetration > 0.0f;

    object->tex_variant = map_truthy(cJSON_GetObjectItem(raw_obj, "tv")) ? map_int(raw_obj, "tv") : 0;
    // at: parseInt((at + "").replace(/[^\d]/g, "")) (NaN -> "0/texture"); a "v" marks a video
    const cJSON *at = cJSON_GetObjectItem(raw_obj, "at");
    object->asset_texture = at && !cJSON_IsNull(at) ? map_asset_id(at) : -1;
    object->asset_video = (unsigned char) (cJSON_IsString(at) && strchr(cJSON_GetStringValue(at), 'v'));

    // tro: parseInt(tro) || 0, limitMM 0..360; a legacy "tr" (radians) wins: tro = toDeg(parseFloat(tr || 0)) || 0
    float tro = (float) map_int(raw_obj, "tro");
    if (map_has(raw_obj, "tr")) tro = (float) (int) (map_float(raw_obj, "tr", 0.0f) * 180.0f / (float) M_PI);
    object->tex_rotation = map_clampf(tro, 0.0f, 360.0f);
    object->tex_scale_mult = map_float(raw_obj, "tsm", 0.0f);
    object->tex_encoding = map_clamp(map_int(raw_obj, "ten"), 0, 2);
    object->frames = map_clamp(map_int(raw_obj, "fct"), 0, 100);
    object->frame_time = object->frames ? map_round(map_clampf(map_float(raw_obj, "fs", 0.0f), 0.0f, 10.0f), 10.0f) * 100.0f : 0.0f;
    object->render_side = map_has(raw_obj, "rs") ? map_clamp(map_int(raw_obj, "rs"), 0, 2) : -1;
    // alphaTest: parseFloat(alt || 0) || 0. The asset branch of addTrigger used parseInt (0.5 -> 0):
    // a v7.1.4 bug, fixed (doc/v7/decisions.md): alt is a float everywhere.
    object->alpha_test = map_round(map_clampf(map_float(raw_obj, "alt", 0.0f), 0.0f, 1.0f), 100.0f);
    object->darken = map_float(raw_obj, "da", 0.0f);
    object->step_sound = map_clamp(map_int(raw_obj, "sat"), 0, 4);
    object->hitbox_h = map_clampf(map_float(raw_obj, "hbh", 1.0f), 0.1f, 10.0f);
    object->hitbox_w = map_clampf(map_float(raw_obj, "hbw", 1.0f), 0.1f, 10.0f);
    object->spin = map_clampf(map_float(raw_obj, "spn", 0.0f), -20.0f, 20.0f);
    object->spin_axis = map_clamp(map_int(raw_obj, "spa"), 0, 1);
    object->anim_speed = map_float(raw_obj, "as", 0.0f);
    object->anim_start = map_text(raw_obj, "ast", 32);
    object->interact_msg = map_text(raw_obj, "inm", TRIGGER_TEXT_MAX);

    object->force_trans = (unsigned char) map_has(raw_obj, "ft");
    object->sprite = (unsigned char) map_has(raw_obj, "msp");
    object->no_ai_col = (unsigned char) map_has(raw_obj, "nai");
    object->no_grapple = (unsigned char) map_has(raw_obj, "gp");
    object->no_impulse = (unsigned char) map_has(raw_obj, "ip");
    object->complex_coll = (unsigned char) map_has(raw_obj, "cpx");
    object->no_minimap = (unsigned char) map_has(raw_obj, "smm");
    object->no_cast = (unsigned char) map_has(raw_obj, "cst");
    object->no_receive = (unsigned char) map_has(raw_obj, "rcv");
    object->force_render = (unsigned char) map_has(raw_obj, "fcr");
    object->can_script = (unsigned char) map_truthy(cJSON_GetObjectItem(raw_obj, "sci"));
    if (map_has(raw_obj, "ns")) object->no_shoot = 1; // noShoot: ns != null

    object->objective_uid = -1;
    object->node_id = -1;

    // v10: "dyn" on the canDynamic prefabs (dyn !== undefined ? !!dyn : undefined, L167488 /
    // L167732 / L168320); "mp" on the custom-asset builders (CUSTOM_ASSET, asset TRIGGER: L167718,
    // L168283)
    const int prefab = map_raw_object_prefab(raw_obj);
    if (prefab_has_flag(prefab, PF_CANDYNAMIC)) object->dyn = (unsigned char) map_truthy(cJSON_GetObjectItem(raw_obj, "dyn"));
    const cJSON *mp = cJSON_GetObjectItem(raw_obj, "mp");
    if ((prefab == PREFAB_CUSTOM_ASSET || prefab == PREFAB_TRIGGER) && cJSON_IsObject(mp)) object->mp = mp;
}

// Prefab-specific keys of the v7 prefabs (map.js generate, MapManager builders).
static void map_parse_prefab_keys(Object *object, const cJSON *raw_obj, const int prefab_id) {
    switch (prefab_id) {
        case PREFAB_BOOST_PAD: {
            // bounce: bm == null ? 1 : parseFloat(bm || 0) || 0 -> limitMM(-3, 3).round(2) (0 allowed;
            // v3.1.1 parseFloat(bm) || 1); crouch: cr == null; direction: limitMM(parseInt(d) || 0, 0, 5)
            object->jump_pad = 1;
            object->bounce = map_has(raw_obj, "bm") ? map_float(raw_obj, "bm", 0.0f) : 1.0f;
            object->bounce = map_round(map_clampf(object->bounce, -3.0f, 3.0f), 100.0f);
            object->crouch = !map_has(raw_obj, "cr");
            object->direction = (unsigned char) map_clamp(map_int(raw_obj, "d"), 0, 5);
            break;
        }
        case PREFAB_OBJECTIVE: {
            // obi 1..10 -> uid obi - 1 (MapManager.addObjective:452-500), else auto in file order
            const int obi = map_int(raw_obj, "obi");
            object->objective_uid = obi >= 1 && obi <= 10 ? obi - 1 : -1;
            break;
        }
        case PREFAB_CAMERA:
            // v7 reads "obl"; a v3.1.1 SPECTATE_CAM's "obi" is not read, so old cams link to 0
            object->obj_link = map_int(raw_obj, "obl");
            object->camera_type = map_clamp(map_int(raw_obj, "camt"), 0, 1);
            break;
        case PREFAB_PATH_NODE:
            object->node_id = map_int(raw_obj, "nid");
            break;
        case PREFAB_KR_ZONE:
            object->kr_zone = 1;
            object->kr_amount = map_clamp(map_int(raw_obj, "krs"), 0, 10000000);
            break;
        case PREFAB_PARTNER_ZONE:
            object->partner_zone = 1;
            break;
        case PREFAB_KPD_ZONE:
            object->kpd_zone = 1;
            break;
        case PREFAB_NOPVP_ZONE:
            object->no_pvp = 1;
            break;
        case PREFAB_PVP_ZONE:
            object->force_pvp = 1;
            break;
        case PREFAB_AI_REDIRECTION:
            // addAIRedirection(x, y, z, sx, sy, sz, (d || 0) * PI / 2, {boost: b}): AIs only
            object->redirection = 1;
            object->direction = (unsigned char) (((map_int(raw_obj, "d") % 4) + 4) % 4);
            object->bounce = map_float(raw_obj, "b", 0.0f);
            break;
        case PREFAB_SERVER_NODE:
            object->server_node = 1;
            object->no_ai_col = 1;
            object->spread = map_clampf(map_float(raw_obj, "ssp", 0.0f), 0.0f, 50.0f);
            object->node_tag = map_text(raw_obj, "nt", 70);
            break;
        case PREFAB_ROPE_LADDER: {
            // GameObject w = l = min(sx, sz) / 2, collT "cylinder" (MapManager.addRopeLadder:1360)
            object->rope_ladder = 1;
            const float side = fminf(object->scale.x, object->scale.z);
            object->scale.x = object->scale.z = side;
            break;
        }
        case PREFAB_MAP_ANCHOR:
            object->anchor_type = map_int(raw_obj, "ant");
            break;
        case PREFAB_PERK_MACHINE:
            object->zombie_perk = map_clamp(map_int(raw_obj, "zp"), 0, 6);
            object->score_points = (unsigned int) map_int(raw_obj, "sp");
            break;
        case PREFAB_MYSTERY_BOX:
            object->start_inactive = 1; // addMysteryBox: active false until switchMysteryBox
            object->score_points = (unsigned int) map_int(raw_obj, "sp");
            break;
        case PREFAB_KRUM_MACHINE:
            object->score_points = (unsigned int) map_int(raw_obj, "sp");
            break;
        case PREFAB_SPRAY:
            object->spray_id = map_truthy(cJSON_GetObjectItem(raw_obj, "spt")) ? map_int(raw_obj, "spt") : 2482;
            if (!object->spray_id) object->spray_id = 2482;
            break;
        default:
            break;
    }
}

// render.js initEnvironment keys of the map (map-logic.md §1.1). A missing ambient / light means
// there is no such light (has_ambient / has_light); the colours keep the editor defaults.
static void map_parse_environment(MapEnvironment *env, const cJSON *raw, const int zone) {
    env->has_ambient = zone || map_has(raw, "ambient");
    env->has_light = zone || map_has(raw, "light");
    map_color(raw, zone ? "ambientC" : "ambient", "#97a0a8", env->ambient);
    map_color(raw, "light", "#f2f8fc", env->light);
    map_color(raw, "sky", "#dce8ed", env->sky);
    map_color(raw, zone ? "fogC" : "fog", "#8d9aa0", env->fog);
    env->ambient_i = map_float_or(raw, "ambientI", 1.0f);
    env->light_i = map_float_or(raw, "lightI", 1.3f);
    env->light_d = map_float_or(raw, "lightD", 500.0f);
    env->sun_ang_x = map_float_or(raw, "sunAngX", 90.0f);
    env->sun_ang_y = map_float_or(raw, "sunAngY", 54.0f);
    env->shadow_r = (int) map_float_or(raw, "shadowR", 1024.0f);
    env->shadow_d = (int) map_float_or(raw, "shadowD", 1200.0f);
    env->fog_d = map_float_or(raw, "fogD", 2000.0f);
    env->sky_dome = (unsigned char) map_truthy(cJSON_GetObjectItem(raw, "skyDome"));
    env->sky_dome_tex = (unsigned char) map_truthy(cJSON_GetObjectItem(raw, "skyDomeTex"));
    map_color(raw, "skyDomeCol0", "#74a4b9", env->sky_dome_col[0]);
    map_color(raw, "skyDomeCol1", "#dce8ed", env->sky_dome_col[1]);
    map_color(raw, "skyDomeCol2", "#dce8ed", env->sky_dome_col[2]);
    map_color(raw, "skyDomeEmis", "#000000", env->sky_dome_emis);
    env->sky_dome_tex_asset = map_texture_id(cJSON_GetObjectItem(raw, "skyDomeTexA"));
    env->sky_dome_emis_tex = map_texture_id(cJSON_GetObjectItem(raw, "skyDomeEmisTex"));
    env->sky_dome_mov_t = map_float(raw, "skyDomeMovT", 0.0f);
    env->sky_dome_mov_d = map_float(raw, "skyDomeMovD", 0.0f);
    env->amb_ind = map_has(raw, "ambInd") ? map_int(raw, "ambInd") : 1;
    env->amb_ind_custom = map_asset_id(cJSON_GetObjectItem(raw, "ambIndC"));
    env->tone_mapping = map_clamp(map_int(raw, "toneMapping"), 0, 4);
    env->tone_mapping_exposure = map_round(map_clampf(map_float_or(raw, "toneMappingExposure", 1.0f), 0.0f, 10.0f), 10.0f);
    env->output_encoding = map_clamp(map_int(raw, "outputEncoding"), 0, 2);
    env->correct_lights = (unsigned char) map_truthy(cJSON_GetObjectItem(raw, "correctLights"));
    // v10 sceneInit: normalBias = isNumber(shadowNB) ? limitMM(shadowNB, 0, 5) : 0 (L201758). The
    // zone data (addEnvironmentZone) has no shadowNB: 0 there.
    env->shadow_nb = !zone && cJSON_IsNumber(cJSON_GetObjectItem(raw, "shadowNB"))
                         ? map_clampf((float) cJSON_GetNumberValue(cJSON_GetObjectItem(raw, "shadowNB")), 0.0f, 5.0f) : 0.0f;
}

// a terrain seed is a string or a number in the corpus: kept as text ("" = none)
static void map_seed_text(const cJSON *item, char *out, const size_t size) {
    out[0] = 0;
    if (cJSON_IsString(item)) snprintf(out, size, "%s", cJSON_GetStringValue(item));
    else if (cJSON_IsNumber(item) && cJSON_GetNumberValue(item) != 0.0) snprintf(out, size, "%.17g", cJSON_GetNumberValue(item));
}

// map.js botPaths (v10 L168471-168486, the v7 loader ignores the key): {pathNodes: p.map(parseInt
// x / y / z || 0), reversible: !!r} for every entry whose p is an array.
static void map_parse_bot_paths(Map *map) {
    const int count = map->bot_paths ? cJSON_GetArraySize(map->bot_paths) : 0;
    if (count <= 0) return;
    map->bot_path_list = calloc((size_t) count, sizeof(MapBotPath));
    if (!map->bot_path_list) return;
    const cJSON *entry;
    cJSON_ArrayForEach(entry, map->bot_paths) {
        const cJSON *nodes = cJSON_GetObjectItem(entry, "p");
        if (!cJSON_IsArray(nodes)) continue;
        MapBotPath *path = &map->bot_path_list[map->bot_path_count++];
        path->reversible = (unsigned char) map_truthy(cJSON_GetObjectItem(entry, "r"));
        const int n = cJSON_GetArraySize(nodes);
        path->nodes = n > 0 ? calloc((size_t) n, sizeof(vec3)) : NULL;
        const cJSON *node;
        cJSON_ArrayForEach(node, nodes) {
            if (!path->nodes) break;
            // parseInt(node[k]) || 0: a number or numeric string, truncated
            path->nodes[path->node_count++] = (vec3) {(float) map_item_int(cJSON_GetArrayItem(node, 0)),
                                                      (float) map_item_int(cJSON_GetArrayItem(node, 1)),
                                                      (float) map_item_int(cJSON_GetArrayItem(node, 2))};
        }
    }
}

// A JSON number array entry k (p / s of an archetype part), fallback when missing or not a number.
static float map_array_number(const cJSON *array, const int k, const float fallback) {
    const float v = map_number(cJSON_GetArrayItem(array, k));
    return cJSON_IsArray(array) && v == v ? v : fallback;
}

// v10 entities.loadMapData(archetypes): one record per entry with an id; the footprint of the
// solid parts (min / max of p -+ s/2 in x / z, max of p.y + s.y), soft parts become soft zones
// (L184118-184170). Validation (map_validate.c) rejects malformed lists for custom maps; this
// keeps whatever loads, like the manager.
static void map_parse_archetypes(Map *map, const cJSON *list) {
    const int count = cJSON_IsArray(list) ? cJSON_GetArraySize(list) : 0;
    if (count <= 0) return;
    map->archetypes = calloc((size_t) count, sizeof(MapArchetype));
    if (!map->archetypes) return;
    const cJSON *entry;
    cJSON_ArrayForEach(entry, list) {
        const cJSON *id = cJSON_GetObjectItem(entry, "id");
        if (!cJSON_IsObject(entry) || !id || cJSON_IsNull(id)) continue;
        MapArchetype *a = &map->archetypes[map->archetype_count++];
        a->id = map_item_int(id);
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(entry, "name"));
        if (name && name[0]) snprintf(a->name, sizeof(a->name), "%s", name);
        else snprintf(a->name, sizeof(a->name), "archetype_%d", a->id);
        a->coll = map_int(entry, "coll");
        a->hp = map_float(entry, "hp", 0.0f);
        const cJSON *data = cJSON_GetObjectItem(entry, "data");
        a->data = cJSON_IsObject(data) ? data : NULL;
        const cJSON *objects = cJSON_GetObjectItem(entry, "objects");
        a->objects = cJSON_IsArray(objects) ? objects : NULL;
        a->object_count = a->objects ? cJSON_GetArraySize(objects) : 0;
        float min_x = 0.0f, max_x = 0.0f, min_z = 0.0f, max_z = 0.0f, height = 0.0f; // the manager starts at 0
        a->soft_zones = a->object_count ? calloc((size_t) a->object_count, sizeof(MapArchetypeSoftZone)) : NULL;
        const cJSON *part;
        cJSON_ArrayForEach(part, a->objects) {
            const cJSON *pp = cJSON_GetObjectItem(part, "p"), *ps = cJSON_GetObjectItem(part, "s");
            const vec3 pos = {map_array_number(pp, 0, 0.0f), map_array_number(pp, 1, 0.0f), map_array_number(pp, 2, 0.0f)};
            const vec3 size = {map_array_number(ps, 0, 2.0f), map_array_number(ps, 1, 2.0f), map_array_number(ps, 2, 2.0f)};
            if (map_truthy(cJSON_GetObjectItem(part, "soft"))) {
                if (a->soft_zones) {
                    a->soft_zones[a->soft_zone_count++] = (MapArchetypeSoftZone) {
                        {pos.x, pos.y + size.y / 2.0f, pos.z}, {size.x / 2.0f, size.y / 2.0f, size.z / 2.0f}};
                }
                continue;
            }
            min_x = fminf(min_x, pos.x - size.x / 2.0f);
            max_x = fmaxf(max_x, pos.x + size.x / 2.0f);
            min_z = fminf(min_z, pos.z - size.z / 2.0f);
            max_z = fmaxf(max_z, pos.z + size.z / 2.0f);
            height = fmaxf(height, pos.y + size.y);
        }
        a->foot_cx = (min_x + max_x) / 2.0f;
        a->foot_cz = (min_z + max_z) / 2.0f;
        a->foot_hx = fmaxf(0.1f, (max_x - min_x) / 2.0f);
        a->foot_hz = fmaxf(0.1f, (max_z - min_z) / 2.0f);
        a->foot_h = fmaxf(0.1f, height);
    }
}

const MapArchetype *map_archetype(const Map *map, const int id) {
    for (size_t i = 0; map && i < map->archetype_count; i++) {
        if (map->archetypes[i].id == id) return &map->archetypes[i];
    }
    return NULL;
}

// map.js ENTITY (L168363-168371): eid != null && ar != null && fewer than 64 instances. The
// manager (loadMapData) then keeps only instances whose archetype exists.
static void map_add_entity_instance(Map *map, const cJSON *raw_obj) {
    const cJSON *eid = cJSON_GetObjectItem(raw_obj, "eid"), *ar = cJSON_GetObjectItem(raw_obj, "ar");
    if (!map_has(raw_obj, "eid") || !map_has(raw_obj, "ar") || map->entity_instance_count >= MAP_ENTITY_LIMIT) return;
    MapEntityInstance e = {0};
    e.eid = map_item_int(eid);
    e.archetype = map_item_int(ar);
    if (!map_archetype(map, e.archetype)) return;
    const cJSON *p = cJSON_GetObjectItem(raw_obj, "p"), *r = cJSON_GetObjectItem(raw_obj, "r");
    e.position = (vec3) {map_array_number(p, 0, 0.0f), map_array_number(p, 1, 0.0f), map_array_number(p, 2, 0.0f)};
    e.rotation = (vec3) {map_array_number(r, 0, 0.0f), map_array_number(r, 1, 0.0f), map_array_number(r, 2, 0.0f)};
    const cJSON *tag = cJSON_GetObjectItem(raw_obj, "tag");
    if (map_has(raw_obj, "tag")) { // (tag + "").slice(0, 32)
        e.has_tag = 1;
        if (cJSON_IsString(tag)) snprintf(e.tag, sizeof(e.tag), "%.32s", cJSON_GetStringValue(tag));
        else if (cJSON_IsNumber(tag)) snprintf(e.tag, sizeof(e.tag), "%g", cJSON_GetNumberValue(tag));
        else snprintf(e.tag, sizeof(e.tag), "%s", cJSON_IsTrue(tag) ? "true" : cJSON_IsFalse(tag) ? "false" : "[object Object]");
    }
    // spawnPending: 0 !== as (missing / any other value spawns)
    const cJSON *as = cJSON_GetObjectItem(raw_obj, "as");
    e.auto_spawn = !(cJSON_IsNumber(as) && cJSON_GetNumberValue(as) == 0.0);
    const cJSON *sp = cJSON_GetObjectItem(raw_obj, "sp");
    e.spawn_params = cJSON_IsObject(sp) ? sp : NULL;
    e.raw = raw_obj;
    MapEntityInstance *grown = realloc(map->entity_instances, (map->entity_instance_count + 1) * sizeof(e));
    if (!grown) return;
    map->entity_instances = grown;
    map->entity_instances[map->entity_instance_count++] = e;
}

static void map_parse_top_level(Map *map, const cJSON *raw) {
    map->name = cJSON_GetStringValue(cJSON_GetObjectItem(raw, "name"));
    map->welcome = map_text(raw, "welMsg", 70);
    map->no_ambient = (unsigned char) map_truthy(cJSON_GetObjectItem(raw, "nAmb"));
    map->ramp_fix = (unsigned char) map_truthy(cJSON_GetObjectItem(raw, "rampFix")); // v10.0.3 Solid Ramp Sides: !!map.rampFix
    map->old_ao = (unsigned char) map_truthy(cJSON_GetObjectItem(raw, "oldAO"));
    map->has_lightmaps = (unsigned char) map_truthy(cJSON_GetObjectItem(raw, "hasLM"));
    map->minimap_asset = map_texture_id(cJSON_GetObjectItem(raw, "mmId"));
    map_parse_environment(&map->env, raw, 0);

    MapTerrainConfig *terrain = &map->terrain;
    map_seed_text(cJSON_GetObjectItem(raw, "terrainSeed"), terrain->seed, sizeof(terrain->seed));
    map_seed_text(cJSON_GetObjectItem(raw, "terrainTSeed"), terrain->tree_seed, sizeof(terrain->tree_seed));
    terrain->width = map_float_or(raw, "terrainWidth", 3000.0f);
    terrain->height = map_float_or(raw, "terrainHeight", 3000.0f);
    terrain->mountain_mlt = map_float_or(raw, "terrainMntMlt", 1.0f);
    terrain->ease = map_has(raw, "terrainEase") ? map_int(raw, "terrainEase") : 12;
    map_color(raw, "terrainMntCol", "#707070", terrain->mountain_col);
    map_color(raw, "terrainSnwCol", "#e8fdff", terrain->snow_col);
    map_color(raw, "terrainGrsCol", "#5e692f", terrain->grass_col);
    map_color(raw, "terrainDrtCol", "#7f6238", terrain->dirt_col);
    map_color(raw, "terrainSndCol", "#967f51", terrain->sand_col);
    // map.js: built only when terrainSeed && terrainWidth && terrainHeight (raw values, truthy)
    terrain->enabled = map_truthy(cJSON_GetObjectItem(raw, "terrainSeed")) && map_truthy(cJSON_GetObjectItem(raw, "terrainWidth")) &&
                       map_truthy(cJSON_GetObjectItem(raw, "terrainHeight"));

    MapZoneConfig *zone = &map->zone;
    zone->enabled = map_truthy(cJSON_GetObjectItem(raw, "zone")) && map_truthy(cJSON_GetObjectItem(raw, "zoneSize")) &&
                    map_truthy(cJSON_GetObjectItem(raw, "zoneSpeed"));
    zone->random = (unsigned char) map_truthy(cJSON_GetObjectItem(raw, "zoneRnd"));
    zone->size = map_float_or(raw, "zoneSize", 500.0f);
    zone->speed = map_float_or(raw, "zoneSpeed", 1.0f);
    zone->height = map_float_or(raw, "zoneHeight", 1000.0f);
    zone->wait = map_float_or(raw, "zoneWait", 5.0f);
    zone->stages = map_clamp(map_int(raw, "zoneStages"), 0, 10);
    map_color(raw, "zoneCol0", "#ff6600", zone->col[0]);
    map_color(raw, "zoneCol1", "#ff0000", zone->col[1]);
    map_color(raw, "zoneCol2", "#ffff00", zone->col[2]);

    MapOceanConfig *ocean = &map->ocean;
    ocean->enabled = (unsigned char) map_truthy(cJSON_GetObjectItem(raw, "ocean"));
    map_color(raw, "ocCol", "#001e0f", ocean->col);
    ocean->opacity = map_float_or(raw, "ocOpac", 0.9f);
    ocean->distortion = map_float_or(raw, "ocDist", 3.7f);
    ocean->speed = map_float_or(raw, "ocSpd", 1.0f);
    ocean->y_offset = map_float_or(raw, "ocYOff", -1.0f);
    ocean->reflection_res = map_has(raw, "ocRefRes") ? map_int(raw, "ocRefRes") : 3;
    ocean->width = map_float_or(raw, "ocW", 1000.0f);
    ocean->length = map_float_or(raw, "ocL", 1000.0f);

    const cJSON *modes = cJSON_GetObjectItem(raw, "modes");
    if (cJSON_IsArray(modes) && cJSON_GetArraySize(modes) > 0) {
        map->modes = calloc((size_t) cJSON_GetArraySize(modes), sizeof(int));
        const cJSON *m;
        cJSON_ArrayForEach(m, modes) {
            if (map->modes && cJSON_IsNumber(m)) map->modes[map->mode_count++] = (int) cJSON_GetNumberValue(m);
        }
    }

    // tris: pendingTriangles = tris when it is a non-empty array; 9 floats per triangle
    const cJSON *tris = cJSON_GetObjectItem(raw, "tris");
    const int tri_count = cJSON_IsArray(tris) ? cJSON_GetArraySize(tris) / 9 : 0;
    if (tri_count > 0) {
        map->tris = calloc((size_t) tri_count * 9, sizeof(float));
        if (map->tris) {
            int n = 0;
            const cJSON *v;
            cJSON_ArrayForEach(v, tris) {
                if (n >= tri_count * 9) break;
                const float f = map_number(v);
                map->tris[n++] = f == f ? f : 0.0f;
            }
            map->tri_count = (size_t) tri_count;
        }
    }

    const cJSON *item = cJSON_GetObjectItem(raw, "config");
    map->game_config = cJSON_IsObject(item) ? item : NULL;
    item = cJSON_GetObjectItem(raw, "gui");
    map->gui = cJSON_IsArray(item) ? item : NULL;
    item = cJSON_GetObjectItem(raw, "scripts");
    map->scripts = cJSON_IsObject(item) ? item : NULL;
    item = cJSON_GetObjectItem(raw, "botPaths");
    map->bot_paths = cJSON_IsArray(item) ? item : NULL;
    map_parse_bot_paths(map);
    item = cJSON_GetObjectItem(map->scripts, "shared");
    map->shared_script = cJSON_IsString(item) ? item : NULL;
    map_parse_archetypes(map, cJSON_GetObjectItem(raw, "archetypes"));
    item = cJSON_GetObjectItem(raw, "player");
    map->editor_player = cJSON_IsObject(item) ? item : NULL;
}

// appends value to a calloc'd / realloc'd array of `size` byte elements
static int map_push(void **array, size_t *count, const void *value, const size_t size) {
    unsigned char *grown = realloc(*array, (*count + 1) * size);
    if (!grown) return 0;
    memcpy(grown + *count * size, value, size);
    *array = grown;
    (*count)++;
    return 1;
}

// The lists the v7 managers keep besides objects (pathNodes, serverNodes, mapAnchors, domzones,
// objectives, envZones; pending AIs / bots).
static void map_index_object(Map *map, const Object *object, const cJSON *raw_obj, const size_t index) {
    switch (object->prefab) {
        case PREFAB_PATH_NODE: {
            const MapPathNode node = {object->node_id, index};
            map_push((void **) &map->path_nodes, &map->path_node_count, &node, sizeof(node));
            break;
        }
        case PREFAB_SERVER_NODE:
            map_push((void **) &map->server_nodes, &map->server_node_count, &index, sizeof(index));
            break;
        case PREFAB_MAP_ANCHOR:
            map_push((void **) &map->map_anchors, &map->map_anchor_count, &index, sizeof(index));
            break;
        case PREFAB_DOMINATION_ZONE:
            map_push((void **) &map->dom_zones, &map->dom_zone_count, &index, sizeof(index));
            break;
        case PREFAB_OBJECTIVE:
            map_push((void **) &map->objectives, &map->objective_count, &index, sizeof(index));
            break;
        case PREFAB_ENVIRONMENT_ZONE: {
            MapEnvironmentZone zone = {0};
            zone.position = object->position;
            zone.size = object->scale;
            map_parse_environment(&zone.env, raw_obj, 1);
            map_push((void **) &map->env_zones, &map->env_zone_count, &zone, sizeof(zone));
            break;
        }
        default:
            break;
    }
}

void map_object_rotation(const Object *object, float out[9]) {
    // three.js Euler (a, b, c) order "XYZ": R = Rx(a) * Ry(b) * Rz(c)
    float x = 0.0f, y = 0.0f, z = 0.0f;
    int plane = 0;
    const vec3 r = object ? object->rotation : (vec3) {0};
    switch (object ? (int) object->prefab : -1) {
        case PREFAB_PLANE: case PREFAB_BILLBOARD: case PREFAB_SIGN: case PREFAB_LIQUID: case PREFAB_SPRITE:
        case PREFAB_LIGHT_BAR: case PREFAB_SPRAY: case PREFAB_LEADERBOARD: case PREFAB_RUNE:
            // render.js similateRot: parent rotateX(r0) rotateY(r1) rotateZ(r2), child rotateX(-PI/2)
            // (RUNE too: map.js passes r[0..2] to addFixedPlane; map-G fix, it was unrotated)
            if (object->v3_plane_rotation) {
                // v3.1.1 93.js addPlane(..., yR = r2, xR = r0 - PI/2, zR = r1) -> 94.js xR + PI/2 ->
                // 55.js rotateY(yR) rotateX(xR - PI/2) rotateZ(zR): Ry(r2) * Rx(r0 - PI/2) * Rz(r1)
                const float a = r.x - (float) M_PI / 2.0f, b = r.z, c = r.y;
                const float ca = cosf(a), sa = sinf(a), cb = cosf(b), sb = sinf(b), cc = cosf(c), sc = sinf(c);
                const float yxz[9] = {
                    cb * cc + sb * sa * sc, sb * sa * cc - cb * sc, sb * ca,
                    ca * sc, ca * cc, -sa,
                    cb * sa * sc - sb * cc, sb * sc + cb * sa * cc, cb * ca,
                };
                memcpy(out, yxz, sizeof(yxz));
                return;
            }
            x = r.x, y = r.y, z = r.z;
            plane = 1;
            break;
        case PREFAB_BOOST_PAD:
            x = r.x, y = r.y, z = r.z; // xR = r0, yR = r1, zR = r2
            break;
        case PREFAB_RECTAREA_LIGHT: {
            // render.js addRectAreaLight(..., r[2], r[0], r[1]): rotateY(r2) rotateX(r0) rotateZ(r1),
            // i.e. Euler "YXZ" R = Ry(r2) * Rx(r0) * Rz(r1) (map-G fix: was XYZ(r2, r0, r1))
            const float a = r.x, b = r.y, c = r.z; // r0, r1, r2
            const float ca = cosf(a), sa = sinf(a), cb = cosf(b), sb = sinf(b), cc = cosf(c), sc = sinf(c);
            const float yxz[9] = {
                cc * cb + sc * sa * sb, sc * sa * cb - cc * sb, sc * ca,
                ca * sb, ca * cb, -sa,
                cc * sa * sb - sc * cb, sb * sc + cc * sa * cb, cc * ca,
            };
            memcpy(out, yxz, sizeof(yxz));
            return;
        }
        default:
            // map.js "yR: r[0], xR: r[1], zR: r[2]", but the builders call mesh.rotation.set(yR, xR, zR)
            // (render.js addCube / addCylinder / addCone / addSphere) and loadMesh copies the r array
            // into rotation.x/y/z, so the Euler is XYZ (r0, r1, r2) (map-G fix: was (r1, r0, r2))
            x = r.x, y = r.y, z = r.z;
            break;
    }
    const float cx = cosf(x), sx = sinf(x), cy = cosf(y), sy = sinf(y), cz = cosf(z), sz = sinf(z);
    float m[9] = {
        cy * cz, -cy * sz, sy,
        cx * sz + sx * sy * cz, cx * cz - sx * sy * sz, -sx * cy,
        sx * sz - cx * sy * cz, sx * cz + cx * sy * sz, cx * cy,
    };
    if (plane) {
        // * Rx(-PI/2): columns (c0, c1, c2) -> (c0, -c2, c1)
        for (int row = 0; row < 3; row++) {
            const float c1 = m[row * 3 + 1], c2 = m[row * 3 + 2];
            m[row * 3 + 1] = -c2;
            m[row * 3 + 2] = c1;
        }
    }
    memcpy(out, m, sizeof(m));
}

// Render-only fix for Atomic (assets/maps/official/23_atomic.json, mmId 52269, unique among the
// official files). Its ground slabs are CUBEs with the top-only face mask "8" (Krunker face 2), so
// geos.js omits the bottom face: a collidable slab is invisible from below and players bump into it.
// With this flag prefab.c also draws the bottom ("c") of each collidable CUBE whose mask is top-only
// and whose height is at most MAP_SLAB_UNDERSIDE_MAX_HEIGHT (Atomic's 14 slabs are 0.01 to 3 units
// high; its other top-only cubes are non-collidable or 77 units tall, and stay as they are).
// Collision, the top face and every other map are unchanged. The map's own mmId picks the map, so
// no coordinates are needed.
#define MAP_SLAB_UNDERSIDE_MMID 52269
#define MAP_SLAB_UNDERSIDE_MAX_HEIGHT 4.0f

// One kept map object record -> a new Object, not yet in any map (map_init_ex's loop, and the
// KrunkNative Object Manager's run-time records through map_object_from_json). `position` /
// `scale` get the record's rounded p / s (before the ladder offsets). NULL: out of memory.
static Object *map_parse_object(const cJSON *raw_obj, const vec4 *parsed_colors, const int map_no_ambient, const int v3_planes, const int slab_undersides,
                                const int build_meshes, vec3 *out_position, vec3 *out_scale) {
    (void) parsed_colors;
    (void) build_meshes;
    const cJSON *obj_pos = cJSON_GetObjectItem(raw_obj, "p");
    const cJSON *obj_scale = cJSON_GetObjectItem(raw_obj, "s");
    const int prefab_id = map_raw_object_prefab(raw_obj);

    const PrefabInfo *info = prefab_info(prefab_id);
    Object *object = calloc(1, sizeof(Object));
    if (!object) return NULL;

    // v7 map.js: p = limit(p, positionRange).round(2), s = (parseFloat(s) || 0).round(2)
    vec3 position, scale;
    float *pv = &position.x, *sv = &scale.x;
    for (int k = 0; k < 3; k++) {
        const float p = map_number(cJSON_GetArrayItem(obj_pos, k));
        const float sz = map_number(cJSON_GetArrayItem(obj_scale, k));
        pv[k] = map_round(map_clampf(p == p ? p : 0.0f, -500000.0f, 500000.0f), 100.0f);
        sv[k] = map_round(sz == sz ? sz : 0.0f, 100.0f);
    }

    object->active = 1;
    object->raw = raw_obj;
    object->position = position;
    object->scale = scale;
    object->prefab = prefab_id;
    // the v3.1.1 addPlane objects of a 311_ / 152_ map (map_object_rotation), before the mesh is built
    object->v3_plane_rotation = (unsigned char) (v3_planes && (prefab_id == PREFAB_PLANE || prefab_id == PREFAB_BILLBOARD ||
                                                               prefab_id == PREFAB_LIQUID || prefab_id == PREFAB_RUNE));
    map_parse_generic(object, raw_obj);
    const cJSON *obj_ab = cJSON_GetObjectItem(raw_obj, "ab"); // "ab != null", so ab: 0 counts too
    object->ambient_enabled = !map_no_ambient && (!obj_ab || cJSON_IsNull(obj_ab));
    object->fog_enabled = !map_has(raw_obj, "nf"); // noFog: nf != null
    // noVis: v == null ? undefined : 1, passed to every rendered builder but addCanvas (SIGN) /
    // addShowcase / particles. Parsed here so server_bin (no meshes) knows it too: players.spray
    // skips noVis objects.
    object->no_vis = prefab_id != PREFAB_SIGN && prefab_id != PREFAB_SHOWCASE && prefab_id != PREFAB_PARTICLES &&
                     map_has(raw_obj, "v");

    if (info && (info->roles & PREFAB_R_NO_SHOOT)) object->no_shoot = 1;

    map_parse_prefab_keys(object, raw_obj, prefab_id);

    const cJSON *dir = cJSON_GetObjectItem(raw_obj, "d");

    if (prefab_id == PREFAB_LADDER) {
        object->ladder = 1;
        object->direction = cJSON_IsNumber(dir) ? (unsigned char) ((((int) cJSON_GetNumberValue(dir) % 4) + 4) % 4) : 0;

        const float direction = (float) M_PI * 0.5f * (float) object->direction;

        object->position.x += game_constants.ladder_scale * cosf(direction);
        object->position.z += game_constants.ladder_scale * sinf(direction);

        object->scale.x = (object->direction % 2 ? game_constants.ladder_width : game_constants.ladder_scale) * 2.0f;
        object->scale.z = (object->direction % 2 ? game_constants.ladder_scale : game_constants.ladder_width) * 2.0f;
    }

    if (prefab_id == PREFAB_RAMP) {
        const cJSON *boost = cJSON_GetObjectItem(raw_obj, "b");

        Ramp *ramp = calloc(1, sizeof(Ramp));
        if (!ramp) {
            free(object);
            return NULL;
        }

        object->direction = cJSON_IsNumber(dir) ? (unsigned char) ((((int) cJSON_GetNumberValue(dir) % 4) + 4) % 4) : 0;

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

    const int no_collisions = map_truthy(cJSON_GetObjectItem(raw_obj, "l")) || map_truthy(cJSON_GetObjectItem(raw_obj, "col"));

    // isBorder: bo == null ? undefined : 1 (the prefabs whose builders take it)
    if (info && (info->roles & PREFAB_R_BORDER)) object->is_border = (unsigned char) map_has(raw_obj, "bo");

    switch (info ? info->collision : PREFAB_COL_NONE) {
        case PREFAB_COL_NONE:
            object->collision_type = COLLISION_TYPE_NONE;
            break;
        case PREFAB_COL_FORCED:
            object->collision_type = COLLISION_TYPE_BOX;
            break;
        case PREFAB_COL_FORCED_CYLINDER:
            object->collision_type = COLLISION_TYPE_CYLINDER;
            break;
        case PREFAB_COL_OPTIONAL_CYLINDER:
            object->collision_type = no_collisions ? COLLISION_TYPE_NONE : COLLISION_TYPE_CYLINDER;
            break;
        default:
            object->collision_type = no_collisions ? COLLISION_TYPE_NONE : COLLISION_TYPE_BOX;
            break;
    }

    // the slab underside fix (MAP_SLAB_UNDERSIDE_MMID): a collidable thin CUBE; prefab.c checks its mask
    object->slab_underside = slab_undersides && prefab_id == PREFAB_CUBE && object->collision_type == COLLISION_TYPE_BOX &&
                             object->scale.y <= MAP_SLAB_UNDERSIDE_MAX_HEIGHT;

    // notWallJ: wj != null; the non-solid prefabs and the own-branch ones never wall-jump
    object->wall_jumpable = info && !(info->roles & PREFAB_R_NO_WALL_JUMP) && !map_has(raw_obj, "wj");

    switch (prefab_id) {
        case PREFAB_VERIFIED_ZONE:
            object->verified = 1;
            break;
        case PREFAB_PREMIUM_ZONE:
            object->premium = 1;
            break;
        case PREFAB_TEAM_ZONE:
            object->team_zone = 1;
            object->team = (unsigned int) map_int(raw_obj, "tm");
            break;
        case PREFAB_SCORE_ZONE:
            object->score_zone = 1;
            // addScoreZone(..., parseInt(sp) || 1)
            object->score_points = (unsigned int) (map_int(raw_obj, "sp") ? map_int(raw_obj, "sp") : 1);
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
        default:
            break;
    }

    map_parse_interface(object, raw_obj, prefab_id); // destructibles, interfaces, gates, banks, triggers, check points, teleporters

#ifdef KRUNKNATIVE_CLIENT
    if (build_meshes) {
        object->mesh = prefab_init(object, parsed_colors, raw_obj);
        client_animate_object_texture(object, 0.0f);
        object->no_vis = object->no_vis || (object->mesh && !((Mesh *) object->mesh)->visible); // + meshes created hidden (objectives)
        if (object->mesh && !object->active && !object->no_vis) ((Mesh *) object->mesh)->visible = 0; // meshRef.visible = active
    }
#endif

    *out_position = position;
    *out_scale = scale;
    return object;
}

Map *map_init(const cJSON *raw_data) {
    return map_init_ex(raw_data, 1);
}

Map *map_init_ex(const cJSON *raw_data, const int build_meshes) {
    (void) build_meshes; // only the client build has meshes
    if (!cJSON_IsObject(raw_data)) return NULL;
    // server/game.ts sanitizeCustomMapJson / v7 map.js: pooled "si" sizes become "s", objects
    // without p / s go. Idempotent, and it has to edit the document itself because the renderer,
    // shadows and modes read the raw objects too.
    map_sanitize_json((cJSON *) raw_data);

    const cJSON *objects = cJSON_GetObjectItem(raw_data, "objects");
    const cJSON *spawns = cJSON_GetObjectItem(raw_data, "spawns");
    const cJSON *colors = cJSON_GetObjectItem(raw_data, "colors");
    const cJSON *cam_pos = cJSON_GetObjectItem(raw_data, "camPos");
    const cJSON *death_y = cJSON_GetObjectItem(raw_data, "dthY");
    // Krunker 93.js: amb = map.nAmb || obj.ab != null ? null : ambientVal (cube shading gradient)
    const int map_no_ambient = map_flag_is_set(cJSON_GetObjectItem(raw_data, "nAmb"));
    const int v3_planes = cJSON_IsTrue(cJSON_GetObjectItem(raw_data, MAP_KEY_V3_PLANES));

    if (!cJSON_IsArray(objects) || !cJSON_IsArray(spawns)) return NULL;

    vec4 *parsed_colors = cJSON_IsArray(colors) ? calloc(cJSON_GetArraySize(colors), sizeof(vec4)) : NULL;
    Map *map = calloc(1, sizeof(Map));

    if (!map) {
        free(parsed_colors);
        return NULL;
    }

    map->raw_data = raw_data;
    map->config = g_default_map_config; // v3.1.1 map.game is ignored, as by the v7 runtime (§1.9)
    map->death_y = cJSON_IsNumber(death_y) ? (float) cJSON_GetNumberValue(death_y) : game_constants.death_y;
    map_parse_top_level(map, raw_data);
    const int slab_undersides = map->minimap_asset == MAP_SLAB_UNDERSIDE_MMID;

    if (cJSON_IsArray(cam_pos) && cJSON_GetArraySize(cam_pos) >= 3) {
        map->camera_position.x = (float) cJSON_GetNumberValue(cJSON_GetArrayItem(cam_pos, 0));
        map->camera_position.y = (float) cJSON_GetNumberValue(cJSON_GetArrayItem(cam_pos, 1));
        map->camera_position.z = (float) cJSON_GetNumberValue(cJSON_GetArrayItem(cam_pos, 2));
        // v7: [3] static menu camera, rotateCam(p5, p4, p6); a 3-element v3 camPos: false, rotation 0
        map->camera_static = (unsigned char) map_truthy(cJSON_GetArrayItem(cam_pos, 3));
        const float rx = map_number(cJSON_GetArrayItem(cam_pos, 4));
        const float ry = map_number(cJSON_GetArrayItem(cam_pos, 5));
        const float rz = map_number(cJSON_GetArrayItem(cam_pos, 6));
        map->camera_rotation = (vec3) {rx == rx ? rx : 0.0f, ry == ry ? ry : 0.0f, rz == rz ? rz : 0.0f};
    }

    if (parsed_colors) {
        int i = 0;
        for (const cJSON *item = colors->child; item; item = item->next, i++) {
            const char *hex = cJSON_GetStringValue(item);
            vec4 color = {1.0f, 1.0f, 1.0f, 1.0f};

            if (hex) parse_hex_color(hex, &color);
            parsed_colors[i] = color;
        }
    }

    for (int i = 0; i < cJSON_GetArraySize(spawns); i++) {
        const cJSON *raw_spawn = cJSON_GetArrayItem(spawns, i);
        if (!cJSON_IsArray(raw_spawn) || cJSON_GetArraySize(raw_spawn) < 3) continue;

        const vec3 position = {
            (float) cJSON_GetNumberValue(cJSON_GetArrayItem(raw_spawn, 0)),
            (float) cJSON_GetNumberValue(cJSON_GetArrayItem(raw_spawn, 1)),
            (float) cJSON_GetNumberValue(cJSON_GetArrayItem(raw_spawn, 2)),
        };

        // v7 map.js:1474 keeps spawn[3] as is: a 3-element row has team undefined (falsy = any team).
        // cJSON_GetNumberValue would give NaN there, and (int) NaN is undefined behaviour.
        const int team = map_item_int(cJSON_GetArrayItem(raw_spawn, 3));
        const int dir = map_item_int(cJSON_GetArrayItem(raw_spawn, 4)); // parseInt(spawn[4]) || 0
        const int comp = map_truthy(cJSON_GetArrayItem(raw_spawn, 5));  // !!spawn[5]

        Spawn *spawn = calloc(1, sizeof(Spawn));
        if (!spawn) continue;

        spawn->position = position;
        spawn->team = team;
        spawn->direction = dir;
        spawn->comp = comp;
        // v7 map.js:1466-1480: teamOnly = !!spawn[6], spread = limitMM(parseFloat(spawn[7]) || 0, 0, 50)
        spawn->team_only = map_truthy(cJSON_GetArrayItem(raw_spawn, 6));
        const float spread = map_number(cJSON_GetArrayItem(raw_spawn, 7));
        spawn->spread = spread == spread ? map_clampf(spread, 0.0f, 50.0f) : 0.0f;

        Spawn **new_spawns = realloc(map->spawns, (map->spawn_count + 1) * sizeof(Spawn *));
        if (!new_spawns) {
            free(spawn);
            continue;
        }
        map->spawns = new_spawns;
        map->spawns[map->spawn_count++] = spawn;
    }

#ifdef KRUNKNATIVE_CLIENT
    if (build_meshes) prefab_begin_map(objects, colors);
#endif

    // walk the list: cJSON_GetArrayItem(objects, i) starts at the head every time (quadratic)
    for (const cJSON *raw_obj = objects->child; raw_obj; raw_obj = raw_obj->next) {
        const int prefab_id = map_raw_object_prefab(raw_obj);

        if (prefab_id == PREFAB_AI_SPAWNER) {
            map_push((void **) &map->ai_spawners, &map->ai_spawner_count, &raw_obj, sizeof(raw_obj));
        } else if (prefab_id == PREFAB_BOT_SPAWNER) {
            map_push((void **) &map->bot_spawners, &map->bot_spawner_count, &raw_obj, sizeof(raw_obj));
        } else if (prefab_id == PREFAB_ENTITY) {
            map_add_entity_instance(map, raw_obj); // v10: an instance for the entity manager (ks_entities.c), no map object
        }
        if (!map_raw_object_kept(raw_obj)) continue;

        vec3 position, scale;
        Object *object = map_parse_object(raw_obj, parsed_colors, map_no_ambient, v3_planes, slab_undersides, build_meshes, &position, &scale);
        if (!object) continue;

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

        Object **new_objects = realloc(map->objects, sizeof(Object *) * (map->object_count + 1));

        if (!new_objects) {
            free(object->trig ? object->trig->actions : NULL);
            free(object->trig);
            free(object->ramp);
            free(object);
            continue;
        }

        map->objects = new_objects;
        map->objects[map->object_count++] = object;
        map_index_object(map, object, raw_obj, map->object_count - 1);
    }

    if (parsed_colors) free(parsed_colors);
    return map;
}

// MapManager.resetAll (+ game.js resetGates / resetBanks) for the next round on the same
// map: start state of the destructibles, gates and banks, trigger storage / triggered.
void map_reset(Map *map) {
    for (size_t i = 0; map && i < map->object_count; i++) {
        Object *object = map->objects[i];
        if (!object->can_destroy && !object->start_health && !object->gate && !object->bank && !object->trig &&
            !object->teleporter) continue;

        object->active = !object->start_closed && !object->start_destroyed && !object->start_inactive;
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

Object *map_object_from_json(const Map *map, const cJSON *raw_obj, const int build_meshes) {
    if (!cJSON_IsObject(raw_obj) || !map_raw_object_kept(raw_obj)) return NULL;
    const int map_no_ambient = map && map_flag_is_set(cJSON_GetObjectItem(map->raw_data, "nAmb"));
    const int v3_planes = map && cJSON_IsTrue(cJSON_GetObjectItem(map->raw_data, MAP_KEY_V3_PLANES));
    const int slab_undersides = map && map->minimap_asset == MAP_SLAB_UNDERSIDE_MMID;
    vec3 position, scale;
    return map_parse_object(raw_obj, NULL, map_no_ambient, v3_planes, slab_undersides, build_meshes, &position, &scale);
}

void map_object_free(Object *object) {
    if (!object) return;
    if (object->tex_anim) free(object->tex_anim);
    free(object->ramp);
    if (object->trig) free(object->trig->actions);
    free(object->trig);
#ifdef KRUNKNATIVE_CLIENT
    if (object->mesh) {
        map_post_object_freed(object); // map_post.c: a run-time object's mesh leaves the scene
        user_asset_mesh_freed(object->mesh); // its GLTF model instance (user_asset.c)
        mesh_fini(object->mesh);
    }
#endif
    free(object);
}

void map_fini(Map *map) {
    if (map->spawn_count) {
        for (size_t i = 0; i < map->spawn_count; i++) free(map->spawns[i]);
    }
    free(map->spawns);
    map->spawn_count = 0;
    map->spawns = NULL;

    if (map->object_count) {
        for (size_t i = 0; i < map->object_count; i++) {
            Object *object = map->objects[i];

            if (object->tex_anim) free(object->tex_anim);
            if (object->ramp) free(object->ramp);
            if (object->trig) free(object->trig->actions);
            free(object->trig);

#ifdef KRUNKNATIVE_CLIENT
            if (object->mesh) {
                user_asset_mesh_freed(object->mesh); // its GLTF model instance (user_asset.c)
                mesh_fini(object->mesh);
            }
#endif

            free(object);
        }
    }
    free(map->objects);
    map->object_count = 0;
    map->objects = NULL;

    map_terrain_free(map); // WP-B: the cached map.terrain
    free(map->modes);
    free(map->tris);
    free(map->path_nodes);
    free(map->server_nodes);
    free(map->map_anchors);
    free(map->dom_zones);
    free(map->objectives);
    free(map->env_zones);
    free((void *) map->ai_spawners);
    free((void *) map->bot_spawners);
    for (size_t i = 0; i < map->bot_path_count; i++) free(map->bot_path_list[i].nodes);
    free(map->bot_path_list);
    for (size_t i = 0; i < map->archetype_count; i++) free(map->archetypes[i].soft_zones);
    free(map->archetypes);
    free(map->entity_instances);
    map->bot_path_list = NULL;
    map->archetypes = NULL;
    map->entity_instances = NULL;
    map->bot_path_count = map->archetype_count = map->entity_instance_count = 0;
    map->shared_script = NULL;
    map->modes = NULL;
    map->tris = NULL;
    map->path_nodes = NULL;
    map->server_nodes = map->map_anchors = map->dom_zones = map->objectives = NULL;
    map->env_zones = NULL;
    map->ai_spawners = map->bot_spawners = NULL;
    map->mode_count = map->tri_count = map->path_node_count = map->server_node_count = map->map_anchor_count = 0;
    map->dom_zone_count = map->objective_count = map->env_zone_count = map->ai_spawner_count = map->bot_spawner_count = 0;
}
