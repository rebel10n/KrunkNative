// asset_fetch_test: the native on-demand content fetch's queue (client/include/asset_fetch.h) over
// an in-memory transport: manifest scoping, origin order and fallback, size + sha256 verification
// (a mismatch is refused, nothing written), atomic writes, 404 everywhere given up once, transport
// failures retried with backoff, de-duplication, the concurrency cap, content-addressed origins, a
// size mismatch of a present file refetched, the git checkout guard (root and per directory) and
// stopping with a transfer in flight, and the split CDN's only=/skip= origins.
#include <asset_fetch.h>
#include <kn_fs.h>
#include <kn_update.h>

#include <auth_join.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int g_failed, g_checks;
#define CHECK(cond, ...)                                                                                              \
    do {                                                                                                              \
        g_checks++;                                                                                                   \
        if (!(cond)) {                                                                                                \
            g_failed++;                                                                                               \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                                                      \
            fprintf(stderr, __VA_ARGS__);                                                                             \
            fputc('\n', stderr);                                                                                      \
        }                                                                                                             \
    } while (0)

// ------------------------------------------------------------------ the fake transport

#define ROUTES_MAX 32
typedef struct {
    char url[600];
    int status;          // 0 = transport error
    const char *body;
    int fail_first;      // this many transport errors before answering
    int hits;
} Route;

static struct {
    pthread_mutex_t lock;
    Route routes[ROUTES_MAX];
    int count;
    int requests, inflight, max_inflight;
    int delay_ms;
    char last_urls[64][600];
    int url_count;
} T = {.lock = PTHREAD_MUTEX_INITIALIZER};

static void routes_reset(void) {
    pthread_mutex_lock(&T.lock);
    T.count = T.requests = T.inflight = T.max_inflight = T.delay_ms = T.url_count = 0;
    pthread_mutex_unlock(&T.lock);
}

static void route(const char *url, const int status, const char *body, const int fail_first) {
    Route *r = &T.routes[T.count++];
    snprintf(r->url, sizeof(r->url), "%s", url);
    r->status = status;
    r->body = body;
    r->fail_first = fail_first;
    r->hits = 0;
}

static void sleep_ms(const int ms) {
    const struct timespec ts = {ms / 1000, (long) (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

static int fake_get(void *user, const char *url, KnHttpSink *sink, char *err, const size_t errn) {
    (void) user;
    pthread_mutex_lock(&T.lock);
    T.requests++;
    if (T.url_count < 64) snprintf(T.last_urls[T.url_count++], sizeof(T.last_urls[0]), "%s", url);
    T.inflight++;
    if (T.inflight > T.max_inflight) T.max_inflight = T.inflight;
    const int delay = T.delay_ms;
    Route *r = NULL;
    for (int i = 0; i < T.count; i++)
        if (!strcmp(T.routes[i].url, url)) r = &T.routes[i];
    int status = 404, transport = 0;
    const char *body = "not found";
    if (r) {
        r->hits++;
        transport = r->hits <= r->fail_first || r->status == 0;
        status = r->status;
        body = r->body;
    }
    pthread_mutex_unlock(&T.lock);
    for (int waited = 0; waited < delay; waited += 5) {
        if (sink->cancel && *sink->cancel) break;
        sleep_ms(5);
    }
    pthread_mutex_lock(&T.lock);
    T.inflight--;
    pthread_mutex_unlock(&T.lock);
    if (sink->cancel && *sink->cancel) {
        snprintf(err, errn, "cancelled");
        return 0;
    }
    if (transport) {
        snprintf(err, errn, "connection refused");
        return 0;
    }
    sink->status = status;
    if (status >= 200 && status < 300 && body) {
        // in two pieces, like a socket
        const size_t len = strlen(body), half = len / 2;
        if (!sink->write(sink->user, body, half) || !sink->write(sink->user, body + half, len - half)) {
            snprintf(err, errn, "aborted by the sink");
            return 0;
        }
    }
    return status;
}

// ------------------------------------------------------------------ fixtures

static char g_root[1024];

static void sha_hex(const char *s, char out[65]) {
    uint8_t d[32];
    kn_sha256_bytes(s, strlen(s), d);
    kn_sha256_hex(d, out);
}

static void write_file(const char *path, const char *text) {
    kn_fs_mkdirs_parent(path);
    FILE *f = fopen(path, "wb");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static char *read_text(const char *path) {
    char *data = NULL;
    size_t len = 0;
    return kn_fs_read(path, &data, &len, 1 << 20) ? data : NULL;
}

typedef struct {
    const char *path, *content;
} ManifestFile;

// <dir>/.kn-assets.json listing files (sorted by the caller) with the content's size and sha256
static void write_manifest(const char *dir, const ManifestFile *files, const int n) {
    char json[8192] = "{\"format\":1,\"version\":\"test\",\"files\":[";
    for (int i = 0; i < n; i++) {
        char hex[65], item[512];
        sha_hex(files[i].content, hex);
        snprintf(item, sizeof(item), "%s[\"%s\",\"%s\",%zu]", i ? "," : "", files[i].path, hex, strlen(files[i].content));
        strcat(json, item);
    }
    strcat(json, "]}");
    char path[1200];
    snprintf(path, sizeof(path), "%s/.kn-assets.json", dir);
    write_file(path, json);
}

static void make_dir(char *out, const size_t n, const char *name) {
    snprintf(out, n, "%s/%s", g_root, name);
    kn_fs_rmtree(out);
    kn_fs_mkdirs(out);
}

static char g_arrived[16][1200];
static int g_arrived_count;
static void on_arrived(void *user, const char *path) {
    (void) user;
    if (g_arrived_count < 16) snprintf(g_arrived[g_arrived_count++], sizeof(g_arrived[0]), "%s", path);
}

// asset_fetch_request until it is no longer pending (wait_idle + the answer)
static int settle(const char *path) {
    asset_fetch_wait_idle(5000);
    return asset_fetch_request(path, 0);
}

static int has_part_files(const char *dir) {
    char cmd[1400];
    snprintf(cmd, sizeof(cmd), "find '%s' -name '*.part' | grep -q .", dir);
    return system(cmd) == 0;
}

#define A "https://a.test/assets/"
#define B "https://b.test/assets/"

static AssetFetchConfig config(const char *dir) {
    return (AssetFetchConfig) {.assets_dir = dir, .origins = A " " B, .require_manifest = 1, .backoff_ms = 10,
                               .get = fake_get, .workers = 2};
}

// ------------------------------------------------------------------ tests

static void test_off(void) {
    printf("off without a manifest / inside a checkout...\n");
    char dir[1100], why[1400];
    make_dir(dir, sizeof(dir), "nomanifest");
    AssetFetchConfig cfg = config(dir);
    CHECK(!asset_fetch_start(&cfg, why, sizeof(why)) && strstr(why, "manifest"), "no manifest: off (%s)", why);
    CHECK(asset_fetch_request("/x/models/a.obj", 1) == 1, "not running: read it");
    cfg.require_manifest = 0;
    CHECK(asset_fetch_start(&cfg, why, sizeof(why)), "forced: on (%s)", why);
    asset_fetch_stop();

    char repo[1100], git[1200];
    make_dir(repo, sizeof(repo), "repo");
    snprintf(git, sizeof(git), "%s/.git", repo);
    kn_fs_mkdirs(git);
    snprintf(dir, sizeof(dir), "%s/assets", repo);
    kn_fs_mkdirs(dir);
    write_manifest(dir, NULL, 0);
    cfg = config(dir);
    CHECK(!asset_fetch_start(&cfg, why, sizeof(why)) && strstr(why, "git checkout"), "inside a checkout: off (%s)", why);
    cfg.allow_checkout = 1;
    CHECK(asset_fetch_start(&cfg, why, sizeof(why)), "allow_checkout: on");
    asset_fetch_stop();
}

static void test_fetch(void) {
    printf("fetch, verify, fallback, refuse, give up...\n");
    char dir[1100], why[1400], p_ok[1200], p_bad[1200], p_404[1200], p_other[1200], p_stale[1200];
    make_dir(dir, sizeof(dir), "content");
    const ManifestFile files[] = {
        {"models/adrp_0.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n"},
        {"sound/adrop_1.mp3", "ID3 claim"},
        {"textures/adrp_0.png", "PNG crate"},
        {"textures/adrp_1.png", "PNG landed"},
        {"textures/stale.png", "the right content"},
    };
    write_manifest(dir, files, 5);
    snprintf(p_ok, sizeof(p_ok), "%s/models/adrp_0.obj", dir);
    snprintf(p_bad, sizeof(p_bad), "%s/textures/adrp_0.png", dir);
    snprintf(p_404, sizeof(p_404), "%s/textures/adrp_1.png", dir);
    snprintf(p_other, sizeof(p_other), "%s/textures/not_listed.png", dir);
    snprintf(p_stale, sizeof(p_stale), "%s/textures/stale.png", dir);
    write_file(p_stale, "trunc"); // present, wrong size
    routes_reset();
    route(A "models/adrp_0.obj", 404, NULL, 0); // A lacks it: B has it
    route(B "models/adrp_0.obj", 200, files[0].content, 0);
    route(A "textures/adrp_0.png", 200, "PNG crate but wrong", 0); // both serve other content
    route(B "textures/adrp_0.png", 200, "PNG crat3", 0);          // same size, other bytes
    route(A "textures/stale.png", 200, files[4].content, 0);

    AssetFetchConfig cfg = config(dir);
    CHECK(asset_fetch_start(&cfg, why, sizeof(why)), "start (%s)", why);
    CHECK(asset_fetch_request(p_other, 1) == 1, "a path the manifest does not list is not ours");
    char outside[1300];
    snprintf(outside, sizeof(outside), "%s-elsewhere/models/adrp_0.obj", dir);
    CHECK(asset_fetch_request(outside, 1) == 1, "outside the assets dir: not ours");
    char hidden[1200];
    snprintf(hidden, sizeof(hidden), "%s/.kn-sync/x.part", dir);
    CHECK(asset_fetch_request(hidden, 1) == 1, "the launcher's own files: not ours");

    CHECK(asset_fetch_request(p_ok, 0) == -1, "enqueue 0: unknown and missing");
    CHECK(asset_fetch_request(p_ok, 1) == 0, "missing: queued");
    for (int i = 0; i < 20; i++) asset_fetch_request(p_ok, 1); // de-duplicated
    CHECK(asset_fetch_request(p_bad, 1) == 0, "queued");
    CHECK(asset_fetch_request(p_404, 1) == 0, "queued");
    CHECK(asset_fetch_request(p_stale, 1) == 0, "present with the wrong size: refetched");

    CHECK(settle(p_ok) == 1, "fetched");
    char *got = read_text(p_ok);
    CHECK(got && !strcmp(got, files[0].content), "the file's content (%s)", got ? got : "(none)");
    free(got);
    CHECK(settle(p_bad) == -1, "a checksum mismatch on every origin is given up");
    CHECK(!kn_fs_exists(p_bad), "nothing written for a refused download");
    CHECK(settle(p_404) == -1, "404 on both origins: given up");
    got = read_text(p_stale);
    CHECK(got && !strcmp(got, files[4].content), "the truncated copy was replaced (%s)", got ? got : "(none)");
    free(got);
    CHECK(!has_part_files(dir), "no partial files left");

    AssetFetchStats st;
    asset_fetch_stats(&st);
    CHECK(st.fetched == 2 && st.failed == 2 && st.refused == 2 && st.queued == 4 && st.pending == 0,
          "stats fetched %zu failed %zu refused %zu queued %zu pending %zu", st.fetched, st.failed, st.refused, st.queued, st.pending);
    // ok: A 404 + B; bad: A + B; 404: A + B; stale: A
    CHECK(T.requests == 7, "requests %d (one download per file and origin)", T.requests);
    const int before = T.requests;
    CHECK(asset_fetch_request(p_404, 1) == -1 && asset_fetch_request(p_bad, 1) == -1, "given up stays given up");
    sleep_ms(50);
    CHECK(T.requests == before, "no new requests for given-up files");

    g_arrived_count = 0;
    CHECK(asset_fetch_poll(on_arrived, NULL) == 2 && g_arrived_count == 2, "poll: two arrivals (%d)", g_arrived_count);
    CHECK(asset_fetch_poll(on_arrived, NULL) == 0, "poll: once");
    int saw_ok = 0;
    for (int i = 0; i < g_arrived_count; i++) saw_ok |= !strcmp(g_arrived[i], p_ok);
    CHECK(saw_ok, "the arrived path is the full path");
    CHECK(asset_fetch_request(p_ok, 1) == 1, "after the poll: read it");
    asset_fetch_stop();
}

static void test_retry_and_cap(void) {
    printf("transport failures, backoff, concurrency cap...\n");
    char dir[1100], why[1400];
    make_dir(dir, sizeof(dir), "retry");
    ManifestFile files[8];
    static char names[8][64], bodies[8][64];
    for (int i = 0; i < 8; i++) {
        snprintf(names[i], sizeof(names[i]), "sound/clip_%d.mp3", i);
        snprintf(bodies[i], sizeof(bodies[i]), "sound %d", i);
        files[i] = (ManifestFile) {names[i], bodies[i]};
    }
    write_manifest(dir, files, 8);
    routes_reset();
    T.delay_ms = 30;
    char url[600];
    for (int i = 0; i < 8; i++) {
        snprintf(url, sizeof(url), A "%s", names[i]);
        route(url, 200, bodies[i], i == 0 ? 2 : 0); // clip 0: A refuses connections twice
        snprintf(url, sizeof(url), B "%s", names[i]);
        route(url, 0, NULL, 0); // B is down
    }
    AssetFetchConfig cfg = config(dir);
    cfg.workers = 3;
    CHECK(asset_fetch_start(&cfg, why, sizeof(why)), "start");
    char paths[8][1200];
    for (int i = 0; i < 8; i++) {
        snprintf(paths[i], sizeof(paths[i]), "%s/%s", dir, names[i]);
        CHECK(asset_fetch_request(paths[i], 1) == 0, "queued %d", i);
    }
    CHECK(asset_fetch_wait_idle(10000), "idle");
    int ok = 0;
    for (int i = 0; i < 8; i++) ok += asset_fetch_request(paths[i], 0) == 1;
    CHECK(ok == 8, "every clip arrived (%d), clip 0 after retries", ok);
    CHECK(T.max_inflight <= 3 && T.max_inflight >= 2, "at most 3 transfers at once (%d)", T.max_inflight);
    AssetFetchStats st;
    asset_fetch_stats(&st);
    CHECK(st.fetched == 8 && st.failed == 0, "fetched %zu failed %zu", st.fetched, st.failed);
    asset_fetch_stop();

    // every origin unreachable: ASSET_FETCH_ATTEMPTS rounds, then given up
    make_dir(dir, sizeof(dir), "down");
    write_manifest(dir, files, 1);
    routes_reset();
    snprintf(url, sizeof(url), A "%s", names[0]);
    route(url, 0, NULL, 0);
    snprintf(url, sizeof(url), B "%s", names[0]);
    route(url, 0, NULL, 0);
    cfg = config(dir);
    CHECK(asset_fetch_start(&cfg, why, sizeof(why)), "start");
    snprintf(paths[0], sizeof(paths[0]), "%s/%s", dir, names[0]);
    asset_fetch_request(paths[0], 1);
    CHECK(settle(paths[0]) == -1, "unreachable origins: given up");
    CHECK(T.requests == 2 * ASSET_FETCH_ATTEMPTS, "%d requests over %d rounds", T.requests, ASSET_FETCH_ATTEMPTS);
    if (T.requests != 2 * ASSET_FETCH_ATTEMPTS)
        for (int i = 0; i < T.url_count; i++) fprintf(stderr, "  %s\n", T.last_urls[i]);
    asset_fetch_stop();
}

static void test_objects_and_guard(void) {
    printf("content-addressed origin, per-directory checkout guard, stop in flight...\n");
    char dir[1100], why[1400], path[1200];
    make_dir(dir, sizeof(dir), "objects");
    const ManifestFile files[] = {{"models/teddy_0.obj", "teddy"}, {"textures/linked/a.png", "linked"}};
    write_manifest(dir, files, 2);
    char hex[65], url[600];
    sha_hex("teddy", hex);
    routes_reset();
    snprintf(url, sizeof(url), "https://o.test/objects/%.2s/%s", hex, hex);
    route(url, 200, "teddy", 0);
    route(A "textures/linked/a.png", 200, "linked", 0);
    // textures/linked -> a directory inside a git checkout: never written through the link
    char repo[1100], git[1200], target[1200], link[1200];
    make_dir(repo, sizeof(repo), "repo2");
    snprintf(git, sizeof(git), "%s/.git", repo);
    kn_fs_mkdirs(git);
    snprintf(target, sizeof(target), "%s/assets/textures/linked", repo);
    kn_fs_mkdirs(target);
    snprintf(link, sizeof(link), "%s/textures", dir);
    kn_fs_mkdirs(link);
    snprintf(link, sizeof(link), "%s/textures/linked", dir);
    CHECK(kn_fs_symlink(target, link), "symlink");

    AssetFetchConfig cfg = config(dir);
    cfg.origins = "objects:https://o.test/objects/ " A;
    CHECK(asset_fetch_start(&cfg, why, sizeof(why)), "start");
    snprintf(path, sizeof(path), "%s/models/teddy_0.obj", dir);
    asset_fetch_request(path, 1);
    CHECK(settle(path) == 1, "fetched from the objects origin");
    CHECK(T.url_count >= 1 && !strcmp(T.last_urls[0], url), "objects URL %s", T.url_count ? T.last_urls[0] : "-");
    snprintf(path, sizeof(path), "%s/textures/linked/a.png", dir);
    asset_fetch_request(path, 1);
    CHECK(settle(path) == -1, "a directory that resolves into a checkout is refused");
    char inside[1300];
    snprintf(inside, sizeof(inside), "%s/a.png", target);
    CHECK(!kn_fs_exists(inside), "nothing written into the checkout");
    asset_fetch_stop();

    // stop with a slow transfer in flight: returns promptly
    make_dir(dir, sizeof(dir), "stop");
    write_manifest(dir, files, 1);
    routes_reset();
    T.delay_ms = 10000;
    route(A "models/teddy_0.obj", 200, "teddy", 0);
    cfg = config(dir);
    CHECK(asset_fetch_start(&cfg, why, sizeof(why)), "start");
    snprintf(path, sizeof(path), "%s/models/teddy_0.obj", dir);
    asset_fetch_request(path, 1);
    sleep_ms(50);
    const time_t t0 = time(NULL);
    asset_fetch_stop();
    CHECK(time(NULL) - t0 <= 2, "stop aborts the transfer");
    CHECK(!asset_fetch_running(), "stopped");
    CHECK(!has_part_files(dir), "no partial file after the abort");
}

// the split Pages CDN (deploy/asset-shards.json, as the launcher hands it down): each file from
// its own project only, kru's objects as the fallback
static void test_split(void) {
    printf("split CDN: only= / skip= origins, objects fallback...\n");
    char dir[1100], why[1400], p_a[1200], p_b[1200], p_down[1200];
    make_dir(dir, sizeof(dir), "split");
    const ManifestFile files[] = {
        {"models/hats/hat_1.obj", "hat model"},
        {"textures/hats/hat_1.png", "hat texture"},
        {"textures/hatsX/odd.png", "not a hats dir"},
    };
    write_manifest(dir, files, 3);
    char hex[65], obj[600];
    sha_hex(files[2].content, hex);
    snprintf(obj, sizeof(obj), "https://o.test/objects/%.2s/%s", hex, hex);
    routes_reset();
    route(A "models/hats/hat_1.obj", 200, files[0].content, 0);
    route(B "textures/hats/hat_1.png", 200, files[1].content, 0);
    route(A "textures/hatsX/odd.png", 0, NULL, 0); // project a is down for it: the objects serve it
    route(obj, 200, files[2].content, 0);
    AssetFetchConfig cfg = config(dir);
    cfg.origins = "skip=textures/hats/+textures/previews/@" A " only=textures/hats/+textures/previews/@" B
                  " objects:https://o.test/objects/";
    CHECK(asset_fetch_start(&cfg, why, sizeof(why)), "start (%s)", why);
    snprintf(p_a, sizeof(p_a), "%s/%s", dir, files[0].path);
    snprintf(p_b, sizeof(p_b), "%s/%s", dir, files[1].path);
    snprintf(p_down, sizeof(p_down), "%s/%s", dir, files[2].path);
    asset_fetch_request(p_a, 1);
    CHECK(settle(p_a) == 1, "a's file fetched");
    CHECK(T.requests == 1 && !strcmp(T.last_urls[0], A "models/hats/hat_1.obj"), "from a only (%s)", T.url_count ? T.last_urls[0] : "-");
    asset_fetch_request(p_b, 1);
    CHECK(settle(p_b) == 1, "b's file fetched");
    CHECK(T.requests == 2 && !strcmp(T.last_urls[1], B "textures/hats/hat_1.png"), "from b directly, a not asked (%s)",
          T.url_count > 1 ? T.last_urls[1] : "-");
    asset_fetch_request(p_down, 1);
    CHECK(settle(p_down) == 1, "a prefix that only looks like b's: a, then the objects");
    int asked_b = 0;
    for (int i = 0; i < T.url_count; i++) asked_b += !strncmp(T.last_urls[i], B, strlen(B));
    CHECK(T.requests >= 4 && asked_b == 1, "b never asked for a's paths (%d requests, %d to b)", T.requests, asked_b);
    asset_fetch_stop();
}

int main(void) {
    const char *base = getenv("XDG_CACHE_HOME");
    char tmpl[900];
    snprintf(tmpl, sizeof(tmpl), "%s/kn-asset-fetch-test-XXXXXX", base && *base ? base : "/tmp");
    if (!mkdtemp(tmpl)) {
        snprintf(tmpl, sizeof(tmpl), "/tmp/kn-asset-fetch-test-XXXXXX");
        if (!mkdtemp(tmpl)) return 1;
    }
    snprintf(g_root, sizeof(g_root), "%s", tmpl);
    test_off();
    test_fetch();
    test_retry_and_cap();
    test_objects_and_guard();
    test_split();
    kn_fs_rmtree(g_root);
    printf("asset_fetch_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
