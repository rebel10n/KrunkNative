/* Unit test: release metadata (the kn-build line of .github/workflows/release.yml), picking the
 * latest server release from a GitHub /releases array, overrides, and the outdated /
 * incompatible rule (release_info.c). Pure, no network. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "release_info.h"

static int g_fail;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)

#define DAY 86400LL
#define T0 1791000000LL

static const char *k_releases =
    "["
    " {\"tag_name\":\"angle-b52e7ee00249\",\"draft\":false,\"body\":\"Prebuilt ANGLE\",\"assets\":[]},"
    " {\"tag_name\":\"v1.0.0\",\"draft\":false,\"body\":\"<!-- kn-build {\\\"commit\\\":\\\"1111111111111111111111111111111111111111\\\",\\\"commit_time\\\":1799000000,\\\"protocol\\\":\\\"1.3\\\"} -->\",\"assets\":[]},"
    " {\"tag_name\":\"server-v1.1.0\",\"draft\":false,\"html_url\":\"https://github.com/rebel10n/KrunkNative/releases/tag/server-v1.1.0\","
    "  \"body\":\"Server build\\n<!-- kn-build {\\\"describe\\\":\\\"server-v1.1.0\\\",\\\"commit\\\":\\\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\\\",\\\"commit_time\\\":1791000000,\\\"build_time\\\":\\\"2026-10-01T00:00:00Z\\\",\\\"protocol\\\":\\\"1.3\\\"} -->\","
    "  \"assets\":[]},"
    " {\"tag_name\":\"server-latest\",\"draft\":false,\"prerelease\":true,\"html_url\":\"https://github.com/rebel10n/KrunkNative/releases/tag/server-latest\","
    "  \"body\":\"Rolling\\n\\n<!-- kn-build {\\\"describe\\\":\\\"v1.0.0-60-gbbbbbbb\\\",\\\"commit\\\":\\\"BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB\\\",\\\"commit_time\\\":1791259200,\\\"build_time\\\":\\\"2026-10-04T00:00:00Z\\\",\\\"protocol\\\":\\\"1.3\\\"} -->\\n\","
    "  \"assets\":["
    "   {\"name\":\"BoxOffice-server-linux-x64.tar.gz\",\"browser_download_url\":\"https://github.com/rebel10n/KrunkNative/releases/download/server-latest/BoxOffice-server-linux-x64.tar.gz\"},"
    "   {\"name\":\"SHA256SUMS\",\"browser_download_url\":\"https://github.com/x/SHA256SUMS\"},"
    "   {\"name\":\"BoxOffice-server-windows-x64.zip\",\"browser_download_url\":\"https://github.com/rebel10n/KrunkNative/releases/download/server-latest/BoxOffice-server-windows-x64.zip\"},"
    "   {\"name\":\"BoxOffice-server-macos-arm64.zip\",\"browser_download_url\":\"https://github.com/rebel10n/KrunkNative/releases/download/server-latest/BoxOffice-server-macos-arm64.zip\"},"
    "   {\"name\":\"BoxOffice-server-linux-x64-polyfilled.tar.gz\",\"browser_download_url\":\"http://insecure/x\"}"
    "  ]},"
    " {\"tag_name\":\"server-v9.9.9\",\"draft\":true,\"body\":\"<!-- kn-build {\\\"commit\\\":\\\"cccccccccccccccccccccccccccccccccccccccc\\\",\\\"commit_time\\\":1899000000,\\\"protocol\\\":\\\"1.3\\\"} -->\",\"assets\":[]}"
    "]";

static void test_protocol(void) {
    int ma, mi;
    CHECK(release_parse_protocol("1.3", &ma, &mi) && ma == 1 && mi == 3);
    CHECK(release_parse_protocol("12.0", &ma, &mi) && ma == 12 && mi == 0);
    CHECK(!release_parse_protocol("1", &ma, &mi) && ma == -1);
    CHECK(!release_parse_protocol("1.3.0", &ma, &mi));
    CHECK(!release_parse_protocol("", &ma, &mi));
    CHECK(!release_parse_protocol(NULL, &ma, &mi));
}

static void test_body(void) {
    ReleaseInfo r;
    memset(&r, 0, sizeof(r));
    CHECK(!release_parse_body("no metadata", &r));
    CHECK(!release_parse_body("<!-- kn-build {\"commit\":\"xyz\"} -->", &r)); /* not hex */
    CHECK(!release_parse_body("<!-- kn-build {broken -->", &r));
    CHECK(release_parse_body("x <!-- kn-build {\"describe\":\"d\",\"commit\":\"ABCDEF0123\",\"commit_time\":5,"
                             "\"build_time\":\"2026-01-01T00:00:00Z\",\"protocol\":\"1.3\"} --> y", &r));
    CHECK(!strcmp(r.commit, "abcdef0123") && r.commit_time == 5 && r.proto_major == 1 && r.proto_minor == 3);
    CHECK(!strcmp(r.describe, "d") && !strcmp(r.build_time, "2026-01-01T00:00:00Z"));
}

static void test_pick(void) {
    cJSON *arr = cJSON_Parse(k_releases);
    CHECK(cJSON_IsArray(arr));
    ReleaseInfo r;
    CHECK(release_pick_latest(arr, &r));
    /* newest commit_time among server-* non-drafts: server-latest (v1.0.0 and the draft ignored) */
    CHECK(!strcmp(r.tag, "server-latest"));
    CHECK(!strcmp(r.commit, "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"));
    CHECK(r.commit_time == 1791259200LL);
    CHECK(!strcmp(r.html_url, "https://github.com/rebel10n/KrunkNative/releases/tag/server-latest"));
    CHECK(strstr(r.dl_linux, "linux-x64.tar.gz") && !strstr(r.dl_linux, "SHA256SUMS"));
    CHECK(strstr(r.dl_windows, "windows-x64.zip") != NULL);
    CHECK(strstr(r.dl_macos, "macos-arm64.zip") != NULL);
    CHECK(r.dl_linux_polyfilled[0] == 0); /* http:// is never offered */

    cJSON *o = cJSON_CreateObject();
    release_add_json(&r, o);
    CHECK(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(o, "download_url")), r.html_url));
    CHECK(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(o, "latest"), "protocol")), "1.3"));
    CHECK(cJSON_GetObjectItem(cJSON_GetObjectItem(o, "downloads"), "windows") != NULL);
    cJSON_Delete(o);

    /* nothing usable */
    cJSON *none = cJSON_Parse("[{\"tag_name\":\"angle-x\",\"body\":\"\"}]");
    CHECK(!release_pick_latest(none, &r));
    cJSON_Delete(none);
    cJSON_Delete(arr);

    /* no release known: the default page */
    o = cJSON_CreateObject();
    release_add_json(NULL, o);
    CHECK(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(o, "download_url")), RELEASE_PAGE_DEFAULT));
    CHECK(!cJSON_GetObjectItem(o, "latest"));
    cJSON_Delete(o);
}

static void test_outdated(void) {
    ReleaseInfo latest;
    memset(&latest, 0, sizeof(latest));
    latest.known = 1;
    snprintf(latest.commit, sizeof(latest.commit), "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
    latest.commit_time = T0 + 10 * DAY;
    latest.proto_major = 1;
    latest.proto_minor = 3;
    const int64_t grace = 2 * DAY;

    ServerBuild same = {"v1", "bbbbbbbbbbbb", T0, 1, 3}; /* same commit (12-hex prefix): never outdated */
    CHECK(!release_server_outdated(&latest, &same, grace, 1, 3));
    ServerBuild recent = {"v1", "aaaaaaaaaaaa", T0 + 9 * DAY, 1, 3}; /* 1 day older: within the grace */
    CHECK(!release_server_outdated(&latest, &recent, grace, 1, 3));
    ServerBuild old = {"v1", "aaaaaaaaaaaa", T0 + 7 * DAY, 1, 3}; /* 3 days older: outdated */
    CHECK(release_server_outdated(&latest, &old, grace, 1, 3));
    ServerBuild newer = {"v1", "cccccccccccc", T0 + 11 * DAY, 1, 3}; /* newer than the release */
    CHECK(!release_server_outdated(&latest, &newer, grace, 1, 3));
    ServerBuild old_proto = {"v1", "cccccccccccc", T0 + 11 * DAY, 1, 2}; /* (a) older protocol */
    CHECK(release_server_outdated(&latest, &old_proto, grace, 1, 3));
    CHECK(release_server_outdated(NULL, &old_proto, grace, 1, 3));
    ServerBuild legacy = {"", "", 0, -1, -1}; /* (c) a server_bin without build info */
    CHECK(release_server_outdated(&latest, &legacy, grace, 1, 3));
    CHECK(!release_server_outdated(NULL, &legacy, grace, 1, 3)); /* nothing known: not flagged */
    ServerBuild no_time = {"x", "dddddddddddd", 0, 1, 3};
    CHECK(release_server_outdated(&latest, &no_time, grace, 1, 3));

    /* config override: describe or commit, no time */
    ReleaseInfo cfg;
    memset(&cfg, 0, sizeof(cfg));
    release_apply_overrides(&cfg, "v1.0.0-60-gbbbbbbb", "https://example.org/dl");
    CHECK(cfg.known && cfg.from_config && !strcmp(cfg.html_url, "https://example.org/dl"));
    ServerBuild match_desc = {"v1.0.0-60-gbbbbbbb", "bbbbbbbbbbbb", T0, 1, 3};
    CHECK(!release_server_outdated(&cfg, &match_desc, grace, 1, 3));
    ServerBuild other = {"v1.0.0-59-gaaaaaaa", "aaaaaaaaaaaa", T0 + 100 * DAY, 1, 3};
    CHECK(release_server_outdated(&cfg, &other, grace, 1, 3));
    ReleaseInfo by_commit;
    memset(&by_commit, 0, sizeof(by_commit));
    release_apply_overrides(&by_commit, "BBBBBBB", NULL);
    CHECK(!release_server_outdated(&by_commit, &match_desc, grace, 1, 3));
    CHECK(release_server_outdated(&by_commit, &other, grace, 1, 3));

    /* finder entry / lobby meta parsing */
    cJSON *e = cJSON_Parse("{\"version\":\"v1\",\"commit\":\"aaaaaaaaaaaa\",\"commit_time\":1791000000,\"protocol\":\"1.3\"}");
    ServerBuild s;
    release_server_build_from_finder(e, &s);
    CHECK(s.commit_time == 1791000000LL && s.proto_major == 1 && s.proto_minor == 3 && !strcmp(s.commit, "aaaaaaaaaaaa"));
    cJSON_Delete(e);
    e = cJSON_Parse("{\"version\":\"1.0.0\"}"); /* a pre-build-info server_bin */
    release_server_build_from_finder(e, &s);
    CHECK(release_server_outdated(&latest, &s, grace, 1, 3));
    cJSON_Delete(e);
    e = cJSON_Parse("{\"v\":\"1.0.0\",\"build\":\"b\",\"commit\":\"bbbbbbbbbbbb\",\"commitTime\":1,\"proto\":\"2.0\"}");
    release_server_build_from_meta(e, &s);
    CHECK(s.proto_major == 2 && !release_server_outdated(&latest, &s, grace, 1, 3));
    cJSON_Delete(e);

    CHECK(release_protocol_incompatible(1, 2));
    CHECK(!release_protocol_incompatible(1, 1));
    CHECK(!release_protocol_incompatible(1, -1)); /* not announced: a 1.x server */
    CHECK(!release_protocol_incompatible(-1, 2));
    CHECK(release_same_commit("ABCDEF1", "abcdef123456"));
    CHECK(!release_same_commit("abcdef", "abcdef123456")); /* too short */
    CHECK(!release_same_commit("", "abcdef123456"));
}

int main(void) {
    test_protocol();
    test_body();
    test_pick();
    test_outdated();
    if (g_fail) return 1;
    printf("releases: ok\n");
    return 0;
}
