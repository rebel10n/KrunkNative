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
//
// The same restart serves the updates (Settings > Game > Gameplay > "Auto-restart for updates", default
// on; doc/backend.md 9.4 "rejoin", doc/console.md "F5: full reload"):
//  * a server stopping for an update (its ERROR "GAME UPDATING", code 004004 on a live session:
//    "restarting for an update, reconnect after") shows the Game Updating screen briefly, then
//    restarts the client; the new process rejoins the same lobby with the restart rejoin's backoff
//    (a fresh join ticket for the same game id once the matchmaker lists it again, every 3 s for
//    2 min, else the region's best lobby) or reconnects to the same direct server; on the web the
//    page reloads with its ?game= / ?ws= query. Natively a client the launcher started restarts
//    through it (update_ui.h update_ui_restart_via_launcher), so the launcher's update check runs
//    first and a newer build, installed and verified (launcher.c staged_client_runs), is what comes
//    back; otherwise the executable runs again (a replaced file is the new build).
//  * a newer client ready to run (update_ui.h update_ui_newer_ready: the manifest's build the
//    launcher can install, or on the web a newer deployed page): in the menu the client restarts
//    right away; in a match it waits for the match to end or the next menu visit (the Outdated box
//    says so). Never for installs the package manager owns (deb/rpm: the Download link).
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
// After game_reload_startup: 1 when the restart handed over a lobby, a direct server or an official
// game, so main.c skips what the (same) command line starts: --offline / Start Offline's local game,
// --krunker's any official game. A lobby is sought by its game id, never by the Quickplay launch
// seek or excludeGame (servers_ui_reload_resume).
int game_reload_resume_online(void);
// main.c: an official krunker.io game handed over is joined again now (krunker_net_reload_join), in
// place of the network start; 1 = it was one.
int game_reload_resume_official(Client *client);

// ---- auto-restart for updates
// The server of a live session stopped for an update (servers_ui.c: ERROR 004004). With the
// setting on, the client restarts after showing the screen briefly and the new process rejoins
// `lobby_id` (a matchmade lobby; NULL = the direct server client->net_address:net_port) with the
// restart rejoin's backoff: returns 1 and the caller skips its in-place rejoin. 0 = setting off.
int game_reload_server_updating(Client *client, const char *lobby_id);
// A newer client is ready to run (update_ui.c): with the setting on, the client restarts in the
// menu, else at the end of the match / the next menu visit. `what` names the build (logged).
void game_reload_update_available(const char *what);
// 0 none, 1 a newer client waits for the match to end, 2 the restart is due (seconds away).
int game_reload_update_pending(void);
// 1 while a restart is due (the error screen's "Restarting..." line).
int game_reload_restart_pending(void);
