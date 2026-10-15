/*
 * services/brain/streams.c - GET /streams/top (and /api/streams/top): the Live Streams list of
 * the v10 menu (streams.h). The answer is {success, data: {streams, promo, dropCampaigns}}, the
 * shape v10's getTopStreams() unwraps (v10:25992).
 *
 * Upstream: Twitch Helix. An app access token (client credentials) is fetched once and kept
 * until it nears its expiry; the Krunker category's id comes from /games?name=; /streams lists
 * its live streams. The whole chain runs on one detached worker thread at a time (blocking
 * libcurl, like ai.c); its result comes back to the loop thread through kn_loop_post_threadsafe.
 * The answer is cached for streams.cache_s; while the upstream is down the last good list is
 * served. The client secret is sent only in the token request's body and is never logged.
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

#define MOD "streams"
#define STREAMS_MAX_WAITING 64
#define STREAMS_MAX_BODY (1u << 20)
#define STREAMS_TOKEN_MARGIN_S 60
#define STREAMS_MAX_PER_PAGE 100

typedef struct {
    int enabled;
    char client_id[128];
    char client_secret[256];
    char game[64];
    char auth_url[256];
    char api_url[256];
    int64_t cache_ms;
    int timeout_ms;
} Cfg;

static Cfg C;
static Brain *g_b;

/* the answer state: touched by the loop thread only (the worker gets copies in a Job) */
static struct {
    char token[512];
    uint64_t token_until;   /* kn_now_ms() */
    char game_id[32];
    cJSON *data;            /* streams_normalise()'s object of the last good answer */
    uint64_t fetched;       /* kn_now_ms() of data */
    int fetching;
    kn_http_req *waiting[STREAMS_MAX_WAITING];
    int nwaiting;
} S;

static atomic_int g_live;   /* worker threads running */
static atomic_int g_cancel; /* shutdown: stop the transfers */

/* one worker's copy of the state it reads and the result it hands back */
typedef struct {
    char token[512];
    uint64_t token_until;
    char game_id[32];
    cJSON *data;
    int ok;
} Job;

static void copy_str(char *dst, const size_t cap, const char *src) { snprintf(dst, cap, "%s", src ? src : ""); }

static void config_load(const kn_config *cfg) {
    C.enabled = kn_config_bool(cfg, "streams.enabled", 1);
    copy_str(C.client_id, sizeof(C.client_id), kn_config_str(cfg, "streams.twitch.client_id", ""));
    copy_str(C.client_secret, sizeof(C.client_secret), kn_config_str(cfg, "streams.twitch.client_secret", ""));
    copy_str(C.game, sizeof(C.game), kn_config_str(cfg, "streams.twitch.game", "Krunker"));
    copy_str(C.auth_url, sizeof(C.auth_url), kn_config_str(cfg, "streams.twitch.auth_url", "https://id.twitch.tv/oauth2/token"));
    copy_str(C.api_url, sizeof(C.api_url), kn_config_str(cfg, "streams.twitch.api_url", "https://api.twitch.tv/helix"));
    C.cache_ms = kn_config_int(cfg, "streams.cache_s", 30) * 1000;
    if (C.cache_ms < 0) C.cache_ms = 0;
    C.timeout_ms = (int) kn_config_int(cfg, "streams.timeout_ms", 8000);
}

static int configured(void) { return C.enabled && C.client_id[0] && C.client_secret[0]; }

static void reply_error(kn_http_req *req, const int status, const char *code, const char *message) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "success", 0);
    cJSON_AddStringToObject(o, "error", code);
    cJSON_AddStringToObject(o, "message", message);
    brain_reply_json(req, status, o);
    cJSON_Delete(o);
}

static void reply_list(kn_http_req *req) {
    if (!S.data) {
        reply_error(req, 503, "StreamsUnavailable", "the live streams are not available right now");
        return;
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "success", 1);
    cJSON_AddItemToObject(o, "data", cJSON_Duplicate(S.data, 1));
    brain_reply_json(req, 200, o);
    cJSON_Delete(o);
}

/* ------------------------------------------------------------------ upstream (worker thread) */

typedef struct {
    char *data;
    size_t len;
} Buf;

#ifdef KN_HAVE_CURL
static size_t buf_write(char *p, size_t size, size_t n, void *ud) {
    Buf *b = ud;
    const size_t len = size * n;
    if (b->len + len > STREAMS_MAX_BODY) return 0;
    char *grown = realloc(b->data, b->len + len + 1);
    if (!grown) return 0;
    memcpy(grown + b->len, p, len);
    b->len += len;
    grown[b->len] = 0;
    b->data = grown;
    return len;
}

static int progress_cb(void *ud, curl_off_t dl, curl_off_t dt, curl_off_t ul, curl_off_t ut) {
    (void) ud; (void) dl; (void) dt; (void) ul; (void) ut;
    return atomic_load(&g_cancel) ? 1 : 0;
}

/* one request: the HTTP status (0 = no answer); the body in out (free(out->data)). post NULL = GET */
static long http_call(const char *url, const char *post, const char *client_id, const char *bearer, Buf *out) {
    CURL *c = curl_easy_init();
    if (!c) return 0;
    struct curl_slist *h = NULL;
    char hdr[600];
    if (client_id && client_id[0]) {
        snprintf(hdr, sizeof(hdr), "Client-Id: %s", client_id);
        h = curl_slist_append(h, hdr);
    }
    if (bearer && bearer[0]) {
        snprintf(hdr, sizeof(hdr), "Authorization: Bearer %s", bearer);
        h = curl_slist_append(h, hdr);
    }
    curl_easy_setopt(c, CURLOPT_URL, url);
    if (h) curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    if (post) curl_easy_setopt(c, CURLOPT_POSTFIELDS, post);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, buf_write);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, out);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "kn_brain");
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, (long) C.timeout_ms);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, progress_cb);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    long code = 0;
    if (curl_easy_perform(c) == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(h);
    curl_easy_cleanup(c);
    return code;
}
#endif

/* the whole chain for one answer: token, game id, live streams. Fills j (worker thread). */
static void job_fetch(Job *j) {
#ifdef KN_HAVE_CURL
    Buf b = {0};
    long code = 0;
    const uint64_t now = kn_now_ms();
    if (!j->token[0] || now >= j->token_until) { /* the app access token (client credentials) */
        char *cid = curl_easy_escape(NULL, C.client_id, 0);
        char *sec = curl_easy_escape(NULL, C.client_secret, 0);
        if (cid && sec) {
            char body[1024];
            snprintf(body, sizeof(body), "client_id=%s&client_secret=%s&grant_type=client_credentials", cid, sec);
            code = http_call(C.auth_url, body, NULL, NULL, &b);
        }
        curl_free(cid);
        curl_free(sec);
        long expires = 0;
        if (code == 200 && streams_parse_token(b.data, j->token, sizeof(j->token), &expires)) {
            const long life = expires > STREAMS_TOKEN_MARGIN_S ? expires - STREAMS_TOKEN_MARGIN_S : STREAMS_TOKEN_MARGIN_S;
            j->token_until = now + (uint64_t) life * 1000u;
        } else {
            KN_WARN(MOD, "Twitch token request failed (HTTP %ld)", code);
            j->token[0] = 0;
            free(b.data);
            return;
        }
        free(b.data);
        b.data = NULL;
        b.len = 0;
    }
    if (!j->game_id[0]) { /* the Krunker category's id */
        char *g = curl_easy_escape(NULL, C.game, 0);
        char url[600];
        snprintf(url, sizeof(url), "%s/games?name=%s", C.api_url, g ? g : "");
        curl_free(g);
        code = http_call(url, NULL, C.client_id, j->token, &b);
        if (code == 401) j->token[0] = 0; /* the token expired early: fetch another next time */
        if (code != 200 || !streams_parse_game_id(b.data, j->game_id, sizeof(j->game_id))) {
            KN_WARN(MOD, "Twitch game lookup failed (HTTP %ld)", code);
            free(b.data);
            return;
        }
        free(b.data);
        b.data = NULL;
        b.len = 0;
    }
    char url[600]; /* the live streams of that category */
    snprintf(url, sizeof(url), "%s/streams?game_id=%s&first=%d", C.api_url, j->game_id, STREAMS_MAX_PER_PAGE);
    code = http_call(url, NULL, C.client_id, j->token, &b);
    if (code == 401) j->token[0] = 0;
    if (code == 200 && b.data) {
        cJSON *root = cJSON_Parse(b.data);
        j->data = streams_normalise(cJSON_GetObjectItemCaseSensitive(root, "data"));
        cJSON_Delete(root);
        j->ok = j->data != NULL;
    } else {
        KN_WARN(MOD, "Twitch streams request failed (HTTP %ld)", code);
    }
    free(b.data);
#else
    (void) j; /* built without libcurl: configured() cannot be true with a Twitch request to make */
#endif
}

static void job_done(kn_loop *loop, void *ud); /* loop thread, below */

static void *fetch_thread(void *arg) {
    Job *j = arg;
    job_fetch(j);
    kn_loop_post_threadsafe(g_b->loop, job_done, j); /* the loop thread takes j from here */
    atomic_fetch_sub(&g_live, 1);
    return NULL;
}

/* ------------------------------------------------------------------ loop thread */

static void flush_waiters(void) {
    const int n = S.nwaiting;
    kn_http_req *w[STREAMS_MAX_WAITING];
    memcpy(w, S.waiting, sizeof(w[0]) * (size_t) n);
    S.nwaiting = 0;
    for (int i = 0; i < n; i++) reply_list(w[i]);
}

static void job_done(kn_loop *loop, void *ud) {
    (void) loop;
    Job *j = ud;
    S.fetching = 0;
    memcpy(S.token, j->token, sizeof(S.token));
    S.token_until = j->token_until;
    memcpy(S.game_id, j->game_id, sizeof(S.game_id));
    if (j->ok && j->data) {
        cJSON_Delete(S.data);
        S.data = j->data;
        j->data = NULL;
        S.fetched = kn_now_ms();
    } else {
        cJSON_Delete(j->data);
    }
    free(j);
    flush_waiters();
}

/* starts one worker; 0 = could not start */
static int start_job(void) {
    Job *j = calloc(1, sizeof(*j));
    if (!j) return 0;
    memcpy(j->token, S.token, sizeof(j->token));
    j->token_until = S.token_until;
    memcpy(j->game_id, S.game_id, sizeof(j->game_id));
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
    S.fetching = 1;
    return 1;
}

static void h_top(kn_http_req *req, void *ud) {
    (void) ud;
    if (!configured()) {
        reply_error(req, 503, "StreamsUnavailable", "the live streams are not configured on this server");
        return;
    }
    if (S.data && (int64_t) (kn_now_ms() - S.fetched) < C.cache_ms) {
        reply_list(req);
        return;
    }
    if (S.nwaiting >= STREAMS_MAX_WAITING) {
        reply_error(req, 503, "StreamsUnavailable", "busy, try again");
        return;
    }
    S.waiting[S.nwaiting++] = req;
    if (!S.fetching && !start_job()) flush_waiters(); /* could not start: the last list, or an error */
}

static void route2(Brain *b, const char *path) {
    char alias[80];
    kn_http_route(b->http, "GET", path, h_top, b);
    snprintf(alias, sizeof(alias), "/api%s", path);
    kn_http_route(b->http, "GET", alias, h_top, b);
}

int streams_init(Brain *b) {
    g_b = b;
    config_load(b->cfg);
    route2(b, "/streams/top");
    if (!C.enabled) {
        KN_INFO(MOD, "Live Streams off (streams.enabled)");
        return 0;
    }
    if (!configured()) {
        KN_INFO(MOD, "Live Streams unconfigured: set KN_STREAMS_TWITCH_CLIENT_ID and KN_STREAMS_TWITCH_CLIENT_SECRET (the route answers 503)");
        return 0;
    }
#ifndef KN_HAVE_CURL
    KN_WARN(MOD, "built without libcurl: the Twitch request cannot be made");
#endif
    KN_INFO(MOD, "Live Streams on: Twitch category \"%s\", cached %lld s", C.game, (long long) (C.cache_ms / 1000));
    return 0;
}

void streams_shutdown(Brain *b) {
    (void) b;
    atomic_store(&g_cancel, 1);
    for (int i = 0; i < 200 && atomic_load(&g_live) > 0; i++) {
        struct timespec ts = {0, 10 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    cJSON_Delete(S.data);
    S.data = NULL;
}
