// Blocking HTTP(S)/WS(S) client connections (client/include/http_conn.h), moved out of
// backend_native.c so the launcher's downloader shares them.
#include <http_conn.h>
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
#include <http_proxy.h>
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

#ifdef KRUNKNATIVE_HAVE_TLS
// A TLS client for host on fd, or inside `under` (an https:// proxy's TLS: the tunnelled
// handshake reads and writes through it, BIO_f_ssl) when that is set. NULL on failure (logged).
static SSL *tls_start(const int fd, SSL *under, const char *host) {
    pthread_once(&g_tls_once, tls_init_once);
    SSL *ssl = g_tls_ctx ? SSL_new(g_tls_ctx) : NULL;
    int ok = ssl != NULL;
    if (ok) {
        tls_resume(ssl, host);
        if (under) {
            BIO *bio = BIO_new(BIO_f_ssl());
            ok = bio != NULL;
            if (bio) {
                BIO_set_ssl(bio, under, BIO_NOCLOSE);
                SSL_set_bio(ssl, bio, bio); // ssl owns bio (one reference for both directions)
            }
        } else {
            ok = SSL_set_fd(ssl, fd);
        }
    }
    if (!ok || !SSL_set_tlsext_host_name(ssl, host) || !SSL_set1_host(ssl, host) || SSL_connect(ssl) != 1) {
        const long verify = ssl ? SSL_get_verify_result(ssl) : X509_V_OK;
        fprintf(stderr, "[http] TLS to %s failed%s%s\n", host, verify != X509_V_OK ? ": " : "",
                verify != X509_V_OK ? X509_verify_cert_error_string(verify) : "");
        ERR_clear_error();
        if (ssl) SSL_free(ssl);
        return NULL;
    }
    return ssl;
}
#endif

// ------------------------------------------------------------------ proxy tunnels (http_proxy.h)

static int recv_exact(HttpConn *c, void *out, size_t len) {
    char *p = out;
    while (len) {
        const ssize_t r = http_conn_recv(c, p, len);
        if (r <= 0) return 0;
        p += r;
        len -= (size_t) r;
    }
    return 1;
}

static void host_port(const HttpAddr *a, char *out, const size_t n) {
    const int v6 = strchr(a->host, ':') != NULL;
    snprintf(out, n, "%s%s%s:%s", v6 ? "[" : "", a->host, v6 ? "]" : "", a->port);
}

// HTTP CONNECT host:port (RFC 9110 9.3.6), Basic proxy credentials when the URL has them.
static int connect_tunnel(HttpConn *c, const HttpProxy *px, const HttpAddr *a, const char *label) {
    char target[160], auth[400], req[800];
    host_port(a, target, sizeof(target));
    http_proxy_basic_auth(px, auth, sizeof(auth));
    const int len = snprintf(req, sizeof(req), "CONNECT %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: KrunkNative\r\n%s%s%s\r\n", target, target,
                             auth[0] ? "Proxy-Authorization: Basic " : "", auth, auth[0] ? "\r\n" : "");
    if (len <= 0 || (size_t) len >= sizeof(req) || !http_conn_send(c, req, (size_t) len)) {
        fprintf(stderr, "[http] proxy %s: sending CONNECT %s failed\n", label, target);
        return 0;
    }
    // the answer's head, byte by byte: whatever follows it is the tunnel's
    char resp[4096];
    size_t n = 0;
    while (n + 1 < sizeof(resp)) {
        if (http_conn_recv(c, resp + n, 1) <= 0) {
            fprintf(stderr, "[http] proxy %s: no answer to CONNECT %s\n", label, target);
            return 0;
        }
        n++;
        if (n >= 4 && !memcmp(resp + n - 4, "\r\n\r\n", 4)) break;
    }
    resp[n] = 0;
    int status = 0;
    if (sscanf(resp, "HTTP/%*s %d", &status) != 1 || status < 200 || status > 299) {
        fprintf(stderr, "[http] proxy %s refused CONNECT %s (HTTP %d%s)\n", label, target, status,
                status == 407 ? ": proxy authentication required" : "");
        return 0;
    }
    return 1;
}

// SOCKS5 CONNECT (RFC 1928; user / password RFC 1929). socks5h:// hands the proxy the name,
// socks5:// the address this client resolved.
static int socks5_tunnel(HttpConn *c, const HttpProxy *px, const HttpAddr *a, const char *label) {
    static const char *const REPLIES[] = {"", "general failure", "not allowed by the ruleset", "network unreachable",
                                          "host unreachable", "connection refused", "TTL expired", "command not supported",
                                          "address type not supported"};
    const int auth = px->user[0] != 0;
    const unsigned char hello[4] = {5, (unsigned char) (auth ? 2 : 1), 0, 2}; // no authentication [, user / password]
    unsigned char r[262];
    if (!http_conn_send(c, hello, auth ? 4 : 3) || !recv_exact(c, r, 2) || r[0] != 5) {
        fprintf(stderr, "[http] proxy %s: not a SOCKS5 proxy\n", label);
        return 0;
    }
    if (r[1] == 2 && auth) {
        unsigned char msg[3 + 255 + 255];
        const size_t ul = strlen(px->user) > 255 ? 255 : strlen(px->user), pl = strlen(px->pass) > 255 ? 255 : strlen(px->pass);
        size_t n = 0;
        msg[n++] = 1;
        msg[n++] = (unsigned char) ul;
        memcpy(msg + n, px->user, ul);
        n += ul;
        msg[n++] = (unsigned char) pl;
        memcpy(msg + n, px->pass, pl);
        n += pl;
        if (!http_conn_send(c, msg, n) || !recv_exact(c, r, 2) || r[1] != 0) {
            fprintf(stderr, "[http] proxy %s: SOCKS5 login refused\n", label);
            return 0;
        }
    } else if (r[1] != 0) {
        fprintf(stderr, "[http] proxy %s: SOCKS5 wants %s\n", label, r[1] == 2 ? "a user name and password" : "an unsupported login");
        return 0;
    }
    unsigned char req[4 + 1 + 255 + 2];
    size_t n = 0;
    req[n++] = 5;
    req[n++] = 1; // CONNECT
    req[n++] = 0;
    const size_t hl = strlen(a->host);
    if (px->kind == HTTP_PROXY_SOCKS5H) {
        if (hl > 255) return 0;
        req[n++] = 3; // DOMAINNAME
        req[n++] = (unsigned char) hl;
        memcpy(req + n, a->host, hl);
        n += hl;
    } else {
        struct addrinfo hints = {0}, *res = NULL;
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(a->host, NULL, &hints, &res) != 0 || !res) {
            fprintf(stderr, "[http] proxy %s: cannot resolve %s (socks5h:// lets the proxy resolve it)\n", label, a->host);
            return 0;
        }
        if (res->ai_family == AF_INET6) {
            req[n++] = 4;
            memcpy(req + n, &((const struct sockaddr_in6 *) res->ai_addr)->sin6_addr, 16);
            n += 16;
        } else {
            req[n++] = 1;
            memcpy(req + n, &((const struct sockaddr_in *) res->ai_addr)->sin_addr, 4);
            n += 4;
        }
        freeaddrinfo(res);
    }
    const int port = atoi(a->port);
    req[n++] = (unsigned char) (port >> 8);
    req[n++] = (unsigned char) port;
    if (!http_conn_send(c, req, n) || !recv_exact(c, r, 4) || r[0] != 5) {
        fprintf(stderr, "[http] proxy %s: no SOCKS5 answer for %s\n", label, a->host);
        return 0;
    }
    if (r[1] != 0) {
        fprintf(stderr, "[http] proxy %s: SOCKS5 CONNECT %s:%s failed: %s\n", label, a->host, a->port,
                r[1] < sizeof(REPLIES) / sizeof(REPLIES[0]) ? REPLIES[r[1]] : "error");
        return 0;
    }
    size_t skip = r[3] == 1 ? 4 : r[3] == 4 ? 16 : 0; // BND.ADDR, then BND.PORT
    if (r[3] == 3) {
        if (!recv_exact(c, r, 1)) return 0;
        skip = r[0];
    }
    return recv_exact(c, r, skip + 2);
}

// Connects (and runs the TLS handshake, verifying the certificate for a->host). 0 on failure.
int http_conn_open(HttpConn *c, const HttpAddr *a, const int timeout_ms) {
    memset(c, 0, sizeof(*c));
    c->fd = -1;
    HttpProxy px;
    const int via = http_proxy_for(a->host, a->tls, &px);
    char label[160] = "";
    if (via) {
        http_proxy_label(&px, label, sizeof(label));
        HttpAddr pa;
        memset(&pa, 0, sizeof(pa));
        snprintf(pa.host, sizeof(pa.host), "%s", px.host);
        snprintf(pa.port, sizeof(pa.port), "%s", px.port);
#ifndef KRUNKNATIVE_HAVE_TLS
        if (px.kind == HTTP_PROXY_HTTPS) {
            fprintf(stderr, "[http] proxy %s needs TLS, but this client was built without OpenSSL\n", label);
            return 0;
        }
#endif
        c->fd = connect_to(&pa, timeout_ms);
        if (c->fd < 0) fprintf(stderr, "[http] proxy %s is not reachable (for %s)\n", label, a->host);
    } else {
        c->fd = connect_to(a, timeout_ms);
    }
    if (c->fd < 0) return 0;
    struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if (via) {
#ifdef KRUNKNATIVE_HAVE_TLS
        if (px.kind == HTTP_PROXY_HTTPS && !(c->ssl = tls_start(c->fd, NULL, px.host))) goto fail; // CONNECT inside it
#endif
        const int ok = px.kind == HTTP_PROXY_SOCKS5 || px.kind == HTTP_PROXY_SOCKS5H ? socks5_tunnel(c, &px, a, label)
                                                                                    : connect_tunnel(c, &px, a, label);
        if (!ok) goto fail;
    }
#ifdef KRUNKNATIVE_HAVE_TLS
    if (a->tls) {
        SSL *under = c->ssl; // the https:// proxy's TLS, NULL otherwise
        SSL *ssl = tls_start(c->fd, under, a->host);
        if (!ssl) goto fail;
        c->proxy_ssl = under;
        c->ssl = ssl;
    }
#endif
    return 1;
fail:
    http_conn_close(c);
    return 0;
}

void http_conn_close(HttpConn *c) {
#ifdef KRUNKNATIVE_HAVE_TLS
    if (c->ssl) {
        SSL_shutdown(c->ssl);
        SSL_free(c->ssl); // a tunnelled one frees its BIO_f_ssl, not the proxy's SSL under it
        c->ssl = NULL;
    }
    if (c->proxy_ssl) {
        SSL_shutdown(c->proxy_ssl);
        SSL_free(c->proxy_ssl);
        c->proxy_ssl = NULL;
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
    if (c->ssl) return SSL_pending(c->ssl) > 0 || (c->proxy_ssl && SSL_pending(c->proxy_ssl) > 0);
#endif
    (void) c;
    return 0;
}
#endif
