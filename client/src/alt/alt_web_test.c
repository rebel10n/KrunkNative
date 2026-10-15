// The web vault's self-test (client/include/alt_web.h, alt_web_selftest): the vault on a throwaway
// IndexedDB database, so a real vault is never touched. Runs from the web build's ?kn_selftest=1 (web_dom.c).
// Emscripten only: the native build has its own test (client/tests/alt_vault_test.c).
#ifdef __EMSCRIPTEN__

#include <alt_store.h>
#include <alt_web.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_DB "krunknative-alt-selftest"
#define TEST_DIR "/kn-alt-selftest"              // the vault's directory: its record is TEST_DIR/alt/vault.bin
#define TEST_RECORD "/kn-alt-selftest/alt/vault.bin"

static int g_pass;
static int g_fail;

static void check(const int ok, const char *what) {
    if (ok) g_pass++;
    else g_fail++;
    printf("[kn-selftest] %s alt: %s\n", ok ? "PASS" : "FAIL", what);
}

static int contains(const uint8_t *hay, const size_t n, const char *needle) {
    const size_t m = strlen(needle);
    if (!m || m > n) return 0;
    for (size_t i = 0; i + m <= n; i++) {
        if (memcmp(hay + i, needle, m) == 0) return 1;
    }
    return 0;
}

// The stored record, as the browser holds it. The caller frees and wipes it.
static uint8_t *stored_record(size_t *n) {
    uint8_t *raw = NULL;
    if (alt_web_blob_get(TEST_RECORD, &raw, n) != 0) return NULL;
    return raw;
}

static void test_primitives(void) {
    check(alt_web_available() == 1, "SubtleCrypto and IndexedDB are present (a secure origin)");
    uint8_t a[32], b[32];
    const int ra = alt_web_random(a, sizeof(a)), rb = alt_web_random(b, sizeof(b));
    check(ra == 0 && rb == 0 && memcmp(a, b, sizeof(a)) != 0, "getRandomValues: two 32-byte draws, different");

    // the AES-GCM known answer from the NIST vectors is the native test's; here: a seal opens with its own
    // key and a flipped tag is refused (the key itself is never exposed, checked below)
    uint8_t iv[ALT_IV_LEN] = {0}, tag[ALT_TAG_LEN], ct[16], out[16], pt[16];
    memset(pt, 0x41, sizeof(pt));
    const uint8_t aad[8] = {'K', 'N', 'A', 'L', 'T', 'V', '0', '1'};
    check(alt_web_key_make() == 0, "a master key is made and stored");
    check(alt_web_key_loaded_for_test() == 1, "the master key is loaded");
    check(alt_web_seal(iv, aad, sizeof(aad), pt, sizeof(pt), ct, tag) == 0, "seal 16 bytes");
    check(memcmp(ct, pt, sizeof(pt)) != 0, "the ciphertext is not the plaintext");
    check(alt_web_open(iv, aad, sizeof(aad), ct, sizeof(ct), tag, out) == 0 && memcmp(out, pt, sizeof(pt)) == 0,
          "open returns the plaintext");
    uint8_t bad[ALT_TAG_LEN];
    memcpy(bad, tag, sizeof(bad));
    bad[15] ^= 0x80;
    memset(out, 0xAA, sizeof(out));
    check(alt_web_open(iv, aad, sizeof(aad), ct, sizeof(ct), bad, out) != 0, "a changed tag is refused");
    int zeroed = 1;
    for (size_t i = 0; i < sizeof(out); i++) zeroed = zeroed && out[i] == 0;
    check(zeroed, "a refused open leaves no plaintext");
    uint8_t other_aad[8];
    memcpy(other_aad, aad, sizeof(aad));
    other_aad[7] = '2';
    check(alt_web_open(iv, other_aad, sizeof(other_aad), ct, sizeof(ct), tag, out) != 0, "other additional data is refused");
    check(alt_web_key_exportable_for_test() == 0, "the master key cannot be exported (non-extractable)");
}

static void test_vault(void) {
    char name[64] = "", token[256] = "";
    check(alt_store_open(TEST_DIR) == 0, "open with no vault yet");
    check(alt_store_kn_save("alice", "tok-web-111") == 0, "save alice's session");
    check(!strcmp(alt_store_keystore(), "browser"), "the vault names the browser keystore");

    alt_store_close();
    check(alt_store_open(TEST_DIR) == 0, "reopen: the key comes back from IndexedDB, the record opens");
    check(alt_store_kn_session(name, sizeof(name), token, sizeof(token)) == 1 && !strcmp(name, "alice") &&
              !strcmp(token, "tok-web-111"),
          "the session survives a reopen");

    size_t n = 0;
    uint8_t *raw = stored_record(&n);
    check(raw && n > 41 && !contains(raw, n, "tok-web-111") && !contains(raw, n, "alice") && !contains(raw, n, "\"secret\""),
          "the stored record holds no token, name or JSON");
    if (raw) {
        alt_wipe(raw, n);
        free(raw);
    }

    // a different master key cannot open the record
    check(alt_web_key_make() == 0, "a second master key replaces the first");
    alt_store_close();
    check(alt_store_open(TEST_DIR) != 0 && !alt_store_ready(), "a wrong key is refused: the record stays unopened");

    // fresh vault under a fresh key; a flipped byte in the stored record is refused
    check(alt_web_drop_database() == 0, "the self-test database is dropped");
    check(alt_store_open(TEST_DIR) == 0 && alt_store_kn_save("bob", "tok-web-222") == 0, "a fresh vault for the tamper check");
    alt_store_close();
    raw = stored_record(&n);
    if (raw && n > 41) {
        raw[n - 3] ^= 0x10;
        check(alt_web_blob_put(TEST_RECORD, raw, n) == 0, "the tampered record is written back");
        alt_wipe(raw, n);
        free(raw);
    }
    check(alt_store_open(TEST_DIR) != 0 && !alt_store_ready(), "a tampered record is refused");
    alt_store_close();
}

int alt_web_selftest(void) {
    g_pass = 0;
    g_fail = 0;
    // a throwaway database for the whole run; the page's own vault is reopened afterwards
    alt_store_close();
    alt_web_use_database(TEST_DB);
    test_primitives();
    test_vault();
    alt_store_close();
    alt_web_drop_database();
    alt_web_use_database(NULL);
    alt_store_open(NULL); // the page's own vault, as at startup
    printf("[kn-selftest] alt: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}

#endif
