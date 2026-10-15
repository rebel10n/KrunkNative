#pragma once
// The Host Game flow (client/src/ui/host_flow_ui.c): one window (HOST_FLOW_WINDOW) behind every Host
// Game entry point, with four steps: 1 Map (the catalogue and the community maps), 2 Mode & classes,
// 3 Rules (weapons, classes, effects, settings), 4 Server & command: the server_bin line to copy
// (shared/include/host_command.h), practice on the embedded offline game, and the link to the
// dedicated-server download page.
#include <cJSON.h>
#include <html_dom.h>
#include <html_ui.h>

// The dedicated-server download page. PLACEHOLDER: another agent is writing the page; replace the
// URL when it is published.
#define HOST_FLOW_SERVER_DOWNLOAD_URL "https://krunk.io/host/dedicated-server"

// The Host Game chooser's server types (window 42's cards): config.hostTypes index. Opens step 1.
void host_flow_open(int host_type);
// The Games window's Host button on a catalogue map (selectHostMap): the Mode step with that map.
void host_flow_open_map(int catalog_index);
// The chooser's Community Games card (showGames(true)): the Map step, community maps only.
void host_flow_open_community(void);
// windows[HOST_FLOW_WINDOW] (ui_windows.c): the window's markup for the current state.
char *host_flow_window_gen(void);
// knHostFlow(action, ...) (ui_handlers.c's module chain). -1 when the name is not the flow's.
int host_flow_ui_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, int argc);
