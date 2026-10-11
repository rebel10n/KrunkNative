/* WebSocket server + client: knsvc<->knsvc echo, raw-socket protocol edge cases (Autobahn-style),
 * backpressure, keepalive. Also an interop helper for ws_interop.py:
 *   knsvc_test_ws --serve        echo server on an ephemeral port (prints "PORT <n>")
 *   knsvc_test_ws --client URL   echo round-trips against a foreign server, exit 0 on success */
#include "kn_test.h"
#include "knsvc/crypto.h"
#include "knsvc/http.h"
#include "knsvc/log.h"
#include "knsvc/loop.h"
#include "knsvc/ws.h"
#include "net_test_util.h"

#include <pthread.h>

static kn_loop *loop;
static int port;

/* ------------------------------------------------------------------ server side */

static int srv_close_code;
static char srv_close_reason[128];
static char srv_auth[128];
static int srv_drains;
static int srv_messages;

static void s_message(kn_ws *ws, const void *d, size_t n, int bin, void *ud) {
    srv_messages++;
    if (bin) kn_ws_send_binary(ws, d, n);
    else kn_ws_send_text(ws, d, n);
}

static void s_close(kn_ws *ws, int code, const char *reason, void *ud) {
    srv_close_code = code;
    snprintf(srv_close_reason, sizeof srv_close_reason, "%s", reason);
}

static void s_open(kn_ws *ws, void *ud) {
    const char *a = kn_ws_header(ws, "Authorization");
    snprintf(srv_auth, sizeof srv_auth, "%s", a ? a : "");
}

static const kn_ws_callbacks echo_cb = {s_open, s_message, s_close, NULL, NULL};

static void h_ws(kn_http_req *req, void *ud) {
    kn_ws_options o = {0};
    o.max_message_bytes = 64 * 1024;
    o.subprotocol = "kn1";
    o.ping_interval_ms = (int)(intptr_t)ud;
    o.pong_timeout_ms = (int)(intptr_t)ud;
    kn_ws_accept(req, &echo_cb, &o, NULL);
}

/* flood: on_open fills the send buffer, on_drain refills a few times */
static char chunk[256 * 1024];
static int flood_rounds;
static void f_fill(kn_ws *ws) {
    while (kn_ws_buffered(ws) < 600 * 1024) kn_ws_send_binary(ws, chunk, sizeof chunk);
}
static void f_open(kn_ws *ws, void *ud) { f_fill(ws); }
static void f_drain(kn_ws *ws, void *ud) {
    srv_drains++;
    if (++flood_rounds < 3) f_fill(ws);
    else kn_ws_close(ws, 1000, "done");
}
static const kn_ws_callbacks flood_cb = {f_open, NULL, s_close, f_drain, NULL};
static void h_flood(kn_http_req *req, void *ud) {
    kn_ws_options o = {0};
    o.max_send_buffer = 1024 * 1024;
    kn_ws_accept(req, &flood_cb, &o, NULL);
}

/* overflow: one send far beyond the limit closes 1008 */
static int overflow_rc;
static void o_open(kn_ws *ws, void *ud) {
    for (int i = 0; i < 8 && kn_ws_is_open(ws); i++) overflow_rc = kn_ws_send_binary(ws, chunk, sizeof chunk);
}
static const kn_ws_callbacks overflow_cb = {o_open, NULL, s_close, NULL, NULL};
static void h_overflow(kn_http_req *req, void *ud) {
    kn_ws_options o = {0};
    o.max_send_buffer = 300 * 1024;
    kn_ws_accept(req, &overflow_cb, &o, NULL);
}

/* ------------------------------------------------------------------ knsvc client (loop thread) */

static int c_stage, c_close_code, c_pongs, c_refused_code = -1, c_refused_inside_connect;
static int in_connect;
static char c_proto[16];
static char big[60000];

static void c_open(kn_ws *ws, void *ud) {
    const char *p = kn_ws_subprotocol(ws);
    snprintf(c_proto, sizeof c_proto, "%s", p ? p : "");
    kn_ws_send_text(ws, "h\xc3\xa9llo", (size_t)-1);
}

static void c_message(kn_ws *ws, const void *d, size_t n, int bin, void *ud) {
    if (c_stage == 0) {
        CHECK(!bin && n == 6 && memcmp(d, "h\xc3\xa9llo", 6) == 0 && ((const char *)d)[n] == 0);
        c_stage = 1;
        kn_ws_send_binary(ws, big, sizeof big);
    } else if (c_stage == 1) {
        CHECK(bin && n == sizeof big && memcmp(d, big, n) == 0);
        c_stage = 2;
        kn_ws_send_ping(ws, "p1", 2);
    }
}

static void c_pong(kn_ws *ws, const void *d, size_t n, void *ud) {
    CHECK(n == 2 && memcmp(d, "p1", 2) == 0);
    c_pongs++;
    kn_ws_close(ws, 4001, "bye");
}

static void c_close(kn_ws *ws, int code, const char *reason, void *ud) {
    c_close_code = code;
    kn_loop_stop(loop);
}

static void r_close(kn_ws *ws, int code, const char *reason, void *ud) {
    c_refused_code = code;
    if (in_connect) c_refused_inside_connect = 1;
}

/* wss:// against the plain server: the TLS handshake must fail cleanly (1006 "tls: ..."). */
static int t_code;
static char t_reason[256];
static void t_close(kn_ws *ws, int code, const char *reason, void *ud) {
    t_code = code;
    snprintf(t_reason, sizeof t_reason, "%s", reason);
}

static void *plain_http_peer(void *arg) {
    int lfd = *(int *)arg;
    int fd = accept(lfd, NULL, NULL);
    if (fd >= 0) {
        char buf[512];
        (void)!recv(fd, buf, sizeof buf, 0); /* the ClientHello */
        send_all(fd, "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n", 47);
        close(fd);
    }
    return NULL;
}

static void stop_when_tls_closed(kn_loop *l, void *ud) {
    if (t_code) kn_loop_stop(l);
}

static void test_tls_handshake_failure(void) {
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    socklen_t al = sizeof a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(bind(lfd, (struct sockaddr *)&a, sizeof a) == 0 && listen(lfd, 1) == 0);
    getsockname(lfd, (struct sockaddr *)&a, &al);
    pthread_t th;
    pthread_create(&th, NULL, plain_http_peer, &lfd);
    char wss[128];
    snprintf(wss, sizeof wss, "wss://127.0.0.1:%d/ws", ntohs(a.sin_port));
    kn_ws_callbacks tcb = {NULL, NULL, t_close, NULL, NULL};
    if (kn_ws_connect(loop, wss, &tcb, NULL, NULL)) {
        kn_timer_id poll_t = kn_loop_add_timer(loop, 10, 10, stop_when_tls_closed, NULL);
        kn_timer_id guard = kn_loop_add_timer(loop, 5000, 0, (kn_timer_cb)kn_loop_stop, NULL);
        kn_loop_run(loop);
        kn_loop_cancel_timer(loop, guard);
        kn_loop_cancel_timer(loop, poll_t);
        CHECK_EQ_INT(t_code, KN_WS_CLOSE_ABNORMAL);
        CHECK(strncmp(t_reason, "tls:", 4) == 0);
    } else {
        int fd = tcp_connect(ntohs(a.sin_port)); /* knsvc without libssl: release the peer thread */
        if (fd >= 0) close(fd);
    }
    pthread_join(th, NULL);
    close(lfd);
}

static void test_client_server(void) {
    for (size_t i = 0; i < sizeof big; i++) big[i] = (char)(i * 7);
    kn_ws_callbacks cb = {c_open, c_message, c_close, NULL, c_pong};
    char url[128];
    snprintf(url, sizeof url, "ws://127.0.0.1:%d/ws?token=abc", port);
    kn_ws_options o = {0};
    const char *hdrs[] = {"Authorization: Bearer s3cret", "Bad\r\nInjected: x"};
    o.headers = hdrs;
    o.header_count = 2;
    o.subprotocol = "kn1";
    CHECK(kn_ws_connect(loop, url, &cb, &o, NULL) != NULL);
    kn_ws_callbacks rcb = {NULL, NULL, r_close, NULL, NULL};
    in_connect = 1;
    CHECK(kn_ws_connect(loop, "ws://127.0.0.1:1/x", &rcb, NULL, NULL) != NULL); /* refused */
    in_connect = 0;
    CHECK(kn_ws_connect(loop, "http://127.0.0.1/x", &rcb, NULL, NULL) == NULL);
    CHECK(kn_ws_connect(loop, "not a url", &rcb, NULL, NULL) == NULL);
    kn_timer_id guard = kn_loop_add_timer(loop, 5000, 0, (kn_timer_cb)kn_loop_stop, NULL);
    kn_loop_run(loop);
    kn_loop_cancel_timer(loop, guard);
    for (int i = 0; i < 10; i++) kn_loop_run_once(loop, 10);
    CHECK_EQ_INT(c_stage, 2);
    CHECK_EQ_INT(c_pongs, 1);
    CHECK_EQ_INT(c_close_code, 4001);
    CHECK_EQ_INT(srv_close_code, 4001);
    CHECK_EQ_STR(srv_close_reason, "bye");
    CHECK_EQ_STR(srv_auth, "Bearer s3cret");
    CHECK_EQ_STR(c_proto, "kn1");
    CHECK_EQ_INT(c_refused_code, 1006);
    CHECK_EQ_INT(c_refused_inside_connect, 0);
}

/* ------------------------------------------------------------------ raw client (thread) */

static char rbuf[1 << 21];
static size_t rhave;

static int raw_open(const char *path, const char *extra) {
    int fd = tcp_connect(port);
    char req[512];
    snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: keep-alive, Upgrade\r\n"
                              "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n%s\r\n",
             path, extra ? extra : "");
    send_all(fd, req, strlen(req));
    size_t n = recv_until(fd, rbuf, sizeof rbuf, "\r\n\r\n", 2000);
    if (n == 0 || !strstr(rbuf, "101 Switching") || !strstr(rbuf, "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=")) {
        close(fd);
        return -1;
    }
    size_t head = (size_t)(strstr(rbuf, "\r\n\r\n") + 4 - rbuf);
    memmove(rbuf, rbuf + head, n - head);
    rhave = n - head;
    return fd;
}

static void raw_send(int fd, int opcode, int fin, const void *p, size_t n, int masked) {
    kn_buf b = {0};
    uint8_t mask[4] = {1, 2, 3, 4};
    kn_ws_frame_encode(&b, opcode, fin, p, n, masked ? mask : NULL);
    send_all(fd, b.data, b.len);
    kn_buf_free(&b);
}

/* Read one frame (server frames are unmasked), keeping extra bytes for the next call.
 * Returns opcode, -1 on EOF, -2 on timeout. */
static int raw_read_frame(int fd, uint8_t *payload, size_t cap, size_t *len) {
    for (;;) {
        kn_ws_frame f;
        int r = kn_ws_frame_parse((uint8_t *)rbuf, rhave, &f);
        if (r == 1 && rhave >= f.header_len + f.payload_len) {
            *len = (size_t)f.payload_len;
            if (*len <= cap) memcpy(payload, rbuf + f.header_len, *len);
            size_t used = f.header_len + (size_t)f.payload_len;
            memmove(rbuf, rbuf + used, rhave - used);
            rhave -= used;
            return f.opcode;
        }
        struct pollfd p = {fd, POLLIN, 0};
        if (poll(&p, 1, 3000) <= 0) return -2;
        ssize_t n = recv(fd, rbuf + rhave, sizeof rbuf - rhave, 0);
        if (n <= 0) return -1;
        rhave += (size_t)n;
    }
}

static int expect_close(int fd, int code) {
    uint8_t p[256];
    size_t n;
    int op;
    while ((op = raw_read_frame(fd, p, sizeof p, &n)) >= 0 && op != 8) {
    }
    if (op != 8 || n < 2) return 0;
    return (p[0] << 8 | p[1]) == code;
}

static void raw_cases(void) {
    uint8_t p[70000];
    size_t n;
    int fd;

    /* bad handshakes */
    fd = tcp_connect(port);
    const char *nokey = "GET /ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\n\r\n";
    send_all(fd, nokey, strlen(nokey));
    recv_until(fd, rbuf, sizeof rbuf, "}", 2000);
    CHECK(strstr(rbuf, "HTTP/1.1 400"));
    close(fd);
    fd = tcp_connect(port);
    const char *v12 = "GET /ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 12\r\n\r\n";
    send_all(fd, v12, strlen(v12));
    recv_until(fd, rbuf, sizeof rbuf, "}", 2000);
    CHECK(strstr(rbuf, "HTTP/1.1 426") && strstr(rbuf, "Sec-WebSocket-Version: 13"));
    close(fd);
    fd = tcp_connect(port);
    send_all(fd, "GET /ws HTTP/1.1\r\nHost: x\r\n\r\n", 30);
    recv_until(fd, rbuf, sizeof rbuf, "}", 2000);
    CHECK(strstr(rbuf, "HTTP/1.1 426"));
    close(fd);

    /* fragmented text with an interleaved ping: pong first, then the whole message */
    fd = raw_open("/ws", NULL);
    CHECK(fd >= 0);
    raw_send(fd, 1, 0, "frag", 4, 1);
    raw_send(fd, 9, 1, "pp", 2, 1);
    raw_send(fd, 0, 0, "men", 3, 1);
    raw_send(fd, 0, 1, "ted", 3, 1);
    CHECK_EQ_INT(raw_read_frame(fd, p, sizeof p, &n), 10);
    CHECK(n == 2 && memcmp(p, "pp", 2) == 0);
    CHECK_EQ_INT(raw_read_frame(fd, p, sizeof p, &n), 1);
    CHECK(n == 10 && memcmp(p, "fragmented", 10) == 0);
    /* empty messages and a 64 KiB-boundary message (16-bit length) */
    raw_send(fd, 2, 1, "", 0, 1);
    CHECK_EQ_INT(raw_read_frame(fd, p, sizeof p, &n), 2);
    CHECK_EQ_INT(n, 0);
    memset(p, 'x', 65535);
    raw_send(fd, 2, 1, p, 65535, 1);
    CHECK_EQ_INT(raw_read_frame(fd, p, sizeof p, &n), 2);
    CHECK_EQ_INT(n, 65535);
    /* close without status: empty close echoed */
    raw_send(fd, 8, 1, "", 0, 1);
    CHECK_EQ_INT(raw_read_frame(fd, p, sizeof p, &n), 8);
    CHECK_EQ_INT(n, 0);
    CHECK(wait_eof(fd, 2000));
    close(fd);
    CHECK_EQ_INT(srv_close_code, KN_WS_CLOSE_NO_STATUS);

    struct { const char *what; int op, fin, masked; const char *payload; size_t len; int code; } bad[] = {
        {"invalid utf-8 text", 1, 1, 1, "\xc3\x28", 2, 1007},
        {"utf-8 surrogate", 1, 1, 1, "\xed\xa0\x80", 3, 1007},
        {"unmasked client frame", 1, 1, 0, "hi", 2, 1002},
        {"reserved opcode 3", 3, 1, 1, "", 0, 1002},
        {"reserved control opcode 11", 11, 1, 1, "", 0, 1002},
        {"fragmented ping", 9, 0, 1, "", 0, 1002},
        {"continuation without start", 0, 1, 1, "x", 1, 1002},
        {"close code 1005 on the wire", 8, 1, 1, "\x03\xed", 2, 1002},
        {"close code 999", 8, 1, 1, "\x03\xe7", 2, 1002},
        {"close with 1-byte payload", 8, 1, 1, "\x03", 1, 1002},
        {"close reason invalid utf-8", 8, 1, 1, "\x03\xe8\xff", 3, 1007},
    };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        fd = raw_open("/ws", NULL);
        raw_send(fd, bad[i].op, bad[i].fin, bad[i].payload, bad[i].len, bad[i].masked);
        int ok = expect_close(fd, bad[i].code);
        if (!ok) fprintf(stderr, "case '%s' did not close with %d\n", bad[i].what, bad[i].code);
        CHECK(ok);
        close(fd);
    }
    /* RSV1 set */
    fd = raw_open("/ws", NULL);
    send_all(fd, "\xc1\x80\x00\x00\x00\x00", 6);
    CHECK(expect_close(fd, 1002));
    close(fd);
    /* text start then a new text frame (expected continuation) */
    fd = raw_open("/ws", NULL);
    raw_send(fd, 1, 0, "a", 1, 1);
    raw_send(fd, 1, 1, "b", 1, 1);
    CHECK(expect_close(fd, 1002));
    close(fd);
    /* oversized (64 KiB limit): single frame and fragmented */
    fd = raw_open("/ws", NULL);
    memset(p, 'y', sizeof p);
    raw_send(fd, 2, 1, p, 65537, 1);
    CHECK(expect_close(fd, 1009));
    close(fd);
    fd = raw_open("/ws", NULL);
    raw_send(fd, 2, 0, p, 40000, 1);
    raw_send(fd, 0, 1, p, 40000, 1);
    CHECK(expect_close(fd, 1009));
    close(fd);
    /* non-minimal length encoding */
    fd = raw_open("/ws", NULL);
    send_all(fd, "\x82\xfe\x00\x05\x01\x02\x03\x04hello", 13);
    CHECK(expect_close(fd, 1002));
    close(fd);
    /* frames split byte by byte still parse */
    fd = raw_open("/ws", NULL);
    kn_buf fb = {0};
    uint8_t mk[4] = {9, 8, 7, 6};
    kn_ws_frame_encode(&fb, 1, 1, "slowly", 6, mk);
    for (size_t i = 0; i < fb.len; i++) {
        send_all(fd, fb.data + i, 1);
        usleep(2000);
    }
    kn_buf_free(&fb);
    CHECK_EQ_INT(raw_read_frame(fd, p, sizeof p, &n), 1);
    CHECK(n == 6 && memcmp(p, "slowly", 6) == 0);
    /* frames sent together with the upgrade request are processed */
    close(fd);

    /* keepalive: a peer that never answers pings is dropped (ping 100 ms, pong timeout 100 ms) */
    srv_close_code = 0;
    fd = raw_open("/ws-ping", NULL);
    CHECK_EQ_INT(raw_read_frame(fd, p, sizeof p, &n), 9);
    usleep(500000);
    CHECK_EQ_INT(srv_close_code, 1006);
    close(fd);

    /* backpressure: slow reader -> on_drain refills, then a normal close */
    srv_drains = 0;
    fd = raw_open("/flood", NULL);
    usleep(200000);
    size_t total = 0;
    int op;
    while ((op = raw_read_frame(fd, p, 0, &n)) == 2) {
        total += n;
        /* consume exactly this frame from rbuf: re-read starts fresh, so drain socket slowly */
    }
    (void)total;
    close(fd);
    usleep(100000);
    CHECK(srv_drains >= 1);

    /* overflow: sends beyond max_send_buffer close the socket with 1008 and return -1 */
    srv_close_code = 0;
    fd = raw_open("/overflow", NULL);
    usleep(200000);
    CHECK_EQ_INT(overflow_rc, -1);
    CHECK_EQ_INT(srv_close_code, 1008);
    close(fd);
}

static void *raw_main(void *arg) {
    raw_cases();
    kn_loop_stop(loop);
    return NULL;
}

/* ------------------------------------------------------------------ interop helpers */

static void serve_forever(void) {
    kn_loop_stop_on_signals(loop);
    printf("PORT %d\n", port);
    fflush(stdout);
    kn_loop_run(loop);
}

static int interop_ok, interop_step;
static void i_open(kn_ws *ws, void *ud) { kn_ws_send_text(ws, "interop \xe2\x9c\x93", (size_t)-1); }
static void i_message(kn_ws *ws, const void *d, size_t n, int bin, void *ud) {
    if (interop_step == 0 && !bin && n == 11 && memcmp(d, "interop \xe2\x9c\x93", 11) == 0) {
        interop_step = 1;
        kn_ws_send_binary(ws, big, sizeof big);
    } else if (interop_step == 1 && bin && n == sizeof big && memcmp(d, big, n) == 0) {
        interop_step = 2;
        kn_ws_close(ws, 1000, "done");
    }
}
static void i_close(kn_ws *ws, int code, const char *reason, void *ud) {
    interop_ok = interop_step == 2 && code == 1000;
    kn_loop_stop(loop);
}

static int run_client(const char *url) {
    for (size_t i = 0; i < sizeof big; i++) big[i] = (char)(i * 13);
    kn_ws_callbacks cb = {i_open, i_message, i_close, NULL, NULL};
    if (!kn_ws_connect(loop, url, &cb, NULL, NULL)) return 1;
    kn_loop_add_timer(loop, 5000, 0, (kn_timer_cb)kn_loop_stop, NULL);
    kn_loop_run(loop);
    printf("interop client: %s\n", interop_ok ? "ok" : "FAILED");
    return interop_ok ? 0 : 1;
}

int main(int argc, char **argv) {
    kn_log_init("test");
    kn_log_set_level(KN_LOG_ERROR);
    loop = kn_loop_new();
    if (argc == 3 && !strcmp(argv[1], "--client")) return run_client(argv[2]);

    kn_http_config cfg = {0};
    cfg.host = "127.0.0.1";
    kn_http_server *srv = kn_http_server_new(loop, &cfg);
    kn_http_route(srv, "GET", "/ws", h_ws, (void *)0);
    kn_http_route(srv, "GET", "/echo", h_ws, (void *)0);
    kn_http_route(srv, "GET", "/ws-ping", h_ws, (void *)100);
    kn_http_route(srv, "GET", "/flood", h_flood, NULL);
    kn_http_route(srv, "GET", "/overflow", h_overflow, NULL);
    CHECK(kn_http_server_listen(srv) == 0);
    port = kn_http_server_port(srv);
    if (argc == 2 && !strcmp(argv[1], "--serve")) {
        serve_forever();
        return 0;
    }

    test_client_server();
    test_tls_handshake_failure();

    pthread_t th;
    pthread_create(&th, NULL, raw_main, NULL);
    kn_timer_id guard = kn_loop_add_timer(loop, 60000, 0, (kn_timer_cb)kn_loop_stop, NULL);
    kn_loop_run(loop);
    pthread_join(th, NULL);
    kn_loop_cancel_timer(loop, guard);
    for (int i = 0; i < 20; i++) kn_loop_run_once(loop, 10);
    kn_http_server_free(srv);
    kn_loop_free(loop);
    TEST_DONE("knsvc_ws");
}
