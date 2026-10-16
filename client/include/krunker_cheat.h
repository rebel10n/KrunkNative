/*
 * krunker_cheat.h - Krunker official-protocol CHEAT_CHECK (`cc`) responder.
 *
 * The official Krunker server periodically sends a CHEAT_CHECK packet
 *   cc = [nonce, <unused>, ciphertextB64]
 * whose ciphertext XOR-decrypts (keystream derived from `nonce`) to a short
 * snippet of JavaScript source. The client eval()s that source to a result R
 * and replies
 *   ccr = [nonce, xorB64(JSON.stringify(R), nonce), fnv1aJs(nonce + JSON.stringify(R))]
 *
 * This module owns the crypto (keystream / XOR / base64 / the game's JS-style
 * FNV-1a) in portable C, plus one eval seam (krunker_cheat_answer) backed by
 * QuickJS natively and by the browser's eval on the web build. See
 * krunker_cheat.c for the exact algorithm write-up.
 *
 * Reference: game.deob.js:218169 (handler), :194974 (keystream), :218200 (hash).
 * This header is client-only; it must never be pulled into server/ or shared/.
 */
#ifndef KRUNKER_CHEAT_H
#define KRUNKER_CHEAT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Build constants: the current KrunkerBuild's (krunker_build_live.h: a published
 * build.json's, else the compiled ones), whose defaults are the generated
 * krunker_build.h's (tools/krunker/extract_build.py, anchored on
 * game.deob.js:194974 and :218200):
 *   KRUNKER_CHEAT_SALT / _SALT_LEN  the keystream salt, read as _SALT_LEN raw
 *                                   bytes (a string literal or a brace list)
 *   KRUNKER_CHEAT_FNV_OFFSET/_PRIME the hash constants
 *   KRUNKER_CHEAT_FNV_DOUBLE_MUL    1: `PRIME * (h ^= c) | 0` in doubles (this
 *                                   build); 0: an exact 32-bit Math.imul step
 *   KRUNKER_CHEAT_KS_FORMULA        keystream formula revision (1, below) */
#include "krunker_build.h"
#include "krunker_build_live.h" /* the values used at runtime: krunker_build() (a published build.json may change them) */

#if KRUNKER_CHEAT_KS_FORMULA != 1
#error "krunker_build.h: unknown CHEAT_CHECK keystream formula; port the new one in krunker_cheat.c"
#endif

/* ccr arguments produced for one cc challenge. Free with krunker_ccr_free(). */
typedef struct {
    char *nonce;       /* strdup of the challenge nonce (ccr arg 0) */
    char *payload_b64; /* base64(JSON.stringify(R) XOR keystream) (ccr arg 1) */
    char  fnv[9];      /* 8 lowercase hex digits + NUL           (ccr arg 2) */
} KrunkerCcr;

/*
 * Full cc -> ccr. `nonce` and `ciphertext_b64` are the server's cc args 0 and
 * 2, both UTF-8 C strings. On success returns 0 and fills *out (caller frees
 * with krunker_ccr_free). Returns non-zero only on hard failure (OOM, bad
 * base64, engine init) - a thrown eval is NOT a failure: it yields the game's
 * {r:null,e:{_eval:...}} result like the real client.
 */
int  krunker_cheat_answer(const char *nonce, const char *ciphertext_b64,
                          KrunkerCcr *out);
void krunker_ccr_free(KrunkerCcr *out);

/* ---- primitives, exposed for tests and for building/verifying cc frames ---- */

/* Keystream: out[i] = ((u[i % ulen] ^ SALT[i % SALT_LEN]) + i) & 0xff, where u
 * are the UTF-16 code units (JS charCodeAt) of the UTF-8 C string `nonce`.
 * For the live base64url (ASCII) nonces u is just the bytes. */
void krunker_cheat_keystream(const char *nonce, uint8_t *out, size_t len);

/* XOR `data` with keystream(nonce) in place / into `out` (len bytes each). */
void krunker_cheat_xor(const char *nonce, const uint8_t *data, uint8_t *out,
                       size_t len);

/* Standard base64 (RFC 4648, '+'/'/', '=' padding) as atob/btoa use it.
 * decode returns a malloc'd buffer (*out_len set) or NULL on malformed input.
 * encode returns a malloc'd NUL-terminated string or NULL on OOM. */
uint8_t *krunker_cheat_b64_decode(const char *b64, size_t *out_len);
char    *krunker_cheat_b64_encode(const uint8_t *data, size_t len);

/*
 * The game's hash (game.deob.js:218200): FNV-1a over the JS UTF-16 code units
 * of `str`, but with the multiply written as the floating-point
 *   h = 0x1000193 * (h ^= c) | 0
 * not Math.imul - the product can reach ~2^55 and is rounded to a double
 * before ToInt32, so low bits differ from a pure 32-bit imul
 * (KRUNKER_CHEAT_FNV_DOUBLE_MUL; 0 selects the imul step). `str` is a UTF-8
 * C string; it is walked as UTF-16 (astral code points count as two units) to
 * match JS String semantics. Writes 8 lowercase hex + NUL into out9. */
void krunker_cheat_fnv1a_js(const char *str, char out9[9]);

#ifdef __cplusplus
}
#endif

#endif /* KRUNKER_CHEAT_H */
