// krunker_kio_routes (client/src/net/krunker_kio_routes.c): the URL of each krunker.io read that v10.0.2 makes while
// krunker.io is the active account backend, built the way its caller builds it (krunker_host_url / krunker_service_url
// from krunker_account.c, included whole with the same doubles as krunker_host_test). The host of every URL is checked
// against v10's own: the Games window's maps and the hub's maps / mods / search and the streams on the data client
// (api.krunker.io), the footer's count on social.krunker.io, the leaderboards on gapi.svc.krunker.io. Nothing is requested.
//   cmake-build-debug/krunker_kio_routes_test
#include "../src/net/krunker_account.c"
#include "../src/net/krunker_kio_routes.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail, g_pass;
#define CHECK(cond, ...) do { if (cond) g_pass++; else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// ---- doubles: the same as krunker_host_test (krunker_account.c's storage, transfers, captcha and i18n are not reached)
int html_storage_get(const char *key, char *buf, size_t n) { (void) key; (void) buf; (void) n; return -1; }
void html_storage_set(const char *key, const char *value) { (void) key; (void) value; }
HttpStream *http_stream_start(const char *method, const char *url, const char *headers, const char *body,
                              const HttpStreamCallbacks *cb, void *user) {
    (void) method; (void) url; (void) headers; (void) body; (void) cb; (void) user;
    return NULL;
}
void http_stream_cancel(HttpStream *s) { (void) s; }
void http_stream_poll(void) {}
void krunker_captcha_ask(int attempt, int rejected, KrunkerCaptchaDone done, void *user) { (void) attempt; (void) rejected; (void) done; (void) user; }
void krunker_captcha_close(void) {}
int krunker_captcha_asking(void) { return 0; }
const char *i18n_tr(const char *key, const char *english) { (void) key; return english; }
const char *i18n_trf(char *out, size_t n, const char *key, const char *english, int argc, const char *const *argv) {
    (void) key; (void) argc; (void) argv;
    snprintf(out, n, "%s", english);
    return out;
}
int progression_level(double score, double rank_var) { (void) score; (void) rank_var; return 0; }

static void env_clear(void) {
    unsetenv("KRUNKNATIVE_KRUNKER_API");
    unsetenv("KRUNKNATIVE_KRUNKER_DATA_API");
    unsetenv("KRUNKNATIVE_KRUNKER_GAPI");
    unsetenv("KRUNKNATIVE_KRUNKER_SOCIAL");
    unsetenv("KRUNKNATIVE_KRUNKER_EDITOR");
}

// the host of a URL ("https://api.krunker.io/...") -> "api.krunker.io"
static void host_of(const char *url, char *host, const size_t n) {
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    size_t i = 0;
    while (p[i] && p[i] != '/' && p[i] != '?' && i + 1 < n) {
        host[i] = p[i];
        i++;
    }
    host[i] = 0;
}

// the bases as the callers get them: the data client (Games, hub, streams) and the social service (the footer)
static void data_base(char *out, const size_t n) { krunker_host_url(KRUNKER_HOST_API, "", out, n); }
static void social_base(char *out, const size_t n) { krunker_service_url(KRUNKER_SVC_SOCIAL, "", out, n); }

int main(void) {
    char base[256], url[640], query[320], host[128];

    // 1. the Games window's Community / All search: searchMap (v10:131908) on the data client
    env_clear();
    data_base(base, sizeof(base));
    CHECK(!strcmp(base, "https://api.krunker.io"), "the data client's base is %s (v10's API_URI)", base);
    CHECK(krunker_kio_search_query(query, sizeof(query), "kanji", -1) && !strcmp(query, "val=kanji&cat=-1"),
          "the search query is %s (cat -1 = All)", query);
    CHECK(krunker_kio_webhook_url(url, sizeof(url), base, "maps/search", query) &&
              !strcmp(url, "https://api.krunker.io/webhooks/general/maps/search?val=kanji&cat=-1"),
          "the Community search URL is %s", url);
    host_of(url, host, sizeof(host));
    CHECK(!strcmp(host, "api.krunker.io"), "the Community search is asked of %s, not kn_brain", host);
    CHECK(krunker_kio_search_query(query, sizeof(query), "a b&c", 3) && !strcmp(query, "val=a+b%26c&cat=3"),
          "a space is '+' and '&' is %%26 (URLSearchParams): %s", query);

    // 2. the Games window's tabs: listMaps (v10:131895), with the account id only once the game server gave it
    krunker_kio_webhook_url(url, sizeof(url), base, "maps/list", "index=3&pos=42&accountId=7");
    CHECK(!strcmp(url, "https://api.krunker.io/webhooks/general/maps/list?index=3&pos=42&accountId=7"), "the tab list URL is %s", url);
    krunker_kio_webhook_url(url, sizeof(url), base, "maps/list", "index=0&pos=0");
    CHECK(!strcmp(url, "https://api.krunker.io/webhooks/general/maps/list?index=0&pos=0"), "the tab list without an account id: %s", url);

    // 3. the hub's stand-in for the brain's /api routes: maps, mods and search (listMods, searchMod: v10:131922-131941)
    CHECK(!strcmp(krunker_kio_webhook_group("/maps", NULL), "maps/list"), "hub /maps is maps/list");
    CHECK(!strcmp(krunker_kio_webhook_group("/mods", NULL), "mods/list"), "hub /mods is mods/list");
    CHECK(!strcmp(krunker_kio_webhook_group("/search", "map"), "maps/search"), "hub /search type map is maps/search");
    CHECK(!strcmp(krunker_kio_webhook_group("/search", "mod"), "mods/search"), "hub /search type mod is mods/search");
    CHECK(krunker_kio_webhook_group("/search", "creator") == NULL, "an unknown search type is no webhook");
    CHECK(krunker_kio_webhook_group("/players", NULL) == NULL, "a brain-only hub route is no webhook");
    krunker_kio_webhook_url(url, sizeof(url), base, "mods/search", "val=gun&type=mod");
    host_of(url, host, sizeof(host));
    CHECK(!strcmp(host, "api.krunker.io") && !strcmp(url, "https://api.krunker.io/webhooks/general/mods/search?val=gun&type=mod"),
          "the hub's mod search: %s", url);

    // 4. the streams of the Home screen (v10 getTopStreams, v10:25991): the data client, not gapi (404 there)
    krunker_kio_streams_url(url, sizeof(url), base);
    host_of(url, host, sizeof(host));
    CHECK(!strcmp(url, "https://api.krunker.io/streams/top?cb=2") && !strcmp(host, "api.krunker.io"), "the streams URL is %s", url);

    // 5. the Servers footer's "on Social" count (v10 loadData, v10:211170): social.krunker.io/player-count
    social_base(base, sizeof(base));
    CHECK(!strcmp(base, "https://social.krunker.io/"), "krunker_service_url leaves its trailing slash: %s", base);
    krunker_kio_player_count_url(url, sizeof(url), base);
    host_of(url, host, sizeof(host));
    CHECK(!strcmp(url, "https://social.krunker.io/player-count") && !strcmp(host, "social.krunker.io"), "the player count URL is %s", url);
    // the editor's count (v10 loadData's second fetch, EDITOR_URI): editor.krunker.io/player-count
    char editor_base[256];
    krunker_service_url(KRUNKER_SVC_EDITOR, "", editor_base, sizeof(editor_base));
    krunker_kio_player_count_url(url, sizeof(url), editor_base);
    host_of(url, host, sizeof(host));
    CHECK(!strcmp(url, "https://editor.krunker.io/player-count") && !strcmp(host, "editor.krunker.io"), "the editor count URL is %s", url);

    // 6. the leaderboards (v10's leaderboards client, _10449 = gapi, v10:26987-27036): the gapi path, then gapi's host
    char gpath[320];
    CHECK(krunker_kio_gapi_leaderboard_path(gpath, sizeof(gpath), "/api/leaderboards/player_kills") != 0 &&
              !strcmp(gpath, "/leaderboards/player_kills"),
          "the players board's gapi path: %s", gpath);
    krunker_service_url(KRUNKER_SVC_API, gpath, url, sizeof(url)); // what v10_gapi_http sends (krunker_account_api)
    host_of(url, host, sizeof(host));
    CHECK(!strcmp(url, "https://gapi.svc.krunker.io/leaderboards/player_kills") && !strcmp(host, "gapi.svc.krunker.io"),
          "the players board is asked of %s", url);
    CHECK(krunker_kio_gapi_leaderboard_path(gpath, sizeof(gpath), "/api/leaderboards/raids/18/monthly?month=2026-10&sort=best_time") != 0 &&
              !strcmp(gpath, "/leaderboards/raids/18/monthly?month=2026-10&sort=best_time"),
          "a raid board keeps its query: %s", gpath);
    CHECK(krunker_kio_gapi_leaderboard_path(gpath, sizeof(gpath), "/mm/ranked/leaderboard?season=1") == 0 && gpath[0] == 0,
          "a ranked board of KN's brain has no gapi route (nothing is sent)");

    // 6b. Turf Wars and the clan reads: the brain's JSON paths map to v10's gapi paths (v10:26670-26806), the rest has none
    const struct {
        const char *kn, *gapi;
    } gapi_cases[] = {
        {"/api/territorial-wars/status", "/territorial-wars/status"},
        {"/api/territorial-wars/clan/42/players?day=3&map_id=2", "/territorial-wars/clan/42/players?day=3&map_id=2"},
        {"/api/territorial-wars/leaderboards/top-grossing-clans?clan_name=dev", "/territorial-wars/leaderboards/top-grossing-clans?clan_name=dev"},
        {"/api/territorial-wars/player/Some%20One/contributions?day=2", "/territorial-wars/player/Some%20One/contributions?day=2"},
        {"/api/clans/recruitment?page=1&limit=20&region=EU", "/clans/recruitment?page=1&limit=20&region=EU"},
        {"/api/clans/by-name/dev", "/clans/by-name/dev"},
        {"/api/clans/77/recruitment", "/clans/77/recruitment"},
        {"/api/leaderboards/krunkcup/teams/4/contributors", "/leaderboards/krunkcup/teams/4/contributors"},
    };
    for (size_t i = 0; i < sizeof(gapi_cases) / sizeof(gapi_cases[0]); i++) {
        CHECK(krunker_kio_gapi_path(gpath, sizeof(gpath), gapi_cases[i].kn) != 0 && !strcmp(gpath, gapi_cases[i].gapi),
              "%s maps to %s (got '%s')", gapi_cases[i].kn, gapi_cases[i].gapi, gpath);
    }
    // no route: the caller sends nothing. Box Office's my-applications and token reads, the clan writes, the clan's
    // non-recruitment reads with no gapi counterpart here, and anything with a token
    const char *no_route[] = {"/api/clans/my-applications?token=abc", "/api/clans/by-name/", "/api/clans/12/apply?token=x",
                              "/api/clans/recruitment2", "/api/clans/x/recruitment", "/api/territorial-wars/status?token=x",
                              "/api/shop/displays?tag=limited", "/mm/ranked/leaderboard?season=1", "/api/lottery/3", NULL};
    for (size_t i = 0; no_route[i]; i++) {
        const int got = krunker_kio_gapi_path(gpath, sizeof(gpath), no_route[i]);
        CHECK(got == 0 && gpath[0] == 0, "%s has no gapi route (got '%s')", no_route[i], gpath);
    }
    CHECK(krunker_kio_gapi_path(gpath, sizeof(gpath), NULL) == 0, "a NULL path has no gapi route");

    // 7. a mirror moves the data client: the webhooks and the streams follow KRUNKNATIVE_KRUNKER_DATA_API
    env_clear();
    setenv("KRUNKNATIVE_KRUNKER_DATA_API", "http://127.0.0.1:9101/api/", 1);
    data_base(base, sizeof(base));
    krunker_kio_webhook_url(url, sizeof(url), base, "maps/search", "val=x&cat=-1");
    CHECK(!strcmp(url, "http://127.0.0.1:9101/api/webhooks/general/maps/search?val=x&cat=-1"), "a data override moves the search: %s", url);
    env_clear();

    printf("krunker_kio_routes_test: %d/%d\n", g_pass, g_pass + g_fail);
    return g_fail ? 1 : 0;
}
