// Brain client, native transports (client/include/backend.h): one HTTP worker thread
// (blocking HTTP/1.1 GETs, "Connection: close") and one social socket thread (a small
// RFC 6455 client: JSON text frames, masked, ping/pong, reassembly). Both hand their
// results to the main thread through mutex-protected queues drained by backend_poll().
// POSIX: http:// and ws://, plus https:// and wss:// when the build found OpenSSL
// (KRUNKNATIVE_HAVE_TLS: libssl, peer verified against the system CA store and the
// host name; the online-mode defaults https://coordinator.krunk.io need it), over http_conn.c.
// Windows: WinHTTP (backend_winhttp.c: TLS against the Windows certificate store, the system
// proxy, WebSockets on Windows 8+). A Windows client used to compile this file to no-op stubs:
// no account service, an empty server finder and a game connection to 127.0.0.1.
#ifndef __EMSCRIPTEN__
#include <backend.h>
#include <cJSON.h>
#include "backend_internal.h"
#include <headless.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stddef.h>
#include <html_dom.h>
#include <online_mode.h>
#ifdef _WIN32
#include <windows.h>
#include "backend_winhttp.h"
#else
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <http_conn.h>
#endif

#define HTTP_TIMEOUT_MS 8000
#define HTTP_MAX_BODY (4u * 1024u * 1024u)
#define WS_MAX_MESSAGE (2u * 1024u * 1024u)
#define WS_BACKOFF_MIN_MS 1000
#define WS_BACKOFF_MAX_MS 10000
#define HTTP_RETRY_DELAY_MS 500 // a brain GET that got no answer is tried once more after this

// ------------------------------------------------------------------ small helpers

static void sleep_ms(const int ms) {
#ifdef _WIN32
    Sleep((DWORD) ms);
#else
    struct timespec ts = {ms / 1000, (long) (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
#endif
}

typedef struct {
    char *data;
    size_t len, cap;
} Bytes;

static int bytes_push(Bytes *b, const void *data, const size_t len) {
    if (b->len + len + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while (cap < b->len + len + 1) cap *= 2;
        char *grown = realloc(b->data, cap);
        if (!grown) return 0;
        b->data = grown;
        b->cap = cap;
    }
    memcpy(b->data + b->len, data, len);
    b->len += len;
    b->data[b->len] = 0;
    return 1;
}

// ------------------------------------------------------------------ state

typedef struct HttpJob {
    struct HttpJob *next;
    char *url;      // full URL
    char method[8];
    char *body;     // request body (JSON), NULL = none; replaced by the response body
    BackendHttpFn fn;
    void *user;
    int status;
    size_t length;
    int timeout_ms; // connect / receive timeout
    char err[192];  // status 0: why (handed to the callback as the body, backend.h)
    int tls_reply;  // backend_http_get_parallel: a failed TLS handshake is BACKEND_HTTP_TLS_FAILED, not 0
    int tls_failed; // the target's TLS handshake failed (http_conn_tls_failed when the connection did not open)
} HttpJob;

typedef struct WsMsg {
    struct WsMsg *next;
    char *text;     // NULL = event
    const char *event;
} WsMsg;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
// Three lanes, one worker each: the brain's requests, other hosts' (backend_http_get_external:
// krunker.io's game list) so a slow or unreachable outside host never delays a seek, and page media
// (backend_http_get_media: remote images) so a page's thumbnails never delay the game list.
enum { HTTP_LANE_BRAIN, HTTP_LANE_EXTERNAL, HTTP_LANE_MEDIA, HTTP_LANES };
static pthread_cond_t g_http_cond[HTTP_LANES] = {PTHREAD_COND_INITIALIZER, PTHREAD_COND_INITIALIZER, PTHREAD_COND_INITIALIZER};
static HttpJob *g_http_pending[HTTP_LANES], *g_http_done;
static int g_http_thread_up[HTTP_LANES];
static WsMsg *g_ws_out, *g_ws_in;
static int g_ws_thread_up;
static int g_ws_open;          // the socket is open (written by the thread, under g_lock)
static int g_ws_wanted;        // something asked for the socket
static unsigned g_ws_generation; // bumped by backend_set_url: the thread reconnects

static void push_tail(void **head, void *item, const size_t next_offset) {
    void **link = head;
    while (*link) link = (void **) ((char *) *link + next_offset);
    *link = item;
}
#define QUEUE_PUSH(head, item) push_tail((void **) &(head), (item), offsetof(__typeof__(*(item)), next))

// ------------------------------------------------------------------ URL

static int g_url_default; // the URL is the mode's default account service (online_mode.h): probe it

// Online mode's account service (online_mode.h, ONLINE_DEFAULT_ACCOUNT_URL). The client
// never reads the backend's config/config.json (it holds the database credentials): a
// local backend reaches the client as KRUNKNATIVE_BRAIN_URL (the Makefile's run exports
// it). backend_startup passes "" in offline mode: no account service at all.
void backend_init(const char *cli_url) {
    g_url_default = 0;
    if (cli_url && !cli_url[0]) {
        backend_store_url("");
        return;
    }
    const char *url = cli_url;
    if (!url || !url[0]) url = getenv("KRUNKNATIVE_BRAIN_URL");
    char saved[256] = "";
    if ((!url || !url[0]) && html_storage_get("knBrainUrl", saved, sizeof(saved)) > 0) url = saved; // client setting
    char local[256] = "";
    if ((!url || !url[0]) && headless_enabled()) {
        // headless runs (tests, captures, smoke) never reach the production coordinator unless
        // a URL is given explicitly (--brain / KRUNKNATIVE_BRAIN_URL)
        backend_store_url("");
        printf("[backend] headless: no account service (set KRUNKNATIVE_BRAIN_URL to use one)\n");
        return;
    }
    if (!url || !url[0]) {
        // nothing configured: the mode's account service (online: coordinator.krunk.io), probed first
        snprintf(local, sizeof(local), "%s", online_default_account_url());
        url = local;
        g_url_default = 1;
    }
    backend_store_url(url);
    if (backend_enabled()) printf("[backend] brain %s\n", backend_url());
}

int backend_url_is_default(void) { return g_url_default && backend_enabled(); }

void backend_set_url(const char *url) {
    g_url_default = 0;
    char copy[256];
    snprintf(copy, sizeof(copy), "%s", url ? url : "");
    pthread_mutex_lock(&g_lock);
    const int changed = strcmp(copy, backend_url()) != 0;
    backend_store_url(copy);
    if (changed) g_ws_generation++; // same brain: keep the socket
    pthread_mutex_unlock(&g_lock);
}

void backend_social_reconnect(void) {
    pthread_mutex_lock(&g_lock);
    g_ws_generation++; // the session ends at its next turn, the thread connects again
    pthread_mutex_unlock(&g_lock);
}

// ------------------------------------------------------------------ HTTP

// Removes chunked transfer-encoding in place; returns the new length.
static size_t dechunk(char *body, const size_t len) {
    size_t in = 0, out = 0;
    while (in < len) {
        char *end = NULL;
        const unsigned long size = strtoul(body + in, &end, 16);
        if (end == body + in) break;
        const char *line_end = strstr(end, "\r\n");
        if (!line_end) break;
        in = (size_t) (line_end - body) + 2;
        if (size == 0 || in + size > len) break;
        memmove(body + out, body + in, size);
        out += size;
        in += size + 2;
    }
    body[out] = 0;
    return out;
}

#ifdef _WIN32
static void http_run(HttpJob *job) {
    char *req_body = job->body;
    job->body = NULL;
    job->status = 0;
    char *resp = NULL;
    size_t len = 0;
    const int status = kn_winhttp_request(job->method, job->url, req_body, job->timeout_ms, &resp, &len, job->err, sizeof(job->err));
    free(req_body);
    job->body = resp;
    job->length = len;
    job->status = resp ? status : 0;
}
#else
static void http_run(HttpJob *job) {
    HttpAddr a;
    char *req_body = job->body;
    job->body = NULL;
    job->status = 0;
    if (!http_url_parse(job->url, &a)) {
        snprintf(job->err, sizeof(job->err), "%s", strncmp(job->url, "https://", 8) ? "bad URL" : "https:// needs a client built with OpenSSL");
        free(req_body);
        return;
    }
    HttpConn c;
    if (!http_conn_open(&c, &a, job->timeout_ms)) {
        job->tls_failed = http_conn_tls_failed();
        snprintf(job->err, sizeof(job->err), "cannot connect to %s:%s%s", a.host, a.port, a.tls ? " (TLS)" : "");
        free(req_body);
        return;
    }
    // the whole path + query of the URL after host[:port]
    Bytes req = {0};
    char head[640];
    const size_t body_len = req_body ? strlen(req_body) : 0;
    const char *target = http_url_target(job->url);
    bytes_push(&req, job->method, strlen(job->method));
    bytes_push(&req, " ", 1);
    if (!target[0]) bytes_push(&req, "/", 1);
    bytes_push(&req, target, strlen(target));
    const int default_port = !strcmp(a.port, a.tls ? "443" : "80");
    snprintf(head, sizeof(head), " HTTP/1.1\r\nHost: %s%s%s\r\nUser-Agent: KrunkNative\r\nAccept: application/json\r\nConnection: close\r\n",
             a.host, default_port ? "" : ":", default_port ? "" : a.port);
    bytes_push(&req, head, strlen(head));
    if (req_body) {
        snprintf(head, sizeof(head), "Content-Type: application/json\r\nContent-Length: %zu\r\n", body_len);
        bytes_push(&req, head, strlen(head));
    }
    bytes_push(&req, "\r\n", 2);
    if (req_body) bytes_push(&req, req_body, body_len);
    free(req_body);
    const int sent = req.data && http_conn_send(&c, req.data, req.len);
    free(req.data);
    if (!sent) {
        snprintf(job->err, sizeof(job->err), "send to %s failed", a.host);
        http_conn_close(&c);
        return;
    }

    Bytes resp = {0};
    char buf[8192];
    for (;;) {
        const ssize_t r = http_conn_recv(&c, buf, sizeof(buf));
        if (r <= 0) break;
        if (!bytes_push(&resp, buf, (size_t) r) || resp.len > HTTP_MAX_BODY) break;
    }
    http_conn_close(&c);
    if (!resp.data) {
        snprintf(job->err, sizeof(job->err), "no answer from %s:%s (%s)", a.host, a.port, errno == EAGAIN || errno == EWOULDBLOCK ? "timed out" : "closed");
        return;
    }

    int status = 0;
    char *header_end = strstr(resp.data, "\r\n\r\n");
    if (sscanf(resp.data, "HTTP/%*s %d", &status) != 1 || !header_end) {
        snprintf(job->err, sizeof(job->err), "not an HTTP answer from %s", a.host);
        free(resp.data);
        return;
    }
    *header_end = 0;
    int chunked = 0;
    for (char *h = resp.data; *h; h++) {
        if ((h == resp.data || h[-1] == '\n') && !strncasecmp(h, "transfer-encoding:", 18) && strstr(h, "chunked")) chunked = 1;
    }
    char *body = header_end + 4;
    size_t length = resp.len - (size_t) (body - resp.data);
    if (chunked) length = dechunk(body, length);
    job->body = malloc(length + 1);
    if (job->body) {
        memcpy(job->body, body, length);
        job->body[length] = 0;
        job->length = length;
        job->status = status;
    }
    free(resp.data);
}
#endif

static char *redacted_url(const char *url); // the main thread's log helper (below)

// A GET that creates a game on a server (/mm/host-game): its first try may have created the game already,
// and a second try would create another one (an orphan lobby) and join that. Never retried.
static int creates_game(const char *url) {
    const char *p = strstr(url, "/mm/host-game");
    return p && (!p[13] || p[13] == '?'); // "/mm/host-game" is 13 characters
}

// A brain GET that got no HTTP answer at all (status 0: a connect that timed out or was refused, a
// DNS or TLS failure, a send that failed, no reply before the timeout) is tried once more,
// HTTP_RETRY_DELAY_MS later. Those failures are usually transient: a lost SYN past the connect timeout,
// a reset, a peer that closed before it answered, a DNS entry gone stale. The retry is a new connection: a
// failed connect dropped its DNS entry (http_conn.c), and the TLS ticket the failed try took is spent,
// so the next handshake is a full one. Only GETs: a POST may already have taken effect. A real answer,
// whatever its status (a refused seek token, a 403, a 404), is final and never retried here.
static void http_retry(HttpJob *job) {
    char *shown = redacted_url(job->url);
    fprintf(stderr, "[http] GET %s failed: %s; trying once more\n", shown ? shown : "(url)", job->err);
    free(shown);
    job->err[0] = 0;
    sleep_ms(HTTP_RETRY_DELAY_MS);
    http_run(job);
}

static void *http_thread(void *arg) {
    const int lane = (int) (intptr_t) arg;
    for (;;) {
        pthread_mutex_lock(&g_lock);
        while (!g_http_pending[lane]) pthread_cond_wait(&g_http_cond[lane], &g_lock);
        HttpJob *job = g_http_pending[lane];
        g_http_pending[lane] = job->next;
        job->next = NULL;
        pthread_mutex_unlock(&g_lock);

        // the brain's GETs only: a POST may already have taken effect, and http_run takes its body
        const int retry = lane == HTTP_LANE_BRAIN && !strcmp(job->method, "GET") && !job->body && !creates_game(job->url);
        http_run(job);
        if (retry && !job->status) http_retry(job);

        pthread_mutex_lock(&g_lock);
        QUEUE_PUSH(g_http_done, job);
        pthread_mutex_unlock(&g_lock);
    }
    return NULL;
}

static int http_queue_lane(const int lane, const int timeout_ms, const char *method, const char *url, const char *body,
                           const BackendHttpFn fn, void *user) {
    if (!url || !url[0]) return 0;
    HttpJob *job = calloc(1, sizeof(*job));
    if (!job) return 0;
    job->timeout_ms = timeout_ms > 0 ? timeout_ms : HTTP_TIMEOUT_MS;
    snprintf(job->method, sizeof(job->method), "%s", method);
    job->url = strdup(url);
    job->body = body ? strdup(body) : NULL;
    job->fn = fn;
    job->user = user;
    if (!job->url || (body && !job->body)) {
        free(job->url);
        free(job->body);
        free(job);
        return 0;
    }
    pthread_mutex_lock(&g_lock);
    if (!g_http_thread_up[lane]) {
        pthread_t t;
        if (pthread_create(&t, NULL, http_thread, (void *) (intptr_t) lane) == 0) {
            pthread_detach(t);
            g_http_thread_up[lane] = 1;
        }
    }
    QUEUE_PUSH(g_http_pending[lane], job);
    pthread_cond_signal(&g_http_cond[lane]);
    const int up = g_http_thread_up[lane];
    pthread_mutex_unlock(&g_lock);
    return up;
}

static int http_queue(const char *method, const char *url, const char *body, const BackendHttpFn fn, void *user) {
    return http_queue_lane(HTTP_LANE_BRAIN, HTTP_TIMEOUT_MS, method, url, body, fn, user);
}

int backend_http_get_external(const char *url, const int timeout_ms, const BackendHttpFn fn, void *user) {
    return http_queue_lane(HTTP_LANE_EXTERNAL, timeout_ms, "GET", url, NULL, fn, user);
}

int backend_http_get_media(const char *url, const int timeout_ms, const BackendHttpFn fn, void *user) {
    if (backend_brain_refuses_url(url)) return 0;
    return http_queue_lane(HTTP_LANE_MEDIA, timeout_ms, "GET", url, NULL, fn, user);
}

// One job on a thread of its own (backend_http_get_parallel): its completion joins the same done
// queue, so backend_poll hands the answers over in the order they arrived.
static void *http_job_thread(void *arg) {
    HttpJob *job = arg;
    http_run(job);
    pthread_mutex_lock(&g_lock);
    QUEUE_PUSH(g_http_done, job);
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

int backend_http_get_parallel(const char *url, const int timeout_ms, const BackendHttpFn fn, void *user) {
    if (!url || !url[0] || backend_brain_refuses_url(url)) return 0;
    HttpJob *job = calloc(1, sizeof(*job));
    if (!job) return 0;
    job->timeout_ms = timeout_ms > 0 ? timeout_ms : HTTP_TIMEOUT_MS;
    snprintf(job->method, sizeof(job->method), "GET");
    job->url = strdup(url);
    job->fn = fn;
    job->user = user;
    job->tls_reply = 1;
    pthread_t t;
    if (!job->url || pthread_create(&t, NULL, http_job_thread, job) != 0) {
        free(job->url);
        free(job);
        return 0;
    }
    pthread_detach(t);
    return 1;
}

int backend_http_get(const char *path, const BackendHttpFn fn, void *user) {
    if (!backend_enabled() || !path || backend_brain_refuses_path()) return 0;
    char url[2048];
    pthread_mutex_lock(&g_lock);
    const int n = snprintf(url, sizeof(url), "%s%s", backend_url(), path);
    pthread_mutex_unlock(&g_lock);
    if (n <= 0 || (size_t) n >= sizeof(url)) {
        // long paths (host-game ?config=): heap copy
        const size_t len = strlen(backend_url()) + strlen(path) + 1;
        char *big = malloc(len);
        if (!big) return 0;
        snprintf(big, len, "%s%s", backend_url(), path);
        const int ok = http_queue("GET", big, NULL, fn, user);
        free(big);
        return ok;
    }
    return http_queue("GET", url, NULL, fn, user);
}

int backend_http_request(const char *method, const char *url, const char *json_body, const BackendHttpFn fn, void *user) {
    if (backend_brain_refuses_url(url)) return 0;
    return http_queue(method && method[0] ? method : "GET", url, json_body, fn, user);
}

// ------------------------------------------------------------------ social socket

static void ws_queue_in(char *text, const char *event) {
    WsMsg *m = calloc(1, sizeof(*m));
    if (!m) {
        free(text);
        return;
    }
    m->text = text;
    m->event = event;
    pthread_mutex_lock(&g_lock);
    QUEUE_PUSH(g_ws_in, m);
    pthread_mutex_unlock(&g_lock);
}

#ifndef _WIN32
static int ws_send_frame(HttpConn *c, const int opcode, const char *data, const size_t len) {
    unsigned char head[14];
    size_t n = 0;
    head[n++] = (unsigned char) (0x80 | opcode);
    if (len < 126) {
        head[n++] = (unsigned char) (0x80 | len);
    } else if (len < 65536) {
        head[n++] = 0x80 | 126;
        head[n++] = (unsigned char) (len >> 8);
        head[n++] = (unsigned char) len;
    } else {
        head[n++] = 0x80 | 127;
        for (int i = 7; i >= 0; i--) head[n++] = (unsigned char) ((uint64_t) len >> (i * 8));
    }
    unsigned char mask[4];
    for (int i = 0; i < 4; i++) mask[i] = (unsigned char) (rand() & 0xff);
    memcpy(head + n, mask, 4);
    n += 4;
    char *masked = malloc(len ? len : 1);
    if (!masked) return 0;
    for (size_t i = 0; i < len; i++) masked[i] = (char) (data[i] ^ mask[i & 3]);
    const int ok = http_conn_send(c, head, n) && http_conn_send(c, masked, len);
    free(masked);
    return ok;
}

static int ws_handshake(HttpConn *c, const HttpAddr *a, const char *path) {
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    unsigned char raw[16];
    char key[25];
    for (int i = 0; i < 16; i++) raw[i] = (unsigned char) (rand() & 0xff);
    int k = 0;
    for (int i = 0; i < 16; i += 3) {
        const unsigned v = (unsigned) raw[i] << 16 | (i + 1 < 16 ? (unsigned) raw[i + 1] << 8 : 0) | (i + 2 < 16 ? raw[i + 2] : 0);
        key[k++] = b64[(v >> 18) & 63];
        key[k++] = b64[(v >> 12) & 63];
        key[k++] = i + 1 < 16 ? b64[(v >> 6) & 63] : '=';
        key[k++] = i + 2 < 16 ? b64[v & 63] : '=';
    }
    key[24] = 0;
    char req[768];
    const int len = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\nHost: %s:%s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\nUser-Agent: KrunkNative\r\n\r\n",
        path, a->host, a->port, key);
    if (len <= 0 || (size_t) len >= sizeof(req) || !http_conn_send(c, req, (size_t) len)) return 0;
    // read the response head byte by byte (it is tiny), stop at the blank line
    char resp[2048];
    size_t n = 0;
    while (n + 1 < sizeof(resp)) {
        const ssize_t r = http_conn_recv(c, resp + n, 1);
        if (r <= 0) return 0;
        n++;
        if (n >= 4 && !memcmp(resp + n - 4, "\r\n\r\n", 4)) break;
    }
    resp[n] = 0;
    int status = 0;
    return sscanf(resp, "HTTP/%*s %d", &status) == 1 && status == 101;
}

// Reads exactly len bytes, polling `quit` conditions every 100 ms. 0 on failure.
static int recv_exact(HttpConn *c, void *out, size_t len) {
    char *p = out;
    while (len) {
        const ssize_t r = http_conn_recv(c, p, len);
        if (r < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (r <= 0) return 0;
        p += r;
        len -= (size_t) r;
    }
    return 1;
}

// One open session: returns when the socket drops or the URL changed.
static void ws_session(HttpConn *c, const unsigned generation) {
    Bytes message = {0};
    int message_text = 1;
    for (;;) {
        // flush outgoing
        pthread_mutex_lock(&g_lock);
        WsMsg *out = g_ws_out;
        g_ws_out = NULL;
        const int stale = generation != g_ws_generation;
        pthread_mutex_unlock(&g_lock);
        int ok = !stale;
        while (out) {
            WsMsg *next = out->next;
            if (ok) ok = ws_send_frame(c, 1, out->text, strlen(out->text));
            free(out->text);
            free(out);
            out = next;
        }
        if (!ok) break;

        struct pollfd pfd = {c->fd, POLLIN, 0};
        const int ready = http_conn_pending(c) ? 1 : poll(&pfd, 1, 30);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) break;
        if (ready == 0) continue;
        if (pfd.revents & (POLLERR | POLLNVAL)) break;

        unsigned char head[2];
        if (!recv_exact(c, head, 2)) break;
        const int fin = head[0] & 0x80;
        const int opcode = head[0] & 0x0f;
        uint64_t len = head[1] & 0x7f;
        if (len == 126) {
            unsigned char ext[2];
            if (!recv_exact(c, ext, 2)) break;
            len = (uint64_t) ext[0] << 8 | ext[1];
        } else if (len == 127) {
            unsigned char ext[8];
            if (!recv_exact(c, ext, 8)) break;
            len = 0;
            for (int i = 0; i < 8; i++) len = len << 8 | ext[i];
        }
        unsigned char mask[4] = {0};
        const int masked = head[1] & 0x80;
        if (masked && !recv_exact(c, mask, 4)) break;
        if (len > WS_MAX_MESSAGE) break;
        char *payload = malloc((size_t) len + 1);
        if (!payload) break;
        if (len && !recv_exact(c, payload, (size_t) len)) {
            free(payload);
            break;
        }
        if (masked) for (uint64_t i = 0; i < len; i++) payload[i] ^= (char) mask[i & 3];
        payload[len] = 0;

        if (opcode == 8) { // close
            free(payload);
            break;
        } else if (opcode == 9) { // ping -> pong
            ok = ws_send_frame(c, 10, payload, (size_t) len);
            free(payload);
            if (!ok) break;
            continue;
        } else if (opcode == 10) {
            free(payload);
            continue;
        }
        if (opcode == 1 || opcode == 2) {
            message.len = 0;
            message_text = opcode == 1;
        }
        bytes_push(&message, payload, (size_t) len);
        free(payload);
        if (message.len > WS_MAX_MESSAGE) break;
        if (fin) {
            // binary frames are v3.1.1 msgpack (sent before the brain saw ?enc=json): ignored
            if (message_text && message.data) ws_queue_in(strdup(message.data), NULL);
            message.len = 0;
        }
    }
    free(message.data);
}

static void *ws_thread(void *arg) {
    (void) arg;
    int backoff = WS_BACKOFF_MIN_MS;
    srand((unsigned) time(NULL) ^ (unsigned) (uintptr_t) &backoff);
    for (;;) {
        char url[256], ws_url[320];
        pthread_mutex_lock(&g_lock);
        snprintf(url, sizeof(url), "%s", backend_url());
        backend_social_ws_url(ws_url, sizeof(ws_url));
        const unsigned generation = g_ws_generation;
        pthread_mutex_unlock(&g_lock);

        HttpAddr a;
        HttpConn c;
        if (url[0] && http_url_parse(url, &a) && http_conn_open(&c, &a, 5000)) {
            char path[400];
            snprintf(path, sizeof(path), "%s/social/ws?enc=json", a.base);
            if (ws_handshake(&c, &a, path)) {
                struct timeval none = {0, 0};
                setsockopt(c.fd, SOL_SOCKET, SO_RCVTIMEO, &none, sizeof(none));
                pthread_mutex_lock(&g_lock);
                g_ws_open = 1;
                pthread_mutex_unlock(&g_lock);
                ws_queue_in(NULL, "__open");
                backoff = WS_BACKOFF_MIN_MS;
                ws_session(&c, generation);
                pthread_mutex_lock(&g_lock);
                g_ws_open = 0;
                pthread_mutex_unlock(&g_lock);
                ws_queue_in(NULL, "__close");
            }
            http_conn_close(&c);
        }
        sleep_ms(backoff);
        backoff = backoff * 2 > WS_BACKOFF_MAX_MS ? WS_BACKOFF_MAX_MS : backoff * 2;
    }
    return NULL;
}

#else
// Windows: WinHTTP's WebSocket (backend_winhttp.c). A reader thread blocks on receive while
// this thread flushes the outgoing queue (WinHTTP allows one send and one receive at a time).
typedef struct {
    KnWinWs *ws;
    volatile LONG done;
} WinWsReader;

static void *ws_reader(void *arg) {
    WinWsReader *r = arg;
    for (;;) {
        char *msg = NULL;
        size_t len = 0;
        int text = 0;
        if (!kn_winws_recv(r->ws, &msg, &len, &text)) break;
        // binary frames are v3.1.1 msgpack (sent before the brain saw ?enc=json): ignored
        if (text) ws_queue_in(msg, NULL);
        else free(msg);
    }
    InterlockedExchange(&r->done, 1);
    return NULL;
}

static void ws_session_win(KnWinWs *ws, const unsigned generation) {
    WinWsReader reader = {ws, 0};
    pthread_t t;
    if (pthread_create(&t, NULL, ws_reader, &reader) != 0) return;
    for (;;) {
        pthread_mutex_lock(&g_lock);
        WsMsg *out = g_ws_out;
        g_ws_out = NULL;
        const int stale = generation != g_ws_generation;
        pthread_mutex_unlock(&g_lock);
        int ok = !stale;
        while (out) {
            WsMsg *next = out->next;
            if (ok) ok = kn_winws_send_text(ws, out->text, strlen(out->text));
            free(out->text);
            free(out);
            out = next;
        }
        if (!ok || reader.done) break;
        sleep_ms(30);
    }
    kn_winws_abort(ws);
    pthread_join(t, NULL);
}

static void *ws_thread(void *arg) {
    (void) arg;
    int backoff = WS_BACKOFF_MIN_MS;
    for (;;) {
        char ws_url[320];
        pthread_mutex_lock(&g_lock);
        backend_social_ws_url(ws_url, sizeof(ws_url));
        const unsigned generation = g_ws_generation;
        pthread_mutex_unlock(&g_lock);
        char err[192] = "";
        KnWinWs *ws = ws_url[0] ? kn_winws_open(ws_url, 5000, err, sizeof(err)) : NULL;
        if (ws) {
            pthread_mutex_lock(&g_lock);
            g_ws_open = 1;
            pthread_mutex_unlock(&g_lock);
            ws_queue_in(NULL, "__open");
            backoff = WS_BACKOFF_MIN_MS;
            ws_session_win(ws, generation);
            pthread_mutex_lock(&g_lock);
            g_ws_open = 0;
            pthread_mutex_unlock(&g_lock);
            ws_queue_in(NULL, "__close");
            kn_winws_free(ws);
        } else if (ws_url[0] && backoff == WS_BACKOFF_MIN_MS) {
            fprintf(stderr, "[backend] social socket %s: %s\n", ws_url, err);
        }
        sleep_ms(backoff);
        backoff = backoff * 2 > WS_BACKOFF_MAX_MS ? WS_BACKOFF_MAX_MS : backoff * 2;
    }
    return NULL;
}
#endif

void backend_social_open(void) {
    if (!backend_enabled() || !backend_social_allowed()) return;
    pthread_mutex_lock(&g_lock);
    g_ws_wanted = 1;
    if (!g_ws_thread_up) {
        pthread_t t;
        if (pthread_create(&t, NULL, ws_thread, NULL) == 0) {
            pthread_detach(t);
            g_ws_thread_up = 1;
        }
    }
    pthread_mutex_unlock(&g_lock);
}

int backend_social_is_open(void) {
    pthread_mutex_lock(&g_lock);
    const int open = g_ws_open;
    pthread_mutex_unlock(&g_lock);
    return open;
}

int backend_social_send_raw(const char *json) {
    if (!backend_enabled() || !json) return 0;
    backend_social_open();
    WsMsg *m = calloc(1, sizeof(*m));
    if (!m) return 0;
    m->text = strdup(json);
    if (!m->text) {
        free(m);
        return 0;
    }
    pthread_mutex_lock(&g_lock);
    QUEUE_PUSH(g_ws_out, m);
    pthread_mutex_unlock(&g_lock);
    return 1;
}

// ------------------------------------------------------------------ main thread

// The URL for a log line: the values of query parameters that carry secrets (krunker.io's seek-game:
// captchaToken, accessToken, validationToken; a socket's at=; any token=) shown as "(redacted)".
static char *redacted_url(const char *url) {
    static const char *const secret[] = {"captchaToken", "accessToken", "validationToken", "at", "token"};
    static const char MARK[] = "(redacted)";
    size_t parts = 1;
    for (const char *c = url; *c; c++) parts += *c == '&';
    char *out = malloc(strlen(url) + 1 + parts * sizeof(MARK)); // room for a mark per parameter
    if (!out) return NULL;
    const char *q = strchr(url, '?');
    if (!q) {
        strcpy(out, url);
        return out;
    }
    size_t o = (size_t) (q - url) + 1;
    memcpy(out, url, o);
    for (const char *p = q + 1; *p;) {
        const size_t part = strcspn(p, "&");
        const char *eq = memchr(p, '=', part);
        int hide = 0;
        for (size_t i = 0; eq && i < sizeof(secret) / sizeof(secret[0]); i++)
            hide |= (size_t) (eq - p) == strlen(secret[i]) && !strncmp(p, secret[i], (size_t) (eq - p));
        const size_t keep = hide ? (size_t) (eq - p) + 1 : part;
        memcpy(out + o, p, keep);
        o += keep;
        if (hide) {
            memcpy(out + o, MARK, sizeof(MARK) - 1);
            o += sizeof(MARK) - 1;
        }
        p += part;
        if (*p == '&') out[o++] = *p++;
    }
    out[o] = 0;
    return out;
}

void backend_poll(void) {
    pthread_mutex_lock(&g_lock);
    HttpJob *done = g_http_done;
    g_http_done = NULL;
    WsMsg *in = g_ws_in;
    g_ws_in = NULL;
    pthread_mutex_unlock(&g_lock);

    while (done) {
        HttpJob *next = done->next;
        // a parallel request whose TLS handshake failed says so (backend.h BACKEND_HTTP_TLS_FAILED); the rest see 0
        const int status = done->status ? done->status : done->tls_failed && done->tls_reply ? BACKEND_HTTP_TLS_FAILED : 0;
        if (!status) {
            // a transport error: say which request and why (silent before: an empty server
            // finder looked like "no servers"); the reason is also the callback's body
            if (!done->err[0]) snprintf(done->err, sizeof(done->err), "no answer");
            char *shown = redacted_url(done->url);
            fprintf(stderr, "[http] %s %s failed: %s\n", done->method, shown ? shown : "(url)", done->err);
            free(shown);
        }
        if (done->fn) {
            const char *body = status > 0 ? (done->body ? done->body : "") : done->err;
            done->fn(done->user, status, body, status > 0 ? done->length : strlen(done->err));
        }
        free(done->url);
        free(done->body);
        free(done);
        done = next;
    }
    while (in) {
        WsMsg *next = in->next;
        if (in->text) backend_dispatch_text(in->text);
        else if (in->event) backend_dispatch_event(in->event);
        free(in->text);
        free(in);
        in = next;
    }
}
#endif // !__EMSCRIPTEN__
