// audio_distance_test: the map sound emitters' distance models (client/src/audio.c audio_play_3d_loop).
//
// Krunker v10.0.3 plays an emitter with SOUND.play3DL -> Howler pannerAttr {refDistance: dist || 25,
// rolloffFactor: roll || 1, distanceModel: ["inverse", "linear", "exponential"][distM || 0], maxDistance:
// 10000} (deob-10.0.3.js:224689, :225409-225424), i.e. a Web Audio PannerNode with one of its three models.
//   - miniaudio's attenuation models of the same names give the PannerNode's gains (the Web Audio spec formulas)
//     for Krunker's emitter values: rendered through an ma_engine with no device, the level against the
//     level at refDistance
//   - audio.c starts an emitter loop with the model it was given (any other value plays inverse), refDistance /
//     rolloff with their 25 / 1 defaults, and a second loop of the same sound plays beside the first (play3DL has
//     no isPlaying check); on the null backend with a generated clip
//   audio_distance_test  (writes audio_distance_test.assets/ in the working directory)
#include <audio.h>
#include "../src/audio_miniaudio.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int g_failures, g_checks;
#define CHECK(cond, ...) do { g_checks++; if (!(cond)) { g_failures++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

void voice_audio_fini(void) {} // audio.c's one link into voice chat (client/src/voice.c): not under test

#define MAX_DISTANCE 10000.0

// The Web Audio PannerNode distance gain (Web Audio API, "Distance Effects").
static double web_audio_gain(const int model, const double distance, const double ref, double rolloff) {
    if (model == AUDIO_DISTANCE_LINEAR) {
        rolloff = rolloff < 0 ? 0 : rolloff > 1 ? 1 : rolloff;
        const double d = distance < ref ? ref : distance > MAX_DISTANCE ? MAX_DISTANCE : distance;
        return 1.0 - rolloff * (d - ref) / (MAX_DISTANCE - ref);
    }
    const double d = distance < ref ? ref : distance;
    if (model == AUDIO_DISTANCE_EXPONENTIAL) return pow(d / ref, -rolloff);
    return ref / (ref + rolloff * (d - ref));
}

// The output level of a constant mono source straight ahead of the listener at `distance` (the gain ramps are
// over long before the measured tail).
static double rendered_level(ma_engine *engine, ma_sound *sound, const double distance) {
    ma_sound_set_position(sound, 0.0f, 0.0f, (float) -distance);
    static float out[2 * 9600];
    ma_engine_read_pcm_frames(engine, out, 9600, NULL);
    double sum = 0;
    for (int i = 8600; i < 9600; i++) sum += fabs(out[2 * i]) + fabs(out[2 * i + 1]);
    return sum / 1000.0;
}

static void check_models(void) {
    ma_engine_config config = ma_engine_config_init();
    config.noDevice = MA_TRUE;
    config.channels = 2;
    config.sampleRate = 48000;
    config.listenerCount = 1;
    ma_engine engine;
    CHECK(ma_engine_init(&config, &engine) == MA_SUCCESS, "an ma_engine without a device");
    static float mono[4800];
    for (int i = 0; i < 4800; i++) mono[i] = 0.5f;
    ma_audio_buffer_config bc = ma_audio_buffer_config_init(ma_format_f32, 1, 4800, mono, NULL);
    bc.sampleRate = 48000;
    ma_audio_buffer buffer;
    CHECK(ma_audio_buffer_init(&bc, &buffer) == MA_SUCCESS, "the source buffer");

    // Krunker's emitter shapes: the defaults (25 / 1), Krunk Plaza's two (ed 2, ero 0.36, edm 1; ed 2, ero 0.55,
    // edm 2: assets/maps/official/20_krunk_plaza.json) and Facility's (ed 5000, edm 2, ero 0 -> 1)
    static const struct {
        int model;
        double ref, rolloff, distances[3];
    } CASES[] = {
        {AUDIO_DISTANCE_INVERSE, 25, 1, {50, 200, 1000}},
        {AUDIO_DISTANCE_LINEAR, 25, 1, {50, 2000, 9000}},
        {AUDIO_DISTANCE_LINEAR, 2, 0.36, {10, 3000, 8000}},
        {AUDIO_DISTANCE_EXPONENTIAL, 2, 0.55, {4, 40, 400}},
        {AUDIO_DISTANCE_EXPONENTIAL, 5000, 1, {6000, 8000, 9500}},
    };
    static const ma_attenuation_model MODELS[] = {ma_attenuation_model_inverse, ma_attenuation_model_linear, ma_attenuation_model_exponential};
    for (size_t c = 0; c < sizeof(CASES) / sizeof(CASES[0]); c++) {
        ma_sound sound;
        if (ma_sound_init_from_data_source(&engine, &buffer, 0, NULL, &sound) != MA_SUCCESS) {
            CHECK(0, "a sound for case %zu", c);
            continue;
        }
        ma_sound_set_looping(&sound, MA_TRUE);
        ma_sound_set_attenuation_model(&sound, MODELS[CASES[c].model]);
        ma_sound_set_min_distance(&sound, (float) CASES[c].ref);
        ma_sound_set_max_distance(&sound, (float) MAX_DISTANCE);
        ma_sound_set_rolloff(&sound, (float) CASES[c].rolloff);
        ma_sound_set_doppler_factor(&sound, 0.0f);
        ma_audio_buffer_seek_to_pcm_frame(&buffer, 0);
        ma_sound_start(&sound);
        const double at_ref = rendered_level(&engine, &sound, CASES[c].ref); // gain 1 in every model
        for (int k = 0; k < 3; k++) {
            const double d = CASES[c].distances[k];
            const double got = rendered_level(&engine, &sound, d) / at_ref;
            const double want = web_audio_gain(CASES[c].model, d, CASES[c].ref, CASES[c].rolloff);
            CHECK(at_ref > 0.1 && fabs(got - want) <= 0.01 * want + 1e-4, "model %d ref %g rolloff %g at %g: %.5f, Web Audio %.5f",
                  CASES[c].model, CASES[c].ref, CASES[c].rolloff, d, got, want);
        }
        ma_sound_uninit(&sound);
    }
    ma_audio_buffer_uninit(&buffer);
    ma_engine_uninit(&engine);
}

// A 16-bit mono WAV of a constant tone (audio.c decodes a clip by its content, whatever the extension).
static int write_tone(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    const uint32_t frames = 22050, rate = 22050, bytes = frames * 2;
    const uint8_t header[44] = {'R', 'I', 'F', 'F', (uint8_t) (36 + bytes), (uint8_t) ((36 + bytes) >> 8), (uint8_t) ((36 + bytes) >> 16), 0,
                                'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 1, 0,
                                (uint8_t) rate, (uint8_t) (rate >> 8), 0, 0, (uint8_t) (rate * 2), (uint8_t) ((rate * 2) >> 8), (uint8_t) ((rate * 2) >> 16), 0,
                                2, 0, 16, 0, 'd', 'a', 't', 'a', (uint8_t) bytes, (uint8_t) (bytes >> 8), (uint8_t) (bytes >> 16), 0};
    fwrite(header, 1, sizeof(header), f);
    for (uint32_t i = 0; i < frames; i++) {
        const int16_t v = (int16_t) (8000.0 * sin(2.0 * M_PI * 440.0 * i / rate));
        fwrite(&v, 2, 1, f);
    }
    return fclose(f) == 0;
}

static void check_audio_c(void) {
    mkdir("audio_distance_test.assets", 0755);
    mkdir("audio_distance_test.assets/sound", 0755);
    CHECK(write_tone("audio_distance_test.assets/sound/kn_distance_tone.mp3"), "the test clip");
    setenv("KRUNKNATIVE_ASSETS_DIR", "audio_distance_test.assets", 1);
    setenv("KRUNKNATIVE_AUDIO_BACKEND", "null", 1);
    if (!audio_init()) {
        printf("audio_distance_test: no null audio backend in this build, skipping the audio.c part\n");
        return;
    }
    const vec3 listener = {0, 0, 0}, at = {0, 0, -50};
    int model = -1;
    float ref = 0, rolloff = 0;

    audio_play_3d_loop("kn_distance_tone", at, 1.0f, 1.0f, 2.0f, 0.36f, AUDIO_DISTANCE_LINEAR); // Krunk Plaza's first emitter
    audio_update(listener, 0.0f, 0.0f); // loads the clip and starts the voice
    CHECK(audio_voice_distance("kn_distance_tone", &model, &ref, &rolloff) && model == AUDIO_DISTANCE_LINEAR && ref == 2.0f &&
              fabsf(rolloff - 0.36f) < 1e-6f,
          "an edm 1 emitter plays linear, ref 2, rolloff 0.36 (%d %g %g)", model, ref, rolloff);

    audio_play_3d_loop("kn_distance_tone", at, 1.0f, 1.0f, 0.0f, 0.0f, AUDIO_DISTANCE_EXPONENTIAL); // a second emitter of the clip
    audio_update(listener, 0.0f, 0.0f);
    CHECK(audio_voice_distance("kn_distance_tone", &model, &ref, &rolloff) && model == AUDIO_DISTANCE_EXPONENTIAL && ref == 25.0f &&
              rolloff == 1.0f,
          "a second loop of the same sound plays (play3DL), exponential, dist / roll 0 -> 25 / 1 (%d %g %g)", model, ref, rolloff);

    audio_play_3d_loop("kn_distance_tone", at, 1.0f, 1.0f, 5.0f, 0.5f, 7);
    audio_update(listener, 0.0f, 0.0f);
    CHECK(audio_voice_distance("kn_distance_tone", &model, &ref, &rolloff) && model == AUDIO_DISTANCE_INVERSE && ref == 5.0f,
          "an unknown model plays inverse (%d)", model);

    audio_stop_3d_loops();
    CHECK(!audio_voice_distance("kn_distance_tone", NULL, NULL, NULL), "audio_stop_3d_loops ends them");
    audio_fini();
}

int main(void) {
    check_models();
    check_audio_c();
    printf("audio_distance_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
