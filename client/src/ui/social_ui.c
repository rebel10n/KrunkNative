// Krunker v7.1.4 social hub (public/social.html + js/social.js, readable source
// src/social.js and src/ui/social.js in the v7.1.4 tree) ported to C over html_dom.h.
// The page is ui/social.html (imported by tools/ui-import/social_page.py); app_mode.c
// shows it for APP_MODE_HUB (--hub, the web page social.html, the menu's Hub button and every
// /social.html link) and calls the AppPage hooks at the bottom of this file.
//
// Krunker's hub talks to the matchmaker over its own socket; here the same request /
// reply frames travel over the game connection as JSON (shared/include/social.h) and the
// KrunkNative server (or the in-process local server) answers them like Krunker's offline
// server does, or the brain (kn_brain) answers them over its social socket. Without an
// account every view shows what the real page shows a guest for the data the server has
// (the map catalogue is the server's own map list).
//
// Function names in comments are v7 social.js's: updateWindow, io_0, f556 (send "r"),
// f492 (side boxes), f544 / f545 (map / mod cards), windows[0] / windows[1] (maps / mods),
// genericPopup / resetPops (every popup is #genericPop with a class since v7). The v3.1.1
// views v7 dropped (Clan Wars, Tourney, Krunkitis) fall back to the feed.
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
#ifdef _WIN32
#define timegm _mkgmtime // MinGW has no timegm (same as editor_net.c)
#endif
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#include "../viewer/viewer_store.h"

#define HUB_KEY_MAX 128
#define MARKET_MIN_LVL 15       // config.marketMinLVl (v7.1.4)
#define TRADE_MIN_LVL 20        // config.tradeMinLvl

// ------------------------------------------------------------------ v3.1.1 data (src/config.js, i18n/en.js)

typedef struct {
    const char *name;
    const char *alias;
    const char *tag;
    int id;
} HubFollow;

// config.whoToFol (v7.1.4)
static const HubFollow k_who_to_follow[] = {
    {"Sidney", NULL, "Developer", 201211}, {"Vince", NULL, "Developer", 105683},
    {"MoonJuiceAlt", "MoonJuice", "Streamer", 2968503}, {"ImWaspy", NULL, "Streamer", 2593871},
    {"cimota", NULL, "Skin Maker", 7516049}, {"Kilfy", NULL, "Skin Maker", 8871318}, {"iSpy", NULL, "Skin Maker", 177694},
    {"Nxbulah", NULL, "Skin Maker", 5739887}, {"Skriller33", NULL, "Skin Maker", 5068654}, {"Andrii", NULL, "Map Maker", 163429},
    {"JavieBhop", NULL, "Map Maker", 1517556}, {"Keybi", NULL, "Map Maker", 310968}, {"siktis", NULL, "Mod Maker", 5746131},
    {"DukeV", NULL, "Map Maker", 943615}, {"Chwoubaka", NULL, "Mod Maker", 4927907}, {"Swixie", NULL, "Streamer", 5720620},
    {"QuackyTTV", "Quacky", "Streamer", 3742667}, {"Highnoon", NULL, "Support", 361086}, {"eskrr", NULL, "Map Maker", 383703},
};
#define WHO_TO_FOLLOW_COUNT (sizeof(k_who_to_follow) / sizeof(k_who_to_follow[0]))

static const char *const k_social_regions[] = {"All Regions", "Oceania", "Asia", "Europe", "North America", "South America"};

// navigation buttons (vA177) and the pages without one (vA178)
static const char *const k_nav_pages[] = {"leaders", "mods", "maps", "market", "feed", "items"};
static const char *const k_sub_pages[] = {"itemsales", "clan", "map", "mod", "search", "profile", "post", "listing"};
static const char *const k_market_queries[] = {"market", "inventory", "sales", "stats", "trades", "blackm"}; // config.marketQueries

#define STR_MAPS_NONE "No Games found..."       // social.maps.none
#define STR_MODS_NONE "No Mods found..."        // social.mods.none
#define STR_WIN_MAPS_NONE "No Games found!"     // windows.maps.none
#define STR_WIN_MODS_NONE "No Mods found"       // windows.mods.none
#define STR_MARKET_NONE "No Items Found"        // social.market.none
#define STR_MARKET_LOGIN "You must be logged in." // social.market.login
#define STR_MARKET_FAILED "Failed. Try Again"   // social.market.failed

// ------------------------------------------------------------------ settings (vO78 = {...})

typedef enum { SET_SELECT, SET_SLIDER, SET_SWITCH, SET_HIDDEN, SET_URL } HubSettingKind;

typedef struct {
    const char *key;
    const char *name;   // en.js text of vO78[key].name
    const char *info;   // vO78[key].info ("" = none)
    HubSettingKind kind;
    double def, min, max, step;
    int hide;           // -1 = no `hide` field, 0/1 = vO78[key].hide
    double val;
    char str[256];      // SET_URL: the value (background image URL)
} HubSetting;

enum {
    HS_REGION, HS_SCALE_UI, HS_UI_SCALING, HS_RARITY_ANIM, HS_SHOW_PFP, HS_SHOW_TRADE_MSG, HS_HIDE_NON_TRADE,
    HS_SOUND, HS_AMBIENT_VOLUME, HS_UI_VOLUME, HS_MENU_SATURATION, HS_MENU_HUE, HS_BACKGROUND, HS_COUNT
};

static HubSetting g_settings[HS_COUNT] = {
    {"region", "Region Stats", "", SET_SELECT, 0, 0, 0, 0, -1, 0, ""},
    {"scaleUI", "UI Scale", "", SET_SLIDER, 0.7, 0.1, 1, 0.1, -1, 0, ""},
    {"uiScaling", "UI Scale", "", SET_HIDDEN, 1, 0, 0, 0, 1, 0, ""},
    {"disableRarityAnim", "Disable Rarity Animations", "", SET_SWITCH, 0, 0, 0, 0, -1, 0, ""},
    {"showPfp", "Show Profile Pictures", "", SET_SWITCH, 1, 0, 0, 0, -1, 0, ""},
    {"showTradeMsg", "Show Trade Messages", "", SET_SWITCH, 1, 0, 0, 0, -1, 0, ""},
    {"hideNonTrade", "Hide Non-Tradable Skins", "", SET_SWITCH, 0, 0, 0, 0, -1, 0, ""},
    {"sound", "Master Volume", "", SET_SLIDER, 1, 0, 1, 0.01, -1, 0, ""},
    {"ambientVolume", "Ambient Volume", "Background sound volume", SET_SLIDER, 1, 0, 1, 0.01, -1, 0, ""},
    {"uiVolume", "UI Volume", "Menu interface volume", SET_SLIDER, 1, 0, 1, 0.01, -1, 0, ""},
    {"menuSaturation", "Saturation (UI)", "Increase color saturation for menu", SET_SLIDER, 1, 0, 3, 0.1, -1, 0, ""},
    {"menuHue", "Color Hue (UI)", "Shift colors in-menu", SET_SLIDER, 0, 0, 100, 5, -1, 0, ""},
    {"background", "Background Image", "", SET_URL, 0, 0, 0, 0, -1, 0, ""},
};

// ------------------------------------------------------------------ state

typedef struct {
    int used;
    int load_ind;
    int waiting;
    StrBuf dat;      // accumulated cards ("dat"), empty = none yet
    int has_dat;
} HubListData;

#define HUB_LIST_TABS 13 // config.mapTabs.length (modTabs has 7)
typedef struct {
    int tab_index;
    HubListData data[HUB_LIST_TABS];
    cJSON *home_rows;  // listData[home].list: the featured card's pick (mods); maps: the featured list (v3540)
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
    int rarity_anim;        // v3530
    int show_pfp;           // v3531
    int show_trade_msg;     // v3532
    int hide_non_trade;     // v3533
    double volume;
    double ambient_volume, ui_volume;

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

// UTILS.compareString-like case-insensitive "contains"
static int contains_ci_str(const char *hay, const char *needle) {
    if (!hay || !needle) return 0;
    const size_t n = strlen(needle);
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < n && p[i] && tolower((unsigned char) p[i]) == tolower((unsigned char) needle[i])) i++;
        if (i == n) return 1;
    }
    return !n;
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

// The market filter / sort panels keep their state across popups (v7 saves #genericPop's
// innerHTML in resetPops when it holds one: v3528 / v3527); here the checkbox and field
// values are read back into these before the popup goes.
typedef struct {
    int built;
    int checked[3][40];  // [rarity | weapon | type][index]
    char text[128], season[8], order[8];
    int missing;
} HubFilterState;
static HubFilterState g_filter_state[2]; // 0 filter (market search), 1 sort (inventory)
static void filter_state_save(int sort);

static void reset_genericpop_state(void) {
    const HtmlElem pop = el("genericPop");
    if (!pop) return;
    if (html_dom_has_class(pop, "sortPop")) filter_state_save(1);
    if (html_dom_has_class(pop, "filterPop")) filter_state_save(0);
}

// resetPops()
static void reset_pops(void) {
    set_display("popupHolder", "block");
    set_display("popupContent", "none");
    set_display("giftPop", "none");
    reset_genericpop_state();
    const HtmlElem pop = el("genericPop");
    if (pop) {
        html_dom_set_display(pop, "none");
        html_dom_remove_attr(pop, "class");
        html_dom_set_html(pop, "");
    }
    set_display("itemViewPop", "none");
}

// clearPops()
static void clear_pops(void) {
    reset_pops();
    set_display("popupHolder", "none");
}

// genericPopup(html, cls, style): every v7 popup (settings, login, filters, confirmations,
// responses) is #genericPop with its class.
static void generic_popup(const char *html, const char *cls, const char *style) {
    reset_pops();
    const HtmlElem pop = el("genericPop");
    if (!pop) return;
    if (cls && *cls) html_dom_set_attr(pop, "class", cls);
    html_dom_set_attr(pop, "style", style ? style : "");
    html_dom_set_html(pop, html ? html : "");
    html_dom_set_display(pop, "block");
}

// showPopup() with no item: closes the popup (popupBack's onclick).
static void show_popup_none(void) {
    reset_pops();
    set_display("popupHolder", "none");
    set_style("popupContent", "width", "618px");
    set_display("popupContent", "block");
}

// io_po(e, a, t): genericPopup(..., "responsePop")
static void io_po(const int success, const char *text) {
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
    generic_popup(html, "responsePop", NULL);
    free(html);
}

// f520(text): the waiting popup (genericPopup(..., "loadingPop"))
static void io_wait(const char *text) {
    StrBuf sb = {0};
    sb_append(&sb, "<div style='width:100%;text-align:center'><div class='lds-ring'><div></div><div></div><div></div><div></div></div><div style='font-size:20px;color:rgba(255,255,255,0.5)'>");
    sb_append(&sb, text);
    sb_append(&sb, "</div></div>");
    char *html = sb_finish(&sb);
    generic_popup(html, "loadingPop", NULL);
    free(html);
}

// hideAll(e)
static void hide_all(const int keep_loading) {
    static const char *const holders[] = {"profileHolder", "searchHolder", "mapsHolder", "modsHolder", "mapHolder",
                                          "modHolder", "leaderHolder", "feedHolder", "aBox", "marketHolder",
                                          "itemsHolder", "itemsalesHolder", "listingHolder", "clanHolder", "popupHolder",
                                          "averageCost", "totalSold", "chatBox", "itemViewPop"};
    if (!keep_loading) set_display("loadMessage", "block");
    for (size_t i = 0; i < sizeof(holders) / sizeof(holders[0]); i++) set_display(holders[i], "none");
    const HtmlElem tip = el("chartjs-tooltip");
    if (tip) html_dom_remove(tip);
}

// io_error(text, code): the error screen (DISCONNECTED)
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
    const HtmlElem code = el("errorCode");
    if (code) html_dom_set_text(code, "");
}

// ------------------------------------------------------------------ settings

static void storage_key(char *out, const size_t n, const char *key) {
    snprintf(out, n, "krs_setngss_%s", key);
}

// f483(): the 1700x900 design space times vLN155 (1 - scaleUI + 1) on #uiBase (v7 scales
// #uiBase, v3.1.1 scaled <body>), when uiScaling is on. toFixed(3) like the page.
static void js_fixed3(char *out, const size_t n, const double v) {
    snprintf(out, n, "%.3f", v);
}

static void on_resize(void) {
    const HtmlElem base = el("uiBase");
    if (!base) return;
    if (!g_hub.ui_scaling) {
        html_dom_set_style(base, "transform", "");
        html_dom_set_style(base, "width", "");
        html_dom_set_style(base, "height", "");
        return;
    }
    float w = 0, h = 0;
    html_dom_viewport(&w, &h, NULL);
    if (w <= 0 || h <= 0) return;
    const double t = g_hub.ui_scale_b * 1700.0;
    const double n = g_hub.ui_scale_b * 900.0;
    const double i = w / t;
    const double r = h / n;
    char transform[48], width[32], height[32], num[32];
    if (r < i) {
        js_fixed3(num, sizeof(num), r);
        snprintf(transform, sizeof(transform), "scale(%s)", num);
        js_fixed3(num, sizeof(num), w / r);
        snprintf(width, sizeof(width), "%spx", num);
        snprintf(height, sizeof(height), "%.15gpx", n);
    } else {
        js_fixed3(num, sizeof(num), i);
        snprintf(transform, sizeof(transform), "scale(%s)", num);
        snprintf(width, sizeof(width), "%.15gpx", t);
        js_fixed3(num, sizeof(num), h / i);
        snprintf(height, sizeof(height), "%spx", num);
    }
    html_dom_set_style(base, "transform", transform);
    html_dom_set_style(base, "width", width);
    html_dom_set_style(base, "height", height);
}

static void request_page(void);

// window.bundleFilters(): saturate / hue-rotate on #uiBase
static void bundle_filters(void) {
    char f[96] = "";
    const double sat = g_settings[HS_MENU_SATURATION].val, hue = g_settings[HS_MENU_HUE].val;
    size_t o = 0;
    if (sat != 1) o += (size_t) snprintf(f + o, sizeof(f) - o, "saturate(%.15g%%)", sat * 100);
    if (hue != 0) o += (size_t) snprintf(f + o, sizeof(f) - o, "%shue-rotate(%.15gdeg)", o ? " " : "", hue * 3.6);
    set_style("uiBase", "filter", f);
}

// vO78[key].set(val)
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
        case HS_SHOW_TRADE_MSG: g_hub.show_trade_msg = v != 0; break;
        case HS_HIDE_NON_TRADE: g_hub.hide_non_trade = v != 0; break;
        case HS_SOUND: g_hub.volume = v; break;
        case HS_AMBIENT_VOLUME: g_hub.ambient_volume = v; break;
        case HS_UI_VOLUME: g_hub.ui_volume = v; break;
        case HS_MENU_SATURATION:
        case HS_MENU_HUE: bundle_filters(); break;
        case HS_BACKGROUND: {
            // isURL && !svg, else the default bg_test.png
            const char *u = g_settings[index].str;
            const int ok = (!strncmp(u, "http://", 7) || !strncmp(u, "https://", 8)) && !strstr(u, ".svg");
            StrBuf sb = {0};
            sb_append(&sb, "url(");
            sb_append(&sb, ok && strlen(u) > 1 ? u : "./img/backgrounds/bg_test.png");
            sb_append(&sb, ")");
            char *bg = sb_finish(&sb);
            set_style("backgroundImage", "background-image", bg);
            free(bg);
            break;
        }
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
    if (s->kind == SET_URL) snprintf(out, n, "%s", s->str);
    else if (s->kind == SET_SWITCH || s->kind == SET_HIDDEN) snprintf(out, n, "%s", s->val != 0 ? "true" : "false");
    else js_num(out, n, s->val);
}

static int setting_index(const char *key) {
    for (int i = 0; i < HS_COUNT; i++) if (key && !strcmp(g_settings[i].key, key)) return i;
    return -1;
}

// img settings: /\.(jpg|jpeg|png|gif|webp|ico)/ else the default; quotes removed
static void setting_set_url(HubSetting *s, const char *text) {
    static const char *const exts[] = {".jpg", ".jpeg", ".png", ".gif", ".webp", ".ico"};
    int ok = 0;
    for (size_t i = 0; text && i < sizeof(exts) / sizeof(exts[0]); i++) ok |= strstr(text, exts[i]) != NULL;
    size_t o = 0;
    for (const char *p = ok ? text : ""; *p && o + 1 < sizeof(s->str); p++) {
        if (*p != '\'' && *p != '"') s->str[o++] = *p;
    }
    s->str[o] = 0;
}

// setSetting(key, value)
static void set_setting(const char *key, const char *value) {
    const int i = setting_index(key);
    if (i < 0) return;
    HubSetting *s = &g_settings[i];
    char cleaned[256];
    size_t o = 0;
    for (const char *p = value ? value : ""; *p && o + 1 < sizeof(cleaned); p++) {
        if (*p != '<' && *p != '>') cleaned[o++] = *p;
    }
    cleaned[o] = 0;
    if (s->kind == SET_URL) setting_set_url(s, value ? cleaned : s->str);
    else s->val = value ? setting_parse(s, cleaned) : s->val;
    char text[256], id[96];
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
        s->str[0] = 0;
        char skey[64], saved[256];
        storage_key(skey, sizeof(skey), s->key);
        if (html_storage_get(skey, saved, sizeof(saved)) >= 0) {
            if (s->kind == SET_URL) setting_set_url(s, saved);
            else s->val = setting_parse(s, saved);
        }
        setting_apply(i);
    }
}

// openSettings(): initSocialUI._html.settings.input.* markup (v7 src/ui/social.js), byte for byte.
static void append_setting_html(StrBuf *sb, const HubSetting *s) {
    char val[256], min[32], max[32], step[32];
    setting_value_text(s, val, sizeof(val));
    js_num(min, sizeof(min), s->min);
    js_num(max, sizeof(max), s->max);
    js_num(step, sizeof(step), s->step);
    switch (s->kind) {
        case SET_SELECT:
            sb_appendf(sb, "<select onchange='setSetting(\"%s\", this.value)' class='inputGrey2'>\n\t\t\t\t\t\t\t", s->key);
            for (size_t i = 0; i < sizeof(k_social_regions) / sizeof(k_social_regions[0]); i++) {
                // Object.keys(l).map(...) concatenated to a string: joined with ","
                sb_appendf(sb, "%s<option value='%zu' %s>%s</option>", i ? "," : "", i,
                           (int) s->val == (int) i ? "selected" : "", k_social_regions[i]);
            }
            sb_append(sb, "</select>");
            break;
        case SET_SWITCH:
            sb_appendf(sb, "<label class='switch' style='margin-left:10px'><input type='checkbox' onclick='setSetting(\"%s\", this.checked)'\n\t\t\t\t\t\t\t%s><span class='slider'></span></label>",
                       s->key, s->val != 0 ? "checked" : "");
            break;
        case SET_SLIDER: {
            const char *style = s == &g_settings[HS_SCALE_UI] ? "width: 180px" : "";
            if (s == &g_settings[HS_SCALE_UI]) {
                // switch(vO78.uiScaling) + slider(this)
                HubSetting sw = g_settings[HS_UI_SCALING];
                sw.kind = SET_SWITCH;
                append_setting_html(sb, &sw);
            }
            sb_appendf(sb, "<input type='number' class='sliderVal' id='slid_input_%s' min='%s' max='%s'\n\t\t\t\t\t\t\tvalue='%s' onkeypress='return delayExecute(\"%s\", this, undefined)' style='margin-right:0px;border-width:0px'/>\n\t\t\t\t\t\t\t<div class='slidecontainer' style='margin-top: -8px;%s'><input type='range' id='slid_%s' min='%s' max='%s' step='%s'\n\t\t\t\t\t\t\tvalue='%s' class='sliderM' oninput='setSetting(\"%s\", this.value)'></div>",
                       s->key, min, max, val, s->key, style, s->key, min, max, step, val, s->key);
            break;
        }
        case SET_URL: // generic(): type "url", placeholder settings.social.bg.paste
            sb_appendf(sb, "<input type='url' name='url' id='slid_%s' class='inputGrey2' placeholder='Background Image URL'\n\t\t\t\t\t\t\tvalue='", s->key);
            sb_append_esc(sb, val);
            sb_appendf(sb, "' oninput='setSetting(\"%s\", this.value)'/>", s->key);
            break;
        case SET_HIDDEN:
            sb_append(sb, "undefined"); // vO78.uiScaling.html() returns undefined
            break;
    }
}

static void open_settings(void) {
    StrBuf sb = {0};
    sb_append(&sb, "<div class='setPopHeader'>Settings</div><div id='settHolder'>");
    for (int i = 0; i < HS_COUNT; i++) {
        const HubSetting *s = &g_settings[i];
        sb_appendf(&sb, "<div class='settName' title='%s'", s->info);
        if (s->hide >= 0) sb_appendf(&sb, "id='%s_div' style='display:%s'", s->key, s->hide ? "none" : "block");
        sb_appendf(&sb, ">%s ", s->name);
        if (s->kind == SET_URL) sb_appendf(&sb, "<div class='openCollection material-icons' onclick='imagePopup(&quot;%s&quot;)'>collections</div>", s->key);
        append_setting_html(&sb, s);
        sb_append(&sb, "</div>");
    }
    // openOneTrust(): KrunkNative shows no ads (the button stays, it does nothing)
    sb_append(&sb, "<div style='background-color: #994cd1!important;font-size: 17px;padding: 5px;border-radius: 4px;text-align: center;margin-top: 10px;color: rgba(255, 255, 255, 1) !important;' onclick='openOneTrust()'>Manage Ad Preferences</div>");
    sb_append(&sb, "</div>");
    char *html = sb_finish(&sb);
    generic_popup(html, "settingsPop", NULL);
    free(html);
}

// ------------------------------------------------------------------ side boxes (f492)

static void side_boxes(void) {
    set_display("aBox", "block");
    int picks[WHO_TO_FOLLOW_COUNT];
    int count = (int) WHO_TO_FOLLOW_COUNT;
    for (int i = 0; i < count; i++) picks[i] = i;
    StrBuf sb = {0};
    for (int n = 0; n < 3; ++n) {
        const int e = rand_int(0, count - 1);
        const HubFollow *f = &k_who_to_follow[picks[e]];
        // src: user-assets.krunker.io/p<id>/profile.png has no picture for these accounts, so
        // the real page always ends on its onerror fallback, player_pfp.png (the native engine
        // runs no onerror): the fallback is the src here.
        (void) f->id;
        sb_appendf(&sb, "<div class='sideBarItem'><img class='replyPfp pfpOutlW' onerror='this.onerror=null;this.src=\"./img/player_pfp.png\";'src='./img/player_pfp.png' /><div class='reqFollH'><a class='menuLink blueLink reqFollN' href='/social.html?p=profile&q=%s'>%s</a><div style='color:rgba(255,255,255,0.4);font-size:12px'>%s</div></div><div class='reqFollB' onclick='instaFollo(&quot;%s&quot;,this)'>Follow</div></div>",
                   f->name, f->alias ? f->alias : f->name, f->tag, f->name);
        memmove(&picks[e], &picks[e + 1], (size_t) (count - e - 1) * sizeof(int)); // splice(e, 1)
        count--;
    }
    char *html = sb_finish(&sb);
    set_html("whoToFolL", html);
    free(html);
    set_style("leftBox", "display", "");
    // recEmbed.src stays empty: no live stream is listed (v3490 null), the box hides
    set_style("leftBox", "display", "none");
}

// ------------------------------------------------------------------ maps / mods lists (windows[0], windows[1])

static double parse_time_ms(const cJSON *v);
static double now_epoch_ms(void);

// config.mapTabs / config.modTabs (v7.1.4). `api` is the /api/maps|mods `index` the
// KrunkNative server and kn_brain answer (shared/src/social.c, services/brain/creations.c:
// 0 Popular, 1 Featured, 2 New, 3 Favorites, 4 My Games / My Mods); v7's own index space
// (Home, the hidden recent/trending lists) has no server of its own.
typedef struct {
    const char *n, *txt, *click;
    int home, search, send_id, no_pages, hide, fav, owned, no_load, api;
} HubTab;

static const HubTab k_map_tabs[] = {
    {"Home", "<span class='material-icons material-icons-outlined' style='color:#fbc02d;font-size:42px;vertical-align:bottom'>home</span>", NULL, 1, 0, 0, 1, 0, 0, 0, 0, 0},
    {"Popular", NULL, NULL, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {"Featured", NULL, NULL, 0, 0, 0, 1, 0, 0, 0, 0, 1},
    {"New", NULL, NULL, 0, 0, 0, 0, 0, 0, 0, 0, 2},
    {"Favorites", NULL, NULL, 0, 0, 1, 0, 0, 1, 0, 0, 3},
    {"My Games", NULL, NULL, 0, 0, 1, 0, 0, 0, 1, 0, 4},
    {"Search", "<span class='material-icons' style='color:#ffffff;font-size:42px;vertical-align:bottom'>search</span>", NULL, 0, 1, 0, 0, 0, 0, 0, 0, -1},
    {"Editor", "<span class='material-icons' style='color:#ffffff;font-size:44px;vertical-align:bottom'>construction</span>Editor", "openURL(`./editor.html`)", 0, 0, 0, 0, 0, 0, 0, 0, -1},
    {"Sandbox", "<span class='material-icons' style='color:#ffffff;font-size:44px;vertical-align:bottom'>castle</span>Sandbox", "openURL(`./?sandbox`)", 0, 0, 0, 0, 0, 0, 0, 0, -1},
    {"New & Popular", NULL, NULL, 0, 0, 0, 0, 1, 0, 0, 0, 0},
    {"All time", NULL, NULL, 0, 0, 0, 0, 1, 0, 0, 0, 0},
    {"Trending Now", NULL, NULL, 0, 0, 0, 0, 1, 0, 0, 0, 0},
    {"New & Trending", NULL, NULL, 0, 0, 0, 0, 1, 0, 0, 0, 2},
};
#define MAP_TAB_COUNT ((int) (sizeof(k_map_tabs) / sizeof(k_map_tabs[0])))

static const HubTab k_mod_tabs[] = {
    {"Home", "<span class='material-icons-outlined' style='color:#fbc02d;font-size:42px;vertical-align:bottom'>home</span>", NULL, 1, 0, 0, 1, 0, 0, 0, 0, 1},
    {"Popular", NULL, NULL, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {"New", NULL, NULL, 0, 0, 0, 0, 0, 0, 0, 0, 2},
    {"Favorites", NULL, NULL, 0, 0, 1, 0, 0, 1, 0, 0, 3},
    {"My Mods", NULL, NULL, 0, 0, 1, 0, 0, 0, 1, 0, 4},
    {"Loaded", NULL, NULL, 0, 0, 0, 0, 0, 0, 0, 1, -1},
    {"Search", "<span class='material-icons' style='color:#ffffff;font-size:42px;vertical-align:bottom'>search</span>", NULL, 0, 1, 0, 0, 0, 0, 0, 0, -1},
};
#define MOD_TAB_COUNT ((int) (sizeof(k_mod_tabs) / sizeof(k_mod_tabs[0])))

// config.mapCats / config.mapCatsP
static const char *const k_map_cats[] = {"Other", "Parkour", "Puzzle", "Trading", "Training", "Social", "Horror", "Adventure",
                                         "Battle Royale", "Realism", "Roleplay", "Rotation", "Racing", "Raid", "Remake", "NCR",
                                         "Zombies", "Simulation", "Relaxation", "Action", "Tycoon", "Mini-Games", "Single-player", "Education"};
#define MAP_CAT_COUNT ((int) (sizeof(k_map_cats) / sizeof(k_map_cats[0])))
static const struct { const char *n; int i; } k_map_cats_p[] = {
    {"Action", 1}, {"Parkour", 8}, {"Remake", 4}, {"Zombies", 7}, {"Horror", 0}, {"Realism", 12}, {"Mini-Games", 5},
    {"Adventure", 2}, {"Single-player", 10}, {"Battle Royale", 9}, {"Relaxation", 3}, {"Simulation", 11}, {"Tycoon", 6}, {"Raid", 13},
};

static const HubTab *list_tab(const int is_mods, const int tab) {
    if (is_mods) return tab >= 0 && tab < MOD_TAB_COUNT ? &k_mod_tabs[tab] : NULL;
    return tab >= 0 && tab < MAP_TAB_COUNT ? &k_map_tabs[tab] : NULL;
}

static int list_search_id(const int is_mods) { return 6; } // config.mapTabs/modTabs.findIndex(t => t.search)

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

// API_CLIENT.listMaps(tab, pos, ...) / listMods
static void list_request(const int is_mods, const int tab, const int pos) {
    const HubTab *t = list_tab(is_mods, tab);
    if (t && t->api >= 0) api_request(is_mods ? "/mods" : "/maps", t->api, pos, NULL, NULL);
}

// UTILS.formatNumYT(n)
static void format_num_yt(char *out, const size_t n, const double v) {
    static const char *const suffix[] = {"", "k", "m", "b", "t"};
    if (!(v >= 1000)) {
        js_num(out, n, v);
        return;
    }
    char digits[64];
    js_num(digits, sizeof(digits), v);
    int l = (int) (strlen(digits) / 3);
    if (l > 4) l = 4;
    double val = 0;
    for (int p = 2; p >= 1; p--) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.*g", p, v / pow(1000.0, l));
        val = atof(buf);
        js_num(buf, sizeof(buf), val);
        int len = 0;
        for (const char *c = buf; *c; c++) len += isalnum((unsigned char) *c) || *c == ' ';
        if (len <= 2) break;
    }
    if (fmod(val, 1.0) != 0) snprintf(out, n, "%.1f%s", val, suffix[l]);
    else snprintf(out, n, "%.0f%s", val, suffix[l]);
}

// The row field aliases f544 / f545 accept (dte / mna / mid / ctn / vte, ...)
static const char *row_str2(const cJSON *r, const char *a, const char *b) {
    const char *v = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(r, a));
    if (!v && b) v = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(r, b));
    return v;
}

static double row_num2(const cJSON *r, const char *a, const char *b) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(r, a);
    if (!v && b) v = cJSON_GetObjectItemCaseSensitive(r, b);
    if (cJSON_IsNumber(v)) return v->valuedouble;
    if (cJSON_IsString(v)) return atof(v->valuestring);
    return 0;
}

static double row_age_days(const cJSON *r, const char *a, const char *b) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(r, a);
    if (!v && b) v = cJSON_GetObjectItemCaseSensitive(r, b);
    const double born = parse_time_ms(v);
    return born > 0 ? (now_epoch_ms() - born) / 86400000.0 : -1;
}

static void shuffle_rows(cJSON *rows) {
    for (int m = cJSON_GetArraySize(rows); m-- > 0;) { // UTILS.shuffleArray
        const int p = rand_int(0, m);
        if (p == m) continue;
        cJSON *a = cJSON_DetachItemFromArray(rows, m);
        cJSON *b = cJSON_DetachItemFromArray(rows, p);
        cJSON_InsertItemInArray(rows, p, a);
        cJSON_InsertItemInArray(rows, m, b);
    }
}

// f544(rows, tab): map cards (tab 0 = Home: shuffled, "Popular Categories" first)
static void append_map_cards(StrBuf *sb, const cJSON *rows_in, const int tab) {
    if (!cJSON_IsArray(rows_in) || !cJSON_GetArraySize(rows_in)) sb_append(sb, "<span style='color:rgba(0,0,0,0.4);'>" STR_MAPS_NONE "</span>");
    if (!cJSON_IsArray(rows_in)) return;
    const HubTab *cfg = list_tab(0, tab);
    cJSON *rows = cJSON_Duplicate(rows_in, 1);
    if (tab == 0) {
        shuffle_rows(rows);
        sb_append(sb, "<div class='mapListCat'>Popular Categories</div>");
        StrBuf cats = {0};
        for (size_t i = 0; i < sizeof(k_map_cats_p) / sizeof(k_map_cats_p[0]); ++i) {
            int idx = 0;
            for (int c = 0; c < MAP_CAT_COUNT; c++) if (!strcmp(k_map_cats[c], k_map_cats_p[i].n)) idx = c;
            sb_appendf(&cats, "<div class='mapListItem' style='width:450px;height:130px;'><div class='mapLICat' onclick='windows[0].switchTab(7);mpCat.selectedIndex=%d;windows[0].searchMaps();' style='background:url(./img/backgrounds/gamecats/c_%d.png)'>%s</div></div>",
                       idx + 1, k_map_cats_p[i].i, k_map_cats_p[i].n);
        }
        char *c = sb_finish(&cats);
        sb_appendf(sb, "<div class='mapCatScrl'>%s</div>", c);
        free(c);
    }
    const AccountInfo *acc = g_hub.logged_in ? account_get() : NULL;
    const char *last_cat = NULL;
    const cJSON *r;
    cJSON_ArrayForEach(r, rows) {
        const char *name = row_str2(r, "mna", "map_name");
        const char *creator = row_str2(r, "ctn", "creatorname");
        if (!name) continue;
        const char *mod_url = row_str2(r, "mod_url", NULL);
        const char *mod_id = row_str2(r, "mod_id", NULL);
        const char *image = row_str2(r, "map_image", NULL);
        const char *cat = row_str2(r, "category", NULL);
        const double age = row_age_days(r, "dte", "map_date");
        const double map_id = row_num2(r, "mid", "map_id");
        const int fav = row_num2(r, "fav", NULL) != 0 || (cfg && cfg->fav);
        const int edit = acc && (acc->is_dev || (creator && !strcmp(acc->name, creator) && cfg && cfg->owned));
        const int del = acc && (acc->is_dev || (creator && !strcmp(acc->name, creator) && cfg && cfg->owned));
        if (cat && (!last_cat || strcmp(last_cat, cat))) {
            if (last_cat) sb_append(sb, "</div>");
            last_cat = cat;
            sb_appendf(sb, "<div class='mapListCat'>%s</div><div class='mapCatScrl'>", cat);
        }
        char id[32], plays[32], fund[32];
        js_num(id, sizeof(id), map_id);
        format_num_yt(plays, sizeof(plays), row_num2(r, "map_pl", NULL));
        comma_number(fund, sizeof(fund), row_num2(r, "fund", NULL));
        sb_append(sb, "<div style='display:inline-block;margin-bottom:20px;vertical-align:top;'><div class='mapListItem'>");
        if (age >= 0 && age <= 7) {
            const int upd = row_num2(r, "map_updatecounter", NULL) > 1;
            sb_appendf(sb, "<div class='mapAgeD' style='background-color:%s'>%s</div>", upd ? "#0080ff" : "#da00ff", upd ? "UPDATED" : "NEW");
        }
        sb_append(sb, "<div class='creationActions'>");
        if (edit) {
            sb_append(sb, "<span title='Edit' class='material-icons creationAction edit' onclick='openEdit(&quot;");
            sb_append_js_escape(sb, name);
            sb_appendf(sb, "&quot;,%s,%s)'>create</span>", id, fund);
        }
        if (del) sb_appendf(sb, "<span title='Delete' class='material-icons creationAction del' onclick='deleteCreationConf(%s,0);'>delete_forever</span>", id);
        if (acc && acc->is_dev) sb_appendf(sb, "<span title='Feature' class='material-icons creationAction%s' onclick='featureCreation(this, %s,0);'>check_circle</span>", row_num2(r, "map_featured", NULL) ? " sel" : "", id);
        sb_appendf(sb, "<span title='Favorite' class='material-icons creationAction%s' onclick='favoriteM(0, this, %s);'>grade</span></div><div class='mapListThumbHolder'><img draggable='false' class='mapListThumb' loading='lazy' src='", fav ? " sel" : "", id);
        sb_append(sb, image && *image ? image : "./img/noimg.png");
        sb_append(sb, "' onerror='this.onerror=null;this.src=\"./img/noimg.png\";' onclick='openMap(&quot;");
        sb_append_js_escape(sb, name);
        sb_append(sb, "&quot;)'/>");
        if (edit) sb_appendf(sb, "<div class='mapsVotes' style='top: 145px;right: 9px;' title='KR Bank'>%s<span style='margin-left: 8px;color:#FBC02D;font-size: 17px;'>KR</span></div>", fund);
        sb_append(sb, "<div class='mapsVotes' title='Live Players' id='pcnt_");
        sb_append_js_escape(sb, name);
        // windows[0].playerData: live counts come from custom servers in the matchmaker's game
        // list; KrunkNative servers list none, every count stays 0
        sb_append(sb, "' style='left:10px;right:unset;top:145px;'><span class='material-icons' style='margin-right: 8px;color:rgba(255, 255, 255, 0.5);vertical-align: middle;margin-top:-5px;font-size:30px;'>person</span>0</div><div class='mapListCrdI'><a href='javascript:;' style='color:#fff !important;margin-left:6px;' onclick='openMap(&quot;");
        sb_append_js_escape(sb, name);
        sb_appendf(sb, "&quot;,&quot;%s&quot;,&quot;%s&quot;)'/>", mod_url ? mod_url : "undefined", mod_id ? mod_id : "undefined");
        sb_append(sb, name);
        sb_appendf(sb, "</a><span style='float:right;color:#fff;margin-right:6px;'>%s <span style='color:rgba(255,255,255,0.6)'>Plays</span></span></div></div></div></div>", plays);
    }
    cJSON_Delete(rows);
}

// f545(rows, tab): mod cards
static void append_mod_cards(StrBuf *sb, const cJSON *rows, const int tab) {
    if (!cJSON_IsArray(rows) || !cJSON_GetArraySize(rows)) sb_append(sb, "<span style='color:rgba(0,0,0,0.4);'>" STR_MODS_NONE "</span>");
    if (!cJSON_IsArray(rows)) return;
    const HubTab *cfg = list_tab(1, tab);
    const AccountInfo *acc = g_hub.logged_in ? account_get() : NULL;
    const char *last_cat = NULL;
    const cJSON *r;
    cJSON_ArrayForEach(r, rows) {
        const char *name = row_str2(r, "mna", "mod_name");
        const char *creator = row_str2(r, "ctn", "creatorname");
        if (!name) continue;
        const char *image = row_str2(r, "mod_image", NULL);
        const char *cat = row_str2(r, "category", NULL);
        const double age = row_age_days(r, "dte", "mod_date");
        const double mod_id = row_num2(r, "mid", "mod_id");
        const int fav = row_num2(r, "fav", NULL) != 0 || (cfg && cfg->fav);
        const int own = acc && (acc->is_dev || (creator && !strcmp(acc->name, creator) && cfg && cfg->owned));
        if (cat && (!last_cat || strcmp(last_cat, cat))) {
            if (last_cat) sb_append(sb, "</div>");
            last_cat = cat;
            sb_appendf(sb, "<div class='mapListCat'>%s</div><div class='mapCatScrl'>", cat);
        }
        char id[32], votes[32];
        js_num(id, sizeof(id), mod_id);
        format_num_yt(votes, sizeof(votes), row_num2(r, "vte", "mod_votes"));
        sb_append(sb, "<div class='mapListItem'>");
        if (age >= 0 && age <= 7) sb_append(sb, "<div class='mapAgeD' style='background-color:#da00ff'>NEW</div>");
        sb_append(sb, "<div class='creationActions'>");
        if (own) {
            sb_append(sb, "<span class='material-icons creationAction edit' onclick='updateMod(&quot;");
            sb_append_js_escape(sb, name);
            sb_append(sb, "&quot;);'>edit</span>");
            sb_appendf(sb, "<span class='material-icons creationAction del' onclick='deleteCreationConf(%s,1);'>delete_forever</span>", id);
        }
        if (acc && acc->is_dev) sb_appendf(sb, "<span class='material-icons creationAction%s' onclick='featureCreation(this, %s,1);'>check_circle</span>", row_num2(r, "mod_featured", NULL) ? " sel" : "", id);
        sb_appendf(sb, "<span title='Favorite' class='material-icons creationAction%s' onclick='favoriteM(1, this, %s);'>grade</span></div><div class='mapListThumbHolder'><img draggable='false' class='mapListThumb' loading='lazy' src='", fav ? " sel" : "", id);
        sb_append(sb, image && *image ? image : "./img/noimg.png");
        sb_append(sb, "' onerror='this.onerror=null;this.src=\"./img/noimg.png\";' onclick='openMod(&quot;");
        sb_append_js_escape(sb, name);
        sb_append(sb, "&quot;)'/><div class='mapListCrdI'><a href='javascript:;' style='color:#fff !important;margin-left:6px;' onclick='openMod(&quot;");
        sb_append_js_escape(sb, name);
        sb_append(sb, "&quot;)'/>");
        sb_append(sb, name);
        sb_appendf(sb, "</a><span style='float:right;color:#fff;margin-right:6px;'>%s <span style='color:rgba(255,255,255,0.6)'>Likes</span></span></div></div></div>", votes);
    }
}

static HubListData *list_data(HubList *list, const int tab) {
    if (tab < 0 || tab >= HUB_LIST_TABS) return NULL;
    return &list->data[tab];
}

static void list_reset(HubListData *d) {
    sb_free(&d->dat);
    memset(d, 0, sizeof(*d));
}

// The listData body of the current tab (both gens): cards + Loading / Load More
static void list_body(StrBuf *sb, HubList *list, const int is_mods, const HubTab *t) {
    const int tab = list->tab_index;
    HubListData *d = list_data(list, tab);
    if (d && d->used) {
        if (d->has_dat) {
            sb_append(sb, d->dat.data ? d->dat.data : "");
            if (d->waiting) sb_append(sb, "<div class='loadMoreW'>Loading...</div>");
            else if (!t->home && !t->send_id && !t->no_pages) sb_appendf(sb, "<div class='loadMoreD' onclick='windows[%d].loadMore(this)'>Load More</div>", is_mods);
        } else {
            sb_append(sb, "<div class='loadMoreW'>Loading...</div>");
        }
    } else {
        sb_append(sb, "<div class='loadMoreW'>Loading...</div>");
        if (d) {
            d->used = 1;
            d->load_ind = 0;
        }
        list_request(is_mods, tab, 0);
    }
}

// windows[0].gen()
static char *maps_gen(void) {
    HubList *list = &g_hub.maps;
    const HubTab *t = list_tab(0, list->tab_index);
    if (!t) t = &k_map_tabs[0];
    StrBuf tabs = {0};
    sb_append(&tabs, "<div class='bigMenTabs' style='margin-top:25px'>");
    for (int i = 0; i < MAP_TAB_COUNT; ++i) {
        if (k_map_tabs[i].hide) continue;
        sb_appendf(&tabs, "<div class='bigMenTab%s' onclick='playSelect();", list->tab_index == i ? " bigMenTabA" : "");
        if (k_map_tabs[i].click) sb_append(&tabs, k_map_tabs[i].click);
        else sb_appendf(&tabs, "windows[0].switchTab(%d)", i);
        sb_appendf(&tabs, "' onmouseenter='playTick(0.1);'>%s</div>", k_map_tabs[i].txt ? k_map_tabs[i].txt : k_map_tabs[i].n);
    }
    sb_append(&tabs, "</div>");
    char *tabs_s = sb_finish(&tabs);
    StrBuf body = {0}, out = {0};
    if (t->home) {
        list_body(&body, list, 0, t);
        char *b = sb_finish(&body);
        sb_appendf(&out, "<div style='margin-top:15px;margin-bottom:15px;color: #fff;'>%s</div>", tabs_s);
        // v3540: one of the featured games (API_CLIENT.listMaps(2) at connect) as the big card
        const int n = cJSON_GetArraySize(list->home_rows);
        if (n >= 1) {
            const cJSON *f = cJSON_GetArrayItem(list->home_rows, rand_int(0, n - 1));
            const char *name = row_str2(f, "mna", "map_name"), *creator = row_str2(f, "ctn", "creatorname");
            const char *image = row_str2(f, "map_image", NULL), *descr = row_str2(f, "map_description", NULL);
            char pl[32], vt[32];
            comma_number(pl, sizeof(pl), row_num2(f, "map_pl", NULL));
            comma_number(vt, sizeof(vt), row_num2(f, "vte", "map_votes"));
            sb_append(&out, "<div class='bigMFeatHold' style='background-image:linear-gradient(to right, black, rgba(0, 0, 0, 0.4)),url(");
            sb_append(&out, image && *image ? image : "./img/noimg.png"); // KrunkNative: the catalogue thumbnail, no user-assets thumb
            sb_appendf(&out, ");'><div id='bigMFeatName'>%s</div><div id='bigMFeatVbar'><span style='color:#fff;'>%s</span><span class='material-icons' style='margin-top:-8px;margin-left:0px;font-size:46px;vertical-align:middle;color:rgba(255,255,255,0.5);margin-right:10px;'>play_arrow</span>%s<span class='material-icons' style='margin-left:10px;font-size:33px;vertical-align:top;color:rgba(255,255,255,0.5);'>thumb_up</span><div style='color:#7d7d7d;margin-left:20px;margin-top:-12px;display:inline-block;vertical-align:middle;'>by <a target='_blank' href='/social.html?p=profile&q=%s' style='color: #2196F3;'>%s</a></div></div><div id='bigMFeatDesc'>%s</div><div id='bigMFeatIBtn' class='button' onclick='openURL(&quot;/social.html?p=map&q=%s&quot;, 1)' style='color:#fff !important;'>?</div><div id='bigMFeatPBtn' class='btnAnim buttonS' onclick='openURL(&quot;/?play=%s&quot;, 1)' style='color:#fff !important;'>Play Now</div></div>",
                       name ? name : "", pl, vt, creator ? creator : "", creator ? creator : "", descr ? descr : "undefined", name ? name : "", name ? name : "");
            // the featured video (config.fetMapVids / map_videolink) is a YouTube iframe: no
            // embedded video natively, the box stays
            sb_append(&out, "<div class='bigMFeatVid'></div>");
        }
        sb_appendf(&out, "<div id='mapList'>%s</div>", b);
        free(b);
        free(tabs_s);
        return sb_finish(&out);
    }
    if (t->search) {
        sb_append(&body, "<input type='text' id='mpSrch' class='smlInput' placeholder='Game Name or Keyword' onkeydown='if(window.event.keyCode==13)windows[0].searchMaps()' />");
        sb_append(&body, "<select id='mpCat'><option style='color:#fff' value='-1'>All</option>");
        for (int i = 0; i < MAP_CAT_COUNT; i++) sb_appendf(&body, "<option style='color:#fff;' value='%d'>%s</option>", i, k_map_cats[i]);
        sb_append(&body, "</select>");
        sb_append(&body, "<div class='searchListBtn' onclick='windows[0].searchMaps()'><span class='material-icons' style='color:#fff;font-size:45px;margin-bottom:-30px;line-height:75px'>search</span></div><div style='color:rgba(255,255,255,0.5);margin-top:10px' id='searchResultsMA'></div>");
    } else {
        list_body(&body, list, 0, t);
    }
    char *b = sb_finish(&body);
    sb_appendf(&out, "<div style='margin-top:15px;margin-bottom:15px'>%s</div>", tabs_s);
    if (!strcmp(t->n, "My Games")) sb_append(&out, "<div class='setBodH'><div class='settName'>Publish or Create a Game<a style='float:right' href='./editor.html' target='_blank'>Open Editor</a></div></div>");
    sb_appendf(&out, "<div id='mapList'>%s</div>", b);
    free(b);
    free(tabs_s);
    return sb_finish(&out);
}

// windows[1].gen()
static char *mods_gen(void) {
    HubList *list = &g_hub.mods;
    const HubTab *t = list_tab(1, list->tab_index);
    if (!t) t = &k_mod_tabs[0];
    StrBuf out = {0}, body = {0};
    sb_append(&out, "<div style='margin-top:30px;margin-bottom:10px'><div class='bigMenTabs' style='margin-top:25px;'>");
    for (int i = 0; i < MOD_TAB_COUNT; ++i) {
        const char *sel = list->tab_index == i ? " bigMenTabA" : "";
        if (k_mod_tabs[i].no_load) sb_appendf(&out, "<div class='bigMenTab%s' style='background-color: #993299!important;' ' onmouseenter='playTick(0.1);' onclick='playSelect();publishModPopup(true)'>Publish</div>", sel);
        else sb_appendf(&out, "<div class='bigMenTab%s' onmouseenter='playTick(0.1);' onclick='playSelect();windows[1].switchTab(%d)'>%s</div>", sel, i, k_mod_tabs[i].txt ? k_mod_tabs[i].txt : k_mod_tabs[i].n);
    }
    sb_append(&out, "</div></div>");
    if (t->search) sb_append(&body, "<input type='text' id='mdSrch' class='smlInput' placeholder='Mod Name or Keyword' onkeydown='if(window.event.keyCode==13)windows[1].searchMods()' /><div class='searchListBtn' onclick='windows[1].searchMods()'><span class='material-icons' style='color:#fff;font-size:45px;margin-bottom:-30px;line-height:75px'>search</span></div><div style='color:rgba(0,0,0,0.5);margin-top:10px' id='searchResultsMO'></div>");
    else list_body(&body, list, 1, t);
    if (t->home) {
        // listData.list: one of the loaded mods as the big card
        const int n = cJSON_GetArraySize(list->home_rows);
        if (n >= 1) {
            const cJSON *f = cJSON_GetArrayItem(list->home_rows, rand_int(0, n - 1));
            const char *name = row_str2(f, "mna", "mod_name"), *creator = row_str2(f, "ctn", "creatorname");
            char vt[32];
            comma_number(vt, sizeof(vt), row_num2(f, "vte", "mod_votes"));
            sb_appendf(&out, "<div class='bigMFeatHold' style='background-image:linear-gradient(to right, black, rgba(0, 0, 0, 0.4)),url(.png);'><div id='bigMFeatName' style='top:185px;'>%s</div><div id='bigMFeatVbar' style='top:260px;'>%s<span class='material-icons' style='margin-left:10px;font-size:33px;vertical-align:top;color:rgba(255,255,255,0.5);'>thumb_up</span><div style='color:#7d7d7d;margin-left:20px;margin-top:-12px;display:inline-block;vertical-align:middle;'>by <a style='color: #2196F3;'>%s</a></div></div><div id='bigMFeatPBtn' class='btnAnim' style='top:225px;width:232px' onclick='openMod(&quot;",
                       name ? name : "", vt, creator ? creator : "");
            sb_append_js_escape(&out, name ? name : "");
            sb_append(&out, "&quot;)'>Load Mod <span class='material-icons' style='font-size:50px;color:#fff;vertical-align:middle;margin-top:-16px;margin-left:5px'>move_to_inbox</span></div></div><div class='bigMFeatVid'><div style='color:rgba(255,255,255,0.4);line-height:320px;'>Missing Preview</div></div>");
            sb_append(&out, "<div class='mapListCat' style='margin-bottom:10px;padding-bottom:10px;margin-top:20px;'>Featured Mods</div>");
        }
    }
    char *b = sb_finish(&body);
    sb_appendf(&out, "<div style='color:rgba(255,255,255,0.3)' id='modList'>%s</div>", b);
    free(b);
    return sb_finish(&out);
}

// windows[n].update(): mapsHolder / modsHolder.innerHTML = gen()
static void list_update(const int is_mods) {
    char *html = is_mods ? mods_gen() : maps_gen();
    set_html(is_mods ? "modsHolder" : "mapsHolder", html);
    free(html);
}

// windows[n].switchTab(e, a)
static void list_switch_tab(const int is_mods, const int tab, const int no_update) {
    HubList *list = is_mods ? &g_hub.mods : &g_hub.maps;
    if (list->tab_index != tab && list_tab(is_mods, tab)) {
        list->tab_index = tab;
        if (!no_update) list_update(is_mods);
    }
}

// windows[0].mapsLoaded / windows[1].modsLoaded for every tab waiting on this API index
static void list_loaded(const int is_mods, const cJSON *rows, const int api, const cJSON *error) {
    HubList *list = is_mods ? &g_hub.mods : &g_hub.maps;
    // the featured list of the maps Home card (v3540: listMaps(2) at connect, shuffled; its own
    // callback, before the maps window exists)
    if (!is_mods && api == 1 && cJSON_IsArray(rows) && cJSON_GetArraySize(rows) && !list->home_rows) {
        list->home_rows = cJSON_Duplicate(rows, 1);
        shuffle_rows(list->home_rows);
        const HubTab *t = list_tab(0, list->tab_index);
        if (t && t->home && el("mapList")) list_update(0);
    }
    if (!el(is_mods ? "modList" : "mapList")) return; // `if (mapList)`
    for (int tab = 0; tab < (is_mods ? MOD_TAB_COUNT : MAP_TAB_COUNT); tab++) {
        const HubTab *t = list_tab(is_mods, tab);
        HubListData *d = list_data(list, tab);
        if (!t || t->api != api || !d || !d->used || (d->has_dat && !d->waiting)) continue;
        d->waiting = 0;
        if (cJSON_IsArray(rows) && cJSON_GetArraySize(rows)) {
            if (cJSON_IsString(error)) {
                if (!d->has_dat) {
                    set_html(is_mods ? "modList" : "mapList", error->valuestring);
                    list_reset(d);
                }
                continue;
            }
            StrBuf sb = {0};
            if (is_mods) append_mod_cards(&sb, rows, tab);
            else append_map_cards(&sb, rows, tab);
            char *cards = sb_finish(&sb);
            sb_append(&d->dat, cards);
            d->has_dat = 1;
            free(cards);
            if (is_mods && t->home) {
                cJSON_Delete(list->home_rows);
                list->home_rows = cJSON_Duplicate(rows, 1);
            }
            if (list->tab_index == tab) list_update(is_mods);
        } else if (!d->has_dat) {
            set_html(is_mods ? "modList" : "mapList", is_mods ? STR_WIN_MODS_NONE : STR_WIN_MAPS_NONE);
            list_reset(d);
        }
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
    list_request(is_mods, list->tab_index, d->load_ind);
}

// windows[n].searchResp(rows) (f550)
static void list_search_resp(const int is_mods, const cJSON *rows) {
    StrBuf sb = {0};
    if (cJSON_IsArray(rows) && cJSON_GetArraySize(rows)) {
        sb_append(&sb, "<div style='height:10px'></div>");
        if (is_mods) append_mod_cards(&sb, rows, list_search_id(1));
        else append_map_cards(&sb, rows, list_search_id(0));
    } else {
        sb_append(&sb, is_mods ? STR_WIN_MODS_NONE : STR_WIN_MAPS_NONE);
    }
    char *html = sb_finish(&sb);
    set_html(is_mods ? "searchResultsMO" : "searchResultsMA", html);
    free(html);
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
        // the market ambience loops (ambient_4 / ambient_8) are not played
        hide_all(0);
    }
    // v3.1.1 pages v7.1.4 dropped (no holder, no nav button: v7's updateWindow would throw on
    // `document.getElementById(v).style`): the feed instead
    const char *page = e && *e ? e : "feed";
    if (!str_in(page, k_nav_pages, sizeof(k_nav_pages) / sizeof(k_nav_pages[0])) &&
        !str_in(page, k_sub_pages, sizeof(k_sub_pages) / sizeof(k_sub_pages[0]))) page = "feed";
    snprintf(g_hub.v, sizeof(g_hub.v), "%s", page);
    snprintf(g_hub.k, sizeof(g_hub.k), "%s", t ? t : "");
    snprintf(g_hub.x, sizeof(g_hub.x), "%s", n ? n : "");
    snprintf(g_hub.b, sizeof(g_hub.b), "%s", a ? a : "");
    snprintf(g_hub.ja_k, sizeof(g_hub.ja_k), "%s", g_hub.k);
    if (!g_hub.b[0]) {
        if (!strcmp(g_hub.v, "market") || !strcmp(g_hub.v, "itemsales") || !strcmp(g_hub.v, "listing")) snprintf(g_hub.b, sizeof(g_hub.b), "market");
        else if (!strcmp(g_hub.v, "items")) snprintf(g_hub.b, sizeof(g_hub.b), "items");
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
            if (el(id)) html_dom_set_class(el(id), "bigMenTabA", 0);
        }
        snprintf(id, sizeof(id), "m_%s", g_hub.b);
        if (el(id)) html_dom_set_class(el(id), "bigMenTabA", 1);
    }
    snprintf(g_hub.S, sizeof(g_hub.S), "%s_%s", g_hub.v, g_hub.b[0] ? g_hub.b : "null");
    g_hub.has_s = 1;
    if (g_hub.connected && !s) request_page();
}

// ------------------------------------------------------------------ brain data views (social.js io_0 branches)

// COLORS.getClanCol(tag, fallback) (v7.1.4: the clan war ranks are gone)
static const char *clan_col(const char *tag, const char *fallback) {
    if (tag && !strcmp(tag, "DEV")) return "#40C4FF";
    if (tag && !strcmp(tag, "VIP")) return "#11D445";
    if (tag && !strcmp(tag, "CMD")) return "#993299";
    return fallback;
}

static double jnum(const cJSON *o, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (cJSON_IsNumber(v)) return v->valuedouble;
    if (cJSON_IsBool(v)) return cJSON_IsTrue(v);
    if (cJSON_IsString(v)) return strtod(v->valuestring, NULL);
    return 0.0;
}

static const char *jstr(const cJSON *o, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

// User text the brain serves already HTML-escaped (kn_html: & < > " ', for the page's
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

// UTILS.getReadableTime(ms) (v7.1.4: years too; no seconds)
static void readable_time(char *out, const size_t n, const double ms) {
    long long sec = (long long) floor(ms / 1000.0), m = sec / 60, h = m / 60;
    m %= 60;
    long long d = h / 24;
    h %= 24;
    const long long y = d / 365;
    d %= 365;
    char yp[24] = "", dp[24] = "", hp[24] = "";
    if (y) snprintf(yp, sizeof(yp), "%lldy ", y);
    if (d) snprintf(dp, sizeof(dp), "%lldd ", d);
    if (h) snprintf(hp, sizeof(hp), "%lldh ", h);
    snprintf(out, n, "%s%s%s%lldm ", yp, dp, hp, m);
}

// UTILS.commaFloat(n): commas in the integer part only
static void comma_float(char *out, const size_t n, const char *num) {
    const char *dot = strchr(num, '.');
    char ip[64];
    snprintf(ip, sizeof(ip), "%.*s", (int) (dot ? (size_t) (dot - num) : strlen(num)), num);
    char c[80];
    comma_number(c, sizeof(c), atof(ip));
    snprintf(out, n, "%s%s", c, dot ? dot : "");
}

static void comma_fixed(char *out, const size_t n, const double v, const int places) {
    char raw[64];
    if (places < 0) js_num(raw, sizeof(raw), v);
    else snprintf(raw, sizeof(raw), "%.*f", places, v);
    comma_float(out, n, raw);
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

// config.leaderCal.player_score(score): level icon + level
static void append_level_cell(StrBuf *sb, const double score) {
    int lvl = (int) floor(0.03 * sqrt(score > 0 ? score : 0));
    if (lvl < 1) lvl = 1;
    int icon = (int) (round(lvl / 2.0) * 2) - 1; // roundToNearest(2) - 1, clamped to [0, maxLevel 102 - 1]
    if (icon > 101) icon = 101;
    if (icon < 0) icon = 0;
    sb_appendf(sb, "<span class='floatR' style='color:rgba(255,255,255,0.5);'><img src='./img/levels/%d.png' class='rnkIcon'>%d</span>", icon, lvl);
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
    const AccountInfo *acc = g_hub.logged_in ? account_get() : NULL;
    const HtmlElem pic = el("socialPic");
    if (pic) {
        char src[64];
        snprintf(src, sizeof(src), "./textures/classes/icon_%d.png", (int) jnum(stats, "c"));
        html_dom_set_attr(pic, "src", src);
    }
    const char *clan = jstr(t, "player_clan");
    const int premium = jnum(t, "player_premium") > 0;
    const char *alias = jstr(t, "player_alias");
    char num[64], num2[64];
    StrBuf sb = {0};
    sb_appendf(&sb, "<div class='onStatus onStatProf' style='background:%s'></div>", jnum(t, "st") ? "#27ab32" : "#7f7f7f");
    sb_append(&sb, "<div class='leftTopHolder'>");
    if (jnum(t, "partner_approved") >= 2) sb_append(&sb, "<i class='material-icons' style='margin-right:-2px;font-size:32px;color:#993299;vertical-align:middle;margin-top:-14px;'>verified</i>");
    if (premium) sb_append(&sb, "<i class='material-icons' style='margin-right:-2px;font-size:32px;color:#FBC02D;vertical-align:middle;margin-top:-14px;'>beenhere</i>");
    if (jnum(t, "player_featured")) sb_append(&sb, "<i class='material-icons' style='margin-right:1px;font-size:32px;color:#40C4FF;vertical-align:middle;margin-top:-14px;'>check_circle</i>");
    if (premium && alias && alias[0]) {
        sb_append(&sb, "<span id='nameSwitch' onclick='switchNames()'>");
        sb_append_esc(&sb, g_hub.profile_name);
        sb_append(&sb, "</span><span style='display:none;color:#FBC02D' id='nameSwitch' onclick='switchNames()'>");
        sb_append_esc(&sb, alias);
        sb_append(&sb, "</span>");
    } else {
        sb_append(&sb, "<span id='nameSwitch'>");
        sb_append_esc(&sb, g_hub.profile_name);
        sb_append(&sb, "</span>");
    }
    if (clan && clan[0]) {
        sb_appendf(&sb, " <a target='_blank' style='color:%s' href='/social.html?p=clan&q=", clan_col(clan, "rgba(255,255,255,0.6)"));
        sb_append_esc(&sb, clan);
        sb_append(&sb, "'>[");
        sb_append_esc(&sb, clan);
        sb_append(&sb, "]</a>");
    }
    get_date(num, sizeof(num), jstr(t, "player_datenew"));
    sb_appendf(&sb, "</div><div class='leftMidHolder'>Since <span style='color:rgba(255,255,255,0.6)'>%s</span></div><div class='leftBottomHolder'>", num);
    format_num(num, sizeof(num), g_hub.profile_followed);
    format_num(num2, sizeof(num2), g_hub.profile_following);
    sb_appendf(&sb, "<div class='follL' id='followerC' onclick='viewFollowers()'>%s<span class='follCount'>Followers</span></div><div class='follL' id='followC' style='margin-left:10px;' onclick='viewFollowing()'>%s<span class='follCount'>following</span></div></div>", num, num2);
    sb_appendf(&sb, "<div class='rightTopHolder'><span class='material-icons canFlag%s' id='flagBtn' onmouseover=\"SOUND.play('tick_0',0.1)\" onclick='flagPopup();'>flag</span><span class='material-icons' id='reprtBtn' onmouseover=\"SOUND.play('tick_0',0.1)\" onclick='reportPopup();'>report</span><span class='material-icons' id='shareBtn' onmouseover=\"SOUND.play('tick_0',0.1)\" onclick='shareProfile();'>share</span></div>",
               acc && (acc->is_dev || acc->is_mod) ? "" : " hide");
    sb_append(&sb, "<div class='rightMidHolder'>");
    const char *twitch = jstr(t, "player_twitchname");
    if (twitch && *twitch) {
        sb_append(&sb, "<img id='twitchBtn' src='./img/twitch.png' onClick='openLink(&quot;https://twitch.tv/");
        sb_append_esc(&sb, twitch);
        sb_append(&sb, "&quot;, &quot;_blank&quot;)'>");
    }
    sb_append(&sb, "</div>");
    // the follow state [{f}]: the reply's 7th argument
    const cJSON *follow = cJSON_GetArrayItem(cJSON_GetArrayItem(extra, 6), 0);
    const int not_following = !follow || !jnum(follow, "f");
    sb_append(&sb, "<div class='rightBottomHolder'><div class='bottomBtn' id='tradeBtn' onmouseover=\"SOUND.play('tick_0',0.1)\" onclick='tradePopup();'>Trade</div><div class='bottomBtn' id='giftBtn' onmouseover=\"SOUND.play('tick_0',0.1)\" onclick='giftPopup();'>Send Gift</div>");
    sb_appendf(&sb, "<div class='bottomBtn %s' id='followBtn' onmouseover=\"SOUND.play('tick_0',0.1)\" onclick='followUser();'>%s</div>", not_following ? "follow" : "unfollow", not_following ? "Follow" : "Unfollow");
    sb_appendf(&sb, "<div class='bottomBtn canVerify%s' id='verifyBtn' onmouseover=\"SOUND.play('tick_0',0.1)\" onclick='verifyPopup();'>%s</div>", acc && acc->is_dev ? "" : " hide", jnum(t, "player_featured") ? "Unverify" : "Verify");
    sb_appendf(&sb, "<div class='bottomBtn canBadge%s' id='badgeBtn' onmouseover=\"SOUND.play('tick_0',0.1)\" onclick='badgePopup();'>Give Badge</div>", acc && acc->is_dev ? "" : " hide");
    char *html = sb_finish(&sb);
    set_html("rightProfile", html);
    free(html);
    if (g_hub.profile_name[0] && acc && strcmp(acc->name, g_hub.profile_name) != 0) set_display("followBtn", "inline-block");
    if (acc && acc->level >= TRADE_MIN_LVL && k >= TRADE_MIN_LVL) set_display("tradeBtn", "inline-block");

    // pTabContent_about: Overall Stats with the four config.statTabs
    const double kills = jnum(t, "player_kills"), deaths = jnum(t, "player_deaths"), games = jnum(t, "player_games_played"), wins = jnum(t, "player_wins");
    const double lo = k > 1 ? pow(k / 0.03, 2) : 0, hi = pow((k + 1) / 0.03, 2);
    sb_append(&sb, "<div class='scrollItemNew'><span class='scrollItemHead'>Overall Stats</span><div class='material-icons toggleView' onclick='toggleView(this)'>expand_less</div><div>");
    js_num(num, sizeof(num), (double) (long long) (score - lo) / (double) (long long) (hi - lo) * 100.0);
    sb_appendf(&sb, "<div class='xpBar'><div class='xpBarB' style='width:%s%%'></div><span class='xpBarV'>%lld / %lld</span></div>", num, (long long) (score - lo), (long long) (hi - lo));
    sb_append(&sb, "</div><div id='statHolder' style='margin-top: 0px;'><div class='bigMenTabs' style='margin-bottom: 5px;table-layout: fixed;width: calc(100% - 18px);margin-left: 9px;background-color: rgba(0, 0, 0, 0.5);'>");
    static const char *const stat_tabs[4] = {"General Stats", "Killstreak Stats", "Other Stats", "Class Stats"};
    for (int i = 0; i < 4; ++i)
        sb_appendf(&sb, "<div id='statTab%d' class='bigMenTab%s' onclick='playSelect(0.1);switchStat(%d)' style='padding-left: 5px; padding-right: 5px; padding-top: 10px; padding-bottom: 5px; line-height: 35px;font-size: 14px;'onmouseenter='playTick();'>%s</div>",
                   i, i == 0 ? " bigMenTabA" : "", i, stat_tabs[i]);
    sb_append(&sb, "</div><div class='statTabB0'>");
#define PST(label, value) sb_appendf(&sb, "<div class='pSt'>%s<strong>%s</strong></div>", label, value)
    char v[64];
    snprintf(v, sizeof(v), "%d", k);
    PST("LVL", v);
    comma_fixed(v, sizeof(v), jnum(t, "player_funds"), -1);
    PST("KR", v);
    comma_fixed(v, sizeof(v), jnum(t, "player_elo4"), 2);
    PST("Junk", v);
    comma_fixed(num, sizeof(num), jnum(t, "player_skinvalue"), -1);
    snprintf(v, sizeof(v), " ~%s", num);
    PST("Inventory", v);
    comma_fixed(v, sizeof(v), score, -1);
    PST("Score", v);
    comma_fixed(v, sizeof(v), score / (kills > 1 ? kills : 1), 2);
    PST("SPK", v);
    comma_fixed(v, sizeof(v), kills, -1);
    PST("Kills", v);
    comma_fixed(v, sizeof(v), deaths, -1);
    PST("Deaths", v);
    comma_fixed(v, sizeof(v), kills / (deaths > 1 ? deaths : 1), 4);
    PST("KDR", v);
    comma_fixed(v, sizeof(v), kills / (games > 1 ? games : 1), 4);
    PST("KPG", v);
    comma_fixed(v, sizeof(v), games, -1);
    PST("Games", v);
    js_num(v, sizeof(v), round(wins / (games - wins > 1 ? games - wins : 1) * 1000.0) / 1000.0); // .round(3)
    PST("W/L", v);
    comma_fixed(v, sizeof(v), wins, -1);
    PST("Wins", v);
    comma_fixed(v, sizeof(v), games - wins > 0 ? games - wins : 0, -1);
    PST("Losses", v);
    static const char *const stat_keys[][2] = {{"Assists", "ast"}, {"Melee", "mk"}, {"Beatdowns", "fk"}, {"Bullseyes", "tmk"}, {"Headshots", "hs"},
                                              {"Legshots", "ls"}, {"Wallbangs", "wb"}, {"Shots", "s"}};
    for (size_t i = 0; i < sizeof(stat_keys) / sizeof(stat_keys[0]); i++) {
        comma_fixed(v, sizeof(v), jnum(stats, stat_keys[i][1]), -1);
        PST(stat_keys[i][0], v);
    }
    const double shots = jnum(stats, "s"), hits = fmin(shots, jnum(stats, "h"));
    comma_fixed(v, sizeof(v), hits, -1);
    PST("Hits", v);
    comma_fixed(v, sizeof(v), shots - hits, -1);
    PST("Misses", v);
    comma_fixed(num, sizeof(num), round((shots ? hits / shots * 100.0 : 0.0) * 100.0) / 100.0, -1);
    snprintf(v, sizeof(v), "%s%%", num);
    PST("Accuracy", v);
    readable_time(v, sizeof(v), jnum(t, "player_timeplayed"));
    PST("Time Played", v);
    sb_append(&sb, "</div><div class='statTabB1 hide'>");
    static const char *const streak_keys[][2] = {{"Nukes", "n"}, {"Airdrops", "ad"}, {"Airdrops Stolen", "sad"}, {"Slimers", "sl"}, {"Juggernauts", "jg"}};
    for (size_t i = 0; i < sizeof(streak_keys) / sizeof(streak_keys[0]); i++) {
        comma_fixed(v, sizeof(v), jnum(stats, streak_keys[i][1]), -1);
        PST(streak_keys[i][0], v);
    }
    comma_fixed(v, sizeof(v), jnum(stats, "kjg"), -1);
    sb_appendf(&sb, "<div class='pSt' style='font-size: 9.5px;padding-left: 0px;padding-right: 0px;width: 140px;'>Juggernauts Killed<strong>%s</strong></div>", v);
    comma_fixed(v, sizeof(v), jnum(stats, "wm"), -1);
    PST("War Machines", v);
    sb_append(&sb, "</div><div class='statTabB2 hide'>");
    comma_fixed(v, sizeof(v), jnum(stats, "crc"), -1);
    PST("Crouches", v);
    comma_fixed(v, sizeof(v), jnum(stats, "spry"), -1);
    PST("Sprays Placed", v);
    sb_appendf(&sb, "<div class='pSt' style='padding-left: 9px;padding-right: 9px;width: 122px;'>Helpful Reports<strong>%d/%d</strong></div>", (int) jnum(t, "player_jobratingpositive"), (int) jnum(t, "player_jobrating"));
    comma_fixed(v, sizeof(v), jnum(t, "player_calls"), -1);
    sb_appendf(&sb, "<div class='pSt' title='Amount of times you called KPD'>Total Calls<strong>%s</strong></div>", v);
    comma_fixed(v, sizeof(v), jnum(t, "player_taggedaccounts"), -1);
    sb_appendf(&sb, "<div class='pSt' title='Amount of users banned from calls you made to KPD' style='padding-left: 8px;padding-right: 8px;width: 124px;'>Bans from calls<strong>%s</strong></div>", v);
#undef PST
    sb_append(&sb, "</div><div class='statTabB3 hide'>");
    for (size_t c = 0; c < sizeof(g_classes) / sizeof(g_classes[0]); c++) {
        const ClassConfig *cc = &g_classes[c];
        if (!cc->name || !cc->name[0]) continue;
        char key[16];
        snprintf(key, sizeof(key), "c%zu", c);
        const double xp = jnum(stats, key);
        const double e2 = 0.03 * sqrt(xp > 0 ? xp : 0);
        int lvl = (int) floor(e2);
        const int prog = (int) round((e2 - lvl) * 100);
        const double clo = lvl > 1 ? pow(lvl / 0.03, 2) : 0, chi = pow((lvl + 1) / 0.03, 2);
        lvl = lvl + 1 > 1 ? lvl + 1 : 1;
        const int w = cc->loadout ? cc->loadout[0] : -1;
        const char *weapon = w >= 0 && w < WEAPON_COUNT && g_weapons[w] && g_weapons[w]->name ? g_weapons[w]->name : "";
        sb_appendf(&sb, "<div class='classCard'><img class='classImg' src='./textures/classes/icon_%zu.png'/><div class='className'>%s</div><div class='classWeap'>%s</div><div class='classXPBar'><div class='clsXPBarC' style='width:%d%%'></div><div class='clsXPBarV'>%lld / %lld</div></div><div class='classLvl'>Lvl %d</div></div>",
                   c, cc->name, weapon, prog, (long long) (xp - clo), (long long) (chi - clo), lvl);
    }
    sb_append(&sb, "</div></div></div><div></div><div></div>");
    sb_append(&sb, "<div class='scrollItemNew'><span class='scrollItemHead'>Sales</span><div class='material-icons toggleView' onclick='toggleView(this)'>expand_less</div><div style='margin-top:10px;margin-left: 8px;'><span style='color: rgba(255,255,255,0.4);'>Data Range</span><select class='rangeSelect' onchange='changeDateRange2(this.value)'><option value='0'>1 week</option><option value='1'>2 weeks</option><option value='2'>3 weeks</option><option value='3' selected>1 month</option><option value='4'>2 months</option><option value='5'>3 months</option><option value='6'>4 months</option></select></div><div class='graphHolder'><div class='graphHeader'><span class='graphHeaderTxt'>Market Sales</span><span id='totalSalesHeader' class='graphHeaderTotal'>0</span></div><canvas id='profSales'></canvas></div></div><div></div>");
    html = sb_finish(&sb);
    set_html("pTabContent_about", html);
    free(html);
    set_html("pTabContent_activity", "");
    set_html("pTabContent_listings", "");
    // badges (v7 data/badges.js): KrunkNative has no badge table yet: the none text
    set_html("pTabContent_badges", "<div class='scrollItemNew'><div class='noneText'>No Badges found for this user</div></div>");
    // creations: skins (store skins by this creator), games r, mods l, assets f
    const cJSON *games_rows = cJSON_GetArrayItem(extra, 4), *mods_rows = cJSON_GetArrayItem(extra, 5), *assets_rows = cJSON_GetArrayItem(extra, 7);
    int skins_n = 0;
    for (int i = 0; i < g_viewer_skin_count; i++) {
        const char *cr = g_viewer_skins[i].creator;
        if (cr && g_hub.profile_name[0] && strstr(cr, g_hub.profile_name)) skins_n++;
    }
    sb_appendf(&sb, "<div class='scrollItemNew'><div class=\"creationTabs\">\n\t\t\t\t<div class=\"creationTab creationTabA\" id=\"creationTab_skins\" onmouseenter=\"playTick();\" onclick=\"playSelect(0.1);openCreationTab('skins')\">Skins [%d]</div>\n\t\t\t\t<div class=\"creationTab\" id=\"creationTab_games\" onmouseenter=\"playTick();\" onclick=\"playSelect(0.1);openCreationTab('games')\">Games [%d]</div>\n\t\t\t\t<div class=\"creationTab\" id=\"creationTab_mods\" onmouseenter=\"playTick();\" onclick=\"playSelect(0.1);openCreationTab('mods')\">Mods [%d]</div>\n\t\t\t\t<div class=\"creationTab\" id=\"creationTab_assets\" onmouseenter=\"playTick();\" onclick=\"playSelect(0.1);openCreationTab('assets')\">Assets [%d]</div>\n\t\t\t</div>",
               skins_n, cJSON_GetArraySize(games_rows), cJSON_GetArraySize(mods_rows), cJSON_GetArraySize(assets_rows));
    sb_append(&sb, "<div class='creationContent selected' id='creationContent_skins'>");
    if (skins_n) {
        for (int i = g_viewer_skin_count - 1; i >= 0; i--) { // reversed
            const ViewerSkin *sk = &g_viewer_skins[i];
            if (!sk->creator || !strstr(sk->creator, g_hub.profile_name)) continue;
            char path[256], url[256];
            viewer_skin_preview_path(sk, path, sizeof(path));
            html_dom_asset_url(path, url, sizeof(url));
            sb_appendf(&sb, "<div class='creationSkinItem'><div class='creationSkinImgH' onclick='viewItem(%d)'><img draggable='false' class='creationSkinImg' loading='lazy' src='%s'/></div><div class='creationSkinName'>", i, url);
            sb_append_esc(&sb, sk->name);
            sb_append(&sb, "</div></div>");
        }
    } else {
        sb_append(&sb, "<div class='noneText'>No published Skins from this user</div>");
    }
    sb_append(&sb, "</div>");
    static const char *const heads[3] = {"games", "mods", "assets"};
    static const char *const nones[3] = {"No published Games from this user", "No published Mods from this user", "No published Assets from this user"};
    const cJSON *lists[3] = {games_rows, mods_rows, assets_rows};
    for (int i = 0; i < 3; i++) {
        sb_appendf(&sb, "<div class='creationContent' id='creationContent_%s'>", heads[i]);
        const cJSON *r;
        int any = 0;
        cJSON_ArrayForEach(r, lists[i]) {
            const char *nm = i == 2 ? jstr(r, "nm") : row_str2(r, "mna", i == 0 ? "map_name" : "mod_name");
            if (!nm) continue;
            any = 1;
            const double age = i == 2 ? row_age_days(r, "ini", NULL) : row_age_days(r, "dte", i == 0 ? "map_date" : "mod_date");
            sb_append(&sb, "<div class='mapListItem2'>");
            if (age >= 0 && age <= 7) sb_append(&sb, "<div class='mapAgeD' style='background-color:#da00ff'>NEW</div>");
            if (i == 2) {
                sb_append(&sb, "<div class='mapListThumbHolder' style='width:204px;height: 90px;'><img class='mapListThumb' loading='lazy' style='width:204px;height: 90px;' src='./img/noimg.png' onerror='this.onerror=null;this.src=\"./img/noimg.png\";'/></div><div style='margin-top:1px;text-overflow: ellipsis;overflow: hidden;white-space: nowrap;'><a href='javascript:;'/>");
                sb_append_served(&sb, nm);
                sb_append(&sb, "</a></div></div>");
                continue;
            }
            const char *img = jstr(r, i == 0 ? "map_image" : "mod_image");
            const char *open = i == 0 ? "openMap" : "openMod";
            const char *by = row_str2(r, "ctn", "creatorname");
            if (!by) by = g_hub.profile_name;
            char votes[32];
            js_num(votes, sizeof(votes), row_num2(r, "vte", i == 0 ? "map_votes" : "mod_votes"));
            sb_append(&sb, "<div class='mapListThumbHolder' style='width:204px;height:90px;'><img draggable='false' class='mapListThumb' loading='lazy' style='width:204px;height: 90px;' src='");
            sb_append_esc(&sb, img && *img ? img : "./img/noimg.png");
            sb_appendf(&sb, "' onclick='%s(&quot;", open);
            sb_append_js_escape(&sb, nm);
            sb_appendf(&sb, "&quot;)'/><div class='mapsVotes' style='top: 68px;'>%s<span class='material-icons' style='margin-left: 5px;color:rgba(255, 255, 255, 0.5);vertical-align: middle;margin-top: -3px;font-size: 18px;'>thumb_up</span></div></div><div style='margin-top:1px;text-overflow: ellipsis;overflow: hidden;white-space: nowrap;'><a href='javascript:;' onclick='%s(&quot;", votes, open);
            sb_append_js_escape(&sb, nm);
            sb_append(&sb, "&quot;)'/>");
            sb_append_served(&sb, nm);
            sb_append(&sb, "</a></div><div style='margin-top:1px;display: block;color: rgba(255,255,255,0.2);font-size: 15px;text-align: left;'>by <span onclick='openProfile(&quot;");
            sb_append_esc(&sb, by);
            sb_append(&sb, "&quot;)' style='color: rgba(255,255,255,0.6);'>");
            sb_append_esc(&sb, by);
            sb_append(&sb, "</span></div></div>");
        }
        if (!any) sb_appendf(&sb, "<div class='noneText'>%s</div>", nones[i]);
        sb_append(&sb, "</div>");
    }
    sb_append(&sb, "</div>");
    html = sb_finish(&sb);
    set_html("pTabContent_creations", html);
    free(html);
    set_display("hackText", jnum(t, "player_hack") ? "block" : "none");
    // io.send("pst", t.player_name, vLN33): the sales graph data (vLN33 = 3: 1 month)
    cJSON *f = cJSON_CreateArray();
    cJSON_AddItemToArray(f, cJSON_CreateString("pst"));
    cJSON_AddItemToArray(f, cJSON_CreateString(g_hub.profile_name));
    cJSON_AddItemToArray(f, cJSON_CreateNumber(30));
    hub_send(f);
    cJSON_Delete(stats);
}

// config.leaderCal[a](h) (v7.1.4)
static void append_leader_cal(StrBuf *sb, const char *a, const double h) {
    char n[48];
    comma_number(n, sizeof(n), h);
    if (!strcmp(a, "player_kills")) { js_num(n, sizeof(n), h); sb_appendf(sb, "%s<span class='lName'> kills</span>", n); }
    else if (!strcmp(a, "player_wins")) { js_num(n, sizeof(n), h); sb_appendf(sb, "%s<span class='lName'> wins</span>", n); }
    else if (!strcmp(a, "player_funds")) sb_appendf(sb, "<span style='color:rgba(255,255,255,0.4)'>%s</span> KR", n);
    else if (!strcmp(a, "player_skinvalue")) sb_appendf(sb, "<span style='color:rgba(255,255,255,0.4)'>~ %s</span> KR", n);
    else if (!strcmp(a, "player_followed")) sb_appendf(sb, "%s <span class='lName'>followers</span>", n);
    else if (!strcmp(a, "player_eventcount")) { js_num(n, sizeof(n), h); sb_appendf(sb, "<span class='lName'>found</span> %s/187", n); } // config.easterEggs
    else if (!strcmp(a, "player_eventtime")) { js_num(n, sizeof(n), h); sb_appendf(sb, "Round %s", n); }
    else if (!strcmp(a, "player_timeplayed")) {
        const double ms = h * 1.2;
        long long sec = (long long) floor(ms / 1000), m = sec / 60, hr = m / 60;
        sec %= 60;
        m %= 60;
        const long long d = hr / 24;
        hr %= 24;
        sb_append(sb, "<span style='color:rgba(255,255,255,0.5)'>");
        if (d) sb_appendf(sb, "%lldd ", d);
        if (hr) sb_appendf(sb, "%lldh ", hr);
        sb_appendf(sb, "%lldm %llds</span>", m, sec);
    } else { js_num(n, sizeof(n), h); sb_append(sb, n); }
}

static void hub_render_leaders(const char *a, const cJSON *t) {
    StrBuf sb = {0};
    sb_append(&sb, "<div class='scrollItem'>");
    const int clans = a && !strcmp(a, "player_clan");
    const cJSON *r;
    int v = 0;
    cJSON_ArrayForEach(r, t) {
        v++;
        const char *clan = jstr(r, "player_clan");
        if (clans) {
            const char *col = clan_col(clan, "rgba(255,255,255,0.6)");
            sb_appendf(&sb, "<div style='color:rgba(255,255,255,0.6)'>%d. <a target='_blank' style='color:%s' href='/social.html?p=clan&q=", v, col);
            sb_append_esc(&sb, clan ? clan : "");
            sb_appendf(&sb, "'>[<div class='clanNameFixed' style='color:%s'>", col);
            sb_append_esc(&sb, jstr(r, "clan_name") ? jstr(r, "clan_name") : "");
            sb_appendf(&sb, "</div>]</a> (%d) ", (int) jnum(r, "clan_membercount"));
            if (jnum(r, "clan_hackcount")) sb_appendf(&sb, "<span style='color:#E85656'>%d</span>", (int) jnum(r, "clan_hackcount"));
            char n[32];
            comma_number(n, sizeof(n), jnum(r, "clan_score"));
            sb_appendf(&sb, "<div style='float:right;color:rgba(255,255,255,0.7);'>%s</div></div>", n);
            continue;
        }
        const char *name = jstr(r, "player_name");
        sb_appendf(&sb, "<div style='color:rgba(255,255,255,0.6)'>%d. <div class='leaderText'>", v);
        if (jnum(r, "player_featured")) sb_append(&sb, "<i class='material-icons' style='margin-right:5px;font-size:26px;color:#40C4FF'>check_circle</i>");
        sb_append(&sb, "<a target='_blank' style='color:#fff' href='/social.html?p=profile&q=");
        sb_append_esc(&sb, name ? name : "");
        sb_appendf(&sb, "' class='lName' style='%s'>", jnum(r, "player_hack") ? "color:#E85656" : "");
        sb_append_esc(&sb, name ? name : "");
        sb_append(&sb, "</a>");
        if (clan && clan[0]) {
            sb_appendf(&sb, " <a target='_blank' style='color:%s' href='/social.html?p=clan&q=", clan_col(clan, "rgba(255,255,255,0.4)"));
            sb_append_esc(&sb, clan);
            sb_append(&sb, "'>[");
            sb_append_esc(&sb, clan);
            sb_append(&sb, "]</a>");
        }
        sb_append(&sb, "</div><div style='float:right;color:rgba(255,255,255,0.7)'>");
        if (!a || !strcmp(a, "player_score")) append_level_cell(&sb, jnum(r, "player_score"));
        else append_leader_cal(&sb, a, jnum(r, a));
        sb_append(&sb, "</div></div>");
    }
    sb_append(&sb, "</div>");
    char *html = sb_finish(&sb);
    set_html("leaderList", html);
    free(html);
}

// io_0("map"/"mod", name, row): the creation page (mapProfile / modProfile). Active servers
// (f540: the matchmaker's game list filtered to this map) are not fetched: none online.
static int g_map_tab; // vLN0588

// getMapTapHTML(canEdit)
static void append_map_tabs(StrBuf *sb, const int can_edit) {
    sb_appendf(sb, "<div class='mapTabP%s' onclick='updateMapTab(0, %s)'>About</div><div class='mapTabP%s' onclick='updateMapTab(1, %s)'>Servers</div><div class='mapTabP%s' onclick='updateMapTab(2, %s)'>Leaderboard</div>",
               g_map_tab == 0 ? " activeTab" : "", can_edit ? "true" : "false", g_map_tab == 1 ? " activeTab" : "", can_edit ? "true" : "false",
               g_map_tab == 2 ? " activeTab" : "", can_edit ? "true" : "false");
    if (can_edit) sb_appendf(sb, "<div class='mapTabP%s' onclick='updateMapTab(3, true)'>Data</div>", g_map_tab == 3 ? " activeTab" : "");
}

static void hub_render_creation(const int is_mod, const cJSON *t) {
    const char *pre = is_mod ? "mod" : "map";
    StrBuf sb = {0};
    sb_append(&sb, "<div class='scrollItem' style='position:relative;padding-bottom:10px;padding-top:16px;'>");
    char key[32], date[48], date2[48], num[48], ver[32];
    snprintf(key, sizeof(key), "%s_name", pre);
    const char *name = cJSON_IsObject(t) ? jstr(t, key) : NULL;
    if (!name) {
        sb_appendf(&sb, "<span style='color:rgba(%s,0.4);'>%s</span>", is_mod ? "0,0,0" : "255,255,255", is_mod ? STR_MODS_NONE : STR_MAPS_NONE);
        char *html = sb_finish(&sb);
        set_html(is_mod ? "modProfile" : "mapProfile", html);
        free(html);
        return;
    }
    const AccountInfo *acc = g_hub.logged_in ? account_get() : NULL;
    snprintf(key, sizeof(key), "%s_id", pre);
    const int id = (int) jnum(t, key);
    snprintf(key, sizeof(key), "%s_featured", pre);
    const int featured = (int) jnum(t, key);
    snprintf(key, sizeof(key), "%s_creator", pre);
    const char *creator = jstr(t, key) ? jstr(t, key) : "";
    snprintf(key, sizeof(key), "%s_image", pre);
    const char *image = jstr(t, key);
    snprintf(key, sizeof(key), "%s_updatecounter", pre);
    js_num(ver, sizeof(ver), (jnum(t, key) > 0 ? jnum(t, key) : 1) / 10);
    const int can_edit = !is_mod && acc && (acc->is_dev || !strcmp(acc->name, creator));
    const int dev = acc && acc->is_dev;
    sb_append(&sb, "<div class='creationActions'>");
    if (can_edit) {
        sb_append(&sb, "<span title='Edit' class='material-icons creationAction edit' onclick='openEdit(&quot;");
        sb_append_js_escape(&sb, name);
        js_num(num, sizeof(num), jnum(t, "fund"));
        sb_appendf(&sb, "&quot;,%d,%s)'>create</span>", id, num);
    }
    sb_appendf(&sb, "<span class='material-icons creationAction del canDeleteCreations%s' onclick='deleteCreationConf(%d,%d);'>delete_forever</span><span class='material-icons creationAction canFeatureCreations%s%s' onclick='featureCreation(this, %d,%d);'>check_circle</span></div>",
               dev ? "" : " hide", id, is_mod, featured ? " sel" : "", dev ? "" : " hide", id, is_mod);
    sb_append(&sb, "<img draggable='false' class='mapPageThumb' src='");
    sb_append_esc(&sb, image && *image ? image : "./img/noimg.png");
    sb_append(&sb, "' onerror='this.onerror=null;this.src=\"./img/noimg.png\";'><div style='display: inline-block;width: calc(100% - 260px);'>");
    if (is_mod) {
        sb_append(&sb, "<div style='font-size:22px;margin-bottom: 2px;color:#fff'>");
        sb_append_served(&sb, name);
        sb_append(&sb, "</div><div style='font-size:16px;color:#fff;'>by <a target='_blank' href='/social.html?p=profile&q=");
        sb_append_esc(&sb, creator);
        sb_append(&sb, "' style='color: rgba(255, 255, 255, 0.4);'>");
        sb_append_esc(&sb, creator);
        snprintf(key, sizeof(key), "%s_date", pre);
        get_date(date, sizeof(date), jstr(t, key));
        snprintf(key, sizeof(key), "%s_initialdate", pre);
        get_date(date2, sizeof(date2), jstr(t, key));
        sb_appendf(&sb, "</a></div><div style='position:absolute;right:28px;top:336px;text-align:right'><div style='font-size:17px;color:rgba(255,255,255,0.5);'>updated <b style='color:#fff;'>%s</b></div><div style='font-size:17px;color:rgba(255,255,255,0.5);'>created <b style='color:#fff;'>%s</b></div></div><div class='mapText' style='font-size:26px;top:268px;left:35px;'>v%s</div><div class='mapText' style='top:275px;right:33px;'>%d<span class='material-icons' style='margin-left:10px;color:rgba(255, 255, 255, 0.5);vertical-align:middle;margin-top:-6px;font-size:30px'>thumb_up</span></div></div><div style='height:12px'></div>",
                   date, date2, ver, (int) jnum(t, "vt"));
        // t.vl: a YouTube embed (no iframes natively): the box stays hidden
        sb_append(&sb, "<div id='modVid' style='display:none'></div><div id='modDescr'>");
        snprintf(key, sizeof(key), "%s_description", pre);
        const char *descr = jstr(t, key);
        if (descr && *descr) sb_append_served(&sb, descr);
        else sb_append(&sb, "No Description");
        sb_append(&sb, "</div><div class='button mod buttonP' onclick='shareModLink(this, &quot;");
        sb_append_js_escape(&sb, name);
        sb_append(&sb, "&quot;)'>Share</div><div class='button mod buttonH' style='margin-left:8px' onclick='playMod(&quot;");
        sb_append_js_escape(&sb, name);
        sb_append(&sb, "&quot;)'>Use</div><div class='button mod buttonR' style='margin-left:8px' onclick='repPop(`Mod`)'>Report</div>");
    } else {
        const double pl = jnum(t, "pl");
        sb_append(&sb, "<div style='font-size:22px;margin-bottom:2px;color:#fff;'>");
        sb_append_served(&sb, name);
        sb_append(&sb, "</div><div style='font-size:16px;color:#fff !important;'>by <a target='_blank' href='/social.html?p=profile&q=");
        sb_append_esc(&sb, creator);
        sb_append(&sb, "' style='color: rgba(255,255,255,0.4);'>");
        sb_append_esc(&sb, creator);
        comma_number(num, sizeof(num), pl);
        sb_appendf(&sb, "</a></div><div style='position:absolute;right:30px;top:350px;text-align:right;font-size:17px;'><b style='color:#fff;'>%s</b><span style='color:rgba(255,255,255,0.5);'> Plays</span><div></div><b id='totalOnline' style='color:#fff;'>0</b><span style='color: rgba(255, 255, 255, 0.5);'> Online</span></div><div class='mapPagePlB buttonS' style='width:150px;' onclick='playMap(&quot;", num);
        sb_append_js_escape(&sb, name);
        sb_append(&sb, "&quot;)'><span class='material-icons' style='color:#fff;font-size:40px;line-height:0px;vertical-align:middle;margin-top:-8px;'>play_arrow</span></div><div class='mapPagePlB buttonH' style='height:35px;left:245px;'onclick='shareGameTweet(`");
        sb_append_js_escape(&sb, name);
        comma_number(num, sizeof(num), jnum(t, "vt"));
        sb_appendf(&sb, "`)'><img src='./img/social/twitter.png' style='width:40px;margin-bottom:-30px;position:absolute;top:2px;left:11px;' /></div><div class='mapText' style='top:285px;right:20px;'>%s<span class='material-icons' style='margin-left:10px;color:rgba(255, 255, 255, 0.5);vertical-align:middle;margin-top:-6px;font-size:30px'>thumb_up</span></div></div><div id='mapTabsP'>", num);
        append_map_tabs(&sb, can_edit);
        sb_appendf(&sb, "</div><div id='mapVid' style='display:none'></div><div id='mapDescr' style='padding-top:20px%s'>", g_map_tab ? ";display:none" : "");
        snprintf(key, sizeof(key), "%s_description", pre);
        const char *descr = jstr(t, key);
        if (descr && *descr) sb_append_served(&sb, descr);
        else sb_append(&sb, "No Description");
        const int ca = (int) jnum(t, "ca");
        get_date(date, sizeof(date), jstr(t, "map_initialdate"));
        char cash[48], cash_c[64];
        snprintf(cash, sizeof(cash), "%.2f", round(pl / 1000 * 0.8 * 100) / 100); // formatNumCash(pl / 1000 * config.EPM)
        snprintf(cash_c, sizeof(cash_c), "%s", cash);
        comma_number(num, sizeof(num), pl);
        sb_appendf(&sb, "<div class='mapSubDesc'><div class='mapSubItm'><b style='color:#fff'>Category</b><br/>%s</div><div class='mapSubItm'><b style='color:#fff'>~Revenue</b><br/>$%s</div><div class='mapSubItm'><b style='color:#fff'>Plays</b><br/>%s</div><div class='mapSubItm'><b style='color:#fff'>Released</b><br/>%s</div><div class='mapSubItm'><b style='color:#fff'>Version</b><br/>v%s</div></div></div>",
                   k_map_cats[ca >= 0 && ca < MAP_CAT_COUNT ? ca : 0], cash_c, num, date, ver);
        sb_appendf(&sb, "<div id='activeGames' style='display:%s'></div><div id='mapLeaders' style='display:%s;'><div style='font-size:16px;margin-bottom: 2px;text-align:center;color:#fff'>Not Set Yet!</div></div>",
                   g_map_tab == 1 ? "block" : "none", g_map_tab == 2 ? "block" : "none");
        if (can_edit) sb_appendf(&sb, "<div id='mapData' style='display:%s;margin-bottom:20px'><div style='font-size:16px;margin-bottom: 2px'><input type='text' id='searchDInput' placeholder='Search User' onkeyup='if (window.event.keyCode==13) window.searchPData();'><div id='searchDBtn' onclick='window.searchPData();'>Search</div><div id='deleteDBtn' onclick='window.deleteDataPop();'>Delete By Key</div></div><div id='dataList'></div></div>", g_map_tab == 3 ? "block" : "none");
        sb_append(&sb, "<div class='button map buttonP' onclick='shareMapLink(this, &quot;");
        sb_append_js_escape(&sb, name);
        sb_append(&sb, "&quot;)'>Share</div><div class='button map buttonH' style='margin-left:8px' onclick='hostMap(&quot;");
        sb_append_js_escape(&sb, name);
        sb_append(&sb, "&quot;)'>Host</div><div class='button map buttonR' style='margin-left:8px' onclick='repPop(`Map`)'>Report</div>");
    }
    char *html = sb_finish(&sb);
    set_html(is_mod ? "modProfile" : "mapProfile", html);
    free(html);
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
            sb_appendf(&sb, "<span class='searchPClan' style='color:%s' onclick='openLink(&quot;/social.html?p=clan&q=", clan_col(clan, "rgba(0,0,0,0.4)"));
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

// f515(post, width): likes / comments footer
static void append_post_footer(StrBuf *sb, const cJSON *e, const int width) {
    const int pid = (int) jnum(e, "pid");
    const int liked = jnum(e, "dlk") == 1;
    sb_append(sb, "<div class='likesHolder' ");
    if (width) sb_appendf(sb, "style='width:%dpx'", width);
    sb_appendf(sb, "><span class='shareBtnHol' onclick='copyPost(this, %d)'>Share</span><span id='comVal%d' style='color:#fff;'>%d</span> <span onclick='openPost(%d)' class='material-icons likeBtn' style='font-size:33px;vertical-align:middle;margin-top:-4px;margin-right:20px;color:rgba(255,255,255,0.5)'>chat_bubble_outline</span><span id='likesVal%d' style='color:#fff'>%d</span> Likes <span class='material-icons likeBtn'  onclick='likePost(%d,this)'  style='font-size:33px;vertical-align:middle;margin-top:-5px;color:%s'>favorite%s</span></div>",
               pid, pid, (int) jnum(e, "cnt"), pid, pid, (int) jnum(e, "vot"), pid, liked ? "#eb4034" : "#fff", liked ? "" : "_border");
}

static void append_post_author(StrBuf *sb, const cJSON *e) {
    const char *pln = jstr(e, "pln");
    const char *cln = jstr(e, "cln");
    sb_append(sb, "<span class='postLink'><a href='/social.html?p=profile&q=");
    sb_append_esc(sb, pln ? pln : "");
    sb_append(sb, "' style='color:#fff'>");
    sb_append_esc(sb, pln ? pln : "");
    sb_append(sb, "</a>");
    if (cln && cln[0]) {
        sb_appendf(sb, " <span style='color:%s'>[", clan_col(cln, "rgba(255,255,255,0.2)"));
        sb_append_esc(sb, cln);
        sb_append(sb, "]</span>");
    }
    if (jnum(e, "prm") > 0) sb_append(sb, " <i class='material-icons' style='margin-bottom:5px;font-size:25px;color:#FBC02D;vertical-align:middle;margin-left:-2px'>beenhere</i>");
    if (jnum(e, "ftr")) sb_append(sb, " <i class='material-icons' style='margin-bottom:5px;font-size:25px;color:#40C4FF;vertical-align:middle;margin-left:-2px'>check_circle</i>");
}

// f516(post, single, fromProfile): one feed post (text posts, game / mod links, invites;
// skin unboxings need the v7 skin table KrunkNative's viewer data does not carry yet)
static void append_post(StrBuf *sb, const cJSON *e, const int single) {
    char ago[64] = "Just Now";
    if (jnum(e, "tim") > 0) {
        readable_time(ago, sizeof(ago) - 4, jnum(e, "tim"));
        strcat(ago, " ago");
    }
    char flw[32];
    comma_number(flw, sizeof(flw), jnum(e, "flw"));
    sb_appendf(sb, "<div></div><div %s><div class='postHead'><img class='postPFP' onerror='this.onerror=null;this.src=\"./img/player_pfp.png\";'src='./img/player_pfp.png' /><div class='postInfo'><div>", single ? "" : "class='socPost'");
    append_post_author(sb, e);
    sb_appendf(sb, "<span class='postTime'>%s</span></div><span class='postSubInfo'>%s Followers</span></div><div class='postContent'><div style='overflow-x:hidden;color:rgba(255,255,255,0.8);user-select: text;'>", ago, flw);
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
    if (!single) append_post_footer(sb, e, 0);
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
    int i = 0, ads = 0;
    cJSON_ArrayForEach(post, t) {
        append_post(&sb, post, 0);
        if (i++ % 3 == 0) sb_appendf(&sb, "<div id=\"innerDiv%d\" class=\"innerContent\"></div>", ++ads); // the in-feed ad slots (empty: no ads)
    }
    char *html = sb_finish(&sb);
    set_html("postHolder", html);
    free(html);
}

// ------------------------------------------------------------------ brain: market, item sales, clan (social.js io_0)


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

// UTILS.getPreview(skin, store): textures/<sprays/<id> | previews/...>.png on the asset host
static void skin_preview(char *out, const size_t n, const ViewerSkin *skin) {
    char path[256];
    viewer_skin_preview_path(skin, path, sizeof(path));
    html_dom_asset_url(path, out, n);
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

// io_0("clan", tag, {clan_name, clan_score, creatorname, clan_link4, members[{p, r, s, su, ve, h}]})
static void hub_render_clan(const cJSON *t) {
    static const char *const roles[4] = {"Recruit", "Soldier", "Captain", "Commander"}; // config.clanRoles
    static const int role_perms[4] = {0, 0, 1, 2};
    const char *name = jstr(t, "clan_name");
    const char *creator = jstr(t, "creatorname");
    const char *discord = jstr(t, "clan_link4");
    const cJSON *members = cJSON_GetObjectItemCaseSensitive(t, "members");
    const int count = cJSON_GetArraySize(members);
    char score[32];
    comma_number(score, sizeof(score), jnum(t, "clan_score"));
    StrBuf sb = {0};
    sb_appendf(&sb, "<div class='scrollItem'><div style='height:2px'></div><img class='clanLogoP'><div class='clanInfH'><span style='font-size:24px;color:%s'>[", clan_col(name, "rgba(255,255,255,0.2)"));
    sb_append_esc(&sb, name ? name : "");
    sb_append(&sb, "]</span><span style='color:rgba(255,255,255,0.3)'> by <a target='_blank' href='/social.html?p=profile&q=");
    sb_append_esc(&sb, creator ? creator : "");
    sb_append(&sb, "' class='lName'>");
    sb_append_esc(&sb, creator ? creator : "");
    sb_appendf(&sb, "</a></span><div style='float:right;color:rgba(255,255,255,0.5)'>%s</div><div style='margin-top:4px;color:rgba(255,255,255,0.5)'><span style='font-size:26px;color:rgba(255,255,255,0.5)'>100.00</span>  0.00 (0.00%%)</div></div>", score);
    sb_appendf(&sb, "<div class='clanStatH'><div style='color: rgba(255,255,255,.5);'>Members<span style='float:right;color: rgba(255,255,255,.5);'>%d</span></div><div style='color: rgba(255,255,255,.5);'>Market Cap<span style='float:right;color: rgba(255,255,255,.5);'>~</span></div><div style='color: rgba(255,255,255,.5);'>Purchase Power<span style='float:right;color: rgba(255,255,255,.5);'>~</span></div></div>", count);
    sb_append(&sb, "<div style='margin-top:15px;margin-bottom:8px'><div class='clanBut' style='background-color:#659ad6' onclick='");
    if (discord && *discord) {
        sb_append(&sb, "window.open(&quot;https://discord.gg/");
        sb_append_esc(&sb, discord);
        sb_append(&sb, "&quot;);");
    }
    sb_append(&sb, "'>Clan Discord</div><div class='clanBut' style='float:right;background-color:#c365d6' onclick='clanRS(0, `");
    sb_append_esc(&sb, name ? name : "");
    sb_append(&sb, "`)'>Request to Join</div></div></div>");
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
    sb_append(&sb, "<div class='scrollItem' style='max-height: calc(100% - 285px);'>");
    for (int re = 3; re >= 0; --re) {
        int ie = 0;
        StrBuf le = {0};
        for (int v = 0; v < n; v++) {
            const char *pl = jstr(sorted[v], "p");
            const int is_creator = creator && pl && !strcmp(creator, pl);
            if (!((is_creator && role_perms[re] == 2) || ((int) jnum(sorted[v], "r") == re && !is_creator))) continue;
            ie++;
            sb_append(&le, "<div style='padding-left:8px;padding-right:8px'>");
            if (jnum(sorted[v], "su")) sb_append(&le, "<i class='material-icons' style='color:#FBC02D;margin-top:-8px;font-size:28px;vertical-align:middle;'>beenhere</i>");
            if (jnum(sorted[v], "ve")) sb_append(&le, "<i class='material-icons' style='color:#40C4FF;margin-top:4px;margin-right: 4px;font-size:28px;vertical-align:baseline;'>check_circle</i>");
            sb_append(&le, "<a target='_blank' href='/social.html?p=profile&q=");
            sb_append_esc(&le, pl ? pl : "");
            sb_appendf(&le, "' class='lName' style='%s'>", jnum(sorted[v], "h") ? "color:#E85656" : "");
            sb_append_esc(&le, pl ? pl : "");
            sb_append(&le, "</a><div style='float:right'>");
            append_level_cell(&le, jnum(sorted[v], "s"));
            sb_append(&le, "</div></div>");
        }
        sb_appendf(&sb, "<div style='margin-top:%dpx;margin-bottom:5px;font-size:22px;background-color:rgba(255,255,255,0.1);padding:3px;padding-left:10px;padding-right:10px;border-radius:5px;color:rgba(255,255,255,0.5)'>%ss <span style='float:right;color:rgba(255,255,255,0.4)'>%d Members</span></div>",
                   role_perms[re] == 2 ? 10 : 18, roles[re], ie);
        char *list = sb_finish(&le);
        sb_append(&sb, list);
        free(list);
        if (!ie) sb_appendf(&sb, "<div style='color:rgba(255,255,255,0.5);padding-left:8px'>No %ss</div>", roles[re]);
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

// ------------------------------------------------------------------ item list (io_0 "items", f494-f496, itemFilter)

#define ITEMS_CHUNK 500
typedef struct {
    int seas, rarity, weapon, type; // -1 = no filter (vO81 keys); seas 0..: Season n
    int blackm;                     // categories ViewerSkin has no field for: always empty
    int sort;                       // vLN0570: 0 normal (rarity), 1 newest, 2 oldest
    int *list;                      // vA156: skin indices
    int count, shown;               // vLN0571 chunks rendered
} HubItems;
static HubItems g_items = {-1, -1, -1, -1, 0, 0, NULL, 0, 0};

static void items_card(StrBuf *sb, const int index) {
    const ViewerSkin *sk = hub_skin(index);
    if (!sk) return;
    const char *col = rarity_color(sk);
    char path[256], url[256];
    viewer_skin_preview_path(sk, path, sizeof(path));
    html_dom_asset_url(path, url, sizeof(url));
    sb_appendf(sb, "<div class='marketCard%s' style='color:%s;border:5px solid %s' onmouseenter='SOUND.play(`tick_0`,0.1)'>",
               g_hub.rarity_anim && rarity_animate(sk) ? " rainbow" : "", col, col);
    sb_append_esc(sb, sk->name);
    sb_appendf(sb, "<div class='itemOwn'>%s</div>", skin_type_label(sk));
    if (sk->type == 4) {
        sb_appendf(sb, "<div class='sprayMarketImg' style='background: url(&quot;%s&quot;);", url);
        if (sk->frames) sb_appendf(sb, "animation: itemAnm%d %.15gs steps(%d) infinite;", sk->frames, 1 + sk->frame_t / 1000.0, sk->frames);
        sb_append(sb, "'></div>");
    } else {
        const char *suffix = !sk->type ? "" : sk->type >= 16 ? "P" : sk->type >= 8 ? "S" : sk->type == 5 ? "D" : "C";
        sb_appendf(sb, "<img draggable='false' loading='lazy' class='marketImg%s%s' src='%s'/>", suffix, (sk->flags & VS_RGB) ? " rgbHue" : "", url);
    }
    if (skin_animated(sk)) sb_append(sb, "<div class='material-icons isAnim'>loop</div>");
    sb_appendf(sb, "<div class='cardActions'><a class='cardAction' onmouseenter='SOUND.play(`tick_0`,0.1)' onclick='viewItem(%d)'>Preview</a><div class='cardActionSep'></div><a class='cardAction' onmouseenter='SOUND.play(`tick_0`,0.1)' onclick='openLink(&quot;/social.html?p=market&i=%d&quot;, &quot;_blank&quot;)'>Listings</a><div class='cardActionSep'></div><a class='cardAction' onmouseenter='SOUND.play(`tick_0`,0.1)' onclick='openSales(%d)'>Info</a></div></div>",
               index, index, index);
}

// f496(): the next chunk of 500
static void items_more(void) {
    const int from = g_items.shown * ITEMS_CHUNK;
    if (from >= g_items.count) return;
    StrBuf sb = {0};
    for (int i = from; i < g_items.count && i < from + ITEMS_CHUNK; i++) items_card(&sb, g_items.list[i]);
    char *html = sb_finish(&sb);
    const HtmlElem list = el("itemsList");
    if (list) html_dom_insert_html(list, HTML_INSERT_BEFORE_END, html);
    free(html);
    g_items.shown++;
}

static int items_rarity_cmp(const void *pa, const void *pb) { // rarity desc, then the table order
    const int a = *(const int *) pa, b = *(const int *) pb;
    const ViewerSkin *sa = hub_skin(a), *sbk = hub_skin(b);
    const int ra = sa ? sa->rarity : 0, rb = sbk ? sbk->rarity : 0;
    if (ra != rb) return rb - ra;
    return a - b;
}

static int items_index_cmp(const void *pa, const void *pb) { return *(const int *) pa - *(const int *) pb; }

// f494(): the keyframes, the count, sort, first chunk
static void items_show(void) {
    StrBuf sb = {0};
    sb_append(&sb, "<style>");
    for (int i = 0; i < 11; i++) sb_appendf(&sb, "@keyframes itemAnm%d {100%%{background-position:-%dpx}}", i, i * 200);
    sb_append(&sb, "</style>");
    char *html = sb_finish(&sb);
    set_html("itemsList", html);
    free(html);
    char cnt[32];
    snprintf(cnt, sizeof(cnt), "%d Items", g_items.count);
    const HtmlElem c = el("itemCount");
    if (c) html_dom_set_text(c, cnt);
    g_items.shown = 0;
    if (g_items.sort) {
        qsort(g_items.list, (size_t) g_items.count, sizeof(int), items_index_cmp);
        if (g_items.sort != 2) {
            for (int i = 0, j = g_items.count - 1; i < j; i++, j--) {
                const int t = g_items.list[i];
                g_items.list[i] = g_items.list[j];
                g_items.list[j] = t;
            }
        }
    } else {
        qsort(g_items.list, (size_t) g_items.count, sizeof(int), items_rarity_cmp);
    }
    items_more();
}

static void items_alloc(void) {
    if (!g_items.list) g_items.list = calloc((size_t) (g_viewer_skin_count ? g_viewer_skin_count : 1), sizeof(int));
    g_items.count = 0;
}

// f495(): store.skins filtered by vO81
static void items_filter_apply(void) {
    items_alloc();
    for (int i = 0; g_items.list && i < g_viewer_skin_count; i++) {
        const ViewerSkin *sk = &g_viewer_skins[i];
        if (sk->flags & VS_BLOCKED) continue;
        if (g_items.blackm) continue;
        if (g_items.seas >= 0 && (sk->seas ? sk->seas : 1) != g_items.seas) continue;
        if (g_items.rarity >= 0 && sk->rarity != g_items.rarity) continue;
        if (g_items.weapon >= 0 && sk->weapon != g_items.weapon) continue;
        if (g_items.type >= 0 && sk->type != g_items.type) continue;
        g_items.list[g_items.count++] = i;
    }
    items_show();
}

// itemFilter(key, value): key seas / rarity / weapon / type / sort / category / the
// category groups (blackM, drop, ranked, challenge, spinable)
static void items_filter(const char *key, const char *value) {
    const int all = !value || !strcmp(value, "all");
    if (!strcmp(key, "sort")) {
        g_items.sort = all ? 0 : !strcmp(value, "new") ? 1 : !strcmp(value, "old") ? 2 : 0;
        items_filter_apply();
        return;
    }
    const int v = all ? -1 : atoi(value);
    if (!strcmp(key, "weapon")) { g_items.type = -1; g_items.weapon = v; }
    else if (!strcmp(key, "type")) { g_items.weapon = -1; g_items.type = v; }
    else if (!strcmp(key, "rarity")) g_items.rarity = v;
    else if (!strcmp(key, "seas")) { g_items.blackm = 0; g_items.seas = v; }
    else if (!strcmp(key, "category")) { g_items.seas = -1; g_items.blackm = 0; }
    else { g_items.seas = -1; g_items.blackm = !all; } // blackM / drop / ranked / challenge / spinable: no such fields in the table
    const HtmlElem in = el("itemFilterInput");
    if (in) html_dom_set_value(in, "");
    items_filter_apply();
}

// itemFilterSearch(text): every skin whose name / weapon matches (case-insensitive)
static void items_search(const char *text) {
    g_items.seas = g_items.rarity = g_items.weapon = g_items.type = -1;
    g_items.blackm = 0;
    g_items.sort = 0;
    static const char *const sels[] = {"itemCategory", "itemWeapon", "itemRarity", "itemCosm", "itemSortOther"};
    for (size_t i = 0; i < sizeof(sels) / sizeof(sels[0]); i++) {
        const HtmlElem e = el(sels[i]);
        if (e) {
            HtmlElem first = html_dom_first_child(e);
            char t[64] = "";
            if (first) html_dom_get_text(first, t, sizeof(t));
            html_dom_set_value(e, t);
        }
    }
    items_alloc();
    for (int i = 0; g_items.list && i < g_viewer_skin_count; i++) {
        const ViewerSkin *sk = &g_viewer_skins[i];
        int hit = !text || !*text;
        if (!hit && sk->name && contains_ci_str(sk->name, text)) hit = 1;
        if (!hit && sk->weapon >= 1 && sk->weapon <= WEAPON_COUNT && g_weapons[sk->weapon - 1] && contains_ci_str(g_weapons[sk->weapon - 1]->name, text)) hit = 1;
        if (!hit && sk->creator && contains_ci_str(sk->creator, text)) hit = 1;
        if (sk->flags & VS_BLOCKED) hit = 0;
        if (hit) g_items.list[g_items.count++] = i;
    }
    items_show();
}

// itemFilterChange(select): the chosen <option>'s onclick (the native engine does not click
// options): the option's text is looked up in what items_page wrote.
static void items_filter_change(const HtmlElem sel) {
    char id[32] = "", text[96] = "";
    if (!sel) return;
    html_dom_get_attr(sel, "id", id, sizeof(id));
    html_dom_get_value(sel, text, sizeof(text));
    if (!strcmp(id, "itemCategory")) {
        if (!strcmp(text, "All")) items_filter("category", "all");
        else if (!strncmp(text, "Season ", 7)) items_filter("seas", text + 7);
        else items_filter("blackM", "true");
    } else if (!strcmp(id, "itemRarity")) {
        if (!strcmp(text, "All")) items_filter("rarity", "all");
        for (int i = 0; i < g_viewer_rarity_count; i++) {
            if (!strcmp(g_viewer_rarities[i].name, text)) {
                char v[8];
                snprintf(v, sizeof(v), "%d", i);
                items_filter("rarity", v);
            }
        }
    } else if (!strcmp(id, "itemWeapon")) {
        if (!strcmp(text, "All")) items_filter("weapon", "all");
        for (int i = 0; i < WEAPON_COUNT; i++) {
            if (g_weapons[i] && g_weapons[i]->name && !strcmp(g_weapons[i]->name, text)) {
                char v[8];
                snprintf(v, sizeof(v), "%d", i + 1);
                items_filter("weapon", v);
                break;
            }
        }
    } else if (!strcmp(id, "itemCosm")) {
        if (!strcmp(text, "All")) items_filter("type", "all");
        for (int i = 1; i < g_viewer_type_count; i++) {
            char name[32];
            snprintf(name, sizeof(name), "%s", g_viewer_types[i]);
            char *slash = strchr(name, '/');
            if (slash) *slash = 0;
            name[0] = (char) toupper((unsigned char) name[0]);
            if (!strcmp(name, text)) {
                char v[8];
                snprintf(v, sizeof(v), "%d", i);
                items_filter("type", v);
            }
        }
    } else if (!strcmp(id, "itemSortOther")) {
        items_filter("sort", !strcmp(text, "Newest") ? "new" : !strcmp(text, "Oldest") ? "old" : "all");
    }
}

static void items_page(const char *a, const char *k) {
    StrBuf sb = {0};
    sb_append(&sb, "<select id='itemCategory' onchange='itemFilterChange(this)'><option disabled selected>Category</option><option onclick='itemFilter(`category`,`all`)'>All</option>");
    for (int i = 1; i <= 7; i++) sb_appendf(&sb, "<option onclick='itemFilter(`seas`,%d)'>Season %d</option>", i, i); // config.currentSeason
    sb_append(&sb, "<option onclick='itemFilter(`blackM`,true)'>Black Market</option><option onclick='itemFilter(`drop`,true)'>Drops</option><option onclick='itemFilter(`ranked`,true)'>Ranked</option><option onclick='itemFilter(`challenge`,true)'>Challenge</option><option onclick='itemFilter(`spinable`,true)'>Spinable</option></select>");
    sb_append(&sb, "<select id='itemRarity' onchange='itemFilterChange(this)'><option disabled selected>Rarity</option><option onclick='itemFilter(`rarity`,`all`)'>All</option>");
    for (int i = 0; i < g_viewer_rarity_count; i++) sb_appendf(&sb, "<option onclick='itemFilter(`rarity`,%d)'>%s</option>", i, g_viewer_rarities[i].name);
    sb_append(&sb, "</select><select id='itemWeapon' onchange='itemFilterChange(this)'><option disabled selected>Weapon</option><option onclick='itemFilter(`weapon`,`all`)'>All</option>");
    for (int i = 0; i < WEAPON_COUNT; i++) {
        if (!g_weapons[i] || g_weapons[i]->melee || g_weapons[i]->equipment) continue;
        sb_appendf(&sb, "<option onclick='itemFilter(`weapon`,%d)'>%s</option>", i + 1, g_weapons[i]->name);
    }
    sb_append(&sb, "</select><select id='itemCosm' onchange='itemFilterChange(this)'><option disabled selected>Cosmetics</option><option onclick='itemFilter(`type`,`all`)'>All</option>");
    for (int i = 1; i < g_viewer_type_count; i++) {
        char name[32];
        snprintf(name, sizeof(name), "%s", g_viewer_types[i]);
        char *slash = strchr(name, '/');
        if (slash) *slash = 0;
        name[0] = (char) toupper((unsigned char) name[0]);
        sb_appendf(&sb, "<option onclick='itemFilter(`type`,%d)'>%s</option>", i, name);
    }
    sb_append(&sb, "</select><select id='itemSortOther' onchange='itemFilterChange(this)'><option disabled selected>Sort</option><option onclick='itemFilter(`sort`,`all`)'>Normal</option><option onclick='itemFilter(`sort`,`new`)'>Newest</option><option onclick='itemFilter(`sort`,`old`)'>Oldest</option></select>");
    sb_append(&sb, "<input type='text' id='itemFilterInput' placeholder='Search Item' onkeyup='itemFilterSearch(this.value)'><div id='itemCount'></div>");
    char *html = sb_finish(&sb);
    set_html("itemsListNav", html);
    free(html);
    set_display("itemsHolder", "block");
    set_html("itemsList", "");
    g_items.seas = g_items.rarity = g_items.weapon = g_items.type = -1;
    g_items.blackm = 0;
    g_items.sort = 0;
    items_filter("seas", "all");
    // ?p=items&q=<key>&s=<value>: a preset filter
    if (a && *a && strcmp(a, "items") && k && *k) items_filter(a, k);
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

// filterPopup / sortPopup (v7: genericPopup(..., "filterPop" | "sortPop"); the panel's state
// lives on in g_filter_state between popups, as v7 keeps the popup's innerHTML)
static int filter_weapon_shown(const int a) {
    return g_weapons[a] && !g_weapons[a]->melee && !g_weapons[a]->equipment;
}

static void filter_state_save(const int sort) {
    HubFilterState *st = &g_filter_state[sort];
    const char *pre = sort ? "Sort" : "Filt";
    char id[32];
    for (int r = 0; r < g_viewer_rarity_count && r < 40; r++) {
        snprintf(id, sizeof(id), "rar%s%d", pre, r);
        if (el(id)) st->checked[0][r] = html_dom_get_checked(el(id));
    }
    for (int r = 0; r < WEAPON_COUNT && r < 40; r++) {
        snprintf(id, sizeof(id), "wep%s%d", pre, r);
        if (el(id)) st->checked[1][r] = html_dom_get_checked(el(id));
    }
    for (int r = 1; r < g_viewer_type_count && r < 40; r++) {
        snprintf(id, sizeof(id), "type%s%d", pre, r);
        if (el(id)) st->checked[2][r] = html_dom_get_checked(el(id));
    }
    const HtmlElem in = el(sort ? "invSortTxt" : "marketFiltTxt");
    if (in) html_dom_get_value(in, st->text, sizeof(st->text));
    snprintf(id, sizeof(id), "sea%s", pre);
    if (el(id)) html_dom_get_value(el(id), st->season, sizeof(st->season));
    if (!sort && el("sortFilt")) html_dom_get_value(el("sortFilt"), st->order, sizeof(st->order));
    if (sort && el("onlyMissing")) st->missing = html_dom_get_checked(el("onlyMissing"));
}

static void market_filter_popup(const int sort) {
    HubFilterState *st = &g_filter_state[sort];
    if (!st->built) {
        st->built = 1;
        for (int g = 0; g < 3; g++) for (int i = 0; i < 40; i++) st->checked[g][i] = 1;
        snprintf(st->season, sizeof(st->season), "0");
        snprintf(st->order, sizeof(st->order), "1");
    }
    const char *pre = sort ? "Sort" : "Filt";
    const char *nav = sort ? "sort" : "filter";
    StrBuf sb = {0};
    if (!sort) {
        sb_appendf(&sb, "<div class='filterPopHeader'>Market Search</div><div class='filterName'>Sort</div><div id='orderBySel'><select id='sortFilt' onchange='retainSelected(this)'><option value='0'%s>Highest Price</option><option value='1'%s>Lowest Price</option></select></div>",
                   !strcmp(st->order, "0") ? " selected" : "", strcmp(st->order, "0") ? " selected" : "");
    } else {
        sb_append(&sb, "<div class='filterPopHeader'>Sort Inventory</div>");
    }
    sb_appendf(&sb, "<div class='filterName'>Season</div><div id='orderBySel'><select id='sea%s' onchange='retainSelected(this)'><option value='0'>All Seasons</option>", pre);
    for (int s2 = 1; s2 <= 7; s2++) sb_appendf(&sb, "<option%s>%d</option>", atoi(st->season) == s2 ? " selected" : "", s2); // config.currentSeason
    sb_append(&sb, "</select></div>");
    if (sort) sb_appendf(&sb, "<div class='filterName' style='position: relative;display: block;width: 100%%;'>Only Show Missing<label class='filterSwitch'><input type='checkbox' onchange='retainChecked(this)' id='onlyMissing'%s><span class='filterSlider'></span></label></div>", st->missing ? " checked" : "");
    sb_appendf(&sb, "<input type='search' id='%s' onkeydown='if(window.event.keyCode==13) run%s()' placeholder='Keyword: Sniper, Plasma, Wings' value='", sort ? "invSortTxt" : "marketFiltTxt", sort ? "Sort" : "Filter");
    sb_append_esc(&sb, st->text);
    sb_appendf(&sb, "' /><div class='filterNav' id='%sNav_0' onmouseover='SOUND.play(`tick_0`,0.1)' onclick='switchSortFilter(%d, 0)' style='color:#faa332'>Rarities</div><div class='filterNav' id='%sNav_1' onmouseover='SOUND.play(`tick_0`,0.1)' onclick='switchSortFilter(%d, 1)'>Weapons</div><div class='filterNav' id='%sNav_2' onmouseover='SOUND.play(`tick_0`,0.1)' onclick='switchSortFilter(%d, 2)'>Cosmetics</div><div id='%sTog' onmouseover='SOUND.play(`tick_0`,0.1)' onclick='toggleSortFilters(%d)'>Toggle</div>",
               nav, sort, nav, sort, nav, sort, nav, sort);
    sb_appendf(&sb, "<div class='filterDiv' id='%sDiv_0'>", nav);
    for (int a = g_viewer_rarity_count - 1; a >= 0; --a) {
        sb_append(&sb, "<div class='raritySel'>");
        if (g_hub.rarity_anim && a == 6) sb_append(&sb, "<div class='colCub rainbowBG'></div>");
        else sb_appendf(&sb, "<div class='colCub' style='background-color:%s'></div>", g_viewer_rarities[a].color);
        sb_appendf(&sb, "%s<label class='filterSwitch'><input type='checkbox' onchange='retainChecked(this)' id='rar%s%d'%s><span class='filterSlider'></span></label></div>",
                   g_viewer_rarities[a].name, pre, a, st->checked[0][a] ? " checked" : "");
    }
    sb_appendf(&sb, "</div><div class='filterDiv' id='%sDiv_1' style='display:none;'>", nav);
    for (int a = 0; a < WEAPON_COUNT && a < 40; ++a) {
        if (!filter_weapon_shown(a)) continue;
        sb_appendf(&sb, "<div class='wepSel' style='color:#fff'>%s<label class='filterSwitch'><input type='checkbox' onchange='retainChecked(this)' id='wep%s%d'%s><span class='filterSlider'></span></label></div>",
                   g_weapons[a]->name, pre, a, st->checked[1][a] ? " checked" : "");
    }
    sb_appendf(&sb, "</div><div class='filterDiv' id='%sDiv_2' style='display:none;'>", nav);
    for (int a = 1; a < g_viewer_type_count && a < 40; ++a) {
        char name[32];
        snprintf(name, sizeof(name), "%s", g_viewer_types[a]);
        char *slash = strchr(name, '/');
        if (slash) *slash = 0;
        name[0] = (char) toupper((unsigned char) name[0]);
        sb_appendf(&sb, "<div class='otherSel' style='color:#fff'>%s<label class='filterSwitch'><input type='checkbox' onchange='retainChecked(this)' id='type%s%d'%s><span class='filterSlider'></span></label></div>",
                   name, pre, a, st->checked[2][a] ? " checked" : "");
    }
    sb_appendf(&sb, "</div><div id='filterRBtn' onmouseover='SOUND.play(`tick_0`,0.1)' onclick='run%s()'>%s</div><div id='resetFBtn' onmouseover='SOUND.play(`tick_0`,0.1)' onclick='resetSortFilter(%d)'>Reset</div>",
               sort ? "Sort" : "Filter", sort ? "Sort" : "Search", sort);
    char *html = sb_finish(&sb);
    generic_popup(html, sort ? "sortPop" : "filterPop", NULL);
    free(html);
}

static int g_filter_n, g_sort_n; // the latest request (stale replies are dropped)

// runFilter / runSort: io.send("fil"|"srt", n, text, rarities{}, weapons{}, types{}, season, order|missing)
static void market_run_filter(const int sort) {
    filter_state_save(sort);
    const HubFilterState *st = &g_filter_state[sort];
    cJSON *rar = cJSON_CreateObject(), *wep = cJSON_CreateObject(), *typ = cJSON_CreateObject();
    char key[8];
    for (int r = 0; r < g_viewer_rarity_count && r < 40; ++r) {
        snprintf(key, sizeof(key), "%d", r);
        if (st->checked[0][r]) cJSON_AddNumberToObject(rar, key, 1);
    }
    for (int r = 0; r < WEAPON_COUNT && r < 40; ++r) {
        snprintf(key, sizeof(key), "%d", r + 1);
        if (filter_weapon_shown(r) && st->checked[1][r]) cJSON_AddNumberToObject(wep, key, 1);
    }
    for (int r = 1; r < g_viewer_type_count && r < 40; ++r) {
        snprintf(key, sizeof(key), "%d", r);
        if (st->checked[2][r]) cJSON_AddNumberToObject(typ, key, 1);
    }
    cJSON *f = cJSON_CreateArray();
    cJSON_AddItemToArray(f, cJSON_CreateString(sort ? "srt" : "fil"));
    cJSON_AddItemToArray(f, cJSON_CreateNumber(sort ? ++g_sort_n : ++g_filter_n));
    cJSON_AddItemToArray(f, cJSON_CreateString(st->text));
    cJSON_AddItemToArray(f, rar);
    cJSON_AddItemToArray(f, wep);
    cJSON_AddItemToArray(f, typ);
    cJSON_AddItemToArray(f, cJSON_CreateString(st->season));
    if (sort) cJSON_AddItemToArray(f, cJSON_CreateBool(st->missing));
    else cJSON_AddItemToArray(f, cJSON_CreateString(st->order));
    io_wait("Searching");
    hub_send(f);
}

// resetSortFilter(sort): everything checked again, the search runs
static void market_reset_filter(const int sort) {
    HubFilterState *st = &g_filter_state[sort];
    for (int g = 0; g < 3; g++) for (int i = 0; i < 40; i++) st->checked[g][i] = 1;
    st->text[0] = 0;
    snprintf(st->season, sizeof(st->season), "0");
    snprintf(st->order, sizeof(st->order), "1");
    st->missing = 0;
    market_filter_popup(sort);
    filter_state_save(sort);
    market_run_filter(sort);
}

// switchSortFilter(sort, tab) / toggleSortFilters(sort)
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

// The v7 item list (io_0 "items": the whole store table, client side)
static void items_page(const char *a, const char *k);

// io_0(e, a, t, ...): one page's data arrived
static void io_0(const char *e, const char *a, const cJSON *t, const cJSON *extra) {
    char key[HUB_KEY_MAX * 2];
    snprintf(key, sizeof(key), "%s_%s", e ? e : "null", a && *a ? a : "null");
    if (g_hub.has_s && strcmp(g_hub.S, key) != 0) return;
    if (!e) return;
    const int has_t = t && !cJSON_IsNull(t);
    hide_all(0);
    set_display("loadMessage", "none");
    const HtmlElem title = el("navTitle");
    if (title) html_dom_set_class(title, "glitch", a && !strcmp(a, "blackm"));

    if (!strcmp(e, "profile")) {
        if (has_t && cJSON_IsObject(t)) hub_render_profile(t, extra);
        else io_po(0, "Profile doesn't exist");
        set_display("pTab_reports", "none");
    } else if (!strcmp(e, "search")) {
        set_display("searchHolder", "block");
        if (has_t && cJSON_IsArray(t)) hub_render_search(t);
    } else if (!strcmp(e, "mods")) {
        set_display("modsHolder", "block");
        list_update(1);
    } else if (!strcmp(e, "mod") || !strcmp(e, "map")) {
        const int is_mod = !strcmp(e, "mod");
        side_boxes();
        set_display(is_mod ? "modHolder" : "mapHolder", "block");
        hub_render_creation(is_mod, has_t ? t : NULL);
    } else if (!strcmp(e, "maps")) {
        set_display("mapsHolder", "block");
        list_update(0);
    } else if (!strcmp(e, "leaders")) {
        side_boxes();
        set_display("leaderHolder", "block");
        hub_render_leaders(a, has_t ? t : NULL); // no leaderboard rows offline: an empty list
    } else if (!strcmp(e, "market")) {
        set_display("marketHolder", "block");
        set_display("invSortB", a && !strcmp(a, "inventory") ? "table-cell" : "none");
        set_display("marketSearchB", a && !strcmp(a, "market") ? "table-cell" : "none");
        set_display("invStats", a && (!strcmp(a, "inventory") || !strcmp(a, "sales")) ? "table-cell" : "none");
        // no listings / inventory for a guest: social.market.login (K is null)
        if (g_hub.brain) hub_render_market(a, has_t ? t : NULL);
        else set_html("marketList", "<div style='font-size:20px;margin-top:20px;color:#fff'>" STR_MARKET_LOGIN "...</div>");
    } else if (!strcmp(e, "items")) {
        items_page(a, g_hub.k);
    } else if (!strcmp(e, "itemsales") && g_hub.brain) {
        hub_render_itemsales(has_t ? t : NULL, cJSON_GetArrayItem(extra, 4));
    } else if (!strcmp(e, "itemsales")) {
        side_boxes();
        set_display("itemsalesList", "none");
        if (!has_t) set_html("itemsalesHolder", "<div style='font-size:20px;margin-top:20px;color:#fff'>" STR_MARKET_NONE "...</div>");
        set_display("itemsalesHolder", "block");
    } else if (!strcmp(e, "listing")) {
        side_boxes();
        // a listing row needs the v7 skin table and the brain market: the none text
        set_html("listingHolder", "<div style='font-size:20px;margin-top:20px;color:#fff'>" STR_MARKET_NONE "...</div>");
        set_display("listingHolder", "block");
    } else if (!strcmp(e, "clan")) {
        side_boxes();
        set_display("clanHolder", "block");
        if (g_hub.brain && has_t && cJSON_IsObject(t)) hub_render_clan(t);
    } else if (!strcmp(e, "feed")) {
        set_display("feedHolder", "block");
        set_display("postEntryC", g_hub.logged_in ? "inline-block" : "none"); // acc ? inline-block : none
        set_display("fpHolder", "block");
        set_display("singlePost", "none");
        side_boxes();
        g_hub.feed_switching = 0;
        hub_render_feed(has_t ? t : NULL);
    } else if (!strcmp(e, "post")) {
        set_display("feedHolder", "block");
        set_display("postEntryC", g_hub.logged_in ? "inline-block" : "none");
        side_boxes();
        g_hub.feed_switching = 0;
        if (has_t) {
            set_html("postTopStat", "");
            set_display("singlePost", "inline-block");
            set_display("fpHolder", "none");
            set_html("singlePostH", "<div style='margin-top:25px;margin-left:310px;margin-bottom:10px;opacity:0.4'><div class='lds-ring darkring'><div></div><div></div><div></div><div></div></div></div>");
        }
        set_html("postHolder", "");
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

// playTick(v) / playSelect(v): hover_0 at v || 0.2, select_0 at v || 0.15 (v7 social.js)
H(h_play_tick) { (void) user; (void) ev; hub_sound("hover_0", (float) (argc > 0 && args[0].num ? args[0].num : 0.2)); return 1; }
H(h_play_select) { (void) user; (void) ev; hub_sound("select_0", (float) (argc > 0 && args[0].num ? args[0].num : 0.15)); return 1; }

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

// loginPopup(): genericPopup(..., "loginPop") when no account
H(h_login_popup) {
    UNUSED;
    if (g_hub.logged_in) return 1;
    char saved[128] = "";
    if (html_storage_get("krunker_username", saved, sizeof(saved)) < 0) saved[0] = 0;
    StrBuf sb = {0};
    sb_append(&sb, "<div style='position:relative;z-index:9'><div id='referralHeader'>Login</div><div style='height:20px;'></div><input id='accName' type='text' placeholder='Enter Username' class='accountInput' style='margin-top:0' value='");
    sb_append_esc(&sb, saved);
    sb_append(&sb, "'></input><input id='accPass' type='password' placeholder='Enter Password' class='accountInput'></input><div class='setBodH' style='margin-left:0px;width:calc(100% - 40px)'><div id='accResp' style='margin-top:20px;margin-bottom:20px;font-size:18px;color:rgba(255,255,255,0.5);text-align:center'>For lost Passwords/Accounts contact <span style='color:rgba(255,255,255,0.8)'>recovery@yendis.ch</span></div></div><div style='width:100%;text-align:center;margin-top:10px;background-color:rgba(0,0,0,0.3);padding-top:10px;padding-bottom:20px;'><div class='accBtn button buttonG' style='width: calc(100% - 30px);'onclick='loginAcc()'>Login</div></div>");
    char *html = sb_finish(&sb);
    generic_popup(html, "loginPop", NULL);
    free(html);
    return 1;
}

H(h_login_acc) {
    UNUSED;
    char name[128] = "", pass[128] = "";
    const HtmlElem n = el("accName"), p = el("accPass");
    if (n) html_dom_get_value(n, name, sizeof(name));
    if (p) html_dom_get_value(p, pass, sizeof(pass));
    io_wait("Please wait...");
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

H(h_clear_pops) { UNUSED; clear_pops(); return 1; }

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
    StrBuf sb = {0};
    sb_appendf(&sb, "<div style='color:#fff;font-size:20px;position:relative;margin-bottom:15px;'>Are you sure you want to list <span style='color:%s'>", rarity_color(pa));
    sb_append_esc(&sb, pa->name);
    sb_appendf(&sb, "</span> for %dKR ?</div><div id='confirmBtn' onclick='listConfirmed(%d, %d,%d, %d)' onmouseover='SOUND.play(&quot;tick_0&quot;,0.1)'>YES</div><div id='declineBtn' onclick='refreshPopup()' onmouseover='SOUND.play(&quot;tick_0&quot;,0.1)'>NO</div>",
               atoi(price), skin, (int) arg_n(args, argc, 1, 0), (int) arg_n(args, argc, 2, 0), atoi(price));
    char *html = sb_finish(&sb);
    generic_popup(html, "confListPop", NULL); // confListPopup
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
        io_po(0, "Must be logged in"); // account.login.required
        return 1;
    }
    const int skin = (int) arg_n(args, argc, 0, 0), uid = (int) arg_n(args, argc, 2, 0);
    const ViewerSkin *pa = hub_skin(skin);
    if (!pa) return 1;
    StrBuf sb = {0};
    sb_appendf(&sb, "<div style='color:#fff;font-size:19px;position:relative;margin-bottom:15px;'>Are you sure you want to Quick Sell <span style='color:%s'>", rarity_color(pa));
    sb_append_esc(&sb, pa->name);
    sb_appendf(&sb, "</span>?</div><div class='quickSellText'>KR<div class='quickSellKR'><span style='color: #FBC02D'>%d</span> KR (each)</div></div><div class='quickSellText'>Amount<select id='quickSellSelect'><option value='0'>1 Item</option><option value='1'>All But 1</option><option value='2'>All</option></select></div><div id='confirmBtn' onclick='sellConfirmed(%d,%d)' onmouseover='SOUND.play(&quot;tick_0&quot;,0.1)'>YES</div><div id='declineBtn' onclick='showPopup()' onmouseover='SOUND.play(&quot;tick_0&quot;,0.1)'>NO</div>",
               pa->rarity >= 0 && pa->rarity < 7 ? k_rarity_sell[pa->rarity] : 1, skin, uid);
    char *html = sb_finish(&sb);
    generic_popup(html, "confQSPop", NULL); // confQSPopup
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
// resetSortFilter(sort) / switchSortFilter(sort, tab) / toggleSortFilters(sort)
H(h_reset_filter) { UNUSED; market_reset_filter((int) arg_n(args, argc, 0, 0) != 0); return 1; }
H(h_switch_filter) { UNUSED; market_switch_filter((int) arg_n(args, argc, 0, 0) != 0, (int) arg_n(args, argc, 1, 0)); return 1; }
H(h_toggle_filters) { UNUSED; market_toggle_filters((int) arg_n(args, argc, 0, 0) != 0); return 1; }

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
    clear_pops();
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
    if (!strcmp(tab, "listings") && g_hub.profile_name[0]) {
        // io.send("uml", id): the user's market listings (kn_brain does not answer "uml": the
        // waiting popup closes on the backdrop, the tab stays empty)
        io_wait("Please wait...");
        const double v[1] = {g_hub.profile_id};
        send_ints("uml", 1, v);
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

// changeDateRange(range) (item page) / changeDateRange2(range) (profile sales):
// config.dataDateRanges = [7, 14, 21, 30, 60, 90, 120] days
H(h_change_date_range) {
    UNUSED;
    static const int days[7] = {7, 14, 21, 30, 60, 90, 120};
    char buf[16];
    const char *v = arg_s(ev, args, argc, 0);
    if (!v && argc > 0) {
        js_num(buf, sizeof(buf), arg_n(args, argc, 0, 3));
        v = buf;
    }
    const int range = v ? atoi(v) : 3;
    if (range < 0 || range > 6 || !g_hub.brain) return 1;
    if (!strcmp(html_ui_current_handler(), "changeDateRange2") && g_hub.profile_name[0]) {
        cJSON *f = cJSON_CreateArray();
        cJSON_AddItemToArray(f, cJSON_CreateString("pst"));
        cJSON_AddItemToArray(f, cJSON_CreateString(g_hub.profile_name));
        cJSON_AddItemToArray(f, cJSON_CreateNumber(days[range]));
        hub_send(f);
    }
    return 1;
}

// openCreationTab(tab): skins / games / mods / assets of the profile's Creations tab
H(h_open_creation_tab) {
    UNUSED;
    const char *tab = arg_s(ev, args, argc, 0);
    if (!tab) return 1;
    HtmlElem found[8];
    int n = html_dom_query_all(0, "div[id^=creationTab_]", found, 8);
    for (int i = 0; i < n; i++) {
        char id[48] = "";
        html_dom_get_attr(found[i], "id", id, sizeof(id));
        html_dom_set_class(found[i], "creationTabA", !strcmp(id + 12, tab));
    }
    n = html_dom_query_all(0, "div[id^=creationContent_]", found, 8);
    for (int i = 0; i < n; i++) {
        char id[48] = "";
        html_dom_get_attr(found[i], "id", id, sizeof(id));
        html_dom_set_class(found[i], "selected", !strcmp(id + 16, tab));
    }
    return 1;
}

// switchStat(tab): the profile's four stat tabs
H(h_switch_stat) {
    UNUSED;
    const int tab = (int) arg_n(args, argc, 0, 0);
    for (int i = 0; i < 4; ++i) {
        char id[16], sel[24];
        snprintf(id, sizeof(id), "statTab%d", i);
        if (!el(id)) continue;
        html_dom_set_class(el(id), "bigMenTabA", i == tab);
        snprintf(sel, sizeof(sel), ".statTabB%d", i);
        HtmlElem found[4];
        const int n = html_dom_query_all(0, sel, found, 4);
        for (int k = 0; k < n; k++) html_dom_set_class(found[k], "hide", i != tab);
    }
    return 1;
}

// switchNames(): the premium alias <-> account name
H(h_switch_names) {
    UNUSED;
    HtmlElem found[4];
    const int n = html_dom_query_all(0, "span[id=\"nameSwitch\"]", found, 4);
    for (int i = 0; i < n; i++) {
        char d[16] = "";
        html_dom_get_style(found[i], "display", d, sizeof(d));
        html_dom_set_display(found[i], !strcmp(d, "none") ? "" : "none");
    }
    return 1;
}

// viewItem(index, mode): the item popup. The 3D showcase iframe
// (viewer.html?nobg&showcase=<i>) has no embedded viewer natively: its 2D preview instead.
H(h_view_item) {
    UNUSED;
    const int index = (int) arg_n(args, argc, 0, -1);
    const ViewerSkin *sk = hub_skin(index);
    if (!sk) {
        char n[16];
        snprintf(n, sizeof(n), "%d", index);
        open_hub_url("/social.html?p=itemsales&i=", n, 1);
        return 1;
    }
    reset_pops();
    const char *col = rarity_color(sk);
    const HtmlElem pop = el("itemViewPop");
    if (!pop) return 1;
    html_dom_set_display(pop, "block");
    char border[64];
    snprintf(border, sizeof(border), "5px solid %s", col);
    html_dom_set_style(pop, "border", border);
    html_dom_set_class(pop, "rainbow", rarity_animate(sk));
    char path[256], url[256];
    viewer_skin_preview_path(sk, path, sizeof(path));
    html_dom_asset_url(path, url, sizeof(url));
    StrBuf sb = {0};
    sb_appendf(&sb, "<div class='itemViewName blackShad%s' style='color:%s;'>", g_hub.rarity_anim && rarity_animate(sk) ? " rainbow" : "", col);
    sb_append_esc(&sb, sk->name);
    sb_append(&sb, "</div>");
    if ((sk->seas ? sk->seas : 1) != g_viewer_current_season) sb_append(&sb, "<div class='itemViewSea'>Vaulted</div>");
    else if (sk->flags & VS_BLOCKED) sb_append(&sb, "<div class='itemViewSea' style='background:#faa332;'>Limited</div>");
    sb_appendf(&sb, "<img style='width: 100%%;height: 450px;border:none;margin-top:10px;object-fit:contain;' src='%s'>", url);
    if ((int) arg_n(args, argc, 1, 0) == 2) sb_appendf(&sb, "<div id='itemInfoBtn' onclick='openViewer(%d)'>Open Model Viewer</div>", index);
    else sb_appendf(&sb, "<div id='itemInfoBtn' onclick='openSales(%d)'>More Information</div>", index);
    char *html = sb_finish(&sb);
    html_dom_set_html(pop, html);
    free(html);
    return 0;
}

// openSales(index): the item's sales page in a new tab
H(h_open_sales) {
    UNUSED;
    char n[16];
    snprintf(n, sizeof(n), "%d", (int) arg_n(args, argc, 0, 0));
    open_hub_url("/social.html?p=itemsales&i=", n, 1);
    return 0;
}

// openViewer(index): /viewer.html?class=9&<type>=<index>
H(h_open_viewer) {
    UNUSED;
    const int index = (int) arg_n(args, argc, 0, 0);
    const ViewerSkin *sk = hub_skin(index);
    if (!sk) return 1;
    static const char *const keys[] = {"skin", "hat", "body", "melee", "spray", "dye", "waist", "face"};
    char url[128];
    if (sk->type == 3) snprintf(url, sizeof(url), "/viewer.html?class=9&hidePlayer&melee=%d", index);
    else if (sk->type >= 1 && sk->type <= 7) snprintf(url, sizeof(url), "/viewer.html?class=9&%s=%d", keys[sk->type], index);
    else snprintf(url, sizeof(url), "/viewer.html?class=9&skin=%d", index);
    open_hub_url(url, "", 1);
    return 1;
}

// repPop(kind): the creation report info popup
H(h_rep_pop) {
    UNUSED;
    const char *k = arg_s(ev, args, argc, 0);
    if (!k) k = "";
    StrBuf sb = {0};
    sb_appendf(&sb, "<div style='font-size:20px;color:#fff;margin-bottom:10px'>Report %s</div>If this %s breaches our Terms of Service or Endorses or Contains any of the following:<div style='color:rgba(255,255,255,0.6);margin-top:10px;'>- Copyrighted Materials</div><div style='color:rgba(255,255,255,0.6);'>- Gambling</div><div style='color:rgba(255,255,255,0.6)'>- Scamming</div><div style='height:10px;'></div>Please Report the map name to <a class='menuLink blueLink' style='font-size:16px;margin-right:5px'>krunker@yendis.ch</a> for removal. <div style='margin-top:10px;color:#fff'>If this %s infringes your rights report it to <a class='blueLink'>legal@yendis.ch</a></div>", k, k, k);
    char *html = sb_finish(&sb);
    generic_popup(html, "custPop", NULL);
    free(html);
    return 1;
}

static void share_restore_text(void *user);

// copyPost / shareListing (this, id): the link to the clipboard, "Copied" for 1.25 s
H(h_copy_link) {
    UNUSED;
    if (!ev || !ev->target) return 1;
    char origin[128], url[256];
    html_page_origin(origin, sizeof(origin));
    if (!strcmp(html_ui_current_handler(), "copyPost")) snprintf(url, sizeof(url), "%s/social.html?p=post&q=%d", origin, (int) arg_n(args, argc, 1, 0));
    else snprintf(url, sizeof(url), "%s/social.html?p=listing&i=%d", origin, (int) arg_n(args, argc, 1, 0));
    html_clipboard_write(url);
    html_dom_set_text(ev->target, "Copied");
    html_timer_add(1250, share_restore_text, (void *) (uintptr_t) ev->target);
    return 1;
}

// retainSelected / retainChecked: v7 keeps the filter popups' markup in sync for its
// innerHTML copy; the state lives in g_filter_state here (filter_state_save)
H(h_retain) { UNUSED; return 1; }

// item list handlers (itemFilter, itemFilterSearch, itemFilterChange, itemListScroll)
H(h_item_filter) {
    UNUSED;
    const char *k = arg_s(ev, args, argc, 0);
    char v[32];
    if (argc > 1 && args[1].kind == HTML_ARG_STR) snprintf(v, sizeof(v), "%s", args[1].str ? args[1].str : "");
    else if (argc > 1 && args[1].kind == HTML_ARG_BOOL) snprintf(v, sizeof(v), "%s", args[1].num ? "true" : "false");
    else js_num(v, sizeof(v), arg_n(args, argc, 1, 0));
    if (k) items_filter(k, v);
    return 1;
}
H(h_item_filter_search) {
    UNUSED;
    char buf[128] = "";
    const char *v = arg_s(ev, args, argc, 0);
    if ((!v || !*v) && ev && ev->target) {
        html_dom_get_value(ev->target, buf, sizeof(buf));
        v = buf;
    }
    items_search(v);
    return 1;
}
H(h_item_filter_change) { UNUSED; items_filter_change(argc > 0 && args[0].kind == HTML_ARG_THIS ? (HtmlElem) args[0].num : ev ? ev->target : 0); return 1; }
H(h_item_list_scroll) { UNUSED; items_more(); return 1; }

H(h_maps_switch_tab) { UNUSED; list_switch_tab(0, (int) arg_n(args, argc, 0, 0), 0); return 1; }
H(h_mods_switch_tab) { UNUSED; list_switch_tab(1, (int) arg_n(args, argc, 0, 0), 0); return 1; }
H(h_maps_load_more) { (void) user; (void) args; (void) argc; list_load_more(0, ev ? ev->target : 0); return 1; }
H(h_mods_load_more) { (void) user; (void) args; (void) argc; list_load_more(1, ev ? ev->target : 0); return 1; }

H(h_maps_search) {
    UNUSED;
    char value[128] = "";
    const HtmlElem input = el("mpSrch");
    if (input) html_dom_get_value(input, value, sizeof(value));
    set_html("searchResultsMA", "Loading...");
    api_request("/search", 0, 0, "map", value); // API_CLIENT.searchMap(value, category): the server matches names
    return 1;
}

H(h_mods_search) {
    UNUSED;
    char value[128] = "";
    const HtmlElem input = el("mdSrch");
    if (input) html_dom_get_value(input, value, sizeof(value));
    set_html("searchResultsMO", "Loading...");
    api_request("/search", 3, 0, "mod", value);
    return 1;
}

static void open_hub_url(const char *prefix, const char *name, const int new_tab) {
    char url[512];
    snprintf(url, sizeof(url), "%s%s", prefix, name ? name : "");
    if (!app_route_url_target(url, new_tab)) ui_open_external(url);
}

// openMap/openMod/openProfile, playMap, hostMap, playMod: "_blank" (v7.1.4)
H(h_open_map) { UNUSED; open_hub_url("/social.html?p=map&q=", arg_s(ev, args, argc, 0), 1); return 1; }
H(h_open_mod) { UNUSED; open_hub_url("/social.html?p=mod&q=", arg_s(ev, args, argc, 0), 1); return 1; }
H(h_open_profile) { UNUSED; open_hub_url("/social.html?p=profile&q=", arg_s(ev, args, argc, 0), 1); return 1; }
H(h_play_map) { UNUSED; open_hub_url("/?play=", arg_s(ev, args, argc, 0), 1); return 1; }
H(h_play_mod) { UNUSED; open_hub_url("/?mod=", arg_s(ev, args, argc, 0), 1); return 1; }

// updateMapTab(tab, canEdit): About / Servers / Leaderboard / Data on the map page
H(h_update_map_tab) {
    UNUSED;
    g_map_tab = (int) arg_n(args, argc, 0, 0);
    const int can_edit = argc > 1 && args[1].kind == HTML_ARG_BOOL && args[1].num != 0;
    StrBuf sb = {0};
    append_map_tabs(&sb, can_edit);
    char *html = sb_finish(&sb);
    set_html("mapTabsP", html);
    free(html);
    set_display("mapDescr", g_map_tab == 0 ? "block" : "none");
    set_display("mapVid", "none");
    set_display("activeGames", g_map_tab == 1 ? "block" : "none");
    set_display("mapLeaders", g_map_tab == 2 ? "block" : "none");
    if (el("mapData")) set_display("mapData", g_map_tab == 3 ? "block" : "none");
    return 1;
}

// shareMapLink / shareModLink(this, name): UTILS.copyToClipboard, then the button reads
// "Copied" for 1.25 s
static void share_restore(void *user) {
    const HtmlElem b = (HtmlElem) (uintptr_t) user;
    if (b) html_dom_set_text(b, "Share");
}
static void share_restore_text(void *user) { share_restore(user); }

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
    {"changeDateRange2", h_change_date_range},
    // v7.1.4
    {"playTick", h_play_tick},
    {"playSelect", h_play_select},
    {"switchSortFilter", h_switch_filter},
    {"toggleSortFilters", h_toggle_filters},
    {"resetSortFilter", h_reset_filter},
    {"retainSelected", h_retain},
    {"retainChecked", h_retain},
    {"openCreationTab", h_open_creation_tab},
    {"switchStat", h_switch_stat},
    {"switchNames", h_switch_names},
    {"viewItem", h_view_item},
    {"openSales", h_open_sales},
    {"openViewer", h_open_viewer},
    {"repPop", h_rep_pop},
    {"copyPost", h_copy_link},
    {"shareListing", h_copy_link},
    {"itemFilter", h_item_filter},
    {"itemFilterSearch", h_item_filter_search},
    {"itemFilterChange", h_item_filter_change},
    {"itemListScroll", h_item_list_scroll},
    {"openOneTrust", h_noop},   // no ads / consent manager in KrunkNative
    {"closeOneTrust", h_noop},
    {"logBtnFRVR", h_noop},     // analytics
    {"imagePopup", h_noop},     // the custom background collection (krk_custImgs): not ported
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
    // API_CLIENT.listMaps(2, 0): the featured games of the maps Home card (v3540); KN's /maps
    // index 1 is Featured
    api_request("/maps", 1, 0, NULL, NULL);
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

// The listData of both windows, the featured picks and the item list.
static void hub_lists_free(void) {
    for (int i = 0; i < 2; i++) {
        HubList *list = i ? &g_hub.mods : &g_hub.maps;
        for (int t = 0; t < HUB_LIST_TABS; t++) list_reset(&list->data[t]);
        cJSON_Delete(list->home_rows);
        list->home_rows = NULL;
    }
    free(g_items.list);
    g_items.list = NULL;
    g_items.count = g_items.shown = 0;
}

static void social_enter(Client *client, const char *query) {
    hub_lists_free();
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
    // the app badges: iosDB / androidDB when window.canShowExternalLinks (always true in
    // KrunkNative, as in ui_handlers.c), steamDB always
    set_display("iosDB", "block");
    set_display("androidDB", "block");
    set_display("steamDB", "block");

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
    hub_lists_free();
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
