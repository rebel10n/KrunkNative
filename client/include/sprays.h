#pragma once
// Player sprays (client/src/sprays.c): Krunker v3.1.1 RENDER.addSpray / clearSprays
// (src/libs/render.js) behind the "sp" handler addSpray (src/index.js). The authority
// (shared/src/interact.c game_player_spray) places them; this module only renders.
#include <client.h>

#define SPRAYS_BASE_COUNT 53 // Game.sprays (src/data/sprays.js) entries

// NET_PACKET_SPRAY: places (or moves) the sprayer's decal. The spray index resolves like
// addSpray: Game.sprays[i], else a type-4 store skin, else Game.sprays[0].
void sprays_add(Client*, const NetSprayPacket*);
void sprays_tick(Client*);  // per frame: frame / movT texture animation
void sprays_clear(Client*); // "init" (new map or round): RENDER.clearSprays()
