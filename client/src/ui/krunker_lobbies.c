// Official krunker.io lobbies (client/include/krunker_lobbies.h, doc/v10/matchmaker-token.md
// "Official lobbies"). Native only: platform behaviour, so the web build compiles the stubs at
// the end (#ifndef __EMSCRIPTEN__) and never lists or requests anything from krunker.io.
#include <join_errors.h>
#include <krunker_lobbies.h>
#include <krunker_net.h>
#include <seek_prefs.h>
#include <server_names.h>
#include <ui_logic.h>
#include <backend.h>
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
    cJSON *list;            // tuples (owned), most players first
    double players;         // totalPlayerCount
    double fetched_ms;      // the last request's start (glfwGetTime ms), -inf = never
    double answered_ms;     // the last good answer
    int loading;
    int seek_pending;       // krunker_lobbies_seek waits for the answer
    int refetch;            // the network changed while a request ran: ask again after it
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
    if (cJSON_IsNumber(left)) cJSON_AddNumberToObject(meta, "timeLeft", left->valuedouble);
    cJSON_AddItemToArray(t, meta);
    return t;
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
        cJSON *list = cJSON_CreateArray();
        if (rows) {
            qsort(rows, (size_t) count, sizeof(*rows), cmp_players);
            for (int i = 0; i < count && i < KRUNKER_LIST_MAX; i++) cJSON_AddItemToArray(list, cJSON_Duplicate(rows[i], 1));
        }
        free(rows);
        cJSON_Delete(built);
        cJSON_Delete(K.list);
        K.list = list;
        const cJSON *total = cJSON_GetObjectItem(res, "totalPlayerCount");
        K.players = cJSON_IsNumber(total) ? total->valuedouble : 0;
        K.answered_ms = now_ms();
        K.error[0] = 0;
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
    if (K.seek_pending) {
        K.seek_pending = 0;
        krunker_lobbies_seek();
    }
    menu_rerender_if_open(2); // the Servers window shows the section
}

void krunker_lobbies_refresh(const int force) {
    if (!krunker_lobbies_enabled() || K.loading) return;
    const double now = now_ms(), age = now - K.fetched_ms;
    const double wait = K.error[0] ? KRUNKER_RETRY_MS : force ? KRUNKER_MIN_INTERVAL_MS : KRUNKER_LOBBIES_CACHE_MS;
    if (age < wait) return;
    char url[512];
    snprintf(url, sizeof(url), "%s/game-list?hostname=krunker.io", matchmaker_url());
    K.fetched_ms = now;
    K.loading = backend_http_get_external(url, KRUNKER_LOBBIES_TIMEOUT_MS, on_list, NULL) != 0;
    if (!K.loading) snprintf(K.error, sizeof(K.error), "%s", I18N("kn.kio.unreachable", "krunker.io servers unavailable: krunker.io is not reachable."));
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

int krunker_lobbies_pick(char *id, const size_t n) {
    // the busiest match this client was not in lately; else the least recent of those it was in, the
    // game it is in last (recent_lobbies.h, krunker_net_recent_rank): Find New Game never lands back
    // in the game it leaves, nor bounces between the last two, while anything else has room
    const cJSON *t, *best = NULL;
    int best_rank = 0;
    cJSON_ArrayForEach(t, krunker_lobbies_list()) { // most players first
        const cJSON *meta = cJSON_GetArrayItem(t, 4);
        const int players = (int) cJSON_GetNumberValue(cJSON_GetArrayItem(t, 2));
        const int max = (int) cJSON_GetNumberValue(cJSON_GetArrayItem(t, 3));
        if (players >= max || cJSON_IsTrue(cJSON_GetObjectItem(meta, "cs")) || cJSON_IsTrue(cJSON_GetObjectItem(meta, "pw"))) continue;
        const cJSON *mode = cJSON_GetObjectItem(meta, "mode");
        if (!seek_prefs_match(cJSON_IsNumber(mode) ? mode->valueint : -1, cJSON_GetStringValue(cJSON_GetObjectItem(meta, "i")))) continue;
        const int rank = krunker_net_recent_rank(cJSON_GetStringValue(cJSON_GetArrayItem(t, 0)));
        if (best && (!best_rank || (rank && rank <= best_rank))) continue;
        best = t;
        best_rank = rank;
        if (!rank) break;
    }
    if (!best) return 0;
    snprintf(id, n, "%s", cJSON_GetStringValue(cJSON_GetArrayItem(best, 0)));
    return 1;
}

int krunker_lobbies_seek(void) {
    if (!krunker_lobbies_enabled()) return 0;
    if (now_ms() - K.answered_ms >= KRUNKER_LOBBIES_CACHE_MS || !K.list) {
        // a stale list: the fresh one decides (on_list runs this again)
        K.seek_pending = 1;
        krunker_lobbies_refresh(1);
        if (K.loading) {
            ui_notify(I18N("kn.kio.looking", "Looking for a krunker.io server..."));
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
            // the only official lobby with room is the one the player is in: stay, no rejoin
            seek_prefs_describe(want, sizeof(want));
            I18N_F(text, "kn.kio.noOtherLobby", "No other krunker.io server has room for {0}: you stay in this one", want);
            printf("[krunker] seek: no other official lobby has room, staying in %s\n", id);
            ui_notify(text);
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
void krunker_lobbies_refresh(const int force) { (void) force; }
const cJSON *krunker_lobbies_list(void) { return NULL; }
const cJSON *krunker_lobbies_find(const char *id) {
    (void) id;
    return NULL;
}
int krunker_lobbies_loading(void) { return 0; }
const char *krunker_lobbies_error(void) { return ""; }
void krunker_lobbies_network_changed(void) {}
double krunker_lobbies_players(void) { return 0; }
int krunker_lobbies_pick(char *id, const size_t n) {
    (void) id;
    (void) n;
    return 0;
}
int krunker_lobbies_seek(void) { return 0; }
void krunker_lobbies_open(const char *id) { (void) id; }
void krunker_lobbies_attach(Client *client) { (void) client; }

#endif
