// Game content sync (launcher/include/kn_assets.h).
#include <kn_assets.h>
#include <kn_fs.h>
#include <kn_http.h>

#include <auth_join.h> // KnSha256

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ASSET_LIST_MAX (64u * 1024u * 1024u)
#define ASSET_RETRIES 3

static void set_err(char *err, const size_t errn, const char *msg, const char *arg) {
    if (err && errn) snprintf(err, errn, "%s%s%s", msg, arg ? ": " : "", arg ? arg : "");
}

static int read_local(const char *assets_dir, KnAssetList *out) {
    char path[4096], *json = NULL;
    size_t len = 0;
    memset(out, 0, sizeof(*out));
    if (!kn_fs_join(path, sizeof(path), assets_dir, KN_ASSETS_STATE) || !kn_fs_read(path, &json, &len, ASSET_LIST_MAX))
        return 0;
    char err[64];
    const int ok = kn_asset_list_parse(json, len, out, err, sizeof(err));
    free(json);
    return ok;
}

int kn_assets_local_version(const char *assets_dir, char *out, const size_t n) {
    // only the "version" field: no need to parse 20k entries for the fast path
    char path[4096], *json = NULL;
    size_t len = 0;
    out[0] = 0;
    if (!kn_fs_join(path, sizeof(path), assets_dir, KN_ASSETS_STATE) || !kn_fs_read(path, &json, &len, ASSET_LIST_MAX))
        return 0;
    const char *v = strstr(json, "\"version\":\"");
    if (v) {
        v += 11;
        const char *end = strchr(v, '"');
        if (end && (size_t) (end - v) < n) snprintf(out, n, "%.*s", (int) (end - v), v);
    }
    free(json);
    return 1;
}

typedef struct {
    const char *dir;
    const KnAssetsSyncOpts *opts;
    size_t total, done;
} CheckCtx;

static int64_t probe_stat(void *user, const char *path) {
    const CheckCtx *c = user;
    char full[4096];
    if (!kn_fs_join(full, sizeof(full), c->dir, path) || kn_fs_is_symlink(full)) return -1;
    return kn_fs_size(full);
}

static int probe_hash(void *user, const char *path, char out_hex[65]) {
    CheckCtx *c = user;
    char full[4096];
    if (!kn_fs_join(full, sizeof(full), c->dir, path)) return 0;
    c->done++;
    if (c->opts && c->opts->progress && (c->done % 64) == 0)
        c->opts->progress(c->opts->user, "check", (int64_t) c->done, (int64_t) c->total, c->done, c->total);
    return kn_fs_sha256(full, out_hex);
}

typedef struct {
    pthread_mutex_t lock;
    const char *dir;
    const char *objects;
    const KnAssetList *want;
    const KnAssetDiff *diff;
    const KnAssetsSyncOpts *opts;
    size_t next, files_done;
    int64_t bytes_done;
    volatile int failed;
    char err[512];
} Pool;

typedef struct {
    FILE *f;
    KnSha256 sha;
    Pool *pool;
    int64_t written;
} FileSink;

static int file_write(void *user, const void *data, const size_t len) {
    FileSink *s = user;
    if (s->pool->failed || fwrite(data, 1, len, s->f) != len) return 0;
    kn_sha256_update(&s->sha, data, len);
    s->written += (int64_t) len;
    pthread_mutex_lock(&s->pool->lock);
    s->pool->bytes_done += (int64_t) len;
    const int64_t done = s->pool->bytes_done;
    const size_t files = s->pool->files_done;
    if (s->pool->opts && s->pool->opts->progress)
        s->pool->opts->progress(s->pool->opts->user, "download", done, s->pool->diff->fetch_bytes, files, s->pool->diff->fetch_count);
    pthread_mutex_unlock(&s->pool->lock);
    return 1;
}

static void fail(Pool *p, const char *msg, const char *arg) {
    pthread_mutex_lock(&p->lock);
    if (!p->failed) set_err(p->err, sizeof(p->err), msg, arg);
    p->failed = 1;
    pthread_mutex_unlock(&p->lock);
}

// One object into place: .kn-sync/<index>.part (per entry: identical files share a sha256 and
// may download at the same time), verified, renamed over <dir>/<path>.
static int fetch_one(Pool *p, KnHttp *http, const KnAssetEntry *e, const size_t index) {
    char url[KN_UPDATE_URL_MAX + 80], part[4096], dest[4096], tmpdir[4096];
    snprintf(url, sizeof(url), "%s%.2s/%s", p->objects, e->sha256, e->sha256);
    kn_fs_join(tmpdir, sizeof(tmpdir), p->dir, ".kn-sync");
    char name[80];
    snprintf(name, sizeof(name), "%zu.part", index);
    kn_fs_join(part, sizeof(part), tmpdir, name);
    kn_fs_join(dest, sizeof(dest), p->dir, e->path);
    char err[256] = "";
    for (int attempt = 0; attempt < ASSET_RETRIES && !p->failed; attempt++) {
        if (p->opts && p->opts->cancel && *p->opts->cancel) {
            fail(p, "cancelled", NULL);
            return 0;
        }
        FileSink s = {.f = kn_fs_fopen(part, "wb"), .pool = p};
        if (!s.f) {
            fail(p, "cannot write", part);
            return 0;
        }
        kn_sha256_init(&s.sha);
        KnHttpSink sink = {.write = file_write, .user = &s, .cancel = p->opts ? p->opts->cancel : NULL};
        const int status = kn_http_get(http, url, &sink, err, sizeof(err));
        const int closed = fclose(s.f) == 0;
        uint8_t digest[32];
        char hex[65];
        kn_sha256_final(&s.sha, digest);
        kn_sha256_hex(digest, hex);
        if (status == 200 && closed && s.written == e->size && !strcmp(hex, e->sha256)) {
            if (!kn_fs_mkdirs_parent(dest) || !kn_fs_rename(part, dest)) {
                kn_fs_remove(part);
                fail(p, "cannot write", dest);
                return 0;
            }
            return 1;
        }
        kn_fs_remove(part);
        pthread_mutex_lock(&p->lock);
        p->bytes_done -= s.written; // counted again by the retry
        pthread_mutex_unlock(&p->lock);
        if (status == 404) {
            fail(p, "missing on the update server", e->path);
            return 0;
        }
        if (status && status != 200) snprintf(err, sizeof(err), "HTTP %d", status);
        else if (status == 200) snprintf(err, sizeof(err), "checksum mismatch");
    }
    if (!p->failed) {
        char what[600];
        snprintf(what, sizeof(what), "%s (%s)", e->path, err[0] ? err : "failed");
        fail(p, "download failed", what);
    }
    return 0;
}

static void *worker(void *arg) {
    Pool *p = arg;
    KnHttp *http = kn_http_new(30000);
    if (!http) {
        fail(p, "out of memory", NULL);
        return NULL;
    }
    for (;;) {
        pthread_mutex_lock(&p->lock);
        const size_t i = p->next < p->diff->fetch_count && !p->failed ? p->next++ : (size_t) -1;
        pthread_mutex_unlock(&p->lock);
        if (i == (size_t) -1) break;
        if (!fetch_one(p, http, &p->want->files[p->diff->fetch[i]], i)) break;
        pthread_mutex_lock(&p->lock);
        p->files_done++;
        pthread_mutex_unlock(&p->lock);
    }
    kn_http_free(http);
    return NULL;
}

int kn_assets_sync(const char *assets_dir, const KnUpdateManifest *m, const KnAssetsSyncOpts *opts,
                   KnAssetsSyncResult *result, char *err, const size_t errn) {
    KnAssetsSyncResult local_result = {0};
    if (!result) result = &local_result;
    memset(result, 0, sizeof(*result));
    if (!m->assets.version[0]) {
        set_err(err, errn, "the update server names no game content", NULL);
        return 0;
    }
    if (!kn_fs_mkdirs(assets_dir)) {
        set_err(err, errn, "cannot create", assets_dir);
        return 0;
    }
    if (opts && opts->progress) opts->progress(opts->user, "list", 0, 0, 0, 0);
    KnHttp *http = kn_http_new(30000);
    char *json = NULL;
    size_t len = 0;
    char herr[256] = "";
    const int status = http ? kn_http_get_mem(http, m->assets.manifest.url, ASSET_LIST_MAX, &json, &len, herr, sizeof(herr)) : 0;
    kn_http_free(http);
    if (status != 200 || !json) {
        char what[800];
        snprintf(what, sizeof(what), "%s (%s)", m->assets.manifest.url, status ? "HTTP error" : herr);
        set_err(err, errn, "cannot download the game content list", what);
        free(json);
        return 0;
    }
    if (m->assets.manifest.sha256[0]) {
        uint8_t digest[32];
        char hex[65];
        kn_sha256_bytes(json, len, digest);
        kn_sha256_hex(digest, hex);
        if (strcmp(hex, m->assets.manifest.sha256) != 0) {
            free(json);
            set_err(err, errn, "the game content list failed its checksum", NULL);
            return 0;
        }
    }
    KnAssetList want, have;
    char perr[128];
    const int parsed = kn_asset_list_parse(json, len, &want, perr, sizeof(perr));
    free(json);
    if (!parsed) {
        set_err(err, errn, "bad game content list", perr);
        return 0;
    }
    if (!want.version[0]) snprintf(want.version, sizeof(want.version), "%s", m->assets.version);
    const int has_local = read_local(assets_dir, &have);

    CheckCtx check = {.dir = assets_dir, .opts = opts, .total = want.count};
    KnAssetDiff diff;
    if (!kn_asset_diff(&want, has_local ? &have : NULL, m->assets.packaged, m->assets.packaged_count, probe_stat,
                       probe_hash, &check, &diff)) {
        kn_asset_list_free(&want);
        kn_asset_list_free(&have);
        set_err(err, errn, "out of memory", NULL);
        return 0;
    }
    result->kept = diff.kept;
    result->adopted = diff.adopted;
    result->hashed = diff.hashed;

    char tmpdir[4096];
    kn_fs_join(tmpdir, sizeof(tmpdir), assets_dir, ".kn-sync");
    int ok = 1;
    if (diff.fetch_count) {
        kn_fs_rmtree(tmpdir);
        if (!kn_fs_mkdirs(tmpdir)) {
            set_err(err, errn, "cannot create", tmpdir);
            ok = 0;
        }
    }
    if (ok && diff.fetch_count) {
        Pool pool = {.dir = assets_dir, .objects = m->assets.objects, .want = &want, .diff = &diff, .opts = opts};
        pthread_mutex_init(&pool.lock, NULL);
        int n = opts && opts->threads > 0 ? opts->threads : 4;
        if (n > 16) n = 16;
        if ((size_t) n > diff.fetch_count) n = (int) diff.fetch_count;
        pthread_t threads[16];
        int started = 0;
        for (int i = 0; i < n; i++)
            if (pthread_create(&threads[i], NULL, worker, &pool) == 0) started++;
            else break;
        if (!started) fail(&pool, "cannot start download threads", NULL);
        for (int i = 0; i < started; i++) pthread_join(threads[i], NULL);
        pthread_mutex_destroy(&pool.lock);
        result->fetched = pool.files_done;
        result->bytes = pool.bytes_done;
        if (pool.failed) {
            set_err(err, errn, pool.err, NULL);
            ok = 0;
        }
    }
    if (ok) {
        for (size_t i = 0; i < diff.remove_count; i++) {
            char full[4096];
            if (!kn_update_path_ok(diff.remove[i]) || !kn_fs_join(full, sizeof(full), assets_dir, diff.remove[i])) continue;
            if (kn_fs_remove(full)) result->removed++;
        }
        char *state = kn_asset_list_json(&want);
        char path[4096];
        kn_fs_join(path, sizeof(path), assets_dir, KN_ASSETS_STATE);
        if (!state || !kn_fs_write_atomic(path, state, strlen(state))) {
            set_err(err, errn, "cannot record the game content state in", assets_dir);
            ok = 0;
        }
        free(state);
    }
    kn_fs_rmtree(tmpdir);
    if (ok && opts && opts->progress)
        opts->progress(opts->user, "done", diff.fetch_bytes, diff.fetch_bytes, diff.fetch_count, diff.fetch_count);
    kn_asset_diff_free(&diff);
    kn_asset_list_free(&want);
    if (has_local) kn_asset_list_free(&have);
    return ok;
}
