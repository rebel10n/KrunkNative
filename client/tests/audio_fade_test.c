// audio_fade_test: v10.0.7's changeAmbient custom soundscape and fade (client/src/sound_ambient.c over
// client/src/audio.c audio_play_fade_in / audio_fade_out).
//
// v10 (deob-10.0.7.js:227084-227090): changeAmbient(id, custom, fade) with fade = 1000 * (fade || 0) ms calls
// SOUND.fadeOut(last, lastCustom, amvVols[last] || 0.12, fade) and SOUND.play(custom ? id : "ambient_" + id,
// amvVols[id] || 0.12, true, 1, custom, undefined, {fadeIn: fade}); without a fade SOUND.stop and a plain play.
// Howler's fade (:200766-200807) is a Web Audio linearRampToValueAtTime over every sound of the Howl, and it never
// stops a sound, so the old loop keeps playing at gain 0.
//   - miniaudio's fader, which audio.c fades with, is that linear ramp: rendered through an ma_engine with no device,
//     the level at 1/4, 1/2 and 3/4 of the fade, and a fade to 0 that leaves the sound playing
//   - the changeAmbient sequence on the null backend with generated clips: fade parameters, the old loop left
//     silent and looping, a second voice when the same soundscape comes back, the category volume ending a fade,
//     a custom soundscape as user-assets/<id>/sound.mp3 at assetVolume, a fade queued behind a load, the custom flag
//     not compared, ambient_3 never playing
//   audio_fade_test  (writes audio_fade_test.assets/ in the working directory; 77 without a null backend)
#include <audio.h>
#include <sound_ambient.h>
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

static int near(const float a, const float b) { return fabsf(a - b) < 1e-4f; }

// the mean absolute level of `frames` stereo frames of the engine's output
static double read_level(ma_engine *engine, const int frames) {
    static float out[2 * 48000];
    ma_engine_read_pcm_frames(engine, out, (ma_uint64) frames, NULL);
    double sum = 0;
    for (int i = 0; i < frames; i++) sum += fabs(out[2 * i]) + fabs(out[2 * i + 1]);
    return sum / (2.0 * frames);
}

static void check_fader_curve(void) {
    ma_engine_config config = ma_engine_config_init();
    config.noDevice = MA_TRUE;
    config.channels = 2;
    config.sampleRate = 48000;
    ma_engine engine;
    if (ma_engine_init(&config, &engine) != MA_SUCCESS) {
        CHECK(0, "an ma_engine without a device");
        return;
    }
    static float mono[4800];
    for (int i = 0; i < 4800; i++) mono[i] = 0.5f;
    ma_audio_buffer_config bc = ma_audio_buffer_config_init(ma_format_f32, 1, 4800, mono, NULL);
    bc.sampleRate = 48000;
    ma_audio_buffer buffer;
    ma_sound sound;
    if (ma_audio_buffer_init(&bc, &buffer) != MA_SUCCESS ||
        ma_sound_init_from_data_source(&engine, &buffer, MA_SOUND_FLAG_NO_SPATIALIZATION, NULL, &sound) != MA_SUCCESS) {
        CHECK(0, "the source sound");
        ma_engine_uninit(&engine);
        return;
    }
    ma_sound_set_looping(&sound, MA_TRUE);
    ma_sound_start(&sound);
    const double full = read_level(&engine, 4800);

    // Howl.fade(0, 0.8, 1000): linearRampToValueAtTime, so gain(t) = 0.8 * t / 1000 ms. Each 480-frame window is
    // centred on its quarter, where the mean of a linear ramp is its midpoint value.
    ma_sound_set_fade_in_milliseconds(&sound, 0.0f, 0.8f, 1000);
    static const double QUARTERS[] = {0.25, 0.5, 0.75};
    int at = 0;
    for (int q = 0; q < 3; q++) {
        const int centre = (int) (QUARTERS[q] * 48000.0);
        read_level(&engine, centre - 240 - at);
        const double got = read_level(&engine, 480) / full;
        at = centre + 240;
        CHECK(fabs(got - 0.8 * QUARTERS[q]) < 0.005, "a 1 s fade 0 -> 0.8 at %.0f%%: gain %.4f, linear %.4f", 100.0 * QUARTERS[q],
              got, 0.8 * QUARTERS[q]);
    }
    read_level(&engine, 48000 - at);
    CHECK(fabs(read_level(&engine, 4800) / full - 0.8) < 0.005, "the fade holds 0.8 at its end");

    // SOUND.fadeOut: fade(0.12, 0, 500); the sound is still playing at gain 0 afterwards
    ma_sound_set_fade_in_milliseconds(&sound, 0.12f, 0.0f, 500);
    const double first = read_level(&engine, 480) / full;
    read_level(&engine, 24000);
    const double after = read_level(&engine, 4800) / full;
    CHECK(fabs(first - 0.12 * (1.0 - 240.0 / 24000.0)) < 0.002, "a fade out starts at its from gain 0.12 (%.4f)", first);
    CHECK(after < 1e-6 && ma_sound_is_playing(&sound), "after the fade out: silent (%.6f) and still playing", after);

    ma_sound_uninit(&sound);
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

static void tick(void) {
    const vec3 listener = {0, 0, 0};
    audio_update(listener, 0.0f, 0.0f); // loads queued clips and starts their voices
}

static int voices(const char *name) {
    int n = 0;
    while (audio_voice_fade(name, n, NULL)) n++;
    return n;
}

static AudioVoiceFade voice(const char *name, const int index) {
    AudioVoiceFade v;
    memset(&v, 0, sizeof(v));
    v.from = v.to = v.ms = v.gain = -1.0f;
    audio_voice_fade(name, index, &v);
    return v;
}

static int check_change_ambient(void) {
    mkdir("audio_fade_test.assets", 0755);
    mkdir("audio_fade_test.assets/sound", 0755);
    mkdir("audio_fade_test.assets/user-assets", 0755);
    mkdir("audio_fade_test.assets/user-assets/4242", 0755);
    mkdir("audio_fade_test.assets/user-assets/7777", 0755);
    static const char *const FILES[] = {"sound/ambient_3.mp3", "sound/ambient_5.mp3", "sound/ambient_6.mp3",
                                        "user-assets/4242/sound.mp3", "user-assets/7777/sound.mp3"};
    for (size_t i = 0; i < sizeof(FILES) / sizeof(FILES[0]); i++) {
        char path[256];
        snprintf(path, sizeof(path), "audio_fade_test.assets/%s", FILES[i]);
        CHECK(write_tone(path), "the test clip %s", FILES[i]);
    }
    setenv("KRUNKNATIVE_ASSETS_DIR", "audio_fade_test.assets", 1);
    setenv("KRUNKNATIVE_AUDIO_BACKEND", "null", 1);
    if (!audio_init()) {
        printf("audio_fade_test: no null audio backend in this build\n");
        return 0;
    }

    // the game start's soundscape: SOUND.play("ambient_5", 0.12, true) at ambientVolume
    sound_events_change_ambient(5, 0, 0.0f);
    tick();
    AudioVoiceFade a = voice("ambient_5", 0);
    CHECK(voices("ambient_5") == 1 && !a.faded && a.playing && a.looping && near(a.gain, 0.12f), "ambient_5 loops at 0.12 (%g)", a.gain);
    audio_set_volume(AUDIO_CAT_AMBIENT, 0.5f);
    CHECK(near(voice("ambient_5", 0).gain, 0.06f), "at ambientVolume 0.5: 0.06 (%g)", voice("ambient_5", 0).gain);

    // camb [4242, true, 1.5]: ambient_5 fades out from amvVols[5] || 0.12 (not its 0.06) to 0 over 1500 ms and keeps
    // looping silent; user asset 4242 fades in from 0 to 0.12 x assetVolume over the same 1500 ms
    sound_events_change_ambient(4242, 1, 1500.0f);
    tick();
    a = voice("ambient_5", 0);
    CHECK(a.faded && near(a.from, 0.12f) && near(a.to, 0.0f) && near(a.ms, 1500.0f) && !a.playing && a.looping,
          "the old loop: fade 0.12 -> 0 over 1500 ms, still looping, not playing (%d %g %g %g %d %d)", a.faded, a.from, a.to, a.ms,
          a.playing, a.looping);
    AudioVoiceFade c = voice("asset:4242", 0);
    CHECK(voices("asset:4242") == 1 && c.faded && near(c.from, 0.0f) && near(c.to, 0.12f) && near(c.ms, 1500.0f) && c.playing &&
              c.looping && near(c.gain, 0.12f),
          "custom 4242 (user-assets/4242/sound.mp3): fade 0 -> 0.12 over 1500 ms (%d %g %g %g gain %g)", c.faded, c.from, c.to, c.ms,
          c.gain);

    // camb [5, 0, 1]: the asset fades out; ambient_5's faded loop does not count as playing, so a second voice starts
    // and fade(0, 0.06, 1000) covers both (a Howl's fade without an id)
    sound_events_change_ambient(5, 0, 1000.0f);
    tick();
    c = voice("asset:4242", 0);
    CHECK(c.faded && near(c.from, 0.12f) && near(c.to, 0.0f) && near(c.ms, 1000.0f) && !c.playing, "the asset fades out 0.12 -> 0");
    CHECK(voices("ambient_5") == 2, "ambient_5 again: a second voice beside the silent one (%d)", voices("ambient_5"));
    for (int i = 0; i < 2; i++) {
        a = voice("ambient_5", i);
        CHECK(a.faded && near(a.from, 0.0f) && near(a.to, 0.06f) && near(a.ms, 1000.0f), "ambient_5 voice %d: fade 0 -> 0.06 over 1000 ms "
              "(%g %g %g)", i, a.from, a.to, a.ms);
    }
    // the same id again, custom or not: lastAmbS is unchanged, nothing happens
    sound_events_change_ambient(5, 1, 500.0f);
    tick();
    CHECK(voices("ambient_5") == 2 && voices("asset:5") == 0 && near(voice("ambient_5", 1).ms, 1000.0f), "custom 5 after ambient_5: no change");

    // SOUND.setVolume -> Howl.volume(vol x category) ends every fade of the category: both ambient_5 voices at 0.12
    audio_set_volume(AUDIO_CAT_AMBIENT, 1.0f);
    CHECK(!voice("ambient_5", 0).faded && !voice("ambient_5", 1).faded && near(voice("ambient_5", 0).gain, 0.12f) &&
              near(voice("ambient_5", 1).gain, 0.12f),
          "ambientVolume 1: the fades end, both voices at 0.12 (%g %g)", voice("ambient_5", 0).gain, voice("ambient_5", 1).gain);
    CHECK(voice("asset:4242", 0).faded, "the asset's fade is not the ambient slider's");

    // without a fade: SOUND.stop of every ambient_5 voice; play() never plays ambient_3 (deob-10.0.7.js:200768)
    sound_events_change_ambient(3, 0, 0.0f);
    tick();
    CHECK(voices("ambient_5") == 0 && voices("ambient_3") == 0, "ambient_3: ambient_5 stopped, ambient_3 silent");

    // a fade out before the clip loaded: Howler queues it behind the play, so asset 7777 starts already fading out
    sound_events_change_ambient(7777, 1, 0.0f); // queued: decoded on the next audio_update
    sound_events_change_ambient(6, 0, 800.0f);
    tick();
    c = voice("asset:7777", 0);
    a = voice("ambient_6", 0);
    CHECK(voices("asset:7777") == 1 && c.faded && near(c.from, 0.12f) && near(c.to, 0.0f) && near(c.ms, 800.0f) && !c.playing,
          "asset 7777 fades out once loaded (%d %g %g %g)", c.faded, c.from, c.to, c.ms);
    CHECK(a.faded && near(a.from, 0.0f) && near(a.to, 0.3f) && near(a.ms, 800.0f), "ambient_6 fades in to amvVols[6] 0.3 (%g)", a.to);

    // NaN never equals lastAmbS; nothing plays under it
    sound_events_change_ambient(SOUND_AMBIENT_NAN, 0, 0.0f);
    sound_events_change_ambient(SOUND_AMBIENT_NAN, 1, 0.0f);
    tick();
    CHECK(voices("ambient_6") == 0, "NaN stops ambient_6");
    audio_fini();
    return 1;
}

int main(void) {
    check_fader_curve();
    const int ran = check_change_ambient();
    printf("audio_fade_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : ran ? 0 : 77;
}
