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
// Two backends (ranked_route.h, ranked_active_backend below): a krunker.io account uses v10's own ranked
// services at krunker.io (krunker_ranked.h: api / gapi, and the queue WebSocket of v10's gamefrontend),
// their answers reshaped into the brain's shape here, so the renderers below read one shape. A
// KrunkNative account uses kn_brain's /mm/ranked/* routes. krunker.io wins when both are signed in.
//
// Transport of the KrunkNative backend (deviation, doc/v10/ranked-impl.md): v10 opens a WebSocket to
// its matchmaker; this client long-polls the brain's /mm/ranked/* routes (same {type, payload}
// messages), so the queue works through the HTTPS edge relays and in the web build. A MATCHED
// assignment is joined after 1 s (v10: pop_3, wait 1 s, acceptMatch) with its ticket. The krunker.io
// queue delivers v10's frames unchanged, and its MATCHED joins the match's game socket like v10's
// joinAssignment: JOINED_RANK on an open game socket, then the socket at the assignment's connection
// with the account's token (krunker_net_join_match). Nothing is shown for it but the loading screen.
#include <v10_ui.h>
#include <v10_menu.h> // v10_menu_handle("loginOrRegister"): guests get v10's login modal
#include <audio.h>
#include <backend.h>
#include <backend_ui.h>
#include <ui_logic.h>
#include <html_ui.h>
#include <ranked.h>
#include <ranked_route.h>   // which backend: krunker.io first, then KrunkNative
#include <krunker_account.h> // the krunker.io account (krunker_account_signed_in, its name and level)
#include <krunker_ranked.h> // v10's ranked services at krunker.io
#include <krunker_net.h>    // the ranked match's game socket (krunker_net_join_match, krunker_net_ranked_elsewhere)
#include <net_ext.h> // NET_EXT_JOINED_RANK (v10 JOINED_RANK)
#include <flavor.h> // the coordinator's flavor pool: ranked is Box Office's
#include <math.h>
#include <kio_store.h> // krunker.io's ranked shop while krunker.io is the active backend
#include <i18n.h>       // I18N("generic.copied", "Copied!"): v10's key for the share button's copied state
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "../viewer/viewer_store.h"

#define POLL_WAIT_S 6           // long poll: under the brain's request timeout (backend_internal.h BACKEND_HTTP_TIMEOUT_MS, 8 s), so an idle poll answers instead of timing out
#define DATA_TTL_MS 0           // v10 keeps no ranked answers (getOrFetch TTL 0, v10:98587, 98620, 98639): see open_popup
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
    char join_host[96];           // the host of that match (v10 isInOtherRankedMatch compares hosts)
    int join_pending;             // joined, its INIT not applied yet: "joinedRank" goes out with it
    int in_game_match;            // the joined match's game is applied (INIT) and not left: v10 _33840 + the socket
    char status[128];             // the status line ("" = the default)
    char status_icon[16];
    char status_color[16];
    int status_lock;              // "You are already in a Ranked game." (_37948): REJOIN
    int busy;                     // _37947: LOADING... (the connection check, a request in flight)
    char rejoin[96];              // /mm/ranked/connection serverUrl ("" none)
    cJSON *rejoin_as;
    int home_check;               // the home load's rejoin check is in flight: open the popup if it is in a match (v10:209847)
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

static const AccountInfo *acc(void) { return active_account(); }

// THE choice of the ranked backend: the connected server's (backend_ui.h account_backend(): krunker.io on an
// official server, KrunkNative elsewhere), never the other account. A --render-scenario render answers from
// the brain's routes (its fixtures), whoever is signed in.
static RankedBackend ranked_active_backend(void) {
    if (R.scenario) return RANKED_BACKEND_KN;
    return ranked_backend_route(account_backend() == ACCOUNT_BACKEND_KRUNKER, krunker_account_signed_in(), krunkio_account() != NULL);
}

// The active account's name and level (level -1 = not known yet: krunker.io's level comes from the game
// server; the server still refuses an unready account with MIN_LEVEL_REQUIREMENT).
static const char *active_name(void) {
    if (ranked_active_backend() == RANKED_BACKEND_KRUNKER) return krunker_account_name();
    const AccountInfo *a = acc();
    return a ? a->name : "";
}

static int active_level(void) {
    if (ranked_active_backend() == RANKED_BACKEND_KRUNKER) {
        const int level = krunker_account_level();
        return level > 0 ? level : -1;
    }
    const AccountInfo *a = acc();
    return a ? a->level : -1;
}

static int current_season(void) { return ranked_season_at((int64_t) time(NULL)); }

// v10 regionAcronym (_34037): the default region is the saved defaultRegion's node folded to its ranked
// region (shared/src/ranked.c ranked_region_of_node, v10's _7296 / _7297 table), else the region the client
// seeks in (pingRegion7, ClientManager.getRegion), else eu (v10's default). KrunkNative's own server regions
// that are not v10 nodes fold by their prefix (oc -> as, sa -> na). krunker_rankedRegion (KrunkNative) pins one.
static const char *my_region(void) {
    static char region[8];
    if (html_storage_get("krunker_rankedRegion", region, sizeof(region)) >= 0 && ranked_region_index(region) >= 0) return region;
    const cJSON *def = ui_setting_saved("defaultRegion");
    char node[96] = "";
    if (cJSON_IsString(def) && def->valuestring[0]) snprintf(node, sizeof(node), "%s", def->valuestring);
    else html_storage_get("pingRegion7", node, sizeof(node));
    int index = ranked_region_of_node(node);
    if (index < 0) {
        static const struct { const char *prefix; int index; } fold[] = {
            {"eu", 3}, {"de", 3}, {"fr", 3}, {"gb", 3}, {"uk", 3}, {"nl", 3}, {"pl", 3},
            {"as", 2}, {"sg", 2}, {"jp", 2}, {"jb", 2}, {"au", 2}, {"oc", 2}, {"in", 2}, {"kr", 2}, {"hk", 2},
            {"na", 4}, {"us", 4}, {"ca", 4}, {"sa", 4}, {"br", 4}, {"mx", 4},
        };
        for (size_t i = 0; i < sizeof(fold) / sizeof(fold[0]); i++)
            if (!strncmp(node, fold[i].prefix, strlen(fold[i].prefix))) {
                index = fold[i].index;
                break;
            }
    }
    snprintf(region, sizeof(region), "%s", ranked_region_code(index >= 0 ? index : RANKED_DEFAULT_REGION_INDEX));
    return region;
}

static void token(char *out, const size_t n) { v10_url_token(out, n); }

// ------------------------------------------------------------------ krunker.io data (krunker_ranked.h)
//
// The same cache rules as v10_get (one answer per request, DATA_TTL_MS, the stale answer stays up while
// the new one loads). Each v10 answer is reshaped by an adapter into the brain's shape, which the
// renderers read: profile {profile, milestones}, leaderboard {leaderboard, totalPages}, history
// {matchHistory}, shop {skins}, wallet {rankedPoints}, connection {serverUrl}.

#define KIO_SLOTS 12
typedef cJSON *(*KioAdapt)(const cJSON *body); // v10's {data, error} envelope -> the brain's shape (NULL: failed)

typedef struct {
    char key[96];
    int used;
    int inflight;
    unsigned gen;     // kio_forget() bumps it: an answer of an older generation is dropped
    double at_ms;     // when the answer came (0 = never)
    V10Req req;
    KioAdapt adapt;
} KioSlot;

static KioSlot g_kio[KIO_SLOTS];
static unsigned g_kio_gen = 1;

static double kio_now_ms(void) { return (double) time(NULL) * 1000.0; }

static cJSON *adapt_data(const cJSON *body) { // data as it is (an object; a 204's null data: an empty one)
    const cJSON *data = cJSON_GetObjectItem(body, "data");
    return cJSON_IsObject(data) ? cJSON_Duplicate(data, 1) : cJSON_CreateObject();
}

// v10 keeps the milestone rows of the last profile fetch (_33842, set by getPlayerProfile, v10:98579) across
// its refetches, and a claim takes its reward from them and from last_milestone_claimed (v10:98738-98744).
static cJSON *g_milestones;
static int g_last_claimed = -1;

static cJSON *adapt_profile(const cJSON *body) { // v10 playerProfile: data.profile and data.milestones (v10:98579)
    const cJSON *data = cJSON_GetObjectItem(body, "data");
    cJSON *out = cJSON_CreateObject();
    if (!out) return NULL;
    const cJSON *p = cJSON_GetObjectItem(data, "profile"), *ms = cJSON_GetObjectItem(data, "milestones");
    if (p) cJSON_AddItemToObject(out, "profile", cJSON_Duplicate(p, 1));
    if (ms) cJSON_AddItemToObject(out, "milestones", cJSON_Duplicate(ms, 1));
    if (ms) {
        cJSON_Delete(g_milestones);
        g_milestones = cJSON_Duplicate(ms, 1);
    }
    if (p) g_last_claimed = (int) v10_num(p, "last_milestone_claimed", -1);
    return out;
}

// v10's {data: {success}, error} (claimMilestone, purchaseSkin) as the brain's {success, error} the popups read
static cJSON *kio_result(const cJSON *body) {
    const cJSON *data = cJSON_GetObjectItem(body, "data");
    const cJSON *err = cJSON_GetObjectItem(data, "error");
    if (!err) err = cJSON_GetObjectItem(body, "error");
    cJSON *out = cJSON_CreateObject();
    if (!out) return NULL;
    cJSON_AddBoolToObject(out, "success", cJSON_IsTrue(cJSON_GetObjectItem(data, "success")));
    cJSON_AddStringToObject(out, "error", cJSON_IsString(err) ? err->valuestring : "");
    return out;
}

static void on_kio(void *user, const int status, const cJSON *body) {
    KioSlot *s = &g_kio[(size_t) (uintptr_t) user];
    s->inflight = 0;
    if (s->gen != g_kio_gen) { // forgotten meanwhile: the popup asks again
        cJSON_Delete(s->req.json);
        s->req.json = NULL;
        s->used = 0;
        v10_refresh();
        return;
    }
    cJSON_Delete(s->req.json);
    s->req.json = status == 200 && s->adapt ? s->adapt(body) : NULL;
    s->req.status = s->req.json ? 200 : status ? status : -1;
    s->at_ms = kio_now_ms();
    v10_refresh();
}

// The cached krunker.io answer of `key` (one per request), requested when it is due. Like v10_get.
static const V10Req *kio_get(const char *key, const KrunkerRankedHost host, const char *path, const KioAdapt adapt, const int ttl_ms) {
    const double now = kio_now_ms();
    int index = -1, free_slot = -1, oldest = -1;
    for (int i = 0; i < KIO_SLOTS; i++) {
        if (!g_kio[i].used) {
            if (free_slot < 0) free_slot = i;
            continue;
        }
        if (!strcmp(g_kio[i].key, key)) index = i;
        else if (!g_kio[i].inflight && (oldest < 0 || g_kio[i].at_ms < g_kio[oldest].at_ms)) oldest = i;
    }
    if (index >= 0) {
        KioSlot *s = &g_kio[index];
        if (s->inflight || !s->at_ms || ttl_ms <= 0 || now - s->at_ms < ttl_ms) return &s->req;
    } else {
        index = free_slot >= 0 ? free_slot : oldest;
        if (index < 0) {
            static const V10Req busy = {-1, NULL};
            return &busy;
        }
        KioSlot *s = &g_kio[index];
        cJSON_Delete(s->req.json);
        memset(s, 0, sizeof(*s));
        snprintf(s->key, sizeof(s->key), "%s", key);
        s->used = 1;
    }
    KioSlot *s = &g_kio[index];
    s->adapt = adapt;
    s->gen = g_kio_gen;
    s->inflight = 1;
    if (!krunker_ranked_request(host, "GET", path, NULL, on_kio, (void *) (uintptr_t) index)) { // not sent: no krunker.io session
        s->inflight = 0;
        s->req.status = -1;
        s->at_ms = now;
    }
    return &s->req;
}

static void kio_forget(void) {
    g_kio_gen++;
    if (!g_kio_gen) g_kio_gen = 1;
    for (int i = 0; i < KIO_SLOTS; i++) {
        if (!g_kio[i].used || g_kio[i].inflight) continue;
        cJSON_Delete(g_kio[i].req.json);
        memset(&g_kio[i], 0, sizeof(g_kio[i]));
    }
}

// ------------------------------------------------------------------ data (v10_get, 60 s cache)

static const V10Req *get_profile(const int season) {
    if (ranked_active_backend() == RANKED_BACKEND_KRUNKER) { // getPlayerProfile(season, regionIndex) (v10:25953)
        char key[96], path[128];
        snprintf(key, sizeof(key), "profile/%d/%s", season, my_region());
        snprintf(path, sizeof(path), "/player-profile/me/season/%d/region/%d", season, ranked_region_index(my_region()));
        return kio_get(key, KRUNKER_RANKED_API, path, adapt_profile, DATA_TTL_MS);
    }
    char t[256], path[512];
    token(t, sizeof(t));
    snprintf(path, sizeof(path), "/mm/ranked/profile?token=%s&season=%d&region=%s", t, season, my_region());
    return v10_get(path, DATA_TTL_MS);
}

// the brain's level gate (brain.ranked.min_level, the profile's minLevel; v10:111566 hard-codes 10 until it is known)
static int g_min_level = RANKED_MIN_LEVEL;

static const cJSON *profile_of(const V10Req *r) {
    if (r->status != 200) return NULL;
    const cJSON *min = cJSON_GetObjectItem(r->json, "minLevel");
    if (cJSON_IsNumber(min) && min->valueint > 0) g_min_level = min->valueint;
    return cJSON_GetObjectItem(r->json, "profile");
}

static const V10Req *get_board(void) {
    if (ranked_active_backend() == RANKED_BACKEND_KRUNKER) { // getLeaderboard(50, page, season, regionIndex) (v10:25961)
        char key[96], path[256];
        snprintf(key, sizeof(key), "board/%d/%s/%d", current_season(), my_region(), R.board_page);
        snprintf(path, sizeof(path), "/leaderboards/rankings/season/%d/region/%d?limit=50&offset=%d", current_season(),
                 ranked_region_index(my_region()), R.board_page - 1);
        return kio_get(key, KRUNKER_RANKED_API, path, adapt_data, DATA_TTL_MS);
    }
    char path[256];
    snprintf(path, sizeof(path), "/mm/ranked/leaderboard?season=%d&region=%s&limit=50&offset=%d", current_season(), my_region(), R.board_page - 1);
    return v10_get(path, DATA_TTL_MS);
}

static const V10Req *get_history(void) {
    if (ranked_active_backend() == RANKED_BACKEND_KRUNKER) {
        // getMatchHistory(limit, page, season, region) (v10:25937): v10 sends the unset gameMode as "undefined"
        char key[96], path[320];
        snprintf(key, sizeof(key), "history/%d/%s/%d", current_season(), my_region(), R.history_page);
        snprintf(path, sizeof(path), "/match-history/me/season/%d/gameMode/undefined/isRanked/true/region/%d?limit=%d&offset=%d", current_season(),
                 ranked_region_index(my_region()), HISTORY_PAGE, R.history_page - 1);
        return kio_get(key, KRUNKER_RANKED_API, path, adapt_data, DATA_TTL_MS);
    }
    char t[256], path[512];
    token(t, sizeof(t));
    snprintf(path, sizeof(path), "/mm/ranked/history?token=%s&season=%d&region=%s&limit=%d&offset=%d", t, current_season(), my_region(), HISTORY_PAGE,
             R.history_page - 1);
    return v10_get(path, DATA_TTL_MS);
}

static const V10Req *get_shop(void) {
    if (kio_store_active()) return kio_store_ranked_shop(); // krunker.io: ranked.getShop() with the account's token (v10:26380, _37760)
    if (ranked_active_backend() == RANKED_BACKEND_KRUNKER) return kio_get("shop", KRUNKER_RANKED_GAPI, "/ranked/shop", adapt_data, DATA_TTL_MS);
    char t[256], path[400];
    token(t, sizeof(t));
    snprintf(path, sizeof(path), "/mm/ranked/shop?token=%s", t);
    return v10_get(path, DATA_TTL_MS);
}

// The RP balance. A krunker.io account's is its account row's rankedPoints (v10's Account.rankedPoints,
// row[47], v10:112873): the game server sends the row ("a" / "ua") when a krunker.io game signs the account
// in. Until then v10 shows 0: the balance is `rankedPoints || 0` (v10:111254, the shop's $rankedPointsBalance
// v10:112354), and v10 never asks krunker.io for it. KrunkNative's is the brain's profile.
static int rp_balance(double *out) {
    if (ranked_active_backend() == RANKED_BACKEND_KRUNKER) {
        *out = 0; // v10's display until the account row arrives (krunker_account_ranked_points leaves it)
        krunker_account_ranked_points(out);
        return 1;
    }
    *out = v10_num(get_profile(current_season())->json, "rankedPoints", 0);
    return 1;
}

static void invalidate(void) {
    v10_forget("/mm/ranked/");
    kio_forget();
}

// The Leaderboards window's Rating tab for a krunker.io account (v10 getLeaderboard(40, 0, season, region),
// v10:139145): the krunker.io rankings of that season and region, 40 from the top. NULL = not a krunker.io
// account (the caller asks the brain). region 0 = Global (no region in the path).
const V10Req *ranked_ui_rating_board(const int season, const int region) {
    if (ranked_active_backend() != RANKED_BACKEND_KRUNKER) return NULL;
    char key[96], path[200];
    snprintf(key, sizeof(key), "rating/%d/%d", season, region);
    if (region) snprintf(path, sizeof(path), "/leaderboards/rankings/season/%d/region/%d?limit=40&offset=0", season, region);
    else snprintf(path, sizeof(path), "/leaderboards/rankings/season/%d?limit=40&offset=0", season);
    return kio_get(key, KRUNKER_RANKED_API, path, adapt_data, DATA_TTL_MS);
}

static void render(void);
static void open_popup(void);

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
// v10's $rankedMatchMakingStatus.message while its error is shown (v10:98476-98490, 98444-98452): the banner
// shows it, and the component opens the popup for error.code (v10:111606-111646). "" = none.
static char g_banner_error[160];

static void update_banner(void);

static void banner_error(const char *message) {
    snprintf(g_banner_error, sizeof(g_banner_error), "%s", message && message[0] ? message : "Unknown error");
    update_banner();
}

static void update_banner(void) {
    const HtmlElem holder = html_dom_by_id("matchmakingStatusEl");
    if (!holder) return;
    char text[160] = "";
    if (g_banner_error[0]) {
        snprintf(text, sizeof(text), "%s", g_banner_error);
    } else if (R.state == MM_SEARCHING) {
        char t[16];
        fmt_time(t, sizeof(t), (int) ((double) time(NULL) - R.queued_at));
        snprintf(text, sizeof(text), "Searching for match... (%s)", t);
    } else if (R.state == MM_FOUND) {
        snprintf(text, sizeof(text), "Match found!");
    }
    StrBuf sb = {0};
    sb_appendf(&sb, "<div class=\"ranked-matchmaking-status svelte-3k0h7w%s\">", R.state != MM_IDLE || g_banner_error[0] ? " show" : "");
    sb_append_esc(&sb, text);
    sb_append(&sb, "</div>");
    char *html = sb_finish(&sb);
    html_dom_set_html(holder, html);
    free(html);
}

// ------------------------------------------------------------------ queue session (MatchmakingService)

static void poll_fire(void *user);
static void status_tick(void *user);

static int g_kio_queue;          // the queue is krunker.io's socket (krunker_ranked_queue_start), not the brain's

static void clear_state(void) { // clearState (v10 closes its socket here)
    if (g_kio_queue) {
        g_kio_queue = 0;
        krunker_ranked_queue_stop();
    }
    g_banner_error[0] = 0; // (v10 clearState: message '', show false, v10:98416)
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
    invalidate();
    v10_popup_close();      // quickJoinRankedMatch / acceptMatch: clearPops()
    menu_show_window(MENU_WINDOW_NONE, 0);
    servers_ui_join_ticket(host, port, game, ticket, web);
    // after the call: joining leaves the previous game (client_leave_game -> ranked_ui_left_match), which must not undo this
    R.in_match = 1;
    R.join_pending = 1;
    snprintf(R.join_host, sizeof(R.join_host), "%s", host);
}

// v10's joinAssignment (v10:98678) / quickJoinRankedMatch (v10:98729) for a krunker.io match: the match's
// game socket at connection (krunker_net_join_match: JOINED_RANK first when a game socket is open). The
// popups close as v10's clearPops does. No connection, nothing to join (v10 answers success false).
static void join_kio_match(const char *connection, const char *game) {
    if (!connection || !connection[0]) return;
    printf("[ranked] joining %s (%s)\n", game ? game : "", connection);
    R.in_match = 1;
    invalidate();
    v10_popup_close();
    menu_show_window(MENU_WINDOW_NONE, 0);
    if (!krunker_net_join_match(connection, game)) printf("[ranked] the match could not be joined: no krunker.io game client\n");
}

static void accept_fire(void *user) { // acceptMatch: "Connecting to match..."
    (void) user;
    if (R.state != MM_FOUND || !R.assignment) return;
    cJSON *as = R.assignment;
    R.assignment = NULL;
    const int kio = g_kio_queue; // (clear_state ends the krunker.io queue)
    clear_state();
    if (kio) {
        join_kio_match(v10_str(as, "connection"), v10_str(as, "gameId"));
        cJSON_Delete(as);
        render();
        return;
    }
    join_assignment(as);
    cJSON_Delete(as);
    render();
}

// A queue frame that does not parse (v10's catch, v10:98489-98497): the state clears, the banner says so and
// the popup is the internal error (INTERNAL_ERROR)
static void queue_unparseable(void) {
    clear_state();
    R.busy = 0;
    banner_error("Failed to parse server message");
    internal_error();
    render();
}

// The queue could not be reached: v10's socket onerror (v10:98441-98452) sets the banner to "Failed to connect to
// matchmaking server" with error INTERNAL_ERROR, so the banner keeps that text and the Internal Error popup opens.
static void queue_unreachable(void) {
    clear_state();
    R.busy = 0;
    banner_error("Failed to connect to matchmaking server");
    internal_error();
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
        banner_error(v10_str(p, "message")); // v10: message = payload.message || "Unknown error", show true
        R.busy = 0;
        if (!strcmp(code, "MIN_LEVEL_REQUIREMENT")) {
            char text[96];
            snprintf(text, sizeof(text), "You must be level %d or higher to access Ranked Mode.", g_min_level);
            show_overlay("Level Required", text, "OK", "#ff3939", "\xf0\x9f\x94\x92", 0);
        } else if (!strcmp(code, "LOGIN_REQUIRED"))
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
        if (R.state != MM_SEARCHING) { // v10 handleQueueStatus QUEUED (v10:98459): Searching and its start time, only now
            R.state = MM_SEARCHING;
            R.queued_at = (double) time(NULL);
            set_status("Looking for match... (0:00)", "\xf0\x9f\x94\x8d", "#5ce05a");
            if (!R.tick_timer) R.tick_timer = html_timer_add(1000, status_tick, NULL);
        }
        R.busy = 0;
    } else if (!strcmp(status, "MATCHED")) {
        const cJSON *as = cJSON_GetObjectItem(p, "assignment");
        if (g_kio_queue && krunker_net_ranked_elsewhere(v10_str(as, "connection"))) {
            printf("[ranked] match found while in another ranked game: ignored (v10 isInOtherRankedMatch)\n");
            clear_state();
            render();
            return;
        }
        // v10:98509 isInOtherRankedMatch: already in a ranked game on another host: the match is dropped, silently
        if (R.in_game_match && strcmp(v10_str(as, "host"), R.join_host)) {
            clear_state();
            R.busy = 0;
            update_banner();
            render();
            return;
        }
        printf("[ranked] match found\n");
        R.state = MM_FOUND;
        R.busy = 0;
        cJSON_Delete(R.assignment);
        R.assignment = cJSON_Duplicate(as, 1);
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
        queue_unreachable();
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
    else if (status == 200) queue_unparseable(); // a brain frame that is not an object: v10's parse error
    else queue_unreachable(); // the queue could not be reached (v10's socket onerror, v10:98441-98452)
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

// ------------------------------------------------------------------ krunker.io queue (v10's matchmaking socket)

// v10's handleWebSocketMessage: the socket's {type, payload} frames are handled as the brain's are
static void kio_frame(void *user, const cJSON *frame) {
    (void) user;
    if (frame) handle_message(frame);
    else queue_unparseable();
}

// the socket ended: v10's onclose clears the state (no message), a socket that never opened says so
static void kio_done(void *user, const int failed) {
    (void) user;
    clear_state();
    R.busy = 0;
    if (failed) { // v10 onerror: message "Failed to connect to matchmaking server", error INTERNAL_ERROR (v10:98441-98452)
        banner_error("Failed to connect to matchmaking server");
        internal_error();
    }
    render();
}

// startMatchmaking(allRegions): connectWebSocket with the region (v10:98420); the queue's frames follow. The
// state stays Idle (LOADING...) until QUEUED, as v10's (handle_message starts the search then).
static void start_kio_queue(void) {
    if (flavor_get() != FLAVOR_BASE) {
        // another app's client never queues a Box Office ranked match. v10 has no such refusal: its ERROR for an
        // unknown code reads "Unknown error" and opens the Internal Error popup (handleWebSocketMessage, v10:98444-98455)
        banner_error("Unknown error");
        internal_error();
        return;
    }
    R.busy = 1;
    g_kio_queue = 1;
    g_banner_error[0] = 0; // (v10 updateGlobalStatus: error null, v10:98389)
    if (!krunker_ranked_queue_start(R.all_regions ? "as,eu,na" : my_region(), kio_frame, kio_done, NULL)) {
        g_kio_queue = 0;
        queue_unreachable();
    }
    update_banner();
}

// v10 ranked.getMyConnection (api, v10:25985): the game socket's URL of a match in progress. v10's
// REJOIN (quickJoinRankedMatch) joins it; the popup shows "You are already in a Ranked game." and REJOIN.
static void on_kio_connection(void *user, const int status, const cJSON *body) {
    (void) user;
    R.busy = 0;
    const cJSON *data = cJSON_GetObjectItem(body, "data");
    const char *url = status == 200 ? v10_str(data, "serverUrl") : "";
    snprintf(R.rejoin, sizeof(R.rejoin), "%s", url);
    cJSON_Delete(R.rejoin_as);
    R.rejoin_as = NULL;
    if (R.rejoin[0]) R.status_lock = 1;
    render();
}

static void fetch_kio_connection(void) { // fetchIsPlayerInRankedMatch
    if (R.scenario) return;
    R.busy = 1;
    if (!krunker_ranked_request(KRUNKER_RANKED_API, "GET", "/ranked/me/connection", NULL, on_kio_connection, NULL)) R.busy = 0;
}

static void start_matchmaking(void) { // startMatchmaking(allRegions)
    const RankedBackend be = ranked_active_backend();
    if (R.state != MM_IDLE || be == RANKED_BACKEND_NONE) return;
    if (be == RANKED_BACKEND_KRUNKER) return start_kio_queue();
    char t[256], path[512];
    token(t, sizeof(t));
    int len = snprintf(path, sizeof(path), "/mm/ranked/queue?token=%s&regions=%s", t, R.all_regions ? "as,eu,na" : my_region());
    // another app's client (Krunker Strike) says so: the coordinator never queues it into a Box Office
    // ranked match (FLAVOR_UNAVAILABLE), whose server would refuse it
    if (flavor_get() != FLAVOR_BASE && len > 0 && (size_t) len < sizeof(path))
        snprintf(path + len, sizeof(path) - (size_t) len, "&flavor=%s", flavor_current()->key);
    R.busy = 1; // LOADING... until the answer: its QUEUED frame starts the search (handle_message), as v10's
    if (!backend_http_get(path, on_queue, NULL)) queue_unreachable();
    update_banner();
}

static void cancel_matchmaking(void) { // cancelMatchmaking: v10 closes the socket
    if (g_kio_queue) { // (no leave request: v10's cancel only closes its socket)
        clear_state();
        return;
    }
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
    if (R.home_check) { // the home load found a match: the popup opens with the REJOIN status (v10:209847)
        R.home_check = 0;
        if (R.rejoin[0]) {
            open_popup();
            return;
        }
    }
    render();
}

static void fetch_connection(void) { // fetchIsPlayerInRankedMatch
    const RankedBackend be = ranked_active_backend();
    if (be == RANKED_BACKEND_KRUNKER) return fetch_kio_connection();
    if (R.scenario || be == RANKED_BACKEND_NONE || backend_available() != 1) return;
    char t[256], path[400];
    token(t, sizeof(t));
    snprintf(path, sizeof(path), "/mm/ranked/connection?token=%s", t);
    R.busy = 1;
    if (!backend_http_get(path, on_connection, NULL)) {
        R.busy = 0;
        R.home_check = 0;
    }
}

// v10 (v10:209847): once the signed-in home has loaded, fetchIsPlayerInRankedMatch(); a player in a
// ranked match gets the Ranked popup. Returns 0 while the check cannot run yet (no account, the
// brain not reachable): the menu asks again.
int ranked_ui_home_ready(void) {
    if (R.scenario) return 1;
    if (!acc() || R.busy || R.state != MM_IDLE || backend_available() != 1) return 0;
    R.home_check = 1;
    fetch_connection();
    return 1;
}

const char *ranked_ui_status(void) {
    if (html_dom_by_id("matchmakingStatusEl")) return NULL; // the v10 page shows its banner instead
    return R.state != MM_IDLE && R.status[0] ? R.status : NULL;
}
const char *ranked_ui_ready(void) { return NULL; } // v10: solo queue, no party ready count
int ranked_ui_in_ranked(void) { return R.state != MM_IDLE || R.in_match; }
// v10's effect on $account (v10:111598-111600): a Login Required popup hides once an account is signed in
void ranked_ui_account_changed(void) {
    if (!R.overlay.visible || strcmp(R.overlay.title, "Login Required") || ranked_active_backend() == RANKED_BACKEND_NONE) return;
    R.overlay.visible = 0;
    R.overlay.intro = R.overlay.outro = 0;
    v10_refresh();
}
// The announcement card's ranked-queue action (v10 'ranked-queue', v10:116880-116888, 117007-117013): 1 In Queue
// while the search runs ($inRankedQueue), 2 Requires Level 10 for a signed-in account under 10 ((level || 0) < 10),
// else 0 (on; a guest's click is the login).
int ranked_ui_queue_action_state(void) {
    if (R.state == MM_SEARCHING) return 1;
    if (ranked_active_backend() != RANKED_BACKEND_NONE && active_level() < 10) return 2;
    return 0;
}
// the click (v10:116880-116882): a signed-in account that is not queued starts the search (startMatchmaking with
// the region); a guest gets v10's login. KrunkNative starts one request at a time (R.busy); v10 has no such guard.
void ranked_ui_queue_action(void) {
    if (ranked_ui_queue_action_state() != 0) return;
    if (ranked_active_backend() == RANKED_BACKEND_NONE) {
        v10_menu_handle("loginOrRegister", NULL, NULL, 0);
        return;
    }
    if (!R.busy) start_matchmaking();
}

// quickJoinRankedMatch's JOINED_RANK: the match's INIT is applied, so the server has this client in
// its ranked game. KrunkNative sends it as NET_EXT "joinedRank" (no body) once, for the joined match.
void ranked_ui_on_init(Client *client) {
    if (!R.join_pending || !client) return;
    R.join_pending = 0;
    R.in_game_match = 1;
    uint8_t buf[NET_EXT_HEADER_SIZE];
    const uint16_t n = net_ext_pack(buf, sizeof(buf), NET_EXT_JOINED_RANK, NULL, 0);
    if (n) client_net_send(client, (NetPacketType) NET_PACKET_EXT, buf, n);
}

void ranked_ui_left_match(void) {
    if (R.in_match) invalidate(); // the profile changed
    R.in_match = 0;
    R.in_game_match = 0;
    R.join_pending = 0;
    R.join_host[0] = 0;
    R.status_lock = 0;
    R.rejoin[0] = 0;
}
void ranked_ui_class_changed(const int index) { (void) index; } // v10: the class is picked in the match's comp lobby

// ------------------------------------------------------------------ milestones, shop

static int g_claim_reward; // the milestone the claim takes (knRkClaim sets it): v10's _33842[index].reward (v10:98744)

// The reward of the milestone a claim takes: the one after last_milestone_claimed, in the rows of the last profile
// fetch (v10's claimable row, v10:98927). 0 when there is none.
static int next_milestone_reward(const RankedBackend be) {
    if (be == RANKED_BACKEND_KRUNKER) { // the krunker.io profile's rows (adapt_profile keeps them)
        const int next = g_last_claimed + 1;
        return g_milestones && next >= 0 && next < cJSON_GetArraySize(g_milestones) ? (int) v10_num(cJSON_GetArrayItem(g_milestones, next), "reward", 0) : 0;
    }
    const V10Req *pr = get_profile(current_season()); // the brain's profile: {profile, milestones, rankedPoints} (services/brain/ranked.c)
    const int last = (int) v10_num(profile_of(pr), "last_milestone_claimed", -1);
    return (int) v10_num(cJSON_GetArrayItem(cJSON_GetObjectItem(pr->json, "milestones"), last + 1), "reward", 0);
}

static void on_claim(void *user, const int status, const cJSON *json) {
    (void) user;
    // v10 (onClaimMilestone, v10:112238-112240): a failed claim is silent, nothing is refetched (v10:98757-98764)
    if (status != 200 || !cJSON_IsTrue(cJSON_GetObjectItem(json, "success"))) return;
    // v10 (v10:98744): the balance grows by the reward at once, then the profile is fetched again. A krunker.io
    // account's balance is the account row (on_kio_claim changes it). The brain's is the cached profile's: it is bumped
    // in place and the answers are expired, not dropped, so the bumped profile stays up until the new one comes.
    if (ranked_active_backend() == RANKED_BACKEND_KRUNKER) {
        invalidate(); // _33975.clear(): the profile (and RP balance) again
    } else {
        const V10Req *pr = get_profile(current_season());
        cJSON *rp = pr->json ? cJSON_GetObjectItem(pr->json, "rankedPoints") : NULL;
        if (cJSON_IsNumber(rp) && g_claim_reward > 0) cJSON_SetNumberValue(rp, rp->valuedouble + g_claim_reward);
        v10_expire("/mm/ranked/");
    }
    render();
}

static void on_kio_claim(void *user, const int status, const cJSON *body) {
    (void) user;
    cJSON *res = kio_result(body);
    // v10 adds the reward to its balance when the claim succeeds (rankedPoints += reward, v10:98744)
    if (status == 200 && cJSON_IsTrue(cJSON_GetObjectItem(res, "success"))) {
        printf("[ranked] milestone claimed: +%d RP to the balance\n", g_claim_reward);
        krunker_account_add_points(g_claim_reward);
    }
    on_claim(NULL, status, res);
    cJSON_Delete(res);
}

typedef struct {
    int id;          // ranked_shop row id
    int skin;
    int price;
} Purchase;

static Purchase g_buy;
static int g_buy_kio;   // the purchase is krunker.io's: its skin is not added to the KrunkNative inventory

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
        if (!g_buy_kio) account_add_skin(g_buy.skin);
    } else {
        const char *e = v10_str(json, "error");
        // v10: a thrown request is "Network error" (v10:111062); an answer without success or error is "Purchase failed" (v10:111056)
        v10_view_push(v10_io_error("Error", status == 0 ? "Network error" : e[0] ? e : "Purchase failed"), NULL, NULL, NULL);
    }
    invalidate();
    render();
}

static void on_kio_purchase(void *user, const int status, const cJSON *body) {
    (void) user;
    cJSON *res = kio_result(body);
    // v10 takes the price off its balance when the purchase succeeds (rankedPoints -= price, v10:111042)
    if (status == 200 && cJSON_IsTrue(cJSON_GetObjectItem(res, "success"))) {
        printf("[ranked] skin bought: -%d RP from the balance\n", g_buy.price);
        krunker_account_add_points(-(double) g_buy.price);
    }
    on_purchase(NULL, status, res);
    cJSON_Delete(res);
}

static void confirm_closed(const int accepted, void *user) {
    (void) user;
    if (!accepted || R.buying) return;
    R.buying = 1;
    render();
    if (ranked_active_backend() == RANKED_BACKEND_KRUNKER) { // purchaseSkin({skin_id: the shop row's id}) (v10:111039)
        char body[48];
        snprintf(body, sizeof(body), "{\"skin_id\":%d}", g_buy.id);
        g_buy_kio = 1;
        if (!krunker_ranked_request(KRUNKER_RANKED_GAPI, "POST", "/ranked/purchase", body, on_kio_purchase, NULL)) on_purchase(NULL, 0, NULL);
        return;
    }
    g_buy_kio = 0;
    char t[256], path[400];
    token(t, sizeof(t));
    snprintf(path, sizeof(path), "/mm/ranked/purchase?token=%s&id=%d", t, g_buy.id);
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

// v10's restrictedBy === "mmr" (v10:111314, 112369): the server's restricted_by, nothing else
static int mmr_restricted(const cJSON *s) {
    return !strcmp(v10_str(s, "restricted_by"), "mmr");
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
                    "<button class=\"share-btn svelte-1524ito\" type=\"button\" title=\"Copy link\" id=\"knRkShareBtn\" onclick=\"knRkShare(%d)\"><i class=\"material-icons svelte-1524ito\">share</i> "
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

// v10 (openRankedChallenges, v10:215670-215678): the Challenges window scrolls to its Ranked block,
// #challRankedHeadr, when that is inside #menuWindow. The page is scaled (scaleBase), so the viewport
// distance is divided by the header's scale to get the layout offset that scrollTop takes.
static void scroll_to_ranked_challenges(HtmlElem mw, HtmlElem head) {
    HtmlRect hr = {0}, mr = {0};
    HtmlScroll s = {0};
    if (!html_dom_get_rect(head, &hr) || !html_dom_get_rect(mw, &mr) || !html_dom_get_scroll(mw, &s)) return;
    const float ow = html_dom_offset_width(head), scale = ow > 0.0f && hr.w > 0.0f ? hr.w / ow : 1.0f;
    html_dom_set_scroll(mw, s.left, s.top + (hr.y - mr.y) / scale);
}

// The window shows "Loading" until its challenges arrive (account_ui.c gen_challenges), so the Ranked
// block is there a little later: look for it every 100 ms for up to 10 s, then scroll once.
#define RANKED_SCROLL_TRIES 100
static int g_ranked_scroll_tries;
static void scroll_ranked_header_fire(void *user) {
    (void) user;
    HtmlElem mw = html_dom_by_id("menuWindow"), head = html_dom_by_id("challRankedHeadr");
    if (!mw || !head) {
        if (++g_ranked_scroll_tries < RANKED_SCROLL_TRIES) html_timer_add(100.0, scroll_ranked_header_fire, NULL);
        return;
    }
    scroll_to_ranked_challenges(mw, head);
}

// ShareButton's copied state (v10:103485-103520): the title "Copied!", the check icon, the label "Copied!", the
// .share-copied class, all back to Copy link / share / Share after 1.5 s (a new copy restarts the 1.5 s).
static int g_share_timer;
static void share_copied_set(const int copied) {
    const HtmlElem btn = html_dom_by_id("knRkShareBtn"); // the item view's Share button (gone with its view: no-op)
    if (!btn) return;
    html_dom_set_attr(btn, "title", copied ? I18N("generic.copied", "Copied!") : "Copy link");
    html_dom_set_class(btn, "share-copied", copied);
    const HtmlElem icon = html_dom_query(btn, "i"), label = html_dom_query(btn, "span");
    if (icon) html_dom_set_text(icon, copied ? "check" : "share");
    if (label) html_dom_set_text(label, copied ? I18N("generic.copied", "Copied!") : "Share");
}
static void share_copied_done(void *user) {
    (void) user;
    g_share_timer = 0;
    share_copied_set(0);
}
static void share_copied_show(void) {
    if (g_share_timer) html_timer_cancel(g_share_timer);
    share_copied_set(1);
    g_share_timer = html_timer_add(1500, share_copied_done, NULL);
}

int ranked_ui_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    if (!name) return -1;
    const int arg0 = arg_num(args, argc, 0, 0);
    if (!strcmp(name, "openRankedMenu")) { // v10: guests get loginOrRegister()
        if (ranked_active_backend() != RANKED_BACKEND_NONE || R.scenario) open_popup();
        else if (v10_menu_handle("loginOrRegister", NULL, NULL, 0) < 0) menu_show_window(5, 1);
        return 1;
    }
    if (!strcmp(name, "openRankedChallenges")) { // the home's "Ranked Challenges" link (v10:215670)
        const HtmlArg select = {.kind = HTML_ARG_NUM, .num = 0.1};
        html_ui_dispatch("playSelect", ev, &select, 1); // playSelect(0.1)
        v10_popup_close();                               // clearPops()
        html_ui_dispatch("windows.32.openHome", ev, NULL, 0); // windows[32].openHome(): showWindow(33) for an account
        g_ranked_scroll_tries = 0;
        html_timer_add(0.0, scroll_ranked_header_fire, NULL);
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
        if (ranked_active_backend() == RANKED_BACKEND_NONE || R.busy || R.state == MM_FOUND) return 1;
        if (R.status_lock && R.rejoin_as) {
            cJSON *as = R.rejoin_as;
            R.rejoin_as = NULL;
            R.status_lock = 0;
            R.rejoin[0] = 0;
            join_assignment(as); // quickJoinRankedMatch(); clearPops()
            cJSON_Delete(as);
            return 1;
        }
        if (R.status_lock && ranked_active_backend() == RANKED_BACKEND_KRUNKER && R.rejoin[0]) { // REJOIN: quickJoinRankedMatch (v10:98729)
            char url[sizeof(R.rejoin)];
            snprintf(url, sizeof(url), "%s", R.rejoin);
            R.status_lock = 0;
            R.rejoin[0] = 0;
            join_kio_match(url, NULL);
            render();
            return 1;
        }
        // REJOIN with no server yet (the match is still being formed, ALREADY_IN_MATCH without a serverUrl):
        // look the connection up again instead of starting a new search (v10 quickJoinRankedMatch with no URL)
        if (R.status_lock) {
            fetch_connection();
            return 1;
        }
        if (R.state == MM_IDLE) start_matchmaking();
        else if (R.state == MM_SEARCHING) cancel_matchmaking();
    } else if (!strcmp(name, "knRkClaim")) { // onClaimMilestone(index): the next one, in order
        const RankedBackend be = ranked_active_backend();
        if (be == RANKED_BACKEND_NONE) return 1;
        // v10 (v10:112105-112116): the next milestone's reward flies off its marker before the request
        g_claim_reward = next_milestone_reward(be);
        if (g_claim_reward > 0 && ev) v10_flying_number(ev->target, g_claim_reward);
        if (be == RANKED_BACKEND_KRUNKER) { // claimMilestone(index) (v10:25974): the region index
            char path[96];
            snprintf(path, sizeof(path), "/player-profile/me/claim-milestone/region/%d", ranked_region_index(my_region()));
            if (!krunker_ranked_request(KRUNKER_RANKED_API, "POST", path, NULL, on_kio_claim, NULL)) on_claim(NULL, 0, NULL);
            return 1;
        }
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
            R.history_page = next; // v10 rebuilds the page's entries: collapsed, sorted by score again (v10:108074-108078)
            memset(R.match_open, 0, sizeof(R.match_open));
            memset(R.match_sort, 0, sizeof(R.match_sort)); // v10 builds each page's entries again at the default sort (v10:108072-108080)
            memset(R.match_desc, 0, sizeof(R.match_desc));
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
        if (kio_store_active()) kio_store_read_only("ranked/purchase"); // krunker.io: RP purchases are never sent
        else if (ranked_active_backend() != RANKED_BACKEND_NONE) buy(arg0);
        return 1;
    } else if (!strcmp(name, "knRkShare")) { // ShareButton's click (v10:103485): the link, then "Copied!" for 1.5 s
        html_ui_stop_propagation();
        char origin[256], url[320];
        html_page_origin(origin, sizeof(origin));
        snprintf(url, sizeof(url), "%s/?item=%d", origin, arg0);
        html_clipboard_write(url);
        share_copied_show();
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
    // v10's _34033 (v10:98335) sets total_matches to wins + losses and derives every ratio from the totals
    d.matches = ranked_active_backend() == RANKED_BACKEND_KRUNKER ? d.wins + d.losses : v10_num(p, "total_matches", d.wins + d.losses);
    if (d.matches <= 0) return d;
    const double kills = v10_num(p, "total_kills", 0), deaths = v10_num(p, "total_deaths", 0), time_ms = v10_num(p, "total_time_played", 0);
    d.avg_kills = kills / d.matches;
    d.avg_deaths = deaths / d.matches;
    d.kd = kills / (deaths > 1 ? deaths : 1);
    d.dpm = v10_num(p, "total_damage_done", 0) / (time_ms / 60000.0); // v10 (v10:98338): NaN (0 / 0) reads 0
    if (isnan(d.dpm)) d.dpm = 0;
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
    // the season-end estimate is the brain's payout formula (shared/src/ranked.c, v10:112066-112067)
    const long long points = (long long) ranked_season_points(v10_num(p, "conservative_rating", 0), matches, current_season());
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
    sb_append(sb, " <div class=\"challenges-link svelte-17kea4i\" onclick=\"playSelect(0.1),openRankedChallenges()\" onmouseenter=\"playTick()\"><span class=\"material-icons challenges-link-icon svelte-17kea4i\">"
                  "emoji_events</span> <span class=\"challenges-link-text svelte-17kea4i\">Ranked Challenges</span> <span class=\"material-icons svelte-17kea4i\">chevron_right</span></div> "
                  "<div class=\"description-container svelte-17kea4i\"><p class=\"svelte-17kea4i\">Compete in a best of 3 - 4v4 Hardpoint - map pool rotates every season</p> "
                  "<div class=\"map-carousel-wrapper svelte-17kea4i\">");
    int maps[RANKED_SEASON_MAPS];
    ranked_season_maps(season, maps);
    sb_appendf(sb, "<div class=\"map-carousel-container svelte-17kea4i\" style=\"--total-maps: %d; --extended-maps: %d;\">", RANKED_SEASON_MAPS, 2 * RANKED_SEASON_MAPS);
    for (int k = 0; k < 2 * RANKED_SEASON_MAPS; k++) { // [...maps, ...maps]: the carousel loops (v10:111965-111969)
        // v10 lists every pool map alike (no greying, no alt text): the queue's own filter decides what plays
        const int id = maps[k % RANKED_SEASON_MAPS];
        const char *name = ranked_pool_map_name(id);
        sb_appendf(sb, "<div class=\"map-card svelte-17kea4i\"><img class=\"map-image svelte-17kea4i\" src=\"./img/maps/map_%d.png\"> <div class=\"map-name svelte-17kea4i\">", id);
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
    sb_append_esc(sb, name[0] ? name : active_name());
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
    const cJSON *own = profile_of(get_profile(current_season())); // v10 compares with the profile's player_name (v10:111751)
    const char *me = v10_str(own, "player_name");
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
        const int mine = me[0] && !strcmp(name, me);
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
    const char *me = active_name();
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
            const int desc = R.match_sort[index][0] ? R.match_desc[index] : 1; // v10's default: score, descending (v10:108078, toggled from asc at :108099)
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
// v10's countdown onExpired (v10:112316-112320, 112384-112393) refetches the shop (getShop, v10:111020): an item's
// expires_at (epoch ms) passing drops the shop's cached answer and re-renders. One timer per expiry, at most 32.
#define EXPIRY_TIMERS 32
static double g_expiry_set[EXPIRY_TIMERS];
static int g_expiry_n;

static void shop_expired(void *user) {
    (void) user;
    for (int i = 0; i < KIO_SLOTS; i++) { // the shop's krunker.io answer
        if (!g_kio[i].used || g_kio[i].inflight || strcmp(g_kio[i].key, "shop")) continue;
        cJSON_Delete(g_kio[i].req.json);
        memset(&g_kio[i], 0, sizeof(g_kio[i]));
    }
    v10_forget("/mm/ranked/shop");   // the brain's
    v10_refresh();
}

static void schedule_expiry(const double expires_ms) {
    if (expires_ms <= 0) return;
    for (int i = 0; i < g_expiry_n; i++)
        if (g_expiry_set[i] == expires_ms) return;
    const double left = expires_ms - (double) time(NULL) * 1000.0;
    if (left <= 0 || g_expiry_n >= EXPIRY_TIMERS) return;
    g_expiry_set[g_expiry_n++] = expires_ms;
    html_timer_add(left + 1000, shop_expired, NULL); // (a second over: the countdown reads 0 by then)
}

static void gen_shop(StrBuf *sb) {
    const V10Req *r = get_shop();
    const cJSON *expiring;
    cJSON_ArrayForEach(expiring, cJSON_GetObjectItem(r->json, "skins"))
        if (has_num(expiring, "expires_at")) schedule_expiry(v10_num(expiring, "expires_at", 0));
    sb_append(sb, "<div class=\"shop-container svelte-1gtpoav\"><div class=\"shop-header svelte-1gtpoav\"><div class=\"header-top svelte-1gtpoav\">"
                  "<div class=\"header-subtitle svelte-1gtpoav\">Spend your Ranked Points on exclusive items.</div> <div class=\"rp-balance svelte-1gtpoav\">");
    double rp = 0;
    rp_balance(&rp);
    v10_currency(sb, rp, "RP", "large", 0);
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
    if (ranked_active_backend() == RANKED_BACKEND_NONE && !R.scenario)
        show_overlay("Login Required", "You must be logged in to access Ranked Mode.", "GOT IT", "#5ce05a", "\xf0\x9f\x94\x92", 1);
    else if (active_level() >= 0 && active_level() < g_min_level) { // (krunker.io: the level is known once its game server said so)
        char text[96];
        snprintf(text, sizeof(text), "You must be level %d or higher to access Ranked Mode.", g_min_level);
        show_overlay("Level Required", text, "OK", "#ff3939", "\xf0\x9f\x94\x92", 1);
    }
    invalidate(); // v10's component fetches on mount, nothing kept from an earlier visit (v10:98587-98639)
    R.tab = TAB_HOME; // the component starts on Home every time it mounts
    R.board_page = R.history_page = 1;
    memset(R.match_open, 0, sizeof(R.match_open));
    memset(R.match_sort, 0, sizeof(R.match_sort));
    memset(R.match_desc, 0, sizeof(R.match_desc));
    if (R.state == MM_IDLE && !R.status_lock) fetch_connection();
    char *html = gen_popup();
    v10_popup_show(POPUP_CLASS, html, render);
    free(html);
}

// windows[26].gen() returns '' in v10 (the Ranked UI is the popup)
char *ranked_ui_window_gen(void) { return strdup(""); }

// --render-scenario: state.ranked {open, tab, profile, info, board, history, shop, found,
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
    if (cJSON_IsTrue(cJSON_GetObjectItem(state, "found"))) { // MATCHED (on_status) without its assignment / accept timer
        R.state = MM_FOUND;
        set_status("Match found!", "\xe2\x9c\xa8", "#5ce05a");
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
