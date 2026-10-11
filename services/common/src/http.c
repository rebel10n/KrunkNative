/*
 * knsvc/http.h server implementation.
 *
 * One kn_http_conn per socket. Requests are parsed from conn->in with
 * kn_http_parse_request and handled one at a time (pipelined requests wait in
 * conn->in). A request object (req_impl) outlives its connection if the
 * handler answers asynchronously; it is freed by the respond call.
 * Connections are only freed at the end of the outermost event callback
 * (conn->busy) so handlers can respond/close re-entrantly.
 */
#include "http_internal.h"
#include "knsvc/log.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MAX_PARAMS 8
#define SWEEP_MS 1000
#define READ_CHUNK 16384

typedef struct route {
    char *method;           /* NULL = any */
    char *pattern;
    kn_http_handler handler;
    void *ud;
} route;

typedef struct kn_http_conn kn_http_conn;
typedef struct req_impl req_impl;

struct kn_http_conn {
    kn_http_server *srv;
    int fd;
    kn_buf in, out;
    req_impl *req;          /* active request */
    uint64_t last_activity;
    uint64_t request_start; /* first byte of a pending request, 0 if none */
    int served;
    int busy;               /* callback depth */
    int dead;               /* close requested */
    int close_after_write;
    int peer_closed;        /* read EOF */
    int want_write;
    int sent_continue;      /* "100 Continue" sent for the pending request */
    char remote[64];
    kn_http_conn *prev, *next;
};

struct req_impl {
    kn_http_req pub;        /* first: kn_http_req* <-> req_impl* */
    kn_http_parsed parsed;
    kn_http_conn *conn;     /* NULL once the client is gone */
    kn_buf res_headers;
    char remote[64];
    char param_names[MAX_PARAMS][32];
    char *param_values[MAX_PARAMS];
    int nparams;
    int keep_alive;
    int streaming;
    int head;               /* HEAD request: no body */
};

struct kn_http_server {
    kn_loop *loop;
    kn_http_config cfg;
    char *host;
    char *server_name;
    char **cors;
    size_t ncors;
    int listen_fd;
    int port;
    route *routes;
    size_t nroutes;
    kn_http_handler fallback;
    void *fallback_ud;
    kn_http_conn *conns;
    int nconns;
    kn_timer_id sweep;
};

static void conn_on_io(kn_loop *loop, int fd, unsigned ev, void *ud);
static void conn_process(kn_http_conn *c);

/* ------------------------------------------------------------------ status */

const char *kn_http_status_text(int s) {
    switch (s) {
    case 100: return "Continue";
    case 101: return "Switching Protocols";
    case 200: return "OK";
    case 201: return "Created";
    case 202: return "Accepted";
    case 204: return "No Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 410: return "Gone";
    case 413: return "Content Too Large";
    case 415: return "Unsupported Media Type";
    case 422: return "Unprocessable Content";
    case 426: return "Upgrade Required";
    case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    case 505: return "HTTP Version Not Supported";
    default: return s < 300 ? "OK" : s < 400 ? "Redirect" : s < 500 ? "Client Error" : "Server Error";
    }
}

/* ------------------------------------------------------------------ config */

void kn_http_config_defaults(kn_http_config *c) {
    if (!c->host) c->host = "0.0.0.0";
    if (!c->max_header_bytes) c->max_header_bytes = 16384;
    if (!c->max_body_bytes) c->max_body_bytes = 1024 * 1024;
    if (!c->max_headers) c->max_headers = 64;
    if (!c->keepalive_timeout_ms) c->keepalive_timeout_ms = 15000;
    if (!c->request_timeout_ms) c->request_timeout_ms = 30000;
    if (!c->max_connections) c->max_connections = 4096;
    if (!c->server_name) c->server_name = "knsvc";
}

/* ------------------------------------------------------------------ connection basics */

static void conn_free(kn_http_conn *c) {
    kn_http_server *s = c->srv;
    if (c->fd >= 0) {
        kn_loop_del_fd(s->loop, c->fd);
        close(c->fd);
    }
    if (c->req) { /* async request outlives the connection */
        c->req->conn = NULL;
        c->req = NULL;
    }
    if (c->prev) c->prev->next = c->next;
    else s->conns = c->next;
    if (c->next) c->next->prev = c->prev;
    s->nconns--;
    kn_buf_free(&c->in);
    kn_buf_free(&c->out);
    free(c);
}

/* Free now if no callback is running on c, else at the end of it. */
static void conn_close(kn_http_conn *c) {
    c->dead = 1;
    if (!c->busy) conn_free(c);
}

static void conn_update_events(kn_http_conn *c) {
    unsigned ev = KN_EV_READ | (c->out.len ? KN_EV_WRITE : 0u);
    if (c->peer_closed) ev &= ~(unsigned)KN_EV_READ;
    kn_loop_mod_fd(c->srv->loop, c->fd, ev);
}

/* Write as much of c->out as possible. Returns -1 on a fatal error. */
static int conn_flush(kn_http_conn *c) {
    while (c->out.len) {
        ssize_t n = send(c->fd, c->out.data, c->out.len, MSG_NOSIGNAL);
        if (n > 0) {
            kn_buf_consume(&c->out, (size_t)n);
            c->last_activity = kn_now_ms();
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        return -1;
    }
    conn_update_events(c);
    return 0;
}

/* After a response is fully queued: flush, then close or continue with the next request. */
static void conn_after_response(kn_http_conn *c) {
    if (conn_flush(c) != 0) {
        conn_close(c);
        return;
    }
    if (c->close_after_write && !c->out.len) {
        shutdown(c->fd, SHUT_WR);
        conn_close(c);
        return;
    }
    if (!c->close_after_write && !c->dead) conn_process(c);
}

/* ------------------------------------------------------------------ request objects */

static void req_free(req_impl *r) {
    kn_http_parsed_free(&r->parsed);
    kn_buf_free(&r->res_headers);
    for (int i = 0; i < r->nparams; i++) free(r->param_values[i]);
    free(r);
}

/* Detach r from its connection (response done). */
static kn_http_conn *req_finish(req_impl *r) {
    kn_http_conn *c = r->conn;
    if (c) {
        c->req = NULL;
        c->served++;
        c->last_activity = kn_now_ms();
    }
    req_free(r);
    return c;
}

int kn_http_req_alive(const kn_http_req *req) {
    const req_impl *r = (const req_impl *)req;
    return r->conn != NULL && !r->conn->dead;
}

const char *kn_http_header_get(const kn_http_req *req, const char *name) {
    for (size_t i = 0; i < req->header_count; i++)
        if (strcasecmp(req->headers[i].name, name) == 0) return req->headers[i].value;
    return NULL;
}

const char *kn_http_param(const kn_http_req *req, const char *name) {
    const req_impl *r = (const req_impl *)req;
    for (int i = 0; i < r->nparams; i++)
        if (strcmp(r->param_names[i], name) == 0) return r->param_values[i];
    return NULL;
}

int kn_http_query_get(const kn_http_req *req, const char *key, char *out, size_t cap) {
    if (cap) out[0] = 0;
    size_t kl = strlen(key);
    for (const char *p = req->query; p && *p;) {
        const char *amp = strchr(p, '&');
        size_t seg = amp ? (size_t)(amp - p) : strlen(p);
        const char *eq = memchr(p, '=', seg);
        size_t nl = eq ? (size_t)(eq - p) : seg;
        char name[256];
        int dn = nl < sizeof name ? kn_url_decode(p, nl, name, sizeof name, 1) : -1;
        if (dn >= 0 && (size_t)dn == kl && memcmp(name, key, kl) == 0) {
            if (!eq) return 0;
            int n = kn_url_decode(eq + 1, seg - nl - 1, out, cap, 1);
            if (n < 0 && cap) out[0] = 0;
            return n < 0 ? -2 : n;
        }
        p = amp ? amp + 1 : p + seg;
    }
    return -1;
}

cJSON *kn_http_body_json(const kn_http_req *req) {
    if (!req->body_len) return NULL;
    return cJSON_ParseWithLength(req->body, req->body_len);
}

int kn_http_is_ws_upgrade(const kn_http_req *req) {
    const char *up = kn_http_header_get(req, "Upgrade");
    const char *conn = kn_http_header_get(req, "Connection");
    if (!up || !conn || strcasecmp(req->method, "GET") != 0) return 0;
    if (strcasecmp(up, "websocket") != 0) return 0;
    /* Connection is a token list ("keep-alive, Upgrade") */
    for (const char *p = conn; *p;) {
        while (*p == ' ' || *p == ',') p++;
        size_t n = strcspn(p, ", ");
        if (n == 7 && strncasecmp(p, "upgrade", 7) == 0) return 1;
        p += n;
    }
    return 0;
}

const char *kn_http_bearer(const kn_http_req *req) {
    const char *a = kn_http_header_get(req, "Authorization");
    if (!a || strncasecmp(a, "Bearer ", 7) != 0) return NULL;
    a += 7;
    while (*a == ' ') a++;
    return *a ? a : NULL;
}

/* ------------------------------------------------------------------ responses */

int kn_http_set_header(kn_http_req *req, const char *name, const char *value) {
    req_impl *r = (req_impl *)req;
    if (strpbrk(name, "\r\n:") || strpbrk(value, "\r\n")) return -1; /* header injection */
    return kn_buf_appendf(&r->res_headers, "%s: %s\r\n", name, value);
}

static int origin_allowed(const kn_http_server *s, const char *origin, int *wildcard) {
    *wildcard = 0;
    for (size_t i = 0; i < s->ncors; i++) {
        if (strcmp(s->cors[i], "*") == 0) {
            *wildcard = 1;
            return 1;
        }
        if (strcasecmp(s->cors[i], origin) == 0) return 1;
    }
    return 0;
}

static void append_cors(kn_buf *b, const kn_http_server *s, const kn_http_req *req) {
    if (!s || !s->ncors) return;
    const char *origin = kn_http_header_get(req, "Origin");
    int wildcard;
    if (!origin || !origin_allowed(s, origin, &wildcard)) return;
    if (wildcard && !s->cfg.cors_allow_credentials) {
        kn_buf_append_str(b, "Access-Control-Allow-Origin: *\r\n");
        return;
    }
    kn_buf_appendf(b, "Access-Control-Allow-Origin: %s\r\nVary: Origin\r\n", origin);
    if (s->cfg.cors_allow_credentials) kn_buf_append_str(b, "Access-Control-Allow-Credentials: true\r\n");
}

static void append_date(kn_buf *b) {
    char date[64];
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    strftime(date, sizeof date, "%a, %d %b %Y %H:%M:%S GMT", &tm);
    kn_buf_appendf(b, "Date: %s\r\n", date);
}

/* Status line + common headers into c->out. */
static void write_head(kn_http_conn *c, req_impl *r, int status, const char *ctype, long long length) {
    kn_buf *o = &c->out;
    int minor = r->parsed.version_minor;
    kn_buf_appendf(o, "HTTP/1.%d %d %s\r\n", minor ? 1 : 0, status, kn_http_status_text(status));
    kn_buf_appendf(o, "Server: %s\r\n", c->srv->server_name);
    append_date(o);
    if (ctype && status != 204 && status != 304) kn_buf_appendf(o, "Content-Type: %s\r\n", ctype);
    if (length >= 0 && status != 204 && status != 304) kn_buf_appendf(o, "Content-Length: %lld\r\n", length);
    else if (length == -1) kn_buf_append_str(o, "Transfer-Encoding: chunked\r\n");
    append_cors(o, c->srv, &r->pub);
    int max = c->srv->cfg.max_requests_per_conn;
    if (max > 0 && c->served + 1 >= max) r->keep_alive = 0;
    if (c->peer_closed) r->keep_alive = 0;
    if (!r->keep_alive) kn_buf_append_str(o, "Connection: close\r\n");
    else if (minor == 0) kn_buf_append_str(o, "Connection: keep-alive\r\n");
    if (r->res_headers.len) kn_buf_append(o, r->res_headers.data, r->res_headers.len);
    kn_buf_append_str(o, "\r\n");
}

void kn_http_respond(kn_http_req *req, int status, const char *ctype, const void *body, size_t len) {
    req_impl *r = (req_impl *)req;
    if (r->streaming) return;
    kn_http_conn *c = r->conn;
    if (!c || c->dead) {
        req_finish(r);
        return;
    }
    write_head(c, r, status, ctype, (long long)len);
    if (!r->head && len && status != 204 && status != 304) kn_buf_append(&c->out, body, len);
    if (!r->keep_alive) c->close_after_write = 1;
    req_finish(r);
    c->busy++;
    conn_after_response(c);
    c->busy--;
    if (c->dead && !c->busy) conn_free(c);
}

void kn_http_respond_text(kn_http_req *req, int status, const char *text) {
    kn_http_respond(req, status, "text/plain; charset=utf-8", text, strlen(text));
}

void kn_http_respond_json_str(kn_http_req *req, int status, const char *json) {
    kn_http_respond(req, status, "application/json; charset=utf-8", json, strlen(json));
}

void kn_http_respond_json(kn_http_req *req, int status, const cJSON *json) {
    char *s = json ? cJSON_PrintUnformatted(json) : NULL;
    kn_http_respond_json_str(req, s ? status : 500, s ? s : "{\"error\":\"serialization failed\"}");
    free(s);
}

void kn_http_respond_error(kn_http_req *req, int status, const char *message) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "error", message ? message : kn_http_status_text(status));
    kn_http_respond_json(req, status, o);
    cJSON_Delete(o);
}

int kn_http_stream_begin(kn_http_req *req, int status, const char *ctype) {
    req_impl *r = (req_impl *)req;
    if (r->streaming) return -1;
    r->streaming = 1;
    kn_http_conn *c = r->conn;
    if (!c || c->dead) return -1;
    if (r->parsed.version_minor == 0) r->keep_alive = 0; /* no chunked in 1.0: close-delimited */
    if (r->parsed.version_minor == 0) {
        r->keep_alive = 0;
        write_head(c, r, status, ctype, -2);
        /* -2: neither length nor chunked; body ends with the connection */
    } else {
        write_head(c, r, status, ctype, -1);
    }
    return conn_flush(c);
}

int kn_http_stream_write(kn_http_req *req, const void *data, size_t len) {
    req_impl *r = (req_impl *)req;
    kn_http_conn *c = r->conn;
    if (!r->streaming || !c || c->dead) return -1;
    if (!len || r->head) return 0;
    if (r->parsed.version_minor == 0) kn_buf_append(&c->out, data, len);
    else {
        kn_buf_appendf(&c->out, "%zx\r\n", len);
        kn_buf_append(&c->out, data, len);
        kn_buf_append_str(&c->out, "\r\n");
    }
    return conn_flush(c);
}

void kn_http_stream_end(kn_http_req *req) {
    req_impl *r = (req_impl *)req;
    kn_http_conn *c = r->conn;
    if (!c || c->dead) {
        req_finish(r);
        return;
    }
    if (r->parsed.version_minor > 0 && !r->head) kn_buf_append_str(&c->out, "0\r\n\r\n");
    if (!r->keep_alive) c->close_after_write = 1;
    req_finish(r);
    c->busy++;
    conn_after_response(c);
    c->busy--;
    if (c->dead && !c->busy) conn_free(c);
}

/* ------------------------------------------------------------------ routing */

int kn_http_route(kn_http_server *s, const char *method, const char *pattern, kn_http_handler h, void *ud) {
    route *n = realloc(s->routes, (s->nroutes + 1) * sizeof *n);
    if (!n) return -1;
    s->routes = n;
    route *r = &s->routes[s->nroutes];
    r->method = method ? strdup(method) : NULL;
    r->pattern = strdup(pattern);
    r->handler = h;
    r->ud = ud;
    if (!r->pattern || (method && !r->method)) return -1;
    s->nroutes++;
    return 0;
}

void kn_http_set_fallback(kn_http_server *s, kn_http_handler h, void *ud) {
    s->fallback = h;
    s->fallback_ud = ud;
}

/* Match raw path (percent-encoded, no query) against pattern; fills params. */
static int route_match(const char *pattern, const char *raw, req_impl *r) {
    const char *p = pattern, *u = raw;
    int np = 0;
    char *vals[MAX_PARAMS];
    const char *names[MAX_PARAMS];
    while (*p) {
        if (*p == '*' && p[1] == 0) {
            if (np < MAX_PARAMS) {
                size_t n = strlen(u);
                vals[np] = malloc(n + 1);
                if (vals[np] && kn_url_decode(u, n, vals[np], n + 1, 0) >= 0) names[np++] = "*";
                else free(vals[np]);
            }
            goto matched;
        }
        if (*p == ':') {
            const char *pe = p + 1 + strcspn(p + 1, "/");
            size_t ul = strcspn(u, "/");
            if (ul == 0 || np >= MAX_PARAMS) goto nomatch;
            vals[np] = malloc(ul + 1);
            if (!vals[np] || kn_url_decode(u, ul, vals[np], ul + 1, 0) < 0) {
                free(vals[np]);
                goto nomatch;
            }
            names[np++] = p + 1; /* name runs to the next '/' in pattern: copied below */
            p = pe;
            u += ul;
            continue;
        }
        if (*p != *u) goto nomatch;
        p++;
        u++;
    }
    if (*u) goto nomatch;
matched:
    for (int i = 0; i < np; i++) {
        if (!r) {
            free(vals[i]);
            continue;
        }
        size_t nl = strcspn(names[i], "/");
        if (nl >= sizeof r->param_names[0]) nl = sizeof r->param_names[0] - 1;
        memcpy(r->param_names[r->nparams], names[i], nl);
        r->param_names[r->nparams][nl] = 0;
        r->param_values[r->nparams++] = vals[i];
    }
    return 1;
nomatch:
    for (int i = 0; i < np; i++) free(vals[i]);
    return 0;
}

/* Raw (still percent-encoded) path of the request target. */
static void raw_path(const req_impl *r, char *out, size_t cap) {
    const char *t = r->parsed.target;
    if (t[0] != '/') {
        const char *s = strstr(t, "://");
        t = s ? strchr(s + 3, '/') : NULL;
        if (!t) t = "/";
    }
    size_t n = strcspn(t, "?");
    if (n >= cap) n = cap - 1;
    memcpy(out, t, n);
    out[n] = 0;
}

static int method_ok(const route *rt, const char *method, int is_head) {
    if (!rt->method) return 1;
    if (strcasecmp(rt->method, method) == 0) return 1;
    return is_head && strcasecmp(rt->method, "GET") == 0;
}

/* Answer a CORS preflight for a known path. */
static void preflight(req_impl *r, const char *allow) {
    kn_http_req *req = &r->pub;
    kn_http_set_header(req, "Access-Control-Allow-Methods", allow);
    const char *h = kn_http_header_get(req, "Access-Control-Request-Headers");
    kn_http_set_header(req, "Access-Control-Allow-Headers", h && *h ? h : "Content-Type, Authorization");
    kn_http_set_header(req, "Access-Control-Max-Age", "600");
    kn_http_respond(req, 204, NULL, NULL, 0);
}

static void dispatch(kn_http_conn *c, req_impl *r) {
    kn_http_server *s = c->srv;
    char raw[4096];
    raw_path(r, raw, sizeof raw);
    const char *m = r->pub.method;
    int is_head = strcasecmp(m, "HEAD") == 0;
    r->head = is_head;
    char allow[256] = "";
    int path_known = 0;
    for (size_t i = 0; i < s->nroutes; i++) {
        route *rt = &s->routes[i];
        if (!route_match(rt->pattern, raw, NULL)) continue;
        path_known = 1;
        if (method_ok(rt, m, is_head)) {
            route_match(rt->pattern, raw, r);
            rt->handler(&r->pub, rt->ud);
            return;
        }
        if (rt->method && !strstr(allow, rt->method) && strlen(allow) + strlen(rt->method) + 3 < sizeof allow) {
            if (*allow) strcat(allow, ", ");
            strcat(allow, rt->method);
        }
    }
    if (path_known && strcasecmp(m, "OPTIONS") == 0) {
        if (strlen(allow) + 10 < sizeof allow) strcat(allow, *allow ? ", OPTIONS" : "OPTIONS");
        if (kn_http_header_get(&r->pub, "Access-Control-Request-Method")) preflight(r, allow);
        else {
            kn_http_set_header(&r->pub, "Allow", allow);
            kn_http_respond(&r->pub, 204, NULL, NULL, 0);
        }
        return;
    }
    if (path_known) {
        kn_http_set_header(&r->pub, "Allow", allow);
        kn_http_respond_error(&r->pub, 405, "method not allowed");
        return;
    }
    if (s->fallback) {
        s->fallback(&r->pub, s->fallback_ud);
        return;
    }
    kn_http_respond_error(&r->pub, 404, "not found");
}

/* ------------------------------------------------------------------ connection processing */

static void forwarded_addr(const kn_http_server *s, req_impl *r, const char *peer) {
    snprintf(r->remote, sizeof r->remote, "%s", peer);
    if (!s->cfg.trust_proxy) return;
    const char *real = kn_http_header_get(&r->pub, "X-Real-IP");
    const char *xff = kn_http_header_get(&r->pub, "X-Forwarded-For");
    if (real && *real) {
        snprintf(r->remote, sizeof r->remote, "%s", real);
    } else if (xff && *xff) {
        /* the right-most entry was added by our (trusted) proxy */
        const char *last = strrchr(xff, ',');
        last = last ? last + 1 : xff;
        while (*last == ' ') last++;
        snprintf(r->remote, sizeof r->remote, "%s", last);
        char *e = r->remote + strlen(r->remote);
        while (e > r->remote && e[-1] == ' ') *--e = 0;
    }
}

static void send_error_and_close(kn_http_conn *c, int status) {
    const char *msg = kn_http_status_text(status);
    kn_buf_appendf(&c->out,
                   "HTTP/1.1 %d %s\r\nServer: %s\r\nContent-Type: application/json; charset=utf-8\r\n"
                   "Content-Length: %zu\r\nConnection: close\r\n\r\n{\"error\":\"%s\"}",
                   status, msg, c->srv->server_name, strlen(msg) + 12, msg);
    c->close_after_write = 1;
    conn_after_response(c);
}

/* Headers complete, body pending and "Expect: 100-continue": tell the client to send it. */
static void maybe_continue(kn_http_conn *c) {
    if (c->sent_continue) return;
    const char *end = strstr(c->in.data, "\r\n\r\n");
    if (!end) return;
    for (const char *p = c->in.data; p && p < end; p = strchr(p, '\n')) {
        if (*p == '\n') p++;
        if (strncasecmp(p, "Expect:", 7) == 0) {
            const char *v = p + 7;
            while (*v == ' ' || *v == '\t') v++;
            if (strncasecmp(v, "100-continue", 12) == 0) {
                c->sent_continue = 1;
                kn_buf_append_str(&c->out, "HTTP/1.1 100 Continue\r\n\r\n");
                conn_flush(c);
            }
            return;
        }
    }
}

/* Parse and dispatch the next request if none is active. */
static void conn_process(kn_http_conn *c) {
    while (!c->req && !c->dead && !c->close_after_write && c->in.len) {
        kn_http_parsed parsed;
        size_t used = 0;
        kn_http_parse_status st = kn_http_parse_request(c->in.data, c->in.len, &c->srv->cfg, &parsed, &used);
        if (st == KN_HTTP_PARSE_INCOMPLETE) {
            if (!c->request_start) c->request_start = kn_now_ms();
            maybe_continue(c);
            return;
        }
        if (st == KN_HTTP_PARSE_ERROR) {
            KN_DEBUG("http", "%s: bad request (%d)", c->remote, parsed.error_status);
            send_error_and_close(c, parsed.error_status ? parsed.error_status : 400);
            return;
        }
        kn_buf_consume(&c->in, used);
        c->sent_continue = 0;
        c->request_start = c->in.len ? kn_now_ms() : 0;
        req_impl *r = calloc(1, sizeof *r);
        if (!r) {
            kn_http_parsed_free(&parsed);
            send_error_and_close(c, 500);
            return;
        }
        r->parsed = parsed;
        r->conn = c;
        r->keep_alive = parsed.keep_alive;
        kn_http_req *p = &r->pub;
        p->method = parsed.method;
        p->target = parsed.target;
        p->path = parsed.path;
        p->query = parsed.query;
        p->version_minor = parsed.version_minor;
        p->headers = parsed.headers;
        p->header_count = parsed.header_count;
        p->body = parsed.body;
        p->body_len = parsed.body_len;
        p->server = c->srv;
        forwarded_addr(c->srv, r, c->remote);
        p->remote_addr = r->remote;
        c->req = r;
        dispatch(c, r);
    }
}

static void conn_read(kn_http_conn *c) {
    size_t cap_total = c->srv->cfg.max_header_bytes + c->srv->cfg.max_body_bytes + 65536;
    for (;;) {
        if (c->in.len >= cap_total) break; /* backpressure: stop reading until processed */
        if (kn_buf_reserve(&c->in, READ_CHUNK) != 0) {
            conn_close(c);
            return;
        }
        ssize_t n = recv(c->fd, c->in.data + c->in.len, READ_CHUNK, 0);
        if (n > 0) {
            c->in.len += (size_t)n;
            c->in.data[c->in.len] = 0;
            c->last_activity = kn_now_ms();
            continue;
        }
        if (n == 0) {
            c->peer_closed = 1;
            break;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        conn_close(c);
        return;
    }
    conn_process(c);
    if (c->dead) return;
    if (c->peer_closed && !c->out.len) {
        /* EOF: the client is gone (HTTP clients do not half-close); a pending async
         * request is detached and kn_http_req_alive() reports 0 */
        conn_close(c);
        return;
    }
    conn_update_events(c);
}

static void conn_on_io(kn_loop *loop, int fd, unsigned ev, void *ud) {
    kn_http_conn *c = ud;
    c->busy++;
    if (ev & (KN_EV_ERROR)) conn_close(c);
    if (!c->dead && (ev & KN_EV_WRITE)) {
        if (conn_flush(c) != 0) conn_close(c);
        else if (!c->out.len && c->close_after_write && !c->req) {
            shutdown(c->fd, SHUT_WR);
            conn_close(c);
        }
    }
    if (!c->dead && (ev & (KN_EV_READ | KN_EV_HUP))) conn_read(c);
    c->busy--;
    if (c->dead && !c->busy) conn_free(c);
}

/* ------------------------------------------------------------------ accept / sweep */

static void peer_name(int fd, char *out, size_t cap) {
    struct sockaddr_storage ss;
    socklen_t sl = sizeof ss;
    snprintf(out, cap, "?");
    if (getpeername(fd, (struct sockaddr *)&ss, &sl) != 0) return;
    if (ss.ss_family == AF_INET) inet_ntop(AF_INET, &((struct sockaddr_in *)&ss)->sin_addr, out, (socklen_t)cap);
    else if (ss.ss_family == AF_INET6) {
        struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)&ss;
        if (IN6_IS_ADDR_V4MAPPED(&s6->sin6_addr)) inet_ntop(AF_INET, &s6->sin6_addr.s6_addr[12], out, (socklen_t)cap);
        else inet_ntop(AF_INET6, &s6->sin6_addr, out, (socklen_t)cap);
    }
}

static void on_accept(kn_loop *loop, int lfd, unsigned ev, void *ud) {
    kn_http_server *s = ud;
    for (int i = 0; i < 256; i++) {
        int fd = kn_accept_nb(lfd);
        if (fd < 0) {
            if (errno == EINTR) continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK) KN_WARN("http", "accept: %s", strerror(errno));
            return;
        }
        if (s->nconns >= s->cfg.max_connections) {
            static const char busy[] = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            ssize_t w = send(fd, busy, sizeof busy - 1, MSG_NOSIGNAL);
            (void)w;
            close(fd);
            continue;
        }
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        kn_http_conn *c = calloc(1, sizeof *c);
        if (!c) {
            close(fd);
            continue;
        }
        c->srv = s;
        c->fd = fd;
        c->last_activity = kn_now_ms();
        peer_name(fd, c->remote, sizeof c->remote);
        if (kn_loop_add_fd(loop, fd, KN_EV_READ, conn_on_io, c) != 0) {
            close(fd);
            free(c);
            continue;
        }
        c->next = s->conns;
        if (s->conns) s->conns->prev = c;
        s->conns = c;
        s->nconns++;
    }
}

static void on_sweep(kn_loop *loop, void *ud) {
    kn_http_server *s = ud;
    uint64_t now = kn_now_ms();
    for (kn_http_conn *c = s->conns, *next; c; c = next) {
        next = c->next;
        if (c->req) continue; /* handler owns the timing */
        if (c->request_start && now - c->request_start > (uint64_t)s->cfg.request_timeout_ms) {
            c->busy++;
            send_error_and_close(c, 408);
            c->busy--;
            if (!c->dead) conn_close(c);
            else conn_free(c);
            continue;
        }
        if (!c->request_start && !c->out.len && now - c->last_activity > (uint64_t)s->cfg.keepalive_timeout_ms)
            conn_close(c);
        else if (c->out.len && now - c->last_activity > (uint64_t)s->cfg.request_timeout_ms)
            conn_close(c); /* peer not reading */
    }
}

/* ------------------------------------------------------------------ server */

kn_http_server *kn_http_server_new(kn_loop *loop, const kn_http_config *cfg) {
    kn_http_server *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->loop = loop;
    s->listen_fd = -1;
    if (cfg) s->cfg = *cfg;
    kn_http_config_defaults(&s->cfg);
    s->host = strdup(s->cfg.host);
    s->server_name = strdup(s->cfg.server_name);
    s->cfg.host = s->host;
    s->cfg.server_name = s->server_name;
    if (s->cfg.cors_origin_count) {
        s->cors = calloc(s->cfg.cors_origin_count, sizeof *s->cors);
        for (size_t i = 0; s->cors && i < s->cfg.cors_origin_count; i++)
            if (s->cfg.cors_origins[i]) s->cors[s->ncors++] = strdup(s->cfg.cors_origins[i]);
    }
    s->cfg.cors_origins = NULL;
    s->cfg.cors_origin_count = s->ncors;
    if (!s->host || !s->server_name) {
        kn_http_server_free(s);
        return NULL;
    }
    return s;
}

int kn_http_server_listen(kn_http_server *s) {
    char port[16];
    snprintf(port, sizeof port, "%d", s->cfg.port);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;
    const char *host = (!strcmp(s->host, "") || !strcmp(s->host, "*")) ? NULL : s->host;
    int rc = getaddrinfo(host, port, &hints, &res);
    if (rc != 0) {
        KN_ERROR("http", "resolve %s: %s", s->host, gai_strerror(rc));
        return -1;
    }
    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = kn_socket_nb(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        if (bind(fd, ai->ai_addr, ai->ai_addrlen) == 0 && listen(fd, 511) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) {
        KN_ERROR("http", "cannot listen on %s:%d: %s", s->host, s->cfg.port, strerror(errno));
        return -1;
    }
    struct sockaddr_storage ss;
    socklen_t sl = sizeof ss;
    getsockname(fd, (struct sockaddr *)&ss, &sl);
    s->port = ntohs(ss.ss_family == AF_INET6 ? ((struct sockaddr_in6 *)&ss)->sin6_port : ((struct sockaddr_in *)&ss)->sin_port);
    if (kn_loop_add_fd(s->loop, fd, KN_EV_READ, on_accept, s) != 0) {
        close(fd);
        return -1;
    }
    s->listen_fd = fd;
    s->sweep = kn_loop_add_timer(s->loop, SWEEP_MS, SWEEP_MS, on_sweep, s);
    KN_INFO("http", "listening on %s:%d", s->host, s->port);
    return 0;
}

int kn_http_server_port(const kn_http_server *s) { return s->port; }
kn_loop *kn_http_server_loop(const kn_http_server *s) { return s->loop; }

void kn_http_server_free(kn_http_server *s) {
    if (!s) return;
    if (s->listen_fd >= 0) {
        kn_loop_del_fd(s->loop, s->listen_fd);
        close(s->listen_fd);
    }
    kn_loop_cancel_timer(s->loop, s->sweep);
    while (s->conns) {
        kn_http_conn *c = s->conns;
        if (c->req) c->req->pub.server = NULL;
        c->busy = 0;
        conn_free(c);
    }
    for (size_t i = 0; i < s->nroutes; i++) {
        free(s->routes[i].method);
        free(s->routes[i].pattern);
    }
    free(s->routes);
    for (size_t i = 0; i < s->ncors; i++) free(s->cors[i]);
    free(s->cors);
    free(s->host);
    free(s->server_name);
    free(s);
}

/* ------------------------------------------------------------------ websocket takeover */

int kn_http__takeover(kn_http_req *req, kn_http_takeover *out) {
    req_impl *r = (req_impl *)req;
    kn_http_conn *c = r->conn;
    memset(out, 0, sizeof *out);
    out->fd = -1;
    snprintf(out->remote_addr, sizeof out->remote_addr, "%s", r->remote);
    if (!c || c->dead) {
        req_finish(r);
        return -1;
    }
    out->fd = c->fd;
    out->loop = c->srv->loop;
    out->leftover = c->in; /* steal buffer */
    kn_buf_init(&c->in);
    kn_loop_del_fd(c->srv->loop, c->fd);
    c->fd = -1;
    req_finish(r);
    conn_close(c); /* frees now or at the end of the running callback; fd stays open */
    return 0;
}
