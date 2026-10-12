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
