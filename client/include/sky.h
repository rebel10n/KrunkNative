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

// The authored gradient for the physical sky: 1 = an untextured dome (its three stops: zenith, horizon, nadir),
// 0 = no dome (the clear colour only), -1 = a textured dome (an authored picture: the physical sky leaves it).
int sky_dome_gradient(vec3 stops[3]);

// ---- Box Office physical sky (rtSky; client/src/sky_physical.c, assets/shaders/rt_sky.glsl)
// Per-frame constants (sky_tick calls it): active with rtMode > 0, rtSky on, the tracer supported and no
// textured dome. Everything below is a no-op / 0 while inactive.
void sky_physical_frame(float delta_ms);
// The loaded map (client_apply_map_lighting, after the lighting): finds the sun the map draws itself (LIGHT_CONE
// prefabs parked in its sky) so the physical sky's disk sits on it instead of next to it; NULL clears it.
void sky_physical_set_map(const Map *map);
int sky_physical_active(void);
// The env table's cache key for the current physical sky ("" while inactive; env_map.c).
const char *sky_physical_key(void);
// rt_sky.glsl's psky_base towards `d` (no clouds, stars or disk), display values: the PMREM table rows.
void sky_physical_base(const float d[3], float out[3]);
// rt_sky.glsl's psky_authored towards `d`: the map's own gradient (dome stops, or the clear colour).
void sky_physical_authored(const float d[3], float out[3]);
// Uploads rt_sky.glsl's psky_* uniforms to `program` (psky_on = on && active).
// The Cinematic grade's mood (rt_grade.c): the physical sky's day factor (0 night .. 1 day) when it runs, else the
// same formula over the map's clear colour and sun (so a textured-dome map still gets a mood).
float sky_physical_day(void);
float sky_physical_warmth(void); // 0..1, the authored horizon's warmth (Cinematic grade)
// The sun's colour after the atmosphere, luminance 1 (rt_shafts under Cinematic); 0 when the physical sky is off.
int sky_physical_sun_color(float out[3]);
void sky_physical_uniforms(unsigned int program, int on);
// Draws the physical sky (sky_render calls it first); 1 when it drew.
int sky_physical_render(Camera *camera);
void sky_physical_fini(void);
