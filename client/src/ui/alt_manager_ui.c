// The Account Manager window (client/include/alt_manager_ui.h): the accounts the player listed, both
// backends in one list. Each row is one account: its name, a tag (krunk.io: a Box Office account; krunker.io:
// an official account), and Use / Remove. A row keeps its session (a token) and its password, both sealed in
// the alt vault. Add signs the typed login and password in to the backend the chips pick, lists the account
// with them, and leaves the signed-in account as it is (no switch). Use signs the row in with its token, or with
// its stored password when the token is gone or refused; a row with neither offers a Sign in (the form).
//
// Switching accounts (Use) is refused in an official game: the game plays on the krunker.io account.
#include <alt_manager_ui.h>

#include <alt_crypto.h>
#include <alt_store.h>
#include <backend_ui.h>
#include <krunker_account.h>
#include <ui_logic.h>

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define ROWS_MAX ALT_MAX_PROFILES

// The last result, shown under the list. Never a token or a credential.
static char g_msg[200];
// The form: the login name (a row that needs its password fills it; a failed Add keeps what was typed) and
// the backend its chips pick (ALT_KIND_KN or ALT_KIND_KRUNKER).
static char g_form_user[ALT_LABEL_MAX];
static int g_form_kind = ALT_KIND_KN;
// The krunker.io Add's password, kept until its answer (listed sealed, then wiped); a Use's stored one is read
// from the vault when it is needed.
static char g_pending_pass[ALT_SECRET_MAX];
// The krunker.io request the window started (its outcome comes as a krunker.io event). krunk.io's comes
// through alt_manager_ui_sign_in_done (account_ui.c).
enum { PEND_NONE, PEND_ADD, PEND_USE };
static int g_pending;
static char g_pending_name[ALT_LABEL_MAX];
// render scenarios: state.altAccounts stands in for the vault's rows (no vault, no actions)
static cJSON *g_scenario;

typedef struct {
    char id[ALT_ID_LEN];
    char label[ALT_LABEL_MAX];
    int kind; // ALT_KIND_KN (krunk.io) | ALT_KIND_KRUNKER (krunker.io)
    int active;     // the account the client plays as now
    int has_secret; // a stored session (krunk.io token / krunker.io refresh token)
    int has_pass;   // a stored password (sealed): signs the account in when the session is gone or refused
} Row;

static void set_msg(const char *text) {
    snprintf(g_msg, sizeof(g_msg), "%s", text ? text : "");
}

static void rerender(void) {
    menu_rerender_if_open(ALT_MANAGER_WINDOW);
}

static const char *arg_text(const HtmlArg *args, const int argc, const int i) {
    return args && i < argc && args[i].kind == HTML_ARG_STR ? args[i].str : NULL;
}

static void read_input(const char *id, char *out, const size_t n) {
    out[0] = 0;
    HtmlElem el = html_dom_by_id(id);
    if (el) html_dom_get_value(el, out, n);
}

static int in_official_game(void) { return account_backend() == ACCOUNT_BACKEND_KRUNKER; }

static int kn_signed_as(const char *name) {
    const AccountInfo *acc = krunkio_account();
    return acc && !strcmp(acc->name, name);
}

// krunker.io's logins are case-blind, and the name it reports after a restart can differ in case from the one
// typed (the profile's answer): the rows match case-blind.
static int same_login(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        if (tolower((unsigned char) *a) != tolower((unsigned char) *b)) return 0;
    }
    return *a == *b;
}

static int kio_signed_as(const char *name) {
    return krunker_account_signed_in() && same_login(krunker_account_name(), name);
}

// The vault's krunker.io row for this login (its stored label), NULL when none.
static const AltProfile *kio_profile(const char *name) {
    AltVault *v = alt_store_vault();
    if (!v || !name || !name[0]) return NULL;
    for (int i = 0; i < v->count; i++) {
        if (v->profiles[i].kind == ALT_KIND_KRUNKER && same_login(v->profiles[i].label, name)) return &v->profiles[i];
    }
    return NULL;
}

static void pending_end(void) {
    g_pending = PEND_NONE;
    alt_wipe(g_pending_pass, sizeof(g_pending_pass));
}

// ---------------------------------------------------------------- rows

// The vault's profiles (both kinds), or the render scenario's stand-in rows.
static int build_rows(Row *rows) {
    int n = 0;
    if (g_scenario) {
        const cJSON *item;
        cJSON_ArrayForEach(item, g_scenario) {
            if (n >= ROWS_MAX) break;
            const char *name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "name"));
            const char *backend = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "backend"));
            const cJSON *active = cJSON_GetObjectItemCaseSensitive(item, "active");
            if (!name) continue;
            Row *r = &rows[n++];
            memset(r, 0, sizeof(*r));
            snprintf(r->id, sizeof(r->id), "%s", name);
            snprintf(r->label, sizeof(r->label), "%s", name);
            r->kind = backend && !strcmp(backend, "krunker.io") ? ALT_KIND_KRUNKER : ALT_KIND_KN;
            r->active = cJSON_IsTrue(active);
            r->has_secret = 1;
            r->has_pass = 1;
        }
        return n;
    }
    AltVault *v = alt_store_vault();
    if (!v) return 0;
    for (int i = 0; i < v->count && n < ROWS_MAX; i++) {
        const AltProfile *p = &v->profiles[i];
#ifdef __EMSCRIPTEN__
        if (p->kind == ALT_KIND_KRUNKER) continue; // the web plays krunk.io only: no krunker.io rows
#endif
        Row *r = &rows[n++];
        memset(r, 0, sizeof(*r));
        snprintf(r->id, sizeof(r->id), "%s", p->id);
        snprintf(r->label, sizeof(r->label), "%s", p->label);
        r->kind = p->kind;
        r->has_secret = p->secret[0] != 0;
        r->has_pass = p->pass[0] != 0;
        r->active = p->kind == ALT_KIND_KN ? krunkio_account() != NULL && !strcmp(v->active_kn, p->id) : kio_signed_as(p->label);
    }
    return n;
}

static int find_profile(const char *id, const AltProfile **out) {
    AltVault *v = alt_store_vault();
    const int i = v ? alt_vault_find(v, id) : -1;
    if (i < 0) return 0;
    *out = &v->profiles[i];
    return 1;
}

// ---------------------------------------------------------------- krunker.io (the vault's krunker rows)

// Stores the krunker.io account's current refresh token under its row's label (added, or the same account's new one).
static int store_kio(const char *name) {
    char token[ALT_SECRET_MAX];
    AltVault *v = alt_store_vault();
    if (!v || !name || !name[0] || !krunker_account_refresh_token(token, sizeof(token))) {
        alt_wipe(token, sizeof(token));
        return -1;
    }
    const AltProfile *row = kio_profile(name);
    char label[ALT_LABEL_MAX];
    snprintf(label, sizeof(label), "%s", row ? row->label : name);
    char id[ALT_ID_LEN];
    const int64_t now = (int64_t) time(NULL) * 1000;
    const int rc = alt_vault_put(v, ALT_KIND_KRUNKER, label, token, now, id);
    alt_wipe(token, sizeof(token));
    if (rc < 0 || alt_store_save() != 0) return -1;
    return 0;
}

// An account the list already holds gets its new session (a sign-in or a refresh); others are not listed.
static void sync_kio(const char *name) {
    if (kio_profile(name)) store_kio(name);
}

static void forget_kio(const char *name) {
    AltVault *v = alt_store_vault();
    const AltProfile *row = kio_profile(name);
    if (!v || !row) return;
    char id[ALT_ID_LEN];
    snprintf(id, sizeof(id), "%s", row->id);
    alt_vault_clear_secret(v, id);
    alt_store_save();
}

// krunker.io's sign-in and session events (krunker_account.h). Only a request the window started
// (g_pending) reports to the player; the listed accounts' stored sessions follow the account in any case.
static void on_kio_event(void *user, const KrunkerAccountEvent ev) {
    (void) user;
    const int mine = g_pending != PEND_NONE;
    int finished = 1; // the window's request ends with this event (an expired Use's stored password answers later)
    char msg[sizeof(g_msg)];
    switch (ev) {
        case KRUNKER_EV_ADDED: // the Add: the account is listed with its refresh token and password; nothing is signed in
            if (mine && g_pending == PEND_ADD) {
                char token[ALT_SECRET_MAX];
                const char *name = krunker_account_added_name();
                const AltProfile *row = kio_profile(name);
                const int got = krunker_account_added_refresh(token, sizeof(token));
                const int rc = got ? alt_store_list(ALT_KIND_KRUNKER, row ? row->label : name, token, g_pending_pass, NULL) : -1;
                alt_wipe(token, sizeof(token));
                if (rc == 0) {
                    I18N_F(msg, "kn.alt.added", "Added {0}.", name);
                    set_msg(msg);
                    if (!strcmp(g_form_user, g_pending_name)) g_form_user[0] = 0;
                } else {
                    set_msg(I18N("kn.alt.saveFailed", "Could not write the vault."));
                }
            }
            break;
        case KRUNKER_EV_SIGNED_IN:
            if (mine && g_pending == PEND_USE) { // a Use signed in with the stored password: the row takes the new session
                if (store_kio(krunker_account_name()) == 0) {
                    I18N_F(msg, "kn.alt.signedIn", "Signed in as {0}.", krunker_account_name());
                    set_msg(msg);
                } else {
                    set_msg(I18N("kn.alt.saveFailed", "Could not write the vault."));
                }
            } else {
                sync_kio(krunker_account_name());
            }
            break;
        case KRUNKER_EV_SIGN_IN_FAILED:
            if (mine && g_pending == PEND_ADD) set_msg(krunker_account_message()[0] ? krunker_account_message() : I18N("kn.alt.failed", "Could not sign in."));
            else if (mine && g_pending == PEND_USE) set_msg(I18N("kn.alt.invalid", "Saved login is no longer valid. Sign in again to restore it."));
            break;
        case KRUNKER_EV_RESUMED:
            if (mine && g_pending == PEND_USE) {
                if (store_kio(krunker_account_name()) == 0) {
                    I18N_F(msg, "kn.alt.signedIn", "Signed in as {0}.", krunker_account_name());
                    set_msg(msg);
                } else {
                    set_msg(I18N("kn.alt.saveFailed", "Could not write the vault."));
                }
            } else {
                sync_kio(krunker_account_name());
            }
            break;
        case KRUNKER_EV_RESUME_FAILED:
            if (mine && g_pending == PEND_USE) set_msg(krunker_account_message()[0] ? krunker_account_message() : I18N("kn.alt.failed", "Could not sign in."));
            break;
        case KRUNKER_EV_EXPIRED: { // the stored session is refused: forgotten (the row keeps its password)
            char name[ALT_LABEL_MAX];
            snprintf(name, sizeof(name), "%s", krunker_account_name());
            forget_kio(name);
            if (mine && g_pending == PEND_USE) {
                char pass[ALT_SECRET_MAX];
                if (alt_store_password(ALT_KIND_KRUNKER, g_pending_name, pass, sizeof(pass))) {
                    // the stored password signs in, silently: it starts once the expired session is forgotten
                    krunker_account_sign_in_when_expired(g_pending_name, pass);
                    alt_wipe(pass, sizeof(pass));
                    finished = 0;
                } else {
                    set_msg(I18N("kn.alt.invalid", "Saved login is no longer valid. Sign in again to restore it."));
                }
            }
            break;
        }
        case KRUNKER_EV_REFRESHED:
            sync_kio(krunker_account_name());
            break;
        case KRUNKER_EV_CODE_ASKED: // the code step: its instruction is krunker_account_message()
            if (mine && (g_pending == PEND_ADD || g_pending == PEND_USE)) set_msg(krunker_account_message());
            break;
    }
    if (mine && finished && (ev == KRUNKER_EV_ADDED || ev == KRUNKER_EV_SIGNED_IN || ev == KRUNKER_EV_SIGN_IN_FAILED || ev == KRUNKER_EV_RESUMED ||
                             ev == KRUNKER_EV_RESUME_FAILED || ev == KRUNKER_EV_EXPIRED))
        pending_end();
    rerender();
}

void alt_manager_ui_init(void) {
    krunker_account_listen_events(on_kio_event, NULL);
}

// ---------------------------------------------------------------- actions

// Use: a listed account signs in with its stored session (the service checks it); with none, or a refused one,
// its stored password signs it in (silently). krunk.io: account_ui.c's switch (its answer comes to
// alt_manager_ui_sign_in_done); krunker.io: krunker_account_resume / krunker_account_sign_in (its answer comes as
// a krunker.io event). A row with neither says so, and offers a Sign in (the form).
static void use_row(const char *id) {
    const AltProfile *p = NULL;
    if (!find_profile(id, &p)) {
        set_msg(I18N("kn.alt.gone", "That account is no longer saved."));
        return;
    }
    if (p->kind == ALT_KIND_KN ? kn_signed_as(p->label) : kio_signed_as(p->label)) {
        char msg[sizeof(g_msg)];
        I18N_F(msg, "kn.alt.already", "You are already signed in as {0}.", p->label);
        set_msg(msg);
        return;
    }
    if (in_official_game()) {
        set_msg(I18N("kn.alt.inGame", "Leave the official game first."));
        return;
    }
    if (!p->secret[0] && !p->pass[0]) {
        set_msg(I18N("kn.alt.invalid", "Saved login is no longer valid. Sign in again to restore it."));
        return;
    }
    char name[ALT_LABEL_MAX], token[ALT_SECRET_MAX], pass[ALT_SECRET_MAX];
    snprintf(name, sizeof(name), "%s", p->label);
    snprintf(token, sizeof(token), "%s", p->secret);
    snprintf(pass, sizeof(pass), "%s", p->pass);
    char msg[sizeof(g_msg)];
    if (p->kind == ALT_KIND_KN) {
        const int started = token[0] ? account_ui_use_session(name, token) : account_ui_use_password(name, pass);
        alt_wipe(token, sizeof(token));
        alt_wipe(pass, sizeof(pass));
        if (started != 0) {
            set_msg(I18N("kn.alt.noServer", "Can't reach the account server."));
            return;
        }
    } else {
        const int started = token[0] ? krunker_account_resume(name, token) : krunker_account_sign_in(name, pass);
        alt_wipe(token, sizeof(token));
        alt_wipe(pass, sizeof(pass));
        if (!started) {
            set_msg(krunker_account_message()[0] ? krunker_account_message() : I18N("kn.alt.failed", "Could not sign in."));
            return;
        }
        g_pending = PEND_USE;
        snprintf(g_pending_name, sizeof(g_pending_name), "%s", name);
    }
    I18N_F(msg, "kn.alt.signingIn", "Signing in as {0}...", name);
    set_msg(msg);
}

// Remove: forgets the account and its stored session. The account signed in is signed out first (krunk.io
// always; krunker.io only outside an official game, where it plays).
static void remove_row(const char *id) {
    const AltProfile *p = NULL;
    if (!find_profile(id, &p)) {
        set_msg(I18N("kn.alt.gone", "That account is no longer saved."));
        return;
    }
    char label[ALT_LABEL_MAX];
    snprintf(label, sizeof(label), "%s", p->label);
    if (p->kind == ALT_KIND_KN) {
        account_ui_sign_out_of(label);
    } else if (kio_signed_as(label)) {
        if (in_official_game()) {
            set_msg(I18N("kn.alt.inGame", "Leave the official game first."));
            return;
        }
        krunker_account_sign_out();
    }
    AltVault *v = alt_store_vault();
    alt_vault_remove(v, id);
    if (alt_store_save() != 0) {
        set_msg(I18N("kn.alt.saveFailed", "Could not write the vault."));
        return;
    }
    if (!strcmp(g_form_user, label)) g_form_user[0] = 0;
    char msg[sizeof(g_msg)];
    I18N_F(msg, "kn.alt.removed", "Removed {0}.", label);
    set_msg(msg);
}

// The code step's code (the 2FA app's or the email's): the same submit as v10's login modal.
static void submit_code(const char *code) {
    if (!code || !code[0]) return;
    if (!krunker_account_submit_code(code)) set_msg(krunker_account_message()[0] ? krunker_account_message() : I18N("kn.alt.failed", "Could not sign in."));
}

// Add: the typed login and password sign in to the chosen backend, and the account is listed with them. The
// signed-in account stays as it is (nothing is switched to). The password is wiped by the caller and kept here
// only until the answer.
static void add_account(const char *name, const char *pass) {
    snprintf(g_form_user, sizeof(g_form_user), "%s", name ? name : "");
    if (!name || !name[0] || !pass || !pass[0]) {
        set_msg(I18N("kn.alt.needCreds", "Enter the login and the password."));
        return;
    }
    if (in_official_game()) {
        set_msg(I18N("kn.alt.inGame", "Leave the official game first."));
        return;
    }
    char msg[sizeof(g_msg)];
    if (g_form_kind == ALT_KIND_KN) {
        if (account_ui_add_sign_in(name, pass) != 0) {
            set_msg(I18N("kn.alt.noServer", "Can't reach the account server."));
            return;
        }
    } else {
        snprintf(g_pending_pass, sizeof(g_pending_pass), "%s", pass);
        if (!krunker_account_add(name, pass)) {
            alt_wipe(g_pending_pass, sizeof(g_pending_pass));
            set_msg(krunker_account_message()[0] ? krunker_account_message() : I18N("kn.alt.failed", "Could not sign in."));
            return;
        }
        g_pending = PEND_ADD;
        snprintf(g_pending_name, sizeof(g_pending_name), "%s", name);
    }
    I18N_F(msg, "kn.alt.signingIn", "Signing in as {0}...", name);
    set_msg(msg);
}

// ---------------------------------------------------------------- answers from account_ui.c (krunk.io)

void alt_manager_ui_sign_in_done(const AltSignIn result, const char *name, const char *error) {
    char msg[sizeof(g_msg)];
    switch (result) {
        case ALT_SIGN_IN_ADDED:
            if (name && !strcmp(g_form_user, name)) g_form_user[0] = 0;
            I18N_F(msg, "kn.alt.added", "Added {0}.", name ? name : "");
            set_msg(msg);
            break;
        case ALT_SIGN_IN_OK:
            if (name && !strcmp(g_form_user, name)) g_form_user[0] = 0;
            I18N_F(msg, "kn.alt.signedIn", "Signed in as {0}.", name ? name : "");
            set_msg(msg);
            break;
        case ALT_SIGN_IN_REFUSED:
            set_msg(error && error[0] ? error : I18N("kn.alt.failed", "Could not sign in."));
            break;
        case ALT_SIGN_IN_EXPIRED: // the session and the stored password are both refused: the row offers a Sign in
            set_msg(I18N("kn.alt.invalid", "Saved login is no longer valid. Sign in again to restore it."));
            break;
        case ALT_SIGN_IN_UNSAVED:
            set_msg(I18N("kn.alt.saveFailed", "Could not write the vault."));
            break;
    }
    rerender();
}

// ---------------------------------------------------------------- window

static void append_row(StrBuf *sb, const Row *r) {
    sb_append(sb, r->active ? "<div class='knAltRow knAltActive'>" : "<div class='knAltRow'>");
    sb_append(sb, "<div class='knAltName'>");
    sb_append_esc(sb, r->label);
    sb_append(sb, "</div>");
    sb_append(sb, r->kind == ALT_KIND_KN ? "<div class='knAltTag knAltTagKn'>krunk.io</div>"
                                         : "<div class='knAltTag knAltTagKio'>krunker.io</div>");
    if (r->active) {
        sb_append(sb, "<div class='knAltBtn knAltOn'>");
        sb_append_esc(sb, I18N("kn.alt.use", "Use"));
        sb_append(sb, "</div>");
    } else if (r->has_secret || r->has_pass) {
        sb_append(sb, "<div class='knAltBtn knAltUseBtn' onclick='knAltUse(\"");
        sb_append_esc(sb, r->id);
        sb_append(sb, "\")'>");
        sb_append_esc(sb, I18N("kn.alt.use", "Use"));
        sb_append(sb, "</div>");
    }
    if (!r->active && !r->has_secret && !r->has_pass) { // nothing left to sign in with: the row asks for a sign-in (the form)
        sb_append(sb, "<div class='knAltBtn knAltUseBtn' onclick='knAltSignInRow(\"");
        sb_append_esc(sb, r->id);
        sb_append(sb, "\")'>");
        sb_append_esc(sb, I18N("kn.alt.signIn", "Sign in"));
        sb_append(sb, "</div>");
    }
    sb_append(sb, "<div class='knAltBtn knAltDanger' onclick='knAltRemove(\"");
    sb_append_esc(sb, r->id);
    sb_append(sb, "\")'>");
    sb_append_esc(sb, I18N("kn.alt.remove", "Remove"));
    sb_append(sb, "</div></div>");
}

#ifndef __EMSCRIPTEN__ // the backend chips of the Add form (native only: the web's Accounts are krunk.io's)
static void append_chip(StrBuf *sb, const int kind, const char *text) {
    sb_append(sb, kind == g_form_kind ? "<div class='knAltChip knAltChipOn' " : "<div class='knAltChip' ");
    sb_appendf(sb, "onclick='knAltBackend(%d)'>", kind == ALT_KIND_KN ? 0 : 1);
    sb_append_esc(sb, text);
    sb_append(sb, "</div>");
}
#endif

char *alt_manager_window_gen(const int id, const char **header) {
    if (id != ALT_MANAGER_WINDOW) return NULL;
    if (header) *header = I18N("kn.alt.title", "Accounts");
    alt_store_open(NULL); // idempotent; reads no key without a vault file
    Row rows[ROWS_MAX];
    const int n = build_rows(rows);
    const int have = g_scenario != NULL || alt_store_vault() != NULL;
    StrBuf sb = {0};
    // v7 hides #windowHeader: the title is part of the window (as the Object Manager's is)
    sb_append(&sb, "<div class='knAlt'><div class='knAltTitle'>");
    sb_append_esc(&sb, I18N("kn.alt.title", "Accounts"));
    sb_append(&sb, "</div>");
    if (!have) {
        sb_append(&sb, "<div class='knAltWarn'>");
        sb_append_esc(&sb, I18N("kn.alt.unavailable", "Saved accounts are not available."));
        sb_append(&sb, "</div>");
    } else {
        sb_append(&sb, "<div class='knAltList'>");
        if (!n) {
            sb_append(&sb, "<div class='knAltEmpty'>");
            sb_append_esc(&sb, I18N("kn.alt.empty", "No saved accounts yet."));
            sb_append(&sb, "</div>");
        }
        for (int i = 0; i < n; i++) append_row(&sb, &rows[i]);
        sb_append(&sb, "</div>");
        if (in_official_game() && !active_account()) { // an official krunker.io guest: v10's login modal signs in (its own screens)
            sb_append(&sb, "<div class='joinBtn knAltWide' id='knAltSignIn' onclick='playSelect(0.1);loginOrRegister()'>");
            sb_append_esc(&sb, I18N("generic.login", "Login"));
            sb_append(&sb, "</div>");
        }
        if (krunker_account_state() == KRUNKER_ACCOUNT_CODE) { // the krunker.io code step (2FA or email)
            sb_append(&sb, "<div class='knAltAdd'><input id='knAltCode' type='text' class='accountInput' autocomplete='one-time-code' placeholder='");
            sb_append_esc(&sb, I18N("kn.alt.code", "Code"));
            sb_append(&sb, "'><div class='joinBtn knAltWide' onclick='knAltCodeSubmit()'>");
            sb_append_esc(&sb, I18N("kn.alt.submit", "Submit"));
            sb_append(&sb, "</div></div>");
        } else {
            sb_append(&sb, "<div class='knAltAdd'>");
#ifndef __EMSCRIPTEN__ // the web's Accounts are krunk.io's only: no backend chips
            sb_append(&sb, "<div class='knAltChips'>");
            append_chip(&sb, ALT_KIND_KN, "krunk.io");
            append_chip(&sb, ALT_KIND_KRUNKER, "krunker.io");
            sb_append(&sb, "</div>");
#endif
            sb_append(&sb, "<input id='knAltUser' type='text' class='accountInput' placeholder='");
            sb_append_esc(&sb, I18N("generic.username", "Username"));
            sb_append(&sb, "' value='");
            sb_append_esc(&sb, g_form_user);
            sb_append(&sb, "'><input id='knAltPass' type='password' class='accountInput' placeholder='");
            sb_append_esc(&sb, I18N("generic.password", "Password"));
            sb_append(&sb, "'><div class='joinBtn knAltWide' onclick='knAltAdd()'>");
            sb_append_esc(&sb, I18N("kn.alt.add", "Add"));
            sb_append(&sb, "</div></div>");
        }
    }
    sb_append(&sb, "<div id='knAltMsg' class='knAltMsg'>");
    sb_append_esc(&sb, g_msg);
    sb_append(&sb, "</div></div>");
    return sb_finish(&sb);
}

void alt_manager_ui_scenario(const cJSON *rows) {
    cJSON_Delete(g_scenario);
    g_scenario = cJSON_IsArray(rows) ? cJSON_Duplicate(rows, 1) : NULL;
}

int alt_manager_ui_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) ev;
    if (!name || strncmp(name, "knAlt", 5) != 0) return -1;
    if (!strcmp(name, "knAltUse")) {
        const char *id = arg_text(args, argc, 0);
        if (id) use_row(id);
    } else if (!strcmp(name, "knAltSignInRow")) { // a row with nothing left to sign in with: the form takes its login
        const char *id = arg_text(args, argc, 0);
        const AltProfile *p = NULL;
        if (id && find_profile(id, &p)) {
            snprintf(g_form_user, sizeof(g_form_user), "%s", p->label);
            g_form_kind = p->kind;
            set_msg("");
        }
    } else if (!strcmp(name, "knAltRemove")) {
        const char *id = arg_text(args, argc, 0);
        if (id) remove_row(id);
    } else if (!strcmp(name, "knAltBackend")) {
        char typed[ALT_LABEL_MAX];
        read_input("knAltUser", typed, sizeof(typed)); // the login typed so far stays
        snprintf(g_form_user, sizeof(g_form_user), "%s", typed);
        g_form_kind = ALT_KIND_KN; // the web's Accounts are krunk.io's only (no chip picks krunker.io there)
#ifndef __EMSCRIPTEN__
        if (argc > 0 && args[0].kind == HTML_ARG_NUM && args[0].num != 0) g_form_kind = ALT_KIND_KRUNKER;
#endif
    } else if (!strcmp(name, "knAltCodeSubmit")) {
        char code[32];
        read_input("knAltCode", code, sizeof(code));
        submit_code(code);
        alt_wipe(code, sizeof(code));
    } else if (!strcmp(name, "knAltAdd")) {
        char user[ALT_LABEL_MAX], pass[128];
        read_input("knAltUser", user, sizeof(user));
        read_input("knAltPass", pass, sizeof(pass));
        add_account(user, pass);
        alt_wipe(pass, sizeof(pass));
    } else {
        return -1;
    }
    rerender();
    return 0;
}
