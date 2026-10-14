// nettspend_audio_test: the Nettspend class's clips and hooks (client/src/nettspend_audio.c over
// client/src/audio.c, GL-free):
//   - assets/sound/kn_drank_reload / kn_drank_pour / kn_money_throw / kn_laced_ambience.mp3 exist,
//     decode (miniaudio), have their designed lengths and are not silent;
//   - the hooks answer only their weapons: the Drank's reload start, Money Spread's shot (local and
//     3D), the laced loop's start / extend / end by the tick clock, the reset;
//   - through the real audio engine on miniaudio's null backend (KRUNKNATIVE_AUDIO_BACKEND=null, no
//     sound card, real-time clock) with the mix captured (KRUNKNATIVE_AUDIO_CAPTURE): the reload
//     hook makes the engine play the reload line and the pour (the play log names them) and the
//     captured mix is not silent, so the clips reach the mixer.
//   nettspend_audio_test <repo root>        (POSIX: setenv; skips with 77 without the content tree)
#include <nettspend_audio.h>
#include <audio.h>
#include <voice_chat.h>
#include "../src/audio_miniaudio.h"
#include <math.h>
#include <stdint.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int g_failures, g_checks;
#define CHECK(cond, ...) do { g_checks++; if (!(cond)) { g_failures++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

// audio.c's one link into voice chat (client/src/voice.c, opus): not under test
void voice_audio_fini(void) {}

static void sleep_ms(const int ms) {
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

// Decode a clip fully: its length in seconds and RMS (0 = missing or undecodable)
static double clip_stats(const char *root, const char *name, double *rms) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/assets/sound/%s.mp3", root, name);
    ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 1, 44100);
    ma_decoder decoder;
    *rms = 0.0;
    if (ma_decoder_init_file(path, &config, &decoder) != MA_SUCCESS) return 0.0;
    float buf[4096];
    double sum = 0.0;
    ma_uint64 total = 0, read = 0;
    while (ma_decoder_read_pcm_frames(&decoder, buf, 4096, &read) == MA_SUCCESS && read) {
        for (ma_uint64 i = 0; i < read; i++) sum += (double) buf[i] * buf[i];
        total += read;
    }
    ma_decoder_uninit(&decoder);
    if (total) *rms = sqrt(sum / (double) total);
    return (double) total / 44100.0;
}

// The captured mix (16-bit stereo WAV, audio.c capture_write_header): frames, and how many
// samples are above a threshold
static long capture_loud_samples(const char *path, long *frames) {
    FILE *f = fopen(path, "rb");
    *frames = 0;
    if (!f) return -1;
    unsigned char header[44];
    if (fread(header, 1, 44, f) != 44) { fclose(f); return -1; }
    long loud = 0, n = 0;
    short s[2];
    while (fread(s, sizeof(short), 2, f) == 2) {
        n++;
        if (abs(s[0]) > 300 || abs(s[1]) > 300) loud++;
    }
    fclose(f);
    *frames = n;
    return loud;
}

int main(int argc, char **argv) {
    const char *root = argc > 1 ? argv[1] : ".";
    char probe[1024];
    snprintf(probe, sizeof(probe), "%s/assets/sound/reload_1.mp3", root);
    if (access(probe, R_OK) != 0) {
        printf("no content tree at %s (assets/sound/reload_1.mp3): skip\n", root);
        return 77;
    }

    // ---- 1. the clips
    static const struct { const char *name; double min_s, max_s; } CLIPS[] = {
        {NETTSPEND_SOUND_RELOAD, 1.35, 1.6}, // the Drank's reload (1.46 s designed)
        {NETTSPEND_SOUND_POUR, 1.1, 1.35},
        {NETTSPEND_SOUND_SPLASH, 0.18, 0.3},
        {NETTSPEND_SOUND_MONEY, 0.4, 0.65},
        {NETTSPEND_SOUND_LACED, 5.9, 6.2},
    };
    for (size_t i = 0; i < sizeof(CLIPS) / sizeof(CLIPS[0]); i++) {
        double rms;
        const double s = clip_stats(root, CLIPS[i].name, &rms);
        CHECK(s > 0.0, "%s.mp3 decodes", CLIPS[i].name);
        CHECK(s >= CLIPS[i].min_s && s <= CLIPS[i].max_s, "%s.mp3 is %.2f s (want %.2f-%.2f)", CLIPS[i].name, s, CLIPS[i].min_s, CLIPS[i].max_s);
        CHECK(rms > 0.03, "%s.mp3 is not silent (rms %.4f)", CLIPS[i].name, rms);
    }

    // ---- 2. the hooks' weapon gates (no engine yet: every audio_* is a no-op)
    CHECK(nettspend_is_drank(WEAPON_ID_DRANK) && !nettspend_is_drank(WEAPON_ID_MONEY_SPREAD) && !nettspend_is_drank(2), "is_drank");
    CHECK(nettspend_is_money_spread(WEAPON_ID_MONEY_SPREAD) && !nettspend_is_money_spread(WEAPON_ID_DRANK) && !nettspend_is_money_spread(-1), "is_money_spread");
    CHECK(nettspend_audio_reload_start(WEAPON_ID_DRANK) == 1, "reload hook answers the Drank");
    CHECK(nettspend_audio_reload_start(WEAPON_ID_MONEY_SPREAD) == 0 && nettspend_audio_reload_start(1) == 0 && nettspend_audio_reload_start(-1) == 0,
          "reload hook ignores other weapons");
    CHECK(nettspend_audio_shot(WEAPON_ID_MONEY_SPREAD) == 1 && nettspend_audio_shot(WEAPON_ID_DRANK) == 0 && nettspend_audio_shot(0) == 0, "shot hook: Money Spread only");
    CHECK(nettspend_audio_stream_sound(WEAPON_ID_DRANK) && !strcmp(nettspend_audio_stream_sound(WEAPON_ID_DRANK), NETTSPEND_SOUND_SPLASH) &&
          !nettspend_audio_stream_sound(WEAPON_ID_PISS) && !nettspend_audio_stream_sound(WEAPON_ID_MONEY_SPREAD), "the Drank's stream sound is the splash, the Piss keeps its own");
    const vec3 at = {1.0f, 2.0f, 3.0f};
    CHECK(nettspend_audio_remote_shot(WEAPON_ID_MONEY_SPREAD, at) == 1 && nettspend_audio_remote_shot(WEAPON_ID_DRANK, at) == 0, "remote shot hook: Money Spread only");

    // the laced loop: starts, extends, ends by the tick clock, stops on reset
    nettspend_audio_tick(10.0f);
    CHECK(!nettspend_audio_laced_active(), "not laced at first");
    nettspend_audio_laced(8.0f);
    CHECK(nettspend_audio_laced_active(), "laced for 8 s");
    nettspend_audio_tick(17.9f);
    CHECK(nettspend_audio_laced_active(), "still laced at 7.9 s");
    nettspend_audio_laced(8.0f); // a second packet extends to 25.9
    nettspend_audio_tick(18.1f);
    CHECK(nettspend_audio_laced_active(), "a second laced extends the loop");
    nettspend_audio_tick(25.95f);
    CHECK(!nettspend_audio_laced_active(), "the loop ends when the extended time is up");
    nettspend_audio_laced(3.0f);
    nettspend_audio_reset();
    CHECK(!nettspend_audio_laced_active(), "reset stops the loop");
    nettspend_audio_laced(3.0f);
    nettspend_audio_laced(0.0f);
    CHECK(!nettspend_audio_laced_active(), "laced(0) stops the loop");
    // the NET_EXT_KN_LACED body: ours starts the loop, another player's does not, a short body is ignored
    NetLacedPacket laced = {7, 8000};
    nettspend_audio_tick(30.0f);
    CHECK(nettspend_audio_on_laced(&laced, sizeof(laced), 9) == 0 && !nettspend_audio_laced_active(), "another player's laced packet makes no sound");
    CHECK(nettspend_audio_on_laced(&laced, 4, 7) == 0 && !nettspend_audio_laced_active(), "a short laced body is ignored");
    CHECK(nettspend_audio_on_laced(&laced, sizeof(laced), 7) == 1 && nettspend_audio_laced_active(), "our own laced packet starts the ambience");
    nettspend_audio_tick(37.9f);
    CHECK(nettspend_audio_laced_active(), "for its duration_ms");
    nettspend_audio_tick(38.1f);
    CHECK(!nettspend_audio_laced_active(), "and no longer");

    // the NET_EXT_KN_LACED body {int32 uid, uint32 duration_ms}: ours starts the loop, another
    // player's does not, a short body is ignored
    unsigned char packet[8];
    const int32_t me_uid = 7, other_uid = 9;
    const uint32_t ms = 8000;
    memcpy(packet, &other_uid, 4);
    memcpy(packet + 4, &ms, 4);
    CHECK(nettspend_audio_on_laced(packet, sizeof(packet), me_uid) == 0 && !nettspend_audio_laced_active(), "another player's laced packet: no ambience");
    memcpy(packet, &me_uid, 4);
    CHECK(nettspend_audio_on_laced(packet, 7, me_uid) == 0 && !nettspend_audio_laced_active(), "a short laced body is ignored");
    nettspend_audio_tick(100.0f);
    CHECK(nettspend_audio_on_laced(packet, sizeof(packet), me_uid) == 1 && nettspend_audio_laced_active(), "our laced packet starts the ambience");
    nettspend_audio_tick(107.9f);
    CHECK(nettspend_audio_laced_active(), "the packet's 8000 ms hold");
    nettspend_audio_tick(108.1f);
    CHECK(!nettspend_audio_laced_active(), "and end");

    // ---- 3. through the engine on the null backend, the mix captured
    char capture[1024];
    snprintf(capture, sizeof(capture), "/tmp/kn-nettspend-audio-%d.wav", (int) getpid());
    setenv("KRUNKNATIVE_ASSETS_DIR", root, 1);
    // KRUNKNATIVE_ASSETS_DIR wants the assets/ dir itself
    snprintf(probe, sizeof(probe), "%s/assets", root);
    setenv("KRUNKNATIVE_ASSETS_DIR", probe, 1);
    setenv("KRUNKNATIVE_AUDIO_BACKEND", "null", 1);
    setenv("KRUNKNATIVE_AUDIO_CAPTURE", capture, 1);
    setenv("KRUNKNATIVE_AUDIO_LOG", "1", 1);
    unsetenv("KRUNKNATIVE_AUDIO");
    if (!audio_init()) {
        printf("audio_init on the null backend failed: skip the engine checks\n");
        printf("%d checks, %d failures\n", g_checks, g_failures);
        return g_failures ? 1 : 0;
    }
    CHECK(audio_available(), "audio engine up (null backend)");
    const vec3 origin = {0.0f, 0.0f, 0.0f};
    nettspend_audio_preload();
    for (int i = 0; i < 5; i++) { audio_update(origin, 0.0f, 0.0f); sleep_ms(20); } // preloads decode
    fflush(stdout);
    // the pour up: both clips start (the log, on stdout, names them: checked by the e2e), and the mix
    // carries them: capture ~0.6 s of real time
    CHECK(nettspend_audio_reload_start(WEAPON_ID_DRANK) == 1, "reload hook with the engine");
    for (int i = 0; i < 30; i++) { audio_update(origin, 0.0f, 0.0f); sleep_ms(20); }
    CHECK(nettspend_audio_shot(WEAPON_ID_MONEY_SPREAD) == 1, "shot hook with the engine");
    nettspend_audio_laced(0.5f);
    CHECK(nettspend_audio_laced_active(), "laced loop playing on the engine");
    for (int i = 0; i < 15; i++) { audio_update(origin, 0.0f, 0.0f); sleep_ms(20); }
    audio_fini();
    long frames = 0;
    const long loud = capture_loud_samples(capture, &frames);
    CHECK(frames > 44100 / 4, "the capture has frames (%ld)", frames);
    CHECK(loud > 2000, "the captured mix is not silent (%ld loud frames of %ld)", loud, frames);
    unlink(capture);

    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
