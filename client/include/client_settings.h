#pragma once
// KrunkNative client settings: the Settings window's "Client" tab and the menu's Exit
// button, native only (client/src/ui/ui_client_settings.c).
//
// References: the Krunker v3.1.1 desktop client (jail Krunker_v3.1.1/main/preload.js +
// utilities.js: a "Client" tab pushed onto windows[0].tabs whose rows come from
// getCSettings(), the #settingsTabLayout flex override, the #clientExit menu item),
// Krunker v7.1.4 (public/index.html #clientExit, clientExitPopup() "Are you sure you
// want to close krunker?" YES/NO, closeClient()) and glorp (src/cSettings.json +
// frontend/settings.js: categories of switches/sliders/selects, "*" = needs a restart,
// buttons that open the client's folders, Clear Cache & Storage).
//
// The values live in their own storage entry, `knClient` (html_storage_*: native
// $XDG_CONFIG_HOME/krunknative/knClient.json, web localStorage), apart from the game's
// `knSettings`, like glorp's settings.json / the v3.1.1 client's `utilities_*` keys: the
// game's Reset / presets / import / export never touch them. Rows that surface a game
// setting (window mode, V-Sync, frame cap, UI scale) keep using setSetting() and the
// game store; the matchmaker URL is the `knBrainUrl` entry backend_native.c reads.
#include <client.h>
#include <html_dom.h>

// ---- main.c
// Before audio_init(): loads the store and applies the startup options (audio backend,
// process priority, raw mouse input, window resolution, start offline, resource
// swapper, Discord RPC). `offline` may be set to 1 ("Start Offline").
void client_settings_startup(Client *client, int *offline);
// After the native shutdown: closes Discord RPC and, when a restart was requested
// (settings marked "*", Clear Cache & Storage), starts the client again.
void client_settings_exit(void);
// Before this process re-executes itself (a relaunch, F5's restart fallback): the environment it
// set from the settings (brain URL, audio backend) is dropped, so
// the next process reads the saved settings again.
void client_settings_before_exec(void);
// F5 (game_reload.c): the Client tab's store read again and its live settings applied. Returns
// the name of a changed setting that only applies at startup (the "*" rows), else NULL.
const char *ui_client_settings_reload(void);

// ---- game side
int  client_settings_hs_sound_always(void);              // "Always Play HS Sound on Kill"
// "Show Game Time" (Client > Game, default off): the top-right tile of the time played on this device (hud_ui.c).
// KrunkNative only: v10 has no such setting (doc/v10/ notes the difference).
int  client_settings_game_time(void);
// "Hide player names" (Client tab, knClient hidePlayerNames, default off): every other player shows as
// Player_<n>, the local one as Player, and no clan tag or profile picture (client/include/player_names.h).
int  client_settings_hide_player_names(void);
// The Notifications switch (v10_menu.c): desktop notifications for the notification centre's items
// (desktop_notify.c). knClient "desktopNotify", default on; the setter also updates the notifier.
int  client_settings_desktop_notify(void);
void client_settings_set_desktop_notify(int on);
// "Auto-restart for updates" (default on, game_reload.h): the client restarts itself (the web
// page reloads) when the server stops for an update or a newer client is ready to run.
int  client_settings_auto_restart(void);
void client_settings_fullscreen_size(int *w, int *h);    // "Resolution" for exclusive fullscreen (unchanged when unset)
// GLFW raw mouse motion again: Controls > Raw Mouse Input (rawMouse)
void client_settings_apply_raw_mouse(void);

// ---- UI (client/src/ui)
void  ui_client_settings_dom_init(void);                  // menu_dom_init: Exit button, custom CSS, auto spectate, timers
int   ui_client_settings_tab(void);                       // settings tab index of "Client"; -1 when not shown
char *ui_client_settings_head(const char *head, int active_tab); // the settings head with the Client tab added (malloc'd)
char *ui_client_settings_tab_html(int searching);
// A game settings tab with the client's rows added (the Sound tab's Audio Output); NULL =
// unchanged (malloc'd otherwise).
char *ui_client_settings_extend_tab(const char *tab);         // the tab body (malloc'd); searching: rows for windows[0].searchList
// Inline handlers: clientExitPopup(), closeClient(), knClientSet(key, value), knClientAction(name).
int   ui_client_settings_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, int argc);
