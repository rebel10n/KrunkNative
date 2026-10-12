#pragma once
// Krunker v7.1.4 ocean (libs/Ocean.js + three.js examples Water.js): a reflective water plane
// ocW x ocL at y = ocYOff with a mirror render target (client/src/ocean.c). Client only.
#include <client.h>

void ocean_load(const Map *map);  // map "ocean": builds the plane, its material and target
void ocean_unload(void);          // removes the plane from its scene, frees GL objects
// Every frame before the world render: advances the waves (time += dt / 1000 * ocSpd), puts the
// plane in `scene` (once) and renders the mirror reflection of `scene` from `camera`
// (Water.onBeforeRender). No-op without an ocean.
void ocean_update(Scene *scene, Camera *camera, float delta_ms);
