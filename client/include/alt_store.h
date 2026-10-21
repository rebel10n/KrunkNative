#pragma once
// The client's one open alt vault (client/src/alt/alt_store.c): the KrunkNative session the player
// is signed in with lives here, sealed, instead of in the plain krunker_token storage key. The
// store opens the vault at startup when a vault file exists, creates the master key on the first
// save, and reports why it cannot (no crypto in this build, a locked keyring, a damaged file).
// Every call is a no-op returning failure until alt_store_open() succeeds.
#include <alt_vault.h>

#include <stddef.h>

// config_dir NULL = $XDG_CONFIG_HOME/<the flavor's directory> (else ~/.config/...; flavor.h
// flavor_storage_subdir: krunknative for Box Office, krunknative-strike for Krunker Strike).
// 0 open (a vault loaded, or none yet), -1 unavailable: alt_store_error() says why.
int alt_store_open(const char *config_dir);
int alt_store_ready(void);
const char *alt_store_error(void);
const char *alt_store_keystore(void); // "file", "secret service", "keychain"; "" before the first save
// The open vault, for the manager to list and edit (NULL unless ready). Call alt_store_save() after a change.
AltVault *alt_store_vault(void);
int alt_store_save(void); // 0 ok, -1 failure (the vault stays as it was in memory)
// The active KrunkNative session: 1 and its username / token, 0 when signed out or not ready.
int alt_store_kn_session(char *name, size_t namecap, char *token, size_t tokencap);
// Keeps a KrunkNative session: the account's profile gets this token and becomes the active one.
// 0 ok, -1 failure (the caller may keep its own copy then).
int alt_store_kn_save(const char *username, const char *token);
// Signs out: the active pointer clears, the saved profile stays. 0 ok (or nothing to clear).
int alt_store_kn_sign_out(void);
// The KrunkNative profile with this login name is listed (1) or not (0; 0 when not ready).
int alt_store_kn_listed(const char *username);
// Forgets the session of the listed profile with this name: its token is wiped, the profile stays
// listed (its stored password signs it in next time) and signed out. 0 ok or not listed, -1 failure.
int alt_store_kn_forget(const char *username);
// The Account Manager's Add: lists the account (kind = ALT_KIND_KN or ALT_KIND_KRUNKER) with its session
// token ("" or NULL keeps the one it has) and its password ("" or NULL keeps the one it has). Nothing is
// switched to. 0 ok, -1 failure. out_id (ALT_ID_LEN, may be NULL) gets the profile's id.
int alt_store_list(int kind, const char *label, const char *token, const char *pass, char *out_id);
// The listed profile's stored password (copied into out): 1 found, 0 none (out is "").
int alt_store_password(int kind, const char *label, char *out, size_t cap);
// Drops the stored password of a listed profile (a refused one). 0 ok or not listed, -1 failure.
int alt_store_forget_password(int kind, const char *label);
// The active KrunkNative profile's login name: 1 and the name, 0 none (its token may be gone: see alt_store_kn_session).
int alt_store_kn_active_name(char *name, size_t namecap);
void alt_store_close(void); // wipes the vault and the key from memory
