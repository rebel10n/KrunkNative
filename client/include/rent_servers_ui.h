#pragma once
// Rent Dedicated Server (Krunker v10.0.7 hostTypes[2], buyServ): client/src/ui/rent_servers_ui.c,
// doc/v10/rented-servers.md. The Host Game window with host type 2 draws these pages; the frames go to kn_brain's
// social socket (services/brain/rentals.c).
#include <cJSON.h>
#include <html_dom.h>
#include <ui_logic.h> // StrBuf

// The window opened (openHostWindow(false, 2)): the Rent Server tab, the list asked for again.
void rent_servers_ui_open(void);
// The window's markup (the Host Game window's generator for host type 2).
char *rent_servers_ui_gen(void);
// windows.7.switchTab2, windows.7.dediCst, rentDediServer; -1 when the name is not ours.
int rent_servers_ui_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, int argc);
// A social frame ["dsrvl", ...] (DEDI_BUY_RESPONSE).
void rent_servers_ui_frame(const cJSON *frame);
// showGames(2): the Games window picks the rented server's game.
void rent_servers_ui_browse(void);
// 1 while the Games window is picking for a rental (selectHostMap goes to rent_servers_ui_pick).
int rent_servers_ui_picking(void);
// selectHostMap during a rental pick: the game (name, creator, catalogue key or "", id or ""); back to the window.
void rent_servers_ui_pick(const char *name, const char *creator, const char *key, const char *id);
// The Host chooser's (window 42) Rent a Server card: v7.1.4's Dedicated card, the only way into the window.
void rent_servers_ui_chooser_card(StrBuf *sb);
// A scenario's fixture {rows: [...], game: "name"}: the list (kept through the window's open) and a chosen game.
void rent_servers_ui_scenario(const cJSON *fixture);
// iííìïïï(ms) without seconds (deob-10.0.7.js:23957): "1y 2d 3h 4m " style, for Time remaining.
void rent_servers_ui_time(double ms, char *out, size_t size);
