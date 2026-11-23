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
// The Host Presets' commMap key (ui_host_config.c, deob-10.0.3.js:249190-249209): the chosen community map's name,
// "" when none is chosen (Save stores it only then).
const char *host_flow_community_name(void);
// Load of a preset with commMap: the chosen community map becomes the one of that name (the catalogue's community map,
// else the published map). 0 = not applied: no community map is chosen (v10 throws there), or no such name.
int host_flow_set_community_name(const char *name);
// showGames(true): the Games window, to browse and pick a community map (its Host button comes back here)
void host_flow_browse_community(void);
// The window's markup for the state (windows[8] / MENU_WINDOW_HOST, servers_ui.c's window table).
char *host_flow_window_gen(void);
// windows[7].switchTab(n) on the page: the tab and its body (the Start Game step refreshes the command)
void host_flow_switch_tab(int tab);
// createPrivateRoom(rehost) (ui_handlers.c, deob-10.0.7.js:204669-204811): v10's checks (a map, then the rows), then
// "Please wait..." with Start Game / Rehost greyed, and the request on the window's backend (its tab's; the active one
// when it was opened from elsewhere). rehost: a krunker.io dev / admin's Rehost on the game it hosts.
void host_flow_create(int rehost);
void host_flow_start(void); // host_flow_create(0)
// customRoomResponse (deob-10.0.7.js:204617-204631): the hosting's answer gives Start Game / Rehost back; created (a
// game was made) also empties the Raw Game Data field. servers_ui.c calls it for Box Office's answers.
void host_flow_room_response(int created);
// "+ Host Game" on a Servers tab: the chooser (window 42), whose server type then opens the Host Game window with
// that tab's backend (krunker 1 = krunker.io, 0 = Box Office)
void host_flow_set_chooser_backend(int krunker);
// menu_show_window's hook: leaving the chooser for another window forgets the tab's backend
void host_flow_on_show_window(int id);
// openHostWindow(true): the chosen game's trash: no game, host type Custom, the window again
void host_flow_clear_map(void);
// knHostFlow(action, ...) (ui_handlers.c's module chain). -1 when the name is not the flow's.
int host_flow_ui_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, int argc);
