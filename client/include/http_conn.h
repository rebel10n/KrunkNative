#pragma once
// Blocking HTTP(S)/WS(S) client connections (POSIX sockets, OpenSSL when the build found it:
// KRUNKNATIVE_HAVE_TLS, peers verified against the system CA store and the host name). Shared by
// the brain client (client/src/net/backend_native.c) and the launcher's downloader
// (launcher/src/http_fetch.c). Not available on Windows (WinHTTP there) or the web.
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
#include <stddef.h>
#include <sys/types.h>

typedef struct {
    char host[128];
    char port[8];
    char base[128]; // path prefix, "" or "/prefix"
    int tls;        // https:// / wss://
} HttpAddr;

// http(s)://host[:port][/base] (also ws:// / wss://); TLS needs KRUNKNATIVE_HAVE_TLS. 1 = parsed.
int http_url_parse(const char *url, HttpAddr *a);
const char *http_url_target(const char *url);

typedef struct {
    int fd;
    void *ssl;       // SSL* (KRUNKNATIVE_HAVE_TLS) the connection reads and writes, NULL for plain connections
    void *proxy_ssl; // SSL* of an https:// proxy under ssl (the target's TLS runs inside it), else NULL
} HttpConn;

// Connects (and runs the TLS handshake, verifying the certificate for a->host). 0 on failure.
// Host lookups are cached for 5 minutes and TLS sessions per host, process-wide.
// A host the proxy covers (http_proxy.h: the client setting or HTTPS_PROXY / HTTP_PROXY /
// ALL_PROXY / NO_PROXY) is reached through it: HTTP CONNECT (http:// / https:// proxies) or
// SOCKS5, for plain and TLS connections alike; the TLS handshake then runs end to end.
// timeout_ms bounds the connect and every later send/receive (SO_SNDTIMEO / SO_RCVTIMEO).
int http_conn_open(HttpConn *c, const HttpAddr *a, int timeout_ms);
// Why the calling thread's last failed http_conn_open failed ("cannot resolve dl.krunk.io (DNS:
// ...)", "cannot connect to <host> port 443 (timed out)", "TLS handshake with <host> failed: ...").
const char *http_conn_error(void);
// Whether the calling thread's last failed http_conn_open failed in the target's own TLS handshake (the
// certificate or the host name did not verify), not in the connect or a proxy. 0 otherwise.
int http_conn_tls_failed(void);
void http_conn_close(HttpConn *c);
int http_conn_send(HttpConn *c, const void *data, size_t len);
// >0 bytes, 0 closed, <0 error (errno EAGAIN on a receive timeout)
ssize_t http_conn_recv(HttpConn *c, void *buf, size_t len);
// Decrypted bytes waiting inside the TLS layer (poll() on the fd cannot see them).
int http_conn_pending(const HttpConn *c);
#endif
