// asset_fetch_test: the native on-demand content fetch's queue (client/include/asset_fetch.h) over
// an in-memory transport: manifest scoping, origin order and fallback, size + sha256 verification
// (a mismatch is refused, nothing written), atomic writes, 404 everywhere given up once, transport
// failures retried with backoff, de-duplication, the concurrency cap, content-addressed origins, a
// size mismatch of a present file refetched, the git checkout guard (root and per directory) and
// stopping with a transfer in flight, the split CDN's only=/skip= origins, and Krunker user assets
// (their own origins, user-assets-only mode, 403s, block pages, inside a checkout, beside a manifest),
// and the content-addressed cache (a missing file restored without a request, damaged objects refetched).
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

#define UK "https://krunker.test/"
#define UM "https://mirror.test/api/user-assets/"

// Krunker user assets (user_asset_path.h): their own origins (<url><id>/<file>), also without a
// manifest and inside a checkout (user-assets/ is git-ignored); a 403 takes an origin out; an HTML
// page or a wrong kind of file is refused; with a manifest the listed ones keep the content origins.
static void test_user_assets(void) {
    printf("user assets: own origins, ua-only mode, 403, block pages, checkout...\n");
    char dir[1100], why[1400], tex[1200], obj[1200], blocked[1200], other[1200], bad[1200], badname[1200];
    make_dir(dir, sizeof(dir), "ua");
    snprintf(tex, sizeof(tex), "%s/user-assets/9356/texture.png", dir);
    snprintf(obj, sizeof(obj), "%s/user-assets/7282/model.obj", dir);
    snprintf(blocked, sizeof(blocked), "%s/user-assets/5000/texture.png", dir);
    snprintf(other, sizeof(other), "%s/textures/wall_0.png", dir);
    snprintf(bad, sizeof(bad), "%s/user-assets/12a/texture.png", dir);
    snprintf(badname, sizeof(badname), "%s/user-assets/9356/run.sh", dir);
    routes_reset();
    route(UK "9356/texture.png", 403, "<!DOCTYPE html><title>Krunker.io</title>", 0);
    route(UK "7282/model.obj", 403, "<!DOCTYPE html>", 0);
    route(UK "5000/texture.png", 403, "<!DOCTYPE html>", 0);
    route(UM "9356/texture.png", 200, "\x89PNG\r\n\x1a\n texture", 0);
    route(UM "7282/model.obj", 200, "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n", 0);
    route(UM "5000/texture.png", 200, "<!DOCTYPE html><html>blocked</html>", 0);

    AssetFetchConfig cfg = config(dir); // no manifest, required: the content fetch is off
    cfg.user_assets = 1;
    cfg.user_asset_origins = UK " " UM;
    cfg.workers = 1;
    CHECK(asset_fetch_start(&cfg, why, sizeof(why)), "no manifest: user assets alone (%s)", why);
    AssetFetchStats st;
    asset_fetch_stats(&st);
    CHECK(st.user_assets_only && !st.has_manifest, "stats: user assets only");
    CHECK(asset_fetch_request(other, 1) == 1, "game content: not fetched in ua-only mode");
    CHECK(asset_fetch_request(bad, 1) == 1 && asset_fetch_request(badname, 1) == 1, "not user-asset paths: not ours");
    CHECK(asset_fetch_request(tex, 1) == 0, "a user asset: queued");
    CHECK(settle(tex) == 1, "fetched");
    char *got = read_text(tex);
    CHECK(got && strstr(got, "texture"), "written (%s)", got ? got : "(none)");
    free(got);
    CHECK(T.url_count >= 2 && !strcmp(T.last_urls[0], UK "9356/texture.png") && !strcmp(T.last_urls[1], UM "9356/texture.png"),
          "Krunker's host first, then the mirror, <url><id>/<file> (%s, %s)", T.last_urls[0], T.last_urls[1]);
    CHECK(asset_fetch_request(obj, 1) == 0 && settle(obj) == 1, "the next file");
    int krunker_obj = 0;
    for (int i = 0; i < T.url_count; i++) krunker_obj |= !strcmp(T.last_urls[i], UK "7282/model.obj");
    CHECK(!krunker_obj, "the 403'ing host is out: the mirror is asked first");
    CHECK(asset_fetch_request(blocked, 1) == 0 && settle(blocked) == -1, "an HTML page answered 200 is refused");
    CHECK(!kn_fs_exists(blocked), "nothing written for it");
    asset_fetch_stats(&st);
    CHECK(st.refused == 1 && st.fetched == 2, "stats refused %zu fetched %zu", st.refused, st.fetched);
    CHECK(!has_part_files(dir), "no partial files left");
    asset_fetch_stop();

    // off: user_assets 0 keeps the old rule (no manifest: off)
    cfg.user_assets = 0;
    CHECK(!asset_fetch_start(&cfg, why, sizeof(why)), "user_assets 0: off without a manifest");

    // inside a checkout: user assets still (user-assets/ is git-ignored), nothing else
    char repo[1100], git[1200], rdir[1200], rtex[1300], rother[1300];
    make_dir(repo, sizeof(repo), "uarepo");
    snprintf(git, sizeof(git), "%s/.git", repo);
    kn_fs_mkdirs(git);
    snprintf(rdir, sizeof(rdir), "%s/assets", repo);
    kn_fs_mkdirs(rdir);
    snprintf(rtex, sizeof(rtex), "%s/user-assets/9356/texture.png", rdir);
    snprintf(rother, sizeof(rother), "%s/textures/wall_0.png", rdir);
    cfg = config(rdir);
    cfg.user_assets = 1;
    cfg.user_asset_origins = UM;
    CHECK(asset_fetch_start(&cfg, why, sizeof(why)), "inside a checkout: user assets (%s)", why);
    CHECK(asset_fetch_request(rother, 1) == 1, "inside a checkout: no game content");
    CHECK(asset_fetch_request(rtex, 1) == 0 && settle(rtex) == 1, "inside a checkout: a user asset is written");
    asset_fetch_stop();

    // with a manifest: a listed user asset comes from the content origins, an unlisted one from its own
    char mdir[1100], listed[1200], unlisted[1200];
    make_dir(mdir, sizeof(mdir), "uamanifest");
    const ManifestFile files[] = {{"user-assets/52268/texture.png", "PNG listed"}};
    write_manifest(mdir, files, 1);
    snprintf(listed, sizeof(listed), "%s/user-assets/52268/texture.png", mdir);
    snprintf(unlisted, sizeof(unlisted), "%s/user-assets/9356/texture.png", mdir);
    routes_reset();
    route(A "user-assets/52268/texture.png", 200, "PNG listed", 0);
    route(UM "9356/texture.png", 200, "\x89PNG\r\n\x1a\n texture", 0);
    cfg = config(mdir);
    cfg.user_assets = 1;
    cfg.user_asset_origins = UM;
    CHECK(asset_fetch_start(&cfg, why, sizeof(why)), "with a manifest (%s)", why);
    asset_fetch_stats(&st);
    CHECK(!st.user_assets_only && st.has_manifest, "the content fetch runs too");
    CHECK(asset_fetch_request(listed, 1) == 0 && settle(listed) == 1, "a listed user asset");
    CHECK(asset_fetch_request(unlisted, 1) == 0 && settle(unlisted) == 1, "an unlisted user asset");
    CHECK(T.url_count == 2 && !strcmp(T.last_urls[0], A "user-assets/52268/texture.png") && !strcmp(T.last_urls[1], UM "9356/texture.png"),
          "listed: the content origin; unlisted: the user-asset origin (%s, %s)", T.last_urls[0], T.last_urls[1]);
    asset_fetch_stop();
}

// A Krunker user asset (a map's model) whose hosts fail for longer than the content budget: both
// origins answer transport errors for ASSET_FETCH_ATTEMPTS + 1 rounds, then the mirror answers. The
// file still arrives (the user-asset budget is ASSET_FETCH_USER_ASSET_ATTEMPTS); before, it was given
// up for the session after ASSET_FETCH_ATTEMPTS rounds, and every wall built from it stayed missing.
static void test_user_asset_outage(void) {
    printf("user asset outage: retried past the content budget...\n");
    char dir[1100], why[1400], model[1200], body[64];
    make_dir(dir, sizeof(dir), "uaoutage");
    snprintf(model, sizeof(model), "%s/user-assets/4411/model.obj", dir);
    snprintf(body, sizeof(body), "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
    routes_reset();
    route(UK "4411/model.obj", 200, body, ASSET_FETCH_ATTEMPTS + 1);
    route(UM "4411/model.obj", 200, body, ASSET_FETCH_ATTEMPTS + 1);
    AssetFetchConfig cfg = config(dir); // no manifest: user assets alone
    cfg.user_assets = 1;
    cfg.user_asset_origins = UK " " UM;
    cfg.workers = 1;
    CHECK(asset_fetch_start(&cfg, why, sizeof(why)), "start (%s)", why);
    CHECK(asset_fetch_request(model, 1) == 0, "queued");
    CHECK(settle(model) == 1, "still arriving after %d failed rounds (requests %d)", ASSET_FETCH_ATTEMPTS + 1, T.requests);
    CHECK(kn_fs_exists(model), "written");
    asset_fetch_stop();
}

// the content-addressed cache (<assets>/.kn-objects): a missing file, or the same bytes under
// another path, comes back from disk with no request; an object whose bytes are wrong is removed
// and the file fetched again
static void test_cache(void) {
    printf("content-addressed cache: restored without a request, damaged objects refetched...\n");
    char dir[1100], why[1400], p0[1200], p1[1200], obj[1400], h[65];
    make_dir(dir, sizeof(dir), "cache");
    const ManifestFile files[] = {
        {"textures/c_0.png", "CACHE one"},
        {"textures/c_1.png", "CACHE one"}, // the same bytes under another path: no route for it below
    };
    write_manifest(dir, files, 2);
    snprintf(p0, sizeof(p0), "%s/textures/c_0.png", dir);
    snprintf(p1, sizeof(p1), "%s/textures/c_1.png", dir);
    sha_hex(files[0].content, h);
    snprintf(obj, sizeof(obj), "%s/.kn-objects/%.2s/%s", dir, h, h);
    routes_reset();
    route(A "textures/c_0.png", 200, files[0].content, 0);
    AssetFetchConfig cfg = config(dir);

    // a session: the path is known to it once fetched, so a missing file is asked for by a new one
    CHECK(asset_fetch_start(&cfg, why, sizeof(why)), "start (%s)", why);
    CHECK(asset_fetch_request(p0, 1) == 0 && settle(p0) == 1, "fetched from the origin");
    CHECK(T.requests == 1 && kn_fs_exists(obj), "one request, and the object is cached (requests %d)", T.requests);
    asset_fetch_stop();

    kn_fs_remove(p0);
    CHECK(asset_fetch_start(&cfg, why, sizeof(why)), "start (%s)", why);
    CHECK(asset_fetch_request(p0, 1) == 0 && settle(p0) == 1, "missing in a new session: restored");
    char *got = read_text(p0);
    CHECK(got && !strcmp(got, files[0].content), "restored content (%s)", got ? got : "(none)");
    free(got);
    CHECK(asset_fetch_request(p1, 1) == 0 && settle(p1) == 1, "the same bytes under another path: from the cache");
    CHECK(T.requests == 1, "no request for either (requests %d)", T.requests);
    AssetFetchStats st;
    asset_fetch_stats(&st);
    CHECK(st.cached == 2 && st.fetched == 2, "stats cached %zu fetched %zu", st.cached, st.fetched);
    asset_fetch_stop();

    write_file(obj, "CACHE onX"); // damaged: same size, other bytes (p0 is the same inode: damaged too)
    kn_fs_remove(p0);
    CHECK(asset_fetch_start(&cfg, why, sizeof(why)), "start (%s)", why);
    CHECK(asset_fetch_request(p0, 1) == 0 && settle(p0) == 1, "a damaged object: the file is fetched again");
    CHECK(T.requests == 2, "one request for it (requests %d)", T.requests);
    got = read_text(obj);
    CHECK(got && !strcmp(got, files[0].content), "the object holds the verified bytes again (%s)", got ? got : "(none)");
    free(got);
    asset_fetch_stop();

    write_file(obj, "CAC"); // truncated
    kn_fs_remove(p0);
    CHECK(asset_fetch_start(&cfg, why, sizeof(why)), "start (%s)", why);
    CHECK(asset_fetch_request(p0, 1) == 0 && settle(p0) == 1, "a truncated object: fetched again");
    CHECK(T.requests == 3, "one request for it (requests %d)", T.requests);
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
    test_user_assets();
    test_user_asset_outage();
    test_cache();
    kn_fs_rmtree(g_root);
    printf("asset_fetch_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
