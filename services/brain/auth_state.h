/*
 * services/brain/auth_state.h - in-memory state of the central auth role (auth.c), kept
 * pure (explicit clocks, no loop / DB) so tests/test_auth_state.c can drive it:
 *
 *   - join records (Mojang sessionserver "join"): the client says "account A joins the
 *     server whose serverHash is H"; hasJoined(username, H) answers A's profile while
 *     the record lives (AUTH_JOIN_TTL_MS). Keyed by H: a hash binds one connection of
 *     one server boot, so a record can never verify on another server.
 *   - fixed-window rate limiters per (bucket, IP) for login / register / join.
 */
#ifndef KN_BRAIN_AUTH_STATE_H
#define KN_BRAIN_AUTH_STATE_H

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

#define AUTH_JOIN_TTL_MS 30000
#define AUTH_JOIN_MAX 65536            /* records kept; the oldest is evicted beyond */

typedef struct AuthJoins AuthJoins;

AuthJoins *auth_joins_new(void);
void auth_joins_free(AuthJoins *j);
/* Record (replaces an older record of the same hash). profile is copied. 0 ok. */
int auth_joins_put(AuthJoins *j, const char *server_hash, int64_t account_id, const char *username,
                   const char *ip, const cJSON *profile, uint64_t now_ms);
/* The profile (borrowed until the next put/purge) when a live record of server_hash
 * matches username (case-insensitive) and, when ip is given and check_ip is set, the
 * client IP of the join; NULL otherwise. */
const cJSON *auth_joins_get(AuthJoins *j, const char *server_hash, const char *username, const char *ip,
                            int check_ip, uint64_t now_ms);
/* Drop expired records; returns how many remain. */
size_t auth_joins_purge(AuthJoins *j, uint64_t now_ms);
size_t auth_joins_count(const AuthJoins *j);

/* ---- rate limiter: at most `limit` hits per `window_ms` per key */
typedef struct AuthLimiter AuthLimiter;

AuthLimiter *auth_limiter_new(int limit, uint64_t window_ms);
void auth_limiter_free(AuthLimiter *l);
/* 1 = allowed (counted), 0 = over the limit. limit <= 0 = unlimited. */
int auth_limiter_hit(AuthLimiter *l, const char *key, uint64_t now_ms);

#endif
