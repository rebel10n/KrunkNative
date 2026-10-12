// Blocking HTTP(S) GET (launcher/include/kn_http.h).
#ifndef __EMSCRIPTEN__
#include <kn_http.h>
#include <kn_update.h> // kn_update_resolve_url (redirects)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KN_HTTP_MAX_REDIRECTS 5

static char g_user_agent[128] = "KrunkNative";

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
    MemSink m = {.max = max};
    KnHttpSink sink = {.write = mem_write, .user = &m};
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
    if (sink) sink->content_length = -1;
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
    int status = 0;
    if (req && WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(req, NULL)) {
        DWORD code = 0, size = sizeof(code);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &code, &size, WINHTTP_NO_HEADER_INDEX);
        status = (int) code;
        wchar_t clen[32];
        size = sizeof(clen);
        if (sink && WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, clen, &size,
                                        WINHTTP_NO_HEADER_INDEX))
            sink->content_length = _wtoi64(clen);
        if (status >= 200 && status < 300 && sink && sink->write) {
            char buf[65536];
            for (;;) {
                if (sink->cancel && *sink->cancel) {
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

#else
// ------------------------------------------------------------------ POSIX (http_conn.c)
#include <errno.h>
#include <http_conn.h>
#include <strings.h>

struct KnHttp {
    HttpConn conn;
    int open;
    char key[200]; // host:port:tls of the open connection
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
        if (sink && sink->cancel && *sink->cancel) return 0;
        if (!fill(h)) return until_close ? 1 : -1;
        size_t n = h->len - h->pos;
        if (!until_close && (int64_t) n > want) n = (size_t) want;
        if (to_sink && sink && sink->write && !sink->write(sink->user, h->buf + h->pos, n)) return 0;
        h->pos += n;
        want -= (int64_t) n;
    }
    return 1;
}

static int one_request(KnHttp *h, const char *url, KnHttpSink *sink, char *location, const size_t location_n,
                       char *err, const size_t errn) {
    HttpAddr a;
    if (!http_url_parse(url, &a)) {
        set_err(err, errn, "unsupported URL %s", url);
        return 0;
    }
    char key[sizeof(h->key)];
    snprintf(key, sizeof(key), "%s:%s:%d", a.host, a.port, a.tls);
    if (h->open && strcmp(key, h->key) != 0) drop(h);
    const char *target = http_url_target(url);
    const int default_port = !strcmp(a.port, a.tls ? "443" : "80");
    char req[2600];
    const int rl = snprintf(req, sizeof(req),
                            "GET %s HTTP/1.1\r\nHost: %s%s%s\r\nUser-Agent: %s\r\nAccept-Encoding: identity\r\n"
                            "Connection: keep-alive\r\n\r\n",
                            target[0] ? target : "/", a.host, default_port ? "" : ":", default_port ? "" : a.port,
                            g_user_agent);
    if (rl <= 0 || (size_t) rl >= sizeof(req)) {
        set_err(err, errn, "URL too long: %s", url);
        return 0;
    }
    char line[4096];
    int reused = h->open;
    for (int attempt = 0;; attempt++) {
        if (!h->open) {
            if (!http_conn_open(&h->conn, &a, h->timeout_ms)) {
                set_err(err, errn, "cannot connect to %s", a.host);
                return 0;
            }
            h->open = 1;
            h->pos = h->len = 0;
            snprintf(h->key, sizeof(h->key), "%s", key);
        }
        if (http_conn_send(&h->conn, req, (size_t) rl) && read_line(h, line, sizeof(line))) break;
        drop(h); // a kept-alive connection the server closed meanwhile: once more on a new one
        if (!reused || attempt) {
            set_err(err, errn, "no answer from %s", a.host);
            return 0;
        }
        reused = 0;
    }
    int status = 0, minor = 1;
    if (sscanf(line, "HTTP/1.%d %d", &minor, &status) != 2 || status < 100) {
        drop(h);
        set_err(err, errn, "bad HTTP answer from %s", a.host);
        return 0;
    }
    int64_t length = -1;
    int chunked = 0, close_after = minor == 0;
    if (location && location_n) location[0] = 0;
    for (;;) {
        if (!read_line(h, line, sizeof(line))) {
            drop(h);
            set_err(err, errn, "truncated headers from %s", a.host);
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
    }
    const int ok_status = status >= 200 && status < 300;
    if (sink) sink->content_length = ok_status ? length : -1;
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
        set_err(err, errn, "connection to %s lost", a.host);
        return 0;
    }
    if (r == 0) {
        set_err(err, errn, "%s", sink && sink->cancel && *sink->cancel ? "cancelled" : "aborted");
        return 0;
    }
    return status;
}

int kn_http_get(KnHttp *h, const char *url, KnHttpSink *sink, char *err, const size_t errn) {
    if (sink) sink->content_length = -1;
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
