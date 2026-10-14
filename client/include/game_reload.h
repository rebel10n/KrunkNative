#pragma once
// F5 / the console's `reload`: a full reload of the running client (client/src/game_reload.c;
// what it covers, step by step: doc/console.md "F5: full reload").
//
// Natively it reloads in place: the stored settings (game, Client tab, binds, console mods) are
// read again and applied, every cached texture and OBJ model is read again into the same GL
// objects, GLTF models and sounds are read again at their next use, the menu's window templates
// are read again, the HUD is rewritten, and the session is re-entered (the offline game restarts
// on its map and mode, an online one rejoins the same server, else the menu stays with a notice).
// What only a new process loads (the menu page itself, shaders, map files, startup-only "*"
// settings, another Game version's page) makes it re-execute itself like F4, then come back to
// the same game. On the web F5 is the browser's reload (the address bar keeps the game).
#include <client.h>

// Asks for a reload at the start of the next frame (safe from key callbacks and handlers);
// `why` is logged ("F5", "console").
void game_reload_request(const char *why);
// F5 (mm-token): relaunch the client process (app_reload: the same command line, the window as it
// is) and re-enter this session in the new one (KRUNKNATIVE_RELOAD_RESUME); the in-place reload
// above only if the exec fails. The web reloads the page, as for game_reload_request.
void game_reload_relaunch_request(const char *why);
// Per frame (client_menu_tick): runs a requested reload; after a restart, re-enters the game the
// previous process was in.
void game_reload_tick(Client *client);
// main.c at startup, before anything is loaded: files changed after this moment are new to this
// process; also picks up the session a reload restart hands over (KRUNKNATIVE_RELOAD_RESUME).
void game_reload_startup(void);
