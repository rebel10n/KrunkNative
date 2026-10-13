/* knsvc/redis.h against a throwaway redis-server / valkey-server (KN_TEST_REDIS_SERVER, else
 * valkey-server or redis-server on PATH; skips 77 without one): handshake (AUTH, SELECT, a
 * wrong password), commands (binary-safe strings, nil, error replies, 2000 pipelined INCRs in
 * order, EVAL), pub/sub, kill -9 of the server (callbacks fail, commands refused, state
 * callback, reconnect + re-SUBSCRIBE after a restart), a stopped server (command timeout),
 * free with pending commands. */
#include "knsvc/redis.h"
#include "kn_test.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "knsvc/loop.h"

static const char *g_server;
static pid_t g_pid;
static int g_port;
static char g_dir[] = "/tmp/kn_test_redis_XXXXXX";

static int free_port(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof a;
    if (bind(fd, (struct sockaddr *)&a, sizeof a) || getsockname(fd, (struct sockaddr *)&a, &len)) return -1;
    close(fd);
    return ntohs(a.sin_port);
}

static int port_open(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int ok = connect(fd, (struct sockaddr *)&a, sizeof a) == 0;
    close(fd);
    return ok;
}

static const char *find_server(void) {
    const char *env = getenv("KN_TEST_REDIS_SERVER");
    if (env && *env) return env;
    static char path[512];
    const char *names[] = {"valkey-server", "redis-server"};
    const char *pathenv = getenv("PATH");
    for (size_t n = 0; n < 2; n++) {
        char dirs[4096];
        snprintf(dirs, sizeof dirs, "%s", pathenv ? pathenv : "/usr/bin:/usr/local/bin");
        for (char *d = strtok(dirs, ":"); d; d = strtok(NULL, ":")) {
            snprintf(path, sizeof path, "%s/%s", d, names[n]);
            if (access(path, X_OK) == 0) return path;
        }
    }
    return NULL;
}

static void server_start(void) {
    char port[16];
    snprintf(port, sizeof port, "%d", g_port);
    g_pid = fork();
    if (g_pid == 0) {
        freopen("/dev/null", "w", stdout);
        execl(g_server, g_server, "--port", port, "--bind", "127.0.0.1", "--save", "", "--appendonly", "no",
              "--requirepass", "s3cret", "--dir", g_dir, (char *)NULL);
        _exit(127);
    }
    for (int i = 0; i < 300 && !port_open(g_port); i++) usleep(10000);
}

static void server_kill(int sig) {
    if (g_pid <= 0) return;
    kill(g_pid, sig);
    if (sig == SIGKILL || sig == SIGTERM) {
        waitpid(g_pid, NULL, 0);
        g_pid = 0;
    }
}

typedef int (*cond_fn)(void *);
static int run_until(kn_loop *l, cond_fn cond, void *ud, int timeout_ms) {
    uint64_t end = kn_now_ms() + (uint64_t)timeout_ms;
    while (kn_now_ms() < end) {
        if (cond(ud)) return 1;
        kn_loop_run_once(l, 10);
    }
    return cond(ud);
}

static int is_up(void *ud) { return kn_redis_up(ud); }
static int is_down(void *ud) { return !kn_redis_up(ud); }
static int sub_up(void *ud) { return kn_redis_sub_up(ud); }

typedef struct Got {
    int n, errs, type;
    long long ival;
    char str[256];
    size_t len;
    char err[128];
} Got;
static int got_one(void *ud) { return ((Got *)ud)->n > 0; }

static void on_reply(kn_redis *r, const kn_redis_reply *rp, const char *err, void *ud) {
    (void)r;
    Got *g = ud;
    g->n++;
    if (err) {
        g->errs++;
        snprintf(g->err, sizeof g->err, "%s", err);
    }
    g->type = kn_redis_reply_type(rp);
    g->ival = kn_redis_reply_int(rp);
    size_t len = 0;
    const char *s = kn_redis_reply_str(rp, &len);
    g->len = len;
    if (s) memcpy(g->str, s, len < sizeof g->str ? len : sizeof g->str - 1);
}

typedef struct Seq {
    int n, bad;
} Seq;
static int seq_done(void *ud) { return ((Seq *)ud)->n >= 2000; }
static void on_incr(kn_redis *r, const kn_redis_reply *rp, const char *err, void *ud) {
    (void)r;
    Seq *s = ud;
    s->n++;
    if (err || kn_redis_reply_int(rp) != s->n) s->bad++;
}

typedef struct Msgs {
    int n;
    char last[64];
} Msgs;
static int msg_got(void *ud) { return ((Msgs *)ud)->n > 0; }
static void on_msg(kn_redis *r, const char *ch, const char *payload, size_t len, void *ud) {
    (void)r;
    (void)ch;
    Msgs *m = ud;
    m->n++;
    snprintf(m->last, sizeof m->last, "%.*s", (int)len, payload);
}

static int g_ups, g_downs;
static void on_state(kn_redis *r, int up, void *ud) {
    (void)r;
    (void)ud;
    if (up) g_ups++;
    else g_downs++;
}

static kn_redis *client(kn_loop *l, const char *url, const char *password, int db) {
    kn_redis_config c = {0};
    c.url = url;
    c.password = password;
    c.db = db;
    c.connect_timeout_ms = 500;
    c.command_timeout_ms = 400;
    c.client_name = "kn_test";
    char err[256] = "";
    kn_redis *r = kn_redis_new(l, &c, err, sizeof err);
    if (!r) fprintf(stderr, "kn_redis_new(%s): %s\n", url, err);
    return r;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    g_server = find_server();
    if (!g_server) {
        printf("knsvc_redis: no redis-server / valkey-server (KN_TEST_REDIS_SERVER): skipped\n");
        return 77;
    }
    if (!mkdtemp(g_dir)) return 1;
    g_port = free_port();
    server_start();
    if (!port_open(g_port)) {
        fprintf(stderr, "could not start %s\n", g_server);
        server_kill(SIGKILL);
        return 77;
    }
    kn_loop *l = kn_loop_new();
    char url[128], err[256];

    /* URLs */
    kn_redis_config bad = {.url = "http://x", .db = -1};
    CHECK(kn_redis_new(l, &bad, err, sizeof err) == NULL);
    bad.url = "rediss://127.0.0.1:6380";
    CHECK(kn_redis_new(l, &bad, err, sizeof err) == NULL && strstr(err, "TLS"));
    bad.url = "redis://127.0.0.1:99999";
    CHECK(kn_redis_new(l, &bad, err, sizeof err) == NULL);

    /* wrong password: never ready */
    snprintf(url, sizeof url, "redis://127.0.0.1:%d", g_port);
    kn_redis *wrong = client(l, url, "nope", -1);
    CHECK(wrong && !run_until(l, is_up, wrong, 800));
    CHECK(kn_redis_cmd(wrong, NULL, NULL, "PING", NULL) == -1);
    kn_redis_free(wrong);

    /* password in the URL + db 2 */
    snprintf(url, sizeof url, "redis://:s3cret@127.0.0.1:%d/2", g_port);
    kn_redis *r = client(l, url, NULL, -1);
    CHECK(r && run_until(l, is_up, r, 3000));
    CHECK_EQ_STR(kn_redis_endpoint(r), (snprintf(err, sizeof err, "127.0.0.1:%d", g_port), err));
    kn_redis_on_state(r, on_state, NULL);

    /* binary-safe SET / GET, nil, error replies */
    const char *argv[3] = {"SET", "k:bin", "a\0b"};
    size_t lens[3] = {3, 5, 3};
    Got g = {0};
    CHECK(kn_redis_cmd_argv(r, on_reply, &g, 3, argv, lens) == 0);
    CHECK(run_until(l, got_one, &g, 2000) && !g.errs && g.type == KN_REDIS_STATUS);
    memset(&g, 0, sizeof g);
    kn_redis_cmd(r, on_reply, &g, "GET", "k:bin", NULL);
    CHECK(run_until(l, got_one, &g, 2000) && g.type == KN_REDIS_STR && g.len == 3 && !memcmp(g.str, "a\0b", 3));
    memset(&g, 0, sizeof g);
    kn_redis_cmd(r, on_reply, &g, "GET", "k:none", NULL);
    CHECK(run_until(l, got_one, &g, 2000) && g.type == KN_REDIS_NIL && !g.errs);
    memset(&g, 0, sizeof g);
    kn_redis_cmd(r, on_reply, &g, "INCR", "k:bin", NULL);
    CHECK(run_until(l, got_one, &g, 2000) && g.type == KN_REDIS_ERROR && g.errs && strstr(g.err, "ERR"));
    memset(&g, 0, sizeof g);
    kn_redis_cmd(r, on_reply, &g, "EVAL", "return {redis.call('INCRBY', KEYS[1], ARGV[1]), 'x'}", "1", "k:ev", "5",
                 NULL);
    CHECK(run_until(l, got_one, &g, 2000) && g.type == KN_REDIS_ARRAY && !g.errs);

    /* the db index is applied: the key is not in db 0 */
    snprintf(url, sizeof url, "redis://127.0.0.1:%d", g_port);
    kn_redis *r0 = client(l, url, "s3cret", -1);
    CHECK(r0 && run_until(l, is_up, r0, 3000));
    memset(&g, 0, sizeof g);
    kn_redis_cmd(r0, on_reply, &g, "EXISTS", "k:bin", NULL);
    CHECK(run_until(l, got_one, &g, 2000) && g.type == KN_REDIS_INT && g.ival == 0);

    /* pipelining keeps order */
    Seq seq = {0};
    for (int i = 0; i < 2000; i++) CHECK(kn_redis_cmd(r, on_incr, &seq, "INCR", "k:seq", NULL) == 0);
    CHECK(run_until(l, seq_done, &seq, 5000));
    CHECK_EQ_INT(seq.bad, 0);

    /* pub/sub (published by another client) */
    Msgs m = {0};
    CHECK(kn_redis_subscribe(r, "kn:test:ch", on_msg, &m) == 0);
    CHECK(run_until(l, sub_up, r, 3000));
    for (int i = 0; i < 20 && !m.n; i++) {
        kn_redis_publish(r0, "kn:test:ch", "hello", 5);
        run_until(l, msg_got, &m, 100);
    }
    CHECK(m.n >= 1 && !strcmp(m.last, "hello"));

    /* kill -9: the pending command fails, the state callback says down, commands are refused */
    memset(&g, 0, sizeof g);
    kn_redis_cmd(r, on_reply, &g, "BLPOP", "k:never", "5", NULL); /* stays pending until the kill */
    server_kill(SIGKILL);
    CHECK(run_until(l, got_one, &g, 2000) && g.errs);
    CHECK(run_until(l, is_down, r, 2000));
    CHECK(g_downs >= 1);
    CHECK(kn_redis_cmd(r, NULL, NULL, "PING", NULL) == -1);
    CHECK(kn_redis_publish(r, "kn:test:ch", "x", 1) == -1);

    /* restart: reconnect (backoff <= 5 s) and re-SUBSCRIBE */
    server_start();
    CHECK(run_until(l, is_up, r, 8000));
    CHECK(g_ups >= 1);
    CHECK(run_until(l, sub_up, r, 8000));
    CHECK(run_until(l, is_up, r0, 8000));
    m.n = 0;
    for (int i = 0; i < 30 && !m.n; i++) {
        kn_redis_publish(r0, "kn:test:ch", "again", 5);
        run_until(l, msg_got, &m, 100);
    }
    CHECK(m.n >= 1 && !strcmp(m.last, "again"));
    memset(&g, 0, sizeof g);
    kn_redis_cmd(r, on_reply, &g, "EXISTS", "k:seq", NULL); /* no persistence: the restart forgot it */
    CHECK(run_until(l, got_one, &g, 2000) && g.ival == 0);

    /* a stopped (hung) server: the command times out, the connection is dropped and comes back */
    server_kill(SIGSTOP);
    memset(&g, 0, sizeof g);
    uint64_t t0 = kn_now_ms();
    CHECK(kn_redis_cmd(r, on_reply, &g, "PING", NULL) == 0);
    CHECK(run_until(l, got_one, &g, 3000) && g.errs && strstr(g.err, "timeout"));
    uint64_t took = kn_now_ms() - t0;
    CHECK(took >= 350 && took < 1500);
    CHECK(run_until(l, is_down, r, 1000));
    server_kill(SIGCONT);
    CHECK(run_until(l, is_up, r, 8000));

    /* free with a pending command: its callback reports "closed" */
    memset(&g, 0, sizeof g);
    kn_redis_cmd(r, on_reply, &g, "BLPOP", "k:never", "5", NULL);
    kn_redis_free(r);
    CHECK(g.n == 1 && g.errs && !strcmp(g.err, "closed"));
    kn_redis_stats st;
    kn_redis_get_stats(r0, &st);
    CHECK(st.connects >= 2 && st.commands >= 2);
    kn_redis_free(r0);

    server_kill(SIGTERM);
    kn_loop_free(l);
    char cmd[256];
    snprintf(cmd, sizeof cmd, "rm -rf %s", g_dir);
    if (system(cmd)) { /* best effort */
    }
    TEST_DONE("knsvc_redis");
}
