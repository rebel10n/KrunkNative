// Map editor left panel: Krunker v7.1.4 ui/editor.js `panel.left.essential` (UI:723-1246): the
// map's name, messages, death height, Force Old AO, minimap asset, and the folders Game Modes,
// Server Preset (config.serverConfig grouped by `sub`), Weapon Config (48 fields per weapon),
// Classes, Class Config (79 fields per class, grouped, with Copy to All / Reset) and Effect
// Config; plus the Editor methods behind their buttons (E:1430-1490: copyPlayerConfig,
// copyPlayerConfigCat, resetPlayerConfig, resetEffectConfig, selectPlayerType, selectCrosshair(C),
// selectWeaponIcon / Asset) and the asset-selection targets of KE.selectAsset (E:1640-1690).
// doc/v7/editor.md WP4 (sections 1.9 / 3.9).
//
// Values live in EditorProject.server_config ({modes, classes, settings, weps, cls, prks},
// index-keyed). As v7, showing the panel fills every missing value with its default / stock
// value; getMapExport (editor_model.c) drops the ones equal to the stock data. A legacy v3.1.1
// top-level "game" block is copied into every class on import (editor_model.c import_game).
//
// Intentional differences from v7.1.4:
// - A Class / Effect / Weapon folder whose name was edited is rebuilt 1 s after the last key (v7's
//   delayTimer) through an html timer; v7 rebuilds the panel the same way.
// - A pending "Select Asset / Icon / Image / Custom" target is dropped when another window opens
//   (v7 keeps KE.activeAction / the viewModels argument until the next selection).
#include "editor_internal.h"
#include "editor_panel_kit.h"
#include <html_dom.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char g_left_active[32] = "";
static int g_refresh_timer;

static void refresh_now(void *user) {
    (void) user;
    g_refresh_timer = 0;
    ed_panel_left_show();
}

// v7's panel.state.refresh("panel_left_essential")
static void refresh_left(void) { ed_panel_left_show(); }

// delayTimer("<Kind>Name" + id, refresh, 1000): the folder titles follow an edited name
static void on_name_change(EdCtl *c, const cJSON *v) {
    (void) c; (void) v;
    if (g_refresh_timer) html_timer_cancel(g_refresh_timer);
    g_refresh_timer = html_timer_add(1000, refresh_now, NULL);
}

static void on_death_height(EdCtl *c, const cJSON *v) { (void) c; (void) v; } // updateDeathBarrier (drawn per frame)

// ---------------------------------------------------------------------------
// asset selection targets (KE.viewTextures / viewModels / viewSounds([kind, index]) -> selectAsset)

static struct {
    int kind, index, window;
} g_target;

void ed_asset_target_clear(void) { memset(&g_target, 0, sizeof(g_target)); }

void ed_asset_target_open(const int kind, const int index) {
    // viewTextures: window 7 (Textures), viewModels: window 2 (Assets), viewSounds: window 9
    int window = 7;
    if (kind == ED_ASSET_WEAPON_ASSET) window = 2;
    else if (kind == ED_ASSET_ACTION_AUDIO) window = 9;
    else if (kind == ED_ASSET_PLAYER_TYPE) {
        // selectPlayerType: a sprite player (mdl 1) takes a texture, else a model
        char key[16];
        snprintf(key, sizeof(key), "%d", index);
        const cJSON *cls = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(g_ed.proj->server_config, "cls"), key);
        window = (int) ed_json_num(cls, "mdl", 0) == 1 ? 7 : 2;
    }
    if (window == 2) ed_net_view_assets(1);
    else editor_show_window(window); // clears any older target
    g_target.kind = kind;
    g_target.index = index;
    g_target.window = window;
}

static cJSON *config_entry(const char *list, const int index) {
    char key[16];
    snprintf(key, sizeof(key), "%d", index);
    return edp_child_obj(edp_child_obj(g_ed.proj->server_config, list), key);
}

int ed_asset_target_apply(const char *id) {
    // KE.selectAsset's cases for the targets above (E:1655-1683)
    const int kind = g_target.kind, index = g_target.index;
    const int live = kind && g_target.window == g_ed.window_index;
    ed_asset_target_clear();
    if (!live || !id) return 0;
    if (kind == ED_ASSET_CROSSHAIR || kind == ED_ASSET_CROSSHAIR_C || kind == ED_ASSET_PLAYER_TYPE) {
        ed_json_set_str(config_entry("cls", index), kind == ED_ASSET_CROSSHAIR ? "cID" : kind == ED_ASSET_CROSSHAIR_C ? "ccID" : "aID", id);
        ed_mark_changed();
        refresh_left();
    } else if (kind == ED_ASSET_WEAPON_ICON || kind == ED_ASSET_WEAPON_ASSET) {
        ed_json_set_str(config_entry("weps", index), kind == ED_ASSET_WEAPON_ICON ? "assetIcon" : "pj_asset", id);
        ed_mark_changed();
        refresh_left();
    } else if (kind == ED_ASSET_GUI_IMAGE) {
        ed_customgui_set_image(index, id);
    } else if (kind == ED_ASSET_ACTION_AUDIO) {
        // trigActions[KE.activeAction].assetSound = parseInt(id) || 0
        EditorObject *o = g_ed.proj->selected;
        if (o) {
            char prop[32];
            snprintf(prop, sizeof(prop), "assetSound@%d", index);
            cJSON *v = cJSON_CreateNumber(atoi(id));
            ed_prop_set(o, prop, v);
            cJSON_Delete(v);
            ed_mark_changed();
            g_ed.dirty_panel = 1;
        }
    } else {
        return 0;
    }
    editor_close_window();
    return 1;
}

static void on_select_asset(EdCtl *c) { ed_asset_target_open(c->arg >> 8, c->arg & 0xff); }

static EdCtl *asset_button(EdBuild *b, const char *label, const int kind, const int index) {
    EdCtl *c = ed_gui_button(b, label, on_select_asset);
    c->arg = kind << 8 | (index & 0xff);
    return c;
}

// ---------------------------------------------------------------------------
// the Editor methods of the Class / Effect Config buttons

static void on_copy_class(EdCtl *c) {
    // copyPlayerConfig: every class becomes a copy of class id (its name included, as v7)
    cJSON *cls = edp_child_obj(g_ed.proj->server_config, "cls");
    char key[16];
    snprintf(key, sizeof(key), "%d", c->arg);
    cJSON *src = cJSON_Duplicate(edp_child_obj(cls, key), 1);
    const int n = cJSON_GetArraySize(editor_data_get("classes"));
    for (int i = 0; i < n; i++) {
        snprintf(key, sizeof(key), "%d", i);
        cJSON_DeleteItemFromObjectCaseSensitive(cls, key);
        cJSON_AddItemToObject(cls, key, cJSON_Duplicate(src, 1));
    }
    cJSON_Delete(src);
    ed_mark_changed();
    refresh_left();
}

static const char *g_copy_cats[256]; // the category of each "Copy <sub> to All Classes" button
static int g_copy_cat_count;

static void on_copy_class_cat(EdCtl *c) {
    // copyPlayerConfigCat: the values of one category of class id on every class
    const int id = c->arg & 0xff, cat = c->arg >> 8;
    if (cat < 0 || cat >= g_copy_cat_count || !g_copy_cats[cat]) return;
    const char *sub = g_copy_cats[cat];
    cJSON *cls = edp_child_obj(g_ed.proj->server_config, "cls");
    char key[16];
    snprintf(key, sizeof(key), "%d", id);
    const cJSON *src = edp_child_obj(cls, key);
    const int n = cJSON_GetArraySize(editor_data_get("classes"));
    const cJSON *f;
    cJSON_ArrayForEach(f, editor_data_get("classConfig")) {
        if (strcmp(ed_json_str(f, "sub", ""), sub)) continue;
        const char *var = ed_json_str(f, "varN", "");
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(src, var);
        for (int i = 0; i < n; i++) {
            if (i == id) continue;
            snprintf(key, sizeof(key), "%d", i);
            cJSON *dst = edp_child_obj(cls, key);
            if (!v) cJSON_DeleteItemFromObjectCaseSensitive(dst, var);
            else if (cJSON_GetObjectItemCaseSensitive(dst, var)) cJSON_ReplaceItemInObjectCaseSensitive(dst, var, cJSON_Duplicate(v, 1));
            else cJSON_AddItemToObject(dst, var, cJSON_Duplicate(v, 1));
        }
    }
    ed_mark_changed();
    refresh_left();
}

static void reset_entry(const char *list, const int id) {
    // resetPlayerConfig / resetEffectConfig: delete serverConfig.<list>[id] (the panel refills it)
    char key[16];
    snprintf(key, sizeof(key), "%d", id);
    cJSON_DeleteItemFromObjectCaseSensitive(edp_child_obj(g_ed.proj->server_config, list), key);
    ed_mark_changed();
    refresh_left();
}

static void on_reset_class(EdCtl *c) { reset_entry("cls", c->arg); }
static void on_reset_effect(EdCtl *c) { reset_entry("prks", c->arg); }

// ---------------------------------------------------------------------------
// the panel

// select options valued "0", "1", ...: GUI.action stores a select's value as its string, which
// getMapExport writes as is for config.cls / prks (the server preset is normalised to the index)
static EdOption *string_index_options(const cJSON *names, int *count) {
    static const char *const digits[] = {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12", "13", "14", "15",
                                         "16", "17", "18", "19", "20", "21", "22", "23", "24", "25", "26", "27", "28", "29", "30", "31"};
    int n;
    EdOption *o = edp_opts_from_names(names, &n);
    for (int i = 0; i < n; i++) o[i].svalue = i < 32 ? digits[i] : NULL;
    if (n > 32) for (int i = 0; i < n; i++) o[i].svalue = NULL; // (no config select is that long)
    *count = n;
    return o;
}

// One config field (serverConfig / classConfig / effectConfig / weaponConfig entry) bound to
// target[varN]: switch (bool), text (input), select (select) or slider. `weapon`: weaponConfig's
// sliders take max (the others maxF || maxP || max). `string_select`: the select stores the
// option's string (the class / effect entries, as v7).
static EdCtl *config_field(EdBuild *b, cJSON *target, const cJSON *w, const int sub, const int weapon, const int string_select) {
    const char *name = ed_json_str(w, "name", "");
    const char *var = ed_json_str(w, "varN", "");
    if (ed_json_truthy(cJSON_GetObjectItemCaseSensitive(w, "bool"))) return edp_json_switch(b, name, target, var, sub, NULL);
    if (ed_json_truthy(cJSON_GetObjectItemCaseSensitive(w, "input"))) return edp_json_text(b, name, target, var, "", sub, NULL);
    if (!weapon && ed_json_truthy(cJSON_GetObjectItemCaseSensitive(w, "select"))) {
        int n;
        const cJSON *names = cJSON_GetObjectItemCaseSensitive(w, "options");
        EdOption *opts = string_select ? string_index_options(names, &n) : edp_opts_from_names(names, &n);
        return edp_json_select(b, name, target, var, opts, n, sub, NULL);
    }
    const double mx = weapon ? ed_json_num(w, "max", 1) : ed_json_num(w, "maxF", ed_json_num(w, "maxP", ed_json_num(w, "max", 1)));
    return edp_json_slider(b, name, target, var, ed_json_num(w, "min", 0), mx, ed_json_num(w, "step", 1), sub, NULL);
}

// "<name> ID" text (placeholder "ID") and its wide Select button
static void asset_field(EdBuild *b, cJSON *target, const cJSON *w, const char *label, const int kind, const int index) {
    char name[96];
    snprintf(name, sizeof(name), "%s ID", ed_json_str(w, "name", ""));
    edp_json_text(b, name, target, ed_json_str(w, "varN", ""), "ID", 1, NULL);
    asset_button(b, label, kind, index);
}

enum { LIST_CLASS, LIST_EFFECT };

// A field of a class / effect: the specials of v7's builder, else config_field
static void entry_field(EdBuild *b, const int list, cJSON *target, const cJSON *w, const int index) {
    const char *var = ed_json_str(w, "varN", "");
    if (!strcmp(var, "name")) edp_json_text(b, ed_json_str(w, "name", ""), target, var, "", 1, on_name_change);
    else if (list == LIST_CLASS && !strcmp(var, "aID")) asset_field(b, target, w, "Select Asset", ED_ASSET_PLAYER_TYPE, index);
    else if (list == LIST_CLASS && !strcmp(var, "cID")) asset_field(b, target, w, "Select Asset", ED_ASSET_CROSSHAIR, index);
    else if (list == LIST_CLASS && !strcmp(var, "ccID")) asset_field(b, target, w, "Select Asset", ED_ASSET_CROSSHAIR_C, index);
    else config_field(b, target, w, 1, 0, 1);
}

// the index of a classConfig category in g_copy_cats (the stored name is editor_data's string)
static int copy_cat_index(const char *sub) {
    for (int i = 0; i < g_copy_cat_count; i++) if (!strcmp(g_copy_cats[i], sub)) return i;
    if (g_copy_cat_count >= (int) (sizeof(g_copy_cats) / sizeof(g_copy_cats[0]))) return -1;
    const cJSON *f;
    cJSON_ArrayForEach(f, editor_data_get("classConfig")) {
        const cJSON *s = cJSON_GetObjectItemCaseSensitive(f, "sub");
        if (!cJSON_IsString(s) || strcmp(s->valuestring, sub)) continue;
        g_copy_cats[g_copy_cat_count] = s->valuestring;
        return g_copy_cat_count++;
    }
    return -1;
}

// The children of one Class / Effect Config entry. v7 builds a tree: a field without `sub` goes
// where it is, the first field of a `sub` opens a "smaller" folder there that collects every later
// field of that sub; the tree is emitted here in the same order.
static void entry_fields(EdBuild *b, const int list, cJSON *target, const cJSON *stock, const cJSON *fields, const int index) {
    const int n = cJSON_GetArraySize(fields);
    char *done = calloc((size_t) (n > 0 ? n : 1), 1);
    if (!done) return;
    // the missing values are filled in list order first (v7 fills them in the same loop that
    // groups them), so the entry's key order is v7's
    const cJSON *f;
    cJSON_ArrayForEach(f, fields) {
        const cJSON *def = cJSON_GetObjectItemCaseSensitive(stock, ed_json_str(f, "varN", ""));
        if (def && !cJSON_IsNull(def)) edp_ensure_default(target, ed_json_str(f, "varN", ""), def);
    }
    for (int i = 0; i < n; i++) {
        const cJSON *w = cJSON_GetArrayItem(fields, i);
        const char *var = ed_json_str(w, "varN", "");
        const cJSON *def = cJSON_GetObjectItemCaseSensitive(stock, var);
        if (done[i] || !def || cJSON_IsNull(def)) continue;
        const char *sub = ed_json_str(w, "sub", NULL);
        if (!sub) {
            edp_ensure_default(target, var, def);
            entry_field(b, list, target, w, index);
            continue;
        }
        char state[96];
        snprintf(state, sizeof(state), "%s.%d.%s", list == LIST_CLASS ? "cls" : "prks", index, sub);
        ed_gui_folder_begin(b, sub, 1, 1, 1, state); // size "smaller" (.subsubheader)
        if (list == LIST_CLASS) {
            char label[96];
            snprintf(label, sizeof(label), "Copy %s to All Classes", sub);
            EdCtl *c = ed_gui_button(b, label, on_copy_class_cat);
            const int cat = copy_cat_index(sub);
            c->arg = (cat < 0 ? 0xffff : cat) << 8 | (index & 0xff);
        }
        for (int j = i; j < n; j++) {
            const cJSON *x = cJSON_GetArrayItem(fields, j);
            const char *xv = ed_json_str(x, "varN", "");
            const cJSON *xd = cJSON_GetObjectItemCaseSensitive(stock, xv);
            if (done[j] || !xd || cJSON_IsNull(xd) || strcmp(ed_json_str(x, "sub", ""), sub)) continue;
            done[j] = 1;
            edp_ensure_default(target, xv, xd);
            entry_field(b, list, target, x, index);
        }
        ed_gui_folder_end(b, 1);
    }
    free(done);
}

static void build_modes(EdBuild *b) {
    cJSON *modes = edp_child_obj(g_ed.proj->server_config, "modes");
    ed_gui_folder_begin(b, "Game Modes", 0, 1, 0, "modes");
    int index = 0;
    const cJSON *m;
    cJSON_ArrayForEach(m, editor_data_get("modes")) {
        // !cost && (!preventCustomGames || account.isDev): the editor has no developer accounts
        if (!cJSON_GetObjectItemCaseSensitive(m, "cost") && !ed_json_truthy(cJSON_GetObjectItemCaseSensitive(m, "pcg"))) {
            char key[16], name[96];
            snprintf(key, sizeof(key), "%d", index);
            snprintf(name, sizeof(name), "%s [ID: %d]", ed_json_str(m, "name", ""), index);
            const cJSON *cur = cJSON_GetObjectItemCaseSensitive(modes, key);
            if (!cur || cJSON_IsNull(cur)) {
                cJSON_DeleteItemFromObjectCaseSensitive(modes, key);
                cJSON_AddBoolToObject(modes, key, index == 0);
            }
            edp_json_switch(b, name, modes, key, 1, NULL);
        }
        index++;
    }
    ed_gui_folder_end(b, 0);
}

static void build_server_preset(EdBuild *b) {
    cJSON *settings = edp_child_obj(g_ed.proj->server_config, "settings");
    const cJSON *list = editor_data_get("serverConfig");
    const int n = cJSON_GetArraySize(list);
    char *done = calloc((size_t) (n > 0 ? n : 1), 1);
    if (!done) return;
    ed_gui_folder_begin(b, "Server Preset", 0, 1, 0, "server");
    for (int i = 0; i < n; i++) { // the defaults in list order (the settings' key order is v7's)
        const cJSON *w = cJSON_GetArrayItem(list, i);
        if (cJSON_GetObjectItemCaseSensitive(w, "cost") || ed_json_truthy(cJSON_GetObjectItemCaseSensitive(w, "hideE"))) continue;
        edp_ensure_default(settings, ed_json_str(w, "varN", ""), cJSON_GetObjectItemCaseSensitive(w, "def"));
    }
    for (int i = 0; i < n; i++) {
        const cJSON *w = cJSON_GetArrayItem(list, i);
        if (done[i] || cJSON_GetObjectItemCaseSensitive(w, "cost") || ed_json_truthy(cJSON_GetObjectItemCaseSensitive(w, "hideE"))) continue;
        const char *sub = ed_json_str(w, "sub", NULL);
        if (!sub) {
            edp_ensure_default(settings, ed_json_str(w, "varN", ""), cJSON_GetObjectItemCaseSensitive(w, "def"));
            config_field(b, settings, w, 1, 0, 0);
            continue;
        }
        // a "small" folder per sub, a rule first and last (v7 unshift / push)
        char state[96];
        snprintf(state, sizeof(state), "server.%s", sub);
        ed_gui_folder_begin(b, sub, 1, 1, 0, state);
        ed_gui_rule(b);
        for (int j = i; j < n; j++) {
            const cJSON *x = cJSON_GetArrayItem(list, j);
            if (done[j] || cJSON_GetObjectItemCaseSensitive(x, "cost") || ed_json_truthy(cJSON_GetObjectItemCaseSensitive(x, "hideE")) ||
                strcmp(ed_json_str(x, "sub", ""), sub)) continue;
            done[j] = 1;
            edp_ensure_default(settings, ed_json_str(x, "varN", ""), cJSON_GetObjectItemCaseSensitive(x, "def"));
            config_field(b, settings, x, 1, 0, 0);
        }
        ed_gui_rule(b);
        ed_gui_folder_end(b, 1);
    }
    ed_gui_folder_end(b, 0);
    free(done);
}

static void build_weapons(EdBuild *b) {
    cJSON *weps = edp_child_obj(g_ed.proj->server_config, "weps");
    ed_gui_folder_begin(b, "Weapon Config", 0, 1, 0, "weapons");
    int index = 0;
    const cJSON *weapon;
    cJSON_ArrayForEach(weapon, editor_data_get("weapons")) {
        char key[16], name[96], state[32];
        snprintf(key, sizeof(key), "%d", index);
        cJSON *wc = edp_child_obj(weps, key);
        snprintf(name, sizeof(name), "%s [ID: %d]", ed_json_str(wc, "name", NULL) && *ed_json_str(wc, "name", "") ? ed_json_str(wc, "name", "")
                                                                                                                 : ed_json_str(weapon, "name", ""), index);
        snprintf(state, sizeof(state), "weapons.w%d", index);
        ed_gui_folder_begin(b, name, 1, 1, 0, state);
        const cJSON *x;
        cJSON_ArrayForEach(x, editor_data_get("weaponConfig")) {
            const char *var = ed_json_str(x, "varN", "");
            const cJSON *def = cJSON_GetObjectItemCaseSensitive(weapon, var);
            if (!def || cJSON_IsNull(def)) continue;
            edp_ensure_default(wc, var, def);
            if (!strcmp(var, "name")) edp_json_text(b, ed_json_str(x, "name", ""), wc, var, "", 1, on_name_change);
            else if (!strcmp(var, "pj_asset")) asset_field(b, wc, x, "Select Asset", ED_ASSET_WEAPON_ASSET, index);
            else if (!strcmp(var, "assetIcon")) asset_field(b, wc, x, "Select Icon", ED_ASSET_WEAPON_ICON, index);
            else config_field(b, wc, x, 1, 1, 0);
        }
        ed_gui_folder_end(b, 1);
        index++;
    }
    ed_gui_folder_end(b, 0);
}

static void build_classes(EdBuild *b) {
    cJSON *classes = edp_child_obj(g_ed.proj->server_config, "classes");
    ed_gui_folder_begin(b, "Classes", 0, 1, 0, "classes");
    int index = 0;
    const cJSON *m;
    cJSON_ArrayForEach(m, editor_data_get("classes")) {
        if (!cJSON_GetObjectItemCaseSensitive(m, "cost")) {
            char key[16], name[96];
            snprintf(key, sizeof(key), "%d", index);
            snprintf(name, sizeof(name), "%s [ID: %d]", ed_json_str(m, "name", ""), index);
            const cJSON *cur = cJSON_GetObjectItemCaseSensitive(classes, key);
            if (!cur || cJSON_IsNull(cur)) {
                cJSON_DeleteItemFromObjectCaseSensitive(classes, key);
                cJSON_AddBoolToObject(classes, key, 1);
            }
            edp_json_switch(b, name, classes, key, 1, NULL);
        }
        index++;
    }
    ed_gui_folder_end(b, 0);
}

// Class Config / Effect Config: a "small" folder per class / effect (its edited name, else the
// stock one) with a rule first and last
static void build_entries(EdBuild *b, const int list) {
    const char *key_list = list == LIST_CLASS ? "cls" : "prks";
    const cJSON *stock_list = editor_data_get(list == LIST_CLASS ? "classes" : "effects");
    const cJSON *fields = editor_data_get(list == LIST_CLASS ? "classConfig" : "effectConfig");
    cJSON *entries = edp_child_obj(g_ed.proj->server_config, key_list);
    ed_gui_folder_begin(b, list == LIST_CLASS ? "Class Config" : "Effect Config", 0, 1, 0, key_list);
    int index = 0;
    const cJSON *stock;
    cJSON_ArrayForEach(stock, stock_list) {
        char key[16], state[32];
        snprintf(key, sizeof(key), "%d", index);
        cJSON *e = edp_child_obj(entries, key);
        const char *name = ed_json_str(e, "name", "");
        snprintf(state, sizeof(state), "%s.%d", key_list, index);
        ed_gui_folder_begin(b, *name ? name : ed_json_str(stock, "name", ""), 1, 1, 0, state);
        ed_gui_rule(b);
        if (list == LIST_CLASS) ed_gui_button(b, "Copy to All Classes", on_copy_class)->arg = index;
        ed_gui_button(b, "Reset", list == LIST_CLASS ? on_reset_class : on_reset_effect)->arg = index;
        entry_fields(b, list, e, stock, fields, index);
        ed_gui_rule(b);
        ed_gui_folder_end(b, 1);
        index++;
    }
    ed_gui_folder_end(b, 0);
}

void ed_panel_left_show(void) {
    if (ed_project_background()) { // working on another project: rebuild once it is shown again
        g_ed.ui_stale = 1;
        return;
    }
    if (ed_panel_locked(0) && strcmp(g_left_active, "essential")) return;
    if (g_refresh_timer) {
        html_timer_cancel(g_refresh_timer);
        g_refresh_timer = 0;
    }
    snprintf(g_left_active, sizeof(g_left_active), "essential");
    g_copy_cat_count = 0;
    cJSON *map = g_ed.proj->map_config;
    EdBuild b;
    ed_gui_begin(&b, PANEL_LEFT);
    ed_gui_panel_buttons(&b, 0, "Essential", 0);
    edp_json_text(&b, "Name", map, "name", "Name", 0, NULL);
    edp_json_text(&b, "Welcome Msg", map, "welMsg", "Welcome", 0, NULL);
    edp_json_text(&b, "Mod URL", map, "modURL", "Mod URL", 0, NULL);
    ed_gui_button(&b, "Select Mod", ed_select_mod);
    edp_json_slider(&b, "Death Height", map, "dthY", -1000, 3000, 1, 0, on_death_height);
    edp_json_switch(&b, "Force Old AO", map, "oldAO", 0, NULL);
    edp_json_text(&b, "Minimap AssetId", map, "mmId", "", 0, NULL);
    ed_gui_rule(&b);
    build_modes(&b);
    build_server_preset(&b);
    build_weapons(&b);
    build_classes(&b);
    build_entries(&b, LIST_CLASS);
    build_entries(&b, LIST_EFFECT);
    ed_gui_end(&b, g_ed.el_left_list);
}

void ed_select_mod(EdCtl *c) {
    (void) c;
    editor_show_window(5); // advancedOptions.selectMod: showWindow(5) (editor_net.c)
}

// KRUNKNATIVE_EDITOR_SCRIPT commands of the left panel (editor.c run_script_text), for the parity
// tests (tools/editor/logic_parity.py): cfg:set:<path>:<json> (a control's GUI.action; path
// "map.<key>" or a dot path in serverConfig: "cls.3.speed", "settings.gravMlt", "modes.2"),
// cfg:copy:<class>, cfg:copycat:<class>:<sub>, cfg:reset:cls|prks:<index>. 1 = handled.
int ed_gameconfig_script(const char *cmd, const char *arg) {
    if (strcmp(cmd, "cfg") || !arg) return 0;
    EdCtl fake;
    memset(&fake, 0, sizeof(fake));
    if (!strncmp(arg, "set:", 4)) {
        char path[128];
        const char *colon = strchr(arg + 4, ':');
        if (!colon || (size_t) (colon - arg - 4) >= sizeof(path)) return 1;
        snprintf(path, sizeof(path), "%.*s", (int) (colon - arg - 4), arg + 4);
        cJSON *v = cJSON_Parse(colon + 1);
        if (!v) return 1;
        cJSON *obj = g_ed.proj->server_config;
        char *key = path;
        if (!strncmp(path, "map.", 4)) {
            obj = g_ed.proj->map_config;
            key = path + 4;
        }
        for (char *dot; (dot = strchr(key, '.')); key = dot + 1) {
            *dot = 0;
            obj = edp_child_obj(obj, key);
        }
        if (cJSON_GetObjectItemCaseSensitive(obj, key)) cJSON_ReplaceItemInObjectCaseSensitive(obj, key, v); // keeps its place, as v7
        else cJSON_AddItemToObject(obj, key, v);
        ed_mark_changed();
        refresh_left();
    } else if (!strncmp(arg, "copy:", 5)) {
        fake.arg = atoi(arg + 5);
        on_copy_class(&fake);
    } else if (!strncmp(arg, "copycat:", 8)) {
        const char *sub = strchr(arg + 8, ':');
        const int cat = sub ? copy_cat_index(sub + 1) : -1;
        if (cat >= 0) {
            fake.arg = cat << 8 | (atoi(arg + 8) & 0xff);
            on_copy_class_cat(&fake);
        }
    } else if (!strncmp(arg, "reset:cls:", 10)) {
        reset_entry("cls", atoi(arg + 10));
    } else if (!strncmp(arg, "reset:prks:", 11)) {
        reset_entry("prks", atoi(arg + 11));
    }
    return 1;
}
