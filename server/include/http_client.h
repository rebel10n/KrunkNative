#pragma once
// Blocking HTTP(S) client for the game server's outbound calls (finder heartbeat,
// central auth hasJoined, the comp webhook). Call from worker threads only. https:// needs libcurl
// (KRUNKNATIVE_HAVE_CURL); without it only plain http:// works.
#include <stddef.h>

typedef struct {
    int status;        // HTTP status, 0 = transport error (see error)
    char *body;        // NUL-terminated response body (caller frees with free()), may be NULL
    size_t body_len;
    char error[128];
} HttpResult;

// method "GET" / "POST"; body + content_type may be NULL. timeout in ms (connect + transfer).
// Returns 0 when a response arrived (any status), -1 on transport errors.
int http_request(const char *method, const char *url, const char *content_type, const char *body, int timeout_ms,
                 size_t max_body, HttpResult *out);
void http_result_free(HttpResult *r);

// A POST to a URL that a player chose (the comp host's webhook, server/src/comp_webhook.c): https:// only
// (http:// too when allow_http), no proxy, no redirect, and every address libcurl is about to connect to must
// pass addr_ok (NULL: any), so a name that resolves to a private address is refused at connect time.
// connect_ms bounds the connect, timeout_ms the whole transfer. Same results as http_request; worker
// threads only. libcurl only: -1 ("needs libcurl") without it.
struct sockaddr;
typedef int (*HttpAddrCheck)(const struct sockaddr *addr);
int http_post_checked(const char *url, const char *content_type, const char *body, int connect_ms, int timeout_ms,
                      size_t max_body, int allow_http, HttpAddrCheck addr_ok, HttpResult *out);
// Percent-encodes in into out (RFC 3986 unreserved kept).
void http_url_encode(char *out, size_t size, const char *in);
