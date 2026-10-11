// Settings store: the port of krunknative.js's settings section. Values live under
// Krunker's own keys (sensitivityX, fov, scaleUI, ...) in the `knSettings` storage
// entry (html_storage_*: native file / web localStorage); defaults are whatever the
// real game rendered into its settings tabs (scraped from ui/windows/settings-tab-*.html
// through a detached element, exactly like collectDefaults()). Every change goes to
// the game through client_apply_setting().
#include <ui_logic.h>
#include <audio.h>
#include <voice_chat.h> // audioInput: microphone list / selection (voice.c)
#include <client_settings.h> // the Client tab's rows in searchList (ui_client_settings.c)
#if defined(__has_include)
#if __has_include(<render_settings.h>)
#include <render_settings.h> // client_apply_render_setting (ui_render_settings.c)
#define UI_HAVE_RENDER_SETTINGS 1
#endif
#endif
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#define UI_SETTINGS_STORAGE_KEY "knSettings"
#define UI_SETTINGS_JSON_MAX (64 * 1024)
#define UI_QUERY_MAX 1024

static cJSON *g_settings;  // saved values (object)
static cJSON *g_defaults;  // scraped defaults (object)
static cJSON *g_ranges;    // slider key -> [min, max] when v3.1.1's `min || max` holds

// v3.1.1 settings with `color: true` (134.js): set()/init() replace any value failing
// g.isHexColor (/^#([0-9A-F]{3}){1,2}$/i) with the default. dmgColor and hudHealthHigh
// are `type: "switch"` there, so the page shows a checkbox whose true/false never passes
// and the colour default (13.js) is what gets saved; their scraped default would be the
// checkbox state, hence the explicit defaults. NULL = the scraped <input type=color> value.
static const struct { const char *key, *def; } COLOR_SETTINGS[] = {
    {"hitColor", NULL}, {"dmgColor", "#DE292F"}, {"critColor", NULL}, {"speedColor", NULL},
    {"speedColorMax", NULL}, {"healthColT", NULL}, {"healthColE", NULL}, {"xpBarCol", NULL},
    {"crosshairColor", NULL}, {"crosshairShadow", NULL}, {"hitmCol", NULL}, {"hitmKCol", NULL},
    {"hudHealthHigh", "#9eeb56"}, {"hudHealthLow", NULL}, {"spdLinesCol", NULL}, {"scoreColor", NULL},
};

// Same selectors as collectDefaults()/applySettingValues() in krunknative.js.
static const char *SLIDER_SELECTOR = "input[type=range][id^=slid_]";
static const char *CONTROL_SELECTOR = "input[type=checkbox][onclick], select[onchange], input[type=text][onchange], input[type=color][onchange]";

// Settings the engine is expected to act on; pushed (saved or default value) once the
// page is up, followed by every other saved key.
static const char *CORE_SETTINGS[] = {
    "sensitivityX", "sensitivityY", "aimSensitivityX", "aimSensitivityY", "invertY", "fov",
    "adsFovMlt", "scaleUI", "uiScaling", "resolution", "shadows", "softShad", "highResShad", "ambientShading",
    "showFPS", "speedScale", "dmgScale", "ambientVolume", "gunsVolume", "playerVolume", "uiVolume",
    // input/camera settings (ui_controls.c)
    "fpsFOV", "scrollDir", "showWeapon", "showWeaponSec", "showWeaponMel", "shwChat", "slidCont",
    "sound", "voiceVolume", "assetVolume",
    // Controller (CONTROLS.cntrlInput, ui_controls.c / gamepad.c)
    "sensitivityXCntrl", "sensitivityYCntrl", "aimSensitivityXCntrl", "aimSensitivityYCntrl",
    "deadZoneL", "deadZoneR", "triggerThres", "invertYCntrl", "vibration", "gradSpeed",
};

// ---------------------------------------------------------------- helpers

// String(value) for the JSON value kinds the store holds.
void ui_setting_to_string(const cJSON *value, char *buf, const size_t n) {
    if (!buf || !n) return;
    buf[0] = 0;
    if (!value) return;
    if (cJSON_IsString(value)) {
        snprintf(buf, n, "%s", cJSON_GetStringValue(value));
    } else if (cJSON_IsBool(value)) {
        snprintf(buf, n, "%s", cJSON_IsTrue(value) ? "true" : "false");
    } else if (cJSON_IsNumber(value)) {
        const double d = cJSON_GetNumberValue(value);
        if (d == floor(d) && fabs(d) < 1e15) snprintf(buf, n, "%.0f", d);
        else snprintf(buf, n, "%.15g", d);
    }
}

// JS `!isNaN(value)` for a non-empty string: the whole string is a number.
static int string_is_number(const char *s, double *out) {
    if (!s || !*s) return 0;
    char *end;
    const double d = strtod(s, &end);
    while (*end && isspace((unsigned char) *end)) end++;
    if (*end) return 0;
    if (out) *out = d;
    return 1;
}

// /setSetting\("([^"]+)"/ over an on* attribute (decoded or still &quot;-escaped).
static int extract_set_setting_key(const char *attr, char *key, const size_t n) {
    const char *p = attr ? strstr(attr, "setSetting(") : NULL;
    if (!p) return 0;
    p += strlen("setSetting(");
    if (*p == '"') p++;
    else if (!strncmp(p, "&quot;", 6)) p += 6;
    else return 0;
    size_t len = 0;
    while (p[len] && p[len] != '"' && p[len] != '&' && len + 1 < n) len++;
    if (!len) return 0;
    memcpy(key, p, len);
    key[len] = 0;
    return 1;
}

static int control_setting_key(const HtmlElem el, char *key, const size_t n) {
    char attr[512];
    if (html_dom_get_attr(el, "onclick", attr, sizeof(attr)) < 0 && html_dom_get_attr(el, "onchange", attr, sizeof(attr)) < 0) return 0;
    return extract_set_setting_key(attr, key, n);
}

static int elem_is_checkbox(const HtmlElem el) {
    char type[32];
    return html_dom_get_attr(el, "type", type, sizeof(type)) >= 0 && !strcmp(type, "checkbox");
}

static int g_save_batch; // > 0: many setSetting calls in a row, saved once at the end

static void ui_settings_save(void) {
    if (g_save_batch) return;
    char *text = cJSON_PrintUnformatted(g_settings);
    if (text) html_storage_set(UI_SETTINGS_STORAGE_KEY, text);
    free(text);
}

// ---------------------------------------------------------------- load / defaults

void ui_settings_load(void) {
    cJSON_Delete(g_settings);
    g_settings = NULL;
    char *buf = malloc(UI_SETTINGS_JSON_MAX);
    if (buf && html_storage_get(UI_SETTINGS_STORAGE_KEY, buf, UI_SETTINGS_JSON_MAX) >= 0) {
        cJSON *parsed = cJSON_Parse(buf);
        if (cJSON_IsObject(parsed)) g_settings = parsed;
        else cJSON_Delete(parsed);
    }
    free(buf);
    if (!g_settings) g_settings = cJSON_CreateObject();
    if (!g_defaults) g_defaults = cJSON_CreateObject();
}

static cJSON *normalize_setting(const char *key, cJSON *v);

// One-time repair of stores written before the value rules above: colour settings saved
// as booleans (dmgColor: true), "true"/"false" strings, out-of-range numbers, and slider
// numbers saved for boolean settings (invertYMbl: 50, the range input's sanitised value
// the old default scrape stored) -- those go back to the default.
static void settings_repair(void) {
    if (!g_settings) return;
    int changed = 0;
    cJSON *item = g_settings->child;
    while (item) {
        cJSON *next = item->next;
        const char *key = item->string;
        const cJSON *def = g_defaults ? cJSON_GetObjectItem(g_defaults, key) : NULL;
        if (key && cJSON_IsBool(def) && cJSON_IsNumber(item) && !(g_ranges && cJSON_GetObjectItem(g_ranges, key))) {
            printf("[ui] settings repair: %s=%g -> default\n", key, cJSON_GetNumberValue(item));
            cJSON_DeleteItemFromObject(g_settings, key);
            changed = 1;
        } else if (key) {
            cJSON *fixed = normalize_setting(key, cJSON_Duplicate(item, 1));
            if (fixed && !cJSON_Compare(fixed, item, 1)) {
                char before[64], after[64];
                ui_setting_to_string(item, before, sizeof(before));
                ui_setting_to_string(fixed, after, sizeof(after));
                printf("[ui] settings repair: %s=%s -> %s\n", key, before, after);
                cJSON_ReplaceItemInObject(g_settings, key, fixed);
                changed = 1;
            } else {
                cJSON_Delete(fixed);
            }
        }
        item = next;
    }
    if (changed) ui_settings_save();
}

void ui_settings_collect_defaults(void) {
    if (!g_defaults) g_defaults = cJSON_CreateObject();
    ui_windows_load();

    HtmlElem *found = malloc(sizeof(HtmlElem) * UI_QUERY_MAX);
    if (!found) return;
    HtmlElem box = html_dom_create("div", NULL);
    if (!box) {
        free(found);
        return;
    }

    for (int tab = 0; tab < ui_settings_tab_count(); tab++) {
        html_dom_set_html(box, ui_settings_tab_html(tab));

        int n = html_dom_query_all(box, SLIDER_SELECTOR, found, UI_QUERY_MAX);
        for (int i = 0; i < n; i++) {
            char id[96], value[64];
            if (html_dom_get_attr(found[i], "id", id, sizeof(id)) < 0 || strncmp(id, "slid_", 5)) continue;
            // The generator wrote the setting's val into the value attribute; the live
            // .value of a range input is sanitised (invertYMbl's `false` reads back as 50).
            if (html_dom_get_attr(found[i], "value", value, sizeof(value)) < 0 &&
                html_dom_get_value(found[i], value, sizeof(value)) < 0) continue;
            cJSON_DeleteItemFromObject(g_defaults, id + 5);
            char lo_text[32], hi_text[32];
            double lo = 0.0, hi = 0.0;
            if (html_dom_get_attr(found[i], "min", lo_text, sizeof(lo_text)) >= 0 && string_is_number(lo_text, &lo) &&
                html_dom_get_attr(found[i], "max", hi_text, sizeof(hi_text)) >= 0 && string_is_number(hi_text, &hi) &&
                (lo != 0.0 || hi != 0.0)) {
                if (!g_ranges) g_ranges = cJSON_CreateObject();
                const double pair[2] = {lo, hi};
                cJSON_DeleteItemFromObject(g_ranges, id + 5);
                cJSON_AddItemToObject(g_ranges, id + 5, cJSON_CreateDoubleArray(pair, 2));
            }
            if (!strcmp(value, "true") || !strcmp(value, "false")) {
                cJSON_AddBoolToObject(g_defaults, id + 5, value[0] == 't'); // a boolean val on a "slider" (invertYMbl)
                continue;
            }
            double d;
            if (!string_is_number(value, &d)) d = strtod(value, NULL); // parseFloat
            cJSON_AddNumberToObject(g_defaults, id + 5, d);
        }

        // colour pickers: <input type=color id=slid_<key> value=<val> oninput=setSetting(...)>
        n = html_dom_query_all(box, "input[type=color][id^=slid_]", found, UI_QUERY_MAX);
        for (int i = 0; i < n; i++) {
            char id[96], value[64];
            if (html_dom_get_attr(found[i], "id", id, sizeof(id)) < 0 || strncmp(id, "slid_", 5)) continue;
            if (html_dom_get_attr(found[i], "value", value, sizeof(value)) < 0) continue;
            cJSON_DeleteItemFromObject(g_defaults, id + 5);
            cJSON_AddStringToObject(g_defaults, id + 5, value);
        }

        n = html_dom_query_all(box, CONTROL_SELECTOR, found, UI_QUERY_MAX);
        for (int i = 0; i < n; i++) {
            char key[64], value[256];
            if (!control_setting_key(found[i], key, sizeof(key))) continue;
            cJSON_DeleteItemFromObject(g_defaults, key);
            if (elem_is_checkbox(found[i])) {
                cJSON_AddBoolToObject(g_defaults, key, html_dom_get_checked(found[i]));
            } else {
                if (html_dom_get_value(found[i], value, sizeof(value)) < 0) value[0] = 0;
                cJSON_AddStringToObject(g_defaults, key, value);
            }
        }
    }

    html_dom_set_html(box, "");
    html_dom_remove(box);
    free(found);

    for (size_t i = 0; i < sizeof(COLOR_SETTINGS) / sizeof(COLOR_SETTINGS[0]); i++) {
        if (!COLOR_SETTINGS[i].def) continue;
        cJSON_DeleteItemFromObject(g_defaults, COLOR_SETTINGS[i].key);
        cJSON_AddStringToObject(g_defaults, COLOR_SETTINGS[i].key, COLOR_SETTINGS[i].def);
    }
    settings_repair();
}

// ---------------------------------------------------------------- lookups

const cJSON *ui_setting_saved(const char *key) {
    return g_settings ? cJSON_GetObjectItem(g_settings, key) : NULL;
}

const cJSON *ui_setting_value(const char *key) {
    const cJSON *v = ui_setting_saved(key);
    if (v) return v;
    return g_defaults ? cJSON_GetObjectItem(g_defaults, key) : NULL;
}

int ui_setting_number(const char *key, double *out) {
    const cJSON *v = ui_setting_value(key);
    if (cJSON_IsNumber(v)) {
        if (out) *out = cJSON_GetNumberValue(v);
        return 1;
    }
    // parseFloat(settingValue(key)) on a numeric string
    if (cJSON_IsString(v)) return string_is_number(cJSON_GetStringValue(v), out);
    return 0;
}

int ui_setting_is_false(const char *key) {
    const cJSON *v = ui_setting_value(key);
    return cJSON_IsFalse(v);
}

// ---------------------------------------------------------------- set / apply

static void sync_controls(const char *key, const char *value) {
    // keep the slider and its number box in sync (Krunker does this in setSetting too)
    char id[96], current[64];
    snprintf(id, sizeof(id), "slid_%s", key);
    HtmlElem slider = html_dom_by_id(id);
    if (slider && (html_dom_get_value(slider, current, sizeof(current)) < 0 || strcmp(current, value))) html_dom_set_value(slider, value);
    snprintf(id, sizeof(id), "slid_input_%s", key);
    HtmlElem box = html_dom_by_id(id);
    if (box && (html_dom_get_value(box, current, sizeof(current)) < 0 || strcmp(current, value))) html_dom_set_value(box, value);
}

// g.isHexColor
static int is_hex_color(const char *s) {
    if (!s || s[0] != '#') return 0;
    const size_t n = strlen(s + 1);
    if (n != 3 && n != 6) return 0;
    for (size_t i = 1; i <= n; i++) {
        if (!isxdigit((unsigned char) s[i])) return 0;
    }
    return 1;
}

static int color_setting(const char *key) {
    for (size_t i = 0; i < sizeof(COLOR_SETTINGS) / sizeof(COLOR_SETTINGS[0]); i++) {
        if (!strcmp(COLOR_SETTINGS[i].key, key)) return 1;
    }
    return 0;
}

static cJSON *default_copy(const char *key) {
    const cJSON *def = g_defaults ? cJSON_GetObjectItem(g_defaults, key) : NULL;
    return def ? cJSON_Duplicate(def, 1) : NULL;
}

// The value rules of v3.1.1's settings.set() (134.js), so every stored value has the
// type and value the real client keeps in settings.list[key].val:
//   "false"/"true" -> booleans, "undefined" -> default; colour settings that are not a
//   hex colour -> default; settings with min/max -> Math.min(max, Math.max(min, +G)).
// Takes ownership of `v`, returns the value to store.
static cJSON *normalize_setting(const char *key, cJSON *v) {
    if (!v) return v;
    if (cJSON_IsString(v)) {
        const char *str = cJSON_GetStringValue(v);
        cJSON *fixed = !strcmp(str, "false") ? cJSON_CreateFalse()
            : !strcmp(str, "true") ? cJSON_CreateTrue()
            : !strcmp(str, "undefined") ? default_copy(key) : NULL;
        if (fixed) {
            cJSON_Delete(v);
            v = fixed;
        }
    }
    if (color_setting(key) && !(cJSON_IsString(v) && is_hex_color(cJSON_GetStringValue(v)))) {
        cJSON *def = default_copy(key);
        if (def) {
            cJSON_Delete(v);
            v = def;
        }
    }
    const cJSON *range = g_ranges ? cJSON_GetObjectItem(g_ranges, key) : NULL;
    if (cJSON_IsArray(range) && cJSON_GetArraySize(range) == 2) {
        double d = NAN;
        if (cJSON_IsNumber(v)) d = cJSON_GetNumberValue(v);
        else if (cJSON_IsBool(v)) d = cJSON_IsTrue(v) ? 1.0 : 0.0;            // +true
        else if (cJSON_IsString(v)) {
            const char *str = cJSON_GetStringValue(v);
            if (!*str) d = 0.0;                                             // +""
            else if (!string_is_number(str, &d)) d = NAN;
        }
        if (isnan(d)) { // Math.min(max, NaN) = NaN: keep the default rather than store NaN
            cJSON *def = default_copy(key);
            if (def) {
                cJSON_Delete(v);
                return def;
            }
            return v;
        }
        const double lo = cJSON_GetNumberValue(cJSON_GetArrayItem(range, 0));
        const double hi = cJSON_GetNumberValue(cJSON_GetArrayItem(range, 1));
        d = fmin(hi, fmax(lo, d));
        cJSON_Delete(v);
        v = cJSON_CreateNumber(d);
    }
    return v;
}

// Store an already-coerced value, forward it and sync the page (setSetting's tail).
static void store_setting(const char *key, cJSON *value) {
    if (!g_settings) ui_settings_load();
    value = normalize_setting(key, value);
    if (!value) return;
    cJSON_DeleteItemFromObject(g_settings, key);
    cJSON_AddItemToObject(g_settings, key, value);
    ui_settings_save();

    char text[256];
    ui_setting_to_string(value, text, sizeof(text));
    client_apply_setting(menu_ui()->client, key, text);
    sync_controls(key, text);
    if (!strcmp(key, "scaleUI") || !strcmp(key, "uiScaling")) menu_scale_base();
}

// coerce(): numeric strings become numbers when the default is a number.
static cJSON *coerce_string(const char *key, const char *value) {
    double d;
    const cJSON *def = g_defaults ? cJSON_GetObjectItem(g_defaults, key) : NULL;
    if (cJSON_IsNumber(def) && string_is_number(value, &d)) return cJSON_CreateNumber(d);
    return cJSON_CreateString(value ? value : "");
}

void ui_settings_set_string(const char *key, const char *value) {
    if (!key || !*key) return;
    store_setting(key, coerce_string(key, value));
}

void ui_settings_set(const char *key, const HtmlArg *value) {
    if (!key || !*key) return;
    cJSON *v;
    if (!value) v = cJSON_CreateString("");
    else switch (value->kind) {
        case HTML_ARG_NUM: v = cJSON_CreateNumber(value->num); break;
        case HTML_ARG_BOOL: v = cJSON_CreateBool(value->num != 0.0); break;
        case HTML_ARG_STR: v = coerce_string(key, value->str ? value->str : ""); break;
        default: v = cJSON_CreateString(""); break; // this.value/this.checked are resolved by the caller
    }
    store_setting(key, v);
}

void ui_settings_reset(void) {
    cJSON_Delete(g_settings);
    g_settings = cJSON_CreateObject();
    ui_settings_save();
    const cJSON *item;
    cJSON_ArrayForEach(item, g_defaults) {
        char text[256];
        ui_setting_to_string(item, text, sizeof(text));
        client_apply_setting(menu_ui()->client, item->string, text);
    }
    menu_scale_base();
    menu_rerender_if_open(1);
}

// settings.export(): every setting whose value differs from its default, as JSON, handed
// to downloadFile("settings.txt", ...). Natively the "download" is ./settings.txt.
// exportSettings(true): the settings text (changed values + controls), malloc'd; NULL on failure.
char *ui_settings_export_text(void) {
    cJSON *out = cJSON_CreateObject();
    const cJSON *item;
    cJSON_ArrayForEach(item, g_settings) {
        const cJSON *def = item->string ? cJSON_GetObjectItem(g_defaults, item->string) : NULL;
        if (item->string && (!def || !cJSON_Compare(item, def, 1))) cJSON_AddItemToObject(out, item->string, cJSON_Duplicate(item, 1));
    }
    cJSON *controls = ui_controls_export_json(); // G.controls = CONTROLS.export() when non-empty
    if (controls) cJSON_AddItemToObject(out, "controls", controls);
    char *text = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    return text;
}

void ui_settings_export(void) {
    char *text = ui_settings_export_text();
    if (!text) return;
#ifdef __EMSCRIPTEN__
    EM_ASM({
        var a = document.createElement('a');
        a.href = URL.createObjectURL(new Blob([UTF8ToString($0)], {type: 'text/plain'}));
        a.download = 'settings.txt';
        a.click();
    }, text);
#else
    // a browser download lands in the downloads folder: $XDG_DOWNLOAD_DIR, else
    // ~/Downloads (%USERPROFILE%\\Downloads), else the working directory
    char path[1024];
    const char *dir = getenv("XDG_DOWNLOAD_DIR");
    const char *home = getenv("HOME");
#ifdef _WIN32
    if (!home || !*home) home = getenv("USERPROFILE");
#endif
    if (dir && *dir) snprintf(path, sizeof(path), "%s/settings.txt", dir);
    else if (home && *home) snprintf(path, sizeof(path), "%s/Downloads/settings.txt", home);
    else snprintf(path, sizeof(path), "settings.txt");
    FILE *f = fopen(path, "wb");
    if (!f) {
        snprintf(path, sizeof(path), "settings.txt");
        f = fopen(path, "wb");
    }
    if (f) {
        fputs(text, f);
        fclose(f);
    }
    menu_log("exportSettings: %s %s: %s", f ? "wrote" : "could not write", path, text);
#endif
    free(text);
}

// ---------------------------------------------------------------- presets / reset / import

// settings.list[key].pro / .perf of v3.1.1 (src/libs/settings.js, read from the running
// game): the Pro and Performance presets set these, every other setting gets its default.
// Values are JSON; NULL = the preset has no value (default). The pro_* images are the
// files the v3.1.1 site serves at its root (ui/pro_*.png); the value is Krunker's URL.
typedef struct {
    const char *key, *pro, *perf;
} SettingPreset;

static const SettingPreset PRESETS[] = {
    {"resolution", "1", "0.4"}, {"aspectRatio", "\"1280x1024\"", NULL}, {"antiAlias", "false", "false"},
    {"lowSpec", NULL, "true"}, {"noTex", NULL, "true"}, {"mapDet", NULL, "false"},
    {"particles", "false", "false"}, {"shadows", "false", "false"}, {"softShad", "false", "false"},
    {"highResShad", "false", "false"}, {"shadowsDynamic", "false", "false"}, {"ambientShading", "false", "false"},
    {"showTrails", "false", "false"}, {"yourTrails", NULL, "false"}, {"muzzleFlash", "false", "false"},
    {"textureAnim", NULL, "false"}, {"objectAnim", NULL, "false"}, {"screenShake", NULL, "false"},
    {"weaponShine", NULL, "false"}, {"lighting", NULL, "0"}, {"scaleUI", NULL, "1"},
    {"dynamicHP", NULL, "false"}, {"showHitInd", "false", "false"}, {"showDMG", "false", "false"},
    {"dmgScale", "0.5", NULL}, {"showDeaths", "true", NULL}, {"showStreak", "true", NULL},
    {"showFPS", "true", "false"}, {"showMedals", NULL, "false"}, {"showUnboxings", "false", NULL},
    {"crosshairSho", "4", NULL}, {"crosshairImage", "\"https://assets.krunker.io/pro_crosshair.png\"", NULL},
    {"crosshairAlways", "true", NULL}, {"aimSensitivityX", "0.69", NULL}, {"aimSensitivityY", "0.69", NULL},
    {"sound", "0.1", "0.1"}, {"voiceVolume", "0", NULL}, {"fov", "110", NULL}, {"fpsFOV", "105", NULL},
    {"weaponBob", "0.2", "0"}, {"weaponLean", "2", "0"}, {"weaponRot", "0", NULL}, {"weaponOffX", "1.4", NULL},
    {"weaponOffY", "2", NULL}, {"weaponOffZ", "1.5", NULL}, {"aimAnim", NULL, "false"},
    {"hideADS", "true", "true"}, {"showSpdLines", NULL, "0"}, {"scoreScale", "0.6", NULL},
    {"scoreOffY", "2.5", NULL}, {"saturationn", NULL, "1"}, {"menuSaturation", "1", NULL},
    {"feedLimit", "4", NULL}, {"bulletTracers", NULL, "false"}, {"scopeBorders", "false", NULL},
    {"customScope", "\"https://assets.krunker.io/pro_scope.png\"", NULL},
    {"customADSDot", "\"https://assets.krunker.io/pro_ads.png\"", NULL}, {"useDamageOverlay", "false", "false"},
};

// KrunkNative addition: the "Beautiful" preset (index 4, offered after Performance in
// #settingsPreset; v3.1.1 has Default/Pro/Performance/Custom only). Like Pro/Performance
// it sets these keys and every other setting to its default, and it persists the same
// way (preIndex = 4; leaving Custom saves krk_customPre, going back to Custom restores it).
// Every render setting was chosen against what KrunkNative implements (render_settings.h):
//   resolution 1        native framebuffer size, no supersampling (2x quadruples the fill
//                       cost for little visible gain; on the web dpr already scales it)
//   antiAlias           4x MSAA on the scene FBO
//   highResShad         4096 sun shadow map instead of 1024 (shadow.c); shadows on
//   softShad, shadowsDynamic  Krunker's best shadow flags; not ported yet (shadow.c), set so
//                       they apply once they land
//   lighting 2          high: Phong + map point lights (basic.c); default is 1
//   particlesDist 1000  slider max: particles and impacts drawn at any distance (vfx.c)
//   renderDist 100      slider max: camera far = 6000
//   weaponShine, sniperFlap  Krunker's quality extras (stored; viewmodel flags)
//   updateRate 0        no frame cap; mapDet/ambientShading/textureAnim/objectAnim/particles/
//                       trails/muzzle flash/tracers/damage overlay on, noTex/lowSpec off
//   postProcessing      on (CopyShader in v3.1.1: no visible effect)
// Kept at Krunker's defaults on purpose: fov/fpsFOV 90 (listed: wide FOVs stretch the edges),
// screenShake, showSpdLines, aspectRatio (window), saturationn/hue/vignette/menu filters
// (colour grading is taste, the defaults are the maps' intended colours), HUD/viewmodel,
// and the KrunkNative-only vsync/windowMode (device choices, not quality). Texture filtering
// is not a setting: v3.1.1 samples map textures nearest-neighbour by design.
static const struct { const char *key, *value; } BEAUTIFUL[] = {
    {"resolution", "1"}, {"updateRate", "0"}, {"antiAlias", "true"}, {"postProcessing", "true"},
    {"lowSpec", "false"}, {"noTex", "false"}, {"mapDet", "true"}, {"particles", "true"},
    {"particlesDist", "1000"}, {"renderDist", "100"}, {"shadows", "true"}, {"softShad", "true"},
    {"highResShad", "true"}, {"shadowsDynamic", "true"}, {"ambientShading", "true"},
    {"showTrails", "true"}, {"yourTrails", "true"}, {"muzzleFlash", "true"}, {"bulletTracers", "true"},
    {"sniperFlap", "true"}, {"textureAnim", "true"}, {"objectAnim", "true"}, {"weaponShine", "true"},
    {"lighting", "2"}, {"useDamageOverlay", "true"}, {"fov", "90"}, {"fpsFOV", "90"},
};
#define PRESET_BEAUTIFUL 4

static const char *beautiful_value(const char *key) {
    for (size_t i = 0; i < sizeof(BEAUTIFUL) / sizeof(BEAUTIFUL[0]); i++) {
        if (!strcmp(BEAUTIFUL[i].key, key)) return BEAUTIFUL[i].value;
    }
    return NULL;
}

static const SettingPreset *preset_for(const char *key) {
    for (size_t i = 0; i < sizeof(PRESETS) / sizeof(PRESETS[0]); i++) {
        if (!strcmp(PRESETS[i].key, key)) return &PRESETS[i];
    }
    return NULL;
}

// JS `a != b` for the value kinds the store holds (numbers vs numeric strings, bools).
static int loose_equal(const cJSON *a, const cJSON *b) {
    if (!a || !b) return a == b;
    char sa[256], sb[256];
    ui_setting_to_string(a, sa, sizeof(sa));
    ui_setting_to_string(b, sb, sizeof(sb));
    double da, db;
    if (string_is_number(sa, &da) && string_is_number(sb, &db)) return da == db;
    return !strcmp(sa, sb);
}

static void batch_set(const char *key, const cJSON *value) {
    if (key && value) store_setting(key, cJSON_Duplicate(value, 1));
}

// settings.list: every setting the page knows (scraped defaults) plus the preset keys.
typedef void (*SettingVisit)(const char *key, const cJSON *def, void *user);
static void each_setting(SettingVisit fn, void *user) {
    const cJSON *item;
    cJSON_ArrayForEach(item, g_defaults) fn(item->string, item, user);
    for (size_t i = 0; i < sizeof(PRESETS) / sizeof(PRESETS[0]); i++) {
        if (!cJSON_GetObjectItem(g_defaults, PRESETS[i].key)) fn(PRESETS[i].key, NULL, user);
    }
    for (size_t i = 0; i < sizeof(BEAUTIFUL) / sizeof(BEAUTIFUL[0]); i++) {
        if (!cJSON_GetObjectItem(g_defaults, BEAUTIFUL[i].key) && !preset_for(BEAUTIFUL[i].key)) fn(BEAUTIFUL[i].key, NULL, user);
    }
}

static int preset_index(void) {
    char buf[16];
    if (html_storage_get("preIndex", buf, sizeof(buf)) < 0 || !buf[0]) return 2; // getSavedVal("preIndex") || 2
    return atoi(buf);
}

typedef struct {
    int target, from;   // hj, cG
    cJSON *custom;      // hm
} PresetRun;

static void preset_visit(const char *key, const cJSON *def, void *user) {
    PresetRun *r = user;
    const cJSON *val = ui_setting_value(key);
    if (r->target == 0) {
        if (r->from == 2 && val && !loose_equal(val, def)) cJSON_AddItemToObject(r->custom, key, cJSON_Duplicate(val, 1));
        if (def) batch_set(key, def);
        else cJSON_DeleteItemFromObject(g_settings, key);
        return;
    }
    if (r->from == 2 && val) cJSON_AddItemToObject(r->custom, key, cJSON_Duplicate(val, 1));
    const SettingPreset *p = preset_for(key);
    const char *lit = r->target == PRESET_BEAUTIFUL ? beautiful_value(key) : p ? (r->target == 1 ? p->pro : p->perf) : NULL;
    cJSON *pv = lit ? cJSON_Parse(lit) : NULL;
    if (pv) batch_set(key, pv);
    else if (def) batch_set(key, def);
    else cJSON_DeleteItemFromObject(g_settings, key);
    cJSON_Delete(pv);
}

// selectSettingPre(hj, hk, hl) after the confirm: hj 0 Default, 1 Pro, 3 Performance,
// 2 Custom (restores what was saved in krk_customPre when leaving Custom).
static void preset_apply(const int target, const int keep_custom, const int silent) {
    const int from = preset_index();
    if (from == target) return;
    g_save_batch++;
    if (target == 0 || target == 1 || target == 3 || target == PRESET_BEAUTIFUL) {
        PresetRun r = {target, from, cJSON_CreateObject()};
        each_setting(preset_visit, &r);
        if (from == 2) {
            char *text = cJSON_PrintUnformatted(r.custom);
            if (text) html_storage_set("krk_customPre", text);
            free(text);
        }
        cJSON_Delete(r.custom);
    } else if (target == 2 && !keep_custom) {
        char *buf = malloc(UI_SETTINGS_JSON_MAX);
        if (buf && html_storage_get("krk_customPre", buf, UI_SETTINGS_JSON_MAX) >= 0) {
            cJSON *saved = cJSON_Parse(buf);
            const cJSON *item;
            cJSON_ArrayForEach(item, saved) {
                if (item->string && (cJSON_GetObjectItem(g_defaults, item->string) || preset_for(item->string))) batch_set(item->string, item);
            }
            cJSON_Delete(saved);
        }
        free(buf);
    }
    g_save_batch--;
    ui_settings_save();
    char idx[16];
    snprintf(idx, sizeof(idx), "%d", target);
    html_storage_set("preIndex", idx);
    if (!silent) menu_show_window(1, 1);
}

// The preset <select> shows the current preset (the static capture says Custom), with
// KrunkNative's Beautiful option added after Performance.
static void sync_preset_select(void) {
    HtmlElem sel = html_dom_by_id("settingsPreset");
    if (!sel) return;
    if (!html_dom_query(sel, "option[value=\"4\"]")) {
        HtmlElem custom = html_dom_query(sel, "option[value=\"2\"]");
        if (custom) html_dom_insert_html(custom, HTML_INSERT_BEFORE_BEGIN, "<option value=\"4\">Beautiful</option>");
        else html_dom_insert_html(sel, HTML_INSERT_BEFORE_END, "<option value=\"4\">Beautiful</option>");
    }
    char idx[16];
    snprintf(idx, sizeof(idx), "%d", preset_index());
    html_dom_set_value(sel, idx);
}

static void preset_confirmed(const int accepted, const char *text, void *user) {
    (void) text;
    if (accepted) preset_apply(0, 0, 0);
    else sync_preset_select(); // the select goes back to the current preset
    (void) user;
}

void ui_settings_select_preset(const int index) {
    if (index == preset_index()) return;
    if (index == 0) {
        ui_confirm("Are you sure you want to reset all your settings?", preset_confirmed, NULL);
        return;
    }
    preset_apply(index, 0, 0);
}

// resetSettings(): confirm, selectSettingPre(0, undefined, true), drop the cont_* binds,
// showWindow(1, true).
static void reset_confirmed(const int accepted, const char *text, void *user) {
    (void) text;
    (void) user;
    if (!accepted) return;
    preset_apply(0, 0, 1);
    ui_controls_reset();
    menu_scale_base();
    menu_show_window(1, 1);
}

void ui_settings_reset_confirm(void) {
    ui_confirm("Are you sure you want to reset all your settings?", reset_confirmed, NULL);
}

// settings.import(text): every known setting takes the text's value or its default;
// "controls" goes to the bind table (ui_controls_import_json).
static void import_visit(const char *key, const cJSON *def, void *user) {
    const cJSON *parsed = user;
    const cJSON *v = cJSON_GetObjectItem(parsed, key);
    if (v && !cJSON_IsNull(v)) batch_set(key, v);
    else if (def) batch_set(key, def);
}

int ui_settings_import_text(const char *text) {
    if (!text || !*text) return 0;
    cJSON *parsed = cJSON_Parse(text);
    if (!cJSON_IsObject(parsed)) {
        cJSON_Delete(parsed);
        char msg[160];
        snprintf(msg, sizeof(msg), "Failed to import settings with error:\nSyntaxError: Unexpected token in JSON");
        ui_alert(msg);
        return 0;
    }
    g_save_batch++;
    each_setting(import_visit, parsed);
    g_save_batch--;
    ui_settings_save();
    ui_controls_import_json(cJSON_GetObjectItem(parsed, "controls"));
    cJSON_Delete(parsed);
    menu_scale_base();
    return 1;
}

static void import_answered(const int accepted, const char *text, void *user) {
    (void) user;
    // the settings window stays up, re-rendered with the imported values (checked on the
    // real v3.1.1 page: after importSettings() the open window is still "Game Settings")
    if (accepted && text && *text && ui_settings_import_text(text)) menu_show_window(1, 1);
}

// importSettings(): prompt("Copy Paste Settings Text Here"), then settings.import.
void ui_settings_import_prompt(void) {
    ui_prompt("Copy Paste Settings Text Here", import_answered, NULL);
}

// settings.list.audioInput.options = getAudioInputs() (updateAudioInputs: deviceId -> label),
// rendered as the select's options like _html.settings.input.select.
static void audio_input_options(const HtmlElem select) {
    char *json = voice_audio_inputs_json();
    cJSON *options = json ? cJSON_Parse(json) : NULL;
    free(json);
    StrBuf sb = {0};
    const cJSON *item;
    cJSON_ArrayForEach(item, options) {
        if (!cJSON_IsString(item)) continue;
        sb_append(&sb, "<option value=\"");
        sb_append_esc(&sb, item->string);
        sb_append(&sb, "\">");
        sb_append_esc(&sb, cJSON_GetStringValue(item));
        sb_append(&sb, "</option>");
    }
    cJSON_Delete(options);
    if (sb.data) html_dom_set_html(select, sb.data);
    sb_free(&sb);
}

void ui_settings_apply_values(const HtmlElem root) {
    if (!root) return;
    sync_preset_select();
    HtmlElem *found = malloc(sizeof(HtmlElem) * UI_QUERY_MAX);
    if (!found) return;

    int n = html_dom_query_all(root, SLIDER_SELECTOR, found, UI_QUERY_MAX);
    for (int i = 0; i < n; i++) {
        char id[96], text[256];
        if (html_dom_get_attr(found[i], "id", id, sizeof(id)) < 0 || strncmp(id, "slid_", 5)) continue;
        const cJSON *v = ui_setting_value(id + 5);
        if (!v) continue;
        ui_setting_to_string(v, text, sizeof(text));
        html_dom_set_value(found[i], text);
        char box_id[128];
        snprintf(box_id, sizeof(box_id), "slid_input_%s", id + 5);
        HtmlElem box = html_dom_by_id(box_id);
        if (box) html_dom_set_value(box, text);
    }

    n = html_dom_query_all(root, CONTROL_SELECTOR, found, UI_QUERY_MAX);
    for (int i = 0; i < n; i++) {
        char key[64], text[256];
        if (!control_setting_key(found[i], key, sizeof(key))) continue;
        if (!strcmp(key, "audioInput")) audio_input_options(found[i]);
        const cJSON *v = ui_setting_saved(key);
        if (!v) continue;
        if (elem_is_checkbox(found[i])) {
            // !!settings[key]
            const int on = cJSON_IsTrue(v) || (cJSON_IsNumber(v) && cJSON_GetNumberValue(v) != 0.0) ||
                (cJSON_IsString(v) && cJSON_GetStringValue(v)[0]);
            html_dom_set_checked(found[i], on);
        } else {
            ui_setting_to_string(v, text, sizeof(text));
            html_dom_set_value(found[i], text);
        }
    }
    free(found);
}

void ui_settings_push_saved(void) {
    Client *client = menu_ui()->client;
    char text[256];
    for (size_t i = 0; i < sizeof(CORE_SETTINGS) / sizeof(CORE_SETTINGS[0]); i++) {
        const cJSON *v = ui_setting_value(CORE_SETTINGS[i]);
        if (!v) continue;
        ui_setting_to_string(v, text, sizeof(text));
        client_apply_setting(client, CORE_SETTINGS[i], text);
    }
    const cJSON *item;
    cJSON_ArrayForEach(item, g_settings) {
        int core = 0;
        for (size_t i = 0; i < sizeof(CORE_SETTINGS) / sizeof(CORE_SETTINGS[0]); i++) {
            if (!strcmp(CORE_SETTINGS[i], item->string)) core = 1;
        }
        if (core) continue;
        ui_setting_to_string(item, text, sizeof(text));
        client_apply_setting(client, item->string, text);
    }
}

// ---------------------------------------------------------------- settings window helpers

static void str_tolower(char *s) {
    for (; *s; s++) *s = (char) tolower((unsigned char) *s);
}

static int inline_display_is_none(const HtmlElem el) {
    char display[32];
    return html_dom_get_style(el, "display", display, sizeof(display)) >= 0 && !strcmp(display, "none");
}

// windows[0].searchList(): Krunker searches every tab, so show matching rows from all.
void ui_settings_search(const char *query) {
    // settingSearch = hj.value (untrimmed); searchMatches lowercases both sides. The field is
    // restored with the raw value, so spaces and case survive typing.
    char raw[128], q[128];
    snprintf(raw, sizeof(raw), "%s", query ? query : "");
    snprintf(q, sizeof(q), "%s", raw);
    char *start = q;
    str_tolower(start);

    HtmlElem holder = html_dom_by_id("settHolder");
    if (!holder) return;
    if (!*start) {
        menu_show_window(1, 1);
        return;
    }

    StrBuf all = {0};
    for (int tab = 0; tab < ui_settings_tab_count(); tab++) sb_append(&all, ui_settings_tab_html(tab));
    char *client_rows = ui_client_settings_tab_html(1); // Settings > Client (ui_client_settings.c)
    sb_append(&all, client_rows);
    free(client_rows);
    html_dom_set_html(holder, all.data ? all.data : "");
    sb_free(&all);
    ui_settings_apply_values(holder);

    HtmlElem *found = malloc(sizeof(HtmlElem) * UI_QUERY_MAX);
    HtmlElem *rows = malloc(sizeof(HtmlElem) * UI_QUERY_MAX);
    if (!found || !rows) {
        free(found);
        free(rows);
        return;
    }
    char text[1024];
    int n = html_dom_query_all(holder, ".settName", found, UI_QUERY_MAX);
    for (int i = 0; i < n; i++) {
        // searchMatches(setting) && !setting.hide: rows generated hidden (`<key>_div`
        // with display:none, e.g. the mobile rows off mobile) never match.
        const int hidden = inline_display_is_none(found[i]);
        html_dom_get_text(found[i], text, sizeof(text));
        text[sizeof(text) - 1] = 0;
        str_tolower(text);
        html_dom_set_display(found[i], !hidden && strstr(text, start) ? "" : "none");
    }
    n = html_dom_query_all(holder, ".setHed", found, UI_QUERY_MAX);
    for (int i = 0; i < n; i++) {
        char id[96];
        if (html_dom_get_attr(found[i], "id", id, sizeof(id)) < 0 || strncmp(id, "setHed_", 7)) continue;
        char body_id[104];
        snprintf(body_id, sizeof(body_id), "setBod_%s", id + 7);
        HtmlElem body = html_dom_by_id(body_id);
        int any = 0;
        if (body) {
            const int m = html_dom_query_all(body, ".settName", rows, UI_QUERY_MAX);
            for (int j = 0; j < m && !any; j++) any = !inline_display_is_none(rows[j]);
        }
        html_dom_set_display(found[i], any ? "" : "none");
    }
    free(found);
    free(rows);

    HtmlElem search = html_dom_by_id("settSearch");
    if (search) {
        html_dom_set_value(search, raw);
        html_dom_focus(search);
    }
}

// windows[0].collapseFolder(hd)
void ui_settings_collapse_folder(const HtmlElem header) {
    char id[96];
    if (!header || html_dom_get_attr(header, "id", id, sizeof(id)) < 0 || strncmp(id, "setHed_", 7)) return;
    char body_id[104];
    snprintf(body_id, sizeof(body_id), "setBod_%s", id + 7);
    HtmlElem body = html_dom_by_id(body_id);
    HtmlElem icon = html_dom_query(header, ".plusOrMinus");
    if (!body) return;
    const int closed = inline_display_is_none(body);
    html_dom_set_display(body, closed ? "" : "none");
    if (icon) html_dom_set_text(icon, closed ? "keyboard_arrow_down" : "keyboard_arrow_right");
}

// ---------------------------------------------------------------- game side

static float parse_float(const char *value, const float fallback) {
    double d;
    return string_is_number(value, &d) ? (float) d : fallback;
}

static int parse_bool(const char *value) {
    if (!value) return 0;
    if (!strcmp(value, "true")) return 1;
    if (!strcmp(value, "false")) return 0;
    double d;
    return string_is_number(value, &d) ? d != 0.0 : value[0] != 0;
}

void client_apply_setting(Client *client, const char *key, const char *value) {
    MenuUi *ui = menu_ui();
    ClientSettings *s = &ui->settings;
    UiInputSettings *in = ui_controls_settings();
    UiControllerInput *cntrl = ui_controls_cntrl_input();
    if (!key || !value) return;
#ifdef UI_HAVE_RENDER_SETTINGS
    // display/render settings (frame cap, resolution, shadows, crosshair flags, ...)
    if (client_apply_render_setting(client, key, value)) return;
#endif
    // SOUND.setVolume: sound, voiceVolume, ambientVolume, gunsVolume, playerVolume,
    // uiVolume, assetVolume (also mirrored below for the settings readers).
    audio_apply_setting(key, value);

    // Read every frame by the input code (ui_controls_fill_input: sensitivityX/Y,
    // aimSensitivityX/Y while the right button is held, invertY) and the camera
    // (fov, adsFovMlt in main.c's zoom, fpsFOV for the first-person scene).

    if (!strcmp(key, "sensitivityX")) s->sensitivity_x = parse_float(value, s->sensitivity_x);
    else if (!strcmp(key, "sensitivityY")) s->sensitivity_y = parse_float(value, s->sensitivity_y);
    else if (!strcmp(key, "aimSensitivityX")) s->aim_sensitivity_x = parse_float(value, s->aim_sensitivity_x);
    else if (!strcmp(key, "aimSensitivityY")) s->aim_sensitivity_y = parse_float(value, s->aim_sensitivity_y);
    else if (!strcmp(key, "invertY")) s->invert_y = parse_bool(value);
    else if (!strcmp(key, "fov")) {
        s->fov = parse_float(value, s->fov);
        // Krunker's fov setting is the (vertical, three.js) camera fov in degrees.
        if (client && s->fov >= 10.0f && s->fov <= 179.0f) client->camera.fov = s->fov * (float) M_PI / 180.0f;
    }
    else if (!strcmp(key, "adsFovMlt")) s->ads_fov_mlt = parse_float(value, s->ads_fov_mlt);
    else if (!strcmp(key, "scaleUI")) s->scale_ui = parse_float(value, s->scale_ui);
    else if (!strcmp(key, "uiScaling")) s->ui_scaling = parse_bool(value);
    else if (!strcmp(key, "resolution")) s->resolution_pct = (int) parse_float(value, (float) s->resolution_pct);
    else if (!strcmp(key, "shadows")) s->shadows = parse_bool(value);
    else if (!strcmp(key, "softShad")) s->soft_shadows = parse_bool(value);
    else if (!strcmp(key, "highResShad")) s->high_res_shadows = parse_bool(value);
    else if (!strcmp(key, "ambientShading")) s->ambient_shading = parse_bool(value);
    else if (!strcmp(key, "showFPS")) {
        s->show_fps = parse_bool(value);
        // Krunker toggles #fpsDisplay with this setting; hud_ui.c's visibility pass reads
        // menu_ui()->settings.show_fps for the in-game writes, this covers a live change.
        HtmlElem fps = html_dom_by_id("fpsDisplay");
        if (fps) html_dom_set_display(fps, s->show_fps ? "" : "none");
    }
    else if (!strcmp(key, "speedScale")) s->speed_scale = parse_float(value, s->speed_scale);
    else if (!strcmp(key, "dmgScale")) s->dmg_scale = parse_float(value, s->dmg_scale);
    else if (!strcmp(key, "ambientVolume")) s->ambient_volume = parse_float(value, s->ambient_volume);
    else if (!strcmp(key, "gunsVolume")) s->guns_volume = parse_float(value, s->guns_volume);
    else if (!strcmp(key, "playerVolume")) s->player_volume = parse_float(value, s->player_volume);
    else if (!strcmp(key, "uiVolume")) s->ui_volume = parse_float(value, s->ui_volume);
    else if (!strcmp(key, "fpsFOV")) in->fps_fov = parse_float(value, in->fps_fov);
    else if (!strcmp(key, "scrollDir")) in->scroll_dir = parse_bool(value);
    else if (!strcmp(key, "slidCont")) in->slid_cont = parse_bool(value); // settings.tmp.slidCont, sent in the next ENT
    else if (!strcmp(key, "showWeapon")) in->show_weapon = parse_bool(value);
    else if (!strcmp(key, "showWeaponSec")) in->show_weapon_sec = parse_bool(value);
    else if (!strcmp(key, "showWeaponMel")) in->show_weapon_mel = parse_bool(value);
    else if (!strcmp(key, "shwChat")) {
        in->shw_chat = (int) parse_float(value, (float) in->shw_chat);
        ui_controls_apply_chat_visibility();
    }
    else if (!strcmp(key, "sound")) in->master_volume = parse_float(value, in->master_volume);
    else if (!strcmp(key, "voiceVolume")) in->voice_volume = parse_float(value, in->voice_volume);
    else if (!strcmp(key, "assetVolume")) in->asset_volume = parse_float(value, in->asset_volume);
    // Controller settings -> CONTROLS.cntrlInput (134.js: the sensitivities are stored x5)
    else if (!strcmp(key, "sensitivityXCntrl")) cntrl->sens_x = parse_float(value, cntrl->sens_x / 5.0f) * 5.0f;
    else if (!strcmp(key, "sensitivityYCntrl")) cntrl->sens_y = parse_float(value, cntrl->sens_y / 5.0f) * 5.0f;
    else if (!strcmp(key, "aimSensitivityXCntrl")) cntrl->aim_x = parse_float(value, cntrl->aim_x / 5.0f) * 5.0f;
    else if (!strcmp(key, "aimSensitivityYCntrl")) cntrl->aim_y = parse_float(value, cntrl->aim_y / 5.0f) * 5.0f;
    else if (!strcmp(key, "deadZoneL")) cntrl->dead_zone_l = parse_float(value, cntrl->dead_zone_l);
    else if (!strcmp(key, "deadZoneR")) cntrl->dead_zone_r = parse_float(value, cntrl->dead_zone_r);
    else if (!strcmp(key, "triggerThres")) cntrl->trigger_thres = parse_float(value, cntrl->trigger_thres);
    else if (!strcmp(key, "invertYCntrl")) cntrl->invert = parse_bool(value);
    else if (!strcmp(key, "vibration")) cntrl->vibration = parse_bool(value);
    else if (!strcmp(key, "gradSpeed")) {
        cntrl->grad_speed = parse_bool(value);
        ui_controls_set_speed_limit(1.0f); // q.speedLmt = 1
    }
    else if (!strcmp(key, "audioInput")) voice_set_audio_input(value); // settings.tmp.audioInput
    else return; // cosmetic Krunker settings the engine has no counterpart for (kept in the store only)

    printf("[ui] setting %s=%s\n", key, value);
}
