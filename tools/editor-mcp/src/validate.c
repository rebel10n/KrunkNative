#define _GNU_SOURCE
#include "validate.h"
#include "jsonw.h"
#include "schema.h"
#include "util.h"
#include <bots.h>
#include <map_catalog.h>
#include <triggers.h>
// v7.1.4 publish validation (customGameConfig.js validateMap port, shared/src/map_validate.c):
// used when the tree has it; weak so the server links without it.
#if defined(__has_include)
#if __has_include(<map_validate.h>)
#include <map_validate.h>
#pragma weak map_validate
#define KN_HAVE_V7_VALIDATE 1
#endif
#endif
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------- native map

int native_map_load(const Doc *doc, NativeMap *out, char *err, const size_t errsz) {
    memset(out, 0, sizeof(*out));
    out->radius = game_constants.player_scale + game_constants.collision_padding;
    out->height = game_constants.player_height;
    out->climb = game_constants.climb_height;
    out->json = doc_map_form(doc);
    if (!out->json) {
        snprintf(err, errsz, "out of memory");
        return -1;
    }
    map_sanitize_json(out->json);
    const cJSON *objects = js_get(out->json, "objects");
    const int n = cJSON_GetArraySize(objects);
    out->ids = calloc((size_t) (n ? n : 1), sizeof(int));
    int k = 0;
    for (const cJSON *o = objects ? objects->child : NULL; o; o = o->next) {
        if (map_raw_object_kept(o)) out->ids[k++] = o->valueint;
    }
    out->map = map_init(out->json);
    if (!out->map) {
        snprintf(err, errsz, "KrunkNative's map loader (map_init) rejected the map (objects / spawns must be arrays)");
        native_map_free(out);
        return -1;
    }
    return 0;
}

void native_map_free(NativeMap *nm) {
    if (nm->map) {
        map_fini(nm->map);
        free(nm->map);
    }
    cJSON_Delete(nm->json);
    free(nm->ids);
    memset(nm, 0, sizeof(*nm));
}

int native_spawn_blocker(const NativeMap *nm, const double p[3]) {
    const double r = nm->radius - 0.25, eps = 0.05;
    for (size_t i = 0; i < nm->map->object_count; i++) {
        const Object *o = nm->map->objects[i];
        if (!bot_object_blocks_player(o) || o->ramp) continue;
        const double x0 = o->position.x - o->scale.x / 2, x1 = o->position.x + o->scale.x / 2;
        const double z0 = o->position.z - o->scale.z / 2, z1 = o->position.z + o->scale.z / 2;
        const double y0 = o->position.y, y1 = o->position.y + o->scale.y;
        if (p[0] + r <= x0 || p[0] - r >= x1 || p[2] + r <= z0 || p[2] - r >= z1) continue;
        if (p[1] + nm->height <= y0 + eps || p[1] >= y1 - eps) continue;
        return (int) i;
    }
    return -1;
}

double native_ground(const NativeMap *nm, const double p[3]) {
    const double r = nm->radius * 0.5;
    double best = -1e30;
    for (size_t i = 0; i < nm->map->object_count; i++) {
        const Object *o = nm->map->objects[i];
        if (!bot_object_blocks_player(o)) continue;
        if (p[0] + r <= o->position.x - o->scale.x / 2 || p[0] - r >= o->position.x + o->scale.x / 2) continue;
        if (p[2] + r <= o->position.z - o->scale.z / 2 || p[2] - r >= o->position.z + o->scale.z / 2) continue;
        const double top = o->position.y + o->scale.y;
        if (top <= p[1] + nm->climb + 0.01 && top > best) best = top;
    }
    return best;
}

// ---------------------------------------------------------------- diagnostics

typedef struct {
    cJSON *list;
    int errors, warnings, infos;
    int max;
    int truncated;
    cJSON *codes; // {code: count}
} Diags;

static cJSON *diag(Diags *d, const char *severity, const char *code, const char *fmt, ...) __attribute__((format(printf, 4, 5)));

static cJSON *diag(Diags *d, const char *severity, const char *code, const char *fmt, ...) {
    if (severity[0] == 'e') d->errors++;
    else if (severity[0] == 'w') d->warnings++;
    else d->infos++;
    cJSON *c = cJSON_GetObjectItemCaseSensitive(d->codes, code);
    if (c) cJSON_SetNumberValue(c, c->valuedouble + 1);
    else cJSON_AddNumberToObject(d->codes, code, 1);
    if (cJSON_GetArraySize(d->list) >= d->max) {
        d->truncated++;
        return NULL;
    }
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "severity", severity);
    cJSON_AddStringToObject(o, "code", code);
    cJSON_AddStringToObject(o, "message", msg);
    cJSON_AddItemToArray(d->list, o);
    return o;
}

static void diag_ids(cJSON *o, const int a, const int b) {
    if (!o) return;
    cJSON *ids = cJSON_AddArrayToObject(o, "object_ids");
    cJSON_AddItemToArray(ids, cJSON_CreateNumber(a));
    if (b) cJSON_AddItemToArray(ids, cJSON_CreateNumber(b));
}

static void diag_spawn(cJSON *o, const int index) {
    if (o) cJSON_AddNumberToObject(o, "spawn", index);
}

static int all_finite(const cJSON *arr) {
    const cJSON *it;
    cJSON_ArrayForEach(it, arr) if (!cJSON_IsNumber(it) || !isfinite(it->valuedouble)) return 0;
    return 1;
}

// value check of a known property: dry-run doc_set_prop on a scratch object
static int prop_value_ok(Doc *doc, const cJSON *obj, const SchemaProp *p, const cJSON *v, char *err, const size_t errsz) {
    if (p->type == PT_ANY || p->type == PT_COLOR || schema_core_key(p->key)) return 1;
    if (p->type == PT_BOOL_PRESENT || p->type == PT_BOOL_ABSENT) return 1;
    cJSON *scratch = cJSON_Duplicate(obj, 0);
    for (const cJSON *k = obj->child; k; k = k->next) if (!strcmp(k->string, "i") || !strcmp(k->string, "id")) cJSON_AddItemToObject(scratch, k->string, cJSON_Duplicate(k, 1));
    cJSON *probe = cJSON_Duplicate(v, 1);
    if (p->type == PT_TEXTURE && cJSON_IsNumber(probe) && probe->valuedouble == 0) {
        cJSON_Delete(probe);
        cJSON_Delete(scratch);
        return 1;
    }
    const int rc = doc_set_prop(doc, scratch, p->prop, probe, SET_FORCE, err, errsz);
    cJSON_Delete(probe);
    cJSON_Delete(scratch);
    return rc == 0;
}

static int texture_file_exists(const Ctx *ctx, const char *src) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/assets/textures/%s.png", ctx->root, src);
    return file_exists(path);
}

static const int k_target_actions[] = {4, 5, 6, 7, 8, 11, 12, 21, 25, 26, 27, 28, 41, 42, 43, 44, 48, -1};

// ---------------------------------------------------------------- checks

static void check_map_level(Ctx *ctx, Doc *doc, Diags *d) {
    const cJSON *root = doc->root;
    const cJSON *name = js_get(root, "name");
    if (!cJSON_IsString(name) || !name->valuestring[0]) diag(d, "warning", "map.no_name", "the map has no name");
    else if (strlen(name->valuestring) > (size_t) schema_limit("nameMaxChars", 16))
        diag(d, "info", "map.name_long", "name \"%s\" is longer than the %g characters v7.1.4 publishes", name->valuestring, schema_limit("nameMaxChars", 16));
    const char *wel = js_str(root, "welMsg", "");
    if (strlen(wel) > (size_t) schema_limit("welcomeMaxChars", 70)) diag(d, "warning", "map.welcome_long", "welMsg is longer than %g characters", schema_limit("welcomeMaxChars", 70));
    for (const cJSON *k = root->child; k; k = k->next) {
        const cJSON *mk = schema_map_key(k->string);
        if (!mk) {
            diag(d, "info", "map.unknown_key", "top-level key \"%s\" is not in the schema (kept as-is)", k->string);
            continue;
        }
        const char *type = js_str(mk, "type", "any");
        if (!strcmp(type, "number")) {
            if (!cJSON_IsNumber(k) || !isfinite(k->valuedouble)) diag(d, "error", "map.bad_value", "\"%s\" should be a finite number", k->string);
            else if ((cJSON_IsNumber(js_get(mk, "min")) && k->valuedouble < js_num(mk, "min", 0)) ||
                     (cJSON_IsNumber(js_get(mk, "max")) && k->valuedouble > js_num(mk, "max", 0)))
                diag(d, "warning", "map.out_of_range", "\"%s\" = %g is outside [%g, %g]", k->string, k->valuedouble, js_num(mk, "min", -INFINITY), js_num(mk, "max", INFINITY));
        } else if (!strcmp(type, "hex_or_int")) {
            char hex[8];
            if (!color_parse(k, hex)) diag(d, "error", "map.bad_color", "\"%s\" is not a colour", k->string);
        } else if (!strcmp(type, "array") && !cJSON_IsArray(k)) {
            diag(d, "error", "map.bad_type", "\"%s\" should be an array", k->string);
        } else if (!strcmp(type, "object") && !cJSON_IsObject(k)) {
            diag(d, "error", "map.bad_type", "\"%s\" should be an object", k->string);
        }
    }
    const cJSON *spawns = js_get(root, "spawns");
    if (!doc->editor_form && !cJSON_IsArray(spawns)) diag(d, "error", "map.no_spawns", "the map has no \"spawns\" array: the game refuses to load it");
    char *text = jsonw_print(root, JSONW_COMPACT);
    const size_t size = text ? strlen(text) : 0;
    free(text);
    const double limit = schema_limit("publishMaxChars", 500000);
    if (size > limit) diag(d, "error", "map.too_large", "the map is %zu characters: v7.1.4 publishes at most %.0f (map_compile pools sizes / colours)", size, limit);
    else if (size > limit * 0.8) diag(d, "warning", "map.near_size_limit", "the map is %zu characters, %.0f%% of the %.0f publish limit", size, 100.0 * size / limit, limit);
    (void) ctx;
}

static void check_objects(Ctx *ctx, Doc *doc, Diags *d, const cJSON *colors_json) {
    const double range = schema_limit("positionRange", 500000);
    const int palette = cJSON_GetArraySize(colors_json);
    const cJSON *xyz = js_get(doc->root, "xyz");
    int collidable = 0, total = 0, signs = 0;
    cJSON *unknown_keys = cJSON_CreateObject();
    char *tex_missing = calloc((size_t) schema_texture_count() + 1, 1);
    int index = -1;
    for (const cJSON *o = doc_objects(doc)->child; o; o = o->next) {
        index++;
        const int id = o->valueint;
        if (!cJSON_IsObject(o)) {
            diag_ids(diag(d, "error", "object.not_object", "objects[%d] is not a JSON object", index), id, 0);
            continue;
        }
        total++;
        const int prefab = obj_prefab(o);
        const SchemaPrefab *pf = schema_prefab(prefab);
        if (!obj_prefab_raw_valid(o)) diag_ids(diag(d, "error", "object.unknown_prefab", "object %d names an unknown prefab (the game loads it as a CUBE)", id), id, 0);
        else if (map_raw_object_prefab(o) != prefab && pf)
            diag_ids(diag(d, "warning", "object.not_native", "%s (prefab %d, %s) is not implemented by KrunkNative's native loader yet: it loads as prefab %d", pf->name, prefab, pf->since, map_raw_object_prefab(o)), id, 0);
        const cJSON *p = js_get(o, "p"), *s = js_get(o, "s"), *si = js_get(o, "si");
        if (!cJSON_IsArray(p) || cJSON_GetArraySize(p) < 3) diag_ids(diag(d, "error", "object.no_position", "object %d has no p [x,y,z]: the game drops it", id), id, 0);
        else if (!all_finite(p)) diag_ids(diag(d, "error", "object.nan", "object %d has a non-numeric / non-finite position", id), id, 0);
        if (!s && !si) diag_ids(diag(d, "error", "object.no_size", "object %d has no s (or si): the game drops it", id), id, 0);
        else if (s && (!cJSON_IsArray(s) || cJSON_GetArraySize(s) < 3 || !all_finite(s))) diag_ids(diag(d, "error", "object.nan", "object %d has a bad size", id), id, 0);
        else if (!s && si && (!cJSON_IsNumber(si) || !cJSON_IsArray(xyz) || (int) si->valuedouble * 3 + 2 >= cJSON_GetArraySize(xyz)))
            diag_ids(diag(d, "error", "object.bad_size_index", "object %d: si points outside the xyz size pool", id), id, 0);
        double pos[3], size[3], rot[3];
        obj_pos(o, pos);
        obj_size(doc, o, size);
        if (js_vec3(js_get(o, "r"), rot) && !(isfinite(rot[0]) && isfinite(rot[1]) && isfinite(rot[2])))
            diag_ids(diag(d, "error", "object.nan", "object %d has a non-finite rotation", id), id, 0);
        for (int k = 0; k < 3; k++) {
            if (fabs(pos[k]) > range) {
                diag_ids(diag(d, "error", "object.out_of_range", "object %d position %g is outside +-%g", id, pos[k], range), id, 0);
                break;
            }
        }
        for (int k = 0; k < 3; k++) {
            if (size[k] < 0) {
                diag_ids(diag(d, "error", "object.negative_size", "object %d has a negative size", id), id, 0);
                break;
            }
            if (size[k] == 0 && pf && !schema_prefab_has_flag(prefab, "scaleWithSize") && strcmp(pf->name, "PLANE")) {
                diag_ids(diag(d, "warning", "object.zero_size", "object %d (%s) has a zero size component", id, pf->name), id, 0);
                break;
            }
        }
        if (obj_collidable(o) && pf && !schema_prefab_has_flag(prefab, "noCol")) collidable++;
        if (pf && schema_prefab_has_flag(prefab, "hasText")) signs++;
        // properties
        for (const cJSON *k = o->child; k; k = k->next) {
            if (schema_core_key(k->string)) continue;
            if (!strcmp(k->string, "ci") || !strcmp(k->string, "ei")) {
                if (pf && schema_prefab_has_flag(prefab, "ai") && k->string[0] == 'c') continue;
                if (!cJSON_IsNumber(k) || k->valuedouble < 0 || k->valuedouble >= palette)
                    diag_ids(diag(d, "error", "object.bad_color_index", "object %d: %s = %g is outside the %d-colour palette", id, k->string, cJSON_IsNumber(k) ? k->valuedouble : -1, palette), id, 0);
                continue;
            }
            const SchemaProp *sp = schema_prop_by_key(k->string);
            if (!sp) {
                cJSON *c = cJSON_GetObjectItemCaseSensitive(unknown_keys, k->string);
                if (c) cJSON_SetNumberValue(c, c->valuedouble + 1);
                else cJSON_AddNumberToObject(unknown_keys, k->string, 1);
                continue;
            }
            if (!schema_prop_allowed(sp, prefab))
                diag_ids(diag(d, "info", "object.prop_not_applicable", "object %d (%s) carries %s (%s), which its prefab does not use", id, pf ? pf->name : "?", sp->prop, sp->key), id, 0);
            char err[256];
            if (!prop_value_ok(doc, o, sp, k, err, sizeof(err))) diag_ids(diag(d, "warning", "object.bad_value", "object %d: %s", id, err), id, 0);
        }
        if (pf && schema_prefab_has_flag(prefab, "texturable")) {
            const int t = obj_texture(o);
            if (t < 0 || t >= schema_texture_count()) diag_ids(diag(d, "error", "object.bad_texture", "object %d: texture %d does not exist", id, t), id, 0);
            else if (tex_missing && !tex_missing[t]) {
                const cJSON *tex = cJSON_GetArrayItem(js_get(schema_json(), "textures"), t);
                const char *src = js_str(tex, "src", NULL);
                tex_missing[t] = 1;
                if (src && strcmp(js_str(tex, "name", ""), "DEFAULT") && !texture_file_exists(ctx, src))
                    diag(d, "warning", "texture.missing_file", "texture %s (assets/textures/%s.png) is not in the KrunkNative assets", js_str(tex, "name", "?"), src);
            }
            if (js_get(o, "at")) diag_ids(diag(d, "info", "object.asset_texture", "object %d uses custom texture asset %s (not available offline)", id, cJSON_IsString(js_get(o, "at")) ? js_get(o, "at")->valuestring : "(id)"), id, 0);
        }
    }
    for (const cJSON *k = unknown_keys->child; k; k = k->next)
        diag(d, "info", "object.unknown_key", "%d object(s) carry key \"%s\", which is not in the schema (kept as-is)", (int) k->valuedouble, k->string);
    cJSON_Delete(unknown_keys);
    free(tex_missing);
    const double ol = schema_limit("objectLimit", 5000), cl = schema_limit("objectColLimit", 4500), sl = schema_limit("signLimit", 100);
    if (total > ol) diag(d, "error", "limit.objects", "%d objects: the v7.1.4 limit is %.0f (%.0f for verified creators)", total, ol, schema_limit("objectLimitF", 7500));
    if (collidable > cl) diag(d, "error", "limit.collidable", "%d collidable objects: the v7.1.4 limit is %.0f", collidable, cl);
    if (signs > sl) diag(d, "warning", "limit.signs", "%d text objects: the limit is %.0f", signs, sl);
}

static void check_logic(Doc *doc, Diags *d) {
    const int max_iface = (int) schema_limit("interfaceIDMax", 2000);
    const int events = schema_event_count(), actions = schema_action_count();
    // interface ids present
    int cap = 64, n_if = 0;
    int *ifaces = malloc(sizeof(int) * (size_t) cap);
    for (const cJSON *o = doc_objects(doc)->child; o; o = o->next) {
        const int in = js_int(o, "in", 0);
        if (!in) continue;
        if (in < 0 || in > max_iface) diag_ids(diag(d, "error", "interface.out_of_range", "object %d: interface id %d is outside 0..%d", o->valueint, in, max_iface), o->valueint, 0);
        if (n_if == cap) {
            cap *= 2;
            int *t = realloc(ifaces, sizeof(int) * (size_t) cap);
            if (!t) break;
            ifaces = t;
        }
        ifaces[n_if++] = in;
    }
    // teleporter channels
    int senders[101] = {0}, receivers[101] = {0};
    for (const cJSON *o = doc_objects(doc)->child; o; o = o->next) {
        const int prefab = obj_prefab(o);
        const SchemaPrefab *pf = schema_prefab(prefab);
        if (!pf) continue;
        if (!strcmp(pf->name, "TELEPORTER")) {
            const int ch = js_int(o, "ch", 0), node = js_int(o, "n", 0);
            if (ch >= 0 && ch <= 100) {
                if (node == 0 || node == 2) senders[ch]++;
                if (node == 1 || node == 2) receivers[ch]++;
            }
        }
        const int is_trigger = !strcmp(pf->name, "TRIGGER") || (schema_prefab_has_flag(prefab, "ai") && js_get(o, "ct"));
        if (!is_trigger) continue;
        const int id = o->valueint;
        const int te = js_int(o, "te", 0), ta = js_int(o, "ta", 0);
        if (te < 0 || te >= events) diag_ids(diag(d, "error", "trigger.bad_event", "trigger %d: event %d does not exist (0..%d)", id, te, events - 1), id, 0);
        else if (te >= 16) diag_ids(diag(d, "warning", "trigger.event_not_native", "trigger %d: event %s (v7) is not simulated by KrunkNative yet", id, schema_event_name(te)), id, 0);
        const cJSON *tas = js_get(o, "tas");
        if (cJSON_IsArray(tas)) {
            if (cJSON_GetArraySize(tas) > schema_limit("triggerActionLimit", 5))
                diag_ids(diag(d, "error", "trigger.too_many_actions", "trigger %d has %d actions (limit %.0f)", id, cJSON_GetArraySize(tas), schema_limit("triggerActionLimit", 5)), id, 0);
        } else if (js_get(o, "ta") || is_trigger) {
            if (ta < 0 || ta >= actions) diag_ids(diag(d, "error", "trigger.bad_action", "trigger %d: action %d does not exist (0..%d)", id, ta, actions - 1), id, 0);
            else if (ta >= 32) diag_ids(diag(d, "warning", "trigger.action_not_native", "trigger %d: action %s (v7) is not simulated by KrunkNative yet", id, schema_action_name(ta)), id, 0);
        }
        const cJSON *tct = js_get(o, "tct");
        if (cJSON_IsString(tct) && !trigger_text_valid(tct->valuestring))
            diag_ids(diag(d, "error", "trigger.bad_text", "trigger %d: value text \"%.40s\" has characters / length the game rejects (it is dropped)", id, tct->valuestring), id, 0);
        const int tin = js_int(o, "tin", 0);
        int needs_target = 0;
        for (int i = 0; k_target_actions[i] >= 0; i++) needs_target |= k_target_actions[i] == ta;
        if (tin) {
            int found = 0;
            for (int i = 0; i < n_if && !found; i++) found = ifaces[i] == tin;
            if (!found) diag_ids(diag(d, "warning", "trigger.dangling_target", "trigger %d targets interface %d, which no object has (in = %d)", id, tin, tin), id, 0);
        } else if (needs_target && !cJSON_IsArray(tas)) {
            diag_ids(diag(d, "warning", "trigger.no_target", "trigger %d: action \"%s\" needs a target interface (tin)", id, schema_action_name(ta) ? schema_action_name(ta) : "?"), id, 0);
        }
    }
    for (int ch = 0; ch <= 100; ch++) {
        if (senders[ch] && !receivers[ch]) diag(d, "warning", "teleporter.no_receiver", "teleporter channel %d has %d sender(s) but no receiver (node 1 or 2)", ch, senders[ch]);
    }
    free(ifaces);
}

static void check_spawns(const NativeMap *nm, Diags *d, const int reachability, cJSON *report) {
    const Map *map = nm->map;
    const int n = (int) map->spawn_count;
    if (!n) {
        diag(d, "error", "spawn.none", "the map has no spawn points");
        return;
    }
    const double sl = schema_limit("spawnLimit", 20);
    if (n > sl) diag(d, "warning", "limit.spawns", "%d spawns: the editor limit is %.0f", n, sl);
    cJSON *arr = cJSON_AddArrayToObject(report, "spawns");
    for (int i = 0; i < n; i++) {
        const Spawn *s = map->spawns[i];
        const double p[3] = {s->position.x, s->position.y, s->position.z};
        cJSON *e = cJSON_CreateObject();
        cJSON_AddNumberToObject(e, "index", i);
        cJSON_AddItemToObject(e, "p", js_vec3_new(p));
        cJSON_AddNumberToObject(e, "team", s->team);
        const int blocker = native_spawn_blocker(nm, p);
        if (blocker >= 0) {
            const int bid = nm->ids[blocker];
            cJSON *dg = diag(d, "error", "spawn.inside_solid", "spawn %d at (%.1f, %.1f, %.1f) is inside a solid object (id %d)", i, p[0], p[1], p[2], bid);
            diag_spawn(dg, i);
            diag_ids(dg, bid, 0);
            cJSON_AddNumberToObject(e, "inside_object", bid);
        }
        const double g = native_ground(nm, p);
        if (g < -1e29 || g < map->death_y) {
            diag_spawn(diag(d, "error", "spawn.over_void", "spawn %d at (%.1f, %.1f, %.1f) has no ground below: the player falls out of the map", i, p[0], p[1], p[2]), i);
            cJSON_AddNullToObject(e, "ground");
        } else {
            cJSON_AddNumberToObject(e, "ground", js_round(g, 2));
            if (p[1] - g > 40) diag_spawn(diag(d, "info", "spawn.high_above_ground", "spawn %d is %.0f units above the ground below it", i, p[1] - g), i);
        }
        for (int j = 0; j < i; j++) {
            const Spawn *t = map->spawns[j];
            const double dx = s->position.x - t->position.x, dz = s->position.z - t->position.z, dy = fabs(s->position.y - t->position.y);
            if (hypot(dx, dz) < nm->radius * 2 && dy < nm->height) {
                cJSON *dg = diag(d, "warning", "spawn.overlap", "spawns %d and %d overlap (%.1f units apart)", j, i, hypot(dx, dz));
                diag_spawn(dg, i);
            }
        }
        if (s->team < 0 || s->team > 2) diag_spawn(diag(d, "warning", "spawn.bad_team", "spawn %d has team %d (0 any, 1, 2)", i, s->team), i);
        cJSON_AddItemToArray(arr, e);
    }
    int t1 = 0, t2 = 0;
    for (int i = 0; i < n; i++) {
        t1 += map->spawns[i]->team == 1;
        t2 += map->spawns[i]->team == 2;
    }
    if ((t1 == 0) != (t2 == 0)) diag(d, "warning", "spawn.team_imbalance", "only one team has team spawns (team 1: %d, team 2: %d)", t1, t2);
    if (!reachability || n < 2) return;
    Game g;
    memset(&g, 0, sizeof(g));
    g.map = nm->map;
    const double t0 = now_seconds();
    BotNav *nav = bot_nav_for(&g);
    if (!nav) {
        diag(d, "info", "reach.no_nav", "the navigation grid could not be built (map too large or no walkable surface): reachability skipped");
        game_bots_free(&g);
        return;
    }
    const double built = now_seconds() - t0;
    int *reach = calloc((size_t) n, sizeof(int));
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            if (i == j) continue;
            BotPath path;
            const vec3 a = map->spawns[i]->position, b = map->spawns[j]->position;
            if (bot_nav_find_path(&g, a, b, &path) && path.reached) reach[i]++;
        }
    }
    cJSON *rr = cJSON_AddObjectToObject(report, "reachability");
    cJSON_AddNumberToObject(rr, "nav_nodes", bot_nav_node_count(nav));
    cJSON_AddNumberToObject(rr, "nav_build_ms", js_round(built * 1000, 1));
    cJSON_AddNumberToObject(rr, "total_ms", js_round((now_seconds() - t0) * 1000, 1));
    cJSON *counts = cJSON_AddArrayToObject(rr, "reaches");
    for (int i = 0; i < n; i++) {
        cJSON_AddItemToArray(counts, cJSON_CreateNumber(reach[i]));
        if (reach[i] == 0) diag_spawn(diag(d, "error", "spawn.unreachable", "spawn %d cannot walk / jump to any other spawn (isolated)", i), i);
        else if (reach[i] < (n - 1) / 2) diag_spawn(diag(d, "warning", "spawn.poorly_connected", "spawn %d reaches only %d of %d other spawns", i, reach[i], n - 1), i);
    }
    free(reach);
    game_bots_free(&g);
}

cJSON *validate_project(Ctx *ctx, Project *p, const cJSON *opts) {
    Diags d = {.list = cJSON_CreateArray(), .max = js_int(opts, "max_diagnostics", 300), .codes = cJSON_CreateObject()};
    if (d.max < 1) d.max = 1;
    cJSON *report = cJSON_CreateObject();
    const double t0 = now_seconds();
    check_map_level(ctx, p->doc, &d);
    check_objects(ctx, p->doc, &d, js_get(p->doc->root, "colors"));
    check_logic(p->doc, &d);
    char err[512];
    NativeMap nm;
    if (native_map_load(p->doc, &nm, err, sizeof(err)) != 0) {
        diag(&d, "error", "map.load_failed", "%s", err);
    } else {
        check_spawns(&nm, &d, js_bool(opts, "reachability", 1), report);
        native_map_free(&nm);
    }
    // v7.1.4's own validateMap (what publishing / a v7 server would answer)
    const char *v7_status = "v7 validateMap port not in this build";
#ifdef KN_HAVE_V7_VALIDATE
    if (map_validate) {
        cJSON *form7 = doc_map_form(p->doc);
        MapValidateOptions vo;
        memset(&vo, 0, sizeof(vo));
        vo.featured = (unsigned char) js_bool(opts, "featured", 0);
        static MapIssue issues[256];
        size_t v7_errors = 0;
        const size_t total = map_validate(form7, &vo, issues, 256, &v7_errors);
        const cJSON *objs = js_get(form7, "objects");
        for (size_t i = 0; i < total && i < 256; i++) {
            char code[64];
            snprintf(code, sizeof(code), "v7.%s", issues[i].code);
            cJSON *dg = diag(&d, issues[i].severity == MAP_ISSUE_ERROR ? "error" : "warning", code, "v7.1.4 validateMap: %s", issues[i].message);
            if (dg && issues[i].object >= 0) {
                const cJSON *o = cJSON_GetArrayItem(objs, issues[i].object);
                if (o && o->valueint) diag_ids(dg, o->valueint, 0);
                else cJSON_AddNumberToObject(dg, "object_index", issues[i].object);
            }
            if (dg && issues[i].action >= 0) cJSON_AddNumberToObject(dg, "action", issues[i].action);
        }
        v7_status = v7_errors ? "v7.1.4 would reject this map (see v7.* errors)" : "v7.1.4 validateMap accepts this map";
        cJSON_Delete(form7);
    }
#endif
    // KrunkScript
    cJSON *ks = cJSON_CreateArray();
    int available = 0;
    cJSON *form = doc_map_form(p->doc);
    ks_bridge_check_scripts(form, ks, &available);
    cJSON_Delete(form);
    const cJSON *it;
    cJSON_ArrayForEach(it, ks) {
        const char *sev = js_str(it, "severity", "info");
        cJSON *dg = diag(&d, sev, js_str(it, "code", "script"), "%s", js_str(it, "message", ""));
        if (dg) {
            if (js_get(it, "side")) cJSON_AddStringToObject(dg, "side", js_str(it, "side", ""));
            if (js_get(it, "line")) cJSON_AddNumberToObject(dg, "line", js_int(it, "line", 0));
            if (js_get(it, "col")) cJSON_AddNumberToObject(dg, "col", js_int(it, "col", 0));
        }
    }
    cJSON_Delete(ks);
    cJSON *out = cJSON_CreateObject();
    cJSON_AddStringToObject(out, "project", p->id);
    cJSON_AddBoolToObject(out, "ok", d.errors == 0);
    cJSON_AddNumberToObject(out, "errors", d.errors);
    cJSON_AddNumberToObject(out, "warnings", d.warnings);
    cJSON_AddNumberToObject(out, "infos", d.infos);
    cJSON_AddItemToObject(out, "codes", d.codes);
    cJSON_AddItemToObject(out, "diagnostics", d.list);
    if (d.truncated) cJSON_AddNumberToObject(out, "truncated", d.truncated);
    cJSON_AddStringToObject(out, "krunkscript", ks_bridge_status());
    cJSON_AddStringToObject(out, "v7_publish", v7_status);
    for (cJSON *k = report->child; k;) {
        cJSON *next = k->next;
        cJSON_AddItemToObject(out, k->string, cJSON_DetachItemViaPointer(report, k));
        k = next;
    }
    cJSON_Delete(report);
    cJSON_AddNumberToObject(out, "ms", js_round((now_seconds() - t0) * 1000, 1));
    return out;
}
