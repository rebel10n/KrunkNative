// The first-run choice (client/include/first_run_ui.h): one popup, asked once per device, over the
// v10 page's menu. Its markup is v10's popup shell (the login and Usernames popups: .view-stack >
// .view-stack-overlay > .view-wrapper > .popup-container > .popup > .io-popup-container), its three
// options are buttons in v10's colours (css/kn-v10-firstrun.css). The choice is written to html_storage
// (knFirstRun) before anything else happens; then the Server Type (knSeekServers) and the window
// follow it, and the start-up seek either runs (Box Office) or waits for the player (the others).
// The web build asks the same way (its localStorage is the device); on the web the Krunker choice
// says whether krunker.io is reachable from this bridge (krunker_relay.h), once the probe answered.
#include <first_run_ui.h>
#include <backend_ui.h>   // SERVERS_WINDOW_CONNECT, servers_ui_skip_startup_seek
#include <flavor.h>       // Krunker Strike has its own page and servers: no first-run popup
#include <krunker_relay.h> // the web's krunker.io note (krunker_relay_on / _probing)
#ifndef __EMSCRIPTEN__
#include <headless.h> // headless runs (the tests, renders) do not ask unless KRUNKNATIVE_FIRST_RUN=1
#endif
#include <html_ui.h>
#include <i18n.h>
#include <seek_prefs.h>   // the Server Type the choice sets
#include <ui_logic.h>     // menu_show_window, ui_scenario_active, sb_* helpers
#include <ui_mobile.h>    // ui_mobile_active: the touch layout (real pixels, css/kn-v10-firstrun.css)
#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FIRST_RUN_KEY "knFirstRun"
#define FIRST_RUN_CSS "css/kn-v10-firstrun.css"

static int g_checked;   // the stored value was read (once per process)
static int g_stored;   // a valid choice is stored on this device
static int g_chosen;   // answered in this session (the stored value is written, or it failed to be)
static int g_open;     // the popup is in the page
static int g_css;      // the stylesheet is added
static int g_note;     // private server chosen: the Connect window shows the IP and port note

static const char *choice_value(const FirstRunChoice c) {
    return c == FIRST_RUN_KRUNKER ? "krunker" : c == FIRST_RUN_BOX_OFFICE ? "boxoffice" : "private";
}

// html_storage_get: -1 when absent, the stored size otherwise. Only the three values count.
static int stored_choice(void) {
    char buf[32] = "";
    if (html_storage_get(FIRST_RUN_KEY, buf, sizeof(buf)) <= 0) return 0;
    return !strcmp(buf, "krunker") || !strcmp(buf, "boxoffice") || !strcmp(buf, "private");
}

int first_run_ui_applicable(void) {
    if (flavor_get() != FLAVOR_BASE) return 0; // Krunker Strike: its own page and its own servers
    if (!html_ui_available()) return 0;
#ifdef __EMSCRIPTEN__
    return 1; // the web build asks too (a fresh browser profile is a fresh device)
#else
    const char *env = getenv("KRUNKNATIVE_FIRST_RUN"); // 1: ask in a headless run too, 0: never ask
    if (env && !strcmp(env, "0")) return 0;
    if (env && !strcmp(env, "1")) return 1;
    return !headless_enabled() && !ui_scenario_active();
#endif
}

int first_run_ui_decided(void) {
    if (g_chosen) return 1;
    if (!g_checked) {
        g_checked = 1;
        g_stored = stored_choice();
    }
    return g_stored;
}

int first_run_ui_wait(void) { return first_run_ui_applicable() && !first_run_ui_decided(); }

int first_run_ui_connect_note(void) { return g_note; }

// ---------------------------------------------------------------- the popup

// The web's note under the Krunker option: whether krunker.io is reachable from this bridge
// (krunker_relay.h). NULL natively, where krunker.io is joined directly.
static const char *web_krunker_note(void) {
#ifdef __EMSCRIPTEN__
    return krunker_relay_on()
        ? I18N("kn.kio.relayNotBuilt", "krunker.io's relay is on here, but joining krunker.io from the browser is not in this build yet. Box Office servers still work.")
        : I18N("kn.kio.relayOff", "krunker.io is not reachable from this server right now. Box Office servers still work.");
#else
    return NULL;
#endif
}

static void option_html(StrBuf *sb, const int index, const int recommended, const char *name, const char *tag, const char *desc, const char *note) {
    sb_appendf(sb, "<button type=\"button\" class=\"kn-firstrun-option%s\" id=\"knFirstRunOpt%d\" onclick=\"knFirstRun(%d)\">",
               recommended ? " kn-firstrun-option--recommended" : "", index, index);
    if (tag) {
        sb_append(sb, "<span class=\"kn-firstrun-tag\">");
        sb_append_esc(sb, tag);
        sb_append(sb, "</span>");
    }
    sb_append(sb, "<span class=\"kn-firstrun-name\">");
    sb_append_esc(sb, name);
    sb_append(sb, "</span><span class=\"kn-firstrun-desc\">");
    sb_append_esc(sb, desc);
    sb_append(sb, "</span>");
    if (note) { // the web's krunker.io state (web_krunker_note): said plainly, the option stays
        sb_append(sb, "<span class=\"kn-firstrun-desc kn-firstrun-warn\">");
        sb_append_esc(sb, note);
        sb_append(sb, "</span>");
    }
    sb_append(sb, "</button>");
}

static char *popup_html(void) {
    // the touch layout (ui_mobile.h: the phone / tablet controls) is not scaled with the page: the
    // page's design scale would make its options a few pixels high on a phone, so the popup keeps
    // real pixels there (css/kn-v10-firstrun.css, .kn-firstrun-touch)
    StrBuf sb = {0};
    sb_appendf(&sb, "<div class=\"view-stack svelte-sdh4ve%s\" id=\"knFirstRun\"><div class=\"view-stack-overlay svelte-sdh4ve\" onclick=\"knFirstRunBlock()\">",
               ui_mobile_active() ? " kn-firstrun-touch" : "");
    sb_append(&sb, "<div class=\"view-wrapper svelte-sdh4ve\" style=\"display: block\"><div class=\"popup-container svelte-1wz7ymp\">"
                   "<div class=\"popup svelte-1wz7ymp kn-firstrun\" onclick=\"knFirstRunBlock()\"><div class=\"io-popup-container\">"
                   "<div class=\"io-popup-header\">");
    sb_append_esc(&sb, I18N("kn.firstRun.title", "Choose your servers"));
    sb_append(&sb, "</div><div class=\"io-popup-content kn-firstrun-body\"><p class=\"kn-firstrun-lead\">");
    sb_append_esc(&sb, I18N("kn.firstRun.lead", "Krunker Box Office plays on two kinds of server. Pick where to start. "
                                                 "Your choice is kept on this device."));
    sb_append(&sb, "</p>");
    option_html(&sb, FIRST_RUN_KRUNKER, 1, I18N("kn.firstRun.krunker", "Krunker servers"), I18N("kn.firstRun.recommended", "Recommended"),
                I18N("kn.firstRun.krunker.desc", "krunker.io's own servers, joined right here. Use your krunker.io account, or play as a guest."),
                web_krunker_note());
    option_html(&sb, FIRST_RUN_BOX_OFFICE, 0, I18N("kn.firstRun.boxOffice", "Box Office"), NULL,
                I18N("kn.firstRun.boxOffice.desc", "What krunk.io players use. A fair private server hosted by the developers, with its own economy."), NULL);
    option_html(&sb, FIRST_RUN_PRIVATE, 0, I18N("kn.firstRun.private", "Private server"), NULL,
                I18N("kn.firstRun.private.desc", "Connect by IP and port. If you were given an IP and port, use this."), NULL);
    sb_append(&sb, "</div></div></div></div></div></div></div>");
    return sb_finish(&sb);
}

static void close_popup(void) {
    g_open = 0;
    HtmlElem host = html_dom_by_id("knFirstRun");
    if (host) html_dom_remove(host);
}

static void open_popup(void) {
    if (g_open) return;
    if (html_dom_by_id("knFirstRun")) {
        g_open = 1;
        return;
    }
    HtmlElem body = html_dom_body();
    if (!body) return;
    if (!g_css) {
        html_ui_add_stylesheet(FIRST_RUN_CSS);
        g_css = 1;
    }
    char *html = popup_html();
    if (!html) return;
    html_dom_insert_html(body, HTML_INSERT_BEFORE_END, html);
    free(html);
    g_open = 1;
}

// ---------------------------------------------------------------- the choice

static void choose(const FirstRunChoice c) {
    if (g_chosen) return;
    g_chosen = 1;
    html_storage_set(FIRST_RUN_KEY, choice_value(c)); // first: the answer is kept whatever follows
    close_popup();
    switch (c) {
    case FIRST_RUN_KRUNKER:
        // the Server Type Quick Match and F4 seek into (seek_prefs.h), then the server list; the
        // krunker.io account keeps the routing backend_ui.h gives it
        seek_prefs_set_servers(SEEK_SERVERS_OFFICIAL);
        servers_ui_skip_startup_seek();
        menu_show_window(2, 0);
        if (web_krunker_note()) ui_notify(web_krunker_note()); // the web: krunker.io's state, once, as a toast
        break;
    case FIRST_RUN_BOX_OFFICE:
        // Box Office stays the backend: the start-up seek runs as it always did
        seek_prefs_set_servers(SEEK_SERVERS_KN);
        break;
    case FIRST_RUN_PRIVATE:
        g_note = 1;
        servers_ui_skip_startup_seek();
        menu_show_window(SERVERS_WINDOW_CONNECT, 0);
        {
            HtmlElem input = html_dom_by_id("connectAddr");
            if (input) html_dom_focus(input);
        }
        break;
    }
}

void first_run_ui_tick(Client *client) {
    if (g_open || !first_run_ui_wait()) return;
    if (krunker_relay_probing()) return; // the web: the popup's krunker.io note waits for the bridge's answer
    if (client && (client->in_game || client->net_connected)) return; // a game is joined: asked when the menu is back
    open_popup();
}

void first_run_ui_scenario(const cJSON *state) {
    if (cJSON_IsTrue(state)) open_popup(); // the popup over the menu, nothing stored
}

int first_run_ui_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) ev;
    if (!name) return -1;
    if (!strcmp(name, "knFirstRunBlock")) { // the popup and its backdrop keep the click to themselves
        html_ui_stop_propagation();
        return 1;
    }
    if (strcmp(name, "knFirstRun") != 0) return -1;
    html_ui_stop_propagation();
    const int n = argc > 0 && args[0].kind == HTML_ARG_NUM ? (int) args[0].num : -1;
    if (n == FIRST_RUN_KRUNKER || n == FIRST_RUN_BOX_OFFICE || n == FIRST_RUN_PRIVATE) choose((FirstRunChoice) n);
    return 1;
}
