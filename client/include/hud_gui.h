#pragma once
// Krunker v7.1.4 custom map HUD ("gui" map key, game.js customGUIs: utils.js parseGUIs ->
// overlayHelpers.customGUI on the overlay canvas for Box / Text / Image, game-overlay.js
// generateUIS for "Div (HTML)") as children "cusDIV<i>" of #customGUIHolder
// (client/src/ui/hud_gui.c). Values of Text / Div elements: the constant "v", a player value
// or a global value (WP-C cv / gv: hud_gui_set_value).
#include <client.h>

void hud_gui_update(Client *client); // every frame after the HUD values: builds / refreshes the elements
void hud_gui_clear(void);            // map unload: removes this module's cusDIV<i> elements only

// trigger action "toggle GUI" / tgui (index.js io_toggleElement): mode 0 toggle, 1 show, 2 hide
// every element with that id (Div elements too)
void hud_gui_toggle(int id, int mode);
// The value a Text / Div element shows for vt 1 (player value, the local player) / vt 2 (global
// value) "key"; NULL forgets it (shown as 0)
void hud_gui_set_value(int global, const char *key, const char *value);
