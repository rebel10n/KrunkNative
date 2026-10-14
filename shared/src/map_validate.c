// Krunker v7.1.4 custom map validation (shared/include/map_validate.h): customGameConfig.js
// validateMap (:152-368) and the prepareGame checks (game.js:606-678), in v7's order.
#include <map_validate.h>
#include <prefabs.h>
#include <trigger_v7_data.h>
#include <ruleset.h> // config.triggerActionLimit, entityLimit, archetypeLimit

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const MapValidateOptions *opt;
    MapIssue *issues;
    size_t max, count, errors;
    int stop; // first_only and an error was found
} Report;

static void report(Report *r, const MapIssueSeverity severity, const int object, const int action, const char *code,
                   const char *format, ...) {
    if (r->stop) return;
    if (severity == MAP_ISSUE_WARNING && r->opt->no_warnings) return;
    if (r->count < r->max && r->issues) {
        MapIssue *issue = &r->issues[r->count];
        memset(issue, 0, sizeof(*issue));
        issue->severity = severity;
        issue->object = object;
        issue->action = action;
        snprintf(issue->code, sizeof(issue->code), "%s", code);
        va_list args;
        va_start(args, format);
        vsnprintf(issue->message, sizeof(issue->message), format, args);
        va_end(args);
    }
    r->count++;
    if (severity == MAP_ISSUE_ERROR) {
        r->errors++;
        if (r->opt->first_only) r->stop = 1;
    }
}

// UTILS.isNumber: a finite number value
static int is_number(const cJSON *item) {
    return cJSON_IsNumber(item) && isfinite(cJSON_GetNumberValue(item));
}

// JS truthiness
static int truthy(const cJSON *item) {
    if (!item || cJSON_IsNull(item) || cJSON_IsFalse(item)) return 0;
    if (cJSON_IsNumber(item)) return cJSON_GetNumberValue(item) != 0.0 && !isnan(cJSON_GetNumberValue(item));
    if (cJSON_IsString(item)) return cJSON_GetStringValue(item)[0] != 0;
    return 1;
}

static int present(const cJSON *obj, const char *key) { // obj.key != null
    const cJSON *item = cJSON_GetObjectItem(obj, key);
    return item && !cJSON_IsNull(item);
}

// String.length (UTF-16 code units) of a UTF-8 string
static size_t js_length(const char *text) {
    size_t n = 0;
    for (const unsigned char *c = (const unsigned char *) text; c && *c; c++) {
        if ((*c & 0xC0) == 0x80) continue;
        n += *c >= 0xF0 ? 2 : 1;
    }
    return n;
}

// /[^\w\:\;\(\)\?\,\.\|\[\] \-\+*\$\@\=\^!{}]+/ finds nothing
static int text_charset_ok(const char *text) {
    for (const unsigned char *c = (const unsigned char *) text; *c; c++) {
        const int word = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '_';
        if (!word && !strchr(":;()?,.|[] -+*$@=^!{}", *c)) return 0;
    }
    return 1;
}

static int key_charset_ok(const char *text) { // /[^a-z0-9_]+/gmi finds nothing
    for (const unsigned char *c = (const unsigned char *) text; *c; c++) {
        if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '_')) return 0;
    }
    return 1;
}

// parseInt(value): 1 and *out when it yields a number
static int js_parse_int(const cJSON *item, long *out) {
    if (cJSON_IsNumber(item)) {
        const double v = cJSON_GetNumberValue(item);
        if (!isfinite(v)) return 0;
        *out = (long) v;
        return 1;
    }
    if (cJSON_IsString(item)) {
        const char *text = cJSON_GetStringValue(item);
        while (*text == ' ' || *text == '\t' || *text == '\n') text++;
        char *end = NULL;
        const long v = strtol(text, &end, 10);
        if (end == text) return 0;
        *out = v;
        return 1;
    }
    if (cJSON_IsTrue(item) || cJSON_IsFalse(item) || cJSON_IsNull(item)) return 0; // parseInt("true") is NaN
    return 0;
}

// an array index of prefabs.prefabs / prefabs.textures: a number or a canonical numeric string
static int js_index(const cJSON *item, const int count) {
    if (!item || !truthy(item)) return 0; // x || 0
    if (cJSON_IsNumber(item)) {
        const double v = cJSON_GetNumberValue(item);
        return v >= 0.0 && v < count && v == floor(v) ? (int) v : -1;
    }
    if (cJSON_IsString(item)) {
        const char *text = cJSON_GetStringValue(item);
        if (text[0] == '0' && text[1]) return -1;
        long v = 0;
        for (const char *c = text; *c; c++) {
            if (*c < '0' || *c > '9' || v > count) return -1;
            v = v * 10 + (*c - '0');
        }
        return v < count ? (int) v : -1;
    }
    return -1;
}

// UTILS.isArray(a) && a.length == 3 && UTILS.arrayInts(a)
static int vec3_ok(const cJSON *item) {
    if (!cJSON_IsArray(item) || cJSON_GetArraySize(item) != 3) return 0;
    const cJSON *v;
    cJSON_ArrayForEach(v, item) {
        if (!is_number(v)) return 0;
    }
    return 1;
}

// tct / tcc of a trigger or one of its actions
static void check_text(Report *r, const cJSON *holder, const char *key, const int object, const int action,
                       const char *too_long, const char *illegal) {
    const cJSON *text = cJSON_GetObjectItem(holder, key);
    if (!truthy(text)) return;
    if (!cJSON_IsString(text)) {
        report(r, MAP_ISSUE_WARNING, object, action, "trigger_text_type", "\"%s\" is not a string: v7 replaces it with \"\"", key);
        return;
    }
    if (js_length(cJSON_GetStringValue(text)) > 70) report(r, MAP_ISSUE_ERROR, object, action, "trigger_text_length", "%s", too_long);
    else if (!text_charset_ok(cJSON_GetStringValue(text))) report(r, MAP_ISSUE_ERROR, object, action, "trigger_text_chars", "%s", illegal);
}

static void check_condition(Report *r, const cJSON *entry, const int object, const int action) {
    const cJSON *cd = cJSON_GetObjectItem(entry, "cd");
    if (!truthy(cd)) return;
    const int index = js_index(cd, TRIGGER_V10_CONDITION_COUNT);
    if (index < 0) {
        report(r, MAP_ISSUE_ERROR, object, action, "trigger_condition", "INVALID CONDITION");
        return;
    }
    const TriggerV7Condition *condition = &g_trigger_v7_conditions[index];
    for (int i = 0; i < condition->val_count; i++) {
        const TriggerV7ConditionVal *val = &condition->vals[i];
        char key[4] = {'c', 'v', (char) ('0' + i), 0};
        const cJSON *value = cJSON_GetObjectItem(entry, key);
        const int missing = !value || cJSON_IsNull(value);
        char text[64];
        if (val->kind == TRIGGER_V7_VAL_INPUT) {
            // v += "": numbers and booleans become their text
            if (missing) text[0] = 0;
            else if (cJSON_IsString(value)) snprintf(text, sizeof(text), "%s", cJSON_GetStringValue(value));
            else if (cJSON_IsNumber(value)) snprintf(text, sizeof(text), "%g", cJSON_GetNumberValue(value));
            else snprintf(text, sizeof(text), "%s", cJSON_IsTrue(value) ? "true" : cJSON_IsFalse(value) ? "false" : "[object Object]");
            if (!key_charset_ok(text)) {
                report(r, MAP_ISSUE_ERROR, object, action, "condition_chars", "INVALID CHARACTERS IN CONDITION VALUE (%s)", val->name);
            } else if (js_length(text) > (size_t) (val->len ? val->len : 10)) {
                report(r, MAP_ISSUE_ERROR, object, action, "condition_length", "CONDITION VALUE TOO LONG (%s)", val->name);
            }
        } else if (val->kind == TRIGGER_V7_VAL_SELECT) {
            long option = (long) val->def;
            const int ok = missing ? 1 : js_parse_int(value, &option);
            // select[v] == null: v must index an option (a number or a canonical numeric string)
            const int indexable = missing || js_index(value, val->option_count) >= 0 || (is_number(value) && cJSON_GetNumberValue(value) == 0.0);
            if (!ok || !indexable || option < 0 || option >= val->option_count) {
                report(r, MAP_ISSUE_ERROR, object, action, "condition_option", "CONDITION VALUE OPTION DOESNT EXIST (%s)", val->name);
            }
        } else if (val->kind == TRIGGER_V7_VAL_NUMBER) {
            long parsed = 0;
            if (!missing && !js_parse_int(value, &parsed)) {
                report(r, MAP_ISSUE_ERROR, object, action, "condition_number", "CONDITION VALUE MUST BE A NUMBER(%s)", val->name);
                continue;
            }
            double v = val->def;
            if (!missing) v = cJSON_IsNumber(value) ? cJSON_GetNumberValue(value) : strtod(cJSON_GetStringValue(value), NULL);
            if (v > val->max || v < val->min) {
                report(r, MAP_ISSUE_ERROR, object, action, "condition_range", "CONDITION VALUE OUT OF RANGE (%s)", val->name);
            }
        }
    }
}

typedef struct {
    int count;
    int id;
} InterfaceCount;

// JSON.stringify(value || {}).length >= 15000 (archetype data / entity spawn params)
static int json_too_large(const cJSON *value) {
    if (!value || !truthy(value)) return 0;
    char *text = cJSON_PrintUnformatted(value);
    const int large = text && js_length(text) >= 15000;
    free(text);
    return large;
}

// [0, 1, ..., max].indexOf(parseInt(v)) >= 0
static int js_int_in(const cJSON *v, const int max) {
    long n = 0;
    return js_parse_int(v, &n) && n >= 0 && n <= max;
}

// /[^<allowed>]+/ finds nothing in text (word characters plus `extra`)
static int word_chars_ok(const char *text, const char *extra) {
    for (const char *c = text; *c; c++) {
        if (!(isalnum((unsigned char) *c) || *c == '_' || strchr(extra, *c))) return 0;
    }
    return 1;
}

// v10 validateMap "archetypes" (L187580-187650): null passes; <= 32 entries; ids 1..9999 unique;
// name <= 24 of [\w -]; coll 0..2; hp >= 0; data an object under 15000 chars of JSON; 1..40
// parts, no ENTITY part; part names /^\w{1,12}$/; soft 0 / 1. ids[id] = 1 for each archetype.
// The generic per-object check v10 also runs on parts (_61121) is not repeated here.
static void check_archetypes(Report *r, const cJSON *list, unsigned char *ids) {
    if (!list || cJSON_IsNull(list)) return;
    if (!cJSON_IsArray(list)) {
        report(r, MAP_ISSUE_ERROR, -1, -1, "archetype_list", "Archetype list error.");
        return;
    }
    if (cJSON_GetArraySize(list) > MAP_ARCHETYPE_LIMIT) {
        report(r, MAP_ISSUE_ERROR, -1, -1, "archetype_limit", "Too many archetypes.");
        return;
    }
    const cJSON *a;
    cJSON_ArrayForEach(a, list) {
        long id = 0;
        if (!cJSON_IsObject(a)) {
            report(r, MAP_ISSUE_ERROR, -1, -1, "archetype", "Archetype error.");
            return;
        }
        if (!js_parse_int(cJSON_GetObjectItem(a, "id"), &id) || id < 1 || id > 9999) {
            report(r, MAP_ISSUE_ERROR, -1, -1, "archetype_id", "Archetype id error.");
            return;
        }
        if (ids[id]) {
            report(r, MAP_ISSUE_ERROR, -1, -1, "archetype_duplicate", "Duplicate archetype id. (ID: %ld)", id);
            return;
        }
        ids[id] = 1;
        if (present(a, "name")) {
            const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(a, "name"));
            if (!name || js_length(name) > MAP_ARCHETYPE_NAME_MAX) {
                report(r, MAP_ISSUE_ERROR, -1, -1, "archetype_name", "Archetype name error. (ID: %ld)", id);
                return;
            }
            if (!word_chars_ok(name, " -")) {
                report(r, MAP_ISSUE_ERROR, -1, -1, "archetype_name_chars", "Illegal characters in Archetype name. (ID: %ld)", id);
                return;
            }
        }
        if (present(a, "coll") && !js_int_in(cJSON_GetObjectItem(a, "coll"), 2)) {
            report(r, MAP_ISSUE_ERROR, -1, -1, "archetype_coll", "Archetype collision error. (ID: %ld)", id);
            return;
        }
        const cJSON *hp = cJSON_GetObjectItem(a, "hp");
        if (present(a, "hp") && (!is_number(hp) || cJSON_GetNumberValue(hp) < 0)) {
            report(r, MAP_ISSUE_ERROR, -1, -1, "archetype_hp", "Archetype health error. (ID: %ld)", id);
            return;
        }
        const cJSON *data = cJSON_GetObjectItem(a, "data");
        if (present(a, "data") && !cJSON_IsObject(data)) {
            report(r, MAP_ISSUE_ERROR, -1, -1, "archetype_data", "Archetype data error. (ID: %ld)", id);
            return;
        }
        if (json_too_large(data)) {
            report(r, MAP_ISSUE_ERROR, -1, -1, "archetype_data_size", "Archetype data too large. (ID: %ld)", id);
            return;
        }
        const cJSON *parts = cJSON_GetObjectItem(a, "objects");
        if (!cJSON_IsArray(parts) || !cJSON_GetArraySize(parts)) {
            report(r, MAP_ISSUE_ERROR, -1, -1, "archetype_objects", "Archetype missing objects. (ID: %ld)", id);
            return;
        }
        if (cJSON_GetArraySize(parts) > MAP_ARCHETYPE_OBJECT_LIMIT) {
            report(r, MAP_ISSUE_ERROR, -1, -1, "archetype_object_limit", "Archetype exceeds object limit. (ID: %ld)", id);
            return;
        }
        const cJSON *part;
        cJSON_ArrayForEach(part, parts) {
            const int prefab = cJSON_IsObject(part) ? js_index(cJSON_GetObjectItem(part, "i"), PREFAB_COUNT) : -1;
            if (prefab < 0) {
                report(r, MAP_ISSUE_ERROR, -1, -1, "archetype_part", "Invalid Archetype Object. (ID: %ld)", id);
                return;
            }
            if (prefab_has_flag(prefab, PF_ISENTITY)) {
                report(r, MAP_ISSUE_ERROR, -1, -1, "archetype_entity", "Archetypes cannot contain entities. (ID: %ld)", id);
                return;
            }
            const cJSON *n = cJSON_GetObjectItem(part, "n");
            if (present(part, "n")) { // /^\w{1,12}$/.test(n + "")
                char number[32];
                const char *text = cJSON_GetStringValue(n);
                if (!text && cJSON_IsNumber(n)) snprintf(number, sizeof(number), "%g", cJSON_GetNumberValue(n));
                if (!text) text = cJSON_IsNumber(n) ? number : cJSON_IsTrue(n) ? "true" : cJSON_IsFalse(n) ? "false" : "-";
                if (!text[0] || strlen(text) > 12 || !word_chars_ok(text, "")) {
                    report(r, MAP_ISSUE_ERROR, -1, -1, "archetype_part_name", "Archetype part name error. (ID: %ld)", id);
                    return;
                }
            }
            if (present(part, "soft") && !js_int_in(cJSON_GetObjectItem(part, "soft"), 1)) {
                report(r, MAP_ISSUE_ERROR, -1, -1, "archetype_soft", "Archetype part soft-zone error. (ID: %ld)", id);
                return;
            }
        }
    }
}

// v10 ENTITY object checks (L187700-187730): <= 64; eid 1..9999 unique; ar a known archetype;
// tag <= 32 of [\w-]; as 0 / 1; sp an object under 15000 chars of JSON.
static void check_entity(Report *r, const cJSON *object, const int index, int *entities, unsigned char *eids,
                         const unsigned char *archetypes) {
    if (++*entities > MAP_ENTITY_LIMIT) {
        report(r, MAP_ISSUE_ERROR, index, -1, "entity_limit", "Too many entities.");
        return;
    }
    long eid = 0, ar = 0;
    if (!js_parse_int(cJSON_GetObjectItem(object, "eid"), &eid) || eid < 1 || eid >= 10000) {
        report(r, MAP_ISSUE_ERROR, index, -1, "entity_id", "Entity id error.");
        return;
    }
    if (eids[eid]) {
        report(r, MAP_ISSUE_ERROR, index, -1, "entity_duplicate", "Duplicate entity id. (ID: %ld)", eid);
        return;
    }
    eids[eid] = 1;
    if (!js_parse_int(cJSON_GetObjectItem(object, "ar"), &ar) || ar < 1 || ar > 9999 || !archetypes[ar]) {
        report(r, MAP_ISSUE_ERROR, index, -1, "entity_archetype", "Unknown entity archetype. (ID: %ld)", eid);
        return;
    }
    if (present(object, "tag")) {
        const char *tag = cJSON_GetStringValue(cJSON_GetObjectItem(object, "tag"));
        if (!tag || js_length(tag) > MAP_ENTITY_TAG_MAX) {
            report(r, MAP_ISSUE_ERROR, index, -1, "entity_tag", "Entity tag error. (ID: %ld)", eid);
            return;
        }
        if (!word_chars_ok(tag, "-")) {
            report(r, MAP_ISSUE_ERROR, index, -1, "entity_tag_chars", "Illegal characters in Entity tag. (ID: %ld)", eid);
            return;
        }
    }
    if (present(object, "as") && !js_int_in(cJSON_GetObjectItem(object, "as"), 1)) {
        report(r, MAP_ISSUE_ERROR, index, -1, "entity_auto_spawn", "Entity auto spawn error. (ID: %ld)", eid);
        return;
    }
    const cJSON *sp = cJSON_GetObjectItem(object, "sp");
    if (present(object, "sp") && !cJSON_IsObject(sp)) {
        report(r, MAP_ISSUE_ERROR, index, -1, "entity_params", "Entity spawn params error. (ID: %ld)", eid);
    } else if (json_too_large(sp)) {
        report(r, MAP_ISSUE_ERROR, index, -1, "entity_params_size", "Entity spawn params too large. (ID: %ld)", eid);
    }
}

size_t map_validate(const cJSON *map, const MapValidateOptions *options, MapIssue *issues, const size_t max, size_t *errors) {
    static const MapValidateOptions defaults = {0};
    Report r = {options ? options : &defaults, issues, max, 0, 0, 0};
    const int header = !r.opt->skip_header;

    if (!cJSON_IsObject(map)) {
        report(&r, MAP_ISSUE_ERROR, -1, -1, "map_format", "Map error occured.");
        if (errors) *errors = r.errors;
        return r.count;
    }

    if (header) {
        const cJSON *name = cJSON_GetObjectItem(map, "name");
        const char *text = cJSON_GetStringValue(name);
        int blank = 1;
        for (const char *c = text; c && *c; c++) {
            if (!strchr(" \t\n\r\f\v", *c)) blank = 0;
        }
        if (!cJSON_IsString(name) || blank) report(&r, MAP_ISSUE_ERROR, -1, -1, "name_missing", "Missing map name.");
        else if (strchr(text, '<') || strchr(text, '>')) report(&r, MAP_ISSUE_ERROR, -1, -1, "name_chars", "Illegal characters in map name.");
        else if (js_length(text) > MAP_V7_NAME_MAX) report(&r, MAP_ISSUE_ERROR, -1, -1, "name_length", "Map name too long.");

        const cJSON *tris = cJSON_GetObjectItem(map, "tris");
        if (tris && !cJSON_IsArray(tris) && !cJSON_IsNull(tris)) {
            report(&r, MAP_ISSUE_WARNING, -1, -1, "tris_type", "\"tris\" is not an array: v7 deletes it");
        } else if (cJSON_IsArray(tris) && cJSON_GetArraySize(tris) / 9.0 >= MAP_V7_TRIANGLE_LIMIT) {
            report(&r, MAP_ISSUE_ERROR, -1, -1, "tris_limit", "Complex Collisions exceeds limits.");
        }

        const cJSON *spawns = cJSON_GetObjectItem(map, "spawns");
        if (!cJSON_IsArray(spawns) || !cJSON_GetArraySize(spawns)) {
            report(&r, MAP_ISSUE_ERROR, -1, -1, "spawn_missing", "Missing spawn points.");
        } else if (cJSON_GetArraySize(spawns) > MAP_V7_SPAWN_LIMIT) {
            report(&r, MAP_ISSUE_ERROR, -1, -1, "spawn_count", "Too many spawn points.");
        } else {
            const cJSON *spawn;
            int index = 0;
            cJSON_ArrayForEach(spawn, spawns) {
                const int n = cJSON_IsArray(spawn) ? cJSON_GetArraySize(spawn) : 0;
                int ok = n >= 3 && n <= 8;
                for (int k = 0; ok && k < 3; k++) ok = is_number(cJSON_GetArrayItem(spawn, k));
                // v7 also has a "Spawn team error." check that can never fire (team == null && team == 1
                // && team == 2); its intent is unknown, so it is not reproduced.
                if (!ok) report(&r, MAP_ISSUE_ERROR, -1, -1, "spawn_format", "Spawn point error.");
                else if (n >= 8 && is_number(cJSON_GetArrayItem(spawn, 7)) &&
                         (cJSON_GetNumberValue(cJSON_GetArrayItem(spawn, 7)) > 50 || cJSON_GetNumberValue(cJSON_GetArrayItem(spawn, 7)) < 0)) {
                    report(&r, MAP_ISSUE_WARNING, -1, -1, "spawn_spread", "spawn %d: spread [7] is clamped to 0..50", index);
                }
                index++;
            }
        }

        const cJSON *welcome = cJSON_GetObjectItem(map, "welMsg");
        if (truthy(welcome)) {
            if (!cJSON_IsString(welcome)) {
                report(&r, MAP_ISSUE_WARNING, -1, -1, "welcome_type", "\"welMsg\" is not a string: v7 replaces it with \"\"");
            } else if (js_length(cJSON_GetStringValue(welcome)) > MAP_V7_WELCOME_MAX) {
                report(&r, MAP_ISSUE_ERROR, -1, -1, "welcome_length", "Welcome Message too Long");
            } else if (!text_charset_ok(cJSON_GetStringValue(welcome))) {
                report(&r, MAP_ISSUE_ERROR, -1, -1, "welcome_chars", "Illegal characters in Welcome Message");
            }
        }
    }

    const int prefab_count = PREFAB_COUNT;
    const int action_limit = ruleset_current()->trigger_action_limit;
    // v10: the archetype list is checked before the objects; its id flags feed the ENTITY checks
    unsigned char *archetype_ids = calloc(2 * 10000, 1);
    unsigned char *entity_ids = archetype_ids ? archetype_ids + 10000 : NULL;
    int entities = 0;
    if (archetype_ids) check_archetypes(&r, cJSON_GetObjectItem(map, "archetypes"), archetype_ids);

    const cJSON *objects = cJSON_GetObjectItem(map, "objects");
    const int object_count = cJSON_IsArray(objects) ? cJSON_GetArraySize(objects) : 0;
    if (r.stop) {
        // first_only: an archetype error is v10's answer
    } else if (!object_count) {
        report(&r, MAP_ISSUE_ERROR, -1, -1, "objects_missing", "Missing objects.");
    } else if (object_count > (r.opt->featured ? MAP_V7_OBJECT_LIMIT_FEATURED : MAP_V7_OBJECT_LIMIT)) {
        report(&r, MAP_ISSUE_ERROR, -1, -1, "objects_limit", "Map exceeds overall object limit.");
    }

    if (cJSON_GetObjectItem(map, "game")) {
        report(&r, MAP_ISSUE_WARNING, -1, -1, "legacy_game", "the v3 \"game\" block is ignored by the v7 runtime");
    }

    const int collidable_limit = r.opt->featured ? MAP_V7_COLLIDABLE_LIMIT_FEATURED : MAP_V7_COLLIDABLE_LIMIT;
    int collidable = 0, signs = 0, wheels = 0, leaderboards = 0, ais = 0, bots = 0, anchors = 0;
    InterfaceCount *interfaces = object_count ? calloc((size_t) object_count, sizeof(InterfaceCount)) : NULL;
    int interface_count = 0;
    const cJSON *xyz = cJSON_GetObjectItem(map, "xyz");

    for (int index = 0; index < object_count && !r.stop; index++) {
        const cJSON *object = cJSON_GetArrayItem(objects, index);
        if (!cJSON_IsObject(object)) {
            report(&r, MAP_ISSUE_ERROR, index, -1, "object_format", "Map error occured.");
            continue;
        }

        // the size: s || si (expanded from the "xyz" pool)
        const cJSON *size = cJSON_GetObjectItem(object, "s");
        double pooled[3];
        cJSON *pooled_json = NULL;
        const cJSON *si = cJSON_GetObjectItem(object, "si");
        if (si && !cJSON_IsNull(si)) {
            if (!truthy(size)) size = si;
            const int base = is_number(si) ? (int) cJSON_GetNumberValue(si) * 3 : -1;
            if (base >= 0 && cJSON_IsArray(xyz) && cJSON_GetArraySize(xyz) > base) {
                int ok = 1;
                for (int k = 0; k < 3; k++) {
                    const cJSON *v = cJSON_GetArrayItem(xyz, base + k);
                    ok = ok && is_number(v);
                    pooled[k] = ok ? cJSON_GetNumberValue(v) : 0.0;
                }
                pooled_json = ok ? cJSON_CreateDoubleArray(pooled, 3) : cJSON_CreateArray();
                size = pooled_json;
            }
        }

        // validateMap: prefabs.prefabs[i || 0]
        const cJSON *i_item = cJSON_GetObjectItem(object, "i");
        const int prefab = js_index(i_item, prefab_count);
        if (prefab < 0) {
            report(&r, MAP_ISSUE_ERROR, index, -1, "object_prefab", "Invalid Object.");
            cJSON_Delete(pooled_json);
            continue;
        }
        if (cJSON_IsString(i_item) && prefab_id_from_name(cJSON_GetStringValue(i_item)) >= 0) {
            report(&r, MAP_ISSUE_WARNING, index, -1, "object_prefab_name", "prefab name \"%s\" loads as CUBE in v7", cJSON_GetStringValue(i_item));
        }

        if (!prefab_has_flag(prefab, PF_NOCOL) && !present(object, "l") && !present(object, "col")) collidable++;
        if (collidable > collidable_limit) {
            report(&r, MAP_ISSUE_ERROR, index, -1, "collidable_limit", "Map exceeds collidable object limit.");
            cJSON_Delete(pooled_json);
            break;
        }

        if (prefab == 30 /* SIGN */) {
            signs++;
            const cJSON *st = cJSON_GetObjectItem(object, "st");
            const cJSON *sf = cJSON_GetObjectItem(object, "sf");
            if (st && !cJSON_IsNull(st) && !cJSON_IsString(st)) report(&r, MAP_ISSUE_ERROR, index, -1, "sign_text", "Object sign text error.");
            else if (cJSON_IsString(st) && js_length(cJSON_GetStringValue(st)) > MAP_V7_SIGN_TEXT_LIMIT) {
                report(&r, MAP_ISSUE_ERROR, index, -1, "sign_text_length", "Object sign text to long.");
            } else if (signs > MAP_V7_SIGN_LIMIT) {
                report(&r, MAP_ISSUE_ERROR, index, -1, "sign_limit", "Too many signs.");
            } else if (truthy(sf) && cJSON_IsNumber(sf) &&
                       (cJSON_GetNumberValue(sf) > MAP_V7_SIGN_FONT_MAX || cJSON_GetNumberValue(sf) < MAP_V7_SIGN_FONT_MIN)) {
                report(&r, MAP_ISSUE_ERROR, index, -1, "sign_font", "Object sign font error.");
            }
        }
        if (prefab == 68 /* PRIZE_WHEEL */ && ++wheels > MAP_V7_WHEEL_LIMIT) report(&r, MAP_ISSUE_ERROR, index, -1, "wheel_limit", "Too many Prize Wheels.");
        if (prefab == 72 /* LEADERBOARD */ && ++leaderboards > MAP_V7_LEADERBOARD_LIMIT) {
            report(&r, MAP_ISSUE_ERROR, index, -1, "leaderboard_limit", "Too many Leaderboards.");
        }
        if (prefab_has_flag(prefab, PF_AI) && ++ais > MAP_V7_AI_LIMIT) report(&r, MAP_ISSUE_ERROR, index, -1, "ai_limit", "Too many AI.");
        if (entity_ids && prefab_has_flag(prefab, PF_ISENTITY) && !r.stop) check_entity(&r, object, index, &entities, entity_ids, archetype_ids);
        // not checked by validateMap: addPendingBots / addMapAnchor stop at the limit
        if (prefab == 95 /* BOT_SPAWNER */ && ++bots == MAP_V7_BOT_LIMIT + 1) {
            report(&r, MAP_ISSUE_WARNING, index, -1, "bot_limit", "more than %d BOT_SPAWNERs: the rest are not added", MAP_V7_BOT_LIMIT);
        }
        if (prefab == 96 /* MAP_ANCHOR */ && ++anchors == MAP_V7_ANCHOR_LIMIT + 1) {
            report(&r, MAP_ISSUE_WARNING, index, -1, "anchor_limit", "more than %d MAP_ANCHORs: the rest are ignored", MAP_V7_ANCHOR_LIMIT);
        }
        if (prefab == 41 /* TERMINAL / BOT_PATH_NODE */) {
            report(&r, MAP_ISSUE_WARNING, index, -1, "terminal", "BOT_PATH_NODE builds nothing (bots walk \"botPaths\")");
        }
        if (prefab == 33 /* CAMERA */ && present(object, "obi") && !present(object, "obl")) {
            report(&r, MAP_ISSUE_WARNING, index, -1, "camera_obi", "v3 SPECTATE_CAM \"obi\" is not read by v7 (\"obl\"): links to objective 0");
        }

        const cJSON *tas = cJSON_GetObjectItem(object, "tas");
        if (cJSON_IsArray(tas)) {
            if (cJSON_GetArraySize(tas) > action_limit) {
                report(&r, MAP_ISSUE_ERROR, index, -1, "trigger_actions", "Trigger Has Too Many Actions");
            }
            int a = 0;
            const cJSON *entry;
            cJSON_ArrayForEach(entry, tas) {
                if (r.stop) break;
                if (!cJSON_IsObject(entry)) {
                    report(&r, MAP_ISSUE_ERROR, index, a, "trigger_action", "Map error occured.");
                } else {
                    check_text(&r, entry, "tct", index, a, "Trigger Message too Long", "Illegal characters in Trigger Message");
                    check_text(&r, entry, "tcc", index, a, "Trigger Custom Key too Long", "Illegal characters in Trigger Custom Key");
                    check_condition(&r, entry, index, a);
                }
                a++;
            }
        }
        check_text(&r, object, "tct", index, -1, "Trigger Message too Long", "Illegal characters in Trigger Message");

        long id = 0;
        if (js_parse_int(cJSON_GetObjectItem(object, "in"), &id) && id) {
            int slot = 0;
            while (slot < interface_count && interfaces[slot].id != (int) id) slot++;
            if (slot == interface_count && interfaces) interfaces[interface_count++].id = (int) id;
            if (interfaces && ++interfaces[slot].count == MAP_V7_INTERFACE_LIMIT + 1) {
                // v7's message says "Max 20" while the limit is 30 (config.interfaceLimit): fixed
                report(&r, MAP_ISSUE_ERROR, index, -1, "interface_limit", "Interface limit Exceeded: Max %d (Per id)", MAP_V7_INTERFACE_LIMIT);
            }
        }

        if (!vec3_ok(size)) report(&r, MAP_ISSUE_ERROR, index, -1, "object_scale", "Object scale error.");
        else if (!vec3_ok(cJSON_GetObjectItem(object, "p"))) report(&r, MAP_ISSUE_ERROR, index, -1, "object_position", "Object position error.");
        else {
            const cJSON *rot = cJSON_GetObjectItem(object, "r");
            if (cJSON_IsArray(rot)) {
                if (!vec3_ok(rot)) report(&r, MAP_ISSUE_ERROR, index, -1, "object_rotation_value", "Object rotation value error.");
            } else if (rot && !cJSON_IsNull(rot)) {
                report(&r, MAP_ISSUE_ERROR, index, -1, "object_rotation", "Object rotation error.");
            }
            const cJSON *d = cJSON_GetObjectItem(object, "d");
            if (d && !cJSON_IsNull(d) && !is_number(d)) report(&r, MAP_ISSUE_ERROR, index, -1, "object_direction", "Object direction error.");
        }
        cJSON_Delete(pooled_json);
    }
    free(interfaces);

    if (header && !r.stop) {
        const cJSON *gui = cJSON_GetObjectItem(map, "gui");
        if (cJSON_IsArray(gui) && cJSON_GetArraySize(gui) > MAP_V7_GUI_LIMIT) {
            report(&r, MAP_ISSUE_ERROR, -1, -1, "gui_limit", "Map exceeds gui limit.");
        }
    }

    // prepareGame: prefabs.prefabs[i || id || 0] and prefabs.textures[t || 0] must exist
    for (int index = 0; index < object_count && !r.stop; index++) {
        const cJSON *object = cJSON_GetArrayItem(objects, index);
        if (!cJSON_IsObject(object)) continue;
        const cJSON *i_item = cJSON_GetObjectItem(object, "i");
        const cJSON *pick = truthy(i_item) ? i_item : cJSON_GetObjectItem(object, "id");
        if (js_index(pick, prefab_count) < 0) report(&r, MAP_ISSUE_ERROR, index, -1, "object_id", "Object ID error.");
        else if (js_index(cJSON_GetObjectItem(object, "t"), PREFAB_TEXTURE_COUNT) < 0) {
            report(&r, MAP_ISSUE_ERROR, index, -1, "object_texture", "Object Texture error.");
        }
    }

    free(archetype_ids);
    if (errors) *errors = r.errors;
    return r.count;
}

const char *map_validate_error(const cJSON *map, const int featured) {
    static MapIssue issue;
    const MapValidateOptions options = {.featured = (unsigned char) !!featured, .no_warnings = 1, .first_only = 1};
    size_t errors = 0;
    map_validate(map, &options, &issue, 1, &errors);
    return errors ? issue.message : NULL;
}

char *map_prepare_custom_text(const char *json_text) {
    if (!json_text) return NULL;
    size_t parens = 0, length = 0;
    for (const char *c = json_text; *c; c++, length++) parens += *c == '(' || *c == ')';
    char *out = malloc(length + parens * 2 + 1); // "（" / "）" are 3 bytes of UTF-8
    if (!out) return NULL;
    char *o = out;
    for (const char *c = json_text; *c; c++) {
        if (*c == '(') o += sprintf(o, "\xEF\xBC\x88");
        else if (*c == ')') o += sprintf(o, "\xEF\xBC\x89");
        else *o++ = *c;
    }
    *o = 0;
    return out;
}

static int replace_parens(char **text) {
    if (!*text || !strpbrk(*text, "()")) return 0;
    char *out = map_prepare_custom_text(*text);
    if (!out) return 0;
    cJSON_free(*text);
    char *copy = cJSON_malloc(strlen(out) + 1); // cJSON owns its strings: allocate with its allocator
    if (copy) strcpy(copy, out);
    free(out);
    *text = copy;
    return copy != NULL;
}

int map_prepare_custom_json(cJSON *item) {
    int changed = 0;
    for (; item; item = item->next) {
        if (item->string && !(item->type & cJSON_StringIsConst)) changed += replace_parens(&item->string);
        if (cJSON_IsString(item) && !(item->type & cJSON_IsReference)) changed += replace_parens(&item->valuestring);
        if (item->child) changed += map_prepare_custom_json(item->child);
    }
    return changed;
}

cJSON *map_prepare_custom(const char *json_text, const int featured, const char **error) {
    static const char *const k_map_error = "Map error occured.";
    if (error) *error = NULL;
    char *text = map_prepare_custom_text(json_text);
    cJSON *map = text ? cJSON_Parse(text) : NULL;
    free(text);
    if (!cJSON_IsObject(map) || !cJSON_IsArray(cJSON_GetObjectItem(map, "objects"))) {
        cJSON_Delete(map);
        if (error) *error = k_map_error;
        return NULL;
    }
    const char *message = map_validate_error(map, featured);
    if (message) {
        cJSON_Delete(map);
        if (error) *error = message;
        return NULL;
    }
    return map;
}
