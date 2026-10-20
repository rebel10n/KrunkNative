/*
 * services/brain/stream_thumbs.c - GET /streams/thumb?url= (and /api/streams/thumb): the Live
 * Streams cards' Twitch previews for the native client (streams.h).
 *
 * No client asks this route now: a card's <img src> is its thumbnailUrl, as v10 sets it, and the
 * native engine fetches http(s) images itself (client/src/ui/remote_images.c). The route answers
 * {success, data: {url, image: "data:image/jpeg;base64,..."}}, the image as a data: URL.
 *
 * It is not an open proxy: the URL must be https on an allow-listed Twitch CDN host
 * (streams.thumbs.hosts, default static-cdn.jtvnw.net) with a plain image path
 * (streams_thumb_url_ok); redirects are not followed; the body is capped (streams.thumbs.max_bytes)
 * and must be a JPEG or PNG; answers are cached (streams.thumbs.cache_s, failures for
 * streams.thumbs.fail_cache_s) in a small LRU table; at most THUMBS_FETCHES transfers run at once.
 * streams.thumbs.cdn_url (empty in production) sends the allow-listed URL's path to another base
 * URL instead: the tests' stub CDN, or an operator's mirror. Requests for one URL while it is being
 * fetched wait for that fetch.
 */
#include "streams.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "brain.h"
#include "knsvc/config.h"
#include "knsvc/http.h"
#include "knsvc/json.h"
#include "knsvc/log.h"
#include "knsvc/loop.h"

#ifdef KN_HAVE_CURL
#include <curl/curl.h>
#endif

#define MOD "stream-thumbs"
#define THUMBS_SLOTS 32
#define THUMBS_WAIT 16
#define THUMBS_FETCHES 4
#define THUMBS_HOSTS_MAX 8
#define THUMBS_URL_MAX 512

static struct {
    int enabled;
    char hosts[THUMBS_HOSTS_MAX][128];
    const char *host_ptr[THUMBS_HOSTS_MAX];
    size_t nhosts;
    char cdn_url[256];
    size_t max_bytes;
    int64_t cache_ms, fail_ms;
    int timeout_ms;
} C;

typedef struct {
    char url[THUMBS_URL_MAX];
    char *image;            /* the data: URL; NULL = failed */
    const char *error;      /* the failure's code when image is NULL */
    uint64_t at;            /* kn_now_ms() of the answer */
    uint64_t used;          /* LRU */
    unsigned gen;           /* bumped at every reuse: a late fetch result is dropped */
    int state;              /* 0 free, 1 fetching, 2 answered */
    kn_http_req *waiting[THUMBS_WAIT];
    int nwaiting;
} Slot;

static Slot S[THUMBS_SLOTS];
static Brain *g_b;
static atomic_int g_live, g_cancel;
static int g_fetching;      /* loop thread: transfers started and not yet back */

typedef struct {
    int slot;
    unsigned gen;
    char fetch_url[THUMBS_URL_MAX + 256];
    int allow_http;         /* only for streams.thumbs.cdn_url */
    char *image;
    const char *error;
} Job;

static void reply_error(kn_http_req *req, const int status, const char *code, const char *message) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "success", 0);
    cJSON_AddStringToObject(o, "error", code);
    cJSON_AddStringToObject(o, "message", message);
    brain_reply_json(req, status, o);
    cJSON_Delete(o);
}

static void reply_slot(kn_http_req *req, const Slot *s) {
    if (!kn_http_req_alive(req)) {
        kn_http_respond_error(req, 500, "gone");
        return;
    }
    if (!s->image) {
        const int too_large = s->error && !strcmp(s->error, "ThumbnailTooLarge");
        reply_error(req, 502, s->error ? s->error : "ThumbnailUnavailable",
                    too_large ? "the thumbnail is larger than the brain serves" : "the thumbnail could not be fetched");
        return;
    }
    cJSON *o = cJSON_CreateObject(), *d = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "success", 1);
    cJSON_AddStringToObject(d, "url", s->url);
    cJSON_AddStringToObject(d, "image", s->image);
    cJSON_AddItemToObject(o, "data", d);
    brain_reply_json(req, 200, o);
    cJSON_Delete(o);
}

/* ------------------------------------------------------------------ fetch (worker thread) */

#ifdef KN_HAVE_CURL
typedef struct {
    unsigned char *data;
    size_t len;
    int too_large;
} Body;

static size_t body_write(char *p, size_t size, size_t n, void *ud) {
    Body *b = ud;
    const size_t len = size * n;
    if (b->len + len > C.max_bytes) {
        b->too_large = 1;
        return 0; /* aborts the transfer */
    }
    unsigned char *grown = realloc(b->data, b->len + len);
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
    j->error = "ThumbnailUnavailable";
#ifdef KN_HAVE_CURL
    CURL *c = curl_easy_init();
    if (!c) return;
    Body b = {0};
    curl_easy_setopt(c, CURLOPT_URL, j->fetch_url);
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, j->allow_http ? "http,https" : "https");
#else
    curl_easy_setopt(c, CURLOPT_PROTOCOLS, (long) (j->allow_http ? CURLPROTO_HTTP | CURLPROTO_HTTPS : CURLPROTO_HTTPS));
#endif
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(c, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t) C.max_bytes);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, body_write);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "kn_brain");
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, (long) C.timeout_ms);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, progress_cb);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    long code = 0;
    const CURLcode rc = curl_easy_perform(c);
    if (rc == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(c);
    if (b.too_large || rc == CURLE_FILESIZE_EXCEEDED) {
        j->error = "ThumbnailTooLarge";
    } else if (code == 200) {
        const char *mime = streams_image_mime(b.data, b.len);
        if (mime) {
            j->image = streams_data_url(mime, b.data, b.len);
            if (j->image) j->error = NULL;
        } else {
            j->error = "ThumbnailNotImage";
        }
    }
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

static void flush(Slot *s) {
    const int n = s->nwaiting;
    kn_http_req *w[THUMBS_WAIT];
    memcpy(w, s->waiting, sizeof(w[0]) * (size_t) n);
    s->nwaiting = 0;
    for (int i = 0; i < n; i++) reply_slot(w[i], s);
}

static void job_done(kn_loop *loop, void *ud) {
    (void) loop;
    Job *j = ud;
    g_fetching--;
    Slot *s = &S[j->slot];
    if (s->gen == j->gen && s->state == 1) {
        free(s->image);
        s->image = j->image;
        j->image = NULL;
        s->error = j->error;
        s->at = kn_now_ms();
        s->state = 2;
        if (!s->image) KN_WARN(MOD, "thumbnail fetch failed (%s)", s->error ? s->error : "?");
        flush(s);
    }
    free(j->image);
    free(j);
}

static int start_fetch(Slot *s, const char *path) {
    if (g_fetching >= THUMBS_FETCHES) return 0;
    Job *j = calloc(1, sizeof(*j));
    if (!j) return 0;
    j->slot = (int) (s - S);
    j->gen = s->gen;
    if (C.cdn_url[0]) {
        snprintf(j->fetch_url, sizeof(j->fetch_url), "%s%s", C.cdn_url, path);
        j->allow_http = 1;
    } else {
        snprintf(j->fetch_url, sizeof(j->fetch_url), "%s", s->url);
    }
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
        return 0;
    }
    g_fetching++;
    s->state = 1;
    return 1;
}

static int fresh(const Slot *s, const uint64_t now) {
    return s->state == 2 && (int64_t) (now - s->at) < (s->image ? C.cache_ms : C.fail_ms);
}

/* the slot of url: its own, else a free one, else the least recently used one not fetching */
static Slot *slot_for(const char *url) {
    Slot *lru = NULL;
    for (int i = 0; i < THUMBS_SLOTS; i++) {
        if (S[i].state && !strcmp(S[i].url, url)) return &S[i];
    }
    for (int i = 0; i < THUMBS_SLOTS; i++) {
        if (!S[i].state) return &S[i];
        if (S[i].state == 2 && (!lru || S[i].used < lru->used)) lru = &S[i];
    }
    return lru;
}

static void h_thumb(kn_http_req *req, void *ud) {
    (void) ud;
    if (!C.enabled) {
        reply_error(req, 503, "StreamsUnavailable", "stream thumbnails are off on this server");
        return;
    }
    char url[THUMBS_URL_MAX], path[THUMBS_URL_MAX];
    if (kn_http_query_get(req, "url", url, sizeof(url)) <= 0 ||
        !streams_thumb_url_ok(url, C.host_ptr, C.nhosts, path, sizeof(path))) {
        reply_error(req, 400, "InvalidThumbnail", "not a Twitch stream thumbnail");
        return;
    }
    const uint64_t now = kn_now_ms();
    Slot *s = slot_for(url);
    if (!s) {
        reply_error(req, 503, "StreamsUnavailable", "busy, try again");
        return;
    }
    s->used = now;
    if (strcmp(s->url, url) != 0 || !s->state) { /* a new URL in this slot */
        free(s->image);
        s->image = NULL;
        s->error = NULL;
        s->state = 0;
        s->gen++;
        snprintf(s->url, sizeof(s->url), "%s", url);
    }
    if (fresh(s, now)) {
        reply_slot(req, s);
        return;
    }
    if (s->nwaiting >= THUMBS_WAIT) {
        reply_error(req, 503, "StreamsUnavailable", "busy, try again");
        return;
    }
    if (s->state != 1) {
        const int had = s->state == 2;
        if (!start_fetch(s, path)) {
            if (had) {
                reply_slot(req, s); /* the last answer while no transfer can start */
            } else {
                s->state = 0;
                s->url[0] = 0;
                reply_error(req, 503, "StreamsUnavailable", "busy, try again");
            }
            return;
        }
    }
    s->waiting[s->nwaiting++] = req;
}

int stream_thumbs_init(Brain *b) {
    g_b = b;
    C.enabled = kn_config_bool(b->cfg, "streams.thumbs.enabled", 1);
    const char *hosts[THUMBS_HOSTS_MAX];
    size_t n = kn_config_has(b->cfg, "streams.thumbs.hosts") ? kn_config_str_list(b->cfg, "streams.thumbs.hosts", hosts, THUMBS_HOSTS_MAX) : 0;
    if (!kn_config_has(b->cfg, "streams.thumbs.hosts")) {
        hosts[0] = "static-cdn.jtvnw.net"; /* Twitch's preview CDN (Helix thumbnail_url) */
        n = 1;
    }
    C.nhosts = 0;
    for (size_t i = 0; i < n; i++) {
        if (!hosts[i] || !hosts[i][0]) continue;
        snprintf(C.hosts[C.nhosts], sizeof(C.hosts[0]), "%s", hosts[i]);
        C.host_ptr[C.nhosts] = C.hosts[C.nhosts];
        C.nhosts++;
    }
    snprintf(C.cdn_url, sizeof(C.cdn_url), "%s", kn_config_str(b->cfg, "streams.thumbs.cdn_url", ""));
    const size_t len = strlen(C.cdn_url);
    if (len && C.cdn_url[len - 1] == '/') C.cdn_url[len - 1] = 0;
    const int64_t max = kn_config_int(b->cfg, "streams.thumbs.max_bytes", 256 * 1024);
    C.max_bytes = (size_t) (max < 1024 ? 1024 : max > 2 * 1024 * 1024 ? 2 * 1024 * 1024 : max);
    C.cache_ms = kn_config_int(b->cfg, "streams.thumbs.cache_s", 300) * 1000;
    C.fail_ms = kn_config_int(b->cfg, "streams.thumbs.fail_cache_s", 30) * 1000;
    if (C.cache_ms < 0) C.cache_ms = 0;
    if (C.fail_ms < 0) C.fail_ms = 0;
    C.timeout_ms = (int) kn_config_int(b->cfg, "streams.thumbs.timeout_ms", 5000);
    kn_http_route(b->http, "GET", "/streams/thumb", h_thumb, b);
    kn_http_route(b->http, "GET", "/api/streams/thumb", h_thumb, b);
#ifndef KN_HAVE_CURL
    if (C.enabled) KN_WARN(MOD, "built without libcurl: stream thumbnails cannot be fetched");
#endif
    if (C.enabled) KN_INFO(MOD, "stream thumbnails on: %zu allowed host(s), up to %zu bytes, cached %lld s%s", C.nhosts, C.max_bytes,
                           (long long) (C.cache_ms / 1000), C.cdn_url[0] ? ", fetched through streams.thumbs.cdn_url" : "");
    return 0;
}

void stream_thumbs_shutdown(Brain *b) {
    (void) b;
    atomic_store(&g_cancel, 1);
    for (int i = 0; i < 200 && atomic_load(&g_live) > 0; i++) {
        struct timespec ts = {0, 10 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    for (int i = 0; i < THUMBS_SLOTS; i++) {
        free(S[i].image);
        S[i].image = NULL;
        S[i].nwaiting = 0; /* the HTTP server closes their connections */
    }
}
