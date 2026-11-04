// Cinematic colour pipeline (client/include/rt.h "Cinematic colour pipeline", doc/raytracing.md "Cinematic
// preset" > "Colour pipeline"): the tone curve's exposure and the grading LUT. Everything here is idle unless
// rt_cinematic(); the Custom preset and rtMode 0 never bind the LUT.
//
// The LUT maps display colours (after the tone curve and bloom) to graded ones. It is a RT_GRADE_LUT_SIZE^3
// RGBA8 3D texture (core in OpenGL ES 3.0), rebuilt on the CPU when the map's mood (the day factor, quantised)
// changes: a few ms once per map.
#include <gl.h>
#include <rt.h>
#include <sky.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static struct {
    unsigned int lut;
    int built_day;          // quantised day factor of the LUT in the texture (-1 none)
} g_grade = {0, -1};

float rt_grade_exposure(void) {
    if (!rt_cinematic()) return 0.0f;
    float e = RT_CINE_EXPOSURE * rt_cinematic_exposure();
    if (g_render_lighting.tone_mapping == 1) e *= g_render_lighting.tone_mapping_exposure; // Linear keeps its exposure
    return e;
}

static float grade_clamp(const float v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }
static float grade_smooth(const float e0, const float e1, const float x) {
    const float t = grade_clamp((x - e0) / (e1 - e0));
    return t * t * (3.0f - 2.0f * t);
}
static float grade_lum(const float c[3]) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; }

static void grade_saturate(float c[3], const float amount) {
    const float l = grade_lum(c);
    for (int i = 0; i < 3; i++) c[i] = l + (c[i] - l) * amount;
}

// Day: a mild S-curve around the mid tones, cool shadows / warm highlights (split toning, luminance kept), and
// +12 % saturation in the mid tones (none in the blacks and whites, so skies and shadows do not go garish).
static void grade_day(const float in[3], float out[3], const float warmth) {
    for (int i = 0; i < 3; i++) {
        const float x = in[i];
        const float s = x * x * (3.0f - 2.0f * x);
        out[i] = x + (s - x) * 0.3f;
    }
    const float l = grade_lum(out);
    const float shadow = 1.0f - grade_smooth(0.0f, 0.45f, l);
    const float high = grade_smooth(0.5f, 1.0f, l);
    const float cool[3] = {0.98f, 1.0f, 1.03f}, warm[3] = {1.08f, 1.0f, 0.88f}; // warm sun, a hint of cool shade
    for (int i = 0; i < 3; i++) {
        // the cool shade fades out on maps that authored a warm sky (sky_physical_warmth): Sandstorm's shade stays neutral-warm
        const float tint = (1.0f + (cool[i] - 1.0f) * shadow * 0.7f * (1.0f - warmth)) * (1.0f + (warm[i] - 1.0f) * high * 0.7f);
        out[i] *= tint;
    }
    const float l2 = grade_lum(out);
    if (l2 > 1e-4f) for (int i = 0; i < 3; i++) out[i] *= l / l2; // split toning moves hue, not brightness
    const float mid = 1.0f - fabsf(2.0f * l - 1.0f);
    grade_saturate(out, 1.1f + 0.12f * mid); // +10 % everywhere, +22 % in the mid tones
    // a small black lift: the preset's low fill and the ACES toe crushed ~3 % of a sunny frame to near black,
    // which hides players in shade (the night grade keeps its deeper blacks on purpose)
    for (int i = 0; i < 3; i++) out[i] = 0.025f + out[i] * (1.0f - 0.025f);
}

// Night: the authored hue (no split toning), slightly deeper blacks (a toe) and saturated colours lifted further
// (neon), greys left alone.
static void grade_night(const float in[3], float out[3]) {
    for (int i = 0; i < 3; i++) out[i] = powf(in[i], 1.12f);
    float mx = out[0], mn = out[0];
    for (int i = 1; i < 3; i++) { mx = out[i] > mx ? out[i] : mx; mn = out[i] < mn ? out[i] : mn; }
    grade_saturate(out, 1.0f + 0.3f * grade_smooth(0.15f, 0.6f, mx - mn));
}

static void grade_build(const float day, const float warmth) {
    const int n = RT_GRADE_LUT_SIZE;
    unsigned char *texels = malloc((size_t) n * n * n * 4);
    if (!texels) return;
    for (int b = 0; b < n; b++) {
        for (int g = 0; g < n; g++) {
            for (int r = 0; r < n; r++) {
                const float c[3] = {(float) r / (n - 1), (float) g / (n - 1), (float) b / (n - 1)};
                float d[3], ni[3];
                grade_day(c, d, warmth);
                grade_night(c, ni);
                unsigned char *t = &texels[(((size_t) b * n + g) * n + r) * 4];
                for (int i = 0; i < 3; i++) t[i] = (unsigned char) lroundf(grade_clamp(ni[i] + (d[i] - ni[i]) * day) * 255.0f);
                t[3] = 255;
            }
        }
    }
    if (!g_grade.lut) glGenTextures(1, &g_grade.lut);
    glActiveTexture(GL_TEXTURE0 + RT_GRADE_UNIT);
    glBindTexture(GL_TEXTURE_3D, g_grade.lut);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA8, n, n, n, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glActiveTexture(GL_TEXTURE0);
    free(texels);
}

int rt_grade_bind(void) {
    if (!rt_cinematic()) return 0;
    const float day = sky_physical_day();
    const int q = (int) lroundf(grade_clamp(day) * 20.0f) // 5 % steps: a moving sun does not rebuild every frame
                  + 100 * (int) lroundf(grade_clamp(sky_physical_warmth()) * 10.0f); // and the authored warmth, 10 % steps
    if (q != g_grade.built_day || !g_grade.lut) {
        grade_build((float) (q % 100) / 20.0f, (float) (q / 100) / 10.0f);
        g_grade.built_day = q;
        fprintf(stderr, "[rt] cinematic grade: day %.2f, warmth %.1f\n", (float) (q % 100) / 20.0f, (float) (q / 100) / 10.0f);
    }
    if (!g_grade.lut) return 0;
    glActiveTexture(GL_TEXTURE0 + RT_GRADE_UNIT);
    glBindTexture(GL_TEXTURE_3D, g_grade.lut);
    glActiveTexture(GL_TEXTURE0);
    return 1;
}

void rt_grade_fini(void) {
    if (g_grade.lut) glDeleteTextures(1, &g_grade.lut);
    g_grade.lut = 0;
    g_grade.built_day = -1;
}
