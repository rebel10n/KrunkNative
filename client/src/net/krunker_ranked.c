// Krunker v10.0.2's ranked services at krunker.io (client/include/krunker_ranked.h): the authenticated
// requests (api / gapi, Bearer) and the queue socket (gamefrontend /v1/matchmaking/queue).
//
// Requests go over http_stream.h (headers on every platform, the client proxy natively). The queue
// socket is a WebSocket of JSON text frames: a reader thread on native (http_conn.h, like the brain's
// social socket), the browser's WebSocket on the web. Either way the frames reach the caller on the
// main thread from krunker_ranked_tick, and the socket is referenced-counted: the main thread, the
// token request and the socket each hold the queue until they are done with it, so a stop can never
// free it under a callback.
#include <krunker_ranked.h>
#include "klog.h"
#include <krunker_account.h>
#include <http_stream.h>
#include <backend.h> // backend_url_encode
#include <cJSON.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
#define KRN_RANKED_WS_NATIVE 1
#include <http_conn.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#ifdef __EMSCRIPTEN__
#include <stdbool.h>
#include <emscripten.h>
#include <emscripten/websocket.h>
#endif

#define RANKED_BODY_MAX (1024u * 1024u)  // a request's answer
#define RANKED_TOKEN_MAX 4096            // krunker_account's TOKEN_MAX
#define RANKED_FRAME_MAX (1024u * 1024u) // one WebSocket message
#define QUEUE_IO_MS 5000                 // the socket's connect / send / receive bound: a timeout re-checks the stop
// v10's preferredMaps (v10:98372), joined with commas as v10 does (not encoded)
#define QUEUE_PREFERRED_MAPS "0,2,4,11,14,17,42"

static void wipe(void *p, size_t n) {
    volatile unsigned char *v = p;
    while (n--) *v++ = 0;
}

// ---------------------------------------------------------------- requests

typedef struct {
    KrunkerRankedFn fn;
    void *user;
    char method[8];
    char url[512];   // host base + path (no token: the token is a header)
    char *body;      // the JSON body (a copy), NULL none
    HttpStream *stream;
    char *buf;       // the answer so far
    size_t len;
    int sync;        // inside krunker_ranked_request: a refusal is reported to it, not to fn
    int refused;
} Rq;

static void rq_free(Rq *r) {
    free(r->body);
    free(r->buf);
    free(r);
}

// The request could not be sent: the caller hears it now (sync) or fn runs with status 0 (later).
static void rq_refuse(Rq *r) {
    if (r->sync) {
        r->refused = 1;
        return;
    }
    KrunkerRankedFn fn = r->fn;
    void *user = r->user;
    rq_free(r);
    if (fn) fn(user, 0, NULL);
}

static void rq_open(void *user, const int status, const char *content_type) {
    (void) user;
    (void) status;
    (void) content_type;
}

static void rq_data(void *user, const char *data, const size_t len) {
    Rq *r = user;
    if (r->len + len > RANKED_BODY_MAX) return;
    char *grown = realloc(r->buf, r->len + len + 1);
    if (!grown) return;
    r->buf = grown;
    memcpy(r->buf + r->len, data, len);
    r->len += len;
    r->buf[r->len] = 0;
}

static void rq_done(void *user, const int status, const char *error) {
    Rq *r = user;
    (void) error;
    r->stream = NULL;
    cJSON *body = status && r->buf ? cJSON_Parse(r->buf) : NULL;
    KN_LOG_DEBUG1("[krunker-ranked] %s %s -> %s\n", r->method, r->url, status ? "answered" : "no answer");
    KrunkerRankedFn fn = r->fn;
    void *user_fn = r->user;
    rq_free(r);
    if (fn) fn(user_fn, status, body);
    cJSON_Delete(body);
}

// A fresh X-Idempotency-Key (crypto.randomUUID's shape), as v10's API client sends on every POST.
static void idempotency_key(char *out, const size_t n) {
    static const char hex[] = "0123456789abcdef";
    const char *shape = "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx";
    size_t i = 0;
    for (; shape[i] && i + 1 < n; i++) {
        const int r = rand() & 15;
        out[i] = shape[i] == 'x' ? hex[r] : shape[i] == 'y' ? hex[(r & 3) | 8] : shape[i];
    }
    out[i] = 0;
}

static void rq_token(void *user, const char *token) {
    Rq *r = user;
    if (!token || !token[0] || strlen(token) > RANKED_TOKEN_MAX) {
        rq_refuse(r);
        return;
    }
    char headers[RANKED_TOKEN_MAX + 320];
    int n = snprintf(headers, sizeof(headers), "Accept: application/json\r\nAuthorization: Bearer %s\r\n", token);
    if (n > 0 && (size_t) n < sizeof(headers) && !strcmp(r->method, "POST")) {
        char key[40];
        idempotency_key(key, sizeof(key));
        const int m = snprintf(headers + n, sizeof(headers) - (size_t) n, "X-Idempotency-Key: %s\r\n", key);
        n = m > 0 && (size_t) (n + m) < sizeof(headers) ? n + m : -1;
    }
    static const HttpStreamCallbacks cb = {rq_open, rq_data, rq_done};
    r->stream = n > 0 && (size_t) n < sizeof(headers) ? http_stream_start(r->method, r->url, headers, r->body, &cb, r) : NULL;
    wipe(headers, sizeof(headers)); // carries the access token
    if (!r->stream) rq_refuse(r);
}

int krunker_ranked_request(const KrunkerRankedHost host, const char *method, const char *path, const char *json_body,
                           const KrunkerRankedFn fn, void *user) {
    if (!fn || !path || !krunker_account_signed_in()) return 0;
    Rq *r = calloc(1, sizeof(*r));
    if (!r) return 0;
    r->fn = fn;
    r->user = user;
    snprintf(r->method, sizeof(r->method), "%s", method && method[0] ? method : "GET");
    krunker_host_url(host == KRUNKER_RANKED_GAPI ? KRUNKER_HOST_GAPI : KRUNKER_HOST_API, path, r->url, sizeof(r->url));
    if (json_body) {
        r->body = strdup(json_body);
        if (!r->body) {
            rq_free(r);
            return 0;
        }
    }
    r->sync = 1;
    krunker_account_fresh_token(rq_token, r); // Bearer: the fresh access token (refreshed first when due)
    r->sync = 0;
    if (r->refused) {
        rq_free(r);
        return 0;
    }
    return 1;
}

// ---------------------------------------------------------------- the queue socket

typedef struct QMsg {
    struct QMsg *next;
    char *text;
} QMsg;

typedef struct Queue {
    KrunkerQueueFrameFn on_frame;
    KrunkerQueueDoneFn on_done;
    void *user;
    char regions[64];
    char *url;          // the socket's URL with the token (set once the token came)
    QMsg *head, *tail;  // frames for the main thread
    int users;          // holders: the main thread (g_queue), the token request, the socket
    int finished;       // the socket ended (or never started): the main thread delivers on_done
    int failed;         // it never opened
    int opened;         // web: onopen (the native reader sets it locally)
    int main_released;  // stop() or the done delivery let go of the main thread's hold
    int sync;           // inside krunker_ranked_queue_start
    int refused;
    int fd;             // native: the socket's descriptor while it is open (stop shuts it down), else -1
#ifdef KRN_RANKED_WS_NATIVE
    pthread_mutex_t lock;
#endif
#ifdef __EMSCRIPTEN__
    EMSCRIPTEN_WEBSOCKET_T ws; // the browser socket (0 none)
#endif
} Queue;

static Queue *g_queue;

#ifdef KRN_RANKED_WS_NATIVE
#define Q_LOCK(q) pthread_mutex_lock(&(q)->lock)
#define Q_UNLOCK(q) pthread_mutex_unlock(&(q)->lock)
#else
#define Q_LOCK(q) ((void) (q))
#define Q_UNLOCK(q) ((void) (q))
#endif

static void q_destroy(Queue *q) {
    for (QMsg *m = q->head; m;) {
        QMsg *next = m->next;
        free(m->text);
        free(m);
        m = next;
    }
    free(q->url);
#ifdef KRN_RANKED_WS_NATIVE
    pthread_mutex_destroy(&q->lock);
#endif
    free(q);
}

// Lets go of one hold; the last one frees the queue.
static void q_drop(Queue *q) {
    Q_LOCK(q);
    const int last = --q->users == 0;
    Q_UNLOCK(q);
    if (last) q_destroy(q);
}

static int q_stopped(Queue *q) {
    Q_LOCK(q);
    const int stopped = q->main_released;
    Q_UNLOCK(q);
    return stopped;
}

#if defined(KRN_RANKED_WS_NATIVE) || defined(__EMSCRIPTEN__)
// The socket's side: a text frame for the main thread (dropped once the main thread let go).
static void q_push(Queue *q, char *text) {
    QMsg *m = malloc(sizeof(*m));
    Q_LOCK(q);
    if (!m || q->main_released) {
        Q_UNLOCK(q);
        free(m);
        free(text);
        return;
    }
    m->text = text;
    m->next = NULL;
    if (q->tail) q->tail->next = m;
    else q->head = m;
    q->tail = m;
    Q_UNLOCK(q);
}

// The socket has ended: the main thread delivers on_done, then drops its hold.
static void q_socket_end(Queue *q, const int opened) {
    Q_LOCK(q);
    q->finished = 1;
    q->failed = !opened;
    Q_UNLOCK(q);
    q_drop(q);
}
#endif

// wss:// / ws:// URL of v10's gamefrontend (http -> ws, as v10's replace("http", "ws")).
static void queue_url(const char *token, const char *regions, char *out, const size_t n) {
    char base[320], enc[RANKED_TOKEN_MAX * 3 + 8];
    krunker_host_url(KRUNKER_HOST_GAMEFRONTEND, "/v1/matchmaking/queue", base, sizeof(base));
    const char *rest = base;
    const char *scheme = "wss://";
    if (!strncmp(base, "https://", 8)) rest = base + 8;
    else if (!strncmp(base, "http://", 7)) {
        scheme = "ws://";
        rest = base + 7;
    } else if (!strncmp(base, "wss://", 6)) rest = base + 6;
    else if (!strncmp(base, "ws://", 5)) {
        scheme = "ws://";
        rest = base + 5;
    }
    backend_url_encode(enc, sizeof(enc), token);
    snprintf(out, n, "%s%s?token=%s&maps=%s&regions=%s", scheme, rest, enc, QUEUE_PREFERRED_MAPS, regions);
    wipe(enc, sizeof(enc));
}

#ifdef KRN_RANKED_WS_NATIVE

// ---- the native socket (RFC 6455, text frames)

static int recv_exact(Queue *q, HttpConn *c, void *out, size_t len) {
    uint8_t *p = out;
    while (len) {
        if (q_stopped(q)) return 0;
        const ssize_t r = http_conn_recv(c, p, len);
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) continue; // QUEUE_IO_MS: re-check the stop
        if (r <= 0) return 0;
        p += r;
        len -= (size_t) r;
    }
    return 1;
}

// A masked client frame (RFC 6455 5.3): text, pong.
static int ws_send(HttpConn *c, const int opcode, const uint8_t *data, const size_t len) {
    uint8_t head[14];
    size_t n = 0;
    head[n++] = (uint8_t) (0x80 | opcode);
    if (len < 126) {
        head[n++] = (uint8_t) (0x80 | len);
    } else if (len < 65536) {
        head[n++] = 0x80 | 126;
        head[n++] = (uint8_t) (len >> 8);
        head[n++] = (uint8_t) len;
    } else {
        head[n++] = 0x80 | 127;
        for (int i = 7; i >= 0; i--) head[n++] = (uint8_t) ((uint64_t) len >> (i * 8));
    }
    uint8_t mask[4];
    for (int i = 0; i < 4; i++) mask[i] = (uint8_t) (rand() & 0xff);
    memcpy(head + n, mask, 4);
    n += 4;
    uint8_t *masked = malloc(len ? len : 1);
    if (!masked) return 0;
    for (size_t i = 0; i < len; i++) masked[i] = data[i] ^ mask[i & 3];
    const int ok = http_conn_send(c, head, n) && (!len || http_conn_send(c, masked, len));
    free(masked);
    return ok;
}

// The upgrade. 1 = 101 Switching Protocols.
static int ws_handshake(HttpConn *c, const HttpAddr *a, const char *target) {
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    uint8_t raw[16];
    char key[25];
    for (int i = 0; i < 16; i++) raw[i] = (uint8_t) (rand() & 0xff);
    int k = 0;
    for (int i = 0; i < 16; i += 3) {
        const unsigned v = (unsigned) raw[i] << 16 | (i + 1 < 16 ? (unsigned) raw[i + 1] << 8 : 0) | (i + 2 < 16 ? raw[i + 2] : 0);
        key[k++] = b64[(v >> 18) & 63];
        key[k++] = b64[(v >> 12) & 63];
        key[k++] = i + 1 < 16 ? b64[(v >> 6) & 63] : '=';
        key[k++] = i + 2 < 16 ? b64[v & 63] : '=';
    }
    key[24] = 0;
    const int default_port = !strcmp(a->port, a->tls ? "443" : "80");
    char req[2048 + RANKED_TOKEN_MAX];
    const int len = snprintf(req, sizeof(req),
                             "GET %s HTTP/1.1\r\nHost: %s%s%s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                             "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\nOrigin: https://krunker.io\r\n"
                             "User-Agent: KrunkNative\r\n\r\n",
                             target, a->host, default_port ? "" : ":", default_port ? "" : a->port, key);
    const int sent = len > 0 && (size_t) len < sizeof(req) && http_conn_send(c, req, (size_t) len);
    wipe(req, sizeof(req)); // carries the token
    if (!sent) return 0;
    char resp[4096];
    size_t n = 0;
    while (n + 1 < sizeof(resp)) {
        if (http_conn_recv(c, resp + n, 1) != 1) return 0;
        n++;
        if (n >= 4 && !memcmp(resp + n - 4, "\r\n\r\n", 4)) break;
    }
    resp[n] = 0;
    int status = 0;
    if (sscanf(resp, "HTTP/%*s %d", &status) != 1 || status != 101) {
        printf("[krunker-ranked] queue socket refused the upgrade (HTTP %d)\n", status);
        return 0;
    }
    return 1;
}

// Reads frames until the server closes, a stop, or an error. Text frames (fragments joined) go to the queue.
static void ws_read(Queue *q, HttpConn *c) {
    uint8_t *frag = NULL;
    size_t frag_len = 0;
    for (;;) {
        uint8_t h[2];
        if (!recv_exact(q, c, h, 2)) break;
        const int fin = h[0] & 0x80, op = h[0] & 0x0f, masked = h[1] & 0x80;
        uint64_t len = h[1] & 0x7f;
        if (len == 126) {
            uint8_t e[2];
            if (!recv_exact(q, c, e, 2)) break;
            len = (uint64_t) e[0] << 8 | e[1];
        } else if (len == 127) {
            uint8_t e[8];
            if (!recv_exact(q, c, e, 8)) break;
            len = 0;
            for (int i = 0; i < 8; i++) len = len << 8 | e[i];
        }
        if (len > RANKED_FRAME_MAX) break;
        uint8_t mask[4] = {0, 0, 0, 0};
        if (masked && !recv_exact(q, c, mask, 4)) break;
        uint8_t *payload = malloc(len + 1);
        if (!payload) break;
        if (len && !recv_exact(q, c, payload, (size_t) len)) {
            free(payload);
            break;
        }
        payload[len] = 0;
        if (masked)
            for (uint64_t i = 0; i < len; i++) payload[i] ^= mask[i & 3];
        if (op == 0x8) { // close
            free(payload);
            break;
        }
        if (op == 0x9) { // ping: pong with the same payload
            ws_send(c, 0xA, payload, (size_t) len);
            free(payload);
            continue;
        }
        if (op != 0x1 && op != 0x0) { // pong, binary: not used by the queue
            free(payload);
            continue;
        }
        if (op == 0x1) { // a new message: a fragment restarts it
            free(frag);
            frag = NULL;
            frag_len = 0;
        }
        if (!fin) { // a fragment of a longer message: keep it
            uint8_t *grown = realloc(frag, frag_len + (size_t) len);
            if (!grown) {
                free(payload);
                break;
            }
            memcpy(grown + frag_len, payload, (size_t) len);
            frag = grown;
            frag_len += (size_t) len;
            free(payload);
            continue;
        }
        if (frag) { // the message's last fragment
            uint8_t *grown = realloc(frag, frag_len + (size_t) len + 1);
            if (!grown) {
                free(payload);
                break;
            }
            memcpy(grown + frag_len, payload, (size_t) len);
            grown[frag_len + (size_t) len] = 0;
            free(payload);
            q_push(q, (char *) grown);
            frag = NULL;
            frag_len = 0;
        } else {
            q_push(q, (char *) payload);
        }
    }
    free(frag);
}

static void *queue_thread(void *arg) {
    Queue *q = arg;
    HttpAddr a;
    HttpConn c;
    memset(&c, 0, sizeof(c));
    c.fd = -1;
    int opened = 0;
    if (!q_stopped(q) && http_url_parse(q->url, &a) && http_conn_open(&c, &a, QUEUE_IO_MS)) {
        Q_LOCK(q);
        q->fd = c.fd;
        Q_UNLOCK(q);
        if (!q_stopped(q) && ws_handshake(&c, &a, http_url_target(q->url))) {
            opened = 1;
            KN_LOG_DEBUG1("[krunker-ranked] queue socket open\n");
            ws_read(q, &c);
        }
        Q_LOCK(q);
        q->fd = -1;
        Q_UNLOCK(q);
        http_conn_close(&c);
    }
    if (!opened) printf("[krunker-ranked] queue socket could not open\n");
    q_socket_end(q, opened);
    return NULL;
}

// 1 = the socket's thread runs (it holds the queue until it ends).
static int q_open_socket(Queue *q, const char *token) {
    char *url = malloc(RANKED_TOKEN_MAX * 3 + 512);
    if (!url) return 0;
    queue_url(token, q->regions, url, RANKED_TOKEN_MAX * 3 + 512);
    free(q->url);
    q->url = strdup(url);
    wipe(url, RANKED_TOKEN_MAX * 3 + 512); // carries the token
    free(url);
    if (!q->url) return 0;
    pthread_attr_t attr;
    pthread_t thread;
    if (pthread_attr_init(&attr) != 0) return 0;
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    Q_LOCK(q);
    q->users++; // the socket's thread
    Q_UNLOCK(q);
    const int ok = pthread_create(&thread, &attr, queue_thread, q) == 0;
    pthread_attr_destroy(&attr);
    if (!ok) q_drop(q);
    return ok;
}

// stop: the socket is shut down; its thread ends and drops its hold.
static void q_close_socket(Queue *q) {
    Q_LOCK(q);
    if (q->fd >= 0) shutdown(q->fd, SHUT_RDWR);
    Q_UNLOCK(q);
}

#elif defined(__EMSCRIPTEN__)

// ---- the browser's socket (the callbacks run on the main thread)

static bool ws_on_open(int type, const EmscriptenWebSocketOpenEvent *e, void *user) {
    (void) type;
    Queue *q = user;
    if (e->socket == q->ws) q->opened = 1;
    return true;
}

static bool ws_on_message(int type, const EmscriptenWebSocketMessageEvent *e, void *user) {
    (void) type;
    Queue *q = user;
    if (e->socket != q->ws || !e->isText || e->numBytes > RANKED_FRAME_MAX) return true;
    char *text = malloc(e->numBytes + 1);
    if (!text) return true;
    memcpy(text, e->data, e->numBytes);
    text[e->numBytes] = 0;
    q_push(q, text);
    return true;
}

static bool ws_on_error(int type, const EmscriptenWebSocketErrorEvent *e, void *user) {
    (void) type;
    (void) e;
    (void) user;
    return true; // the close callback follows
}

static bool ws_on_close(int type, const EmscriptenWebSocketCloseEvent *e, void *user) {
    (void) type;
    Queue *q = user;
    emscripten_websocket_delete(e->socket);
    if (e->socket == q->ws) q->ws = 0;
    const int opened = q->opened;
    q_socket_end(q, opened); // (q may be freed here: the socket's hold was its last)
    return true;
}

static int q_open_socket(Queue *q, const char *token) {
    char *url = malloc(RANKED_TOKEN_MAX * 3 + 512);
    if (!url) return 0;
    queue_url(token, q->regions, url, RANKED_TOKEN_MAX * 3 + 512);
    free(q->url);
    q->url = strdup(url);
    wipe(url, RANKED_TOKEN_MAX * 3 + 512);
    free(url);
    if (!q->url) return 0;
    EmscriptenWebSocketCreateAttributes attr;
    emscripten_websocket_init_create_attributes(&attr);
    attr.url = q->url;
    attr.createOnMainThread = true;
    const EMSCRIPTEN_WEBSOCKET_T s = emscripten_websocket_new(&attr);
    if (s <= 0) return 0;
    q->ws = s;
    q->users++; // the socket
    emscripten_websocket_set_onopen_callback(s, q, ws_on_open);
    emscripten_websocket_set_onmessage_callback(s, q, ws_on_message);
    emscripten_websocket_set_onerror_callback(s, q, ws_on_error);
    emscripten_websocket_set_onclose_callback(s, q, ws_on_close);
    return 1;
}

static void q_close_socket(Queue *q) {
    if (q->ws <= 0) return;
    const EMSCRIPTEN_WEBSOCKET_T s = q->ws;
    q->ws = 0;
    emscripten_websocket_close(s, 1000, "stop");
}

#else

// Windows: no queue socket yet (the requests above work; the Ranked popup says the queue failed)
static int q_open_socket(Queue *q, const char *token) {
    (void) q;
    (void) token;
    return 0;
}

static void q_close_socket(Queue *q) { (void) q; }

#endif

// The token came (or not): open the socket for this queue (not when it was stopped meanwhile).
static void queue_token(void *user, const char *token) {
    Queue *q = user;
    const int started = !q_stopped(q) && token && token[0] && strlen(token) <= RANKED_TOKEN_MAX && q_open_socket(q, token);
    if (!started) {
        if (q->sync) {
            q->refused = 1; // krunker_ranked_queue_start frees it
            return;
        }
        Q_LOCK(q);
        q->failed = 1;
        q->finished = 1; // no socket: on_done(failed) on the next tick
        Q_UNLOCK(q);
    }
    q_drop(q); // the token request's hold
}

int krunker_ranked_queue_start(const char *regions, const KrunkerQueueFrameFn on_frame, const KrunkerQueueDoneFn on_done, void *user) {
    if (g_queue || !regions || !regions[0] || !krunker_account_signed_in()) return 0;
    Queue *q = calloc(1, sizeof(*q));
    if (!q) return 0;
    q->on_frame = on_frame;
    q->on_done = on_done;
    q->user = user;
    snprintf(q->regions, sizeof(q->regions), "%s", regions);
    q->users = 2; // the main thread, the token request
    q->fd = -1;
#ifdef KRN_RANKED_WS_NATIVE
    pthread_mutex_init(&q->lock, NULL);
#endif
#ifdef __EMSCRIPTEN__
    q->ws = 0;
#endif
    g_queue = q;
    q->sync = 1;
    krunker_account_fresh_token(queue_token, q);
    q->sync = 0;
    if (q->refused) {
        g_queue = NULL;
        q_destroy(q);
        return 0;
    }
    return 1;
}

void krunker_ranked_queue_stop(void) {
    Queue *q = g_queue;
    if (!q) return;
    g_queue = NULL;
    Q_LOCK(q);
    q->main_released = 1;
    Q_UNLOCK(q);
    q_close_socket(q);
    q_drop(q); // the main thread's hold
}

int krunker_ranked_queue_active(void) { return g_queue != NULL; }

void krunker_ranked_tick(void) {
    for (;;) {
        Queue *q = g_queue;
        if (!q) return;
        char *text = NULL;
        int done = 0, failed = 0;
        Q_LOCK(q);
        if (q->head) {
            QMsg *m = q->head;
            q->head = m->next;
            if (!q->head) q->tail = NULL;
            text = m->text;
            free(m);
        } else if (q->finished) {
            done = 1;
            failed = q->failed;
            q->main_released = 1;
        }
        Q_UNLOCK(q);
        if (text) {
            cJSON *frame = cJSON_Parse(text);
            free(text);
            if (q->on_frame) q->on_frame(q->user, frame); // a frame that does not parse is passed as NULL (v10's catch)
            cJSON_Delete(frame);
            continue; // (a callback may have stopped the queue: g_queue says)
        }
        if (done) {
            const KrunkerQueueDoneFn fn = q->on_done;
            void *user = q->user;
            g_queue = NULL;
            q_drop(q);
            if (fn) fn(user, failed);
        }
        return;
    }
}
