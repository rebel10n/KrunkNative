#pragma once
// Krunker v10.0.2 main menu shell on the v10 page (ui/; client/src/ui/v10_menu.c, doc/v10/ui.md 2):
// the action cards (Quick Match / Host Game / Find Game / Custom Games), the side nav (order,
// visibility, actions), the header bar (guest / signed in, level, KR, Settings, More Krunker), the
// match-info bar under the class preview and the login / register modal (loginOrRegister).
#include <html_dom.h>

int  v10_menu_page(void);                // the loaded page is v10's (#menuBtnQuickMatch exists)
void v10_menu_dom_init(void);            // menu_dom_init: wire the Svelte markup (no-op on other pages)
void v10_menu_account_changed(void);     // account_ui: login / logout / funds / level -> header + nav
int  v10_menu_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, int argc); // -1 = not ours
int  v10_menu_login_open(void);          // the login / register modal is up (Enter in its fields is its own)
int  v10_menu_login_escape(void);        // Escape: closes the modal; 1 = it was up
int  v10_login_guest_window(void);       // showWindow(5) for a guest: the modal instead (1 = taken over)
