#pragma once
// The alt manager window (ALT_MANAGER_WINDOW = KN_WINDOW_BASE + 4, ui_logic.h): the player's own
// accounts, KrunkNative sign-ins and krunker.io sessions, listed and switched between. Opened from
// More Krunker (v10_menu.c). Its data is the alt vault (client/include/alt_store.h).
#include <html_dom.h>

// The window's markup (NULL for other ids). Opens the vault if the player has one.
char *alt_manager_window_gen(int id, const char **header);
// knAlt* handlers from the window (-1 = not ours).
int alt_manager_ui_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, int argc);
