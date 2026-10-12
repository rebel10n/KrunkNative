#pragma once
// Client updates in the menu (client/src/ui/update_ui.c, doc/backend.md "Client updates"): the
// native client reads the updates manifest (launcher/include/kn_update.h) at startup and every
// hour, compares it with its own build (kn_build_info) by the servers' rule and, when a newer
// build exists, shows a box in the menu's left column: "Outdated" (older than the latest by more
// than the grace period, or an older protocol) / "Update available", with
//   Update now  restart through the launcher (KRUNKNATIVE_LAUNCHER, set by the launcher), which
//               installs the update and starts the new client; offered only for self-updating
//               installs (not deb/rpm, kn-install.json) and never forced: the player clicks it.
//   Download    the platform's package (deb/rpm installs: their package), else the release page.
// Console: `update` (status), `update check`, `update now`, `update download`.
// The web build is always the latest: everything here is a no-op there.
#include <client.h>
#include <html_dom.h>

// Per frame from client_menu_tick(): starts the checks, applies their results to the page.
void update_ui_tick(Client *client);
// knUpdateNow / knUpdateDownload; -1 when the name is not ours.
int update_ui_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, int argc);
// The console's `update [check|now|download]`.
void update_ui_console(int argc, char **argv);
// --render-scenario: state.update {"state":"outdated"|"available","latest":"…","launcher":bool}
// shows the box without a network check (tools/uidiff/scenarios/menu-outdated.json).
void update_ui_scenario(const cJSON *update);
