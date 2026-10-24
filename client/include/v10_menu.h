#pragma once
// Krunker v10.0.3 main menu shell on the v10 page (ui/; client/src/ui/v10_menu.c, doc/v10/ui.md 2):
// the action cards (Quick Match / Host Game / Find Game / Custom Games), the side nav (order,
// visibility, actions), the header bar (guest / signed in, level, KR, Settings, More Krunker), the
// match-info bar under the class preview and the login / register modal (loginOrRegister).
#include <html_dom.h>

int  v10_menu_page(void);                // the loaded page is v10's (#menuBtnQuickMatch exists)
void v10_menu_dom_init(void);            // menu_dom_init: wire the Svelte markup (no-op on other pages)
void v10_menu_account_changed(void);     // account_ui: login / logout / funds / level -> header + nav
void v10_menu_scenario_official(int on);
int  v10_menu_official_session(void);     // in a krunker.io server (krunker_net_active), or a scenario's officialSession   // --render-scenario state.officialSession: the header as in a krunker.io server
void v10_menu_account_tick(void);
// The Server Type changed (Settings > Quickplay, seek_prefs.h): Popular Now and the Live Streams overlay read the
// new type's lists / host at once (menu_sources.h) instead of at their next 60 s / 30 s interval.
void v10_menu_sources_changed(void);
int  v10_menu_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, int argc); // -1 = not ours
int  v10_menu_login_open(void);          // the login / register modal is up (Enter in its fields is its own)
int  v10_menu_login_escape(void);        // Escape: closes the modal; 1 = it was up
int  v10_login_guest_window(void);       // showWindow(5) for a guest: the modal instead (1 = taken over)
// The Account Manager's Use: the login card opens with a listed account's Username and its saved Password filled
// in, and signs in with them (the card's Login, for the player). The card signs in to the connected server's backend
// (account_split.h). 1 shown, 0 not here (the menu page is not up, or the card did not open).
int  v10_login_use(const char *name, const char *pass);
void v10_menu_tick(void);                // per frame: the menu's idle state (Popular Now, #activeBuffs, the Store item's badges)
// isKRSale (deob-10.0.3.js:28530): a Saturday / Sunday (UTC) of an odd ISO week, on the menu's clock (knV10MenuClock,
// a scenario's pinned Wednesday). The Store item's KR DISCOUNT badge and the Purchase KR tab's 10% Off read it.
int  v10_menu_kr_sale(void);
// v10's INIT (g:219154-219156): the Invite button greyed (buttonD) in a ranked match; the Host card greyed for an
// official game only (v10's second toggle of menuBtnHost overrides the ranked one, so ranked does not grey it).
void v10_menu_match_gate(int ranked, int official);
// v10's CurrencyDisplay flying number (svelte-1gpc1nd): "+N" / "-N" rising 40 px and fading over
// 1.5 s over `anchor` (the display whose value changed by `value`), then removed; a new one
// replaces the previous. v10 shows it on the Ranked shop's RP balance (v10:113910).
void v10_flying_number(HtmlElem anchor, double value);
