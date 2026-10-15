// The alt vault's master key: the desktop keyring (Linux: Secret Service through secret-tool;
// macOS: the login Keychain), else a 0600 file under <config>/alt (client/include/alt_keystore.h).
// The key only ever travels through pipes and the keychain APIs, never argv or a log line.
#include <alt_keystore.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_err(char *err, const size_t n, const char *msg) {
    if (err && n) snprintf(err, n, "%s", msg);
}

#ifdef KRUNKNATIVE_HAVE_TLS
#include <errno.h>
#include <fcntl.h>
#include <openssl/sha.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <Security/Security.h>
#endif

#define ALT_KEY_HEX (2 * ALT_KEY_LEN)
#define ALT_SECRET_TOOL_SERVICE "krunknative-alt"
#define ALT_KEYCHAIN_SERVICE "com.krunknative.alt"

// The keyring holds one item per user, so each config directory (each vault) gets its own: the item is
// named by a digest of the directory. Without it, a second vault would replace the first one's key.
static void dir_tag(const char *dir, char out[17]) {
    static const char digits[] = "0123456789abcdef";
    unsigned char md[SHA256_DIGEST_LENGTH];
    SHA256((const unsigned char *) dir, strlen(dir), md);
    for (int i = 0; i < 8; i++) {
        out[2 * i] = digits[md[i] >> 4];
        out[2 * i + 1] = digits[md[i] & 15];
    }
    out[16] = 0;
}

const char *alt_keystore_name(const int kind) {
    switch (kind) {
    case ALT_KS_FILE: return "file";
    case ALT_KS_SECRET_SERVICE: return "secret service";
    case ALT_KS_KEYCHAIN: return "keychain";
    default: return "none";
    }
}

int alt_keystore_default(void) {
    const char *force = getenv("KRUNKNATIVE_ALT_KEYSTORE");
    if (force && !strcmp(force, "file")) return ALT_KS_FILE;
    if (force && !strcmp(force, "secret")) return ALT_KS_SECRET_SERVICE;
    if (force && !strcmp(force, "keychain")) return ALT_KS_KEYCHAIN;
#if defined(__APPLE__)
    return ALT_KS_KEYCHAIN;
#elif defined(__linux__)
    return ALT_KS_SECRET_SERVICE; // falls back to the file key when secret-tool is missing or fails
#else
    return ALT_KS_FILE;
#endif
}

// ---------------------------------------------------------------- hex

static int hex_val(const char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int key_from_hex(const char *hex, const size_t n, uint8_t key[ALT_KEY_LEN]) {
    if (n != ALT_KEY_HEX) return -1;
    for (size_t i = 0; i < ALT_KEY_LEN; i++) {
        const int hi = hex_val(hex[2 * i]), lo = hex_val(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return -1;
        key[i] = (uint8_t) (hi * 16 + lo);
    }
    return 0;
}

static void key_to_hex(const uint8_t key[ALT_KEY_LEN], char out[ALT_KEY_HEX + 1]) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < ALT_KEY_LEN; i++) {
        out[2 * i] = digits[key[i] >> 4];
        out[2 * i + 1] = digits[key[i] & 15];
    }
    out[ALT_KEY_HEX] = 0;
}

// ---------------------------------------------------------------- file

static int key_path(const char *dir, char *out, const size_t n) {
    if (!dir || !*dir) return -1;
    return snprintf(out, n, "%s/alt/master.key", dir) > 0 ? 0 : -1;
}

static int ensure_alt_dir(const char *dir) {
    char path[600];
    snprintf(path, sizeof(path), "%s/alt", dir);
    if (mkdir(dir, 0700) != 0 && errno != EEXIST) return -1;
    if (mkdir(path, 0700) != 0 && errno != EEXIST) return -1;
    return 0;
}

static int file_store_key(const char *dir, const uint8_t key[ALT_KEY_LEN]) {
    char path[640], tmp[660];
    if (key_path(dir, path, sizeof(path)) != 0 || ensure_alt_dir(dir) != 0) return -1;
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    // created 0600 from the start: the key never sits in a world-readable file
    const int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    const ssize_t put = write(fd, key, ALT_KEY_LEN);
    const int closed = close(fd);
    if (put != (ssize_t) ALT_KEY_LEN || closed != 0 || rename(tmp, path) != 0) {
        remove(tmp);
        return -1;
    }
    return 0;
}

static int file_fetch_key(const char *dir, uint8_t key[ALT_KEY_LEN]) {
    char path[640];
    if (key_path(dir, path, sizeof(path)) != 0) return -1;
    const int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    uint8_t buf[ALT_KEY_LEN + 1];
    const ssize_t got = read(fd, buf, sizeof(buf)); // exactly 32 bytes, nothing after them
    close(fd);
    const int ok = got == (ssize_t) ALT_KEY_LEN ? 0 : -1;
    if (ok == 0) memcpy(key, buf, ALT_KEY_LEN);
    alt_wipe(buf, sizeof(buf));
    return ok;
}

// ---------------------------------------------------------------- Linux: Secret Service via secret-tool

#if defined(__linux__)
// secret-tool lookup prints the secret with no newline; anything else (a locked keyring, a missing
// tool, no such item) is a failure. The hex never reaches argv: it goes through the pipe only.
static int secret_lookup(const char *dir, uint8_t key[ALT_KEY_LEN]) {
    char tag[17], cmd[256];
    dir_tag(dir, tag); // hex only: nothing to quote
    snprintf(cmd, sizeof(cmd), "secret-tool lookup service %s account master vault %s 2>/dev/null", ALT_SECRET_TOOL_SERVICE, tag);
    FILE *p = popen(cmd, "r");
    if (!p) return -1;
    char hex[ALT_KEY_HEX + 4];
    const size_t n = fread(hex, 1, sizeof(hex) - 1, p);
    hex[n] = 0;
    const int status = pclose(p);
    size_t len = n;
    while (len && (hex[len - 1] == '\n' || hex[len - 1] == '\r')) hex[--len] = 0;
    const int ok = status == 0 && key_from_hex(hex, len, key) == 0 ? 0 : -1;
    alt_wipe(hex, sizeof(hex));
    return ok;
}

static int secret_store(const char *dir, const uint8_t key[ALT_KEY_LEN]) {
    // the item for this directory only (see dir_tag)
    char hex[ALT_KEY_HEX + 1], tag[17], cmd[300];
    key_to_hex(key, hex);
    dir_tag(dir, tag);
    snprintf(cmd, sizeof(cmd), "secret-tool store --label='KrunkNative alt vault' service %s account master vault %s 2>/dev/null",
             ALT_SECRET_TOOL_SERVICE, tag);
    FILE *p = popen(cmd, "w");
    int ok = -1;
    if (p) {
        const size_t put = fwrite(hex, 1, ALT_KEY_HEX, p);
        const int status = pclose(p);
        ok = put == ALT_KEY_HEX && status == 0 ? 0 : -1;
    }
    alt_wipe(hex, sizeof(hex));
    if (ok != 0) return -1;
    // the keyring is the only copy from here on: prove it reads back before the vault depends on it
    uint8_t back[ALT_KEY_LEN];
    const int same = secret_lookup(dir, back) == 0 && memcmp(back, key, ALT_KEY_LEN) == 0;
    alt_wipe(back, sizeof(back));
    return same ? 0 : -1;
}
#endif

// ---------------------------------------------------------------- macOS: Keychain

#if defined(__APPLE__)
// one generic-password item per config directory (account "master-<dir tag>", see dir_tag)
static CFMutableDictionaryRef keychain_query(const char *dir) {
    char tag[17], account_text[32];
    dir_tag(dir, tag);
    snprintf(account_text, sizeof(account_text), "master-%s", tag);
    CFMutableDictionaryRef q = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (!q) return NULL;
    CFStringRef service = CFStringCreateWithCString(NULL, ALT_KEYCHAIN_SERVICE, kCFStringEncodingUTF8);
    CFStringRef account = CFStringCreateWithCString(NULL, account_text, kCFStringEncodingUTF8);
    CFDictionarySetValue(q, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(q, kSecAttrService, service);
    CFDictionarySetValue(q, kSecAttrAccount, account);
    CFRelease(service);
    CFRelease(account);
    return q;
}

static int keychain_fetch(const char *dir, uint8_t key[ALT_KEY_LEN]) {
    CFMutableDictionaryRef q = keychain_query(dir);
    if (!q) return -1;
    CFDictionarySetValue(q, kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(q, kSecMatchLimit, kSecMatchLimitOne);
    CFTypeRef result = NULL;
    const OSStatus st = SecItemCopyMatching(q, &result);
    CFRelease(q);
    int ok = -1;
    if (st == errSecSuccess && result && CFGetTypeID(result) == CFDataGetTypeID() && CFDataGetLength((CFDataRef) result) == ALT_KEY_LEN) {
        memcpy(key, CFDataGetBytePtr((CFDataRef) result), ALT_KEY_LEN);
        ok = 0;
    }
    if (result) CFRelease(result);
    return ok;
}

static int keychain_store(const char *dir, const uint8_t key[ALT_KEY_LEN]) {
    CFMutableDictionaryRef q = keychain_query(dir);
    if (!q) return -1;
    SecItemDelete(q); // this directory's item only: replaced (called only when no vault exists here)
    CFDataRef data = CFDataCreate(NULL, key, ALT_KEY_LEN);
    if (!data) {
        CFRelease(q);
        return -1;
    }
    CFDictionarySetValue(q, kSecValueData, data);
    CFDictionarySetValue(q, kSecAttrAccessible, kSecAttrAccessibleWhenUnlockedThisDeviceOnly);
    const OSStatus st = SecItemAdd(q, NULL);
    CFRelease(data);
    CFRelease(q);
    return st == errSecSuccess ? 0 : -1;
}
#endif

// ---------------------------------------------------------------- public

static int store_key(const int kind, const char *dir, const uint8_t key[ALT_KEY_LEN]) {
    switch (kind) {
    case ALT_KS_FILE: return file_store_key(dir, key);
#if defined(__linux__)
    case ALT_KS_SECRET_SERVICE: return secret_store(dir, key);
#endif
#if defined(__APPLE__)
    case ALT_KS_KEYCHAIN: return keychain_store(dir, key);
#endif
    default: return -1;
    }
}

int alt_keystore_create(const char *config_dir, uint8_t key[ALT_KEY_LEN], int *kind, char *err, const size_t errn) {
    if (!config_dir || !*config_dir || !key || !kind) {
        set_err(err, errn, "no config directory");
        return -1;
    }
    if (alt_random_bytes(key, ALT_KEY_LEN) != 0) {
        set_err(err, errn, "no random bytes");
        return -1;
    }
    const int want = alt_keystore_default();
    const int forced = getenv("KRUNKNATIVE_ALT_KEYSTORE") != NULL;
    if (store_key(want, config_dir, key) == 0) {
        *kind = want;
        return 0;
    }
    if (!forced && want != ALT_KS_FILE && store_key(ALT_KS_FILE, config_dir, key) == 0) {
        // the platform keyring refused (no daemon, locked): the file keeps the vault usable
        *kind = ALT_KS_FILE;
        return 0;
    }
    alt_wipe(key, ALT_KEY_LEN);
    set_err(err, errn, "the keystore refused the vault key");
    return -1;
}

int alt_keystore_fetch(const char *config_dir, const int kind, uint8_t key[ALT_KEY_LEN], char *err, const size_t errn) {
    int ok = -1;
    switch (kind) {
    case ALT_KS_FILE: ok = file_fetch_key(config_dir, key); break;
#if defined(__linux__)
    case ALT_KS_SECRET_SERVICE: ok = secret_lookup(config_dir, key); break;
#endif
#if defined(__APPLE__)
    case ALT_KS_KEYCHAIN: ok = keychain_fetch(config_dir, key); break;
#endif
    default: break;
    }
    if (ok != 0) set_err(err, errn, "the keystore has no key for this vault (locked, or another machine's)");
    return ok;
}

void alt_keystore_forget(void) {} // the desktop stores keep no key in this process

#elif defined(__EMSCRIPTEN__)

// The web build: the master key is the browser's non-extractable CryptoKey (client/src/alt/alt_web.c).
// No key bytes are made or read here; the key argument is zeroed and unused.
#include <alt_web.h>

const char *alt_keystore_name(const int kind) { return kind == ALT_KS_WEB ? "browser" : "none"; }
int alt_keystore_default(void) { return ALT_KS_WEB; }

void alt_keystore_forget(void) { alt_web_key_forget(); }

int alt_keystore_create(const char *config_dir, uint8_t key[ALT_KEY_LEN], int *kind, char *err, const size_t errn) {
    (void) config_dir;
    if (!key || !kind) {
        set_err(err, errn, "no key buffer");
        return -1;
    }
    memset(key, 0, ALT_KEY_LEN);
    if (!alt_web_available()) {
        set_err(err, errn, "this browser has no SubtleCrypto or IndexedDB (the page must be https or localhost)");
        return -1;
    }
    if (alt_web_key_make() != 0) {
        set_err(err, errn, "the browser refused to make the vault key");
        return -1;
    }
    *kind = ALT_KS_WEB;
    return 0;
}

int alt_keystore_fetch(const char *config_dir, const int kind, uint8_t key[ALT_KEY_LEN], char *err, const size_t errn) {
    (void) config_dir;
    if (!key || kind != ALT_KS_WEB) {
        set_err(err, errn, "this vault was made by another keystore");
        return -1;
    }
    memset(key, 0, ALT_KEY_LEN);
    if (alt_web_key_load() != 0) {
        set_err(err, errn, "the browser has no key for this vault (its site data was cleared?)");
        return -1;
    }
    return 0;
}

#else // no crypto backend: nothing can be sealed, so no key is ever made or read

void alt_keystore_forget(void) {}

const char *alt_keystore_name(const int kind) { return kind == ALT_KS_FILE ? "file" : "none"; }
int alt_keystore_default(void) { return ALT_KS_NONE; }
int alt_keystore_create(const char *config_dir, uint8_t key[ALT_KEY_LEN], int *kind, char *err, const size_t errn) {
    (void) config_dir;
    (void) key;
    (void) kind;
    set_err(err, errn, "this build has no encryption (OpenSSL is not linked)");
    return -1;
}
int alt_keystore_fetch(const char *config_dir, const int kind, uint8_t key[ALT_KEY_LEN], char *err, const size_t errn) {
    (void) config_dir;
    (void) kind;
    (void) key;
    set_err(err, errn, "this build has no encryption (OpenSSL is not linked)");
    return -1;
}

#endif
