/*
 * services/brain/captcha.c - the ALTCHA challenges of Box Office's captcha: issue, verify, replay and expiry
 * (captcha.h, doc/v10/captcha.md). No Brain here: the module and its route are captcha_http.c.
 */
#include "captcha.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "knsvc/crypto.h"
#include "knsvc/json.h"
#include "knsvc/log.h"
#include "knsvc/loop.h"

#define MOD "captcha"
#define CAPTCHA_KDF_LABEL "kn-captcha-altcha-v1"
#define CAPTCHA_SALT_BYTES 12 /* altcha-lib's default saltLength */
#define CAPTCHA_SALT_MAX 160
#define CAPTCHA_NUMBER_MAX 1000000000000LL /* a JSON number past it is not one this brain chose */
/* the min / max of captcha.max_number: the client refuses a challenge over 1e7 (KRUNKER_ALTCHA_MAX_CAP) */
#define CAPTCHA_MAX_NUMBER_MIN 1000L
#define CAPTCHA_MAX_NUMBER_MAX 10000000L
/* spent challenges: open addressing over a fixed table, like mm_token.c's spent validation tokens. A flood past
 * it evicts the entry closest to expiry, which could then be accepted once more: each one still cost its solver
 * a proof of work */
#define CAPTCHA_SPENT_SLOTS 16384
#define CAPTCHA_SPENT_PROBE 16

static struct {
    char key[65];
    long max_number;
    int ttl_s;
    int enforce;
} g_cap = {.max_number = CAPTCHA_DEFAULT_MAX_NUMBER, .ttl_s = CAPTCHA_DEFAULT_TTL_S};

typedef struct {
    char challenge[65];
    int64_t exp_ms;
} SpentSlot;

static SpentSlot g_spent[CAPTCHA_SPENT_SLOTS];

int captcha_configure(const CaptchaSettings *s) {
    const long max = s->max_number > 0 ? s->max_number : CAPTCHA_DEFAULT_MAX_NUMBER;
    g_cap.max_number = max < CAPTCHA_MAX_NUMBER_MIN ? CAPTCHA_MAX_NUMBER_MIN : max > CAPTCHA_MAX_NUMBER_MAX ? CAPTCHA_MAX_NUMBER_MAX : max;
    const int ttl = s->ttl_s > 0 ? s->ttl_s : CAPTCHA_DEFAULT_TTL_S;
    g_cap.ttl_s = ttl < 10 ? 10 : ttl > 3600 ? 3600 : ttl;
    g_cap.enforce = s->enforce != 0;
    g_cap.key[0] = 0;
    memset(g_spent, 0, sizeof g_spent);
    if (!s->join_secret || !*s->join_secret) return -1;
    /* derived: a captcha signature is never a validation token's or a join ticket's (other keys) */
    return kn_hmac_sha256_hex(s->join_secret, CAPTCHA_KDF_LABEL, strlen(CAPTCHA_KDF_LABEL), g_cap.key);
}

int captcha_enforced(void) {
    return g_cap.enforce && g_cap.key[0];
}

long captcha_max_number(void) {
    return g_cap.max_number;
}

int captcha_ttl(void) {
    return g_cap.ttl_s;
}

int captcha_purpose_ok(const char *purpose) {
    /* v10.0.3's bCpt names its flow ("hosting", deob-10.0.3.js:229992); the sign-in ones are Box Office's */
    return purpose && (!strcmp(purpose, "login") || !strcmp(purpose, "register") || !strcmp(purpose, "hosting"));
}

/* hex(SHA-256(salt + decimal(number))): altcha.1.0.6.min.js kt(salt, number, "SHA-256") */
static int challenge_of(const char *salt, long long number, char out[65]) {
    char buf[CAPTCHA_SALT_MAX + 24];
    const int n = snprintf(buf, sizeof buf, "%s%lld", salt, number);
    uint8_t h[KN_SHA256_LEN];
    if (n <= 0 || (size_t) n >= sizeof buf || kn_sha256(buf, (size_t) n, h) != 0) return -1;
    return kn_hex_encode(h, sizeof h, out, 65) == 64 ? 0 : -1;
}

char *captcha_issue(const char *purpose, int64_t now_ms) {
    if (!g_cap.key[0] || !captcha_purpose_ok(purpose)) return NULL;
    if (!now_ms) now_ms = kn_wall_ms();
    char rnd[CAPTCHA_SALT_BYTES * 2 + 1], salt[CAPTCHA_SALT_MAX], challenge[65], signature[65];
    if (kn_random_hex(rnd, sizeof rnd, CAPTCHA_SALT_BYTES) != 0) return NULL;
    /* altcha-lib's salt params (createChallenge's expires): the widget reads ?expires= back
     * (altcha.1.0.6.min.js, its challengeurl fetch) */
    snprintf(salt, sizeof salt, "%s?expires=%lld&for=%s", rnd, (long long) (now_ms / 1000 + g_cap.ttl_s), purpose);
    const long long number = (long long) (kn_random_u64() % (uint64_t) (g_cap.max_number + 1));
    if (challenge_of(salt, number, challenge) != 0) return NULL;
    if (kn_hmac_sha256_hex(g_cap.key, challenge, 64, signature) != 0) return NULL;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "algorithm", "SHA-256");
    cJSON_AddStringToObject(o, "challenge", challenge);
    cJSON_AddNumberToObject(o, "maxnumber", (double) g_cap.max_number);
    cJSON_AddStringToObject(o, "salt", salt);
    cJSON_AddStringToObject(o, "signature", signature);
    char *json = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return json;
}

/* the value of key in the salt's "?a=b&c=d" params; 0 when absent or too long */
static int salt_param(const char *salt, const char *key, char *out, size_t n) {
    const char *p = strchr(salt, '?');
    const size_t kl = strlen(key);
    if (!p) return 0;
    for (p++; *p;) {
        const char *amp = strchr(p, '&');
        const size_t len = amp ? (size_t) (amp - p) : strlen(p);
        if (len > kl && !strncmp(p, key, kl) && p[kl] == '=') {
            const size_t vl = len - kl - 1;
            if (vl >= n) return 0;
            memcpy(out, p + kl + 1, vl);
            out[vl] = 0;
            return 1;
        }
        p += len;
        if (*p == '&') p++;
    }
    return 0;
}

static uint32_t fnv1a(const char *s) {
    uint32_t h = 2166136261u;
    for (; *s; s++) h = (h ^ (uint8_t) *s) * 16777619u;
    return h;
}

/* 1 = the challenge was spent already; else it is recorded as spent until exp_ms */
static int spend(const char *challenge, int64_t exp_ms, int64_t now_ms) {
    const uint32_t base = fnv1a(challenge);
    SpentSlot *free_slot = NULL, *oldest = NULL;
    for (uint32_t i = 0; i < CAPTCHA_SPENT_PROBE; i++) {
        SpentSlot *s = &g_spent[(base + i) & (CAPTCHA_SPENT_SLOTS - 1)];
        const int live = s->challenge[0] && s->exp_ms > now_ms;
        if (live && !strcmp(s->challenge, challenge)) return 1;
        if (!live && !free_slot) free_slot = s;
        if (!oldest || s->exp_ms < oldest->exp_ms) oldest = s;
    }
    SpentSlot *s = free_slot ? free_slot : oldest;
    snprintf(s->challenge, sizeof s->challenge, "%s", challenge);
    s->exp_ms = exp_ms;
    return 0;
}

static CaptchaResult verify_json(const cJSON *o, const char *purpose, int64_t now_ms) {
    const char *alg = kn_json_str(o, "algorithm", NULL), *challenge = kn_json_str(o, "challenge", NULL);
    const char *salt = kn_json_str(o, "salt", NULL), *signature = kn_json_str(o, "signature", NULL);
    const cJSON *num = cJSON_GetObjectItemCaseSensitive(o, "number");
    if (!alg || !challenge || !salt || !signature || !cJSON_IsNumber(num)) return CAPTCHA_INVALID;
    /* the only algorithm issued (the widget also knows SHA-384 / SHA-512: never offered here) */
    if (strcmp(alg, "SHA-256") != 0 || strlen(challenge) != 64 || strlen(signature) != 64 || strlen(salt) >= CAPTCHA_SALT_MAX)
        return CAPTCHA_INVALID;
    const double d = num->valuedouble;
    if (!(d >= 0 && d <= (double) CAPTCHA_NUMBER_MAX) || (double) (long long) d != d) return CAPTCHA_INVALID;
    char want[65], want_sig[65];
    if (challenge_of(salt, (long long) d, want) != 0 || !kn_ct_streq(want, challenge)) return CAPTCHA_INVALID;
    if (kn_hmac_sha256_hex(g_cap.key, challenge, 64, want_sig) != 0 || !kn_ct_streq(want_sig, signature)) return CAPTCHA_INVALID;
    /* ours from here on: the salt (its params included) is under the signature */
    char expires[24], what[24];
    if (!salt_param(salt, "expires", expires, sizeof expires)) return CAPTCHA_INVALID;
    char *end = NULL;
    const long long exp_s = strtoll(expires, &end, 10);
    if (!end || *end || exp_s <= 0) return CAPTCHA_INVALID;
    if (exp_s * 1000 <= now_ms) return CAPTCHA_EXPIRED;
    if (!salt_param(salt, "for", what, sizeof what) || !purpose || strcmp(what, purpose) != 0) return CAPTCHA_PURPOSE;
    return spend(challenge, exp_s * 1000, now_ms) ? CAPTCHA_SPENT : CAPTCHA_OK;
}

CaptchaResult captcha_verify(const char *payload, const char *purpose, int64_t now_ms) {
    if (!payload || !*payload) return CAPTCHA_MISSING;
    const size_t len = strlen(payload);
    if (len > CAPTCHA_PAYLOAD_MAX || !g_cap.key[0]) return CAPTCHA_INVALID;
    if (!now_ms) now_ms = kn_wall_ms();
    uint8_t raw[CAPTCHA_PAYLOAD_MAX];
    const int n = kn_base64_decode(payload, len, raw, sizeof raw);
    if (n <= 0) return CAPTCHA_INVALID;
    cJSON *o = kn_json_parse((const char *) raw, (size_t) n);
    const CaptchaResult r = cJSON_IsObject(o) ? verify_json(o, purpose, now_ms) : CAPTCHA_INVALID;
    cJSON_Delete(o);
    return r;
}

const char *captcha_result_name(CaptchaResult r) {
    switch (r) {
        case CAPTCHA_OK: return "ok";
        case CAPTCHA_MISSING: return "missing";
        case CAPTCHA_INVALID: return "invalid";
        case CAPTCHA_EXPIRED: return "expired";
        case CAPTCHA_SPENT: return "already used";
        case CAPTCHA_PURPOSE: return "for another flow";
    }
    return "?";
}

const char *captcha_refusal_text(CaptchaResult r) {
    if (r == CAPTCHA_OK) return NULL;
    return r == CAPTCHA_MISSING ? "Please complete the security challenge to continue." : "CAPTCHA FAILED";
}

const char *captcha_refusal_error(CaptchaResult r) {
    if (r == CAPTCHA_OK) return NULL;
    return r == CAPTCHA_MISSING ? "CaptchaRequired" : "RecaptchaFailed";
}

int captcha_gate(const char *payload, const char *purpose, const char *who, CaptchaResult *result) {
    const CaptchaResult r = captcha_verify(payload, purpose, 0);
    if (result) *result = r;
    if (r == CAPTCHA_OK) {
        KN_DEBUG(MOD, "%s from %s: captcha accepted", purpose, who ? who : "?");
        return 1;
    }
    if (!captcha_enforced()) {
        /* the rollout's first step (doc/backend.md 7.10): what enforcement would refuse, refused nothing */
        KN_DEBUG(MOD, "%s from %s: captcha %s (captcha.enforce off: let through)", purpose, who ? who : "?", captcha_result_name(r));
        return 1;
    }
    KN_INFO(MOD, "%s from %s refused: captcha %s", purpose, who ? who : "?", captcha_result_name(r));
    return 0;
}
