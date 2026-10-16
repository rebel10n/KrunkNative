// The krunker.io account's UI (client/include/krunker_account.h): the "krunker.io account" modal
// (sign in, the 2FA / email code step, sign out) and the "Accounts" line that says which account each
// server type sees (the Servers window, the Account window's Settings tab).
//
// The modal is built on the page's <body> like the Security Challenge one (krunker_captcha_ui.c) and
// wears its look (.kn-captcha-*, ui/css/krunknative.css), one level below it (.kn-kio-account-holder):
// a username sign-in shows the challenge on top of it. Its buttons call the one page handler
// knKioAccount(n); kioAccount() opens it (the Accounts line's buttons).
#include <krunker_account.h>
#include <backend_ui.h>
#include <html_dom.h>
#include <html_ui.h>
#include <ui_logic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HANDLER "knKioAccount"

enum { // knKioAccount(n)
    ACT_SIGN_IN = 2,
    ACT_CODE = 3,
    ACT_CANCEL = 4,
    ACT_SIGN_OUT = 5,
    ACT_CLOSE = 6,
};

static struct {
    int open;
    int handlers_set;
    unsigned serial; // html_ui_page_serial() the handlers went in at
    unsigned seen_generation;
    int shown_state;
} U = {.shown_state = -1};

static int handle(void *user, const HtmlEvent *ev, const HtmlArg *args, int argc);

// The game page's handlers (each page has its own table: installed again after a page switch).
static void ensure_handlers(void) {
    if (!html_ui_available() || !html_ui_is_ready() || strcmp(html_ui_current_page(), "index.html")) return;
    if (U.handlers_set && U.serial == html_ui_page_serial()) return;
    html_ui_add_handler(HANDLER, handle);
    html_ui_add_handler("kioAccount", handle);
    U.handlers_set = 1;
    U.serial = html_ui_page_serial();
}

// ---------------------------------------------------------------- the Accounts line

// "KrunkNative servers: <name> | krunker.io servers: <name>" with a sign-in / sign-out button each.
void krunker_account_ui_accounts_html(StrBuf *sb) {
    ensure_handlers();
    const AccountInfo *kn = account_get();
    sb_append(sb, "<div id='knAccounts' class='kn-accounts'>");
    sb_append(sb, "<span class='kn-accounts-row' id='knAccountsKn'><span class='kn-accounts-kind'>");
    sb_append_esc(sb, I18N("kn.accounts.knServers", "Box Office servers:"));
    sb_append(sb, "</span> ");
    if (kn && kn->name[0]) {
        sb_append(sb, "<span class='kn-accounts-name'>");
        sb_append_esc(sb, kn->name);
        sb_append(sb, "</span>");
    } else {
        sb_append(sb, "<span class='kn-accounts-guest'>");
        sb_append_esc(sb, I18N("kn.accounts.guest", "Guest"));
        sb_append(sb, "</span> <a class='kn-accounts-btn' onclick='playSelect(0.1);showWindow(5)'>");
        sb_append_esc(sb, I18N("generic.login", "Login"));
        sb_append(sb, "</a>");
    }
    sb_append(sb, "</span><span class='kn-accounts-row' id='knAccountsKio'><span class='kn-accounts-kind'>");
    sb_append_esc(sb, I18N("kn.accounts.kioServers", "krunker.io servers:"));
    sb_append(sb, "</span> ");
    if (krunker_account_signed_in()) {
        sb_append(sb, "<span class='kn-accounts-name' id='knAccountsKioName'>");
        sb_append_esc(sb, krunker_account_name()[0] ? krunker_account_name() : I18N("kn.accounts.signedIn", "signed in"));
        sb_append(sb, "</span>");
        if (krunker_account_level() > 0) sb_appendf(sb, " <span class='kn-accounts-level'>%s %d</span>", I18N("generic.level", "Level"), krunker_account_level());
        sb_append(sb, " <a class='kn-accounts-btn' id='knAccountsKioOut' onclick='playSelect(0.1);" HANDLER "(5)'>");
        sb_append_esc(sb, I18N("generic.logout", "Logout"));
        sb_append(sb, "</a>");
    } else {
        sb_append(sb, "<span class='kn-accounts-guest' id='knAccountsKioName'>");
        sb_append_esc(sb, I18N("kn.accounts.guest", "Guest"));
        sb_append(sb, "</span> <a class='kn-accounts-btn' id='knAccountsKioIn' onclick='playSelect(0.1);kioAccount()'>");
        sb_append_esc(sb, I18N("generic.login", "Login"));
        sb_append(sb, "</a>");
    }
    sb_append(sb, "</span></div>");
}

// ---------------------------------------------------------------- the modal

static char *markup(void) {
    StrBuf sb = {0};
    sb_appendf(&sb,
               "<div id=\"knKioAccountHolder\" class=\"kn-captcha-holder kn-kio-account-holder\" style=\"display:none\">"
               "<div id=\"knKioAccountModal\" class=\"kn-captcha-modal\">"
               "<div class=\"kn-captcha-title\">%s</div>"
               "<div id=\"knKioAccountText\" class=\"kn-captcha-text\"></div>",
               I18N("kn.kio.account.title", "krunker.io account"));
    // signed out: the login form (username or email + password)
    sb_appendf(&sb,
               "<div id=\"knKioAccountForm\" class=\"kn-captcha-block\" style=\"display:none\">"
               "<input id=\"knKioLogin\" class=\"kn-captcha-input kn-kio-input\" type=\"text\" placeholder=\"%s\" autocomplete=\"off\" spellcheck=\"false\">"
               "<input id=\"knKioPassword\" class=\"kn-captcha-input kn-kio-input\" type=\"password\" placeholder=\"%s\" autocomplete=\"off\">"
               "<div class=\"kn-captcha-small\">%s</div>"
               "</div>",
               I18N("kn.kio.account.loginPlaceholder", "Username or email"), I18N("kn.kio.account.passwordPlaceholder", "Password"),
               I18N("kn.kio.account.note", "Your own krunker.io account. The password goes to krunker.io only and is never stored."));
    // the code step
    sb_appendf(&sb,
               "<div id=\"knKioAccountCodeRow\" class=\"kn-captcha-block\" style=\"display:none\">"
               "<input id=\"knKioCode\" class=\"kn-captcha-input kn-kio-input\" type=\"text\" placeholder=\"%s\" autocomplete=\"off\" spellcheck=\"false\">"
               "</div>",
               I18N("kn.kio.account.codePlaceholder", "Code"));
    sb_append(&sb, "<div id=\"knKioAccountStatus\" class=\"kn-captcha-status\"></div><div class=\"kn-captcha-row kn-captcha-actions\">");
    sb_appendf(&sb, "<div id=\"knKioSignIn\" class=\"kn-captcha-btn\" onclick=\"" HANDLER "(2)\" style=\"display:none\">%s</div>", I18N("generic.login", "Login"));
    sb_appendf(&sb, "<div id=\"knKioSubmitCode\" class=\"kn-captcha-btn\" onclick=\"" HANDLER "(3)\" style=\"display:none\">%s</div>", I18N("generic.submit", "Submit"));
    sb_appendf(&sb, "<div id=\"knKioSignOut\" class=\"kn-captcha-btn\" onclick=\"" HANDLER "(5)\" style=\"display:none\">%s</div>", I18N("generic.logout", "Logout"));
    sb_appendf(&sb, "<div id=\"knKioCancel\" class=\"kn-captcha-btn kn-captcha-cancel\" onclick=\"" HANDLER "(4)\" style=\"display:none\">%s</div>", I18N("generic.cancel", "Cancel"));
    sb_appendf(&sb, "<div id=\"knKioClose\" class=\"kn-captcha-btn kn-captcha-cancel\" onclick=\"" HANDLER "(6)\">%s</div>", I18N("generic.close", "Close"));
    sb_append(&sb, "</div></div></div>");
    return sb_finish(&sb);
}

static HtmlElem holder(void) {
    if (!html_ui_available() || !html_ui_is_ready()) return 0;
    ensure_handlers();
    HtmlElem h = html_dom_by_id("knKioAccountHolder");
    if (!h) {
        const HtmlElem body = html_dom_body();
        if (!body) return 0;
        char *html = markup();
        html_dom_insert_html(body, HTML_INSERT_BEFORE_END, html);
        free(html);
        h = html_dom_by_id("knKioAccountHolder");
    }
    return h;
}

static void show(const char *id, const int on) {
    const HtmlElem el = html_dom_by_id(id);
    if (el) html_dom_set_display(el, on ? "" : "none");
}

static void set_text(const char *id, const char *text) {
    const HtmlElem el = html_dom_by_id(id);
    if (el) html_dom_set_text(el, text ? text : "");
}

// The modal for the account's state.
static void render(void) {
    const HtmlElem h = holder();
    if (!h) return;
    html_dom_set_display(h, U.open ? "flex" : "none");
    if (!U.open) return;
    const KrunkerAccountState st = krunker_account_state();
    char text[300];
    if (st == KRUNKER_ACCOUNT_SIGNED_IN) {
        if (krunker_account_level() > 0) {
            char level[16];
            snprintf(level, sizeof(level), "%d", krunker_account_level());
            I18N_F(text, "kn.kio.account.signedInLevel", "Signed in as {0} (level {1}): krunker.io servers see this account.", krunker_account_name(), level);
        } else {
            I18N_F(text, "kn.kio.account.signedInAs", "Signed in as {0}: krunker.io servers see this account.", krunker_account_name());
        }
    } else if (st == KRUNKER_ACCOUNT_CODE) {
        snprintf(text, sizeof(text), "%s", !strcmp(krunker_account_code_kind(), "2fa") ? I18N("kn.kio.account.ask2fa", "Enter the 6-digit code from your authenticator app.")
                                                                                         : I18N("kn.kio.account.askEmailCode", "Enter the code krunker.io sent to your email."));
    } else if (st == KRUNKER_ACCOUNT_BUSY) {
        snprintf(text, sizeof(text), "%s", I18N("generic.wait", "Please wait..."));
    } else {
        snprintf(text, sizeof(text), "%s", I18N("kn.kio.account.guestText", "Sign in to play on krunker.io servers with your krunker.io account (name, level and loadout). Not signed in, you join them as a guest."));
    }
    set_text("knKioAccountText", text);
    show("knKioAccountForm", st == KRUNKER_ACCOUNT_GUEST);
    show("knKioAccountCodeRow", st == KRUNKER_ACCOUNT_CODE);
    show("knKioSignIn", st == KRUNKER_ACCOUNT_GUEST);
    show("knKioSubmitCode", st == KRUNKER_ACCOUNT_CODE);
    show("knKioSignOut", st == KRUNKER_ACCOUNT_SIGNED_IN);
    show("knKioCancel", st == KRUNKER_ACCOUNT_BUSY || st == KRUNKER_ACCOUNT_CODE);
    const char *msg = krunker_account_message();
    // (the state's own sentence is in the text: the status line shows outcomes)
    set_text("knKioAccountStatus", st == KRUNKER_ACCOUNT_BUSY || st == KRUNKER_ACCOUNT_CODE ? "" : msg);
    const HtmlElem status = html_dom_by_id("knKioAccountStatus");
    if (status) html_dom_set_style(status, "color", krunker_account_message_is_error() ? "#e53935" : "");
    if (U.shown_state != (int) st) {
        if (st == KRUNKER_ACCOUNT_CODE) {
            const HtmlElem code = html_dom_by_id("knKioCode");
            if (code) {
                html_dom_set_value(code, "");
                html_dom_focus(code);
            }
        }
        U.shown_state = (int) st;
    }
}

void krunker_account_ui_open(void) {
    U.open = 1;
    U.shown_state = -1;
    render();
    const HtmlElem login = html_dom_by_id("knKioLogin");
    if (login && krunker_account_state() == KRUNKER_ACCOUNT_GUEST) html_dom_focus(login);
}

static void read_value(const char *id, char *out, const size_t n) {
    out[0] = 0;
    const HtmlElem el = html_dom_by_id(id);
    if (el) html_dom_get_value(el, out, n);
}

static void clear_value(const char *id) {
    const HtmlElem el = html_dom_by_id(id);
    if (el) html_dom_set_value(el, "");
}

static void wipe(void *p, size_t n) {
    volatile unsigned char *v = p;
    while (n--) *v++ = 0;
}

static int handle(void *user, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) user;
    (void) ev;
    const int act = argc > 0 && args[0].kind == HTML_ARG_NUM ? (int) args[0].num : 1;
    switch (act) {
        case ACT_SIGN_IN: {
            char login[256], password[256];
            read_value("knKioLogin", login, sizeof(login));
            read_value("knKioPassword", password, sizeof(password));
            clear_value("knKioPassword"); // the field never keeps it
            // trim the login (a pasted name may carry spaces)
            char *l = login;
            while (*l == ' ') l++;
            size_t len = strlen(l);
            while (len && l[len - 1] == ' ') l[--len] = 0;
            krunker_account_sign_in(l, password);
            wipe(password, sizeof(password));
            break;
        }
        case ACT_CODE: {
            char code[32];
            read_value("knKioCode", code, sizeof(code));
            clear_value("knKioCode");
            krunker_account_submit_code(code);
            wipe(code, sizeof(code));
            break;
        }
        case ACT_CANCEL: krunker_account_cancel(); break;
        case ACT_SIGN_OUT: krunker_account_sign_out(); break;
        case ACT_CLOSE:
            if (krunker_account_state() == KRUNKER_ACCOUNT_CODE) krunker_account_cancel();
            U.open = 0;
            break;
        default: krunker_account_ui_open(); return 1;
    }
    render();
    menu_rerender_if_open(2); // the Servers window's Accounts line
    return 1;
}

// Main thread, once per frame: the modal and the Accounts line follow the account.
void krunker_account_ui_tick(void) {
    ensure_handlers();
    const unsigned gen = krunker_account_generation();
    if (gen == U.seen_generation) return;
    U.seen_generation = gen;
    if (U.open) render();
    menu_rerender_if_open(2);
}
