// krunker_host_url (client/src/net/krunker_account.c): the krunker.io hosts v10.0.2's client talks to.
// v10 has two API clients (v10:16439, 27594, 27600). The data client `_10444 = new _9997(_7283)`, _7283 = API_URI
// = https://api.krunker.io, serves the ranked profile (playerProfile), the boards, the match history, the ranked
// connection and the claim-milestone (doc/v10/ranked.md). The gapi client `_10449 = new _10077(_7284)`, _7284 =
// GAPI_URI = https://gapi.svc.krunker.io, serves the account services, the ranked shop and the purchase. A
// default Ranked popup on an official server reads the data client's paths: they must resolve to api.krunker.io
// with nothing set, not to gapi (the "Failed to load data" regression on official servers). The env overrides
// (KRUNKNATIVE_KRUNKER_DATA_API, KRUNKNATIVE_KRUNKER_API for a mirror) are checked too. Nothing is requested.
//   cmake-build-debug/krunker_host_test
#include "../src/net/krunker_account.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail, g_pass;
#define CHECK(cond, ...) do { if (cond) g_pass++; else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// ---- doubles: the account's storage, transfers, captcha, i18n and the level table (none is reached here)
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
    unsetenv("KRUNKNATIVE_KRUNKER_GAMEFRONTEND");
}

static void url_of(const KrunkerHost host, const char *path, char *out, const size_t n) { krunker_host_url(host, path, out, n); }

int main(void) {
    char url[512];
    const char *profile = "/player-profile/me/season/1/region/3";
    const char *shop = "/ranked/shop";

    // 1. nothing set: the data client is v10's API_URI, the gapi client is v10's GAPI_URI
    env_clear();
    url_of(KRUNKER_HOST_API, profile, url, sizeof(url));
    printf("default data (ranked profile): %s\n", url);
    CHECK(!strcmp(url, "https://api.krunker.io/player-profile/me/season/1/region/3"),
          "the ranked profile resolves to %s (v10's data client is api.krunker.io)", url);
    url_of(KRUNKER_HOST_API, "/ranked/me/connection", url, sizeof(url));
    CHECK(!strcmp(url, "https://api.krunker.io/ranked/me/connection"), "the ranked connection resolves to %s", url);
    url_of(KRUNKER_HOST_GAPI, shop, url, sizeof(url));
    printf("default gapi (ranked shop): %s\n", url);
    CHECK(!strcmp(url, "https://gapi.svc.krunker.io/ranked/shop"), "the ranked shop resolves to %s (v10's gapi client)", url);

    // 2. the account services keep gapi (krunker_service_url's default, v10's gapi client for /player and /auth)
    krunker_service_url(KRUNKER_SVC_API, "/player/me", url, sizeof(url));
    printf("default account service: %s\n", url);
    CHECK(!strcmp(url, "https://gapi.svc.krunker.io/player/me"), "the account service resolves to %s", url);

    // 3. KRUNKNATIVE_KRUNKER_DATA_API moves the data client alone
    env_clear();
    setenv("KRUNKNATIVE_KRUNKER_DATA_API", "http://127.0.0.1:9001/api/", 1);
    setenv("KRUNKNATIVE_KRUNKER_API", "http://127.0.0.1:9002/gapi", 1);
    url_of(KRUNKER_HOST_API, profile, url, sizeof(url));
    CHECK(!strcmp(url, "http://127.0.0.1:9001/api/player-profile/me/season/1/region/3"),
          "a data override wins over the API override (trailing slash dropped): %s", url);
    url_of(KRUNKER_HOST_GAPI, shop, url, sizeof(url));
    CHECK(!strcmp(url, "https://gapi.svc.krunker.io/ranked/shop"), "the data override leaves the gapi client alone: %s", url);

    // 4. a mirror that sets only KRUNKNATIVE_KRUNKER_API serves the data client from it, as before
    env_clear();
    setenv("KRUNKNATIVE_KRUNKER_API", "http://127.0.0.1:9003/api", 1);
    url_of(KRUNKER_HOST_API, profile, url, sizeof(url));
    CHECK(!strcmp(url, "http://127.0.0.1:9003/api/player-profile/me/season/1/region/3"),
          "a mirror's API override serves the data client too: %s", url);
    env_clear();

    printf("krunker_host_test: %d/%d\n", g_pass, g_pass + g_fail);
    return g_fail ? 1 : 0;
}
