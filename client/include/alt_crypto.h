#pragma once
// AES-256-GCM for the alt manager's vault (client/src/alt/alt_crypto.c). The backend is picked
// per build: OpenSSL's EVP where the client links libcrypto (KRUNKNATIVE_HAVE_TLS), SubtleCrypto
// under a non-extractable key in IndexedDB on the web (alt_web.h; the key argument is then unused,
// the browser holds the key), and none elsewhere (alt_crypto_available() == 0, so the manager
// reports itself unavailable instead of storing anything).
#include <stddef.h>
#include <stdint.h>

#define ALT_KEY_LEN 32  // AES-256
#define ALT_IV_LEN 12   // the GCM nonce: fresh random per write
#define ALT_TAG_LEN 16  // the GCM tag, always full length

// 1 when seal / open / random work in this build.
int alt_crypto_available(void);
// CSPRNG bytes, 0 ok, -1 failure (never a weak fallback).
int alt_random_bytes(void *out, size_t n);
// Encrypts ptlen bytes into ct (ct may equal pt) and writes the tag; aad is authenticated, not
// encrypted. 0 ok, -1 failure.
int alt_aes_gcm_seal(const uint8_t key[ALT_KEY_LEN], const uint8_t iv[ALT_IV_LEN], const uint8_t *aad, size_t aadlen,
                     const uint8_t *pt, size_t ptlen, uint8_t *ct, uint8_t tag[ALT_TAG_LEN]);
// 0 ok (pt written), -1 when the tag does not verify or the backend fails. On failure pt is
// zeroed, so a rejected ciphertext never leaves partial plaintext behind.
int alt_aes_gcm_open(const uint8_t key[ALT_KEY_LEN], const uint8_t iv[ALT_IV_LEN], const uint8_t *aad, size_t aadlen,
                     const uint8_t *ct, size_t ctlen, const uint8_t tag[ALT_TAG_LEN], uint8_t *pt);
// Overwrites n bytes in a way the compiler may not drop (secrets: keys, tokens, plaintext).
void alt_wipe(void *p, size_t n);
