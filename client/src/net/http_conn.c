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
#include <time.h>
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

// DNS cache: the launcher's download workers open dozens of connections to the same two hosts
// at once; one lookup per host:port per 5 minutes instead of one per connection. An address
// that fails to connect drops the entry, so the next attempt resolves again.
#define DNS_CACHE_N 8
#define DNS_CACHE_TTL_S 300
#define DNS_CACHE_ADDRS 4
typedef struct {
    char host[128], port[8];
    time_t at;
    int n;
    struct {
        struct sockaddr_storage addr;
        socklen_t len;
        int family, socktype, protocol;
    } a[DNS_CACHE_ADDRS];
} DnsEntry;
static DnsEntry g_dns[DNS_CACHE_N];
static pthread_mutex_t g_dns_lock = PTHREAD_MUTEX_INITIALIZER;

static int dns_lookup(const HttpAddr *a, DnsEntry *out) {
    const time_t now = time(NULL);
    pthread_mutex_lock(&g_dns_lock);
    for (int i = 0; i < DNS_CACHE_N; i++) {
        if (g_dns[i].n && now - g_dns[i].at < DNS_CACHE_TTL_S && !strcmp(g_dns[i].host, a->host) &&
            !strcmp(g_dns[i].port, a->port)) {
            *out = g_dns[i];
            pthread_mutex_unlock(&g_dns_lock);
            return 1;
        }
    }
    pthread_mutex_unlock(&g_dns_lock);
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(a->host, a->port, &hints, &res) != 0 || !res) return 0;
    memset(out, 0, sizeof(*out));
    snprintf(out->host, sizeof(out->host), "%s", a->host);
    snprintf(out->port, sizeof(out->port), "%s", a->port);
    out->at = now;
    for (struct addrinfo *ai = res; ai && out->n < DNS_CACHE_ADDRS; ai = ai->ai_next) {
        if (ai->ai_addrlen > sizeof(out->a[0].addr)) continue;
        memcpy(&out->a[out->n].addr, ai->ai_addr, ai->ai_addrlen);
        out->a[out->n].len = (socklen_t) ai->ai_addrlen;
        out->a[out->n].family = ai->ai_family;
        out->a[out->n].socktype = ai->ai_socktype;
        out->a[out->n].protocol = ai->ai_protocol;
        out->n++;
    }
    freeaddrinfo(res);
    if (!out->n) return 0;
    pthread_mutex_lock(&g_dns_lock);
    int slot = 0;
    for (int i = 0; i < DNS_CACHE_N; i++) {
        if (!strcmp(g_dns[i].host, a->host) && !strcmp(g_dns[i].port, a->port)) {
            slot = i;
            break;
        }
        if (g_dns[i].at < g_dns[slot].at) slot = i; // empty (at 0) or the oldest
    }
    g_dns[slot] = *out;
    pthread_mutex_unlock(&g_dns_lock);
    return 1;
}

static void dns_forget(const HttpAddr *a) {
    pthread_mutex_lock(&g_dns_lock);
    for (int i = 0; i < DNS_CACHE_N; i++)
        if (!strcmp(g_dns[i].host, a->host) && !strcmp(g_dns[i].port, a->port)) memset(&g_dns[i], 0, sizeof(g_dns[i]));
    pthread_mutex_unlock(&g_dns_lock);
}

// Blocking connect (SO_SNDTIMEO bounds it) to the first address that answers.
static int connect_to(const HttpAddr *a, const int timeout_ms) {
    DnsEntry d;
    if (!dns_lookup(a, &d)) return -1;
    int fd = -1;
    for (int i = 0; i < d.n && fd < 0; i++) {
        fd = (int) socket(d.a[i].family, d.a[i].socktype, d.a[i].protocol);
        if (fd < 0) continue;
        struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        if (connect(fd, (const struct sockaddr *) &d.a[i].addr, d.a[i].len) != 0) {
            close(fd);
            fd = -1;
        }
    }
    if (fd < 0) {
        dns_forget(a);
        return -1;
    }
    const int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
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

// TLS session cache: the last session (ticket) per host, so a reconnect (a kept-alive connection
// the server closed, a retry) resumes instead of running a full handshake. TSan reports races
// inside libcrypto here and in the handshake itself: libcrypto is not instrumented, so its atomic
// reference counts are invisible to it (the same reports come from the code before this cache).
#define TLS_SESS_N 8
static struct {
    char host[128];
    SSL_SESSION *sess;
} g_tls_sess[TLS_SESS_N];
static pthread_mutex_t g_tls_sess_lock = PTHREAD_MUTEX_INITIALIZER;

static int tls_new_session(SSL *ssl, SSL_SESSION *sess) {
    const char *host = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
    if (!host || strlen(host) >= sizeof(g_tls_sess[0].host)) return 0;
    pthread_mutex_lock(&g_tls_sess_lock);
    int slot = -1;
    for (int i = 0; i < TLS_SESS_N && slot < 0; i++)
        if (!strcmp(g_tls_sess[i].host, host)) slot = i;
    for (int i = 0; i < TLS_SESS_N && slot < 0; i++)
        if (!g_tls_sess[i].host[0]) slot = i;
    if (slot < 0) slot = 0;
    if (g_tls_sess[slot].sess) SSL_SESSION_free(g_tls_sess[slot].sess);
    snprintf(g_tls_sess[slot].host, sizeof(g_tls_sess[slot].host), "%s", host);
    g_tls_sess[slot].sess = sess;
    pthread_mutex_unlock(&g_tls_sess_lock);
    return 1; // the cache owns sess now
}

// Single use (RFC 8446 C.4: a client should not reuse a ticket): the session leaves the cache when
// a connection takes it; that connection's own new ticket refills the slot.
static void tls_resume(SSL *ssl, const char *host) {
    pthread_mutex_lock(&g_tls_sess_lock);
    for (int i = 0; i < TLS_SESS_N; i++)
        if (g_tls_sess[i].sess && !strcmp(g_tls_sess[i].host, host)) {
            SSL_set_session(ssl, g_tls_sess[i].sess); // takes its own reference
            SSL_SESSION_free(g_tls_sess[i].sess);
            g_tls_sess[i].sess = NULL;
            break;
        }
    pthread_mutex_unlock(&g_tls_sess_lock);
}

static void tls_init_once(void) {
#ifdef KRUNKNATIVE_TLS_STATIC
    OPENSSL_init_ssl(OPENSSL_INIT_NO_LOAD_CONFIG, NULL);
#endif
    g_tls_ctx = SSL_CTX_new(TLS_client_method());
    if (!g_tls_ctx) return;
    SSL_CTX_set_min_proto_version(g_tls_ctx, TLS1_2_VERSION);
    SSL_CTX_set_verify(g_tls_ctx, SSL_VERIFY_PEER, NULL);
    SSL_CTX_set_session_cache_mode(g_tls_ctx, SSL_SESS_CACHE_CLIENT | SSL_SESS_CACHE_NO_INTERNAL_STORE);
    SSL_CTX_sess_set_new_cb(g_tls_ctx, tls_new_session);
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
        if (ssl) tls_resume(ssl, a->host);
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
