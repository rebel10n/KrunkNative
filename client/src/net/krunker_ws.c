// A WebSocket to a krunker.io service (client/include/krunker_ws.h): natively a thread over http_conn
// (through the client's proxy, the game socket's handshake: krunker_net.c), on the web the browser's
// WebSocket. Events are queued and handed to the main thread by krunker_ws_poll.
#include <krunker_ws.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten/websocket.h>
#elif !defined(_WIN32)
#include <http_conn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#define WS_NATIVE 1
#endif

#define WS_TIMEOUT_MS 8000
#define WS_MAX_MESSAGE (4u * 1024u * 1024u) // a hub page's market or map list fits; a larger message drops the socket
#define WS_URL_MAX 6144                     // the base + /ws? + an encoded JWT and a pre-auth token
#define WS_SLOTS 4

typedef struct WsMsg {
    struct WsMsg *next;
    int event, code, binary;
    size_t len;
    unsigned char data[];
} WsMsg;

struct KrunkerWs {
    char tag[24];
    pthread_mutex_t lock;
    WsMsg *in_head, *in_tail;   // socket -> main thread
    WsMsg *out_head, *out_tail; // main thread -> socket (native)
    volatile int open;
    unsigned generation;
#ifdef __EMSCRIPTEN__
    EMSCRIPTEN_WEBSOCKET_T ws;
#elif defined(WS_NATIVE)
    pthread_t thread;
    int thread_running;
    volatile int quit;
    int wake[2]; // wakes the thread for a send / the close
    char url[WS_URL_MAX];
#endif
};

static KrunkerWs g_slots[WS_SLOTS];
static int g_used;

static void wipe(void *p, size_t n) {
    volatile unsigned char *v = p;
    while (n--) *v++ = 0;
}

static void lock(KrunkerWs *w) { pthread_mutex_lock(&w->lock); }
static void unlock(KrunkerWs *w) { pthread_mutex_unlock(&w->lock); }

static WsMsg *msg_new(const int event, const void *data, const size_t len, const int binary, const int code) {
    WsMsg *m = malloc(sizeof(WsMsg) + len + 1);
    if (!m) return NULL;
    m->next = NULL;
    m->event = event;
    m->code = code;
    m->binary = binary;
    m->len = len;
    if (len) memcpy(m->data, data, len);
    m->data[len] = 0; // a text message reads as a C string
    return m;
}

static void push(WsMsg **head, WsMsg **tail, WsMsg *m) {
    if (!m) return;
    if (*tail) (*tail)->next = m;
    else *head = m;
    *tail = m;
}

static void free_list(WsMsg **head, WsMsg **tail) {
    while (*head) {
        WsMsg *next = (*head)->next;
        wipe((*head)->data, (*head)->len); // the hub's frames carry tokens (patR)
        free(*head);
        *head = next;
    }
    *tail = NULL;
}

// (the generation check is the caller's: under the lock; the Windows build has no socket to queue from)
__attribute__((unused)) static void queue_in(KrunkerWs *w, const int event, const void *data, const size_t len, const int binary, const int code) {
    push(&w->in_head, &w->in_tail, msg_new(event, data, len, binary, code));
}

KrunkerWs *krunker_ws_new(const char *tag) {
    if (g_used >= WS_SLOTS) return NULL;
    KrunkerWs *w = &g_slots[g_used++];
    memset(w, 0, sizeof(*w));
    snprintf(w->tag, sizeof(w->tag), "%s", tag ? tag : "krunker-ws");
    pthread_mutex_init(&w->lock, NULL);
#ifdef WS_NATIVE
    w->wake[0] = w->wake[1] = -1;
#endif
    return w;
}

void krunker_ws_poll(KrunkerWs *w, const KrunkerWsFn fn, void *user) {
    if (!w) return;
    lock(w);
    WsMsg *m = w->in_head;
    w->in_head = w->in_tail = NULL;
    unlock(w);
    while (m) {
        WsMsg *next = m->next;
        if (fn) fn(user, m->event, m->data, m->len, m->binary, m->code);
        wipe(m->data, m->len);
        free(m);
        m = next;
    }
}

int krunker_ws_is_open(const KrunkerWs *w) { return w && w->open; }

// ---------------------------------------------------------------- web: the browser's WebSocket

#ifdef __EMSCRIPTEN__
// the callbacks' user is the slot; the generation in it tells a stale socket's events apart
static bool on_open(int type, const EmscriptenWebSocketOpenEvent *e, void *user) {
    (void) type;
    KrunkerWs *w = user;
    if (e->socket != w->ws) return true;
    w->open = 1;
    lock(w);
    queue_in(w, KRUNKER_WS_OPEN, NULL, 0, 0, 0);
    unlock(w);
    return true;
}

static bool on_message(int type, const EmscriptenWebSocketMessageEvent *e, void *user) {
    (void) type;
    KrunkerWs *w = user;
    if (e->socket != w->ws) return true;
    // a text message's numBytes counts emscripten's terminating NUL
    const size_t n = e->isText && e->numBytes && e->data[e->numBytes - 1] == 0 ? e->numBytes - 1 : e->numBytes;
    lock(w);
    queue_in(w, KRUNKER_WS_MESSAGE, e->data, n, !e->isText, 0);
    unlock(w);
    return true;
}

static bool on_error(int type, const EmscriptenWebSocketErrorEvent *e, void *user) {
    (void) type;
    (void) e;
    (void) user;
    return true; // onclose follows and reports
}

static bool on_close(int type, const EmscriptenWebSocketCloseEvent *e, void *user) {
    (void) type;
    KrunkerWs *w = user;
    emscripten_websocket_delete(e->socket);
    if (e->socket != w->ws) return true;
    w->ws = 0;
    w->open = 0;
    lock(w);
    queue_in(w, KRUNKER_WS_CLOSED, NULL, 0, 0, e->code);
    unlock(w);
    return true;
}

int krunker_ws_open(KrunkerWs *w, const char *url) {
    if (!w) return 0;
    krunker_ws_close(w);
    if (!url || strncmp(url, "ws", 2)) return 0;
    w->generation++;
    EmscriptenWebSocketCreateAttributes attr;
    emscripten_websocket_init_create_attributes(&attr);
    attr.url = url;
    attr.createOnMainThread = true;
    w->ws = emscripten_websocket_new(&attr);
    if (w->ws <= 0) {
        w->ws = 0;
        return 0;
    }
    emscripten_websocket_set_onopen_callback(w->ws, w, on_open);
    emscripten_websocket_set_onmessage_callback(w->ws, w, on_message);
    emscripten_websocket_set_onerror_callback(w->ws, w, on_error);
    emscripten_websocket_set_onclose_callback(w->ws, w, on_close);
    return 1;
}

int krunker_ws_send(KrunkerWs *w, const void *data, const size_t len, const int binary) {
    if (!w || !w->open || w->ws <= 0 || !data) return 0;
    if (binary) return emscripten_websocket_send_binary(w->ws, (void *) data, (uint32_t) len) == EMSCRIPTEN_RESULT_SUCCESS;
    char *text = malloc(len + 1);
    if (!text) return 0;
    memcpy(text, data, len);
    text[len] = 0;
    const int ok = emscripten_websocket_send_utf8_text(w->ws, text) == EMSCRIPTEN_RESULT_SUCCESS;
    free(text);
    return ok;
}

void krunker_ws_close(KrunkerWs *w) {
    if (!w) return;
    w->generation++;
    w->open = 0;
    if (w->ws > 0) {
        const EMSCRIPTEN_WEBSOCKET_T old = w->ws;
        w->ws = 0;
        emscripten_websocket_close(old, 1000, "client disconnect");
    }
    lock(w);
    free_list(&w->in_head, &w->in_tail);
    unlock(w);
}

// ---------------------------------------------------------------- native: a thread over http_conn

#elif defined(WS_NATIVE)
static int send_frame(HttpConn *c, const int opcode, const void *data, const size_t len) {
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
    for (size_t i = 0; i < len; i++) masked[i] = ((const uint8_t *) data)[i] ^ mask[i & 3];
    const int ok = http_conn_send(c, head, n) && (!len || http_conn_send(c, masked, len));
    free(masked);
    return ok;
}

static int recv_exact(KrunkerWs *w, HttpConn *c, void *out, size_t len) {
    uint8_t *p = out;
    while (len) {
        if (w->quit) return 0;
        const ssize_t r = http_conn_recv(c, p, len);
        if (r < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (r <= 0) return 0;
        p += r;
        len -= (size_t) r;
    }
    return 1;
}

// ws(s)://host[:port]/path?query -> the address and the request target
static int url_split(const char *url, HttpAddr *a, char *target, const size_t n) {
    memset(a, 0, sizeof(*a));
    const char *p;
    if (!strncmp(url, "wss://", 6)) {
        a->tls = 1;
        p = url + 6;
    } else if (!strncmp(url, "ws://", 5)) {
        p = url + 5;
    } else {
        return 0;
    }
    const size_t h = strcspn(p, ":/?");
    if (!h || h >= sizeof(a->host)) return 0;
    memcpy(a->host, p, h);
    a->host[h] = 0;
    p += h;
    snprintf(a->port, sizeof(a->port), "%s", a->tls ? "443" : "80");
    if (*p == ':') {
        const size_t pl = strcspn(p + 1, "/?");
        if (!pl || pl >= sizeof(a->port)) return 0;
        memcpy(a->port, p + 1, pl);
        a->port[pl] = 0;
        p += 1 + pl;
    }
    snprintf(target, n, "%s%s", *p == '/' ? "" : "/", p);
    return 1;
}

static int handshake(KrunkerWs *w, HttpConn *c, const HttpAddr *a, const char *target) {
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
    char req[WS_URL_MAX + 1024];
    // Origin: the page krunker.io's services serve (a browser sends its page origin)
    const int len = snprintf(req, sizeof(req),
                             "GET %s HTTP/1.1\r\nHost: %s%s%s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                             "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\nOrigin: https://krunker.io\r\n"
                             "User-Agent: KrunkNative\r\n\r\n",
                             target, a->host, default_port ? "" : ":", default_port ? "" : a->port, key);
    const int sent = len > 0 && (size_t) len < sizeof(req) && http_conn_send(c, req, (size_t) len);
    wipe(req, sizeof(req));
    if (!sent) return 0;
    char resp[4096];
    size_t n = 0;
    while (n + 1 < sizeof(resp)) {
        if (!recv_exact(w, c, resp + n, 1)) return 0;
        n++;
        if (n >= 4 && !memcmp(resp + n - 4, "\r\n\r\n", 4)) break;
    }
    resp[n] = 0;
    int status = 0;
    if (sscanf(resp, "HTTP/%*s %d", &status) != 1 || status != 101) {
        fprintf(stderr, "[%s] upgrade refused: %.*s\n", w->tag, (int) strcspn(resp, "\r\n"), resp);
        return 0;
    }
    return 1;
}

static void deliver(KrunkerWs *w, const unsigned generation, const int event, const void *data, const size_t len, const int binary, const int code) {
    lock(w);
    if (generation == w->generation) queue_in(w, event, data, len, binary, code);
    unlock(w);
}

typedef struct {
    KrunkerWs *w;
    unsigned generation;
} WsStart;

static void *ws_thread(void *arg) {
    WsStart start = *(WsStart *) arg;
    free(arg);
    KrunkerWs *w = start.w;
    const unsigned generation = start.generation;
    HttpAddr a;
    char target[WS_URL_MAX];
    HttpConn c = {.fd = -1};
    int code = 1006;
    const int split = url_split(w->url, &a, target, sizeof(target));
    wipe(w->url, sizeof(w->url));
    if (!split) {
        fprintf(stderr, "[%s] bad socket URL\n", w->tag);
    } else if (!http_conn_open(&c, &a, WS_TIMEOUT_MS)) {
        fprintf(stderr, "[%s] cannot reach %s: %s\n", w->tag, a.host, http_conn_error());
    } else if (handshake(w, &c, &a, target) && !w->quit) {
        w->open = 1;
        printf("[%s] connected to %s\n", w->tag, a.host);
        deliver(w, generation, KRUNKER_WS_OPEN, NULL, 0, 0, 0);
        uint8_t *message = NULL;
        size_t message_len = 0;
        int ok = 1, binary = 0;
        while (ok && !w->quit) {
            lock(w);
            WsMsg *out = w->out_head;
            w->out_head = w->out_tail = NULL;
            unlock(w);
            while (out) {
                WsMsg *next = out->next;
                if (ok) ok = send_frame(&c, out->binary ? 2 : 1, out->data, out->len);
                wipe(out->data, out->len);
                free(out);
                out = next;
            }
            if (!ok) break;
            struct pollfd pfd[2] = {{c.fd, POLLIN, 0}, {w->wake[0], POLLIN, 0}};
            const int ready = http_conn_pending(&c) ? 1 : poll(pfd, 2, 100);
            if (ready < 0 && errno == EINTR) continue;
            if (ready < 0) break;
            if (pfd[1].revents & POLLIN) {
                char drain[64];
                while (read(w->wake[0], drain, sizeof(drain)) > 0) {}
            }
            if (!http_conn_pending(&c) && !(pfd[0].revents & (POLLIN | POLLERR | POLLHUP))) continue;
            uint8_t head[2];
            if (!recv_exact(w, &c, head, 2)) break;
            const int fin = head[0] & 0x80, opcode = head[0] & 0x0f;
            uint64_t len = head[1] & 0x7f;
            if (len == 126) {
                uint8_t ext[2];
                if (!recv_exact(w, &c, ext, 2)) break;
                len = (uint64_t) ext[0] << 8 | ext[1];
            } else if (len == 127) {
                uint8_t ext[8];
                if (!recv_exact(w, &c, ext, 8)) break;
                len = 0;
                for (int i = 0; i < 8; i++) len = len << 8 | ext[i];
            }
            uint8_t mask[4] = {0};
            if ((head[1] & 0x80) && !recv_exact(w, &c, mask, 4)) break;
            if (len > WS_MAX_MESSAGE || message_len + len > WS_MAX_MESSAGE) break;
            uint8_t *payload = malloc((size_t) len + 1);
            if (!payload || (len && !recv_exact(w, &c, payload, (size_t) len))) {
                free(payload);
                break;
            }
            if (head[1] & 0x80) for (uint64_t i = 0; i < len; i++) payload[i] ^= mask[i & 3];
            if (opcode == 8) { // close: status code
                code = len >= 2 ? payload[0] << 8 | payload[1] : 1005;
                free(payload);
                break;
            }
            if (opcode == 9) {
                ok = send_frame(&c, 10, payload, (size_t) len);
                free(payload);
                continue;
            }
            if (opcode == 10) {
                free(payload);
                continue;
            }
            if (opcode == 1 || opcode == 2) {
                message_len = 0;
                binary = opcode == 2;
            }
            uint8_t *grown = realloc(message, message_len + (size_t) len + 1);
            if (!grown) {
                free(payload);
                break;
            }
            message = grown;
            memcpy(message + message_len, payload, (size_t) len);
            message_len += (size_t) len;
            free(payload);
            if (fin) {
                if (message_len) deliver(w, generation, KRUNKER_WS_MESSAGE, message, message_len, binary, 0);
                message_len = 0;
            }
        }
        if (w->quit) {
            const uint8_t bye[2] = {1000 >> 8, 1000 & 0xff};
            send_frame(&c, 8, bye, 2);
            code = 1000;
        }
        free(message);
    }
    if (c.fd >= 0) http_conn_close(&c);
    w->open = 0;
    if (!w->quit) deliver(w, generation, KRUNKER_WS_CLOSED, NULL, 0, 0, code);
    return NULL;
}

static void wake(KrunkerWs *w) {
    if (w->wake[1] >= 0) {
        const char b = 1;
        if (write(w->wake[1], &b, 1) < 0) { /* full: the thread wakes anyway */ }
    }
}

int krunker_ws_open(KrunkerWs *w, const char *url) {
    if (!w) return 0;
    krunker_ws_close(w);
    if (!url || strlen(url) >= sizeof(w->url) || strncmp(url, "ws", 2)) return 0;
    if (w->wake[0] < 0 && pipe(w->wake) == 0) {
        fcntl(w->wake[0], F_SETFL, O_NONBLOCK);
        fcntl(w->wake[1], F_SETFL, O_NONBLOCK);
    }
    WsStart *start = malloc(sizeof(*start));
    if (!start) return 0;
    lock(w);
    w->generation++;
    start->w = w;
    start->generation = w->generation;
    unlock(w);
    snprintf(w->url, sizeof(w->url), "%s", url);
    w->quit = 0;
    if (pthread_create(&w->thread, NULL, ws_thread, start) != 0) {
        free(start);
        wipe(w->url, sizeof(w->url));
        return 0;
    }
    w->thread_running = 1;
    return 1;
}

int krunker_ws_send(KrunkerWs *w, const void *data, const size_t len, const int binary) {
    if (!w || !w->open || !w->thread_running || !data) return 0;
    WsMsg *m = msg_new(0, data, len, binary, 0);
    if (!m) return 0;
    lock(w);
    push(&w->out_head, &w->out_tail, m);
    unlock(w);
    wake(w);
    return 1;
}

void krunker_ws_close(KrunkerWs *w) {
    if (!w) return;
    if (w->thread_running) {
        w->quit = 1;
        wake(w);
        pthread_join(w->thread, NULL);
        w->thread_running = 0;
    }
    w->open = 0;
    lock(w);
    w->generation++;
    free_list(&w->out_head, &w->out_tail);
    free_list(&w->in_head, &w->in_tail);
    unlock(w);
}

// ---------------------------------------------------------------- Windows: none yet

#else
int krunker_ws_open(KrunkerWs *w, const char *url) {
    (void) w;
    (void) url;
    return 0; // no WebSocket client for krunker.io in this build (krunker_net.c: the same)
}
int krunker_ws_send(KrunkerWs *w, const void *data, const size_t len, const int binary) {
    (void) w;
    (void) data;
    (void) len;
    (void) binary;
    return 0;
}
void krunker_ws_close(KrunkerWs *w) {
    if (!w) return;
    lock(w);
    free_list(&w->in_head, &w->in_tail);
    unlock(w);
}
#endif
