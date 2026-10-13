// assets_bench: the launcher's game content sync, headless, into a directory, with timings
// (launcher/src/assets_sync.c). Not a ctest: it talks to the real asset origins.
//
//   assets_bench <dir> [--manifest URL] [--threads N] [--mirror URL|off] [--limit N] [--seed N]
//                [--cancel-after BYTES]
//
// --threads: connections per origin; --mirror: "off" (only the manifest's objects) or by-path
// mirror URLs (KnAssetsSyncOpts.mirrors).
// --limit N syncs a random sample of N files of the content list (same --seed = same sample), so
// runs against the live origins stay short. Prints one line per phase and a JSON summary.
#include <kn_assets.h>
#include <kn_fs.h>
#include <kn_http.h>
#include <kn_update.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec + (double) ts.tv_nsec / 1e9;
}

typedef struct {
    char phase[16];
    double t0, t_phase[8];
    int64_t bytes;
    size_t files;
    int64_t cancel_after; // --cancel-after: set the cancel flag once this many bytes arrived
    volatile int cancel;
} Bench;

static const char *const PHASES[] = {"start", "list", "check", "download", "done"};

static int phase_index(const char *p) {
    for (int i = 0; i < 5; i++)
        if (!strcmp(PHASES[i], p)) return i;
    return 0;
}

static void progress(void *user, const char *phase, const int64_t done, const int64_t total, const size_t files_done,
                     const size_t files_total) {
    Bench *b = user;
    (void) total;
    (void) files_total;
    if (strcmp(b->phase, phase) != 0) {
        const int i = phase_index(phase);
        b->t_phase[i] = now_s() - b->t0;
        fprintf(stderr, "[bench] %-8s at %7.2f s\n", phase, b->t_phase[i]);
        snprintf(b->phase, sizeof(b->phase), "%s", phase);
    }
    if (!strcmp(phase, "download")) {
        b->bytes = done;
        b->files = files_done;
        if (b->cancel_after > 0 && done >= b->cancel_after) __atomic_store_n(&b->cancel, 1, __ATOMIC_RELAXED);
    }
}

// a deterministic sample: FNV-1a of seed + path, kept with probability permille / 1e6
typedef struct {
    unsigned seed, permille;
} Sample;
static Sample g_sample;

static int select_sample(void *user, const KnAssetEntry *e) {
    (void) user;
    uint32_t h = 2166136261u ^ g_sample.seed;
    for (const char *p = e->path; *p; p++) h = (h ^ (unsigned char) *p) * 16777619u;
    return h % 1000000u < g_sample.permille;
}

int main(int argc, char **argv) {
    const char *dir = NULL, *manifest_url = KN_UPDATE_DEFAULT_URL, *mirror = NULL;
    int threads = 0;
    long limit = 0;
    unsigned seed = 1;
    long long cancel_after = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--manifest") && i + 1 < argc) manifest_url = argv[++i];
        else if (!strcmp(argv[i], "--threads") && i + 1 < argc) threads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--mirror") && i + 1 < argc) mirror = argv[++i];
        else if (!strcmp(argv[i], "--limit") && i + 1 < argc) limit = atol(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = (unsigned) atol(argv[++i]);
        else if (!strcmp(argv[i], "--cancel-after") && i + 1 < argc) cancel_after = atoll(argv[++i]);
        else if (!dir && argv[i][0] != '-') dir = argv[i];
        else {
            fprintf(stderr, "usage: assets_bench <dir> [--manifest URL] [--threads N] [--mirror URL|off] [--limit N] [--seed N]\n");
            return 2;
        }
    }
    if (!dir) return 2;
    kn_http_set_user_agent("KrunkNative-assets-bench/1");
    KnHttp *http = kn_http_new(15000);
    char *json = NULL, err[512] = "";
    size_t len = 0;
    if (kn_http_get_mem(http, manifest_url, 1 << 22, &json, &len, err, sizeof(err)) != 200) {
        fprintf(stderr, "manifest: %s\n", err);
        return 1;
    }
    kn_http_free(http);
    KnUpdateManifest m;
    if (!kn_update_manifest_parse(json, len, manifest_url, &m, err, sizeof(err))) {
        fprintf(stderr, "manifest: %s\n", err);
        return 1;
    }
    free(json);
    Bench b = {.t0 = now_s(), .cancel_after = cancel_after};
    Sample sample = {.seed = seed, .permille = limit > 0 && m.assets.files > 0 ? (unsigned) (1000000.0 * (double) limit / (double) m.assets.files) : 1000000};
    KnAssetsSyncOpts opts = {.progress = progress, .user = &b, .threads = threads, .mirrors = mirror, .cancel = &b.cancel};
    if (limit > 0) {
        opts.select = select_sample;
        g_sample = sample;
    }
    KnAssetsSyncResult r;
    const int ok = kn_assets_sync(dir, &m, &opts, &r, err, sizeof(err));
    const double t = now_s() - b.t0;
    KnHttpStats st;
    kn_http_stats(&st);
    for (char *c = err; *c; c++) // JSON-safe
        if (*c == '"' || *c == '\\' || (unsigned char) *c < 32) *c = '\'';
    const double dl = b.t_phase[3] > 0 ? t - b.t_phase[3] : 0;
    printf("{\"ok\":%d,\"err\":\"%s\",\"total_s\":%.2f,\"list_s\":%.2f,\"check_s\":%.2f,\"download_s\":%.2f,"
           "\"fetched\":%zu,\"bytes\":%lld,\"kept\":%zu,\"adopted\":%zu,\"hashed\":%zu,\"files_per_s\":%.1f,\"mb_per_s\":%.2f,\"requests\":%lld,\"connects\":%lld,\"unique\":%zu,\"copied\":%zu,\"resumed\":%lld,\"retries\":%zu,\"removed\":%zu,\"origins\":[",
           ok, ok ? "" : err, t, b.t_phase[2] > 0 ? b.t_phase[2] - b.t_phase[1] : 0,
           b.t_phase[3] > 0 ? b.t_phase[3] - b.t_phase[2] : (b.t_phase[4] > 0 ? b.t_phase[4] - b.t_phase[2] : 0), dl,
           r.fetched, (long long) r.bytes, r.kept, r.adopted, r.hashed, dl > 0 ? (double) r.fetched / dl : 0,
           dl > 0 ? (double) r.bytes / dl / 1048576.0 : 0, (long long) st.requests, (long long) st.connects, r.unique, r.copied, (long long) r.resumed, r.retries, r.removed);
    for (int g = 0; g < r.origin_count; g++)
        printf("%s{\"url\":\"%s\",\"files\":%zu,\"bytes\":%lld,\"failures\":%zu,\"disabled\":%d}", g ? "," : "",
               r.origins[g].url, r.origins[g].files, (long long) r.origins[g].bytes, r.origins[g].failures, r.origins[g].disabled);
    printf("]}\n");
    for (int g = 0; g < r.origin_count; g++)
        fprintf(stderr, "[bench] origin %s: %zu files, %.1f MB, %zu failures%s\n", r.origins[g].url, r.origins[g].files,
                (double) r.origins[g].bytes / 1048576.0, r.origins[g].failures, r.origins[g].disabled ? " (disabled)" : "");
    fprintf(stderr, "[bench] unique %zu, copied %zu, resumed %.1f MB, retries %zu; connect %.2f s total (%.0f ms avg), ttfb %.0f ms avg\n",
            r.unique, r.copied, (double) r.resumed / 1048576.0, r.retries, st.connect_s,
            st.connects ? st.connect_s * 1000.0 / (double) st.connects : 0, st.requests ? st.ttfb_s * 1000.0 / (double) st.requests : 0);
    kn_update_manifest_free(&m);
    return ok ? 0 : 1;
}
