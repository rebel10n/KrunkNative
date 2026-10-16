// The in-game HUD on the Krunker v10.0.2 page (ui/): what v10 does differently from v7.1.4's legacy
// HUD DOM, which it keeps (client/src/ui/hud_v10.c). hud_v10_page(): the main page is loaded and
// ready (not the hub / editor page).
#ifndef HUD_V10_H
#define HUD_V10_H

#include <stddef.h>

// 1 when the loaded page is the v10 one (v10_menu_page()), cached per page load.
int hud_v10_page(void);

// Per frame (client_hud_html_tick): v10's #chatUIHolder (hideChatIfGuestUser), and nothing of it
// until the player is in a game (in_game = client->in_game: entered, dead, spectating, the end screen).
void hud_v10_tick(int in_game);

// GUI.endScreen.tabs.performance.gen() of v10 (Combat Earnings + Combat Record) for the current
// match: kr_html is the earned-KR cell ("NONE" = nothing earned), the rest the local player's
// end stats. Appends to out.
void hud_v10_end_performance_html(char *out, size_t size, size_t *len, const char *kr_html, int score, int kills,
                                  int deaths, int rank);

// The match-end signup incentive (#matchEndSignupIncentives, v10's signup rewards component with
// origin match_end) for a guest; hidden for an account. Called when the results come up.
void hud_v10_end_signup(int show);

// Page handlers of this module (hud_ui_handle forwards unknown names here): 1 when handled.
int hud_v10_handle(const char *name);

#endif
