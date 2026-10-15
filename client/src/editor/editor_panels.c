// Map editor panels: Krunker v7.1.4 ui/editor.js `_toolbar`, `panel.right.blank` /
// `objectList` (Scene Graph), the right panel switch and `_window` (windows 1-15 of editor.js),
// built with editor_gui.c, plus the control helpers every panel file uses (editor_panel_kit.h). The object panel is editor_inspector.c, the environment / terrain / zone
// panels editor_env.c and the left Essential panel editor_gameconfig.c.
#include "editor_internal.h"
#include "editor_ai.h"
#include "editor_panel_kit.h"
#include <app_mode.h>    // app_route_url (Documentation, links)
#include <backend_ui.h>  // account_get (the Account menu)
#include <html_ui.h>
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
// toolbar (_toolbar.show, UI:543-688)

enum {
    TB_IMPORT, TB_NEW, TB_SAVE, TB_LOAD_BACKUP, TB_EXPORT, TB_COPY_EXPORT, TB_EXPORT_GLTF, TB_EXPORT_MTL,
    TB_UNDO, TB_REDO, TB_DELETE, TB_DUPLICATE, TB_SETTINGS,
    TB_SCENE_GRAPH, TB_PLACEHOLDER, TB_GROUP_CREATE, TB_GROUP_STOP, TB_GROUP_STOP_ALL, TB_GROUP_COPY, TB_GROUP_CUT, TB_GROUP_PASTE,
    TB_PREFAB_JSON, TB_PREFAB_FILE, TB_PREFAB_EXPORT, TB_PREFAB_TOOLS, TB_CUSTOM,
    TB_ENVIRONMENT, TB_TERRAIN, TB_ZONE, TB_OCEAN, TB_GUI, TB_SCRIPTING,
    TB_TEST, TB_HOST, TB_QUICK_START, TB_TEMPLATES, TB_TOOLS,
    TB_PUBLISH_GAME, TB_PUBLISH_ASSET, TB_PUBLISH_GUIDE,
    TB_LOGIN, TB_ACCOUNT_NAME, TB_MY_GAMES, TB_MY_ASSETS,
    TB_CONTROLS, TB_GUIDE, TB_DOCUMENTATION, TB_AI,
    TB_PROJECT = 900, // + ED_PM_* (editor_project.c)
    TB_CREATE = 1000, // + prefab id
};

static void toolbar_close_menus(void) {
    HtmlElem drops[64];
    const int n = html_dom_query_all(html_dom_by_id("_toolbar"), ".toolbarDropDown, .toolbarSubDropDown", drops, 64);
    for (int i = 0; i < n; i++) html_dom_set_display(drops[i], "");
}

static void new_project_confirmed(void *user, const char *value) {
    (void) user;
    if (value) ed_map_clear(); // clearMap
}

void ed_import_map_popup(void) {
    // importMapPopup (E:3891): a paste box; importMapData imports it and clears the popup
    ed_generic_popup("<div class='pubHed'>Import Map</div><div style='color:rgba(255,255,255,0.7);font-size:19px;text-align:center;margin-bottom:15px;margin-top: -10px;'>Copy Paste Map Text Here</div><textarea id='importTxt' rows='3' onkeydown='importMapKey()'></textarea><div class='mapFndB' onclick='importMapData()' style='margin-left: 0px;margin-top: 10px;background-color:#4ca9f5;width: 380px;'>Import</div>",
                     "importPop");
    const HtmlElem t = html_dom_by_id("importTxt");
    if (t) html_dom_focus(t);
}

static void import_map_data(void) {
    // importMapData: KE.importMap(importTxt.value) when not empty, then clearPops
    const HtmlElem t = html_dom_by_id("importTxt");
    const int n = t ? html_dom_get_value(t, NULL, 0) : 0;
    char *v = n > 0 ? malloc((size_t) n + 1) : NULL;
    if (v) {
        html_dom_get_value(t, v, (size_t) n + 1);
        ed_map_import(v);
        free(v);
    }
    ed_clear_pops();
}

static void open_documentation(void) {
    // window.open("//docs.krunker.io", "_blank")
    if (!app_route_url("https://docs.krunker.io")) editor_message("Documentation: https://docs.krunker.io");
}

// reuseSelected(i) (E:5954): defaultFromType(i, selected.pos, selected.size), else createObjects[i]
static void reuse_selected(const int prefab) {
    const EditorObject *sel = g_ed.proj->selected;
    if (!sel || prefab == ED_PREFAB_CUSTOM_ASSET) {
        ed_obj_create(prefab);
        return;
    }
    cJSON *d = cJSON_CreateObject();
    if (prefab) cJSON_AddNumberToObject(d, "i", prefab);
    const cJSON *p = cJSON_GetObjectItemCaseSensitive(sel->data, "p"), *s = cJSON_GetObjectItemCaseSensitive(sel->data, "s");
    if (p) cJSON_AddItemToObject(d, "p", cJSON_Duplicate(p, 1));
    if (s) cJSON_AddItemToObject(d, "s", cJSON_Duplicate(s, 1));
    EditorObject *o = ed_obj_new(d, 0);
    cJSON_Delete(d);
    ed_obj_add(o, 0, 1);
}

static void toolbar_action(EdCtl *c) {
    toolbar_close_menus();
    const int a = c->arg;
    const int shift = (g_ed.mods & GLFW_MOD_SHIFT) != 0;
    if (a >= TB_CREATE) {
        // Shift+click: reuseSelected(i) (E:5954): the new prefab takes the selection's pos and size
        if (shift && g_ed.proj->selected) reuse_selected(a - TB_CREATE);
        else ed_obj_create(a - TB_CREATE);
        return;
    }
    if (a >= TB_PROJECT) {
        ed_project_menu(a - TB_PROJECT);
        return;
    }
    switch (a) {
        case TB_IMPORT: // shift: loadFile("importMap")
            if (shift) ed_file_import();
            else ed_import_map_popup();
            break;
        case TB_NEW: ed_confirm("Are you sure you want to reset the map?", new_project_confirmed, NULL); break;
        case TB_SAVE: ed_file_save_as(); break; // save(): "<name>_save.txt"
        case TB_LOAD_BACKUP: editor_show_window(15); break;
        case TB_EXPORT: ed_file_export(); break; // exportMap (KrunkNative also copies it, shift or not)
        case TB_COPY_EXPORT: {
            char *text = ed_map_export_string();
            if (text) {
                ed_clipboard_copy(text);
                free(text);
                editor_message("Map copied to the clipboard.");
            }
            break;
        }
        case TB_EXPORT_GLTF: ed_file_export_gltf(); break; // exportScene (editor_export.c)
        case TB_EXPORT_MTL: ed_file_export_mtl(); break;   // exportScene2 (editor_export.c)
        case TB_UNDO: ed_history_execute(0); break;
        case TB_REDO: ed_history_execute(1); break;
        case TB_DELETE: ed_obj_delete_selected(); break;
        case TB_DUPLICATE: ed_obj_duplicate(); break;
        case TB_SETTINGS: editor_show_window(3); break;
        case TB_SCENE_GRAPH: ed_panel_right_show("objectList"); break;
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
        case TB_PREFAB_TOOLS: ed_panel_right_show("prefab"); break;
        case TB_CUSTOM: ed_obj_create(ED_PREFAB_CUSTOM_ASSET); break;
        case TB_ENVIRONMENT: ed_panel_right_show("environment"); break;
        case TB_TERRAIN: ed_panel_right_show("terrain"); break;
        case TB_ZONE: ed_panel_right_show("zone"); break;
        case TB_OCEAN: ed_panel_right_show("ocean"); break;
        case TB_GUI: ed_panel_right_show("gui"); break;
        case TB_SCRIPTING: editor_script_popup(); break;
        case TB_TEST: editor_test_map(0, 0); break;
        case TB_HOST: editor_test_map(1, 0); break;
        case TB_QUICK_START: ed_quick_start(); break;
        case TB_TEMPLATES: editor_show_window(10); break;
        case TB_TOOLS: ed_panel_right_show("tools"); break;
        case TB_PUBLISH_GAME: ed_net_publish(); break; // publishMap (editor_net.c)
        case TB_PUBLISH_ASSET: ed_net_publish_asset(); break; // publishAssetPopup (editor_net.c)
        case TB_PUBLISH_GUIDE: editor_show_window(11); break;
        case TB_LOGIN: // loginPopup: KrunkNative signs in through the hub (account_ui.c keeps the session)
            if (!app_route_url("/social.html")) editor_message("Log in from the Krunker Hub.");
            break;
        case TB_MY_GAMES: editor_show_window(14); break;
        case TB_MY_ASSETS: ed_net_my_assets(); break; // myAssets: windows[1] tab 1 (editor_net.c)
        case TB_CONTROLS: editor_show_window(1); break;
        case TB_GUIDE: editor_show_window(8); break;
        case TB_DOCUMENTATION: open_documentation(); break;
        case TB_AI: ed_ai_toggle(); break; // KrunkNative: the AI panel (editor_ai.c)
        default: break;
    }
}

// Krunker's toolbar markup (GUI._build "toolbar"): siblings joined with "\n"
typedef struct {
    EdBuild *b;
    int first[8];
    int depth;
} TbBuild;

static void tb_sep(TbBuild *t) {
    if (!t->first[t->depth]) edb_add(&t->b->html, "\n");
    t->first[t->depth] = 0;
}

static void tb_option(TbBuild *t, const char *name, const int action) {
    tb_sep(t);
    EdCtl *c = ed_gui_ctl(t->b->panel, EG_TOOLBAR_OPTION);
    c->arg = action;
    c->action = toolbar_action;
    edb_addf(&t->b->html, "<div class=\"toolbarDropDownItem\" onclick=\";GUI.action(%d, this)\">", c->id);
    edb_add_escaped(&t->b->html, name);
    edb_add(&t->b->html, "</div>");
}

static void tb_rule(TbBuild *t) {
    tb_sep(t);
    edb_add(&t->b->html, "<hr>");
}

static void tb_push(TbBuild *t) {
    t->depth++;
    t->first[t->depth] = 1;
}

static void tb_root_begin(TbBuild *t, const char *name) {
    tb_sep(t);
    edb_addf(&t->b->html, "<div class=\"toolbarItem\" onmouseover=\"GUI.update.toolbar(this, true)\" onmouseout=\"GUI.update.toolbar(this)\">\n\t\t\t\t\t\t%s<div class=\"toolbarDropDown\">", name);
    tb_push(t);
}

static void tb_menu_begin(TbBuild *t, const char *name) {
    tb_sep(t);
    edb_addf(&t->b->html, "<div class=\"toolbarDropDownItem\" onmouseover=\"GUI.update.toolbar(this, true)\" onmouseout=\"GUI.update.toolbar(this)\">\n\t\t\t\t\t\t%s<div class=\"toolbarSubDropDown\" style=\"top: this.offsetTop\">", name);
    tb_push(t);
}

static void tb_end(TbBuild *t) {
    edb_add(&t->b->html, "</div></div>");
    t->depth--;
}

// the prefab menus of the Object menu: Lights (edCat "lights"), Zones (zone), Tools (tool /
// tool2), Models (gen), Basic (the rest); Models and Tools in 12-row columns ("chunktoolbar")
enum { CAT_BASIC, CAT_MODELS, CAT_TOOLS, CAT_LIGHTS, CAT_ZONES };

static int prefab_listed(const EditorPrefab *p) {
    // (!localOnly || !config.isProd) && !custom && !noExport && name != "": the page is
    // production (krunker.io), so localOnly prefabs (Event, Terminal, Bot Spawner, ...) are not
    // offered, as on the public v7 editor
    return !(p->flags & (EP_CUSTOM | EP_NOEXPORT | EP_LOCALONLY)) && p->name[0];
}

static int prefab_menu(const EditorPrefab *p) {
    if (p->ed_cat && !strcmp(p->ed_cat, "lights")) return CAT_LIGHTS;
    if (p->flags & EP_ZONE) return CAT_ZONES;
    if (p->flags & (EP_TOOL | EP_TOOL2)) return CAT_TOOLS;
    if (p->flags & EP_GEN) return CAT_MODELS;
    return CAT_BASIC;
}

static void tb_prefab_menu(TbBuild *t, const char *name, const int cat, const int chunked) {
    tb_menu_begin(t, name);
    int n = 0;
    for (int i = 0; i < EDITOR_PREFAB_COUNT; i++) {
        const EditorPrefab *p = &editor_prefabs[i];
        if (!prefab_listed(p) || prefab_menu(p) != cat) continue;
        if (chunked && n % 12 == 0) {
            if (n) edb_add(&t->b->html, "</div>");
            edb_add(&t->b->html, "<div class=\"toolbarChunkSubDropDown\">");
            t->first[t->depth] = 1;
        }
        tb_option(t, p->label, TB_CREATE + i);
        n++;
    }
    if (chunked && n) edb_add(&t->b->html, "</div>");
    tb_end(t);
}

static int g_toolbar_account = -1; // the account id the toolbar shows (0 = none)

void ed_toolbar_account_check(void) {
    const AccountInfo *acc = account_get();
    if ((acc ? acc->id : 0) != g_toolbar_account) ed_toolbar_show();
}

void ed_toolbar_show(void) {
    g_toolbar_account = account_get() ? account_get()->id : 0;
    EdBuild b;
    ed_gui_begin(&b, PANEL_TOOLBAR);
    TbBuild tb = {.b = &b, .first = {1}, .depth = 0};
    TbBuild *t = &tb;
    tb_root_begin(t, "File");
    tb_option(t, "Import Project", TB_IMPORT);
    // "Import Old Project" is dev-only (KE.dev) in v7
    tb_option(t, "New Project", TB_NEW);
    tb_option(t, "Save Project", TB_SAVE);
    tb_option(t, "Load Backup", TB_LOAD_BACKUP);
    tb_rule(t);
    tb_option(t, "Export", TB_EXPORT);
    tb_option(t, "Copy Export", TB_COPY_EXPORT);
    tb_rule(t);
    tb_option(t, "Export GLTF", TB_EXPORT_GLTF);
    tb_option(t, "Export MTL", TB_EXPORT_MTL);
    // KrunkNative: several maps open at once (editor_project.c), detached views and the
    // editor bus (editor_view.c, editor_bus_link.c)
    tb_rule(t);
    tb_menu_begin(t, "Projects");
    tb_option(t, "New Project", TB_PROJECT + ED_PM_NEW);
    tb_option(t, "Open Project...", TB_PROJECT + ED_PM_OPEN);
    tb_option(t, "Save", TB_PROJECT + ED_PM_SAVE);
    tb_option(t, "Save All", TB_PROJECT + ED_PM_SAVE_ALL);
    tb_option(t, "Close", TB_PROJECT + ED_PM_CLOSE);
    tb_option(t, "Next Project", TB_PROJECT + ED_PM_NEXT);
    tb_rule(t);
#ifndef __EMSCRIPTEN__
    tb_option(t, "Detach View", TB_PROJECT + ED_PM_DETACH);
    tb_option(t, "Open in New Window", TB_PROJECT + ED_PM_NEW_WINDOW);
    tb_rule(t);
    tb_option(t, ed_bus_active() ? "Editor Bus: On" : "Editor Bus: Off", TB_PROJECT + ED_PM_BUS);
    tb_option(t, "MCP Access...", TB_PROJECT + ED_PM_ACCESS);
#else
    tb_option(t, "Open in New Tab", TB_PROJECT + ED_PM_NEW_WINDOW);
#endif
    tb_end(t);
    tb_end(t);

    tb_root_begin(t, "Edit");
    tb_option(t, "Undo (Ctrl + Z)", TB_UNDO);
    tb_option(t, "Redo (Ctrl + Y)", TB_REDO);
    tb_rule(t);
    tb_option(t, "Delete (Del)", TB_DELETE);
    tb_option(t, "Duplicate (Shift + R)", TB_DUPLICATE);
    tb_rule(t);
    tb_option(t, "Settings...", TB_SETTINGS);
    tb_end(t);

    tb_root_begin(t, "Object");
    tb_option(t, "Scene Graph", TB_SCENE_GRAPH);
    tb_rule(t);
    tb_prefab_menu(t, "Basic", CAT_BASIC, 0);
    tb_prefab_menu(t, "Models", CAT_MODELS, 1);
    tb_prefab_menu(t, "Tools", CAT_TOOLS, 1);
    tb_prefab_menu(t, "Lights", CAT_LIGHTS, 0);
    tb_prefab_menu(t, "Zones", CAT_ZONES, 0);
    tb_rule(t);
    tb_option(t, "Placeholder", TB_PLACEHOLDER);
    tb_menu_begin(t, "Group");
    tb_option(t, "Create", TB_GROUP_CREATE);
    tb_option(t, "Stop", TB_GROUP_STOP);
    tb_option(t, "Stop All", TB_GROUP_STOP_ALL);
    tb_rule(t);
    tb_option(t, "Copy", TB_GROUP_COPY);
    tb_option(t, "Cut", TB_GROUP_CUT);
    tb_option(t, "Paste", TB_GROUP_PASTE);
    tb_rule(t);
    tb_menu_begin(t, "Prefab");
    tb_menu_begin(t, "Import");
    tb_option(t, "JSON", TB_PREFAB_JSON);
    tb_option(t, "File", TB_PREFAB_FILE);
    tb_end(t);
    tb_option(t, "Export", TB_PREFAB_EXPORT);
    tb_option(t, "Tools", TB_PREFAB_TOOLS);
    tb_end(t);
    tb_end(t);
    tb_rule(t);
    tb_option(t, "Custom...", TB_CUSTOM);
    tb_end(t);

    tb_root_begin(t, "Components");
    tb_option(t, "Environment", TB_ENVIRONMENT);
    tb_option(t, "Terrain", TB_TERRAIN);
    tb_option(t, "Zone", TB_ZONE);
    tb_option(t, "Ocean", TB_OCEAN);
    tb_option(t, "GUI", TB_GUI);
    tb_end(t);

    tb_option(t, "Scripting", TB_SCRIPTING);

    tb_root_begin(t, "Game");
    tb_option(t, "Test Game", TB_TEST);
    tb_option(t, "Host Game", TB_HOST);
    tb_rule(t);
    tb_option(t, "Quick Start", TB_QUICK_START);
    tb_option(t, "Templates", TB_TEMPLATES);
    tb_rule(t);
    tb_option(t, "Tools", TB_TOOLS);
    tb_end(t);

    tb_root_begin(t, "Publish");
    tb_option(t, "Game", TB_PUBLISH_GAME);
    tb_option(t, "Asset", TB_PUBLISH_ASSET);
    tb_rule(t);
    tb_option(t, "Publishing Guide", TB_PUBLISH_GUIDE);
    tb_end(t);

    tb_root_begin(t, "Account");
    const AccountInfo *acc = account_get();
    if (acc) {
        char name[64];
        snprintf(name, sizeof(name), "Username: %s", acc->name);
        tb_option(t, name, TB_ACCOUNT_NAME);
        tb_option(t, "My Games", TB_MY_GAMES);
        tb_option(t, "My Assets", TB_MY_ASSETS);
    } else {
        tb_option(t, "Login", TB_LOGIN);
    }
    // KrunkNative: the map editor's AI panel (editor_ai.c; signed-in accounts), also the canvas "AI" button
    tb_option(t, "AI Assistant", TB_AI);
    tb_end(t);

    tb_root_begin(t, "Help");
    tb_option(t, "Controls", TB_CONTROLS);
    tb_option(t, "Guide", TB_GUIDE);
    tb_end(t);

    tb_option(t, "Documentation", TB_DOCUMENTATION);
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

// panel.right.objectList ("Scene Graph", UI:6258): a Focus button per object (focusObject)
static void object_focus(EdCtl *c) {
    if (c->arg >= 0 && (size_t) c->arg < g_ed.proj->count) editor_frame_object(g_ed.proj->objects[c->arg]);
}

void ed_panel_right_object_list(void) {
    if (ed_project_background()) {
        g_ed.ui_stale = 1;
        return;
    }
    g_ed.panel_hidden[1] = 0;
    if (!edp_right_can_show("objectList")) return;
    EdBuild b;
    edp_right_begin(&b, "objectList");
    ed_gui_panel_buttons(&b, 1, "Objects", 0);
    for (size_t i = 0; i < g_ed.proj->count; i++) {
        char label[64];
        edp_const_name(editor_prefabs[g_ed.proj->objects[i]->prefab].name, label, sizeof(label)); // formatConstName(objInsts[i].name)
        ed_gui_button_line(&b, label, "Focus", object_focus, 0)->arg = (int) i;
    }
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
    else if (!strcmp(name, "objectList")) ed_panel_right_object_list();
    else if (!strcmp(name, "gui")) ed_panel_right_gui(); // editor_customgui.c
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
// windows (window.windows[n].gen into #menuWindow, E:373-985; blueprints UI:6562-7632)

static void win_settings_changed(EdCtl *c, const cJSON *v) {
    (void) c; (void) v;
    editor_settings_save();
}

// GUI._window keys: onChange(value) = setSettings(key, value.toUpperCase())
static void win_key_changed(EdCtl *c, const cJSON *v) {
    if (cJSON_IsString(v) && c->json) {
        char up[16];
        size_t i = 0;
        for (; v->valuestring[i] && i + 1 < sizeof(up); i++) up[i] = (char) toupper((unsigned char) v->valuestring[i]);
        up[i] = 0;
        ed_json_set_str(c->json, c->key, up);
    }
    editor_settings_save();
    // faceModeKey / groupModeKey / wireframeKey also refresh their toggles
    if (!strcmp(c->key, "faceModeKey")) editor_update_face_selection();
    else if (!strcmp(c->key, "groupModeKey")) editor_update_group_selection();
    else if (!strcmp(c->key, "wireframeKey")) editor_update_wireframe();
}

// speedNormal / speedSprint: Math.abs(value) || the default
static void win_speed_changed(EdCtl *c, const cJSON *v) {
    const double x = fabs(cJSON_IsNumber(v) ? v->valuedouble : cJSON_IsString(v) ? atof(v->valuestring) : 0);
    ed_json_set_num(g_ed.settings, c->key, x > 0 ? x : !strcmp(c->key, "speedSprint") ? 180 : 70);
    editor_settings_save();
}

static void win_reset_speed(EdCtl *c) {
    (void) c;
    ed_json_set_num(g_ed.settings, "speedNormal", 70);
    ed_json_set_num(g_ed.settings, "speedSprint", 180);
    editor_settings_save();
    ed_window_show(1);
}

static int g_controls_tab;

static void key_bind(EdBuild *b, const char *name, const char *key, const char *extra) {
    edp_json_key_extra(b, name, g_ed.settings, key, 1, 0, win_key_changed, extra);
}

static void window_controls(EdBuild *b) {
    edb_add(&b->html, "<div style='margin-bottom:15px'><div class=\"windowHeader\">\n"
                      "\t\t\t\t<div style=\"cursor: pointer; margin-right:5px\"onclick=\"showWindow(3)\">\n"
                      "\t\t\t\t\t<i class=\"material-icons\" style=\"font-weight:bold; transform:rotate(-135deg);\">call_made</i>\n"
                      "\t\t\t\t</div>\n\t\t\t\t<div>Controls</div>\n\t\t\t</div>\n\t\t\t<div class='menuTabsNew'>");
    static const char *tabs[2] = {"Default", "Advanced"};
    for (int i = 0; i < 2; i++) edb_addf(&b->html, "<div class=\"menuTabNew %s\" onclick=\"windows[0].switchTab(%d)\">%s</div>", g_controls_tab == i ? " tabANew" : "", i, tabs[i]);
    edb_add(&b->html, "</div></div>\n\t\t\t<div class=\"windowBody\">");
    cJSON *s = g_ed.settings;
    if (g_controls_tab == 0) {
        ed_gui_rule(b);
        ed_gui_folder_begin(b, "Movement", 0, 0, 0, "controls.movement");
        key_bind(b, "Fly forward", "fowardKey", arrow_html("up"));
        key_bind(b, "Fly backwards", "backwardKey", arrow_html("down"));
        key_bind(b, "Fly left", "leftKey", arrow_html("left"));
        key_bind(b, "Fly right", "rightKey", arrow_html("right"));
        key_bind(b, "Fly up", "upKey", NULL);
        key_bind(b, "Fly down", "downKey", NULL);
        key_line(b, "Hold to fly faster", "Shift");
        ed_gui_folder_begin(b, "Speed", 1, 0, 0, "controls.speed");
        edp_json_number(b, "Normal", s, "speedNormal", "", 1, win_speed_changed);
        edp_json_number(b, "Sprinting", s, "speedSprint", "", 1, win_speed_changed);
        ed_gui_button_line(b, "Reset Speed", "Reset", win_reset_speed, 1);
        ed_gui_folder_end(b, 1);
        ed_gui_folder_end(b, 0);
        ed_gui_folder_begin(b, "Transform", 0, 0, 0, "controls.transform");
        key_bind(b, "Translate", "translateKey", NULL);
        key_bind(b, "Rotate", "rotateKey", NULL);
        key_bind(b, "Scale", "scaleKey", NULL);
        key_bind(b, "Toggle World / Local space", "transformSpaceKey", NULL);
        ed_gui_folder_end(b, 0);
        ed_gui_folder_begin(b, "Edit", 0, 0, 0, "controls.edit");
        key_line(b, "Undo", "Ctrl + Z");
        key_line(b, "Redo", "Ctrl + Y or Shift + Ctrl + Z");
        ed_gui_folder_end(b, 0);
        ed_gui_folder_begin(b, "Object", 0, 0, 0, "controls.object");
        key_bind(b, "Face Selection Mode", "faceModeKey", NULL);
        key_bind(b, "Group Selection Mode", "groupModeKey", NULL);
        key_bind(b, "Wireframe Mode", "wireframeKey", NULL);
        key_line(b, "Delete element", "Del or Backspace");
        key_line(b, "Toggle snapping", "Control");
        key_line(b, "Toggle collidable", "Shift + C");
        key_line(b, "Toggle visibilty", "Shift + V");
        ed_gui_folder_end(b, 0);
    } else {
        key_bind(b, "Open Texture/Model List", "textureKey", NULL);
        key_bind(b, "Open Scripting Window", "scriptingKey", NULL);
        key_bind(b, "Create placeholder", "placeHolderKey", NULL);
        key_bind(b, "Create Cube", "createCubeKey", NULL);
        key_bind(b, "Create Gate", "createGateKey", NULL);
        key_bind(b, "Create Trigger", "createTriggerKey", NULL);
        key_line(b, "Hitbox fixer", "Shift + F");
        key_line(b, "Flip x/z size", "F");
        key_bind(b, "Quick Add", "quickAddKey", NULL);
        ed_gui_rule(b);
        ed_gui_folder_begin(b, "Group", 0, 0, 0, "controls.group");
        key_line(b, "Remove all groups", "Shift + Alt + G");
        key_line(b, "Create group / Remove group if exist", "Shift + G");
        key_line(b, "Duplicate Group / Object", "Shift + R");
        key_line(b, "Copy group", "Ctrl + C");
        key_line(b, "Paste group", "Ctrl + V");
        ed_gui_folder_end(b, 0);
        // blueprint.file is assigned twice in v7.1.4: only the second File folder survives
        ed_gui_folder_begin(b, "File", 0, 0, 0, "controls.file");
        key_line(b, "Paste preset JSON", "Shift + Click (w/ Objectpainter)");
        ed_gui_folder_end(b, 0);
    }
    edb_add(&b->html, "</div>");
}

static void on_setting(EdCtl *c, const cJSON *v) { (void) c; (void) v; editor_settings_save(); }

static void on_ui_scale(EdCtl *c, const cJSON *v) {
    (void) c; (void) v;
    editor_settings_save();
    editor_scale_ui(); // scaleUI
}

static void on_auto_backups(EdCtl *c, const cJSON *v) {
    (void) c;
    if (c->bind == EB_JSON) ed_json_set_bool(g_ed.settings, "autoBackups", ed_json_truthy(v));
    editor_settings_save();
    ed_backup_reset_timer(g_ed.now); // initAutoBackup
}

static void on_backup_timer(EdCtl *c, const cJSON *v) {
    (void) c;
    ed_json_set_num(g_ed.settings, "backupTimer", fabs(cJSON_IsNumber(v) ? v->valuedouble : 10));
    editor_settings_save();
    ed_backup_reset_timer(g_ed.now);
}

static void on_disable_warnings(EdCtl *c, const cJSON *v) {
    (void) c; (void) v;
    editor_settings_save();
    editor_clear_warnings(); // clearWarnings
    g_ed.dirty_counts = 1;
}

static void on_anti_alias(EdCtl *c, const cJSON *v) {
    (void) c; (void) v;
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
    editor_scale_ui();
    editor_show_window(3);
}

// Settings > Object: Lightcone / Model & Asset bounding box toggles (toggleLightCones /
// toggleModels, E:5100-5114): the notices; the helpers' visibility is editor_render.c's
static void toggle_light_cones(EdCtl *c) {
    (void) c;
    g_ed.hide_light_cones = !g_ed.hide_light_cones;
    editor_warning("lightconeabuse", g_ed.hide_light_cones, "Light Cones are hidden", NULL);
}

static void toggle_models(EdCtl *c) {
    (void) c;
    g_ed.hide_models = !g_ed.hide_models;
    editor_warning("showModels", g_ed.hide_models, "Models & Assets are hidden", NULL);
}

static int g_settings_tab;

static EdCtl *div_switch(EdBuild *b, const char *name, const char *key, const char *div, const int invert) {
    EdCtl *c = edp_json_switch(b, name, g_ed.settings, key, 0, on_toggle_div);
    snprintf(c->path, sizeof(c->path), "%s", div);
    c->arg = invert;
    return c;
}

// v10's multiplayer settings (E10:9416-9468: localStorage "mp_settings", read by KE.multiplayer;
// the Settings window's Multiplayer tab, IDX:21705-21760). Kept for parity; there is no editor room
// server (window_multiplayer).
static cJSON *g_mp_settings;

static cJSON *mp_settings(void) {
    if (g_mp_settings) return g_mp_settings;
    g_mp_settings = cJSON_Parse("{\"name\":\"Player\",\"renderOutlines\":true,\"playerOpacity\":0.6,\"streamerMode\":false,\"autoChatScroll\":true}");
    const int n = html_storage_get("mp_settings", NULL, 0);
    char *buf = n > 0 ? malloc((size_t) n + 1) : NULL;
    if (buf && html_storage_get("mp_settings", buf, (size_t) n + 1) >= 0) {
        cJSON *saved = cJSON_Parse(buf);
        const cJSON *it;
        cJSON_ArrayForEach(it, saved) {
            if (cJSON_GetObjectItemCaseSensitive(g_mp_settings, it->string))
                cJSON_ReplaceItemInObjectCaseSensitive(g_mp_settings, it->string, cJSON_Duplicate(it, 1));
        }
        cJSON_Delete(saved);
    }
    free(buf);
    return g_mp_settings;
}

static void on_mp_setting(EdCtl *c, const cJSON *v) {
    (void) c; (void) v;
    char *text = cJSON_PrintUnformatted(mp_settings());
    if (text) html_storage_set("mp_settings", text);
    free(text);
}

static void window_settings(EdBuild *b) {
    // windows[2]: tabs General / Display / Object / Quick GUI (GUI._window.settings.*), v10: + Multiplayer (E10:13035)
    static const char *tabs[5] = {"General", "Display", "Object", "Quick GUI", "Multiplayer"};
    EdCtl *reset = ed_gui_ctl(b->panel, EG_BUTTON);
    reset->action = reset_all;
    edb_addf(&b->html, "<div style='margin-bottom:15px'><div class=\"windowHeader\"><div>Settings</div>\n"
                       "\t\t\t<div style=\"cursor:pointer;margin-top:5px;font-size:14px;float:right;color:#FFF;user-select:none\" onclick=\"GUI.action(%d, this)\">Reset All</div>\n"
                       "\t\t\t</div><div class='menuTabsNew'>\n\t\t\t", reset->id);
    for (int i = 0; i < 5; i++) edb_addf(&b->html, "<div class=\"menuShortTabNew %s\" onclick=\"windows[2].switchTab(%d)\">%s</div>", g_settings_tab == i ? " tabANew" : "", i, tabs[i]);
    edb_add(&b->html, "</div></div>\n\t\t\t<div class=\"windowBody\">");
    cJSON *s = g_ed.settings;
    if (g_settings_tab == 0) {
        ed_gui_button_line(b, "Controls", "Open Controls", open_controls, 0);
        edp_json_switch(b, "Auto Backup", s, "autoBackups", 0, on_auto_backups);
        edp_json_slider(b, "Backup Timer (Minutes)", s, "backupTimer", 1, 30, 1, 0, on_backup_timer);
        edp_json_slider(b, "UI Scale", s, "uiScale", 0.5, 1.5, 0.1, 0, on_ui_scale);
        edp_json_switch(b, "Pointer Lock", s, "pointerLock", 0, on_setting);
        edp_json_switch(b, "Disable Warnings", s, "disableWarnings", 0, on_disable_warnings);
        edp_json_switch(b, "Test Map in Editor", s, "testInEditor", 0, on_setting);
        edp_json_switch(b, "Autologin", s, "autoLog", 0, on_setting);
        ed_gui_rule(b);
        ed_gui_folder_begin(b, "Render", 0, 1, 0, "settings.render");
        edp_json_slider(b, "Resolution", s, "resMltV", 0.1, 2, 0.1, 0, on_setting);
        // v7.1.4 binds this switch to "resMltV" and stores antiAlias on change; here it shows
        // and stores antiAlias (intentional difference from v7.1.4, doc/v7/editor.md 1.13)
        edp_json_switch(b, "Anti-aliasing", s, "antiAlias", 0, on_anti_alias);
        edp_json_slider(b, "Render Distance", s, "renderDist", 100, 20000, 50, 0, on_setting);
        ed_gui_folder_end(b, 0);
        ed_gui_folder_begin(b, "History", 0, 1, 0, "settings.history");
        div_switch(b, "Disable", "disableHistory", "grid", 1);
        edp_div_begin(b, "grid", !editor_setting_bool("disableHistory"));
        edp_json_slider(b, "Limit", s, "historyLimit", 0, 1000, 1, 1, on_setting);
        edb_add(&b->html, "</div>\n");
        ed_gui_folder_end(b, 0);
        ed_gui_folder_begin(b, "Game", 0, 1, 0, "settings.game");
        edp_json_switch(b, "Show Real Hitbox", s, "showRealHitbox", 0, on_setting);
        ed_gui_folder_end(b, 0);
        ed_gui_folder_begin(b, "Scripting", 0, 1, 0, "settings.scripting");
        edp_json_slider(b, "Scripting Font Size", s, "scriptFont", 5, 30, 1, 0, on_setting);
        ed_gui_folder_end(b, 0);
    } else if (g_settings_tab == 1) {
        edp_json_switch(b, "Show FPS", s, "showFPS", 0, on_setting);
        edp_json_switch(b, "Dark Mode", s, "darkMode", 0, on_setting);
        ed_gui_rule(b);
        ed_gui_folder_begin(b, "Grid", 0, 0, 0, "settings.grid");
        div_switch(b, "Visible", "gridVisibility", "grid", 0);
        edp_div_begin(b, "grid", editor_setting_bool("gridVisibility"));
        edp_json_slider(b, "Opacity", s, "gridOpacity", 0.05, 1, 0.05, 1, on_setting);
        edp_json_number(b, "Size", s, "gridSize", "", 1, on_setting);
        edp_json_number(b, "Spacing", s, "gridSpacing", "", 1, on_setting);
        edb_add(&b->html, "</div>\n");
        ed_gui_button_line(b, "Reset Grid", "Reset", grid_reset, 0);
        ed_gui_folder_end(b, 0);
        ed_gui_folder_begin(b, "Death Barrier", 0, 0, 0, "settings.db");
        div_switch(b, "Visible", "dbVisibility", "deathBarrier", 0);
        edp_div_begin(b, "deathBarrier", editor_setting_bool("dbVisibility"));
        edp_json_slider(b, "Opacity", s, "dbOpacity", 0.05, 1, 0.05, 1, on_setting);
        edb_add(&b->html, "</div>\n");
        ed_gui_button_line(b, "Reset Barrier", "Reset", db_reset, 0);
        ed_gui_folder_end(b, 0);
    } else if (g_settings_tab == 2) {
        edp_json_switch(b, "Object Connections", s, "triggerCons", 0, on_setting);
        edp_json_switch(b, "Select through invisible objects [ALT]", s, "selectThroughInvis", 0, on_setting);
        edp_json_switch(b, "Object Border Visuals", s, "borderVisuals", 0, on_setting);
        edp_json_switch(b, "Highlight", s, "objectHighlight", 0, on_setting);
        edp_json_switch(b, "Create Near", s, "createObjectNear", 0, on_setting);
        ed_gui_button_line(b, "Lightcone Boundingbox", "Toggle", toggle_light_cones, 0);
        ed_gui_button_line(b, "Model & Asset Boundingbox", "Toggle", toggle_models, 0);
        ed_gui_rule(b);
        ed_gui_folder_begin(b, "Config", 0, 0, 0, "settings.config");
        edp_json_switch(b, "Anti Radians", s, "degToRad", 0, on_setting);
        edp_json_switch(b, "Preserve Folders", s, "preserveFolderState", 0, on_setting);
        ed_gui_folder_end(b, 0);
        ed_gui_folder_begin(b, "Snapping", 0, 0, 0, "settings.snapping");
        edp_json_slider(b, "Translation", s, "translationSnapping", 0.001, 25, 0.001, 0, on_setting);
        edp_json_slider(b, "Rotation", s, "rotationSnapping", 0.001, 45, 0.001, 0, on_setting);
        edp_json_slider(b, "Scale", s, "scaleSnapping", 0.001, 25, 0.001, 0, on_setting);
        ed_gui_button_line(b, "Reset Snapping", "Reset", snapping_reset, 0);
        ed_gui_folder_end(b, 0);
    } else if (g_settings_tab == 4) {
        cJSON *m = mp_settings();
        edp_json_text(b, "Player Name", m, "name", "", 0, on_mp_setting);
        edp_json_switch(b, "Render Coop Outlines", m, "renderOutlines", 0, on_mp_setting);
        edp_json_slider(b, "Player Opacity", m, "playerOpacity", 0, 1, 0.1, 0, on_mp_setting);
        edp_json_switch(b, "Streamer Mode (hide room ID)", m, "streamerMode", 0, on_mp_setting);
        edp_json_switch(b, "Auto Chat Scroll", m, "autoChatScroll", 0, on_mp_setting);
    } else {
        static const EdOption open_on[] = {{"Middle Click", 0, NULL}, {"Right Click", 1, NULL}};
        div_switch(b, "Enabled", "quickGUI", "quickgui", 0);
        edp_div_begin(b, "quickgui", editor_setting_bool("quickGUI"));
        edp_json_select(b, "Open On", s, "quickGUIOpen", open_on, 2, 1, on_setting);
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
        // every prefab whose formatted name contains it, sorted by name length (stable)
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
            cJSON_AddNumberToObject(o, "size", 0.5);
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
        if (!first) edb_add(h, "\n");
        first = 0;
        if (blank && !g_quick_edit) {
            t++;
            continue;
        }
        const char *name = ed_json_str(e, "name", "");
        char img[96];
        size_t k = 0;
        for (const char *p = name; *p && k + 1 < sizeof(img); p++) if (*p != ' ') img[k++] = (char) tolower((unsigned char) *p);
        img[k] = 0;
        if (g_quick_edit) edb_addf(h, "<div class='quickAddButton'><div class='previewDesc' onclick='windows[3].showCustomize(%d)'><div>", t);
        else edb_addf(h, "<div class='quickAddButton'><div class='previewDesc' onclick='windows[3].clickButton(%d)'><div>", t);
        const char *desc = ed_json_str(e, "desc", "");
        edb_add_escaped(h, g_quick_edit ? "Click to change item" : desc[0] ? desc : "Click to add");
        edb_addf(h, "</div></div><div class='previewDeleteIcon'%sonclick='windows[3].deleteButton(%d)'><i class='material-icons'>delete_forever</i></div><div class='previewImg'",
                 g_quick_edit && !blank ? "" : "style='display:none'", t);
        if (!blank) edb_addf(h, "style='background-size:%d%%;background-image:url(img/%s.png)'", ed_json_truthy(cJSON_GetObjectItemCaseSensitive(e, "cust")) ? 80 : 50, img);
        edb_addf(h, "><div %s>", blank ? "style='margin-top:50%;transform:translateY(-50%)'" : "");
        if (blank) edb_add(h, "<i class='material-icons' style='font-size:65px;color:#FFF'>add</i>");
        else edb_add_escaped(h, name);
        edb_add(h, "</div></div></div>");
        t++;
    }
}

static void window_quick_add(EdBuild *b) {
    edb_addf(&b->html, "<div class='windowHeader'><div>Quick Add</div><input type='text' id='quickSearch' class='smlInput' placeholder='Search' onkeyup='windows[3].search()'><div id='toglEditBtn' onclick='windows[3].toggleEditMode()'>%s</div></div><div class='windowBody'><div id='buttonGrid' class='buttonGrid'>",
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
            if (!prefab_listed(p)) continue;
            const int cat = (p->flags & (EP_TOOL | EP_TOOL2)) ? 2 : (p->flags & EP_GEN) ? 1 : 0;
            if (cat != f) continue;
            ed_gui_button_line(b, p->label, "Add", quick_cust_add, 0)->arg = i;
        }
        ed_gui_folder_end(b, 0);
    }
    edb_add(&b->html, "</div>");
}

static void quick_click(const int idx) {
    // windows[3].clickButton(i)
    const cJSON *b = cJSON_GetArrayItem(g_quick_current, idx);
    if (!b) return;
    if (ed_json_truthy(cJSON_GetObjectItemCaseSensitive(b, "cust"))) {
        ed_obj_create(ED_PREFAB_CUSTOM_ASSET); // createObjects[44]: the asset browser
    } else if (ed_json_truthy(cJSON_GetObjectItemCaseSensitive(b, "place"))) {
        ed_obj_create_placeholder();
        editor_close_window();
    } else {
        const int item = (int) ed_json_num(b, "item", 0);
        ed_obj_create(item);
        if (item != ED_PREFAB_CUSTOM_ASSET) editor_close_window();
    }
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
        quick_click(idx);
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
        if (html_key_is_enter(ev->key) && cJSON_GetArraySize(g_quick_current)) quick_click(0);
    }
    return 1;
}

// windows[7] "Welcome": the animated counters (vA499) run in ed_window_tick
static struct { const char *id; double max, t, val; } g_counters[3];

// windows[7] "Welcome", Krunker v10's (E10:13296-13323): v7.1.4's "and earn real money", the
// "$X+ paid to developers" item, "or worrying about monetization" and the Scripting Community
// link are gone (animVl2 is still counted, as in v10, with no element to show it)
static void window_welcome(EdBuild *b) {
    EdBuf *h = &b->html;
    edb_add(h, "<div id='welcomeVideoHolder'>\n\t\t\t<div id='welcomeHeader'>\n\t\t\t\t<div id='welcomeTitle'>Welcome to the Krunker Editor</div>\n"
               "\t\t\t\t<div id='welcomeSubTitle'>Design, Create & Publish for multiple platforms!</div>\n"
               "\t\t\t\t<div id='welcomeButtons'>\n\t\t\t\t\t<div class='welcomeButton' onclick='KE.quickStart()'>Quick Start</div>\n"
               "\t\t\t\t\t<div class='welcomeButton' onclick='showWindow(14)'>My Games</div>\n"
               "\t\t\t\t\t<div class='welcomeButton' onclick='showWindow(10)'>Templates</div>\n"
               "\t\t\t\t\t<div class='welcomeButton' onclick='showWindow(11)'>Publishing</div>\n"
               "\t\t\t\t\t<div class='welcomeButton' onclick='openLink(\"//docs.krunker.io\", \"_blank\")'>Documentation</div>\n"
               "\t\t\t\t</div>\n\t\t\t</div>\n\t\t</div>");
    g_counters[0].id = "animVl0", g_counters[0].max = 160000000, g_counters[0].t = 11000;
    g_counters[1].id = "animVl1", g_counters[1].max = 12, g_counters[1].t = 5000;
    g_counters[2].id = "animVl2", g_counters[2].max = 750000, g_counters[2].t = 9000;
    for (int i = 0; i < 3; i++) g_counters[i].val = 0;
    edb_add(h, "<div style='padding: 50px; background-color: #383838; transform: translateY(0); user-select: none'>\n"
               "\t\t\t<div id='editInfoBody' class='editInfoTable'>\n\t\t\t\t<div class='editInfoItem'>\n"
               "\t\t\t\t\t<div class='editInHedr'><span id='animVl0' class='editInAvl'></span>+ Users since 2018</div>\n"
               "\t\t\t\t\t<div>The Krunker Editor allows you to create <b>Cross-Platform Games</b> quickly and easily to share with <b>\n"
               "\t\t\t\t\tmillions of players</b> without prior knowledge on Game Development</div>\n\t\t\t\t</div>\n"
               "\t\t\t\t<div class='editInfoItem'>\n\t\t\t\t\t<div class='editInHedr'><span id='animVl1' class='editInAvl'></span> Dedicated Server Regions</div>\n"
               "\t\t\t\t\t<div>Using Krunker as a Platform to Create &amp; Publish your game, allows you to <b>bypass the hassle\n"
               "\t\t\t\t\tof hosting servers</b>.</div>\n\t\t\t\t</div>\n\t\t\t</div>\n\t\t</div>");
    edb_add(h, "<div id='welcomeVideoHolder' class='welcomeBG2'>\n\t\t\t<div id='welcomeHeader' style='height:100%;'>\n"
               "\t\t\t\t<div id='welcomeTitle' style='position:absolute;top:20px;left:50px;'>\n\t\t\t\t\tKrunker in the Classroom\n\t\t\t\t</div>\n"
               "\t\t\t\t<div id='welcomeSubTitle' style='position:absolute;\n\t\t\t\t\tleft:50px;text-align:left;top:120px;width:800px;'>\n"
               "\t\t\t\t\tFollow along in customizable coding lessons. Create & prototype\n\t\t\t\t\twithin minutes and share with the whole class!\n\t\t\t\t</div>\n"
               "\t\t\t\t<div class='learnMorLnk'>\n\t\t\t\t\t<a href='./edu.html' style='color:#2196F3;'>Learn more</a>\n\t\t\t\t</div>\n\t\t\t</div>\n\t\t</div>");
    edb_add(h, "<div style='padding: 50px 50px 60px; background-color: #222; transform: translateY(0); user-select: none'>\n"
               "\t\t\t<div class='editInfoTable'>\n\t\t\t\t<div class='editInfoItem'>\n"
               "\t\t\t\t\t<div class='edInfHeader' style='color:#db4f6b !important;font-size:80px;margin-bottom:-10px;'>Learn</div>\n"
               "\t\t\t\t\t<div class='edInfBody'>Combine Maths, Arts, Programming, Geometry and Design to create fun & simple experiences</div>\n\t\t\t\t</div>\n"
               "\t\t\t\t<div class='editInfoItem'>\n\t\t\t\t\t<div class='edInfHeader' style='font-size:80px;margin-bottom:-10px;'>Share</div>\n"
               "\t\t\t\t\t<div class='edInfBody'>Share your creation easily with your class or thousands of other students.</div>\n\t\t\t\t</div>\n"
               "\t\t\t\t<div class='editInfoItem'>\n\t\t\t\t\t<div class='edInfHeader' style='color:#f2a82c !important;font-size:80px;margin-bottom:-10px;'>Reward</div>\n"
               "\t\t\t\t\t<div class='edInfBody'>Teachers can create custom lessons with unique reward systems for their students</div>\n\t\t\t\t</div>\n\t\t\t</div>\n\t\t</div>");
    // the community banner's background is assets.krunker.io/img/menu-bg/4.png in v7: the
    // imported menu background (ui/img/menu-bg/) here
    edb_add(h, "<div style='height:250px;position:relative;background-color:#3a3a3a;user-select:none;\n"
               "\t\t\tbackground-image:linear-gradient(#48484869,#48484869),\n\t\t\turl(img/menu-bg/4.png) !important;background-position-y:-200px;\n"
               "\t\t\tbackground-size:cover;'>\n\t\t\t<div id='communityTable'>\n\t\t\t\t<div style='position:absolute;left:50%;top:50%;\n"
               "\t\t\t\t\ttransform:translate(-50%,-50%);width:100%;'>\n\t\t\t\t\t<div style='display:inline-block;'>\n"
               "\t\t\t\t\t\t<div id='communityIcon'></div>\n\t\t\t\t\t\t<div id='communityInfo'>Mapping Community<div></div>\n"
               "\t\t\t\t\t\t<a target='_blank' href='https://discord.gg/Kfypyp5' class='popupLink'>Join Now!</a></div>\n\t\t\t\t\t</div>\n"
               "\t\t\t\t</div>\n\t\t\t</div>\n\t\t</div>");
}

// UTILS.commaFormatNum
static void comma_format(const double v, char *out, const size_t n) {
    char digits[32];
    snprintf(digits, sizeof(digits), "%.0f", v);
    const size_t len = strlen(digits);
    size_t j = 0;
    for (size_t i = 0; i < len && j + 2 < n; i++) {
        if (i && (len - i) % 3 == 0) out[j++] = ',';
        out[j++] = digits[i];
    }
    out[j] = 0;
}

void ed_window_tick(const float dt) {
    // Editor.render: each counter rises to its max in t ms
    for (int i = 0; i < 3; i++) {
        if (!g_counters[i].id) continue;
        const HtmlElem e = html_dom_by_id(g_counters[i].id);
        if (!e) continue;
        g_counters[i].val += dt * 1000.0 / g_counters[i].t * g_counters[i].max;
        if (g_counters[i].val >= g_counters[i].max) g_counters[i].val = g_counters[i].max;
        char text[32];
        comma_format(round(g_counters[i].val), text, sizeof(text));
        html_dom_set_text(e, text);
    }
}

// windows[9] "Templates": script templates, game templates, default maps (loadQuick), then
// the Import / Load / New / Guide tiles
static void templ_pairs(EdBuf *h, const cJSON *list, const int script, const int map) {
    const int n = cJSON_GetArraySize(list), pairs = (n + 1) / 2;
    for (int i = 0; i < pairs; i++) {
        for (int k = 0; k < 2; k++) {
            const int idx = i * 2 + k;
            const cJSON *t = cJSON_GetArrayItem(list, idx);
            if (!t) break;
            char img[160];
            if (map) snprintf(img, sizeof(img), "img/maps/map_%d.png", idx);
            else if (script) snprintf(img, sizeof(img), "img/templates/scripts/%d.png", idx);
            else snprintf(img, sizeof(img), "img/templates/%d.png", (int) ed_json_num(t, "icon", 0)); // icon: a number (templates.json)
            if (!k) edb_addf(h, "<div class='editInfoPair'%s>", i == pairs - 1 && map ? "" : "style='margin-right: 10px'");
            edb_addf(h, "<div class='editInfoView' style='background-image:url(%s);%s%s' onclick='loadQuick(%d,%s,%s)'><div class='editInfoText'>", img,
                     k ? "margin-top: 10px;" : "", map ? "background-size: cover;" : "", idx, script ? "true" : "false", map ? "true" : "undefined");
            edb_add_escaped(h, ed_json_str(t, "name", ""));
            edb_addf(h, "</div>%s</div>", map ? "<div class='tempITag'>Default Map</div>" : script ? "<div class='tempITag'>KrunkScript</div>" : "");
        }
        edb_add(h, "</div>");
    }
}

static void window_templates(EdBuild *b) {
    EdBuf *h = &b->html;
    edb_add(h, "<div style='text-align:center;font-size:50px;color:#fff;\n\t\t\tmargin-top:20px'>Basic Game Templates\n\t\t</div>\n"
               "\t\t<div style='text-align:center;color:rgba(255, 255, 255, 0.3);margin-bottom:20px;\n\t\t\tmargin-top: 5px;font-size:23px'>\n"
               "\t\t\tUse these templates to Quickstart your Project\n\t\t</div>");
    edb_add(h, "<div class='editInfoScroll'>");
    const cJSON *t = ed_templates_get();
    templ_pairs(h, cJSON_GetObjectItemCaseSensitive(t, "script"), 1, 0);
    templ_pairs(h, cJSON_GetObjectItemCaseSensitive(t, "game"), 0, 0);
    templ_pairs(h, cJSON_GetObjectItemCaseSensitive(t, "defaultMaps"), 0, 1);
    edb_add(h, "</div>");
    // v7.1.4's Load tile quotes 'importMap' inside a single-quoted attribute (the click is a
    // syntax error there); fixed here (intentional difference)
    edb_add(h, "<div style='user-select: none;'>\n\t\t\t<div class='editInfoTable' style='grid-template-columns: auto auto auto auto;\n\t\t\t\t\tmargin-bottom:20px;'>\n"
               "\t\t\t\t<div class='editInfoItem templates' onclick='closeWindow();KE.importMapPopup()'>\n"
               "\t\t\t\t\t<span class='material-icons templates'>file_copy</span>\n\t\t\t\t\t<div class='edInfHeader templates'>Import</div>\n\t\t\t\t</div>\n"
               "\t\t\t\t<div class='editInfoItem templates' onclick='closeWindow();KE.loadFile(&quot;importMap&quot;)'>\n"
               "\t\t\t\t\t<span class='material-icons templates'>folder_open</span>\n\t\t\t\t\t<div class='edInfHeader templates'>Load</div>\n\t\t\t\t</div>\n"
               "\t\t\t\t<div class='editInfoItem templates' onclick='closeWindow()'>\n"
               "\t\t\t\t\t<span class='material-icons templates'>create_new_folder</span>\n\t\t\t\t\t<div class='edInfHeader templates'>New</div>\n\t\t\t\t</div>\n"
               "\t\t\t\t<div class='editInfoItem templates' onclick='showWindow(8)'>\n"
               "\t\t\t\t<span class='material-icons templates'>menu_book</span>\n\t\t\t\t<div class='edInfHeader templates'>Guide</div>\n\t\t\t</div>\n"
               "\t\t\t</div>\n\t\t</div>");
}

// windows[10] "Publishing": the static guide (images from v7's asset host, img/publishing/)
static void window_publishing(EdBuild *b) {
    edb_add(&b->html,
            "<div style='text-align:center;font-size:30px;color:#fff;margin-bottom:20px'>Publishing Guide</div>"
            "<div class='pubGIt'><span style='color:#fff'>1.</span> Create a Thumbnail</div>"
            "<div class='pubGItSub'>Tip: Use bright colors and include the name of your Game to help it stand out</div><div class='pubGRule'></div>"
            "<div class='pubGImgSplit'><div><img class='pubGImg' src='img/publishing/1_bad.png'/><div class='material-icons' style='color:red !important'>clear</div></div>"
            "<div><img class='pubGImg' src='img/publishing/1_good.png'/><div class='material-icons' style='color:#09c709 !important'>done</div></div><div></div></div>"
            "<div class='pubGIt'><span style='color:#fff'>2.</span> Click <a class='popupLink' onclick='KE.publishMap()'>here</a> to publish your Game</div><div class='pubGRule'></div>"
            "<div class='pubGImgArrow material-icons' style='transform:translate(498px, 97px);color:rgba(255, 255, 255, 0.1) !important'>arrow_right_alt</div>"
            "<img class='pubGImg' style='display:inline-block;margin-bottom:40px;margin-left:100px' src='img/publishing/2_main.png'/>"
            "<img class='pubGImg' style='display:inline-block;margin-left:20px;margin-left:90px' src='img/publishing/2_options.png'/>"
            "<div class='pubGIt'><span style='color:#fff'>3.</span> Once published you can share the game link to invite new players.</div>"
            "<div class='pubGItSub'>If you want your own custom URL. You can register one <a class='popupLink' target='_blank' href='https://www.namecheap.com/'>here</a> and iFrame your game link</div><div class='pubGRule'></div>"
            "<div class='pubGImgArrow material-icons' style='transform:translate(438px, 0px);color:rgba(255, 255, 255, 0.1) !important'>arrow_right_alt</div>"
            "<div class='pubGImgArrow material-icons' style='transform:translate(393px, 200px);color:rgba(255, 255, 255, 0.1) !important'>arrow_right_alt</div>"
            "<img class='pubGImg' style='max-width:45%;display:inline-block;margin-left:20px' src='img/publishing/3_main.png'/>"
            "<div style='max-width:45%;display:inline-block;margin-left:50px'><img class='pubGImg' style='max-width:100%;display:block;margin-left:45px'src='img/publishing/3_search.png'/>"
            "<img class='pubGImg' style='max-width:100%;display:block;margin-top:33px' src='img/publishing/3_share.png'/></div>"
            "<div class='pubGIt'><span style='color:#fff'>4.</span> To help <b>increase plays on your game</b>, we recommend that you publish your game to Online Game Sites like "
            "<a target='_blank' class='popupLink' href='https://developer.crazygames.com/'>Crazy Games</a>. Sharing your game with "
            "<a class='popupLink' target='_blank' href='https://www.twitch.tv/directory/game/Krunker'>Content Creators</a> is also a great way to get additional feedback and exposure."
            "<div style=''><img class='pubGImg' style='max-width:100%;display:block;margin-top:33px' src='img/publishing/4_promo.png'/></div></div>"
            "<div class='pubGIt'><span style='color:#fff'>5.</span> Once your game hits <b>50,000</b> Plays in 30 days you can apply to become a Partner and start earning Revenue.</div><div class='pubGRule'></div>");
}

// windows[11] / [12] "Tutorials" / "Tips & Tricks": YouTube iframes in v7 (config.tutorials /
// tipsAndTricks). The engine has no iframes: each card is a link to the video instead
// (intentional difference from v7.1.4, doc/v7/editor.md open question 3).
static const struct { const char *name, *code; } k_tutorials[] = {
    {"Editor Basics", "Ewtb2rBvg9g"}, {"Basic Controls", "1_HixIbbCM0"}, {"Promotion", "SxGLXklICb8"}, {"Common Issues", "0nT8kdpAi7g"},
    {"Importing Models", "E8JWupugj4s"}, {"Testing & Hosting", "SxM2684Us5I"}, {"Groups & Prefabs", "qPR7EuWXM3A"},
};
static const struct { const char *name, *code; } k_tips[] = {
    {"Decals", "36-oUcgoyd4"}, {"Slippery Ice Effect", "hc4qZD0fJYk"}, {"How to make No-Teleport (better) Ramps", "eP3Kt5HdVeo"},
    {"Fake Mirror", "yVjZGsy74Ao"}, {"Better Glass", "EOaVlaOmgLw"}, {"How to make Objects Fade to Black", "22945ymW_Vs"},
    {"Texture Merging", "4DuWCrdt_6Y"}, {"Angled Rooves", "VZTtKefBk70"}, {"No Kill (safe) Spawns", "99BNfVl8ybI"},
    {"No Glitch Ledges", "hLoeU9tcwg4"}, {"Moving Fabric", "bk6MJiYqp28"}, {"Fire", "xcy8rF9P2jU"}, {"Face Rendering", "KcI12XLM73E"},
    {"Moving Water", "nMoz5AtCXkM"}, {"Bevelled Edges", "Qr_H-f-WgJk"},
};

static void video_card(EdBuf *h, const char *name, const char *code) {
    edb_addf(h, "<div class='editHlpCrd'><a class='editVideo popupLink' target='_blank' href='https://www.youtube.com/watch?v=%s' "
                "style='display:flex;align-items:center;justify-content:center;font-size:20px'>youtube.com/watch?v=%s</a><div class='editVidHeader'>", code, code);
    edb_add_escaped(h, name);
    edb_add(h, "</div></div>");
}

static void window_videos(EdBuild *b, const int tips) {
    edb_addf(&b->html, "<div style='text-align:center;font-size:30px;color:#fff;margin-bottom:20px'>%s</div><div class='editVidHolder'>", tips ? "Tips & Tricks" : "Editor Tutorials");
    if (tips) for (size_t i = 0; i < sizeof(k_tips) / sizeof(k_tips[0]); i++) video_card(&b->html, k_tips[i].name, k_tips[i].code);
    else for (size_t i = 0; i < sizeof(k_tutorials) / sizeof(k_tutorials[0]); i++) video_card(&b->html, k_tutorials[i].name, k_tutorials[i].code);
    edb_add(&b->html, "</div>");
}

// windows[14] "Load Backup": newest first, "date - name"; click to import; Clear All
static void window_backups(EdBuild *b) {
    EdBuf *h = &b->html;
    edb_add(h, "<div style='margin-bottom:15px'><div class=\"windowHeader\">\n\t\t\t\t<div style='display: block;'>Load Backup<div class=\"publishAsset\" onclick=\"windows[14].clear()\">Clear All</div></div>\n\t\t\t</div></div>\n\t\t\t<div class=\"windowBody\">");
    cJSON *list = ed_backup_list();
    const int n = cJSON_GetArraySize(list);
    if (!n) edb_add(h, "No Backups found");
    for (int i = n - 1; i >= 0; i--) {
        const cJSON *e = cJSON_GetArrayItem(list, i);
        const double ts = ed_json_num(e, "timestamp", 0);
        char date[64];
        ed_backup_date(ts, date, sizeof(date));
        edb_addf(h, "<div class='eButton' onclick='KE.importMapBackup(%.0f);closeWindow()'>%s - ", ts, date);
        edb_add_escaped(h, ed_json_str(e, "name", ""));
        edb_add(h, "</div>");
    }
    cJSON_Delete(list);
    edb_add(h, "</div>");
}

// windows[15] / [16] (v10 KE.multiplayer, E10:9595-9646): collaborative editing rooms on Krunker's
// editor socket (mpHost / mpJoin ... on editor.krunker.io/ws). Intentional difference: Krunker Box
// Office has no editor room server, so the window is v10's "no room" form and Host / Join say so.
static const char *const k_mp_unavailable = "Multiplayer editing needs an editor room server, which Krunker Box Office does not have yet.";

static void window_multiplayer(EdBuild *b) {
    edb_add(&b->html, "<div style=\"padding:10px\">\n\t\t\t<div style=\"margin-bottom:15px;padding:10px;background:rgba(255,200,0,0.1);border:1px solid rgba(255,200,0,0.3);border-radius:4px;color:#ffd700;font-size:12px\">\n\t\t\t\tOnly invite people you trust - they have full control over the map\n\t\t\t</div>"
                      "\n\t\t\t<div style=\"margin-bottom:10px\">\n\t\t\t\t<label style=\"display:block;margin-bottom:4px;color:#aaa;font-size:12px\">Max Players</label>\n\t\t\t\t<input type=\"range\" id=\"mpMaxPlayers\" min=\"2\" max=\"20\" value=\"5\" style=\"width:100%\" />\n\t\t\t\t<span id=\"mpMaxPlayersVal\" style=\"color:#fff\">5</span>\n\t\t\t</div>"
                      "\n\t\t\t<div style=\"margin-bottom:10px\">\n\t\t\t\t<label style=\"display:block;margin-bottom:4px;color:#aaa;font-size:12px\">Room Password (optional)</label>\n\t\t\t\t<input type=\"text\" id=\"mpPassword\" placeholder=\"Leave empty for no password\"\n\t\t\t\t\tstyle=\"width:100%;box-sizing:border-box;padding:6px;background:#333;color:#fff;border:1px solid #555;border-radius:3px\" />\n\t\t\t</div>"
                      "\n\t\t\t<div style=\"margin-bottom:10px\">\n\t\t\t\t<label style=\"color:#aaa;font-size:12px\">\n\t\t\t\t\t<input type=\"checkbox\" id=\"mpCloseOnDisconnect\" checked /> Close room when host disconnects\n\t\t\t\t</label>\n\t\t\t</div>"
                      "\n\t\t\t<div style=\"display:flex;gap:8px\">\n\t\t\t\t<button class=\"mpBtnPrimary\" onclick=\"KE.multiplayer.hostFromUI()\" style=\"flex:1\">Host Room</button>\n\t\t\t\t<button class=\"mpBtnPrimary\" onclick=\"KE.multiplayer.joinPrompt()\" style=\"flex:1\">Join Room</button>\n\t\t\t</div>\n\t\t</div>");
}

static void window_mp_players(EdBuild *b) {
    edb_add(&b->html, "<div class=\"windowHeader\"><div>Players [0/?]</div></div><div style=\"padding:10px\"></div>");
}

static int h_mp_unavailable(void *user, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) user; (void) ev; (void) args; (void) argc;
    editor_message(k_mp_unavailable);
    return 1;
}

void ed_window_show(const int index) {
    EdBuild b;
    ed_gui_begin(&b, PANEL_WINDOW);
    for (int i = 0; i < 3; i++) g_counters[i].id = NULL;
    switch (index) {
        case 1: window_controls(&b); break;
        case 3: window_settings(&b); break;
        case 4: window_quick_add(&b); break;
        case 6: window_quick_customize(&b); break;
        case 8: window_welcome(&b); break;
        case 10: window_templates(&b); break;
        case 11: window_publishing(&b); break;
        case 12: window_videos(&b, 0); break;
        case 13: window_videos(&b, 1); break;
        case 15: window_backups(&b); break;
        case 16: window_multiplayer(&b); break;
        case 17: window_mp_players(&b); break;
        case 2: case 5: case 7: case 9: case 14: ed_net_window_gen(index, &b.html); break; // editor_net.c (Assets, Mods, Textures, Sounds, Maps)
        default: break;
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
        editor_show_window(1);
    }
    return 1;
}

static int h_backup(void *user, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) user; (void) ev;
    const char *name = html_ui_current_handler();
    if (!name) return 1;
    if (!strcmp(name, "windows.14.clear")) {
        // LocalDB.clear: io_po("SUCCESS", null, "Backups Cleared") (a #chatList notice here)
        editor_message(ed_backup_clear() ? "Backups Cleared" : "Failed to clear Backups");
        editor_show_window(15);
    } else if (!strcmp(name, "KE.importMapBackup") && argc >= 1 && args[0].kind == HTML_ARG_NUM) {
        ed_backup_load(args[0].num);
    }
    return 1;
}

static int arg_flag(const HtmlArg *a, const int n, const int i) {
    return n > i && (a[i].kind == HTML_ARG_BOOL || a[i].kind == HTML_ARG_NUM) && a[i].num != 0;
}

static int h_page_calls(void *user, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) user;
    const char *name = html_ui_current_handler();
    if (!name) return 1;
    if (!strcmp(name, "loadQuick")) {
        // window.loadQuick(i, script, defaultMap): closeWindow + importMap (editor_io.c)
        if (argc >= 1 && args[0].kind == HTML_ARG_NUM) ed_load_quick((int) args[0].num, arg_flag(args, argc, 1), arg_flag(args, argc, 2));
    } else if (!strcmp(name, "KE.quickStart")) {
        ed_quick_start();
    } else if (!strcmp(name, "KE.importMapPopup")) {
        ed_import_map_popup();
    } else if (!strcmp(name, "importMapData")) {
        import_map_data();
    } else if (!strcmp(name, "importMapKey")) {
        if (html_key_is_enter(ev->key)) import_map_data(); // onkeydown: keyCode 13
    } else if (!strcmp(name, "KE.loadFile")) {
        g_ed.mods |= GLFW_MOD_SHIFT;
        ed_file_import(); // loadFile("importMap"): a file (web) / the clipboard (native)
        g_ed.mods &= ~GLFW_MOD_SHIFT;
    } else if (!strcmp(name, "KE.publishMap")) {
        ed_net_publish();
    } else if (!strcmp(name, "openLink")) {
        // openLink(url, target): //docs.krunker.io, ...
        if (argc >= 1 && args[0].kind == HTML_ARG_STR) {
            char url[512];
            snprintf(url, sizeof(url), "%s%s", !strncmp(args[0].str, "//", 2) ? "https:" : "", args[0].str);
            if (!app_route_url(url)) editor_message(url);
        }
    }
    return 1;
}

void ed_panels_install(HtmlHandlerEntry *table, size_t *count, const size_t max) {
    const HtmlHandlerEntry entries[] = {
        {"GUI.panel.state.lock", h_lock},
        {"windows.0.switchTab", h_switch_tab},
        {"windows.2.switchTab", h_settings_tab},
        {"KE.multiplayer.hostFromUI", h_mp_unavailable},
        {"KE.multiplayer.joinPrompt", h_mp_unavailable},
        {"windows.3.toggleEditMode", h_quick},
        {"windows.3.clickButton", h_quick},
        {"windows.3.deleteButton", h_quick},
        {"windows.3.showCustomize", h_quick},
        {"windows.3.search", h_quick},
        {"windows.14.clear", h_backup},
        {"KE.importMapBackup", h_backup},
        {"loadQuick", h_page_calls},
        {"KE.quickStart", h_page_calls},
        {"KE.importMapPopup", h_page_calls},
        {"importMapData", h_page_calls},
        {"importMapKey", h_page_calls},
        {"KE.loadFile", h_page_calls},
        {"KE.publishMap", h_page_calls},
        {"openLink", h_page_calls},
    };
    for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]) && *count < max; i++) table[(*count)++] = entries[i];
}

// KRUNKNATIVE_EDITOR_SCRIPT test driver (editor.c): set the window tabs directly
void ed_window_set_tabs(const int controls, const int settings, const int quick_edit) {
    if (controls >= 0) g_controls_tab = controls;
    if (settings >= 0) g_settings_tab = settings;
    if (quick_edit >= 0) g_quick_edit = quick_edit;
}
