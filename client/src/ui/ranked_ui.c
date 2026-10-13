// Krunker v10.0.2 ranked (doc/v10/ranked-impl.md): the Ranked window and the queue session.
//
// v10's Ranked is a Svelte popup (rankedMenuModal) opened by #menuBtnRanked, tabs Home / Profile /
// Leaderboard / Match History / Shop; window 27 is an empty legacy slot. KrunkNative keeps its C
// window on slot 27 (showWindow(27)) and lays it out like v10's modal: the status banner ("Ready to
// compete? Join the ranked queue!"), FIND MATCH / CANCEL with "Searching for match... (m:ss)",
// the All Regions checkbox and region indicator, the rank card (rank icon, name, rating, win rate,
// K/D, "N games remaining until ranked", the decay warning), the Ranked Progression bar with its
// milestones ("CLAIM +N RP"), the season's map rotation, the leaderboard / match history tables with
// a season select, the RP shop. The popups for the queue errors use v10's titles and texts.
//
// Transport (deviation, doc/v10/ranked-impl.md): v10 opens a WebSocket to its matchmaker; this
// client long-polls the brain's /mm/ranked/* routes (same {type, payload} messages), so the queue
// works through the HTTPS edge relays and in the web build. A MATCHED assignment is joined at once
// after 1 s (v10: pop_3, wait 1 s, acceptMatch) with its ticket.
#include <backend.h>
#include <backend_ui.h>
#include <ui_logic.h>
#include <html_ui.h>
#include <ranked.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define POLL_WAIT_S 15          // long poll (the brain holds the request up to 20 s)
#define DATA_TTL_MS 60000       // v10's request cache: 60 s
#define ACCEPT_DELAY_MS 1000    // MATCHED: wait 1 s, then acceptMatch()

enum { TAB_HOME, TAB_PROFILE, TAB_LEADERBOARD, TAB_HISTORY, TAB_SHOP, TAB_COUNT };
static const char *const k_tabs[TAB_COUNT] = {"Home", "Profile", "Leaderboard", "Match History", "Shop"};

enum { MM_IDLE, MM_SEARCHING, MM_FOUND };

static struct {
    int tab;
    int state;                    // currentMatchmakingState
    char key[128];                // the queue's clientKey ("" = not queued)
    double queued_at;             // time() of QUEUED
    int poll_inflight;
    cJSON *assignment;            // MATCHED assignment (owned)
    int in_match;                 // joined a ranked match
    char status[128];             // the banner text ("" = the default)
    char status_color[16];
    cJSON *info;                  // /mm/ranked/info
    cJSON *profile;               // /mm/ranked/profile (owned)
    cJSON *board;                 // /mm/ranked/leaderboard
    cJSON *history;               // /mm/ranked/history
    cJSON *shop;                  // /mm/ranked/shop
    char rejoin[96];              // /mm/ranked/connection serverUrl ("" none)
    cJSON *rejoin_as;
    int fresh[5];                 // per data set: loaded within DATA_TTL_MS
    int inflight[5];
    int season;                   // the season the tables show (-1 = current)
    int page;                     // leaderboard / history page (0-based)
    int all_regions;              // s_rankedAllRegions
    char popup[512];              // the error popup ("" = none)
    int tick_timer;
    int scenario;
} R = {.season = -1};

enum { D_INFO, D_PROFILE, D_BOARD, D_HISTORY, D_SHOP };

static const AccountInfo *acc(void) { return account_get(); }
static void enc(char *out, size_t n, const char *s) { backend_url_encode(out, n, s ? s : ""); }
static void rerender(void) { menu_update_instructions(); menu_rerender_if_open(27); }

static int num(const cJSON *o, const char *k, const int def) {
    const cJSON *v = cJSON_GetObjectItem(o, k);
    return cJSON_IsNumber(v) ? v->valueint : def;
}
static double dnum(const cJSON *o, const char *k, const double def) {
    const cJSON *v = cJSON_GetObjectItem(o, k);
    return cJSON_IsNumber(v) ? v->valuedouble : def;
}
static const char *str(const cJSON *o, const char *k) {
    const char *s = cJSON_GetStringValue(cJSON_GetObjectItem(o, k));
    return s ? s : "";
}

static int current_season(void) { return num(R.info, "season", ranked_season_at((int64_t) time(NULL))); }
static int shown_season(void) { return R.season >= 0 ? R.season : current_season(); }
static int min_level(void) { return num(R.info, "minLevel", RANKED_MIN_LEVEL); }

// v10 regionAcronym: the default region folds oc -> as, sa -> na; KrunkNative's game servers are
// all North American, so na unless the player picked another (krunker_rankedRegion)
static const char *my_region(void) {
    static char region[8];
    if (html_storage_get("krunker_rankedRegion", region, sizeof(region)) < 0 || ranked_region_index(region) < 0)
        snprintf(region, sizeof(region), "na");
    return region;
}

static void set_status(const char *text, const char *color) {
    snprintf(R.status, sizeof(R.status), "%s", text ? text : "");
    snprintf(R.status_color, sizeof(R.status_color), "%s", color ? color : "#5ce05a");
}

// ------------------------------------------------------------------ data fetches

static void invalidate(void) {
    for (int i = 0; i < 5; i++) R.fresh[i] = 0;
}

static void expire(void *user) {
    const int which = (int) (intptr_t) user;
    if (which >= 0 && which < 5) R.fresh[which] = 0;
}

static cJSON **slot_of(const int which) {
    switch (which) {
        case D_INFO: return &R.info;
        case D_PROFILE: return &R.profile;
        case D_BOARD: return &R.board;
        case D_HISTORY: return &R.history;
        default: return &R.shop;
    }
}

static void on_data(void *user, const int status, const char *body, const size_t length) {
    (void) length;
    const int which = (int) (intptr_t) user;
    R.inflight[which] = 0;
    cJSON *res = status == 200 ? cJSON_Parse(body) : NULL;
    if (!cJSON_IsObject(res)) {
        cJSON_Delete(res);
        return;
    }
    cJSON **slot = slot_of(which);
    cJSON_Delete(*slot);
    *slot = res;
    R.fresh[which] = 1;
    html_timer_add(DATA_TTL_MS, expire, (void *) (intptr_t) which);
    menu_rerender_if_open(27);
}

static void fetch(const int which) {
    if (R.scenario || R.inflight[which] || R.fresh[which] || backend_available() != 1) return;
    if (which != D_INFO && which != D_BOARD && !acc()) return;
    char token[256], path[512];
    enc(token, sizeof(token), account_session_token());
    const int season = shown_season();
    switch (which) {
        case D_INFO: snprintf(path, sizeof(path), "/mm/ranked/info"); break;
        case D_PROFILE: snprintf(path, sizeof(path), "/mm/ranked/profile?token=%s&season=%d&region=%s", token, season, my_region()); break;
        case D_BOARD: snprintf(path, sizeof(path), "/mm/ranked/leaderboard?season=%d&region=%s&limit=10&offset=%d", season, my_region(), R.page); break;
        case D_HISTORY: snprintf(path, sizeof(path), "/mm/ranked/history?token=%s&season=%d&region=%s&limit=10&offset=%d", token, season, my_region(), R.page); break;
        default: snprintf(path, sizeof(path), "/mm/ranked/shop?token=%s", token); break;
    }
    R.inflight[which] = 1;
    if (backend_http_get(path, on_data, (void *) (intptr_t) which) != 0) R.inflight[which] = 0;
}

static void on_connection(void *user, const int status, const char *body, const size_t length) {
    (void) user;
    (void) length;
    cJSON *res = status == 200 ? cJSON_Parse(body) : NULL;
    const char *url = cJSON_GetStringValue(cJSON_GetObjectItem(res, "serverUrl"));
    snprintf(R.rejoin, sizeof(R.rejoin), "%s", url ? url : "");
    cJSON_Delete(R.rejoin_as);
    R.rejoin_as = url ? cJSON_DetachItemFromObject(res, "assignment") : NULL;
    cJSON_Delete(res);
    menu_rerender_if_open(27);
}

static void fetch_connection(void) { // fetchIsPlayerInRankedMatch
    if (R.scenario || !acc() || backend_available() != 1) return;
    char token[256], path[400];
    enc(token, sizeof(token), account_session_token());
    snprintf(path, sizeof(path), "/mm/ranked/connection?token=%s", token);
    backend_http_get(path, on_connection, NULL);
}
// ------------------------------------------------------------------ queue session (MatchmakingService)

static void poll_fire(void *user);

static void clear_state(void) { // clearState
    R.state = MM_IDLE;
    R.key[0] = 0;
    R.queued_at = 0;
    cJSON_Delete(R.assignment);
    R.assignment = NULL;
    set_status("", NULL);
}

static void show_popup(const char *title, const char *text) {
    snprintf(R.popup, sizeof(R.popup), "%s\n%s", title, text);
    menu_rerender_if_open(27);
}

static void join_assignment(const cJSON *as) {
    const char *host = str(as, "host"), *game = str(as, "gameId"), *ticket = str(as, "clientId");
    const char *web = cJSON_GetStringValue(cJSON_GetObjectItem(as, "webUrl"));
    const int port = num(as, "port", 0);
    if (!host[0] || port <= 0) return;
    printf("[ranked] joining %s (%s)\n", game, str(as, "connection"));
    R.in_match = 1;
    invalidate();
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
    rerender();
}

// handleWebSocketMessage({type, payload})
static void handle_message(const cJSON *msg) {
    const char *type = str(msg, "type");
    const cJSON *p = cJSON_GetObjectItem(msg, "payload");
    if (!strcmp(type, "ERROR")) {
        const char *code = str(p, "code");
        const cJSON *inner = cJSON_GetObjectItem(p, "payload");
        clear_state();
        if (!strcmp(code, "MIN_LEVEL_REQUIREMENT")) show_popup("Level Required", "You must be level 10 or higher to access Ranked Mode.");
        else if (!strcmp(code, "LOGIN_REQUIRED")) show_popup("Login Required", "You must be logged in to access Ranked Mode.");
        else if (!strcmp(code, "NO_RANK_DATA")) show_popup("Rank Data Missing", "We could not retrieve your ranked data. Please try again later.");
        else if (!strcmp(code, "COOLDOWN")) {
            const int s = (int) (dnum(inner, "cooldown", 0) / 1000.0);
            char text[160];
            snprintf(text, sizeof(text), "You are on cooldown and cannot queue for ranked matches. Cooldown will end in %d:%02d", s / 60, s % 60);
            show_popup("Cooldown", text);
        } else if (!strcmp(code, "ALREADY_IN_MATCH")) {
            snprintf(R.rejoin, sizeof(R.rejoin), "%s", str(inner, "serverUrl"));
            cJSON_Delete(R.rejoin_as);
            const cJSON *as = cJSON_GetObjectItem(inner, "assignment");
            R.rejoin_as = as ? cJSON_Duplicate(as, 1) : NULL;
            show_popup("Ranked", "You are already in a Ranked game.");
        } else {
            show_popup("Error", str(p, "message")[0] ? str(p, "message") : "Failed to connect to matchmaking server");
        }
        rerender();
        return;
    }
    if (strcmp(type, "QUEUE_STATUS")) return;
    const char *status = str(p, "status");
    if (!strcmp(status, "QUEUED")) {
        const char *key = cJSON_GetStringValue(cJSON_GetObjectItem(p, "clientKey"));
        if (key) snprintf(R.key, sizeof(R.key), "%s", key);
        if (R.state != MM_SEARCHING) {
            R.state = MM_SEARCHING;
            R.queued_at = (double) time(NULL);
        }
    } else if (!strcmp(status, "MATCHED")) {
        R.state = MM_FOUND;
        cJSON_Delete(R.assignment);
        R.assignment = cJSON_Duplicate(cJSON_GetObjectItem(p, "assignment"), 1);
        set_status("Match found!", "#5ce05a");
        html_timer_add(ACCEPT_DELAY_MS, accept_fire, NULL);
    }
    rerender();
}

static void on_poll(void *user, const int status, const char *body, const size_t length) {
    (void) user;
    (void) length;
    R.poll_inflight = 0;
    if (!R.key[0] || R.state != MM_SEARCHING) return;
    if (status == 404) { // the brain forgot the key (it stopped polling for too long): search again
        clear_state();
        set_status("Failed to connect to matchmaking server", "#ff3939");
        rerender();
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
    enc(key, sizeof(key), R.key);
    snprintf(path, sizeof(path), "/mm/ranked/poll?clientKey=%s&wait=%d", key, POLL_WAIT_S);
    R.poll_inflight = 1;
    if (backend_http_get(path, on_poll, NULL) != 0) R.poll_inflight = 0;
}

static void on_queue(void *user, const int status, const char *body, const size_t length) {
    (void) user;
    (void) length;
    cJSON *msg = status == 200 ? cJSON_Parse(body) : NULL;
    if (cJSON_IsObject(msg)) handle_message(msg);
    else {
        clear_state();
        show_popup("Error", "Failed to connect to matchmaking server");
    }
    cJSON_Delete(msg);
    if (R.state == MM_SEARCHING) poll_fire(NULL);
}

static void start_matchmaking(void) { // startMatchmaking(allRegions)
    if (R.state != MM_IDLE || !acc()) return;
    char token[256], path[512];
    enc(token, sizeof(token), account_session_token());
    snprintf(path, sizeof(path), "/mm/ranked/queue?token=%s&regions=%s", token, R.all_regions ? "as,eu,na" : my_region());
    R.state = MM_SEARCHING;
    R.queued_at = (double) time(NULL);
    set_status("Looking for match...", "#5ce05a");
    backend_http_get(path, on_queue, NULL);
    rerender();
}

static void cancel_matchmaking(void) { // cancelMatchmaking: v10 closes the socket
    if (R.key[0]) {
        char key[256], path[400];
        enc(key, sizeof(key), R.key);
        snprintf(path, sizeof(path), "/mm/ranked/leave?clientKey=%s", key);
        backend_http_get(path, NULL, NULL);
    }
    clear_state();
    rerender();
}

// the banner seconds tick while searching (setupStatusUpdateInterval, 1 s)
static void status_tick(void *user) {
    (void) user;
    R.tick_timer = 0;
    if (R.state != MM_SEARCHING) return;
    const int s = (int) ((double) time(NULL) - R.queued_at);
    char text[64];
    snprintf(text, sizeof(text), "Searching for match... (%d:%02d)", s / 60, s % 60);
    set_status(text, "#5ce05a");
    rerender();
    R.tick_timer = html_timer_add(1000, status_tick, NULL);
}

const char *ranked_ui_status(void) { return R.state != MM_IDLE && R.status[0] ? R.status : NULL; }
const char *ranked_ui_ready(void) { return NULL; } // v10: solo queue, no party ready count
int ranked_ui_in_ranked(void) { return R.state != MM_IDLE || R.in_match; }
void ranked_ui_left_match(void) {
    if (R.in_match) invalidate(); // the profile changed
    R.in_match = 0;
}
void ranked_ui_class_changed(const int index) { (void) index; } // v10: the class is picked in the match's comp lobby

static void on_claim(void *user, const int status, const char *body, const size_t length) {
    (void) user;
    (void) length;
    cJSON *res = cJSON_Parse(body);
    if (status != 200) show_popup("Ranked", str(res, "error")[0] ? str(res, "error") : "Failed to claim milestone");
    cJSON_Delete(res);
    R.fresh[D_PROFILE] = 0;
    fetch(D_PROFILE);
}

static void on_purchase(void *user, const int status, const char *body, const size_t length) {
    (void) user;
    (void) length;
    cJSON *res = cJSON_Parse(body);
    show_popup("Shop", status == 200 ? "Purchased! The skin is in your inventory." : str(res, "error")[0] ? str(res, "error") : "Purchase failed");
    cJSON_Delete(res);
    R.fresh[D_SHOP] = R.fresh[D_PROFILE] = 0;
}

int ranked_ui_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    if (!name) return -1;
    const int arg0 = argc > 0 && args[0].kind == HTML_ARG_NUM ? (int) args[0].num : 0;
    if (!strcmp(name, "rankedTab")) {
        if (arg0 >= 0 && arg0 < TAB_COUNT && arg0 != R.tab) {
            R.tab = arg0;
            R.page = 0;
            menu_rerender_if_open(27);
        }
        return 1;
    }
    if (!strcmp(name, "closeRankedPopup")) {
        if (R.popup[0]) R.popup[0] = 0;
        else menu_show_window(MENU_WINDOW_NONE, 0);
        menu_rerender_if_open(27);
        return 1;
    }
    if (!strcmp(name, "rankedSeason")) { // the season select: arg = this.value (the season)
        char v[16] = "";
        if (argc > 0 && args[0].kind == HTML_ARG_THIS_VALUE) {
            if (ev && ev->value && ev->value[0]) snprintf(v, sizeof(v), "%s", ev->value);
            else if (ev && ev->target) html_dom_get_value(ev->target, v, sizeof(v));
        } else if (argc > 0 && args[0].kind == HTML_ARG_STR) {
            snprintf(v, sizeof(v), "%s", args[0].str);
        } else {
            snprintf(v, sizeof(v), "%d", arg0);
        }
        R.season = v[0] ? atoi(v) : -1;
        R.page = 0;
        R.fresh[D_PROFILE] = R.fresh[D_BOARD] = R.fresh[D_HISTORY] = 0;
        menu_rerender_if_open(27);
        return 1;
    }
    if (!strcmp(name, "rankedPage")) {
        R.page += arg0;
        if (R.page < 0) R.page = 0;
        R.fresh[D_BOARD] = R.fresh[D_HISTORY] = 0;
        menu_rerender_if_open(27);
        return 1;
    }
    if (!strcmp(name, "rankedRegion")) { // region: "as" / "eu" / "na"
        if (argc > 0 && args[0].kind == HTML_ARG_STR && ranked_region_index(args[0].str) >= 0) {
            html_storage_set("krunker_rankedRegion", args[0].str);
            invalidate();
            menu_rerender_if_open(27);
        }
        return 1;
    }
    if (!strcmp(name, "rankedAllRegions")) {
        R.all_regions = !R.all_regions;
        html_storage_set("s_rankedAllRegions", R.all_regions ? "1" : "0");
        menu_rerender_if_open(27);
        return 1;
    }
    if (!backend_enabled() || !acc()) return -1;
    if (!strcmp(name, "rankedQueue")) { // the start button: FIND MATCH / CANCEL / REJOIN
        if (R.rejoin[0] && R.rejoin_as) {
            join_assignment(R.rejoin_as); // quickJoinRankedMatch
            R.rejoin[0] = 0;
        } else if (R.state == MM_IDLE) {
            start_matchmaking();
            if (!R.tick_timer) R.tick_timer = html_timer_add(1000, status_tick, NULL);
        } else if (R.state == MM_SEARCHING) {
            cancel_matchmaking();
        }
        return 1;
    }
    if (!strcmp(name, "rankedClaim")) { // onClaimMilestone
        char token[256], path[400];
        enc(token, sizeof(token), account_session_token());
        snprintf(path, sizeof(path), "/mm/ranked/claim-milestone?token=%s&region=%d", token, ranked_region_index(my_region()));
        backend_http_get(path, on_claim, NULL);
        return 1;
    }
    if (!strcmp(name, "rankedBuy")) {
        char token[256], path[400];
        enc(token, sizeof(token), account_session_token());
        snprintf(path, sizeof(path), "/mm/ranked/purchase?token=%s&id=%d", token, arg0);
        backend_http_get(path, on_purchase, NULL);
        return 1;
    }
    return -1;
}
// ------------------------------------------------------------------ windows[26].gen() in v10's layout

static void rank_icon(StrBuf *sb, const int rank, const char *cls) {
    sb_appendf(sb, "<img class='%s' src='./%s' alt='Rank'>", cls, ranked_rank_icon(rank));
}

static void gen_tabs(StrBuf *sb) {
    sb_append(sb, "<div class='bigMenTabs rkTabs'>");
    for (int i = 0; i < TAB_COUNT; i++)
        sb_appendf(sb, "<div class='bigMenTab%s' onclick='playSelect(0.1);rankedTab(%d)' onmouseenter='playTick()'>%s</div>",
                   R.tab == i ? " bigMenTabA" : "", i, k_tabs[i]);
    sb_append(sb, "</div>");
}

static void gen_season_select(StrBuf *sb) {
    const int cur = current_season(), shown = shown_season();
    sb_append(sb, "<div class='rkControls'><select class='inputGrey rkSeason' onchange='rankedSeason(this.value)'>");
    for (int s = cur; s >= 0 && s > cur - 12; s--) {
        if (s == 0) sb_appendf(sb, "<option value='0'%s>Beta Season</option>", shown == 0 ? " selected" : "");
        else sb_appendf(sb, "<option value='%d'%s>Season %d</option>", s, shown == s ? " selected" : "", s);
    }
    sb_append(sb, "</select><span class='rkRegionTabs'>");
    for (int i = 2; i <= 4; i++) {
        const char *code = ranked_region_code(i);
        sb_appendf(sb, "<span class='rkRegionTab%s' onclick='playSelect(0.1);rankedRegion(\"%s\")'>%s</span>", !strcmp(code, my_region()) ? " active" : "",
                   code, ranked_region_name(code));
    }
    sb_append(sb, "</span></div>");
}

// the rank card (.rank-card: season banner, icon, rank name, rating, quick stats)
static void gen_rank_card(StrBuf *sb, const cJSON *p) {
    const int rank = num(p, "rank", 0), matches = num(p, "total_matches", 0);
    const int wins = num(p, "wins", 0), kills = num(p, "total_kills", 0), deaths = num(p, "total_deaths", 0);
    const double cr = dnum(p, "conservative_rating", 0);
    const int season = shown_season();
    sb_append(sb, "<div class='rkCard'><div class='rkSeasonBanner'>");
    if (season == 0) sb_append(sb, "Beta Season");
    else sb_appendf(sb, "Season %d", season);
    sb_append(sb, "</div>");
    rank_icon(sb, rank, "rkRankIcon");
    sb_appendf(sb, "<div class='rkRankName' style='color:%s'>%s</div>", ranked_rank_color(rank), ranked_rank_name(rank));
    if (rank == RANK_UNRANKED)
        sb_appendf(sb, "<div class='rkUnranked'>%d games remaining until ranked</div>", num(p, "placement_left", RANKED_PLACEMENT_MATCHES));
    else
        sb_appendf(sb, "<div class='rkRating'>%d <span>Rating</span></div>", ranked_display(cr));
    sb_appendf(sb, "<div class='rkQuick'><div><span>%.1f%%</span>Win Rate</div><div><span>%.2f</span>K/D</div><div><span>%d</span>Matches</div></div>",
               matches ? 100.0 * wins / matches : 0.0, (double) kills / (deaths > 0 ? deaths : 1), matches);
    if (rank >= RANK_DIAMOND) sb_append(sb, "<div class='rkDecay'>Your rating will decay after 7+ days of inactivity</div>");
    sb_append(sb, "</div>");
}

// Ranked Progression: matches toward 100, milestones claimable in order
static void gen_progression(StrBuf *sb, const cJSON *profile) {
    const cJSON *p = cJSON_GetObjectItem(profile, "profile");
    const cJSON *ms = cJSON_GetObjectItem(profile, "milestones");
    const int matches = num(p, "total_matches", 0), last = num(p, "last_milestone_claimed", -1);
    const int beta = shown_season() == 0;
    const int est = (int) ranked_season_points(dnum(p, "conservative_rating", 0), matches, shown_season());
    sb_appendf(sb, "<div class='rkProg'><div class='rkProgHead'>Ranked Progression<span><b>%d</b> matches</span></div>"
                   "<div class='rkProgBar'><div style='width:%d%%'></div>", matches, matches >= RANKED_MAX_MATCHES ? 100 : matches);
    int i = 0;
    const cJSON *m;
    cJSON_ArrayForEach(m, ms) {
        const int at = num(m, "matches", 0), reward = num(m, "reward", 0);
        sb_appendf(sb, "<div class='rkMilestone' style='left:%d%%'><div class='rkMsNum'>%d</div>", at, at);
        if (i <= last) sb_append(sb, "<div class='rkMsBonus claimed'>CLAIMED</div>");
        else if (matches >= at && i == last + 1)
            sb_appendf(sb, "<div class='rkMsBonus claimable' onclick='playSelect();rankedClaim()'>CLAIM +%d RP</div>", reward);
        else sb_appendf(sb, "<div class='rkMsBonus'>+%d RP</div>", reward);
        sb_append(sb, "</div>");
        i++;
    }
    sb_appendf(sb, "</div><div class='rkProgFoot'><span class='rkEst'>%d</span> estimated</div><div class='rkHow'>", est);
    if (beta) sb_append(sb, "Beta Season: 1:1 conversion rate (100%) &middot; No milestones during Beta Season");
    else sb_append(sb, "100 matches = 100% conversion rate &middot; Milestones give rewards when reached &middot; "
                       "Points awarded for your best region at end of month");
    sb_append(sb, "</div></div>");
}

static void gen_maps(StrBuf *sb) {
    sb_append(sb, "<div class='rkMapsHed'>Map Rotation</div><div class='rkMaps'>");
    int maps[RANKED_SEASON_MAPS];
    ranked_season_maps(current_season(), maps);
    for (int i = 0; i < RANKED_SEASON_MAPS; i++) {
        const char *name = ranked_pool_map_name(maps[i]);
        const int local = maps[i] < 32; // KrunkNative's catalogue (and its map_N.png) ends at 31
        sb_appendf(sb, "<div class='rkMap%s'>", local ? "" : " missing");
        if (local) sb_appendf(sb, "<img src='./img/maps/map_%d.png'>", maps[i]);
        sb_append(sb, "<div class='rkMapName'>");
        sb_append_esc(sb, name ? name : "?");
        sb_append(sb, "</div></div>");
    }
    sb_append(sb, "</div>");
}

static void gen_home(StrBuf *sb) {
    // the status banner and the start button
    const char *text = R.status[0] ? R.status : "Ready to compete? Join the ranked queue!";
    const char *icon = R.state == MM_SEARCHING ? "&#9203;" : R.state == MM_FOUND ? "&#9989;" : "&#9876;&#65039;";
    sb_appendf(sb, "<div class='rkBanner' style='color:%s'><span class='rkBannerIcon'>%s</span>", R.status_color[0] ? R.status_color : "#5ce05a", icon);
    sb_append_esc(sb, text);
    sb_append(sb, "</div><div class='rkStartRow'>");
    const char *label = R.rejoin[0] ? "REJOIN" : R.state == MM_SEARCHING ? "CANCEL" : R.state == MM_FOUND ? "MATCH FOUND" : "FIND MATCH";
    sb_appendf(sb, "<div class='joinBtn rkStart%s%s' onclick='playSelect();rankedQueue()' onmouseenter='playTick()'>%s</div>",
               R.state == MM_SEARCHING ? " cancel" : "", R.rejoin[0] ? " rejoin" : "", label);
    sb_appendf(sb, "<label class='rkAll' onclick='rankedAllRegions()'><span class='rkCheck%s'></span>All regions</label>", R.all_regions ? " on" : "");
    sb_appendf(sb, "<span class='rkRegion'>Region: %s</span></div>", R.all_regions ? "All" : ranked_region_name(my_region()));
    fetch(D_PROFILE);
    const cJSON *p = cJSON_GetObjectItem(R.profile, "profile");
    if (!p) {
        sb_append(sb, "<div class='rkLoading'>Loading profile data...</div>");
    } else {
        sb_append(sb, "<div class='rkHomeGrid'>");
        gen_rank_card(sb, p);
        gen_progression(sb, R.profile);
        sb_append(sb, "</div>");
    }
    gen_maps(sb);
    sb_appendf(sb, "<div class='rkFoot'>Abandoning a match will result in a temporary ban &amp; loss in rating &middot; %d Ranked Points</div>",
               num(R.profile, "rankedPoints", 0));
}

static void gen_profile(StrBuf *sb) {
    gen_season_select(sb);
    fetch(D_PROFILE);
    const cJSON *p = cJSON_GetObjectItem(R.profile, "profile");
    if (!p) {
        sb_append(sb, "<div class='rkLoading'>Loading profile data...</div>");
        return;
    }
    if (num(p, "total_matches", 0) == 0) {
        sb_append(sb, "<div class='rkEmpty'>No ranked data for this season.</div>");
        return;
    }
    gen_rank_card(sb, p);
    const int matches = num(p, "total_matches", 1) ? num(p, "total_matches", 1) : 1;
    const double minutes = dnum(p, "total_time_played", 0) / 60000.0;
    const struct {
        const char *label;
        double value;
        const char *fmt;
    } rows[] = {
        {"Peak Rating", dnum(p, "peak_mmr", 0), "%.0f"},       {"Wins", dnum(p, "wins", 0), "%.0f"},
        {"Losses", dnum(p, "losses", 0), "%.0f"},              {"Abandons", dnum(p, "abandons", 0), "%.0f"},
        {"Kills", dnum(p, "total_kills", 0), "%.0f"},          {"Deaths", dnum(p, "total_deaths", 0), "%.0f"},
        {"Average Kills", dnum(p, "total_kills", 0) / matches, "%.1f"},
        {"Average Score", dnum(p, "total_score", 0) / matches, "%.0f"},
        {"Damage per Minute", minutes > 0 ? dnum(p, "total_damage_done", 0) / minutes : 0, "%.0f"},
        {"Highest Kills", dnum(p, "highest_kill_count", 0), "%.0f"},
        {"Highest Damage", dnum(p, "highest_damage_in_match", 0), "%.0f"},
    };
    sb_append(sb, "<div class='setBodH rkStats'>");
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        char v[32];
        snprintf(v, sizeof(v), rows[i].fmt, rows[i].value);
        sb_appendf(sb, "<div class='settName'>%s<span class='rkVal'>%s</span></div>", rows[i].label, v);
    }
    sb_append(sb, "</div>");
}

static void gen_pager(StrBuf *sb, const int pages) {
    sb_appendf(sb, "<div class='rkPager'><span class='menuLink%s' onclick='rankedPage(-1)'>Prev</span> %d / %d "
                   "<span class='menuLink%s' onclick='rankedPage(1)'>Next</span></div>",
               R.page > 0 ? "" : " disabled", R.page + 1, pages > 0 ? pages : 1, R.page + 1 < pages ? "" : " disabled");
}

static void gen_leaderboard(StrBuf *sb) {
    gen_season_select(sb);
    fetch(D_BOARD);
    const cJSON *rows = cJSON_GetObjectItem(R.board, "leaderboard");
    if (!R.board) {
        sb_append(sb, "<div class='rkLoading'>Loading leaderboard data...</div>");
        return;
    }
    if (!cJSON_GetArraySize(rows)) {
        sb_append(sb, "<div class='rkEmpty'>No data available for the selected filters.</div>");
        return;
    }
    sb_append(sb, "<table class='rkTable'><tr><th>#</th><th>Name</th><th>Rank</th><th>Rating</th><th>Matches</th></tr>");
    const cJSON *r;
    cJSON_ArrayForEach(r, rows) {
        const cJSON *p = cJSON_GetObjectItem(r, "profile");
        const int rank = num(p, "rank", 0);
        sb_appendf(sb, "<tr><td>%d</td><td><a class='menuLink' onclick='openPlayerProfile(\"", num(r, "position", 0));
        sb_append_esc(sb, str(r, "player_name"));
        sb_append(sb, "\")'>");
        sb_append_esc(sb, str(r, "player_name"));
        if (str(r, "clan")[0]) {
            sb_append(sb, " <span class='rkClan'>[");
            sb_append_esc(sb, str(r, "clan"));
            sb_append(sb, "]</span>");
        }
        sb_append(sb, "</a></td><td>");
        rank_icon(sb, rank, "rkRowIcon");
        sb_appendf(sb, " %s</td><td>%d</td><td>%d</td></tr>", ranked_rank_name(rank), num(p, "mmr", 1000), num(p, "total_matches", 0));
    }
    sb_append(sb, "</table>");
    gen_pager(sb, num(R.board, "totalPages", 1));
}

static void gen_history(StrBuf *sb) {
    gen_season_select(sb);
    fetch(D_HISTORY);
    const cJSON *mh = cJSON_GetObjectItem(R.history, "matchHistory");
    const cJSON *rows = cJSON_GetObjectItem(mh, "matches");
    if (!R.history) {
        sb_append(sb, "<div class='rkLoading'>Loading match history...</div>");
        return;
    }
    if (!cJSON_GetArraySize(rows)) {
        sb_append(sb, "<div class='rkEmpty'>No match history available.</div>");
        return;
    }
    sb_append(sb, "<table class='rkTable'><tr><th>Result</th><th>Map</th><th>K / D / A</th><th>Score</th><th>Damage</th><th>Rating</th></tr>");
    const cJSON *m;
    cJSON_ArrayForEach(m, rows) {
        const cJSON *v = cJSON_GetObjectItem(m, "victory");
        const int abandoned = cJSON_IsTrue(cJSON_GetObjectItem(m, "abandoned"));
        const char *res = abandoned ? "ABANDON" : !cJSON_IsNumber(v) ? "DRAW" : v->valueint ? "WIN" : "LOSS";
        const int change = num(m, "mmr_change", 0);
        sb_appendf(sb, "<tr><td class='rk%s'>%s</td><td>", res, res);
        sb_append_esc(sb, str(m, "map_name"));
        sb_appendf(sb, "</td><td>%d / %d / %d</td><td>%d</td><td>%d</td><td class='%s'>%+d Rating</td></tr>", num(m, "kills", 0), num(m, "deaths", 0),
                   num(m, "assists", 0), num(m, "score", 0), num(m, "damage_done", 0), change > 0 ? "positive" : "negative", change);
    }
    sb_append(sb, "</table>");
    gen_pager(sb, num(mh, "totalPages", 1));
}

static void gen_shop(StrBuf *sb) {
    fetch(D_SHOP);
    fetch(D_PROFILE);
    sb_appendf(sb, "<div class='rkShopHead'>Ranked Points <span class='rkRp'>%d RP</span></div>", num(R.profile, "rankedPoints", 0));
    const cJSON *skins = cJSON_GetObjectItem(R.shop, "skins");
    if (!R.shop) {
        sb_append(sb, "<div class='rkLoading'>Loading...</div>");
        return;
    }
    if (!cJSON_GetArraySize(skins)) {
        sb_append(sb, "<div class='rkEmpty'>Nothing in the Ranked shop right now.</div>");
        return;
    }
    sb_append(sb, "<div class='rkShop'>");
    const cJSON *s;
    cJSON_ArrayForEach(s, skins) {
        const int bought = num(s, "purchase_count", 0), max = num(s, "max_purchase", 1);
        sb_appendf(sb, "<div class='rkItem'><div class='rkItemName'>Skin #%d</div><div class='rkItemPrice'>%d RP</div>", num(s, "skin_id", 0),
                   num(s, "price", 0));
        if (cJSON_IsNumber(cJSON_GetObjectItem(s, "min_mmr"))) sb_appendf(sb, "<div class='rkReq'>Requires MMR: %d</div>", num(s, "min_mmr", 0));
        if (bought >= max) sb_append(sb, "<div class='rkBuy owned'>Owned</div>");
        else sb_appendf(sb, "<div class='rkBuy' onclick='playSelect();rankedBuy(%d)'>Buy</div>", num(s, "id", 0));
        sb_append(sb, "</div>");
    }
    sb_append(sb, "</div>");
}

static void gen_popup(StrBuf *sb) {
    const char *nl = strchr(R.popup, '\n');
    sb_append(sb, "<div class='rkPopup'><div class='rkPopupTitle'>");
    if (nl) sb_append_n(sb, R.popup, (size_t) (nl - R.popup));
    sb_append(sb, "</div><div class='rkPopupText'>");
    sb_append_esc(sb, nl ? nl + 1 : R.popup);
    sb_append(sb, "</div><div class='joinBtn rkPopupOk' onclick='closeRankedPopup()'>OK</div></div>");
}

// windows[26] (showWindow(27)): the LOGIN REQUIRED card for guests, the LEVEL card below level 10
char *ranked_ui_window_gen(void) {
    static int css;
    if (!css) css = html_ui_add_stylesheet("css/ranked-kn.css") || 1;
    static int once;
    if (!once) {
        once = 1;
        char v[8] = "";
        R.all_regions = html_storage_get("s_rankedAllRegions", v, sizeof(v)) >= 0 && v[0] == '1';
    }
    const AccountInfo *a = acc();
    if (!a || (backend_available() != 1 && !R.scenario))
        return strdup("<div class='reqW login' onclick='showWindow(5)'><i class='material-icons reqWIcon'>clear</i><div class='reqWLvl'>LOGIN REQUIRED</div>"
                      "<div class='reqWTxt'>You must be logged in to access Ranked Mode.</div></div>");
    fetch(D_INFO);
    if (!R.scenario && R.state == MM_IDLE) fetch_connection();
    StrBuf sb = {0};
    if (a->level < min_level()) {
        sb_appendf(&sb, "<div class='reqW'><i class='material-icons reqWIcon'>lock</i><div class='reqWLvl'>LEVEL %d REQUIRED</div><div class='reqWTxt'>"
                        "You must be level %d or higher to access Ranked Mode. Right now you are at level %d.</div></div>",
                   min_level(), min_level(), a->level);
        return sb_finish(&sb);
    }
    sb_append(&sb, "<div class='rkWin'>");
    gen_tabs(&sb);
    switch (R.tab) {
        case TAB_PROFILE: gen_profile(&sb); break;
        case TAB_LEADERBOARD: gen_leaderboard(&sb); break;
        case TAB_HISTORY: gen_history(&sb); break;
        case TAB_SHOP: gen_shop(&sb); break;
        default: gen_home(&sb); break;
    }
    if (R.popup[0]) gen_popup(&sb);
    sb_append(&sb, "</div>");
    return sb_finish(&sb);
}

// --render-scenario: state.ranked {tab, profile, info, board, history, shop, status, searching}
void ranked_ui_scenario(const cJSON *state) {
    if (!cJSON_IsObject(state)) return;
    R.scenario = 1;
    const char *keys[5] = {"info", "profile", "board", "history", "shop"};
    for (int i = 0; i < 5; i++) {
        cJSON **slot = slot_of(i);
        cJSON_Delete(*slot);
        const cJSON *v = cJSON_GetObjectItem(state, keys[i]);
        *slot = cJSON_IsObject(v) ? cJSON_Duplicate(v, 1) : NULL;
        R.fresh[i] = 1;
    }
    R.tab = num(state, "tab", 0);
    R.state = cJSON_IsTrue(cJSON_GetObjectItem(state, "searching")) ? MM_SEARCHING : MM_IDLE;
    set_status(cJSON_GetStringValue(cJSON_GetObjectItem(state, "status")), NULL);
    R.season = num(state, "season", -1);
}
