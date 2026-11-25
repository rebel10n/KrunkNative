#pragma once
// The soundscape loop (client/src/sound_ambient.c): v10's window.changeAmbient over audio.h, apart from
// sound_events.c so it builds and tests without the client.
#include <stdint.h>
#include <cJSON.h>

// parseInt's NaN for a soundscape id: never the current one
#define SOUND_AMBIENT_NAN INT32_MIN

// window.changeAmbient(parseInt(id), custom, 1000 * (fade || 0)): custom = the id is a user asset sound, fade_ms != 0
// fades the old loop out and the new one in. The "camb" handler (map_triggers.c) and the game start
// (sound_events.c) call it.
void sound_events_change_ambient(int id, int custom, float fade_ms);
// SOUND.play of soundscape `id` as changeAmbient plays it (amvVols[id] || 0.12, looping)
void sound_ambient_play(int id, int custom, float fade_ms);
// amvVols[id] || 0.12
float sound_ambient_volume(int id);

// parseInt of a map value (a number or a string), SOUND_AMBIENT_NAN for anything else
int sound_ambient_parse_int(const cJSON *v);
// The soundscape of a map's data (the game start, deob-10.0.7.js:193909-193915, and updateEnvironment(-1),
// :208559-208576): `mode.ambInd || map.ambInd || 1`, a truthy ambIndC replacing it as a custom one; fade_ms (may be
// NULL) = 1000 * (map.ambFadeTime || 0). mode_amb_ind: the mode's ambInd, 0 none.
void sound_ambient_map(const cJSON *map, int mode_amb_ind, int *id, int *custom, float *fade_ms);
// updateEnvironment(id)'s changeAmbient (deob-10.0.7.js:208571-208575) when the camera's environment zone changes:
// `zone` is the ENVIRONMENT_ZONE record the camera entered, NULL when it left every zone (the map's data then).
void sound_ambient_environment(const cJSON *zone, const cJSON *map, int mode_amb_ind);
