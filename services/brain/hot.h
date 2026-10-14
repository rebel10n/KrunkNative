/*
 * services/brain/hot.h - the brain's optional Redis / Valkey hot tier
 * (doc/backend.md 3.11). The database stays the source of truth: everything here
 * is either a cache that can be rebuilt from it, a short-lived record (join
 * handshakes, presence, rate-limit windows), or a transport (pub/sub). Every call
 * is safe when Redis is not configured, not built in (no hiredis) or down: it
 * reports a miss / "not available" and the caller keeps its database path.
 *
 * Key layout (prefix = redis.prefix, default "kn:"):
 *   <p>sess:<sha256(token)>     -> account id          EX redis.session_cache_s
 *   <p>sessidx:<account>        SET of cached hashes   (revoke-all)
 *   <p>sessrev:<account>        revocation time (us)   tombstone, EX 2 x session_cache_s
 *   <p>sessdead:<sha256(token)> revocation time (us)   tombstone, EX 2 x session_cache_s
 *   <p>join:<serverHash>        join record JSON       EX 30 (auth role)
 *   <p>rl:<bucket>:<key>        hit counter            PEXPIRE window
 *   <p>online                   ZSET account -> expiry ms
 *   <p>pres:<account>           HASH brain id -> expiry ms, PEXPIRE redis.presence_ttl_s
 *   <p>lb:<query>:<region>[:s<season>] leaderboard rows JSON, EX accounts.leaderboard_cache_s
 * Channels: <p><db channel> (kn_mm_lobby, kn_mm_host, ...: kn_db_set_pubsub),
 *   <p>push:<brain id> (social pushes for accounts connected to that brain).
 */
#ifndef KN_BRAIN_HOT_H
#define KN_BRAIN_HOT_H

#include <stddef.h>
#include <stdint.h>

#include "brain.h"
#include "cJSON.h"

/* BrainModule hooks: runs before federation (it installs the pub/sub transport). */
int hot_init(Brain *b);
void hot_shutdown(Brain *b);

/* "off" (no redis.url / not built in), "up" or "down". */
const char *hot_state(void);
int hot_up(void);

/* ---- rate limits. local_ok = the per-process limiter's verdict for this hit.
 * Returns 0 when the key is over `limit` in the window across every brain (learnt
 * from earlier hits' replies; the global count is updated asynchronously, so a
 * burst may pass up to one extra hit per brain), else local_ok. */
int hot_limit(const char *bucket, const char *key, int limit, int window_s, int local_ok);

/* ---- session cache (token -> account id). The token itself never reaches Redis. */
typedef void (*HotSessionFn)(void *ud, int64_t account_id, uint64_t t0_us);
/* 0 = cb will fire (account_id 0 = miss: verify with the database, then
 * hot_session_store(token, id, t0_us)); -1 = cache not available (cb not called). */
int hot_session_lookup(const char *token, HotSessionFn cb, void *ud);
void hot_session_store(const char *token, int64_t account_id, uint64_t t0_us);
/* Call once the database revocation has committed (logout, password change, trims). */
void hot_session_forget(const char *token);
void hot_session_forget_account(int64_t account_id);

/* ---- generic cached values (join records, leaderboard pages) */
typedef void (*HotGetFn)(void *ud, const char *value, size_t len, int64_t pttl_ms); /* value NULL = miss */
int hot_get(const char *key, HotGetFn cb, void *ud);      /* -1 = not available (cb not called) */
void hot_put(const char *key, const char *value, int ttl_ms);

/* ---- presence: accounts with a social socket on any brain */
typedef size_t (*HotPresenceSource)(int64_t *ids, size_t max); /* this brain's online account ids */
void hot_presence_source(HotPresenceSource fn);
void hot_presence_add(int64_t account_id);
void hot_presence_remove(int64_t account_id);   /* the account's last local socket closed */
typedef void (*HotCountFn)(void *ud, int64_t count); /* -1 = unknown */
int hot_presence_count(HotCountFn cb, void *ud);  /* -1 = not available */
/* online[i] = 1 when ids[i] has a live social socket on any brain (social_eb.c, v10 friends);
 * online NULL = the lookup failed. Returns -1 (cb not called) when Redis is not available. */
typedef void (*HotOnlineFn)(void *ud, const int *online, size_t n);
int hot_presence_online(const int64_t *ids, size_t n, HotOnlineFn cb, void *ud);

/* ---- social pushes to accounts connected to other brains */
typedef void (*HotPushHandler)(int64_t account_id, cJSON *frame, int json_only); /* frame consumed */
void hot_push_handler(HotPushHandler fn);
void hot_push(int64_t account_id, const cJSON *frame, int json_only);

#endif
