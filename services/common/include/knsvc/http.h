/*
 * knsvc/http.h - non-blocking HTTP/1.1 server on a kn_loop, plus URL helpers.
 *
 * Features: routing (method + pattern with :params and trailing *), keep-alive,
 * pipelining (one request processed at a time per connection), chunked request
 * bodies, chunked streaming responses, request size limits (431/413), idle and
 * header timeouts, CORS (preflight answered automatically for routed paths),
 * JSON helpers, X-Forwarded-For (trust_proxy, for nginx in front), WebSocket
 * upgrade (see kn_ws_accept in knsvc/ws.h).
 *
 * Handlers may respond synchronously or later (after a DB query): every
 * kn_http_req must receive exactly one kn_http_respond*() (or kn_ws_accept, or a
 * completed stream). If the client disconnects first, kn_http_req_alive()
 * returns 0 and responding just releases the request.
 *
 * Route patterns:
 *   "/api/games"           exact
 *   "/api/user/:name"      :name matches one segment, read with kn_http_param()
 *   "/static/" + "*"       a trailing star matches the rest: kn_http_param(req, "*")
 * Routes are tried in registration order; method NULL matches any method.
 * Unmatched paths -> 404 JSON {"error":"not found"}; known path, wrong method -> 405.
 */
#ifndef KNSVC_HTTP_H
#define KNSVC_HTTP_H

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "knsvc/loop.h"

typedef struct kn_http_server kn_http_server;
typedef struct kn_http_req kn_http_req;

typedef struct kn_http_header {
    const char *name;   /* as received; compare case-insensitively */
    const char *value;  /* leading/trailing whitespace trimmed */
} kn_http_header;

typedef struct kn_http_config {
    const char *host;              /* bind address, default "0.0.0.0" */
    int port;                      /* 0 = ephemeral (see kn_http_server_port) */
    size_t max_header_bytes;       /* request line + headers, default 16 KiB -> 431 */
    size_t max_body_bytes;         /* default 1 MiB -> 413 */
    int max_headers;               /* default 64 */
    int keepalive_timeout_ms;      /* idle keep-alive connection close, default 15000 */
    int request_timeout_ms;        /* full request must arrive within, default 30000 -> 408 */
    int max_connections;           /* default 4096; extra accepts are closed */
    int max_requests_per_conn;     /* 0 = unlimited */
    int trust_proxy;               /* remote_addr from X-Real-IP, else the right-most X-Forwarded-For entry */
    const char *server_name;       /* "Server:" header, default "knsvc" */
    /* CORS: allowed origins ("*" = any; NULL/0 = CORS disabled). Copied. */
    const char *const *cors_origins;
    size_t cors_origin_count;
    int cors_allow_credentials;    /* echo origin + Access-Control-Allow-Credentials: true */
} kn_http_config;

/* Request (read-only for handlers; strings live until the request is released). */
struct kn_http_req {
    const char *method;            /* "GET", ... */
    const char *target;            /* raw request target "/a/b?x=1" */
    const char *path;              /* percent-decoded path without the query */
    const char *query;             /* raw query string without '?', "" if none */
    int version_minor;             /* HTTP/1.<n> */
    const kn_http_header *headers;
    size_t header_count;
    const char *body;              /* NUL-terminated (body[body_len] == 0) */
    size_t body_len;
    const char *remote_addr;       /* peer IP (or forwarded IP with trust_proxy) */
    kn_http_server *server;
    void *user;                    /* free for the handler (async state) */
};

typedef void (*kn_http_handler)(kn_http_req *req, void *ud);

/* Defaults for every zero field of cfg are applied by kn_http_server_new. */
void kn_http_config_defaults(kn_http_config *cfg);
kn_http_server *kn_http_server_new(kn_loop *loop, const kn_http_config *cfg);
int kn_http_server_listen(kn_http_server *srv);       /* 0 ok, -1 error (logged) */
int kn_http_server_port(const kn_http_server *srv);   /* bound port */
kn_loop *kn_http_server_loop(const kn_http_server *srv);
void kn_http_server_free(kn_http_server *srv);        /* closes every connection */

int kn_http_route(kn_http_server *srv, const char *method, const char *pattern,
                  kn_http_handler handler, void *ud);
/* Fallback for unmatched requests (instead of the 404). */
void kn_http_set_fallback(kn_http_server *srv, kn_http_handler handler, void *ud);

/* ---- request accessors */
const char *kn_http_header_get(const kn_http_req *req, const char *name); /* case-insensitive, NULL if absent */
const char *kn_http_param(const kn_http_req *req, const char *name);      /* route :param, NULL if absent */
/* URL-decoded query value; returns its length, -1 if absent, -2 if present but it
 * does not fit in cap (out is then ""). out is always NUL-terminated. */
int kn_http_query_get(const kn_http_req *req, const char *key, char *out, size_t cap);
cJSON *kn_http_body_json(const kn_http_req *req);     /* NULL if empty/invalid; caller frees */
int kn_http_req_alive(const kn_http_req *req);        /* 0 once the client went away */
int kn_http_is_ws_upgrade(const kn_http_req *req);
/* Bearer token from "Authorization: Bearer x" (NULL if absent). Points into the header. */
const char *kn_http_bearer(const kn_http_req *req);

/* ---- responses (each releases the request) */
/* Extra response header; call before responding. Content-Length/Connection/Date/CORS are automatic. */
int kn_http_set_header(kn_http_req *req, const char *name, const char *value);
void kn_http_respond(kn_http_req *req, int status, const char *content_type, const void *body, size_t len);
void kn_http_respond_text(kn_http_req *req, int status, const char *text);
void kn_http_respond_json(kn_http_req *req, int status, const cJSON *json);   /* json not consumed */
void kn_http_respond_json_str(kn_http_req *req, int status, const char *json_text);
void kn_http_respond_error(kn_http_req *req, int status, const char *message); /* {"error":message} */
/* Chunked streaming: begin, any number of writes, end (end releases). */
int kn_http_stream_begin(kn_http_req *req, int status, const char *content_type);
int kn_http_stream_write(kn_http_req *req, const void *data, size_t len);
void kn_http_stream_end(kn_http_req *req);

const char *kn_http_status_text(int status);

/* ---- incremental request parser (exposed for tests / fuzzing) */
typedef enum kn_http_parse_status {
    KN_HTTP_PARSE_INCOMPLETE = 0,   /* need more bytes */
    KN_HTTP_PARSE_DONE = 1,         /* one full request parsed; *consumed bytes used */
    KN_HTTP_PARSE_ERROR = -1        /* malformed: respond `error_status` and close */
} kn_http_parse_status;

typedef struct kn_http_parsed {
    char *method, *target, *path, *query;
    int version_minor;
    kn_http_header *headers;
    size_t header_count;
    char *body;
    size_t body_len;
    int keep_alive;                 /* after Connection/version rules */
    int error_status;               /* 400/413/431/501/505 when ERROR */
    void *storage_;                 /* private */
} kn_http_parsed;

kn_http_parse_status kn_http_parse_request(const char *data, size_t len, const kn_http_config *limits,
                                           kn_http_parsed *out, size_t *consumed);
void kn_http_parsed_free(kn_http_parsed *p);

/* ---- URL helpers */
typedef struct kn_url {
    char scheme[16];                /* "http", "ws", ... (lowercase) */
    char host[256];                 /* without brackets for IPv6 */
    int port;                       /* explicit or scheme default (80/443) */
    char path[1024];                /* path + "?query" ("/" when empty) */
} kn_url;

int kn_url_parse(const char *url, kn_url *out);                        /* 0 ok, -1 invalid */
/* Percent-decoding ('+' -> ' ' when form != 0). Returns decoded length or -1. */
int kn_url_decode(const char *in, size_t len, char *out, size_t cap, int form);
int kn_url_encode(const char *in, char *out, size_t cap);              /* RFC 3986 unreserved kept */

#endif
