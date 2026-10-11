/*
 * knsvc/crypto.h - OpenSSL-backed helpers: CSPRNG, password hashing, HMAC,
 * constant-time compare, encodings and signed (JWT HS256) tokens.
 *
 * Password hash format (stored in a TEXT column, <= KN_PASSWORD_HASH_MAX bytes):
 *     $pbkdf2-sha256$i=<iterations>$<salt b64>$<hash b64>     (PHC style, std base64 no padding)
 * PBKDF2-HMAC-SHA256, 16-byte random salt, 32-byte key. Default iterations
 * 600000 (OWASP 2023), tunable with kn_password_set_iterations() / config
 * "accounts.password_iterations". kn_password_verify also accepts crypt(3)
 * strings ("$y$..." yescrypt, "$6$..." sha512crypt) when libcrypt is available.
 *
 * Signed tokens (brain -> game server join tokens, sessions): compact JWS
 *     base64url(header).base64url(claims).base64url(HMAC-SHA256)
 * with header {"alg":"HS256","typ":"JWT"}; "iat"/"exp" (seconds) are added.
 */
#ifndef KNSVC_CRYPTO_H
#define KNSVC_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

#define KN_PASSWORD_HASH_MAX 160
#define KN_SHA256_LEN 32
#define KN_SHA1_LEN 20

int kn_random_bytes(void *out, size_t n);                  /* 0 ok, -1 failure (never weak fallback) */
/* nbytes random bytes as lowercase hex (out_cap >= 2*nbytes+1). 0 ok. */
int kn_random_hex(char *out, size_t out_cap, size_t nbytes);
/* nbytes random bytes as base64url without padding (out_cap >= 4*ceil(nbytes/3)+1). 0 ok. */
int kn_random_token(char *out, size_t out_cap, size_t nbytes);
uint64_t kn_random_u64(void);

void kn_password_set_iterations(unsigned iterations);     /* clamps to >= 10000 */
unsigned kn_password_get_iterations(void);
int kn_password_hash(const char *password, char *out, size_t cap);      /* 0 ok */
int kn_password_verify(const char *password, const char *encoded);      /* 1 match, 0 no, -1 bad format */
int kn_password_needs_rehash(const char *encoded);  /* 1 if not pbkdf2-sha256 or fewer iterations than current */

int kn_ct_equal(const void *a, const void *b, size_t n);   /* 1 equal; time independent of content */
int kn_ct_streq(const char *a, const char *b);             /* constant time for equal lengths */

int kn_sha256(const void *data, size_t len, uint8_t out[KN_SHA256_LEN]);
int kn_sha1(const void *data, size_t len, uint8_t out[KN_SHA1_LEN]);
int kn_hmac_sha256(const void *key, size_t keylen, const void *data, size_t len, uint8_t out[KN_SHA256_LEN]);
/* HMAC as lowercase hex (out >= 65 bytes). */
int kn_hmac_sha256_hex(const char *key, const void *data, size_t len, char out[65]);
int kn_pbkdf2_sha256(const char *password, size_t pwlen, const uint8_t *salt, size_t saltlen,
                     unsigned iterations, uint8_t *out, size_t outlen);

/* Encodings. Encoders return output length (excluding NUL) or -1 when out is
 * too small; decoders return decoded length or -1 on invalid input. */
int kn_hex_encode(const void *in, size_t len, char *out, size_t cap);
int kn_hex_decode(const char *in, size_t len, uint8_t *out, size_t cap);
int kn_base64_encode(const void *in, size_t len, char *out, size_t cap, int url, int pad);
int kn_base64_decode(const char *in, size_t len, uint8_t *out, size_t cap); /* std or url, padding optional */

/* Signed tokens. claims is not consumed. ttl_s <= 0 = no "exp". Returns malloc'd string. */
char *kn_signed_token_create(const char *secret, const cJSON *claims, int64_t ttl_s);
/* Returns the claims (caller frees) or NULL when the signature/format is bad or
 * exp < now_s (now_s 0 = current time). err (optional) gets the reason. */
cJSON *kn_signed_token_verify(const char *secret, const char *token, int64_t now_s, char *err, size_t errlen);

#endif
