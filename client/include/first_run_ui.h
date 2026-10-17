#pragma once
// The first-run choice: where a new install plays. Asked once per device (not per account), on
// the native base game's menu (the PC client and its phone / tablet touch layout, ui_mobile.h):
//
//   Krunker servers   the default for the Server Type (seek_prefs.h: official krunker.io lobbies,
//                     Quick Match / F4 seek into them), and the server list opens (window 2).
//                     Which backend the krunker.io account routes through stays backend_ui.h's
//                     (account_unify): this sets the choice only.
//   Box Office        the Server Type stays Box Office (KrunkNative servers); the start-up seek
//                     runs as it always did.
//   Private server    the Connect window (SERVERS_WINDOW_CONNECT, the host:port form) with a note:
//                     if you were given an IP and port, use this.
//
// On the web build too (a fresh browser profile is a fresh device): the same popup, its Krunker option
// says whether krunker.io is reachable from this bridge (krunker_relay.h, asked at start-up; off when the
// bridge does not report the relay). Never in Krunker Strike (its own page, its own servers), and never in headless runs unless
// KRUNKNATIVE_FIRST_RUN=1 asks for it (the e2e tests run headless; KRUNKNATIVE_FIRST_RUN=0 turns it
// off). The modal is the v10 page's own popup (client/src/ui/first_run_ui.c, css/kn-v10-firstrun.css):
// it has no close button and ignores the backdrop, so it is always answered.
//
// Stored under html_storage key "knFirstRun" ("krunker" | "boxoffice" | "private"; native:
// $XDG_CONFIG_HOME/krunknative/knFirstRun.json, so per device; web: localStorage, per browser profile). A missing or unreadable value means
// not decided, the modal shows again. Written when the choice is made; never shown again after that.
#include <client.h>
#include <html_dom.h>
#include <cJSON.h>

typedef enum {
    FIRST_RUN_KRUNKER = 0,
    FIRST_RUN_BOX_OFFICE = 1,
    FIRST_RUN_PRIVATE = 2,
} FirstRunChoice;

// The modal can show in this process (base game, a page to draw it on, not headless unless forced).
int first_run_ui_applicable(void);
// A choice is stored (or made in this session).
int first_run_ui_decided(void);
// The launch waits for the choice: applicable and not decided. The start-up seek (servers_ui.c)
// and the modal's opening both follow it.
int first_run_ui_wait(void);
// Per frame, once the menu's page is up (menu_ui.c): opens the modal when it is due and no game
// is connected.
void first_run_ui_tick(Client *client);
// Page handlers: knFirstRun(0 | 1 | 2) (the three options), knFirstRunBlock() (swallows clicks on the
// popup and the backdrop). Returns -1 for other names.
int first_run_ui_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, int argc);
// The Connect window's first-run note (the private server choice this session): 1 = show it.
int first_run_ui_connect_note(void);
// --render-scenario state.firstRun: true opens the modal over the menu (decided or not, nothing stored).
void first_run_ui_scenario(const cJSON *state);
