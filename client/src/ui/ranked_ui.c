// Krunker v10.0.2 ranked (doc/v10/ranked-impl.md): the Ranked popup and the queue session.
//
// v10's Ranked is the Svelte component _37935 (v10:111440-112700) that openRankedMenu() mounts with
// genericPopup(.., 'rankedMenuModal'); window 27 is an empty legacy slot (gen() returns ''), so
// showWindow(27) opens the popup here too. The markup is v10's templates (svelte-mqcul7 frame:
// tabs Home / Profile / Leaderboard / Match History / Shop, the region indicator, the footer with
// the matchmaking status, "All regions" and the start button; svelte-17kea4i home: rank card,
// svelte-6hxhl0 Ranked Progression with milestones, Ranked Challenges link, map carousel;
// svelte-18dbnt3 profile; svelte-1enb567 leaderboard; svelte-1irqx9a match history; svelte-1gtpoav
// shop with svelte-1q06uv7 items; svelte-jxc66d message overlay), styled by ui/css/svelte.css.
// While searching, the .ranked-matchmaking-status banner (svelte-3k0h7w) shows in
// #matchmakingStatusEl like v10's matchmakingStatusEl mount.
//
// Transport (deviation, doc/v10/ranked-impl.md): v10 opens a WebSocket to its matchmaker; this
// client long-polls the brain's /mm/ranked/* routes (same {type, payload} messages), so the queue
// works through the HTTPS edge relays and in the web build. A MATCHED assignment is joined after
// 1 s (v10: pop_3, wait 1 s, acceptMatch) with its ticket.
#include <v10_ui.h>
#include <v10_menu.h> // v10_menu_handle("loginOrRegister"): guests get v10's login modal
#include <audio.h>
#include <backend.h>
#include <backend_ui.h>
#include <ui_logic.h>
#include <html_ui.h>
#include <ranked.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../viewer/viewer_store.h"

#define POLL_WAIT_S 6           // long poll: under backend_native.c HTTP_TIMEOUT_MS (8 s), so an idle poll answers instead of timing out
#define DATA_TTL_MS 60000       // v10's request cache: 60 s
#define ACCEPT_DELAY_MS 1000    // MATCHED: wait 1 s, then acceptMatch()
#define POPUP_CLASS "rankedMenuModal"

enum { TAB_HOME, TAB_PROFILE, TAB_LEADERBOARD, TAB_HISTORY, TAB_SHOP, TAB_COUNT };
static const char *const k_tabs[TAB_COUNT] = {"Home", "Profile", "Leaderboard", "Match History", "Shop"};

enum { MM_IDLE, MM_SEARCHING, MM_FOUND };

#define HISTORY_PAGE 5          // _36203 pageSize 5
#define MATCHES_MAX 16

static struct {
    int tab;
    int state;                    // currentMatchmakingState
    char key[128];                // the queue's clientKey ("" = not queued)
    double queued_at;             // time() of QUEUED
    int poll_inflight;
    cJSON *assignment;            // MATCHED assignment (owned)
    int in_match;                 // joined a ranked match
    char status[128];             // the status line ("" = the default)
    char status_icon[16];
    char status_color[16];
    int status_lock;              // "You are already in a Ranked game." (_37948): REJOIN
    int busy;                     // _37947: LOADING... (the connection check, a request in flight)
    char rejoin[96];              // /mm/ranked/connection serverUrl ("" none)
    cJSON *rejoin_as;
    int all_regions;              // s_rankedAllRegions
    int board_page;               // leaderboard page (1-based)
    int history_page;             // match history page (1-based)
    int expanded;                 // progression details open
    int match_open[MATCHES_MAX];  // match history: expanded entries
    char match_sort[MATCHES_MAX][24];
    int match_desc[MATCHES_MAX];
    struct {                      // the message overlay (_34187)
        int visible;
        char title[64], text[256], button[24], color[16], icon[16];
        int force_close;
        int intro;                // 200 ms after it appeared: Svelte's scale / fade intro (g:114154-114158)
        int outro;                // 200 ms after the button / Escape hid it: the same transition backwards
        int timer;                // the intro / outro's end (overlay_done)
    } overlay;
    int buying;                   // a purchase in flight
    int tick_timer;
    int scenario;
} R = {.board_page = 1, .history_page = 1};

static const AccountInfo *acc(void) { return account_get(); }

static int current_season(void) { return ranked_season_at((int64_t) time(NULL)); }

// v10 regionAcronym (_34037): the default region folds oc -> as, sa -> na; ranked regions are as /
// eu / na. KrunkNative's defaultRegion values are its own server regions, folded by their prefix;
// krunker_rankedRegion (KrunkNative) pins one. Default na: KrunkNative's game servers are there.
static const char *my_region(void) {
    static char region[8];
    if (html_storage_get("krunker_rankedRegion", region, sizeof(region)) >= 0 && ranked_region_index(region) >= 0) return region;
    const cJSON *def = ui_setting_saved("defaultRegion");
    const char *v = cJSON_IsString(def) ? def->valuestring : "";
    static const struct { const char *prefix, *region; } fold[] = {
        {"eu", "eu"}, {"de", "eu"}, {"fr", "eu"}, {"gb", "eu"}, {"uk", "eu"}, {"nl", "eu"}, {"pl", "eu"},
        {"as", "as"}, {"sg", "as"}, {"jp", "as"}, {"jb", "as"}, {"au", "as"}, {"oc", "as"}, {"in", "as"}, {"kr", "as"}, {"hk", "as"},
        {"na", "na"}, {"us", "na"}, {"ca", "na"}, {"sa", "na"}, {"br", "na"}, {"mx", "na"},
    };
    snprintf(region, sizeof(region), "na");
    for (size_t i = 0; v && v[0] && i < sizeof(fold) / sizeof(fold[0]); i++)
        if (!strncmp(v, fold[i].prefix, strlen(fold[i].prefix))) {
            snprintf(region, sizeof(region), "%s", fold[i].region);
            break;
        }
    return region;
}

static void token(char *out, const size_t n) { v10_url_token(out, n); }

// ------------------------------------------------------------------ data (v10_get, 60 s cache)

static const V10Req *get_profile(const int season) {
    char t[256], path[512];
    token(t, sizeof(t));
    snprintf(path, sizeof(path), "/mm/ranked/profile?token=%s&season=%d&region=%s", t, season, my_region());
    return v10_get(path, DATA_TTL_MS);
}

static const cJSON *profile_of(const V10Req *r) { return r->status == 200 ? cJSON_GetObjectItem(r->json, "profile") : NULL; }

static const V10Req *get_board(void) {
    char path[256];
    snprintf(path, sizeof(path), "/mm/ranked/leaderboard?season=%d&region=%s&limit=50&offset=%d", current_season(), my_region(), R.board_page - 1);
    return v10_get(path, DATA_TTL_MS);
}

static const V10Req *get_history(void) {
    char t[256], path[512];
    token(t, sizeof(t));
    snprintf(path, sizeof(path), "/mm/ranked/history?token=%s&season=%d&region=%s&limit=%d&offset=%d", t, current_season(), my_region(), HISTORY_PAGE,
             R.history_page - 1);
    return v10_get(path, DATA_TTL_MS);
}

static const V10Req *get_shop(void) {
    char t[256], path[400];
    token(t, sizeof(t));
    snprintf(path, sizeof(path), "/mm/ranked/shop?token=%s", t);
    return v10_get(path, DATA_TTL_MS);
}

static void invalidate(void) { v10_forget("/mm/ranked/"); }

static void render(void);

// ------------------------------------------------------------------ the status line and banner

static void set_status(const char *text, const char *icon, const char *color) {
    snprintf(R.status, sizeof(R.status), "%s", text ? text : "");
    snprintf(R.status_icon, sizeof(R.status_icon), "%s", icon ? icon : "");
    snprintf(R.status_color, sizeof(R.status_color), "%s", color ? color : "#5ce05a");
}

static void status_line(const char **text, const char **icon, const char **color) {
    if (R.status_lock) {
        *text = "You are already in a Ranked game.", *icon = "\xe2\x9a\x94\xef\xb8\x8f", *color = "#ffcc33";
    } else if (R.status[0]) {
        *text = R.status, *icon = R.status_icon, *color = R.status_color;
    } else {
        *text = "Ready to compete? Join the ranked queue!", *icon = "\xe2\x9a\x94\xef\xb8\x8f", *color = "#5ce05a";
    }
}

// The emoji of v10's ranked texts (lock, magnifier, sparkles, cross, swords) are images (v10_emoji).
static void emoji(StrBuf *sb, const char *icon, const char *cls) {
    v10_emoji(sb, icon, cls);
}

static void fmt_time(char *out, const size_t n, const int s) { snprintf(out, n, "%d:%02d", s / 60, s % 60); }

// $rankedMatchMakingStatus: the .ranked-matchmaking-status banner (v10 matchmakingStatusEl)
static void update_banner(void) {
    const HtmlElem holder = html_dom_by_id("matchmakingStatusEl");
    if (!holder) return;
    char text[96] = "";
    if (R.state == MM_SEARCHING) {
        char t[16];
        fmt_time(t, sizeof(t), (int) ((double) time(NULL) - R.queued_at));
        snprintf(text, sizeof(text), "Searching for match... (%s)", t);
    } else if (R.state == MM_FOUND) {
        snprintf(text, sizeof(text), "Match found!");
    }
    StrBuf sb = {0};
    sb_appendf(&sb, "<div class=\"ranked-matchmaking-status svelte-3k0h7w%s\">", R.state != MM_IDLE ? " show" : "");
    sb_append_esc(&sb, text);
    sb_append(&sb, "</div>");
    char *html = sb_finish(&sb);
    html_dom_set_html(holder, html);
    free(html);
}

// ------------------------------------------------------------------ queue session (MatchmakingService)

static void poll_fire(void *user);

static void clear_state(void) { // clearState
    R.state = MM_IDLE;
    R.key[0] = 0;
    R.queued_at = 0;
    cJSON_Delete(R.assignment);
    R.assignment = NULL;
    set_status("", NULL, NULL);
    update_banner();
}

// v10's message overlay mounts and unmounts with `transition:scale={{start: 0.9, duration: 200}}` and fade:
// both ways, 200 ms (g:114154-114158, the 'both' transitions). The intro and outro classes ride on every
// render inside their 200 ms, so the poll's re-renders do not cut the animation short.
static void overlay_done(void *user) {
    (void) user;
    R.overlay.timer = 0;
    R.overlay.intro = 0;
    if (R.overlay.outro) {
        R.overlay.outro = 0;
        render(); // the outro is over: the overlay is unmounted
    }
}

static void overlay_timer_restart(void) {
    if (R.overlay.timer) html_timer_cancel(R.overlay.timer);
    R.overlay.timer = html_timer_add(200, overlay_done, NULL);
}

static void show_overlay(const char *title, const char *text, const char *button, const char *color, const char *icon, const int force_close) {
    R.overlay.visible = 1;
    snprintf(R.overlay.title, sizeof(R.overlay.title), "%s", title);
    snprintf(R.overlay.text, sizeof(R.overlay.text), "%s", text);
    snprintf(R.overlay.button, sizeof(R.overlay.button), "%s", button ? button : "OK");
    snprintf(R.overlay.color, sizeof(R.overlay.color), "%s", color ? color : "#5ce05a");
    snprintf(R.overlay.icon, sizeof(R.overlay.icon), "%s", icon ? icon : "");
    R.overlay.force_close = force_close;
    R.overlay.intro = 1;
    R.overlay.outro = 0;
    overlay_timer_restart();
}

static void internal_error(void) {
    show_overlay("Internal Error", "An internal error occurred. Please try again later.", "OK", "#ff3939", "\xe2\x9d\x8c", 0);
}

static void join_assignment(const cJSON *as) {
    const char *host = v10_str(as, "host"), *game = v10_str(as, "gameId"), *ticket = v10_str(as, "clientId");
    const char *web = cJSON_GetStringValue(cJSON_GetObjectItem(as, "webUrl"));
    const int port = (int) v10_num(as, "port", 0);
    if (!host[0] || port <= 0) return;
    printf("[ranked] joining %s (%s)\n", game, v10_str(as, "connection"));
    R.in_match = 1;
    invalidate();
    v10_popup_close();      // quickJoinRankedMatch / acceptMatch: clearPops()
    menu_show_window(MENU_WINDOW_NONE, 0);
    servers_ui_join_ticket(host, port, game, ticket, web);
}

static void accept_fire(void *user) { // acceptMatch: "Connecting to match..."
    (void) user;
    if (R.state != MM_FOUND || !R.assignment) return;
    cJSON *as = R.assignment;
    R.assignment = NULL;
    clear_state();
    join_assignment(as);
    cJSON_Delete(as);
    render();
}

// handleWebSocketMessage({type, payload})
static void handle_message(const cJSON *msg) {
    const char *type = v10_str(msg, "type");
    const cJSON *p = cJSON_GetObjectItem(msg, "payload");
    if (!strcmp(type, "ERROR")) {
        const char *code = v10_str(p, "code");
        const cJSON *inner = cJSON_GetObjectItem(p, "payload");
        clear_state();
        R.busy = 0;
        if (!strcmp(code, "MIN_LEVEL_REQUIREMENT"))
            show_overlay("Level Required", "You must be level 10 or higher to access Ranked Mode.", "OK", "#ff3939", "\xf0\x9f\x94\x92", 0);
        else if (!strcmp(code, "LOGIN_REQUIRED"))
            show_overlay("Login Required", "You must be logged in to access Ranked Mode.", "GOT IT", "#5ce05a", "\xf0\x9f\x94\x92", 1);
        else if (!strcmp(code, "NO_RANK_DATA"))
            show_overlay("Rank Data Missing", "We could not retrieve your ranked data. Please try again later.", "OK", "#ff3939", "\xe2\x9d\x8c", 0);
        else if (!strcmp(code, "COOLDOWN")) {
            char t[16], text[160];
            fmt_time(t, sizeof(t), (int) (v10_num(inner, "cooldown", 0) / 1000.0));
            snprintf(text, sizeof(text), "You are on cooldown and cannot queue for ranked matches. Cooldown will end in %s", t);
            show_overlay("Cooldown", text, "OK", "#5ce05a", NULL, 0);
        } else if (!strcmp(code, "ALREADY_IN_MATCH")) {
            snprintf(R.rejoin, sizeof(R.rejoin), "%s", v10_str(inner, "serverUrl"));
            cJSON_Delete(R.rejoin_as);
            const cJSON *as = cJSON_GetObjectItem(inner, "assignment");
            R.rejoin_as = as ? cJSON_Duplicate(as, 1) : NULL;
            R.status_lock = 1;
        } else {
            internal_error();
        }
        render();
        return;
    }
    if (strcmp(type, "QUEUE_STATUS")) return;
    const char *status = v10_str(p, "status");
    if (!strcmp(status, "QUEUED")) {
        const char *key = cJSON_GetStringValue(cJSON_GetObjectItem(p, "clientKey"));
        if (key) snprintf(R.key, sizeof(R.key), "%s", key);
        if (R.state != MM_SEARCHING) {
            R.state = MM_SEARCHING;
            R.queued_at = (double) time(NULL);
        }
        R.busy = 0;
    } else if (!strcmp(status, "MATCHED")) {
        R.state = MM_FOUND;
        R.busy = 0;
        cJSON_Delete(R.assignment);
        R.assignment = cJSON_Duplicate(cJSON_GetObjectItem(p, "assignment"), 1);
        set_status("Match found!", "\xe2\x9c\xa8", "#5ce05a");
        audio_play("pop_3", 0.15f, 0, 1.0f);
        html_timer_add(ACCEPT_DELAY_MS, accept_fire, NULL);
    }
    update_banner();
    render();
}

static void on_poll(void *user, const int status, const char *body, const size_t length) {
    (void) user;
    (void) length;
    R.poll_inflight = 0;
    if (!R.key[0] || R.state != MM_SEARCHING) return;
    if (status == 404) { // the brain forgot the key (it stopped polling for too long)
        clear_state();
        set_status("Failed to connect to matchmaking server", "\xe2\x9d\x8c", "#ff3939");
        render();
        return;
    }
    cJSON *msg = status == 200 ? cJSON_Parse(body) : NULL;
    if (cJSON_IsObject(msg)) handle_message(msg);
    cJSON_Delete(msg);
    if (R.state == MM_SEARCHING) html_timer_add(status == 200 ? 50 : 2000, poll_fire, NULL); // a transport error: back off
}

static void poll_fire(void *user) {
    (void) user;
    if (!R.key[0] || R.state != MM_SEARCHING || R.poll_inflight) return;
    char key[256], path[400];
    backend_url_encode(key, sizeof(key), R.key);
    snprintf(path, sizeof(path), "/mm/ranked/poll?clientKey=%s&wait=%d", key, POLL_WAIT_S);
    R.poll_inflight = 1;
    if (!backend_http_get(path, on_poll, NULL)) R.poll_inflight = 0;
}

static void on_queue(void *user, const int status, const char *body, const size_t length) {
    (void) user;
    (void) length;
    cJSON *msg = status == 200 ? cJSON_Parse(body) : NULL;
    if (cJSON_IsObject(msg)) handle_message(msg);
    else {
        clear_state();
        R.busy = 0;
        set_status("Failed to connect to matchmaking server", "\xe2\x9d\x8c", "#ff3939");
        render();
    }
    cJSON_Delete(msg);
    if (R.state == MM_SEARCHING) poll_fire(NULL);
}

// the status line ticks while searching (setupStatusUpdateInterval / the modal's 1 s interval)
static void status_tick(void *user) {
    (void) user;
    R.tick_timer = 0;
    if (R.state != MM_SEARCHING) return;
    char t[16], text[64];
    fmt_time(t, sizeof(t), (int) ((double) time(NULL) - R.queued_at));
    snprintf(text, sizeof(text), "Looking for match... (%s)", t);
    set_status(text, "\xf0\x9f\x94\x8d", "#5ce05a");
    update_banner();
    // only the footer's text changes: no full re-render every second
    HtmlElem st = v10_popup_open(POPUP_CLASS) ? html_dom_query(html_dom_by_id("genericPop"), ".status-text") : 0;
    if (st) html_dom_set_text(st, text);
    R.tick_timer = html_timer_add(1000, status_tick, NULL);
}

static void start_matchmaking(void) { // startMatchmaking(allRegions)
    if (R.state != MM_IDLE || !acc()) return;
    char t[256], path[512];
    token(t, sizeof(t));
    snprintf(path, sizeof(path), "/mm/ranked/queue?token=%s&regions=%s", t, R.all_regions ? "as,eu,na" : my_region());
    R.state = MM_SEARCHING;
    R.busy = 1;
    R.queued_at = (double) time(NULL);
    set_status("Looking for match... (0:00)", "\xf0\x9f\x94\x8d", "#5ce05a");
    if (!backend_http_get(path, on_queue, NULL)) {
        clear_state();
        R.busy = 0;
        internal_error();
    }
    if (!R.tick_timer) R.tick_timer = html_timer_add(1000, status_tick, NULL);
    update_banner();
}

static void cancel_matchmaking(void) { // cancelMatchmaking: v10 closes the socket
    if (R.key[0]) {
        char key[256], path[400];
        backend_url_encode(key, sizeof(key), R.key);
        snprintf(path, sizeof(path), "/mm/ranked/leave?clientKey=%s", key);
        backend_http_get(path, NULL, NULL);
    }
    clear_state();
}

static void on_connection(void *user, const int status, const char *body, const size_t length) {
    (void) user;
    (void) length;
    R.busy = 0;
    cJSON *res = status == 200 ? cJSON_Parse(body) : NULL;
    const char *url = cJSON_GetStringValue(cJSON_GetObjectItem(res, "serverUrl"));
    snprintf(R.rejoin, sizeof(R.rejoin), "%s", url && url[0] ? url : "");
    cJSON_Delete(R.rejoin_as);
    R.rejoin_as = url && url[0] ? cJSON_DetachItemFromObject(res, "assignment") : NULL;
    if (R.rejoin[0]) R.status_lock = 1; // "You are already in a Ranked game." + REJOIN
    cJSON_Delete(res);
    render();
}

static void fetch_connection(void) { // fetchIsPlayerInRankedMatch
    if (R.scenario || !acc() || backend_available() != 1) return;
    char t[256], path[400];
    token(t, sizeof(t));
    snprintf(path, sizeof(path), "/mm/ranked/connection?token=%s", t);
    R.busy = 1;
    if (!backend_http_get(path, on_connection, NULL)) R.busy = 0;
}

const char *ranked_ui_status(void) {
    if (html_dom_by_id("matchmakingStatusEl")) return NULL; // the v10 page shows its banner instead
    return R.state != MM_IDLE && R.status[0] ? R.status : NULL;
}
const char *ranked_ui_ready(void) { return NULL; } // v10: solo queue, no party ready count
int ranked_ui_in_ranked(void) { return R.state != MM_IDLE || R.in_match; }
void ranked_ui_left_match(void) {
    if (R.in_match) invalidate(); // the profile changed
    R.in_match = 0;
    R.status_lock = 0;
    R.rejoin[0] = 0;
}
void ranked_ui_class_changed(const int index) { (void) index; } // v10: the class is picked in the match's comp lobby

// ------------------------------------------------------------------ milestones, shop

static void on_claim(void *user, const int status, const cJSON *json) {
    (void) user;
    if (status != 200 || !cJSON_IsTrue(cJSON_GetObjectItem(json, "success"))) {
        const char *e = v10_str(json, "error");
        v10_view_push(v10_io_error("Error", e[0] ? e : "Failed to claim milestone"), NULL, NULL, NULL);
    }
    invalidate(); // _33975.clear(): the profile (and RP balance) again
    render();
}

typedef struct {
    int id;          // ranked_shop row id
    int skin;
    int price;
} Purchase;

static Purchase g_buy;

static void on_purchase(void *user, const int status, const cJSON *json) {
    (void) user;
    R.buying = 0;
    if (status == 200 && cJSON_IsTrue(cJSON_GetObjectItem(json, "success"))) {
        // _37818: the item and "Successfully added to your inventory!"
        StrBuf sb = {0};
        sb_append(&sb, "<div class=\"purchase-confirmation svelte-1lii2o\"><div class=\"item-preview svelte-1lii2o\">");
        v10_market_card(&sb, g_buy.skin, 120, 120, "center", NULL, NULL);
        sb_append(&sb, "</div> <div class=\"purchase-details svelte-1lii2o\"><div class=\"success-message svelte-1lii2o\">"
                       "<span class=\"success-text svelte-1lii2o\">Successfully added to your inventory!</span></div></div></div>");
        char *msg = sb_finish(&sb);
        v10_view_clear();
        v10_view_push(v10_io_success("Purchase complete!", msg, "Nice!", "accept slim"), NULL, NULL, NULL);
        free(msg);
        account_add_skin(g_buy.skin);
    } else {
        const char *e = v10_str(json, "error");
        v10_view_push(v10_io_error("Error", e[0] ? e : "Purchase failed. Please try again."), NULL, NULL, NULL);
    }
    invalidate();
    render();
}

static void confirm_closed(const int accepted, void *user) {
    (void) user;
    if (!accepted || R.buying) return;
    char t[256], path[400];
    token(t, sizeof(t));
    snprintf(path, sizeof(path), "/mm/ranked/purchase?token=%s&id=%d", t, g_buy.id);
    R.buying = 1;
    render();
    v10_send("POST", path, NULL, on_purchase, NULL);
}

// _36471: "Confirm purchase?" with the item card and the price
static void confirm_purchase(void) {
    StrBuf sb = {0};
    sb_append(&sb, "<div class=\"popup-container svelte-1wz7ymp\"><div class=\"popup svelte-1wz7ymp\" style=\"width: 450px;\">"
                   "<div class=\"confirm-purchase-container io-popup-container\"><div class=\"confirm-purchase-header io-popup-header io-popup-header--slim\">"
                   "<svg xmlns=\"http://www.w3.org/2000/svg\" class=\"confirm-icon svelte-1p13km7\" viewBox=\"0 0 24 24\" fill=\"currentColor\">"
                   "<path d=\"M12 2C6.48 2 2 6.48 2 12s4.48 10 10 10 10-4.48 10-10S17.52 2 12 2zm-2 15l-5-5 1.41-1.41L10 14.17l7.59-7.59L19 8l-9 9z\" class=\"svelte-1p13km7\"></path></svg>"
                   " Confirm purchase?</div> <div class=\"confirm-purchase-content io-popup-content svelte-1p13km7\"><div class=\"purchase-preview svelte-1p13km7\">"
                   "<div class=\"item-display-container svelte-1p13km7\">");
    v10_market_card(&sb, g_buy.skin, 120, 120, "top-center", NULL, NULL);
    sb_append(&sb, "</div> <div class=\"purchase-info svelte-1p13km7\"><!----> <div class=\"purchase-question svelte-1p13km7\">Are you sure you want to purchase this item?</div> "
                   "<div class=\"price-display\">");
    v10_currency(&sb, g_buy.price, "RP", "large", 1);
    sb_append(&sb, "</div></div></div> <div class=\"io-button-group svelte-1p13km7\"><button type=\"button\" class=\"io-button\" onclick=\"knViewPop(0)\">Cancel</button> "
                   "<button type=\"button\" class=\"io-button io-button--accept\" onclick=\"knViewPop(1)\">Buy</button></div></div></div></div></div>");
    v10_view_push(sb_finish(&sb), NULL, confirm_closed, NULL);
}

static const cJSON *shop_row(const int id) {
    const V10Req *r = get_shop();
    const cJSON *s;
    cJSON_ArrayForEach(s, cJSON_GetObjectItem(r->json, "skins")) {
        if ((int) v10_num(s, "id", -1) == id) return s;
    }
    return NULL;
}

static int has_num(const cJSON *o, const char *k) { return cJSON_IsNumber(cJSON_GetObjectItem(o, k)); }

static void mmr_requirement(const cJSON *s, char *out, const size_t n) {
    out[0] = 0;
    const int lo = has_num(s, "min_mmr"), hi = has_num(s, "max_mmr");
    if (lo && hi) snprintf(out, n, "Requires MMR: %.0f-%.0f", v10_num(s, "min_mmr", 0), v10_num(s, "max_mmr", 0));
    else if (lo) snprintf(out, n, "Requires MMR: %.0f+", v10_num(s, "min_mmr", 0));
    else if (hi) snprintf(out, n, "Requires MMR: up to %.0f", v10_num(s, "max_mmr", 0));
}

// restricted_by "mmr": the brain says so, else the player's rating is outside the item's range
static int mmr_restricted(const cJSON *s) {
    if (!strcmp(v10_str(s, "restricted_by"), "mmr")) return 1;
    if (!has_num(s, "min_mmr") && !has_num(s, "max_mmr")) return 0;
    const cJSON *p = profile_of(get_profile(current_season()));
    if (!p) return 0;
    const int mmr = ranked_display(v10_num(p, "conservative_rating", 0));
    return (has_num(s, "min_mmr") && mmr < v10_num(s, "min_mmr", 0)) || (has_num(s, "max_mmr") && mmr > v10_num(s, "max_mmr", 0));
}

static void countdown(StrBuf *sb, const double expires_ms, const char *hash) {
    const double left = expires_ms - (double) time(NULL) * 1000.0;
    char text[48];
    if (left <= 0) snprintf(text, sizeof(text), "Expired");
    else {
        const long long ms = (long long) left, d = ms / 86400000, h = ms % 86400000 / 3600000, m = ms % 3600000 / 60000, s = ms % 60000 / 1000;
        if (d > 0) snprintf(text, sizeof(text), "%lldd %lldh %lldm", d, h, m);
        else if (h > 0) snprintf(text, sizeof(text), "%lldh %lldm %llds", h, m, s);
        else if (m > 0) snprintf(text, sizeof(text), "%lldm %llds", m, s);
        else snprintf(text, sizeof(text), "%llds", s);
    }
    (void) hash;
    sb_appendf(sb, "<div class=\"countdown-container svelte-68lwmp%s\"><span class=\"material-icons svelte-68lwmp\">schedule</span> <span class=\"time-text svelte-68lwmp\">%s</span></div>",
               left <= 0 ? " expired" : "", text);
}

// _35204 with _37868: the item showcase (KrunkNative: its preview image, v10 a 3D viewer iframe) and
// the purchase section
static void item_view(const cJSON *s) {
    const int skin = (int) v10_num(s, "skin_id", -1), id = (int) v10_num(s, "id", 0);
    const ViewerSkin *vs = viewer_skin(skin);
    const int max = (int) v10_num(s, "max_purchase", 0), bought = (int) v10_num(s, "purchase_count", 0);
    const int sold = max > 0 && bought >= max, restricted = mmr_restricted(s);
    const char *color = vs && vs->rarity >= 0 && vs->rarity < g_viewer_rarity_count && g_viewer_rarities[vs->rarity].color ? g_viewer_rarities[vs->rarity].color : "#B0B0B0";
    char img[512], req[64];
    store_skin_preview(skin, img, sizeof(img));
    mmr_requirement(s, req, sizeof(req));
    StrBuf sb = {0};
    sb_appendf(&sb, "<div class=\"popup-container svelte-1wz7ymp\"><div class=\"popup svelte-1wz7ymp translucent slim-border\" style=\"width: 600px;\">"
                    "<div class=\"item-showcase-container io-popup-container svelte-2tp35a\"><div class=\"item-showcase-content io-popup-content svelte-2tp35a\">"
                    "<div class=\"image-display svelte-2tp35a\" style=\"border: 3px solid %s; background-image: url(%s);\"></div> "
                    "<div class=\"showcase-overlay svelte-2tp35a\"><!----> <div class=\"showcase-header svelte-2tp35a\"><!----> <div class=\"item-showcase-header svelte-2tp35a\">"
                    "<div class=\"item-info svelte-2tp35a\"><!----> <div class=\"item-name svelte-2tp35a\" style=\"color: %s;\">",
               color, img, color);
    sb_append_esc(&sb, vs && vs->name ? vs->name : "Unnamed Item");
    sb_append(&sb, "</div> <!----> <div class=\"item-description svelte-2tp35a\">");
    if (vs && vs->type > 0) {
        static const char *const types[] = {NULL, "Hat Item", "Body Item", "Melee Item", "Spray Item", "Dye Item", "Waist Item", "Face Item", "Shoe Item", "Pet Item",
                                            "Collectible Item", "Wrist Item", "Charm Item", "Spin Ticket Item", "Back Item", "Head Item", "Player Card"};
        if (vs->type < (int) (sizeof(types) / sizeof(types[0]))) sb_append(&sb, types[vs->type]);
    }
    sb_appendf(&sb, "</div> <div class=\"item-status-badge svelte-2tp35a\" style=\"background-color: #41c300;\">Ranked</div></div> "
                    "<div class=\"item-showcase-btns svelte-2tp35a\"><button class=\"more-info-btn svelte-2tp35a\" onclick=\"openSales(%d)\">More info</button> "
                    "<button class=\"share-btn svelte-1524ito\" type=\"button\" title=\"Copy link\" onclick=\"knRkShare(%d)\"><i class=\"material-icons svelte-1524ito\">share</i> "
                    "<span class=\"svelte-1524ito\">Share</span></button></div></div></div> ",
               skin, skin);
    if (sold) {
        sb_append(&sb, "<div class=\"not-available-section svelte-15t1j1p\"><div class=\"status-text sold-out svelte-15t1j1p\">SOLD OUT</div> "
                       "<div class=\"status-description svelte-15t1j1p\">This item has reached its purchase limit and is no longer available.</div></div>");
    } else {
        sb_append(&sb, "<div class=\"purchase-section svelte-r01l8m\"><div class=\"purchase-info svelte-r01l8m\"><div class=\"price-display svelte-r01l8m\">");
        v10_currency(&sb, v10_num(s, "price", 0), "RP", "large", 0);
        sb_append(&sb, "</div> ");
        if (max > 0) sb_appendf(&sb, "<div class=\"purchase-count svelte-r01l8m\">%d/%d remaining</div>", bought, max);
        else sb_append(&sb, "<!---->");
        sb_append(&sb, " ");
        if (has_num(s, "expires_at") && v10_num(s, "expires_at", 0) > 0) {
            sb_append(&sb, "<div class=\"expiration-info svelte-r01l8m\">");
            countdown(&sb, v10_num(s, "expires_at", 0), "r01l8m");
            sb_append(&sb, "</div>");
        } else sb_append(&sb, "<!---->");
        sb_appendf(&sb, "</div> <div class=\"io-button-group\"><button class=\"buy-button svelte-r01l8m\"%s%s%s onclick=\"knRkBuy(%d)\">", R.buying || restricted ? " disabled" : "",
                   restricted && req[0] ? " title=\"" : "", restricted && req[0] ? req : "", id);
        if (restricted && req[0]) sb_append(&sb, "\"");
        if (R.buying) sb_append(&sb, "<span class=\"material-icons svelte-r01l8m\">hourglass_empty</span> <span class=\"svelte-r01l8m\">Buying...</span>");
        else if (restricted) sb_appendf(&sb, "<span class=\"material-icons svelte-r01l8m\">lock</span> <span class=\"svelte-r01l8m\">%s</span>", req[0] ? req : "Rating Required");
        else sb_append(&sb, "<span class=\"material-icons svelte-r01l8m\">shopping_cart</span> <span class=\"svelte-r01l8m\">Buy Now</span>");
        sb_append(&sb, "</button></div></div>");
    }
    sb_append(&sb, "</div></div></div></div></div>");
    v10_view_push(sb_finish(&sb), NULL, NULL, NULL);
}

static void buy(const int id) { // _37823: the confirm dialog, then /ranked/purchase
    const cJSON *s = shop_row(id);
    if (!s) {
        v10_view_push(v10_io_error("Error", "Item not found in shop. Please refresh and try again."), NULL, NULL, NULL);
        return;
    }
    if (R.buying) return;
    g_buy.id = id;
    g_buy.skin = (int) v10_num(s, "skin_id", -1);
    g_buy.price = (int) v10_num(s, "price", 0);
    confirm_purchase();
}

// ------------------------------------------------------------------ handlers

static int arg_num(const HtmlArg *args, const int argc, const int i, const int def) {
    if (i >= argc) return def;
    if (args[i].kind == HTML_ARG_NUM || args[i].kind == HTML_ARG_BOOL) return (int) args[i].num;
    if (args[i].kind == HTML_ARG_STR && args[i].str) return atoi(args[i].str);
    return def;
}

static void open_popup(void);

int ranked_ui_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    if (!name) return -1;
    const int arg0 = arg_num(args, argc, 0, 0);
    if (!strcmp(name, "openRankedMenu")) { // v10: guests get loginOrRegister()
        if (acc() || R.scenario) open_popup();
        else if (v10_menu_handle("loginOrRegister", NULL, NULL, 0) < 0) menu_show_window(5, 1);
        return 1;
    }
    if (strncmp(name, "knRk", 4)) return -1;
    if (!strcmp(name, "knRkTab")) {
        if (arg0 >= 0 && arg0 < TAB_COUNT) R.tab = arg0;
    } else if (!strcmp(name, "knRkOverlay")) { // the overlay's button / Escape
        const int was_shown = R.overlay.visible;
        R.overlay.visible = 0;
        if (R.overlay.force_close) {
            render();
            v10_popup_close();
            return 1;
        }
        if (was_shown) { // the outro: the same transition backwards (the overlay unmounts when it ends)
            R.overlay.intro = 0;
            R.overlay.outro = 1;
            overlay_timer_restart();
        }
    } else if (!strcmp(name, "knRkAllRegions")) {
        int on = !R.all_regions;
        if (argc > 0 && (args[0].kind == HTML_ARG_THIS_CHECKED || args[0].kind == HTML_ARG_BOOL)) on = ev && args[0].kind == HTML_ARG_THIS_CHECKED ? ev->checked : (int) args[0].num;
        R.all_regions = on != 0;
        html_storage_set("s_rankedAllRegions", R.all_regions ? "true" : "false");
    } else if (!strcmp(name, "knRkStart")) { // the start button: REJOIN / FIND MATCH / CANCEL
        if (!acc() || R.busy || R.state == MM_FOUND) return 1;
        if (R.status_lock && R.rejoin_as) {
            cJSON *as = R.rejoin_as;
            R.rejoin_as = NULL;
            R.status_lock = 0;
            R.rejoin[0] = 0;
            join_assignment(as); // quickJoinRankedMatch(); clearPops()
            cJSON_Delete(as);
            return 1;
        }
        if (R.state == MM_IDLE) start_matchmaking();
        else if (R.state == MM_SEARCHING) cancel_matchmaking();
    } else if (!strcmp(name, "knRkClaim")) { // onClaimMilestone(index): the next one, in order
        if (!acc()) return 1;
        char t[256], path[400];
        token(t, sizeof(t));
        snprintf(path, sizeof(path), "/mm/ranked/claim-milestone?token=%s&region=%d", t, ranked_region_index(my_region()));
        v10_send("POST", path, NULL, on_claim, NULL);
        return 1;
    } else if (!strcmp(name, "knRkDetails")) {
        R.expanded = !R.expanded;
    } else if (!strcmp(name, "knRkBoardPage")) {
        const V10Req *r = get_board();
        const int pages = (int) v10_num(r->json, "totalPages", 1);
        const int next = R.board_page + arg0;
        if (next >= 1 && next <= (pages > 0 ? pages : 1)) R.board_page = next;
    } else if (!strcmp(name, "knRkHistoryPage")) {
        const V10Req *r = get_history();
        const int pages = (int) v10_num(cJSON_GetObjectItem(r->json, "matchHistory"), "totalPages", 1);
        const int next = R.history_page + arg0;
        if (next >= 1 && next <= (pages > 0 ? pages : 1)) {
            R.history_page = next;
            memset(R.match_open, 0, sizeof(R.match_open));
        }
    } else if (!strcmp(name, "knRkMatch")) {
        if (arg0 >= 0 && arg0 < MATCHES_MAX) R.match_open[arg0] = !R.match_open[arg0];
    } else if (!strcmp(name, "knRkSort")) { // (match, column)
        const int col = arg_num(args, argc, 1, 0);
        static const char *const keys[] = {"player_name", "score", "kills", "assists", "deaths", "kd", "objective_score", "damage_done"};
        if (arg0 >= 0 && arg0 < MATCHES_MAX && col >= 0 && col < 8) {
            if (!strcmp(R.match_sort[arg0], keys[col])) R.match_desc[arg0] = !R.match_desc[arg0];
            else {
                snprintf(R.match_sort[arg0], sizeof(R.match_sort[0]), "%s", keys[col]);
                R.match_desc[arg0] = 0;
            }
        }
        html_ui_stop_propagation();
    } else if (!strcmp(name, "knRkItem")) {
        const cJSON *s = shop_row(arg0);
        if (s) item_view(s);
        return 1;
    } else if (!strcmp(name, "knRkBuy")) {
        html_ui_stop_propagation();
        if (acc()) buy(arg0);
        return 1;
    } else if (!strcmp(name, "knRkShare")) {
        char origin[256], url[320];
        html_page_origin(origin, sizeof(origin));
        snprintf(url, sizeof(url), "%s/?item=%d", origin, arg0);
        html_clipboard_write(url);
        return 1;
    } else {
        return -1;
    }
    render();
    return 1;
}

// ------------------------------------------------------------------ v10 markup

static void rank_img(StrBuf *sb, const int rank, const char *cls, const char *alt) {
    sb_appendf(sb, "<img class=\"%s\" src=\"./%s\" alt=\"%s\">", cls, ranked_rank_icon(rank), alt ? alt : ranked_rank_name(rank));
}

// _34033: the derived profile fields (total_matches = wins + losses, averages, rates)
typedef struct {
    double matches, wins, losses, win_rate, kd, dpm, avg_kills, avg_deaths, avg_score, avg_obj, avg_acc, avg_assists;
} Derived;

static Derived derive(const cJSON *p) {
    Derived d = {0};
    d.wins = v10_num(p, "wins", 0);
    d.losses = v10_num(p, "losses", 0);
    // KrunkNative counts draws too (placement and ranks are the brain's); v10's wins + losses otherwise
    d.matches = v10_num(p, "total_matches", d.wins + d.losses);
    if (d.matches <= 0) return d;
    const double kills = v10_num(p, "total_kills", 0), deaths = v10_num(p, "total_deaths", 0), time_ms = v10_num(p, "total_time_played", 0);
    d.avg_kills = kills / d.matches;
    d.avg_deaths = deaths / d.matches;
    d.kd = kills / (deaths > 1 ? deaths : 1);
    d.dpm = time_ms > 0 ? v10_num(p, "total_damage_done", 0) / (time_ms / 60000.0) : 0;
    d.avg_score = v10_num(p, "total_score", 0) / d.matches;
    d.avg_obj = v10_num(p, "total_objective_score", 0) / d.matches;
    d.win_rate = d.wins / d.matches * 100.0;
    d.avg_acc = v10_num(p, "total_accuracy", 0) / d.matches;
    d.avg_assists = v10_num(p, "total_assists", 0) / d.matches;
    return d;
}

// _34236: the Ranked Progression component (svelte-6hxhl0)
static void gen_progression(StrBuf *sb, const cJSON *profile_res) {
    const cJSON *p = cJSON_GetObjectItem(profile_res, "profile");
    const cJSON *ms = cJSON_GetObjectItem(profile_res, "milestones");
    const int beta = current_season() == 0, matches = (int) v10_num(p, "total_matches", 0);
    const int last = (int) v10_num(p, "last_milestone_claimed", -1), max = RANKED_MAX_MATCHES, minm = RANKED_PLACEMENT_MATCHES;
    const double pct = matches * 100.0 / max > 100 ? 100 : matches * 100.0 / max;
    const double rate = beta ? (matches >= minm ? 1 : 0) : (matches >= minm ? (matches / 100.0 < 1 ? matches / 100.0 : 1) : 0);
    const long long points = matches >= minm ? (long long) (ranked_display(v10_num(p, "conservative_rating", 0)) * rate) : 0;
    int next_at = -1, i = 0;
    const int count = beta ? 0 : cJSON_GetArraySize(ms);
    const cJSON *m;
    cJSON_ArrayForEach(m, ms) {
        if (beta) break;
        if (next_at < 0 && matches < (int) v10_num(m, "matches", 0)) next_at = (int) v10_num(m, "matches", 0);
    }
    char mc[32];
    if (matches < max) snprintf(mc, sizeof(mc), "%d/%d", matches, max);
    else snprintf(mc, sizeof(mc), "%d", matches);
    sb_appendf(sb, "<div class=\"progression-container svelte-6hxhl0\"><div class=\"progression-header svelte-6hxhl0\"><h3 class=\"svelte-6hxhl0\">Ranked Progression</h3> "
                   "<div class=\"matches-info svelte-6hxhl0\"><span class=\"matches-count svelte-6hxhl0\">%s</span> <span class=\"matches-label svelte-6hxhl0\">matches</span></div></div> "
                   "<div class=\"progression-bar-container svelte-6hxhl0\"><div class=\"progression-bar svelte-6hxhl0\"><div class=\"progression-fill svelte-6hxhl0\" style=\"width: %g%%\"></div> ",
               mc, pct);
    cJSON_ArrayForEach(m, ms) {
        if (beta) break;
        const int at = (int) v10_num(m, "matches", 0), reward = (int) v10_num(m, "reward", 0);
        const int achieved = matches >= at, claimed = i <= last, claimable = achieved && i == last + 1;
        sb_appendf(sb, "<div class=\"milestone-marker %s %s %s svelte-6hxhl0%s\" style=\"left: %g%%\"%s><div class=\"milestone-line svelte-6hxhl0\"></div> "
                       "<div class=\"milestone-indicator svelte-6hxhl0%s\"><div class=\"milestone-number svelte-6hxhl0\">%d</div> ",
                   achieved ? "achieved" : "", claimed ? "claimed" : "", claimable ? "claimable" : "", i == count - 1 && achieved ? " last-and-achieved" : "",
                   at * 100.0 / max, claimable ? " onclick=\"playSelect();knRkClaim()\"" : "", achieved ? " reached" : "", at);
        if (claimed) sb_append(sb, "<div class=\"milestone-bonus claimed svelte-6hxhl0\">CLAIMED</div>");
        else if (claimable) sb_appendf(sb, "<div class=\"milestone-bonus claimable svelte-6hxhl0\">CLAIM +%d RP</div>", reward);
        else if (achieved) sb_appendf(sb, "<div class=\"milestone-bonus svelte-6hxhl0\">+%d RP</div>", reward);
        else sb_append(sb, "<!---->");
        sb_append(sb, "</div></div>");
        i++;
    }
    sb_appendf(sb, " <div class=\"progress-indicator svelte-6hxhl0\" style=\"left: %g%%\"><div class=\"indicator-dot svelte-6hxhl0\"></div></div></div></div> "
                   "<div class=\"progression-summary svelte-6hxhl0\"><div class=\"summary-left svelte-6hxhl0\">",
               pct);
    if (beta) sb_append(sb, "<span class=\"next-reward svelte-6hxhl0\"><span class=\"highlight svelte-6hxhl0\">Beta Season</span> - 1:1 conversion rate</span>");
    else if (next_at >= 0)
        sb_appendf(sb, "<span class=\"next-reward svelte-6hxhl0\"><span class=\"highlight svelte-6hxhl0\">%d matches</span> until next reward</span>", next_at - matches);
    else sb_append(sb, "<span class=\"next-reward svelte-6hxhl0\">All milestones completed!</span>");
    sb_appendf(sb, "</div> <div class=\"summary-right svelte-6hxhl0\"><span class=\"estimated-points svelte-6hxhl0\"><span class=\"points-value svelte-6hxhl0\">%lld RP</span> estimated</span> "
                   "<button class=\"details-toggle svelte-6hxhl0\" onclick=\"playTick(0.5);knRkDetails()\"><span class=\"material-icons svelte-6hxhl0\">%s</span></button></div></div> ",
               points, R.expanded ? "expand_less" : "expand_more");
    if (R.expanded) {
        char played[32];
        if (matches < max) snprintf(played, sizeof(played), "%d / %d", matches, max);
        else snprintf(played, sizeof(played), "%d", matches);
        sb_appendf(sb, "<div class=\"progression-details svelte-6hxhl0\"><div class=\"details-row svelte-6hxhl0\"><div class=\"detail-item svelte-6hxhl0\">"
                       "<span class=\"detail-label svelte-6hxhl0\">Conversion Rate</span> <span class=\"detail-value svelte-6hxhl0\">%.0f%%</span></div> ",
                   100.0 * rate);
        if (!beta && next_at >= 0)
            sb_appendf(sb, "<div class=\"detail-item svelte-6hxhl0\"><span class=\"detail-label svelte-6hxhl0\">Next Milestone</span> <span class=\"detail-value svelte-6hxhl0\">%d matches</span></div>",
                       next_at - matches);
        else sb_append(sb, "<!---->");
        sb_appendf(sb, " <div class=\"detail-item svelte-6hxhl0\"><span class=\"detail-label svelte-6hxhl0\">Matches Played</span> <span class=\"detail-value svelte-6hxhl0\">%s</span></div> "
                       "<div class=\"detail-item svelte-6hxhl0\"><span class=\"detail-label svelte-6hxhl0\">Estimated Points</span> <span class=\"detail-value svelte-6hxhl0\">%lld RP</span></div></div> "
                       "<div class=\"details-info svelte-6hxhl0\"><h4 class=\"svelte-6hxhl0\">How it works</h4> <ul class=\"svelte-6hxhl0\"><li class=\"svelte-6hxhl0\">Minimum %d matches to earn points</li> ",
                   played, points, minm);
        if (beta) sb_append(sb, "<li class=\"svelte-6hxhl0\">Beta Season: 1:1 conversion rate (100%)</li> <li class=\"svelte-6hxhl0\">No milestones during Beta Season</li>");
        else sb_append(sb, "<li class=\"svelte-6hxhl0\">100 matches = 100% conversion rate</li> <li class=\"svelte-6hxhl0\">Milestones give rewards when reached</li>");
        sb_append(sb, " <li class=\"svelte-6hxhl0\">Points awarded for your best region at end of month</li></ul></div></div>");
    } else sb_append(sb, "<!---->");
    sb_append(sb, "</div>");
}

// _34252 home (svelte-17kea4i)
static void gen_home(StrBuf *sb) {
    const int season = current_season();
    const V10Req *r = get_profile(season);
    sb_append(sb, "<div class=\"home-container svelte-17kea4i\">");
    if (r->status == 0 && !r->json) {
        sb_append(sb, "<div class=\"loading svelte-17kea4i\">Loading profile data...</div></div>");
        return;
    }
    if (r->status != 200) {
        sb_append(sb, "<div class=\"error svelte-17kea4i\">Failed to load data. Please try again.</div></div>");
        return;
    }
    const cJSON *p = cJSON_GetObjectItem(r->json, "profile");
    const int rank = (int) v10_num(p, "rank", 0);
    const Derived d = derive(p);
    sb_appendf(sb, "<div class=\"rank-card svelte-17kea4i\"><div class=\"season-banner svelte-17kea4i\">Season %d</div> <div class=\"rank-icon svelte-17kea4i\">", season);
    rank_img(sb, rank, "svelte-17kea4i", "Rank icon");
    sb_appendf(sb, "</div> <div class=\"rank-and-stats svelte-17kea4i\"><div class=\"rank-name svelte-17kea4i\" style=\"color: %s\">%s</div> ", ranked_rank_color(rank),
               ranked_rank_name(rank));
    if (rank == RANK_UNRANKED)
        sb_appendf(sb, "<div class=\"unranked-message svelte-17kea4i\">%d games remaining until ranked</div>", RANKED_PLACEMENT_MATCHES - (int) d.matches);
    else sb_append(sb, "<!---->");
    sb_append(sb, " ");
    if (rank >= RANK_DIAMOND)
        sb_append(sb, "<div class=\"decay-warning svelte-17kea4i\"><span class=\"warning-text svelte-17kea4i\">Your rating will decay after 7+ days of inactivity</span></div>");
    else sb_append(sb, "<!---->");
    sb_appendf(sb, " <div class=\"quick-stats svelte-17kea4i\"><div class=\"quick-stat svelte-17kea4i\"><span class=\"quick-stat-value svelte-17kea4i\">%d</span> "
                   "<span class=\"quick-stat-label svelte-17kea4i\">Rating</span></div> <div class=\"quick-stat svelte-17kea4i\"><span class=\"quick-stat-value svelte-17kea4i\">%.0f</span> "
                   "<span class=\"quick-stat-label svelte-17kea4i\">Matches</span></div> <div class=\"quick-stat svelte-17kea4i\"><span class=\"quick-stat-value svelte-17kea4i\">%.1f%%</span> "
                   "<span class=\"quick-stat-label svelte-17kea4i\">Win Rate</span></div> <div class=\"quick-stat svelte-17kea4i\"><span class=\"quick-stat-value svelte-17kea4i\">%.2f</span> "
                   "<span class=\"quick-stat-label svelte-17kea4i\">KD Average</span></div></div></div></div> ",
               rank != RANK_UNRANKED ? ranked_display(v10_num(p, "conservative_rating", 0)) : 0, d.matches, d.win_rate, d.kd);
    gen_progression(sb, r->json);
    sb_append(sb, " <div class=\"challenges-link svelte-17kea4i\" onclick=\"openRankedChallenges()\" onmouseenter=\"playTick()\"><span class=\"material-icons challenges-link-icon svelte-17kea4i\">"
                  "emoji_events</span> <span class=\"challenges-link-text svelte-17kea4i\">Ranked Challenges</span> <span class=\"material-icons svelte-17kea4i\">chevron_right</span></div> "
                  "<div class=\"description-container svelte-17kea4i\"><p class=\"svelte-17kea4i\">Compete in a best of 3 - 4v4 Hardpoint - map pool rotates every season</p> "
                  "<div class=\"map-carousel-wrapper svelte-17kea4i\">");
    int maps[RANKED_SEASON_MAPS];
    ranked_season_maps(season, maps);
    sb_appendf(sb, "<div class=\"map-carousel-container svelte-17kea4i\" style=\"--total-maps: %d; --extended-maps: %d;\">", RANKED_SEASON_MAPS, 2 * RANKED_SEASON_MAPS);
    for (int k = 0; k < 2 * RANKED_SEASON_MAPS; k++) { // [...maps, ...maps]: the carousel loops
        const int id = maps[k % RANKED_SEASON_MAPS];
        const char *name = ranked_pool_map_name(id);
        sb_appendf(sb, "<div class=\"map-card svelte-17kea4i\"><img class=\"map-image svelte-17kea4i\" src=\"./img/maps/map_%d.png\" alt=\"", id);
        sb_append_esc(sb, name ? name : "");
        sb_append(sb, "\"> <div class=\"map-name svelte-17kea4i\">");
        sb_append_esc(sb, name ? name : "");
        sb_append(sb, "</div></div>");
    }
    sb_append(sb, "</div></div></div></div>");
}

// _37741 profile (svelte-18dbnt3)
static void gen_profile(StrBuf *sb) {
    const V10Req *r = get_profile(current_season());
    sb_append(sb, "<div class=\"profile-container svelte-18dbnt3\">");
    if (r->status == 0 && !r->json) {
        sb_append(sb, "<div class=\"loading svelte-18dbnt3\">Loading profile data...</div></div>");
        return;
    }
    if (r->status != 200) {
        sb_append(sb, "<div class=\"error svelte-18dbnt3\">Failed to load profile. Please try again.</div></div>");
        return;
    }
    const cJSON *p = cJSON_GetObjectItem(r->json, "profile");
    const int rank = (int) v10_num(p, "rank", 0);
    const Derived d = derive(p);
    sb_append(sb, "<div class=\"profile-header svelte-18dbnt3\"><div class=\"rank-container svelte-18dbnt3\">");
    rank_img(sb, rank, "rank-icon svelte-18dbnt3", "Rank");
    sb_append(sb, "</div> <div class=\"profile-info svelte-18dbnt3\"><div class=\"username svelte-18dbnt3\">");
    const char *name = v10_str(p, "player_name");
    sb_append_esc(sb, name[0] ? name : acc() ? acc()->name : "");
    sb_appendf(sb, "</div> <div class=\"rank-display svelte-18dbnt3\" style=\"color: %s\">%s</div></div></div> ", ranked_rank_color(rank), ranked_rank_name(rank));
#define CARD(cls, val, label) sb_appendf(sb, "<div class=\"stat-card%s svelte-18dbnt3\"><span class=\"stat-value svelte-18dbnt3\">%s</span> <span class=\"stat-label svelte-18dbnt3\">%s</span></div>", cls, val, label)
    char v[16][48];
    snprintf(v[0], 48, "%.0f", d.matches);
    snprintf(v[1], 48, "%.0f", d.wins);
    snprintf(v[2], 48, "%.0f", d.losses);
    snprintf(v[3], 48, "%.1f%%", d.win_rate);
    snprintf(v[4], 48, "%.2f", d.kd);
    snprintf(v[5], 48, "%.2f", d.dpm);
    snprintf(v[6], 48, "%.0f", v10_num(p, "highest_kill_count", 0));
    snprintf(v[7], 48, "%.0f", v10_num(p, "highest_damage_in_match", 0));
    sb_append(sb, "<div class=\"stats-container svelte-18dbnt3\"><div class=\"stats-section svelte-18dbnt3\"><h2 class=\"svelte-18dbnt3\">Overview</h2> <div class=\"stats-grid svelte-18dbnt3\">");
    CARD("", v[0], "Matches");
    sb_append(sb, " ");
    CARD("", v[1], "Wins");
    sb_append(sb, " ");
    CARD("", v[2], "Losses");
    sb_append(sb, " ");
    CARD(" highlight", v[3], "Win Rate");
    sb_append(sb, "</div></div> <div class=\"stats-section svelte-18dbnt3\"><h2 class=\"svelte-18dbnt3\">Performance</h2> <div class=\"stats-grid svelte-18dbnt3\">");
    CARD(" highlight", v[4], "KD Average");
    sb_append(sb, " ");
    CARD("", v[5], "Damage per minute");
    sb_append(sb, " ");
    CARD("", v[6], "Highest Kill Count");
    sb_append(sb, " ");
    CARD("", v[7], "Highest Damage");
    sb_append(sb, "</div> <div class=\"section-divider svelte-18dbnt3\"></div> <h2 class=\"svelte-18dbnt3\">Matches Averages</h2> <div class=\"stats-grid svelte-18dbnt3\">");
    snprintf(v[0], 48, "%.2f", d.avg_kills);
    snprintf(v[1], 48, "%.2f", d.avg_deaths);
    snprintf(v[2], 48, "%.2f", d.avg_score);
    snprintf(v[3], 48, "%.2f", d.avg_obj);
    snprintf(v[4], 48, "%.2f", d.avg_acc);
    snprintf(v[5], 48, "%.2f", d.avg_assists);
    static const char *const avg_labels[] = {"Kills", "Deaths", "Score", "Objective", "Accuracy", "Assists"};
    for (int i = 0; i < 6; i++) {
        if (i) sb_append(sb, " ");
        CARD("", v[i], avg_labels[i]);
    }
    sb_append(sb, "</div> <div class=\"section-divider svelte-18dbnt3\"></div> <h2 class=\"svelte-18dbnt3\">Overall</h2> <div class=\"stats-grid svelte-18dbnt3\">");
    snprintf(v[0], 48, "%.0f", v10_num(p, "total_kills", 0));
    snprintf(v[1], 48, "%.0f", v10_num(p, "total_deaths", 0));
    snprintf(v[2], 48, "%.0f", v10_num(p, "total_assists", 0));
    snprintf(v[3], 48, "%.0f", v10_num(p, "total_score", 0));
    snprintf(v[4], 48, "%.0f", v10_num(p, "total_objective_score", 0));
    snprintf(v[5], 48, "%.0f", v10_num(p, "total_damage_done", 0));
    snprintf(v[6], 48, "%.1fh", v10_num(p, "total_time_played", 0) / 3600000.0);
    static const char *const tot_labels[] = {"Total Kills", "Total Deaths", "Total Assists", "Total Score", "Total Objective", "Total Damage", "Time Played"};
    for (int i = 0; i < 7; i++) {
        if (i) sb_append(sb, " ");
        CARD("", v[i], tot_labels[i]);
    }
#undef CARD
    sb_append(sb, "</div></div></div></div>");
}

// _37736 leaderboard (svelte-1enb567): 50 per page, the player's own row highlighted
static void gen_leaderboard(StrBuf *sb) {
    const V10Req *r = get_board();
    const cJSON *rows = cJSON_GetObjectItem(r->json, "leaderboard");
    const int loading = r->status == 0;
    sb_append(sb, "<div class=\"leaderboard-container svelte-1enb567\">");
    if (loading && !cJSON_GetArraySize(rows)) {
        sb_append(sb, "<div class=\"loading svelte-1enb567\">Loading leaderboard data...</div></div>");
        return;
    }
    if (r->status != 200 && !loading) {
        sb_append(sb, "<div class=\"error svelte-1enb567\">Failed to load leaderboard data. Please try again.</div></div>");
        return;
    }
    const AccountInfo *a = acc();
    const int pages = (int) v10_num(r->json, "totalPages", 0);
    sb_append(sb, "<div class=\"leaderboard-content svelte-1enb567\"><div class=\"table-wrapper svelte-1enb567\"><table class=\"header-table svelte-1enb567\"><thead><tr>"
                  "<th class=\"rank-cell svelte-1enb567\">#</th><th class=\"name-cell svelte-1enb567\">Name</th><th class=\"rank-cell svelte-1enb567\">Rank</th>"
                  "<th class=\"rank-cell svelte-1enb567\">Rating</th><th class=\"stat-cell svelte-1enb567\">Matches</th></tr></thead></table> ");
    sb_append(sb, loading ? "<div class=\"loading-overlay svelte-1enb567\"><div class=\"loading-spinner svelte-1enb567\"></div> <span>Refreshing data...</span></div>" : "<!---->");
    sb_append(sb, " <div class=\"table-container svelte-1enb567\"><table class=\"data-table svelte-1enb567\"><tbody>");
    const cJSON *row;
    cJSON_ArrayForEach(row, rows) {
        const cJSON *p = cJSON_GetObjectItem(row, "profile");
        const int rank = (int) v10_num(p, "rank", 0);
        const char *name = v10_str(p, "player_name")[0] ? v10_str(p, "player_name") : v10_str(row, "player_name");
        const int mine = a && !strcmp(name, a->name);
        sb_appendf(sb, "<tr class=\"%ssvelte-1enb567\"><td class=\"rank-cell svelte-1enb567\">%.0f</td><td class=\"name-cell svelte-1enb567\">"
                       "<button type=\"button\" class=\"player-name-link\" onclick=\"openPlayerProfile('",
                   mine ? "highlight-row " : rank == RANK_KRACKED ? "kracked-row " : "", v10_num(row, "position", 0));
        sb_append_esc(sb, name);
        sb_append(sb, "')\">");
        sb_append_esc(sb, name);
        sb_appendf(sb, "</button></td><td class=\"rank-cell svelte-1enb567\"><div class=\"rank-display svelte-1enb567\"><img class=\"rank-icon %ssvelte-1enb567\" src=\"./%s\" alt=\"%s\"> "
                       "<span class=\"rank-text svelte-1enb567\" style=\"color: %s\">%s</span></div></td><td class=\"rank-cell svelte-1enb567\">%d</td><td class=\"stat-cell svelte-1enb567\">%.0f</td></tr>",
                   rank == RANK_KRACKED ? "kracked-icon " : "", ranked_rank_icon(rank), ranked_rank_name(rank), ranked_rank_color(rank), ranked_rank_name(rank),
                   ranked_display(v10_num(p, "conservative_rating", 0)), v10_num(p, "total_matches", 0));
    }
    sb_append(sb, "</tbody></table></div></div> <div class=\"pagination svelte-1enb567\">");
    if (pages > 0)
        sb_appendf(sb, "<button class=\"page-button svelte-1enb567\"%s onclick=\"knRkBoardPage(-1)\">Previous</button> <span class=\"page-indicator svelte-1enb567\">%d / %d</span> "
                       "<button class=\"page-button svelte-1enb567\"%s onclick=\"knRkBoardPage(1)\">Next</button>",
                   R.board_page == 1 || loading ? " disabled" : "", R.board_page, pages, R.board_page >= pages || loading ? " disabled" : "");
    else sb_append(sb, "<span class=\"no-data-message svelte-1enb567\">Nothing to see</span>");
    sb_append(sb, "</div></div></div>");
}

// a history entry's sort value
static double entry_num(const cJSON *e, const char *key) {
    if (!strcmp(key, "kd")) {
        const double k = v10_num(e, "kills", 0), d = v10_num(e, "deaths", 0);
        return d > 0 ? k / d : k;
    }
    return v10_num(e, key, 0);
}

static const char *g_sort_key;
static int g_sort_desc;
static int cmp_entries(const void *a, const void *b) {
    const cJSON *x = *(const cJSON *const *) a, *y = *(const cJSON *const *) b;
    int c;
    if (!strcmp(g_sort_key, "player_name")) c = strcmp(v10_str(x, "player_name"), v10_str(y, "player_name"));
    else {
        const double dx = entry_num(x, g_sort_key), dy = entry_num(y, g_sort_key);
        c = dx < dy ? -1 : dx > dy ? 1 : 0;
    }
    return g_sort_desc ? -c : c;
}

// _36203 match history (svelte-1irqx9a): 5 per page, an entry expands into the scoreboard
static void gen_history(StrBuf *sb) {
    const V10Req *r = get_history();
    const cJSON *mh = cJSON_GetObjectItem(r->json, "matchHistory");
    const cJSON *rows = cJSON_GetObjectItem(mh, "matches");
    const int loading = r->status == 0;
    const AccountInfo *a = acc();
    const char *me = a ? a->name : "";
    sb_append(sb, "<div class=\"match-history svelte-1irqx9a\">");
    if (loading && !cJSON_GetArraySize(rows)) {
        sb_append(sb, "<div class=\"loading svelte-1irqx9a\">Loading match history...</div></div>");
        return;
    }
    if (r->status != 200 && !loading) {
        sb_append(sb, "<div class=\"error svelte-1irqx9a\">Failed to load match history. Please try again.</div></div>");
        return;
    }
    if (!cJSON_GetArraySize(rows)) {
        sb_append(sb, "<div class=\"no-matches svelte-1irqx9a\">No match history available.</div></div>");
        return;
    }
    sb_append(sb, "<div class=\"match-list svelte-1irqx9a\">");
    int index = 0;
    const cJSON *m;
    cJSON_ArrayForEach(m, rows) {
        const cJSON *entries = cJSON_GetObjectItem(m, "historyEntries");
        const cJSON *mine = NULL, *e;
        cJSON_ArrayForEach(e, entries) {
            if (!strcmp(v10_str(e, "player_name"), me)) mine = e;
        }
        // KrunkNative rows carry the player's own stats at the top level too
        if (!mine) mine = m;
        const int rank = (int) v10_num(mine, "rank", 0);
        const int team = (int) v10_num(mine, "team", 1);
        const double sa = v10_num(m, "score_alpha", 0), sbv = v10_num(m, "score_bravo", 0);
        int win;
        if (cJSON_GetObjectItem(m, "score_alpha")) win = (team == 1 && sa > sbv) || (team == 2 && sbv > sa);
        else win = v10_num(m, "victory", 0) > 0;
        char score[32];
        if (team == 2) snprintf(score, sizeof(score), "%.0f - %.0f", sbv, sa);
        else snprintf(score, sizeof(score), "%.0f - %.0f", sa, sbv);
        char date[16] = "";
        const time_t t = (time_t) (v10_num(m, "date", 0) / 1000.0);
        struct tm tm;
#ifdef _WIN32
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        strftime(date, sizeof(date), "%d/%m/%Y", &tm);
        const int change = (int) v10_num(mine, "mmr_change", 0);
        sb_appendf(sb, "<div class=\"match-entry %s svelte-1irqx9a\" onclick=\"playTick(0.5);knRkMatch(%d)\"><div class=\"match-summary svelte-1irqx9a\">"
                       "<img alt=\"map thumbnail\" class=\"map-thumbnail svelte-1irqx9a\" src=\"./img/maps/map_%d.png\"> <div class=\"match-info svelte-1irqx9a\">"
                       "<div class=\"rank-display svelte-1irqx9a\"><img class=\"rank-icon svelte-1irqx9a\" src=\"./%s\" alt=\"%d\"> <span class=\"rank svelte-1irqx9a\">%s</span> "
                       "<span class=\"match-date svelte-1irqx9a\">%s</span> <!----></div> <div class=\"stats svelte-1irqx9a\"><span class=\"stat svelte-1irqx9a\">KDA: %.0f/%.0f/%.0f</span> "
                       "<span class=\"stat svelte-1irqx9a\">Score: %.0f</span> ",
                   win ? "win" : "loss", index, (int) v10_num(m, "map", 0), ranked_rank_icon(rank), rank, ranked_rank_name(rank), date, v10_num(mine, "kills", 0),
                   v10_num(mine, "deaths", 0), v10_num(mine, "assists", 0), v10_num(mine, "score", 0));
        if (change) sb_appendf(sb, "<span class=\"stat %s svelte-1irqx9a\">%s%d Rating</span>", change > 0 ? "positive" : "negative", change > 0 ? "+" : "", change);
        else sb_append(sb, "<!---->");
        sb_appendf(sb, "</div></div> <div class=\"match-result svelte-1irqx9a\"><div class=\"result svelte-1irqx9a\">%s</div> <div class=\"score svelte-1irqx9a\">%s</div></div></div> ",
                   win ? "WIN" : "LOSS", score);
        if (index < MATCHES_MAX && R.match_open[index] && cJSON_GetArraySize(entries)) {
            static const char *const keys[] = {"player_name", "score", "kills", "assists", "deaths", "kd", "objective_score", "damage_done"};
            static const char *const labels[] = {"Player", "Score", "Kills", "Assists", "Deaths", "K/D", "Objective", "Damage"};
            const char *key = R.match_sort[index][0] ? R.match_sort[index] : "score";
            const int desc = R.match_sort[index][0] ? R.match_desc[index] : 0;
            sb_append(sb, "<div class=\"match-details svelte-1irqx9a\" onclick=\"\"><table class=\"table-container svelte-1irqx9a\"><thead><tr class=\"table-row svelte-1irqx9a\">");
            for (int c = 0; c < 8; c++)
                sb_appendf(sb, "<td class=\"table-header sortable svelte-1irqx9a\" onclick=\"playTick(0.5);knRkSort(%d,%d)\">%s %s</td>", index, c, labels[c],
                           !strcmp(key, keys[c]) ? (desc ? "\xe2\x86\x93" : "\xe2\x86\x91") : "");
            sb_append(sb, "</tr></thead><tbody>");
            const int n = cJSON_GetArraySize(entries);
            const cJSON **sorted = calloc((size_t) n, sizeof(*sorted));
            int k = 0;
            cJSON_ArrayForEach(e, entries) if (sorted) sorted[k++] = e;
            g_sort_key = key;
            g_sort_desc = desc;
            if (sorted) qsort(sorted, (size_t) k, sizeof(*sorted), cmp_entries);
            for (int j = 0; sorted && j < k; j++) {
                const cJSON *x = sorted[j];
                const int xr = (int) v10_num(x, "rank", 0);
                const double kills = v10_num(x, "kills", 0), deaths = v10_num(x, "deaths", 0);
                sb_appendf(sb, "<tr class=\"table-row %ssvelte-1irqx9a\"><td class=\"table-cell svelte-1irqx9a\"><div class=\"player-info svelte-1irqx9a\">"
                               "<img class=\"player-rank-icon svelte-1irqx9a\" src=\"./%s\" alt=\"%d\"> <span class=\"%s svelte-1irqx9a\">",
                           !strcmp(v10_str(x, "player_name"), me) ? "current-player " : "", ranked_rank_icon(xr), xr, (int) v10_num(x, "team", 1) == 2 ? "blue-team" : "red-team");
                sb_append_esc(sb, v10_str(x, "player_name"));
                sb_appendf(sb, "</span></div></td><td class=\"table-cell svelte-1irqx9a\">%.0f</td><td class=\"table-cell svelte-1irqx9a\">%.0f</td><td class=\"table-cell svelte-1irqx9a\">%.0f</td>"
                               "<td class=\"table-cell svelte-1irqx9a\">%.0f</td><td class=\"table-cell svelte-1irqx9a\">%.2f</td><td class=\"table-cell svelte-1irqx9a\">%.0f</td>"
                               "<td class=\"table-cell svelte-1irqx9a\">%.0f</td></tr>",
                           v10_num(x, "score", 0), kills, v10_num(x, "assists", 0), deaths, deaths > 0 ? kills / deaths : kills, v10_num(x, "objective_score", 0),
                           v10_num(x, "damage_done", 0));
            }
            free(sorted);
            sb_append(sb, "</tbody></table></div>");
        } else sb_append(sb, "<!---->");
        sb_append(sb, "</div>");
        index++;
    }
    const int pages = (int) v10_num(mh, "totalPages", 1);
    sb_append(sb, "</div> ");
    if (pages > 1)
        sb_appendf(sb, "<div class=\"pagination svelte-1irqx9a\"><button class=\"page-btn svelte-1irqx9a\"%s onclick=\"knRkHistoryPage(-1)\">&lt; Previous</button> "
                       "<span class=\"page-info svelte-1irqx9a\">Page %d of %d</span> <button class=\"page-btn svelte-1irqx9a\"%s onclick=\"knRkHistoryPage(1)\">Next &gt;</button></div>",
                   R.history_page == 1 || loading ? " disabled" : "", R.history_page, pages, R.history_page >= pages || loading ? " disabled" : "");
    else sb_append(sb, "<!---->");
    sb_append(sb, "</div>");
}

// _37915 shop (svelte-1gtpoav) with _37839 items (svelte-1q06uv7)
static void gen_shop(StrBuf *sb) {
    const V10Req *r = get_shop();
    const V10Req *pr = get_profile(current_season());
    sb_append(sb, "<div class=\"shop-container svelte-1gtpoav\"><div class=\"shop-header svelte-1gtpoav\"><div class=\"header-top svelte-1gtpoav\">"
                  "<div class=\"header-subtitle svelte-1gtpoav\">Spend your Ranked Points on exclusive items.</div> <div class=\"rp-balance svelte-1gtpoav\">");
    v10_currency(sb, v10_num(pr->json, "rankedPoints", 0), "RP", "large", 0);
    sb_append(sb, "</div></div></div> <div class=\"shop-grid svelte-1gtpoav\">");
    const cJSON *s;
    cJSON_ArrayForEach(s, cJSON_GetObjectItem(r->json, "skins")) {
        const int skin = (int) v10_num(s, "skin_id", -1), id = (int) v10_num(s, "id", 0);
        if (!viewer_skin(skin)) continue; // .filter(item)
        const int max = (int) v10_num(s, "max_purchase", 0), bought = (int) v10_num(s, "purchase_count", 0);
        const int sold = max > 0 && bought >= max, restricted = mmr_restricted(s);
        char req[64], click[48];
        mmr_requirement(s, req, sizeof(req));
        StrBuf c = {0};
        sb_append(&c, sold ? "<div class=\"sold-out-overlay svelte-1q06uv7\"><div class=\"sold-out-text svelte-1q06uv7\">SOLD OUT</div></div>" : "<!---->");
        sb_append(&c, " <div class=\"bottom-overlay svelte-1q06uv7\">");
        if (has_num(s, "expires_at") && v10_num(s, "expires_at", 0) > 0) {
            sb_append(&c, "<div class=\"expiration-row svelte-1q06uv7\">");
            countdown(&c, v10_num(s, "expires_at", 0), "1q06uv7");
            sb_append(&c, "</div>");
        } else sb_append(&c, "<!---->");
        sb_append(&c, " <div class=\"price-buy-row svelte-1q06uv7\"><div class=\"price-tag svelte-1q06uv7\">");
        v10_currency(&c, v10_num(s, "price", 0), "RP", "small", 0);
        sb_append(&c, " ");
        if (max > 0) sb_appendf(&c, "<div class=\"purchase-count svelte-1q06uv7\">%d/%d</div>", bought, max);
        else sb_append(&c, "<!---->");
        sb_append(&c, "</div> <div class=\"buy-button-wrapper svelte-1q06uv7\">");
        if (restricted && req[0]) sb_appendf(&c, "<div class=\"mmr-requirement svelte-1q06uv7\">%s</div>", req);
        else sb_append(&c, "<!---->");
        sb_appendf(&c, " <button class=\"buy-button svelte-1q06uv7\"%s onclick=\"knRkBuy(%d)\">", R.buying || sold || restricted ? " disabled" : "", id);
        if (R.buying && g_buy.id == id) sb_append(&c, "<span class=\"material-icons svelte-1q06uv7\">hourglass_empty</span> <span class=\"svelte-1q06uv7\">Buying...</span>");
        else sb_append(&c, "<span class=\"material-icons svelte-1q06uv7\">shopping_cart</span> <span class=\"svelte-1q06uv7\">Buy</span>");
        sb_append(&c, "</button></div></div></div>");
        char *children = sb_finish(&c);
        snprintf(click, sizeof(click), "knRkItem(%d)", id);
        sb_append(sb, "<div class=\"shop-item-wrapper svelte-1q06uv7\">");
        v10_market_card(sb, skin, 240, 240, "top-center", click, children);
        sb_append(sb, "</div>");
        free(children);
    }
    sb_append(sb, "</div></div>");
}

// The overlay's markup: kn-ranked-intro while it mounts, kn-ranked-outro while it unmounts (css/kn-v10-popups.css).
static void gen_overlay(StrBuf *sb) {
    if (!R.overlay.visible && !R.overlay.outro) {
        sb_append(sb, "<!---->");
        return;
    }
    const char *cls = R.overlay.intro ? " kn-ranked-intro" : !R.overlay.visible ? " kn-ranked-outro" : "";
    sb_appendf(sb, "<div class=\"overlay-container svelte-jxc66d%s\"><div class=\"overlay-content svelte-jxc66d\">", cls);
    if (R.overlay.icon[0]) {
        sb_append(sb, "<div class=\"emoji-icon svelte-jxc66d\">");
        emoji(sb, R.overlay.icon, "overlay-emoji");
        sb_append(sb, "</div>");
    } else sb_append(sb, "<!---->");
    sb_append(sb, " <h2 class=\"overlay-title svelte-jxc66d\">");
    sb_append_esc(sb, R.overlay.title);
    sb_append(sb, "</h2> <p class=\"overlay-description svelte-jxc66d\">");
    sb_append_esc(sb, R.overlay.text);
    sb_appendf(sb, "</p> <button class=\"overlay-button svelte-jxc66d\" style=\"background-color: %s;\" onclick=\"playTick(0.5);playSelect(0.1);knRkOverlay()\">", R.overlay.color);
    sb_append_esc(sb, R.overlay.button);
    sb_append(sb, "</button></div></div>");
}

static void gen_footer(StrBuf *sb) {
    const char *text, *icon, *color;
    status_line(&text, &icon, &color);
    sb_appendf(sb, "<div class=\"footer %ssvelte-mqcul7\"><div class=\"matchmaking-info svelte-mqcul7\"><span class=\"status-icon svelte-mqcul7\" "
                   "style=\"display:inline-block;width:26px;height:26px;line-height:26px;font-size:26px;margin-right:10px;\">",
               R.state == MM_SEARCHING ? "matchmaking-active " : "");
    emoji(sb, icon, "status-emoji");
    sb_appendf(sb, "</span> <span class=\"status-text svelte-mqcul7\" style=\"color: %s;\">", color);
    sb_append_esc(sb, text);
    sb_append(sb, "</span></div> <div class=\"footer-controls svelte-mqcul7\">");
    if (R.state == MM_IDLE)
        sb_appendf(sb, "<div class=\"queue-all-regions-container svelte-mqcul7\"><input type=\"checkbox\" name=\"queueAllRegions\" id=\"queueAllRegions\" class=\"svelte-mqcul7\"%s "
                       "onchange=\"knRkAllRegions(this.checked)\"> <label for=\"queueAllRegions\" class=\"svelte-mqcul7\" onclick=\"knRkAllRegions()\">All regions</label></div>",
                   R.all_regions ? " checked" : "");
    else sb_append(sb, "<!---->");
    const char *label = R.busy ? "LOADING..." : R.status_lock ? "REJOIN" : R.state == MM_SEARCHING ? "CANCEL" : R.state == MM_FOUND ? "MATCH FOUND" : "FIND MATCH";
    sb_appendf(sb, " <button class=\"start-button %s %ssvelte-mqcul7\"%s onclick=\"knRkStart()\">%s</button></div></div>", R.state == MM_SEARCHING ? "cancel-button" : "",
               R.status_lock ? "rejoin-button " : "", R.busy || R.state == MM_FOUND ? " disabled" : "", label);
}

// the popup (_37933)
static char *gen_popup(void) {
    StrBuf sb = {0};
    sb_append(&sb, "<div class=\"outer-container svelte-mqcul7\"><div class=\"container svelte-mqcul7\"><div class=\"menu-container svelte-mqcul7\">");
    for (int i = 0; i < TAB_COUNT; i++)
        sb_appendf(&sb, "<div class=\"menu-item %s svelte-mqcul7\" onclick=\"playTick(0.5);knRkTab(%d)\">%s</div>", R.tab == i ? "active" : "", i, k_tabs[i]);
    sb_appendf(&sb, " <div class=\"region-indicator svelte-mqcul7\">Region: %s</div></div> <div class=\"content svelte-mqcul7\"><div class=\"tab-content svelte-mqcul7\">",
               ranked_region_name(my_region()));
    switch (R.tab) {
        case TAB_PROFILE: gen_profile(&sb); break;
        case TAB_LEADERBOARD: gen_leaderboard(&sb); break;
        case TAB_HISTORY: gen_history(&sb); break;
        case TAB_SHOP: gen_shop(&sb); break;
        default: gen_home(&sb); break;
    }
    sb_append(&sb, "</div></div></div> ");
    gen_footer(&sb);
    sb_append(&sb, "</div> ");
    gen_overlay(&sb);
    return sb_finish(&sb);
}

static void render(void) {
    if (!v10_popup_open(POPUP_CLASS)) return;
    char *html = gen_popup();
    v10_popup_set(POPUP_CLASS, html);
    free(html);
}

static void open_popup(void) {
    static int once;
    if (!once) {
        once = 1;
        char v[8] = "";
        R.all_regions = html_storage_get("s_rankedAllRegions", v, sizeof(v)) >= 0 && (v[0] == '1' || v[0] == 't');
    }
    R.overlay.visible = 0;
    const AccountInfo *a = acc();
    if (!a && !R.scenario)
        show_overlay("Login Required", "You must be logged in to access Ranked Mode.", "GOT IT", "#5ce05a", "\xf0\x9f\x94\x92", 1);
    else if (a && a->level < RANKED_MIN_LEVEL)
        show_overlay("Level Required", "You must be level 10 or higher to access Ranked Mode.", "OK", "#ff3939", "\xf0\x9f\x94\x92", 1);
    R.tab = TAB_HOME; // the component starts on Home every time it mounts
    R.board_page = R.history_page = 1;
    memset(R.match_open, 0, sizeof(R.match_open));
    if (R.state == MM_IDLE && !R.status_lock) fetch_connection();
    char *html = gen_popup();
    v10_popup_show(POPUP_CLASS, html, render);
    free(html);
}

// windows[26].gen() returns '' in v10 (the Ranked UI is the popup)
char *ranked_ui_window_gen(void) { return strdup(""); }

// --render-scenario: state.ranked {open, tab, profile, info, board, history, shop, status,
// searching, expanded, overlay}: the brain's answers for this popup
void ranked_ui_scenario(const cJSON *state) {
    if (!cJSON_IsObject(state)) return;
    R.scenario = 1;
    static const struct { const char *key, *path; } k_paths[] = {
        {"profile", "/mm/ranked/profile"}, {"board", "/mm/ranked/leaderboard"}, {"history", "/mm/ranked/history"}, {"shop", "/mm/ranked/shop"},
    };
    for (size_t i = 0; i < sizeof(k_paths) / sizeof(k_paths[0]); i++) {
        const cJSON *v = cJSON_GetObjectItem(state, k_paths[i].key);
        if (cJSON_IsObject(v)) v10_scenario_answer(k_paths[i].path, v);
    }
    R.state = cJSON_IsTrue(cJSON_GetObjectItem(state, "searching")) ? MM_SEARCHING : MM_IDLE;
    if (R.state == MM_SEARCHING) {
        R.queued_at = (double) time(NULL) - v10_num(state, "searchingFor", 0);
        char t[16], text[64];
        fmt_time(t, sizeof(t), (int) v10_num(state, "searchingFor", 0));
        snprintf(text, sizeof(text), "Looking for match... (%s)", t);
        set_status(text, "\xf0\x9f\x94\x8d", "#5ce05a");
    }
    R.expanded = cJSON_IsTrue(cJSON_GetObjectItem(state, "expanded"));
    if (cJSON_IsFalse(cJSON_GetObjectItem(state, "open"))) return;
    open_popup();
    R.tab = (int) v10_num(state, "tab", 0);
    if (R.tab < 0 || R.tab >= TAB_COUNT) R.tab = 0;
    const cJSON *open = cJSON_GetObjectItem(state, "matchOpen");
    if (cJSON_IsNumber(open) && open->valueint >= 0 && open->valueint < MATCHES_MAX) R.match_open[open->valueint] = 1;
    const cJSON *ov = cJSON_GetObjectItem(state, "overlay");
    if (cJSON_IsObject(ov))
        show_overlay(v10_str(ov, "title"), v10_str(ov, "description"), v10_str(ov, "button")[0] ? v10_str(ov, "button") : "OK",
                     v10_str(ov, "color")[0] ? v10_str(ov, "color") : NULL, v10_str(ov, "icon"), 0);
    update_banner();
    render();
}
