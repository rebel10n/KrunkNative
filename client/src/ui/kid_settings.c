// Privacy & Age (Krunker v10.0.4): the k-ID section of Account Settings (kid_settings.h).
//
// The kid API is on the gapi (deob-10.0.4.js:26519-26543, the client `new iiiîiïî(GAPI_URI)` at :27725):
//   getSettings        GET  /account/kid                    -> {data: settings}
//   requestPermission  POST /account/kid/permissions        {name}  -> {data: settings} | {error: {code}}
//   emailParent        POST /account/kid/permissions/email  {email} -> {data: {sent}}
// settings: {status, ageCategory, jurisdiction, permissions: [{name, managedBy, enabled}],
//            pending: {permission, type, url, oneTimePassword} | null, result: "approved" | "denied" | ...}
// (the fields the component reads, 97519-97616).
//
// The section is the component at deob-10.0.4.js:97480-97770:
//   gate        status !== "none" (97621)
//   header      kid.settings.header (97628); info + age group / region (97631-97656)
//   status      kidInfo kidNote, kid.settings.status.<status>, unless active (97658-97664)
//   rows        one per permission (97666-97743), its name, then by managedBy:
//                 PLAYER    active: the switch (turn on only: locked once on, busy while a request runs);
//                           else the state ("On" or nothing)
//                 GUARDIAN  on: "On (approved by a parent)"; this permission pending: Show Code / Verify Age +
//                           "Waiting for a parent"; else (active) Ask a Parent + "Needs a parent's approval"
//                 other     "Not available for your age in your region"
//               and its description when the table has kid.settings.perm.<name>.desc (97735-97741)
//   offHint     when the player turned a PLAYER permission on (97745-97751)
//   result      the last request's line, kidOk when it went well (97752-97760)
// Requests (97527-97615): a switch / Ask a Parent sends requestPermission; the answer's settings replace
// the section's, then: the permission is on -> "enabled"; an age check is pending -> open its page; a parent's
// approval is pending -> "requested" and the parent dialog (the code, its QR code, the parent's email).
// While anything is pending the settings are asked again every 30 s (97538), and an approved / denied answer
// sets the result line (97536-97537).
#include <kid_settings.h>
#include <krunker_account.h>
#include <v10_ui.h>
#include <i18n.h>
#include <cJSON.h>
#include "qr_code.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KID_CACHE_MS 15000.0 // a mount within 15 s of the last answer reuses it (97540: 0x3a98)
#define KID_POLL_MS 30000.0  // pending: the settings are asked again every 30 s (97538: 0x7530)
#define KID_AGE_ASSURANCE "CHALLENGE_SESSION_UPGRADE_BY_AGE_ASSURANCE" // pending.type of an age check (97515)

typedef enum {
    KID_IDLE,    // never asked, or the last ask could not be sent
    KID_LOADING, // GET /account/kid in flight
    KID_READY,   // the last GET answered (data may still be NULL: an error answer keeps the old data, 97546)
} KidLoad;

static struct {
    KidLoad load;
    double asked_at;          // when the last GET answered (the 15 s reuse)
    cJSON *data;              // the settings (the answer's "data")
    void (*on_change)(void);  // re-renders the Account Settings tab
    int busy;                 // a permission request in flight (iÏiïíïi)
    int generation;           // bumped by kid_settings_clear: an answer for a signed-out account is dropped
    char result[512];         // the result line (iÌíîííî.text), "" = none
    int result_ok;            // iÌíîííî.ok
    int poll;                 // the 30 s timer while pending (0 = none)
    char code[64];            // the open dialog's one-time password (knKidCopy)
    int copy_timer;           // clears the dialog's "Copied!" after 2 s
} g_kid;

static double now_ms(void) {
    const double t = html_timer_now_ms();
    return t < 0 ? 0 : t;
}

static void changed(void) {
    if (g_kid.on_change) g_kid.on_change();
}

static const char *jstr(const cJSON *o, const char *key) {
    const cJSON *v = o ? cJSON_GetObjectItem(o, key) : NULL;
    return v && cJSON_IsString(v) ? v->valuestring : NULL;
}

static int jtrue(const cJSON *o, const char *key) {
    const cJSON *v = o ? cJSON_GetObjectItem(o, key) : NULL;
    return v && cJSON_IsTrue(v);
}

static const cJSON *pending_of(const cJSON *d) {
    const cJSON *p = d ? cJSON_GetObjectItem(d, "pending") : NULL;
    return p && cJSON_IsObject(p) ? p : NULL;
}

static int active(void) { return g_kid.data && jstr(g_kid.data, "status") && !strcmp(jstr(g_kid.data, "status"), "active"); }

// iÏîíìíï: the permission's name in the table, else the raw name
static const char *perm_name(const char *name) {
    char key[160];
    snprintf(key, sizeof(key), "kid.settings.perm.%s", name ? name : "");
    return I18N(key, name ? name : "");
}

// iÎíïîîï: the result line, kid.settings.result.<key> with its arguments
static const struct {
    const char *key, *english;
} RESULTS[] = {
    {"kid.settings.result.approved", "A parent approved your request. {0} is on."},
    {"kid.settings.result.denied", "A parent declined your request."},
    {"kid.settings.result.enabled", "{0} is on. You can use it right away."},
    {"kid.settings.result.requested", "Request created. Show the code to a parent or email it to them."},
    {"kid.settings.result.ageCheck", "Finish verifying your age in the page that opened, then come back here."},
    {"kid.settings.result.emailed", "We sent the request to your parent's email."},
    {"kid.settings.result.emailFailed", "We couldn't send the email. Check the address or use the code instead."},
    {"kid.settings.result.noSession", "Your k-ID session has expired. Log out and back in to verify again."},
    {"kid.settings.result.failed", "Something went wrong. Please try again later."},
};
// (i18n_lint reads the pairs from these lines:)
// I18N("kid.settings.result.approved", "A parent approved your request. {0} is on.")
// I18N("kid.settings.result.denied", "A parent declined your request.")
// I18N("kid.settings.result.enabled", "{0} is on. You can use it right away.")
// I18N("kid.settings.result.requested", "Request created. Show the code to a parent or email it to them.")
// I18N("kid.settings.result.ageCheck", "Finish verifying your age in the page that opened, then come back here.")
// I18N("kid.settings.result.emailed", "We sent the request to your parent's email.")
// I18N("kid.settings.result.emailFailed", "We couldn't send the email. Check the address or use the code instead.")
// I18N("kid.settings.result.noSession", "Your k-ID session has expired. Log out and back in to verify again.")
// I18N("kid.settings.result.failed", "Something went wrong. Please try again later.")
static void set_result(const char *key, const int ok, const char *arg) {
    char full[96];
    snprintf(full, sizeof(full), "kid.settings.result.%s", key);
    const char *english = full;
    for (size_t i = 0; i < sizeof(RESULTS) / sizeof(RESULTS[0]); i++)
        if (!strcmp(RESULTS[i].key, full)) english = RESULTS[i].english;
    const char *argv[1] = {arg};
    i18n_trf(g_kid.result, sizeof(g_kid.result), full, english, arg ? 1 : 0, argv);
    g_kid.result_ok = ok;
}

static void refresh_now(void);

static void poll_fire(void *user) {
    (void) user;
    g_kid.poll = 0;
    refresh_now();
}

// iÎìïïii: new settings from any answer
static void apply(const cJSON *data) {
    char prev[96] = "";
    const char *was = jstr(pending_of(g_kid.data), "permission");
    if (was) snprintf(prev, sizeof(prev), "%s", was);
    cJSON *copy = cJSON_Duplicate(data, 1);
    if (!copy) return;
    cJSON_Delete(g_kid.data);
    g_kid.data = copy;
    g_kid.asked_at = now_ms();
    const char *result = jstr(data, "result");
    if (result && !strcmp(result, "approved")) set_result("approved", 1, perm_name(prev[0] ? prev : "text-chat-public"));
    else if (result && !strcmp(result, "denied")) set_result("denied", 0, NULL);
    if (g_kid.poll) html_timer_cancel(g_kid.poll);
    g_kid.poll = pending_of(data) ? html_timer_add(KID_POLL_MS, poll_fire, NULL) : 0;
}

static void on_settings(void *user, const int status, const cJSON *res) {
    if ((int) (intptr_t) user != g_kid.generation) return; // signed out meanwhile
    g_kid.load = KID_READY;
    g_kid.asked_at = now_ms();
    const cJSON *data = status == 200 && res ? cJSON_GetObjectItem(res, "data") : NULL;
    if (data && cJSON_IsObject(data)) apply(data); // an error keeps what was shown (97546: catch {})
    changed();
}

static void refresh_now(void) {
    if (g_kid.load == KID_LOADING || !krunker_account_signed_in()) return;
    g_kid.load = KID_LOADING;
    if (!krunker_account_api("GET", KRUNKER_SVC_API, "/account/kid", NULL, on_settings, (void *) (intptr_t) g_kid.generation))
        g_kid.load = KID_IDLE;
}

void kid_settings_refresh(void (*on_change)(void)) {
    g_kid.on_change = on_change;
    if (g_kid.load == KID_LOADING) return;
    if (g_kid.load == KID_READY && now_ms() - g_kid.asked_at < KID_CACHE_MS) return;
    refresh_now();
}

void kid_settings_clear(void) {
    cJSON_Delete(g_kid.data);
    if (g_kid.poll) html_timer_cancel(g_kid.poll);
    if (g_kid.copy_timer) html_timer_cancel(g_kid.copy_timer);
    const int generation = g_kid.generation + 1;
    void (*on_change)(void) = g_kid.on_change;
    memset(&g_kid, 0, sizeof(g_kid));
    g_kid.generation = generation;
    g_kid.on_change = on_change;
}

// ---------------------------------------------------------------- the parent request dialog (iÏíííïi)

static void dialog_closed(const int accepted, void *user) {
    (void) accepted;
    (void) user;
    g_kid.code[0] = 0;
    if (g_kid.copy_timer) html_timer_cancel(g_kid.copy_timer);
    g_kid.copy_timer = 0;
}

// v10's io-popup form (deob-10.0.4.js:15113, svelte-1xwmszg) with the fields iÏíííïi builds (97571-97590):
// the one-time password (otp: the copyable code, svelte-owjkr), its QR code (qr-code) and the parent's email
static void show_dialog(const cJSON *pending) {
    const char *otp = jstr(pending, "oneTimePassword");
    const char *url = jstr(pending, "url");
    snprintf(g_kid.code, sizeof(g_kid.code), "%s", otp ? otp : "");
    char desc[640];
    StrBuf name = {0};
    sb_append_esc(&name, perm_name(jstr(pending, "permission")));
    char *name_html = sb_finish(&name);
    I18N_F(desc, "kid.settings.request.desc",
           "Ask a parent to turn on {0}. They can scan the QR code, or go to <a href=\"https://asktoplay.com\" target=\"_blank\" rel=\"noopener noreferrer\">asktoplay.com</a> and enter this code:",
           name_html);
    free(name_html);
    StrBuf sb = {0};
    sb_append(&sb, "<div class=\"popup-container svelte-1wz7ymp\"><div class=\"popup svelte-1wz7ymp\" style=\"width: 800px;\">"
                   "<div class=\"io-popup-container\"><div class=\"io-popup-header\">");
    sb_append_esc(&sb, I18N("kid.settings.request.header", "Ask a Parent"));
    sb_append(&sb, "</div> <div class=\"io-popup-content\"><div class=\"io-card\"><p>");
    sb_append(&sb, desc); // description.html (97595)
    sb_append(&sb, "</p> <form><div class=\"io-input-container\">");
    if (otp && otp[0]) {
        sb_append(&sb, "<div class=\"io-section otp-container svelte-1xwmszg\"> <div class=\"copyable-container svelte-owjkr\">"
                       "<strong class=\"io-code-display svelte-owjkr\" onclick=\"knKidCopy()\">");
        sb_append_esc(&sb, otp);
        sb_append(&sb, "</strong> <div id=\"knKidCopied\"></div></div></div>");
    }
    char *qr = url && url[0] ? qr_svg_data_url(url) : NULL;
    if (qr) {
        sb_append(&sb, "<div class=\"io-section qr-code-container svelte-1xwmszg\"> <img alt=\"QR Code\" class=\"qr-code-image svelte-1xwmszg\" src=\"");
        sb_append(&sb, qr);
        sb_append(&sb, "\"></div>");
        free(qr);
    }
    sb_append(&sb, "<div class=\"label-row\"><label class=\"input-label\">");
    sb_append_esc(&sb, I18N("kid.settings.request.email", "Or send the request to their email:"));
    sb_append(&sb, "</label></div> <input class=\"io-input-field\" id=\"knKidEmail\" type=\"email\" placeholder=\"");
    sb_append_esc(&sb, I18N("windows.kid.consent.email.placeholder", "Enter your parent's email"));
    sb_append(&sb, "\"> <div class=\"io-button-group\"><button type=\"button\" class=\"io-button\" onclick=\"knViewPop(0)\">");
    sb_append_esc(&sb, I18N("kid.settings.request.close", "Close"));
    sb_append(&sb, "</button> <button type=\"submit\" class=\"io-button io-button--accept\" onclick=\"knKidSend()\">");
    sb_append_esc(&sb, I18N("kid.settings.request.send", "Send Email"));
    sb_append(&sb, "</button></div></div></form></div></div></div></div></div>");
    v10_view_push(sb_finish(&sb), NULL, dialog_closed, NULL);
}

// iÏíííïi: an age check opens its page; a parent's approval opens the dialog
static void show_pending(void) {
    const cJSON *pending = pending_of(g_kid.data);
    if (!pending) return;
    const char *type = jstr(pending, "type");
    if (type && !strcmp(type, KID_AGE_ASSURANCE)) {
        const char *url = jstr(pending, "url");
        if (url && url[0]) ui_open_browser(url); // window.open(url, "_blank", "noopener")
        set_result("ageCheck", 1, NULL);
        changed();
        return;
    }
    show_dialog(pending);
}

static void on_email(void *user, const int status, const cJSON *res) {
    if ((int) (intptr_t) user != g_kid.generation) return;
    const cJSON *data = status == 200 && res ? cJSON_GetObjectItem(res, "data") : NULL;
    const int sent = data && jtrue(data, "sent");
    set_result(sent ? "emailed" : "emailFailed", sent, NULL);
    changed();
}

static void send_email(void) {
    char email[256] = "";
    const HtmlElem in = html_dom_by_id("knKidEmail");
    if (in) html_dom_get_value(in, email, sizeof(email));
    v10_view_pop(1);
    if (!email[0]) return; // the dialog closes; nothing is sent without an address (97606)
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "email", email);
    char *text = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!text || !krunker_account_api("POST", KRUNKER_SVC_API, "/account/kid/permissions/email", text, on_email,
                                      (void *) (intptr_t) g_kid.generation)) {
        set_result("emailFailed", 0, NULL);
        changed();
    }
    free(text);
}

static void copy_cleared(void *user) {
    (void) user;
    g_kid.copy_timer = 0;
    const HtmlElem e = html_dom_by_id("knKidCopied");
    if (e) html_dom_set_html(e, "");
}

// the copyable code (svelte-owjkr, deob-10.0.4.js:15058-15068): to the clipboard, "Copied!" for 2 s
static void copy_code(void) {
    if (!g_kid.code[0]) return;
    html_clipboard_write(g_kid.code);
    const HtmlElem e = html_dom_by_id("knKidCopied");
    if (e) {
        StrBuf sb = {0};
        sb_append(&sb, "<div class=\"copy-feedback svelte-owjkr\">");
        sb_append_esc(&sb, I18N("generic.copied", "Copied!"));
        sb_append(&sb, "</div>");
        char *html = sb_finish(&sb);
        html_dom_set_html(e, html);
        free(html);
    }
    if (g_kid.copy_timer) html_timer_cancel(g_kid.copy_timer);
    g_kid.copy_timer = html_timer_add(2000, copy_cleared, NULL);
}

// ---------------------------------------------------------------- requestPermission (iÏîiìií)

typedef struct {
    int generation;
    char name[96];
} KidAsk;

static void on_request(void *user, const int status, const cJSON *res) {
    KidAsk *ask = user;
    const int current = ask->generation == g_kid.generation;
    char name[96];
    snprintf(name, sizeof(name), "%s", ask->name);
    free(ask);
    if (!current) return;
    g_kid.busy = 0;
    const cJSON *data = status == 200 && res ? cJSON_GetObjectItem(res, "data") : NULL;
    if (!data || !cJSON_IsObject(data)) {
        const cJSON *err = res ? cJSON_GetObjectItem(res, "error") : NULL;
        const char *code = jstr(err, "code");
        set_result(code && !strcmp(code, "KID_NO_SESSION") ? "noSession" : "failed", 0, NULL);
        changed();
        return;
    }
    apply(data);
    int on = 0;
    const cJSON *perm;
    cJSON_ArrayForEach(perm, cJSON_GetObjectItem(data, "permissions")) {
        const char *n = jstr(perm, "name");
        if (n && !strcmp(n, name)) {
            on = jtrue(perm, "enabled");
            break;
        }
    }
    const cJSON *pending = pending_of(data);
    const char *type = jstr(pending, "type");
    if (on) set_result("enabled", 1, perm_name(name));
    else if (pending && type && !strcmp(type, KID_AGE_ASSURANCE)) show_pending();
    else if (pending) {
        set_result("requested", 1, NULL);
        show_pending();
    }
    changed();
}

static void request_permission(const char *name) {
    if (g_kid.busy || !active() || !name || !name[0]) return;
    KidAsk *ask = calloc(1, sizeof(KidAsk));
    if (!ask) return;
    ask->generation = g_kid.generation;
    snprintf(ask->name, sizeof(ask->name), "%s", name);
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "name", name);
    char *text = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    g_kid.busy = 1;
    g_kid.result[0] = 0;
    if (!text || !krunker_account_api("POST", KRUNKER_SVC_API, "/account/kid/permissions", text, on_request, ask)) {
        free(ask);
        g_kid.busy = 0;
        set_result("failed", 0, NULL);
    }
    free(text);
    changed();
}

int kid_settings_handle(const char *name, const HtmlArg *args, const int argc) {
    if (!name || strncmp(name, "knKid", 5)) return -1;
    if (!strcmp(name, "knKidRequest")) {
        request_permission(argc > 0 && args[0].kind == HTML_ARG_STR ? args[0].str : NULL);
        return 0; // preventDefault (97497): the switch shows the answer, not the click
    }
    if (!strcmp(name, "knKidPending")) show_pending();
    else if (!strcmp(name, "knKidSend")) send_email();
    else if (!strcmp(name, "knKidCopy")) copy_code();
    else return -1;
    return 0; // the dialog's form is not submitted
}

// ---------------------------------------------------------------- the section

// a permission name as a handler argument: v10's names are ids ("text-chat-public"); anything else gets no control
static int safe_name(const char *name) {
    if (!name || !name[0]) return 0;
    for (const char *c = name; *c; c++)
        if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '-' || *c == '_' || *c == '.')) return 0;
    return 1;
}

static void state_span(StrBuf *sb, const char *cls, const char *text) {
    sb_appendf(sb, "<span class=\"%s\">", cls);
    sb_append_esc(sb, text);
    sb_append(sb, "</span>");
}

static void permission_row(StrBuf *sb, const cJSON *perm, const int is_active, const cJSON *pending) {
    const char *name = jstr(perm, "name");
    if (!name) return;
    const char *managed = jstr(perm, "managedBy");
    const int enabled = jtrue(perm, "enabled"), controls = safe_name(name);
    sb_append(sb, "<div class=\"settName\">");
    sb_append_esc(sb, perm_name(name));
    sb_append(sb, " ");
    if (managed && !strcmp(managed, "PLAYER")) {
        if (is_active && controls) { // the switch: turns the permission on; locked once on, busy while a request runs (97675-97686)
            sb_appendf(sb, "<label class=\"switch%s%s\" style=\"margin-left:10px\"><input type=\"checkbox\" onclick=\"knKidRequest('%s')\"%s%s> <span class=\"slider\"></span></label>",
                       enabled ? " kidLocked" : "", g_kid.busy ? " kidBusy" : "", name, enabled ? " checked" : "", enabled || g_kid.busy ? " disabled" : "");
        } else {
            state_span(sb, "kidState", enabled ? I18N("kid.settings.state.on", "On") : "");
        }
    } else if (managed && !strcmp(managed, "GUARDIAN")) {
        const char *pending_perm = jstr(pending, "permission");
        if (enabled) {
            state_span(sb, "kidState", I18N("kid.settings.state.parentOn", "On (approved by a parent)"));
        } else if (pending_perm && !strcmp(pending_perm, name)) { // its request waits: Show Code / Verify Age (97700-97709)
            const char *type = jstr(pending, "type");
            sb_append(sb, "<div class=\"updDisN kidBtn\" role=\"button\" tabindex=\"0\" onclick=\"knKidPending()\">");
            sb_append_esc(sb, type && !strcmp(type, KID_AGE_ASSURANCE) ? I18N("kid.settings.verifyAge", "Verify Age")
                                                                       : I18N("kid.settings.showCode", "Show Code"));
            sb_append(sb, "</div> ");
            state_span(sb, "kidState", I18N("kid.settings.state.waiting", "Waiting for a parent"));
        } else {
            if (is_active && controls) { // Ask a Parent (97711-97719)
                sb_appendf(sb, "<div class=\"updDisN kidBtn%s\" role=\"button\" tabindex=\"0\" onclick=\"knKidRequest('%s')\">", g_kid.busy ? " kidBusy" : "", name);
                sb_append_esc(sb, I18N("kid.settings.askParent", "Ask a Parent"));
                sb_append(sb, "</div>");
            }
            sb_append(sb, " ");
            state_span(sb, "kidState", I18N("kid.settings.state.needsParent", "Needs a parent's approval"));
        }
    } else {
        state_span(sb, "kidState kidOff", I18N("kid.settings.state.prohibited", "Not available for your age in your region"));
    }
    char key[200];
    snprintf(key, sizeof(key), "kid.settings.perm.%s.desc", name);
    if (i18n_has(key)) { // the description, when the table has one (97735-97741)
        sb_append(sb, "<div class=\"kidDesc\">");
        sb_append_esc(sb, i18n_t(key));
        sb_append(sb, "</div>");
    }
    sb_append(sb, "</div>");
}

void kid_settings_html(StrBuf *sb) {
    const cJSON *d = g_kid.data;
    const char *status = jstr(d, "status");
    if (!status || !strcmp(status, "none")) return;
    const int is_active = !strcmp(status, "active");
    char key[160];
    sb_append(sb, "<div class=\"setHedS\">");
    sb_append_esc(sb, I18N("kid.settings.header", "Privacy & Age"));
    sb_append(sb, "</div> <div class=\"setBodH\" style=\"padding-bottom:10px;\"><div class=\"kidInfo\">");
    sb_append_esc(sb, I18N("kid.settings.info", "Your account is age-verified with k-ID. These settings control which features you can use."));
    sb_append(sb, " ");
    // the age group (its table name, else the raw category) and the region (97638-97656)
    const char *age = jstr(d, "ageCategory");
    const char *region = jstr(d, "jurisdiction");
    const char *age_label = NULL;
    if (age && age[0]) {
        snprintf(key, sizeof(key), "kid.settings.age.%s", age);
        age_label = I18N(key, age);
    }
    if ((age_label && age_label[0]) || (region && region[0])) {
        sb_append(sb, "<span class=\"kidMeta\">");
        if (age_label && age_label[0]) {
            sb_append_esc(sb, I18N("kid.settings.ageCategory", "Age group"));
            sb_append(sb, ": <span class=\"kidValue\">");
            sb_append_esc(sb, age_label);
            sb_append(sb, "</span>");
        }
        if (age_label && age_label[0] && region && region[0]) sb_append(sb, "&nbsp;&middot;&nbsp;");
        if (region && region[0]) {
            sb_append_esc(sb, I18N("kid.settings.region", "Region"));
            sb_append(sb, ": <span class=\"kidValue\">");
            sb_append_esc(sb, region);
            sb_append(sb, "</span>");
        }
        sb_append(sb, "</span>");
    }
    sb_append(sb, "</div>");
    if (!is_active) { // kid.settings.status.<status> (97658-97664)
        // I18N("kid.settings.status.pending", "Your account is waiting for a parent to approve it. You can change these settings once they do.")
        // I18N("kid.settings.status.inactive", "Your k-ID settings can't be changed right now.")
        // I18N("kid.settings.status.prohibited", "Some features aren't available for your age where you live.")
        snprintf(key, sizeof(key), "kid.settings.status.%s", status);
        sb_append(sb, " <div class=\"kidInfo kidNote\">");
        sb_append_esc(sb, i18n_t(key));
        sb_append(sb, "</div>");
    }
    const cJSON *pending = pending_of(d);
    int player_on = 0;
    const cJSON *perm;
    cJSON_ArrayForEach(perm, cJSON_GetObjectItem(d, "permissions")) {
        sb_append(sb, " ");
        permission_row(sb, perm, is_active, pending);
        const char *managed = jstr(perm, "managedBy");
        if (managed && !strcmp(managed, "PLAYER") && jtrue(perm, "enabled")) player_on = 1;
    }
    if (player_on) { // iÏiîiîï: chat is the player's to hide (97745-97751)
        sb_append(sb, " <div class=\"kidDesc\">");
        sb_append_esc(sb, I18N("kid.settings.offHint", "You can hide chat any time in Settings > Chat."));
        sb_append(sb, "</div>");
    }
    if (g_kid.result[0]) {
        sb_appendf(sb, " <div class=\"kidMsg%s\">", g_kid.result_ok ? " kidOk" : "");
        sb_append_esc(sb, g_kid.result);
        sb_append(sb, "</div>");
    }
    sb_append(sb, "</div>");
}
