#pragma once
// Client updates in the menu (client/src/ui/update_ui.c, doc/backend.md "Client updates"): the
// native client reads the updates manifest (launcher/include/kn_update.h) at startup and every
// hour, compares it with its own build (kn_build_info) by the servers' rule and, when a newer
// build exists, shows a box in the menu's left column: "Outdated" (older than the latest by more
// than the grace period, or an older protocol) / "Update available", with
//   Update now  restart through the launcher (KRUNKNATIVE_LAUNCHER, set by the launcher), which
//               installs the update and starts the new client; offered only for self-updating
//               installs (not deb/rpm, kn-install.json).
//   Download    the platform's package (deb/rpm installs: their package), else the release page.
// With Settings > Client > "Auto-restart for updates" (default on) a newer build that can be
// installed restarts the client by itself: in the menu right away, in a match at its end or the
// next menu visit (game_reload.h, game_reload_update_available); the box says so meanwhile.
// Console: `update` (status), `update check`, `update now`, `update download`.
// The web build is served from the site itself, so a newer client is a newer page: a deployed
// page carries its build stamp (<meta name="kn-build">, deploy/pages/stage.py) and the client
// fetches its own page every hour to compare; a newer stamp is "Update available" (Update now =
// reload) and the same automatic restart, as the page reload. The plain build has no stamp:
// nothing to check.
#include <client.h>
#include <html_dom.h>

// Per frame from client_menu_tick(): starts the checks, applies their results to the page.
void update_ui_tick(Client *client);
// knUpdateNow / knUpdateDownload; -1 when the name is not ours.
int update_ui_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, int argc);
// The console's `update [check|now|download]`.
void update_ui_console(int argc, char **argv);
// --render-scenario: state.update {"state":"outdated"|"available","latest":"…","launcher":bool}
// shows the box without a network check (a scenario state, tools/uidiff/README.md).
void update_ui_scenario(const cJSON *update);
// Restart through the launcher (KRUNKNATIVE_LAUNCHER, a launcher-started client): it waits for
// this process (--wait-pid), runs its update check first (so a newer build is what comes back,
// after its staged-client check), then starts the client with `args` (app_exec_args: the same
// command line; NULL = none). The caller put what the new process needs into the environment
// (KRUNKNATIVE_RELOAD_RESUME, the window state). 1 = started and this window asked to close;
// 0 = no launcher (the caller restarts itself). Always 0 on the web.
int update_ui_restart_via_launcher(Client *client, char *const *args);
// 1 while a newer client is known that a restart would run (native: the manifest's build is newer
// and the launcher can install it; web: the site serves a newer page build).
int update_ui_newer_ready(void);
