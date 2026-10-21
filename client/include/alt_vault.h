#pragma once
// The alt manager's vault (client/src/alt/alt_vault.c): the user's own accounts, KrunkNative sign-ins
// and krunker.io sessions, kept as profiles the player can switch between. The vault is one file,
// sealed with AES-256-GCM (alt_crypto.h) under a master key from the keystore (alt_keystore.h):
//
//   offset  size  field
//   0       8     magic "KNALTV01"
//   8       1     keystore kind (AltKeystoreKind; says where the key is, not a secret)
//   9       12    GCM nonce, fresh random on every write
//   21      4     ciphertext length, big endian
//   25      16    GCM tag
//   41      n     ciphertext of the JSON document
//
// Bytes 0..24 are the GCM additional data, so changing any byte of the file (header included)
// makes the open fail; a wrong key fails the same way. The JSON is never written in plain text.
// Plaintext secrets live only in AltVault while it is open and are wiped by alt_vault_wipe().
#include <alt_crypto.h>
#include <alt_keystore.h>

#include <stddef.h>
#include <stdint.h>

#define ALT_MAX_PROFILES 16
#define ALT_ID_LEN 24          // "p" + 16 hex digits + NUL
#define ALT_LABEL_MAX 64       // KN: the username; krunker.io: the name the player gave it
#define ALT_SECRET_MAX 2048    // a KN session token (<= 192 in practice) or a pasted krunker.io credential
#define ALT_HEADER_LEN 41
#define ALT_AAD_LEN 25

typedef enum {
    ALT_KIND_NONE = 0,
    ALT_KIND_KN = 1,      // a KrunkNative account: the secret is its session token
    ALT_KIND_KRUNKER = 2, // a krunker.io account: the secret is its refresh token (krunker_account.h)
} AltKind;

typedef struct {
    char id[ALT_ID_LEN];
    int kind;                      // AltKind
    char label[ALT_LABEL_MAX];
    char secret[ALT_SECRET_MAX];   // the session (a token); refused or forgotten -> ""
    char pass[ALT_SECRET_MAX];     // the account's password, kept for a silent sign-in ("" = none); sealed like the rest
    int64_t added_ms;
} AltProfile;

typedef struct {
    AltProfile profiles[ALT_MAX_PROFILES];
    int count;
    char active_kn[ALT_ID_LEN];       // "" = signed out of KrunkNative
    char active_krunker[ALT_ID_LEN];  // "" = no krunker.io account chosen
    int keystore;                     // AltKeystoreKind the vault was sealed with (set on load)
} AltVault;

void alt_vault_init(AltVault *v);
// Wipes every secret the vault holds (the whole struct: profiles, ids, active pointers).
void alt_vault_wipe(AltVault *v);
int alt_vault_find(const AltVault *v, const char *id);                // index, or -1
// Adds a profile of kind, or replaces the secret of the one with the same kind and label ("" = no session).
// Returns its index (-1 when invalid or the vault is full); out_id (ALT_ID_LEN) gets the id.
// Does not make it active: alt_vault_switch does that.
int alt_vault_put(AltVault *v, int kind, const char *label, const char *secret, int64_t now_ms, char *out_id);
// Removes a profile; an active pointer that named it is cleared. 0 ok, -1 not found.
int alt_vault_remove(AltVault *v, const char *id);
// Wipes a profile's secret (its session) and keeps the profile listed: the account asks for its
// password at the next use. An active pointer that named it is cleared. 0 ok, -1 not found.
int alt_vault_clear_secret(AltVault *v, const char *id);
// Sets a profile's password (sealed with the vault). 0 ok, -1 not found or too long.
int alt_vault_set_password(AltVault *v, const char *id, const char *pass);
// Drops every profile with neither a session nor a password (nothing to sign in with). Returns how many.
int alt_vault_prune(AltVault *v);
// Makes a profile the active one for its kind (a KN profile is the signed-in account, a krunker.io
// profile the chosen one). 0 ok, -1 not found.
int alt_vault_switch(AltVault *v, const char *id);
// Signs out of KrunkNative: the active pointer clears, the profile and its session stay saved.
void alt_vault_sign_out(AltVault *v);
const AltProfile *alt_vault_active(const AltVault *v, int kind); // NULL when none

// Sealed record in memory. alt_vault_seal's output is malloc'd: wipe it and free it.
int alt_vault_seal(const AltVault *v, const uint8_t key[ALT_KEY_LEN], uint8_t **out, size_t *outlen);
// Opens a sealed record into v (v is reset first). 0 ok; -1 on a bad format, a wrong key or any
// change to the bytes (err says which, never the content).
int alt_vault_open_bytes(AltVault *v, const uint8_t *in, size_t inlen, const uint8_t key[ALT_KEY_LEN], char *err, size_t errn);
// The keystore kind a sealed record names, without decrypting it: -1 if it is not a vault.
int alt_vault_peek_keystore(const uint8_t *in, size_t inlen);
// File I/O. load: 0 loaded, 1 no file yet (v is left empty), -1 error. save: atomic (a temp file
// then rename), created 0600. Both need KRUNKNATIVE_HAVE_TLS (the cipher); elsewhere -1.
int alt_vault_load_file(AltVault *v, const char *path, const uint8_t key[ALT_KEY_LEN], char *err, size_t errn);
int alt_vault_save_file(const AltVault *v, const char *path, const uint8_t key[ALT_KEY_LEN]);
// The keystore kind a vault file names: >= 1, 0 no file, -1 not a vault.
int alt_vault_peek_file(const char *path);
