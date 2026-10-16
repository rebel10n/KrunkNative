#pragma once
// The Host Game window (client/src/ui/host_flow_ui.c): v10's host_games window (MENU_WINDOW_HOST, the
// captured ui/windows/8.html markup) with its Select, Game Settings, Advanced and Server tabs, then the
// last step, Start Game: the choice of where the game runs. The choice is krunker.io's official hosting
// while the official krunker.io connection is active, else Box Office servers, and always the dedicated
// server (the server_bin command and the Dedicated Server window: dedicated_server_ui.h).
#include <cJSON.h>
#include <html_dom.h>
#include <html_ui.h>

// The Host Game chooser's server types (window 42's cards): config.hostTypes index. Opens the Select tab.
void host_flow_open(int host_type);
// The Games window's Host button on a catalogue map (selectHostMap): an official map is checked, a
// community map is the banner's choice. The window's other values are kept.
void host_flow_open_map(int catalog_index);
// The Games window's Host button on a published map (selectHostMap): the banner's choice
// (the command's gameserver.maps and the request's customMap are published:<creator>/<name>).
void host_flow_open_published(const char *name, const char *creator);
// showGames(true): the Games window, to browse and pick a community map (its Host button comes back here)
void host_flow_browse_community(void);
// The window's markup for the state (windows[8] / MENU_WINDOW_HOST, servers_ui.c's window table).
char *host_flow_window_gen(void);
// windows[7].switchTab(n) on the page: the tab and its body (the Start Game step refreshes the command)
void host_flow_switch_tab(int tab);
// Start Game (the choice's first option; createPrivateRoom, ui_handlers.c): hosts on the active backend
void host_flow_start(void);
// knHostFlow(action, ...) (ui_handlers.c's module chain). -1 when the name is not the flow's.
int host_flow_ui_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, int argc);
