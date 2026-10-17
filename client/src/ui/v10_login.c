// Krunker v10.0.2 login / register modal: loginOrRegister(defaultToRegister) (v10:210151) pushes
// the Svelte component _39035 (v10:114780) onto the view stack (_5217; the stack host _40140 /
// _40139, v10:117090): .view-stack > .view-stack-overlay (click outside = close) > .view-wrapper
// (scaled like #uiBase through --ui-scale / --ui-width / --ui-height) > .popup-container > .popup
// (width 800) > .io-popup-container with the header (Login to Krunker / Create Account / Recover
// Account), the "New to Krunker? Register here" switch with the sign-up bonuses, the form card
// and the .io-message line. Signed in, loginOrRegister() is showWindow(5).
//
// The login card is v10's: one field, "Email" (#accEmail, "Enter Email") or "Username" (#accName,
// "Enter Username") with the "(use username instead)" / "(use email instead)" toggle (_39146),
// Email first unless krunker_username is saved (_39221); an '@' typed as a username offers "Switch
// to email login" (_39233). In Email mode the password field stays hidden (.hidden-password) and its
// "Password / Forgot Password?" row out until a password is typed or asked for. Either field takes a
// username or an email: kn_brain signs in to the account holding the address (accounts.c).
//
// It signs in to the ACTIVE BACKEND's account (backend_ui.h active_account()): krunker.io's in an
// official game, krunk.io's elsewhere. There is no other login or account modal.
//
// krunker.io (krunker_account.h): v10's own flow. Login posts the username (after the Security
// Challenge, krunker_captcha.h) or the email with the password; an Email with no password offers
// v10's "Send Email Code" / "Use Password" (loginEmailCode); a 2FA or email code is asked with v10's
// generic form (_6696: "Two-Factor Authentication" / "Enter your code", six code boxes, not
// dismissable); the recover card posts recoverAccount. Register: this client creates no krunker.io
// account, so the register card (v10's, Email only) says to create it on krunker.io.
//
// krunk.io runs on KrunkNative's account service (account_ui.c: the social socket's sendAccountRequest,
// the replies written into #accResp, the forgot-password flow). Differences from v10, because
// that service is what it is (doc/v10/ui.md):
//   - accounts sign in with a password: Login with an email and no password goes straight to the
//     password (v10 offers "Send Email Code" / "Use Password" there; there is no email code), and
//     the Twitch-link line is not offered
//   - registering takes a username and a password besides the email (v10 registers by email and
//     mails a code): the card is v10's Email field first, then Username and Password. The email is
//     required as in v10 ("Please enter your email to register."): the forgot-password flow mails
//     it. The news opt-in is shown but nothing subscribes to it (KrunkNative sends no newsletters)
//   - "Forgot Password?" -> the recover card -> Submit continues in the Account window's reset
//     steps (account_ui.c: code by email, new password) instead of v10's emailed link
#include <v10_menu.h>
#include <backend_ui.h>
#include <krunker_account.h>
#include <html_ui.h>
#include <ui_logic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct {
    int open;
    int reg;      // _39220: the register card
    int recover;  // _39222: the recover card
    int email;    // _39221: the login card's field is Email (else Username)
    int use_pass; // _39235: Email mode, the password asked for (shown before one is typed)
    int to_reset; // recover(): showWindow(5) is the frame of the reset steps, not the modal
    int choice;   // _39234: krunker.io, Email with no password: "Send Email Code" / "Use Password"
    int code;     // krunker.io: the code form (_6696) is up
    unsigned kio_gen; // krunker_account_generation() the modal last followed
    int backend;      // account_backend() the modal signs in to
} L;

// The modal signs in to krunker.io (an official game is joined or played) instead of krunk.io
static int kio(void) { return account_backend() == ACCOUNT_BACKEND_KRUNKER; }

static HtmlElem by_id(const char *id) { return html_dom_by_id(id); }

int v10_menu_login_open(void) { return L.open && by_id("knV10Login") != 0; }

static int arg_true(const HtmlArg *args, const int argc) {
    return argc > 0 && (args[0].kind == HTML_ARG_BOOL || args[0].kind == HTML_ARG_NUM) && args[0].num != 0.0;
}

static void read_value(const char *id, char *out, const size_t n) {
    out[0] = 0;
    HtmlElem el = by_id(id);
    if (el) html_dom_get_value(el, out, n);
}

// document.body --ui-scale / --ui-width / --ui-height (v10 scaleBase sets them with #uiBase's
// transform): the view stack's wrapper scales like the menu
static void sync_ui_vars(HtmlElem stack) {
    HtmlElem base = by_id("uiBase");
    char transform[64] = "", width[32] = "", height[32] = "";
    if (!base || !stack) return;
    html_dom_get_style(base, "transform", transform, sizeof(transform));
    html_dom_get_style(base, "width", width, sizeof(width));
    html_dom_get_style(base, "height", height, sizeof(height));
    double scale = 1.0;
    if (sscanf(transform, "scale(%lf)", &scale) != 1 || scale <= 0.0) scale = 1.0;
    char value[32];
    snprintf(value, sizeof(value), "%.3f", scale);
    html_dom_set_style(stack, "--ui-scale", value);
    html_dom_set_style(stack, "--ui-width", width[0] ? width : "100%");
    html_dom_set_style(stack, "--ui-height", height[0] ? height : "100%");
}

// Email mode: the password field and its label row wait until a password is typed or asked for
static int pass_shown(const char *pass) { return L.reg || !L.email || L.use_pass || (pass && pass[0]); }

static void sync_pass(void) {
    char pass[128];
    read_value("accPass", pass, sizeof(pass));
    const int shown = pass_shown(pass);
    memset(pass, 0, sizeof(pass));
    HtmlElem el;
    if ((el = by_id("accPass"))) html_dom_set_class(el, "hidden-password", !shown);
    if ((el = by_id("knV10PassRow"))) html_dom_set_display(el, shown ? "flex" : "none");
}

// _39233: an '@' in the Username field offers the Email field
static void sync_hint(void) {
    char name[256];
    read_value("accName", name, sizeof(name));
    HtmlElem hint = by_id("knV10EmailHint");
    if (hint) html_dom_set_display(hint, !L.email && strchr(name, '@') ? "block" : "none");
}

static void append_value(StrBuf *sb, const char *v) {
    sb_append(sb, "\" value=\"");
    sb_append_esc(sb, v ? v : "");
    sb_append(sb, "\">");
}

// The login card (_39206): the Email or Username field, the password, Login
static void render_login(StrBuf *sb, const char *ident, const char *pass) {
    sb_append(sb, "<div class=\"io-card\"><form data-priority=\"true\"><div class=\"io-input-container svelte-16z1lxi\">");
    if (L.email) { // _39202
        sb_appendf(sb, "<div class=\"label-row\"><label for=\"accEmail\" class=\"input-label\">%s</label> <button type=\"button\" "
                       "class=\"auth-toggle-btn svelte-16z1lxi\" id=\"knV10LoginMode\" onclick=\"window.knV10LoginToggle()\">%s</button></div> "
                       "<input id=\"accEmail\" type=\"email\" class=\"io-input-field svelte-16z1lxi\" autocomplete=\"email\" placeholder=\"%s\" "
                       "onkeydown=\"knV10LoginKey()",
                   I18N("generic.email", "Email"), I18N("generic.auth.useUsername", "(use username instead)"), I18N("generic.auth.enterEmail", "Enter Email"));
        append_value(sb, ident);
    } else { // _39201 (+ _39200, the email suggestion)
        sb_appendf(sb, "<div class=\"label-row\"><label for=\"accName\" class=\"input-label\">%s</label> <button type=\"button\" "
                       "class=\"auth-toggle-btn svelte-16z1lxi\" id=\"knV10LoginMode\" onclick=\"window.knV10LoginToggle()\">%s</button></div> "
                       "<input id=\"accName\" type=\"text\" class=\"io-input-field svelte-16z1lxi\" autocomplete=\"username\" placeholder=\"%s\" "
                       "oninput=\"knV10LoginInput()\" onkeydown=\"knV10LoginKey()",
                   I18N("generic.username", "Username"), I18N("generic.auth.useEmail", "(use email instead)"), I18N("generic.auth.enterUsername", "Enter Username"));
        append_value(sb, ident);
        sb_appendf(sb, " <div class=\"email-suggestion svelte-16z1lxi\" id=\"knV10EmailHint\" style=\"display:%s\">%s "
                       "<button type=\"button\" class=\"suggestion-link svelte-16z1lxi\" onclick=\"window.knV10LoginToggle()\">%s</button> %s</div>",
                   ident && strchr(ident, '@') ? "block" : "none", I18N("generic.auth.emailSuggestion", "Looks like you're using an email."),
                   I18N("generic.auth.switchToEmail", "Switch to email login"), I18N("generic.auth.emailSuggestionEnd", "instead"));
    }
    const int shown = pass_shown(pass);
    // _39203: the label row (display toggled in place by sync_pass, so typing keeps the field)
    sb_appendf(sb, " <div class=\"label-row\" id=\"knV10PassRow\" style=\"margin-top:15px;display:%s\"><label for=\"accPass\" class=\"input-label\">%s</label> "
                   "<button type=\"button\" class=\"auth-toggle-btn svelte-16z1lxi\" id=\"knV10Forgot\" onclick=\"knV10LoginForgot()\">%s</button></div> "
                   "<input id=\"accPass\" type=\"password\" autocomplete=\"current-password\" placeholder=\"%s\" class=\"io-input-field %ssvelte-16z1lxi\" "
                   "oninput=\"knV10LoginInput()\" onkeydown=\"knV10LoginKey()",
               shown ? "flex" : "none", I18N("generic.password", "Password"), I18N("generic.auth.forgotPassword", "Forgot Password?"), I18N("generic.auth.enterPassword", "Enter Password"),
               shown ? "" : "hidden-password ");
    append_value(sb, pass);
    if (L.choice) // _39204: krunker.io's email code or the password
        sb_appendf(sb, " <div class=\"password-choice svelte-16z1lxi\"><div class=\"choice-buttons svelte-16z1lxi\"><button type=\"button\" "
                       "class=\"io-button io-button--accept-secondary svelte-16z1lxi\" id=\"knV10SendCode\" onclick=\"knV10LoginSendCode()\">%s</button> "
                       "<button type=\"button\" class=\"io-button io-button--accept svelte-16z1lxi\" id=\"knV10UsePass\" onclick=\"knV10LoginUsePass()\">%s</button>"
                       "</div></div></div></form></div>",
                   I18N("generic.auth.sendCode", "Send Email Code"), I18N("generic.auth.usePassword", "Use Password"));
    else
        sb_appendf(sb, " <button type=\"button\" class=\"io-button io-button--accept svelte-16z1lxi\" id=\"knV10LoginSubmit\" onclick=\"window.knV10LoginGo()\">%s</button>"
                       "</div></form></div>", I18N("generic.login", "Login"));
}

// The register card (_39207): v10's Email field, then KrunkNative's Username and Password, the
// news opt-in, Register and the terms line
static void render_register(StrBuf *sb, const char *mail, const char *name, const char *pass) {
    sb_appendf(sb, "<div class=\"io-card\"><form data-priority=\"true\"><div class=\"io-input-container svelte-16z1lxi\">"
                   "<div class=\"label-row\"><label for=\"accEmail\" class=\"input-label\">%s</label></div> "
                   "<input id=\"accEmail\" type=\"email\" class=\"io-input-field svelte-16z1lxi\" autocomplete=\"email\" placeholder=\"%s\" "
                   "onkeydown=\"knV10LoginKey()", I18N("generic.email", "Email"), I18N("generic.auth.enterEmail", "Enter Email"));
    append_value(sb, mail);
    if (!kio()) {
    sb_appendf(sb, " <div class=\"label-row\" style=\"margin-top:15px\"><label for=\"accName\" class=\"input-label\">%s</label></div> "
                   "<input id=\"accName\" type=\"text\" class=\"io-input-field svelte-16z1lxi\" autocomplete=\"username\" placeholder=\"%s\" "
                   "onkeydown=\"knV10LoginKey()", I18N("generic.username", "Username"), I18N("generic.auth.enterUsername", "Enter Username"));
    append_value(sb, name);
    sb_appendf(sb, " <div class=\"label-row\" style=\"margin-top:15px\"><label for=\"accPass\" class=\"input-label\">%s</label></div> "
                   "<input id=\"accPass\" type=\"password\" class=\"io-input-field svelte-16z1lxi\" autocomplete=\"new-password\" placeholder=\"%s\" "
                   "onkeydown=\"knV10LoginKey()", I18N("generic.password", "Password"), I18N("generic.auth.enterPassword", "Enter Password"));
    append_value(sb, pass);
    }
    sb_appendf(sb, " <label class=\"promo-optin svelte-16z1lxi\"><input type=\"checkbox\" class=\"svelte-16z1lxi\"> "
                   "<span>%s</span></label> "
                   "<button type=\"button\" class=\"io-button io-button--accept svelte-16z1lxi\" id=\"knV10LoginSubmit\" onclick=\"window.knV10LoginGo()\">%s</button> "
                   "<div class=\"terms-privacy-notice svelte-16z1lxi\">%s "
                   "<a class=\"svelte-16z1lxi\" onclick=\"openURL('https://frvr.com/legal/krunker#TermsofService')\">%s</a> %s "
                   "<a class=\"svelte-16z1lxi\" onclick=\"openURL('https://frvr.com/legal/krunker#PrivacyPolicy')\">%s</a></div>"
                   "</div></form></div>",
               I18N("emailPrefs.register.optIn", "Send me news about events, updates and season releases"), I18N("generic.register", "Register"),
               I18N("generic.auth.termsPrivacyNotice", "By registering, you agree to our"), I18N("generic.auth.terms", "Terms of Service"),
               I18N("generic.auth.and", "and"), I18N("generic.auth.privacy", "Privacy Policy"));
}

// name / mail / pass: what the fields hold now (Svelte keeps its $state across a re-render)
static void render_with(const char *name, const char *mail, const char *pass) {
    HtmlElem host = by_id("knV10Login");
    if (!host) return;
    StrBuf sb = {0};
    sb_append(&sb, "<div class=\"popup-container svelte-1wz7ymp\"><div class=\"popup svelte-1wz7ymp\" style=\"width: 800px;\" "
                   "onclick=\"knV10LoginStay()\"><div class=\"io-popup-container\"><div class=\"io-popup-header\">");
    sb_append(&sb, L.recover ? I18N("generic.auth.recover", "Recover Account") : L.reg ? I18N("generic.register.header", "Create Account")
                                                                                   : I18N("generic.login.header", "Login to Krunker"));
    sb_append(&sb, "</div> <div class=\"io-popup-content\">");
    if (!L.recover) {
        sb_appendf(&sb, "<div class=\"auth-actions svelte-16z1lxi\"><span class=\"svelte-16z1lxi\">%s</span> "
                        "<button type=\"button\" class=\"login-button svelte-16z1lxi\" id=\"knV10AuthSwitch\" onclick=\"knV10LoginSwitch()\">%s</button></div> ",
                   L.reg ? I18N("generic.auth.alreadyHaveAccount", "Already have an account?") : I18N("generic.auth.newToKrunker", "New to Krunker?"),
                   L.reg ? I18N("generic.auth.loginHere", "Login here") : I18N("generic.auth.registerHere", "Register here"));
        if (L.reg) sb_append(&sb, "<div class=\"sign-up-bonuses svelte-16z1lxi\"><span class=\"svelte-16z1lxi\">Sign up bonuses</span> <div>"
                                  "<li class=\"svelte-16z1lxi\"><span class=\"svelte-16z1lxi\">14 Cosmetics</span></li> "
                                  "<li class=\"svelte-16z1lxi\"><span class=\"svelte-16z1lxi\">24h of premium account</span></li></div></div>");
    }
    if (L.recover) {
        sb_appendf(&sb, "<div class=\"io-card\"><div class=\"io-input-container svelte-16z1lxi\"><div class=\"label-row\"><label for=\"recoveryEmail\" "
                        "class=\"input-label\">%s</label></div> <input id=\"recoveryEmail\" type=\"email\" class=\"io-input-field svelte-16z1lxi\" "
                        "autocomplete=\"email\" placeholder=\"%s\" onkeydown=\"knV10LoginKey()\"> <button type=\"button\" "
                        "class=\"io-button io-button--accept svelte-16z1lxi\" id=\"knV10RecoverSubmit\" onclick=\"knV10LoginRecover()\">%s</button></div></div> "
                        "<button type=\"button\" class=\"io-button\" id=\"knV10RecoverBack\" onclick=\"knV10LoginForgot()\">%s</button>",
                   I18N("generic.email", "Email"), I18N("generic.auth.enterEmail", "Enter Email"), I18N("generic.submit", "Submit"), I18N("generic.back", "Back"));
    } else if (L.reg) {
        render_register(&sb, mail, name, pass);
    } else {
        render_login(&sb, L.email ? mail : name, pass);
    }
    // {#if message !== ''} .io-message: account_ui.c's showAccResp writes #accResp
    sb_append(&sb, " <div class=\"io-message\" id=\"accResp\" style=\"display:none\"></div></div></div></div></div>");
    char *html = sb_finish(&sb);
    HtmlElem wrapper = html_dom_query(host, ".view-wrapper");
    if (wrapper) html_dom_set_html(wrapper, html);
    memset(html, 0, strlen(html)); // it held the password
    free(html);
    sync_ui_vars(host);
    const char *ident = L.email ? mail : name;
    const char *focus_id = L.recover ? "recoveryEmail"
                           : L.reg   ? "accEmail"
                           : ident && ident[0] && pass_shown(pass) ? "accPass"
                           : L.email ? "accEmail" : "accName";
    HtmlElem focus = by_id(focus_id);
    if (focus) html_dom_focus(focus);
}

static void render(void) {
    char name[256], mail[256], pass[128];
    read_value("accName", name, sizeof(name));
    read_value("accEmail", mail, sizeof(mail));
    read_value("accPass", pass, sizeof(pass));
    if (!name[0] && !mail[0] && !kio() && !account_saved_name(name, sizeof(name))) name[0] = 0; // getSavedVal("krunker_username"): the resumed account
    render_with(name, mail, pass);
    memset(pass, 0, sizeof(pass));
}

static void close_modal(void) {
    L.open = 0;
    L.recover = 0;
    L.choice = 0;
    L.code = 0;
    if (kio() && krunker_account_state() == KRUNKER_ACCOUNT_CODE) krunker_account_cancel(); // the code step ends with the modal
    HtmlElem host = by_id("knV10Login");
    if (host) html_dom_remove(host);
}

// the language changed while the popup is up: render it again in the new one (fields kept)
static void on_locale(void *user) {
    (void) user;
    if (L.open && by_id("knV10Login")) render();
}

// loginOrRegister(defaultToRegister)
static void open_modal(const int reg) {
    i18n_on_change(on_locale, NULL);
    if (active_account()) { // signed in: showWindow(5)
        menu_show_window(5, 0);
        return;
    }
    menu_close_window(); // one login form on the page (#accName / #accPass)
    if (!by_id("knV10Login")) {
        HtmlElem body = html_dom_body();
        if (!body) return;
        html_dom_insert_html(body, HTML_INSERT_BEFORE_END,
            "<div class=\"view-stack svelte-sdh4ve\" id=\"knV10Login\"><div class=\"view-stack-overlay svelte-sdh4ve\" onclick=\"knV10LoginClose()\">"
            "<div class=\"view-wrapper svelte-sdh4ve\" style=\"display: block\"></div></div></div>");
    }
    L.open = 1;
    L.reg = reg;
    L.recover = 0;
    L.use_pass = 0;
    L.choice = 0;
    L.code = 0;
    L.kio_gen = krunker_account_generation();
    L.backend = (int) account_backend();
    char saved[64] = ""; // _39221: Email first unless a username is saved (krunk.io's: never offered to krunker.io)
    if (kio() || !account_saved_name(saved, sizeof(saved))) saved[0] = 0;
    const char *t = saved;
    while (*t == ' ') t++;
    L.email = *t == 0;
    render();
}

// windows[4].gen (v10:10399794): showWindow(5) for a guest pushes this modal (_5217(_39035)) and
// returns autoWindowClose, so the window closes. Every guest showWindow(5) (the comp menu, Store /
// Market "Register to craft!", LOGIN REQUIRED cards, Battle Pass, a logout with the window up)
// lands here. KrunkNative keeps the window only as the frame of its password-reset steps
// (account_ui.c); its template (ui/windows/5.html) is v10's own guest capture, which is empty.
int v10_login_guest_window(void) {
    if (active_account() || L.to_reset || account_ui_reset_active() || !v10_menu_page()) return 0;
    if (menu_window_is_open()) menu_toggle_window(0); // closWind()
    open_modal(0);
    return 1;
}

void v10_login_account_changed(void) {
    // $account set: the modal leaves the stack; so does a modal of the other backend (an official
    // game joined or left while it was up)
    if (L.open && (active_account() || (int) account_backend() != L.backend)) close_modal();
}

static void message(const char *text) {
    HtmlElem resp = by_id("accResp");
    if (!resp) return;
    html_dom_set_text(resp, text ? text : "");
    html_dom_set_display(resp, text && text[0] ? "block" : "none"); // {#if message !== ''}
}

// ---------------------------------------------------------------- krunker.io

// _6696 (genericForm) for the challenge loop's code step (v10 _39014: check_2fa "Two-Factor
// Authentication", enter_code "Enter your code"): one one-time-code field of six boxes, Submit,
// no Cancel, not dismissable (a click on its overlay does nothing). It is a second view of the
// login modal's view stack, over the login card.
#define CODE_LEN 6 // config.twoFAKeyLen / the field's codeLength

static void code_show(const char *kind) {
    HtmlElem host = by_id("knV10Login");
    if (!host || by_id("knV10Code")) return;
    const int twofa = !strcmp(kind, "2fa");
    StrBuf sb = {0};
    sb_append(&sb, "<div class=\"view-stack-overlay svelte-sdh4ve\" id=\"knV10Code\" onclick=\"knV10LoginCodeStay()\">"
                   "<div class=\"view-wrapper svelte-sdh4ve\" style=\"display: block\"><div class=\"popup-container svelte-1wz7ymp\">"
                   "<div class=\"popup svelte-1wz7ymp\" style=\"width: 800px;\"><div class=\"io-popup-container\"><div class=\"io-popup-header\">");
    sb_append_esc(&sb, twofa ? "Two-Factor Authentication" : "Enter your code");
    sb_append(&sb, "</div> <div class=\"io-popup-content\"><div class=\"io-card\"><p>");
    sb_append_esc(&sb, twofa ? "Please enter the 6-digit code from your authenticator app." : "Please enter the code you received in your email.");
    sb_append(&sb, "</p> <form><div class=\"io-input-container\"><div class=\"io-section\"> <div class=\"code-input-container svelte-1xwmszg\">");
    for (int i = 0; i < CODE_LEN; i++)
        sb_appendf(&sb, "<input type=\"number\" min=\"0\" max=\"9\" maxlength=\"1\" placeholder=\" \" id=\"knV10Code%d\" "
                        "class=\"code-input code-input-code svelte-1xwmszg\" oninput=\"knV10LoginCodeInput(%d)\" onkeydown=\"knV10LoginCodeKey(%d)\">", i, i, i);
    sb_appendf(&sb, "</div></div> <div class=\"io-button-group\"> <button type=\"submit\" class=\"io-button io-button--accept\" id=\"knV10CodeSubmit\" "
                    "onclick=\"knV10LoginCodeSubmit()\">%s</button></div></div></form></div></div></div></div></div></div></div>",
               I18N("generic.submit", "Submit"));
    char *html = sb_finish(&sb);
    html_dom_insert_html(host, HTML_INSERT_BEFORE_END, html);
    free(html);
    L.code = 1;
    HtmlElem first = by_id("knV10Code0");
    if (first) html_dom_focus(first);
}

static void code_hide(void) {
    HtmlElem view = by_id("knV10Code");
    if (view) html_dom_remove(view);
    L.code = 0;
}

static void code_value(char *out, const size_t n) {
    size_t o = 0;
    for (int i = 0; i < CODE_LEN && o + 1 < n; i++) {
        char id[16], v[8] = "";
        snprintf(id, sizeof(id), "knV10Code%d", i);
        read_value(id, v, sizeof(v));
        if (v[0] >= '0' && v[0] <= '9') out[o++] = v[0];
    }
    out[o] = 0;
}

static void code_submit(void) {
    char code[CODE_LEN + 2];
    code_value(code, sizeof(code));
    if (strlen(code) < CODE_LEN) return; // the form is not valid yet (v10 keeps Submit disabled)
    code_hide(); // the view pops with its answer; the loop goes on under the login card
    krunker_account_submit_code(code);
    memset(code, 0, sizeof(code));
    message(krunker_account_message());
    L.kio_gen = krunker_account_generation();
}

// v10's code boxes: a digit moves to the next box, Backspace in an empty one to the previous,
// the sixth digit submits (the field is the form's only one)
static void code_input(const int i) {
    char id[16], v[16] = "";
    snprintf(id, sizeof(id), "knV10Code%d", i);
    read_value(id, v, sizeof(v));
    HtmlElem el = by_id(id);
    if (strlen(v) > 1 && el) { // keep the last digit typed
        char last[2] = {v[strlen(v) - 1], 0};
        html_dom_set_value(el, last);
    }
    if (v[0] && i + 1 < CODE_LEN) {
        snprintf(id, sizeof(id), "knV10Code%d", i + 1);
        HtmlElem next = by_id(id);
        if (next) html_dom_focus(next);
    }
    char code[CODE_LEN + 2];
    code_value(code, sizeof(code));
    if (strlen(code) == CODE_LEN) code_submit();
    memset(code, 0, sizeof(code));
}

// _39062 / _39177 (login): krunker.io's username or email + password, the email code choice
static void kio_submit(const char *mail, const char *name, const char *pass) {
    if (L.reg) {
        if (!*mail) {
            message(I18N("generic.auth.emailRequiredForRegister", "Please enter your email to register."));
            return;
        }
        // registerEmailCode: krunker.io accounts are created on krunker.io, not by this client
        message(I18N("kn.kio.registerOnSite", "Create your krunker.io account on krunker.io, then log in here."));
        return;
    }
    const char *ident = L.email ? mail : name;
    if (!ident[0]) {
        message(I18N("generic.auth.fillAllFields", "Please fill in all fields"));
        return;
    }
    if (!pass[0]) {
        if (L.email && !L.use_pass) { // _39234: "Send Email Code" / "Use Password"
            L.choice = 1;
            render();
            return;
        }
        message(I18N("generic.auth.fillAllFields", "Please fill in all fields"));
        return;
    }
    krunker_account_sign_in(ident, pass);
    message(krunker_account_message()); // "Please wait..." (v10 _39219), or why it was refused
    L.kio_gen = krunker_account_generation();
}

// Per frame: the krunker.io sign-in's progress in the modal (the message line, the code form);
// signed in, the active account changes and the modal leaves (v10_login_account_changed).
void v10_login_tick(void) {
    if (!L.open || !kio() || !by_id("knV10Login")) return;
    const unsigned gen = krunker_account_generation();
    if (gen == L.kio_gen) return;
    L.kio_gen = gen;
    const KrunkerAccountState st = krunker_account_state();
    if (st == KRUNKER_ACCOUNT_CODE) code_show(krunker_account_code_kind());
    else if (L.code) code_hide();
    const char *msg = krunker_account_message();
    if (st != KRUNKER_ACCOUNT_CODE && msg[0]) message(msg);
}

static void submit(void) {
    if (!L.open || L.recover) return;
    char name[256], mail[256], pass[128];
    read_value("accName", name, sizeof(name));
    read_value("accEmail", mail, sizeof(mail));
    read_value("accPass", pass, sizeof(pass));
    const int has_pass = pass[0] != 0;
    const char *m = mail;
    while (*m == ' ') m++;
    if (kio()) {
        kio_submit(m, name, pass);
        memset(pass, 0, sizeof(pass));
        return;
    }
    memset(pass, 0, sizeof(pass));
    if (L.reg) {
        if (!*m) { // the register card's own check (v10 _39207 submit)
            message(I18N("generic.auth.emailRequiredForRegister", "Please enter your email to register."));
            return;
        }
        if (!name[0] || !has_pass) {
            message(I18N("generic.auth.fillAllFields", "Please fill in all fields"));
            return;
        }
    } else {
        const char *ident = L.email ? m : name;
        if (!ident[0]) {
            message(I18N("generic.auth.fillAllFields", "Please fill in all fields"));
            return;
        }
        if (!has_pass) {
            if (L.email && !L.use_pass) {
                // v10 (_39234) offers "Send Email Code" / "Use Password" here; KrunkNative accounts
                // sign in with a password only: "Use Password" (_39184), the password row shows
                L.use_pass = 1;
                sync_pass();
                HtmlElem p = by_id("accPass");
                if (p) html_dom_focus(p);
                return;
            }
            message(I18N("generic.auth.fillAllFields", "Please fill in all fields"));
            return;
        }
    }
    // account_ui.c reads #accName or #accEmail / #accPass (/ #accEmail) and answers in #accResp
    if (account_ui_handle(L.reg ? "registerAcc" : "loginAcc", NULL, NULL, 0) < 0)
        message(I18N("generic.error", "Error")); // no account service in this build
}

// The recover card's Submit: krunker.io's recoverAccount (v10 _39188); krunk.io: the Account
// window's reset steps, started with this address
static void recover(void) {
    char who[256];
    read_value("recoveryEmail", who, sizeof(who));
    if (!who[0]) {
        HtmlElem resp = by_id("accResp");
        if (resp) {
            html_dom_set_text(resp, I18N("generic.auth.fillAllFields", "Please fill in all fields"));
            html_dom_set_display(resp, "block");
        }
        return;
    }
    if (kio()) {
        krunker_account_recover(who);
        message(krunker_account_message());
        L.kio_gen = krunker_account_generation();
        return;
    }
    close_modal();
    L.to_reset = 1;
    menu_show_window(5, 0);
    L.to_reset = 0;
    account_ui_handle("knResetOpen", NULL, NULL, 0);
    HtmlElem in = by_id("knResetWho");
    if (in) html_dom_set_value(in, who);
    account_ui_handle("knResetNext", NULL, NULL, 0);
}

int v10_login_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    if (!strcmp(name, "loginOrRegister")) {
        open_modal(arg_true(args, argc));
        return 1;
    }
    if (strncmp(name, "knV10Login", 10) != 0) return -1;
    // a click inside the popup never reaches the overlay (its markup may be re-rendered by then)
    if (strcmp(name, "knV10LoginClose") != 0) html_ui_stop_propagation();
    if (!strcmp(name, "knV10LoginStay") || !strcmp(name, "knV10LoginCodeStay")) return 1; // (the code form is not dismissable)
    if (!strcmp(name, "knV10LoginSendCode")) { // _39177: loginEmailCode
        char mail[256];
        read_value("accEmail", mail, sizeof(mail));
        if (!mail[0]) message(I18N("generic.auth.fillAllFields", "Please fill in all fields"));
        else {
            krunker_account_email_code(mail);
            message(krunker_account_message());
            L.kio_gen = krunker_account_generation();
        }
        return 1;
    }
    if (!strcmp(name, "knV10LoginUsePass")) { // _39184: the password after all
        L.choice = 0;
        L.use_pass = 1;
        render();
        return 1;
    }
    if (!strcmp(name, "knV10LoginCodeForm")) { // --render-scenario / uidiff only: the code form ("2fa" | "email"), no request
        code_show(argc > 0 && args[0].kind == HTML_ARG_STR && args[0].str && !strcmp(args[0].str, "email") ? "email" : "2fa");
        return 1;
    }
    if (!strcmp(name, "knV10LoginCodeInput")) {
        code_input(argc > 0 && args[0].kind == HTML_ARG_NUM ? (int) args[0].num : 0);
        return 1;
    }
    if (!strcmp(name, "knV10LoginCodeSubmit")) {
        code_submit();
        return 1;
    }
    if (!strcmp(name, "knV10LoginCodeKey")) {
        const int i = argc > 0 && args[0].kind == HTML_ARG_NUM ? (int) args[0].num : 0;
        if (ev && html_key_is_enter(ev->key)) {
            code_submit();
            return 0;
        }
        if (ev && (ev->key == 259 || ev->key == 8) && i > 0) { // Backspace (GLFW / DOM) in an empty box: the previous one
            char id[16], v[8] = "";
            snprintf(id, sizeof(id), "knV10Code%d", i);
            read_value(id, v, sizeof(v));
            if (!v[0]) {
                snprintf(id, sizeof(id), "knV10Code%d", i - 1);
                HtmlElem prev = by_id(id);
                if (prev) {
                    html_dom_set_value(prev, "");
                    html_dom_focus(prev);
                }
            }
        }
        return 1;
    }
    if (!strcmp(name, "knV10LoginClose")) {
        close_modal();
        return 1;
    }
    if (!strcmp(name, "knV10LoginSwitch")) { // _39150: the other card, fields and message cleared
        L.reg = !L.reg;
        L.use_pass = 0;
        L.choice = 0;
        render_with("", "", "");
        return 1;
    }
    // (the buttons call window.<handler>(): in a browser an inline handler inside the
    // <form> sees the form's named elements first, so a bare onclick="knV10LoginMode()" on #knV10LoginMode
    // would call the button itself, not the C handler: the web build's toggle and Login did nothing)
    if (!strcmp(name, "knV10LoginToggle")) { // _39146: Email <-> Username, the value and password kept
        char ident[256], pass[128];
        read_value(L.email ? "accEmail" : "accName", ident, sizeof(ident));
        read_value("accPass", pass, sizeof(pass));
        L.email = !L.email;
        L.use_pass = 0;
        L.choice = 0;
        render_with(ident, ident, pass);
        memset(pass, 0, sizeof(pass));
        return 1;
    }
    if (!strcmp(name, "knV10LoginInput")) { // _39166 / _39172: the suggestion and the password row follow the typing
        if (L.choice) { // a password typed: Login again (_39166 resets _39234)
            char pw[128];
            read_value("accPass", pw, sizeof(pw));
            const int typed = pw[0] != 0;
            memset(pw, 0, sizeof(pw));
            if (typed) {
                L.choice = 0;
                render();
                return 1;
            }
        }
        sync_hint();
        sync_pass();
        return 1;
    }
    if (!strcmp(name, "knV10LoginForgot")) { // _39160: the recover card and back
        L.recover = !L.recover;
        L.reg = 0;
        L.choice = 0;
        render();
        return 1;
    }
    if (!strcmp(name, "knV10LoginGo")) {
        submit();
        return 1;
    }
    if (!strcmp(name, "knV10LoginRecover")) {
        recover();
        return 1;
    }
    if (!strcmp(name, "knV10LoginKey")) { // the form's submit on Enter, Escape closes
        if (ev && html_key_is_enter(ev->key)) {
            if (L.recover) recover();
            else submit();
            return 0;
        }
        if (ev && html_key_is_escape(ev->key)) {
            close_modal();
            return 0;
        }
        return 1;
    }
    return -1;
}

// Escape with the modal up (not in a field): the modal goes, the menu stays
int v10_menu_login_escape(void) {
    if (!v10_menu_login_open()) return 0;
    if (L.code) return 1; // the code form is not dismissable
    close_modal();
    return 1;
}
