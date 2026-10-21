// The Account Manager window (client/include/alt_manager_ui.h): the accounts the player listed, both
// backends in one list. Each row is one account: its name, a tag (krunk.io: a Box Office account; krunker.io:
// an official account), and Use / Remove. A row keeps its login (the name and the password, sealed in the alt
// vault) and, once the player has signed it in, its session. Add stores the typed login and password under the
// chosen backend and lists the account: nothing is signed in, nothing is switched, no request is sent. Use fills
// the login card's Username and Password with the row's saved ones and stops there: the player presses Login
// (the card's own sign-in, v10_login.c).
//
// Use never refuses: over a signed-in account, or in an official game, the card still opens with the row's login.
// Login is the player's press, and the card signs in as it does for any login (v10_login.c).
#include <alt_manager_ui.h>

#include <alt_crypto.h>
#include <alt_store.h>
#include <backend_ui.h>
#include <krunker_account.h>
#include <ui_logic.h>
#include <v10_menu.h>

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define ROWS_MAX ALT_MAX_PROFILES

// The last result, shown under the list. Never a token or a credential.
static char g_msg[200];
// The Add form: the login name (kept when an Add fails) and the backend its chips pick (ALT_KIND_KN or ALT_KIND_KRUNKER).
static char g_form_user[ALT_LABEL_MAX];
static int g_form_kind = ALT_KIND_KN;
// render scenarios: state.altAccounts stands in for the vault's rows (no vault, no actions)
static cJSON *g_scenario;

typedef struct {
    char id[ALT_ID_LEN];
    char label[ALT_LABEL_MAX];
    int kind;       // ALT_KIND_KN (krunk.io) | ALT_KIND_KRUNKER (krunker.io)
    int active;     // the account the client plays as now
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

// Stores the krunker.io account's current refresh token under its row's label (the same account's new one).
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

// krunker.io's sign-in and session events (krunker_account.h). A listed account's stored session follows the
// account; the login card (v10_login.c) reports its own sign-in.
static void on_kio_event(void *user, const KrunkerAccountEvent ev) {
    (void) user;
    switch (ev) {
        case KRUNKER_EV_SIGNED_IN:
        case KRUNKER_EV_REFRESHED:
            sync_kio(krunker_account_name());
            break;
        case KRUNKER_EV_EXPIRED: { // the stored session is refused: forgotten (the row keeps its login)
            char name[ALT_LABEL_MAX];
            snprintf(name, sizeof(name), "%s", krunker_account_name());
            forget_kio(name);
            break;
        }
        default:
            break;
    }
    rerender();
}

void alt_manager_ui_init(void) {
    krunker_account_listen_events(on_kio_event, NULL);
}

// ---------------------------------------------------------------- actions

// Use: the login card opens over the window with the row's Username and its saved Password in the fields, and
// nothing else happens: no sign-in, no session, no request, no refusal. Over a signed-in account, or in an official
// game, the card opens too (v10_login_fill is the whole of it). The player presses Login. A krunker.io row's card
// signs in to krunker.io, a krunk.io row's to krunk.io's account service (v10_login.c).
static void use_row(const char *id) {
    const AltProfile *p = NULL;
    if (!find_profile(id, &p)) {
        set_msg(I18N("kn.alt.gone", "That account is no longer saved."));
        return;
    }
    char name[ALT_LABEL_MAX], pass[ALT_SECRET_MAX];
    snprintf(name, sizeof(name), "%s", p->label);
    snprintf(pass, sizeof(pass), "%s", p->pass);
    const int shown = v10_login_fill(p->kind == ALT_KIND_KRUNKER, name, pass);
    alt_wipe(pass, sizeof(pass));
    if (!shown) set_msg(I18N("kn.alt.noCard", "The login card is not open here."));
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

// Add: the typed login and password are stored under the backend the chips pick, and the account is listed. Nothing
// is signed in, no account is switched, and no request leaves the client. The password is sealed in the alt vault.
static void add_account(const char *name, const char *pass) {
    snprintf(g_form_user, sizeof(g_form_user), "%s", name ? name : "");
    if (!name || !name[0] || !pass || !pass[0]) {
        set_msg(I18N("kn.alt.needCreds", "Enter the login and the password."));
        return;
    }
    // a krunker.io login is case-blind: the row it already has keeps its label
    const AltProfile *row = g_form_kind == ALT_KIND_KRUNKER ? kio_profile(name) : NULL;
    char label[ALT_LABEL_MAX];
    snprintf(label, sizeof(label), "%s", row ? row->label : name);
    if (alt_store_list(g_form_kind, label, NULL, pass, NULL) != 0) {
        set_msg(I18N("kn.alt.saveFailed", "Could not write the vault."));
        return;
    }
    char msg[sizeof(g_msg)];
    I18N_F(msg, "kn.alt.added", "Added {0}.", label);
    set_msg(msg);
    g_form_user[0] = 0;
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
        g_form_kind = ALT_KIND_KN; // the web's Accounts are krunk.io's only (no chip picks krunker.io there)
#ifndef __EMSCRIPTEN__
        if (argc > 0 && args[0].kind == HTML_ARG_NUM && args[0].num != 0) g_form_kind = ALT_KIND_KRUNKER;
#endif
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
