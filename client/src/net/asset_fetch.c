// Native on-demand game content fetch (client/include/asset_fetch.h).
#ifndef __EMSCRIPTEN__
#include <asset_fetch.h>

#include <kn_fs.h>
#include <kn_install.h> // kn_install_dir_in_checkout: the launcher's guard
#include <kn_update.h>  // KnAssetList, kn_update_path_ok, KN_ASSETS_STATE

#include <auth_join.h> // KnSha256

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#endif

#define FETCH_LIST_MAX (64u * 1024u * 1024u)

typedef enum {
    ENTRY_QUEUED,  // waiting for a worker
    ENTRY_ACTIVE,  // a worker is downloading it
    ENTRY_WAIT,    // failed a round (transport): queued again at next_ms
    ENTRY_ARRIVED, // written, not polled yet
    ENTRY_DONE,    // polled: the loaders know
    ENTRY_DEAD,    // given up for this session
} EntryState;

typedef struct {
    char *rel;
    uint32_t hash;
    const KnAssetEntry *want; // manifest entry, NULL without one
    EntryState state;
    int attempts;
    int64_t next_ms;
} Entry;

typedef struct {
    char url[KN_UPDATE_URL_MAX];
    int by_path;
    KnPathFilter filter; // the paths it has (one Pages project of the split CDN)
    int fail_streak;   // transport failures in a row
    int64_t down_until;
} Origin;

static struct {
    int running;
    volatile int stop;
    pthread_mutex_t lock;
    pthread_cond_t wake; // workers: work or stop
    pthread_cond_t idle; // asset_fetch_wait_idle
    pthread_t threads[ASSET_FETCH_WORKERS_MAX];
    int thread_count;
    char dir[KN_PATH_MAX];
    size_t dir_len;
    int allow_checkout;
    int backoff_ms, timeout_ms;
    AssetFetchGetFn get;
    void *get_user;
    KnAssetList list;
    int has_list;
    Origin origins[ASSET_FETCH_MAX_ORIGINS];
    int origin_count;
    Entry *entries;
    size_t entry_count, entry_cap;
    char **arrived;
    size_t arrived_count, arrived_cap;
    size_t busy; // queued + active + waiting
    AssetFetchStats stats;
    // directories checked against the git checkout guard: "<0|1><dir>"
    char **dirs_checked;
    size_t dirs_checked_count;
} F;

static int64_t now_ms(void) {
#ifdef _WIN32
    return (int64_t) GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

static uint32_t fnv1a(const char *s) {
    uint32_t h = 2166136261u;
    for (; *s; s++) h = (h ^ (uint8_t) *s) * 16777619u;
    return h;
}

// ------------------------------------------------------------------ configuration

// "[objects:][only=P+P@|skip=P+P@]URL" (kn_origin_spec_parse): a filter = the paths it has.
static void add_origin(const char *spec) {
    if (F.origin_count >= ASSET_FETCH_MAX_ORIGINS || !spec || !*spec) return;
    Origin *o = &F.origins[F.origin_count];
    memset(o, 0, sizeof(*o));
    if (!kn_origin_spec_parse(spec, o->url, sizeof(o->url), &o->by_path, &o->filter)) {
        fprintf(stderr, "[assets] ignoring asset origin %.200s (not [objects:|only=..@|skip=..@]http(s)://...)\n", spec);
        return;
    }
    for (int i = 0; i < F.origin_count; i++)
        if (!strcmp(F.origins[i].url, o->url) && F.origins[i].by_path == o->by_path) return; // listed twice
    F.origin_count++;
}

static void setup_origins(const char *list) {
    char buf[4096];
    snprintf(buf, sizeof(buf), "%s", list && *list ? list : ASSET_FETCH_DEFAULT_ORIGINS);
    for (char *p = buf; *p;) {
        while (*p && strchr(" ,\t\r\n", *p)) p++;
        char *t = p;
        while (*p && !strchr(" ,\t\r\n", *p)) p++;
        if (*p) *p++ = 0;
        if (*t) add_origin(t);
    }
}

static int load_manifest(void) {
    char path[KN_PATH_MAX], *json = NULL;
    size_t len = 0;
    if (!kn_fs_join(path, sizeof(path), F.dir, KN_ASSETS_STATE) || !kn_fs_read(path, &json, &len, FETCH_LIST_MAX)) return 0;
    char err[128] = "";
    const int ok = kn_asset_list_parse(json, len, &F.list, err, sizeof(err));
    free(json);
    if (!ok) fprintf(stderr, "[assets] ignoring %s%s: %s\n", F.dir, KN_ASSETS_STATE, err);
    return ok;
}

static void set_why(char *why, const size_t whyn, const char *fmt, const char *a, const char *b) {
    if (why && whyn) snprintf(why, whyn, fmt, a ? a : "", b ? b : "");
}

static void *worker(void *arg);
static void free_state(void);

int asset_fetch_start(const AssetFetchConfig *cfg, char *why, const size_t whyn) {
    set_why(why, whyn, "%s%s", "", "");
    if (F.running || !cfg || !cfg->assets_dir || !cfg->assets_dir[0]) {
        set_why(why, whyn, "%s%s", F.running ? "already running" : "no assets dir", NULL);
        return F.running;
    }
    memset(&F, 0, sizeof(F));
    const size_t len = strlen(cfg->assets_dir);
    if (len + 2 >= sizeof(F.dir)) {
        set_why(why, whyn, "%s%s", "assets dir path too long", NULL);
        return 0;
    }
    const char last = cfg->assets_dir[len - 1];
    snprintf(F.dir, sizeof(F.dir), "%s%s", cfg->assets_dir, last == '/' || last == '\\' ? "" : "/");
    F.dir_len = strlen(F.dir);
    if (!kn_fs_is_dir(F.dir)) {
        set_why(why, whyn, "%s is not a directory%s", F.dir, NULL);
        return 0;
    }
    F.allow_checkout = cfg->allow_checkout;
    char checkout[KN_PATH_MAX];
    if (!F.allow_checkout && kn_install_dir_in_checkout(F.dir, checkout, sizeof(checkout))) {
        // a dev tree: never write into a repository (the launcher refuses to sync there too)
        set_why(why, whyn, "%s is inside the git checkout %s", F.dir, checkout);
        return 0;
    }
    F.has_list = load_manifest();
    if (cfg->require_manifest && !F.has_list) {
        set_why(why, whyn, "no content manifest %s%s", F.dir, KN_ASSETS_STATE);
        kn_asset_list_free(&F.list);
        return 0;
    }
    setup_origins(cfg->origins);
    if (!F.origin_count) {
        set_why(why, whyn, "%s%s", "no usable asset origin", NULL);
        kn_asset_list_free(&F.list);
        return 0;
    }
    F.backoff_ms = cfg->backoff_ms > 0 ? cfg->backoff_ms : ASSET_FETCH_BACKOFF_MS;
    F.timeout_ms = cfg->timeout_ms > 0 ? cfg->timeout_ms : 20000;
    F.get = cfg->get;
    F.get_user = cfg->get_user;
    F.stats.has_manifest = F.has_list;
    pthread_mutex_init(&F.lock, NULL);
    pthread_cond_init(&F.wake, NULL);
    pthread_cond_init(&F.idle, NULL);
    int n = cfg->workers > 0 ? cfg->workers : ASSET_FETCH_WORKERS_DEFAULT;
    if (n > ASSET_FETCH_WORKERS_MAX) n = ASSET_FETCH_WORKERS_MAX;
    F.running = 1;
    for (int i = 0; i < n; i++)
        if (!pthread_create(&F.threads[F.thread_count], NULL, worker, NULL)) F.thread_count++;
    if (!F.thread_count) {
        F.running = 0;
        set_why(why, whyn, "%s%s", "cannot start a worker thread", NULL);
        free_state();
        return 0;
    }
    return 1;
}

int asset_fetch_running(void) {
    return F.running;
}

static void free_state(void) {
    for (size_t i = 0; i < F.entry_count; i++) free(F.entries[i].rel);
    free(F.entries);
    for (size_t i = 0; i < F.arrived_count; i++) free(F.arrived[i]);
    free(F.arrived);
    for (size_t i = 0; i < F.dirs_checked_count; i++) free(F.dirs_checked[i]);
    free(F.dirs_checked);
    kn_asset_list_free(&F.list);
    pthread_mutex_destroy(&F.lock);
    pthread_cond_destroy(&F.wake);
    pthread_cond_destroy(&F.idle);
    memset(&F, 0, sizeof(F));
}

void asset_fetch_stop(void) {
    if (!F.running) return;
    pthread_mutex_lock(&F.lock);
    __atomic_store_n(&F.stop, 1, __ATOMIC_RELAXED);
    pthread_cond_broadcast(&F.wake);
    pthread_mutex_unlock(&F.lock);
    for (int i = 0; i < F.thread_count; i++) pthread_join(F.threads[i], NULL);
    F.running = 0;
    free_state();
}

// ------------------------------------------------------------------ requests

// under the lock
static Entry *entry_find(const char *rel, const uint32_t hash) {
    for (size_t i = 0; i < F.entry_count; i++)
        if (F.entries[i].hash == hash && !strcmp(F.entries[i].rel, rel)) return &F.entries[i];
    return NULL;
}

// under the lock: a new queued entry (NULL when full / out of memory)
static Entry *entry_add(const char *rel, const uint32_t hash, const KnAssetEntry *want) {
    if (F.entry_count >= ASSET_FETCH_MAX_ENTRIES) return NULL;
    if (F.entry_count == F.entry_cap) {
        const size_t cap = F.entry_cap ? F.entry_cap * 2 : 64;
        Entry *grown = realloc(F.entries, cap * sizeof(*grown));
        if (!grown) return NULL;
        F.entries = grown;
        F.entry_cap = cap;
    }
    char *copy = strdup(rel);
    if (!copy) return NULL;
    Entry *e = &F.entries[F.entry_count++];
    *e = (Entry) {.rel = copy, .hash = hash, .want = want, .state = ENTRY_QUEUED};
    F.busy++;
    F.stats.queued++;
    pthread_cond_signal(&F.wake);
    return e;
}

static int state_answer(const EntryState s) {
    switch (s) {
    case ENTRY_QUEUED:
    case ENTRY_ACTIVE:
    case ENTRY_WAIT: return 0;
    case ENTRY_DEAD: return -1;
    default: return 1;
    }
}

int asset_fetch_request(const char *path, const int enqueue) {
    if (!F.running || !path || strncmp(path, F.dir, F.dir_len) != 0) return 1;
    const char *rel = path + F.dir_len;
    while (rel[0] == '.' && rel[1] == '/') rel += 2;
    // not ours: unsafe paths, the launcher's own files (.kn-assets.json, .kn-sync/, ...)
    if (!kn_update_path_ok(rel) || rel[0] == '.') return 1;
    const KnAssetEntry *want = F.has_list ? kn_asset_list_find(&F.list, rel) : NULL;
    if (F.has_list && !want) return 1; // not part of the content set
    const uint32_t hash = fnv1a(rel);

    pthread_mutex_lock(&F.lock);
    Entry *e = entry_find(rel, hash);
    int answer;
    if (e) {
        answer = state_answer(e->state);
        pthread_mutex_unlock(&F.lock);
        return answer;
    }
    pthread_mutex_unlock(&F.lock);

    // unknown so far: present with the manifest's size = fine (the launcher's sync hashed it)
    const int64_t size = kn_fs_size(path);
    if (size >= 0 && (!want || size == want->size)) return 1;
    if (!enqueue) return size >= 0 ? 1 : -1;
    pthread_mutex_lock(&F.lock);
    e = entry_find(rel, hash); // another thread may have queued it meanwhile
    if (!e) e = entry_add(rel, hash, want);
    answer = e ? state_answer(e->state) : (size >= 0 ? 1 : -1);
    pthread_mutex_unlock(&F.lock);
    return answer;
}

int asset_fetch_poll(void (*fn)(void *user, const char *path), void *user) {
    if (!F.running) return 0;
    pthread_mutex_lock(&F.lock);
    if (!F.arrived_count) {
        pthread_mutex_unlock(&F.lock);
        return 0;
    }
    char **paths = F.arrived;
    const size_t n = F.arrived_count;
    F.arrived = NULL;
    F.arrived_count = F.arrived_cap = 0;
    for (size_t i = 0; i < n; i++) {
        Entry *e = entry_find(paths[i] + F.dir_len, fnv1a(paths[i] + F.dir_len));
        if (e && e->state == ENTRY_ARRIVED) e->state = ENTRY_DONE;
    }
    pthread_mutex_unlock(&F.lock);
    for (size_t i = 0; i < n; i++) {
        if (fn) fn(user, paths[i]);
        free(paths[i]);
    }
    free(paths);
    return (int) n;
}

int asset_fetch_wait_idle(const int timeout_ms) {
    if (!F.running) return 1;
    const int64_t end = now_ms() + timeout_ms;
    pthread_mutex_lock(&F.lock);
    while (F.busy && now_ms() < end) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 20 * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        pthread_cond_timedwait(&F.idle, &F.lock, &ts);
    }
    const int idle = !F.busy;
    pthread_mutex_unlock(&F.lock);
    return idle;
}

void asset_fetch_stats(AssetFetchStats *out) {
    if (!out) return;
    if (!F.running) {
        memset(out, 0, sizeof(*out));
        return;
    }
    pthread_mutex_lock(&F.lock);
    *out = F.stats;
    out->pending = F.busy;
    pthread_mutex_unlock(&F.lock);
}

// ------------------------------------------------------------------ the workers

typedef struct {
    FILE *f;
    KnSha256 sha;
    int64_t bytes, max;
    int overflow, write_error;
} FileSink;

static int file_write(void *user, const void *data, const size_t len) {
    FileSink *s = user;
    if (s->bytes + (int64_t) len > s->max) {
        s->overflow = 1;
        return 0;
    }
    if (fwrite(data, 1, len, s->f) != len) {
        s->write_error = 1;
        return 0;
    }
    kn_sha256_update(&s->sha, data, len);
    s->bytes += (int64_t) len;
    return 1;
}

static int dir_checked(const char *dir, char *why, size_t whyn);

// mkdir -p that follows symlinked directories (kn_fs_mkdirs does not: lstat)
static int make_dirs(const char *path) {
#ifdef _WIN32
    return kn_fs_mkdirs(path);
#else
    struct stat st;
    if (!stat(path, &st)) return S_ISDIR(st.st_mode);
    char parent[KN_PATH_MAX];
    snprintf(parent, sizeof(parent), "%s", path);
    kn_fs_dirname(parent);
    if (parent[0] && strcmp(parent, path) != 0 && !make_dirs(parent)) return 0;
    return !mkdir(path, 0755) || (!stat(path, &st) && S_ISDIR(st.st_mode));
#endif
}

// The directory of `target` (created) may be written: not inside a git checkout (symlinks
// resolved, so a private dir whose subdirs link into a repository's assets/ is caught too).
static int dir_writable(const char *target, char *why, const size_t whyn) {
    char dir[KN_PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", target);
    kn_fs_dirname(dir);
    if (!F.allow_checkout) {
        // the deepest directory that exists decides (symlinks resolved), before anything is created
        char probe[KN_PATH_MAX];
        snprintf(probe, sizeof(probe), "%s", dir);
        while (probe[0] && !kn_fs_exists(probe)) {
            const size_t before = strlen(probe);
            kn_fs_dirname(probe);
            if (strlen(probe) >= before) break;
        }
        if (!dir_checked(probe, why, whyn)) return 0;
    }
    if (!make_dirs(dir)) {
        snprintf(why, whyn, "cannot create %s", dir);
        return 0;
    }
    return 1;
}

// kn_install_dir_in_checkout(dir), answers cached per directory
static int dir_checked(const char *dir, char *why, const size_t whyn) {
    pthread_mutex_lock(&F.lock);
    for (size_t i = 0; i < F.dirs_checked_count; i++) {
        if (!strcmp(F.dirs_checked[i] + 1, dir)) {
            const int ok = F.dirs_checked[i][0] == '1';
            pthread_mutex_unlock(&F.lock);
            if (!ok) snprintf(why, whyn, "%s is inside a git checkout", dir);
            return ok;
        }
    }
    pthread_mutex_unlock(&F.lock);
    char checkout[KN_PATH_MAX];
    const int ok = !kn_install_dir_in_checkout(dir, checkout, sizeof(checkout));
    if (!ok) snprintf(why, whyn, "%s is inside the git checkout %s", dir, checkout);
    pthread_mutex_lock(&F.lock);
    char **grown = realloc(F.dirs_checked, (F.dirs_checked_count + 1) * sizeof(*grown));
    const size_t dlen = strlen(dir);
    char *rec = grown ? malloc(dlen + 2) : NULL;
    if (grown) F.dirs_checked = grown;
    if (rec) {
        rec[0] = ok ? '1' : '0';
        memcpy(rec + 1, dir, dlen + 1);
        F.dirs_checked[F.dirs_checked_count++] = rec;
    }
    pthread_mutex_unlock(&F.lock);
    return ok;
}

typedef enum {
    TRY_OK,       // written
    TRY_MISSING,  // the origin answered, without this file (404, other status)
    TRY_BAD,      // content did not verify
    TRY_TRANSPORT,
    TRY_LOCAL,    // cannot write here: no point asking another origin
} TryResult;

static TryResult try_origin(KnHttp *http, const Origin *o, const Entry *e, const char *target, char *msg, const size_t msgn) {
    char url[KN_UPDATE_URL_MAX + 512];
    if (o->by_path) snprintf(url, sizeof(url), "%s%s", o->url, e->rel);
    else snprintf(url, sizeof(url), "%s%.2s/%s", o->url, e->want->sha256, e->want->sha256);

    char tmp[KN_PATH_MAX + 64];
    static volatile int serial;
    snprintf(tmp, sizeof(tmp), "%s.kn-fetch-%d.part", target, __atomic_add_fetch(&serial, 1, __ATOMIC_RELAXED));
    FileSink fs = {.max = e->want ? e->want->size : ASSET_FETCH_MAX_BYTES};
    fs.f = kn_fs_fopen(tmp, "wb");
    if (!fs.f) {
        snprintf(msg, msgn, "cannot write %s", tmp);
        return TRY_LOCAL;
    }
    kn_sha256_init(&fs.sha);
    KnHttpSink sink = {.write = file_write, .user = &fs, .cancel = &F.stop, .content_length = -1};
    char err[256] = "";
    pthread_mutex_lock(&F.lock);
    F.stats.requests++;
    pthread_mutex_unlock(&F.lock);
    const int status = F.get ? F.get(F.get_user, url, &sink, err, sizeof(err)) : kn_http_get(http, url, &sink, err, sizeof(err));
    const int closed = fclose(fs.f) == 0;
    if (fs.write_error || !closed) {
        kn_fs_remove(tmp);
        snprintf(msg, msgn, "cannot write %s", tmp);
        return TRY_LOCAL;
    }
    if (status < 200 || status >= 300 || fs.overflow) {
        kn_fs_remove(tmp);
        if (fs.overflow) {
            snprintf(msg, msgn, "%s: larger than %lld bytes", o->url, (long long) fs.max);
            return TRY_BAD;
        }
        if (!status) {
            snprintf(msg, msgn, "%s: %s", o->url, err[0] ? err : "transport error");
            return TRY_TRANSPORT;
        }
        snprintf(msg, msgn, "%s: HTTP %d", o->url, status);
        return status >= 500 ? TRY_TRANSPORT : TRY_MISSING;
    }
    uint8_t digest[32];
    char hex[65];
    kn_sha256_final(&fs.sha, digest);
    kn_sha256_hex(digest, hex);
    if (e->want && (fs.bytes != e->want->size || strcmp(hex, e->want->sha256) != 0)) {
        kn_fs_remove(tmp);
        snprintf(msg, msgn, "%s: content does not match the manifest (%lld bytes, sha256 %.12s; want %lld, %.12s)", o->url,
                 (long long) fs.bytes, hex, (long long) e->want->size, e->want->sha256);
        return TRY_BAD;
    }
    if (!kn_fs_rename(tmp, target)) {
        kn_fs_remove(tmp);
        snprintf(msg, msgn, "cannot rename into %s", target);
        return TRY_LOCAL;
    }
    snprintf(msg, msgn, "%s (%lld bytes%s)", o->url, (long long) fs.bytes, e->want ? ", verified" : "");
    return TRY_OK;
}

// under the lock: the next entry to work on, NULL = none; *wait_ms = until the next retry is due
static Entry *pick_locked(int64_t *wait_ms) {
    const int64_t now = now_ms();
    *wait_ms = -1;
    for (size_t i = 0; i < F.entry_count; i++) {
        Entry *e = &F.entries[i];
        if (e->state == ENTRY_QUEUED) return e;
        if (e->state == ENTRY_WAIT) {
            if (e->next_ms <= now) return e;
            const int64_t w = e->next_ms - now;
            if (*wait_ms < 0 || w < *wait_ms) *wait_ms = w;
        }
    }
    return NULL;
}

static void fetch_one(KnHttp *http, size_t index, char *rel) {
    pthread_mutex_lock(&F.lock);
    Entry snapshot = F.entries[index]; // entries may move (realloc): work on a copy
    pthread_mutex_unlock(&F.lock);
    snapshot.rel = rel;

    char target[KN_PATH_MAX];
    char msg[640] = "", why[KN_PATH_MAX + 64] = "", last[640] = "";
    TryResult outcome = TRY_MISSING;
    int transport = 0, tried = 0;
    unsigned tried_mask = 0;
    if (!kn_fs_join(target, sizeof(target), F.dir, rel)) {
        snprintf(last, sizeof(last), "path too long");
        outcome = TRY_LOCAL;
    } else if (!dir_writable(target, why, sizeof(why))) {
        snprintf(last, sizeof(last), "refusing to write: %s", why);
        outcome = TRY_LOCAL;
    } else {
        for (int pass = 0; pass < 2 && outcome != TRY_OK && outcome != TRY_LOCAL; pass++) {
            // pass 0: the origins that are up, in order; pass 1: the ones taken out (last resort)
            for (int g = 0; g < F.origin_count && !__atomic_load_n(&F.stop, __ATOMIC_RELAXED); g++) {
                Origin *o = &F.origins[g];
                if (!o->by_path && !snapshot.want) continue; // content-addressed: needs the sha256
                if (!kn_path_filter_ok(&o->filter, rel)) continue; // not in its part of a split CDN
                pthread_mutex_lock(&F.lock);
                const int down = o->down_until > now_ms();
                pthread_mutex_unlock(&F.lock);
                if ((tried_mask & (1u << g)) || (pass == 0 && down)) continue;
                tried_mask |= 1u << g;
                tried++;
                const TryResult r = try_origin(http, o, &snapshot, target, msg, sizeof(msg));
                pthread_mutex_lock(&F.lock);
                if (r == TRY_TRANSPORT) {
                    o->fail_streak++;
                    int64_t out = (int64_t) F.backoff_ms << (o->fail_streak < 6 ? o->fail_streak - 1 : 5);
                    if (out > ASSET_FETCH_ORIGIN_DOWN_MS) out = ASSET_FETCH_ORIGIN_DOWN_MS;
                    o->down_until = now_ms() + out;
                } else if (r != TRY_LOCAL) {
                    o->fail_streak = 0;
                    o->down_until = 0;
                }
                if (r == TRY_BAD) F.stats.refused++;
                pthread_mutex_unlock(&F.lock);
                snprintf(last, sizeof(last), "%s", msg);
                if (r == TRY_BAD) fprintf(stderr, "[assets] %s: refused %s\n", rel, msg);
                transport |= r == TRY_TRANSPORT;
                outcome = r;
                if (r == TRY_OK || r == TRY_LOCAL) break;
            }
        }
        if (!tried && outcome != TRY_LOCAL) {
            snprintf(last, sizeof(last), "no origin can serve it");
            outcome = TRY_MISSING;
        }
    }
    const int stopping = __atomic_load_n(&F.stop, __ATOMIC_RELAXED);

    pthread_mutex_lock(&F.lock);
    Entry *e = &F.entries[index];
    e->attempts++;
    if (outcome == TRY_OK) {
        e->state = ENTRY_ARRIVED;
        F.stats.fetched++;
        F.busy--;
        char full[KN_PATH_MAX];
        snprintf(full, sizeof(full), "%s%s", F.dir, rel);
        char *copy = strdup(full);
        if (copy && F.arrived_count == F.arrived_cap) {
            const size_t cap = F.arrived_cap ? F.arrived_cap * 2 : 16;
            char **grown = realloc(F.arrived, cap * sizeof(*grown));
            if (grown) {
                F.arrived = grown;
                F.arrived_cap = cap;
            }
        }
        if (copy && F.arrived_count < F.arrived_cap) F.arrived[F.arrived_count++] = copy;
        else free(copy);
        fprintf(stderr, "[assets] fetched %s from %s\n", rel, last);
    } else if (stopping) {
        e->state = ENTRY_DEAD;
        F.busy--;
    } else if (transport && outcome != TRY_LOCAL && e->attempts < ASSET_FETCH_ATTEMPTS) {
        // an origin was unreachable: try again later (quietly; the final outcome is logged)
        e->state = ENTRY_WAIT;
        e->next_ms = now_ms() + ((int64_t) F.backoff_ms << (e->attempts - 1));
    } else {
        e->state = ENTRY_DEAD;
        F.stats.failed++;
        F.busy--;
        fprintf(stderr, "[assets] cannot fetch %s (given up after %d attempt%s): %s\n", rel, e->attempts,
                e->attempts == 1 ? "" : "s", last);
    }
    if (!F.busy) pthread_cond_broadcast(&F.idle);
    pthread_cond_broadcast(&F.wake); // a WAIT entry changes the next wake-up
    pthread_mutex_unlock(&F.lock);
}

static void *worker(void *arg) {
    (void) arg;
    KnHttp *http = F.get ? NULL : kn_http_new(F.timeout_ms);
    pthread_mutex_lock(&F.lock);
    while (!__atomic_load_n(&F.stop, __ATOMIC_RELAXED)) {
        int64_t wait_ms;
        Entry *e = pick_locked(&wait_ms);
        if (!e) {
            if (wait_ms < 0) {
                pthread_cond_wait(&F.wake, &F.lock);
            } else {
                struct timespec ts;
                clock_gettime(CLOCK_REALTIME, &ts);
                ts.tv_sec += (time_t) (wait_ms / 1000);
                ts.tv_nsec += (long) (wait_ms % 1000) * 1000000L + 1000000L;
                while (ts.tv_nsec >= 1000000000L) {
                    ts.tv_sec++;
                    ts.tv_nsec -= 1000000000L;
                }
                pthread_cond_timedwait(&F.wake, &F.lock, &ts);
            }
            continue;
        }
        e->state = ENTRY_ACTIVE;
        const size_t index = (size_t) (e - F.entries);
        char *rel = strdup(e->rel);
        pthread_mutex_unlock(&F.lock);
        if (rel) {
            fetch_one(http, index, rel);
            free(rel);
        }
        pthread_mutex_lock(&F.lock);
        if (!rel && F.entries[index].state == ENTRY_ACTIVE) { // out of memory: give up quietly
            F.entries[index].state = ENTRY_DEAD;
            F.busy--;
        }
    }
    pthread_mutex_unlock(&F.lock);
    kn_http_free(http);
    return NULL;
}
#endif
