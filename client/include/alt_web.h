#pragma once
// The web vault's browser half (client/src/alt/alt_web.c, Emscripten only). Sealing is SubtleCrypto
// AES-256-GCM under a NON-EXTRACTABLE master key kept in IndexedDB: the page can use the key but can
// never read its bytes, so no raw key ever enters wasm memory. The sealed vault record is one IndexedDB
// value (one put is atomic, so no temp file is needed).
//
// The browser calls are asynchronous. The web build links -sASYNCIFY, so each one suspends the calling
// C code until the browser answers and the call reads like the native one. Only one such call may be in
// flight: a second one returns -1 at once (busy), it never waits. Keys are passed as NULL-free buffers
// and the key argument of alt_aes_gcm_* is not used on the web (the master key is the page's).
#include <alt_crypto.h>

#include <stddef.h>
#include <stdint.h>

// 1 when crypto.subtle and IndexedDB exist (a secure origin: https or localhost). Synchronous.
int alt_web_available(void);
// CSPRNG bytes (getRandomValues). 0 ok, -1 failure.
int alt_web_random(void *out, size_t n);
// A fresh non-extractable AES-256-GCM master key, stored in IndexedDB and loaded. 0 ok, -1 failure.
int alt_web_key_make(void);
// Loads the stored master key. 0 ok, -1 when there is none or it cannot be read.
int alt_web_key_load(void);
// Drops the page's reference to the master key (the stored copy stays).
void alt_web_key_forget(void);
// Seals / opens with the master key. Same contract as alt_aes_gcm_seal / alt_aes_gcm_open.
int alt_web_seal(const uint8_t iv[ALT_IV_LEN], const uint8_t *aad, size_t aadlen, const uint8_t *pt, size_t ptlen,
                 uint8_t *ct, uint8_t tag[ALT_TAG_LEN]);
int alt_web_open(const uint8_t iv[ALT_IV_LEN], const uint8_t *aad, size_t aadlen, const uint8_t *ct, size_t ctlen,
                 const uint8_t tag[ALT_TAG_LEN], uint8_t *pt);
// A named record in IndexedDB (the sealed vault). get: 0 ok (*out is malloc'd, the caller frees and
// wipes it), 1 absent, -1 error. put: 0 ok, -1 error.
int alt_web_blob_get(const char *name, uint8_t **out, size_t *outlen);
int alt_web_blob_put(const char *name, const uint8_t *buf, size_t n);

// The IndexedDB database the vault uses. NULL = the vault's own ("krunknative-alt"); the self-test
// switches to a throwaway one and drops it afterwards, so it never touches a real vault.
int alt_web_use_database(const char *name);
int alt_web_drop_database(void);
// Self-test probes: 1 when the master key can be exported (must be 0), and whether a key is loaded.
int alt_web_key_exportable_for_test(void);
int alt_web_key_loaded_for_test(void);

// Self-test (the web build's ?kn_selftest=1, client/src/alt/alt_web_test.c): runs the vault on a
// throwaway database. Prints one [kn-selftest] PASS/FAIL line per check, returns the failure count.
int alt_web_selftest(void);
