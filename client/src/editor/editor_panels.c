// Map editor panels: Krunker v3.1.1 ui/editor.js `_toolbar`, `panel.right.blank`, the right panel
// switch and `_window`, built with editor_gui.c, plus the control helpers every panel file uses
// (editor_panel_kit.h). The object panel is editor_inspector.c, the environment / terrain / zone
// panels editor_env.c and the left Essential panel editor_gameconfig.c.
#include "editor_internal.h"
#include "editor_panel_kit.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static int g_locked[2];
static char g_right_active[32] = "blank";

int ed_panel_locked(const int right) { return g_locked[right ? 1 : 0]; }
const char *ed_panel_right_active(void) { return g_right_active; }

// ---------------------------------------------------------------------------
// control helpers: create + bind + emit

EdCtl *edp_finish(EdBuild *b, EdCtl *c, const EdChangeFn fn) {
    c->on_change = fn;
    ed_gui_emit_value(b, c);
    return c;
}

EdCtl *edp_obj_switch(EdBuild *b, const char *name, const char *prop, const int sub, const EdChangeFn fn) {
    EdCtl *c = ed_gui_switch(b, name, sub);
    ed_ctl_bind_obj(c, prop);
    return edp_finish(b, c, fn);
}

EdCtl *edp_obj_slider(EdBuild *b, const char *name, const char *prop, const double mn, const double mx, const double step, const int sub, const EdChangeFn fn) {
    EdCtl *c = ed_gui_slider(b, name, mn, mx, step, sub);
    ed_ctl_bind_obj(c, prop);
    return edp_finish(b, c, fn);
}

EdCtl *edp_obj_select(EdBuild *b, const char *name, const char *prop, const EdOption *opts, const int n, const int sub, const EdChangeFn fn) {
    EdCtl *c = ed_gui_select(b, name, opts, n, sub);
    ed_ctl_bind_obj(c, prop);
    c->value_is_string = n > 0 && opts[0].svalue != NULL;
    return edp_finish(b, c, fn);
}

EdCtl *edp_obj_color(EdBuild *b, const char *name, const char *prop, const int sub) {
    EdCtl *c = ed_gui_color(b, name, sub);
    ed_ctl_bind_obj(c, prop);
    return edp_finish(b, c, NULL);
}

EdCtl *edp_obj_text(EdBuild *b, const char *name, const char *prop, const char *placeholder, const int sub, const EdChangeFn fn) {
    EdCtl *c = ed_gui_text(b, name, placeholder, sub);
    ed_ctl_bind_obj(c, prop);
    return edp_finish(b, c, fn);
}

EdCtl *edp_obj_number(EdBuild *b, const char *name, const char *prop, const char *placeholder, const int sub) {
    EdCtl *c = ed_gui_number(b, name, placeholder, 0, sub);
    ed_ctl_bind_obj(c, prop);
    return edp_finish(b, c, NULL);
}

EdCtl *edp_obj_vector3(EdBuild *b, const char *name, const char *prop, const EdChangeFn fn) {
    // Krunker passes min only for Size (min: 0); Position / Rotation get min=""
    EdCtl *c = ed_gui_vector3(b, name, !strcmp(prop, "size") ? 0 : NAN);
    ed_ctl_bind_obj(c, prop);
    return edp_finish(b, c, fn);
}

EdCtl *edp_json_switch(EdBuild *b, const char *name, cJSON *obj, const char *key, const int sub, const EdChangeFn fn) {
    EdCtl *c = ed_gui_switch(b, name, sub);
    ed_ctl_bind_json(c, obj, key);
    return edp_finish(b, c, fn);
}

EdCtl *edp_json_slider(EdBuild *b, const char *name, cJSON *obj, const char *key, const double mn, const double mx, const double step, const int sub, const EdChangeFn fn) {
    EdCtl *c = ed_gui_slider(b, name, mn, mx, step, sub);
    ed_ctl_bind_json(c, obj, key);
    return edp_finish(b, c, fn);
}

EdCtl *edp_json_text(EdBuild *b, const char *name, cJSON *obj, const char *key, const char *placeholder, const int sub, const EdChangeFn fn) {
    EdCtl *c = ed_gui_text(b, name, placeholder, sub);
    ed_ctl_bind_json(c, obj, key);
    return edp_finish(b, c, fn);
}

EdCtl *edp_json_select(EdBuild *b, const char *name, cJSON *obj, const char *key, const EdOption *opts, const int n, const int sub, const EdChangeFn fn) {
    EdCtl *c = ed_gui_select(b, name, opts, n, sub);
    ed_ctl_bind_json(c, obj, key);
    c->value_is_string = n > 0 && opts[0].svalue != NULL;
    return edp_finish(b, c, fn);
}

EdCtl *edp_json_color(EdBuild *b, const char *name, cJSON *obj, const char *key, const int sub, const EdChangeFn fn) {
    EdCtl *c = ed_gui_color(b, name, sub);
    ed_ctl_bind_json(c, obj, key);
    return edp_finish(b, c, fn);
}

EdCtl *edp_json_number(EdBuild *b, const char *name, cJSON *obj, const char *key, const char *placeholder, const int sub, const EdChangeFn fn) {
    EdCtl *c = ed_gui_number(b, name, placeholder, 0, sub);
    ed_ctl_bind_json(c, obj, key);
    return edp_finish(b, c, fn);
}

static EdCtl *edp_json_key_extra(EdBuild *b, const char *name, cJSON *obj, const char *key, const int editable, const int sub, const EdChangeFn fn, const char *extra) {
    EdCtl *c = ed_gui_key(b, name, editable, NULL, 0, sub);
    ed_ctl_bind_json(c, obj, key);
    if (extra) snprintf(c->path, sizeof(c->path), "%s", extra);
    return edp_finish(b, c, fn);
}

static EdCtl *edp_json_key(EdBuild *b, const char *name, cJSON *obj, const char *key, const int editable, const int sub, const EdChangeFn fn) {
    return edp_json_key_extra(b, name, obj, key, editable, sub, fn, NULL);
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
    edp_json_key(b, name, tmp, "val", 0, 0, NULL);
    cJSON_Delete(tmp); // the markup is written; the control is never active
}

cJSON *edp_child_obj(cJSON *parent, const char *key) {
    cJSON *o = cJSON_GetObjectItemCaseSensitive(parent, key);
    if (!cJSON_IsObject(o)) {
        cJSON_DeleteItemFromObjectCaseSensitive(parent, key);
        o = cJSON_AddObjectToObject(parent, key);
    }
    return o;
}

void edp_ensure_default(cJSON *obj, const char *key, const cJSON *def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (v && !cJSON_IsNull(v)) return;
    cJSON_DeleteItemFromObjectCaseSensitive(obj, key);
    cJSON_AddItemToObject(obj, key, def ? cJSON_Duplicate(def, 1) : cJSON_CreateNull());
}

// options built from a JSON string list; the arrays live for the panel's lifetime
EdOption g_opt_pool[OPT_POOL];
static int g_opt_used;
char g_opt_names[OPT_POOL][48];

EdOption *edp_opts_alloc(const int n) {
    if (g_opt_used + n > OPT_POOL) g_opt_used = 0;
    EdOption *o = &g_opt_pool[g_opt_used];
    g_opt_used += n;
    return o;
}

EdOption *edp_opts_from_names(const cJSON *names, int *count) {
    const int n = cJSON_GetArraySize(names);
    EdOption *o = edp_opts_alloc(n);
    for (int i = 0; i < n; i++) {
        const cJSON *it = cJSON_GetArrayItem(names, i);
        o[i] = (EdOption) {cJSON_IsString(it) ? it->valuestring : "", i, NULL};
    }
    *count = n;
    return o;
}

// UTILS.formatConstName
const char *edp_const_name(const char *s, char *out, const size_t n) {
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

void edp_div_begin(EdBuild *b, const char *id, const int show) {
    // _build "div": `"<div id=" + id + " " + (show ? null : 'style="display: none"') + ">"`
    edb_addf(&b->html, "<div id=%s %s>", id, show ? "null" : "style=\"display: none\"");
}

// ---------------------------------------------------------------------------
// toolbar (_toolbar.show)

enum {
    TB_IMPORT, TB_EXPORT, TB_EXPORT_GLTF, TB_PUBLISH, TB_SAVE_AS, TB_NEW, TB_COPY,
    TB_UNDO, TB_REDO, TB_DELETE, TB_DUPLICATE, TB_SETTINGS,
    TB_PLACEHOLDER, TB_GROUP_CREATE, TB_GROUP_STOP, TB_GROUP_STOP_ALL, TB_GROUP_COPY, TB_GROUP_CUT, TB_GROUP_PASTE,
    TB_PREFAB_JSON, TB_PREFAB_FILE, TB_PREFAB_EXPORT, TB_GROUP_EDIT, TB_CUSTOM,
    TB_ENVIRONMENT, TB_TERRAIN, TB_ZONE, TB_TOOLS, TB_TEST, TB_HOST, TB_QUICK_START, TB_CUSTOMIZE, TB_CONTROLS,
    TB_PROJECT = 900, // + ED_PM_* (editor_project.c)
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
    if (a >= TB_PROJECT) {
        ed_project_menu(a - TB_PROJECT);
        return;
    }
    switch (a) {
        case TB_IMPORT: ed_file_import(); break;
        case TB_EXPORT: ed_file_export(); break;
        case TB_EXPORT_GLTF: editor_message("GLTF export is not available in KrunkNative."); break;
        case TB_PUBLISH: ed_net_publish(); break; // editor_net.c
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
        case TB_PREFAB_JSON: ed_json_input(0); break; // advancedOptions.json: jsonInput()
        case TB_PREFAB_FILE: ed_json_input(1); break; // advancedOptions.file: jsonInput(true)
        case TB_PREFAB_EXPORT: ed_export_group(); break;
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
    tb_option(&b, "Copy", TB_COPY); tb_sep(&b);
    // KrunkNative: several maps open at once (editor_project.c), detached views and the
    // editor bus (editor_view.c, editor_bus_link.c)
    tb_rule(&b); tb_sep(&b);
    tb_menu_begin(&b, "Projects");
    tb_option(&b, "New Project", TB_PROJECT + ED_PM_NEW); tb_sep(&b);
    tb_option(&b, "Open Project...", TB_PROJECT + ED_PM_OPEN); tb_sep(&b);
    tb_option(&b, "Save", TB_PROJECT + ED_PM_SAVE); tb_sep(&b);
    tb_option(&b, "Save All", TB_PROJECT + ED_PM_SAVE_ALL); tb_sep(&b);
    tb_option(&b, "Close", TB_PROJECT + ED_PM_CLOSE); tb_sep(&b);
    tb_option(&b, "Next Project", TB_PROJECT + ED_PM_NEXT); tb_sep(&b);
    tb_rule(&b); tb_sep(&b);
#ifndef __EMSCRIPTEN__
    tb_option(&b, "Detach View", TB_PROJECT + ED_PM_DETACH); tb_sep(&b);
    tb_option(&b, "Open in New Window", TB_PROJECT + ED_PM_NEW_WINDOW); tb_sep(&b);
    tb_rule(&b); tb_sep(&b);
    tb_option(&b, ed_bus_active() ? "Editor Bus: On" : "Editor Bus: Off", TB_PROJECT + ED_PM_BUS); tb_sep(&b);
    tb_option(&b, "MCP Access...", TB_PROJECT + ED_PM_ACCESS);
#else
    tb_option(&b, "Open in New Tab", TB_PROJECT + ED_PM_NEW_WINDOW);
#endif
    tb_end(&b);
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
            // `!localOnly || !config.isProd`: the v3.1.1 server build is not isProd, so localOnly
            // prefabs (Terminal) are listed like the reference editor lists them
            if ((p->flags & (EP_CUSTOM | EP_NOEXPORT)) || !p->name[0]) continue;
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
// right panel

int edp_right_can_show(const char *name) {
    return !g_locked[1] || !strcmp(g_right_active, name);
}

void edp_right_begin(EdBuild *b, const char *name) {
    snprintf(g_right_active, sizeof(g_right_active), "%s", name);
    ed_gui_begin(b, PANEL_RIGHT);
}

// for the panels of editor_tools.c / editor_terrain.c: 0 when the locked panel is another one
int ed_panel_right_begin(EdBuild *b, const char *name) {
    if (!edp_right_can_show(name)) return 0;
    edp_right_begin(b, name);
    return 1;
}

void ed_panel_right_blank(void) {
    if (ed_project_background()) { // working on another project: rebuild once it is shown again
        g_ed.ui_stale = 1;
        return;
    }
    if (!edp_right_can_show("blank")) return;
    EdBuild b;
    edp_right_begin(&b, "blank");
    ed_gui_panel_buttons(&b, 1, "", 1);
    ed_gui_end(&b, g_ed.el_right_list);
}

void ed_panel_right_show(const char *name) {
    if (ed_project_background()) { // working on another project: rebuild once it is shown again
        g_ed.ui_stale = 1;
        return;
    }
    // e.resizable.restore(panel.right.restore)
    g_ed.panel_hidden[1] = 0;
    if (!strcmp(name, "environment")) ed_panel_right_environment();
    else if (!strcmp(name, "object")) ed_panel_right_object();
    else if (ed_tools_panel_show(name)) return;
    else if (ed_terrain_panel_show(name)) return;
    else ed_panel_right_blank();
}

void ed_panel_refresh_all(void) {
    if (ed_project_background()) { // working on another project: rebuild once it is shown again
        g_ed.ui_stale = 1;
        return;
    }
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
        edp_json_key_extra(b, "Fly forward", s, "fowardKey", 1, 0, win_settings_changed, arrow_html("up"));
        edp_json_key_extra(b, "Fly backwards", s, "backwardKey", 1, 0, win_settings_changed, arrow_html("down"));
        edp_json_key_extra(b, "Fly left", s, "leftKey", 1, 0, win_settings_changed, arrow_html("left"));
        edp_json_key_extra(b, "Fly right", s, "rightKey", 1, 0, win_settings_changed, arrow_html("right"));
        edp_json_key(b, "Fly up", s, "upKey", 1, 0, win_settings_changed);
        edp_json_key(b, "Fly down", s, "downKey", 1, 0, win_settings_changed);
        key_line(b, "Hold to fly faster", "Shift");
        ed_gui_folder_begin(b, "Speed", 1, 0, 0, "controls.speed");
        edp_json_number(b, "Normal", s, "speedNormal", "", 1, win_settings_changed);
        edp_json_number(b, "Sprinting", s, "speedSprint", "", 1, win_settings_changed);
        ed_gui_button_line(b, "Reset Speed", "Reset", win_reset_speed, 1);
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
        edp_json_key(b, "Face Selection Mode", s, "faceModeKey", 1, 0, win_settings_changed);
        edp_json_key(b, "Group Selection Mode", s, "groupModeKey", 1, 0, win_settings_changed);
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

static void on_anti_alias(EdCtl *c, const cJSON *v) {
    (void) c;
    ed_json_set_bool(g_ed.settings, "antiAlias", ed_json_truthy(v));
    editor_settings_save();
    editor_message("This change will occur after you refresh");
}

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
    EdCtl *c = edp_json_switch(b, name, g_ed.settings, key, 0, on_toggle_div);
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
        edp_json_slider(b, "Auto Backup", s, "autoBackup", 0, 25, 1, 0, on_grid);
        edp_json_slider(b, "UI Scale", s, "uiScale", 0.5, 1.5, 0.1, 0, on_grid);
        edp_json_switch(b, "Pointer Lock", s, "pointerLock", 0, on_grid);
        edp_json_switch(b, "Disable Warnings", s, "disableWarnings", 0, on_grid);
        ed_gui_rule(b);
        ed_gui_folder_begin(b, "Render", 0, 1, 0, "settings.render");
        edp_json_slider(b, "Resolution", s, "resMltV", 0.1, 2, 0.1, 0, on_grid);
        // v3.1.1 binds this switch to key "resMltV" (a Krunker bug): it shows the resolution
        // value's truthiness and its onChange stores antiAlias. GUI.action also writes the
        // checkbox into settings.resMltV; that part is not copied (it would break Resolution).
        {
            cJSON *tmp = cJSON_CreateObject();
            cJSON_AddBoolToObject(tmp, "aa", ed_json_truthy(cJSON_GetObjectItemCaseSensitive(s, "resMltV")));
            EdCtl *aa = edp_json_switch(b, "Anti-aliasing", tmp, "aa", 0, on_anti_alias);
            aa->bind = EB_NONE;
            aa->json = NULL;
            cJSON_Delete(tmp);
        }
        ed_gui_folder_end(b, 0);
        ed_gui_folder_begin(b, "History", 0, 1, 0, "settings.history");
        div_switch(b, "Disable", "disableHistory", "grid", 1);
        edp_div_begin(b, "grid", !editor_setting_bool("disableHistory"));
        edp_json_slider(b, "Limit", s, "historyLimit", 0, 1000, 1, 1, on_grid);
        edb_add(&b->html, "</div>\n");
        ed_gui_folder_end(b, 0);
        ed_gui_folder_begin(b, "Game", 0, 1, 0, "settings.game");
        edp_json_switch(b, "Show Real Hitbox", s, "showRealHitbox", 0, on_grid);
        ed_gui_folder_end(b, 0);
    } else if (g_settings_tab == 1) {
        edp_json_switch(b, "Dark Mode", s, "darkMode", 0, on_grid);
        ed_gui_rule(b);
        ed_gui_folder_begin(b, "Grid", 0, 0, 0, "settings.grid");
        div_switch(b, "Visible", "gridVisibility", "grid", 0);
        edp_div_begin(b, "grid", editor_setting_bool("gridVisibility"));
        edp_json_slider(b, "Opacity", s, "gridOpacity", 0.05, 1, 0.05, 1, on_grid);
        edp_json_number(b, "Size", s, "gridSize", "", 1, on_grid);
        edp_json_number(b, "Spacing", s, "gridSpacing", "", 1, on_grid);
        edb_add(&b->html, "</div>\n");
        ed_gui_button_line(b, "Reset Grid", "Reset", grid_reset, 0);
        ed_gui_folder_end(b, 0);
        ed_gui_folder_begin(b, "Death Barrier", 0, 0, 0, "settings.db");
        div_switch(b, "Visible", "dbVisibility", "deathBarrier", 0);
        edp_div_begin(b, "deathBarrier", editor_setting_bool("dbVisibility"));
        edp_json_slider(b, "Opacity", s, "dbOpacity", 0.05, 1, 0.05, 1, on_grid);
        edb_add(&b->html, "</div>\n");
        ed_gui_button_line(b, "Reset Barrier", "Reset", db_reset, 0);
        ed_gui_folder_end(b, 0);
    } else if (g_settings_tab == 2) {
        edp_json_switch(b, "Trigger Connections", s, "triggerCons", 0, on_grid);
        edp_json_switch(b, "Select through invisible objects [ALT]", s, "selectThroughInvis", 0, on_grid);
        edp_json_switch(b, "Object Border Visuals", s, "borderVisuals", 0, on_grid);
        edp_json_switch(b, "Highlight", s, "objectHighlight", 0, on_grid);
        edp_json_switch(b, "Create Near", s, "createObjectNear", 0, on_grid);
        ed_gui_button_line(b, "Lightcone Boundingbox", "Toggle", NULL, 0);
        ed_gui_rule(b);
        ed_gui_folder_begin(b, "Config", 0, 0, 0, "settings.config");
        edp_json_switch(b, "Anti Radians", s, "degToRad", 0, on_grid);
        edp_json_switch(b, "Preserve Folders", s, "preserveFolderState", 0, on_grid);
        ed_gui_folder_end(b, 0);
        ed_gui_folder_begin(b, "Snapping", 0, 0, 0, "settings.snapping");
        edp_json_slider(b, "Translation", s, "translationSnapping", 0.001, 25, 0.001, 0, on_grid);
        edp_json_slider(b, "Rotation", s, "rotationSnapping", 0.001, 45, 0.001, 0, on_grid);
        edp_json_slider(b, "Scale", s, "scaleSnapping", 0.001, 25, 0.001, 0, on_grid);
        ed_gui_button_line(b, "Reset Snapping", "Reset", snapping_reset, 0);
        ed_gui_folder_end(b, 0);
    } else {
        static const EdOption open_on[] = {{"Middle Click", 0, NULL}, {"Right Click", 1, NULL}};
        div_switch(b, "Enabled", "quickGUI", "quickGUISettings", 0);
        edp_div_begin(b, "quickGUISettings", editor_setting_bool("quickGUI"));
        edp_json_select(b, "Open On", s, "quickGUIOpen", open_on, 2, 1, on_grid);
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

// windows[3] "Quick Add": settings.quickAddButtons (8 slots), Customize mode, search
static int g_quick_edit;
static int g_quick_custom_index = -1;
static cJSON *g_quick_current; // currBtns (borrowed from settings, or the search results)

static cJSON *quick_buttons(void) {
    cJSON *b = cJSON_GetObjectItemCaseSensitive(g_ed.settings, "quickAddButtons");
    if (!cJSON_IsArray(b)) {
        cJSON *def = cJSON_Parse(editor_default_settings());
        cJSON_DeleteItemFromObjectCaseSensitive(g_ed.settings, "quickAddButtons");
        cJSON_AddItemToObject(g_ed.settings, "quickAddButtons", cJSON_DetachItemFromObject(def, "quickAddButtons"));
        cJSON_Delete(def);
        b = cJSON_GetObjectItemCaseSensitive(g_ed.settings, "quickAddButtons");
    }
    return b;
}

static void quick_grid(EdBuf *h, const char *search) {
    // windows[3].genGrid(search)
    static cJSON *results;
    cJSON_Delete(results);
    results = NULL;
    if (search && *search) {
        results = cJSON_CreateArray();
        char lower[64];
        size_t n = 0;
        for (; search[n] && n + 1 < sizeof(lower); n++) lower[n] = (char) tolower((unsigned char) search[n]);
        lower[n] = 0;
        // filter by formatted name, sorted by name length (stable)
        int order[EDITOR_PREFAB_COUNT], count = 0;
        for (int i = 0; i < EDITOR_PREFAB_COUNT; i++) {
            char l[64];
            size_t k = 0;
            for (; editor_prefabs[i].label[k] && k + 1 < sizeof(l); k++) l[k] = (char) tolower((unsigned char) editor_prefabs[i].label[k]);
            l[k] = 0;
            if (strstr(l, lower)) order[count++] = i;
        }
        for (int a = 1; a < count; a++) {
            const int v = order[a];
            int b = a - 1;
            while (b >= 0 && strlen(editor_prefabs[order[b]].name) > strlen(editor_prefabs[v].name)) {
                order[b + 1] = order[b];
                b--;
            }
            order[b + 1] = v;
        }
        for (int a = 0; a < count; a++) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "name", editor_prefabs[order[a]].label);
            cJSON_AddStringToObject(o, "desc", "");
            cJSON_AddNumberToObject(o, "item", order[a]);
            cJSON_AddItemToArray(results, o);
        }
        g_quick_current = results;
    } else {
        g_quick_current = quick_buttons();
    }
    int t = 0;
    const cJSON *e;
    int first = 1;
    cJSON_ArrayForEach(e, g_quick_current) {
        const int blank = !strcmp(ed_json_str(e, "type", ""), "blank");
        if (!blank || g_quick_edit) {
            const char *name = ed_json_str(e, "name", "");
            char img[96];
            size_t k = 0;
            for (const char *p = name; *p && k + 1 < sizeof(img); p++) if (*p != ' ') img[k++] = (char) tolower((unsigned char) *p);
            img[k] = 0;
            if (!first) edb_add(h, "\n");
            edb_addf(h, "<div class='quickAddButton'><div class='previewDesc' onclick='windows[3].%s(%d)'><div>", g_quick_edit ? "showCustomize" : "clickButton", t);
            const char *desc = ed_json_str(e, "desc", "");
            edb_add_escaped(h, g_quick_edit ? "Click to change item" : desc[0] ? desc : "Click to add");
            edb_addf(h, "</div></div><div class='previewDeleteIcon'%sonclick='windows[3].deleteButton(%d)'><i class='material-icons'>delete_forever</i></div><div class='previewImg'",
                     g_quick_edit && !blank ? "" : "style='display:none'", t);
            if (!blank) edb_addf(h, "style='background-size:%d%%;background-image:url(img/%s.png)'", ed_json_truthy(cJSON_GetObjectItemCaseSensitive(e, "cust")) ? 80 : 50, img);
            edb_addf(h, "><div %s>", blank ? "style='margin-top:50%;transform:translateY(-50%)'" : "");
            if (blank) edb_add(h, "<i class='material-icons' style='font-size:65px;color:#FFF'>add</i>");
            else edb_add_escaped(h, name);
            edb_add(h, "</div></div></div>");
        } else if (!first) {
            edb_add(h, "\n");
        }
        first = 0;
        t++;
    }
}

static void window_quick_add(EdBuild *b) {
    edb_addf(&b->html, "<div class='windowHeader'><div>Quick Add</div><input type='text' id='quickSearch' class='smlInput' autofocus='' placeholder='Search' onkeyup='windows[3].search()'><div style='cursor:pointer;margin-top:5px;font-size:14px;float:right;color:#FFF;user-select:none' onclick='windows[3].toggleEditMode()'>%s</div></div><div class='windowBody'><div id='buttonGrid' class='buttonGrid'>",
             g_quick_edit ? "Done" : "Customize");
    quick_grid(&b->html, NULL);
    edb_add(&b->html, "</div></div>");
}

static void quick_change(cJSON *button) {
    // windows[3].changeButton(e)
    cJSON *list = quick_buttons();
    if (g_quick_custom_index >= 0 && g_quick_custom_index < cJSON_GetArraySize(list)) cJSON_ReplaceItemInArray(list, g_quick_custom_index, button);
    else cJSON_Delete(button);
    g_quick_custom_index = -1;
    editor_settings_save();
    editor_show_window(4);
}

static void quick_cust_add(EdCtl *c) {
    cJSON *o = cJSON_CreateObject();
    if (c->arg == -1) {
        cJSON_AddStringToObject(o, "name", "Custom Asset");
        cJSON_AddStringToObject(o, "desc", "Import community made assets");
        cJSON_AddBoolToObject(o, "cust", 1);
    } else if (c->arg == -2) {
        cJSON_AddStringToObject(o, "name", "Placeholder");
        cJSON_AddStringToObject(o, "desc", "Stretch over objects to form a group");
        cJSON_AddBoolToObject(o, "place", 1);
    } else {
        cJSON_AddStringToObject(o, "name", editor_prefabs[c->arg].label);
        cJSON_AddStringToObject(o, "desc", "Click to add");
        cJSON_AddNumberToObject(o, "item", c->arg);
    }
    quick_change(o);
}

static void window_quick_customize(EdBuild *b) {
    // windows[5] "Quick Add Customize": GUI._window.quickaddcust
    edb_add(&b->html, "<div style='margin-bottom:15px'><div class='windowHeader'><div style='cursor: pointer; margin-right:5px'onclick='showWindow(4)'><i class='material-icons' style='font-weight:bold; transform:rotate(-135deg);'>call_made</i></div><div>Quick Add Customize</div></div></div><div class='windowBody'>");
    ed_gui_rule(b);
    ed_gui_folder_begin(b, "General", 0, 0, 0, "quickadd.general");
    ed_gui_button_line(b, "Custom Asset", "Add", quick_cust_add, 0)->arg = -1;
    ed_gui_button_line(b, "Placeholder", "Add", quick_cust_add, 0)->arg = -2;
    ed_gui_folder_end(b, 0);
    static const char *folders[3] = {"Basic", "Models", "Tools"};
    for (int f = 0; f < 3; f++) {
        ed_gui_folder_begin(b, folders[f], 0, 0, 0, f == 0 ? "quickadd.basic" : f == 1 ? "quickadd.models" : "quickadd.tools");
        for (int i = 0; i < EDITOR_PREFAB_COUNT; i++) {
            const EditorPrefab *p = &editor_prefabs[i];
            // `!localOnly || !config.isProd`: the v3.1.1 server build is not isProd, so localOnly
            // prefabs (Terminal) are listed like the reference editor lists them
            if ((p->flags & (EP_CUSTOM | EP_NOEXPORT)) || !p->name[0]) continue;
            const int cat = (p->flags & (EP_TOOL | EP_TOOL2)) ? 2 : (p->flags & EP_GEN) ? 1 : 0;
            if (cat != f) continue;
            ed_gui_button_line(b, p->label, "Add", quick_cust_add, 0)->arg = i;
        }
        ed_gui_folder_end(b, 0);
    }
    edb_add(&b->html, "</div>");
}

static int h_quick(void *user, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) user;
    const char *name = html_ui_current_handler();
    const int idx = argc >= 1 ? (int) args[0].num : 0;
    if (!name) return 1;
    if (!strcmp(name, "windows.3.toggleEditMode")) {
        g_quick_edit = !g_quick_edit;
        editor_show_window(4);
    } else if (!strcmp(name, "windows.3.clickButton")) {
        const cJSON *b = cJSON_GetArrayItem(g_quick_current, idx);
        if (!b) return 1;
        if (ed_json_truthy(cJSON_GetObjectItemCaseSensitive(b, "cust"))) {
            editor_show_window(2); // clickButton: cust -> showWindow(2) (editor_net.c)
        } else if (ed_json_truthy(cJSON_GetObjectItemCaseSensitive(b, "place"))) {
            editor_close_window();
            ed_obj_create_placeholder();
        } else {
            const int item = (int) ed_json_num(b, "item", 0);
            editor_close_window();
            ed_obj_create(item);
        }
    } else if (!strcmp(name, "windows.3.deleteButton")) {
        cJSON *b = cJSON_GetArrayItem(quick_buttons(), idx);
        if (b) ed_json_set_str(b, "type", "blank");
        editor_settings_save();
        editor_show_window(4);
    } else if (!strcmp(name, "windows.3.showCustomize")) {
        g_quick_custom_index = idx;
        editor_show_window(6);
    } else if (!strcmp(name, "windows.3.search")) {
        const HtmlElem input = html_dom_by_id("quickSearch");
        char v[64] = "";
        html_dom_get_value(input, v, sizeof(v));
        EdBuf h;
        edb_init(&h);
        quick_grid(&h, v);
        html_dom_set_html(html_dom_by_id("buttonGrid"), h.buf ? h.buf : "");
        edb_free(&h);
        if (html_key_is_enter(ev->key) && cJSON_GetArraySize(g_quick_current)) {
            HtmlArg a = {HTML_ARG_NUM, 0, NULL};
            (void) a;
            const cJSON *b = cJSON_GetArrayItem(g_quick_current, 0);
            const int item = (int) ed_json_num(b, "item", 0);
            editor_close_window();
            ed_obj_create(item);
        }
    }
    return 1;
}

void ed_window_show(const int index) {
    EdBuild b;
    ed_gui_begin(&b, PANEL_WINDOW);
    switch (index) {
        case 1: window_controls(&b); break;
        case 3: window_settings(&b); break;
        case 4: window_quick_add(&b); break;
        case 6: window_quick_customize(&b); break;
        case 2: case 5: case 7: case 8: ed_net_window_gen(index, &b.html); break; // editor_net.c
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
        {"windows.3.toggleEditMode", h_quick},
        {"windows.3.clickButton", h_quick},
        {"windows.3.deleteButton", h_quick},
        {"windows.3.showCustomize", h_quick},
        {"windows.3.search", h_quick},
    };
    for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]) && *count < max; i++) table[(*count)++] = entries[i];
}

// KRUNKNATIVE_EDITOR_SCRIPT test driver (editor.c): set the window tabs directly
void ed_window_set_tabs(const int controls, const int settings, const int quick_edit) {
    if (controls >= 0) g_controls_tab = controls;
    if (settings >= 0) g_settings_tab = settings;
    if (quick_edit >= 0) g_quick_edit = quick_edit;
}
