// Map editor I/O: Krunker v3.1.1 editor.js getMapExport / importMap / loadObjects / save /
// clearMap / exportMap / quickStart, plus the storage autosave and the platform file and
// clipboard paths (native: files under the user data dir + the GLFW clipboard; web: a
// download / file input + navigator.clipboard).
//
// The exported JSON is the map format of assets/maps/*.json (and of Krunker's "custMap"):
// map config keys first, then objects (colours folded into a "colors" palette with ci/ei),
// config, game, camPos, spawns.
#include "editor_internal.h"
#include <html_ui.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#define STORAGE_KEY "knEditorMap"

// ---------------------------------------------------------------------------
// defaults (Editor.initialize mapConfig)

void ed_map_defaults(void) {
    cJSON_Delete(g_ed.map_config);
    const cJSON *d = editor_data_get("defaults");
    g_ed.map_config = cJSON_CreateObject();
    cJSON *m = g_ed.map_config;
    cJSON_AddStringToObject(m, "name", "New Krunker Map");
    cJSON_AddNumberToObject(m, "ambInd", 1);
    cJSON_AddStringToObject(m, "modURL", "");
    cJSON_AddStringToObject(m, "welMsg", "");
    cJSON_AddStringToObject(m, "terrainSeed", "");
    cJSON_AddStringToObject(m, "terrainTSeed", "");
    cJSON_AddNumberToObject(m, "terrainWidth", 3000);
    cJSON_AddNumberToObject(m, "terrainHeight", 3000);
    cJSON_AddNumberToObject(m, "terrainMntMlt", 1);
    cJSON_AddStringToObject(m, "terrainMntCol", "#707070");
    cJSON_AddStringToObject(m, "terrainSnwCol", "#e8fdff");
    cJSON_AddStringToObject(m, "terrainGrsCol", "#5e692f");
    cJSON_AddStringToObject(m, "terrainDrtCol", "#7f6238");
    cJSON_AddStringToObject(m, "terrainSndCol", "#967f51");
    cJSON_AddNumberToObject(m, "terrainEase", 12);
    cJSON_AddBoolToObject(m, "skyDome", 0);
    cJSON_AddStringToObject(m, "skyDomeCol0", "#74a4b9");
    cJSON_AddStringToObject(m, "skyDomeCol1", "#dce8ed");
    cJSON_AddStringToObject(m, "skyDomeCol2", "#dce8ed");
    cJSON_AddStringToObject(m, "skyDomeEmis", "#000000");
    cJSON_AddBoolToObject(m, "skyDomeTex", 0);
    cJSON_AddNumberToObject(m, "skyDomeMovD", 0);
    cJSON_AddNumberToObject(m, "skyDomeMovT", 0);
    cJSON_AddBoolToObject(m, "zone", 0);
    cJSON_AddNumberToObject(m, "zoneSize", 500);
    cJSON_AddNumberToObject(m, "zoneSpeed", 1);
    cJSON_AddBoolToObject(m, "zoneRnd", 0);
    cJSON_AddStringToObject(m, "zoneCol0", "#c542d9");
    cJSON_AddStringToObject(m, "zoneCol1", "#c542d9");
    cJSON_AddStringToObject(m, "zoneCol2", "#c542d9");
    cJSON_AddNumberToObject(m, "sizeMlt", 2);
    cJSON_AddNumberToObject(m, "shadowR", ed_json_num(d, "shadowRes", 1024));
    cJSON_AddNumberToObject(m, "shadowD", ed_json_num(d, "shadowDst", 1200));
    cJSON_AddStringToObject(m, "ambient", "#97a0a8");
    cJSON_AddNumberToObject(m, "ambientI", ed_json_num(d, "ambIntensity", 1));
    cJSON_AddStringToObject(m, "light", "#f2f8fc");
    cJSON_AddNumberToObject(m, "lightD", ed_json_num(d, "lightDistance", 500));
    cJSON_AddNumberToObject(m, "lightI", ed_json_num(d, "lightIntensity", 1.3));
    cJSON_AddNumberToObject(m, "sunAngX", ed_json_num(d, "sunAngleX", 90));
    cJSON_AddNumberToObject(m, "sunAngY", ed_json_num(d, "sunAngleY", 54));
    cJSON_AddStringToObject(m, "sky", "#dce8ed");
    cJSON_AddStringToObject(m, "fog", "#8d9aa0");
    cJSON_AddNumberToObject(m, "fogD", 2000);
    cJSON_AddNumberToObject(m, "dthY", -100);

    cJSON_Delete(g_ed.server_config);
    g_ed.server_config = cJSON_Parse("{\"modes\":{},\"classes\":{},\"settings\":{},\"weps\":{}}");
    cJSON_Delete(g_ed.game_config);
    g_ed.game_config = cJSON_CreateObject();
}

void ed_map_clear(void) {
    // clearMap
    ed_objects_clear();
    ed_history_clear();
    cJSON_Delete(g_ed.copy);
    g_ed.copy = NULL;
    g_ed.camera.position = (vec3) {0, 50, 100};
    g_ed.yaw = g_ed.pitch = 0;
    ed_map_defaults();
    editor_apply_lighting();
    ed_terrain_regen(); // regenAll: regenTerrain / regenZone
    ed_zone_regen();
    ed_panel_refresh_all();
    g_ed.dirty_counts = 1;
    g_ed.dirty_storage = 1;
}

// ---------------------------------------------------------------------------
// export (getMapExport)

static double num_of(const cJSON *v) {
    if (cJSON_IsNumber(v)) return cJSON_GetNumberValue(v);
    if (cJSON_IsBool(v)) return cJSON_IsTrue(v);
    if (cJSON_IsString(v)) return strtod(v->valuestring, NULL);
    return 0;
}

static int config_value_equal(const cJSON *a, const cJSON *b) {
    if (!a || !b) return 0;
    if ((cJSON_IsNumber(a) || cJSON_IsBool(a)) && (cJSON_IsNumber(b) || cJSON_IsBool(b))) return num_of(a) == num_of(b);
    return cJSON_Compare(a, b, 1);
}

static char *sanitize(const char *s) {
    // UTILS.sanitizeStr: drop < > & and non-ASCII
    char *out = malloc(strlen(s) + 1);
    size_t j = 0;
    for (size_t i = 0; out && s[i]; i++) {
        const unsigned char c = (unsigned char) s[i];
        if (c == '<' || c == '>' || c == '&' || c > 0x7f) continue;
        out[j++] = (char) c;
    }
    if (out) out[j] = 0;
    return out;
}

// gameConfig / serverConfig.settings entries: normalised, defaults dropped
static void export_config_list(cJSON *values, const cJSON *list, const int skip_hidden) {
    const cJSON *w;
    cJSON_ArrayForEach(w, list) {
        if (cJSON_GetObjectItemCaseSensitive(w, "cost")) continue;
        if (skip_hidden && ed_json_truthy(cJSON_GetObjectItemCaseSensitive(w, "hideE"))) continue;
        const char *var = ed_json_str(w, "varN", "");
        const cJSON *def = cJSON_GetObjectItemCaseSensitive(w, "def");
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(values, var);
        cJSON *nv;
        if (ed_json_truthy(cJSON_GetObjectItemCaseSensitive(w, "bool"))) nv = cJSON_CreateNumber(ed_json_truthy(v) ? 1 : 0);
        else if (ed_json_truthy(cJSON_GetObjectItemCaseSensitive(w, "input"))) {
            char *s = sanitize(cJSON_IsString(v) ? v->valuestring : "");
            nv = cJSON_CreateString(s ? s : "");
            free(s);
        } else if (ed_json_truthy(cJSON_GetObjectItemCaseSensitive(w, "select"))) {
            const int idx = (int) num_of(v);
            nv = idx >= 0 && idx < cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(w, "options")) ? cJSON_CreateNumber(idx) : cJSON_Duplicate(def, 1);
        } else nv = cJSON_CreateNumber(num_of(v));
        const int is_default = config_value_equal(nv, def) || (cJSON_IsNumber(nv) && cJSON_IsBool(def) && cJSON_GetNumberValue(nv) == cJSON_IsTrue(def));
        cJSON_DeleteItemFromObjectCaseSensitive(values, var);
        if (!is_default) cJSON_AddItemToObject(values, var, nv);
        else cJSON_Delete(nv);
    }
}

static int fold_color(cJSON *o, const char *key, cJSON *palette) {
    cJSON *c = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!c || !ed_json_truthy(c)) return 0;
    char hex[16];
    if (cJSON_IsNumber(c)) snprintf(hex, sizeof(hex), "#%06x", (int) cJSON_GetNumberValue(c) & 0xffffff);
    else snprintf(hex, sizeof(hex), "%s", cJSON_IsString(c) ? c->valuestring : "#000000");
    int index = -1, i = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, palette) {
        if (cJSON_IsString(it) && !strcmp(it->valuestring, hex)) index = i;
        i++;
    }
    if (index < 0) {
        index = cJSON_GetArraySize(palette);
        cJSON_AddItemToArray(palette, cJSON_CreateString(hex));
    }
    char ikey[8];
    snprintf(ikey, sizeof(ikey), "%si", key);
    cJSON_DeleteItemFromObjectCaseSensitive(o, key);
    cJSON_DeleteItemFromObjectCaseSensitive(o, ikey);
    cJSON_AddNumberToObject(o, ikey, index);
    return 1;
}

static int keep_map_key(const char *p, const cJSON *v) {
    const cJSON *mc = g_ed.map_config;
    const cJSON *d = editor_data_get("defaults");
    const int zone = ed_json_truthy(cJSON_GetObjectItemCaseSensitive(mc, "zone"));
    const char *seed = ed_json_str(mc, "terrainSeed", "");
    const int dome = ed_json_truthy(cJSON_GetObjectItemCaseSensitive(mc, "skyDome"));
    if (!strncmp(p, "zone", 4) && !zone) return 0;
    if ((!strncmp(p, "terrain", 7) || !strcmp(p, "sizeMlt")) && !seed[0]) return 0;
    if (!strncmp(p, "skyDome", 7) && !dome) return 0;
    const double n = num_of(v);
    if (!strcmp(p, "skyDomeEmis") && cJSON_IsString(v) && !strcasecmp(v->valuestring, "#000000")) return 0;
    if ((!strcmp(p, "skyDomeMovT") || !strcmp(p, "skyDomeMovD")) && n == 0) return 0;
    if ((!strcmp(p, "modURL") || !strcmp(p, "welMsg")) && !(cJSON_IsString(v) && v->valuestring[0])) return 0;
    if (!strcmp(p, "shadowR") && n == ed_json_num(d, "shadowRes", 1024)) return 0;
    if (!strcmp(p, "shadowD") && n == ed_json_num(d, "shadowDst", 1200)) return 0;
    if (!strcmp(p, "ambInd") && n == 1) return 0;
    if (!strcmp(p, "dthY") && n == -100) return 0;
    if (!strcmp(p, "lightD") && n == ed_json_num(d, "lightDistance", 500)) return 0;
    if (!strcmp(p, "lightI") && n == ed_json_num(d, "lightIntensity", 1.3)) return 0;
    if (!strcmp(p, "ambientI") && n == ed_json_num(d, "ambIntensity", 1)) return 0;
    if (!strcmp(p, "sunAngX") && n == ed_json_num(d, "sunAngleX", 90)) return 0;
    if (!strcmp(p, "sunAngY") && n == ed_json_num(d, "sunAngleY", 54)) return 0;
    return 1;
}

cJSON *ed_map_export(const int full) {
    cJSON *out = cJSON_CreateObject();
    // ...u: the map config
    const cJSON *it;
    cJSON_ArrayForEach(it, g_ed.map_config) {
        if (!keep_map_key(it->string, it)) continue;
        cJSON *v = cJSON_Duplicate(it, 1);
        if ((!strcmp(it->string, "fogD") || !strcmp(it->string, "shadowD")) && cJSON_IsNumber(v)) cJSON_SetNumberValue(v, round(cJSON_GetNumberValue(v)));
        cJSON_AddItemToObject(out, it->string, v);
    }
    // fields of an imported map the editor does not model (kept so maps round-trip)
    cJSON_ArrayForEach(it, g_ed.extra_map) {
        if (!cJSON_GetObjectItemCaseSensitive(out, it->string)) cJSON_AddItemToObject(out, it->string, cJSON_Duplicate(it, 1));
    }
    cJSON *objects = cJSON_AddArrayToObject(out, "objects");
    cJSON *palette = cJSON_CreateArray();
    cJSON *spawns = cJSON_CreateArray();
    cJSON *cam = NULL;
    for (size_t i = 0; i < g_ed.count; i++) {
        const EditorObject *o = g_ed.objects[i];
        if (!full && editor_prefab_has(o->prefab, EP_NOEXPORT)) continue;
        if (!full && o->prefab == ED_PREFAB_SPAWN_POINT) {
            const vec3 p = ed_obj_vec(o, "p", (vec3) {0});
            cJSON *s = cJSON_CreateArray();
            cJSON_AddItemToArray(s, cJSON_CreateNumber(ed_round(p.x, 2)));
            cJSON_AddItemToArray(s, cJSON_CreateNumber(ed_round(p.y, 2)));
            cJSON_AddItemToArray(s, cJSON_CreateNumber(ed_round(p.z, 2)));
            cJSON_AddItemToArray(s, cJSON_CreateNumber((int) ed_json_num(o->data, "tm", 0)));
            cJSON_AddItemToArray(s, cJSON_CreateNumber((int) ed_json_num(o->data, "d", 0)));
            cJSON_AddItemToArray(s, cJSON_CreateNumber(ed_json_path(o->data, "cp") ? 1 : 0));
            cJSON_AddItemToArray(spawns, s);
            continue;
        }
        if (!full && o->prefab == ED_PREFAB_CAMERA_POSITION) {
            cJSON_Delete(cam);
            cam = cJSON_Duplicate(ed_json_path(o->data, "p"), 1);
            continue;
        }
        cJSON *s = ed_obj_serialize(o);
        if (full) cJSON_AddNumberToObject(s, "objUUID", o->uuid);
        fold_color(s, "e", palette);
        fold_color(s, "c", palette);
        cJSON_AddItemToArray(objects, s);
    }
    if (cJSON_GetArraySize(palette)) cJSON_AddItemToObject(out, "colors", palette);
    else cJSON_Delete(palette);

    // config: modes / classes / settings / weps
    cJSON *a = cJSON_Duplicate(g_ed.server_config, 1);
    cJSON *modes = cJSON_CreateArray();
    int count = 0, only_zero = 1;
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(a, "modes")) {
        if (ed_json_truthy(it)) {
            const int idx = atoi(it->string);
            cJSON_AddItemToArray(modes, cJSON_CreateNumber(idx));
            count++;
            if (idx != 0) only_zero = 0;
        }
    }
    cJSON_DeleteItemFromObjectCaseSensitive(a, "modes");
    if (count && !(count == 1 && only_zero)) cJSON_AddItemToObject(a, "modes", modes);
    else cJSON_Delete(modes);
    cJSON *classes = cJSON_CreateArray();
    count = 0;
    const int class_total = cJSON_GetArraySize(editor_data_get("classes"));
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(a, "classes")) {
        if (ed_json_truthy(it)) {
            cJSON_AddItemToArray(classes, cJSON_CreateNumber(atoi(it->string)));
            count++;
        }
    }
    cJSON_DeleteItemFromObjectCaseSensitive(a, "classes");
    if (count && count != class_total) cJSON_AddItemToObject(a, "classes", classes);
    else cJSON_Delete(classes);
    cJSON *settings = cJSON_GetObjectItemCaseSensitive(a, "settings");
    if (settings) export_config_list(settings, editor_data_get("serverConfig"), 1);
    if (!cJSON_GetArraySize(settings)) cJSON_DeleteItemFromObjectCaseSensitive(a, "settings");
    cJSON *weps = cJSON_GetObjectItemCaseSensitive(a, "weps");
    int wi = 0;
    const cJSON *weapon;
    cJSON_ArrayForEach(weapon, editor_data_get("weapons")) {
        char key[8];
        snprintf(key, sizeof(key), "%d", wi++);
        cJSON *wc = cJSON_GetObjectItemCaseSensitive(weps, key);
        if (!wc) continue;
        const cJSON *x;
        cJSON_ArrayForEach(x, editor_data_get("weaponConfig")) {
            const char *var = ed_json_str(x, "varN", "");
            const cJSON *def = cJSON_GetObjectItemCaseSensitive(weapon, var);
            if (def && config_value_equal(cJSON_GetObjectItemCaseSensitive(wc, var), def)) cJSON_DeleteItemFromObjectCaseSensitive(wc, var);
        }
        if (!cJSON_GetArraySize(wc)) cJSON_DeleteItemFromObjectCaseSensitive(weps, key);
    }
    if (!cJSON_GetArraySize(weps)) cJSON_DeleteItemFromObjectCaseSensitive(a, "weps");
    if (cJSON_GetArraySize(a)) cJSON_AddItemToObject(out, "config", a);
    else cJSON_Delete(a);

    cJSON *game = cJSON_Duplicate(g_ed.game_config, 1);
    export_config_list(game, editor_data_get("gameConfig"), 0);
    if (cJSON_GetArraySize(game)) cJSON_AddItemToObject(out, "game", game);
    else cJSON_Delete(game);

    if (!full && cam) cJSON_AddItemToObject(out, "camPos", cam);
    else cJSON_Delete(cam);
    if (!full) cJSON_AddItemToObject(out, "spawns", spawns);
    else cJSON_Delete(spawns);
    return out;
}

char *ed_map_export_string(void) {
    cJSON *m = ed_map_export(0);
    char *text = cJSON_PrintUnformatted(m);
    cJSON_Delete(m);
    return text;
}

// ---------------------------------------------------------------------------
// import (importMap / loadObjects)

static const char *const k_map_keys[] = {
    "name", "ambInd", "modURL", "welMsg", "terrainSeed", "terrainTSeed", "terrainWidth", "terrainHeight", "terrainMntMlt",
    "terrainMntCol", "terrainSnwCol", "terrainGrsCol", "terrainDrtCol", "terrainSndCol", "terrainEase", "skyDome",
    "skyDomeCol0", "skyDomeCol1", "skyDomeCol2", "skyDomeEmis", "skyDomeTex", "skyDomeMovD", "skyDomeMovT", "zone",
    "zoneSize", "zoneSpeed", "zoneRnd", "zoneCol0", "zoneCol1", "zoneCol2", "sizeMlt", "shadowR", "shadowD", "ambient",
    "ambientI", "light", "lightD", "lightI", "sunAngX", "sunAngY", "sky", "fog", "fogD", "dthY", NULL,
};

static int known_map_key(const char *k) {
    for (int i = 0; k_map_keys[i]; i++) if (!strcmp(k_map_keys[i], k)) return 1;
    return 0;
}

static void import_config(const cJSON *config) {
    const cJSON *modes = cJSON_GetObjectItemCaseSensitive(config, "modes");
    if (cJSON_GetArraySize(modes)) {
        cJSON *m = cJSON_GetObjectItemCaseSensitive(g_ed.server_config, "modes");
        const int n = cJSON_GetArraySize(editor_data_get("modes"));
        for (int r = 0; r < n; r++) {
            int on = 0;
            const cJSON *it;
            cJSON_ArrayForEach(it, modes) if ((int) num_of(it) == r) on = 1;
            char key[8];
            snprintf(key, sizeof(key), "%d", r);
            ed_json_set_bool(m, key, on);
        }
    }
    const cJSON *classes = cJSON_GetObjectItemCaseSensitive(config, "classes");
    if (cJSON_GetArraySize(classes)) {
        cJSON *c = cJSON_GetObjectItemCaseSensitive(g_ed.server_config, "classes");
        const int n = cJSON_GetArraySize(editor_data_get("classes"));
        for (int r = 0; r < n; r++) {
            int on = 0;
            const cJSON *it;
            cJSON_ArrayForEach(it, classes) if ((int) num_of(it) == r) on = 1;
            char key[8];
            snprintf(key, sizeof(key), "%d", r);
            ed_json_set_bool(c, key, on);
        }
    }
    const cJSON *settings = cJSON_GetObjectItemCaseSensitive(config, "settings");
    const cJSON *w;
    cJSON_ArrayForEach(w, editor_data_get("serverConfig")) {
        if (ed_json_truthy(cJSON_GetObjectItemCaseSensitive(w, "hideE"))) continue;
        const char *var = ed_json_str(w, "varN", "");
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(settings, var);
        if (!v || cJSON_IsNull(v)) continue;
        cJSON *s = cJSON_GetObjectItemCaseSensitive(g_ed.server_config, "settings");
        cJSON_DeleteItemFromObjectCaseSensitive(s, var);
        cJSON_AddItemToObject(s, var, cJSON_Duplicate(v, 1));
    }
    const cJSON *weps = cJSON_GetObjectItemCaseSensitive(config, "weps");
    cJSON *ws = cJSON_GetObjectItemCaseSensitive(g_ed.server_config, "weps");
    const cJSON *it;
    cJSON_ArrayForEach(it, weps) {
        if (!cJSON_IsObject(it)) continue;
        cJSON *dst = cJSON_GetObjectItemCaseSensitive(ws, it->string);
        if (!dst) dst = cJSON_AddObjectToObject(ws, it->string);
        const cJSON *x;
        cJSON_ArrayForEach(x, editor_data_get("weaponConfig")) {
            const char *var = ed_json_str(x, "varN", "");
            const cJSON *v = cJSON_GetObjectItemCaseSensitive(it, var);
            if (!v || cJSON_IsNull(v)) continue;
            cJSON_DeleteItemFromObjectCaseSensitive(dst, var);
            cJSON_AddItemToObject(dst, var, cJSON_Duplicate(v, 1));
        }
    }
}

static void add_tool_object(const int prefab, const cJSON *p, const int team, const int dir, const int cmp) {
    cJSON *d = cJSON_CreateObject();
    cJSON_AddNumberToObject(d, "i", prefab);
    cJSON_AddItemToObject(d, "p", cJSON_Duplicate(p, 1));
    if (prefab == ED_PREFAB_SPAWN_POINT) {
        cJSON_AddNumberToObject(d, "tm", team);
        if (dir) cJSON_AddNumberToObject(d, "d", dir);
        if (cmp) cJSON_AddNumberToObject(d, "cp", 1);
    }
    EditorObject *o = ed_obj_new(d, 0);
    cJSON_Delete(d);
    ed_obj_add(o, 1, 0);
}

static int import_doc(cJSON *e) {
    // `e.states || e.id || e.map ? e.map : e` (a save file / a hub map record holds .map)
    cJSON *map = cJSON_GetObjectItemCaseSensitive(e, "map");
    if (!(cJSON_GetObjectItemCaseSensitive(e, "states") || cJSON_GetObjectItemCaseSensitive(e, "id") || map)) map = e;
    if (cJSON_IsString(map)) {
        cJSON *inner = cJSON_Parse(map->valuestring);
        if (!inner) return 0;
        const int ok = import_doc(inner);
        cJSON_Delete(inner);
        return ok;
    }
    if (!cJSON_IsObject(map)) return 0;
    ed_objects_clear();
    ed_history_clear();
    ed_map_defaults();
    cJSON_Delete(g_ed.extra_map);
    g_ed.extra_map = cJSON_CreateObject();
    g_ed.camera.position = (vec3) {0, 50, 100};
    g_ed.yaw = g_ed.pitch = 0;

    // loadObjects: palette colours back inline
    const cJSON *colors = cJSON_GetObjectItemCaseSensitive(map, "colors");
    const cJSON *obj;
    cJSON_ArrayForEach(obj, cJSON_GetObjectItemCaseSensitive(map, "objects")) {
        cJSON *d = cJSON_Duplicate(obj, 1);
        static const char *keys[2] = {"e", "c"};
        for (int k = 0; k < 2; k++) {
            char ikey[4];
            snprintf(ikey, sizeof(ikey), "%si", keys[k]);
            const cJSON *idx = cJSON_GetObjectItemCaseSensitive(d, ikey);
            const cJSON *col = cJSON_IsNumber(idx) ? cJSON_GetArrayItem(colors, (int) cJSON_GetNumberValue(idx)) : NULL;
            if (cJSON_IsString(col)) {
                cJSON_DeleteItemFromObjectCaseSensitive(d, keys[k]);
                cJSON_AddStringToObject(d, keys[k], col->valuestring);
                cJSON_DeleteItemFromObjectCaseSensitive(d, ikey);
            }
        }
        EditorObject *o = ed_obj_new(d, 1);
        cJSON_Delete(d);
        ed_obj_add(o, 1, 0);
    }
    const cJSON *cam = cJSON_GetObjectItemCaseSensitive(map, "camPos");
    if (cJSON_GetArraySize(cam) >= 3) add_tool_object(ED_PREFAB_CAMERA_POSITION, cam, 0, 0, 0);
    const cJSON *sp;
    cJSON_ArrayForEach(sp, cJSON_GetObjectItemCaseSensitive(map, "spawns")) {
        if (cJSON_GetArraySize(sp) < 3) continue;
        cJSON *p = cJSON_CreateArray();
        for (int i = 0; i < 3; i++) cJSON_AddItemToArray(p, cJSON_CreateNumber(num_of(cJSON_GetArrayItem(sp, i))));
        add_tool_object(ED_PREFAB_SPAWN_POINT, p, (int) num_of(cJSON_GetArrayItem(sp, 3)), (int) num_of(cJSON_GetArrayItem(sp, 4)), (int) num_of(cJSON_GetArrayItem(sp, 5)));
        cJSON_Delete(p);
    }
    const cJSON *config = cJSON_GetObjectItemCaseSensitive(map, "config");
    if (config) import_config(config);
    const cJSON *game = cJSON_GetObjectItemCaseSensitive(map, "game");
    const cJSON *w;
    cJSON_ArrayForEach(w, editor_data_get("gameConfig")) {
        const char *var = ed_json_str(w, "varN", "");
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(game, var);
        if (!v || cJSON_IsNull(v)) continue;
        cJSON_DeleteItemFromObjectCaseSensitive(g_ed.game_config, var);
        cJSON_AddItemToObject(g_ed.game_config, var, cJSON_Duplicate(v, 1));
    }
    // Object.assign(mapConfig, importedMap) without objects/camPos/spawns/config/game
    const cJSON *it;
    cJSON_ArrayForEach(it, map) {
        const char *k = it->string;
        if (!strcmp(k, "objects") || !strcmp(k, "camPos") || !strcmp(k, "spawns") || !strcmp(k, "config") || !strcmp(k, "game") || !strcmp(k, "colors")) continue;
        cJSON *target = known_map_key(k) ? g_ed.map_config : g_ed.extra_map;
        cJSON_DeleteItemFromObjectCaseSensitive(target, k);
        cJSON_AddItemToObject(target, k, cJSON_Duplicate(it, 1));
    }
    // a save file's camera / selection
    const cJSON *ecam = cJSON_GetObjectItemCaseSensitive(e, "cam");
    if (ecam) {
        const cJSON *p = cJSON_GetObjectItemCaseSensitive(ecam, "p"), *r = cJSON_GetObjectItemCaseSensitive(ecam, "r");
        if (cJSON_GetArraySize(p) >= 3) g_ed.camera.position = (vec3) {(float) num_of(cJSON_GetArrayItem(p, 0)), (float) num_of(cJSON_GetArrayItem(p, 1)), (float) num_of(cJSON_GetArrayItem(p, 2))};
        if (cJSON_GetArraySize(r) >= 2) {
            g_ed.yaw = (float) num_of(cJSON_GetArrayItem(r, 0));
            g_ed.pitch = (float) num_of(cJSON_GetArrayItem(r, 1));
        }
    }
    const cJSON *selected = cJSON_GetObjectItemCaseSensitive(e, "selected");
    ed_obj_select(cJSON_IsNumber(selected) ? ed_obj_find((uint32_t) cJSON_GetNumberValue(selected)) : NULL);
    const cJSON *groups = cJSON_GetObjectItemCaseSensitive(e, "groups");
    cJSON_ArrayForEach(it, groups) {
        EditorObject *owner = ed_obj_find((uint32_t) strtoul(it->string, NULL, 10));
        const cJSON *ids = cJSON_GetObjectItemCaseSensitive(it, "ids");
        if (!owner || !cJSON_GetArraySize(ids)) continue;
        EditorGroup *gs = realloc(g_ed.groups, (g_ed.group_count + 1) * sizeof(EditorGroup));
        if (!gs) break;
        g_ed.groups = gs;
        EditorGroup *g = &g_ed.groups[g_ed.group_count++];
        memset(g, 0, sizeof(*g));
        g->owner = owner->uuid;
        g->objects = calloc((size_t) cJSON_GetArraySize(ids), sizeof(uint32_t));
        const cJSON *id;
        cJSON_ArrayForEach(id, ids) if (g->objects) g->objects[g->count++] = (uint32_t) num_of(id);
        g->pos = ed_obj_vec(owner, "p", (vec3) {0});
        g->scale = ed_obj_vec(owner, "s", (vec3) {1, 1, 1});
        g->rot_y = ed_obj_vec(owner, "r", (vec3) {0}).y;
    }
    editor_apply_lighting();
    ed_terrain_regen(); // regenAll: regenTerrain / regenZone
    ed_zone_regen();
    ed_panel_refresh_all();
    g_ed.dirty_counts = 1;
    g_ed.dirty_panel = 1;
    return 1;
}

int ed_map_import(const char *text) {
    if (!text || !*text) return 0;
    cJSON *e = cJSON_Parse(text);
    if (!e) {
        editor_message("Failed to import map: not valid JSON.");
        return 0;
    }
    const int ok = import_doc(e);
    cJSON_Delete(e);
    if (!ok) editor_message("Failed to import map: no map data.");
    return ok;
}

// ---------------------------------------------------------------------------
// save files (Editor.save) and the autosave

static cJSON *save_doc(void) {
    cJSON *n = cJSON_CreateObject();
    cJSON_AddItemToObject(n, "map", ed_map_export(1));
    cJSON *groups = cJSON_AddObjectToObject(n, "groups");
    for (size_t i = 0; i < g_ed.group_count; i++) {
        char key[16];
        snprintf(key, sizeof(key), "%u", g_ed.groups[i].owner);
        cJSON *g = cJSON_AddObjectToObject(groups, key);
        cJSON *ids = cJSON_AddArrayToObject(g, "ids");
        for (size_t j = 0; j < g_ed.groups[i].count; j++) cJSON_AddItemToArray(ids, cJSON_CreateNumber(g_ed.groups[i].objects[j]));
    }
    if (g_ed.selected) cJSON_AddNumberToObject(n, "selected", g_ed.selected->uuid);
    cJSON *cam = cJSON_AddObjectToObject(n, "cam");
    cJSON *p = cJSON_AddArrayToObject(cam, "p");
    cJSON_AddItemToArray(p, cJSON_CreateNumber(g_ed.camera.position.x));
    cJSON_AddItemToArray(p, cJSON_CreateNumber(g_ed.camera.position.y));
    cJSON_AddItemToArray(p, cJSON_CreateNumber(g_ed.camera.position.z));
    cJSON *r = cJSON_AddArrayToObject(cam, "r");
    cJSON_AddItemToArray(r, cJSON_CreateNumber(g_ed.yaw));
    cJSON_AddItemToArray(r, cJSON_CreateNumber(g_ed.pitch));
    cJSON_AddItemToArray(r, cJSON_CreateNumber(0));
    cJSON_AddArrayToObject(n, "history");
    return n;
}

void ed_map_store(void) {
    if (!g_ed.map_config) return;
    cJSON *doc = save_doc();
    char *text = cJSON_PrintUnformatted(doc);
    cJSON_Delete(doc);
    if (text) {
        html_storage_set(STORAGE_KEY, text);
        free(text);
    }
}

static char *storage_read(const char *key) {
    const int n = html_storage_get(key, NULL, 0);
    if (n <= 0) return NULL;
    char *buf = malloc((size_t) n + 1);
    if (!buf) return NULL;
    html_storage_get(key, buf, (size_t) n + 1);
    return buf;
}

int ed_map_restore(void) {
    // checkEdit: "mapToEdit" (the hub's Edit button) first, else our autosave
    char *text = storage_read("mapToEdit");
    if (text) {
        html_storage_set("mapToEdit", NULL);
        const int ok = ed_map_import(text);
        free(text);
        if (ok) return 1;
    }
    text = storage_read(STORAGE_KEY);
    if (!text) return 0;
    const int ok = ed_map_import(text);
    free(text);
    return ok;
}

void ed_quick_start(void) {
    // quickStart: a 100x10x100 floor at y -10 and a spawn point at the origin
    cJSON *floor = cJSON_Parse("{\"p\":[0,-10,0],\"s\":[100,10,100]}");
    ed_obj_add(ed_obj_new(floor, 0), 1, 1);
    cJSON_Delete(floor);
    cJSON *spawn = cJSON_Parse("{\"i\":5,\"p\":[0,0,0]}");
    ed_obj_add(ed_obj_new(spawn, 0), 1, 1);
    cJSON_Delete(spawn);
}

// ---------------------------------------------------------------------------
// files and clipboard

#ifdef __EMSCRIPTEN__
EM_JS(void, kn_editor_download, (const char *name, const char *text), {
    var blob = new Blob([UTF8ToString(text)], {type: "text/plain"});
    var a = document.createElement("a");
    a.style.display = "none";
    a.href = URL.createObjectURL(blob);
    a.download = UTF8ToString(name);
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);
});

EM_JS(void, kn_editor_pick_file, (void), {
    var input = document.createElement("input");
    input.type = "file";
    input.accept = ".txt,.json,text/plain,application/json";
    input.onchange = function () {
        var f = input.files && input.files[0];
        if (!f) return;
        var r = new FileReader();
        r.onload = function () { window.__knEditorFile = String(r.result || ""); };
        r.readAsText(f);
    };
    input.click();
});

EM_JS(char *, kn_editor_take_file, (void), {
    var t = window.__knEditorFile;
    if (t === undefined || t === null) return 0;
    window.__knEditorFile = null;
    return stringToNewUTF8(t);
});

EM_JS(void, kn_editor_clipboard_write, (const char *text), {
    var t = UTF8ToString(text);
    window.__knEditorClip = t;
    try { navigator.clipboard.writeText(t); } catch (e) {}
});

EM_JS(void, kn_editor_clipboard_request, (void), {
    try {
        navigator.clipboard.readText().then(function (t) { window.__knEditorFile = t; }, function () {
            var t = window.prompt("Copy Paste Map Text Here");
            if (t) window.__knEditorFile = t;
        });
    } catch (e) {
        var t = window.prompt("Copy Paste Map Text Here");
        if (t) window.__knEditorFile = t;
    }
});

static double g_file_poll_until;

static void web_poll_file(void *user) {
    (void) user;
    char *text = kn_editor_take_file();
    if (text) {
        if (g_ed.active) ed_map_import(text);
        free(text);
        return;
    }
    if (g_ed.active && glfwGetTime() < g_file_poll_until) html_timer_add(200, web_poll_file, NULL);
}

static void web_wait_file(void) {
    g_file_poll_until = glfwGetTime() + 120.0;
    html_timer_add(200, web_poll_file, NULL);
}
#else
static int user_maps_dir(char *out, const size_t n) {
    // $XDG_DATA_HOME/krunknative/maps (else ~/.local/share/..., Windows %USERPROFILE%)
    const char *xdg = getenv("XDG_DATA_HOME");
    char base[512];
    if (xdg && *xdg) snprintf(base, sizeof(base), "%s", xdg);
    else {
        const char *home = getenv("HOME");
#ifdef _WIN32
        if (!home || !*home) home = getenv("USERPROFILE");
#endif
        snprintf(base, sizeof(base), "%s/.local/share", home && *home ? home : ".");
    }
    char path[640];
    snprintf(path, sizeof(path), "%s", base);
#ifdef _WIN32
    mkdir(path);
    snprintf(path, sizeof(path), "%s/krunknative", base);
    mkdir(path);
    snprintf(out, n, "%s/krunknative/maps", base);
    mkdir(out);
#else
    mkdir(path, 0700);
    snprintf(path, sizeof(path), "%s/krunknative", base);
    mkdir(path, 0700);
    snprintf(out, n, "%s/krunknative/maps", base);
    mkdir(out, 0700);
#endif
    return 1;
}

static void safe_name(const char *in, char *out, const size_t n) {
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 1 < n; i++) {
        const char c = in[i];
        out[j++] = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ? c : '_';
    }
    out[j] = 0;
    if (!j) snprintf(out, n, "map");
}

static int write_user_file(const char *suffix, const char *text, char *path_out, const size_t n) {
    char dir[600], name[128];
    user_maps_dir(dir, sizeof(dir));
    safe_name(ed_json_str(g_ed.map_config, "name", "map"), name, sizeof(name));
    snprintf(path_out, n, "%s/%s%s", dir, name, suffix);
    FILE *f = fopen(path_out, "wb");
    if (!f) return 0;
    fwrite(text, 1, strlen(text), f);
    fclose(f);
    return 1;
}
#endif

void ed_clipboard_copy(const char *text) {
#ifdef __EMSCRIPTEN__
    kn_editor_clipboard_write(text);
#else
    if (g_ed.client && g_ed.client->window) glfwSetClipboardString(g_ed.client->window, text);
#endif
}

char *ed_clipboard_paste(void) {
#ifdef __EMSCRIPTEN__
    return NULL; // async on the web: ed_file_import requests it
#else
    const char *s = g_ed.client && g_ed.client->window ? glfwGetClipboardString(g_ed.client->window) : NULL;
    if (!s) return NULL;
    char *copy = malloc(strlen(s) + 1);
    if (copy) strcpy(copy, s);
    return copy;
#endif
}

void ed_file_export(void) {
    // exportMap: download "<name>_export" (and, with shift, copy it); KrunkNative always copies
    char *text = ed_map_export_string();
    if (!text) return;
    ed_clipboard_copy(text);
#ifdef __EMSCRIPTEN__
    char name[160];
    snprintf(name, sizeof(name), "%s_export", ed_json_str(g_ed.map_config, "name", "map"));
    kn_editor_download(name, text);
    editor_message("Map exported (download) and copied to the clipboard.");
#else
    char path[800], msg[900];
    if (write_user_file("_export.json", text, path, sizeof(path))) snprintf(msg, sizeof(msg), "Map exported to %s and copied to the clipboard.", path);
    else snprintf(msg, sizeof(msg), "Map copied to the clipboard (could not write %s).", path);
    editor_message(msg);
#endif
    free(text);
}

// download(text, name, "text/plain"): the web build downloads the file; natively it is
// written to the user maps directory under that name and copied to the clipboard
void ed_download(const char *text, const char *name) {
    if (!text) return;
#ifdef __EMSCRIPTEN__
    kn_editor_download(name, text);
#else
    char dir[600], safe[128], path[800], msg[900];
    user_maps_dir(dir, sizeof(dir));
    size_t j = 0;
    for (size_t i = 0; name[i] && j + 1 < sizeof(safe); i++) {
        const char c = name[i];
        safe[j++] = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ? c : '_';
    }
    safe[j] = 0;
    snprintf(path, sizeof(path), "%s/%s", dir, safe);
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(text, 1, strlen(text), f);
        fclose(f);
    }
    ed_clipboard_copy(text);
    snprintf(msg, sizeof(msg), f ? "Saved %s (also copied to the clipboard)." : "Could not write %s (copied to the clipboard).", path);
    editor_message(msg);
#endif
}

void ed_file_save_as(void) {
    // save(): "<name>_save.txt" with groups, selection and camera
    cJSON *doc = save_doc();
    char *text = cJSON_PrintUnformatted(doc);
    cJSON_Delete(doc);
    if (!text) return;
#ifdef __EMSCRIPTEN__
    char name[160];
    snprintf(name, sizeof(name), "%s_save.txt", ed_json_str(g_ed.map_config, "name", "map"));
    kn_editor_download(name, text);
#else
    char path[800], msg[900];
    if (write_user_file("_save.txt", text, path, sizeof(path))) snprintf(msg, sizeof(msg), "Saved %s.", path);
    else snprintf(msg, sizeof(msg), "Could not write %s.", path);
    editor_message(msg);
#endif
    free(text);
}

void ed_file_import(void) {
    // importMap(): prompt("Copy Paste Map Text Here") -> the clipboard (native) / a file
    // picker or the clipboard (web); shift = loadFile in Krunker
#ifdef __EMSCRIPTEN__
    if (g_ed.mods & GLFW_MOD_SHIFT) kn_editor_clipboard_request();
    else kn_editor_pick_file();
    web_wait_file();
#else
    char *text = ed_clipboard_paste();
    if (text && (text[0] == '{' || text[0] == ' ' || text[0] == '\n')) {
        ed_map_import(text);
        free(text);
        return;
    }
    free(text);
    // no map on the clipboard: load the last export of a map with this name from the user dir
    char dir[600], name[128], path[800];
    user_maps_dir(dir, sizeof(dir));
    safe_name(ed_json_str(g_ed.map_config, "name", "map"), name, sizeof(name));
    snprintf(path, sizeof(path), "%s/%s_export.json", dir, name);
    unsigned char *buf = NULL;
    size_t len = 0;
    if (!read_file(path, &buf, &len)) {
        char *t = malloc(len + 1);
        if (t) {
            memcpy(t, buf, len);
            t[len] = 0;
            ed_map_import(t);
            free(t);
        }
        free(buf);
        return;
    }
    char msg[900];
    snprintf(msg, sizeof(msg), "Copy a map's JSON to the clipboard, then Import (exports are saved in %s).", dir);
    editor_message(msg);
#endif
}
