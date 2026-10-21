// The client's one open alt vault (client/include/alt_store.h). One vault per process, one key.
#include <alt_store.h>

#include <alt_keystore.h>
#include <flavor.h> // flavor_storage_subdir: the vault's directory is per flavor

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef KRUNKNATIVE_HAVE_TLS
#include <errno.h>
#include <sys/stat.h>
#endif

static AltVault g_vault;
static int g_ready;
static int g_has_key;
static uint8_t g_key[ALT_KEY_LEN];
static char g_dir[512];
static char g_path[600];
static char g_err[160];

static void set_error(const char *msg) {
    snprintf(g_err, sizeof(g_err), "%s", msg);
}

static int resolve_dir(const char *config_dir) {
    if (config_dir && *config_dir) {
        return snprintf(g_dir, sizeof(g_dir), "%s", config_dir) > 0 ? 0 : -1;
    }
    // the flavor's own directory (flavor.h): Box Office's vault is unchanged, Krunker Strike has its own
    char sub[64];
    if (!flavor_storage_subdir(flavor_get(), sub, sizeof(sub))) return -1;
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    int n;
    if (xdg && *xdg) n = snprintf(g_dir, sizeof(g_dir), "%s/%s", xdg, sub);
    else n = snprintf(g_dir, sizeof(g_dir), "%s/.config/%s", home && *home ? home : ".", sub);
    return n > 0 && (size_t) n < sizeof(g_dir) ? 0 : -1;
}

static void forget_key(void) {
    alt_wipe(g_key, sizeof(g_key));
    alt_keystore_forget(); // the web's key reference (the browser keeps the key itself)
    g_has_key = 0;
}

const char *alt_store_error(void) { return g_err; }

int alt_store_ready(void) { return g_ready; }

int alt_store_open(const char *config_dir) {
    if (g_ready) return 0;
    g_err[0] = 0;
    if (!alt_crypto_available()) {
        set_error("this build has no encryption: saved accounts are off");
        return -1;
    }
    if (resolve_dir(config_dir) != 0 || snprintf(g_path, sizeof(g_path), "%s/alt/vault.bin", g_dir) <= 0) {
        set_error("no storage directory");
        return -1;
    }
    alt_vault_init(&g_vault);
    const int kind = alt_vault_peek_file(g_path);
    if (kind < 0) {
        set_error("the vault file cannot be read");
        return -1;
    }
    if (kind == 0) { // no vault yet: the key is made on the first save
        g_ready = 1;
        return 0;
    }
    if (alt_keystore_fetch(g_dir, kind, g_key, g_err, sizeof(g_err)) != 0) {
        forget_key();
        return -1;
    }
    g_has_key = 1;
    if (alt_vault_load_file(&g_vault, g_path, g_key, g_err, sizeof(g_err)) != 0) {
        alt_vault_wipe(&g_vault);
        forget_key();
        return -1;
    }
    g_ready = 1;
    if (alt_vault_prune(&g_vault) > 0) alt_store_save(); // an entry with no session and no password is dropped
    return 0;
}

AltVault *alt_store_vault(void) { return g_ready ? &g_vault : NULL; }

const char *alt_store_keystore(void) {
    if (!g_ready || !g_has_key) return "";
    return alt_keystore_name(g_vault.keystore);
}

#ifdef KRUNKNATIVE_HAVE_TLS
static int ensure_dirs(void) {
    char alt[600];
    if (snprintf(alt, sizeof(alt), "%s/alt", g_dir) <= 0) return -1;
    if (mkdir(g_dir, 0700) != 0 && errno != EEXIST) return -1;
    if (mkdir(alt, 0700) != 0 && errno != EEXIST) return -1;
    return 0;
}
#endif

int alt_store_save(void) {
    if (!g_ready) return -1;
#ifdef KRUNKNATIVE_HAVE_TLS
    if (ensure_dirs() != 0) {
        set_error("cannot create the storage directory");
        return -1;
    }
#endif
    if (!g_has_key) { // first save: a new vault, so a new key (only ever made where no vault exists)
        int kind = ALT_KS_NONE;
        if (alt_keystore_create(g_dir, g_key, &kind, g_err, sizeof(g_err)) != 0) return -1;
        g_has_key = 1;
        g_vault.keystore = kind;
    }
    if (alt_vault_save_file(&g_vault, g_path, g_key) != 0) {
        set_error("cannot write the vault file");
        return -1;
    }
    return 0;
}

static const AltProfile *kn_profile_named(const char *username) {
    if (!g_ready || !username || !*username) return NULL;
    for (int i = 0; i < g_vault.count; i++) {
        const AltProfile *p = &g_vault.profiles[i];
        if (p->kind == ALT_KIND_KN && !strcmp(p->label, username)) return p;
    }
    return NULL;
}

int alt_store_kn_listed(const char *username) { return kn_profile_named(username) != NULL; }

int alt_store_kn_forget(const char *username) {
    const AltProfile *p = kn_profile_named(username);
    if (!p) return 0;
    char id[ALT_ID_LEN];
    snprintf(id, sizeof(id), "%s", p->id);
    alt_vault_clear_secret(&g_vault, id);
    return alt_store_save();
}

int alt_store_kn_session(char *name, const size_t namecap, char *token, const size_t tokencap) {
    if (!g_ready || !name || !token || !namecap || !tokencap) return 0;
    const AltProfile *p = alt_vault_active(&g_vault, ALT_KIND_KN);
    if (!p || !p->secret[0]) return 0; // a listed account whose session was forgotten has no token to resume
    snprintf(name, namecap, "%s", p->label);
    snprintf(token, tokencap, "%s", p->secret);
    return 1;
}

int alt_store_kn_save(const char *username, const char *token) {
    if (!g_ready || !username || !*username || !token || !*token) return -1;
    char id[ALT_ID_LEN];
    const int64_t now = (int64_t) time(NULL) * 1000;
    if (alt_vault_put(&g_vault, ALT_KIND_KN, username, token, now, id) < 0) {
        set_error("the vault is full or the account name is too long");
        return -1;
    }
    alt_vault_switch(&g_vault, id);
    return alt_store_save();
}

static const AltProfile *profile_named(const int kind, const char *label) {
    if (!g_ready || !label || !*label) return NULL;
    for (int i = 0; i < g_vault.count; i++) {
        const AltProfile *p = &g_vault.profiles[i];
        if (p->kind == kind && !strcmp(p->label, label)) return p;
    }
    return NULL;
}

int alt_store_list(const int kind, const char *label, const char *token, const char *pass, char *out_id) {
    if (!g_ready || !label || !*label || strlen(label) >= ALT_LABEL_MAX) return -1;
    char id[ALT_ID_LEN];
    const int64_t now = (int64_t) time(NULL) * 1000;
    const AltProfile *old = profile_named(kind, label);
    if (token && *token) { // a session: the profile keeps (or gets) this token
        if (alt_vault_put(&g_vault, kind, label, token, now, id) < 0) {
            set_error("the vault is full or the account name is too long");
            return -1;
        }
    } else if (old) {
        snprintf(id, sizeof(id), "%s", old->id);
    } else { // a password only: the profile is listed without a session
        if (alt_vault_put(&g_vault, kind, label, "", now, id) < 0) {
            set_error("the vault is full or the account name is too long");
            return -1;
        }
    }
    if (pass && *pass && alt_vault_set_password(&g_vault, id, pass) != 0) {
        set_error("the password is too long");
        return -1;
    }
    if (out_id) snprintf(out_id, ALT_ID_LEN, "%s", id);
    return alt_store_save(); // (listed, not switched to: the signed-in account stays as it is)
}

int alt_store_password(const int kind, const char *label, char *out, const size_t cap) {
    if (out && cap) out[0] = 0;
    const AltProfile *p = profile_named(kind, label);
    if (!p || !p->pass[0] || !out || !cap) return 0;
    snprintf(out, cap, "%s", p->pass);
    return 1;
}

int alt_store_forget_password(const int kind, const char *label) {
    const AltProfile *p = profile_named(kind, label);
    if (!p) return 0;
    char id[ALT_ID_LEN];
    snprintf(id, sizeof(id), "%s", p->id);
    alt_vault_set_password(&g_vault, id, "");
    return alt_store_save();
}

int alt_store_kn_active_name(char *name, const size_t namecap) {
    if (!g_ready || !name || !namecap) return 0;
    const AltProfile *p = alt_vault_active(&g_vault, ALT_KIND_KN);
    if (!p) return 0;
    snprintf(name, namecap, "%s", p->label);
    return 1;
}

int alt_store_kn_sign_out(void) {
    if (!g_ready || !alt_vault_active(&g_vault, ALT_KIND_KN)) return 0;
    alt_vault_sign_out(&g_vault);
    return alt_store_save();
}

void alt_store_close(void) {
    alt_vault_wipe(&g_vault);
    forget_key();
    g_ready = 0;
    g_err[0] = 0;
}
