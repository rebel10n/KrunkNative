// Game content sync (launcher/include/kn_assets.h).
#include <kn_archive.h> // kn_gunzip_mem (the content list)
#include <kn_assets.h>
#include <kn_fs.h>
#include <kn_http.h>

#include <auth_join.h> // KnSha256

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

#define ASSET_LIST_MAX (64u * 1024u * 1024u)
#define ASSET_RETRIES 3              // rounds over every origin before a file fails the sync
#define ASSET_BACKOFF_MS 500         // before retry round r: ASSET_BACKOFF_MS << (r - 1)
#define ASSET_SMALL_MAX (1 << 20)    // files up to this size are fetched in pipelined batches
#define ASSET_BATCH_MAX 16           // files per pipelined batch
#define ASSET_BATCH_BYTES (8 << 20)  // bytes per pipelined batch
#define ASSET_PIPELINE_DEPTH 8       // requests in flight per connection
#define ASSET_THREADS_MAX 32         // connections per origin
#define ASSET_HASH_THREADS 8         // check phase: files hashed in parallel
#define ASSET_ORIGIN_DOWN 8          // consecutive transport failures that take an origin out
#define ASSET_ORIGIN_STALE 32        // consecutive 404 / mismatches that take a mirror out
#define ASSET_HASHES ".kn-hashes"    // hash cache: "<size> <mtime> <sha256> <path>" lines
#define ASSET_SYNC_DIR ".kn-sync"    // partial downloads: <sha256>.part (resumed by the next run)

static void set_err(char *err, const size_t errn, const char *msg, const char *arg) {
    if (err && errn) snprintf(err, errn, "%s%s%s", msg, arg ? ": " : "", arg ? arg : "");
}

static int64_t now_us(void) {
#ifdef _WIN32
    return (int64_t) GetTickCount64() * 1000;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t) ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
#endif
}

static void sleep_ms(const int ms) {
#ifdef _WIN32
    Sleep((DWORD) ms);
#else
    const struct timespec ts = {ms / 1000, (long) (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
#endif
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

// ------------------------------------------------------------------ hash cache
// <assets>/.kn-hashes: the sha256 of every content file the last sync verified, keyed by path +
// size + mtime, so a start without a usable state file (first run over an existing tree, an
// interrupted sync) re-hashes only files that changed since (2 GB cold: ~15 s -> ~0.5 s).

typedef struct {
    char *path;
    int64_t size, mtime;
    char sha256[65];
} HashEntry;

typedef struct {
    HashEntry *e;
    size_t n;
    char *text; // the file: paths point into it
} HashCache;

static int hash_entry_cmp(const void *a, const void *b) {
    return strcmp(((const HashEntry *) a)->path, ((const HashEntry *) b)->path);
}

static void hash_cache_load(const char *dir, HashCache *c) {
    memset(c, 0, sizeof(*c));
    char path[4096];
    size_t len = 0;
    if (!kn_fs_join(path, sizeof(path), dir, ASSET_HASHES) || !kn_fs_read(path, &c->text, &len, ASSET_LIST_MAX)) return;
    size_t lines = 0;
    for (size_t i = 0; i < len; i++) lines += c->text[i] == '\n';
    c->e = lines ? calloc(lines, sizeof(*c->e)) : NULL;
    if (!c->e) return;
    char *p = c->text;
    while (*p && c->n < lines) {
        char *nl = strchr(p, '\n');
        if (!nl) break;
        *nl = 0;
        HashEntry *e = &c->e[c->n];
        long long size, mtime;
        int off = 0;
        if (sscanf(p, "%lld %lld %64s %n", &size, &mtime, e->sha256, &off) == 3 && off > 0 && p[off] &&
            strlen(e->sha256) == 64) {
            e->size = size;
            e->mtime = mtime;
            e->path = p + off;
            c->n++;
        }
        p = nl + 1;
    }
    qsort(c->e, c->n, sizeof(*c->e), hash_entry_cmp);
}

static const HashEntry *hash_cache_find(const HashCache *c, const char *path) {
    if (!c->n) return NULL;
    const HashEntry key = {.path = (char *) path};
    return bsearch(&key, c->e, c->n, sizeof(*c->e), hash_entry_cmp);
}

static void hash_cache_free(HashCache *c) {
    free(c->e);
    free(c->text);
    memset(c, 0, sizeof(*c));
}

// ------------------------------------------------------------------ check phase
// Every wanted path is stat'ed once; the files the diff would hash (right size, not vouched for
// by the last state) are hashed up front, in parallel, unless the hash cache knows them.

typedef struct {
    const char *dir;
    const KnAssetList *want, *have;
    const KnAssetsSyncOpts *opts;
    const HashCache *cache;
    int64_t *size, *mtime; // per want entry; size -1 = missing
    char (*sha)[65];       // per want entry: the content's sha256 when known ("" = unknown)
    size_t *jobs, job_count, job_next, hashed, from_cache;
    pthread_mutex_t lock;
    int64_t last_progress;
} CheckCtx;

static int vouched(const CheckCtx *c, const KnAssetEntry *w) {
    const KnAssetEntry *h = kn_asset_list_find(c->have, w->path);
    return h && !strcmp(h->sha256, w->sha256) && h->size == w->size;
}

static void *hash_worker(void *arg) {
    CheckCtx *c = arg;
    for (;;) {
        pthread_mutex_lock(&c->lock);
        const size_t j = c->job_next < c->job_count && !(c->opts && c->opts->cancel && __atomic_load_n(c->opts->cancel, __ATOMIC_RELAXED)) ? c->job_next++ : (size_t) -1;
        pthread_mutex_unlock(&c->lock);
        if (j == (size_t) -1) break;
        const size_t i = c->jobs[j];
        char full[4096], hex[65];
        if (kn_fs_join(full, sizeof(full), c->dir, c->want->files[i].path) && kn_fs_sha256(full, hex))
            memcpy(c->sha[i], hex, 65);
        pthread_mutex_lock(&c->lock);
        c->hashed++;
        const int64_t t = now_us();
        if (c->opts && c->opts->progress && t - c->last_progress >= 100000) {
            c->last_progress = t;
            c->opts->progress(c->opts->user, "check", (int64_t) c->hashed, (int64_t) c->job_count, c->hashed, c->job_count);
        }
        pthread_mutex_unlock(&c->lock);
    }
    return NULL;
}

static void check_prepare(CheckCtx *c) {
    for (size_t i = 0; i < c->want->count; i++) {
        const KnAssetEntry *w = &c->want->files[i];
        char full[4096];
        c->size[i] = -1;
        c->sha[i][0] = 0;
        if (!kn_fs_join(full, sizeof(full), c->dir, w->path) || !kn_fs_stat(full, &c->size[i], &c->mtime[i])) {
            c->size[i] = -1;
            continue;
        }
        if (c->size[i] != w->size) continue;
        if (vouched(c, w)) {
            memcpy(c->sha[i], w->sha256, 65); // the diff keeps it without hashing
            continue;
        }
        const HashEntry *h = hash_cache_find(c->cache, w->path);
        if (h && h->size == c->size[i] && h->mtime == c->mtime[i]) {
            memcpy(c->sha[i], h->sha256, 65);
            c->from_cache++;
            continue;
        }
        c->jobs[c->job_count++] = i;
    }
    if (!c->job_count) return;
    pthread_mutex_init(&c->lock, NULL);
    pthread_t threads[ASSET_HASH_THREADS];
    int n = 0;
    for (int t = 0; t < ASSET_HASH_THREADS && (size_t) t < c->job_count; t++)
        if (pthread_create(&threads[n], NULL, hash_worker, c) == 0) n++;
    if (!n) hash_worker(c);
    for (int t = 0; t < n; t++) pthread_join(threads[t], NULL);
    pthread_mutex_destroy(&c->lock);
}

static size_t want_index(const CheckCtx *c, const char *path) {
    const KnAssetEntry *e = kn_asset_list_find(c->want, path);
    return e ? (size_t) (e - c->want->files) : (size_t) -1;
}

static int64_t probe_stat(void *user, const char *path) {
    const CheckCtx *c = user;
    const size_t i = want_index(c, path);
    return i == (size_t) -1 ? -1 : c->size[i];
}

static int probe_hash(void *user, const char *path, char out_hex[65]) {
    const CheckCtx *c = user;
    const size_t i = want_index(c, path);
    if (i == (size_t) -1 || !c->sha[i][0]) return 0;
    memcpy(out_hex, c->sha[i], 65);
    return 1;
}

// The hash cache after a completed sync: every wanted file is in place and verified.
static void hash_cache_save(const char *dir, const KnAssetList *want) {
    size_t cap = 1 << 20, len = 0;
    char *buf = malloc(cap);
    if (!buf) return;
    for (size_t i = 0; i < want->count; i++) {
        const KnAssetEntry *w = &want->files[i];
        char full[4096];
        int64_t size, mtime;
        if (!kn_fs_join(full, sizeof(full), dir, w->path) || !kn_fs_stat(full, &size, &mtime) || size != w->size) continue;
        if (len + strlen(w->path) + 128 > cap) {
            char *grown = realloc(buf, cap * 2);
            if (!grown) break;
            buf = grown;
            cap *= 2;
        }
        len += (size_t) snprintf(buf + len, cap - len, "%lld %lld %s %s\n", (long long) size, (long long) mtime, w->sha256, w->path);
    }
    char path[4096];
    if (kn_fs_join(path, sizeof(path), dir, ASSET_HASHES)) kn_fs_write_atomic(path, buf, len);
    free(buf);
}

// ------------------------------------------------------------------ download pool

typedef struct {
    char url[KN_UPDATE_URL_MAX];
    int by_path;                  // <url><path> (a copy of the tree) or <url><aa>/<sha256>
    KnPathFilter filter;          // by path: the part of the tree it has (one Pages project of the split)
    int disabled;
    int transport_streak, stale_streak;
    KnAssetsOriginStat stat;
} Origin;

typedef struct {
    const char *sha256;
    int64_t size;
    size_t *idx, nidx;            // want entries with this content (fetch order: idx[0] is downloaded)
    unsigned serve;               // origins that have it (bit per origin; their path filters)
    unsigned tried;               // origins tried this round (bit per origin)
    int not_found;                // every answer this round was a 404
    int round;
    int64_t not_before_us;
    int last_status;
    char last_err[160];
} Obj;

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    const char *dir;
    char tmpdir[4096];
    const KnAssetList *want;
    const KnAssetsSyncOpts *opts;
    Origin origins[KN_ASSETS_MAX_ORIGINS];
    int origin_count;
    Obj *objs;
    size_t obj_count;
    // objects grouped by the origins that serve them (lanes), each lane largest first; a lane's
    // open part is [head, tail) of queue. One lane unless mirrors have path filters.
    size_t *queue;
    struct {
        unsigned serve;
        size_t head, tail;
    } lanes[1u << KN_ASSETS_MAX_ORIGINS];
    int lane_count;
    size_t in_flight;             // objects a worker is fetching
    size_t *idx_storage;          // the objects' idx arrays
    size_t *retry, retry_count;   // objects waiting for another origin / the next round
    size_t completed, files_done, files_total, retries;
    int64_t bytes_done, bytes_total, bytes_net, resumed, last_progress_us;
    int failed;
    char err[512];
} Pool;

static int cancelled(const Pool *p) {
    return p->opts && p->opts->cancel && __atomic_load_n(p->opts->cancel, __ATOMIC_RELAXED);
}

// under the lock
static void fail_locked(Pool *p, const char *msg, const char *arg) {
    if (!p->failed) set_err(p->err, sizeof(p->err), msg, arg);
    p->failed = 1;
    pthread_cond_broadcast(&p->cond);
}

static void fail(Pool *p, const char *msg, const char *arg) {
    pthread_mutex_lock(&p->lock);
    fail_locked(p, msg, arg);
    pthread_mutex_unlock(&p->lock);
}

// under the lock: at most 10 progress calls a second (force: always)
static void progress_locked(Pool *p, const int force) {
    if (!p->opts || !p->opts->progress) return;
    const int64_t t = now_us();
    if (!force && t - p->last_progress_us < 100000) return;
    p->last_progress_us = t;
    p->opts->progress(p->opts->user, "download", p->bytes_done, p->bytes_total, p->files_done, p->files_total);
}

static void add_bytes(Pool *p, const int64_t n, const int net) {
    pthread_mutex_lock(&p->lock);
    p->bytes_done += n;
    if (net) p->bytes_net += n;
    progress_locked(p, 0);
    pthread_mutex_unlock(&p->lock);
}

static void obj_url(const Pool *p, const int origin, const Obj *o, char *out, const size_t n) {
    const Origin *g = &p->origins[origin];
    if (!g->by_path) {
        snprintf(out, n, "%s%.2s/%s", g->url, o->sha256, o->sha256);
        return;
    }
    size_t k = (size_t) snprintf(out, n, "%s", g->url);
    static const char HEX[] = "0123456789ABCDEF";
    const char *path = p->want->files[o->idx[0]].path; // a path with this content the origin has
    for (size_t i = 0; i < o->nidx; i++)
        if (kn_path_filter_ok(&g->filter, p->want->files[o->idx[i]].path)) {
            path = p->want->files[o->idx[i]].path;
            break;
        }
    for (const unsigned char *c = (const unsigned char *) path; *c && k + 4 < n; c++) {
        if ((*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || strchr("-._~/", *c)) {
            out[k++] = (char) *c;
        } else {
            out[k++] = '%';
            out[k++] = HEX[*c >> 4];
            out[k++] = HEX[*c & 15];
        }
    }
    out[k < n ? k : n - 1] = 0;
}

static void part_path(const Pool *p, const Obj *o, const char *suffix, char *out, const size_t n) {
    char name[96];
    snprintf(name, sizeof(name), "%s%s", o->sha256, suffix);
    kn_fs_join(out, n, p->tmpdir, name);
}

// The verified part file into every path of the object: renamed over the first, copied (via a
// temporary file + rename) to the others. 1 ok.
static int install_obj(Pool *p, const Obj *o, const char *part) {
    char dest[4096];
    kn_fs_join(dest, sizeof(dest), p->dir, p->want->files[o->idx[0]].path);
    if (!kn_fs_mkdirs_parent(dest) || !kn_fs_rename(part, dest)) {
        kn_fs_remove(part);
        fail(p, "cannot write", dest);
        return 0;
    }
    for (size_t k = 1; k < o->nidx; k++) {
        char other[4096], tmp[4096];
        kn_fs_join(other, sizeof(other), p->dir, p->want->files[o->idx[k]].path);
        part_path(p, o, ".copy", tmp, sizeof(tmp));
        if (!kn_fs_mkdirs_parent(other) || !kn_fs_copy(dest, tmp) || !kn_fs_rename(tmp, other)) {
            kn_fs_remove(tmp);
            fail(p, "cannot write", other);
            return 0;
        }
    }
    return 1;
}

typedef struct {
    Pool *pool;
    const Obj *obj;
    KnHttpSink sink;
    char part[4096];
    FILE *f;
    KnSha256 sha;
    int64_t offset;  // resumed bytes already in the part (and in sha)
    int64_t written; // bytes of this answer
    int started, bad;
} FileSink;

static int file_write(void *user, const void *data, const size_t len) {
    FileSink *s = user;
    if (s->pool->failed || s->bad) return 0;
    if (!s->started) {
        s->started = 1;
        if (s->offset > 0 && s->sink.status != 206) { // the whole body after all: start over
            if (s->f) fclose(s->f);
            s->f = NULL;
            add_bytes(s->pool, -s->offset, 0);
            pthread_mutex_lock(&s->pool->lock);
            s->pool->resumed -= s->offset;
            pthread_mutex_unlock(&s->pool->lock);
            s->offset = 0;
            kn_sha256_init(&s->sha);
        }
        if (!s->f) s->f = kn_fs_fopen(s->part, s->offset > 0 ? "ab" : "wb");
        if (!s->f) {
            s->bad = 1;
            return 0;
        }
    }
    if (s->offset + s->written + (int64_t) len > s->obj->size || fwrite(data, 1, len, s->f) != len) {
        s->bad = 1; // longer than the list says, or the disk is full
        return 0;
    }
    kn_sha256_update(&s->sha, data, len);
    s->written += (int64_t) len;
    add_bytes(s->pool, (int64_t) len, 1);
    return 1;
}

// After the answer: 1 = verified and installed, 0 = not usable (the part is kept when it is a
// clean prefix, for a resume), -1 = the sync failed (cannot write).
static int file_finish(FileSink *s, const int status) {
    Pool *p = s->pool;
    if (!s->started && !(status >= 200 && status < 300)) { // no body: a resumed part stays as it is
        add_bytes(p, -s->offset, 0);
        pthread_mutex_lock(&p->lock);
        p->resumed -= s->offset;
        pthread_mutex_unlock(&p->lock);
        return 0;
    }
    if (!s->f && status >= 200 && status < 300 && !s->bad) s->f = kn_fs_fopen(s->part, s->offset > 0 ? "ab" : "wb");
    const int closed = s->f ? fclose(s->f) == 0 : 0;
    s->f = NULL;
    const int64_t total = s->offset + s->written;
    if ((status == 200 || status == 206) && closed && !s->bad && total == s->obj->size) {
        uint8_t digest[32];
        char hex[65];
        kn_sha256_final(&s->sha, digest);
        kn_sha256_hex(digest, hex);
        if (!strcmp(hex, s->obj->sha256)) return install_obj(p, s->obj, s->part) ? 1 : -1;
        kn_fs_remove(s->part); // wrong content: never resume from it
        add_bytes(p, -total, 0);
        pthread_mutex_lock(&p->lock);
        p->resumed -= s->offset;
        pthread_mutex_unlock(&p->lock);
        return 0;
    }
    // A transfer cut short keeps its part (a resume continues there); anything else starts over.
    // Either way none of it counts as done: a resume counts the kept prefix again.
    const int keep = !s->bad && closed && total > 0 && total < s->obj->size && (status == 200 || status == 206 || status == 0);
    if (!keep) kn_fs_remove(s->part);
    add_bytes(p, -total, 0);
    pthread_mutex_lock(&p->lock);
    p->resumed -= s->offset;
    pthread_mutex_unlock(&p->lock);
    return 0;
}

// Prepares a sink for o; resume: continue an existing part file. 0 = the part was already
// complete and is installed (*installed set), or the sync failed.
static int file_begin(Pool *p, const Obj *o, FileSink *s, const int resume, int *installed) {
    memset(s, 0, sizeof(*s));
    s->pool = p;
    s->obj = o;
    s->sink.write = file_write;
    s->sink.user = s;
    s->sink.cancel = p->opts ? p->opts->cancel : NULL;
    *installed = 0;
    part_path(p, o, ".part", s->part, sizeof(s->part));
    kn_sha256_init(&s->sha);
    int64_t have = 0, mtime;
    if (!resume || !kn_fs_stat(s->part, &have, &mtime) || have <= 0) return 1;
    if (have > o->size) {
        kn_fs_remove(s->part);
        return 1;
    }
    FILE *f = kn_fs_fopen(s->part, "rb"); // the prefix into the hash
    if (!f) return 1;
    char buf[65536];
    size_t r;
    int64_t got = 0;
    while ((r = fread(buf, 1, sizeof(buf), f)) > 0) {
        kn_sha256_update(&s->sha, buf, r);
        got += (int64_t) r;
    }
    fclose(f);
    if (got != have) {
        kn_fs_remove(s->part);
        kn_sha256_init(&s->sha);
        return 1;
    }
    s->offset = have;
    pthread_mutex_lock(&p->lock);
    p->bytes_done += have;
    p->resumed += have;
    pthread_mutex_unlock(&p->lock);
    if (have == o->size) { // complete from an earlier run: only the verification was missing
        s->sink.status = 200;
        s->started = 1;
        const int r2 = file_finish(s, 200);
        if (r2 == 1) *installed = 1;
        return r2 >= 0;
    }
    s->sink.range_from = have;
    return 1;
}

// under the lock: how many origins can still serve
static int origins_up(const Pool *p) {
    int n = 0;
    for (int g = 0; g < p->origin_count; g++) n += !p->origins[g].disabled;
    return n;
}

// under the lock: the outcome of one object on origin g (1 ok, 0 failed with status / err)
static void obj_done_locked(Pool *p, const int g, Obj *o, const int ok, const int status, const char *err) {
    Origin *og = &p->origins[g];
    if (ok) {
        og->transport_streak = og->stale_streak = 0;
        og->stat.files++;
        og->stat.bytes += o->size;
        p->completed++;
        p->files_done += o->nidx;
        progress_locked(p, 0);
        return;
    }
    og->stat.failures++;
    if (status == 0) og->transport_streak++;
    else og->transport_streak = 0;
    // a mirror serving other content (checksum mismatches, error statuses) is stale; a 404 is
    // not counted (the Pages CDN may leave prefixes out on purpose), nor a redirect a pipelined
    // batch does not follow (assets.krunk.io 301s the other Pages project's directories; the
    // retry is a plain GET, which follows it)
    if (status != 0 && status != 404 && !(status >= 300 && status < 400)) og->stale_streak++;
    // An origin that keeps failing is taken out, never the last one (its errors end the sync).
    if (origins_up(p) > 1 && (og->transport_streak >= ASSET_ORIGIN_DOWN || (g > 0 && og->stale_streak >= ASSET_ORIGIN_STALE))) {
        og->disabled = 1;
        og->stat.disabled = 1;
    }
    o->tried |= 1u << g;
    if (status != 404) o->not_found = 0;
    o->last_status = status;
    if (status && !(status >= 200 && status < 300)) snprintf(o->last_err, sizeof(o->last_err), "HTTP %d", status);
    else snprintf(o->last_err, sizeof(o->last_err), "%s", err && err[0] ? err : "failed");
    unsigned up = 0;
    for (int k = 0; k < p->origin_count; k++)
        if (!p->origins[k].disabled) up |= 1u << k;
    if (o->serve & up) up &= o->serve; // the origins that have it (filters), unless all are gone
    if ((o->tried & up) == up) { // every origin had it this round
        // 404 from all of them (o->not_found survives only 404s): no retry will find it
        if (o->not_found || ++o->round >= ASSET_RETRIES) {
            char what[600];
            snprintf(what, sizeof(what), "%s (%s)", p->want->files[o->idx[0]].path, o->last_err);
            fail_locked(p, o->not_found ? "missing on the update server" : "download failed", what);
            return;
        }
        o->tried = 0;
        o->not_found = 1;
        o->not_before_us = now_us() + (int64_t) (ASSET_BACKOFF_MS << (o->round - 1)) * 1000;
    }
    p->retries++;
    p->retry[p->retry_count++] = (size_t) (o - p->objs);
}

typedef struct {
    Pool *pool;
    int origin;
    int small_first; // half the workers start from the small end of the queue (latency-bound
                     // pipelined batches), the others from the large end (bandwidth-bound)
} Worker;

// under the lock: up to max objects for a worker of origin g into out. 0 = nothing right now
// (*wait set when there may be work later: retries in backoff or in flight elsewhere).
static size_t pick_locked(Pool *p, const Worker *w, size_t *out, const size_t max, int *wait) {
    *wait = 0;
    const unsigned bit = 1u << w->origin;
    unsigned up = 0;
    for (int k = 0; k < p->origin_count; k++)
        if (!p->origins[k].disabled) up |= 1u << k;
    const int64_t t = now_us();
    for (size_t r = 0; r < p->retry_count; r++) { // retries first: one at a time
        Obj *o = &p->objs[p->retry[r]];
        const unsigned has = o->serve & up ? o->serve & up : up; // up and have it (path filters)
        if ((o->tried & has) == has) o->tried = 0; // the origins that had it are gone: a fresh round
        if (!(has & bit)) continue; // not on this origin (a part of a split CDN)
        if ((o->tried & bit) || o->not_before_us > t) {
            *wait = 1;
            continue;
        }
        out[0] = p->retry[r];
        p->retry[r] = p->retry[--p->retry_count];
        return 1;
    }
    // The lane (objects with the same serving origins) with the most open objects this origin
    // has (a lane none of whose origins is up any more: anyone's). Its open part, largest first,
    // is [head, tail): large-end workers take one object from the head, small-end workers a
    // pipelined batch from the tail.
    int lane = -1;
    size_t most = 0;
    for (int l = 0; l < p->lane_count; l++) {
        const unsigned has = p->lanes[l].serve & up;
        const size_t open = p->lanes[l].tail - p->lanes[l].head;
        if ((!has || (has & bit)) && open > most) {
            lane = l;
            most = open;
        }
    }
    size_t n = 0;
    int64_t bytes = 0;
    if (lane < 0) {
        // nothing open for this origin
    } else if (!w->small_first) {
        out[n++] = p->queue[p->lanes[lane].head++];
    } else {
        while (n < max && p->lanes[lane].head < p->lanes[lane].tail) {
            const size_t last = p->queue[p->lanes[lane].tail - 1];
            const int64_t size = p->objs[last].size;
            if (n > 0 && (size > ASSET_SMALL_MAX || bytes + size > ASSET_BATCH_BYTES)) break;
            out[n++] = last;
            p->lanes[lane].tail--;
            bytes += size;
            if (size > ASSET_SMALL_MAX) break;
        }
    }
    if (n) return n;
    if (p->in_flight) *wait = 1; // a failure in flight may come back as a retry
    return 0;
}

typedef struct {
    Pool *pool;
    int origin;
    FileSink *sink;
    size_t obj;
} BatchItem;

// a pipelined answer: finish its file right away (the next answer is not read yet)
static void batch_done(void *user, KnHttpReq *r) {
    BatchItem *b = user;
    Pool *p = b->pool;
    const int st = file_finish(b->sink, r->status);
    pthread_mutex_lock(&p->lock);
    p->in_flight--;
    // st 0 with a 2xx: the body was wrong (size or sha256); a status 0 is a transport failure
    const int ok_status = r->status >= 200 && r->status < 300;
    if (st >= 0)
        obj_done_locked(p, b->origin, &p->objs[b->obj], st == 1, r->status,
                        st == 1 ? NULL : ok_status ? "checksum mismatch" : r->status ? "HTTP error" : r->err);
    pthread_mutex_unlock(&p->lock);
    b->sink = NULL;
}

static void *worker(void *arg) {
    Worker *w = arg;
    Pool *p = w->pool;
    const Origin *g = &p->origins[w->origin];
    KnHttp *http = kn_http_new(30000);
    FileSink *sinks = calloc(ASSET_BATCH_MAX, sizeof(*sinks));
    KnHttpReq *reqs = calloc(ASSET_BATCH_MAX, sizeof(*reqs));
    BatchItem *items = calloc(ASSET_BATCH_MAX, sizeof(*items));
    char (*urls)[KN_UPDATE_URL_MAX + 512] = calloc(ASSET_BATCH_MAX, sizeof(*urls));
    if (!http || !sinks || !reqs || !items || !urls) {
        fail(p, "out of memory", NULL);
        goto out;
    }
    for (;;) {
        size_t pick[ASSET_BATCH_MAX];
        int wait = 0;
        pthread_mutex_lock(&p->lock);
        size_t n = 0;
        if (!p->failed && !cancelled(p) && !g->disabled) n = pick_locked(p, w, pick, ASSET_BATCH_MAX, &wait);
        p->in_flight += n;
        pthread_mutex_unlock(&p->lock);
        if (!n) {
            if (!wait) break;
            sleep_ms(50);
            continue;
        }
        // prepare the sinks (a part complete from an earlier run is installed here)
        size_t m = 0;
        for (size_t k = 0; k < n; k++) {
            int installed = 0;
            if (!file_begin(p, &p->objs[pick[k]], &sinks[m], 1, &installed)) { // cannot write
                pthread_mutex_lock(&p->lock);
                p->in_flight -= n - k;
                pthread_mutex_unlock(&p->lock);
                goto out;
            }
            if (installed) {
                pthread_mutex_lock(&p->lock);
                p->in_flight--;
                obj_done_locked(p, w->origin, &p->objs[pick[k]], 1, 200, NULL);
                pthread_mutex_unlock(&p->lock);
                continue;
            }
            obj_url(p, w->origin, &p->objs[pick[k]], urls[m], sizeof(urls[m]));
            items[m] = (BatchItem) {.pool = p, .origin = w->origin, .sink = &sinks[m], .obj = pick[k]};
            reqs[m] = (KnHttpReq) {.url = urls[m], .sink = &sinks[m].sink, .done = batch_done, .user = &items[m]};
            m++;
        }
        if (!m) continue;
        const int served = m == 1 ? -1 : kn_http_get_pipelined(http, reqs, m, ASSET_PIPELINE_DEPTH, p->opts ? p->opts->cancel : NULL);
        if (served < 0) { // one file: a plain GET (follows redirects, retries a stale connection)
            reqs[0].status = kn_http_get(http, urls[0], &sinks[0].sink, reqs[0].err, sizeof(reqs[0].err));
            batch_done(&items[0], &reqs[0]);
            continue;
        }
        // not served (the connection closed, a failure, cancel): back to the queue as retries;
        // only the first counts as a failure of this origin, so a dead connection cannot loop
        for (size_t k = (size_t) served; k < m; k++) {
            FileSink *s = items[k].sink;
            if (!s) continue;
            const int st = file_finish(s, 0);
            pthread_mutex_lock(&p->lock);
            p->in_flight--;
            Obj *o = &p->objs[items[k].obj];
            if (st >= 0) {
                if (k == (size_t) served) obj_done_locked(p, w->origin, o, 0, 0, reqs[k].err[0] ? reqs[k].err : "connection closed");
                else p->retry[p->retry_count++] = items[k].obj;
            }
            pthread_mutex_unlock(&p->lock);
        }
    }
out:
    kn_http_free(http);
    free(sinks);
    free(reqs);
    free(items);
    free(urls);
    return NULL;
}

static const Obj *g_sort_objs; // qsort has no context argument in C11
static int queue_cmp(const void *a, const void *b) {
    const Obj *x = &g_sort_objs[*(const size_t *) a], *y = &g_sort_objs[*(const size_t *) b];
    if (x->serve != y->serve) return x->serve < y->serve ? -1 : 1; // by lane, then largest first
    if (x->size != y->size) return x->size > y->size ? -1 : 1;
    return strcmp(x->sha256, y->sha256);
}
static pthread_mutex_t g_sort_lock = PTHREAD_MUTEX_INITIALIZER;
static const KnAssetList *g_sort_want;
static int by_sha_cmp(const void *a, const void *b) {
    const KnAssetEntry *x = &g_sort_want->files[*(const size_t *) a], *y = &g_sort_want->files[*(const size_t *) b];
    const int c = strcmp(x->sha256, y->sha256);
    return c ? c : (*(const size_t *) a < *(const size_t *) b ? -1 : 1);
}

// The objects to download: the diff's fetch list grouped by sha256 (identical content once).
static int build_objects(Pool *p, const KnAssetDiff *diff) {
    size_t *order = malloc((diff->fetch_count + 1) * sizeof(*order));
    p->objs = calloc(diff->fetch_count + 1, sizeof(*p->objs));
    p->queue = malloc((diff->fetch_count + 1) * sizeof(*p->queue));
    p->retry = malloc((diff->fetch_count + 1) * sizeof(*p->retry));
    size_t *idx = malloc((diff->fetch_count + 1) * sizeof(*idx));
    if (!order || !p->objs || !p->queue || !p->retry || !idx) {
        free(order);
        free(idx);
        return 0;
    }
    memcpy(order, diff->fetch, diff->fetch_count * sizeof(*order));
    pthread_mutex_lock(&g_sort_lock);
    g_sort_want = p->want;
    qsort(order, diff->fetch_count, sizeof(*order), by_sha_cmp);
    pthread_mutex_unlock(&g_sort_lock);
    memcpy(idx, order, diff->fetch_count * sizeof(*idx)); // the objects' idx point into this
    free(order);
    for (size_t i = 0; i < diff->fetch_count;) {
        const KnAssetEntry *e = &p->want->files[idx[i]];
        size_t j = i + 1;
        while (j < diff->fetch_count && !strcmp(p->want->files[idx[j]].sha256, e->sha256)) j++;
        Obj *o = &p->objs[p->obj_count];
        o->sha256 = e->sha256;
        o->size = e->size;
        o->idx = &idx[i];
        o->nidx = j - i;
        o->not_found = 1;
        // the origins that have it: no filter, or one of its paths passes the origin's
        for (int g = 0; g < p->origin_count; g++)
            for (size_t k = 0; k < o->nidx; k++)
                if (!p->origins[g].by_path || kn_path_filter_ok(&p->origins[g].filter, p->want->files[o->idx[k]].path)) {
                    o->serve |= 1u << g;
                    break;
                }
        p->queue[p->obj_count] = p->obj_count;
        p->obj_count++;
        i = j;
    }
    pthread_mutex_lock(&g_sort_lock);
    g_sort_objs = p->objs;
    qsort(p->queue, p->obj_count, sizeof(*p->queue), queue_cmp);
    pthread_mutex_unlock(&g_sort_lock);
    p->lane_count = 0;
    for (size_t i = 0; i < p->obj_count; i++) {
        const unsigned serve = p->objs[p->queue[i]].serve;
        if (!p->lane_count || p->lanes[p->lane_count - 1].serve != serve) {
            p->lanes[p->lane_count].serve = serve;
            p->lanes[p->lane_count].head = i;
            p->lane_count++;
        }
        p->lanes[p->lane_count - 1].tail = i + 1;
    }
    p->idx_storage = idx;
    return 1;
}

static void add_origin(Pool *p, const char *url, const int by_path, const KnPathFilter *filter) {
    if (p->origin_count >= KN_ASSETS_MAX_ORIGINS || !url[0] || strlen(url) + 2 >= KN_UPDATE_URL_MAX) return;
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) return;
    Origin *g = &p->origins[p->origin_count];
    memset(g, 0, sizeof(*g));
    snprintf(g->url, sizeof(g->url), "%s%s", url, url[strlen(url) - 1] == '/' ? "" : "/");
    for (int k = 0; k < p->origin_count; k++)
        if (!strcmp(p->origins[k].url, g->url)) return; // listed twice
    g->by_path = by_path;
    if (by_path && filter) g->filter = *filter;
    snprintf(g->stat.url, sizeof(g->stat.url), "%s", g->url);
    p->origin_count++;
}

// Origin 0 is the manifest's objects; then opts->mirrors (origin specs, kn_origin_spec_parse:
// "[only=P+P@|skip=P+P@]URL"), else the manifest's assets.mirrors (with their only/skip path
// filters: the Pages projects of deploy/asset-shards.json), else (a manifest from the default
// origin that has no "mirrors" key) the built-in CDN.
static void setup_origins(Pool *p, const KnUpdateManifest *m, const KnAssetsSyncOpts *opts) {
    add_origin(p, m->assets.objects, 0, NULL);
    const char *env = opts ? opts->mirrors : NULL;
    if (env && !strcmp(env, "off")) return;
    if (env && *env) {
        char buf[4096];
        snprintf(buf, sizeof(buf), "%s", env);
        for (char *t = strtok(buf, " ,"); t; t = strtok(NULL, " ,")) {
            char url[KN_UPDATE_URL_MAX];
            int by_path;
            KnPathFilter f;
            if (kn_origin_spec_parse(t, url, sizeof(url), &by_path, &f)) add_origin(p, url, by_path, &f);
        }
        return;
    }
    if (m->assets.mirrors_set) {
        for (int i = 0; i < m->assets.mirror_count; i++)
            add_origin(p, m->assets.mirrors[i].url, m->assets.mirrors[i].by_path, &m->assets.mirrors[i].filter);
        return;
    }
    if (!strncmp(m->assets.objects, KN_ASSETS_DEFAULT_ORIGIN, strlen(KN_ASSETS_DEFAULT_ORIGIN)))
        add_origin(p, KN_ASSETS_DEFAULT_MIRROR, 1, NULL);
}

// Downloads every object of the diff. 1 ok.
static int download(Pool *p, const KnAssetsSyncOpts *opts, KnAssetsSyncResult *result) {
    int per = opts && opts->threads > 0 ? opts->threads : KN_ASSETS_THREADS_DEFAULT;
    if (per > ASSET_THREADS_MAX) per = ASSET_THREADS_MAX;
    // no more connections than objects (a small update), at least 2 per origin for both ends
    if ((size_t) per * (size_t) p->origin_count > p->obj_count)
        per = (int) ((p->obj_count + (size_t) p->origin_count - 1) / (size_t) p->origin_count);
    if (per < 1) per = 1;
    const int n = per * p->origin_count;
    pthread_t *threads = calloc((size_t) n, sizeof(*threads));
    Worker *workers = calloc((size_t) n, sizeof(*workers));
    if (!threads || !workers) {
        free(threads);
        free(workers);
        fail(p, "out of memory", NULL);
        return 0;
    }
    int started = 0;
    for (int k = 0; k < per; k++)
        for (int g = 0; g < p->origin_count; g++) {
            Worker *w = &workers[started];
            w->pool = p;
            w->origin = g;
            w->small_first = k % 2 == 1 || per == 1;
            if (pthread_create(&threads[started], NULL, worker, w) == 0) started++;
        }
    if (!started) fail(p, "cannot start download threads", NULL);
    for (int i = 0; i < started; i++) pthread_join(threads[i], NULL);
    free(threads);
    free(workers);
    if (!p->failed && p->completed < p->obj_count) // every worker gave up (e.g. its origin disabled)
        fail(p, cancelled(p) ? "cancelled" : "download failed", p->retry_count ? p->objs[p->retry[0]].last_err : NULL);
    if (cancelled(p) && !p->err[0]) set_err(p->err, sizeof(p->err), "cancelled", NULL);
    result->unique = p->completed;
    result->fetched = p->files_done;
    result->bytes = p->bytes_net;
    result->resumed = p->resumed;
    result->retries = p->retries;
    result->origin_count = p->origin_count;
    for (int g = 0; g < p->origin_count; g++) result->origins[g] = p->origins[g].stat;
    for (size_t i = 0; i < p->obj_count; i++) result->copied += p->objs[i].nidx - 1;
    return !p->failed;
}

// The content list (gzip on the wire when the origin compresses it: 2.2 MB -> 0.9 MB), verified.
static char *fetch_list(const KnUpdateManifest *m, size_t *len, char *err, const size_t errn) {
    KnHttp *http = kn_http_new(30000);
    char *body = NULL, herr[256] = "";
    int gz = 0;
    const int status = http ? kn_http_get_mem_gz(http, m->assets.manifest.url, ASSET_LIST_MAX, &body, len, &gz, herr, sizeof(herr)) : 0;
    kn_http_free(http);
    if (status == 200 && body && gz) {
        size_t out_len = 0;
        char zerr[64];
        unsigned char *plain = kn_gunzip_mem((const unsigned char *) body, *len, ASSET_LIST_MAX, &out_len, zerr, sizeof(zerr));
        free(body);
        body = NULL;
        if (plain) {
            body = realloc(plain, out_len + 1);
            if (!body) free(plain);
            else body[out_len] = 0;
            *len = out_len;
        } else {
            snprintf(herr, sizeof(herr), "%s", zerr);
        }
    }
    if (status != 200 || !body) {
        char what[800];
        snprintf(what, sizeof(what), "%s (%s)", m->assets.manifest.url, status && status != 200 ? "HTTP error" : herr);
        set_err(err, errn, "cannot download the game content list", what);
        free(body);
        return NULL;
    }
    if (m->assets.manifest.sha256[0]) {
        uint8_t digest[32];
        char hex[65];
        kn_sha256_bytes(body, *len, digest);
        kn_sha256_hex(digest, hex);
        if (strcmp(hex, m->assets.manifest.sha256) != 0) {
            free(body);
            set_err(err, errn, "the game content list failed its checksum", NULL);
            return NULL;
        }
    }
    return body;
}

int kn_assets_sync(const char *assets_dir, const KnUpdateManifest *m, const KnAssetsSyncOpts *opts,
                   KnAssetsSyncResult *result, char *err, const size_t errn) {
    KnAssetsSyncResult local_result;
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
    size_t len = 0;
    char *json = fetch_list(m, &len, err, errn);
    if (!json) return 0;
    KnAssetList want, have;
    char perr[128];
    const int parsed = kn_asset_list_parse(json, len, &want, perr, sizeof(perr));
    free(json);
    if (!parsed) {
        set_err(err, errn, "bad game content list", perr);
        return 0;
    }
    if (opts && opts->select) { // drop the unselected entries (the list stays sorted)
        size_t n = 0;
        for (size_t i = 0; i < want.count; i++) {
            if (opts->select(opts->user, &want.files[i])) want.files[n++] = want.files[i];
            else free(want.files[i].path);
        }
        want.count = n;
    }
    if (!want.version[0]) snprintf(want.version, sizeof(want.version), "%s", m->assets.version);
    const int has_local = read_local(assets_dir, &have);
    if (opts && opts->progress) opts->progress(opts->user, "check", 0, 0, 0, 0);

    // check: stat everything, hash what neither the state nor the hash cache vouches for
    HashCache cache;
    hash_cache_load(assets_dir, &cache);
    const size_t wc = want.count ? want.count : 1;
    CheckCtx check = {.dir = assets_dir, .want = &want, .have = has_local ? &have : NULL, .opts = opts, .cache = &cache,
                      .size = malloc(wc * sizeof(int64_t)), .mtime = malloc(wc * sizeof(int64_t)),
                      .sha = malloc(wc * sizeof(*check.sha)), .jobs = malloc(wc * sizeof(size_t))};
    KnAssetDiff diff;
    memset(&diff, 0, sizeof(diff));
    int ok = check.size && check.mtime && check.sha && check.jobs;
    if (ok) {
        check_prepare(&check);
        ok = kn_asset_diff(&want, has_local ? &have : NULL, m->assets.packaged, m->assets.packaged_count, probe_stat,
                           probe_hash, &check, &diff);
    }
    hash_cache_free(&cache);
    const size_t hashed = check.hashed;
    free(check.size);
    free(check.mtime);
    free(check.sha);
    free(check.jobs);
    if (!ok) {
        kn_asset_list_free(&want);
        if (has_local) kn_asset_list_free(&have);
        set_err(err, errn, "out of memory", NULL);
        return 0;
    }
    result->kept = diff.kept;
    result->adopted = diff.adopted;
    result->hashed = hashed; // the diff counts cache hits as hashed too: report the real work
    if (opts && opts->cancel && __atomic_load_n(opts->cancel, __ATOMIC_RELAXED)) {
        set_err(err, errn, "cancelled", NULL);
        ok = 0;
    }

    char tmpdir[4096];
    kn_fs_join(tmpdir, sizeof(tmpdir), assets_dir, ASSET_SYNC_DIR);
    if (ok && diff.fetch_count) {
        // the parts of an interrupted earlier sync stay: they are resumed (or replaced)
        if (!kn_fs_mkdirs(tmpdir)) {
            set_err(err, errn, "cannot create", tmpdir);
            ok = 0;
        }
    }
    if (ok && diff.fetch_count) {
        Pool pool;
        memset(&pool, 0, sizeof(pool));
        pool.dir = assets_dir;
        snprintf(pool.tmpdir, sizeof(pool.tmpdir), "%s", tmpdir);
        pool.want = &want;
        pool.opts = opts;
        pool.files_total = diff.fetch_count;
        pool.bytes_total = diff.fetch_bytes;
        pthread_mutex_init(&pool.lock, NULL);
        pthread_cond_init(&pool.cond, NULL);
        setup_origins(&pool, m, opts);
        if (!build_objects(&pool, &diff)) {
            set_err(err, errn, "out of memory", NULL);
            ok = 0;
        } else {
            // bytes_total counts every path; duplicates are copied, so count them as done once
            // their object is: the bar moves by object bytes times the paths that share it
            int64_t unique_bytes = 0;
            for (size_t i = 0; i < pool.obj_count; i++) unique_bytes += pool.objs[i].size;
            pool.bytes_total = unique_bytes;
            if (!download(&pool, opts, result)) {
                set_err(err, errn, pool.err, NULL);
                ok = 0;
            }
        }
        free(pool.objs);
        free(pool.queue);
        free(pool.retry);
        free(pool.idx_storage);
        pthread_cond_destroy(&pool.cond);
        pthread_mutex_destroy(&pool.lock);
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
        if (ok) hash_cache_save(assets_dir, &want);
        kn_fs_rmtree(tmpdir);
    }
    if (ok && opts && opts->progress)
        opts->progress(opts->user, "done", diff.fetch_bytes, diff.fetch_bytes, diff.fetch_count, diff.fetch_count);
    kn_asset_diff_free(&diff);
    kn_asset_list_free(&want);
    if (has_local) kn_asset_list_free(&have);
    return ok;
}
