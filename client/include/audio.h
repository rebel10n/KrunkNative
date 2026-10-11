#pragma once
// Client audio: a C port of Krunker v3.1.1's SOUND manager (Krunker.Unpacked-v3.1.1/64.js,
// Howler.js underneath) on top of miniaudio (client/lib/miniaudio, compiled in
// client/src/miniaudio.c). Sounds are assets/sound/<name>.mp3, decoded once and cached.
//
// Everything here is main-thread only (the mixer runs on miniaudio's device thread, or the
// Web Audio callback on the web). Every call is a no-op until audio_init() succeeded, so the
// game, the UI scenario renderer and builds without an audio device keep working silently.
//
// Debug environment (native):
//   KRUNKNATIVE_AUDIO=0                 no audio at all
//   KRUNKNATIVE_AUDIO_BACKEND=<list>    miniaudio backends to try, in order, comma separated:
//                                       pulseaudio (alias pulse, pipewire: PipeWire through its
//                                       PulseAudio server), alsa, jack, coreaudio, wasapi, dsound,
//                                       winmm, sndio, audio4, oss, aaudio, opensl, webaudio, null
//                                       (no sound card; real-time clock). Unset: every backend
//                                       built for the platform in miniaudio's order (Linux:
//                                       PulseAudio/PipeWire, ALSA, JACK; BSD: sndio, audio(4),
//                                       OSS; macOS: Core Audio; Windows: WASAPI, DirectSound,
//                                       WinMM; Android: AAudio, OpenSL ES; web: Web Audio).
//                                       Playback and voice capture use the same list.
//   KRUNKNATIVE_AUDIO_CAPTURE=out.wav   also write the final mix to a 16-bit stereo WAV
//   KRUNKNATIVE_AUDIO_LOG=1             print every play/stop with its effective volume
#include <common.h>

// Krunker's SOUND.volumes indices (settings: sound=0, voiceVolume=1, ambientVolume=2,
// gunsVolume=3, playerVolume=4, uiVolume=5, assetVolume=6).
typedef enum {
    AUDIO_CAT_MASTER = 0,
    AUDIO_CAT_VOICE = 1,
    AUDIO_CAT_AMBIENT = 2,
    AUDIO_CAT_WEAPON = 3,
    AUDIO_CAT_PLAYER = 4,
    AUDIO_CAT_UI = 5,
    AUDIO_CAT_ASSET = 6,
    AUDIO_CAT_COUNT
} AudioCategory;

// Krunker constants (config 1.js): otherSoundMlt scales every play3D; refDistance /
// rolloffFactor are play3D's pannerAttr fallbacks; maxDistance is Howler's PannerNode default.
#define AUDIO_OTHER_SOUND_MLT 0.55f
#define AUDIO_REF_DISTANCE 25.0f
#define AUDIO_ROLLOFF 1.0f
#define AUDIO_MAX_DISTANCE 10000.0f

int audio_init(void);
void audio_fini(void);
int audio_available(void);
// The ma_engine (void* keeps miniaudio out of this header) for voice chat playback
// (client/src/voice.c), NULL without audio.
void *audio_engine(void);
// KRUNKNATIVE_AUDIO_BACKEND as ma_backend values (into `out`, at most `max`); 0 = unset or
// nothing usable: miniaudio's default order.
int audio_backends(int *out, int max);

// Per frame: listener = camera (Howler.pos/orientation), finished voices are recycled and
// deferred loads/plays are serviced (decoding never runs inside UI event handlers, which on
// the web cannot block for an asset fetch).
void audio_update(vec3 listener_position, float yaw, float pitch);

// SOUND.setVolume(v, category). Defaults are Krunker's slider defaults
// (master 0.8, weapons 0.5, everything else 1).
void audio_set_volume(AudioCategory category, float volume);
float audio_get_volume(AudioCategory category);
// Krunker setting key -> audio_set_volume ("sound", "ambientVolume", "voiceVolume",
// "gunsVolume", "playerVolume", "uiVolume", "assetVolume"). Returns 1 when the key was an
// audio setting.
int audio_apply_setting(const char *key, const char *value);

// Queue a sound for decoding ahead of its first play.
void audio_preload(const char *name);
// SOUND.rate = Game.config.deltaMlt (times endAnimationTimeLeft / endAnim during the round-end
// slow motion): scales audio_play's explicit rate; 0 mutes new audio_play calls. Default 1.
void audio_set_rate(float rate);

// SOUND.play(name, volume, loop, rate): 2D. A looping sound that is already playing is not
// restarted (Krunker's `!j || !f.isPlaying`). volume <= 0 means 1, rate <= 0 means 1.
void audio_play(const char *name, float volume, int loop, float rate);
// SOUND.play3D(name, x, y, z, volume, rate, refDistance, rolloff): positional one-shot;
// volume is multiplied by AUDIO_OTHER_SOUND_MLT like Krunker. ref_distance/rolloff <= 0 use
// the defaults above.
void audio_play_3d(const char *name, vec3 position, float volume, float rate, float ref_distance, float rolloff);
// SOUND.play3DL: positional loop (map sound emitters), no otherSoundMlt.
void audio_play_3d_loop(const char *name, vec3 position, float volume, float rate, float ref_distance, float rolloff);
// SOUND.play3Dv: distance-faded 2D sound, played when 1 - dist/range > 0.1.
void audio_play_3d_fade(const char *name, vec3 position, float range, float volume, float rate);

// SOUND.stop(name): every voice of that sound. audio_stop_3d_loops() ends the map emitters.
void audio_stop(const char *name);
void audio_stop_3d_loops(void);
void audio_stop_all(void);
