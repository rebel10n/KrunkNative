// Map editor I/O: Krunker v7.1.4 editor.js save / clearMap / exportMap / loadQuick, the editor
// side of getMapExport / importMap (the map JSON itself is editor_model.c's, GL-free), plus the
// storage autosave and the platform file and clipboard paths (native: files under the user data
// dir + the GLFW clipboard; web: a download / file input + navigator.clipboard).
//
// The exported JSON is the v7.1.4 map format (assets/maps/*.json, Krunker's "custMap"): map
// config keys first, colors / xyz pools, objects, config, gui, scripts, camPos, spawns, tris.
#include "editor_internal.h"
#include "editor_model.h"
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

// the project's map state as the model sees it (borrowed pointers)
static EdMapState proj_state(const EditorProject *p) {
    return (EdMapState) {p->map_config, p->extra_map, p->server_config, p->gui, p->scripts};
}

static void proj_adopt(EditorProject *p, EdMapState *st) {
    EdMapState old = proj_state(p);
    ed_model_map_free(&old);
    p->map_config = st->map_config;
    p->extra_map = st->extra;
    p->server_config = st->server_config;
    p->gui = st->gui;
    p->scripts = st->scripts;
    memset(st, 0, sizeof(*st));
}

// ---------------------------------------------------------------------------
// defaults (Editor.defineDefaults: mapConfig, serverConfig, custom GUI, scripts)

void ed_map_defaults(void) {
    EdMapState st;
    ed_model_map_init(&st);
    proj_adopt(g_ed.proj, &st);
}

void ed_map_set_all_classes(const char *var, const cJSON *value) {
    // the v3.1.1 map-wide "game" value on every class (v7 imports a "game" block the same way)
    cJSON *cls = cJSON_GetObjectItemCaseSensitive(g_ed.proj->server_config, "cls");
    if (!cJSON_IsObject(cls)) {
        cJSON_DeleteItemFromObjectCaseSensitive(g_ed.proj->server_config, "cls");
        cls = cJSON_AddObjectToObject(g_ed.proj->server_config, "cls");
    }
    const int n = cJSON_GetArraySize(editor_data_get("classes"));
    for (int i = 0; i < n; i++) {
        char key[16];
        snprintf(key, sizeof(key), "%d", i);
        cJSON *c = cJSON_GetObjectItemCaseSensitive(cls, key);
        if (!cJSON_IsObject(c)) {
            cJSON_DeleteItemFromObjectCaseSensitive(cls, key);
            c = cJSON_AddObjectToObject(cls, key);
        }
        cJSON_DeleteItemFromObjectCaseSensitive(c, var);
        if (value) cJSON_AddItemToObject(c, var, cJSON_Duplicate(value, 1));
    }
    ed_mark_changed();
}

void ed_map_clear(void) {
    // clearMap
    ed_objects_clear();
    ed_history_clear();
    cJSON_Delete(g_ed.copy);
    g_ed.copy = NULL;
    g_ed.proj->camera.position = (vec3) {0, 50, 100};
    g_ed.proj->yaw = g_ed.proj->pitch = 0;
    ed_map_defaults();
    editor_apply_lighting();
    ed_terrain_regen(); // regenAll: regenTerrain / regenZone
    ed_zone_regen();
    ed_panel_refresh_all();
    g_ed.dirty_counts = 1;
    ed_mark_changed();
}

// ---------------------------------------------------------------------------
// export (getMapExport): editor_model.c

cJSON *ed_map_export_flags(const int flags) {
    const EditorProject *p = g_ed.proj;
    EdModelObj *list = calloc(p->count ? p->count : 1, sizeof(EdModelObj));
    if (!list) return NULL;
    for (size_t i = 0; i < p->count; i++) list[i] = (EdModelObj) {p->objects[i]->data, p->objects[i]->prefab, p->objects[i]->uuid};
    const EdMapState st = proj_state(p);
    cJSON *out = ed_model_export(&st, list, p->count, flags);
    free(list);
    return out;
}

cJSON *ed_map_export(const int full) { return ed_map_export_flags(full ? ED_EXPORT_RAW : 0); }

char *ed_map_export_string(void) {
    cJSON *m = ed_map_export(0);
    char *text = ed_model_print(m); // JSON.stringify(getMapExport())
    cJSON_Delete(m);
    return text;
}

// ---------------------------------------------------------------------------
// import (importMap / loadObjects): editor_model.c, then the editor objects and a save's extras

static double num_at(const cJSON *a, const int i) {
    const cJSON *v = cJSON_GetArrayItem(a, i);
    return cJSON_IsNumber(v) ? v->valuedouble : 0.0;
}

static int import_doc(cJSON *e) {
    EdMapState st;
    ed_model_map_init(&st);
    cJSON *objects = NULL;
    if (!ed_model_import(e, &st, &objects, NULL)) {
        ed_model_map_free(&st);
        return 0;
    }
    ed_objects_clear();
    ed_history_clear();
    proj_adopt(g_ed.proj, &st);
    g_ed.proj->camera.position = (vec3) {0, 50, 100};
    g_ed.proj->yaw = g_ed.proj->pitch = 0;
    const cJSON *obj;
    cJSON_ArrayForEach(obj, objects) ed_obj_add(ed_obj_new(obj, 1), 1, 0); // objUUIDs of a save kept
    cJSON_Delete(objects);

    // a save file's camera / selection / groups (the wrapper's members)
    const cJSON *ecam = cJSON_GetObjectItemCaseSensitive(e, "cam");
    if (ecam) {
        const cJSON *p = cJSON_GetObjectItemCaseSensitive(ecam, "p"), *r = cJSON_GetObjectItemCaseSensitive(ecam, "r");
        if (cJSON_GetArraySize(p) >= 3) g_ed.proj->camera.position = (vec3) {(float) num_at(p, 0), (float) num_at(p, 1), (float) num_at(p, 2)};
        if (cJSON_GetArraySize(r) >= 2) { // controls.setRotation(r[0], r[1]): pitch, yaw
            g_ed.proj->pitch = (float) num_at(r, 0);
            g_ed.proj->yaw = (float) num_at(r, 1);
        }
    }
    const cJSON *selected = cJSON_GetObjectItemCaseSensitive(e, "selected");
    ed_obj_select(cJSON_IsNumber(selected) ? ed_obj_find((uint32_t) cJSON_GetNumberValue(selected)) : NULL);
    const cJSON *it;
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(e, "groups")) {
        EditorObject *owner = ed_obj_find((uint32_t) strtoul(it->string, NULL, 10));
        const cJSON *ids = cJSON_GetObjectItemCaseSensitive(it, "ids");
        if (!owner || !cJSON_GetArraySize(ids)) continue;
        EditorGroup *gs = realloc(g_ed.proj->groups, (g_ed.proj->group_count + 1) * sizeof(EditorGroup));
        if (!gs) break;
        g_ed.proj->groups = gs;
        EditorGroup *g = &g_ed.proj->groups[g_ed.proj->group_count++];
        memset(g, 0, sizeof(*g));
        g->owner = owner->uuid;
        g->objects = calloc((size_t) cJSON_GetArraySize(ids), sizeof(uint32_t));
        const cJSON *id;
        cJSON_ArrayForEach(id, ids) if (g->objects && cJSON_IsNumber(id)) g->objects[g->count++] = (uint32_t) cJSON_GetNumberValue(id);
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
    for (size_t i = 0; i < g_ed.proj->group_count; i++) {
        char key[16];
        snprintf(key, sizeof(key), "%u", g_ed.proj->groups[i].owner);
        cJSON *g = cJSON_AddObjectToObject(groups, key);
        cJSON *ids = cJSON_AddArrayToObject(g, "ids");
        for (size_t j = 0; j < g_ed.proj->groups[i].count; j++) cJSON_AddItemToArray(ids, cJSON_CreateNumber(g_ed.proj->groups[i].objects[j]));
    }
    if (g_ed.proj->selected) cJSON_AddNumberToObject(n, "selected", g_ed.proj->selected->uuid);
    cJSON *cam = cJSON_AddObjectToObject(n, "cam");
    cJSON *p = cJSON_AddArrayToObject(cam, "p");
    cJSON_AddItemToArray(p, cJSON_CreateNumber(g_ed.proj->camera.position.x));
    cJSON_AddItemToArray(p, cJSON_CreateNumber(g_ed.proj->camera.position.y));
    cJSON_AddItemToArray(p, cJSON_CreateNumber(g_ed.proj->camera.position.z));
    // controls.getRotation(): [pitch, yaw] (E10:4306, the same order in v7's save files)
    cJSON *r = cJSON_AddArrayToObject(cam, "r");
    cJSON_AddItemToArray(r, cJSON_CreateNumber(g_ed.proj->pitch));
    cJSON_AddItemToArray(r, cJSON_CreateNumber(g_ed.proj->yaw));
    cJSON_AddArrayToObject(n, "history");
    return n;
}

cJSON *ed_save_doc(void) { return save_doc(); }

void ed_map_store(void) {
    if (!g_ed.proj->map_config) return;
    cJSON *doc = save_doc();
    char *text = ed_model_print(doc);
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

// assets/editor/templates.json (tools/editor/gen_editor_data.py): {game, script, defaultMaps}
const cJSON *ed_templates_get(void) {
    static cJSON *templates;
    static int tried;
    if (!templates && !tried) {
        tried = 1;
        char *path = concat(assets_path(), "editor/templates.json");
        unsigned char *buf = NULL;
        size_t len = 0;
        if (path && ensure_asset_file(path) && !read_file(path, &buf, &len)) templates = cJSON_ParseWithLength((const char *) buf, len);
        free(buf);
        free(path);
    }
    return templates;
}

int ed_load_quick(const int index, const int script, const int default_map) {
    // window.loadQuick(i, script, defaultMap): import a template (its "data" map JSON string) or a
    // default map (maps/<fileName>.json, here assets/maps/<fileName>.json)
    const cJSON *t = ed_templates_get();
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(t, default_map ? "defaultMaps" : script ? "script" : "game");
    const cJSON *entry = cJSON_GetArrayItem(list, index);
    if (!entry) return 0;
    if (default_map) {
        const char *file = ed_json_str(entry, "fileName", NULL);
        if (!file) return 0;
        char rel[256];
        snprintf(rel, sizeof(rel), "maps/%s.json", file);
        char *path = concat(assets_path(), rel);
        unsigned char *buf = NULL;
        size_t len = 0;
        int ok = 0;
        if (path && ensure_asset_file(path) && !read_file(path, &buf, &len)) {
            char *text = malloc(len + 1);
            if (text) {
                memcpy(text, buf, len);
                text[len] = 0;
                ok = ed_map_import(text);
                free(text);
            }
        }
        free(buf);
        free(path);
        return ok;
    }
    const char *data = ed_json_str(entry, "data", "");
    if (!data[0]) return 0; // the empty script templates (2D Car Game, 3D Pong, Basic Collisions)
    editor_close_window();
    return ed_map_import(data);
}

void ed_quick_start(void) {
    // quickStart: loadQuick(0), the first game template ("Quickstart": a 100x10x100 floor and a
    // spawn). Without the template data, the same map is built here.
    if (ed_load_quick(0, 0, 0)) return;
    ed_map_clear();
    cJSON *floor = cJSON_Parse("{\"p\":[0,-10,0],\"s\":[100,10,100]}");
    ed_obj_add(ed_obj_new(floor, 0), 1, 1);
    cJSON_Delete(floor);
    cJSON *spawn = cJSON_Parse("{\"i\":5,\"p\":[0,0,0]}");
    ed_obj_add(ed_obj_new(spawn, 0), 1, 1);
    cJSON_Delete(spawn);
}

// ---------------------------------------------------------------------------
// KRUNKNATIVE_EDITOR_SCRIPT test driver (editor.c run_script_text): the File menu's actions, so
// tools/editor/io_e2e.py can run them without clicking. file:save (Save Project), file:export
// (Export), file:exportcopy (Shift+Export), file:import:PATH (a picked / dropped file),
// file:backup (a backup now), file:loadbackup:N (window 15's Nth entry, newest first),
// file:clearbackups (Clear All), file:new (New Project after its confirm), file:copy (Copy Export),
// file:drop:A|B (a drag and drop of several files), file:gltf / file:mtl (Export GLTF / MTL).
int ed_file_script(const char *cmd, const char *arg) {
    if (strcmp(cmd, "file") != 0) return 0;
    if (!arg) return 1;
    if (!strcmp(arg, "save")) ed_file_save_as();
    else if (!strcmp(arg, "export")) ed_file_export(0);
    else if (!strcmp(arg, "exportcopy")) ed_file_export(1);
    else if (!strcmp(arg, "copy")) ed_file_copy_export();
    else if (!strncmp(arg, "import:", 7)) ed_file_import_path(arg + 7);
    else if (!strcmp(arg, "backup")) ed_backup_do();
    else if (!strncmp(arg, "loadbackup:", 11)) {
        cJSON *list = ed_backup_list();
        const int i = atoi(arg + 11);
        const int n = cJSON_GetArraySize(list);
        // window 15 lists the newest first: entry i is the (n - 1 - i)th of the index
        if (i >= 0 && i < n) ed_backup_load(ed_json_num(cJSON_GetArrayItem(list, n - 1 - i), "timestamp", 0));
        cJSON_Delete(list);
    } else if (!strcmp(arg, "clearbackups")) ed_backup_clear();
    else if (!strncmp(arg, "drop:", 5)) {
        // drop:A|B: the paths of a drag and drop (ed_file_drop)
        char *list = strdup(arg + 5), *p = list;
        const char *paths[16];
        int n = 0;
        while (p && *p && n < 16) {
            paths[n++] = p;
            char *bar = strchr(p, '|');
            if (bar) *bar++ = 0;
            p = bar;
        }
        ed_file_drop(n, paths);
        free(list);
    } else if (!strcmp(arg, "new")) ed_map_clear();
    else if (!strcmp(arg, "gltf")) ed_file_export_gltf();     // Export GLTF (editor_export.c)
    else if (!strcmp(arg, "mtl")) ed_file_export_mtl();       // Export MTL (editor_export.c)
    return 1;
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

EM_JS(void, kn_editor_clipboard_write, (const char *text), {
    var t = UTF8ToString(text);
    window.__knEditorClip = t;
    try { navigator.clipboard.writeText(t); } catch (e) {}
});

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
    safe_name(ed_json_str(g_ed.proj->map_config, "name", "map"), name, sizeof(name));
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

void ed_file_export(const int copy) {
    // exportMap (E10:16661): download "<name>_export" (natively <name>_export.json in the user
    // maps directory, since a desktop file needs an extension); Shift+click (copy) also copies
    // the text to the clipboard (IDX:14455-14459)
    char *text = ed_map_export_string();
    if (!text) return;
    if (copy) ed_clipboard_copy(text);
#ifdef __EMSCRIPTEN__
    char name[160];
    snprintf(name, sizeof(name), "%s_export", ed_json_str(g_ed.proj->map_config, "name", "map"));
    kn_editor_download(name, text);
    editor_message(copy ? "Map exported (download) and copied to the clipboard." : "Map exported (download).");
#else
    char path[800], msg[900];
    if (write_user_file("_export.json", text, path, sizeof(path))) snprintf(msg, sizeof(msg), copy ? "Map exported to %s and copied to the clipboard." : "Map exported to %s.", path);
    else snprintf(msg, sizeof(msg), "Could not write %s.", path);
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

void ed_file_copy_export(void) {
    // Copy Export: copyToClipboard(getMapExport()) (E10:19802)
    char *text = ed_map_export_string();
    if (!text) return;
    ed_clipboard_copy(text);
    free(text);
    editor_message("Map copied to the clipboard.");
}

void ed_file_save_as(void) {
    // save(): "<name>_save.txt" with groups, selection and camera
    cJSON *doc = save_doc();
    char *text = ed_model_print(doc);
    cJSON_Delete(doc);
    if (!text) return;
#ifdef __EMSCRIPTEN__
    char name[160];
    snprintf(name, sizeof(name), "%s_save.txt", ed_json_str(g_ed.proj->map_config, "name", "map"));
    kn_editor_download(name, text);
#else
    char path[800], msg[900];
    if (write_user_file("_save.txt", text, path, sizeof(path))) snprintf(msg, sizeof(msg), "Saved %s.", path);
    else snprintf(msg, sizeof(msg), "Could not write %s.", path);
    editor_message(msg);
#endif
    free(text);
}

void ed_file_import_path(const char *path) {
    unsigned char *buf = NULL;
    size_t len = 0;
    if (!path || !*path) return;
    if (read_file(path, &buf, &len) || !buf) {
        char msg[1200];
        snprintf(msg, sizeof(msg), "Cannot read map file %s", path);
        editor_message(msg);
        return;
    }
    char *t = malloc(len + 1);
    if (t) {
        memcpy(t, buf, len);
        t[len] = 0;
        ed_map_import(t);
        free(t);
    }
    free(buf);
}

static void import_picked(void *user, const char *path) {
    (void) user;
    ed_file_import_path(path);
}

void ed_file_drop(const int count, const char **paths) {
    // drag and drop onto the editor (E10:15825: one FileReader per file, each importMap'd in turn,
    // so the last file shown is the last one dropped)
    for (int i = 0; i < count; i++) ed_file_import_path(paths[i]);
}

void ed_file_import(void) {
    // importMap with Shift (loadFile("importMap"), IDX:14435-14441): a picked file replaces the
    // shown map. Without Shift the paste popup opens (ed_import_map_popup).
    ed_pick_file(".txt,.json,text/plain,application/json", import_picked, NULL);
}

// download(blob, name): a binary file (the GLTF / MTL exports, editor_export.c). The web build
// saves a Blob of `mime`; natively the file goes to the user maps directory under that name.
#ifdef __EMSCRIPTEN__
EM_JS(void, kn_editor_download_bytes, (const char *name, const void *data, int size, const char *mime), {
    var blob = new Blob([HEAPU8.slice(data, data + size)], {type: UTF8ToString(mime)});
    var a = document.createElement("a");
    a.style.display = "none";
    a.href = URL.createObjectURL(blob);
    a.download = UTF8ToString(name);
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);
    setTimeout(function () { URL.revokeObjectURL(a.href); }, 1000);
});
#endif

void ed_download_bytes(const void *data, const size_t size, const char *name, const char *mime) {
    if (!data || !name) return;
#ifdef __EMSCRIPTEN__
    kn_editor_download_bytes(name, data, (int) size, mime ? mime : "application/octet-stream");
#else
    (void) mime;
    char dir[600], safe[128], path[800], msg[900];
    user_maps_dir(dir, sizeof(dir));
    size_t j = 0;
    for (size_t i = 0; name[i] && j + 1 < sizeof(safe); i++) {
        const char c = name[i];
        safe[j++] = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ? c : '_';
    }
    safe[j] = 0;
    if (!j || safe[0] == '.') snprintf(safe, sizeof(safe), "map%s", strrchr(name, '.') ? strrchr(name, '.') : "");
    snprintf(path, sizeof(path), "%s/%s", dir, safe);
    FILE *f = fopen(path, "wb");
    int ok = 0;
    if (f) {
        ok = fwrite(data, 1, size, f) == size;
        ok &= fclose(f) == 0;
    }
    snprintf(msg, sizeof(msg), ok ? "Saved %s." : "Could not write %s.", path);
    editor_message(msg);
#endif
}
