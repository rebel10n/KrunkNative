// The client's one open alt vault (client/include/alt_store.h). One vault per process, one key.
#include <alt_store.h>

#include <alt_keystore.h>

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
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    int n;
    if (xdg && *xdg) n = snprintf(g_dir, sizeof(g_dir), "%s/krunknative", xdg);
    else n = snprintf(g_dir, sizeof(g_dir), "%s/.config/krunknative", home && *home ? home : ".");
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

int alt_store_kn_session(char *name, const size_t namecap, char *token, const size_t tokencap) {
    if (!g_ready || !name || !token || !namecap || !tokencap) return 0;
    const AltProfile *p = alt_vault_active(&g_vault, ALT_KIND_KN);
    if (!p) return 0;
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
