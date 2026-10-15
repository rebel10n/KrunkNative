// AES-256-GCM and the CSPRNG for the alt manager (client/include/alt_crypto.h). Built with the
// client's OpenSSL (KRUNKNATIVE_HAVE_TLS, the same libcrypto as the TLS code); every other build
// (Windows, the web, a build without OpenSSL) gets the stubs at the bottom and reports
// alt_crypto_available() == 0, so no vault is ever written without an authenticated cipher.
#include <alt_crypto.h>

#include <string.h>

#ifdef KRUNKNATIVE_HAVE_TLS
#include <limits.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#endif

void alt_wipe(void *p, size_t n) {
#ifdef KRUNKNATIVE_HAVE_TLS
    if (p && n) OPENSSL_cleanse(p, n);
#else
    volatile unsigned char *v = (volatile unsigned char *) p;
    while (p && n--) *v++ = 0;
#endif
}

#ifdef KRUNKNATIVE_HAVE_TLS

int alt_crypto_available(void) { return 1; }

int alt_random_bytes(void *out, size_t n) {
    if (!out || !n) return n ? -1 : 0;
    if (n > INT_MAX) return -1;
    return RAND_bytes((unsigned char *) out, (int) n) == 1 ? 0 : -1;
}

int alt_aes_gcm_seal(const uint8_t key[ALT_KEY_LEN], const uint8_t iv[ALT_IV_LEN], const uint8_t *aad, size_t aadlen,
                     const uint8_t *pt, size_t ptlen, uint8_t *ct, uint8_t tag[ALT_TAG_LEN]) {
    if (!key || !iv || !tag || (ptlen && (!pt || !ct))) return -1;
    if (aadlen > INT_MAX || ptlen > INT_MAX) return -1;
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return -1;
    int ok = -1, len = 0, fin = 0;
    unsigned char dummy[16];
    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1) goto done;
    if (EVP_EncryptInit_ex(ctx, NULL, NULL, key, iv) != 1) goto done;
    if (aadlen && EVP_EncryptUpdate(ctx, NULL, &len, aad, (int) aadlen) != 1) goto done;
    if (ptlen && EVP_EncryptUpdate(ctx, ct, &len, pt, (int) ptlen) != 1) goto done;
    if (EVP_EncryptFinal_ex(ctx, dummy, &fin) != 1) goto done; // GCM: no output, the tag only
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, ALT_TAG_LEN, tag) != 1) goto done;
    ok = 0;
done:
    alt_wipe(dummy, sizeof(dummy));
    EVP_CIPHER_CTX_free(ctx);
    return ok;
}

int alt_aes_gcm_open(const uint8_t key[ALT_KEY_LEN], const uint8_t iv[ALT_IV_LEN], const uint8_t *aad, size_t aadlen,
                     const uint8_t *ct, size_t ctlen, const uint8_t tag[ALT_TAG_LEN], uint8_t *pt) {
    if (!key || !iv || !tag || (ctlen && (!ct || !pt))) return -1;
    if (aadlen > INT_MAX || ctlen > INT_MAX) return -1;
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return -1;
    int ok = -1, len = 0, fin = 0;
    unsigned char tagbuf[ALT_TAG_LEN];
    memcpy(tagbuf, tag, sizeof(tagbuf)); // EVP wants a writable tag pointer on some providers
    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1) goto done;
    if (EVP_DecryptInit_ex(ctx, NULL, NULL, key, iv) != 1) goto done;
    if (aadlen && EVP_DecryptUpdate(ctx, NULL, &len, aad, (int) aadlen) != 1) goto done;
    if (ctlen && EVP_DecryptUpdate(ctx, pt, &len, ct, (int) ctlen) != 1) goto done;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, ALT_TAG_LEN, tagbuf) != 1) goto done;
    // a failed tag check makes this <= 0: the plaintext produced so far is not trusted
    if (EVP_DecryptFinal_ex(ctx, NULL, &fin) <= 0) goto done;
    ok = 0;
done:
    alt_wipe(tagbuf, sizeof(tagbuf));
    if (ok != 0 && pt && ctlen) alt_wipe(pt, ctlen);
    EVP_CIPHER_CTX_free(ctx);
    return ok;
}

#elif defined(__EMSCRIPTEN__)

// The web build: SubtleCrypto under the page's non-extractable master key (client/src/alt/alt_web.c).
// The key argument is not used here: the vault is sealed with the key the browser holds, and no key
// bytes exist in wasm memory to pass in.
#include <alt_web.h>

int alt_crypto_available(void) { return alt_web_available(); }

int alt_random_bytes(void *out, size_t n) { return alt_web_random(out, n); }

int alt_aes_gcm_seal(const uint8_t key[ALT_KEY_LEN], const uint8_t iv[ALT_IV_LEN], const uint8_t *aad, size_t aadlen,
                     const uint8_t *pt, size_t ptlen, uint8_t *ct, uint8_t tag[ALT_TAG_LEN]) {
    (void) key;
    return alt_web_seal(iv, aad, aadlen, pt, ptlen, ct, tag);
}

int alt_aes_gcm_open(const uint8_t key[ALT_KEY_LEN], const uint8_t iv[ALT_IV_LEN], const uint8_t *aad, size_t aadlen,
                     const uint8_t *ct, size_t ctlen, const uint8_t tag[ALT_TAG_LEN], uint8_t *pt) {
    (void) key;
    return alt_web_open(iv, aad, aadlen, ct, ctlen, tag, pt);
}

#else // no OpenSSL in this build: nothing here may store or seal anything

int alt_crypto_available(void) { return 0; }
int alt_random_bytes(void *out, size_t n) {
    (void) out;
    (void) n;
    return -1;
}
int alt_aes_gcm_seal(const uint8_t key[ALT_KEY_LEN], const uint8_t iv[ALT_IV_LEN], const uint8_t *aad, size_t aadlen,
                     const uint8_t *pt, size_t ptlen, uint8_t *ct, uint8_t tag[ALT_TAG_LEN]) {
    (void) key;
    (void) iv;
    (void) aad;
    (void) aadlen;
    (void) pt;
    (void) ptlen;
    (void) ct;
    (void) tag;
    return -1;
}
int alt_aes_gcm_open(const uint8_t key[ALT_KEY_LEN], const uint8_t iv[ALT_IV_LEN], const uint8_t *aad, size_t aadlen,
                     const uint8_t *ct, size_t ctlen, const uint8_t tag[ALT_TAG_LEN], uint8_t *pt) {
    (void) key;
    (void) iv;
    (void) aad;
    (void) aadlen;
    (void) ct;
    (void) ctlen;
    (void) tag;
    (void) pt;
    return -1;
}

#endif
