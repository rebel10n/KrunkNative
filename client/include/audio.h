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
// gunsVolume=3, playerVolume=4, uiVolume=5, assetVolume=6; v10.0.2 adds dialogueVolume=7, the
// "Action" sounds such as medals, bomb plant / defuse and zone calls, and skinVolume=8, cosmetic
// sounds such as shoe steps: v10.js:193554 volumes = [1 x 9]).
typedef enum {
    AUDIO_CAT_MASTER = 0,
    AUDIO_CAT_VOICE = 1,
    AUDIO_CAT_AMBIENT = 2,
    AUDIO_CAT_WEAPON = 3,
    AUDIO_CAT_PLAYER = 4,
    AUDIO_CAT_UI = 5,
    AUDIO_CAT_ASSET = 6,
    AUDIO_CAT_DIALOGUE = 7,
    AUDIO_CAT_SKIN = 8,
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

// Playback device selection (Settings > Sound > Audio Output, ui_client_settings.c). The
// device is identified by its backend name; "" or NULL = the backend's default device.
// Before audio_init() it only records the choice; afterwards the output device is re-opened
// live under the running engine (sounds keep playing). A name that is not connected falls
// back to the default device. No-op on the web (Web Audio has one output).
#define AUDIO_DEVICE_NAME_MAX 256
typedef struct {
    char name[AUDIO_DEVICE_NAME_MAX];
    int is_default;
} AudioDeviceInfo;
// The playback devices of the backend in use (at most `max`); 0 without audio or on the web.
int audio_output_devices(AudioDeviceInfo *out, int max);
void audio_set_output_device(const char *name);

// Per frame: listener = camera (Howler.pos/orientation), finished voices are recycled and
// deferred loads/plays are serviced (decoding never runs inside UI event handlers, which on
// the web cannot block for an asset fetch).
void audio_update(vec3 listener_position, float yaw, float pitch);

// SOUND.setVolume(v, category). Defaults are Krunker's slider defaults
// (master 0.8, weapons 0.5, everything else 1).
void audio_set_volume(AudioCategory category, float volume);
float audio_get_volume(AudioCategory category);
// SOUND.getVolume(name): the category volume a sound plays at (1 for a sound soundCats does not list)
float audio_sound_volume(const char *name);
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
// Howler's pannerAttr distanceModel, the Web Audio PannerNode models (deob-10.0.3.js:224689 ["inverse",
// "linear", "exponential"]): miniaudio's attenuation models of the same name.
typedef enum {
    AUDIO_DISTANCE_INVERSE = 0,
    AUDIO_DISTANCE_LINEAR = 1,
    AUDIO_DISTANCE_EXPONENTIAL = 2,
} AudioDistanceModel;
// SOUND.play3DL(name, x, y, z, vol, rate, dist, roll, isAsset, distM): positional loop (map sound
// emitters), no otherSoundMlt; distance_model is distM (AudioDistanceModel, anything else plays
// inverse). Unlike play(), a loop of the same sound that already plays does not stop a second one.
void audio_play_3d_loop(const char *name, vec3 position, float volume, float rate, float ref_distance, float rolloff,
                        int distance_model);
// SOUND.play3Dv: distance-faded 2D sound, played when 1 - dist/range > 0.1.
void audio_play_3d_fade(const char *name, vec3 position, float range, float volume, float rate);

// SOUND.stop(name): every voice of that sound. audio_stop_3d_loops() ends the map emitters.
void audio_stop(const char *name);
void audio_stop_3d_loops(void);
void audio_stop_all(void);
// The file of `name` changed (a resource pack came or went, mod_pack.c): its playing voices
// stop and the next play reads and decodes the file again (v7 SOUND.updateSound).
void audio_reload(const char *name);
void audio_reload_all(void); // F5 (game_reload.c): every sound, as audio_reload

// Voice handles (KrunkScript GAME.SOUND sound objects, client/src/ks/ks_sound.c): one play
// returns a handle (0 = not played) that later calls address; a finished one-shot or a stopped
// voice makes the handle inert. `volume` is Howl's vol (before the category factor).
int  audio_handle_play(const char *name, float volume, int loop, float rate, int positional, vec3 position,
                       float ref_distance, float rolloff);
void audio_handle_volume(int handle, float volume);
// The distance model (AudioDistanceModel), refDistance and rolloff the newest positional voice of
// `name` plays with, as miniaudio holds them; 0 when none plays (audio_distance_test).
int  audio_voice_distance(const char *name, int *model, float *ref_distance, float *rolloff);
void audio_handle_rate(int handle, float rate);
void audio_handle_fade(int handle, float from, float to, float ms);
void audio_handle_position(int handle, vec3 position); // a positional voice moves (Howl.pos; 2D voices ignore it)
void audio_handle_mute(int handle, int muted);
int  audio_handle_resume(int handle);  // play(): resumes a paused voice, restarts a finished one
void audio_handle_pause(int handle);
void audio_handle_stop(int handle);
int  audio_handle_active(int handle);
