// Editor bus protocol test (shared/src/editor_bus.c): framing, scopes, base64, tokens, and a
// live host/client exchange over the unix socket and the TCP fallback in a private dir.
#include <editor_bus.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

static int g_fail;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static void test_framing(void) {
    unsigned char buf[64];
    const size_t n = edbus_frame_encode("{\"t\":\"x\"}", 9, buf);
    CHECK(n == 13);
    CHECK(buf[0] == 0 && buf[1] == 0 && buf[2] == 0 && buf[3] == 9);
    size_t payload = 0, consumed = 0;
    CHECK(edbus_frame_decode(buf, 3, &payload, &consumed) == 0);  // header incomplete
    CHECK(edbus_frame_decode(buf, 12, &payload, &consumed) == 0); // payload incomplete
    CHECK(edbus_frame_decode(buf, 13, &payload, &consumed) == 1 && payload == 9 && consumed == 13);
    const unsigned char huge[4] = {0x7f, 0xff, 0xff, 0xff};
    CHECK(edbus_frame_decode(huge, 4, &payload, &consumed) == -1); // > 64 MiB
    const unsigned char limit[4] = {0x04, 0x00, 0x00, 0x00};     // exactly 64 MiB: needs more
    CHECK(edbus_frame_decode(limit, 4, &payload, &consumed) == 0);
}

static void test_scopes(void) {
    CHECK(edbus_scope_parse("read,edit") == (EDBUS_SCOPE_READ | EDBUS_SCOPE_EDIT));
    CHECK(edbus_scope_parse("all") == EDBUS_SCOPE_ALL);
    CHECK(edbus_scope_parse("1") == EDBUS_SCOPE_ALL);
    CHECK(edbus_scope_parse("project") == EDBUS_SCOPE_PROJECT);
    CHECK(edbus_scope_parse("bogus") == 0);
    cJSON *j = edbus_scope_json(EDBUS_SCOPE_READ | EDBUS_SCOPE_PROJECT);
    CHECK(cJSON_GetArraySize(j) == 2);
    CHECK(edbus_scope_from_json(j) == (EDBUS_SCOPE_READ | EDBUS_SCOPE_PROJECT));
    cJSON_Delete(j);
    CHECK(edbus_scope_for("list", NULL) == EDBUS_SCOPE_READ);
    CHECK(edbus_scope_for("open", NULL) == EDBUS_SCOPE_PROJECT);
    CHECK(edbus_scope_for("call", "query") == EDBUS_SCOPE_READ);
    CHECK(edbus_scope_for("call", "render") == EDBUS_SCOPE_READ);
    CHECK(edbus_scope_for("call", "batch") == EDBUS_SCOPE_EDIT);
    CHECK(edbus_scope_for("call", "save") == EDBUS_SCOPE_EDIT);
    CHECK(edbus_scope_for("call", "unknown_tool") == EDBUS_SCOPE_EDIT);
    CHECK(edbus_scope_for("paste", NULL) == EDBUS_SCOPE_EDIT);
}

static void test_base64(void) {
    char *a = edbus_base64((const unsigned char *) "Man", 3), *b = edbus_base64((const unsigned char *) "Ma", 2),
         *c = edbus_base64((const unsigned char *) "M", 1), *d = edbus_base64((const unsigned char *) "", 0);
    CHECK(a && !strcmp(a, "TWFu"));
    CHECK(b && !strcmp(b, "TWE="));
    CHECK(c && !strcmp(c, "TQ=="));
    CHECK(d && !strcmp(d, ""));
    free(a), free(b), free(c), free(d);
}

static void test_messages(void) {
    cJSON *id = cJSON_CreateNumber(7);
    cJSON *ok = edbus_res_ok(id, NULL);
    CHECK(cJSON_IsTrue(cJSON_GetObjectItem(ok, "ok")) && cJSON_GetObjectItem(ok, "id")->valuedouble == 7);
    cJSON *err = edbus_res_err(id, "permission_denied", "no");
    CHECK(!strcmp(cJSON_GetObjectItem(cJSON_GetObjectItem(err, "error"), "code")->valuestring, "permission_denied"));
    cJSON_Delete(ok);
    cJSON_Delete(err);
    cJSON_Delete(id);
    char t1[EDBUS_TOKEN_LEN + 1], t2[EDBUS_TOKEN_LEN + 1];
    CHECK(edbus_token_generate(t1) && edbus_token_generate(t2));
    CHECK(strlen(t1) == EDBUS_TOKEN_LEN && !edbus_token_equal(t1, t2) && edbus_token_equal(t1, t1));
    CHECK(!edbus_token_equal(t1, "abc"));
}

#ifndef _WIN32
// host side of one exchange: accept, read hello, check the token, answer welcome, then echo
// one request back with a big payload
static void live_exchange(const char *dir, const int tcp) {
    setenv("KRUNKNATIVE_EDITOR_BUS_TCP", tcp ? "1" : "0", 1);
    EdBusPaths p;
    CHECK(edbus_paths_init(&p, dir));
    struct stat st;
    CHECK(stat(p.dir, &st) == 0 && (st.st_mode & 0777) == 0700);
    const int lock = edbus_lock_try(&p);
    CHECK(lock >= 0);
    CHECK(edbus_lock_try(&p) < 0); // a second host loses the race (flock is per open file)
    char token[EDBUS_TOKEN_LEN + 1];
    edbus_token_generate(token);
    CHECK(edbus_token_write(&p, token));
    CHECK(stat(p.token, &st) == 0 && (st.st_mode & 0777) == 0600);
    const int lfd = edbus_listen(&p);
    CHECK(lfd >= 0);
    if (!tcp) CHECK(stat(p.sock, &st) == 0 && (st.st_mode & 0777) == 0600);

    pid_t pid = fork();
    if (pid == 0) {
        // client process: handshake + one request
        EdBusConn c;
        cJSON *welcome = NULL;
        if (!edbus_client_connect(&c, &p, "mcp", "editor_bus_test", "editor_bus_test", "1", 20000, &welcome)) _exit(10);
        if (cJSON_GetObjectItem(welcome, "peer")->valueint != 3) _exit(11);
        cJSON_Delete(welcome);
        cJSON *req = edbus_req(1, "echo", NULL);
        edbus_send(&c, req);
        edbus_flush_blocking(&c, 5000);
        cJSON_Delete(req);
        cJSON *res = edbus_recv(&c, 10000);
        const cJSON *big = cJSON_GetObjectItem(cJSON_GetObjectItem(res, "result"), "big");
        const int ok = res && cJSON_IsString(big) && strlen(big->valuestring) == 3u * 1024 * 1024 && big->valuestring[12345] == 'x';
        cJSON_Delete(res);
        // a bad token is refused
        EdBusConn bad;
        const int fd = edbus_connect(&p);
        edbus_conn_init(&bad, fd);
        cJSON *hello = edbus_msg("hello");
        cJSON_AddStringToObject(hello, "token", "0000");
        edbus_send(&bad, hello);
        edbus_flush_blocking(&bad, 5000);
        cJSON_Delete(hello);
        cJSON *refused = edbus_recv(&bad, 5000);
        const int refused_ok = refused && !strcmp(cJSON_GetObjectItem(refused, "t")->valuestring, "error");
        cJSON_Delete(refused);
        edbus_conn_close(&bad);
        edbus_conn_close(&c);
        _exit(ok && refused_ok ? 0 : 12);
    }

    // host: two connections (good, bad)
    for (int served = 0; served < 2;) {
        int fd = -1;
        for (int i = 0; i < 3000 && fd < 0; i++) { // 30 s: loaded CI machines
            fd = edbus_accept(lfd);
            if (fd < 0) usleep(10000);
        }
        CHECK(fd >= 0);
        if (fd < 0) break;
        EdBusConn s;
        edbus_conn_init(&s, fd);
        cJSON *hello = edbus_recv(&s, 5000);
        const cJSON *tok = cJSON_GetObjectItem(hello, "token");
        if (!cJSON_IsString(tok) || !edbus_token_equal(tok->valuestring, token)) {
            cJSON *e = edbus_msg("error");
            cJSON_AddStringToObject(e, "code", "bad_token");
            edbus_send(&s, e);
            edbus_flush_blocking(&s, 5000);
            cJSON_Delete(e);
        } else {
            cJSON *w = edbus_msg("welcome");
            cJSON_AddNumberToObject(w, "proto", EDBUS_PROTO);
            cJSON_AddNumberToObject(w, "peer", 3);
            edbus_send(&s, w);
            cJSON_Delete(w);
            cJSON *req = edbus_recv(&s, 5000);
            CHECK(req && !strcmp(cJSON_GetObjectItem(req, "op")->valuestring, "echo"));
            char *big = malloc(3u * 1024 * 1024 + 1);
            memset(big, 'x', 3u * 1024 * 1024);
            big[3u * 1024 * 1024] = 0;
            cJSON *r = cJSON_CreateObject();
            cJSON_AddStringToObject(r, "big", big);
            free(big);
            cJSON *res = edbus_res_ok(cJSON_GetObjectItem(req, "id"), r);
            CHECK(edbus_send(&s, res));
            CHECK(edbus_flush_blocking(&s, 10000));
            cJSON_Delete(res);
            cJSON_Delete(req);
        }
        cJSON_Delete(hello);
        // let the client read before closing
        edbus_recv(&s, 2000);
        edbus_conn_close(&s);
        served++;
    }
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    if (!(WIFEXITED(status) && WEXITSTATUS(status) == 0)) fprintf(stderr, "client exit %d\n", WEXITSTATUS(status));
    edbus_close_fd(lfd);
    edbus_unlink(&p);
    edbus_lock_release(lock);
    // takeover: the lock is free again
    const int again = edbus_lock_try(&p);
    CHECK(again >= 0);
    edbus_lock_release(again);
}
#endif

int main(void) {
    test_framing();
    test_scopes();
    test_base64();
    test_messages();
#ifndef _WIN32
    char dir[] = "/tmp/kn-edbus-test-XXXXXX";
    if (mkdtemp(dir)) {
        char sub[sizeof(dir) + 8];
        snprintf(sub, sizeof(sub), "%s/bus", dir);
        live_exchange(sub, 0);
        live_exchange(sub, 1);
        rmdir(sub);
        char lockf[sizeof(sub) + 32];
        snprintf(lockf, sizeof(lockf), "%s/editor-bus.lock", sub);
        unlink(lockf);
        rmdir(sub);
        rmdir(dir);
    }
#endif
    if (g_fail) {
        fprintf(stderr, "editor_bus_test: %d failure(s)\n", g_fail);
        return 1;
    }
    printf("editor_bus_test: ok\n");
    return 0;
}
