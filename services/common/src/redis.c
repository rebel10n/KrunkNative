/*
 * knsvc/redis.h: Redis / Valkey client on a kn_loop through the hiredis async API
 * (hiredis >= 0.14: Debian 11/12 ship 0.14, Fedora 1.2; only the API common to
 * both is used: redisAsyncConnect[Unix], the ac->ev hooks, redisAsyncCommandArgv).
 *
 * Each kn_redis owns two rconn: `cmd` (commands, FIFO replies, so the oldest
 * pending command is the list head and one timer enforces command_timeout_ms)
 * and `sub` (SUBSCRIBE). A broken, timed-out or never-connecting connection is
 * freed (hiredis then fails its pending callbacks with a NULL reply) and a new
 * one is started after a backoff.
 */
#include "knsvc/redis.h"

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <hiredis/async.h>
#include <hiredis/hiredis.h>

#include "knsvc/config.h"
#include "knsvc/log.h"

#define MOD "redis"
#define BACKOFF_MIN_MS 250
#define BACKOFF_MAX_MS 5000
#define WATCHDOG_MS 50
#define WARN_EVERY_MS 30000
#define MAX_ARGS 64

typedef struct pending {
    struct rconn *c;
    kn_redis_cb cb;      /* user callback, or NULL */
    void (*icb)(struct rconn *c, redisReply *rp); /* internal (AUTH / SELECT / PING) */
    void *ud;
    uint64_t sent_ms;
    struct pending *prev, *next;
} pending;

typedef struct rconn {
    kn_redis *r;
    int is_sub;
    redisAsyncContext *ac;
    int fd;
    unsigned events;
    int registered;
    int connected;       /* TCP up */
    int ready;           /* authenticated (+ SELECT / subscribed) */
    uint64_t started_ms; /* connect attempt start */
    int backoff_ms;
    kn_timer_id retry;
    pending *head, *tail;
    unsigned npending;
    const char *fail_reason; /* reported to callbacks failed by a forced free */
    uint64_t last_warn_ms;
    int warned_down;
} rconn;

typedef struct sub {
    char *channel;
    kn_redis_msg_cb cb;
    void *ud;
} sub;

struct kn_redis {
    kn_loop *loop;
    char host[256];
    int port;
    char unix_path[512];
    char username[128];
    char password[512];
    int db;
    int connect_timeout_ms, command_timeout_ms;
    char client_name[64];
    char endpoint[600];
    rconn cmd, sub;
    sub *subs;
    size_t nsubs;
    kn_timer_id watchdog;
    kn_redis_state_cb state_cb;
    void *state_ud;
    kn_redis_stats st;
    int freeing;
};

/* ------------------------------------------------------------------ config */

void kn_redis_config_from(const kn_config *cfg, kn_redis_config *o) {
    memset(o, 0, sizeof *o);
    o->url = kn_config_str(cfg, "redis.url", "");
    o->username = kn_config_str(cfg, "redis.username", "");
    o->password = kn_config_str(cfg, "redis.password", "");
    o->db = (int)kn_config_int(cfg, "redis.db", -1);
    o->connect_timeout_ms = (int)kn_config_int(cfg, "redis.connect_timeout_ms", 1000);
    o->command_timeout_ms = (int)kn_config_int(cfg, "redis.command_timeout_ms", 500);
}

static int pct_decode(const char *in, size_t len, char *out, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        int c = (unsigned char)in[i];
        if (c == '%' && i + 2 < len) {
            char hx[3] = {in[i + 1], in[i + 2], 0};
            char *end = NULL;
            long v = strtol(hx, &end, 16);
            if (end != hx + 2) return -1;
            c = (int)v;
            i += 2;
        }
        if (o + 1 >= cap) return -1;
        out[o++] = (char)c;
    }
    out[o] = 0;
    return 0;
}

/* redis://[user[:password]@]host[:port][/db] | unix:///path[?db=N] */
static int parse_url(kn_redis *r, const char *url, char *err, size_t errlen) {
    r->port = 6379;
    r->db = 0;
    if (!strncmp(url, "unix://", 7)) {
        const char *p = url + 7;
        const char *q = strchr(p, '?');
        size_t n = q ? (size_t)(q - p) : strlen(p);
        if (!n || n >= sizeof r->unix_path) goto bad;
        memcpy(r->unix_path, p, n);
        r->unix_path[n] = 0;
        if (q && !strncmp(q, "?db=", 4)) r->db = atoi(q + 4);
        snprintf(r->endpoint, sizeof r->endpoint, "unix:%s", r->unix_path);
        return 0;
    }
    if (!strncmp(url, "rediss://", 9)) {
        snprintf(err, errlen, "rediss:// (TLS) is not built in: use a private network or a TLS tunnel");
        return -1;
    }
    if (strncmp(url, "redis://", 8) != 0 && strncmp(url, "valkey://", 9) != 0) goto bad;
    const char *p = strstr(url, "://") + 3;
    const char *at = strrchr(p, '@');
    const char *slash = strchr(p, '/');
    if (at && slash && at > slash) at = NULL;
    if (at) {
        const char *colon = memchr(p, ':', (size_t)(at - p));
        if (colon) {
            if (pct_decode(p, (size_t)(colon - p), r->username, sizeof r->username) ||
                pct_decode(colon + 1, (size_t)(at - colon - 1), r->password, sizeof r->password))
                goto bad;
        } else if (pct_decode(p, (size_t)(at - p), r->password, sizeof r->password)) {
            goto bad;
        }
        p = at + 1;
    }
    const char *end = p + strcspn(p, "/?");
    const char *hs = p, *he;
    const char *portp = NULL;
    if (*p == '[') {
        hs = p + 1;
        he = memchr(hs, ']', (size_t)(end - hs));
        if (!he) goto bad;
        if (he + 1 < end && he[1] == ':') portp = he + 2;
    } else {
        he = memchr(p, ':', (size_t)(end - p));
        if (he) portp = he + 1;
        else he = end;
    }
    if (he <= hs || (size_t)(he - hs) >= sizeof r->host) goto bad;
    memcpy(r->host, hs, (size_t)(he - hs));
    r->host[he - hs] = 0;
    if (portp) {
        char *pe = NULL;
        long port = strtol(portp, &pe, 10);
        if (pe != end || port <= 0 || port > 65535) goto bad;
        r->port = (int)port;
    }
    if (*end == '/' && end[1] && end[1] != '?') r->db = atoi(end + 1);
    if (strchr(r->host, ':')) snprintf(r->endpoint, sizeof r->endpoint, "[%s]:%d", r->host, r->port);
    else snprintf(r->endpoint, sizeof r->endpoint, "%s:%d", r->host, r->port);
    return 0;
bad:
    snprintf(err, errlen, "invalid redis.url (redis://[user[:password]@]host[:port][/db] or unix:///path)");
    return -1;
}

/* ------------------------------------------------------------------ event loop adapter */

static void io_cb(kn_loop *loop, int fd, unsigned events, void *ud);

static void conn_set_events(rconn *c, unsigned ev) {
    if (!c->ac) return;
    c->events = ev;
    if (!c->registered) {
        c->fd = c->ac->c.fd;
        if (c->fd < 0) return;
        if (kn_loop_add_fd(c->r->loop, c->fd, ev, io_cb, c) == 0) c->registered = 1;
    } else {
        kn_loop_mod_fd(c->r->loop, c->fd, ev);
    }
}

static void ev_add_read(void *p) { rconn *c = p; conn_set_events(c, c->events | KN_EV_READ); }
static void ev_del_read(void *p) { rconn *c = p; if (c->registered) conn_set_events(c, c->events & ~(unsigned)KN_EV_READ); }
static void ev_add_write(void *p) { rconn *c = p; conn_set_events(c, c->events | KN_EV_WRITE); }
static void ev_del_write(void *p) { rconn *c = p; if (c->registered) conn_set_events(c, c->events & ~(unsigned)KN_EV_WRITE); }
static void ev_cleanup(void *p) {
    rconn *c = p;
    if (c->registered) kn_loop_del_fd(c->r->loop, c->fd);
    c->registered = 0;
    c->events = 0;
    c->fd = -1;
}

static void io_cb(kn_loop *loop, int fd, unsigned events, void *ud) {
    (void)loop;
    (void)fd;
    rconn *c = ud;
    redisAsyncContext *ac = c->ac;
    if (!ac) return;
    if (events & (KN_EV_READ | KN_EV_ERROR | KN_EV_HUP)) redisAsyncHandleRead(ac);
    if (c->ac == ac && (events & KN_EV_WRITE)) redisAsyncHandleWrite(ac);
}

/* ------------------------------------------------------------------ connection lifecycle */

static void conn_start(rconn *c);

static void retry_cb(kn_loop *loop, void *ud) {
    (void)loop;
    rconn *c = ud;
    c->retry = 0;
    conn_start(c);
}

static void conn_log_down(rconn *c, int was_ready, const char *why) {
    uint64_t now = kn_now_ms();
    if (!c->warned_down || now - c->last_warn_ms >= WARN_EVERY_MS) {
        KN_WARN(MOD, "%s connection to %s %s: %s (retrying every %d..%d ms)", c->is_sub ? "subscriber" : "command",
                c->r->endpoint, was_ready ? "lost" : "failed", why ? why : "?", BACKOFF_MIN_MS, BACKOFF_MAX_MS);
        c->last_warn_ms = now;
    }
    c->warned_down = 1;
}

/* The context is gone (freed by hiredis or by us): reset and schedule a reconnect.
 * The fd watcher is ev_cleanup's (hiredis calls it while freeing the context). */
static void conn_gone(rconn *c, const char *why) {
    kn_redis *r = c->r;
    int was_ready = c->ready;
    c->ac = NULL;
    c->connected = 0;
    c->ready = 0;
    if (r->freeing) return;
    r->st.disconnects++;
    conn_log_down(c, was_ready, why);
    if (!c->retry) {
        c->retry = kn_loop_add_timer(r->loop, (uint64_t)c->backoff_ms, 0, retry_cb, c);
        c->backoff_ms = c->backoff_ms * 2 > BACKOFF_MAX_MS ? BACKOFF_MAX_MS : c->backoff_ms * 2;
    }
    if (was_ready && !c->is_sub && r->state_cb) r->state_cb(r, 0, r->state_ud);
}

/* Free a live context from outside hiredis callbacks (timeouts). */
static void conn_drop(rconn *c, const char *why) {
    redisAsyncContext *ac = c->ac;
    if (!ac) return;
    c->fail_reason = why;
    redisAsyncFree(ac); /* pending callbacks: NULL reply; onDisconnect (-> conn_gone) when it was connected */
    if (c->ac == ac) conn_gone(c, why);
    c->fail_reason = NULL;
}

static void pending_unlink(rconn *c, pending *p) {
    if (p->prev) p->prev->next = p->next;
    else c->head = p->next;
    if (p->next) p->next->prev = p->prev;
    else c->tail = p->prev;
    c->npending--;
}

static const char *reply_err(const redisReply *rp) {
    return rp && rp->type == REDIS_REPLY_ERROR ? (rp->str ? rp->str : "ERR") : NULL;
}

static void on_reply(redisAsyncContext *ac, void *reply, void *privdata) {
    pending *p = privdata;
    rconn *c = p->c;
    kn_redis *r = c->r;
    redisReply *rp = reply;
    (void)ac;
    pending_unlink(c, p);
    if (p->icb) {
        p->icb(c, rp);
    } else if (p->cb) {
        const char *err = rp ? reply_err(rp) : (r->freeing ? "closed" : c->fail_reason ? c->fail_reason : "connection lost");
        if (!rp || err) r->st.failures++;
        p->cb(r, (const kn_redis_reply *)rp, err, p->ud);
    }
    free(p);
}

static int conn_send(rconn *c, kn_redis_cb cb, void (*icb)(rconn *, redisReply *), void *ud, int argc,
                     const char **argv, const size_t *argvlen) {
    pending *p = calloc(1, sizeof *p);
    if (!p) return -1;
    p->c = c;
    p->cb = cb;
    p->icb = icb;
    p->ud = ud;
    p->sent_ms = kn_now_ms();
    if (redisAsyncCommandArgv(c->ac, on_reply, p, argc, argv, argvlen) != REDIS_OK) {
        free(p);
        return -1;
    }
    p->prev = c->tail;
    if (c->tail) c->tail->next = p;
    else c->head = p;
    c->tail = p;
    c->npending++;
    return 0;
}

static void sub_message(redisAsyncContext *ac, void *reply, void *privdata) {
    (void)ac;
    kn_redis *r = privdata;
    redisReply *rp = reply;
    if (!rp || rp->type != REDIS_REPLY_ARRAY || rp->elements < 3) return;
    const redisReply *kind = rp->element[0], *ch = rp->element[1], *pl = rp->element[2];
    if (kind->type != REDIS_REPLY_STRING || ch->type != REDIS_REPLY_STRING) return;
    if (strcmp(kind->str, "message") != 0 || pl->type != REDIS_REPLY_STRING) return;
    r->st.messages++;
    for (size_t i = 0; i < r->nsubs; i++)
        if (!strcmp(r->subs[i].channel, ch->str)) r->subs[i].cb(r, ch->str, pl->str, pl->len, r->subs[i].ud);
}

static int sub_send(kn_redis *r, const char *channel) {
    const char *argv[2] = {"SUBSCRIBE", channel};
    return redisAsyncCommandArgv(r->sub.ac, sub_message, r, 2, argv, NULL) == REDIS_OK ? 0 : -1;
}

static void conn_ready(rconn *c) {
    kn_redis *r = c->r;
    c->ready = 1;
    c->backoff_ms = BACKOFF_MIN_MS;
    if (c->warned_down || r->st.connects == 0)
        KN_INFO(MOD, "%s connection to %s up%s", c->is_sub ? "subscriber" : "command", r->endpoint,
                c->is_sub ? "" : (r->db ? " (db selected)" : ""));
    c->warned_down = 0;
    r->st.connects++;
    if (c->is_sub) {
        for (size_t i = 0; i < r->nsubs; i++) {
            int dup = 0;
            for (size_t k = 0; k < i && !dup; k++) dup = !strcmp(r->subs[k].channel, r->subs[i].channel);
            if (!dup) sub_send(r, r->subs[i].channel);
        }
    } else if (r->state_cb) {
        r->state_cb(r, 1, r->state_ud);
    }
}

/* Handshake replies: AUTH / SELECT / CLIENT SETNAME ignored unless an error, the final PING makes it ready. */
static void hs_step(rconn *c, redisReply *rp) {
    if (!rp) return; /* connection gone: on_disconnect / conn_drop handle it */
    if (rp->type == REDIS_REPLY_ERROR) {
        KN_ERROR(MOD, "%s handshake with %s refused: %s", c->is_sub ? "subscriber" : "command", c->r->endpoint,
                 rp->str ? rp->str : "error");
        c->backoff_ms = BACKOFF_MAX_MS; /* wrong password / ACL: do not hammer */
        redisAsyncDisconnect(c->ac);
    }
}

static void hs_done(rconn *c, redisReply *rp) {
    if (!rp) return;
    if (rp->type == REDIS_REPLY_ERROR) {
        hs_step(c, rp);
        return;
    }
    if (!c->ready && c->ac) conn_ready(c);
}

static void on_connect(const redisAsyncContext *cac, int status) {
    rconn *c = cac->data;
    if (!c) return;
    if (status != REDIS_OK) {
        /* hiredis frees the context after this callback */
        const char *why = cac->errstr[0] ? cac->errstr : "connect failed";
        char msg[160];
        snprintf(msg, sizeof msg, "%s", why);
        conn_gone(c, msg);
        return;
    }
    c->connected = 1;
}

static void on_disconnect(const redisAsyncContext *cac, int status) {
    rconn *c = cac->data;
    if (!c || c->ac != cac) return;
    char msg[160];
    snprintf(msg, sizeof msg, "%s", status == REDIS_OK ? (c->fail_reason ? c->fail_reason : "closed")
                                                      : (cac->errstr[0] ? cac->errstr : "connection lost"));
    conn_gone(c, msg);
}

static void conn_start(rconn *c) {
    kn_redis *r = c->r;
    if (r->freeing || c->ac) return;
    redisAsyncContext *ac = r->unix_path[0] ? redisAsyncConnectUnix(r->unix_path) : redisAsyncConnect(r->host, r->port);
    if (!ac) {
        conn_gone(c, "out of memory");
        return;
    }
    if (ac->err) {
        char msg[160];
        snprintf(msg, sizeof msg, "%s", ac->errstr);
        redisAsyncFree(ac);
        conn_gone(c, msg);
        return;
    }
    c->ac = ac;
    c->connected = 0;
    c->ready = 0;
    c->started_ms = kn_now_ms();
    ac->data = c;
    ac->ev.data = c;
    ac->ev.addRead = ev_add_read;
    ac->ev.delRead = ev_del_read;
    ac->ev.addWrite = ev_add_write;
    ac->ev.delWrite = ev_del_write;
    ac->ev.cleanup = ev_cleanup;
    redisAsyncSetConnectCallback(ac, on_connect); /* also arms the first write event */
    redisAsyncSetDisconnectCallback(ac, on_disconnect);
    /* the handshake is queued now and written once the socket connects */
    if (r->password[0]) {
        const char *argv[3] = {"AUTH", r->username[0] ? r->username : r->password, r->password};
        conn_send(c, NULL, hs_step, NULL, r->username[0] ? 3 : 2, argv, NULL);
    }
    if (!c->is_sub && r->db > 0) {
        char db[16];
        snprintf(db, sizeof db, "%d", r->db);
        const char *argv[2] = {"SELECT", db};
        conn_send(c, NULL, hs_step, NULL, 2, argv, NULL);
    }
    if (r->client_name[0]) {
        char name[80];
        snprintf(name, sizeof name, "%s%s", r->client_name, c->is_sub ? "-sub" : "");
        const char *argv[3] = {"CLIENT", "SETNAME", name};
        conn_send(c, NULL, NULL, NULL, 3, argv, NULL); /* ignored: older servers / ACLs may refuse it */
    }
    const char *ping[1] = {"PING"};
    conn_send(c, NULL, hs_done, NULL, 1, ping, NULL);
}

static void watchdog_cb(kn_loop *loop, void *ud) {
    (void)loop;
    kn_redis *r = ud;
    uint64_t now = kn_now_ms();
    rconn *cs[2] = {&r->cmd, &r->sub};
    for (int i = 0; i < 2; i++) {
        rconn *c = cs[i];
        if (!c->ac) continue;
        if (!c->ready && now - c->started_ms > (uint64_t)r->connect_timeout_ms) {
            r->st.timeouts++;
            conn_drop(c, "connect timeout");
        } else if (c->ready && c->head && now - c->head->sent_ms > (uint64_t)r->command_timeout_ms) {
            r->st.timeouts++;
            conn_drop(c, "command timeout");
        }
    }
}

/* ------------------------------------------------------------------ public API */

kn_redis *kn_redis_new(kn_loop *loop, const kn_redis_config *cfg, char *err, size_t errlen) {
    char dummy[8];
    if (!err || !errlen) {
        err = dummy;
        errlen = sizeof dummy;
    }
    err[0] = 0;
    if (!loop || !cfg || !cfg->url || !*cfg->url) {
        snprintf(err, errlen, "redis.url is empty");
        return NULL;
    }
    kn_redis *r = calloc(1, sizeof *r);
    if (!r) {
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    r->loop = loop;
    if (parse_url(r, cfg->url, err, errlen)) {
        free(r);
        return NULL;
    }
    if (cfg->username && *cfg->username) snprintf(r->username, sizeof r->username, "%s", cfg->username);
    if (cfg->password && *cfg->password) snprintf(r->password, sizeof r->password, "%s", cfg->password);
    if (cfg->db >= 0) r->db = cfg->db;
    r->connect_timeout_ms = cfg->connect_timeout_ms > 0 ? cfg->connect_timeout_ms : 1000;
    r->command_timeout_ms = cfg->command_timeout_ms > 0 ? cfg->command_timeout_ms : 500;
    if (cfg->client_name) snprintf(r->client_name, sizeof r->client_name, "%s", cfg->client_name);
    rconn *cs[2] = {&r->cmd, &r->sub};
    for (int i = 0; i < 2; i++) {
        cs[i]->r = r;
        cs[i]->is_sub = i;
        cs[i]->fd = -1;
        cs[i]->backoff_ms = BACKOFF_MIN_MS;
    }
    r->watchdog = kn_loop_add_timer(loop, WATCHDOG_MS, WATCHDOG_MS, watchdog_cb, r);
    conn_start(&r->cmd);
    return r;
}

void kn_redis_free(kn_redis *r) {
    if (!r) return;
    r->freeing = 1;
    rconn *cs[2] = {&r->cmd, &r->sub};
    for (int i = 0; i < 2; i++) {
        if (cs[i]->retry) kn_loop_cancel_timer(r->loop, cs[i]->retry);
        if (cs[i]->ac) {
            redisAsyncContext *ac = cs[i]->ac;
            cs[i]->fail_reason = "closed";
            redisAsyncFree(ac);
            cs[i]->ac = NULL;
        }
    }
    if (r->watchdog) kn_loop_cancel_timer(r->loop, r->watchdog);
    for (size_t i = 0; i < r->nsubs; i++) free(r->subs[i].channel);
    free(r->subs);
    memset(r->password, 0, sizeof r->password);
    free(r);
}

int kn_redis_up(const kn_redis *r) { return r && !r->freeing && r->cmd.ready && r->cmd.ac; }
int kn_redis_sub_up(const kn_redis *r) { return r && !r->freeing && r->sub.ready && r->sub.ac; }
const char *kn_redis_endpoint(const kn_redis *r) { return r ? r->endpoint : ""; }

void kn_redis_on_state(kn_redis *r, kn_redis_state_cb cb, void *ud) {
    if (!r) return;
    r->state_cb = cb;
    r->state_ud = ud;
}

int kn_redis_cmd_argv(kn_redis *r, kn_redis_cb cb, void *ud, int argc, const char *const *argv, const size_t *argvlen) {
    if (!kn_redis_up(r) || argc <= 0 || !argv) return -1;
    if (conn_send(&r->cmd, cb, NULL, ud, argc, (const char **)argv, argvlen) != 0) return -1;
    r->st.commands++;
    return 0;
}

int kn_redis_cmd(kn_redis *r, kn_redis_cb cb, void *ud, const char *arg0, ...) {
    const char *argv[MAX_ARGS];
    int argc = 0;
    va_list ap;
    va_start(ap, arg0);
    for (const char *a = arg0; a && argc < MAX_ARGS; a = va_arg(ap, const char *)) argv[argc++] = a;
    va_end(ap);
    return kn_redis_cmd_argv(r, cb, ud, argc, argv, NULL);
}

int kn_redis_subscribe(kn_redis *r, const char *channel, kn_redis_msg_cb cb, void *ud) {
    if (!r || !channel || !*channel || !cb) return -1;
    sub *n = realloc(r->subs, (r->nsubs + 1) * sizeof *n);
    if (!n) return -1;
    r->subs = n;
    n[r->nsubs].channel = strdup(channel);
    if (!n[r->nsubs].channel) return -1;
    n[r->nsubs].cb = cb;
    n[r->nsubs].ud = ud;
    r->nsubs++;
    /* the subscriber connection only exists once something is subscribed */
    if (!r->sub.ac && !r->sub.retry) conn_start(&r->sub);
    else if (r->sub.ready) {
        int already = 0;
        for (size_t i = 0; i + 1 < r->nsubs; i++) already |= !strcmp(r->subs[i].channel, channel);
        if (!already) sub_send(r, channel);
    }
    return 0;
}

int kn_redis_publish(kn_redis *r, const char *channel, const char *payload, size_t len) {
    const char *argv[3] = {"PUBLISH", channel, payload ? payload : ""};
    size_t lens[3] = {7, channel ? strlen(channel) : 0, payload ? len : 0};
    if (!channel) return -1;
    return kn_redis_cmd_argv(r, NULL, NULL, 3, argv, lens);
}

void kn_redis_get_stats(const kn_redis *r, kn_redis_stats *out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    if (!r) return;
    *out = r->st;
    out->inflight = r->cmd.npending;
}

/* ------------------------------------------------------------------ replies */

int kn_redis_reply_type(const kn_redis_reply *rp) {
    const redisReply *x = (const redisReply *)rp;
    if (!x) return KN_REDIS_NIL;
    switch (x->type) {
    case REDIS_REPLY_NIL: return KN_REDIS_NIL;
    case REDIS_REPLY_INTEGER: return KN_REDIS_INT;
    case REDIS_REPLY_STRING: return KN_REDIS_STR;
    case REDIS_REPLY_STATUS: return KN_REDIS_STATUS;
    case REDIS_REPLY_ERROR: return KN_REDIS_ERROR;
    case REDIS_REPLY_ARRAY: return KN_REDIS_ARRAY;
    default: return KN_REDIS_OTHER;
    }
}

long long kn_redis_reply_int(const kn_redis_reply *rp) {
    const redisReply *x = (const redisReply *)rp;
    if (!x) return 0;
    if (x->type == REDIS_REPLY_INTEGER) return x->integer;
    if (x->type == REDIS_REPLY_STRING && x->str) return strtoll(x->str, NULL, 10);
    return 0;
}

const char *kn_redis_reply_str(const kn_redis_reply *rp, size_t *len) {
    const redisReply *x = (const redisReply *)rp;
    if (x && (x->type == REDIS_REPLY_STRING || x->type == REDIS_REPLY_STATUS || x->type == REDIS_REPLY_ERROR)) {
        if (len) *len = x->len;
        return x->str;
    }
    if (len) *len = 0;
    return NULL;
}

size_t kn_redis_reply_count(const kn_redis_reply *rp) {
    const redisReply *x = (const redisReply *)rp;
    return x && x->type == REDIS_REPLY_ARRAY ? x->elements : 0;
}

const kn_redis_reply *kn_redis_reply_at(const kn_redis_reply *rp, size_t i) {
    const redisReply *x = (const redisReply *)rp;
    if (!x || x->type != REDIS_REPLY_ARRAY || i >= x->elements) return NULL;
    return (const kn_redis_reply *)x->element[i];
}
