/* knsvc/crypto.h implementation (OpenSSL libcrypto; optional libcrypt). */
#include "knsvc/crypto.h"
#include "knsvc/buf.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef KNSVC_HAVE_LIBCRYPT
#include <crypt.h>
#endif

#define PW_SALT_LEN 16
#define PW_KEY_LEN 32
#define PW_MIN_ITER 10000u
#define PW_MAX_ITER 100000000u

static unsigned g_pw_iterations = 600000;

/* ------------------------------------------------------------------ random */

int kn_random_bytes(void *out, size_t n) {
    if (n > (size_t)0x7fffffff) return -1;
    return RAND_bytes(out, (int)n) == 1 ? 0 : -1;
}

int kn_random_hex(char *out, size_t out_cap, size_t nbytes) {
    uint8_t tmp[256];
    if (nbytes > sizeof tmp || out_cap < nbytes * 2 + 1) return -1;
    if (kn_random_bytes(tmp, nbytes) != 0) return -1;
    int r = kn_hex_encode(tmp, nbytes, out, out_cap);
    OPENSSL_cleanse(tmp, sizeof tmp);
    return r < 0 ? -1 : 0;
}

int kn_random_token(char *out, size_t out_cap, size_t nbytes) {
    uint8_t tmp[256];
    if (nbytes > sizeof tmp) return -1;
    if (kn_random_bytes(tmp, nbytes) != 0) return -1;
    int r = kn_base64_encode(tmp, nbytes, out, out_cap, 1, 0);
    OPENSSL_cleanse(tmp, sizeof tmp);
    return r < 0 ? -1 : 0;
}

uint64_t kn_random_u64(void) {
    uint64_t v = 0;
    if (kn_random_bytes(&v, sizeof v) != 0) abort(); /* never hand out predictable values */
    return v;
}

/* ------------------------------------------------------------------ compare */

int kn_ct_equal(const void *a, const void *b, size_t n) {
    return CRYPTO_memcmp(a, b, n) == 0;
}

int kn_ct_streq(const char *a, const char *b) {
    if (!a || !b) return 0;
    size_t la = strlen(a), lb = strlen(b);
    /* compare over la bytes against b repeated safely; length mismatch still fails */
    unsigned char diff = (unsigned char)(la != lb);
    for (size_t i = 0; i < la; i++) diff |= (unsigned char)(a[i] ^ (lb ? b[i % lb] : 0));
    return diff == 0;
}

/* ------------------------------------------------------------------ digests */

static int digest(const EVP_MD *md, const void *data, size_t len, uint8_t *out) {
    unsigned int olen = 0;
    return EVP_Digest(data, len, out, &olen, md, NULL) == 1 ? 0 : -1;
}

int kn_sha256(const void *data, size_t len, uint8_t out[KN_SHA256_LEN]) {
    return digest(EVP_sha256(), data, len, out);
}

int kn_sha1(const void *data, size_t len, uint8_t out[KN_SHA1_LEN]) {
    return digest(EVP_sha1(), data, len, out);
}

int kn_hmac_sha256(const void *key, size_t keylen, const void *data, size_t len, uint8_t out[KN_SHA256_LEN]) {
    unsigned int olen = 0;
    static const uint8_t empty = 0;
    if (!key) key = &empty;
    if (!data) data = &empty;
    return HMAC(EVP_sha256(), key, (int)keylen, data, len, out, &olen) ? 0 : -1;
}

int kn_hmac_sha1(const void *key, size_t keylen, const void *data, size_t len, uint8_t out[KN_SHA1_LEN]) {
    unsigned int olen = 0;
    static const uint8_t empty = 0;
    if (!key) key = &empty;
    if (!data) data = &empty;
    return HMAC(EVP_sha1(), key, (int)keylen, data, len, out, &olen) ? 0 : -1;
}

/* ---- base32 (RFC 4648) and TOTP (RFC 6238) */

int kn_base32_encode(const void *in, size_t len, char *out, size_t cap) {
    static const char alpha[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    const uint8_t *p = in;
    size_t o = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < len; i++) {
        acc = (acc << 8) | p[i];
        bits += 8;
        while (bits >= 5) {
            if (o + 1 >= cap) return -1;
            out[o++] = alpha[(acc >> (bits - 5)) & 31];
            bits -= 5;
        }
    }
    if (bits > 0) {
        if (o + 1 >= cap) return -1;
        out[o++] = alpha[(acc << (5 - bits)) & 31];
    }
    if (o >= cap) return -1;
    out[o] = 0;
    return (int)o;
}

int kn_base32_decode(const char *in, size_t len, uint8_t *out, size_t cap) {
    uint32_t acc = 0;
    int bits = 0;
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        const char c = in[i];
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a';
        else if (c >= '2' && c <= '7') v = c - '2' + 26;
        else if (c == ' ' || c == '-' || c == '=') continue;
        else return -1;
        acc = (acc << 5) | (uint32_t)v;
        bits += 5;
        if (bits >= 8) {
            if (o >= cap) return -1;
            out[o++] = (uint8_t)(acc >> (bits - 8));
            bits -= 8;
        }
    }
    return (int)o;
}

int kn_totp_code(const uint8_t *secret, size_t secret_len, int64_t step, char *out, size_t cap) {
    uint8_t msg[8], mac[KN_SHA1_LEN];
    if (cap < KN_TOTP_DIGITS + 1 || step < 0) return -1;
    for (int i = 7; i >= 0; i--) {
        msg[i] = (uint8_t)(step & 0xff);
        step >>= 8;
    }
    if (kn_hmac_sha1(secret, secret_len, msg, sizeof msg, mac) != 0) return -1;
    const int off = mac[KN_SHA1_LEN - 1] & 15;
    const uint32_t bin = ((uint32_t)(mac[off] & 0x7f) << 24) | ((uint32_t)mac[off + 1] << 16) |
                         ((uint32_t)mac[off + 2] << 8) | mac[off + 3];
    snprintf(out, cap, "%06u", (unsigned)(bin % 1000000u));
    return 0;
}

int kn_totp_check(const uint8_t *secret, size_t secret_len, const char *code, int64_t now_s, int window,
                  int64_t *last_step) {
    if (!secret || !code || strlen(code) != KN_TOTP_DIGITS || strspn(code, "0123456789") != KN_TOTP_DIGITS) return 0;
    if (now_s <= 0) now_s = (int64_t)time(NULL);
    const int64_t now_step = now_s / KN_TOTP_PERIOD;
    int64_t hit = -1;
    for (int d = -window; d <= window; d++) { /* every candidate, so timing does not reveal which one matched */
        char want[16];
        if (kn_totp_code(secret, secret_len, now_step + d, want, sizeof want) != 0) continue;
        if (kn_ct_streq(want, code) && hit < 0) hit = now_step + d;
    }
    if (hit < 0 || (last_step && hit <= *last_step)) return 0;
    if (last_step) *last_step = hit;
    return 1;
}

int kn_hmac_sha256_hex(const char *key, const void *data, size_t len, char out[65]) {
    uint8_t mac[KN_SHA256_LEN];
    if (kn_hmac_sha256(key, key ? strlen(key) : 0, data, len, mac) != 0) return -1;
    return kn_hex_encode(mac, sizeof mac, out, 65) < 0 ? -1 : 0;
}

int kn_pbkdf2_sha256(const char *password, size_t pwlen, const uint8_t *salt, size_t saltlen,
                     unsigned iterations, uint8_t *out, size_t outlen) {
    return PKCS5_PBKDF2_HMAC(password, (int)pwlen, salt, (int)saltlen, (int)iterations, EVP_sha256(),
                             (int)outlen, out) == 1 ? 0 : -1;
}

/* ------------------------------------------------------------------ encodings */

int kn_hex_encode(const void *in, size_t len, char *out, size_t cap) {
    static const char hex[] = "0123456789abcdef";
    if (cap < len * 2 + 1) return -1;
    const uint8_t *p = in;
    for (size_t i = 0; i < len; i++) {
        out[2 * i] = hex[p[i] >> 4];
        out[2 * i + 1] = hex[p[i] & 15];
    }
    out[len * 2] = 0;
    return (int)(len * 2);
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int kn_hex_decode(const char *in, size_t len, uint8_t *out, size_t cap) {
    if (len % 2 || cap < len / 2) return -1;
    for (size_t i = 0; i < len / 2; i++) {
        int hi = hexval(in[2 * i]), lo = hexval(in[2 * i + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return (int)(len / 2);
}

int kn_base64_encode(const void *in, size_t len, char *out, size_t cap, int url, int pad) {
    const char *alpha = url ? "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"
                            : "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const uint8_t *p = in;
    size_t o = 0;
    size_t need = pad ? ((len + 2) / 3) * 4 : (len / 3) * 4 + (len % 3 ? len % 3 + 1 : 0);
    if (cap < need + 1) return -1;
    size_t i = 0;
    for (; i + 2 < len; i += 3) {
        uint32_t v = (uint32_t)p[i] << 16 | (uint32_t)p[i + 1] << 8 | p[i + 2];
        out[o++] = alpha[v >> 18];
        out[o++] = alpha[(v >> 12) & 63];
        out[o++] = alpha[(v >> 6) & 63];
        out[o++] = alpha[v & 63];
    }
    if (i < len) {
        uint32_t v = (uint32_t)p[i] << 16 | (i + 1 < len ? (uint32_t)p[i + 1] << 8 : 0);
        out[o++] = alpha[v >> 18];
        out[o++] = alpha[(v >> 12) & 63];
        if (i + 1 < len) out[o++] = alpha[(v >> 6) & 63];
        else if (pad) out[o++] = '=';
        if (pad) out[o++] = '=';
    }
    out[o] = 0;
    return (int)o;
}

static int b64val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

int kn_base64_decode(const char *in, size_t len, uint8_t *out, size_t cap) {
    while (len && in[len - 1] == '=') len--;
    if (len % 4 == 1) return -1;
    size_t need = len / 4 * 3 + (len % 4 ? len % 4 - 1 : 0);
    if (cap < need) return -1;
    size_t o = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < len; i++) {
        int v = b64val(in[i]);
        if (v < 0) return -1;
        acc = acc << 6 | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[o++] = (uint8_t)(acc >> bits);
        }
    }
    if (bits && (acc & ((1u << bits) - 1))) return -1; /* non-canonical trailing bits */
    return (int)o;
}

/* ------------------------------------------------------------------ passwords */

void kn_password_set_iterations(unsigned iterations) {
    if (iterations < PW_MIN_ITER) iterations = PW_MIN_ITER;
    if (iterations > PW_MAX_ITER) iterations = PW_MAX_ITER;
    g_pw_iterations = iterations;
}

unsigned kn_password_get_iterations(void) {
    return g_pw_iterations;
}

int kn_password_hash(const char *password, char *out, size_t cap) {
    uint8_t salt[PW_SALT_LEN], key[PW_KEY_LEN];
    char salt64[32], key64[48];
    if (!password || kn_random_bytes(salt, sizeof salt) != 0) return -1;
    if (kn_pbkdf2_sha256(password, strlen(password), salt, sizeof salt, g_pw_iterations, key, sizeof key) != 0)
        return -1;
    kn_base64_encode(salt, sizeof salt, salt64, sizeof salt64, 0, 0);
    kn_base64_encode(key, sizeof key, key64, sizeof key64, 0, 0);
    OPENSSL_cleanse(key, sizeof key);
    int n = snprintf(out, cap, "$pbkdf2-sha256$i=%u$%s$%s", g_pw_iterations, salt64, key64);
    return (n < 0 || (size_t)n >= cap) ? -1 : 0;
}

/* Parse "$pbkdf2-sha256$i=N$salt$hash". 0 ok. */
static int parse_pbkdf2(const char *enc, unsigned *iter, uint8_t *salt, int *saltlen, uint8_t *key, int *keylen) {
    static const char prefix[] = "$pbkdf2-sha256$i=";
    if (strncmp(enc, prefix, sizeof prefix - 1) != 0) return -1;
    const char *p = enc + sizeof prefix - 1;
    char *end;
    unsigned long it = strtoul(p, &end, 10);
    if (end == p || *end != '$' || it < 1000 || it > PW_MAX_ITER) return -1;
    const char *s = end + 1;
    const char *d = strchr(s, '$');
    if (!d) return -1;
    *saltlen = kn_base64_decode(s, (size_t)(d - s), salt, 64);
    *keylen = kn_base64_decode(d + 1, strlen(d + 1), key, 64);
    if (*saltlen < 8 || *keylen < 16) return -1;
    *iter = (unsigned)it;
    return 0;
}

int kn_password_verify(const char *password, const char *encoded) {
    if (!password || !encoded) return -1;
    uint8_t salt[64], key[64], got[64];
    int saltlen, keylen;
    unsigned iter;
    if (parse_pbkdf2(encoded, &iter, salt, &saltlen, key, &keylen) == 0) {
        if (kn_pbkdf2_sha256(password, strlen(password), salt, (size_t)saltlen, iter, got, (size_t)keylen) != 0)
            return -1;
        int ok = kn_ct_equal(got, key, (size_t)keylen);
        OPENSSL_cleanse(got, sizeof got);
        return ok;
    }
#ifdef KNSVC_HAVE_LIBCRYPT
    if (encoded[0] == '$') {
        struct crypt_data *cd = calloc(1, sizeof *cd);
        if (!cd) return -1;
        const char *h = crypt_r(password, encoded, cd);
        int r = -1;
        if (h && h[0] != '*') r = strlen(h) == strlen(encoded) && kn_ct_streq(h, encoded);
        OPENSSL_cleanse(cd, sizeof *cd);
        free(cd);
        return r;
    }
#endif
    return -1;
}

int kn_password_needs_rehash(const char *encoded) {
    uint8_t salt[64], key[64];
    int saltlen, keylen;
    unsigned iter;
    if (!encoded || parse_pbkdf2(encoded, &iter, salt, &saltlen, key, &keylen) != 0) return 1;
    return iter < g_pw_iterations;
}

/* ------------------------------------------------------------------ signed tokens (JWT HS256) */

static int b64url_append(kn_buf *b, const void *data, size_t len) {
    size_t need = (len / 3 + 1) * 4 + 1;
    if (kn_buf_reserve(b, need) != 0) return -1;
    int n = kn_base64_encode(data, len, b->data + b->len, need, 1, 0);
    if (n < 0) return -1;
    b->len += (size_t)n;
    return 0;
}

char *kn_signed_token_create(const char *secret, const cJSON *claims, int64_t ttl_s) {
    if (!secret || !*secret) return NULL;
    cJSON *c = claims ? cJSON_Duplicate(claims, 1) : cJSON_CreateObject();
    if (!c || !cJSON_IsObject(c)) {
        cJSON_Delete(c);
        return NULL;
    }
    int64_t now = (int64_t)time(NULL);
    cJSON_DeleteItemFromObjectCaseSensitive(c, "iat");
    cJSON_AddNumberToObject(c, "iat", (double)now);
    if (ttl_s > 0) {
        cJSON_DeleteItemFromObjectCaseSensitive(c, "exp");
        cJSON_AddNumberToObject(c, "exp", (double)(now + ttl_s));
    }
    char *payload = cJSON_PrintUnformatted(c);
    cJSON_Delete(c);
    if (!payload) return NULL;
    static const char header[] = "{\"alg\":\"HS256\",\"typ\":\"JWT\"}";
    kn_buf b = {0};
    int ok = b64url_append(&b, header, sizeof header - 1) == 0 && kn_buf_append_byte(&b, '.') == 0 &&
             b64url_append(&b, payload, strlen(payload)) == 0;
    free(payload);
    uint8_t mac[KN_SHA256_LEN];
    ok = ok && kn_hmac_sha256(secret, strlen(secret), b.data, b.len, mac) == 0 &&
         kn_buf_append_byte(&b, '.') == 0 && b64url_append(&b, mac, sizeof mac) == 0;
    if (!ok) {
        kn_buf_free(&b);
        return NULL;
    }
    return kn_buf_detach(&b, NULL);
}

static cJSON *token_fail(char *err, size_t errlen, const char *why) {
    if (err && errlen) snprintf(err, errlen, "%s", why);
    return NULL;
}

cJSON *kn_signed_token_verify(const char *secret, const char *token, int64_t now_s, char *err, size_t errlen) {
    if (!secret || !*secret || !token) return token_fail(err, errlen, "missing secret or token");
    const char *dot1 = strchr(token, '.');
    const char *dot2 = dot1 ? strchr(dot1 + 1, '.') : NULL;
    if (!dot1 || !dot2 || strchr(dot2 + 1, '.')) return token_fail(err, errlen, "malformed token");
    size_t siglen = strlen(dot2 + 1);
    uint8_t sig[64], mac[KN_SHA256_LEN];
    if (kn_base64_decode(dot2 + 1, siglen, sig, sizeof sig) != KN_SHA256_LEN)
        return token_fail(err, errlen, "malformed signature");
    if (kn_hmac_sha256(secret, strlen(secret), token, (size_t)(dot2 - token), mac) != 0)
        return token_fail(err, errlen, "hmac failure");
    if (!kn_ct_equal(sig, mac, sizeof mac)) return token_fail(err, errlen, "bad signature");

    size_t hlen = (size_t)(dot1 - token), plen = (size_t)(dot2 - dot1 - 1);
    uint8_t *raw = malloc((hlen > plen ? hlen : plen) + 4);
    if (!raw) return token_fail(err, errlen, "out of memory");
    int n = kn_base64_decode(token, hlen, raw, hlen + 4);
    cJSON *hdr = n > 0 ? cJSON_ParseWithLength((const char *)raw, (size_t)n) : NULL;
    const cJSON *alg = cJSON_GetObjectItemCaseSensitive(hdr, "alg");
    int alg_ok = cJSON_IsString(alg) && strcmp(alg->valuestring, "HS256") == 0;
    cJSON_Delete(hdr);
    if (!alg_ok) {
        free(raw);
        return token_fail(err, errlen, "unsupported alg");
    }
    n = kn_base64_decode(dot1 + 1, plen, raw, plen + 4);
    cJSON *claims = n > 0 ? cJSON_ParseWithLength((const char *)raw, (size_t)n) : NULL;
    free(raw);
    if (!cJSON_IsObject(claims)) {
        cJSON_Delete(claims);
        return token_fail(err, errlen, "malformed claims");
    }
    if (!now_s) now_s = (int64_t)time(NULL);
    const cJSON *exp = cJSON_GetObjectItemCaseSensitive(claims, "exp");
    if (exp && (!cJSON_IsNumber(exp) || (int64_t)exp->valuedouble < now_s)) {
        cJSON_Delete(claims);
        return token_fail(err, errlen, "token expired");
    }
    return claims;
}
