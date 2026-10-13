/*
 * kn_loadgen - closed-loop load generator for the brain's hot endpoints
 * (services/brain/tests/load_test.py drives it; doc/backend.md 3.11 has the numbers).
 *
 *   kn_loadgen --port P [--host H] [--threads T] [--conns C] [--seconds S] [--warmup S]
 *              --paths FILE [--method GET|POST] [--body JSON | --bodies FILE]
 *   kn_loadgen --port P ... --ws PATH --frame JSON --reply LABEL [--login JSON] [--interval-ms N]
 *
 * HTTP: C keep-alive connections per thread, each sends the next request as soon as the
 * previous answer is complete, cycling through the request paths of FILE (one per line).
 * WS: C sockets per thread (JSON social socket), each sends FRAME and waits for a reply
 * whose label is LABEL, at most one frame per --interval-ms (the social socket's own rate
 * limit is 20 frames/s). Prints one JSON line: requests, errors, rps, p50/p90/p99/max ms.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "knsvc/loop.h"
#include "knsvc/ws.h"

typedef struct Opts {
    const char *host;
    int port, threads, conns, seconds, warmup, interval_ms;
    const char *method, *body, *ws_path, *frame, *reply, *login;
    char **paths, **bodies;
    int npaths, nbodies;
} Opts;

static Opts g_o;

typedef struct Stats {
    uint32_t *lat;
    size_t n, cap;
    uint64_t errors;
    int measuring;
} Stats;

static void stat_add(Stats *s, uint64_t us) {
    if (!s->measuring) return;
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 65536;
        s->lat = realloc(s->lat, s->cap * sizeof *s->lat);
    }
    s->lat[s->n++] = (uint32_t)(us > UINT32_MAX ? UINT32_MAX : us);
}

static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

typedef struct Worker Worker;

/* ------------------------------------------------------------------ HTTP connection */

typedef struct Conn {
    Worker *w;
    int fd, idx, path_i;
    char out[8192];
    size_t out_len, out_off;
    char in[262144];
    size_t in_len;
    uint64_t t0;
} Conn;

struct Worker {
    kn_loop *loop;
    Stats st;
    Conn *conns;
    kn_ws **ws;
    uint64_t *ws_t0;
    int *ws_ready;
    int id;
};

static void conn_start(Conn *c);

static void conn_close(Conn *c) {
    if (c->fd >= 0) {
        kn_loop_del_fd(c->w->loop, c->fd);
        close(c->fd);
    }
    c->fd = -1;
}

static void conn_request(Conn *c) {
    const char *body = g_o.nbodies ? g_o.bodies[c->path_i % g_o.nbodies] : g_o.body;
    const char *path = g_o.paths[(c->path_i++) % g_o.npaths];
    if (body)
        c->out_len = (size_t)snprintf(c->out, sizeof c->out,
                                      "%s %s HTTP/1.1\r\nHost: %s\r\nContent-Type: application/json\r\n"
                                      "Content-Length: %zu\r\n\r\n%s",
                                      g_o.method, path, g_o.host, strlen(body), body);
    else
        c->out_len = (size_t)snprintf(c->out, sizeof c->out, "%s %s HTTP/1.1\r\nHost: %s\r\n\r\n", g_o.method, path,
                                      g_o.host);
    c->out_off = 0;
    c->in_len = 0;
    c->t0 = now_us();
    kn_loop_mod_fd(c->w->loop, c->fd, KN_EV_READ | KN_EV_WRITE);
}

/* 1 = a complete response is in c->in (status in *status), 0 = need more, -1 = bad */
static int parse(Conn *c, int *status) {
    c->in[c->in_len] = 0;
    char *hend = strstr(c->in, "\r\n\r\n");
    if (!hend) return 0;
    if (sscanf(c->in, "HTTP/1.%*d %d", status) != 1) return -1;
    size_t clen = 0;
    for (char *p = c->in; p < hend; p++) {
        if ((*p == '\n') && !strncasecmp(p + 1, "content-length:", 15)) clen = (size_t)strtoul(p + 16, NULL, 10);
    }
    return (size_t)(hend + 4 - c->in) + clen <= c->in_len ? 1 : 0;
}

static void conn_io(kn_loop *loop, int fd, unsigned ev, void *ud) {
    (void)loop;
    (void)fd;
    Conn *c = ud;
    if (ev & KN_EV_WRITE) {
        while (c->out_off < c->out_len) {
            ssize_t n = send(c->fd, c->out + c->out_off, c->out_len - c->out_off, MSG_NOSIGNAL);
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            if (n <= 0) {
                c->w->st.errors++;
                conn_close(c);
                conn_start(c);
                return;
            }
            c->out_off += (size_t)n;
        }
        if (c->out_off >= c->out_len) kn_loop_mod_fd(c->w->loop, c->fd, KN_EV_READ);
    }
    if (ev & (KN_EV_READ | KN_EV_HUP | KN_EV_ERROR)) {
        for (;;) {
            ssize_t n = recv(c->fd, c->in + c->in_len, sizeof c->in - 1 - c->in_len, 0);
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            if (n <= 0) {
                c->w->st.errors += c->out_len ? 1 : 0;
                conn_close(c);
                conn_start(c);
                return;
            }
            c->in_len += (size_t)n;
            int status = 0, r = parse(c, &status);
            if (r < 0) {
                c->w->st.errors++;
                conn_close(c);
                conn_start(c);
                return;
            }
            if (r == 1) {
                if (status >= 200 && status < 300) stat_add(&c->w->st, now_us() - c->t0);
                else if (c->w->st.measuring) c->w->st.errors++;
                conn_request(c);
                return;
            }
        }
    }
}

static void conn_start(Conn *c) {
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)g_o.port);
    inet_pton(AF_INET, g_o.host, &a.sin_addr);
    c->fd = kn_socket_nb(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(c->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    if (connect(c->fd, (struct sockaddr *)&a, sizeof a) != 0 && errno != EINPROGRESS) {
        close(c->fd);
        c->fd = -1;
        return;
    }
    kn_loop_add_fd(c->w->loop, c->fd, KN_EV_WRITE, conn_io, c);
    conn_request(c);
}

/* ------------------------------------------------------------------ WS sockets */

typedef struct WsCtx {
    Worker *w;
    int i;
    int logged_in;
    kn_timer_id timer;
} WsCtx;

static void ws_send(kn_loop *loop, void *ud) {
    (void)loop;
    WsCtx *x = ud;
    x->timer = 0;
    kn_ws *ws = x->w->ws[x->i];
    if (!ws || !kn_ws_is_open(ws)) return;
    x->w->ws_t0[x->i] = now_us();
    kn_ws_send_text(ws, g_o.frame, (size_t)-1);
}

static int has_label(const char *text, const char *label) {
    char want[64];
    snprintf(want, sizeof want, "[\"%s\"", label);
    return !strncmp(text, want, strlen(want));
}

static void ws_open(kn_ws *ws, void *ud) {
    WsCtx *x = ud;
    if (g_o.login) kn_ws_send_text(ws, g_o.login, (size_t)-1);
    else {
        x->logged_in = 1;
        ws_send(NULL, x);
    }
}

static void ws_msg(kn_ws *ws, const void *data, size_t len, int bin, void *ud) {
    (void)ws;
    (void)len;
    WsCtx *x = ud;
    if (bin) return;
    if (!x->logged_in) {
        if (has_label(data, "a")) {
            x->logged_in = 1;
            ws_send(NULL, x);
        }
        return;
    }
    if (!has_label(data, g_o.reply)) return;
    uint64_t took = now_us() - x->w->ws_t0[x->i];
    stat_add(&x->w->st, took);
    uint64_t wait_ms = took / 1000 >= (uint64_t)g_o.interval_ms ? 0 : (uint64_t)g_o.interval_ms - took / 1000;
    x->timer = kn_loop_add_timer(x->w->loop, wait_ms, 0, ws_send, x);
}

static void ws_close(kn_ws *ws, int code, const char *reason, void *ud) {
    (void)ws;
    (void)code;
    (void)reason;
    WsCtx *x = ud;
    x->w->ws[x->i] = NULL;
    if (x->w->st.measuring) x->w->st.errors++;
}

/* ------------------------------------------------------------------ threads */

static void start_measuring(kn_loop *loop, void *ud) {
    (void)loop;
    Worker *w = ud;
    w->st.measuring = 1;
}

static void stop_loop(kn_loop *loop, void *ud) {
    (void)ud;
    kn_loop_stop(loop);
}

static void *worker_run(void *arg) {
    Worker *w = arg;
    w->loop = kn_loop_new();
    WsCtx *ctx = NULL;
    if (g_o.ws_path) {
        w->ws = calloc((size_t)g_o.conns, sizeof *w->ws);
        w->ws_t0 = calloc((size_t)g_o.conns, sizeof *w->ws_t0);
        ctx = calloc((size_t)g_o.conns, sizeof *ctx);
        char url[512];
        snprintf(url, sizeof url, "ws://%s:%d%s", g_o.host, g_o.port, g_o.ws_path);
        static const kn_ws_callbacks cbs = {.on_open = ws_open, .on_message = ws_msg, .on_close = ws_close};
        kn_ws_options wo = {0};
        wo.max_message_bytes = 8u << 20;
        for (int i = 0; i < g_o.conns; i++) {
            ctx[i].w = w;
            ctx[i].i = i;
            w->ws[i] = kn_ws_connect(w->loop, url, &cbs, &wo, &ctx[i]);
        }
    } else {
        w->conns = calloc((size_t)g_o.conns, sizeof *w->conns);
        for (int i = 0; i < g_o.conns; i++) {
            w->conns[i].w = w;
            w->conns[i].fd = -1;
            w->conns[i].idx = i;
            w->conns[i].path_i = i * 7 + w->id * 131;
            conn_start(&w->conns[i]);
        }
    }
    kn_loop_add_timer(w->loop, (uint64_t)g_o.warmup * 1000, 0, start_measuring, w);
    kn_loop_add_timer(w->loop, (uint64_t)(g_o.warmup + g_o.seconds) * 1000, 0, stop_loop, w);
    kn_loop_run(w->loop);
    w->st.measuring = 0;
    if (w->conns)
        for (int i = 0; i < g_o.conns; i++) conn_close(&w->conns[i]);
    if (w->ws)
        for (int i = 0; i < g_o.conns; i++)
            if (w->ws[i]) kn_ws_abort(w->ws[i]);
    return NULL;
}

static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static int load_lines(const char *file, char ***out, int *n) {
    FILE *f = fopen(file, "r");
    if (!f) return -1;
    char line[4096];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!*line) continue;
        *out = realloc(*out, (size_t)(*n + 1) * sizeof **out);
        (*out)[(*n)++] = strdup(line);
    }
    fclose(f);
    return *n ? 0 : -1;
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    g_o.host = "127.0.0.1";
    g_o.threads = 2;
    g_o.conns = 16;
    g_o.seconds = 10;
    g_o.warmup = 2;
    g_o.method = "GET";
    g_o.interval_ms = 55;
    const char *paths = NULL, *bodies = NULL;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
#define OPT(name) (!strcmp(a, name) && v && ++i)
        if (OPT("--host")) g_o.host = v;
        else if (OPT("--port")) g_o.port = atoi(v);
        else if (OPT("--threads")) g_o.threads = atoi(v);
        else if (OPT("--conns")) g_o.conns = atoi(v);
        else if (OPT("--seconds")) g_o.seconds = atoi(v);
        else if (OPT("--warmup")) g_o.warmup = atoi(v);
        else if (OPT("--paths")) paths = v;
        else if (OPT("--method")) g_o.method = v;
        else if (OPT("--body")) g_o.body = v;
        else if (OPT("--bodies")) bodies = v;
        else if (OPT("--ws")) g_o.ws_path = v;
        else if (OPT("--frame")) g_o.frame = v;
        else if (OPT("--reply")) g_o.reply = v;
        else if (OPT("--login")) g_o.login = v;
        else if (OPT("--interval-ms")) g_o.interval_ms = atoi(v);
        else {
            fprintf(stderr, "usage: kn_loadgen --port P [--threads T --conns C --seconds S --warmup S] "
                            "(--paths FILE [--method M --body JSON | --bodies FILE] | --ws PATH --frame JSON --reply LABEL "
                            "[--login JSON --interval-ms N])\n");
            return 2;
        }
#undef OPT
    }
    if (bodies && load_lines(bodies, &g_o.bodies, &g_o.nbodies)) {
        fprintf(stderr, "kn_loadgen: cannot read --bodies %s\n", bodies);
        return 2;
    }
    if (!g_o.port || (!g_o.ws_path && (!paths || load_lines(paths, &g_o.paths, &g_o.npaths))) || (g_o.ws_path && (!g_o.frame || !g_o.reply))) {
        fprintf(stderr, "kn_loadgen: --port and --paths FILE (or --ws/--frame/--reply) are required\n");
        return 2;
    }
    Worker *w = calloc((size_t)g_o.threads, sizeof *w);
    pthread_t *t = calloc((size_t)g_o.threads, sizeof *t);
    for (int i = 0; i < g_o.threads; i++) {
        w[i].id = i;
        pthread_create(&t[i], NULL, worker_run, &w[i]);
    }
    Stats all = {0};
    for (int i = 0; i < g_o.threads; i++) {
        pthread_join(t[i], NULL);
        all.errors += w[i].st.errors;
        for (size_t k = 0; k < w[i].st.n; k++) {
            all.measuring = 1;
            stat_add(&all, w[i].st.lat[k]);
        }
    }
    qsort(all.lat, all.n, sizeof *all.lat, cmp_u32);
#define PCT(p) (all.n ? all.lat[(size_t)((double)(all.n - 1) * (p))] / 1000.0 : 0.0)
    printf("{\"requests\":%zu,\"errors\":%llu,\"seconds\":%d,\"rps\":%.1f,\"p50_ms\":%.3f,\"p90_ms\":%.3f,"
           "\"p99_ms\":%.3f,\"max_ms\":%.3f,\"threads\":%d,\"conns\":%d}\n",
           all.n, (unsigned long long)all.errors, g_o.seconds, (double)all.n / g_o.seconds, PCT(0.50), PCT(0.90),
           PCT(0.99), all.n ? all.lat[all.n - 1] / 1000.0 : 0.0, g_o.threads, g_o.conns);
    return 0;
}
