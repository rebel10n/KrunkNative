// Krunker v10.0.2 login / register modal: loginOrRegister(defaultToRegister) (v10:210151) pushes
// the Svelte component _39035 (v10:114780) onto the view stack (_5217; the stack host _40140 /
// _40139, v10:117090): .view-stack > .view-stack-overlay (click outside = close) > .view-wrapper
// (scaled like #uiBase through --ui-scale / --ui-width / --ui-height) > .popup-container > .popup
// (width 800) > .io-popup-container with the header (Login to Krunker / Create Account / Recover
// Account), the "New to Krunker? Register here" switch with the sign-up bonuses, the form card
// and the .io-message line. Signed in, loginOrRegister() is showWindow(5).
//
// It runs on KrunkNative's account service (account_ui.c: the social socket's sendAccountRequest,
// the replies written into #accResp, the forgot-password flow). Differences from v10, because
// that service is what it is (doc/v10/ui.md):
//   - accounts log in by username + password: the "(use email instead)" toggle, the passwordless
//     "Send Email Code" choice and the Twitch-link line are not offered
//   - registering takes a username and a password besides the email (v10 registers by email and
//     mails a code); the email stays optional as in account_ui.c, the news opt-in is shown but
//     nothing subscribes to it (KrunkNative sends no newsletters)
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

static void render(void) {
    HtmlElem host = by_id("knV10Login");
    if (!host) return;
    // the inputs keep what was typed across a re-render (Svelte keeps its $state)
    char name[64], mail[256];
    read_value("accName", name, sizeof(name));
    read_value("accEmail", mail, sizeof(mail));
    if (!name[0]) html_storage_get("krunker_username", name, sizeof(name)); // getSavedVal("krunker_username")
    StrBuf sb = {0};
    sb_append(&sb, "<div class=\"popup-container svelte-1wz7ymp\"><div class=\"popup svelte-1wz7ymp\" style=\"width: 800px;\" "
                   "onclick=\"knV10LoginStay()\"><div class=\"io-popup-container\"><div class=\"io-popup-header\">");
    sb_append(&sb, L.recover ? "Recover Account" : L.reg ? "Create Account" : "Login to Krunker");
    sb_append(&sb, "</div> <div class=\"io-popup-content\">");
    if (!L.recover) {
        sb_appendf(&sb, "<div class=\"auth-actions svelte-16z1lxi\"><span class=\"svelte-16z1lxi\">%s</span> "
                        "<button type=\"button\" class=\"login-button svelte-16z1lxi\" id=\"knV10AuthSwitch\" onclick=\"knV10LoginSwitch()\">%s</button></div> ",
                   L.reg ? "Already have an account?" : "New to Krunker?", L.reg ? "Login here" : "Register here");
        if (L.reg) sb_append(&sb, "<div class=\"sign-up-bonuses svelte-16z1lxi\"><span class=\"svelte-16z1lxi\">Sign up bonuses</span> <div>"
                                  "<li class=\"svelte-16z1lxi\"><span class=\"svelte-16z1lxi\">14 Cosmetics</span></li> "
                                  "<li class=\"svelte-16z1lxi\"><span class=\"svelte-16z1lxi\">24h of premium account</span></li></div></div>");
    }
    if (L.recover) {
        sb_append(&sb, "<div class=\"io-card\"><div class=\"io-input-container svelte-16z1lxi\"><div class=\"label-row\"><label for=\"recoveryEmail\" "
                       "class=\"input-label\">Email</label></div> <input id=\"recoveryEmail\" type=\"email\" class=\"io-input-field svelte-16z1lxi\" "
                       "autocomplete=\"email\" placeholder=\"Enter Email\" onkeydown=\"knV10LoginKey()\"> <button type=\"button\" "
                       "class=\"io-button io-button--accept svelte-16z1lxi\" id=\"knV10RecoverSubmit\" onclick=\"knV10LoginRecover()\">Submit</button></div></div> "
                       "<button type=\"button\" class=\"io-button\" id=\"knV10RecoverBack\" onclick=\"knV10LoginForgot()\">Back</button>");
    } else {
        sb_append(&sb, "<div class=\"io-card\"><form data-priority=\"true\"><div class=\"io-input-container svelte-16z1lxi\">"
                       "<div class=\"label-row\"><label for=\"accName\" class=\"input-label\">Username</label></div> "
                       "<input id=\"accName\" type=\"text\" class=\"io-input-field svelte-16z1lxi\" autocomplete=\"username\" placeholder=\"Enter Username\" "
                       "onkeydown=\"knV10LoginKey()\" value=\"");
        sb_append_esc(&sb, name);
        sb_append(&sb, "\"> <div class=\"label-row\" style=\"margin-top:15px\"><label for=\"accPass\" class=\"input-label\">Password</label>");
        if (!L.reg) sb_append(&sb, " <button type=\"button\" class=\"auth-toggle-btn svelte-16z1lxi\" id=\"knV10Forgot\" onclick=\"knV10LoginForgot()\">Forgot Password?</button>");
        sb_appendf(&sb, "</div> <input id=\"accPass\" type=\"password\" class=\"io-input-field svelte-16z1lxi\" autocomplete=\"%s\" "
                        "placeholder=\"Enter Password\" onkeydown=\"knV10LoginKey()\"> ", L.reg ? "new-password" : "current-password");
        if (L.reg) {
            sb_append(&sb, "<div class=\"label-row\" style=\"margin-top:15px\"><label for=\"accEmail\" class=\"input-label\">Email</label></div> "
                           "<input id=\"accEmail\" type=\"email\" class=\"io-input-field svelte-16z1lxi\" autocomplete=\"email\" placeholder=\"Enter Email\" "
                           "onkeydown=\"knV10LoginKey()\" value=\"");
            sb_append_esc(&sb, mail);
            sb_append(&sb, "\"> <label class=\"promo-optin svelte-16z1lxi\"><input type=\"checkbox\" class=\"svelte-16z1lxi\"> "
                           "<span>Send me news about events, updates and season releases</span></label> "
                           "<button type=\"button\" class=\"io-button io-button--accept svelte-16z1lxi\" id=\"knV10LoginSubmit\" onclick=\"knV10LoginSubmit()\">Register</button> "
                           "<div class=\"terms-privacy-notice svelte-16z1lxi\">By registering, you agree to our "
                           "<a class=\"svelte-16z1lxi\" onclick=\"openURL('https://frvr.com/legal/krunker#TermsofService')\">Terms of Service</a> and "
                           "<a class=\"svelte-16z1lxi\" onclick=\"openURL('https://frvr.com/legal/krunker#PrivacyPolicy')\">Privacy Policy</a></div>");
        } else {
            sb_append(&sb, "<button type=\"button\" class=\"io-button io-button--accept svelte-16z1lxi\" id=\"knV10LoginSubmit\" onclick=\"knV10LoginSubmit()\">Login</button>");
        }
        sb_append(&sb, "</div></form></div>");
    }
    // {#if message !== ''} .io-message: account_ui.c's showAccResp writes #accResp
    sb_append(&sb, " <div class=\"io-message\" id=\"accResp\" style=\"display:none\"></div></div></div></div></div>");
    char *html = sb_finish(&sb);
    HtmlElem wrapper = html_dom_query(host, ".view-wrapper");
    if (wrapper) html_dom_set_html(wrapper, html);
    free(html);
    sync_ui_vars(host);
    HtmlElem focus = by_id(L.recover ? "recoveryEmail" : name[0] && !L.reg ? "accPass" : "accName");
    if (focus) html_dom_focus(focus);
}

static void close_modal(void) {
    L.open = 0;
    L.recover = 0;
    HtmlElem host = by_id("knV10Login");
    if (host) html_dom_remove(host);
}

// loginOrRegister(defaultToRegister)
static void open_modal(const int reg) {
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
    render();
}

void v10_login_account_changed(void) {
    if (L.open && account_get()) close_modal(); // $account set: the modal leaves the stack
}

static void submit(void) {
    if (!L.open || L.recover) return;
    char name[64], pass[128];
    read_value("accName", name, sizeof(name));
    read_value("accPass", pass, sizeof(pass));
    HtmlElem resp = by_id("accResp");
    if (!name[0] || !pass[0]) { // generic.auth.fillAllFields
        if (resp) {
            html_dom_set_text(resp, "Please fill in all fields");
            html_dom_set_display(resp, "block");
        }
        memset(pass, 0, sizeof(pass));
        return;
    }
    memset(pass, 0, sizeof(pass));
    // account_ui.c reads #accName / #accPass (/ #accEmail) and answers in #accResp
    if (account_ui_handle(L.reg ? "registerAcc" : "loginAcc", NULL, NULL, 0) < 0 && resp) {
        html_dom_set_text(resp, "Error"); // generic.error: no account service in this build
        html_dom_set_display(resp, "block");
    }
}

// The recover card's Submit: the Account window's reset steps, started with this address
static void recover(void) {
    char who[256];
    read_value("recoveryEmail", who, sizeof(who));
    if (!who[0]) {
        HtmlElem resp = by_id("accResp");
        if (resp) {
            html_dom_set_text(resp, "Please fill in all fields");
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
        HtmlElem el;
        if ((el = by_id("accPass"))) html_dom_set_value(el, "");
        if ((el = by_id("accEmail"))) html_dom_set_value(el, "");
        render();
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
