// Brain client, native transports (client/include/backend.h): one HTTP worker thread
// (blocking HTTP/1.1 GETs, "Connection: close") and one social socket thread (a small
// RFC 6455 client: JSON text frames, masked, ping/pong, reassembly). Both hand their
// results to the main thread through mutex-protected queues drained by backend_poll().
// http:// and ws://, plus https:// and wss:// when the build found OpenSSL
// (KRUNKNATIVE_HAVE_TLS: libssl, peer verified against the system CA store and the
// host name; the online-mode defaults https://coordinator.krunk.io need it).
#ifndef __EMSCRIPTEN__
#include <backend.h>
#include <cJSON.h>
#include "backend_internal.h"
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef WIN32
// No Windows transport yet: the backend stays disabled there.
void backend_init(const char *cli_url) { (void) cli_url; backend_store_url(""); }
void backend_set_url(const char *url) { (void) url; }
int backend_url_is_default(void) { return 0; }
int backend_http_get(const char *path, BackendHttpFn fn, void *user) { (void) path; (void) fn; (void) user; return 0; }
int backend_http_request(const char *method, const char *url, const char *json_body, BackendHttpFn fn, void *user) { (void) method; (void) url; (void) json_body; (void) fn; (void) user; return 0; }
void backend_poll(void) {}
void backend_social_open(void) {}
int backend_social_is_open(void) { return 0; }
int backend_social_send_text(const char *json) { (void) json; return 0; }
#else
#include <html_dom.h>
#include <online_mode.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#ifdef KRUNKNATIVE_HAVE_TLS
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#endif

#define HTTP_TIMEOUT_MS 8000
#define HTTP_MAX_BODY (4u * 1024u * 1024u)
#define WS_MAX_MESSAGE (2u * 1024u * 1024u)
#define WS_BACKOFF_MIN_MS 1000
#define WS_BACKOFF_MAX_MS 10000

// ------------------------------------------------------------------ small helpers

typedef struct {
    char host[128];
    char port[8];
    char base[128]; // path prefix, "" or "/prefix"
    int tls;        // https:// / wss://
} BrainAddr;

// http(s)://host[:port][/base] (also ws:// / wss://); TLS needs KRUNKNATIVE_HAVE_TLS
static int parse_url(const char *url, BrainAddr *a) {
    memset(a, 0, sizeof(*a));
    const char *p = url;
    if (!strncmp(p, "http://", 7)) p += 7;
    else if (!strncmp(p, "ws://", 5)) p += 5;
    else if (!strncmp(p, "https://", 8) || !strncmp(p, "wss://", 6)) {
#ifdef KRUNKNATIVE_HAVE_TLS
        p += p[0] == 'h' ? 8 : 6;
        a->tls = 1;
#else
        static int warned;
        if (!warned++) fprintf(stderr, "[backend] %s needs TLS, but this client was built without OpenSSL\n", url);
        return 0;
#endif
    }
    const char *host_end = p;
    while (*host_end && *host_end != ':' && *host_end != '/') host_end++;
    const size_t host_len = (size_t) (host_end - p);
    if (!host_len || host_len >= sizeof(a->host)) return 0;
    memcpy(a->host, p, host_len);
    p = host_end;
    if (*p == ':') {
        p++;
        size_t n = 0;
        while (*p >= '0' && *p <= '9' && n + 1 < sizeof(a->port)) a->port[n++] = *p++;
        a->port[n] = 0;
    }
    if (!a->port[0]) strcpy(a->port, a->tls ? "443" : "80");
    if (*p == '/') snprintf(a->base, sizeof(a->base), "%s", p);
    size_t n = strlen(a->base);
    while (n && a->base[n - 1] == '/') a->base[--n] = 0;
    return 1;
}

// The request target of an http(s) URL: everything from the first '/' after host[:port]
// ("" for none). Not BrainAddr.base: that one is char[128] and cut a signed-in seek-game
// (?game=...&token=<32 hex>) short by a character, so the brain saw a guest.
static const char *url_target(const char *url) {
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    while (*p && *p != '/') p++;
    return p;
}

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void sleep_ms(const int ms) {
    struct timespec ts = {ms / 1000, (long) (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

// Blocking connect with a timeout (non-blocking connect + poll), then blocking again.
static int connect_to(const BrainAddr *a, const int timeout_ms) {
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(a->host, a->port, &hints, &res) != 0 || !res) return -1;
    int fd = -1;
    for (struct addrinfo *ai = res; ai && fd < 0; ai = ai->ai_next) {
        fd = (int) socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) != 0) {
            close(fd);
            fd = -1;
        }
    }
    freeaddrinfo(res);
    if (fd >= 0) {
        const int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    }
    return fd;
}

static int send_all(const int fd, const void *data, size_t len) {
    const char *p = data;
    while (len) {
        const ssize_t w = send(fd, p, len, MSG_NOSIGNAL);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return 0;
        p += w;
        len -= (size_t) w;
    }
    return 1;
}

// ------------------------------------------------------------------ connection (plain or TLS)

typedef struct {
    int fd;
#ifdef KRUNKNATIVE_HAVE_TLS
    SSL *ssl;
#endif
} Conn;

#ifdef KRUNKNATIVE_HAVE_TLS
static SSL_CTX *g_tls_ctx;
static pthread_once_t g_tls_once = PTHREAD_ONCE_INIT;

static void tls_init_once(void) {
    g_tls_ctx = SSL_CTX_new(TLS_client_method());
    if (!g_tls_ctx) return;
    SSL_CTX_set_min_proto_version(g_tls_ctx, TLS1_2_VERSION);
    SSL_CTX_set_verify(g_tls_ctx, SSL_VERIFY_PEER, NULL);
    if (SSL_CTX_set_default_verify_paths(g_tls_ctx) != 1) fprintf(stderr, "[backend] TLS: no system CA store\n");
}
#endif

// Connects (and runs the TLS handshake, verifying the certificate for a->host). 0 on failure.
static int conn_open(Conn *c, const BrainAddr *a, const int timeout_ms) {
    memset(c, 0, sizeof(*c));
    c->fd = connect_to(a, timeout_ms);
    if (c->fd < 0) return 0;
    struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#ifdef KRUNKNATIVE_HAVE_TLS
    if (a->tls) {
        pthread_once(&g_tls_once, tls_init_once);
        c->ssl = g_tls_ctx ? SSL_new(g_tls_ctx) : NULL;
        if (!c->ssl || !SSL_set_fd(c->ssl, c->fd) || !SSL_set_tlsext_host_name(c->ssl, a->host) ||
            !SSL_set1_host(c->ssl, a->host) || SSL_connect(c->ssl) != 1) {
            const long verify = c->ssl ? SSL_get_verify_result(c->ssl) : X509_V_OK;
            fprintf(stderr, "[backend] TLS to %s failed%s%s\n", a->host, verify != X509_V_OK ? ": " : "",
                    verify != X509_V_OK ? X509_verify_cert_error_string(verify) : "");
            ERR_clear_error();
            if (c->ssl) SSL_free(c->ssl);
            c->ssl = NULL;
            close(c->fd);
            c->fd = -1;
            return 0;
        }
    }
#endif
    return 1;
}

static void conn_close(Conn *c) {
#ifdef KRUNKNATIVE_HAVE_TLS
    if (c->ssl) {
        SSL_shutdown(c->ssl);
        SSL_free(c->ssl);
        c->ssl = NULL;
    }
#endif
    if (c->fd >= 0) close(c->fd);
    c->fd = -1;
}

static int conn_send(Conn *c, const void *data, size_t len) {
#ifdef KRUNKNATIVE_HAVE_TLS
    if (c->ssl) {
        const char *p = data;
        while (len) {
            const int w = SSL_write(c->ssl, p, len > 1 << 30 ? 1 << 30 : (int) len);
            if (w <= 0) return 0;
            p += w;
            len -= (size_t) w;
        }
        return 1;
    }
#endif
    return send_all(c->fd, data, len);
}

// >0 bytes, 0 closed, <0 error (errno EAGAIN on a receive timeout)
static ssize_t conn_recv(Conn *c, void *buf, const size_t len) {
#ifdef KRUNKNATIVE_HAVE_TLS
    if (c->ssl) {
        const int r = SSL_read(c->ssl, buf, len > 1 << 30 ? 1 << 30 : (int) len);
        if (r > 0) return r;
        const int e = SSL_get_error(c->ssl, r);
        if (e == SSL_ERROR_ZERO_RETURN) return 0;
        if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) errno = EAGAIN;
        ERR_clear_error();
        return -1;
    }
#endif
    for (;;) {
        const ssize_t r = recv(c->fd, buf, len, 0);
        if (r < 0 && errno == EINTR) continue;
        return r;
    }
}

// Decrypted bytes waiting inside the TLS layer (poll() on the fd cannot see them).
static int conn_pending(const Conn *c) {
#ifdef KRUNKNATIVE_HAVE_TLS
    if (c->ssl) return SSL_pending(c->ssl) > 0;
#endif
    (void) c;
    return 0;
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
} HttpJob;

typedef struct WsMsg {
    struct WsMsg *next;
    char *text;     // NULL = event
    const char *event;
} WsMsg;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_http_cond = PTHREAD_COND_INITIALIZER;
static HttpJob *g_http_pending, *g_http_done;
static int g_http_thread_up;
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

static int g_url_default; // the URL is the local default (127.0.0.1:5050): probe it

// Online mode's account service (online_mode.h, ONLINE_DEFAULT_ACCOUNT_URL). The client
// never reads the backend's config/config.json (it holds the database credentials): a
// local backend reaches the client as KRUNKNATIVE_BRAIN_URL (the Makefile's run exports
// it). backend_startup passes "" in offline mode: no account service at all.
void backend_init(const char *cli_url) {
    if (cli_url && !cli_url[0]) {
        backend_store_url("");
        return;
    }
    const char *url = cli_url;
    if (!url || !url[0]) url = getenv("KRUNKNATIVE_BRAIN_URL");
    char saved[256] = "";
    if ((!url || !url[0]) && html_storage_get("knBrainUrl", saved, sizeof(saved)) > 0) url = saved; // client setting
    char local[256] = "";
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

static void http_run(HttpJob *job) {
    BrainAddr a;
    char *req_body = job->body;
    job->body = NULL;
    job->status = 0;
    if (!parse_url(job->url, &a)) {
        free(req_body);
        return;
    }
    Conn c;
    if (!conn_open(&c, &a, HTTP_TIMEOUT_MS)) {
        free(req_body);
        return;
    }
    // the whole path + query of the URL after host[:port]
    Bytes req = {0};
    char head[640];
    const size_t body_len = req_body ? strlen(req_body) : 0;
    const char *target = url_target(job->url);
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
    const int sent = req.data && conn_send(&c, req.data, req.len);
    free(req.data);
    if (!sent) {
        conn_close(&c);
        return;
    }

    Bytes resp = {0};
    char buf[8192];
    for (;;) {
        const ssize_t r = conn_recv(&c, buf, sizeof(buf));
        if (r <= 0) break;
        if (!bytes_push(&resp, buf, (size_t) r) || resp.len > HTTP_MAX_BODY) break;
    }
    conn_close(&c);
    if (!resp.data) return;

    int status = 0;
    char *header_end = strstr(resp.data, "\r\n\r\n");
    if (sscanf(resp.data, "HTTP/%*s %d", &status) != 1 || !header_end) {
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

static void *http_thread(void *arg) {
    (void) arg;
    for (;;) {
        pthread_mutex_lock(&g_lock);
        while (!g_http_pending) pthread_cond_wait(&g_http_cond, &g_lock);
        HttpJob *job = g_http_pending;
        g_http_pending = job->next;
        job->next = NULL;
        pthread_mutex_unlock(&g_lock);

        http_run(job);

        pthread_mutex_lock(&g_lock);
        QUEUE_PUSH(g_http_done, job);
        pthread_mutex_unlock(&g_lock);
    }
    return NULL;
}

static int http_queue(const char *method, const char *url, const char *body, const BackendHttpFn fn, void *user) {
    if (!url || !url[0]) return 0;
    HttpJob *job = calloc(1, sizeof(*job));
    if (!job) return 0;
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
    if (!g_http_thread_up) {
        pthread_t t;
        if (pthread_create(&t, NULL, http_thread, NULL) == 0) {
            pthread_detach(t);
            g_http_thread_up = 1;
        }
    }
    QUEUE_PUSH(g_http_pending, job);
    pthread_cond_signal(&g_http_cond);
    pthread_mutex_unlock(&g_lock);
    return g_http_thread_up;
}

int backend_http_get(const char *path, const BackendHttpFn fn, void *user) {
    if (!backend_enabled() || !path) return 0;
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

static int ws_send_frame(Conn *c, const int opcode, const char *data, const size_t len) {
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
    const int ok = conn_send(c, head, n) && conn_send(c, masked, len);
    free(masked);
    return ok;
}

static int ws_handshake(Conn *c, const BrainAddr *a, const char *path) {
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
    if (len <= 0 || (size_t) len >= sizeof(req) || !conn_send(c, req, (size_t) len)) return 0;
    // read the response head byte by byte (it is tiny), stop at the blank line
    char resp[2048];
    size_t n = 0;
    while (n + 1 < sizeof(resp)) {
        const ssize_t r = conn_recv(c, resp + n, 1);
        if (r <= 0) return 0;
        n++;
        if (n >= 4 && !memcmp(resp + n - 4, "\r\n\r\n", 4)) break;
    }
    resp[n] = 0;
    int status = 0;
    return sscanf(resp, "HTTP/%*s %d", &status) == 1 && status == 101;
}

// Reads exactly len bytes, polling `quit` conditions every 100 ms. 0 on failure.
static int recv_exact(Conn *c, void *out, size_t len) {
    char *p = out;
    while (len) {
        const ssize_t r = conn_recv(c, p, len);
        if (r < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (r <= 0) return 0;
        p += r;
        len -= (size_t) r;
    }
    return 1;
}

// One open session: returns when the socket drops or the URL changed.
static void ws_session(Conn *c, const unsigned generation) {
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
        const int ready = conn_pending(c) ? 1 : poll(&pfd, 1, 30);
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

        BrainAddr a;
        Conn c;
        if (url[0] && parse_url(url, &a) && conn_open(&c, &a, 5000)) {
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
            conn_close(&c);
        }
        sleep_ms(backoff);
        backoff = backoff * 2 > WS_BACKOFF_MAX_MS ? WS_BACKOFF_MAX_MS : backoff * 2;
    }
    return NULL;
}

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

int backend_social_send_text(const char *json) {
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

void backend_poll(void) {
    pthread_mutex_lock(&g_lock);
    HttpJob *done = g_http_done;
    g_http_done = NULL;
    WsMsg *in = g_ws_in;
    g_ws_in = NULL;
    pthread_mutex_unlock(&g_lock);

    while (done) {
        HttpJob *next = done->next;
        if (done->fn) done->fn(done->user, done->status, done->body ? done->body : "", done->length);
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
#endif // WIN32
#endif // !__EMSCRIPTEN__
