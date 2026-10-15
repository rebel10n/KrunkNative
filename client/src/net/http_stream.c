// Streamed HTTP(S) requests (client/include/http_stream.h): native worker threads over
// http_conn.c (POSIX) or WinHTTP (Windows), the browser fetch() + ReadableStream on the web. The
// workers queue events (open / data / done) that http_stream_poll() delivers on the main thread.
#include <http_stream.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct HsEvent {
    struct HsEvent *next;
    int kind;              // HS_OPEN / HS_DATA / HS_DONE
    int status;
    char *data;            // data bytes / content type / error text
    size_t len;
} HsEvent;
enum { HS_OPEN, HS_DATA, HS_DONE };

#define HS_TIMEOUT_MS 180000 // connect + every receive: kn_brain pings an idle stream every 15 s

#ifdef __EMSCRIPTEN__
// ------------------------------------------------------------------ web: fetch()
#include <emscripten.h>

struct HttpStream {
    struct HttpStream *next;
    int id;
    int cancelled;
    int finished;          // HS_DONE queued
    HsEvent *events;
    HttpStreamCallbacks cb;
    void *user;
};

static HttpStream *g_streams;
static int g_next_id = 1;

static HttpStream *by_id(const int id) {
    for (HttpStream *s = g_streams; s; s = s->next) {
        if (s->id == id) return s;
    }
    return NULL;
}

static void push(HttpStream *s, const int kind, const int status, char *data, const size_t len) {
    HsEvent *e = calloc(1, sizeof(*e));
    if (!e) {
        free(data);
        return;
    }
    e->kind = kind;
    e->status = status;
    e->data = data;
    e->len = len;
    HsEvent **link = &s->events;
    while (*link) link = &(*link)->next;
    *link = e;
}

EMSCRIPTEN_KEEPALIVE void http_stream_web_event(const int id, const int kind, const int status, char *text) {
    HttpStream *s = by_id(id);
    if (!s || s->finished) {
        free(text);
        return;
    }
    if (kind == HS_DONE) s->finished = 1;
    push(s, kind, status, text, text ? strlen(text) : 0);
}

EM_JS(void, hs_web_fetch, (int id, const char *method, const char *url, const char *headers, const char *body), {
    var map = Module.knHttpStreams || (Module.knHttpStreams = {});
    var ctl = typeof AbortController !== 'undefined' ? new AbortController() : null;
    map[id] = ctl;
    var opts = { method: UTF8ToString(method), headers: { 'Accept': 'text/event-stream, application/json' } };
    if (ctl) opts.signal = ctl.signal;
    if (body) {
        opts.body = UTF8ToString(body);
        opts.headers['Content-Type'] = 'application/json';
    }
    if (headers) {
        UTF8ToString(headers).split('\r\n').forEach(function (line) {
            var i = line.indexOf(':');
            if (i > 0) opts.headers[line.slice(0, i).trim()] = line.slice(i + 1).trim();
        });
    }
    var send = function (kind, status, text) { _http_stream_web_event(id, kind, status, text !== null ? stringToNewUTF8(text) : 0); };
    fetch(UTF8ToString(url), opts).then(function (r) {
        send(0, r.status, r.headers.get("Content-Type") || "");
        if (!r.body || !r.body.getReader) {
            return r.text().then(function (t) { if (t) send(1, 0, t); send(2, r.status, null); });
        }
        var reader = r.body.getReader();
        var dec = new TextDecoder('utf-8');
        var pump = function () {
            return reader.read().then(function (res) {
                if (res.done) {
                    var tail = dec.decode();
                    if (tail) send(1, 0, tail);
                    send(2, r.status, null);
                    return;
                }
                var t = dec.decode(res.value, { stream: true });
                if (t) send(1, 0, t);
                return pump();
            });
        };
        return pump();
    }).catch(function (e) {
        send(2, 0, (e && e.name === 'AbortError') ? 'cancelled' : 'network error (' + (e && e.message ? e.message : 'fetch failed') + ')');
    }).finally(function () { delete map[id]; });
});

EM_JS(void, hs_web_abort, (int id), {
    var map = Module.knHttpStreams || {};
    if (map[id]) { try { map[id].abort(); } catch (e) {} delete map[id]; }
});

HttpStream *http_stream_start(const char *method, const char *url, const char *headers, const char *body, const HttpStreamCallbacks *cb,
                              void *user) {
    if (!url || !url[0] || !cb) return NULL;
    HttpStream *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->id = g_next_id++;
    s->cb = *cb;
    s->user = user;
    s->next = g_streams;
    g_streams = s;
    hs_web_fetch(s->id, method && method[0] ? method : "GET", url, headers, body);
    return s;
}

static void unlink_free(HttpStream *s) {
    for (HttpStream **l = &g_streams; *l; l = &(*l)->next) {
        if (*l == s) {
            *l = s->next;
            break;
        }
    }
    while (s->events) {
        HsEvent *n = s->events->next;
        free(s->events->data);
        free(s->events);
        s->events = n;
    }
    free(s);
}

void http_stream_cancel(HttpStream *s) {
    if (!s || s->cancelled) return;
    s->cancelled = 1;
    if (!s->finished) hs_web_abort(s->id); // the abort ends it with a done event: freed in http_stream_poll
}

int http_stream_active(void) {
    int n = 0;
    for (HttpStream *s = g_streams; s; s = s->next) n += !s->cancelled;
    return n;
}

void http_stream_poll(void) {
    HttpStream *s = g_streams;
    while (s) {
        HttpStream *next = s->next;
        int done = 0;
        HsEvent *events = s->events;
        s->events = NULL;
        while (events) {
            HsEvent *e = events;
            events = e->next;
            if (!s->cancelled) {
                if (e->kind == HS_OPEN && s->cb.on_open) s->cb.on_open(s->user, e->status, e->data ? e->data : "");
                else if (e->kind == HS_DATA && s->cb.on_data) s->cb.on_data(s->user, e->data ? e->data : "", e->len);
                else if (e->kind == HS_DONE && s->cb.on_done) s->cb.on_done(s->user, e->status, e->status ? NULL : (e->data ? e->data : "network error"));
            }
            done |= e->kind == HS_DONE;
            free(e->data);
            free(e);
        }
        if (done) unlink_free(s);
        s = next;
    }
}
#else
// ------------------------------------------------------------------ native: worker threads
#include <pthread.h>
#include <stdatomic.h>
#ifdef _WIN32
#include "backend_winhttp.h"
#else
#include <errno.h>
#include <http_conn.h>
#include <strings.h>
#include <sys/socket.h>
#endif

struct HttpStream {
    struct HttpStream *next;
    pthread_mutex_t mu;
    HsEvent *events;
    atomic_int cancelled;  // main thread: no more callbacks; the worker stops
    int finished;          // worker: HS_DONE queued (under mu)
    int fd;                // POSIX: the socket while the worker reads (under mu; -1 none)
    char *method, *url, *headers, *body;
    HttpStreamCallbacks cb;
    void *user;
};

static pthread_mutex_t g_list_mu = PTHREAD_MUTEX_INITIALIZER;
static HttpStream *g_streams;

static void push(HttpStream *s, const int kind, const int status, const char *data, const size_t len) {
    HsEvent *e = calloc(1, sizeof(*e));
    if (!e) return;
    e->kind = kind;
    e->status = status;
    if (data) {
        e->data = malloc(len + 1);
        if (e->data) {
            memcpy(e->data, data, len);
            e->data[len] = 0;
            e->len = len;
        }
    }
    pthread_mutex_lock(&s->mu);
    HsEvent **link = &s->events;
    while (*link) link = &(*link)->next;
    *link = e;
    if (kind == HS_DONE) s->finished = 1;
    pthread_mutex_unlock(&s->mu);
}

static void stream_free(HttpStream *s) {
    while (s->events) {
        HsEvent *n = s->events->next;
        free(s->events->data);
        free(s->events);
        s->events = n;
    }
    free(s->method);
    free(s->url);
    if (s->headers) memset(s->headers, 0, strlen(s->headers)); // may carry a key
    free(s->headers);
    if (s->body) memset(s->body, 0, strlen(s->body)); // may carry a password (krunker_account.c)
    free(s->body);
    pthread_mutex_destroy(&s->mu);
    free(s);
}

#ifdef _WIN32
static void win_head(void *user, const int status, const char *ctype) { push(user, HS_OPEN, status, ctype, strlen(ctype)); }
static int win_data(void *user, const char *data, const size_t len) {
    HttpStream *s = user;
    if (atomic_load(&s->cancelled)) return 0;
    push(s, HS_DATA, 0, data, len);
    return 1;
}

static void run(HttpStream *s) {
    char err[256] = "";
    const int status = kn_winhttp_stream(s->method, s->url, s->headers, s->body, HS_TIMEOUT_MS, win_head, win_data, s, err, sizeof(err));
    push(s, HS_DONE, status, status ? NULL : err, status ? 0 : strlen(err));
}
#else
// incremental chunked transfer-encoding decoder
typedef struct {
    int chunked;
    size_t left;           // bytes left in the current chunk
    int state;             // 0 size line, 1 data, 2 CRLF after data, 3 trailer / end
    char line[32];
    size_t line_len;
} Dechunk;

static void dechunk_feed(HttpStream *s, Dechunk *d, const char *p, size_t n) {
    if (!d->chunked) {
        if (n) push(s, HS_DATA, 0, p, n);
        return;
    }
    while (n) {
        if (d->state == 0) {
            const char c = *p++;
            n--;
            if (c == '\n') {
                d->line[d->line_len] = 0;
                d->left = strtoul(d->line, NULL, 16);
                d->line_len = 0;
                d->state = d->left ? 1 : 3;
            } else if (c != '\r' && d->line_len + 1 < sizeof(d->line)) {
                d->line[d->line_len++] = c;
            }
        } else if (d->state == 1) {
            const size_t take = n < d->left ? n : d->left;
            push(s, HS_DATA, 0, p, take);
            p += take;
            n -= take;
            d->left -= take;
            if (!d->left) d->state = 2;
        } else if (d->state == 2) {
            if (*p == '\n') d->state = 0;
            p++;
            n--;
        } else {
            return; // trailers: ignored
        }
    }
}

static void run(HttpStream *s) {
    HttpAddr a;
    char err[256];
    if (!http_url_parse(s->url, &a)) {
        snprintf(err, sizeof(err), "%s", strncmp(s->url, "https://", 8) ? "bad URL" : "https:// needs a client built with OpenSSL");
        push(s, HS_DONE, 0, err, strlen(err));
        return;
    }
    HttpConn c;
    if (!http_conn_open(&c, &a, HS_TIMEOUT_MS)) {
        snprintf(err, sizeof(err), "cannot connect to %s:%s%s", a.host, a.port, a.tls ? " (TLS)" : "");
        push(s, HS_DONE, 0, err, strlen(err));
        return;
    }
    pthread_mutex_lock(&s->mu);
    s->fd = c.fd;
    pthread_mutex_unlock(&s->mu);
    const char *target = http_url_target(s->url);
    const int default_port = !strcmp(a.port, a.tls ? "443" : "80");
    const size_t body_len = s->body ? strlen(s->body) : 0;
    const size_t cap = strlen(target) + (s->headers ? strlen(s->headers) : 0) + 512;
    char *req = malloc(cap);
    int ok = 0;
    if (req) {
        const int n = snprintf(req, cap,
                               "%s %s HTTP/1.1\r\nHost: %s%s%s\r\nUser-Agent: KrunkNative\r\nAccept: text/event-stream, application/json\r\n"
                               "Connection: close\r\n%s%s",
                               s->method, target[0] ? target : "/", a.host, default_port ? "" : ":", default_port ? "" : a.port,
                               s->headers ? s->headers : "", s->body ? "Content-Type: application/json\r\n" : "");
        char cl[64];
        const int m = s->body ? snprintf(cl, sizeof(cl), "Content-Length: %zu\r\n\r\n", body_len) : snprintf(cl, sizeof(cl), "\r\n");
        ok = n > 0 && http_conn_send(&c, req, (size_t) n) && http_conn_send(&c, cl, (size_t) m) && (!body_len || http_conn_send(&c, s->body, body_len));
        memset(req, 0, cap); // the headers may carry a key
        free(req);
    }
    if (!ok) {
        snprintf(err, sizeof(err), atomic_load(&s->cancelled) ? "cancelled" : "send to %s failed", a.host);
        goto fail;
    }
    // status line + headers
    char head[16384];
    size_t hl = 0;
    char *end = NULL;
    while (!end) {
        if (hl + 1 >= sizeof(head)) {
            snprintf(err, sizeof(err), "response headers too large");
            goto fail;
        }
        const ssize_t r = http_conn_recv(&c, head + hl, sizeof(head) - 1 - hl);
        if (r <= 0 || atomic_load(&s->cancelled)) {
            snprintf(err, sizeof(err), atomic_load(&s->cancelled) ? "cancelled" : r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) ? "timed out" : "connection closed");
            goto fail;
        }
        hl += (size_t) r;
        head[hl] = 0;
        end = strstr(head, "\r\n\r\n");
    }
    int status = 0;
    if (sscanf(head, "HTTP/%*s %d", &status) != 1) {
        snprintf(err, sizeof(err), "not an HTTP answer from %s", a.host);
        goto fail;
    }
    *end = 0;
    Dechunk d = {0};
    char ctype[128] = "";
    for (char *h = head; *h; h++) {
        if (h != head && h[-1] != '\n') continue;
        if (!strncasecmp(h, "transfer-encoding:", 18) && strstr(h, "chunked")) d.chunked = 1;
        if (!strncasecmp(h, "content-type:", 13)) {
            const char *v = h + 13;
            while (*v == ' ') v++;
            size_t k = 0;
            while (v[k] && v[k] != '\r' && v[k] != '\n' && k + 1 < sizeof(ctype)) {
                ctype[k] = v[k];
                k++;
            }
            ctype[k] = 0;
        }
    }
    push(s, HS_OPEN, status, ctype, strlen(ctype));
    const char *body = end + 4;
    dechunk_feed(s, &d, body, hl - (size_t) (body - head));
    char buf[16384];
    for (;;) {
        if (atomic_load(&s->cancelled)) {
            snprintf(err, sizeof(err), "cancelled");
            goto fail;
        }
        if (d.chunked && d.state == 3) break;
        const ssize_t r = http_conn_recv(&c, buf, sizeof(buf));
        if (r == 0) break;
        if (r < 0) {
            snprintf(err, sizeof(err), atomic_load(&s->cancelled) ? "cancelled" : errno == EAGAIN || errno == EWOULDBLOCK ? "timed out" : "connection reset");
            goto fail;
        }
        dechunk_feed(s, &d, buf, (size_t) r);
    }
    pthread_mutex_lock(&s->mu);
    s->fd = -1;
    pthread_mutex_unlock(&s->mu);
    http_conn_close(&c);
    push(s, HS_DONE, status, NULL, 0);
    return;
fail:
    pthread_mutex_lock(&s->mu);
    s->fd = -1;
    pthread_mutex_unlock(&s->mu);
    http_conn_close(&c);
    push(s, HS_DONE, 0, err, strlen(err));
}
#endif

static void *worker(void *arg) {
    run(arg);
    return NULL;
}

static char *dup_or_null(const char *s) {
    if (!s) return NULL;
    const size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

HttpStream *http_stream_start(const char *method, const char *url, const char *headers, const char *body, const HttpStreamCallbacks *cb,
                              void *user) {
    if (!url || !url[0] || !cb) return NULL;
#if !defined(_WIN32) && !defined(KRUNKNATIVE_HAVE_TLS)
    if (!strncmp(url, "https://", 8)) return NULL;
#endif
    HttpStream *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    pthread_mutex_init(&s->mu, NULL);
    atomic_init(&s->cancelled, 0);
    s->fd = -1;
    s->method = dup_or_null(method && method[0] ? method : "GET");
    s->url = dup_or_null(url);
    s->headers = dup_or_null(headers);
    s->body = dup_or_null(body);
    s->cb = *cb;
    s->user = user;
    if (!s->method || !s->url || (headers && !s->headers) || (body && !s->body)) {
        stream_free(s);
        return NULL;
    }
    pthread_mutex_lock(&g_list_mu);
    s->next = g_streams;
    g_streams = s;
    pthread_mutex_unlock(&g_list_mu);
    pthread_t t;
    if (pthread_create(&t, NULL, worker, s) != 0) {
        pthread_mutex_lock(&g_list_mu);
        g_streams = s->next;
        pthread_mutex_unlock(&g_list_mu);
        stream_free(s);
        return NULL;
    }
    pthread_detach(t);
    return s;
}

void http_stream_cancel(HttpStream *s) {
    if (!s) return;
    atomic_store(&s->cancelled, 1);
#ifndef _WIN32
    pthread_mutex_lock(&s->mu);
    if (s->fd >= 0) shutdown(s->fd, SHUT_RDWR); // unblocks the worker's recv
    pthread_mutex_unlock(&s->mu);
#endif
    // freed by http_stream_poll once the worker has queued its HS_DONE
}

int http_stream_active(void) {
    int n = 0;
    pthread_mutex_lock(&g_list_mu);
    for (HttpStream *s = g_streams; s; s = s->next) n += !atomic_load(&s->cancelled);
    pthread_mutex_unlock(&g_list_mu);
    return n;
}

void http_stream_poll(void) {
    pthread_mutex_lock(&g_list_mu);
    HttpStream *s = g_streams;
    pthread_mutex_unlock(&g_list_mu);
    while (s) {
        pthread_mutex_lock(&g_list_mu);
        HttpStream *next = s->next;
        pthread_mutex_unlock(&g_list_mu);
        pthread_mutex_lock(&s->mu);
        HsEvent *events = s->events;
        s->events = NULL;
        const int finished = s->finished;
        pthread_mutex_unlock(&s->mu);
        int done = 0;
        while (events) {
            HsEvent *e = events;
            events = e->next;
            if (!atomic_load(&s->cancelled)) {
                if (e->kind == HS_OPEN && s->cb.on_open) s->cb.on_open(s->user, e->status, e->data ? e->data : "");
                else if (e->kind == HS_DATA && s->cb.on_data) s->cb.on_data(s->user, e->data ? e->data : "", e->len);
                else if (e->kind == HS_DONE && s->cb.on_done) s->cb.on_done(s->user, e->status, e->status ? NULL : (e->data ? e->data : "network error"));
            }
            done |= e->kind == HS_DONE;
            free(e->data);
            free(e);
        }
        if (done || (finished && atomic_load(&s->cancelled))) {
            // the worker has returned (HS_DONE is its last act): unlink and free
            pthread_mutex_lock(&g_list_mu);
            for (HttpStream **l = &g_streams; *l; l = &(*l)->next) {
                if (*l == s) {
                    *l = s->next;
                    break;
                }
            }
            pthread_mutex_unlock(&g_list_mu);
            stream_free(s);
        }
        s = next;
    }
}
#endif
