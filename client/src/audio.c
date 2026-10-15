// Krunker's SOUND manager (v3.1.1 Krunker.Unpacked-v3.1.1/64.js) on miniaudio's ma_engine.
//
// Howler semantics kept here:
// - each play() starts a new overlapping voice of the clip (Howl sound pool);
// - per-voice volume = clamp(volume * category volume, 0, 1): Howler's volume() ignores
//   values outside [0, 1], so e.g. hit_0 at 3.1 plays at full volume, as here;
// - the master slider (category 0) is Howler.volume(), i.e. the engine volume, and is also
//   applied to sounds listed in category 0 (nuke_0, siren_0, beep_0, ...), like Krunker;
// - sounds missing from soundCats play with a category factor of 1;
// - play3D: Web Audio PannerNode with the "inverse" distance model, refDistance 25,
//   rolloffFactor 1, maxDistance 10000 = ma_attenuation_model_inverse with the same values
//   (panning is miniaudio's, level-matched to an equal-power panner; Howler uses HRTF).
//
// Clips are decoded to f32 once (assets/sound/<name>.mp3). Long files (the ambient loops,
// > AUDIO_STREAM_BYTES of MP3) stay compressed in memory and every voice streams them
// through its own decoder instead.
#include <audio.h>
#include <voice_chat.h> // voice_audio_fini
#include <shared.h>
#include "audio_miniaudio.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#define AUDIO_MAX_CLIPS 192
#define AUDIO_MAX_VOICES 64
#define AUDIO_MAX_PENDING 64
#define AUDIO_NAME_MAX 48
#define AUDIO_STREAM_BYTES (192 * 1024)
#ifdef __EMSCRIPTEN__
#define AUDIO_LOADS_PER_FRAME 1 // every first load is a blocking (asyncify) fetch
#else
#define AUDIO_LOADS_PER_FRAME 2
#endif

typedef enum {
    CLIP_UNLOADED = 0,
    CLIP_QUEUED,
    CLIP_READY,
    CLIP_FAILED,
} ClipState;

typedef struct {
    char name[AUDIO_NAME_MAX];
    ClipState state;
    int category;          // -1: not in Krunker's soundCats (factor 1)
    float *pcm;            // decoded clip (NULL when streamed)
    ma_uint64 frames;
    ma_uint32 channels;
    ma_uint32 sample_rate;
    unsigned char *file;   // compressed bytes of a streamed clip
    size_t file_size;
} AudioClip;

typedef enum {
    VOICE_2D,
    VOICE_3D,
    VOICE_3D_LOOP,
} VoiceKind;

typedef struct {
    int in_use;
    int clip;
    VoiceKind kind;
    int loop;
    int streamed;
    float volume;          // Howl `vol`: volume before the category factor
    uint32_t handle;       // audio_handle_play voice (KrunkScript sounds), 0 = none
    int muted;             // audio_handle_mute
    ma_audio_buffer_ref buffer;
    ma_decoder decoder;
    ma_sound sound;
} AudioVoice;

typedef struct {
    int clip;
    VoiceKind kind;
    int loop;
    vec3 position;
    float volume, rate, ref_distance, rolloff;
    uint32_t handle;       // audio_handle_play, 0 = none
} PendingPlay;

static struct {
    int ready;
    int log;
    ma_context context;
    int context_ready;
    ma_engine engine;
#ifndef __EMSCRIPTEN__
    // The playback device is ours (ma_engine_config.pDevice), not the engine's, so the
    // output can be re-opened under the running engine (audio_set_output_device).
    ma_device device;
    int device_ready;
#endif
    char output_device[AUDIO_DEVICE_NAME_MAX]; // requested playback device name, "" = default
    float volumes[AUDIO_CAT_COUNT];
    float rate_scale; // SOUND.rate (1 unless the round-end slow motion runs), see audio_set_rate

    AudioClip clips[AUDIO_MAX_CLIPS];
    int clip_count;
    AudioVoice voices[AUDIO_MAX_VOICES];
    PendingPlay pending[AUDIO_MAX_PENDING];
    int pending_count;

    FILE *capture;
    ma_uint64 capture_frames;
} g_audio = {
    .volumes = {0.8f, 1.0f, 1.0f, 0.5f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f},
    .rate_scale = 1.0f,
};

// SOUND.soundCats (v10.js:193556), verbatim: the category of every sound Krunker v10.0.2 lists. A sound
// it does not list plays at volume 1 (getVolume: `soundCats[name] != null ? volumes[cat] : 1`).
static const struct {
    const char *name;
    int category;
} SOUND_CATS[] = {
    {"nuke_0", 0}, {"siren_0", 0}, {"scare_0", 0}, {"scare_1", 0}, {"beep_0", 0}, {"dragon_0", 0}, {"egg_0", 0},
    {"ambient_1", 2}, {"ambient_2", 2}, {"ambient_3", 2}, {"ambient_4", 2}, {"ambient_5", 2}, {"ambient_6", 2},
    {"ambient_7", 2}, {"ambient_8", 2}, {"ambient_9", 2}, {"ambient_10", 2}, {"gong_0", 2}, {"ambient_11", 2},
    {"ambient_12", 2}, {"ambient_13", 2}, {"ambient_14", 2}, {"ambient_15", 2}, {"ambient_16", 2}, {"ambient_17", 2},
    {"ambient_18", 2}, {"ambient_19", 2}, {"jug_0", 2}, {"aim_0", 3}, {"aim_1", 3}, {"explosion", 3},
    {"gclick_0", 3}, {"pick_0", 3}, {"pick_1", 3}, {"reload_1", 3}, {"reload_2", 3}, {"swsh_0_0", 3},
    {"swsh_0_1", 3}, {"weapon_1", 3}, {"weapon_1_0", 3}, {"weapon_2", 3}, {"weapon_3", 3}, {"weapon_4", 3},
    {"weapon_5", 3}, {"weapon_6", 3}, {"weapon_7", 3}, {"weapon_8", 3}, {"weapon_9", 3}, {"weapon_10", 3},
    {"weapon_11", 3}, {"weapon_12", 3}, {"weapon_13", 3}, {"weapon_14", 3}, {"weapon_15", 3}, {"weapon_16", 3},
    {"weapon_17", 3}, {"weapon_18", 3}, {"whizz_0", 3}, {"whizz_1", 3}, {"weapon_6_0", 3}, {"weapon_1_2", 3},
    {"weapon_3_0", 3}, {"rico_1", 3}, {"rico_2", 3}, {"case_0", 3}, {"case_1", 3}, {"weapon_3_2", 3},
    {"weapon_2_0", 3}, {"weapon_1_4", 3}, {"weapon_1_3", 3}, {"weapon_17_0", 3}, {"weapon_8_0", 3},
    {"weapon_10_0", 3}, {"melee_12", 3}, {"swsh_12_0", 3}, {"swsh_12_1", 3}, {"weapon_4_0", 3}, {"melee_equip", 3},
    {"melee_14", 3}, {"swsh_14_0", 3}, {"swsh_14_1", 3}, {"weapon_1_5", 3}, {"weapon_19", 3}, {"melee_19", 3},
    {"swsh_19_0", 3}, {"swsh_19_1", 3}, {"weapon_11_0", 3}, {"weapon_4_1", 3}, {"weapon_4_2", 3}, {"weapon_4_3", 3},
    {"weapon_7_0", 3}, {"melee_25", 3}, {"melee_25_1", 3}, {"weapon_1_6", 3}, {"weapon_7_1", 3}, {"weapon_2_1", 3},
    {"weapon_1_7", 3}, {"weapon_2_3", 3}, {"swing_0", 3}, {"weapon_9_0", 3}, {"weapon_1_8", 3}, {"weapon_3_16", 3},
    {"weapon_14_0", 3}, {"weapon_10_1", 3}, {"weapon_11_1", 3}, {"melee_38", 3}, {"weapon_12_0", 3},
    {"weapon_14_1", 3}, {"weapon_6_1", 3}, {"weapon_2_4", 3}, {"weapon_10_2", 3}, {"weapon_8_1", 3},
    {"weapon_5_0", 3}, {"weapon_4_4", 3}, {"weapon_20", 3}, {"weapon_17_1", 3}, {"weapon_3_3", 3}, {"melee_42", 3},
    {"weapon_2_5", 3}, {"weapon_1_12", 3}, {"weapon_1_9", 3}, {"weapon_2_6", 3}, {"weapon_2_7", 3},
    {"weapon_2_8", 3}, {"weapon_2_9", 3}, {"weapon_5_1", 3}, {"weapon_5_2", 3}, {"weapon_7_2", 3}, {"weapon_7_3", 3},
    {"weapon_7_4", 3}, {"weapon_7_5", 3}, {"weapon_9_1", 3}, {"weapon_10_3", 3}, {"weapon_15_1", 3},
    {"weapon_15_2", 3}, {"weapon_15_3", 3}, {"weapon_16_1", 3}, {"weapon_6_2", 3}, {"weapon_3_4", 3},
    {"weapon_4_6", 3}, {"weapon_4_5", 3}, {"weapon_1_11", 3}, {"weapon_17_3", 3}, {"weapon_2_10", 3},
    {"weapon_7_6", 3}, {"weapon_8_2", 3}, {"weapon_11_3", 3}, {"weapon_1_10", 3}, {"weapon_22", 3},
    {"weapon_17_4", 3}, {"weapon_19_0", 3}, {"weapon_12_1", 3}, {"weapon_14_2", 3}, {"weapon_1_13", 3},
    {"weapon_2_11", 3}, {"weapon_3_6", 3}, {"weapon_4_8", 3}, {"weapon_5_3", 3}, {"weapon_6_3", 3},
    {"weapon_7_7", 3}, {"weapon_8_3", 3}, {"weapon_9_2", 3}, {"weapon_10_4", 3}, {"weapon_11_4", 3},
    {"weapon_12_2", 3}, {"weapon_14_3", 3}, {"weapon_15_4", 3}, {"weapon_16_2", 3}, {"weapon_19_1", 3},
    {"weapon_3_5", 3}, {"weapon_9_3", 3}, {"weapon_6_4", 3}, {"weapon_2_13", 3}, {"weapon_7_9", 3}, {"melee_65", 3},
    {"weapon_28_0", 3}, {"weapon_11_6", 3}, {"weapon_10_6", 3}, {"weapon_1_15", 3}, {"weapon_1_16", 3},
    {"weapon_2_14", 3}, {"weapon_6_6", 3}, {"weapon_4_10", 3}, {"weapon_2_15", 3}, {"weapon_1_17", 3},
    {"weapon_23", 3}, {"weapon_6_7", 3}, {"weapon_2_16", 3}, {"weapon_21", 3}, {"weapon_6_8", 3}, {"weapon_6_9", 3},
    {"weapon_11_7", 3}, {"weapon_7_10", 3}, {"weapon_24", 3}, {"weapon_4_11", 3}, {"weapon_1_18", 3},
    {"weapon_25", 3}, {"weapon_26", 3}, {"weapon_5_5", 3}, {"weapon_1_19", 3}, {"weapon_27", 3}, {"weapon_28", 3},
    {"weapon_6_10", 3}, {"weapon_1_20", 3}, {"weapon_17_6", 3}, {"weapon_3_8", 3}, {"weapon_2_18", 3},
    {"weapon_29", 3}, {"weapon_29_blast", 3}, {"weapon_4_12", 3}, {"weapon_1_21", 3}, {"weapon_9_6", 3},
    {"weapon_4_13", 3}, {"weapon_16_4", 3}, {"weapon_4_14", 3}, {"weapon_1_22", 3}, {"weapon_8_5", 3},
    {"weapon_8_5_r_1", 3}, {"weapon_8_5_r_2", 3}, {"weapon_4_15", 3}, {"weapon_6_11", 3}, {"weapon_9_7", 3},
    {"weapon_upgraded", 3}, {"weapon_14_5", 3}, {"weapon_12_4", 3}, {"weapon_3_9", 3}, {"weapon_4_17", 3},
    {"weapon_5_6", 3}, {"weapon_19_3", 3}, {"weapon_1_23", 3}, {"weapon_1_24", 3}, {"weapon_4_18", 3},
    {"weapon_11_9", 3}, {"weapon_29_2", 3}, {"weapon_4_16", 3}, {"weapon_2_19", 3}, {"weapon_2_20", 3},
    {"weapon_10_7", 3}, {"weapon_3_10", 3}, {"weapon_31", 3}, {"weapon_15_7", 3}, {"weapon_15_7_r_1", 3},
    {"weapon_15_7_r_2", 3}, {"weapon_8_8", 3}, {"weapon_2_25", 3}, {"weapon_2_26", 3}, {"weapon_7_16", 3},
    {"weapon_1_33", 3}, {"weapon_4_20", 3}, {"weapon_4_21", 3}, {"weapon_5_10", 3}, {"weapon_1_34", 3},
    {"weapon_29_3", 3}, {"weapon_14_6", 3}, {"weapon_11_10", 3}, {"weapon_2_27", 3}, {"weapon_6_14", 3},
    {"weapon_31_1", 3}, {"weapon_31_1_r_1", 3}, {"weapon_31_1_r_2", 3}, {"weapon_8_10", 3}, {"weapon_8_10_r_1", 3},
    {"weapon_8_10_r_2", 3}, {"weapon_5_11", 3}, {"weapon_5_11_r_1", 3}, {"weapon_5_11_r_2", 3}, {"weapon_10_12", 3},
    {"weapon_6_15", 3}, {"weapon_6_16", 3}, {"weapon_1_35", 3}, {"weapon_3_11", 3}, {"weapon_22_4", 3},
    {"weapon_8_7", 3}, {"weapon_10_11", 3}, {"weapon_7_15", 3}, {"weapon_6_12", 3}, {"weapon_7_13", 3},
    {"weapon_9_8", 3}, {"weapon_5_7", 3}, {"weapon_1_31", 3}, {"weapon_7_14", 3}, {"weapon_2_22", 3},
    {"weapon_8_6", 3}, {"weapon_5_8", 3}, {"weapon_1_32", 3}, {"weapon_7_12", 3}, {"weapon_2_24", 3},
    {"weapon_4_19", 3}, {"weapon_1_30", 3}, {"weapon_19_4", 3}, {"weapon_19_6", 3}, {"weapon_1_28", 3},
    {"weapon_6_13", 3}, {"weapon_1_29", 3}, {"weapon_2_23", 3}, {"weapon_7_11", 3}, {"weapon_5_9", 3},
    {"weapon_22_3", 3}, {"weapon_10_10", 3}, {"weapon_2_28", 3}, {"weapon_1_36", 3}, {"weapon_3_12", 3},
    {"weapon_4_22", 3}, {"weapon_1_37", 3}, {"weapon_5_12", 3}, {"weapon_6_17", 3}, {"weapon_12_6", 3},
    {"weapon_1_38", 3}, {"weapon_4_23", 3}, {"weapon_4_24", 3}, {"weapon_15_8", 3}, {"weapon_15_9", 3},
    {"weapon_2_29", 3}, {"weapon_4_25", 3}, {"weapon_6_19", 3}, {"weapon_7_18", 3}, {"weapon_1_39", 3},
    {"weapon_2_30", 3}, {"weapon_4_27", 3}, {"weapon_8_12", 3}, {"weapon_4_28", 3}, {"melee_butterfly_equip", 3},
    {"melee_butterfly_inspect", 3}, {"weapon_1_42", 3}, {"weapon_5_13", 3}, {"weapon_5_14", 3}, {"weapon_8_14", 3},
    {"weapon_10_13", 3}, {"weapon_5_15", 3}, {"weapon_14_8", 3}, {"weapon_14_9", 3}, {"weapon_5_16", 3},
    {"weapon_1_43", 3}, {"weapon_1_44", 3}, {"weapon_1_46", 3}, {"weapon_14_10", 3}, {"weapon_5_17", 3},
    {"weapon_2_33", 3}, {"weapon_5_18", 3}, {"weapon_4_29", 3}, {"weapon_1_47", 3}, {"weapon_1_48", 3},
    {"weapon_4_30", 3}, {"weapon_8_15", 3}, {"weapon_12_8", 3}, {"weapon_16_6", 3}, {"weapon_4_31", 3},
    {"weapon_1_49", 3}, {"weapon_4_32", 3}, {"weapon_1_50", 3}, {"weapon_4_33", 3}, {"weapon_7_19", 3},
    {"weapon_17_8", 3}, {"weapon_19_8", 3}, {"weapon_11_12", 3}, {"weapon_5_20", 3}, {"weapon_6_21", 3},
    {"weapon_3_14", 3}, {"weapon_15_11", 3}, {"weapon_8_17", 3}, {"weapon_22_5", 3}, {"weapon_1_52", 3},
    {"weapon_4_36", 3}, {"melee_dragonbfk_inspect", 3}, {"melee_dragonbfk_equip", 3}, {"vcoreintial", 3},
    {"vcoreblasterinspect", 3}, {"vcoredeagleinspect", 3}, {"vcoreaxeinspect", 3}, {"fart_0", 4}, {"headshot_0", 4},
    {"hit_0", 4}, {"impact_0", 4}, {"jump_0", 4}, {"jump_1", 4}, {"land_0", 4}, {"punch_0", 4}, {"punch_1", 4},
    {"slide_0", 4}, {"spray", 4}, {"step_0", 4}, {"step_1", 4}, {"step_2", 4}, {"taunt_0", 4}, {"taunt_1", 4},
    {"taunt_2", 4}, {"taunt_3", 4}, {"taunt_4", 4}, {"cough_1", 4}, {"cough_2", 4}, {"collect_1", 4},
    {"bounce_0", 4}, {"build_0", 4}, {"break_wood", 4}, {"taunt_5", 4}, {"spray_1", 4}, {"takedmg_0", 4},
    {"takedmg_1", 4}, {"stepwo_0", 4}, {"stepwo_1", 4}, {"stepwat_0", 4}, {"stepwat_1", 4}, {"stepsa_0", 4},
    {"stepsa_1", 4}, {"stepsno_0", 4}, {"stepsno_1", 4}, {"stepsno_2", 4}, {"egg_1", 4}, {"bounce_1", 4},
    {"zap_0", 4}, {"crit_0", 4}, {"reload_3", 4}, {"buy_0", 5}, {"buy_1", 5}, {"cheer_0", 5}, {"reward", 5},
    {"cheer_1", 5}, {"store", 5}, {"tick_0", 5}, {"limited_0", 5}, {"limited_1", 5}, {"limited_2", 5},
    {"limited_3", 5}, {"limited_4", 5}, {"limited_5", 5}, {"limited_6", 5}, {"limited_7", 5}, {"bundle_1", 5},
    {"bundle_2", 5}, {"bundle_3", 5}, {"bundle_4", 5}, {"select_0", 5}, {"unwrap_0", 5}, {"hover_0", 5},
    {"bundle_5", 5}, {"menu_0", 5}, {"bundle_6", 5}, {"bundle_7", 5}, {"menu_1", 5}, {"bundle_8", 5},
    {"bundle_9", 5}, {"bundle_10", 5}, {"bundle_11", 5}, {"bundle_12", 5}, {"bundle_13", 5}, {"bundle_14", 5},
    {"bundle_15", 5}, {"bundle_16", 5}, {"bundle_17", 5}, {"bundle_18", 5}, {"eterno_lofi", 5}, {"bo_plant", 7},
    {"bo_diff", 7}, {"bhop_reset", 7}, {"bhop_check", 7}, {"dom_zone_securing_0", 7}, {"dom_zone_securing_1", 7},
    {"dom_zone_losing_0", 7}, {"dom_zone_secured_0", 7}, {"dom_zone_secured_1", 7}, {"dom_zone_secured_2", 7},
    {"dom_zone_all_0", 7}, {"dom_zone_all_1", 7}, {"medal_0", 7}, {"medal_1", 7}, {"gift_0", 7}, {"mystery_0", 7},
    {"mystery_1", 7}, {"firesale_0", 7}, {"firesale_1", 7}, {"instantkill_0", 7}, {"shoe_1", 8}, {"shoe_2", 8},
    {"shoe_19", 8}, {"shoe_28", 8}, {"shoe_46", 8}, {"inspect_idol_music_loop", 8}, {"inspect_monte_carlo_spin", 8},
};

// ---------------------------------------------------------------- helpers

static int strncasecmp_ascii(const char *a, const char *b, const size_t n) {
    for (size_t i = 0; i < n; i++) {
        const int ca = a[i] >= 'A' && a[i] <= 'Z' ? a[i] + 32 : a[i];
        const int cb = b[i] >= 'A' && b[i] <= 'Z' ? b[i] + 32 : b[i];
        if (ca != cb || !ca) return ca - cb;
    }
    return 0;
}

static float clamp01(const float v) {
    return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
}

// User-asset sounds (sound.js getSound with the asset flag: userAssetsUrl(id + "/sound") +
// ".mp3", Howl category 6 = assetVolume) are named "asset:<id>" (AI sounds, trigger "esa").
#define AUDIO_ASSET_PREFIX "asset:"

static int sound_is_asset(const char *name) {
    return !strncmp(name, AUDIO_ASSET_PREFIX, sizeof(AUDIO_ASSET_PREFIX) - 1);
}

// The file of a clip under assets/: sound/<name>.mp3, or user-assets/<id>/sound.mp3.
static void clip_relative_path(const char *name, char *out, const size_t size) {
    if (sound_is_asset(name)) snprintf(out, size, "user-assets/%s/sound.mp3", name + sizeof(AUDIO_ASSET_PREFIX) - 1);
    else snprintf(out, size, "sound/%s.mp3", name);
}

// KrunkNative's own clips (outside Krunker's table): the Nettspend class (client/src/nettspend_audio.c)
// under the weapons slider, its laced drone under ambience.
static const struct {
    const char *name;
    int category;
} KN_SOUND_CATS[] = {
    {"kn_drank_reload", 3}, {"kn_drank_pour", 3}, {"kn_drank_splash", 3}, {"kn_money_throw", 3}, {"kn_laced_ambience", 2},
};

static int sound_category(const char *name) {
    if (sound_is_asset(name)) return AUDIO_CAT_ASSET;
    for (size_t i = 0; i < sizeof(SOUND_CATS) / sizeof(SOUND_CATS[0]); i++) {
        if (!strcmp(SOUND_CATS[i].name, name)) return SOUND_CATS[i].category;
    }
    for (size_t i = 0; i < sizeof(KN_SOUND_CATS) / sizeof(KN_SOUND_CATS[0]); i++) {
        if (!strcmp(KN_SOUND_CATS[i].name, name)) return KN_SOUND_CATS[i].category;
    }
    return -1;
}

static float category_factor(const int category) {
    return category < 0 ? 1.0f : g_audio.volumes[category];
}

static double audio_now(void) {
    return (double) ma_engine_get_time_in_milliseconds(&g_audio.engine) / 1000.0;
}

static int clip_find(const char *name, const int create) {
    if (!name || !*name || strlen(name) >= AUDIO_NAME_MAX) return -1;

    for (int i = 0; i < g_audio.clip_count; i++) {
        if (!strcmp(g_audio.clips[i].name, name)) return i;
    }

    if (!create || g_audio.clip_count >= AUDIO_MAX_CLIPS) return -1;

    AudioClip *clip = &g_audio.clips[g_audio.clip_count];
    memset(clip, 0, sizeof(*clip));
    strcpy(clip->name, name);
    clip->category = sound_category(name);
    return g_audio.clip_count++;
}

// 0 while assets/sound/<name>.mp3 is still downloading in the background (web).
static int clip_prefetched(const AudioClip *clip) {
    char relative[AUDIO_NAME_MAX + 32];
    clip_relative_path(clip->name, relative, sizeof(relative));
    char *path = concat(assets_path(), relative);
    const int state = path ? asset_prefetch(path) : -1;
    free(path);
    return state != 0;
}

// Read and (for short clips) decode assets/sound/<name>.mp3. Can block on the web
// (ensure_asset_file fetch): only called from audio_update().
static int clip_load(AudioClip *clip) {
    if (clip->state == CLIP_READY) return 1;
    if (clip->state == CLIP_FAILED) return 0;

    char relative[AUDIO_NAME_MAX + 32];
    clip_relative_path(clip->name, relative, sizeof(relative));
    char *path = concat(assets_path(), relative);

    unsigned char *data = NULL;
    size_t size = 0;
    const int read_error = !path || read_file(path, &data, &size);
    // natively a missing file may be downloading (asset_fetch.h): not failed, the next play reads it
    const int downloading = (read_error || !data || !size) && path && asset_prefetch(path) == 0;
    free(path);

    if (downloading) {
        free(data);
        return 0;
    }
    if (read_error || !data || !size) {
        free(data);
        fprintf(stderr, "[audio] missing sound %s\n", clip->name);
        clip->state = CLIP_FAILED;
        return 0;
    }

    ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0);

    if (size > AUDIO_STREAM_BYTES) {
        // Long loop: probe the format, keep the MP3 bytes, decode per voice while playing.
        ma_decoder probe;
        if (ma_decoder_init_memory(data, size, &config, &probe) != MA_SUCCESS) {
            free(data);
            fprintf(stderr, "[audio] cannot decode %s\n", clip->name);
            clip->state = CLIP_FAILED;
            return 0;
        }
        clip->channels = probe.outputChannels;
        clip->sample_rate = probe.outputSampleRate;
        ma_decoder_uninit(&probe);

        clip->file = data;
        clip->file_size = size;
    } else {
        void *pcm = NULL;
        ma_uint64 frames = 0;
        const ma_result result = ma_decode_memory(data, size, &config, &frames, &pcm);
        free(data);

        if (result != MA_SUCCESS || !pcm || !frames) {
            if (pcm) ma_free(pcm, NULL);
            fprintf(stderr, "[audio] cannot decode %s\n", clip->name);
            clip->state = CLIP_FAILED;
            return 0;
        }

        clip->pcm = pcm;
        clip->frames = frames;
        clip->channels = config.channels;
        clip->sample_rate = config.sampleRate;
    }

    clip->state = CLIP_READY;
    return 1;
}

static void clip_free(AudioClip *clip) {
    if (clip->pcm) ma_free(clip->pcm, NULL);
    free(clip->file);
    clip->pcm = NULL;
    clip->file = NULL;
    clip->state = CLIP_UNLOADED;
}

// ---------------------------------------------------------------- voices

static void voice_release(AudioVoice *voice) {
    if (!voice->in_use) return;
    ma_sound_uninit(&voice->sound);
    if (voice->streamed) ma_decoder_uninit(&voice->decoder);
    else ma_audio_buffer_ref_uninit(&voice->buffer);
    voice->in_use = 0;
}

// miniaudio's spatializer pans linearly: a sound straight ahead reaches each speaker at
// 0.5 gain. Web Audio's panner (Howler) keeps a frontal stereo source at unity and a mono
// one at cos(PI/4) per speaker (equal power), so spatialized voices are scaled back up.
static float spatial_compensation(const AudioVoice *voice) {
    if (voice->kind == VOICE_2D) return 1.0f;
    return g_audio.clips[voice->clip].channels >= 2 ? 2.0f : (float) M_SQRT2;
}

static void voice_apply_volume(AudioVoice *voice) {
    const AudioClip *clip = &g_audio.clips[voice->clip];
    if (voice->muted) {
        ma_sound_set_volume(&voice->sound, 0.0f);
        return;
    }
    ma_sound_set_volume(&voice->sound, clamp01(voice->volume * category_factor(clip->category)) * spatial_compensation(voice));
}

static AudioVoice *voice_alloc(void) {
    for (int i = 0; i < AUDIO_MAX_VOICES; i++) {
        if (!g_audio.voices[i].in_use) return &g_audio.voices[i];
    }

    // Pool full: steal the one-shot that has played longest (loops are never stolen).
    AudioVoice *oldest = NULL;
    ma_uint64 oldest_cursor = 0;

    for (int i = 0; i < AUDIO_MAX_VOICES; i++) {
        AudioVoice *voice = &g_audio.voices[i];
        if (voice->loop) continue;

        ma_uint64 cursor = 0;
        ma_sound_get_cursor_in_pcm_frames(&voice->sound, &cursor);
        if (!oldest || cursor > oldest_cursor) {
            oldest = voice;
            oldest_cursor = cursor;
        }
    }

    if (oldest) voice_release(oldest);
    return oldest;
}

static void voice_start(const PendingPlay *play) {
    AudioClip *clip = &g_audio.clips[play->clip];
    if (!clip_load(clip)) return;

    AudioVoice *voice = voice_alloc();
    if (!voice) return;

    memset(voice, 0, sizeof(*voice));
    voice->clip = play->clip;
    voice->kind = play->kind;
    voice->loop = play->loop;
    voice->volume = play->volume;
    voice->handle = play->handle;
    voice->streamed = clip->file != NULL;

    ma_data_source *source;

    if (voice->streamed) {
        ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0);
        if (ma_decoder_init_memory(clip->file, clip->file_size, &config, &voice->decoder) != MA_SUCCESS) return;
        source = &voice->decoder;
    } else {
        if (ma_audio_buffer_ref_init(ma_format_f32, clip->channels, clip->pcm, clip->frames, &voice->buffer) != MA_SUCCESS) return;
        voice->buffer.sampleRate = clip->sample_rate;
        source = &voice->buffer;
    }

    const ma_uint32 flags = play->kind == VOICE_2D ? MA_SOUND_FLAG_NO_SPATIALIZATION : 0;

    if (ma_sound_init_from_data_source(&g_audio.engine, source, flags, NULL, &voice->sound) != MA_SUCCESS) {
        if (voice->streamed) ma_decoder_uninit(&voice->decoder);
        else ma_audio_buffer_ref_uninit(&voice->buffer);
        return;
    }

    voice->in_use = 1;

    if (play->kind != VOICE_2D) {
        ma_sound_set_position(&voice->sound, play->position.x, play->position.y, play->position.z);
        ma_sound_set_attenuation_model(&voice->sound, ma_attenuation_model_inverse);
        ma_sound_set_min_distance(&voice->sound, play->ref_distance);
        ma_sound_set_max_distance(&voice->sound, AUDIO_MAX_DISTANCE);
        ma_sound_set_rolloff(&voice->sound, play->rolloff);
        ma_sound_set_doppler_factor(&voice->sound, 0.0f); // Web Audio panners have no doppler
    }

    ma_sound_set_pitch(&voice->sound, play->rate);
    ma_sound_set_looping(&voice->sound, play->loop ? MA_TRUE : MA_FALSE);
    voice_apply_volume(voice);
    ma_sound_start(&voice->sound);

    if (g_audio.log) {
        printf("[audio] t=%.3f play%s %s vol=%.3f rate=%.2f%s", audio_now(),
            play->kind == VOICE_2D ? "2d" : "3d", clip->name,
            clamp01(voice->volume * category_factor(clip->category)) * g_audio.volumes[AUDIO_CAT_MASTER],
            play->rate, play->loop ? " loop" : "");
        if (play->kind != VOICE_2D) {
            const ma_vec3f l = ma_engine_listener_get_position(&g_audio.engine, 0);
            const float dx = play->position.x - l.x, dy = play->position.y - l.y, dz = play->position.z - l.z;
            printf(" pos=(%.1f,%.1f,%.1f) dist=%.1f", play->position.x, play->position.y, play->position.z, sqrtf(dx * dx + dy * dy + dz * dz));
        }
        printf("\n");
        fflush(stdout);
    }
}

static int clip_is_playing(const int clip, const VoiceKind kind) {
    for (int i = 0; i < AUDIO_MAX_VOICES; i++) {
        const AudioVoice *voice = &g_audio.voices[i];
        if (voice->in_use && voice->clip == clip && voice->kind == kind && ma_sound_is_playing(&voice->sound)) return 1;
    }
    for (int i = 0; i < g_audio.pending_count; i++) {
        if (g_audio.pending[i].clip == clip && g_audio.pending[i].kind == kind) return 1;
    }
    return 0;
}

static void play_request_handle(const char *name, const VoiceKind kind, const int loop, const vec3 position,
                                const float volume, const float rate, const float ref_distance, const float rolloff,
                                const uint32_t handle) {
    if (!g_audio.ready) return;

    const int clip = clip_find(name, 1);
    if (clip < 0) return;
    if (g_audio.clips[clip].state == CLIP_FAILED) return;

    // SOUND.play(..., loop): `if (!j || !f.isPlaying)` - a running loop is not restarted.
    if (loop && clip_is_playing(clip, kind)) return;

    const PendingPlay play = {
        .clip = clip,
        .kind = kind,
        .loop = loop,
        .position = position,
        .volume = volume,
        .rate = rate > 0.0f ? rate : 1.0f,
        .ref_distance = ref_distance > 0.0f ? ref_distance : AUDIO_REF_DISTANCE,
        .rolloff = rolloff > 0.0f ? rolloff : AUDIO_ROLLOFF,
        .handle = handle,
    };

    if (g_audio.clips[clip].state == CLIP_READY) {
        voice_start(&play);
        return;
    }

    // Not decoded yet: audio_update() loads it (outside any UI event handler) and plays it.
    if (g_audio.pending_count < AUDIO_MAX_PENDING) g_audio.pending[g_audio.pending_count++] = play;
    if (g_audio.clips[clip].state == CLIP_UNLOADED) g_audio.clips[clip].state = CLIP_QUEUED;
}

static void play_request(const char *name, const VoiceKind kind, const int loop, const vec3 position,
                         const float volume, const float rate, const float ref_distance, const float rolloff) {
    play_request_handle(name, kind, loop, position, volume, rate, ref_distance, rolloff, 0);
}

// ---------------------------------------------------------------- capture (debug)

static void capture_write_header(FILE *file, const ma_uint32 sample_rate, const ma_uint64 frames) {
    const ma_uint32 data_bytes = (ma_uint32) (frames * 4);
    const ma_uint32 byte_rate = sample_rate * 4;
    unsigned char header[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ',
                                16, 0, 0, 0, 1, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 0, 16, 0,
                                'd', 'a', 't', 'a', 0, 0, 0, 0};
    const ma_uint32 riff = 36 + data_bytes;
    for (int i = 0; i < 4; i++) {
        header[4 + i] = (unsigned char) (riff >> (8 * i));
        header[24 + i] = (unsigned char) (sample_rate >> (8 * i));
        header[28 + i] = (unsigned char) (byte_rate >> (8 * i));
        header[40 + i] = (unsigned char) (data_bytes >> (8 * i));
    }
    fseek(file, 0, SEEK_SET);
    fwrite(header, 1, sizeof(header), file);
    fseek(file, 0, SEEK_END);
}

// ma_engine onProcess: the final (master-scaled) mix, on the audio thread.
static void capture_process(void *user, float *frames, const ma_uint64 frame_count) {
    (void) user;
    if (!g_audio.capture) return;

    const ma_uint32 channels = ma_engine_get_channels(&g_audio.engine);
    short out[2 * 512];

    for (ma_uint64 done = 0; done < frame_count;) {
        const ma_uint64 n = frame_count - done < 512 ? frame_count - done : 512;
        for (ma_uint64 i = 0; i < n; i++) {
            const float *in = frames + (done + i) * channels;
            const float l = in[0];
            const float r = channels > 1 ? in[1] : in[0];
            out[i * 2] = (short) (fmaxf(-1.0f, fminf(1.0f, l)) * 32767.0f);
            out[i * 2 + 1] = (short) (fmaxf(-1.0f, fminf(1.0f, r)) * 32767.0f);
        }
        fwrite(out, 4, (size_t) n, g_audio.capture);
        done += n;
    }

    // Keep the header valid while running (the client is often killed in automated runs).
    const ma_uint64 second = ma_engine_get_sample_rate(&g_audio.engine);
    const ma_uint64 before = g_audio.capture_frames;
    g_audio.capture_frames += frame_count;
    if (second && before / second != g_audio.capture_frames / second) {
        capture_write_header(g_audio.capture, (ma_uint32) second, g_audio.capture_frames);
    }
}

// ---------------------------------------------------------------- lifecycle

#ifdef __EMSCRIPTEN__
// miniaudio resumes its AudioContext on the first click/touchend (autoplay policy); keys
// and pointer presses are user activations as well (the game is often entered by key).
EM_JS(void, audio_web_install_unlock, (void), {
    if (typeof window === 'undefined' || !window.miniaudio || window.knAudioUnlock) return;
    window.knAudioUnlock = function () {
        if (window.miniaudio && window.miniaudio.unlock) window.miniaudio.unlock();
    };
    ['keydown', 'pointerdown', 'mousedown'].forEach(function (type) {
        document.addEventListener(type, window.knAudioUnlock, true);
    });
});
#endif

#ifndef __EMSCRIPTEN__
// The engine does not own the device (ma_engine_config.pDevice): its callback pulls the mix.
static void output_data_callback(ma_device *device, void *out, const void *in, const ma_uint32 frame_count) {
    (void) in;
    ma_engine_read_pcm_frames((ma_engine *) device->pUserData, out, frame_count, NULL);
}

// Opens g_audio.device on g_audio.output_device (by name), falling back to the default
// device when that one is not connected or fails to open. channels/sample_rate 0 = the
// device's native format (first open); a re-open passes the engine's so its node graph
// keeps working (miniaudio converts to the new device's format).
static int output_device_open(const ma_uint32 channels, const ma_uint32 sample_rate) {
    ma_device_id id;
    memset(&id, 0, sizeof(id));
    int have_id = 0;
    if (g_audio.output_device[0]) {
        ma_device_info *infos = NULL;
        ma_uint32 count = 0;
        if (ma_context_get_devices(&g_audio.context, &infos, &count, NULL, NULL) == MA_SUCCESS) {
            for (ma_uint32 i = 0; i < count && !have_id; i++) {
                if (strcmp(infos[i].name, g_audio.output_device)) continue;
                id = infos[i].id;
                have_id = 1;
            }
        }
        if (!have_id) fprintf(stderr, "[audio] output device \"%s\" not found, using the default device\n", g_audio.output_device);
    }
    for (int attempt = have_id ? 0 : 1; attempt < 2; attempt++) {
        ma_device_config dc = ma_device_config_init(ma_device_type_playback);
        dc.playback.pDeviceID = attempt == 0 ? &id : NULL;
        dc.playback.format = ma_format_f32;
        dc.playback.channels = channels;
        dc.sampleRate = sample_rate;
        dc.dataCallback = output_data_callback;
        dc.pUserData = &g_audio.engine;
        dc.noPreSilencedOutputBuffer = MA_TRUE; // the engine writes every frame (ma_engine_init does the same)
        dc.noClip = MA_TRUE;
        const ma_result r = ma_device_init(&g_audio.context, &dc, &g_audio.device);
        if (r == MA_SUCCESS) {
            g_audio.device_ready = 1;
            return 1;
        }
        fprintf(stderr, "[audio] %s: cannot open output device \"%s\": %s\n", ma_get_backend_name(g_audio.context.backend),
                attempt == 0 ? g_audio.output_device : "default", ma_result_description(r));
    }
    return 0;
}
#endif

int audio_output_devices(AudioDeviceInfo *out, const int max) {
#ifdef __EMSCRIPTEN__
    (void) out;
    (void) max;
    return 0;
#else
    if (!g_audio.context_ready || !out || max <= 0) return 0;
    ma_device_info *infos = NULL;
    ma_uint32 count = 0;
    if (ma_context_get_devices(&g_audio.context, &infos, &count, NULL, NULL) != MA_SUCCESS) return 0;
    int n = 0;
    for (ma_uint32 i = 0; i < count && n < max; i++) {
        snprintf(out[n].name, sizeof(out[n].name), "%s", infos[i].name);
        out[n].is_default = infos[i].isDefault != 0;
        n++;
    }
    return n;
#endif
}

void audio_set_output_device(const char *name) {
    if (!name) name = "";
    if (!strcmp(name, g_audio.output_device)) return;
    snprintf(g_audio.output_device, sizeof(g_audio.output_device), "%s", name);
#ifndef __EMSCRIPTEN__
    if (!g_audio.ready || !g_audio.device_ready) return; // audio_init opens it
    // Re-open the output under the running engine: the node graph, voices and listener stay.
    const ma_uint32 channels = ma_engine_get_channels(&g_audio.engine);
    const ma_uint32 sample_rate = ma_engine_get_sample_rate(&g_audio.engine);
    ma_device_uninit(&g_audio.device); // stops it: no callback runs past this point
    g_audio.device_ready = 0;
    if (!output_device_open(channels, sample_rate)) {
        fprintf(stderr, "[audio] no output device, sound stopped\n");
        return;
    }
    if (ma_device_start(&g_audio.device) != MA_SUCCESS) fprintf(stderr, "[audio] cannot start the output device\n");
    printf("[audio] output device %s\n", g_audio.device.playback.name);
#endif
}

int audio_init(void) {
    if (g_audio.ready) return 1;

    const char *enabled = getenv("KRUNKNATIVE_AUDIO");
    if (enabled && !strcmp(enabled, "0")) return 0;

    const char *log = getenv("KRUNKNATIVE_AUDIO_LOG");
    g_audio.log = log && *log && strcmp(log, "0") != 0;

    ma_engine_config config = ma_engine_config_init();
    config.listenerCount = 1;

    int backend_list[16];
    int backend_count = audio_backends(backend_list, 16);
    ma_backend backends[16];
    for (int i = 0; i < backend_count; i++) backends[i] = (ma_backend) backend_list[i];

    const char *capture = getenv("KRUNKNATIVE_AUDIO_CAPTURE");
    if (capture && *capture) {
        config.channels = 2;
        config.onProcess = capture_process;
    }

#ifdef __EMSCRIPTEN__
    if (backend_count) {
        if (ma_context_init(backends, (ma_uint32) backend_count, NULL, &g_audio.context) != MA_SUCCESS) return 0;
        g_audio.context_ready = 1;
        config.pContext = &g_audio.context;
    }
#else
    // Natively the context and the device are ours (the device list, live re-open). Like
    // ma_device_init_ex, a backend whose context comes up but whose device does not open
    // (e.g. a PulseAudio server refusing clients) is skipped for the next one.
    if (!backend_count) {
        size_t n = 0;
        ma_get_enabled_backends(backends, 16, &n);
        backend_count = (int) n;
    }
    for (int tries = 0; tries < 16 && backend_count > 0 && !g_audio.device_ready; tries++) {
        if (ma_context_init(backends, (ma_uint32) backend_count, NULL, &g_audio.context) != MA_SUCCESS) break;
        g_audio.context_ready = 1;
        if (output_device_open(config.channels, 0)) break;
        const ma_backend failed = g_audio.context.backend;
        ma_context_uninit(&g_audio.context);
        g_audio.context_ready = 0;
        int kept = 0;
        for (int i = 0; i < backend_count; i++) {
            if (backends[i] != failed) backends[kept++] = backends[i];
        }
        if (kept == backend_count) break; // the context reported a backend outside the list
        backend_count = kept;
    }
    if (!g_audio.device_ready) {
        fprintf(stderr, "[audio] no audio device, sound disabled\n");
        if (g_audio.context_ready) ma_context_uninit(&g_audio.context);
        g_audio.context_ready = 0;
        return 0;
    }
    config.pContext = &g_audio.context;
    config.pDevice = &g_audio.device;
#endif
    if (ma_engine_init(&config, &g_audio.engine) != MA_SUCCESS) {
        fprintf(stderr, "[audio] no audio device, sound disabled\n");
#ifndef __EMSCRIPTEN__
        ma_device_uninit(&g_audio.device);
        g_audio.device_ready = 0;
#endif
        if (g_audio.context_ready) ma_context_uninit(&g_audio.context);
        g_audio.context_ready = 0;
        return 0;
    }

    if (capture && *capture) {
        g_audio.capture = fopen(capture, "wb+");
        if (g_audio.capture) capture_write_header(g_audio.capture, ma_engine_get_sample_rate(&g_audio.engine), 0);
        else fprintf(stderr, "[audio] cannot open capture file %s\n", capture);
    }

    ma_engine_set_volume(&g_audio.engine, g_audio.volumes[AUDIO_CAT_MASTER]);
    ma_engine_listener_set_world_up(&g_audio.engine, 0, 0.0f, 1.0f, 0.0f);
    g_audio.ready = 1;

#ifdef __EMSCRIPTEN__
    audio_web_install_unlock();
#endif

    if (g_audio.log) {
        const ma_device *device = ma_engine_get_device(&g_audio.engine);
        printf("[audio] engine %u Hz, %u channels, %s: %s\n", ma_engine_get_sample_rate(&g_audio.engine), ma_engine_get_channels(&g_audio.engine),
            device ? ma_get_backend_name(device->pContext->backend) : "?", device ? device->playback.name : "?");
    }

    return 1;
}

void audio_fini(void) {
    if (!g_audio.ready) return;

    ma_engine_stop(&g_audio.engine); // no more onProcess calls past this point
    const ma_uint32 sample_rate = ma_engine_get_sample_rate(&g_audio.engine);

    voice_audio_fini(); // voice chat sounds (voice.c) live on this engine
    for (int i = 0; i < AUDIO_MAX_VOICES; i++) voice_release(&g_audio.voices[i]);
    ma_engine_uninit(&g_audio.engine);
#ifndef __EMSCRIPTEN__
    if (g_audio.device_ready) ma_device_uninit(&g_audio.device);
    g_audio.device_ready = 0;
#endif
    if (g_audio.context_ready) ma_context_uninit(&g_audio.context);
    g_audio.context_ready = 0;

    for (int i = 0; i < g_audio.clip_count; i++) clip_free(&g_audio.clips[i]);
    g_audio.clip_count = 0;
    g_audio.pending_count = 0;

    if (g_audio.capture) {
        capture_write_header(g_audio.capture, sample_rate, g_audio.capture_frames);
        fclose(g_audio.capture);
        g_audio.capture = NULL;
    }

    g_audio.ready = 0;
}

int audio_available(void) {
    return g_audio.ready;
}

void *audio_engine(void) {
    return g_audio.ready ? &g_audio.engine : NULL;
}

int audio_backends(int *out, const int max) {
    static const struct {
        const char *name;
        ma_backend backend;
    } NAMES[] = {
        {"pulseaudio", ma_backend_pulseaudio}, {"pulse", ma_backend_pulseaudio}, {"pipewire", ma_backend_pulseaudio},
        {"alsa", ma_backend_alsa}, {"jack", ma_backend_jack}, {"coreaudio", ma_backend_coreaudio},
        {"wasapi", ma_backend_wasapi}, {"dsound", ma_backend_dsound}, {"directsound", ma_backend_dsound},
        {"winmm", ma_backend_winmm}, {"sndio", ma_backend_sndio}, {"audio4", ma_backend_audio4},
        {"oss", ma_backend_oss}, {"aaudio", ma_backend_aaudio}, {"opensl", ma_backend_opensl},
        {"webaudio", ma_backend_webaudio}, {"null", ma_backend_null},
    };
    const char *list = getenv("KRUNKNATIVE_AUDIO_BACKEND");
    int count = 0;
    while (list && *list && count < max) {
        const size_t len = strcspn(list, ", ");
        int known = 0;
        for (size_t i = 0; len && i < sizeof(NAMES) / sizeof(NAMES[0]); i++) {
            if (strlen(NAMES[i].name) != len || strncasecmp_ascii(NAMES[i].name, list, len) != 0) continue;
            known = 1;
            if (ma_is_backend_enabled(NAMES[i].backend)) out[count++] = (int) NAMES[i].backend;
            else fprintf(stderr, "[audio] backend %.*s is not available in this build\n", (int) len, list);
        }
        if (len && !known) fprintf(stderr, "[audio] unknown audio backend %.*s\n", (int) len, list);
        list += len;
        while (*list == ',' || *list == ' ') list++;
    }
    return count;
}

void audio_update(const vec3 listener_position, const float yaw, const float pitch) {
    if (!g_audio.ready) return;

    // Howler.pos(camera) / Howler.orientation(forward): three.js YXZ camera, -Z forward.
    ma_engine_listener_set_position(&g_audio.engine, 0, listener_position.x, listener_position.y, listener_position.z);
    ma_engine_listener_set_direction(&g_audio.engine, 0,
        -sinf(yaw) * cosf(pitch), sinf(pitch), -cosf(yaw) * cosf(pitch));

    for (int i = 0; i < AUDIO_MAX_VOICES; i++) {
        AudioVoice *voice = &g_audio.voices[i];
        if (voice->in_use && !voice->loop && ma_sound_at_end(&voice->sound)) voice_release(voice);
    }

    // Deferred plays first (each loads its clip), then idle preloading.
    int loads = 0;
    int kept = 0;

    for (int i = 0; i < g_audio.pending_count; i++) {
        const PendingPlay play = g_audio.pending[i];
        AudioClip *clip = &g_audio.clips[play.clip];

        if (clip->state != CLIP_READY && clip->state != CLIP_FAILED) {
            if (loads >= AUDIO_LOADS_PER_FRAME) {
                g_audio.pending[kept++] = play;
                continue;
            }
            loads++;
            clip_load(clip);
        }

        if (clip->state == CLIP_READY) voice_start(&play);
    }

    g_audio.pending_count = kept;

    // Idle preloads download in the background first (asset_prefetch; web only, all queued
    // clips in parallel) and are decoded once their file is in MEMFS, so the menu never
    // waits on the network for a clip nobody is playing yet.
    for (int i = 0; i < g_audio.clip_count && loads < AUDIO_LOADS_PER_FRAME; i++) {
        if (g_audio.clips[i].state == CLIP_QUEUED) {
            if (!clip_prefetched(&g_audio.clips[i])) continue;
            clip_load(&g_audio.clips[i]);
            loads++;
        }
    }
}

// ---------------------------------------------------------------- volume

void audio_set_volume(const AudioCategory category, const float volume) {
    if ((int) category < 0 || category >= AUDIO_CAT_COUNT) return;

    g_audio.volumes[category] = clamp01(volume);
    if (!g_audio.ready) return;

    if (category == AUDIO_CAT_MASTER) ma_engine_set_volume(&g_audio.engine, g_audio.volumes[AUDIO_CAT_MASTER]);

    for (int i = 0; i < AUDIO_MAX_VOICES; i++) {
        AudioVoice *voice = &g_audio.voices[i];
        if (voice->in_use && g_audio.clips[voice->clip].category == (int) category) voice_apply_volume(voice);
    }
}

float audio_sound_volume(const char *name) {
    return name ? category_factor(sound_category(name)) : 0.0f;
}

float audio_get_volume(const AudioCategory category) {
    if ((int) category < 0 || category >= AUDIO_CAT_COUNT) return 0.0f;
    return g_audio.volumes[category];
}

int audio_apply_setting(const char *key, const char *value) {
    static const struct {
        const char *key;
        AudioCategory category;
    } KEYS[] = {
        {"sound", AUDIO_CAT_MASTER},
        {"voiceVolume", AUDIO_CAT_VOICE},
        {"ambientVolume", AUDIO_CAT_AMBIENT},
        {"gunsVolume", AUDIO_CAT_WEAPON},
        {"playerVolume", AUDIO_CAT_PLAYER},
        {"uiVolume", AUDIO_CAT_UI},
        {"assetVolume", AUDIO_CAT_ASSET},
        {"dialogueVolume", AUDIO_CAT_DIALOGUE}, // v10.js: SOUND.setVolume(v, 7), "Audio performed on action"
        {"skinVolume", AUDIO_CAT_SKIN},         // v10.js: SOUND.setVolume(v, 8), "Sound emitted from skins"
    };

    if (!key || !value) return 0;

    for (size_t i = 0; i < sizeof(KEYS) / sizeof(KEYS[0]); i++) {
        if (strcmp(KEYS[i].key, key) != 0) continue;

        char *end;
        const float v = strtof(value, &end);
        if (end != value) audio_set_volume(KEYS[i].category, v);
        return 1;
    }

    return 0;
}

// ---------------------------------------------------------------- playback

void audio_preload(const char *name) {
    if (!g_audio.ready) return;
    const int clip = clip_find(name, 1);
    if (clip >= 0 && g_audio.clips[clip].state == CLIP_UNLOADED) g_audio.clips[clip].state = CLIP_QUEUED;
}

void audio_set_rate(const float rate) {
    g_audio.rate_scale = rate > 0.0f ? rate : 0.0f;
}

void audio_play(const char *name, const float volume, const int loop, const float rate) {
    // SOUND.play: nothing plays while SOUND.rate is 0, and an explicit rate is scaled by it
    // (sounds played without one keep rate 1; 3D sounds ignore SOUND.rate).
    const float scale = g_audio.rate_scale;
    if (scale <= 0.0f) return;
    const vec3 origin = {0};
    play_request(name, VOICE_2D, loop, origin, volume > 0.0f ? volume : 1.0f, rate > 0.0f ? rate * scale : rate, 0.0f, 0.0f);
}

void audio_play_3d(const char *name, const vec3 position, const float volume, const float rate,
                   const float ref_distance, const float rolloff) {
    // play3D: `if ((m = (m || 1) * otherSoundMlt) > 0.05)`. (Krunker still starts the Howl
    // unpositioned below that threshold; such sounds are simply skipped here.)
    const float v = (volume > 0.0f ? volume : 1.0f) * AUDIO_OTHER_SOUND_MLT;
    if (v <= 0.05f) return;
    play_request(name, VOICE_3D, 0, position, v, rate, ref_distance, rolloff);
}

void audio_play_3d_loop(const char *name, const vec3 position, const float volume, const float rate,
                        const float ref_distance, const float rolloff) {
    const float v = volume > 0.0f ? volume : 1.0f;
    if (v <= 0.05f) return;
    play_request(name, VOICE_3D_LOOP, 1, position, v, rate, ref_distance, rolloff);
}

void audio_play_3d_fade(const char *name, const vec3 position, const float range, const float volume, const float rate) {
    if (!g_audio.ready || range <= 0.0f) return;

    const ma_vec3f listener = ma_engine_listener_get_position(&g_audio.engine, 0);
    const float dx = position.x - listener.x, dy = position.y - listener.y, dz = position.z - listener.z;
    const float fade = 1.0f - sqrtf(dx * dx + dy * dy + dz * dz) / range;
    if (fade <= 0.1f) return;

    const vec3 origin = {0};
    play_request(name, VOICE_2D, 0, origin, (volume > 0.0f ? volume : 1.0f) * fade * AUDIO_OTHER_SOUND_MLT, rate, 0.0f, 0.0f);
}

static void stop_matching(const int clip, const int only_kind, const VoiceKind kind) {
    for (int i = 0; i < AUDIO_MAX_VOICES; i++) {
        AudioVoice *voice = &g_audio.voices[i];
        if (!voice->in_use) continue;
        if (clip >= 0 && voice->clip != clip) continue;
        if (only_kind && voice->kind != kind) continue;
        voice_release(voice);
    }

    int kept = 0;
    for (int i = 0; i < g_audio.pending_count; i++) {
        const PendingPlay *play = &g_audio.pending[i];
        const int match = (clip < 0 || play->clip == clip) && (!only_kind || play->kind == kind);
        if (!match) g_audio.pending[kept++] = *play;
    }
    g_audio.pending_count = kept;
}

void audio_stop(const char *name) {
    if (!g_audio.ready) return;
    const int clip = clip_find(name, 0);
    if (clip < 0) return;
    if (g_audio.log) printf("[audio] t=%.3f stop %s\n", audio_now(), name);
    stop_matching(clip, 1, VOICE_2D);
}

void audio_reload(const char *name) {
    if (!g_audio.ready) return;
    const int clip = clip_find(name, 0);
    if (clip < 0) return;
    stop_matching(clip, 0, VOICE_2D); // its voices hold the decoded samples
    clip_free(&g_audio.clips[clip]);  // UNLOADED: the next play reads the file again
}

void audio_reload_all(void) {
    if (!g_audio.ready) return;
    for (int i = 0; i < g_audio.clip_count; i++) {
        stop_matching(i, 0, VOICE_2D);
        clip_free(&g_audio.clips[i]);
    }
}

void audio_stop_3d_loops(void) {
    if (!g_audio.ready) return;
    stop_matching(-1, 1, VOICE_3D_LOOP);
}

void audio_stop_all(void) {
    if (!g_audio.ready) return;
    stop_matching(-1, 0, VOICE_2D);
}

// ---------------------------------------------------------------- handles (KrunkScript GAME.SOUND)

static uint32_t g_next_handle;

static AudioVoice *handle_voice(const int handle) {
    if (!g_audio.ready || handle <= 0) return NULL;
    for (int i = 0; i < AUDIO_MAX_VOICES; i++) {
        if (g_audio.voices[i].in_use && g_audio.voices[i].handle == (uint32_t) handle) return &g_audio.voices[i];
    }
    return NULL;
}

static PendingPlay *handle_pending(const int handle) {
    for (int i = 0; i < g_audio.pending_count; i++) {
        if (g_audio.pending[i].handle == (uint32_t) handle) return &g_audio.pending[i];
    }
    return NULL;
}

int audio_handle_play(const char *name, const float volume, const int loop, const float rate, const int positional,
                      const vec3 position, const float ref_distance, const float rolloff) {
    if (!g_audio.ready) return 0;
    if (++g_next_handle == 0 || g_next_handle > 0x7fffffffu) g_next_handle = 1;
    const VoiceKind kind = positional ? (loop ? VOICE_3D_LOOP : VOICE_3D) : VOICE_2D;
    play_request_handle(name, kind, loop, position, volume, rate > 0.0f ? rate : 1.0f, ref_distance, rolloff, g_next_handle);
    return handle_voice((int) g_next_handle) || handle_pending((int) g_next_handle) ? (int) g_next_handle : 0;
}

void audio_handle_volume(const int handle, const float volume) {
    AudioVoice *voice = handle_voice(handle);
    PendingPlay *pending = handle_pending(handle);
    if (pending) pending->volume = volume;
    if (!voice) return;
    voice->volume = volume;
    voice_apply_volume(voice);
}

void audio_handle_rate(const int handle, const float rate) {
    AudioVoice *voice = handle_voice(handle);
    PendingPlay *pending = handle_pending(handle);
    const float r = rate > 0.0f ? rate : 1.0f;
    if (pending) pending->rate = r;
    if (voice) ma_sound_set_pitch(&voice->sound, r);
}

void audio_handle_fade(const int handle, const float from, const float to, const float ms) {
    AudioVoice *voice = handle_voice(handle);
    if (!voice) return;
    const float cat = category_factor(g_audio.clips[voice->clip].category) * spatial_compensation(voice);
    voice->volume = to;
    ma_sound_set_fade_in_milliseconds(&voice->sound, clamp01(from) * cat, clamp01(to) * cat, (ma_uint64) (ms > 0.0f ? ms : 0.0f));
}

void audio_handle_mute(const int handle, const int muted) {
    AudioVoice *voice = handle_voice(handle);
    if (!voice) return;
    voice->muted = muted;
    voice_apply_volume(voice);
}

int audio_handle_resume(const int handle) {
    AudioVoice *voice = handle_voice(handle);
    if (!voice) return 0;
    if (ma_sound_at_end(&voice->sound)) ma_sound_seek_to_pcm_frame(&voice->sound, 0);
    ma_sound_start(&voice->sound);
    return 1;
}

void audio_handle_pause(const int handle) {
    AudioVoice *voice = handle_voice(handle);
    if (voice) ma_sound_stop(&voice->sound);
}

void audio_handle_stop(const int handle) {
    if (!g_audio.ready || handle <= 0) return;
    AudioVoice *voice = handle_voice(handle);
    if (voice) voice_release(voice);
    int kept = 0;
    for (int i = 0; i < g_audio.pending_count; i++) {
        if (g_audio.pending[i].handle != (uint32_t) handle) g_audio.pending[kept++] = g_audio.pending[i];
    }
    g_audio.pending_count = kept;
}

int audio_handle_active(const int handle) { return handle_voice(handle) || handle_pending(handle); }
