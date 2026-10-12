#pragma once
// Blocking HTTP(S) GET for the launcher and the client's update check: one keep-alive
// connection per KnHttp (use one per thread), redirects followed, the body streamed to a sink.
// POSIX: client/src/net/http_conn.c (sockets + OpenSSL when KRUNKNATIVE_HAVE_TLS); Windows:
// WinHTTP (system TLS and certificate store). Not built for the web.
#include <stddef.h>
#include <stdint.h>

typedef struct KnHttp KnHttp;

KnHttp *kn_http_new(int timeout_ms);
void kn_http_free(KnHttp *h);

// Called with each piece of a 2xx body; return 0 to abort the transfer.
typedef int (*KnHttpWriteFn)(void *user, const void *data, size_t len);

typedef struct {
    KnHttpWriteFn write;
    void *user;
    volatile int *cancel;     // optional: non-zero aborts
    int64_t content_length;   // out: the final response's length, -1 unknown
} KnHttpSink;

// GET url (http:// or https://), following up to 5 redirects. Only a 2xx body reaches the sink.
// Returns the final HTTP status, 0 on a transport error or abort (err says why).
int kn_http_get(KnHttp *h, const char *url, KnHttpSink *sink, char *err, size_t errn);
// GET into memory (NUL-terminated, at most max bytes; caller frees *body). Returns the status;
// *body is set only for a 2xx answer.
int kn_http_get_mem(KnHttp *h, const char *url, size_t max, char **body, size_t *len, char *err, size_t errn);
// The User-Agent sent ("KrunkNative/<describe>"); set once before the first request.
void kn_http_set_user_agent(const char *ua);
