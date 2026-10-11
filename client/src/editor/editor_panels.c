// Map editor panels: Krunker v3.1.1 ui/editor.js `_toolbar`, `panel.left.essential`,
// `panel.right.{blank,object,environment,...}` and `_window`, built with editor_gui.c.
#include "editor_internal.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { PANEL_LEFT, PANEL_RIGHT, PANEL_WINDOW, PANEL_TOOLBAR };

static int g_locked[2];
static char g_right_active[32] = "blank";
static char g_left_active[32] = "";

int ed_panel_locked(const int right) { return g_locked[right ? 1 : 0]; }
const char *ed_panel_right_active(void) { return g_right_active; }

// ---------------------------------------------------------------------------
// data (config lists)

static cJSON *g_data;

const cJSON *editor_data_json(void) {
    if (!g_data) g_data = cJSON_Parse(editor_data_text);
    return g_data;
}

const cJSON *editor_data_get(const char *key) {
    return cJSON_GetObjectItemCaseSensitive(editor_data_json(), key);
}

// ---------------------------------------------------------------------------
// control helpers: create + bind + emit

static EdCtl *finish(EdBuild *b, EdCtl *c, const EdChangeFn fn) {
    c->on_change = fn;
    ed_gui_emit_value(b, c);
    return c;
}

static EdCtl *obj_switch(EdBuild *b, const char *name, const char *prop, const int sub, const EdChangeFn fn) {
    EdCtl *c = ed_gui_switch(b, name, sub);
    ed_ctl_bind_obj(c, prop);
    return finish(b, c, fn);
}

static EdCtl *obj_slider(EdBuild *b, const char *name, const char *prop, const double mn, const double mx, const double step, const int sub, const EdChangeFn fn) {
    EdCtl *c = ed_gui_slider(b, name, mn, mx, step, sub);
    ed_ctl_bind_obj(c, prop);
    return finish(b, c, fn);
}

static EdCtl *obj_select(EdBuild *b, const char *name, const char *prop, const EdOption *opts, const int n, const int sub, const EdChangeFn fn) {
    EdCtl *c = ed_gui_select(b, name, opts, n, sub);
    ed_ctl_bind_obj(c, prop);
    c->value_is_string = n > 0 && opts[0].svalue != NULL;
    return finish(b, c, fn);
}

static EdCtl *obj_color(EdBuild *b, const char *name, const char *prop, const int sub) {
    EdCtl *c = ed_gui_color(b, name, sub);
    ed_ctl_bind_obj(c, prop);
    return finish(b, c, NULL);
}

static EdCtl *obj_text(EdBuild *b, const char *name, const char *prop, const char *placeholder, const int sub, const EdChangeFn fn) {
    EdCtl *c = ed_gui_text(b, name, placeholder, sub);
    ed_ctl_bind_obj(c, prop);
    return finish(b, c, fn);
}

static EdCtl *obj_number(EdBuild *b, const char *name, const char *prop, const char *placeholder, const int sub) {
    EdCtl *c = ed_gui_number(b, name, placeholder, 0, sub);
    ed_ctl_bind_obj(c, prop);
    return finish(b, c, NULL);
}

static EdCtl *obj_vector3(EdBuild *b, const char *name, const char *prop, const EdChangeFn fn) {
    EdCtl *c = ed_gui_vector3(b, name, 0);
    ed_ctl_bind_obj(c, prop);
    return finish(b, c, fn);
}

static EdCtl *json_switch(EdBuild *b, const char *name, cJSON *obj, const char *key, const int sub, const EdChangeFn fn) {
    EdCtl *c = ed_gui_switch(b, name, sub);
    ed_ctl_bind_json(c, obj, key);
    return finish(b, c, fn);
}

static EdCtl *json_slider(EdBuild *b, const char *name, cJSON *obj, const char *key, const double mn, const double mx, const double step, const int sub, const EdChangeFn fn) {
    EdCtl *c = ed_gui_slider(b, name, mn, mx, step, sub);
    ed_ctl_bind_json(c, obj, key);
    return finish(b, c, fn);
}

static EdCtl *json_text(EdBuild *b, const char *name, cJSON *obj, const char *key, const char *placeholder, const int sub, const EdChangeFn fn) {
    EdCtl *c = ed_gui_text(b, name, placeholder, sub);
    ed_ctl_bind_json(c, obj, key);
    return finish(b, c, fn);
}

static EdCtl *json_select(EdBuild *b, const char *name, cJSON *obj, const char *key, const EdOption *opts, const int n, const int sub, const EdChangeFn fn) {
    EdCtl *c = ed_gui_select(b, name, opts, n, sub);
    ed_ctl_bind_json(c, obj, key);
    c->value_is_string = n > 0 && opts[0].svalue != NULL;
    return finish(b, c, fn);
}

static EdCtl *json_color(EdBuild *b, const char *name, cJSON *obj, const char *key, const int sub, const EdChangeFn fn) {
    EdCtl *c = ed_gui_color(b, name, sub);
    ed_ctl_bind_json(c, obj, key);
    return finish(b, c, fn);
}

static EdCtl *json_number(EdBuild *b, const char *name, cJSON *obj, const char *key, const char *placeholder, const int sub, const EdChangeFn fn) {
    EdCtl *c = ed_gui_number(b, name, placeholder, 0, sub);
    ed_ctl_bind_json(c, obj, key);
    return finish(b, c, fn);
}

static EdCtl *json_key_extra(EdBuild *b, const char *name, cJSON *obj, const char *key, const int editable, const int sub, const EdChangeFn fn, const char *extra) {
    EdCtl *c = ed_gui_key(b, name, editable, NULL, 0, sub);
    ed_ctl_bind_json(c, obj, key);
    if (extra) snprintf(c->path, sizeof(c->path), "%s", extra);
    return finish(b, c, fn);
}

static EdCtl *json_key(EdBuild *b, const char *name, cJSON *obj, const char *key, const int editable, const int sub, const EdChangeFn fn) {
    return json_key_extra(b, name, obj, key, editable, sub, fn, NULL);
}

// _html.arrow(direction): the arrow icons of the fly keys
static const char *arrow_html(const char *dir) {
    static char buf[4][128];
    static int n;
    const int deg = !strcmp(dir, "up") ? -45 : !strcmp(dir, "down") ? 135 : !strcmp(dir, "left") ? -135 : 45;
    char *out = buf[n++ & 3];
    snprintf(out, 128, "<i class=\"material-icons\" style=\"transform: translateY(5px) rotate(%ddeg)\">call_made</i>", deg);
    return out;
}

// a fixed label in a "key" line (Krunker's keybind list rows with key: "val")
static void key_line(EdBuild *b, const char *name, const char *keys) {
    cJSON *tmp = cJSON_CreateObject();
    cJSON_AddStringToObject(tmp, "val", keys);
    json_key(b, name, tmp, "val", 0, 0, NULL);
    cJSON_Delete(tmp); // the markup is written; the control is never active
}

static cJSON *child_obj(cJSON *parent, const char *key) {
    cJSON *o = cJSON_GetObjectItemCaseSensitive(parent, key);
    if (!cJSON_IsObject(o)) {
        cJSON_DeleteItemFromObjectCaseSensitive(parent, key);
        o = cJSON_AddObjectToObject(parent, key);
    }
    return o;
}

static void ensure_default(cJSON *obj, const char *key, const cJSON *def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (v && !cJSON_IsNull(v)) return;
    cJSON_DeleteItemFromObjectCaseSensitive(obj, key);
    cJSON_AddItemToObject(obj, key, def ? cJSON_Duplicate(def, 1) : cJSON_CreateNull());
}

// options built from a JSON string list; the arrays live for the panel's lifetime
#define OPT_POOL 4096
static EdOption g_opt_pool[OPT_POOL];
static int g_opt_used;
static char g_opt_names[OPT_POOL][48];

static EdOption *opts_alloc(const int n) {
    if (g_opt_used + n > OPT_POOL) g_opt_used = 0;
    EdOption *o = &g_opt_pool[g_opt_used];
    g_opt_used += n;
    return o;
}

static EdOption *opts_from_names(const cJSON *names, int *count) {
    const int n = cJSON_GetArraySize(names);
    EdOption *o = opts_alloc(n);
    for (int i = 0; i < n; i++) {
        const cJSON *it = cJSON_GetArrayItem(names, i);
        o[i] = (EdOption) {cJSON_IsString(it) ? it->valuestring : "", i, NULL};
    }
    *count = n;
    return o;
}

// UTILS.formatConstName
static const char *const_name(const char *s, char *out, const size_t n) {
    size_t j = 0;
    int start = 1;
    for (size_t i = 0; s[i] && j + 1 < n; i++) {
        if (s[i] == '_') {
            out[j++] = ' ';
            start = 1;
            continue;
        }
        out[j++] = start ? (char) toupper((unsigned char) s[i]) : (char) tolower((unsigned char) s[i]);
        start = 0;
    }
    out[j] = 0;
    return out;
}

// ---------------------------------------------------------------------------
// toolbar (_toolbar.show)

enum {
    TB_IMPORT, TB_EXPORT, TB_EXPORT_GLTF, TB_PUBLISH, TB_SAVE_AS, TB_NEW, TB_COPY,
    TB_UNDO, TB_REDO, TB_DELETE, TB_DUPLICATE, TB_SETTINGS,
    TB_PLACEHOLDER, TB_GROUP_CREATE, TB_GROUP_STOP, TB_GROUP_STOP_ALL, TB_GROUP_COPY, TB_GROUP_CUT, TB_GROUP_PASTE,
    TB_PREFAB_JSON, TB_PREFAB_FILE, TB_PREFAB_EXPORT, TB_GROUP_EDIT, TB_CUSTOM,
    TB_ENVIRONMENT, TB_TERRAIN, TB_ZONE, TB_TOOLS, TB_TEST, TB_HOST, TB_QUICK_START, TB_CUSTOMIZE, TB_CONTROLS,
    TB_CREATE = 1000, // + prefab id
};

static void toolbar_close_menus(void) {
    HtmlElem drops[64];
    const int n = html_dom_query_all(html_dom_by_id("_toolbar"), ".toolbarDropDown, .toolbarSubDropDown", drops, 64);
    for (int i = 0; i < n; i++) html_dom_set_display(drops[i], "");
}

static void toolbar_action(EdCtl *c) {
    toolbar_close_menus();
    const int a = c->arg;
    if (a >= TB_CREATE) {
        ed_obj_create(a - TB_CREATE);
        return;
    }
    switch (a) {
        case TB_IMPORT: ed_file_import(); break;
        case TB_EXPORT: ed_file_export(); break;
        case TB_EXPORT_GLTF: editor_message("GLTF export is not available in KrunkNative."); break;
        case TB_PUBLISH: editor_message("Publishing needs a Krunker account server; use Export instead."); break;
        case TB_SAVE_AS: ed_file_save_as(); break;
        case TB_NEW: ed_map_clear(); break; // confirm("Are you sure you want to reset the map?")
        case TB_COPY: {
            char *text = ed_map_export_string();
            if (text) {
                ed_clipboard_copy(text);
                free(text);
                editor_message("Map copied to the clipboard.");
            }
            break;
        }
        case TB_UNDO: ed_history_execute(0); break;
        case TB_REDO: ed_history_execute(1); break;
        case TB_DELETE: ed_obj_delete_selected(); break;
        case TB_DUPLICATE: ed_obj_duplicate(); break;
        case TB_SETTINGS: editor_show_window(3); break;
        case TB_PLACEHOLDER: ed_obj_create_placeholder(); break;
        case TB_GROUP_CREATE: ed_group_create(); break;
        case TB_GROUP_STOP: ed_group_stop(0); break;
        case TB_GROUP_STOP_ALL: ed_group_stop(1); break;
        case TB_GROUP_COPY: ed_obj_copy(0, 0); break;
        case TB_GROUP_CUT: ed_obj_copy(1, 0); break;
        case TB_GROUP_PASTE: ed_obj_paste(); break;
        case TB_PREFAB_JSON: {
            char *text = ed_clipboard_paste();
            if (text) {
                cJSON *arr = cJSON_Parse(text);
                if (cJSON_IsObject(arr) && cJSON_GetObjectItem(arr, "objects")) {
                    cJSON *objects = cJSON_DetachItemFromObject(arr, "objects");
                    cJSON_Delete(arr);
                    arr = objects;
                }
                if (cJSON_IsArray(arr)) {
                    cJSON_Delete(g_ed.copy);
                    g_ed.copy = arr;
                    ed_obj_paste();
                } else {
                    cJSON_Delete(arr);
                    editor_message("Clipboard does not hold prefab JSON.");
                }
                free(text);
            }
            break;
        }
        case TB_PREFAB_FILE: ed_file_import(); break;
        case TB_PREFAB_EXPORT: ed_obj_copy(0, 0); editor_message("Prefab JSON copied to the clipboard."); break;
        case TB_GROUP_EDIT: ed_panel_right_show("grouping"); break;
        case TB_CUSTOM: ed_obj_create(ED_PREFAB_CUSTOM_ASSET); break;
        case TB_ENVIRONMENT: ed_panel_right_show("environment"); break;
        case TB_TERRAIN: ed_panel_right_show("terrain"); break;
        case TB_ZONE: ed_panel_right_show("zone"); break;
        case TB_TOOLS: ed_panel_right_show("tools"); break;
        case TB_CUSTOMIZE: ed_panel_right_show("customize"); break;
        case TB_TEST: editor_test_map(0); break;
        case TB_HOST: editor_test_map(1); break;
        case TB_QUICK_START: ed_quick_start(); break;
        case TB_CONTROLS: editor_show_window(1); break;
        default: break;
    }
}

static void tb_option(EdBuild *b, const char *name, const int action) {
    EdCtl *c = ed_gui_ctl(b->panel, EG_TOOLBAR_OPTION);
    c->arg = action;
    c->action = toolbar_action;
    edb_addf(&b->html, "<div class=\"toolbarDropDownItem\" onclick=\";GUI.action(%d, this)\">", c->id);
    edb_add_escaped(&b->html, name);
    edb_add(&b->html, "</div>");
}

static void tb_sep(EdBuild *b) { edb_add(&b->html, "\n"); }
static void tb_rule(EdBuild *b) { edb_add(&b->html, "<hr>"); }

static void tb_root_begin(EdBuild *b, const char *name) {
    edb_addf(&b->html, "<div class=\"toolbarItem\" onmouseover=\"GUI.update.toolbar(this, true)\" onmouseout=\"GUI.update.toolbar(this)\">\n                  %s<div class=\"toolbarDropDown\">", name);
}

static void tb_menu_begin(EdBuild *b, const char *name) {
    edb_addf(&b->html, "<div class=\"toolbarDropDownItem\" onmouseover=\"GUI.update.toolbar(this, true)\" onmouseout=\"GUI.update.toolbar(this)\">\n                  %s<div class=\"toolbarSubDropDown\" style=\"top: this.offsetTop\">", name);
}

static void tb_end(EdBuild *b) { edb_add(&b->html, "</div></div>"); }

void ed_toolbar_show(void) {
    EdBuild b;
    ed_gui_begin(&b, PANEL_TOOLBAR);
    tb_root_begin(&b, "File");
    tb_option(&b, "Import...", TB_IMPORT); tb_sep(&b);
    tb_option(&b, "Export...", TB_EXPORT); tb_sep(&b);
    tb_option(&b, "Export GLTF", TB_EXPORT_GLTF); tb_sep(&b);
    tb_option(&b, "Publish...", TB_PUBLISH); tb_sep(&b);
    tb_rule(&b); tb_sep(&b);
    tb_option(&b, "Save As", TB_SAVE_AS); tb_sep(&b);
    tb_rule(&b); tb_sep(&b);
    tb_option(&b, "New", TB_NEW); tb_sep(&b);
    tb_option(&b, "Copy", TB_COPY);
    tb_end(&b); tb_sep(&b);

    tb_root_begin(&b, "Edit");
    tb_option(&b, "Undo", TB_UNDO); tb_sep(&b);
    tb_option(&b, "Redo", TB_REDO); tb_sep(&b);
    tb_rule(&b); tb_sep(&b);
    tb_option(&b, "Delete", TB_DELETE); tb_sep(&b);
    tb_option(&b, "Duplicate", TB_DUPLICATE); tb_sep(&b);
    tb_rule(&b); tb_sep(&b);
    tb_option(&b, "Settings...", TB_SETTINGS);
    tb_end(&b); tb_sep(&b);

    tb_root_begin(&b, "Object");
    // Basic / Models / Tools / AI: every exportable prefab (not localOnly in production)
    static const char *menus[4] = {"Basic", "Models", "Tools", "AI"};
    for (int m = 0; m < 4; m++) {
        tb_menu_begin(&b, menus[m]);
        int first = 1;
        for (int i = 0; i < EDITOR_PREFAB_COUNT; i++) {
            const EditorPrefab *p = &editor_prefabs[i];
            if ((p->flags & (EP_LOCALONLY | EP_CUSTOM | EP_NOEXPORT)) || !p->name[0]) continue;
            const int cat = (p->flags & EP_AI) ? 3 : (p->flags & (EP_TOOL | EP_TOOL2)) ? 2 : (p->flags & EP_GEN) ? 1 : 0;
            if (cat != m) continue;
            if (!first) tb_sep(&b);
            first = 0;
            tb_option(&b, p->label, TB_CREATE + i);
        }
        tb_end(&b); tb_sep(&b);
    }
    tb_rule(&b); tb_sep(&b);
    tb_option(&b, "Placeholder", TB_PLACEHOLDER); tb_sep(&b);
    tb_menu_begin(&b, "Group");
    tb_option(&b, "Create", TB_GROUP_CREATE); tb_sep(&b);
    tb_option(&b, "Stop", TB_GROUP_STOP); tb_sep(&b);
    tb_option(&b, "Stop All", TB_GROUP_STOP_ALL); tb_sep(&b);
    tb_rule(&b); tb_sep(&b);
    tb_option(&b, "Copy", TB_GROUP_COPY); tb_sep(&b);
    tb_option(&b, "Cut", TB_GROUP_CUT); tb_sep(&b);
    tb_option(&b, "Paste", TB_GROUP_PASTE); tb_sep(&b);
    tb_rule(&b); tb_sep(&b);
    tb_menu_begin(&b, "Prefab");
    tb_menu_begin(&b, "Import");
    tb_option(&b, "JSON", TB_PREFAB_JSON); tb_sep(&b);
    tb_option(&b, "File", TB_PREFAB_FILE);
    tb_end(&b); tb_sep(&b);
    tb_option(&b, "Export", TB_PREFAB_EXPORT);
    tb_end(&b); tb_sep(&b);
    tb_rule(&b); tb_sep(&b);
    tb_option(&b, "Edit", TB_GROUP_EDIT);
    tb_end(&b); tb_sep(&b);
    tb_rule(&b); tb_sep(&b);
    tb_option(&b, "Custom...", TB_CUSTOM);
    tb_end(&b); tb_sep(&b);

    tb_root_begin(&b, "Component");
    tb_option(&b, "Environment", TB_ENVIRONMENT); tb_sep(&b);
    tb_option(&b, "Terrain", TB_TERRAIN); tb_sep(&b);
    tb_option(&b, "Zone", TB_ZONE); tb_sep(&b);
    tb_rule(&b); tb_sep(&b);
    tb_option(&b, "Tools", TB_TOOLS);
    tb_end(&b); tb_sep(&b);

    tb_root_begin(&b, "Map");
    tb_option(&b, "Test Map", TB_TEST); tb_sep(&b);
    tb_option(&b, "Host Map", TB_HOST); tb_sep(&b);
    tb_rule(&b); tb_sep(&b);
    tb_option(&b, "Quick Start", TB_QUICK_START); tb_sep(&b);
    tb_rule(&b); tb_sep(&b);
    tb_option(&b, "Customize", TB_CUSTOMIZE);
    tb_end(&b); tb_sep(&b);

    tb_root_begin(&b, "Help");
    tb_option(&b, "Controls...", TB_CONTROLS);
    tb_end(&b);
    ed_gui_end(&b, html_dom_by_id("_toolbar"));
}

// ---------------------------------------------------------------------------
// left panel: Essential (map name, messages, death height, game/server/weapon config)

static void on_death_height(EdCtl *c, const cJSON *v) { (void) c; (void) v; } // updateDeathBarrier (drawn per frame)

static void add_config_ctl(EdBuild *b, cJSON *target, const cJSON *w, const int sub) {
    const char *name = ed_json_str(w, "name", "");
    const char *var = ed_json_str(w, "varN", "");
    ensure_default(target, var, cJSON_GetObjectItemCaseSensitive(w, "def"));
    if (!strcmp(var, "aID")) {
        static char label[64];
        snprintf(label, sizeof(label), "%s ID", name);
        json_text(b, label, target, var, "ID", sub, NULL);
    } else if (ed_json_truthy(cJSON_GetObjectItemCaseSensitive(w, "bool"))) {
        json_switch(b, name, target, var, sub, NULL);
    } else if (ed_json_truthy(cJSON_GetObjectItemCaseSensitive(w, "input"))) {
        json_text(b, name, target, var, "", sub, NULL);
    } else if (ed_json_truthy(cJSON_GetObjectItemCaseSensitive(w, "select"))) {
        int n;
        EdOption *opts = opts_from_names(cJSON_GetObjectItemCaseSensitive(w, "options"), &n);
        json_select(b, name, target, var, opts, n, sub, NULL);
    } else {
        const double mx = ed_json_num(w, "maxF", ed_json_num(w, "maxP", ed_json_num(w, "max", 1)));
        json_slider(b, name, target, var, ed_json_num(w, "min", 0), mx, ed_json_num(w, "step", 1), sub, NULL);
    }
}

void ed_panel_left_show(void) {
    if (g_locked[0] && strcmp(g_left_active, "essential")) return;
    snprintf(g_left_active, sizeof(g_left_active), "essential");
    EdBuild b;
    ed_gui_begin(&b, PANEL_LEFT);
    ed_gui_panel_buttons(&b, 0, "Essential", 0);
    json_text(&b, "Name", g_ed.map_config, "name", "Name", 0, NULL);
    json_text(&b, "Welcome Msg", g_ed.map_config, "welMsg", "Welcome", 0, NULL);
    json_text(&b, "Mod URL", g_ed.map_config, "modURL", "Mod URL", 0, NULL);
    extern void ed_select_mod(EdCtl *);
    ed_gui_button(&b, "Select Mod", ed_select_mod);
    json_slider(&b, "Death Height", g_ed.map_config, "dthY", -1000, 3000, 1, 0, on_death_height);
    ed_gui_rule(&b);

    ed_gui_folder_begin(&b, "Game", 0, 0, 0, "game");
    const cJSON *game = editor_data_get("gameConfig");
    for (int cat = 0; cat < 2; cat++) {
        ed_gui_folder_begin(&b, cat ? "Player" : "Camera", 1, 1, 0, cat ? "game.player" : "game.camera");
        const cJSON *w;
        cJSON_ArrayForEach(w, game) {
            if (cJSON_GetObjectItemCaseSensitive(w, "cost")) continue;
            if ((int) ed_json_num(w, "cat", 0) != cat) continue;
            add_config_ctl(&b, g_ed.game_config, w, 1);
        }
        ed_gui_folder_end(&b, 1);
    }
    cJSON *modes = child_obj(g_ed.server_config, "modes");
    ed_gui_folder_begin(&b, "Modes", 1, 1, 0, "game.modes");
    int index = 0;
    const cJSON *m;
    cJSON_ArrayForEach(m, editor_data_get("modes")) {
        char key[8];
        snprintf(key, sizeof(key), "%d", index);
        if (!cJSON_GetObjectItemCaseSensitive(m, "cost") && !ed_json_truthy(cJSON_GetObjectItemCaseSensitive(m, "pcg"))) {
            if (!cJSON_GetObjectItemCaseSensitive(modes, key)) cJSON_AddBoolToObject(modes, key, index == 0);
            json_switch(&b, ed_json_str(m, "name", ""), modes, key, 1, NULL);
        }
        index++;
    }
    ed_gui_folder_end(&b, 1);
    cJSON *classes = child_obj(g_ed.server_config, "classes");
    ed_gui_folder_begin(&b, "Classes", 1, 1, 0, "game.classes");
    index = 0;
    cJSON_ArrayForEach(m, editor_data_get("classes")) {
        char key[8];
        snprintf(key, sizeof(key), "%d", index);
        if (!cJSON_GetObjectItemCaseSensitive(m, "cost")) {
            if (!cJSON_GetObjectItemCaseSensitive(classes, key)) cJSON_AddBoolToObject(classes, key, 1);
            json_switch(&b, ed_json_str(m, "name", ""), classes, key, 1, NULL);
        }
        index++;
    }
    ed_gui_folder_end(&b, 1);
    cJSON *settings = child_obj(g_ed.server_config, "settings");
    ed_gui_folder_begin(&b, "Settings", 1, 1, 0, "game.settings");
    cJSON_ArrayForEach(m, editor_data_get("serverConfig")) {
        if (cJSON_GetObjectItemCaseSensitive(m, "cost") || ed_json_truthy(cJSON_GetObjectItemCaseSensitive(m, "hideE"))) continue;
        add_config_ctl(&b, settings, m, 1);
    }
    ed_gui_folder_end(&b, 1);
    ed_gui_folder_end(&b, 0);

    ed_gui_folder_begin(&b, "Weapon Config", 0, 1, 0, "weapons");
    cJSON *weps = child_obj(g_ed.server_config, "weps");
    index = 0;
    const cJSON *weapon;
    cJSON_ArrayForEach(weapon, editor_data_get("weapons")) {
        char key[8], state[32];
        snprintf(key, sizeof(key), "%d", index);
        snprintf(state, sizeof(state), "weapons.w%d", index);
        cJSON *wc = child_obj(weps, key);
        ed_gui_folder_begin(&b, ed_json_str(weapon, "name", ""), 1, 1, 0, state);
        const cJSON *x;
        cJSON_ArrayForEach(x, editor_data_get("weaponConfig")) {
            const char *var = ed_json_str(x, "varN", "");
            const cJSON *def = cJSON_GetObjectItemCaseSensitive(weapon, var);
            if (!def || cJSON_IsNull(def)) continue;
            ensure_default(wc, var, def);
            if (ed_json_truthy(cJSON_GetObjectItemCaseSensitive(x, "bool"))) json_switch(&b, ed_json_str(x, "name", ""), wc, var, 1, NULL);
            else json_slider(&b, ed_json_str(x, "name", ""), wc, var, ed_json_num(x, "min", 0), ed_json_num(x, "max", 1), ed_json_num(x, "step", 1), 1, NULL);
        }
        ed_gui_folder_end(&b, 1);
        index++;
    }
    ed_gui_folder_end(&b, 0);
    ed_gui_end(&b, g_ed.el_left_list);
}

void ed_select_mod(EdCtl *c) {
    (void) c;
    editor_message("Mods are hosted on the Krunker asset server; paste a Mod URL instead.");
}

// ---------------------------------------------------------------------------
// right panel

static int right_can_show(const char *name) {
    return !g_locked[1] || !strcmp(g_right_active, name);
}

static void right_begin(EdBuild *b, const char *name) {
    snprintf(g_right_active, sizeof(g_right_active), "%s", name);
    ed_gui_begin(b, PANEL_RIGHT);
}

void ed_panel_right_blank(void) {
    if (!right_can_show("blank")) return;
    EdBuild b;
    right_begin(&b, "blank");
    ed_gui_panel_buttons(&b, 1, "", 1);
    ed_gui_end(&b, g_ed.el_right_list);
}

static void refresh_panel(EdCtl *c, const cJSON *v) { (void) c; (void) v; g_ed.dirty_panel = 1; }
static void rebuild_sel(EdCtl *c, const cJSON *v) { (void) c; (void) v; if (g_ed.selected) ed_obj_rebuild(g_ed.selected); }

static void on_faces(EdCtl *c, const cJSON *v) {
    // object: e.objConfig.faces, key: index -> A.faces = objConfig.faces; A.regen()
    EditorObject *o = g_ed.selected;
    if (!o) return;
    cJSON *faces = ed_prop_get(o, "faces");
    const int index = c->arg;
    cJSON_ReplaceItemInArray(faces, index, cJSON_CreateBool(ed_json_truthy(v)));
    ed_prop_set(o, "faces", faces);
    cJSON_Delete(faces);
    ed_obj_rebuild(o);
}

static cJSON *g_scratch; // the switches Krunker's GUI shows but ObjectInstance never stores

void ed_panel_right_object(void) {
    if (g_locked[1] && strcmp(g_right_active, "object")) return;
    EditorObject *o = g_ed.selected;
    if (!o) {
        g_locked[1] = g_locked[1] && strcmp(g_right_active, "object") ? g_locked[1] : g_locked[1];
        ed_panel_right_blank();
        return;
    }
    if (!g_scratch) g_scratch = cJSON_CreateObject();
    const int p = o->prefab;
    const EditorPrefab *pf = &editor_prefabs[p];
#define HAS(f) ((pf->flags & (f)) != 0)
    EdBuild b;
    right_begin(&b, "object");
    ed_gui_panel_buttons(&b, 1, pf->label, 0);

    if (HAS(EP_AI)) json_text(&b, "AI Name", g_scratch, "aiName", "BOT", 0, NULL);
    if (!HAS(EP_TOOL) || HAS(EP_BOUNCE)) obj_switch(&b, "Visible", "visible", 0, NULL);
    if (!HAS(EP_TOOL) && !HAS(EP_FORCECOLLISION) && !HAS(EP_NOCOLLISION)) obj_switch(&b, "Collidable", "collidable", 0, NULL);
    if (HAS(EP_EDITPEN)) obj_switch(&b, "Penetrable", "penetrable", 0, NULL);
    if (HAS(EP_CANWALLJUMP)) json_switch(&b, "Wall Jumpable", g_scratch, "canWallJump", 0, NULL);
    if (HAS(EP_BOOSTABLE)) obj_slider(&b, "Boost", "boost", -10, 10, 0.1, 0, NULL);
    if (HAS(EP_BOUNCE)) {
        obj_slider(&b, "Bounce", "bounce", 0.1, 3, 0.1, 0, NULL);
        obj_switch(&b, "Crouch Reduces", "crouch", 0, NULL);
    }
    if (HAS(EP_EDGENOISE)) obj_slider(&b, "Edge Noise", "edgeNoise", -5, 5, 0.1, 0, NULL);
    if (HAS(EP_HASBORDER)) obj_switch(&b, "Border", "border", 0, NULL);
    if (HAS(EP_HASSPIN)) obj_slider(&b, "Spin", "spin", -20, 20, 0, 0, NULL);
    if (!strcmp(pf->name, "CYLINDER")) obj_switch(&b, "Open Ended", "openEnded", 0, NULL);
    if (HAS(EP_CUSTOMDIRECTION)) obj_slider(&b, "Direction", "direction", 0, 3, 1, 0, NULL);
    if (HAS(EP_OPTIONALDIRECTION)) obj_switch(&b, "Force Direction", "forceDir", 0, NULL);
    if (HAS(EP_TEAMABLE)) {
        static const EdOption two[] = {{"Team 1", 0, NULL}, {"Team 2", 1, NULL}};
        static const EdOption three[] = {{"Default", 0, NULL}, {"Team 1", 1, NULL}, {"Team 2", 2, NULL}};
        if (HAS(EP_NODEFAULT)) obj_select(&b, "Team", "team", two, 2, 0, NULL);
        else obj_select(&b, "Team", "team", three, 3, 0, NULL);
    }
    if (HAS(EP_GEN)) obj_slider(&b, "Model Scale", "mSize", 0.1, 100, 0.1, 0, NULL);
    if (HAS(EP_OBJECTIVELINK)) obj_slider(&b, "Objective Id", "objectiveId", -1, 10, 1, 0, NULL);
    ed_gui_rule(&b);

    ed_gui_folder_begin(&b, "Transform", 0, 0, 0, "object.transform");
    obj_vector3(&b, "Position", "pos", NULL);
    obj_vector3(&b, "Rotation", "rot", NULL);
    if (HAS(EP_SCALABLE)) obj_vector3(&b, "Size", "size", NULL);
    ed_gui_folder_end(&b, 0);

    if (!HAS(EP_NOTSTYLEABLE)) {
        ed_gui_folder_begin(&b, "Style", 0, 0, 0, "object.style");
        if (HAS(EP_TEXTURABLE) || HAS(EP_MOVINGTEXTURE)) {
            ed_gui_folder_begin(&b, "Texture", 1, 1, 0, "object.style.texture");
            if (HAS(EP_TEXTURABLE)) {
                // textureList: Default first, then every other texture by formatted name
                EdOption *opts = opts_alloc(EDITOR_TEXTURE_COUNT);
                int n = 0;
                opts[n++] = (EdOption) {"Default", 0, "DEFAULT"};
                for (int i = 0; i < EDITOR_TEXTURE_COUNT; i++) {
                    if (!strcmp(editor_textures[i], "DEFAULT")) continue;
                    const int slot = (int) (&opts[n] - g_opt_pool);
                    opts[n++] = (EdOption) {const_name(editor_textures[i], g_opt_names[slot], sizeof(g_opt_names[slot])), 0, editor_textures[i]};
                }
                obj_select(&b, "Type", "texture", opts, n, 1, refresh_panel);
                if (HAS(EP_ASSETTEXTURES)) {
                    obj_number(&b, "Asset ID", "assetTexture", "Texture Asset ID", 1);
                    extern void ed_select_texture(EdCtl *);
                    ed_gui_button(&b, "Select Custom", ed_select_texture);
                }
                if (HAS(EP_CANSTRETCH)) obj_switch(&b, "Stretch", "texStretch", 1, NULL);
                ed_gui_folder_begin(&b, "Offset", 1, 0, 1, "object.style.texture.offset");
                obj_slider(&b, "X", "texOffX", 0, 1, 0.1, 2, NULL);
                obj_slider(&b, "Y", "texOffY", 0, 1, 0.1, 2, NULL);
                ed_gui_folder_end(&b, 1);
            }
            if (HAS(EP_MOVINGTEXTURE)) {
                static const EdOption axes[] = {{"X", 0, NULL}, {"Y", 1, NULL}};
                ed_gui_folder_begin(&b, "Movement", 1, 0, 1, "object.style.texture.movement");
                obj_slider(&b, "Speed", "movingSpeed", -20, 20, 0.1, 2, NULL);
                obj_select(&b, "Axis", "movingAxis", axes, 2, 2, NULL);
                ed_gui_folder_end(&b, 1);
            }
            ed_gui_folder_end(&b, 1);
        }
        if (HAS(EP_EDITCOLOR)) obj_color(&b, "Color", "color", 0);
        if (HAS(EP_EDITEMISSIVE)) obj_color(&b, "Emissive", "emissive", 0);
        if (HAS(EP_EDITOPAC)) obj_slider(&b, "Opacity", "opacity", 0, 1, 0.1, 0, NULL);
        if (HAS(EP_CUSTOM)) obj_switch(&b, "Texture Smoothing", "texSmooth", 0, NULL);
        if (!HAS(EP_COMPLEX) && !HAS(EP_TOOL)) obj_switch(&b, "Shading", "ambient", 0, NULL);
        if (HAS(EP_EDITFOG)) obj_switch(&b, "Fog", "fog", 0, NULL);
        if (!strcmp(pf->name, "LIGHT_CONE")) obj_switch(&b, "Full Cone", "fullcone", 0, NULL);
        ed_gui_folder_end(&b, 0);
    }
    if (HAS(EP_EDITFACES)) {
        static const char *faces[6] = {"Right", "Left", "Top", "Bottom", "Back", "Front"};
        ed_gui_folder_begin(&b, "Render Faces", 0, 0, 0, "object.faces");
        cJSON *fv = ed_prop_get(o, "faces");
        for (int i = 0; i < 6; i++) {
            EdCtl *c = ed_gui_switch(&b, faces[i], 0);
            char key[4];
            snprintf(key, sizeof(key), "%d", i);
            cJSON *tmp = cJSON_CreateObject();
            cJSON_AddItemToObject(tmp, key, cJSON_Duplicate(cJSON_GetArrayItem(fv, i), 1));
            ed_ctl_bind_json(c, tmp, key);
            c->arg = i;
            finish(&b, c, on_faces);
            c->bind = EB_NONE; // value handled by on_faces
            c->json = NULL;
            cJSON_Delete(tmp);
        }
        cJSON_Delete(fv);
        ed_gui_folder_end(&b, 0);
    }
    if (HAS(EP_LIGHT)) {
        ed_gui_folder_begin(&b, "Lighting", 0, 0, 0, "object.lighting");
        obj_slider(&b, "Intensity", "intensity", 0, 2, 0.1, 0, NULL);
        obj_slider(&b, "Distance", "distance", 0, 1000, 0.1, 0, NULL);
        obj_slider(&b, "Decay", "decay", 0, 2, 0.1, 0, NULL);
        ed_gui_folder_end(&b, 0);
    }

    if (!HAS(EP_NOTTECHNICAL)) {
        // The Technical folder holds the optional controls below; Krunker only adds it when
        // any applies. Build it into a side buffer and keep it when non-empty.
        EdBuild t = b;
        EdBuf saved = b.html;
        edb_init(&t.html);
        b.html = t.html;
        const size_t ctl_marker = 0;
        (void) ctl_marker;
        int any = 0;
        if (!strcmp(pf->name, "DEPOSIT_BOX")) {
            obj_slider(&b, "Withdraw Amount", "withdraw", 0, 100000, 1, 0, NULL);
            obj_slider(&b, "Deposit Amount", "deposit", 0, 100000, 1, 0, NULL);
            any = 1;
        }
        int custom_score = 0;
        if (HAS(EP_CUSTOMSCORE)) custom_score = 1;
        if (!strcmp(pf->name, "CHECK_POINT") || !strcmp(pf->name, "TRIGGER") || HAS(EP_AI)) custom_score = 2;
        if (custom_score == 1) obj_slider(&b, HAS(EP_INTERACT) ? "Amount" : "Score", "score", HAS(EP_INTERACT) || HAS(EP_AI) ? 0 : 1, 10000, 1, 0, NULL);
        if (custom_score == 2) obj_switch(&b, "Single Use", "single", 0, NULL);
        any |= custom_score != 0;
        if (HAS(EP_INTERACT) || HAS(EP_INTERACT2)) {
            if (HAS(EP_INTERACT)) {
                static const EdOption methods[] = {{"Cost Score", 0, NULL}, {"Require Score", 1, NULL}, {"Require Kills", 2, NULL}};
                obj_select(&b, "Method", "method", methods, 3, 0, NULL);
                if (HAS(EP_CANUNDO)) {
                    obj_switch(&b, "Closeable", "closeable", 0, NULL);
                    obj_switch(&b, "Starts Hidden", "startClosed", 0, NULL);
                }
            }
            obj_switch(&b, "No Popup", "hidden", 0, NULL);
            any = 1;
        }
        if (HAS(EP_HASHEALTH)) {
            json_switch(&b, "Destructible", g_scratch, "canDestroy", 0, NULL);
            if (strcmp(pf->name, "GATE")) json_switch(&b, "Start Destroyed", g_scratch, "startDestroyed", 0, NULL);
            if (!strcmp(pf->name, "TRIGGER")) json_switch(&b, "Destroy On Use", g_scratch, "selfDestruct", 0, NULL);
            obj_slider(&b, "Health", "health", 0, 20000, 10, 0, NULL);
            ed_gui_folder_begin(&b, "Respawn", 1, 0, 0, "object.tech.respawn");
            obj_slider(&b, "Timer (s)", "respawnT", 0, 300, 0.1, 1, NULL);
            obj_switch(&b, "Random", "respawnR", 1, NULL);
            ed_gui_folder_end(&b, 1);
            any = 1;
        }
        if (!strcmp(pf->name, "WEAPON_PICKUP")) {
            EdOption *opts = opts_alloc(32);
            int n = 0, wi = 0;
            const cJSON *w;
            cJSON_ArrayForEach(w, editor_data_get("weapons")) {
                if (ed_json_str(w, "src", NULL)) opts[n++] = (EdOption) {ed_json_str(w, "name", ""), wi, NULL};
                wi++;
            }
            ed_gui_folder_begin(&b, "Weapon", 1, 0, 0, "object.tech.weapon");
            obj_select(&b, "Type", "weaponId", opts, n, 1, rebuild_sel);
            obj_switch(&b, "Replaceable", "weaponReplace", 1, NULL);
            obj_switch(&b, "Random", "randWep", 1, NULL);
            ed_gui_folder_end(&b, 1);
            any = 1;
        }
        if (!strcmp(pf->name, "TRIGGER") || HAS(EP_AI)) {
            const cJSON *trig = editor_data_get("triggers");
            const cJSON *events = cJSON_GetObjectItemCaseSensitive(trig, "events");
            const cJSON *actions = cJSON_GetObjectItemCaseSensitive(trig, "actions");
            if (HAS(EP_AI)) json_switch(&b, "Can Trigger", g_scratch, "canTrigger", 0, NULL);
            EdOption *ev = opts_alloc(cJSON_GetArraySize(events));
            int n = 0, i = 0;
            const cJSON *it;
            cJSON_ArrayForEach(it, events) {
                const char *name = cJSON_IsString(it) ? it->valuestring : "";
                const int ai_event = strstr(name, "onAI") != NULL;
                if ((HAS(EP_AI) && ai_event) || (!strcmp(pf->name, "TRIGGER") && !ai_event)) ev[n++] = (EdOption) {name, i, NULL};
                i++;
            }
            obj_select(&b, "Event", "event", ev, n, 0, NULL);
            obj_slider(&b, "Event Value", "constantEvent", 0, 10000, 1, 0, NULL);
            EdOption *ac = opts_alloc(cJSON_GetArraySize(actions));
            n = 0;
            i = 0;
            cJSON_ArrayForEach(it, actions) ac[n++] = (EdOption) {ed_json_str(it, "name", ""), i++, NULL};
            obj_select(&b, "Action", "action", ac, n, 0, refresh_panel);
            cJSON *action = ed_prop_get(o, "action");
            const cJSON *cur = cJSON_GetArrayItem(actions, (int) cJSON_GetNumberValue(action));
            cJSON_Delete(action);
            if (cur && !strcmp(ed_json_str(cur, "type", ""), "Text")) obj_text(&b, "Value", "constantTxt", "Text", 0, NULL);
            else obj_slider(&b, "Value", "constant", -10000, 10000, 1, 0, NULL);
            json_slider(&b, "Action Chance (%)", g_scratch, "chance", 0, 100, 1, 0, NULL);
            any = 1;
        }
        const int iface = strcmp(pf->name, "WEAPON_PICKUP") && HAS(EP_CANINTERFACE);
        if (iface || HAS(EP_CANTARGETINTERFACE)) {
            ed_gui_folder_begin(&b, "Interface", 1, 0, 0, "object.tech.interface");
            if (iface) obj_slider(&b, "ID", "interfaceId", 0, ed_json_num(editor_data_json(), "interfaceIDMax", 1000), 1, 1, NULL);
            if (HAS(EP_CANTARGETINTERFACE)) obj_slider(&b, "Target", "interfaceIdT", 0, 1000, 1, 1, NULL);
            ed_gui_folder_end(&b, 1);
            any = 1;
        }
        if (!strcmp(pf->name, "TELEPORTER")) {
            static const EdOption nodes[] = {{"Sending", 0, NULL}, {"Receiving", 1, NULL}, {"Both", 2, NULL}};
            obj_select(&b, "Node", "node", nodes, 3, 0, NULL);
            obj_slider(&b, "Channel", "channel", 0, 100, 1, 0, NULL);
            obj_switch(&b, "Stop Momentum", "momentum", 0, NULL);
            any = 1;
        }
        if (HAS(EP_CANBEDYNAMIC)) {
            ed_gui_folder_begin(&b, "Dynamic", 1, 0, 0, "object.tech.dynamic");
            obj_switch(&b, "Enabled", "dynamic", 1, NULL);
            obj_slider(&b, "Gravity", "gravity", 0, 30, 1, 1, NULL);
            ed_gui_folder_end(&b, 1);
            any = 1;
        }
        EdBuf inner = b.html;
        b.html = saved;
        if (any) {
            ed_gui_folder_begin(&b, "Technical", 0, 0, 0, "object.tech");
            edb_add(&b.html, inner.buf ? inner.buf : "");
            ed_gui_folder_end(&b, 0);
        }
        edb_free(&inner);
    }

    if (!strcmp(pf->name, "BILLBOARD")) {
        ed_gui_folder_begin(&b, "Billboard", 0, 0, 0, "object.billboard");
        obj_slider(&b, "Poster", "poster", 1, 6, 1, 0, NULL);
        obj_switch(&b, "Show AD", "showAd", 0, NULL);
        ed_gui_folder_end(&b, 0);
    }
    if (!strcmp(pf->name, "PARTICLES")) {
        int n;
        ed_gui_folder_begin(&b, "Particles", 0, 0, 0, "object.particles");
        EdOption *ids = opts_from_names(editor_data_get("particleIDS"), &n);
        for (int i = 0; i < n; i++) {
            const int slot = (int) (&ids[i] - g_opt_pool);
            ids[i].name = const_name(ids[i].name, g_opt_names[slot], sizeof(g_opt_names[slot]));
        }
        obj_select(&b, "Texture", "part", ids, n, 0, NULL);
        EdOption *blend = opts_from_names(editor_data_get("particleBlending"), &n);
        for (int i = 0; i < n; i++) {
            const int slot = (int) (&blend[i] - g_opt_pool);
            blend[i].name = const_name(blend[i].name, g_opt_names[slot], sizeof(g_opt_names[slot]));
        }
        obj_select(&b, "Blending", "partB", blend, n, 0, NULL);
        obj_slider(&b, "Count", "partC", 0, 300, 1, 0, NULL);
        obj_slider(&b, "Size", "partS", 0, 30, 0.1, 0, NULL);
        obj_slider(&b, "Scale", "partSC", -10, 10, 0.1, 0, NULL);
        ed_gui_folder_begin(&b, "Movement", 1, 0, 0, "object.particles.movement");
        obj_slider(&b, "Speed", "partSP", -10, 10, 0.1, 1, NULL);
        obj_slider(&b, "Deceleration", "partD", -1, 1, 0.1, 1, NULL);
        obj_slider(&b, "Gravity", "partG", -10, 10, 0.1, 1, NULL);
        ed_gui_folder_end(&b, 1);
        ed_gui_folder_end(&b, 0);
    }
    if (HAS(EP_HASTEXT)) {
        static const EdOption align[] = {{"Left", 0, NULL}, {"Center", 1, NULL}, {"Right", 2, NULL}};
        ed_gui_folder_begin(&b, "Text", 0, 0, 0, "object.text");
        obj_text(&b, "Content", "text", "Content", 0, rebuild_sel);
        obj_select(&b, "Align", "tAlign", align, 3, 0, NULL);
        ed_gui_folder_begin(&b, "Font", 1, 0, 0, "object.text.font");
        obj_slider(&b, "Size", "fsize", 10, 120, 1, 1, NULL);
        obj_color(&b, "Color", "fcolor", 1);
        ed_gui_folder_end(&b, 1);
        ed_gui_folder_begin(&b, "Background", 1, 0, 0, "object.text.background");
        obj_color(&b, "Color", "bcolor", 1);
        obj_switch(&b, "Visible", "backVisible", 1, NULL);
        ed_gui_folder_end(&b, 1);
        ed_gui_folder_end(&b, 0);
    }
    if (p == ED_PREFAB_SPAWN_POINT) {
        ed_gui_folder_begin(&b, "Spawn Point", 0, 0, 0, "object.spawn");
        obj_switch(&b, "Starting Area", "cmpSpwn", 0, rebuild_sel);
        ed_gui_folder_end(&b, 0);
    }
    if (!strcmp(pf->name, "LIQUID")) {
        ed_gui_folder_begin(&b, "Wave", 0, 0, 0, "object.wave");
        ed_gui_folder_begin(&b, "Liquid", 1, 0, 0, "object.wave.liquid");
        obj_slider(&b, "Speed", "wSpeed", 0, 25, 0.1, 1, NULL);
        obj_slider(&b, "Height", "wHeight", 0, 5, 0.1, 1, NULL);
        ed_gui_folder_end(&b, 1);
        ed_gui_folder_begin(&b, "Depth", 1, 0, 0, "object.wave.depth");
        obj_slider(&b, "X", "tilesX", 2, 100, 1, 1, NULL);
        obj_slider(&b, "Z", "tilesZ", 2, 100, 1, 1, NULL);
        ed_gui_folder_end(&b, 1);
        ed_gui_folder_end(&b, 0);
    }
    if (!strcmp(pf->name, "SOUND_EMITTER")) {
        int n;
        ed_gui_folder_begin(&b, "Sound", 0, 0, 0, "object.sound");
        EdOption *snd = opts_alloc(cJSON_GetArraySize(editor_data_get("emitterSounds")));
        n = 0;
        const cJSON *it;
        cJSON_ArrayForEach(it, editor_data_get("emitterSounds")) {
            snd[n] = (EdOption) {cJSON_IsString(it) ? it->valuestring : "", n, NULL};
            n++;
        }
        obj_select(&b, "Type", "snd", snd, n, 0, NULL);
        obj_slider(&b, "Volume", "vol", 0.05, 2, 0.01, 0, NULL);
        obj_slider(&b, "Rate", "rate", 0.5, 4, 0.1, 0, NULL);
        obj_slider(&b, "Reference Distance", "dist", 0, 5000, 1, 0, NULL);
        obj_slider(&b, "Roll-off", "roll", 0, 1, 0.01, 0, NULL);
        ed_gui_folder_end(&b, 0);
    }
#undef HAS
    ed_gui_end(&b, g_ed.el_right_list);
}

void ed_select_texture(EdCtl *c) {
    (void) c;
    editor_message("Texture assets come from the Krunker asset server; type an Asset ID instead.");
}

// environment (Component > Environment)
static void env_changed(EdCtl *c, const cJSON *v) {
    (void) c; (void) v;
    editor_apply_lighting();
}

static void env_color(EdBuild *b, const char *name, const char *key, const int sub) {
    // A[C] getters: stringifyColor(mapConfig[C]); setters store parseColor (a number)
    cJSON *tmp = cJSON_CreateObject();
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(g_ed.map_config, key);
    char hex[16] = "#000000";
    if (cJSON_IsNumber(v)) snprintf(hex, sizeof(hex), "#%06x", (int) cJSON_GetNumberValue(v) & 0xffffff);
    else if (cJSON_IsString(v)) snprintf(hex, sizeof(hex), "%s", v->valuestring);
    cJSON_AddStringToObject(tmp, key, hex);
    EdCtl *c = ed_gui_color(b, name, sub);
    ed_ctl_bind_json(c, tmp, key);
    finish(b, c, NULL);
    cJSON_Delete(tmp);
    c->bind = EB_NONE;
    c->json = NULL;
    c->arg = sub;
    snprintf(c->path, sizeof(c->path), "%s", key);
    extern void ed_env_color_changed(EdCtl *, const cJSON *);
    c->on_change = ed_env_color_changed;
}

void ed_env_color_changed(EdCtl *c, const cJSON *v) {
    if (!cJSON_IsString(v)) return;
    const int color = (int) strtol(v->valuestring + (v->valuestring[0] == '#'), NULL, 16);
    ed_json_set_num(g_ed.map_config, c->path, color);
    editor_apply_lighting();
}

static void on_sky_mode(EdCtl *c, const cJSON *v) {
    (void) c;
    const int dome = (int) cJSON_GetNumberValue(v) != 0;
    ed_json_set_bool(g_ed.map_config, "skyDome", dome);
    html_dom_set_display(html_dom_by_id("sky_0"), dome ? "none" : "");
    html_dom_set_display(html_dom_by_id("sky_1"), dome ? "" : "none");
    editor_apply_lighting();
}

static void on_cloud(EdCtl *c, const cJSON *v) {
    (void) c;
    html_dom_set_display(html_dom_by_id("cloud"), ed_json_truthy(v) ? "" : "none");
    editor_apply_lighting();
}

static void div_begin(EdBuild *b, const char *id, const int show) {
    // _build "div": `"<div id=" + id + " " + (show ? null : 'style="display: none"') + ">"`
    edb_addf(&b->html, "<div id=%s %s>", id, show ? "null" : "style=\"display: none\"");
}

void ed_panel_right_environment(void) {
    if (!right_can_show("environment")) return;
    EdBuild b;
    right_begin(&b, "environment");
    ed_gui_panel_buttons(&b, 1, "Environment", 0);
    cJSON *mc = g_ed.map_config;
    const int dome = ed_json_truthy(cJSON_GetObjectItemCaseSensitive(mc, "skyDome"));
    static const EdOption sky[] = {{"Solid", 0, NULL}, {"Gradient", 1, NULL}};
    cJSON *tmp = cJSON_CreateObject();
    cJSON_AddNumberToObject(tmp, "skyDome", dome);
    EdCtl *c = ed_gui_select(&b, "Sky", sky, 2, 0);
    ed_ctl_bind_json(c, tmp, "skyDome");
    finish(&b, c, on_sky_mode);
    c->bind = EB_NONE;
    cJSON_Delete(tmp);
    div_begin(&b, "sky_0", !dome);
    env_color(&b, "Color", "sky", 1);
    edb_add(&b.html, "</div>\n");
    div_begin(&b, "sky_1", dome);
    ed_gui_folder_begin(&b, "Color", 1, 0, 0, "env.sky.color");
    env_color(&b, "Top", "skyDomeCol0", 2);
    env_color(&b, "Middle", "skyDomeCol1", 2);
    env_color(&b, "Bottom", "skyDomeCol2", 2);
    ed_gui_folder_end(&b, 1);
    json_switch(&b, "Cloud Texture", mc, "skyDomeTex", 1, on_cloud);
    div_begin(&b, "cloud", ed_json_truthy(cJSON_GetObjectItemCaseSensitive(mc, "skyDomeTex")));
    env_color(&b, "Emissive", "skyDomeEmis", 2);
    json_slider(&b, "Speed", mc, "skyDomeMovT", -20, 20, 0.1, 2, env_changed);
    static const EdOption axes[] = {{"X", 0, NULL}, {"Y", 1, NULL}};
    json_select(&b, "Axis", mc, "skyDomeMovD", axes, 2, 2, env_changed);
    edb_add(&b.html, "</div>\n</div>\n");
    ed_gui_rule(&b);

    ed_gui_folder_begin(&b, "Ambient", 0, 0, 0, "env.ambient");
    static const EdOption scapes[] = {{"Default", 1, NULL}, {"City", 2, NULL}, {"Desert", 3, NULL}, {"Market", 4, NULL}, {"Scary", 5, NULL},
                                      {"Storm", 6, NULL}, {"Purgatory", 7, NULL}, {"Black Market", 8, NULL}, {"Tomb", 11, NULL}};
    json_select(&b, "Sound", mc, "ambInd", scapes, 9, 0, NULL);
    json_slider(&b, "Intensity", mc, "ambientI", 0.1, 3, 0.1, 0, env_changed);
    env_color(&b, "Color", "ambient", 0);
    ed_gui_folder_end(&b, 0);

    ed_gui_folder_begin(&b, "Light", 0, 0, 0, "env.light");
    env_color(&b, "Color", "light", 0);
    json_slider(&b, "Distance", mc, "lightD", 10, 5000, 1, 0, env_changed);
    json_slider(&b, "Intensity", mc, "lightI", 0.1, 3, 0.1, 0, env_changed);
    ed_gui_folder_begin(&b, "Angle", 1, 0, 0, "env.light.angle");
    json_slider(&b, "X", mc, "sunAngX", 0, 360, 1, 1, env_changed);
    json_slider(&b, "Y", mc, "sunAngY", 0, 360, 1, 1, env_changed);
    ed_gui_folder_end(&b, 1);
    ed_gui_folder_end(&b, 0);

    ed_gui_folder_begin(&b, "Shadows", 0, 0, 0, "env.shadows");
    json_slider(&b, "Resolution", mc, "shadowR", 128, 4096, 1, 0, NULL);
    json_slider(&b, "Distance", mc, "shadowD", 128, 4096, 1, 0, NULL);
    ed_gui_folder_end(&b, 0);

    ed_gui_folder_begin(&b, "Weather", 0, 0, 0, "env.weather");
    ed_gui_folder_begin(&b, "Fog", 1, 0, 0, "env.weather.fog");
    env_color(&b, "Color", "fog", 2);
    json_slider(&b, "Distance", mc, "fogD", 10, 15000, 1, 2, env_changed);
    ed_gui_folder_end(&b, 1);
    ed_gui_folder_end(&b, 0);
    ed_gui_end(&b, g_ed.el_right_list);
}

// Terrain / Zone / Customize / Tools / Grouping: the panels exist so the menu entries
// open something; their generators are not ported yet (see the report in editor.c).
static void right_stub(const char *name, const char *label, const char *text) {
    if (!right_can_show(name)) return;
    EdBuild b;
    right_begin(&b, name);
    ed_gui_panel_buttons(&b, 1, label, 0);
    edb_add(&b.html, "<div class=\"section\" style=\"white-space:normal;font-size:16px;margin:10px 0\">");
    edb_add_escaped(&b.html, text);
    edb_add(&b.html, "</div>\n");
    ed_gui_end(&b, g_ed.el_right_list);
}

static void tools_scale_map(EdCtl *c) {
    (void) c;
    const double sx = ed_json_num(g_ed.advanced, "scaleMapX", 1), sy = ed_json_num(g_ed.advanced, "scaleMapY", 1), sz = ed_json_num(g_ed.advanced, "scaleMapZ", 1);
    for (size_t i = 0; i < g_ed.count; i++) {
        EditorObject *o = g_ed.objects[i];
        const vec3 p = ed_obj_vec(o, "p", (vec3) {0});
        const vec3 s = ed_obj_vec(o, "s", (vec3) {1, 1, 1});
        ed_obj_set_vec(o, "p", (vec3) {(float) (p.x * sx), (float) (p.y * sy), (float) (p.z * sz)});
        if (editor_prefab_has(o->prefab, EP_SCALABLE)) ed_obj_set_vec(o, "s", (vec3) {(float) (s.x * sx), (float) (s.y * sy), (float) (s.z * sz)});
        ed_obj_rebuild(o);
    }
    g_ed.dirty_panel = 1;
}

static void tools_reflect(EdCtl *c) {
    (void) c;
    // reflectMap: mirror every object across the chosen axis through the origin
    const int dir = (int) ed_json_num(g_ed.advanced, "reflectDir", 0);
    for (size_t i = 0; i < g_ed.count; i++) {
        EditorObject *o = g_ed.objects[i];
        vec3 p = ed_obj_vec(o, "p", (vec3) {0});
        vec3 r = ed_obj_vec(o, "r", (vec3) {0});
        if (dir == 0) { p.x = -p.x; r.y = -r.y; r.z = -r.z; }
        else { p.z = -p.z; r.y = -r.y; r.x = -r.x; }
        ed_obj_set_vec(o, "p", p);
        ed_obj_set_vec(o, "r", r);
        ed_obj_rebuild(o);
    }
}

static void tools_colorize(EdCtl *c) {
    (void) c;
    // colorizeR: a random colour per object
    for (size_t i = 0; i < g_ed.count; i++) {
        EditorObject *o = g_ed.objects[i];
        if (!editor_prefab_has(o->prefab, EP_EDITCOLOR)) continue;
        char hex[16];
        snprintf(hex, sizeof(hex), "#%06X", (unsigned) (rand() & 0xffffff));
        cJSON *v = cJSON_CreateString(hex);
        ed_prop_set(o, "color", v);
        cJSON_Delete(v);
        ed_obj_rebuild(o);
    }
}

static void panel_tools(void) {
    if (!right_can_show("tools")) return;
    EdBuild b;
    right_begin(&b, "tools");
    ed_gui_panel_buttons(&b, 1, "Tools", 0);
    ed_gui_folder_begin(&b, "Scale Map", 0, 0, 0, "tools.scale");
    json_slider(&b, "X", g_ed.advanced, "scaleMapX", 0.1, 10, 0.1, 0, NULL);
    json_slider(&b, "Y", g_ed.advanced, "scaleMapY", 0.1, 10, 0.1, 0, NULL);
    json_slider(&b, "Z", g_ed.advanced, "scaleMapZ", 0.1, 10, 0.1, 0, NULL);
    ed_gui_button(&b, "Scale", tools_scale_map);
    ed_gui_folder_end(&b, 0);
    ed_gui_folder_begin(&b, "Reflect Map", 0, 1, 0, "tools.reflect");
    static const EdOption dirs[] = {{"X", 0, NULL}, {"Z", 1, NULL}};
    json_select(&b, "Direction", g_ed.advanced, "reflectDir", dirs, 2, 0, NULL);
    ed_gui_button(&b, "Reflect", tools_reflect);
    ed_gui_folder_end(&b, 0);
    ed_gui_folder_begin(&b, "Colorize", 0, 1, 0, "tools.colorize");
    ed_gui_button(&b, "Random", tools_colorize);
    ed_gui_folder_end(&b, 0);
    ed_gui_end(&b, g_ed.el_right_list);
}

void ed_panel_right_show(const char *name) {
    // e.resizable.restore(panel.right.restore)
    g_ed.panel_hidden[1] = 0;
    if (!strcmp(name, "environment")) ed_panel_right_environment();
    else if (!strcmp(name, "object")) ed_panel_right_object();
    else if (!strcmp(name, "tools")) panel_tools();
    else if (!strcmp(name, "terrain")) right_stub("terrain", "Terrain", "Terrain generation (libs/Terrain.js) is not ported yet; the terrain fields of imported maps are kept and exported unchanged.");
    else if (!strcmp(name, "zone")) right_stub("zone", "Zone", "The battle-royale zone (libs/Zone.js) is not ported yet; zone fields are kept on export.");
    else if (!strcmp(name, "customize")) right_stub("customize", "Customize", "Map customisation (team names, colours) is not ported yet.");
    else if (!strcmp(name, "grouping")) right_stub("grouping", "Grouping", "Select a group placeholder: moving, rotating (Y) and scaling it moves its members.");
    else ed_panel_right_blank();
}

void ed_panel_refresh_all(void) {
    ed_panel_left_show();
    if (!strcmp(g_right_active, "object")) ed_panel_right_object();
    else ed_panel_right_show(g_right_active);
}

// ---------------------------------------------------------------------------
// windows (window.windows[n].gen into #menuWindow)

static void win_settings_changed(EdCtl *c, const cJSON *v) {
    (void) c; (void) v;
    editor_settings_save();
}

static void win_reset_speed(EdCtl *c) {
    (void) c;
    ed_json_set_num(g_ed.settings, "speedNormal", 70);
    ed_json_set_num(g_ed.settings, "speedSprint", 180);
    editor_settings_save();
    ed_window_show(1);
}

static void win_header(EdBuild *b, const char *title, const int back) {
    edb_add(&b->html, "<div class=\"windowHeader\">\n");
    if (back) edb_add(&b->html, "                <div style=\"cursor: pointer; margin-right:5px\"onclick=\"showWindow(3)\">\n"
                                "                    <i class=\"material-icons\" style=\"font-weight:bold; transform:rotate(-135deg);\">call_made</i>\n"
                                "                </div>\n");
    edb_addf(&b->html, "                <div>%s</div>\n            </div>", title);
}

static int g_controls_tab;

static void window_controls(EdBuild *b) {
    edb_add(&b->html, "<div style='margin-bottom:15px'>");
    win_header(b, "Controls", 1);
    edb_add(&b->html, "\n            <div class='menuTabsNew'>");
    static const char *tabs[2] = {"Default", "Advanced"};
    for (int i = 0; i < 2; i++) edb_addf(&b->html, "<div class=\"menuTabNew %s\" onclick=\"windows[0].switchTab(%d)\">%s</div>", g_controls_tab == i ? " tabANew" : "", i, tabs[i]);
    edb_add(&b->html, "</div></div>\n            <div class=\"windowBody\">");
    if (g_controls_tab == 0) ed_gui_rule(b);
    cJSON *s = g_ed.settings;
    if (g_controls_tab == 0) {
        ed_gui_folder_begin(b, "Movement", 0, 0, 0, "controls.movement");
        json_key_extra(b, "Fly forward", s, "fowardKey", 1, 0, win_settings_changed, arrow_html("up"));
        json_key_extra(b, "Fly backwards", s, "backwardKey", 1, 0, win_settings_changed, arrow_html("down"));
        json_key_extra(b, "Fly left", s, "leftKey", 1, 0, win_settings_changed, arrow_html("left"));
        json_key_extra(b, "Fly right", s, "rightKey", 1, 0, win_settings_changed, arrow_html("right"));
        json_key(b, "Fly up", s, "upKey", 1, 0, win_settings_changed);
        json_key(b, "Fly down", s, "downKey", 1, 0, win_settings_changed);
        key_line(b, "Hold to fly faster", "Shift");
        ed_gui_folder_begin(b, "Speed", 1, 0, 0, "controls.speed");
        json_number(b, "Normal", s, "speedNormal", "", 0, win_settings_changed);
        json_number(b, "Sprinting", s, "speedSprint", "", 0, win_settings_changed);
        ed_gui_button_line(b, "Reset Speed", "Reset", win_reset_speed, 0);
        ed_gui_folder_end(b, 1);
        ed_gui_folder_end(b, 0);
        ed_gui_folder_begin(b, "Transform", 0, 0, 0, "controls.transform");
        key_line(b, "Translate", "1");
        key_line(b, "Rotate", "2");
        key_line(b, "Scale", "3");
        key_line(b, "Toggle World / Local space", "`");
        ed_gui_folder_end(b, 0);
        ed_gui_folder_begin(b, "Edit", 0, 0, 0, "controls.edit");
        key_line(b, "Undo", "Ctrl + Z");
        key_line(b, "Redo", "Ctrl + Y or Shift + Ctrl + Z");
        ed_gui_folder_end(b, 0);
        ed_gui_folder_begin(b, "Object", 0, 0, 0, "controls.object");
        json_key(b, "Face Selection Mode", s, "faceModeKey", 1, 0, win_settings_changed);
        json_key(b, "Group Selection Mode", s, "groupModeKey", 1, 0, win_settings_changed);
        key_line(b, "Delete element", "Del or Backspace");
        key_line(b, "Toggle snapping", "Control");
        key_line(b, "Toggle collidable", "Shift + C");
        key_line(b, "Toggle visibilty", "Shift + V");
        ed_gui_folder_end(b, 0);
    } else {
        key_line(b, "Create placeholder", "P");
        key_line(b, "Hitbox fixer", "Shift + F");
        key_line(b, "Flip x/z size", "F");
        key_line(b, "Quick Add", "X");
        ed_gui_rule(b);
        ed_gui_folder_begin(b, "Group", 0, 0, 0, "controls.group");
        key_line(b, "Remove all groups", "Shift + Alt + G");
        key_line(b, "Create group / Remove group if exist", "Shift + G");
        key_line(b, "Duplicate Group / Object", "Shift + R");
        key_line(b, "Copy group", "Ctrl + C");
        key_line(b, "Paste group", "Ctrl + V");
        ed_gui_folder_end(b, 0);
        // blueprint.file is assigned twice in v3.1.1: only the second File folder survives
        ed_gui_folder_begin(b, "File", 0, 0, 0, "controls.file");
        key_line(b, "Paste preset JSON", "Shift + Click (w/ Objectpainter)");
        ed_gui_folder_end(b, 0);
    }
    edb_add(&b->html, "</div>");
}

static void on_grid(EdCtl *c, const cJSON *v) { (void) c; (void) v; editor_settings_save(); }

static void on_toggle_div(EdCtl *c, const cJSON *v) {
    // the switches that show/hide a "div" of dependent settings (history limit, grid, barrier)
    editor_settings_save();
    const int on = ed_json_truthy(v);
    html_dom_set_display(html_dom_by_id(c->path), (c->arg ? !on : on) ? "" : "none");
}

static void open_controls(EdCtl *c) { (void) c; editor_show_window(1); }

static void reset_settings(EdCtl *c, const char *const *keys, const int n) {
    (void) c;
    cJSON *def = cJSON_Parse(editor_default_settings());
    for (int i = 0; i < n; i++) {
        cJSON_DeleteItemFromObjectCaseSensitive(g_ed.settings, keys[i]);
        cJSON_AddItemToObject(g_ed.settings, keys[i], cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(def, keys[i]), 1));
    }
    cJSON_Delete(def);
    editor_settings_save();
    editor_show_window(3);
}

static void grid_reset(EdCtl *c) {
    static const char *const k[] = {"gridOpacity", "gridSize", "gridSpacing", "gridVisibility"};
    reset_settings(c, k, 4);
}

static void db_reset(EdCtl *c) {
    static const char *const k[] = {"dbOpacity", "dbVisibility"};
    reset_settings(c, k, 2);
}

static void snapping_reset(EdCtl *c) {
    static const char *const k[] = {"translationSnapping", "rotationSnapping", "scaleSnapping"};
    reset_settings(c, k, 3);
}

static void reset_all(EdCtl *c) {
    (void) c;
    cJSON_Delete(g_ed.settings);
    g_ed.settings = cJSON_Parse(editor_default_settings());
    editor_settings_save();
    editor_show_window(3);
}

static int g_settings_tab;

static EdCtl *div_switch(EdBuild *b, const char *name, const char *key, const char *div, const int invert) {
    EdCtl *c = json_switch(b, name, g_ed.settings, key, 0, on_toggle_div);
    snprintf(c->path, sizeof(c->path), "%s", div);
    c->arg = invert;
    return c;
}

static void window_settings(EdBuild *b) {
    // windows[2]: tabs General / Display / Object / Quick GUI (GUI._window.settings.*)
    static const char *tabs[4] = {"General", "Display", "Object", "Quick GUI"};
    EdCtl *reset = ed_gui_ctl(b->panel, EG_BUTTON);
    reset->action = reset_all;
    edb_addf(&b->html, "<div style='margin-bottom:15px'><div class=\"windowHeader\"><div>Settings</div>\n"
                       "            <div style=\"cursor:pointer;margin-top:5px;font-size:14px;float:right;color:#FFF;user-select:none\" onclick=\"GUI.action(%d, this)\">Reset All</div>\n"
                       "            </div><div class='menuTabsNew'>\n            ", reset->id);
    for (int i = 0; i < 4; i++) edb_addf(&b->html, "<div class=\"menuShortTabNew %s\" onclick=\"windows[2].switchTab(%d)\">%s</div>", g_settings_tab == i ? " tabANew" : "", i, tabs[i]);
    edb_add(&b->html, "</div></div>\n            <div class=\"windowBody\">");
    cJSON *s = g_ed.settings;
    if (g_settings_tab == 0) {
        ed_gui_button_line(b, "Controls", "Open Controls", open_controls, 0);
        json_slider(b, "Auto Backup", s, "autoBackup", 0, 25, 1, 0, on_grid);
        json_slider(b, "UI Scale", s, "uiScale", 0.5, 1.5, 0.1, 0, on_grid);
        json_switch(b, "Pointer Lock", s, "pointerLock", 0, on_grid);
        json_switch(b, "Disable Warnings", s, "disableWarnings", 0, on_grid);
        ed_gui_rule(b);
        ed_gui_folder_begin(b, "Render", 0, 1, 0, "settings.render");
        json_slider(b, "Resolution", s, "resMltV", 0.1, 2, 0.1, 0, on_grid);
        json_switch(b, "Anti-aliasing", s, "antiAlias", 0, on_grid);
        ed_gui_folder_end(b, 0);
        ed_gui_folder_begin(b, "History", 0, 1, 0, "settings.history");
        div_switch(b, "Disable", "disableHistory", "grid", 1);
        div_begin(b, "grid", !editor_setting_bool("disableHistory"));
        json_slider(b, "Limit", s, "historyLimit", 0, 1000, 1, 1, on_grid);
        edb_add(&b->html, "</div>\n");
        ed_gui_folder_end(b, 0);
        ed_gui_folder_begin(b, "Game", 0, 1, 0, "settings.game");
        json_switch(b, "Show Real Hitbox", s, "showRealHitbox", 0, on_grid);
        ed_gui_folder_end(b, 0);
    } else if (g_settings_tab == 1) {
        json_switch(b, "Dark Mode", s, "darkMode", 0, on_grid);
        ed_gui_rule(b);
        ed_gui_folder_begin(b, "Grid", 0, 0, 0, "settings.grid");
        div_switch(b, "Visible", "gridVisibility", "grid", 0);
        div_begin(b, "grid", editor_setting_bool("gridVisibility"));
        json_slider(b, "Opacity", s, "gridOpacity", 0.05, 1, 0.05, 1, on_grid);
        json_number(b, "Size", s, "gridSize", "", 1, on_grid);
        json_number(b, "Spacing", s, "gridSpacing", "", 1, on_grid);
        edb_add(&b->html, "</div>\n");
        ed_gui_button_line(b, "Reset Grid", "Reset", grid_reset, 0);
        ed_gui_folder_end(b, 0);
        ed_gui_folder_begin(b, "Death Barrier", 0, 0, 0, "settings.db");
        div_switch(b, "Visible", "dbVisibility", "deathBarrier", 0);
        div_begin(b, "deathBarrier", editor_setting_bool("dbVisibility"));
        json_slider(b, "Opacity", s, "dbOpacity", 0.05, 1, 0.05, 1, on_grid);
        edb_add(&b->html, "</div>\n");
        ed_gui_button_line(b, "Reset Barrier", "Reset", db_reset, 0);
        ed_gui_folder_end(b, 0);
    } else if (g_settings_tab == 2) {
        json_switch(b, "Trigger Connections", s, "triggerCons", 0, on_grid);
        json_switch(b, "Select through invisible objects [ALT]", s, "selectThroughInvis", 0, on_grid);
        json_switch(b, "Object Border Visuals", s, "borderVisuals", 0, on_grid);
        json_switch(b, "Highlight", s, "objectHighlight", 0, on_grid);
        json_switch(b, "Create Near", s, "createObjectNear", 0, on_grid);
        ed_gui_button_line(b, "Lightcone Boundingbox", "Toggle", NULL, 0);
        ed_gui_rule(b);
        ed_gui_folder_begin(b, "Config", 0, 0, 0, "settings.config");
        json_switch(b, "Anti Radians", s, "degToRad", 0, on_grid);
        json_switch(b, "Preserve Folders", s, "preserveFolderState", 0, on_grid);
        ed_gui_folder_end(b, 0);
        ed_gui_folder_begin(b, "Snapping", 0, 0, 0, "settings.snapping");
        json_slider(b, "Translation", s, "translationSnapping", 0.001, 25, 0.001, 0, on_grid);
        json_slider(b, "Rotation", s, "rotationSnapping", 0.001, 45, 0.001, 0, on_grid);
        json_slider(b, "Scale", s, "scaleSnapping", 0.001, 25, 0.001, 0, on_grid);
        ed_gui_button_line(b, "Reset Snapping", "Reset", snapping_reset, 0);
        ed_gui_folder_end(b, 0);
    } else {
        static const EdOption open_on[] = {{"Middle Click", 0, NULL}, {"Right Click", 1, NULL}};
        div_switch(b, "Enabled", "quickGUI", "quickGUISettings", 0);
        div_begin(b, "quickGUISettings", editor_setting_bool("quickGUI"));
        json_select(b, "Open On", s, "quickGUIOpen", open_on, 2, 1, on_grid);
        edb_add(&b->html, "</div>\n");
    }
    edb_add(&b->html, "</div>");
}

static int h_settings_tab(void *user, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) user; (void) ev;
    const int tab = argc >= 1 ? (int) args[0].num : 0;
    if (tab != g_settings_tab) {
        g_settings_tab = tab;
        editor_show_window(3);
    }
    return 1;
}

static void quick_add_click(EdCtl *c) {
    editor_close_window();
    if (c->arg == -2) ed_obj_create_placeholder();
    else ed_obj_create(c->arg);
}

static void window_quick_add(EdBuild *b) {
    // windows[3] "Quick Add": settings.quickAddButtons (8 slots)
    edb_add(&b->html, "<div class='windowHeader'><div>Quick Add</div><input type='text' id='quickSearch' class='smlInput' autofocus='' placeholder='Search'></div>");
    edb_add(&b->html, "<div class='buttonGrid'>");
    static const struct { const char *name, *desc; int item; } buttons[] = {
        {"Cube", "Basic building block", ED_PREFAB_CUBE},
        {"Spawn Point", "Allows you to place where the player spawns", ED_PREFAB_SPAWN_POINT},
        {"Custom Asset", "Import community made assets", ED_PREFAB_CUSTOM_ASSET},
        {"Placeholder", "Stretch over objects to form a group", -2},
    };
    for (int i = 0; i < 8; i++) {
        if (i < 4) {
            EdCtl *c = ed_gui_ctl(b->panel, EG_BUTTON);
            c->arg = buttons[i].item;
            c->action = quick_add_click;
            edb_addf(&b->html, "<div class='quickAddButton' onclick='GUI.action(%d, this)'><div class='quickAddName'>%s</div><div class='quickAddDesc'>%s</div></div>",
                     c->id, buttons[i].name, buttons[i].desc);
        } else {
            edb_add(&b->html, "<div class='quickAddButton blank'></div>");
        }
    }
    edb_add(&b->html, "</div>");
}

void ed_window_show(const int index) {
    EdBuild b;
    ed_gui_begin(&b, PANEL_WINDOW);
    switch (index) {
        case 1: window_controls(&b); break;
        case 3: window_settings(&b); break;
        case 4: window_quick_add(&b); break;
        default: edb_add(&b.html, "<div class='windowHeader'><div>Unavailable</div></div>"); break;
    }
    ed_gui_end(&b, g_ed.el_menu_window);
}

static int h_lock(void *user, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) user;
    // GUI.panel.state.lock(this, 'left'|'right')
    const int right = argc >= 2 && args[1].kind == HTML_ARG_STR && !strcmp(args[1].str, "right");
    g_locked[right] = !g_locked[right];
    const HtmlElem icon = html_dom_query(ev->target, "i");
    if (icon) html_dom_set_text(icon, g_locked[right] ? "lock" : "lock_open");
    return 1;
}

static int h_switch_tab(void *user, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) user; (void) ev;
    const int tab = argc >= 1 ? (int) args[0].num : 0;
    if (tab != g_controls_tab) {
        g_controls_tab = tab;
        ed_window_show(1);
    }
    return 1;
}

void ed_panels_install(HtmlHandlerEntry *table, size_t *count, const size_t max) {
    const HtmlHandlerEntry entries[] = {
        {"GUI.panel.state.lock", h_lock},
        {"windows.0.switchTab", h_switch_tab},
        {"windows.2.switchTab", h_settings_tab},
    };
    for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]) && *count < max; i++) table[(*count)++] = entries[i];
}
