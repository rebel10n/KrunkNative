#pragma once
// Where the alt vault's master key lives (client/src/alt/alt_keystore.c). The vault header records
// which keystore sealed it (a plain byte: it says where to look, not a secret), so a vault is only
// ever opened from the store it was made with.
//
//   secret service  Linux: the desktop keyring (libsecret's Secret Service) through the system's
//                   secret-tool, when it is installed and works. No library is linked.
//   keychain        macOS: the login Keychain (Security.framework, generic password item).
//   file            desktop builds without a keyring: 32 random bytes in <config>/alt/master.key,
//                   mode 0600. Weaker than a keyring: anyone who can read the config directory can
//                   read the key. Reported as such in the manager.
//   browser         the web build (ALT_KS_WEB): a NON-EXTRACTABLE AES-256-GCM key in IndexedDB, used
//                   through SubtleCrypto (client/src/alt/alt_web.c). No key bytes exist in wasm.
//
// KRUNKNATIVE_ALT_KEYSTORE=file|secret|keychain forces one. Headless runs (KRUNKNATIVE_HEADLESS=1 or
// --headless: tests, agents, CI) default it to file (headless.c), so they never reach the desktop
// keyring, which a private XDG_CONFIG_HOME does not isolate. Without
// KRUNKNATIVE_HAVE_TLS (and off the web) every call fails: nothing is stored.
#include <alt_crypto.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    ALT_KS_NONE = 0,
    ALT_KS_FILE = 1,
    ALT_KS_SECRET_SERVICE = 2,
    ALT_KS_KEYCHAIN = 3,
    ALT_KS_WEB = 4,
} AltKeystoreKind;

const char *alt_keystore_name(int kind); // "file", "secret service", "keychain", "browser", "none"
// Drops the key bytes this process holds (the web's key reference; a no-op for the desktop stores).
void alt_keystore_forget(void);
// The keystore a new key goes to on this machine (env override first).
int alt_keystore_default(void);
// Creates a fresh random master key in the default keystore (replacing any key that keystore held
// for the alt vault: call only when no vault is sealed with it). 0 ok, -1 failure (err says why).
int alt_keystore_create(const char *config_dir, uint8_t key[ALT_KEY_LEN], int *kind, char *err, size_t errn);
// Reads the key of an existing vault from the keystore it was sealed with. 0 ok, -1 failure.
int alt_keystore_fetch(const char *config_dir, int kind, uint8_t key[ALT_KEY_LEN], char *err, size_t errn);
