#pragma once
// In-world game mode pieces (client/src/mode_world.c): the active Hardpoint / KOTH zone,
// CTF flag crystals + zones and Kill Confirmed tags, from the mirrored mode state.
#include <client.h>

void mode_world_tick(Client*);   // per frame: (re)build for the current map + mode, animate
void mode_world_unload(Client*); // INIT / map change: remove everything from the scene
// Scripted checks (input_script `flags`): flag f's crystal and stand meshes as drawn this frame,
// 0 when the mode has no such flag mesh.
int mode_world_flag_meshes(int f, vec3 *crystal, int *crystal_visible, vec3 *stand, int *stand_visible);
