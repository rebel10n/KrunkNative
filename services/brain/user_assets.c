/*
 * services/brain/user_assets.c - GET /api/user-assets/<id>/<file> (and /user-assets/<id>/<file>):
 * a mirror of Krunker's user-asset host (https://user-assets.krunker.io/<id>/<file>) for the
 * clients (user_assets.h).
 *
 * Community maps name user assets: textures ("at"), models ("aid": model.obj + model.png, GLTF /
 * GLB), sounds, videos, resource packs (modURL: md<id>/mod.zip). Krunker's own client loads them
 * from user-assets.krunker.io. KrunkNative's native client does too (client/src/net/asset_fetch.c)
 * and comes here when that host refuses it (Cloudflare answers 403 to some networks); the web client
 * always comes here: the host sends no CORS headers, so a page on krunk.io cannot read it.
 *
 * Not an open proxy: only the paths of shared/include/user_asset_path.h (an asset id, md<id> or
 * m<id>, and one of Krunker's file names), capped in size (user_asset_max_bytes), checked
 * (user_asset_content_ok: never an HTML block page), redirects not followed. Answers, in order:
 *   1. the brain's asset tree (accounts.assets_dir)/user-assets/<rel>: the imported v7 assets;
 *   2. the cache, user_assets.cache_dir (default cache/user-assets): what an earlier request fetched;
 *   3. user_assets.upstream (default Krunker's host) through user_assets.proxy (curl syntax, empty =
 *      direct; KN_USER_ASSETS_PROXY overrides it: a network Krunker's CDN accepts), written into
 *      the cache and answered. An upstream 404 is remembered for user_assets.fail_cache_s (404),
 *      other failures answer 502 and are remembered for a few seconds.
 * At most UA_FETCHES transfers run at once; requests for a path being fetched wait for that fetch.
 * Responses carry Cache-Control: public (a week): Krunker never changes a published file in place.
 */
#include "user_assets.h"

#include "brain.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "knsvc/config.h"
#include "knsvc/http.h"
#include "knsvc/log.h"
#include "knsvc/loop.h"
#include "user_asset_path.h"

#ifdef KN_HAVE_CURL
#include <curl/curl.h>
#endif

#define MOD "user-assets"
#define UA_SLOTS 64
#define UA_WAIT 32
#define UA_FETCHES 4
#define UA_REL_MAX 96

static struct {
    int enabled;
    char upstream[256];
    char proxy[256];
    char cache_dir[1024];
    char tree_dir[1024];
    int64_t fail_ms, error_ms;
    int timeout_ms;
} C;

typedef struct {
    char rel[UA_REL_MAX];
    int state;              /* 0 free, 1 fetching, 2 failed (remembered until at + ttl) */
    int status;             /* the failure's answer: 404 or 502 */
    uint64_t at;
    unsigned gen;
    kn_http_req *waiting[UA_WAIT];
    int nwaiting;
} Slot;

typedef struct {
    int slot;
    unsigned gen;
    char rel[UA_REL_MAX];
    char url[512];
    unsigned char *data;
    size_t len;
    int status;             /* 200 ok, 404 missing upstream, 502 other */
} Job;

static Slot S[UA_SLOTS];
static Brain *g_b;
static atomic_int g_live, g_cancel;
static int g_fetching;

static const char *content_type(const char *rel) {
    const char *dot = strrchr(rel, '.');
    if (!dot) return "application/octet-stream";
    if (!strcmp(dot, ".png")) return "image/png";
    if (!strcmp(dot, ".obj")) return "text/plain; charset=utf-8";
    if (!strcmp(dot, ".gltf")) return "model/gltf+json";
    if (!strcmp(dot, ".glb")) return "model/gltf-binary";
    if (!strcmp(dot, ".mp3")) return "audio/mpeg";
    if (!strcmp(dot, ".mp4")) return "video/mp4";
    if (!strcmp(dot, ".json")) return "application/json";
    if (!strcmp(dot, ".zip")) return "application/zip";
    return "application/octet-stream";
}

static void reply_bytes(kn_http_req *req, const char *rel, const unsigned char *data, const size_t len) {
    kn_http_set_header(req, "Cache-Control", "public, max-age=604800");
    kn_http_respond(req, 200, content_type(rel), data, len);
}

static void reply_status(kn_http_req *req, const int status) {
    kn_http_respond_error(req, status, status == 404 ? "not_found" : status == 503 ? "unavailable" : "upstream_failed");
}

/* a whole file (NULL when absent / unreadable / larger than the path may be) */
static unsigned char *read_whole(const char *path, const char *rel, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    unsigned char *data = NULL;
    if (fseek(f, 0, SEEK_END) == 0) {
        const long n = ftell(f);
        if (n >= 0 && (long long) n <= user_asset_max_bytes(rel) && fseek(f, 0, SEEK_SET) == 0) {
            data = malloc((size_t) n + 1);
            if (data && fread(data, 1, (size_t) n, f) != (size_t) n) {
                free(data);
                data = NULL;
            }
            if (data) *len = (size_t) n;
        }
    }
    fclose(f);
    return data;
}

static int serve_local(kn_http_req *req, const char *rel) {
    const char *dirs[2] = {C.tree_dir, C.cache_dir};
    for (int i = 0; i < 2; i++) {
        if (!dirs[i][0]) continue;
        char path[1400];
        snprintf(path, sizeof(path), "%s%s%s", dirs[i], i == 0 ? "user-assets/" : "", rel);
        size_t len = 0;
        unsigned char *data = read_whole(path, rel, &len);
        if (!data) continue;
        reply_bytes(req, rel, data, len);
        free(data);
        return 1;
    }
    return 0;
}

static int mkdirs(const char *dir) {
    char tmp[1400];
    snprintf(tmp, sizeof(tmp), "%s", dir);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        if (mkdir(tmp, 0755) && errno != EEXIST) return 0;
        *p = '/';
    }
    return !mkdir(tmp, 0755) || errno == EEXIST;
}

/* worker thread: the cache file, written atomically */
static void cache_write(const char *rel, const unsigned char *data, const size_t len) {
    if (!C.cache_dir[0]) return;
    char path[1400], dir[1400], tmp[1500];
    snprintf(path, sizeof(path), "%s%s", C.cache_dir, rel);
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = 0;
    if (!mkdirs(dir)) {
        KN_WARN(MOD, "cannot create %s: %s", dir, strerror(errno));
        return;
    }
    static atomic_int serial;
    snprintf(tmp, sizeof(tmp), "%s.%d.part", path, atomic_fetch_add(&serial, 1));
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    const int ok = fwrite(data, 1, len, f) == len;
    if (fclose(f) != 0 || !ok || rename(tmp, path) != 0) {
        remove(tmp);
        KN_WARN(MOD, "cannot write %s", path);
    }
}

/* ------------------------------------------------------------------ fetch (worker thread) */

#ifdef KN_HAVE_CURL
typedef struct {
    unsigned char *data;
    size_t len, max;
    int too_large;
} Body;

static size_t body_write(char *p, size_t size, size_t n, void *ud) {
    Body *b = ud;
    const size_t len = size * n;
    if (b->len + len > b->max) {
        b->too_large = 1;
        return 0;
    }
    unsigned char *grown = realloc(b->data, b->len + len + 1);
    if (!grown) return 0;
    memcpy(grown + b->len, p, len);
    b->data = grown;
    b->len += len;
    return len;
}

static int progress_cb(void *ud, curl_off_t dl, curl_off_t dt, curl_off_t ul, curl_off_t ut) {
    (void) ud; (void) dl; (void) dt; (void) ul; (void) ut;
    return atomic_load(&g_cancel) ? 1 : 0;
}
#endif

static void job_fetch(Job *j) {
    j->status = 502;
#ifdef KN_HAVE_CURL
    CURL *c = curl_easy_init();
    if (!c) return;
    Body b = {.max = (size_t) user_asset_max_bytes(j->rel)};
    curl_easy_setopt(c, CURLOPT_URL, j->url);
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "http,https");
#else
    curl_easy_setopt(c, CURLOPT_PROTOCOLS, (long) (CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(c, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t) b.max);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, body_write);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "KrunkNative kn_brain");
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, (long) C.timeout_ms);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, progress_cb);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    if (C.proxy[0]) curl_easy_setopt(c, CURLOPT_PROXY, C.proxy);
    long code = 0;
    const CURLcode rc = curl_easy_perform(c);
    if (rc == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(c);
    if (rc == CURLE_OK && code == 200 && !b.too_large && user_asset_content_ok(j->rel, b.data, b.len)) {
        j->data = b.data;
        j->len = b.len;
        j->status = 200;
        cache_write(j->rel, j->data, j->len);
        return;
    }
    if (rc == CURLE_OK && code == 404) j->status = 404;
    char why[96];
    if (rc != CURLE_OK && !b.too_large) snprintf(why, sizeof(why), "%s", curl_easy_strerror(rc));
    else if (b.too_large) snprintf(why, sizeof(why), "larger than %zu bytes", b.max);
    else if (code != 200) snprintf(why, sizeof(why), "HTTP %ld", code);
    else snprintf(why, sizeof(why), "not a %s", strrchr(j->rel, '/') + 1);
    KN_WARN(MOD, "%s: %s", j->url, why);
    free(b.data);
#else
    (void) j;
#endif
}

static void job_done(kn_loop *loop, void *ud);

static void *fetch_thread(void *arg) {
    Job *j = arg;
    job_fetch(j);
    kn_loop_post_threadsafe(g_b->loop, job_done, j);
    atomic_fetch_sub(&g_live, 1);
    return NULL;
}

/* ------------------------------------------------------------------ loop thread */

static void job_done(kn_loop *loop, void *ud) {
    (void) loop;
    Job *j = ud;
    g_fetching--;
    Slot *s = &S[j->slot];
    if (s->gen == j->gen && s->state == 1) {
        const int n = s->nwaiting;
        kn_http_req *w[UA_WAIT];
        memcpy(w, s->waiting, sizeof(w[0]) * (size_t) n);
        s->nwaiting = 0;
        if (j->status == 200) {
            s->state = 0; /* the cache answers from now on */
            s->rel[0] = 0;
        } else {
            s->state = 2;
            s->status = j->status;
            s->at = kn_now_ms();
        }
        for (int i = 0; i < n; i++) {
            if (!kn_http_req_alive(w[i])) kn_http_respond_error(w[i], 500, "gone");
            else if (j->status == 200) reply_bytes(w[i], j->rel, j->data, j->len);
            else reply_status(w[i], j->status);
        }
    }
    free(j->data);
    free(j);
}

static Slot *slot_for(const char *rel, const uint64_t now) {
    Slot *old = NULL;
    for (int i = 0; i < UA_SLOTS; i++)
        if (S[i].state && !strcmp(S[i].rel, rel)) return &S[i];
    for (int i = 0; i < UA_SLOTS; i++) {
        if (!S[i].state) return &S[i];
        const int64_t ttl = S[i].status == 404 ? C.fail_ms : C.error_ms;
        if (S[i].state == 2 && (int64_t) (now - S[i].at) >= ttl) return &S[i];
        if (S[i].state == 2 && (!old || S[i].at < old->at)) old = &S[i];
    }
    return old;
}

static void h_asset(kn_http_req *req, void *ud) {
    (void) ud;
    if (!C.enabled) {
        reply_status(req, 503);
        return;
    }
    const char *id = kn_http_param(req, "id"), *file = kn_http_param(req, "file");
    char rel[UA_REL_MAX];
    if (!id || !file || snprintf(rel, sizeof(rel), "%s/%s", id, file) >= (int) sizeof(rel) || !user_asset_rel_ok(rel)) {
        reply_status(req, 404);
        return;
    }
    if (serve_local(req, rel)) return;
    const uint64_t now = kn_now_ms();
    Slot *s = slot_for(rel, now);
    if (!s) {
        reply_status(req, 503);
        return;
    }
    if (s->state == 2 && !strcmp(s->rel, rel)) {
        const int64_t ttl = s->status == 404 ? C.fail_ms : C.error_ms;
        if ((int64_t) (now - s->at) < ttl) {
            reply_status(req, s->status);
            return;
        }
    }
    if (s->state != 1 || strcmp(s->rel, rel) != 0) {
        if (g_fetching >= UA_FETCHES) {
            reply_status(req, 503);
            return;
        }
        Job *j = calloc(1, sizeof(*j));
        if (!j) {
            reply_status(req, 503);
            return;
        }
        s->gen++;
        s->state = 1;
        s->nwaiting = 0;
        snprintf(s->rel, sizeof(s->rel), "%s", rel);
        j->slot = (int) (s - S);
        j->gen = s->gen;
        snprintf(j->rel, sizeof(j->rel), "%s", rel);
        snprintf(j->url, sizeof(j->url), "%s%s", C.upstream, rel);
        pthread_t t;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        atomic_fetch_add(&g_live, 1);
        const int ok = pthread_create(&t, &attr, fetch_thread, j) == 0;
        pthread_attr_destroy(&attr);
        if (!ok) {
            atomic_fetch_sub(&g_live, 1);
            free(j);
            s->state = 0;
            s->rel[0] = 0;
            reply_status(req, 503);
            return;
        }
        g_fetching++;
    }
    if (s->nwaiting >= UA_WAIT) {
        reply_status(req, 503);
        return;
    }
    s->waiting[s->nwaiting++] = req;
}

static void dir_with_slash(char *out, const size_t cap, const char *path) {
    snprintf(out, cap, "%s", path);
    const size_t n = strlen(out);
    if (n && out[n - 1] != '/' && n + 1 < cap) out[n] = '/', out[n + 1] = 0;
}

int user_assets_init(Brain *b) {
    g_b = b;
    C.enabled = kn_config_bool(b->cfg, "user_assets.enabled", 1);
    snprintf(C.upstream, sizeof(C.upstream), "%s", kn_config_str(b->cfg, "user_assets.upstream", USER_ASSET_HOST_URL));
    const size_t ulen = strlen(C.upstream);
    if (ulen && C.upstream[ulen - 1] != '/' && ulen + 1 < sizeof(C.upstream)) C.upstream[ulen] = '/', C.upstream[ulen + 1] = 0;
    const char *proxy = getenv("KN_USER_ASSETS_PROXY");
    snprintf(C.proxy, sizeof(C.proxy), "%s", proxy && *proxy ? proxy : kn_config_str(b->cfg, "user_assets.proxy", ""));
    char path[1024];
    const char *cache = kn_config_str(b->cfg, "user_assets.cache_dir", "cache/user-assets");
    if (!kn_config_resolve_path(b->cfg, cache, path, sizeof(path))) snprintf(path, sizeof(path), "%s", cache);
    dir_with_slash(C.cache_dir, sizeof(C.cache_dir), path);
    const char *tree = kn_config_str(b->cfg, "accounts.assets_dir", "assets");
    if (!kn_config_resolve_path(b->cfg, tree, path, sizeof(path))) snprintf(path, sizeof(path), "%s", tree);
    dir_with_slash(C.tree_dir, sizeof(C.tree_dir), path);
    C.fail_ms = kn_config_int(b->cfg, "user_assets.fail_cache_s", 600) * 1000;
    C.error_ms = kn_config_int(b->cfg, "user_assets.error_cache_s", 10) * 1000;
    C.timeout_ms = (int) kn_config_int(b->cfg, "user_assets.timeout_ms", 20000);
    kn_http_route(b->http, "GET", "/api/user-assets/:id/:file", h_asset, b);
    kn_http_route(b->http, "GET", "/user-assets/:id/:file", h_asset, b);
#ifndef KN_HAVE_CURL
    if (C.enabled) KN_WARN(MOD, "built without libcurl: only the asset tree and the cache answer");
#endif
    if (C.enabled)
        KN_INFO(MOD, "user-asset mirror on: %s%s, cache %s", C.upstream, C.proxy[0] ? " through a proxy" : "", C.cache_dir);
    return 0;
}

void user_assets_shutdown(Brain *b) {
    (void) b;
    atomic_store(&g_cancel, 1);
    for (int i = 0; i < 300 && atomic_load(&g_live) > 0; i++) {
        struct timespec ts = {0, 10 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    for (int i = 0; i < UA_SLOTS; i++) S[i].nwaiting = 0; /* the HTTP server closes their connections */
}
