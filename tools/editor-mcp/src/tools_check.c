// map_validate, map_compile and playtest.
#define _GNU_SOURCE
#include "jsonw.h"
#include "mcp.h"
#include "schema.h"
#include "util.h"
#include "validate.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int t_playtest(Ctx *ctx, const cJSON *args, ToolResult *r); // playtest.c

static int t_validate(Ctx *ctx, const cJSON *args, ToolResult *r) {
    Project *p = tool_project(ctx, args, r);
    if (!p) return 0;
    cJSON *out = validate_project(ctx, p, args);
    const char *min = tool_str(args, "min_severity", "info");
    if (strcmp(min, "info")) {
        cJSON *list = cJSON_GetObjectItemCaseSensitive(out, "diagnostics");
        for (cJSON *d = list ? list->child : NULL; d;) {
            cJSON *next = d->next;
            const char *sev = js_str(d, "severity", "info");
            if (!strcmp(sev, "info") || (!strcmp(min, "error") && !strcmp(sev, "warning"))) cJSON_Delete(cJSON_DetachItemViaPointer(list, d));
            d = next;
        }
    }
    tr_text(r, "%s: %d error(s), %d warning(s), %d info", p->name, js_int(out, "errors", 0), js_int(out, "warnings", 0), js_int(out, "infos", 0));
    tr_json(r, out);
    return 0;
}

// ---------------------------------------------------------------- compile

static void round_vec(cJSON *obj, const char *key, const int decimals) {
    double v[3];
    if (!js_vec3(js_get(obj, key), v)) return;
    for (int k = 0; k < 3; k++) v[k] = js_round(v[k], decimals);
    if (!strcmp(key, "r") && !v[0] && !v[1] && !v[2]) {
        cJSON_DeleteItemFromObjectCaseSensitive(obj, "r");
        return;
    }
    cJSON_ReplaceItemInObjectCaseSensitive(obj, key, js_vec3_new(v));
}

static void strip_defaults(cJSON *obj) {
    for (cJSON *k = obj->child; k;) {
        cJSON *next = k->next;
        const SchemaProp *p = schema_prop_by_key(k->string);
        int drop = 0;
        if (!strcmp(k->string, "i") && cJSON_IsNumber(k) && k->valuedouble == 0) drop = 1;
        else if (p && !schema_core_key(p->key) && strcmp(p->key, "tm")) {
            if (p->type == PT_BOOL_PRESENT && (cJSON_IsFalse(k) || cJSON_IsNull(k) || (cJSON_IsNumber(k) && k->valuedouble == 0))) drop = 1;
            else if ((p->type == PT_NUM || p->type == PT_INT) && p->def && cJSON_IsNumber(p->def) && cJSON_IsNumber(k) && k->valuedouble == p->def->valuedouble) drop = 1;
            else if (p->type == PT_STR && cJSON_IsString(p->def) && cJSON_IsString(k) && !strcmp(p->def->valuestring, k->valuestring)) drop = 1;
        }
        if (drop) cJSON_Delete(cJSON_DetachItemViaPointer(obj, k));
        k = next;
    }
    // legacy "id" prefab key -> "i"
    const cJSON *id = js_get(obj, "id");
    if (cJSON_IsNumber(id) && !js_get(obj, "i")) {
        const double v = id->valuedouble;
        cJSON_DeleteItemFromObjectCaseSensitive(obj, "id");
        if (v != 0) cJSON_AddNumberToObject(obj, "i", v);
    }
}

// size pool (xyz / si): triplets mirrored in a flat array so lookups do not walk cJSON lists
typedef struct {
    double *v;
    int n, cap;
} Pool;

static int pool_index(Pool *pool, cJSON *xyz, const double v[3]) {
    for (int i = 0; i < pool->n; i++) {
        const double *e = pool->v + i * 3;
        if (e[0] == v[0] && e[1] == v[1] && e[2] == v[2]) return i;
    }
    if (pool->n == pool->cap) {
        pool->cap = pool->cap ? pool->cap * 2 : 256;
        double *nv = realloc(pool->v, sizeof(double) * 3 * (size_t) pool->cap);
        if (!nv) return 0;
        pool->v = nv;
    }
    memcpy(pool->v + pool->n * 3, v, sizeof(double) * 3);
    for (int k = 0; k < 3; k++) cJSON_AddItemToArray(xyz, cJSON_CreateNumber(v[k]));
    return pool->n++;
}

static int palette_add(cJSON *palette, const char *hex) {
    int i = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, palette) {
        if (cJSON_IsString(it) && !strcmp(it->valuestring, hex)) return i;
        i++;
    }
    cJSON_AddItemToArray(palette, cJSON_CreateString(hex));
    return i;
}

static int t_compile(Ctx *ctx, const cJSON *args, ToolResult *r) {
    Project *p = tool_project(ctx, args, r);
    if (!p) return 0;
    const int decimals = js_int(args, "round", 2);
    const int pool = js_bool(args, "pool_sizes", 1);
    const int fold = js_bool(args, "fold_colors", 1);
    const int defaults = js_bool(args, "strip_defaults", 1);
    const int encode = js_bool(args, "encode_scripts", 1);
    char err[512];

    // validation first: errors block unless force
    cJSON *val = validate_project(ctx, p, NULL);
    const int errors = js_int(val, "errors", 0);
    if (errors && !js_bool(args, "force", 0)) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "error", "validation errors: fix them (or pass force:true)");
        cJSON_AddItemToObject(o, "validation", val);
        tr_json(r, o);
        r->is_error = 1;
        return 0;
    }

    char *before_text = jsonw_print(p->doc->root, JSONW_COMPACT);
    const size_t before = before_text ? strlen(before_text) : 0;
    free(before_text);
    cJSON *m = doc_map_form(p->doc);
    cJSON *objects = cJSON_GetObjectItemCaseSensitive(m, "objects");
    Doc view = {.root = m}; // read-only view for palette / pool lookups
    cJSON *palette = cJSON_CreateArray();
    cJSON *xyz = cJSON_CreateArray();
    Pool pool_tab = {0};
    int xyz_n = 0, dropped = 0, cpx = 0;
    for (cJSON *o = objects->child; o;) {
        cJSON *next = o->next;
        const int prefab = obj_prefab(o);
        if (js_bool(args, "drop_editor_only", 1) && schema_prefab_has_flag(prefab, "noExport")) {
            cJSON_Delete(cJSON_DetachItemViaPointer(objects, o));
            dropped++;
            o = next;
            continue;
        }
        if (js_get(o, "cpx")) cpx++;
        // sizes / colours resolved against the source document
        double s[3];
        obj_size(&view, o, s);
        cJSON_DeleteItemFromObjectCaseSensitive(o, "si");
        cJSON_DeleteItemFromObjectCaseSensitive(o, "s");
        const int ai = schema_prefab_has_flag(prefab, "ai");
        char hex_c[8], hex_e[8];
        const int has_c = obj_color(&view, o, "c", hex_c), has_e = obj_color(&view, o, "e", hex_e);
        cJSON_DeleteItemFromObjectCaseSensitive(o, "c");
        cJSON_DeleteItemFromObjectCaseSensitive(o, "e");
        if (!ai) cJSON_DeleteItemFromObjectCaseSensitive(o, "ci");
        cJSON_DeleteItemFromObjectCaseSensitive(o, "ei");
        round_vec(o, "p", decimals);
        round_vec(o, "r", decimals);
        if (defaults) strip_defaults(o);
        for (int k = 0; k < 3; k++) s[k] = js_round(s[k], decimals);
        if (pool) {
            cJSON_AddNumberToObject(o, "si", pool_index(&pool_tab, xyz, s));
            xyz_n = pool_tab.n;
        } else {
            cJSON_AddItemToObject(o, "s", js_vec3_new(s));
        }
        if (has_c && strcmp(hex_c, "#ffffff")) {
            if (fold && !ai) cJSON_AddNumberToObject(o, "ci", palette_add(palette, hex_c));
            else cJSON_AddStringToObject(o, "c", hex_c);
        }
        if (has_e && strcmp(hex_e, "#000000")) {
            if (fold) cJSON_AddNumberToObject(o, "ei", palette_add(palette, hex_e));
            else cJSON_AddStringToObject(o, "e", hex_e);
        }
        o = next;
    }
    free(pool_tab.v);
    cJSON_DeleteItemFromObjectCaseSensitive(m, "colors");
    cJSON_DeleteItemFromObjectCaseSensitive(m, "xyz");
    // Krunker's export order puts colors / xyz before objects' consumers; position is irrelevant to readers
    if (cJSON_GetArraySize(palette)) cJSON_AddItemToObject(m, "colors", palette);
    else cJSON_Delete(palette);
    if (pool && xyz_n) cJSON_AddItemToObject(m, "xyz", xyz);
    else cJSON_Delete(xyz);
    // spawns rounded
    cJSON *spawns = cJSON_GetObjectItemCaseSensitive(m, "spawns");
    for (cJSON *s = spawns ? spawns->child : NULL; s; s = s->next) {
        int i = 0;
        for (cJSON *c = s->child; c && i < 3; c = c->next, i++) if (cJSON_IsNumber(c)) cJSON_SetNumberValue(c, js_round(c->valuedouble, decimals));
    }
    // scripts
    const cJSON *new_scripts = js_get(args, "scripts");
    if (cJSON_IsObject(new_scripts)) {
        cJSON *sc = cJSON_GetObjectItemCaseSensitive(m, "scripts");
        if (!cJSON_IsObject(sc)) {
            cJSON_DeleteItemFromObjectCaseSensitive(m, "scripts");
            sc = cJSON_AddObjectToObject(m, "scripts");
        }
        for (const cJSON *k = new_scripts->child; k; k = k->next) {
            if (!cJSON_IsString(k) || (strcmp(k->string, "client") && strcmp(k->string, "server"))) continue;
            cJSON_DeleteItemFromObjectCaseSensitive(sc, k->string);
            cJSON_AddStringToObject(sc, k->string, k->valuestring);
        }
    }
    cJSON *ks_diags = cJSON_CreateArray();
    int ks_available = 0;
    const int ks_errors = ks_bridge_check_scripts(m, ks_diags, &ks_available);
    if (encode) {
        cJSON *sc = cJSON_GetObjectItemCaseSensitive(m, "scripts");
        for (cJSON *k = sc ? sc->child : NULL; k; k = k->next) {
            if (!cJSON_IsString(k) || !k->valuestring[0]) continue;
            size_t len;
            char *src = ks_bridge_decode(k->valuestring, &len);
            char *b64 = src ? base64_encode((const unsigned char *) src, len, NULL) : NULL;
            if (b64) cJSON_SetValuestring(k, b64);
            free(b64);
            free(src);
        }
    }
    char *text = jsonw_print(m, JSONW_COMPACT);
    const size_t after = text ? strlen(text) : 0;
    // the compiled form must load in KrunkNative's own map code
    int loads = 0, native_objects = 0;
    {
        cJSON *probe = text ? cJSON_Parse(text) : NULL;
        Doc pd = {.root = probe};
        NativeMap nm;
        if (probe && native_map_load(&pd, &nm, err, sizeof(err)) == 0) {
            loads = 1;
            native_objects = (int) nm.map->object_count;
            native_map_free(&nm);
        }
        cJSON_Delete(probe);
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "project", p->id);
    cJSON_AddNumberToObject(o, "bytes_before", (double) before);
    cJSON_AddNumberToObject(o, "bytes_after", (double) after);
    cJSON_AddNumberToObject(o, "publish_limit", schema_limit("publishMaxChars", 500000));
    cJSON_AddBoolToObject(o, "publishable", after <= schema_limit("publishMaxChars", 500000) && !errors && !ks_errors);
    cJSON_AddNumberToObject(o, "objects", cJSON_GetArraySize(objects));
    cJSON_AddNumberToObject(o, "dropped_editor_only", dropped);
    cJSON_AddNumberToObject(o, "palette_colors", cJSON_GetArraySize(js_get(m, "colors")));
    cJSON_AddNumberToObject(o, "pooled_sizes", xyz_n);
    cJSON_AddBoolToObject(o, "loads_natively", loads);
    cJSON_AddNumberToObject(o, "native_objects", native_objects);
    cJSON *bake = cJSON_AddObjectToObject(o, "bake");
    if (cpx) {
        char msg[200];
        snprintf(msg, sizeof(msg), "%d object(s) ask for complex collision (cpx): baking their triangles into \"tris\" is not supported yet; they collide as boxes", cpx);
        cJSON_AddStringToObject(bake, "tris", msg);
    } else cJSON_AddStringToObject(bake, "tris", "no complex-collision objects");
    cJSON_AddStringToObject(bake, "lightmaps", "v7.1.4 exports carry no lightmaps (lighting is computed at load)");
    cJSON *ks = cJSON_AddObjectToObject(o, "krunkscript");
    cJSON_AddBoolToObject(ks, "compiler_available", ks_available);
    cJSON_AddStringToObject(ks, "status", ks_bridge_status());
    cJSON_AddNumberToObject(ks, "errors", ks_errors);
    cJSON_AddItemToObject(ks, "diagnostics", ks_diags);
    cJSON_AddNumberToObject(o, "validation_errors", errors);
    cJSON_AddNumberToObject(o, "validation_warnings", js_int(val, "warnings", 0));
    cJSON_Delete(val);
    const char *out_path = tool_str(args, "output", NULL);
    if (out_path) {
        char path[PATH_MAX];
        if (ctx_safe_path(ctx, out_path, 1, path, sizeof(path), err, sizeof(err)) != 0) {
            free(text);
            cJSON_Delete(m);
            cJSON_Delete(o);
            return tr_error(r, "%s", err);
        }
        if (file_write(path, text, after) != 0) {
            free(text);
            cJSON_Delete(m);
            cJSON_Delete(o);
            return tr_error(r, "cannot write %s", path);
        }
        cJSON_AddStringToObject(o, "output", path);
    }
    if (js_bool(args, "inline", 0) && text && after < (1u << 20)) cJSON_AddStringToObject(o, "json", text);
    free(text);
    cJSON_Delete(m);
    tr_json(r, o);
    return 0;
}

const ToolDef g_tools_check[] = {
    {"map_validate", "Validate map",
     "Check a project with KrunkNative's own map loader and nav grid: schema (types, NaN, missing p/s, unknown prefabs, prefabs "
     "the native engine does not implement yet), property availability and ranges, colour / size pool indices, textures missing "
     "from assets, triggers (bad events / actions, v7-only ones, invalid text, dangling interface targets), teleporter channels, "
     "spawns (inside solids, over the void, overlapping, team balance, unreachable via the shared bot nav grid), limits (objects, "
     "collidable, spawns, publish size), KrunkScript, and v7.1.4's own validateMap (v7.* codes: what publishing would reject). Returns structured diagnostics with object ids / spawn indices.",
     S({"type":"object","properties":{PROJECT_ARG,"featured":{"type":"boolean","description":"v7 featured-creator limits (7500 objects)"},"reachability":{"type":"boolean","default":true},"max_diagnostics":{"type":"integer","default":300},"min_severity":{"type":"string","enum":["info","warning","error"]}}}),
     1, 0, 1, 0, t_validate},
    {"map_compile", "Compile map",
     "Produce the publishable v7.1.4 form: editor-only objects dropped, editor spawn/camera objects moved to spawns / camPos, "
     "p/s/r rounded (round, default 2), defaults stripped, colours folded into a palette (ci/ei), sizes pooled (xyz/si), "
     "KrunkScript checked and base64-encoded (scripts {client, server} replaces them). Blocks on validation errors unless force. "
     "Verifies the output loads in KrunkNative. output writes it (allowed dirs); inline:true returns the JSON.",
     S({"type":"object","properties":{PROJECT_ARG,"output":{"type":"string"},"inline":{"type":"boolean"},"round":{"type":"integer","default":2},"pool_sizes":{"type":"boolean","default":true},"fold_colors":{"type":"boolean","default":true},"strip_defaults":{"type":"boolean","default":true},"drop_editor_only":{"type":"boolean","default":true},"encode_scripts":{"type":"boolean","default":true},"scripts":{"type":"object","properties":{"client":{"type":"string"},"server":{"type":"string"}}},"force":{"type":"boolean"}}}),
     0, 0, 1, 0, t_compile},
    {"playtest", "Playtest map",
     "Run the map in KrunkNative's game simulation (shared game code, offline authority, no window): bots play for `seconds` "
     "(default 20) and an optional scripted walker follows `path` waypoints. Reports spawn reachability (nav grid), bot falls "
     "out of the map, stuck spots, kills, coverage and tick timings, plus a screenshot from the first spawn (screenshot:false "
     "skips it).",
     S({"type":"object","properties":{PROJECT_ARG,"seconds":{"type":"number","default":20},"bots":{"type":"integer","default":4},"mode":{"type":"string","description":"game mode (ffa, tdm, ...)"},"path":{"type":"array","items":{"type":"array","items":{"type":"number"}},"description":"waypoints [[x,y,z],...] for a scripted walker starting at the first"},"screenshot":{"type":"boolean","default":true},"seed":{"type":"integer"}}}),
     1, 0, 0, 0, t_playtest},
    {0},
};
