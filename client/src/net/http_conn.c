// Blocking HTTP(S)/WS(S) client connections (client/include/http_conn.h), moved out of
// backend_native.c so the launcher's downloader shares them.
#include <http_conn.h>
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#ifdef KRUNKNATIVE_HAVE_TLS
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#endif

// http(s)://host[:port][/base] (also ws:// / wss://); TLS needs KRUNKNATIVE_HAVE_TLS
int http_url_parse(const char *url, HttpAddr *a) {
    memset(a, 0, sizeof(*a));
    const char *p = url;
    if (!strncmp(p, "http://", 7)) p += 7;
    else if (!strncmp(p, "ws://", 5)) p += 5;
    else if (!strncmp(p, "https://", 8) || !strncmp(p, "wss://", 6)) {
#ifdef KRUNKNATIVE_HAVE_TLS
        p += p[0] == 'h' ? 8 : 6;
        a->tls = 1;
#else
        static int warned;
        if (!warned++) fprintf(stderr, "[http] %s needs TLS, but this client was built without OpenSSL\n", url);
        return 0;
#endif
    }
    const char *host_end = p;
    while (*host_end && *host_end != ':' && *host_end != '/') host_end++;
    const size_t host_len = (size_t) (host_end - p);
    if (!host_len || host_len >= sizeof(a->host)) return 0;
    memcpy(a->host, p, host_len);
    p = host_end;
    if (*p == ':') {
        p++;
        size_t n = 0;
        while (*p >= '0' && *p <= '9' && n + 1 < sizeof(a->port)) a->port[n++] = *p++;
        a->port[n] = 0;
    }
    if (!a->port[0]) strcpy(a->port, a->tls ? "443" : "80");
    if (*p == '/') snprintf(a->base, sizeof(a->base), "%s", p);
    size_t n = strlen(a->base);
    while (n && a->base[n - 1] == '/') a->base[--n] = 0;
    return 1;
}

// The request target of an http(s) URL: everything from the first '/' after host[:port]
// ("" for none). Not HttpAddr.base: that one is char[128] and cut a signed-in seek-game
// (?game=...&token=<32 hex>) short by a character, so the brain saw a guest.
const char *http_url_target(const char *url) {
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    while (*p && *p != '/') p++;
    return p;
}

// Blocking connect with a timeout (non-blocking connect + poll), then blocking again.
static int connect_to(const HttpAddr *a, const int timeout_ms) {
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(a->host, a->port, &hints, &res) != 0 || !res) return -1;
    int fd = -1;
    for (struct addrinfo *ai = res; ai && fd < 0; ai = ai->ai_next) {
        fd = (int) socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) != 0) {
            close(fd);
            fd = -1;
        }
    }
    freeaddrinfo(res);
    if (fd >= 0) {
        const int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    }
    return fd;
}

static int send_all(const int fd, const void *data, size_t len) {
    const char *p = data;
    while (len) {
        const ssize_t w = send(fd, p, len, MSG_NOSIGNAL);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return 0;
        p += w;
        len -= (size_t) w;
    }
    return 1;
}

// ------------------------------------------------------------------ connection (plain or TLS)

#ifdef KRUNKNATIVE_HAVE_TLS
static SSL_CTX *g_tls_ctx;
static pthread_once_t g_tls_once = PTHREAD_ONCE_INIT;

#ifdef KRUNKNATIVE_TLS_STATIC
// Static OpenSSL (the polyfilled Linux and the macOS packages, tools/release/build-client-linux.sh,
// build-openssl.sh): its OPENSSLDIR is the build host's, which the user's system may not have,
// and the distro's openssl.cnf may name groups or providers this OpenSSL doesn't know
// (SSL_CTX_new would fail). So no config file, and the CA bundle is the first of the usual
// locations, unless SSL_CERT_FILE / SSL_CERT_DIR point somewhere (set_default_verify_paths
// honours them).
static void tls_load_ca_bundle(SSL_CTX *ctx) {
    if (getenv("SSL_CERT_FILE") || getenv("SSL_CERT_DIR")) return;
    static const char *const BUNDLES[] = {
        "/etc/ssl/certs/ca-certificates.crt",                // Debian, Ubuntu, Arch, Gentoo, Alpine
        "/etc/pki/tls/certs/ca-bundle.crt",                  // Fedora, RHEL, CentOS
        "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem", // Fedora, RHEL 7+
        "/etc/ssl/ca-bundle.pem",                            // openSUSE
        "/etc/ssl/cert.pem",                                 // macOS, Alpine, Void
    };
    for (size_t i = 0; i < sizeof(BUNDLES) / sizeof(BUNDLES[0]); i++) {
        if (access(BUNDLES[i], R_OK) == 0 && SSL_CTX_load_verify_locations(ctx, BUNDLES[i], NULL) == 1) return;
    }
    ERR_clear_error();
    if (SSL_CTX_load_verify_locations(ctx, NULL, "/etc/ssl/certs") != 1) {
        ERR_clear_error();
        fprintf(stderr, "[http] TLS: no CA bundle found (set SSL_CERT_FILE)\n");
    }
}
#endif

static void tls_init_once(void) {
#ifdef KRUNKNATIVE_TLS_STATIC
    OPENSSL_init_ssl(OPENSSL_INIT_NO_LOAD_CONFIG, NULL);
#endif
    g_tls_ctx = SSL_CTX_new(TLS_client_method());
    if (!g_tls_ctx) return;
    SSL_CTX_set_min_proto_version(g_tls_ctx, TLS1_2_VERSION);
    SSL_CTX_set_verify(g_tls_ctx, SSL_VERIFY_PEER, NULL);
    if (SSL_CTX_set_default_verify_paths(g_tls_ctx) != 1) fprintf(stderr, "[http] TLS: no system CA store\n");
#ifdef KRUNKNATIVE_TLS_STATIC
    tls_load_ca_bundle(g_tls_ctx);
#endif
}
#endif

// Connects (and runs the TLS handshake, verifying the certificate for a->host). 0 on failure.
int http_conn_open(HttpConn *c, const HttpAddr *a, const int timeout_ms) {
    memset(c, 0, sizeof(*c));
    c->fd = connect_to(a, timeout_ms);
    if (c->fd < 0) return 0;
    struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#ifdef KRUNKNATIVE_HAVE_TLS
    if (a->tls) {
        pthread_once(&g_tls_once, tls_init_once);
        SSL *ssl = g_tls_ctx ? SSL_new(g_tls_ctx) : NULL;
        c->ssl = ssl;
        if (!c->ssl || !SSL_set_fd(c->ssl, c->fd) || !SSL_set_tlsext_host_name(c->ssl, a->host) ||
            !SSL_set1_host(c->ssl, a->host) || SSL_connect(c->ssl) != 1) {
            const long verify = c->ssl ? SSL_get_verify_result(c->ssl) : X509_V_OK;
            fprintf(stderr, "[http] TLS to %s failed%s%s\n", a->host, verify != X509_V_OK ? ": " : "",
                    verify != X509_V_OK ? X509_verify_cert_error_string(verify) : "");
            ERR_clear_error();
            if (c->ssl) SSL_free(c->ssl);
            c->ssl = NULL;
            close(c->fd);
            c->fd = -1;
            return 0;
        }
    }
#endif
    return 1;
}

void http_conn_close(HttpConn *c) {
#ifdef KRUNKNATIVE_HAVE_TLS
    if (c->ssl) {
        SSL_shutdown(c->ssl);
        SSL_free(c->ssl);
        c->ssl = NULL;
    }
#endif
    if (c->fd >= 0) close(c->fd);
    c->fd = -1;
}

int http_conn_send(HttpConn *c, const void *data, size_t len) {
#ifdef KRUNKNATIVE_HAVE_TLS
    if (c->ssl) {
        const char *p = data;
        while (len) {
            const int w = SSL_write(c->ssl, p, len > 1 << 30 ? 1 << 30 : (int) len);
            if (w <= 0) return 0;
            p += w;
            len -= (size_t) w;
        }
        return 1;
    }
#endif
    return send_all(c->fd, data, len);
}

// >0 bytes, 0 closed, <0 error (errno EAGAIN on a receive timeout)
ssize_t http_conn_recv(HttpConn *c, void *buf, const size_t len) {
#ifdef KRUNKNATIVE_HAVE_TLS
    if (c->ssl) {
        const int r = SSL_read(c->ssl, buf, len > 1 << 30 ? 1 << 30 : (int) len);
        if (r > 0) return r;
        const int e = SSL_get_error(c->ssl, r);
        if (e == SSL_ERROR_ZERO_RETURN) return 0;
        if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) errno = EAGAIN;
        ERR_clear_error();
        return -1;
    }
#endif
    for (;;) {
        const ssize_t r = recv(c->fd, buf, len, 0);
        if (r < 0 && errno == EINTR) continue;
        return r;
    }
}

// Decrypted bytes waiting inside the TLS layer (poll() on the fd cannot see them).
int http_conn_pending(const HttpConn *c) {
#ifdef KRUNKNATIVE_HAVE_TLS
    if (c->ssl) return SSL_pending(c->ssl) > 0;
#endif
    (void) c;
    return 0;
}
#endif
