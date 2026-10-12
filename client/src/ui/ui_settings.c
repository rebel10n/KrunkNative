// Settings store and the Game Settings window (Krunker v7.1.4 windows[0], src/index.js +
// src/libs/settings.js). Values live under Krunker's own keys (sensitivityX, fov, scaleUI,
// ...) in the `knSettings` storage entry (html_storage_*: native file / web localStorage);
// defaults are whatever the real game rendered into its settings tabs (scraped from the
// advanced view's captures, ui/windows/settings-advanced-*.html, through a detached element).
// Every change goes to the game through client_apply_setting(). Rows v7 shows that nothing
// in KrunkNative reads yet are stored and shown (their consumers come with the v7 settings
// work).
#include <ui_logic.h>
#include <ui_mobile.h> // netRate: the mobile default
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

// v7.1.4 settings with `color: true` (settings.js): set()/init() replace any value failing
// UTILS.isHexColor (/^#([0-9A-F]{3}){1,2}$/i) with the default. NULL = the scraped
// <input type=color> value; the three a colour switch / custom html renders without a
// plain colour input carry their settings.js default.
static const struct { const char *key, *def; } COLOR_SETTINGS[] = {
    {"kpdVisionCol", "#00ffff"}, {"hitColor", NULL}, {"dmgColor", NULL}, {"critColor", NULL}, {"speedColor", NULL},
    {"speedColorMax", NULL}, {"healthColT", NULL}, {"healthColE", NULL}, {"crosshairColor", NULL},
    {"crosshairShadow", NULL}, {"crosshairColorM", NULL}, {"crosshairShadowM", NULL}, {"hitmCol", NULL},
    {"hitmKCol", NULL}, {"hudHealthHigh", NULL}, {"hudHealthLow", NULL}, {"spdLinesCol", NULL},
    {"scoreColor", NULL}, {"progColor", NULL}, {"progMColor", NULL}, {"progMOColor", NULL},
    {"bulletTracerCol", "#ffffdb"}, {"trailCol", NULL}, {"ropeCol", NULL}, {"scopeBorderCol", "#000000"},
};

// The value-carrying controls of v7's setting rows (_html.settings.input.*): sliders
// (range #slid_<key> + number box #slid_input_<key>), switches, selects, colour / text /
// url inputs (oninput="window.setSetting(...)").
static const char *SLIDER_SELECTOR = "input[type=range][id^=slid_]";
static const char *CONTROL_SELECTOR = "input[type=checkbox][onclick], select[onchange], input[type=text][oninput], "
                                      "input[type=url][oninput], input[type=input][oninput], input[type=undefined][oninput], "
                                      "input[type=color][oninput], input[type=text][onchange], input[type=color][onchange]";

// Settings the engine is expected to act on; pushed (saved or default value) once the
// page is up, followed by every other saved key.
static const char *CORE_SETTINGS[] = {
    "sensitivityX", "sensitivityY", "aimSensitivityX", "aimSensitivityY", "invertY", "fov",
    "adsFovMlt", "scaleUI", "uiScaling", "resolution", "shadows", "softShad", "highResShad", "ambientShading",
    "showFPS", "speedScale", "dmgScale", "ambientVolume", "gunsVolume", "playerVolume", "uiVolume",
    // input/camera settings (ui_controls.c)
    "fpsFOV", "scrollDir", "showWeapon", "showWeaponSec", "showWeaponMel", "shwChat", "slidCont",
    "sound", "voiceVolume", "assetVolume",
    // v7 voice chat (voice.c): micQuality / micVolume ride in "vc", proximity voice, muteSpec, toggle
    "micQuality", "micVolume", "voiceChatType", "voiceDistance", "muteSpec", "voiceChatTrigger",
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
    static const char *const attrs[] = {"onclick", "onchange", "oninput"};
    char attr[512];
    for (size_t i = 0; i < sizeof(attrs) / sizeof(attrs[0]); i++) {
        if (html_dom_get_attr(el, attrs[i], attr, sizeof(attr)) >= 0 && extract_set_setting_key(attr, key, n)) return 1;
    }
    return 0;
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

    // settings.js netRate (Advanced > General "Network Rate (Hz)", read by net/recon.c): val
    // isMobile ? 1 (60 Hz) : 3 (144 Hz); the captured row has the desktop option selected
    if (ui_mobile_active()) {
        cJSON_DeleteItemFromObject(g_defaults, "netRate");
        cJSON_AddStringToObject(g_defaults, "netRate", "1");
    }

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

// settings.list[key].pro / .perf of v7.1.4 (src/libs/settings.js): the Pro and Performance
// presets set these, every other setting gets its default. Values are JSON; NULL = the
// preset has no value (default). The pro_* image is the file the site serves at its root
// (ui/pro_crosshair.png); the value is Krunker's URL (UTILS.assetsUrl("pro_crosshair.png")).
typedef struct {
    const char *key, *pro, *perf;
} SettingPreset;

static const SettingPreset PRESETS[] = {
    {"resolution", "1", "0.4"}, {"aspectRatio", "\"1280x1024\"", NULL}, {"antiAlias", "false", "false"},
    {"lowSpec", NULL, "true"}, {"noTex", NULL, "true"}, {"mapDet", NULL, "false"}, {"particles", "false", "false"},
    {"shadows", "false", "false"}, {"softShad", "false", "false"}, {"highResShad", "false", "false"},
    {"shadowsDynamic", "false", "false"}, {"ambientShading", "false", "false"}, {"showTrails", "false", "false"},
    {"yourTrails", NULL, "false"}, {"muzzleFlash", "false", "false"}, {"bulletCasings", "false", "false"},
    {"impactHoles", "false", "false"}, {"textureAnim", NULL, "false"}, {"objectAnim", NULL, "false"},
    {"screenShake", NULL, "false"}, {"weaponShine", NULL, "false"}, {"lighting", NULL, "0"}, {"scaleUI", NULL, "1"},
    {"dynamicHP", NULL, "false"}, {"showHitInd", "false", "false"}, {"deathMarkers", NULL, "false"},
    {"showDMG", "false", "false"}, {"dmgScale", "0.5", NULL}, {"showDeaths", "true", NULL}, {"showStreak", "true", NULL},
    {"showFPS", "true", "false"}, {"showMedals", NULL, "false"}, {"playMedals", NULL, "false"}, {"medalSat", NULL, "1"},
    {"showUnboxings", "false", NULL}, {"crosshairSho", "4", NULL},
    {"crosshairImage", "\"https://assets.krunker.io/pro_crosshair.png\"", NULL}, {"crosshairAlways", "true", NULL},
    {"crosshairShoM", "4", NULL}, {"crosshairAlwaysM", "true", NULL}, {"aimSensitivityX", "0.69", NULL},
    {"aimSensitivityY", "0.69", NULL}, {"sound", "0.1", "0.1"}, {"micVolume", "0", NULL}, {"voiceVolume", "0", NULL},
    {"fov", "110", NULL}, {"fpsFOV", "105", NULL}, {"weaponBob", "0.2", "0"}, {"weaponLean", "2", "0"},
    {"weaponRot", "0", NULL}, {"weaponOffX", "1.4", NULL}, {"weaponOffY", "2", NULL}, {"weaponOffZ", "1.5", NULL},
    {"aimAnim", NULL, "false"}, {"hideADS", "true", "true"}, {"showSpdLines", NULL, "0"}, {"scoreScale", "0.6", NULL},
    {"scoreOffY", "2.5", NULL}, {"saturationn", NULL, "1"}, {"menuSaturation", "1", NULL}, {"feedLimit", "4", NULL},
    {"bulletTracers", NULL, "false"}, {"scopeBorders", "true", NULL}, {"useDamageOverlay", "false", "false"},
};

// KrunkNative addition: the "Beautiful" preset (index 4, offered after Performance in
// #settingsPreset; v7.1.4 has Default/Pro/Performance/Custom only). Like Pro/Performance
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
// Kept at Krunker's defaults on purpose: fov/fpsFOV (v7.1.4: 100 / 90; wide FOVs stretch the edges),
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
    {"lighting", "2"}, {"useDamageOverlay", "true"}, {"bulletCasings", "true"}, {"impactHoles", "true"},
    {"showExplo", "true"},
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
        if (!v && !strcmp(key, "netRate")) v = ui_setting_value(key); // the device's default (mobile 60 Hz)
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
    // settings.list.voiceDistance.hide = voiceChatType == 0: the captured row is display:none
    // (voice.c shows / hides it on a live change)
    char type[8] = "";
    ui_setting_to_string(ui_setting_value("voiceChatType"), type, sizeof(type));
    const HtmlElem distance = atoi(type) ? html_dom_by_id("voiceDistance_div") : 0;
    if (distance) html_dom_set_style(distance, "display", "block");
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

// ---- windows[0].searchList() / getSettings() with settingSearch set (v7.1.4): every setting
// of SETTINGS.list (any tab; the basic view skips sType "advanced" ones) whose label (i18n
// name) or category header contains the query, grouped by category, hidden rows (`<key>_div`
// display:none: dev, KPD, mobile-only) left out; the "Keybinds" row when "Keybinds" matches;
// "No settings found" when nothing does. The rows come from the advanced view's captured
// tabs (ui/windows/settings-advanced-*.html: every category).

// End of the element starting at `p` ("<div ..."): just past its matching "</div>".
static const char *div_end(const char *p) {
    int depth = 0;
    while (p && *p) {
        const char *open = strstr(p, "<div");
        const char *close = strstr(p, "</div>");
        if (!close) return NULL;
        if (open && open < close) {
            depth++;
            p = open + 4;
        } else {
            depth--;
            p = close + 6;
            if (depth == 0) return p;
        }
    }
    return NULL;
}

// Lower-cased text of an HTML fragment (tags dropped, the few entities v7's labels use decoded).
static void fragment_text(const char *s, const size_t n, char *out, const size_t cap) {
    size_t k = 0;
    int tag = 0;
    for (size_t i = 0; i < n && k + 1 < cap; i++) {
        const char c = s[i];
        if (c == '<') { tag = 1; continue; }
        if (c == '>') { tag = 0; continue; }
        if (tag) continue;
        if (c == '&') {
            static const struct { const char *ent; char ch; } ENTS[] = {{"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&#39;", '\''}, {"&nbsp;", ' '}};
            size_t e = 0;
            for (; e < sizeof(ENTS) / sizeof(ENTS[0]); e++) {
                const size_t l = strlen(ENTS[e].ent);
                if (i + l <= n && !strncmp(s + i, ENTS[e].ent, l)) {
                    out[k++] = ENTS[e].ch;
                    i += l - 1;
                    break;
                }
            }
            if (e < sizeof(ENTS) / sizeof(ENTS[0])) continue;
        }
        out[k++] = (char) tolower((unsigned char) c);
    }
    out[k] = 0;
}

// The setting's label: the row's text before its first child element (v7 writes
// i18n.t(name) first, then the controls).
static void row_label(const char *row, const char *row_end, char *out, const size_t cap) {
    const char *gt = strchr(row, '>');
    if (!gt || gt >= row_end) { out[0] = 0; return; }
    const char *lt = strchr(gt + 1, '<');
    if (!lt || lt > row_end) lt = row_end;
    fragment_text(gt + 1, (size_t) (lt - gt - 1), out, cap);
}

// The category header text without its arrow icon and the "* requires restart" note.
static void header_text(const char *hed, const char *hed_end, char *out, const size_t cap) {
    StrBuf sb = {0};
    sb_append_n(&sb, hed, (size_t) (hed_end - hed));
    char *h = sb_finish(&sb);
    static const char *const DROP[] = {"<span class=\"material-icons plusOrMinus\">", "<span id=\"requiresRestart\">"};
    for (size_t d = 0; d < sizeof(DROP) / sizeof(DROP[0]); d++) {
        char *at = strstr(h, DROP[d]);
        if (!at) continue;
        // the span's end: its own </span> (requiresRestart nests one more span)
        char *end = at;
        int depth = 0;
        do {
            char *o = strstr(end + 1, "<span"), *c = strstr(end + 1, "</span>");
            if (!c) { end = NULL; break; }
            if (o && o < c) { depth++; end = o; }
            else { depth--; end = c; }
        } while (depth > 0 && end);
        if (end) memmove(at, end + 7, strlen(end + 7) + 1);
    }
    fragment_text(h, strlen(h), out, cap);
    free(h);
}

// settings.js `sType: "advanced"`: rows the basic view leaves out, search included.
static const char *const ADVANCED_ONLY[] = {
    "showNetStat", "updateRate", "antiAlias", "noTex", "reflection", "softShad", "highResShad", "shadowsDynamic",
    "ambientShading", "showTrails", "yourTrails", "muzzleFlash", "bulletCasings", "impactHoles", "sniperFlap",
    "sniperFlapAnim", "textureAnim", "objectAnim", "screenShake", "weaponShine", "lighting", "showExplo",
    "oldScoreboard", "disableRarityAnim", "showPfp", "dynamicHP", "showHitInd", "hitColor", "deathMarkerOpac",
    "deathMarkers", "deathMarkerAdsOpac", "deathMarkerDur", "dmgType", "showDMG", "dmgColor", "critColor",
    "dmgScale", "showKills", "showKillC", "showDeaths", "showKD", "showScore", "showStreak", "speedOffX",
    "speedOffY", "speedScale", "speedOpacity", "speedColor", "speedColorMax", "nametagOpac", "nametagScale",
    "nametagStyle", "healthNum", "healthColT", "healthColE", "objOpac", "adsObjOpac", "hideNonTrade", "showMinimap",
    "minimapScale", "minimapZoom", "miniMapHudScale", "scrollDir", "invertY", "challMode", "weaponBob",
    "weaponLean", "weaponRot", "weaponOffX", "weaponOffY", "weaponOffZ", "weaponADSOffY", "weapSwapY",
    "weapReloadY", "adsFovMlt", "aimAnim", "hideADS", "showHands", "showWeapon", "showWeaponSec", "showWeaponMel",
    "leftHand", "roundArms", "wallLeanMlt", "wallLean",
};

static int advanced_only(const char *row, const char *row_end) {
    char key[64];
    StrBuf sb = {0};
    sb_append_n(&sb, row, (size_t) (row_end - row));
    char *r = sb_finish(&sb);
    const int found = extract_set_setting_key(r, key, sizeof(key));
    free(r);
    if (!found) return 0;
    for (size_t i = 0; i < sizeof(ADVANCED_ONLY) / sizeof(ADVANCED_ONLY[0]); i++) {
        if (!strcmp(ADVANCED_ONLY[i], key)) return 1;
    }
    return 0;
}

static int matches(const char *text, const char *query) {
    return query[0] && strstr(text, query) != NULL;
}

// windows[0].getSettings() of one holder body (a captured tab's #settHolder content, the
// Client tab) filtered by `query`; categories already emitted (`seen`) are skipped.
static void search_holder(StrBuf *out, const char *body, const char *query, StrBuf *seen, int *keybinds) {
    const char *p = body;
    while (p && (p = strstr(p, "<div class=\"")) != NULL) {
        const char *end = div_end(p);
        if (!end) break;
        if (!strncmp(p, "<div class=\"setHed\"", 18)) {
            const char *bod = end;
            while (*bod == ' ' || *bod == '\n' || *bod == '\t') bod++;
            const char *bod_end = !strncmp(bod, "<div", 4) ? div_end(bod) : NULL;
            if (!bod_end) { p = end; continue; }
            char id[64] = "";
            const char *idp = strstr(p, "id=\"setHed_");
            if (idp && idp < end) {
                idp += strlen("id=\"setHed_");
                const size_t l = strcspn(idp, "\"");
                snprintf(id, sizeof(id), "%.*s", (int) (l < sizeof(id) ? l : sizeof(id) - 1), idp);
            }
            char key[80];
            snprintf(key, sizeof(key), "|%s|", id);
            if (id[0] && seen->data && strstr(seen->data, key)) { p = bod_end; continue; }
            char head[256];
            header_text(p, end, head, sizeof(head));
            const int head_match = matches(head, query);
            StrBuf rows = {0};
            const char *body_open_end = strchr(bod, '>') + 1;
            for (const char *r = body_open_end; r && (r = strstr(r, "<div class=\"settName\"")) && r < bod_end;) {
                const char *r_end = div_end(r);
                if (!r_end || r_end > bod_end) break;
                const char *tag_end = strchr(r, '>');
                char tag[512];
                snprintf(tag, sizeof(tag), "%.*s", (int) (tag_end - r < (long) sizeof(tag) ? tag_end - r : (long) sizeof(tag) - 1), r);
                const int hidden = strstr(tag, "display:none") != NULL;
                char label[256];
                row_label(r, r_end, label, sizeof(label));
                const int basic_skip = menu_ui()->settings_view == 0 && advanced_only(r, r_end);
                if (!hidden && !basic_skip && (head_match || matches(label, query))) sb_append_n(&rows, r, (size_t) (r_end - r));
                r = r_end;
            }
            if (rows.len) {
                // head + body with the display:none of an empty category dropped (cnt > 0)
                StrBuf hb = {0};
                sb_append_n(&hb, p, (size_t) (end - p));
                sb_append_n(&hb, bod, (size_t) (body_open_end - bod));
                char *hs = sb_finish(&hb);
                StrBuf fixed = {0};
                sb_append_replace_all(&fixed, hs, " style=\"display:none\"", "");
                free(hs);
                sb_append(out, fixed.data);
                sb_free(&fixed);
                sb_append(out, rows.data);
                sb_append(out, "</div>");
                if (id[0]) sb_appendf(seen, "|%s|", id);
            }
            sb_free(&rows);
            p = bod_end;
            continue;
        }
        if (!strncmp(p, "<div class=\"setBodH\"", 20) && strstr(p, "showWindow(7)") && strstr(p, "showWindow(7)") < end) {
            if (!*keybinds && matches("keybinds", query)) { // searchMatches({name: "settings.controls.change"})
                sb_append_n(out, p, (size_t) (end - p));
                *keybinds = 1;
            }
        }
        p = end;
    }
}

// The #settHolder content of the current view's search for `query` (lower-cased).
static char *search_html(const char *query) {
    const int view = 1; // every category (the advanced tabs)
    StrBuf out = {0}, seen = {0};
    int keybinds = 0;
    for (int tab = 0; tab < ui_settings_view_tab_count(view); tab++) {
        const char *tpl = ui_settings_view_tab_html(view, tab);
        const char *holder = strstr(tpl, "<div id=\"settHolder\">");
        if (!holder) continue;
        char *ext = ui_client_settings_extend_tab(holder); // Default Region list, the Sound tab's client rows
        search_holder(&out, ext ? ext : holder, query, &seen, &keybinds);
        free(ext);
    }
    char *client_rows = ui_client_settings_tab_html(1); // Settings > Client (ui_client_settings.c)
    if (client_rows) search_holder(&out, client_rows, query, &seen, &keybinds);
    free(client_rows);
    sb_free(&seen);
    if (!out.len) sb_append(&out, "<div class=\"setHed\">No settings found</div>"); // i18n settings.none
    return sb_finish(&out);
}

// windows[0].getTabs() with tabIndex -1 (searching): no tab marked, transparent borders.
static void search_tabs(void) {
    HtmlElem layout = html_dom_by_id("settingsTabLayout");
    if (!layout) return;
    const int view = menu_ui()->settings_view;
    StrBuf sb = {0};
    const int tabs = ui_settings_view_tab_count(view);
    for (int t = 0; t < tabs; t++) {
        if (t) sb_append(&sb, "\n");
        sb_appendf(&sb, "<div class=\"settingTab\" style=\"border-bottom: 5px solid transparent\" onmouseenter=\"playTick()\" "
                        "onclick=\"playSelect(0.1);window.windows[0].changeTab(%d)\">", t);
        sb_append_esc(&sb, ui_settings_view_tab_name(view, t));
        sb_append(&sb, "</div>");
    }
    const int client = ui_client_settings_tab();
    if (client >= 0)
        sb_appendf(&sb, "\n<div class=\"settingTab\" style=\"border-bottom: 5px solid transparent\" onmouseenter=\"playTick()\" "
                        "onclick=\"playSelect(0.1);window.windows[0].changeTab(%d)\">Client</div>", client);
    html_dom_set_html(layout, sb.data ? sb.data : "");
    sb_free(&sb);
}

void ui_settings_search(const char *query) {
    // settingSearch = settSearch.value (untrimmed); searchMatches lowercases both sides.
    char q[128];
    snprintf(q, sizeof(q), "%s", query ? query : "");
    str_tolower(q);
    HtmlElem holder = html_dom_by_id("settHolder");
    if (!holder) return;
    if (!q[0]) { // changeTab(tabIndex = 0)
        menu_ui()->settings_tab = 0;
        menu_show_window(1, 1);
        return;
    }
    menu_ui()->settings_tab = -1; // windows[0].tabIndex = -1
    search_tabs();
    char *html = search_html(q);
    html_dom_set_html(holder, html);
    free(html);
    ui_settings_apply_values(holder);
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
    else if (!strcmp(key, "challMode")) in->chall_mode = parse_bool(value); // WP-C: settings.tmp.challMode, sent in the next ENT
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
    else if (!strcmp(key, "micQuality") || !strcmp(key, "micVolume") || !strcmp(key, "voiceChatType") || !strcmp(key, "voiceDistance") ||
             !strcmp(key, "muteSpec") || !strcmp(key, "voiceChatTrigger"))
        voice_apply_setting(key, value); // WP-E: v7 voice chat settings (voice.c)
    else return; // cosmetic Krunker settings the engine has no counterpart for (kept in the store only)

    printf("[ui] setting %s=%s\n", key, value);
}

// ---------------------------------------------------------------- inline handlers

static const char *arg_text(const HtmlEvent *ev, const HtmlArg *args, const int argc, const int i, char *buf, const size_t n) {
    if (i >= argc) return "";
    const HtmlArg *a = &args[i];
    switch (a->kind) {
        case HTML_ARG_STR: return a->str ? a->str : "";
        case HTML_ARG_NUM: snprintf(buf, n, "%.15g", a->num); return buf;
        case HTML_ARG_BOOL: return a->num != 0.0 ? "true" : "false";
        case HTML_ARG_THIS_CHECKED: return ev && ev->checked ? "true" : "false";
        case HTML_ARG_THIS_VALUE:
            if (ev && ev->value && ev->value[0]) return ev->value;
            if (ev && ev->target && html_dom_get_value(ev->target, buf, n) >= 0) return buf; // <select> snapshot reads ""
            return "";
        default: return "";
    }
}

// window.delayExecute(key, el, ms): on every key press in a slider's number box, the
// setting takes the box's value on the next frame (requestAnimationFrame: after the
// pressed key changed it).
typedef struct {
    char key[64];
    HtmlElem el;
} DelayedSet;

static void delayed_set_fire(void *user) {
    DelayedSet *d = user;
    char value[64];
    if (d->el && html_dom_get_value(d->el, value, sizeof(value)) >= 0) ui_settings_set_string(d->key, value);
    free(d);
}

static void delay_execute(const char *key, const HtmlElem el) {
    if (!key || !*key || !el) return;
    DelayedSet *d = calloc(1, sizeof(*d));
    if (!d) return;
    snprintf(d->key, sizeof(d->key), "%s", key);
    d->el = el;
    html_timer_add(0.0, delayed_set_fire, d);
}

// window.aspectSelect(key, select): the Presets <select> of Aspect Ratio writes its pick
// ("Native" = "", else config.customRatios[i]: the option text) into #slid_<key>, goes back
// to "Presets" and sets the setting.
static void aspect_select(const char *key, const HtmlElem select, const char *picked) {
    char id[96], value[64];
    snprintf(value, sizeof(value), "%s", picked && strcmp(picked, "Native") ? picked : "");
    snprintf(id, sizeof(id), "slid_%s", key);
    HtmlElem box = html_dom_by_id(id);
    if (box) html_dom_set_value(box, value);
    if (select) html_dom_set_value(select, "");
    ui_settings_set_string(key, value);
}

// windows[0].toggleType(this): settingType = checked ? "advanced" : "basic",
// saveVal("krk_advanced", ~~checked), updateWindow(1) (a tab the other view lacks -> 0).
static void toggle_type(const int advanced) {
    MenuUi *ui = menu_ui();
    ui->settings_view = !!advanced;
    html_storage_set("krk_advanced", advanced ? "1" : "0");
    if (ui->settings_tab != ui_client_settings_tab() && (ui->settings_tab < 0 || ui->settings_tab >= ui_settings_view_tab_count(ui->settings_view)))
        ui->settings_tab = 0;
    menu_rerender_if_open(1);
}

// window.importSettingsPopup(): genericPopup with the import <textarea id="importTxt">.
static void import_popup(void) {
    HtmlElem holder = html_dom_by_id("popupHolder"), pop = html_dom_by_id("genericPop");
    if (!pop) {
        ui_settings_import_prompt();
        return;
    }
    ui_loadout_clear_pops(); // resetPops()
    html_dom_set_attr(pop, "class", "importPop");
    html_dom_set_attr(pop, "style", "");
    html_dom_set_html(pop, "<div class=\"pubHed\">Import Settings</div><div style=\"color:rgba(255,255,255,0.7);font-size:19px;text-align:center;margin-bottom:15px;\">"
                           "Copy Paste Settings Text Here</div><textarea id=\"importTxt\" rows=\"3\" onkeydown=\"if(event.keyCode == 13) importSettings();\"></textarea>"
                           "<div class=\"mapFndB\" onclick=\"importSettings()\" onmouseover=\"playTick(0.1)\" style=\"margin-left: 0px;margin-top: 10px;background-color:#4ca9f5;width: 380px;\">Import</div>");
    html_dom_set_display(pop, "block");
    if (holder) html_dom_set_display(holder, "block");
    HtmlElem txt = html_dom_by_id("importTxt");
    if (txt) html_dom_focus(txt);
    menu_schedule_rect();
}

// window.importSettings(): the popup's text through settings.import, then clearPops().
static void import_settings(const char *text_arg) {
    if (text_arg && *text_arg) {
        if (ui_settings_import_text(text_arg)) menu_rerender_if_open(1);
        return;
    }
    HtmlElem txt = html_dom_by_id("importTxt");
    if (!txt) {
        import_popup();
        return;
    }
    char *buf = malloc(UI_SETTINGS_IMPORT_MAX);
    if (buf && html_dom_get_value(txt, buf, UI_SETTINGS_IMPORT_MAX) >= 0 && buf[0] && ui_settings_import_text(buf)) menu_rerender_if_open(1);
    free(buf);
    ui_loadout_clear_pops();
}

// An element argument (`this`): the web backend passes its id, the native engine the event
// target (the element whose handler runs).
static HtmlElem arg_elem(const HtmlEvent *ev, const HtmlArg *args, const int argc, const int i) {
    if (i < argc && args[i].kind == HTML_ARG_THIS && args[i].num > 0) return (HtmlElem) args[i].num;
    return ev ? ev->target : 0;
}

// Inline handlers this module owns (ui_handlers.c h_fallback); -1 = not ours.
int ui_settings_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    if (!name) return -1;
    char buf[256];
    if (!strcmp(name, "delayExecute") || !strcmp(name, "window.delayExecute")) {
        const HtmlElem el = arg_elem(ev, args, argc, 1);
        delay_execute(arg_text(ev, args, argc, 0, buf, sizeof(buf)), el);
        return 1; // `return true`
    }
    if (!strcmp(name, "aspectSelect")) {
        char picked[64] = "";
        const HtmlElem sel = arg_elem(ev, args, argc, 1);
        if (sel) html_dom_get_value(sel, picked, sizeof(picked));
        aspect_select(arg_text(ev, args, argc, 0, buf, sizeof(buf)), sel, picked);
        return 1;
    }
    if (!strcmp(name, "windows.0.toggleType")) {
        toggle_type(ev ? ev->checked : 0);
        return 1;
    }
    if (!strcmp(name, "selectSettingPre")) { // the #settingsPreset select (this.value) or a preset box (a number)
        const char *v = arg_text(ev, args, argc, 0, buf, sizeof(buf));
        ui_settings_select_preset(*v ? (int) strtol(v, NULL, 10) : 2);
        return 1;
    }
    if (!strcmp(name, "importSettingsPopup")) {
        import_popup();
        return 1;
    }
    if (!strcmp(name, "importSettings")) {
        import_settings(argc > 0 && args[0].kind == HTML_ARG_STR ? args[0].str : NULL);
        return 1;
    }
    return -1;
}
