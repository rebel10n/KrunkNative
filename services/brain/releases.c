/*
 * services/brain/releases.c - polls the GitHub Releases API for the latest game server build
 * (releases.h) and flags outdated / incompatible servers in the lists (release_info.h rule).
 *
 * The poll runs on a worker thread (libcurl, like auth.c's hasJoined) and posts the result to
 * the loop; the event loop never blocks. Public repo, no token: 60 requests/hour per IP, so
 * the default 10 min interval (two brain processes on one host = 12/h) stays far below it.
 * ETag + If-None-Match: a 304 keeps the cache (and does not count against the limit). A 403/429
 * or X-RateLimit-Remaining: 0 waits until X-RateLimit-Reset (at least the interval); errors keep
 * the last good result and retry after the interval.
 */
#include "releases.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "build_info.h"
#include "brain_internal.h"
#include "knsvc/config.h"
#include "knsvc/json.h"
#include "knsvc/log.h"
#include "knsvc/loop.h"
#include "registry.h"

#define MOD "releases"
#define BODY_MAX (4 * 1024 * 1024)
#define DEFAULT_RELEASES_URL "https://api.github.com/repos/rebel10n/KrunkNative/releases?per_page=30"

typedef struct PollResult {
    int status;                    /* HTTP status, 0 = transport error */
    char *body;
    size_t len;
    char etag[128];
    long ratelimit_remaining;      /* -1 = absent */
    long long ratelimit_reset;     /* unix s, 0 = absent */
} PollResult;

static struct {
    Brain *b;
    ReleaseInfo latest;
    char url[512], etag[128];
    char cfg_version[64], cfg_download[256];
    int poll_s;
    int64_t grace_s;
    kn_timer_id timer;
    pthread_t thread;
    int thread_live;               /* joinable thread not yet joined */
    int inflight;
    int64_t next_poll_ms;          /* wall clock */
    int warned_empty;
    PollResult res;
} g_rel;

const ReleaseInfo *releases_latest(void) {
    return &g_rel.latest;
}

/* ---------------------------------------------------------------- worker */

#ifdef KN_HAVE_CURL
#include <curl/curl.h>

static size_t on_body(char *data, size_t size, size_t n, void *ud) {
    PollResult *r = ud;
    const size_t len = size * n;
    if (r->len + len > BODY_MAX) return 0;
    char *grown = realloc(r->body, r->len + len + 1);
    if (!grown) return 0;
    memcpy(grown + r->len, data, len);
    r->len += len;
    grown[r->len] = 0;
    r->body = grown;
    return len;
}

static void header_value(const char *line, size_t len, const char *name, char *out, size_t cap) {
    const size_t nl = strlen(name);
    if (len <= nl + 1 || strncasecmp(line, name, nl) != 0 || line[nl] != ':') return;
    const char *v = line + nl + 1;
    const char *end = line + len;
    while (v < end && (*v == ' ' || *v == '\t')) v++;
    while (end > v && (end[-1] == '\r' || end[-1] == '\n' || end[-1] == ' ')) end--;
    snprintf(out, cap, "%.*s", (int) (end - v), v);
}

static size_t on_header(char *data, size_t size, size_t n, void *ud) {
    PollResult *r = ud;
    const size_t len = size * n;
    char v[160] = "";
    header_value(data, len, "etag", r->etag, sizeof(r->etag));
    header_value(data, len, "x-ratelimit-remaining", v, sizeof(v));
    if (v[0]) r->ratelimit_remaining = strtol(v, NULL, 10);
    v[0] = 0;
    header_value(data, len, "x-ratelimit-reset", v, sizeof(v));
    if (v[0]) r->ratelimit_reset = strtoll(v, NULL, 10);
    return len;
}
#endif

static void poll_done(kn_loop *loop, void *ud);

static void *poll_thread(void *arg) {
    (void) arg;
    PollResult *r = &g_rel.res;
#ifdef KN_HAVE_CURL
    CURL *c = curl_easy_init();
    if (c) {
        char inm[160];
        struct curl_slist *h = curl_slist_append(NULL, "Accept: application/vnd.github+json");
        h = curl_slist_append(h, "X-GitHub-Api-Version: 2022-11-28");
        if (g_rel.etag[0]) {
            snprintf(inm, sizeof(inm), "If-None-Match: %s", g_rel.etag);
            h = curl_slist_append(h, inm);
        }
        curl_easy_setopt(c, CURLOPT_URL, g_rel.url);
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
        curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, 10000L);
        curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(c, CURLOPT_MAXREDIRS, 3L);
#if LIBCURL_VERSION_NUM >= 0x075500 /* 7.85: CURLOPT_PROTOCOLS deprecated */
        curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "https,http");
        curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, "https");
#else
        curl_easy_setopt(c, CURLOPT_PROTOCOLS, (long) (CURLPROTO_HTTPS | CURLPROTO_HTTP));
        curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS, (long) CURLPROTO_HTTPS);
#endif
        curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_body);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, r);
        curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, on_header);
        curl_easy_setopt(c, CURLOPT_HEADERDATA, r);
        curl_easy_setopt(c, CURLOPT_USERAGENT, "kn_brain (KrunkNative server finder)");
        if (curl_easy_perform(c) == CURLE_OK) {
            long code = 0;
            curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
            r->status = (int) code;
        }
        curl_slist_free_all(h);
        curl_easy_cleanup(c);
    }
#endif
    kn_loop_post_threadsafe(g_rel.b->loop, poll_done, NULL);
    return NULL;
}

static void join_thread(void) {
    if (g_rel.thread_live) {
        pthread_join(g_rel.thread, NULL);
        g_rel.thread_live = 0;
    }
}

static void apply_overrides(void) {
    release_apply_overrides(&g_rel.latest, g_rel.cfg_version, g_rel.cfg_download);
}

static void poll_done(kn_loop *loop, void *ud) {
    (void) loop;
    (void) ud;
    join_thread();
    g_rel.inflight = 0;
    PollResult *r = &g_rel.res;
    const int64_t now = kn_wall_ms();
    int64_t wait_ms = (int64_t) g_rel.poll_s * 1000;
    if (r->status == 200 && r->body) {
        cJSON *arr = cJSON_ParseWithLength(r->body, r->len);
        ReleaseInfo pick;
        if (cJSON_IsArray(arr) && release_pick_latest(arr, &pick)) {
            const int changed = !release_same_commit(pick.commit, g_rel.latest.commit) || strcmp(pick.tag, g_rel.latest.tag);
            g_rel.latest = pick;
            apply_overrides();
            if (changed)
                KN_INFO(MOD, "latest server build %s (%s, %.12s, protocol %d.%d)", pick.describe, pick.tag, pick.commit,
                        pick.proto_major, pick.proto_minor);
            snprintf(g_rel.etag, sizeof(g_rel.etag), "%s", r->etag);
        } else if (!g_rel.warned_empty) {
            g_rel.warned_empty = 1; /* until release.yml has published one: say it once */
            KN_WARN(MOD, "%s: no server release with kn-build metadata yet", g_rel.url);
        }
        cJSON_Delete(arr);
    } else if (r->status == 304) {
        KN_DEBUG(MOD, "releases unchanged (304)");
    } else if (r->status == 403 || r->status == 429) {
        const int64_t reset_ms = r->ratelimit_reset > 0 ? (int64_t) r->ratelimit_reset * 1000 - now : 0;
        if (reset_ms > wait_ms) wait_ms = reset_ms;
        KN_WARN(MOD, "GitHub API HTTP %d (rate limited): next poll in %lld s", r->status, (long long) (wait_ms / 1000));
    } else {
        KN_WARN(MOD, "GitHub API %s: %s %d", g_rel.url, r->status ? "HTTP" : "transport error", r->status);
    }
    if (r->ratelimit_remaining == 0 && r->ratelimit_reset > 0) {
        const int64_t reset_ms = (int64_t) r->ratelimit_reset * 1000 - now;
        if (reset_ms > wait_ms) wait_ms = reset_ms;
    }
    free(r->body);
    memset(r, 0, sizeof(*r));
    g_rel.next_poll_ms = now + wait_ms;
}

static void poll_start(void) {
    if (g_rel.inflight || g_rel.b->draining) return;
    memset(&g_rel.res, 0, sizeof(g_rel.res));
    g_rel.res.ratelimit_remaining = -1;
    g_rel.inflight = 1;
    if (pthread_create(&g_rel.thread, NULL, poll_thread, NULL) != 0) {
        g_rel.inflight = 0;
        g_rel.next_poll_ms = kn_wall_ms() + (int64_t) g_rel.poll_s * 1000;
        return;
    }
    g_rel.thread_live = 1;
}

static void tick(kn_loop *loop, void *ud) {
    (void) loop;
    (void) ud;
    if (kn_wall_ms() >= g_rel.next_poll_ms) poll_start();
}

/* ---------------------------------------------------------------- module */

int releases_init(Brain *b) {
    g_rel.b = b;
    const struct kn_config *cfg = b->cfg;
    memset(&g_rel.latest, 0, sizeof(g_rel.latest));
    g_rel.latest.proto_major = g_rel.latest.proto_minor = -1;
    snprintf(g_rel.url, sizeof(g_rel.url), "%s", kn_config_str(cfg, "servers.releases_url", DEFAULT_RELEASES_URL));
    snprintf(g_rel.cfg_version, sizeof(g_rel.cfg_version), "%s", kn_config_str(cfg, "servers.latest_version", ""));
    snprintf(g_rel.cfg_download, sizeof(g_rel.cfg_download), "%s", kn_config_str(cfg, "servers.download_url", ""));
    int64_t poll = kn_config_int(cfg, "servers.latest_poll_s", 600);
    g_rel.poll_s = poll <= 0 ? 0 : poll < 60 ? 60 : poll > 86400 ? 86400 : (int) poll;
    int64_t grace_h = kn_config_int(cfg, "servers.outdated_grace_h", 48);
    g_rel.grace_s = (grace_h < 0 ? 0 : grace_h) * 3600;
    apply_overrides();
    if (g_rel.cfg_version[0]) {
        KN_INFO(MOD, "latest server build from config: %s (no release poll)", g_rel.cfg_version);
        return 0;
    }
#ifndef KN_HAVE_CURL
    if (g_rel.poll_s && !strncmp(g_rel.url, "https://", 8)) {
        KN_WARN(MOD, "built without libcurl: no release poll (set servers.latest_version)");
        g_rel.poll_s = 0;
    }
#endif
    if (!g_rel.poll_s) return 0;
    g_rel.next_poll_ms = kn_wall_ms() + 2000; /* first poll right after startup */
    g_rel.timer = kn_loop_add_timer(b->loop, 1000, 1000, tick, NULL);
    KN_INFO(MOD, "polling %s every %d s (outdated grace %lld h)", g_rel.url, g_rel.poll_s, (long long) (g_rel.grace_s / 3600));
    return 0;
}

void releases_shutdown(Brain *b) {
    if (g_rel.timer) kn_loop_cancel_timer(b->loop, g_rel.timer);
    g_rel.timer = 0;
    join_thread(); /* at most the 10 s request timeout; its posted task finds nothing to do */
    free(g_rel.res.body);
    memset(&g_rel.res, 0, sizeof(g_rel.res));
}

/* ---------------------------------------------------------------- lists */

static int my_major(void) { return kn_build_info()->protocol_major; }
static int my_minor(void) { return kn_build_info()->protocol_minor; }

int releases_client_major(const char *proto) {
    int ma, mi;
    return release_parse_protocol(proto, &ma, &mi) ? ma : -1;
}

static void set_bool(cJSON *o, const char *key, int v) {
    cJSON *b = cJSON_CreateBool(v);
    if (cJSON_GetObjectItemCaseSensitive(o, key)) cJSON_ReplaceItemInObjectCaseSensitive(o, key, b);
    else cJSON_AddItemToObject(o, key, b);
}

void releases_annotate_finder(cJSON *reply, cJSON *servers, int client_major) {
    const int cm = client_major >= 0 ? client_major : my_major();
    cJSON *e;
    cJSON_ArrayForEach(e, servers) {
        ServerBuild s;
        release_server_build_from_finder(e, &s);
        set_bool(e, "outdated", release_server_outdated(&g_rel.latest, &s, g_rel.grace_s, my_major(), my_minor()));
        set_bool(e, "incompatible", release_protocol_incompatible(cm, s.proto_major));
    }
    release_add_json(&g_rel.latest, reply);
}

void releases_annotate_tuples(cJSON *reply, cJSON *tuples, int client_major) {
    const int cm = client_major >= 0 ? client_major : my_major();
    cJSON *t;
    cJSON_ArrayForEach(t, tuples) {
        cJSON *meta = cJSON_GetArrayItem(t, 4);
        if (!cJSON_IsObject(meta)) continue;
        ServerBuild s;
        release_server_build_from_meta(meta, &s);
        set_bool(meta, "outdated", release_server_outdated(&g_rel.latest, &s, g_rel.grace_s, my_major(), my_minor()));
        set_bool(meta, "incompatible", release_protocol_incompatible(cm, s.proto_major));
    }
    release_add_json(&g_rel.latest, reply);
}

int releases_lobby_outdated(const MmLobby *l, void *ud) {
    (void) ud;
    const cJSON *meta = cJSON_GetArrayItem(l->tuple, 4);
    ServerBuild s;
    release_server_build_from_meta(meta, &s);
    return release_server_outdated(&g_rel.latest, &s, g_rel.grace_s, my_major(), my_minor());
}
