#pragma once
// The in-world side of map-logic WP-H (client/src/zombies_render.c): the mystery box / krum
// machine weapon meshes, the mystery box beams and outline, and the airdrop crates, all driven
// by the Game.zombies mirror (shared/include/zombies.h).
#include <client.h>

void zombies_render_load(Scene *scene, const Map *map); // client_load_map: the machines of the map
void zombies_render_unload(void);                       // client_unload_map: every mesh removed and freed
void zombies_render_frame(Client *client, float delta_ms); // every frame before the world render
