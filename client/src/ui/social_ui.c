// Krunker v3.1.1 social hub (public/social.html + js/social.js, readable source
// src/social.js and src/ui/social.js in the v3.1.1 tree) ported to C over html_dom.h.
// The page is ui/social.html (imported by tools/ui-import/social_page.py); app_mode.c
// shows it for APP_MODE_HUB (--hub, the web page social.html, the menu's Hub button and every
// /social.html link) and calls the AppPage hooks at the bottom of this file.
//
// Krunker's hub talks to the matchmaker over its own socket; here the same request /
// reply frames travel over the game connection as JSON (shared/include/social.h) and the
// KrunkNative server (or the in-process local server) answers them like the v3.1.1
// offline server does. KrunkNative has no accounts, so the hub is always a guest:
// logins are refused by the server, and every view shows what the real page shows a
// guest for the data the server has (the map catalogue is the server's own map list).
//
// Function names in comments are social.js's (minified) names: updateWindow, io_0,
// Xa (send "r"), Se (side boxes), La (map cards), windows[0]/windows[1] (maps/mods).
#include <app_mode.h>
#include <audio.h>
#include <backend.h>
#include <backend_ui.h>
#include <weapons.h>
#include <cJSON.h>
#include <html_dom.h>
#include <html_ui.h>
#include <social.h>
#include <ui_logic.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#define HUB_KEY_MAX 128

// ------------------------------------------------------------------ v3.1.1 data (src/config.js, i18n/en.js)

typedef struct {
    const char *name;
    const char *alias;
    const char *tag;
    int id;
} HubFollow;

// config.whoToFol
static const HubFollow k_who_to_follow[] = {
    {"Sidney", NULL, "Developer", 201211}, {"Vince", NULL, "Developer", 105683}, {"Tehchy", NULL, "Developer", 289362},
    {"MoonJuiceAlt", "MoonJuice", "Streamer", 2968503}, {"ImWaspy", NULL, "Streamer", 2593871},
    {"hey_im_zed", "HeyImZed", "Streamer", 177694}, {"cimota", NULL, "Skin Maker", 7516049},
    {"Kilfy", NULL, "Skin Maker", 8871318}, {"iSpy", NULL, "Skin Maker", 177694}, {"Nxbulah", NULL, "Skin Maker", 5739887},
    {"Skriller33", NULL, "Skin Maker", 5068654}, {"Andrii", NULL, "Map Maker", 163429}, {"JavieBhop", NULL, "Map Maker", 1517556},
    {"Andrii", NULL, "Map Maker", 163429}, {"Keybi", NULL, "Map Maker", 310968}, {"thejavi99", NULL, "Map Maker", 1498591},
    {"owOmar", NULL, "Mod Maker", 5461278}, {"siktis", NULL, "Mod Maker", 5746131}, {"DukeV", NULL, "Map Maker", 943615},
    {"Chwoubaka", NULL, "Mod Maker", 4927907}, {"PhilZ", NULL, "Youtuber", 2378875},
    {"FrostyWolfTTV", "FrostyWolf", "Streamer", 5090891}, {"JamemesG", NULL, "E-Sports", 3524000},
    {"jcStani", NULL, "E-Sports", 6895308}, {"COLOR", NULL, "E-Sports", 164485}, {"roadman", NULL, "E-Sports", 241773},
    {"revzi", NULL, "E-Sports", 8345163}, {"Mosswi", NULL, "Staff", 2768744}, {"wingmangod69", "Wingman", "E-Sports", 2449631},
    {"Swixie", NULL, "Streamer", 5720620}, {"QuackyTTV", "Quacky", "Streamer", 3742667}, {"Highnoon", NULL, "Support", 361086},
    {"StremZ", NULL, "Streamer", 5045705},
};
#define WHO_TO_FOLLOW_COUNT (sizeof(k_who_to_follow) / sizeof(k_who_to_follow[0]))

static const char *const k_social_regions[] = {"All Regions", "Oceania", "Asia", "Europe", "Americas"};

// config.mapTabs / config.modTabs: {search, sendID, noPages} per tab index (6 entries; the
// page's tab buttons are indexed differently, which Krunker's page does too).
typedef struct {
    int search, send_id, no_pages;
} HubTab;
static const HubTab k_list_tabs[6] = {{0, 0, 0}, {0, 0, 1}, {0, 0, 0}, {0, 1, 0}, {0, 1, 0}, {1, 0, 0}};

// navigation buttons (Za) and the pages without one ($a)
static const char *const k_nav_pages[] = {"leaders", "mods", "maps", "market", "clanwars", "feed"};
static const char *const k_sub_pages[] = {"itemsales", "clan", "map", "mod", "search", "profile", "tourney"};
static const char *const k_market_queries[] = {"market", "inventory", "sales", "stats", "trades"};

#define STR_MAPS_NONE "No Games found..."       // social.maps.none
#define STR_MODS_NONE "No Mods found..."        // social.mods.none
#define STR_WIN_MAPS_NONE "No Games found!"     // windows.maps.none
#define STR_WIN_MODS_NONE "No Mods found"       // windows.mods.none
#define STR_MARKET_NONE "No Items Found"        // social.market.none
#define STR_MARKET_LOGIN "You must be logged in." // social.market.login
#define STR_MARKET_FAILED "Failed. Try Again"   // social.market.failed

// ------------------------------------------------------------------ settings (re = {...})

typedef enum { SET_SELECT, SET_SLIDER, SET_SWITCH, SET_HIDDEN } HubSettingKind;

typedef struct {
    const char *key;
    const char *name;   // en.js text of re[key].name
    HubSettingKind kind;
    double def, min, max, step;
    int hide;           // -1 = no `hide` field, 0/1 = re[key].hide
    double val;
} HubSetting;

enum { HS_REGION, HS_SCALE_UI, HS_UI_SCALING, HS_RARITY_ANIM, HS_SHOW_PFP, HS_SOUND, HS_COUNT };

static HubSetting g_settings[HS_COUNT] = {
    {"region", "Region Stats", SET_SELECT, 0, 0, 0, 0, -1, 0},
    {"scaleUI", "UI Scale", SET_SLIDER, 0.7, 0.1, 1, 0.1, -1, 0},
    {"uiScaling", "UI Scale", SET_HIDDEN, 1, 0, 0, 0, 1, 0},
    {"disableRarityAnim", "Disable Rarity Animations", SET_SWITCH, 0, 0, 0, 0, -1, 0},
    {"showPfp", "Show Profile Pictures", SET_SWITCH, 1, 0, 0, 0, -1, 0},
    {"sound", "Master Volume", SET_SLIDER, 1, 0, 1, 0.1, -1, 0},
};

// ------------------------------------------------------------------ state

typedef struct {
    int used;
    int load_ind;
    int waiting;
    StrBuf dat;      // accumulated cards ("dat"), empty = none yet
    int has_dat;
} HubListData;

typedef struct {
    int tab_index;
    HubListData data[8];
} HubList;          // windows[0] (maps) / windows[1] (mods)

typedef struct {
    Client *client;
    int entered;
    double enter_time;
    int connected;          // T: the socket callback ran
    int error_shown;        // Qe: io_1 shown once
    char query[1024];

    // updateWindow state
    char v[32];             // page
    char b[HUB_KEY_MAX];    // sub page / query ("" = null)
    char k[HUB_KEY_MAX];    // t
    char x[HUB_KEY_MAX];    // n
    char S[HUB_KEY_MAX * 2];// expected io_0 key, "" = none
    int has_s;
    int feed_switching;     // Ze
    char ja_k[HUB_KEY_MAX]; // Ja[4] (feed tab)

    // settings effects
    int region;             // V
    double ui_scale_b;      // B
    int ui_scaling;         // D
    int rarity_anim;        // j
    int show_pfp;           // F
    double volume;

    HubList maps, mods;
    uint32_t next_id;
    SocialAssembly assembly;

    // brain (backend.h): the real social socket instead of the game connection
    int brain;              // backend_enabled() when the page was entered
    int fe;                 // fe: a silent token login is in flight (no success popup)
    int logged_in;          // acc != null
    char profile_name[64];  // profile.player_name (follow / followers)
    int profile_id;
    double profile_followed, profile_following;
    int posting;            // Ve: a post is being submitted
} Hub;

static Hub g_hub;

// ------------------------------------------------------------------ small helpers

static HtmlElem el(const char *id) {
    return html_dom_by_id(id);
}

static void set_display(const char *id, const char *value) {
    const HtmlElem e = el(id);
    if (e) html_dom_set_display(e, value);
}

static void set_html(const char *id, const char *html) {
    const HtmlElem e = el(id);
    if (e) html_dom_set_html(e, html ? html : "");
}

static void set_style(const char *id, const char *prop, const char *value) {
    const HtmlElem e = el(id);
    if (e) html_dom_set_style(e, prop, value);
}

static int str_in(const char *s, const char *const *list, const size_t n) {
    for (size_t i = 0; i < n; i++) if (!strcmp(s, list[i])) return 1;
    return 0;
}

// JS Number#toString for the values the hub prints.
static void js_num(char *out, const size_t n, const double v) {
    if (v == floor(v) && fabs(v) < 1e15) {
        snprintf(out, n, "%.0f", v);
        return;
    }
    snprintf(out, n, "%.15g", v);
}

// UTILS.commaNumber
static void comma_number(char *out, const size_t n, const double v) {
    char raw[64];
    js_num(raw, sizeof(raw), v);
    const char *dot = strchr(raw, '.');
    const size_t int_len = dot ? (size_t) (dot - raw) : strlen(raw);
    size_t o = 0;
    const size_t start = raw[0] == '-' ? 1 : 0;
    for (size_t i = 0; i < int_len && o + 2 < n; i++) {
        if (i > start && (int_len - i) % 3 == 0) out[o++] = ',';
        out[o++] = raw[i];
    }
    snprintf(out + o, n - o, "%s", dot ? dot : "");
}

// String.prototype.escape: \ " ' get a backslash (used inside &quot;...&quot; args).
static void sb_append_js_escape(StrBuf *sb, const char *s) {
    for (; s && *s; s++) {
        if (*s == '\\' || *s == '"' || *s == '\'') sb_append_n(sb, "\\", 1);
        sb_append_n(sb, s, 1);
    }
}

static int rand_int(const int lo, const int hi) {
    return lo + (int) ((double) rand() / ((double) RAND_MAX + 1.0) * (hi - lo + 1));
}

static void hub_sound(const char *name, const float volume) {
    if (g_hub.volume > 0) audio_play(name, volume * (float) g_hub.volume, 0, 1.0f);
}

// ------------------------------------------------------------------ transport

static int hub_connection_up(Client *client) {
    if (g_hub.brain) return backend_social_is_open(); // the brain's social socket
    if (!client) return 0;
    if (client->offline) return 1;
    pthread_mutex_lock(&client->net_lock);
    const int socket = client->net_socket;
    pthread_mutex_unlock(&client->net_lock);
    return socket >= 0;
}

static int hub_send_chunk(void *user, const NetPacketType type, const void *payload, const uint16_t length) {
    client_net_send(user, type, payload, length);
    return 1;
}

// io.send(label, ...): `frame` is a cJSON array, consumed.
static void hub_brain_api(const cJSON *frame);

static void hub_send(cJSON *frame) {
    if (g_hub.brain && frame) {
        // brain: socket frames go out as they are; the "api" stand-in is a real HTTP GET
        const char *label = cJSON_GetStringValue(cJSON_GetArrayItem(frame, 0));
        if (label && !strcmp(label, "api")) hub_brain_api(frame);
        else backend_social_send(frame);
        cJSON_Delete(frame);
        return;
    }
    char *json = frame ? cJSON_PrintUnformatted(frame) : NULL;
    cJSON_Delete(frame);
    if (!json) return;
    if (hub_connection_up(g_hub.client)) {
        const uint32_t id = ++g_hub.next_id;
        social_send_message(NET_PACKET_SOCIAL_REQUEST, id, json, strlen(json), hub_send_chunk, g_hub.client);
    }
    cJSON_free(json);
}

static cJSON *str_or_null(const char *s) {
    return s && *s ? cJSON_CreateString(s) : cJSON_CreateNull();
}

// ------------------------------------------------------------------ popups

// resetPops()
static void reset_pops(void) {
    static const char *const pops[] = {"popupContent", "filterPop", "sortPop", "tradePop", "settingsPop", "followPop",
                                       "offerPop", "giftPop", "flagPop", "verifyPop", "reportPop", "loginPop",
                                       "confListPop", "confQSPop", "confOfferPop", "confSubmitPop", "publishPop"};
    set_display("popupHolder", "block");
    for (size_t i = 0; i < sizeof(pops) / sizeof(pops[0]); i++) set_display(pops[i], "none");
}

// showPopup() with no item: closes the popup (popupBack's onclick).
static void show_popup_none(void) {
    reset_pops();
    set_display("popupHolder", "none");
    set_style("popupContent", "width", "618px");
    set_display("popupContent", "block");
}

// io_po(e, a, t)
static void io_po(const int success, const char *text) {
    reset_pops();
    set_display("popupContent", "block");
    set_style("popupContent", "width", "300px");
    StrBuf sb = {0};
    if (success) {
        sb_append(&sb, "<div style='width:100%;text-align:center'><div><i class='material-icons' style='font-size:80px;color:#3be354'>done</i></div><div style='font-size:20px;color:rgba(255,255,255,0.5)'>");
        sb_append(&sb, text ? text : "Success!");
    } else {
        sb_append(&sb, "<div style='width:100%;text-align:center'><div><i class='material-icons' style='font-size:80px;color:#e33b3b'>clear</i></div><div style='font-size:20px;color:rgba(255,255,255,0.5)'>");
        sb_append(&sb, text && *text ? text : STR_MARKET_FAILED);
    }
    sb_append(&sb, "</div></div>");
    char *html = sb_finish(&sb);
    set_html("popupContent", html);
    free(html);
}

// ta(text): the waiting popup
static void io_wait(const char *text) {
    reset_pops();
    set_style("popupContent", "width", "300px");
    set_display("popupContent", "block");
    StrBuf sb = {0};
    sb_append(&sb, "<div style='width:100%;text-align:center'><div class='lds-ring'><div></div><div></div><div></div><div></div></div><div style='font-size:20px;color:rgba(255,255,255,0.5)'>");
    sb_append(&sb, text);
    sb_append(&sb, "</div></div>");
    char *html = sb_finish(&sb);
    set_html("popupContent", html);
    free(html);
}

// hideAll(e)
static void hide_all(const int keep_loading) {
    static const char *const holders[] = {"profileHolder", "searchHolder", "mapsHolder", "modsHolder", "mapHolder",
                                          "modHolder", "leaderHolder", "virusHolder", "clanwarsHolder", "feedHolder",
                                          "adBox", "tourneyHolder", "marketHolder", "itemsalesHolder", "clanHolder",
                                          "popupHolder", "averageCost", "totalSold", "chatBox"};
    if (!keep_loading) set_display("loadMessage", "block");
    for (size_t i = 0; i < sizeof(holders) / sizeof(holders[0]); i++) set_display(holders[i], "none");
    const HtmlElem tip = el("chartjs-tooltip");
    if (tip) html_dom_remove(tip);
}

// io_1(text): the error screen (DISCONNECTED)
static void io_error(const char *text) {
    if (g_hub.error_shown) return;
    g_hub.error_shown = 1;
    hide_all(0);
    show_popup_none();
    set_display("chatHolder", "none");
    set_display("navBar", "none");
    set_display("loadMessage", "none");
    set_display("errorDisplay", "flex");
    const HtmlElem title = el("errorTitle");
    if (title) html_dom_set_text(title, text);
}

// ------------------------------------------------------------------ settings

static void storage_key(char *out, const size_t n, const char *key) {
    snprintf(out, n, "krs_setngss_%s", key);
}

// onResize(): the 1700x900 design space times B (scaleUI), when uiScaling is on.
static void on_resize(void) {
    const HtmlElem body = html_dom_body();
    if (!body) return;
    if (!g_hub.ui_scaling) {
        html_dom_set_style(body, "transform", "");
        html_dom_set_style(body, "width", "");
        html_dom_set_style(body, "height", "");
        return;
    }
    float w = 0, h = 0;
    html_dom_viewport(&w, &h, NULL);
    if (w <= 0 || h <= 0) return;
    const double t = g_hub.ui_scale_b * 1700.0;
    const double n = g_hub.ui_scale_b * 900.0;
    const double i = w / t;
    const double r = h / n;
    char transform[48], width[32], height[32];
    // v3.1.1 rounds with toFixed(3): scale(0.579) at 1280 wide, body 2210px -> 1279.6 px,
    // which the hub references are captured with (full precision shifts text by 1 px).
    if (r < i) {
        snprintf(transform, sizeof(transform), "scale(%.3f)", r);
        snprintf(width, sizeof(width), "%.3fpx", w / r);
        snprintf(height, sizeof(height), "%.0fpx", n);
    } else {
        snprintf(transform, sizeof(transform), "scale(%.3f)", i);
        snprintf(width, sizeof(width), "%.0fpx", t);
        snprintf(height, sizeof(height), "%.3fpx", h / i);
    }
    html_dom_set_style(body, "transform", transform);
    html_dom_set_style(body, "width", width);
    html_dom_set_style(body, "height", height);
}

static void request_page(void);

// re[key].set(val)
static void setting_apply(const int index) {
    const double v = g_settings[index].val;
    switch (index) {
        case HS_REGION:
            // changeRegion(e)
            g_hub.region = v >= 0 && v < (double) (sizeof(k_social_regions) / sizeof(k_social_regions[0])) ? (int) v : 0;
            if (!strcmp(g_hub.v, "leaders") && g_hub.connected) {
                hide_all(0);
                cJSON *f = cJSON_CreateArray();
                cJSON_AddItemToArray(f, cJSON_CreateString("r"));
                cJSON_AddItemToArray(f, cJSON_CreateString(g_hub.v));
                cJSON_AddItemToArray(f, str_or_null(g_hub.b));
                cJSON_AddItemToArray(f, cJSON_CreateNull()); // K
                cJSON_AddItemToArray(f, cJSON_CreateNull()); // q
                cJSON_AddItemToArray(f, cJSON_CreateNull());
                cJSON_AddItemToArray(f, cJSON_CreateNumber(g_hub.region));
                hub_send(f);
            }
            break;
        case HS_SCALE_UI: g_hub.ui_scale_b = 1.0 - v + 1.0; on_resize(); break;
        case HS_UI_SCALING: g_hub.ui_scaling = v != 0; on_resize(); break;
        case HS_RARITY_ANIM: g_hub.rarity_anim = v == 0; break;
        case HS_SHOW_PFP: g_hub.show_pfp = v != 0; break;
        case HS_SOUND: g_hub.volume = v; break;
        default: break;
    }
}

// The value parsing setSetting and the load loop share: "true"/"false", "undefined" ->
// default, min/max clamp. Returns the numeric value (booleans as 0/1).
static double setting_parse(const HubSetting *s, const char *text) {
    if (!text) return s->val;
    if (!strcmp(text, "false")) return 0;
    if (!strcmp(text, "true")) return 1;
    if (!strcmp(text, "undefined")) return s->def;
    double v = atof(text);
    if (s->min != 0 || s->max != 0) v = fmin(s->max, fmax(s->min, v));
    return v;
}

static void setting_value_text(const HubSetting *s, char *out, const size_t n) {
    if (s->kind == SET_SWITCH || s->kind == SET_HIDDEN) snprintf(out, n, "%s", s->val != 0 ? "true" : "false");
    else js_num(out, n, s->val);
}

static int setting_index(const char *key) {
    for (int i = 0; i < HS_COUNT; i++) if (key && !strcmp(g_settings[i].key, key)) return i;
    return -1;
}

// setSetting(key, value)
static void set_setting(const char *key, const char *value) {
    const int i = setting_index(key);
    if (i < 0) return;
    HubSetting *s = &g_settings[i];
    char cleaned[64];
    size_t o = 0;
    for (const char *p = value ? value : ""; *p && o + 1 < sizeof(cleaned); p++) {
        if (*p != '<' && *p != '>') cleaned[o++] = *p;
    }
    cleaned[o] = 0;
    s->val = value ? setting_parse(s, cleaned) : s->val;
    char text[64], id[96];
    setting_value_text(s, text, sizeof(text));
    snprintf(id, sizeof(id), "slid_%s", key);
    HtmlElem e = el(id);
    if (e) html_dom_set_value(e, text);
    snprintf(id, sizeof(id), "slid_input_%s", key);
    e = el(id);
    if (e) html_dom_set_value(e, text);
    setting_apply(i);
    char skey[64];
    storage_key(skey, sizeof(skey), key);
    html_storage_set(skey, text);
}

static void settings_load(void) {
    for (int i = 0; i < HS_COUNT; i++) {
        HubSetting *s = &g_settings[i];
        s->val = s->def;
        char skey[64], saved[64];
        storage_key(skey, sizeof(skey), s->key);
        if (html_storage_get(skey, saved, sizeof(saved)) >= 0) s->val = setting_parse(s, saved);
        setting_apply(i);
    }
}

// openSettings(): initSocialUI._html.settings.input.* markup, byte for byte.
static void append_setting_html(StrBuf *sb, const HubSetting *s) {
    char val[64], min[32], max[32], step[32];
    setting_value_text(s, val, sizeof(val));
    js_num(min, sizeof(min), s->min);
    js_num(max, sizeof(max), s->max);
    js_num(step, sizeof(step), s->step);
    switch (s->kind) {
        case SET_SELECT:
            sb_appendf(sb, "<select onchange='setSetting(\"%s\", this.value)' class='inputGrey2'>\n                       ", s->key);
            for (size_t i = 0; i < sizeof(k_social_regions) / sizeof(k_social_regions[0]); i++) {
                // Object.keys(l).map(...) concatenated to a string: joined with ","
                sb_appendf(sb, "%s<option value='%zu' %s>%s</option>", i ? "," : "", i,
                           (int) s->val == (int) i ? "selected" : "", k_social_regions[i]);
            }
            sb_append(sb, "</select>");
            break;
        case SET_SWITCH:
            sb_appendf(sb, "<label class='switch' style='margin-left:10px'><input type='checkbox' onclick='setSetting(\"%s\", this.checked)'\n                       %s><span class='slider'></span></label>",
                       s->key, s->val != 0 ? "checked" : "");
            break;
        case SET_SLIDER: {
            const char *style = s == &g_settings[HS_SCALE_UI] ? "width: 180px;" : "";
            if (s == &g_settings[HS_SCALE_UI]) {
                // switch("uiScaling", re.uiScaling) + slider("scaleUI", this, "width: 180px;")
                append_setting_html(sb, &(HubSetting) {"uiScaling", "", SET_SWITCH, 0, 0, 0, 0, 0, g_settings[HS_UI_SCALING].val});
            }
            sb_appendf(sb, "<input type='number' class='sliderVal' id='slid_input_%s' min='%s' max='%s'\n                       value='%s' onkeypress='return delayExecute(\"%s\", this, undefined)' style='margin-right:0px;border-width:0px'/>\n                       <div class='slidecontainer' style='%s'><input type='range' id='slid_%s' min='%s' max='%s' step='%s'\n                       value='%s' class='sliderM' oninput='setSetting(\"%s\", this.value)'></div>",
                       s->key, min, max, val, s->key, style, s->key, min, max, step, val, s->key);
            break;
        }
        case SET_HIDDEN:
            sb_append(sb, "undefined"); // re.uiScaling.html() returns undefined
            break;
    }
}

static void open_settings(void) {
    reset_pops();
    set_display("settingsPop", "block");
    StrBuf sb = {0};
    sb_append(&sb, "<div id='settHolder'>");
    for (int i = 0; i < HS_COUNT; i++) {
        const HubSetting *s = &g_settings[i];
        sb_append(&sb, "<div class='settName' title=''");
        if (s->hide >= 0) sb_appendf(&sb, "id='%s_div' style='display:%s'", s->key, s->hide ? "none" : "block");
        sb_appendf(&sb, ">%s ", s->name);
        append_setting_html(&sb, s);
        sb_append(&sb, "</div>");
    }
    sb_append(&sb, "</div>");
    char *html = sb_finish(&sb);
    set_html("settingsPop", html);
    free(html);
}

// ------------------------------------------------------------------ side boxes (Se)

static void side_boxes(void) {
    set_display("adBox", "block");
    int picks[WHO_TO_FOLLOW_COUNT];
    int count = (int) WHO_TO_FOLLOW_COUNT;
    for (int i = 0; i < count; i++) picks[i] = i;
    StrBuf sb = {0};
    for (int n = 0; n < 3; ++n) {
        const int e = rand_int(0, count - 1);
        const HubFollow *f = &k_who_to_follow[picks[e]];
        // src: user-assets.krunker.io/p<id>/profile.png has no picture for these accounts, so
        // the real page always ends on its onerror fallback, favicon.png (the native engine
        // runs no onerror): the fallback is the src here.
        (void) f->id;
        sb_appendf(&sb, "<div class='sideBarItem'><img class='replyPfp pfpOutlW' onerror='this.onerror=null;this.src=\"./img/favicon.png\";'src='./img/favicon.png' /><div class='reqFollH'><a class='menuLink blueLink reqFollN' href='/social.html?p=profile&q=%s'>%s</a><div style='color:rgba(255,255,255,0.4);font-size:12px'>%s</div></div><div class='reqFollB' onclick='instaFollo(&quot;%s&quot;,this)'>Follow</div></div>",
                   f->name, f->alias ? f->alias : f->name, f->tag, f->name);
        memmove(&picks[e], &picks[e + 1], (size_t) (count - e - 1) * sizeof(int)); // t.splice(e, 1)
        count--;
    }
    char *html = sb_finish(&sb);
    set_html("whoToFolL", html);
    free(html);
    set_style("leftYTBox", "display", "");
    // recYTVid.src stays empty: /api/streams has no live stream (n null), the box hides
    set_style("leftYTBox", "display", "none");
}

// ------------------------------------------------------------------ maps / mods lists (windows[0], windows[1])

static void api_request(const char *path, const int index, const int pos, const char *search_type, const char *val) {
    cJSON *params = cJSON_CreateObject();
    if (search_type) {
        cJSON_AddStringToObject(params, "type", search_type);
        cJSON_AddStringToObject(params, "val", val ? val : "");
    } else {
        cJSON_AddNumberToObject(params, "index", index);
        cJSON_AddNumberToObject(params, "pos", pos);
    }
    cJSON *f = cJSON_CreateArray();
    cJSON_AddItemToArray(f, cJSON_CreateString("api"));
    cJSON_AddItemToArray(f, cJSON_CreateString(path));
    cJSON_AddItemToArray(f, params);
    hub_send(f);
}

// La(rows, tab): map cards
static void append_map_cards(StrBuf *sb, const cJSON *rows, const int tab) {
    if (!cJSON_IsArray(rows) || !cJSON_GetArraySize(rows)) {
        sb_append(sb, "<span style='color:rgba(0,0,0,0.4);'>" STR_MAPS_NONE "</span>");
    }
    const cJSON *r;
    cJSON_ArrayForEach(r, rows) {
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(r, "map_name"));
        const char *creator = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(r, "creatorname"));
        const char *mod_url = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(r, "mod_url"));
        const char *mod_id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(r, "mod_id"));
        const char *image = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(r, "map_image"));
        const double map_id = cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(r, "map_id"));
        const double fund = cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(r, "fund"));
        const double votes = cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(r, "map_votes"));
        const double plays = cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(r, "map_pl"));
        const double fav = cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(r, "fav"));
        if (!name || !creator) continue;
        char id[32], fund_s[32], votes_s[32], plays_s[32];
        js_num(id, sizeof(id), map_id);
        comma_number(fund_s, sizeof(fund_s), isnan(fund) ? 0 : fund);
        comma_number(votes_s, sizeof(votes_s), isnan(votes) ? 0 : votes);
        comma_number(plays_s, sizeof(plays_s), isnan(plays) ? 0 : plays);
        const int faved = (!isnan(fav) && fav != 0) || tab == 3;
        // no map_date from KrunkNative servers: never NEW/UPDATED (age -1)
        sb_append(sb, "<div class='mapListItem'><div class='creationActions'></div><div class='mapListThumbHolder'><img draggable='false' class='mapListThumb' loading='lazy' src='");
        sb_append(sb, image && *image ? image : "./img/noimg.png");
        sb_append(sb, "' onerror='this.onerror=null;this.src=\"./img/noimg.png\";' onclick='openMap(&quot;");
        sb_append_js_escape(sb, name);
        sb_appendf(sb, "&quot;)'/><span class='material-icons favIcon%s' onclick='favoriteM(0, this, %s);'>grade</span>", faved ? " favd" : "", id);
        sb_appendf(sb, "<div class='mapsVotes' style='top: 51px;right: 9px;' title='KR Bank'>%s<span style='margin-left: 8px;color:#FBC02D;font-size: 12px;'>KR</span></div>", fund_s);
        sb_appendf(sb, "<div class='mapsVotes' style='top: 75px' title='Total Likes'>%s<span class='material-icons' style='margin-left: 8px;color:rgba(255, 255, 255, 0.5);vertical-align: middle;margin-top: -3px;font-size: 18px;'>thumb_up</span></div>", votes_s);
        sb_appendf(sb, "<div class='mapsVotes' title='Total Plays'>%s<span class='material-icons' style='margin-left: 8px;color:rgba(255, 255, 255, 0.5);vertical-align: middle;margin-top: -3px;font-size: 18px;'>videogame_asset</span></div>", plays_s);
        sb_append(sb, "<div class='mapsVotes' title='Live Players' id='pcnt_");
        sb_append_js_escape(sb, name);
        sb_append(sb, "' style='left: 10px;right:unset'><span class='material-icons' style='margin-right: 8px;color:rgba(255, 255, 255, 0.5);vertical-align: middle;margin-top: -3px;font-size: 18px;'>person</span>0</div></div>");
        sb_append(sb, "<div style='margin-top:1px'><a href='javascript:;' onclick='openMap(&quot;");
        sb_append_js_escape(sb, name);
        sb_appendf(sb, "&quot;,&quot;%s&quot;,&quot;%s&quot;)'/>", mod_url ? mod_url : "", mod_id ? mod_id : "");
        sb_append(sb, name);
        sb_append(sb, "</a></div><div style='margin-top:1px;display: block;color: rgba(0,0,0,0.2);font-size: 15px;text-align: left;'>by <span class='click' onclick='openProfile(&quot;");
        sb_append(sb, creator);
        sb_append(sb, "&quot;)' style='color: rgba(0, 0, 0, 0.6);'>");
        sb_append(sb, creator);
        sb_append(sb, "</span></div></div>");
    }
}

static HubListData *list_data(HubList *list, const int tab) {
    if (tab < 0 || tab >= 8) return NULL;
    return &list->data[tab];
}

static void list_reset(HubListData *d) {
    sb_free(&d->dat);
    memset(d, 0, sizeof(*d));
}

// windows[n].gen()
static char *list_gen(HubList *list, const int is_mods) {
    const int tab = list->tab_index;
    if (tab < 0 || tab >= 6 || k_list_tabs[tab].search) return strdup("");
    HubListData *d = list_data(list, tab);
    StrBuf sb = {0};
    if (d->used) {
        if (d->has_dat) {
            sb_append(&sb, d->dat.data ? d->dat.data : "");
            if (d->waiting) sb_append(&sb, "<div class='loadMoreW'>Loading...</div>");
            else if (!k_list_tabs[tab].send_id && !k_list_tabs[tab].no_pages)
                sb_appendf(&sb, "<div class='loadMoreD' onclick='windows[%d].loadMore(this)'>Load More</div>", is_mods);
        } else {
            sb_append(&sb, "<div class='loadMoreW'>Loading...</div>");
        }
    } else {
        sb_append(&sb, "<div class='loadMoreW'>Loading...</div>");
        d->used = 1;
        d->load_ind = 0;
        api_request(is_mods ? "/mods" : "/maps", tab, 0, NULL, NULL);
    }
    return sb_finish(&sb);
}

static void list_update(const int is_mods) {
    char *html = list_gen(is_mods ? &g_hub.mods : &g_hub.maps, is_mods);
    set_html(is_mods ? "modList" : "mapList", html);
    free(html);
    // windows[0].updatePlayers(): live player counts come from custom servers in the
    // matchmaker's game list; KrunkNative servers list none, every count stays 0.
}

// windows[n].switchTab(e, a)
static void list_switch_tab(const int is_mods, const int tab, const int no_update) {
    HubList *list = is_mods ? &g_hub.mods : &g_hub.maps;
    if (list->tab_index != tab) {
        list->tab_index = tab;
        if (!no_update) list_update(is_mods);
    }
    HtmlElem tabs[16];
    const size_t n = html_dom_query_all(0, is_mods ? "a[id^=mTab_]" : "a[id^=gTab_]", tabs, 16);
    char want[16];
    snprintf(want, sizeof(want), "%cTab_%d", is_mods ? 'm' : 'g', tab);
    for (size_t i = 0; i < n; i++) {
        char id[16] = "";
        html_dom_get_attr(tabs[i], "id", id, sizeof(id));
        html_dom_set_class(tabs[i], "tabSel", !strcmp(id, want));
    }
}

// windows[0].mapsLoaded / windows[1].modsLoaded (both write the "none" text into
// #mapList, as Krunker's modsLoaded does)
static void list_loaded(const int is_mods, const cJSON *rows, const int tab, const cJSON *error) {
    HubList *list = is_mods ? &g_hub.mods : &g_hub.maps;
    HubListData *d = list_data(list, tab);
    if (!d || !el("mapList")) return;
    d->waiting = 0;
    if (cJSON_IsArray(rows) && cJSON_GetArraySize(rows)) {
        if (cJSON_IsString(error)) {
            if (!d->has_dat) {
                set_html("mapList", error->valuestring);
                list_reset(d);
            }
            return;
        }
        StrBuf sb = {0};
        if (is_mods) {
            sb_append(&sb, ""); // KrunkNative servers list no mods
        } else if (tab == 1) {
            // UTILS.shuffleArray(e) for Featured
            cJSON *copy = cJSON_Duplicate(rows, 1);
            for (int m = cJSON_GetArraySize(copy); m-- > 0;) {
                const int p = rand_int(0, m);
                if (p == m) continue;
                cJSON *a = cJSON_DetachItemFromArray(copy, m);
                cJSON *b = cJSON_DetachItemFromArray(copy, p);
                cJSON_InsertItemInArray(copy, p, a);
                cJSON_InsertItemInArray(copy, m, b);
            }
            append_map_cards(&sb, copy, tab);
            cJSON_Delete(copy);
        } else {
            append_map_cards(&sb, rows, tab);
        }
        char *cards = sb_finish(&sb);
        sb_append(&d->dat, cards);
        d->has_dat = 1;
        free(cards);
        if (list->tab_index == tab) list_update(is_mods);
    } else if (!d->has_dat) {
        set_html("mapList", is_mods ? STR_WIN_MODS_NONE : STR_WIN_MAPS_NONE);
        list_reset(d);
    }
}

// windows[n].loadMore(el)
static void list_load_more(const int is_mods, const HtmlElem button) {
    HubList *list = is_mods ? &g_hub.mods : &g_hub.maps;
    HubListData *d = list_data(list, list->tab_index);
    if (!d || d->waiting) return;
    if (button) {
        html_dom_set_attr(button, "class", "loadMoreW");
        html_dom_set_html(button, "Loading...");
    }
    d->waiting = 1;
    d->load_ind += 1;
    api_request(is_mods ? "/mods" : "/maps", list->tab_index, d->load_ind, NULL, NULL);
}

// windows[n].searchResp(rows) (za)
static void list_search_resp(const int is_mods, const cJSON *rows) {
    list_switch_tab(is_mods, is_mods ? 5 : 6, 1);
    StrBuf sb = {0};
    if (cJSON_IsArray(rows) && cJSON_GetArraySize(rows)) {
        if (!is_mods) append_map_cards(&sb, rows, -1);
    } else {
        sb_append(&sb, is_mods ? STR_WIN_MODS_NONE : STR_WIN_MAPS_NONE);
    }
    char *html = sb_finish(&sb);
    set_html(is_mods ? "modList" : "mapList", html);
    free(html);
}


// ------------------------------------------------------------------ clan wars (io_0 "clanwars" + initSocialUI.clanwars)

typedef struct {
    const char *name;
    int x, y;
    int reward_index;        // store skin index of the region's "Clan Wars" reward
    const char *reward_png;  // its preview (textures/previews/...)
} HubClanRegion;

// config.clanRegions + store.skins.find(limT == "Clan Wars" && region == name)
static const HubClanRegion k_clan_regions[] = {
    {"North America", 140, 180, 1893, "textures/previews/cosmetics/6_9.png"},
    {"South America", 230, 285, 1899, "textures/previews/melee/melee_18.png"},
    {"Western Europe", 380, 170, 1898, "textures/previews/cosmetics/2_66.png"},
    {"Africa", 380, 250, 1896, "textures/previews/cosmetics/1_128.png"},
    {"Middle East", 430, 210, 1894, "textures/previews/cosmetics/6_8.png"},
    {"Asia", 530, 210, 1892, "textures/previews/cosmetics/6_10.png"},
    {"Oceania", 600, 310, 1897, "textures/previews/cosmetics/2_65.png"},
    {"Eastern Europe", 476, 165, 1895, "textures/previews/cosmetics/1_129.png"},
};
#define CLAN_REGION_COUNT ((int) (sizeof(k_clan_regions) / sizeof(k_clan_regions[0])))

static HtmlElem g_dropper_elem; // h.clanwars.droppers.elem: the bouncing arrow

static void clanwars_region_deselect(void) {
    if (g_dropper_elem) {
        html_dom_set_style(g_dropper_elem, "transform", "");
        html_dom_set_class(g_dropper_elem, "bounce", 0);
    }
}

// GUI.clanwars.buttons.leader: no clans offline
static void clanwars_show_leaders(void) {
    clanwars_region_deselect();
    set_html("clanwarsInfo", "<div class='regionCard'><div style='margin-bottom:10px;'><span class='regionTitle'>Global Leaders</span></div></div>");
}

static void clanwars_show_soldiers(void) {
    clanwars_region_deselect();
    set_html("clanwarsInfo", "<div class='regionCard'><div style='margin-bottom:10px;'><span class='regionTitle'>Top Soldiers</span></div></div>");
}

// GUI.clanwars.region.show(name, region): reward + occupying clans (none offline)
static void clanwars_show_region(const int index) {
    if (index < 0 || index >= CLAN_REGION_COUNT) return;
    const HubClanRegion *r = &k_clan_regions[index];
    StrBuf sb = {0};
    sb_appendf(&sb, "<div class='regionCard'><div style='margin-bottom:10px;'><span class='regionTitle'>%s</span><div style='float:right;color:rgba(0,0,0,0.3)'>Reward: <div class='rewItemIcon' style='background-image:url(&quot;%s&quot;)' onclick='openSales(%d)'></div><span class='regionKR'> <span style='color:rgba(0,0,0,0.3)'>+</span> 0<span style='color:rgba(0,0,0,0.6)'> KR</span></span></div></div>",
               r->name, r->reward_png, r->reward_index);
    sb_append(&sb, "<div class='rEmpty'>No Clans currently occupying this Region</div></div>");
    char *html = sb_finish(&sb);
    set_html("clanwarsInfo", html);
    free(html);
}

static void clanwars_show(void) {
    // t = {}: no regions, clans or war timer from the server -> 7 days left, all zero
    StrBuf sb = {0};
    sb_append(&sb, "<div class='overlay' id='sea-mask'></div><div class='overlay' id='sea-pseudo'></div><div class='overlay' id='sea-map'></div><div id='clanwarsStats'><div class='cwStatItem' style='font-size:10px;color:var(--yellow);margin-bottom:3px;'>Global Stats</div><div class='cwStatItem' style='font-size:10px;text-align:right;color:var(--yellow)'></div><div class='cwStatItem'>Clans</div><div class='cwStatItem' style='text-align:right'>0</div><div class='cwStatItem'>Soldiers</div><div class='cwStatItem' style='text-align:right'>0</div><div class='cwStatItem'>K.I.A</div><div class='cwStatItem' style='text-align:right'>0</div></div>");
    // help.show() + buttons.show() (GUI.action keys flattened to one string)
    sb_append(&sb, "<div id='clanwarsButtons'><div id=\"clanwarsLeaderIcon\" style=\"\" onclick=\"GUI.action('clanwars.buttons.leader', this)\"><div><i class='material-icons' style='color:white'>emoji_events</i></div></div>\n<div id=\"clanwarsPlayerKillsIcon\" style=\"\" onclick=\"GUI.action('clanwars.buttons.kills', this)\"><img src='./img/soldier_0.png' width='20' height='20' style='image-rendering:pixelated;margin:auto;position:absolute;left:0;right:0;top:0;bottom:0'></div></div>");
    sb_append(&sb, "<div id='clanwarsHelp'><div id=\"clanwarsHelpIcon\" style=\"\" onclick=\"GUI.action('clanwars.help.icon', this)\"><div>?</div></div>\n<div id=\"clanwarsHelpVig\" style=\"display:none\" onclick=\"GUI.action('clanwars.help.vig', this)\"></div><div id='clanwarsHelpWin' style='display:none'><div class='cwHelpHed'>Welcome to Clan Wars</div><div class='cwHelpBod'>Clan Leaders can assign up to 30 Clan Members to different Regions. Once a player is assigned to a Region,\n\t\t\t\t\t\tany kills they get in a pub match will count toward the clans overall kills in that Region.\n\t\t\t\t\t\t<div style='height:16px'></div>If a player runs out of lives (by dying in Pub Matches),\n\t\t\t\t\t\ttheir contract will end and the clan leader has to replace\n\t\t\t\t\t\tthem with another player until the next Clan War.<div style='height:16px'></div>At the end of the\n\t\t\t\t\t\tWar Period, the Clan with the most kills in a specific Region will win all the KR available in that Region.\n\t\t\t\t\t\tThe Clans with the most Kills globally, will also receive special clan tag colors:\n\t\t\t\t\t\t<div class='clanRewH' style='margin-top:20px'># 1 Clan <span style='float:right;' class='rainbowText'>[CLAN]</span></div>\n\t\t\t\t\t\t<div class='clanRewH' style='margin-top:20px'># 2-5 Clan <span style='float:right;color:#eb3437'>[CLAN]</span></div>\n\t\t\t\t\t\t<div class='clanRewH' style='margin-top:20px'># 6-15 Clan <span style='float:right;color:#FBC02D'>[CLAN]</span></div>\n\t\t\t\t\t\t<div class='clanRewH' style='margin-top:20px'># 16-25 Clan <span style='float:right;color:#e834eb'>[CLAN]</span></div>\n\t\t\t\t\t\t<div class='clanRewH' style='margin-top:20px'># 26-50 Clan <span style='float:right;color:#349ceb'>[CLAN]</span></div>\n\t\t\t\t\t\t<div style='height:20px'></div>Remember to have fun and to follow the\n\t\t\t\t\t\t<a style='color:#00a6ff' target='_blank' href='./docs/clan.txt'>Rules</a></div></div></div>");
    sb_append(&sb, "<div id='dropperHolder'>");
    for (int i = 0; i < CLAN_REGION_COUNT; i++) {
        if (i) sb_append(&sb, "\n");
        sb_appendf(&sb, "<div class=\"dropper\" style=\"transform: translate(%dpx, %dpx)\">\n\t\t\t\t\t\t\t<div class=\"dropperPoint\" onclick=\"GUI.action('clanwars.droppers.%d', this)\"></div>\n\t\t\t\t\t\t\t<div class=\"dropperArrow\" onclick=\"GUI.action('clanwars.droppers.%d', this)\"><div class='dropperArrowBod'>\n\t\t\t\t\t\t\t<div class=\"dropperText\" style=\"color:#fff;height:25px;\"></div>\n\t\t\t\t\t\t\t<i class='material-icons' style='color:white;font-size:30px;transform:translateY(-4px)'>tour</i>\n\t\t\t\t\t\t\t</div></div></div>",
                   k_clan_regions[i].x, k_clan_regions[i].y, i, i);
    }
    sb_append(&sb, "</div><svg><filter id='turbulence' x='0' y='0' width='100%' height='100%'><feTurbulence id='sea-filter' numOctaves='1' seed='2' baseFrequency='0.004'></feTurbulence><feDisplacementMap scale='20' in='SourceGraphic'></feDisplacementMap></filter></svg>");
    char *html = sb_finish(&sb);
    set_html("sea", html);
    free(html);
    // getRoundTim(604800000): the war ends in 7 days
    set_html("clanwarsProg", "<div class='xpBar' style='width: calc(100% - 8px);'><div class='xpBarB' style='background-color:var(--yellow);width:100%'></div><span class='xpBarV'>Ends in 7 Days 0 Hour</span></div>");
    g_dropper_elem = 0;
    clanwars_show_leaders(); // clanwarsButtons.children[0].onclick()
}

// GUI.action(key, this)
static int h_gui_action(void *user, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) user;
    const char *key = argc > 0 && args[0].kind == HTML_ARG_STR ? args[0].str : NULL;
    if (!key) return 1;
    if (!strcmp(key, "clanwars.buttons.leader")) {
        clanwars_show_leaders();
    } else if (!strcmp(key, "clanwars.buttons.kills")) {
        clanwars_show_soldiers();
    } else if (!strcmp(key, "clanwars.help.icon") || !strcmp(key, "clanwars.help.vig")) {
        char display[16] = "";
        const HtmlElem win = el("clanwarsHelpWin");
        const int shown = win && html_dom_get_style(win, "display", display, sizeof(display)) >= 0 ? strcmp(display, "none") != 0 : 1;
        set_style("clanwarsHelpWin", "display", shown ? "none" : "");
        set_style("clanwarsHelpVig", "display", shown ? "none" : "");
    } else if (!strncmp(key, "clanwars.droppers.", 18)) {
        // type "dropper": the clicked dropper's arrow bounces, then the region card
        const HtmlElem target = ev ? ev->target : 0;
        const HtmlElem dropper = target ? html_dom_closest(target, ".dropper") : 0;
        HtmlElem arrow = dropper ? html_dom_first_child(dropper) : 0;
        if (arrow) arrow = html_dom_next(arrow); // children[1]: .dropperArrow
        clanwars_region_deselect();
        if (arrow) {
            html_dom_set_style(arrow, "transform", "scale(1.0) translateY(-41px)");
            html_dom_set_class(arrow, "bounce", 1);
            g_dropper_elem = arrow;
        }
        clanwars_show_region(atoi(key + 18));
    }
    return 1;
}

// ------------------------------------------------------------------ pages

static void send_page_request(void) {
    // Xa(v, b, K, q, k, V, x): io.send("r", ...); the market adds the captcha token (true)
    cJSON *f = cJSON_CreateArray();
    cJSON_AddItemToArray(f, cJSON_CreateString("r"));
    cJSON_AddItemToArray(f, cJSON_CreateString(g_hub.v));
    cJSON_AddItemToArray(f, str_or_null(g_hub.b));
    const AccountInfo *acc = g_hub.logged_in ? account_get() : NULL;
    cJSON_AddItemToArray(f, acc ? cJSON_CreateNumber(acc->id) : cJSON_CreateNull()); // K: krunker_id
    cJSON_AddItemToArray(f, acc ? cJSON_CreateString(acc->name) : cJSON_CreateNull()); // q: krunker_username
    cJSON_AddItemToArray(f, str_or_null(g_hub.ja_k));
    cJSON_AddItemToArray(f, cJSON_CreateNumber(g_hub.region));
    cJSON_AddItemToArray(f, str_or_null(g_hub.x));
    if (!strcmp(g_hub.v, "market")) cJSON_AddItemToArray(f, cJSON_CreateTrue());
    hub_send(f);
}

static void request_page(void) {
    send_page_request();
}

static void io_0(const char *e, const char *a, const cJSON *t, const cJSON *extra);

// updateWindow(e, a, t, n, r, s)
static void update_window(const char *e, const char *a, const char *t, const char *n, const char *r, const int s) {
    if (!s) {
        if (r && *r) hub_sound(r, 0.5f);
        // market ambience loops (ambient_4 / ambient_8) are not played
        hide_all(0);
    }
    snprintf(g_hub.v, sizeof(g_hub.v), "%s", e && *e ? e : "feed");
    snprintf(g_hub.k, sizeof(g_hub.k), "%s", t ? t : "");
    snprintf(g_hub.x, sizeof(g_hub.x), "%s", n ? n : "");
    snprintf(g_hub.b, sizeof(g_hub.b), "%s", a ? a : "");
    snprintf(g_hub.ja_k, sizeof(g_hub.ja_k), "%s", g_hub.k);
    if (!g_hub.b[0]) {
        if (!strcmp(g_hub.v, "market") || !strcmp(g_hub.v, "itemsales")) snprintf(g_hub.b, sizeof(g_hub.b), "market");
        else if (!strcmp(g_hub.v, "leaders")) snprintf(g_hub.b, sizeof(g_hub.b), "player_score");
        else if (!strcmp(g_hub.v, "maps")) snprintf(g_hub.b, sizeof(g_hub.b), "votes");
        else if (!strcmp(g_hub.v, "profile") && g_hub.logged_in && account_get()) snprintf(g_hub.b, sizeof(g_hub.b), "%s", account_get()->name); // b = q
    }
    for (size_t i = 0; i < sizeof(k_nav_pages) / sizeof(k_nav_pages[0]); i++) set_style(k_nav_pages[i], "opacity", "1");
    if (!str_in(g_hub.v, k_sub_pages, sizeof(k_sub_pages) / sizeof(k_sub_pages[0]))) set_style(g_hub.v, "opacity", "0.5");
    if (!strcmp(g_hub.v, "market")) {
        char id[96];
        for (size_t i = 0; i < sizeof(k_market_queries) / sizeof(k_market_queries[0]); i++) {
            snprintf(id, sizeof(id), "m_%s", k_market_queries[i]);
            set_style(id, "background-color", "rgba(0, 0, 0, 0.2)");
        }
        snprintf(id, sizeof(id), "m_%s", g_hub.b);
        set_style(id, "background-color", "rgba(0, 0, 0, 0.5)");
    }
    snprintf(g_hub.S, sizeof(g_hub.S), "%s_%s", g_hub.v, g_hub.b[0] ? g_hub.b : "null");
    g_hub.has_s = 1;
    if (!strcmp(g_hub.v, "tourney")) {
        // fetch(assets /json/weekly.json): no asset server offline -> io_0("tourney", null, null)
        io_0("tourney", NULL, NULL, NULL);
    } else if (g_hub.connected && !s) {
        request_page();
    }
}

// ------------------------------------------------------------------ brain data views (social.js io_0 branches)

// colors.getClanCol(tag, rank, fallback)
static const char *clan_col(const char *tag, const int rank, const char *fallback) {
    if (tag && !strcmp(tag, "DEV")) return "#40C4FF";
    if (rank == 1) return "rgb";
    if (rank >= 2 && rank <= 5) return "#eb3437";
    if (rank >= 6 && rank <= 15) return "#FBC02D";
    if (rank >= 16 && rank <= 25) return "#e834eb";
    if (rank >= 26 && rank <= 50) return "#349ceb";
    return fallback;
}

static double jnum(const cJSON *o, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (cJSON_IsNumber(v)) return v->valuedouble;
    if (cJSON_IsBool(v)) return cJSON_IsTrue(v);
    if (cJSON_IsString(v)) return strtod(v->valuestring, NULL);
    return 0.0;
}

static double parse_time_ms(const cJSON *v);
static double now_epoch_ms(void);

static const char *jstr(const cJSON *o, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

// User text the brain serves already HTML-escaped (kn_html: & < > " ', for the v3.1.1 page's
// innerHTML): undo that one level, then escape, so "<b>" shows as typed instead of "&lt;b&gt;".
static void sb_append_served(StrBuf *sb, const char *s) {
    static const struct { const char *ent; char c; } k_ents[] = {{"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&#39;", '\''}};
    char ch[2] = {0, 0};
    while (s && *s) {
        size_t i = 0, len = 1;
        ch[0] = *s;
        if (*s == '&') {
            for (i = 0; i < sizeof(k_ents) / sizeof(k_ents[0]); i++) {
                const size_t n = strlen(k_ents[i].ent);
                if (!strncmp(s, k_ents[i].ent, n)) {
                    ch[0] = k_ents[i].c;
                    len = n;
                    break;
                }
            }
        }
        sb_append_esc(sb, ch);
        s += len;
    }
}

// UTILS.formatNum(n)
static void format_num(char *out, const size_t n, const double v) {
    const int p = v != 0 ? (int) floor(log(fabs(v)) / log(1000.0)) : 0;
    if (p >= 1 && p <= 3) snprintf(out, n, "%.1f%c", v / pow(1000.0, p), "kmb"[p - 1]);
    else js_num(out, n, v);
}

// UTILS.getReadableTime(ms)
static void readable_time(char *out, const size_t n, const double ms) {
    long long s = (long long) floor(ms / 1000.0), m = s / 60, h = m / 60;
    m %= 60;
    const long long d = h / 24;
    h %= 24;
    char dp[24] = "", hp[24] = "";
    if (d) snprintf(dp, sizeof(dp), "%lldd ", d);
    if (h) snprintf(hp, sizeof(hp), "%lldh ", h);
    snprintf(out, n, "%s%s%lldm ", dp, hp, m);
}

// UTILS.getDate("YYYY-MM-DD hh:mm:ss") -> "D Mon YYYY"
static void get_date(char *out, const size_t n, const char *s) {
    static const char *const months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    int y = 0, mo = 0, d = 0;
    if (!s || sscanf(s, "%d-%d-%d", &y, &mo, &d) != 3 || mo < 1 || mo > 12) {
        snprintf(out, n, "None");
        return;
    }
    snprintf(out, n, "%d %s %d", d, months[mo - 1], y);
}

static void append_clan_link(StrBuf *sb, const char *clan, const int rank, const char *fallback) {
    sb_appendf(sb, " <a target='_blank' class='%s' style='color:%s' href='/social.html?p=clan&q=", rank == 1 ? "rainbowText" : "", clan_col(clan, rank, fallback));
    sb_append_esc(sb, clan);
    sb_append(sb, "'>[");
    sb_append_esc(sb, clan);
    sb_append(sb, "]</a>");
}

// config.leaderCal.player_score(score): level icon + level
static void append_level_cell(StrBuf *sb, const double score) {
    int lvl = (int) floor(0.03 * sqrt(score > 0 ? score : 0));
    if (lvl < 1) lvl = 1;
    int icon = (int) (round(lvl / 2.0) * 2) - 1; // j.roundToNearest(2) - 1, clamped to [0, maxLevel - 1]
    if (icon > 101) icon = 101;
    if (icon < 0) icon = 0;
    sb_appendf(sb, "<span class='floatR'><img src='./img/levels/%d.png' class='rnkIcon'>%d</span>", icon, lvl);
}

static void hub_render_profile(const cJSON *t, const cJSON *extra) {
    set_display("profileHolder", "block");
    const char *name = jstr(t, "player_name");
    snprintf(g_hub.profile_name, sizeof(g_hub.profile_name), "%s", name ? name : "");
    g_hub.profile_id = (int) jnum(t, "player_id");
    g_hub.profile_followed = jnum(t, "player_followed");
    g_hub.profile_following = jnum(t, "player_following");
    cJSON *stats = NULL;
    const cJSON *raw_stats = cJSON_GetObjectItemCaseSensitive(t, "player_stats");
    if (cJSON_IsString(raw_stats)) stats = cJSON_Parse(raw_stats->valuestring);
    else if (cJSON_IsObject(raw_stats)) stats = cJSON_Duplicate(raw_stats, 1);
    if (!cJSON_IsObject(stats)) {
        cJSON_Delete(stats);
        stats = cJSON_CreateObject();
    }
    const double score = jnum(t, "player_score");
    int k = (int) floor(0.03 * sqrt(score > 0 ? score : 0));
    if (k < 1) k = 1;
    int chal = (int) jnum(t, "player_chal") + 1;
    if (chal > 26) chal = 26;
    const HtmlElem pic = el("socialPic");
    if (pic) {
        char src[64];
        snprintf(src, sizeof(src), "./textures/classes/icon_%d.png", (int) jnum(stats, "c"));
        html_dom_set_attr(pic, "src", src);
    }
    const char *clan = jstr(t, "player_clan");
    const int clan_rank = (int) jnum(t, "clan_rank");
    const int premium = jnum(t, "player_premium") > 0;
    const char *alias = jstr(t, "player_alias");
    char num[64], num2[64];
    StrBuf sb = {0};
    sb_append(&sb, "<div class='leftTopHolder'>");
    if (jnum(t, "partner_approved") >= 2) sb_append(&sb, "<i class='material-icons' style='margin-right:-2px;font-size:32px;color:#993299;vertical-align:middle;margin-top:-6px;'>verified</i>");
    if (premium) sb_append(&sb, "<i class='material-icons' style='margin-right:-2px;font-size:32px;color:#FBC02D;vertical-align:middle;margin-top:-6px;'>beenhere</i>");
    if (jnum(t, "player_featured")) sb_append(&sb, "<i class='material-icons' style='margin-right:1px;font-size:32px;color:#40C4FF;vertical-align:middle;margin-top:-6px;'>check_circle</i>");
    if (premium && alias && alias[0]) {
        sb_append(&sb, "<span id='nameSwitch' onclick='switchNames()'>");
        sb_append_esc(&sb, g_hub.profile_name);
        sb_append(&sb, "</span><span style='display:none;color:#FBC02D' id='nameSwitch' onclick='switchNames()'>");
        sb_append_esc(&sb, alias);
        sb_append(&sb, "</span>");
    } else {
        sb_append_esc(&sb, g_hub.profile_name);
    }
    if (clan && clan[0]) append_clan_link(&sb, clan, clan_rank, "rgba(0,0,0,0.4)");
    get_date(num, sizeof(num), jstr(t, "player_datenew"));
    sb_appendf(&sb, "</div><div class='leftMidHolder'>Since <span style='color:rgba(0,0,0,0.6)'>%s</span></div><div class='leftBottomHolder'>", num);
    format_num(num, sizeof(num), g_hub.profile_followed);
    format_num(num2, sizeof(num2), g_hub.profile_following);
    sb_appendf(&sb, "<div class='follL' onclick='viewFollowers()'>%s<span class='follCount'>Followers</span></div><div class='follL' style='margin-left:10px;' onclick='viewFollowing()'>%s<span class='follCount'>following</span></div></div>", num, num2);
    sb_append(&sb, "<div class='rightTopHolder'><span class='material-icons' id='flagBtn' onmouseover=\"SOUND.play('tick_0',0.1)\" onclick='flagPopup();'>flag</span><span class='material-icons' id='reprtBtn' onmouseover=\"SOUND.play('tick_0',0.1)\" onclick='reportPopup();'>report</span><span class='material-icons' id='shareBtn' onmouseover=\"SOUND.play('tick_0',0.1)\" onclick='shareProfile();'>share</span></div>");
    // c = follow state [{f}]: the 6th reply argument
    const cJSON *follow = cJSON_GetArrayItem(cJSON_GetArrayItem(extra, 6), 0);
    const int not_following = !follow || !jnum(follow, "f");
    sb_append(&sb, "<div class='rightBottomHolder'><div class='bottomBtn' id='tradeBtn' onmouseover=\"SOUND.play('tick_0',0.1)\" onclick='tradePopup();'>Trade</div><div class='bottomBtn' id='giftBtn' onmouseover=\"SOUND.play('tick_0',0.1)\" onclick='giftPopup();'>Gift KR</div>");
    sb_appendf(&sb, "<div class='bottomBtn %s' id='followBtn' onmouseover=\"SOUND.play('tick_0',0.1)\" onclick='followUser();'>%s</div>", not_following ? "follow" : "unfollow", not_following ? "Follow" : "Unfollow");
    sb_appendf(&sb, "<div class='bottomBtn' id='verifyBtn' onmouseover=\"SOUND.play('tick_0',0.1)\" onclick='verifyPopup();'>%s</div></div>", jnum(t, "player_featured") ? "Unverify" : "Verify");
    char *html = sb_finish(&sb);
    set_html("rightProfile", html);
    free(html);
    const AccountInfo *acc = g_hub.logged_in ? account_get() : NULL;
    if (g_hub.profile_name[0] && acc && strcmp(acc->name, g_hub.profile_name) != 0) set_display("followBtn", "inline-block");
    if (acc && (acc->is_dev || acc->is_mod)) set_display("flagBtn", "inline-block");

    // pTabContent_about: Overall Stats
    const double kills = jnum(t, "player_kills"), deaths = jnum(t, "player_deaths"), games = jnum(t, "player_games_played"), wins = jnum(t, "player_wins");
    const double lo = k > 1 ? pow(k / 0.03, 2) : 0, hi = pow((k + 1) / 0.03, 2);
    sb_append(&sb, "<div class='scrollItemNew'><span class='scrollItemHead'>Overall Stats</span><div class='material-icons toggleView' onclick='toggleView(this)'>expand_less</div><div>");
    js_num(num, sizeof(num), (double) (long long) (score - lo) / (double) (long long) (hi - lo) * 100.0);
    sb_appendf(&sb, "<div class='xpBar'><div class='xpBarB' style='width:%s%%'></div><span class='xpBarV'>%lld / %lld</span></div>", num, (long long) (score - lo), (long long) (hi - lo));
    const double elos[3] = {jnum(t, "player_elo"), jnum(t, "player_elo2"), jnum(t, "player_elo4")};
    static const char *const elo_names[3] = {"Solo", "Duo", "Quad"};
    sb_append(&sb, "<div class='rankBadges'>");
    for (int i = 0; i < 3; i++) {
        int id = (int) floor(elos[i] / 120.0);
        id = (id > 8 ? 8 : id < 1 ? 1 : id) - 1;
        sb_appendf(&sb, "<div class='eloHolder'><img src='./img/ranks/icon_%d.png' class='rnkIconELO'/><div class='eloText'>%s</div></div>", id, elo_names[i]);
    }
    sb_append(&sb, "</div></div><div id='statHolder'>");
    const char *chal_col = chal > 20 ? "#fb2d2d" : chal > 15 ? "#c12dfb" : chal > 10 ? "#fb942d" : "false";
#define PST(label, fmt, ...) sb_appendf(&sb, "<div class='pSt'>" label "<strong>" fmt "</strong></div>", __VA_ARGS__)
    sb_appendf(&sb, "<div class='pSt'>LVL<strong style='color:%s'>%d</strong></div>", chal_col, k);
    PST("Challenge", "%d", chal);
    js_num(num, sizeof(num), jnum(t, "player_funds"));
    PST("KR", "%s", num);
    js_num(num, sizeof(num), score);
    PST("Score", "%s", num);
    PST("SPK", "%.2f", score / (kills > 1 ? kills : 1));
    js_num(num, sizeof(num), kills);
    PST("Kills", "%s", num);
    js_num(num, sizeof(num), deaths);
    PST("Deaths", "%s", num);
    PST("KDR", "%.2f", kills / (deaths > 1 ? deaths : 1));
    PST("KPG", "%.2f", kills / (games > 1 ? games : 1));
    js_num(num, sizeof(num), games);
    PST("Games", "%s", num);
    PST("W/L", "%.2f", wins / (games - wins > 1 ? games - wins : 1));
    js_num(num, sizeof(num), wins);
    PST("Wins", "%s", num);
    js_num(num, sizeof(num), games - wins > 0 ? games - wins : 0);
    PST("Losses", "%s", num);
    for (int i = 0; i < 3; i++) {
        js_num(num, sizeof(num), elos[i]);
        sb_appendf(&sb, "<div class='pSt'>MMR (%s)<strong>%s</strong></div>", i == 0 ? "1v1" : i == 1 ? "2v2" : "4v4", num);
    }
    static const char *const stat_keys[][2] = {{"Nukes", "n"}, {"Melee", "mk"}, {"Beatdowns", "fk"}, {"Bullseyes", "tmk"}, {"Headshots", "hs"}, {"Wallbangs", "wb"}};
    for (size_t i = 0; i < sizeof(stat_keys) / sizeof(stat_keys[0]); i++) {
        js_num(num, sizeof(num), jnum(stats, stat_keys[i][1]));
        sb_appendf(&sb, "<div class='pSt'>%s<strong>%s</strong></div>", stat_keys[i][0], num);
    }
    const double shots = jnum(stats, "s");
    js_num(num, sizeof(num), round((shots ? jnum(stats, "h") / shots * 100.0 : 0.0) * 10.0) / 10.0);
    PST("Accuracy", "%s%%", num);
    readable_time(num, sizeof(num), jnum(t, "player_timeplayed"));
    PST("Time Played", "%s", num);
#undef PST
    sb_append(&sb, "</div></div><div></div><div class='scrollItemNew collapsed'><span class='scrollItemHead'>Class Stats</span><div class='material-icons toggleView' onclick='toggleView(this)'>expand_more</div><div id='skinList'>");
    for (size_t v = 0; v < sizeof(g_classes) / sizeof(g_classes[0]); v++) {
        const ClassConfig *c = &g_classes[v];
        if (!c->name || !c->name[0]) continue;
        char key[16];
        snprintf(key, sizeof(key), "c%zu", v);
        const double xp = jnum(stats, key);
        const double e2 = 0.03 * sqrt(xp > 0 ? xp : 0);
        int lvl = (int) floor(e2);
        const int prog = (int) round((e2 - lvl) * 100);
        const double clo = lvl > 1 ? pow(lvl / 0.03, 2) : 0, chi = pow((lvl + 1) / 0.03, 2);
        lvl = lvl + 1 > 1 ? lvl + 1 : 1;
        const int w = c->loadout ? c->loadout[0] : -1;
        const char *weapon = w >= 0 && w < WEAPON_COUNT && g_weapons[w] && g_weapons[w]->name ? g_weapons[w]->name : "";
        sb_appendf(&sb, "<div class='classCard'><img class='classImg' src='./textures/classes/icon_%zu.png'/><div class='className'>%s</div><div class='classWeap'>%s</div><div class='classXPBar'><div class='clsXPBarC' style='width:%d%%'></div><div class='clsXPBarV'>%lld / %lld</div></div><div class='classLvl'>Lvl %d</div></div>",
                   v, c->name, weapon, prog, (long long) (xp - clo), (long long) (chi - clo), lvl);
    }
    sb_append(&sb, "</div></div><div></div><div class='scrollItemNew'><span class='scrollItemHead'>Sales</span><div class='material-icons toggleView' onclick='toggleView(this)'>expand_less</div><div style='margin-top:10px;margin-left: 8px;'><a class='dateBtn' href='javascript:;' onclick='changeDateRange(this, 0, 1)' onmouseover='SOUND.play(`tick_0`,0.1)'>7d</a><a class='dateBtn' href='javascript:;' style='background-color:rgba(0, 0, 0, 0.6)' onclick='changeDateRange(this, 1, 1)' onmouseover='SOUND.play(`tick_0`,0.1)'>30d</a><a class='dateBtn' href='javascript:;' onclick='changeDateRange(this, 2, 1)' onmouseover='SOUND.play(`tick_0`,0.1)'>90d</a><a class='dateBtn' href='javascript:;' onclick='changeDateRange(this, 3, 1)' onmouseover='SOUND.play(`tick_0`,0.1)'>120d</a></div><div class='graphHolder'><div class='graphHeader'><span class='graphHeaderTxt'>Market Sales</span><span id='totalSalesHeader' class='graphHeaderTotal'>0</span></div><canvas id='profSales'></canvas></div></div><div></div>");
    html = sb_finish(&sb);
    set_html("pTabContent_about", html);
    free(html);
    set_html("pTabContent_activity", "");
    // creations: games r, mods l, assets f (extra[0], extra[1], extra[3])
    static const char *const heads[3] = {"Games", "Mods", "Assets"};
    static const int arg_index[3] = {4, 5, 7}; // frame slots of r, l, f
    for (int i = 0; i < 3; i++) {
        const cJSON *rows = cJSON_GetArrayItem(extra, arg_index[i]);
        sb_appendf(&sb, "<div class='scrollItemNew'><span class='scrollItemHead'>%s</span><div class='material-icons toggleView' onclick='toggleView(this)'>expand_less</div><div style='margin-top:10px;'>", heads[i]);
        const cJSON *r;
        int any = 0;
        cJSON_ArrayForEach(r, rows) {
            const char *nm = i == 2 ? jstr(r, "nm") : jstr(r, i == 0 ? "map_name" : "mod_name");
            if (!nm) nm = jstr(r, "mna");
            if (!nm) continue;
            any = 1;
            // G: age in days of map_date (dte) / mod_date / ini; NEW when 0..7
            const cJSON *dt = cJSON_GetObjectItemCaseSensitive(r, i == 2 ? "ini" : i == 0 ? "map_date" : "mod_date");
            if (i == 0 && !dt) dt = cJSON_GetObjectItemCaseSensitive(r, "dte");
            const double born = parse_time_ms(dt);
            const double age = born > 0 ? (now_epoch_ms() - born) / 86400000.0 : -1;
            sb_append(&sb, "<div class='mapListItem2'>");
            if (age >= 0 && age <= 7) sb_append(&sb, "<div class='mapAgeD' style='background-color:#E040FB'>NEW</div>");
            if (i == 2) {
                const char *plrn = jstr(r, "plrn") ? jstr(r, "plrn") : "";
                sb_append(&sb, "<div class='mapListThumbHolder' style='width:204px;'><img class='mapListThumb' loading='lazy' style='width:204px;' src='./img/noimg.png' onerror='this.onerror=null;this.src=\"./img/noimg.png\";'/></div><div style='margin-top:1px;text-overflow: ellipsis;overflow: hidden;white-space: nowrap;'><a href='javascript:;'/>");
                sb_append_served(&sb, nm);
                sb_append(&sb, "</a></div><div style='display: flex'><span style='color:rgba(0,0,0,0.2);font-size:15px'> by <a target='_blank' class='grey' href='/social.html?p=profile&q=");
                sb_append_esc(&sb, plrn);
                sb_append(&sb, "'>");
                sb_append_esc(&sb, plrn);
                sb_appendf(&sb, "</a></span><div style='font-size:15px;text-align:right;flex-grow:1'>%d <span style='color: rgba(0,0,0,0.6);'>kb</span></div></div></div>", (int) jnum(r, "siz"));
                continue;
            }
            const char *img = jstr(r, i == 0 ? "map_image" : "mod_image");
            const char *open = i == 0 ? "openMap" : "openMod";
            const char *by = i == 0 ? (jstr(r, "creatorname") ? jstr(r, "creatorname") : jstr(r, "ctn")) : g_hub.profile_name;
            const cJSON *votes = cJSON_GetObjectItemCaseSensitive(r, i == 0 ? "map_votes" : "mod_votes");
            if (i == 0 && cJSON_GetObjectItemCaseSensitive(r, "vte")) votes = cJSON_GetObjectItemCaseSensitive(r, "vte");
            sb_append(&sb, "<div class='mapListThumbHolder' style='width:204px;height:90px;'><img draggable='false' class='mapListThumb' loading='lazy' style='width:204px;' src='");
            sb_append_esc(&sb, img && *img ? img : "./img/noimg.png");
            sb_appendf(&sb, "' onclick='%s(&quot;", open);
            sb_append_js_escape(&sb, nm);
            sb_appendf(&sb, "&quot;)'/><div class='mapsVotes' style='top: 68px;'>%d<span class='material-icons' style='margin-left: 5px;color:rgba(255, 255, 255, 0.5);vertical-align: middle;margin-top: -3px;font-size: 18px;'>thumb_up</span></div></div><div style='margin-top:1px;text-overflow: ellipsis;overflow: hidden;white-space: nowrap;'><a href='javascript:;' onclick='%s(&quot;",
                       cJSON_IsNumber(votes) ? votes->valueint : 0, open);
            sb_append_js_escape(&sb, nm);
            sb_append(&sb, "&quot;)'/>");
            sb_append_served(&sb, nm);
            sb_append(&sb, "</a></div><div style='margin-top:1px;display: block;color: rgba(0,0,0,0.2);font-size: 15px;text-align: left;'>by <span onclick='openProfile(&quot;");
            sb_append_esc(&sb, by ? by : "");
            sb_append(&sb, "&quot;)' style='color: rgba(0, 0, 0, 0.6);'>");
            sb_append_esc(&sb, by ? by : "");
            sb_append(&sb, "</span></div></div>");
        }
        if (!any) sb_appendf(&sb, "<div style='margin-bottom:20px;margin-top:20px;text-align:center;'>No published %s from this user</div>", heads[i]);
        sb_append(&sb, "</div></div><div></div>");
    }
    html = sb_finish(&sb);
    set_html("pTabContent_creations", html);
    free(html);
    set_display("hackText", jnum(t, "player_hack") ? "block" : "none");
    // io.send("pst", t.player_name, J[Z]): the sales graph data (30 days)
    cJSON *f = cJSON_CreateArray();
    cJSON_AddItemToArray(f, cJSON_CreateString("pst"));
    cJSON_AddItemToArray(f, cJSON_CreateString(g_hub.profile_name));
    cJSON_AddItemToArray(f, cJSON_CreateNumber(30));
    hub_send(f);
    cJSON_Delete(stats);
}

// config.leaderCal[a](h) for the non-score/elo columns (config.js); other keys print h as is
static void append_leader_cal(StrBuf *sb, const char *a, const double h) {
    char n[32];
    if (!strcmp(a, "player_funds") || !strcmp(a, "player_followed")) comma_number(n, sizeof(n), h);
    else js_num(n, sizeof(n), h);
    if (!strcmp(a, "player_kills")) sb_appendf(sb, "%s<span class='lName'> kills</span>", n);
    else if (!strcmp(a, "player_wins")) sb_appendf(sb, "%s<span class='lName'> wins</span>", n);
    else if (!strcmp(a, "player_funds")) sb_appendf(sb, "<span style='color:rgba(0,0,0,0.4)'>%s</span> KR", n);
    else if (!strcmp(a, "player_followed")) sb_appendf(sb, "%s <span class='lName'>followers</span>", n);
    else if (!strcmp(a, "player_eventcount")) sb_appendf(sb, "<span class='lName'>found</span> %s/150", n); // easterEggs
    else if (!strcmp(a, "player_chal")) sb_appendf(sb, "%d", (int) fmin(26, h + 1)); // maxChal
    else if (!strcmp(a, "player_timeplayed")) {
        const double mins = h / 1000 / 60;
        const int m = (int) fmod(mins, 60), hr = (int) fmod(mins / 60, 24), d = (int) (mins / 60 / 24);
        if (d) sb_appendf(sb, "%dd ", d);
        if (hr) sb_appendf(sb, "%dh ", hr);
        sb_appendf(sb, "%dm ", m);
    } else sb_append(sb, n);
}

static void hub_render_leaders(const char *a, const cJSON *t) {
    StrBuf sb = {0};
    sb_append(&sb, "<div class='scrollItem'>");
    const int clans = a && !strcmp(a, "player_clan");
    const int elo = a && !strncmp(a, "player_elo", 10);
    const cJSON *r;
    int v = 0;
    cJSON_ArrayForEach(r, t) {
        if (elo && jnum(r, "player_hack")) continue;
        v++;
        const char *clan = jstr(r, "player_clan");
        const int rank = (int) jnum(r, "clan_rank");
        if (clans) {
            sb_appendf(&sb, "<div>%d. <a target='_blank' class='%s' style='color:%s' href='/social.html?p=clan&q=", v, rank == 1 ? "rainbowText" : "", clan_col(clan, rank, "rgba(0,0,0,0.6)"));
            sb_append_esc(&sb, clan ? clan : "");
            sb_append(&sb, "'>[");
            sb_append_esc(&sb, jstr(r, "clan_name") ? jstr(r, "clan_name") : "");
            sb_appendf(&sb, "]</a> (%d) ", (int) jnum(r, "clan_membercount"));
            if (jnum(r, "clan_hackcount")) sb_appendf(&sb, "<span style='color:#E85656'>%d</span>", (int) jnum(r, "clan_hackcount"));
            sb_append(&sb, "<span style='color:rgba(0,0,0,0.3)'> by <a target='_blank' href='/social.html?p=profile&q=");
            sb_append_esc(&sb, jstr(r, "creatorname") ? jstr(r, "creatorname") : "");
            sb_append(&sb, "' class='lName'>");
            sb_append_esc(&sb, jstr(r, "creatorname") ? jstr(r, "creatorname") : "");
            char n[32];
            comma_number(n, sizeof(n), jnum(r, "clan_score"));
            sb_appendf(&sb, "</a></span><div style='float:right'>%s</div></div>", n);
            continue;
        }
        const char *name = jstr(r, "player_name");
        sb_appendf(&sb, "<div>%d. <div class='leaderText'>", v);
        if (jnum(r, "player_featured")) sb_append(&sb, "<i class='material-icons' style='margin-right:5px;font-size:26px;color:#40C4FF'>check_circle</i>");
        sb_append(&sb, "<a target='_blank' href='/social.html?p=profile&q=");
        sb_append_esc(&sb, name ? name : "");
        sb_appendf(&sb, "' class='lName' style='%s'>", jnum(r, "player_hack") ? "color:#E85656" : "");
        sb_append_esc(&sb, name ? name : "");
        sb_append(&sb, "</a>");
        if (clan && clan[0]) append_clan_link(&sb, clan, rank, "rgba(0,0,0,0.4)");
        sb_append(&sb, "</div><div style='float:right'>");
        if (!a || !strcmp(a, "player_score")) {
            append_level_cell(&sb, jnum(r, "player_score"));
        } else if (elo) {
            int id = (int) floor(jnum(r, a) / 120.0);
            id = (id > 8 ? 8 : id < 1 ? 1 : id) - 1;
            sb_appendf(&sb, "<span class='floatR'><img src='./img/ranks/icon_%d.png' class='rnkIconR'></span>", id);
        } else {
            append_leader_cal(&sb, a, jnum(r, a));
        }
        sb_append(&sb, "</div></div>");
    }
    sb_append(&sb, "</div>");
    char *html = sb_finish(&sb);
    set_html("leaderList", html);
    free(html);
}

// search results (io_0 "search")
// io_0("map"/"mod", name, row): the creation page (mapProfile / modProfile). Active servers
// (Ra / Aa: matchmaker game-list filtered to this map) are not fetched: "No active servers".
static const char *const k_map_cats[] = {"Other", "Zombies", "Parkour", "Trading", "Social", "Horror",
                                         "Puzzle", "Training", "Battle Royale", "Realism", "Rotation", "Raid"};
static int g_map_tab; // Ce

// getMapTapHTML()
static void append_map_tabs(StrBuf *sb) {
    sb_appendf(sb, "<div class='mapTabP%s' onclick='updateMapTab(0)'>About</div><div class='mapTabP%s' onclick='updateMapTab(1)'>Servers</div>",
               g_map_tab == 0 ? " activeTab" : "", g_map_tab == 1 ? " activeTab" : "");
}

static void append_creation_actions(StrBuf *sb, const int id, const int featured, const int kind) {
    const AccountInfo *acc = g_hub.logged_in ? account_get() : NULL;
    sb_append(sb, "<div class='creationActions'>");
    if (acc && acc->is_dev) // canDeleteCreations (isDev || isCreationMod), canFeatureCreations (isDev)
        sb_appendf(sb, "<span class='material-icons creationAction del' onclick='deleteCreationConf(%d,%d);'>delete_forever</span><span class='material-icons creationAction%s' onclick='featureCreation(this, %d,%d);'>check_circle</span>",
                   id, kind, featured ? " sel" : "", id, kind);
    sb_append(sb, "</div>");
}

static void hub_render_creation(const int is_mod, const cJSON *t) {
    const char *pre = is_mod ? "mod" : "map";
    StrBuf sb = {0};
    sb_append(&sb, "<div class='scrollItem' style='position:relative;padding-bottom:10px;padding-top:16px;'>");
    char key[32], date[48], date2[48], num[48], ver[32];
    snprintf(key, sizeof(key), "%s_name", pre);
    const char *name = cJSON_IsObject(t) ? jstr(t, key) : NULL;
    if (!name) {
        sb_appendf(&sb, "<span style='color:rgba(0,0,0,0.4);'>%s</span>", is_mod ? STR_MODS_NONE : STR_MAPS_NONE);
        char *html = sb_finish(&sb);
        set_html(is_mod ? "modProfile" : "mapProfile", html);
        free(html);
        return;
    }
    snprintf(key, sizeof(key), "%s_id", pre);
    const int id = (int) jnum(t, key);
    snprintf(key, sizeof(key), "%s_featured", pre);
    append_creation_actions(&sb, id, (int) jnum(t, key), is_mod);
    snprintf(key, sizeof(key), "%s_creator", pre);
    const char *creator = jstr(t, key) ? jstr(t, key) : "";
    snprintf(key, sizeof(key), "%s_image", pre);
    const char *image = jstr(t, key);
    snprintf(key, sizeof(key), "%s_updatecounter", pre);
    const double upd = jnum(t, key) > 0 ? jnum(t, key) : 1;
    js_num(ver, sizeof(ver), upd / 10);
    const double pl = jnum(t, "pl"), vt = jnum(t, "vt");
    sb_append(&sb, "<img draggable='false' class='mapPageThumb' src='");
    sb_append_esc(&sb, image && *image ? image : "./img/noimg.png");
    sb_append(&sb, "' onerror='this.onerror=null;this.src=\"./img/noimg.png\";'><div style='display: inline-block;width: calc(100% - 260px);'><div style='font-size:22px;margin-bottom: 2px;'>");
    sb_append_served(&sb, name);
    sb_append(&sb, "</div><div style='font-size:16px;'>by <a target='_blank' href='/social.html?p=profile&q=");
    sb_append_esc(&sb, creator);
    sb_appendf(&sb, "' style='color: rgba(0,0,0,0.4);'>");
    sb_append_esc(&sb, creator);
    sb_append(&sb, "</a></div>");
    if (is_mod) {
        snprintf(key, sizeof(key), "%s_date", pre);
        get_date(date, sizeof(date), jstr(t, key));
        snprintf(key, sizeof(key), "%s_initialdate", pre);
        get_date(date2, sizeof(date2), jstr(t, key));
        sb_appendf(&sb, "<div style='position:absolute;right:28px;top:336px;text-align:right'><div style='font-size:17px;color:rgba(0,0,0,0.5);'>updated <b>%s</b></div><div style='font-size:17px;color:rgba(0,0,0,0.5);'>created <b>%s</b></div></div><div class='mapText' style='font-size:26px;top:268px;left:35px;'>v%s</div>", date, date2, ver);
    } else {
        comma_number(num, sizeof(num), pl);
        sb_appendf(&sb, "<div style='position:absolute;right:30px;top:335px;text-align:right;font-size:17px;'><b>%s</b><span style='color:rgba(0,0,0,0.5);'> Plays</span><div></div><b id='totalOnline'>0</b><span style='color: rgba(0, 0, 0, 0.5);'> Online</span></div>", num);
    }
    sb_appendf(&sb, "<div class='mapText' style='top:275px;right:33px;'>%d<span class='material-icons' style='margin-left:10px;color:rgba(255, 255, 255, 0.5);vertical-align:middle;margin-top:-6px;font-size:30px'>thumb_up</span></div></div>", (int) vt);
    if (!is_mod) {
        sb_append(&sb, "<div id='mapTabsP'>");
        append_map_tabs(&sb);
        sb_append(&sb, "</div>");
    }
    // t.vl: a YouTube embed (no iframes natively): the box stays hidden
    sb_appendf(&sb, "<div id='%sVid' style='display:none'></div><div id='%sDescr'%s>", pre, pre, is_mod ? "" : " style='padding-top:20px'");
    snprintf(key, sizeof(key), "%s_description", pre);
    const char *descr = jstr(t, key);
    if (descr && *descr) sb_append_served(&sb, descr);
    else sb_append(&sb, "No Description");
    if (!is_mod) {
        const int ca = (int) jnum(t, "ca");
        get_date(date, sizeof(date), jstr(t, "map_initialdate"));
        char cash[48];
        snprintf(cash, sizeof(cash), "%.2f", round(pl / 1000 * 0.5 * 100) / 100); // UTILS.formatNumCash
        comma_number(num, sizeof(num), pl);
        sb_appendf(&sb, "<div class='mapSubDesc'><div class='mapSubItm'><b>Category</b><br/>%s</div><div class='mapSubItm'><b>~Revenue</b><br/>$%s</div><div class='mapSubItm'><b>Plays</b><br/>%s</div><div class='mapSubItm'><b>Released</b><br/>%s</div><div class='mapSubItm'><b>Version</b><br/>v%s</div></div></div>",
                   k_map_cats[ca >= 0 && ca < (int) (sizeof(k_map_cats) / sizeof(k_map_cats[0])) ? ca : 0], cash, num, date, ver);
        sb_appendf(&sb, "<div id='activeGames' style='display:%s'><div class='menuSelectorHolder'><div style='position: absolute;top: 50%%;left: 50%%;transform: translate(-50%%, -50%%);color: rgba(0,0,0,0.4);'>No active servers</div></div></div>", g_map_tab == 0 ? "none" : "block");
    } else {
        sb_append(&sb, "</div>");
    }
    sb_appendf(&sb, "<div class='button %s buttonP' onclick='share%sLink(this, &quot;", pre, is_mod ? "Mod" : "Map");
    sb_append_js_escape(&sb, name);
    sb_append(&sb, "&quot;)'>Share</div>");
    if (!is_mod) {
        sb_append(&sb, "<div class='button map buttonH' onclick='hostMap(&quot;");
        sb_append_js_escape(&sb, name);
        sb_append(&sb, "&quot;)'>Host</div>");
    }
    sb_appendf(&sb, "<div class='button %s buttonS' onclick='play%s(&quot;", pre, is_mod ? "Mod" : "Map");
    sb_append_js_escape(&sb, name);
    sb_append(&sb, "&quot;)'>Play</div>");
    char *html = sb_finish(&sb);
    set_html(is_mod ? "modProfile" : "mapProfile", html);
    free(html);
    if (!is_mod) {
        const HtmlElem d = el("mapDescr");
        if (d) html_dom_set_style(d, "display", g_map_tab == 1 ? "none" : "block");
    }
}

static void hub_render_search(const cJSON *t) {
    char n[32];
    comma_number(n, sizeof(n), cJSON_GetArraySize(t));
    StrBuf sb = {0};
    sb_appendf(&sb, "<div class='scrollItem'>Search Results<div style='float:right'>%s</div></div>", n);
    char *html = sb_finish(&sb);
    set_html("searchInfo", html);
    free(html);
    sb_append(&sb, "<div class='scrollItem'>");
    const cJSON *r;
    cJSON_ArrayForEach(r, t) {
        const char *name = jstr(r, "player_name");
        const char *clan = jstr(r, "player_clan");
        sb_append(&sb, "<div>");
        if (jnum(r, "player_premium") > 0) sb_append(&sb, "<i class='material-icons' style='margin-right:1px;font-size:26px;color:#FBC02D'>beenhere</i>");
        if (jnum(r, "player_featured")) sb_append(&sb, "<i class='material-icons' style='margin-right:2px;font-size:26px;color:#40C4FF'>check_circle</i>");
        sb_append(&sb, "<a target='_blank' href='/social.html?p=profile&q=");
        sb_append_esc(&sb, name ? name : "");
        sb_appendf(&sb, "' class='lName' style='%s'>", jnum(r, "player_hack") ? "color:#E85656" : "");
        sb_append_esc(&sb, name ? name : "");
        sb_append(&sb, "</a> ");
        if (clan && clan[0]) {
            const int rank = (int) jnum(r, "clan_rank");
            sb_appendf(&sb, "<span class='searchPClan%s' style='color:%s' onclick='openLink(&quot;/social.html?p=clan&q=", rank == 1 ? " rainbowText" : "", clan_col(clan, rank, "rgba(0,0,0,0.4)"));
            sb_append_esc(&sb, clan);
            sb_append(&sb, "&quot;, &quot;_blank&quot;)'>[");
            sb_append_esc(&sb, clan);
            sb_append(&sb, "]</span>");
        }
        sb_append(&sb, "<div style='float:right'>");
        append_level_cell(&sb, jnum(r, "player_score"));
        sb_append(&sb, "</div></div>");
    }
    sb_append(&sb, "</div>");
    html = sb_finish(&sb);
    set_html("searchList", html);
    free(html);
}

// $e(post): likes / comments footer
static void append_post_footer(StrBuf *sb, const cJSON *e) {
    const int pid = (int) jnum(e, "pid");
    const int liked = jnum(e, "dlk") == 1;
    sb_appendf(sb, "<div class='likesHolder' ><span id='comVal%d'>%d</span> <span onclick='openPost(%d)' class='material-icons likeBtn' style='font-size:33px;vertical-align:middle;margin-top:-4px;margin-right:20px'>chat_bubble_outline</span><span id='likesVal%d'>%d</span> Likes <span class='material-icons likeBtn'  onclick='likePost(%d,this)'  style='font-size:33px;vertical-align:middle;margin-top:-5px;color:%s'>favorite%s</span></div>",
               pid, (int) jnum(e, "cnt"), pid, pid, (int) jnum(e, "vot"), pid, liked ? "#eb4034" : "#000", liked ? "" : "_border");
}

static void append_post_author(StrBuf *sb, const cJSON *e) {
    const char *pln = jstr(e, "pln");
    const char *cln = jstr(e, "cln");
    sb_append(sb, "<span class='postLink'><a href='/social.html?p=profile&q=");
    sb_append_esc(sb, pln ? pln : "");
    sb_append(sb, "'>");
    sb_append_esc(sb, pln ? pln : "");
    sb_append(sb, "</a>");
    if (cln && cln[0]) {
        sb_appendf(sb, " <span style='color:%s'>[", clan_col(cln, (int) jnum(e, "clr"), "rgba(0,0,0,0.2)"));
        sb_append_esc(sb, cln);
        sb_append(sb, "]</span>");
    }
    if (jnum(e, "prm") > 0) sb_append(sb, " <i class='material-icons' style='margin-bottom:5px;font-size:25px;color:#FBC02D;vertical-align:middle;margin-left:-2px'>beenhere</i>");
    if (jnum(e, "ftr")) sb_append(sb, " <i class='material-icons' style='margin-bottom:5px;font-size:25px;color:#40C4FF;vertical-align:middle;margin-left:-2px'>check_circle</i>");
}

// Je(post, single): one feed post (text posts and game links; skin unboxings need the
// store table KrunkNative does not ship)
static void append_post(StrBuf *sb, const cJSON *e, const int single) {
    char ago[64] = "Just Now";
    if (jnum(e, "tim") > 0) {
        readable_time(ago, sizeof(ago) - 4, jnum(e, "tim"));
        strcat(ago, " ago");
    }
    char flw[32];
    comma_number(flw, sizeof(flw), jnum(e, "flw"));
    sb_appendf(sb, "<div %s><div class='postHead'><img class='postPFP' onerror='this.onerror=null;this.src=\"./img/favicon.png\";'src='./img/favicon.png' /><div class='postInfo'><div>", single ? "" : "class='socPost'");
    append_post_author(sb, e);
    sb_appendf(sb, "<span class='postTime'>%s</span></div><span class='postSubInfo'>%s Followers</span></div><div class='postContent'><div style='overflow-x:hidden;'>", ago, flw);
    const char *txt = jstr(e, "txt");
    if (txt) sb_append_served(sb, txt);
    const int typ = (int) jnum(e, "typ");
    const char *lnk = jstr(e, "lnk"), *lnk2 = jstr(e, "lnk2");
    if (typ == 2 && lnk2) {
        sb_append(sb, " <a href='/?play=");
        sb_append_served(sb, lnk2);
        sb_append(sb, "' target='_blank' class='blueLink'>Play it Now!</a>");
    } else if (typ == 3 && lnk2) {
        sb_append(sb, " <a href='/?mod=");
        sb_append_served(sb, lnk2);
        sb_append(sb, "' target='_blank' class='blueLink'>Try it Now!</a>");
    } else if (typ == 7 && lnk) {
        sb_append(sb, " <a href='/?game=");
        sb_append_served(sb, lnk);
        sb_append(sb, "' target='_blank' class='blueLink'>Join Now!</a>");
    }
    sb_append(sb, "</div></div></div>");
    if (!single) append_post_footer(sb, e);
    sb_append(sb, "</div>");
}

static void hub_render_feed(const cJSON *t) {
    const AccountInfo *acc = g_hub.logged_in ? account_get() : NULL;
    if (acc) {
        char n[32];
        format_num(n, sizeof(n), acc->followers);
        set_html("postCFole", n);
        format_num(n, sizeof(n), acc->following);
        set_html("postCFoli", n);
    }
    StrBuf sb = {0};
    if (!cJSON_IsArray(t) || !cJSON_GetArraySize(t)) sb_append(&sb, "<div style='color:#fff;font-size:40px;margin-top:20px'>No Current Posts. Follow some Players!</div>");
    const cJSON *post;
    cJSON_ArrayForEach(post, t) append_post(&sb, post, 0);
    char *html = sb_finish(&sb);
    set_html("postHolder", html);
    free(html);
}

// ------------------------------------------------------------------ brain: market, item sales, clan (social.js io_0)

#include "../viewer/viewer_store.h"

#define MARKET_MIN_LVL 20       // config.marketMinLVl
#define TRADE_MIN_LVL 20        // config.tradeMinLvl
#define SALE_GRACE_MS 300000.0  // config.saleGrace
#define SALE_MAX 10000000       // config.saleMax

// data/prices.js: prices.estimateValue(skin) (the bundled snapshot; 0 = unknown)
static const int k_prices[] = {
    15, 2, 40, 40, 480, 5, 3, 10, 7, 2, 379, 1, 9, 2, 59, 2, 697, 5, 3, 9, 10, 10, 7, 5, 2, 15, 3, 2, 1, 4, 8,
    3, 1, 2, 3, 15, 1, 1, 3, 2, 1, 3, 9, 10, 5, 47, 1, 15, 1, 15, 15, 6, 1, 2, 2, 10, 1, 2, 2, 2, 2, 2, 2, 10,
    12, 75, 1, 5, 5, 10, 10, 15, 3, 15, 2, 1, 10, 2, 2, 1, 2, 1, 3, 1, 300, 1, 2, 60, 3, 5, 1, 2, 1, 2, 4, 15,
    1, 2, 1, 10, 2, 2, 5, 2, 5, 10, 19, 1, 9, 2, 3498, 70, 300, 10, 8850, 42, 398, 60, 4759, 169, 25, 10, 390,
    40, 5, 5, 40, 9, 3149, 45, 9, 8, 50, 9, 15, 5, 3, 20, 5, 14, 1, 3, 17, 2, 25, 30, 14, 43, 2, 1, 1, 10, 2,
    100, 5, 10, 99, 15, 10, 4, 2, 8, 2, 430, 5, 2, 10, 35, 3890, 350, 40, 11, 7, 5, 3, 49, 30, 5300, 650, 65,
    5, 2, 5, 3, 19, 30, 2999, 500, 19, 2, 1, 1, 1, 20, 19, 270, 160, 10, 1, 1, 2, 1, 47, 65, 3989, 579, 79, 1,
    1, 3, 2, 9, 9, 1700, 179, 19, 1, 199, 5, 1, 99, 100, 5999, 950, 99, 13, 1, 1, 1, 10, 10, 1500, 115, 10, 1,
    1, 11, 198, 250, 69, 25, 450, 15, 10, 50, 59, 5500, 373, 1249, 900, 949, 490, 14499, 598, 1099, 89, 198,
    369, 50, 22, 90, 4500, 90, 565, 39, 11, 398, 135, 230, 300, 4200, 999, 2490, 2699, 290, 300, 309, 69, 79,
    150, 287, 148, 249, 38, 6299, 12900, 60, 58, 259, 3150, 350, 3800, 4000, 4000, 999, 200, 200, 600, 237,
    199, 399, 265, 270, 7500, 3000, 3899, 3850, 4110, 30, 40, 40, 15000, 28999, 50000, 19498, 2, 17, 10, 7, 4,
    3, 2, 2, 3, 4, 1, 5, 15, 1, 3, 1, 40, 50, 25, 40, 50, 30, 2, 15, 3, 2, 25, 20, 19, 36, 3, 10, 14, 20, 13,
    5, 10, 28, 25, 10, 15, 50, 55, 69, 89, 97, 55, 10, 85, 2, 10, 20, 20, 19, 30, 15, 16, 30, 18, 15, 16, 18,
    20, 19, 19, 15, 100, 100, 100, 110, 70, 95, 15, 19, 20, 15, 925, 15, 20, 130, 58999, 9999, 500, 17500,
    739, 588, 479, 199, 65, 75, 60, 100, 200, 899, 193, 3999, 4599, 14000, 4750, 4800, 3500, 3799, 5300,
    12699, 1100777, 5, 1, 9, 1, 10, 1, 3, 2, 4, 1, 1, 2, 5, 17, 1, 2, 10, 3, 4, 9, 4, 10, 15, 93, 1400, 24999,
    27999, 15000, 8200, 17, 1, 2, 1, 2, 15, 2, 3, 2, 5, 10, 239, 2, 1, 2, 1, 1, 1, 2, 1, 2, 2, 3, 1, 1, 4, 2,
    30, 45, 3, 4, 10, 15, 25, 40, 199, 2, 37, 3, 2, 2, 200, 1, 20, 20, 1, 1, 1, 2, 2300, 2222, 33, 3850, 205,
    175, 40, 290, 3, 250, 30, 14, 400, 25, 2, 2, 2, 2, 3, 3, 10, 2, 5, 20, 443, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    2, 2, 2, 2, 3, 25, 410, 2, 5, 40, 15, 24, 30, 2, 4, 4, 2, 3, 20, 40, 2, 2, 2, 2, 5540, 5660, 300, 3737,
    40, 50, 13999, 7, 1, 1, 1, 1, 10, 1, 1, 1, 2, 10, 1, 2, 1, 1, 1, 1, 1, 1, 2, 2, 1, 1, 1, 1, 1, 9, 1, 2,
    10, 8, 3, 10, 1, 2, 1, 1, 1, 10, 3, 1, 1, 2, 1, 11, 420, 239, 2888, 20, 15, 1, 1, 12, 10, 7, 10, 1, 3, 1,
    1, 1, 9, 10, 1, 1, 1, 1, 225, 200, 1850, 10, 1, 1, 1, 1, 1, 1, 1, 1, 1, 11, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 2, 1, 1, 1, 10, 139, 1, 1, 1, 6, 100, 1, 1, 5, 6, 5, 5, 1, 1, 1, 1, 1, 5, 9, 1, 1, 1, 1, 294, 300, 5,
    1, 1, 1, 1, 5, 1, 1, 1, 1, 5, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1499, 1, 1, 1, 7, 4, 1, 4, 5, 4, 9, 7, 1,
    1, 1, 1, 1, 5, 10, 1, 1, 1, 1, 5, 1, 1, 1, 1, 4, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    100, 1000, 30, 1, 1, 1, 1, 5, 1, 1, 1, 1, 1, 56, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 7, 10, 1, 1,
    5, 6, 4, 7, 50, 1, 1, 1, 1, 1, 5, 5, 1, 1, 1, 1, 750, 850, 80, 899, 100, 800, 1599, 4600, 4900, 14798,
    13333, 16500, 2, 5, 5, 7, 10, 5, 25, 20, 20, 149, 5, 10, 225, 500, 500, 125, 20, 15, 169, 999, 4000, 3999,
    499, 200, 49, 120, 40, 196, 39, 15, 30, 190, 10, 19, 5, 50, 35, 189, 5490, 420, 469, 50, 50, 247, 589, 55,
    200, 2350, 14200, 10000, 5, 90, 6400, 4899, 200, 14, 100, 15, 9, 10, 59, 29, 10, 95, 100, 16900, 20999,
    10800, 8380, 4500, 0, 260, 2699, 10, 7, 400, 2498, 1, 2, 1, 1, 1, 2, 1, 2, 5, 9, 2, 7, 10, 5, 1, 1, 1, 1,
    1, 1, 1, 2, 2, 1, 10, 9, 10, 8, 1, 1, 2, 247, 43, 2795, 401, 339, 205, 120, 51999, 200, 45, 42, 44, 2300,
    220, 780, 2500, 7499, 230, 266666, 65999, 11999, 39000, 2946, 79, 2899, 56, 750, 9600, 2299, 200, 2995,
    199, 20, 16999, 49, 270, 2280, 2300, 255, 49, 99, 350, 220, 15, 299, 36, 250, 15, 3150, 48, 30, 449, 399,
    19, 44, 59, 40, 6000, 2300, 48, 210, 2129, 219, 400, 260, 7, 10, 80, 496, 470, 5111, 2250, 15500, 12999,
    29999, 6999, 3450, 2000, 190, 23, 3288, 18999, 4100, 275, 444, 400, 7999, 3500, 1399, 900, 1000, 550, 897,
    430, 1150, 899, 2450, 1799, 2269, 53000, 20000, 190, 400, 250, 300, 170, 250, 505, 14777, 400, 320, 329,
    359, 128, 270, 500, 5750, 1599, 2288, 100, 100, 600, 320, 150, 99, 200, 999, 499, 105, 299, 279, 100,
    1130, 777, 100, 90, 190, 179, 850, 179, 10000, 17998, 38555, 1600000, 16999, 519019, 12000, 13000, 15000,
    27000, 33999, 26999, 25000, 239, 1000, 300, 90, 350, 79, 54, 4499, 60, 500, 290, 280, 33000, 50, 410,
    4250, 27299, 192, 449, 199, 150, 19, 49, 55, 400, 50, 90, 15, 40, 50, 7, 20, 6, 14, 20, 30, 20, 20, 9, 30,
    6, 125, 36, 6, 6, 9, 50, 25, 15, 54, 21, 100, 19, 2498, 120, 299, 497, 190, 100, 3399, 449, 6000, 400,
    22500, 44, 299, 500, 369, 2500, 549, 2899, 2400, 4300, 400, 379, 32999, 18999, 3450, 4499, 1200, 89, 120,
    99, 60, 50, 100, 2499, 50, 100, 40, 100, 80, 49, 10, 5, 20, 28, 60, 50, 12, 10, 25, 5, 15, 45, 5, 9, 5,
    31, 35, 24, 40, 50, 19, 15, 59, 260, 194, 45, 50, 129, 8200, 170, 188, 30, 20, 50, 8300, 220, 280, 29,
    150, 1500, 149, 1800, 200, 129, 190, 150, 50, 50, 35, 25, 49, 95, 250, 10, 14, 38, 44, 20, 7, 10, 10, 3,
    19, 20, 50, 10, 5, 20, 5, 6, 19, 5, 3, 5, 70, 19, 15, 8, 10, 24, 7, 1890, 45, 1368, 1288, 148, 100, 35,
    179, 120, 1900, 190, 80, 329, 5, 43, 73, 20, 39, 50, 250, 12, 49, 50, 39, 24, 5, 5, 2, 3, 15, 20, 19, 10,
    20, 15, 3, 5, 10, 3, 2, 9, 1, 9, 10, 2, 19, 15, 5, 200, 300, 1000, 50, 1950, 2080, 2450, 12, 250, 150,
    12099, 1450, 59, 35, 40, 50, 100, 70, 400, 31, 90, 35, 31, 50, 7, 5, 5, 10, 19, 19, 15, 10, 4, 11, 2, 5,
    10, 5, 5, 3, 5, 8, 9, 10, 15, 15, 1, 990, 1150, 100, 69, 52, 120, 55, 10, 199, 225, 49, 150, 498, 699,
    300, 1969, 1399, 29, 15, 39, 25, 15, 500, 249, 18, 30, 15, 15, 20, 2, 2, 1, 2, 1, 25, 5, 8, 6000, 1, 5,
    12, 10, 1, 2, 7, 1, 9, 9, 9, 15, 12, 1, 4500, 11999, 26999, 32999, 23999, 450000, 9157, 475000, 39000, 50,
    600, 90, 4669, 40, 110, 39, 96, 99, 97, 100, 630, 200, 30, 60, 549, 49, 29, 7600, 80, 37999, 3200, 39, 58,
    79, 3600, 98, 34, 95, 500, 145, 400, 1, 1, 200, 149, 225, 490, 300, 999, 99, 95, 99, 5100, 200, 397, 99,
    39, 3499, 298, 2975, 248, 57000, 24500, 19499, 7988, 40, 380, 139, 150, 170, 99, 200, 850, 500, 300, 100,
    59, 81, 2990, 139, 80, 14000, 319, 310, 219, 100, 100, 2800, 3777, 500, 300, 155, 2999, 17999, 22222,
    6799, 711, 2400, 479, 2599, 129, 50, 54, 15, 14, 60, 120, 10, 35, 50, 10, 55, 2, 7, 2, 5, 30, 55, 29, 7,
    4, 20, 4, 8, 25, 3, 3, 5, 1, 10, 9, 10, 10, 2, 3, 36009, 609, 625, 150, 700, 366, 140, 50, 40, 45, 50, 25,
    135, 100, 99, 50, 59, 50, 35, 35, 10, 50, 25, 170, 48, 25, 19, 70, 2, 8, 20, 2, 5, 5, 45, 90, 34, 15, 50,
    75, 1, 10, 10, 790, 680, 19, 20, 90, 70, 5, 10, 90, 5, 1, 1, 10, 10, 1, 1, 2, 79, 5, 1, 1, 5, 5, 10, 10,
    1, 1, 97, 1, 1, 10, 2, 1, 1, 3, 2, 2, 1, 1, 1, 1, 2199, 2500, 3000, 1899, 8500, 6599, 4900, 38000, 19000,
    19000, 38000, 27000, 11500, 23999, 58000, 400, 225, 2498, 3, 3000, 1299, 850, 100, 11000, 2000, 3000, 0,
    22000, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 150000,
};

static int estimate_value(const int skin) {
    return skin >= 0 && (size_t) skin < sizeof(k_prices) / sizeof(k_prices[0]) ? k_prices[skin] : 0;
}

static const int k_rarity_sell[] = {1, 3, 10, 100, 500, 2500, 10000}; // store.rarities[r].sell

static const ViewerSkin *hub_skin(const int index) {
    return index >= 0 && index < g_viewer_skin_count ? &g_viewer_skins[index] : NULL;
}

static const char *rarity_color(const ViewerSkin *s) {
    return s && s->rarity >= 0 && s->rarity < g_viewer_rarity_count ? g_viewer_rarities[s->rarity].color : "#fff";
}

static int rarity_animate(const ViewerSkin *s) {
    return s && s->rarity == 6; // store.rarities[6] (Unobtainable) is the only animated rarity
}

// UTILS.getPreview(skin, store): textures/<sprays/<id> | previews/...>.png
static void skin_preview(char *out, const size_t n, const ViewerSkin *skin) {
    const char *types = skin->type >= 0 && skin->type < g_viewer_type_count ? g_viewer_types[skin->type] : "";
    if (skin->type == 4 && !(skin->flags & VS_IS_3D)) snprintf(out, n, "textures/sprays/%d.png", skin->id);
    else if (skin->type && skin->type != 3) {
        if (viewer_skin_tex_truthy(skin)) snprintf(out, n, "textures/previews/cosmetics/%d_%d_%s.png", skin->type, skin->id, skin->tex);
        else snprintf(out, n, "textures/previews/cosmetics/%d_%d.png", skin->type, skin->id);
    } else if (skin->type == 3) {
        if (skin->pat >= 0) snprintf(out, n, "textures/previews/%s%d_c%d.png", types, skin->id, skin->pat);
        else if (skin->tex) snprintf(out, n, "textures/previews/%s%d_%s.png", types, skin->id, skin->tex);
        else snprintf(out, n, "textures/previews/%s%d.png", types, skin->id);
    } else if (skin->mid >= 0) {
        if (skin->mid_t >= 0) snprintf(out, n, "textures/previews/%s%d_m%d_%d.png", types, skin->weapon, skin->mid, skin->mid_t);
        else snprintf(out, n, "textures/previews/%s%d_m%d.png", types, skin->weapon, skin->mid);
    } else if (skin->pat >= 0) snprintf(out, n, "textures/previews/%s%d_c%d.png", types, skin->weapon, skin->pat);
    else if (viewer_skin_tex_truthy(skin)) snprintf(out, n, "textures/previews/%s%d_%s.png", types, skin->weapon, skin->tex);
    else snprintf(out, n, "textures/previews/%s%d_%d.png", types, skin->weapon, skin->id);
}

// me(skin): the item's type line
static const char *skin_type_label(const ViewerSkin *s) {
    switch (s->type) {
        case 1: return "Head Item";
        case 2: return "Back Item";
        case 3: return "Melee Item";
        case 4: return "Spray Item";
        case 5: return "Dye Item";
        case 6: return "Waist Item";
        case 7: return "Face Item";
        default: return s->weapon >= 1 && s->weapon <= WEAPON_COUNT && g_weapons[s->weapon - 1] ? g_weapons[s->weapon - 1]->name : "";
    }
}

static int skin_animated(const ViewerSkin *s) {
    return s->puls_t > 0 || s->frames > 0 || s->mov_t > 0 || s->r_anim > 0 || (s->flags & VS_RGB);
}

// the Limited / Vaulted corner label (illicit / limT events are not in the client's table)
static void append_item_label(StrBuf *sb, const ViewerSkin *s, const char *cls, const char *style) {
    const int seas = s->seas ? s->seas : 1;
    if (seas == g_viewer_current_season) {
        if (s->flags & VS_BLOCKED) sb_appendf(sb, "<div class='%s' style='background-color:#faa332;%s'>Limited</div>", cls, style);
    } else {
        sb_appendf(sb, "<div class='%s'%s%s%s>Vaulted</div>", cls, style[0] ? " style='" : "", style, style[0] ? "'" : "");
    }
}

static void append_skin_img(StrBuf *sb, const ViewerSkin *s, const char *preview, const char *spray_cls, const char *img_cls) {
    if (s->type == 4) {
        sb_appendf(sb, "<div class='%s' style='background: url(&quot;%s&quot;);'></div>", spray_cls, preview);
    } else {
        sb_appendf(sb, "<img draggable='false' class='%s%s' src='%s'/>", img_cls, (s->flags & VS_RGB) ? " rgbHue" : "", preview);
    }
}

// store.skins entries with `illicit` (src/data/store.js): the black market. The viewer skin
// table does not carry funds / reqT / req, so they live here. req: stat key + minimum, or
// age_ms > 0 = account age (acc.createDate, not in the account data: always denied), or
// neither = never ("No Greenskins!").
typedef struct { int skin, funds; const char *req_t, *stat; double min, age_ms; } IllicitSkin;
static const IllicitSkin k_illicit[] = {
    {1483, 100, "1 Year Account Age", NULL, 0, 31536000000.0},
    {1484, 100, "2 Year Account Age", NULL, 0, 63072000000.0},
    {1641, 2000, "No Greenskins!", NULL, 0, 0},
    {1642, 3000, "Call in 1000 Nukes", "n", 1000, 0},
    {1659, 10000, "???????", "ggr", 59961, 0},
    {1672, 10000, "Level 50 Runner", "c9", 2667778, 0},
    {1734, 10000, "Level 100 Hunter", "c1", 10890000, 0},
    {1866, 10000, "10000 Thrown Melee Kills", "tmk", 10000, 0},
};

static const IllicitSkin *illicit_skin(const int skin) {
    for (size_t i = 0; i < sizeof(k_illicit) / sizeof(k_illicit[0]); i++)
        if (k_illicit[i].skin == skin) return &k_illicit[i];
    return NULL;
}

// g.req(acc)
static int illicit_req_ok(const IllicitSkin *s, const AccountInfo *acc) {
    if (!s->stat) return 0;
    const cJSON *v = acc && acc->stats ? cJSON_GetObjectItemCaseSensitive(acc->stats, s->stat) : NULL;
    return cJSON_IsNumber(v) && v->valuedouble >= s->min;
}

static int row_int(const cJSON *r, const char *a, const char *b, const char *c, const int fallback) {
    const char *keys[3] = {a, b, c};
    for (int i = 0; i < 3; i++) {
        const cJSON *v = keys[i] ? cJSON_GetObjectItemCaseSensitive(r, keys[i]) : NULL;
        if (cJSON_IsNumber(v)) return v->valueint;
        if (cJSON_IsString(v) && v->valuestring[0]) return atoi(v->valuestring);
    }
    return fallback;
}

// A listing time: epoch ms, or the brain's PostgreSQL text "YYYY-MM-DD hh:mm:ss[.f][+-hh[:mm]]"
static double parse_time_ms(const cJSON *v) {
    if (cJSON_IsNumber(v)) return v->valuedouble;
    if (!cJSON_IsString(v)) return 0;
    struct tm tm = {0};
    double frac = 0;
    int consumed = 0;
    if (sscanf(v->valuestring, "%d-%d-%d%*c%d:%d:%d%n", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec, &consumed) < 6) return 0;
    const char *p = v->valuestring + consumed;
    if (*p == '.') frac = strtod(p, (char **) &p);
    int off_min = 0;
    if (*p == '+' || *p == '-') {
        const int sign = *p == '-' ? -1 : 1;
        int hh = 0, mm = 0;
        sscanf(p + 1, "%2d:%2d", &hh, &mm);
        if (!mm && strlen(p + 1) >= 4 && p[3] != ':') sscanf(p + 3, "%2d", &mm);
        off_min = sign * (hh * 60 + mm);
    }
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    const time_t t = timegm(&tm);
    return ((double) t - off_min * 60.0 + frac) * 1000.0;
}

typedef struct {
    int uid, skin, skin_id, count, market_id;
    double funds, salestart;
    char player[64];  // seller ("" = none); copied, the rows outlive the reply frame
} MarketRow;

// io_0's row normalisation (ss/f/m/p/i|ind|sx/s|sid|sk) for one market row
static MarketRow market_row(const cJSON *r, const int uid) {
    MarketRow m = {0};
    m.uid = uid;
    m.skin = row_int(r, "ind", "i", "sx", 0);
    if (!cJSON_GetObjectItemCaseSensitive(r, "ind") && cJSON_GetObjectItemCaseSensitive(r, "skinindex")) m.skin = row_int(r, "skinindex", NULL, NULL, 0);
    m.skin_id = row_int(r, "sid", "s", "sk", 0);
    m.count = row_int(r, "c", NULL, NULL, 0);
    m.market_id = row_int(r, "m", "marketid", NULL, 0);
    m.funds = cJSON_GetObjectItemCaseSensitive(r, "f") ? jnum(r, "f") : jnum(r, "funds");
    m.salestart = parse_time_ms(cJSON_GetObjectItemCaseSensitive(r, cJSON_GetObjectItemCaseSensitive(r, "ss") ? "ss" : "salestart"));
    const char *player = jstr(r, "p");
    if (!player) player = jstr(r, "player");
    snprintf(m.player, sizeof(m.player), "%s", player ? player : "");
    return m;
}

static int g_market_no_sort; // io_fr: the filtered market keeps the server's order
// The rows of the last market list: the cards call showPopup(page, skin, uid) (v3.1.1 passes 11
// arguments; the native inline-handler parser takes 8) and the popup reads the row back.
static MarketRow *g_market_rows;
static int g_market_row_count;
static char g_market_sub[16];
static const MarketRow *g_sort_rows;
static int cmp_market_rows(const void *pa, const void *pb) {
    const MarketRow *e = pa, *a = pb; // sort(function (e, a)) ascending on (rarity, season, -index)
    const ViewerSkin *se = hub_skin(e->skin), *sa = hub_skin(a->skin);
    const int t = sa ? sa->rarity : 0, n = se ? se->rarity : 0;
    const int i = sa ? (sa->seas ? sa->seas : 1) : 0, r = se ? (se->seas ? se->seas : 1) : 0;
    if (t > n) return 1;
    if (t < n) return -1;
    if (i > r) return 1;
    if (i < r) return -1;
    return a->skin - e->skin;
}

static double now_epoch_ms(void) {
#ifdef __EMSCRIPTEN__
    return EM_ASM_DOUBLE({ return Date.now(); });
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double) ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
#endif
}

// ra(funds): the header's KR after a market action
static void hub_set_funds(const double funds) {
    const AccountInfo *acc = g_hub.logged_in ? account_get() : NULL;
    if (!acc) return;
    char kr[32];
    comma_number(kr, sizeof(kr), funds);
    StrBuf sb = {0};
    sb_append_esc(&sb, acc->name);
    sb_appendf(&sb, "<div style='height:4px'></div><div id='profileKR'>%s<span style='color:#fff'> KR</span></div>", kr);
    char *html = sb_finish(&sb);
    set_html("profileName", html);
    free(html);
}

static void hub_render_trades(StrBuf *out, const cJSON *t, int *shown) {
    const AccountInfo *acc = g_hub.logged_in ? account_get() : NULL;
    if (!acc || acc->level < TRADE_MIN_LVL) {
        sb_appendf(out, "Must be level %d", TRADE_MIN_LVL);
        *shown = -1; // marketList = O, no "none" text
        return;
    }
    *shown = 1;
    StrBuf pe[3] = {{0}, {0}, {0}};
    int fe[3] = {0, 0, 0};
    for (int ge = 0; ge < 2; ge++) {
        const cJSON *ue = cJSON_GetArrayItem(t, ge);
        const cJSON *o;
        cJSON_ArrayForEach(o, ue) {
            const int he = (int) jnum(o, "pl") == acc->id;
            const int we = ge == 1 ? 2 : he ? 1 : 0;
            const char *give = jstr(o, he ? "sksell" : "skbuy"), *take = jstr(o, he ? "skbuy" : "sksell");
            const char *name = jstr(o, he ? "buyer" : "player");
            if (!name) continue;
            int ids[2][16], counts[2] = {0, 0};
            const char *lists[2] = {give, take};
            for (int k = 0; k < 2; k++) {
                for (const char *p = lists[k]; p && *p && counts[k] < 16;) {
                    ids[k][counts[k]++] = atoi(p);
                    p = strchr(p, ',');
                    if (p) p++;
                }
            }
            const int be = counts[0] > counts[1] ? counts[0] : counts[1];
            fe[we]++;
            StrBuf *b = &pe[we];
            const int trid = (int) jnum(o, "trid");
            sb_appendf(b, "<div class='offerHolder' %s id='of_%d' onclick='viewOffer(%d,%d)'>",
                       we == 0 && counts[1] == 0 ? "style='background-color:#ffb2b2'" : we == 0 && counts[0] == 0 ? "style='background-color:#C4F399'" : "", trid, trid, we);
            if (we != 2) sb_appendf(b, "<div class='material-icons offerCancel' onclick='fastDeclOffer(%d)'>clear</div>", trid);
            char ago[48] = "just now";
            const char *dte = jstr(o, "dte");
            (void) dte;
            sb_appendf(b, "<div class='offerDate'%s>%s</div><div class='offerFrom'>Offer %s <span id='oProf' onclick='openLink(&quot;/social.html?p=profile&q=", we == 2 ? " style='right:11px;'" : "", ago, he ? "to" : "from");
            sb_append_esc(b, name);
            sb_append(b, "&quot;, &quot;_blank&quot;)'>");
            sb_append_esc(b, name);
            sb_append(b, "</span></div>");
            for (int k = 0; k < 2; k++) {
                sb_append(b, "<div class='offerContents'>");
                for (int i = 0; i < counts[k]; i++) {
                    const ViewerSkin *pa = hub_skin(ids[k][i]);
                    if (!pa) continue;
                    char prev[256];
                    skin_preview(prev, sizeof(prev), pa);
                    sb_appendf(b, "<div class='oItemHolder'style='border: 2px solid %s;'data-index='%d'>", rarity_color(pa), ids[k][i]);
                    append_skin_img(b, pa, prev, "oItemSprayImg", "oItemImg");
                    sb_append(b, "</div>");
                }
                for (int i = 0; i < (be > 5 ? 10 : 5) - counts[k]; i++) sb_append(b, "<div class='oItemHolder' style='border: 2px solid rgba(0,0,0,0);'><div draggable='false' class='oItemImg'></div></div>");
                sb_append(b, "</div>");
                if (k == 0) sb_append(b, "<div class='oSwapHolder'><div class='material-icons oSwapIcon'>swap_horiz</div></div>");
            }
            sb_append(b, "</div>");
        }
    }
    static const char *const ids[3] = {"in", "out", "his"};
    static const char *const heads[3] = {"Incoming Offers", "Outgoing Offers", "Trade History"};
    sb_append(out, "<div class='scrollItem' id='offers'><div id='tradeNav'><div class='tradeNavBtn' style='background-color: rgba(0, 0, 0, 0.5);' onclick='updateTrades(&quot;in&quot;)' id='tn_in'>Incoming</div><div class='tradeNavBtn' id='tn_out' onclick='updateTrades(&quot;out&quot;)'>Outgoing</div><div class='tradeNavBtn' id='tn_his' onclick='updateTrades(&quot;his&quot;)'>History</div></div>");
    for (int i = 0; i < 3; i++) {
        sb_appendf(out, "<div id='o_%s'%s><div style='margin-bottom:10px;margin-top:-4px;'>%s (<span>%d</span>)%s</div>", ids[i], i ? " style='display:none'" : "", heads[i], fe[i],
                   i == 0 ? "<div class='declineAll' onclick='declineAllTrades();'>Decline All</div>" : "");
        char *body = sb_finish(&pe[i]);
        sb_append(out, body[0] ? body : "<div style='color: rgba(0,0,0,0.6);font-size: 17px;'>None</div>");
        free(body);
        sb_append(out, "</div>");
    }
    sb_append(out, "</div>");
}

// io_0("market", sub, rows): the market / inventory / sales / stats / trades lists
static void hub_render_market(const char *a, const cJSON *t) {
    const AccountInfo *acc = g_hub.logged_in ? account_get() : NULL;
    StrBuf sb = {0};
    int shown = 0;
    const char *sub = a ? a : "market";
    if (cJSON_IsArray(t) || cJSON_IsObject(t)) {
        if (!strcmp(sub, "stats")) {
            side_boxes();
            sb_append(&sb, "<div class='scrollItem' style='width:680px;padding-top:20px;padding-bottom:15px;overflow:hidden;max-height:unset;'><div style='margin-bottom:10px;'>Rarest Items<span style='float: right;'>In Circulation</span></div>");
            const cJSON *r;
            cJSON_ArrayForEach(r, t) {
                const int ski = row_int(r, "ski", NULL, NULL, -1);
                const ViewerSkin *pe = hub_skin(ski);
                if (!pe || (pe->flags & VS_BLOCKED)) continue;
                shown++;
                char prev[256];
                skin_preview(prev, sizeof(prev), pe);
                sb_appendf(&sb, "<div class='statCard'><div class='statImgH%s' style='border:5px solid %s'>", rarity_animate(pe) ? " rainbowBorder" : "", rarity_color(pe));
                append_skin_img(&sb, pe, prev, "sprayStatImg", "statImg");
                sb_append(&sb, "</div>");
                append_item_label(&sb, pe, "itemLabel small", "");
                sb_appendf(&sb, "<div style='display:inline-block'><div class='statItem%s' style='color:%s;' onclick='openLink(&quot;/social.html?p=itemsales&i=%d&quot;, &quot;_blank&quot;)'>", rarity_animate(pe) ? " rainbowText" : "", rarity_color(pe), ski);
                sb_append_esc(&sb, pe->name);
                sb_appendf(&sb, "</div><div style='color:rgba(255,255,255,0.4);font-size: 14px;'>%s</div><div class='statInC'><span style='color:rgba(255,255,255,0.4)'>x </span>%d</div><div class='statFind' onclick='openLink(&quot;/social.html?p=market&i=%d&quot;, &quot;_blank&quot;)'>Find</div></div></div>",
                           skin_type_label(pe), row_int(r, "inC", "cnt", NULL, 0), ski);
            }
            sb_append(&sb, "</div>");
        } else if (!strcmp(sub, "blackm")) {
            // every store.skins entry with `illicit` (re = 1: never "No Items Found")
            shown = 1;
            for (size_t i = 0; i < sizeof(k_illicit) / sizeof(k_illicit[0]); i++) {
                const IllicitSkin *il = &k_illicit[i];
                const ViewerSkin *pe = hub_skin(il->skin);
                if (!pe) continue;
                char prev[256], price[32];
                skin_preview(prev, sizeof(prev), pe);
                const char *col = rarity_color(pe);
                sb_appendf(&sb, "<div class='marketCard%s' id='itemCard_%d' style='color:%s;border:5px solid %s' onmouseenter='SOUND.play(&quot;tick_0&quot;,0.1)' onclick='showPopup(&quot;blackm&quot;,%d)'>",
                           rarity_animate(pe) ? " rainbow" : "", il->skin, col, col, il->skin);
                sb_append_esc(&sb, pe->name);
                sb_appendf(&sb, "<div class='itemOwn'>%s</div>", skin_type_label(pe));
                append_skin_img(&sb, pe, prev, "sprayMarketImg", pe->type ? (pe->type == 5 ? "marketImgD" : "marketImgC") : "marketImg");
                comma_number(price, sizeof(price), il->funds);
                sb_appendf(&sb, "<div class='marketPrice'>%s<span style='color:#fff'> KR</span></div><div class='purchInfoBtn' onmouseenter='SOUND.play(`tick_0`,0.1)' onclick='openLink(&quot;/social.html?p=itemsales&i=%d&quot;, &quot;_blank&quot;)'>?</div></div>", price, il->skin);
            }
        } else if (!strcmp(sub, "trades")) {
            hub_render_trades(&sb, t, &shown);
            if (shown < 0) {
                char *html = sb_finish(&sb);
                set_html("marketList", html);
                free(html);
                return;
            }
        } else if (cJSON_IsArray(t)) {
            const int n = cJSON_GetArraySize(t);
            MarketRow *rows = calloc((size_t) (n ? n : 1), sizeof(*rows));
            if (rows) {
                for (int v = 0; v < n; v++) rows[v] = market_row(cJSON_GetArrayItem(t, v), v);
                if (!g_market_no_sort && strcmp(sub, "market") != 0) qsort(rows, (size_t) n, sizeof(*rows), cmp_market_rows);
                free(g_market_rows);
                g_market_rows = calloc((size_t) (n ? n : 1), sizeof(*g_market_rows));
                g_market_row_count = g_market_rows ? n : 0;
                if (g_market_rows) memcpy(g_market_rows, rows, (size_t) n * sizeof(*rows));
                snprintf(g_market_sub, sizeof(g_market_sub), "%s", sub);
                const int inventory = !strcmp(sub, "inventory");
                int te = 0;
                double oe = 0;
                for (int v = 0; v < n; v++) {
                    const MarketRow *m = &rows[v];
                    const ViewerSkin *pe = hub_skin(m->skin);
                    if (!pe) continue;
                    // config.marketQueries[a].checkItem
                    if ((!strcmp(sub, "market") || !strcmp(sub, "sales")) && !m->funds) continue;
                    if (inventory && (m->funds || !m->count)) continue;
                    shown++;
                    te++;
                    const int est = estimate_value(m->skin);
                    oe += (double) est * m->count;
                    char prev[256];
                    skin_preview(prev, sizeof(prev), pe);
                    const char *col = rarity_color(pe);
                    sb_appendf(&sb, "<div class='marketCard%s' id='itemCard%s_%d' style='color:%s;border:5px solid %s' onmouseenter='SOUND.play(&quot;tick_0&quot;,0.1)'>", rarity_animate(pe) ? " rainbow" : "", sub, m->uid, col, col);
                    sb_append_esc(&sb, pe->name);
                    sb_appendf(&sb, "<div class='itemOwn'>%s</div>", skin_type_label(pe));
                    append_skin_img(&sb, pe, prev, "sprayMarketImg", pe->type ? (pe->type == 5 ? "marketImgD" : "marketImgC") : "marketImg");
                    if (inventory) {
                        char est_s[32];
                        comma_number(est_s, sizeof(est_s), est);
                        sb_appendf(&sb, "<div class='cardEst'>~ %s <span style='color:#fff'>KR</span></div><div class='cardCnt'>x <span id='invCnt%d' style='color:#fff'>%d</span></div>", est_s, m->uid, m->count);
                    } else {
                        char price[32];
                        comma_number(price, sizeof(price), m->funds);
                        if (m->funds) sb_appendf(&sb, "<div class='marketPrice'>%s<span style='color:#fff'> KR</span></div>", price);
                        else sb_append(&sb, "<div class='marketPrice'><span style='opacity:0'>s</span></div>");
                    }
                    if (skin_animated(pe)) sb_append(&sb, "<div class='material-icons isAnim'>loop</div>");
                    // v3.1.1: showPopup(page, skin, preview, typeLabel, skinid, funds, count, salestart, player, uid, marketid)
                    sb_appendf(&sb, "<div class='cardActions'><a class='cardAction' onmouseenter='SOUND.play(&quot;tick_0&quot;,0.1)' onclick='showPopup(&quot;%s&quot;,%d,%d)'>%s</a><div class='cardActionSep'></div>",
                               sub, m->skin, v, !strcmp(sub, "market") ? "Purchase" : inventory ? "List" : !strcmp(sub, "sales") ? "Unlist" : "");
                    if (!inventory) sb_appendf(&sb, "<a class='cardAction' onmouseenter='SOUND.play(&quot;tick_0&quot;,0.1)' onclick='searchItem(%d)'>Search</a>", m->skin);
                    else sb_appendf(&sb, "<a class='cardAction' onmouseenter='SOUND.play(&quot;tick_0&quot;,0.1)' onclick='quickSell(%d,%d,%d)'>Quick Sell</a>", m->skin, pe->rarity, m->uid);
                    sb_appendf(&sb, "<div class='cardActionSep'></div><a class='cardAction' onmouseenter='SOUND.play(&quot;tick_0&quot;,0.1)' onclick='openLink(&quot;/social.html?p=itemsales&i=%d&quot;, &quot;_blank&quot;)'>Info</a></div></div>", m->skin);
                }
                char track[96], value[96], v_s[32];
                snprintf(track, sizeof(track), "%d<span style='color:#fff'> / %d Items</span>", te, g_viewer_skin_count);
                comma_number(v_s, sizeof(v_s), oe);
                snprintf(value, sizeof(value), "Est. Value <span style='color:#fff'>%s</span> KR", v_s);
                set_html("invTrack", track);
                set_html("invValue", value);
                free(rows);
            }
        }
    }
    g_market_no_sort = 0;
    if (!shown) {
        sb_free(&sb);
        sb_appendf(&sb, "<div style='font-size:20px;margin-top:20px;color:#fff'>%s...</div>", acc ? STR_MARKET_NONE : STR_MARKET_LOGIN);
    }
    char *html = sb_finish(&sb);
    set_html("marketList", html);
    free(html);
}

// io_0("itemsales", "market", {ind, high, low, onS, inC}, owners[])
static void hub_render_itemsales(const cJSON *t, const cJSON *owners) {
    side_boxes();
    set_display("itemsalesList", "none");
    const ViewerSkin *pe = cJSON_IsObject(t) ? hub_skin(row_int(t, "ind", NULL, NULL, -1)) : NULL;
    if (!pe) {
        set_html("itemsalesHolder", "<div style='font-size:20px;margin-top:20px;color:#fff'>" STR_MARKET_NONE "...</div>");
        set_display("itemsalesHolder", "block");
        return;
    }
    const int ind = row_int(t, "ind", NULL, NULL, 0);
    set_display("itemsalesList", "block");
    char prev[256], a[32], b[32];
    skin_preview(prev, sizeof(prev), pe);
    const char *col = rarity_color(pe);
    StrBuf sb = {0};
    if (pe->type == 4) sb_appendf(&sb, "<div class='sprayPopupImg' style='background: url(&quot;%s&quot;);margin-top:0px;border:5px solid %s'></div>", prev, col);
    else sb_appendf(&sb, "<div class='imgWepH%s' style='border:5px solid %s'><img draggable='false' class='imgWep%s' src='%s'></div>", rarity_animate(pe) ? " rainbowBorder" : "", col, (pe->flags & VS_RGB) ? " rgbHue" : "", prev);
    append_item_label(&sb, pe, "itemLabel", "top: 202px;left: 0px;");
    sb_appendf(&sb, "<div class='popupImgSeason' style='top: 8px;left: 12px;'>Season %d</div>", pe->seas ? pe->seas : 1);
    if (pe->type != 4) sb_appendf(&sb, "<div class='popupImgInfo' style='left: 181px;top: 9px;' onclick='openViewer(%d)'>3D</div>", ind);
    sb_appendf(&sb, "<div class='buyBtn' onclick='openLink(&quot;/social.html?p=market&i=%d&quot;, &quot;_blank&quot;)'>Find</div><div style='display:inline-block'><div class='details%s' style='color:%s'>", ind, rarity_animate(pe) ? " rainbowText" : "", col);
    sb_append_esc(&sb, pe->name);
    comma_number(a, sizeof(a), jnum(t, "onS"));
    comma_number(b, sizeof(b), jnum(t, "inC"));
    sb_appendf(&sb, "</div><div class='smDetails'>by Krunker.io</div><div class='smDetails'>%s</div><div class='smDetails' style='margin-top: 94px;'>On Sale <span style='float:right'>%s <span style='color:#787878'> units</span></span></div><div class='smDetails'>In Circulation <span style='float:right'>%s <span style='color:#787878'> units</span></span></div></div>",
               skin_type_label(pe), a, b);
    comma_number(a, sizeof(a), jnum(t, "high"));
    comma_number(b, sizeof(b), jnum(t, "low"));
    sb_appendf(&sb, "<div style='position:absolute;top: 240px;right: 0px;'><div class='minMax' style='background-color:#41c300'><i class='material-icons' style='color:#fff;font-size:26px;vertical-align:middle;margin-top:-5px'>arrow_upward</i>%s</div><div class='minMax' style='background-color:rgb(244,67,54)'><i class='material-icons' style='color:#fff;font-size:26px;vertical-align:middle;margin-top:-5px'>arrow_downward</i>%s</div>", a, b);
    char *html = sb_finish(&sb);
    set_html("weaponInfo", html);
    free(html);
    // _e(ind, 30): the price graph data (["st"] -> ["gd"]; KrunkNative draws no chart)
    cJSON *f = cJSON_CreateArray();
    cJSON_AddItemToArray(f, cJSON_CreateString("st"));
    cJSON_AddItemToArray(f, cJSON_CreateNumber(ind));
    cJSON_AddItemToArray(f, cJSON_CreateNumber(30));
    cJSON_AddItemToArray(f, cJSON_CreateTrue());
    hub_send(f);
    if (cJSON_IsArray(owners) && cJSON_GetArraySize(owners)) {
        int v = 0;
        const cJSON *r;
        cJSON_ArrayForEach(r, owners) {
            const char *nm = jstr(r, "nm");
            sb_appendf(&sb, "<div>%d. <div class='leaderText'>", ++v);
            if (jnum(r, "su")) sb_append(&sb, "<i class='material-icons' style='color:#FBC02D;margin-top:-8px;font-size:28px;vertical-align:middle;'>beenhere</i>");
            if (jnum(r, "ft")) sb_append(&sb, "<i class='material-icons' style='color:#40C4FF;margin-top:4px;margin-right: 4px;font-size:28px;vertical-align:baseline;'>check_circle</i>");
            sb_append(&sb, "<a target='_blank' href='/social.html?p=profile&q=");
            sb_append_esc(&sb, nm ? nm : "");
            sb_append(&sb, "&autoTrade' class='lName'>");
            sb_append_esc(&sb, nm ? nm : "");
            sb_appendf(&sb, "</a></div><div style='float:right'>x%d</div></div>", (int) jnum(r, "ct"));
        }
        html = sb_finish(&sb);
        set_html("iTabContent_owners", html);
        free(html);
    }
    set_display("itemsalesHolder", "block");
}

// io_0("clan", tag, {clan_name, clan_rank, clan_score, creatorname, members[{p, r, s, su, ve, h}]})
static void hub_render_clan(const cJSON *t) {
    static const char *const roles[4] = {"Recruit", "Soldier", "Captain", "Commander"}; // config.clanRoles
    static const int role_perms[4] = {0, 0, 1, 2};
    const char *name = jstr(t, "clan_name");
    const char *creator = jstr(t, "creatorname");
    const int rank = (int) jnum(t, "clan_rank");
    const cJSON *members = cJSON_GetObjectItemCaseSensitive(t, "members");
    const int count = cJSON_GetArraySize(members);
    char score[32];
    comma_number(score, sizeof(score), jnum(t, "clan_score"));
    StrBuf sb = {0};
    sb_appendf(&sb, "<div class='scrollItem'><span style='color:%s' class='%s'>[", clan_col(name, rank, ""), rank == 1 ? "rainbowText" : "");
    sb_append_esc(&sb, name ? name : "");
    sb_appendf(&sb, "]</span> (%d)<span style='color:rgba(0,0,0,0.3)'> by <a target='_blank' href='/social.html?p=profile&q=", count);
    sb_append_esc(&sb, creator ? creator : "");
    sb_append(&sb, "' class='lName'>");
    sb_append_esc(&sb, creator ? creator : "");
    sb_appendf(&sb, "</a></span><div style='float:right'>%s</div><div style='margin-top:15px;margin-bottom:8px'><div class='clanBut' style='background-color:#659ad6' onclick='clanRS(1)'>Clan Discord</div><div class='clanBut' style='float:right;background-color:#c365d6' onclick='clanRS(0, &quot;", score);
    sb_append_esc(&sb, name ? name : "");
    sb_append(&sb, "&quot;)'>Send Request</div></div></div>");
    char *html = sb_finish(&sb);
    set_html("clanInfo", html);
    free(html);
    // members sorted by score (desc), grouped by role from Commander down
    const cJSON **sorted = calloc((size_t) (count ? count : 1), sizeof(*sorted));
    int n = 0;
    const cJSON *m;
    cJSON_ArrayForEach(m, members) if (sorted) sorted[n++] = m;
    for (int i = 1; i < n; i++) {
        for (int j = i; j > 0 && jnum(sorted[j], "s") > jnum(sorted[j - 1], "s"); j--) {
            const cJSON *tmp = sorted[j];
            sorted[j] = sorted[j - 1];
            sorted[j - 1] = tmp;
        }
    }
    sb_append(&sb, "<div class='scrollItem' style='max-height: calc(100% - 80px);'>");
    for (int re = 3; re >= 0; --re) {
        int ie = 0;
        StrBuf le = {0};
        for (int v = 0; v < n; v++) {
            const char *p = jstr(sorted[v], "p");
            const int is_creator = creator && p && !strcmp(creator, p);
            if (!((is_creator && role_perms[re] == 2) || ((int) jnum(sorted[v], "r") == re && !is_creator))) continue;
            ie++;
            sb_append(&le, "<div style='padding-left:8px;padding-right:8px'>");
            if (jnum(sorted[v], "su")) sb_append(&le, "<i class='material-icons' style='color:#FBC02D;margin-top:-8px;font-size:28px;vertical-align:middle;'>beenhere</i>");
            if (jnum(sorted[v], "ve")) sb_append(&le, "<i class='material-icons' style='color:#40C4FF;margin-top:4px;margin-right: 4px;font-size:28px;vertical-align:baseline;'>check_circle</i>");
            sb_append(&le, "<a target='_blank' href='/social.html?p=profile&q=");
            sb_append_esc(&le, p ? p : "");
            sb_appendf(&le, "' class='lName' style='%s'>", jnum(sorted[v], "h") ? "color:#E85656" : "");
            sb_append_esc(&le, p ? p : "");
            sb_append(&le, "</a><div style='float:right'>");
            append_level_cell(&le, jnum(sorted[v], "s"));
            sb_append(&le, "</div></div>");
        }
        sb_appendf(&sb, "<div style='margin-top:%dpx;margin-bottom:5px;font-size:22px;background-color:rgba(0,0,0,0.1);padding:3px;padding-left:10px;padding-right:10px;border-radius:5px'>%ss <span style='float:right;color:rgba(0,0,0,0.4)'>%d Members</span></div>",
                   role_perms[re] == 2 ? 10 : 18, roles[re], ie);
        char *list = sb_finish(&le);
        sb_append(&sb, list);
        free(list);
        if (!ie) sb_appendf(&sb, "<div style='color:rgba(0,0,0,0.5);padding-left:8px'>No %ss</div>", roles[re]);
    }
    sb_append(&sb, "</div>");
    free(sorted);
    html = sb_finish(&sb);
    set_html("clanRoster", html);
    free(html);
}

// io_uf(posts): the profile's Activity tab
static void hub_render_activity(const cJSON *posts) {
    StrBuf sb = {0};
    if (!cJSON_IsArray(posts) || !cJSON_GetArraySize(posts)) sb_append(&sb, "<div class='scrollItemNew'><div style='font-size:40px;text-align:center;'>No Current Posts!</div></div>");
    const cJSON *p;
    cJSON_ArrayForEach(p, posts) append_post(&sb, p, 0);
    char *html = sb_finish(&sb);
    set_html("pTabContent_activity", html);
    free(html);
}

// io_psd(days): the profile's sales graph. KrunkNative has no chart renderer: the
// header total is filled in, the canvas stays empty.
static void hub_render_sales(const cJSON *days) {
    double total = 0;
    const cJSON *d;
    cJSON_ArrayForEach(d, days) total += jnum(d, "f");
    char n[48];
    js_num(n, sizeof(n), total);
    StrBuf sb = {0};
    sb_appendf(&sb, "%s <span style='color:#fff'>KR</span>", n);
    char *html = sb_finish(&sb);
    set_html("totalSalesHeader", html);
    free(html);
}

// UTILS.getPercentDiff + the io_gd trend pill ("+n%" / "~0%" / "-n%", green / red)
static void set_trend(const char *id, const double from, const double to, const int any) {
    const double c = any && from != 0 ? round((to - from) / from * 100) : 0;
    char s[32];
    snprintf(s, sizeof(s), "%s%d%%", c > 0 ? "+" : c == 0 ? "~" : "-", (int) fabs(c));
    set_html(id, s);
    set_style(id, "background-color", c >= 0 ? "#7ed633" : "#F44336");
}

// io_gd(days): the item page's price / units graph headers (no chart renderer)
static void hub_render_item_graph(const cJSON *days) {
    const int n = cJSON_GetArraySize(days);
    double units = 0, avg = 0;
    const cJSON *d;
    cJSON_ArrayForEach(d, days) {
        units += jnum(d, "t");
        avg += floor(jnum(d, "f") / jnum(d, "t"));
    }
    const cJSON *first = cJSON_GetArrayItem(days, 0), *last = cJSON_GetArrayItem(days, n - 1);
    set_trend("averageCostTrending", n ? jnum(first, "f") / jnum(first, "t") : 0, n ? jnum(last, "f") / jnum(last, "t") : 0, n);
    set_trend("totalSoldTrending", n ? jnum(first, "t") : 0, n ? jnum(last, "t") : 0, n);
    char s[64];
    js_num(s, sizeof(s), units);
    set_html("totalSoldHeader", s);
    const double a = n ? floor(avg / n) : 0;
    snprintf(s, sizeof(s), "%d <span style='color:#fff'>KR</span>", isfinite(a) ? (int) a : 0);
    set_html("averageCostHeader", s);
}

// ------------------------------------------------------------------ brain: market popups (showPopup and friends)

typedef struct {
    char page[16], preview[256], label[64], skin_id[24], funds[24], salestart[24], player[64];
    int skin, count, uid, market_id, valid;
} MarketPop;
static MarketPop g_last_pop; // lastPop (refreshPopup)

static void market_popup(const MarketPop *p) {
    const ViewerSkin *g = hub_skin(p->skin);
    reset_pops();
    set_display("popupHolder", "block");
    set_style("popupContent", "width", "618px");
    set_display("popupContent", "block");
    if (!g) return;
    g_last_pop = *p;
    g_last_pop.valid = 1;
    const AccountInfo *acc = g_hub.logged_in ? account_get() : NULL;
    const double l = atof(p->salestart);
    const double h = now_epoch_ms() - l > 0 ? now_epoch_ms() - l : 0;
    const char *col = rarity_color(g);
    StrBuf sb = {0};
    sb_appendf(&sb, "<div class='popupImgSeason'>Season %d</div>", g->seas ? g->seas : 1);
    if (g->type == 4) sb_appendf(&sb, "<div class='sprayPopupImg' style='background: url(&quot;%s&quot;);border:5px solid %s;'></div>", p->preview, col);
    else sb_appendf(&sb, "<div class='popupImgH%s' style='border:5px solid %s;'><img draggable='false' class='popupImg%s' src='%s'/></div><div class='popupImgInfo' onclick='openViewer(%d)'>3D</div>", rarity_animate(g) ? " rainbowBorder" : "", col, (g->flags & VS_RGB) ? " rgbHue" : "", p->preview, p->skin);
    const IllicitSkin *il = illicit_skin(p->skin);
    if (il) sb_append(&sb, "<div class='itemLabel' style='background-color:#00ffff'>?\?\?</div>");
    else append_item_label(&sb, g, "itemLabel", "");
    sb_appendf(&sb, "<div class='popupInfo'><div class='pItemName%s' style='color:%s'>", rarity_animate(g) ? " rainbowText" : "", col);
    sb_append_esc(&sb, g->name);
    sb_appendf(&sb, "</div><div class='pItemRar'>x <span style='color:rgba(255,255,255,0.8);font-size:22px'>%d</span></div><div class='pItemDesc'>", p->count ? p->count : 1);
    sb_append_esc(&sb, p->label);
    sb_append(&sb, "</div>");
    const int inventory = !strcmp(p->page, "inventory"), blackm = !strcmp(p->page, "blackm");
    if (blackm) {
        sb_append(&sb, "<div class='pItemOwn' style='bottom: 70px;'>");
        sb_append_esc(&sb, il && il->req_t ? il->req_t : "I got what you need");
        sb_append(&sb, "</div>");
    } else if (!inventory) {
        const char *who = p->player[0] && strcmp(p->player, "0") ? p->player : "You";
        sb_append(&sb, "<a class='pItemOwn' href='/social.html?p=profile&q=");
        sb_append_esc(&sb, who);
        sb_append(&sb, "' target='_blank'>");
        sb_append_esc(&sb, who);
        sb_append(&sb, "</a>");
    }
    if (l > 0 && !inventory) {
        char ago[48];
        readable_time(ago, sizeof(ago), h);
        sb_appendf(&sb, "<div class='pItemTime'>listed %s ago</div>", ago);
    }
    char a[32], b[32], c[32];
    if (!strcmp(p->page, "market")) {
        const int grace = h < SALE_GRACE_MS;
        const char *block = !acc ? "Login to Buy" : acc->level < MARKET_MIN_LVL ? "lvl 20 required" : NULL;
        const double funds = acc ? acc->funds : 0, price = atof(p->funds), left = funds - price;
        comma_number(a, sizeof(a), funds);
        comma_number(b, sizeof(b), price);
        comma_number(c, sizeof(c), left);
        sb_appendf(&sb, "<div class='pItemPricing'><div class='priceP'>%s <span style='color:#fff'>KR</span></div><div class='priceP'>-%s <span style='color:#fff'>KR</span></div><div class='pricePF' style='%s'>%s <span style='color:#fff'>KR</span></div></div>",
                   a, b, left < 0 ? "color:#ed4242" : "", c);
        sb_appendf(&sb, "<div class='pItemButton' style='%s' ", block || grace ? "background-color:#2b2b2b" : left < 0 ? "background-color:#ed4242" : "");
        if (!block && !grace && left >= 0) sb_appendf(&sb, "onclick='buyItem(%d,%d)'onmouseover='SOUND.play(&quot;tick_0&quot;,0.1)'", p->market_id, p->uid);
        if (grace) {
            readable_time(a, sizeof(a), SALE_GRACE_MS - h);
            sb_appendf(&sb, "><span id='graceTimer' style='color:rgba(255,255,255,0.6)';'>ready in %s</span></div>", a);
        } else {
            sb_appendf(&sb, ">%s</div>", block ? block : left < 0 ? "Can't Afford" : "Purchase");
        }
    } else if (inventory) {
        const char *block = !acc ? "Login to Sell" : acc->level < MARKET_MIN_LVL ? "lvl 20 required" : NULL;
        sb_appendf(&sb, "<div class='pItemPricing'><div id='sellPricFee'>No List Fee</div><input id='sellPriceIn' oninput='calcFee(this)' type='number' placeholder='List Price in KR' min='0' max='%d' step='1'></div><div id='cPostSaleBtn' class='pItemButton' style='display:none;background-color:#2b2b2b'>Not enough KR</div><div id='postSaleBtn' class='pItemButton' %s",
                   SALE_MAX, block ? "style='background-color:#2b2b2b'" : "");
        if (!block) sb_appendf(&sb, "onclick='listItem(%d,%s,%d)'", p->skin, p->skin_id, p->uid);
        sb_appendf(&sb, "onmouseover='SOUND.play(&quot;tick_0&quot;,0.1)'>%s</div>", block ? block : "List Item");
    } else if (blackm) {
        const char *block = !acc ? "Login to Buy" : !il || !illicit_req_ok(il, acc) ? "Access Denied" : NULL;
        const double funds = acc ? acc->funds : 0, price = atof(p->funds), left = funds - price;
        comma_number(a, sizeof(a), funds);
        comma_number(b, sizeof(b), price);
        comma_number(c, sizeof(c), left);
        sb_appendf(&sb, "<div class='pItemPricing'><div class='priceP'>%s <span style='color:#fff'>KR</span></div><div class='priceP'>-%s <span style='color:#fff'>KR</span></div><div class='pricePF' style='%s'>%s <span style='color:#fff'>KR</span></div></div>",
                   a, b, left < 0 ? "color:#ed4242" : "", c);
        sb_appendf(&sb, "<div class='pItemButton' style='%s' ", block ? "background-color:#2b2b2b" : left < 0 ? "background-color:#ed4242" : "");
        if (!block && left >= 0) sb_appendf(&sb, "onclick='buyIllicit(%d)'onmouseover='SOUND.play(&quot;tick_0&quot;,0.1)'", p->skin);
        sb_appendf(&sb, ">%s</div>", block ? block : left < 0 ? "Can't Afford" : "Purchase");
    } else if (!strcmp(p->page, "sales")) {
        comma_number(a, sizeof(a), atof(p->funds));
        sb_appendf(&sb, "<div class='pItemPricing'>%s <span style='color:rgba(255,255,255,0.4)'>KR</span></div><div class='pItemButton' onclick='unlistItem(%d,%d,%d)' onmouseover='SOUND.play(&quot;tick_0&quot;,0.1)'>Unlist</div>", a, p->skin, p->market_id, p->uid);
    }
    sb_append(&sb, "</div>");
    char *html = sb_finish(&sb);
    set_html("popupContent", html);
    free(html);
}

// la(id): drop a card after a sale / unlisting
static void remove_card(const char *page, const int uid) {
    char id[64];
    snprintf(id, sizeof(id), "itemCard%s_%d", page, uid);
    const HtmlElem e = el(id);
    if (e) html_dom_remove(e);
}

// filterPopup / sortPopup: the market search and inventory sort panels
static void market_filter_popup(const int sort) {
    const char *pre = sort ? "Sort" : "Filt";
    reset_pops();
    set_display(sort ? "sortPop" : "filterPop", "block");
    const HtmlElem pop = el(sort ? "sortPop" : "filterPop");
    if (!pop || html_dom_first_child(pop)) return; // built once (Be / ze)
    StrBuf sb = {0};
    if (!sort) sb_append(&sb, "<div class='filterName'>Sort</div><div id='orderBySel'><select id='sortFilt'><option value='0'>Highest Price</option><option value='1' selected>Lowest Price</option></select></div>");
    sb_appendf(&sb, "<div class='filterName'>Season</div><div id='orderBySel'><select id='sea%s'><option value='0'>All Seasons</option>", pre);
    for (int s = 1; s <= g_viewer_current_season; s++) sb_appendf(&sb, "<option>%d</option>", s);
    sb_append(&sb, "</select></div>");
    if (sort) sb_append(&sb, "<div class='filterName' style='position: relative;display: block;width: 100%;'>Only Show Missing<label class='switchsml'><input type='checkbox' id='onlyMissing'><span class='sliderSml2'></span></label></div>");
    const char *nav = sort ? "sort" : "filter";
    sb_appendf(&sb, "<input type='search' id='%s' placeholder='Keyword: Sniper, Plasma, Wings' /><div class='filterNav' id='%sNav_0' onclick='switch%s(0)' style='color:#faa332'>Rarity</div><div class='filterNav' id='%sNav_1' onclick='switch%s(1)'>Weapons</div><div class='filterNav' id='%sNav_2' onclick='switch%s(2)'>Other</div><div id='%sTog' onclick='toggle%ss()'>Toggle</div>",
               sort ? "invSortTxt" : "marketFiltTxt", nav, sort ? "Sort" : "Filter", nav, sort ? "Sort" : "Filter", nav, sort ? "Sort" : "Filter", sort ? "sort" : "filter", sort ? "Sort" : "Filter");
    sb_appendf(&sb, "<div class='filterDiv' id='%sDiv_0'>", nav);
    for (int a = g_viewer_rarity_count - 1; a >= 0; --a) {
        sb_appendf(&sb, "<div class='raritySel%s' style='color:%s'>%s<label class='switchsml'><input type='checkbox' onclick='' id='rar%s%d' checked><span class='sliderSml2'></span></label></div>",
                   a == 6 ? " rainbowText" : "", g_viewer_rarities[a].color, g_viewer_rarities[a].name, pre, a);
    }
    sb_appendf(&sb, "</div><div class='filterDiv' id='%sDiv_1' style='display:none;'>", nav);
    for (int a = 0; a < WEAPON_COUNT; ++a) {
        if (!g_weapons[a] || g_weapons[a]->melee || a == 17) continue; // melee, Bomb (type 3)
        sb_appendf(&sb, "<div class='wepSel' style='color:#fff'>%s<label class='switchsml'><input type='checkbox' onclick='' id='wep%s%d' checked><span class='sliderSml2'></span></label></div>", g_weapons[a]->name, pre, a);
    }
    sb_appendf(&sb, "</div><div class='filterDiv' id='%sDiv_2' style='display:none;'>", nav);
    for (int a = 1; a < g_viewer_type_count; ++a) {
        char name[32];
        snprintf(name, sizeof(name), "%s", g_viewer_types[a]);
        char *slash = strchr(name, '/');
        if (slash) *slash = 0;
        name[0] = (char) toupper((unsigned char) name[0]);
        sb_appendf(&sb, "<div class='otherSel' style='color:#fff'>%s<label class='switchsml'><input type='checkbox' onclick='' id='type%s%d' checked><span class='sliderSml2'></span></label></div>", name, pre, a);
    }
    sb_appendf(&sb, "</div><div id='filterRBtn' onclick='run%s()'>%s</div><div id='resetFBtn' onclick='reset%s()'>Reset</div>", sort ? "Sort" : "Filter", sort ? "Sort" : "Search", sort ? "Sort" : "Filter");
    char *html = sb_finish(&sb);
    html_dom_set_html(pop, html);
    free(html);
}

static int g_filter_n, g_sort_n; // Ne / We: the latest request (stale replies are dropped)

// runFilter / runSort: io.send("fil"|"srt", n, text, rarities{}, weapons{}, types{}, season, order|missing)
static void market_run_filter(const int sort) {
    const char *pre = sort ? "Sort" : "Filt";
    char text[128] = "", id[32];
    const HtmlElem in = el(sort ? "invSortTxt" : "marketFiltTxt");
    if (in) html_dom_get_value(in, text, sizeof(text));
    cJSON *rar = cJSON_CreateObject(), *wep = cJSON_CreateObject(), *typ = cJSON_CreateObject();
    for (int r = 0; r < g_viewer_rarity_count; ++r) {
        snprintf(id, sizeof(id), "rar%s%d", pre, r);
        const HtmlElem e = el(id);
        char key[8];
        snprintf(key, sizeof(key), "%d", r);
        if (e && html_dom_get_checked(e)) cJSON_AddNumberToObject(rar, key, 1);
    }
    for (int r = 0; r < WEAPON_COUNT; ++r) {
        snprintf(id, sizeof(id), "wep%s%d", pre, r);
        const HtmlElem e = el(id);
        char key[8];
        snprintf(key, sizeof(key), "%d", r + 1);
        if (e && html_dom_get_checked(e)) cJSON_AddNumberToObject(wep, key, 1);
    }
    for (int r = 1; r < g_viewer_type_count; ++r) {
        snprintf(id, sizeof(id), "type%s%d", pre, r);
        const HtmlElem e = el(id);
        char key[8];
        snprintf(key, sizeof(key), "%d", r);
        if (e && html_dom_get_checked(e)) cJSON_AddNumberToObject(typ, key, 1);
    }
    char season[16] = "0", order[16] = "1";
    snprintf(id, sizeof(id), "sea%s", pre);
    const HtmlElem sea = el(id);
    if (sea) html_dom_get_value(sea, season, sizeof(season));
    cJSON *f = cJSON_CreateArray();
    cJSON_AddItemToArray(f, cJSON_CreateString(sort ? "srt" : "fil"));
    cJSON_AddItemToArray(f, cJSON_CreateNumber(sort ? ++g_sort_n : ++g_filter_n));
    cJSON_AddItemToArray(f, cJSON_CreateString(text));
    cJSON_AddItemToArray(f, rar);
    cJSON_AddItemToArray(f, wep);
    cJSON_AddItemToArray(f, typ);
    cJSON_AddItemToArray(f, cJSON_CreateString(season));
    if (sort) {
        const HtmlElem missing = el("onlyMissing");
        cJSON_AddItemToArray(f, cJSON_CreateBool(missing && html_dom_get_checked(missing)));
    } else {
        const HtmlElem o = el("sortFilt");
        if (o) html_dom_get_value(o, order, sizeof(order));
        cJSON_AddItemToArray(f, cJSON_CreateString(order));
    }
    io_wait("Searching");
    hub_send(f);
}

static void market_reset_filter(const int sort) {
    const char *pre = sort ? "Sort" : "Filt";
    char sel[48];
    HtmlElem found[64];
    snprintf(sel, sizeof(sel), "%s input[type=checkbox]", sort ? "#sortPop" : "#filterPop");
    const int n = html_dom_query_all(0, sel, found, 64);
    for (int i = 0; i < n; i++) {
        char id[32] = "";
        html_dom_get_attr(found[i], "id", id, sizeof(id));
        html_dom_set_checked(found[i], strcmp(id, "onlyMissing") != 0);
    }
    const HtmlElem in = el(sort ? "invSortTxt" : "marketFiltTxt");
    if (in) html_dom_set_value(in, "");
    snprintf(sel, sizeof(sel), "sea%s", pre);
    if (el(sel)) html_dom_set_value(el(sel), "0");
    if (!sort && el("sortFilt")) html_dom_set_value(el("sortFilt"), "1");
    market_run_filter(sort);
}

// switchFilter / switchSort and toggleFilters / toggleSorts
static int g_filter_tab, g_sort_tab;
static void market_switch_filter(const int sort, const int tab) {
    const char *nav = sort ? "sort" : "filter";
    if (sort) g_sort_tab = tab;
    else g_filter_tab = tab;
    for (int i = 0; i < 3; i++) {
        char id[32];
        snprintf(id, sizeof(id), "%sDiv_%d", nav, i);
        set_display(id, i == tab ? "block" : "none");
        snprintf(id, sizeof(id), "%sNav_%d", nav, i);
        set_style(id, "color", i == tab ? "#faa332" : "#fff");
    }
}

static void market_toggle_filters(const int sort) {
    char sel[48];
    snprintf(sel, sizeof(sel), "#%sDiv_%d input", sort ? "sort" : "filter", sort ? g_sort_tab : g_filter_tab);
    HtmlElem found[64];
    const int n = html_dom_query_all(0, sel, found, 64);
    for (int i = 0; i < n; i++) html_dom_set_checked(found[i], !html_dom_get_checked(found[i]));
}

// The market / clan / activity reply frames (handle_reply_frame); 1 = handled
static int hub_market_frame(const char *label, const cJSON *frame) {
    const cJSON *a1 = cJSON_GetArrayItem(frame, 1), *a2 = cJSON_GetArrayItem(frame, 2), *a3 = cJSON_GetArrayItem(frame, 3);
    if (!strcmp(label, "po")) { // io_po(kind, error, text): the brain's generic result popup
        const char *kind = cJSON_GetStringValue(a1);
        const int ok = kind && !strcmp(kind, "SUCCESS");
        io_po(ok, ok ? cJSON_GetStringValue(a3) : cJSON_GetStringValue(a2));
    } else if (!strcmp(label, "uf")) {
        hub_render_activity(a1);
    } else if (!strcmp(label, "psd")) {
        hub_render_sales(a1);
    } else if (!strcmp(label, "gd")) {
        hub_render_item_graph(a1);
    } else if (!strcmp(label, "cln")) { // clanRSr(msg, ok)
        io_po(cJSON_IsTrue(a2) || (cJSON_IsNumber(a2) && a2->valuedouble), cJSON_GetStringValue(a1));
    } else if (!strcmp(label, "br")) { // io_br(res, uid)
        const int ok = cJSON_IsObject(a1);
        io_po(ok, ok ? NULL : "Item Unavailable");
        remove_card("market", (int) cJSON_GetNumberValue(a2));
        if (ok) hub_set_funds(jnum(a1, "f"));
    } else if (!strcmp(label, "lir")) { // io_lir(res, uid)
        const cJSON *resp = cJSON_GetObjectItemCaseSensitive(a1, "response");
        if (cJSON_IsString(resp) && !strcmp(resp->valuestring, "MAX")) {
            io_po(0, "Max Items on Sale");
        } else if (cJSON_GetObjectItemCaseSensitive(a1, "cnt")) {
            if (cJSON_GetObjectItemCaseSensitive(a1, "funds")) hub_set_funds(jnum(a1, "funds"));
            const int uid = (int) cJSON_GetNumberValue(a2), cnt = (int) jnum(a1, "cnt");
            char id[32], n[16];
            snprintf(id, sizeof(id), "invCnt%d", uid);
            snprintf(n, sizeof(n), "%d", cnt);
            if (cnt) set_html(id, n);
            else remove_card("inventory", uid);
            io_po(1, NULL);
        } else {
            io_po(0, NULL);
        }
    } else if (!strcmp(label, "ulir")) { // io_ulir(res, uid, err)
        const char *res = cJSON_GetStringValue(a1);
        if (res && !strcmp(res, "SOLD")) io_po(0, "Item already Sold");
        else if (res && !strcmp(res, "SUCCESS")) io_po(1, NULL);
        else io_po(0, cJSON_GetStringValue(a3));
        if (res && (!strcmp(res, "SOLD") || !strcmp(res, "SUCCESS"))) remove_card("sales", (int) cJSON_GetNumberValue(a2));
    } else if (!strcmp(label, "qsr")) { // io_qsr({f, c, i})
        if (cJSON_IsObject(a1)) {
            if (cJSON_GetObjectItemCaseSensitive(a1, "f")) hub_set_funds(jnum(a1, "f"));
            const int uid = (int) jnum(a1, "i"), cnt = (int) jnum(a1, "c");
            char id[32], n[16];
            snprintf(id, sizeof(id), "invCnt%d", uid);
            snprintf(n, sizeof(n), "%d", cnt);
            if (cnt) set_html(id, n);
            else remove_card("inventory", uid);
            io_po(1, NULL);
        } else {
            io_po(0, "Unable to sell");
        }
    } else if (!strcmp(label, "fr") || !strcmp(label, "str")) { // io_fr / Ue(n, rows)
        const int sort = !strcmp(label, "str");
        if ((int) cJSON_GetNumberValue(a1) != (sort ? g_sort_n : g_filter_n)) return 1;
        if (!cJSON_IsArray(a2)) {
            io_po(0, STR_MARKET_NONE);
            return 1;
        }
        update_window("market", sort ? "inventory" : "market", NULL, NULL, NULL, 1);
        show_popup_none();
        g_market_no_sort = !sort;
        io_0("market", sort ? "inventory" : "market", a2, NULL);
    } else {
        return 0;
    }
    return 1;
}

// io_0(e, a, t, ...): one page's data arrived
static void io_0(const char *e, const char *a, const cJSON *t, const cJSON *extra) {
    char key[HUB_KEY_MAX * 2];
    snprintf(key, sizeof(key), "%s_%s", e ? e : "null", a && *a ? a : "null");
    if (g_hub.has_s && strcmp(g_hub.S, key) != 0) return;
    if (!e) return;
    const int has_t = t && !cJSON_IsNull(t);
    hide_all(0);
    set_display("loadMessage", "none");
    const int blackm = a && !strcmp(a, "blackm");
    const HtmlElem body = html_dom_body();
    if (body) html_dom_set_class(body, "darkBG", blackm);
    const HtmlElem title = el("navTitle");
    if (title) html_dom_set_class(title, "glitch", blackm);
    const HtmlElem root = html_dom_root();
    if (root) html_dom_set_class(root, "darkBG", blackm);

    if (!strcmp(e, "profile")) {
        // a guest server has no profile row: nothing is shown
        if (has_t && cJSON_IsObject(t)) hub_render_profile(t, extra);
        set_display("pTab_reports", "none");
        set_display("pTab_inputs", "none");
    } else if (!strcmp(e, "search")) {
        set_display("searchHolder", "block");
        if (has_t && cJSON_IsArray(t)) hub_render_search(t);
    } else if (!strcmp(e, "mods")) {
        set_display("modsHolder", "block");
        char *html = list_gen(&g_hub.mods, 1);
        set_html("modList", html);
        free(html);
    } else if (!strcmp(e, "mod") || !strcmp(e, "map")) {
        const int is_mod = !strcmp(e, "mod");
        side_boxes();
        set_display(is_mod ? "modHolder" : "mapHolder", "block");
        hub_render_creation(is_mod, has_t ? t : NULL);
    } else if (!strcmp(e, "maps")) {
        set_display("mapsHolder", "block");
        char *html = list_gen(&g_hub.maps, 0);
        set_html("mapList", html);
        free(html);
    } else if (!strcmp(e, "leaders")) {
        side_boxes();
        set_display("leaderHolder", "block");
        hub_render_leaders(a, has_t ? t : NULL); // no leaderboard rows offline: an empty list
    } else if (!strcmp(e, "market")) {
        set_display("marketHolder", "block");
        const int inventory = a && !strcmp(a, "inventory");
        set_display("invSortB", inventory ? "inline-block" : "none");
        set_display("marketSearchB", a && !strcmp(a, "market") ? "inline-block" : "none");
        set_display("invValue", inventory ? "inline-block" : "none");
        set_display("invTrack", inventory ? "inline-block" : "none");
        // no listings / inventory for a guest: social.market.login (K is null)
        if (g_hub.brain) hub_render_market(a, has_t ? t : NULL);
        else set_html("marketList", "<div style='font-size:20px;margin-top:20px;color:#fff'>" STR_MARKET_LOGIN "...</div>");
    } else if (!strcmp(e, "itemsales") && g_hub.brain) {
        hub_render_itemsales(has_t ? t : NULL, cJSON_GetArrayItem(extra, 4));
    } else if (!strcmp(e, "itemsales")) {
        side_boxes();
        set_display("itemsalesList", "none");
        if (!has_t) set_html("itemsalesHolder", "<div style='font-size:20px;margin-top:20px;color:#fff'>" STR_MARKET_NONE "...</div>");
        set_display("itemsalesHolder", "block");
    } else if (!strcmp(e, "clan")) {
        set_display("clanHolder", "block");
        if (g_hub.brain && has_t && cJSON_IsObject(t)) hub_render_clan(t);
    } else if (!strcmp(e, "clanwars")) {
        set_display("clanwarsHolder", "block");
        clanwars_show();
    } else if (!strcmp(e, "feed")) {
        set_display("feedHolder", "block");
        set_display("postEntryC", g_hub.logged_in ? "inline-block" : "none"); // acc ? inline-block : none
        side_boxes();
        g_hub.feed_switching = 0;
        hub_render_feed(has_t ? t : NULL);
    } else if (!strcmp(e, "tourney")) {
        set_display("tourneyHolder", "block");
        set_html("tourneyList", "<div>Tourney Results Coming soon</div>");
    }
}

// ------------------------------------------------------------------ hub chat (chatHolder)
// Profile pictures (user-assets.krunker.io/p<id>/profile.png) end on their favicon.png
// onerror fallback, as in side_boxes.
static cJSON *g_chat_list; // we: the open thread, newest first (owned)
static int g_chat_peer;    // ve.id
static int g_chat_pending; // he != null
static char g_chat_pending_txt[320];

// the signed-in sidebar: clan chat, trade chat, then the conversations ("chtb")
static void chat_sidebar(const AccountInfo *acc) {
    set_display("chatHolder", "block");
    StrBuf sb = {0};
    if (acc->clan[0]) {
        sb_append(&sb, "<div class='chatBlobI' data-tip='[");
        sb_append_esc(&sb, acc->clan);
        sb_append(&sb, "] Clan Chat'  onmouseover='createTip(this, 1)' onmouseout='cancelTip(this, 1)'><img class='chatBlobImg' onclick='startChat(0,1,&quot;[");
        sb_append_esc(&sb, acc->clan);
        sb_append(&sb, "] Clan Chat&quot;,this)' src='/img/crossed_swords_0.svg'/></div>");
    }
    sb_append(&sb, "<div class='chatBlobI' data-tip='Trade Chat'  onmouseover='createTip(this, 1)' onmouseout='cancelTip(this, 1)'><img class='chatBlobImg' onclick='startChat(0,2,&quot;Trade Chat&quot;,this)' src='/img/shaking_hands.svg'/></div><div class='chatListSep'></div><div id='chatBubHold'><div class='lds-ring' style='margin-top:10px;opacity:0.5'><div></div><div></div><div></div><div></div></div></div>");
    char *html = sb_finish(&sb);
    set_html("chatScroller", html);
    free(html);
    cJSON *f = cJSON_CreateArray();
    cJSON_AddItemToArray(f, cJSON_CreateString("chtb"));
    hub_send(f);
}

// io_xhtb(rows): the conversation avatars, unread count badges
static void io_xhtb(const cJSON *rows) {
    if (!el("chatBubHold")) return;
    StrBuf sb = {0};
    const cJSON *r;
    cJSON_ArrayForEach(r, rows) {
        const int cht = (int) jnum(r, "cht"), fid = (int) jnum(r, "fid");
        const char *fna = jstr(r, "fna") ? jstr(r, "fna") : "";
        sb_appendf(&sb, "<div class='chatBlobI' style='opacity:%s'  data-tip='", cht > 0 ? "1" : "0.7");
        sb_append_esc(&sb, fna);
        sb_append(&sb, "' onmouseover='createTip(this, 1)' onmouseout='cancelTip(this, 1)'>");
        if (cht) sb_appendf(&sb, "<div class='msgCount' id='msgCnt%d'>%d</div>", fid, cht);
        sb_appendf(&sb, "<img class='chatBlobImg' onclick='startChat(%d,0,&quot;", fid);
        sb_append_esc(&sb, fna);
        sb_append(&sb, "&quot;,this,1)'onerror='this.onerror=null;this.src=\"./img/favicon.png\";' src='./img/favicon.png'/></div>");
    }
    if (!cJSON_GetArraySize(rows)) sb_append(&sb, "<div class='chatBlobI'><img class='chatBlobImg'/></div>");
    char *html = sb_finish(&sb);
    set_html("chatBubHold", html);
    free(html);
}

// updateChatList(): oldest at the top; same sender within a minute folds into one block
static void chat_update_list(void) {
    StrBuf sb = {0};
    const char *prev_name = NULL;
    double prev_tim = 0;
    for (int i = cJSON_GetArraySize(g_chat_list) - 1; i >= 0; --i) {
        const cJSON *m = cJSON_GetArrayItem(g_chat_list, i);
        const char *fna = jstr(m, "fna") ? jstr(m, "fna") : "";
        const double tim = jnum(m, "tim");
        if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(m, "sys"))) {
            sb_append(&sb, "<div class='sysChatMsg'>");
            sb_append_esc(&sb, jstr(m, "txt"));
            sb_append(&sb, "</div>");
        } else if (prev_name && !strcmp(fna, prev_name) && fabs(tim - prev_tim) <= 60000) {
            sb_append(&sb, "<div class='chatItemH' style='margin-top:0px;padding-top:0px'><div class='chatItInfo' style='margin-bottom:0px;margin-left:82px;margin-top:-15px'>");
            sb_append_served(&sb, jstr(m, "txt"));
            sb_append(&sb, "</div></div>");
        } else {
            char ago[48];
            // v3.1.1: getReadableTime(tim), an age in ms; kn_brain's chtl sends epoch ms
            readable_time(ago, sizeof(ago), tim > 1e12 ? fmax(0, now_epoch_ms() - tim) : tim);
            sb_append(&sb, "<div class='chatItemH' style='margin-bottom:12px'><img src='./img/favicon.png' class='chatItPFP'  onerror='this.onerror=null;this.src=\"./img/favicon.png\";' /><div class='chatItInfo'><b style='color:#fff'>");
            sb_append_esc(&sb, fna);
            sb_appendf(&sb, "</b><span style='color:rgba(255,255,255,0.4);font-size:14px;margin-left:10px'>%s ago</span><div style='height:6px'></div>", ago);
            sb_append_served(&sb, jstr(m, "txt"));
            sb_append(&sb, "</div></div>");
        }
        prev_name = fna;
        prev_tim = tim;
    }
    char *html = sb_finish(&sb);
    set_html("chatBoxFill", html);
    free(html);
    const HtmlElem fill = el("chatBoxFill");
    if (fill) html_dom_scroll_to_bottom(fill);
}

// io_chtl(rows)
static void io_chtl(const cJSON *rows) {
    cJSON_Delete(g_chat_list);
    g_chat_list = cJSON_IsArray(rows) ? cJSON_Duplicate(rows, 1) : cJSON_CreateArray();
    if (cJSON_GetArraySize(g_chat_list)) chat_update_list();
    else set_html("chatBoxFill", "");
}

// io_chf(ok, err): the sent message (he), or a system line
static void io_chf(const int ok, const char *err) {
    const AccountInfo *acc = account_get();
    if (!g_chat_list) g_chat_list = cJSON_CreateArray();
    cJSON *m = cJSON_CreateObject();
    if (ok && g_chat_pending && acc) {
        cJSON_AddStringToObject(m, "fna", acc->name);
        cJSON_AddNumberToObject(m, "tim", 0);
        cJSON_AddNumberToObject(m, "fid", acc->id);
        // he.txt is the raw input: escape it once so sb_append_served shows it as typed
        StrBuf t = {0};
        sb_append_esc(&t, g_chat_pending_txt);
        char *txt = sb_finish(&t);
        cJSON_AddStringToObject(m, "txt", txt ? txt : "");
        free(txt);
    } else {
        cJSON_AddTrueToObject(m, "sys");
        cJSON_AddStringToObject(m, "txt", err && *err ? err : "Message not Sent");
    }
    cJSON_InsertItemInArray(g_chat_list, 0, m);
    chat_update_list();
    g_chat_pending = 0;
}

// io_a(error, id, username, data, token): the login reply (account_ui.c keeps the account
// itself: storage keys, level, KR). A guest game server refuses every login.
static void io_a(const char *error) {
    if (!error) {
        // success: acc = new Account(...)
        const AccountInfo *acc = account_get();
        if (!acc) return;
        if (!g_hub.fe) io_po(1, NULL);
        g_hub.fe = 0;
        g_hub.logged_in = 1;
        set_display("profileLogin", "none");
        const HtmlElem img = el("profileImg");
        if (img) {
            char src[64];
            const cJSON *c = acc->stats ? cJSON_GetObjectItem(acc->stats, "c") : NULL;
            snprintf(src, sizeof(src), "./textures/classes/icon_%d.png", cJSON_IsNumber(c) ? c->valueint : 0);
            html_dom_set_attr(img, "src", src);
            html_dom_set_display(img, "inline-block");
        }
        char kr[32];
        comma_number(kr, sizeof(kr), acc->funds);
        StrBuf sb = {0};
        sb_append_esc(&sb, acc->name);
        sb_appendf(&sb, "<div style='height:4px'></div><div id='profileKR'>%s<span style='color:#fff'> KR</span></div>", kr);
        char *html = sb_finish(&sb);
        set_html("profileName", html);
        free(html);
        if (!strcmp(g_hub.v, "feed")) {
            set_display("postEntryC", "inline-block");
            request_page();
        }
        chat_sidebar(acc);
        return;
    }
    g_hub.logged_in = 0;
    set_html("profileName", "");
    set_display("profileLogin", "inline-block");
    set_display("chatHolder", "none");
    if (!strcmp(g_hub.v, "feed")) request_page(); // Qa()
    if (!g_hub.fe) io_po(0, error);
    g_hub.fe = 0;
}

static void handle_reply_frame(const cJSON *frame);

// The "api" stand-in as the brain's real HTTP route: /api/maps|mods?index=&pos=,
// /api/search?type=&val= (publicRoutes.ts); the answer is fed back as an "api" frame.
static void hub_brain_api_done(void *user, const int status, const char *body, const size_t length) {
    (void) length;
    char *path = user;
    cJSON *res = status == 200 ? cJSON_Parse(body) : NULL;
    if (g_hub.entered && cJSON_IsObject(res)) {
        cJSON *frame = cJSON_CreateArray();
        cJSON_AddItemToArray(frame, cJSON_CreateString("api"));
        cJSON_AddItemToArray(frame, cJSON_CreateString(path));
        cJSON_AddItemToArray(frame, res);
        res = NULL;
        handle_reply_frame(frame);
        cJSON_Delete(frame);
    }
    cJSON_Delete(res);
    free(path);
}

static void hub_brain_api(const cJSON *frame) {
    const char *path = cJSON_GetStringValue(cJSON_GetArrayItem(frame, 1));
    const cJSON *params = cJSON_GetArrayItem(frame, 2);
    if (!path) return;
    char url[512], enc[256];
    int n = snprintf(url, sizeof(url), "/api%s?", path);
    const cJSON *p;
    cJSON_ArrayForEach(p, params) {
        if (cJSON_IsString(p)) backend_url_encode(enc, sizeof(enc), p->valuestring);
        else js_num(enc, sizeof(enc), cJSON_GetNumberValue(p));
        n += snprintf(url + n, sizeof(url) - (size_t) n, "%s=%s&", p->string, enc);
        if ((size_t) n >= sizeof(url)) return;
    }
    const AccountInfo *acc = g_hub.logged_in ? account_get() : NULL;
    if (acc) snprintf(url + n, sizeof(url) - (size_t) n, "accountId=%d", acc->id);
    char *copy = strdup(path);
    if (copy && !backend_http_get(url, hub_brain_api_done, copy)) free(copy);
}

// Social socket frames (backend.h listener, main thread from backend_poll)
static void hub_on_social(void *user, const cJSON *frame) {
    (void) user;
    if (!g_hub.entered) return;
    const char *label = cJSON_GetStringValue(cJSON_GetArrayItem(frame, 0));
    if (!label || label[0] == '_') return; // __open / __close: social_tick follows the socket state
    handle_reply_frame(frame);
}

static void handle_reply_frame(const cJSON *frame) {
    const char *label = cJSON_GetStringValue(cJSON_GetArrayItem(frame, 0));
    if (!label) return;
    if (!strcmp(label, "0")) {
        const char *e = cJSON_GetStringValue(cJSON_GetArrayItem(frame, 1));
        const char *a = cJSON_GetStringValue(cJSON_GetArrayItem(frame, 2));
        io_0(e, a, cJSON_GetArrayItem(frame, 3), frame); // the whole frame: r, l, c, f follow t
    } else if (!strcmp(label, "a")) {
        io_a(cJSON_GetStringValue(cJSON_GetArrayItem(frame, 1)));
    } else if (!strcmp(label, "spR")) { // io_spR(rows): the player search results
        const cJSON *rows = cJSON_GetArrayItem(frame, 1);
        if (rows && !cJSON_IsNull(rows) && !cJSON_IsFalse(rows)) {
            update_window("search", NULL, NULL, NULL, NULL, 1);
            show_popup_none();
            io_0("search", NULL, rows, frame);
        } else {
            io_po(0, STR_MARKET_NONE);
        }
    } else if (!strcmp(label, "chtb")) {
        io_xhtb(cJSON_GetArrayItem(frame, 1));
    } else if (!strcmp(label, "chtl")) {
        io_chtl(cJSON_GetArrayItem(frame, 1));
    } else if (!strcmp(label, "chf")) {
        const cJSON *ok = cJSON_GetArrayItem(frame, 1);
        io_chf(cJSON_IsTrue(ok), cJSON_GetStringValue(cJSON_GetArrayItem(frame, 2)));
    } else if (!strcmp(label, "api")) {
        const char *path = cJSON_GetStringValue(cJSON_GetArrayItem(frame, 1));
        const cJSON *res = cJSON_GetArrayItem(frame, 2);
        if (!path || !cJSON_IsObject(res)) return;
        const cJSON *data = cJSON_GetObjectItemCaseSensitive(res, "data");
        const int index = (int) cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(res, "index"));
        const cJSON *error = cJSON_GetObjectItemCaseSensitive(res, "error");
        if (!strcmp(path, "/maps")) list_loaded(0, data, index, error);       // Ea
        else if (!strcmp(path, "/mods")) list_loaded(1, data, index, error);  // Ha
        else if (!strcmp(path, "/search")) list_search_resp(index == 3, data); // za
    }
    else if (g_hub.brain && hub_market_frame(label, frame)) {
        // market / clan / activity / sales replies (hub_market_frame)
    }
    else if (!strcmp(label, "psr")) { // socPostRes(e, a, t)
        const cJSON *res = cJSON_GetArrayItem(frame, 1);
        const char *err = cJSON_GetStringValue(cJSON_GetArrayItem(frame, 2));
        const int kind = (int) cJSON_GetNumberValue(cJSON_GetArrayItem(frame, 3));
        if (kind == 1) { // gps: one post + comments
            const cJSON *post = cJSON_GetObjectItem(res, "p");
            StrBuf sb = {0};
            if (post) {
                sb_append(&sb, "<div style='height:10px'></div>");
                append_post(&sb, post, 1);
                sb_appendf(&sb, "<div class='replyHolder'><img src='./img/favicon.png' class='replyPfp'><input class='replyInput' placeholder='Reply' id='replyInput'></input><div class='replyBtn' onclick='postReply(%d)' id='replyBtn'>Reply</div></div>", (int) jnum(post, "pid"));
                const cJSON *c;
                sb_append(&sb, "<div style='margin-top:10px' id='comHolderH'>");
                cJSON_ArrayForEach(c, cJSON_GetObjectItem(res, "c")) {
                    sb_append(&sb, "<div class='commHolder'><img class='replyPfp' src='./img/favicon.png' /><div class='comTxtHolder'><div style='margin-bottom:6px'>");
                    append_post_author(&sb, c);
                    sb_append(&sb, "</span></div>");
                    if (jstr(c, "txt")) sb_append_served(&sb, jstr(c, "txt"));
                    sb_append(&sb, "</div></div>");
                }
                sb_append(&sb, "</div>");
            } else {
                sb_append_esc(&sb, err ? err : "Post not found.");
            }
            char *html = sb_finish(&sb);
            set_html("singlePostH", html);
            free(html);
        } else if (kind == 2) { // com ack: reload the post
            set_html("replyBtn", "Reply");
            if (res && !cJSON_IsNull(res)) request_page();
            else io_po(0, err);
        } else { // ps ack
            g_hub.posting = 0;
            set_html("postButton", "Post");
            if (res && !cJSON_IsNull(res)) {
                const HtmlElem input = el("postInput");
                if (input) html_dom_set_value(input, "");
                request_page();
            } else {
                io_po(0, err);
            }
        }
    }
    // "psd", "uf": the profile sales graph and activity tab (no graph renderer)
}

static void handle_reply(const char *json) {
    cJSON *frames = cJSON_Parse(json);
    const cJSON *frame;
    cJSON_ArrayForEach(frame, frames) {
        if (cJSON_IsArray(frame)) handle_reply_frame(frame);
    }
    cJSON_Delete(frames);
}

// The game is frozen while the hub is shown (app_mode.c), so its packet queue is not
// drained: pull the social replies out of it here.
static void poll_replies(Client *client) {
    NetPacket *found = NULL;
    size_t found_count = 0;
    pthread_mutex_lock(&client->net_lock);
    size_t keep = 0;
    for (size_t i = 0; i < client->net_packet_count; i++) {
        NetPacket *p = &client->net_packets[i];
        if (p->type == NET_PACKET_SOCIAL_REPLY) {
            NetPacket *grown = realloc(found, (found_count + 1) * sizeof(NetPacket));
            if (grown) {
                found = grown;
                found[found_count++] = *p;
            }
            continue;
        }
        if (keep != i) client->net_packets[keep] = *p;
        keep++;
    }
    client->net_packet_count = keep;
    pthread_mutex_unlock(&client->net_lock);

    for (size_t i = 0; i < found_count; i++) {
        char *msg = NULL;
        uint32_t id = 0;
        if (social_assembly_push(&g_hub.assembly, found[i].payload, found[i].length, &msg, &id) == 1) {
            handle_reply(msg);
            free(msg);
        }
    }
    free(found);
}

// ------------------------------------------------------------------ handlers

#define H(name) static int name(void *user, const HtmlEvent *ev, const HtmlArg *args, const int argc)
#define UNUSED (void) user; (void) ev; (void) args; (void) argc

static const char *arg_s(const HtmlEvent *ev, const HtmlArg *args, const int argc, const int i) {
    if (i >= argc) return NULL;
    const HtmlArg *a = &args[i];
    if (a->kind == HTML_ARG_STR) return a->str ? a->str : "";
    if (a->kind == HTML_ARG_THIS_VALUE) return ev && ev->value ? ev->value : "";
    return NULL;
}

static double arg_n(const HtmlArg *args, const int argc, const int i, const double fallback) {
    if (i >= argc) return fallback;
    if (args[i].kind == HTML_ARG_NUM || args[i].kind == HTML_ARG_BOOL) return args[i].num;
    if (args[i].kind == HTML_ARG_STR && args[i].str) return atof(args[i].str);
    return fallback;
}

H(h_update_window) {
    UNUSED;
    update_window(arg_s(ev, args, argc, 0), arg_s(ev, args, argc, 1), arg_s(ev, args, argc, 2), arg_s(ev, args, argc, 3),
                  arg_s(ev, args, argc, 4), argc > 5 && arg_n(args, argc, 5, 0) != 0);
    return 1;
}

H(h_sound_play) {
    (void) user; (void) ev;
    const char *name = arg_s(ev, args, argc, 0);
    if (name) hub_sound(name, (float) arg_n(args, argc, 1, 1.0));
    return 1;
}

// openLink(url, target = "_self") / openURL(url) (window.open(url, "_blank"))
H(h_open_link) {
    UNUSED;
    const char *url = arg_s(ev, args, argc, 0);
    if (!url) return 1;
    const char *name = html_ui_current_handler();
    const char *target = arg_s(ev, args, argc, 1);
    const int new_tab = name && strcmp(name, "openLink") != 0 ? 1 : (target && !strcmp(target, "_blank"));
    if (!app_route_url_target(url, new_tab)) ui_open_external(url); // other sites: browser tab
    return 1;
}

H(h_location_reload) {
    UNUSED;
#ifdef __EMSCRIPTEN__
    EM_ASM({ try { location.reload(); } catch (e) {} });
#else
    app_set_mode(g_hub.client, APP_MODE_HUB, g_hub.query); // a fresh document
#endif
    return 1;
}

H(h_login_popup) {
    UNUSED;
    reset_pops();
    set_display("loginPop", "block");
    char saved[128] = "";
    if (html_storage_get("krunker_username", saved, sizeof(saved)) < 0) saved[0] = 0;
    StrBuf sb = {0};
    sb_append(&sb, "<input id='accName' type='text' placeholder='Enter Username' class='accountInput' style='margin-top:0' value='");
    sb_append_esc(&sb, saved);
    sb_append(&sb, "'></input><input id='accPass' type='password' placeholder='Enter Password' class='accountInput'></input><div id='accResp' style='margin-top:10px;color:#fff;display:none'></div><div class='accountButton' onclick='loginAcc()'>Login</div>");
    char *html = sb_finish(&sb);
    set_html("loginPop", html);
    free(html);
    return 1;
}

H(h_login_acc) {
    UNUSED;
    io_wait("Please Wait...");
    char name[128] = "", pass[128] = "";
    const HtmlElem n = el("accName"), p = el("accPass");
    if (n) html_dom_get_value(n, name, sizeof(name));
    if (p) html_dom_get_value(p, pass, sizeof(pass));
    cJSON *creds = cJSON_CreateArray();
    cJSON_AddItemToArray(creds, cJSON_CreateString(name));
    cJSON_AddItemToArray(creds, cJSON_CreateString(pass));
    cJSON_AddItemToArray(creds, cJSON_CreateNull());
    cJSON *f = cJSON_CreateArray();
    cJSON_AddItemToArray(f, cJSON_CreateString("a"));
    cJSON_AddItemToArray(f, cJSON_CreateNumber(1));
    cJSON_AddItemToArray(f, creds);
    cJSON_AddItemToArray(f, cJSON_CreateTrue()); // captcha token
    hub_send(f);
    return 1;
}

H(h_search_profile) {
    UNUSED;
    // searchProfile(): io.send("sp", value, captcha) -> io_spR (no answer offline: the popup
    // keeps spinning until the backdrop closes it, like Krunker's)
    char value[128] = "";
    const HtmlElem input = el("searchPInput");
    if (!input || html_dom_get_value(input, value, sizeof(value)) <= 0 || !value[0]) return 1;
    io_wait("Searching...");
    cJSON *f = cJSON_CreateArray();
    cJSON_AddItemToArray(f, cJSON_CreateString("sp"));
    cJSON_AddItemToArray(f, cJSON_CreateString(value));
    cJSON_AddItemToArray(f, cJSON_CreateTrue());
    hub_send(f);
    return 1;
}

// onkeyup="if (window.event.keyCode==13) window.searchProfile();" (the native engine
// does not run that statement: the keyup listener on #searchPInput does it)
// startChat(id, kind, name, img, isPlayer): the chat box (kind 1 clan / 2 trade: id 0)
H(h_start_chat) {
    UNUSED;
    const int id = (int) arg_n(args, argc, 0, 0);
    const char *name = arg_s(ev, args, argc, 2);
    const int player = (int) arg_n(args, argc, 4, 0);
    g_chat_peer = id;
    hide_all(1);
    side_boxes();
    char cnt[32];
    snprintf(cnt, sizeof(cnt), "msgCnt%d", id);
    set_display(cnt, "none");
    cJSON_Delete(g_chat_list);
    g_chat_list = cJSON_CreateArray();
    set_html("chatBoxFill", "<div style='width:100%;opacity:0.6;text-align:center;margin-bottom:10px;font-size:25px'><div style='display:inline-block'><div class='lds-ring'><div></div><div></div><div></div><div></div></div></div></div>");
    char src[256] = "./img/favicon.png";
    if (argc > 3 && args[3].kind == HTML_ARG_THIS && ev && ev->target) html_dom_get_attr(ev->target, "src", src, sizeof(src));
    StrBuf sb = {0};
    sb_append(&sb, "<img src='");
    sb_append_esc(&sb, src);
    sb_append(&sb, "' class='chatHeadPFP' />");
    if (player && name) {
        sb_append(&sb, "<a href='/social.html?p=profile&q=");
        sb_append_esc(&sb, name);
        sb_append(&sb, "' style='color:#fff'>");
        sb_append_esc(&sb, name);
        sb_append(&sb, "</a>");
    } else if (name) {
        sb_append_esc(&sb, name);
    }
    char *html = sb_finish(&sb);
    set_html("chatWithHead", html);
    free(html);
    set_display("chatBox", "block");
    cJSON *f = cJSON_CreateArray();
    cJSON_AddItemToArray(f, cJSON_CreateString("ctd"));
    cJSON_AddItemToArray(f, cJSON_CreateNumber(id));
    hub_send(f);
    return 1;
}

// closeChat(): hideAll(); Qa()
H(h_close_chat) {
    UNUSED;
    hide_all(0);
    request_page();
    return 1;
}

// sendChat(): io.send("ict", ve.id, text)
H(h_send_chat) {
    UNUSED;
    char text[320] = "";
    const HtmlElem in = el("chatBoxInput");
    if (in) html_dom_get_value(in, text, sizeof(text));
    if (g_chat_pending || !g_hub.logged_in || !text[0]) return 1;
    cJSON *f = cJSON_CreateArray();
    cJSON_AddItemToArray(f, cJSON_CreateString("ict"));
    cJSON_AddItemToArray(f, cJSON_CreateNumber(g_chat_peer));
    cJSON_AddItemToArray(f, cJSON_CreateString(text));
    hub_send(f);
    g_chat_pending = 1;
    snprintf(g_chat_pending_txt, sizeof(g_chat_pending_txt), "%s", text);
    html_dom_set_value(in, "");
    return 1;
}

static int l_chat_keyup(void *user, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    if (ev && html_key_is_enter(ev->key)) return h_send_chat(user, ev, args, argc);
    return 1;
}

static int l_search_keyup(void *user, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    if (ev && html_key_is_enter(ev->key)) return h_search_profile(user, ev, args, argc);
    return 1;
}

H(h_open_settings) { UNUSED; open_settings(); return 1; }

H(h_set_setting) {
    (void) user;
    const char *key = arg_s(ev, args, argc, 0);
    if (!key || argc < 2) return 1;
    char value[64];
    const HtmlArg *a = &args[1];
    if (a->kind == HTML_ARG_THIS_CHECKED) snprintf(value, sizeof(value), "%s", ev && ev->checked ? "true" : "false");
    else if (a->kind == HTML_ARG_THIS_VALUE) snprintf(value, sizeof(value), "%s", ev && ev->value ? ev->value : "");
    else if (a->kind == HTML_ARG_STR) snprintf(value, sizeof(value), "%s", a->str ? a->str : "");
    else if (a->kind == HTML_ARG_BOOL) snprintf(value, sizeof(value), "%s", a->num != 0 ? "true" : "false");
    else js_num(value, sizeof(value), a->num);
    set_setting(key, value);
    return 1;
}

// delayExecute(key, el, ms): setSetting(key, el.value) after the delay (600 ms default).
typedef struct {
    char key[32];
    HtmlElem elem;
} HubDelayed;
static HubDelayed g_delayed;
static int g_delayed_timer;

static void delayed_fire(void *user) {
    (void) user;
    g_delayed_timer = 0;
    char value[64] = "";
    if (g_delayed.elem) html_dom_get_value(g_delayed.elem, value, sizeof(value));
    set_setting(g_delayed.key, value);
}

H(h_delay_execute) {
    (void) user;
    const char *key = arg_s(ev, args, argc, 0);
    if (!key) return 1;
    snprintf(g_delayed.key, sizeof(g_delayed.key), "%s", key);
    g_delayed.elem = ev ? ev->target : 0;
    if (g_delayed_timer) html_timer_cancel(g_delayed_timer);
    const double ms = arg_n(args, argc, 2, 600);
    g_delayed_timer = html_timer_add(isnan(ms) ? 600 : ms, delayed_fire, NULL);
    return 1;
}

H(h_clear_pops) { UNUSED; reset_pops(); set_display("popupHolder", "none"); return 1; }

// ---- brain: market / clan / profile tab handlers (social.js window.*)

// showPopup(page, skin, preview, label, skinid, funds, count, salestart, player, uid, marketid); no args: close
H(h_show_popup) {
    UNUSED;
    if (!g_hub.brain || argc < 2) {
        show_popup_none();
        return 1;
    }
    const char *page = arg_s(ev, args, argc, 0);
    if (page && !strcmp(page, "blackm")) { // showPopup("blackm", v, preview, me(Pe), v, Pe.funds)
        const int skin = (int) arg_n(args, argc, 1, -1);
        const IllicitSkin *il = illicit_skin(skin);
        const ViewerSkin *pe = hub_skin(skin);
        if (!il || !pe) return 1;
        MarketPop p = {0};
        snprintf(p.page, sizeof(p.page), "blackm");
        p.skin = skin;
        skin_preview(p.preview, sizeof(p.preview), pe);
        snprintf(p.label, sizeof(p.label), "%s", skin_type_label(pe));
        snprintf(p.skin_id, sizeof(p.skin_id), "%d", skin);
        snprintf(p.funds, sizeof(p.funds), "%d", il->funds);
        market_popup(&p);
        return 1;
    }
    // showPopup(page, skin, row): the row of the last market list (hub_render_market)
    const int row = (int) arg_n(args, argc, 2, -1);
    if (row < 0 || row >= g_market_row_count) return 1;
    const MarketRow *m = &g_market_rows[row];
    const ViewerSkin *pe = hub_skin(m->skin);
    if (!pe) return 1;
    const AccountInfo *acc = g_hub.logged_in ? account_get() : NULL;
    MarketPop p = {0};
    snprintf(p.page, sizeof(p.page), "%s", arg_s(ev, args, argc, 0) ? arg_s(ev, args, argc, 0) : g_market_sub);
    p.skin = m->skin;
    skin_preview(p.preview, sizeof(p.preview), pe);
    snprintf(p.label, sizeof(p.label), "%s", skin_type_label(pe));
    snprintf(p.skin_id, sizeof(p.skin_id), "%d", m->skin_id);
    snprintf(p.funds, sizeof(p.funds), "%.0f", m->funds);
    p.count = m->count;
    snprintf(p.salestart, sizeof(p.salestart), "%.0f", m->salestart);
    snprintf(p.player, sizeof(p.player), "%s", m->player[0] ? m->player : acc ? acc->name : "0");
    p.uid = m->uid;
    p.market_id = m->market_id;
    market_popup(&p);
    return 1;
}

H(h_refresh_popup) {
    UNUSED;
    if (g_last_pop.valid) {
        const MarketPop p = g_last_pop;
        market_popup(&p);
    } else {
        show_popup_none();
    }
    return 1;
}

static void send_ints(const char *label, const int n, const double *v) {
    cJSON *f = cJSON_CreateArray();
    cJSON_AddItemToArray(f, cJSON_CreateString(label));
    for (int i = 0; i < n; i++) cJSON_AddItemToArray(f, cJSON_CreateNumber(v[i]));
    hub_send(f);
}

// buyItem(marketid, uid): io.send("pi", ...)
H(h_buy_item) {
    UNUSED;
    const double v[2] = {arg_n(args, argc, 0, 0), arg_n(args, argc, 1, 0)};
    send_ints("pi", 2, v);
    io_wait("Buying Item...");
    return 1;
}

// buyIllicit(skin): io.send("pbm", skin) (kn_brain does not answer "pbm" yet)
H(h_buy_illicit) {
    UNUSED;
    const double v[1] = {arg_n(args, argc, 0, 0)};
    send_ints("pbm", 1, v);
    io_wait("Buying Item...");
    return 1;
}

// listItem(skin, skinid, uid) -> confListPopup -> listConfirmed: io.send("li", skin, skinid, uid, price)
H(h_list_item) {
    UNUSED;
    char price[32] = "";
    const HtmlElem in = el("sellPriceIn");
    if (in) html_dom_get_value(in, price, sizeof(price));
    if (!price[0] || !strcmp(price, "0") || !g_hub.logged_in) return 1;
    const int skin = (int) arg_n(args, argc, 0, 0);
    const ViewerSkin *pa = hub_skin(skin);
    if (!pa) return 1;
    reset_pops();
    set_display("confListPop", "block");
    StrBuf sb = {0};
    sb_appendf(&sb, "<div style='color:#fff;font-size:20px;position:relative;margin-bottom:15px;'>Are you sure you want to list <span style='color:%s'>", rarity_color(pa));
    sb_append_esc(&sb, pa->name);
    sb_appendf(&sb, "</span> for %dKR ?</div><div id='confirmBtn' onclick='listConfirmed(%d, %d,%d, %d)' onmouseover='SOUND.play(&quot;tick_0&quot;,0.1)'>YES</div><div id='declineBtn' onclick='refreshPopup()' onmouseover='SOUND.play(&quot;tick_0&quot;,0.1)'>NO</div>",
               atoi(price), skin, (int) arg_n(args, argc, 1, 0), (int) arg_n(args, argc, 2, 0), atoi(price));
    char *html = sb_finish(&sb);
    set_html("confListPop", html);
    free(html);
    return 1;
}

H(h_list_confirmed) {
    UNUSED;
    const double v[4] = {arg_n(args, argc, 0, 0), arg_n(args, argc, 1, 0), arg_n(args, argc, 2, 0), arg_n(args, argc, 3, 0)};
    send_ints("li", 4, v);
    io_wait("Posting Item...");
    return 1;
}

// unlistItem(skin, marketid, uid): io.send("uli", ...)
H(h_unlist_item) {
    UNUSED;
    const double v[3] = {arg_n(args, argc, 0, 0), arg_n(args, argc, 1, 0), arg_n(args, argc, 2, 0)};
    send_ints("uli", 3, v);
    io_wait("Unlisting Item...");
    return 1;
}

// quickSell(skin, rarity, uid) -> confQSPopup
H(h_quick_sell) {
    UNUSED;
    if (!g_hub.logged_in) {
        io_po(0, "Must be logged in");
        return 1;
    }
    const int skin = (int) arg_n(args, argc, 0, 0), uid = (int) arg_n(args, argc, 2, 0);
    const ViewerSkin *pa = hub_skin(skin);
    if (!pa) return 1;
    reset_pops();
    set_display("confQSPop", "block");
    StrBuf sb = {0};
    sb_appendf(&sb, "<div style='color:#fff;font-size:19px;position:relative;margin-bottom:15px;'>Are you sure you want to Quick Sell <span style='color:%s'>", rarity_color(pa));
    sb_append_esc(&sb, pa->name);
    sb_appendf(&sb, "</span>?</div><div class='quickSellText'>KR<div class='quickSellKR'><span style='color: #FBC02D'>%d</span> KR (each)</div></div><div class='quickSellText'>Amount<select id='quickSellSelect'><option value='0'>1 Item</option><option value='1'>All But 1</option><option value='2'>All</option></select></div><div id='confirmBtn' onclick='sellConfirmed(%d,%d)' onmouseover='SOUND.play(&quot;tick_0&quot;,0.1)'>YES</div><div id='declineBtn' onclick='showPopup()' onmouseover='SOUND.play(&quot;tick_0&quot;,0.1)'>NO</div>",
               pa->rarity >= 0 && pa->rarity < 7 ? k_rarity_sell[pa->rarity] : 1, skin, uid);
    char *html = sb_finish(&sb);
    set_html("confQSPop", html);
    free(html);
    return 1;
}

H(h_sell_confirmed) {
    UNUSED;
    char amount[8] = "0";
    const HtmlElem sel = el("quickSellSelect");
    if (sel) html_dom_get_value(sel, amount, sizeof(amount));
    const double v[3] = {arg_n(args, argc, 0, 0), arg_n(args, argc, 1, 0), atoi(amount)};
    io_wait("Please Wait...");
    send_ints("qsi", 3, v);
    return 1;
}

// calcFee(input): the 5 % list fee line (config.marketFeeBypass 0)
H(h_calc_fee) {
    (void) user; (void) args; (void) argc;
    const HtmlElem in = ev ? ev->target : 0;
    if (!in) return 1;
    char text[32] = "";
    html_dom_get_value(in, text, sizeof(text));
    double a = atof(text);
    if (a < 0) a = 0;
    if (a > SALE_MAX) a = SALE_MAX;
    const double fee = floor(a * 0.05);
    StrBuf sb = {0};
    if (fee > 0) {
        char n[32];
        comma_number(n, sizeof(n), fee);
        sb_appendf(&sb, "<span style='color:rgba(255,255,255,0.7)'>-%s KR</span> List Fee", n);
    } else {
        sb_append(&sb, "No List Fee");
    }
    char *html = sb_finish(&sb);
    set_html("sellPricFee", html);
    free(html);
    const AccountInfo *acc = g_hub.logged_in ? account_get() : NULL;
    if (acc) {
        set_display("postSaleBtn", fee <= acc->funds ? "block" : "none");
        set_display("cPostSaleBtn", fee <= acc->funds ? "none" : "block");
    }
    return 1;
}

static void open_hub_url(const char *prefix, const char *name, int new_tab);
H(h_search_item) { UNUSED; char n[16]; snprintf(n, sizeof(n), "%d", (int) arg_n(args, argc, 0, 0)); open_hub_url("/social.html?p=market&i=", n, 1); return 1; }
H(h_filter_popup) { UNUSED; if (g_hub.brain) market_filter_popup(!strcmp(html_ui_current_handler(), "sortPopup")); return 1; }
H(h_run_filter) { UNUSED; market_run_filter(!strcmp(html_ui_current_handler(), "runSort")); return 1; }
H(h_reset_filter) { UNUSED; market_reset_filter(!strcmp(html_ui_current_handler(), "resetSort")); return 1; }
H(h_switch_filter) { UNUSED; market_switch_filter(!strcmp(html_ui_current_handler(), "switchSort"), (int) arg_n(args, argc, 0, 0)); return 1; }
H(h_toggle_filters) { UNUSED; market_toggle_filters(!strcmp(html_ui_current_handler(), "toggleSorts")); return 1; }

// updateTrades(tab): incoming / outgoing / history
H(h_update_trades) {
    UNUSED;
    const char *tab = arg_s(ev, args, argc, 0);
    static const char *const ids[3] = {"in", "out", "his"};
    for (int i = 0; i < 3; i++) {
        char a[16], b[16];
        snprintf(a, sizeof(a), "tn_%s", ids[i]);
        snprintf(b, sizeof(b), "o_%s", ids[i]);
        const int on = tab && !strcmp(tab, ids[i]);
        set_style(a, "background-color", on ? "rgba(0, 0, 0, 0.5)" : "rgba(0, 0, 0, 0.2)");
        set_display(b, on ? "block" : "none");
    }
    return 1;
}

// fastDeclOffer(id) / declineAllTrades(): io.send("trC", id) / ("trCA")
H(h_decline_offer) {
    UNUSED;
    if (!strcmp(html_ui_current_handler(), "declineAllTrades")) {
        cJSON *f = cJSON_CreateArray();
        cJSON_AddItemToArray(f, cJSON_CreateString("trCA"));
        hub_send(f);
        set_html("o_in", "<div style='margin-bottom:10px;margin-top:-4px;'>Incoming Offers (<span>0</span>)</div><div style='color: rgba(0,0,0,0.6);font-size: 17px;'>None</div>");
        return 0;
    }
    const double v[1] = {arg_n(args, argc, 0, 0)};
    send_ints("trC", 1, v);
    char id[32];
    snprintf(id, sizeof(id), "of_%d", (int) v[0]);
    if (el(id)) html_dom_remove(el(id));
    return 0;
}

// clanRS(1): Clan Discord ("Coming Soon"); clanRS(0, tag): io.send("cln", tag)
H(h_clan_rs) {
    UNUSED;
    if ((int) arg_n(args, argc, 0, 0) == 1) {
        io_po(0, "Coming Soon");
    } else if (!g_hub.logged_in) {
        io_po(0, "Please Login");
    } else {
        io_wait("Please Wait...");
        cJSON *f = cJSON_CreateArray();
        cJSON_AddItemToArray(f, cJSON_CreateString("cln"));
        cJSON_AddItemToArray(f, cJSON_CreateString(arg_s(ev, args, argc, 1) ? arg_s(ev, args, argc, 1) : ""));
        hub_send(f);
    }
    return 1;
}

// openProfileTab(tab): about / activity (io.send("guf", id)) / creations
H(h_open_profile_tab) {
    UNUSED;
    const char *tab = arg_s(ev, args, argc, 0);
    if (!tab) return 1;
    reset_pops();
    set_display("popupHolder", "none");
    HtmlElem found[16];
    int n = html_dom_query_all(0, "div[id^=pTab_]", found, 16);
    for (int i = 0; i < n; i++) {
        char id[48] = "";
        html_dom_get_attr(found[i], "id", id, sizeof(id));
        html_dom_set_class(found[i], "pTabA", !strcmp(id + 5, tab));
    }
    n = html_dom_query_all(0, "div[id^=pTabContent_]", found, 16);
    for (int i = 0; i < n; i++) {
        char id[48] = "";
        html_dom_get_attr(found[i], "id", id, sizeof(id));
        html_dom_set_class(found[i], "selected", !strcmp(id + 12, tab));
    }
    if (!strcmp(tab, "activity") && g_hub.brain && g_hub.profile_name[0]) {
        set_html("pTabContent_activity", "<div class='lds-ring'><div></div><div></div><div></div><div></div></div>");
        const double v[1] = {g_hub.profile_id};
        send_ints("guf", 1, v);
    }
    return 1;
}

// openItemTab(tab): itemsales avg / sold / owners
H(h_open_item_tab) {
    UNUSED;
    const char *tab = arg_s(ev, args, argc, 0);
    if (!tab) return 1;
    HtmlElem found[8];
    int n = html_dom_query_all(0, "div[id^=iTab_]", found, 8);
    for (int i = 0; i < n; i++) {
        char id[48] = "";
        html_dom_get_attr(found[i], "id", id, sizeof(id));
        html_dom_set_class(found[i], "tabANew", !strcmp(id + 5, tab));
    }
    n = html_dom_query_all(0, "div[id^=iTabContent_]", found, 8);
    for (int i = 0; i < n; i++) {
        char id[48] = "";
        html_dom_get_attr(found[i], "id", id, sizeof(id));
        html_dom_set_display(found[i], !strcmp(id + 12, tab) ? "block" : "none");
    }
    return 1;
}

// changeDateRange(el, range, profile): the sales graph range (7/30/90/120 days)
H(h_change_date_range) {
    (void) user;
    static const int days[4] = {7, 30, 90, 120};
    const int range = (int) arg_n(args, argc, 1, 1);
    const HtmlElem e = ev ? ev->target : 0;
    const HtmlElem parent = e ? html_dom_parent(e) : 0;
    int i = 0;
    for (HtmlElem c = parent ? html_dom_first_child(parent) : 0; c; c = html_dom_next(c), i++) {
        char tag[8] = "";
        html_dom_tag(c, tag, sizeof(tag));
        if (strcmp(tag, "a") != 0) {
            i--;
            continue;
        }
        html_dom_set_style(c, "background-color", c == e ? "rgba(0, 0, 0, 0.6)" : "rgba(0, 0, 0, 0.4)");
    }
    if (range < 0 || range > 3 || !g_hub.brain) return 1;
    if (arg_n(args, argc, 2, 0) != 0 && g_hub.profile_name[0]) {
        cJSON *f = cJSON_CreateArray();
        cJSON_AddItemToArray(f, cJSON_CreateString("pst"));
        cJSON_AddItemToArray(f, cJSON_CreateString(g_hub.profile_name));
        cJSON_AddItemToArray(f, cJSON_CreateNumber(days[range]));
        hub_send(f);
    }
    return 1;
}

H(h_maps_switch_tab) { UNUSED; list_switch_tab(0, (int) arg_n(args, argc, 0, 0), 0); return 1; }
H(h_mods_switch_tab) { UNUSED; list_switch_tab(1, (int) arg_n(args, argc, 0, 0), 0); return 1; }
H(h_maps_load_more) { (void) user; (void) args; (void) argc; list_load_more(0, ev ? ev->target : 0); return 1; }
H(h_mods_load_more) { (void) user; (void) args; (void) argc; list_load_more(1, ev ? ev->target : 0); return 1; }

H(h_maps_search) {
    UNUSED;
    char value[128] = "";
    const HtmlElem input = el("searchInput");
    if (input) html_dom_get_value(input, value, sizeof(value));
    api_request("/search", 0, 0, "map", value);
    return 1;
}

H(h_mods_search) {
    UNUSED;
    char value[128] = "";
    const HtmlElem input = el("searchInputMO");
    if (input) html_dom_get_value(input, value, sizeof(value));
    api_request("/search", 0, 0, "mod", value);
    return 1;
}

static void open_hub_url(const char *prefix, const char *name, const int new_tab) {
    char url[512];
    snprintf(url, sizeof(url), "%s%s", prefix, name ? name : "");
    if (!app_route_url_target(url, new_tab)) ui_open_external(url);
}

// openMap/openMod: openLink(url) (same tab); openProfile, playMap, hostMap, playMod: "_blank"
H(h_open_map) { UNUSED; open_hub_url("/social.html?p=map&q=", arg_s(ev, args, argc, 0), 0); return 1; }
H(h_open_mod) { UNUSED; open_hub_url("/social.html?p=mod&q=", arg_s(ev, args, argc, 0), 0); return 1; }
H(h_open_profile) { UNUSED; open_hub_url("/social.html?p=profile&q=", arg_s(ev, args, argc, 0), 1); return 1; }
H(h_play_map) { UNUSED; open_hub_url("/?play=", arg_s(ev, args, argc, 0), 1); return 1; }
H(h_play_mod) { UNUSED; open_hub_url("/?mod=", arg_s(ev, args, argc, 0), 1); return 1; }

// updateMapTab(e): About / Servers on the map page
H(h_update_map_tab) {
    UNUSED;
    g_map_tab = (int) arg_n(args, argc, 0, 0) == 1;
    StrBuf sb = {0};
    append_map_tabs(&sb);
    char *html = sb_finish(&sb);
    set_html("mapTabsP", html);
    free(html);
    set_display("mapDescr", g_map_tab == 1 ? "none" : "block");
    set_display("mapVid", "none");
    set_display("activeGames", g_map_tab == 0 ? "none" : "block");
    return 1;
}

// shareMapLink / shareModLink(this, name): UTILS.copyToClipboard (no clipboard here), then the
// button reads "Copied" for 1.25 s
static void share_restore(void *user) {
    const HtmlElem b = (HtmlElem) (uintptr_t) user;
    if (b) html_dom_set_text(b, "Share");
}

H(h_share_link) {
    UNUSED;
    if (!ev || !ev->target) return 1;
    html_dom_set_text(ev->target, "Copied");
    html_timer_add(1250, share_restore, (void *) (uintptr_t) ev->target);
    return 1;
}

// Login-only actions: io_po(null, "Must be logged in") / "Please Login"
H(h_publish_popup) { UNUSED; io_po(0, "Must be logged in"); return 1; }
H(h_noop) { UNUSED; return 1; }

// followUser(): toggle the button, io.send("f", profile.player_name, follow)
H(h_follow_user) {
    UNUSED;
    const HtmlElem b = el("followBtn");
    if (!b || !g_hub.logged_in) return 1;
    const int follow = html_dom_has_class(b, "follow");
    html_dom_set_class(b, "follow", !follow);
    html_dom_set_class(b, "unfollow", follow);
    html_dom_set_text(b, follow ? "Unfollow" : "Follow");
    cJSON *f = cJSON_CreateArray();
    cJSON_AddItemToArray(f, cJSON_CreateString("f"));
    cJSON_AddItemToArray(f, cJSON_CreateString(g_hub.profile_name));
    cJSON_AddItemToArray(f, cJSON_CreateNumber(follow));
    hub_send(f);
    return 1;
}

// viewFollowers / viewFollowing: io.send("gfs"|"gfg", name, id, count, captcha)
H(h_view_follows) {
    UNUSED;
    if (!g_hub.brain) return 1;
    const int following = !strcmp(html_ui_current_handler(), "viewFollowing");
    io_wait("Loading...");
    cJSON *f = cJSON_CreateArray();
    cJSON_AddItemToArray(f, cJSON_CreateString(following ? "gfg" : "gfs"));
    cJSON_AddItemToArray(f, cJSON_CreateString(g_hub.profile_name));
    cJSON_AddItemToArray(f, cJSON_CreateNumber(g_hub.profile_id));
    cJSON_AddItemToArray(f, cJSON_CreateNumber(following ? g_hub.profile_following : g_hub.profile_followed));
    cJSON_AddItemToArray(f, cJSON_CreateTrue());
    hub_send(f);
    return 1;
}

// makePost(): io.send("ps", {txt})
H(h_make_post) {
    UNUSED;
    char text[1024] = "";
    const HtmlElem input = el("postInput");
    if (input) html_dom_get_value(input, text, sizeof(text));
    if (!text[0] || g_hub.posting || !g_hub.logged_in) return 1;
    g_hub.posting = 1;
    set_html("postButton", "<div class='lds-rings'><div></div><div></div><div></div><div></div></div>");
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "txt", text);
    cJSON *f = cJSON_CreateArray();
    cJSON_AddItemToArray(f, cJSON_CreateString("ps"));
    cJSON_AddItemToArray(f, body);
    hub_send(f);
    return 1;
}

// likePost(id, el): flip the heart and the count, io.send("lps", id, like)
H(h_like_post) {
    (void) user;
    const int pid = (int) arg_n(args, argc, 0, 0);
    char id[32], count[32] = "0", icon[32] = "";
    snprintf(id, sizeof(id), "likesVal%d", pid);
    const HtmlElem val = el(id);
    const HtmlElem heart = ev ? ev->target : 0;
    if (!g_hub.logged_in || !val || !heart) return 1;
    html_dom_get_text(heart, icon, sizeof(icon));
    const int like = !strcmp(icon, "favorite_border");
    html_dom_get_text(val, count, sizeof(count));
    snprintf(count, sizeof(count), "%d", atoi(count) + (like ? 1 : -1));
    html_dom_set_text(val, count);
    html_dom_set_text(heart, like ? "favorite" : "favorite_border");
    html_dom_set_style(heart, "color", like ? "#eb4034" : "#000");
    cJSON *f = cJSON_CreateArray();
    cJSON_AddItemToArray(f, cJSON_CreateString("lps"));
    cJSON_AddItemToArray(f, cJSON_CreateNumber(pid));
    cJSON_AddItemToArray(f, cJSON_CreateNumber(like));
    hub_send(f);
    return 1;
}

// openPost(id): the single-post view, io.send("gps", id)
H(h_open_post) {
    UNUSED;
    if (strcmp(g_hub.v, "feed") != 0) update_window("feed", NULL, NULL, NULL, NULL, 0);
    set_display("postEntryC", g_hub.logged_in ? "inline-block" : "none");
    set_html("postTopStat", "");
    set_display("singlePost", "inline-block");
    set_display("fpHolder", "none");
    set_html("singlePostH", "<div style='margin-top:25px;margin-left:310px;margin-bottom:10px;opacity:0.4'><div class='lds-ring darkring'><div></div><div></div><div></div><div></div></div></div>");
    cJSON *f = cJSON_CreateArray();
    cJSON_AddItemToArray(f, cJSON_CreateString("gps"));
    cJSON_AddItemToArray(f, cJSON_CreateNumber(arg_n(args, argc, 0, 0)));
    hub_send(f);
    return 1;
}

H(h_leave_post) {
    UNUSED;
    set_display("singlePost", "none");
    set_display("fpHolder", "inline-block");
    return 1;
}

// postReply(id): io.send("com", id, {txt})
H(h_post_reply) {
    UNUSED;
    char text[1024] = "";
    const HtmlElem input = el("replyInput");
    if (input) html_dom_get_value(input, text, sizeof(text));
    if (!text[0] || !g_hub.logged_in) return 1;
    set_html("replyBtn", "<div class='lds-ringm'><div></div><div></div><div></div><div></div></div>");
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "txt", text);
    cJSON *f = cJSON_CreateArray();
    cJSON_AddItemToArray(f, cJSON_CreateString("com"));
    cJSON_AddItemToArray(f, cJSON_CreateNumber(arg_n(args, argc, 0, 0)));
    cJSON_AddItemToArray(f, body);
    hub_send(f);
    return 1;
}

H(h_switch_feed_tab) {
    UNUSED;
    if (g_hub.feed_switching) return 1;
    g_hub.feed_switching = 1;
    set_html("postHolder", "<div class='lds-ring'><div></div><div></div><div></div><div></div></div>");
    const int tab = (int) arg_n(args, argc, 0, 0);
    char id[32];
    for (int t = 0; t < 6; ++t) {
        snprintf(id, sizeof(id), "feedTab%d", t);
        const HtmlElem e = el(id);
        if (e) html_dom_set_attr(e, "class", t == tab ? "feedTab feedTabA" : "feedTab");
    }
    snprintf(g_hub.ja_k, sizeof(g_hub.ja_k), "%d", tab);
    request_page(); // Qa()
    return 1;
}

H(h_toggle_view) {
    (void) user; (void) args; (void) argc;
    const HtmlElem e = ev ? ev->target : 0;
    const HtmlElem parent = e ? html_dom_parent(e) : 0;
    if (!parent) return 1;
    const int collapsed = html_dom_has_class(parent, "collapsed");
    html_dom_set_class(parent, "collapsed", !collapsed);
    html_dom_set_html(e, collapsed ? "expand_less" : "expand_more");
    return 1;
}

static const HtmlHandlerEntry k_handlers[] = {
    {"updateWindow", h_update_window},
    {"SOUND.play", h_sound_play},
    {"openLink", h_open_link},
    {"openURL", h_open_link},
    {"window.open", h_open_link},
    {"location.reload", h_location_reload},
    {"loginPopup", h_login_popup},
    {"loginAcc", h_login_acc},
    {"searchProfile", h_search_profile},
    {"openSettings", h_open_settings},
    {"setSetting", h_set_setting},
    {"delayExecute", h_delay_execute},
    {"showPopup", h_show_popup},
    {"buyIllicit", h_buy_illicit},
    {"startChat", h_start_chat},
    {"closeChat", h_close_chat},
    {"sendChat", h_send_chat},
    {"clearPops", h_clear_pops},
    {"windows.0.switchTab", h_maps_switch_tab},
    {"windows.1.switchTab", h_mods_switch_tab},
    {"windows.0.loadMore", h_maps_load_more},
    {"windows.1.loadMore", h_mods_load_more},
    {"windows.0.searchMaps", h_maps_search},
    {"windows.1.searchMods", h_mods_search},
    {"openMap", h_open_map},
    {"openMod", h_open_mod},
    {"openProfile", h_open_profile},
    {"playMap", h_play_map},
    {"hostMap", h_play_map},
    {"playMod", h_play_mod},
    {"updateMapTab", h_update_map_tab},
    {"shareMapLink", h_share_link},
    {"shareModLink", h_share_link},
    {"publishPopup", h_publish_popup},
    {"publishModPopup", h_publish_popup},
    {"switchFeedTab", h_switch_feed_tab},
    {"toggleView", h_toggle_view},
    {"GUI.action", h_gui_action},
    {"favoriteM", h_noop},
    {"instaFollo", h_noop},
    {"followUser", h_follow_user},
    {"viewFollowers", h_view_follows},
    {"viewFollowing", h_view_follows},
    {"makePost", h_make_post},
    {"likePost", h_like_post},
    {"openPost", h_open_post},
    {"leavePost", h_leave_post},
    {"postReply", h_post_reply},
    {"refreshPopup", h_refresh_popup},
    {"buyItem", h_buy_item},
    {"listItem", h_list_item},
    {"listConfirmed", h_list_confirmed},
    {"unlistItem", h_unlist_item},
    {"quickSell", h_quick_sell},
    {"sellConfirmed", h_sell_confirmed},
    {"calcFee", h_calc_fee},
    {"searchItem", h_search_item},
    {"filterPopup", h_filter_popup},
    {"sortPopup", h_filter_popup},
    {"runFilter", h_run_filter},
    {"runSort", h_run_filter},
    {"resetFilter", h_reset_filter},
    {"resetSort", h_reset_filter},
    {"switchFilter", h_switch_filter},
    {"switchSort", h_switch_filter},
    {"toggleFilters", h_toggle_filters},
    {"toggleSorts", h_toggle_filters},
    {"updateTrades", h_update_trades},
    {"fastDeclOffer", h_decline_offer},
    {"declineAllTrades", h_decline_offer},
    {"clanRS", h_clan_rs},
    {"openProfileTab", h_open_profile_tab},
    {"openItemTab", h_open_item_tab},
    {"changeDateRange", h_change_date_range},
};

// Krunker functions that need an account (follow, trade, post, chat, filters, ...): no-ops
// for a guest, logged like any other dispatch.
static int fallback_handler(void *user, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    UNUSED;
    return 1;
}

static int l_resize(void *user, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    UNUSED;
    on_resize();
    return 1;
}

// <a href> clicks never navigate the page; links to hub pages / the game route in place.
static int l_document_click(void *user, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) user; (void) args; (void) argc;
    if (!ev || !ev->target) return 1;
    const HtmlElem a = html_dom_closest(ev->target, "a");
    if (!a) return 1;
    char href[512] = "";
    if (html_dom_get_attr(a, "href", href, sizeof(href)) < 0 || !href[0] || !strcmp(href, "#")) return 0;
    if (!strncmp(href, "javascript:", 11)) return 0;
    char target[16] = "";
    html_dom_get_attr(a, "target", target, sizeof(target));
    if (!app_route_url_target(href, !strcmp(target, "_blank"))) ui_open_external(href);
    return 0;
}

// ------------------------------------------------------------------ AppPage hooks

static void hub_connected(void) {
    // captchaCallback -> io.connect(...) callback: guest (no saved token) -> Login button
    g_hub.connected = 1;
    char name[64] = "", token[192] = "";
    if (g_hub.brain && html_storage_get("krunker_username", name, sizeof(name)) >= 0 && html_storage_get("krunker_token", token, sizeof(token)) >= 0 && name[0] && token[0]) {
        if (account_get()) {
            // account_ui.c's restore already answered on this socket
            g_hub.fe = 1;
            io_a(NULL);
        } else {
            // q && G: io.send("a", 1, [q, null, G]) is account_ui.c's restore (same socket)
            set_display("profileLogin", "none");
            set_html("profileName", "Logging in...");
            g_hub.fe = 1;
        }
    } else {
        set_display("profileLogin", "inline-block");
        set_html("profileName", "");
    }
    request_page(); // Xa(v, b, K, q, k, V, x)
}

// URL parameter of "p=profile&q=x" (decoded: %XX and '+').
static int query_param(const char *query, const char *name, char *out, const size_t n) {
    const size_t len = strlen(name);
    for (const char *p = query; p && *p;) {
        const char *end = strchr(p, '&');
        if (!end) end = p + strlen(p);
        if ((size_t) (end - p) >= len && !strncmp(p, name, len) && (p[len] == '=' || p + len == end)) {
            const char *v = p[len] == '=' ? p + len + 1 : end;
            size_t o = 0;
            while (v < end && o + 1 < n) {
                if (*v == '%' && end - v >= 3) {
                    char hex[3] = {v[1], v[2], 0};
                    out[o++] = (char) strtol(hex, NULL, 16);
                    v += 3;
                } else {
                    out[o++] = *v == '+' ? ' ' : *v;
                    v++;
                }
            }
            out[o] = 0;
            return 1;
        }
        p = *end ? end + 1 : end;
    }
    if (n) out[0] = 0;
    return 0;
}

static void social_enter(Client *client, const char *query) {
    for (int i = 0; i < 2; i++) {
        HubList *list = i ? &g_hub.mods : &g_hub.maps;
        for (int t = 0; t < 8; t++) list_reset(&list->data[t]);
    }
    social_assembly_free(&g_hub.assembly);
    memset(&g_hub, 0, sizeof(g_hub));
    g_hub.client = client;
    g_hub.entered = 1;
    g_hub.enter_time = glfwGetTime();
    snprintf(g_hub.query, sizeof(g_hub.query), "%s", query ? query : "");
    g_hub.ui_scale_b = 1.3;
    g_hub.ui_scaling = 1;
    g_hub.rarity_anim = 1;
    g_hub.show_pfp = 1;
    g_hub.volume = 1;
    g_hub.next_id = (uint32_t) rand();
    // Math.random(): a fresh pick of who-to-follow each visit (the scenario renderer keeps its seed)
#ifdef UI_SCENARIO_SUPPORTED
    if (!ui_scenario_active())
#endif
        srand((unsigned) (glfwGetTime() * 1000.0) ^ (unsigned) rand());

    html_ui_set_handlers(k_handlers, sizeof(k_handlers) / sizeof(k_handlers[0]), &g_hub, fallback_handler);
    html_ui_set_listener(NULL, "resize", l_resize);
    html_ui_set_listener(NULL, "click", l_document_click);
    html_ui_set_listener("searchPInput", "keyup", l_search_keyup);
    html_ui_set_listener("chatBoxInput", "keyup", l_chat_keyup);
    g_hub.brain = backend_enabled();
    if (g_hub.brain) {
        backend_social_listen(hub_on_social, NULL);
        backend_social_open();
    }

    settings_load(); // re[...].set(saved) at load: B, D, j, F, volume + onResize()
    on_resize();

    // updateWindow(de("p"), de("q"), de("s"), de("i")) (?games opens the maps)
    char p[64], q[HUB_KEY_MAX], s[HUB_KEY_MAX], i[HUB_KEY_MAX], tmp[8];
    if (query_param(g_hub.query, "games", tmp, sizeof(tmp))) {
        update_window("maps", NULL, NULL, NULL, NULL, 0);
    } else {
        const int hp = query_param(g_hub.query, "p", p, sizeof(p));
        const int hq = query_param(g_hub.query, "q", q, sizeof(q));
        const int hs = query_param(g_hub.query, "s", s, sizeof(s));
        const int hi = query_param(g_hub.query, "i", i, sizeof(i));
        update_window(hp ? p : NULL, hq ? q : NULL, hs ? s : NULL, hi ? i : NULL, NULL, 0);
    }
    fprintf(stderr, "[hub] enter ?%s (%s)\n", g_hub.query, hub_connection_up(client) ? "connected" : "no connection");
}

static void social_tick(Client *client, const float now, const float delta) {
    (void) now; (void) delta;
    if (!g_hub.entered) return;
    if (g_hub.brain && !g_hub.connected && backend_available() == 0) {
        // a default brain URL (local config / page origin) with no kn_brain behind it: the
        // guest page over the game connection, as without a backend
        g_hub.brain = 0;
        backend_social_unlisten(hub_on_social, NULL);
        g_hub.enter_time = glfwGetTime();
    }
    if (!g_hub.brain) poll_replies(client); // brain frames arrive through hub_on_social
    if (!g_hub.connected && !g_hub.error_shown) {
        if (hub_connection_up(client)) {
            hub_connected();
        } else if (!(g_hub.brain && backend_available() < 0) && glfwGetTime() - g_hub.enter_time > 3.0) {
            io_error("DISCONNECTED"); // the socket callback's error: io_1("DISCONNECTED")
        }
    } else if (g_hub.connected && !g_hub.error_shown && !hub_connection_up(client)) {
        io_error("DISCONNECTED");
    }
}

static void social_leave(Client *client) {
    (void) client;
    backend_social_unlisten(hub_on_social, NULL);
    if (g_delayed_timer) html_timer_cancel(g_delayed_timer);
    g_delayed_timer = 0;
    for (int i = 0; i < 2; i++) {
        HubList *list = i ? &g_hub.mods : &g_hub.maps;
        for (int t = 0; t < 8; t++) list_reset(&list->data[t]);
    }
    social_assembly_free(&g_hub.assembly);
    g_hub.entered = 0;
}

static const AppPage k_social_page = {
    .file = "social.html",
    .enter = social_enter,
    .leave = social_leave,
    .tick = social_tick,
};

const AppPage *social_app_page(void) {
    return &k_social_page;
}
