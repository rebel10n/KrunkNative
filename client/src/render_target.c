// Scaled scene render target: Krunker v3.1.1's `resolution` (55.js setResMlt:
// renderer.setPixelRatio(devicePixelRatio * v) with the canvas kept at window size),
// `antiAlias` (WebGLRenderer({antialias})), `aspectRatio` (55.js getRes(): camera aspect
// from "WxH", the canvas still fills the window) and the in-game `saturationn`/`hue`
// CSS filters (index.js bundleFilters(0) on the 3D canvas).
//
// The world and first-person passes draw into an offscreen framebuffer of
//   height = framebuffer height * resolution, width = height * aspect
// (aspect = aspectRatio W/H, else the window's), multisampled when antiAlias is on.
// render_target_end() resolves the MSAA buffer and draws it over the whole default
// framebuffer with a fullscreen triangle: GL_LINEAR sampling (the browser's bilinear
// canvas scaling) and the CSS saturate()/hue-rotate() colour matrices. The HUD, the
// overlay, the class preview and the HTML page draw afterwards at native resolution.
//
// Without any of those settings (resolution 1, no AA, no aspect override, no filter)
// begin() returns 0 and the scene renders straight into the default framebuffer.
//
// GL state: begin() binds its framebuffer + viewport and clears colour/depth; end()
// restores the previous draw framebuffer and the full viewport, leaves blending off,
// depth test on (as scene_render leaves it) and records the program/texture it bound
// in g_active_shader. The scene texture is bound to unit 0 only for the composite draw and
// unbound right after it (unit 0 empty, g_active_texture 0), and begin() releases it from
// every unit before binding its framebuffer: a sampler of the scene pass (an untextured sky's
// `tex`) must never see its own target (WebGL / ANGLE "feedback loop", gl_feedback_check.h).
//
// Nettspend "laced" (client/include/nettspend.h): while nettspend_laced_intensity() > 0 the scene
// goes through the target too and end() draws it with assets/shaders/laced.frag (the same
// fullscreen triangle, the same saturate / hue step) instead of the plain composite.
//
// TAA (Box Office-only `rtTaa`, render_settings.h): the scene goes through the target too. begin()
// sets the frame's sub-pixel camera jitter (camera_set_jitter: Halton 2/3, 8 samples, in NDC so the RT
// prepass and the world / first-person draws share it); end() resolves the scene into a history
// (assets/shaders/rt_taa.frag: reprojection with the ray tracer's motion vectors from rt_taa_velocity(),
// 3x3 neighbourhood clamp, 1/10 blend, light sharpen) and composites the resolve instead of the raw
// scene, then clears the jitter so the HUD / previews / page draw unjittered. The history restarts on
// a size change, on rt_history_epoch() (map change) and when no velocity was produced this frame.
// The viewmodel has no velocity: it is reprojected with the camera alone and held by the clamp.
//
// Bloom (Box Office-only `rtBloom` with rtMode >= 1, doc/raytracing.md "Bloom"): the scene target (and the MSAA
// buffer and the TAA history) become RGBA16F. While render_target_bloom_active() is 1, basic.c makes opaque
// basic.frag fragments write alpha = 1 + their bloom weight (radiance above 1 before the tone curve, plus
// emissive glow); additive draws may leave rgb above 1. end(), after the TAA resolve so the glow does not
// shimmer with the jitter, runs assets/shaders/rt_bloom.frag over a BLOOM_LEVELS mip chain from half
// resolution down (13-tap Karis-averaged prefilter / downsample, 3x3 tent upsample added level onto level) and
// the composite adds level 0 * BLOOM_STRENGTH to the clamped scene colour. The laced pass ignores bloom.
// Without rtBloom nothing here changes: RGBA8 targets, bloom_strength 0 (the composite reads as before).
//
// Sharpening (Box Office-only `rtSharpen` 1..3 with rtMode >= 1, doc/raytracing.md "Upscaling and sharpening"):
// instead of the composite's bilinear stretch, end() runs assets/shaders/rt_upscale.frag (an EASU-style
// edge-adaptive 12-tap upscale, written from AMD FSR 1's published description) into a window-size RGBA8
// target, then assets/shaders/rt_sharpen.frag (RCAS-style contrast-adaptive sharpening) onto the window, which
// also adds the bloom and applies the saturationn / hue filter like the composite. When the scene is not
// scaled (resolution 1, no aspect override) the upscale is skipped and only the sharpening runs, and only with
// TAA on (TAA softens). The laced pass keeps its own path. Without rtSharpen nothing here changes.
#include <gl.h>
#include "klog.h"
#include <GLFW/glfw3.h>
#include <nettspend.h>
#include <render_settings.h>
#include <rt.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define RT_SCENE_TEXTURE_UNIT 0 // composite shader sampler `scene`
#define RT_MAX_SAMPLES 4

static struct {
    int initialized;
    unsigned int program, vao;
    int loc_scene, loc_filtered, loc_saturate, loc_hue;
    // the laced pass (nettspend.h): compiled on first use, 0 after a failure (plain composite then)
    int laced_initialized;
    unsigned int laced_program;
    int laced_loc_scene, laced_loc_time, laced_loc_intensity, laced_loc_filtered, laced_loc_saturate, laced_loc_hue;
    int max_size, max_samples;                           // GL_MAX_RENDERBUFFER_SIZE, GL_MAX_SAMPLES

    unsigned int fbo, color_tex, depth_rb;              // resolve / single-sample target
    unsigned int msaa_fbo, msaa_color_rb, msaa_depth_rb; // antiAlias target
    int width, height, samples;                          // allocated size
    int active;                                          // between begin() and end()
    int prev_draw_fbo;
    int fb_width, fb_height;                             // default framebuffer size at begin()
    int last_width, last_height;                         // render_target_size()

    // TAA (rtTaa): the resolve program and the two history targets at scene size
    int taa_initialized;
    unsigned int taa_program;
    int taa_loc_scene, taa_loc_history, taa_loc_velocity, taa_loc_texel, taa_loc_sky, taa_loc_valid, taa_loc_blend, taa_loc_sharpen;
    unsigned int taa_fbo[2], taa_tex[2];
    unsigned int taa_depth_fbo, taa_depth_tex;           // the scene depth, copied for rt_taa.frag's viewmodel test
    int taa_loc_depth, taa_loc_depth_valid;
    int taa_width, taa_height, taa_index, taa_valid;
    unsigned int taa_epoch;                              // rt_history_epoch() the history was built under
    int taa_jittered;                                    // begin() set a jitter this frame
    unsigned int taa_frame;                              // the jitter sequence position
    int taa_hdr;                                         // the history is RGBA16F (bloom on)
    int taa_loc_keep_alpha;

    // bloom (rtBloom)
    int hdr;                                             // the allocated scene target is RGBA16F
    int bloom;                                           // this frame: begin() enabled bloom (end() runs the chain)
    int bloom_failed;                                    // a half-float target was refused once: bloom stays off
    int bloom_initialized;
    unsigned int bloom_program;
    int bloom_loc_source, bloom_loc_texel, bloom_loc_mode, bloom_loc_radius;
    int loc_bloom, loc_bloom_strength;                   // the composite's
    unsigned int bloom_tex[8], bloom_fbo[8];
    int bloom_w[8], bloom_h[8], bloom_levels;

    // sharpening (rtSharpen): the upscale and sharpen programs and the window-size intermediate
    int sharp_initialized;
    unsigned int upscale_program, sharpen_program;
    int up_loc_scene, up_loc_input, up_loc_output;
    int sh_loc_source, sh_loc_sharpness, sh_loc_bloom, sh_loc_bloom_strength, sh_loc_output, sh_loc_filtered, sh_loc_saturate, sh_loc_hue;
    unsigned int up_fbo, up_tex;
    int up_width, up_height;
} rt;

static int rt_complete(unsigned int fbo);

// ---------------------------------------------------------------- per-step post timing (dev)
// KRUNKNATIVE_POST_TIMING=1 (desktop GL only: GL_TIMESTAMP is not in ES 3.0): GL timestamp queries around each
// step of render_target_end(), averaged and printed once a second as `[post] msaa .. taa .. bloom .. up .. final ..`
// (ms). Results are read POST_TIMING_LAG frames late so nothing stalls. Without the variable every call returns.
#ifndef KN_GLES
#define POST_STEPS 6 // entry, msaa, taa, bloom, upscale, final
#define POST_TIMING_LAG 4
static const char *const POST_STEP_NAMES[POST_STEPS - 1] = {"msaa", "taa", "bloom", "up", "final"};
static struct {
    int state; // 0 unknown, 1 on, -1 off
    unsigned int query[POST_TIMING_LAG][POST_STEPS];
    int used[POST_TIMING_LAG];
    int slot;
    double sum[POST_STEPS - 1];
    int frames;
    double last_print;
} g_post_timing;

static int post_timing_on(void) {
    if (!g_post_timing.state) {
        const char *v = getenv("KRUNKNATIVE_POST_TIMING");
        g_post_timing.state = v && *v && *v != '0' ? 1 : -1;
        if (g_post_timing.state > 0) glGenQueries(POST_TIMING_LAG * POST_STEPS, &g_post_timing.query[0][0]);
    }
    return g_post_timing.state > 0;
}

// Marks the end of step `step` (1..5) for this frame.
static void post_timing_mark(const int step) {
    if (!post_timing_on()) return;
    glQueryCounter(g_post_timing.query[g_post_timing.slot][step], GL_TIMESTAMP);
}

// Called at end() entry: reads the oldest slot back, then starts this frame's slot.
static void post_timing_begin(void) {
    if (!post_timing_on()) return;
    g_post_timing.slot = (g_post_timing.slot + 1) % POST_TIMING_LAG;
    const int s = g_post_timing.slot;
    if (g_post_timing.used[s]) {
        GLuint64 t[POST_STEPS];
        for (int i = 0; i < POST_STEPS; i++) glGetQueryObjectui64v(g_post_timing.query[s][i], GL_QUERY_RESULT, &t[i]);
        for (int i = 1; i < POST_STEPS; i++) g_post_timing.sum[i - 1] += (double) (t[i] - t[i - 1]) / 1e6;
        g_post_timing.frames++;
    }
    g_post_timing.used[s] = 1;
    const double now = glfwGetTime();
    if (now - g_post_timing.last_print >= 1.0 && g_post_timing.frames > 0) {
        fprintf(stderr, "[post]");
        for (int i = 0; i < POST_STEPS - 1; i++) {
            fprintf(stderr, " %s %.3f", POST_STEP_NAMES[i], g_post_timing.sum[i] / g_post_timing.frames);
            g_post_timing.sum[i] = 0.0;
        }
        fprintf(stderr, " (%d frames)\n", g_post_timing.frames);
        g_post_timing.frames = 0;
        g_post_timing.last_print = now;
    }
    glQueryCounter(g_post_timing.query[s][0], GL_TIMESTAMP);
}
#else
static void post_timing_mark(const int step) { (void) step; }
static void post_timing_begin(void) {}
#endif

#define TAA_SAMPLES 8
#define TAA_BLEND 0.1f
#define TAA_SHARPEN 0.15f
#define TAA_HISTORY_UNIT 1
#define TAA_VELOCITY_UNIT 2
#define TAA_DEPTH_UNIT 3

#define BLOOM_LEVELS 6          // half .. 1/64 resolution
#define BLOOM_STRENGTH 0.5f     // level 0's weight in the composite (tuned on Kanji's neon and Sandstorm's sun, see the doc)
#define BLOOM_RADIUS 1.0f       // the upsample tent's reach in texels of the smaller level

// Cinematic (rt.h "Cinematic colour pipeline"): a stronger, wider bloom with a display-luminance knee, and the
// grade (LUT + vignette) uniforms of the final composite program. grade_uniforms returns 1 when the LUT is bound.
static float bloom_strength_now(void) { return rt_cinematic() ? RT_CINE_BLOOM_STRENGTH : BLOOM_STRENGTH; }
static int grade_uniforms(const unsigned int program, const int out_w, const int out_h) {
    const int on = rt_grade_bind();
    glUniform1i(glGetUniformLocation(program, "grade_on"), on);
    // Always give the sampler3D its own unit, even with the grade off: left at its default (unit 0) it shares the
    // 2D `scene` sampler's unit, which ES 3.0 / WebGL / ANGLE reject as a draw error (two sampler types on one
    // unit), so the composite drew nothing there and the window showed only the clear colour.
    glUniform1i(glGetUniformLocation(program, "grade_lut"), RT_GRADE_UNIT);
    if (!on) return 0;
    glUniform1f(glGetUniformLocation(program, "grade_lut_size"), (float) RT_GRADE_LUT_SIZE);
    glUniform1f(glGetUniformLocation(program, "grade_vignette"), RT_GRADE_VIGNETTE);
    glUniform1f(glGetUniformLocation(program, "grade_aspect"), out_h > 0 ? (float) out_w / (float) out_h : 1.0f);
    return 1;
}
static void grade_unbind(const int on) {
    if (!on) return;
    glActiveTexture(GL_TEXTURE0 + RT_GRADE_UNIT);
    glBindTexture(GL_TEXTURE_3D, 0);
    glActiveTexture(GL_TEXTURE0 + RT_SCENE_TEXTURE_UNIT);
}
#define BLOOM_UNIT 1            // the composite's `bloom` sampler

// rtSharpen 1 / 2 / 3: RCAS sharpness in stops (sharpness = 2^-stops; 0 stops = the strongest lobe)
static const float SHARPEN_STOPS[4] = {0.0f, 1.0f, 0.5f, 0.2f};

static void sharp_delete_target(void) {
    if (rt.up_fbo) glDeleteFramebuffers(1, &rt.up_fbo);
    if (rt.up_tex) { gl_release_texture(rt.up_tex); glDeleteTextures(1, &rt.up_tex); }
    rt.up_fbo = rt.up_tex = 0;
    rt.up_width = rt.up_height = 0;
}

static unsigned int sharp_compile(const char *frag_name) {
    char *vert = concat(assets_path(), "shaders/scene_composite.vert");
    char *frag = concat(assets_path(), frag_name);
    const unsigned int program = vert && frag ? shader_compile(vert, frag) : 0;
    free(vert);
    free(frag);
    return program;
}

static int sharp_init_programs(void) {
    if (rt.sharp_initialized) return rt.upscale_program && rt.sharpen_program;
    rt.sharp_initialized = 1;
    rt.upscale_program = sharp_compile("shaders/rt_upscale.frag");
    rt.sharpen_program = sharp_compile("shaders/rt_sharpen.frag");
    if (!rt.upscale_program || !rt.sharpen_program) {
        fprintf(stderr, "render_target: upscale / sharpen shaders unavailable, rtSharpen ignored\n");
        return 0;
    }
    rt.up_loc_scene = glGetUniformLocation(rt.upscale_program, "scene");
    rt.up_loc_input = glGetUniformLocation(rt.upscale_program, "input_size");
    rt.up_loc_output = glGetUniformLocation(rt.upscale_program, "output_size");
    rt.sh_loc_source = glGetUniformLocation(rt.sharpen_program, "source");
    rt.sh_loc_sharpness = glGetUniformLocation(rt.sharpen_program, "sharpness");
    rt.sh_loc_bloom = glGetUniformLocation(rt.sharpen_program, "bloom");
    rt.sh_loc_bloom_strength = glGetUniformLocation(rt.sharpen_program, "bloom_strength");
    rt.sh_loc_output = glGetUniformLocation(rt.sharpen_program, "output_size");
    rt.sh_loc_filtered = glGetUniformLocation(rt.sharpen_program, "filtered");
    rt.sh_loc_saturate = glGetUniformLocation(rt.sharpen_program, "saturate_matrix");
    rt.sh_loc_hue = glGetUniformLocation(rt.sharpen_program, "hue_matrix");
    return 1;
}

static int sharp_allocate(const int width, const int height) {
    if (rt.up_fbo && rt.up_width == width && rt.up_height == height) return 1;
    sharp_delete_target();
    glGenTextures(1, &rt.up_tex);
    glBindTexture(GL_TEXTURE_2D, rt.up_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    g_active_texture = 0;
    glGenFramebuffers(1, &rt.up_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, rt.up_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt.up_tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "render_target: %dx%d upscale target incomplete, rtSharpen ignored\n", width, height);
        sharp_delete_target();
        return 0;
    }
    rt.up_width = width;
    rt.up_height = height;
    return 1;
}

// The rtSharpen level that applies this frame (0 = the plain composite)
static int sharp_level(const ClientRenderSettings *s) {
    if (s->rt_sharpen <= 0 || s->rt_mode <= RT_MODE_OFF) return 0;
    return s->rt_sharpen > 3 ? 3 : s->rt_sharpen;
}

static float halton(unsigned int index, const unsigned int base) {
    float f = 1.0f, r = 0.0f;
    while (index > 0) {
        f /= (float) base;
        r += f * (float) (index % base);
        index /= base;
    }
    return r;
}

static void taa_delete_targets(void) {
    for (int i = 0; i < 2; i++) {
        if (rt.taa_fbo[i]) glDeleteFramebuffers(1, &rt.taa_fbo[i]);
        if (rt.taa_tex[i]) { gl_release_texture(rt.taa_tex[i]); glDeleteTextures(1, &rt.taa_tex[i]); }
        rt.taa_fbo[i] = rt.taa_tex[i] = 0;
    }
    if (rt.taa_depth_fbo) glDeleteFramebuffers(1, &rt.taa_depth_fbo);
    if (rt.taa_depth_tex) { gl_release_texture(rt.taa_depth_tex); glDeleteTextures(1, &rt.taa_depth_tex); }
    rt.taa_depth_fbo = rt.taa_depth_tex = 0;
    rt.taa_width = rt.taa_height = 0;
    rt.taa_valid = 0;
}

static int taa_init_program(void) {
    if (rt.taa_initialized) return rt.taa_program != 0;
    rt.taa_initialized = 1;
    char *vert = concat(assets_path(), "shaders/scene_composite.vert");
    char *frag = concat(assets_path(), "shaders/rt_taa.frag");
    if (vert && frag) rt.taa_program = shader_compile(vert, frag);
    free(vert);
    free(frag);
    if (!rt.taa_program) {
        fprintf(stderr, "render_target: TAA shader unavailable, rtTaa ignored\n");
        return 0;
    }
    rt.taa_loc_scene = glGetUniformLocation(rt.taa_program, "scene");
    rt.taa_loc_history = glGetUniformLocation(rt.taa_program, "history");
    rt.taa_loc_velocity = glGetUniformLocation(rt.taa_program, "velocity");
    rt.taa_loc_texel = glGetUniformLocation(rt.taa_program, "texel");
    rt.taa_loc_sky = glGetUniformLocation(rt.taa_program, "sky_reproject");
    rt.taa_loc_valid = glGetUniformLocation(rt.taa_program, "history_valid");
    rt.taa_loc_blend = glGetUniformLocation(rt.taa_program, "blend");
    rt.taa_loc_sharpen = glGetUniformLocation(rt.taa_program, "sharpen");
    rt.taa_loc_keep_alpha = glGetUniformLocation(rt.taa_program, "keep_alpha");
    rt.taa_loc_depth = glGetUniformLocation(rt.taa_program, "scene_depth");
    rt.taa_loc_depth_valid = glGetUniformLocation(rt.taa_program, "depth_valid");
    return 1;
}

static int taa_allocate(const int width, const int height, const int hdr) {
    if (rt.taa_fbo[0] && rt.taa_width == width && rt.taa_height == height && rt.taa_hdr == hdr) return 1;
    taa_delete_targets();
    for (int i = 0; i < 2; i++) {
        glGenTextures(1, &rt.taa_tex[i]);
        glBindTexture(GL_TEXTURE_2D, rt.taa_tex[i]);
        if (hdr) glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_HALF_FLOAT, NULL);
        else glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glGenFramebuffers(1, &rt.taa_fbo[i]);
        glBindFramebuffer(GL_FRAMEBUFFER, rt.taa_fbo[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt.taa_tex[i], 0);
        if (!rt_complete(rt.taa_fbo[i])) {
            fprintf(stderr, "render_target: %dx%d TAA history incomplete, rtTaa ignored\n", width, height);
            glBindTexture(GL_TEXTURE_2D, 0);
            taa_delete_targets();
            return 0;
        }
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    g_active_texture = 0;
    // The scene depth as a texture (the scene's own depth is a renderbuffer, MSAA or not): taa_resolve() blits it
    // here so rt_taa.frag can tell the first-person viewmodel apart (main.c clears depth before the fps scene, so
    // after the frame only viewmodel pixels have depth < 1). Same format as the scene's depth: a legal blit / resolve.
    glGenTextures(1, &rt.taa_depth_tex);
    glBindTexture(GL_TEXTURE_2D, rt.taa_depth_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, width, height, 0, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    glGenFramebuffers(1, &rt.taa_depth_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, rt.taa_depth_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, rt.taa_depth_tex, 0);
    if (!rt_complete(rt.taa_depth_fbo)) { // no viewmodel test then (rt_taa.frag treats every pixel as the world)
        glDeleteFramebuffers(1, &rt.taa_depth_fbo);
        gl_release_texture(rt.taa_depth_tex);
        glDeleteTextures(1, &rt.taa_depth_tex);
        rt.taa_depth_fbo = rt.taa_depth_tex = 0;
    }
    rt.taa_width = width;
    rt.taa_height = height;
    rt.taa_hdr = hdr;
    rt.taa_valid = 0;
    return 1;
}

// The frame's sub-pixel jitter: Halton (2, 3) in [-0.5, 0.5] pixels of the scene target, as NDC.
static void taa_set_jitter(const int width, const int height) {
    const unsigned int i = rt.taa_frame % TAA_SAMPLES + 1;
    const float jx = halton(i, 2) - 0.5f, jy = halton(i, 3) - 0.5f;
    camera_set_jitter(2.0f * jx / (float) width, 2.0f * jy / (float) height);
    rt.taa_jittered = 1;
    rt.taa_frame++;
}

// The resolve: rt.color_tex (this frame) + the previous history -> the other history; returns the
// texture the composite should draw, or 0 to draw the raw scene.
static unsigned int taa_resolve(void) {
    int vel_w, vel_h;
    float sky[16];
    const unsigned int velocity = rt_taa_velocity(&vel_w, &vel_h, sky);
    const unsigned int epoch = rt_history_epoch();
    if (!velocity || !taa_allocate(rt.width, rt.height, rt.hdr)) {
        rt.taa_valid = 0; // no motion vectors this frame (no world yet): restart when they return
        return 0;
    }
    if (rt.taa_epoch != epoch) {
        rt.taa_epoch = epoch;
        rt.taa_valid = 0;
    }
    const int write = rt.taa_index, read = rt.taa_index ^ 1;
    if (rt.taa_depth_fbo) { // this frame's depth (the MSAA one resolved): only the viewmodel's pixels are < 1
        gl_release_texture(rt.taa_depth_tex);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, rt.samples ? rt.msaa_fbo : rt.fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, rt.taa_depth_fbo);
        glBlitFramebuffer(0, 0, rt.width, rt.height, 0, 0, rt.width, rt.height, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    }
    gl_release_texture(rt.taa_tex[write]);
    glBindFramebuffer(GL_FRAMEBUFFER, rt.taa_fbo[write]);
    glViewport(0, 0, rt.width, rt.height);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glUseProgram(rt.taa_program);
    g_active_shader = rt.taa_program;
    glActiveTexture(GL_TEXTURE0 + TAA_VELOCITY_UNIT);
    glBindTexture(GL_TEXTURE_2D, velocity);
    glActiveTexture(GL_TEXTURE0 + TAA_DEPTH_UNIT);
    glBindTexture(GL_TEXTURE_2D, rt.taa_depth_tex); // 0 without the copy: depth_valid 0 then
    glActiveTexture(GL_TEXTURE0 + TAA_HISTORY_UNIT);
    glBindTexture(GL_TEXTURE_2D, rt.taa_valid ? rt.taa_tex[read] : rt.color_tex);
    glActiveTexture(GL_TEXTURE0 + RT_SCENE_TEXTURE_UNIT);
    glBindTexture(GL_TEXTURE_2D, rt.color_tex);
    g_active_texture = rt.color_tex;
    glUniform1i(rt.taa_loc_scene, RT_SCENE_TEXTURE_UNIT);
    glUniform1i(rt.taa_loc_history, TAA_HISTORY_UNIT);
    glUniform1i(rt.taa_loc_velocity, TAA_VELOCITY_UNIT);
    glUniform1i(rt.taa_loc_depth, TAA_DEPTH_UNIT);
    glUniform1i(rt.taa_loc_depth_valid, rt.taa_depth_tex != 0);
    glUniform2f(rt.taa_loc_texel, 1.0f / (float) rt.width, 1.0f / (float) rt.height);
    glUniformMatrix4fv(rt.taa_loc_sky, 1, GL_TRUE, sky);
    glUniform1i(rt.taa_loc_valid, rt.taa_valid);
    glUniform1f(rt.taa_loc_blend, TAA_BLEND);
    glUniform1f(rt.taa_loc_sharpen, TAA_SHARPEN);
    glUniform1i(rt.taa_loc_keep_alpha, rt.hdr); // bloom: the weight in alpha survives the resolve
    glBindVertexArray(rt.vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    // the history and velocity units are not left bound (the next frame renders into the history)
    glActiveTexture(GL_TEXTURE0 + TAA_VELOCITY_UNIT);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0 + TAA_DEPTH_UNIT);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0 + TAA_HISTORY_UNIT);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0 + RT_SCENE_TEXTURE_UNIT);
    rt.taa_valid = 1;
    rt.taa_index = read;
    return rt.taa_tex[write];
}

// ---------------------------------------------------------------- bloom (rtBloom)

static void bloom_delete_targets(void) {
    for (int i = 0; i < rt.bloom_levels; i++) {
        if (rt.bloom_fbo[i]) glDeleteFramebuffers(1, &rt.bloom_fbo[i]);
        if (rt.bloom_tex[i]) { gl_release_texture(rt.bloom_tex[i]); glDeleteTextures(1, &rt.bloom_tex[i]); }
        rt.bloom_fbo[i] = rt.bloom_tex[i] = 0;
    }
    rt.bloom_levels = 0;
}

static int bloom_init_program(void) {
    if (rt.bloom_initialized) return rt.bloom_program != 0;
    rt.bloom_initialized = 1;
    char *vert = concat(assets_path(), "shaders/scene_composite.vert");
    char *frag = concat(assets_path(), "shaders/rt_bloom.frag");
    if (vert && frag) rt.bloom_program = shader_compile(vert, frag);
    free(vert);
    free(frag);
    if (!rt.bloom_program) {
        fprintf(stderr, "render_target: bloom shader unavailable, rtBloom ignored\n");
        return 0;
    }
    rt.bloom_loc_source = glGetUniformLocation(rt.bloom_program, "source");
    rt.bloom_loc_texel = glGetUniformLocation(rt.bloom_program, "texel");
    rt.bloom_loc_mode = glGetUniformLocation(rt.bloom_program, "mode");
    rt.bloom_loc_radius = glGetUniformLocation(rt.bloom_program, "radius");
    return 1;
}

// The mip chain: level i is the scene size / 2^(i + 1), RGBA16F, linear filtering; stops at 8 px.
static int bloom_allocate(const int width, const int height) {
    const int w0 = width / 2 > 0 ? width / 2 : 1, h0 = height / 2 > 0 ? height / 2 : 1;
    if (rt.bloom_levels && rt.bloom_w[0] == w0 && rt.bloom_h[0] == h0) return 1;
    bloom_delete_targets();
    int w = w0, h = h0;
    for (int i = 0; i < BLOOM_LEVELS && w >= 8 && h >= 8; i++) {
        glGenTextures(1, &rt.bloom_tex[i]);
        glBindTexture(GL_TEXTURE_2D, rt.bloom_tex[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_HALF_FLOAT, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glGenFramebuffers(1, &rt.bloom_fbo[i]);
        glBindFramebuffer(GL_FRAMEBUFFER, rt.bloom_fbo[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt.bloom_tex[i], 0);
        rt.bloom_w[i] = w;
        rt.bloom_h[i] = h;
        rt.bloom_levels = i + 1;
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            fprintf(stderr, "render_target: %dx%d bloom level incomplete, rtBloom ignored\n", w, h);
            glBindTexture(GL_TEXTURE_2D, 0);
            bloom_delete_targets();
            return 0;
        }
        w /= 2;
        h /= 2;
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    g_active_texture = 0;
    return rt.bloom_levels > 0;
}

static void bloom_draw(const unsigned int source, const int source_w, const int source_h, const int level, const int mode) {
    glBindFramebuffer(GL_FRAMEBUFFER, rt.bloom_fbo[level]);
    glViewport(0, 0, rt.bloom_w[level], rt.bloom_h[level]);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, source);
    glUniform2f(rt.bloom_loc_texel, 1.0f / (float) source_w, 1.0f / (float) source_h);
    glUniform1i(rt.bloom_loc_mode, mode);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

// The chain over `scene` (the scene target or the TAA resolve, scene size): leaves the glow in level 0.
// Returns 0 when it could not run (the composite then draws without bloom).
static int bloom_run(const unsigned int scene) {
    if (!bloom_allocate(rt.width, rt.height)) return 0;
    for (int i = 0; i < rt.bloom_levels; i++) gl_release_texture(rt.bloom_tex[i]);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glUseProgram(rt.bloom_program);
    g_active_shader = rt.bloom_program;
    glUniform1i(rt.bloom_loc_source, 0);
    const int cine = rt_cinematic();
    glUniform1f(rt.bloom_loc_radius, cine ? RT_CINE_BLOOM_RADIUS : BLOOM_RADIUS);
    glUniform1f(glGetUniformLocation(rt.bloom_program, "knee"), cine ? RT_CINE_BLOOM_KNEE : 0.0f);
    glBindVertexArray(rt.vao);
    bloom_draw(scene, rt.width, rt.height, 0, 0); // prefilter
    for (int i = 1; i < rt.bloom_levels; i++) bloom_draw(rt.bloom_tex[i - 1], rt.bloom_w[i - 1], rt.bloom_h[i - 1], i, 1);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    for (int i = rt.bloom_levels - 1; i > 0; i--) bloom_draw(rt.bloom_tex[i], rt.bloom_w[i], rt.bloom_h[i], i - 1, 2);
    glDisable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    g_active_texture = 0;
    return 1;
}

int render_target_bloom_active(void) {
    return rt.active && rt.bloom;
}

static void rt_delete_targets(void) {
    if (rt.fbo) glDeleteFramebuffers(1, &rt.fbo);
    if (rt.color_tex) glDeleteTextures(1, &rt.color_tex);
    if (rt.depth_rb) glDeleteRenderbuffers(1, &rt.depth_rb);
    if (rt.msaa_fbo) glDeleteFramebuffers(1, &rt.msaa_fbo);
    if (rt.msaa_color_rb) glDeleteRenderbuffers(1, &rt.msaa_color_rb);
    if (rt.msaa_depth_rb) glDeleteRenderbuffers(1, &rt.msaa_depth_rb);
    rt.fbo = rt.color_tex = rt.depth_rb = 0;
    rt.msaa_fbo = rt.msaa_color_rb = rt.msaa_depth_rb = 0;
    rt.width = rt.height = rt.samples = 0;
    rt.hdr = 0;
    taa_delete_targets();
    bloom_delete_targets();
    sharp_delete_target();
}

static int rt_init_program(void) {
    if (rt.initialized) return rt.program != 0;
    rt.initialized = 1;

    char *vert = concat(assets_path(), "shaders/scene_composite.vert");
    char *frag = concat(assets_path(), "shaders/scene_composite.frag");
    if (vert && frag) rt.program = shader_compile(vert, frag);
    free(vert);
    free(frag);
    if (!rt.program) {
        fprintf(stderr, "render_target: composite shader unavailable, scene renders at window resolution\n");
        return 0;
    }
    rt.loc_scene = glGetUniformLocation(rt.program, "scene");
    rt.loc_filtered = glGetUniformLocation(rt.program, "filtered");
    rt.loc_saturate = glGetUniformLocation(rt.program, "saturate_matrix");
    rt.loc_hue = glGetUniformLocation(rt.program, "hue_matrix");
    rt.loc_bloom = glGetUniformLocation(rt.program, "bloom");
    rt.loc_bloom_strength = glGetUniformLocation(rt.program, "bloom_strength");
    glGenVertexArrays(1, &rt.vao); // the fullscreen triangle comes from gl_VertexID
    glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &rt.max_size);
    glGetIntegerv(GL_MAX_SAMPLES, &rt.max_samples);
    if (rt.max_size <= 0) rt.max_size = 4096;
    return 1;
}

// The laced pass's program: scene_composite.vert + laced.frag (nettspend.h)
static int rt_init_laced_program(void) {
    if (rt.laced_initialized) return rt.laced_program != 0;
    rt.laced_initialized = 1;

    char *vert = concat(assets_path(), "shaders/scene_composite.vert");
    char *frag = concat(assets_path(), "shaders/laced.frag");
    if (vert && frag) rt.laced_program = shader_compile(vert, frag);
    free(vert);
    free(frag);
    if (!rt.laced_program) {
        fprintf(stderr, "render_target: laced shader unavailable, the laced effect is skipped\n");
        return 0;
    }
    rt.laced_loc_scene = glGetUniformLocation(rt.laced_program, "scene");
    rt.laced_loc_time = glGetUniformLocation(rt.laced_program, "time");
    rt.laced_loc_intensity = glGetUniformLocation(rt.laced_program, "intensity");
    rt.laced_loc_filtered = glGetUniformLocation(rt.laced_program, "filtered");
    rt.laced_loc_saturate = glGetUniformLocation(rt.laced_program, "saturate_matrix");
    rt.laced_loc_hue = glGetUniformLocation(rt.laced_program, "hue_matrix");
    return 1;
}

static int rt_complete(const unsigned int fbo) {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
}

static int rt_allocate(const int width, const int height, int samples, const int hdr) {
    if (rt.fbo && rt.width == width && rt.height == height && rt.samples == samples && rt.hdr == hdr) return 1;
    rt_delete_targets();

    glGenTextures(1, &rt.color_tex);
    glBindTexture(GL_TEXTURE_2D, rt.color_tex);
    if (hdr) glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_HALF_FLOAT, NULL); // bloom
    else glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0); // the scene draws into it next: not left on unit 0
    g_active_texture = 0;            // loaders.c's unit-0 binding cache

    glGenRenderbuffers(1, &rt.depth_rb);
    glBindRenderbuffer(GL_RENDERBUFFER, rt.depth_rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);

    glGenFramebuffers(1, &rt.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, rt.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt.color_tex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rt.depth_rb);
    if (!rt_complete(rt.fbo)) {
        fprintf(stderr, "render_target: %dx%d framebuffer incomplete\n", width, height);
        rt_delete_targets();
        return 0;
    }

    if (samples > 0) {
        glGenRenderbuffers(1, &rt.msaa_color_rb);
        glBindRenderbuffer(GL_RENDERBUFFER, rt.msaa_color_rb);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, hdr ? GL_RGBA16F : GL_RGBA8, width, height);
        glGenRenderbuffers(1, &rt.msaa_depth_rb);
        glBindRenderbuffer(GL_RENDERBUFFER, rt.msaa_depth_rb);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_DEPTH24_STENCIL8, width, height);
        glGenFramebuffers(1, &rt.msaa_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, rt.msaa_fbo);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rt.msaa_color_rb);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rt.msaa_depth_rb);
        if (!rt_complete(rt.msaa_fbo)) {
            fprintf(stderr, "render_target: %dx MSAA unavailable, antiAlias ignored\n", samples);
            glDeleteFramebuffers(1, &rt.msaa_fbo);
            glDeleteRenderbuffers(1, &rt.msaa_color_rb);
            glDeleteRenderbuffers(1, &rt.msaa_depth_rb);
            rt.msaa_fbo = rt.msaa_color_rb = rt.msaa_depth_rb = 0;
            samples = 0;
        }
    }
    glBindRenderbuffer(GL_RENDERBUFFER, 0);

    rt.width = width;
    rt.height = height;
    rt.samples = samples;
    rt.hdr = hdr;
    KN_LOG_VERBOSE2("[render] scene target %dx%d%s%s\n", width, height, samples ? " (MSAA)" : "", hdr ? " (RGBA16F, bloom)" : "");
    return 1;
}

static int rt_filtered(const ClientRenderSettings *s) {
    return fabsf(s->saturation - 1.0f) > 1e-4f || fabsf(s->hue) > 1e-4f;
}

int render_target_begin(Client *client) {
    const ClientRenderSettings *s = client_render_settings();
    rt.active = 0;
    rt.last_width = rt.last_height = 0;
    if (!client || !client->window) return 0;

    int fb_w, fb_h;
    glfwGetFramebufferSize(client->window, &fb_w, &fb_h);
    if (fb_w <= 0 || fb_h <= 0) return 0;

    const float scale = s->resolution > 0.0f ? s->resolution : 1.0f;
    const int aspect = s->aspect_w > 0 && s->aspect_h > 0;
    const int laced = nettspend_laced_intensity() > 0.0f && rt_init_laced_program(); // Nettspend: the laced pass needs the target
    const int taa = s->rt_taa && taa_init_program(); // TAA resolves the scene target
    rt.taa_jittered = 0;
    if (!taa) { taa_delete_targets(); camera_set_jitter(0.0f, 0.0f); }
    // bloom (rtBloom): the weight lives in a half-float target, written by basic.frag only while ray tracing runs
    int bloom = s->rt_bloom && s->rt_mode > RT_MODE_OFF && !rt.bloom_failed && rt_supported() && bloom_init_program();
    rt.bloom = 0;
    if (!bloom) bloom_delete_targets();
    const int need = fabsf(scale - 1.0f) > 1e-4f || s->anti_alias || aspect || rt_filtered(s) || laced || taa || bloom;
    if (!need) {
        if (rt.fbo) rt_delete_targets(); // back to the direct path: release the buffers
        return 0;
    }
    if (!rt_init_program()) return 0;
    const int max_size = rt.max_size;

    int height = (int) lroundf((float) fb_h * scale);
    int width = aspect ? (int) lroundf((float) height * (float) s->aspect_w / (float) s->aspect_h) : (int) lroundf((float) fb_w * scale);
    if (width < 1) width = 1;
    if (height < 1) height = 1;
    if (width > max_size) width = max_size;
    if (height > max_size) height = max_size;

    int samples = 0;
    if (s->anti_alias) samples = rt.max_samples < RT_MAX_SAMPLES ? rt.max_samples : RT_MAX_SAMPLES;

    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &rt.prev_draw_fbo);
    if (bloom && !rt_allocate(width, height, samples, 1)) {
        fprintf(stderr, "render_target: no half-float scene target, rtBloom ignored\n");
        rt.bloom_failed = 1; // not retried every frame
        bloom = 0;
    } else if (bloom && samples && !rt.samples) {
        // a float multisample buffer was refused: keep MSAA, drop bloom (antiAlias was asked first)
        fprintf(stderr, "render_target: no half-float MSAA buffer, rtBloom ignored with antiAlias\n");
        rt.bloom_failed = 1;
        bloom = 0;
    }
    if (!bloom && !rt_allocate(width, height, samples, 0)) {
        glBindFramebuffer(GL_FRAMEBUFFER, (unsigned int) rt.prev_draw_fbo);
        return 0;
    }
    rt.bloom = bloom;

    gl_release_texture(rt.color_tex); // no unit may hold the texture the scene draws into
    glBindFramebuffer(GL_FRAMEBUFFER, rt.samples ? rt.msaa_fbo : rt.fbo);
    glViewport(0, 0, width, height);
    glDepthMask(GL_TRUE);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT); // glClearColor = map sky (client_apply_map_lighting)
    if (taa) taa_set_jitter(width, height); // every projection of the frame from here on is jittered

    rt.fb_width = fb_w;
    rt.fb_height = fb_h;
    rt.last_width = width;
    rt.last_height = height;
    rt.active = 1;
    return 1;
}

// CSS Filter Effects 1: saturate(s) and hue-rotate(a) colour matrices (row-major).
static void saturate_matrix(const float s, float m[9]) {
    const float v[9] = {
        0.213f + 0.787f * s, 0.715f - 0.715f * s, 0.072f - 0.072f * s,
        0.213f - 0.213f * s, 0.715f + 0.285f * s, 0.072f - 0.072f * s,
        0.213f - 0.213f * s, 0.715f - 0.715f * s, 0.072f + 0.928f * s,
    };
    for (int i = 0; i < 9; i++) m[i] = v[i];
}

static void hue_matrix(const float degrees, float m[9]) {
    const float a = degrees * (float) M_PI / 180.0f;
    const float c = cosf(a), s = sinf(a);
    const float v[9] = {
        0.213f + c * 0.787f - s * 0.213f, 0.715f - c * 0.715f - s * 0.715f, 0.072f - c * 0.072f + s * 0.928f,
        0.213f - c * 0.213f + s * 0.143f, 0.715f + c * 0.285f + s * 0.140f, 0.072f - c * 0.072f - s * 0.283f,
        0.213f - c * 0.213f - s * 0.787f, 0.715f - c * 0.715f + s * 0.715f, 0.072f + c * 0.928f + s * 0.072f,
    };
    for (int i = 0; i < 9; i++) m[i] = v[i];
}

void render_target_end(Client *client) {
    (void) client;
    if (!rt.active) return;
    rt.active = 0;
    const ClientRenderSettings *s = client_render_settings();
    post_timing_begin();

    if (rt.samples) {
        // resolve: MSAA renderbuffer -> single-sample texture (same size)
        glBindFramebuffer(GL_READ_FRAMEBUFFER, rt.msaa_fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, rt.fbo);
        glBlitFramebuffer(0, 0, rt.width, rt.height, 0, 0, rt.width, rt.height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
    post_timing_mark(1);

    // TAA: resolve into the history first (its own framebuffer, scene size); the composite draws the resolve
    unsigned int scene_tex = rt.color_tex;
    if (rt.taa_jittered) {
        camera_set_jitter(0.0f, 0.0f); // the HUD / previews / page draw unjittered
        rt.taa_jittered = 0;
        const unsigned int resolved = s->rt_taa ? taa_resolve() : 0;
        if (resolved) scene_tex = resolved;
    }

    post_timing_mark(2);
    // bloom: the chain over the (resolved) scene; level 0 goes to the composite
    const int bloom = rt.bloom && bloom_run(scene_tex);
    rt.bloom = 0;
    post_timing_mark(3);

    glBindFramebuffer(GL_FRAMEBUFFER, (unsigned int) rt.prev_draw_fbo);
    glViewport(0, 0, rt.fb_width, rt.fb_height);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
#ifndef KN_GLES
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
#endif

    // Nettspend laced (nettspend.h): the distortion pass instead of the plain composite
    const float laced_intensity = nettspend_laced_intensity();
    const int laced = laced_intensity > 0.0f && rt_init_laced_program();

    // rtSharpen: edge-adaptive upscale (when scaled) + contrast-adaptive sharpening instead of the composite
    const int level = laced ? 0 : sharp_level(s);
    const int scaled = rt.width != rt.fb_width || rt.height != rt.fb_height;
    if (level && (scaled || s->rt_taa) && sharp_init_programs()) {
        unsigned int source = scene_tex;
        int ok = 1;
        glBindVertexArray(rt.vao);
        if (scaled) {
            ok = sharp_allocate(rt.fb_width, rt.fb_height);
            if (ok) {
                gl_release_texture(rt.up_tex);
                glBindFramebuffer(GL_FRAMEBUFFER, rt.up_fbo);
                glViewport(0, 0, rt.fb_width, rt.fb_height);
                glUseProgram(rt.upscale_program);
                g_active_shader = rt.upscale_program;
                glActiveTexture(GL_TEXTURE0 + RT_SCENE_TEXTURE_UNIT);
                glBindTexture(GL_TEXTURE_2D, scene_tex);
                glUniform1i(rt.up_loc_scene, RT_SCENE_TEXTURE_UNIT);
                glUniform2f(rt.up_loc_input, (float) rt.width, (float) rt.height);
                glUniform2f(rt.up_loc_output, (float) rt.fb_width, (float) rt.fb_height);
                glDrawArrays(GL_TRIANGLES, 0, 3);
                source = rt.up_tex;
                post_timing_mark(4);
                glBindFramebuffer(GL_FRAMEBUFFER, (unsigned int) rt.prev_draw_fbo);
                glViewport(0, 0, rt.fb_width, rt.fb_height);
            }
        }
        if (ok) {
            if (!scaled) post_timing_mark(4);
            glUseProgram(rt.sharpen_program);
            g_active_shader = rt.sharpen_program;
            glActiveTexture(GL_TEXTURE0 + RT_SCENE_TEXTURE_UNIT);
            glBindTexture(GL_TEXTURE_2D, source);
            glUniform1i(rt.sh_loc_source, RT_SCENE_TEXTURE_UNIT);
            glUniform1f(rt.sh_loc_sharpness, exp2f(-SHARPEN_STOPS[level]));
            glUniform2f(rt.sh_loc_output, (float) rt.fb_width, (float) rt.fb_height);
            glUniform1f(rt.sh_loc_bloom_strength, bloom ? bloom_strength_now() : 0.0f);
            const int graded = grade_uniforms(rt.sharpen_program, rt.fb_width, rt.fb_height);
            if (bloom) {
                glUniform1i(rt.sh_loc_bloom, BLOOM_UNIT);
                glActiveTexture(GL_TEXTURE0 + BLOOM_UNIT);
                glBindTexture(GL_TEXTURE_2D, rt.bloom_tex[0]);
                glActiveTexture(GL_TEXTURE0 + RT_SCENE_TEXTURE_UNIT);
            }
            const int filtered = rt_filtered(s);
            glUniform1i(rt.sh_loc_filtered, filtered);
            if (filtered) {
                float sat[9], hue[9];
                saturate_matrix(s->saturation, sat);
                hue_matrix(s->hue * 3.6f, hue);
                glUniformMatrix3fv(rt.sh_loc_saturate, 1, GL_TRUE, sat);
                glUniformMatrix3fv(rt.sh_loc_hue, 1, GL_TRUE, hue);
            }
            glDrawArrays(GL_TRIANGLES, 0, 3);
            post_timing_mark(5);
            glBindVertexArray(0);
            grade_unbind(graded);
            if (bloom) {
                glActiveTexture(GL_TEXTURE0 + BLOOM_UNIT);
                glBindTexture(GL_TEXTURE_2D, 0);
                glActiveTexture(GL_TEXTURE0 + RT_SCENE_TEXTURE_UNIT);
            }
            glBindTexture(GL_TEXTURE_2D, 0);
            g_active_texture = 0;
            glEnable(GL_DEPTH_TEST);
            glClear(GL_DEPTH_BUFFER_BIT);
            return;
        }
        glBindVertexArray(0);
    }
    if (!level || laced) sharp_delete_target(); // no window-size buffer kept while unused
    const unsigned int program = laced ? rt.laced_program : rt.program;
    post_timing_mark(4);

    glUseProgram(program);
    g_active_shader = program;
    glActiveTexture(GL_TEXTURE0 + RT_SCENE_TEXTURE_UNIT);
    glBindTexture(GL_TEXTURE_2D, scene_tex);
    g_active_texture = scene_tex;
    glUniform1i(laced ? rt.laced_loc_scene : rt.loc_scene, RT_SCENE_TEXTURE_UNIT);
    if (laced) {
        glUniform1f(rt.laced_loc_time, nettspend_laced_time());
        glUniform1f(rt.laced_loc_intensity, laced_intensity);
    }

    if (!laced) {
        glUniform1f(rt.loc_bloom_strength, bloom ? bloom_strength_now() : 0.0f);
        if (bloom) {
            glUniform1i(rt.loc_bloom, BLOOM_UNIT);
            glActiveTexture(GL_TEXTURE0 + BLOOM_UNIT);
            glBindTexture(GL_TEXTURE_2D, rt.bloom_tex[0]);
            glActiveTexture(GL_TEXTURE0 + RT_SCENE_TEXTURE_UNIT);
        }
    }

    const int filtered = rt_filtered(s);
    glUniform1i(laced ? rt.laced_loc_filtered : rt.loc_filtered, filtered);
    if (filtered) {
        float sat[9], hue[9];
        saturate_matrix(s->saturation, sat);
        hue_matrix(s->hue * 3.6f, hue); // hue-rotate(v * 3.6deg)
        glUniformMatrix3fv(laced ? rt.laced_loc_saturate : rt.loc_saturate, 1, GL_TRUE, sat);
        glUniformMatrix3fv(laced ? rt.laced_loc_hue : rt.loc_hue, 1, GL_TRUE, hue);
    }

    const int graded = laced ? 0 : grade_uniforms(program, rt.fb_width, rt.fb_height);
    glBindVertexArray(rt.vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    post_timing_mark(5);
    glBindVertexArray(0);
    grade_unbind(graded);
    if (bloom && !laced) { // the next frame's chain writes level 0: no unit keeps it
        glActiveTexture(GL_TEXTURE0 + BLOOM_UNIT);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE0 + RT_SCENE_TEXTURE_UNIT);
    }
    // the next frame's scene draws into this texture: unit 0 does not keep it
    glBindTexture(GL_TEXTURE_2D, 0);
    g_active_texture = 0;

    glEnable(GL_DEPTH_TEST);
    glClear(GL_DEPTH_BUFFER_BIT); // later passes start from an empty depth buffer, as with the direct path's fps clear
}

void render_target_size(int *width, int *height) {
    if (width) *width = rt.last_width;
    if (height) *height = rt.last_height;
}

void render_target_fini(void) {
    rt_delete_targets();
    rt_grade_fini();
    if (rt.program) glDeleteProgram(rt.program);
    if (rt.laced_program) glDeleteProgram(rt.laced_program);
    if (rt.taa_program) glDeleteProgram(rt.taa_program);
    if (rt.bloom_program) glDeleteProgram(rt.bloom_program);
    if (rt.upscale_program) glDeleteProgram(rt.upscale_program);
    if (rt.sharpen_program) glDeleteProgram(rt.sharpen_program);
    if (g_active_shader == rt.upscale_program || g_active_shader == rt.sharpen_program) g_active_shader = 0;
    rt.upscale_program = rt.sharpen_program = 0;
    rt.sharp_initialized = 0;
    if (g_active_shader == rt.bloom_program) g_active_shader = 0;
    rt.bloom_program = 0;
    rt.bloom_initialized = 0;
    if (rt.vao) glDeleteVertexArrays(1, &rt.vao);
    if (g_active_shader == rt.program || g_active_shader == rt.laced_program || g_active_shader == rt.taa_program) g_active_shader = 0;
    rt.program = rt.laced_program = rt.taa_program = rt.vao = 0;
    rt.initialized = rt.laced_initialized = rt.taa_initialized = 0;
}
