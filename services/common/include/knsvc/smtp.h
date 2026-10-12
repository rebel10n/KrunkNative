/*
 * knsvc/smtp.h - outbound mail: a small SMTP submission client (RFC 5321 / 6409) with
 * STARTTLS (RFC 3207) or implicit TLS (RFC 8314, port 465) through OpenSSL libssl,
 * AUTH PLAIN / LOGIN (RFC 4954 / 4616), and an RFC 5322 + MIME message builder
 * (multipart/alternative text + HTML, UTF-8, quoted-printable bodies, RFC 2047 headers).
 *
 * kn_smtp_send() is blocking. Event-loop code uses a kn_mailer: a queue drained by one
 * worker thread that posts every result back to the loop (kn_loop_post_threadsafe), so the
 * loop never waits for DNS, TCP, TLS or the server. Credentials never appear in errors or
 * logs (AUTH lines are not traced).
 *
 * TLS needs knsvc built with libssl (KNSVC_HAVE_TLS); without it only tls = NONE works.
 * Peer verification uses the system CA store plus cfg.ca_file / KNSVC_TLS_CA_FILE and checks
 * the host name; verify_peer = 0 is for tests against self-signed servers only.
 */
#ifndef KNSVC_SMTP_H
#define KNSVC_SMTP_H

#include <stddef.h>

#include "knsvc/buf.h"
#include "knsvc/loop.h"

typedef enum kn_smtp_tls {
    KN_SMTP_TLS_NONE = 0,     /* clear text (local relays / tests); AUTH refused unless allow_plain_auth */
    KN_SMTP_TLS_STARTTLS = 1, /* EHLO, STARTTLS (required: missing capability = error), EHLO again */
    KN_SMTP_TLS_IMPLICIT = 2, /* TLS from the first byte ("secure: true", port 465) */
} kn_smtp_tls;

/* "none" | "starttls" | "implicit" (aliases "tls", "ssl", "smtps"); NULL / "" / "auto" = by port:
 * 465 implicit, else starttls. -1 for an unknown name. */
int kn_smtp_tls_parse(const char *name, int port);
const char *kn_smtp_tls_name(kn_smtp_tls tls);

typedef struct kn_smtp_config {
    const char *host;
    int port;                 /* 0 = 465 for IMPLICIT, else 587 */
    kn_smtp_tls tls;
    const char *user;         /* NULL / "" = no AUTH */
    const char *password;
    const char *auth;         /* "auto" (PLAIN, else LOGIN) | "plain" | "login"; NULL = auto */
    const char *helo;         /* EHLO name; NULL = gethostname() */
    int timeout_ms;           /* connect and every reply; 0 = 20000 */
    int verify_peer;          /* 1 = verify the certificate and the host name (set it!) */
    const char *ca_file;      /* extra PEM bundle (tests: the self-signed cert) */
    int allow_plain_auth;     /* allow AUTH over a clear-text connection (tests only) */
} kn_smtp_config;

typedef struct kn_mail {
    const char *from;         /* "Krunker <noreply@example.com>" or a bare address */
    const char *to;           /* one recipient address (no display name needed) */
    const char *reply_to;     /* optional */
    const char *subject;      /* UTF-8 */
    const char *text;         /* text/plain body, UTF-8 (required) */
    const char *html;         /* optional text/html alternative, UTF-8 */
    const char *list_unsubscribe; /* optional List-Unsubscribe value (e.g. "<mailto:...>") */
} kn_mail;

/* Bare address of "Name <addr>" / "addr" into out. 0 ok, -1 invalid (no '@', CR/LF, spaces,
 * angle brackets in the wrong place, too long). */
int kn_mail_address(const char *spec, char *out, size_t cap);
/* 1 when addr looks like a deliverable address: local@domain.tld, printable ASCII, <= 254. */
int kn_mail_address_valid(const char *addr);

/* The complete message (headers + MIME body, CRLF line ends, not dot-stuffed) into out.
 * message_id (cap >= 128) receives "<random@from-domain>". date_unix 0 = now.
 * 0 ok, -1 invalid input (header injection, missing from/to/text, bad address). */
int kn_mail_build(const kn_mail *m, long long date_unix, kn_buf *out, char *message_id, size_t message_id_cap);

/* Result of a send: OK, or an error class the caller can retry. */
enum {
    KN_SMTP_OK = 0,
    KN_SMTP_ETEMP = -1,       /* connection / TLS / timeout / 4xx: worth retrying later */
    KN_SMTP_EPERM = -2,       /* 5xx (rejected sender / recipient / auth) or invalid message */
};

/* Blocking: connect, (TLS,) EHLO, AUTH, MAIL / RCPT / DATA, QUIT. message_id gets the
 * Message-ID header value; err (optional) a credential-free reason ("550 5.7.1 ...",
 * "connect: ...", "tls: ..."). Returns KN_SMTP_OK / ETEMP / EPERM. */
int kn_smtp_send(const kn_smtp_config *cfg, const kn_mail *m, char *message_id, size_t message_id_cap, char *err,
                 size_t errcap);

/* Transcript hook for tests and debugging: every line sent ("C: ...", AUTH payloads
 * replaced by "<redacted>") and received ("S: ..."). NULL = off. Called on the sending thread. */
typedef void (*kn_smtp_trace_fn)(void *ud, const char *line);
void kn_smtp_set_trace(kn_smtp_trace_fn fn, void *ud);

/* ---- queued sending for event-loop code */

typedef struct kn_mailer kn_mailer;

/* status: KN_SMTP_OK / ETEMP (still failing after the retries) / EPERM. Runs on the loop. */
typedef void (*kn_mail_cb)(void *ud, int status, const char *message_id, const char *error);

typedef struct kn_mailer_options {
    int queue_max;            /* queued + in-flight mails; 0 = 256 (kn_mailer_send fails beyond) */
    int retries;              /* extra attempts after a temporary failure; 0 = 2, -1 = none */
    int retry_delay_ms;       /* first retry delay (doubles); 0 = 2000 */
} kn_mailer_options;

/* Copies cfg (strings included). NULL on failure. The worker starts at once. */
kn_mailer *kn_mailer_new(kn_loop *loop, const kn_smtp_config *cfg, const kn_mailer_options *opts);
/* Queue one mail (copied). cb (optional) runs on the loop thread later, never inside this
 * call. 0 queued, -1 queue full / invalid / allocation failure (cb not called). */
int kn_mailer_send(kn_mailer *mailer, const kn_mail *m, kn_mail_cb cb, void *ud);
/* Mails queued or being sent. */
int kn_mailer_pending(const kn_mailer *mailer);
/* Stop the worker (a session in progress is cut off: its socket is shut down) and free. Callbacks of
 * mails not sent yet are not called; results not yet delivered to the loop are dropped. */
void kn_mailer_free(kn_mailer *mailer);

#endif
