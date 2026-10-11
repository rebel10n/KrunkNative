/* HTTP/1.1 server: parser edge cases (incl. fuzz), routing, keep-alive/pipelining, chunked,
 * limits, CORS, async + streaming responses, timeouts. Client runs in a thread with raw sockets. */
#include "kn_test.h"
#include "knsvc/crypto.h"
#include "knsvc/http.h"
#include "knsvc/log.h"
#include "knsvc/loop.h"
#include "net_test_util.h"

#include <pthread.h>

static kn_loop *loop;
static int port;
static int async_dead_seen;

/* ------------------------------------------------------------------ handlers (loop thread) */

static void h_hello(kn_http_req *req, void *ud) {
    kn_http_set_header(req, "X-Test", "1");
    kn_http_respond_text(req, 200, "hello");
}

static void h_user(kn_http_req *req, void *ud) {
    char q[64], big[4];
    int n = kn_http_query_get(req, "q", q, sizeof q);
    int nb = kn_http_query_get(req, "big", big, sizeof big);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "name", kn_http_param(req, "name"));
    cJSON_AddStringToObject(o, "q", n >= 0 ? q : "(absent)");
    cJSON_AddNumberToObject(o, "big", nb);
    cJSON_AddStringToObject(o, "ip", req->remote_addr);
    kn_http_respond_json(req, 200, o);
    cJSON_Delete(o);
}

static void h_echo(kn_http_req *req, void *ud) {
    cJSON *j = kn_http_body_json(req);
    if (!j) {
        kn_http_respond_error(req, 400, "bad json");
        return;
    }
    kn_http_respond_json(req, 201, j);
    cJSON_Delete(j);
}

static void h_raw_len(kn_http_req *req, void *ud) {
    char t[64];
    snprintf(t, sizeof t, "len=%zu", req->body_len);
    kn_http_respond_text(req, 200, t);
}

static void async_fire(kn_loop *l, void *ud) {
    kn_http_req *req = ud;
    if (!kn_http_req_alive(req)) async_dead_seen++;
    kn_http_respond_json_str(req, 200, "{\"async\":true}");
}

static void h_async(kn_http_req *req, void *ud) {
    int ms = (int)(intptr_t)ud;
    kn_loop_add_timer(loop, (uint64_t)ms, 0, async_fire, req);
}

static int stream_n;
static void stream_tick(kn_loop *l, void *ud) {
    kn_http_req *req = ud;
    char part[32];
    snprintf(part, sizeof part, "part%d;", stream_n);
    kn_http_stream_write(req, part, strlen(part));
    if (++stream_n == 3) kn_http_stream_end(req);
    else kn_loop_add_timer(l, 10, 0, stream_tick, req);
}

static void h_stream(kn_http_req *req, void *ud) {
    stream_n = 0;
    kn_http_stream_begin(req, 200, "text/plain");
    kn_loop_add_timer(loop, 10, 0, stream_tick, req);
}

static void h_auth(kn_http_req *req, void *ud) {
    const char *b = kn_http_bearer(req);
    kn_http_respond_text(req, b ? 200 : 401, b ? b : "none");
}

static void h_files(kn_http_req *req, void *ud) {
    kn_http_respond_text(req, 200, kn_http_param(req, "*"));
}

/* ------------------------------------------------------------------ client (test thread) */

static char resp[1 << 20];

static size_t exchange(const char *req, size_t len, const char *until) {
    int fd = tcp_connect(port);
    if (fd < 0) return 0;
    send_all(fd, req, len);
    size_t n = recv_until(fd, resp, sizeof resp, until, 2000);
    close(fd);
    return n;
}

#define EX(s) exchange(s, strlen(s), NULL)

static int status_of(const char *r) {
    int st = 0;
    sscanf(r, "HTTP/1.%*d %d", &st);
    return st;
}

static void client_basic(void) {
    EX("GET /hello HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    CHECK_EQ_INT(status_of(resp), 200);
    CHECK(strstr(resp, "Content-Length: 5\r\n") && strstr(resp, "\r\n\r\nhello"));
    CHECK(strstr(resp, "X-Test: 1\r\n") && strstr(resp, "Connection: close"));

    /* keep-alive + pipelining: two requests in one write, answered in order on one connection */
    const char *two = "GET /hello HTTP/1.1\r\nHost: x\r\n\r\nGET /api/user/b%C3%B6b?q=a+b%26c HTTP/1.1\r\nHost: x\r\n\r\n";
    int fd = tcp_connect(port);
    send_all(fd, two, strlen(two));
    recv_until(fd, resp, sizeof resp, "\"ip\"", 2000);
    char *second = strstr(resp + 10, "HTTP/1.1 200");
    CHECK(strncmp(resp, "HTTP/1.1 200", 12) == 0 && second != NULL);
    CHECK(strstr(resp, "\"name\":\"b\xc3\xb6" "b\"") && strstr(resp, "\"q\":\"a b&c\""));
    CHECK(strstr(resp, "\"ip\":\"127.0.0.1\""));
    /* same connection still usable */
    const char *third = "GET /hello HTTP/1.1\r\nHost: x\r\n\r\n";
    send_all(fd, third, strlen(third));
    CHECK(recv_until(fd, resp, sizeof resp, "hello", 2000) > 0 && strstr(resp, "hello"));
    close(fd);

    EX("GET /api/user/x?big=123456 HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    CHECK(strstr(resp, "\"big\":-2") && strstr(resp, "\"q\":\"(absent)\""));

    EX("GET /files/a/b%20c.txt HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    CHECK(strstr(resp, "\r\n\r\na/b c.txt"));

    EX("GET /auth HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer abc.def\r\nConnection: close\r\n\r\n");
    CHECK(strstr(resp, "\r\n\r\nabc.def"));

    /* HTTP/1.0: no keep-alive by default */
    fd = tcp_connect(port);
    send_all(fd, "GET /hello HTTP/1.0\r\n\r\n", 23);
    recv_until(fd, resp, sizeof resp, "hello", 2000);
    CHECK(strncmp(resp, "HTTP/1.0 200", 12) == 0);
    CHECK(wait_eof(fd, 2000));
    close(fd);

    /* HEAD: headers only */
    EX("HEAD /hello HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    CHECK(strstr(resp, "Content-Length: 5") && !strstr(resp, "hello"));
}

static void client_bodies(void) {
    const char *post = "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Type: application/json\r\nContent-Length: 13\r\nConnection: close\r\n\r\n{\"a\":[1,2,3]}";
    EX(post);
    CHECK_EQ_INT(status_of(resp), 201);
    CHECK(strstr(resp, "{\"a\":[1,2,3]}"));

    const char *chunked = "POST /len HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
                          "5;ext=1\r\nhello\r\n6\r\n world\r\n0\r\nTrailer: x\r\n\r\n";
    EX(chunked);
    CHECK(strstr(resp, "len=11"));

    /* Expect: 100-continue: interim response, then the body */
    int fd = tcp_connect(port);
    const char *head = "POST /len HTTP/1.1\r\nHost: x\r\nContent-Length: 4\r\nExpect: 100-continue\r\nConnection: close\r\n\r\n";
    send_all(fd, head, strlen(head));
    recv_until(fd, resp, sizeof resp, "\r\n\r\n", 2000);
    CHECK(strncmp(resp, "HTTP/1.1 100 Continue", 21) == 0);
    send_all(fd, "abcd", 4);
    recv_until(fd, resp, sizeof resp, "len=4", 2000);
    CHECK(strstr(resp, "len=4"));
    close(fd);

    /* body split over many writes */
    fd = tcp_connect(port);
    const char *slow = "POST /len HTTP/1.1\r\nHost: x\r\nContent-Length: 10\r\nConnection: close\r\n\r\n0123456789";
    for (size_t i = 0; i < strlen(slow); i++) {
        send_all(fd, slow + i, 1);
        if (i % 16 == 0) usleep(1000);
    }
    recv_until(fd, resp, sizeof resp, "len=10", 2000);
    CHECK(strstr(resp, "len=10"));
    close(fd);
}

static void client_errors(void) {
    struct { const char *req; int status; } cases[] = {
        {"GET /hello HTTP/1.1\r\n\r\n", 400},                                         /* no Host */
        {"GET /hello HTTP/2.0\r\nHost: x\r\n\r\n", 505},
        {"GET /hello FOO/1.1\r\nHost: x\r\n\r\n", 400},
        {"GET  /hello HTTP/1.1\r\nHost: x\r\n\r\n", 400},
        {"G@T /hello HTTP/1.1\r\nHost: x\r\n\r\n", 400},
        {"GET /hello HTTP/1.1\r\nHost : x\r\n\r\n", 400},                              /* space before colon */
        {"GET /hello HTTP/1.1\r\nHost: x\r\n folded\r\n\r\n", 400},                    /* obs-fold */
        {"POST /len HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n", 400},
        {"POST /len HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\nContent-Length: 4\r\n\r\nabcd", 400},
        {"POST /len HTTP/1.1\r\nHost: x\r\nContent-Length: -1\r\n\r\n", 400},
        {"POST /len HTTP/1.1\r\nHost: x\r\nContent-Length: 1e3\r\n\r\n", 400},
        {"POST /len HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: gzip, chunked\r\n\r\n", 501},
        {"POST /len HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\n", 400},
        {"POST /len HTTP/1.1\r\nHost: x\r\nContent-Length: 999999999\r\n\r\n", 413},
        {"GET /%00x HTTP/1.1\r\nHost: x\r\n\r\n", 400},
        {"GET /nope HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", 404},
        {"DELETE /hello HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", 405},
        {"GET /echo HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", 405},
        {"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\nConnection: close\r\n\r\n{x}", 400},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        EX(cases[i].req);
        if (status_of(resp) != cases[i].status) fprintf(stderr, "case %zu: %.60s\n", i, resp);
        CHECK_EQ_INT(status_of(resp), cases[i].status);
        CHECK(strstr(resp, "{\"error\":") != NULL);
    }
    EX("DELETE /hello HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    CHECK(strstr(resp, "Allow: GET"));

    /* oversized header block -> 431 */
    static char big[40000];
    strcpy(big, "GET /hello HTTP/1.1\r\nHost: x\r\nX-Big: ");
    size_t l = strlen(big);
    memset(big + l, 'a', 20000);
    strcpy(big + l + 20000, "\r\n\r\n");
    exchange(big, strlen(big), NULL);
    CHECK_EQ_INT(status_of(resp), 431);
    /* too many headers -> 431 */
    strcpy(big, "GET /hello HTTP/1.1\r\nHost: x\r\n");
    for (int i = 0; i < 100; i++) sprintf(big + strlen(big), "X-H%d: v\r\n", i);
    strcat(big, "\r\n");
    exchange(big, strlen(big), NULL);
    CHECK_EQ_INT(status_of(resp), 431);
}

static void client_cors(void) {
    EX("OPTIONS /echo HTTP/1.1\r\nHost: x\r\nOrigin: https://krunker.test\r\nAccess-Control-Request-Method: POST\r\n"
       "Access-Control-Request-Headers: content-type\r\nConnection: close\r\n\r\n");
    CHECK_EQ_INT(status_of(resp), 204);
    CHECK(strstr(resp, "Access-Control-Allow-Origin: https://krunker.test"));
    CHECK(strstr(resp, "Access-Control-Allow-Methods: POST, OPTIONS"));
    CHECK(strstr(resp, "Access-Control-Allow-Headers: content-type"));
    EX("GET /hello HTTP/1.1\r\nHost: x\r\nOrigin: https://evil.test\r\nConnection: close\r\n\r\n");
    CHECK_EQ_INT(status_of(resp), 200);
    CHECK(!strstr(resp, "Access-Control-Allow-Origin"));
    EX("GET /hello HTTP/1.1\r\nHost: x\r\nOrigin: https://krunker.test\r\nConnection: close\r\n\r\n");
    CHECK(strstr(resp, "Access-Control-Allow-Origin: https://krunker.test") && strstr(resp, "Vary: Origin"));
}

static void client_async(void) {
    EX("GET /async HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    CHECK(strstr(resp, "{\"async\":true}"));
    /* client leaves before the async answer: no crash, request reported dead */
    int fd = tcp_connect(port);
    send_all(fd, "GET /slow HTTP/1.1\r\nHost: x\r\n\r\n", 32);
    usleep(20000);
    close(fd);
    usleep(300000);
    CHECK_EQ_INT(async_dead_seen, 1);

    /* chunked streaming */
    exchange("GET /stream HTTP/1.1\r\nHost: x\r\n\r\n", 34, "0\r\n\r\n");
    CHECK(strstr(resp, "Transfer-Encoding: chunked"));
    CHECK(strstr(resp, "6\r\npart0;\r\n6\r\npart1;\r\n6\r\npart2;\r\n0\r\n\r\n"));
}

static void client_timeouts(void) {
    /* partial request -> 408 after request_timeout (300 ms) */
    int fd = tcp_connect(port);
    send_all(fd, "GET /hello HTTP/1.1\r\nHo", 23);
    recv_until(fd, resp, sizeof resp, "}", 3000);
    CHECK_EQ_INT(status_of(resp), 408);
    close(fd);
    /* idle keep-alive connection closed after keepalive_timeout (500 ms) */
    fd = tcp_connect(port);
    send_all(fd, "GET /hello HTTP/1.1\r\nHost: x\r\n\r\n", 32);
    recv_until(fd, resp, sizeof resp, "hello", 2000);
    uint64_t t0 = kn_now_ms();
    CHECK(wait_eof(fd, 4000));
    CHECK(kn_now_ms() - t0 >= 400);
    close(fd);
}

static void *client_main(void *arg) {
    client_basic();
    client_bodies();
    client_errors();
    client_cors();
    client_async();
    client_timeouts();
    kn_loop_stop(loop);
    return NULL;
}

/* ------------------------------------------------------------------ parser fuzz */

static void fuzz_parser(void) {
    const char *valid = "POST /a/b?c=d HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\nX: y\r\n\r\n3\r\nabc\r\n0\r\n\r\n";
    size_t vl = strlen(valid);
    kn_http_parsed p;
    size_t used = 0;
    CHECK_EQ_INT(kn_http_parse_request(valid, vl, NULL, &p, &used), KN_HTTP_PARSE_DONE);
    CHECK_EQ_INT(used, vl);
    CHECK_EQ_STR(p.path, "/a/b");
    CHECK_EQ_STR(p.query, "c=d");
    CHECK_EQ_INT(p.body_len, 3);
    kn_http_parsed_free(&p);
    /* every prefix is incomplete, never an error */
    for (size_t i = 0; i < vl; i++) {
        int st = kn_http_parse_request(valid, i, NULL, &p, &used);
        if (st != KN_HTTP_PARSE_INCOMPLETE) fprintf(stderr, "prefix %zu -> %d\n", i, st);
        CHECK(st == KN_HTTP_PARSE_INCOMPLETE);
        kn_http_parsed_free(&p);
    }
    /* bare LF accepted */
    CHECK_EQ_INT(kn_http_parse_request("GET / HTTP/1.1\nHost: h\n\n", 25, NULL, &p, &used), KN_HTTP_PARSE_DONE);
    kn_http_parsed_free(&p);
    /* absolute-form target */
    CHECK_EQ_INT(kn_http_parse_request("GET http://h/x?y HTTP/1.1\r\nHost: h\r\n\r\n", 39, NULL, &p, &used), KN_HTTP_PARSE_DONE);
    CHECK_EQ_STR(p.path, "/x");
    kn_http_parsed_free(&p);
    /* random mutations of the valid request: never crash, always a definite status */
    char buf[256];
    for (int iter = 0; iter < 200000; iter++) {
        memcpy(buf, valid, vl);
        uint8_t r[8];
        kn_random_bytes(r, sizeof r);
        for (int k = 0; k < 1 + r[0] % 4; k++) buf[r[1 + k] % vl] = (char)r[5 + (k % 3)];
        size_t len = vl - r[4] % 8;
        int st = kn_http_parse_request(buf, len, NULL, &p, &used);
        int ok = st == KN_HTTP_PARSE_DONE || st == KN_HTTP_PARSE_INCOMPLETE || st == KN_HTTP_PARSE_ERROR;
        if (st == KN_HTTP_PARSE_DONE) ok = ok && used <= len && p.method && p.path;
        if (st == KN_HTTP_PARSE_ERROR) ok = ok && p.error_status >= 400 && p.error_status < 600;
        if (!ok) CHECK(ok);
        kn_http_parsed_free(&p);
    }
    /* pure garbage */
    for (int iter = 0; iter < 50000; iter++) {
        kn_random_bytes(buf, sizeof buf);
        kn_http_parse_request(buf, (size_t)(uint8_t)buf[0], NULL, &p, &used);
        kn_http_parsed_free(&p);
    }
    CHECK(1);
}

int main(void) {
    kn_log_init("test");
    kn_log_set_level(KN_LOG_WARN);
    fuzz_parser();

    loop = kn_loop_new();
    const char *origins[] = {"https://krunker.test"};
    kn_http_config cfg = {0};
    cfg.host = "127.0.0.1";
    cfg.port = 0;
    cfg.max_body_bytes = 64 * 1024;
    cfg.request_timeout_ms = 300;
    cfg.keepalive_timeout_ms = 500;
    cfg.cors_origins = origins;
    cfg.cors_origin_count = 1;
    kn_http_server *srv = kn_http_server_new(loop, &cfg);
    kn_http_route(srv, "GET", "/hello", h_hello, NULL);
    kn_http_route(srv, "GET", "/api/user/:name", h_user, NULL);
    kn_http_route(srv, "POST", "/echo", h_echo, NULL);
    kn_http_route(srv, "POST", "/len", h_raw_len, NULL);
    kn_http_route(srv, "GET", "/async", h_async, (void *)30);
    kn_http_route(srv, "GET", "/slow", h_async, (void *)150);
    kn_http_route(srv, "GET", "/stream", h_stream, NULL);
    kn_http_route(srv, "GET", "/auth", h_auth, NULL);
    kn_http_route(srv, "GET", "/files/*", h_files, NULL);
    CHECK(kn_http_server_listen(srv) == 0);
    port = kn_http_server_port(srv);
    CHECK(port > 0);

    pthread_t th;
    pthread_create(&th, NULL, client_main, NULL);
    kn_timer_id guard = kn_loop_add_timer(loop, 30000, 0, (kn_timer_cb)kn_loop_stop, NULL);
    kn_loop_run(loop);
    pthread_join(th, NULL);
    kn_loop_cancel_timer(loop, guard);
    kn_http_server_free(srv);
    kn_loop_free(loop);
    TEST_DONE("knsvc_http");
}
