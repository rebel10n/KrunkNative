#pragma once
// Krunker v3.1.1 sky dome (src/libs/SkyDome.js): a radius-50, 32x32 SphereGeometry
// with a three-stop vertical vertex-colour gradient (skyDomeCol0 at the zenith,
// skyDomeCol1 at the horizon, skyDomeCol2 at the nadir), optionally multiplied by
// the "sky_6" texture (skyDomeTex), drawn back-side, unlit and unfogged, centred
// on the camera before the world (the renderer's backgroundScene pass).
// Maps without `skyDome` only show the clear colour (`sky`).
#include <client.h>

// Rebuilds the dome from the map's JSON fields; disables it when the map has none.
void sky_load(const cJSON *map_raw);
// Advances the skyDomeMovT texture scroll (`delta` in milliseconds).
void sky_tick(float delta);
// Draws the dome around `camera` and leaves the depth buffer untouched.
// Matrices are derived from the current viewport like scene_render().
void sky_render(Camera *camera);
// Frees the GL objects (program, VAO, buffers).
void sky_fini(void);
// The dome's texture: the missing-texture checker when its file does not load, 0 without one (tests).
unsigned int sky_texture_id(void);
