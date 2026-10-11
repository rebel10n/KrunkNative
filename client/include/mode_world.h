#pragma once
// In-world game mode pieces (client/src/mode_world.c): the active Hardpoint / KOTH zone,
// CTF flag crystals + zones and Kill Confirmed tags, from the mirrored mode state.
#include <client.h>

void mode_world_tick(Client*);   // per frame: (re)build for the current map + mode, animate
void mode_world_unload(Client*); // INIT / map change: remove everything from the scene
