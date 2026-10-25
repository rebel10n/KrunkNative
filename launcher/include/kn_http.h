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

typedef struct KnHttpSink {
    KnHttpWriteFn write;
    void *user;
    volatile int *cancel;     // optional: non-zero aborts
    int64_t content_length;   // out: the final response's length, -1 unknown
    int64_t range_from;       // in: >0 sends "Range: bytes=<range_from>-" (a 206 answer continues there)
    int status;               // out: the final response's status, set before the first write
    int accept_gzip;          // in: offer "gzip" (POSIX only; WinHTTP always asks for identity)
    int gzip;                 // out: the body reaching the sink is gzip-encoded (Content-Encoding)
} KnHttpSink;

// GET url (http:// or https://), following up to 5 redirects. Only a 2xx body reaches the sink.
// With range_from, a 206 is accepted only when its Content-Range starts at range_from (else the
// transfer fails); a server that ignores the Range answers 200 with the whole body.
// Returns the final HTTP status, 0 on a transport error or abort (err says why).
int kn_http_get(KnHttp *h, const char *url, KnHttpSink *sink, char *err, size_t errn);
// HTTP/1.1 pipelining: GETs of n URLs on ONE host over the one connection, up to depth requests
// in flight (at most KN_HTTP_PIPELINE_MAX), the answers read in order into each reqs[i].sink.
// No redirects are followed (a 3xx is just a status). Returns how many got an answer: reqs[0..r)
// have status set (a 2xx body went to their sink; 0 = failed mid-answer, err says why), the rest
// were not served (the server closed the connection, or a failure: the caller retries them,
// e.g. one at a time). Only for idempotent GETs of servers known to keep requests in order
// (any HTTP/1.1 server must). WinHTTP: one after another.
#define KN_HTTP_PIPELINE_MAX 64
typedef struct KnHttpReq KnHttpReq;
struct KnHttpReq {
    const char *url;
    KnHttpSink *sink;
    // optional: called right after this answer was read (status set), before the next one is
    void (*done)(void *user, KnHttpReq *r);
    void *user;
    int status;
    char err[160];
};
int kn_http_get_pipelined(KnHttp *h, KnHttpReq *reqs, size_t n, int depth, volatile int *cancel);
// GET into memory (NUL-terminated, at most max bytes; caller frees *body). Returns the status;
// *body is set only for a 2xx answer.
int kn_http_get_mem(KnHttp *h, const char *url, size_t max, char **body, size_t *len, char *err, size_t errn);
// Same, offering gzip: *gzip says whether *body is still gzip-encoded (the caller inflates it).
int kn_http_get_mem_gz(KnHttp *h, const char *url, size_t max, char **body, size_t *len, int *gzip, char *err,
                       size_t errn);
// Process-wide counters (benchmarks, the launcher's log): requests sent, connections opened.
typedef struct {
    int64_t requests, connects;
    double connect_s; // sum over connects: DNS + TCP + TLS handshake
    double ttfb_s;    // sum over requests: request sent -> status line
} KnHttpStats;
void kn_http_stats(KnHttpStats *out);
// The User-Agent sent ("KrunkNative/<describe>"); set once before the first request.
void kn_http_set_user_agent(const char *ua);
