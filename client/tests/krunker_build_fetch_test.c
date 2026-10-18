// client/src/net/krunker_build_fetch.c: the live Krunker build an official join speaks, with a fake HTTP
// layer (answers by URL, synchronously): the published build is fetched, hash-checked, cached and made
// current; a failed fetch falls back to the cached build, then to the compiled one; a build this client
// cannot follow is refused with its reason; the TTL memo and the cached copy save requests.
//   cmake-build-debug/krunker_build_fetch_test
#include <krunker_build_fetch.h>
#include <krunker_build_live.h>
#include <auth_join.h>
#include <cJSON.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// setenv for MinGW, whose CRT has only _putenv_s (the same guard as client/src/headless.c).
static int test_setenv(const char *name, const char *value, const int overwrite) {
#ifdef _WIN32
    if (!overwrite && getenv(name)) return 0;
    return _putenv_s(name, value);
#else
    return setenv(name, value, overwrite);
#endif
}

#ifndef KN_SOURCE_DIR
#define KN_SOURCE_DIR "."
#endif

static int g_fail, g_pass;
#define CHECK(cond, ...) do { if (cond) g_pass++; else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define MANIFEST_URL "http://stub.test/updates/client/krunker-build.json"

// ---- the fake HTTP: one answer per URL
static struct {
    char url[256];
    int status;
    char *body;
} g_routes[8];
static int g_route_count, g_requests;
static char g_last_url[256];

static void route(const char *url, const int status, const char *body) {
    for (int i = 0; i < g_route_count; i++) {
        if (strcmp(g_routes[i].url, url)) continue;
        free(g_routes[i].body);
        g_routes[i].status = status;
        g_routes[i].body = body ? strdup(body) : NULL;
        return;
    }
    snprintf(g_routes[g_route_count].url, sizeof(g_routes[0].url), "%s", url);
    g_routes[g_route_count].status = status;
    g_routes[g_route_count].body = body ? strdup(body) : NULL;
    g_route_count++;
}

static int fake_get(const char *url, const KrunkerBuildHttpDone done, void *ctx) {
    g_requests++;
    snprintf(g_last_url, sizeof(g_last_url), "%s", url);
    for (int i = 0; i < g_route_count; i++) {
        if (!strcmp(g_routes[i].url, url)) {
            done(ctx, g_routes[i].status, g_routes[i].body, g_routes[i].body ? strlen(g_routes[i].body) : 0);
            return 1;
        }
    }
    done(ctx, 404, "not found", 9);
    return 1;
}

// ---- outcomes
static int g_done;
static KrunkerBuildSource g_src;
static char g_cv[64], g_why[300];
static void ready(void *user, const KrunkerBuildSource src, const char *client_version, const char *why) {
    (void) user;
    g_done++;
    g_src = src;
    snprintf(g_cv, sizeof(g_cv), "%s", client_version ? client_version : "");
    snprintf(g_why, sizeof(g_why), "%s", why ? why : "");
}
static void refresh(const char *url, const int force) {
    g_done = 0;
    g_requests = 0;
    krunker_build_refresh(url, force, ready, NULL);
}

// ---- documents: tools/krunker/build.json as a new Krunker build would change it
static cJSON *doc_base(void) {
    FILE *f = fopen(KN_SOURCE_DIR "/tools/krunker/build.json", "rb");
    if (!f) return NULL;
    static char text[1 << 20];
    const size_t n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    return cJSON_ParseWithLength(text, n);
}
static char *doc_text(const char *build_version, const char *client_version, const int ahkey, const int break_sid) {
    cJSON *d = doc_base();
    if (!d) return NULL;
    cJSON_ReplaceItemInObject(d, "build_version", cJSON_CreateString(build_version));
    cJSON_ReplaceItemInObject(d, "client_version", cJSON_CreateString(client_version));
    cJSON_ReplaceItemInObject(d, "ahkey", cJSON_CreateNumber(ahkey));
    if (break_sid) { // ADD_PLAYERS lost "sid" (renamed): nothing this client can follow
        cJSON *f;
        cJSON_ArrayForEach(f, cJSON_GetObjectItem(cJSON_GetObjectItem(cJSON_GetObjectItem(d, "layouts"), "add_players"), "fields")) {
            if (!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(f, "name")), "sid"))
                cJSON_ReplaceItemInObject(f, "name", cJSON_CreateString("serverId"));
        }
    }
    char *out = cJSON_PrintUnformatted(d);
    cJSON_Delete(d);
    return out;
}
// publish doc under its sha256 (or a wrong one) with a manifest naming it; returns the sha
static void publish(const char *doc, const char *build_version, const char *client_version, const int wrong_sha, char sha[65]) {
    uint8_t dig[32];
    kn_sha256_bytes(doc, strlen(doc), dig);
    kn_sha256_hex(dig, sha);
    char named[65];
    snprintf(named, sizeof(named), "%s", sha);
    if (wrong_sha) named[0] = named[0] == 'a' ? 'b' : 'a';
    char manifest[600], url[256];
    snprintf(manifest, sizeof(manifest),
             "{\"schema\":1,\"build_version\":\"%s\",\"client_version\":\"%s\",\"sha256\":\"%s\",\"path\":\"krunker/%s.json\","
             "\"published\":\"2026-10-15T12:00:00Z\"}",
             build_version, client_version, named, named);
    snprintf(url, sizeof(url), "http://stub.test/updates/krunker/%s.json", named);
    route(MANIFEST_URL, 200, manifest);
    route(url, 200, doc);
}

int main(void) {
    char cache[256];
    snprintf(cache, sizeof(cache), "/tmp/krunker_build_fetch_test.%d", (int) getpid());
    test_setenv("KRUNKNATIVE_KRUNKER_BUILD_CACHE", cache, 1);
    char cache_file[300];
    snprintf(cache_file, sizeof(cache_file), "%s/krunker-build.json", cache);
    krunker_build_fetch_set_http(fake_get);
    const KrunkerBuild *compiled = krunker_build_compiled();

    // where the manifest comes from
    CHECK(!krunker_build_fetch_url(0, NULL) && !krunker_build_fetch_url(0, ""), "another matchmaker: no fetch");
    CHECK(!strcmp(krunker_build_fetch_url(1, NULL), KRUNKER_BUILD_DEFAULT_URL), "krunker.io's matchmaker: dl.krunk.io");
    CHECK(!krunker_build_fetch_url(1, "off") && !krunker_build_fetch_url(1, "0"), "off");
    CHECK(!strcmp(krunker_build_fetch_url(0, MANIFEST_URL), MANIFEST_URL), "an explicit URL");

    // no URL: the compiled build, at once
    refresh(NULL, 0);
    CHECK(g_done == 1 && g_src == KRUNKER_BUILD_SRC_COMPILED && krunker_build() == compiled && !g_requests, "no URL: compiled");

    // the fetch fails and nothing is cached: the compiled build (the fallback)
    route(MANIFEST_URL, 503, "unavailable");
    refresh(MANIFEST_URL, 0);
    CHECK(g_done == 1 && g_src == KRUNKER_BUILD_SRC_COMPILED && krunker_build() == compiled && strstr(g_why, "HTTP 503"),
          "fetch failed, no cache: compiled (%s)", g_why);

    // a published build: fetched, hash-checked, cached, current
    char *next = doc_text("NextBuildVersion0000000000000001", "10.0.4r1", 0x21, 0);
    char sha_next[65];
    publish(next, "NextBuildVersion0000000000000001", "10.0.4r1", 0, sha_next);
    refresh(MANIFEST_URL, 0);
    const KrunkerBuild *live = krunker_build();
    CHECK(g_done == 1 && g_src == KRUNKER_BUILD_SRC_LIVE && live->live && !strcmp(live->build_version, "NextBuildVersion0000000000000001") &&
          live->ahkey == 0x21 && !strcmp(g_cv, "10.0.4r1") && g_requests == 2,
          "published build: live (%d, %s, %d requests)", g_src, live->build_version, g_requests);
    struct stat st;
    CHECK(stat(cache_file, &st) == 0 && (size_t) st.st_size == strlen(next), "cached in %s", cache_file);

    // within the TTL: no request, the same build
    refresh(MANIFEST_URL, 0);
    CHECK(g_done == 1 && g_src == KRUNKER_BUILD_SRC_LIVE && krunker_build() == live && !g_requests, "TTL memo: no request");

    // forced, the manifest unreachable: the cached build (the last accepted one)
    route(MANIFEST_URL, 0, NULL);
    refresh(MANIFEST_URL, 1);
    CHECK(g_done == 1 && g_src == KRUNKER_BUILD_SRC_CACHED && krunker_build() == live && strstr(g_why, "HTTP 0"),
          "manifest unreachable: cached (%d %s)", g_src, g_why);

    // the manifest names the cached document: no download (its URL now answers 404)
    publish(next, "NextBuildVersion0000000000000001", "10.0.4r1", 0, sha_next);
    char doc_url[256];
    snprintf(doc_url, sizeof(doc_url), "http://stub.test/updates/krunker/%s.json", sha_next);
    route(doc_url, 404, "gone");
    refresh(MANIFEST_URL, 1);
    CHECK(g_done == 1 && g_src == KRUNKER_BUILD_SRC_LIVE && krunker_build() == live && g_requests == 1, "same sha256: no download (%d)",
          g_requests);

    // a document that is not the manifest's sha256 (a truncated or swapped file): the cached build
    char *other = doc_text("OtherBuildVersion000000000000002", "10.0.5r1", 0x22, 0);
    char sha_other[65];
    publish(other, "OtherBuildVersion000000000000002", "10.0.5r1", 1, sha_other);
    refresh(MANIFEST_URL, 1);
    CHECK(g_done == 1 && g_src == KRUNKER_BUILD_SRC_CACHED && krunker_build() == live && strstr(g_why, "sha256"),
          "wrong sha256: cached (%d %s)", g_src, g_why);

    // a build this client cannot follow: refused, saying which and why; the compiled build is current
    char *broken = doc_text("BrokenBuildVersion00000000000003", "10.1.0r1", 0x23, 1);
    char sha_broken[65];
    publish(broken, "BrokenBuildVersion00000000000003", "10.1.0r1", 0, sha_broken);
    refresh(MANIFEST_URL, 1);
    CHECK(g_done == 1 && g_src == KRUNKER_BUILD_SRC_REFUSED && !strcmp(g_cv, "10.1.0r1") && strstr(g_why, "ADD_PLAYERS no longer has \"sid\"") &&
          krunker_build() == compiled,
          "shape changed: refused (%d %s: %s)", g_src, g_cv, g_why);
    CHECK(stat(cache_file, &st) == 0 && (size_t) st.st_size == strlen(next), "a refused build is not cached");

    // a manifest of a schema this client does not read: refused
    route(MANIFEST_URL, 200, "{\"schema\":2,\"build_version\":\"x\"}");
    refresh(MANIFEST_URL, 1);
    CHECK(g_done == 1 && g_src == KRUNKER_BUILD_SRC_REFUSED && krunker_build() == compiled, "manifest schema 2: refused");

    // a new published build after the old one: live again, the cache follows
    publish(other, "OtherBuildVersion000000000000002", "10.0.5r1", 0, sha_other);
    refresh(MANIFEST_URL, 1);
    CHECK(g_done == 1 && g_src == KRUNKER_BUILD_SRC_LIVE && !strcmp(krunker_build()->build_version, "OtherBuildVersion000000000000002") &&
          krunker_build()->ahkey == 0x22, "the next published build");
    CHECK(stat(cache_file, &st) == 0 && (size_t) st.st_size == strlen(other), "the cache holds the newest accepted build");

    // a garbled cache and no manifest: the compiled build
    FILE *f = fopen(cache_file, "wb");
    if (f) {
        fputs("{garbage", f);
        fclose(f);
    }
    route(MANIFEST_URL, 500, "error");
    refresh(MANIFEST_URL, 1);
    CHECK(g_done == 1 && g_src == KRUNKER_BUILD_SRC_COMPILED && krunker_build() == compiled, "garbled cache, no manifest: compiled");

    free(next);
    free(other);
    free(broken);
    remove(cache_file);
    rmdir(cache);
    for (int i = 0; i < g_route_count; i++) free(g_routes[i].body);
    printf("%d/%d passed\n", g_pass, g_pass + g_fail);
    return g_fail ? 1 : 0;
}
