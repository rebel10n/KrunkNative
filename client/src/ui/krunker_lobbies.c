// Official krunker.io lobbies (client/include/krunker_lobbies.h, doc/v10/matchmaker-token.md
// "Official lobbies"). Native only: platform behaviour, so the web build compiles the stubs at
// the end (#ifndef __EMSCRIPTEN__) and never lists or requests anything from krunker.io.
#include <join_errors.h>
#include <krunker_lobbies.h>
#include <krunker_net.h>
#include <seek_prefs.h>
#include <seek_filter.h> // SEEK_LOBBY_* (the lobby flags of seek_prefs_lobby_ok)
#include <server_names.h>
#include <ui_logic.h>
#include <html_dom.h> // html_timer_add (the finder's "Took to long" timer)
#include <v10_menu.h> // v10_menu_popular_changed
#include <backend.h>
#include <flavor.h> // Krunker Strike lists and joins no official krunker.io lobby
#include <krunker_build.h> // KRUNKER_REGIONS / KRUNKER_DEDICATED_ONLY_REGIONS: v10's availableRegions / dedicatedOnlyRegions
#include <region_ping.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// v10 _7517 (v10.js:18672-18701): the region names of Krunker's server browser
static const struct {
    const char *code, *name;
} k_krunker_regions[] = {
    {"us-nj", "New York"}, {"us-il", "Chicago"}, {"us-tx", "Dallas"}, {"us-wa", "Seattle"},
    {"us-ca-la", "Los Angeles"}, {"us-ga", "Atlanta"}, {"nl-ams", "Amsterdam"}, {"gb-lon", "London"},
    {"de-fra", "Frankfurt"}, {"us-ca-sv", "Silicon Valley"}, {"au-syd", "Sydney"}, {"fr-par", "Paris"},
    {"jb-hnd", "Tokyo"}, {"us-fl", "Miami"}, {"sgp", "Singapore"}, {"blr", "India"}, {"as-mb", "Mumbai"},
    {"brz", "Brazil"}, {"me-bhn", "Middle East"}, {"af-ct", "South Africa"}, {"as-seoul", "South Korea"},
    {"mx-cmx", "Mexico"}, {"eu-stm", "Sweden"}, {"tor", "Toronto"}, {"pol", "Poland"}, {"us-hi", "Hawaii"},
    {"sss", "EU Super Secret Servers"}, {"iow", "Iowa"},
};

const char *krunker_region_name(const char *code) {
    for (size_t i = 0; code && i < sizeof(k_krunker_regions) / sizeof(k_krunker_regions[0]); i++) {
        if (!strcmp(k_krunker_regions[i].code, code)) return k_krunker_regions[i].name;
    }
    return code ? code : "";
}

#ifndef __EMSCRIPTEN__
#include <GLFW/glfw3.h>
#include <headless.h>
#include <http_proxy.h>

#define KRUNKER_DEFAULT_MATCHMAKER "https://matchmaker.krunker.io"
#define KRUNKER_DEFAULT_SITE "https://krunker.io"
#define KRUNKER_MIN_INTERVAL_MS 15000.0 // never more often, also when forced
#define KRUNKER_RETRY_MS 30000.0        // after a failure
#define KRUNKER_LIST_MAX 4000           // lobbies kept (the busiest)

static struct {
    cJSON *list;            // tuples (owned), krunker.io's order
    double players;         // totalPlayerCount
    double fetched_ms;      // the last request's start (glfwGetTime ms), -inf = never
    double answered_ms;     // the last good answer
    int loading;
    int seek_pending;       // krunker_lobbies_seek waits for the answer
    int finder_mode;        // krunker_lobbies_find_custom waits for the answer (its mode id; 0 = none)
    int refetch;            // the network changed while a request ran: ask again after it
    int fixed;              // krunker_lobbies_set_fixed: a scenario's list, never fetched
    cJSON *pings;           // {region: ms} of krunker.io's regions, measured with the last list (v10 pingData; -1 failed)
    int pinging;            // the ping list / pings of a refresh are in flight
    char error[200];
} K = {.fetched_ms = -1e18, .answered_ms = -1e18};

static double now_ms(void) { return glfwGetTime() * 1000.0; }

static Client *g_lobbies_client; // the in-client join's client (krunker_lobbies_attach)

void krunker_lobbies_attach(Client *client) { g_lobbies_client = client; }

static const char *matchmaker_url(void) {
    const char *url = getenv("KRUNKNATIVE_KRUNKER_MATCHMAKER");
    return url && url[0] ? url : KRUNKER_DEFAULT_MATCHMAKER;
}

int krunker_lobbies_enabled(void) {
    // official krunker.io lobbies are Krunker Box Office's: the Krunker Strike app's server list,
    // seek and Quick Match show Strike lobbies only (its coordinator pool, servers_ui.c)
    if (flavor_get() != FLAVOR_BASE) return 0;
    if (K.fixed) return 1; // a scenario's / test's list (krunker_lobbies_set_fixed)
    const char *env = getenv("KRUNKNATIVE_KRUNKER_MATCHMAKER");
    if (env && (!strcmp(env, "off") || !strcmp(env, "0"))) return 0;
    if (env && env[0]) return 1;
    // like backend_init: headless runs (tests, captures) and scenario renders never reach the
    // real krunker.io unless the matchmaker is given explicitly
#ifdef UI_SCENARIO_SUPPORTED
    if (ui_scenario_active()) return 0;
#endif
    return !headless_enabled();
}

// The matchmaker's host goes through a proxy (http_proxy.h); its label in *label.
static int via_proxy(char *label, const size_t n) {
    char host[128] = "";
    const char *p = strstr(matchmaker_url(), "://");
    p = p ? p + 3 : matchmaker_url();
    size_t len = 0;
    while (p[len] && p[len] != ':' && p[len] != '/' && len + 1 < sizeof(host)) {
        host[len] = p[len];
        len++;
    }
    host[len] = 0;
    HttpProxy px;
    if (!http_proxy_for(host, !strncmp(matchmaker_url(), "https:", 6), &px)) return 0;
    http_proxy_label(&px, label, n);
    return 1;
}

// krunker.io's block page: "ACCESS DENIED" HTML (a 403) instead of the JSON
static int access_denied(const int status, const char *body) {
    static const char WORD[] = "access denied";
    for (const char *p = body; p && *p; p++) {
        if (!strncasecmp(p, WORD, sizeof(WORD) - 1)) return 1;
    }
    return status == 403;
}

static int cmp_players(const void *a, const void *b) {
    const cJSON *x = *(const cJSON *const *) a, *y = *(const cJSON *const *) b;
    return (int) cJSON_GetNumberValue(cJSON_GetArrayItem(y, 2)) - (int) cJSON_GetNumberValue(cJSON_GetArrayItem(x, 2));
}

static int id_ok(const char *id) {
    if (!id || !id[0] || strlen(id) > 64) return 0;
    for (const char *p = id; *p; p++) {
        const char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ':' || c == '-' || c == '_')) return 0;
    }
    return 1;
}

static void copy_key(cJSON *meta, const cJSON *data, const char *from, const char *to) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(data, from);
    if (cJSON_IsString(v)) cJSON_AddStringToObject(meta, to, v->valuestring);
    else if (cJSON_IsNumber(v)) cJSON_AddNumberToObject(meta, to, v->valuedouble);
    else if (cJSON_IsBool(v)) cJSON_AddBoolToObject(meta, to, cJSON_IsTrue(v));
}

// One game-list entry -> a server-browser tuple, NULL when it is not one.
static cJSON *to_tuple(const cJSON *g) {
    const char *id = cJSON_GetStringValue(cJSON_GetArrayItem(g, 0));
    const char *region = cJSON_GetStringValue(cJSON_GetArrayItem(g, 1));
    const cJSON *players = cJSON_GetArrayItem(g, 2), *max = cJSON_GetArrayItem(g, 3);
    const cJSON *data = cJSON_GetArrayItem(g, 4), *left = cJSON_GetArrayItem(g, 5);
    if (!id_ok(id) || !cJSON_IsNumber(players) || !cJSON_IsNumber(max)) return NULL;
    cJSON *t = cJSON_CreateArray();
    cJSON_AddItemToArray(t, cJSON_CreateString(id));
    cJSON_AddItemToArray(t, cJSON_CreateString(region ? region : ""));
    cJSON_AddItemToArray(t, cJSON_CreateNumber(players->valuedouble > 0 ? players->valuedouble : 0));
    cJSON_AddItemToArray(t, cJSON_CreateNumber(max->valuedouble > 0 ? max->valuedouble : 0));
    cJSON *meta = cJSON_CreateObject();
    cJSON_AddTrueToObject(meta, "krunker");
    copy_key(meta, data, "i", "i");   // map name
    copy_key(meta, data, "h", "h");   // host / custom game name
    copy_key(meta, data, "g", "mode"); // mode index (v10 gameModes, = our mode ids)
    copy_key(meta, data, "c", "cs");  // custom game
    copy_key(meta, data, "oc", "oc"); // official custom
    copy_key(meta, data, "ds", "ds"); // dedicated 24/7
    copy_key(meta, data, "pw", "pw"); // password
    copy_key(meta, data, "kr", "kr"); // KR badge
    copy_key(meta, data, "cm", "cm"); // a custom game's map id (the browser's carousel: user-assets m<cm>/thumb.png)
    if (cJSON_IsNumber(left)) cJSON_AddNumberToObject(meta, "timeLeft", left->valuedouble);
    cJSON_AddItemToArray(t, meta);
    return t;
}

static void run_waiting(void);

// The official lobbies of a region whose ping host failed the TLS handshake (region_ping.h REGION_PING_TLS_FAILED)
// leave the list: the server browser, the seek and Find Game all read it. KrunkNative's rule, v10 has none: the list
// is krunker.io's as it sent it, and only the lobbies of that region go (its group with them).
static void hide_tls_failed(void) {
    if (!K.list) return;
    cJSON *p;
    cJSON_ArrayForEach(p, K.pings) {
        if (!p->string || !cJSON_IsNumber(p) || p->valuedouble != REGION_PING_TLS_FAILED) continue;
        int hidden = 0;
        for (int i = cJSON_GetArraySize(K.list) - 1; i >= 0; i--) {
            const char *region = cJSON_GetStringValue(cJSON_GetArrayItem(cJSON_GetArrayItem(K.list, i), 1));
            if (region && !strcmp(region, p->string)) {
                cJSON_DeleteItemFromArray(K.list, i);
                hidden++;
            }
        }
        if (hidden)
            printf("[krunker] hid %d official %s in region %s: its ping host failed the TLS handshake\n", hidden, hidden == 1 ? "lobby" : "lobbies",
                   p->string);
    }
}

static void on_pings(void *user, cJSON *pings) {
    (void) user;
    cJSON_Delete(K.pings);
    K.pings = pings;
    K.pinging = 0;
    printf("[krunker] lobby row pings measured: %d regions\n", cJSON_GetArraySize(pings));
    hide_tls_failed();
    run_waiting(); // the seek / Find Game that waited for these pings
    menu_rerender_if_open(2); // the rows' ping icons (windows[1].pingData)
}

static void on_kio_ping_list(void *user, const int status, const char *body, const size_t length) {
    (void) user;
    (void) length;
    cJSON *list = status == 200 && body ? cJSON_Parse(body) : NULL;
    if (!cJSON_IsObject(list) || !region_ping_measure(list, on_pings, NULL)) { // nothing pingable: no icons
        K.pinging = 0;
        run_waiting();
    }
    cJSON_Delete(list);
}

// v10's loadData (getRegionPing): the ping list, then every region's /ping timed. A list load asks again;
// a ping round still in flight is not repeated.
static void krunker_lobbies_measure_pings(void) {
    char url[512];
    if (K.pinging) return;
    snprintf(url, sizeof(url), "%s/ping-list?hostname=krunker.io", matchmaker_url()); // fetchPingList (v10.js:147583-147588)
    K.pinging = backend_http_get_external(url, KRUNKER_LOBBIES_TIMEOUT_MS, on_kio_ping_list, NULL) != 0;
}

int krunker_lobbies_region_ping_ms(const char *region, double *ms) {
    const cJSON *p = region && K.pings ? cJSON_GetObjectItemCaseSensitive(K.pings, region) : NULL;
    if (!cJSON_IsNumber(p)) return 0;
    if (ms) *ms = p->valuedouble;
    return 1;
}

static int finder_open(int mode); // Find Game's official customs of a mode (below)
static void finder_none(void);

// The seek and Find Game that waited for the list's pings (krunker_lobbies_seek / krunker_lobbies_find_custom while
// K.pinging) run now, on the list the pings settled: never on a list that still holds a TLS-failed region's lobbies.
static void run_waiting(void) {
    if (K.pinging) return;
    if (K.seek_pending) {
        K.seek_pending = 0;
        krunker_lobbies_seek();
    }
    if (K.finder_mode) {
        const int mode = K.finder_mode;
        K.finder_mode = 0;
        if (!finder_open(mode)) finder_none();
    }
}

static void on_list(void *user, const int status, const char *body, const size_t length) {
    (void) user;
    (void) length;
    K.loading = 0;
    if (K.refetch) { // the proxy changed meanwhile: this answer took the old route
        K.refetch = 0;
        K.fetched_ms = -1e18;
        krunker_lobbies_refresh(0);
        if (K.loading) return;
    }
    // the body may be anything (an HTML block page, a proxy's error page): JSON with a games
    // array or nothing
    cJSON *res = status == 200 && body ? cJSON_Parse(body) : NULL;
    const cJSON *games = cJSON_GetObjectItem(res, "games");
    if (cJSON_IsArray(games)) {
        const int n = cJSON_GetArraySize(games);
        const cJSON **rows = malloc(sizeof(*rows) * (size_t) (n > 0 ? n : 1));
        int count = 0;
        cJSON *built = cJSON_CreateArray();
        const cJSON *g;
        cJSON_ArrayForEach(g, games) {
            cJSON *t = to_tuple(g);
            if (t && rows) {
                cJSON_AddItemToArray(built, t);
                rows[count++] = t;
            } else {
                cJSON_Delete(t);
            }
        }
        // krunker.io's own order, as v10 keeps it (serverListData: the browser's region groups come in
        // the order their first lobby does); only an oversized list is cut to its busiest lobbies
        cJSON *list = cJSON_CreateArray();
        if (rows) {
            if (count > KRUNKER_LIST_MAX) {
                qsort(rows, (size_t) count, sizeof(*rows), cmp_players);
                count = KRUNKER_LIST_MAX;
            }
            for (int i = 0; i < count; i++) cJSON_AddItemToArray(list, cJSON_Duplicate(rows[i], 1));
        }
        free(rows);
        cJSON_Delete(built);
        cJSON_Delete(K.list);
        K.list = list;
        hide_tls_failed(); // the last known pings: a refresh does not show a TLS-failed region's lobbies for a moment
        const cJSON *total = cJSON_GetObjectItem(res, "totalPlayerCount");
        K.players = cJSON_IsNumber(total) ? total->valuedouble : 0;
        K.answered_ms = now_ms();
        K.error[0] = 0;
        krunker_lobbies_measure_pings();
        char proxy[160];
        const int proxied = via_proxy(proxy, sizeof(proxy));
        printf("[krunker] %d official lobbies (%.0f players) from %s%s%s\n", cJSON_GetArraySize(K.list), K.players, matchmaker_url(),
               proxied ? " via proxy " : "", proxied ? proxy : "");
    } else {
        char proxy[160];
        const int proxied = via_proxy(proxy, sizeof(proxy));
        if (status && access_denied(status, body)) {
            if (proxied)
                i18n_trf(K.error, sizeof(K.error), "kn.kio.deniedProxy",
                         "krunker.io refused this network (ACCESS DENIED), also through the proxy {0}. Try another proxy in Settings > Client.",
                         1, I18N_ARGV(proxy));
            else
                snprintf(K.error, sizeof(K.error), "%s", I18N("kn.kio.denied", "krunker.io refused this network (ACCESS DENIED). Set a proxy in Settings > Client."));
        } else if (status) {
            char code[16];
            snprintf(code, sizeof(code), "%d", status);
            if (status == 200)
                snprintf(K.error, sizeof(K.error), "%s", I18N("kn.kio.notAList", "krunker.io servers unavailable: krunker.io answered with something else than the lobby list."));
            else
                i18n_trf(K.error, sizeof(K.error), "kn.kio.http", "krunker.io servers unavailable: krunker.io answered HTTP {0}.", 1, I18N_ARGV(code));
        } else if (proxied) {
            i18n_trf(K.error, sizeof(K.error), "kn.kio.proxyUnreachable", "krunker.io servers unavailable: the proxy {0} did not reach krunker.io (Settings > Client).",
                     1, I18N_ARGV(proxy));
        } else {
            snprintf(K.error, sizeof(K.error), "%s", I18N("kn.kio.unreachable", "krunker.io servers unavailable: krunker.io is not reachable."));
        }
        fprintf(stderr, "[krunker] game list failed (HTTP %d%s%s): %s\n", status, proxied ? " via proxy " : "", proxied ? proxy : "", K.error);
    }
    cJSON_Delete(res);
    if (!K.pinging) run_waiting(); // else on_pings (or the ping list's failure) runs them on the settled list
    menu_rerender_if_open(2); // the Servers window shows the section
    v10_menu_popular_changed(); // Popular Now counts krunker.io's custom games (v10 fetchMatches)
}

void krunker_lobbies_refresh(const int force) {
    if (!krunker_lobbies_enabled() || K.loading || K.fixed) return;
    const double now = now_ms(), age = now - K.fetched_ms;
    const double wait = K.error[0] ? KRUNKER_RETRY_MS : force ? KRUNKER_MIN_INTERVAL_MS : KRUNKER_LOBBIES_CACHE_MS;
    if (age < wait) return;
    char url[512];
    snprintf(url, sizeof(url), "%s/game-list?hostname=krunker.io", matchmaker_url());
    K.fetched_ms = now;
    K.loading = backend_http_get_external(url, KRUNKER_LOBBIES_TIMEOUT_MS, on_list, NULL) != 0;
    if (!K.loading) snprintf(K.error, sizeof(K.error), "%s", I18N("kn.kio.unreachable", "krunker.io servers unavailable: krunker.io is not reachable."));
}

void krunker_lobbies_set_fixed(const cJSON *answer) {
    cJSON_Delete(K.list);
    K.list = NULL;
    K.error[0] = 0;
    K.fixed = answer != NULL;
    if (!answer) return;
    const cJSON *err = cJSON_GetObjectItem(answer, "error"); // the section's failure sentence instead
    if (cJSON_IsString(err)) {
        snprintf(K.error, sizeof(K.error), "%s", err->valuestring);
        return;
    }
    char *body = cJSON_PrintUnformatted(answer);
    on_list(NULL, 200, body, body ? strlen(body) : 0);
    free(body);
}

void krunker_lobbies_network_changed(void) {
    if (K.loading) {
        K.refetch = 1;
        return;
    }
    K.fetched_ms = -1e18; // the next refresh asks at once (also within the retry wait)
    if (menu_window_is_open() && menu_ui()->active_window == 2) {
        krunker_lobbies_refresh(0);
        menu_rerender_if_open(2); // "Loading..."
    }
}

const cJSON *krunker_lobbies_list(void) { return krunker_lobbies_enabled() ? K.list : NULL; }

const cJSON *krunker_lobbies_find(const char *id) {
    const cJSON *t;
    if (!id || !krunker_lobbies_enabled()) return NULL;
    cJSON_ArrayForEach(t, K.list) {
        const char *tid = cJSON_GetStringValue(cJSON_GetArrayItem(t, 0));
        if (tid && !strcmp(tid, id)) return t;
    }
    return NULL;
}
int krunker_lobbies_loading(void) { return K.loading; }
const char *krunker_lobbies_error(void) { return K.error; }
double krunker_lobbies_players(void) { return K.players; }

// a meta flag Krunker sends as true or 1
static int meta_on(const cJSON *meta, const char *key) {
    const cJSON *v = cJSON_GetObjectItem(meta, key);
    return cJSON_IsTrue(v) || (cJSON_IsNumber(v) && v->valuedouble != 0);
}

// ---------------------------------------------------------------- the seek's region (v10 getRegion)

static const char *const k_available[] = {KRUNKER_REGIONS};
static const char *const k_dedicated_only[] = {KRUNKER_DEDICATED_ONLY_REGIONS};
#define KRUNKER_PING_CACHE_KEY "knKrunkerPingRegion" // v10's PING_REGION_CACHE_KEY ("pingRegion7") for krunker.io's
                                                     // regions: KrunkNative's pingRegion7 holds the brain's (servers_ui.c)
#define KRUNKER_PING_RETRY_MS 60000.0                // after every ping failed: the fallback region until then
#define KRUNKER_REGION_WAITERS 8

static struct {
    int racing;          // /ping-list or the race in flight
    double failed_ms;    // every ping failed at (now_ms), -inf = never
    struct {
        KrunkerRegionFn fn;
        void *user;
    } waiters[KRUNKER_REGION_WAITERS];
    int waiter_count;
} R = {.failed_ms = -1e18};

static int available_region(const char *region) {
    int ok = 0;
    for (size_t i = 0; region && i < sizeof(k_available) / sizeof(k_available[0]); i++) ok |= !strcmp(region, k_available[i]);
    for (size_t i = 0; ok && i < sizeof(k_dedicated_only) / sizeof(k_dedicated_only[0]); i++) ok &= strcmp(region, k_dedicated_only[i]) != 0;
    return ok;
}

const char *krunker_seek_region_auto(void) {
    static char cached[64];
    if (html_storage_get(KRUNKER_PING_CACHE_KEY, cached, sizeof(cached)) < 0 || !available_region(cached)) cached[0] = 0;
    return cached;
}

const char *krunker_seek_region(void) {
    const char *env = getenv("KRUNKNATIVE_KRUNKER_REGION"); // v10's __REGION__
    if (env && env[0]) return env;
    if (seek_prefs_krunker_region()[0]) return seek_prefs_krunker_region(); // Settings > Quickplay > krunker.io Region
    if (krunker_seek_region_auto()[0]) return krunker_seek_region_auto();
    // every ping failed lately: v10's seek errors out ("All pings failed."); KrunkNative seeks in the
    // list's first region (the matchmaker's own default) until the race may run again
    if (now_ms() - R.failed_ms < KRUNKER_PING_RETRY_MS) return k_available[0];
    return "";
}

static void region_resolved(const char *region) {
    R.racing = 0;
    const int count = R.waiter_count;
    R.waiter_count = 0;
    for (int i = 0; i < count; i++) R.waiters[i].fn(R.waiters[i].user, region);
}

static void on_race(void *user, const char *region) {
    (void) user;
    if (region) {
        html_storage_set(KRUNKER_PING_CACHE_KEY, region); // ClientManager.setDefaultRegion
        printf("[krunker] seek region: %s (%s), the first to answer the ping race\n", region, krunker_region_name(region));
    } else {
        R.failed_ms = now_ms();
        printf("[krunker] seek region: every ping failed: %s for now\n", k_available[0]);
    }
    region_resolved(krunker_seek_region());
}

static void on_ping_list(void *user, const int status, const char *body, const size_t length) {
    (void) user;
    (void) length;
    cJSON *list = status == 200 && body ? cJSON_Parse(body) : NULL;
    if (!cJSON_IsObject(list) || !region_ping_race(list, available_region, on_race, NULL)) {
        printf("[krunker] seek region: no pingable region (ping-list HTTP %d)\n", status);
        on_race(NULL, NULL); // v10 "No pingable regions."
    }
    cJSON_Delete(list);
}

void krunker_seek_region_resolve(const KrunkerRegionFn fn, void *user) {
    if (!fn) return;
    if (krunker_seek_region()[0]) {
        fn(user, krunker_seek_region());
        return;
    }
    if (R.waiter_count < KRUNKER_REGION_WAITERS) {
        R.waiters[R.waiter_count].fn = fn;
        R.waiters[R.waiter_count++].user = user;
    }
    if (R.racing) return;
    R.racing = 1;
    char url[512];
    snprintf(url, sizeof(url), "%s/ping-list?hostname=krunker.io", matchmaker_url()); // fetchPingList (v10.js:147583-147588)
    printf("[krunker] seek region: none chosen or cached: racing the regions' pings\n");
    if (!backend_http_get_external(url, KRUNKER_LOBBIES_TIMEOUT_MS, on_ping_list, NULL)) on_race(NULL, NULL);
}

int krunker_lobbies_pick(char *id, const size_t n) {
    // the busiest match this client was not in lately; else the least recent of those it was in, the
    // game it is in last (recent_lobbies.h, krunker_net_recent_rank): Find New Game never lands back
    // in the game it leaves, nor bounces between the last two, while anything else has room
    // the seek's region first (krunker_seek_region: Settings > Quickplay > krunker.io Region, else the
    // ping race's, like v10's seek in its getRegion), never the busiest region the world over; any
    // region only when that one has nothing with room
    const char *want_region = krunker_seek_region();
    const cJSON *t, *best = NULL;
    int best_rank = 0;
    int best_players = -1;
    for (int pass = want_region[0] ? 0 : 1; pass < 2 && !best; pass++) cJSON_ArrayForEach(t, krunker_lobbies_list()) { // krunker.io's order
        const cJSON *meta = cJSON_GetArrayItem(t, 4);
        const char *region = cJSON_GetStringValue(cJSON_GetArrayItem(t, 1));
        if (!pass && (!region || strcmp(want_region, region))) continue;
        const int players = (int) cJSON_GetNumberValue(cJSON_GetArrayItem(t, 2));
        const int max = (int) cJSON_GetNumberValue(cJSON_GetArrayItem(t, 3));
        // krunker.io sends its flags as 1 (c: 403 of 403 custom games in a live list): a custom game or a
        // password lobby is never a seek's pick; the mode and map must be switched on in Settings > Quickplay
        // (the public rotation by default: seek_prefs_lobby_ok, no official custom or dedicated lobby)
        if (players >= max) continue;
        const unsigned flags = (meta_on(meta, "cs") ? SEEK_LOBBY_CUSTOM : 0u) | (meta_on(meta, "pw") ? SEEK_LOBBY_PASSWORD : 0u) |
                               (meta_on(meta, "oc") || meta_on(meta, "ds") ? SEEK_LOBBY_HOSTED : 0u);
        const cJSON *mode = cJSON_GetObjectItem(meta, "mode");
        if (!seek_prefs_lobby_ok(cJSON_IsNumber(mode) ? mode->valueint : -1, cJSON_GetStringValue(cJSON_GetObjectItem(meta, "i")), flags)) continue;
        const int rank = krunker_net_recent_rank(cJSON_GetStringValue(cJSON_GetArrayItem(t, 0)));
        // not recent (0) beats any recent one, the most players among those; among recent ones the
        // older (higher rank) wins
        if (best && (rank ? !best_rank || rank <= best_rank : !best_rank && players <= best_players)) continue;
        best = t;
        best_rank = rank;
        best_players = players;
    }
    if (!best) return 0;
    snprintf(id, n, "%s", cJSON_GetStringValue(cJSON_GetArrayItem(best, 0)));
    return 1;
}

int krunker_lobbies_pick_custom(const char *map, char *id, const size_t n) {
    const cJSON *t, *best = NULL;
    int best_players = -1;
    cJSON_ArrayForEach(t, krunker_lobbies_list()) {
        const cJSON *meta = cJSON_GetArrayItem(t, 4);
        const char *i = cJSON_GetStringValue(cJSON_GetObjectItem(meta, "i"));
        const int players = (int) cJSON_GetNumberValue(cJSON_GetArrayItem(t, 2));
        const int max = (int) cJSON_GetNumberValue(cJSON_GetArrayItem(t, 3));
        if (!map || !i || strcmp(i, map) || !meta_on(meta, "cs")) continue; // a custom game on that map
        if (players >= max || meta_on(meta, "pw")) continue;
        if (players > best_players) {
            best = t;
            best_players = players;
        }
    }
    if (!best) return 0;
    snprintf(id, n, "%s", cJSON_GetStringValue(cJSON_GetArrayItem(best, 0)));
    return 1;
}

// Find Game's Raids / Zombies / Trade Plaza (openServerWindow 2-4, v10.js:218379-218415): the official
// customs of that mode with room, the krunker.io Region first, then the busiest (v10 sorts its
// fetchMatches answer by clients, the default region first); the first one is joined. 1 = found (joined),
// 0 = no official custom of that mode is listed.
#define FINDER_TOOK_LONG_MS 2000.0 // v10: setTimeout(..., 0x7d0) "Took to long to find" after the join

// v10's 2 s popup: the join did not take the player into the game (on the site the page would have
// navigated away by now, so the timer never showed there): krunker_net_active is the join under way')
static void finder_took_long(void *user) {
    (void) user;
    if (krunker_net_active()) return; // the join is under way or in the game (the navigation of v10's page)
    printf("[krunker] find game: took too long to find (the join did not start)\n");
    ui_error_popup(I18N("kn.finder.tookLong", "Took to long to find"));
}

static void finder_none(void) {
    printf("[krunker] find game: no official custom of that mode is listed: Couldnt find any\n");
    ui_loadout_clear_pops();
    ui_error_popup(I18N("kn.finder.none", "Couldnt find any")); // v10: iïîïiïì(null, "Couldnt find any")
}

static int finder_open(const int mode) {
    char id[80];
    const char *want_region = krunker_seek_region();
    const cJSON *t, *best = NULL;
    int best_players = -1, best_in_region = 0;
    cJSON_ArrayForEach(t, K.list) {
        const cJSON *meta = cJSON_GetArrayItem(t, 4);
        const int players = (int) cJSON_GetNumberValue(cJSON_GetArrayItem(t, 2));
        const int max = (int) cJSON_GetNumberValue(cJSON_GetArrayItem(t, 3));
        const cJSON *g = cJSON_GetObjectItem(meta, "mode");
        if (!meta_on(meta, "oc") || !cJSON_IsNumber(g) || g->valueint != mode) continue; // an official custom of that mode
        if (players >= max || meta_on(meta, "pw")) continue;                          // with room, no password
        const char *region = cJSON_GetStringValue(cJSON_GetArrayItem(t, 1));
        const int in_region = want_region[0] && region && !strcmp(region, want_region);
        if (best && in_region != best_in_region) {
            if (!in_region) continue; // the region's lobby beats any other one
        } else if (best && players <= best_players) {
            continue;
        }
        best = t;
        best_players = players;
        best_in_region = in_region;
    }
    if (!best) return 0;
    snprintf(id, sizeof(id), "%s", cJSON_GetStringValue(cJSON_GetArrayItem(best, 0)));
    printf("[krunker] find game: official custom %s (mode %d)\n", id, mode);
    ui_loadout_clear_pops(); // the Searching popup gives way to the join
    krunker_lobbies_open(id);
    html_timer_add(FINDER_TOOK_LONG_MS, finder_took_long, NULL);
    return 1;
}

void krunker_lobbies_find_custom(const int mode) {
    if (!krunker_lobbies_enabled()) {
        finder_none();
        return;
    }
    if (K.pinging) { // the list's pings are out: Find Game reads the list they settle (run_waiting)
        K.finder_mode = mode;
        return;
    }
    if (now_ms() - K.answered_ms >= KRUNKER_LOBBIES_CACHE_MS || !K.list) {
        // v10 asks the list again for the finder (fetchGameList): the fresh answer decides (on_list)
        K.finder_mode = mode;
        krunker_lobbies_refresh(1);
        if (K.loading) return; // the Searching popup stays until the answer
        K.finder_mode = 0;
        if (!K.list) {
            finder_none();
            return;
        }
    }
    if (!finder_open(mode)) finder_none();
}

static void on_seek_region(void *user, const char *region) {
    (void) user;
    (void) region;
    krunker_lobbies_seek(); // the region is known (or the fallback) now
}

int krunker_lobbies_seek(void) {
    if (!krunker_lobbies_enabled()) return 0;
    if (!krunker_seek_region()[0]) {
        // v10's seek asks getRegion first: the ping race, then this seek again (on_seek_region). The
        // wait shows the loading spinner, not a toast (no toast in the join flow)
        if (!R.racing && g_lobbies_client && !g_lobbies_client->net_connected && !g_lobbies_client->offline && !menu_connecting()) menu_show_connecting_map(NULL);
        krunker_seek_region_resolve(on_seek_region, NULL);
        return 1;
    }
    if (K.pinging) {
        // the list's pings are out: the lobbies of a region whose TLS failed leave the list when they answer, so the
        // seek waits for that (on_pings runs this again); the spinner, as while the list loads
        K.seek_pending = 1;
        if (g_lobbies_client && !g_lobbies_client->net_connected && !g_lobbies_client->offline && !menu_connecting()) menu_show_connecting_map(NULL);
        return 1;
    }
    if (now_ms() - K.answered_ms >= KRUNKER_LOBBIES_CACHE_MS || !K.list) {
        // a stale list: the fresh one decides (on_list runs this again)
        K.seek_pending = 1;
        krunker_lobbies_refresh(1);
        if (K.loading) {
            // v10's loading state while the list comes (no toast in the join flow): the spinner, unless
            // a game is still up (the join that follows shows it: krunker_net.c join)
            printf("[krunker] seek: looking for a krunker.io server (the lobby list)\n");
            if (g_lobbies_client && !g_lobbies_client->net_connected && !g_lobbies_client->offline && !menu_connecting()) menu_show_connecting_map(NULL);
            return 1;
        }
        K.seek_pending = 0;
        if (!K.list) {
            // no lobby list (krunker.io refused or dropped GET /game-list; this one retries only after
            // KRUNKER_RETRY_MS): in-client, krunker_net_seek_new asks for the list once more itself, else
            // the matchmaker picks, as krunker.io's own client lets it (excludeGame)
            if (krunker_net_in_client() && g_lobbies_client) {
                printf("[krunker] seek: no lobby list (%s): Find New Game through the matchmaker\n", K.error[0] ? K.error : "none");
                menu_show_window(MENU_WINDOW_NONE, 0);
                krunker_net_seek_new(g_lobbies_client, NULL, 1);
                return 1;
            }
            // a failure, not news: the Lost Connection screen (servers_ui.c), the error popup in a game
            servers_ui_seek_failed(K.error[0] ? K.error : I18N("kn.kio.notReachableNow", "krunker.io is not reachable right now"));
            return 1;
        }
    }
    char id[80], want[128], text[256];
    if (krunker_lobbies_pick(id, sizeof(id))) {
        if (!strcmp(id, krunker_net_game())) {
            // the only official lobby with room is the one the player is in: stay, no rejoin. v10's Find New
            // Game reloads the page into whatever the matchmaker answers (its excludeGame fallback: this
            // same game) and says nothing: the player stays in it here, the log alone says why
            seek_prefs_describe(want, sizeof(want));
            printf("[krunker] seek: no other official lobby has room for %s, staying in %s\n", want, id);
            return 1;
        }
        printf("[krunker] seek: official lobby %s%s\n", id, krunker_net_recent_rank(id) ? " (a recent one: nothing else has room)" : "");
        krunker_lobbies_open(id);
    } else {
        // v10's text for a search that matched no game (matchmaker.NoMatchingServers), as a failure
        seek_prefs_describe(want, sizeof(want));
        printf("[krunker] seek: No official krunker.io lobby has room for %s\n", want);
        join_error_text("NoMatchingServers", text, sizeof(text));
        servers_ui_seek_failed(text);
    }
    return 1;
}

// The lobby's region (tuple [1]) from the current list; a pasted link to a lobby that is not listed
// (no list yet, a custom or private game) the region its id prefix names ("NY:" us-nj), so the seek
// and the error screen's Region line are the lobby's, not the saved / default one. "" when neither.
static const char *lobby_region(const char *id) {
    const cJSON *t;
    cJSON_ArrayForEach(t, K.list) {
        const char *tid = cJSON_GetStringValue(cJSON_GetArrayItem(t, 0));
        if (tid && !strcmp(tid, id)) return cJSON_GetStringValue(cJSON_GetArrayItem(t, 1));
    }
    return kn_krunker_game_region(id);
}

void krunker_lobbies_open(const char *id) {
    if (!id_ok(id)) return;
    // KN_KRUNKER_OFFICIAL (krunker_net.h): joined in this client unless the browser opt-out is set.
    // A join that cannot start says why itself (krunker_net.c: the error screen or v10's error popup),
    // so it never falls back to the browser behind the player's back. One that starts shows no toast:
    // the loading screen says it.
    if (krunker_net_in_client() && g_lobbies_client) {
        printf("[krunker] official lobby %s: joining in-client\n", id);
        menu_show_window(MENU_WINDOW_NONE, 0); // like a KrunkNative row: the Servers window makes way for the game
        // v10's loading screen, as for one of ours (servers_ui.c): the ring with "Loading <map> ..." for a
        // listed lobby (Krunker's own map name), the ring alone for a link, until its INIT or the error
        // screen. From the offline game no transport reports a switch, so nothing else raises it.
        const cJSON *listed = krunker_lobbies_find(id);
        menu_show_connecting_map(listed ? cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(listed, 4), "i")) : NULL);
        krunker_net_join(g_lobbies_client, id, lobby_region(id));
        return;
    }
    const char *site = getenv("KRUNKNATIVE_KRUNKER_URL");
    char url[256], text[256];
    snprintf(url, sizeof(url), "%s/?game=%s", site && site[0] ? site : KRUNKER_DEFAULT_SITE, id);
    printf("[krunker] official lobby %s: opening %s (in the browser: KRUNKNATIVE_KRUNKER_JOIN=browser or Windows)\n", id, url);
    // the opt-out: krunker.io's own page joins it (ui_open_browser, never the in-client routing of
    // krunker.io links)
    ui_open_browser(url);
#ifdef _WIN32 // no native WebSocket client for krunker.io's game servers on Windows yet (krunker_net.c)
    I18N_F(text, "kn.kio.openedInBrowserWindows", "krunker.io server {0} opened in your browser (this Windows build joins krunker.io servers there for now)", id);
#else
    I18N_F(text, "kn.kio.openedInBrowserOptOut", "krunker.io server {0} opened in your browser (in-client joining is switched off: KRUNKNATIVE_KRUNKER_JOIN=browser)", id);
#endif
    ui_notify(text);
}

#else // ---------------------------------------------------------------- web: none of it

int krunker_lobbies_enabled(void) { return 0; }
void krunker_lobbies_find_custom(const int mode) {
    (void) mode;
}
int krunker_lobbies_region_ping_ms(const char *region, double *ms) {
    (void) region;
    (void) ms;
    return 0;
}
void krunker_lobbies_refresh(const int force) { (void) force; }
const cJSON *krunker_lobbies_list(void) { return NULL; }
const cJSON *krunker_lobbies_find(const char *id) {
    (void) id;
    return NULL;
}
int krunker_lobbies_loading(void) { return 0; }
const char *krunker_lobbies_error(void) { return ""; }
void krunker_lobbies_network_changed(void) {}
void krunker_lobbies_set_fixed(const cJSON *answer) { (void) answer; }
double krunker_lobbies_players(void) { return 0; }
int krunker_lobbies_pick_custom(const char *map, char *id, const size_t n) {
    (void) map;
    (void) id;
    (void) n;
    return 0;
}
int krunker_lobbies_pick(char *id, const size_t n) {
    (void) id;
    (void) n;
    return 0;
}
int krunker_lobbies_seek(void) { return 0; }
const char *krunker_seek_region(void) { return ""; }
const char *krunker_seek_region_auto(void) { return ""; }
void krunker_seek_region_resolve(const KrunkerRegionFn fn, void *user) {
    if (fn) fn(user, "");
}
void krunker_lobbies_open(const char *id) { (void) id; }
void krunker_lobbies_attach(Client *client) { (void) client; }

#endif
