#pragma once
// The soundscape loop (client/src/sound_ambient.c): v10's window.changeAmbient over audio.h, apart from
// sound_events.c so it builds and tests without the client.
#include <stdint.h>

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
