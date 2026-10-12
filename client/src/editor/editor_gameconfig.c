// Map editor left panel: Krunker v3.1.1 ui/editor.js `panel.left.essential` (map name, messages,
// death height, game / server / weapon config). Split from editor_panels.c (doc/v7/editor.md WP1);
// doc/v7/editor.md WP4 owns it.
#include "editor_internal.h"
#include "editor_panel_kit.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char g_left_active[32] = "";

// ---------------------------------------------------------------------------
// left panel: Essential (map name, messages, death height, game/server/weapon config)

static void on_death_height(EdCtl *c, const cJSON *v) { (void) c; (void) v; } // updateDeathBarrier (drawn per frame)

// The Game folder edits v3.1.1's map-wide "game" values. v7.1.4 keeps them per class
// (config.cls, the Class Config panel of doc/v7/editor.md WP4): until that panel lands, the
// folder shows class 0's value and a change writes every class, which is what v7's import
// does with a legacy "game" block.
static cJSON *g_game_proxy;

static void on_game_value(EdCtl *c, const cJSON *v) {
    (void) v;
    ed_map_set_all_classes(c->key, cJSON_GetObjectItemCaseSensitive(g_game_proxy, c->key));
}

static cJSON *game_proxy(void) {
    cJSON_Delete(g_game_proxy);
    g_game_proxy = cJSON_CreateObject();
    const cJSON *cls0 = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(g_ed.proj->server_config, "cls"), "0");
    const cJSON *stock0 = cJSON_GetArrayItem(editor_data_get("classes"), 0);
    const cJSON *w;
    cJSON_ArrayForEach(w, editor_data_get("gameConfig")) {
        const char *var = ed_json_str(w, "varN", "");
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(cls0, var);
        if (!v || cJSON_IsNull(v)) v = cJSON_GetObjectItemCaseSensitive(stock0, var);
        if (v && !cJSON_IsNull(v)) cJSON_AddItemToObject(g_game_proxy, var, cJSON_Duplicate(v, 1));
    }
    return g_game_proxy;
}

static EdCtl *add_config_ctl(EdBuild *b, cJSON *target, const cJSON *w, const int sub) {
    const char *name = ed_json_str(w, "name", "");
    const char *var = ed_json_str(w, "varN", "");
    edp_ensure_default(target, var, cJSON_GetObjectItemCaseSensitive(w, "def"));
    if (!strcmp(var, "aID")) {
        static char label[64];
        snprintf(label, sizeof(label), "%s ID", name);
        return edp_json_text(b, label, target, var, "ID", sub, NULL);
    } else if (ed_json_truthy(cJSON_GetObjectItemCaseSensitive(w, "bool"))) {
        return edp_json_switch(b, name, target, var, sub, NULL);
    } else if (ed_json_truthy(cJSON_GetObjectItemCaseSensitive(w, "input"))) {
        return edp_json_text(b, name, target, var, "", sub, NULL);
    } else if (ed_json_truthy(cJSON_GetObjectItemCaseSensitive(w, "select"))) {
        int n;
        EdOption *opts = edp_opts_from_names(cJSON_GetObjectItemCaseSensitive(w, "options"), &n);
        return edp_json_select(b, name, target, var, opts, n, sub, NULL);
    }
    const double mx = ed_json_num(w, "maxF", ed_json_num(w, "maxP", ed_json_num(w, "max", 1)));
    return edp_json_slider(b, name, target, var, ed_json_num(w, "min", 0), mx, ed_json_num(w, "step", 1), sub, NULL);
}

void ed_panel_left_show(void) {
    if (ed_project_background()) { // working on another project: rebuild once it is shown again
        g_ed.ui_stale = 1;
        return;
    }
    if (ed_panel_locked(0) && strcmp(g_left_active, "essential")) return;
    snprintf(g_left_active, sizeof(g_left_active), "essential");
    EdBuild b;
    ed_gui_begin(&b, PANEL_LEFT);
    ed_gui_panel_buttons(&b, 0, "Essential", 0);
    edp_json_text(&b, "Name", g_ed.proj->map_config, "name", "Name", 0, NULL);
    edp_json_text(&b, "Welcome Msg", g_ed.proj->map_config, "welMsg", "Welcome", 0, NULL);
    edp_json_text(&b, "Mod URL", g_ed.proj->map_config, "modURL", "Mod URL", 0, NULL);
    extern void ed_select_mod(EdCtl *);
    ed_gui_button(&b, "Select Mod", ed_select_mod);
    edp_json_slider(&b, "Death Height", g_ed.proj->map_config, "dthY", -1000, 3000, 1, 0, on_death_height);
    ed_gui_rule(&b);

    ed_gui_folder_begin(&b, "Game", 0, 0, 0, "game");
    const cJSON *game = editor_data_get("gameConfig");
    cJSON *proxy = game_proxy();
    for (int cat = 0; cat < 2; cat++) {
        ed_gui_folder_begin(&b, cat ? "Player" : "Camera", 1, 1, 0, cat ? "game.player" : "game.camera");
        const cJSON *w;
        cJSON_ArrayForEach(w, game) {
            if (cJSON_GetObjectItemCaseSensitive(w, "cost")) continue;
            if ((int) ed_json_num(w, "cat", 0) != cat) continue;
            add_config_ctl(&b, proxy, w, 1)->on_change = on_game_value;
        }
        ed_gui_folder_end(&b, 1);
    }
    cJSON *modes = edp_child_obj(g_ed.proj->server_config, "modes");
    ed_gui_folder_begin(&b, "Modes", 1, 1, 0, "game.modes");
    int index = 0;
    const cJSON *m;
    cJSON_ArrayForEach(m, editor_data_get("modes")) {
        char key[8];
        snprintf(key, sizeof(key), "%d", index);
        if (!cJSON_GetObjectItemCaseSensitive(m, "cost") && !ed_json_truthy(cJSON_GetObjectItemCaseSensitive(m, "pcg"))) {
            if (!cJSON_GetObjectItemCaseSensitive(modes, key)) cJSON_AddBoolToObject(modes, key, index == 0);
            edp_json_switch(&b, ed_json_str(m, "name", ""), modes, key, 1, NULL);
        }
        index++;
    }
    ed_gui_folder_end(&b, 1);
    cJSON *classes = edp_child_obj(g_ed.proj->server_config, "classes");
    ed_gui_folder_begin(&b, "Classes", 1, 1, 0, "game.classes");
    index = 0;
    cJSON_ArrayForEach(m, editor_data_get("classes")) {
        char key[8];
        snprintf(key, sizeof(key), "%d", index);
        if (!cJSON_GetObjectItemCaseSensitive(m, "cost")) {
            if (!cJSON_GetObjectItemCaseSensitive(classes, key)) cJSON_AddBoolToObject(classes, key, 1);
            edp_json_switch(&b, ed_json_str(m, "name", ""), classes, key, 1, NULL);
        }
        index++;
    }
    ed_gui_folder_end(&b, 1);
    cJSON *settings = edp_child_obj(g_ed.proj->server_config, "settings");
    ed_gui_folder_begin(&b, "Settings", 1, 1, 0, "game.settings");
    cJSON_ArrayForEach(m, editor_data_get("serverConfig")) {
        if (cJSON_GetObjectItemCaseSensitive(m, "cost") || ed_json_truthy(cJSON_GetObjectItemCaseSensitive(m, "hideE"))) continue;
        add_config_ctl(&b, settings, m, 1);
    }
    ed_gui_folder_end(&b, 1);
    ed_gui_folder_end(&b, 0);

    ed_gui_folder_begin(&b, "Weapon Config", 0, 1, 0, "weapons");
    cJSON *weps = edp_child_obj(g_ed.proj->server_config, "weps");
    index = 0;
    const cJSON *weapon;
    cJSON_ArrayForEach(weapon, editor_data_get("weapons")) {
        char key[8], state[32];
        snprintf(key, sizeof(key), "%d", index);
        snprintf(state, sizeof(state), "weapons.w%d", index);
        cJSON *wc = edp_child_obj(weps, key);
        ed_gui_folder_begin(&b, ed_json_str(weapon, "name", ""), 1, 1, 0, state);
        const cJSON *x;
        cJSON_ArrayForEach(x, editor_data_get("weaponConfig")) {
            const char *var = ed_json_str(x, "varN", "");
            const cJSON *def = cJSON_GetObjectItemCaseSensitive(weapon, var);
            if (!def || cJSON_IsNull(def)) continue;
            edp_ensure_default(wc, var, def);
            if (ed_json_truthy(cJSON_GetObjectItemCaseSensitive(x, "bool"))) edp_json_switch(&b, ed_json_str(x, "name", ""), wc, var, 1, NULL);
            else edp_json_slider(&b, ed_json_str(x, "name", ""), wc, var, ed_json_num(x, "min", 0), ed_json_num(x, "max", 1), ed_json_num(x, "step", 1), 1, NULL);
        }
        ed_gui_folder_end(&b, 1);
        index++;
    }
    ed_gui_folder_end(&b, 0);
    ed_gui_end(&b, g_ed.el_left_list);
}

void ed_select_mod(EdCtl *c) {
    (void) c;
    editor_show_window(5); // advancedOptions.selectMod: showWindow(5) (editor_net.c)
}
