// Blocking HTTP(S) GET (launcher/include/kn_http.h).
#ifndef __EMSCRIPTEN__
#include <kn_http.h>
#include <kn_update.h> // kn_update_resolve_url (redirects)
#include <http_proxy.h> // the client's proxy setting / HTTPS_PROXY ... (per connection)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KN_HTTP_MAX_REDIRECTS 5

static char g_user_agent[128] = "KrunkNative";
// process-wide counters (kn_http_stats); the times in microseconds
static volatile int64_t g_stat_requests, g_stat_connects, g_stat_connect_us, g_stat_ttfb_us;
#define STAT_ADD(v) __atomic_add_fetch(&(v), 1, __ATOMIC_RELAXED)
#define STAT_ADD_N(v, n) __atomic_add_fetch(&(v), (int64_t) (n), __ATOMIC_RELAXED)

#ifdef _WIN32
#include <windows.h>
static int64_t now_us(void) {
    return (int64_t) GetTickCount64() * 1000;
}
#else
#include <time.h>
static int64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t) ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
#endif

void kn_http_stats(KnHttpStats *out) {
    out->requests = __atomic_load_n(&g_stat_requests, __ATOMIC_RELAXED);
    out->connects = __atomic_load_n(&g_stat_connects, __ATOMIC_RELAXED);
    out->connect_s = (double) __atomic_load_n(&g_stat_connect_us, __ATOMIC_RELAXED) / 1e6;
    out->ttfb_s = (double) __atomic_load_n(&g_stat_ttfb_us, __ATOMIC_RELAXED) / 1e6;
}

void kn_http_set_user_agent(const char *ua) {
    if (ua && *ua) snprintf(g_user_agent, sizeof(g_user_agent), "%s", ua);
}

static void set_err(char *err, const size_t errn, const char *fmt, const char *arg) {
    if (err && errn) snprintf(err, errn, fmt, arg ? arg : "");
}

typedef struct {
    char *data;
    size_t len, max;
    int overflow;
} MemSink;

static int mem_write(void *user, const void *data, const size_t len) {
    MemSink *m = user;
    if (m->len + len > m->max) {
        m->overflow = 1;
        return 0;
    }
    char *grown = realloc(m->data, m->len + len + 1);
    if (!grown) return 0;
    m->data = grown;
    memcpy(m->data + m->len, data, len);
    m->len += len;
    m->data[m->len] = 0;
    return 1;
}

int kn_http_get_mem(KnHttp *h, const char *url, const size_t max, char **body, size_t *len, char *err, const size_t errn) {
    return kn_http_get_mem_gz(h, url, max, body, len, NULL, err, errn);
}

int kn_http_get_mem_gz(KnHttp *h, const char *url, const size_t max, char **body, size_t *len, int *gzip, char *err,
                       const size_t errn) {
    MemSink m = {.max = max};
    KnHttpSink sink = {.write = mem_write, .user = &m, .accept_gzip = gzip != NULL};
    if (gzip) *gzip = 0;
    *body = NULL;
    if (len) *len = 0;
    const int status = kn_http_get(h, url, &sink, err, errn);
    if (m.overflow) {
        set_err(err, errn, "response larger than %s", "the limit");
        free(m.data);
        return 0;
    }
    if (status >= 200 && status < 300) {
        if (!m.data) m.data = calloc(1, 1); // empty body
        *body = m.data;
        if (len) *len = m.len;
        if (gzip) *gzip = sink.gzip;
    } else {
        free(m.data);
    }
    return status;
}

#ifdef _WIN32
// ------------------------------------------------------------------ WinHTTP
#include <windows.h>
#include <winhttp.h>

struct KnHttp {
    HINTERNET session;
    int timeout_ms;
};

KnHttp *kn_http_new(const int timeout_ms) {
    KnHttp *h = calloc(1, sizeof(*h));
    if (!h) return NULL;
    wchar_t ua[128];
    MultiByteToWideChar(CP_UTF8, 0, g_user_agent, -1, ua, 128);
    h->session = WinHttpOpen(ua, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!h->session) // before Windows 8.1: no automatic proxy
        h->session = WinHttpOpen(ua, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!h->session) {
        free(h);
        return NULL;
    }
    h->timeout_ms = timeout_ms > 0 ? timeout_ms : 15000;
    WinHttpSetTimeouts(h->session, h->timeout_ms, h->timeout_ms, h->timeout_ms, h->timeout_ms);
    return h;
}

void kn_http_free(KnHttp *h) {
    if (!h) return;
    if (h->session) WinHttpCloseHandle(h->session);
    free(h);
}

int kn_http_get(KnHttp *h, const char *url, KnHttpSink *sink, char *err, const size_t errn) {
    if (sink) {
        sink->content_length = -1;
        sink->gzip = 0;
    }
    if (!h || !url) return 0;
    wchar_t wurl[2048];
    if (!MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl, 2048)) {
        set_err(err, errn, "bad URL %s", url);
        return 0;
    }
    URL_COMPONENTS uc;
    memset(&uc, 0, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256], path[2048];
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2048;
    wchar_t extra[2048];
    uc.lpszExtraInfo = extra;
    uc.dwExtraInfoLength = 2048;
    if (!WinHttpCrackUrl(wurl, 0, 0, &uc)) {
        set_err(err, errn, "bad URL %s", url);
        return 0;
    }
    if (uc.dwExtraInfoLength && wcslen(path) + wcslen(extra) < 2048) wcscat(path, extra); // ?query
    HINTERNET conn = WinHttpConnect(h->session, host, uc.nPort, 0);
    if (!conn) {
        set_err(err, errn, "cannot connect to %s", url);
        return 0;
    }
    const DWORD flags = uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET req = WinHttpOpenRequest(conn, L"GET", path[0] ? path : L"/", NULL, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    char host8[256];
    if (req && WideCharToMultiByte(CP_UTF8, 0, host, -1, host8, sizeof(host8), NULL, NULL))
        http_proxy_winhttp_apply(req, host8, uc.nScheme == INTERNET_SCHEME_HTTPS);
    int status = 0;
    wchar_t range[64] = L"";
    if (sink && sink->range_from > 0) swprintf(range, 64, L"Range: bytes=%lld-\r\n", (long long) sink->range_from);
    STAT_ADD(g_stat_requests);
    const int64_t t_send = now_us();
    if (req && WinHttpSendRequest(req, range[0] ? range : WINHTTP_NO_ADDITIONAL_HEADERS, range[0] ? (DWORD) -1L : 0,
                                  WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(req, NULL)) {
        STAT_ADD_N(g_stat_ttfb_us, now_us() - t_send);
        DWORD code = 0, size = sizeof(code);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &code, &size, WINHTTP_NO_HEADER_INDEX);
        status = (int) code;
        wchar_t clen[32];
        size = sizeof(clen);
        if (sink && WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, clen, &size,
                                        WINHTTP_NO_HEADER_INDEX))
            sink->content_length = _wtoi64(clen);
        if (sink && status == 206) {
            wchar_t crange[96];
            size = sizeof(crange);
            long long start = -1;
            if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_RANGE, WINHTTP_HEADER_NAME_BY_INDEX, crange, &size,
                                     WINHTTP_NO_HEADER_INDEX) ||
                swscanf(crange, L"bytes %lld-", &start) != 1 || start != sink->range_from) {
                set_err(err, errn, "%sbad Content-Range", "");
                WinHttpCloseHandle(req);
                WinHttpCloseHandle(conn);
                return 0;
            }
        }
        if (sink) sink->status = status;
        if (status >= 200 && status < 300 && sink && sink->write) {
            char buf[65536];
            for (;;) {
                if (sink->cancel && __atomic_load_n(sink->cancel, __ATOMIC_RELAXED)) {
                    set_err(err, errn, "%scancelled", "");
                    status = 0;
                    break;
                }
                DWORD got = 0;
                if (!WinHttpReadData(req, buf, sizeof(buf), &got)) {
                    set_err(err, errn, "download of %s failed", url);
                    status = 0;
                    break;
                }
                if (!got) break;
                if (!sink->write(sink->user, buf, got)) {
                    set_err(err, errn, "%saborted", "");
                    status = 0;
                    break;
                }
            }
        }
    } else {
        char msg[96];
        snprintf(msg, sizeof(msg), "%s (WinHTTP error %lu)", "request failed", (unsigned long) GetLastError());
        set_err(err, errn, "%s", msg);
    }
    if (req) WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);
    return status;
}

// WinHTTP pools the connections of a session itself, but has no pipelining: one after another.
int kn_http_get_pipelined(KnHttp *h, KnHttpReq *reqs, const size_t n, const int depth, volatile int *cancel) {
    (void) depth;
    size_t done = 0;
    for (; done < n && !(cancel && __atomic_load_n(cancel, __ATOMIC_RELAXED)); done++) {
        reqs[done].err[0] = 0;
        reqs[done].status = kn_http_get(h, reqs[done].url, reqs[done].sink, reqs[done].err, sizeof(reqs[done].err));
        if (reqs[done].done) reqs[done].done(reqs[done].user, &reqs[done]);
        if (!reqs[done].status) {
            done++;
            break;
        }
    }
    return (int) done;
}

#else
// ------------------------------------------------------------------ POSIX (http_conn.c)
#include <errno.h>
#include <http_conn.h>
#include <strings.h>

struct KnHttp {
    HttpConn conn;
    int open;
    char key[200]; // host:port:tls:proxy generation of the open connection
    int timeout_ms;
    char buf[32768];
    size_t pos, len;
};

KnHttp *kn_http_new(const int timeout_ms) {
    KnHttp *h = calloc(1, sizeof(*h));
    if (!h) return NULL;
    h->conn.fd = -1;
    h->timeout_ms = timeout_ms > 0 ? timeout_ms : 15000;
    return h;
}

static void drop(KnHttp *h) {
    if (h->open) http_conn_close(&h->conn);
    h->open = 0;
    h->pos = h->len = 0;
}

void kn_http_free(KnHttp *h) {
    if (!h) return;
    drop(h);
    free(h);
}

// Next byte from the connection (buffered). -1 on EOF / error.
static int fill(KnHttp *h) {
    if (h->pos < h->len) return 1;
    const ssize_t r = http_conn_recv(&h->conn, h->buf, sizeof(h->buf));
    if (r <= 0) return 0;
    h->pos = 0;
    h->len = (size_t) r;
    return 1;
}

static int read_line(KnHttp *h, char *out, const size_t n) {
    size_t i = 0;
    for (;;) {
        if (!fill(h)) return 0;
        const char c = h->buf[h->pos++];
        if (c == '\n') break;
        if (i + 1 < n) out[i++] = c;
    }
    if (i && out[i - 1] == '\r') i--;
    out[i] = 0;
    return 1;
}

// Hands up to `want` body bytes to fn (NULL = discard). -1 transport error, 0 aborted, 1 ok.
static int read_body(KnHttp *h, int64_t want, KnHttpSink *sink, const int to_sink, const int until_close) {
    while (until_close || want > 0) {
        if (sink && sink->cancel && __atomic_load_n(sink->cancel, __ATOMIC_RELAXED)) return 0;
        if (!fill(h)) return until_close ? 1 : -1;
        size_t n = h->len - h->pos;
        if (!until_close && (int64_t) n > want) n = (size_t) want;
        if (to_sink && sink && sink->write && !sink->write(sink->user, h->buf + h->pos, n)) return 0;
        h->pos += n;
        want -= (int64_t) n;
    }
    return 1;
}

static int build_request(const HttpAddr *a, const char *url, const KnHttpSink *sink, char *req, const size_t n) {
    const char *target = http_url_target(url);
    const int default_port = !strcmp(a->port, a->tls ? "443" : "80");
    char range[48] = "";
    if (sink && sink->range_from > 0) snprintf(range, sizeof(range), "Range: bytes=%lld-\r\n", (long long) sink->range_from);
    // identity unless asked: the origins compress none of the content (measured: png/obj/mp3 come
    // back as-is for gzip/br/zstd), and the PNGs that are 83% of the bytes would not shrink anyway
    const int rl = snprintf(req, n,
                            "GET %s HTTP/1.1\r\nHost: %s%s%s\r\nUser-Agent: %s\r\nAccept-Encoding: %s\r\n"
                            "%sConnection: keep-alive\r\n\r\n",
                            target[0] ? target : "/", a->host, default_port ? "" : ":", default_port ? "" : a->port,
                            g_user_agent, sink && sink->accept_gzip ? "gzip" : "identity", range);
    return rl > 0 && (size_t) rl < n ? rl : 0;
}

// Opens the connection for a (or keeps the open one when it goes to the same place). 1 ok.
static int ensure_open(KnHttp *h, const HttpAddr *a, char *err, const size_t errn) {
    char key[sizeof(h->key)];
    // the proxy generation too: a connection opened before a proxy change is not reused
    snprintf(key, sizeof(key), "%s:%s:%d:%u", a->host, a->port, a->tls, http_proxy_generation());
    if (h->open && strcmp(key, h->key) != 0) drop(h);
    if (h->open) return 1;
    STAT_ADD(g_stat_connects);
    const int64_t t0 = now_us();
    const int opened = http_conn_open(&h->conn, a, h->timeout_ms);
    STAT_ADD_N(g_stat_connect_us, now_us() - t0);
    if (!opened) {
        set_err(err, errn, "cannot connect to %s", a->host);
        return 0;
    }
    h->open = 1;
    h->pos = h->len = 0;
    snprintf(h->key, sizeof(h->key), "%s", key);
    return 1;
}

// One response off the connection. >0 the status (the body went to the sink for a 2xx), 0 a
// failure after the status line (err says why; the connection is dropped), -1 no status line
// at all (the server closed a kept-alive connection: the request may be sent again).
static int read_response(KnHttp *h, const char *host, KnHttpSink *sink, char *location, const size_t location_n,
                         const int64_t t_send, char *err, const size_t errn) {
    char line[4096];
    if (location && location_n) location[0] = 0;
    if (!read_line(h, line, sizeof(line))) {
        drop(h);
        set_err(err, errn, "no answer from %s", host);
        return -1;
    }
    if (t_send) STAT_ADD_N(g_stat_ttfb_us, now_us() - t_send);
    int status = 0, minor = 1;
    if (sscanf(line, "HTTP/1.%d %d", &minor, &status) != 2 || status < 100) {
        drop(h);
        set_err(err, errn, "bad HTTP answer from %s", host);
        return 0;
    }
    int64_t length = -1, range_start = -1;
    int chunked = 0, close_after = minor == 0, gzip = 0;
    for (;;) {
        if (!read_line(h, line, sizeof(line))) {
            drop(h);
            set_err(err, errn, "truncated headers from %s", host);
            return 0;
        }
        if (!line[0]) break;
        char *colon = strchr(line, ':');
        if (!colon) continue;
        *colon = 0;
        const char *v = colon + 1;
        while (*v == ' ' || *v == '\t') v++;
        if (!strcasecmp(line, "content-length")) length = strtoll(v, NULL, 10);
        else if (!strcasecmp(line, "transfer-encoding") && strstr(v, "chunked")) chunked = 1;
        else if (!strcasecmp(line, "connection") && !strncasecmp(v, "close", 5)) close_after = 1;
        else if (!strcasecmp(line, "location") && location && location_n) snprintf(location, location_n, "%s", v);
        else if (!strcasecmp(line, "content-encoding")) gzip = !strncasecmp(v, "gzip", 4);
        else if (!strcasecmp(line, "content-range") && !strncasecmp(v, "bytes ", 6)) range_start = strtoll(v + 6, NULL, 10);
    }
    if (status == 206 && (!sink || sink->range_from <= 0 || range_start != sink->range_from)) {
        drop(h); // a range we did not ask for: neither the body nor the connection can be used
        set_err(err, errn, "unexpected partial answer from %s", host);
        return 0;
    }
    const int ok_status = status >= 200 && status < 300;
    if (sink) {
        sink->content_length = ok_status ? length : -1;
        sink->status = status;
        sink->gzip = gzip;
    }
    int r = 1;
    if (status == 204 || status == 304 || (status >= 100 && status < 200)) {
        r = 1;
    } else if (chunked) {
        for (;;) {
            if (!read_line(h, line, sizeof(line))) {
                r = -1;
                break;
            }
            const int64_t size = strtoll(line, NULL, 16);
            if (size <= 0) {
                while (read_line(h, line, sizeof(line)) && line[0]) {} // trailers
                break;
            }
            r = read_body(h, size, sink, ok_status, 0);
            if (r != 1 || !read_line(h, line, sizeof(line))) {
                if (r == 1) r = -1;
                break;
            }
        }
    } else if (length >= 0) {
        r = read_body(h, length, sink, ok_status, 0);
    } else {
        r = read_body(h, 0, sink, ok_status, 1);
        close_after = 1;
    }
    if (r != 1 || close_after) drop(h);
    if (r < 0) {
        set_err(err, errn, "connection to %s lost", host);
        return 0;
    }
    if (r == 0) {
        set_err(err, errn, "%s", sink && sink->cancel && __atomic_load_n(sink->cancel, __ATOMIC_RELAXED) ? "cancelled" : "aborted");
        return 0;
    }
    return status;
}

static int one_request(KnHttp *h, const char *url, KnHttpSink *sink, char *location, const size_t location_n,
                       char *err, const size_t errn) {
    HttpAddr a;
    if (!http_url_parse(url, &a)) {
        set_err(err, errn, "unsupported URL %s", url);
        return 0;
    }
    char req[2600];
    const int rl = build_request(&a, url, sink, req, sizeof(req));
    if (!rl) {
        set_err(err, errn, "URL too long: %s", url);
        return 0;
    }
    for (int attempt = 0;; attempt++) {
        const int reused = h->open;
        if (!ensure_open(h, &a, err, errn)) return 0;
        STAT_ADD(g_stat_requests);
        const int64_t t_send = now_us();
        int status = -1;
        if (http_conn_send(&h->conn, req, (size_t) rl)) {
            status = read_response(h, a.host, sink, location, location_n, t_send, err, errn);
        } else {
            drop(h);
        }
        if (status != -1) return status;
        // a kept-alive connection the server closed meanwhile: once more on a new one
        if (!reused || attempt) {
            set_err(err, errn, "no answer from %s", a.host);
            return 0;
        }
    }
}

int kn_http_get_pipelined(KnHttp *h, KnHttpReq *reqs, const size_t n, int depth, volatile int *cancel) {
    if (!h || !n) return 0;
    if (depth < 1) depth = 1;
    HttpAddr a;
    for (size_t i = 0; i < n; i++) {
        reqs[i].status = 0;
        reqs[i].err[0] = 0;
    }
    if (!http_url_parse(reqs[0].url, &a)) {
        snprintf(reqs[0].err, sizeof(reqs[0].err), "unsupported URL %s", reqs[0].url);
        return 0;
    }
    for (size_t i = 1; i < n; i++) { // one connection: every URL on the same host
        HttpAddr b;
        if (!http_url_parse(reqs[i].url, &b) || strcmp(a.host, b.host) || strcmp(a.port, b.port) || a.tls != b.tls)
            return 0;
    }
    int reused = h->open; // a kept-alive connection may have been closed while idle: one reconnect
    if (!ensure_open(h, &a, reqs[0].err, sizeof(reqs[0].err))) return 0;
    size_t sent = 0, done = 0;
    int64_t t_send[KN_HTTP_PIPELINE_MAX];
    if (depth > KN_HTTP_PIPELINE_MAX) depth = KN_HTTP_PIPELINE_MAX;
    while (done < n) {
        if (cancel && __atomic_load_n(cancel, __ATOMIC_RELAXED)) break;
        // keep `depth` requests in flight: small writes (~300 bytes each), never enough to fill
        // the socket buffers while the answers are not read yet
        while (h->open && sent < n && sent - done < (size_t) depth) {
            char req[2600];
            const int rl = build_request(&a, reqs[sent].url, reqs[sent].sink, req, sizeof(req));
            if (!rl) {
                snprintf(reqs[sent].err, sizeof(reqs[sent].err), "URL too long");
                break;
            }
            STAT_ADD(g_stat_requests);
            t_send[sent % KN_HTTP_PIPELINE_MAX] = now_us();
            if (!http_conn_send(&h->conn, req, (size_t) rl)) {
                drop(h);
                break;
            }
            sent++;
        }
        if (done == sent && done == 0 && reused && !h->open) { // the old connection refused the send
            reused = 0;
            if (!ensure_open(h, &a, reqs[0].err, sizeof(reqs[0].err))) break;
            continue;
        }
        if (done == sent) break; // nothing in flight: the connection is gone
        KnHttpReq *r = &reqs[done];
        if (r->sink) {
            r->sink->content_length = -1;
            r->sink->gzip = 0;
        }
        const int st = read_response(h, a.host, r->sink, NULL, 0, t_send[done % KN_HTTP_PIPELINE_MAX], r->err,
                                     sizeof(r->err));
        if (st < 0 && reused && done == 0) { // nothing served on the old connection: a fresh one
            reused = 0;
            sent = 0;
            if (!ensure_open(h, &a, reqs[0].err, sizeof(reqs[0].err))) break;
            continue;
        }
        reused = 0;
        if (st < 0) break; // closed before answering: this one and the rest were not served
        r->status = st;
        done++;
        if (r->done) r->done(r->user, r);
        if (!h->open) break; // Connection: close, or a failure mid-body: the rest were not served
    }
    if (done < n && h->open) drop(h); // answers still in flight: the connection is out of step
    return (int) done;
}

int kn_http_get(KnHttp *h, const char *url, KnHttpSink *sink, char *err, const size_t errn) {
    if (sink) {
        sink->content_length = -1;
        sink->gzip = 0;
    }
    if (!h || !url) return 0;
    char current[KN_UPDATE_URL_MAX * 2], location[KN_UPDATE_URL_MAX * 2];
    snprintf(current, sizeof(current), "%s", url);
    for (int hop = 0; hop <= KN_HTTP_MAX_REDIRECTS; hop++) {
        const int status = one_request(h, current, sink, location, sizeof(location), err, errn);
        const int redirect = status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
        if (!redirect || !location[0]) return status;
        char next[sizeof(current)];
        if (!kn_update_resolve_url(current, location, next, sizeof(next))) {
            set_err(err, errn, "bad redirect from %s", current);
            return 0;
        }
        snprintf(current, sizeof(current), "%s", next);
    }
    set_err(err, errn, "too many redirects for %s", url);
    return 0;
}
#endif
#endif
