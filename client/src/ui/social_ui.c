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
#include <cJSON.h>
#include <html_dom.h>
#include <html_ui.h>
#include <social.h>
#include <ui_logic.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
static void hub_send(cJSON *frame) {
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
    cJSON_AddItemToArray(f, cJSON_CreateNull()); // K: krunker_id (guest)
    cJSON_AddItemToArray(f, cJSON_CreateNull()); // q: krunker_username (guest)
    cJSON_AddItemToArray(f, str_or_null(g_hub.ja_k));
    cJSON_AddItemToArray(f, cJSON_CreateNumber(g_hub.region));
    cJSON_AddItemToArray(f, str_or_null(g_hub.x));
    if (!strcmp(g_hub.v, "market")) cJSON_AddItemToArray(f, cJSON_CreateTrue());
    hub_send(f);
}

static void request_page(void) {
    send_page_request();
}

static void io_0(const char *e, const char *a, const cJSON *t);

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
        // profile: q (the logged-in name) is null for a guest
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
        io_0("tourney", NULL, NULL);
    } else if (g_hub.connected && !s) {
        request_page();
    }
}

// io_0(e, a, t, ...): one page's data arrived
static void io_0(const char *e, const char *a, const cJSON *t) {
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
        // no profile row from a KrunkNative server (no accounts): nothing is shown
        set_display("pTab_reports", "none");
        set_display("pTab_inputs", "none");
    } else if (!strcmp(e, "search")) {
        set_display("searchHolder", "block");
    } else if (!strcmp(e, "mods")) {
        set_display("modsHolder", "block");
        char *html = list_gen(&g_hub.mods, 1);
        set_html("modList", html);
        free(html);
    } else if (!strcmp(e, "mod") || !strcmp(e, "map")) {
        const int is_mod = !strcmp(e, "mod");
        side_boxes();
        set_display(is_mod ? "modHolder" : "mapHolder", "block");
        set_html(is_mod ? "modProfile" : "mapProfile",
                 is_mod ? "<div class='scrollItem' style='position:relative;padding-bottom:10px;padding-top:16px;'><span style='color:rgba(0,0,0,0.4);'>" STR_MODS_NONE "</span>"
                        : "<div class='scrollItem' style='position:relative;padding-bottom:10px;padding-top:16px;'><span style='color:rgba(0,0,0,0.4);'>" STR_MAPS_NONE "</span>");
    } else if (!strcmp(e, "maps")) {
        set_display("mapsHolder", "block");
        char *html = list_gen(&g_hub.maps, 0);
        set_html("mapList", html);
        free(html);
    } else if (!strcmp(e, "leaders")) {
        side_boxes();
        set_display("leaderHolder", "block");
        set_html("leaderList", "<div class='scrollItem'></div>"); // no leaderboard rows offline
    } else if (!strcmp(e, "market")) {
        set_display("marketHolder", "block");
        const int inventory = a && !strcmp(a, "inventory");
        set_display("invSortB", inventory ? "inline-block" : "none");
        set_display("marketSearchB", a && !strcmp(a, "market") ? "inline-block" : "none");
        set_display("invValue", inventory ? "inline-block" : "none");
        set_display("invTrack", inventory ? "inline-block" : "none");
        // no listings / inventory for a guest: social.market.login (K is null)
        set_html("marketList", "<div style='font-size:20px;margin-top:20px;color:#fff'>" STR_MARKET_LOGIN "...</div>");
    } else if (!strcmp(e, "itemsales")) {
        side_boxes();
        set_display("itemsalesList", "none");
        if (!has_t) set_html("itemsalesHolder", "<div style='font-size:20px;margin-top:20px;color:#fff'>" STR_MARKET_NONE "...</div>");
        set_display("itemsalesHolder", "block");
    } else if (!strcmp(e, "clan")) {
        set_display("clanHolder", "block");
    } else if (!strcmp(e, "clanwars")) {
        set_display("clanwarsHolder", "block");
        clanwars_show();
    } else if (!strcmp(e, "feed")) {
        set_display("feedHolder", "block");
        set_display("postEntryC", "none"); // acc is null
        side_boxes();
        g_hub.feed_switching = 0;
        // no posts on a KrunkNative server
        set_html("postHolder", "<div style='color:#fff;font-size:40px;margin-top:20px'>No Current Posts. Follow some Players!</div>");
    } else if (!strcmp(e, "tourney")) {
        set_display("tourneyHolder", "block");
        set_html("tourneyList", "<div>Tourney Results Coming soon</div>");
    }
}

// io_a(error, ...): the login reply. KrunkNative servers have no accounts.
static void io_a(const char *error) {
    if (!error) return;
    set_html("profileName", "");
    set_display("profileLogin", "inline-block");
    set_display("chatHolder", "none");
    if (!strcmp(g_hub.v, "feed")) request_page(); // Qa()
    io_po(0, error);
}

static void handle_reply_frame(const cJSON *frame) {
    const char *label = cJSON_GetStringValue(cJSON_GetArrayItem(frame, 0));
    if (!label) return;
    if (!strcmp(label, "0")) {
        const char *e = cJSON_GetStringValue(cJSON_GetArrayItem(frame, 1));
        const char *a = cJSON_GetStringValue(cJSON_GetArrayItem(frame, 2));
        io_0(e, a, cJSON_GetArrayItem(frame, 3));
    } else if (!strcmp(label, "a")) {
        io_a(cJSON_GetStringValue(cJSON_GetArrayItem(frame, 1)));
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
    // "psd", "uf": profile graphs/activity (no profiles here)
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
    // searchProfile(): "sp" is not answered by the offline server (the popup keeps
    // spinning until the backdrop closes it), like Krunker's.
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

H(h_show_popup) { UNUSED; show_popup_none(); return 1; }
H(h_clear_pops) { UNUSED; reset_pops(); set_display("popupHolder", "none"); return 1; }

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

// Login-only actions: io_po(null, "Must be logged in") / "Please Login"
H(h_publish_popup) { UNUSED; io_po(0, "Must be logged in"); return 1; }
H(h_noop) { UNUSED; return 1; }

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
    {"publishPopup", h_publish_popup},
    {"publishModPopup", h_publish_popup},
    {"switchFeedTab", h_switch_feed_tab},
    {"toggleView", h_toggle_view},
    {"GUI.action", h_gui_action},
    {"favoriteM", h_noop},
    {"instaFollo", h_noop},
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
    set_display("profileLogin", "inline-block");
    set_html("profileName", "");
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
    poll_replies(client);
    if (!g_hub.connected && !g_hub.error_shown) {
        if (hub_connection_up(client)) {
            hub_connected();
        } else if (glfwGetTime() - g_hub.enter_time > 3.0) {
            io_error("DISCONNECTED"); // the socket callback's error: io_1("DISCONNECTED")
        }
    } else if (g_hub.connected && !g_hub.error_shown && !hub_connection_up(client)) {
        io_error("DISCONNECTED");
    }
}

static void social_leave(Client *client) {
    (void) client;
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
