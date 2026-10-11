#pragma once
// Blocking HTTP(S) client for the game server's outbound calls (finder heartbeat,
// central auth hasJoined). Call from worker threads only. https:// needs libcurl
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
// Percent-encodes in into out (RFC 3986 unreserved kept).
void http_url_encode(char *out, size_t size, const char *in);
