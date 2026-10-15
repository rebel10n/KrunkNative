// The alt manager window (client/include/alt_manager_ui.h): the saved accounts, which one is active,
// add / remove / switch. The data and its encryption are in client/src/alt/ (the vault); this file
// is the window and the account switch, which goes through account_ui.c like a saved login.
#include <alt_manager_ui.h>

#include <alt_store.h>
#include <backend_ui.h>
#include <ui_logic.h>

#include <stdio.h>
#include <string.h>
#include <time.h>

// The last result, shown under the list. Never a token or a credential.
static char g_msg[200];

static void set_msg(const char *text) {
    snprintf(g_msg, sizeof(g_msg), "%s", text ? text : "");
}

static void rerender(void) {
    menu_rerender_if_open(ALT_MANAGER_WINDOW);
}

static int64_t now_ms(void) {
    return (int64_t) time(NULL) * 1000;
}

static const char *arg_text(const HtmlArg *args, const int argc, const int i) {
    return args && i < argc && args[i].kind == HTML_ARG_STR ? args[i].str : NULL;
}

static int is_active(const AltVault *v, const AltProfile *p) {
    return p->kind == ALT_KIND_KN ? !strcmp(v->active_kn, p->id) : !strcmp(v->active_krunker, p->id);
}

// ---------------------------------------------------------------- actions

// Switches to a saved profile. A KrunkNative profile signs in with its saved session (the account
// service checks it); a krunker.io profile becomes the chosen one.
static void use_profile(const char *id) {
    AltVault *v = alt_store_vault();
    const int i = v ? alt_vault_find(v, id) : -1;
    if (i < 0) {
        set_msg(I18N("kn.alt.gone", "That account is no longer saved."));
        return;
    }
    const AltProfile *p = &v->profiles[i];
    if (p->kind == ALT_KIND_KN) {
        if (is_active(v, p) && account_get()) {
            char msg[sizeof(g_msg)];
            I18N_F(msg, "kn.alt.already", "You are already signed in as {0}.", p->label);
            set_msg(msg);
            return;
        }
        char name[ALT_LABEL_MAX], token[ALT_SECRET_MAX];
        snprintf(name, sizeof(name), "%s", p->label);
        snprintf(token, sizeof(token), "%s", p->secret);
        const int started = account_ui_use_session(name, token);
        alt_wipe(token, sizeof(token));
        if (started != 0) {
            set_msg(I18N("kn.alt.noServer", "Accounts need the account server, which this build does not reach."));
            return;
        }
        char msg[sizeof(g_msg)];
        I18N_F(msg, "kn.alt.signingIn", "Signing in as {0}...", name);
        set_msg(msg);
        return;
    }
    // krunker.io profile: remember the choice (the official join does not send a saved session yet)
    if (alt_vault_switch(v, id) != 0 || alt_store_save() != 0) {
        set_msg(I18N("kn.alt.saveFailed", "Could not write the vault."));
        return;
    }
    char msg[sizeof(g_msg)];
    I18N_F(msg, "kn.alt.krunkerChosen", "{0} is the chosen krunker.io account. Official games do not use a saved krunker.io session yet.", p->label);
    set_msg(msg);
}

static void remove_profile(const char *id) {
    AltVault *v = alt_store_vault();
    const int i = v ? alt_vault_find(v, id) : -1;
    if (i < 0) {
        set_msg(I18N("kn.alt.gone", "That account is no longer saved."));
        return;
    }
    char label[ALT_LABEL_MAX];
    snprintf(label, sizeof(label), "%s", v->profiles[i].label);
    alt_vault_remove(v, id);
    if (alt_store_save() != 0) {
        set_msg(I18N("kn.alt.saveFailed", "Could not write the vault."));
        return;
    }
    char msg[sizeof(g_msg)];
    I18N_F(msg, "kn.alt.removed", "Removed {0}.", label);
    set_msg(msg);
}

// Saves the KrunkNative account that is signed in now (normally already saved at sign-in).
static void save_current(void) {
    const AccountInfo *acc = account_get();
    const char *token = account_session_token();
    if (!acc || !token[0]) {
        set_msg(I18N("kn.alt.signInFirst", "Sign in to KrunkNative first, then save the account."));
        return;
    }
    if (alt_store_kn_save(acc->name, token) != 0) {
        set_msg(alt_store_error()[0] ? alt_store_error() : I18N("kn.alt.saveFailed", "Could not write the vault."));
        return;
    }
    char msg[sizeof(g_msg)];
    I18N_F(msg, "kn.alt.saved", "Saved {0}.", acc->name);
    set_msg(msg);
}

// Adds a krunker.io account from the window's two fields. The credential field is cleared at once.
static void add_krunker(void) {
    char label[ALT_LABEL_MAX] = "", secret[ALT_SECRET_MAX] = "";
    const HtmlElem label_el = html_dom_by_id("knAltLabel"), secret_el = html_dom_by_id("knAltSecret");
    if (label_el) html_dom_get_value(label_el, label, sizeof(label));
    if (secret_el) {
        html_dom_get_value(secret_el, secret, sizeof(secret));
        html_dom_set_value(secret_el, "");
    }
    if (!secret[0]) {
        set_msg(I18N("kn.alt.pasteFirst", "Paste the krunker.io session credential first."));
        alt_wipe(secret, sizeof(secret));
        return;
    }
    if (!label[0]) snprintf(label, sizeof(label), "%s", I18N("kn.alt.defaultLabel", "krunker.io account"));
    AltVault *v = alt_store_vault();
    if (!v) {
        set_msg(alt_store_error()[0] ? alt_store_error() : I18N("kn.alt.unavailable", "Saved accounts are not available."));
        alt_wipe(secret, sizeof(secret));
        return;
    }
    char id[ALT_ID_LEN];
    if (alt_vault_put(v, ALT_KIND_KRUNKER, label, secret, now_ms(), id) < 0) {
        set_msg(I18N("kn.alt.tooMany", "Cannot save it: the list is full, or the name or credential is too long."));
    } else {
        if (!v->active_krunker[0]) alt_vault_switch(v, id);
        set_msg(alt_store_save() == 0 ? I18N("kn.alt.krunkerSaved", "Saved the krunker.io account.") : I18N("kn.alt.saveFailed", "Could not write the vault."));
    }
    alt_wipe(secret, sizeof(secret));
}

// ---------------------------------------------------------------- window

static void append_profile(StrBuf *sb, const AltVault *v, const AltProfile *p) {
    const int active = is_active(v, p);
    sb_append(sb, "<div class='knAltRow");
    sb_append(sb, active ? " knAltActive'>" : "'>");
    sb_append(sb, "<div class='knAltWho'><div class='knAltName'>");
    sb_append_esc(sb, p->label);
    sb_append(sb, "</div><div class='knAltKind'>");
    sb_append(sb, p->kind == ALT_KIND_KN ? "KrunkNative" : "krunker.io");
    if (active) sb_append(sb, " &middot; ");
    if (active) sb_append(sb, I18N("kn.alt.active", "active"));
    sb_append(sb, "</div></div><div class='knAltBtns'>");
    if (active) {
        sb_append(sb, "<div class='knAltBtn knAltOn'>");
        sb_append_esc(sb, I18N("kn.alt.activeBtn", "Active"));
        sb_append(sb, "</div>");
    } else {
        sb_append(sb, "<div class='joinBtn knAltBtn' onclick='knAltUse(\"");
        sb_append_esc(sb, p->id);
        sb_append(sb, "\")'>");
        sb_append_esc(sb, I18N("kn.alt.use", "Use"));
        sb_append(sb, "</div>");
    }
    sb_append(sb, "<div class='knAltBtn knAltDanger' onclick='knAltRemove(\"");
    sb_append_esc(sb, p->id);
    sb_append(sb, "\")'>");
    sb_append_esc(sb, I18N("kn.alt.remove", "Remove"));
    sb_append(sb, "</div></div></div>");
}

char *alt_manager_window_gen(const int id, const char **header) {
    if (id != ALT_MANAGER_WINDOW) return NULL;
    if (header) *header = I18N("kn.alt.title", "Accounts");
    alt_store_open(NULL); // idempotent; reads no key without a vault file
    StrBuf sb = {0};
    // v7 hides #windowHeader: the title is part of the window (as the Object Manager's is)
    sb_append(&sb, "<div class='knAlt'><div class='knAltTitle'>");
    sb_append_esc(&sb, I18N("kn.alt.title", "Accounts"));
    sb_append(&sb, "</div><div class='knAltIntro'>");
    sb_append_esc(&sb, I18N("kn.alt.intro", "Switch between your own accounts. Saved sessions are sealed with AES-256-GCM under a master key kept by your system (on the web, by your browser)."));
    sb_append(&sb, "</div>");
    AltVault *v = alt_store_vault();
    if (!v) {
        sb_append(&sb, "<div class='knAltWarn'>");
        sb_append_esc(&sb, alt_store_error()[0] ? alt_store_error() : I18N("kn.alt.unavailable", "Saved accounts are not available."));
        sb_append(&sb, "</div>");
    } else {
        if (!strcmp(alt_store_keystore(), "browser")) {
            sb_append(&sb, "<div class='knAltNote'>");
            sb_append_esc(&sb, I18N("kn.alt.browserKey", "The master key is a non-extractable key in this browser's storage: the page can use it, never read it. Clearing this site's data loses the saved accounts."));
            sb_append(&sb, "</div>");
        }
        if (alt_store_keystore()[0] && !strcmp(alt_store_keystore(), "file")) {
            sb_append(&sb, "<div class='knAltNote'>");
            sb_append_esc(&sb, I18N("kn.alt.fileKey", "No system keyring here: the master key is a file in your config folder, readable by anyone who can read that folder."));
            sb_append(&sb, "</div>");
        }
        sb_append(&sb, "<div class='knAltList'>");
        if (v->count == 0) {
            sb_append(&sb, "<div class='knAltEmpty'>");
            sb_append_esc(&sb, I18N("kn.alt.empty", "No saved accounts yet."));
            sb_append(&sb, "</div>");
        }
        for (int i = 0; i < v->count; i++) append_profile(&sb, v, &v->profiles[i]);
        sb_append(&sb, "</div>");
        if (account_get() && account_session_token()[0]) {
            sb_append(&sb, "<div class='joinBtn knAltWide' onclick='knAltSaveKn()'>");
            sb_append_esc(&sb, I18N("kn.alt.saveCurrent", "Save the KrunkNative account I am signed in to"));
            sb_append(&sb, "</div>");
        }
        sb_append(&sb, "<div class='knAltAdd'><div class='knAltAddTitle'>");
        sb_append_esc(&sb, I18N("kn.alt.addKrunker", "Add a krunker.io account"));
        sb_append(&sb, "</div><input id='knAltLabel' class='accountInput' type='text' maxlength='60' placeholder='");
        sb_append_esc(&sb, I18N("kn.alt.labelPh", "Name for this account"));
        sb_append(&sb, "'><input id='knAltSecret' class='accountInput' type='password' maxlength='2000' autocomplete='off' placeholder='");
        sb_append_esc(&sb, I18N("kn.alt.secretPh", "Session credential"));
        sb_append(&sb, "'><div class='joinBtn knAltWide' onclick='knAltAddKrunker()'>");
        sb_append_esc(&sb, I18N("kn.alt.add", "Add account"));
        sb_append(&sb, "</div></div>");
    }
    sb_append(&sb, "<div id='knAltMsg' class='knAltMsg'>");
    sb_append_esc(&sb, g_msg);
    sb_append(&sb, "</div></div>");
    return sb_finish(&sb);
}

int alt_manager_ui_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) ev;
    if (!name || strncmp(name, "knAlt", 5) != 0) return -1;
    if (!strcmp(name, "knAltUse")) {
        const char *id = arg_text(args, argc, 0);
        if (id) use_profile(id);
    } else if (!strcmp(name, "knAltRemove")) {
        const char *id = arg_text(args, argc, 0);
        if (id) remove_profile(id);
    } else if (!strcmp(name, "knAltSaveKn")) {
        save_current();
    } else if (!strcmp(name, "knAltAddKrunker")) {
        add_krunker();
    } else {
        return -1;
    }
    rerender();
    return 0;
}
