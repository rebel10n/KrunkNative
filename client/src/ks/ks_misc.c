// GAME.COOKIES, GAME.SETTINGS, GAME.PLATFORM, GAME.MODS, GAME.ADS, GAME.setTitle, GAME.changeGame
// and UTILS.copyToClipboard (doc/v7/krunkscript.md 3.1.1, 3.1.5, 3.1.11, 3.1.13, 3.1.24, 3.1.26;
// v7 createKrunkScriptApiContext.js:1307-1327, 1848-1882, 2325-2370, 3268-3278).
#include "ks_client_internal.h"
#include <html_dom.h>
#include <html_ui.h>
#include <ui_logic.h>
#include <ui_mobile.h>
#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

// ---------------------------------------------------------------------------
// GAME.COOKIES: localStorage "krkcst_<mapId>_<key>"; map id = the map's name (KN has no
// custom-map ids for catalogue maps; v7 used customMapData.id).

static int cookie_key(KsScript *s, const KsValue key_v, char *out, const size_t n) {
    if (!ksc_is_str(key_v)) return 0;
    const Map *map = g_ksc.client && g_ksc.client->game.map ? g_ksc.client->game.map : NULL;
    const char *map_id = map && map->name && map->name[0] ? map->name : "map";
    char key[256];
    ksc_str(s, key_v, key, sizeof(key));
    char full[512];
    snprintf(full, sizeof(full), "krkcst_%s_%s", map_id, key);
#ifdef __EMSCRIPTEN__
    snprintf(out, n, "%s", full); // localStorage takes any key
#else
    // native storage keys are file names: escape the rest, hash what does not fit
    size_t k = 0;
    for (const unsigned char *p = (const unsigned char *) full; *p && k + 4 < n && k < 80; p++) {
        const int ok = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_';
        if (ok) out[k++] = (char) *p;
        else k += (size_t) snprintf(out + k, n - k, "-%02x", *p);
    }
    out[k] = 0;
    if (strlen(full) > 60) {
        uint32_t h = 2166136261u;
        for (const unsigned char *p = (const unsigned char *) full; *p; p++) h = (h ^ *p) * 16777619u;
        if (k > 60) k = 60;
        snprintf(out + k, n - k, "-%08x", h);
    }
#endif
    return 1;
}

// save(key, value): strings, numbers and arrays (String(value)); booleans and objects are
// refused (v7's `typeof value == Boolean` is never true).
static KsValue ck_save(KsScript *s, void *u, const KsValue *a, int n) {
    (void) u;
    char key[512];
    const KsValue v = ksc_arg(a, n, 1);
    if (!cookie_key(s, ksc_arg(a, n, 0), key, sizeof(key))) return ks_undef();
    if (v.tag != KS_STR && !ksc_is_num(v) && v.tag != KS_ARR) return ks_undef();
    char *text = malloc(65536);
    if (!text) return ks_undef();
    ks_to_string(s, v, text, 65536);
    html_storage_set(key, text);
    free(text);
    return ks_undef();
}

static KsValue ck_remove(KsScript *s, void *u, const KsValue *a, int n) {
    (void) u;
    char key[512];
    if (cookie_key(s, ksc_arg(a, n, 0), key, sizeof(key))) html_storage_set(key, NULL);
    return ks_undef();
}

// load(key): getSavedVal(key) + "" ("null" when missing, "" for an empty key)
static KsValue ck_load(KsScript *s, void *u, const KsValue *a, int n) {
    (void) u;
    char key[512];
    if (!cookie_key(s, ksc_arg(a, n, 0), key, sizeof(key))) return ks_str(s, "", 0);
    const int len = html_storage_get(key, NULL, 0);
    if (len < 0) return ks_str(s, "null", 4);
    char *buf = malloc((size_t) len + 1);
    if (!buf) return ks_str(s, "null", 4);
    html_storage_get(key, buf, (size_t) len + 1);
    const KsValue out = ks_str(s, buf, strlen(buf));
    free(buf);
    return out;
}

static KsValue ck_has(KsScript *s, void *u, const KsValue *a, int n) {
    (void) u;
    char key[512];
    return ks_bool(cookie_key(s, ksc_arg(a, n, 0), key, sizeof(key)) && html_storage_get(key, NULL, 0) >= 0);
}

// ---------------------------------------------------------------------------
// GAME.SETTINGS: the v7.1.4 settings a script may touch (settings.js entries that are not of
// type url / text and not flagged dev / kpd / verified / noMod). KN answers the ones its own
// settings store knows (ui_setting_value); the others read as undefined.
static const char *const SETTING_KEYS[] = {
    "lagComp", "netRerouting", "oldBrowser", "defaultRegionOnly", "rawMouse", "flickClamp", "mouseFlick",
    "aimFreezeFix", "shaderRendering", "mouseAccel", "extraBan", "resolution", "updateRate",
    "aspectRatio", "antiAlias", "lowSpec", "noTex", "mapDet", "particles", "particlesDist", "renderDist",
    "reflection", "shadows", "softShad", "highResShad", "shadowsDynamic", "ambientShading", "showTrails",
    "yourTrails", "muzzleFlash", "bulletCasings", "impactHoles", "sniperFlap", "sniperFlapAnim",
    "textureAnim", "objectAnim", "screenShake", "weaponShine", "lighting", "showExplo", "postProcessing",
    "scaleUI", "uiScaling", "showUI", "oldScoreboard", "disableRarityAnim", "showPfp", "dynamicHP",
    "showHitInd", "hitColor", "deathMarkerOpac", "deathMarkers", "deathMarkerAdsOpac", "deathMarkerDur",
    "dmgType", "showDMG", "dmgColor", "critColor", "dmgScale", "showKills", "showKillC", "showDeaths",
    "showKD", "showScore", "showStreak", "showPing", "showFPS", "fpsRate", "showSpeed", "speedOffX",
    "speedOffY", "speedScale", "speedOpacity", "speedColor", "speedColorMax", "hideNames", "nametagOpac",
    "nametagScale", "nametagStyle", "healthNum", "healthColT", "healthColE", "objOpac", "adsObjOpac",
    "hideNonTrade", "showMinimap", "minimapScale", "minimapZoom", "miniMapHudScale", "showMedals",
    "classicMedals", "playMedals", "medalScale", "medalOffX", "medalOffY", "medalSat", "medalHue",
    "shwChat", "chatFilter", "showMessages", "showUnboxings", "chatOp", "chatBGOp", "chatTextOutline",
    "chatHeight", "chatTimer", "crosshairSho", "crosshairStyle", "crosshairImageW", "crosshairImageH",
    "crosshairOpacity", "crosshairUseOpacity", "crosshairAlways", "crosshairColor", "crosshairShadow",
    "crosshairShadowThickess", "crosshairThick", "crosshairLen", "crosshairGap", "crosshairDot",
    "crosshairShoM", "crosshairStyleM", "crosshairImageWM", "crosshairImageHM", "crosshairOpacityM",
    "crosshairUseOpacityM", "crosshairAlwaysM", "crosshairColorM", "crosshairShadowM",
    "crosshairShadowThickessM", "crosshairThickM", "crosshairLenM", "crosshairGapM", "crosshairDotM",
    "hitm", "hitmCol", "hitmKCol", "hitOpac", "hitmLen", "hitmThick", "hitmSpac", "hitmAnimD",
    "hitmAnimS", "hitmFad", "gpSetts", "sensitivityX", "sensitivityY", "aimSensitivityX",
    "aimSensitivityY", "scrollDir", "invertY", "challMode", "disableCons", "sensitivityXCntrl",
    "sensitivityYCntrl", "aimSensitivityXCntrl", "aimSensitivityYCntrl", "deadZoneL", "deadZoneR",
    "triggerThres", "invertYCntrl", "vibrationMlt", "vibration", "gradSpeed", "sensitivityXMbl",
    "sensitivityYMbl", "aimSensitivityXMbl", "aimSensitivityYMbl", "invertYMbl", "vibrationMbl",
    "gradSpeedMbl", "sound", "ambientVolume", "dialogueVolume", "audioInput", "micQuality", "micVolume",
    "voiceVolume", "voiceChatType", "voiceDistance", "gunsVolume", "playerVolume", "skinVolume",
    "uiVolume", "assetVolume", "streamMode", "vmSetts", "fov", "fpsFOV", "weaponBob", "weaponLean",
    "weaponRot", "weaponOffX", "weaponOffY", "weaponOffZ", "weaponADSOffY", "weapSwapY", "weapReloadY",
    "adsFovMlt", "toggleADS", "aimAnim", "hideADS", "showHands", "showWeapon", "showWeaponSec",
    "showWeaponMel", "leftHand", "roundArms", "wallLeanMlt", "wallLean", "hudHealthHigh", "hudHealthLow",
    "showSpdLines", "spdLinesCol", "showPScore", "scoreColor", "scoreShadow", "scoreScale", "scoreOffX",
    "scoreOffY", "progColor", "progOpac", "progShadow", "progScale", "progOffX", "progOffY", "progRot",
    "progMColor", "progMOColor", "progMOpac", "progMShadow", "progMScale", "progMOffX", "progMOffY",
    "progMRot", "saturationn", "menuSaturation", "hue", "menuHue", "vignette", "feedLimit",
    "bulletTracers", "bulletTracerCol", "tracerOffset", "trailCol", "ropeCol", "scopeBorderCol",
    "scopeBorders", "scopeBorderOpac", "scopeWidth", "scopeHeight", "scopeOpac", "reticleWidth",
    "reticleHeight", "useDamageOverlay", "scoreBoardTrigger", "voiceChatTrigger",
};

static int setting_allowed(const char *key) {
    for (size_t i = 0; i < sizeof(SETTING_KEYS) / sizeof(SETTING_KEYS[0]); i++) {
        if (!strcmp(SETTING_KEYS[i], key)) return 1;
    }
    return 0;
}

// set(key, value): settings.set(key, value) like the user changing it (persisted)
static KsValue st_set(KsScript *s, void *u, const KsValue *a, int n) {
    (void) u;
    if (!ksc_is_str(ksc_arg(a, n, 0))) return ks_undef();
    char key[64];
    ksc_str(s, ksc_arg(a, n, 0), key, sizeof(key));
    if (!setting_allowed(key) || !ui_setting_value(key)) return ks_undef();
    const KsValue v = ksc_arg(a, n, 1);
    char text[512];
    HtmlArg arg = {HTML_ARG_STR, 0, NULL};
    if (v.tag == KS_NUM) arg = (HtmlArg) {HTML_ARG_NUM, v.as.num, NULL};
    else if (v.tag == KS_BOOL) arg = (HtmlArg) {HTML_ARG_BOOL, v.as.b ? 1 : 0, NULL};
    else arg.str = ksc_str(s, v, text, sizeof(text));
    ui_settings_set(key, &arg);
    return ks_undef();
}

// get(key): String(current value)
static KsValue st_get(KsScript *s, void *u, const KsValue *a, int n) {
    (void) u;
    if (!ksc_is_str(ksc_arg(a, n, 0))) return ks_undef();
    char key[64];
    ksc_str(s, ksc_arg(a, n, 0), key, sizeof(key));
    const cJSON *value = setting_allowed(key) ? ui_setting_value(key) : NULL;
    if (!value) return ks_undef();
    char text[512];
    ui_setting_to_string(value, text, sizeof(text));
    return ks_str(s, text, strlen(text));
}

static KsValue st_list(KsScript *s, void *u, const KsValue *a, int n) {
    (void) u; (void) a; (void) n;
    const KsValue out = ks_arr_new(s);
    for (size_t i = 0; i < sizeof(SETTING_KEYS) / sizeof(SETTING_KEYS[0]); i++) {
        if (ui_setting_value(SETTING_KEYS[i])) ks_arr_push(s, out, ks_str(s, SETTING_KEYS[i], strlen(SETTING_KEYS[i])));
    }
    return out;
}

// ---------------------------------------------------------------------------
// GAME.PLATFORM

static KsValue pf_is_client(KsScript *s, void *u, const KsValue *a, int n) {
    (void) s; (void) u; (void) a; (void) n;
#ifdef __EMSCRIPTEN__
    return ks_bool(0);
#else
    return ks_bool(1); // window.isDeskCli: the native client
#endif
}

static KsValue pf_is_browser(KsScript *s, void *u, const KsValue *a, int n) {
    (void) s; (void) u; (void) a; (void) n;
#ifdef __EMSCRIPTEN__
    return ks_bool(!ui_mobile_active());
#else
    return ks_bool(0);
#endif
}

static KsValue pf_is_mobile(KsScript *s, void *u, const KsValue *a, int n) {
    (void) s; (void) u; (void) a; (void) n;
    return ks_bool(ui_mobile_active());
}

// ---------------------------------------------------------------------------
// GAME root, MODS, ADS, UTILS.copyToClipboard

// setTitle(title): keeps [\w\- ]; empty -> ignored
static KsValue g_set_title(KsScript *s, void *u, const KsValue *a, int n) {
    (void) u;
    if (!ksc_is_str(ksc_arg(a, n, 0))) return ks_undef();
    char raw[256], title[256];
    ksc_str(s, ksc_arg(a, n, 0), raw, sizeof(raw));
    size_t k = 0;
    for (const char *p = raw; *p && k + 1 < sizeof(title); p++) {
        const char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == ' ') title[k++] = c;
    }
    title[k] = 0;
    if (!k) return ks_undef();
#ifdef __EMSCRIPTEN__
    emscripten_set_window_title(title);
#else
    if (g_ksc.client && g_ksc.client->window) glfwSetWindowTitle(g_ksc.client->window, title);
#endif
    return ks_undef();
}

// changeGame(mapName): window.playMap(escapeStr(name)), rate g_change 3 (2 allowed). The page's
// playMap runs on the next UI tick: it may unload this very script.
static char g_change_map[128];

static void change_game_now(void *user) {
    (void) user;
    if (!g_change_map[0]) return;
    HtmlArg arg = {HTML_ARG_STR, 0, g_change_map};
    HtmlEvent ev = {0};
    ev.type = "click";
    ev.value = "";
    html_ui_dispatch("playMap", &ev, &arg, 1);
    g_change_map[0] = 0;
}

static KsValue g_change_game(KsScript *s, void *u, const KsValue *a, int n) {
    (void) u;
    if (!ksc_game() || !ks_game_rate(ksc_game(), "g_change", 3, 0)) return ks_undef();
    char raw[128];
    ksc_str(s, ksc_arg(a, n, 0), raw, sizeof(raw));
    size_t k = 0;
    for (const char *p = raw; *p && k + 1 < sizeof(g_change_map); p++) {
        if (*p != '<' && *p != '>' && *p != '"' && *p != '\'' && *p != '&') g_change_map[k++] = *p; // escapeStr
    }
    g_change_map[k] = 0;
    if (k) html_timer_add(0, change_game_now, NULL);
    return ks_undef();
}

// MODS.load(url): rate loadmd 2. KN loads no remote mod packs from scripts (2.7): no-op.
static KsValue md_load(KsScript *s, void *u, const KsValue *a, int n) {
    (void) s; (void) u; (void) a; (void) n;
    if (ksc_game()) ks_game_rate(ksc_game(), "loadmd", 2, 0);
    return ks_undef();
}

static KsValue noop(KsScript *s, void *u, const KsValue *a, int n) {
    (void) s; (void) u; (void) a; (void) n;
    return ks_undef();
}

static KsValue u_copy(KsScript *s, void *u, const KsValue *a, int n) {
    (void) u;
    char text[4096];
    html_clipboard_write(ksc_str(s, ksc_arg(a, n, 0), text, sizeof(text)));
    return ks_undef();
}

void ks_misc_api(KsApiImpl *api) {
    api[KS_API_COOKIES_SAVE].fn = ck_save;
    api[KS_API_COOKIES_REMOVE].fn = ck_remove;
    api[KS_API_COOKIES_LOAD].fn = ck_load;
    api[KS_API_COOKIES_HAS].fn = ck_has;
    api[KS_API_SETTINGS_SET].fn = st_set;
    api[KS_API_SETTINGS_GET].fn = st_get;
    api[KS_API_SETTINGS_LIST].fn = st_list;
    api[KS_API_PLATFORM_ISCLIENT].fn = pf_is_client;
    api[KS_API_PLATFORM_ISBROWSER].fn = pf_is_browser;
    api[KS_API_PLATFORM_ISMOBILE].fn = pf_is_mobile;
    api[KS_API_SETTITLE].fn = g_set_title;
    api[KS_API_CHANGEGAME].fn = g_change_game;
    api[KS_API_MODS_LOAD].fn = md_load;
    api[KS_API_MODS_RESET].fn = noop;
    api[KS_API_ADS_PLAYVIDEO].fn = noop; // no ads: onAdFinished never fires
    api[KS_API_UTILS_COPYTOCLIPBOARD].fn = u_copy;
}

void ks_misc_unload(void) { g_change_map[0] = 0; }
