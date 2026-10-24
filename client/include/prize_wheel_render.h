#pragma once
// v10.0.3 map prize wheels drawn (client/src/prize_wheel_render.c): render.addSpinner's canvas plane per spinner
// (deob-10.0.3.js:232681-232707), painted from client/src/prize_wheels.c's spinInfo as game.updateSpinners paints it
// (:219576-219675; client/include/prize_wheel_geom.h).
#include <client.h>

void prize_wheel_render_load(Scene*, const Map*); // client_load_map: one plane per spinner
void prize_wheel_render_unload(void);             // client_unload_map: the planes, canvases and textures go
void prize_wheel_render_frame(Client*);           // per frame before the world pass: a spinner's new paint
// Tests: the planes built (*count) and spinner uid's texture (0 none).
int prize_wheel_render_count(void);
unsigned int prize_wheel_render_texture(int uid);
