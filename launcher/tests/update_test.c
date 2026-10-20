// Client updates, unit tests (launcher/include/kn_update.h, kn_archive.h, kn_install.h):
// manifest parsing, URL resolution, version comparison, asset lists and diffing, the install
// marker, package extraction (tar.gz / zip, built here with stored deflate blocks) and the
// journaled swap with its rollback and crash recovery. Run: ./update_test [scratch dir]
#include <kn_archive.h>
#include <kn_fs.h>
#include <kn_install.h>
#include <kn_update.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <direct.h>
#define getcwd _getcwd
#define chdir _chdir
#else
#include <unistd.h>
#endif

static int g_fail, g_checks;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        g_checks++;                                                                    \
        if (!(cond)) {                                                                 \
            g_fail++;                                                                  \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
        }                                                                              \
    } while (0)
#define CHECK_STR(a, b)                                                                \
    do {                                                                               \
        g_checks++;                                                                    \
        if (strcmp((a), (b)) != 0) {                                                   \
            g_fail++;                                                                  \
            fprintf(stderr, "FAIL %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, (a), (b)); \
        }                                                                              \
    } while (0)

static const char *SHA_A = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static const char *SHA_B = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
static const char *SHA_C = "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";

// ------------------------------------------------------------------ URLs, paths

// KRUNKNATIVE_ASSET_ORIGINS / --asset-mirrors origin specs with path filters (the split CDN)
static void test_origin_specs(void) {
    char url[KN_UPDATE_URL_MAX], spec[2048];
    int by_path = -1;
    KnPathFilter f;
    CHECK(kn_origin_spec_parse("https://a.example/assets", url, sizeof(url), &by_path, &f));
    CHECK_STR(url, "https://a.example/assets/");
    CHECK(by_path == 1 && f.mode == KN_PATH_ALL && kn_path_filter_ok(&f, "anything"));
    CHECK(kn_origin_spec_parse("objects:https://dl.example/updates/assets/objects/", url, sizeof(url), &by_path, &f));
    CHECK(by_path == 0 && f.mode == KN_PATH_ALL);
    CHECK(kn_origin_spec_parse("only=textures/previews/+textures/hats/@https://b.example/assets/", url, sizeof(url), &by_path, &f));
    CHECK_STR(url, "https://b.example/assets/");
    CHECK(by_path == 1 && f.mode == KN_PATH_ONLY);
    CHECK(kn_path_filter_ok(&f, "textures/hats/a.png") && !kn_path_filter_ok(&f, "textures/body/a.png"));
    CHECK(kn_origin_spec_format(spec, sizeof(spec), url, by_path, &f));
    CHECK_STR(spec, "only=textures/previews/+textures/hats/@https://b.example/assets/");
    CHECK(kn_origin_spec_parse("skip=textures/hats/@https://a.example/assets/", url, sizeof(url), &by_path, &f));
    CHECK(f.mode == KN_PATH_SKIP && !kn_path_filter_ok(&f, "textures/hats/a.png") && kn_path_filter_ok(&f, "sound/a.mp3"));
    CHECK(kn_origin_spec_format(spec, sizeof(spec), "https://dl.example/o/", 0, NULL));
    CHECK_STR(spec, "objects:https://dl.example/o/");
    CHECK(!kn_origin_spec_parse("only=@https://b.example/", url, sizeof(url), &by_path, &f));     // no prefix
    CHECK(!kn_origin_spec_parse("only=../x/@https://b.example/", url, sizeof(url), &by_path, &f)); // unsafe
    CHECK(!kn_origin_spec_parse("only=x/https://b.example/", url, sizeof(url), &by_path, &f));     // no '@'
    CHECK(!kn_origin_spec_parse("skip=x/@ftp://b.example/", url, sizeof(url), &by_path, &f));      // not http(s)
    CHECK(!kn_origin_spec_format(spec, 8, "https://a.example/", 1, NULL));                        // does not fit
}

static void test_urls(void) {
    char out[KN_UPDATE_URL_MAX];
    const char *base = "https://assets.krunk.io/updates/client/latest.json";
    CHECK(kn_update_resolve_url(base, "builds/abc/p.tar.gz", out, sizeof(out)));
    CHECK_STR(out, "https://assets.krunk.io/updates/client/builds/abc/p.tar.gz");
    CHECK(kn_update_resolve_url(base, "../assets/v1.json", out, sizeof(out)));
    CHECK_STR(out, "https://assets.krunk.io/updates/assets/v1.json");
    CHECK(kn_update_resolve_url(base, "../assets/objects/", out, sizeof(out)));
    CHECK_STR(out, "https://assets.krunk.io/updates/assets/objects/");
    CHECK(kn_update_resolve_url(base, "../../../../x", out, sizeof(out)));
    CHECK_STR(out, "https://assets.krunk.io/x"); // never above the origin
    CHECK(kn_update_resolve_url(base, "/root.json", out, sizeof(out)));
    CHECK_STR(out, "https://assets.krunk.io/root.json");
    CHECK(kn_update_resolve_url(base, "./a/./b", out, sizeof(out)));
    CHECK_STR(out, "https://assets.krunk.io/updates/client/a/b");
    CHECK(kn_update_resolve_url(base, "https://github.com/x/y", out, sizeof(out)));
    CHECK_STR(out, "https://github.com/x/y");
    CHECK(kn_update_resolve_url("http://127.0.0.1:8080", "a.json", out, sizeof(out)));
    CHECK_STR(out, "http://127.0.0.1:8080/a.json");
    CHECK(kn_update_resolve_url("http://h/a/b?q=1", "c?d=2", out, sizeof(out)));
    CHECK_STR(out, "http://h/a/c?d=2");
    CHECK(!kn_update_resolve_url(base, "", out, sizeof(out)));
    CHECK(!kn_update_resolve_url(base, "a b", out, sizeof(out)));
    CHECK(!kn_update_resolve_url(base, "javascript:alert(1)//x://", out, sizeof(out)));
    CHECK(!kn_update_resolve_url("file:///etc", "x", out, sizeof(out)));

    CHECK(kn_update_path_ok("textures/a.png"));
    CHECK(kn_update_path_ok(".kn-assets.json")); // a valid path (the list parser rejects .kn-*)
    CHECK(!kn_update_path_ok("../a"));
    CHECK(!kn_update_path_ok("a/../b"));
    CHECK(!kn_update_path_ok("/etc/passwd"));
    CHECK(!kn_update_path_ok("a//b"));
    CHECK(!kn_update_path_ok("a/./b"));
    CHECK(!kn_update_path_ok("a\\b"));
    CHECK(!kn_update_path_ok("C:x"));
    CHECK(!kn_update_path_ok(""));
    CHECK(!kn_update_path_ok("a/"));
}

// ------------------------------------------------------------------ manifest, compare

static const char *MANIFEST =
    "{\"format\":1,\"channel\":\"latest\",\"outdated_grace_h\":24,"
    "\"page\":\"https://github.com/rebel10n/KrunkNative/releases/tag/client-latest\","
    "\"build\":{\"describe\":\"v1.0.0-90-gABCDEF1\",\"commit\":\"ABCDEF1234567890abcdef1234567890abcdef12\","
    "\"commit_time\":1760000000,\"build_time\":\"2026-10-12T00:00:00Z\",\"protocol\":\"1.3\"},"
    "\"platforms\":{"
    "\"linux-x64\":{\"update\":{\"url\":\"builds/abcdef123456/BoxOffice-linux-x64.tar.gz\","
    "\"sha256\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\",\"size\":1234},"
    "\"downloads\":{\"tar.gz\":\"https://github.com/rebel10n/KrunkNative/releases/download/client-latest/BoxOffice-linux-x64.tar.gz\"}},"
    "\"windows-x64\":{\"update\":{\"url\":\"builds/abcdef123456/BoxOffice-windows-x64.zip\",\"sha256\":\"short\",\"size\":5},"
    "\"downloads\":{\"zip\":\"builds/abcdef123456/BoxOffice-windows-x64.zip\"}}},"
    "\"assets\":{\"version\":\"0123456789abcdef\",\"manifest\":\"../assets/0123456789abcdef.json\","
    "\"sha256\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\",\"size\":99,"
    "\"objects\":\"../assets/objects\",\"files\":3,\"bytes\":300,\"packaged\":[\"shaders/b.frag\",\"maps/a.json\",\"../x\"]}}";

static void test_manifest(void) {
    const char *url = "https://assets.krunk.io/updates/client/latest.json";
    KnUpdateManifest m;
    char err[128];
    CHECK(kn_update_manifest_parse(MANIFEST, strlen(MANIFEST), url, &m, err, sizeof(err)));
    CHECK_STR(m.channel, "latest");
    CHECK_STR(m.build.commit, "abcdef1234567890abcdef1234567890abcdef12");
    CHECK(m.build.commit_time == 1760000000);
    CHECK(m.build.proto_major == 1 && m.build.proto_minor == 3);
    CHECK(m.grace_s == 24 * 3600);
    CHECK(m.platform_count == 2);
    const KnUpdatePlatform *p = kn_update_manifest_platform(&m, "linux-x64");
    CHECK(p && p->has_update);
    if (p) {
        CHECK_STR(p->update.url, "https://assets.krunk.io/updates/client/builds/abcdef123456/BoxOffice-linux-x64.tar.gz");
        CHECK_STR(p->update.sha256, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
        CHECK(p->update.size == 1234);
        CHECK(p->link_count == 1); // the tar.gz only: Linux ships no .deb
    }
    const KnUpdatePlatform *w = kn_update_manifest_platform(&m, "windows-x64");
    CHECK(w && !w->has_update); // a bad sha256 is no update
    CHECK(!kn_update_manifest_platform(&m, "macos-arm64"));
    CHECK_STR(kn_update_download_url(&m, "linux-x64", "tar.gz"),
              "https://github.com/rebel10n/KrunkNative/releases/download/client-latest/BoxOffice-linux-x64.tar.gz");
    CHECK_STR(kn_update_download_url(&m, "linux-x64", "zip"), // no zip for Linux: the archive
              "https://github.com/rebel10n/KrunkNative/releases/download/client-latest/BoxOffice-linux-x64.tar.gz");
    CHECK_STR(kn_update_download_url(&m, "windows-x64", NULL),
              "https://assets.krunk.io/updates/client/builds/abcdef123456/BoxOffice-windows-x64.zip");
    CHECK_STR(kn_update_download_url(&m, "macos-arm64", NULL), "https://github.com/rebel10n/KrunkNative/releases/tag/client-latest");
    CHECK_STR(m.assets.version, "0123456789abcdef");
    CHECK_STR(m.assets.manifest.url, "https://assets.krunk.io/updates/assets/0123456789abcdef.json");
    CHECK_STR(m.assets.objects, "https://assets.krunk.io/updates/assets/objects/");
    CHECK(m.assets.files == 3 && m.assets.bytes == 300);
    CHECK(m.assets.packaged_count == 2); // the unsafe one dropped
    CHECK(kn_update_is_packaged(&m, "maps/a.json") && kn_update_is_packaged(&m, "shaders/b.frag"));
    CHECK(!kn_update_is_packaged(&m, "textures/a.png"));
    CHECK(!m.assets.mirrors_set && m.assets.mirror_count == 0); // no key: the launcher's default
    kn_update_manifest_free(&m);

    // assets.mirrors: strings (by path) or {url, layout}; relative to the manifest; bad ones dropped
    const char *mirrored =
        "{\"format\":1,\"build\":{\"commit\":\"abcdef1\"},\"assets\":{\"version\":\"v1\",\"manifest\":\"../assets/v1.json\","
        "\"objects\":\"../assets/objects/\",\"mirrors\":[\"https://cdn.example/assets\",{\"url\":\"/mirror/objects/\","
        "\"layout\":\"objects\"},{\"url\":\"https://x.example/\",\"layout\":\"zip\"},\"ftp://no\",7]}}";
    CHECK(kn_update_manifest_parse(mirrored, strlen(mirrored), url, &m, err, sizeof(err)));
    CHECK(m.assets.mirrors_set && m.assets.mirror_count == 2);
    CHECK_STR(m.assets.mirrors[0].url, "https://cdn.example/assets/");
    CHECK(m.assets.mirrors[0].by_path == 1);
    CHECK_STR(m.assets.mirrors[1].url, "https://assets.krunk.io/mirror/objects/");
    CHECK(m.assets.mirrors[1].by_path == 0);
    kn_update_manifest_free(&m);
    const char *none = "{\"format\":1,\"build\":{\"commit\":\"abcdef1\"},\"assets\":{\"version\":\"v1\","
                       "\"manifest\":\"v1.json\",\"objects\":\"o/\",\"mirrors\":[]}}";
    CHECK(kn_update_manifest_parse(none, strlen(none), url, &m, err, sizeof(err)));
    CHECK(m.assets.mirrors_set && m.assets.mirror_count == 0); // an empty list: no mirrors at all
    kn_update_manifest_free(&m);

    // the split Pages CDN (deploy/asset-shards.json): "only" / "skip" path filters; a mirror whose
    // filter is unusable (both keys, a bad prefix, on an objects layout, only=[]) is dropped
    const char *split =
        "{\"format\":1,\"build\":{\"commit\":\"abcdef1\"},\"assets\":{\"version\":\"v1\",\"manifest\":\"v1.json\","
        "\"objects\":\"o/\",\"mirrors\":["
        "{\"url\":\"https://a.example/assets/\",\"skip\":[\"textures/previews/\",\"textures/hats/\"]},"
        "{\"url\":\"https://b.example/assets/\",\"layout\":\"path\",\"only\":[\"textures/previews/\",\"textures/hats/\"]},"
        "{\"url\":\"https://c.example/\",\"only\":[\"x/\"],\"skip\":[\"y/\"]},"
        "{\"url\":\"https://d.example/\",\"only\":[\"../x/\"]},"
        "{\"url\":\"https://e.example/\",\"layout\":\"objects\",\"only\":[\"x/\"]}]}}";
    CHECK(kn_update_manifest_parse(split, strlen(split), url, &m, err, sizeof(err)));
    CHECK(m.assets.mirror_count == 2);
    CHECK(m.assets.mirrors[0].filter.mode == KN_PATH_SKIP && m.assets.mirrors[1].filter.mode == KN_PATH_ONLY);
    CHECK_STR(m.assets.mirrors[1].filter.list, "textures/previews/+textures/hats/");
    CHECK(kn_path_filter_ok(&m.assets.mirrors[0].filter, "textures/weapons/skins/w_1.png"));
    CHECK(!kn_path_filter_ok(&m.assets.mirrors[0].filter, "textures/hats/hat_1.png"));
    CHECK(kn_path_filter_ok(&m.assets.mirrors[1].filter, "textures/previews/weapons/w.png"));
    CHECK(!kn_path_filter_ok(&m.assets.mirrors[1].filter, "textures/hatsX/a.png"));
    CHECK(!kn_path_filter_ok(&m.assets.mirrors[1].filter, "models/hats/a.obj"));
    kn_update_manifest_free(&m);

    CHECK(!kn_update_manifest_parse("[]", 2, url, &m, err, sizeof(err)));
    CHECK(!kn_update_manifest_parse("{\"format\":2,\"build\":{\"commit\":\"abcdef1\"}}", 43, url, &m, err, sizeof(err)));
    const char *nobuild = "{\"format\":1}";
    CHECK(!kn_update_manifest_parse(nobuild, strlen(nobuild), url, &m, err, sizeof(err)));
    const char *minimal = "{\"format\":1,\"build\":{\"commit\":\"abcdef1\"},\"assets\":{\"version\":\"../x\"}}";
    CHECK(kn_update_manifest_parse(minimal, strlen(minimal), url, &m, err, sizeof(err)));
    CHECK(m.grace_s == KN_UPDATE_GRACE_DEFAULT_S);
    CHECK(!m.assets.version[0]); // an unsafe version is no asset set
    kn_update_manifest_free(&m);
}

static void test_compare(void) {
    KnUpdateBuild latest = {.commit = "abcdef1234567890", .commit_time = 1000000, .proto_major = 1, .proto_minor = 3};
    KnUpdateBuild mine = {.commit = "abcdef123456", .commit_time = 1000000, .proto_major = 1, .proto_minor = 3};
    const int64_t grace = 48 * 3600;
    CHECK(kn_update_compare(&mine, &latest, grace) == KN_UPDATE_CURRENT); // same commit (prefix)
    snprintf(mine.commit, sizeof(mine.commit), "1111111");
    mine.commit_time = 1000000 - 3600;
    CHECK(kn_update_compare(&mine, &latest, grace) == KN_UPDATE_AVAILABLE);
    mine.commit_time = 1000000 - 49 * 3600;
    CHECK(kn_update_compare(&mine, &latest, grace) == KN_UPDATE_OUTDATED);
    CHECK(kn_update_compare(&mine, &latest, 0) == KN_UPDATE_OUTDATED);
    mine.commit_time = 1000000 + 10; // a newer dev build
    CHECK(kn_update_compare(&mine, &latest, grace) == KN_UPDATE_CURRENT);
    mine.proto_minor = 2; // older protocol: outdated at once
    CHECK(kn_update_compare(&mine, &latest, grace) == KN_UPDATE_OUTDATED);
    mine.proto_minor = 3;
    mine.commit_time = 0; // no build info
    CHECK(kn_update_compare(&mine, &latest, grace) == KN_UPDATE_OUTDATED);
    KnUpdateBuild none = {.proto_major = -1};
    CHECK(kn_update_compare(&mine, &none, grace) == KN_UPDATE_CURRENT);
    CHECK(kn_update_same_commit("ABCDEF1", "abcdef1234"));
    CHECK(!kn_update_same_commit("abcdef", "abcdef1234")); // < 7 digits
    CHECK(!kn_update_same_commit("abcdef12", "abcdef13"));
    int ma, mi;
    CHECK(kn_update_parse_protocol("1.3", &ma, &mi) && ma == 1 && mi == 3);
    CHECK(!kn_update_parse_protocol("1", &ma, &mi));
    CHECK(!kn_update_parse_protocol("x.3", &ma, &mi));
    CHECK_STR(kn_update_state_name(KN_UPDATE_OUTDATED), "outdated");
}

// ------------------------------------------------------------------ asset lists, diff

static char *asset_json(const char *version, const char *entries) {
    static char buf[4096];
    snprintf(buf, sizeof(buf), "{\"format\":1,\"version\":\"%s\",\"files\":[%s]}", version, entries);
    return buf;
}

typedef struct {
    const char *paths[8];
    int64_t sizes[8];
    const char *hashes[8];
    int n;
    int hash_calls;
} FakeFs;

static int64_t fake_stat(void *user, const char *path) {
    const FakeFs *f = user;
    for (int i = 0; i < f->n; i++)
        if (!strcmp(f->paths[i], path)) return f->sizes[i];
    return -1;
}

static int fake_hash(void *user, const char *path, char out[65]) {
    FakeFs *f = user;
    f->hash_calls++;
    for (int i = 0; i < f->n; i++)
        if (!strcmp(f->paths[i], path)) {
            snprintf(out, 65, "%s", f->hashes[i]);
            return 1;
        }
    return 0;
}

static void test_assets(void) {
    char e1[512], e2[512];
    snprintf(e1, sizeof(e1), "[\"b/x.png\",\"%s\",10],[\"a.json\",\"%s\",20],[\"c.ogg\",\"%s\",30]", SHA_A, SHA_B, SHA_C);
    KnAssetList want, have;
    char err[64];
    char *j = asset_json("v2", e1);
    CHECK(kn_asset_list_parse(j, strlen(j), &want, err, sizeof(err)));
    CHECK(want.count == 3);
    CHECK_STR(want.files[0].path, "a.json"); // sorted
    CHECK(kn_asset_list_find(&want, "c.ogg") && kn_asset_list_find(&want, "c.ogg")->size == 30);
    CHECK(!kn_asset_list_find(&want, "zzz"));
    CHECK(kn_asset_list_bytes(&want) == 60);
    char *round = kn_asset_list_json(&want);
    KnAssetList again;
    CHECK(round && kn_asset_list_parse(round, strlen(round), &again, err, sizeof(err)) && again.count == 3);
    CHECK_STR(again.version, "v2");
    kn_asset_list_free(&again);
    free(round);

    // have (v1): b/x.png same hash, a.json old hash, old.txt removed upstream
    snprintf(e2, sizeof(e2), "[\"b/x.png\",\"%s\",10],[\"a.json\",\"%s\",20],[\"old.txt\",\"%s\",5]", SHA_A, SHA_C, SHA_C);
    j = asset_json("v1", e2);
    CHECK(kn_asset_list_parse(j, strlen(j), &have, err, sizeof(err)));
    FakeFs fs = {.paths = {"b/x.png", "a.json", "old.txt", "c.ogg", "user.png"},
                 .sizes = {10, 20, 5, 7, 1},
                 .hashes = {SHA_A, SHA_C, SHA_C, SHA_C, SHA_A},
                 .n = 5};
    KnAssetDiff d;
    CHECK(kn_asset_diff(&want, &have, NULL, 0, fake_stat, fake_hash, &fs, &d));
    CHECK(d.kept == 1);             // b/x.png: have vouches, size matches, not hashed
    CHECK(d.fetch_count == 2);      // a.json (changed), c.ogg (wrong size)
    CHECK(d.fetch_bytes == 50);
    CHECK(d.hashed == 1 && fs.hash_calls == 1); // only a.json (same size, have says another hash)
    CHECK(d.remove_count == 1 && !strcmp(d.remove[0], "old.txt"));
    kn_asset_diff_free(&d);
    // a path the packages now ship (assets.packaged) is never removed by the sync
    char *keep[] = {"old.txt"};
    fs.hash_calls = 0;
    CHECK(kn_asset_diff(&want, &have, keep, 1, fake_stat, fake_hash, &fs, &d) && d.remove_count == 0);
    kn_asset_diff_free(&d);

    // a file of the right size that have does not list, with the right content: adopted
    fs.hashes[1] = SHA_B;
    fs.hash_calls = 0;
    CHECK(kn_asset_diff(&want, NULL, NULL, 0, fake_stat, fake_hash, &fs, &d));
    CHECK(d.adopted == 2 && d.kept == 0);      // b/x.png + a.json hashed and matching
    CHECK(d.fetch_count == 1 && want.files[d.fetch[0]].path[0] == 'c');
    CHECK(d.remove_count == 0);                // no list: nothing is ever deleted
    kn_asset_diff_free(&d);
    // no hash function: unvouched files are fetched
    CHECK(kn_asset_diff(&want, NULL, NULL, 0, fake_stat, NULL, &fs, &d) && d.fetch_count == 3);
    kn_asset_diff_free(&d);
    // first install: nothing local
    CHECK(kn_asset_diff(&want, NULL, NULL, 0, NULL, NULL, NULL, &d) && d.fetch_count == 3 && d.fetch_bytes == 60);
    kn_asset_diff_free(&d);
    kn_asset_list_free(&have);
    kn_asset_list_free(&want);

    const char *bad[] = {"[\"../x\",\"%s\",1]", "[\"/x\",\"%s\",1]", "[\"x\",\"abc\",1]", "[\"x\",\"%s\",-1]",
                         "[\"x\",\"%s\",1],[\"x\",\"%s\",1]", "[\".kn-assets.json\",\"%s\",1]"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        char e[512];
        snprintf(e, sizeof(e), bad[i], SHA_A, SHA_A);
        j = asset_json("v", e);
        CHECK(!kn_asset_list_parse(j, strlen(j), &want, err, sizeof(err)));
    }
}

static void test_marker(void) {
    KnInstallMarker m;
    const char *tgz = "{\"format\":1,\"platform\":\"linux-x64\",\"package\":\"tar.gz\",\"channel\":\"latest\",\"root\":\"..\"}";
    CHECK(kn_install_marker_parse(tgz, strlen(tgz), &m) && kn_install_self_updatable(&m));
    CHECK_STR(m.platform, "linux-x64");
    const char *deb = "{\"format\":1,\"platform\":\"linux-x64\",\"package\":\"deb\"}";
    CHECK(kn_install_marker_parse(deb, strlen(deb), &m) && !kn_install_self_updatable(&m));
    CHECK_STR(m.root, "."); // no "root" in that marker: the default, its own directory
    CHECK_STR(m.channel, "latest");
    const char *app = "{\"format\":1,\"platform\":\"macos-arm64\",\"package\":\"zip\",\"root\":\"../../..\"}";
    CHECK(kn_install_marker_parse(app, strlen(app), &m) && !strcmp(m.root, "../../.."));
    const char *evil = "{\"format\":1,\"root\":\"/etc\"}";
    CHECK(!kn_install_marker_parse(evil, strlen(evil), &m));
    const char *evil2 = "{\"format\":1,\"channel\":\"../x\"}";
    CHECK(!kn_install_marker_parse(evil2, strlen(evil2), &m));
    CHECK(!kn_install_marker_parse("{}", 2, &m));
    CHECK(kn_update_platform_default()[0]);
    CHECK_STR(kn_update_archive_kind("windows-x64"), "zip");
    CHECK_STR(kn_update_archive_kind("macos-arm64"), "zip");
    CHECK_STR(kn_update_archive_kind("linux-x64"), "tar.gz");
}

// ------------------------------------------------------------------ archives

typedef struct {
    unsigned char *d;
    size_t n, cap;
} Buf;

static void put(Buf *b, const void *p, const size_t n) {
    if (b->n + n > b->cap) {
        b->cap = (b->n + n) * 2 + 1024;
        b->d = realloc(b->d, b->cap);
    }
    memcpy(b->d + b->n, p, n);
    b->n += n;
}

static void put16(Buf *b, const unsigned v) {
    const unsigned char c[2] = {(unsigned char) v, (unsigned char) (v >> 8)};
    put(b, c, 2);
}

static void put32(Buf *b, const uint32_t v) {
    const unsigned char c[4] = {(unsigned char) v, (unsigned char) (v >> 8), (unsigned char) (v >> 16), (unsigned char) (v >> 24)};
    put(b, c, 4);
}

static void tar_entry(Buf *b, const char *name, const char type, const unsigned mode, const char *data, const char *link) {
    unsigned char h[512] = {0};
    snprintf((char *) h, 100, "%s", name);
    snprintf((char *) h + 100, 8, "%07o", mode);
    snprintf((char *) h + 108, 8, "%07o", 0);
    snprintf((char *) h + 116, 8, "%07o", 0);
    const size_t len = data ? strlen(data) : 0;
    snprintf((char *) h + 124, 12, "%011zo", len);
    snprintf((char *) h + 136, 12, "%011o", 0);
    h[156] = (unsigned char) type;
    if (link) snprintf((char *) h + 157, 100, "%s", link);
    memcpy(h + 257, "ustar", 6);
    memcpy(h + 263, "00", 2);
    memset(h + 148, ' ', 8);
    unsigned sum = 0;
    for (int i = 0; i < 512; i++) sum += h[i];
    snprintf((char *) h + 148, 8, "%06o", sum);
    put(b, h, 512);
    if (len) {
        put(b, data, len);
        static const unsigned char zero[512] = {0};
        if (len % 512) put(b, zero, 512 - len % 512);
    }
}

// gzip with stored deflate blocks (no compression), as the decoder must also accept
static Buf gzip_stored(const Buf *in) {
    Buf g = {0};
    static const unsigned char head[10] = {0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 3};
    put(&g, head, 10);
    size_t off = 0;
    do {
        const size_t n = in->n - off > 65535 ? 65535 : in->n - off;
        const unsigned char bfinal = off + n >= in->n;
        put(&g, &bfinal, 1);
        put16(&g, (unsigned) n);
        put16(&g, (unsigned) (~n & 0xffff));
        put(&g, in->d + off, n);
        off += n;
    } while (off < in->n);
    put32(&g, 0); // CRC (not checked: the package sha256 is)
    put32(&g, (uint32_t) in->n);
    return g;
}

typedef struct {
    const char *name, *data;
    unsigned mode; // Unix mode with type bits (0100755, 0120777, 040755)
} ZipFile;

static Buf zip_stored(const ZipFile *files, const int n) {
    Buf z = {0}, cd = {0};
    for (int i = 0; i < n; i++) {
        const size_t nl = strlen(files[i].name), dl = files[i].data ? strlen(files[i].data) : 0;
        const uint32_t off = (uint32_t) z.n;
        put32(&z, 0x04034b50);
        put16(&z, 20);
        put16(&z, 0);
        put16(&z, 0);
        put16(&z, 0);
        put16(&z, 0);
        put32(&z, 0);
        put32(&z, (uint32_t) dl);
        put32(&z, (uint32_t) dl);
        put16(&z, (unsigned) nl);
        put16(&z, 0);
        put(&z, files[i].name, nl);
        if (dl) put(&z, files[i].data, dl);
        put32(&cd, 0x02014b50);
        put16(&cd, 3 << 8 | 20); // made by Unix
        put16(&cd, 20);
        put16(&cd, 0);
        put16(&cd, 0);
        put16(&cd, 0);
        put16(&cd, 0);
        put32(&cd, 0);
        put32(&cd, (uint32_t) dl);
        put32(&cd, (uint32_t) dl);
        put16(&cd, (unsigned) nl);
        put16(&cd, 0);
        put16(&cd, 0);
        put16(&cd, 0);
        put16(&cd, 0);
        put32(&cd, (uint32_t) files[i].mode << 16);
        put32(&cd, off);
        put(&cd, files[i].name, nl);
    }
    const uint32_t cd_off = (uint32_t) z.n;
    put(&z, cd.d, cd.n);
    put32(&z, 0x06054b50);
    put16(&z, 0);
    put16(&z, 0);
    put16(&z, (unsigned) n);
    put16(&z, (unsigned) n);
    put32(&z, (uint32_t) cd.n);
    put32(&z, cd_off);
    put16(&z, 0);
    free(cd.d);
    return z;
}

static char g_scratch[1024];

static void path_in(char *out, const size_t n, const char *rel) {
    snprintf(out, n, "%s/%s", g_scratch, rel);
}

static int file_is(const char *path, const char *content) {
    char *data = NULL;
    size_t len = 0;
    if (!kn_fs_read(path, &data, &len, 1 << 20)) return 0;
    const int ok = len == strlen(content) && !memcmp(data, content, len);
    free(data);
    return ok;
}

static void write_file(const char *path, const char *content) {
    kn_fs_mkdirs_parent(path);
    FILE *f = kn_fs_fopen(path, "wb");
    if (!f) return;
    fputs(content, f);
    fclose(f);
}

static void test_archives(void) {
    char dest[1200], p[1400];
    Buf tar = {0};
    // a package is flat: BoxOffice and runtime/ at the top, no wrapping folder
    tar_entry(&tar, "BoxOffice", '0', 0755, "#!launcher", NULL);
    tar_entry(&tar, "runtime/", '5', 0755, NULL, NULL);
    tar_entry(&tar, "runtime/client_bin", '0', 0755, "#!new client", NULL);
    tar_entry(&tar, "runtime/ui/index.html", '0', 0644, "<html>", NULL);
    tar_entry(&tar, "runtime/assets/shaders/a.vert", '0', 0644, "void main(){}", NULL);
    tar_entry(&tar, "runtime/libx.so", '2', 0777, NULL, "libx.so.1");
    static const unsigned char end[1024] = {0};
    put(&tar, end, 1024);
    Buf gz = gzip_stored(&tar);
    path_in(dest, sizeof(dest), "tgz");
    char err[256] = "";
    CHECK(kn_archive_extract_mem(gz.d, gz.n, dest, err, sizeof(err)));
    if (err[0]) fprintf(stderr, "  tar.gz: %s\n", err);
    snprintf(p, sizeof(p), "%s/runtime/client_bin", dest);
    CHECK(file_is(p, "#!new client"));
    snprintf(p, sizeof(p), "%s/BoxOffice", dest);
    CHECK(file_is(p, "#!launcher"));
#ifndef _WIN32
    char out[64];
    char *argv[] = {"/bin/sh", "-c", "", NULL};
    char cmd[1500];
    snprintf(cmd, sizeof(cmd), "test -x '%s'", p);
    argv[2] = cmd;
    CHECK(kn_proc_run("/bin/sh", argv, 5000, out, sizeof(out)) == 0); // executable bit kept
    snprintf(p, sizeof(p), "%s/runtime/libx.so", dest);
    CHECK(kn_fs_is_symlink(p));
#endif
    snprintf(p, sizeof(p), "%s/runtime/ui/index.html", dest);
    CHECK(file_is(p, "<html>"));
    snprintf(p, sizeof(p), "%s/runtime/assets/shaders/a.vert", dest);
    CHECK(file_is(p, "void main(){}"));
    free(gz.d);
    free(tar.d);

    // traversal / escaping symlinks fail the whole package
    Buf evil = {0};
    tar_entry(&evil, "../evil", '0', 0644, "x", NULL);
    put(&evil, end, 1024);
    gz = gzip_stored(&evil);
    path_in(dest, sizeof(dest), "evil1");
    CHECK(!kn_archive_extract_mem(gz.d, gz.n, dest, err, sizeof(err)));
    path_in(p, sizeof(p), "evil");
    CHECK(!kn_fs_exists(p));
    free(gz.d);
    free(evil.d);
    evil = (Buf){0};
    tar_entry(&evil, "pkg/a", '0', 0644, "x", NULL);
    tar_entry(&evil, "pkg/link", '2', 0777, NULL, "../../etc/passwd");
    put(&evil, end, 1024);
    gz = gzip_stored(&evil);
    path_in(dest, sizeof(dest), "evil2");
    CHECK(!kn_archive_extract_mem(gz.d, gz.n, dest, err, sizeof(err)));
    free(gz.d);
    free(evil.d);
    static const unsigned char junk[] = "not an archive at all";
    CHECK(!kn_archive_extract_mem(junk, sizeof(junk), dest, err, sizeof(err)));

    // zip: top dir stripped, Unix exec bit, directories
    const ZipFile zf[] = {{"KrunkNative-win/", NULL, 040755},
                          {"KrunkNative-win/client.exe", "MZnew", 0100755},
                          {"KrunkNative-win/ui/", NULL, 040755},
                          {"KrunkNative-win/ui/index.html", "<zip>", 0100644}};
    Buf z = zip_stored(zf, 4);
    path_in(dest, sizeof(dest), "zip");
    CHECK(kn_archive_extract_mem(z.d, z.n, dest, err, sizeof(err)));
    snprintf(p, sizeof(p), "%s/client.exe", dest);
    CHECK(file_is(p, "MZnew"));
    snprintf(p, sizeof(p), "%s/ui/index.html", dest);
    CHECK(file_is(p, "<zip>"));
    free(z.d);
    const ZipFile zevil[] = {{"a/../../x", "bad", 0100644}};
    z = zip_stored(zevil, 1);
    path_in(dest, sizeof(dest), "zipevil");
    CHECK(!kn_archive_extract_mem(z.d, z.n, dest, err, sizeof(err)));
    free(z.d);
}

// ------------------------------------------------------------------ swap, rollback, recovery

static void make_install(KnInstall *in, const char *name) {
    memset(in, 0, sizeof(*in));
    path_in(in->root, sizeof(in->root), name);
    snprintf(in->marker_dir, sizeof(in->marker_dir), "%s/runtime", in->root); // the package's runtime/
    kn_fs_mkdirs(in->marker_dir);
    snprintf(in->exe_dir, sizeof(in->exe_dir), "%s", in->root);
    snprintf(in->state_dir, sizeof(in->state_dir), "%s/.kn-update", in->root);
    kn_fs_mkdirs(in->state_dir);
    in->has_marker = in->root_writable = in->self_updatable = 1;
}

static void at(char *out, const size_t n, const KnInstall *in, const char *rel) {
    snprintf(out, n, "%s/%s", in->root, rel);
}

static void test_swap(void) {
    KnInstall in;
    make_install(&in, "install");
    char p[KN_PATH_MAX], staged[KN_PATH_MAX];
    const char *old_files[][2] = {{"BoxOffice", "old launcher"}, {"runtime/client_bin", "old client"}, {"runtime/ui/index.html", "old ui"},
                                  {"runtime/ui/gone.html", "removed upstream"}, {"runtime/assets/textures/t.png", "content"},
                                  {"runtime/assets/shaders/a.vert", "old shader"}, {"runtime/assets/.kn-assets.json", "{}"},
                                  {"user.cfg", "mine"}};
    for (size_t i = 0; i < sizeof(old_files) / sizeof(old_files[0]); i++) {
        at(p, sizeof(p), &in, old_files[i][0]);
        write_file(p, old_files[i][1]);
    }
    snprintf(staged, sizeof(staged), "%s/staged", in.state_dir);
    const char *new_files[][2] = {{"BoxOffice", "new launcher"}, {"runtime/client_bin", "new client"}, {"runtime/ui/index.html", "new ui"},
                                  {"runtime/ui/added.html", "added"}, {"runtime/kn-install.json", "{\"format\":1}"},
                                  {"runtime/assets/shaders/a.vert", "new shader"}, {"runtime/server_bin", "new server"}};
    for (size_t i = 0; i < sizeof(new_files) / sizeof(new_files[0]); i++) {
        snprintf(p, sizeof(p), "%s/%s", staged, new_files[i][0]);
        write_file(p, new_files[i][1]);
    }
    char err[256] = "";
    CHECK(kn_install_swap(&in, staged, err, sizeof(err)));
    if (err[0]) fprintf(stderr, "  swap: %s\n", err);
    at(p, sizeof(p), &in, "runtime/client_bin");
    CHECK(file_is(p, "new client"));
    at(p, sizeof(p), &in, "BoxOffice");
    CHECK(file_is(p, "new launcher"));
    at(p, sizeof(p), &in, "runtime/server_bin");
    CHECK(file_is(p, "new server"));
    at(p, sizeof(p), &in, "runtime/ui/index.html");
    CHECK(file_is(p, "new ui"));
    at(p, sizeof(p), &in, "runtime/ui/added.html");
    CHECK(file_is(p, "added"));
    at(p, sizeof(p), &in, "runtime/ui/gone.html"); // ui/ is replaced as a whole
    CHECK(!kn_fs_exists(p));
    at(p, sizeof(p), &in, "runtime/assets/shaders/a.vert"); // the package's own asset file
    CHECK(file_is(p, "new shader"));
    at(p, sizeof(p), &in, "runtime/assets/textures/t.png"); // game content: untouched
    CHECK(file_is(p, "content"));
    at(p, sizeof(p), &in, "runtime/assets/.kn-assets.json");
    CHECK(file_is(p, "{}"));
    at(p, sizeof(p), &in, "user.cfg");
    CHECK(file_is(p, "mine"));
    CHECK(!kn_install_recover(&in)); // committed: nothing to roll back
    kn_install_cleanup(&in);
    CHECK(!kn_fs_exists(in.state_dir));

    // a failing swap rolls everything back: "runtime/lib" is a file, the package wants runtime/lib/x.so
    KnInstall bad;
    make_install(&bad, "install2");
    at(p, sizeof(p), &bad, "runtime/client_bin");
    write_file(p, "old client");
    at(p, sizeof(p), &bad, "runtime/lib");
    write_file(p, "a file");
    at(p, sizeof(p), &bad, "runtime/ui/index.html");
    write_file(p, "old ui");
    snprintf(staged, sizeof(staged), "%s/staged", bad.state_dir);
    snprintf(p, sizeof(p), "%s/runtime/client_bin", staged);
    write_file(p, "new client");
    snprintf(p, sizeof(p), "%s/runtime/ui/index.html", staged);
    write_file(p, "new ui");
    snprintf(p, sizeof(p), "%s/runtime/kn-install.json", staged);
    write_file(p, "{}");
    snprintf(p, sizeof(p), "%s/runtime/lib/x.so", staged);
    write_file(p, "lib");
    err[0] = 0;
    CHECK(!kn_install_swap(&bad, staged, err, sizeof(err)));
    CHECK(err[0] != 0);
    at(p, sizeof(p), &bad, "runtime/client_bin");
    CHECK(file_is(p, "old client"));
    at(p, sizeof(p), &bad, "runtime/ui/index.html");
    CHECK(file_is(p, "old ui"));
    at(p, sizeof(p), &bad, "runtime/lib");
    CHECK(file_is(p, "a file"));

    // crash recovery: a journal without its commit line is rolled back at the next start
    KnInstall crash;
    make_install(&crash, "install3");
    at(p, sizeof(p), &crash, "runtime/client_bin");
    write_file(p, "old client");
    snprintf(staged, sizeof(staged), "%s/staged", crash.state_dir);
    char bak[KN_PATH_MAX], neu[KN_PATH_MAX];
    snprintf(bak, sizeof(bak), "%s/backup/runtime/client_bin", crash.state_dir);
    snprintf(neu, sizeof(neu), "%s/runtime/client_bin", staged);
    write_file(neu, "new client");
    kn_fs_mkdirs_parent(bak);
    kn_fs_rename(p, bak); // B done
    kn_fs_rename(neu, p); // N done, then "crash"
    char journal[KN_PATH_MAX];
    snprintf(journal, sizeof(journal), "%s/journal", crash.state_dir);
    char text[KN_PATH_MAX + 64];
    snprintf(text, sizeof(text), "S %s\nB runtime/client_bin\nN runtime/client_bin\n", staged);
    write_file(journal, text);
    CHECK(kn_install_recover(&crash));
    CHECK(file_is(p, "old client"));
    CHECK(!kn_fs_exists(journal));

    // package assets mirrored into a separate content dir, only where they differ
    KnInstall pk;
    make_install(&pk, "install4");
    at(p, sizeof(p), &pk, "runtime/assets/shaders/a.vert");
    write_file(p, "shader v2");
    at(p, sizeof(p), &pk, "runtime/assets/maps/m.json");
    write_file(p, "{}");
    char data[KN_PATH_MAX];
    path_in(data, sizeof(data), "data-assets");
    snprintf(p, sizeof(p), "%s/maps/m.json", data);
    write_file(p, "{}");
    snprintf(p, sizeof(p), "%s/textures/big.png", data);
    write_file(p, "content");
    CHECK(kn_install_package_assets(&pk, data, err, sizeof(err)) == 1); // a.vert only
    snprintf(p, sizeof(p), "%s/shaders/a.vert", data);
    CHECK(file_is(p, "shader v2"));
    CHECK(kn_install_package_assets(&pk, data, err, sizeof(err)) == 0); // nothing differs now
    at(p, sizeof(p), &pk, "runtime/assets");
    CHECK(kn_install_package_assets(&pk, p, err, sizeof(err)) == 0);    // same dir: nothing
}

// kn_install_dir_in_checkout: a content dir inside a git checkout is refused (the sync deletes
// what it does not list); one outside is not, whatever the cwd (the Windows launcher walked from
// "C:" to "." and found the cwd's .git: started from a checkout, it refused every install).
static void test_checkout(void) {
    char repo[1024], assets[1024], git[1024], plain[1024], root[1024] = "", cwd[1024] = "";
    kn_fs_join(repo, sizeof(repo), g_scratch, "repo");
    kn_fs_join(git, sizeof(git), repo, ".git");
    kn_fs_join(assets, sizeof(assets), repo, "assets/textures");
    kn_fs_join(plain, sizeof(plain), g_scratch, "install/assets");
    CHECK(kn_fs_mkdirs(git) && kn_fs_mkdirs(assets) && kn_fs_mkdirs(plain));
    CHECK(kn_install_dir_in_checkout(assets, root, sizeof(root)));
    CHECK(strstr(root, "repo") != NULL);
    CHECK(!kn_install_dir_in_checkout(plain, root, sizeof(root)));
    CHECK(getcwd(cwd, sizeof(cwd)) != NULL);
    CHECK(chdir(repo) == 0); // the cwd is a checkout: an install elsewhere is still not in it
    CHECK(!kn_install_dir_in_checkout(plain, root, sizeof(root)));
    CHECK(chdir(cwd) == 0);
}

int main(int argc, char **argv) {
    if (argc > 1) snprintf(g_scratch, sizeof(g_scratch), "%s", argv[1]);
    else {
        const char *tmp = getenv("TMPDIR");
#ifdef _WIN32
        if (!tmp) tmp = getenv("TEMP");
#endif
        snprintf(g_scratch, sizeof(g_scratch), "%s/kn-update-test-%ld", tmp && *tmp ? tmp : "/tmp", (long) time(NULL));
    }
    kn_fs_rmtree(g_scratch);
    kn_fs_mkdirs(g_scratch);
    test_urls();
    test_origin_specs();
    test_manifest();
    test_compare();
    test_assets();
    test_marker();
    test_checkout();
    test_archives();
    test_swap();
    if (!g_fail) kn_fs_rmtree(g_scratch);
    printf("update_test: %d/%d checks passed%s%s\n", g_checks - g_fail, g_checks, g_fail ? ", scratch kept in " : "",
           g_fail ? g_scratch : "");
    return g_fail ? 1 : 0;
}
