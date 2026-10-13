#pragma once
// The Krunker v3.1.1 offline build's developer console (jail tree mod/devConsole.ts),
// ported 1:1 in client/src/ui/ui_console.c: the draggable/resizable "Console" modal
// (ui/css/devconsole.css), its log colours, hints dropdown, history, Tab completion, the
// local commands, the server commands sent as "/cmd" chat (shared/src/chat_commands.c on
// server_bin and in the offline game) and devConsole's singleplayer versions of them.
// The toggle key, enable switch and settings rows are mods.ts (ui_mods.c).
#include <client.h>
#include <html_dom.h>

// Per frame from client_menu_tick(): builds the overlay once the game page is up and
// polls the input line (hints, the toggle character).
void ui_console_tick(Client*);
int  ui_console_is_open(void);              // devConsole.visible
void ui_console_toggle(int go_to_game);     // toggleConsole(goToGame): update() frees / relocks the pointer
// GLFW keys, first thing in ui_controls_key_event() after the controls capture: mods.ts'
// keydown (toggle key, F8 cycle, bhop key), handleConsoleKeydown, typing into the input
// while visible, and the session key binds while hidden. 1 = consumed.
int  ui_console_key_event(Client*, int key, int scancode, int action, int mods);
// Page keydown in the console input (web: kn-shim forwards Escape/Enter plus Tab/arrows/`
// for data-kn-keys fields). 1 = consumed (preventDefault).
int  ui_console_page_key(const HtmlEvent*);
// Left button over the modal / the mods.ts bind rows: titlebar drag, grip resize, the
// close button, Submit and the hint items. action GLFW_PRESS/GLFW_RELEASE, -1 = move;
// native framebuffer px (main.c) or CSS px (css = 1, the web glue). 1 = consumed.
int  ui_console_mouse(int action, double x, double y, int css);
// io.send wrapper: a chat line ("/bhop ...") the mod handles on the client. 1 = not sent.
int  ui_console_chat_command(Client*, const char *text);
// hookChat: every "ch" line, and the "noc" / "unl" / "fh" packets (NetCheatFlag).
void ui_console_on_chat(const char *name, const char *text, int kind);
void ui_console_on_cheat(int flag, int on);
void ui_console_exec(const char *line);     // devConsole.exec(line)
// `reconnect` without its echo (F5, game_reload.c): 1 = the offline game restarted or a server is
// being joined again, 0 = nothing to reconnect to (an error line in the console).
int  ui_console_reconnect(void);
void ui_console_printf(const char *color, const char *fmt, ...); // a log line (NULL = log colour)
