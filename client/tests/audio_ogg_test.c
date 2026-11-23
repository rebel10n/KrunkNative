// audio_ogg_test: Ogg Vorbis through the client's decoder (client/src/miniaudio.c: miniaudio with stb_vorbis).
// v10.0.7's Skin Manager takes .ogg sounds (deob-10.0.7.js:127038) and the game loads them by content under
// sound.mp3 / sound_swap.mp3 / sound_inspect.mp3, so ma_decode_memory and ma_decoder_init_memory (audio.c's two
// paths: a short clip decoded once, a long one streamed) must recognise the format without a hint, as they do MP3.
//   audio_ogg_test <repo root>
#include "../src/audio_miniaudio.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures, g_checks;
#define CHECK(cond, ...) do { g_checks++; if (!(cond)) { g_failures++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static unsigned char *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *d = len > 0 ? malloc((size_t) len) : NULL;
    if (d && fread(d, 1, (size_t) len, f) != (size_t) len) {
        free(d);
        d = NULL;
    }
    fclose(f);
    *n = d ? (size_t) len : 0;
    return d;
}

int main(int argc, char **argv) {
    const char *root = argc > 1 ? argv[1] : ".";
    char path[1024];
    // tone_440.ogg: 0.3 s of a 440 Hz sine, mono, 22050 Hz (ffmpeg -f lavfi -i sine=frequency=440:duration=0.3 -c:a libvorbis)
    snprintf(path, sizeof path, "%s/client/tests/data/tone_440.ogg", root);
    size_t n = 0;
    unsigned char *ogg = slurp(path, &n);
    CHECK(ogg && n > 0, "read %s", path);
    if (!ogg) goto done;

    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 0, 0);
    ma_uint64 frames = 0;
    void *pcm = NULL;
    const ma_result r = ma_decode_memory(ogg, n, &cfg, &frames, &pcm);
    CHECK(r == MA_SUCCESS && pcm, "ma_decode_memory decodes Ogg Vorbis (%d)", (int) r);
    CHECK(cfg.channels == 1 && cfg.sampleRate == 22050, "mono 22050 Hz (%u ch, %u Hz)", cfg.channels, cfg.sampleRate);
    CHECK(frames >= 6500 && frames <= 6700, "0.3 s: about 6615 frames (%llu)", (unsigned long long) frames);
    if (pcm && frames > 0) {
        const float *s = pcm;
        double sum = 0.0;
        int crossings = 0;
        for (ma_uint64 i = 0; i < frames; i++) {
            sum += (double) s[i] * s[i];
            if (i && (s[i - 1] < 0.0f) != (s[i] < 0.0f)) crossings++;
        }
        const double rms = sqrt(sum / (double) frames), hz = crossings / 2.0 / ((double) frames / 22050.0);
        CHECK(rms > 0.05, "the tone is audible (rms %.3f)", rms);
        CHECK(fabs(hz - 440.0) < 20.0, "the tone is 440 Hz (%.1f Hz from the zero crossings)", hz);
        ma_free(pcm, NULL);
    }

    // the streamed path: a decoder over the bytes, no format given
    ma_decoder dec;
    ma_decoder_config cfg2 = ma_decoder_config_init(ma_format_f32, 0, 0);
    CHECK(ma_decoder_init_memory(ogg, n, &cfg2, &dec) == MA_SUCCESS, "ma_decoder_init_memory opens Ogg Vorbis");
    {
        float buf[512];
        ma_uint64 got = 0;
        CHECK(ma_decoder_read_pcm_frames(&dec, buf, 512, &got) == MA_SUCCESS && got == 512, "the decoder streams (%llu frames)",
              (unsigned long long) got);
        ma_decoder_uninit(&dec);
    }

    // a truncated file is refused, not crashed on
    {
        ma_uint64 f2 = 0;
        void *p2 = NULL;
        ma_decoder_config c3 = ma_decoder_config_init(ma_format_f32, 0, 0);
        const ma_result r3 = ma_decode_memory(ogg, 40, &c3, &f2, &p2);
        CHECK(r3 != MA_SUCCESS || f2 == 0, "40 bytes of Ogg decode to nothing");
        if (p2) ma_free(p2, NULL);
    }
    free(ogg);

    // MP3 still decodes (Krunker's own sounds)
    snprintf(path, sizeof path, "%s/assets/sound/tick_0.mp3", root);
    unsigned char *mp3 = slurp(path, &n);
    if (mp3) {
        ma_decoder_config c4 = ma_decoder_config_init(ma_format_f32, 0, 0);
        ma_uint64 f4 = 0;
        void *p4 = NULL;
        CHECK(ma_decode_memory(mp3, n, &c4, &f4, &p4) == MA_SUCCESS && f4 > 0, "an MP3 (tick_0) still decodes");
        if (p4) ma_free(p4, NULL);
        free(mp3);
    }
done:
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
