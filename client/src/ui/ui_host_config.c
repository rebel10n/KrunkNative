// WP-E Host Game config fields (client/include/ui_host_config.h). Krunker v7.1.4 index.js
// windows[7].gen (the hostTb1 / hostTb2 / hostTb3 bodies, :6609-6816), its toggle / changeWep /
// toggleWep / changeCls / toggleCls / changeEffects / toggleEffects handlers (:6271-6336) and
// createPrivateRoom's read-back (:382-560). The rows come from the shared option tables
// (shared/src/game_config.c), the base values from the game's own weapon / class / effect tables.
#include <ui_host_config.h>
#include <ui_logic.h>
#include <game_config.h>
#include <modes.h>
#include <html_ui.h>
#include <i18n.h>
#include <backend_ui.h> // active_account(): Server Upgrades for an account, its funds
#include <host_flow_ui.h> // the Host Presets' commMap: the window's chosen community map
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ruleset.h>

// windows[7].wepConf / clsConf / effectConf: the Advanced matrices are open
static int g_wep_conf, g_cls_conf, g_effect_conf;

// A number the way JS prints it (1, 0.97, 0.0047). The base values come from float fields
// (weapon / class / effect tables), so they are rounded to float precision first: 50, not
// 50.00000075; 1900, not 1899.999939.
static void js_num(char *out, const size_t size, const double v) {
    if (floor(v) == v && fabs(v) < 1e15) {
        snprintf(out, size, "%.0f", v);
        return;
    }
    char rounded[32];
    snprintf(rounded, sizeof(rounded), "%.7g", v);
    const double r = strtod(rounded, NULL);
    if (floor(r) == r && fabs(r) < 1e15) snprintf(out, size, "%.0f", r);
    else snprintf(out, size, "%.7g", r); // %g drops the trailing zeros
}

static const cJSON *preset_entry(const cJSON *preset, const char *key, const int id) {
    char name[16];
    snprintf(name, sizeof(name), "%d", id);
    return cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(preset, key), name);
}

static int preset_has(const cJSON *list, const int value) {
    const cJSON *item;
    cJSON_ArrayForEach(item, list) {
        if (cJSON_IsNumber(item) && item->valueint == value) return 1;
    }
    return 0;
}

// The base entry of a table: the game's own weapon / class / effect (v7 Game.weapons / classes / effects).
static const void *base_entry(const GameConfigTable table, const int id) {
    if (table == GAME_CONFIG_WEAPON) return id >= 0 && id < rules_krunker_weapon_count() ? rules_weapon(id) : NULL;
    if (table == GAME_CONFIG_CLASS) return id >= 0 && id < CLASS_COUNT ? &rules_classes()[id] : NULL;
    if (table == GAME_CONFIG_EFFECT) return id >= 0 && id < EFFECT_COUNT ? &g_effects[id] : NULL;
    return NULL;
}

static int table_count(const GameConfigTable table) {
    return table == GAME_CONFIG_WEAPON ? rules_krunker_weapon_count() : table == GAME_CONFIG_CLASS ? CLASS_COUNT : EFFECT_COUNT;
}

static const char *entry_name(const GameConfigTable table, const int id) {
    if (table == GAME_CONFIG_WEAPON) return rules_weapon(id)->name;
    if (table == GAME_CONFIG_CLASS) return rules_classes()[id].name;
    return g_effects[id].name;
}

// The rows of one entry (customW<i><varN> / customC.. / customE..), v7's per-key markup.
static void entry_rows(StrBuf *sb, const GameConfigTable table, const char letter, const int t, const int id, const cJSON *override) {
    size_t count = 0;
    const GameConfigOption *options = game_config_options(table, &count);
    const void *base = base_entry(table, id);
    for (size_t k = 0; k < count; k++) {
        const GameConfigOption *o = &options[k];
        if (!game_config_has_key(table, id, o->var)) continue;
        double num = 0.0;
        const char *str = NULL;
        game_config_value(table, base, o->var, &num, &str);
        const cJSON *ov = cJSON_GetObjectItemCaseSensitive(override, o->var);
        if (ov && cJSON_IsNumber(ov)) num = ov->valuedouble;
        else if (ov && cJSON_IsBool(ov)) num = cJSON_IsTrue(ov);
        else if (ov && cJSON_IsString(ov)) str = ov->valuestring;
        char value[48];
        js_num(value, sizeof(value), num);
        if (o->kind == GC_BOOL) {
            sb_appendf(sb, "<div class='settNameSmall'>%s <label class='switch'><input type='checkbox' id='custom%c%d%s' %s><span class='slider'></span></label></div>",
                       o->name, letter, id, o->var, num != 0.0 ? "checked" : "");
        } else if (o->kind == GC_SELECT) {
            sb_appendf(sb, "<div class='settNameSmall'>%s<select id='custom%c%d%s' class='inputGrey2'>", o->name, letter, id, o->var);
            for (int i = 0; i < o->option_count; i++) {
                sb_appendf(sb, "<option value='%d' %s>%s</option>", i, i == (int) num ? "selected" : "", o->options[i]);
            }
            sb_append(sb, "</select></div>");
        } else if (o->kind == GC_INPUT) {
            sb_appendf(sb, "<div class='settNameSmall'>%s<input id='custom%c%d%s' value='", o->name, letter, id, o->var);
            sb_append_esc(sb, str ? str : "");
            sb_append(sb, "' type='text'style='border-radius:6px;border:none;background:#eee;padding:6px;padding-bottom:6px;float:right;margin-top:5px;margin-bottom:5px;' "
                          "autocomplete='off' autocorrect='off' autocapitalize='off' spellcheck='false'></div>");
        } else {
            char min[32], max[32], step[32];
            js_num(min, sizeof(min), o->min);
            js_num(max, sizeof(max), o->max);
            js_num(step, sizeof(step), o->step);
            sb_appendf(sb, "<div class='settNameSmall'>%s<span class='sliderVal' id='custom%cSet%d_%d'>%s</span><div class='slidecontainer'>"
                           "<input type='range' min='%s' max='%s' step='%s' id='custom%c%d%s' value='%s' oninput='updateSliderLabel2(%d,%d,%d,this.value)' class='sliderM'></div></div>",
                       o->name, letter, id, (int) k, value, min, max, step, letter, id, o->var, value, t, id, (int) k);
        }
    }
}

// v10.0.3's Server Upgrades (deob-10.0.3.js:19510-19518): the Server tab's KR-priced switches of an account's
// Custom host, sent as booleans under their varN (createPrivateRoom, :229870-229876). The game reads maxPS as a
// 40-player lobby and keeps promServ (shared/src/game_config.c); Box Office charges no KR for either, v10's
// funds check before hosting is all there is (:229816).
static const struct {
    const char *name, *var;
    int cost;
} SERVER_UPGRADES[] = {
    {"40 Player Server", "maxPS", 1000},
    {"Promote Server", "promServ", 2500},
};
static int g_cost; // windows[7].costVal: the checked switches' KR, 0 whenever the window is generated (:248659)

// One Advanced matrix: the select + switch header, then every entry's rows (only the open one shown).
// The tab bodies start hidden (the .hide class: windows[7]'s tabs show one at a time); the Host
// Game flow's steps show them by their own step (ui_host_config_step_html), so no .hide there.
static int g_show_bodies;
static const char *hide_class(void) { return g_show_bodies ? "" : " hide"; }

static void matrix(StrBuf *sb, const GameConfigTable table, const char *title, const char *select_id, const char *change, const char *toggle,
                   const char *toggle_id, const char *conf_prefix, const char letter, const int t, const cJSON *preset, const char *preset_key, const int open) {
    sb_appendf(sb, "<div class='hostTb2%s'><div class='setHed' style='margin-bottom:15px%s'>%s<select id='%s' class='inputGrey2' onchange='windows[7].%s(this.value)'>",
               hide_class(), table == GAME_CONFIG_WEAPON ? ";" : "", title, select_id, change);
    const int count = table_count(table);
    for (int i = 0; i < count; i++) {
        sb_appendf(sb, "<option value='%d' %s>", i, i == 0 ? "selected" : "");
        sb_append_esc(sb, entry_name(table, i));
        sb_append(sb, "</option>");
    }
    sb_appendf(sb, "</select><label class='switch'><input type='checkbox' id='%s' onchange='windows[7].%s()' %s><span class='slider'></span></label></div>",
               toggle_id, toggle, open ? "checked" : "");
    for (int i = 0; i < count; i++) {
        sb_appendf(sb, "<div id='%s%d' style='display:%s;margin-top:10px%s'><div class='setBodH'>", conf_prefix, i, i == 0 && open ? "block" : "none",
                   table == GAME_CONFIG_WEAPON ? "" : ";");
        entry_rows(sb, table, letter, t, i, preset_entry(preset, preset_key, i));
        sb_append(sb, "</div></div>");
    }
    sb_append(sb, "</div>");
}

// A server setting's label: v10 writes i18n "server.config.<varN>". Its English names differ
// from the ones these rows carry in places (v7's "Warmup" for v10's "Warmup Time"), so English
// keeps the row's own name; other languages take the table's translation of the setting, and
// the row's name when there is none (KrunkNative's own settings).
static const char *config_name(const GameConfigOption *o) {
    if (!strcmp(i18n_locale(), I18N_DEFAULT_LOCALE)) return o->name;
    char key[96];
    snprintf(key, sizeof(key), "server.config.%s", o->var);
    const char *v = i18n_has(key);
    return v ? v : o->name;
}

// Settings (serverConfig rows this host type offers), v7's "windows.host.settings" body.
static void settings_rows(StrBuf *sb, const int host_type, const cJSON *preset) {
    const unsigned int flags = game_host_type_flags(host_type);
    const int comp = (flags & GAME_HOST_FLAG_COMP) != 0;
    const cJSON *settings = cJSON_GetObjectItemCaseSensitive(preset, "settings");
    size_t count = 0;
    const GameConfigOption *options = game_config_options(GAME_CONFIG_SERVER, &count);
    // t("windows.host.settings") (deob-10.0.3.js:248977)
    sb_appendf(sb, "<div class='hostTb2%s'><div class='setHed' style='margin-bottom:15px'>%s</div><div class='setBodH'>", hide_class(),
               I18N("windows.host.settings", "Server Settings"));
    int open = 1;
    for (size_t k = 0; k < count; k++) {
        const GameConfigOption *o = &options[k];
        const unsigned int types = o->host_types ? o->host_types : GC_HT(0);
        if (!(types & GC_HT(host_type >= 0 ? host_type : 0))) continue;
        if (o->head && !(flags & GAME_HOST_FLAG_HIDE_HEAD)) {
            if (open) sb_append(sb, "</div>");
            open = 1;
            sb_appendf(sb, "<div class='setHed' style='margin-bottom:15px'>%s</div><div class='setBodH'>", o->head);
        }
        const char *hide = (o->flags & GC_HIDE) ? "display:none" : "";
        const float def = comp && (o->flags & GC_HAS_DEF_C) ? o->def_c : o->def;
        const cJSON *given = cJSON_GetObjectItemCaseSensitive(settings, o->var);
        if (o->kind == GC_BOOL) {
            const int on = given && !cJSON_IsNull(given) ? (cJSON_IsTrue(given) || (cJSON_IsNumber(given) && given->valuedouble != 0.0)) : def != 0.0f;
            sb_appendf(sb, "<div class='settNameSmall' style='%s'>%s <label class='switch'><input type='checkbox' onchange='windows[7].updateSet(0,this)' id='customS%s' %s>"
                           "<span class='slider'></span></label></div>", hide, config_name(o), o->var, on ? "checked" : "");
        } else if (o->kind == GC_INPUT) {
            sb_appendf(sb, "<div class='settNameSmall' style='%s'>%s<input id='customS%s' value='", hide, config_name(o), o->var);
            sb_append_esc(sb, o->def_str ? o->def_str : "");
            sb_append(sb, "' type='text'style='border-radius:6px;border:none;background:#eee;padding:6px;padding-bottom:6px;float:right;margin-top:5px;margin-bottom:5px;' "
                          "autocomplete='off' autocorrect='off' autocapitalize='off' spellcheck='false'></div>");
        } else if (o->kind == GC_SELECT) {
            sb_appendf(sb, "<div class='settNameSmall' style='%s'>%s<select id='customS%s' class='inputGrey2'>", hide, config_name(o), o->var);
            for (int i = 0; i < o->option_count; i++) sb_appendf(sb, "<option value='%d' %s>%s</option>", i, i == (int) def ? "selected" : "", o->options[i]);
            sb_append(sb, "</select></div>");
        } else {
            // the slider max of a hosted game (UTILS.getAccountType 3 -> maxF), the preset clamped to it
            const float max = o->max_f ? o->max_f : o->max;
            double v = def;
            if (cJSON_IsNumber(given)) v = given->valuedouble > max ? max : given->valuedouble;
            char value[32], min[32], top[32], step[32];
            js_num(value, sizeof(value), v);
            js_num(min, sizeof(min), o->min);
            js_num(top, sizeof(top), max);
            js_num(step, sizeof(step), o->step);
            const int class_icon = !strncmp(o->var, "classLim", 8) ? atoi(o->var + 8) : -1;
            sb_appendf(sb, "<div class='settNameSmall' style='%s'>", hide);
            if (class_icon >= 0) sb_appendf(sb, "<img loading='lazy' class='classLimitIcon' src='textures/classes/icon_%d.png' />", class_icon);
            sb_appendf(sb, "%s<span class='sliderVal' id='customSet%d'>%s</span><div class='slidecontainer' style='vertical-align:top;margin-top:-6px'>"
                           "<input type='range' min='%s' max='%s' step='%s' id='customS%s' value='%s' oninput='updateSliderLabel(%d,this.value)' class='sliderM'></div></div>",
                       config_name(o), (int) k, value, min, top, step, o->var, value, (int) k);
        }
    }
    if (comp) {
        // a comp host's Team Rosters: account names per team and the spectators, comma separated
        // (deob-10.0.3.js:249026-249037; read back as teams / specs, :229848-229868)
        if (open) sb_append(sb, "</div>");
        open = 1;
        sb_append(sb, "<div class='setHed' style='margin-bottom:15px'>Team Rosters</div><div class='setBodH'>");
        static const char *const ROSTERS[3][2] = {{"Team 1 Players", "compRosterT1"}, {"Team 2 Players", "compRosterT2"}, {"Spectators", "compRosterSpecs"}};
        for (int r = 0; r < 3; r++)
            sb_appendf(sb, "<div class='settNameSmall'>%s<input id='%s' value='' type='text' placeholder='&lt;name0&gt;, &lt;name1&gt;...' "
                           "style='border-radius:6px;border:none;background:#eee;padding:6px;padding-bottom:6px;float:right;margin-top:5px;margin-bottom:5px;' "
                           "autocomplete='off' autocorrect='off' autocapitalize='off' spellcheck='false'></div>",
                       ROSTERS[r][0], ROSTERS[r][1]);
    }
    if (open) sb_append(sb, "</div>");
    sb_append(sb, "</div>");
}

// ---------------------------------------------------------------- Host Presets (krk_hostPresets)

static cJSON *read_override(const GameConfigTable table, char letter, int id, const GameConfigOption *o);
static void matrix_toggle(int *open, const HtmlArg *args, int argc, const char *toggle_id, const char *select_id, const char *prefix,
                          GameConfigTable table);

#define HOST_PRESETS_KEY "krk_hostPresets"
#define HOST_PRESETS_MAX (256 * 1024)

static cJSON *presets_load(void) {
    char *buf = malloc(HOST_PRESETS_MAX);
    cJSON *all = NULL;
    if (buf && html_storage_get(HOST_PRESETS_KEY, buf, HOST_PRESETS_MAX) >= 0) all = cJSON_Parse(buf);
    free(buf);
    if (!cJSON_IsObject(all)) {
        cJSON_Delete(all);
        all = cJSON_CreateObject();
    }
    return all;
}

static void presets_save(const cJSON *all) {
    char *text = cJSON_PrintUnformatted(all);
    if (text) html_storage_set(HOST_PRESETS_KEY, text);
    free(text);
}

// windows[7].genPresets (deob-10.0.3.js:249224-249238): the name field, the saved presets' select, Delete / Save / Load
static void presets_html(StrBuf *sb) {
    cJSON *all = presets_load();
    sb_appendf(sb, "<div class='setHed' style='margin-bottom:15px;'>%s</div><div class='setBodH' style='padding-top:20px;padding-bottom:15px;'>",
               I18N("windows.host.preset", "Host Presets"));
    sb_appendf(sb, "<span class='settNameSmall'><input id='presetName' type='text' class='formInput' placeholder='%s' autocomplete='off' autocorrect='off' "
                   "autocapitalize='off' spellcheck='false' style='border:none;background:#eee;padding-bottom:6px;float:left;margin-top:5px;margin-bottom:5px;'>"
                   "<select id='presetSelect' style='border-radius:6px;margin-left:10px;border:none;background:#eee;;padding:6px;padding-bottom:6px;margin-top:5px;margin-bottom:5px;'>"
                   "<option selected>Select</option>",
               I18N("windows.host.preset.name", "Preset name"));
    const cJSON *p;
    cJSON_ArrayForEach(p, all) {
        sb_append(sb, "<option>");
        sb_append_esc(sb, p->string ? p->string : "");
        sb_append(sb, "</option>");
    }
    sb_appendf(sb, "</select><div class='hostPresetBtn' onclick='windows[7].presetAction(2)'>%s</div><div class='hostPresetBtn' onclick='windows[7].presetAction(1)'>%s</div>"
                   "<div class='hostPresetBtn' onclick='windows[7].presetAction(0)'>%s</div></span></div>",
               I18N("generic.delete", "Delete"), I18N("generic.save", "Save"), I18N("generic.load", "Load"));
    cJSON_Delete(all);
}

// One control into the preset (save) or the preset's value into the control (load), under `key`: a checkbox's
// .checked, any other control's .value (v10 keeps the DOM values as they are, :249080-249110)
static void preset_field(const int save, cJSON *preset, const char *key, const HtmlElem el) {
    if (!el || !key) return;
    char type[16] = "";
    const int box = html_dom_get_attr(el, "type", type, sizeof(type)) >= 0 && !strcmp(type, "checkbox");
    if (save) {
        cJSON_DeleteItemFromObjectCaseSensitive(preset, key);
        if (box) {
            cJSON_AddBoolToObject(preset, key, html_dom_get_checked(el));
        } else {
            char value[256];
            html_dom_get_value(el, value, sizeof(value));
            cJSON_AddStringToObject(preset, key, value);
        }
        return;
    }
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(preset, key);
    if (!v) return;
    if (box) {
        html_dom_set_checked(el, cJSON_IsTrue(v) || (cJSON_IsNumber(v) && v->valuedouble != 0.0) || (cJSON_IsString(v) && !strcmp(v->valuestring, "true")));
    } else {
        char text[256];
        if (cJSON_IsString(v)) snprintf(text, sizeof(text), "%s", v->valuestring);
        else js_num(text, sizeof(text), cJSON_GetNumberValue(v));
        html_dom_set_value(el, text);
    }
}

// the checkboxes whose id starts with `prefix` (gameMap / gameMode / gameClass), keyed by their id
static void preset_boxes(const int save, cJSON *preset, const char *prefix) {
    char selector[64];
    snprintf(selector, sizeof(selector), "input[id^='%s']", prefix);
    HtmlElem *found = malloc(sizeof(HtmlElem) * 512);
    const int n = found ? html_dom_query_all(0, selector, found, 512) : 0;
    for (int i = 0; i < n; i++) {
        char id[48];
        if (html_dom_get_attr(found[i], "id", id, sizeof(id)) >= 0) preset_field(save, preset, id, found[i]);
    }
    free(found);
}

// a Weapon / Class / Effect matrix: "<varN>_W_<i>" (_C_, _E_) per row; a loaded value off the base opens the matrix
static int preset_matrix(const int save, cJSON *preset, const GameConfigTable table, const char letter) {
    size_t count = 0;
    const GameConfigOption *options = game_config_options(table, &count);
    int differs = 0;
    for (int id = 0; id < table_count(table); id++) {
        for (size_t k = 0; k < count; k++) {
            const GameConfigOption *o = &options[k];
            if (!game_config_has_key(table, id, o->var)) continue;
            char dom_id[64], key[64];
            snprintf(dom_id, sizeof(dom_id), "custom%c%d%s", letter, id, o->var);
            snprintf(key, sizeof(key), "%s_%c_%d", o->var, letter, id);
            const HtmlElem el = html_dom_by_id(dom_id);
            if (!el) continue;
            preset_field(save, preset, key, el);
            if (save || !cJSON_GetObjectItemCaseSensitive(preset, key)) continue;
            if (o->kind != GC_BOOL && o->kind != GC_INPUT) { // updateSliderLabel2(t, id, k, value)
                char label[64], text[64];
                snprintf(label, sizeof(label), "custom%cSet%d_%d", letter, id, (int) k);
                const HtmlElem l = html_dom_by_id(label);
                if (l && html_dom_get_value(el, text, sizeof(text)) >= 0) html_dom_set_text(l, text);
            }
            cJSON *off = read_override(table, letter, id, o);
            if (off) differs = 1;
            cJSON_Delete(off);
        }
    }
    return differs;
}

// windows[7].presetAction(action): 0 Load, 1 Save, 2 Delete the selected preset (deob-10.0.3.js:249067-249222). v10's
// rawMapData and krRewards keys have no field here (no raw game data, no KR rewards toggle); makePrivate does, and
// commMap is the Host Game window's chosen community map (host_flow_community_name, :249190-249209).
static void preset_action(const int action) {
    const HtmlElem name_el = html_dom_by_id("presetName"), select_el = html_dom_by_id("presetSelect");
    char name[96] = "", selected[96] = "";
    if (name_el) html_dom_get_value(name_el, name, sizeof(name));
    if (select_el) html_dom_get_value(select_el, selected, sizeof(selected));
    const int none = !selected[0] || !strcmp(selected, "Select");
    if ((action == 0 || action == 2) && none) return;
    cJSON *all = presets_load();
    const char *key = name[0] ? name : !none ? selected : "Preset1";
    if (action == 2) {
        cJSON_DeleteItemFromObjectCaseSensitive(all, selected);
        presets_save(all);
    } else {
        cJSON *preset = action == 1 ? cJSON_CreateObject() : cJSON_GetObjectItemCaseSensitive(all, selected);
        if (preset) {
            const int save = action == 1;
            preset_boxes(save, preset, "gameMap");
            preset_boxes(save, preset, "gameMode");
            preset_boxes(save, preset, "gameClass");
            size_t count = 0;
            const GameConfigOption *options = game_config_options(GAME_CONFIG_SERVER, &count);
            for (size_t k = 0; k < count; k++) {
                char id[48];
                snprintf(id, sizeof(id), "customS%s", options[k].var);
                const HtmlElem el = html_dom_by_id(id);
                if (!el) continue;
                preset_field(save, preset, options[k].var, el);
                if (!save && cJSON_GetObjectItemCaseSensitive(preset, options[k].var) && options[k].kind != GC_BOOL && options[k].kind != GC_INPUT) {
                    char label[32], text[64];
                    snprintf(label, sizeof(label), "customSet%d", (int) k);
                    const HtmlElem l = html_dom_by_id(label);
                    if (l && html_dom_get_value(el, text, sizeof(text)) >= 0) html_dom_set_text(l, text);
                }
            }
            if (preset_matrix(save, preset, GAME_CONFIG_WEAPON, 'W') && !g_wep_conf) {
                const HtmlArg on = {.kind = HTML_ARG_BOOL, .num = 1};
                matrix_toggle(&g_wep_conf, &on, 1, "togConfW", "weaponSelect", "weaponConf", GAME_CONFIG_WEAPON);
            }
            if (preset_matrix(save, preset, GAME_CONFIG_CLASS, 'C') && !g_cls_conf) {
                const HtmlArg on = {.kind = HTML_ARG_BOOL, .num = 1};
                matrix_toggle(&g_cls_conf, &on, 1, "togConfC", "classSelect", "classConf", GAME_CONFIG_CLASS);
            }
            if (preset_matrix(save, preset, GAME_CONFIG_EFFECT, 'E') && !g_effect_conf) {
                const HtmlArg on = {.kind = HTML_ARG_BOOL, .num = 1};
                matrix_toggle(&g_effect_conf, &on, 1, "togConfE", "effectSelect", "effectConf", GAME_CONFIG_EFFECT);
            }
            // commMap: Save keeps the chosen community map's name, Load makes the map of that name the chosen one (v10
            // renames its selection, which it hosts by name; with none chosen it throws and keeps the official maps:
            // host_flow_set_community_name skips it then, and the rest still loads)
            const cJSON *comm = save ? NULL : cJSON_GetObjectItemCaseSensitive(preset, "commMap");
            if (save && host_flow_community_name()[0]) cJSON_AddStringToObject(preset, "commMap", host_flow_community_name());
            else if (cJSON_IsString(comm)) host_flow_set_community_name(comm->valuestring);
            const HtmlElem priv = html_dom_by_id("makePrivate");
            if (save) {
                if (priv && html_dom_get_checked(priv)) cJSON_AddTrueToObject(preset, "makePrivate");
                cJSON_DeleteItemFromObjectCaseSensitive(all, key);
                cJSON_AddItemToObject(all, key, preset);
                presets_save(all);
            } else if (priv && cJSON_GetObjectItemCaseSensitive(preset, "makePrivate")) {
                html_dom_set_checked(priv, cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(preset, "makePrivate")));
            }
        }
    }
    cJSON_Delete(all);
    const HtmlElem holder = html_dom_by_id("hostGamePreset"); // genPresets(): the block again
    if (holder) {
        StrBuf sb = {0};
        presets_html(&sb);
        char *html = sb_finish(&sb);
        html_dom_set_html(holder, html ? html : "");
        free(html);
    }
}

static char *tab_html(const HostConfigTab tab, const int host_type, const cJSON *preset) {
    const unsigned int flags = game_host_type_flags(host_type);
    const int choices = !(flags & (GAME_HOST_FLAG_COMP | GAME_HOST_FLAG_RESTRICT));
    StrBuf sb = {0};
    if (tab == HOST_CONFIG_TAB_MODES && choices) {
        const cJSON *modes = cJSON_GetObjectItemCaseSensitive(preset, "modes"), *classes = cJSON_GetObjectItemCaseSensitive(preset, "classes");
        // t("windows.host.modes") / t("windows.host.classes") (deob-10.0.3.js:248848, 248859)
        sb_appendf(&sb, "<div class='hostTb1%s'><div class='setHed' style='margin-bottom:15px;margin-top:30px;'>%s", hide_class(),
                   I18N("windows.host.modes", "Select Modes"));
        sb_append(&sb, "<div class='hostToggle' onclick='windows[7].toggle(&quot;gameMode&quot;)'>Toggle</div></div><div style='margin-top:5px'>");
        for (int id = 0; id < MODE_COUNT; id++) {
            const GameModeConfig *mode = mode_config(id);
            if (!mode || !mode->name || !mode_hostable(id)) continue;
            const int on = cJSON_GetArraySize(modes) ? preset_has(modes, id) : id == MODE_ID_FFA;
            sb_appendf(&sb, "<label class='hostOpt' onmouseenter='playTick()'><span class='optName'>%s</span><input id='gameMode%d' type='checkbox'%s><div class='optCheck'></div></label>",
                       mode->name, id, on ? "checked" : "");
        }
        sb_appendf(&sb, "</div><div class='setHed' style='margin-bottom:15px;'>%s<div class='hostToggle' onclick='windows[7].toggle(&quot;gameClass&quot;)'>Toggle</div></div><div style='margin-top:5px'>",
                   I18N("windows.host.classes", "Select Classes"));
        for (int id = 0; id < CLASS_COUNT; id++) {
            const int on = !cJSON_GetArraySize(classes) || preset_has(classes, id);
            sb_appendf(&sb, "<label class='hostOpt' onmouseenter='playTick()'><span class='optName'>%s</span><img loading='lazy' class='optImg' src='textures/classes/icon_%d.png' />"
                            "<input id='gameClass%d' type='checkbox'%s><div class='optCheck'></div></label>", rules_classes()[id].name, id, id, on ? "checked" : "");
        }
        sb_append(&sb, "</div></div>");
    } else if (tab == HOST_CONFIG_TAB_ADVANCED) {
        if (choices) {
            g_wep_conf = cJSON_GetObjectItemCaseSensitive(preset, "weps") != NULL;
            g_cls_conf = cJSON_GetObjectItemCaseSensitive(preset, "cls") != NULL;
            g_effect_conf = cJSON_GetObjectItemCaseSensitive(preset, "prks") != NULL;
            matrix(&sb, GAME_CONFIG_WEAPON, "Weapon Config", "weaponSelect", "changeWep", "toggleWep", "togConfW", "weaponConf", 'W', 0, preset, "weps", g_wep_conf);
            matrix(&sb, GAME_CONFIG_CLASS, "Class Config", "classSelect", "changeCls", "toggleCls", "togConfC", "classConf", 'C', 1, preset, "cls", g_cls_conf);
            matrix(&sb, GAME_CONFIG_EFFECT, "Effect Config", "effectSelect", "changeEffects", "toggleEffects", "togConfE", "effectConf", 'E', 2, preset, "prks", g_effect_conf);
        }
        settings_rows(&sb, host_type, preset);
    } else if (tab == HOST_CONFIG_TAB_SERVER && choices) {
        g_cost = 0;
        const AccountInfo *acc = active_account();
        if (acc && acc->level >= 10) { // Social: Share on Hub, an account of level 10+ (deob-10.0.3.js:249042-249045)
            sb_append(&sb, "<div class='setHed' style='margin-bottom:15px'>Social</div><div class='setBodH'><div class='settNameSmall'>Share on Hub "
                           "<label class='switch'><input id='shareToHub' type='checkbox'><span class='slider'></span></label></div></div>");
        }
        if (acc) { // Server Upgrades: an account's host (deob-10.0.3.js:249047-249053)
            sb_append(&sb, "<div class='setHed' style='margin-bottom:15px;'>Server Upgrades</div><div class='setBodH'>");
            for (size_t i = 0; i < sizeof(SERVER_UPGRADES) / sizeof(SERVER_UPGRADES[0]); i++)
                sb_appendf(&sb, "<div class='settNameSmall'>%s <label class='switch'><input id='servUp%s' type='checkbox' onchange='windows[7].updateSet(%d,this)'>"
                                "<span class='slider'></span></label><span style='float:right;margin-right:10px;color:rgba(255,255,255,0.5)'>%d <b style='color:#fff'>KR</b></span></div>",
                           SERVER_UPGRADES[i].name, SERVER_UPGRADES[i].var, SERVER_UPGRADES[i].cost, SERVER_UPGRADES[i].cost);
            sb_append(&sb, "</div>");
        }
        // t("server.config.private") (deob-10.0.3.js:249056)
        sb_appendf(&sb, "<div class='setHed' style='margin-bottom:15px;'>Privacy</div><div class='setBodH'><div class='settNameSmall'>%s <label class='switch'>"
                       "<input id='makePrivate' type='checkbox'><span class='slider'></span></label></div><div class='settNameSmall'>Server Password<input id='passCode' value='' "
                       "type='text' placeholder='Enter Password'style='border-radius:6px;border:none;background:#eee;padding:6px;padding-bottom:6px;float:right;margin-top:5px;"
                       "margin-bottom:5px;' autocomplete='off' autocorrect='off' autocapitalize='off' spellcheck='false'></div></div>",
                   I18N("server.config.private", "Private"));
        // Host Presets (deob-10.0.3.js:249058): hidden only for a community map whose preset forces the defaults,
        // which Box Office has no map presets for
        sb_append(&sb, "<div id='hostGamePreset' style='text-align:left;margin-top:20px;margin-bottom:10px;display:block'>");
        presets_html(&sb);
        sb_append(&sb, "</div>");
    }
    return sb_finish(&sb);
}

char *ui_host_config_tab_html(const HostConfigTab tab, const int host_type, const cJSON *preset) {
    g_show_bodies = 0;
    return tab_html(tab, host_type, preset);
}

char *ui_host_config_step_html(const HostConfigTab tab, const int host_type, const cJSON *preset) {
    g_show_bodies = 1;
    char *html = tab_html(tab, host_type, preset);
    g_show_bodies = 0;
    return html;
}

// ---------------------------------------------------------------- handlers

static int arg_num(const HtmlArg *args, const int argc, const int i, const double def) {
    if (i >= argc) return (int) def;
    if (args[i].kind == HTML_ARG_NUM) return (int) args[i].num;
    if ((args[i].kind == HTML_ARG_STR || args[i].kind == HTML_ARG_THIS_VALUE) && args[i].str) return atoi(args[i].str);
    return (int) def;
}

static const char *arg_str(const HtmlArg *args, const int argc, const int i) {
    return i < argc && args[i].str ? args[i].str : "";
}

// Shows "<prefix><shown>" among the <prefix>N blocks, hides the others (shown -1: all hidden).
static void show_only(const char *prefix, const int count, const int shown) {
    for (int i = 0; i < count; i++) {
        char id[48];
        snprintf(id, sizeof(id), "%s%d", prefix, i);
        const HtmlElem el = html_dom_by_id(id);
        if (el) html_dom_set_display(el, i == shown ? "block" : "none");
    }
}

static int select_value(const char *id) {
    char buf[16] = "";
    const HtmlElem el = html_dom_by_id(id);
    if (el) html_dom_get_value(el, buf, sizeof(buf));
    return buf[0] ? atoi(buf) : 0;
}

// changeX(value) shows that entry while the matrix is open; toggleX(force) opens / closes it.
static void matrix_change(const int open, const char *prefix, const GameConfigTable table, const int value) {
    if (open) show_only(prefix, table_count(table), value);
}

static void matrix_toggle(int *open, const HtmlArg *args, const int argc, const char *toggle_id, const char *select_id, const char *prefix, const GameConfigTable table) {
    const int forced = argc > 0 && (args[0].kind == HTML_ARG_BOOL || args[0].kind == HTML_ARG_NUM);
    *open = forced ? args[0].num != 0.0 : !*open;
    if (forced) {
        const HtmlElem toggle = html_dom_by_id(toggle_id);
        if (toggle) html_dom_set_checked(toggle, *open);
    }
    show_only(prefix, table_count(table), *open ? select_value(select_id) : -1);
}

int ui_host_config_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    if (!name) return -1;
    if (!strcmp(name, "windows.7.updateSet")) { // updateSet(cost, el): costVal +/- cost, #hCustCost (deob-10.0.3.js:248477-248482)
        const HtmlElem el = argc > 1 && args[1].kind == HTML_ARG_THIS ? (HtmlElem) args[1].num : ev ? ev->target : 0;
        const int cost = arg_num(args, argc, 0, 0);
        if (el && cost) {
            g_cost += html_dom_get_checked(el) ? cost : -cost;
            const HtmlElem label = html_dom_by_id("hCustCost");
            char text[32];
            if (g_cost) snprintf(text, sizeof(text), "%d KR", g_cost);
            if (label) html_dom_set_text(label, g_cost ? text : "None");
        }
        return 1;
    }
    if (!strcmp(name, "windows.7.presetAction")) {
        preset_action(arg_num(args, argc, 0, -1));
        return 1;
    }
    if (!strcmp(name, "windows.7.toggle")) { // flips every input whose id starts with the prefix
        char selector[64];
        snprintf(selector, sizeof(selector), "input[id^='%s']", arg_str(args, argc, 0));
        HtmlElem found[64];
        const int n = html_dom_query_all(0, selector, found, 64);
        for (int i = 0; i < n; i++) html_dom_set_checked(found[i], !html_dom_get_checked(found[i]));
        return 1;
    }
    if (!strcmp(name, "windows.7.changeWep")) return matrix_change(g_wep_conf, "weaponConf", GAME_CONFIG_WEAPON, arg_num(args, argc, 0, 0)), 1;
    if (!strcmp(name, "windows.7.changeCls")) return matrix_change(g_cls_conf, "classConf", GAME_CONFIG_CLASS, arg_num(args, argc, 0, 0)), 1;
    if (!strcmp(name, "windows.7.changeEffects")) return matrix_change(g_effect_conf, "effectConf", GAME_CONFIG_EFFECT, arg_num(args, argc, 0, 0)), 1;
    if (!strcmp(name, "windows.7.toggleWep")) return matrix_toggle(&g_wep_conf, args, argc, "togConfW", "weaponSelect", "weaponConf", GAME_CONFIG_WEAPON), 1;
    if (!strcmp(name, "windows.7.toggleCls")) return matrix_toggle(&g_cls_conf, args, argc, "togConfC", "classSelect", "classConf", GAME_CONFIG_CLASS), 1;
    if (!strcmp(name, "windows.7.toggleEffects")) return matrix_toggle(&g_effect_conf, args, argc, "togConfE", "effectSelect", "effectConf", GAME_CONFIG_EFFECT), 1;
    if (!strcmp(name, "updateSliderLabel2") || !strcmp(name, "updateSliderLabel")) {
        const int two = !strcmp(name, "updateSliderLabel2");
        char id[48];
        if (two) {
            static const char letters[3] = {'W', 'C', 'E'};
            const int t = arg_num(args, argc, 0, 0);
            snprintf(id, sizeof(id), "custom%cSet%d_%d", letters[t >= 0 && t < 3 ? t : 0], arg_num(args, argc, 1, 0), arg_num(args, argc, 2, 0));
        } else {
            snprintf(id, sizeof(id), "customSet%d", arg_num(args, argc, 0, 0));
        }
        const HtmlElem label = html_dom_by_id(id);
        if (label) html_dom_set_text(label, arg_str(args, argc, two ? 3 : 1));
        return 1;
    }
    return -1;
}

// ---------------------------------------------------------------- createPrivateRoom

static int checked(const char *id) {
    const HtmlElem el = html_dom_by_id(id);
    return el && html_dom_get_checked(el);
}

static int value_of(const char *id, char *out, const size_t size) {
    out[0] = 0;
    const HtmlElem el = html_dom_by_id(id);
    if (!el) return 0;
    html_dom_get_value(el, out, size);
    return 1;
}

// One override value read back from its row, cJSON in v7 types; NULL when it equals the base.
static cJSON *read_override(const GameConfigTable table, const char letter, const int id, const GameConfigOption *o) {
    char dom_id[64], text[128];
    snprintf(dom_id, sizeof(dom_id), "custom%c%d%s", letter, id, o->var);
    double base = 0.0;
    const char *base_str = NULL;
    game_config_value(table, base_entry(table, id), o->var, &base, &base_str);
    if (o->kind == GC_BOOL) {
        const int on = checked(dom_id);
        return on != (base != 0.0) ? cJSON_CreateBool(on) : NULL;
    }
    if (!value_of(dom_id, text, sizeof(text))) return NULL;
    if (o->kind == GC_INPUT) {
        const int ok = text[0] && strlen(text) <= (size_t) (o->len ? o->len : 16) && strspn(text, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_ -|") == strlen(text);
        return ok && (!base_str || strcmp(text, base_str)) ? cJSON_CreateString(text) : NULL;
    }
    double v = atof(text);
    if (o->kind == GC_SELECT) {
        const int index = atoi(text);
        v = index >= 0 && index < o->option_count ? index : o->def;
    }
    return fabs(v - base) > 1e-9 * fmax(1.0, fabs(base)) ? cJSON_CreateNumber(v) : NULL;
}

static void read_matrix(cJSON *request, const char *key, const GameConfigTable table, const char letter) {
    size_t count = 0;
    const GameConfigOption *options = game_config_options(table, &count);
    cJSON *all = NULL;
    for (int id = 0; id < table_count(table); id++) {
        cJSON *entry = NULL;
        for (size_t k = 0; k < count; k++) {
            if (!game_config_has_key(table, id, options[k].var)) continue;
            cJSON *v = read_override(table, letter, id, &options[k]);
            if (!v) continue;
            if (!all) all = cJSON_AddObjectToObject(request, key);
            if (!entry) {
                char name[16];
                snprintf(name, sizeof(name), "%d", id);
                entry = cJSON_AddObjectToObject(all, name);
            }
            cJSON_AddItemToObject(entry, options[k].var, v);
        }
    }
}

cJSON *ui_host_config_collect(const int host_type, char *err, const size_t err_size) {
    const unsigned int flags = game_host_type_flags(host_type);
    const int choices = !(flags & (GAME_HOST_FLAG_COMP | GAME_HOST_FLAG_RESTRICT));
    cJSON *request = cJSON_CreateObject();
    if (!request) return NULL;
    char id[64], text[128];
    if (choices) {
        cJSON *modes = cJSON_AddArrayToObject(request, "modes");
        for (int m = 0; m < MODE_COUNT; m++) {
            snprintf(id, sizeof(id), "gameMode%d", m);
            if (mode_hostable(m) && checked(id)) cJSON_AddItemToArray(modes, cJSON_CreateNumber(m));
        }
        cJSON *classes = cJSON_AddArrayToObject(request, "classes");
        for (int c = 0; c < CLASS_COUNT; c++) {
            snprintf(id, sizeof(id), "gameClass%d", c);
            if (checked(id)) cJSON_AddItemToArray(classes, cJSON_CreateNumber(c));
        }
        if (!cJSON_GetArraySize(modes) || !cJSON_GetArraySize(classes)) {
            snprintf(err, err_size, "%s", !cJSON_GetArraySize(modes) ? "Select at least one Mode" : "Select at least one Class");
            cJSON_Delete(request);
            return NULL;
        }
        // costVal && (!acc || acc.funds < costVal): "Insufficient Funds" (deob-10.0.3.js:229816-229818)
        const AccountInfo *acc = active_account();
        if (g_cost && (!acc || acc->funds < g_cost)) {
            snprintf(err, err_size, "%s", I18N("funds.insufficient", "Insufficient Funds"));
            cJSON_Delete(request);
            return NULL;
        }
        value_of("passCode", text, sizeof(text));
        if (strlen(text) > 16) {
            snprintf(err, err_size, "Password too long (max 16)");
            cJSON_Delete(request);
            return NULL;
        }
        if (text[0]) cJSON_AddStringToObject(request, "passCode", text);
    }
    cJSON_AddBoolToObject(request, "private", checked("makePrivate"));
    cJSON_AddBoolToObject(request, "share", checked("shareToHub")); // share: !!shareToHub && checked (deob-10.0.3.js:229827)
    cJSON_AddNumberToObject(request, "override", host_type);

    size_t count = 0;
    const GameConfigOption *options = game_config_options(GAME_CONFIG_SERVER, &count);
    for (size_t k = 0; k < count; k++) {
        const GameConfigOption *o = &options[k];
        snprintf(id, sizeof(id), "customS%s", o->var);
        if (o->kind == GC_BOOL) {
            cJSON_AddBoolToObject(request, o->var, checked(id));
        } else if (!value_of(id, text, sizeof(text))) {
            if (o->kind != GC_INPUT) cJSON_AddNumberToObject(request, o->var, o->def); // v7: the def when the row is missing
        } else if (o->kind == GC_INPUT) {
            cJSON_AddStringToObject(request, o->var, text);
        } else if (o->kind == GC_SELECT) {
            const int index = atoi(text);
            cJSON_AddNumberToObject(request, o->var, index >= 0 && index < o->option_count ? index : (int) o->def);
        } else {
            cJSON_AddNumberToObject(request, o->var, atof(text));
        }
    }
    if (flags & GAME_HOST_FLAG_COMP) { // teams = [team 1, team 2] when either is named, specs when any (:229848-229868)
        static const char *const IDS[3] = {"compRosterT1", "compRosterT2", "compRosterSpecs"};
        cJSON *lists[3];
        for (int r = 0; r < 3; r++) {
            char names[512];
            lists[r] = cJSON_CreateArray();
            value_of(IDS[r], names, sizeof(names));
            for (char *t = strtok(names, ","); t; t = strtok(NULL, ",")) { // split(","), trim, drop empties
                while (*t == ' ' || *t == '\t') t++;
                size_t len = strlen(t);
                while (len && (t[len - 1] == ' ' || t[len - 1] == '\t')) t[--len] = 0;
                if (len) cJSON_AddItemToArray(lists[r], cJSON_CreateString(t));
            }
        }
        if (cJSON_GetArraySize(lists[0]) || cJSON_GetArraySize(lists[1])) {
            cJSON *teams = cJSON_AddArrayToObject(request, "teams");
            cJSON_AddItemToArray(teams, lists[0]);
            cJSON_AddItemToArray(teams, lists[1]);
        } else {
            cJSON_Delete(lists[0]);
            cJSON_Delete(lists[1]);
        }
        if (cJSON_GetArraySize(lists[2])) cJSON_AddItemToObject(request, "specs", lists[2]);
        else cJSON_Delete(lists[2]);
    }
    if (choices) {
        for (size_t i = 0; i < sizeof(SERVER_UPGRADES) / sizeof(SERVER_UPGRADES[0]); i++) {
            snprintf(id, sizeof(id), "servUp%s", SERVER_UPGRADES[i].var);
            if (html_dom_by_id(id)) cJSON_AddBoolToObject(request, SERVER_UPGRADES[i].var, checked(id));
        }
        if (g_wep_conf) read_matrix(request, "weps", GAME_CONFIG_WEAPON, 'W');
        if (g_cls_conf) read_matrix(request, "cls", GAME_CONFIG_CLASS, 'C');
        if (g_effect_conf) read_matrix(request, "prks", GAME_CONFIG_EFFECT, 'E');
    }
    return request;
}
