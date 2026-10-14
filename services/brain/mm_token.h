/*
 * services/brain/mm_token.h - the matchmaker validation token: Krunker's HTTP token step in
 * front of seek-game (doc/v10/matchmaker-token.md).
 *
 * Krunker: the loader fetches GET matchmaker.krunker.io/generate-token (plain text) and the
 * client passes it as /seek-game ?validationToken=; the matchmaker refuses a missing, expired or
 * foreign one (InvalidClientValidationToken, ExpiredClientValidationToken,
 * InvalidClientValidationTokenData, InvalidClientIP) before it hands out a seat and the join
 * ticket (clientId) the game server checks.
 *
 * Here the token is a compact JWS (knsvc kn_signed_token, HS256) with the claims
 * {"typ":"mmv","jti":<12 random bytes hex>,"ip":<12 hex of sha256(client address)>,"iat","exp"}
 * under a key derived from brain.join_secret (HMAC-SHA256(join_secret, "kn-mm-validation-v1"),
 * hex), or brain.validation_secret when that is set. Every brain of a network shares
 * join_secret, so any brain accepts a token another one issued. A token is spent by the seek it
 * validates (one seek per token, per brain: the replay table is in memory).
 */
#ifndef KN_BRAIN_MM_TOKEN_H
#define KN_BRAIN_MM_TOKEN_H

#include <stddef.h>
#include <stdint.h>

typedef enum MmTokenResult {
    MM_TOKEN_OK = 0,
    MM_TOKEN_MISSING,  /* no validationToken (an older client): allowed unless required */
    MM_TOKEN_INVALID,  /* bad signature / malformed / already spent */
    MM_TOKEN_EXPIRED,  /* exp passed */
    MM_TOKEN_DATA,     /* signed by us but not a validation token (e.g. a join ticket) */
    MM_TOKEN_IP,       /* issued to another address (brain.matchmaking.validation_bind_ip) */
} MmTokenResult;

typedef struct MmTokenSettings {
    const char *secret;      /* brain.validation_secret ("" = derived from join_secret) */
    const char *join_secret; /* brain.join_secret */
    int ttl_s;               /* brain.matchmaking.validation_ttl_s (default 60) */
    int bind_ip;             /* brain.matchmaking.validation_bind_ip (default off) */
    int require;             /* brain.matchmaking.require_validation_token (default off) */
} MmTokenSettings;

/* Loads the key and knobs (mm_settings_load). 0 ok, -1 no usable key. */
int mm_token_init(const MmTokenSettings *s);
int mm_token_required(void);
int mm_token_ttl(void);

/* A fresh token for a client at remote_addr (NULL = unknown). malloc'd, NULL on failure. */
char *mm_token_issue(const char *remote_addr);

/* Checks (and spends) a token; NULL or "" = MM_TOKEN_MISSING. now_ms 0 = the wall clock. */
MmTokenResult mm_token_check(const char *token, const char *remote_addr, int64_t now_ms);

/* Krunker's matchmaker error name of a refusal (NULL for OK / MISSING). */
const char *mm_token_error_name(MmTokenResult r);

#endif
