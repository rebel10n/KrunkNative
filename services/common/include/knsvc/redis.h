/*
 * knsvc/redis.h - optional Redis / Valkey client on a kn_loop (hiredis async API).
 *
 * Part of the static library `knsvc_redis`, built only when hiredis is found
 * (CMake option KNSVC_REDIS, default ON when found). Only services link it: the
 * game server, the shared code and the client never do. The brain uses it as an
 * optional hot tier in front of its database (doc/backend.md 3.11); everything
 * that goes through it must also work when Redis is absent or down.
 *
 * Two connections per kn_redis: one for commands (pipelined, replies in order)
 * and one for SUBSCRIBE. Both connect in the background, authenticate (AUTH
 * [user] password), SELECT the database, and reconnect with backoff (250 ms ..
 * 5 s) after any failure; channels are re-subscribed after a reconnect.
 *
 *     kn_redis_config rc;
 *     kn_redis_config_from(cfg, &rc);            // section "redis"
 *     kn_redis *r = kn_redis_new(loop, &rc, err, sizeof err);
 *     kn_redis_cmd(r, on_reply, ctx, "GET", key, NULL);
 *
 * Commands are only sent while the command connection is up: kn_redis_cmd*()
 * returns -1 (and never calls cb) when it is down, so callers fall back to the
 * database synchronously. Once sent, cb fires exactly once on the loop thread:
 * with the reply, or with reply NULL and err set when the connection broke or
 * the reply took longer than command_timeout_ms (the connection is then dropped
 * and reconnected, so a hung server cannot stall callers), or "closed" from
 * kn_redis_free(). A Redis error reply arrives with the reply and err = its text.
 *
 * Hostnames are resolved synchronously when connecting (use an IP or a Unix
 * socket for a strictly non-blocking loop). TLS is not built in: across regions
 * use a private network / WireGuard or a TLS tunnel (doc/backend.md 3.11).
 */
#ifndef KNSVC_REDIS_H
#define KNSVC_REDIS_H

#include <stddef.h>
#include <stdint.h>

#include "knsvc/loop.h"

struct kn_config;

typedef struct kn_redis kn_redis;
typedef struct kn_redis_reply kn_redis_reply; /* a hiredis redisReply, read through the accessors */

typedef struct kn_redis_config {
    const char *url;             /* redis://[user[:password]@]host[:port][/db] or unix:///path/sock[?db=N] */
    const char *username;        /* ACL user; overrides the URL's (NULL/"" = URL's, else the default user) */
    const char *password;        /* overrides the URL's; "" = no AUTH. Never logged */
    int db;                      /* -1 = the URL's (default 0) */
    int connect_timeout_ms;      /* default 1000 */
    int command_timeout_ms;      /* default 500 */
    const char *client_name;     /* CLIENT SETNAME (optional) */
} kn_redis_config;

/* Fill from config section "redis": url, username, password, db, connect_timeout_ms,
 * command_timeout_ms (incl. KN_REDIS_* env / CLI overrides). url "" = Redis disabled.
 * Strings point into cfg. */
void kn_redis_config_from(const struct kn_config *cfg, kn_redis_config *out);

/* Validates the URL and starts connecting. NULL (+ err) on a bad URL. */
kn_redis *kn_redis_new(kn_loop *loop, const kn_redis_config *cfg, char *err, size_t errlen);
/* Drops both connections; pending callbacks fire with err "closed". Not from inside a kn_redis callback. */
void kn_redis_free(kn_redis *r);

/* 1 while the command connection is connected and authenticated. */
int kn_redis_up(const kn_redis *r);
/* "host:port" / "unix:/path" (no credentials), for logs. */
const char *kn_redis_endpoint(const kn_redis *r);

/* Called on the loop thread whenever the command connection goes up (1) or down (0). One callback. */
typedef void (*kn_redis_state_cb)(kn_redis *r, int up, void *ud);
void kn_redis_on_state(kn_redis *r, kn_redis_state_cb cb, void *ud);

/* ---- commands. cb may be NULL (fire and forget). 0 = sent, -1 = not up / out of memory (cb not called). */
typedef void (*kn_redis_cb)(kn_redis *r, const kn_redis_reply *reply, const char *err, void *ud);
int kn_redis_cmd_argv(kn_redis *r, kn_redis_cb cb, void *ud, int argc, const char *const *argv,
                      const size_t *argvlen /* NULL = strlen of each */);
/* NULL-terminated string arguments: kn_redis_cmd(r, cb, ud, "SET", key, value, "EX", "30", NULL) */
int kn_redis_cmd(kn_redis *r, kn_redis_cb cb, void *ud, const char *arg0, ...);

/* ---- publish / subscribe. The subscription survives reconnects (re-SUBSCRIBEd);
 * messages published while the subscriber connection was down are lost (Redis
 * pub/sub is fire and forget). Publishing goes through the command connection. */
typedef void (*kn_redis_msg_cb)(kn_redis *r, const char *channel, const char *payload, size_t len, void *ud);
int kn_redis_subscribe(kn_redis *r, const char *channel, kn_redis_msg_cb cb, void *ud);
int kn_redis_publish(kn_redis *r, const char *channel, const char *payload, size_t len);
/* 1 while the subscriber connection is up and every channel is subscribed. */
int kn_redis_sub_up(const kn_redis *r);

/* ---- replies (valid only inside the callback) */
enum {
    KN_REDIS_NIL = 0,
    KN_REDIS_INT,
    KN_REDIS_STR,     /* bulk string (binary safe: use len) */
    KN_REDIS_STATUS,  /* +OK style */
    KN_REDIS_ERROR,
    KN_REDIS_ARRAY,
    KN_REDIS_OTHER    /* RESP3 types (double, bool, map...) are not requested */
};
int kn_redis_reply_type(const kn_redis_reply *rp);
long long kn_redis_reply_int(const kn_redis_reply *rp);           /* INT, or a numeric STR; else 0 */
const char *kn_redis_reply_str(const kn_redis_reply *rp, size_t *len); /* STR/STATUS/ERROR, else NULL */
size_t kn_redis_reply_count(const kn_redis_reply *rp);             /* ARRAY elements, else 0 */
const kn_redis_reply *kn_redis_reply_at(const kn_redis_reply *rp, size_t i);

/* ---- counters (logs, /mm/status-style debug) */
typedef struct kn_redis_stats {
    uint64_t connects, disconnects, commands, failures, timeouts, messages;
    unsigned inflight;
} kn_redis_stats;
void kn_redis_get_stats(const kn_redis *r, kn_redis_stats *out);

#endif
