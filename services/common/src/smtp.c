/*
 * knsvc/smtp.h implementation: message builder, blocking SMTP client, queued mailer.
 *
 * The client runs on whatever thread calls kn_smtp_send(); the mailer gives it one
 * worker thread. Sockets are blocking with SO_RCVTIMEO / SO_SNDTIMEO (the connect has its
 * own poll timeout), so every step is bounded by cfg.timeout_ms.
 */
#include "knsvc/smtp.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "knsvc/crypto.h"
#include "knsvc/log.h"

#ifdef KNSVC_HAVE_TLS
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#endif

#ifndef MSG_NOSIGNAL /* macOS: SO_NOSIGPIPE on the socket instead */
#define MSG_NOSIGNAL 0
#endif
#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0
#endif

#define MOD "smtp"
#define LINE_MAX_BYTES 1000      /* RFC 5321 4.5.3.1.5 text line, CRLF included */
#define REPLY_MAX_BYTES 65536
#define ADDR_MAX 254

/* ------------------------------------------------------------------ names */

int kn_smtp_tls_parse(const char *name, int port) {
    if (!name || !*name || !strcasecmp(name, "auto")) return port == 465 ? KN_SMTP_TLS_IMPLICIT : KN_SMTP_TLS_STARTTLS;
    if (!strcasecmp(name, "none") || !strcasecmp(name, "plain") || !strcasecmp(name, "off")) return KN_SMTP_TLS_NONE;
    if (!strcasecmp(name, "starttls")) return KN_SMTP_TLS_STARTTLS;
    if (!strcasecmp(name, "implicit") || !strcasecmp(name, "tls") || !strcasecmp(name, "ssl") ||
        !strcasecmp(name, "smtps"))
        return KN_SMTP_TLS_IMPLICIT;
    return -1;
}

const char *kn_smtp_tls_name(kn_smtp_tls tls) {
    switch (tls) {
    case KN_SMTP_TLS_NONE: return "none";
    case KN_SMTP_TLS_STARTTLS: return "starttls";
    case KN_SMTP_TLS_IMPLICIT: return "implicit";
    }
    return "?";
}

/* ------------------------------------------------------------------ addresses */

static int has_crlf(const char *s) { return s && strpbrk(s, "\r\n") != NULL; }

int kn_mail_address_valid(const char *a) {
    if (!a) return 0;
    const size_t n = strlen(a);
    if (n < 3 || n > ADDR_MAX) return 0;
    const char *at = strrchr(a, '@');
    if (!at || at == a || strchr(a, '@') != at) return 0;
    const char *dom = at + 1;
    if (!*dom || !strchr(dom, '.') || dom[0] == '.' || dom[strlen(dom) - 1] == '.' || strstr(dom, "..")) return 0;
    if (at - a > 64) return 0;
    for (const char *p = a; *p; p++) {
        const unsigned char c = (unsigned char) *p;
        if (c <= 32 || c >= 127 || strchr("<>()[]\\,;:\"", c)) return 0;
    }
    for (const char *p = dom; *p; p++)
        if (!(*p == '-' || *p == '.' || (*p >= '0' && *p <= '9') || ((*p | 32) >= 'a' && (*p | 32) <= 'z'))) return 0;
    return 1;
}

int kn_mail_address(const char *spec, char *out, size_t cap) {
    if (!spec || !out || cap == 0 || has_crlf(spec)) return -1;
    const char *lt = strrchr(spec, '<');
    const char *b = spec, *e = spec + strlen(spec);
    if (lt) {
        const char *gt = strchr(lt, '>');
        if (!gt || gt[1] != 0) return -1;
        b = lt + 1;
        e = gt;
    } else if (strchr(spec, '>')) {
        return -1;
    }
    while (b < e && (*b == ' ' || *b == '\t')) b++;
    while (e > b && (e[-1] == ' ' || e[-1] == '\t')) e--;
    const size_t n = (size_t) (e - b);
    if (n + 1 > cap || n > ADDR_MAX) return -1;
    memcpy(out, b, n);
    out[n] = 0;
    return kn_mail_address_valid(out) ? 0 : -1;
}

/* Display name of "Name <addr>" (trimmed, outer quotes removed), "" when none. */
static void display_name(const char *spec, char *out, size_t cap) {
    out[0] = 0;
    const char *lt = strrchr(spec, '<');
    if (!lt) return;
    const char *b = spec, *e = lt;
    while (b < e && (*b == ' ' || *b == '\t')) b++;
    while (e > b && (e[-1] == ' ' || e[-1] == '\t')) e--;
    if (e - b >= 2 && *b == '"' && e[-1] == '"') {
        b++;
        e--;
    }
    size_t n = (size_t) (e - b);
    if (n >= cap) n = cap - 1;
    memcpy(out, b, n);
    out[n] = 0;
}

/* ------------------------------------------------------------------ message builder */

static int is_ascii_printable(const char *s) {
    for (; *s; s++)
        if ((unsigned char) *s < 32 || (unsigned char) *s > 126) return 0;
    return 1;
}

/* RFC 2047 "B" encoded words for a header value, folded with CRLF SP between words.
 * Splits only at UTF-8 character boundaries, <= 45 input bytes per word (60 output). */
static int append_encoded_words(kn_buf *out, const char *s) {
    const size_t n = strlen(s);
    size_t i = 0;
    int first = 1;
    while (i < n) {
        size_t take = n - i > 45 ? 45 : n - i;
        while (take > 0 && i + take < n && ((unsigned char) s[i + take] & 0xC0) == 0x80) take--;
        if (take == 0) take = n - i > 45 ? 45 : n - i;
        char enc[80];
        if (kn_base64_encode(s + i, take, enc, sizeof enc, 0, 1) < 0) return -1;
        if (kn_buf_appendf(out, "%s=?UTF-8?B?%s?=", first ? "" : "\r\n ", enc) != 0) return -1;
        first = 0;
        i += take;
    }
    return 0;
}

/* Header value: plain when printable ASCII and short, encoded words otherwise. */
static int append_text_header(kn_buf *out, const char *name, const char *value) {
    if (kn_buf_appendf(out, "%s: ", name) != 0) return -1;
    if (is_ascii_printable(value) && strlen(value) <= 900) {
        if (kn_buf_append_str(out, value) != 0) return -1;
    } else if (append_encoded_words(out, value) != 0) {
        return -1;
    }
    return kn_buf_append_str(out, "\r\n");
}

/* "Name <addr>" with the name as an atom, a quoted string or encoded words. */
static int append_mailbox(kn_buf *out, const char *name, const char *addr) {
    if (!name[0]) return kn_buf_appendf(out, "%s", addr);
    if (!is_ascii_printable(name)) {
        if (append_encoded_words(out, name) != 0) return -1;
    } else if (strpbrk(name, "()<>[]:;@\\,.\"")) {
        if (kn_buf_append_byte(out, '"') != 0) return -1;
        for (const char *p = name; *p; p++) {
            if ((*p == '"' || *p == '\\') && kn_buf_append_byte(out, '\\') != 0) return -1;
            if (kn_buf_append_byte(out, (uint8_t) *p) != 0) return -1;
        }
        if (kn_buf_append_byte(out, '"') != 0) return -1;
    } else if (kn_buf_append_str(out, name) != 0) {
        return -1;
    }
    return kn_buf_appendf(out, " <%s>", addr);
}

/* Quoted-printable body (RFC 2045 6.7): CRLF hard breaks, soft breaks before 76 columns,
 * trailing white space and a leading '.' encoded. */
static int append_qp(kn_buf *out, const char *s) {
    static const char hex[] = "0123456789ABCDEF";
    int col = 0;
    for (const unsigned char *p = (const unsigned char *) s; *p; p++) {
        if (*p == '\r' && p[1] == '\n') continue;
        if (*p == '\n' || *p == '\r') {
            if (kn_buf_append_str(out, "\r\n") != 0) return -1;
            col = 0;
            continue;
        }
        const int at_eol = p[1] == 0 || p[1] == '\n' || (p[1] == '\r');
        const int lit = (*p >= 33 && *p <= 126 && *p != '=' && !(col == 0 && *p == '.')) ||
                        ((*p == ' ' || *p == '\t') && !at_eol);
        const int w = lit ? 1 : 3;
        if (col + w > 75) {
            if (kn_buf_append_str(out, "=\r\n") != 0) return -1;
            col = 0;
            if (*p == '.') { /* would start a line now */
                if (kn_buf_append_str(out, "=2E") != 0) return -1;
                col = 3;
                continue;
            }
        }
        if (lit) {
            if (kn_buf_append_byte(out, *p) != 0) return -1;
        } else {
            char e[4] = {'=', hex[*p >> 4], hex[*p & 15], 0};
            if (kn_buf_append_str(out, e) != 0) return -1;
        }
        col += w;
    }
    return kn_buf_append_str(out, "\r\n");
}

static void rfc5322_date(long long t, char *out, size_t cap) {
    static const char *const days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char *const months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                         "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    const time_t tt = (time_t) t;
    struct tm tm;
    gmtime_r(&tt, &tm);
    snprintf(out, cap, "%s, %02d %s %04d %02d:%02d:%02d +0000", days[tm.tm_wday % 7], tm.tm_mday,
             months[tm.tm_mon % 12], tm.tm_year + 1900, tm.tm_hour, tm.tm_min, tm.tm_sec);
}

int kn_mail_build(const kn_mail *m, long long date_unix, kn_buf *out, char *message_id, size_t message_id_cap) {
    if (!m || !out || !m->from || !m->to || !m->text) return -1;
    if (has_crlf(m->from) || has_crlf(m->to) || has_crlf(m->subject) || has_crlf(m->reply_to) ||
        has_crlf(m->list_unsubscribe))
        return -1;
    char from[ADDR_MAX + 1], to[ADDR_MAX + 1], reply[ADDR_MAX + 1], name[128];
    if (kn_mail_address(m->from, from, sizeof from) != 0 || kn_mail_address(m->to, to, sizeof to) != 0) return -1;
    if (m->reply_to && *m->reply_to && kn_mail_address(m->reply_to, reply, sizeof reply) != 0) return -1;
    display_name(m->from, name, sizeof name);

    char rnd[33], id[160], boundary[48], date[48];
    if (kn_random_hex(rnd, sizeof rnd, 16) != 0) return -1;
    snprintf(id, sizeof id, "<%s@%s>", rnd, strrchr(from, '@') + 1);
    if (kn_random_hex(rnd, sizeof rnd, 12) != 0) return -1;
    snprintf(boundary, sizeof boundary, "=_kn_%s", rnd);
    rfc5322_date(date_unix ? date_unix : (long long) time(NULL), date, sizeof date);

    int rc = 0;
    rc |= kn_buf_appendf(out, "Date: %s\r\n", date);
    rc |= kn_buf_append_str(out, "From: ");
    rc |= append_mailbox(out, name, from);
    rc |= kn_buf_appendf(out, "\r\nTo: %s\r\n", to);
    if (m->reply_to && *m->reply_to) rc |= kn_buf_appendf(out, "Reply-To: %s\r\n", reply);
    rc |= append_text_header(out, "Subject", m->subject ? m->subject : "");
    rc |= kn_buf_appendf(out, "Message-ID: %s\r\n", id);
    rc |= kn_buf_append_str(out, "MIME-Version: 1.0\r\nAuto-Submitted: auto-generated\r\n");
    if (m->list_unsubscribe && *m->list_unsubscribe)
        rc |= kn_buf_appendf(out, "List-Unsubscribe: %s\r\n", m->list_unsubscribe);
    if (m->html && *m->html) {
        rc |= kn_buf_appendf(out, "Content-Type: multipart/alternative; boundary=\"%s\"\r\n\r\n", boundary);
        rc |= kn_buf_append_str(out, "This is a multi-part message in MIME format.\r\n");
        rc |= kn_buf_appendf(out, "\r\n--%s\r\nContent-Type: text/plain; charset=UTF-8\r\n"
                                  "Content-Transfer-Encoding: quoted-printable\r\n\r\n", boundary);
        rc |= append_qp(out, m->text);
        rc |= kn_buf_appendf(out, "\r\n--%s\r\nContent-Type: text/html; charset=UTF-8\r\n"
                                  "Content-Transfer-Encoding: quoted-printable\r\n\r\n", boundary);
        rc |= append_qp(out, m->html);
        rc |= kn_buf_appendf(out, "\r\n--%s--\r\n", boundary);
    } else {
        rc |= kn_buf_append_str(out, "Content-Type: text/plain; charset=UTF-8\r\n"
                                     "Content-Transfer-Encoding: quoted-printable\r\n\r\n");
        rc |= append_qp(out, m->text);
    }
    if (rc != 0) return -1;
    if (message_id && message_id_cap) snprintf(message_id, message_id_cap, "%s", id);
    return 0;
}

/* ------------------------------------------------------------------ connection */

static kn_smtp_trace_fn s_trace;
static void *s_trace_ud;

void kn_smtp_set_trace(kn_smtp_trace_fn fn, void *ud) {
    s_trace = fn;
    s_trace_ud = ud;
}

typedef struct Conn {
    int fd;
    int timeout_ms;
#ifdef KNSVC_HAVE_TLS
    SSL_CTX *ctx;
    SSL *ssl;
#endif
    char rbuf[4096];
    size_t rpos, rlen;
    char *err;
    size_t errcap;
    const char *host;
    int *fd_slot;             /* kn_mailer: the connected fd, so kn_mailer_free can shut it down */
    pthread_mutex_t *fd_lock;
} Conn;

static void publish_fd(Conn *c, int fd) {
    if (!c->fd_slot) return;
    pthread_mutex_lock(c->fd_lock);
    *c->fd_slot = fd;
    pthread_mutex_unlock(c->fd_lock);
}

static void set_err(Conn *c, const char *fmt, ...) KN_PRINTF(2, 3);
static void set_err(Conn *c, const char *fmt, ...) {
    if (!c->err || !c->errcap || c->err[0]) return; /* keep the first (root) cause */
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->err, c->errcap, fmt, ap);
    va_end(ap);
}

static void trace(const char *dir, const char *line, size_t n) {
    if (!s_trace) return;
    char buf[LINE_MAX_BYTES + 8];
    while (n > 0 && (line[n - 1] == '\r' || line[n - 1] == '\n')) n--;
    if (n > LINE_MAX_BYTES) n = LINE_MAX_BYTES;
    snprintf(buf, sizeof buf, "%s%.*s", dir, (int) n, line);
    s_trace(s_trace_ud, buf);
}

static int tcp_connect(Conn *c, const char *host, int port) {
    char ports[16];
    snprintf(ports, sizeof ports, "%d", port);
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    const int gai = getaddrinfo(host, ports, &hints, &res);
    if (gai != 0) {
        set_err(c, "resolve %s: %s", host, gai_strerror(gai));
        return -1;
    }
    int last_errno = 0;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        const int fd = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
        if (fd < 0) {
            last_errno = errno;
            continue;
        }
        const int fl = fcntl(fd, F_GETFL);
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        int ok = connect(fd, ai->ai_addr, ai->ai_addrlen) == 0;
        if (!ok && errno == EINPROGRESS) {
            struct pollfd pfd = {.fd = fd, .events = POLLOUT};
            int pr;
            do pr = poll(&pfd, 1, c->timeout_ms);
            while (pr < 0 && errno == EINTR);
            int soerr = 0;
            socklen_t sl = sizeof soerr;
            if (pr == 1 && getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) == 0 && soerr == 0) ok = 1;
            else last_errno = pr == 0 ? ETIMEDOUT : (soerr ? soerr : errno);
        } else if (!ok) {
            last_errno = errno;
        }
        if (!ok) {
            close(fd);
            continue;
        }
        fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
        struct timeval tv = {.tv_sec = c->timeout_ms / 1000, .tv_usec = (c->timeout_ms % 1000) * 1000};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#ifdef SO_NOSIGPIPE
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        c->fd = fd;
        publish_fd(c, fd);
        freeaddrinfo(res);
        return 0;
    }
    freeaddrinfo(res);
    set_err(c, "connect %s:%d: %s", host, port, strerror(last_errno ? last_errno : ECONNREFUSED));
    return -1;
}

#ifdef KNSVC_HAVE_TLS
static int tls_start(Conn *c, const kn_smtp_config *cfg) {
    struct sigaction sa;
    if (sigaction(SIGPIPE, NULL, &sa) == 0 && sa.sa_handler == SIG_DFL) signal(SIGPIPE, SIG_IGN);
    c->ctx = SSL_CTX_new(TLS_client_method());
    if (!c->ctx) {
        set_err(c, "tls: SSL_CTX_new failed");
        return -1;
    }
    SSL_CTX_set_min_proto_version(c->ctx, TLS1_2_VERSION);
    if (cfg->verify_peer) {
        SSL_CTX_set_verify(c->ctx, SSL_VERIFY_PEER, NULL);
        if (SSL_CTX_set_default_verify_paths(c->ctx) != 1) KN_WARN(MOD, "tls: cannot load the system CA store");
        const char *extra[2] = {cfg->ca_file, getenv("KNSVC_TLS_CA_FILE")};
        for (int i = 0; i < 2; i++)
            if (extra[i] && *extra[i] && SSL_CTX_load_verify_locations(c->ctx, extra[i], NULL) != 1)
                KN_WARN(MOD, "tls: cannot load CA file %s", extra[i]);
    } else {
        SSL_CTX_set_verify(c->ctx, SSL_VERIFY_NONE, NULL);
    }
    c->ssl = SSL_new(c->ctx);
    if (!c->ssl || SSL_set_fd(c->ssl, c->fd) != 1) {
        set_err(c, "tls: SSL_new failed");
        return -1;
    }
    if (cfg->verify_peer) {
        X509_VERIFY_PARAM *vp = SSL_get0_param(c->ssl);
        X509_VERIFY_PARAM_set_hostflags(vp, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
        if (X509_VERIFY_PARAM_set1_ip_asc(vp, c->host) != 1 && SSL_set1_host(c->ssl, c->host) != 1) {
            set_err(c, "tls: bad host name");
            return -1;
        }
    }
    SSL_set_tlsext_host_name(c->ssl, c->host);
    ERR_clear_error();
    if (SSL_connect(c->ssl) != 1) {
        const long vr = SSL_get_verify_result(c->ssl);
        const unsigned long e = ERR_get_error();
        if (vr != X509_V_OK) set_err(c, "tls: certificate verify failed for %s: %s", c->host, X509_verify_cert_error_string(vr));
        else if (e) set_err(c, "tls: %s", ERR_reason_error_string(e) ? ERR_reason_error_string(e) : "handshake failed");
        else set_err(c, "tls: handshake failed (%s)", errno ? strerror(errno) : "connection closed");
        ERR_clear_error();
        return -1;
    }
    KN_DEBUG(MOD, "%s: %s %s", c->host, SSL_get_version(c->ssl), SSL_get_cipher_name(c->ssl));
    return 0;
}
#endif

static void conn_close(Conn *c) {
#ifdef KNSVC_HAVE_TLS
    if (c->ssl) {
        SSL_shutdown(c->ssl);
        SSL_free(c->ssl);
        c->ssl = NULL;
    }
    if (c->ctx) SSL_CTX_free(c->ctx);
    c->ctx = NULL;
#endif
    if (c->fd >= 0) {
        publish_fd(c, -1);
        close(c->fd);
    }
    c->fd = -1;
}

static int conn_write(Conn *c, const char *p, size_t n) {
    while (n > 0) {
        ssize_t w;
#ifdef KNSVC_HAVE_TLS
        if (c->ssl) {
            const int r = SSL_write(c->ssl, p, n > 16384 ? 16384 : (int) n);
            if (r <= 0) {
                set_err(c, "write: %s", SSL_get_error(c->ssl, r) == SSL_ERROR_SYSCALL && errno ? strerror(errno) : "tls error");
                ERR_clear_error();
                return -1;
            }
            w = r;
        } else
#endif
        {
            w = send(c->fd, p, n, MSG_NOSIGNAL);
            if (w < 0 && errno == EINTR) continue;
            if (w <= 0) {
                set_err(c, "write: %s", errno == EAGAIN || errno == EWOULDBLOCK ? "timeout" : strerror(errno));
                return -1;
            }
        }
        p += w;
        n -= (size_t) w;
    }
    return 0;
}

static int conn_fill(Conn *c) {
    if (c->rpos > 0) {
        memmove(c->rbuf, c->rbuf + c->rpos, c->rlen - c->rpos);
        c->rlen -= c->rpos;
        c->rpos = 0;
    }
    if (c->rlen >= sizeof c->rbuf) {
        set_err(c, "reply line too long");
        return -1;
    }
    for (;;) {
        ssize_t r;
#ifdef KNSVC_HAVE_TLS
        if (c->ssl) {
            const int n = SSL_read(c->ssl, c->rbuf + c->rlen, (int) (sizeof c->rbuf - c->rlen));
            if (n <= 0) {
                const int e = SSL_get_error(c->ssl, n);
                set_err(c, "read: %s", e == SSL_ERROR_ZERO_RETURN ? "connection closed"
                                       : (errno == EAGAIN || errno == EWOULDBLOCK) ? "timeout"
                                       : errno ? strerror(errno) : "tls error");
                ERR_clear_error();
                return -1;
            }
            r = n;
        } else
#endif
        {
            r = recv(c->fd, c->rbuf + c->rlen, sizeof c->rbuf - c->rlen, 0);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) {
                set_err(c, "read: %s", r == 0 ? "connection closed"
                                       : (errno == EAGAIN || errno == EWOULDBLOCK) ? "timeout" : strerror(errno));
                return -1;
            }
        }
        c->rlen += (size_t) r;
        return 0;
    }
}

/* One line without CRLF into out. 0 ok. */
static int conn_line(Conn *c, char *out, size_t cap) {
    for (;;) {
        char *nl = memchr(c->rbuf + c->rpos, '\n', c->rlen - c->rpos);
        if (nl) {
            size_t n = (size_t) (nl - (c->rbuf + c->rpos));
            const char *b = c->rbuf + c->rpos;
            c->rpos += n + 1;
            if (n > 0 && b[n - 1] == '\r') n--;
            if (n >= cap) n = cap - 1;
            memcpy(out, b, n);
            out[n] = 0;
            trace("S: ", out, n);
            return 0;
        }
        if (conn_fill(c) != 0) return -1;
    }
}

typedef struct Reply {
    int code;
    char text[512];      /* the last line, "250 2.0.0 Ok: queued as X" */
    kn_buf lines;        /* every line's text after the code, '\n'-separated (EHLO keywords) */
} Reply;

/* Read a (multi-line) reply. Returns the code, -1 on I/O or protocol errors. */
static int read_reply(Conn *c, Reply *r) {
    kn_buf_reset(&r->lines);
    r->code = 0;
    r->text[0] = 0;
    char line[LINE_MAX_BYTES + 1];
    for (;;) {
        if (conn_line(c, line, sizeof line) != 0) return -1;
        if (strlen(line) < 3 || line[0] < '2' || line[0] > '5' || line[1] < '0' || line[1] > '9' || line[2] < '0' ||
            line[2] > '9' || (line[3] && line[3] != ' ' && line[3] != '-')) {
            set_err(c, "malformed reply");
            return -1;
        }
        const int code = (line[0] - '0') * 100 + (line[1] - '0') * 10 + (line[2] - '0');
        if (r->code && code != r->code) {
            set_err(c, "malformed multi-line reply");
            return -1;
        }
        r->code = code;
        kn_buf_appendf(&r->lines, "%s\n", line[3] ? line + 4 : "");
        if (r->lines.len > REPLY_MAX_BYTES) {
            set_err(c, "reply too long");
            return -1;
        }
        if (line[3] != '-') {
            size_t n = strlen(line);
            if (n >= sizeof r->text) n = sizeof r->text - 1;
            memcpy(r->text, line, n);
            r->text[n] = 0;
            return code;
        }
    }
}

/* Send one command line (CRLF appended). redact: trace "<redacted>" instead. */
static int command(Conn *c, const char *line, int redact) {
    if (redact) trace("C: ", "<redacted>", 10);
    else trace("C: ", line, strlen(line));
    if (strlen(line) > LINE_MAX_BYTES - 2) {
        set_err(c, "command too long");
        return -1;
    }
    if (conn_write(c, line, strlen(line)) != 0 || conn_write(c, "\r\n", 2) != 0) return -1;
    return 0;
}

/* Command + reply, the reply's code must be `want` (or in [want, want+99] when want % 100 == 0). */
static int exchange(Conn *c, Reply *r, const char *line, int redact, int want, const char *step, int *status) {
    if (line && command(c, line, redact) != 0) {
        *status = KN_SMTP_ETEMP;
        return -1;
    }
    const int code = read_reply(c, r);
    if (code < 0) {
        *status = KN_SMTP_ETEMP;
        return -1;
    }
    const int ok = want % 100 == 0 ? (code >= want && code < want + 100) : code == want;
    if (!ok) {
        set_err(c, "%s: %s", step, r->text);
        *status = code >= 500 ? KN_SMTP_EPERM : KN_SMTP_ETEMP;
        return -1;
    }
    return 0;
}

/* EHLO keywords: STARTTLS, AUTH mechanisms. */
typedef struct Caps {
    int starttls, auth_plain, auth_login, size_limit;
    long long max_size;
} Caps;

static void parse_caps(const Reply *r, Caps *caps) {
    memset(caps, 0, sizeof *caps);
    const char *p = r->lines.data ? r->lines.data : "";
    const char *nl = strchr(p, '\n');
    if (!nl) return;
    for (p = nl + 1; *p; p = nl + 1) { /* skip the greeting line */
        nl = strchr(p, '\n');
        if (!nl) break;
        char kw[256];
        size_t n = (size_t) (nl - p);
        if (n >= sizeof kw) n = sizeof kw - 1;
        memcpy(kw, p, n);
        kw[n] = 0;
        if (!strcasecmp(kw, "STARTTLS")) caps->starttls = 1;
        if (!strncasecmp(kw, "AUTH", 4) && (kw[4] == ' ' || kw[4] == '=')) {
            for (char *tok = strtok(kw + 5, " "); tok; tok = strtok(NULL, " ")) {
                if (!strcasecmp(tok, "PLAIN")) caps->auth_plain = 1;
                if (!strcasecmp(tok, "LOGIN")) caps->auth_login = 1;
            }
        }
        if (!strncasecmp(kw, "SIZE ", 5)) {
            caps->max_size = atoll(kw + 5);
            caps->size_limit = caps->max_size > 0;
        }
    }
}

static int auth(Conn *c, Reply *r, const kn_smtp_config *cfg, const Caps *caps, int *status) {
    const char *want = cfg->auth && *cfg->auth ? cfg->auth : "auto";
    int plain = caps->auth_plain, login = caps->auth_login;
    if (!strcasecmp(want, "plain")) login = 0;
    else if (!strcasecmp(want, "login")) plain = 0;
    if (!plain && !login) {
        set_err(c, "auth: the server offers no usable mechanism (PLAIN / LOGIN)");
        *status = KN_SMTP_EPERM;
        return -1;
    }
    const char *pass = cfg->password ? cfg->password : "";
    const size_t ul = strlen(cfg->user), pl = strlen(pass);
    if (ul > 255 || pl > 255) {
        set_err(c, "auth: credentials too long");
        *status = KN_SMTP_EPERM;
        return -1;
    }
    char raw[520], enc[720], line[740];
    int rc;
    if (plain) {
        raw[0] = 0;
        memcpy(raw + 1, cfg->user, ul);
        raw[1 + ul] = 0;
        memcpy(raw + 2 + ul, pass, pl);
        rc = kn_base64_encode(raw, 2 + ul + pl, enc, sizeof enc, 0, 1) < 0 ? -1 : 0;
        snprintf(line, sizeof line, "AUTH PLAIN %s", enc);
        if (rc == 0) rc = exchange(c, r, line, 1, 235, "auth", status);
    } else {
        rc = exchange(c, r, "AUTH LOGIN", 0, 334, "auth", status);
        if (rc == 0) {
            kn_base64_encode(cfg->user, ul, enc, sizeof enc, 0, 1);
            rc = exchange(c, r, enc, 1, 334, "auth", status);
        }
        if (rc == 0) {
            kn_base64_encode(pass, pl, enc, sizeof enc, 0, 1);
            rc = exchange(c, r, enc, 1, 235, "auth", status);
        }
    }
    memset(raw, 0, sizeof raw);
    memset(enc, 0, sizeof enc);
    memset(line, 0, sizeof line);
    return rc;
}

/* The message with dot-stuffing (RFC 5321 4.5.2) and the terminating ".". */
static int send_data(Conn *c, const kn_buf *msg) {
    kn_buf out;
    kn_buf_init(&out);
    int bol = 1, rc = 0;
    for (size_t i = 0; i < msg->len && rc == 0; i++) {
        const char ch = msg->data[i];
        if (bol && ch == '.') rc |= kn_buf_append_byte(&out, '.');
        rc |= kn_buf_append_byte(&out, (uint8_t) ch);
        bol = ch == '\n';
    }
    if (!bol) rc |= kn_buf_append_str(&out, "\r\n");
    rc |= kn_buf_append_str(&out, ".\r\n");
    if (rc == 0) {
        trace("C: ", "<message data>", 14);
        rc = conn_write(c, out.data, out.len);
    }
    kn_buf_free(&out);
    return rc;
}

static void default_helo(char *out, size_t cap) {
    if (gethostname(out, cap) != 0 || !out[0]) snprintf(out, cap, "localhost");
    out[cap - 1] = 0;
    for (char *p = out; *p; p++)
        if (!(*p == '-' || *p == '.' || (*p >= '0' && *p <= '9') || ((*p | 32) >= 'a' && (*p | 32) <= 'z'))) *p = '-';
}

static int smtp_send(const kn_smtp_config *cfg, const kn_mail *m, char *message_id, size_t message_id_cap, char *err,
                     size_t errcap, int *fd_slot, pthread_mutex_t *fd_lock) {
    char localerr[256];
    if (!err || !errcap) {
        err = localerr;
        errcap = sizeof localerr;
    }
    err[0] = 0;
    if (message_id && message_id_cap) message_id[0] = 0;
    if (!cfg || !cfg->host || !*cfg->host || !m) {
        snprintf(err, errcap, "smtp: no host configured");
        return KN_SMTP_EPERM;
    }
#ifndef KNSVC_HAVE_TLS
    if (cfg->tls != KN_SMTP_TLS_NONE) {
        snprintf(err, errcap, "smtp: TLS unavailable (knsvc built without libssl)");
        return KN_SMTP_EPERM;
    }
#endif
    const int have_auth = cfg->user && *cfg->user;
    if (have_auth && cfg->tls == KN_SMTP_TLS_NONE && !cfg->allow_plain_auth) {
        snprintf(err, errcap, "smtp: refusing AUTH over a clear-text connection");
        return KN_SMTP_EPERM;
    }
    char from[ADDR_MAX + 1], to[ADDR_MAX + 1], mid[160];
    kn_buf msg;
    kn_buf_init(&msg);
    if (kn_mail_address(m->from, from, sizeof from) != 0 || kn_mail_address(m->to, to, sizeof to) != 0 ||
        kn_mail_build(m, 0, &msg, mid, sizeof mid) != 0) {
        kn_buf_free(&msg);
        snprintf(err, errcap, "smtp: invalid message (address or header)");
        return KN_SMTP_EPERM;
    }

    Conn c = {.fd = -1, .timeout_ms = cfg->timeout_ms > 0 ? cfg->timeout_ms : 20000, .err = err, .errcap = errcap,
              .host = cfg->host, .fd_slot = fd_slot, .fd_lock = fd_lock};
    Reply r = {0};
    kn_buf_init(&r.lines);
    Caps caps;
    int status = KN_SMTP_ETEMP;
    char helo[128], line[600];
    if (cfg->helo && *cfg->helo && !has_crlf(cfg->helo) && strlen(cfg->helo) < sizeof helo) snprintf(helo, sizeof helo, "%s", cfg->helo);
    else default_helo(helo, sizeof helo);
    snprintf(line, sizeof line, "EHLO %s", helo);
    const int port = cfg->port > 0 ? cfg->port : cfg->tls == KN_SMTP_TLS_IMPLICIT ? 465 : 587;

    if (tcp_connect(&c, cfg->host, port) != 0) goto done;
#ifdef KNSVC_HAVE_TLS
    if (cfg->tls == KN_SMTP_TLS_IMPLICIT && tls_start(&c, cfg) != 0) goto done;
#endif
    if (exchange(&c, &r, NULL, 0, 220, "greeting", &status) != 0) goto done;
    if (exchange(&c, &r, line, 0, 250, "ehlo", &status) != 0) goto done;
    parse_caps(&r, &caps);
#ifdef KNSVC_HAVE_TLS
    if (cfg->tls == KN_SMTP_TLS_STARTTLS) {
        if (!caps.starttls) {
            set_err(&c, "starttls: not offered by %s", cfg->host);
            status = KN_SMTP_EPERM;
            goto done;
        }
        if (exchange(&c, &r, "STARTTLS", 0, 220, "starttls", &status) != 0) goto done;
        if (c.rpos != c.rlen) { /* bytes after 220 before the handshake: STARTTLS command injection */
            set_err(&c, "starttls: unexpected data before the TLS handshake");
            status = KN_SMTP_EPERM;
            goto done;
        }
        if (tls_start(&c, cfg) != 0) goto done;
        if (exchange(&c, &r, line, 0, 250, "ehlo", &status) != 0) goto done;
        parse_caps(&r, &caps);
    }
#endif
    if (caps.size_limit && (long long) msg.len > caps.max_size) {
        set_err(&c, "message larger than the server's SIZE limit");
        status = KN_SMTP_EPERM;
        goto done;
    }
    if (have_auth && auth(&c, &r, cfg, &caps, &status) != 0) goto done;
    snprintf(line, sizeof line, "MAIL FROM:<%s>", from);
    if (exchange(&c, &r, line, 0, 250, "mail from", &status) != 0) goto done;
    snprintf(line, sizeof line, "RCPT TO:<%s>", to);
    if (exchange(&c, &r, line, 0, 200, "rcpt to", &status) != 0) goto done;
    if (exchange(&c, &r, "DATA", 0, 354, "data", &status) != 0) goto done;
    if (send_data(&c, &msg) != 0) {
        status = KN_SMTP_ETEMP;
        goto done;
    }
    if (exchange(&c, &r, NULL, 0, 250, "data", &status) != 0) goto done;
    status = KN_SMTP_OK;
    if (message_id && message_id_cap) snprintf(message_id, message_id_cap, "%s", mid);
    if (command(&c, "QUIT", 0) == 0) {
        char quit_err[8] = "";
        c.err = quit_err; /* a failing QUIT does not matter anymore */
        c.errcap = sizeof quit_err;
        read_reply(&c, &r);
    }
done:
    if (status != KN_SMTP_OK && !err[0]) snprintf(err, errcap, "smtp: failed");
    if (status == KN_SMTP_OK) err[0] = 0;
    conn_close(&c);
    kn_buf_free(&r.lines);
    kn_buf_free(&msg);
    return status;
}

int kn_smtp_send(const kn_smtp_config *cfg, const kn_mail *m, char *message_id, size_t message_id_cap, char *err,
                 size_t errcap) {
    return smtp_send(cfg, m, message_id, message_id_cap, err, errcap, NULL, NULL);
}

/* ------------------------------------------------------------------ mailer (queue + worker) */

typedef struct MailJob {
    struct MailJob *next;
    kn_mail mail;
    char *strings;       /* one allocation holding every string of `mail` */
    kn_mail_cb cb;
    void *ud;
    int status;
    char message_id[160];
    char error[256];
    struct Shared *shared;
} MailJob;

/* State shared by the mailer, its worker and results posted to the loop. */
typedef struct Shared {
    pthread_mutex_t lock;
    int refs;            /* mailer + every posted result */
    int freed;           /* kn_mailer_free ran: results are dropped */
    int pending;         /* loop thread only */
} Shared;

struct kn_mailer {
    kn_loop *loop;
    kn_smtp_config cfg;
    char *cfg_strings;
    kn_mailer_options opts;
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    MailJob *head, *tail;
    int stop;
    int cur_fd;               /* the worker's connected socket (-1 none), under lock */
    Shared *shared;
};

static void shared_unref(Shared *s) {
    pthread_mutex_lock(&s->lock);
    const int left = --s->refs;
    pthread_mutex_unlock(&s->lock);
    if (left == 0) {
        pthread_mutex_destroy(&s->lock);
        free(s);
    }
}

/* Copies of NULL-terminated strings packed into one block; pointers into it. */
static char *pack_strings(const char **const *fields, size_t n) {
    size_t total = 0;
    for (size_t i = 0; i < n; i++)
        if (*fields[i]) total += strlen(*fields[i]) + 1;
    char *block = malloc(total ? total : 1);
    if (!block) return NULL;
    char *p = block;
    for (size_t i = 0; i < n; i++) {
        if (!*fields[i]) continue;
        const size_t l = strlen(*fields[i]) + 1;
        memcpy(p, *fields[i], l);
        *fields[i] = p;
        p += l;
    }
    return block;
}

static void job_free(MailJob *j) {
    if (!j) return;
    free(j->strings);
    free(j);
}

static void job_result(kn_loop *loop, void *ud) {
    (void) loop;
    MailJob *j = ud;
    Shared *s = j->shared;
    pthread_mutex_lock(&s->lock);
    const int freed = s->freed;
    pthread_mutex_unlock(&s->lock);
    if (!freed) {
        s->pending--;
        if (j->cb) j->cb(j->ud, j->status, j->message_id[0] ? j->message_id : NULL, j->error[0] ? j->error : NULL);
    }
    job_free(j);
    shared_unref(s);
}

static void *mailer_main(void *arg) {
    kn_mailer *mm = arg;
    for (;;) {
        pthread_mutex_lock(&mm->lock);
        while (!mm->head && !mm->stop) pthread_cond_wait(&mm->cond, &mm->lock);
        if (mm->stop) {
            pthread_mutex_unlock(&mm->lock);
            break;
        }
        MailJob *j = mm->head;
        mm->head = j->next;
        if (!mm->head) mm->tail = NULL;
        pthread_mutex_unlock(&mm->lock);

        int delay = mm->opts.retry_delay_ms;
        for (int attempt = 0;; attempt++) {
            j->error[0] = 0;
            j->status = smtp_send(&mm->cfg, &j->mail, j->message_id, sizeof j->message_id, j->error, sizeof j->error,
                                  &mm->cur_fd, &mm->lock);
            if (j->status != KN_SMTP_ETEMP || attempt >= mm->opts.retries) break;
            KN_WARN(MOD, "temporary failure (attempt %d/%d), retrying in %d ms: %s", attempt + 1, mm->opts.retries + 1,
                    delay, j->error);
            /* wait, but leave at once on stop */
            struct timespec until;
            clock_gettime(CLOCK_REALTIME, &until);
            until.tv_sec += delay / 1000;
            until.tv_nsec += (long) (delay % 1000) * 1000000L;
            if (until.tv_nsec >= 1000000000L) {
                until.tv_sec++;
                until.tv_nsec -= 1000000000L;
            }
            pthread_mutex_lock(&mm->lock);
            while (!mm->stop && pthread_cond_timedwait(&mm->cond, &mm->lock, &until) != ETIMEDOUT) {
            }
            const int stop = mm->stop;
            pthread_mutex_unlock(&mm->lock);
            if (stop) break;
            delay *= 2;
        }
        pthread_mutex_lock(&j->shared->lock);
        j->shared->refs++;
        pthread_mutex_unlock(&j->shared->lock);
        if (kn_loop_post_threadsafe(mm->loop, job_result, j) != 0) {
            shared_unref(j->shared);
            job_free(j);
        }
    }
    return NULL;
}

kn_mailer *kn_mailer_new(kn_loop *loop, const kn_smtp_config *cfg, const kn_mailer_options *opts) {
    if (!loop || !cfg) return NULL;
    kn_mailer *mm = calloc(1, sizeof *mm);
    Shared *s = calloc(1, sizeof *s);
    if (!mm || !s) {
        free(mm);
        free(s);
        return NULL;
    }
    mm->loop = loop;
    mm->cur_fd = -1;
    mm->cfg = *cfg;
    const char **fields[] = {&mm->cfg.host, &mm->cfg.user, &mm->cfg.password, &mm->cfg.auth, &mm->cfg.helo,
                             &mm->cfg.ca_file};
    mm->cfg_strings = pack_strings(fields, sizeof fields / sizeof fields[0]);
    if (opts) mm->opts = *opts;
    if (mm->opts.queue_max <= 0) mm->opts.queue_max = 256;
    if (mm->opts.retries == 0) mm->opts.retries = 2;
    if (mm->opts.retries < 0) mm->opts.retries = 0;
    if (mm->opts.retry_delay_ms <= 0) mm->opts.retry_delay_ms = 2000;
    pthread_mutex_init(&s->lock, NULL);
    s->refs = 1;
    mm->shared = s;
    pthread_mutex_init(&mm->lock, NULL);
    pthread_cond_init(&mm->cond, NULL);
    if (!mm->cfg_strings || pthread_create(&mm->thread, NULL, mailer_main, mm) != 0) {
        pthread_cond_destroy(&mm->cond);
        pthread_mutex_destroy(&mm->lock);
        pthread_mutex_destroy(&s->lock);
        free(s);
        if (mm->cfg_strings) {
            memset(mm->cfg_strings, 0, 1);
            free(mm->cfg_strings);
        }
        free(mm);
        return NULL;
    }
    return mm;
}

int kn_mailer_send(kn_mailer *mm, const kn_mail *m, kn_mail_cb cb, void *ud) {
    if (!mm || !m || !m->from || !m->to || !m->text) return -1;
    if (mm->shared->pending >= mm->opts.queue_max) return -1;
    MailJob *j = calloc(1, sizeof *j);
    if (!j) return -1;
    j->mail = *m;
    const char **fields[] = {&j->mail.from, &j->mail.to, &j->mail.reply_to, &j->mail.subject, &j->mail.text,
                             &j->mail.html, &j->mail.list_unsubscribe};
    j->strings = pack_strings(fields, sizeof fields / sizeof fields[0]);
    if (!j->strings) {
        free(j);
        return -1;
    }
    j->cb = cb;
    j->ud = ud;
    j->shared = mm->shared;
    mm->shared->pending++;
    pthread_mutex_lock(&mm->lock);
    if (mm->tail) mm->tail->next = j;
    else mm->head = j;
    mm->tail = j;
    pthread_cond_signal(&mm->cond);
    pthread_mutex_unlock(&mm->lock);
    return 0;
}

int kn_mailer_pending(const kn_mailer *mm) { return mm ? mm->shared->pending : 0; }

void kn_mailer_free(kn_mailer *mm) {
    if (!mm) return;
    pthread_mutex_lock(&mm->lock);
    mm->stop = 1;
    if (mm->cur_fd >= 0) shutdown(mm->cur_fd, SHUT_RDWR); /* a send in progress fails at once */
    pthread_cond_broadcast(&mm->cond);
    pthread_mutex_unlock(&mm->lock);
    pthread_join(mm->thread, NULL);
    for (MailJob *j = mm->head; j;) {
        MailJob *n = j->next;
        job_free(j);
        j = n;
    }
    pthread_mutex_lock(&mm->shared->lock);
    mm->shared->freed = 1;
    pthread_mutex_unlock(&mm->shared->lock);
    shared_unref(mm->shared);
    pthread_cond_destroy(&mm->cond);
    pthread_mutex_destroy(&mm->lock);
    if (mm->cfg.password) memset((char *) mm->cfg.password, 0, strlen(mm->cfg.password));
    free(mm->cfg_strings);
    free(mm);
}
