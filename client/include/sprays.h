#pragma once
// Player sprays (client/src/sprays.c): Krunker RENDER.addSpray / clearSprays (src/libs/render.js)
// behind the "sp" handler addSpray, and v7.1.4's spray wheel (game-overlay.js). The authority
// (shared/src/interact.c players.spray) places them; this module only renders.
#include <client.h>
#include <html_ui.h>
#include <social_v7.h>

#define SPRAYS_BASE_COUNT 53 // v3.1.1 Game.sprays (src/data/sprays.js) entries: older servers' indices

// NET_PACKET_SPRAY (older servers): places (or moves) the sprayer's one decal. The spray index
// resolves like v3.1.1's addSpray: Game.sprays[i], else a type-4 store skin, else Game.sprays[0].
void sprays_add(Client*, const NetSprayPacket*);
// NET_PACKET_SPRAY_V7: places (or moves) decal "<uid>_<slot>"; the skin is a SPRAY store item,
// else 2482 (v7 game.addSpray).
void sprays_add_v7(Client*, const NetSprayV7Packet*);
void sprays_tick(Client*);  // per frame: frame / movT texture animation
void sprays_clear(Client*); // "init" (new map or round): RENDER.clearSprays()
int sprays_count(void);     // live decals (tests)

// The spray wheel (overlay.sprayWheel*, premium accounts and the sandbox): open while the
// sprayWheel key is held, shown after SPRAY_WHEEL_DELAY; the mouse wheel cycles it and the key's
// release sprays the chosen item (client/src/ui/hud_social.c).
void sprays_wheel_open(void);      // sprayWheelOpen (+ updateSprayWheel: owned sprays + the equipped one)
void sprays_wheel_close(void);     // sprayWheelClose
int sprays_wheel_is_open(void);    // the key is held
int sprays_wheel_opened(void);     // sprayWheelOpened: held long enough to show (takes the scroll)
void sprays_wheel_change(int step); // sprayWheelChange
int sprays_wheel_selected(void);   // getSprayIndex().index
void sprays_wheel_render(Client*, float delta_seconds); // per frame, after the HUD canvases
void sprays_wheel_fini(void);
