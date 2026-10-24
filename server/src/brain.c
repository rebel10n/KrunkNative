// Game server -> kn_brain link: server/include/brain.h (v3.1.1 server/brainClient.ts).
#include <brain.h>
#include <server_log.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef KRUNKNATIVE_HAVE_KNSVC
#include <knsvc/crypto.h>
#include <knsvc/loop.h>
#include <knsvc/ws.h>

#define BRAIN_RECONNECT_MIN_MS 1000   // brainClient.ts RECONNECT_MIN_MS
#define BRAIN_RECONNECT_MAX_MS 30000  // RECONNECT_MAX_MS
// WebSocket-level keepalive on top of the brain's JSON pings (brain.ping_interval_s, 10 s):
// keeps proxies (Cloudflare drops idle websockets after 100 s) from idling the link out and
// detects half-open links when the coordinator vanishes without a FIN.
#define BRAIN_WS_PING_MS 25000
#define BRAIN_WS_PONG_TIMEOUT_MS 60000
#define BRAIN_EVENT_MAX 1024
#define BRAIN_RELIABLE_MAX 512

typedef struct BrainReliable {
    uint32_t rid;
    char *text;
    struct BrainReliable *next;
} BrainReliable;

static struct {
    int enabled;
    pthread_t thread;
    int thread_started;
    kn_loop *loop;

    // loop-thread state
    kn_ws *ws;
    int connected;
    int welcomed;
    uint64_t reconnect_ms;
    kn_timer_id reconnect_timer;
    BrainReliable *reliable;       // commits awaiting their reply (resent after reconnects)
    int reliable_count;
    int stopping;

    char url[GS_STR_LARGE];
    char auth_header[GS_STR_LARGE];
    char origin[GS_STR_LARGE];     // https://<coordinator host> (wss) / http://... (ws)
    char join_secret[GS_STR_SMALL * 2];
    char *hello;                   // hello frame text (sent on every open)

    // shared with the main thread
    pthread_mutex_t lock;
    BrainEvent events[BRAIN_EVENT_MAX];
    int event_head, event_count;
    volatile int linked;
    uint32_t next_rid;
} g_brain = {.lock = PTHREAD_MUTEX_INITIALIZER};

static void brain_push_event(const BrainEventKind kind, cJSON *frame) {
    pthread_mutex_lock(&g_brain.lock);
    if (g_brain.event_count < BRAIN_EVENT_MAX) {
        BrainEvent *ev = &g_brain.events[(g_brain.event_head + g_brain.event_count++) % BRAIN_EVENT_MAX];
        ev->kind = kind;
        ev->frame = frame;
        frame = NULL;
    }
    pthread_mutex_unlock(&g_brain.lock);
    if (frame) {
        server_log_error("[brain] event queue full, dropping a frame");
        cJSON_Delete(frame);
    }
}

int brain_next_event(BrainEvent *out) {
    if (!g_brain.enabled) return 0;
    pthread_mutex_lock(&g_brain.lock);
    const int has = g_brain.event_count > 0;
    if (has) {
        *out = g_brain.events[g_brain.event_head];
        g_brain.event_head = (g_brain.event_head + 1) % BRAIN_EVENT_MAX;
        g_brain.event_count--;
    }
    pthread_mutex_unlock(&g_brain.lock);
    return has;
}

int brain_enabled(void) { return g_brain.enabled; }
int brain_linked(void) { return g_brain.enabled && g_brain.linked; }

uint32_t brain_next_rid(void) {
    pthread_mutex_lock(&g_brain.lock);
    const uint32_t rid = ++g_brain.next_rid;
    pthread_mutex_unlock(&g_brain.lock);
    return rid;
}

// ------------------------------------------------------------------ loop thread

static void brain_connect(void);

static int brain_ready(void) {
    return g_brain.ws && g_brain.connected && g_brain.welcomed && kn_ws_is_open(g_brain.ws);
}

static void brain_send_text(const char *text) {
    if (g_brain.ws && g_brain.connected) kn_ws_send_text(g_brain.ws, text, (size_t) -1);
}

static void brain_reliable_ack(const uint32_t rid) {
    for (BrainReliable **p = &g_brain.reliable; *p; p = &(*p)->next) {
        if ((*p)->rid != rid) continue;
        BrainReliable *done = *p;
        *p = done->next;
        free(done->text);
        free(done);
        g_brain.reliable_count--;
        return;
    }
}

static void brain_resend_reliable(void) {
    for (BrainReliable *r = g_brain.reliable; r; r = r->next) brain_send_text(r->text);
}

static void brain_on_open(kn_ws *ws, void *ud) {
    (void) ws;
    (void) ud;
    g_brain.connected = 1;
    server_log("[brain] connected to %s", g_brain.url);
    if (g_brain.hello) brain_send_text(g_brain.hello);
}

static void brain_on_message(kn_ws *ws, const void *data, const size_t len, const int is_binary, void *ud) {
    (void) ud;
    if (is_binary) return;
    cJSON *frame = cJSON_ParseWithLength((const char *) data, len);
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(frame, "type");
    if (!cJSON_IsString(type)) {
        cJSON_Delete(frame);
        return;
    }

    const char *t = type->valuestring;
    if (!strcmp(t, "ping")) {
        cJSON *pong = cJSON_CreateObject();
        cJSON_AddStringToObject(pong, "type", "pong");
        const cJSON *ts = cJSON_GetObjectItemCaseSensitive(frame, "ts");
        if (ts) cJSON_AddItemToObject(pong, "ts", cJSON_Duplicate(ts, 1));
        char *text = cJSON_PrintUnformatted(pong);
        if (text) kn_ws_send_text(ws, text, (size_t) -1);
        free(text);
        cJSON_Delete(pong);
        cJSON_Delete(frame);
        return;
    }

    if (!strcmp(t, "welcome")) {
        g_brain.welcomed = 1;
        g_brain.linked = 1;
        g_brain.reconnect_ms = BRAIN_RECONNECT_MIN_MS; // only a welcomed link resets the backoff
        const cJSON *region = cJSON_GetObjectItemCaseSensitive(frame, "assignedRegion");
        server_log("[brain] welcomed (region=%s); pushing snapshot", cJSON_IsString(region) ? region->valuestring : "?");
        brain_resend_reliable(); // commits that were in flight when the last link dropped
        brain_push_event(BRAIN_EV_LINK_UP, frame); // the welcome (community name, brainId)
        return;
    }

    if (!strcmp(t, "kick")) {
        const cJSON *reason = cJSON_GetObjectItemCaseSensitive(frame, "reason");
        server_log_error("[brain] kicked: %s", cJSON_IsString(reason) ? reason->valuestring : "?");
        cJSON_Delete(frame);
        return;
    }

    const cJSON *rid = cJSON_GetObjectItemCaseSensitive(frame, "rid");
    if (cJSON_IsNumber(rid)) brain_reliable_ack((uint32_t) rid->valuedouble);
    brain_push_event(BRAIN_EV_FRAME, frame);
}

static void brain_reconnect_cb(kn_loop *loop, void *ud) {
    (void) loop;
    (void) ud;
    g_brain.reconnect_timer = 0;
    brain_connect();
}

static void brain_schedule_reconnect(void) {
    if (g_brain.stopping || g_brain.reconnect_timer) return;
    // +-20% jitter: a coordinator restart must not get every game server back in the same millisecond
    uint8_t rnd = 0;
    kn_random_bytes(&rnd, 1);
    const uint64_t delay = g_brain.reconnect_ms * (80u + (uint64_t) rnd % 41u) / 100u;
    g_brain.reconnect_ms = g_brain.reconnect_ms * 2 > BRAIN_RECONNECT_MAX_MS ? BRAIN_RECONNECT_MAX_MS : g_brain.reconnect_ms * 2;
    g_brain.reconnect_timer = kn_loop_add_timer(g_brain.loop, delay, 0, brain_reconnect_cb, NULL);
}

static void brain_on_close(kn_ws *ws, const int code, const char *reason, void *ud) {
    (void) ws;
    (void) ud;
    const int was_ready = g_brain.welcomed;
    g_brain.ws = NULL;
    g_brain.connected = 0;
    g_brain.welcomed = 0;
    g_brain.linked = 0;
    if (was_ready) server_log_error("[brain] disconnected (code=%d, reason=%s)", code, reason && reason[0] ? reason : "-");
    else if (!g_brain.stopping) server_log_error("[brain] connect to %s failed (%d %s), retrying in %llus", g_brain.url, code,
                                                 reason && reason[0] ? reason : "-", (unsigned long long) (g_brain.reconnect_ms / 1000));
    if (was_ready) brain_push_event(BRAIN_EV_LINK_DOWN, NULL);
    brain_schedule_reconnect();
}

static void brain_connect(void) {
    if (g_brain.stopping) return;
    static const kn_ws_callbacks callbacks = {
        .on_open = brain_on_open,
        .on_message = brain_on_message,
        .on_close = brain_on_close,
    };
    const char *headers[1] = {g_brain.auth_header};
    const kn_ws_options options = {
        .max_message_bytes = 4u << 20,
        .handshake_timeout_ms = 10000,
        .ping_interval_ms = BRAIN_WS_PING_MS,
        .pong_timeout_ms = BRAIN_WS_PONG_TIMEOUT_MS,
        .headers = headers,
        .header_count = g_brain.auth_header[0] ? 1 : 0,
        .origin = g_brain.origin[0] ? g_brain.origin : NULL,
    };
    g_brain.ws = kn_ws_connect(g_brain.loop, g_brain.url, &callbacks, &options, NULL);
    if (!g_brain.ws) {
        server_log_error("[brain] invalid brain_url %s", g_brain.url);
        brain_schedule_reconnect();
    }
}

typedef struct {
    char *text;
    uint32_t rid;
    int reliable;
} BrainOutgoing;

static void brain_send_task(kn_loop *loop, void *ud) {
    (void) loop;
    BrainOutgoing *out = ud;

    if (out->reliable) {
        if (g_brain.reliable_count >= BRAIN_RELIABLE_MAX) {
            server_log_error("[brain] too many unacknowledged commits, dropping rid %u", out->rid);
            free(out->text);
        } else {
            BrainReliable *r = malloc(sizeof(*r));
            if (r) {
                r->rid = out->rid;
                r->text = out->text;
                r->next = NULL;
                BrainReliable **tail = &g_brain.reliable;
                while (*tail) tail = &(*tail)->next;
                *tail = r;
                g_brain.reliable_count++;
                if (brain_ready()) brain_send_text(r->text);
            } else {
                free(out->text);
            }
        }
    } else {
        if (brain_ready() || strstr(out->text, "\"type\":\"hello\"")) brain_send_text(out->text);
        free(out->text);
    }
    free(out);
}

int brain_post(cJSON *frame, const int reliable) {
    if (!frame) return -1;
    if (!g_brain.enabled || !g_brain.loop) {
        cJSON_Delete(frame);
        return -1;
    }
    BrainOutgoing *out = calloc(1, sizeof(*out));
    const cJSON *rid = cJSON_GetObjectItemCaseSensitive(frame, "rid");
    if (out) {
        out->text = cJSON_PrintUnformatted(frame);
        out->rid = cJSON_IsNumber(rid) ? (uint32_t) rid->valuedouble : 0;
        out->reliable = reliable && out->rid;
    }
    cJSON_Delete(frame);
    if (!out || !out->text || kn_loop_post_threadsafe(g_brain.loop, brain_send_task, out)) {
        if (out) free(out->text);
        free(out);
        return -1;
    }
    return 0;
}

static void *brain_thread_main(void *arg) {
    (void) arg;
    kn_loop_run(g_brain.loop);
    return NULL;
}

static void brain_start_task(kn_loop *loop, void *ud) {
    (void) loop;
    (void) ud;
    brain_connect();
}

// The coordinator URL's host is this machine (localhost / 127.x / [::1]).
static int brain_url_is_loopback(const char *url) {
    const char *host = strstr(url, "://");
    host = host ? host + 3 : url;
    return !strncmp(host, "localhost", 9) || !strncmp(host, "127.", 4) || !strncmp(host, "[::1]", 5);
}

int brain_start(const GsConfig *cfg) {
    if (!cfg->brain_url[0]) return 0;

    // Security (doc/v7/command-auth.md): the join secret signs the tickets that say which account
    // (and so which DEV / op / custom host) a connection is. The built-in "kn-dev-secret" (or a
    // blank one) is public: anyone could sign a ticket for any account. It is only accepted for a
    // coordinator on this machine (development); otherwise the server refuses to link and runs
    // standalone (guests only, no ticket is ever trusted).
    const char *join_secret = cfg->join_secret[0] ? cfg->join_secret : cfg->brain_secret;
    if ((!join_secret[0] || !strcmp(join_secret, "kn-dev-secret")) && !brain_url_is_loopback(cfg->brain_url)) {
        server_log_error("[brain] not linking to %s: the coordinator/join secret is %s; set gameserver.brain_secret "
                         "(= brain.internal_secret). Running standalone", cfg->brain_url, join_secret[0] ? "the public development default" : "blank");
        return 0;
    }

    snprintf(g_brain.url, sizeof(g_brain.url), "%s", cfg->brain_url);
    if (cfg->brain_secret[0]) snprintf(g_brain.auth_header, sizeof(g_brain.auth_header), "Authorization: Bearer %s", cfg->brain_secret);
    snprintf(g_brain.join_secret, sizeof(g_brain.join_secret), "%s", cfg->join_secret[0] ? cfg->join_secret : cfg->brain_secret);
    g_brain.reconnect_ms = BRAIN_RECONNECT_MIN_MS;
    // Origin for proxies in front of the coordinator (wss://coordinator.krunk.io via Cloudflare)
    const int tls = !strncmp(g_brain.url, "wss://", 6);
    if (tls || !strncmp(g_brain.url, "ws://", 5)) {
        const char *host = g_brain.url + (tls ? 6 : 5);
        snprintf(g_brain.origin, sizeof(g_brain.origin), "%s://%.*s", tls ? "https" : "http", (int) strcspn(host, "/?#"), host);
    }

    g_brain.loop = kn_loop_new();
    if (!g_brain.loop) {
        server_log_error("[brain] cannot create the event loop; running standalone");
        return 0;
    }
    g_brain.enabled = 1;
    brain_set_http_base(cfg);
    return 1;
}

// The hello frame is built by accounts.c (it knows the lobby); the thread starts with it.
void brain_set_hello_and_run(cJSON *hello) {
    if (!g_brain.enabled) {
        cJSON_Delete(hello);
        return;
    }
    free(g_brain.hello);
    g_brain.hello = cJSON_PrintUnformatted(hello);
    cJSON_Delete(hello);
    if (g_brain.thread_started) return;

    kn_loop_post(g_brain.loop, brain_start_task, NULL);
    if (pthread_create(&g_brain.thread, NULL, brain_thread_main, NULL)) {
        server_log_error("[brain] cannot start the brain thread; running standalone");
        g_brain.enabled = 0;
        return;
    }
    g_brain.thread_started = 1;
}

static void brain_close_task(kn_loop *loop, void *ud) {
    (void) ud;
    g_brain.stopping = 1;
    if (g_brain.reconnect_timer) kn_loop_cancel_timer(loop, g_brain.reconnect_timer);
    g_brain.reconnect_timer = 0;
    if (g_brain.ws) kn_ws_close(g_brain.ws, KN_WS_CLOSE_GOING_AWAY, "shutdown");
}

static void brain_count_task(kn_loop *loop, void *ud) {
    (void) loop;
    int *out = ud;
    out[0] = g_brain.reliable_count;
    out[1] = 1;
}

void brain_stop(const int wait_ms) {
    if (!g_brain.enabled || !g_brain.thread_started) return;

    // let queued commits get their replies
    for (int waited = 0; waited < wait_ms && g_brain.linked; waited += 50) {
        volatile int state[2] = {0, 0};
        kn_loop_post_threadsafe(g_brain.loop, brain_count_task, (void *) state);
        for (int i = 0; i < 20 && !state[1]; i++) {
            const struct timespec ts = {0, 5000000L};
            nanosleep(&ts, NULL);
        }
        if (state[1] && !state[0]) break;
        const struct timespec ts = {0, 50000000L};
        nanosleep(&ts, NULL);
    }
    if (g_brain.reliable_count) server_log_error("[brain] %d commit(s) were not acknowledged before shutdown", g_brain.reliable_count);

    kn_loop_post_threadsafe(g_brain.loop, brain_close_task, NULL);
    for (int i = 0; i < 40 && g_brain.ws; i++) { // close handshake (at most ~2 s)
        const struct timespec ts = {0, 50000000L};
        nanosleep(&ts, NULL);
    }
    kn_loop_stop(g_brain.loop);
    pthread_join(g_brain.thread, NULL);
    g_brain.thread_started = 0;
    g_brain.linked = 0;
}


// ------------------------------------------------------------------ published map fetch
// GET <brain http>/api/map-data?name=<name>&meta=1 (svc-accounts, doc/backend.md 8.7) on its own
// thread: plain HTTP/1.1 (TLS is nginx's job), Content-Length or close-delimited body.
// The result arrives on the main thread as a BRAIN_EV_FRAME
// {"type":"map-data","rid":n,"ok":bool,"map":{...},"id":map_id,"err":"..."} ("id" when the brain gave one).

#include <http_client.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define BRAIN_MAP_FETCH_MAX (4u * 1024u * 1024u) // NET_MAP_MAX_SIZE

typedef struct {
    uint32_t rid;
    char host[128];
    char port[8];
    char path[512];
} BrainFetch;

static char g_http_base[GS_STR_LARGE]; // gameserver.brain_http_url, else derived from brain_url

static void brain_url_encode(char *out, const size_t size, const char *in) {
    static const char hex[] = "0123456789ABCDEF";
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *) in; *p && n + 4 < size; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || strchr("-_.~", *p)) out[n++] = (char) *p;
        else {
            out[n++] = '%';
            out[n++] = hex[*p >> 4];
            out[n++] = hex[*p & 15];
        }
    }
    out[n] = 0;
}

// map-data's meta=1 answer {"id": map_id, "map": {...}} (services/brain/creations.c): the map, its id in *map_id (v10.0.3
// customMapData.id). A brain without meta=1 answers the bare map: kept as it is, id 0.
static cJSON *brain_map_unwrap(cJSON *body, int *map_id) {
    if (map_id) *map_id = 0;
    const cJSON *id = cJSON_GetObjectItem(body, "id");
    if (!cJSON_IsObject(body) || cJSON_GetArraySize(body) != 2 || !cJSON_IsNumber(id) || !cJSON_IsObject(cJSON_GetObjectItem(body, "map")))
        return body;
    if (map_id) *map_id = id->valueint;
    cJSON *map = cJSON_DetachItemFromObject(body, "map");
    cJSON_Delete(body);
    return map;
}

static void brain_fetch_done(const uint32_t rid, cJSON *map, const int map_id, const char *err) {
    cJSON *frame = cJSON_CreateObject();
    cJSON_AddStringToObject(frame, "type", "map-data");
    cJSON_AddNumberToObject(frame, "rid", rid);
    cJSON_AddBoolToObject(frame, "ok", map != NULL);
    if (map) cJSON_AddItemToObject(frame, "map", map);
    if (map && map_id > 0) cJSON_AddNumberToObject(frame, "id", map_id); // the published map's id (published_map_note_id)
    else cJSON_AddStringToObject(frame, "err", err ? err : "fetch failed");
    brain_push_event(BRAIN_EV_FRAME, frame);
}

static void *brain_fetch_thread(void *arg) {
    BrainFetch *f = arg;
    struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM}, *res = NULL;
    char *buf = NULL;
    size_t len = 0;
    const char *err = NULL;
    int fd = -1;

    if (getaddrinfo(f->host, f->port, &hints, &res) || !res) err = "cannot resolve the brain host";
    for (struct addrinfo *a = res; a && fd < 0 && !err; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
        const struct timeval tv = {10, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        if (connect(fd, a->ai_addr, a->ai_addrlen)) {
            close(fd);
            fd = -1;
        }
    }
    if (res) freeaddrinfo(res);
    if (fd < 0 && !err) err = "cannot connect to the brain";

    if (!err) {
        char request[1024];
        const int n = snprintf(request, sizeof(request), "GET %s HTTP/1.1\r\nHost: %s:%s\r\nAccept: application/json\r\nConnection: close\r\n\r\n",
                               f->path, f->host, f->port);
        if (send(fd, request, (size_t) n, MSG_NOSIGNAL) != n) err = "request failed";
    }
    while (!err) {
        char chunk[16384];
        const ssize_t got = recv(fd, chunk, sizeof(chunk), 0);
        if (got <= 0) break;
        if (len + (size_t) got > BRAIN_MAP_FETCH_MAX + 4096) {
            err = "map too large";
            break;
        }
        char *grown = realloc(buf, len + (size_t) got + 1);
        if (!grown) {
            err = "out of memory";
            break;
        }
        buf = grown;
        memcpy(buf + len, chunk, (size_t) got);
        len += (size_t) got;
        buf[len] = 0;
    }
    if (fd >= 0) close(fd);

    cJSON *map = NULL;
    int map_id = 0;
    if (!err) {
        const char *body = buf ? strstr(buf, "\r\n\r\n") : NULL;
        int status = 0;
        if (!buf || sscanf(buf, "HTTP/1.%*d %d", &status) != 1 || !body) err = "bad HTTP response";
        else if (status == 404) err = "map not found";
        else if (status != 200) err = "map service error";
        else if (strstr(buf, "Transfer-Encoding: chunked") && strstr(buf, "Transfer-Encoding: chunked") < body) err = "chunked responses are not supported";
        else {
            map = brain_map_unwrap(cJSON_Parse(body + 4), &map_id);
            if (!cJSON_IsObject(map) || !cJSON_IsArray(cJSON_GetObjectItem(map, "objects"))) {
                cJSON_Delete(map);
                map = NULL;
                err = "not a Krunker map";
            }
        }
    }
    free(buf);
    brain_fetch_done(f->rid, map, map_id, err);
    free(f);
    return NULL;
}

void brain_set_http_base(const GsConfig *cfg) {
    if (cfg->brain_http_url[0]) {
        snprintf(g_http_base, sizeof(g_http_base), "%s", cfg->brain_http_url);
        return;
    }
    // ws://host:port/mm/internal/ws -> http://host:port, wss://host/... -> https://host
    const char *url = cfg->brain_url;
    const int tls = !strncmp(url, "wss://", 6);
    const char *rest = !strncmp(url, "ws://", 5) ? url + 5 : tls ? url + 6 : url;
    const size_t host_len = strcspn(rest, "/");
    snprintf(g_http_base, sizeof(g_http_base), "%s://%.*s", tls ? "https" : "http", (int) host_len, rest);
}

// https:// base: the blocking http_client (libcurl) on a worker thread instead.
typedef struct {
    uint32_t rid;
    char url[GS_STR_LARGE + 320];
} BrainFetchHttps;

static void *brain_fetch_https_thread(void *arg) {
    BrainFetchHttps *f = arg;
    HttpResult res;
    cJSON *map = NULL;
    int map_id = 0;
    const char *err = NULL;
    if (http_request("GET", f->url, NULL, NULL, 10000, BRAIN_MAP_FETCH_MAX, &res) != 0) err = res.error[0] ? res.error : "cannot reach the brain";
    else if (res.status == 404) err = "map not found";
    else if (res.status != 200) err = "map service error";
    else {
        map = brain_map_unwrap(res.body ? cJSON_Parse(res.body) : NULL, &map_id);
        if (!cJSON_IsObject(map) || !cJSON_IsArray(cJSON_GetObjectItem(map, "objects"))) {
            cJSON_Delete(map);
            map = NULL;
            err = "not a Krunker map";
        }
    }
    char err_copy[128];
    snprintf(err_copy, sizeof(err_copy), "%s", err ? err : "");
    http_result_free(&res);
    brain_fetch_done(f->rid, map, map_id, err ? err_copy : NULL);
    free(f);
    return NULL;
}

int brain_fetch_map(const uint32_t rid, const char *name, const char *creator) {
    if (!g_brain.enabled || !name || !name[0]) return -1;
    if (!strncmp(g_http_base, "https://", 8)) {
        BrainFetchHttps *f = calloc(1, sizeof(*f));
        if (!f) return -1;
        f->rid = rid;
        char encoded[256], enc_creator[192];
        brain_url_encode(encoded, sizeof(encoded), name);
        brain_url_encode(enc_creator, sizeof(enc_creator), creator ? creator : "");
        snprintf(f->url, sizeof(f->url), "%s/api/map-data?name=%s%s%s&meta=1", g_http_base, encoded, enc_creator[0] ? "&creator=" : "", enc_creator);
        server_log("[brain] fetching published map '%s' from %s", name, g_http_base);
        pthread_t thread;
        if (pthread_create(&thread, NULL, brain_fetch_https_thread, f)) {
            free(f);
            return -1;
        }
        pthread_detach(thread);
        return 0;
    }
    const char *rest = !strncmp(g_http_base, "http://", 7) ? g_http_base + 7 : NULL;
    if (!rest) {
        server_log_error("[brain] published maps need an http(s):// gameserver.brain_http_url (got '%s')", g_http_base);
        return -1;
    }
    BrainFetch *f = calloc(1, sizeof(*f));
    if (!f) return -1;
    f->rid = rid;
    const size_t host_len = strcspn(rest, ":/");
    snprintf(f->host, sizeof(f->host), "%.*s", (int) host_len, rest);
    if (rest[host_len] == ':') snprintf(f->port, sizeof(f->port), "%.*s", (int) strcspn(rest + host_len + 1, "/"), rest + host_len + 1);
    else snprintf(f->port, sizeof(f->port), "80");
    const char *prefix = strchr(rest, '/');
    char encoded[256], enc_creator[192];
    brain_url_encode(encoded, sizeof(encoded), name);
    brain_url_encode(enc_creator, sizeof(enc_creator), creator ? creator : "");
    snprintf(f->path, sizeof(f->path), "%s/api/map-data?name=%s%s%s&meta=1", prefix ? prefix : "", encoded, enc_creator[0] ? "&creator=" : "", enc_creator);

    server_log("[brain] fetching published map '%s' from %s", name, g_http_base);
    pthread_t thread;
    if (pthread_create(&thread, NULL, brain_fetch_thread, f)) {
        free(f);
        return -1;
    }
    pthread_detach(thread); // f is the thread's from here on
    return 0;
}

// The brain's HTTP base for a config, without a running link (published maps load before
// accounts_init starts the brain): gameserver.brain_http_url, else derived from brain_url.
static int brain_http_base_of(const GsConfig *cfg, char *out, const size_t size) {
    if (cfg && cfg->brain_http_url[0]) {
        snprintf(out, size, "%s", cfg->brain_http_url);
        return 1;
    }
    const char *url = cfg ? cfg->brain_url : "";
    const int tls = !strncmp(url, "wss://", 6);
    if (!tls && strncmp(url, "ws://", 5)) return 0;
    const char *rest = url + (tls ? 6 : 5);
    snprintf(out, size, "%s://%.*s", tls ? "https" : "http", (int) strcspn(rest, "/"), rest);
    return 1;
}

cJSON *brain_fetch_map_sync(const GsConfig *cfg, const char *name, const char *creator, const int timeout_ms, int *missing,
                            char *err, const size_t err_size) {
    return brain_fetch_map_sync_id(cfg, name, creator, timeout_ms, missing, NULL, err, err_size);
}

cJSON *brain_fetch_map_sync_id(const GsConfig *cfg, const char *name, const char *creator, const int timeout_ms, int *missing,
                               int *map_id, char *err, const size_t err_size) {
    if (missing) *missing = 0;
    if (map_id) *map_id = 0;
    char base[GS_STR_LARGE];
    if (!brain_http_base_of(cfg, base, sizeof(base))) {
        snprintf(err, err_size, "no brain configured (gameserver.brain_url or brain_http_url)");
        return NULL;
    }
    char enc_name[256], enc_creator[192], url[GS_STR_LARGE + 512];
    brain_url_encode(enc_name, sizeof(enc_name), name);
    brain_url_encode(enc_creator, sizeof(enc_creator), creator ? creator : "");
    snprintf(url, sizeof(url), "%s/api/map-data?name=%s%s%s%s", base, enc_name, enc_creator[0] ? "&creator=" : "", enc_creator,
             map_id ? "&meta=1" : "");
    server_log("[maps] fetching published map '%s' from %s", name, base);
    HttpResult res;
    cJSON *map = NULL;
    if (http_request("GET", url, NULL, NULL, timeout_ms > 0 ? timeout_ms : 10000, BRAIN_MAP_FETCH_MAX, &res) != 0) {
        snprintf(err, err_size, "%s", res.error[0] ? res.error : "cannot reach the brain");
    } else if (res.status == 404) {
        if (missing) *missing = 1;
        snprintf(err, err_size, "map not found");
    } else if (res.status != 200) {
        snprintf(err, err_size, "map service error (HTTP %d)", res.status);
    } else {
        map = res.body ? cJSON_Parse(res.body) : NULL;
        if (map_id) map = brain_map_unwrap(map, map_id); // meta=1: {id, map}
        if (!cJSON_IsObject(map)) {
            cJSON_Delete(map);
            map = NULL;
            snprintf(err, err_size, "not a Krunker map");
        }
    }
    http_result_free(&res);
    return map;
}

cJSON *brain_verify_ticket(const char *ticket, char *err, const size_t err_size) {
    if (!ticket || !ticket[0]) {
        snprintf(err, err_size, "no ticket");
        return NULL;
    }
    return kn_signed_token_verify(g_brain.join_secret, ticket, 0, err, err_size);
}

cJSON *brain_verify_ticket_any_age(const char *ticket, char *err, const size_t err_size) {
    if (!ticket || !ticket[0]) {
        snprintf(err, err_size, "no ticket");
        return NULL;
    }
    return kn_signed_token_verify(g_brain.join_secret, ticket, 1, err, err_size); // "now" = 1 s: any exp passes
}

void brain_random_hex(char *out, const size_t size, const size_t bytes) {
    if (kn_random_hex(out, size, bytes)) snprintf(out, size, "%08lx", (unsigned long) time(NULL));
}

#else // ---------------------------------------------------------------- no knsvc

void brain_set_http_base(const GsConfig *cfg) { (void) cfg; }
int brain_fetch_map(const uint32_t rid, const char *name, const char *creator) { (void) rid; (void) name; (void) creator; return -1; }
cJSON *brain_fetch_map_sync(const GsConfig *cfg, const char *name, const char *creator, const int timeout_ms, int *missing,
                            char *err, const size_t err_size) {
    (void) cfg; (void) name; (void) creator; (void) timeout_ms;
    if (missing) *missing = 0;
    snprintf(err, err_size, "no brain link in this build");
    return NULL;
}
cJSON *brain_fetch_map_sync_id(const GsConfig *cfg, const char *name, const char *creator, const int timeout_ms, int *missing,
                               int *map_id, char *err, const size_t err_size) {
    if (map_id) *map_id = 0;
    return brain_fetch_map_sync(cfg, name, creator, timeout_ms, missing, err, err_size);
}

int brain_start(const GsConfig *cfg) {
    if (cfg->brain_url[0]) server_log_error("[brain] gameserver.brain_url is set but this server_bin was built without knsvc (OpenSSL); running standalone");
    return 0;
}
void brain_set_hello_and_run(cJSON *hello) { cJSON_Delete(hello); }
int brain_enabled(void) { return 0; }
int brain_linked(void) { return 0; }
int brain_post(cJSON *frame, const int reliable) { (void) reliable; cJSON_Delete(frame); return -1; }
uint32_t brain_next_rid(void) { return 0; }
int brain_next_event(BrainEvent *out) { (void) out; return 0; }
void brain_stop(const int wait_ms) { (void) wait_ms; }
cJSON *brain_verify_ticket(const char *ticket, char *err, const size_t err_size) {
    (void) ticket;
    snprintf(err, err_size, "tickets need knsvc");
    return NULL;
}
cJSON *brain_verify_ticket_any_age(const char *ticket, char *err, const size_t err_size) {
    return brain_verify_ticket(ticket, err, err_size);
}
void brain_random_hex(char *out, const size_t size, const size_t bytes) {
    (void) bytes;
    static unsigned int counter;
    snprintf(out, size, "%08lx%04x", (unsigned long) time(NULL), ++counter & 0xffff);
}

#endif
