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
// It runs on KrunkNative's account service (account_ui.c: the social socket's sendAccountRequest,
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
} L;

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
                       "class=\"auth-toggle-btn svelte-16z1lxi\" id=\"knV10LoginMode\" onclick=\"knV10LoginMode()\">%s</button></div> "
                       "<input id=\"accEmail\" type=\"email\" class=\"io-input-field svelte-16z1lxi\" autocomplete=\"email\" placeholder=\"%s\" "
                       "onkeydown=\"knV10LoginKey()",
                   I18N("generic.email", "Email"), I18N("generic.auth.useUsername", "(use username instead)"), I18N("generic.auth.enterEmail", "Enter Email"));
        append_value(sb, ident);
    } else { // _39201 (+ _39200, the email suggestion)
        sb_appendf(sb, "<div class=\"label-row\"><label for=\"accName\" class=\"input-label\">%s</label> <button type=\"button\" "
                       "class=\"auth-toggle-btn svelte-16z1lxi\" id=\"knV10LoginMode\" onclick=\"knV10LoginMode()\">%s</button></div> "
                       "<input id=\"accName\" type=\"text\" class=\"io-input-field svelte-16z1lxi\" autocomplete=\"username\" placeholder=\"%s\" "
                       "oninput=\"knV10LoginInput()\" onkeydown=\"knV10LoginKey()",
                   I18N("generic.username", "Username"), I18N("generic.auth.useEmail", "(use email instead)"), I18N("generic.auth.enterUsername", "Enter Username"));
        append_value(sb, ident);
        sb_appendf(sb, " <div class=\"email-suggestion svelte-16z1lxi\" id=\"knV10EmailHint\" style=\"display:%s\">%s "
                       "<button type=\"button\" class=\"suggestion-link svelte-16z1lxi\" onclick=\"knV10LoginMode()\">%s</button> %s</div>",
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
    sb_appendf(sb, " <button type=\"button\" class=\"io-button io-button--accept svelte-16z1lxi\" id=\"knV10LoginSubmit\" onclick=\"knV10LoginSubmit()\">%s</button>"
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
    sb_appendf(sb, " <div class=\"label-row\" style=\"margin-top:15px\"><label for=\"accName\" class=\"input-label\">%s</label></div> "
                   "<input id=\"accName\" type=\"text\" class=\"io-input-field svelte-16z1lxi\" autocomplete=\"username\" placeholder=\"%s\" "
                   "onkeydown=\"knV10LoginKey()", I18N("generic.username", "Username"), I18N("generic.auth.enterUsername", "Enter Username"));
    append_value(sb, name);
    sb_appendf(sb, " <div class=\"label-row\" style=\"margin-top:15px\"><label for=\"accPass\" class=\"input-label\">%s</label></div> "
                   "<input id=\"accPass\" type=\"password\" class=\"io-input-field svelte-16z1lxi\" autocomplete=\"new-password\" placeholder=\"%s\" "
                   "onkeydown=\"knV10LoginKey()", I18N("generic.password", "Password"), I18N("generic.auth.enterPassword", "Enter Password"));
    append_value(sb, pass);
    sb_appendf(sb, " <label class=\"promo-optin svelte-16z1lxi\"><input type=\"checkbox\" class=\"svelte-16z1lxi\"> "
                   "<span>%s</span></label> "
                   "<button type=\"button\" class=\"io-button io-button--accept svelte-16z1lxi\" id=\"knV10LoginSubmit\" onclick=\"knV10LoginSubmit()\">%s</button> "
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
    if (!name[0] && !mail[0]) html_storage_get("krunker_username", name, sizeof(name)); // getSavedVal("krunker_username")
    render_with(name, mail, pass);
    memset(pass, 0, sizeof(pass));
}

static void close_modal(void) {
    L.open = 0;
    L.recover = 0;
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
    if (account_get()) { // signed in: showWindow(5)
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
    char saved[64] = ""; // _39221: Email first unless a username is saved
    if (html_storage_get("krunker_username", saved, sizeof(saved)) < 0) saved[0] = 0;
    const char *t = saved;
    while (*t == ' ') t++;
    L.email = *t == 0;
    render();
}

void v10_login_account_changed(void) {
    if (L.open && account_get()) close_modal(); // $account set: the modal leaves the stack
}

static void message(const char *text) {
    HtmlElem resp = by_id("accResp");
    if (!resp) return;
    html_dom_set_text(resp, text);
    html_dom_set_display(resp, "block");
}

static void submit(void) {
    if (!L.open || L.recover) return;
    char name[256], mail[256], pass[128];
    read_value("accName", name, sizeof(name));
    read_value("accEmail", mail, sizeof(mail));
    read_value("accPass", pass, sizeof(pass));
    const int has_pass = pass[0] != 0;
    memset(pass, 0, sizeof(pass));
    const char *m = mail;
    while (*m == ' ') m++;
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

// The recover card's Submit: the Account window's reset steps, started with this address
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
    close_modal();
    menu_show_window(5, 0);
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
    if (!strcmp(name, "knV10LoginStay")) return 1;
    if (!strcmp(name, "knV10LoginClose")) {
        close_modal();
        return 1;
    }
    if (!strcmp(name, "knV10LoginSwitch")) { // _39150: the other card, fields and message cleared
        L.reg = !L.reg;
        L.use_pass = 0;
        render_with("", "", "");
        return 1;
    }
    if (!strcmp(name, "knV10LoginMode")) { // _39146: Email <-> Username, the value and password kept
        char ident[256], pass[128];
        read_value(L.email ? "accEmail" : "accName", ident, sizeof(ident));
        read_value("accPass", pass, sizeof(pass));
        L.email = !L.email;
        L.use_pass = 0;
        render_with(ident, ident, pass);
        memset(pass, 0, sizeof(pass));
        return 1;
    }
    if (!strcmp(name, "knV10LoginInput")) { // _39166 / _39172: the suggestion and the password row follow the typing
        sync_hint();
        sync_pass();
        return 1;
    }
    if (!strcmp(name, "knV10LoginForgot")) { // _39160: the recover card and back
        L.recover = !L.recover;
        L.reg = 0;
        render();
        return 1;
    }
    if (!strcmp(name, "knV10LoginSubmit")) {
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
    close_modal();
    return 1;
}
