/*
 * services/brain/mm_token.c - the matchmaker validation token (mm_token.h,
 * doc/v10/matchmaker-token.md).
 */
#include "mm_token.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "knsvc/crypto.h"
#include "knsvc/json.h"
#include "knsvc/loop.h"

#define MM_TOKEN_TYP "mmv"
#define MM_TOKEN_KDF_LABEL "kn-mm-validation-v1"
#define MM_TOKEN_JTI_BYTES 12
#define MM_TOKEN_IP_HEX 12
/* spent tokens: open addressing over a fixed table (a seek is rare; 8192 live tokens per brain
 * within one ttl is far above any real rate, and the generate-token route is rate-limited) */
#define MM_SPENT_SLOTS 8192
#define MM_SPENT_PROBE 16

static struct {
    char key[65];
    int ttl_s;
    int bind_ip;
    int require;
} g_tok = {.ttl_s = 60};

typedef struct {
    char jti[MM_TOKEN_JTI_BYTES * 2 + 1];
    int64_t exp_ms;
} SpentSlot;

static SpentSlot g_spent[MM_SPENT_SLOTS];

int mm_token_init(const MmTokenSettings *s) {
    g_tok.ttl_s = s->ttl_s < 5 ? 5 : s->ttl_s > 3600 ? 3600 : s->ttl_s;
    g_tok.bind_ip = s->bind_ip != 0;
    g_tok.require = s->require != 0;
    g_tok.key[0] = 0;
    if (s->secret && *s->secret) {
        /* an explicit brain.validation_secret: also hashed, so any length works */
        return kn_hmac_sha256_hex(s->secret, MM_TOKEN_KDF_LABEL, strlen(MM_TOKEN_KDF_LABEL), g_tok.key);
    }
    if (!s->join_secret || !*s->join_secret) return -1;
    /* derived: a validation token is never a valid join ticket (another key) and vice versa */
    return kn_hmac_sha256_hex(s->join_secret, MM_TOKEN_KDF_LABEL, strlen(MM_TOKEN_KDF_LABEL), g_tok.key);
}

int mm_token_required(void) {
    return g_tok.require;
}

int mm_token_ttl(void) {
    return g_tok.ttl_s;
}

static void ip_tag(const char *addr, char out[MM_TOKEN_IP_HEX + 1]) {
    uint8_t h[KN_SHA256_LEN];
    char hex[2 * KN_SHA256_LEN + 1];
    const char *a = addr ? addr : "";
    /* ::ffff:1.2.3.4 and 1.2.3.4 are the same client */
    if (!strncmp(a, "::ffff:", 7) && strchr(a + 7, '.')) a += 7;
    if (kn_sha256(a, strlen(a), h) != 0 || kn_hex_encode(h, sizeof h, hex, sizeof hex) < 0) {
        out[0] = 0;
        return;
    }
    memcpy(out, hex, MM_TOKEN_IP_HEX);
    out[MM_TOKEN_IP_HEX] = 0;
}

char *mm_token_issue(const char *remote_addr) {
    if (!g_tok.key[0]) return NULL;
    char jti[MM_TOKEN_JTI_BYTES * 2 + 1], ip[MM_TOKEN_IP_HEX + 1];
    if (kn_random_hex(jti, sizeof jti, MM_TOKEN_JTI_BYTES) != 0) return NULL;
    ip_tag(remote_addr, ip);
    cJSON *claims = cJSON_CreateObject();
    cJSON_AddStringToObject(claims, "typ", MM_TOKEN_TYP);
    cJSON_AddStringToObject(claims, "jti", jti);
    cJSON_AddStringToObject(claims, "ip", ip);
    char *token = kn_signed_token_create(g_tok.key, claims, g_tok.ttl_s);
    cJSON_Delete(claims);
    return token;
}

static uint32_t fnv1a(const char *s) {
    uint32_t h = 2166136261u;
    for (; *s; s++) h = (h ^ (uint8_t) *s) * 16777619u;
    return h;
}

/* 1 = the jti was already spent; else it is recorded as spent until exp_ms */
static int spend(const char *jti, int64_t exp_ms, int64_t now_ms) {
    const uint32_t base = fnv1a(jti);
    SpentSlot *free_slot = NULL, *oldest = NULL;
    for (uint32_t i = 0; i < MM_SPENT_PROBE; i++) {
        SpentSlot *s = &g_spent[(base + i) & (MM_SPENT_SLOTS - 1)];
        const int live = s->jti[0] && s->exp_ms > now_ms;
        if (live && !strcmp(s->jti, jti)) return 1;
        if (!live && !free_slot) free_slot = s;
        if (!oldest || s->exp_ms < oldest->exp_ms) oldest = s;
    }
    SpentSlot *s = free_slot ? free_slot : oldest; /* a flood evicts the token closest to expiry */
    snprintf(s->jti, sizeof s->jti, "%s", jti);
    s->exp_ms = exp_ms;
    return 0;
}

MmTokenResult mm_token_check(const char *token, const char *remote_addr, int64_t now_ms) {
    if (!token || !*token) return MM_TOKEN_MISSING;
    if (!g_tok.key[0]) return MM_TOKEN_INVALID;
    if (!now_ms) now_ms = kn_wall_ms();
    char err[64] = "";
    cJSON *claims = kn_signed_token_verify(g_tok.key, token, now_ms / 1000, err, sizeof err);
    if (!claims) return strstr(err, "expired") ? MM_TOKEN_EXPIRED : MM_TOKEN_INVALID;
    MmTokenResult r = MM_TOKEN_OK;
    const char *typ = kn_json_str(claims, "typ", "");
    const char *jti = kn_json_str(claims, "jti", "");
    const char *ip = kn_json_str(claims, "ip", "");
    if (strcmp(typ, MM_TOKEN_TYP) != 0 || strlen(jti) != MM_TOKEN_JTI_BYTES * 2) {
        r = MM_TOKEN_DATA;
    } else if (g_tok.bind_ip) {
        char want[MM_TOKEN_IP_HEX + 1];
        ip_tag(remote_addr, want);
        if (!kn_ct_streq(ip, want)) r = MM_TOKEN_IP;
    }
    if (r == MM_TOKEN_OK) {
        const int64_t exp_ms = (int64_t) kn_json_int(claims, "exp", now_ms / 1000 + g_tok.ttl_s) * 1000;
        if (spend(jti, exp_ms, now_ms)) r = MM_TOKEN_INVALID;
    }
    cJSON_Delete(claims);
    return r;
}

const char *mm_token_error_name(MmTokenResult r) {
    switch (r) {
        case MM_TOKEN_INVALID: return "InvalidClientValidationToken";
        case MM_TOKEN_EXPIRED: return "ExpiredClientValidationToken";
        case MM_TOKEN_DATA: return "InvalidClientValidationTokenData";
        case MM_TOKEN_IP: return "InvalidClientIP";
        case MM_TOKEN_MISSING: /* only when required: the same name Krunker's client shows */
            return mm_token_required() ? "InvalidClientValidationToken" : NULL;
        default: return NULL;
    }
}
