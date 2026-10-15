/*
 * services/brain/finder.c - central server finder (finder.h): HTTP routes, the UDP
 * socket that polls KNINFO, and the timers. The table logic is finder_table.c.
 *
 * Host names in heartbeats are resolved once per entry (getaddrinfo, blocking but only
 * for non-numeric hosts; dedicated servers normally submit "" = their source IP).
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "auth_state.h"
#include "brain_internal.h"
#include "finder.h"
#include "hot.h"
#include "finder_table.h"
#include "releases.h"
#include "knsvc/config.h"
#include "knsvc/crypto.h"
#include "knsvc/http.h"
#include "knsvc/json.h"
#include "knsvc/log.h"
#include "knsvc/loop.h"

#define MOD "finder"
#define TICK_MS 500
#define POLLS_PER_TICK 256

typedef struct QueryAddr {
    struct sockaddr_storage sa;
    socklen_t len;
} QueryAddr;

static struct {
    Brain *b;
    FinderTable *t;
    AuthLimiter *submit_limit;
    int submit_per_min;
    int fd4, fd6;
    kn_timer_id timer;
} g_f = {.fd4 = -1, .fd6 = -1};

/* ---------------------------------------------------------------- UDP */

static int resolve(FinderEntry *e) {
    if (e->addr) return 0;
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_NUMERICHOST;
    char port[8];
    snprintf(port, sizeof(port), "%d", e->query_port);
    if (getaddrinfo(e->host, port, &hints, &res) != 0) {
        hints.ai_flags = 0;
        if (getaddrinfo(e->host, port, &hints, &res) != 0 || !res) return -1;
    }
    QueryAddr *q = calloc(1, sizeof(*q));
    if (!q) {
        freeaddrinfo(res);
        return -1;
    }
    memcpy(&q->sa, res->ai_addr, res->ai_addrlen);
    q->len = res->ai_addrlen;
    freeaddrinfo(res);
    e->addr = q;
    return 0;
}

static int same_addr(const struct sockaddr_storage *a, const struct sockaddr_storage *b) {
    if (a->ss_family != b->ss_family) return 0;
    if (a->ss_family == AF_INET) {
        const struct sockaddr_in *x = (const void *) a, *y = (const void *) b;
        return x->sin_port == y->sin_port && x->sin_addr.s_addr == y->sin_addr.s_addr;
    }
    if (a->ss_family == AF_INET6) {
        const struct sockaddr_in6 *x = (const void *) a, *y = (const void *) b;
        return x->sin6_port == y->sin6_port && !memcmp(&x->sin6_addr, &y->sin6_addr, sizeof(x->sin6_addr));
    }
    return 0;
}

static void on_udp(kn_loop *loop, int fd, unsigned events, void *ud) {
    (void) loop;
    (void) events;
    (void) ud;
    for (int i = 0; i < 64; i++) {
        uint8_t buf[2048];
        struct sockaddr_storage from;
        socklen_t flen = sizeof(from);
        const ssize_t n = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *) &from, &flen);
        if (n < 0) return; /* EAGAIN */
        cJSON *info = finder_parse_reply(buf, (size_t) n);
        if (!info) continue;
        const char *nonce = kn_json_str(info, "nonce", "");
        FinderEntry *match = NULL;
        for (size_t k = 0; k < finder_table_count(g_f.t); k++) {
            FinderEntry *e = finder_table_at(g_f.t, k);
            if (e->addr && e->nonce[0] && !strcmp(e->nonce, nonce) && same_addr(&((QueryAddr *) e->addr)->sa, &from)) {
                match = e;
                break;
            }
        }
        if (!match) {
            cJSON_Delete(info);
            continue;
        }
        const int was_verified = match->info != NULL;
        if (finder_poll_reply(g_f.t, match, info, kn_now_ms(), kn_wall_ms() / 1000) && !was_verified)
            KN_INFO(MOD, "verified %s:%d (\"%s\")", match->host, match->port, kn_json_str(match->info, "name", ""));
    }
}

static int open_udp(int family) {
    const int fd = socket(family, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    kn_set_nonblock(fd);
    kn_set_cloexec(fd);
    if (family == AF_INET6) {
        const int one = 1;
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof(one));
    }
    if (kn_loop_add_fd(g_f.b->loop, fd, KN_EV_READ, on_udp, NULL) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void send_poll(FinderEntry *e, uint64_t now) {
    char nonce[FINDER_NONCE_LEN + 1];
    if (kn_random_hex(nonce, sizeof(nonce), FINDER_NONCE_LEN / 2) != 0) return;
    finder_poll_stamp(g_f.t, e, nonce, now); /* a failed send times out like a lost reply */
    if (resolve(e) != 0) return;
    const QueryAddr *q = e->addr;
    const int fd = q->sa.ss_family == AF_INET6 ? g_f.fd6 : g_f.fd4;
    if (fd < 0) return;
    uint8_t pkt[64];
    const size_t len = finder_build_query(nonce, pkt, sizeof(pkt));
    if (sendto(fd, pkt, len, 0, (const struct sockaddr *) &q->sa, q->len) < 0)
        KN_DEBUG(MOD, "poll %s:%d: %s", e->host, e->query_port, strerror(errno));
}

static void tick(kn_loop *loop, void *ud) {
    (void) loop;
    (void) ud;
    const uint64_t now = kn_now_ms();
    const size_t removed = finder_sweep(g_f.t, now, free);
    if (removed) KN_INFO(MOD, "%zu server(s) expired, %zu listed", removed, finder_table_count(g_f.t));
    FinderEntry *due[POLLS_PER_TICK];
    const size_t n = finder_due_polls(g_f.t, now, due, POLLS_PER_TICK);
    for (size_t i = 0; i < n; i++) send_poll(due[i], now);
}

/* ---------------------------------------------------------------- HTTP */

static void reply(kn_http_req *req, int status, cJSON *o) {
    brain_reply_json(req, status, o);
    cJSON_Delete(o);
}

static void reply_err(kn_http_req *req, int status, const char *msg) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", 0);
    cJSON_AddStringToObject(o, "error", msg);
    reply(req, status, o);
}

static void h_heartbeat(kn_http_req *req, void *ud) {
    (void) ud;
    /* after finder_shutdown (SIGTERM) the HTTP server still answers for 400 ms: game servers
     * stopping at the same time send their final "shutdown" heartbeat into that window */
    if (!g_f.t) {
        reply_err(req, 503, "Shutting down");
        return;
    }
    const char *ip = req->remote_addr ? req->remote_addr : "";
    if (!hot_limit("finder_submit", ip, g_f.submit_per_min, 60, auth_limiter_hit(g_f.submit_limit, ip, kn_now_ms()))) {
        reply_err(req, 429, "Too many requests");
        return;
    }
    cJSON *body = kn_http_body_json(req);
    if (!cJSON_IsObject(body)) {
        cJSON_Delete(body);
        reply_err(req, 400, "JSON body required");
        return;
    }
    const char *host = kn_json_str(body, "host", "");
    if (!host || !*host || !strcmp(host, "0.0.0.0")) host = ip;
    const int port = (int) kn_json_int(body, "port", 0);
    if (kn_json_bool(body, "shutdown", 0)) {
        const int removed = finder_remove(g_f.t, host, port, ip);
        if (removed) KN_INFO(MOD, "%s:%d shut down", host, port);
        cJSON_Delete(body);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddBoolToObject(o, "ok", 1);
        cJSON_AddBoolToObject(o, "removed", removed);
        reply(req, 200, o);
        return;
    }
    int created = 0;
    const char *err = NULL;
    FinderEntry *e = finder_heartbeat(g_f.t, host, port, (int) kn_json_int(body, "query_port", port),
                                      kn_json_str(body, "name", ""), kn_json_str(body, "community", ""),
                                      kn_json_bool(body, "online_mode", 0), ip, kn_now_ms(), &created, &err);
    cJSON_Delete(body);
    if (!e) {
        reply_err(req, 400, err ? err : "refused");
        return;
    }
    if (created) {
        KN_INFO(MOD, "new server %s:%d (query %d) from %s", e->host, e->port, e->query_port, ip);
        send_poll(e, kn_now_ms());
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", 1);
    cJSON_AddNumberToObject(o, "interval", finder_table_config(g_f.t)->heartbeat_interval_s);
    cJSON_AddBoolToObject(o, "verified", e->info != NULL);
    reply(req, 200, o);
}

static void h_list(kn_http_req *req, void *ud) {
    (void) ud;
    if (!g_f.t) {
        reply_err(req, 503, "Shutting down");
        return;
    }
    char map[64] = "", mode[32] = "", tag[32] = "", community[64] = "", region[32] = "", q[64] = "", flavor[16] = "", tmp[16];
    kn_http_query_get(req, "map", map, sizeof(map));
    kn_http_query_get(req, "mode", mode, sizeof(mode));
    kn_http_query_get(req, "tag", tag, sizeof(tag));
    kn_http_query_get(req, "community", community, sizeof(community));
    kn_http_query_get(req, "region", region, sizeof(region));
    kn_http_query_get(req, "q", q, sizeof(q));
    /* the asking client's product flavor (flavor.h): base clients omit it and never see Strike
     * lobbies; a Strike client sends ?flavor=strike and sees only Strike lobbies */
    kn_http_query_get(req, "flavor", flavor, sizeof(flavor));
    FinderFilter f = {.map = map, .mode = mode, .tag = tag, .community = community, .region = region, .q = q,
                      .flavor = flavor, .online_mode = -1};
    if (kn_http_query_get(req, "space", tmp, sizeof(tmp)) > 0) f.space = strcmp(tmp, "0") && strcmp(tmp, "false");
    if (kn_http_query_get(req, "online_mode", tmp, sizeof(tmp)) > 0)
        f.online_mode = (!strcmp(tmp, "1") || !strcmp(tmp, "true")) ? 1 : 0;
    if (kn_http_query_get(req, "limit", tmp, sizeof(tmp)) > 0) f.limit = atoi(tmp);
    /* a seek's servers to leave out (the client's current and recent ones): listed only when nothing else is */
    char exclude[(FINDER_HOST_MAX + 8) * 4] = "";
    if (kn_http_query_get(req, "exclude", exclude, sizeof(exclude)) > 0) f.exclude = exclude;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", 1);
    cJSON *servers = finder_list(g_f.t, &f, kn_now_ms());
    cJSON_AddItemToObject(o, "servers", servers);
    /* outdated / incompatible per server + latest, download_url, downloads (releases.h);
     * ?proto=M.m is the asking client's protocol */
    char proto[16] = "";
    kn_http_query_get(req, "proto", proto, sizeof(proto));
    releases_annotate_finder(o, servers, releases_client_major(proto));
    reply(req, 200, o);
}

/* ---------------------------------------------------------------- module */

static int cfg_ms(const struct kn_config *cfg, const char *key, int def_s) {
    const int64_t v = kn_config_int(cfg, key, def_s);
    return (int) ((v > 0 ? v : def_s) * 1000);
}

int finder_init(Brain *b) {
    g_f.b = b;
    const struct kn_config *cfg = b->cfg;
    FinderConfig fc;
    finder_config_defaults(&fc);
    fc.poll_interval_ms = cfg_ms(cfg, "finder.poll_interval_s", 30);
    fc.poll_timeout_ms = cfg_ms(cfg, "finder.poll_timeout_s", 3);
    fc.expire_ms = cfg_ms(cfg, "finder.expire_s", 180);
    fc.max_failures = (int) kn_config_int(cfg, "finder.max_failures", 3);
    fc.max_per_ip = (int) kn_config_int(cfg, "finder.max_per_ip", 64);
    fc.heartbeat_interval_s = (int) kn_config_int(cfg, "finder.heartbeat_interval_s", 60);
    if (fc.max_failures < 1) fc.max_failures = 1;
    g_f.t = finder_table_new(&fc);
    g_f.submit_per_min = (int) kn_config_int(cfg, "finder.submit_per_min", 30);
    g_f.submit_limit = auth_limiter_new(g_f.submit_per_min, 60 * 1000);
    if (!g_f.t || !g_f.submit_limit) return -1;
    g_f.fd4 = open_udp(AF_INET);
    g_f.fd6 = open_udp(AF_INET6);
    if (g_f.fd4 < 0 && g_f.fd6 < 0) {
        KN_ERROR(MOD, "no UDP socket for KNINFO polls: %s", strerror(errno));
        return -1;
    }
    g_f.timer = kn_loop_add_timer(b->loop, TICK_MS, TICK_MS, tick, NULL);
    static const char *const prefixes[] = {"", "/finder"};
    for (size_t i = 0; i < 2; i++) {
        char path[64];
        snprintf(path, sizeof(path), "%s/servers/heartbeat", prefixes[i]);
        kn_http_route(b->http, "POST", path, h_heartbeat, b);
        snprintf(path, sizeof(path), "%s/servers/submit", prefixes[i]);
        kn_http_route(b->http, "POST", path, h_heartbeat, b);
        snprintf(path, sizeof(path), "%s/servers", prefixes[i]);
        kn_http_route(b->http, "GET", path, h_list, b);
    }
    KN_INFO(MOD, "server finder ready (poll %d s, expire %d s)", fc.poll_interval_ms / 1000, fc.expire_ms / 1000);
    return 0;
}

void finder_shutdown(Brain *b) {
    if (g_f.timer) kn_loop_cancel_timer(b->loop, g_f.timer);
    g_f.timer = 0;
    int *fds[2] = {&g_f.fd4, &g_f.fd6};
    for (int i = 0; i < 2; i++) {
        if (*fds[i] >= 0) {
            kn_loop_del_fd(b->loop, *fds[i]);
            close(*fds[i]);
            *fds[i] = -1;
        }
    }
    finder_table_free(g_f.t);
    auth_limiter_free(g_f.submit_limit);
    g_f.t = NULL;
    g_f.submit_limit = NULL;
}
