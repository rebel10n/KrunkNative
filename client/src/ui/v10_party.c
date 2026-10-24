// Krunker v10.0.2 Invite / Join (client/include/v10_party.h). Readable v10 sources (the
// 10.0.2 client, ~/jails/kru/src/Krunker_v10.0.3/deob/v10.0.2/game.js): window.copyInviteLink, openJoinWindow, joinGame
// and its parser (_72569: krunker.io|127.0.0.1:8080 /?(party|game)= links, /^([A-Z]+):(\w+)$/
// codes), the matchInfoHolder Svelte component (svelte-1vrfz9w: Invite = playSelect +
// copyInviteLink, Join = playSelect + openJoinWindow), the social panel's conversation header
// Join (svelte-128yn5i: switchServer(friend.gameInstanceId)), windows[4] tab 1 (the Friends tab:
// openURL(`/?game=${gid}`, 1)) and gameCode / submitGameCode (ENTER_CODE).
//
// KrunkNative differences (intentional):
//  - links name our sites (servers_ui.c / shared/src/server_names.c kn_game_link_parse: any of
//    them, the short CH2:kz862 ids too) and the native client joins in place where v10 navigates
//    the page (location.href = link): the web build reloads with ?game= exactly like v10.
//  - ?party= links: v10 navigates to them, but no v10 page reads ?party= any more (a fresh
//    game): here they open the Join window, like a link that names no game.
//  - the server password: v10's server asks after the join (ENTER_CODE, a countdown on Submit,
//    CODE_PASSED); server_bin refuses the join instead (ERROR 004005, gameserver.password) and
//    the client asks with the same popup, then joins the same game again with the password in
//    its JOIN packet (servers_ui_join_password). No countdown. Leave seeks a new game (v10:
//    location = location.origin, a fresh page = a fresh game).
#include <v10_party.h>
#include "klog.h"
#include <backend_ui.h>
#include <html_ui.h>
#include <ui_logic.h>
#include <server_names.h> // kn_game_link_parse
#include <krunker_net.h>  // the official lobby's "code": krunker_net_send_packet, krunker_net_leave
#include <net_ext.h>      // NET_EXT_KN_OFFICIAL_GAME_CODE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INVITE_RESET_MS 1250.0 // copyInviteLink: setTimeout(..., 1250)
#define SERVER_PASSWORD_MAX sizeof(((NetJoinPacket *) 0)->password) // the JOIN packet's field, NUL included

static int g_invite_timer;
static char *g_clipboard;   // openJoinWindow: the clipboard's text, joined from a timer (outside the read)
static int g_clipboard_timer;

// ------------------------------------------------------------------ Invite

static void invite_reset(void *user) {
    (void) user;
    g_invite_timer = 0;
    HtmlElem b = html_dom_by_id("inviteButton");
    if (b) html_dom_set_text(b, "Invite"); // inviteButton.textContent = "Invite"
}

void v10_party_copy_invite_link(const int toast) {
    char link[512];
    servers_ui_invite_link(link, sizeof(link)); // "https://" + location.host + "/?game=" + gameId
    html_clipboard_write(link);
    KN_LOG_DEBUG1("[party] invite link copied: %s\n", link);
    HtmlElem b = html_dom_by_id("inviteButton");
    if (b) html_dom_set_text(b, "Copied URL"); // inviteButton.textContent
    if (g_invite_timer) html_timer_cancel(g_invite_timer);
    g_invite_timer = html_timer_add(INVITE_RESET_MS, invite_reset, NULL);
    // _69625(-1, null, "Link Copied. ...", 2): a system chat line
    if (toast) hud_ui_add_chat(NULL, "Link Copied. Send it to someone to invite them", "#fc03ec");
}

// ------------------------------------------------------------------ Join

// The clipboard's text (and the Join button's text when no field is open): a refused text reopens the
// Join window (showWindow(24)) with the error as a notice.
void v10_party_join_text(const char *text) {
    if (!servers_ui_join_link(text ? text : "")) menu_show_window(MENU_WINDOW_JOIN, 0);
}

// The Join window's field (joinGame()): a refused text keeps the window as it is, the typed text with it.
void v10_party_join_field(const char *text) {
    servers_ui_join_link(text ? text : "");
}

static void clipboard_join(void *user) {
    (void) user;
    g_clipboard_timer = 0;
    char *text = g_clipboard;
    g_clipboard = NULL;
    if (text) KN_LOG_DEBUG1("[party] join from the clipboard: %s\n", text);
    if (text) v10_party_join_text(text);
    else menu_show_window(MENU_WINDOW_JOIN, 0); // readText() refused: .catch -> showWindow(24)
    free(text);
}

static void on_clipboard(void *user, const char *text) {
    (void) user;
    free(g_clipboard);
    g_clipboard = text ? strdup(text) : NULL;
    // natively the text comes back inside the click's dispatch, on the web from a promise: the
    // join runs from the page's timers either way
    if (!g_clipboard_timer) g_clipboard_timer = html_timer_add(0.0, clipboard_join, NULL);
}

void v10_party_open_join_window(const int skip_clipboard) {
    if (skip_clipboard) {
        menu_show_window(MENU_WINDOW_JOIN, 0);
        return;
    }
    html_clipboard_read(on_clipboard, NULL);
}

int v10_party_join_game(const char *game_id) {
    if (!game_id || !game_id[0]) return 0;
    KN_LOG_DEBUG1("[party] join friend's game %s\n", game_id);
    return servers_ui_join_game(game_id);
}

// The page's own site: a relative URL, this page's origin or the public web client's
static int same_site(const char *url) {
    if (url[0] == '/' || url[0] == '?' || !strncmp(url, "./", 2)) return url[0] != '/' || url[1] != '/';
    char origin[256];
    html_page_origin(origin, sizeof(origin));
    const char *sites[] = {origin, KN_PUBLIC_ORIGIN};
    for (size_t i = 0; i < sizeof(sites) / sizeof(sites[0]); i++) {
        const size_t n = strlen(sites[i]);
        if (n && !strncmp(url, sites[i], n) && (url[n] == '/' || url[n] == '?' || !url[n])) return 1;
    }
    return 0;
}

int v10_party_open_url(const char *url) {
    char id[160];
    if (!url || !same_site(url) || !strstr(url, "game=") || !kn_game_link_parse(url, id, sizeof(id))) return 0;
    if (!servers_ui_join_game(id) && !servers_ui_join_link(url)) menu_show_window(MENU_WINDOW_JOIN, 0); // no matchmaker: the Join window
    return 1;
}

// ------------------------------------------------------------------ server password

// genericPopup(html, cls): #genericPop over #popupBack
static void generic_popup(const char *html, const char *cls) {
    ui_loadout_clear_pops();
    // v10 resetPops() hides #twitchPop as well (an empty .twitchPop card would show behind)
    HtmlElem twitch = html_dom_by_id("twitchPop");
    if (twitch) html_dom_set_display(twitch, "none");
    HtmlElem holder = html_dom_by_id("popupHolder");
    if (holder) html_dom_set_display(holder, "block");
    HtmlElem pop = html_dom_by_id("genericPop");
    if (!pop) return;
    html_dom_set_attr(pop, "class", cls);
    html_dom_set_attr(pop, "style", "");
    html_dom_set_html(pop, html);
    html_dom_set_display(pop, "block");
    menu_schedule_rect();
}

static int g_code_official; // the shown password popup is an official lobby's ("code"), not our server's refusal

void v10_party_password_prompt(void) {
    g_code_official = 0;
    printf("[party] the game needs a password\n");
    // window.gameCode() (Leave: v10's onclick='location=location.origin')
    // i18n custom.passcode.input; the placeholder and the buttons are English in v10's markup
    char html[640];
    snprintf(html, sizeof(html), "<div style='color:#fff;font-size:20px;position:relative;'>%s</div>"
                                 "<input type='text' id='codeInput' placeholder='Password'>"
                                 "<div id='confirmBtn' onclick='submitGameCode()' onmouseover='playTick(0.1)'>Submit</div>"
                                 "<div id='declineBtn' onclick='knGameCodeLeave()' onmouseover='playTick(0.1)'>Leave</div>",
             I18N("custom.passcode.input", "Enter Server Password to join"));
    generic_popup(html, "gameCodePop");
    HtmlElem input = html_dom_by_id("codeInput");
    if (input) html_dom_focus(input);
}

void v10_party_password_prompt_official(void) {
    v10_party_password_prompt();
    g_code_official = 1;
    printf("[party] the official lobby needs a password\n");
}

void v10_party_code_timer(const int seconds) {
    HtmlElem btn = html_dom_by_id("confirmBtn"); // iïiìïíï (:219224): confirmBtn.innerHTML = "Submit - " + n + "s"
    if (!btn || !g_code_official) return;
    char text[32];
    snprintf(text, sizeof(text), "Submit - %ds", seconds);
    html_dom_set_html(btn, text);
}

void v10_party_code_passed(void) {
    if (!g_code_official) return;
    g_code_official = 0;
    ui_loadout_clear_pops(); // CODE_PASSED: clearPops (:219221)
    KN_LOG_DEBUG1("[party] the official lobby took the password\n");
}

static void submit_game_code(void) {
    char code[64] = "";
    HtmlElem input = html_dom_by_id("codeInput");
    if (input) html_dom_get_value(input, code, sizeof(code));
    if (!code[0]) return; // _72781.value && send(CODE, value)
    if (g_code_official) { // submitGameCode (:219238): send(CODE, value); the popup stays until "codeP"
        uint8_t ext[NET_EXT_HEADER_SIZE + sizeof(code)];
        const uint16_t n = net_ext_pack(ext, sizeof(ext), NET_EXT_KN_OFFICIAL_GAME_CODE, code, (uint16_t) (strlen(code) + 1));
        if (n) krunker_net_send_packet(NET_PACKET_EXT, ext, n);
        KN_LOG_DEBUG1("[party] password sent to the official lobby\n");
        return;
    }
    if (strlen(code) >= SERVER_PASSWORD_MAX) {
        ui_alert(I18N("custom.passcode.long", "Password too long"));
        return;
    }
    ui_loadout_clear_pops(); // CODE_PASSED: clearPops (a wrong one is refused again: the popup again)
    servers_ui_join_password(code);
}

int v10_party_ui_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) ev;
    (void) args;
    (void) argc;
    if (!name) return -1;
    if (!strcmp(name, "submitGameCode")) {
        submit_game_code();
        return 1;
    }
    if (!strcmp(name, "knGameCodeLeave")) {
        ui_loadout_clear_pops();
        if (g_code_official) { // location = location.origin: the official game is left, back to the menu
            g_code_official = 0;
            krunker_net_leave(menu_ui()->client, NULL);
            menu_show_window(MENU_WINDOW_NONE, 0);
            return 1;
        }
        if (!servers_ui_retry()) menu_show_window(MENU_WINDOW_NONE, 0); // location = location.origin
        return 1;
    }
    return -1;
}
