#pragma once
// WP-G (doc/v7/map-logic.md §1.1, §1.8.3-1.8.5): the client side of the v7.1.4 map environment
// (client/src/map_environment.c): renderer output (toneMapping, toneMappingExposure,
// outputEncoding, correctLights -> RenderLighting), the ocean (ocean.c) and the ENVIRONMENT_ZONE
// swaps of lights / fog / sky colour / sky dome / shadows by camera position (render.js
// updateEnvironment). main.c applies the map's own lighting first (client_apply_map_lighting),
// then calls map_environment_map_loaded.
#include <shared.h>
#include <modes.h>

// After the map's lighting is in g_render_lighting: adds the v7 output state, snapshots it as
// the environment of zone -1 and builds the ocean.
void map_environment_map_loaded(const Map *map, const GameMode *mode);
void map_environment_unload(void);

// Every frame with the camera position: switches RenderLighting / sky / fog / shadows to the
// environment zone the camera is in (-1 = the map's own). Returns the active zone index.
// The zone's ambient sound (ambInd / ambIndC) is not switched here (sound_events.c).
int map_environment_update(const Map *map, vec3 camera_position);
