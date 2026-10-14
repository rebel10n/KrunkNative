// Official krunker.io lobbies (client/include/krunker_lobbies.h, doc/v10/matchmaker-token.md
// "Official lobbies"). Native only: platform behaviour, so the web build compiles the stubs at
// the end (#ifndef __EMSCRIPTEN__) and never lists or requests anything from krunker.io.
#include <krunker_lobbies.h>
#include <seek_prefs.h>
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
    char error[160];
} K = {.fetched_ms = -1e18, .answered_ms = -1e18};

static double now_ms(void) { return glfwGetTime() * 1000.0; }

static const char *matchmaker_url(void) {
    const char *url = getenv("KRUNKNATIVE_KRUNKER_MATCHMAKER");
    return url && url[0] ? url : KRUNKER_DEFAULT_MATCHMAKER;
}

int krunker_lobbies_enabled(void) {
    const char *url = matchmaker_url();
    return strcmp(url, "off") != 0 && strcmp(url, "0") != 0;
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
    cJSON *res = status == 200 ? cJSON_Parse(body) : NULL;
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
        printf("[krunker] %d official lobbies (%.0f players) from %s\n", cJSON_GetArraySize(K.list), K.players, matchmaker_url());
    } else {
        snprintf(K.error, sizeof(K.error), "%s", status ? "krunker.io answered with an error" : "krunker.io is not reachable");
        fprintf(stderr, "[krunker] game list failed (HTTP %d)\n", status);
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
    if (!K.loading) snprintf(K.error, sizeof(K.error), "krunker.io is not reachable");
}

const cJSON *krunker_lobbies_list(void) { return krunker_lobbies_enabled() ? K.list : NULL; }
int krunker_lobbies_loading(void) { return K.loading; }
const char *krunker_lobbies_error(void) { return K.error; }
double krunker_lobbies_players(void) { return K.players; }

int krunker_lobbies_pick(char *id, const size_t n) {
    const cJSON *t;
    cJSON_ArrayForEach(t, krunker_lobbies_list()) { // most players first
        const cJSON *meta = cJSON_GetArrayItem(t, 4);
        const int players = (int) cJSON_GetNumberValue(cJSON_GetArrayItem(t, 2));
        const int max = (int) cJSON_GetNumberValue(cJSON_GetArrayItem(t, 3));
        if (players >= max || cJSON_IsTrue(cJSON_GetObjectItem(meta, "cs")) || cJSON_IsTrue(cJSON_GetObjectItem(meta, "pw"))) continue;
        const cJSON *mode = cJSON_GetObjectItem(meta, "mode");
        if (!seek_prefs_match(cJSON_IsNumber(mode) ? mode->valueint : -1, cJSON_GetStringValue(cJSON_GetObjectItem(meta, "i")))) continue;
        snprintf(id, n, "%s", cJSON_GetStringValue(cJSON_GetArrayItem(t, 0)));
        return 1;
    }
    return 0;
}

int krunker_lobbies_seek(void) {
    if (!krunker_lobbies_enabled()) return 0;
    if (now_ms() - K.answered_ms >= KRUNKER_LOBBIES_CACHE_MS || !K.list) {
        // a stale list: the fresh one decides (on_list runs this again)
        K.seek_pending = 1;
        krunker_lobbies_refresh(1);
        if (K.loading) {
            ui_notify("Looking for an official krunker.io lobby...");
            return 1;
        }
        K.seek_pending = 0;
        if (!K.list) {
            ui_notify("krunker.io is not reachable right now");
            return 1;
        }
    }
    char id[80], want[128], text[256];
    if (krunker_lobbies_pick(id, sizeof(id))) {
        krunker_lobbies_open(id);
    } else {
        seek_prefs_describe(want, sizeof(want));
        snprintf(text, sizeof(text), "No official krunker.io lobby has room for %s", want);
        printf("[krunker] seek: %s\n", text);
        ui_notify(text);
    }
    return 1;
}

void krunker_lobbies_open(const char *id) {
    if (!id_ok(id)) return;
    const char *site = getenv("KRUNKNATIVE_KRUNKER_URL");
    char url[256], text[256];
    snprintf(url, sizeof(url), "%s/?game=%s", site && site[0] ? site : KRUNKER_DEFAULT_SITE, id);
    printf("[krunker] official lobby %s: opening %s\n", id, url);
    // Krunker's protocol is not this client's: the browser joins it (ui_open_browser, never the
    // in-client routing of krunker.io links)
    ui_open_browser(url);
    snprintf(text, sizeof(text), "Official lobby %s opened in your browser (krunker.io games can't be joined in this client)", id);
    ui_notify(text);
}

#else // ---------------------------------------------------------------- web: none of it

int krunker_lobbies_enabled(void) { return 0; }
void krunker_lobbies_refresh(const int force) { (void) force; }
const cJSON *krunker_lobbies_list(void) { return NULL; }
int krunker_lobbies_loading(void) { return 0; }
const char *krunker_lobbies_error(void) { return ""; }
double krunker_lobbies_players(void) { return 0; }
int krunker_lobbies_pick(char *id, const size_t n) {
    (void) id;
    (void) n;
    return 0;
}
int krunker_lobbies_seek(void) { return 0; }
void krunker_lobbies_open(const char *id) { (void) id; }

#endif
