#pragma once
// Krunker v7.1.4 custom map HUD ("gui" map key, game.js customGUIs: utils.js parseGUIs ->
// overlayHelpers.customGUI on the overlay canvas for Box / Text / Image, game-overlay.js
// generateUIS for "Div (HTML)") as children "cusDIV<i>" of #customGUIHolder
// (client/src/ui/hud_gui.c). Values of Text / Div elements: the constant "v", a player value
// or a global value; visibility after "tgui" (map-C map_triggers.h / triggers_v7.h).
#include <client.h>

void hud_gui_update(Client *client); // every frame after the HUD values: builds / refreshes the elements
void hud_gui_clear(void);            // map unload: removes this module's cusDIV<i> elements only
