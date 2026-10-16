// krunker.io's friends event bus socket (client/include/krunker_eb.h): a text WebSocket, natively
// a thread over http_conn (through the client's proxy, the game socket's handshake: krunker_net.c),
// on the web the browser's WebSocket. Events are queued and handed to the main thread by
// krunker_eb_poll.
#include <krunker_eb.h>
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
#define EB_NATIVE 1
#endif

#define EB_TIMEOUT_MS 8000
#define EB_MAX_MESSAGE (1u * 1024u * 1024u)
#define EB_URL_MAX 6144 // the base + /ws?at= + an encoded JWT

typedef struct EbMsg {
    struct EbMsg *next;
    int event, code;
    char text[];
} EbMsg;

static struct {
    pthread_mutex_t lock;
    int lock_ready;
    EbMsg *in_head, *in_tail;   // socket -> main thread
    EbMsg *out_head, *out_tail; // main thread -> socket (native)
    volatile int open;
    unsigned generation;
#ifdef __EMSCRIPTEN__
    EMSCRIPTEN_WEBSOCKET_T ws;
#elif defined(EB_NATIVE)
    pthread_t thread;
    int thread_running;
    volatile int quit;
    char url[EB_URL_MAX];
#endif
} E;
#ifdef EB_NATIVE
static int g_wake[2] = {-1, -1}; // wakes the thread for a send / the close
#endif

static void wipe(void *p, size_t n) {
    volatile unsigned char *v = p;
    while (n--) *v++ = 0;
}

static void lock(void) {
    if (!E.lock_ready) {
        pthread_mutex_init(&E.lock, NULL);
        E.lock_ready = 1;
    }
    pthread_mutex_lock(&E.lock);
}

static void unlock(void) { pthread_mutex_unlock(&E.lock); }

static EbMsg *msg_new(const int event, const char *text, const size_t len, const int code) {
    EbMsg *m = malloc(sizeof(EbMsg) + len + 1);
    if (!m) return NULL;
    m->next = NULL;
    m->event = event;
    m->code = code;
    if (len) memcpy(m->text, text, len);
    m->text[len] = 0;
    return m;
}

static void push(EbMsg **head, EbMsg **tail, EbMsg *m) {
    if (!m) return;
    if (*tail) (*tail)->next = m;
    else *head = m;
    *tail = m;
}

static void free_list(EbMsg **head, EbMsg **tail) {
    while (*head) {
        EbMsg *next = (*head)->next;
        free(*head);
        *head = next;
    }
    *tail = NULL;
}

// (the generation check is the caller's: under the lock)
static void queue_in(const int event, const char *text, const size_t len, const int code) {
    push(&E.in_head, &E.in_tail, msg_new(event, text, len, code));
}

void krunker_eb_poll(const KrunkerEbFn fn, void *user) {
    if (!E.lock_ready) return;
    lock();
    EbMsg *m = E.in_head;
    E.in_head = E.in_tail = NULL;
    unlock();
    while (m) {
        EbMsg *next = m->next;
        if (fn) fn(user, m->event, m->text, m->code);
        free(m);
        m = next;
    }
}

int krunker_eb_is_open(void) { return E.open; }

// ---------------------------------------------------------------- web: the browser's WebSocket

#ifdef __EMSCRIPTEN__
static bool on_open(int type, const EmscriptenWebSocketOpenEvent *e, void *user) {
    (void) type;
    if ((unsigned) (uintptr_t) user != E.generation || e->socket != E.ws) return true;
    E.open = 1;
    lock();
    queue_in(KRUNKER_EB_OPEN, NULL, 0, 0);
    unlock();
    return true;
}

static bool on_message(int type, const EmscriptenWebSocketMessageEvent *e, void *user) {
    (void) type;
    // v10: only string messages are parsed
    if ((unsigned) (uintptr_t) user != E.generation || e->socket != E.ws || !e->isText) return true;
    const size_t n = e->numBytes && e->data[e->numBytes - 1] == 0 ? e->numBytes - 1 : e->numBytes;
    lock();
    queue_in(KRUNKER_EB_MESSAGE, (const char *) e->data, n, 0);
    unlock();
    return true;
}

static bool on_error(int type, const EmscriptenWebSocketErrorEvent *e, void *user) {
    (void) type;
    (void) e;
    (void) user;
    return true; // v10: onerror does nothing, onclose reports
}

static bool on_close(int type, const EmscriptenWebSocketCloseEvent *e, void *user) {
    (void) type;
    emscripten_websocket_delete(e->socket);
    if (e->socket != E.ws || (unsigned) (uintptr_t) user != E.generation) return true;
    E.ws = 0;
    E.open = 0;
    lock();
    queue_in(KRUNKER_EB_CLOSED, NULL, 0, e->code);
    unlock();
    return true;
}

int krunker_eb_open(const char *url) {
    krunker_eb_close();
    if (!url || strncmp(url, "ws", 2)) return 0;
    E.generation++;
    EmscriptenWebSocketCreateAttributes attr;
    emscripten_websocket_init_create_attributes(&attr);
    attr.url = url;
    attr.createOnMainThread = true;
    E.ws = emscripten_websocket_new(&attr);
    if (E.ws <= 0) {
        E.ws = 0;
        return 0;
    }
    void *gen = (void *) (uintptr_t) E.generation;
    emscripten_websocket_set_onopen_callback(E.ws, gen, on_open);
    emscripten_websocket_set_onmessage_callback(E.ws, gen, on_message);
    emscripten_websocket_set_onerror_callback(E.ws, gen, on_error);
    emscripten_websocket_set_onclose_callback(E.ws, gen, on_close);
    return 1;
}

int krunker_eb_send(const char *text) {
    if (!E.open || E.ws <= 0 || !text) return 0;
    return emscripten_websocket_send_utf8_text(E.ws, text) == EMSCRIPTEN_RESULT_SUCCESS;
}

void krunker_eb_close(void) {
    E.generation++;
    E.open = 0;
    if (E.ws > 0) {
        const EMSCRIPTEN_WEBSOCKET_T old = E.ws;
        E.ws = 0;
        emscripten_websocket_close(old, 1000, "client disconnect");
    }
    if (E.lock_ready) {
        lock();
        free_list(&E.in_head, &E.in_tail);
        unlock();
    }
}

// ---------------------------------------------------------------- native: a thread over http_conn

#elif defined(EB_NATIVE)
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

static int recv_exact(HttpConn *c, void *out, size_t len) {
    uint8_t *p = out;
    while (len) {
        if (E.quit) return 0;
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

static int handshake(HttpConn *c, const HttpAddr *a, const char *target) {
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
    char req[EB_URL_MAX + 1024];
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
        if (!recv_exact(c, resp + n, 1)) return 0;
        n++;
        if (n >= 4 && !memcmp(resp + n - 4, "\r\n\r\n", 4)) break;
    }
    resp[n] = 0;
    int status = 0;
    if (sscanf(resp, "HTTP/%*s %d", &status) != 1 || status != 101) {
        fprintf(stderr, "[krunker-eb] event bus upgrade refused: %.*s\n", (int) strcspn(resp, "\r\n"), resp);
        return 0;
    }
    return 1;
}

static void deliver(const unsigned generation, const int event, const char *text, const size_t len, const int code) {
    lock();
    if (generation == E.generation) queue_in(event, text, len, code);
    unlock();
}

static void *eb_thread(void *arg) {
    const unsigned generation = (unsigned) (uintptr_t) arg;
    HttpAddr a;
    char target[EB_URL_MAX];
    HttpConn c = {.fd = -1};
    int code = 1006;
    const int split = url_split(E.url, &a, target, sizeof(target));
    wipe(E.url, sizeof(E.url));
    if (!split) {
        fprintf(stderr, "[krunker-eb] bad event bus URL\n");
    } else if (!http_conn_open(&c, &a, EB_TIMEOUT_MS)) {
        fprintf(stderr, "[krunker-eb] cannot reach the event bus: %s\n", http_conn_error());
    } else if (handshake(&c, &a, target) && !E.quit) {
        E.open = 1;
        printf("[krunker-eb] connected to %s\n", a.host);
        deliver(generation, KRUNKER_EB_OPEN, NULL, 0, 0);
        uint8_t *message = NULL;
        size_t message_len = 0;
        int ok = 1, text = 0;
        while (ok && !E.quit) {
            lock();
            EbMsg *out = E.out_head;
            E.out_head = E.out_tail = NULL;
            unlock();
            while (out) {
                EbMsg *next = out->next;
                if (ok) ok = send_frame(&c, 1, out->text, strlen(out->text));
                free(out);
                out = next;
            }
            if (!ok) break;
            struct pollfd pfd[2] = {{c.fd, POLLIN, 0}, {g_wake[0], POLLIN, 0}};
            const int ready = http_conn_pending(&c) ? 1 : poll(pfd, 2, 100);
            if (ready < 0 && errno == EINTR) continue;
            if (ready < 0) break;
            if (pfd[1].revents & POLLIN) {
                char drain[64];
                while (read(g_wake[0], drain, sizeof(drain)) > 0) {}
            }
            if (!http_conn_pending(&c) && !(pfd[0].revents & (POLLIN | POLLERR | POLLHUP))) continue;
            uint8_t head[2];
            if (!recv_exact(&c, head, 2)) break;
            const int fin = head[0] & 0x80, opcode = head[0] & 0x0f;
            uint64_t len = head[1] & 0x7f;
            if (len == 126) {
                uint8_t ext[2];
                if (!recv_exact(&c, ext, 2)) break;
                len = (uint64_t) ext[0] << 8 | ext[1];
            } else if (len == 127) {
                uint8_t ext[8];
                if (!recv_exact(&c, ext, 8)) break;
                len = 0;
                for (int i = 0; i < 8; i++) len = len << 8 | ext[i];
            }
            uint8_t mask[4] = {0};
            if ((head[1] & 0x80) && !recv_exact(&c, mask, 4)) break;
            if (len > EB_MAX_MESSAGE || message_len + len > EB_MAX_MESSAGE) break;
            uint8_t *payload = malloc((size_t) len + 1);
            if (!payload || (len && !recv_exact(&c, payload, (size_t) len))) {
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
                text = opcode == 1;
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
                if (text && message_len) deliver(generation, KRUNKER_EB_MESSAGE, (const char *) message, message_len, 0); // v10: strings only
                message_len = 0;
            }
        }
        if (E.quit) {
            const uint8_t bye[2] = {1000 >> 8, 1000 & 0xff};
            send_frame(&c, 8, bye, 2);
            code = 1000;
        }
        free(message);
    }
    if (c.fd >= 0) http_conn_close(&c);
    E.open = 0;
    if (!E.quit) deliver(generation, KRUNKER_EB_CLOSED, NULL, 0, code);
    return NULL;
}

int krunker_eb_open(const char *url) {
    krunker_eb_close();
    if (!url || strlen(url) >= sizeof(E.url) || strncmp(url, "ws", 2)) return 0;
    if (g_wake[0] < 0 && pipe(g_wake) == 0) {
        fcntl(g_wake[0], F_SETFL, O_NONBLOCK);
        fcntl(g_wake[1], F_SETFL, O_NONBLOCK);
    }
    lock();
    E.generation++;
    const unsigned gen = E.generation;
    unlock();
    snprintf(E.url, sizeof(E.url), "%s", url);
    E.quit = 0;
    if (pthread_create(&E.thread, NULL, eb_thread, (void *) (uintptr_t) gen) != 0) {
        wipe(E.url, sizeof(E.url));
        return 0;
    }
    E.thread_running = 1;
    return 1;
}

int krunker_eb_send(const char *text) {
    if (!E.open || !E.thread_running || !text) return 0;
    lock();
    push(&E.out_head, &E.out_tail, msg_new(0, text, strlen(text), 0));
    unlock();
    if (g_wake[1] >= 0) {
        const char b = 1;
        if (write(g_wake[1], &b, 1) < 0) { /* full: the thread wakes anyway */ }
    }
    return 1;
}

void krunker_eb_close(void) {
    if (E.thread_running) {
        E.quit = 1;
        if (g_wake[1] >= 0) {
            const char b = 1;
            if (write(g_wake[1], &b, 1) < 0) { /* full: it wakes anyway */ }
        }
        pthread_join(E.thread, NULL);
        E.thread_running = 0;
    }
    E.open = 0;
    if (E.lock_ready) {
        lock();
        E.generation++;
        free_list(&E.out_head, &E.out_tail);
        free_list(&E.in_head, &E.in_tail);
        unlock();
    }
}

// ---------------------------------------------------------------- Windows: none yet

#else
int krunker_eb_open(const char *url) {
    (void) url;
    return 0; // no WebSocket client for krunker.io in this build (krunker_net.c: the same)
}
int krunker_eb_send(const char *text) {
    (void) text;
    return 0;
}
void krunker_eb_close(void) {}
#endif
