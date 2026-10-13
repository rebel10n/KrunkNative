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
    if (table == GAME_CONFIG_WEAPON) return id >= 0 && id < WEAPON_COUNT ? rules_weapon(id) : NULL;
    if (table == GAME_CONFIG_CLASS) return id >= 0 && id < CLASS_COUNT ? &rules_classes()[id] : NULL;
    if (table == GAME_CONFIG_EFFECT) return id >= 0 && id < EFFECT_COUNT ? &g_effects[id] : NULL;
    return NULL;
}

static int table_count(const GameConfigTable table) {
    return table == GAME_CONFIG_WEAPON ? WEAPON_COUNT : table == GAME_CONFIG_CLASS ? CLASS_COUNT : EFFECT_COUNT;
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

// One Advanced matrix: the select + switch header, then every entry's rows (only the open one shown).
static void matrix(StrBuf *sb, const GameConfigTable table, const char *title, const char *select_id, const char *change, const char *toggle,
                   const char *toggle_id, const char *conf_prefix, const char letter, const int t, const cJSON *preset, const char *preset_key, const int open) {
    sb_appendf(sb, "<div class='hostTb2 hide'><div class='setHed' style='margin-bottom:15px%s'>%s<select id='%s' class='inputGrey2' onchange='windows[7].%s(this.value)'>",
               table == GAME_CONFIG_WEAPON ? ";" : "", title, select_id, change);
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

// Settings (serverConfig rows this host type offers), v7's "windows.host.settings" body.
static void settings_rows(StrBuf *sb, const int host_type, const cJSON *preset) {
    const unsigned int flags = game_host_type_flags(host_type);
    const int comp = (flags & GAME_HOST_FLAG_COMP) != 0;
    const cJSON *settings = cJSON_GetObjectItemCaseSensitive(preset, "settings");
    size_t count = 0;
    const GameConfigOption *options = game_config_options(GAME_CONFIG_SERVER, &count);
    sb_append(sb, "<div class='hostTb2 hide'><div class='setHed' style='margin-bottom:15px'>Settings</div><div class='setBodH'>");
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
                           "<span class='slider'></span></label></div>", hide, o->name, o->var, on ? "checked" : "");
        } else if (o->kind == GC_INPUT) {
            sb_appendf(sb, "<div class='settNameSmall' style='%s'>%s<input id='customS%s' value='", hide, o->name, o->var);
            sb_append_esc(sb, o->def_str ? o->def_str : "");
            sb_append(sb, "' type='text'style='border-radius:6px;border:none;background:#eee;padding:6px;padding-bottom:6px;float:right;margin-top:5px;margin-bottom:5px;' "
                          "autocomplete='off' autocorrect='off' autocapitalize='off' spellcheck='false'></div>");
        } else if (o->kind == GC_SELECT) {
            sb_appendf(sb, "<div class='settNameSmall' style='%s'>%s<select id='customS%s' class='inputGrey2'>", hide, o->name, o->var);
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
                       o->name, (int) k, value, min, top, step, o->var, value, (int) k);
        }
    }
    if (open) sb_append(sb, "</div>");
    sb_append(sb, "</div>");
}

char *ui_host_config_tab_html(const HostConfigTab tab, const int host_type, const cJSON *preset) {
    const unsigned int flags = game_host_type_flags(host_type);
    const int choices = !(flags & (GAME_HOST_FLAG_COMP | GAME_HOST_FLAG_RESTRICT));
    StrBuf sb = {0};
    if (tab == HOST_CONFIG_TAB_MODES && choices) {
        const cJSON *modes = cJSON_GetObjectItemCaseSensitive(preset, "modes"), *classes = cJSON_GetObjectItemCaseSensitive(preset, "classes");
        sb_append(&sb, "<div class='hostTb1 hide'><div class='setHed' style='margin-bottom:15px;margin-top:30px;'>Game Modes"
                       "<div class='hostToggle' onclick='windows[7].toggle(&quot;gameMode&quot;)'>Toggle</div></div><div style='margin-top:5px'>");
        for (int id = 0; id < MODE_COUNT; id++) {
            const GameModeConfig *mode = mode_config(id);
            if (!mode || !mode->name || !mode_hostable(id)) continue;
            const int on = cJSON_GetArraySize(modes) ? preset_has(modes, id) : id == MODE_ID_FFA;
            sb_appendf(&sb, "<label class='hostOpt' onmouseenter='playTick()'><span class='optName'>%s</span><input id='gameMode%d' type='checkbox'%s><div class='optCheck'></div></label>",
                       mode->name, id, on ? "checked" : "");
        }
        sb_append(&sb, "</div><div class='setHed' style='margin-bottom:15px;'>Classes<div class='hostToggle' onclick='windows[7].toggle(&quot;gameClass&quot;)'>Toggle</div></div><div style='margin-top:5px'>");
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
        sb_append(&sb, "<div class='setHed' style='margin-bottom:15px;'>Privacy</div><div class='setBodH'><div class='settNameSmall'>Private Server <label class='switch'>"
                       "<input id='makePrivate' type='checkbox'><span class='slider'></span></label></div><div class='settNameSmall'>Server Password<input id='passCode' value='' "
                       "type='text' placeholder='Enter Password'style='border-radius:6px;border:none;background:#eee;padding:6px;padding-bottom:6px;float:right;margin-top:5px;"
                       "margin-bottom:5px;' autocomplete='off' autocorrect='off' autocapitalize='off' spellcheck='false'></div></div>");
    }
    return sb_finish(&sb);
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
    (void) ev;
    if (!name) return -1;
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
        value_of("passCode", text, sizeof(text));
        if (strlen(text) > 16) {
            snprintf(err, err_size, "Password too long (max 16)");
            cJSON_Delete(request);
            return NULL;
        }
        if (text[0]) cJSON_AddStringToObject(request, "passCode", text);
    }
    cJSON_AddBoolToObject(request, "private", checked("makePrivate"));
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
    if (choices) {
        if (g_wep_conf) read_matrix(request, "weps", GAME_CONFIG_WEAPON, 'W');
        if (g_cls_conf) read_matrix(request, "cls", GAME_CONFIG_CLASS, 'C');
        if (g_effect_conf) read_matrix(request, "prks", GAME_CONFIG_EFFECT, 'E');
    }
    return request;
}
