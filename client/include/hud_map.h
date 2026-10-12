#pragma once
// player-viewer WP-E part 2a: the v7.1.4 minimap (src/libs/Minimap.js on #miniMapCanvas) and
// the death markers (game-overlay.js addDeathMarker / f1566). client/src/ui/hud_map.c.
#include <client.h>

// io_initGame: overlay.miniMap.reset(Game, showMinimap) and overlay.resetAll() (no markers).
void hud_map_on_init(Client*);
// Per frame after the HUD pass: Minimap.toggle / update and the death markers.
void hud_map_tick(Client*);
// The top-left layout of the current toggle state (#miniMapOverlay, .topLeftOld): hud_ui.c's
// visibility passes re-apply it after rewriting the HUD defaults.
void hud_minimap_apply_layout(void);
// f1988: a player fired (the "9" tracer / a projectile): enemies blip for config.playerBlipTimer.
void hud_minimap_on_shot(Client*, int uid);
// The specMiniMap bind while spectating (see hud_map.c for the v7 bug this fixes).
void hud_minimap_spectator_toggle(void);
int  hud_minimap_spectator_hidden(void);
// v7 production hides the minimap of custom-map games (Minimap.toggle: customsData &&
// config.isProd). -1 = decide from the map (not an official catalogue map = custom), 0 / 1 =
// forced (ui_scenario: v7's reference captures load Kanji as a custom map).
void hud_minimap_set_custom_override(int custom);

// overlay.addDeathMarker(x, y, z, isYou): the deathMarkers setting filters it.
void hud_death_marker_add(vec3 position, int is_you);
