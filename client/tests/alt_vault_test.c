/*
 * alt_vault_test.c - the alt manager's vault (client/src/alt/):
 *   - AES-256-GCM known answers: NIST GCM test cases 2 and 14, and a vector with additional data
 *     generated with the Python "cryptography" package (AESGCM), not with this code
 *   - the sealed vault record: round trip, every single-byte change rejected, a wrong key rejected,
 *     truncation and extension rejected
 *   - the file: 0600, the magic, no token / label / credential bytes anywhere in it, atomic load / save
 *   - profile switching: the active KrunkNative account and the active krunker.io account, sign-out,
 *     removal, all of it surviving a save and reload
 *   - the file keystore (0600 key file, same key back) and the store: save, reopen, sign out, a
 *     missing key refused, a tampered file refused
 * The keystore is forced to "file" (KRUNKNATIVE_ALT_KEYSTORE): the keyring is never touched here.
 */
#include <alt_crypto.h>
#include <alt_keystore.h>
#include <alt_store.h>
#include <alt_vault.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// setenv for MinGW, whose CRT has only _putenv_s (the same guard as client/src/headless.c).
static int test_setenv(const char *name, const char *value, const int overwrite) {
#ifdef _WIN32
    if (!overwrite && getenv(name)) return 0;
    return _putenv_s(name, value);
#else
    return setenv(name, value, overwrite);
#endif
}

static int g_pass;
static int g_fail;

#define CHECK(cond, what)                                                                           \
    do {                                                                                            \
        if (cond) {                                                                                 \
            g_pass++;                                                                               \
        } else {                                                                                    \
            g_fail++;                                                                               \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, (what));                        \
        }                                                                                           \
    } while (0)

static int hexval(const char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// hex -> bytes; returns the byte count written (at most cap)
static size_t unhex(const char *hex, uint8_t *out, const size_t cap) {
    size_t n = 0;
    for (size_t i = 0; hex[2 * i] && hex[2 * i + 1] && n < cap; i++) {
        out[n++] = (uint8_t) (hexval(hex[2 * i]) * 16 + hexval(hex[2 * i + 1]));
    }
    return n;
}

static int bytes_equal_hex(const uint8_t *b, const size_t n, const char *hex) {
    uint8_t want[256];
    const size_t m = unhex(hex, want, sizeof(want));
    return m == n && memcmp(b, want, n) == 0;
}

static int contains(const uint8_t *hay, const size_t n, const char *needle) {
    const size_t m = strlen(needle);
    if (!m || m > n) return 0;
    for (size_t i = 0; i + m <= n; i++) {
        if (memcmp(hay + i, needle, m) == 0) return 1;
    }
    return 0;
}

static uint8_t *read_all(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(size > 0 ? (size_t) size : 1);
    const size_t got = buf && size > 0 ? fread(buf, 1, (size_t) size, f) : 0;
    fclose(f);
    if (!buf || got != (size_t) (size > 0 ? size : 0)) {
        free(buf);
        return NULL;
    }
    *n = got;
    return buf;
}

static int mode_is_0600(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && (st.st_mode & 0777) == 0600;
}

static void join(char *out, const size_t cap, const char *a, const char *b) {
    snprintf(out, cap, "%s/%s", a, b);
}

// ---------------------------------------------------------------- GCM

static void test_gcm_vectors(void) {
    static const uint8_t zero_key[32] = {0}, zero_iv[12] = {0}, zero_pt[16] = {0};
    uint8_t ct[16], tag[16], out[16];

    // NIST GCM test case 2: key 0, iv 0, 16 zero bytes
    CHECK(alt_aes_gcm_seal(zero_key, zero_iv, NULL, 0, zero_pt, 16, ct, tag) == 0, "seal TC2");
    CHECK(bytes_equal_hex(ct, 16, "cea7403d4d606b6e074ec5d3baf39d18"), "TC2 ciphertext");
    CHECK(bytes_equal_hex(tag, 16, "d0d1c8a799996bf0265b98b5d48ab919"), "TC2 tag");
    CHECK(alt_aes_gcm_open(zero_key, zero_iv, NULL, 0, ct, 16, tag, out) == 0, "open TC2");
    CHECK(memcmp(out, zero_pt, 16) == 0, "TC2 round trip");

    // NIST GCM test case 14 style: empty plaintext, key 0, iv 0
    CHECK(alt_aes_gcm_seal(zero_key, zero_iv, NULL, 0, NULL, 0, NULL, tag) == 0, "seal TC14");
    CHECK(bytes_equal_hex(tag, 16, "530f8afbc74536b9a963b4f1c4cb738b"), "TC14 tag");

    // with additional data: key 00..1f, iv 00..0b, aad "KNALTV01", plaintext "sk-token-ABC123"
    uint8_t key[32], iv[12], pt[15], out15[15], tag2[16], ct2[15];
    for (int i = 0; i < 32; i++) key[i] = (uint8_t) i;
    for (int i = 0; i < 12; i++) iv[i] = (uint8_t) i;
    memcpy(pt, "sk-token-ABC123", 15);
    const uint8_t aad[8] = {'K', 'N', 'A', 'L', 'T', 'V', '0', '1'};
    CHECK(alt_aes_gcm_seal(key, iv, aad, sizeof(aad), pt, sizeof(pt), ct2, tag2) == 0, "seal KAT");
    CHECK(bytes_equal_hex(ct2, sizeof(ct2), "3469fb6faa8ea775a000d5c880db4b"), "KAT ciphertext");
    CHECK(bytes_equal_hex(tag2, 16, "3b5e826fd19dc185bb8dbd61abc6ff8a"), "KAT tag");
    CHECK(alt_aes_gcm_open(key, iv, aad, sizeof(aad), ct2, sizeof(ct2), tag2, out15) == 0, "open KAT");
    CHECK(memcmp(out15, pt, sizeof(pt)) == 0, "KAT round trip");

    // the same ciphertext with other additional data, or a changed tag, is refused and leaves no plaintext
    const uint8_t other_aad[8] = {'K', 'N', 'A', 'L', 'T', 'V', '0', '2'};
    memset(out15, 0xAA, sizeof(out15));
    CHECK(alt_aes_gcm_open(key, iv, other_aad, sizeof(other_aad), ct2, sizeof(ct2), tag2, out15) != 0, "other AAD refused");
    int zeroed = 1;
    for (size_t i = 0; i < sizeof(out15); i++) zeroed = zeroed && out15[i] == 0;
    CHECK(zeroed, "a refused open wipes its plaintext");
    uint8_t bad_tag[16];
    memcpy(bad_tag, tag2, 16);
    bad_tag[15] ^= 0x80;
    CHECK(alt_aes_gcm_open(key, iv, aad, sizeof(aad), ct2, sizeof(ct2), bad_tag, out15) != 0, "changed tag refused");
}

// ---------------------------------------------------------------- record

static void fill_vault(AltVault *v) {
    char a[ALT_ID_LEN], b[ALT_ID_LEN], c[ALT_ID_LEN];
    alt_vault_init(v);
    v->keystore = ALT_KS_FILE;
    CHECK(alt_vault_put(v, ALT_KIND_KN, "alice", "tok-alice-111", 1000, a) >= 0, "put alice");
    CHECK(alt_vault_put(v, ALT_KIND_KN, "bob", "tok-bob-222", 2000, b) >= 0, "put bob");
    CHECK(alt_vault_put(v, ALT_KIND_KRUNKER, "main krunker", "cookie=zzz-333", 3000, c) >= 0, "put krunker");
    CHECK(alt_vault_switch(v, a) == 0, "switch alice");
    CHECK(alt_vault_switch(v, c) == 0, "switch krunker");
}

static void test_record(void) {
    AltVault v, w;
    fill_vault(&v);
    uint8_t key[ALT_KEY_LEN];
    CHECK(alt_random_bytes(key, sizeof(key)) == 0, "random key");

    uint8_t *rec = NULL;
    size_t n = 0;
    CHECK(alt_vault_seal(&v, key, &rec, &n) == 0 && rec && n > ALT_HEADER_LEN, "seal the vault");
    if (!rec) return;
    CHECK(memcmp(rec, "KNALTV01", 8) == 0, "magic");
    CHECK(alt_vault_peek_keystore(rec, n) == ALT_KS_FILE, "keystore kind in the header");

    char err[160] = "";
    CHECK(alt_vault_open_bytes(&w, rec, n, key, err, sizeof(err)) == 0, "open the sealed vault");
    CHECK(w.count == 3, "three profiles back");
    CHECK(w.count == 3 && !strcmp(w.profiles[0].label, "alice") && !strcmp(w.profiles[0].secret, "tok-alice-111"), "alice's profile");
    CHECK(w.count == 3 && w.profiles[2].kind == ALT_KIND_KRUNKER && !strcmp(w.profiles[2].secret, "cookie=zzz-333"), "krunker profile");
    CHECK(w.count == 3 && w.profiles[1].added_ms == 2000, "added time kept");
    CHECK(!strcmp(w.active_kn, v.active_kn) && !strcmp(w.active_krunker, v.active_krunker), "active pointers kept");
    CHECK(w.keystore == ALT_KS_FILE, "keystore kept");
    alt_vault_wipe(&w);

    // every single byte of the record, flipped: refused (the header is additional data, the rest is ciphertext or tag)
    int accepted = 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t *copy = malloc(n);
        memcpy(copy, rec, n);
        copy[i] ^= 0x01;
        if (alt_vault_open_bytes(&w, copy, n, key, NULL, 0) == 0) {
            accepted++;
            fprintf(stderr, "  flipped byte %zu was accepted\n", i);
        }
        alt_vault_wipe(&w);
        alt_wipe(copy, n);
        free(copy);
    }
    CHECK(accepted == 0, "a flipped byte always fails to open");

    // a wrong key fails the same way
    uint8_t wrong[ALT_KEY_LEN];
    memcpy(wrong, key, sizeof(wrong));
    wrong[0] ^= 0x01;
    CHECK(alt_vault_open_bytes(&w, rec, n, wrong, err, sizeof(err)) != 0, "a wrong key is refused");
    CHECK(strstr(err, "wrong") != NULL, "the error says the key is wrong or the vault changed");
    CHECK(w.count == 0, "a refused vault holds nothing");

    // truncated and extended records
    CHECK(alt_vault_open_bytes(&w, rec, n - 1, key, NULL, 0) != 0, "truncated record refused");
    uint8_t *longer = malloc(n + 1);
    memcpy(longer, rec, n);
    longer[n] = 0;
    CHECK(alt_vault_open_bytes(&w, longer, n + 1, key, NULL, 0) != 0, "extended record refused");
    free(longer);
    CHECK(alt_vault_open_bytes(&w, rec, 8, key, NULL, 0) != 0, "a stub is not a vault");

    alt_wipe(rec, n);
    free(rec);
}

// ---------------------------------------------------------------- file

static void test_file(const char *dir) {
    char path[600], tmp[620];
    join(path, sizeof(path), dir, "vault-file.bin");
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    remove(path);

    AltVault v, w;
    fill_vault(&v);
    uint8_t key[ALT_KEY_LEN];
    CHECK(alt_random_bytes(key, sizeof(key)) == 0, "random key (file)");

    CHECK(alt_vault_load_file(&w, path, key, NULL, 0) == 1, "no file yet is reported as absent");
    CHECK(alt_vault_peek_file(path) == 0, "peek: absent");
    CHECK(alt_vault_save_file(&v, path, key) == 0, "save the vault file");
    CHECK(mode_is_0600(path), "vault file is mode 0600");
    CHECK(alt_vault_peek_file(path) == ALT_KS_FILE, "peek: the file keystore");

    size_t n = 0;
    uint8_t *raw = read_all(path, &n);
    CHECK(raw != NULL && n > ALT_HEADER_LEN, "read the vault file back");
    if (raw) {
        // no token, label, credential or the JSON around them: the file is ciphertext only
        CHECK(!contains(raw, n, "tok-alice-111"), "no token bytes on disk");
        CHECK(!contains(raw, n, "tok-bob-222"), "no second token on disk");
        CHECK(!contains(raw, n, "cookie=zzz-333"), "no krunker.io credential on disk");
        CHECK(!contains(raw, n, "alice"), "no account name on disk");
        CHECK(!contains(raw, n, "\"secret\""), "no JSON field names on disk");
        free(raw);
    }

    CHECK(alt_vault_load_file(&w, path, key, NULL, 0) == 0, "load the vault file");
    CHECK(w.count == 3 && !strcmp(w.profiles[0].secret, "tok-alice-111"), "loaded secret matches");
    alt_vault_wipe(&w);

    uint8_t wrong[ALT_KEY_LEN];
    memcpy(wrong, key, sizeof(wrong));
    wrong[31] ^= 0x40;
    char err[160] = "";
    CHECK(alt_vault_load_file(&w, path, wrong, err, sizeof(err)) != 0, "load with the wrong key fails");
    CHECK(w.count == 0, "a failed load holds nothing");

    // a second save replaces the first atomically and leaves no temp file
    CHECK(alt_vault_switch(&v, v.profiles[1].id) == 0, "switch bob");
    CHECK(alt_vault_save_file(&v, path, key) == 0, "save again");
    CHECK(access(tmp, F_OK) != 0, "no temp file left behind");
    CHECK(alt_vault_load_file(&w, path, key, NULL, 0) == 0 && !strcmp(w.active_kn, v.profiles[1].id), "the second save is what loads");
    alt_vault_wipe(&w);

    // an unreadable path is an error, never "absent" (a save there would replace something we cannot see)
    char dirpath[600];
    join(dirpath, sizeof(dirpath), dir, "not-a-file-dir");
    mkdir(dirpath, 0700);
    CHECK(alt_vault_peek_file(dirpath) < 0, "a directory is not a vault: error");
    rmdir(dirpath);

    alt_vault_wipe(&v);
    alt_wipe(key, sizeof(key));
    remove(path);
}

// ---------------------------------------------------------------- switching

static void test_switching(void) {
    AltVault v;
    alt_vault_init(&v);
    char alice[ALT_ID_LEN], bob[ALT_ID_LEN], main[ALT_ID_LEN];
    CHECK(alt_vault_put(&v, ALT_KIND_KN, "alice", "t1", 1, alice) >= 0, "put alice");
    CHECK(alt_vault_put(&v, ALT_KIND_KN, "bob", "t2", 2, bob) >= 0, "put bob");
    CHECK(alt_vault_put(&v, ALT_KIND_KRUNKER, "main", "c1", 3, main) >= 0, "put main");
    CHECK(alt_vault_active(&v, ALT_KIND_KN) == NULL, "no active KrunkNative account at first");

    CHECK(alt_vault_switch(&v, alice) == 0, "switch to alice");
    const AltProfile *active = alt_vault_active(&v, ALT_KIND_KN);
    CHECK(active && !strcmp(active->label, "alice"), "alice is the active account");
    CHECK(alt_vault_switch(&v, bob) == 0, "switch to bob");
    active = alt_vault_active(&v, ALT_KIND_KN);
    CHECK(active && !strcmp(active->label, "bob"), "the switch changed the active account");
    CHECK(alt_vault_switch(&v, "p0000000000000000") != 0, "switching to an unknown id fails");
    active = alt_vault_active(&v, ALT_KIND_KN);
    CHECK(active && !strcmp(active->label, "bob"), "a failed switch changes nothing");

    // the krunker.io choice is separate from the KrunkNative one
    CHECK(alt_vault_switch(&v, main) == 0, "choose the krunker.io account");
    CHECK(alt_vault_active(&v, ALT_KIND_KRUNKER) != NULL, "krunker.io account active");
    active = alt_vault_active(&v, ALT_KIND_KN);
    CHECK(active && !strcmp(active->label, "bob"), "choosing krunker.io leaves the KrunkNative account alone");

    alt_vault_sign_out(&v);
    CHECK(alt_vault_active(&v, ALT_KIND_KN) == NULL, "signed out");
    CHECK(v.count == 3, "signing out keeps the saved profiles");
    CHECK(alt_vault_active(&v, ALT_KIND_KRUNKER) != NULL, "signing out leaves the krunker.io choice");

    // the same account signed in again replaces its secret, not a second profile
    char again[ALT_ID_LEN];
    CHECK(alt_vault_put(&v, ALT_KIND_KN, "bob", "t2-new", 9, again) >= 0, "bob signs in again");
    CHECK(!strcmp(again, bob) && v.count == 3, "same account, same profile");
    CHECK(!strcmp(v.profiles[alt_vault_find(&v, bob)].secret, "t2-new"), "its secret is the new one");

    // a removed active profile clears its pointer; the rest stay
    CHECK(alt_vault_switch(&v, bob) == 0, "bob active again");
    CHECK(alt_vault_remove(&v, bob) == 0, "remove bob");
    CHECK(alt_vault_active(&v, ALT_KIND_KN) == NULL, "removing the active account clears it");
    CHECK(alt_vault_find(&v, bob) < 0 && v.count == 2, "removed profile is gone");
    CHECK(alt_vault_remove(&v, bob) != 0, "removing twice fails");
    CHECK(alt_vault_find(&v, alice) >= 0, "other profiles stay");

    // the choice survives a save and a reload
    AltVault w;
    uint8_t key[ALT_KEY_LEN] = {7};
    v.keystore = ALT_KS_FILE;
    CHECK(alt_vault_switch(&v, alice) == 0, "alice active");
    uint8_t *rec = NULL;
    size_t n = 0;
    CHECK(alt_vault_seal(&v, key, &rec, &n) == 0, "seal after switching");
    CHECK(alt_vault_open_bytes(&w, rec, n, key, NULL, 0) == 0, "open after switching");
    const AltProfile *kept = alt_vault_active(&w, ALT_KIND_KN);
    CHECK(kept && !strcmp(kept->label, "alice"), "the active account survives a reload");
    CHECK(alt_vault_active(&w, ALT_KIND_KRUNKER) != NULL, "the krunker.io choice survives a reload");
    alt_vault_wipe(&w);
    alt_wipe(rec, n);
    free(rec);
    alt_vault_wipe(&v);
}

// ---------------------------------------------------------------- keystore and store

static void remove_store_files(const char *dir) {
    char p[700];
    join(p, sizeof(p), dir, "alt/vault.bin");
    remove(p);
    join(p, sizeof(p), dir, "alt/vault.bin.tmp");
    remove(p);
    join(p, sizeof(p), dir, "alt/master.key");
    remove(p);
    join(p, sizeof(p), dir, "alt/master.key.tmp");
    remove(p);
    join(p, sizeof(p), dir, "alt");
    rmdir(p);
}

static void test_keystore(const char *dir) {
    uint8_t k1[ALT_KEY_LEN], k2[ALT_KEY_LEN];
    int kind = ALT_KS_NONE;
    char err[160] = "";
    remove_store_files(dir);
    CHECK(alt_keystore_create(dir, k1, &kind, err, sizeof(err)) == 0, "create a key (file keystore)");
    CHECK(kind == ALT_KS_FILE, "forced file keystore is used");
    CHECK(strcmp(alt_keystore_name(kind), "file") == 0, "keystore name");
    char keypath[700];
    join(keypath, sizeof(keypath), dir, "alt/master.key");
    CHECK(mode_is_0600(keypath), "the key file is mode 0600");
    size_t n = 0;
    uint8_t *raw = read_all(keypath, &n);
    CHECK(raw && n == ALT_KEY_LEN, "the key file holds exactly the 32-byte key");
    free(raw);
    CHECK(alt_keystore_fetch(dir, ALT_KS_FILE, k2, err, sizeof(err)) == 0, "fetch the key back");
    CHECK(memcmp(k1, k2, ALT_KEY_LEN) == 0, "the same key comes back");
    remove(keypath);
    CHECK(alt_keystore_fetch(dir, ALT_KS_FILE, k2, err, sizeof(err)) != 0, "a missing key is refused");
    remove_store_files(dir);
}

static void test_store(const char *dir) {
    char name[64] = "", token[256] = "";
    char vault_path[700];
    join(vault_path, sizeof(vault_path), dir, "alt/vault.bin");
    remove_store_files(dir);
    alt_store_close();

    CHECK(alt_store_open(dir) == 0, "open with no vault yet");
    CHECK(alt_store_ready(), "ready without a vault file");
    CHECK(alt_store_kn_session(name, sizeof(name), token, sizeof(token)) == 0, "no session at first");
    CHECK(alt_store_kn_sign_out() == 0, "signing out with nothing saved is fine");
    CHECK(access(vault_path, F_OK) != 0, "signing out with nothing saved writes nothing");

    CHECK(alt_store_kn_save("alice", "tokA-store-444") == 0, "save alice's session");
    CHECK(alt_store_kn_session(name, sizeof(name), token, sizeof(token)) == 1, "session is active");
    CHECK(!strcmp(name, "alice") && !strcmp(token, "tokA-store-444"), "the active session is alice's");
    CHECK(strcmp(alt_store_keystore(), "file") == 0, "the vault names its keystore");
    CHECK(mode_is_0600(vault_path), "the vault file is mode 0600");
    alt_store_close();

    // reopen: the session comes back from the sealed file, with the key from the keystore
    CHECK(alt_store_open(dir) == 0, "reopen the vault");
    memset(token, 0, sizeof(token));
    CHECK(alt_store_kn_session(name, sizeof(name), token, sizeof(token)) == 1, "session survives a restart");
    CHECK(!strcmp(token, "tokA-store-444"), "the restored token matches");
    size_t n = 0;
    uint8_t *raw = read_all(vault_path, &n);
    CHECK(raw && !contains(raw, n, "tokA-store-444") && !contains(raw, n, "alice"), "no token or name in the vault file");
    free(raw);

    CHECK(alt_store_kn_sign_out() == 0, "sign out");
    CHECK(alt_store_kn_session(name, sizeof(name), token, sizeof(token)) == 0, "signed out");
    alt_store_close();
    CHECK(alt_store_open(dir) == 0, "reopen after signing out");
    CHECK(alt_store_kn_session(name, sizeof(name), token, sizeof(token)) == 0, "still signed out after a restart");
    CHECK(alt_store_vault() && alt_store_vault()->count == 1, "the profile is kept after signing out");

    // a listed account whose session is forgotten stays listed, signed out and without its token, and
    // the token is not resumed after a restart (the Account Manager asks for the password then)
    CHECK(alt_store_kn_listed("alice") == 1 && alt_store_kn_listed("bob") == 0, "listed: alice yes, bob no");
    CHECK(alt_store_kn_save("alice", "tokA-store-444") == 0, "alice's session stored again");
    CHECK(alt_store_kn_forget("alice") == 0, "forget alice's session");
    CHECK(alt_store_kn_session(name, sizeof(name), token, sizeof(token)) == 0, "a forgotten session is not resumed");
    CHECK(alt_store_kn_listed("alice") == 1, "a forgotten account stays listed");
    alt_store_close();
    CHECK(alt_store_open(dir) == 0, "reopen after forgetting");
    CHECK(alt_store_kn_session(name, sizeof(name), token, sizeof(token)) == 0, "still nothing resumed after a restart");
    CHECK(alt_store_vault() && alt_store_vault()->count == 0, "a restart drops the forgotten account (no session, no password)");
    uint8_t *raw2 = read_all(vault_path, &n);
    CHECK(raw2 && !contains(raw2, n, "tokA-store-444"), "no token in the vault file after forgetting");
    free(raw2);
    CHECK(alt_store_kn_forget("nobody") == 0, "forgetting an unlisted name is fine");

    // a vault whose key is gone is refused, and nothing is replaced
    char keypath[700];
    join(keypath, sizeof(keypath), dir, "alt/master.key");
    remove(keypath);
    alt_store_close();
    CHECK(alt_store_open(dir) != 0, "a vault without its key is refused");
    CHECK(!alt_store_ready(), "not ready without the key");
    CHECK(access(vault_path, F_OK) == 0, "the refused vault is still there");

    // a tampered vault file is refused
    alt_store_close();
    remove_store_files(dir);
    CHECK(alt_store_open(dir) == 0 && alt_store_kn_save("bob", "tokB") == 0, "a fresh vault for the tamper check");
    alt_store_close();
    raw = read_all(vault_path, &n);
    if (raw && n > ALT_HEADER_LEN) {
        raw[n - 3] ^= 0x10;
        FILE *f = fopen(vault_path, "wb");
        if (f) {
            fwrite(raw, 1, n, f);
            fclose(f);
        }
    }
    free(raw);
    CHECK(alt_store_open(dir) != 0, "a tampered vault file is refused");
    alt_store_close();
    remove_store_files(dir);
}

// The Account Manager's Add lists an account without switching to it, keeps a stored password sealed next to the
// token, a session forgotten keeps the password, and an entry with neither is dropped when the vault opens.
static void test_passwords(const char *dir) {
    char name[64] = "", token[256] = "", pass[256] = "";
    char vault_path[700];
    join(vault_path, sizeof(vault_path), dir, "alt/vault.bin");
    remove_store_files(dir);
    alt_store_close();
    CHECK(alt_store_open(dir) == 0, "open for the password checks");

    CHECK(alt_store_kn_save("carol", "tokC-pass-555") == 0, "carol is signed in (the current account)");
    CHECK(alt_store_list(ALT_KIND_KN, "dave", "tokD-pass-666", "pwD-pass-777", NULL) == 0, "Add lists dave with a token and a password");
    CHECK(alt_store_kn_session(name, sizeof(name), token, sizeof(token)) == 1 && !strcmp(name, "carol"),
          "the Add did not switch: carol is still the active account");
    CHECK(alt_store_kn_listed("dave") == 1, "dave is listed");
    CHECK(alt_store_password(ALT_KIND_KN, "dave", pass, sizeof(pass)) == 1 && !strcmp(pass, "pwD-pass-777"), "dave's password is kept");
    CHECK(alt_store_password(ALT_KIND_KN, "carol", pass, sizeof(pass)) == 0 && !pass[0], "carol has no password (none given)");

    CHECK(alt_store_list(ALT_KIND_KN, "erin", NULL, "pwE-pass-888", NULL) == 0, "Add with a password only lists erin");
    CHECK(alt_store_kn_session(name, sizeof(name), token, sizeof(token)) == 1 && !strcmp(name, "carol"), "still carol active");
    CHECK(alt_store_password(ALT_KIND_KN, "erin", pass, sizeof(pass)) == 1 && !strcmp(pass, "pwE-pass-888"), "erin's password is kept");
    CHECK(alt_store_list(ALT_KIND_KN, "dave", "", "", NULL) == 0, "an Add with nothing new keeps dave's token and password");
    CHECK(alt_store_password(ALT_KIND_KN, "dave", pass, sizeof(pass)) == 1 && !strcmp(pass, "pwD-pass-777"), "dave's password still there");

    // the password is sealed: no file under the store holds it
    size_t n = 0;
    uint8_t *raw = read_all(vault_path, &n);
    CHECK(raw && !contains(raw, n, "pwD-pass-777") && !contains(raw, n, "pwE-pass-888"), "no password on the vault file");
    free(raw);

    // a refused session is forgotten, the password stays and signs in next
    CHECK(alt_store_kn_forget("dave") == 0, "dave's session is forgotten");
    CHECK(alt_store_password(ALT_KIND_KN, "dave", pass, sizeof(pass)) == 1, "the password outlives the session");
    CHECK(alt_store_kn_listed("dave") == 1, "dave stays listed with its password");
    // dave's password is cleared as well: listed with neither a session nor a password (the restart drops it)
    AltVault *av = alt_store_vault();
    for (int i = 0; av && i < av->count; i++) {
        if (!strcmp(av->profiles[i].label, "dave")) alt_vault_set_password(av, av->profiles[i].id, "");
    }
    CHECK(alt_store_save() == 0 && alt_store_password(ALT_KIND_KN, "dave", pass, sizeof(pass)) == 0, "dave's password is cleared");

    // a restart drops what cannot sign in: dave (neither) goes; erin (a password) and carol (a session) stay
    alt_store_close();
    CHECK(alt_store_open(dir) == 0, "reopen the vault");
    CHECK(alt_store_kn_listed("dave") == 0, "a restart drops the entry with neither a session nor a password");
    CHECK(alt_store_kn_listed("erin") == 1 && alt_store_kn_listed("carol") == 1, "erin (a password) and carol (a session) stay");
    CHECK(alt_store_kn_session(name, sizeof(name), token, sizeof(token)) == 1, "carol's session survives");
    alt_store_close();
    remove_store_files(dir);
}

int main(void) {
    test_setenv("KRUNKNATIVE_ALT_KEYSTORE", "file", 1);
    const char *tmp = getenv("TMPDIR");
    char templ[512];
    snprintf(templ, sizeof(templ), "%s/alt-vault-test.XXXXXX", tmp && *tmp ? tmp : "/tmp");
    if (!mkdtemp(templ)) {
        fprintf(stderr, "alt_vault_test: cannot make a temp dir (%s)\n", strerror(errno));
        return 1;
    }
    char alt[600];
    join(alt, sizeof(alt), templ, "alt");
    mkdir(alt, 0700);

    test_gcm_vectors();
    test_record();
    test_file(alt);
    test_switching();
    test_keystore(templ);
    test_store(templ);
    test_passwords(templ);

    rmdir(alt);
    rmdir(templ);
    printf("alt_vault_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
