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
 *   3. user_assets.upstream (default Krunker's host) through a proxy when one is set (curl syntax:
 *      KN_USER_ASSETS_PROXY, which the deploy writes from the krunkbox .env's PROXY on kru, else
 *      user_assets.proxy, else KN_FETCH_PROXY; none = direct; a comma-separated list is tried in
 *      order, the next one when a proxy fails or the host blocks it), written into
 *      the cache and answered. An upstream 404 is remembered for user_assets.fail_cache_s (404),
 *      other failures answer 502 and are remembered for a few seconds.
 * At most UA_FETCHES transfers run at once; requests for a path being fetched wait for that fetch.
 * Responses carry Cache-Control: public (a week): Krunker never changes a published file in place.
 *
 * <id>/video.mp4.knv is not on Krunker's host: it is the asset's video.mp4 (fetched as above) as
 * KrunkNative's frame sidecar (shared/include/knv.h: JPEG frames, user_assets.video_width wide at most,
 * user_assets.video_fps, the first user_assets.video_max_s seconds), made with user_assets.ffmpeg and
 * cached like a fetched file: the clients have no video decoder (client/src/map_video.c plays it on a
 * map object's material, Krunker's VideoTexture). Without ffmpeg it answers 404 and the clients show
 * the asset's texture.png instead.
 */
#include "user_assets.h"

#include "brain.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "knsvc/config.h"
#include "knsvc/http.h"
#include "knsvc/log.h"
#include "knsvc/loop.h"
#include "knv.h"
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
    char proxy[1024]; /* one proxy or a comma-separated list (fetch_upstream tries them in order) */
    char cache_dir[1024];
    char tree_dir[1024];
    int64_t fail_ms, error_ms;
    int timeout_ms;
    char ffmpeg[256];
    int video_width, video_fps, video_quality, video_max_s;
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

#ifdef KN_HAVE_CURL
/* one try of upstream <rel> through `proxy` (NULL / "" = direct): 200, 404 (the host has no such file),
   403 / 5xx as the host answered, or 502 when the transfer failed (the proxy among other things) */
static int fetch_upstream_via(const char *rel, const char *proxy, unsigned char **data, size_t *len) {
    char url[512];
    snprintf(url, sizeof(url), "%s%s", C.upstream, rel);
    CURL *c = curl_easy_init();
    if (!c) return 502;
    Body b = {.max = (size_t) user_asset_max_bytes(rel)};
    curl_easy_setopt(c, CURLOPT_URL, url);
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
    if (proxy && *proxy) curl_easy_setopt(c, CURLOPT_PROXY, proxy);
    long code = 0;
    const CURLcode rc = curl_easy_perform(c);
    if (rc == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(c);
    if (rc == CURLE_OK && code == 200 && !b.too_large && user_asset_content_ok(rel, b.data, b.len)) {
        *data = b.data;
        *len = b.len;
        cache_write(rel, b.data, b.len);
        return 200;
    }
    char why[96];
    if (rc != CURLE_OK && !b.too_large) snprintf(why, sizeof(why), "%s", curl_easy_strerror(rc));
    else if (b.too_large) snprintf(why, sizeof(why), "larger than %zu bytes", b.max);
    else if (code != 200) snprintf(why, sizeof(why), "HTTP %ld", code);
    else snprintf(why, sizeof(why), "not a %s", strrchr(rel, '/') + 1);
    KN_WARN(MOD, "%s: %s%s", url, why, proxy && *proxy ? " (through a proxy)" : "");
    free(b.data);
    if (rc != CURLE_OK || b.too_large) return 502;
    return code == 404 || code == 403 || code >= 500 ? (int) code : 502;
}
#endif

/* upstream <rel> into *data / *len: 200, 404 (the host has no such file) or 502. C.proxy may list
   several proxies ("http://a:1,http://b:2", as the krunkbox .env's PROXY does): each is tried in turn
   until one gets an answer from the host (200 / 404); a proxy that fails or is blocked (403 / 5xx)
   passes the request to the next one. */
static int fetch_upstream(const char *rel, unsigned char **data, size_t *len) {
    *data = NULL;
    *len = 0;
#ifdef KN_HAVE_CURL
    if (!C.proxy[0]) {
        const int r = fetch_upstream_via(rel, NULL, data, len);
        return r == 200 || r == 404 ? r : 502;
    }
    char list[sizeof(C.proxy)];
    snprintf(list, sizeof(list), "%s", C.proxy);
    char *save = NULL;
    for (char *p = strtok_r(list, ",", &save); p; p = strtok_r(NULL, ",", &save)) {
        while (*p == ' ') p++;
        if (!*p) continue;
        const int r = fetch_upstream_via(rel, p, data, len);
        if (r == 200 || r == 404) return r;
    }
    return 502;
#else
    (void) rel;
    return 502;
#endif
}

/* ---- video.mp4.knv: the asset's video as KrunkNative's frame sidecar (knv.h), made with ffmpeg */

typedef struct {
    unsigned char *data;
    size_t len, cap;
} Bytes;

static int bytes_add(Bytes *b, const void *p, const size_t n) {
    if (b->len + n > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 1 << 16;
        while (cap < b->len + n) cap *= 2;
        unsigned char *grown = realloc(b->data, cap);
        if (!grown) return 0;
        b->data = grown;
        b->cap = cap;
    }
    memcpy(b->data + b->len, p, n);
    b->len += n;
    return 1;
}

/* the size of a baseline / progressive JPEG (its SOF marker); 0 when none */
static int jpeg_size(const unsigned char *p, const size_t n, int *w, int *h) {
    for (size_t i = 2; i + 9 < n;) {
        if (p[i] != 0xff) return 0;
        const unsigned m = p[i + 1];
        if (m >= 0xc0 && m <= 0xcf && m != 0xc4 && m != 0xc8 && m != 0xcc) {
            *h = p[i + 5] << 8 | p[i + 6];
            *w = p[i + 7] << 8 | p[i + 8];
            return 1;
        }
        i += 2u + (size_t) (p[i + 2] << 8 | p[i + 3]);
    }
    return 0;
}

/* ffmpeg's MJPEG stream of `mp4` (a file) as a .knv; 200, 404 (no ffmpeg / no video stream) or 502 */
static int make_knv(const char *mp4, const char *rel, unsigned char **out, size_t *out_len) {
    *out = NULL;
    *out_len = 0;
    char vf[160], q[16], t[16], fps[16];
    snprintf(vf, sizeof(vf), "fps=%d,scale='min(%d,iw)':-2", C.video_fps, C.video_width);
    snprintf(q, sizeof(q), "%d", C.video_quality);
    snprintf(t, sizeof(t), "%d", C.video_max_s);
    snprintf(fps, sizeof(fps), "%d", C.video_fps);
    char *const argv[] = {C.ffmpeg, "-nostdin", "-v", "error", "-t", t, "-i", (char *) mp4, "-an", "-vf", vf,
                          "-q:v", q, "-f", "image2pipe", "-vcodec", "mjpeg", "pipe:1", NULL};
    int fd[2];
    if (pipe(fd)) return 502;
    const pid_t pid = fork();
    if (pid < 0) {
        close(fd[0]);
        close(fd[1]);
        return 502;
    }
    if (pid == 0) {
        dup2(fd[1], 1);
        close(fd[0]);
        close(fd[1]);
        const int null = open("/dev/null", O_RDWR);
        if (null >= 0) {
            dup2(null, 0);
            dup2(null, 2);
        }
        execvp(argv[0], argv);
        _exit(127);
    }
    close(fd[1]);
    Bytes stream = {0};
    const size_t max = (size_t) user_asset_max_bytes(rel);
    unsigned char buf[65536];
    int overflow = 0;
    for (;;) {
        const ssize_t n = read(fd[0], buf, sizeof(buf));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        if (stream.len + (size_t) n > max || !bytes_add(&stream, buf, (size_t) n) || atomic_load(&g_cancel)) {
            overflow = 1;
            kill(pid, SIGKILL);
            break;
        }
    }
    close(fd[0]);
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    if (WIFEXITED(st) && WEXITSTATUS(st) == 127) {
        KN_WARN(MOD, "%s: no %s to make video frames (user_assets.ffmpeg)", rel, C.ffmpeg);
        free(stream.data);
        return 404;
    }
    if (overflow || !WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        KN_WARN(MOD, "%s: ffmpeg failed%s", rel, overflow ? " (larger than the cap)" : "");
        free(stream.data);
        return 502;
    }
    /* split the MJPEG stream at each SOI (FFD8FF) that follows an EOI (FFD9) */
    size_t *starts = NULL, nframes = 0;
    for (size_t i = 0; i + 3 <= stream.len; i++) {
        if (stream.data[i] != 0xff || stream.data[i + 1] != 0xd8 || stream.data[i + 2] != 0xff) continue;
        if (i && !(stream.data[i - 2] == 0xff && stream.data[i - 1] == 0xd9)) continue;
        size_t *grown = realloc(starts, (nframes + 1) * sizeof(*starts));
        if (!grown) break;
        starts = grown;
        starts[nframes++] = i;
        if (nframes >= KNV_MAX_FRAMES) break;
    }
    int w = 0, h = 0;
    if (!nframes || !jpeg_size(stream.data, stream.len, &w, &h) || w <= 0 || h <= 0 || w > 65535 || h > 65535) {
        KN_WARN(MOD, "%s: ffmpeg made no frames", rel);
        free(starts);
        free(stream.data);
        return 404;
    }
    const VideoInfo info = {.natural_w = w, .natural_h = h, .frame_w = w, .frame_h = h, .fps_num = (uint32_t) C.video_fps,
                            .fps_den = 1, .frame_count = (uint32_t) nframes,
                            .duration_ms = (uint32_t) (nframes * 1000u / (unsigned) C.video_fps)};
    Bytes knv = {0};
    unsigned char head[KNV_HEADER_SIZE];
    knv_write_header(head, &info);
    int ok = bytes_add(&knv, head, sizeof(head));
    const size_t table = KNV_HEADER_SIZE + 4u * (nframes + 1u);
    for (size_t i = 0; ok && i <= nframes; i++) {
        const size_t o = table + (i < nframes ? starts[i] : stream.len) - starts[0];
        const unsigned char le[4] = {(unsigned char) o, (unsigned char) (o >> 8), (unsigned char) (o >> 16), (unsigned char) (o >> 24)};
        ok = bytes_add(&knv, le, 4);
    }
    ok = ok && bytes_add(&knv, stream.data + starts[0], stream.len - starts[0]);
    free(starts);
    free(stream.data);
    if (!ok || knv.len > max || !video_parse(knv.data, knv.len, NULL, NULL)) {
        free(knv.data);
        return 502;
    }
    KN_INFO(MOD, "%s: %zu frames %dx%d at %d fps (%zu bytes)", rel, nframes, w, h, C.video_fps, knv.len);
    *out = knv.data;
    *out_len = knv.len;
    return 200;
}

/* video.mp4.knv: video.mp4 from the tree / cache / upstream, then ffmpeg */
static int fetch_knv(const char *rel, unsigned char **data, size_t *len) {
    char mp4_rel[UA_REL_MAX], path[1400];
    snprintf(mp4_rel, sizeof(mp4_rel), "%.*s", (int) (strlen(rel) - 4), rel); /* without ".knv" */
    const char *mp4 = NULL;
    snprintf(path, sizeof(path), "%suser-assets/%s", C.tree_dir, mp4_rel);
    if (C.tree_dir[0] && !access(path, R_OK)) mp4 = path;
    if (!mp4) {
        snprintf(path, sizeof(path), "%s%s", C.cache_dir, mp4_rel);
        if (access(path, R_OK)) {
            unsigned char *video = NULL;
            size_t video_len = 0;
            const int st = fetch_upstream(mp4_rel, &video, &video_len); /* cached on success */
            free(video);
            if (st != 200) return st;
        }
        if (!access(path, R_OK)) mp4 = path;
    }
    if (!mp4) return 502; /* no cache dir to transcode from */
    const int st = make_knv(mp4, rel, data, len);
    if (st == 200) cache_write(rel, *data, *len);
    return st;
}

static int ends_with(const char *s, const char *suffix) {
    const size_t a = strlen(s), b = strlen(suffix);
    return a >= b && !strcmp(s + a - b, suffix);
}

static void job_fetch(Job *j) {
    j->status = ends_with(j->rel, ".knv") ? fetch_knv(j->rel, &j->data, &j->len) : fetch_upstream(j->rel, &j->data, &j->len);
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
    /* KN_USER_ASSETS_PROXY (the deploy writes it from the krunkbox .env), user_assets.proxy, else
     * KN_FETCH_PROXY (the tools' convention for local runs, e.g. http://127.0.0.1:8888) */
    const char *proxy = getenv("KN_USER_ASSETS_PROXY");
    if (!proxy || !*proxy) proxy = kn_config_str(b->cfg, "user_assets.proxy", "");
    if (!proxy || !*proxy) proxy = getenv("KN_FETCH_PROXY");
    snprintf(C.proxy, sizeof(C.proxy), "%s", proxy ? proxy : "");
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
    snprintf(C.ffmpeg, sizeof(C.ffmpeg), "%s", kn_config_str(b->cfg, "user_assets.ffmpeg", "ffmpeg"));
    C.video_width = (int) kn_config_int(b->cfg, "user_assets.video_width", 512);
    C.video_fps = (int) kn_config_int(b->cfg, "user_assets.video_fps", 15);
    C.video_quality = (int) kn_config_int(b->cfg, "user_assets.video_quality", 6);
    C.video_max_s = (int) kn_config_int(b->cfg, "user_assets.video_max_s", 120);
    if (C.video_width < 16) C.video_width = 16;
    if (C.video_fps < 1) C.video_fps = 1;
    if (C.video_quality < 2) C.video_quality = 2;
    if (C.video_max_s < 1) C.video_max_s = 1;
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
