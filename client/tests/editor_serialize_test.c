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
#include "editor_model.h"
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
    // the multi-action list: at most 5, action 0 stays
    for (int i = 0; i < 6; i++) ed_model_action_add(&o);
    CHECK(ed_model_action_count(o) == 5, "trigger actions are limited to 5 (%d)", ed_model_action_count(o));
    CHECK(!ed_model_action_remove(&o, 0) && ed_model_action_remove(&o, 4) && ed_model_action_count(o) == 4, "action 0 cannot be removed");
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

int main(int argc, char **argv) {
    const char *repo = argc > 1 ? argv[1] : ".";
    char path[1024];
    for (int i = 2; i < argc; i++) if (!strcmp(argv[i], "--verbose")) g_verbose = 1;
    snprintf(path, sizeof(path), "%s/client/tests/data/editor_v7_golden.json", repo);
    check_golden(path);
    for (int i = 2; i + 1 < argc; i++) if (!strcmp(argv[i], "--golden")) check_golden(argv[++i]);
    unit_checks();
    static const char *const dirs[3] = {"assets/maps", "assets/maps/custom", "assets/maps/v7"};
    for (int i = 0; i < 3; i++) {
        snprintf(path, sizeof(path), "%s/%s", repo, dirs[i]);
        round_trip_dir(path);
    }
    for (int i = 2; i + 1 < argc; i++) if (!strcmp(argv[i], "--maps")) round_trip_dir(argv[++i]);
    printf("round trips: %d maps (%d with baked tris)\n", g_maps, g_tris_maps);
    printf("editor_serialize_test: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
