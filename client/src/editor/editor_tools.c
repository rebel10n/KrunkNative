// Map editor: the Tools and Prefabs right panels (Krunker v7.1.4 ui/editor.js panel.right.tools
// UI:6087-6200, which merges v3.1.1's Customize panel, and panel.right.prefab UI:6202-6257) and the
// editor.js methods behind their buttons: optimizeFaces, mergeColors, drunkMap, timelapseMap,
// scaleMap, reflectObjects, frameObject, breakableMap, colorizeMap, textToObjects, convert (image /
// voxel), layoutPlanner / replicate, the Object Painter's scraps, editGroup, jsonInput, plus
// replaceObject / createGroup / findCenter, which the Quick GUI, the inspector's group header and
// the hitbox fixer use too.
//
// Differences (intentional):
// - prompt()/confirm() are the in-page dialog of editor_prompt.c (asynchronous).
// - loadFile(): the web build opens a file picker like Krunker; natively a zenity /
//   kdialog dialog when installed, else the prompt asks for a file path.
// - download() (convert without Import, reflect): the web build downloads the file;
//   natively it is written to the user maps directory (editor_io.c) and copied.
// - Intentional differences from v7.1.4 (doc/v7/editor.md 1.13): Color Merging visits every object
//   once (v7 reuses the loop index), Frame's "Has Ceiling" edits frameCeiling (v7 binds the
//   thickness), Colorize > Gold colours the map gold (a no-op in v7), the Object Painter's div
//   follows the switch; Optimize Faces and Color Merging are one undo step each.
#include "editor_internal.h"
#include <ctype.h>
#include <math.h>
#include <stb_image.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

enum { PANEL_RIGHT = 1 };

// ---------------------------------------------------------------------------
// serialized-object arrays ({p, s, ...}): the helpers of editor.js that work on them

static double arr_num(const cJSON *o, const char *key, const int i) {
    const cJSON *v = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(o, key), i);
    return cJSON_IsNumber(v) ? v->valuedouble : 0.0; // `e.p[1] || 0`
}

static cJSON *arr_get(cJSON *o, const char *key) {
    cJSON *a = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!cJSON_IsArray(a)) {
        cJSON_DeleteItemFromObjectCaseSensitive(o, key);
        a = cJSON_AddArrayToObject(o, key);
    }
    while (cJSON_GetArraySize(a) < 3) cJSON_AddItemToArray(a, cJSON_CreateNumber(!strcmp(key, "s") ? 1 : 0));
    return a;
}

static void arr_set(cJSON *o, const char *key, const int i, const double v) {
    cJSON *a = arr_get(o, key);
    cJSON_ReplaceItemInArray(a, i, cJSON_CreateNumber(v));
}

// findCenter(e): [x, minY, z, width, height, depth] with x/z rounded
void ed_find_center(const cJSON *arr, double out[6]) {
    const cJSON *first = cJSON_GetArrayItem(arr, 0);
    double n = arr_num(first, "p", 1), i = arr_num(first, "p", 1) + arr_num(first, "s", 1);
    double r = arr_num(first, "p", 0) - arr_num(first, "s", 0) / 2, a = arr_num(first, "p", 0) + arr_num(first, "s", 0) / 2;
    double s = arr_num(first, "p", 2) - arr_num(first, "s", 2) / 2, o = arr_num(first, "p", 2) + arr_num(first, "s", 2) / 2;
    const cJSON *l;
    cJSON_ArrayForEach(l, arr) {
        const double p0 = arr_num(l, "p", 0), p1 = arr_num(l, "p", 1), p2 = arr_num(l, "p", 2);
        const double s0 = arr_num(l, "s", 0), s1 = arr_num(l, "s", 1), s2 = arr_num(l, "s", 2);
        if (p1 < n) n = p1;
        if (p1 + s1 > i) i = p1 + s1;
        if (p0 - s0 / 2 < r) r = p0 - s0 / 2;
        if (p0 + s0 / 2 > a) a = p0 + s0 / 2;
        if (p2 - s2 / 2 < s) s = p2 - s2 / 2;
        if (p2 + s2 / 2 > o) o = p2 + s2 / 2;
    }
    const double x = round((r + a) / 2), z = round((s + o) / 2);
    out[0] = x;
    out[1] = n;
    out[2] = z;
    out[3] = (a - x) * 2;
    out[4] = i - n;
    out[5] = (o - z) * 2;
}

static void apply_center(cJSON *arr) {
    if (!cJSON_GetArraySize(arr)) return;
    double c[6];
    ed_find_center(arr, c);
    cJSON *o;
    cJSON_ArrayForEach(o, arr) for (int k = 0; k < 3; k++) arr_set(o, "p", k, arr_num(o, "p", k) - c[k]);
}

static void change_angle(cJSON *arr) {
    cJSON *o;
    cJSON_ArrayForEach(o, arr) {
        const double s0 = arr_num(o, "s", 0), s2 = arr_num(o, "s", 2), p0 = arr_num(o, "p", 0), p2 = arr_num(o, "p", 2);
        arr_set(o, "s", 0, s2);
        arr_set(o, "s", 2, s0);
        arr_set(o, "p", 0, p2);
        arr_set(o, "p", 2, p0);
    }
}

static void reflect_angle(cJSON *arr) {
    cJSON *o;
    cJSON_ArrayForEach(o, arr) {
        arr_set(o, "p", 0, arr_num(o, "p", 0) * -1);
        arr_set(o, "p", 2, arr_num(o, "p", 2) * -1);
    }
}

static void rotate_objects(cJSON *arr, const int deg) {
    if (deg == 90) change_angle(arr);
    else if (deg == 180) reflect_angle(arr);
    else if (deg == 270) {
        change_angle(arr);
        reflect_angle(arr);
    }
}

// reflect(e, t, false) on an object array: mirror across the centre on axis t, ramp
// directions follow
void ed_reflect_array(cJSON *arr, const int t) {
    if (!cJSON_GetArraySize(arr)) return;
    double c[6];
    ed_find_center(arr, c);
    cJSON *o;
    cJSON_ArrayForEach(o, arr) {
        const double p = arr_num(o, "p", t);
        arr_set(o, "p", t, p + (c[t] - p) * 2);
        const cJSON *dv = cJSON_GetObjectItemCaseSensitive(o, "d");
        if (dv) {
            const int d = cJSON_IsNumber(dv) ? (int) dv->valuedouble : 0;
            const int null_d = !cJSON_IsNumber(dv);
            if (((t == 0 && (d == 0 || null_d || d == 2)) || (t == 2 && (d == 1 || d == 3))) && abs(t + 2 - d) != 0) {
                cJSON_DeleteItemFromObjectCaseSensitive(o, "d");
                cJSON_AddNumberToObject(o, "d", abs(t + 2 - d));
            }
        }
    }
}

// mergeObjects(e): joins touching boxes of equal cross-section and look along x, y, z
static int same_key(const cJSON *a, const cJSON *b, const char *key) {
    const cJSON *x = cJSON_GetObjectItemCaseSensitive(a, key), *y = cJSON_GetObjectItemCaseSensitive(b, key);
    if (!x || !y) return !x && !y;
    if (cJSON_IsNumber(x) && cJSON_IsNumber(y)) return x->valuedouble == y->valuedouble;
    if (cJSON_IsString(x) && cJSON_IsString(y)) return !strcmp(x->valuestring, y->valuestring);
    return cJSON_Compare(x, y, 1);
}

void ed_merge_objects(cJSON *e) {
    if (cJSON_GetArraySize(e) < 2) return;
    static const char *keys[] = {"c", "e", "o", "t", "l", "pe", "hp", "v", "a"};
    int t = 0;
    for (int n = 0; n < 3; n++) {
        const int i = (n + 1) % 3, r = (n + 2) % 3;
        for (int a = 0; a < cJSON_GetArraySize(e) - 1; a++) {
            for (int s = a + 1; s < cJSON_GetArraySize(e); s++) {
                cJSON *ea = cJSON_GetArrayItem(e, a), *es = cJSON_GetArrayItem(e, s);
                const double o = n % 2 ? arr_num(ea, "p", n) + arr_num(ea, "s", n) / 2 : arr_num(ea, "p", n);
                const double l = n % 2 ? arr_num(es, "p", n) + arr_num(es, "s", n) / 2 : arr_num(es, "p", n);
                int same = arr_num(es, "s", i) == arr_num(ea, "s", i) && arr_num(es, "s", r) == arr_num(ea, "s", r) &&
                           arr_num(es, "p", i) == arr_num(ea, "p", i) && arr_num(es, "p", r) == arr_num(ea, "p", r) &&
                           fabs(l - o) <= fabs(arr_num(es, "s", n) / 2 + arr_num(ea, "s", n) / 2);
                for (size_t k = 0; same && k < sizeof(keys) / sizeof(keys[0]); k++) same = same_key(es, ea, keys[k]);
                if (!same) continue;
                const double size = fabs(l - o) + fabs(arr_num(es, "s", n) / 2 + arr_num(ea, "s", n) / 2);
                double pos = (l + (t + 1) * o) / (t + 2);
                if (n == 1) pos = fmin(arr_num(ea, "p", n), arr_num(es, "p", n));
                arr_set(ea, "p", n, pos);
                arr_set(ea, "s", n, size);
                cJSON_DeleteItemFromArray(e, s);
                t++;
                s--;
            }
            t = 0;
        }
    }
}

// ---------------------------------------------------------------------------
// replaceObject / createGroup

static EditorObject *selected_group(void) {
    // objectSelected(true): the selection when it is a group placeholder
    return g_ed.proj->selected && ed_group_of(g_ed.proj->selected) ? g_ed.proj->selected : NULL;
}

static EditorGroup *group_add(EditorObject *owner, EditorObject **members, const size_t count) {
    EditorGroup *groups = realloc(g_ed.proj->groups, (g_ed.proj->group_count + 1) * sizeof(EditorGroup));
    if (!groups) return NULL;
    g_ed.proj->groups = groups;
    EditorGroup *g = &g_ed.proj->groups[g_ed.proj->group_count++];
    memset(g, 0, sizeof(*g));
    g->owner = owner->uuid;
    g->objects = count ? calloc(count, sizeof(uint32_t)) : NULL;
    for (size_t i = 0; i < count && g->objects; i++) g->objects[g->count++] = members[i]->uuid;
    g->pos = ed_obj_vec(owner, "p", (vec3) {0});
    g->scale = ed_obj_vec(owner, "s", (vec3) {1, 1, 1});
    g->rot_y = ed_obj_vec(owner, "r", (vec3) {0}).y;
    return g;
}

// createGroup(e, t, n, i): a placeholder around the members (createBoundingBox) replaces e
void ed_create_group(EditorObject *replaced, EditorObject **members, const size_t count, const double rot_y, const int remove) {
    if (count < 1) {
        editor_message("There is nothing to group");
        return;
    }
    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < count; i++) cJSON_AddItemToArray(arr, ed_obj_serialize(members[i]));
    double c[6];
    ed_find_center(arr, c);
    cJSON_Delete(arr);
    if (remove && replaced) ed_obj_remove(replaced, 0, 1);
    cJSON *d = cJSON_CreateObject();
    cJSON_AddNumberToObject(d, "i", ED_PREFAB_PLACEHOLDER);
    cJSON *p = cJSON_AddArrayToObject(d, "p");
    cJSON_AddItemToArray(p, cJSON_CreateNumber(c[0]));
    cJSON_AddItemToArray(p, cJSON_CreateNumber(c[1] - 1));
    cJSON_AddItemToArray(p, cJSON_CreateNumber(c[2]));
    cJSON *s = cJSON_AddArrayToObject(d, "s");
    cJSON_AddItemToArray(s, cJSON_CreateNumber(c[3] + 1));
    cJSON_AddItemToArray(s, cJSON_CreateNumber(c[4] + 2));
    cJSON_AddItemToArray(s, cJSON_CreateNumber(c[5] + 1));
    if (rot_y != 0) {
        cJSON *r = cJSON_AddArrayToObject(d, "r");
        cJSON_AddItemToArray(r, cJSON_CreateNumber(0));
        cJSON_AddItemToArray(r, cJSON_CreateNumber(rot_y));
        cJSON_AddItemToArray(r, cJSON_CreateNumber(0));
    }
    EditorObject *owner = ed_obj_new(d, 0);
    cJSON_Delete(d);
    if (!owner) return;
    ed_obj_add(owner, 1, 1);
    group_add(owner, members, count);
    ed_mark_changed();
}

static int str_in(const char *s, const char *const *list, const size_t n) {
    for (size_t i = 0; s && i < n; i++) if (!strcmp(s, list[i])) return 1;
    return 0;
}

// replaceObject(e, t, n, i, r): put the objects of JSON t in place of e (i = the replaced
// prefab type for the hitbox fixer, r = group them under a placeholder)
// replaceObject(e, json, i, t, n); fake = e is fakeSelected(): never removed, not in the scene
static void replace_objects(EditorObject *e, const char *text, const char *type, const int group, const int fake) {
    if (!e) {
        editor_message("You must select a object first");
        return;
    }
    cJSON *root = cJSON_Parse(text ? text : "");
    if (!root) {
        editor_message("Invalid Prefab");
        return;
    }
    cJSON *a = root;
    if (cJSON_IsObject(root)) {
        cJSON *objects = cJSON_GetObjectItemCaseSensitive(root, "objects");
        if (objects) a = objects;
        else if (cJSON_GetObjectItemCaseSensitive(root, "states") || cJSON_GetObjectItemCaseSensitive(root, "id"))
            a = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(root, "map"), "objects");
    }
    if (!cJSON_IsArray(a) || !cJSON_GetArraySize(a)) {
        cJSON_Delete(root);
        return;
    }
    const vec3 ep = ed_obj_vec(e, "p", (vec3) {0});
    const vec3 es = ed_obj_vec(e, "s", (vec3) {1, 1, 1});
    const vec3 er = ed_obj_vec(e, "r", (vec3) {0});
    if (!type && !group && !fake) ed_obj_remove(e, 0, 1);
    double deg = (int) ed_json_num(g_ed.advanced, "rotation", 0); // parseInt(advancedOptions.rotation)
    static const char *const keep_rot[] = {"VEHICLE", "CONTAINER", "CONTAINERR", "TREE"};
    if (type && str_in(type, keep_rot, 4)) {
        deg = 360 - er.y * 180.0 / M_PI; // not an integer
        g_ed.dirty_panel = 1; // objConfig.visible = false; refresh (the panel re-reads the object)
    }
    if (deg == 90 || deg == 180 || deg == 270) rotate_objects(a, deg);
    double c[6];
    ed_find_center(a, c);
    const size_t n = (size_t) cJSON_GetArraySize(a);
    EditorObject **added = calloc(n, sizeof(EditorObject *));
    size_t count = 0;
    // the bounding mesh of e: its centre is the rotation pivot
    const double cx = ep.x, cz = ep.z;
    const double th = deg * M_PI / 180.0;
    cJSON *t;
    cJSON_ArrayForEach(t, a) {
        arr_set(t, "p", 0, arr_num(t, "p", 0) + ep.x - c[0]);
        // owner.position (the box centre, p + s.y / 2) - boundingMesh.scale.y / 2 == p.y
        arr_set(t, "p", 1, arr_num(t, "p", 1) + (ep.y + es.y / 2) - es.y / 2 - c[1] - (type && !strcmp(type, "RAMP") ? 2 : 0));
        arr_set(t, "p", 2, arr_num(t, "p", 2) + ep.z - c[2] - (type && !strcmp(type, "VEHICLE") ? 0.5 : 0));
        EditorObject *o = ed_obj_new(t, 0);
        if (!o) continue;
        if (deg > 0 && deg != 90 && deg != 180 && deg != 270) {
            // rotateAroundPoint(boundingMesh, e.position, Y, rad): Vector3.applyAxisAngle + rotateOnWorldAxis
            vec3 p = ed_obj_vec(o, "p", (vec3) {0});
            const double x = p.x - cx, z = p.z - cz;
            p.x = (float) (cx + x * cos(th) + z * sin(th));
            p.z = (float) (cz - x * sin(th) + z * cos(th));
            ed_obj_set_vec(o, "p", p);
            vec3 r = ed_obj_vec(o, "r", (vec3) {0});
            // rotateOnWorldAxis then Euler.setFromQuaternion: Y in (-PI, PI]
            r.y = (float) atan2(sin(r.y + th), cos(r.y + th));
            ed_obj_set_vec(o, "r", r);
            ed_obj_rebuild(o);
        }
        if (group && added) added[count++] = o;
        ed_obj_add(o, 1, 1);
    }
    if (group) ed_create_group(fake ? NULL : e, added, count, deg == 90 || deg == 180 || deg == 270 ? 0 : th, !fake);
    if (!group) ed_obj_select(NULL); // hideTransform (n is always true here)
    free(added);
    cJSON_Delete(root);
    ed_json_set_num(g_ed.advanced, "rotation", 0);
    if (!strcmp(ed_panel_right_active(), "prefab")) ed_panel_right_show("prefab"); // the Rotation slider back to 0
    ed_mark_changed();
}

void ed_obj_replace(EditorObject *e, const char *text, const char *type, const int group) { replace_objects(e, text, type, group, 0); }

void ed_obj_replace_at(const vec3 pos, const char *text, const int group) {
    // replaceObject(fakeSelected(x, y, z), json, true, false, group): the fake's position is the
    // point, its scale 1, so the objects land at y - 0.5 (owner.position.y - scale.y / 2)
    cJSON *d = cJSON_CreateObject();
    const double p[3] = {pos.x, pos.y - 0.5, pos.z}, s[3] = {1, 1, 1};
    cJSON_AddItemToObject(d, "p", cJSON_CreateDoubleArray(p, 3));
    cJSON_AddItemToObject(d, "s", cJSON_CreateDoubleArray(s, 3));
    EditorObject *fake = ed_obj_new(d, 0);
    cJSON_Delete(d);
    if (!fake) return;
    replace_objects(fake, text, "", group, 1);
    ed_obj_free(fake);
}

// ---------------------------------------------------------------------------
// loadFile(): pick a file, then call back with its path

typedef struct {
    EdFileFn fn;
    void *user;
} PickReq;

static PickReq g_pick;

#ifdef __EMSCRIPTEN__
EM_JS(void, kn_ed_pick_open, (const char *accept), {
    Module.knEdPick = null;
    var input = document.createElement('input');
    input.type = 'file';
    input.id = 'jsonInput';
    input.accept = UTF8ToString(accept);
    input.addEventListener('change', function() {
        if (!input.files || input.files.length != 1) { Module.knEdPick = '!'; return; }
        var file = input.files[0], reader = new FileReader();
        reader.onload = function() {
            try { FS.mkdir('/tmp'); } catch (e) {}
            var path = '/tmp/kn_editor_' + Date.now() + '_' + file.name.replace(/[^A-Za-z0-9._-]/g, '_');
            FS.writeFile(path, new Uint8Array(reader.result));
            Module.knEdPick = path;
        };
        reader.readAsArrayBuffer(file);
    }, false);
    input.click();
});

EM_JS(char *, kn_ed_pick_take, (void), {
    var p = Module.knEdPick;
    if (!p) return 0;
    Module.knEdPick = null;
    return stringToNewUTF8(p);
});

static double g_pick_until;

static void pick_poll(void *user) {
    (void) user;
    char *path = kn_ed_pick_take();
    if (path) {
        PickReq r = g_pick;
        g_pick.fn = NULL;
        if (!strcmp(path, "!")) editor_message("Please select 1 file");
        else if (r.fn && g_ed.active) r.fn(r.user, path);
        free(path);
        return;
    }
    if (g_ed.active && g_pick.fn && glfwGetTime() < g_pick_until) html_timer_add(200, pick_poll, NULL);
}
#else
static void pick_prompted(void *user, const char *value) {
    (void) user;
    PickReq r = g_pick;
    g_pick.fn = NULL;
    if (value && *value && r.fn) r.fn(r.user, value);
}
#endif

void ed_pick_file(const char *accept, const EdFileFn fn, void *user) {
    g_pick.fn = fn;
    g_pick.user = user;
#ifdef __EMSCRIPTEN__
    kn_ed_pick_open(accept ? accept : "");
    g_pick_until = glfwGetTime() + 300.0;
    html_timer_add(200, pick_poll, NULL);
#else
    (void) accept;
    static const char *commands[] = {"zenity --file-selection 2>/dev/null", "kdialog --getopenfilename . 2>/dev/null"};
    for (size_t c = 0; c < sizeof(commands) / sizeof(commands[0]); c++) {
        FILE *pipe = popen(commands[c], "r");
        if (!pipe) continue;
        char line[1024] = "";
        const int got = fgets(line, sizeof(line), pipe) != NULL;
        const int status = pclose(pipe);
        line[strcspn(line, "\r\n")] = 0;
        if (got && *line) {
            g_pick.fn = NULL;
            fn(user, line);
            return;
        }
        if (status == 0 || status == 256) { // the dialog ran and was cancelled
            g_pick.fn = NULL;
            return;
        }
    }
    ed_prompt("Path of the file to load", "", pick_prompted, NULL);
#endif
}

static char *read_text_file(const char *path) {
    unsigned char *buf = NULL;
    size_t len = 0;
    if (read_file(path, &buf, &len)) return NULL;
    char *text = malloc(len + 1);
    if (text) {
        memcpy(text, buf, len);
        text[len] = 0;
    }
    free(buf);
    return text;
}

// download(text, name): web download; natively the user maps dir + the clipboard
void ed_download(const char *text, const char *name);

// ---------------------------------------------------------------------------
// GUI helpers (the panel.right blueprints)

static EdCtl *with(EdBuild *b, EdCtl *c, cJSON *obj, const char *key, const EdChangeFn fn) {
    ed_ctl_bind_json(c, obj, key);
    c->on_change = fn;
    ed_gui_emit_value(b, c);
    return c;
}

static void settings_changed(EdCtl *c, const cJSON *v) {
    (void) c; (void) v;
    editor_settings_save(); // setSettings(key, value)
}

static void apply_object_change(EditorObject *o) {
    ed_obj_rebuild(o);
    ed_mark_changed();
}

static void set_prop_num(EditorObject *o, const char *prop, const double v) {
    cJSON *n = cJSON_CreateNumber(v);
    ed_prop_set(o, prop, n);
    cJSON_Delete(n);
}

static void set_prop_str(EditorObject *o, const char *prop, const char *v) {
    cJSON *n = cJSON_CreateString(v);
    ed_prop_set(o, prop, n);
    cJSON_Delete(n);
}

static void refresh_counts(void) {
    for (int i = 0; i < 3; i++) g_ed.proj->obj_count[i] = 0;
    for (size_t i = 0; i < g_ed.proj->count; i++) g_ed.proj->obj_count[ed_obj_count_class(g_ed.proj->objects[i])]++;
    g_ed.dirty_counts = 1;
}

// ---------------------------------------------------------------------------
// Tools: Customize-era actions (v3.1.1 Customize panel, now in Tools)

static void do_drunk(void *user, const char *ok) {
    (void) user;
    if (!ok) return;
    // drunkMap: every object gets a random texture movement
    for (size_t i = 0; i < g_ed.proj->count; i++) {
        EditorObject *o = g_ed.proj->objects[i];
        set_prop_num(o, "movingSpeed", (double) (rand() % 41 - 20)); // UTILS.getRandom([-20, 20])
        set_prop_num(o, "movingAxis", (double) (rand() % 2));
        apply_object_change(o);
    }
}

static void act_drunk(EdCtl *c) {
    (void) c;
    ed_confirm("Are you sure you want to make the whole map move?", do_drunk, NULL);
}

// timelapseMap: export, clear, then add the objects back one every timelapseMS
static cJSON *g_lapse;
static int g_lapse_index, g_lapse_follow, g_lapse_ms;
static unsigned g_lapse_timer;

static void lapse_step(void *user) {
    (void) user;
    g_lapse_timer = 0;
    if (!g_ed.active || !g_lapse) return;
    const cJSON *d = cJSON_GetArrayItem(g_lapse, g_lapse_index++);
    if (!d) {
        cJSON_Delete(g_lapse);
        g_lapse = NULL;
        return;
    }
    EditorObject *o = ed_obj_new(d, 0);
    if (o) {
        ed_obj_add(o, 1, 1);
        if (g_lapse_follow) editor_frame_object(o);
    }
    g_lapse_timer = html_timer_add((unsigned) g_lapse_ms, lapse_step, NULL);
}

static void act_timelapse(EdCtl *c) {
    (void) c;
    char *text = ed_map_export_string();
    cJSON *map = text ? cJSON_Parse(text) : NULL;
    free(text);
    if (!map) return;
    cJSON *objects = cJSON_DetachItemFromObjectCaseSensitive(map, "objects");
    // colour indexes back to colours (loadObjects)
    const cJSON *colors = cJSON_GetObjectItemCaseSensitive(map, "colors");
    cJSON *o;
    cJSON_ArrayForEach(o, objects) {
        static const char *const keys[] = {"c", "e"};
        for (int k = 0; k < 2; k++) {
            char ik[4];
            snprintf(ik, sizeof(ik), "%si", keys[k]);
            const cJSON *idx = cJSON_GetObjectItemCaseSensitive(o, ik);
            const cJSON *col = cJSON_IsNumber(idx) ? cJSON_GetArrayItem(colors, (int) idx->valuedouble) : NULL;
            if (col) {
                cJSON_DeleteItemFromObjectCaseSensitive(o, keys[k]);
                cJSON_AddItemToObject(o, keys[k], cJSON_Duplicate(col, 1));
                cJSON_DeleteItemFromObjectCaseSensitive(o, ik);
            }
        }
    }
    char *rest = cJSON_PrintUnformatted(map);
    cJSON_Delete(map);
    if (g_lapse_timer) html_timer_cancel(g_lapse_timer);
    cJSON_Delete(g_lapse);
    g_lapse = objects;
    g_lapse_index = 0;
    g_lapse_ms = (int) ed_json_num(g_ed.advanced, "timelapseMS", 500);
    g_lapse_follow = ed_json_truthy(cJSON_GetObjectItemCaseSensitive(g_ed.advanced, "timelapseFollow"));
    if (rest) ed_map_import(rest); // clearMap + the map config, spawns and camera
    free(rest);
    lapse_step(NULL);
}

static void act_scale_map(EdCtl *c) {
    (void) c;
    // scaleMap: positions and sizes times scaleMap[XYZ] (size only where the setter takes it)
    const double k[3] = {ed_json_num(g_ed.advanced, "scaleMapX", 1), ed_json_num(g_ed.advanced, "scaleMapY", 1), ed_json_num(g_ed.advanced, "scaleMapZ", 1)};
    for (size_t i = 0; i < g_ed.proj->count; i++) {
        EditorObject *o = g_ed.proj->objects[i];
        const vec3 p = ed_obj_vec(o, "p", (vec3) {0});
        cJSON *size = ed_prop_get(o, "size");
        cJSON *ns = cJSON_CreateArray();
        for (int a = 0; a < 3; a++) cJSON_AddItemToArray(ns, cJSON_CreateNumber(cJSON_GetNumberValue(cJSON_GetArrayItem(size, a)) * k[a]));
        ed_prop_set(o, "size", ns);
        cJSON_Delete(ns);
        cJSON_Delete(size);
        ed_obj_set_vec(o, "p", (vec3) {(float) (p.x * k[0]), (float) (p.y * k[1]), (float) (p.z * k[2])});
        apply_object_change(o);
    }
    g_ed.dirty_panel = 1;
}

// reflectObjects(group): mirror the map (or the selected group) across its centre
static void reflect_objects(const int group) {
    EditorObject **list = g_ed.proj->objects;
    size_t count = g_ed.proj->count;
    EditorObject **members = NULL;
    int dir;
    if (group) {
        dir = (int) ed_json_num(g_ed.advanced, "groupReflectDir", 0);
        EditorObject *r = selected_group();
        if (!r) {
            editor_message("You cant reflect a group that doesnt exist");
            return;
        }
        const EditorGroup *g = ed_group_of(r);
        members = calloc(g->count ? g->count : 1, sizeof(EditorObject *));
        count = 0;
        for (size_t i = 0; members && i < g->count; i++) {
            EditorObject *m = ed_obj_find(g->objects[i]);
            if (m) members[count++] = m;
        }
        list = members;
    } else {
        dir = (int) ed_json_num(g_ed.advanced, "reflectDir", 0);
    }
    if (count && list) {
        // findCenter(t, true) on the ObjectInstances: their position is the box centre
        // (p + s.y / 2) and their scale the size only for scaleWithSize models, else 1
        cJSON *arr = cJSON_CreateArray();
        for (size_t i = 0; i < count; i++) {
            cJSON *d = cJSON_CreateObject();
            const vec3 p = ed_obj_vec(list[i], "p", (vec3) {0});
            cJSON *sz = ed_prop_get(list[i], "size");
            const double sy = cJSON_GetNumberValue(cJSON_GetArrayItem(sz, 1));
            cJSON *pa = cJSON_AddArrayToObject(d, "p");
            cJSON_AddItemToArray(pa, cJSON_CreateNumber(p.x));
            cJSON_AddItemToArray(pa, cJSON_CreateNumber(p.y + sy / 2));
            cJSON_AddItemToArray(pa, cJSON_CreateNumber(p.z));
            if (editor_prefab_has(list[i]->prefab, EP_SCALEWITHSIZE)) cJSON_AddItemToObject(d, "s", sz);
            else {
                cJSON_Delete(sz);
                cJSON *one = cJSON_AddArrayToObject(d, "s");
                for (int k = 0; k < 3; k++) cJSON_AddItemToArray(one, cJSON_CreateNumber(1));
            }
            cJSON_AddItemToArray(arr, d);
        }
        double c[6];
        ed_find_center(arr, c);
        cJSON_Delete(arr);
        for (size_t i = 0; i < count; i++) {
            EditorObject *o = list[i];
            vec3 p = ed_obj_vec(o, "p", (vec3) {0});
            float *axis = dir == 0 ? &p.x : dir == 1 ? &p.y : &p.z;
            *axis += (float) ((c[dir] - *axis) * 2);
            ed_obj_set_vec(o, "p", p);
            if (editor_prefab_has(o->prefab, EP_CUSTOMDIRECTION)) {
                cJSON *dv = ed_prop_get(o, "direction");
                const int d = (int) cJSON_GetNumberValue(dv);
                cJSON_Delete(dv);
                if ((dir == 0 && (d == 0 || d == 2)) || (dir == 2 && (d == 1 || d == 3))) set_prop_num(o, "direction", abs(dir + 2 - d));
            }
            // v7: the faces of the reflected axis swap ([faces[2d], faces[2d + 1]])
            cJSON *fa = ed_prop_get(o, "faces");
            if (cJSON_GetArraySize(fa) >= 6) {
                cJSON *lo = cJSON_DetachItemFromArray(fa, dir * 2 + 1);
                cJSON_InsertItemInArray(fa, dir * 2, lo);
                ed_prop_set(o, "faces", fa);
            }
            cJSON_Delete(fa);
            apply_object_change(o);
        }
    }
    free(members);
    if (group) {
        ed_json_set_num(g_ed.advanced, "groupReflectDir", 0);
        g_ed.dirty_panel = 1; // refresh("panel_right_object")
    } else {
        // v7 refreshes the Customize panel it no longer has; the Tools panel shows the reset select
        ed_json_set_num(g_ed.advanced, "reflectDir", 0);
        if (!strcmp(ed_panel_right_active(), "tools")) ed_panel_right_show("tools");
    }
}

static void act_reflect_map(EdCtl *c) { (void) c; reflect_objects(0); }

static void add_box(const double px, const double py, const double pz, const double sx, const double sy, const double sz) {
    cJSON *d = cJSON_CreateObject();
    cJSON *p = cJSON_AddArrayToObject(d, "p"), *s = cJSON_AddArrayToObject(d, "s");
    cJSON_AddItemToArray(p, cJSON_CreateNumber(px));
    cJSON_AddItemToArray(p, cJSON_CreateNumber(py));
    cJSON_AddItemToArray(p, cJSON_CreateNumber(pz));
    cJSON_AddItemToArray(s, cJSON_CreateNumber(sx));
    cJSON_AddItemToArray(s, cJSON_CreateNumber(sy));
    cJSON_AddItemToArray(s, cJSON_CreateNumber(sz));
    EditorObject *o = ed_obj_new(d, 0);
    cJSON_Delete(d);
    if (o) ed_obj_add(o, 1, 1);
}

static void act_frame(EdCtl *c) {
    (void) c;
    // frameObject: four walls (+ ceiling / floor) around the selected box
    EditorObject *e = g_ed.proj->selected;
    if (!e) {
        editor_message("Please Select a object");
        return;
    }
    const double t = ed_json_num(g_ed.advanced, "frameThickness", 10);
    const int ceiling = ed_json_truthy(cJSON_GetObjectItemCaseSensitive(g_ed.advanced, "frameCeiling"));
    const int floor_ = ed_json_truthy(cJSON_GetObjectItemCaseSensitive(g_ed.advanced, "frameFloor"));
    if (t < 1) {
        editor_message("Wall Thickness must be 1 or greator");
        return;
    }
    // e is the bounding mesh: its position is the box centre, its scale the size
    const vec3 p = ed_obj_vec(e, "p", (vec3) {0});
    cJSON *sz = ed_prop_get(e, "size");
    const vec3 a = {(float) cJSON_GetNumberValue(cJSON_GetArrayItem(sz, 0)), (float) cJSON_GetNumberValue(cJSON_GetArrayItem(sz, 1)), (float) cJSON_GetNumberValue(cJSON_GetArrayItem(sz, 2))};
    cJSON_Delete(sz);
    const vec3 r = p; // boundingMesh.position: the bottom centre
    add_box(r.x, r.y, r.z - a.z / 2 - t / 2, a.x + t * 2, a.y, t);
    add_box(r.x, r.y, r.z + a.z / 2 + t / 2, a.x + t * 2, a.y, t);
    add_box(r.x - a.x / 2 - t / 2, r.y, r.z, t, a.y, a.z);
    add_box(r.x + a.x / 2 + t / 2, r.y, r.z, t, a.y, a.z);
    if (ceiling) add_box(r.x, r.y + a.y, r.z, a.x + t * 2, t, a.z + t * 2);
    if (floor_) add_box(r.x, r.y - t, r.z, a.x + t * 2, t, a.z + t * 2);
    ed_json_set_num(g_ed.advanced, "frameThickness", 0);
    ed_json_set_bool(g_ed.advanced, "frameCeiling", 0);
    ed_json_set_bool(g_ed.advanced, "frameFloor", 0);
    if (!strcmp(ed_panel_right_active(), "tools")) ed_panel_right_show("tools"); // v7: refresh("panel_right_customize")
}

static void do_breakable(void *user, const char *ok) {
    (void) user;
    if (!ok) return;
    const double hp = ed_json_num(g_ed.advanced, "breakableHealth", 1);
    const int col = ed_json_truthy(cJSON_GetObjectItemCaseSensitive(g_ed.advanced, "breakableCollision"));
    for (size_t i = 0; i < g_ed.proj->count; i++) {
        EditorObject *o = g_ed.proj->objects[i];
        set_prop_num(o, "health", hp);
        if (col) {
            cJSON *t = cJSON_CreateTrue();
            ed_prop_set(o, "collidable", t);
            cJSON_Delete(t);
        }
        apply_object_change(o);
    }
    refresh_counts();
}

static void act_breakable(EdCtl *c) {
    (void) c;
    ed_confirm("Are you sure you want to make the whole map breakable?", do_breakable, NULL);
}

// colorizeMap(e, t, n): e = colour list, n = random; t (Gold) is never used by v3.1.1
static void colorize(const char *list, const int random) {
    char *colors[256];
    int n = 0;
    char *copy = NULL;
    if (list) {
        copy = strdup(list);
        char *s = copy;
        while (*s && isspace((unsigned char) *s)) s++;
        char *end = s + strlen(s);
        while (end > s && isspace((unsigned char) end[-1])) *--end = 0;
        for (char *tok = s; tok && n < 256;) {
            char *comma = strchr(tok, ',');
            if (comma) *comma = 0;
            colors[n++] = tok;
            tok = comma ? comma + 1 : NULL;
        }
    }
    for (size_t i = 0; i < g_ed.proj->count; i++) {
        EditorObject *o = g_ed.proj->objects[i];
        if (n) set_prop_str(o, "color", n > 1 ? colors[rand() % n] : colors[0]);
        if (random) {
            char hex[16];
            snprintf(hex, sizeof(hex), "#%06x", (unsigned) (((unsigned) rand() << 12) ^ (unsigned) rand()) & 0xffffffu);
            set_prop_str(o, "color", hex);
        }
        if (n || random) apply_object_change(o);
    }
    free(copy);
}

static void act_colorize_random(EdCtl *c) { (void) c; colorize(NULL, 1); }
// Colorize > Gold: colorizeMap(false, true) is a no-op in v7.1.4 (the gold flag is never read).
// Intentional difference from v7.1.4 (doc/v7/editor.md 1.13): every object becomes gold, colour
// #FFD700 with a dark gold emissive #4D4100 (30 %), so the button does what it says.
static void act_colorize_gold(EdCtl *c) {
    (void) c;
    ed_history_poll();
    const uint32_t batch = ed_history_batch_begin("colorize gold");
    for (size_t i = 0; i < g_ed.proj->count; i++) {
        EditorObject *o = g_ed.proj->objects[i];
        set_prop_str(o, "color", "#FFD700");
        set_prop_str(o, "emissive", "#4D4100");
        apply_object_change(o);
    }
    ed_history_poll();
    ed_history_batch_end(batch);
}

static void colorize_input(void *user, const char *value) {
    (void) user;
    if (value && *value) colorize(value, 0);
}

static void act_colorize_input(EdCtl *c) {
    (void) c;
    ed_prompt("Input colors. (Seperate using a comma)", "", colorize_input, NULL);
}

// vector3 proxies: Transform "Scale" = scaleMap[XYZ], Tools "Cubes/Space in XYZ" = planner[XYZ]Dir/Space
static cJSON *g_proxy;

static void proxy_load(const char *name, const char *fmt) {
    if (!g_proxy) g_proxy = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    static const char *const xyz[3] = {"X", "Y", "Z"};
    for (int i = 0; i < 3; i++) {
        char key[48];
        snprintf(key, sizeof(key), fmt, xyz[i]);
        cJSON_AddItemToArray(arr, cJSON_CreateNumber(ed_json_num(g_ed.advanced, key, 0)));
    }
    cJSON_DeleteItemFromObjectCaseSensitive(g_proxy, name);
    cJSON_AddItemToObject(g_proxy, name, arr);
}

static void proxy_store(EdCtl *c, const cJSON *v) {
    const char *fmt = !strcmp(c->key, "scaleMap") ? "scaleMap%s" : !strcmp(c->key, "Dir") ? "planner%sDir" : "planner%sSpace";
    static const char *const xyz[3] = {"X", "Y", "Z"};
    for (int i = 0; i < 3; i++) {
        char key[48];
        snprintf(key, sizeof(key), fmt, xyz[i]);
        ed_json_set_num(g_ed.advanced, key, cJSON_GetNumberValue(cJSON_GetArrayItem(v, i)));
    }
}

// ---------------------------------------------------------------------------
// Tools (Component > Tools)

// textToObjects: Krunker's block font (a-z), 9 units per letter, "." = new line
static const char k_glyphs[] =
#include "editor_glyphs.inc"
    ;

static void text_input(void *user, const char *value) {
    (void) user;
    if (!value || !*value) {
        editor_message("Please input proper text");
        return;
    }
    cJSON *font = cJSON_Parse(k_glyphs);
    cJSON *out = cJSON_CreateArray();
    double n = 0, i = 0;
    for (const char *ch = value; *ch; ch++) {
        const char a = (char) tolower((unsigned char) *ch);
        if (a == ' ') n += 5;
        if (a == '.') {
            n = 0;
            i -= 11;
        }
        const char key[2] = {a, 0};
        const cJSON *glyph = cJSON_GetObjectItemCaseSensitive(font, key);
        if (!glyph) continue;
        const cJSON *g;
        cJSON_ArrayForEach(g, glyph) {
            cJSON *t = cJSON_Duplicate(g, 1);
            arr_set(t, "p", 0, arr_num(t, "p", 0) + n);
            arr_set(t, "p", 1, arr_num(t, "p", 1) + i);
            cJSON_AddItemToArray(out, t);
        }
        n += 9;
    }
    cJSON_Delete(font);
    char *text = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    ed_obj_replace(g_ed.proj->selected, text, NULL, 1);
    free(text);
}

static void act_text_gen(EdCtl *c) {
    (void) c;
    ed_prompt("Input text", "", text_input, NULL);
}

static const char *k_blank_map = "{\"name\":\"New Krunker Map\",\"modURL\":\"\",\"ambient\":9937064,\"light\":15923452,\"sky\":14477549,"
                                 "\"fog\":9280160,\"fogD\":900,\"camPos\":[0,0,0],\"spawns\":[],\"objects\":[]}";

static void deliver(cJSON *objects, const int replace, const char *map_name, const char *file) {
    // convert*(…, n): replace the selection, else download the map
    if (replace) {
        char *text = cJSON_PrintUnformatted(objects);
        ed_obj_replace(g_ed.proj->selected, text, NULL, 1);
        free(text);
        cJSON_Delete(objects);
        return;
    }
    cJSON *map = cJSON_Parse(k_blank_map);
    if (map_name) ed_json_set_str(map, "name", map_name);
    cJSON_DeleteItemFromObjectCaseSensitive(map, "objects");
    cJSON_AddItemToObject(map, "objects", objects);
    char *text = cJSON_PrintUnformatted(map);
    cJSON_Delete(map);
    if (text) ed_download(text, file);
    free(text);
}

static void convert_image(void *user, const char *path) {
    // convertImage(e, img, n): one box per opaque pixel, merged and centred
    const int replace = (int) (intptr_t) user;
    if (replace && !g_ed.proj->selected) {
        editor_message("Select a object to replace first");
        return;
    }
    unsigned char *file = NULL;
    size_t len = 0;
    if (read_file(path, &file, &len)) return;
    int w = 0, h = 0, comp = 0;
    unsigned char *px = stbi_load_from_memory(file, (int) len, &w, &h, &comp, 4);
    free(file);
    if (!px) {
        editor_message("Could not read the image");
        return;
    }
    const double size = editor_setting_num("imageSize", 1), depth = editor_setting_num("imageDepth", 1);
    const int col = editor_setting_bool("imageCol");
    cJSON *o = cJSON_CreateArray();
    int l = 0, c = 0;
    for (int e = 0; e < w * h; e++) {
        c += 1;
        if (e % w == 0) {
            l++;
            c = 0;
        }
        const unsigned char *p = px + e * 4;
        const double alpha = round(p[3] / 255.0 * 100) / 100;
        if (alpha == 0) continue;
        cJSON *t = cJSON_CreateObject();
        cJSON *pa = cJSON_AddArrayToObject(t, "p"), *sa = cJSON_AddArrayToObject(t, "s");
        cJSON_AddItemToArray(pa, cJSON_CreateNumber(c * size));
        cJSON_AddItemToArray(pa, cJSON_CreateNumber(l * size * -1));
        cJSON_AddItemToArray(pa, cJSON_CreateNumber(0));
        cJSON_AddItemToArray(sa, cJSON_CreateNumber(size));
        cJSON_AddItemToArray(sa, cJSON_CreateNumber(size));
        cJSON_AddItemToArray(sa, cJSON_CreateNumber(depth));
        char hex[16];
        snprintf(hex, sizeof(hex), "#%02x%02x%02x", p[0], p[1], p[2]); // rgbArrayToHex
        cJSON_AddStringToObject(t, "c", hex);
        if (alpha != 1) cJSON_AddNumberToObject(t, "o", alpha);
        if (!col) cJSON_AddNumberToObject(t, "l", 1);
        cJSON_AddItemToArray(o, t);
    }
    stbi_image_free(px);
    ed_merge_objects(o);
    apply_center(o);
    deliver(o, replace, NULL, "convertedImage.txt");
}

static void convert_voxel(void *user, const char *path) {
    // convertVoxel(e, json, n): {voxels: [{x, y, z}]} -> voxelSize boxes
    const int replace = (int) (intptr_t) user;
    if (replace && !g_ed.proj->selected) {
        editor_message("Select a object to replace first");
        return;
    }
    char *text = read_text_file(path);
    cJSON *doc = text ? cJSON_Parse(text) : NULL;
    free(text);
    if (!doc) {
        editor_message("Invalid voxel file");
        return;
    }
    const double size = editor_setting_num("voxelSize", 10);
    cJSON *o = cJSON_CreateArray();
    const cJSON *v;
    cJSON_ArrayForEach(v, cJSON_GetObjectItemCaseSensitive(doc, "voxels")) {
        cJSON *t = cJSON_CreateObject();
        cJSON *pa = cJSON_AddArrayToObject(t, "p"), *sa = cJSON_AddArrayToObject(t, "s");
        static const char *const axes[3] = {"x", "y", "z"};
        for (int k = 0; k < 3; k++) {
            const cJSON *n = cJSON_GetObjectItemCaseSensitive(v, axes[k]);
            const double val = cJSON_IsNumber(n) ? n->valuedouble : cJSON_IsString(n) ? atof(n->valuestring) : 0;
            cJSON_AddItemToArray(pa, cJSON_CreateNumber((double) (long) val * size)); // parseInt
            cJSON_AddItemToArray(sa, cJSON_CreateNumber(size));
        }
        cJSON_AddItemToArray(o, t);
    }
    cJSON_Delete(doc);
    if (editor_setting_bool("mergeVoxels")) ed_merge_objects(o);
    deliver(o, replace, "modmap", "convertedVoxels.txt");
}

static void act_image_convert(EdCtl *c) { (void) c; ed_pick_file("image/*", convert_image, (void *) (intptr_t) 0); }
static void act_image_import(EdCtl *c) { (void) c; ed_pick_file("image/*", convert_image, (void *) (intptr_t) 1); }
static void act_voxel_convert(EdCtl *c) { (void) c; ed_pick_file("", convert_voxel, (void *) (intptr_t) 0); }
static void act_voxel_import(EdCtl *c) { (void) c; ed_pick_file("", convert_voxel, (void *) (intptr_t) 1); }

// replicate(e, t, n): a copy of e moved past its extent on axis n, t apart
static cJSON *replicate(const cJSON *e, const double t, const int n) {
    cJSON *i = cJSON_Duplicate(e, 1);
    const cJSON *first = cJSON_GetArrayItem(e, n); // e[n] (v3.1.1 indexes the array by the axis)
    if (!first) first = cJSON_GetArrayItem(e, 0);
    double r = arr_num(first, "p", n) + arr_num(first, "s", n), a = arr_num(first, "p", n);
    const cJSON *o;
    cJSON_ArrayForEach(o, e) {
        if (arr_num(o, "p", n) + arr_num(o, "s", n) > r) r = arr_num(o, "p", n) + arr_num(o, "s", n);
        if (arr_num(o, "p", n) < a) a = arr_num(o, "p", n);
    }
    cJSON *x;
    cJSON_ArrayForEach(x, i) arr_set(x, "p", n, arr_num(x, "p", n) + r - a + t);
    return i;
}

static cJSON *replicate_multi(const cJSON *e, const int t, const double n, const int i) {
    cJSON *r = cJSON_Duplicate(e, 1);
    cJSON *a = cJSON_Duplicate(r, 1);
    for (int k = 0; k < t; k++) {
        cJSON *next = replicate(a, n, i);
        cJSON_Delete(a);
        a = next;
        cJSON *x;
        cJSON_ArrayForEach(x, a) cJSON_AddItemToArray(r, cJSON_Duplicate(x, 1));
    }
    cJSON_Delete(a);
    return r;
}

static void act_planner(EdCtl *c) {
    (void) c;
    // layoutPlanner: a grid of plannerSize cubes, Dir copies per axis, Space apart, centred
    const double s = ed_json_num(g_ed.advanced, "plannerSize", 10);
    cJSON *o = cJSON_CreateArray();
    cJSON *cube = cJSON_CreateObject();
    cJSON *pa = cJSON_AddArrayToObject(cube, "p"), *sa = cJSON_AddArrayToObject(cube, "s");
    for (int k = 0; k < 3; k++) {
        cJSON_AddItemToArray(pa, cJSON_CreateNumber(0));
        cJSON_AddItemToArray(sa, cJSON_CreateNumber(s));
    }
    cJSON_AddStringToObject(cube, "c", ed_json_str(g_ed.advanced, "plannerColor", "#460050"));
    cJSON_AddItemToArray(o, cube);
    static const char *const xyz[3] = {"X", "Y", "Z"};
    for (int k = 0; k < 3; k++) {
        char dk[24], sk[24];
        snprintf(dk, sizeof(dk), "planner%sDir", xyz[k]);
        snprintf(sk, sizeof(sk), "planner%sSpace", xyz[k]);
        cJSON *next = replicate_multi(o, (int) ed_json_num(g_ed.advanced, dk, 0), (double) (int) ed_json_num(g_ed.advanced, sk, 0), k);
        cJSON_Delete(o);
        o = next;
    }
    apply_center(o);
    const cJSON *e;
    cJSON_ArrayForEach(e, o) {
        EditorObject *obj = ed_obj_new(e, 0);
        if (obj) ed_obj_add(obj, 1, 1);
    }
    cJSON_Delete(o);
}

// ---------------------------------------------------------------------------
// Optimizations (v7: Optimize Faces, Color Merging)

// advancedOptions keys the editor's defaults (editor.c) do not hold yet
static void advanced_defaults(void) {
    if (!cJSON_GetObjectItemCaseSensitive(g_ed.advanced, "colorRange")) cJSON_AddNumberToObject(g_ed.advanced, "colorRange", 1500);
    if (!cJSON_GetObjectItemCaseSensitive(g_ed.advanced, "objectPainterType")) cJSON_AddNumberToObject(g_ed.advanced, "objectPainterType", 0);
}

// UTILS.getPercentDiff(after, before): Math.round((before - after) / after * 100) as JS prints it
static void percent_diff(const double after, const double before, char *out, const size_t n) {
    const double v = (before - after) / after * 100;
    if (isnan(v)) snprintf(out, n, "NaN");
    else if (isinf(v)) snprintf(out, n, "%sInfinity", v < 0 ? "-" : "");
    else snprintf(out, n, "%.0f", floor(v + 0.5) + 0.0);
}

typedef struct {
    double pos[3], size[3]; // the box centre and size
    int faces[6];
    int cube, candidate, blocker, changed;
} FaceBox;

// i(a, b, axis): b covers a's max side; f974: b covers a's min side; f975: a's cross section within b's
static int covers_max(const FaceBox *a, const FaceBox *b, const int k) {
    return a->pos[k] + a->size[k] / 2 >= b->pos[k] - b->size[k] / 2 && b->pos[k] + b->size[k] / 2 > a->pos[k] + a->size[k] / 2;
}
static int covers_min(const FaceBox *a, const FaceBox *b, const int k) {
    return a->pos[k] - a->size[k] / 2 <= b->pos[k] + b->size[k] / 2 && b->pos[k] - b->size[k] / 2 < a->pos[k] - a->size[k] / 2;
}
static int within(const FaceBox *a, const FaceBox *b, const int k, const int l) {
    return a->size[k] <= b->size[k] && a->size[l] <= b->size[l] && fabs(a->pos[k] - b->pos[k]) + a->size[k] / 2 <= b->size[k] / 2 &&
           fabs(a->pos[l] - b->pos[l]) + a->size[l] / 2 <= b->size[l] / 2;
}

static int prop_truthy(const EditorObject *o, const char *prop) {
    cJSON *v = ed_prop_get(o, prop);
    const int t = ed_json_truthy(v);
    cJSON_Delete(v);
    return t;
}

// Editor.optimizeFaces (E:6280): for axis-aligned visible CUBE pairs, a face fully covered by an
// opaque neighbour (not a FLAG / LINK texture) is switched off. Alerts "N% Face Reduction" (N
// relative to the faces left, as v7's getPercentDiff). One undo step.
static void optimize_faces(void) {
    const size_t n = g_ed.proj->count;
    ed_obj_select(NULL); // hideTransform
    FaceBox *box = calloc(n ? n : 1, sizeof(FaceBox));
    if (!box) return;
    int before = 0, after = 0;
    for (size_t i = 0; i < n; i++) {
        EditorObject *o = g_ed.proj->objects[i];
        FaceBox *f = &box[i];
        f->cube = !strcmp(editor_prefabs[o->prefab].name, "CUBE");
        const vec3 p = ed_obj_vec(o, "p", (vec3) {0}), r = ed_obj_vec(o, "r", (vec3) {0});
        cJSON *sz = ed_prop_get(o, "size");
        for (int k = 0; k < 3; k++) f->size[k] = cJSON_GetNumberValue(cJSON_GetArrayItem(sz, k));
        cJSON_Delete(sz);
        f->pos[0] = p.x;
        f->pos[1] = p.y + f->size[1] / 2; // pos[1] += size[1] / 2: the box centre
        f->pos[2] = p.z;
        cJSON *fa = ed_prop_get(o, "faces");
        for (int k = 0; k < 6; k++) f->faces[k] = ed_json_truthy(cJSON_GetArrayItem(fa, k));
        cJSON_Delete(fa);
        if (f->cube) for (int k = 0; k < 6; k++) before += f->faces[k];
        const int flat = r.x == 0 && r.y == 0 && r.z == 0;
        f->candidate = f->cube && flat && prop_truthy(o, "visible");
        cJSON *op = ed_prop_get(o, "opacity");
        cJSON *tex = ed_prop_get(o, "texture");
        const char *t = cJSON_IsString(tex) ? tex->valuestring : "";
        f->blocker = f->candidate && cJSON_IsNumber(op) && op->valuedouble == 1 && strcmp(t, "FLAG") && strcmp(t, "LINK");
        cJSON_Delete(op);
        cJSON_Delete(tex);
    }
    for (size_t i = 0; i < n; i++) {
        FaceBox *a = &box[i];
        if (!a->candidate) continue;
        for (size_t j = 0; j < n; j++) {
            const FaceBox *b = &box[j];
            if (i == j || !b->blocker) continue;
            static const int cross[3][2] = {{1, 2}, {0, 2}, {1, 0}};
            for (int k = 0; k < 3; k++) {
                if (!within(a, b, cross[k][0], cross[k][1])) continue;
                if (covers_max(a, b, k) && a->faces[k * 2]) a->faces[k * 2] = 0, a->changed = 1;
                if (covers_min(a, b, k) && a->faces[k * 2 + 1]) a->faces[k * 2 + 1] = 0, a->changed = 1;
            }
        }
    }
    ed_history_poll();
    const uint32_t batch = ed_history_batch_begin("optimize faces");
    for (size_t i = 0; i < n; i++) {
        if (box[i].cube) for (int k = 0; k < 6; k++) after += box[i].faces[k];
        if (!box[i].changed) continue;
        EditorObject *o = g_ed.proj->objects[i];
        cJSON *fa = cJSON_CreateArray();
        for (int k = 0; k < 6; k++) cJSON_AddItemToArray(fa, cJSON_CreateBool(box[i].faces[k]));
        ed_prop_set(o, "faces", fa);
        cJSON_Delete(fa);
        apply_object_change(o);
    }
    ed_history_poll();
    ed_history_batch_end(batch);
    free(box);
    char pct[32], msg[64];
    percent_diff(after, before, pct, sizeof(pct));
    snprintf(msg, sizeof(msg), "%s%% Face Reduction", pct);
    editor_message(msg);
}

static void act_optimize_faces(EdCtl *c) { (void) c; optimize_faces(); }

// a colour value as mergeColors reads it: a number is stringified (UTILS.stringifyColor), then
// upper-cased; 0 = null / not a colour
static int color_upper(const cJSON *v, char *out, const size_t n) {
    if (cJSON_IsNumber(v)) snprintf(out, n, "#%06x", (unsigned) v->valuedouble & 0xffffffu);
    else if (cJSON_IsString(v)) snprintf(out, n, "%s", v->valuestring);
    else return 0;
    for (char *s = out; *s; s++) *s = (char) toupper((unsigned char) *s);
    return 1;
}

// parseInt(c.slice(1), 16); -1 = NaN (never within range)
static long color_value(const char *c) {
    char *end = NULL;
    if (!c[0] || !isxdigit((unsigned char) c[1])) return -1;
    return strtol(c + 1, &end, 16);
}

#define MERGE_PALETTE_MAX 4096

// one colour key of Editor.mergeColors: 1 = merged into a palette entry
static int merge_one(EditorObject *o, const char *prop, const char *skip, char (*pal)[16], int *count, const double range) {
    cJSON *v = ed_prop_get(o, prop);
    int merged = 0;
    char c[16];
    // `color != "#FFFFFF" && color != null` (emissive: "#000000"), compared before the case change
    if (v && !(cJSON_IsString(v) && !strcmp(v->valuestring, skip)) && color_upper(v, c, sizeof(c))) {
        int known = 0;
        for (int k = 0; k < *count && !known; k++) known = !strcmp(pal[k], c);
        if (!known) {
            const long x = color_value(c);
            for (int k = 0; k < *count && x >= 0; k++) {
                const long y = color_value(pal[k]);
                if (y >= 0 && fabs((double) (x - y)) <= range) {
                    snprintf(c, sizeof(c), "%s", pal[k]);
                    merged = 1;
                    break;
                }
            }
            if (!merged && *count < MERGE_PALETTE_MAX) snprintf(pal[(*count)++], 16, "%s", c);
        }
        if (!cJSON_IsString(v) || strcmp(v->valuestring, c)) {
            cJSON *nv = cJSON_CreateString(c); // the setter stores the upper-case string
            ed_prop_set(o, prop, nv);
            cJSON_Delete(nv);
            apply_object_change(o);
        }
    }
    cJSON_Delete(v);
    return merged;
}

// Editor.mergeColors (E:6212): each colour / emissive snaps to the first palette colour (seeded with
// #FFFFFF, #000000, then every colour not merged) whose integer value is within colorRange.
// Alerts "N Colors Merged". One undo step.
// Intentional difference from v7.1.4: v7 reuses the object loop's index for the palette loops, so
// objects are skipped or visited again depending on the palette; here every object is visited once.
static void merge_colors(void) {
    advanced_defaults();
    const cJSON *rv = cJSON_GetObjectItemCaseSensitive(g_ed.advanced, "colorRange");
    double range = cJSON_IsNumber(rv) ? trunc(rv->valuedouble) : cJSON_IsString(rv) ? (double) atoi(rv->valuestring) : 0;
    if (range == 0) range = 1500; // parseInt(colorRange) || 1500
    char (*pal)[16] = calloc(MERGE_PALETTE_MAX, 16);
    if (!pal) return;
    int count = 2, merged = 0;
    snprintf(pal[0], 16, "#FFFFFF");
    snprintf(pal[1], 16, "#000000");
    ed_history_poll();
    const uint32_t batch = ed_history_batch_begin("merge colors");
    for (size_t i = 0; i < g_ed.proj->count; i++) {
        EditorObject *o = g_ed.proj->objects[i];
        merged += merge_one(o, "color", "#FFFFFF", pal, &count, range);
        merged += merge_one(o, "emissive", "#000000", pal, &count, range);
    }
    ed_history_poll();
    ed_history_batch_end(batch);
    free(pal);
    char msg[64];
    snprintf(msg, sizeof(msg), "%d Colors Merged", merged);
    editor_message(msg);
}

static void do_merge_colors(void *user, const char *ok) {
    (void) user;
    if (ok) merge_colors();
}

static void act_merge_colors(EdCtl *c) {
    (void) c;
    ed_confirm("Are you sure you want to merge similar colors? (This will effect the entire map)", do_merge_colors, NULL);
}

// panel.right.tools (UI:6087-6200): v7 merges v3.1.1's Customize panel into it
static void panel_tools(void) {
    EdBuild b;
    if (!ed_panel_right_begin(&b, "tools")) return;
    advanced_defaults();
    ed_gui_panel_buttons(&b, 1, "Tools", 0);
    ed_gui_rule(&b);
    ed_gui_folder_begin(&b, "Optimizations", 0, 0, 0, "tools.optimizations");
    ed_gui_button_line(&b, "Optimize Faces", "Execute", act_optimize_faces, 0);
    ed_gui_rule(&b);
    ed_gui_folder_begin(&b, "Color Merging", 0, 0, 0, "tools.optimizations.colorMerging");
    with(&b, ed_gui_slider(&b, "Color Range", 500, 50000, 100, 0), g_ed.advanced, "colorRange", NULL);
    ed_gui_button(&b, "Merge Colors", act_merge_colors);
    ed_gui_folder_end(&b, 0);
    ed_gui_folder_end(&b, 0);
    ed_gui_folder_begin(&b, "Text Generator", 0, 1, 0, "tools.textGenerator");
    ed_gui_button_line(&b, "Enter Text", "Open", act_text_gen, 0);
    ed_gui_folder_end(&b, 0);
    ed_gui_folder_begin(&b, "Image Converter", 0, 1, 0, "tools.imageConverter");
    with(&b, ed_gui_number(&b, "Size", "", NAN, 0), g_ed.settings, "imageSize", settings_changed);
    with(&b, ed_gui_number(&b, "Depth", "", NAN, 0), g_ed.settings, "imageDepth", settings_changed);
    with(&b, ed_gui_switch(&b, "Collidable", 0), g_ed.settings, "imageCol", settings_changed);
    ed_gui_button_line(&b, "Convert", "Convert", act_image_convert, 0);
    ed_gui_button_line(&b, "Import", "Import", act_image_import, 0);
    ed_gui_folder_end(&b, 0);
    ed_gui_folder_begin(&b, "Voxel Manager", 0, 1, 0, "tools.voxelManager");
    with(&b, ed_gui_switch(&b, "Merge", 0), g_ed.settings, "mergeVoxels", settings_changed);
    with(&b, ed_gui_number(&b, "Size", "", NAN, 0), g_ed.settings, "voxelSize", settings_changed);
    ed_gui_button_line(&b, "Convert", "Convert", act_voxel_convert, 0);
    ed_gui_button_line(&b, "Import", "Import", act_voxel_import, 0);
    ed_gui_folder_end(&b, 0);
    ed_gui_folder_begin(&b, "Layout Planner", 0, 1, 0, "tools.layoutPlanner");
    with(&b, ed_gui_switch(&b, "Merge", 0), g_ed.settings, "mergeVoxels", settings_changed);
    with(&b, ed_gui_number(&b, "Size", "", NAN, 0), g_ed.advanced, "plannerSize", NULL);
    with(&b, ed_gui_color(&b, "Color", 0), g_ed.advanced, "plannerColor", NULL);
    proxy_load("Dir", "planner%sDir");
    proxy_load("Space", "planner%sSpace");
    with(&b, ed_gui_vector3(&b, "Cubes in XYZ", NAN), g_proxy, "Dir", proxy_store);
    with(&b, ed_gui_vector3(&b, "Space in XYZ", NAN), g_proxy, "Space", proxy_store);
    ed_gui_button(&b, "Execute", act_planner);
    ed_gui_folder_end(&b, 0);
    ed_gui_folder_begin(&b, "Timelapse", 0, 1, 0, "tools.timelapse");
    with(&b, ed_gui_slider(&b, "Timer", 100, 5000, 1, 0), g_ed.advanced, "timelapseMS", NULL);
    with(&b, ed_gui_switch(&b, "Follow Objects", 0), g_ed.advanced, "timelapseFollow", NULL);
    ed_gui_button(&b, "Execute", act_timelapse);
    ed_gui_folder_end(&b, 0);
    ed_gui_folder_begin(&b, "Transform", 0, 1, 0, "tools.transform");
    proxy_load("scaleMap", "scaleMap%s");
    with(&b, ed_gui_vector3(&b, "Scale", NAN), g_proxy, "scaleMap", proxy_store);
    ed_gui_button(&b, "Scale", act_scale_map);
    static const EdOption dirs[] = {{"X", 0, NULL}, {"Y", 1, NULL}, {"Z", 2, NULL}};
    with(&b, ed_gui_select(&b, "Reflection", dirs, 3, 0), g_ed.advanced, "reflectDir", NULL);
    ed_gui_button(&b, "Reflect", act_reflect_map);
    ed_gui_folder_end(&b, 0);
    ed_gui_folder_begin(&b, "Frame", 0, 1, 0, "tools.frame");
    with(&b, ed_gui_number(&b, "Wall Thickness", "Thickness", NAN, 0), g_ed.advanced, "frameThickness", NULL);
    // intentional difference from v7.1.4: v7 binds "Has Ceiling" to frameThickness (it shows on
    // while the thickness is set and edits the thickness); it edits frameCeiling here
    with(&b, ed_gui_switch(&b, "Has Ceiling", 0), g_ed.advanced, "frameCeiling", NULL);
    with(&b, ed_gui_switch(&b, "Has Floor", 0), g_ed.advanced, "frameFloor", NULL);
    ed_gui_button(&b, "Frame It", act_frame);
    ed_gui_folder_end(&b, 0);
    ed_gui_folder_begin(&b, "Breakable", 0, 1, 0, "tools.breakable");
    with(&b, ed_gui_slider(&b, "Health", 1, 500, 1, 0), g_ed.advanced, "breakableHealth", NULL);
    with(&b, ed_gui_switch(&b, "Force Collision", 0), g_ed.advanced, "breakableCollision", NULL);
    ed_gui_button(&b, "Execute", act_breakable);
    ed_gui_folder_end(&b, 0);
    ed_gui_folder_begin(&b, "Colorize", 0, 1, 0, "tools.colorize");
    ed_gui_button_line(&b, "Random", "Execute", act_colorize_random, 0);
    ed_gui_button_line(&b, "Gold", "Execute", act_colorize_gold, 0);
    ed_gui_button_line(&b, "Input", "Execute", act_colorize_input, 0);
    ed_gui_folder_end(&b, 0);
    ed_gui_button_line(&b, "Drunk Map", "Execute", act_drunk, 0);
    ed_gui_end(&b, g_ed.el_right_list);
}

// ---------------------------------------------------------------------------
// group edits (the object panel's group header, editor_inspector.c)

static void edit_group(const char *key, const cJSON *value) {
    // editGroup(key, value): set one property on every member of the selected group
    EditorObject *n = selected_group();
    if (!g_ed.proj->group_count || !n) {
        editor_message("You cant edit a group that doesnt exist");
        return;
    }
    const EditorGroup *g = ed_group_of(n);
    for (size_t i = 0; i < g->count; i++) {
        EditorObject *o = ed_obj_find(g->objects[i]);
        if (!o) continue;
        ed_prop_set(o, key, value);
        apply_object_change(o);
    }
    refresh_counts();
}

// the object panel's group header (editor_inspector.c, v7 layout)
void ed_group_edit(const char *key, const cJSON *value) { edit_group(key, value); }
void ed_group_reflect(void) { reflect_objects(1); }

// jsonInput(file, target): import a prefab in place of the selection
// jsonInput(file, t): t = the Quick GUI's object (by uuid: the prompt is asynchronous),
// 0 = the selection
static EditorObject *json_target(void *user) {
    const uint32_t uuid = (uint32_t) (uintptr_t) user;
    return uuid ? ed_obj_find(uuid) : g_ed.proj->selected;
}

static void json_text_in(void *user, const char *value) {
    EditorObject *t = json_target(user);
    if (value && *value && t) ed_obj_replace(t, value, NULL, editor_setting_bool("assetAutoGroup"));
}

static void json_file_in(void *user, const char *path) {
    EditorObject *t = json_target(user);
    char *text = read_text_file(path);
    if (text && t) ed_obj_replace(t, text, NULL, editor_setting_bool("assetAutoGroup"));
    else if (text) editor_message("You must select a object first");
    free(text);
}

void ed_json_input_for(const int file, EditorObject *target) {
    void *user = (void *) (uintptr_t) (target ? target->uuid : 0);
    if (file) ed_pick_file(".txt,.json,text/plain,application/json", json_file_in, user);
    else ed_prompt("Import Object Json", "", json_text_in, user);
}

void ed_json_input(const int file) { ed_json_input_for(file, NULL); }

// exportGroup(): the selected group's members, centred, downloaded as prefab_<name>.txt
static void export_named(void *user, const char *name) {
    (void) user;
    if (!name || !*name) {
        editor_message("Please name your Prefab");
        return;
    }
    cJSON *t = cJSON_Duplicate(g_ed.copy, 1);
    apply_center(t);
    char *text = cJSON_PrintUnformatted(t);
    cJSON_Delete(t);
    char file[160];
    size_t j = (size_t) snprintf(file, sizeof(file), "prefab_");
    for (size_t i = 0; name[i] && j + 5 < sizeof(file); i++) file[j++] = name[i] == ' ' ? '_' : name[i];
    snprintf(file + j, sizeof(file) - j, ".txt");
    ed_download(text, file);
    free(text);
}

void ed_export_group(void) {
    const cJSON *t = ed_copy_group(NULL);
    if (!t) return; // copyGroup() returns undefined: v3.1.1 throws on t.length here
    if (!cJSON_GetArraySize(t)) {
        editor_message("There was nothing to save");
        return;
    }
    ed_prompt("Name your Prefab", "", export_named, NULL);
}

static void act_json(EdCtl *c) { (void) c; ed_json_input(0); }
static void act_file(EdCtl *c) { (void) c; ed_json_input(1); }

// ---------------------------------------------------------------------------
// Prefabs (Object > Prefab > Tools): panel.right.prefab (UI:6202-6257)

// the Object Painter's scrap presets (E:66-89 `scraps`, Type 1..8; Type 0 = the JSON field)
static const struct { const char *name, *json; } k_scraps[] = {
    {"Junk", "[{\"p\":[259.7,63.9,6.1],\"s\":[8.29,3.15,8.6],\"i\":29,\"pl\":1,\"cdy\":1,\"tm\":0,\"su\":1,\"te\":16,\"sfd\":1,\"tas\":[{\"esa\":29736,\"ev\":0.1,\"er\":1,\"ef\":2,\"ta\":34},{\"ta\":59,\"tct\":\"0.01\"}],\"rsc\":25,\"sd\":1,\"ms\":5,\"aid\":\"30082\",\"inm\":\"pickup Junk\"}]"},
    {"Cloth", "[{\"p\":[112.9,64,-31.5],\"s\":[6.87,1.22,11.62],\"i\":29,\"pl\":1,\"cdy\":1,\"tm\":0,\"su\":1,\"te\":16,\"sfd\":1,\"tas\":[{\"esa\":29736,\"ev\":0.1,\"er\":1,\"ef\":2,\"ta\":34},{\"ta\":60,\"tct\":\"3238\"}],\"rsc\":20,\"sd\":1,\"ms\":5,\"aid\":\"30162\",\"inm\":\"pickup Cloth\"}]"},
    {"Wood", "[{\"p\":[-32.2,58,-93.2],\"s\":[3.38,0.65,16.87],\"i\":29,\"pl\":1,\"cdy\":1,\"tm\":0,\"su\":1,\"te\":16,\"sfd\":1,\"tas\":[{\"esa\":29736,\"ev\":0.1,\"er\":1,\"ef\":2,\"ta\":34},{\"ta\":60,\"tct\":\"3237\"}],\"rsc\":40,\"sd\":1,\"ms\":4,\"aid\":\"30061\",\"inm\":\"pickup Wood\"}]"},
    {"Wires", "[{\"p\":[91.9,-139.1,-161.6],\"s\":[12.52,4.18,7.76],\"i\":29,\"pl\":1,\"cdy\":1,\"r\":[0,1.2,0],\"tm\":0,\"su\":1,\"te\":16,\"sfd\":1,\"tas\":[{\"esa\":29736,\"ev\":0.1,\"er\":1,\"ef\":2,\"ta\":34},{\"ta\":60,\"tct\":\"3239\"}],\"rsc\":10,\"sd\":1,\"ms\":4,\"aid\":\"30163\",\"inm\":\"pickup Wires\"}]"},
    {"Tin can", "[{\"p\":[-136,-144,-100.5],\"s\":[2.22,4.57,2.22],\"i\":29,\"pl\":1,\"cdy\":1,\"tm\":0,\"su\":1,\"te\":16,\"sfd\":1,\"tas\":[{\"esa\":29736,\"ev\":0.1,\"er\":1,\"ef\":2,\"ta\":34},{\"ta\":60,\"tct\":\"4044\"}],\"rsc\":20,\"sd\":1,\"ms\":3,\"aid\":\"30167\",\"inm\":\"pickup Tin Can\"}]"},
    {"Chocolate", "[{\"p\":[-136,-144,-100.5],\"s\":[2.22,4.57,2.22],\"i\":29,\"pl\":1,\"cdy\":1,\"tm\":0,\"su\":1,\"te\":16,\"sfd\":1,\"tas\":[{\"esa\":29736,\"ev\":0.1,\"er\":1,\"ef\":2,\"ta\":34},{\"ta\":60,\"tct\":\"4786\"}],\"rsc\":20,\"sd\":1,\"ms\":3,\"aid\":\"39525\",\"inm\":\"pickup Chocolate\"}]"},
    {"Carrot", "[{\"p\":[-136,-144,-100.5],\"s\":[2.22,4.57,2.22],\"i\":29,\"pl\":1,\"cdy\":1,\"tm\":0,\"su\":1,\"te\":16,\"sfd\":1,\"tas\":[{\"esa\":29736,\"ev\":0.1,\"er\":1,\"ef\":2,\"ta\":34},{\"ta\":60,\"tct\":\"4787\"}],\"rsc\":20,\"sd\":1,\"ms\":3,\"aid\":\"39526\",\"inm\":\"pickup Carrot\"}]"},
    {"Frog", "[{\"p\":[0,9,-32],\"s\":[2.32,2.49,2.73],\"i\":29,\"pl\":1,\"cdy\":1,\"tm\":0,\"su\":1,\"te\":16,\"sfd\":1,\"tas\":[{\"esa\":29736,\"ev\":0.1,\"er\":1,\"ef\":2,\"ta\":34},{\"ta\":60,\"tct\":\"5366\"}],\"rsc\":2,\"sd\":1,\"aid\":\"46480\",\"inm\":\"pickup Frog\"}]"},
};

const char *ed_object_painter_json(void) {
    // Shift+click painting (E:2823): objectPainterType 0 = objectPainterJson (when not empty), else
    // scraps[type - 1]; NULL = nothing to paint
    if (!ed_json_truthy(cJSON_GetObjectItemCaseSensitive(g_ed.advanced, "objectPainter"))) return NULL;
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(g_ed.advanced, "objectPainterType");
    const int type = cJSON_IsNumber(t) ? (int) t->valuedouble : cJSON_IsString(t) ? atoi(t->valuestring) : 0;
    if (type == 0) {
        const char *json = ed_json_str(g_ed.advanced, "objectPainterJson", "");
        return json[0] ? json : NULL;
    }
    return type >= 1 && type <= (int) (sizeof(k_scraps) / sizeof(k_scraps[0])) ? k_scraps[type - 1].json : NULL;
}

static void on_painter(EdCtl *c, const cJSON *v) {
    (void) c;
    html_dom_set_display(html_dom_by_id("objPaint"), ed_json_truthy(v) ? "" : "none");
}

static void panel_prefab(void) {
    EdBuild b;
    if (!ed_panel_right_begin(&b, "prefab")) return;
    advanced_defaults();
    ed_gui_panel_buttons(&b, 1, "Prefabs", 0);
    ed_gui_rule(&b);
    with(&b, ed_gui_switch(&b, "Auto Group", 0), g_ed.settings, "assetAutoGroup", settings_changed);
    with(&b, ed_gui_slider(&b, "Rotation", 0, 359, 1, 0), g_ed.advanced, "rotation", NULL);
    ed_gui_folder_begin(&b, "Import", 1, 0, 0, "prefab.import");
    ed_gui_button_line(&b, "Json", "Import", act_json, 1);
    ed_gui_button_line(&b, "File", "Import", act_file, 1);
    ed_gui_folder_end(&b, 1);
    with(&b, ed_gui_switch(&b, "Object Painter", 0), g_ed.advanced, "objectPainter", on_painter);
    // the "div" blueprint: `<div id=objPaint null>` when shown. Intentional difference from v7.1.4:
    // v7 builds it shown when objectPainterType is set (addDiv(.., objectPainterType)), so a rebuilt
    // panel hides the JSON field of an active painter; here it follows the Object Painter switch
    const int show = ed_json_truthy(cJSON_GetObjectItemCaseSensitive(g_ed.advanced, "objectPainter"));
    edb_addf(&b.html, "<div id=objPaint %s>", show ? "null" : "style=\"display: none\"");
    with(&b, ed_gui_text(&b, "JSON", "JSON", 1), g_ed.advanced, "objectPainterJson", NULL);
    static EdOption types[1 + sizeof(k_scraps) / sizeof(k_scraps[0])];
    types[0] = (EdOption) {"JSON", 0, NULL};
    for (size_t i = 0; i < sizeof(k_scraps) / sizeof(k_scraps[0]); i++) types[i + 1] = (EdOption) {k_scraps[i].name, (double) (i + 1), NULL};
    with(&b, ed_gui_select(&b, "Type", types, (int) (sizeof(types) / sizeof(types[0])), 1), g_ed.advanced, "objectPainterType", NULL);
    edb_add(&b.html, "</div>\n");
    ed_gui_end(&b, g_ed.el_right_list);
}

// ---------------------------------------------------------------------------

int ed_tools_panel_show(const char *name) {
    if (!strcmp(name, "tools")) panel_tools();
    else if (!strcmp(name, "prefab")) panel_prefab();
    else return 0;
    return 1;
}

// KRUNKNATIVE_EDITOR_SCRIPT "tool:..." (tools/editor/tools_check.py): tool:import:<file> (importMap
// of a JSON file), tool:optimizeFaces, tool:mergeColors[:<range>] (no confirm). 1 = handled.
int ed_tools_script(const char *cmd, const char *arg) {
    if (strcmp(cmd, "tool") || !arg) return 0;
    if (!strncmp(arg, "import:", 7)) {
        char *text = read_text_file(arg + 7);
        if (!text || !ed_map_import(text)) fprintf(stderr, "[editor] tool:import: cannot import %s\n", arg + 7);
        free(text);
    } else if (!strcmp(arg, "optimizeFaces")) {
        optimize_faces();
    } else if (!strncmp(arg, "mergeColors", 11)) {
        advanced_defaults();
        if (arg[11] == ':') ed_json_set_num(g_ed.advanced, "colorRange", atof(arg + 12));
        merge_colors();
    } else {
        fprintf(stderr, "[editor] tool: unknown \"%s\"\n", arg);
    }
    return 1;
}
