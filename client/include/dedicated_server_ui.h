#pragma once
// The Dedicated Server window (client/src/ui/dedicated_server_ui.c): everything a player needs to
// run server_bin, inside the client (native and web) instead of a web page. Opened by the Host Game
// flow's "Get the dedicated server" button (host_flow_ui.c, knHostFlow('download')). Built like the
// Download window (download_ui.c, showWindow(25)) with its .knDl* look (ui/css/krunknative.css):
//   - the server_bin packages of the rolling GitHub release `server-latest` (.github/workflows/
//     release.yml, tools/release/package-server.sh): this computer's first, every other platform
//     below, with the release's SHA256SUMS; Linux on ARM builds from source;
//   - quick start, the main server.cfg keys (config/server.cfg.example), port forwarding (TCP and
//     UDP 21015: net_tcp.c and net_udp.c / info_query.c bind the same number), getting listed by the
//     coordinator's finder (finder_url, doc/backend.md 8.8) and accounts (online_mode, 8.9);
//   - VPS suggestions with approximate, dated monthly prices.
#include <ui_logic.h> // KN_WINDOW_BASE

// A KrunkNative window: its id is above every Krunker page's windows[] (ui_logic.h KN_WINDOW_BASE:
// +0 Object Manager, +2 Connect, +3 Host Game flow, +4 alt manager).
#define DEDICATED_SERVER_WINDOW (KN_WINDOW_BASE + 5)

// Shows the window. from_host_flow: it gets a "Back to Host Game" button (knHostFlow('reopen')).
void dedicated_server_ui_open(int from_host_flow);
// windows[DEDICATED_SERVER_WINDOW] (ui_windows.c): the markup, and the window header when `header`
// is non-NULL. NULL for any other id.
char *dedicated_server_window_gen(int id, const char **header);
