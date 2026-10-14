// editor_serialize_test (doc/v7/editor.md WP1): the editor's data model (client/src/editor/
// editor_model.c, GL-free) against Krunker v7.1.4.
//
//   editor_serialize_test <repo> [--golden FILE]... [--maps DIR]... [--verbose]
//
// 1. Goldens (client/tests/data/editor_v7_golden.json, made by tools/editor/dump_oi_golden.js
//    from v7's own libs/ObjectInstance.js): every prefab's default object, and every object of the
//    fixture maps, must serialize exactly as v7 does (values compared, key order ignored).
//    --golden adds more golden files (e.g. the whole map corpus: node tools/editor/dump_oi_golden.js
//    --defaults assets/maps/*.json ... > corpus.json).
// 2. Round trips: every map under assets/maps (and --maps DIR) is imported and exported twice; the
//    second export must equal the first, objects / spawns / camPos must survive, the map keys of
//    the source must be kept, and the complex-collision "tris" the editor bakes must match the
//    triangles the map was exported with (as a set; v7 bakes them in the same order).
// 3. Unit checks: legacy v3.1.1 keys, the multi-action list, scripts, GUI, properties.
// 4. Logic edits (doc/v7/editor.md WP4): the trigger cases of tools/editor/logic_cases.json
//    replayed through ed_model_prop_set ("<prop>@N") / ed_model_action_add / _remove must give
//    the object v7's editor serialized after the same GUI.action sequence
//    (doc/ref/krunker-v7.1.4/editor/logic/<case>.json, tools/editor/v7ref.js state "logic"), key
//    order included, and survive a load (ed_model_object) unchanged.
// 5. The KrunkScript window (WP5, editor_script_lang.c): edit -> store -> export -> import round
//    trips of the scripts, the krunkscript mode's tokens, hints, brackets, indentation,
//    closebrackets, toggleComment and the diagnostics (ks_validate).
#include "editor_model.h"
#include "editor_script_lang.h"
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail, g_checks, g_verbose;

#define CHECK(cond, ...) do { g_checks++; if (!(cond)) { g_fail++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static char *read_text(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t) n + 1);
    if (buf && fread(buf, 1, (size_t) n, f) != (size_t) n) {
        free(buf);
        buf = NULL;
    }
    if (buf) buf[n] = 0;
    fclose(f);
    return buf;
}

static cJSON *read_json(const char *path) {
    char *t = read_text(path);
    cJSON *j = t ? cJSON_Parse(t) : NULL;
    free(t);
    return j;
}

// value equality: numbers within 1e-9 (JSON null == NaN, as JSON.stringify writes NaN), key order ignored
static int json_eq(const cJSON *a, const cJSON *b) {
    if (!a || !b) return a == b;
    const int an = cJSON_IsNumber(a) && !isnan(a->valuedouble), bn = cJSON_IsNumber(b) && !isnan(b->valuedouble);
    const int a_null = cJSON_IsNull(a) || (cJSON_IsNumber(a) && !isfinite(a->valuedouble));
    const int b_null = cJSON_IsNull(b) || (cJSON_IsNumber(b) && !isfinite(b->valuedouble));
    if (a_null || b_null) return a_null && b_null;
    if (an || bn) return an && bn && fabs(a->valuedouble - b->valuedouble) <= 1e-9 * fmax(1.0, fabs(a->valuedouble));
    if (cJSON_IsBool(a) || cJSON_IsBool(b)) return cJSON_IsBool(a) && cJSON_IsBool(b) && cJSON_IsTrue(a) == cJSON_IsTrue(b);
    if (cJSON_IsString(a) || cJSON_IsString(b)) return cJSON_IsString(a) && cJSON_IsString(b) && !strcmp(a->valuestring, b->valuestring);
    if (cJSON_IsArray(a) || cJSON_IsArray(b)) {
        if (!cJSON_IsArray(a) || !cJSON_IsArray(b) || cJSON_GetArraySize(a) != cJSON_GetArraySize(b)) return 0;
        for (const cJSON *x = a->child, *y = b->child; x; x = x->next, y = y->next) if (!json_eq(x, y)) return 0;
        return 1;
    }
    if (cJSON_GetArraySize(a) != cJSON_GetArraySize(b)) return 0;
    const cJSON *x;
    cJSON_ArrayForEach(x, a) if (!json_eq(x, cJSON_GetObjectItemCaseSensitive(b, x->string))) return 0;
    return 1;
}

static void print_diff(const char *what, const cJSON *in, const cJSON *got, const cJSON *want) {
    char *i = in ? cJSON_PrintUnformatted(in) : NULL, *g = cJSON_PrintUnformatted(got), *w = cJSON_PrintUnformatted(want);
    printf("  %s\n    in:   %s\n    got:  %s\n    want: %s\n", what, i ? i : "-", g ? g : "-", w ? w : "-");
    free(i);
    free(g);
    free(w);
}

// The documented differences from v7.1.4 (editor_model.h) applied to a v7 golden before comparing.
static void apply_documented(cJSON *want, const cJSON *in, const cJSON *got) {
    // sci: KrunkNative writes the script id (v7 writes null)
    const cJSON *gs = cJSON_GetObjectItemCaseSensitive(got, "sci");
    if (cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(want, "sci"))) {
        cJSON_DeleteItemFromObjectCaseSensitive(want, "sci");
        if (gs) cJSON_AddItemToObject(want, "sci", cJSON_Duplicate(gs, 1));
    }
    // ENVIRONMENT_ZONE skyDomeEmis keeps the zone's colour
    const cJSON *ge = cJSON_GetObjectItemCaseSensitive(got, "skyDomeEmis");
    if (ge && cJSON_GetObjectItemCaseSensitive(want, "skyDomeEmis")) cJSON_ReplaceItemInObjectCaseSensitive(want, "skyDomeEmis", cJSON_Duplicate(ge, 1));
    // Krunker v10's object keys (v7.1.4 ignores them): every v10 key the native editor wrote is
    // taken from it (v10_checks() tests their values), e.g. dyn, ambFadeTime, crt, eid, pn, sfz
    static const char *const v10_keys[] = {"dyn", "boss", "cnch", "ambFadeTime", "crt", "eid", "ar", "tag", "pn", "sfz"};
    for (size_t k = 0; k < sizeof(v10_keys) / sizeof(v10_keys[0]); k++) {
        const cJSON *g = cJSON_GetObjectItemCaseSensitive(got, v10_keys[k]);
        if (g && !cJSON_GetObjectItemCaseSensitive(want, v10_keys[k])) cJSON_AddItemToObject(want, v10_keys[k], cJSON_Duplicate(g, 1));
    }
    // keys ObjectInstance does not know are kept
    const cJSON *it;
    cJSON_ArrayForEach(it, in) {
        if (!ed_model_known_key(it->string) && !cJSON_GetObjectItemCaseSensitive(want, it->string))
            cJSON_AddItemToObject(want, it->string, cJSON_Duplicate(it, 1));
    }
}

static int prefab_size_from_model(const int prefab, const cJSON *obj) {
    // v7 takes these sizes from the loaded model (never loaded by the golden harness)
    return prefab >= 0 && (editor_prefabs[prefab].has_default_size != 1 || cJSON_GetObjectItemCaseSensitive(obj, "aid"));
}

static int compare_object(const char *where, const cJSON *in, const cJSON *want_in) {
    if (cJSON_GetObjectItemCaseSensitive(want_in, "error")) return 1; // v7 itself fails on it
    // id 41 is Krunker v10's BOT_PATH_NODE (v7's TERMINAL): checked by v10_checks(), not v7's golden
    if (ed_model_prefab(in) == 41) return 1;
    cJSON *got = ed_model_object(in, 0);
    cJSON *want = cJSON_Duplicate(want_in, 1);
    apply_documented(want, in, got);
    const int prefab = ed_model_prefab(in);
    if (got && prefab_size_from_model(prefab, in) && !cJSON_GetObjectItemCaseSensitive(in, "s")) {
        cJSON_DeleteItemFromObjectCaseSensitive(got, "s");
        cJSON_DeleteItemFromObjectCaseSensitive(want, "s");
    }
    const int ok = got && json_eq(got, want);
    g_checks++;
    if (!ok) {
        g_fail++;
        printf("FAIL: %s: serialize differs from v7.1.4\n", where);
        print_diff(editor_prefabs[prefab < 0 ? 0 : prefab].name, in, got, want);
    }
    cJSON_Delete(got);
    cJSON_Delete(want);
    return ok;
}

static void check_golden(const char *path) {
    cJSON *g = read_json(path);
    CHECK(g != NULL, "golden %s unreadable", path);
    if (!g) return;
    int i = 0, n = 0, bad = 0;
    const cJSON *d;
    cJSON_ArrayForEach(d, cJSON_GetObjectItemCaseSensitive(g, "defaults")) {
        cJSON *in = cJSON_CreateObject();
        cJSON_AddNumberToObject(in, "i", i);
        cJSON_AddItemToObject(in, "p", cJSON_CreateIntArray((const int[3]) {0, 0, 0}, 3));
        char where[64];
        snprintf(where, sizeof(where), "default of prefab %d", i);
        bad += !compare_object(where, in, d);
        cJSON_Delete(in);
        i++;
        n++;
    }
    const cJSON *map;
    cJSON_ArrayForEach(map, cJSON_GetObjectItemCaseSensitive(g, "maps")) {
        int k = 0;
        const cJSON *pair;
        cJSON_ArrayForEach(pair, map) {
            char where[512];
            snprintf(where, sizeof(where), "%s object %d", map->string, k++);
            bad += !compare_object(where, cJSON_GetArrayItem(pair, 0), cJSON_GetArrayItem(pair, 1));
            n++;
            if (bad > 40) break;
        }
    }
    printf("golden %s: %d objects, %d differ\n", path, n, bad);
    cJSON_Delete(g);
}

// ---------------------------------------------------------------------------
// round trips

static EdModelObj *model_objs(const cJSON *objects, int *count) {
    const int n = cJSON_GetArraySize(objects);
    EdModelObj *list = calloc((size_t) (n ? n : 1), sizeof(EdModelObj));
    int i = 0;
    const cJSON *o;
    cJSON_ArrayForEach(o, objects) {
        list[i] = (EdModelObj) {o, ed_model_prefab(o), (uint32_t) (i + 1)};
        i++;
    }
    *count = n;
    return list;
}

static cJSON *export_of(const cJSON *doc, char **text, int *object_count) {
    EdMapState st;
    ed_model_map_init(&st);
    cJSON *objects = NULL;
    cJSON *out = NULL;
    if (ed_model_import(doc, &st, &objects, NULL)) {
        int n;
        EdModelObj *list = model_objs(objects, &n);
        out = ed_model_export(&st, list, (size_t) n, 0);
        free(list);
        if (object_count) *object_count = n;
    }
    cJSON_Delete(objects);
    ed_model_map_free(&st);
    if (text) *text = out ? ed_model_print(out) : NULL;
    return out;
}

static int tri_cmp(const void *a, const void *b) { return memcmp(a, b, 9 * sizeof(long)); }

static long *tri_list(const cJSON *tris, int *n) {
    *n = cJSON_GetArraySize(tris) / 9;
    long *t = calloc((size_t) (*n ? *n : 1) * 9, sizeof(long));
    int i = 0;
    const cJSON *v;
    cJSON_ArrayForEach(v, tris) if (i < *n * 9) t[i++] = lround(cJSON_GetNumberValue(v));
    qsort(t, (size_t) *n, 9 * sizeof(long), tri_cmp);
    return t;
}

static int g_maps, g_tris_maps;

static void round_trip(const char *path) {
    cJSON *doc = read_json(path);
    if (!doc) return; // not a map file
    const cJSON *src = cJSON_GetObjectItemCaseSensitive(doc, "map");
    if (!cJSON_IsObject(src)) src = doc;
    if (!cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(src, "objects"))) {
        cJSON_Delete(doc);
        return;
    }
    g_maps++;
    // export(import(m)) is a fixed point after one pass: the first pass may migrate legacy keys
    // (v7 writes a legacy trigger's object-level sound next to tas once, then drops it)
    char *t1 = NULL, *t2 = NULL, *t3 = NULL;
    cJSON *e1 = export_of(doc, &t1, NULL);
    CHECK(e1 != NULL, "%s: import failed", path);
    cJSON *e2 = e1 ? export_of(e1, &t2, NULL) : NULL;
    cJSON *e3 = e2 ? export_of(e2, &t3, NULL) : NULL;
    CHECK(t2 && t3 && !strcmp(t2, t3), "%s: export(import(export)) is not stable", path);
    cJSON_Delete(e3);
    free(t3);
    if (e1) {
        // objects, spawns and the camera survive (no source map holds editor-only prefabs)
        int src_objects = 0;
        const cJSON *o;
        cJSON_ArrayForEach(o, cJSON_GetObjectItemCaseSensitive(src, "objects")) {
            const int prefab = ed_model_prefab(o);
            src_objects += prefab >= 0 && !editor_prefab_has(prefab, EP_NOEXPORT) && strcmp(editor_prefabs[prefab].name, "SPAWN_POINT") &&
                           strcmp(editor_prefabs[prefab].name, "CAMERA_POSITION");
        }
        CHECK(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(e1, "objects")) == src_objects, "%s: %d objects exported, %d in the map", path,
              cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(e1, "objects")), src_objects);
        int spawns = 0;
        const cJSON *sp;
        cJSON_ArrayForEach(sp, cJSON_GetObjectItemCaseSensitive(src, "spawns")) spawns += cJSON_IsArray(sp);
        cJSON_ArrayForEach(o, cJSON_GetObjectItemCaseSensitive(src, "objects")) spawns += ed_model_prefab(o) == 5;
        CHECK(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(e1, "spawns")) == spawns, "%s: spawns %d -> %d", path, spawns,
              cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(e1, "spawns")));
        const cJSON *cam = cJSON_GetObjectItemCaseSensitive(src, "camPos");
        if (cJSON_IsArray(cam) && cJSON_GetArraySize(cam) >= 3) {
            const cJSON *ec = cJSON_GetObjectItemCaseSensitive(e1, "camPos");
            CHECK(cJSON_GetArraySize(ec) == 7, "%s: camPos is not the 7-tuple", path);
            for (int k = 0; k < 3; k++) CHECK(json_eq(cJSON_GetArrayItem(ec, k), cJSON_GetArrayItem(cam, k)), "%s: camPos moved", path);
        }
        // every top-level key is kept, but the ones importMap consumes and mapConfig defaults
        const cJSON *it;
        cJSON_ArrayForEach(it, src) {
            static const char *const consumed[] = {"colors", "xyz", "tris", "botPaths", "game", "objects", "config", "gui", "scripts", NULL};
            int skip = 0;
            for (int k = 0; consumed[k]; k++) skip |= !strcmp(consumed[k], it->string);
            if (skip || cJSON_GetObjectItemCaseSensitive(e1, it->string)) continue;
            // getMapExport gates by prefix: oc* (ocean), zone*, terrain* (a seed), skyDome*
            const cJSON *mc_src = src;
            const int gated = (!strncmp(it->string, "oc", 2) && !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(mc_src, "ocean"))) ||
                              (!strncmp(it->string, "zone", 4) && !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(mc_src, "zone"))) ||
                              (!strncmp(it->string, "skyDome", 7) && !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(mc_src, "skyDome"))) ||
                              !strncmp(it->string, "terrain", 7);
            CHECK(gated || ed_model_map_key_known(it->string), "%s: top-level key \"%s\" lost", path, it->string);
        }
        // the complex-collision bake matches the triangles the map shipped with
        const cJSON *tris = cJSON_GetObjectItemCaseSensitive(src, "tris");
        if (cJSON_GetArraySize(tris)) {
            g_tris_maps++;
            int na, nb;
            long *a = tri_list(tris, &na), *b = tri_list(cJSON_GetObjectItemCaseSensitive(e1, "tris"), &nb);
            int same = na == nb;
            for (int i = 0; same && i < na * 9; i++) same = a[i] == b[i];
            CHECK(same, "%s: baked tris (%d) differ from the map's (%d)", path, nb, na);
            free(a);
            free(b);
        }
    }
    if (g_verbose) printf("round trip %s: %s\n", path, t1 && t2 && !strcmp(t1, t2) ? "stable" : "migrated");
    free(t1);
    free(t2);
    cJSON_Delete(e1);
    cJSON_Delete(e2);
    cJSON_Delete(doc);
}

static void round_trip_dir(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return;
    const struct dirent *e;
    static char names[512][200];
    int n = 0;
    while ((e = readdir(d)) && n < 512) {
        const size_t len = strlen(e->d_name);
        if (len > 5 && !strcmp(e->d_name + len - 5, ".json")) snprintf(names[n++], sizeof(names[0]), "%s", e->d_name);
    }
    closedir(d);
    for (int i = 0; i < n; i++) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
        round_trip(path);
    }
}

// ---------------------------------------------------------------------------
// unit checks

static cJSON *obj_of(const char *json) {
    cJSON *in = cJSON_Parse(json);
    cJSON *out = ed_model_object(in, 0);
    cJSON_Delete(in);
    return out;
}

static int has_key(const cJSON *o, const char *k) { return cJSON_GetObjectItemCaseSensitive(o, k) != NULL; }
static double num(const cJSON *o, const char *k) { return cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(o, k)); }

static void unit_checks(void) {
    // legacy v3.1.1 object keys
    cJSON *o = obj_of("{\"i\":0,\"p\":[0,0,0],\"s\":[1,2,3],\"col\":1,\"hp\":30,\"tr\":1.5708}");
    CHECK(num(o, "l") == 1 && !has_key(o, "col"), "legacy col -> l");
    CHECK(num(o, "h") == 30 && !has_key(o, "hp"), "legacy hp -> h");
    CHECK(num(o, "tro") == 90 && !has_key(o, "tr"), "legacy tr (radians) -> tro (degrees)");
    cJSON_Delete(o);
    o = obj_of("{\"i\":26,\"p\":[0,0,0],\"wid\":4}");
    CHECK(num(o, "w") == 4 && !has_key(o, "wid"), "legacy wid -> w");
    cJSON_Delete(o);
    // legacy single-action trigger -> tas[0]
    o = obj_of("{\"i\":29,\"p\":[0,0,0],\"te\":3,\"ta\":5,\"tc\":7,\"tct\":\"hi\"}");
    const cJSON *a0 = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(o, "tas"), 0);
    CHECK(num(o, "te") == 3 && num(a0, "ta") == 5 && num(a0, "tc") == 7 && !strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(a0, "tct")), "hi") &&
          !has_key(o, "ta") && !has_key(o, "tc"), "legacy single action -> tas[0]");
    // the multi-action list: at most 8 (v10 triggerActionLimit; v7.1.4: 5), action 0 stays
    for (int i = 0; i < 9; i++) ed_model_action_add(&o);
    CHECK(ed_model_action_count(o) == 8, "trigger actions are limited to 8 (%d)", ed_model_action_count(o));
    CHECK(!ed_model_action_remove(&o, 0) && ed_model_action_remove(&o, 7) && ed_model_action_count(o) == 7, "action 0 cannot be removed");
    while (ed_model_action_count(o) > 4) ed_model_action_remove(&o, ed_model_action_count(o) - 1);
    cJSON *v = cJSON_CreateNumber(14);
    CHECK(ed_model_action_set(&o, 29, 1, "ta", v), "action set");
    cJSON_Delete(v);
    a0 = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(o, "tas"), 1);
    CHECK(num(a0, "ta") == 14 && num(a0, "es") == 0 && num(a0, "ev") == 1 && num(a0, "er") == 1, "a sound action writes es / ev / er");
    // properties by name: action props live in tas[0]
    v = cJSON_CreateNumber(9);
    ed_model_prop_set(&o, 29, "constant", v);
    cJSON_Delete(v);
    cJSON *g = ed_model_prop_get(o, 29, "constant");
    CHECK(cJSON_GetNumberValue(g) == 9 && num(cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(o, "tas"), 0), "tc") == 9, "prop constant -> tas[0].tc");
    cJSON_Delete(g);
    cJSON_Delete(o);
    o = obj_of("{\"i\":0,\"p\":[0,0,0]}");
    v = cJSON_CreateFalse();
    ed_model_prop_set(&o, 0, "canWallJump", v);
    CHECK(num(o, "wj") == 1, "canWallJump false writes wj");
    ed_model_prop_set(&o, 0, "visible", v);
    CHECK(num(o, "v") == 1, "visible false writes v");
    cJSON_Delete(v);
    v = cJSON_CreateString("x");
    CHECK(ed_model_prop_set(&o, 0, "aiName", v) && !has_key(o, "ain"), "aiName is gated to AI prefabs");
    cJSON_Delete(v);
    cJSON_Delete(o);
    // keys v7 does not know are kept
    o = obj_of("{\"i\":0,\"p\":[0,0,0],\"dyn\":1,\"gy\":2}");
    CHECK(num(o, "dyn") == 1 && num(o, "gy") == 2, "unknown object keys are kept");
    cJSON_Delete(o);

    // the map: spawns 8-tuple (a spread without teamOnly leaves null), camPos 7-tuple, pools
    cJSON *doc = cJSON_Parse("{\"name\":\"T\",\"objects\":[{\"p\":[0,0,0],\"s\":[5,5,5]},{\"p\":[1,0,0],\"s\":[5,5,5],\"c\":\"#ff0000\"}],"
                             "\"spawns\":[[1,2,3,1,2,1,0,5],[4,5,6]],\"camPos\":[7,8,9],"
                             "\"game\":{\"jpC\":3},\"config\":{\"modes\":[0,2],\"settings\":{\"gravMlt\":0.5}},"
                             "\"scripts\":{\"client\":\"" "IyDDqQ==" "\"},\"gui\":[{\"t\":1,\"v\":\"hi\",\"x\":150}],\"customKey\":7}");
    char *text = NULL;
    cJSON *e = export_of(doc, &text, NULL);
    const cJSON *spawns = cJSON_GetObjectItemCaseSensitive(e, "spawns");
    CHECK(cJSON_GetArraySize(cJSON_GetArrayItem(spawns, 0)) == 8 && cJSON_IsNull(cJSON_GetArrayItem(cJSON_GetArrayItem(spawns, 0), 6)),
          "spawn [.., spread] keeps null for teamOnly");
    CHECK(cJSON_GetArraySize(cJSON_GetArrayItem(spawns, 1)) == 6, "spawn without extras is the 6-tuple");
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(e, "camPos")) == 7, "camPos is the 7-tuple");
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(e, "xyz")) == 3, "the size pool dedups");
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(e, "colors")) == 1, "the colour pool");
    CHECK(num(e, "customKey") == 7, "unknown top-level keys are kept");
    CHECK(!has_key(e, "game"), "the legacy game block is not exported");
    const cJSON *cls = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(e, "config"), "cls");
    CHECK(num(cJSON_GetObjectItemCaseSensitive(cls, "0"), "jpC") == 3 && num(cJSON_GetObjectItemCaseSensitive(cls, "5"), "jpC") == 3,
          "game -> every class");
    const cJSON *cfg = cJSON_GetObjectItemCaseSensitive(e, "config");
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(cfg, "modes")) == 2 && num(cJSON_GetObjectItemCaseSensitive(cfg, "settings"), "gravMlt") == 0.5,
          "config modes / settings");
    const cJSON *gui = cJSON_GetObjectItemCaseSensitive(e, "gui");
    CHECK(num(cJSON_GetArrayItem(gui, 0), "x") == 100 && num(cJSON_GetArrayItem(gui, 0), "t") == 1, "gui clamps");
    const cJSON *scripts = cJSON_GetObjectItemCaseSensitive(e, "scripts");
    CHECK(scripts && !strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(scripts, "client")), "IyDDqQ=="), "scripts round-trip as UTF-8 base64");
    char *decoded = ed_model_b64_decode("IyDDqQ==");
    CHECK(decoded && !strcmp(decoded, "# \xc3\xa9"), "base64 decodes UTF-8");
    free(decoded);
    free(text);
    cJSON_Delete(e);
    cJSON_Delete(doc);
    // import checks: name and modURL
    doc = cJSON_Parse("{\"name\":\"bad<name>\",\"modURL\":\"not a url\",\"welMsg\":\"ok\",\"objects\":[]}");
    EdMapState st;
    ed_model_map_init(&st);
    ed_model_import(doc, &st, NULL, NULL);
    CHECK(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(st.map_config, "name")), "New Krunker Map"), "an invalid name resets");
    CHECK(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(st.map_config, "modURL")), ""), "an invalid modURL resets");
    cJSON_ReplaceItemInObjectCaseSensitive(st.map_config, "name", cJSON_CreateString("a&b"));
    e = ed_model_export(&st, NULL, 0, 0);
    CHECK(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(e, "name")), "a&amp;b"), "name goes through sanitizeHTML");
    cJSON_Delete(e);
    ed_model_map_free(&st);
    cJSON_Delete(doc);
    // numbers print as JSON.stringify does
    static const struct { double v; const char *s; } nums[] = {
        {321.92092327706604, "321.92092327706604"}, {0.1, "0.1"}, {1e21, "1e+21"}, {1e-7, "1e-7"}, {0.000001, "0.000001"},
        {-0.0, "0"}, {123456789012345680000.0, "123456789012345680000"}, {2.5e-5, "0.000025"}, {-1.5, "-1.5"},
    };
    for (size_t i = 0; i < sizeof(nums) / sizeof(nums[0]); i++) {
        char buf[64];
        ed_model_number(nums[i].v, buf, sizeof(buf));
        CHECK(!strcmp(buf, nums[i].s), "number %s printed as %s", nums[i].s, buf);
    }
}

// ---------------------------------------------------------------------------
// 4. logic edits

// a step's value as the native control stores it: a select gives a number, except a condition
// value's select (the option's string, as v7's GUI.action); text a string; switch a boolean
static cJSON *step_value(const cJSON *step) {
    const char *kind = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(step, "kind"));
    const char *prop = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(step, "set"));
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(step, "value");
    char buf[64];
    if (kind && !strcmp(kind, "select") && prop && !strncmp(prop, "conditionVal", 12)) {
        if (cJSON_IsString(v)) return cJSON_CreateString(v->valuestring);
        ed_model_number(cJSON_GetNumberValue(v), buf, sizeof(buf));
        return cJSON_CreateString(buf);
    }
    if (kind && !strcmp(kind, "switch")) return cJSON_CreateBool(cJSON_IsTrue(v) || (cJSON_IsNumber(v) && v->valuedouble != 0));
    return cJSON_Duplicate(v, 1);
}

static int has_cust(const int action) {
    const cJSON *a = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(editor_data_get("triggers"), "actions"), action);
    return a && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(a, "hasCust"));
}

static void logic_cases(const char *repo) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/tools/editor/logic_cases.json", repo);
    cJSON *doc = read_json(path);
    CHECK(doc != NULL, "%s unreadable", path);
    int n = 0;
    const cJSON *c;
    cJSON_ArrayForEach(c, cJSON_GetObjectItemCaseSensitive(doc, "cases")) {
        const cJSON *pf = cJSON_GetObjectItemCaseSensitive(c, "prefab");
        if (!cJSON_IsNumber(pf)) continue; // GUI / config cases: tools/editor/logic_parity.py
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(c, "name"));
        const int prefab = (int) pf->valuedouble;
        snprintf(path, sizeof(path), "%s/doc/ref/krunker-v7.1.4/editor/logic/%s.json", repo, name);
        cJSON *want = read_json(path);
        CHECK(want != NULL, "%s unreadable", path);
        if (!want) continue;
        char init[64];
        snprintf(init, sizeof(init), "{\"i\":%d,\"p\":[0,0,0]}", prefab);
        cJSON *in = cJSON_Parse(init);
        cJSON *o = ed_model_object(in, ED_MODEL_EXACT_TRANSFORM);
        cJSON_Delete(in);
        const cJSON *step;
        cJSON_ArrayForEach(step, cJSON_GetObjectItemCaseSensitive(c, "steps")) {
            const cJSON *set = cJSON_GetObjectItemCaseSensitive(step, "set");
            if (cJSON_IsString(set)) {
                cJSON *v = step_value(step);
                CHECK(ed_model_prop_set(&o, prefab, set->valuestring, v), "%s: set %s", name, set->valuestring);
                cJSON_Delete(v);
                if (!strncmp(set->valuestring, "action@", 7)) {
                    // the panel's rule (editor_triggers.c normalize_value_type): constantType
                    // goes back to 0 for an action without hasCust
                    char ct[32];
                    snprintf(ct, sizeof(ct), "constantType@%s", set->valuestring + 7);
                    cJSON *a = ed_model_prop_get(o, prefab, set->valuestring);
                    cJSON *t = ed_model_prop_get(o, prefab, ct);
                    if (!has_cust((int) cJSON_GetNumberValue(a)) && cJSON_GetNumberValue(t) != 0) {
                        cJSON *zero = cJSON_CreateNumber(0);
                        ed_model_prop_set(&o, prefab, ct, zero);
                        cJSON_Delete(zero);
                    }
                    cJSON_Delete(a);
                    cJSON_Delete(t);
                }
            } else if (cJSON_GetObjectItemCaseSensitive(step, "add")) {
                ed_model_action_add(&o); // (the 6th is refused: v7 hides the button)
            } else if (cJSON_GetObjectItemCaseSensitive(step, "remove")) {
                ed_model_action_remove(&o, (int) cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(step, "remove")));
            }
        }
        // a new model prefab takes the generator's OBJ bounds (v7: the loaded model's): "s" of AI
        // objects is not compared (doc/v7/editor.md 1.13)
        if (prefab == 55) {
            const cJSON *s = cJSON_GetObjectItemCaseSensitive(o, "s");
            if (s) cJSON_ReplaceItemInObjectCaseSensitive(want, "s", cJSON_Duplicate(s, 1));
        }
        char *got_text = ed_model_print(o), *want_text = ed_model_print(want);
        CHECK(got_text && want_text && !strcmp(got_text, want_text), "logic %s: serialize differs\n    got:  %s\n    want: %s", name, got_text ? got_text : "-",
              want_text ? want_text : "-");
        cJSON *again = ed_model_object(o, ED_MODEL_EXACT_TRANSFORM);
        char *again_text = again ? ed_model_print(again) : NULL;
        CHECK(again_text && got_text && !strcmp(again_text, got_text), "logic %s: a reload changes the object", name);
        free(again_text);
        cJSON_Delete(again);
        free(got_text);
        free(want_text);
        cJSON_Delete(o);
        cJSON_Delete(want);
        n++;
    }
    CHECK(n >= 4, "logic cases: %d trigger cases", n);
    printf("logic cases: %d trigger cases\n", n);
    cJSON_Delete(doc);
}

// v7's game / script templates (assets/editor/templates.json): an import and export keeps every
// trigger's action list ("tas") as v7 serializes it (e.g. "Triggers & Scripts", "Advanced
// Triggers"; a source "cd": 0 with cv values loses them, as in v7's serialize())
static void template_tas(const char *repo) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/assets/editor/templates.json", repo);
    cJSON *t = read_json(path);
    if (!t) {
        printf("templates: %s missing, skipped\n", path);
        return;
    }
    int maps = 0, lists = 0;
    static const char *const kinds[2] = {"game", "script"};
    for (int k = 0; k < 2; k++) {
        const cJSON *tpl;
        cJSON_ArrayForEach(tpl, cJSON_GetObjectItemCaseSensitive(t, kinds[k])) {
            const char *data = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(tpl, "data"));
            const char *name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(tpl, "name"));
            cJSON *src = data ? cJSON_Parse(data) : NULL;
            if (!src) {
                cJSON_Delete(src);
                continue;
            }
            int count = 0;
            char *text = NULL;
            cJSON *e = export_of(src, &text, &count);
            free(text);
            const cJSON *so = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(src, "map") ? cJSON_GetObjectItemCaseSensitive(src, "map") : src, "objects");
            const cJSON *eo = cJSON_GetObjectItemCaseSensitive(e, "objects");
            // the source triggers' lists in order against the exported ones
            const cJSON *a = so ? so->child : NULL, *b = eo ? eo->child : NULL;
            for (; a; a = a->next) {
                const cJSON *tas = cJSON_GetObjectItemCaseSensitive(a, "tas");
                if (!cJSON_IsArray(tas)) continue;
                while (b && !cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(b, "tas"))) b = b->next;
                cJSON *canon = ed_model_object(a, 0);
                const cJSON *want = canon ? cJSON_GetObjectItemCaseSensitive(canon, "tas") : tas;
                CHECK(b && json_eq(want, cJSON_GetObjectItemCaseSensitive(b, "tas")), "template %s: a trigger's tas changed on import / export", name ? name : "?");
                cJSON_Delete(canon);
                if (b) b = b->next;
                lists++;
            }
            cJSON_Delete(e);
            cJSON_Delete(src);
            maps++;
        }
    }
    printf("templates: %d maps, %d action lists kept\n", maps, lists);
    cJSON_Delete(t);
}

static int tok_style(const char *src, const char *what, int nth) {
    // style of the nth occurrence (0-based) of `what` in src
    EdKsToken t[256];
    const int n = ed_ks_tokenize(src, strlen(src), t, 256);
    const char *p = src;
    for (int i = 0; i <= nth && p; i++) p = strstr(i ? p + 1 : p, what);
    return p ? ed_ks_style_at(t, n < 256 ? n : 256, (int) (p - src)) : -1;
}

static void script_checks(void) {
    // --- the krunkscript simple mode (scripting.html)
    const char *src = "# hi\npublic action start(num a) {\n\tstr s = \"x(\" + 'y';  # c\n\tGAME.log(toNum s, true, 12.5, x2, 2x);\n\tif(a) { obj o = {}; }\n}";
    CHECK(tok_style(src, "# hi", 0) == EKS_COMMENT, "comment");
    CHECK(tok_style(src, "public", 0) == EKS_KEYWORD && tok_style(src, "action", 0) == EKS_KEYWORD, "keywords");
    CHECK(tok_style(src, "start", 0) == EKS_DEF && tok_style(src, "(num", 0) == EKS_NONE, "def + (");
    CHECK(tok_style(src, "num", 0) == EKS_TYPE && tok_style(src, "str", 0) == EKS_TYPE, "types");
    CHECK(tok_style(src, "\"x(\"", 0) == EKS_STRING && tok_style(src, "'y'", 0) == EKS_STRING, "strings");
    CHECK(tok_style(src, "  # c", 0) == EKS_COMMENT, "a trailing comment takes its leading spaces");
    CHECK(tok_style(src, "GAME", 0) == EKS_ATOM && tok_style(src, "log", 0) == EKS_DEF && tok_style(src, "true", 0) == EKS_ATOM, "atoms, call");
    CHECK(tok_style(src, "toNum", 0) == EKS_KEYWORD && tok_style(src, "12.5", 0) == EKS_NUMBER, "keyword op, number");
    CHECK(tok_style(src, "x2", 0) == EKS_VARIABLE && tok_style(src, "2x", 0) == EKS_NUMBER && tok_style(src, "x);", 0) == EKS_VARIABLE, "x2 / 2x");
    CHECK(tok_style(src, "if", 0) == EKS_KEYWORD && tok_style(src, "obj", 0) == EKS_TYPE, "if( is a keyword");
    CHECK(tok_style("num(x)", "num", 0) == EKS_DEF && tok_style("(num) x", "num", 0) == EKS_TYPE, "num( is a def, (num) a type");
    CHECK(tok_style("x = \"open", "\"", 0) == EKS_NONE, "an unterminated string is not a token");
    CHECK(tok_style("inside in", "inside", 0) == EKS_VARIABLE && tok_style("inside in", " in", 0) == EKS_NONE && tok_style("inside in", "in", 1) == EKS_KEYWORD, "word boundaries");

    // --- matchBrackets
    {
        const char *b = "a(\"(\", [1]) )";
        EdKsToken t[64];
        const int n = ed_ks_tokenize(b, strlen(b), t, 64);
        int x = -1, y = -1;
        CHECK(ed_ks_match_bracket(b, strlen(b), t, n, 2, &x, &y) == 1 && x == 1 && y == 10, "( matches ) past a string");
        CHECK(ed_ks_match_bracket(b, strlen(b), t, n, 11, &x, &y) == 1 && x == 10 && y == 1, "the bracket before the caret first");
        CHECK(ed_ks_match_bracket(b, strlen(b), t, n, 13, &x, &y) == -1 && x == 12, "an unmatched )");
        CHECK(ed_ks_match_bracket(b, strlen(b), t, n, 0, &x, &y) == 0, "no bracket at the caret");
    }

    // --- hints (KSS autoComplete + alias)
    {
        cJSON *entries = cJSON_Parse("[{\"text\":\"GAME.UTILS.math.floor(n)\",\"type\":\"num action()\"},{\"text\":\"GAME.UTILS.random()\",\"type\":\"num action()\"},"
                                     "{\"text\":\"GAME.PLAYERS.findByID(id)\",\"type\":\"obj action()\"},{\"text\":\"GAME.log(...args)\",\"type\":\"action()\"}]");
        cJSON *alias = cJSON_Parse("{\"UTILS\":\"GAME.UTILS\",\"Math\":\"GAME.UTILS.math\"}");
        EdKsHint h[8];
        int from = -1;
        const char *l1 = "\tnum v = game.pl";
        int n = ed_ks_hints(l1, strlen(l1), (int) strlen(l1), entries, alias, h, 8, &from);
        CHECK(n == 1 && !strcmp(h[0].text, "GAME.PLAYERS.findByID(id)") && from == 9, "case-insensitive prefix, word start");
        const char *l2 = "Math.fl";
        n = ed_ks_hints(l2, strlen(l2), 7, entries, alias, h, 8, &from);
        CHECK(n == 1 && !strcmp(h[0].text, "Math.floor(n)") && !strcmp(h[0].type, "num action()"), "an alias completes with its own name");
        const char *l3 = "GAME.";
        n = ed_ks_hints(l3, strlen(l3), 5, entries, alias, h, 8, &from);
        CHECK(n == 4 && !strcmp(h[0].text, "GAME.UTILS.math.floor(n)"), "entries matched directly keep their text");
        const char *l4 = "x = \"GAME\"; GAME";
        CHECK(ed_ks_hints(l4, strlen(l4), (int) strlen(l4), entries, alias, h, 8, &from) == 0, "no hints for a word quoted in the line");
        CHECK(ed_ks_hints("a b", 3, 2, entries, alias, h, 8, &from) == 0, "no word, no hints");
        cJSON_Delete(entries);
        cJSON_Delete(alias);
    }

    // --- indentation (indentUnit 4, tabs), closebrackets, toggleComment
    {
        const char *t1 = "public action start() {\n";
        CHECK(ed_ks_indent_at(t1, strlen(t1), (int) strlen(t1)) == 4, "a { indents the next line");
        const char *t2 = "public action start() {\n\tGAME.log(\"x\");\n";
        CHECK(ed_ks_indent_at(t2, strlen(t2), (int) strlen(t2)) == 4, "a call's ) does not dedent (v7 fix)");
        const char *t3 = "if (a) {\n\tb();\n}";
        CHECK(ed_ks_indent_at(t3, strlen(t3), (int) strlen(t3) - 1) == 0, "a leading } dedents");
        const char *t4 = "x = \"{\"; # {\n";
        CHECK(ed_ks_indent_at(t4, strlen(t4), (int) strlen(t4)) == 0, "brackets in strings and comments do not indent");
        char ind[16];
        CHECK(ed_ks_indent_string(10, ind, sizeof(ind)) == 4 && !strcmp(ind, "\t\t  "), "indentWithTabs");
        CHECK(ed_ks_close_action("f()", 3, 2, ')') == 2, "typing ) before ) skips over it");
        CHECK(ed_ks_close_action("f", 1, 1, '(') == 1 && ed_ks_close_action("fx", 2, 1, '(') == 0, "( pairs only before space / closers");
        CHECK(ed_ks_close_action("a ", 2, 1, '"') == 0 && ed_ks_close_action(" ", 1, 1, '"') == 1, "quotes pair outside words");
        int ra, rb;
        const char *c1 = "a\n\tb\n";
        char *r = ed_ks_toggle_comment(c1, strlen(c1), 0, 4, &ra, &rb);
        CHECK(r && !strcmp(r, "# a\n# \tb") && ra == 0 && rb == 4, "comment at the smallest indentation");
        char *r2 = r ? ed_ks_toggle_comment(r, strlen(r), 0, (int) strlen(r), &ra, &rb) : NULL;
        CHECK(r2 && !strcmp(r2, "a\n\tb"), "uncomment");
        free(r);
        free(r2);
    }

    // --- the store: edit -> save -> export -> import (base64 UTF-8, the template is omitted)
    {
        EdMapState st;
        ed_model_map_init(&st);
        CHECK(!strcmp(ed_ks_script_text(st.scripts, 0), ed_model_script_template("client")), "a new map shows the client template");
        const char *code = "# h\xc3\xa9llo \xe2\x9c\x93\npublic action start() {\n\tGAME.log(\"\xc3\xbc\");\n}\n";
        ed_ks_script_store(st.scripts, 0, code);
        ed_ks_script_store(st.scripts, 1, ed_model_script_template("server"));
        cJSON *e = ed_model_export(&st, NULL, 0, 0);
        const cJSON *sc = cJSON_GetObjectItemCaseSensitive(e, "scripts");
        char *b64 = ed_model_b64_encode(code);
        CHECK(sc && b64 && !strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(sc, "client")), b64), "the client script exports as UTF-8 base64");
        CHECK(sc && !cJSON_GetObjectItemCaseSensitive(sc, "server"), "a script equal to its template is not exported");
        EdMapState back;
        ed_model_map_init(&back);
        ed_model_import(e, &back, NULL, NULL);
        CHECK(!strcmp(ed_ks_script_text(back.scripts, 0), code), "non-ASCII code survives export + import");
        CHECK(!strcmp(ed_ks_script_text(back.scripts, 1), ed_model_script_template("server")), "the server tab shows its template again");
        cJSON *e2 = ed_model_export(&back, NULL, 0, 0);
        char *t1 = ed_model_print(e), *t2 = ed_model_print(e2);
        CHECK(t1 && t2 && !strcmp(t1, t2), "the second export equals the first");
        free(t1);
        free(t2);
        free(b64);
        cJSON_Delete(e);
        cJSON_Delete(e2);
        ed_model_map_free(&back);
        ed_model_map_free(&st);
    }

    // --- diagnostics
    {
        EdKsDiag d[4];
        const char *bad = "public action start() {\n\tnum x = 1;\n\tx = \"s\";\n}\n";
        const int n = ed_ks_check(bad, 0, d, 4);
        CHECK(n >= 1 && d[0].error && d[0].line == 3, "a type error is reported on its line (%d:%d %s)", n ? d[0].line : 0, n ? d[0].col : 0, n ? d[0].message : "");
        CHECK(ed_ks_check(ed_model_script_template("client"), 0, d, 4) == 0, "the client template checks clean");
        CHECK(ed_ks_check(ed_model_script_template("server"), 1, d, 4) == 0, "the server template checks clean");
    }
}

// Krunker v10 map data the editor keeps (doc/v10/editor-social.md): BOT_PATH_NODE and botPaths,
// the ENTITY / crate / AI / zone keys, scripts.shared. Expected values derived from the webcracked
// v10 editor (E10 = ~/.cache/kn-v10/webcrack/editor/renamed.js; v10's JS is never run).
static void v10_checks(void) {
    cJSON *o = obj_of("{\"i\":41,\"p\":[1,2,3]}");
    CHECK(o && num(o, "bng") == 0 && num(o, "bn") == 0 && num(o, "bnn") == 1 && num(o, "bnr") == 0 && num(o, "l") == 1,
          "BOT_PATH_NODE defaults bng 0, bn 0, bnn 1, bnr 0, not collidable (E10:7409, 8038)");
    cJSON_Delete(o);
    o = obj_of("{\"i\":98,\"p\":[0,0,0],\"eid\":3,\"ar\":7,\"tag\":\"boss-1\",\"as\":0,\"sp\":{\"pt0\":[1,2,3]}}");
    CHECK(o && num(o, "eid") == 3 && num(o, "ar") == 7 && has_key(o, "tag") && num(o, "as") == 0 && cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(o, "sp")),
          "ENTITY eid / ar / tag / as / sp kept (E10:7451-7457, 8770-8781)");
    cJSON_Delete(o);
    o = obj_of("{\"i\":98,\"p\":[0,0,0],\"tag\":\"bad tag!\"}");
    CHECK(o && !has_key(o, "tag") && !has_key(o, "as"), "ENTITY: an invalid tag is dropped, autoSpawn on writes no as");
    cJSON_Delete(o);
    o = obj_of("{\"i\":97,\"p\":[0,0,0],\"crt\":3}");
    CHECK(o && num(o, "crt") == 3, "SCRAPCRATE crt kept (E10:8766)");
    cJSON_Delete(o);
    o = obj_of("{\"i\":0,\"p\":[0,0,0],\"dyn\":true,\"pn\":\"door\",\"sfz\":1}");
    CHECK(o && num(o, "dyn") == 1 && has_key(o, "pn") && num(o, "sfz") == 1, "CUBE dyn / pn / sfz (E10:7814, 8783-8787)");
    cJSON_Delete(o);
    // botPaths: import -> BOT_PATH_NODEs -> export (E10:17046-17064, 16405-16420)
    cJSON *doc = cJSON_Parse("{\"name\":\"bp\",\"objects\":[],\"spawns\":[],\"botPaths\":[{\"p\":[[0,0,0],[10,0,0]],\"r\":0},{\"p\":[[5,5,5]],\"r\":1}],"
                             "\"scripts\":{\"shared\":\"c2hhcmVk\"}}");
    int n = 0;
    cJSON *out = export_of(doc, NULL, &n);
    const cJSON *bp = cJSON_GetObjectItemCaseSensitive(out, "botPaths");
    const cJSON *g0 = cJSON_GetArrayItem(bp, 0), *g1 = cJSON_GetArrayItem(bp, 1);
    CHECK(n == 3 && cJSON_GetArraySize(bp) == 2 && cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(g0, "p")) == 2 &&
          num(g0, "r") == 0 && num(g1, "r") == 1 && cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(out, "objects")) == 0,
          "botPaths round trip (3 nodes, 2 groups, r kept, no node in objects)");
    const cJSON *sc = cJSON_GetObjectItemCaseSensitive(out, "scripts");
    const cJSON *sh = cJSON_GetObjectItemCaseSensitive(sc, "shared");
    CHECK(cJSON_IsString(sh) && !strcmp(sh->valuestring, "c2hhcmVk"), "scripts.shared round trip");
    cJSON_Delete(out);
    cJSON_Delete(doc);
}

int main(int argc, char **argv) {
    const char *repo = argc > 1 ? argv[1] : ".";
    char path[1024];
    for (int i = 2; i < argc; i++) if (!strcmp(argv[i], "--verbose")) g_verbose = 1;
    snprintf(path, sizeof(path), "%s/client/tests/data/editor_v7_golden.json", repo);
    check_golden(path);
    for (int i = 2; i + 1 < argc; i++) if (!strcmp(argv[i], "--golden")) check_golden(argv[++i]);
    unit_checks();
    v10_checks();
    logic_cases(repo);
    template_tas(repo);
    script_checks();
    static const char *const dirs[2] = {"assets/maps", "assets/maps/custom"}; // v7's maps + 311_ / 152_, community
    for (int i = 0; i < 2; i++) {
        snprintf(path, sizeof(path), "%s/%s", repo, dirs[i]);
        round_trip_dir(path);
    }
    for (int i = 2; i + 1 < argc; i++) if (!strcmp(argv[i], "--maps")) round_trip_dir(argv[++i]);
    printf("round trips: %d maps (%d with baked tris)\n", g_maps, g_tris_maps);
    printf("editor_serialize_test: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
