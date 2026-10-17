// The Account Manager window (client/include/alt_manager_ui.h): the accounts the player listed, both
// backends in one list. Each row is one account: its name, a tag (krunk.io: a Box Office account, the
// session in the alt vault; krunker.io: an official account, the refresh token in the vault), and Use /
// Remove. Add signs in with a typed login and password to the backend the chips pick. The sign-ins are
// the accounts' own: account_ui.c's loginAcc request for krunk.io, krunker_account_sign_in for krunker.io.
//
// Switching accounts (Add, Use) is refused in an official game: the game plays on the krunker.io account.
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

// The form asks for this account's password (a listed account whose session is gone).
static void ask_password(const char *label, const int kind) {
    snprintf(g_form_user, sizeof(g_form_user), "%s", label);
    g_form_kind = kind;
    char msg[sizeof(g_msg)];
    I18N_F(msg, "kn.alt.enterPass", "Enter the password for {0}.", label);
    set_msg(msg);
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
        }
        return n;
    }
    AltVault *v = alt_store_vault();
    if (!v) return 0;
    for (int i = 0; i < v->count && n < ROWS_MAX; i++) {
        const AltProfile *p = &v->profiles[i];
        Row *r = &rows[n++];
        memset(r, 0, sizeof(*r));
        snprintf(r->id, sizeof(r->id), "%s", p->id);
        snprintf(r->label, sizeof(r->label), "%s", p->label);
        r->kind = p->kind;
        r->has_secret = p->secret[0] != 0;
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
    char msg[sizeof(g_msg)];
    switch (ev) {
        case KRUNKER_EV_SIGNED_IN:
            if (mine && g_pending == PEND_ADD) {
                if (store_kio(krunker_account_name()) == 0) {
                    I18N_F(msg, "kn.alt.signedIn", "Signed in as {0}.", krunker_account_name());
                    set_msg(msg);
                    if (!strcmp(g_form_user, g_pending_name)) g_form_user[0] = 0;
                } else {
                    set_msg(I18N("kn.alt.saveFailed", "Could not write the vault."));
                }
            } else {
                sync_kio(krunker_account_name());
            }
            break;
        case KRUNKER_EV_SIGN_IN_FAILED:
            if (mine && g_pending == PEND_ADD) set_msg(krunker_account_message()[0] ? krunker_account_message() : I18N("kn.alt.failed", "Could not sign in."));
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
        case KRUNKER_EV_EXPIRED: // the stored session is over: the row asks for its password
            forget_kio(krunker_account_name());
            if (mine && g_pending == PEND_USE) ask_password(g_pending_name, ALT_KIND_KRUNKER);
            break;
        case KRUNKER_EV_REFRESHED:
            sync_kio(krunker_account_name());
            break;
        case KRUNKER_EV_CODE_ASKED: // the code step: its instruction is krunker_account_message()
            if (mine && g_pending == PEND_ADD) set_msg(krunker_account_message());
            break;
    }
    if (mine && (ev == KRUNKER_EV_SIGNED_IN || ev == KRUNKER_EV_SIGN_IN_FAILED || ev == KRUNKER_EV_RESUMED ||
                 ev == KRUNKER_EV_RESUME_FAILED || ev == KRUNKER_EV_EXPIRED))
        g_pending = PEND_NONE;
    rerender();
}

void alt_manager_ui_init(void) {
    krunker_account_listen_events(on_kio_event, NULL);
}

// ---------------------------------------------------------------- actions

// Use: a listed account signs in with its stored session (the service checks it). Without one the form takes
// the password. krunk.io: account_ui.c's switch (its answer comes to alt_manager_ui_sign_in_done); krunker.io:
// krunker_account_resume (its answer comes as a krunker.io event).
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
    if (!p->secret[0]) {
        ask_password(p->label, p->kind);
        return;
    }
    char name[ALT_LABEL_MAX], token[ALT_SECRET_MAX];
    snprintf(name, sizeof(name), "%s", p->label);
    snprintf(token, sizeof(token), "%s", p->secret);
    char msg[sizeof(g_msg)];
    if (p->kind == ALT_KIND_KN) {
        const int started = account_ui_use_session(name, token);
        alt_wipe(token, sizeof(token));
        if (started != 0) {
            set_msg(I18N("kn.alt.noServer", "Can't reach the account server."));
            return;
        }
    } else {
        const int started = krunker_account_resume(name, token);
        alt_wipe(token, sizeof(token));
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

// Add: the typed login and password sign in to the chosen backend. The password is wiped by the caller and
// never kept here (both sign-ins take it once).
// The code step's code (the 2FA app's or the email's): the same submit as v10's login modal.
static void submit_code(const char *code) {
    if (!code || !code[0]) return;
    if (!krunker_account_submit_code(code)) set_msg(krunker_account_message()[0] ? krunker_account_message() : I18N("kn.alt.failed", "Could not sign in."));
}

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
        if (!krunker_account_sign_in(name, pass)) {
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
        case ALT_SIGN_IN_OK:
            if (name && !strcmp(g_form_user, name)) g_form_user[0] = 0;
            I18N_F(msg, "kn.alt.signedIn", "Signed in as {0}.", name ? name : "");
            set_msg(msg);
            break;
        case ALT_SIGN_IN_REFUSED:
            set_msg(error && error[0] ? error : I18N("kn.alt.failed", "Could not sign in."));
            break;
        case ALT_SIGN_IN_EXPIRED:
            ask_password(name ? name : "", ALT_KIND_KN);
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
    } else {
        sb_append(sb, "<div class='knAltBtn knAltUseBtn' onclick='knAltUse(\"");
        sb_append_esc(sb, r->id);
        sb_append(sb, "\")'>");
        sb_append_esc(sb, I18N("kn.alt.use", "Use"));
        sb_append(sb, "</div>");
    }
    sb_append(sb, "<div class='knAltBtn knAltDanger' onclick='knAltRemove(\"");
    sb_append_esc(sb, r->id);
    sb_append(sb, "\")'>");
    sb_append_esc(sb, I18N("kn.alt.remove", "Remove"));
    sb_append(sb, "</div></div>");
}

static void append_chip(StrBuf *sb, const int kind, const char *text) {
    sb_append(sb, kind == g_form_kind ? "<div class='knAltChip knAltChipOn' " : "<div class='knAltChip' ");
    sb_appendf(sb, "onclick='knAltBackend(%d)'>", kind == ALT_KIND_KN ? 0 : 1);
    sb_append_esc(sb, text);
    sb_append(sb, "</div>");
}

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
            sb_append(&sb, "<div class='knAltAdd'><div class='knAltChips'>");
            append_chip(&sb, ALT_KIND_KN, "krunk.io");
            append_chip(&sb, ALT_KIND_KRUNKER, "krunker.io");
            sb_append(&sb, "</div><input id='knAltUser' type='text' class='accountInput' placeholder='");
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
    } else if (!strcmp(name, "knAltRemove")) {
        const char *id = arg_text(args, argc, 0);
        if (id) remove_row(id);
    } else if (!strcmp(name, "knAltBackend")) {
        char typed[ALT_LABEL_MAX];
        read_input("knAltUser", typed, sizeof(typed)); // the login typed so far stays
        snprintf(g_form_user, sizeof(g_form_user), "%s", typed);
        g_form_kind = argc > 0 && args[0].kind == HTML_ARG_NUM && args[0].num != 0 ? ALT_KIND_KRUNKER : ALT_KIND_KN;
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
