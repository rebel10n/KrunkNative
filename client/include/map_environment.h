#pragma once
// WP-G (doc/v7/map-logic.md §1.1, §1.8.3-1.8.5): the client side of the v7.1.4 map environment
// (client/src/map_environment.c): renderer output (toneMapping, toneMappingExposure,
// outputEncoding, correctLights -> RenderLighting), the ocean (ocean.c) and the ENVIRONMENT_ZONE
// swaps of lights / fog / sky colour / sky dome / shadows by camera position (render.js
// updateEnvironment; v10.0.3's return to the map keeps a zone's lights where the map has none),
// and getEnvMap for the GLB reflections (env_map.h). main.c applies the map's own lighting first
// (client_apply_map_lighting), then calls map_environment_map_loaded.
#include <shared.h>
#include <modes.h>

// After the map's lighting is in g_render_lighting: adds the v7 output state, snapshots it as
// the environment of zone -1 and builds the ocean.
void map_environment_map_loaded(const Map *map, const GameMode *mode);
void map_environment_unload(void);
// The loaded map's current environment (its zone, else its own) applied again: app_mode.c calls it
// when a page is left, so the game behind a page that drew with its own lighting (the editor) gets
// its lighting back. No map loaded: the v3.1.1 output, as map_environment_unload.
void map_environment_restore(void);

// Every frame with the camera position: switches RenderLighting / sky / fog / shadows to the
// environment zone the camera is in (-1 = the map's own). Returns the active zone index.
// The zone's ambient sound (ambInd / ambIndC) is not switched here (sound_events.c).
int map_environment_update(const Map *map, vec3 camera_position);
