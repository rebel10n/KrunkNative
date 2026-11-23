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
#include <ui_tooltip.h>
#include <image_collection.h>
#include <backend_ui.h>
#include <kn_captcha.h> // the hub login's captcha (doc/v10/captcha.md)
#include <krunker_account.h> // krunker_host_url: the data client's webhooks (the hub's maps, mods and search)
#include <krunker_kio_routes.h> // the webhook groups and URL of those requests (krunker_kio_routes.c)
#include <weapons.h>
#include <cJSON.h>
#include <html_dom.h>
#include <html_ui.h>
#include <social.h>
#include <badges.h>    // badge_get: the earned badges of a profile (Badges tab)
#include <ranked.h>    // ranked_rank_icon: the profile header's rank (rankedRank)
#include <v10_social.h> // v10_gapi: the giveaway post (gapi POST /social/post)
#include <ui_logic.h>
#include <ui_vgrid.h> // the market / inventory / item lists are virtual grids
#include <youtube_link.h> // a map's YouTube video (map_videolink / vl): the featured card and map page link
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> // strncasecmp
#include <flavor.h> // the game list's flavor pool (the map page's Servers, the cards' live players)
#include <time.h>
#ifdef _WIN32
#define timegm _mkgmtime // MinGW has no timegm (same as editor_net.c)
#endif
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#include "../viewer/viewer_store.h"
#include <ruleset.h>
#include "account_flags.h" // the profile's #socialFlag (stats.flg: countries[flg][1], hub-10.0.7.js:31, 2773-2776)
#include <modes.h> // mode_config: a game-results post's end stats
#include <market_bulk.h> // market_qs_value: the Quick Sell popup's KR / Junk

#define HUB_KEY_MAX 128

// The economy rules (doc/v7/economy.md): v7.1.4 src/config.js until kn_brain's "ecfg" reply
// replaces them with what the server really charges (gift / list fees, levels, limits); the
// protocol half is economy_client.c (shared with the v10 Market & Trading popup, v10_market.c).
#include <economy_client.h>
#define g_econ g_econ_rules
#define MARKET_MIN_LVL ((int) g_econ.market_min_lvl)
#define TRADE_MIN_LVL ((int) g_econ.trade_min_lvl)

// ------------------------------------------------------------------ v3.1.1 data (src/config.js, i18n/en.js)


static const char *const k_social_regions[] = {"All Regions", "Oceania", "Asia", "Europe", "North America", "South America"};

// navigation buttons (vA177) and the pages without one (vA178)
static const char *const k_nav_pages[] = {"leaders", "mods", "maps", "market", "feed", "items"};
static const char *const k_sub_pages[] = {"itemsales", "clan", "map", "mod", "search", "profile", "post", "listing"};
static const char *const k_market_queries[] = {"market", "inventory", "sales", "stats", "trades", "blackm"}; // config.marketQueries

// i18n (client/include/i18n.h): looked up where they are written
#define STR_MAPS_NONE I18N("social.maps.none", "No Games found...")
#define STR_MODS_NONE I18N("social.mods.none", "No Mods found...")
#define STR_WIN_MAPS_NONE I18N("windows.maps.none", "No Games found!")
#define STR_WIN_MODS_NONE I18N("windows.mods.none", "No Mods found")
#define STR_MARKET_NONE I18N("social.market.none", "No Items Found")
#define STR_MARKET_LOGIN I18N("social.market.login", "You must be logged in.")
#define STR_MARKET_FAILED I18N("social.market.failed", "Failed. Try Again")

// ------------------------------------------------------------------ settings (vO78 = {...})

typedef enum { SET_SELECT, SET_SLIDER, SET_SWITCH, SET_HIDDEN, SET_URL } HubSettingKind;

typedef struct {
    const char *key;
    const char *i18n;   // vO78[key].name: the i18n key openSettings translates
    const char *name;   // its en.json text
    const char *info;   // vO78[key].info ("" = none)
    HubSettingKind kind;
    double def, min, max, step;
    int hide;           // -1 = no `hide` field, 0/1 = vO78[key].hide
    double val;
    char str[2048];     // SET_URL: the value (background image URL)
} HubSetting;

enum {
    HS_REGION, HS_SCALE_UI, HS_UI_SCALING, HS_RARITY_ANIM, HS_SHOW_PFP, HS_SHOW_TRADE_MSG, HS_HIDE_NON_TRADE,
    HS_SOUND, HS_AMBIENT_VOLUME, HS_UI_VOLUME, HS_MENU_SATURATION, HS_MENU_HUE, HS_BACKGROUND, HS_COUNT
};

static HubSetting g_settings[HS_COUNT] = {
    // hub-10.0.7.js:1754-1920
    {"region", "settings.social.region", "Region Stats", "", SET_SELECT, 0, 0, 0, 0, -1, 0, ""},
    {"scaleUI", "settings.interface.scale", "UI Scale", "", SET_SLIDER, 0.7, 0.1, 1, 0.1, -1, 0, ""},
    {"uiScaling", "settings.interface.scale", "UI Scale", "", SET_HIDDEN, 1, 0, 0, 0, 1, 0, ""},
    {"disableRarityAnim", "settings.interface.rarityanim", "Disable Rarity Animations", "", SET_SWITCH, 0, 0, 0, 0, -1, 0, ""},
    {"showPfp", "settings.interface.showPfp", "Show Profile Pictures", "", SET_SWITCH, 1, 0, 0, 0, -1, 0, ""},
    {"showTradeMsg", "settings.interface.showTradeMsg", "Show Trade Messages", "", SET_SWITCH, 1, 0, 0, 0, -1, 0, ""},
    {"hideNonTrade", "settings.interface.hideNonTrade", "Hide Non-Tradable Skins", "", SET_SWITCH, 0, 0, 0, 0, -1, 0, ""},
    {"sound", "settings.audio.sound", "Master Volume", "", SET_SLIDER, 1, 0, 1, 0.01, -1, 0, ""},
    {"ambientVolume", "settings.audio.ambient", "Ambient Volume", "Background sound volume", SET_SLIDER, 1, 0, 1, 0.01, -1, 0, ""},
    {"uiVolume", "settings.audio.ui", "UI Volume", "Menu interface volume", SET_SLIDER, 1, 0, 1, 0.01, -1, 0, ""},
    {"menuSaturation", "settings.editing.menuSaturation", "Saturation (UI)", "Increase color saturation for menu", SET_SLIDER, 1, 0, 3, 0.1, -1, 0, ""},
    {"menuHue", "settings.editing.menuHue", "Color Hue (UI)", "Shift colors in-menu", SET_SLIDER, 0, 0, 100, 5, -1, 0, ""},
    {"background", "settings.social.bg", "Background Image", "", SET_URL, 0, 0, 0, 0, -1, 0, ""},
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
    int page_after_login;   // the connect's page request waits for that login (hub_connected)
    int logged_in;          // acc != null
    char profile_name[64];  // profile.player_name (follow / followers)
    int profile_id;
    int profile_featured, profile_hack; // player_featured / player_hack of the profile (Verify, Flag)
    int profile_creator;                // creator_badge of the profile (v10's creator badge: Creator / Uncreator)
    int profile_level;                  // the profile's level (iïìîiií: its Trade button needs config.tradeMinLvl)
    int pub_mo_pending;                 // iîïííiî: ?openPubMo waits for the login
    int profile_chat_banned;            // the profile's chat ban in force (GET /moderation/chat/:name, staff only)
    double profile_followed, profile_following;
    int giveaway;           // the post box's KR giveaway switch (togglPos)
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

// "<text>..." in the market / listing holders (social.market.none / .login + "...")
static void set_market_note(const char *id, const char *text) {
    char html[256];
    snprintf(html, sizeof(html), "<div style='font-size:20px;margin-top:20px;color:#fff'>%s...</div>", text);
    set_html(id, html);
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

// the hub's login frame once its captcha is done (kn_captcha_send_frame keeps and wipes the original)
static void hub_send_login(const cJSON *frame) { hub_send(cJSON_Duplicate(frame, 1)); }

// ------------------------------------------------------------------ popups

// The market filter / sort panels keep their state across popups (v7 saves #genericPop's
// innerHTML in resetPops when it holds one: v3528 / v3527); here the checkbox and field
// values are read back into these before the popup goes.
typedef struct {
    int built;
    int checked[3][40];  // [rarity | weapon | type][index]
    char text[128], season[8], order[8];
    int missing, non_tradable; // sortPopup's Only Show Missing / Hide Non-Tradable Skins
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

// showPopup() with no item: closes the popup (popupBack's onclick); lastPop becomes that empty call (hub-10.0.7.js:4791)
static void last_pop_clear(void);
static void show_popup_none(void) {
    last_pop_clear();
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
static int hub_is_url(const char *u);
static void hub_mod_popup(const cJSON *mod);
static void profile_chat_ban_fetch(void);

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
                const AccountInfo *racc = g_hub.logged_in ? active_account() : NULL;
                cJSON_AddItemToArray(f, racc ? cJSON_CreateNumber(racc->id) : cJSON_CreateNull()); // K: krunker_id
                cJSON_AddItemToArray(f, racc ? cJSON_CreateString(racc->name) : cJSON_CreateNull()); // q: krunker_username
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
            const int ok = hub_is_url(u) && !strstr(u, ".svg");
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

// isURL (config-10.0.7.js:4102-4108, image_collection_is_url). v10's built-in images are its asset URLs
// (iíìïïìï("/img/backgrounds/0.png")); this host's assets are the page's own img/ paths, taken as such.
static int hub_is_url(const char *u) {
    if (u && (!strncmp(u, "img/", 4) || !strncmp(u, "./img/", 6)) && !strstr(u, "..")) return 1;
    return image_collection_is_url(u);
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
    char cleaned[2048];
    size_t o = 0;
    for (const char *p = value ? value : ""; *p && o + 1 < sizeof(cleaned); p++) {
        if (*p != '<' && *p != '>') cleaned[o++] = *p;
    }
    cleaned[o] = 0;
    if (s->kind == SET_URL) setting_set_url(s, value ? cleaned : s->str);
    else s->val = value ? setting_parse(s, cleaned) : s->val;
    char text[2048], id[96];
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
        char skey[64], saved[2048];
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
    char val[2048], min[32], max[32], step[32];
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
            sb_appendf(sb, "<input type='url' name='url' id='slid_%s' class='inputGrey2' placeholder='%s'\n\t\t\t\t\t\t\tvalue='", s->key, I18N("settings.social.bg.paste", "Background Image URL"));
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
        sb_appendf(&sb, ">%s ", I18N(s->i18n, s->name)); // iíìíìíi.t(name)
        if (s->kind == SET_URL) sb_appendf(&sb, "<div class='openCollection material-icons' onclick='imagePopup(&quot;%s&quot;)'>collections</div>", s->key);
        append_setting_html(&sb, s);
        sb_append(&sb, "</div>");
    }
    // hub-10.0.7.js:1968: the "Manage Ad Preferences" button; openOneTrust does nothing without window.OneTrust (:1972-1984),
    // and this host loads no OneTrust
    sb_append(&sb, "<div style='background-color: #994cd1!important;font-size: 17px;padding: 5px;border-radius: 4px;text-align: center;margin-top: 10px;color: rgba(255, 255, 255, 1) !important;' onclick='openOneTrust()'>Manage Ad Preferences</div>");
    sb_append(&sb, "</div>");
    char *html = sb_finish(&sb);
    generic_popup(html, "settingsPop", NULL);
    free(html);
}

// ------------------------------------------------------------------ side boxes
// v10 (social/renamed.js:2389-2391): only the ad box; v7.1.4's f492 also filled Krunk TV
// (#leftBox) and Who to Follow (#rightFollBox), which v10's page no longer has.
static void side_boxes(void) {
    set_display("aBox", "block");
}

// ------------------------------------------------------------------ maps / mods lists (windows[0], windows[1])

static double parse_time_ms(const cJSON *v);
static double now_epoch_ms(void);

// ------------------------------------------------------------------ profile pictures
// v10's picture of a player (config-BDN_kuhS.js:4749-4757): <userAssetsHost>/p<id>/profile.png?v=<the hour>, the
// hour being the player's own upload time after an upload (pfpUploaded). The host is Box Office's for a Box Office
// account (kn_brain answers p<id>/profile.png with the approved profile picture, avatar.c; 404 without one) and
// krunker.io's for a krunker.io account, whose ids are Krunker's.
static double g_pfp_up_id, g_pfp_up_ms;

void social_pfp_uploaded(const int id, const double ms) {
    g_pfp_up_id = id;
    g_pfp_up_ms = ms;
}

static void pfp_url(char *out, const size_t n, const double id) {
    double v = floor(now_epoch_ms() / 3600000.0) * 3600000.0;
    if (g_pfp_up_id == id && g_pfp_up_ms > v) v = g_pfp_up_ms;
    if (account_backend() == ACCOUNT_BACKEND_KRUNKER || !backend_url()[0])
        snprintf(out, n, "https://user-assets.krunker.io/p%.0f/profile.png?v=%.0f", id, v);
    else
        snprintf(out, n, "%s/user-assets/p%.0f/profile.png?v=%.0f", backend_url(), id, v);
}

// iíìiíïi(img, id, cond, fallback) (hub-10.0.7.js:2143-2149): the picture when cond, else the fallback, which
// also replaces a picture that fails to load
static void set_pfp(const HtmlElem img, const double id, const int cond, const char *fallback) {
    if (!img) return;
    char src[512], on[300];
    if (cond) pfp_url(src, sizeof(src), id);
    else snprintf(src, sizeof(src), "%s", fallback);
    snprintf(on, sizeof(on), "this.onerror=null;this.src='%s';", fallback);
    html_dom_set_attr(img, "onerror", on);
    html_dom_set_attr(img, "src", src);
}

// the picture's URL as markup: cond ? pfp(id) : fallback ("" or ./img/player_pfp.png)
static void sb_pfp(StrBuf *sb, const double id, const int cond, const char *fallback) {
    char src[512];
    if (cond) pfp_url(src, sizeof(src), id);
    else snprintf(src, sizeof(src), "%s", fallback);
    sb_append_esc(sb, src);
}

// iíiiiîì(path) (config-10.0.7.js:4729-4744): the user-assets host + path, Krunker's for a krunker.io account, else
// kn_brain's /user-assets (m<id>/thumb.png: the map's published thumbnail, user_assets.c; md<id>: the mod's)
static void ua_url(char *out, const size_t n, const char *path) {
    if (account_backend() == ACCOUNT_BACKEND_KRUNKER || !backend_url()[0]) snprintf(out, n, "https://user-assets.krunker.io%s", path);
    else snprintf(out, n, "%s/user-assets%s", backend_url(), path);
}

// iîìîííi(info).t: the creation's map_info / mod_info (JSON text, or an object) says it has a thumbnail
static int info_thumb(const cJSON *r, const char *key) {
    const cJSON *v = r ? cJSON_GetObjectItemCaseSensitive(r, key) : NULL;
    cJSON *parsed = cJSON_IsString(v) && v->valuestring ? cJSON_Parse(v->valuestring) : NULL;
    const cJSON *o = parsed ? parsed : v;
    const cJSON *t = cJSON_IsObject(o) ? cJSON_GetObjectItemCaseSensitive(o, "t") : NULL;
    const int yes = t && !cJSON_IsFalse(t) && !cJSON_IsNull(t) && !(cJSON_IsNumber(t) && t->valuedouble == 0) && !(cJSON_IsString(t) && !t->valuestring[0]);
    cJSON_Delete(parsed);
    return yes;
}

// a creation's picture: `info.t ? <host>/<m|md><id>/thumb.png : image || ./img/noimg.png` (hub-10.0.7.js:3023, 3066,
// 3142, 3163, 6534, 6611)
static void sb_creation_thumb(StrBuf *sb, const int is_mod, const double id, const int thumb, const char *image) {
    if (thumb) {
        char path[48], url[400];
        snprintf(path, sizeof(path), "/m%s%.0f/thumb.png", is_mod ? "d" : "", id);
        ua_url(url, sizeof(url), path);
        sb_append_esc(sb, url);
    } else {
        sb_append_esc(sb, image && *image ? image : "./img/noimg.png");
    }
}

static cJSON *g_friends; // the last "chtb" rows, online first (owned; the hub chat below)
static double jnum(const cJSON *o, const char *key);
static const char *jstr(const cJSON *o, const char *key);
static void hub_title(const char *name, int escape, const char *suffix);

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

// api_request's `category` for a search: cat= (-1 = every category, what v10's searchMap sends for mpCat's All);
// NO_CATEGORY leaves cat= out (the lists, the mods search)
#define NO_CATEGORY (-2)
static void api_request(const char *path, const int index, const int pos, const char *search_type, const char *val, const int category) {
    cJSON *params = cJSON_CreateObject();
    if (search_type) {
        cJSON_AddStringToObject(params, "type", search_type);
        cJSON_AddStringToObject(params, "val", val ? val : "");
        if (category != NO_CATEGORY) cJSON_AddNumberToObject(params, "cat", category);
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
    if (t && t->api >= 0) api_request(is_mods ? "/mods" : "/maps", t->api, pos, NULL, NULL, NO_CATEGORY);
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

static double hub_player_data(const char *name);

// Home's lists (hub-10.0.7.js:6449-6460 maps, 6542-6553 mods): the rows bucketed by category in first-seen order
// ({}[category] ||= [], then for..in: category names are never array indices, so insertion order), joined back
static void group_rows_by_category(cJSON *rows) {
    const int n = cJSON_GetArraySize(rows);
    if (n < 2) return;
    cJSON **items = calloc((size_t) n, sizeof(*items));
    if (!items) return;
    for (int i = n; i-- > 0;) items[i] = cJSON_DetachItemFromArray(rows, i);
    for (int i = 0; i < n; i++) {
        if (!items[i]) continue;
        const char *cat = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(items[i], "category"));
        for (int j = i; j < n; j++) {
            if (!items[j]) continue;
            const char *c2 = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(items[j], "category"));
            if ((!cat && !c2) || (cat && c2 && !strcmp(cat, c2))) {
                cJSON_AddItemToArray(rows, items[j]);
                items[j] = NULL;
            }
        }
    }
    free(items);
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
    if (!cJSON_IsArray(rows_in) || !cJSON_GetArraySize(rows_in)) sb_appendf(sb, "<span style='color:rgba(0,0,0,0.4);'>%s</span>", STR_MAPS_NONE);
    if (!cJSON_IsArray(rows_in)) return;
    const HubTab *cfg = list_tab(0, tab);
    cJSON *rows = cJSON_Duplicate(rows_in, 1);
    if (tab == 0) {
        shuffle_rows(rows);
        group_rows_by_category(rows);
        sb_append(sb, "<div class='mapListCat'>Popular Categories</div>");
        StrBuf cats = {0};
        // A tile runs v10's windows[0].switchTab(7);mpCat.selectedIndex=idx+1;windows[0].searchMaps() (hub-10.0.7.js:6466).
        // Tab 7 of v10.0.7's table is the hidden "New & Popular" list, which draws no #mpCat: the statement throws
        // there and searchMaps never runs, so a tile opens that list. Kept as v10 behaves: the switch alone.
        for (size_t i = 0; i < sizeof(k_map_cats_p) / sizeof(k_map_cats_p[0]); ++i)
            sb_appendf(&cats, "<div class='mapListItem' style='width:450px;height:130px;'><div class='mapLICat' onclick='windows[0].switchTab(7);' style='background:url(./img/backgrounds/gamecats/c_%d.png)'>%s</div></div>",
                       k_map_cats_p[i].i, k_map_cats_p[i].n);
        char *c = sb_finish(&cats);
        sb_appendf(sb, "<div class='mapCatScrl'>%s</div>", c);
        free(c);
    }
    const AccountInfo *acc = g_hub.logged_in ? active_account() : NULL;
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
        char id[32], plays[32], fund[32], fund_raw[32];
        js_num(id, sizeof(id), map_id);
        format_num_yt(plays, sizeof(plays), row_num2(r, "map_pl", NULL));
        comma_number(fund, sizeof(fund), row_num2(r, "fund", NULL));
        js_num(fund_raw, sizeof(fund_raw), row_num2(r, "fund", NULL));
        sb_append(sb, "<div style='display:inline-block;margin-bottom:20px;vertical-align:top;'><div class='mapListItem'>");
        if (age >= 0 && age <= 7) {
            const int upd = row_num2(r, "map_updatecounter", NULL) > 1;
            sb_appendf(sb, "<div class='mapAgeD' style='background-color:%s'>%s</div>", upd ? "#0080ff" : "#da00ff", upd ? "UPDATED" : "NEW");
        }
        sb_append(sb, "<div class='creationActions'>");
        if (edit) {
            sb_append(sb, "<span title='Edit' class='material-icons creationAction edit' onclick='openEdit(&quot;");
            sb_append_js_escape(sb, name);
            sb_appendf(sb, "&quot;,%s,%s)'>create</span>", id, fund_raw);
        }
        if (del) sb_appendf(sb, "<span title='Delete' class='material-icons creationAction del' onclick='deleteCreationConf(%s,0);'>delete_forever</span>", id);
        if (acc && acc->is_dev) sb_appendf(sb, "<span title='Feature' class='material-icons creationAction%s' onclick='featureCreation(this, %s,0);'>check_circle</span>", row_num2(r, "map_featured", NULL) ? " sel" : "", id);
        sb_appendf(sb, "<span title='Favorite' class='material-icons creationAction%s' onclick='favoriteM(0, this, %s);'>grade</span></div><div class='mapListThumbHolder'><img draggable='false' class='mapListThumb' loading='lazy' src='", fav ? " sel" : "", id);
        sb_creation_thumb(sb, 0, map_id, info_thumb(r, cJSON_GetObjectItemCaseSensitive(r, "inf") ? "inf" : "map_info") || info_thumb(r, "mi"), image);
        sb_append(sb, "' onerror='this.onerror=null;this.src=\"./img/noimg.png\";' onclick='openMap(&quot;");
        sb_append_js_escape(sb, name);
        sb_append(sb, "&quot;)'/>");
        if (edit) sb_appendf(sb, "<div class='mapsVotes' style='top: 145px;right: 9px;' title='KR Bank'>%s<span style='margin-left: 8px;color:#FBC02D;font-size: 17px;'>KR</span></div>", fund);
        sb_append(sb, "<div class='mapsVotes' title='Live Players' id='pcnt_");
        sb_append_js_escape(sb, name);
        // windows[0].playerData[name] || 0: the custom games of the game list on that map (hub_players_update)
        char live[24];
        js_num(live, sizeof(live), hub_player_data(name));
        sb_appendf(sb, "' style='left:10px;right:unset;top:145px;'><span class='material-icons' style='margin-right: 8px;color:rgba(255, 255, 255, 0.5);vertical-align: middle;margin-top:-5px;font-size:30px;'>person</span>%s</div><div class='mapListCrdI'><a href='javascript:;' style='color:#fff !important;margin-left:6px;' onclick='openMap(&quot;", live);
        sb_append_js_escape(sb, name);
        sb_appendf(sb, "&quot;,&quot;%s&quot;,&quot;%s&quot;)'/>", mod_url ? mod_url : "undefined", mod_id ? mod_id : "undefined");
        sb_append(sb, name);
        sb_appendf(sb, "</a><span style='float:right;color:#fff;margin-right:6px;'>%s <span style='color:rgba(255,255,255,0.6)'>Plays</span></span></div></div></div>", plays);
        // the friends playing it (at most 4 pictures): an online friend whose status is "playing <map>"
        if (cJSON_GetArraySize(g_friends)) {
            int playing = 0;
            const char *who = NULL;
            const cJSON *f;
            sb_append(sb, "<div class='frndPlyingH'>");
            cJSON_ArrayForEach(f, g_friends) {
                const char *sm = jstr(f, "sm");
                if (playing >= 4 || jnum(f, "st") != 1 || !sm || !*sm) continue;
                const char *at = strstr(sm, "playing ");
                char map[160];
                if (at) snprintf(map, sizeof(map), "%.*s%s", (int) (at - sm), sm, at + 8); // String.replace: the first one
                else snprintf(map, sizeof(map), "%s", sm);
                if (strcmp(map, name)) continue;
                sb_append(sb, "<img class='frndPlyinIcn' src='");
                sb_pfp(sb, jnum(f, "fid"), jnum(f, "sub") != 0, "");
                sb_append(sb, "'>");
                who = jstr(f, "fna");
                playing++;
            }
            if (playing > 1) sb_append(sb, " several friends playing");
            else if (playing) {
                sb_append(sb, " ");
                sb_append_esc(sb, who ? who : "");
                sb_append(sb, " is Playing");
            }
            sb_append(sb, "</div>");
        }
        sb_append(sb, "</div>");
    }
    cJSON_Delete(rows);
}

// f545(rows, tab): mod cards
static void append_mod_cards(StrBuf *sb, const cJSON *rows_in, const int tab) {
    if (!cJSON_IsArray(rows_in) || !cJSON_GetArraySize(rows_in)) sb_appendf(sb, "<span style='color:rgba(0,0,0,0.4);'>%s</span>", STR_MODS_NONE);
    if (!cJSON_IsArray(rows_in)) return;
    cJSON *rows = cJSON_Duplicate(rows_in, 1);
    if (tab == 0) group_rows_by_category(rows); // no shuffle for mods
    const HubTab *cfg = list_tab(1, tab);
    const AccountInfo *acc = g_hub.logged_in ? active_account() : NULL;
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
        sb_creation_thumb(sb, 1, mod_id, info_thumb(r, cJSON_GetObjectItemCaseSensitive(r, "minf") ? "minf" : cJSON_GetObjectItemCaseSensitive(r, "inf") ? "inf" : "mod_info"), image);
        sb_append(sb, "' onerror='this.onerror=null;this.src=\"./img/noimg.png\";' onclick='openMod(&quot;");
        sb_append_js_escape(sb, name);
        sb_append(sb, "&quot;)'/><div class='mapListCrdI'><a href='javascript:;' style='color:#fff !important;margin-left:6px;' onclick='openMod(&quot;");
        sb_append_js_escape(sb, name);
        sb_append(sb, "&quot;)'/>");
        sb_append(sb, name);
        sb_appendf(sb, "</a><span style='float:right;color:#fff;margin-right:6px;'>%s <span style='color:rgba(255,255,255,0.6)'>Likes</span></span></div></div></div>", votes);
    }
    cJSON_Delete(rows);
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
            else if (!t->fav && !t->no_pages) sb_appendf(sb, "<div class='loadMoreD' onclick='windows[%d].loadMore(this)'>Load More</div>", is_mods);
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

// A map's YouTube video (map_videolink in the lists, vl on the map page: the id kn_brain stores,
// shared/include/youtube_link.h). v10 embeds it (an iframe, v10:213293); natively a YouTube button
// opens https://www.youtube.com/watch?v=<id> in the browser. Nothing without a valid id.
// config.fetMapVids / fetModVids (config-10.0.7.js:9351-9390): the featured card's video when the row has none
typedef struct { const char *name, *vid; } FetVid;
static const FetVid k_fet_map_vids[] = {
    {"Atrox", "ieuJNE2lPgM"}, {"BattleRoyaleXXL", "1"}, {"Black_Tower", "AjBeYdd8tm8"}, {"BowsersCastle", "eWheNpYzEZ8"},
    {"CS1.6_dust2", "2aBrAAtlKyI"}, {"CS1.6_train", "1"}, {"DQ_Battleboats_1", "nfcnpwCJRIs"}, {"FacingWorlds", "xt_5DjpwOiY"},
    {"FallBots", "zWZ4Rn_cy0o"}, {"Firefly_Defense", "1"}, {"Firefly_Royale", "1"}, {"Flood_Escape", "oOnuyqoXezY"},
    {"Gangville", "JX-Z1xdWUfc"}, {"JailBird", "xtA0ceyl3jo"}, {"LaserWar", "y-BW5MftcuY"}, {"Lava_Run", "P4WZhhXYI88"},
    {"Minecraft_Ville", "195ha7DvRT8"}, {"Parkour_Growth", "qclcH-sIlg0"}, {"Pizza_place", "lZyVcTyHaoY"},
    {"Portal_Puzzles", "199vP_Lcx0o"}, {"Pristis", "XdMQr0Kax88"}, {"STAR_WARS_versus", "1"}, {"Saucer_Labs", ""},
    {"Sky_Wars!", "raB3WyRNZXU"}, {"SpongeGun", "-DtGLkEwFa8"}, {"TNT_Run!", "1"}, {"Ten-Pin_Bowling", "1"},
    {"TheCliff", "WGtV-4xPQ5k"}, {"Walljump_6.4", "8P5ORjnUlcw"}, {"War_of_Krunk", "1"}, {"Warlord", "1"},
    {"Zombie_Bulwark", "2XXCQo4YpSQ"}, {"race_AirshpPnbal", "R86AuYFTmls"}, {"race_Mariokart2", "vVRG5pa39GE"},
    {"slide_daydream", "JKz4SRQaA7k"}, {"slide_moonlight", "UEZCekc7jjs"}, {NULL, NULL},
};
static const FetVid k_fet_mod_vids[] = {{"RGBRave", "K4FjJRjoAUM"}, {NULL, NULL}};
static const char *fet_video(const FetVid *t, const char *name) {
    for (; name && t->name; t++)
        if (!strcmp(t->name, name)) return t->vid;
    return NULL;
}

static int append_video_link(StrBuf *sb, const char *stored, const char *id, const int big) {
    char url[64];
    if (!kn_youtube_watch_url(stored, url, sizeof(url))) return 0;
    sb_appendf(sb, "<div id='%s' class='buttonR' title='%s' onclick='openLink(&quot;%s&quot;, &quot;_blank&quot;)' onmouseenter='playTick(0.1);' "
                   "style='cursor:pointer;pointer-events:all;display:inline-flex;align-items:center;justify-content:center;gap:12px;"
                   "background-color:#ff0000;color:#fff;border-radius:8px;padding:%s;font-size:%dpx;'>"
                   "<img src='./img/youtube.png' style='width:%dpx;height:%dpx;' draggable='false'>Watch on YouTube</div>",
               id, url, url, big ? "14px 26px" : "8px 16px", big ? 24 : 18, big ? 40 : 28, big ? 40 : 28);
    return 1;
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
            (void) image;
            sb_creation_thumb(&out, 0, row_num2(f, "mid", "map_id"), 1, NULL); // hub-10.0.7.js:6179: always the map's thumb.png
            sb_appendf(&out, ");'><div id='bigMFeatName'>%s</div><div id='bigMFeatVbar'><span style='color:#fff;'>%s</span><span class='material-icons' style='margin-top:-8px;margin-left:0px;font-size:46px;vertical-align:middle;color:rgba(255,255,255,0.5);margin-right:10px;'>play_arrow</span>%s<span class='material-icons' style='margin-left:10px;font-size:33px;vertical-align:top;color:rgba(255,255,255,0.5);'>thumb_up</span><div style='color:#7d7d7d;margin-left:20px;margin-top:-12px;display:inline-block;vertical-align:middle;'>by <a target='_blank' href='/social.html?p=profile&q=%s' style='color: #2196F3;'>%s</a></div></div><div id='bigMFeatDesc'>%s</div><div id='bigMFeatIBtn' class='button' onclick='openURL(&quot;/social.html?p=map&q=%s&quot;, 1)' style='color:#fff !important;'>?</div><div id='bigMFeatPBtn' class='btnAnim buttonS' onclick='openURL(&quot;/?play=%s&quot;, 1)' style='color:#fff !important;'>Play Now</div></div>",
                       name ? name : "", pl, vt, creator ? creator : "", creator ? creator : "", descr ? descr : "undefined", name ? name : "", name ? name : "");
            // the featured video (config.fetMapVids / map_videolink) is a YouTube iframe in v10: no
            // embedded video natively, the box holds a link to the video instead (empty without one)
            sb_append(&out, "<div class='bigMFeatVid' style='display:inline-flex;align-items:center;justify-content:center;'>");
            const char *vl = row_str2(f, "map_videolink", NULL);
            append_video_link(&out, vl && *vl ? vl : fet_video(k_fet_map_vids, name), "bigMFeatVidLink", 1);
            sb_append(&out, "</div>");
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
            const char *name = row_str2(f, "mna", "mod_name"), *creator_raw = row_str2(f, "ctn", "creatorname");
            char vt[32], thumb[400] = "", creator_s[128] = "";
            // iiìîiîï (config-10.0.7.js:4255): the creator with its quotes stripped
            for (size_t i = 0, o = 0; creator_raw && creator_raw[i] && o + 1 < sizeof(creator_s); i++)
                if (creator_raw[i] != '\'' && creator_raw[i] != '"') creator_s[o++] = creator_raw[i], creator_s[o] = 0;
            const char *creator = creator_raw ? creator_s : NULL;
            comma_number(vt, sizeof(vt), row_num2(f, "vte", "mod_votes"));
            if (info_thumb(f, cJSON_GetObjectItemCaseSensitive(f, "minf") ? "minf" : "mod_info")) {
                char path[48];
                snprintf(path, sizeof(path), "/md%.0f/thumb.png", row_num2(f, "mid", "mod_id"));
                ua_url(thumb, sizeof(thumb), path);
            }
            sb_append(&out, "<div class='bigMFeatHold' style='background-image:linear-gradient(to right, black, rgba(0, 0, 0, 0.4)),url(");
            sb_append_esc(&out, thumb);
            sb_appendf(&out, ".png);'><div id='bigMFeatName' style='top:185px;'>%s</div><div id='bigMFeatVbar' style='top:260px;'>%s<span class='material-icons' style='margin-left:10px;font-size:33px;vertical-align:top;color:rgba(255,255,255,0.5);'>thumb_up</span><div style='color:#7d7d7d;margin-left:20px;margin-top:-12px;display:inline-block;vertical-align:middle;'>by <a style='color: #2196F3;'>%s</a></div></div><div id='bigMFeatPBtn' class='btnAnim' style='top:225px;width:232px' onclick='openMod(&quot;",
                       name ? name : "", vt, creator ? creator : "");
            sb_append_js_escape(&out, name ? name : "");
            sb_append(&out, "&quot;)'>Load Mod <span class='material-icons' style='font-size:50px;color:#fff;vertical-align:middle;margin-top:-16px;margin-left:5px'>move_to_inbox</span></div></div><div class='bigMFeatVid'>");
            // mod_videolink, else config's mod video (hub-10.0.7.js:6322-6327); "Missing Preview" with neither
            const char *mvl = row_str2(f, "mod_videolink", NULL);
            if (!mvl || !*mvl) mvl = fet_video(k_fet_mod_vids, name);
            StrBuf mv = {0};
            const int has_mv = mvl && *mvl && append_video_link(&mv, mvl, "bigMFeatVidLink", 1);
            char *mv_s = sb_finish(&mv);
            if (has_mv) sb_appendf(&out, "<div style='display:inline-flex;align-items:center;justify-content:center;width:100%%;height:100%%;'>%s</div>", mv_s ? mv_s : "");
            else sb_append(&out, "<div style='color:rgba(255,255,255,0.4);line-height:320px;'>Missing Preview</div>");
            free(mv_s);
            sb_append(&out, "</div>");
            sb_append(&out, "<div class='mapListCat' style='margin-bottom:10px;padding-bottom:10px;margin-top:20px;'>Featured Mods</div>");
        }
    }
    char *b = sb_finish(&body);
    sb_appendf(&out, "<div style='color:rgba(255,255,255,0.3)' id='modList'>%s</div>", b);
    free(b);
    return sb_finish(&out);
}

// windows[n].update(): mapsHolder / modsHolder.innerHTML = gen()
static void hub_players_update(void);

static void list_update(const int is_mods) {
    char *html = is_mods ? mods_gen() : maps_gen();
    set_html(is_mods ? "modsHolder" : "mapsHolder", html);
    free(html);
    if (!is_mods) hub_players_update(); // windows[0].update(): gen(), then updatePlayers()
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
    const AccountInfo *acc = g_hub.logged_in ? active_account() : NULL;
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
static void hub_render_listing(const cJSON *t);

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
        else if (!strcmp(g_hub.v, "profile") && g_hub.logged_in && active_account()) snprintf(g_hub.b, sizeof(g_hub.b), "%s", active_account()->name); // b = q
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

// getReadableTime(ms, withSec) (config-10.0.7.js:4311-4325): "Ny Nd Nh Nm ", and with withSec the seconds ("Ns", no
// trailing space): the market grace timer (hub-10.0.7.js:4372, 4832)
static void readable_time_s(char *out, const size_t n, const double ms, const int with_sec) {
    long long sec = ms > 0 ? (long long) floor(ms / 1000.0) : 0, m = sec / 60, h = m / 60;
    sec %= 60;
    m %= 60;
    long long d = h / 24;
    h %= 24;
    const long long y = d / 365;
    d %= 365;
    char yp[24] = "", dp[24] = "", hp[24] = "";
    if (y) snprintf(yp, sizeof(yp), "%lldy ", y);
    if (d) snprintf(dp, sizeof(dp), "%lldd ", d);
    if (h) snprintf(hp, sizeof(hp), "%lldh ", h);
    char sp[24] = "";
    if (with_sec) snprintf(sp, sizeof(sp), "%llds", sec);
    snprintf(out, n, "%s%s%s%lldm %s", yp, dp, hp, m, sp);
}
static void readable_time(char *out, const size_t n, const double ms) { readable_time_s(out, n, ms, 0); }

// the offers' and reports' age (config-10.0.7.js:4342-4361, used at hub-10.0.7.js:2665, 4185): the largest unit only
static void time_ago(char *out, const size_t n, const double ms) {
    const long long m = ms < 0 ? 0 : (long long) floor(ms / 60000.0), h = m / 60, d = h / 24;
    if (d) snprintf(out, n, "%lldd ago", d);
    else if (h) snprintf(out, n, "%lldh ago", h);
    else if (m) snprintf(out, n, "%lldm ago", m);
    else snprintf(out, n, "just now");
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

// The profile's Chat Ban (hub-10.0.7.js:2801-2813, 4700-4723): v10's moderation API on the brain (services/brain/v10_mods.c),
// GET /moderation/chat/:name for the label, POST /moderation/chat {player_name, ban} for the switch; staff only.
static void chat_ban_label(void) {
    const HtmlElem b = el("chatBanBtn");
    if (b) html_dom_set_text(b, g_hub.profile_chat_banned ? "Chat Unban" : "Chat Ban");
}

static void profile_chat_ban_got(void *user, const int status, const char *body, const size_t length) {
    (void) length;
    char *name = user;
    cJSON *j = status == 200 && body ? cJSON_Parse(body) : NULL;
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(j, "data"), "chat_banned");
    if (name && !strcmp(name, g_hub.profile_name) && cJSON_IsBool(v)) {
        g_hub.profile_chat_banned = cJSON_IsTrue(v);
        chat_ban_label();
    }
    cJSON_Delete(j);
    free(name);
}

static void moderation_url(char *url, const size_t n, const char *path) {
    char token[400];
    backend_url_encode(token, sizeof(token), account_session_token());
    snprintf(url, n, "%s%s?token=%s", backend_url(), path, token);
}

static void profile_chat_ban_fetch(void) {
    char enc[200], path[260], url[1024];
    backend_url_encode(enc, sizeof(enc), g_hub.profile_name);
    snprintf(path, sizeof(path), "/moderation/chat/%s", enc);
    moderation_url(url, sizeof(url), path);
    char *name = strdup(g_hub.profile_name);
    if (name && !backend_http_request("GET", url, NULL, profile_chat_ban_got, name)) free(name);
}

static void profile_chat_ban_set(void *user, const int status, const char *body, const size_t length) {
    (void) length;
    char *name = user;
    cJSON *j = body ? cJSON_Parse(body) : NULL;
    const cJSON *d = cJSON_GetObjectItemCaseSensitive(j, "data"), *err = cJSON_GetObjectItemCaseSensitive(j, "error");
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(d, "chat_banned");
    if (status == 200 && cJSON_IsBool(v)) {
        if (name && !strcmp(name, g_hub.profile_name)) {
            g_hub.profile_chat_banned = cJSON_IsTrue(v);
            chat_ban_label();
        }
    } else {
        const char *msg = cJSON_IsString(err) ? err->valuestring : cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(err, "message"));
        io_po(0, msg && *msg ? msg : "Failed to update chat ban");
    }
    cJSON_Delete(j);
    free(name);
}

// The profile's earned badges (hub-10.0.7.js:2778): player_badge "1,5,9" split and mapped to numbers; kn_brain's socket
// profile carries them as the "badges" array (player_badges)
static int profile_has_badge(const cJSON *t, const int index) {
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(t, "badges"), *b;
    cJSON_ArrayForEach(b, list) if (cJSON_IsNumber(b) && (int) b->valuedouble == index) return 1;
    const char *csv = jstr(t, "player_badge");
    for (const char *p = csv; p && *p;) {
        char *end;
        const long v = strtol(p, &end, 10);
        if (end != p && v == index) return 1;
        p = strchr(p, ',');
        if (p) p++;
    }
    return 0;
}

// iîííïìï[i].url: textures/badges/badge_<id>[_<tex>].png on the asset host (hub-10.0.7.js:460-463; badge_texture)
static void badge_url(const int index, char *out, const size_t n) {
    char tex[96];
    html_dom_asset_url(badge_texture(index, tex, sizeof(tex)), out, n);
}

// creator.toLowerCase().split(", ").includes(name.toLowerCase()) (config-10.0.7.js:4758, classes-10.0.7.js:4124)
static int creator_has(const char *creator, const char *name) {
    if (!creator || !name || !*name) return 0;
    const size_t len = strlen(name);
    for (const char *p = creator;;) {
        const char *end = strstr(p, ", ");
        const size_t n = end ? (size_t) (end - p) : strlen(p);
        if (n == len && !strncasecmp(p, name, len)) return 1;
        if (!end) return 0;
        p = end + 2;
    }
}

// config.statTabs (config-10.0.7.js:9493)
static const char *const k_stat_tabs[] = {"General Stats", "Killstreaks", "Other Stats", "Class Stats", "Raids", "Match History"};
#define STAT_TAB_COUNT ((int) (sizeof(k_stat_tabs) / sizeof(k_stat_tabs[0])))

// statTabB4 (hub-10.0.7.js:2903-2948): t.raidStats summed, then one card per raid map (iíììiìi, in key order)
static void append_raid_stats(StrBuf *sb, const cJSON *raids) {
    static const struct { int id; const char *name; } maps[] = {{18, "Tortuga"}, {29, "Laboratory"}, {31, "Soul Sanctum"}, {35, "Khepri"}, {43, "Bastion"}};
    double c = 0, a = 0, ai = 0, boss = 0, deaths = 0;
    const cJSON *r;
    cJSON_ArrayForEach(r, raids) {
        c += jnum(r, "completions");
        a += jnum(r, "attempts");
        ai += jnum(r, "ai_kills");
        boss += jnum(r, "boss_kills");
        deaths += jnum(r, "deaths");
    }
    char n1[32], n2[32], v[48];
    js_num(n1, sizeof(n1), c);
    js_num(n2, sizeof(n2), a);
    sb_appendf(sb, "<div class='pSt'>Totals<strong>%s/%s (%d%%)</strong></div>", n1, n2, a > 0 ? (int) round(c / a * 100) : 0);
    comma_fixed(v, sizeof(v), ai, -1);
    sb_appendf(sb, "<div class='pSt'>AI Kills<strong>%s</strong></div>", v);
    comma_fixed(v, sizeof(v), boss, -1);
    sb_appendf(sb, "<div class='pSt'>Boss Kills<strong>%s</strong></div>", v);
    comma_fixed(v, sizeof(v), deaths, -1);
    sb_appendf(sb, "<div class='pSt'>Deaths<strong>%s</strong></div><div style='height:10px'></div>", v);
    for (size_t m = 0; m < sizeof(maps) / sizeof(maps[0]); m++) {
        const cJSON *row = NULL;
        cJSON_ArrayForEach(r, raids) if ((int) jnum(r, "map_id") == maps[m].id) { row = r; break; }
        const double mc = row ? jnum(row, "completions") : 0, ma = row ? jnum(row, "attempts") : 0;
        const cJSON *bt = row ? cJSON_GetObjectItemCaseSensitive(row, "best_time") : NULL;
        char best[24] = "--:--";
        if (cJSON_IsNumber(bt)) snprintf(best, sizeof(best), "%.0f:%02d", floor(bt->valuedouble / 60000), (int) floor(fmod(bt->valuedouble, 60000) / 1000));
        js_num(n1, sizeof(n1), mc);
        js_num(n2, sizeof(n2), ma);
        sb_appendf(sb, "<div class='pSt' style='text-align:center;padding:10px;height:100%%;width: 180px;'><div class='pStTitle'>%s</div><div class='pSt' style='margin:2px 0;'>Completions<strong>%s/%s (%d%%)</strong></div><div class='pSt' style='margin:2px 0;'>Best Time<strong>%s</strong></div>",
                   maps[m].name, n1, n2, ma > 0 ? (int) round(mc / ma * 100) : 0, best);
        comma_fixed(v, sizeof(v), row ? jnum(row, "ai_kills") : 0, -1);
        sb_appendf(sb, "<div class='pSt' style='margin:2px 0;'>AI Kills<strong>%s</strong></div>", v);
        comma_fixed(v, sizeof(v), row ? jnum(row, "boss_kills") : 0, -1);
        sb_appendf(sb, "<div class='pSt' style='margin:2px 0;'>Boss Kills<strong>%s</strong></div>", v);
        comma_fixed(v, sizeof(v), row ? jnum(row, "deaths") : 0, -1);
        sb_appendf(sb, "<div class='pSt' style='margin:2px 0;'>Deaths<strong>%s</strong></div></div>", v);
    }
}

// The Match History tab: v10 mounts its MatchHistory component (hub-10.0.7.js:1105-1520, svelte-1irqx9a) on
// #matchHistoryContainer with {data: t.matchHistory, playerName, hasPagination: false, showRegion: true}
// (switchStat, :2376-2392); an entry opens into its scoreboard, sorted by a header's column.
#define PM_MAX 64
static struct {
    cJSON *data;
    unsigned char open[PM_MAX];
    signed char sort[PM_MAX]; // 0 the default (score, descending); else column + 1, negative ascending
} g_pm;

static double pm_entry_num(const cJSON *e, const int col) {
    static const char *const keys[] = {"player_name", "score", "kills", "assists", "deaths", "kd", "objective_score", "damage_done"};
    if (col == 5) {
        const double k = jnum(e, "kills"), d = jnum(e, "deaths");
        return d > 0 ? k / d : k;
    }
    return jnum(e, keys[col]);
}
static int g_pm_col, g_pm_desc;
static int pm_cmp(const void *a, const void *b) {
    const cJSON *x = *(const cJSON *const *) a, *y = *(const cJSON *const *) b;
    int c;
    if (!g_pm_col) {
        const char *nx = jstr(x, "player_name"), *ny = jstr(y, "player_name");
        c = strcmp(nx ? nx : "", ny ? ny : "");
    } else {
        const double dx = pm_entry_num(x, g_pm_col), dy = pm_entry_num(y, g_pm_col);
        c = dx < dy ? -1 : dx > dy ? 1 : 0;
    }
    return g_pm_desc ? -c : c;
}

static void hub_match_history_render(void) {
    if (!el("matchHistoryContainer")) return;
    StrBuf sb = {0};
    const char *me = g_hub.profile_name;
    sb_append(&sb, "<div class=\"match-history svelte-1irqx9a\">");
    if (!cJSON_GetArraySize(g_pm.data)) sb_append(&sb, "<div class=\"no-matches svelte-1irqx9a\">No match history available.</div>");
    else {
        sb_append(&sb, "<div class=\"match-list svelte-1irqx9a\">");
        int index = 0;
        const cJSON *m;
        cJSON_ArrayForEach(m, g_pm.data) {
            const cJSON *entries = cJSON_GetObjectItemCaseSensitive(m, "historyEntries"), *mine = NULL, *e;
            cJSON_ArrayForEach(e, entries) {
                const char *pn = jstr(e, "player_name");
                if (pn && !strcmp(pn, me)) mine = e;
            }
            const int team = mine ? (int) jnum(mine, "team") : 0; // Alpha 1, Bravo 2
            const double sa = jnum(m, "score_alpha"), sbv = jnum(m, "score_bravo");
            const int win = mine && ((team == 1 && sa > sbv) || (team == 2 && sbv > sa));
            char score[48];
            if (team == 2) snprintf(score, sizeof(score), "%.0f - %.0f", sbv, sa);
            else snprintf(score, sizeof(score), "%.0f - %.0f", sa, sbv);
            const int rank = (int) jnum(m, "rank");
            char date[16] = "";
            const time_t tt = (time_t) (jnum(m, "date") / 1000.0);
            struct tm tm;
#ifdef _WIN32
            localtime_s(&tm, &tt);
#else
            localtime_r(&tt, &tm);
#endif
            strftime(date, sizeof(date), "%d/%m/%Y", &tm);
            sb_appendf(&sb, "<div class=\"match-entry %s svelte-1irqx9a\" onclick=\"playTick(0.5);knPmMatch(%d)\"><div class=\"match-summary svelte-1irqx9a\">"
                            "<img alt=\"map thumbnail\" class=\"map-thumbnail svelte-1irqx9a\" src=\"./img/maps/map_%d.png\"> <div class=\"match-info svelte-1irqx9a\">"
                            "<div class=\"rank-display svelte-1irqx9a\"><img class=\"rank-icon svelte-1irqx9a\" src=\"./%s\"> <span class=\"rank svelte-1irqx9a\">%s</span> "
                            "<span class=\"match-date svelte-1irqx9a\">%s</span> <span class=\"match-region svelte-1irqx9a\">",
                       win ? "win" : "loss", index, (int) jnum(m, "map"), ranked_rank_icon(rank), ranked_rank_name(rank), date);
            sb_append_esc(&sb, jstr(m, "region") ? jstr(m, "region") : "");
            sb_appendf(&sb, "</span></div> <div class=\"stats svelte-1irqx9a\"><span class=\"stat svelte-1irqx9a\">KDA: %.0f/%.0f/%.0f</span> <span class=\"stat svelte-1irqx9a\">Score: %.0f</span> ",
                       jnum(mine ? mine : m, "kills"), jnum(mine ? mine : m, "deaths"), jnum(mine ? mine : m, "assists"), jnum(mine ? mine : m, "score"));
            const int change = (int) jnum(m, "mmr_change");
            if (change) sb_appendf(&sb, "<span class=\"stat %s svelte-1irqx9a\">%s%d Rating</span>", change > 0 ? "positive" : "negative", change > 0 ? "+" : "", change);
            else sb_append(&sb, "<!---->");
            sb_appendf(&sb, "</div></div> <div class=\"match-result svelte-1irqx9a\"><div class=\"result svelte-1irqx9a\">%s</div> <div class=\"score svelte-1irqx9a\">%s</div></div></div> ",
                       win ? "WIN" : "LOSS", score);
            if (index < PM_MAX && g_pm.open[index] && cJSON_GetArraySize(entries)) {
                static const char *const labels[] = {"Player", "Score", "Kills", "Assists", "Deaths", "K/D", "Objective", "Damage"};
                const int col = g_pm.sort[index] ? abs(g_pm.sort[index]) - 1 : 1, desc = g_pm.sort[index] ? g_pm.sort[index] > 0 : 1;
                sb_append(&sb, "<div class=\"match-details svelte-1irqx9a\"><table class=\"table-container svelte-1irqx9a\"><thead><tr class=\"table-row svelte-1irqx9a\">");
                for (int c = 0; c < 8; c++)
                    sb_appendf(&sb, "<td class=\"table-header sortable svelte-1irqx9a\" onclick=\"playTick(0.5);knPmSort(%d,%d)\">%s %s</td>", index, c, labels[c],
                               c == col ? (desc ? "\xe2\x86\x93" : "\xe2\x86\x91") : "");
                sb_append(&sb, "</tr></thead><tbody>");
                const int n = cJSON_GetArraySize(entries);
                const cJSON **sorted = calloc((size_t) n, sizeof(*sorted));
                int k = 0;
                cJSON_ArrayForEach(e, entries) if (sorted) sorted[k++] = e;
                g_pm_col = col;
                g_pm_desc = desc;
                if (sorted) qsort(sorted, (size_t) k, sizeof(*sorted), pm_cmp);
                for (int j = 0; sorted && j < k; j++) {
                    const cJSON *x = sorted[j];
                    const int xr = (int) jnum(x, "rank");
                    const double kills = jnum(x, "kills"), deaths = jnum(x, "deaths");
                    const char *pn = jstr(x, "player_name");
                    sb_appendf(&sb, "<tr class=\"table-row %ssvelte-1irqx9a\"><td class=\"table-cell svelte-1irqx9a\"><div class=\"player-info svelte-1irqx9a\"><img class=\"player-rank-icon svelte-1irqx9a\" src=\"./%s\"> <span class=\"%s svelte-1irqx9a\">",
                               pn && !strcmp(pn, me) ? "current-player " : "", ranked_rank_icon(xr), (int) jnum(x, "team") == 2 ? "blue-team" : "red-team");
                    sb_append_esc(&sb, pn ? pn : "");
                    sb_appendf(&sb, "</span></div></td><td class=\"table-cell svelte-1irqx9a\">%.0f</td><td class=\"table-cell svelte-1irqx9a\">%.0f</td><td class=\"table-cell svelte-1irqx9a\">%.0f</td>"
                                    "<td class=\"table-cell svelte-1irqx9a\">%.0f</td><td class=\"table-cell svelte-1irqx9a\">%.2f</td><td class=\"table-cell svelte-1irqx9a\">%.0f</td><td class=\"table-cell svelte-1irqx9a\">%.0f</td></tr>",
                               jnum(x, "score"), kills, jnum(x, "assists"), deaths, deaths > 0 ? kills / deaths : kills, jnum(x, "objective_score"), jnum(x, "damage_done"));
                }
                free(sorted);
                sb_append(&sb, "</tbody></table></div>");
            } else sb_append(&sb, "<!---->");
            sb_append(&sb, "</div>");
            index++;
        }
        sb_append(&sb, "</div> <!---->"); // hasPagination false: no pagination
    }
    sb_append(&sb, "</div>");
    char *html = sb_finish(&sb);
    set_html("matchHistoryContainer", html);
    free(html);
}

static int query_param(const char *query, const char *name, char *out, size_t n);
static int h_report_popup(void *user, const HtmlEvent *ev, const HtmlArg *args, int argc);
static int h_gift_popup(void *user, const HtmlEvent *ev, const HtmlArg *args, int argc);
static int h_trade_popup(void *user, const HtmlEvent *ev, const HtmlArg *args, int argc);
static int g_auto_gift_pending, g_auto_trade_pending; // iiiîíïî / iiîiïii: run at the login (hub_auto_after_login)

static void auto_gift(void) {
    char v[64];
    if (!query_param(g_hub.query, "autoGift", v, sizeof(v))) return;
    const HtmlArg a = {HTML_ARG_STR, 0, v};
    h_gift_popup(NULL, NULL, v[0] ? &a : NULL, v[0] ? 1 : 0); // giftPopup(searchParams.get("autoGift"))
}

static void profile_auto_actions(const cJSON *t) {
    (void) t;
    char v[64];
    const AccountInfo *acc = g_hub.logged_in ? active_account() : NULL;
    if (query_param(g_hub.query, "autoReport", v, sizeof(v)) && v[0]) h_report_popup(NULL, NULL, NULL, 0);
    if (query_param(g_hub.query, "autoGift", v, sizeof(v)) && v[0]) {
        if (acc) auto_gift();
        else g_auto_gift_pending = 1;
    }
    if (query_param(g_hub.query, "autoTrade", v, sizeof(v))) {
        if (acc && acc->level >= TRADE_MIN_LVL && g_hub.profile_level >= TRADE_MIN_LVL) h_trade_popup(NULL, NULL, NULL, 0);
        else g_auto_trade_pending = 1;
    }
}

static void hub_render_profile(const cJSON *t, const cJSON *extra) {
    set_display("profileHolder", "block");
    const char *name = jstr(t, "player_name");
    snprintf(g_hub.profile_name, sizeof(g_hub.profile_name), "%s", name ? name : "");
    g_hub.profile_id = (int) jnum(t, "player_id");
    g_hub.profile_featured = (int) jnum(t, "player_featured");
    g_hub.profile_hack = (int) jnum(t, "player_hack");
    g_hub.profile_creator = (int) jnum(t, "creator_badge");
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
    g_hub.profile_level = k;
    const AccountInfo *acc = g_hub.logged_in ? active_account() : NULL;
    // #socialFlag: "flag-icon flag-icon-" + countries[flg][1] when stats.flg > -1; otherwise the class stays as it was
    const char *flag = jnum(stats, "flg") > -1 && cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(stats, "flg")) ? account_flag_code((int) jnum(stats, "flg")) : NULL;
    if (flag && el("socialFlag")) {
        char cls[48];
        snprintf(cls, sizeof(cls), "flag-icon flag-icon-%s", flag);
        html_dom_set_attr(el("socialFlag"), "class", cls);
    }
    const HtmlElem pic = el("socialPic");
    if (pic) { // the picture with showPfp on and a premium player, else the class icon (Kamile's: "a")
        char icon[64];
        if (name && !strcmp(name, "Kamile")) snprintf(icon, sizeof(icon), "./textures/classes/icon_a.png");
        else snprintf(icon, sizeof(icon), "./textures/classes/icon_%d.png", (int) jnum(stats, "c"));
        set_pfp(pic, jnum(t, "player_id"), g_hub.show_pfp && jnum(t, "player_premium") > 0, icon);
    }
    const char *clan = jstr(t, "player_clan");
    const int premium = jnum(t, "player_premium") > 0;
    const char *alias = jstr(t, "player_alias");
    char num[64], num2[64];
    StrBuf sb = {0};
    // the presence dot (social_eb.c's st)
    sb_appendf(&sb, "<div class='onStatus onStatProf' style='background:%s'></div>", jnum(t, "st") ? "#27ab32" : "#7f7f7f");
    sb_append(&sb, "<div class='leftTopHolder'>");
    // v10's creator badge (creator_badge, #993299 verified: v10:109621) shares the legacy partner mark's icon
    if (jnum(t, "creator_badge") > 0)
        sb_append(&sb, "<i class='material-icons' style='margin-right:-2px;font-size:32px;color:#993299;vertical-align:middle;margin-top:-14px;'>verified</i>");
    if (premium) sb_append(&sb, "<i class='material-icons' style='margin-right:-2px;font-size:32px;color:#FBC02D;vertical-align:middle;margin-top:-14px;'>beenhere</i>");
    if (jnum(t, "player_featured")) sb_append(&sb, "<i class='material-icons' style='margin-right:1px;font-size:32px;color:#40C4FF;vertical-align:middle;margin-top:-14px;'>check_circle</i>");
    // the selected badge (stats.bdg) when the profile has earned it (hub-10.0.7.js:2779-2780)
    const cJSON *bdg = cJSON_GetObjectItemCaseSensitive(stats, "bdg");
    if (cJSON_IsNumber(bdg) && bdg->valuedouble >= 0 && badge_get((int) bdg->valuedouble) && profile_has_badge(t, (int) bdg->valuedouble)) {
        char url[512];
        badge_url((int) bdg->valuedouble, url, sizeof(url));
        sb_append(&sb, "<img draggable='false' src='");
        sb_append_esc(&sb, url);
        sb_append(&sb, "' class='customBadge'>");
    }
    // t.rankedRank (hub-10.0.7.js:2780): the rank's icon, img/ranked/ranks/rank_<tier>.svg (index-D_a-cl5k.js
    // iiïìîïï, unknown ranks the unranked icon), drawn only for a truthy rank. Box Office's profile row carries it
    // (services/brain/profiles.c), krunker.io's comes as krunker.io sends it.
    const cJSON *rr = cJSON_GetObjectItemCaseSensitive(t, "rankedRank");
    const double rank = cJSON_IsNumber(rr) ? rr->valuedouble : cJSON_IsString(rr) && rr->valuestring[0] ? atof(rr->valuestring) : 0;
    if (rank != 0 || (cJSON_IsString(rr) && rr->valuestring[0])) {
        char url[256];
        html_dom_asset_url(ranked_rank_icon(rank == (int) rank ? (int) rank : 0), url, sizeof(url));
        sb_append(&sb, "<img draggable='false' src='");
        sb_append_esc(&sb, url);
        sb_append(&sb, "' class='playerProfileRank' alt='Rank'>");
    }
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
    sb_appendf(&sb, "</div><div class='leftMidHolder'>%s <span style='color:rgba(255,255,255,0.6)'>%s</span></div><div class='leftBottomHolder'>", I18N("social.since", "Since"), num);
    format_num(num, sizeof(num), g_hub.profile_followed);
    format_num(num2, sizeof(num2), g_hub.profile_following);
    sb_appendf(&sb, "<div class='follL' id='followerC' onclick='viewFollowers()'>%s<span class='follCount'>%s</span></div><div class='follL' id='followC' style='margin-left:10px;' onclick='viewFollowing()'>%s<span class='follCount'>%s</span></div></div>",
               num, I18N("generic.followers", "Followers"), num2, I18N("generic.following", "following"));
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
    sb_appendf(&sb, "<div class='bottomBtn %s' id='followBtn' onmouseover=\"SOUND.play('tick_0',0.1)\" onclick='followUser();'>%s</div>", not_following ? "follow" : "unfollow",
               not_following ? I18N("generic.follow", "Follow") : I18N("generic.unfollow", "Unfollow"));
    sb_appendf(&sb, "<div class='bottomBtn canVerify%s' id='verifyBtn' onmouseover=\"SOUND.play('tick_0',0.1)\" onclick='verifyPopup();'>%s</div>", acc && acc->is_dev ? "" : " hide", jnum(t, "player_featured") ? "Unverify" : "Verify");
    sb_appendf(&sb, "<div class='bottomBtn canBadge%s' id='badgeBtn' onmouseover=\"SOUND.play('tick_0',0.1)\" onclick='badgePopup();'>Give Badge</div>", acc && acc->is_dev ? "" : " hide");
    // Chat Ban (canChatBan: staff), hub-10.0.7.js:2801-2813; the label follows the ban in force (profile_chat_ban_fetch)
    const int chat_staff = acc && (acc->is_dev || acc->is_mod);
    sb_appendf(&sb, "<div class='bottomBtn canChatBan%s' id='chatBanBtn' onmouseover=\"SOUND.play('tick_0',0.1)\" onclick='chatBanUser();'>Chat Ban</div>", chat_staff ? "" : " hide");
    // KrunkNative: v10's creator badge has no setter in the v10 page; developers give it here ("crt")
    sb_appendf(&sb, "<div class='bottomBtn canVerify%s' id='creatorBtn' style='color:#993299;border-color:#993299' onmouseover=\"SOUND.play('tick_0',0.1)\" onclick='creatorPopup();'>%s</div>", acc && acc->is_dev ? "" : " hide",
               g_hub.profile_creator ? "Uncreator" : "Creator");
    char *html = sb_finish(&sb);
    set_html("rightProfile", html);
    free(html);
    g_hub.profile_chat_banned = 0;
    if (chat_staff && g_hub.profile_name[0]) {
        profile_chat_ban_fetch();
        // canViewReports: the profile's Reports tab (hub-10.0.7.js:2173-2175: ["grp", name] -> ["rps", rows])
        cJSON *f = cJSON_CreateArray();
        cJSON_AddItemToArray(f, cJSON_CreateString("grp"));
        cJSON_AddItemToArray(f, cJSON_CreateString(g_hub.profile_name));
        hub_send(f);
    }
    if (g_hub.profile_name[0] && acc && strcmp(acc->name, g_hub.profile_name) != 0) set_display("followBtn", "inline-block");
    if (acc && acc->level >= TRADE_MIN_LVL && k >= TRADE_MIN_LVL) set_display("tradeBtn", "inline-block");

    // pTabContent_about: Overall Stats with the four config.statTabs
    const double kills = jnum(t, "player_kills"), deaths = jnum(t, "player_deaths"), games = jnum(t, "player_games_played"), wins = jnum(t, "player_wins");
    const double lo = k > 1 ? pow(k / 0.03, 2) : 0, hi = pow((k + 1) / 0.03, 2);
    sb_appendf(&sb, "<div class='scrollItemNew'><span class='scrollItemHead'>%s</span><div class='material-icons toggleView' onclick='toggleView(this)'>expand_less</div><div>",
               I18N("social.profile.stats", "Overall Stats"));
    js_num(num, sizeof(num), (double) (long long) (score - lo) / (double) (long long) (hi - lo) * 100.0);
    sb_appendf(&sb, "<div class='xpBar'><div class='xpBarB' style='width:%s%%'></div><span class='xpBarV'>%lld / %lld</span></div>", num, (long long) (score - lo), (long long) (hi - lo));
    sb_append(&sb, "</div><div id='statHolder' style='margin-top: 0px;'><div class='bigMenTabs' style='margin-bottom: 5px;table-layout: fixed;width: calc(100% - 18px);margin-left: 9px;background-color: rgba(0, 0, 0, 0.5);'>");
    for (int i = 0; i < STAT_TAB_COUNT; ++i)
        sb_appendf(&sb, "<div id='statTab%d' class='bigMenTab%s' onclick='playSelect(0.1);switchStat(%d)' style='padding-left: 5px; padding-right: 5px; padding-top: 10px; padding-bottom: 5px; line-height: 35px;font-size: 14px;'onmouseenter='playTick();'>%s</div>",
                   i, i == 0 ? " bigMenTabA" : "", i, k_stat_tabs[i]);
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
    PST(I18N("generic.score", "Score"), v);
    comma_fixed(v, sizeof(v), score / (kills > 1 ? kills : 1), 2);
    PST("SPK", v);
    comma_fixed(v, sizeof(v), kills, -1);
    PST(I18N("generic.kills", "Kills"), v);
    comma_fixed(v, sizeof(v), deaths, -1);
    PST(I18N("generic.deaths", "Deaths"), v);
    comma_fixed(v, sizeof(v), kills / (deaths > 1 ? deaths : 1), 4);
    PST(I18N("generic.kdr", "KDR"), v);
    comma_fixed(v, sizeof(v), kills / (games > 1 ? games : 1), 4);
    PST("KPG", v);
    comma_fixed(v, sizeof(v), games, -1);
    PST(I18N("generic.games", "Games"), v);
    js_num(v, sizeof(v), round(wins / (games - wins > 1 ? games - wins : 1) * 1000.0) / 1000.0); // .round(3)
    PST("W/L", v);
    comma_fixed(v, sizeof(v), wins, -1);
    PST(I18N("generic.wins", "Wins"), v);
    comma_fixed(v, sizeof(v), games - wins > 0 ? games - wins : 0, -1);
    PST(I18N("generic.lose", "Losses"), v);
    // {i18n key, en text, stats field}
    static const char *const stat_keys[][3] = {{"stats.assists", "Assists", "ast"}, {"stats.melee", "Melee", "mk"}, {"stats.fistkills", "Beatdowns", "fk"},
                                              {"stats.thrownkills", "Bullseyes", "tmk"}, {"stats.headshots", "Headshots", "hs"},
                                              {"stats.legshots", "Legshots", "ls"}, {"stats.wallbangs", "Wallbangs", "wb"}, {"stats.shots", "Shots", "s"}};
    for (size_t i = 0; i < sizeof(stat_keys) / sizeof(stat_keys[0]); i++) {
        comma_fixed(v, sizeof(v), jnum(stats, stat_keys[i][2]), -1);
        PST(i18n_tr(stat_keys[i][0], stat_keys[i][1]), v);
    }
    const double shots = jnum(stats, "s"), hits = fmin(shots, jnum(stats, "h"));
    comma_fixed(v, sizeof(v), hits, -1);
    PST(I18N("stats.hits", "Hits"), v);
    comma_fixed(v, sizeof(v), shots - hits, -1);
    PST(I18N("stats.misses", "Misses"), v);
    comma_fixed(num, sizeof(num), round((shots ? hits / shots * 100.0 : 0.0) * 100.0) / 100.0, -1);
    snprintf(v, sizeof(v), "%s%%", num);
    PST(I18N("generic.accuracy", "Accuracy"), v);
    readable_time(v, sizeof(v), jnum(t, "player_timeplayed"));
    PST(I18N("generic.timep", "Time Played"), v);
    sb_append(&sb, "</div><div class='statTabB1 hide'>");
    static const char *const streak_keys[][3] = {{"stats.nukes", "Nukes", "n"}, {"stats.airdrops", "Airdrops", "ad"}, {"stats.airdrops.stolen", "Airdrops Stolen", "sad"},
                                                {"stats.slimers", "Slimers", "sl"}, {"stats.juggernauts", "Juggernauts", "jg"}};
    for (size_t i = 0; i < sizeof(streak_keys) / sizeof(streak_keys[0]); i++) {
        comma_fixed(v, sizeof(v), jnum(stats, streak_keys[i][2]), -1);
        PST(i18n_tr(streak_keys[i][0], streak_keys[i][1]), v);
    }
    comma_fixed(v, sizeof(v), jnum(stats, "kjg"), -1);
    sb_appendf(&sb, "<div class='pSt' style='font-size: 9.5px;padding-left: 0px;padding-right: 0px;width: 140px;'>%s<strong>%s</strong></div>", I18N("stats.juggernauts.killed", "Juggernauts Killed"), v);
    comma_fixed(v, sizeof(v), jnum(stats, "wm"), -1);
    PST(I18N("stats.warmachines", "War Machines"), v);
    sb_append(&sb, "</div><div class='statTabB2 hide'>");
    comma_fixed(v, sizeof(v), jnum(stats, "crc"), -1);
    PST(I18N("stats.crouches", "Crouches"), v);
    comma_fixed(v, sizeof(v), jnum(stats, "spry"), -1);
    PST(I18N("stats.sprays", "Sprays Placed"), v);
    sb_appendf(&sb, "<div class='pSt' style='padding-left: 9px;padding-right: 9px;width: 122px;'>%s<strong>%d/%d</strong></div>", I18N("stats.kpd", "Helpful Reports"),
               (int) jnum(t, "player_jobratingpositive"), (int) jnum(t, "player_jobrating"));
    comma_fixed(v, sizeof(v), jnum(t, "player_calls"), -1);
    sb_appendf(&sb, "<div class='pSt' title='%s'>%s<strong>%s</strong></div>", I18N("stats.kpd.total.tip", "Amount of times you called KPD"), I18N("stats.kpd.total", "Total Calls"), v);
    comma_fixed(v, sizeof(v), jnum(t, "player_taggedaccounts"), -1);
    sb_appendf(&sb, "<div class='pSt' title='%s' style='padding-left: 8px;padding-right: 8px;width: 124px;'>%s<strong>%s</strong></div>",
               I18N("stats.kpd.bans.tip", "Amount of users banned from calls you made to KPD"), I18N("stats.kpd.bans", "Bans from calls"), v);
#undef PST
    sb_append(&sb, "</div><div class='statTabB3 hide'>");
    for (size_t c = 0; c < rules_class_count(); c++) {
        const ClassConfig *cc = &rules_classes()[c];
        if (!cc->name || !cc->name[0] || cc->hide) continue; // classes[i].hide (hub-10.0.7.js:2895)
        char key[16];
        snprintf(key, sizeof(key), "c%zu", c);
        const double xp = jnum(stats, key);
        const double e2 = 0.03 * sqrt(xp > 0 ? xp : 0);
        int lvl = (int) floor(e2);
        const int prog = (int) round((e2 - lvl) * 100);
        const double clo = lvl > 1 ? pow(lvl / 0.03, 2) : 0, chi = pow((lvl + 1) / 0.03, 2);
        lvl = lvl + 1 > 1 ? lvl + 1 : 1;
        const int w = cc->loadout ? cc->loadout[0] : -1;
        const char *weapon = w >= 0 && w < rules_krunker_weapon_count() && rules_weapon(w) && rules_weapon(w)->name ? rules_weapon(w)->name : "";
        sb_appendf(&sb, "<div class='classCard'><img class='classImg' src='./textures/classes/icon_%zu.png'/><div class='className'>%s</div><div class='classWeap'>%s</div><div class='classXPBar'><div class='clsXPBarC' style='width:%d%%'></div><div class='clsXPBarV'>%lld / %lld</div></div><div class='classLvl'>Lvl %d</div></div>",
                   c, cc->name, weapon, prog, (long long) (xp - clo), (long long) (chi - clo), lvl);
    }
    sb_append(&sb, "</div><div class='statTabB4 hide'>");
    append_raid_stats(&sb, cJSON_GetObjectItemCaseSensitive(t, "raidStats"));
    sb_append(&sb, "</div><div class='statTabB5 hide'><div id='matchHistoryContainer'></div></div>");
    sb_append(&sb, "</div></div><div></div><div></div>");
    cJSON_Delete(g_pm.data); // the Match History tab's t.matchHistory (switchStat mounts it)
    g_pm.data = cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(t, "matchHistory"), 1);
    memset(g_pm.open, 0, sizeof(g_pm.open));
    memset(g_pm.sort, 0, sizeof(g_pm.sort));
    sb_appendf(&sb, "<div class='scrollItemNew'><span class='scrollItemHead'>%s</span><div class='material-icons toggleView' onclick='toggleView(this)'>expand_less</div><div style='margin-top:10px;margin-left: 8px;'><span style='color: rgba(255,255,255,0.4);'>Data Range</span><select class='rangeSelect' onchange='changeDateRange2(this.value)'><option value='0'>1 week</option><option value='1'>2 weeks</option><option value='2'>3 weeks</option><option value='3' selected>1 month</option><option value='4'>2 months</option><option value='5'>3 months</option><option value='6'>4 months</option></select></div><div class='graphHolder'><div class='graphHeader'><span class='graphHeaderTxt'>%s</span><span id='totalSalesHeader' class='graphHeaderTotal'>0</span></div><canvas id='profSales'></canvas></div></div><div></div>",
               I18N("social.profile.sales", "Sales"), I18N("social.profile.sales.market", "Market Sales"));
    html = sb_finish(&sb);
    set_html("pTabContent_about", html);
    free(html);
    set_html("pTabContent_activity", "");
    set_html("pTabContent_listings", "");
    // the Badges tab (hub-10.0.7.js:2966-2977): a card per earned badge in the table's order, its art, and the badge's
    // tooltip on hover (createBadgeTip)
    StrBuf bsb = {0};
    int nbadges = 0;
    sb_append(&bsb, "<div class='scrollItemNew'>");
    for (int i = 0; i < BADGE_COUNT; i++) {
        if (!profile_has_badge(t, i)) continue;
        char url[512];
        badge_url(i, url, sizeof(url));
        nbadges++;
        sb_appendf(&bsb, "<div class='badgeCard' data-index='%d' onmouseover='createBadgeTip(this)' onmouseout='cancelTip()'><img class='badgeImg' src='", i);
        sb_append_esc(&bsb, url);
        sb_append(&bsb, "'></div>");
    }
    if (!nbadges) sb_appendf(&bsb, "<div class='noneText'>%s</div>", I18N("social.profile.badges.none", "No Badges found for this user"));
    sb_append(&bsb, "</div>");
    char *badges_html = sb_finish(&bsb);
    set_html("pTabContent_badges", badges_html);
    free(badges_html);
    // creations: skins (store skins by this creator), games r, mods l, assets f
    const cJSON *games_rows = cJSON_GetArrayItem(extra, 4), *mods_rows = cJSON_GetArrayItem(extra, 5), *assets_rows = cJSON_GetArrayItem(extra, 7);
    int skins_n = 0;
    for (int i = 0; i < g_viewer_skin_count; i++)
        if (creator_has(g_viewer_skins[i].creator, g_hub.profile_name)) skins_n++;
    {
        char n0[16], n1[16], n2[16], n3[16], l0[64], l1[64], l2[64], l3[64];
        snprintf(n0, sizeof(n0), "%d", skins_n);
        snprintf(n1, sizeof(n1), "%d", cJSON_GetArraySize(games_rows));
        snprintf(n2, sizeof(n2), "%d", cJSON_GetArraySize(mods_rows));
        snprintf(n3, sizeof(n3), "%d", cJSON_GetArraySize(assets_rows));
        I18N_F(l0, "social.profile.skins", "Skins [{0}]", n0);
        I18N_F(l1, "social.profile.games", "Games [{0}]", n1);
        I18N_F(l2, "social.profile.mods", "Mods [{0}]", n2);
        I18N_F(l3, "social.profile.assets", "Assets [{0}]", n3);
        sb_appendf(&sb, "<div class='scrollItemNew'><div class=\"creationTabs\">\n\t\t\t\t<div class=\"creationTab creationTabA\" id=\"creationTab_skins\" onmouseenter=\"playTick();\" onclick=\"playSelect(0.1);openCreationTab('skins')\">%s</div>\n\t\t\t\t<div class=\"creationTab\" id=\"creationTab_games\" onmouseenter=\"playTick();\" onclick=\"playSelect(0.1);openCreationTab('games')\">%s</div>\n\t\t\t\t<div class=\"creationTab\" id=\"creationTab_mods\" onmouseenter=\"playTick();\" onclick=\"playSelect(0.1);openCreationTab('mods')\">%s</div>\n\t\t\t\t<div class=\"creationTab\" id=\"creationTab_assets\" onmouseenter=\"playTick();\" onclick=\"playSelect(0.1);openCreationTab('assets')\">%s</div>\n\t\t\t</div>",
                   l0, l1, l2, l3);
    }
    sb_append(&sb, "<div class='creationContent selected' id='creationContent_skins'>");
    if (skins_n) {
        for (int i = g_viewer_skin_count - 1; i >= 0; i--) { // reversed
            const ViewerSkin *sk = &g_viewer_skins[i];
            if (!creator_has(sk->creator, g_hub.profile_name)) continue;
            char path[256], url[256];
            viewer_skin_preview_path(sk, path, sizeof(path));
            html_dom_asset_url(path, url, sizeof(url));
            sb_appendf(&sb, "<div class='creationSkinItem'><div class='creationSkinImgH' onclick='viewItem(%d)'><img draggable='false' class='creationSkinImg' loading='lazy' src='%s'/></div><div class='creationSkinName'>", i, url);
            sb_append_esc(&sb, sk->name);
            sb_append(&sb, "</div></div>");
        }
    } else {
        sb_appendf(&sb, "<div class='noneText'>%s</div>", I18N("social.profile.skins.none", "No published Skins from this user"));
    }
    sb_append(&sb, "</div>");
    static const char *const heads[3] = {"games", "mods", "assets"};
    static const char *const nones[3][2] = {{"social.profile.games.none", "No published Games from this user"},
                                            {"social.profile.mods.none", "No published Mods from this user"},
                                            {"social.profile.assets.none", "No published Assets from this user"}};
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
                sb_append(&sb, "<div class='mapListThumbHolder' style='width:204px;height: 90px;'><img class='mapListThumb' loading='lazy' style='width:204px;height: 90px;' src='");
                if (info_thumb(r, "dat")) {
                    char path[48], url[400];
                    snprintf(path, sizeof(path), "/%.0f/thumb.png", jnum(r, "id"));
                    ua_url(url, sizeof(url), path);
                    sb_append_esc(&sb, url);
                } else {
                    sb_append(&sb, "./img/noimg.png");
                }
                sb_append(&sb, "' onerror='this.onerror=null;this.src=\"./img/noimg.png\";'/></div><div style='margin-top:1px;text-overflow: ellipsis;overflow: hidden;white-space: nowrap;'><a href='javascript:;'/>");
                sb_append_served(&sb, nm);
                sb_append(&sb, "</a></div></div>");
                continue;
            }
            const char *img = jstr(r, i == 0 ? "map_image" : "mod_image");
            const char *open = i == 0 ? "openMap" : "openMod";
            char votes[32];
            js_num(votes, sizeof(votes), row_num2(r, "vte", i == 0 ? "map_votes" : "mod_votes"));
            sb_append(&sb, "<div class='mapListThumbHolder' style='width:204px;height:90px;'><img draggable='false' class='mapListThumb' loading='lazy' style='width:204px;height: 90px;' src='");
            sb_creation_thumb(&sb, i == 1, row_num2(r, "mid", i == 0 ? "map_id" : "mod_id"),
                              info_thumb(r, i == 0 ? "map_info" : "mod_info") || (i == 1 && info_thumb(r, "minf")), img);
            sb_appendf(&sb, "' onclick='%s(&quot;", open);
            sb_append_js_escape(&sb, nm);
            sb_appendf(&sb, "&quot;)'/><div class='mapsVotes' style='top: 68px;'>%s<span class='material-icons' style='margin-left: 5px;color:rgba(255, 255, 255, 0.5);vertical-align: middle;margin-top: -3px;font-size: 18px;'>thumb_up</span></div></div><div style='margin-top:1px;text-overflow: ellipsis;overflow: hidden;white-space: nowrap;'><a href='javascript:;' onclick='%s(&quot;", votes, open);
            sb_append_js_escape(&sb, nm);
            sb_append(&sb, "&quot;)'/>");
            sb_append_served(&sb, nm);
            sb_append(&sb, "</a></div></div>"); // no creator line: the profile is the creator's (hub-10.0.7.js:3023, 3066)
        }
        if (!any) sb_appendf(&sb, "<div class='noneText'>%s</div>", i18n_tr(nones[i][0], nones[i][1]));
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
    profile_auto_actions(t);
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
    else if (!strcmp(a, "player_ranked_matches")) { js_num(n, sizeof(n), h); sb_appendf(sb, "%s <span class='lName'>matches</span>", n); } // v10 config:9535
    else if (!strcmp(a, "season_score")) sb_appendf(sb, "%s <span class='lName'>score</span>", n);
    else if (!strcmp(a, "season_kills")) { js_num(n, sizeof(n), h); sb_appendf(sb, "%s<span class='lName'> kills</span>", n); }
    else if (!strcmp(a, "season_wins")) { js_num(n, sizeof(n), h); sb_appendf(sb, "%s<span class='lName'> wins</span>", n); }
    else if (!strcmp(a, "player_timeplayed") || !strcmp(a, "season_timeplayed")) {
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
static void hub_map_opened(const char *name, int id); // the map page tools (below): the open map, its Leaderboard
static void hub_map_versions_show(void);

// getMapTapHTML(canEdit)
static void append_map_tabs(StrBuf *sb, const int can_edit) {
    sb_appendf(sb, "<div class='mapTabP%s' onclick='updateMapTab(0, %s)'>About</div><div class='mapTabP%s' onclick='updateMapTab(1, %s)'>Servers</div><div class='mapTabP%s' onclick='updateMapTab(2, %s)'>Leaderboard</div>",
               g_map_tab == 0 ? " activeTab" : "", can_edit ? "true" : "false", g_map_tab == 1 ? " activeTab" : "", can_edit ? "true" : "false",
               g_map_tab == 2 ? " activeTab" : "", can_edit ? "true" : "false");
    if (can_edit) sb_appendf(sb, "<div class='mapTabP%s' onclick='updateMapTab(3, true)'>Data</div>", g_map_tab == 3 ? " activeTab" : "");
    // v10.0.7's owner tab "Versions" (deob-10.0.7.js:107938-107945)
    if (can_edit) sb_appendf(sb, "<div class='mapTabP%s' onclick='updateMapTab(4, true)'>%s</div>", g_map_tab == 4 ? " activeTab" : "",
                             i18n_tr("creations.versions.tab", "Versions"));
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
    const AccountInfo *acc = g_hub.logged_in ? active_account() : NULL;
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
    sb_creation_thumb(&sb, is_mod, id, info_thumb(t, is_mod ? "mod_info" : "mi"), image);
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
        // t.vl: v10's YouTube embed (display block with vl); natively the link button, as the map page's
        StrBuf mvid = {0};
        const int has_mvid = append_video_link(&mvid, jstr(t, "vl"), "modVidLink", 0);
        char *mvid_s = sb_finish(&mvid);
        sb_appendf(&sb, "<div id='modVid' style='%s'>%s</div><div id='modDescr'>", has_mvid ? "display:block;text-align:center" : "display:none",
                   mvid_s ? mvid_s : "");
        free(mvid_s);
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
        // t.vl: v10's video embed; natively the YouTube link button (hidden without a video)
        StrBuf vid = {0};
        const int has_vid = append_video_link(&vid, jstr(t, "vl"), "mapVidLink", 0);
        char *vid_s = sb_finish(&vid);
        sb_appendf(&sb, "</div><div id='mapVid' style='%s'>%s</div><div id='mapDescr' style='padding-top:20px%s'>",
                   has_vid && !g_map_tab ? "text-align:center" : "display:none", vid_s ? vid_s : "", g_map_tab ? ";display:none" : "");
        free(vid_s);
        snprintf(key, sizeof(key), "%s_description", pre);
        const char *descr = jstr(t, key);
        if (descr && *descr) sb_append_served(&sb, descr);
        else sb_append(&sb, "No Description");
        const int ca = (int) jnum(t, "ca");
        get_date(date, sizeof(date), jstr(t, "map_initialdate"));
        comma_number(num, sizeof(num), pl);
        sb_appendf(&sb, "<div class='mapSubDesc'><div class='mapSubItm'><b style='color:#fff'>Category</b><br/>%s</div><div class='mapSubItm'><b style='color:#fff'>Plays</b><br/>%s</div><div class='mapSubItm'><b style='color:#fff'>Released</b><br/>%s</div><div class='mapSubItm'><b style='color:#fff'>Version</b><br/>v%s</div></div></div>",
                   k_map_cats[ca >= 0 && ca < MAP_CAT_COUNT ? ca : 0], num, date, ver);
        sb_appendf(&sb, "<div id='activeGames' style='display:%s'></div><div id='mapLeaders' style='display:%s;'><div style='font-size:16px;margin-bottom: 2px;text-align:center;color:#fff'>Not Set Yet!</div></div>",
                   g_map_tab == 1 ? "block" : "none", g_map_tab == 2 ? "block" : "none");
        if (can_edit) sb_appendf(&sb, "<div id='mapData' style='display:%s;margin-bottom:20px'><div style='font-size:16px;margin-bottom: 2px'><input type='text' id='searchDInput' placeholder='Search User' onkeyup='if (window.event.keyCode==13) window.searchPData();'><div id='searchDBtn' onclick='window.searchPData();'>Search</div><div id='deleteDBtn' onclick='window.deleteDataPop();'>Delete By Key</div></div><div id='dataList'></div></div>", g_map_tab == 3 ? "block" : "none");
        if (can_edit) sb_appendf(&sb, "<div id='mapVersions' style='display:%s;margin-bottom:20px'></div>", g_map_tab == 4 ? "block" : "none");
        sb_append(&sb, "<div class='button map buttonP' onclick='shareMapLink(this, &quot;");
        sb_append_js_escape(&sb, name);
        sb_append(&sb, "&quot;)'>Share</div><div class='button map buttonH' style='margin-left:8px' onclick='hostMap(&quot;");
        sb_append_js_escape(&sb, name);
        sb_append(&sb, "&quot;)'>Host</div><div class='button map buttonR' style='margin-left:8px' onclick='repPop(`Map`)'>Report</div>");
    }
    char *html = sb_finish(&sb);
    set_html(is_mod ? "modProfile" : "mapProfile", html);
    free(html);
    if (!is_mod) hub_map_opened(name, id);
}

static void hub_render_search(const cJSON *t) {
    char n[32];
    comma_number(n, sizeof(n), cJSON_GetArraySize(t));
    StrBuf sb = {0};
    sb_appendf(&sb, "<div class='scrollItem'>%s<div style='float:right'>%s</div></div>", I18N("social.search", "Search Results"), n);
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

// iïìììiì(post, single, fromProfile) (hub-10.0.7.js:4875-4944): text posts (a yt-<id> tag taken out), game / mod
// links and invites with their thumb.png, skin unboxings with the skin's picture, game results (typ 8: the mode's end
// stats table) and KR giveaways (typ 9: the KR, "Follow and Like to Enter", "ends in")
static const ViewerSkin *hub_skin(int index);
static const char *rarity_color(const ViewerSkin *s);
static int rarity_animate(const ViewerSkin *s);
static void skin_preview(char *out, size_t n, const ViewerSkin *skin);

// iïîïïíí(txt) (config-10.0.7.js): the first "yt-<11 characters>" removed
static void strip_yt_tag(char *t) {
    for (char *p = t; p && (p = strstr(p, "yt-")) != NULL; p++) {
        int ok = 1;
        for (int i = 0; i < 11 && ok; i++) ok = isalnum((unsigned char) p[3 + i]) || p[3 + i] == '_' || p[3 + i] == '-';
        if (!ok) continue;
        memmove(p, p + 14, strlen(p + 14) + 1);
        return;
    }
}

// typ 8's table (hub-10.0.7.js:4914-4936): lnk the mode (its endStats, else config.endStats), lnk2 the flat JSON
// rows, len values each (the first, sid, not shown); the poster's own name in white
static void append_game_results(StrBuf *sb, const cJSON *e) {
    static const char *const k_default[] = {"sid", "name", "score", "kills", "deaths", "reward", NULL}; // config-10.0.7.js:9067
    const char *lnk = jstr(e, "lnk"), *lnk2 = jstr(e, "lnk2"), *pln = jstr(e, "pln");
    const cJSON *lj = cJSON_GetObjectItemCaseSensitive(e, "lnk");
    const int mode = cJSON_IsNumber(lj) ? (int) lj->valuedouble : lnk ? atoi(lnk) : 0;
    const GameModeConfig *mc = mode_config(mode);
    sb_append(sb, "<table id='postGameRes'>");
    if (mc) {
        const char *const *stats = mc->end_stats ? mc->end_stats : k_default;
        int len = 0;
        while (stats[len]) len++;
        sb_append(sb, "<tr><th class='postResRo'>Rank</th>");
        for (int i = 1; i < len; i++) {
            char cap[48];
            snprintf(cap, sizeof(cap), "%s", stats[i]);
            cap[0] = (char) toupper((unsigned char) cap[0]);
            sb_appendf(sb, "<th class='postResRo'>%s</th>", cap);
        }
        sb_append(sb, "</tr>");
        cJSON *rows = lnk2 ? cJSON_Parse(lnk2) : NULL;
        const int n = cJSON_GetArraySize(rows);
        for (int i = 0, rank = 1; len > 0 && i < n; i += len, rank++) {
            sb_appendf(sb, "<tr style='%s'><td class='postResRoD' style='color:#fff'>%d.</td>", i + len >= n ? "border-bottom:none" : "", rank);
            for (int j = 1; j < len; j++) {
                // `v != null && v || 0`, then string concatenation: an array (the name column's [name],
                // deob-10.0.7.js:195291) is its elements joined with ","; the == against pln is loose, so ["bob"] is bob
                const cJSON *v = cJSON_GetArrayItem(rows, i + j);
                char val[160] = "0";
                if (cJSON_IsArray(v)) {
                    val[0] = 0;
                    const cJSON *el;
                    cJSON_ArrayForEach(el, v) {
                        char part[96] = "";
                        if (cJSON_IsString(el)) snprintf(part, sizeof(part), "%s", el->valuestring);
                        else if (cJSON_IsNumber(el)) js_num(part, sizeof(part), el->valuedouble);
                        else if (cJSON_IsBool(el)) snprintf(part, sizeof(part), "%s", cJSON_IsTrue(el) ? "true" : "false");
                        const size_t at = strlen(val);
                        snprintf(val + at, sizeof(val) - at, "%s%s", el != v->child ? "," : "", part);
                    }
                } else if (cJSON_IsString(v) && v->valuestring[0]) {
                    snprintf(val, sizeof(val), "%s", v->valuestring);
                } else if (cJSON_IsNumber(v) && v->valuedouble != 0) {
                    js_num(val, sizeof(val), v->valuedouble);
                } else if (cJSON_IsTrue(v)) {
                    snprintf(val, sizeof(val), "true");
                }
                const int me = pln && !strcmp(val, pln);
                sb_appendf(sb, "<td class='postResRoD' style='%s'>", me ? "color:#fff" : "");
                // v10 writes every value as HTML; the reward column is endForm's markup ("<span
                // style='color:#F8C55C'>+N</span> KR", deob-10.0.7.js:18317; the game's own MatchResult view keeps only
                // that column as HTML, :107016-107019). KrunkNative draws that markup and escapes everything else.
                int kr = 0;
                char tail[8] = "";
                if (!strcmp(stats[j], "reward") && sscanf(val, "<span style='color:#F8C55C'>+%d</span> K%2s", &kr, tail) == 2 && !strcmp(tail, "R") &&
                    kr > 0) {
                    char exact[96];
                    snprintf(exact, sizeof(exact), "<span style='color:#F8C55C'>+%d</span> KR", kr);
                    if (!strcmp(exact, val)) sb_append(sb, exact);
                    else sb_append_esc(sb, val);
                } else {
                    sb_append_esc(sb, val);
                }
                sb_append(sb, "</td>");
            }
            sb_append(sb, "</tr>");
        }
        cJSON_Delete(rows);
    }
    sb_append(sb, "</table>");
}

static void append_post(StrBuf *sb, const cJSON *e, const int single, const int from_profile) {
    const int typ = (int) jnum(e, "typ");
    char ago[64] = "Just Now";
    if (jnum(e, "tim") > 0) {
        readable_time(ago, sizeof(ago) - 4, jnum(e, "tim"));
        strcat(ago, " ago");
    }
    char *txt = strdup(jstr(e, "txt") ? jstr(e, "txt") : "");
    if (txt) strip_yt_tag(txt);
    char flw[32];
    comma_number(flw, sizeof(flw), jnum(e, "flw"));
    if (single) sb_append(sb, "<div></div><div ><div class='postHead'><img class='postPFP' onerror='this.onerror=null;this.src=\"./img/player_pfp.png\";'src='");
    else sb_appendf(sb, "<div></div><div class='socPost%s'><div class='postHead'><img class='postPFP' onerror='this.onerror=null;this.src=\"./img/player_pfp.png\";'src='", from_profile ? " fromProfile" : "");
    sb_pfp(sb, jnum(e, "pli"), jnum(e, "prm") > 0, "./img/player_pfp.png");
    sb_append(sb, "' /><div class='postInfo'><div>");
    append_post_author(sb, e);
    sb_append(sb, "<span class='postTime'>");
    if (typ == 9) { // a giveaway's time is when it ends
        char ends[64] = "0m";
        if (jnum(e, "exp")) readable_time(ends, sizeof(ends), jnum(e, "exp"));
        sb_appendf(sb, "<span style='color:rgba(255,255,255,0.6)'>ends in %s</span>", ends);
    } else {
        sb_append(sb, ago);
    }
    sb_appendf(sb, "</span></div><span class='postSubInfo'>%s Followers</span></div><div class='postContent'>", flw);
    const char *lnk = jstr(e, "lnk"), *lnk2 = jstr(e, "lnk2");
    // the post's picture (hub-10.0.7.js:4883-4910, 4944): a game's or mod's thumb.png (lnk / lnk2 as parseInt), an
    // unboxed skin's thumbnail with its name over it
    const cJSON *lj = cJSON_GetObjectItemCaseSensitive(e, "lnk"), *l2j = cJSON_GetObjectItemCaseSensitive(e, "lnk2");
    const long long lnk_n = cJSON_IsNumber(lj) ? (long long) lj->valuedouble : lnk ? atoll(lnk) : 0;
    const long long lnk2_n = cJSON_IsNumber(l2j) ? (long long) l2j->valuedouble : lnk2 ? atoll(lnk2) : 0;
    char img[400] = "", path[48];
    const ViewerSkin *unboxed = typ == 5 ? hub_skin((int) lnk_n) : NULL;
    if ((typ == 2 || typ == 3) && lnk_n > 0) {
        snprintf(path, sizeof(path), "/m%s%lld/thumb.png", typ == 3 ? "d" : "", lnk_n);
        ua_url(img, sizeof(img), path);
    } else if (typ == 7 && lnk2_n > 0) {
        snprintf(path, sizeof(path), "/m%lld/thumb.png", lnk2_n);
        ua_url(img, sizeof(img), path);
    } else if (unboxed) {
        skin_preview(img, sizeof(img), unboxed);
    }
    if (img[0]) {
        sb_append(sb, "<img class='postIMG' onclick='");
        if (typ == 2 || typ == 3) {
            sb_append(sb, typ == 2 ? "playMap(&quot;" : "playMod(&quot;");
            if (lnk2) sb_append_js_escape(sb, lnk2);
            else sb_append(sb, "null");
            sb_append(sb, "&quot;)");
        } else if (unboxed) {
            sb_appendf(sb, "viewItem(%lld)", lnk_n);
        }
        sb_append(sb, "' onerror='this.onerror=null;this.src=\"./img/noimg.png\";' src='");
        sb_append_esc(sb, img);
        sb_append(sb, "'/>");
        if (unboxed) {
            const char *col = rarity_color(unboxed);
            sb_appendf(sb, "<div class='imgTextO %s' style='color:%s'>", rarity_animate(unboxed) ? "rainbowText" : "", !strcmp(col, "#292929") ? "#fff" : col);
            sb_append_esc(sb, unboxed->name);
            sb_append(sb, "</div>");
        }
    }
    sb_append(sb, "<div style='overflow-x:hidden;color:rgba(255,255,255,0.8);user-select: text;'>");
    if (typ == 9) {
        char kr[32];
        comma_number(kr, sizeof(kr), jnum(e, "fds"));
        sb_appendf(sb, "<div class='krGivPost'>%s <span style='color:#FBC02D'>KR</span></div>", kr);
    }
    if (typ != 8 && txt) sb_append_served(sb, txt);
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
    } else if (unboxed) {
        sb_append(sb, "Just unboxed a ");
        sb_append_esc(sb, unboxed->name);
        sb_append(sb, "!");
    } else if (typ == 8) {
        append_game_results(sb, e);
        if (txt && txt[0]) {
            sb_append(sb, "<div style='overflow-x:hidden;margin-top:10px;color:rgba(255,255,255,0.7)'>");
            sb_append_served(sb, txt);
            sb_append(sb, "</div>");
        }
    }
    sb_append(sb, "</div></div></div>");
    if (typ == 9 && !single) { // instaFollo(pln, this, 1)
        const char *pln = jstr(e, "pln");
        sb_append(sb, "<div class='krGivFol'><div class='follBtnGiv' onclick='instaFollo(&quot;");
        sb_append_esc(sb, pln ? pln : "");
        sb_append(sb, "&quot;,this,1)'>Follow</div> and Like to Enter</div>");
    }
    if (!single) append_post_footer(sb, e, 0);
    sb_append(sb, "</div>");
    free(txt);
}

static void hub_render_feed(const cJSON *t) {
    const AccountInfo *acc = g_hub.logged_in ? active_account() : NULL;
    if (acc) {
        set_pfp(el("postProf"), acc->id, acc->premium > 0, "./img/player_pfp.png");
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
        append_post(&sb, post, 0, 0);
        if (i++ % 3 == 0) sb_appendf(&sb, "<div id=\"innerDiv%d\" class=\"innerContent\"></div>", ++ads); // the in-feed ad slots (empty: no ads)
    }
    char *html = sb_finish(&sb);
    set_html("postHolder", html);
    free(html);
}

// ------------------------------------------------------------------ brain: market, item sales, clan (social.js io_0)


#define SALE_GRACE_MS g_econ.sale_grace // config.saleGrace
#define SALE_MAX ((int) g_econ.sale_max) // config.saleMax

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

// v10's getPrices (items/prices: _36377 replaces the estimate list when the exchange opens): KrunkNative's
// kn_brain answers ["r","market","prices"] with this economy's own sale prices ({ski, est}: the mean of the
// item's completed market sales, services/brain/economy.c). They come first; the snapshot above covers the
// rest; an item in neither is v10's "Unknown".
static int *g_server_prices;

void social_set_estimates(const cJSON *rows) {
    if (!g_server_prices) g_server_prices = calloc((size_t) g_viewer_skin_count, sizeof(int));
    if (!g_server_prices) return;
    memset(g_server_prices, 0, (size_t) g_viewer_skin_count * sizeof(int));
    const cJSON *it;
    cJSON_ArrayForEach(it, rows) {
        const cJSON *ski = cJSON_GetObjectItemCaseSensitive(it, "ski"), *est = cJSON_GetObjectItemCaseSensitive(it, "est");
        if (!cJSON_IsNumber(ski) || !cJSON_IsNumber(est)) continue;
        const int k = (int) ski->valuedouble;
        if (k >= 0 && k < g_viewer_skin_count && est->valuedouble > 0) g_server_prices[k] = (int) fmin(est->valuedouble, 2e9);
    }
}

static int estimate_value(const int skin) {
    if (g_server_prices && skin >= 0 && skin < g_viewer_skin_count && g_server_prices[skin] > 0) return g_server_prices[skin];
    return skin >= 0 && (size_t) skin < sizeof(k_prices) / sizeof(k_prices[0]) ? k_prices[skin] : 0;
}

int social_estimate_value(const int skin) { return estimate_value(skin); }


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

// iiííììì(skin, weapons, i18n) (config-10.0.7.js:5160, 5166): the item's type line, i18n.get of its type's key, else
// its weapon's name
static const char *const k_type_key[19] = {
    NULL, "social.market.hat", "social.market.body", "social.market.melee", "social.market.spray", "social.market.dye", "social.market.waist",
    "social.market.face", "social.market.shoe", "social.market.pet", "social.market.collectible", "social.market.wrist", "social.market.charm",
    "social.market.ticket", "social.market.back", "social.market.head", "social.market.playerCard", "social.market.emoji", "social.market.paint",
};
static const char *const k_type_label[19] = {
    "", "Hat Item", "Body Item", "Melee Item", "Spray Item", "Dye Item", "Waist Item", "Face Item", "Shoe Item", "Pet Item",
    "Collectible Item", "Wrist Item", "Charm Item", "Spin Ticket Item", "Back Item", "Head Item", "Player Card", "Emoji", "Paint",
};
static const char *skin_type_label(const ViewerSkin *s) {
    if (s->type >= 1 && s->type <= 18) return I18N(k_type_key[s->type], k_type_label[s->type]);
    return s->weapon >= 1 && s->weapon <= rules_krunker_weapon_count() && rules_weapon(s->weapon - 1) ? rules_weapon(s->weapon - 1)->name : "";
}

// store.skins[index] as assets/skins.json has it (ui_loadout.c): the fields the viewer table leaves out (blackM, limT,
// limited, itemShop, noSale, opensea, lore, event, streamer, text, keyW, drop, ranked, challenge, ...)
static const cJSON *skin_field(const int index, const char *key) {
    return cJSON_GetObjectItemCaseSensitive(ui_loadout_skin_json(index), key);
}
static int skin_true(const int index, const char *key) { // JavaScript truthiness
    const cJSON *v = skin_field(index, key);
    return v && !cJSON_IsFalse(v) && !cJSON_IsNull(v) && !(cJSON_IsNumber(v) && v->valuedouble == 0) && !(cJSON_IsString(v) && !v->valuestring[0]);
}
static const char *skin_str(const int index, const char *key) {
    const char *s = cJSON_GetStringValue(skin_field(index, key));
    return s && s[0] ? s : NULL;
}

// the cards' badges (hub-10.0.7.js:3208, 3251, 3334): an animated item's loop (import_export for an rAnimT bob), a
// sound's volume_up. The profile's Listings test neither video nor projection (:4135).
static void append_card_badges(StrBuf *sb, const ViewerSkin *s, const int listings) {
    if ((s->flags & VS_HAS_ANIM) || s->puls_t || s->frames || s->mov_t || s->r_anim || (s->flags & VS_RGB) || (s->flags2 & VS2_SHADER) ||
        (!listings && (s->flags2 & (VS2_VIDEO | VS2_PROJECTION))))
        sb_appendf(sb, "<div class='material-icons isAnim'>%s</div>", s->r_anim_t ? "import_export" : "loop");
    if (s->flags2 & VS2_SND) sb_append(sb, "<div class='material-icons hasSnd'>volume_up</div>");
}

// the card picture's class (hub-10.0.7.js:3208): marketImg, + D (emoji, dye), P (player card and up), S (shoe and
// up), C (any other type)
static const char *market_img_class(const ViewerSkin *s) {
    if (!s->type) return "marketImg";
    return s->type == 17 ? "marketImgD" : s->type >= 16 ? "marketImgP" : s->type >= 8 ? "marketImgS" : s->type == 5 ? "marketImgD" : "marketImgC";
}

// the corner label (hub-10.0.7.js:4808, 3451, 3513): ??? (Black Market), the limT tag in its store.tags colour, Vaulted
// (another season than the current one, or a limited noSpin item outside the shop), Limited. pos: the item sales
// page's placement, written first for ??? / limT and last for Limited, as v10 does.
static void append_item_label(StrBuf *sb, const ViewerSkin *s, const int index, const char *pos) {
    const char *lim_t = skin_str(index, "limT");
    if (skin_true(index, "blackM")) {
        sb_appendf(sb, "<div class='itemLabel' style='%sbackground-color:#00ffff'>?\?\?</div>", pos);
    } else if (lim_t) {
        sb_appendf(sb, "<div class='itemLabel' style='%sbackground:%s'>", pos, ui_loadout_tag_color(lim_t));
        sb_append_esc(sb, lim_t);
        sb_append(sb, "</div>");
    } else if (s->seas != g_viewer_current_season || (skin_true(index, "limited") && (s->flags & VS_NO_SPIN) && !skin_true(index, "itemShop"))) {
        sb_appendf(sb, "<div class='itemLabel'%s%s%s>Vaulted</div>", pos[0] ? " style='" : "", pos, pos[0] ? "'" : "");
    } else if (skin_true(index, "limited") || (s->flags & VS_BLOCKED)) {
        sb_appendf(sb, "<div class='itemLabel' style='background-color:#faa332;%s'>Limited</div>", pos);
    }
}

// iiìîíïî(wear, i18n) (config-10.0.7.js:4255-4263, the tiers :7052-7068): the wear's name, NULL past the last tier
static const char *wear_name(const double wear) {
    static const double max[] = {0.1, 0.3, 0.6, 1};
    static const char *const keys[] = {"wear_flawless", "wear_tested", "wear_trashed", "wear_wrecked"};
    for (int i = 0; i < 4; i++)
        if (wear < max[i]) return i18n_t(keys[i]);
    return NULL;
}

// A row's item metadata, its m when that is a JSON string (hub-10.0.7.js:3325-3333, 4124-4134; KrunkNative's brain
// sends a painted copy's as an object, read the same way). The market rows outlive the reply frame: copied flat.
typedef struct CardMeta {
    char name[64];
    double wear, serial, seed;
    unsigned char has_wear, has_serial, has_seed;
} CardMeta;

static void card_meta(const cJSON *r, CardMeta *cm) {
    memset(cm, 0, sizeof(*cm));
    const cJSON *m = cJSON_GetObjectItemCaseSensitive(r, "m");
    cJSON *parsed = cJSON_IsString(m) ? cJSON_Parse(m->valuestring) : NULL;
    const cJSON *o = parsed ? parsed : m;
    if (cJSON_IsObject(o)) {
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, "name"));
        const cJSON *wear = cJSON_GetObjectItemCaseSensitive(o, "wear"), *serial = cJSON_GetObjectItemCaseSensitive(o, "serial");
        const cJSON *seed = cJSON_GetObjectItemCaseSensitive(o, "seed");
        snprintf(cm->name, sizeof(cm->name), "%s", name ? name : "");
        if (cJSON_IsNumber(wear) && wear_name(wear->valuedouble)) cm->has_wear = 1, cm->wear = wear->valuedouble;
        if (cJSON_IsNumber(serial)) cm->has_serial = 1, cm->serial = serial->valuedouble;
        if (cJSON_IsNumber(seed)) cm->has_seed = 1, cm->seed = seed->valuedouble;
    }
    cJSON_Delete(parsed);
}

// iiìîiîï (config-10.0.7.js:4255): quotes dropped (KrunkNative: and < > & escaped)
static void sb_append_unquoted(StrBuf *sb, const char *s) {
    for (; s && *s; s++) {
        if (*s == '"' || *s == '\'') continue;
        if (*s == '<') sb_append(sb, "&lt;");
        else if (*s == '>') sb_append(sb, "&gt;");
        else if (*s == '&') sb_append(sb, "&amp;");
        else sb_appendf(sb, "%c", *s);
    }
}

// a card's data-nametag / data-wear / data-serial for its tooltip, and (seed) iïîîíiï's data-seed (config-10.0.7.js:4828)
static void append_meta_attrs(StrBuf *sb, const CardMeta *cm, const double skinid, const int seed) {
    if (cm->name[0]) {
        sb_append(sb, " data-nametag='");
        sb_append_unquoted(sb, cm->name);
        sb_append(sb, "'");
    }
    if (cm->has_wear) sb_appendf(sb, " data-wear='%s' data-wear-float='%.4f'", wear_name(cm->wear), cm->wear);
    if (cm->has_serial) sb_appendf(sb, " data-serial='%.15g'", cm->serial);
    if (!seed) return;
    const int32_t v = skin_copy_seed((int64_t) skinid, cm->has_seed, cm->seed);
    if (v) sb_appendf(sb, " data-seed='%d'", (int) v);
}

// the card's name, then its nametag in quotes and its wear / serial line (hub-10.0.7.js:3334, 4135)
static void append_card_name(StrBuf *sb, const ViewerSkin *s, const CardMeta *cm) {
    sb_append_esc(sb, s->name);
    if (cm->name[0]) {
        sb_append(sb, "<div class='itemOwn' style='font-style:italic;color:#ffdd57'>\"");
        sb_append_unquoted(sb, cm->name);
        sb_append(sb, "\"</div>");
    }
    if (cm->has_wear || cm->has_serial) {
        sb_appendf(sb, "<div class='itemOwn' style='color:rgba(255,255,255,0.5)'>%s", cm->has_wear ? wear_name(cm->wear) : "");
        if (cm->has_serial) sb_appendf(sb, "<span class='serialTag'>#%.15g</span>", cm->serial);
        sb_append(sb, "</div>");
    }
}

static int query_param(const char *query, const char *name, char *out, size_t n);
// iîîîíïì (hub-10.0.7.js:1518): the hub's URL has ?dev: the cards' "ID:" line
static int hub_dev(void) {
    char v[8];
    return query_param(g_hub.query, "dev", v, sizeof(v));
}

// A spray's animation, v10's iìïîìiî(skin, name) (config-10.0.7.js:4870): the sheet steps through the store item's
// `frames` in 1 + frameT / 1000 s. The frames come from the skin table (store.skins frames / frameT, the viewer
// table's .frames / .frame_t, gen_viewer_store.js). The list collects each sheet length it drew, and its <style> has
// one "@keyframes <name><frames>" per length, shifting the sheet by frames * the list's width (hub-10.0.7.js:2706
// offers 60, 3264 / 3353 / 4149 market cards 200, 3432 / 3529 / 4852 the sales page and popups 220, 4026 / 5518
// the trade windows 60).
typedef struct SprayAnims {
    const char *name; // the animation's name: "sprayAnm", "sprayAnmOf", "sprayPopAnm"
    int n, frames[32];
} SprayAnims;
static SprayAnims *g_spray_anims; // the list being drawn (append_skin_img), NULL for none

static void spray_anim_style(StrBuf *sb, const ViewerSkin *s, const char *name) {
    if (s->frames) sb_appendf(sb, "animation: %s%d %.15gs steps(%d) infinite;", name, s->frames, 1 + s->frame_t / 1000.0, s->frames);
}

static void spray_anims_add(SprayAnims *a, const ViewerSkin *s) {
    if (!a || s->type != 4 || !s->frames) return;
    for (int i = 0; i < a->n; i++)
        if (a->frames[i] == s->frames) return;
    if (a->n < (int) (sizeof(a->frames) / sizeof(a->frames[0]))) a->frames[a->n++] = s->frames;
}

// the list's "<style>@keyframes <name><frames> {100%{background-position:-<frames * width>px}}...</style>", "" for none
static void spray_anims_keyframes(StrBuf *sb, const SprayAnims *a, const int width) {
    if (!a || !a->n) return;
    sb_append(sb, "<style>");
    for (int i = 0; i < a->n; i++) sb_appendf(sb, "@keyframes %s%d {100%%{background-position:-%dpx}}", a->name, a->frames[i], a->frames[i] * width);
    sb_append(sb, "</style>");
}

static void append_skin_img(StrBuf *sb, const ViewerSkin *s, const char *preview, const char *spray_cls, const char *img_cls) {
    if (s->type == 4) {
        sb_appendf(sb, "<div class='%s' style='background: url(&quot;%s&quot;);", spray_cls, preview);
        if (g_spray_anims) {
            spray_anim_style(sb, s, g_spray_anims->name);
            spray_anims_add(g_spray_anims, s);
        }
        sb_append(sb, "'></div>");
    } else {
        sb_appendf(sb, "<img draggable='false' class='%s%s' src='%s'/>", img_cls, (s->flags & VS_RGB) ? " rgbHue" : "", preview);
    }
}

// the open black-market popup's purchase key (KrunkNative: the pbm op's idempotency key, one per popup)
static char g_pbm_key[48];

// store.skins entries with `illicit` / blackM: the black market, econ_black_market() (economy_client.c, shared with
// the v10 popup; generated from assets/skins.json), each with its store.checkReq rule.
typedef EconBlackMarketItem IllicitSkin;

static const IllicitSkin *illicit_skin(const int skin) { return econ_black_market_find(skin); }

// account.skins.some(ind == id): the account's skins (data[10], {ind, cnt})
static int illicit_owns(const int skin, void *user) {
    const AccountInfo *acc = user;
    const cJSON *skins = acc && acc->data ? cJSON_GetArrayItem(acc->data, 10) : NULL, *e;
    cJSON_ArrayForEach(e, skins) {
        const cJSON *ind = cJSON_GetObjectItemCaseSensitive(e, "ind");
        if (cJSON_IsNumber(ind) && ind->valueint == skin) return 1;
    }
    return 0;
}

// store.checkReq(item, acc) (deob-10.0.7.js:103566-103580): econ_black_market_req_met with the account's values.
// The hub has no peak rank read: a rank item is unmet here (the market's Black Market tab reads it).
static int illicit_req_ok(const IllicitSkin *s, const AccountInfo *acc) {
    if (s->req == ECON_REQ_NONE) return 1;
    if (!acc) return 0;
    const EconReqAccount ra = {
        .create_ms = acc->create_ms, .funds = acc->funds, .trades = acc->trades, .job_rating_pos = acc->job_rating_pos,
        .kills = acc->kills, .time_played = acc->time_played, .followers = acc->followers, .stats = acc->stats, .owns = illicit_owns,
        .owns_user = (void *) acc,
    };
    return econ_black_market_req_met(s, &ra);
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
    CardMeta meta;    // m: the copy's nametag / wear / serial / seed
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
    card_meta(r, &m.meta);
    return m;
}

static int g_market_no_sort; // io_fr: the filtered market keeps the server's order
// The rows of the last market list (remove_card / inventory_count / quickSell find a card by its uid).
static MarketRow *g_market_rows;
static int g_market_row_count;
static char g_market_sub[16];
// #marketList's cards (market / inventory / sales): g_market_rows indices in list order; a
// virtual grid (ui_vgrid.h) writes the ones near the scroll position. v7.1.4 writes every row
// into one innerHTML (a staff inventory is all 6448 store items).
static int *g_market_view, g_market_view_n;
static VGrid g_market_grid;
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
    const AccountInfo *acc = g_hub.logged_in ? active_account() : NULL;
    if (!acc) return;
    char kr[32];
    comma_number(kr, sizeof(kr), funds);
    StrBuf sb = {0};
    sb_append_esc(&sb, acc->name);
    sb_appendf(&sb, "<div style='height:4px'></div><div id='profileKR' style='cursor:pointer' title='KR History' onclick='krHistory()'>%s<span style='color:#fff'> KR</span></div>", kr);
    char *html = sb_finish(&sb);
    set_html("profileName", html);
    free(html);
    account_set_funds(funds); // acc.funds: the gift / trade / market affordability checks
}

// The Trades tab's offers (viewOffer reads them back): the economy section below.
typedef struct HubOffer {
    int trid, we;            // we: 0 incoming, 1 outgoing, 2 history
    char name[64];
    int give[64], ngive, take[64], ntake; // skin ids, one per item (UTILS.splitIds)
    double kr_give, kr_take; // KrunkNative: KR in the offer, from this side's point of view
    char *message;
} HubOffer;
static void offers_clear(void);
static HubOffer *g_offers;
static int g_offer_count;

// Trade History pages (hub-10.0.7.js:2690-2702, 3844-3890: getHistory pages, a Load More row while the server has more).
// The socket's trade page carries the whole history (economy.c, 500 rows); the hub shows it a page at a time, the page
// of kn_brain's GET /trades/history (30).
#define TRADE_HISTORY_PAGE 30
static cJSON *g_trades_cache; // the last trade page (loadMoreTradeHistory draws it again)
static int g_his_page = TRADE_HISTORY_PAGE, g_his_keep;

static int cmp_offer_ids(const void *pa, const void *pb);

// iíîììîì's onclick strings (hub-10.0.7.js:2661-2665): text between &quot;s in a single-quoted attribute; the
// backslash doubled as v10 does for the name, and (KrunkNative) a quote escaped so a message with one still opens
static void sb_offer_arg(StrBuf *b, const char *v) {
    for (const char *c = v; c && *c; c++) {
        if (*c == '\\') sb_append(b, "\\\\");
        else if (!strncmp(c, "&quot;", 6)) {
            sb_append(b, "\\&quot;");
            c += 5;
        } else if (*c == '\'') sb_append(b, "&#39;");
        else sb_appendf(b, "%c", *c);
    }
}

// iìíiïií(items, currencies, sprays, most) (hub-10.0.7.js:2605-2645): one side of an offer row
static void offer_row_side(StrBuf *b, const int *ids, const int n, const int most, const double kr) {
    for (int i = 0; i < n; i++) {
        const ViewerSkin *pa = hub_skin(ids[i]);
        if (!pa || (pa->flags & VS_FREE) || skin_true(ids[i], "free")) continue; // item && !item.free
        char prev[256];
        skin_preview(prev, sizeof(prev), pa);
        sb_appendf(b, "<div class='oItemHolder%s'style='border: 2px solid %s;'data-index='%d' onmouseover='createTip(this)' onmouseout='cancelTip()'>",
                   g_hub.rarity_anim && rarity_animate(pa) ? " rainbowBorder" : "", rarity_color(pa), ids[i]);
        append_skin_img(b, pa, prev, "oItemSprayImg", "oItemImg");
        sb_append(b, "</div>");
    }
    for (int i = 0; i < (most + 4) / 5 * 5 - n; i++) sb_append(b, "<div class='oItemHolder' style='border: 2px solid rgba(0,0,0,0);'><div draggable='false' class='oItemImg'></div></div>");
    if (kr > 0) { // csell / cbuy [{cid: 1, amt}]: kn_brain's krsell / krbuy
        char v[32];
        format_num(v, sizeof(v), kr);
        sb_appendf(b, "<div class='oCurrencyLine'><span class='oCurrencyItem'>+%s KR</span></div>", v);
    }
}

static void offer_card_remove(int trid);

static void hub_render_trades(StrBuf *out, const cJSON *t, int *shown) {
    const AccountInfo *acc = g_hub.logged_in ? active_account() : NULL;
    if (t != g_trades_cache) {
        cJSON_Delete(g_trades_cache);
        g_trades_cache = cJSON_Duplicate(t, 1);
    }
    if (!g_his_keep) g_his_page = TRADE_HISTORY_PAGE;
    g_his_keep = 0;
    int his_more = 0;
    if (!acc || acc->level < TRADE_MIN_LVL) {
        sb_appendf(out, "Must be level %d", TRADE_MIN_LVL);
        *shown = -1; // marketList = O, no "none" text
        return;
    }
    *shown = 1;
    SprayAnims anims = {.name = "sprayAnmOf"};
    g_spray_anims = &anims;
    StrBuf pe[3] = {{0}, {0}, {0}};
    int fe[3] = {0, 0, 0};
    offers_clear(); // the rows viewOffer reads back
    g_offers = calloc((size_t) (cJSON_GetArraySize(cJSON_GetArrayItem(t, 0)) + cJSON_GetArraySize(cJSON_GetArrayItem(t, 1)) + 1), sizeof(HubOffer));
    for (int ge = 0; ge < 2; ge++) {
        const cJSON *ue = cJSON_GetArrayItem(t, ge);
        const cJSON *o;
        cJSON_ArrayForEach(o, ue) {
            const int he = (int) jnum(o, "pl") == acc->id;
            const int we = ge == 1 ? 2 : he ? 1 : 0;
            const char *give = jstr(o, he ? "sksell" : "skbuy"), *take = jstr(o, he ? "skbuy" : "sksell");
            const char *name = jstr(o, he ? "buyer" : "player");
            if (!name || !*name) continue;
            if (we == 2 && fe[2] >= g_his_page) {
                his_more = 1;
                continue;
            }
            // UTILS.splitIds, sorted as iiîíïií sorts them (rarity, season, index)
            int ids[2][64], counts[2] = {0, 0};
            const char *lists[2] = {give, take};
            const cJSON *copies[2] = {cJSON_GetObjectItemCaseSensitive(o, he ? "copies_s" : "copies_r"),
                                      cJSON_GetObjectItemCaseSensitive(o, he ? "copies_r" : "copies_s")};
            for (int k = 0; k < 2; k++) {
                for (const char *q = lists[k]; q && *q && counts[k] < 64;) {
                    if (*q != ',') ids[k][counts[k]++] = atoi(q);
                    q = strchr(q, ',');
                    if (q) q++;
                }
                const cJSON *cp;
                cJSON_ArrayForEach(cp, copies[k]) if (counts[k] < 64 && cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(cp, "i")))
                    ids[k][counts[k]++] = (int) jnum(cp, "i");
                qsort(ids[k], (size_t) counts[k], sizeof(int), cmp_offer_ids);
            }
            const int be = counts[0] > counts[1] ? counts[0] : counts[1];
            fe[we]++;
            StrBuf *b = &pe[we];
            const int trid = (int) jnum(o, "trid");
            const double kr_give = jnum(o, he ? "krsell" : "krbuy"), kr_take = jnum(o, he ? "krbuy" : "krsell");
            const char *m = jstr(o, "message");
            if (g_offers) {
                HubOffer *of = &g_offers[g_offer_count++];
                of->trid = trid;
                of->we = we;
                snprintf(of->name, sizeof(of->name), "%s", name);
                memcpy(of->give, ids[0], sizeof(of->give));
                memcpy(of->take, ids[1], sizeof(of->take));
                of->ngive = counts[0];
                of->ntake = counts[1];
                of->kr_give = kr_give;
                of->kr_take = kr_take;
                of->message = m && *m ? strdup(m) : NULL;
            }
            // iíîììîì(offer, we, acc) (hub-10.0.7.js:2646-2675)
            sb_appendf(b, "<div class='offerHolder' style='%s' id='of_%d' onclick='viewOffer(%d,%d,&quot;",
                       we == 0 && counts[1] == 0 ? "background-color:#ffb2b2" : we == 0 && counts[0] == 0 ? "background-color:#C4F399" : "", trid, trid, we);
            char *ename = NULL;
            {
                StrBuf e = {0};
                sb_append_esc(&e, name);
                ename = sb_finish(&e);
            }
            sb_offer_arg(b, ename);
            for (int k = 0; k < 2; k++) {
                sb_append(b, "&quot;,&quot;");
                for (int i = 0; i < counts[k]; i++) sb_appendf(b, i ? ",%d" : "%d", ids[k][i]);
            }
            sb_append(b, "&quot;");
            if (m && *m) {
                sb_append(b, ",&quot;");
                sb_offer_arg(b, m); // html-escaped by kn_brain
                sb_append(b, "&quot;");
            }
            sb_append(b, ")'>");
            if (we != 2) sb_appendf(b, "<div class='material-icons offerCancel' onclick='fastDeclOffer(%d)'>clear</div>", trid);
            char ago[48] = "just now";
            const double dte = jnum(o, "dte"); // kn_brain: epoch ms (sent / closed)
            if (dte > 0) time_ago(ago, sizeof(ago), now_epoch_ms() - dte);
            sb_appendf(b, "<div class='offerDate'%s>%s</div><div class='offerFrom'>%sOffer %s <span id='oProf' onclick='openLink(&quot;/social.html?p=profile&q=", we == 2 ? " style='right:11px;'" : "", ago,
                       m && *m && !he ? "<span class='material-icons offerMsg'>markunread</span>" : "", he ? "to" : "from");
            sb_append(b, ename);
            sb_append(b, "&quot;, &quot;_blank&quot;)'>");
            sb_append(b, ename);
            free(ename);
            sb_append(b, "</span></div><div class='offerContents'>");
            offer_row_side(b, ids[0], counts[0], be, kr_give);
            sb_append(b, "</div><div class='oSwapHolder'><div class='material-icons oSwapIcon'>swap_horiz</div></div><div class='offerContents'>");
            offer_row_side(b, ids[1], counts[1], be, kr_take);
            sb_append(b, "</div></div>");
        }
    }
    static const char *const ids[3] = {"in", "out", "his"};
    static const char *const heads[3][2] = {{"social.trades.in.head", "Incoming Offers ({0})"}, {"social.trades.out.head", "Outgoing Offers ({0})"},
                                            {"social.trades.history.head", "Trade History ({0})"}};
    g_spray_anims = NULL;
    spray_anims_keyframes(out, &anims, 60);
    // iiîiïií(trades, acc, more) (hub-10.0.7.js:2677-2714): the three columns side by side, no tabs
    for (int i = 0; i < 3; i++) {
        char count[64], head[160], decline[128] = "";
        snprintf(count, sizeof(count), "<span%s style='color:#9f9f9f'>%d</span>", i == 2 ? " id='hisCnt'" : "", fe[i]);
        i18n_trf(head, sizeof(head), heads[i][0], heads[i][1], 1, I18N_ARGV(count));
        if (i == 0) snprintf(decline, sizeof(decline), "<div class='declineAll' onclick='declineAllTrades();'>%s</div>", I18N("social.trades.decline", "Decline All"));
        sb_appendf(out, "<div class='offers scrollItem' id='o_%s'><div style='margin-bottom:10px;margin-top:-4px;color:#9f9f9f'>%s%s</div>", ids[i], head, decline);
        char *body = sb_finish(&pe[i]);
        sb_append(out, body[0] ? body : "<div style='color: rgba(0,0,0,0.6);font-size: 17px;'>None</div>");
        if (i == 2 && his_more && body[0]) sb_append(out, "<div class='offerHolder loadMoreHis' onclick='loadMoreTradeHistory()' style='text-align: center;'>Load More</div>");
        free(body);
        sb_append(out, "</div>");
    }
}

// The sold-out market card's showPopup string argument (hub-10.0.7.js:3337): iiíiììi's quotes dropped, a backslash
// doubled (the inline handler's string escape)
static void sb_popup_arg(StrBuf *sb, const char *v) {
    for (; v && *v; v++) {
        if (*v == '"' || *v == '\'') continue;
        if (*v == '\\') sb_append(sb, "\\\\");
        else if (*v == '<') sb_append(sb, "&lt;");
        else if (*v == '&') sb_append(sb, "&amp;");
        else sb_appendf(sb, "%c", *v);
    }
}

// The cards' Preview / Info pair (hub-10.0.7.js:3256, 3342): Info, a vertical separator, Preview
static void append_info_preview(StrBuf *sb, const int index) {
    sb_appendf(sb, "<div><a class='cardAction' style='display: inline-block;' onmouseenter='SOUND.play(`tick_0`,0.1)' onclick='openSales(%d)'>Info</a><div class='cardActionSepVert'></div>"
                   "<a class='cardAction' style='display: inline-block;' onmouseenter='SOUND.play(`tick_0`,0.1)' onclick='viewItem(%d)'>Preview</a></div>", index, index);
}

// One market / inventory / sales card (hub-10.0.7.js:3312-3346): row v of g_market_rows
static SprayAnims g_market_anims = {.name = "sprayAnm"};
static int g_market_ro; // iïiíììî, io_0's 4th argument: the list is read-only (no Purchase / List, estimate, price)

static void market_card(StrBuf *sb, const int pos, void *user) {
    (void) user;
    if (pos < 0 || pos >= g_market_view_n) return;
    const MarketRow *m = &g_market_rows[g_market_view[pos]];
    const ViewerSkin *pe = hub_skin(m->skin);
    if (!pe) return;
    g_spray_anims = &g_market_anims;
    const char *sub = g_market_sub;
    const int inventory = !strcmp(sub, "inventory"), sales = !strcmp(sub, "sales");
    const AccountInfo *acc = g_hub.logged_in ? active_account() : NULL;
    char prev[256], v[32];
    skin_preview(prev, sizeof(prev), pe);
    const char *col = rarity_color(pe), *label = skin_type_label(pe);
    sb_appendf(sb, "<div class='marketCard%s' id='itemCard%s_%d' style='color:%s;border:5px solid %s' onmouseenter='SOUND.play(`tick_0`,0.1)' data-index='%d'",
               g_hub.rarity_anim && rarity_animate(pe) ? " rainbow" : "", sub, m->uid, col, col, m->skin);
    append_meta_attrs(sb, &m->meta, m->skin_id, 1);
    sb_append(sb, " onmouseover='createTip(this)' onmouseout='cancelTip()'>");
    append_card_name(sb, pe, &m->meta);
    sb_appendf(sb, "<div class='itemOwn'>%s</div>", label);
    if (hub_dev()) sb_appendf(sb, "<div class='itemOwn'>ID: %d</div>", m->skin);
    append_skin_img(sb, pe, prev, "sprayMarketImg", market_img_class(pe));
    if (inventory && !g_market_ro) {
        // iiiïiìì(skinindex): the estimate, "Unknown" when the price list has none
        const int est = estimate_value(m->skin);
        comma_number(v, sizeof(v), est);
        sb_appendf(sb, "<div class='cardEst'>~ %s%s</div><div class='cardCnt'>x <span id='invCnt%d' style='color:#fff'>%d</span></div>", est ? v : "Unknown",
                   est ? " <span style='color:#fff'>KR</span>" : "", m->uid, m->count);
    }
    if (!inventory && !g_market_ro) {
        comma_number(v, sizeof(v), m->funds);
        if (m->funds) sb_appendf(sb, "<div class='marketPrice'>%s<span style='color:#fff'> KR</span></div>", v);
        else sb_append(sb, "<div class='marketPrice'><span style='opacity:0'>s</span></div>");
    }
    append_card_badges(sb, pe, 0);
    sb_append(sb, "<div class='cardActions'><a class='cardAction' onmouseenter='SOUND.play(`tick_0`,0.1)'");
    if (!g_market_ro) {
        // showPopup(sub, skinindex, thumbnail, label, skinid, funds, c, salestart, player || acc.name || 0, uid, marketid)
        sb_appendf(sb, " onclick='showPopup(&quot;%s&quot;,%d,&quot;%s&quot;,&quot;%s&quot;,&quot;%d&quot;,&quot;%.15g&quot;,%d,&quot;%.15g&quot;,", sub, m->skin, prev, label, m->skin_id,
                   m->funds, m->count, m->salestart);
        const char *who = m->player[0] ? m->player : acc ? acc->name : NULL;
        if (who) {
            sb_append(sb, "&quot;");
            sb_popup_arg(sb, who);
            sb_append(sb, "&quot;");
        } else {
            sb_append(sb, "0");
        }
        sb_appendf(sb, ",%d,%d)'", m->uid, m->market_id);
    }
    sb_appendf(sb, ">%s</a><div class='cardActionSep'></div>", !strcmp(sub, "market") ? "Purchase" : inventory ? "List" : sales ? "Unlist" : "");
    if (inventory && !g_market_ro) {
        sb_appendf(sb, "<a class='cardAction' onmouseenter='SOUND.play(`tick_0`,0.1)' onclick='quickSell(%d,%d,%d)'>Quick Sell</a>", m->skin, pe->rarity, m->uid);
    } else {
        sb_appendf(sb, "<div><a class='cardAction' style='display: inline-block;' onmouseenter='SOUND.play(`tick_0`,0.1)' onclick='searchItem(%d)'>Search</a>", m->skin);
        if (!inventory) sb_appendf(sb, "<div class='cardActionSepVert'></div><a class='cardAction' style='display: inline-block;' onmouseenter='SOUND.play(`tick_0`,0.1)' onclick='shareListing(this, %d)'>Share</a>", m->market_id);
        sb_append(sb, "</div>");
    }
    sb_append(sb, "<div class='cardActionSep'></div>");
    append_info_preview(sb, m->skin);
    sb_append(sb, "</div></div>");
    g_spray_anims = NULL;
}

// io_0("market", sub, rows, readOnly, ..., list) (hub-10.0.7.js:2717, 3187-3361): the market / inventory / sales /
// stats / Black Market / trades lists. readOnly (Only Show Missing's reply, :4600) draws the cards without their
// Purchase / List, estimate and price; list keeps only those store indices.
static void hub_render_market(const char *a, const cJSON *t, const int read_only, const cJSON *list) {
    const AccountInfo *acc = g_hub.logged_in ? active_account() : NULL;
    StrBuf sb = {0};
    int shown = 0;
    const char *sub = a ? a : "market";
    if (cJSON_IsArray(t) || cJSON_IsObject(t)) {
        if (!strcmp(sub, "stats")) {
            // hub-10.0.7.js:3197-3226: a market card per item (createTip, the "ID:" line, In Circulation, the badges;
            // Preview / Search / Info). v10 names the keyframes sprayStatAnm (60 px frames) while the cards' animation is
            // sprayAnm: a stat card's spray stays on its first frame, as in v10. Its trailing "</div>" closes nothing.
            SprayAnims anims = {.name = "sprayAnm"};
            g_spray_anims = &anims;
            StrBuf cards = {0};
            const cJSON *r;
            cJSON_ArrayForEach(r, t) {
                const int ski = row_int(r, "ski", NULL, NULL, -1);
                const ViewerSkin *pe = hub_skin(ski);
                if (!pe || (pe->flags & (VS_BLOCKED | VS_FREE))) continue;
                shown++;
                char prev[256];
                skin_preview(prev, sizeof(prev), pe);
                const char *col = rarity_color(pe);
                sb_appendf(&cards, "<div class='marketCard%s' style='color:%s;border:5px solid %s' onmouseenter='SOUND.play(`tick_0`,0.1)'data-index='%d' onmouseover='createTip(this)' onmouseout='cancelTip()'>",
                           g_hub.rarity_anim && rarity_animate(pe) ? " rainbow" : "", col, col, ski);
                sb_append_esc(&cards, pe->name);
                sb_appendf(&cards, "<div class='itemOwn'>%s</div>", skin_type_label(pe));
                if (hub_dev()) sb_appendf(&cards, "<div class='itemOwn'>ID: %d</div>", ski);
                append_skin_img(&cards, pe, prev, "sprayMarketImg", market_img_class(pe));
                sb_appendf(&cards, "<div class='cardEst'>%s <span style='color:#fff'>%d</span></div>", I18N("social.market.stats.circulation", "In Circulation"), row_int(r, "inC", NULL, NULL, 0));
                append_card_badges(&cards, pe, 0);
                sb_appendf(&cards, "<div class='cardActions'><a class='cardAction' onmouseenter='SOUND.play(`tick_0`,0.1)' onclick='viewItem(%d)'>Preview</a><div class='cardActionSep'></div>"
                                   "<a class='cardAction' onmouseenter='SOUND.play(`tick_0`,0.1)' onclick='searchItem(%d)'>Search</a><div class='cardActionSep'></div>"
                                   "<div><a class='cardAction' style='display: inline-block;' onmouseenter='SOUND.play(`tick_0`,0.1)' onclick='openSales(%d)'>Info</a></div></div></div>", ski, ski, ski);
            }
            g_spray_anims = NULL;
            anims.name = "sprayStatAnm";
            StrBuf kf = {0};
            spray_anims_keyframes(&kf, &anims, 60);
            char *kfs = sb_finish(&kf), *body = sb_finish(&cards);
            sb_appendf(&sb, "%s%s</div>%s", kfs, body, kfs); // marketList = styles + (cards + "</div>" + styles)
            free(kfs);
            free(body);
        } else if (!strcmp(sub, "blackm")) {
            // hub-10.0.7.js:3229-3264: every store item with blackM && !free && !hide (shown = 1: never "none"); t holds
            // the ones this account already has (kn_brain: bought here or owned), each card of those gets the blackMDone
            // tick. Purchase is showPopup("blackm", index, thumbnail, label, index, funds).
            shown = 1;
            SprayAnims anims = {.name = "sprayAnm"};
            g_spray_anims = &anims;
            StrBuf cards = {0};
            int n_illicit = 0;
            const IllicitSkin *illicit = econ_black_market(&n_illicit);
            for (int i = 0; i < n_illicit; i++) {
                const IllicitSkin *il = &illicit[i];
                if (il->hide || il->free_) continue;
                const ViewerSkin *pe = hub_skin(il->skin);
                if (!pe) continue;
                int done = 0;
                const cJSON *o;
                cJSON_ArrayForEach(o, t) done |= cJSON_IsNumber(o) && (int) o->valuedouble == il->skin;
                char prev[256], price[32];
                skin_preview(prev, sizeof(prev), pe);
                const char *col = rarity_color(pe), *label = skin_type_label(pe);
                sb_appendf(&cards, "<div class='marketCard%s' id='itemCard_%d' style='color:%s;border:5px solid %s' onmouseenter='SOUND.play(`tick_0`,0.1)' data-index='%d' onmouseover='createTip(this)' onmouseout='cancelTip()'>",
                           g_hub.rarity_anim && rarity_animate(pe) ? " rainbow" : "", il->skin, col, col, il->skin);
                sb_append_esc(&cards, pe->name);
                sb_appendf(&cards, "<div class='itemOwn'>%s</div>", label);
                if (hub_dev()) sb_appendf(&cards, "<div class='itemOwn'>ID: %d</div>", il->skin);
                append_skin_img(&cards, pe, prev, "sprayMarketImg", market_img_class(pe));
                comma_number(price, sizeof(price), il->funds);
                if (il->funds) sb_appendf(&cards, "<div class='marketPrice'>%s<span style='color:#fff'> KR</span></div>", price);
                else sb_append(&cards, "<div class='marketPrice'><span style='color:#fff'>FREE</span></div>");
                append_card_badges(&cards, pe, 0);
                sb_appendf(&cards, "<div class='cardActions'><a class='cardAction' onmouseenter='SOUND.play(`tick_0`,0.1)' onclick='showPopup(&quot;blackm&quot;,%d,&quot;%s&quot;,&quot;%s&quot;,&quot;%d&quot;,&quot;%d&quot;)'>Purchase</a><div class='cardActionSep'></div>",
                           il->skin, prev, label, il->skin, il->funds);
                append_info_preview(&cards, il->skin);
                sb_append(&cards, "</div>");
                if (done) sb_append(&cards, "<div class=\"blackMDone\"><span class=\"material-icons\" style=\"font-size:160px;color:#38cd3a;line-height:215px\">done</span></div>");
                sb_append(&cards, "</div>");
            }
            g_spray_anims = NULL;
            spray_anims_keyframes(&sb, &anims, 200); // marketList = styles + cards
            char *body = sb_finish(&cards);
            sb_append(&sb, body);
            free(body);
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
                g_market_ro = read_only;
                const int inventory = !strcmp(sub, "inventory"), sales = !strcmp(sub, "sales");
                int te = 0;
                double oe = 0;
                free(g_market_view);
                g_market_view = malloc((size_t) (n ? n : 1) * sizeof(int));
                g_market_view_n = 0;
                for (int v = 0; v < n; v++) {
                    const MarketRow *m = &rows[v];
                    const ViewerSkin *pe = hub_skin(m->skin); // NULL past the store (skinindex >= 1e7: a custom skin)
                    if (!pe || (pe->flags & (VS_FREE | VS_BLOCKED))) continue;
                    // config.marketQueries[a].checkItem (config-10.0.7.js:9496)
                    if ((!strcmp(sub, "market") || !strcmp(sub, "sales")) && !m->funds) continue;
                    if (inventory && (m->funds || !m->count)) continue;
                    if (cJSON_IsArray(list)) { // list.indexOf(skinindex) >= 0
                        int in = 0;
                        const cJSON *o;
                        cJSON_ArrayForEach(o, list) in |= cJSON_IsNumber(o) && (int) o->valuedouble == m->skin;
                        if (!in) continue;
                    }
                    shown++;
                    te++;
                    // the stat's value (hub-10.0.7.js:3321-3322): a sale's price, else the estimate, times the count
                    const double val = sales ? (double) (long long) m->funds : (double) estimate_value(m->skin);
                    oe += round(val * (m->count ? m->count : sales ? 1 : 0));
                    if (g_market_view) g_market_view[g_market_view_n++] = v;
                }
                if (shown) {
                    vgrid_begin(&g_market_grid, "marketList", g_market_view_n, market_card, NULL);
                    g_market_anims.n = 0;
                    for (int i = 0; i < g_market_view_n; i++) spray_anims_add(&g_market_anims, hub_skin(g_market_rows[g_market_view[i]].skin));
                    StrBuf kf = {0};
                    spray_anims_keyframes(&kf, &g_market_anims, 200); // hub-10.0.7.js:3353
                    char *kfs = sb_finish(&kf);
                    vgrid_set_prefix(&g_market_grid, kfs);
                    free(kfs);
                    vgrid_html(&g_market_grid, &sb);
                }
                // #invStats (hub-10.0.7.js:3349): "N / <skins> Items" and Est. Value, or My Sales' "N / 20 (40 premium)
                // On Sale" and Potential Earnings
                char stats[400], v_s[32];
                comma_number(v_s, sizeof(v_s), oe);
                if (sales)
                    snprintf(stats, sizeof(stats), "<div style='display: inline-block;vertical-align: bottom;'><div id='invStat'>%d<span style='color:#fff'> / %d On Sale</span></div>"
                             "<div id='invStat'>Potential Earnings <span style='color:#fff'>%s</span> KR</div></div>", te, acc && acc->premium > 0 ? 40 : 20, v_s);
                else
                    snprintf(stats, sizeof(stats), "<div style='display: inline-block;vertical-align: bottom;'><div id='invStat'>%d<span style='color:#fff'> / %d Items</span></div>"
                             "<div id='invStat'>Est. Value <span style='color:#fff'>%s</span> KR</div></div>", te, g_viewer_skin_count, v_s);
                set_html("invStats", stats);
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

// config.events' names (config-10.0.7.js:10098-10406): a store item's `event`
static const char *const k_event_names[] = {
    "#FORTHEKIDS Charity Event", "April Drops", "June Drops", "#FORTHEKIDS Charity Event", "#FORTHEKIDS Charity Event",
    "Final #FORTHEKIDS Charity Event", "End of Holiday Season Drops", "Junkyard Tuesdays #1", "Junkyard Tuesdays #2",
    "Junkyard Tuesdays #3", "Junkyard Tuesdays #4", "Junkyard Tuesdays #5", "Junkyard Tuesdays #6", "Junkyard Tuesdays #7",
    "Junkyard Tuesdays #8", "Junkyard Tuesdays #9", "Junkyard Tuesdays #10", "Junkyard Tuesdays #11", "Junkyard Tuesdays #12",
    "Junkyard Tuesdays #13", "Junkyard Tuesdays #14", "Krunker x FRVR", "Season 7 Launch",
};

static void open_item_tab(const char *tab);

// io_0("itemsales", "market", {ind, high, low, onS, inC}, owners[]) (hub-10.0.7.js:3418-3494): the item's page: its
// picture, corner label, season, 3D (viewItem(ind, 2)), the lock of an item that cannot be sold, Find, the creators'
// profile links, its type, lore, event, emoji text, streamers, On Sale / In Circulation, the high / low and owners.
// An opensea item shows only its owners tab.
static void hub_render_itemsales(const cJSON *t, const cJSON *owners) {
    side_boxes();
    set_display("itemsalesList", "none");
    const int ind = cJSON_IsObject(t) ? row_int(t, "ind", NULL, NULL, -1) : -1;
    const ViewerSkin *pe = hub_skin(ind);
    if (!pe || (pe->flags & VS_BLOCKED)) {
        set_market_note("itemsalesHolder", STR_MARKET_NONE);
        set_display("itemsalesHolder", "block");
        return;
    }
    set_display("itemsalesList", "block");
    char prev[256], a[32], b[32];
    skin_preview(prev, sizeof(prev), pe);
    const char *col = rarity_color(pe);
    const int rb = g_hub.rarity_anim && rarity_animate(pe);
    const int opensea = skin_true(ind, "opensea");
    const cJSON *item_shop = skin_field(ind, "itemShop");
    const int bound = (pe->flags & VS_FREE) || skin_true(ind, "free") || skin_true(ind, "noSale") || skin_true(ind, "blackM") || opensea ||
                      (cJSON_IsNumber(item_shop) && item_shop->valuedouble == 1);
    const char *lore = skin_str(ind, "lore"), *streamer = skin_str(ind, "streamer"), *creator = skin_str(ind, "creator");
    const cJSON *event = skin_field(ind, "event"), *text = skin_field(ind, "text");
    const int has_event = event && !cJSON_IsNull(event);
    const int top = 94 - (lore ? 25 : 0) - (has_event ? 25 : 0) - (streamer ? 30 : 0);
    StrBuf sb = {0};
    if (pe->type == 4 && pe->frames) sb_appendf(&sb, "<style>@keyframes sprayAnm%d {100%%{background-position:-%dpx}}</style>", pe->frames, pe->frames * 220);
    if (pe->type == 4) {
        sb_appendf(&sb, "<div class='spraySalesImg' style='background: url(&quot;%s&quot;);margin-top:0px;", prev);
        spray_anim_style(&sb, pe, "sprayAnm");
        sb_appendf(&sb, "border:5px solid %s'></div>", col);
    } else {
        sb_appendf(&sb, "<div class='imgWepH%s' style='border:5px solid %s'><img draggable='false' class='imgWep%s%s' src='%s'></div>", rb ? " rainbowBorder" : "", col, pe->type == 16 ? "P" : "",
                   (pe->flags & VS_RGB) ? " rgbHue" : "", prev);
    }
    append_item_label(&sb, pe, ind, "top: 202px;left: -1px;");
    sb_appendf(&sb, "<div class='popupImgSeason' style='top: 8px;left: 12px;'>Season %d</div><div class='popupImgInfo' style='left: 181px;top: 9px;' onclick='viewItem(%d, 2)'>3D</div>", pe->seas ? pe->seas : 1, ind);
    if (bound) sb_append(&sb, "<div class='popupImgBound'><span class='material-icons' style='font-size:30px;color:#fff;'>lock</span></div>");
    // an opensea item's OpenSea button needs window.canShowNFTs, which KrunkNative never sets: Find
    sb_appendf(&sb, "<div class='buyBtn' onclick='openLink(&quot;/social.html?p=market&i=%d&quot;, &quot;_blank&quot;)'>Find</div><div style='display:inline-block'><div class='details%s' style='color:%s'>", ind, rb ? " rainbowText" : "",
               pe->rarity == 5 ? "rgba(255,255,255,0.4)" : col); // CONTRABAND (hub-10.0.7.js:478)
    sb_append_esc(&sb, pe->name);
    sb_append(&sb, "</div><div class='smDetails'>by ");
    if (creator) { // creator.split(", "): a profile link each
        for (const char *c = creator; *c;) {
            const char *e = strstr(c, ", ");
            const size_t len = e ? (size_t) (e - c) : strlen(c);
            char name[96];
            snprintf(name, sizeof(name), "%.*s", (int) (len < sizeof(name) ? len : sizeof(name) - 1), c);
            if (c != creator) sb_append(&sb, ", ");
            sb_append(&sb, "<a style='color:#fff;' href='/social.html?p=profile&q=");
            sb_append_esc(&sb, name);
            sb_append(&sb, "' target='_blank'>");
            sb_append_esc(&sb, name);
            sb_append(&sb, "</a>");
            c = e ? e + 2 : c + len;
        }
    } else {
        sb_append(&sb, "Krunker.io");
    }
    sb_appendf(&sb, "</div><div class='smDetails'>%s</div>", skin_type_label(pe));
    if (lore) {
        sb_append(&sb, "<div class='smDetails' style='font-style:italic;'>");
        sb_append_esc(&sb, lore);
        sb_append(&sb, "</div>");
    }
    if (has_event) {
        const int ev_i = cJSON_IsNumber(event) ? event->valueint : -1;
        sb_appendf(&sb, "<div class='smDetails' style='font-style:italic;'>%s</div>",
                   ev_i >= 0 && ev_i < (int) (sizeof(k_event_names) / sizeof(k_event_names[0])) ? k_event_names[ev_i] : "");
    }
    if (cJSON_IsArray(text) && cJSON_GetStringValue(cJSON_GetArrayItem(text, 0))) {
        sb_append(&sb, "<div class='smDetails' style='margin-top: 20px'>Type <span style='color:#ccc'>:");
        sb_append_esc(&sb, cJSON_GetStringValue(cJSON_GetArrayItem(text, 0)));
        sb_append(&sb, ":</span> in chat to use</div>");
    }
    if (streamer) { // several: a link each; one: the whole line links
        if (strstr(streamer, ", ")) {
            sb_append(&sb, "<div class='smDetails' id='streamerSkinH'><img id='streamerSkin' src='./img/twitch.png'>");
            for (const char *c = streamer; *c;) {
                const char *e = strstr(c, ", ");
                const size_t len = e ? (size_t) (e - c) : strlen(c);
                char name[96];
                snprintf(name, sizeof(name), "%.*s", (int) (len < sizeof(name) ? len : sizeof(name) - 1), c);
                sb_append(&sb, "<span id='streamerSkinN' onclick='openLink(&quot;https://twitch.tv/");
                sb_append_esc(&sb, name);
                sb_append(&sb, "&quot;, &quot;_blank&quot;)'>");
                sb_append_esc(&sb, name);
                sb_append(&sb, "</span>");
                if (e) sb_append(&sb, ", ");
                c = e ? e + 2 : c + len;
            }
            sb_append(&sb, "</div>");
        } else {
            sb_append(&sb, "<div class='smDetails' id='streamerSkinH' onclick='openLink(&quot;https://twitch.tv/");
            sb_append_esc(&sb, streamer);
            sb_append(&sb, "&quot;, &quot;_blank&quot;)'><img id='streamerSkin' src='./img/twitch.png'><span id='streamerSkinN'>");
            sb_append_esc(&sb, streamer);
            sb_append(&sb, "</span></div>");
        }
    }
    if (opensea) {
        set_display("iTab_avg", "none");
        set_display("iTab_sold", "none");
        set_style("iTab_owners", "width", "100%");
        set_display("dateInfo", "none");
        open_item_tab("owners");
    } else {
        comma_number(a, sizeof(a), jnum(t, "onS"));
        comma_number(b, sizeof(b), jnum(t, "inC"));
        sb_appendf(&sb, "<div class='smDetails' style='margin-top: %dpx;'>On Sale <span style='float:right;color: rgba(255,255,255,0.6);'>", top);
        if (skin_true(ind, "noSale")) sb_append(&sb, "Cannot be Sold");
        else sb_appendf(&sb, "%s <span style='color:#787878'> units</span>", a);
        sb_appendf(&sb, "</span></div><div class='smDetails'>%s <span style='float:right;color: rgba(255,255,255,0.6);'>%s <span style='color:#787878'> %s</span></span></div></div>",
                   I18N("social.market.stats.circulation", "In Circulation"), b, I18N("social.market.stats.units", "units"));
        comma_number(a, sizeof(a), jnum(t, "high"));
        comma_number(b, sizeof(b), jnum(t, "low"));
        sb_appendf(&sb, "<div style='position:absolute;top: 240px;right: 0px;'><div class='minMax' style='background-color:#41c300'><i class='material-icons' style='color:#fff;font-size:26px;vertical-align:middle;margin-top:-5px'>arrow_upward</i>%s</div><div class='minMax' style='background-color:rgb(244,67,54)'><i class='material-icons' style='color:#fff;font-size:26px;vertical-align:middle;margin-top:-5px'>arrow_downward</i>%s</div></div>", a, b);
        // iíîîîìì(ind, range): the price graph data (["st"] -> ["gd"]; KrunkNative draws no chart)
        cJSON *f = cJSON_CreateArray();
        cJSON_AddItemToArray(f, cJSON_CreateString("st"));
        cJSON_AddItemToArray(f, cJSON_CreateNumber(ind));
        cJSON_AddItemToArray(f, cJSON_CreateNumber(30));
        cJSON_AddItemToArray(f, cJSON_CreateTrue());
        hub_send(f);
    }
    char *html = sb_finish(&sb);
    set_html("weaponInfo", html);
    free(html);
    if (cJSON_IsArray(owners) && cJSON_GetArraySize(owners)) {
        int v = 0;
        const cJSON *r;
        cJSON_ArrayForEach(r, owners) {
            const char *nm = jstr(r, "nm");
            sb_appendf(&sb, "<div style='color:#5c5c5c'>%d. <div class='leaderText'>", ++v);
            if (jnum(r, "su")) sb_append(&sb, "<i class='material-icons' style='color:#FBC02D;margin-top:-8px;font-size:28px;vertical-align:middle;'>beenhere</i>");
            if (jnum(r, "ft")) sb_append(&sb, "<i class='material-icons' style='color:#40C4FF;margin-top:4px;margin-right: 4px;font-size:28px;vertical-align:baseline;'>check_circle</i>");
            sb_append(&sb, "<a target='_blank' href='/social.html?p=profile&q=");
            sb_append_esc(&sb, nm ? nm : "");
            sb_append(&sb, "&autoTrade' class='lName'>");
            sb_append_esc(&sb, nm ? nm : "");
            sb_appendf(&sb, "</a></div><div style='float:right;color:#5c5c5c'>x%d</div></div>", (int) jnum(r, "ct"));
        }
        html = sb_finish(&sb);
        set_html("iTabContent_owners", html);
        free(html);
    }
    if (pe->name) hub_title(pe->name, 0, " Skin - Krunker Hub");
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
    // hub-10.0.7.js:4165-4174: the profile's posts are socPost fromProfile
    if (!cJSON_IsArray(posts) || !cJSON_GetArraySize(posts)) sb_append(&sb, "<div class='scrollItemNew'><div class='noneText'>No Current Posts!</div></div>");
    const cJSON *p;
    cJSON_ArrayForEach(p, posts) append_post(&sb, p, 0, 1);
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
    comma_number(n, sizeof(n), total);
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

// The Skin List's filters, iiiîiìi (hub-10.0.7.js:2413-2453): a store field and the value it must equal (-1 / NULL: no
// such filter). The categories (seas, limT, blackM, drop, ranked, challenge, spinable) exclude each other.
typedef struct {
    int seas, rarity, weapon, type; // -1 = no filter
    char cat[16];                   // blackM / drop / ranked / challenge / spinable / limT ("" none)
    char lim_t[32];                 // limT's tag
    int sort;                       // iîïïîíì: 0 normal (rarity), 1 newest, 2 oldest
    int *list;                      // iiîiiìi: skin indices
    int count, shown;
    VGrid grid;                     // #itemsList: a virtual grid over list (ui_vgrid.h; v10 appends 500 at a time)
} HubItems;
static HubItems g_items = {-1, -1, -1, -1, "", "", 0, NULL, 0, 0, {{0}}};

// iïïíìîì(item) (hub-10.0.7.js:2535-2541): the Skin List's card (no tooltip); ?dev adds the "ID:" line and Viewer
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
    const int dev = hub_dev();
    if (dev) sb_appendf(sb, "<div class='itemOwn'>ID: %d</div>", index);
    if (sk->type == 4) {
        sb_appendf(sb, "<div class='sprayMarketImg' style='background: url(&quot;%s&quot;);", url);
        spray_anim_style(sb, sk, "itemAnm");
        sb_append(sb, "'></div>");
    } else {
        sb_appendf(sb, "<img draggable='false' loading='lazy' class='%s%s' src='%s'/>", market_img_class(sk), (sk->flags & VS_RGB) ? " rgbHue" : "", url);
    }
    if ((sk->flags & VS_HAS_ANIM) || sk->puls_t || sk->frames || sk->mov_t || sk->r_anim || (sk->flags & VS_RGB) || (sk->flags2 & (VS2_SHADER | VS2_VIDEO | VS2_PROJECTION)))
        sb_appendf(sb, "<div class='material-icons isAnim'>%s</div>", sk->r_anim_t ? "import_export" : "loop");
    sb_appendf(sb, "<div class='cardActions'><a class='cardAction' onmouseenter='SOUND.play(`tick_0`,0.1)' onclick='viewItem(%d)'>Preview</a><div class='cardActionSep'></div><a class='cardAction' onmouseenter='SOUND.play(`tick_0`,0.1)' onclick='openLink(&quot;/social.html?p=market&i=%d&quot;, &quot;_blank&quot;)'>Listings</a><div class='cardActionSep'></div><a class='cardAction' onmouseenter='SOUND.play(`tick_0`,0.1)' onclick='openSales(%d)'>Info</a>",
               index, index, index);
    if (dev) sb_appendf(sb, "<div class='cardActionSep'></div><a class='cardAction' onmouseenter='SOUND.play(`tick_0`,0.1)' onclick='openURL(&quot;/viewer.html?cosmetic=%d&quot;)'>Viewer</a>", index);
    sb_append(sb, "</div>");
    if (sk->flags2 & VS2_SND) sb_append(sb, "<div class='material-icons hasSnd'>volume_up</div>");
    sb_append(sb, "</div>");
}

static void items_grid_card(StrBuf *sb, const int pos, void *user) {
    (void) user;
    if (pos >= 0 && pos < g_items.count) items_card(sb, g_items.list[pos]);
}

// iiîìïïì's normal order (hub-10.0.7.js:2523-2527): rarity, highest first. v10 breaks a tie on `size`, which no store
// item has (its comparator then answers 1 both ways); KrunkNative keeps the table order.
static int items_rarity_cmp(const void *pa, const void *pb) {
    const int a = *(const int *) pa, b = *(const int *) pb;
    const ViewerSkin *sa = hub_skin(a), *sbk = hub_skin(b);
    const int ra = sa ? sa->rarity : 0, rb = sbk ? sbk->rarity : 0;
    if (ra != rb) return rb - ra;
    return a - b;
}

static int items_index_cmp(const void *pa, const void *pb) { return *(const int *) pa - *(const int *) pb; }

// iîìiíìï + iiîìïïì (hub-10.0.7.js:2506-2530): the keyframes, the count, sort, the cards
static void items_show(void) {
    StrBuf sb = {0};
    sb_append(&sb, "<style>");
    for (int i = 0; i < 11; i++) sb_appendf(&sb, "@keyframes itemAnm%d {100%%{background-position:-%dpx}}", i, i * 200);
    sb_append(&sb, "</style>");
    char *style = sb_finish(&sb);
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
    vgrid_begin(&g_items.grid, "itemsList", g_items.count, items_grid_card, NULL);
    vgrid_set_prefix(&g_items.grid, style); // v10 keeps the @keyframes inside #itemsList
    free(style);
    StrBuf list = {0};
    vgrid_html(&g_items.grid, &list);
    char *html = sb_finish(&list);
    set_html("itemsList", html);
    free(html);
}

static void items_alloc(void) {
    if (!g_items.list) g_items.list = calloc((size_t) (g_viewer_skin_count ? g_viewer_skin_count : 1), sizeof(int));
    g_items.count = 0;
}

// item[key] == true, JavaScript's loose equality (true, 1)
static int skin_is_true(const int index, const char *key) {
    const cJSON *v = skin_field(index, key);
    return cJSON_IsTrue(v) || (cJSON_IsNumber(v) && v->valuedouble == 1) || (cJSON_IsString(v) && !strcmp(v->valuestring, "1"));
}

// iïïíîìì (hub-10.0.7.js:2512-2522): store.skins without the blocked / opensea ones, each filter's field equal to its
// value (a missing field fails); spinable: this season's, none of limT / limG / noSpin / opensea / blackM / blocked /
// limited / req / mapReward / eventReward
static void items_filter_apply(void) {
    items_alloc();
    for (int i = 0; g_items.list && i < g_viewer_skin_count; i++) {
        const ViewerSkin *sk = &g_viewer_skins[i];
        if ((sk->flags & VS_BLOCKED) || skin_true(i, "opensea")) continue;
        if (g_items.seas >= 0 && (!skin_field(i, "seas") || sk->seas != g_items.seas)) continue;
        if (g_items.rarity >= 0 && (!skin_field(i, "rarity") || sk->rarity != g_items.rarity)) continue;
        if (g_items.weapon >= 0 && sk->weapon != g_items.weapon) continue;
        if (g_items.type >= 0 && sk->type != g_items.type) continue;
        if (!strcmp(g_items.cat, "limT")) {
            const char *t = skin_str(i, "limT");
            if (!t || strcmp(t, g_items.lim_t) != 0) continue;
        } else if (!strcmp(g_items.cat, "spinable")) {
            static const char *const no[] = {"limT", "limG", "noSpin", "opensea", "blackM", "limited", "req", "mapReward", "eventReward"};
            int bad = sk->seas != g_viewer_current_season;
            for (size_t k = 0; !bad && k < sizeof(no) / sizeof(no[0]); k++) bad = skin_true(i, no[k]);
            if (bad) continue;
        } else if (g_items.cat[0] && !skin_is_true(i, g_items.cat)) {
            continue;
        }
        g_items.list[g_items.count++] = i;
    }
    items_show();
}

// window.itemFilter(key, value) (hub-10.0.7.js:2419-2453): sort; "all" drops the filter (weapon / type both,
// category every category); a weapon drops the type and back, a category the other categories
static void items_filter(const char *key, const char *value) {
    const int all = !value || !strcmp(value, "all");
    if (!strcmp(key, "sort")) {
        if (all) g_items.sort = 0;
        else if (!strcmp(value, "new")) g_items.sort = 1;
        else if (!strcmp(value, "old")) g_items.sort = 2;
        items_filter_apply();
        return;
    }
    static const char *const cats[] = {"seas", "limT", "blackM", "drop", "ranked", "challenge", "spinable"};
    int is_cat = 0;
    for (size_t i = 0; i < sizeof(cats) / sizeof(cats[0]); i++) is_cat |= !strcmp(key, cats[i]);
    if (all) {
        if (!strcmp(key, "weapon") || !strcmp(key, "type")) g_items.weapon = g_items.type = -1;
        else if (!strcmp(key, "rarity")) g_items.rarity = -1;
        else if (!strcmp(key, "seas")) g_items.seas = -1;
        else if (!strcmp(key, "category")) g_items.seas = -1, g_items.cat[0] = 0;
        else if (!strcmp(key, g_items.cat)) g_items.cat[0] = 0;
    } else {
        if (is_cat) { // the other categories go
            if (strcmp(key, "seas") != 0) g_items.seas = -1;
            if (strcmp(key, g_items.cat) != 0) g_items.cat[0] = 0;
        }
        if (!strcmp(key, "weapon")) g_items.type = -1, g_items.weapon = atoi(value);
        else if (!strcmp(key, "type")) g_items.weapon = -1, g_items.type = atoi(value);
        else if (!strcmp(key, "rarity")) g_items.rarity = atoi(value);
        else if (!strcmp(key, "seas")) g_items.seas = atoi(value);
        else if (is_cat) {
            snprintf(g_items.cat, sizeof(g_items.cat), "%s", key);
            if (!strcmp(key, "limT")) snprintf(g_items.lim_t, sizeof(g_items.lim_t), "%s", value);
        }
    }
    const HtmlElem in = el("itemFilterInput");
    if (in) html_dom_set_value(in, "");
    items_filter_apply();
}

// iíïíiií(a, b) (config-10.0.7.js): either contains the other, ignoring case
static int contains_either_ci(const char *a, const char *b) {
    return a && b && (contains_ci_str(a, b) || contains_ci_str(b, a));
}

// window.itemFilterSearch(text) (hub-10.0.7.js:2466-2499): the filters reset; every item whose keyW, "drops" (a drop),
// limT, weapon, name or a creator matches the text either way round, without the blocked / opensea ones
static void items_search(const char *text) {
    g_items.seas = g_items.rarity = g_items.weapon = g_items.type = -1;
    g_items.cat[0] = 0;
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
        if (!hit && skin_str(i, "keyW") && contains_either_ci(text, skin_str(i, "keyW"))) hit = 1;
        else if (!hit && skin_true(i, "drop") && contains_either_ci(text, "drops")) hit = 1;
        else if (!hit && skin_str(i, "limT") && contains_either_ci(text, skin_str(i, "limT"))) hit = 1;
        else if (!hit && sk->weapon >= 1 && sk->weapon <= rules_krunker_weapon_count() && rules_weapon(sk->weapon - 1) && contains_either_ci(text, rules_weapon(sk->weapon - 1)->name)) hit = 1;
        else if (!hit && sk->name && contains_either_ci(text, sk->name)) hit = 1;
        else if (!hit) {
            const cJSON *c;
            cJSON_ArrayForEach(c, skin_field(i, "creators")) {
                if (cJSON_IsString(c) && contains_either_ci(text, c->valuestring)) {
                    hit = 1;
                    break;
                }
            }
        }
        if ((sk->flags & VS_BLOCKED) || skin_true(i, "opensea")) hit = 0;
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
        static const char *const cats[][2] = {{"Black Market", "blackM"}, {"Drops", "drop"}, {"Ranked", "ranked"}, {"Challenge", "challenge"}, {"Spinable", "spinable"}};
        if (!strcmp(text, "All")) items_filter("category", "all");
        else if (!strncmp(text, "Season ", 7)) items_filter("seas", text + 7);
        else {
            int done = 0;
            for (size_t i = 0; !done && i < sizeof(cats) / sizeof(cats[0]); i++)
                if (!strcmp(text, cats[i][0])) items_filter(cats[i][1], "true"), done = 1;
            if (!done) items_filter("limT", text); // the store.tags options
        }
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
        for (int i = 0; i < rules_krunker_weapon_count(); i++) {
            if (rules_weapon(i) && rules_weapon(i)->name && !strcmp(rules_weapon(i)->name, text)) {
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
    // hub-10.0.7.js:3364-3377: Season 1..currentSeason, the categories, one option per store.tags name
    for (int i = 1; i <= g_viewer_current_season; i++) sb_appendf(&sb, "<option onclick='itemFilter(`seas`,%d)'>Season %d</option>", i, i);
    sb_append(&sb, "<option onclick='itemFilter(`blackM`,true)'>Black Market</option><option onclick='itemFilter(`drop`,true)'>Drops</option><option onclick='itemFilter(`ranked`,true)'>Ranked</option><option onclick='itemFilter(`challenge`,true)'>Challenge</option><option onclick='itemFilter(`spinable`,true)'>Spinable</option>");
    for (int i = 0; ui_loadout_tag_name(i); i++) sb_appendf(&sb, "<option onclick='itemFilter(`limT`,`%s`)'>%s</option>", ui_loadout_tag_name(i), ui_loadout_tag_name(i));
    sb_append(&sb, "</select>");
    sb_append(&sb, "<select id='itemRarity' onchange='itemFilterChange(this)'><option disabled selected>Rarity</option><option onclick='itemFilter(`rarity`,`all`)'>All</option>");
    for (int i = 0; i < g_viewer_rarity_count; i++) sb_appendf(&sb, "<option onclick='itemFilter(`rarity`,%d)'>%s</option>", i, g_viewer_rarities[i].name);
    sb_append(&sb, "</select><select id='itemWeapon' onchange='itemFilterChange(this)'><option disabled selected>Weapon</option><option onclick='itemFilter(`weapon`,`all`)'>All</option>");
    for (int i = 0; i < rules_krunker_weapon_count(); i++) {
        if (!rules_weapon(i) || rules_weapon(i)->melee || rules_weapon(i)->equipment) continue;
        sb_appendf(&sb, "<option onclick='itemFilter(`weapon`,%d)'>%s</option>", i + 1, rules_weapon(i)->name);
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
    g_items.cat[0] = 0;
    g_items.sort = 0;
    items_filter("seas", "all");
    // ?p=items&q=<key>&s=<value>: a preset filter
    if (a && *a && strcmp(a, "items") && k && *k) items_filter(a, k);
}

// ------------------------------------------------------------------ brain: market popups (showPopup and friends)

// showPopup's twelve arguments (hub-10.0.7.js:4790): page, skin, thumbnail, label, skinid, funds, count, salestart,
// player, uid, marketid, refresh. The strings are kept as passed ("" for an absent one).
typedef struct {
    char page[16], preview[256], label[64], skin_id[24], funds[24], salestart[24], player[64];
    int skin, count, uid, market_id, refresh;
    unsigned char has_count, has_uid, valid;
} MarketPop;
static MarketPop g_last_pop; // iïiiïïí: lastPop, the arguments again with refresh = true (refreshPopup)
static void last_pop_clear(void) { g_last_pop = (MarketPop) {.refresh = 1, .valid = 1}; }
static double g_grace_start; // iîïììîi: the salestart the grace timer counts from (0: none)
static int g_grace_timer;

// iïìiiïì: an inventory row's count after a listing (hub-10.0.7.js:5250), cleared by every io_0 (:2721); the popup
// shows it in place of the card's count (:4792)
typedef struct { int uid, cnt; } CountOverride;
static CountOverride g_count_over[64];
static int g_count_over_n;

static void count_override_set(const int uid, const int cnt) {
    for (int i = 0; i < g_count_over_n; i++)
        if (g_count_over[i].uid == uid) {
            g_count_over[i].cnt = cnt;
            return;
        }
    if (g_count_over_n < (int) (sizeof(g_count_over) / sizeof(g_count_over[0]))) g_count_over[g_count_over_n++] = (CountOverride) {uid, cnt};
}

static const CountOverride *count_override(const int uid) {
    for (int i = 0; i < g_count_over_n; i++)
        if (g_count_over[i].uid == uid) return &g_count_over[i];
    return NULL;
}

static void market_popup_html(StrBuf *out, const MarketPop *p, double elapsed);
static void grace_tick(void *user);

// window.showPopup (hub-10.0.7.js:4790-4857)
static void market_popup(const MarketPop *in) {
    MarketPop p = *in;
    g_last_pop = p;
    g_last_pop.refresh = 1;
    g_last_pop.valid = 1;
    const CountOverride *co = p.has_uid ? count_override(p.uid) : NULL;
    if (co) p.count = co->cnt, p.has_count = 1;
    reset_pops();
    set_display("popupHolder", p.page[0] ? "block" : "none");
    set_style("popupContent", "width", "618px");
    set_display("popupContent", "block");
    if (!hub_skin(p.skin) || !p.page[0]) return;
    const double salestart = atof(p.salestart);
    const double elapsed = fmax(0, now_epoch_ms() - salestart);
    if (!p.refresh && elapsed < SALE_GRACE_MS) g_grace_start = salestart;
    if (g_grace_start && !g_grace_timer) g_grace_timer = html_timer_add(1000, grace_tick, NULL);
    StrBuf sb = {0};
    market_popup_html(&sb, &p, elapsed);
    char *html = sb_finish(&sb);
    set_html("popupContent", html);
    free(html);
}

// iíííïiï, every second (hub-10.0.7.js:4363-4377): the open grace timer's "ready in" with seconds; once the window has
// passed, refreshPopup. (KrunkNative: the interval runs while a grace start is set; it does nothing otherwise.)
static void grace_tick(void *user) {
    (void) user;
    g_grace_timer = 0;
    if (!g_grace_start) return;
    const double h = now_epoch_ms() - g_grace_start;
    if (h >= SALE_GRACE_MS) {
        g_grace_start = 0;
        if (g_last_pop.valid && g_hub.brain) {
            const MarketPop p = g_last_pop;
            market_popup(&p);
        }
        return;
    }
    char t[48], ready[96];
    readable_time_s(t, sizeof(t), fmax(0, SALE_GRACE_MS - h), 1);
    I18N_F(ready, "social.market.grace", "ready in {0}", t);
    set_html("graceTimer", ready);
    g_grace_timer = html_timer_add(1000, grace_tick, NULL);
}

// the "Login to Buy" / "lvl N required" block (hub-10.0.7.js:4811-4818, 3515-3522)
static const char *market_block(const AccountInfo *acc, const int sell, char *buf, const size_t n) {
    if (!acc) return sell ? I18N("social.login.sell", "Login to Sell") : I18N("social.login.buy", "Login to Buy");
    if (acc->level >= MARKET_MIN_LVL) return NULL;
    char lvl[16];
    snprintf(lvl, sizeof(lvl), "%d", MARKET_MIN_LVL);
    I18N_F(buf, "social.market.req", "lvl {0} required", lvl);
    (void) n;
    return buf;
}

// the popup's image, 3D button and corner label (hub-10.0.7.js:4806-4808; the listing page's :3513)
static void append_popup_head(StrBuf *sb, const ViewerSkin *g, const int skin, const char *preview) {
    const char *col = rarity_color(g);
    const int rb = g_hub.rarity_anim && rarity_animate(g);
    sb_appendf(sb, "<div class='popupImgSeason'>Season %d</div>", g->seas ? g->seas : 1);
    if (g->type == 4) {
        sb_appendf(sb, "<div class='sprayPopupImg%s' style='background: url(&quot;%s&quot;);border:5px solid %s;", rb ? " rainbowBorder" : "", preview, col);
        spray_anim_style(sb, g, "sprayPopAnm");
        sb_append(sb, "'></div>");
    } else {
        sb_appendf(sb, "<div class='popupImgH%s' style='border:5px solid %s;'><img draggable='false' class='popupImg%s' src='%s'/></div>", rb ? " rainbowBorder" : "", col, (g->flags & VS_RGB) ? " rgbHue" : "", preview);
    }
    sb_appendf(sb, "<div class='popupImgInfo' onclick='openViewer(%d)'>3D</div>", skin);
    append_item_label(sb, g, skin, "");
}

// The popup's content (hub-10.0.7.js:4806-4855)
static void market_popup_html(StrBuf *out, const MarketPop *p, const double elapsed) {
    const ViewerSkin *g = hub_skin(p->skin);
    if (!g) return;
    const AccountInfo *acc = g_hub.logged_in ? active_account() : NULL;
    const char *col = rarity_color(g);
    StrBuf sb = {0};
    if (g->type == 4 && g->frames) sb_appendf(&sb, "<style>@keyframes sprayPopAnm%d {100%%{background-position:-%dpx}}</style>", g->frames, g->frames * 220);
    append_popup_head(&sb, g, p->skin, p->preview);
    sb_appendf(&sb, "<div class='popupInfo'><div class='pItemName%s' style='color:%s'>", g_hub.rarity_anim && rarity_animate(g) ? " rainbowText" : "", col);
    sb_append_esc(&sb, g->name);
    sb_appendf(&sb, "</div><div class='pItemRar'>x <span style='color:rgba(255,255,255,0.8);font-size:22px'>%d</span></div><div class='pItemDesc'>", p->has_count && p->count ? p->count : 1);
    sb_append_esc(&sb, p->label);
    sb_append(&sb, "</div>");
    const int inventory = !strcmp(p->page, "inventory"), blackm = !strcmp(p->page, "blackm");
    const IllicitSkin *il = illicit_skin(p->skin);
    if (blackm) {
        sb_append(&sb, "<div class='pItemOwn' style='bottom: 70px;max-width: 240px;'>");
        sb_append_esc(&sb, il && il->req_t ? il->req_t : "I got what you need");
        sb_append(&sb, "</div>");
    } else if (!inventory) {
        // iïîííïì(player || "You"): "./?profile=" + encodeURIComponent(the name without quotes)
        const char *who = p->player[0] ? p->player : "You";
        sb_append(&sb, "<a class='pItemOwn' href='./?profile=");
        for (const unsigned char *c = (const unsigned char *) who; *c; c++) {
            if (*c == '"' || *c == '\'') continue;
            if (isalnum(*c) || strchr("-_.!~*()", *c)) sb_appendf(&sb, "%c", *c);
            else sb_appendf(&sb, "%%%02X", *c);
        }
        sb_append(&sb, "' target='_blank'>");
        sb_append_unquoted(&sb, who);
        sb_append(&sb, "</a>");
    }
    if (atof(p->salestart) && !inventory) {
        char ago[48], listed[96];
        readable_time(ago, sizeof(ago), elapsed);
        I18N_F(listed, "social.market.listed", "listed {0} ago", ago);
        sb_appendf(&sb, "<div class='pItemTime'>%s</div>", listed);
    }
    char a[32], b[32], c[32], req[64];
    if (!strcmp(p->page, "market") || blackm) {
        const int grace = !blackm && elapsed < SALE_GRACE_MS;
        const char *block = blackm ? (!acc ? I18N("social.login.buy", "Login to Buy") : il && il->req != ECON_REQ_NONE && !illicit_req_ok(il, acc) ? "Access Denied" : NULL)
                                   : market_block(acc, 0, req, sizeof(req));
        const double funds = acc ? acc->funds : 0, price = atof(p->funds), left = funds - price;
        comma_number(a, sizeof(a), funds);
        comma_number(b, sizeof(b), price);
        comma_number(c, sizeof(c), left);
        sb_appendf(&sb, "<div class='pItemPricing'><div class='priceP'>%s <span style='color:#fff'>KR</span></div><div class='priceP'>-%s <span style='color:#fff'>KR</span></div><div class='pricePF' style='%s'>%s <span style='color:#fff'>KR</span></div></div>",
                   a, b, left < 0 ? "color:#ed4242" : "", c);
        sb_appendf(&sb, "<div class='pItemButton' style='%s' ", block || grace ? "background-color:#2b2b2b" : left < 0 ? "background-color:#ed4242" : "");
        if (blackm) econ_new_op(g_pbm_key, sizeof(g_pbm_key), "pbm"); // KrunkNative: one purchase per popup, however often it is clicked
        if (!block && !grace && left >= 0) {
            if (blackm) sb_appendf(&sb, "onclick='buyblackM(%d)'", p->skin);
            else sb_appendf(&sb, "onclick='buyItem(%d,%d)'", p->market_id, p->uid);
        }
        if (!block && !grace && left >= 0) sb_append(&sb, "onmouseover='SOUND.play(&quot;tick_0&quot;,0.1)'");
        if (grace) {
            readable_time_s(a, sizeof(a), SALE_GRACE_MS - elapsed, 1);
            char ready[96];
            I18N_F(ready, "social.market.grace", "ready in {0}", a);
            sb_appendf(&sb, "><span id='graceTimer' style='color:rgba(255,255,255,0.6)';'>%s</span></div>", ready);
        } else {
            sb_appendf(&sb, ">%s</div>", block ? block : left < 0 ? I18N("social.market.cant", "Can't Afford") : I18N("social.market.purchase", "Purchase"));
        }
    } else if (inventory) {
        const char *block = market_block(acc, 1, req, sizeof(req));
        sb_appendf(&sb, "<div class='pItemPricing'><div id='sellPricFee'>%s</div><input id='sellPriceIn' oninput='calcFee(this)' type='number' placeholder='%s KR' min='0' max='%d' step='1'></div><div id='cPostSaleBtn' class='pItemButton' style='display:none;background-color:#2b2b2b'>Not enough KR</div><div id='postSaleBtn' class='pItemButton' %s",
                   I18N("social.market.fee", "No List Fee"), I18N("social.market.price.list", "List Price in"), SALE_MAX, block ? "style='background-color:#2b2b2b'" : "");
        if (!block) sb_appendf(&sb, "onclick='listItem(%d,%s,%d)'", p->skin, p->skin_id[0] ? p->skin_id : "0", p->uid);
        sb_appendf(&sb, "onmouseover='SOUND.play(&quot;tick_0&quot;,0.1)'>%s</div>", block ? block : I18N("social.market.list", "List Item"));
    } else if (!strcmp(p->page, "sales")) {
        comma_number(a, sizeof(a), atof(p->funds));
        sb_appendf(&sb, "<div class='pItemPricing'>%s <span style='color:rgba(255,255,255,0.4)'>KR</span></div><div class='pItemButton' onclick='unlistItem(%d,%d,%d)' onmouseover='SOUND.play(&quot;tick_0&quot;,0.1)'>%s</div>", a, p->skin, p->market_id, p->uid,
                   I18N("social.market.unlist", "Unlist"));
    }
    sb_append(&sb, "</div>");
    char *html = sb_finish(&sb);
    sb_append(out, html);
    free(html);
}

// la(id): drop a card after a sale / unlisting (from the grid's list too: the cards after it
// move up, as the DOM removal reflows them)
static void remove_card(const char *page, const int uid) {
    if (!strcmp(page, g_market_sub)) {
        for (int p = 0; p < g_market_view_n; p++) {
            if (g_market_rows[g_market_view[p]].uid != uid) continue;
            memmove(&g_market_view[p], &g_market_view[p + 1], (size_t) (g_market_view_n - p - 1) * sizeof(int));
            g_market_view_n--;
            g_market_grid.count = g_market_view_n;
            vgrid_refresh(&g_market_grid);
            return;
        }
    }
    char id[64];
    snprintf(id, sizeof(id), "itemCard%s_%d", page, uid);
    const HtmlElem e = el(id);
    if (e) html_dom_remove(e);
}

// An inventory row's count changed (io_lir / io_qsr): the row (re-rendered cards) and its card. A listing (listed)
// also keeps the count for the row's popup (iïìiiïì, hub-10.0.7.js:5250).
static void inventory_count(const int uid, const int cnt, const int listed) {
    if (listed && cnt) count_override_set(uid, cnt);
    for (int i = 0; i < g_market_row_count; i++) {
        if (g_market_rows[i].uid == uid) g_market_rows[i].count = cnt;
    }
    char id[32], n[16];
    snprintf(id, sizeof(id), "invCnt%d", uid);
    snprintf(n, sizeof(n), "%d", cnt);
    if (cnt) set_html(id, n);
    else remove_card("inventory", uid);
}

// filterPopup / sortPopup (v7: genericPopup(..., "filterPop" | "sortPop"); the panel's state
// lives on in g_filter_state between popups, as v7 keeps the popup's innerHTML)
static int filter_weapon_shown(const int a) {
    return rules_weapon(a) && !rules_weapon(a)->melee && !rules_weapon(a)->equipment;
}

static void filter_state_save(const int sort) {
    HubFilterState *st = &g_filter_state[sort];
    const char *pre = sort ? "Sort" : "Filt";
    char id[32];
    for (int r = 0; r < g_viewer_rarity_count && r < 40; r++) {
        snprintf(id, sizeof(id), "rar%s%d", pre, r);
        if (el(id)) st->checked[0][r] = html_dom_get_checked(el(id));
    }
    for (int r = 0; r < rules_krunker_weapon_count() && r < 40; r++) {
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
    if (sort && el("nonTradable")) st->non_tradable = html_dom_get_checked(el("nonTradable"));
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
    // filterPopup / sortPopup (hub-10.0.7.js:4392-4448, 4518-4558)
    if (!sort) {
        sb_appendf(&sb, "<div class='filterPopHeader'>Market Search</div><div class='filterName'>%s</div><div id='orderBySel'><select id='sortFilt' onchange='retainSelected(this)'><option value='0'%s>%s</option><option value='1'%s>%s</option></select></div>",
                   I18N("social.market.sort", "Filter/Sort"), !strcmp(st->order, "0") ? " selected" : "", I18N("social.market.price.high", "Highest Price"),
                   strcmp(st->order, "0") ? " selected" : "", I18N("social.market.price.low", "Lowest Price"));
        sb_append(&sb, "<div class='filterName'>Season</div><div id='orderBySel'><select id='seaFilt' onchange='retainSelected(this)'><option value='0'>All Seasons</option>");
    } else {
        sb_append(&sb, "<div class='filterPopHeader'>Inventory Sort</div><div class='filterName'>Season</div><div id='orderBySel'><select id='seaSort' style='color: #353535;' onchange='retainSelected(this)'><option value='0'>All Seasons</option>");
    }
    for (int s2 = 1; s2 <= g_viewer_current_season; s2++) sb_appendf(&sb, "<option%s>%d</option>", atoi(st->season) == s2 ? " selected" : "", s2);
    sb_append(&sb, "</select></div>");
    if (sort) {
        sb_appendf(&sb, "<div class='filterName' style='position: relative;display: block;width: 100%%;margin-top: 5px;'>Only Show Missing<label class='filterSwitch'><input type='checkbox' id='onlyMissing' onchange='retainChecked(this)'%s><span class='filterSlider'></span></label></div>", st->missing ? " checked" : "");
        sb_appendf(&sb, "<div class='filterName' style='position: relative;display: block;width: 100%%;margin-top: 5px;'>Hide Non-Tradable Skins<label class='filterSwitch'><input type='checkbox' id='nonTradable' onchange='retainChecked(this)'%s><span class='filterSlider'></span></label></div>", st->non_tradable ? " checked" : "");
    }
    sb_appendf(&sb, "<input type='search' id='%s' onkeydown='if(window.event.keyCode==13) run%s()' placeholder='Keyword: Sniper, Plasma, Wings' value='", sort ? "invSortTxt" : "marketFiltTxt", sort ? "Sort" : "Filter");
    sb_append_esc(&sb, st->text);
    sb_appendf(&sb, "' /><div class='filterNav' id='%sNav_0' onmouseover='SOUND.play(`tick_0`,0.1)' onclick='switchSortFilter(%d, 0)' style='color:#faa332'>Rarities</div><div class='filterNav' id='%sNav_1' onmouseover='SOUND.play(`tick_0`,0.1)' onclick='switchSortFilter(%d, 1)'>Weapons</div><div class='filterNav' id='%sNav_2' onmouseover='SOUND.play(`tick_0`,0.1)' onclick='switchSortFilter(%d, 2)'>Cosmetics</div><div id='%sTog' onmouseover='SOUND.play(`tick_0`,0.1)' onclick='toggleSortFilters(%d)'>Toggle</div>",
               nav, sort, nav, sort, nav, sort, nav, sort);
    sb_appendf(&sb, "<div class='filterDiv' id='%sDiv_0'>", nav);
    for (int a = g_viewer_rarity_count - 1; a >= 0; --a) {
        sb_append(&sb, "<div class='raritySel'>");
        if (g_hub.rarity_anim && a == 6) sb_append(&sb, "<div class='colCub rainbowBG'></div>"); // store.rarities[6].animate
        else sb_appendf(&sb, "<div class='colCub' style='background-color:%s'></div>", g_viewer_rarities[a].color);
        sb_appendf(&sb, "%s<label class='filterSwitch'><input type='checkbox' onchange='retainChecked(this)' id='rar%s%d'%s><span class='filterSlider'></span></label></div>",
                   g_viewer_rarities[a].name, pre, a, st->checked[0][a] ? " checked" : "");
    }
    sb_appendf(&sb, "</div><div class='filterDiv' id='%sDiv_1' style='display:none;'>", nav);
    for (int a = 0; a < rules_krunker_weapon_count() && a < 40; ++a) {
        if (!filter_weapon_shown(a)) continue;
        sb_appendf(&sb, "<div class='wepSel' style='color:#fff'>%s<label class='filterSwitch'><input type='checkbox' onchange='retainChecked(this)' id='wep%s%d'%s><span class='filterSlider'></span></label></div>",
                   rules_weapon(a)->name, pre, a, st->checked[1][a] ? " checked" : "");
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
               sort ? "Sort" : "Filter", sort ? I18N("social.market.sort2", "Sort") : I18N("social.market.filter", "Search"), sort);
    char *html = sb_finish(&sb);
    generic_popup(html, sort ? "sortPop" : "filterPop", NULL);
    free(html);
}

static int g_filter_n, g_sort_n; // the latest request (stale replies are dropped)

// runFilter / runSort (hub-10.0.7.js:4449-4470, 4560-4590): io.send("fil", n, text, rarities{}, weapons{}, types{},
// season, order) / io.send("srt", n, text, rarities{}, weapons{}, types{}, season, onlyMissing, nonTradable)
static void market_run_filter(const int sort) {
    filter_state_save(sort);
    const HubFilterState *st = &g_filter_state[sort];
    cJSON *rar = cJSON_CreateObject(), *wep = cJSON_CreateObject(), *typ = cJSON_CreateObject();
    char key[8];
    for (int r = 0; r < g_viewer_rarity_count && r < 40; ++r) {
        snprintf(key, sizeof(key), "%d", r);
        if (st->checked[0][r]) cJSON_AddNumberToObject(rar, key, 1);
    }
    for (int r = 0; r < rules_krunker_weapon_count() && r < 40; ++r) {
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
    if (sort) {
        cJSON_AddItemToArray(f, cJSON_CreateBool(st->missing));
        cJSON_AddItemToArray(f, cJSON_CreateBool(st->non_tradable));
    } else {
        cJSON_AddItemToArray(f, cJSON_CreateString(st->order));
    }
    io_wait(I18N("social.market.searching", "Searching"));
    hub_send(f);
}

// resetSortFilter(sort): everything checked again, the search runs
static void market_reset_filter(const int sort) {
    HubFilterState *st = &g_filter_state[sort];
    for (int g = 0; g < 3; g++) for (int i = 0; i < 40; i++) st->checked[g][i] = 1;
    st->text[0] = 0;
    snprintf(st->season, sizeof(st->season), "0");
    snprintf(st->order, sizeof(st->order), "1");
    st->missing = st->non_tradable = 0;
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

static int hub_economy_frame(const char *label, const cJSON *frame);
static int hub_map_tools_frame(const char *label, const cJSON *frame); // the map page tools (below)

// The market / clan / activity reply frames (handle_reply_frame); 1 = handled
static int hub_market_frame(const char *label, const cJSON *frame) {
    if (hub_economy_frame(label, frame)) return 1;
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
            inventory_count((int) cJSON_GetNumberValue(a2), (int) jnum(a1, "cnt"), 1);
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
            inventory_count((int) jnum(a1, "i"), (int) jnum(a1, "c"), 0);
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
        // iïîiiíï(n, rows, readOnly) (hub-10.0.7.js:4596-4600): the sort reply's third value is io_0's readOnly
        cJSON *fr = cJSON_CreateArray();
        cJSON_AddItemToArray(fr, cJSON_CreateString("0"));
        cJSON_AddItemToArray(fr, cJSON_CreateString("market"));
        cJSON_AddItemToArray(fr, cJSON_CreateString(sort ? "inventory" : "market"));
        cJSON_AddItemToArray(fr, cJSON_CreateNull());
        cJSON_AddItemToArray(fr, cJSON_CreateBool(sort && cJSON_IsTrue(a3)));
        io_0("market", sort ? "inventory" : "market", a2, fr);
        cJSON_Delete(fr);
    } else {
        return 0;
    }
    return 1;
}

// The v7 item list (io_0 "items": the whole store table, client side)
static void items_page(const char *a, const char *k);

// iíïïiìi(title) (hub-10.0.7.js:7136-7141): document.title, "Krunker Hub" by default; a page's own title is
// "<name><suffix>", the name escaped as v10's (config's `am`: a backslash before \, " and ')
static void hub_title(const char *name, const int escape, const char *suffix) {
    if (!name) {
        app_set_page_title("Krunker Hub");
        return;
    }
    StrBuf sb = {0};
    for (const char *c = name; *c; c++) {
        if (escape && (*c == '\\' || *c == '"' || *c == '\'')) sb_append(&sb, "\\");
        sb_appendf(&sb, "%c", *c);
    }
    sb_append(&sb, suffix);
    char *title = sb_finish(&sb);
    app_set_page_title(title ? title : "Krunker Hub");
    free(title);
}

// io_0(e, a, t, ...): one page's data arrived
static void io_0(const char *e, const char *a, const cJSON *t, const cJSON *extra) {
    char key[HUB_KEY_MAX * 2];
    snprintf(key, sizeof(key), "%s_%s", e ? e : "null", a && *a ? a : "null");
    if (g_hub.has_s && strcmp(g_hub.S, key) != 0) return;
    if (!e) return;
    const int has_t = t && !cJSON_IsNull(t);
    g_count_over_n = 0; // iïìiiïì = {} (hub-10.0.7.js:2721)
    hub_title(NULL, 0, NULL); // the page's own title below
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
        // ?openPubMo: the Publish Mod popup, at once when signed in, else at the login (hub-10.0.7.js:3125-3129, 2188-2190)
        char pm[8];
        if (query_param(g_hub.query, "openPubMo", pm, sizeof(pm)) && g_hub.logged_in) hub_mod_popup(NULL);
        else g_hub.pub_mo_pending = 1;
        hub_title("Mods", 0, " - Krunker Hub");
    } else if (!strcmp(e, "mod") || !strcmp(e, "map")) {
        const int is_mod = !strcmp(e, "mod");
        side_boxes();
        set_display(is_mod ? "modHolder" : "mapHolder", "block");
        hub_render_creation(is_mod, has_t ? t : NULL);
        const char *cname = has_t && cJSON_IsObject(t) ? jstr(t, is_mod ? "mod_name" : "map_name") : NULL;
        if (cname) hub_title(cname, 1, " - Krunker Hub");
    } else if (!strcmp(e, "maps")) {
        set_display("mapsHolder", "block");
        list_update(0);
        hub_title("Games", 0, " - Krunker Hub");
    } else if (!strcmp(e, "leaders")) {
        side_boxes();
        set_display("leaderHolder", "block");
        hub_render_leaders(a, has_t ? t : NULL); // no leaderboard rows offline: an empty list
        if (has_t) hub_title("Leaderboards", 0, " - Krunker Hub");
    } else if (!strcmp(e, "market")) {
        set_display("marketHolder", "block");
        set_display("invSortB", a && !strcmp(a, "inventory") ? "table-cell" : "none");
        set_display("marketSearchB", a && !strcmp(a, "market") ? "table-cell" : "none");
        set_display("invStats", a && (!strcmp(a, "inventory") || !strcmp(a, "sales")) ? "table-cell" : "none");
        // no listings / inventory for a guest: social.market.login (K is null)
        if (g_hub.brain) hub_render_market(a, has_t ? t : NULL, cJSON_IsTrue(cJSON_GetArrayItem(extra, 4)), cJSON_GetArrayItem(extra, 8));
        else set_market_note("marketList", STR_MARKET_LOGIN);
    } else if (!strcmp(e, "items")) {
        items_page(a, g_hub.k);
        hub_title("Skin List", 0, " - Krunker Hub");
    } else if (!strcmp(e, "itemsales") && g_hub.brain) {
        hub_render_itemsales(has_t ? t : NULL, cJSON_GetArrayItem(extra, 4));
    } else if (!strcmp(e, "itemsales")) {
        side_boxes();
        set_display("itemsalesList", "none");
        if (!has_t) set_market_note("itemsalesHolder", STR_MARKET_NONE);
        set_display("itemsalesHolder", "block");
    } else if (!strcmp(e, "listing")) {
        side_boxes();
        if (g_hub.brain && has_t) hub_render_listing(t); // kn_brain: the open listing ("listing" page)
        else set_market_note("listingHolder", STR_MARKET_NONE);
        set_display("listingHolder", "block");
    } else if (!strcmp(e, "clan")) {
        side_boxes();
        set_display("clanHolder", "block");
        if (g_hub.brain && has_t && cJSON_IsObject(t)) hub_render_clan(t);
        if (has_t && cJSON_IsObject(t) && jstr(t, "clan_name")) {
            char clan[96];
            snprintf(clan, sizeof(clan), "[%s", jstr(t, "clan_name"));
            hub_title(clan, 0, "] Clan - Krunker Hub");
        }
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
        const AccountInfo *me = g_hub.logged_in ? active_account() : NULL;
        if (me) set_pfp(el("postProf"), me->id, me->premium > 0, "./img/player_pfp.png");
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
// Profile pictures (pfp_url: <userAssetsHost>/p<id>/profile.png) end on ./img/player_pfp.png when they fail.
static cJSON *g_chat_list; // we: the open thread, newest first (owned)
static int g_chat_peer;    // ve.id
static char g_chat_peer_name[64]; // ve.name: startChat's name, the incoming lines' sender
static int g_chat_pending; // he != null
static char g_chat_pending_txt[320];

// The signed-in chat sidebar (hub-10.0.7.js:2201-2207): the Friends button, then the friends' bubbles ("chtb", the
// brain's mutual follows with presence). The friends list is kept for showFriends / removeFriend.
static void io_msg(int from, const char *text); // ["msg", fromId, text] (below)
static void profile_reports(const cJSON *rows);   // ["rps", rows]: the profile's Reports tab (below)

static void chat_sidebar(const AccountInfo *acc) {
    (void) acc;
    set_display("chatHolder", "block");
    set_html("chatScroller", "<div class='chatBlobI' data-tip='Friends' onclick='showFriends()' onmouseover='createTip(this, 1, `view friends list`)' "
                             "onmouseout='cancelTip(1)'><div class='chatBlobIcn'><span class='material-icons' style='color:rgba(255,255,255,0.9);"
                             "font-size:38px;line-height:57px;'>group</span></div></div><div class='chatListSep'></div><div id='chatBubHold'>"
                             "<div class='lds-ring' style='margin-top:10px;opacity:0.5'><div></div><div></div><div></div><div></div></div></div>");
    cJSON *f = cJSON_CreateArray();
    cJSON_AddItemToArray(f, cJSON_CreateString("chtb"));
    hub_send(f);
}

// a friend's status line: "In game" (sm) / "Online" / "Offline"
static const char *friend_status(const cJSON *r) {
    if (!jnum(r, "st")) return "Offline";
    const char *sm = jstr(r, "sm");
    return sm && *sm ? sm : "Online";
}

// startChat(fid, 0, name, picture, 1, `sm`, `gid`) for a friend row (hub-10.0.7.js:2340, 5337): the picture is a
// subscriber's, else ""; the status is the row's raw sm, "undefined" without one (the header then says Offline, also
// for an online friend without a status text, as in v10)
static void friend_chat_call(StrBuf *sb, const cJSON *r) {
    const char *fna = jstr(r, "fna") ? jstr(r, "fna") : "", *gid = jstr(r, "gid") ? jstr(r, "gid") : "";
    const char *sm = jstr(r, "sm");
    sb_appendf(sb, "startChat(%d,0,&quot;", (int) jnum(r, "fid"));
    sb_append_js_escape(sb, fna);
    sb_append(sb, "&quot;,&quot;");
    sb_pfp(sb, jnum(r, "fid"), jnum(r, "sub") != 0, "");
    sb_append(sb, "&quot;,1,`");
    sb_append_js_escape(sb, sm ? sm : "undefined");
    sb_append(sb, "`,`");
    sb_append_js_escape(sb, gid);
    sb_append(sb, "`)");
}

static int friend_order(const cJSON *a, const cJSON *b) { return (int) jnum(b, "st") - (int) jnum(a, "st"); }

// io_xhtb(rows) (hub-10.0.7.js:2321-2346): online friends first, a status dot, the unread count
static void io_xhtb(const cJSON *rows) {
    const cJSON *r;
    if (cJSON_GetArraySize(rows)) {
        cJSON_Delete(g_friends);
        g_friends = cJSON_CreateArray();
    }
    if (cJSON_GetArraySize(rows)) cJSON_ArrayForEach(r, rows) {
        if (!cJSON_IsObject(r)) continue;
        int at = cJSON_GetArraySize(g_friends); // a stable sort: after every friend at least as online
        while (at > 0 && friend_order(cJSON_GetArrayItem(g_friends, at - 1), r) > 0) at--;
        cJSON_InsertItemInArray(g_friends, at, cJSON_Duplicate(r, 1));
    }
    if (!el("chatBubHold")) return;
    StrBuf sb = {0};
    const cJSON *shown = cJSON_GetArraySize(rows) ? g_friends : rows;
    cJSON_ArrayForEach(r, shown) {
        const int cht = (int) jnum(r, "cht"), fid = (int) jnum(r, "fid"), on = jnum(r, "st") != 0;
        const char *fna = jstr(r, "fna") ? jstr(r, "fna") : "";
        sb_appendf(&sb, "<div class='chatBlobI' style='opacity:%s' data-tip='", cht > 0 || on ? "1" : "0.7");
        sb_append_esc(&sb, fna);
        sb_append(&sb, "' onmouseover='createTip(this, 1, `");
        sb_append_js_escape(&sb, friend_status(r));
        sb_appendf(&sb, "`, %d)' onmouseout='cancelTip(1)'><div class='statusBlob' style='background:%s'></div>", on, on ? "#27ab32;" : "#7f7f7f");
        sb_appendf(&sb, "<div class='msgCount' id='msgCnt%d'%s>%d</div><img class='chatBlobImg' onclick='", fid, cht ? "" : " style='display: none;'", cht);
        friend_chat_call(&sb, r);
        sb_append(&sb, "'onerror='this.onerror=null;this.src=\"./img/player_pfp.png\";' src='");
        sb_pfp(&sb, fid, jnum(r, "sub") != 0, "");
        sb_append(&sb, "'/></div>");
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
            readable_time(ago, sizeof(ago), tim); // getReadableTime(tim): an age in ms (io_chtl)
            sb_append(&sb, "<div class='chatItemH' style='margin-bottom:12px'><img src='");
            sb_pfp(&sb, jnum(m, "fid"), 1, "");
            sb_append(&sb, "' class='chatItPFP'  onerror='this.onerror=null;this.src=\"./img/player_pfp.png\";' /><div class='chatItInfo'><b style='color:#fff'>");
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
    cJSON *m;
    cJSON_ArrayForEach(m, g_chat_list) { // kn_brain's tim is the line's epoch ms: v10's is its age
        cJSON *t = cJSON_GetObjectItemCaseSensitive(m, "tim");
        if (cJSON_IsNumber(t) && t->valuedouble > 1e12) cJSON_SetNumberValue(t, fmax(0, now_epoch_ms() - t->valuedouble));
    }
    if (cJSON_GetArraySize(g_chat_list)) chat_update_list();
    else set_html("chatBoxFill", "");
}

// io_chf(ok, err): the sent message (he), or a system line
static void io_chf(const int ok, const char *err) {
    const AccountInfo *acc = active_account();
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

static void login_modal_close(void);
static int login_modal_message(const char *msg);

// io_a(error, id, username, data, token): the login reply (account_ui.c keeps the account
// itself: storage keys, level, KR). A guest game server refuses every login.

// The login's page updates (hub-10.0.7.js:2173-2216) when the page was drawn signed out: the profile's Reports, Trade
// and Follow buttons, the deferred ?autoTrade / ?autoGift / ?openPubMo, the staff controls (.canFlag ... .canChatBan)
static void hub_after_login(const AccountInfo *acc) {
    const int staff = acc->is_dev || acc->is_mod;
    if (!strcmp(g_hub.v, "profile") && g_hub.profile_name[0]) {
        if (staff) {
            profile_chat_ban_fetch();
            cJSON *f = cJSON_CreateArray();
            cJSON_AddItemToArray(f, cJSON_CreateString("grp"));
            cJSON_AddItemToArray(f, cJSON_CreateString(g_hub.profile_name));
            hub_send(f);
        }
        if (acc->level >= TRADE_MIN_LVL && g_hub.profile_level >= TRADE_MIN_LVL) {
            set_display("tradeBtn", "inline-block");
            char v[8];
            if (g_auto_trade_pending && query_param(g_hub.query, "autoTrade", v, sizeof(v))) h_trade_popup(NULL, NULL, NULL, 0);
        }
        if (strcmp(acc->name, g_hub.profile_name)) set_display("followBtn", "inline-block");
    }
    if (g_auto_gift_pending) auto_gift();
    char pm[8];
    if (g_hub.pub_mo_pending && query_param(g_hub.query, "openPubMo", pm, sizeof(pm))) hub_mod_popup(NULL);
    g_auto_gift_pending = g_auto_trade_pending = g_hub.pub_mo_pending = 0;
    static const struct { const char *sel; int need; } gates[] = {
        {".canDeleteCreations", 1}, {".canFeatureCreations", 1}, {".canFlag", 2}, {".canVerify", 1}, {".canBadge", 1}, {".canChatBan", 2}};
    for (size_t g = 0; g < sizeof(gates) / sizeof(gates[0]); g++) {
        HtmlElem found[64];
        const int n = html_dom_query_all(0, gates[g].sel, found, 64);
        const int on = gates[g].need == 1 ? acc->is_dev : staff;
        for (int i = 0; i < n; i++) html_dom_set_class(found[i], "hide", !on);
    }
}

static void io_a(const char *error) {
    // the page request hub_connected held back: v10's socket is signed in from its URL (pat=), so
    // its first page request already answers as the account; the brain's session signs in with "a"
    const int page = g_hub.page_after_login && strcmp(g_hub.v, "feed"); // the feed requests below
    g_hub.page_after_login = 0;
    if (!error) {
        // success: acc = new Account(...)
        const AccountInfo *acc = active_account();
        if (!acc) {
            if (page) request_page();
            return;
        }
        if (!g_hub.fe) io_po(1, NULL);
        g_hub.fe = 0;
        g_hub.logged_in = 1;
        login_modal_close(); // v10: the LoginModal pops on success
        set_display("profileLogin", "none");
        const HtmlElem img = el("profileImg");
        if (img) { // a premium account's picture, else its class icon
            char src[64];
            const cJSON *c = acc->stats ? cJSON_GetObjectItem(acc->stats, "c") : NULL;
            snprintf(src, sizeof(src), "./textures/classes/icon_%d.png", cJSON_IsNumber(c) ? c->valueint : 0);
            set_pfp(img, acc->id, acc->premium > 0, src);
            html_dom_set_display(img, "inline-block");
        }
        char kr[32];
        comma_number(kr, sizeof(kr), acc->funds);
        StrBuf sb = {0};
        sb_append_esc(&sb, acc->name);
        sb_appendf(&sb, "<div style='height:4px'></div><div id='profileKR' style='cursor:pointer' title='KR History' onclick='krHistory()'>%s<span style='color:#fff'> KR</span></div>", kr);
        char *html = sb_finish(&sb);
        set_html("profileName", html);
        free(html);
        if (!strcmp(g_hub.v, "feed")) {
            set_display("postEntryC", "inline-block");
            request_page();
        }
        if (page) request_page();
        else hub_after_login(acc);
        chat_sidebar(acc);
        return;
    }
    g_hub.logged_in = 0;
    cJSON_Delete(g_friends);
    g_friends = NULL;
    set_html("profileName", "");
    set_display("profileLogin", "inline-block");
    set_display("chatHolder", "none");
    if (!strcmp(g_hub.v, "feed") || page) request_page(); // Qa()
    if (!g_hub.fe && !login_modal_message(error)) io_po(0, error); // v10: the modal's message line
    g_hub.fe = 0;
}

static void handle_reply_frame(const cJSON *frame);

// followers / following (v10 viewFollowers / viewFollowing): the popup's rows and state
static struct FollowState {
    int open, following, own; // own: the list is the viewer's (its id is the account's)
    char title[128];
    double count;
    cJSON *rows;      // the current popup's rows (the list deleteFollower indexes)
} g_follows;
static void follows_reply(const cJSON *rows, const char *name, double id, int following);

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
    const int kio = account_backend() == ACCOUNT_BACKEND_KRUNKER;
    char query[480], enc[256];
    int n = 0;
    const cJSON *p;
    cJSON_ArrayForEach(p, params) {
        if (cJSON_IsString(p)) backend_url_encode(enc, sizeof(enc), p->valuestring);
        else js_num(enc, sizeof(enc), cJSON_GetNumberValue(p));
        n += snprintf(query + n, sizeof(query) - (size_t) n, "%s=%s&", p->string, enc);
        if ((size_t) n >= sizeof(query)) return;
    }
    const AccountInfo *acc = g_hub.logged_in ? active_account() : NULL;
    if (acc && (!kio || acc->id > 0)) snprintf(query + n, sizeof(query) - (size_t) n, "accountId=%d", acc->id);
    char url[640];
    char *copy = strdup(path);
    if (kio) {
        // v10's listMaps / searchMap / listMods (v10:131895-131941) are the data client's webhooks (api.krunker.io);
        // a path with no webhook is a brain-only route and is not asked of krunker.io
        const char *group = krunker_kio_webhook_group(path, cJSON_GetStringValue(cJSON_GetObjectItem(params, "type")));
        char base[256];
        krunker_host_url(KRUNKER_HOST_API, "", base, sizeof(base));
        int sent = 0;
        if (group && copy && krunker_kio_webhook_url(url, sizeof(url), base, group, query))
            sent = backend_http_get_remote(url, KRUNKER_KIO_READ_TIMEOUT_MS, hub_brain_api_done, copy) != 0;
        if (!sent) free(copy);
        return;
    }
    snprintf(url, sizeof(url), "/api%s?%s", path, query);
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

// The staff and report results (the brain's flag: true | null): the success popup and the page
// again, so the button shows the new state (v10's verifyUser / badgeUser / flagUser callbacks)
static void hub_action_result(const int ok, const char *text) {
    if (!ok) {
        io_po(0, NULL);
        return;
    }
    io_po(1, text);
    request_page();
}

static void handle_reply_frame(const cJSON *frame) {
    const char *label = cJSON_GetStringValue(cJSON_GetArrayItem(frame, 0));
    if (!label) return;
    if (!strcmp(label, "verr") || !strcmp(label, "fgr") || !strcmp(label, "sbdgr") || !strcmp(label, "crtr") || !strcmp(label, "rptr")) {
        hub_action_result(cJSON_IsTrue(cJSON_GetArrayItem(frame, 1)), "SUCCESS");
        return;
    }
    if (!strcmp(label, "chs")) return; // kn_brain's chat presence: v10's header keeps startChat's status (hub-10.0.7.js:2249)
    if (!strcmp(label, "rps") && cJSON_IsArray(cJSON_GetArrayItem(frame, 1))) { // the profile's reports (staff)
        profile_reports(cJSON_GetArrayItem(frame, 1));
        return;
    }
    if (!strcmp(label, "msg")) {
        io_msg((int) cJSON_GetNumberValue(cJSON_GetArrayItem(frame, 1)), cJSON_GetStringValue(cJSON_GetArrayItem(frame, 2)));
        return;
    }
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
    } else if (!strcmp(label, "fwsr") || !strcmp(label, "fwr")) {
        follows_reply(cJSON_GetArrayItem(frame, 1), cJSON_GetStringValue(cJSON_GetArrayItem(frame, 2)),
                      cJSON_GetNumberValue(cJSON_GetArrayItem(frame, 3)), !strcmp(label, "fwr"));
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
    else if (g_hub.brain && hub_map_tools_frame(label, frame)) {
        // the map page tools (glbR / sdR / ddR / mfr / upMp / edMp)
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
                append_post(&sb, post, 1, 0);
                const AccountInfo *me = g_hub.logged_in ? active_account() : NULL;
                sb_append(&sb, "<div class='replyHolder'><img src='");
                sb_pfp(&sb, me ? me->id : 0, me && me->premium, "./img/player_pfp.png");
                sb_appendf(&sb, "' class='replyPfp'><input class='replyInput' placeholder='Reply' id='replyInput'></input><div class='replyBtn' onclick='postReply(%d)' id='replyBtn'>Reply</div></div>", (int) jnum(post, "pid"));
                const cJSON *c;
                sb_append(&sb, "<div style='margin-top:10px' id='comHolderH'>");
                cJSON_ArrayForEach(c, cJSON_GetObjectItem(res, "c")) {
                    sb_append(&sb, "<div class='commHolder'><img class='replyPfp' onerror='this.onerror=null;this.src=\"./img/player_pfp.png\";'src='");
                    sb_pfp(&sb, jnum(c, "pli"), jnum(c, "prm") > 0, "./img/player_pfp.png");
                    sb_append(&sb, "' /><div class='comTxtHolder'><div style='margin-bottom:6px'>");
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
    const int new_tab = name && !strcmp(name, "openURL") ? !arg_n(args, argc, 1, 0)
                      : name && strcmp(name, "openLink") != 0 ? 1 : (target && !strcmp(target, "_blank"));
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

// ------------------------------------------------------------------ login (v10's LoginModal)
// Krunker v10's hub loginPopup() pushes the Svelte LoginModal ({canRegister: false}) on the view
// stack (#viewStack; webcrack social/renamed.js:4375, shared/index/renamed.js:11493-11870): an
// Email / Username field with a "(use ... instead)" toggle (email first unless krunker_username is
// saved), the password field (hidden in email mode until typed; v10 then offers a sign-in code),
// the Login button and a message line. Markup = the component templates (index:11466-11479) in the
// popup frame (index:8534, 800 px) of the view stack (index:12537); CSS = css/hub-svelte.css
// (tools/ui-import/v10_hub_editor.py) + static/index.css (io-*).
// Intentional differences: KrunkNative's account server signs in by password over the hub
// socket ("a", as v7.1.4), not FRVR's REST auth: the Email or Username field goes as the name
// (kn_brain signs in to the account holding an email), and the email-code sign-in ("Send Code")
// is not offered.
static struct {
    int open;
    int email;          // the email field is shown (v10: no saved krunker_username)
    char msg[160];
} g_login;

static void login_modal_values(char *name, size_t nsz, char *pass, size_t psz) {
    const HtmlElem n = el(g_login.email ? "accEmail" : "accName"), p = el("accPass");
    if (name && nsz) name[0] = 0;
    if (pass && psz) pass[0] = 0;
    if (n && name) html_dom_get_value(n, name, nsz);
    if (p && pass) html_dom_get_value(p, pass, psz);
}

static void login_modal_render(const char *name, const char *pass) {
    StrBuf sb = {0};
    sb_append(&sb, "<div class=\"view-stack svelte-sdh4ve\"><div class=\"view-stack-overlay svelte-sdh4ve\" onclick=\"knLoginOverlay()\">"
                   "<div class=\"view-wrapper svelte-sdh4ve\" style=\"display: block\"><div class=\"popup-container svelte-1wz7ymp\">"
                   "<div class=\"popup svelte-1wz7ymp\" style=\"width: 800px;\"><div class=\"io-popup-container\"><div class=\"io-popup-header\">Login to Krunker</div> "
                   "<div class=\"io-popup-content\"><div class=\"io-card\"><form data-priority=\"true\"><div class=\"io-input-container svelte-16z1lxi\">");
    const int pass_typed = pass && *pass;
    if (g_login.email) {
        sb_append(&sb, "<div class=\"label-row\"><label for=\"accEmail\" class=\"input-label\">Email</label> <button type=\"button\" class=\"auth-toggle-btn svelte-16z1lxi\" onclick=\"knLoginToggle()\">(use username instead)</button></div> "
                       "<input id=\"accEmail\" type=\"email\" class=\"io-input-field svelte-16z1lxi\" autocomplete=\"email\" placeholder=\"Enter Email\" onkeyup=\"knLoginKey()\" value=\"");
    } else {
        sb_append(&sb, "<div class=\"label-row\"><label for=\"accName\" class=\"input-label\">Username</label> <button type=\"button\" class=\"auth-toggle-btn svelte-16z1lxi\" onclick=\"knLoginToggle()\">(use email instead)</button></div> "
                       "<input id=\"accName\" type=\"text\" class=\"io-input-field svelte-16z1lxi\" autocomplete=\"username\" placeholder=\"Enter Username\" onkeyup=\"knLoginKey()\" value=\"");
    }
    sb_append_esc(&sb, name ? name : "");
    sb_append(&sb, "\"> ");
    // the password label row: username mode, or once a password is typed (index:11752)
    if (!g_login.email || pass_typed)
        sb_append(&sb, "<div class=\"label-row\" style=\"margin-top:15px\"><label for=\"accPass\" class=\"input-label\">Password</label> <button type=\"button\" class=\"auth-toggle-btn svelte-16z1lxi\" onclick=\"knLoginForgot()\">Forgot Password?</button></div>");
    sb_appendf(&sb, "<input id=\"accPass\" type=\"password\" autocomplete=\"current-password\" placeholder=\"Enter Password\" class=\"io-input-field %ssvelte-16z1lxi\" onkeyup=\"knLoginKey()\" value=\"",
               g_login.email && !pass_typed ? "hidden-password " : "");
    sb_append_esc(&sb, pass ? pass : "");
    sb_append(&sb, "\"> <button type=\"submit\" class=\"io-button io-button--accept svelte-16z1lxi\" onclick=\"loginAcc()\">Login</button></div></form></div>    ");
    if (g_login.msg[0]) {
        sb_append(&sb, "<div class=\"io-message\">");
        sb_append_esc(&sb, g_login.msg);
        sb_append(&sb, "</div>");
    }
    sb_append(&sb, "</div></div></div></div></div></div></div>");
    char *html = sb_finish(&sb);
    set_html("viewStack", html);
    free(html);
}

static void login_modal_close(void) {
    if (!g_login.open) return;
    g_login.open = 0;
    g_login.msg[0] = 0;
    set_html("viewStack", "");
}

// 0 when the modal is not open
static int login_modal_message(const char *msg) {
    if (!g_login.open) return 0;
    char name[128], pass[128];
    login_modal_values(name, sizeof(name), pass, sizeof(pass));
    snprintf(g_login.msg, sizeof(g_login.msg), "%s", msg ? msg : "");
    login_modal_render(name, pass);
    return 1;
}

// loginPopup(): the LoginModal when no account. On an official krunker.io server the sign-in is krunker.io's: the login
// card of the krunker.io account (v10_login.c, loginOrRegister), never Box Office's modal (backend_ui.h account_backend).
H(h_login_popup) {
    UNUSED;
    if (g_hub.logged_in) return 1;
    if (account_backend() == ACCOUNT_BACKEND_KRUNKER) {
        html_ui_dispatch("loginOrRegister", NULL, NULL, 0);
        return 1;
    }
    char saved[128] = "";
    if (!account_saved_name(saved, sizeof(saved))) saved[0] = 0;
    clear_pops();
    g_login.open = 1;
    g_login.msg[0] = 0;
    g_login.email = saved[0] == 0; // index:11506: email first for a player with no saved name
    login_modal_render(saved, "");
    return 1;
}

H(h_login_toggle) {
    UNUSED;
    char name[128], pass[128];
    login_modal_values(name, sizeof(name), pass, sizeof(pass));
    g_login.email = !g_login.email;
    g_login.msg[0] = 0;
    login_modal_render(strchr(name, '@') && !g_login.email ? "" : name, pass);
    return 1;
}

H(h_login_forgot) {
    UNUSED;
    login_modal_message("For a lost password, contact the server's support team.");
    return 1;
}

// a click on the dimmed overlay (not inside the popup) dismisses the view (index:12531)
H(h_login_overlay) {
    (void) user; (void) args; (void) argc;
    const HtmlElem hit = ev ? html_dom_element_at(ev->x, ev->y) : 0;
    if (hit && html_dom_has_class(hit, "view-stack-overlay")) login_modal_close();
    return 1;
}

H(h_login_acc) {
    UNUSED;
    if (account_backend() == ACCOUNT_BACKEND_KRUNKER) { // this modal is Box Office's: nothing is sent for krunker.io
        login_modal_close();
        return 1;
    }
    char name[256] = "", pass[128] = "";
    login_modal_values(name, sizeof(name), pass, sizeof(pass));
    if (!name[0] || !pass[0]) {
        // generic.auth.fillAllFields; in email mode v10 offers the sign-in code first (not ported)
        if (g_login.email && name[0] && !pass[0]) {
            login_modal_message("Enter your password.");
            HtmlElem p = el("accPass");
            if (p) html_dom_focus(p);
        } else {
            login_modal_message("Please fill in all fields");
        }
        return 1;
    }
    login_modal_message(I18N("generic.wait", "Please wait..."));
    cJSON *creds = cJSON_CreateArray();
    cJSON_AddItemToArray(creds, cJSON_CreateString(name));
    cJSON_AddItemToArray(creds, cJSON_CreateString(pass));
    cJSON_AddItemToArray(creds, cJSON_CreateNull());
    cJSON *f = cJSON_CreateArray();
    cJSON_AddItemToArray(f, cJSON_CreateString("a"));
    cJSON_AddItemToArray(f, cJSON_CreateNumber(1));
    cJSON_AddItemToArray(f, creds);
    // the captcha token (v7.1.4's hub sent true): Box Office's ALTCHA payload, solved first (kn_captcha.h)
    kn_captcha_send_frame("login", f, 3, hub_send_login);
    memset(pass, 0, sizeof(pass));
    return 1;
}

H(h_login_key) {
    if (ev && html_key_is_enter(ev->key)) return h_login_acc(user, ev, args, argc);
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

// showFriends() (hub-10.0.7.js:5324-5343): every friend, with Message, Join (in a game) and Remove Friend
H(h_show_friends) {
    UNUSED;
    if (!g_hub.logged_in || !g_friends) return 1;
    const int n = cJSON_GetArraySize(g_friends);
    StrBuf sb = {0};
    char total[32];
    comma_number(total, sizeof(total), n);
    sb_appendf(&sb, "<div style='font-size:22px;margin-bottom:20px;color:#fff;'>Your Friends<div style='float:right;color:rgba(255,255,255,0.8);'>%s Total</div></div>", total);
    for (int i = 0; i < n; i++) {
        const cJSON *r = cJSON_GetArrayItem(g_friends, i);
        const char *fna = jstr(r, "fna") ? jstr(r, "fna") : "", *gid = jstr(r, "gid");
        const int on = jnum(r, "st") != 0;
        sb_append(&sb, "<div class='friendBHold'><div style='text-align:left;'><img class='folPfp' onerror='this.onerror=null;this.src=\"./img/player_pfp.png\";'  "
                       "style='width:60px;height:60px;' src='");
        sb_pfp(&sb, jnum(r, "fid"), jnum(r, "sub") != 0, "");
        sb_append(&sb, "'/><div class='folInfHldr'>");
        if (jnum(r, "sub")) sb_append(&sb, "<i class='material-icons' style='color:#FBC02D;margin-top:-8px;font-size:28px;vertical-align:middle;'>beenhere</i>");
        if (jnum(r, "plf")) sb_append(&sb, "<i class='material-icons' style='color:#40C4FF;margin-top:-8px;margin-right: 4px;font-size:28px;vertical-align:middle;'>check_circle</i>");
        sb_append(&sb, "<a target='_blank' href='/social.html?p=profile&q=");
        sb_append_esc(&sb, fna);
        sb_append(&sb, "' class='lName' style='color:rgba(255,255,255,0.9)'>");
        sb_append_esc(&sb, fna);
        sb_appendf(&sb, "</a><div style='color:rgba(255,255,255,0.4);font-size:16px;margin-left:2px;'><div class='onStatus' style='background:%s'></div>",
                   on ? "#27ab32" : "#7f7f7f");
        sb_append_esc(&sb, friend_status(r));
        sb_append(&sb, "</div></div><div class='friendHrgt'><div class='dmFrndBtn' onclick='");
        friend_chat_call(&sb, r);
        sb_append(&sb, "'><span class='material-icons' style='color:#fff;font-size:47px;vertical-align:middle;margin-bottom:-16px;'>chat_bubble_outline</span></div>");
        if (gid && *gid) {
            sb_append(&sb, "<div class='button buttonB' style='margin-right:15px;padding:10px;transform:translateY(7px);display:inline-block;'onclick='openURL(`/?game=");
            sb_append_js_escape(&sb, gid);
            sb_append(&sb, "`,1)'>Join</div>");
        }
        sb_appendf(&sb, "<div class='button buttonR' style='padding: 10px;transform:translateY(7px);display:inline-block;vertical-align: unset;'"
                        "onclick='removeFriend(%d)'>Remove Friend</div></div></div></div>", i);
    }
    if (!n) sb_append(&sb, "You have no friends");
    char *html = sb_finish(&sb);
    generic_popup(html, "followPop", NULL);
    free(html);
    return 1;
}

// removeFriend(i) (hub-10.0.7.js:5345-5352): unfollow (["f", name, 0]), drop the row, show the list again
H(h_remove_friend) {
    const int i = (int) arg_n(args, argc, 0, -1);
    const cJSON *r = g_friends ? cJSON_GetArrayItem(g_friends, i) : NULL;
    const char *fna = r ? jstr(r, "fna") : NULL;
    if (!fna) return 1;
    cJSON *f = cJSON_CreateArray();
    cJSON_AddItemToArray(f, cJSON_CreateString("f"));
    cJSON_AddItemToArray(f, cJSON_CreateString(fna));
    cJSON_AddItemToArray(f, cJSON_CreateNumber(0));
    hub_send(f);
    cJSON_DeleteItemFromArray(g_friends, i);
    if (cJSON_GetArraySize(g_friends)) {
        cJSON *rows = cJSON_Duplicate(g_friends, 1);
        io_xhtb(rows);
        cJSON_Delete(rows);
    } else if (el("chatBubHold")) set_html("chatBubHold", "<div class='chatBlobI'><img class='chatBlobImg'/></div>");
    return h_show_friends(user, ev, args, argc);
}

// createTip(el, 1, sub, online) / cancelTip(1) (hub-10.0.7.js:6862-6878, 6885-6890): the chat sidebar's tooltip,
// right of the bubble. The item-card form (createTip(el) on a market card) is hub_item_tip.
H(h_create_tip) {
    UNUSED;
    if (!ev || !ev->target) return 1;
    if (!(int) arg_n(args, argc, 1, 0)) { // createTip(el): an item card's tooltip (hub-10.0.7.js:6879-6884; ui_tooltip.c)
        ui_tooltip_item(ev->target);
        return 1;
    }
    const HtmlElem base = el("uiBase");
    if (!base) return 1;
    for (HtmlElem old; (old = html_dom_query(base, ".tooltip2"));) html_dom_remove(old);
    char tip[200] = "";
    html_dom_get_attr(ev->target, "data-tip", tip, sizeof(tip));
    const char *sub = argc > 2 ? arg_s(ev, args, argc, 2) : "";
    StrBuf sb = {0};
    sb_append(&sb, "<div class='tooltip2'><div class='tooltipSub'>");
    if (argc > 3) sb_appendf(&sb, "<div class='onStatus' style='background:%s;margin-top:-8px;vertical-align:middle;'></div>", arg_n(args, argc, 3, 0) ? "#27ab32;" : "#7f7f7f;");
    sb_append_esc(&sb, tip);
    sb_append(&sb, "<div class='tooltipSubS'>");
    sb_append_esc(&sb, sub ? sub : "");
    sb_append(&sb, "</div></div></div>");
    char *html = sb_finish(&sb);
    html_dom_insert_html(base, HTML_INSERT_AFTER_BEGIN, html);
    free(html);
    HtmlRect r, t;
    const HtmlElem tipel = html_dom_query(base, ".tooltip2");
    if (tipel && html_dom_get_rect(ev->target, &r) && html_dom_get_rect(tipel, &t)) {
        char style[96];
        snprintf(style, sizeof(style), "top:%.0fpx;left:%.0fpx;", r.y + r.h / 2 - t.h / 2, r.x + r.w + 15);
        html_dom_set_attr(tipel, "style", style);
    }
    return 1;
}

// createBadgeTip(el) (hub-10.0.7.js:6879-6885): the badge of el's data-index, its name in gold over its description,
// through the page's tooltip helper (iììïìiî, ui_tooltip_show)
H(h_create_badge_tip) {
    UNUSED;
    char idx[16] = "";
    if (!ev || !ev->target || html_dom_get_attr(ev->target, "data-index", idx, sizeof(idx)) < 0) return 1;
    const BadgeDef *b = badge_get(idx[0] ? atoi(idx) : -1);
    if (!b) return 1;
    StrBuf sb = {0};
    sb_append(&sb, "<div class='tooltipSub'><span style='color:#FBC02D'>");
    sb_append_esc(&sb, b->name);
    sb_append(&sb, "</span></div><div class='tooltipSub' style='font-style:italic;font-size:8px;max-width: 300px;'>");
    sb_append_esc(&sb, b->desc);
    sb_append(&sb, "</div>");
    char *html = sb_finish(&sb);
    ui_tooltip_show(ev->target, html);
    free(html);
    return 1;
}

H(h_cancel_tip) {
    UNUSED;
    if (!(int) arg_n(args, argc, 0, 0)) {
        ui_tooltip_cancel();
        return 1;
    }
    const HtmlElem base = el("uiBase");
    const char *sel = (int) arg_n(args, argc, 0, 0) ? ".tooltip2" : ".tooltip";
    for (HtmlElem old; base && (old = html_dom_query(base, sel));) html_dom_remove(old);
    return 1;
}

// ["msg", fromId, text] (hub-10.0.7.js:6932-6950): the open thread gets the line, else the friend's bubble counts it
static void io_msg(const int from, const char *text) {
    char shown[16] = "";
    if (el("chatBox")) html_dom_get_style(el("chatBox"), "display", shown, sizeof(shown));
    if (g_chat_peer == from && g_chat_list && !strcmp(shown, "block")) {
        cJSON *m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "fna", g_chat_peer_name);
        cJSON_AddNumberToObject(m, "tim", 0);
        cJSON_AddNumberToObject(m, "fid", from);
        StrBuf t = {0};
        sb_append_esc(&t, text ? text : "");
        char *txt = sb_finish(&t);
        cJSON_AddStringToObject(m, "txt", txt ? txt : "");
        free(txt);
        cJSON_InsertItemInArray(g_chat_list, 0, m);
        chat_update_list();
        return;
    }
    hub_sound("notif_0", 0.7f);
    char id[32];
    snprintf(id, sizeof(id), "msgCnt%d", from);
    const HtmlElem c = el(id);
    if (!c) return;
    char now[16] = "";
    html_dom_get_text(c, now, sizeof(now));
    char next[16];
    snprintf(next, sizeof(next), "%d", atoi(now) + 1);
    html_dom_set_text(c, next);
    html_dom_set_display(c, "block");
    const HtmlElem blob = html_dom_parent(c);
    if (blob) html_dom_set_style(blob, "opacity", "1");
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
    snprintf(g_chat_peer_name, sizeof(g_chat_peer_name), "%s", name ? name : "");
    hide_all(1);
    side_boxes();
    char cnt[32];
    snprintf(cnt, sizeof(cnt), "msgCnt%d", id);
    set_display(cnt, "none");
    set_html(cnt, "0");
    cJSON_Delete(g_chat_list);
    g_chat_list = cJSON_CreateArray();
    set_html("chatBoxFill", "<div style='width:100%;opacity:0.6;text-align:center;margin-bottom:10px;font-size:25px'><div style='display:inline-block'><div class='lds-ring'><div></div><div></div><div></div><div></div></div></div></div>");
    char src[512] = "";
    if (argc > 3 && args[3].kind == HTML_ARG_THIS && ev && ev->target) html_dom_get_attr(ev->target, "src", src, sizeof(src));
    else if (argc > 3) snprintf(src, sizeof(src), "%s", arg_s(ev, args, argc, 3) ? arg_s(ev, args, argc, 3) : "");
    StrBuf sb = {0};
    sb_append(&sb, "<img src='");
    sb_append_esc(&sb, src);
    sb_append(&sb, "' class='chatHeadPFP' onerror='this.onerror=null;this.src=\"./img/player_pfp.png\";' />");
    if (player && name) {
        sb_append(&sb, "<a href='/social.html?p=profile&q=");
        sb_append_esc(&sb, name);
        sb_append(&sb, "' style='color:#fff'>");
        sb_append_esc(&sb, name);
        sb_append(&sb, "</a>");
    } else if (name) {
        sb_append_esc(&sb, name);
    }
    // the peer's presence at the right of the header (hub-10.0.7.js:2249-2255): startChat's status text, "Offline" when
    // it is empty or "undefined"; Join unless the game argument is "" (a missing one is undefined: Join too)
    const char *status = argc > 5 ? arg_s(ev, args, argc, 5) : NULL, *game = argc > 6 ? arg_s(ev, args, argc, 6) : "undefined";
    sb_append(&sb, "<div style='position:absolute;vertical-align:top;color:rgba(255,255,255,0.6);margin-right:5px;right:5px;top:6px;'>");
    if (status && *status && strcmp(status, "undefined")) {
        sb_append(&sb, "<div class='onStatus' style='background:#27ab32;margin-top:-8px;vertical-align:middle;'></div>");
        sb_append_esc(&sb, status);
    } else {
        sb_append(&sb, "<div class='onStatus' style='background:#7f7f7f;margin-top:-8px;vertical-align:middle;'></div>Offline");
    }
    if (!game || *game) {
        sb_append(&sb, "<div class='frndQickJoin' onclick='openURL(`/?game=");
        sb_append_js_escape(&sb, game ? game : "undefined");
        sb_append(&sb, "`,1)'><div title='Join'><span class='material-icons'style='color:#fff;font-size:35px;'>launch</span></div></div>");
    }
    sb_append(&sb, "</div>");
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

// The settings' image collection (imagePopup / imageNewPopup / addNewImage / removeImage / selectImage, hub-10.0.7.js:
// 6335-6440): the menu's (image_collection.c, the same "krk_custImgs" list) in the hub's popup; a pick sets the
// setting and opens the settings again (selectImage, :6359-6368).
static void image_host_show(const char *cls, const char *html) { generic_popup(html, cls, NULL); }
static void image_host_set(const char *key, const char *url) { set_setting(key, url); }
static void image_host_close(void) { open_settings(); }

H(h_image_collection) {
    static const ImageCollectionHost host = {image_host_show, image_host_set, image_host_close};
    (void) user;
    image_collection_set_host(&host);
    image_collection_handle(html_ui_current_handler(), ev, args, argc);
    image_collection_set_host(NULL);
    return 1;
}

H(h_set_setting) {
    (void) user;
    const char *key = arg_s(ev, args, argc, 0);
    if (!key || argc < 2) return 1;
    char value[2048];
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
    char value[2048] = "";
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

// window.showPopup(page, skin, thumbnail, label, skinid, funds, count, salestart, player, uid, marketid, refresh)
// (hub-10.0.7.js:4790): the market / Black Market / inventory / sales popup; no page closes it
static void pop_str(char *out, const size_t n, const HtmlEvent *ev, const HtmlArg *args, const int argc, const int i) {
    out[0] = 0;
    if (i >= argc) return;
    if (args[i].kind == HTML_ARG_NUM) js_num(out, n, args[i].num);
    else if (args[i].kind == HTML_ARG_STR && args[i].str && strcmp(args[i].str, "undefined") && strcmp(args[i].str, "null")) snprintf(out, n, "%s", arg_s(ev, args, argc, i));
}

H(h_show_popup) {
    UNUSED;
    MarketPop p = {0};
    pop_str(p.page, sizeof(p.page), ev, args, argc, 0);
    if (!g_hub.brain) p.page[0] = 0; // KrunkNative: no market without kn_brain
    p.skin = (int) arg_n(args, argc, 1, -1);
    pop_str(p.preview, sizeof(p.preview), ev, args, argc, 2);
    pop_str(p.label, sizeof(p.label), ev, args, argc, 3);
    pop_str(p.skin_id, sizeof(p.skin_id), ev, args, argc, 4);
    pop_str(p.funds, sizeof(p.funds), ev, args, argc, 5);
    p.has_count = argc > 6 && args[6].kind == HTML_ARG_NUM;
    p.count = (int) arg_n(args, argc, 6, 0);
    pop_str(p.salestart, sizeof(p.salestart), ev, args, argc, 7);
    if (argc > 8 && !(args[8].kind == HTML_ARG_NUM && args[8].num == 0)) pop_str(p.player, sizeof(p.player), ev, args, argc, 8);
    p.has_uid = argc > 9 && args[9].kind == HTML_ARG_NUM;
    p.uid = (int) arg_n(args, argc, 9, 0);
    p.market_id = (int) arg_n(args, argc, 10, 0);
    p.refresh = argc > 11 && arg_n(args, argc, 11, 0) != 0;
    market_popup(&p);
    return 1;
}

// window.refreshPopup (hub-10.0.7.js:4378-4382): showPopup(...lastPop)
H(h_refresh_popup) {
    UNUSED;
    if (g_last_pop.valid) {
        const MarketPop p = g_last_pop;
        market_popup(&p);
    }
    return 1;
}

static void send_ints(const char *label, const int n, const double *v) {
    cJSON *f = cJSON_CreateArray();
    cJSON_AddItemToArray(f, cJSON_CreateString(label));
    for (int i = 0; i < n; i++) cJSON_AddItemToArray(f, cJSON_CreateNumber(v[i]));
    hub_send(f);
}

// An economy action's idempotency key (doc/v7/economy.md, econ_new_op): kn_brain runs an action
// once per key, a resent frame (double click, a retry) replays the first result. Keys of actions
// that can only happen once (buy listing N, accept / close offer N) are derived from the target;
// the others are made when their confirmation popup opens and travel in its YES button.
// io.send(label, numbers..., {op: key})
static void send_econ(const char *label, const int n, const double *v, const char *key) {
    hub_send(econ_frame(label, n, v, key));
}

// buyItem(marketid, uid): io.send("pi", marketid, uid); the listing page passes null (hub-10.0.7.js:3530): no card
H(h_buy_item) {
    UNUSED;
    const double v[2] = {arg_n(args, argc, 0, 0), argc > 1 && args[1].kind == HTML_ARG_NUM ? args[1].num : -1};
    char key[48];
    snprintf(key, sizeof(key), "buy-%.0f", v[0]);
    send_econ("pi", 2, v, key);
    io_wait("Buying Item...");
    return 1;
}

// buyblackM(skin) (hub-10.0.7.js:5182-5195): market.purchaseBlackMarketItem; kn_brain's io.send("pbm", skin) -> io_bmr
H(h_buy_illicit) {
    UNUSED;
    const double v[1] = {arg_n(args, argc, 0, 0)};
    if (!g_pbm_key[0]) econ_new_op(g_pbm_key, sizeof(g_pbm_key), "pbm");
    send_econ("pbm", 1, v, g_pbm_key);
    io_wait("Buying Item...");
    return 1;
}

// listItem(skin, skinid, uid) -> confListPopup (hub-10.0.7.js:5212-5230: a noSale or opensea item cannot be listed)
// -> listConfirmed: io.send("li", skin, skinid, uid, price)
H(h_list_item) {
    UNUSED;
    char price[32] = "";
    const HtmlElem in = el("sellPriceIn");
    if (in) html_dom_get_value(in, price, sizeof(price));
    if (!price[0] || !strcmp(price, "0") || !g_hub.logged_in) return 1;
    const int skin = (int) arg_n(args, argc, 0, 0);
    const ViewerSkin *pa = hub_skin(skin);
    if (!pa) return 1;
    if (skin_true(skin, "noSale")) {
        io_po(0, "Item cant be sold.");
        return 1;
    }
    if (skin_true(skin, "opensea")) {
        io_po(0, I18N("item.sell.cant.krunker", "Item cant be sold on krunker."));
        return 1;
    }
    StrBuf sb = {0};
    sb_appendf(&sb, "<div style='color:#fff;font-size:20px;position:relative;margin-bottom:15px;'>Are you sure you want to list <span style='color:%s'>", rarity_color(pa));
    sb_append_esc(&sb, pa->name);
    char key[48];
    econ_new_op(key, sizeof(key), "li");
    sb_appendf(&sb, "</span> for %dKR ?</div><div id='confirmBtn' onclick='listConfirmed(%d, %d,%d, %d, &quot;%s&quot;)' onmouseover='SOUND.play(&quot;tick_0&quot;,0.1)'>YES</div><div id='declineBtn' onclick='refreshPopup()' onmouseover='SOUND.play(&quot;tick_0&quot;,0.1)'>NO</div>",
               atoi(price), skin, (int) arg_n(args, argc, 1, 0), (int) arg_n(args, argc, 2, 0), atoi(price), key);
    char *html = sb_finish(&sb);
    generic_popup(html, "confListPop", NULL); // confListPopup
    free(html);
    return 1;
}

H(h_list_confirmed) {
    UNUSED;
    const double v[4] = {arg_n(args, argc, 0, 0), arg_n(args, argc, 1, 0), arg_n(args, argc, 2, 0), arg_n(args, argc, 3, 0)};
    const char *key = arg_s(ev, args, argc, 4);
    char fresh[48];
    if (!key || !*key) econ_new_op(fresh, sizeof(fresh), "li");
    send_econ("li", 4, v, key && *key ? key : fresh);
    io_wait(I18N("social.market.posting", "Posting Item..."));
    return 1;
}

// unlistItem(skin, marketid, uid): io.send("uli", ...)
H(h_unlist_item) {
    UNUSED;
    const double v[3] = {arg_n(args, argc, 0, 0), arg_n(args, argc, 1, 0), arg_n(args, argc, 2, 0)};
    // a fresh key per attempt (as listing has): a fixed "uli-<market id>" made an unlist tried again right after a failed
    // one replay that failure from kn_brain's economy_requests (v10.0.7: "Fixed unlisting or listing again right after a
    // Krunker Hub listing failing")
    char key[48];
    econ_new_op(key, sizeof(key), "uli");
    send_econ("uli", 3, v, key);
    io_wait(I18N("social.market.unlisting", "Unlisting Item..."));
    return 1;
}

// quickSell(skin, rarity, uid) -> confQSPopup(skin, uid) (hub-10.0.7.js:3717-3754): the item's quick sell KR and Junk
// (store.rarities' sell / scrap, minQuickSell, qsKR, qsJunk: market_qs_value), Get KR / Get Junk, or Destroy for a
// noQuickSell item; a Black Market noSale or an opensea item cannot be sold
H(h_quick_sell) {
    UNUSED;
    if (!g_hub.logged_in) {
        io_po(0, I18N("account.login.required", "Must be logged in"));
        return 1;
    }
    const int skin = (int) arg_n(args, argc, 0, 0), uid = (int) arg_n(args, argc, 2, 0);
    const ViewerSkin *pa = hub_skin(skin);
    if (!pa) return 1;
    const int no_qs = skin_true(skin, "noQuickSell");
    if (skin_true(skin, "blackM") && skin_true(skin, "noSale")) {
        io_po(0, I18N("item.sell.cant", "Item cant be sold."));
        return 1;
    }
    if (skin_true(skin, "opensea")) {
        io_po(0, I18N("item.sell.cant.krunker", "Item cant be sold on krunker."));
        return 1;
    }
    const MarketQsValue qv = market_qs_value(ui_loadout_skin_json(skin));
    char key[48], name[160], head[320];
    econ_new_op(key, sizeof(key), "qs");
    snprintf(name, sizeof(name), "<span style='color:%s'>", rarity_color(pa));
    {
        StrBuf n = {0};
        sb_append(&n, name);
        sb_append_esc(&n, pa->name);
        sb_append(&n, "</span>");
        char *t = sb_finish(&n);
        I18N_F(head, no_qs ? "item.sell.destroy" : "item.sell.quick", no_qs ? "Are you sure you want to Destroy {0}?" : "Are you sure you want to Quick Sell {0}?", t);
        free(t);
    }
    StrBuf sb = {0};
    sb_appendf(&sb, "<div style='color:#FBC02D;font-size:19px;position:relative;margin-bottom:15px;'>%s</div>", head);
    if (no_qs) sb_append(&sb, "<div style='color:#ed4242;font-size:15px;'>This item has no value</div>");
    char v[32];
    if (qv.kr) {
        js_num(v, sizeof(v), qv.kr);
        sb_appendf(&sb, "<div class='quickSellText'>KR<div class='quickSellKR'><span style='color: #FBC02D'>%s</span> KR (each)</div></div>", v);
    }
    if (qv.junk) sb_appendf(&sb, "<div class='quickSellText'>Junk<div class='quickSellKR'><span style='color: #FBC02D'>%.2f</span> Junk (each)</div></div>", qv.junk);
    sb_append(&sb, "<div class='quickSellText'>Amount<select id='quickSellSelect'><option value='0'>1 Item</option><option value='1'>All But 1</option><option value='2'>All</option></select></div>");
    if (no_qs) {
        sb_appendf(&sb, "<div id='quickSellBtn' onclick='sellConfirmed(%d,2,%d,&quot;%s&quot;)' style='width:74%%' onmouseover='SOUND.play(&quot;tick_0&quot;,0.1)'>Destroy</div>", skin, uid, key);
    } else {
        sb_appendf(&sb, "<div id='quickSellBtn' onclick='sellConfirmed(%d,0,%d,&quot;%s&quot;)' %sonmouseover='SOUND.play(&quot;tick_0&quot;,0.1)'>Get KR</div>", skin, uid, key, qv.junk ? "" : "style='width:74%' ");
        if (qv.junk) sb_appendf(&sb, "<div id='scrapBtn' onclick='sellConfirmed(%d,1,%d,&quot;%s&quot;)' onmouseover='SOUND.play(&quot;tick_0&quot;,0.1)'>Get Junk</div>", skin, uid, key);
    }
    sb_append(&sb, "<div id='declineBtn' style='width: 15%;' onclick='showPopup()' onmouseover='SOUND.play(&quot;tick_0&quot;,0.1)'>NO</div>");
    char *html = sb_finish(&sb);
    generic_popup(html, "confQSPop", NULL); // confQSPopup
    free(html);
    return 1;
}

// sellConfirmed(skin, mode, uid) (hub-10.0.7.js:3741-3748): io.send("qsi", mode (0 KR, 1 Junk, 2 destroy), skin, uid,
// amount); KrunkNative passes the popup's op key fourth
H(h_sell_confirmed) {
    UNUSED;
    if (!g_hub.logged_in) {
        io_po(0, I18N("account.login.required", "Must be logged in"));
        return 1;
    }
    char amount[8] = "0";
    const HtmlElem sel = el("quickSellSelect");
    if (sel) html_dom_get_value(sel, amount, sizeof(amount));
    const double v[4] = {arg_n(args, argc, 1, 0), arg_n(args, argc, 0, 0), arg_n(args, argc, 2, 0), atoi(amount)};
    const char *key = arg_s(ev, args, argc, 3);
    char fresh[48];
    if (!key || !*key) econ_new_op(fresh, sizeof(fresh), "qs");
    io_wait(I18N("generic.wait", "Please Wait..."));
    send_econ("qsi", 4, v, key && *key ? key : fresh);
    return 1;
}

// calcFee(input): the list fee line, floor(price * marketFee) unless price <= marketFeeBypass
H(h_calc_fee) {
    (void) user; (void) args; (void) argc;
    const HtmlElem in = ev ? ev->target : 0;
    if (!in) return 1;
    char text[32] = "";
    html_dom_get_value(in, text, sizeof(text));
    double a = atof(text);
    if (a < 0) a = 0;
    if (a > SALE_MAX) a = SALE_MAX;
    const double fee = a <= g_econ.market_fee_bypass ? 0 : floor(a * g_econ.market_fee / 100.0);
    StrBuf sb = {0};
    if (fee > 0) {
        char n[32];
        comma_number(n, sizeof(n), fee);
        sb_appendf(&sb, "<span style='color:rgba(255,255,255,0.7)'>-%s KR</span> List Fee", n);
    } else {
        sb_append(&sb, I18N("social.market.fee", "No List Fee"));
    }
    char *html = sb_finish(&sb);
    set_html("sellPricFee", html);
    free(html);
    const AccountInfo *acc = g_hub.logged_in ? active_account() : NULL;
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

// loadMoreTradeHistory(): the next page of the Trade History (hub-10.0.7.js:3843-3890)
H(h_load_more_trade_history) {
    UNUSED;
    if (!g_trades_cache || strcmp(g_hub.v, "market") || strcmp(g_hub.b, "trades")) return 1;
    g_his_page += TRADE_HISTORY_PAGE;
    g_his_keep = 1;
    hub_render_market("trades", g_trades_cache, 0, NULL);
    return 1;
}

// fastDeclOffer(id) / declineAllTrades(): io.send("trC", id) / ("trCA")
H(h_decline_offer) {
    UNUSED;
    if (!strcmp(html_ui_current_handler(), "declineAllTrades")) {
        char key[48];
        econ_new_op(key, sizeof(key), "trca");
        send_econ("trCA", 0, NULL, key);
        // iíïìiîí() (hub-10.0.7.js:5722-5735): the incoming rows go, the head's count reads 0
        const HtmlElem in = el("o_in");
        if (in) {
            for (HtmlElem c = html_dom_first_child(in), nx; c; c = nx) {
                nx = html_dom_next(c);
                char cls[64];
                if (html_dom_get_attr(c, "class", cls, sizeof(cls)) >= 0 && !strcmp(cls, "offerHolder")) html_dom_remove(c);
            }
            const HtmlElem head = html_dom_first_child(in), cnt = head ? html_dom_first_child(head) : 0;
            if (cnt) html_dom_set_html(cnt, "0");
        }
        return 0;
    }
    html_ui_stop_propagation(); // f486(window.event): not the row's viewOffer
    const double v[2] = {arg_n(args, argc, 0, 0), 1}; // trC(id, 1): the fast decline
    char key[48];
    snprintf(key, sizeof(key), "trc-%.0f", v[0]);
    offer_card_remove((int) v[0]); // fastDeclOffer: the card goes before the send (hub-10.0.7.js:4060)
    send_econ("trC", 2, v, key);
    return 0;
}

// ---- economy: KR gifts, the trade window, offers, KR history (social.js giftPopup / calcGiftFee /
// giftUser, tradePopup / io_trD / addItem / removeItem / searchItems / showMissingItems /
// confSubmitOffer / submitOffer / io_trR, viewOffer / confOffer / offerConfirmed / declOffer;
// KrunkNative: KR in offers, "krh"). doc/v7/economy.md

static char g_gift_op[48]; // the open gift popup's key (a new one after each sent gift)

// calcGiftFee: ceil(amount * giftFee), on top of the amount. Integer maths: v7's float version
// (Math.ceil(10 * 1.1) = 12) disagrees with what the server charges by 1 KR at some amounts.
static double gift_fee_of(const double amount) {
    const long long a = (long long) amount, pct = (long long) g_econ.gift_fee;
    return (double) ((a * pct + 99) / 100);
}

// giftPopup(auto): #giftPop for the profile on screen
H(h_gift_popup) {
    UNUSED;
    reset_pops();
    const HtmlElem pop = el("giftPop");
    if (!pop) return 1;
    html_dom_set_display(pop, "block");
    const AccountInfo *acc = g_hub.logged_in ? active_account() : NULL;
    char req[64] = "";
    if (!acc) snprintf(req, sizeof(req), "%s", I18N("social.login.gift", "Login to Gift"));
    else if (acc->level < (int) g_econ.gift_min_lvl) {
        char lvl[16];
        snprintf(lvl, sizeof(lvl), "%d", (int) g_econ.gift_min_lvl);
        I18N_F(req, "social.market.req", "lvl {0} required", lvl);
    }
    econ_new_op(g_gift_op, sizeof(g_gift_op), "kr");
    StrBuf sb = {0};
    sb_appendf(&sb, "<div class='popHeader'>%s</div><div id='giftKRBody'><div id='giftAmntFee'>Fee: None</div><input id='giftIn' oninput='calcGiftFee(this)' type='number' placeholder='%s KR' min='0' max='%.0f' step='1'><textarea id='giftMsg' rows='5' placeholder='%s'></textarea>",
               I18N("social.market.gift", "Gift KR"), I18N("social.market.gift.amount", "Gift Amount in"), g_econ.gift_max, I18N("social.market.gift.msg", "Leave a message"));
    sb_append(&sb, "<div id='cPostSaleBtn' class='pGiftButton' style='display:none;background-color:#2b2b2b'>Not enough KR</div>");
    sb_appendf(&sb, "<div id='postSaleBtn' class='pGiftButton' %s%sonmouseover='SOUND.play(&quot;tick_0&quot;,0.1)'>%s</div></div>",
               req[0] ? "style='background-color:#2b2b2b'" : "", req[0] ? "" : "onclick='giftUser()'", req[0] ? req : I18N("social.market.gift", "Gift KR"));
    char *html = sb_finish(&sb);
    html_dom_set_html(pop, html);
    free(html);
    return 1;
}

H(h_calc_gift_fee) {
    (void) user; (void) args; (void) argc;
    const HtmlElem in = ev ? ev->target : el("giftIn");
    if (!in) return 1;
    char text[32] = "";
    html_dom_get_value(in, text, sizeof(text));
    double a = floor(atof(text));
    if (a < 0) a = 0;
    if (a > g_econ.gift_max) a = g_econ.gift_max;
    if (a && a != atof(text)) {
        js_num(text, sizeof(text), a);
        html_dom_set_value(in, text);
    }
    const double fee = gift_fee_of(a);
    if (fee > 0) {
        char n[32], html[160];
        comma_number(n, sizeof(n), fee);
        snprintf(html, sizeof(html), "Fee: <span style='color:var(--red);'>%s KR</span>", n);
        set_html("giftAmntFee", html);
    } else {
        set_html("giftAmntFee", "Fee: None");
    }
    const AccountInfo *acc = g_hub.logged_in ? active_account() : NULL;
    if (acc && el("postSaleBtn") && el("cPostSaleBtn")) {
        const int afford = a + fee <= acc->funds, small = a < g_econ.gift_min;
        set_display("postSaleBtn", afford && !small ? "block" : "none");
        set_display("cPostSaleBtn", afford && !small ? "none" : "block");
        char t[48];
        snprintf(t, sizeof(t), small ? "%.0f KR minimum" : "Not enough KR", g_econ.gift_min);
        set_html("cPostSaleBtn", t);
    }
    return 1;
}

// giftUser(): io.send("kr", name, amount, message)
H(h_gift_user) {
    UNUSED;
    char amount[32] = "", msg[512] = "";
    if (el("giftIn")) html_dom_get_value(el("giftIn"), amount, sizeof(amount));
    if (el("giftMsg")) html_dom_get_value(el("giftMsg"), msg, sizeof(msg));
    if (!amount[0] || !strcmp(amount, "0") || !g_hub.profile_name[0]) return 1;
    if (!g_gift_op[0]) econ_new_op(g_gift_op, sizeof(g_gift_op), "kr");
    cJSON *f = cJSON_CreateArray();
    cJSON_AddItemToArray(f, cJSON_CreateString("kr"));
    cJSON_AddItemToArray(f, cJSON_CreateString(g_hub.profile_name));
    cJSON_AddItemToArray(f, cJSON_CreateNumber(floor(atof(amount))));
    cJSON_AddItemToArray(f, cJSON_CreateString(msg));
    cJSON *ext = cJSON_CreateObject();
    cJSON_AddStringToObject(ext, "op", g_gift_op);
    cJSON_AddItemToArray(f, ext);
    hub_send(f);
    io_wait(I18N("generic.wait", "Please wait..."));
    return 1;
}

// confPremium(index) (hub-10.0.7.js:5751-5755): "Gift '<profile>' <days> days of Premium?" with YES / NO over the
// store's premium table (classes chunk `premium`: 7 / 30 / 60 / 90 days, the store's k_premium). Nothing in the 10.0.7
// hub draws a caller; the page defines it, so the Box Office hub does too.
H(h_conf_premium) {
    UNUSED;
    const int index = (int) arg_n(args, argc, 0, -1);
    const int days = store_ui_premium_days(index);
    if (days <= 0 || !g_hub.profile_name[0]) return 1;
    char dtext[16], q[256];
    snprintf(dtext, sizeof(dtext), "%d", days);
    I18N_F(q, "windows.store.premium.gift", "Are you sure you want to Gift '{0}' {1} days of Premium?", g_hub.profile_name, dtext);
    StrBuf sb = {0};
    sb_append(&sb, "<div style='color:#fff;font-size:20px;position:relative;margin-bottom:15px;'>");
    sb_append_esc(&sb, q);
    sb_appendf(&sb, "</div><div id='confirmBtn' onclick='premiumGiftConfirmed(%d)' onmouseover='playTick(0.1)'>YES</div>"
                    "<div id='declineBtn' onclick='clearPops()' onmouseover='playTick(0.1)'>NO</div>",
               index);
    char *html = sb_finish(&sb);
    generic_popup(html, "confPremiumPop", NULL);
    free(html);
    return 1;
}

// premiumGiftConfirmed(index) (hub-10.0.7.js:5756-5759): io.send("prm", profile.player_id, index) and the wait popup.
// Premium is bought with real money (store.premium's USD prices), so kn_brain answers the Box Office refusal ("po").
H(h_premium_gift_confirmed) {
    UNUSED;
    if (!g_hub.profile_id) return 1;
    const double v[2] = {g_hub.profile_id, (int) arg_n(args, argc, 0, 0)};
    send_ints("prm", 2, v);
    io_wait(I18N("generic.wait", "Please wait..."));
    return 1;
}

// io_gir(result): funds, the success popup, the page again (f557)
static void io_gir(const cJSON *r) {
    if (!cJSON_IsObject(r)) {
        io_po(0, NULL);
        return;
    }
    if (cJSON_GetObjectItemCaseSensitive(r, "funds")) hub_set_funds(jnum(r, "funds"));
    g_gift_op[0] = 0; // the next gift is a new action
    io_po(1, NULL);
    send_page_request();
}

// ---- the trade window (tradePopup -> "ri" -> io_trD)

typedef struct {
    int i, c, nt; // skin, count, not tradable (kn_brain's nt)
} TradeItem;
#define TRADE_SEL_MAX 64

static struct {
    int target_id;
    char target[64];
    TradeItem *inv[2];    // vA169: [mine, theirs]
    int ninv[2];
    TradeItem sel[2][TRADE_SEL_MAX]; // vA170: the trade lists
    int nsel[2];
    int fee;              // vLN0611
    int missing;          // v3758
    double kr[2];         // KrunkNative: KR each side puts in
    char msg[512];        // v3782
} g_trade;

static int trade_max_items(void) {
    const AccountInfo *acc = g_hub.logged_in ? active_account() : NULL;
    return (int) (acc && acc->premium > 0 ? g_econ.trade_max_out_p : g_econ.trade_max_out);
}

static int cmp_trade_items(const void *pa, const void *pb) {
    const TradeItem *e = pa, *a = pb; // UTILS.sortItems
    const ViewerSkin *se = hub_skin(e->i), *sa = hub_skin(a->i);
    const int t = sa ? sa->rarity : 0, n = se ? se->rarity : 0;
    const int i = sa ? (sa->seas ? sa->seas : 1) : 0, r = se ? (se->seas ? se->seas : 1) : 0;
    if (t > n) return 1;
    if (t < n) return -1;
    if (i > r) return 1;
    if (i < r) return -1;
    return a->i - e->i;
}

static TradeItem *trade_sel_find(const int side, const int skin) {
    for (int k = 0; k < g_trade.nsel[side]; k++)
        if (g_trade.sel[side][k].i == skin) return &g_trade.sel[side][k];
    return NULL;
}

static const TradeItem *trade_inv_find(const int side, const int skin) {
    for (int k = 0; k < g_trade.ninv[side]; k++)
        if (g_trade.inv[side][k].i == skin) return &g_trade.inv[side][k];
    return NULL;
}

static int trade_side_total(const int side) {
    int t = 0;
    for (int k = 0; k < g_trade.nsel[side]; k++) t += g_trade.sel[side][k].c;
    return t;
}

// f531: store.calcTradeFee of every item on both lists; premium pays none
static void trade_update_fee(void) {
    const AccountInfo *acc = g_hub.logged_in ? active_account() : NULL;
    int fee = 0;
    for (int side = 0; side < 2; side++) {
        for (int k = 0; k < g_trade.nsel[side]; k++) {
            const ViewerSkin *pa = hub_skin(g_trade.sel[side][k].i);
            if (pa) fee += econ_rarity_trade_fee(pa->rarity) * g_trade.sel[side][k].c;
        }
    }
    g_trade.fee = acc && acc->premium > 0 ? 0 : fee;
    // KrunkNative: KR you add to an offer pays the gift fee (rounded up), premium or not
    if (g_trade.kr[0] > 0) g_trade.fee += (int) gift_fee_of(g_trade.kr[0]);
    char t[64];
    if (g_trade.fee) snprintf(t, sizeof(t), "Submit Offer - %d KR", g_trade.fee);
    else snprintf(t, sizeof(t), "Submit Offer");
    set_html("trdSubBt", t);
}

static SprayAnims g_trade_anims = {.name = "sprayAnm"};

static void trade_item_html(StrBuf *sb, const int side, const TradeItem *it, const int in_list) {
    const ViewerSkin *pa = hub_skin(it->i);
    if (!pa) return;
    char prev[256];
    skin_preview(prev, sizeof(prev), pa);
    if (in_list) sb_appendf(sb, "<div id='trd_%d_%d' class='tItemHolder%s' onclick='removeItem(%d,%d)' oncontextmenu='openSales(%d)' data-index='%d' onmouseover='createTip(this)' onmouseout='cancelTip()' style='border: 2px solid %s;'>",
                            side, it->i, g_hub.rarity_anim && rarity_animate(pa) ? " rainbowBorder" : "", it->i, side, it->i, it->i, rarity_color(pa));
    else sb_appendf(sb, "<div id='trd_inv_%d_%d' class='tItemHolder%s' onclick='addItem(%d, %d)' oncontextmenu='openSales(%d)' data-index='%d' onmouseover='createTip(this)' onmouseout='cancelTip()' style='border: 2px solid %s;'>",
                    side, it->i, g_hub.rarity_anim && rarity_animate(pa) ? " rainbowBorder" : "", it->i, side, it->i, it->i, rarity_color(pa));
    g_spray_anims = &g_trade_anims;
    append_skin_img(sb, pa, prev, "tItemSprayImg", "tItemImg");
    g_spray_anims = NULL;
    sb_append(sb, "<div class='tItemN'>");
    sb_append_esc(sb, pa->name);
    sb_appendf(sb, "</div><div class='tItemCnt' id='%s_%d_%d'>x%d</div>", in_list ? "trd_cnt" : "trd_inv_cnt", side, it->i, it->c);
    if (!in_list && it->nt) sb_append(sb, "<div class='tItemBound'><span class='material-icons' style='font-size:30px;color:#fff;'>lock</span></div>");
    sb_append(sb, "</div>");
}

// io_trD(name, id, [mine, theirs])
static void io_trd(const char *name, const int id, const cJSON *invs) {
    const AccountInfo *acc = g_hub.logged_in ? active_account() : NULL;
    if (!acc) return;
    if (name && !strcmp(name, acc->name)) {
        io_po(0, "Cant trade with yourself");
        return;
    }
    for (int side = 0; side < 2; side++) {
        free(g_trade.inv[side]);
        g_trade.inv[side] = NULL;
        g_trade.ninv[side] = 0;
        g_trade.nsel[side] = 0;
        g_trade.kr[side] = 0;
        const cJSON *list = cJSON_GetArrayItem(invs, side);
        const int n = cJSON_GetArraySize(list);
        g_trade.inv[side] = n ? calloc((size_t) n, sizeof(TradeItem)) : NULL;
        const cJSON *r;
        cJSON_ArrayForEach(r, list) {
            if (!g_trade.inv[side]) break;
            TradeItem *t = &g_trade.inv[side][g_trade.ninv[side]];
            t->i = (int) jnum(r, "i");
            t->c = (int) jnum(r, "c");
            t->nt = jnum(r, "nt") != 0;
            if (t->c > 0 && hub_skin(t->i)) g_trade.ninv[side]++;
        }
        if (g_trade.ninv[side]) qsort(g_trade.inv[side], (size_t) g_trade.ninv[side], sizeof(TradeItem), cmp_trade_items);
    }
    g_trade.target_id = id;
    snprintf(g_trade.target, sizeof(g_trade.target), "%s", name ? name : "");
    g_trade.fee = 0;
    g_trade.msg[0] = 0;
    const int mx = trade_max_items();
    StrBuf sb = {0};
    g_trade_anims.n = 0;
    for (int side = 0; side < 2; side++) {
        sb_appendf(&sb, "<div class='%s'><div class='tListN inv'>", side ? "tHolderR" : "tHolderL");
        if (side) {
            sb_append_esc(&sb, g_trade.target);
            sb_append(&sb, "'s Inventory");
        } else {
            sb_append(&sb, "Your Inventory");
        }
        sb_appendf(&sb, "</div><div id='invList%d' class='tList' style='height: 225px;'>", side);
        for (int k = 0; k < g_trade.ninv[side]; k++) {
            if (g_trade.inv[side][k].nt && g_hub.hide_non_trade) continue; // hideNonTrade
            trade_item_html(&sb, side, &g_trade.inv[side][k], 0);
        }
        sb_appendf(&sb, "</div><input class='tSearch' type='text' placeholder='Search Item' onkeyup='searchItems(this, %d)'>", side);
        sb_appendf(&sb, "<div class='tListN trade' id='tradeTotal%d'>Trade List 0/%d</div><div id='tradeList%d' class='tList' style='height:120px'></div>", side, mx, side);
        sb_appendf(&sb, "<div class='estOfferVal' id='offerVal%d'>Est. Value <span style='color:#fff'>0</span> KR</div>", side);
        // KrunkNative: KR in the offer (kn_brain escrows the sender's, takes the receiver's on accept)
        if (g_econ.trade_kr_max > 0)
            sb_appendf(&sb, "<input class='tSearch' id='tradeKR%d' type='number' min='0' max='%.0f' step='1' placeholder='%s' oninput='tradeKR(this, %d)'>",
                       side, g_econ.trade_kr_max, side ? "KR you ask for" : "KR you add", side);
        sb_append(&sb, "</div>");
    }
    // clear the two floated holders (a browser wraps the 829px textarea below them anyway; the
    // native engine needs the clearance to put it there)
    sb_append(&sb, "<div style='clear:both'></div><textarea id='tMsg' placeholder='Add a message to the offer'></textarea>");
    sb_appendf(&sb, "<div class='tSubOffer' id='trdSubBt' onclick='confSubmitOffer(%d)'>Submit Offer</div>", id);
    sb_append(&sb, "<div class='material-icons tradeSwap' title='Toggle Missing Items' onclick='showMissingItems(this)'>rule</div>");
    spray_anims_keyframes(&sb, &g_trade_anims, 60);
    char *html = sb_finish(&sb);
    generic_popup(html, "tradePop", NULL);
    free(html);
}

// tradePopup(): io.send("ri", name, id)
H(h_trade_popup) {
    UNUSED;
    const AccountInfo *acc = g_hub.logged_in ? active_account() : NULL;
    if (!acc) {
        io_po(0, I18N("account.login.required", "Must be logged in"));
    } else if (g_hub.profile_id == acc->id) {
        io_po(0, "Cant trade with yourself");
    } else if (acc->level < TRADE_MIN_LVL) {
        char t[32];
        snprintf(t, sizeof(t), "Must be level %d", TRADE_MIN_LVL);
        io_po(0, t);
    } else {
        io_wait("Fetching Inventories");
        cJSON *f = cJSON_CreateArray();
        cJSON_AddItemToArray(f, cJSON_CreateString("ri"));
        cJSON_AddItemToArray(f, cJSON_CreateString(g_hub.profile_name));
        cJSON_AddItemToArray(f, cJSON_CreateNumber(g_hub.profile_id));
        hub_send(f);
    }
    return 1;
}

// The counts, totals and value after a change of side's list.
static void trade_refresh_side(const int side, const int skin) {
    const TradeItem *have = trade_inv_find(side, skin);
    const TradeItem *sel = trade_sel_find(side, skin);
    char id[48], t[64];
    snprintf(id, sizeof(id), "trd_inv_cnt_%d_%d", side, skin);
    snprintf(t, sizeof(t), "x%d", have ? have->c - (sel ? sel->c : 0) : 0);
    if (el(id)) html_dom_set_text(el(id), t);
    snprintf(id, sizeof(id), "tradeTotal%d", side);
    snprintf(t, sizeof(t), "Trade List %d/%d", trade_side_total(side), trade_max_items());
    set_html(id, t);
    long long value = 0;
    for (int k = 0; k < g_trade.nsel[side]; k++) value += (long long) estimate_value(g_trade.sel[side][k].i) * g_trade.sel[side][k].c;
    char v[32], html[96];
    comma_number(v, sizeof(v), (double) value);
    snprintf(html, sizeof(html), "Est. Value <span style='color:#fff'>%s</span> KR", v);
    snprintf(id, sizeof(id), "offerVal%d", side);
    set_html(id, html);
    trade_update_fee();
}

// addItem(skin, side)
H(h_add_item) {
    UNUSED;
    const int skin = (int) arg_n(args, argc, 0, -1), side = (int) arg_n(args, argc, 1, 0) ? 1 : 0;
    const TradeItem *have = trade_inv_find(side, skin);
    TradeItem *sel = trade_sel_find(side, skin);
    if (!have || have->nt || (sel ? sel->c : 0) + 1 > have->c) return 1; // hasEnough, tradable
    if (trade_side_total(side) >= trade_max_items()) return 1;
    char id[48];
    if (sel) {
        sel->c++;
        snprintf(id, sizeof(id), "trd_cnt_%d_%d", side, skin);
        char t[16];
        snprintf(t, sizeof(t), "x%d", sel->c);
        if (el(id)) html_dom_set_html(el(id), t);
    } else {
        if (g_trade.nsel[side] >= TRADE_SEL_MAX) return 1;
        sel = &g_trade.sel[side][g_trade.nsel[side]++];
        sel->i = skin;
        sel->c = 1;
        sel->nt = 0;
        StrBuf sb = {0};
        trade_item_html(&sb, side, sel, 1);
        char *html = sb_finish(&sb);
        snprintf(id, sizeof(id), "tradeList%d", side);
        if (el(id)) html_dom_insert_html(el(id), HTML_INSERT_BEFORE_END, html);
        free(html);
    }
    trade_refresh_side(side, skin);
    return 1;
}

// removeItem(skin, side)
H(h_remove_item) {
    UNUSED;
    const int skin = (int) arg_n(args, argc, 0, -1), side = (int) arg_n(args, argc, 1, 0) ? 1 : 0;
    TradeItem *sel = trade_sel_find(side, skin);
    if (!sel) return 1;
    char id[48];
    if (--sel->c <= 0) {
        const int k = (int) (sel - g_trade.sel[side]);
        memmove(&g_trade.sel[side][k], &g_trade.sel[side][k + 1], (size_t) (g_trade.nsel[side] - k - 1) * sizeof(TradeItem));
        g_trade.nsel[side]--;
        snprintf(id, sizeof(id), "trd_%d_%d", side, skin);
        if (el(id)) html_dom_remove(el(id));
        ui_tooltip_cancel();
    } else {
        snprintf(id, sizeof(id), "trd_cnt_%d_%d", side, skin);
        char t[16];
        snprintf(t, sizeof(t), "x%d", sel->c);
        if (el(id)) html_dom_set_html(el(id), t);
    }
    trade_refresh_side(side, skin);
    return 1;
}

// searchItems(input, side): name / weapon / "drops" / limT
H(h_search_items) {
    (void) user;
    const int side = (int) arg_n(args, argc, 1, 0) ? 1 : 0;
    const HtmlElem in = ev ? ev->target : 0;
    char text[128] = "";
    if (in) html_dom_get_value(in, text, sizeof(text));
    for (int k = 0; k < g_trade.ninv[side]; k++) {
        const int i = g_trade.inv[side][k].i;
        const ViewerSkin *sk = hub_skin(i);
        int hit = !text[0] || !sk;
        if (!hit && skin_str(i, "keyW") && contains_either_ci(text, skin_str(i, "keyW"))) hit = 1;
        else if (!hit && skin_true(i, "drop") && contains_either_ci(text, "drops")) hit = 1;
        else if (!hit && skin_str(i, "limT") && contains_either_ci(text, skin_str(i, "limT"))) hit = 1;
        else if (!hit && sk->weapon >= 1 && sk->weapon <= rules_krunker_weapon_count() && rules_weapon(sk->weapon - 1) &&
                 contains_either_ci(text, rules_weapon(sk->weapon - 1)->name)) hit = 1;
        else if (!hit && sk->name && contains_either_ci(text, sk->name)) hit = 1;
        else if (!hit) {
            const cJSON *c;
            cJSON_ArrayForEach(c, skin_field(i, "creators")) {
                if (cJSON_IsString(c) && contains_either_ci(text, c->valuestring)) {
                    hit = 1;
                    break;
                }
            }
        }
        char id[48];
        snprintf(id, sizeof(id), "trd_inv_%d_%d", side, g_trade.inv[side][k].i);
        if (el(id)) html_dom_set_display(el(id), hit ? "inline-block" : "none");
    }
    return 1;
}

// showMissingItems(btn): hide what both sides own
H(h_show_missing) {
    (void) user; (void) args; (void) argc;
    g_trade.missing = !g_trade.missing;
    if (ev && ev->target) html_dom_set_style(ev->target, "color", g_trade.missing ? "#eb5656" : "#fff");
    for (int side = 0; side < 2; side++) {
        for (int k = 0; k < g_trade.ninv[side]; k++) {
            const int skin = g_trade.inv[side][k].i;
            char id[48];
            snprintf(id, sizeof(id), "trd_inv_%d_%d", side, skin);
            if (!el(id)) continue;
            const int both = trade_inv_find(!side, skin) != NULL;
            if (!g_trade.missing) html_dom_set_display(el(id), "inline-block");
            else if (both) html_dom_set_display(el(id), "none");
        }
    }
    return 1;
}

// tradeKR(input, side) (KrunkNative)
H(h_trade_kr) {
    (void) user;
    const int side = (int) arg_n(args, argc, 1, 0) ? 1 : 0;
    const HtmlElem in = ev ? ev->target : 0;
    char text[32] = "";
    if (in) html_dom_get_value(in, text, sizeof(text));
    double v = floor(atof(text));
    if (v < 0) v = 0;
    if (v > g_econ.trade_kr_max) v = g_econ.trade_kr_max;
    g_trade.kr[side] = v;
    trade_update_fee();
    return 1;
}

// confSubmitOffer(id)
H(h_conf_submit_offer) {
    UNUSED;
    if (!g_hub.logged_in) {
        io_po(0, I18N("account.login.required", "Must be logged in"));
        return 1;
    }
    if (el("tMsg")) html_dom_get_value(el("tMsg"), g_trade.msg, sizeof(g_trade.msg));
    const int id = (int) arg_n(args, argc, 0, 0);
    char key[48];
    econ_new_op(key, sizeof(key), "ctr");
    StrBuf sb = {0};
    if (!g_trade.nsel[1] && !g_trade.kr[1])
        sb_append(&sb, "<div style='color:#fff;font-size:18px;position:relative;margin-bottom:5px;color:#FBC02D'>You get no items from this offer!</div>");
    if (g_trade.kr[0] || g_trade.kr[1]) {
        char a[32], b[32];
        comma_number(a, sizeof(a), g_trade.kr[0]);
        comma_number(b, sizeof(b), g_trade.kr[1]);
        sb_appendf(&sb, "<div style='color:#fff;font-size:17px;position:relative;margin-bottom:8px;'>You give <span style='color:#FBC02D'>%s KR</span>, you get <span style='color:#FBC02D'>%s KR</span></div>", a, b);
    }
    sb_appendf(&sb, "<div style='color:#fff;font-size:19px;position:relative;margin-bottom:15px;'>Are you sure you want to submit this offer? (Cost %dKR)</div><div id='confirmBtn' onclick='submitOffer(%d, &quot;%s&quot;)' onmouseover='SOUND.play(&quot;tick_0&quot;,0.1)'>YES</div><div id='declineBtn' onclick='showPopup()' onmouseover='SOUND.play(&quot;tick_0&quot;,0.1)'>NO</div>",
               g_trade.fee, id, key);
    char *html = sb_finish(&sb);
    generic_popup(html, "confSubmitPop", NULL);
    free(html);
    return 1;
}

static cJSON *trade_list_json(const int side) {
    cJSON *a = cJSON_CreateArray();
    for (int k = 0; k < g_trade.nsel[side]; k++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "i", g_trade.sel[side][k].i);
        cJSON_AddNumberToObject(o, "c", g_trade.sel[side][k].c);
        cJSON_AddItemToArray(a, o);
    }
    return a;
}

// submitOffer(id, key): io.send("ctr", id, mine, theirs, message, {op, skr, rkr})
H(h_submit_offer) {
    UNUSED;
    if (!g_hub.logged_in) {
        io_po(0, I18N("account.login.required", "Must be logged in"));
        return 1;
    }
    const int mx = trade_max_items();
    if (!g_trade.nsel[0] && !g_trade.nsel[1] && !g_trade.kr[0] && !g_trade.kr[1]) {
        io_po(0, "No Items Selected");
        return 1;
    }
    if (g_trade.nsel[0] > mx || g_trade.nsel[1] > mx) {
        char t[48];
        snprintf(t, sizeof(t), "%d items per user", mx);
        io_po(0, t);
        return 1;
    }
    const char *key = arg_s(ev, args, argc, 1);
    char fresh[48];
    if (!key || !*key) econ_new_op(fresh, sizeof(fresh), "ctr");
    io_wait(I18N("social.trading.submitting", "Submitting Offer..."));
    cJSON *f = cJSON_CreateArray();
    cJSON_AddItemToArray(f, cJSON_CreateString("ctr"));
    cJSON_AddItemToArray(f, cJSON_CreateNumber(arg_n(args, argc, 0, 0)));
    cJSON_AddItemToArray(f, trade_list_json(0));
    cJSON_AddItemToArray(f, trade_list_json(1));
    cJSON_AddItemToArray(f, cJSON_CreateString(g_trade.msg));
    cJSON *ext = cJSON_CreateObject();
    cJSON_AddStringToObject(ext, "op", key && *key ? key : fresh);
    if (g_trade.kr[0] > 0) cJSON_AddNumberToObject(ext, "skr", g_trade.kr[0]);
    if (g_trade.kr[1] > 0) cJSON_AddNumberToObject(ext, "rkr", g_trade.kr[1]);
    cJSON_AddItemToArray(f, ext);
    hub_send(f);
    return 1;
}

// io_trR(result, tradeId, msg)
static void offer_card_remove(const int trid) {
    char id[32];
    snprintf(id, sizeof(id), "of_%d", trid);
    const HtmlElem card = el(id);
    if (!card) return;
    const HtmlElem col = html_dom_parent(card);
    html_dom_remove(card);
    if (!col) return;
    int n = 0;
    for (HtmlElem c = html_dom_first_child(col); c; c = html_dom_next(c)) n++;
    const HtmlElem head = html_dom_first_child(col), cnt = head ? html_dom_first_child(head) : 0;
    if (n <= 1) {
        if (cnt) html_dom_set_html(cnt, "0");
        html_dom_insert_html(col, HTML_INSERT_BEFORE_END, "<div style='color: rgba(0,0,0,0.6);font-size: 17px;'>None</div>");
    } else if (cnt) {
        char t[16];
        snprintf(t, sizeof(t), "%d", n - 1);
        html_dom_set_html(cnt, t);
    }
}

static void io_trr(const cJSON *res, const cJSON *tid, const char *msg) {
    if (!cJSON_IsObject(res)) {
        io_po(0, NULL);
        return;
    }
    if (cJSON_GetObjectItemCaseSensitive(res, "funds")) hub_set_funds(jnum(res, "funds"));
    if (cJSON_IsNumber(tid)) offer_card_remove(tid->valueint); // f533: drop the offer card
    io_po(1, msg);
}

// ---- offers (the Trades tab rows; viewOffer / confOffer / offerConfirmed / declOffer)


static void offers_clear(void) {
    for (int i = 0; i < g_offer_count; i++) free(g_offers[i].message);
    free(g_offers);
    g_offers = NULL;
    g_offer_count = 0;
}

static const HubOffer *offer_find(const int trid) {
    for (int i = 0; i < g_offer_count; i++)
        if (g_offers[i].trid == trid) return &g_offers[i];
    return NULL;
}

static int cmp_offer_ids(const void *pa, const void *pb) {
    const TradeItem a = {*(const int *) pa, 1, 0}, b = {*(const int *) pb, 1, 0};
    return cmp_trade_items(&a, &b);
}

static void offer_side_html(StrBuf *sb, const int *ids, const int n, const int pad_to, const double kr) {
    long long value = 0;
    sb_append(sb, "<div class='tList' style='height:unset;overflow-y: auto;text-align: center;'>");
    for (int k = 0; k < n; k++) {
        const ViewerSkin *pa = hub_skin(ids[k]);
        if (!pa) continue;
        char prev[256];
        skin_preview(prev, sizeof(prev), pa);
        sb_appendf(sb, "<div class='tItemHolder%s' style='border: 2px solid %s;' onclick='openSales(%d)' data-index='%d' onmouseover='createTip(this)' onmouseout='cancelTip()'>",
                   g_hub.rarity_anim && rarity_animate(pa) ? " rainbowBorder" : "", rarity_color(pa), ids[k], ids[k]);
        append_skin_img(sb, pa, prev, "tItemSprayImg", "tItemImg");
        sb_append(sb, "<div class='tItemN' style='text-align: left;'>");
        sb_append_esc(sb, pa->name);
        sb_append(sb, "</div></div>");
        value += estimate_value(ids[k]);
    }
    for (int k = 0; k < pad_to - n; k++)
        sb_append(sb, "<div class='tItemHolder' style='border: 2px solid rgba(0,0,0,0);pointer-events: none;'><div draggable='false' class='tItemImg'></div></div>");
    char v[32];
    if (kr > 0) {
        format_num(v, sizeof(v), kr);
        sb_appendf(sb, "<div class='oCurrencyLine'><span class='oCurrencyItem'>+%s KR</span></div>", v);
    }
    comma_number(v, sizeof(v), (double) value);
    sb_appendf(sb, "</div><div class='estOfferVal'>Est. Value <span style='color:#fff'>%s</span> KR</div>", v);
}

// viewOffer(trid, we)
H(h_view_offer) {
    UNUSED;
    if (!g_hub.logged_in) {
        io_po(0, I18N("account.login.required", "Must be logged in"));
        return 1;
    }
    const HubOffer *o = offer_find((int) arg_n(args, argc, 0, -1));
    if (!o) return 1;
    int give[64], take[64];
    memcpy(give, o->give, sizeof(give));
    memcpy(take, o->take, sizeof(take));
    qsort(give, (size_t) o->ngive, sizeof(int), cmp_offer_ids);
    qsort(take, (size_t) o->ntake, sizeof(int), cmp_offer_ids);
    const int most = o->ngive > o->ntake ? o->ngive : o->ntake, pad = (most + 4) / 5 * 5;
    StrBuf sb = {0};
    SprayAnims anims = {.name = "sprayAnm"};
    g_spray_anims = &anims;
    StrBuf body = {0};
    sb_appendf(&body, "<div style='display: block;height:unset'><div class='tHolderL'><div class='tListN inv'>Your Items [%d]</div>", o->ngive);
    offer_side_html(&body, give, o->ngive, pad, o->kr_give);
    sb_append(&body, "</div><div class='tHolderR'><div class='tListN inv'>");
    sb_append_esc(&body, o->name);
    sb_appendf(&body, "'s Items [%d]</div>", o->ntake);
    offer_side_html(&body, take, o->ntake, pad, o->kr_take);
    sb_append(&body, "</div><div style='clear:both'></div></div>"); // (the clear: the native engine's floats)
    g_spray_anims = NULL;
    spray_anims_keyframes(&sb, &anims, 60);
    char *body_s = sb_finish(&body);
    sb_append(&sb, body_s ? body_s : "");
    free(body_s);
    if (o->message && o->message[0]) {
        if (g_hub.show_trade_msg) {
            sb_append(&sb, "<textarea id='tMsg' style='cursor: none;' readonly>");
            sb_append(&sb, o->message); // html-escaped by kn_brain
            sb_append(&sb, "</textarea>");
        } else {
            sb_appendf(&sb, "<div style='color: #eb5656;background-color:#ffffff;text-align:center;'>%s</div>", I18N("social.trades.message", "ENABLE TRADE MESSAGES IN SETTINGS"));
        }
    }
    if (o->we == 0) {
        sb_appendf(&sb, "<div class='tOfferBtn accept' onclick='confOffer(%d, %s)'>Accept Offer</div>", o->trid, o->ntake || o->kr_take > 0 ? "false" : "true");
        sb_appendf(&sb, "<div class='tOfferBtn decline' onclick='declOffer(%d)'>Decline Offer</div>", o->trid);
    } else if (o->we == 1) {
        sb_appendf(&sb, "<div class='tOfferBtn remove' onclick='declOffer(%d)'>Remove Offer</div>", o->trid);
    }
    char *html = sb_finish(&sb);
    generic_popup(html, "offerPop", NULL);
    free(html);
    return 1;
}

// confOffer(trid, nothingForMe)
H(h_conf_offer) {
    UNUSED;
    if (!g_hub.logged_in) {
        io_po(0, I18N("account.login.required", "Must be logged in"));
        return 1;
    }
    const int trid = (int) arg_n(args, argc, 0, 0);
    const int empty = argc > 1 && (args[1].kind == HTML_ARG_BOOL ? args[1].num != 0 : arg_n(args, argc, 1, 0) != 0);
    StrBuf sb = {0};
    if (empty) sb_appendf(&sb, "<div style='color:#fff;font-size:18px;position:relative;margin-bottom:5px;color:#FBC02D'>%s</div>", I18N("social.trades.warning", "You get no items from this trade!"));
    sb_appendf(&sb, "<div style='color:#fff;font-size:20px;position:relative;margin-bottom:15px;'>%s</div><div id='confirmBtn' onclick='offerConfirmed(%d)' onmouseover='SOUND.play(&quot;tick_0&quot;,0.1)'>YES</div><div id='declineBtn' onclick='refreshPopup()' onmouseover='SOUND.play(&quot;tick_0&quot;,0.1)'>NO</div>",
               I18N("social.trades.accept", "Are you sure you want to accept this offer?"), trid);
    char *html = sb_finish(&sb);
    generic_popup(html, "confOfferPop", NULL);
    free(html);
    return 1;
}

// offerConfirmed(trid): io.send("trA", trid) / declOffer(trid): io.send("trC", trid)
H(h_offer_action) {
    UNUSED;
    if (!g_hub.logged_in) {
        io_po(0, I18N("account.login.required", "Must be logged in"));
        return 1;
    }
    const int accept = !strcmp(html_ui_current_handler(), "offerConfirmed");
    const double v[1] = {arg_n(args, argc, 0, 0)};
    char key[48];
    snprintf(key, sizeof(key), "%s-%.0f", accept ? "acc" : "trc", v[0]);
    io_wait(I18N("generic.wait", "Please wait..."));
    send_econ(accept ? "trA" : "trC", 1, v, key);
    return 1;
}

// ---- KR history (KrunkNative "krh": the account's KR ledger)

static const char *kr_source_label(const char *src) {
    static const char *const map[][2] = {
        {"gift-out", "Gift sent"}, {"gift-in", "Gift received"}, {"market-buy", "Market purchase"},
        {"market-sale", "Market sale"}, {"market-fee", "Listing fee"}, {"quicksell", "Quick sell"},
        {"trade-fee", "Trade fee"}, {"trade-escrow", "Trade offer (held)"}, {"trade-return", "Trade offer returned"},
        {"trade-in", "Trade"}, {"trade-out", "Trade"}, {"reward", "Match reward"}, {"challenge", "Challenge"},
        {"spin", "Spin"}, {"turf", "Turf Wars"}, {"starting", "Starting KR"}, {"admin", "Adjustment"},
        {"mail", "Mail"}, {"clan-deposit", "Clan deposit"}, {"clan-withdraw", "Clan withdrawal"},
        {"battlepass", "Battle Pass"}, {"battlepass-skip", "Battle Pass tiers"}, {"map_reward", "Map reward"},
        {"map_funds", "Map funds"},
    };
    for (size_t i = 0; src && i < sizeof(map) / sizeof(map[0]); i++)
        if (!strcmp(src, map[i][0])) return map[i][1];
    return src ? src : "";
}

H(h_kr_history) {
    UNUSED;
    html_ui_stop_propagation(); // #profileKR sits in #profileInfo (onclick: the own profile)
    if (!g_hub.logged_in || !g_hub.brain) return 1;
    io_wait(I18N("generic.wait", "Please wait..."));
    const double v[1] = {30};
    send_ints("krh", 1, v);
    return 1;
}

static void io_krh(const cJSON *rows) {
    StrBuf sb = {0};
    sb_append(&sb, "<div class='popHeader'>KR History</div><div style='max-height:430px;overflow-y:auto;font-size:15px;'>"
                   "<table style='width:100%;border-collapse:collapse;text-align:left'>");
    int n = 0;
    const cJSON *r;
    cJSON_ArrayForEach(r, rows) {
        const double amount = jnum(r, "amount"), fee = jnum(r, "fee");
        char amt[32], when[48] = "", feetxt[48] = "";
        comma_number(amt, sizeof(amt), fabs(amount));
        const time_t t = (time_t) (jnum(r, "tim") / 1000.0);
        struct tm tm;
#ifdef _WIN32
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        strftime(when, sizeof(when), "%m-%d %H:%M", &tm);
        if (fee > 0) {
            char f[32];
            comma_number(f, sizeof(f), fee);
            snprintf(feetxt, sizeof(feetxt), "fee %s", f);
        }
        const char *cp = jstr(r, "cp");
        sb_appendf(&sb, "<tr style='border-bottom:1px solid rgba(255,255,255,0.08)'><td style='padding:5px 4px;color:rgba(255,255,255,0.4)'>%s</td><td style='padding:5px 4px;color:#fff'>%s</td><td style='padding:5px 4px;color:rgba(255,255,255,0.6)'>",
                   when, kr_source_label(jstr(r, "source")));
        if (cp) sb_append_esc(&sb, cp);
        sb_appendf(&sb, "</td><td style='padding:5px 4px;color:rgba(255,255,255,0.4)'>%s</td><td style='padding:5px 4px;text-align:right;color:%s'>%s%s KR</td></tr>",
                   feetxt, amount < 0 ? "#eb5656" : "#5ce05a", amount < 0 ? "-" : "+", amt);
        n++;
    }
    sb_append(&sb, "</table>");
    if (!n) sb_append(&sb, "<div style='color:rgba(255,255,255,0.5);text-align:center;padding:20px'>No KR movements in the last 30 days</div>");
    sb_append(&sb, "</div>");
    char *html = sb_finish(&sb);
    generic_popup(html, "offerPop", "width:680px"); // v7's offer box, narrower
    free(html);
}

// ---- the profile's Listings tab (io_ul) and the listing page

// The rows of a list whose cards remove_card finds by uid (the profile's listings, the listing page).
static void set_market_rows(MarketRow *rows, const int n, const char *sub) {
    free(g_market_rows);
    g_market_rows = rows;
    g_market_row_count = n;
    snprintf(g_market_sub, sizeof(g_market_sub), "%s", sub);
}

// io_ul(rows): the profile's Listings tab (hub-10.0.7.js:4104-4154): market cards without the seed, the video /
// projection badges or the Share / Preview actions; Purchase is showPopup("market", sk, thumbnail, label, skid, kr, c,
// ms, the profile's name, uid, id)
static void io_ul(const cJSON *rows) {
    clear_pops();
    const int n = cJSON_GetArraySize(rows);
    StrBuf sb = {0};
    if (n > 0) {
        MarketRow *m = calloc((size_t) n, sizeof(MarketRow));
        int k = 0;
        const cJSON *r;
        cJSON_ArrayForEach(r, rows) {
            if (!m) break;
            MarketRow *row = &m[k];
            row->uid = k;
            row->skin = (int) jnum(r, "sk");
            row->skin_id = (int) jnum(r, "skid");
            row->count = (int) jnum(r, "c");
            row->funds = jnum(r, "kr");
            row->salestart = jnum(r, "ms");
            row->market_id = (int) jnum(r, "id");
            snprintf(row->player, sizeof(row->player), "%s", g_hub.profile_name);
            card_meta(r, &row->meta);
            k++;
        }
        set_market_rows(m, k, "market");
        sb_appendf(&sb, "<div class='scrollItemNew' style='width:%dpx'>", n > 3 ? 1000 : 900);
        SprayAnims anims = {.name = "sprayAnm"};
        g_spray_anims = &anims;
        for (int i = 0; i < k; i++) {
            const ViewerSkin *pe = hub_skin(m[i].skin);
            if (!pe || (pe->flags & (VS_FREE | VS_BLOCKED))) continue;
            char prev[256], price[32];
            skin_preview(prev, sizeof(prev), pe);
            const char *col = rarity_color(pe), *label = skin_type_label(pe);
            sb_appendf(&sb, "<div class='marketCard%s' id='itemCardmarket_%d' style='color:%s;border:5px solid %s' onmouseenter='SOUND.play(`tick_0`,0.1)' data-index='%d'",
                       g_hub.rarity_anim && rarity_animate(pe) ? " rainbow" : "", i, col, col, m[i].skin);
            append_meta_attrs(&sb, &m[i].meta, 0, 0);
            sb_append(&sb, " onmouseover='createTip(this)' onmouseout='cancelTip()'>");
            append_card_name(&sb, pe, &m[i].meta);
            sb_appendf(&sb, "<div class='itemOwn'>%s</div>", label);
            if (hub_dev()) sb_appendf(&sb, "<div class='itemOwn'>ID: %d</div>", m[i].skin);
            append_skin_img(&sb, pe, prev, "sprayMarketImg", market_img_class(pe));
            comma_number(price, sizeof(price), m[i].funds);
            if (m[i].funds) sb_appendf(&sb, "<div class='marketPrice'>%s<span style='color:#fff'> KR</span></div>", price);
            else sb_append(&sb, "<div class='marketPrice'><span style='opacity:0'>s</span></div>");
            append_card_badges(&sb, pe, 1);
            sb_appendf(&sb, "<div class='cardActions'><a class='cardAction' onmouseenter='SOUND.play(`tick_0`,0.1)' onclick='showPopup(&quot;market&quot;,%d,&quot;%s&quot;,&quot;%s&quot;,&quot;%d&quot;,&quot;%.15g&quot;,%d,&quot;%.15g&quot;,&quot;",
                       m[i].skin, prev, label, m[i].skin_id, m[i].funds, m[i].count, m[i].salestart);
            sb_popup_arg(&sb, g_hub.profile_name);
            sb_appendf(&sb, "&quot;,%d,%d)'>Purchase</a><div class='cardActionSep'></div>", i, m[i].market_id);
            sb_appendf(&sb, "<a class='cardAction' onmouseenter='SOUND.play(`tick_0`,0.1)' onclick='searchItem(%d)'>Search</a><div class='cardActionSep'></div><a class='cardAction' onmouseenter='SOUND.play(`tick_0`,0.1)' onclick='openSales(%d)'>Info</a></div></div>", m[i].skin, m[i].skin);
        }
        g_spray_anims = NULL;
        sb_append(&sb, "</div>");
        spray_anims_keyframes(&sb, &anims, 200);
    } else {
        sb_append(&sb, "<div class='scrollItemNew'><div class='noneText'>No Market Listings!</div></div>");
    }
    char *html = sb_finish(&sb);
    set_html("pTabContent_listings", html);
    free(html);
}

// io_0("listing", q, {sk, ms, kr, id}) (hub-10.0.7.js:3495-3537): one listing on its own page (?p=listing&i=<id>),
// its own markup (pItemDesc2 / pItemTime2 / priceP2 / pricePF2 / pItemButton2, buyItem(id, null)). v10's spray branch
// reads an undefined `src` and throws; KrunkNative draws the thumbnail. A blocked item's note goes to
// #itemsalesHolder, as v10 writes it.
static void hub_render_listing(const cJSON *t) {
    const int sk = cJSON_IsObject(t) ? (int) jnum(t, "sk") : -1;
    const ViewerSkin *pe = hub_skin(sk);
    if (cJSON_IsObject(t) && !pe) return;
    if (!pe) {
        set_market_note("listingHolder", STR_MARKET_NONE);
        set_display("listingHolder", "block");
        return;
    }
    if (pe->flags & VS_BLOCKED) {
        set_market_note("itemsalesHolder", STR_MARKET_NONE);
        set_display("listingHolder", "block");
        return;
    }
    const AccountInfo *acc = g_hub.logged_in ? active_account() : NULL;
    const double ms = jnum(t, "ms"), kr = jnum(t, "kr");
    const double elapsed = fmax(0, now_epoch_ms() - ms);
    if (elapsed < SALE_GRACE_MS) {
        g_grace_start = ms;
        if (!g_grace_timer) g_grace_timer = html_timer_add(1000, grace_tick, NULL);
    }
    char prev[256], a[32], b[32], c[32], req[64];
    skin_preview(prev, sizeof(prev), pe);
    StrBuf sb = {0};
    sb_append(&sb, "<div class='scrollView'><div class='scrollItem' style='position:relative;overflow-y:auto;padding-top: 20px;padding: 15px;'>");
    append_popup_head(&sb, pe, sk, prev);
    sb_appendf(&sb, "<div class='popupInfo'><div class='pItemName%s' style='color:%s'>", g_hub.rarity_anim && rarity_animate(pe) ? " rainbowText" : "", rarity_color(pe));
    sb_append_esc(&sb, pe->name);
    sb_appendf(&sb, "</div><div class='pItemDesc2'>%s</div>", skin_type_label(pe));
    if (ms) {
        char ago[48], listed[96];
        readable_time(ago, sizeof(ago), elapsed);
        I18N_F(listed, "social.market.listed", "listed {0} ago", ago);
        sb_appendf(&sb, "<div class='pItemTime2'>%s</div>", listed);
    }
    const int grace = elapsed < SALE_GRACE_MS;
    const char *block = market_block(acc, 0, req, sizeof(req));
    const double funds = acc ? acc->funds : 0, left = funds - kr;
    comma_number(a, sizeof(a), funds);
    comma_number(b, sizeof(b), kr);
    comma_number(c, sizeof(c), left);
    sb_appendf(&sb, "<div class='pItemPricing'><div class='priceP2'>%s <span style='color:#000'>KR</span></div><div class='priceP2'>-%s <span style='color:#000'>KR</span></div><div class='pricePF2' style='%s'>%s <span style='color:#000'>KR</span></div></div>",
               a, b, left < 0 ? "color:#ed4242" : "", c);
    sb_appendf(&sb, "<div class='pItemButton2' style='%s' ", block || grace ? "background-color:#2b2b2b" : left < 0 ? "background-color:#ed4242" : "");
    if (!block && !grace && left >= 0) sb_appendf(&sb, "onclick='buyItem(%d,null)'onmouseover='SOUND.play(&quot;tick_0&quot;,0.1)'", (int) jnum(t, "id"));
    if (grace) {
        readable_time_s(a, sizeof(a), SALE_GRACE_MS - elapsed, 1);
        char ready[96];
        I18N_F(ready, "social.market.grace", "ready in {0}", a);
        sb_appendf(&sb, "><span id='graceTimer' style='color:rgba(255,255,255,0.6)';'>%s</span></div>", ready);
    } else {
        sb_appendf(&sb, ">%s</div>", block ? block : left < 0 ? I18N("social.market.cant", "Can't Afford") : I18N("social.market.purchase", "Purchase"));
    }
    sb_append(&sb, "</div></div></div></div>");
    if (pe->type == 4 && pe->frames) sb_appendf(&sb, "<style>@keyframes sprayPopAnm%d {100%%{background-position:-%dpx}}</style>", pe->frames, pe->frames * 220);
    char *html = sb_finish(&sb);
    set_html("listingHolder", html);
    free(html);
    set_display("listingHolder", "block");
}

// ---- economy frames (handle_reply_frame -> hub_market_frame); 1 = handled
static int hub_economy_frame(const char *label, const cJSON *frame) {
    const cJSON *a1 = cJSON_GetArrayItem(frame, 1), *a2 = cJSON_GetArrayItem(frame, 2), *a3 = cJSON_GetArrayItem(frame, 3);
    if (!strcmp(label, "ecfg")) { // the server's rules
        econ_rules_apply(a1);
    } else if (!strcmp(label, "gir")) {
        io_gir(a1);
    } else if (!strcmp(label, "bmr")) { // io_bmr({item, kr, f} | null)
        io_po(cJSON_IsObject(a1), cJSON_IsObject(a1) ? NULL : "Item Unavailable");
        if (cJSON_IsObject(a1)) hub_set_funds(jnum(a1, "f"));
    } else if (!strcmp(label, "trD")) {
        io_trd(cJSON_GetStringValue(a1), (int) cJSON_GetNumberValue(a2), a3);
    } else if (!strcmp(label, "trR")) {
        io_trr(a1, a2, cJSON_GetStringValue(a3));
    } else if (!strcmp(label, "krh")) {
        io_krh(a1);
    } else if (!strcmp(label, "ul")) {
        io_ul(a1);
    } else if (!strcmp(label, "pRes")) { // "Declined All" (trCA)
        const char *kind = cJSON_GetStringValue(a1);
        io_po(kind && !strcmp(kind, "SUCCESS"), kind && !strcmp(kind, "SUCCESS") ? cJSON_GetStringValue(a3) : cJSON_GetStringValue(a2));
    } else if (!strcmp(label, "econN")) { // a trade / sale / gift of someone else touched this account
        const HtmlElem holder = el("popupHolder");
        if (!strcmp(g_hub.v, "market") && !strcmp(g_hub.b, "trades")) request_page();
        if (!holder || !html_dom_is_shown(holder)) io_po(1, cJSON_GetStringValue(a2));
    } else if (!strcmp(label, "ua")) { // the header's KR after a server-side change
        const cJSON *info = a2;
        if (cJSON_IsObject(info) && cJSON_GetObjectItemCaseSensitive(info, "funds")) hub_set_funds(jnum(info, "funds"));
    } else {
        return 0;
    }
    return 1;
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
        io_wait(I18N("generic.wait", "Please wait..."));
        const double v[1] = {g_hub.profile_id};
        send_ints("uml", 1, v);
    }
    return 1;
}

// openItemTab(tab): itemsales avg / sold / owners
static void open_item_tab(const char *tab) {
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
}

H(h_open_item_tab) {
    UNUSED;
    const char *tab = arg_s(ev, args, argc, 0);
    if (tab) open_item_tab(tab);
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

// switchStat(tab): the profile's six stat tabs (hub-10.0.7.js:2376-2392)
// knPmMatch(i) / knPmSort(i, column): an entry opens / closes, a header sorts its scoreboard (again: the other way)
H(h_pm_match) {
    UNUSED;
    const int i = (int) arg_n(args, argc, 0, -1);
    if (i < 0 || i >= PM_MAX) return 1;
    g_pm.open[i] = !g_pm.open[i];
    hub_match_history_render();
    return 1;
}
H(h_pm_sort) {
    UNUSED;
    const int i = (int) arg_n(args, argc, 0, -1), col = (int) arg_n(args, argc, 1, -1);
    if (i < 0 || i >= PM_MAX || col < 0 || col > 7) return 1;
    const int cur = g_pm.sort[i] ? abs(g_pm.sort[i]) - 1 : 1, desc = g_pm.sort[i] ? g_pm.sort[i] > 0 : 1;
    g_pm.sort[i] = (signed char) ((col + 1) * (cur == col ? (desc ? -1 : 1) : 1));
    if (ev && ev->target) html_ui_stop_propagation();
    hub_match_history_render();
    return 1;
}

H(h_switch_stat) {
    UNUSED;
    const int tab = (int) arg_n(args, argc, 0, 0);
    if (tab == STAT_TAB_COUNT - 1) hub_match_history_render(); // Match History: mounted when chosen
    else if (el("matchHistoryContainer")) set_html("matchHistoryContainer", "");
    for (int i = 0; i < STAT_TAB_COUNT; ++i) {
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

// window.viewItem(index, mode) (hub-10.0.7.js:2054-2069): the item popup with the 3D showcase iframe
// (viewer.html?nobg&showcase=<i>; natively drawn into the iframe's box by viewer_embed.h), a player card's / emoji's /
// paint's thumbnail instead; its Black Market / limT / Vaulted / Limited label; mode 2 offers the model viewer.
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
    sb_appendf(&sb, "<div class='itemViewName%s%s' style='color:%s;'>", sk->rarity != 5 ? " blackShad" : "", g_hub.rarity_anim && rarity_animate(sk) ? " rainbow" : "", col); // 5: CONTRABAND
    sb_append_esc(&sb, sk->name);
    sb_append(&sb, "</div>");
    const char *lim_t = skin_str(index, "limT");
    const int flat = sk->type == 16 || sk->type == 17 || sk->type == 18; // PLAYER_CARD, EMOJI, PAINT
    if (skin_true(index, "blackM")) {
        sb_append(&sb, "<div class='itemViewSea' style='background:#00ffff'>Black Market</div>");
    } else if (lim_t) {
        sb_appendf(&sb, "<div class='itemViewSea' style='background:%s'>", ui_loadout_tag_color(lim_t));
        sb_append_esc(&sb, lim_t);
        sb_append(&sb, "</div>");
    } else if (sk->type != 18 && (sk->seas != g_viewer_current_season || (skin_true(index, "limited") && (sk->flags & VS_NO_SPIN) && !skin_true(index, "itemShop")))) {
        sb_append(&sb, "<div class='itemViewSea'>Vaulted</div>");
    } else if (skin_true(index, "limited") || (sk->flags & VS_BLOCKED)) {
        sb_append(&sb, "<div class='itemViewSea' style='background:#faa332;'>Limited</div>");
    }
    if (flat) sb_appendf(&sb, "<div class='viewItem%s' style='background-image:url(%s)'></div>", sk->type == 16 ? "Card" : sk->type == 17 ? "Emoji" : "Paint", url);
    else {
        char paint[80]; // the account's own worn / painted copy on the showcase (v10's instance → viewer URL, deob-10.0.6.js:221934)
        ui_loadout_paint_query(index, paint, sizeof(paint));
        sb_appendf(&sb, "<iframe style='width: 100%%;height: 450px;border:none;margin-top:10px;' src='/viewer.html?nobg&showcase=%d%s'></iframe>", index, paint);
    }
    if ((int) arg_n(args, argc, 1, 0) == 2) {
        if (!flat) sb_appendf(&sb, "<div id='itemInfoBtn' onclick='openViewer(%d)'>%s</div>", index, I18N("item.view.open", "Open Model Viewer"));
    } else {
        sb_appendf(&sb, "<div id='itemInfoBtn' onclick='openSales(%d)'>%s</div>", index, I18N("item.view.more", "More Information"));
    }
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
    html_dom_set_text(ev->target, I18N("menu.btn.invite.clicked", "Copied"));
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
// itemListScroll (f496 on reaching the bottom: v7.1.4's next 500 cards): the virtual grid of
// items_show() already spans every item, so there is nothing to append.
H(h_item_list_scroll) { UNUSED; return 1; }

H(h_maps_switch_tab) { UNUSED; list_switch_tab(0, (int) arg_n(args, argc, 0, 0), 0); return 1; }
H(h_mods_switch_tab) { UNUSED; list_switch_tab(1, (int) arg_n(args, argc, 0, 0), 0); return 1; }
H(h_maps_load_more) { (void) user; (void) args; (void) argc; list_load_more(0, ev ? ev->target : 0); return 1; }
H(h_mods_load_more) { (void) user; (void) args; (void) argc; list_load_more(1, ev ? ev->target : 0); return 1; }

// windows[0].searchMaps([text, category]) (v10 searchMaps: mpSrch's value and mpCat's option). A Popular Categories tile
// passes "" and its category, which fill the box and the select first; the box's button and Enter pass nothing.
H(h_maps_search) {
    UNUSED;
    char value[128] = "";
    const HtmlElem input = el("mpSrch"), cat = el("mpCat");
    const char *text = arg_s(ev, args, argc, 0);
    if (text && input) html_dom_set_value(input, text);
    if (argc > 1 && cat) {
        char pick[16];
        snprintf(pick, sizeof(pick), "%d", (int) arg_n(args, argc, 1, -1));
        html_dom_set_value(cat, pick);
    }
    if (input) html_dom_get_value(input, value, sizeof(value));
    char selected[16] = "-1";
    if (cat) html_dom_get_value(cat, selected, sizeof(selected));
    set_html("searchResultsMA", "Loading...");
    api_request("/search", 0, 0, "map", value, atoi(selected)); // API_CLIENT.searchMap(value, category): the server matches names and the category
    return 1;
}

H(h_mods_search) {
    UNUSED;
    char value[128] = "";
    const HtmlElem input = el("mdSrch");
    if (input) html_dom_get_value(input, value, sizeof(value));
    set_html("searchResultsMO", "Loading...");
    api_request("/search", 3, 0, "mod", value, NO_CATEGORY);
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
// playMap: the name is percent-encoded, so a space or & # % + reaches the game's ?play= whole
// (servers_ui_query decodes %XX and +; app_url.c keeps the game's query)
H(h_play_map) {
    UNUSED;
    char enc[400];
    backend_url_encode(enc, sizeof(enc), arg_s(ev, args, argc, 0));
    open_hub_url("/?play=", enc, 1);
    return 1;
}
// hostMap(name) (hub-10.0.7.js:5809-5812): saveVal("mapToLoad", name), then "/" in a new tab, whose game opens the Host
// Game window on that map (app_mode.c map_to_load: selectHostMap, deob-10.0.7.js:229320-229322)
H(h_host_map) {
    UNUSED;
    const char *name = arg_s(ev, args, argc, 0);
    if (!name || !*name) return 1;
    html_storage_set("mapToLoad", name);
    open_hub_url("/", "", 1);
    return 1;
}
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
    set_display("mapVid", g_map_tab == 0 && el("mapVidLink") ? "block" : "none"); // the video link is part of About
    set_display("activeGames", g_map_tab == 1 ? "block" : "none");
    set_display("mapLeaders", g_map_tab == 2 ? "block" : "none");
    if (el("mapData")) set_display("mapData", g_map_tab == 3 ? "block" : "none");
    if (el("mapVersions")) set_display("mapVersions", g_map_tab == 4 ? "block" : "none");
    if (g_map_tab == 4 && can_edit) hub_map_versions_show();
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
    // UTILS.copyToClipboard("https://krunker.io/?play=" + name) / "?mod=" (hub-10.0.7.js:5813-5822); this host's origin
    char origin[128], url[512];
    html_page_origin(origin, sizeof(origin));
    snprintf(url, sizeof(url), "%s/?%s=%s", origin, strcmp(html_ui_current_handler(), "shareModLink") ? "play" : "mod", arg_s(ev, args, argc, 1));
    html_clipboard_write(url);
    html_dom_set_text(ev->target, I18N("menu.btn.invite.clicked", "Copied"));
    html_timer_add(1250, share_restore, (void *) (uintptr_t) ev->target);
    return 1;
}

// Login-only actions: io_po(null, "Must be logged in") / "Please Login"
H(h_publish_popup) { UNUSED; io_po(0, I18N("account.login.required", "Must be logged in")); return 1; }

// publishModPopup(publish, mod) (hub-10.0.7.js:6732-6759): the Publish / Update Mod popup. Its form and upload are the
// menu's Upload Mod (mods_ui.c: the brain's /mods/publish, chunked), whose publish of a name the account already has
// updates that mod; mod (plm's answer) prefills the name and the thumbnail of an update.
static void hub_mod_published(void) { io_po(1, "Mod Uploaded!"); } // hub-10.0.7.js:6829 (the upload's SUCCESS)

static void hub_mod_popup(const cJSON *mod) {
    if (!g_hub.logged_in) {
        io_po(0, I18N("account.login.required", "Must be logged in"));
        return;
    }
    char id[32] = "";
    int thumb = 0;
    const char *name = mod ? jstr(mod, "mod_name") : NULL;
    if (mod) {
        js_num(id, sizeof(id), jnum(mod, "mod_id"));
        const cJSON *info = cJSON_GetObjectItemCaseSensitive(mod, "mod_info");
        cJSON *parsed = cJSON_IsString(info) ? cJSON_Parse(info->valuestring) : NULL;
        const cJSON *t = cJSON_GetObjectItemCaseSensitive(parsed ? parsed : info, "t");
        thumb = t && !cJSON_IsFalse(t) && !(cJSON_IsNumber(t) && t->valuedouble == 0);
        cJSON_Delete(parsed);
    }
    char *form = mods_ui_hub_publish(name, id, thumb, hub_mod_published);
    if (!form) {
        io_po(0, I18N("account.login.required", "Must be logged in"));
        return;
    }
    StrBuf sb = {0};
    sb_append(&sb, "<div><div class='pubHed'>");
    sb_append(&sb, name && *name ? "Update Mod" : "Publish Mod");
    sb_append(&sb, "</div>");
    sb_append(&sb, form);
    sb_append(&sb, "</div>");
    free(form);
    char *html = sb_finish(&sb);
    generic_popup(html, "publishPop", NULL);
    free(html);
}

H(h_publish_mod_popup) {
    UNUSED;
    hub_mod_popup(NULL);
    return 1;
}

// updateMod(name) (hub-10.0.7.js:6640-6646): ["plm", name] -> ["plm", false, mod] (hub_map_tools_frame)
H(h_update_mod) {
    UNUSED;
    if (!g_hub.logged_in) {
        io_po(0, I18N("account.login.required", "Must be logged in"));
        return 1;
    }
    const char *name = arg_s(ev, args, argc, 0);
    if (!name || !*name) return 1;
    io_wait("Please Wait...");
    cJSON *f = cJSON_CreateArray();
    cJSON_AddItemToArray(f, cJSON_CreateString("plm"));
    cJSON_AddItemToArray(f, cJSON_CreateString(name));
    hub_send(f);
    return 1;
}

// the popup's file pickers and Publish / Update (mods_ui.c)
H(h_mod_form) {
    UNUSED;
    return mods_ui_handle(html_ui_current_handler(), ev, args, argc) >= 0;
}
H(h_noop) { UNUSED; return 1; }

// followUser(): toggle the button, io.send("f", profile.player_name, follow)
H(h_follow_user) {
    UNUSED;
    const HtmlElem b = el("followBtn");
    if (!b || !g_hub.logged_in) return 1;
    const int follow = html_dom_has_class(b, "follow");
    html_dom_set_class(b, "follow", !follow);
    html_dom_set_class(b, "unfollow", follow);
    html_dom_set_text(b, follow ? I18N("generic.unfollow", "Unfollow") : I18N("generic.follow", "Follow"));
    cJSON *f = cJSON_CreateArray();
    cJSON_AddItemToArray(f, cJSON_CreateString("f"));
    cJSON_AddItemToArray(f, cJSON_CreateString(g_hub.profile_name));
    cJSON_AddItemToArray(f, cJSON_CreateNumber(follow));
    hub_send(f);
    return 1;
}

// viewFollowers / viewFollowing: io.send("gfs"|"gfg", name, id, count, captcha)
// viewFollowers(own) / viewFollowing(own) (hub-10.0.7.js:5353-5359, 5373-5381): with own (the feed's counters) the
// lists are the signed-in account's, whose id, name and counts become the viewed profile's first; then
// io.send("gfs" | "gfg", player_name, player_id)
H(h_view_follows) {
    UNUSED;
    if (!g_hub.brain) return 1;
    if (argc >= 1 && arg_n(args, argc, 0, 0)) {
        const AccountInfo *acc = g_hub.logged_in ? active_account() : NULL;
        g_hub.profile_id = acc ? acc->id : 0;
        snprintf(g_hub.profile_name, sizeof(g_hub.profile_name), "%s", acc ? acc->name : "");
        g_hub.profile_following = acc ? acc->following : 0;
        g_hub.profile_followed = acc ? acc->followers : 0;
    }
    cJSON_Delete(g_follows.rows);
    g_follows.rows = NULL;
    const int following = !strcmp(html_ui_current_handler(), "viewFollowing");
    io_wait("Loading...");
    cJSON *f = cJSON_CreateArray();
    cJSON_AddItemToArray(f, cJSON_CreateString(following ? "gfg" : "gfs"));
    cJSON_AddItemToArray(f, cJSON_CreateString(g_hub.profile_name));
    cJSON_AddItemToArray(f, cJSON_CreateNumber(g_hub.profile_id));
    hub_send(f);
    return 1;
}

// ------------------------------------------------------------------ followers / following (v10)
// viewFollowers / viewFollowing ask the brain ("gfs" / "gfg"), fwsr / fwr come back with the rows
// (services/brain/profiles.c) and the popup renders them (social/renamed.js:5346-5399, _1394/_1403).

// fwsr / fwr (hub-10.0.7.js:5360-5372, 5382-5394): "Your followers" / "<name>'s followers", "You follow" /
// "<name> follows", then a row per player (Since <date>); a row's button is Follow, except the viewer's own
// following list, whose rows say Unfollow (deleteFollower)
static void follows_popup(void) {
    StrBuf sb = {0};
    char num[32];
    comma_number(num, sizeof(num), g_follows.count);
    sb_append(&sb, "<div style='margin-top:-8px;font-size:");
    if (g_follows.following) {
        sb_append(&sb, "22px;margin-bottom:10px;color:#fff;'>");
        if (g_follows.own) sb_append(&sb, "You follow");
        else {
            sb_append_esc(&sb, g_follows.title);
            sb_append(&sb, " follows");
        }
        sb_appendf(&sb, "<div style='float:right;color:rgba(255,255,255,0.8)' id='followCP'>%s People</div></div>", num);
    } else {
        sb_append(&sb, " 22px;margin-bottom:10px;color:#fff;'>");
        if (g_follows.own) sb_append(&sb, "Your followers");
        else {
            sb_append_esc(&sb, g_follows.title);
            sb_append(&sb, "'s followers");
        }
        sb_appendf(&sb, "<div style='float: right;color:rgba(255,255,255,0.8);' id='followerCP'>%s People</div></div>", num);
    }
    if (cJSON_GetArraySize(g_follows.rows)) {
        const cJSON *r;
        int i = 0;
        cJSON_ArrayForEach(r, g_follows.rows) {
            const char *name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(r, "friendname"));
            const int premium = (int) jnum(r, "subscription");
            const int featured = (int) jnum(r, "featured");
            sb_append(&sb, "<div class='friendBHold'><div class='leaderText'><img class='folPfp' onerror='this.onerror=null;this.src=\"./img/player_pfp.png\";' src='");
            sb_pfp(&sb, jnum(r, "friendid"), 1, "");
            sb_append(&sb, "'/><div class='folInfHldr'>");
            if (premium) sb_append(&sb, "<i class='material-icons' style='color:#FBC02D;margin-top:-8px;font-size:28px;vertical-align:middle;'>beenhere</i>");
            if (featured) sb_append(&sb, "<i class='material-icons' style='color:#40C4FF;margin-top:-8px;margin-right: 4px;font-size:28px;vertical-align:middle;'>check_circle</i>");
            sb_append(&sb, "<a target='_blank' href='/social.html?p=profile&q=");
            sb_append_esc(&sb, name ? name : "");
            sb_append(&sb, "' class='lName' style='color:rgba(255,255,255,0.9)'>");
            sb_append_esc(&sb, name ? name : "");
            sb_appendf(&sb, "</a><div style='color:rgba(255,255,255,0.4);font-size:14px;'>%s <span style='color:rgba(255,255,255,0.6)'>", I18N("social.since", "Since"));
            get_date(num, sizeof(num), (char *) cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(r, "date")));
            sb_append_esc(&sb, num);
            sb_append(&sb, "</span></div></div></div>");
            if (g_follows.following && g_follows.own) {
                sb_appendf(&sb, "<div class='reqFollB buttonR' style='float:right;' title='Unfollow' onclick='deleteFollower(%d, this)'>Unfollow</div>", i);
            } else if (g_follows.following || g_hub.logged_in) {
                sb_append(&sb, "<div class='reqFollB' style='float:right;' onclick='instaFollo(&quot;");
                sb_append_esc(&sb, name ? name : "");
                sb_append(&sb, "&quot;,this)'>Follow</div>");
            }
            sb_append(&sb, "</div>");
            i++;
        }
    } else {
        sb_append(&sb, "None");
    }
    char *html = sb_finish(&sb);
    generic_popup(html, "followPop", NULL);
    free(html);
}

// fwsr(rows, name, id) / fwr(rows, name, id)
static void follows_reply(const cJSON *rows, const char *name, const double id, const int following) {
    const AccountInfo *acc = active_account();
    cJSON_Delete(g_follows.rows);
    g_follows.rows = cJSON_IsArray(rows) ? cJSON_Duplicate(rows, 1) : cJSON_CreateArray();
    g_follows.open = 1;
    g_follows.following = following;
    g_follows.own = g_hub.logged_in && acc && id > 0 && acc->id == (int) id;
    snprintf(g_follows.title, sizeof(g_follows.title), "%s", name ? name : "");
    g_follows.count = following ? g_hub.profile_following : g_hub.profile_followed;
    follows_popup();
}

H(h_insta_follo) {
    (void) user;
    const char *name = arg_s(ev, args, argc, 0);
    if (!name || !g_hub.logged_in || !g_hub.brain) return 1;
    const HtmlElem b = ev ? ev->target : 0;
    if ((int) arg_n(args, argc, 2, 0) == 1) {
        if (b) {
            char cls[96] = "";
            html_dom_get_attr(b, "class", cls, sizeof(cls));
            if (strstr(cls, "follBtnGivD")) return 1; // el.onclick = function () {}
            html_dom_set_class(b, "follBtnGivD", 1);
        }
    } else if (b) {
        char cls[96] = "";
        html_dom_get_attr(b, "class", cls, sizeof(cls));
        if (!strcmp(cls, "disFolB")) return 1;
        html_dom_set_attr(b, "class", "disFolB");
    }
    cJSON *f = cJSON_CreateArray();
    cJSON_AddItemToArray(f, cJSON_CreateString("f"));
    cJSON_AddItemToArray(f, cJSON_CreateString(name));
    cJSON_AddItemToArray(f, cJSON_CreateNumber(1));
    hub_send(f);
    return 1;
}

// deleteFollower(index, this) (hub-10.0.7.js:5395-5406): the row's player is unfollowed (io.send("f", name, 0)),
// the row goes, and the profile's following count and the popup's "N People" go down by one
H(h_delete_follower) {
    (void) user;
    const cJSON *row = cJSON_GetArrayItem(g_follows.rows, (int) arg_n(args, argc, 0, -1));
    const char *name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(row, "friendname"));
    if (!name) return 1;
    cJSON *f = cJSON_CreateArray();
    cJSON_AddItemToArray(f, cJSON_CreateString("f"));
    cJSON_AddItemToArray(f, cJSON_CreateString(name));
    cJSON_AddItemToArray(f, cJSON_CreateNumber(0));
    hub_send(f);
    const HtmlElem btn = argc >= 2 && args[1].kind == HTML_ARG_THIS && ev ? ev->target : 0;
    if (btn) {
        const HtmlElem holder = html_dom_closest(btn, ".friendBHold");
        if (holder) html_dom_remove(holder);
    }
    g_hub.profile_following -= 1;
    char num[32], text[160];
    format_num(num, sizeof(num), g_hub.profile_following); // followC.firstChild: the profile's count
    snprintf(text, sizeof(text), "%s<span class='follCount'>%s</span>", num, I18N("generic.following", "following"));
    set_html("followC", text);
    format_num(num, sizeof(num), g_hub.profile_following); // iíiïiiï, as the profile's counter (hub-10.0.7.js:5403-5405)
    snprintf(text, sizeof(text), "%s People", num);
    const HtmlElem cp = el("followCP");
    if (cp) html_dom_set_text(cp, text);
    return 1;
}

// makePost(): io.send("ps", {txt})
// KR giveaway post (v10 makePost with the giveaway switch): POST /social/post {type: 9, text, funds}
static void giveaway_posted(void *user, const int status, const cJSON *data, const cJSON *error) {
    (void) user; (void) data;
    g_hub.posting = 0;
    set_html("postButton", "Post");
    if (status >= 200 && status < 300) {
        const HtmlElem input = el("postInput");
        if (input) html_dom_set_value(input, "");
        g_hub.giveaway = 0;
        set_style("givaButton", "background-color", "rgba(0,0,0,0.3)");
        set_display("krInptHol", "none");
        request_page();
        return;
    }
    io_po(0, v10_gapi_error_text(error, STR_MARKET_FAILED));
}

static void hub_post_giveaway(const char *text) {
    char kr[32] = "";
    const HtmlElem kin = el("postKR");
    if (kin) html_dom_get_value(kin, kr, sizeof(kr));
    cJSON *body = cJSON_CreateObject();
    cJSON_AddNumberToObject(body, "type", 9);
    cJSON_AddStringToObject(body, "text", text);
    cJSON_AddNumberToObject(body, "funds", (double) strtoll(kr, NULL, 10));
    g_hub.posting = 1;
    set_html("postButton", "<div class='lds-rings'><div></div><div></div><div></div><div></div></div>");
    if (!v10_gapi("POST", "/social/post", body, giveaway_posted, NULL)) {
        g_hub.posting = 0;
        set_html("postButton", "Post");
        io_po(0, NULL);
    }
    cJSON_Delete(body);
}

H(h_make_post) {
    UNUSED;
    char text[1024] = "";
    const HtmlElem input = el("postInput");
    if (input) html_dom_get_value(input, text, sizeof(text));
    if (!text[0] || g_hub.posting || !g_hub.logged_in) return 1;
    if (g_hub.giveaway) {
        hub_post_giveaway(text);
        return 1;
    }
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

// ------------------------------------------------------------------ staff, report, creations, giveaway (v10 window.* popups)

// The signed-in account is a developer (verify, badges, feature / delete creations) or a
// developer / moderator (flag, the server checks the same rule).
static int hub_is_dev(void) {
    const AccountInfo *acc = g_hub.logged_in ? active_account() : NULL;
    return acc && acc->is_dev;
}

static int hub_is_staff(void) {
    const AccountInfo *acc = g_hub.logged_in ? active_account() : NULL;
    return acc && (acc->is_dev || acc->is_mod);
}

// genericPopup("<question>" + YES, cls): the confirm popup of v10's verify / flag / delete
static void confirm_popup(const char *question, const char *onyes, const char *cls) {
    StrBuf sb = {0};
    sb_append(&sb, "<div style='color:#fff;font-size:20px;position:relative;margin-bottom:15px;'>");
    sb_append_esc(&sb, question);
    sb_appendf(&sb, "</div><div id='confBtn' onmouseover=\"SOUND.play('tick_0',0.1)\" onclick='%s'>YES</div>", onyes);
    char *html = sb_finish(&sb);
    generic_popup(html, cls, NULL);
    free(html);
}

// ["ver" | "flg" | "sbdg" | "crt", ...] over the social socket, then the waiting popup (io_wait)
static void hub_staff_send(const char *label, const char *name, const int64_t value, const int on, const int with_value) {
    cJSON *f = cJSON_CreateArray();
    cJSON_AddItemToArray(f, cJSON_CreateString(label));
    cJSON_AddItemToArray(f, cJSON_CreateString(name));
    if (with_value) cJSON_AddItemToArray(f, cJSON_CreateNumber((double) value));
    cJSON_AddItemToArray(f, cJSON_CreateNumber(on));
    io_wait("Please Wait...");
    hub_send(f);
}

// verifyPopup(): "Are you sure you want to verify - NAME?" (dev only, the Verify button)
H(h_verify_popup) {
    UNUSED;
    if (!hub_is_dev() || !g_hub.profile_name[0]) return 1;
    char q[192];
    snprintf(q, sizeof(q), "Are you sure you want to %s - %s?", g_hub.profile_featured ? "unverify" : "verify", g_hub.profile_name);
    confirm_popup(q, "verifyUser()", "verifyPop");
    return 1;
}

// verifyUser(): io.send("ver", name, !featured) -> "verr"
H(h_verify_user) {
    UNUSED;
    if (!hub_is_dev() || !g_hub.profile_name[0]) return 1;
    hub_staff_send("ver", g_hub.profile_name, 0, g_hub.profile_featured ? 0 : 1, 0);
    return 1;
}

// creatorPopup() / creatorUser(): v10's creator badge (dev only, the Creator button) -> "crt"
// (name, !creator) -> "crtr"
H(h_creator_popup) {
    UNUSED;
    if (!hub_is_dev() || !g_hub.profile_name[0]) return 1;
    char q[192];
    snprintf(q, sizeof(q), "Are you sure you want to %s the creator badge - %s?", g_hub.profile_creator ? "remove" : "give", g_hub.profile_name);
    confirm_popup(q, "creatorUser()", "verifyPop"); // the Verify popup's box
    return 1;
}

H(h_creator_user) {
    UNUSED;
    if (!hub_is_dev() || !g_hub.profile_name[0]) return 1;
    hub_staff_send("crt", g_hub.profile_name, 0, g_hub.profile_creator ? 0 : 1, 0);
    return 1;
}

// flagPopup() / flagUser(): "... as a Hacker?" -> "flg" (name, !hack) -> "fgr"
H(h_flag_popup) {
    UNUSED;
    if (!hub_is_staff() || !g_hub.profile_name[0]) return 1;
    char q[192];
    snprintf(q, sizeof(q), "Are you sure you want to %s - %s as a Hacker?", g_hub.profile_hack ? "unflag" : "flag", g_hub.profile_name);
    confirm_popup(q, "flagUser()", "flagPop");
    return 1;
}

H(h_flag_user) {
    UNUSED;
    if (!hub_is_staff() || !g_hub.profile_name[0]) return 1;
    hub_staff_send("flg", g_hub.profile_name, 0, g_hub.profile_hack ? 0 : 1, 0);
    return 1;
}

// badgePopup(): the badge list (v7 names) with Give / Remove (dev only)
H(h_badge_popup) {
    UNUSED;
    if (!hub_is_dev() || !g_hub.profile_name[0]) return 1;
    StrBuf sb = {0};
    sb_append(&sb, "<div style='font-size:18px;color:#fff;margin-bottom:15px'>Badge:<select id='badgeList' class='inputGrey2' style='padding: 4px;'>");
    for (int i = 0; i < BADGE_COUNT; i++) {
        sb_appendf(&sb, "<option value='%d'>", i);
        sb_append_esc(&sb, g_badges[i].name);
        sb_append(&sb, "</option>");
    }
    sb_append(&sb, "</select></div><div id='confirmBtn' onclick='badgeUser(1)' onmouseover=\"SOUND.play('tick_0',0.1)\">Give</div>"
                   "<div id='declineBtn' onclick='badgeUser(0)' onmouseover=\"SOUND.play('tick_0',0.1)\">Remove</div>");
    char *html = sb_finish(&sb);
    generic_popup(html, "badgePop", NULL);
    free(html);
    return 1;
}

// badgeUser(on): io.send("sbdg", name, badge, on) -> "sbdgr"
H(h_badge_user) {
    UNUSED;
    const HtmlElem list = el("badgeList");
    if (!hub_is_dev() || !list || !g_hub.profile_name[0]) return 1;
    char value[16] = "";
    html_dom_get_value(list, value, sizeof(value));
    const int badge = atoi(value);
    if (badge < 0 || badge >= BADGE_COUNT) return 1;
    hub_staff_send("sbdg", g_hub.profile_name, badge, arg_n(args, argc, 0, 0) ? 1 : 0, 1);
    return 1;
}

// reportPopup(): the reason list (v10's reportOptions) and the details (10 characters at least)
static const char *const k_report_reasons[] = {"Hacking", "Exploiting", "Scamming", "Harassment", "Other", "Boosting", "Market Manipulation"};

// The profile's Reports tab for staff (hub-10.0.7.js:4176-4193): newest first, the age, the reporter (viewReporter opens
// their profile), the reason; a click on a report (openReport) shows or hides its text.
static cJSON *g_reports; // the last "rps" rows (viewReporter reads the reporter's name back)

static void profile_reports(const cJSON *rows) {
    cJSON_Delete(g_reports);
    g_reports = cJSON_Duplicate(rows, 1);
    StrBuf sb = {0};
    sb_append(&sb, "<div class='scrollItemNew'><div style='margin-top:10px;margin-bottom:10px;'>");
    const int n = cJSON_GetArraySize(g_reports);
    if (!n) sb_append(&sb, "<div style='color: rgba(255,255,255,0.6);'>None</div>");
    for (int i = 0; i < n; i++) {
        const cJSON *r = cJSON_GetArrayItem(g_reports, i);
        const int idx = (int) jnum(r, "rp_index");
        char ago[48] = "just now";
        const double ms = jnum(r, "rp_ms");
        if (ms > 0) time_ago(ago, sizeof(ago), now_epoch_ms() - ms);
        sb_appendf(&sb, "<div class='reportHolder' id='rpt_%d' onclick='openReport(this)'><div class='reportDate'>%s</div>"
                        "<div class='reportFrom'>Report from <span class='reportFromView' onclick='viewReporter(%d)'>",
                   (int) jnum(r, "rp_id"), ago, i);
        sb_append_esc(&sb, jstr(r, "rp_reportername") ? jstr(r, "rp_reportername") : "");
        sb_appendf(&sb, "</span></div><div class='reportType'>%s</div><div class='reportText'>",
                   k_report_reasons[idx >= 0 && idx < (int) (sizeof(k_report_reasons) / sizeof(k_report_reasons[0])) ? idx : 4]);
        sb_append(&sb, jstr(r, "rp_text") ? jstr(r, "rp_text") : ""); // the brain's kn_html: escaped already
        sb_append(&sb, "</div></div>");
    }
    sb_append(&sb, "</div></div><div></div>");
    char *html = sb_finish(&sb);
    set_html("pTabContent_reports", html);
    free(html);
    set_display("pTab_reports", "");
}

// openReport(this): the report's text (its fourth child) shown or hidden
H(h_open_report) {
    UNUSED;
    if (!ev || !ev->target) return 1;
    const HtmlElem holder = html_dom_closest(ev->target, ".reportHolder");
    const HtmlElem text = holder ? html_dom_query(holder, ".reportText") : 0;
    if (!text) return 1;
    char disp[16] = "";
    html_dom_get_style(text, "display", disp, sizeof(disp));
    html_dom_set_display(text, strcmp(disp, "block") ? "block" : "none");
    return 1;
}

// viewReporter(i): the reporter's profile
H(h_view_reporter) {
    UNUSED;
    const char *name = jstr(cJSON_GetArrayItem(g_reports, (int) arg_n(args, argc, 0, -1)), "rp_reportername");
    if (!name) return 1;
    char url[200];
    snprintf(url, sizeof(url), "/social.html?p=profile&q=%s", name);
    if (!app_route_url_target(url, 1)) ui_open_external(url);
    return 1;
}

H(h_report_popup) {
    UNUSED;
    if (!g_hub.profile_name[0]) return 1;
    if (!g_hub.logged_in) {
        io_po(0, I18N("account.login.required", "Must be logged in"));
        return 1;
    }
    StrBuf sb = {0};
    sb_append(&sb, "<div style='color:#fff;font-size:20px;position:relative;margin-bottom:15px;'>Report: ");
    sb_append_esc(&sb, g_hub.profile_name);
    sb_append(&sb, "</div><div style='color:#fff;font-size: 16px;position:relative;'>Reason:<select id='reportSelect' class='inputGrey2' style='padding: 4px;margin-left:8px'>");
    for (size_t i = 0; i < sizeof(k_report_reasons) / sizeof(k_report_reasons[0]); i++) {
        sb_appendf(&sb, "<option value='%zu'>%s</option>", i, k_report_reasons[i]);
    }
    sb_append(&sb, "</select></div><textarea type='text' rows='8' id='reportTxt' placeholder='Provide more information (links to video clip)'></textarea>"
                   "<div id='reportError' style='display:none;color:#ed4242'></div>"
                   "<div id='reportBtn' onmouseover=\"SOUND.play('tick_0',0.1)\" onclick='reportUser()'>Report</div>");
    char *html = sb_finish(&sb);
    generic_popup(html, "reportPop", NULL);
    free(html);
    return 1;
}

// reportUser(): io.send("rpt", name, reason, details) -> "rptr"
H(h_report_user) {
    UNUSED;
    const HtmlElem sel = el("reportSelect"), txt = el("reportTxt"), err = el("reportError");
    if (!sel || !txt || !g_hub.profile_name[0]) return 1;
    char value[16] = "", text[2048] = "";
    html_dom_get_value(sel, value, sizeof(value));
    html_dom_get_value(txt, text, sizeof(text));
    const int reason = atoi(value);
    if (reason < 0 || reason >= (int) (sizeof(k_report_reasons) / sizeof(k_report_reasons[0]))) return 1;
    if (strlen(text) < 10) {
        if (err) {
            html_dom_set_text(err, "More information required.");
            set_display("reportError", "block");
        }
        return 1;
    }
    cJSON *f = cJSON_CreateArray();
    cJSON_AddItemToArray(f, cJSON_CreateString("rpt"));
    cJSON_AddItemToArray(f, cJSON_CreateString(g_hub.profile_name));
    cJSON_AddItemToArray(f, cJSON_CreateString(k_report_reasons[reason]));
    cJSON_AddItemToArray(f, cJSON_CreateString(text));
    io_wait("Reporting User...");
    hub_send(f);
    return 1;
}

// shareGameTweet(name) (hub-10.0.7.js:1568-1571): a post on X / Twitter with the map's play link, in the browser
H(h_share_game_tweet) {
    UNUSED;
    const char *raw = arg_s(ev, args, argc, 0);
    if (!raw || !*raw) return 1;
    // name.replace("!", "%21"): the first one only
    char name[400];
    const char *bang = strchr(raw, '!');
    if (bang) snprintf(name, sizeof(name), "%.*s%%21%s", (int) (bang - raw), raw, bang + 1);
    else snprintf(name, sizeof(name), "%s", raw);
    char origin[128], enc_origin[400];
    html_page_origin(origin, sizeof(origin));
    backend_url_encode(enc_origin, sizeof(enc_origin), origin); // v10: https%3A%2F%2Fkrunker.io
    char url[1400];
    snprintf(url, sizeof(url), "https://twitter.com/intent/tweet?url=%s/?play=%s&text=Join%%20my%%20game%%20on%%20Krunker&hashtags=krunker%%2Cgaming",
             enc_origin, name);
    ui_open_external(url);
    return 1;
}

// chatBanUser(): the switch (profile_chat_ban_set answers)
H(h_chat_ban_user) {
    UNUSED;
    const AccountInfo *acc = active_account();
    if (!acc || !(acc->is_dev || acc->is_mod) || !g_hub.profile_name[0]) return 1;
    char url[1024];
    moderation_url(url, sizeof(url), "/moderation/chat");
    cJSON *b = cJSON_CreateObject();
    cJSON_AddStringToObject(b, "player_name", g_hub.profile_name);
    cJSON_AddBoolToObject(b, "ban", !g_hub.profile_chat_banned);
    char *json = cJSON_PrintUnformatted(b);
    cJSON_Delete(b);
    char *name = strdup(g_hub.profile_name);
    if (!json || !name || !backend_http_request("POST", url, json, profile_chat_ban_set, name)) free(name);
    free(json);
    return 1;
}

// shareProfile(): the profile link to the clipboard (v10 copies https://krunker.io/social.html?p=profile&q=NAME)
H(h_share_profile) {
    UNUSED;
    if (!g_hub.profile_name[0]) return 1;
    char origin[128], url[256];
    html_page_origin(origin, sizeof(origin));
    snprintf(url, sizeof(url), "%s/social.html?p=profile&q=%s", origin, g_hub.profile_name);
    html_clipboard_write(url);
    return 1;
}

// favoriteM(kind, this, id) (hub-10.0.7.js:6616-6622): signed in, the star's sel class flips and
// io.send("fvM", kind, id, 1 favourite | 0 not)
H(h_favorite_m) {
    UNUSED;
    if (!g_hub.logged_in || !ev || !ev->target) return 1;
    const int on = !html_dom_has_class(ev->target, "sel");
    html_dom_set_class(ev->target, "sel", on);
    cJSON *f = cJSON_CreateArray();
    cJSON_AddItemToArray(f, cJSON_CreateString("fvM"));
    cJSON_AddItemToArray(f, cJSON_CreateNumber(arg_n(args, argc, 0, 0)));
    cJSON_AddItemToArray(f, cJSON_CreateNumber(arg_n(args, argc, 2, 0)));
    cJSON_AddItemToArray(f, cJSON_CreateNumber(on));
    hub_send(f);
    return 1;
}

// featureCreation(this, id, kind): the star toggles and io.send("ftC", id, on, kind) (dev only)
H(h_feature_creation) {
    UNUSED;
    if (!hub_is_dev() || !ev || !ev->target) return 1;
    const int on = !html_dom_has_class(ev->target, "sel");
    html_dom_set_class(ev->target, "sel", on);
    cJSON *f = cJSON_CreateArray();
    cJSON_AddItemToArray(f, cJSON_CreateString("ftC"));
    cJSON_AddItemToArray(f, cJSON_CreateNumber(arg_n(args, argc, 1, 0)));
    cJSON_AddItemToArray(f, cJSON_CreateNumber(on));
    cJSON_AddItemToArray(f, cJSON_CreateNumber(arg_n(args, argc, 2, 0)));
    hub_send(f);
    return 1;
}

// deleteCreationConf(id, kind): "Are you sure you want to delete this Map?" then deleteCreation(id, kind)
H(h_delete_creation_conf) {
    UNUSED;
    if (!g_hub.logged_in) {
        io_po(0, I18N("account.login.required", "Must be logged in"));
        return 1;
    }
    const int id = (int) arg_n(args, argc, 0, 0); // deleteCreationConf(id, kind)
    const int kind = (int) arg_n(args, argc, 1, 0);
    char onyes[64];
    snprintf(onyes, sizeof(onyes), "deleteCreation(%d,%d)", id, kind);
    confirm_popup(kind == 0 ? "Are you sure you want to delete this Map?" : kind == 1 ? "Are you sure you want to delete this Mod?" : "Are you sure you want to delete this Asset?",
                  onyes, "confSubmitPop");
    return 1;
}

// deleteCreation(id, kind): io.send("delC", id, kind); the brain answers with its "po" popup
H(h_delete_creation) {
    UNUSED;
    if (!g_hub.logged_in) return 1;
    cJSON *f = cJSON_CreateArray();
    cJSON_AddItemToArray(f, cJSON_CreateString("delC"));
    cJSON_AddItemToArray(f, cJSON_CreateNumber(arg_n(args, argc, 0, 0)));
    cJSON_AddItemToArray(f, cJSON_CreateNumber(arg_n(args, argc, 1, 0)));
    io_wait("Please Wait...");
    hub_send(f);
    return 1;
}

// ---- the map page tools: the Leaderboard tab, the creator's Data tab and Edit popup
//
// Krunker's hub (static/social-CURLIuB7.js, cited hub-10.0.7.js:<line> after the repo's webcrack pass;
// doc/v10/parity-audit-2026-11-20-social.md) asks kn_brain (v10_catalog.c) over the social socket:
//   glb   the Leaderboard tab once the map page shows (:3168) -> glbR(type, map, [name, value, ...]) (:3804-3822)
//   sd    Search on the Data tab (searchPData, :3654-3660) -> sdR([{mk, kv}], player) (:3663-3681)
//   dd    a row's delete (deleteData, :3691-3696) -> ddR(key, player) (:3682-3690)
//   dkd   Delete By Key, the key of every player (deleteKeyData, :3649-3653) -> po
//   mfnds the Edit popup's Deposit (0) / Withdraw (1) (chngMapFunds, :6673-6679) -> mfr(funds | null) (:3796-3803)
//   ulb   its Leaderboard Display (updateMapLB, :6680-6688) -> upMp(text)
//   edMp  its Open in Editor (grabMap, :6689-6693) -> edMp(json | null): staged as mapToEdit (s2c_ui_edit_map)
// The markup and wording are v10.0.7's: the Edit popup (openEdit, :6653-6656) with its KR Fee line (calcMapFee,
// :6657-6671: 10% of the typed amount, shown only), the Leaderboard tab's board (:3804-3822), the Data tab's rows
// (:3663-3681), Delete By Key (deleteDataPop / deleteDataConf, :3627-3642) and the funds answer (mfr, :3796-3803).
static struct {
    char name[160];  // the map page's map
    int id;
    cJSON *rows;     // the Data tab's last search: [{mk, kv}]
    char player[64]; // ... and whose rows they are
    char key[128];   // the key Delete By Key asked about
} g_map_tools;

// v10.0.7 Creations' Versions tab (the map details window's, deob-10.0.7.js:107649-107654, :107794-107835,
// :108190-108280), on the hub's map page: its markup (md-data / md-ver-*, svelte-1kkpw01) and creations.versions.*
// texts. getMapVersions is GET /maps/<id>/versions, revertMapVersion POST /maps/<id>/versions/<v>/revert
// (deob-10.0.7.js:26713-26723). Deviations: v10's confirm dialog (title "Revert Map", button "Revert") is the hub's
// confirm popup with the same description; a revert does not reload the rest of the map page (v10's iÏïììiï).
// Each request carries its map id and a generation: switching pages cannot install a late response.
static struct {
    unsigned generation; // the list request in flight's
    int map_id;          // the map the state below is for
    cJSON *rows;         // the last list: [{version, created, data_size, current_version}], newest first
    int loaded, loading; // the list was fetched once for this map (v10 loads it on the tab's first open) / in flight
    int reverting;       // the version a revert is in flight for, 0 none
    char error[256], success[160];
} g_mv;

typedef struct {
    int map_id;
    unsigned generation;
} MapVersionsRequest;

static int map_versions_url(char *url, size_t n, const char *path) {
    const char *token = account_session_token();
    if (!backend_enabled() || !token || !*token) return 0;
    char encoded[512];
    backend_url_encode(encoded, sizeof encoded, token);
    snprintf(url, n, "%s%s?token=%s", backend_url(), path, encoded);
    return 1;
}

// A reply's error.message || error (as a string), else NULL.
static const char *map_versions_error(const cJSON *root) {
    const cJSON *e = cJSON_GetObjectItemCaseSensitive(root, "error");
    if (cJSON_IsString(e) && e->valuestring[0]) return e->valuestring;
    const char *message = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(e, "message"));
    return message && *message ? message : NULL;
}

static void hub_map_versions_render(void) {
    if (!el("mapVersions")) return;
    StrBuf sb = {0};
    sb_append(&sb, "<div class='md-data svelte-1kkpw01'><div class='md-ver-intro svelte-1kkpw01'>");
    sb_append_esc(&sb, i18n_tr("creations.versions.intro", "Every publish stores a snapshot of your game. Revert to roll the live game back to an older snapshot — the current one is kept, so a revert can be undone."));
    sb_append(&sb, "</div>");
    if (g_mv.error[0]) {
        sb_append(&sb, " <div class='md-data-error svelte-1kkpw01'>");
        sb_append_esc(&sb, g_mv.error);
        sb_append(&sb, "</div>");
    }
    if (g_mv.success[0]) {
        sb_append(&sb, " <div class='md-data-success svelte-1kkpw01'>");
        sb_append_esc(&sb, g_mv.success);
        sb_append(&sb, "</div>");
    }
    const int count = cJSON_GetArraySize(g_mv.rows);
    if (g_mv.loading) {
        sb_append(&sb, " <div class='md-center-sm svelte-1kkpw01'><i class='material-icons spin svelte-1kkpw01'>hourglass_empty</i></div>");
    } else if (!count) {
        sb_append(&sb, " <div class='md-empty svelte-1kkpw01'>");
        sb_append_esc(&sb, i18n_tr("creations.versions.empty", "No saved versions yet."));
        sb_append(&sb, "</div>");
    } else {
        // the live version: versions[0].current_version (deob-10.0.7.js:107654)
        const int live = (int) jnum(cJSON_GetArrayItem(g_mv.rows, 0), "current_version");
        sb_append(&sb, " <div class='md-data-table svelte-1kkpw01'><div class='md-ver-head svelte-1kkpw01'><span>");
        sb_append_esc(&sb, i18n_tr("creations.versions.colVersion", "Version"));
        sb_append(&sb, "</span> <span>");
        sb_append_esc(&sb, i18n_tr("creations.versions.colDate", "Saved"));
        sb_append(&sb, "</span> <span>");
        sb_append_esc(&sb, i18n_tr("creations.versions.colSize", "Size"));
        sb_append(&sb, "</span> <span></span></div> ");
        const cJSON *row;
        cJSON_ArrayForEach(row, g_mv.rows) {
            const int version = (int) jnum(row, "version");
            char date[64], kb[48], size[64];
            get_date(date, sizeof date, jstr(row, "created"));
            // "{0} KB" of Math.max(1, Math.round(data_size / 1024)).toLocaleString() (deob-10.0.7.js:108257)
            const double k = round(jnum(row, "data_size") / 1024.0);
            comma_number(kb, sizeof kb, k > 1 ? k : 1);
            i18n_trf(size, sizeof size, "creations.versions.size", "{0} KB", 1, I18N_ARGV(kb));
            sb_appendf(&sb, "<div class='md-ver-row svelte-1kkpw01'><span class='md-ver-num svelte-1kkpw01'>v%d ", version);
            if (version == live) {
                sb_append(&sb, "<span class='md-ver-badge svelte-1kkpw01'>");
                sb_append_esc(&sb, i18n_tr("creations.versions.current", "Current"));
                sb_append(&sb, "</span>");
            }
            sb_append(&sb, "</span> <span class='md-ver-date svelte-1kkpw01'>");
            sb_append_esc(&sb, date);
            sb_append(&sb, "</span> <span class='md-ver-size svelte-1kkpw01'>");
            sb_append_esc(&sb, size);
            sb_append(&sb, "</span> ");
            if (version == live) {
                sb_append(&sb, "<span></span>");
            } else {
                sb_appendf(&sb, "<button class='md-ver-btn svelte-1kkpw01' type='button' onclick='revertMapVersion(%d,%d)'%s><i class='material-icons svelte-1kkpw01'>history</i> ",
                           g_mv.map_id, version, g_mv.reverting ? " disabled" : "");
                sb_append_esc(&sb, g_mv.reverting == version ? i18n_tr("creations.versions.reverting", "Reverting...")
                                                             : i18n_tr("creations.versions.revert", "Revert"));
                sb_append(&sb, "</button>");
            }
            sb_append(&sb, "</div>");
        }
        sb_append(&sb, "</div>");
    }
    sb_append(&sb, "</div>");
    char *html = sb_finish(&sb);
    set_html("mapVersions", html);
    free(html);
}

static void map_versions_done(void *user, int status, const char *body, size_t len) {
    MapVersionsRequest *req = user;
    if (!req) return;
    const int current = req->map_id == g_mv.map_id && req->generation == g_mv.generation;
    free(req);
    if (!current) return;
    g_mv.loading = 0;
    cJSON *root = body ? cJSON_ParseWithLength(body, len) : NULL;
    cJSON *rows = cJSON_GetObjectItemCaseSensitive(root, "data");
    const char *error = map_versions_error(root);
    cJSON_Delete(g_mv.rows);
    g_mv.rows = NULL;
    if (status == 200 && !error) { // versions = data || []
        g_mv.rows = cJSON_IsArray(rows) ? cJSON_DetachItemViaPointer(root, rows) : NULL;
        g_mv.loaded = 1;
    } else {
        snprintf(g_mv.error, sizeof g_mv.error, "%s", error ? error : i18n_tr("creations.versions.error", "Failed to load versions."));
    }
    cJSON_Delete(root);
    hub_map_versions_render();
}

// loadVersions (deob-10.0.7.js:107794-107811)
static void hub_map_versions_load(void) {
    if (g_mv.map_id <= 0 || !el("mapVersions")) return;
    ++g_mv.generation;
    g_mv.loading = 1;
    g_mv.error[0] = 0;
    hub_map_versions_render();
    MapVersionsRequest *req = malloc(sizeof *req);
    char path[128], url[1024];
    snprintf(path, sizeof path, "/maps/%d/versions", g_mv.map_id);
    if (req && map_versions_url(url, sizeof url, path)) {
        *req = (MapVersionsRequest) {g_mv.map_id, g_mv.generation};
        if (backend_http_request("GET", url, NULL, map_versions_done, req)) return;
    }
    free(req);
    g_mv.loading = 0;
    snprintf(g_mv.error, sizeof g_mv.error, "%s", i18n_tr("creations.versions.error", "Failed to load versions."));
    hub_map_versions_render();
}

// The tab opened: "versions" !== tab || loaded || loadVersions() (deob-10.0.7.js:107848), else the current state.
static void hub_map_versions_show(void) {
    if (!g_mv.loaded && !g_mv.loading) hub_map_versions_load();
    else hub_map_versions_render();
}

// A row's Revert: v10's confirm (deob-10.0.7.js:107813-107818), then the revert
H(h_revert_map_version) {
    UNUSED;
    const int map = (int) arg_n(args, argc, 0, 0), version = (int) arg_n(args, argc, 1, 0);
    if (map <= 0 || version <= 0 || map != g_mv.map_id || g_mv.reverting) return 1;
    char callback[96], question[320], v[16];
    snprintf(callback, sizeof callback, "revertMapVersionGo(%d,%d)", map, version);
    snprintf(v, sizeof v, "%d", version);
    i18n_trf(question, sizeof question, "creations.versions.confirmDesc",
             "Revert this map to version {0}? The version currently live is kept as a snapshot, so you can undo this.", 1, I18N_ARGV(v));
    confirm_popup(question, callback, "confSubmitPop");
    return 1;
}

static void map_revert_done(void *user, int status, const char *body, size_t len) {
    MapVersionsRequest *req = user;
    if (!req) return;
    const int current = req->map_id == g_mv.map_id;
    free(req);
    if (!current) return;
    g_mv.reverting = 0;
    cJSON *root = body ? cJSON_ParseWithLength(body, len) : NULL;
    const char *error = map_versions_error(root);
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (status == 200 && !error) {
        // "Reverted. Live version is now v{0}." with data.version ?? "" (deob-10.0.7.js:107827), then loadVersions()
        char v[24] = "";
        const cJSON *version = cJSON_GetObjectItemCaseSensitive(data, "version");
        if (cJSON_IsNumber(version)) js_num(v, sizeof v, version->valuedouble);
        i18n_trf(g_mv.success, sizeof g_mv.success, "creations.versions.reverted", "Reverted. Live version is now v{0}.", 1, I18N_ARGV(v));
        cJSON_Delete(root);
        hub_map_versions_load();
        return;
    }
    snprintf(g_mv.error, sizeof g_mv.error, "%s", error ? error : i18n_tr("creations.versions.revertError", "Failed to revert this version."));
    cJSON_Delete(root);
    hub_map_versions_render();
}

H(h_revert_map_version_go) {
    UNUSED;
    clear_pops();
    const int map = (int) arg_n(args, argc, 0, 0), version = (int) arg_n(args, argc, 1, 0);
    if (map <= 0 || version <= 0 || map != g_mv.map_id || g_mv.reverting) return 1;
    char path[160], url[1024];
    snprintf(path, sizeof path, "/maps/%d/versions/%d/revert", map, version);
    g_mv.reverting = version;
    g_mv.error[0] = 0;
    g_mv.success[0] = 0;
    MapVersionsRequest *req = malloc(sizeof *req);
    if (req && map_versions_url(url, sizeof url, path)) {
        *req = (MapVersionsRequest) {map, 0};
        if (backend_http_request("POST", url, NULL, map_revert_done, req)) {
            hub_map_versions_render();
            return 1;
        }
    }
    free(req);
    g_mv.reverting = 0;
    snprintf(g_mv.error, sizeof g_mv.error, "%s", i18n_tr("creations.versions.revertError", "Failed to revert this version."));
    hub_map_versions_render();
    return 1;
}

// ---- the game list (fetchGameList, hub-10.0.7.js:5988-5994): the coordinator's /game-list, its custom games read by
// the maps list's live players (updatePlayers, :6107-6131) and the map page's Servers tab (iïíiîìi, :5999-6029)
static cJSON *g_player_data;   // windows[0].playerData: {map: players}, filled once (Object.keys(...).length)
static int g_player_data_busy; // a fetch for it is on its way

static const cJSON *game_meta(const cJSON *t) { return cJSON_GetArrayItem(t, 4); }
static int game_flag(const cJSON *t, const char *k) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(game_meta(t), k);
    return v && !cJSON_IsFalse(v) && !cJSON_IsNull(v) && !(cJSON_IsNumber(v) && v->valuedouble == 0);
}
static int game_custom(const cJSON *t) { return game_flag(t, "c") || game_flag(t, "cs"); } // !!info && !!info.c
// info.i: the game's map (a Box Office custom game names its map in meta.map)
static const char *game_map_name(const cJSON *t) {
    const char *m = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(game_meta(t), "map"));
    if (game_custom(t) && m && *m) return m;
    m = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(game_meta(t), "i"));
    return m ? m : "";
}
static double game_num(const cJSON *t, const int i) {
    const cJSON *v = cJSON_GetArrayItem(t, i);
    return cJSON_IsNumber(v) ? v->valuedouble : 0;
}

static int hub_game_list(const BackendHttpFn fn, void *user) {
    char path[160];
    snprintf(path, sizeof(path), "/mm/game-list?hostname=krunknative%s%s", flavor_get() == FLAVOR_BASE ? "" : "&flavor=",
             flavor_get() == FLAVOR_BASE ? "" : flavor_current()->key);
    return backend_http_get(path, fn, user);
}

static double hub_player_data(const char *name) {
    const cJSON *v = name ? cJSON_GetObjectItemCaseSensitive(g_player_data, name) : NULL;
    return cJSON_IsNumber(v) ? v->valuedouble : 0;
}

static void pcnt_set(const char *map, const double n) {
    char id[300], num[32], html[300];
    snprintf(id, sizeof(id), "pcnt_%s", map);
    if (!el(id)) return;
    js_num(num, sizeof(num), n);
    snprintf(html, sizeof(html), "<span class='material-icons' style='margin-right: 8px;color:rgba(255, 255, 255, 0.5);vertical-align: middle;margin-top:-5px;font-size:30px;'>person</span>%s", num);
    set_html(id, html);
}

static void on_player_data(void *user, const int status, const char *body, const size_t length) {
    (void) user;
    (void) length;
    g_player_data_busy = 0;
    cJSON *res = status == 200 && body ? cJSON_Parse(body) : NULL;
    const cJSON *t;
    if (!g_player_data) g_player_data = cJSON_CreateObject();
    cJSON_ArrayForEach(t, cJSON_GetObjectItemCaseSensitive(res, "games")) {
        if (!game_custom(t)) continue;
        const char *map = game_map_name(t);
        const double p = game_num(t, 2), max = game_num(t, 3), add = p < max ? p : max;
        cJSON *have = cJSON_GetObjectItemCaseSensitive(g_player_data, map);
        if (have) cJSON_SetNumberValue(have, have->valuedouble + add);
        else cJSON_AddNumberToObject(g_player_data, map, add);
        pcnt_set(map, hub_player_data(map));
    }
    cJSON_Delete(res);
}

// updatePlayers(): the first time (no playerData yet) the list is fetched and every card's count added to; after that
// the cards take playerData (gen already drew it)
static void hub_players_update(void) {
    if (cJSON_GetArraySize(g_player_data)) {
        const cJSON *v;
        cJSON_ArrayForEach(v, g_player_data) pcnt_set(v->string, v->valuedouble);
        return;
    }
    if (g_player_data_busy) return;
    g_player_data_busy = 1;
    if (!hub_game_list(on_player_data, NULL)) g_player_data_busy = 0;
}

// iïíiîìi(games): the open map's custom games, starred (pr) first then by players, in #activeGames; the players summed
// in #totalOnline
static char g_active_map[128];
static int cmp_active_games(const void *a, const void *b) {
    const cJSON *x = *(const cJSON *const *) a, *y = *(const cJSON *const *) b;
    const int d = game_flag(y, "pr") - game_flag(x, "pr");
    if (d) return d;
    const double px = game_num(x, 2) < game_num(x, 3) ? game_num(x, 2) : game_num(x, 3);
    const double py = game_num(y, 2) < game_num(y, 3) ? game_num(y, 2) : game_num(y, 3);
    return py > px ? 1 : py < px ? -1 : 0;
}

static void on_active_games(void *user, const int status, const char *body, const size_t length) {
    (void) length;
    char *map = user;
    cJSON *res = status == 200 && body ? cJSON_Parse(body) : NULL;
    if (!map || strcmp(map, g_active_map) || !el("activeGames")) { // another map's page by now
        cJSON_Delete(res);
        free(map);
        return;
    }
    const cJSON *t, *list = cJSON_GetObjectItemCaseSensitive(res, "games");
    const int n = cJSON_GetArraySize(list);
    const cJSON **games = n > 0 ? calloc((size_t) n, sizeof(*games)) : NULL;
    int count = 0;
    double players = 0;
    cJSON_ArrayForEach(t, list) {
        if (!games || !game_custom(t) || strcmp(game_map_name(t), map)) continue;
        players += game_num(t, 2);
        games[count++] = t;
    }
    if (count > 1) qsort(games, (size_t) count, sizeof(*games), cmp_active_games);
    StrBuf sb = {0};
    sb_append(&sb, "<div class='menuSelectorHolder'>");
    for (int i = 0; i < count; i++) {
        const char *id = cJSON_GetStringValue(cJSON_GetArrayItem(games[i], 0));
        const double p = game_num(games[i], 2), max = game_num(games[i], 3);
        char pc[32], mc[32];
        js_num(pc, sizeof(pc), p < max ? p : max);
        js_num(mc, sizeof(mc), max);
        sb_append(&sb, "<div class='menuSelector' onclick='openLink(&quot;/?game=");
        sb_append_esc(&sb, id ? id : "");
        sb_append(&sb, "&quot;, &quot;_blank&quot;)'>");
        sb_append_esc(&sb, map);
        if (game_flag(games[i], "pr")) sb_append(&sb, " <span class='material-icons' style='color:#FBC02D;vertical-align:middle'>star</span> ");
        sb_appendf(&sb, "<div class='serverPCount'>%s/%s</div><div class='serverRegion'>", pc, mc);
        const char *colon = id ? strchr(id, ':') : NULL;
        char region[96];
        snprintf(region, sizeof(region), "%.*s", id ? (int) (colon ? (size_t) (colon - id) : strlen(id)) : 0, id ? id : ""); // id.split(":")[0]
        sb_append_esc(&sb, region);
        sb_append(&sb, " -&nbsp;</div></div>");
    }
    if (!count) sb_append(&sb, "<div style='position: absolute;top: 50%;left: 50%;transform: translate(-50%, -50%);color: rgba(255,255,255,0.4);font-size:20px;'>No active servers</div>");
    sb_append(&sb, "</div>");
    char *html = sb_finish(&sb);
    set_html("activeGames", html);
    free(html);
    char total[32];
    comma_number(total, sizeof(total), players);
    set_html("totalOnline", total);
    free(games);
    cJSON_Delete(res);
    free(map);
}

static void hub_active_games_fetch(const char *name) {
    snprintf(g_active_map, sizeof(g_active_map), "%s", name ? name : "");
    char *map = strdup(g_active_map);
    if (map && !hub_game_list(on_active_games, map)) on_active_games(map, 0, NULL, 0); // no list: "No active servers"
}

static void hub_map_opened(const char *name, const int id) {
    char copy[sizeof(g_map_tools.name)]; // name may be g_map_tools.name itself (the refresh after Update Leaderboard)
    snprintf(copy, sizeof(copy), "%s", name ? name : "");
    memcpy(g_map_tools.name, copy, sizeof(copy));
    g_map_tools.id = id;
    if (g_mv.map_id != id) { // another map: its Versions state starts over, a reply in flight is dropped
        ++g_mv.generation;
        cJSON_Delete(g_mv.rows);
        g_mv.rows = NULL;
        g_mv.map_id = id;
        g_mv.loaded = g_mv.loading = g_mv.reverting = 0;
        g_mv.error[0] = g_mv.success[0] = 0;
    }
    cJSON_Delete(g_map_tools.rows);
    g_map_tools.rows = NULL;
    g_map_tools.player[0] = 0;
    if (!g_map_tools.name[0]) return;
    cJSON *f = cJSON_CreateArray();
    cJSON_AddItemToArray(f, cJSON_CreateString("glb"));
    cJSON_AddItemToArray(f, cJSON_CreateString(g_map_tools.name));
    cJSON_AddItemToArray(f, cJSON_CreateNumber(0)); // the brain reads the map's own Leaderboard Display
    cJSON_AddItemToArray(f, cJSON_CreateNumber(0));
    cJSON_AddItemToArray(f, cJSON_CreateString(""));
    hub_send(f);
    hub_active_games_fetch(g_map_tools.name); // fetchGameList(map_name, iïíiîìi) (hub-10.0.7.js:3169)
    if (g_map_tab == 4) hub_map_versions_show();
}

// UTILS.getTime(ms, true, ms >= 3600000, 1000, ms >= 86400000, round) (config-BDN_kuhS.js iîíiïií): "mm:ss.<fraction
// rounded to `round` places, without its "0.">", hours (and days) once the time reaches them
static void js_get_time(const double t, const int round_to, char *out, const size_t n) {
    const int show_hour = t >= 3600000.0, show_day = t >= 86400000.0;
    double frac = fmod(t, 1000.0) / 1000.0;
    if (frac != 0.0 && round_to) frac = round(frac * pow(10, round_to)) / pow(10, round_to); // Number.round(round_to)
    char ms[24] = "0";
    if (frac != 0.0) {
        char num[32];
        js_num(num, sizeof(num), frac);
        snprintf(ms, sizeof(ms), "%s", !strncmp(num, "0.", 2) ? num + 2 : num); // replace("0.", "")
    }
    const int seconds = (int) fmod(t / 1000.0, 60.0), minutes = (int) fmod(t / 60000.0, 60.0);
    const int hours = (int) floor(fmod(t / 3600000.0, 24.0));
    const long long days = (long long) floor(t / 86400000.0);
    char day[32] = "", hour[16] = "";
    if (show_day) snprintf(day, sizeof(day), "%s%lld:", days < 10 ? "0" : "", days);
    if (show_hour) snprintf(hour, sizeof(hour), "%s%d:", show_day && hours < 10 ? "0" : "", hours); // days is "0d": truthy
    snprintf(out, n, "%s%s%02d:%02d.%s", day, hour, minutes, seconds, ms);
}

// a stored value as v10 writes it: "time" (any case) through getTime, anything else as JS prints it
static void map_tools_value(StrBuf *sb, const char *label, const cJSON *v, const int round_to) {
    char num[48];
    const double ms = cJSON_IsNumber(v) ? v->valuedouble : cJSON_IsString(v) ? atof(v->valuestring) : 0;
    if (label && !strcasecmp(label, "time")) {
        js_get_time(ms, round_to, num, sizeof(num));
        sb_append(sb, num);
    } else if (cJSON_IsString(v)) {
        sb_append_esc(sb, v->valuestring);
    } else if (cJSON_IsNumber(v)) {
        js_num(num, sizeof(num), ms);
        sb_append(sb, num);
    } else {
        sb_append(sb, cJSON_IsTrue(v) ? "true" : cJSON_IsFalse(v) ? "false" : cJSON_IsNull(v) ? "null" : "undefined");
    }
}

// glbR(type, map, [name, value, ...]) (hub-10.0.7.js:3804-3822): #mapLeaders
static void map_tools_board(const char *type, const cJSON *list) {
    if (!el("mapLeaders")) return;
    StrBuf sb = {0};
    sb_append(&sb, "<div style='font-size:22px;margin-bottom: 2px;color:rgba(255,255,255,0.7)'>Player<span style='float: right;color:rgba(255,255,255,0.7)'>");
    if (type && type[0]) { // capitalize(type): the first character upper-cased
        const char first[2] = {(char) toupper((unsigned char) type[0]), 0};
        sb_append_esc(&sb, first);
        sb_append_esc(&sb, type + 1);
    }
    sb_append(&sb, "</span></div><hr style='border-color: #fff;'>");
    const int n = cJSON_GetArraySize(list);
    for (int i = 0; i < n; i += 2) {
        sb_appendf(&sb, "<div style='font-size:16px;margin-bottom: 2px;color:rgba(255,255,255,0.5)'>%d. ", i / 2 + 1);
        const cJSON *who = cJSON_GetArrayItem(list, i);
        if (cJSON_IsString(who)) sb_append_esc(&sb, who->valuestring);
        sb_append(&sb, "<span style='float:right;color:rgba(255,255,255,0.4);'>");
        map_tools_value(&sb, type, cJSON_GetArrayItem(list, i + 1), 1);
        sb_append(&sb, "</span></div>");
    }
    if (!n) sb_append(&sb, "<div style='font-size:16px;margin-bottom: 2px;text-align:center'>No Records Yet!</div>");
    char *html = sb_finish(&sb);
    set_html("mapLeaders", html);
    free(html);
}

// sdR([{mk, kv}], player) (:3663-3681): #dataList
static void map_tools_rows(void) {
    if (!el("dataList")) return;
    StrBuf sb = {0};
    sb_append(&sb, "<div style='font-size:20px;margin-bottom:2px;color:rgba(255,255,255,0.7);'>Key<span style='float:right;color:rgba(255,255,255,0.7);'>Value</span></div>"
                   "<hr style='border-color: #fff;'>");
    const cJSON *row;
    int n = 0;
    cJSON_ArrayForEach(row, g_map_tools.rows) {
        const char *key = jstr(row, "mk") ? jstr(row, "mk") : "";
        sb_append(&sb, "<div style='font-size:16px;margin-bottom: 2px;'><span class='material-icons deleteDBtn' onclick='deleteData(&quot;");
        sb_append_js_escape(&sb, g_map_tools.player);
        sb_append(&sb, "&quot;, &quot;");
        sb_append_js_escape(&sb, key);
        sb_append(&sb, "&quot;)'>delete_forever</span> <span style='color:rgba(255,255,255,0.5);'>");
        sb_append_esc(&sb, key);
        sb_append(&sb, "</span><span style='float: right;color:rgba(255,255,255,0.4)'>");
        map_tools_value(&sb, key, cJSON_GetObjectItemCaseSensitive(row, "kv"), 2);
        sb_append(&sb, "</span></div>");
        n++;
    }
    if (!n) sb_append(&sb, "<div style='font-size:16px;margin-bottom: 2px;text-align:center;color:rgba(255,255,255,0.7)'>No Records Yet!</div>");
    char *html = sb_finish(&sb);
    set_html("dataList", html);
    free(html);
}

// the map page's replies: 1 when the label is one of them
static int hub_map_tools_frame(const char *label, const cJSON *frame) {
    const cJSON *a1 = cJSON_GetArrayItem(frame, 1), *a2 = cJSON_GetArrayItem(frame, 2), *a3 = cJSON_GetArrayItem(frame, 3);
    if (!strcmp(label, "glbR")) {
        const char *map = cJSON_GetStringValue(a2);
        if (map && !strcasecmp(map, g_map_tools.name)) map_tools_board(cJSON_GetStringValue(a1), a3);
    } else if (!strcmp(label, "sdR")) {
        clear_pops();
        cJSON_Delete(g_map_tools.rows);
        g_map_tools.rows = cJSON_IsArray(a1) ? cJSON_Duplicate(a1, 1) : cJSON_CreateArray();
        snprintf(g_map_tools.player, sizeof(g_map_tools.player), "%s", cJSON_GetStringValue(a2) ? cJSON_GetStringValue(a2) : "");
        map_tools_rows();
    } else if (!strcmp(label, "ddR")) {
        clear_pops();
        const char *key = cJSON_GetStringValue(a1);
        const int n = cJSON_GetArraySize(g_map_tools.rows);
        for (int i = 0; key && i < n; i++) {
            if (!strcmp(jstr(cJSON_GetArrayItem(g_map_tools.rows, i), "mk") ? jstr(cJSON_GetArrayItem(g_map_tools.rows, i), "mk") : "", key)) {
                cJSON_DeleteItemFromArray(g_map_tools.rows, i);
                break;
            }
        }
        map_tools_rows();
    } else if (!strcmp(label, "plm")) { // updateMod's answer: publishModPopup(false, mod)
        clear_pops();
        hub_mod_popup(cJSON_IsObject(a2) ? a2 : NULL);
    } else if (!strcmp(label, "mfr")) {
        // :3796-3803: the funds and SUCCESS ("Success!"), else the error popup's default text
        if (a1 && !cJSON_IsNull(a1)) hub_set_funds(cJSON_GetNumberValue(a1));
        io_po(a1 && !cJSON_IsNull(a1), NULL);
    } else if (!strcmp(label, "upMp")) {
        const char *text = cJSON_GetStringValue(a1);
        io_po(text && !strcmp(text, "Leaderboard Updated!"), text ? text : "Error");
        if (text && !strcmp(text, "Leaderboard Updated!")) hub_map_opened(g_map_tools.name, g_map_tools.id);
    } else if (!strcmp(label, "edMp")) {
        const char *json = cJSON_GetStringValue(a1);
        if (json) {
            clear_pops();
            s2c_ui_edit_map(json, strlen(json));
        } else {
            io_po(0, "That game could not be opened in the editor");
        }
    } else {
        return 0;
    }
    return 1;
}

static void map_tools_send(cJSON *f, const char *wait) {
    if (wait) io_wait(wait);
    hub_send(f);
}

// openEdit(name, id, funds): the creator's Edit popup
H(h_open_edit) {
    UNUSED;
    const char *name = arg_s(ev, args, argc, 0);
    const int id = (int) arg_n(args, argc, 1, 0);
    char funds[48];
    comma_number(funds, sizeof(funds), arg_n(args, argc, 2, 0));
    // hub-10.0.7.js:6653-6656, word for word (the name escaped, as KN escapes every name it writes)
    StrBuf sb = {0};
    sb_append(&sb, "<div style='font-size:20px;color:#fff'>");
    sb_append_esc(&sb, name ? name : "");
    sb_append(&sb, "<div style='float:right' class='editLink' onclick='grabMap(&quot;");
    sb_append_js_escape(&sb, name ? name : "");
    sb_append(&sb, "&quot;)'>Open in Editor</div></div><div></div><div style='margin-top:25px;font-size:24px;color:#b2b2b2'>Leaderboard Display:</div>"
                   "<div style='font-size:18px;color:#fff'>Type:<select id='leaderType' class='inputGrey2' style='padding: 4px;'><option value='0'>Score</option><option value='1'>Time</option></select></div>"
                   "<div style='margin-top: 5px;font-size:18px;color:#fff'>Custom Type:<input id='leaderTypeCust' class='inputGrey2' style='padding: 4px;'></div>"
                   "<div style='margin-top:5px;font-size:18px;color:#fff'>Order:<select id='leaderOrder' class='inputGrey2' style='padding: 4px;'><option value='0'>ASC</option><option value='1'>DESC</option></select></div>");
    sb_appendf(&sb, "<div class='mapLeaderB' onclick='updateMapLB(%d)' onmouseover='SOUND.play(&quot;tick_0&quot;,0.1)'>Update Leaderboard</div>"
                    "<div style='margin-top:25px;font-size:24px;color:#b2b2b2'>Map Funds:</div>"
                    "<div style='font-size:18px;color:#fff'>Funds <span style='float:right;color:rgba(255,255,255,0.5)'>%s <span style='color:#fff'>KR</span></span></div>"
                    "<div style='font-size:18px;color:#fff'>KR Fee <span id='mapFeeVal' style='float:right;color:rgba(255,255,255,0.5)'>None</span></div>"
                    "<input type='number' oninput='calcMapFee(this)' id='mapFundsInp' placeholder='Enter KR Amount'></input>"
                    "<div class='mapFndB' onclick='chngMapFunds(%d, 1)' onmouseover='SOUND.play(&quot;tick_0&quot;,0.1)' style='background-color:#fc5a4e'>Withdraw</div>"
                    "<div class='mapFndB' onclick='chngMapFunds(%d, 0)' onmouseover='SOUND.play(&quot;tick_0&quot;,0.1)' style='margin-left:16px;background-color:#4ca9f5'>Deposit</div>",
               id, funds, id, id);
    char *html = sb_finish(&sb);
    generic_popup(html, "editMapPop", NULL);
    free(html);
    return 1;
}

// grabMap(name): the map's data for the editor
H(h_grab_map) {
    UNUSED;
    if (!g_hub.logged_in) return 1;
    cJSON *f = cJSON_CreateArray();
    cJSON_AddItemToArray(f, cJSON_CreateString("edMp"));
    cJSON_AddItemToArray(f, cJSON_CreateString(arg_s(ev, args, argc, 0) ? arg_s(ev, args, argc, 0) : ""));
    map_tools_send(f, "Please Wait...");
    return 1;
}

static double map_tools_input_number(const char *id) {
    char text[64] = "";
    if (el(id)) html_dom_get_value(el(id), text, sizeof(text));
    return atof(text);
}

// calcMapFee(input) (hub-10.0.7.js:6657-6671): a negative amount reads 0; #mapFeeVal shows floor(10%) or None
H(h_calc_map_fee) {
    (void) user; (void) args; (void) argc;
    const HtmlElem in = ev ? ev->target : 0;
    if (!in) return 1;
    char text[64] = "";
    html_dom_get_value(in, text, sizeof(text));
    double v = atof(text);
    if (v < 0) v = 0; // only the fee reads 0: v10 writes the value back only when it is not 0
    const double fee = floor(v * 0.1);
    if (fee) {
        char n[48], html[160];
        comma_number(n, sizeof(n), fee);
        snprintf(html, sizeof(html), "<span style='color:rgba(255,255,255,0.7)'>-%s <span style='color:#fff'>KR</span></span>", n);
        set_html("mapFeeVal", html);
    } else {
        set_html("mapFeeVal", "None");
    }
    return 1;
}

// chngMapFunds(id, 0 deposit | 1 withdraw) (:6673-6679): parseInt of #mapFundsInp, sent when it is not 0
H(h_chng_map_funds) {
    UNUSED;
    const double amount = trunc(map_tools_input_number("mapFundsInp"));
    if (!amount || isnan(amount)) return 1;
    cJSON *f = cJSON_CreateArray();
    cJSON_AddItemToArray(f, cJSON_CreateString("mfnds"));
    cJSON_AddItemToArray(f, cJSON_CreateNumber(arg_n(args, argc, 0, 0)));
    cJSON_AddItemToArray(f, cJSON_CreateNumber(arg_n(args, argc, 1, 0) == 1 ? 1 : 0));
    cJSON_AddItemToArray(f, cJSON_CreateNumber(amount));
    map_tools_send(f, "Please Wait...");
    return 1;
}

// updateMapLB(id): #leaderType, #leaderTypeCust, #leaderOrder
H(h_update_map_lb) {
    UNUSED;
    char custom[64] = "";
    if (el("leaderTypeCust")) html_dom_get_value(el("leaderTypeCust"), custom, sizeof(custom));
    cJSON *f = cJSON_CreateArray();
    cJSON_AddItemToArray(f, cJSON_CreateString("ulb"));
    cJSON_AddItemToArray(f, cJSON_CreateNumber(arg_n(args, argc, 0, 0)));
    cJSON_AddItemToArray(f, cJSON_CreateNumber(map_tools_input_number("leaderType") == 1 ? 1 : 0));
    cJSON_AddItemToArray(f, cJSON_CreateString(custom));
    cJSON_AddItemToArray(f, cJSON_CreateNumber(map_tools_input_number("leaderOrder") == 1 ? 1 : 0));
    map_tools_send(f, "Please Wait...");
    return 1;
}

// searchPData(): the player of #searchDInput on the Data tab
H(h_search_pdata) {
    UNUSED;
    char player[64] = "";
    if (el("searchDInput")) html_dom_get_value(el("searchDInput"), player, sizeof(player));
    if (!player[0] || !g_map_tools.name[0]) return 1;
    cJSON *f = cJSON_CreateArray();
    cJSON_AddItemToArray(f, cJSON_CreateString("sd"));
    cJSON_AddItemToArray(f, cJSON_CreateString(g_map_tools.name));
    cJSON_AddItemToArray(f, cJSON_CreateString(player));
    map_tools_send(f, "Searching...");
    return 1;
}

// deleteData(player, key): one key of one player
H(h_delete_data) {
    UNUSED;
    const char *player = arg_s(ev, args, argc, 0), *key = arg_s(ev, args, argc, 1);
    if (!player || !key || g_map_tools.id <= 0) return 1;
    cJSON *f = cJSON_CreateArray();
    cJSON_AddItemToArray(f, cJSON_CreateString("dd"));
    cJSON_AddItemToArray(f, cJSON_CreateString(player));
    cJSON_AddItemToArray(f, cJSON_CreateString(key));
    cJSON_AddItemToArray(f, cJSON_CreateNumber(g_map_tools.id));
    map_tools_send(f, "Please Wait...");
    return 1;
}

// deleteDataPop() (hub-10.0.7.js:3627-3632): which key to delete for every player (signed in)
H(h_delete_data_pop) {
    UNUSED;
    if (!g_hub.logged_in) return 1;
    generic_popup("<div class='imageListHeader' style='font-size: 26px;'>Delete Data</div><div style='font-size:20px;color:#fff'>"
                  "<div style='margin-top: 3px;font-size: 22px;color: rgba(255, 255, 255, 0.7);'>Key:<input type='text' id='deleteDKey' class='inputGrey2' placeholder='Key'></div>"
                  "<div class='mapFndB' onclick='window.deleteDataConf()' onmouseover='playTick(0.1)' style='background-color:#fc5a4e;width: 380px;margin-top: 10px;'>Delete All From Key</div></div>",
                  "imageNewPop", NULL);
    return 1;
}

// deleteDataConf() (:3634-3642): "No Key Provided", else the confirmation; YES deleteKeyData(key), NO showPopup()
H(h_delete_data_conf) {
    UNUSED;
    if (!g_hub.logged_in) return 1;
    g_map_tools.key[0] = 0;
    if (el("deleteDKey")) html_dom_get_value(el("deleteDKey"), g_map_tools.key, sizeof(g_map_tools.key));
    if (!g_map_tools.key[0]) {
        io_po(0, "No Key Provided");
        return 1;
    }
    StrBuf sb = {0};
    sb_append(&sb, "<div style='color:#fff;font-size:18px;position:relative;margin-bottom:5px;color:#FBC02D'>This action is not reversible!</div>"
                   "<div style='color:#fff;font-size:19px;position:relative;margin-bottom:15px;'>Are you sure you want to delete this data?</div>"
                   "<div id='confirmBtn' onclick='deleteKeyData(&quot;");
    sb_append_js_escape(&sb, g_map_tools.key);
    sb_append(&sb, "&quot;)' onmouseover='SOUND.play(&quot;tick_0&quot;,0.1)'>YES</div>"
                   "<div id='declineBtn' onclick='showPopup()' onmouseover='SOUND.play(&quot;tick_0&quot;,0.1)'>NO</div>");
    char *html = sb_finish(&sb);
    generic_popup(html, "confSubmitPop", NULL);
    free(html);
    return 1;
}

// deleteKeyData([key]): the key of every player (the confirmed key when none is given)
H(h_delete_key_data) {
    UNUSED;
    const char *key = argc > 0 ? arg_s(ev, args, argc, 0) : g_map_tools.key;
    if (!key || !*key || g_map_tools.id <= 0) return 1;
    cJSON *f = cJSON_CreateArray();
    cJSON_AddItemToArray(f, cJSON_CreateString("dkd"));
    cJSON_AddItemToArray(f, cJSON_CreateNumber(g_map_tools.id));
    cJSON_AddItemToArray(f, cJSON_CreateString(key));
    map_tools_send(f, "Deleting...");
    return 1;
}

// togglPos(): the KR giveaway switch of the post box (#givaButton, the KR field #krInptHol)
H(h_toggle_pos) {
    UNUSED;
    g_hub.giveaway = !g_hub.giveaway;
    set_style("givaButton", "background-color", g_hub.giveaway ? "#ebb734" : "rgba(0,0,0,0.3)");
    set_display("krInptHol", g_hub.giveaway ? "block" : "none");
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
    {"knLoginToggle", h_login_toggle},
    {"knLoginForgot", h_login_forgot},
    {"knLoginOverlay", h_login_overlay},
    {"knLoginKey", h_login_key},
    {"searchProfile", h_search_profile},
    {"openSettings", h_open_settings},
    {"openOneTrust", h_noop},
    {"setSetting", h_set_setting},
    {"delayExecute", h_delay_execute},
    {"showPopup", h_show_popup},
    {"buyblackM", h_buy_illicit},
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
    {"hostMap", h_host_map},
    {"playMod", h_play_mod},
    {"updateMapTab", h_update_map_tab},
    {"revertMapVersion", h_revert_map_version},
    {"revertMapVersionGo", h_revert_map_version_go},
    {"shareMapLink", h_share_link},
    {"shareGameTweet", h_share_game_tweet},
    {"chatBanUser", h_chat_ban_user},
    {"loadMoreTradeHistory", h_load_more_trade_history},
    {"openReport", h_open_report},
    {"viewReporter", h_view_reporter},
    {"shareModLink", h_share_link},
    {"publishPopup", h_publish_popup},
    {"publishModPopup", h_publish_mod_popup},
    {"updateMod", h_update_mod},
    {"showFriends", h_show_friends},   // the chat sidebar's friends (io_xhtb)
    {"removeFriend", h_remove_friend},
    {"createTip", h_create_tip},
    {"cancelTip", h_cancel_tip},
    {"createBadgeTip", h_create_badge_tip},
    {"selectFile", h_mod_form},  // the Publish / Update Mod popup (hub-10.0.7.js:6746-6754, mods_ui.c)
    {"uploadMod", h_mod_form},
    {"updateFile", h_noop},      // its hidden file inputs' onchange: selectFile's platform dialog fills them in place
    {"switchFeedTab", h_switch_feed_tab},
    {"toggleView", h_toggle_view},
    {"favoriteM", h_favorite_m},
    {"instaFollo", h_insta_follo},
    {"deleteFollower", h_delete_follower},
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
    {"giftPopup", h_gift_popup},          // economy (doc/v7/economy.md)
    {"calcGiftFee", h_calc_gift_fee},
    {"giftUser", h_gift_user},
    {"confPremium", h_conf_premium},
    {"premiumGiftConfirmed", h_premium_gift_confirmed},
    {"tradePopup", h_trade_popup},
    {"knPmMatch", h_pm_match},
    {"knPmSort", h_pm_sort},
    {"addItem", h_add_item},
    {"removeItem", h_remove_item},
    {"searchItems", h_search_items},
    {"showMissingItems", h_show_missing},
    {"tradeKR", h_trade_kr},
    {"confSubmitOffer", h_conf_submit_offer},
    {"submitOffer", h_submit_offer},
    {"viewOffer", h_view_offer},
    {"confOffer", h_conf_offer},
    {"offerConfirmed", h_offer_action},
    {"declOffer", h_offer_action},
    {"krHistory", h_kr_history},
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
    {"verifyPopup", h_verify_popup},
    {"verifyUser", h_verify_user},
    {"creatorPopup", h_creator_popup},
    {"creatorUser", h_creator_user},
    {"flagPopup", h_flag_popup},
    {"flagUser", h_flag_user},
    {"badgePopup", h_badge_popup},
    {"badgeUser", h_badge_user},
    {"reportPopup", h_report_popup},
    {"reportUser", h_report_user},
    {"shareProfile", h_share_profile},
    {"togglPos", h_toggle_pos},
    {"featureCreation", h_feature_creation},
    {"deleteCreationConf", h_delete_creation_conf},
    {"deleteCreation", h_delete_creation},
    {"openEdit", h_open_edit},           // the map page tools (hub_map_tools_frame)
    {"grabMap", h_grab_map},
    {"chngMapFunds", h_chng_map_funds},
    {"updateMapLB", h_update_map_lb},
    {"searchPData", h_search_pdata},
    {"deleteData", h_delete_data},
    {"deleteDataPop", h_delete_data_pop},
    {"deleteDataConf", h_delete_data_conf},
    {"calcMapFee", h_calc_map_fee},
    {"deleteKeyData", h_delete_key_data},
    {"logBtnFRVR", h_noop},     // analytics
    {"imagePopup", h_image_collection}, // the settings' image collection (image_collection.c)
    {"imageNewPopup", h_image_collection},
    {"window.imageNewPopup", h_image_collection},
    {"addNewImage", h_image_collection},
    {"window.addNewImage", h_image_collection},
    {"removeImage", h_image_collection},
    {"selectImage", h_image_collection},
};

// Krunker functions that need an account (follow, trade, post, chat, filters, ...): no-ops
// for a guest, logged like any other dispatch.
static int fallback_handler(void *user, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) user;
    // v7.1.4 social.html's #twoFAPop (a 2FA account's password login): account_ui.c owns it
    if (account_ui_twofa_handler(html_ui_current_handler())) return account_ui_handle(html_ui_current_handler(), ev, args, argc) != 0;
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
    // iïîííïì's "./?profile=<name>" (config-10.0.7.js:4877: the market popup's seller): v10 opens the game page's
    // profile; KrunkNative's game page has no ?profile, so the hub's own profile page opens
    if (!strncmp(href, "./?profile=", 11) || !strncmp(href, "/?profile=", 10)) {
        char url[600];
        snprintf(url, sizeof(url), "/social.html?p=profile&q=%s", strchr(href, '=') + 1);
        app_route_url_target(url, !strcmp(target, "_blank"));
        return 0;
    }
    if (!app_route_url_target(href, !strcmp(target, "_blank"))) ui_open_external(href);
    return 0;
}

// ------------------------------------------------------------------ AppPage hooks

static void hub_connected(void) {
    // captchaCallback -> io.connect(...) callback: guest (no saved token) -> Login button
    g_hub.connected = 1;
    char name[64] = "";
    // the saved session: the alt vault's (account_ui.c), not the plain krunker_token key, which a cipher build empties
    if (g_hub.brain && account_saved_name(name, sizeof(name)) && account_session_token()[0] && name[0]) {
        if (active_account()) {
            // account_ui.c's restore already answered on this socket
            g_hub.fe = 1;
            io_a(NULL);
        } else {
            // q && G: io.send("a", 1, [q, null, G]) is account_ui.c's restore (same socket)
            set_display("profileLogin", "none");
            set_html("profileName", I18N("social.login.proccessing", "Logging in..."));
            g_hub.fe = 1;
            g_hub.page_after_login = 1;
        }
    } else {
        set_display("profileLogin", "inline-block");
        set_html("profileName", "");
    }
    if (g_hub.brain) { // the server's economy rules (fees, levels, limits): "ecfg"
        cJSON *f = cJSON_CreateArray();
        cJSON_AddItemToArray(f, cJSON_CreateString("ecfg"));
        hub_send(f);
    }
    // API_CLIENT.listMaps(2, 0): the featured games of the maps Home card (v3540); KN's /maps
    // index 1 is Featured
    api_request("/maps", 1, 0, NULL, NULL, NO_CATEGORY);
    if (!g_hub.page_after_login) request_page(); // Xa(v, b, K, q, k, V, x); else at io_a
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
    // the app badge: v10 shows #androidDB when window.canShowExternalLinks (always true in
    // KrunkNative, as in ui_handlers.c; social/renamed.js:1514-1518); #iosDB stays hidden and v10's
    // page has no #steamDB. KrunkNative: the badge opens the menu's Download window (/?download: the
    // Android APK; tools/ui-import/social_page.py APP_BADGES).
    set_display("androidDB", "block");

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
    vgrid_tick(&g_market_grid); // the card lists follow their scroll positions (ui_vgrid.h)
    vgrid_tick(&g_items.grid);
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
