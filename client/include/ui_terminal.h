#pragma once
// Krunker v3.1.1 map terminals on the client (client/src/ui/ui_terminal.c): the
// "Press [G] to Open Terminal" #interactMsg prompt (Player interact checks, updateInteract
// type 2), "int" on the interact keys, openTerminal on "ot" (#terminalHolder, the
// #terminalText typewriter, #terminalInput focus) and "tmc" when the chat key submits it.
#include <client.h>
#include <html_dom.h>

void ui_terminal_tick(Client*);              // per frame: the interact prompt, the restarted typewriter
void ui_terminal_interact(Client*, int secondary); // interactKey / interactSecKey: Game.players.interact + io.send("int", 1[, true])
void ui_terminal_open(Client*, int uid);     // "ot": openTerminal(uid)
int  ui_terminal_focused(void);              // document.activeElement == terminalInput
// GLFW keys while #terminalInput has focus (ui_controls_key_event): the chat key sends,
// Escape leaves the field, the rest types. 1 = consumed.
int  ui_terminal_key_event(Client*, int key, int scancode, int action, int mods, int chat_key);
// Page keydown in #terminalInput (web: kn-shim forwards Enter/Escape of fields). 1 = consumed.
int  ui_terminal_page_key(const HtmlEvent*);
