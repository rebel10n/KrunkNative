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
#include <gl.h>
#include "klog.h"
#include <GLFW/glfw3.h>
#include <nettspend.h>
#include <render_settings.h>
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
} rt;

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

static int rt_allocate(const int width, const int height, int samples) {
    if (rt.fbo && rt.width == width && rt.height == height && rt.samples == samples) return 1;
    rt_delete_targets();

    glGenTextures(1, &rt.color_tex);
    glBindTexture(GL_TEXTURE_2D, rt.color_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
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
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_RGBA8, width, height);
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
    KN_LOG_VERBOSE2("[render] scene target %dx%d%s\n", width, height, samples ? " (MSAA)" : "");
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
    const int need = fabsf(scale - 1.0f) > 1e-4f || s->anti_alias || aspect || rt_filtered(s) || laced;
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
    if (!rt_allocate(width, height, samples)) {
        glBindFramebuffer(GL_FRAMEBUFFER, (unsigned int) rt.prev_draw_fbo);
        return 0;
    }

    gl_release_texture(rt.color_tex); // no unit may hold the texture the scene draws into
    glBindFramebuffer(GL_FRAMEBUFFER, rt.samples ? rt.msaa_fbo : rt.fbo);
    glViewport(0, 0, width, height);
    glDepthMask(GL_TRUE);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT); // glClearColor = map sky (client_apply_map_lighting)

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

    if (rt.samples) {
        // resolve: MSAA renderbuffer -> single-sample texture (same size)
        glBindFramebuffer(GL_READ_FRAMEBUFFER, rt.msaa_fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, rt.fbo);
        glBlitFramebuffer(0, 0, rt.width, rt.height, 0, 0, rt.width, rt.height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }

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
    const unsigned int program = laced ? rt.laced_program : rt.program;

    glUseProgram(program);
    g_active_shader = program;
    glActiveTexture(GL_TEXTURE0 + RT_SCENE_TEXTURE_UNIT);
    glBindTexture(GL_TEXTURE_2D, rt.color_tex);
    g_active_texture = rt.color_tex;
    glUniform1i(laced ? rt.laced_loc_scene : rt.loc_scene, RT_SCENE_TEXTURE_UNIT);
    if (laced) {
        glUniform1f(rt.laced_loc_time, nettspend_laced_time());
        glUniform1f(rt.laced_loc_intensity, laced_intensity);
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

    glBindVertexArray(rt.vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
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
    if (rt.program) glDeleteProgram(rt.program);
    if (rt.laced_program) glDeleteProgram(rt.laced_program);
    if (rt.vao) glDeleteVertexArrays(1, &rt.vao);
    if (g_active_shader == rt.program || g_active_shader == rt.laced_program) g_active_shader = 0;
    rt.program = rt.laced_program = rt.vao = 0;
    rt.initialized = rt.laced_initialized = 0;
}
