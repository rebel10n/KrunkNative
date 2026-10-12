#pragma once
// Krunker v7.1.4 interaction on the client (client/src/ui/ui_interact.c; WP-C): the #interactMsg
// prompt of players.updateInteract (gates, deposit boxes, hold-mouse and interact-key triggers,
// weapon pickups: game_interact_scan), #interactProg, and the keys: interactKey / interactSecKey
// send "int" / "intSec" type, uid (lastInteractData), dropKey "drpw", confirmKey "cnfm" for a
// pending KR-cost trigger. v7 has no map terminals (the v3 terminal UI is retired).
#include <client.h>

void ui_interact_tick(Client*);                  // per frame: the prompt and the hold progress bar
void ui_interact_key(Client*, int secondary);    // interactKey (0) / interactSecKey (1)
void ui_interact_drop(Client*);                  // dropKey: "drpw"
// confirmKey (v7 controls.js binds.confirm, keyCode 75 "K" until the controls window has the
// bind): "cnfm" when a confirmation is pending. 1 = consumed.
int  ui_interact_confirm(Client*);
#define UI_INTERACT_CONFIRM_KEYCODE 75
