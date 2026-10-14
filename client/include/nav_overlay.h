#pragma once
// Krunker v7.1.4 dev console nav mesh visualizer (mod/devConsole.ts "Nav mesh visualizer",
// the `show_navmeshes` command): the generated Source-style nav areas of the current map and
// their connections as 3D debug lines over the world (client/src/nav_overlay.c).
//
// The mesh is v7's buildGeneratedNavMesh (shared/src/navgen.c), built on the client from the
// current map's JSON like v7's mod does in the browser, so it works offline and online alike.
// Natively it is built on a worker thread (a few ms on most maps, ~0.1 s on the largest);
// the web build has no threads and builds it inside the command. The result is cached per map.
#include <client.h>
#include <stdint.h>
#include <stddef.h>

void nav_overlay_show(Client*);     // show_navmeshes: build (or reuse) the current map's overlay
void nav_overlay_clear(int log);     // show_navmeshes off: clearNavOverlay ("nav: overlay cleared")
int  nav_overlay_active(void);       // shown or being built
// devConsole's offline map switches (offlineInitAt, runOfflineSandbox: clearNavOverlayForMapSwitch
// + scheduleNavOverlayRebuild): an active overlay is rebuilt on the next map.
void nav_overlay_map_switch(void);
// Per frame before the world renders: a finished build goes on screen, a map change clears the
// overlay (ensureNavOverlayOwnerCurrent) or rebuilds it after a console map switch.
void nav_overlay_frame(Client*);
// After the world scene (and its effects), before the first-person pass: the overlay scenes.
void nav_overlay_render(Client*);
void nav_overlay_fini(void);         // renderer shutdown: waits for a running build

// KrunkNative spectate overlay (shared/include/net_ext.h NET_EXT_KN_BOT_PATH): while the overlay is
// up and we follow a bot, its route polyline, goal marker and target / last-known markers are drawn
// with the nav layers (client/src/nav_overlay_geom.c builds the lines) from the server's packets.
void nav_overlay_bot_path(Client*, const uint8_t *body, uint16_t length); // a NET_EXT_KN_BOT_PATH body
// The spectator HUD's state line ("BOT Hunt · goal 37u · 12 waypoints · target Name (visible)"):
// 1 and the text while the overlay is active and a fresh packet is in for the followed bot.
int  nav_overlay_bot_status(Client*, char *out, size_t size);
