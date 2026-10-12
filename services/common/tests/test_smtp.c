/* knsvc/smtp.h: message builder units, and the client / mailer driven by tests/smtp_test.py
 * against tests/fake_smtp.py.
 *
 *   knsvc_test_smtp unit
 *   knsvc_test_smtp send  <port> <tls> <user|-> <pass|-> <cafile|-> <verify 0|1> <from> <to> [auth]
 *       -> "status <n>", "id <message-id>", "err <text>" lines on stdout, the transcript on stderr
 *   knsvc_test_smtp queue <port> <tls> <user> <pass> <cafile> <count> <to>
 *       -> mails through a kn_mailer while a 10 ms timer runs; checks the loop never stalls
 */
#include "kn_test.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "knsvc/buf.h"
#include "knsvc/loop.h"
#include "knsvc/smtp.h"

static const char *arg_or_null(const char *s) { return s && strcmp(s, "-") ? s : NULL; }

static void test_addresses(void) {
    char a[256];
    CHECK(kn_mail_address("Krunker <noreply@example.com>", a, sizeof a) == 0);
    CHECK_EQ_STR(a, "noreply@example.com");
    CHECK(kn_mail_address("  user.name+tag@sub.example.org ", a, sizeof a) == 0);
    CHECK_EQ_STR(a, "user.name+tag@sub.example.org");
    CHECK(kn_mail_address("\"Some, One\" <x@y.io>", a, sizeof a) == 0);
    CHECK(kn_mail_address("x@y.io>", a, sizeof a) == -1);
    CHECK(kn_mail_address("<x@y.io> trailing", a, sizeof a) == -1);
    CHECK(kn_mail_address("x@y.io\r\nBcc: z@w.io", a, sizeof a) == -1);
    CHECK(kn_mail_address_valid("a@b.co") == 1);
    CHECK(kn_mail_address_valid("a@localhost") == 0);
    CHECK(kn_mail_address_valid("a b@c.de") == 0);
    CHECK(kn_mail_address_valid("a@b@c.de") == 0);
    CHECK(kn_mail_address_valid("@c.de") == 0);
    CHECK(kn_mail_address_valid("a@c..de") == 0);
    CHECK(kn_mail_address_valid("a@-c_d.de") == 0);
    CHECK(kn_smtp_tls_parse(NULL, 465) == KN_SMTP_TLS_IMPLICIT);
    CHECK(kn_smtp_tls_parse("auto", 587) == KN_SMTP_TLS_STARTTLS);
    CHECK(kn_smtp_tls_parse("ssl", 25) == KN_SMTP_TLS_IMPLICIT);
    CHECK(kn_smtp_tls_parse("none", 25) == KN_SMTP_TLS_NONE);
    CHECK(kn_smtp_tls_parse("bogus", 25) == -1);
}

static void test_build(void) {
    kn_buf b;
    kn_buf_init(&b);
    char id[160];
    kn_mail m = {.from = "Krunker <noreply@example.com>", .to = "player@example.org",
                 .subject = "Verify your Krunker email",
                 .text = "Hello,\n.leading dot\nline with trailing space \nend",
                 .html = "<p>Hello \xC3\xA9</p>"};
    CHECK(kn_mail_build(&m, 1760000000LL, &b, id, sizeof id) == 0);
    CHECK(strstr(b.data, "Date: Thu, 09 Oct 2025 08:53:20 +0000\r\n") != NULL);
    CHECK(strstr(b.data, "From: Krunker <noreply@example.com>\r\n") != NULL);
    CHECK(strstr(b.data, "To: player@example.org\r\n") != NULL);
    CHECK(strstr(b.data, "Subject: Verify your Krunker email\r\n") != NULL);
    CHECK(strstr(b.data, "MIME-Version: 1.0\r\n") != NULL);
    CHECK(strstr(b.data, "multipart/alternative; boundary=") != NULL);
    CHECK(strncmp(id, "<", 1) == 0 && strstr(id, "@example.com>") != NULL);
    char hdr[200];
    snprintf(hdr, sizeof hdr, "Message-ID: %s\r\n", id);
    CHECK(strstr(b.data, hdr) != NULL);
    CHECK(strstr(b.data, "\r\n=2Eleading dot\r\n") != NULL);   /* QP: leading dot */
    CHECK(strstr(b.data, "trailing space=20\r\n") != NULL);    /* QP: trailing space */
    CHECK(strstr(b.data, "<p>Hello =C3=A9</p>") != NULL);      /* QP: UTF-8 */
    for (size_t i = 0; i < b.len; i++) {
        if (b.data[i] == '\n') CHECK(i > 0 && b.data[i - 1] == '\r'); /* CRLF only */
        CHECK((unsigned char) b.data[i] < 128);                       /* 7-bit clean */
    }
    /* every line <= 78 + CRLF */
    size_t col = 0, longest = 0;
    for (size_t i = 0; i < b.len; i++) {
        if (b.data[i] == '\n') col = 0;
        else if (++col > longest) longest = col;
    }
    CHECK(longest <= 79);

    /* text only, UTF-8 subject and display name -> encoded words */
    kn_buf_reset(&b);
    kn_mail u = {.from = "Kr\xC3\xBCnker <noreply@example.com>", .to = "p@example.org",
                 .subject = "Bitte best\xC3\xA4tige deine E-Mail-Adresse f\xC3\xBCr dein Konto bei Krunker, danke sch\xC3\xB6n",
                 .text = "x"};
    CHECK(kn_mail_build(&u, 0, &b, NULL, 0) == 0);
    CHECK(strstr(b.data, "From: =?UTF-8?B?") != NULL);
    CHECK(strstr(b.data, "Subject: =?UTF-8?B?") != NULL);
    CHECK(strstr(b.data, "?=\r\n =?UTF-8?B?") != NULL); /* folded */
    CHECK(strstr(b.data, "Content-Type: text/plain; charset=UTF-8") != NULL);
    CHECK(strstr(b.data, "multipart") == NULL);

    /* quoted display name with specials */
    kn_buf_reset(&b);
    kn_mail q = {.from = "Krunker, Inc. <noreply@example.com>", .to = "p@example.org", .subject = "s", .text = "x"};
    CHECK(kn_mail_build(&q, 0, &b, NULL, 0) == 0);
    CHECK(strstr(b.data, "From: \"Krunker, Inc.\" <noreply@example.com>") != NULL);

    /* long QP lines get soft breaks */
    kn_buf_reset(&b);
    char longtext[400];
    memset(longtext, 'a', sizeof longtext - 1);
    longtext[sizeof longtext - 1] = 0;
    kn_mail l = {.from = "noreply@example.com", .to = "p@example.org", .subject = "s", .text = longtext};
    CHECK(kn_mail_build(&l, 0, &b, NULL, 0) == 0);
    CHECK(strstr(b.data, "=\r\naaaa") != NULL);

    /* header injection and missing fields */
    kn_mail bad1 = {.from = "noreply@example.com", .to = "p@example.org", .subject = "hi\r\nBcc: x@y.io", .text = "x"};
    kn_mail bad2 = {.from = "noreply@example.com", .to = "p@example.org\nBcc: x@y.io", .subject = "s", .text = "x"};
    kn_mail bad3 = {.from = "noreply@example.com", .to = "p@example.org", .subject = "s"};
    kn_mail bad4 = {.from = "not an address", .to = "p@example.org", .subject = "s", .text = "x"};
    CHECK(kn_mail_build(&bad1, 0, &b, NULL, 0) == -1);
    CHECK(kn_mail_build(&bad2, 0, &b, NULL, 0) == -1);
    CHECK(kn_mail_build(&bad3, 0, &b, NULL, 0) == -1);
    CHECK(kn_mail_build(&bad4, 0, &b, NULL, 0) == -1);
    kn_buf_free(&b);
}

static void trace_stderr(void *ud, const char *line) {
    (void) ud;
    fprintf(stderr, "%s\n", line);
}

static int cmd_send(int argc, char **argv) {
    if (argc < 10) return 2;
    kn_smtp_config cfg = {.host = "127.0.0.1", .port = atoi(argv[2]),
                          .tls = (kn_smtp_tls) kn_smtp_tls_parse(argv[3], atoi(argv[2])),
                          .user = arg_or_null(argv[4]), .password = arg_or_null(argv[5]),
                          .ca_file = arg_or_null(argv[6]), .verify_peer = atoi(argv[7]),
                          .auth = argc > 10 ? argv[10] : NULL, .helo = "knsvc-test.local", .timeout_ms = 5000,
                          .allow_plain_auth = 1};
    kn_mail m = {.from = argv[8], .to = argv[9], .subject = "Verify your Krunker email \xE2\x9C\x94",
                 .text = "Hi player,\n\nOpen this link to verify your email:\nhttps://example.com/auth/verify-email?token=abc\n"
                         ".\n..double\nThanks",
                 .html = "<p>Hi player,</p><p><a href=\"https://example.com/auth/verify-email?token=abc\">Verify</a></p>"};
    char id[160], err[256];
    kn_smtp_set_trace(trace_stderr, NULL);
    const int st = kn_smtp_send(&cfg, &m, id, sizeof id, err, sizeof err);
    printf("status %d\nid %s\nerr %s\n", st, id, err);
    return 0;
}

/* ---- queue: results come back on the loop thread while a timer keeps ticking */

typedef struct QueueState {
    kn_loop *loop;
    pthread_t loop_thread;
    int expected, done, ok, wrong_thread, null_id;
    uint64_t last_tick, max_gap;
} QueueState;

static void on_tick(kn_loop *loop, void *ud) {
    (void) loop;
    QueueState *q = ud;
    const uint64_t now = kn_now_ms();
    if (q->last_tick && now - q->last_tick > q->max_gap) q->max_gap = now - q->last_tick;
    q->last_tick = now;
}

static void on_sent(void *ud, int status, const char *message_id, const char *error) {
    QueueState *q = ud;
    if (!pthread_equal(pthread_self(), q->loop_thread)) q->wrong_thread++;
    if (status == KN_SMTP_OK) q->ok++;
    else fprintf(stderr, "mail failed: %d %s\n", status, error ? error : "");
    if (status == KN_SMTP_OK && !message_id) q->null_id++;
    if (++q->done == q->expected) kn_loop_stop(q->loop);
}

static void on_timeout(kn_loop *loop, void *ud) {
    (void) ud;
    kn_loop_stop(loop);
}

static int cmd_queue(int argc, char **argv) {
    if (argc < 9) return 2;
    QueueState q = {0};
    q.loop = kn_loop_new();
    q.loop_thread = pthread_self();
    q.expected = atoi(argv[7]);
    kn_smtp_config cfg = {.host = "127.0.0.1", .port = atoi(argv[2]),
                          .tls = (kn_smtp_tls) kn_smtp_tls_parse(argv[3], atoi(argv[2])),
                          .user = argv[4], .password = argv[5], .ca_file = argv[6], .verify_peer = 1,
                          .timeout_ms = 5000};
    kn_mailer_options opts = {.queue_max = q.expected, .retries = 2, .retry_delay_ms = 100};
    kn_mailer *mm = kn_mailer_new(q.loop, &cfg, &opts);
    CHECK(mm != NULL);
    if (!mm) return 1;
    for (int i = 0; i < q.expected; i++) {
        char subject[64];
        snprintf(subject, sizeof subject, "queued %d", i);
        kn_mail m = {.from = "Krunker <noreply@example.com>", .to = argv[8], .subject = subject, .text = "queued body"};
        CHECK(kn_mailer_send(mm, &m, on_sent, &q) == 0);
    }
    kn_mail extra = {.from = "noreply@example.com", .to = argv[8], .subject = "x", .text = "x"};
    CHECK(kn_mailer_send(mm, &extra, on_sent, &q) == -1); /* queue_max reached */
    CHECK_EQ_INT(kn_mailer_pending(mm), q.expected);
    kn_loop_add_timer(q.loop, 10, 10, on_tick, &q);
    kn_loop_add_timer(q.loop, 30000, 0, on_timeout, NULL);
    kn_loop_run(q.loop);
    CHECK_EQ_INT(q.done, q.expected);
    CHECK_EQ_INT(q.ok, q.expected);
    CHECK_EQ_INT(q.wrong_thread, 0);
    CHECK_EQ_INT(q.null_id, 0);
    CHECK_EQ_INT(kn_mailer_pending(mm), 0);
    CHECK(q.max_gap < 250); /* the loop kept running while the worker talked SMTP */
    printf("queue: %d sent, timer max gap %llu ms\n", q.ok, (unsigned long long) q.max_gap);

    /* free with work still queued: no callback afterwards, no crash */
    q.expected = 1000;
    for (int i = 0; i < 3; i++) {
        kn_mail m = {.from = "noreply@example.com", .to = argv[8], .subject = "dropped", .text = "x"};
        kn_mailer_send(mm, &m, on_sent, &q);
    }
    const int done_before = q.done;
    kn_mailer_free(mm);
    kn_loop_add_timer(q.loop, 200, 0, on_timeout, NULL);
    kn_loop_run(q.loop);
    CHECK_EQ_INT(q.done, done_before);
    kn_loop_free(q.loop);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2 || !strcmp(argv[1], "unit")) {
        test_addresses();
        test_build();
        TEST_DONE("knsvc_smtp unit");
    }
    if (!strcmp(argv[1], "send")) return cmd_send(argc, argv);
    if (!strcmp(argv[1], "queue")) {
        const int rc = cmd_queue(argc, argv);
        if (rc) return rc;
        TEST_DONE("knsvc_smtp queue");
    }
    fprintf(stderr, "usage: knsvc_test_smtp unit|send|queue ...\n");
    return 2;
}
