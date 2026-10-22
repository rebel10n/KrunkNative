// Krunker Strike's connection popups (strike_ui.h). FRVR 1.37.2's disconnect handling, read as text
// from assets/index-Cz2uYXJQ.js (the .deob.js copy keeps the names):
//   oN(error, ...)  "GAME UPDATING" or NoServersMatchQuery: P1({message: "Game Updating"}, {hideAction})
//                   (no button; nothing reconnects); otherwise vT(), the "Reconnecting..." loading
//                   screen (Fm(Al, {info})), and the reconnect (uSe: 50 tries within 10 s); its failure
//                   calls dSe
//   dSe(error, ...) P1({message: [title "Lost Connection", small: error, br, "ID: 0x<code>", br "Code: n",
//                   br "Reason: r"]}, {onAction: kt() + backToOrigin, actionText: "Reconnect"})
// P1 is Pt(UI) = POPUP_WARNING: ui/strike/screens/POPUP_WARNING.html. Pt pushes it over the stack
// (preventReplace), and RA() completes the loading screen it covers, as strike_screen_push does.
// Here menu_ui.c reports the transport's state (strike_connection_state); the popup is kept in step
// with it on each frame (strike_connect_tick), so it stays over the screens the way FRVR's Pi keeps it.
// Reconnect runs menu_retry_connect (the same server again, or a fresh seek on the brain), where
// FRVR reloads the page (backToOrigin).
#include <strike_ui.h>
#include <html_dom.h>
#include <html_ui.h>
#include <i18n.h>
#include <ui_logic.h>
#include <shared.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONNECT_TEXT_MAX 160

enum { POPUP_NONE = 0, POPUP_LOST, POPUP_UPDATING };

static struct {
    int popup;                  // POPUP_*: what the popup shows now
    int reconnecting;           // the loading screen shows "Reconnecting..." for the reconnect under way
    int dirty;                  // the popup's content changed since it was rendered
    int held;                   // --render-scenario: the fixture's state stays (the offline client is "connected")
    char error[CONNECT_TEXT_MAX];
    char code[16];
    int close_code;
    char close_reason[32];
    int reconnect_timer;        // the deadline of the reconnect under way (html_timer id, 0 none)
} C;

// A reconnect that is still going after RECONNECT_WAIT_MS is a lost connection: the curtain opens on the popup
// with its error, rather than "Reconnecting..." for good
#define RECONNECT_WAIT_MS 15000
static void reconnect_expired(void *user);

static void set_popup(const int popup) {
    if (C.popup != popup) C.dirty = 1;
    C.popup = popup;
}

// Lost Connection's text: the title, the server's message, then "ID: 0x<code>" and the socket's "Code:"
// and "Reason:" when it gave them (FRVR's dSe).
static char *lost_markup(void) {
    StrBuf sb = {0};
    sb_append(&sb, "<span><p class=\"title-text\">Lost Connection</p><p class=\"small-text\">");
    sb_append_esc(&sb, C.error);
    sb_append(&sb, "<br>ID: 0x");
    sb_append_esc(&sb, C.code[0] ? C.code : "000000");
    if (C.close_code) sb_appendf(&sb, "<br>Code: %d", C.close_code);
    if (C.close_reason[0]) {
        sb_append(&sb, "<br>Reason: ");
        sb_append_esc(&sb, C.close_reason);
    }
    sb_append(&sb, "</p></span>");
    return sb_finish(&sb);
}

// POPUP_WARNING's fill: the message (Game Updating has no button: hideAction)
void strike_connect_render(const HtmlElem root) {
    const HtmlElem text = html_dom_query(root, ".alert-text");
    if (!text) return;
    if (C.popup == POPUP_UPDATING) {
        html_dom_set_html(text, "<span>Game Updating</span>");
        const HtmlElem button = html_dom_query(root, ".frvr-button");
        if (button) html_dom_remove(button);
        return;
    }
    char *html = lost_markup();
    if (html) html_dom_set_html(text, html);
    free(html);
}

void strike_connection_state(const StrikeConnState state, const char *error, const char *code, const int close_code, const char *close_reason) {
    if (C.held && state == STRIKE_CONN_CONNECTED) return;
    // a join refused for the server password: the password prompt (strike_menu.c) asks instead of a Lost Connection
    if (state == STRIKE_CONN_LOST && strike_screen_shown(STRIKE_SCREEN_SERVER_PASSWORD)) return;
    if (state == STRIKE_CONN_RECONNECTING) {
        if (!C.reconnecting) {
            C.reconnecting = 1;
            strike_loading_show("Reconnecting...", 0);
        }
        html_timer_cancel(C.reconnect_timer);
        C.reconnect_timer = html_timer_add(RECONNECT_WAIT_MS, reconnect_expired, NULL);
        return;
    }
    if (state == STRIKE_CONN_LOST) {
        // a repeat of the same error changes nothing; a new one is rendered again (strike_connect_tick)
        char text[CONNECT_TEXT_MAX], id[16], reason[32];
        snprintf(text, sizeof(text), "%s", error ? error : "");
        snprintf(id, sizeof(id), "%s", code ? code : "");
        snprintf(reason, sizeof(reason), "%s", close_reason ? close_reason : "");
        if (strcmp(text, C.error) || strcmp(id, C.code) || close_code != C.close_code || strcmp(reason, C.close_reason)) C.dirty = 1;
        memcpy(C.error, text, sizeof(text));
        memcpy(C.code, id, sizeof(id));
        memcpy(C.close_reason, reason, sizeof(reason));
        C.close_code = close_code;
    }
    if (state == STRIKE_CONN_CONNECTED || state == STRIKE_CONN_UPDATING || state == STRIKE_CONN_LOST) {
        html_timer_cancel(C.reconnect_timer);
        C.reconnect_timer = 0;
        if (C.reconnecting) { // the reconnect is over: the curtains open (RA)
            C.reconnecting = 0;
            strike_loading_complete();
        }
    }
    set_popup(state == STRIKE_CONN_LOST ? POPUP_LOST : state == STRIKE_CONN_UPDATING ? POPUP_UPDATING : POPUP_NONE);
}

static void reconnect_expired(void *user) {
    (void) user;
    C.reconnect_timer = 0;
    if (!C.reconnecting) return;
    strike_connection_state(STRIKE_CONN_LOST, I18N("kn.strike.reconnectFailed", "Couldn't reconnect to the server."), NULL, 0, "");
}

void strike_connect_tick(void) {
    const int shown = strike_screen_shown(STRIKE_SCREEN_POPUP_WARNING);
    if (C.popup == POPUP_NONE) {
        if (shown) {
            printf("[strike] popup %s closed\n", strike_screen_id(STRIKE_SCREEN_POPUP_WARNING));
            strike_screen_remove(STRIKE_SCREEN_POPUP_WARNING);
        }
        C.dirty = 0;
        return;
    }
    if (!shown) strike_screen_push(STRIKE_SCREEN_POPUP_WARNING); // Pt: the fill runs in strike_menu_render
    else if (C.dirty) strike_screen_rerender(STRIKE_SCREEN_POPUP_WARNING);
    C.dirty = 0;
}

int strike_connect_action(const char *name) {
    if (!name || strcmp(name, "reconnect") != 0) return 0;
    printf("[strike] reconnect\n");
    // kt(): the popup goes; with no screen left the menu is back, as "back" does
    set_popup(POPUP_NONE);
    strike_connect_tick();
    if (strike_screen_depth() == 0 && html_ui_is_menu_open()) strike_screen_push(STRIKE_SCREEN_MAIN_MENU);
    // FRVR reloads the page (backToOrigin) under "Reconnecting..."; here the same server is tried again
    strike_connection_state(STRIKE_CONN_RECONNECTING, NULL, NULL, 0, NULL);
    menu_retry_connect();
    return 1;
}

// state.strike.connection (tools/uidiff/scenarios/strike/connection-*.json):
//   {"state": "lost" | "updating" | "reconnecting", "error", "code", "closeCode", "reason"}
void strike_connect_scenario(const cJSON *state) {
    const cJSON *c = cJSON_GetObjectItemCaseSensitive(state, "connection");
    if (!cJSON_IsObject(c)) return;
    C.held = 1; // the scenario's connection is the picture: the offline client's "connected" does not clear it
    const cJSON *kind = cJSON_GetObjectItemCaseSensitive(c, "state");
    const char *k = cJSON_IsString(kind) ? kind->valuestring : "";
    const cJSON *error = cJSON_GetObjectItemCaseSensitive(c, "error");
    const cJSON *code = cJSON_GetObjectItemCaseSensitive(c, "code");
    const cJSON *close = cJSON_GetObjectItemCaseSensitive(c, "closeCode");
    const cJSON *reason = cJSON_GetObjectItemCaseSensitive(c, "reason");
    if (!strcmp(k, "reconnecting")) {
        strike_connection_state(STRIKE_CONN_RECONNECTING, NULL, NULL, 0, NULL);
    } else if (!strcmp(k, "updating")) {
        strike_connection_state(STRIKE_CONN_UPDATING, NULL, NULL, 0, NULL);
    } else if (!strcmp(k, "lost")) {
        strike_connection_state(STRIKE_CONN_LOST, cJSON_IsString(error) ? error->valuestring : "",
                                cJSON_IsString(code) ? code->valuestring : "000000", cJSON_IsNumber(close) ? close->valueint : 0,
                                cJSON_IsString(reason) ? reason->valuestring : "");
    } else {
        return;
    }
    strike_connect_tick();
}
