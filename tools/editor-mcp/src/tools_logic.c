// Spawns, triggers and interfaces.
#define _GNU_SOURCE
#include "mcp.h"
#include "objops.h"
#include "schema.h"
#include "util.h"
#include "validate.h"
#include "editor_model.h" // client/src/editor: tas canonicalisation (ObjectInstance.serialize) and the trigger tables
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// ---------------------------------------------------------------- spawns
// File projects keep spawns in the top-level "spawns" array ([x,y,z,team,dir,comp,...]); live
// editor projects as SPAWN_POINT objects ({i:5, p, tm, d, cp}). Both are addressed by index.

typedef struct {
    double p[3];
    int team, dir, comp;
    int id; // live object id, 0 for array spawns
} SpawnInfo;

static int spawn_prefab(void) { return schema_prefab_find("SPAWN_POINT"); }

static int spawns_read(Doc *doc, SpawnInfo **out) {
    *out = NULL;
    int n = 0, cap = 0;
    SpawnInfo *list = NULL;
    if (doc->editor_form) {
        const int sp = spawn_prefab();
        for (const cJSON *o = doc_objects(doc)->child; o; o = o->next) {
            if (obj_prefab(o) != sp) continue;
            if (n == cap) {
                cap = cap ? cap * 2 : 16;
                SpawnInfo *t = realloc(list, sizeof(SpawnInfo) * (size_t) cap);
                if (!t) break;
                list = t;
            }
            SpawnInfo *s = &list[n++];
            obj_pos(o, s->p);
            s->team = js_int(o, "tm", 0);
            s->dir = js_int(o, "d", 0);
            s->comp = js_get(o, "cp") != NULL;
            s->id = o->valueint;
        }
    } else {
        const cJSON *arr;
        cJSON_ArrayForEach(arr, js_get(doc->root, "spawns")) {
            if (n == cap) {
                cap = cap ? cap * 2 : 16;
                SpawnInfo *t = realloc(list, sizeof(SpawnInfo) * (size_t) cap);
                if (!t) break;
                list = t;
            }
            SpawnInfo *s = &list[n++];
            memset(s, 0, sizeof(*s));
            js_vec3(arr, s->p);
            const cJSON *t = cJSON_GetArrayItem(arr, 3), *d = cJSON_GetArrayItem(arr, 4), *c = cJSON_GetArrayItem(arr, 5);
            s->team = cJSON_IsNumber(t) ? (int) t->valuedouble : 0;
            s->dir = cJSON_IsNumber(d) ? (int) d->valuedouble : 0;
            s->comp = cJSON_IsNumber(c) && c->valuedouble != 0;
        }
    }
    *out = list;
    return n;
}

static cJSON *spawn_array(const SpawnInfo *s, const cJSON *old) {
    cJSON *a = cJSON_CreateArray();
    for (int k = 0; k < 3; k++) cJSON_AddItemToArray(a, cJSON_CreateNumber(js_round(s->p[k], 2)));
    cJSON_AddItemToArray(a, cJSON_CreateNumber(s->team));
    cJSON_AddItemToArray(a, cJSON_CreateNumber(s->dir));
    cJSON_AddItemToArray(a, cJSON_CreateNumber(s->comp ? 1 : 0));
    // keep v7 extras (teamOnly, spread)
    for (int i = 6; old && i < cJSON_GetArraySize(old); i++) cJSON_AddItemToArray(a, cJSON_Duplicate(cJSON_GetArrayItem(old, i), 1));
    return a;
}

static cJSON *spawn_object(const SpawnInfo *s) {
    cJSON *o = cJSON_CreateObject();
    double p[3];
    for (int k = 0; k < 3; k++) p[k] = js_round(s->p[k], 2);
    cJSON_AddItemToObject(o, "p", js_vec3_new(p));
    cJSON_AddNumberToObject(o, "i", spawn_prefab());
    cJSON_AddNumberToObject(o, "tm", s->team);
    if (s->dir) cJSON_AddNumberToObject(o, "d", s->dir);
    if (s->comp) cJSON_AddNumberToObject(o, "cp", 1);
    return o;
}

static cJSON *spawns_array(Doc *doc) {
    cJSON *a = cJSON_GetObjectItemCaseSensitive(doc->root, "spawns");
    if (!cJSON_IsArray(a)) {
        cJSON_DeleteItemFromObjectCaseSensitive(doc->root, "spawns");
        a = cJSON_AddArrayToObject(doc->root, "spawns");
    }
    return a;
}

static int parse_spawn(const cJSON *spec, SpawnInfo *s, char *err, const size_t errsz) {
    const cJSON *p = js_get(spec, "p");
    if (!p) p = js_get(spec, "position");
    if (p && !js_vec3(p, s->p)) {
        snprintf(err, errsz, "p needs [x, y, z]");
        return -1;
    }
    for (int k = 0; k < 3; k++) {
        if (!isfinite(s->p[k]) || fabs(s->p[k]) > schema_limit("positionRange", 500000)) {
            snprintf(err, errsz, "spawn position out of range");
            return -1;
        }
    }
    if (js_get(spec, "team")) s->team = js_int(spec, "team", 0);
    if (s->team < 0 || s->team > 2) {
        snprintf(err, errsz, "team is 0 (any), 1 or 2");
        return -1;
    }
    if (js_get(spec, "dir")) s->dir = ((js_int(spec, "dir", 0) % 4) + 4) % 4;
    // player.c player_spawn: yaw = -90 deg * (1 + dir) (forward = (-sin yaw, -cos yaw): dir 0 faces +X)
    if (js_get(spec, "yaw_deg")) s->dir = (((int) lround(-js_num(spec, "yaw_deg", 0) / 90.0) - 1) % 4 + 4) % 4;
    if (js_get(spec, "comp")) s->comp = js_bool(spec, "comp", 0);
    return 0;
}

static void snap_spawn(Doc *doc, SpawnInfo *s) {
    NativeMap nm;
    char err[256];
    if (native_map_load(doc, &nm, err, sizeof(err)) != 0) return;
    double probe[3] = {s->p[0], s->p[1] + 1000, s->p[2]};
    // the highest ground under the point that is not above the requested height + a storey
    probe[1] = s->p[1] + 20;
    const double g = native_ground(&nm, probe);
    if (g > -1e29) s->p[1] = g;
    native_map_free(&nm);
}

static int t_spawns_list(Ctx *ctx, const cJSON *args, ToolResult *r) {
    Project *p = tool_project(ctx, args, r);
    if (!p) return 0;
    SpawnInfo *list;
    const int n = spawns_read(p->doc, &list);
    NativeMap nm;
    char err[256];
    const int native = native_map_load(p->doc, &nm, err, sizeof(err)) == 0;
    cJSON *o = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(o, "spawns");
    for (int i = 0; i < n; i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddNumberToObject(e, "index", i);
        if (list[i].id) cJSON_AddNumberToObject(e, "id", list[i].id);
        cJSON_AddItemToObject(e, "p", js_vec3_new(list[i].p));
        cJSON_AddNumberToObject(e, "team", list[i].team);
        cJSON_AddNumberToObject(e, "dir", list[i].dir);
        cJSON_AddNumberToObject(e, "yaw_deg", fmod(fmod(-90.0 * (1 + list[i].dir), 360.0) + 360.0, 360.0));
        static const char *facing[4] = {"+x", "+z", "-x", "-z"};
        cJSON_AddStringToObject(e, "facing", facing[((list[i].dir % 4) + 4) % 4]);
        if (list[i].comp) cJSON_AddTrueToObject(e, "comp");
        if (native) {
            const int b = native_spawn_blocker(&nm, list[i].p);
            if (b >= 0) cJSON_AddNumberToObject(e, "inside_object", nm.ids[b]);
            const double g = native_ground(&nm, list[i].p);
            if (g > -1e29) cJSON_AddNumberToObject(e, "ground_y", js_round(g, 2));
            else cJSON_AddTrueToObject(e, "over_void");
        }
        cJSON_AddItemToArray(arr, e);
    }
    if (native) native_map_free(&nm);
    free(list);
    cJSON_AddNumberToObject(o, "count", n);
    cJSON_AddNumberToObject(o, "limit", schema_limit("spawnLimit", 20));
    tr_json(r, o);
    return 0;
}

static int t_spawns_add(Ctx *ctx, const cJSON *args, ToolResult *r) {
    Project *p = tool_project(ctx, args, r);
    if (!p) return 0;
    const cJSON *list = js_get(args, "spawns");
    if (!cJSON_IsArray(list) || !cJSON_GetArraySize(list)) return tr_error(r, "spawns_add needs spawns: [{p, team, dir, comp}]");
    char err[512];
    Edit e;
    if (edit_begin(ctx, p, "add spawns", &e, err, sizeof(err)) != 0) return tr_error(r, "%s", err);
    const int snap = js_bool(args, "snap_to_ground", 1);
    int k = 0;
    const cJSON *spec;
    cJSON_ArrayForEach(spec, list) {
        SpawnInfo s = {0};
        if (parse_spawn(spec, &s, err, sizeof(err)) != 0) {
            edit_abort(&e);
            return tr_error(r, "spawns[%d]: %s", k, err);
        }
        if (snap) snap_spawn(p->doc, &s);
        if (p->doc->editor_form) edit_add_object(&e, spawn_object(&s), -1);
        else cJSON_AddItemToArray(spawns_array(p->doc), spawn_array(&s, NULL));
        k++;
    }
    if (edit_commit(&e, err, sizeof(err)) != 0) return tr_error(r, "%s", err);
    edit_end(&e);
    return t_spawns_list(ctx, args, r);
}

static int t_spawns_update(Ctx *ctx, const cJSON *args, ToolResult *r) {
    Project *p = tool_project(ctx, args, r);
    if (!p) return 0;
    const cJSON *list = js_get(args, "updates");
    if (!cJSON_IsArray(list)) return tr_error(r, "spawns_update needs updates: [{index, p?, team?, dir?, comp?}]");
    char err[512];
    Edit e;
    if (edit_begin(ctx, p, "update spawns", &e, err, sizeof(err)) != 0) return tr_error(r, "%s", err);
    SpawnInfo *cur;
    const int n = spawns_read(p->doc, &cur);
    const cJSON *u;
    cJSON_ArrayForEach(u, list) {
        const int index = js_int(u, "index", -1);
        if (index < 0 || index >= n) {
            free(cur);
            edit_abort(&e);
            return tr_error(r, "spawn index %d does not exist (0..%d)", index, n - 1);
        }
        SpawnInfo s = cur[index];
        if (parse_spawn(u, &s, err, sizeof(err)) != 0) {
            free(cur);
            edit_abort(&e);
            return tr_error(r, "spawn %d: %s", index, err);
        }
        if (js_bool(u, "snap_to_ground", 0)) snap_spawn(p->doc, &s);
        if (p->doc->editor_form) {
            cJSON *o = doc_object_by_id(p->doc, s.id, NULL);
            if (o) {
                obj_set_vec(o, "p", s.p);
                cJSON_DeleteItemFromObjectCaseSensitive(o, "tm");
                cJSON_AddNumberToObject(o, "tm", s.team);
                cJSON_DeleteItemFromObjectCaseSensitive(o, "d");
                if (s.dir) cJSON_AddNumberToObject(o, "d", s.dir);
                cJSON_DeleteItemFromObjectCaseSensitive(o, "cp");
                if (s.comp) cJSON_AddNumberToObject(o, "cp", 1);
            }
        } else {
            cJSON *arr = spawns_array(p->doc);
            const cJSON *old = cJSON_GetArrayItem(arr, index);
            cJSON_ReplaceItemInArray(arr, index, spawn_array(&s, old));
        }
    }
    free(cur);
    if (edit_commit(&e, err, sizeof(err)) != 0) return tr_error(r, "%s", err);
    edit_end(&e);
    return t_spawns_list(ctx, args, r);
}

static int t_spawns_delete(Ctx *ctx, const cJSON *args, ToolResult *r) {
    Project *p = tool_project(ctx, args, r);
    if (!p) return 0;
    int n;
    int *idx = tool_ids(js_get(args, "indices"), &n);
    if (!idx || !n) {
        free(idx);
        return tr_error(r, "spawns_delete needs indices");
    }
    SpawnInfo *cur;
    const int count = spawns_read(p->doc, &cur);
    for (int i = 0; i < n; i++) {
        if (idx[i] < 0 || idx[i] >= count) {
            const int bad = idx[i];
            free(idx);
            free(cur);
            return tr_error(r, "spawn index %d does not exist", bad);
        }
    }
    char err[512];
    Edit e;
    if (edit_begin(ctx, p, "delete spawns", &e, err, sizeof(err)) != 0) {
        free(idx);
        free(cur);
        return tr_error(r, "%s", err);
    }
    // highest index first so earlier indices stay valid
    for (int i = 0; i < n; i++) for (int j = i + 1; j < n; j++) if (idx[j] > idx[i]) { const int t = idx[i]; idx[i] = idx[j]; idx[j] = t; }
    for (int i = 0; i < n; i++) {
        if (i && idx[i] == idx[i - 1]) continue;
        if (p->doc->editor_form) doc_remove_object(p->doc, cur[idx[i]].id);
        else cJSON_DeleteItemFromArray(spawns_array(p->doc), idx[i]);
    }
    free(idx);
    free(cur);
    if (edit_commit(&e, err, sizeof(err)) != 0) return tr_error(r, "%s", err);
    edit_end(&e);
    return t_spawns_list(ctx, args, r);
}

// ---------------------------------------------------------------- triggers

static int is_trigger(const cJSON *o) {
    const int prefab = obj_prefab(o);
    const SchemaPrefab *pf = schema_prefab(prefab);
    return pf && (!strcmp(pf->name, "TRIGGER") || (schema_prefab_has_flag(prefab, "ai") && js_get(o, "ct")));
}

static cJSON *event_json(const int te) {
    cJSON *e = cJSON_CreateObject();
    cJSON_AddNumberToObject(e, "id", te);
    const cJSON *ev = cJSON_GetArrayItem(js_get(schema_triggers(), "events"), te);
    cJSON_AddStringToObject(e, "name", js_str(ev, "name", "?"));
    cJSON_AddBoolToObject(e, "native", js_bool(ev, "native", 0));
    return e;
}

static cJSON *action_json(const int ta) {
    cJSON *e = cJSON_CreateObject();
    cJSON_AddNumberToObject(e, "id", ta);
    const cJSON *ac = cJSON_GetArrayItem(js_get(schema_triggers(), "actions"), ta);
    cJSON_AddStringToObject(e, "name", js_str(ac, "name", "?"));
    cJSON_AddStringToObject(e, "value", js_str(ac, "value", "number"));
    cJSON_AddBoolToObject(e, "native", js_bool(ac, "native", 0));
    return e;
}

static const cJSON *trigger_table(const char *key) { return cJSON_GetObjectItemCaseSensitive(editor_data_get("triggers"), key); }

// a name in a list of names / {name} objects, or an index: -1 when unknown
static int name_index(const cJSON *list, const cJSON *v) {
    const int n = cJSON_GetArraySize(list);
    if (cJSON_IsNumber(v)) return v->valuedouble >= 0 && (int) v->valuedouble < n ? (int) v->valuedouble : -1;
    if (!cJSON_IsString(v)) return -1;
    for (int i = 0; i < n; i++) {
        const cJSON *it = cJSON_GetArrayItem(list, i);
        const char *name = cJSON_IsString(it) ? it->valuestring : js_str(it, "name", "");
        if (!strcasecmp(name, v->valuestring)) return i;
    }
    return -1;
}

// editor_data string list entry, "?" when out of range
static const char *list_name(const char *key, const int index) {
    const char *v = cJSON_GetStringValue(cJSON_GetArrayItem(editor_data_get(key), index));
    return v ? v : "?";
}

// one tas entry decoded (v7 TriggerAction fields, doc/v7/editor.md 1.7)
static cJSON *tas_action_json(const cJSON *a) {
    cJSON *e = action_json(js_int(a, "ta", 0));
    if (js_get(a, "tc")) cJSON_AddNumberToObject(e, "value", js_num(a, "tc", 0));
    if (js_get(a, "tc2")) cJSON_AddNumberToObject(e, "value2", js_num(a, "tc2", 0));
    if (js_get(a, "tct")) cJSON_AddStringToObject(e, "text", js_str(a, "tct", ""));
    if (js_int(a, "tce", 0)) {
        cJSON_AddStringToObject(e, "value_type", list_name("inputTypes", js_int(a, "tce", 0)));
        if (js_get(a, "tcc")) cJSON_AddStringToObject(e, "key", js_str(a, "tcc", ""));
    }
    if (js_get(a, "tad")) cJSON_AddNumberToObject(e, "delay", js_num(a, "tad", 0));
    if (js_int(a, "cd", 0)) {
        const cJSON *cd = cJSON_GetArrayItem(trigger_table("conditions"), js_int(a, "cd", 0));
        cJSON *c = cJSON_AddObjectToObject(e, "condition");
        cJSON_AddNumberToObject(c, "id", js_int(a, "cd", 0));
        cJSON_AddStringToObject(c, "name", js_str(cd, "name", "?"));
        cJSON *vals = cJSON_AddArrayToObject(c, "values");
        for (int k = 0; k < 3; k++) {
            char key[8];
            snprintf(key, sizeof(key), "cv%d", k);
            const cJSON *v = js_get(a, key);
            if (k < cJSON_GetArraySize(js_get(cd, "vals"))) cJSON_AddItemToArray(vals, v ? cJSON_Duplicate(v, 1) : cJSON_CreateNull());
        }
    }
    if (js_get(a, "es") || js_get(a, "esa")) {
        cJSON *snd = cJSON_AddObjectToObject(e, "sound");
        if (js_get(a, "esa")) cJSON_AddNumberToObject(snd, "asset", js_int(a, "esa", 0));
        else {
            cJSON_AddNumberToObject(snd, "id", js_int(a, "es", 0));
            cJSON_AddStringToObject(snd, "name", list_name("emitterSounds", js_int(a, "es", 0)));
        }
        cJSON_AddNumberToObject(snd, "volume", js_num(a, "ev", 1));
        cJSON_AddNumberToObject(snd, "rate", js_num(a, "er", 1));
        cJSON_AddNumberToObject(snd, "emit_from", js_int(a, "ef", 0));
    }
    return e;
}

static cJSON *trigger_json(Doc *doc, const cJSON *o, const int index) {
    cJSON *t = doc_object_json(doc, o, index, 0);
    cJSON_AddItemToObject(t, "event", event_json(js_int(o, "te", 0)));
    cJSON_AddNumberToObject(t, "event_value", js_int(o, "tce", 0));
    const cJSON *tas = js_get(o, "tas");
    if (cJSON_IsArray(tas)) {
        cJSON *acts = cJSON_AddArrayToObject(t, "actions");
        const cJSON *a;
        cJSON_ArrayForEach(a, tas) cJSON_AddItemToArray(acts, tas_action_json(a));
        if (js_get(o, "tam") || js_get(o, "tac")) {
            cJSON_AddNumberToObject(t, "action_method", js_int(o, "tam", 0));
            cJSON_AddNumberToObject(t, "action_cost", js_int(o, "tac", 0));
        }
    } else {
        cJSON *e = action_json(js_int(o, "ta", 0));
        if (js_get(o, "tct")) cJSON_AddStringToObject(e, "text", js_str(o, "tct", ""));
        else cJSON_AddNumberToObject(e, "value", js_int(o, "tc", 0));
        cJSON_AddItemToObject(t, "action", e);
    }
    if (js_get(o, "cha")) cJSON_AddNumberToObject(t, "chance", js_int(o, "cha", 0));
    if (js_get(o, "su")) cJSON_AddTrueToObject(t, "single_use");
    if (js_get(o, "tm")) cJSON_AddNumberToObject(t, "team", js_int(o, "tm", 0));
    const int in = js_int(o, "in", 0), tin = js_int(o, "tin", 0);
    if (in) cJSON_AddNumberToObject(t, "interface_id", in);
    if (tin) {
        cJSON_AddNumberToObject(t, "target_interface", tin);
        cJSON *targets = cJSON_AddArrayToObject(t, "targets");
        for (const cJSON *x = doc_objects(doc)->child; x; x = x->next) if (js_int(x, "in", 0) == tin && x != o) cJSON_AddItemToArray(targets, cJSON_CreateNumber(x->valueint));
    }
    return t;
}

static int t_triggers_list(Ctx *ctx, const cJSON *args, ToolResult *r) {
    Project *p = tool_project(ctx, args, r);
    if (!p) return 0;
    cJSON *o = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(o, "triggers");
    int i = 0;
    for (const cJSON *x = doc_objects(p->doc)->child; x; x = x->next, i++) if (is_trigger(x)) cJSON_AddItemToArray(arr, trigger_json(p->doc, x, i));
    cJSON_AddNumberToObject(o, "count", cJSON_GetArraySize(arr));
    tr_json(r, o);
    return 0;
}

static int t_triggers_catalog(Ctx *ctx, const cJSON *args, ToolResult *r) {
    (void) ctx;
    (void) args;
    cJSON *o = cJSON_Duplicate(schema_triggers(), 1);
    cJSON_AddStringToObject(o, "note", "native = simulated by KrunkNative (v3.1.1 set); v7-only events / actions are kept in the map and run on v7 servers");
    cJSON *keys = cJSON_AddObjectToObject(o, "keys");
    cJSON_AddStringToObject(keys, "te", "event");
    cJSON_AddStringToObject(keys, "tce", "event value (timer s, storage threshold, ...)");
    cJSON_AddStringToObject(keys, "ta", "action (legacy single action)");
    cJSON_AddStringToObject(keys, "tc", "value (number actions)");
    cJSON_AddStringToObject(keys, "tct", "text (text actions, <= 70 chars)");
    cJSON_AddStringToObject(keys, "cha", "chance %% (0 = always)");
    cJSON_AddStringToObject(keys, "tin", "target interface id");
    cJSON_AddStringToObject(keys, "in", "own interface id");
    cJSON_AddStringToObject(keys, "tas", "v7 action list [{ta, tc, tct, cd, cv0..2, ...}] (<= 5)");
    tr_json(r, o);
    return 0;
}

// spec.actions -> obj.tas (the v7 action list, <= triggerActionLimit): every entry built from
// {action, value, value2, text, key, value_type, delay, condition, condition_values, sound,
// asset_sound, volume, rate, emit_from} and then written as ObjectInstance.serialize() writes it
// (editor_model.c: the sound keys only on 14 / 34 / 54, cv0..2 only with a condition, ...).
// Legacy object-level ta / tc / tc2 / tct go (v7 reads tas when present).
static int apply_actions(cJSON *obj, const cJSON *list, char *err, const size_t errsz) {
    const int n = cJSON_GetArraySize(list), limit = (int) schema_limit("triggerActionLimit", 5);
    if (!cJSON_IsArray(list) || n < 1 || n > limit) {
        snprintf(err, errsz, "actions needs 1..%d entries", limit);
        return -1;
    }
    cJSON *tas = cJSON_CreateArray();
    int i = 0;
    const cJSON *a;
    cJSON_ArrayForEach(a, list) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddItemToArray(tas, e);
        const cJSON *ac = js_get(a, "action");
        const int ta = !ac ? 0 : cJSON_IsNumber(ac) ? (int) ac->valuedouble : schema_action_find(cJSON_IsString(ac) ? ac->valuestring : "");
        if (ta < 0 || ta >= schema_action_count()) {
            snprintf(err, errsz, "actions[%d]: unknown action (triggers_catalog lists %d)", i, schema_action_count());
            cJSON_Delete(tas);
            return -1;
        }
        if (ta) cJSON_AddNumberToObject(e, "ta", ta);
        static const struct { const char *spec, *key; } nums[] = {{"value", "tc"}, {"value2", "tc2"}, {"delay", "tad"}, {"sound", "es"},
                                                                  {"asset_sound", "esa"}, {"volume", "ev"}, {"rate", "er"}, {"emit_from", "ef"}, {NULL, NULL}};
        for (int k = 0; nums[k].spec; k++) {
            const cJSON *v = js_get(a, nums[k].spec);
            if (!v) continue;
            if (!cJSON_IsNumber(v)) {
                snprintf(err, errsz, "actions[%d].%s needs a number", i, nums[k].spec);
                cJSON_Delete(tas);
                return -1;
            }
            cJSON_AddNumberToObject(e, nums[k].key, v->valuedouble);
        }
        const cJSON *text = js_get(a, "text"), *key = js_get(a, "key");
        if (text) {
            if (!cJSON_IsString(text) || !trigger_text_valid(text->valuestring)) {
                snprintf(err, errsz, "actions[%d].text: <= 70 characters of the trigger charset", i);
                cJSON_Delete(tas);
                return -1;
            }
            cJSON_AddStringToObject(e, "tct", text->valuestring);
        }
        if (key) {
            if (!cJSON_IsString(key) || !trigger_text_valid(key->valuestring)) {
                snprintf(err, errsz, "actions[%d].key: a value key ([a-z0-9_])", i);
                cJSON_Delete(tas);
                return -1;
            }
            cJSON_AddStringToObject(e, "tcc", key->valuestring);
        }
        const cJSON *vt = js_get(a, "value_type");
        if (vt) {
            const int t = name_index(editor_data_get("inputTypes"), vt);
            if (t < 0) {
                snprintf(err, errsz, "actions[%d].value_type: Input Value, Trigger Value, Player Value or Global Value (0..3)", i);
                cJSON_Delete(tas);
                return -1;
            }
            if (t) cJSON_AddNumberToObject(e, "tce", t);
        }
        const cJSON *cd = js_get(a, "condition");
        if (cd) {
            const int c = name_index(trigger_table("conditions"), cd);
            if (c < 0) {
                snprintf(err, errsz, "actions[%d].condition: unknown (triggers_catalog conditions)", i);
                cJSON_Delete(tas);
                return -1;
            }
            if (c) cJSON_AddNumberToObject(e, "cd", c);
            const cJSON *vals = js_get(a, "condition_values");
            for (int k = 0; k < 3 && k < cJSON_GetArraySize(vals); k++) {
                const cJSON *v = cJSON_GetArrayItem(vals, k);
                char cv[8];
                snprintf(cv, sizeof(cv), "cv%d", k);
                if (!cJSON_IsNull(v)) cJSON_AddItemToObject(e, cv, cJSON_Duplicate(v, 1));
            }
        }
        i++;
    }
    static const char *const legacy[] = {"ta", "tc", "tc2", "tct", "es", "esa", "ev", "er", "ef", NULL};
    for (int k = 0; legacy[k]; k++) cJSON_DeleteItemFromObjectCaseSensitive(obj, legacy[k]);
    if (cJSON_GetObjectItemCaseSensitive(obj, "tas")) cJSON_ReplaceItemInObjectCaseSensitive(obj, "tas", tas);
    else cJSON_AddItemToObject(obj, "tas", tas);
    // serialize()'s form of the list (the rest of the object is left as it is)
    cJSON *canon = ed_model_object(obj, ED_MODEL_KEEP_UUID | ED_MODEL_EXACT_TRANSFORM);
    const cJSON *ct = canon ? cJSON_GetObjectItemCaseSensitive(canon, "tas") : NULL;
    if (ct) cJSON_ReplaceItemInObjectCaseSensitive(obj, "tas", cJSON_Duplicate(ct, 1));
    cJSON_Delete(canon);
    return 0;
}

// trigger spec -> props on obj
static int apply_trigger_spec(Doc *doc, cJSON *obj, const cJSON *spec, const int flags, char *err, const size_t errsz) {
    const cJSON *ev = js_get(spec, "event");
    if (ev) {
        const int te = cJSON_IsNumber(ev) ? (int) ev->valuedouble : schema_event_find(cJSON_IsString(ev) ? ev->valuestring : "");
        if (te < 0 || te >= schema_event_count()) {
            snprintf(err, errsz, "unknown event (triggers_catalog lists %d)", schema_event_count());
            return -1;
        }
        cJSON *v = cJSON_CreateNumber(te);
        const int rc = doc_set_prop(doc, obj, "te", v, flags | SET_FORCE, err, errsz);
        cJSON_Delete(v);
        if (rc) return -1;
    }
    const cJSON *actions = js_get(spec, "actions");
    if (actions && apply_actions(obj, actions, err, errsz) != 0) return -1;
    const cJSON *ac = js_get(spec, "action");
    int ta = js_int(obj, "ta", 0);
    int first_action = actions != NULL; // value / text went to tas[0] (or the actions list)
    if (!actions && cJSON_IsArray(js_get(obj, "tas")) && (ac || js_get(spec, "value") || js_get(spec, "text"))) {
        // a v7 trigger (tas): action / value / text are its first action's ("action@0", ... of
        // editor_model.c's property table, so serialize()'s rules apply)
        cJSON *copy = cJSON_Duplicate(obj, 1);
        const int prefab = obj_prefab(obj);
        if (ac) {
            const int id = cJSON_IsNumber(ac) ? (int) ac->valuedouble : schema_action_find(cJSON_IsString(ac) ? ac->valuestring : "");
            if (id < 0 || id >= schema_action_count()) {
                cJSON_Delete(copy);
                snprintf(err, errsz, "unknown action (triggers_catalog lists %d)", schema_action_count());
                return -1;
            }
            cJSON *v = cJSON_CreateNumber(id);
            ed_model_prop_set(&copy, prefab, "action@0", v);
            cJSON_Delete(v);
        }
        const cJSON *value = js_get(spec, "value"), *text = js_get(spec, "text");
        if (value) ed_model_prop_set(&copy, prefab, "constant@0", value);
        if (text) {
            if (!cJSON_IsString(text) || !trigger_text_valid(text->valuestring)) {
                cJSON_Delete(copy);
                snprintf(err, errsz, "text: <= 70 characters of the trigger charset");
                return -1;
            }
            ed_model_prop_set(&copy, prefab, "constantTxt@0", text);
        }
        const cJSON *tas = copy ? cJSON_GetObjectItemCaseSensitive(copy, "tas") : NULL;
        if (tas) cJSON_ReplaceItemInObjectCaseSensitive(obj, "tas", cJSON_Duplicate(tas, 1));
        cJSON_Delete(copy);
        ac = NULL;
        first_action = 1;
    }
    if (ac) {
        ta = cJSON_IsNumber(ac) ? (int) ac->valuedouble : schema_action_find(cJSON_IsString(ac) ? ac->valuestring : "");
        if (ta < 0 || ta >= schema_action_count()) {
            snprintf(err, errsz, "unknown action (triggers_catalog lists %d)", schema_action_count());
            return -1;
        }
        cJSON_DeleteItemFromObjectCaseSensitive(obj, "ta");
        if (ta) cJSON_AddNumberToObject(obj, "ta", ta);
    }
    static const struct { const char *spec, *prop; } map[] = {
        {"event_value", "tce"}, {"value", "tc"}, {"text", "tct"}, {"chance", "cha"}, {"target", "tin"}, {"target_interface", "tin"},
        {"interface_id", "in"}, {"single_use", "su"}, {"team", "tm"}, {"sound", "es"}, {"volume", "ev"}, {"rate", "er"},
        {"emit_from", "ef"}, {"self_destruct", "sfd"}, {"action_method", "tam"}, {"action_cost", "tac"}, {"once_per_player", "sup"},
        {NULL, NULL},
    };
    for (int i = 0; map[i].spec; i++) {
        const cJSON *v = js_get(spec, map[i].spec);
        if (!v || (first_action && (!strcmp(map[i].spec, "value") || !strcmp(map[i].spec, "text")))) continue;
        if (doc_set_prop(doc, obj, map[i].prop, v, flags | SET_FORCE, err, errsz) != 0) return -1;
    }
    return 0;
}

static int t_triggers_create(Ctx *ctx, const cJSON *args, ToolResult *r) {
    Project *p = tool_project(ctx, args, r);
    if (!p) return 0;
    const cJSON *list = js_get(args, "triggers");
    if (!cJSON_IsArray(list) || !cJSON_GetArraySize(list)) return tr_error(r, "triggers_create needs triggers: [{p, s, event, action, value|text, target, ...}]");
    char err[512];
    Edit e;
    if (edit_begin(ctx, p, "create triggers", &e, err, sizeof(err)) != 0) return tr_error(r, "%s", err);
    const int n = cJSON_GetArraySize(list);
    int *ids = malloc(sizeof(int) * (size_t) n);
    int k = 0;
    const cJSON *spec;
    cJSON_ArrayForEach(spec, list) {
        cJSON *build = cJSON_Duplicate(spec, 1);
        cJSON_DeleteItemFromObjectCaseSensitive(build, "prefab");
        cJSON_AddStringToObject(build, "prefab", "TRIGGER");
        if (!js_get(build, "s")) {
            const double s[3] = {10, 10, 10};
            cJSON_AddItemToObject(build, "s", js_vec3_new(s));
        }
        cJSON *obj = objops_build(p->doc, build, objops_flags(args), 0, err, sizeof(err));
        cJSON_Delete(build);
        if (!obj || apply_trigger_spec(p->doc, obj, spec, objops_flags(args), err, sizeof(err)) != 0) {
            cJSON_Delete(obj);
            free(ids);
            edit_abort(&e);
            return tr_error(r, "triggers[%d]: %s", k, err);
        }
        ids[k++] = edit_add_object(&e, obj, -1);
    }
    if (edit_commit(&e, err, sizeof(err)) != 0) {
        free(ids);
        return tr_error(r, "%s", err);
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "created", objops_ids_json(&e, ids, k));
    edit_end(&e);
    free(ids);
    tr_json(r, o);
    return 0;
}

static int t_triggers_update(Ctx *ctx, const cJSON *args, ToolResult *r) {
    Project *p = tool_project(ctx, args, r);
    if (!p) return 0;
    const cJSON *list = js_get(args, "updates");
    if (!cJSON_IsArray(list)) return tr_error(r, "triggers_update needs updates: [{id, event?, action?, value?, text?, target?, ...}]");
    char err[512];
    Edit e;
    if (edit_begin(ctx, p, "update triggers", &e, err, sizeof(err)) != 0) return tr_error(r, "%s", err);
    const cJSON *u;
    cJSON *updated = cJSON_CreateArray();
    cJSON_ArrayForEach(u, list) {
        const int id = js_int(u, "id", 0);
        cJSON *obj = doc_object_by_id(p->doc, id, NULL);
        if (!obj || !is_trigger(obj)) {
            edit_abort(&e);
            cJSON_Delete(updated);
            return tr_error(r, "object %d is not a trigger", id);
        }
        if (apply_trigger_spec(p->doc, obj, u, objops_flags(args), err, sizeof(err)) != 0) {
            edit_abort(&e);
            cJSON_Delete(updated);
            return tr_error(r, "trigger %d: %s", id, err);
        }
        cJSON_AddItemToArray(updated, cJSON_CreateNumber(id));
    }
    if (edit_commit(&e, err, sizeof(err)) != 0) {
        cJSON_Delete(updated);
        return tr_error(r, "%s", err);
    }
    edit_end(&e);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "updated", updated);
    tr_json(r, o);
    return 0;
}

static int next_free_interface(Doc *doc) {
    int max = 0;
    for (const cJSON *o = doc_objects(doc)->child; o; o = o->next) {
        if (js_int(o, "in", 0) > max) max = js_int(o, "in", 0);
        if (js_int(o, "tin", 0) > max) max = js_int(o, "tin", 0);
    }
    return max + 1;
}

static int t_triggers_link(Ctx *ctx, const cJSON *args, ToolResult *r) {
    Project *p = tool_project(ctx, args, r);
    if (!p) return 0;
    const int trig = js_int(args, "trigger", 0);
    int n;
    int *targets = tool_ids(js_get(args, "targets"), &n);
    cJSON *t = doc_object_by_id(p->doc, trig, NULL);
    if (!t || !is_trigger(t)) {
        free(targets);
        return tr_error(r, "trigger %d is not a trigger object", trig);
    }
    if (!targets || !n) {
        free(targets);
        return tr_error(r, "triggers_link needs targets (object ids)");
    }
    char err[512];
    if (objops_check_ids(p->doc, targets, n, err, sizeof(err)) != 0) {
        free(targets);
        return tr_error(r, "%s", err);
    }
    int iface = js_int(args, "interface_id", 0);
    if (!iface) {
        // reuse the targets' common interface id when they share one
        const int first = js_int(doc_object_by_id(p->doc, targets[0], NULL), "in", 0);
        int same = first != 0;
        for (int i = 1; i < n && same; i++) same = js_int(doc_object_by_id(p->doc, targets[i], NULL), "in", 0) == first;
        iface = same ? first : next_free_interface(p->doc);
    }
    Edit e;
    if (edit_begin(ctx, p, "link trigger", &e, err, sizeof(err)) != 0) {
        free(targets);
        return tr_error(r, "%s", err);
    }
    cJSON *v = cJSON_CreateNumber(iface);
    for (int i = 0; i < n; i++) {
        cJSON *o = doc_object_by_id(p->doc, targets[i], NULL);
        if (doc_set_prop(p->doc, o, "in", v, js_bool(args, "force", 0) ? SET_FORCE : 0, err, sizeof(err)) != 0) {
            const int bad = targets[i];
            cJSON_Delete(v);
            free(targets);
            edit_abort(&e);
            return tr_error(r, "target %d: %s (pass force:true to set it anyway)", bad, err);
        }
    }
    doc_set_prop(p->doc, t, "tin", v, SET_FORCE, err, sizeof(err));
    cJSON_Delete(v);
    free(targets);
    if (edit_commit(&e, err, sizeof(err)) != 0) return tr_error(r, "%s", err);
    edit_end(&e);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "interface_id", iface);
    cJSON_AddItemToObject(o, "trigger", trigger_json(p->doc, doc_object_by_id(p->doc, trig, NULL), -1));
    tr_json(r, o);
    return 0;
}

static int t_interfaces_list(Ctx *ctx, const cJSON *args, ToolResult *r) {
    Project *p = tool_project(ctx, args, r);
    if (!p) return 0;
    cJSON *ifaces = cJSON_CreateObject();
    for (const cJSON *o = doc_objects(p->doc)->child; o; o = o->next) {
        const int in = js_int(o, "in", 0), tin = js_int(o, "tin", 0);
        char key[16];
        for (int pass = 0; pass < 2; pass++) {
            const int id = pass ? tin : in;
            if (!id) continue;
            snprintf(key, sizeof(key), "%d", id);
            cJSON *e = cJSON_GetObjectItemCaseSensitive(ifaces, key);
            if (!e) {
                e = cJSON_AddObjectToObject(ifaces, key);
                cJSON_AddArrayToObject(e, "members");
                cJSON_AddArrayToObject(e, "targeted_by");
            }
            cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(e, pass ? "targeted_by" : "members"), cJSON_CreateNumber(o->valueint));
        }
    }
    cJSON *dangling = cJSON_CreateArray();
    for (const cJSON *e = ifaces->child; e; e = e->next) {
        if (!cJSON_GetArraySize(js_get(e, "members"))) cJSON_AddItemToArray(dangling, cJSON_CreateNumber(atoi(e->string)));
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "interfaces", ifaces);
    cJSON_AddItemToObject(o, "dangling_targets", dangling);
    cJSON_AddNumberToObject(o, "next_free_id", next_free_interface(p->doc));
    tr_json(r, o);
    return 0;
}

const ToolDef g_tools_logic[] = {
    {"spawns_list", "List spawns",
     "Spawn points by index: position, team (0 any, 1, 2), direction (0..3; dir 0 faces +X, 1 +Z, 2 -X, 3 -Z; yaw_deg is the "
     "player yaw it gives), competitive flag, plus the "
     "object they stand inside (if any) and the ground height under them.",
     S({"type":"object","properties":{PROJECT_ARG}}), 1, 0, 1, 0, t_spawns_list},
    {"spawns_add", "Add spawns",
     "Add spawn points (one undo step): [{p, team, dir | yaw_deg, comp}]. snap_to_ground (default true) drops each onto the "
     "highest solid below p.y + 20.",
     S({"type":"object","properties":{PROJECT_ARG,"spawns":{"type":"array","items":{"type":"object","properties":{"p":{"type":"array"},"team":{"type":"integer","minimum":0,"maximum":2},"dir":{"type":"integer"},"yaw_deg":{"type":"number"},"comp":{"type":"boolean"}},"required":["p"]}},"snap_to_ground":{"type":"boolean","default":true}},"required":["spawns"]}),
     0, 0, 0, 0, t_spawns_add},
    {"spawns_update", "Update spawns", "Change spawns by index: [{index, p?, team?, dir? | yaw_deg?, comp?, snap_to_ground?}].",
     S({"type":"object","properties":{PROJECT_ARG,"updates":{"type":"array","items":{"type":"object","properties":{"index":{"type":"integer"},"p":{"type":"array"},"team":{"type":"integer"},"dir":{"type":"integer"},"yaw_deg":{"type":"number"},"comp":{"type":"boolean"},"snap_to_ground":{"type":"boolean"}},"required":["index"]}}},"required":["updates"]}),
     0, 0, 0, 0, t_spawns_update},
    {"spawns_delete", "Delete spawns", "Delete spawns by index.",
     S({"type":"object","properties":{PROJECT_ARG,"indices":{"type":"array","items":{"type":"integer"}}},"required":["indices"]}),
     0, 1, 0, 0, t_spawns_delete},
    {"triggers_list", "List triggers",
     "Every trigger (TRIGGER objects, AIs with Can Trigger) decoded: event, event value, action(s) with value / text, chance, "
     "single use, team, own interface id and the target interface with the object ids it reaches.",
     S({"type":"object","properties":{PROJECT_ARG}}), 1, 0, 1, 0, t_triggers_list},
    {"triggers_catalog", "Trigger catalog",
     "Trigger events (32), actions (72) and conditions of v7.1.4 with their value kind (number, text, sound, both) and whether "
     "KrunkNative simulates them natively, plus the map keys they use.",
     S({"type":"object","properties":{}}), 1, 0, 1, 0, t_triggers_catalog},
    {"triggers_create", "Create triggers",
     "Create TRIGGER objects (one undo step): [{p, s (default 10^3), event (name or id, e.g. \"onEnter\"), event_value, action "
     "(name or id, e.g. \"Give Player Score\"), value | text, chance, target (interface id), interface_id, single_use, team, "
     "sound / volume / rate / emit_from, action_method / action_cost (onInteractKey), once_per_player, props}]. actions: the v7 "
     "list (<= 5) [{action, value, value2, text, key, value_type (Input / Trigger / Player / Global Value), delay, condition "
     "(name or id), condition_values [..], sound, asset_sound, volume, rate, emit_from}] replaces action / value / text.",
     S({"type":"object","properties":{PROJECT_ARG,"triggers":{"type":"array","items":{"type":"object","properties":{"p":{"type":"array"},"s":{"type":"array"},"event":{"type":["string","integer"]},"event_value":{"type":"integer"},"action":{"type":["string","integer"]},"value":{"type":"number"},"text":{"type":"string"},"chance":{"type":"integer"},"target":{"type":"integer"},"interface_id":{"type":"integer"},"single_use":{"type":"boolean"},"team":{"type":"integer"},"actions":{"type":"array","maxItems":5,"items":{"type":"object"}},"props":{"type":"object"}}}},"force":{"type":"boolean"},"clamp":{"type":"boolean"}},"required":["triggers"]}),
     0, 0, 0, 0, t_triggers_create},
    {"triggers_update", "Update triggers",
     "Change triggers: [{id, event?, event_value?, action?, value?, text?, chance?, target?, actions?, ...}] (the fields of "
     "triggers_create; on a v7 trigger action / value / text edit its first action, actions replaces the whole list).",
     S({"type":"object","properties":{PROJECT_ARG,"updates":{"type":"array","items":{"type":"object","properties":{"id":{"type":"integer"}},"required":["id"]}}},"required":["updates"]}),
     0, 0, 0, 0, t_triggers_update},
    {"triggers_link", "Link trigger to objects",
     "Make a trigger act on objects: gives the targets an interface id (their shared one, interface_id, or the next free one) and "
     "sets the trigger's target interface to it (one undo step).",
     S({"type":"object","properties":{PROJECT_ARG,"trigger":{"type":"integer"},"targets":{"type":"array","items":{"type":"integer"}},"interface_id":{"type":"integer"},"force":{"type":"boolean"}},"required":["trigger","targets"]}),
     0, 0, 0, 0, t_triggers_link},
    {"interfaces_list", "List interfaces",
     "Interface ids: member objects (in) and the triggers / AIs targeting them (tin), dangling targets and the next free id.",
     S({"type":"object","properties":{PROJECT_ARG}}), 1, 0, 1, 0, t_interfaces_list},
    {0},
};
