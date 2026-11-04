// Ray tracing per-frame driver (client/include/rt.h, doc/raytracing.md). Owns the RtScene,
// its GPU tables (rt_gpu.c), the RT framebuffers and programs, and runs, from main.c's world pass
// (after the sky and the hide / third-person swaps, right before the world scene_render):
//   1. rt_scene_build + rt_gpu_upload           the BVH of this frame's visible world meshes (+ rt_self.c's
//                                               first-person shadow puppet as RT_INST_SHADOW_ONLY casters)
//   2. atlas update (rt_atlas.c)                only when a reflective or alpha-tested caster needs it
//   3. prepass (rt_prepass.vert/.frag)          depth + world normal (+ material, velocity) of the opaque world
//   4. trace (rt_trace.frag)                    per pixel: the sun shadow ray (phase 2), the AO rays (phase 3),
//                                               the local light shadow rays (phase 5, second attachment)
//   5. denoise (rt_denoise.frag)                temporal reprojection (motion vectors), then a-trous steps
//   6. reflect + rt_refl_denoise (phase 4)      one GGX-jittered closest-hit ray where a material reflects
//   7. gi + rt_gi_denoise (RT_EFFECT_GI)        one cosine-weighted closest-hit ray per lit pixel: one-bounce diffuse GI
// and hands the result to basic.frag for the world draw (rt_begin_world / rt_apply_mesh_uniforms:
// RT_TEXTURE_UNIT.., rt_effects incl. RT_EFFECT_SKY_AMBIENT, rt_inv_viewport). rt_mode 0 (the default) runs
// none of it, except that rtTaa runs step 3 alone for its motion vectors (the velocity-only path, rt_taa_velocity).
// Later in the frame, main.c's fps scene draw calls rt_viewmodel_frame (rtMode >= 2): a prepass of the first-person
// weapon / arms with the fps camera into a second target set, then GI / reflect from those surfaces into the
// world's tables; rt_begin_viewmodel / rt_end_viewmodel bracket that draw. RT_EFFECT_MATERIALS (rt_materials.c)
// joins rt_effects with any rtMode > 0 when the class table loaded.
//
// GL state: every pass saves and restores the framebuffer and viewport it found, and leaves depth
// test on (GL_LESS, mask on), blend off, cull off, VAO 0, program 0 (g_active_shader 0), unit 0
// active with no texture (g_active_texture 0), like shadow_update(). Texture units 0..7 are used
// inside the passes only; units 1..6 are snapshotted on entry (save_units) and restored, since the
// world pass already bound the sun shadow map (shadow_begin_world) before rt_frame runs; materials
// rebind theirs per draw (unit 0 through g_active_texture).
#include <gl.h>
#include <rt.h>
#include <sky.h> // sky_physical_uniforms: the rtSky misses of the reflect / GI passes
#include <shadow.h> // shadow_sun_map: the sun shadow map as a cheap sun visibility at GI / reflection hits
#include <render_settings.h>
#include <ruleset.h> // light_unit_factor: the local lights' luminance as basic.c scales it
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RT_TRACE_FAR 20000.0f  // shadow ray length (world units; maps are ~1000 across)
// AO ray length. Tuned on Sandstorm / Kanji / Burg against Krunker's ambient_0/1 strips (ambient.c,
// which RT AO replaces): the strips are ~10 units wide, and at 12 units the contact corners of the
// usual 10-20 unit crates and walls darken about as much as the strips do while open floors and
// roofs stay untouched; 24 greyed out alleys, 6 only showed under overhangs.
#define RT_AO_RADIUS 12.0f
// Occluders nearer than this along an AO ray are ignored: the posters, decal planes, crate planks
// and trims that sit a fraction of a unit proud of a face would otherwise draw a dark seam around
// themselves (Krunker's strips only form between whole boxes). 0.5 keeps every real corner.
#define RT_AO_MIN 0.5f
#define RT_DEBUG_NONE 0

typedef struct {
    RtScene scene;
    RtGpu gpu;

    // programs (compiled on first use; a failure turns the tracer off for the run)
    unsigned int prepass_program, trace_program, denoise_program, debug_program;
    int prepass_loc_transform, prepass_loc_dynamic;
    int prepass_loc_vertex_normal; // the viewmodel prepass takes the vertex normals
    unsigned int vao;          // the fullscreen triangle (gl_VertexID, scene_composite.vert)
    int programs_tried, programs_ok;

    // targets at RT resolution
    int width, height;
    unsigned int prepass_fbo, depth_texture, normal_texture;
    unsigned int trace_fbo, trace_texture;
    unsigned int history_fbo[2], history_texture[2];
    int history_index;         // the history the next temporal pass reads
    int history_valid;         // 0 after a reset / resize / map change
    unsigned int filter_fbo[2], filter_texture[2];
    unsigned int result_texture; // what basic.frag reads this frame (one of the above, not owned)

    // phase 5, local light shadows (rt.h RT_LOCAL_*): the trace pass's second colour attachment
    // (packed visibilities, RGBA32F nearest), its temporal history, and this frame's light set
    int local_supported;       // the RGBA32F attachment completed (0: the effect stays off, logged once)
    unsigned int local_texture;
    unsigned int local_history_fbo[2], local_history_texture[2];
    unsigned int local_result_texture; // what basic.frag reads (not owned)
    int local_count;           // lights traced this frame (0 = effect off)
    int local_index[BASIC_MAX_LIGHTS]; // basic.frag light slot -> traced light index, -1 none
    int local_mask;            // bit t: traced slot t shoots its rays this frame (RT_LOCAL_PER_FRAME round robin)
    int local_persistent;      // the first local_persistent traced slots are map lights (round robin, history)
    int local_fresh_mask;      // bit t: traced slot t is a transient light (vfx.c flash): shot every frame, no history

    // phase 4, reflections (rt_reflect.frag, rt_refl_denoise.frag, rt_atlas.c): the prepass's second
    // attachment (the pixel's reflection material), the traced radiance, its history and filter targets
    int refl_checked, refl_supported; // basic.frag can bind rt_refl (GL_MAX_TEXTURE_IMAGE_UNITS) and the programs compiled
    int refl_active;                  // this frame traces reflections (rt_mode 3): the prepass writes the materials
    unsigned int refl_program, refl_denoise_program;
    int prepass_loc_material;
    unsigned int material_texture;    // prepass attachment 1: (reflect intensity, roughness, metalness)
    int material_attached;
    unsigned int refl_fbo, refl_texture;
    unsigned int refl_history_fbo[2], refl_history_texture[2];
    int refl_history_index, refl_history_valid;
    unsigned int refl_filter_fbo[2], refl_filter_texture[2];
    unsigned int refl_result_texture; // what basic.frag reads (not owned)

    // phase 6, motion vectors: the prepass's third colour attachment (rt_prepass.frag frag_velocity,
    // RGBA16F: uv delta, previous view depth, written flag); the denoiser reprojects with it
    unsigned int velocity_texture;
    int prepass_loc_prev_transform, prepass_loc_previous_view_projection;
    int prepass_loc_velocity_view_projection; // TAA: the unjittered current camera for the velocity delta

    // one-bounce diffuse GI (rt.h RT_EFFECT_GI, rt_gi.frag, rt_gi_denoise.frag): the traced radiance, its
    // history and filter targets (RGBA16F); basic.frag reads the result on RT_GI_TEXTURE_UNIT
    int gi_checked, gi_supported;     // an 18th fragment sampler exists and the two programs compiled
    unsigned int gi_program, gi_denoise_program;
    int gi_width, gi_height;          // RT resolution / RT_GI_DIVISOR
    unsigned int gi_fbo, gi_texture;
    unsigned int gi_history_fbo[2], gi_history_texture[2];
    int gi_history_index, gi_history_valid;
    unsigned int gi_filter_fbo[2], gi_filter_texture[2];
    unsigned int gi_result_texture;   // what basic.frag reads (not owned)

    float previous_view_projection[16]; // unjittered (TAA): the velocity and the camera-only reprojections use it
    int frame_index;

    // TAA (render_target.c, rt_taa_velocity): this frame's unjittered view-projection, the one before it, and
    // whether the prepass ran (with rtMode 0 + rtTaa the velocity-only path runs the prepass alone)
    float taa_view_projection[16], taa_previous_view_projection[16];
    int velocity_ready, taa_sky_cached;
    float taa_sky_reproject[16];
    unsigned history_epoch;   // rt_history_epoch(): bumped on every history reset
    // what texture units 1..6 held when rt_frame started (the world pass's sun shadow map sits on unit 1,
    // bound by shadow_begin_world() before rt_frame): restore_state puts them back instead of unbinding
    unsigned int saved_units[7];

    // the world pass
    int world_active;          // between rt_frame (with a result) and rt_end_world
    int effects;               // RT_EFFECT_* bits of this frame
    int scene_width, scene_height; // the world pass's viewport (the scene target or the window)

    // the first-person viewmodel (rt_viewmodel_frame, main.c's fps scene draw): the weapon / arms get the
    // reflect and GI passes with the fps camera, into targets of their own (g_vm; swapped into the fields above
    // while its passes run, vm_swap), and the fps draw reads them between rt_begin_viewmodel / rt_end_viewmodel
    int vm_active;             // between rt_begin_viewmodel and rt_end_viewmodel
    int vm_drawing;            // the fps scene is being drawn (set even when the viewmodel pass did not run)
    int vm_ready;              // rt_viewmodel_frame produced results this frame
    int vm_effects;            // RT_EFFECT_* bits of the fps draw
    float sky_probe[6];        // the viewmodel's sky visibility along +x -x +y -y +z -z (RT_EFFECT_SKY_PROBE), smoothed
    int sky_probe_valid;       // 0: the next probe starts the average afresh (map load, history reset)
    unsigned sky_probe_frame;  // rotates the probe's ray set
    int vm_running;            // inside rt_viewmodel_frame's passes: the reflect / GI shaders take the prepass normal as is
    unsigned int vm_refl_result_texture, vm_gi_result_texture; // what the fps draw reads (not owned)
    int traced_frame;          // rt_frame ran the full trace this frame (tables uploaded; consumed by rt_viewmodel_frame)

    int support_checked, supported;
    int debug_mode;            // rt_debug: 0 off, 1 bvh, 2 normals, 3 visibility, 4 history, 5 ao, 6 local, 7 refl, 8 motion, 9 gi
    int debug_env_read;        // KRUNKNATIVE_RT_DEBUG consulted
    int build_logged;
} RtState;

static RtState g_rt;

// ---------------------------------------------------------------- matrices (row-major, like Camera's)

static int mat4_invert(const float m[16], float out[16]) {
    float inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    const float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (fabsf(det) < 1e-30f) return 0;
    const float inv_det = 1.0f / det;
    for (int i = 0; i < 16; i++) out[i] = inv[i] * inv_det;
    return 1;
}

static float determinant3(const float m[16]) {
    return m[0] * (m[5] * m[10] - m[6] * m[9]) - m[1] * (m[4] * m[10] - m[6] * m[8]) + m[2] * (m[4] * m[9] - m[5] * m[8]);
}

// ---------------------------------------------------------------- support / programs

int rt_supported_cached(void) {
    return g_rt.support_checked && g_rt.supported;
}

int rt_cinematic(void) {
    const ClientRenderSettings *s = client_render_settings_stored();
    return s->rt_preset == 1 && s->rt_mode > RT_MODE_OFF && rt_supported();
}

int rt_supported(void) {
    if (g_rt.support_checked) return g_rt.supported;
    g_rt.support_checked = 1;
    g_rt.supported = 0;

    int max_size = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_size);
    if (max_size < RT_TEX_WIDTH) {
        fprintf(stderr, "[rt] GL_MAX_TEXTURE_SIZE %d < %d: ray tracing unavailable\n", max_size, RT_TEX_WIDTH);
        return 0;
    }
    // a half-float colour attachment (core on desktop; EXT_color_buffer_float on ES / WebGL2)
    int previous_fbo;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous_fbo);
    unsigned int texture = 0, fbo = 0;
    glGenTextures(1, &texture);
    glActiveTexture(GL_TEXTURE0 + RT_TEXTURE_UNIT);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, 4, 4, 0, GL_RGBA, GL_HALF_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    const int complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, (unsigned int) previous_fbo);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &texture);
    if (!complete) {
        fprintf(stderr, "[rt] no RGBA16F colour attachment (EXT_color_buffer_float): ray tracing unavailable\n");
        return 0;
    }
    g_rt.supported = 1;
    return 1;
}

static unsigned int compile(const char *vert_name, const char *frag_name) {
    char *vert = concat(assets_path(), vert_name);
    char *frag = concat(assets_path(), frag_name);
    unsigned int program = 0;
    if (vert && frag) program = shader_compile(vert, frag);
    free(vert);
    free(frag);
    return program;
}

static int programs_ready(void) {
    if (g_rt.programs_tried) return g_rt.programs_ok;
    g_rt.programs_tried = 1;
    g_rt.prepass_program = compile("shaders/rt_prepass.vert", "shaders/rt_prepass.frag");
    g_rt.trace_program = compile("shaders/scene_composite.vert", "shaders/rt_trace.frag");
    g_rt.denoise_program = compile("shaders/scene_composite.vert", "shaders/rt_denoise.frag");
    g_rt.debug_program = compile("shaders/scene_composite.vert", "shaders/rt_debug.frag");
    g_rt.programs_ok = g_rt.prepass_program && g_rt.trace_program && g_rt.denoise_program && g_rt.debug_program;
    if (!g_rt.programs_ok) {
        fprintf(stderr, "[rt] shaders failed to compile: ray tracing off for this run\n");
        return 0;
    }
    g_rt.prepass_loc_transform = glGetUniformLocation(g_rt.prepass_program, "transform");
    g_rt.prepass_loc_dynamic = glGetUniformLocation(g_rt.prepass_program, "dynamic");
    g_rt.prepass_loc_vertex_normal = glGetUniformLocation(g_rt.prepass_program, "use_vertex_normal");
    g_rt.prepass_loc_material = glGetUniformLocation(g_rt.prepass_program, "material_refl");
    g_rt.prepass_loc_prev_transform = glGetUniformLocation(g_rt.prepass_program, "prev_transform");
    g_rt.prepass_loc_previous_view_projection = glGetUniformLocation(g_rt.prepass_program, "previous_view_projection");
    g_rt.prepass_loc_velocity_view_projection = glGetUniformLocation(g_rt.prepass_program, "velocity_view_projection");
    glGenVertexArrays(1, &g_rt.vao);
    return 1;
}

// ---------------------------------------------------------------- targets

static unsigned int make_texture(const int width, const int height, const unsigned int internal, const unsigned int format,
                                 const unsigned int type, const int linear) {
    unsigned int texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, (int) internal, width, height, 0, format, type, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return texture;
}

static unsigned int make_fbo(const unsigned int color, const unsigned int depth) {
    unsigned int fbo;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
    if (depth) glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depth, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        glDeleteFramebuffers(1, &fbo);
        return 0;
    }
    return fbo;
}

// ---- phase 4 targets (reflections)
static void delete_refl_targets(void) {
    unsigned int fbos[5] = {g_rt.refl_fbo, g_rt.refl_history_fbo[0], g_rt.refl_history_fbo[1], g_rt.refl_filter_fbo[0], g_rt.refl_filter_fbo[1]};
    unsigned int textures[6] = {g_rt.material_texture, g_rt.refl_texture, g_rt.refl_history_texture[0], g_rt.refl_history_texture[1],
                                g_rt.refl_filter_texture[0], g_rt.refl_filter_texture[1]};
    for (int i = 0; i < 5; i++) if (fbos[i]) glDeleteFramebuffers(1, &fbos[i]);
    for (int i = 0; i < 6; i++) if (textures[i]) gl_release_texture(textures[i]), glDeleteTextures(1, &textures[i]);
    g_rt.refl_fbo = g_rt.refl_history_fbo[0] = g_rt.refl_history_fbo[1] = g_rt.refl_filter_fbo[0] = g_rt.refl_filter_fbo[1] = 0;
    g_rt.material_texture = g_rt.refl_texture = 0;
    g_rt.refl_history_texture[0] = g_rt.refl_history_texture[1] = g_rt.refl_filter_texture[0] = g_rt.refl_filter_texture[1] = 0;
    g_rt.refl_result_texture = 0;
    g_rt.material_attached = 0;
    g_rt.refl_history_valid = 0;
}

// ---- GI targets
static void delete_gi_targets(void) {
    unsigned int fbos[5] = {g_rt.gi_fbo, g_rt.gi_history_fbo[0], g_rt.gi_history_fbo[1], g_rt.gi_filter_fbo[0], g_rt.gi_filter_fbo[1]};
    unsigned int textures[5] = {g_rt.gi_texture, g_rt.gi_history_texture[0], g_rt.gi_history_texture[1], g_rt.gi_filter_texture[0], g_rt.gi_filter_texture[1]};
    for (int i = 0; i < 5; i++) if (fbos[i]) glDeleteFramebuffers(1, &fbos[i]);
    for (int i = 0; i < 5; i++) if (textures[i]) gl_release_texture(textures[i]), glDeleteTextures(1, &textures[i]);
    g_rt.gi_fbo = g_rt.gi_history_fbo[0] = g_rt.gi_history_fbo[1] = g_rt.gi_filter_fbo[0] = g_rt.gi_filter_fbo[1] = 0;
    g_rt.gi_texture = g_rt.gi_history_texture[0] = g_rt.gi_history_texture[1] = g_rt.gi_filter_texture[0] = g_rt.gi_filter_texture[1] = 0;
    g_rt.gi_result_texture = 0;
    g_rt.gi_history_valid = 0;
}

// The GI radiance / history / filter targets (RGBA16F, linear: basic.frag samples the result bilinearly).
// Called with RT_TEXTURE_UNIT active, like ensure_targets. Incomplete: GI stays off (logged once).
static void ensure_gi_targets(const int full_width, const int full_height) {
    delete_gi_targets();
    const int width = full_width / RT_GI_DIVISOR > 0 ? full_width / RT_GI_DIVISOR : 1;
    const int height = full_height / RT_GI_DIVISOR > 0 ? full_height / RT_GI_DIVISOR : 1;
    g_rt.gi_width = width;
    g_rt.gi_height = height;
    g_rt.gi_texture = make_texture(width, height, GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, 1);
    for (int i = 0; i < 2; i++) {
        g_rt.gi_history_texture[i] = make_texture(width, height, GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, 1);
        g_rt.gi_filter_texture[i] = make_texture(width, height, GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, 1);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    g_rt.gi_fbo = make_fbo(g_rt.gi_texture, 0);
    int ok = g_rt.gi_fbo != 0;
    for (int i = 0; i < 2; i++) {
        g_rt.gi_history_fbo[i] = make_fbo(g_rt.gi_history_texture[i], 0);
        g_rt.gi_filter_fbo[i] = make_fbo(g_rt.gi_filter_texture[i], 0);
        ok = ok && g_rt.gi_history_fbo[i] && g_rt.gi_filter_fbo[i];
    }
    if (!ok) {
        static int logged;
        if (!logged) logged = 1, fprintf(stderr, "[rt] GI targets incomplete: global illumination unavailable\n");
        delete_gi_targets();
    }
}

static void delete_targets(void) {
    delete_refl_targets();
    delete_gi_targets();
    unsigned int fbos[9] = {g_rt.prepass_fbo, g_rt.trace_fbo, g_rt.history_fbo[0], g_rt.history_fbo[1], g_rt.filter_fbo[0], g_rt.filter_fbo[1],
                            g_rt.local_history_fbo[0], g_rt.local_history_fbo[1], 0};
    unsigned int textures[11] = {g_rt.depth_texture, g_rt.normal_texture, g_rt.trace_texture, g_rt.history_texture[0], g_rt.history_texture[1],
                                 g_rt.filter_texture[0], g_rt.filter_texture[1], g_rt.local_texture, g_rt.local_history_texture[0],
                                 g_rt.local_history_texture[1], 0};
    for (int i = 0; i < 9; i++) if (fbos[i]) glDeleteFramebuffers(1, &fbos[i]);
    for (int i = 0; i < 11; i++) if (textures[i]) gl_release_texture(textures[i]), glDeleteTextures(1, &textures[i]);
    g_rt.prepass_fbo = g_rt.trace_fbo = 0;
    g_rt.history_fbo[0] = g_rt.history_fbo[1] = g_rt.filter_fbo[0] = g_rt.filter_fbo[1] = 0;
    g_rt.local_history_fbo[0] = g_rt.local_history_fbo[1] = 0;
    g_rt.depth_texture = g_rt.normal_texture = g_rt.trace_texture = 0;
    g_rt.history_texture[0] = g_rt.history_texture[1] = g_rt.filter_texture[0] = g_rt.filter_texture[1] = 0;
    g_rt.local_texture = g_rt.local_history_texture[0] = g_rt.local_history_texture[1] = 0;
    if (g_rt.velocity_texture) gl_release_texture(g_rt.velocity_texture), glDeleteTextures(1, &g_rt.velocity_texture);
    g_rt.velocity_texture = 0;
    g_rt.result_texture = g_rt.local_result_texture = 0;
    g_rt.width = g_rt.height = 0;
    g_rt.history_valid = 0;
}

// The prepass FBO's draw buffer list from whatever is attached (attachment 0 the normal, 1 the
// reflection material, 2 the motion vector): each attachment is set up by its own ensure_* step, so
// the list is rebuilt from the FBO itself rather than assumed. Call with the prepass FBO bound.
static void prepass_draw_buffers(void) {
    unsigned int buffers[3];
    int count = 0;
    for (int i = 0; i < 3; i++) {
        int type = GL_NONE;
        glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + (unsigned int) i, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
        buffers[i] = type != GL_NONE ? GL_COLOR_ATTACHMENT0 + (unsigned int) i : GL_NONE;
        if (type != GL_NONE) count = i + 1;
    }
    glDrawBuffers(count ? count : 1, buffers);
}

// The motion vector target as the prepass FBO's third attachment (rt_prepass.frag location 2; phase
// 6). Without it (a driver refusing the attachment) the denoiser falls back to the camera-only
// reprojection it had before (rt_velocity_valid 0).
static int rt_no_materials(void) { // KRUNKNATIVE_RT_NO_MATERIALS=1 (dev only): Krunker's shading for every material (A/B of the class table)
    static int cached = -1;
    if (cached < 0) { const char *v = getenv("KRUNKNATIVE_RT_NO_MATERIALS"); cached = v && *v && *v != '0'; }
    return cached;
}

static int rt_no_sky_ambient(void) { // KRUNKNATIVE_RT_NO_SKY_AMBIENT=1 (dev only): keep Krunker's constant ambient (A/B)
    static int no_sky = -1;
    if (no_sky < 0) { const char *e = getenv("KRUNKNATIVE_RT_NO_SKY_AMBIENT"); no_sky = e && *e && strcmp(e, "0") != 0; }
    return no_sky;
}

static int rt_no_bevel(void) { // KRUNKNATIVE_RT_NO_BEVEL=1 (dev only): Krunker's hard cube edges (A/B of RT_EFFECT_BEVEL)
    static int no_bevel = -1;
    if (no_bevel < 0) { const char *e = getenv("KRUNKNATIVE_RT_NO_BEVEL"); no_bevel = e && *e && strcmp(e, "0") != 0; }
    return no_bevel;
}

static int rt_sky_vis_ray(void) { // KRUNKNATIVE_RT_SKY_VIS=1 (dev / opt-in): rtMode 1's per-pixel sky-visibility ray (rt.h)
    static int on = -1;
    if (on < 0) { const char *e = getenv("KRUNKNATIVE_RT_SKY_VIS"); on = e && *e && strcmp(e, "0") != 0; }
    return on;
}

static int rt_no_spec_occlusion(void) { // KRUNKNATIVE_RT_NO_SPEC_OCCLUSION=1 (dev only): the sky reflection unoccluded (A/B)
    static int no_so = -1;
    if (no_so < 0) { const char *e = getenv("KRUNKNATIVE_RT_NO_SPEC_OCCLUSION"); no_so = e && *e && strcmp(e, "0") != 0; }
    return no_so;
}

// One probe ray: the closest hit, stepping past transparent instances (Krunker's ambient_0/1 strips lie on
// every face, glass, liquids) up to 4 times. Closest hit, not rt_scene_occluded: the any-hit query starts in the
// self-shadow puppet's subtree, and the camera sits inside the puppet's head.
static int sky_probe_blocked(const float origin[3], const float dir[3], float t_max) {
    float o[3] = {origin[0], origin[1], origin[2]};
    for (int step = 0; step < 4 && t_max > 0.0f; step++) {
        RtHit hit;
        if (!rt_scene_trace(&g_rt.scene, o, dir, t_max, &hit) || hit.instance < 0) return 0;
        if (!(g_rt.scene.instances[hit.instance].flags & RT_INST_TRANSPARENT)) return 1;
        const float advance = hit.t + 0.05f;
        for (int k = 0; k < 3; k++) o[k] += dir[k] * advance;
        t_max -= advance;
    }
    return 0;
}

// The viewmodel's sky visibility (rt.h RT_EFFECT_SKY_PROBE): an "ambient cube" of six visibilities, along
// +x -x +y -y +z -z from the camera, each the open fraction of RT_SKY_PROBE_RAYS rays in a cosine lobe about
// its axis (golden-angle spiral, rotated each frame), closest hit through the CPU tracer (sky_probe_blocked: past
// transparent hits, never into the self-shadow puppet) within RT_SKY_VIS_RADIUS. basic.frag weighs
// the six by the squared components of each pixel's reflection direction, so a weapon face mirroring a wall
// next to the player goes dark while one mirroring open sky above keeps its sheen. Exponentially smoothed so
// walking under a roof fades instead of popping.
static void sky_probe_update(const Camera *camera) {
    const float origin[3] = {camera->position.x, camera->position.y, camera->position.z};
    const float golden = 2.39996323f;
    const float spin = (float) (g_rt.sky_probe_frame++ % 64u) * 0.61803398875f * 6.28318530718f;
    static const float axes[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    // CPU cost: RT_SKY_PROBE_PER_FRAME of the six axes a frame, round robin (all six on the first frame), each blended
    // with the rate that gives the same time constant as updating it every frame: 1 - (1 - blend)^(6 / per frame)
    // While the camera moves (walk, jump, bob) all six axes are traced every frame, so the weapon's sky occlusion
    // follows the motion instead of catching up over three frames; the round robin is for a still camera only.
    static float last_origin[3];
    const int camera_moved = fabsf(origin[0] - last_origin[0]) + fabsf(origin[1] - last_origin[1]) +
                             fabsf(origin[2] - last_origin[2]) > 0.01f;
    memcpy(last_origin, origin, sizeof(last_origin));
    const int first = !g_rt.sky_probe_valid;
    const int all = first || camera_moved;
    const int per_frame = all ? 6 : RT_SKY_PROBE_PER_FRAME;
    const int start = all ? 0 : (int) ((g_rt.sky_probe_frame * (unsigned) RT_SKY_PROBE_PER_FRAME) % 6u);
    const float blend = all && !first ? RT_SKY_PROBE_BLEND
                                      : 1.0f - powf(1.0f - RT_SKY_PROBE_BLEND, 6.0f / (float) RT_SKY_PROBE_PER_FRAME);
    for (int step = 0; step < per_frame; step++) {
        const int a = (start + step) % 6;
        const float *z = axes[a];
        // a basis about the axis
        const float up[3] = {fabsf(z[1]) < 0.9f ? 0.0f : 1.0f, fabsf(z[1]) < 0.9f ? 1.0f : 0.0f, 0.0f};
        float x[3] = {up[1] * z[2] - up[2] * z[1], up[2] * z[0] - up[0] * z[2], up[0] * z[1] - up[1] * z[0]};
        const float xl = sqrtf(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
        for (int k = 0; k < 3; k++) x[k] /= xl;
        const float y[3] = {z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]};
        int open = 0;
        for (int i = 0; i < RT_SKY_PROBE_RAYS; i++) {
            const float u = ((float) i + 0.5f) / (float) RT_SKY_PROBE_RAYS;
            const float sin_theta = sqrtf(u), cos_theta = sqrtf(1.0f - u); // cosine-weighted about the axis
            const float phi = (float) i * golden + spin;
            const float cp = cosf(phi) * sin_theta, sp = sinf(phi) * sin_theta;
            const float dir[3] = {x[0] * cp + y[0] * sp + z[0] * cos_theta, x[1] * cp + y[1] * sp + z[1] * cos_theta,
                                  x[2] * cp + y[2] * sp + z[2] * cos_theta};
            if (!sky_probe_blocked(origin, dir, RT_SKY_VIS_RADIUS)) open++;
        }
        const float now = (float) open / (float) RT_SKY_PROBE_RAYS;
        g_rt.sky_probe[a] = first ? now : g_rt.sky_probe[a] + (now - g_rt.sky_probe[a]) * blend;
    }
    g_rt.sky_probe_valid = 1;
}

// KRUNKNATIVE_RT_NO_GRADIENT=1 (dev only): the denoisers keep their pre-2026-11-03 history policy (no temporal-gradient
// clip of the AO / GI / reflection histories against this frame's 3x3 band; reflections reproject with the camera only)
static int rt_no_gradient(void) {
    static int no_gradient = -1;
    if (no_gradient < 0) { const char *e = getenv("KRUNKNATIVE_RT_NO_GRADIENT"); no_gradient = e && *e && strcmp(e, "0") != 0; }
    return no_gradient;
}

static int rt_no_motion(void) { // KRUNKNATIVE_RT_NO_MOTION=1 (dev only): no velocity target, camera-only reprojection (A/B)
    static int no_motion = -1;
    if (no_motion < 0) { const char *e = getenv("KRUNKNATIVE_RT_NO_MOTION"); no_motion = e && *e && strcmp(e, "0") != 0; }
    return no_motion;
}

static void ensure_velocity_target(const int width, const int height) {
    static int logged;
    if (rt_no_motion()) return;
    g_rt.velocity_texture = make_texture(width, height, GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, g_rt.prepass_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_2D, g_rt.velocity_texture, 0);
    prepass_draw_buffers();
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) return;
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_2D, 0, 0);
    prepass_draw_buffers();
    glDeleteTextures(1, &g_rt.velocity_texture);
    g_rt.velocity_texture = 0;
    if (!logged) {
        logged = 1;
        fprintf(stderr, "[rt] no third prepass attachment: motion vectors unavailable (camera-only reprojection)\n");
    }
}

// The local light target as the trace FBO's second attachment (rt_trace.frag location 1) and its
// history pair. An RGBA32F attachment is covered by the same EXT_color_buffer_float as the RGBA16F
// one on ES / WebGL2, but a driver may still refuse it: then the trace FBO keeps one attachment and
// local light shadows stay off (logged once). Nearest filtering: the channels hold packed integers.
static void ensure_local_targets(const int width, const int height) {
    static int logged;
    g_rt.local_supported = 0;
    g_rt.local_texture = make_texture(width, height, GL_RGBA32F, GL_RGBA, GL_FLOAT, 0);
    for (int i = 0; i < 2; i++) g_rt.local_history_texture[i] = make_texture(width, height, GL_RGBA32F, GL_RGBA, GL_FLOAT, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, g_rt.trace_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, g_rt.local_texture, 0);
    const unsigned int both[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
    glDrawBuffers(2, both);
    int ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    for (int i = 0; i < 2 && ok; i++) {
        g_rt.local_history_fbo[i] = make_fbo(g_rt.local_history_texture[i], 0);
        ok = ok && g_rt.local_history_fbo[i];
    }
    if (ok) {
        g_rt.local_supported = 1;
        return;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, g_rt.trace_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, 0, 0);
    glDrawBuffers(1, both);
    for (int i = 0; i < 2; i++) if (g_rt.local_history_fbo[i]) glDeleteFramebuffers(1, &g_rt.local_history_fbo[i]);
    glDeleteTextures(1, &g_rt.local_texture);
    glDeleteTextures(2, g_rt.local_history_texture);
    g_rt.local_history_fbo[0] = g_rt.local_history_fbo[1] = 0;
    g_rt.local_texture = g_rt.local_history_texture[0] = g_rt.local_history_texture[1] = 0;
    if (!logged) {
        logged = 1;
        fprintf(stderr, "[rt] no RGBA32F colour attachment: local light shadows unavailable\n");
    }
}

// The reflection material as the prepass FBO's second attachment (rt_prepass.frag location 1) and the
// reflection radiance / history / filter targets (RGBA16F). An incomplete attachment leaves the prepass
// single-target and reflections off (logged once). Called with RT_TEXTURE_UNIT active, like ensure_targets.
static void ensure_refl_targets(const int width, const int height) {
    delete_refl_targets();
    g_rt.material_texture = make_texture(width, height, GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, 0);
    g_rt.refl_texture = make_texture(width, height, GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, 1);
    for (int i = 0; i < 2; i++) {
        g_rt.refl_history_texture[i] = make_texture(width, height, GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, 1);
        g_rt.refl_filter_texture[i] = make_texture(width, height, GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, 1);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, g_rt.prepass_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, g_rt.material_texture, 0);
    const unsigned int both[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
    glDrawBuffers(2, both);
    int ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (!ok) {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, 0, 0);
        glDrawBuffers(1, both);
    }
    g_rt.material_attached = ok;
    g_rt.refl_fbo = make_fbo(g_rt.refl_texture, 0);
    ok = ok && g_rt.refl_fbo;
    for (int i = 0; i < 2; i++) {
        g_rt.refl_history_fbo[i] = make_fbo(g_rt.refl_history_texture[i], 0);
        g_rt.refl_filter_fbo[i] = make_fbo(g_rt.refl_filter_texture[i], 0);
        ok = ok && g_rt.refl_history_fbo[i] && g_rt.refl_filter_fbo[i];
    }
    if (!ok) {
        static int logged;
        if (!logged) logged = 1, fprintf(stderr, "[rt] reflection targets incomplete: reflections unavailable\n");
        delete_refl_targets();
    }
}

// (Re)allocates the targets for `width` x `height`; on the active texture unit RT_TEXTURE_UNIT.
static int ensure_targets(const int width, const int height) {
    if (g_rt.prepass_fbo && g_rt.width == width && g_rt.height == height) return 1;
    delete_targets();
    glActiveTexture(GL_TEXTURE0 + RT_TEXTURE_UNIT);
    g_rt.depth_texture = make_texture(width, height, GL_DEPTH_COMPONENT24, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, 0);
    g_rt.normal_texture = make_texture(width, height, GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, 0);
    g_rt.trace_texture = make_texture(width, height, GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, 1);
    for (int i = 0; i < 2; i++) {
        g_rt.history_texture[i] = make_texture(width, height, GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, 1);
        g_rt.filter_texture[i] = make_texture(width, height, GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, 1);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    g_rt.prepass_fbo = make_fbo(g_rt.normal_texture, g_rt.depth_texture);
    g_rt.trace_fbo = make_fbo(g_rt.trace_texture, 0);
    int ok = g_rt.prepass_fbo && g_rt.trace_fbo;
    for (int i = 0; i < 2; i++) {
        g_rt.history_fbo[i] = make_fbo(g_rt.history_texture[i], 0);
        g_rt.filter_fbo[i] = make_fbo(g_rt.filter_texture[i], 0);
        ok = ok && g_rt.history_fbo[i] && g_rt.filter_fbo[i];
    }
    if (!ok) {
        fprintf(stderr, "[rt] %dx%d targets incomplete: ray tracing unavailable\n", width, height);
        delete_targets();
        g_rt.supported = 0;
        return 0;
    }
    ensure_local_targets(width, height);
    ensure_refl_targets(width, height);
    ensure_gi_targets(width, height);
    ensure_velocity_target(width, height);
    g_rt.width = width;
    g_rt.height = height;
    g_rt.history_valid = 0;
    g_rt.history_epoch++;
    return 1;
}

// ---------------------------------------------------------------- passes

static GlUniform u_view_projection = GL_UNIFORM_INIT("view_projection");
static GlUniform u_inverse_view_projection = GL_UNIFORM_INIT("inverse_view_projection");
static GlUniform u_previous_view_projection = GL_UNIFORM_INIT("previous_view_projection");
static GlUniform u_camera_position = GL_UNIFORM_INIT("camera_position");
static GlUniform u_sun_direction = GL_UNIFORM_INIT("sun_direction");
static GlUniform u_sun_tan_radius = GL_UNIFORM_INIT("sun_tan_radius");
static GlUniform u_frame_index = GL_UNIFORM_INIT("frame_index");
static GlUniform u_rt_ao_samples = GL_UNIFORM_INIT("rt_ao_samples");
static GlUniform u_rt_ao_radius = GL_UNIFORM_INIT("rt_ao_radius");
static GlUniform u_rt_ao_min = GL_UNIFORM_INIT("rt_ao_min");
static GlUniform u_filter_mask = GL_UNIFORM_INIT("filter_mask");
static GlUniform u_rt_texel = GL_UNIFORM_INIT("rt_texel");
static GlUniform u_rt_depth = GL_UNIFORM_INIT("rt_depth");
static GlUniform u_rt_normal = GL_UNIFORM_INIT("rt_normal");
static GlUniform u_rt_self_root = GL_UNIFORM_INIT("rt_self_root"); // the shadow puppet's own TLAS subtree (rt.h self_tlas_root), -1 none
static GlUniform u_rt_atlas_ready = GL_UNIFORM_INIT("rt_atlas_ready"); // trace pass: 0 unless alpha-tested casters exist
static GlUniform u_rt_input = GL_UNIFORM_INIT("rt_input");
static GlUniform u_rt_history = GL_UNIFORM_INIT("rt_history");
static GlUniform u_pass = GL_UNIFORM_INIT("pass");
static GlUniform u_step_size = GL_UNIFORM_INIT("step_size");
static GlUniform u_history_valid = GL_UNIFORM_INIT("history_valid");
static GlUniform u_debug_trace = GL_UNIFORM_INIT("rt_trace_tex");
static GlUniform u_debug_normal = GL_UNIFORM_INIT("rt_normal");
static GlUniform u_debug_result = GL_UNIFORM_INIT("rt_result");
static GlUniform u_debug_mode = GL_UNIFORM_INIT("mode");
static GlUniform u_debug_local = GL_UNIFORM_INIT("rt_local");
static GlUniform u_debug_refl = GL_UNIFORM_INIT("rt_refl_tex"); // phase 4 (mode 7, unit 4)
// local light shadows (phase 5)
static GlUniform u_rt_local_count = GL_UNIFORM_INIT("rt_local_count");
static GlUniform u_rt_local_radius = GL_UNIFORM_INIT("rt_local_radius");
static GlUniform u_rt_local_pos[RT_LOCAL_MAX], u_rt_local_dir[RT_LOCAL_MAX], u_rt_local_params[RT_LOCAL_MAX];
static GlUniform u_rt_local_mask = GL_UNIFORM_INIT("rt_local_mask");
static GlUniform u_rt_local_min = GL_UNIFORM_INIT("rt_local_min");
static GlUniform u_rt_correct_lights = GL_UNIFORM_INIT("rt_correct_lights");
static GlUniform u_rt_history_main = GL_UNIFORM_INIT("rt_history_main");
// motion vectors (phase 6)
static GlUniform u_rt_velocity = GL_UNIFORM_INIT("rt_velocity");
static GlUniform u_rt_velocity_valid = GL_UNIFORM_INIT("rt_velocity_valid");
static GlUniform u_ao_gradient = GL_UNIFORM_INIT("rt_ao_gradient"); // rt_denoise.frag pass 0: the AO temporal-gradient clip
static GlUniform u_debug_velocity = GL_UNIFORM_INIT("rt_velocity");
static GlUniform u_debug_gi = GL_UNIFORM_INIT("rt_gi_tex"); // mode 9, unit 6

static void local_uniforms_init(void) {
    static char names[3][RT_LOCAL_MAX][24];
    if (u_rt_local_pos[0].name) return;
    for (int i = 0; i < RT_LOCAL_MAX; i++) {
        snprintf(names[0][i], sizeof(names[0][i]), "rt_local_pos[%d]", i);
        snprintf(names[1][i], sizeof(names[1][i]), "rt_local_dir[%d]", i);
        snprintf(names[2][i], sizeof(names[2][i]), "rt_local_params[%d]", i);
        u_rt_local_pos[i] = (GlUniform) GL_UNIFORM_INIT(names[0][i]);
        u_rt_local_dir[i] = (GlUniform) GL_UNIFORM_INIT(names[1][i]);
        u_rt_local_params[i] = (GlUniform) GL_UNIFORM_INIT(names[2][i]);
    }
}

// The traced slots that shoot rays this frame: all of them while no history can hold the others
// (first frame, reset, denoiser off) or when they fit; else a window of RT_LOCAL_PER_FRAME slots
// sliding with the frame index, so every slot is refreshed within count / PER_FRAME frames.
// Transient lights (BasicLight.transient: vfx.c muzzle-flash / explosion lights, rt.h RT_FLASH_*) sit after the
// map's lights (slots local_persistent..count-1) and are shot every frame.
static int local_frame_mask(const int count, const int denoise) {
    const int persistent = g_rt.local_persistent < count ? g_rt.local_persistent : count;
    if (persistent <= RT_LOCAL_PER_FRAME || !g_rt.history_valid || !denoise) return (1 << count) - 1;
    int mask = g_rt.local_fresh_mask;
    for (int k = 0; k < RT_LOCAL_PER_FRAME; k++) mask |= 1 << ((g_rt.frame_index * RT_LOCAL_PER_FRAME + k) % persistent);
    return mask;
}

// Picks this frame's traced lights: the point / spot lights of basic.frag's list (basic_lights_list,
// slot order) nearest to the camera, up to RT_LOCAL_MAX, and fills g_rt.local_index (slot -> traced
// index). Uploads them to the trace program (in use). Returns the count.
static int select_local_lights(const unsigned int program, const Camera *camera) {
    const ClientRenderSettings *s = client_render_settings();
    for (int i = 0; i < BASIC_MAX_LIGHTS; i++) g_rt.local_index[i] = -1;
    g_rt.local_count = 0;
    g_rt.local_persistent = g_rt.local_fresh_mask = 0;
    if (!s->rt_local_lights || !g_rt.local_supported) return 0;

    BasicLight lights[BASIC_MAX_LIGHTS];
    const int count = basic_lights_list(lights, BASIC_MAX_LIGHTS, s->lighting);
    int order[BASIC_MAX_LIGHTS], candidates = 0;
    float distance[BASIC_MAX_LIGHTS];
    // transient lights (vfx.c flashes) apart, nearest to the camera first: they take the traced slots after the
    // map lights (at most RT_FLASH_TRACE_MAX), so the map lights keep their slots and histories while a flash lives
    int transient_order[BASIC_MAX_LIGHTS], transients = 0;
    float transient_distance[BASIC_MAX_LIGHTS];
    for (int i = 0; i < count; i++) {
        if (lights[i].type != BASIC_LIGHT_POINT && lights[i].type != BASIC_LIGHT_SPOT) continue; // rect / directional: unshadowed
        if (lights[i].transient) {
            const float dx = lights[i].position.x - camera->position.x, dy = lights[i].position.y - camera->position.y, dz = lights[i].position.z - camera->position.z;
            int j = transients++;
            const float d = dx * dx + dy * dy + dz * dz;
            while (j > 0 && transient_distance[j - 1] > d) { transient_order[j] = transient_order[j - 1]; transient_distance[j] = transient_distance[j - 1]; j--; }
            transient_order[j] = i;
            transient_distance[j] = d;
            continue;
        }
        const float dx = lights[i].position.x - camera->position.x, dy = lights[i].position.y - camera->position.y, dz = lights[i].position.z - camera->position.z;
        distance[candidates] = dx * dx + dy * dy + dz * dz;
        order[candidates++] = i;
    }
    // insertion sort by camera distance (16 at most)
    for (int i = 1; i < candidates; i++) {
        const int slot = order[i];
        const float d = distance[i];
        int j = i - 1;
        while (j >= 0 && distance[j] > d) { order[j + 1] = order[j]; distance[j + 1] = distance[j]; j--; }
        order[j + 1] = slot;
        distance[j + 1] = d;
    }
    const int flash = transients < RT_FLASH_TRACE_MAX ? transients : RT_FLASH_TRACE_MAX;
    const int persistent = candidates < RT_LOCAL_MAX - flash ? candidates : RT_LOCAL_MAX - flash;
    for (int k = 0; k < flash; k++) order[persistent + k] = transient_order[k];
    const int traced = persistent + flash;
    g_rt.local_persistent = persistent;
    g_rt.local_fresh_mask = ((1 << traced) - 1) & ~((1 << persistent) - 1);
    local_uniforms_init();
    // lt_color as basic_upload_lights scales it: colour * intensity * (legacy ? PI : 1) * light_unit_factor
    const float scale = (g_render_lighting.correct_lights ? 1.0f : (float) M_PI) * ruleset_current()->light_unit_factor;
    for (int t = 0; t < traced; t++) {
        const BasicLight *light = &lights[order[t]];
        g_rt.local_index[order[t]] = t;
        const float range = light->distance > 0.0f ? light->distance : RT_LOCAL_RANGE_CAP;
        gl_uniform_4f(&u_rt_local_pos[t], program, light->position.x, light->position.y, light->position.z, range);
        float penumbra_cos = 0.0f;
        if (light->type == BASIC_LIGHT_SPOT) {
            vec3 d = {light->position.x - light->target.x, light->position.y - light->target.y, light->position.z - light->target.z};
            const float length = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
            if (length > 0.0f) { d.x /= length; d.y /= length; d.z /= length; } else d = (vec3) {0.0f, 1.0f, 0.0f};
            gl_uniform_4f(&u_rt_local_dir[t], program, d.x, d.y, d.z, cosf(light->angle)); // coneCos, as basic.c
            penumbra_cos = cosf(light->angle * (1.0f - light->penumbra));
        } else {
            gl_uniform_4f(&u_rt_local_dir[t], program, 0.0f, 1.0f, 0.0f, -2.0f);
        }
        const float luminance = (0.2126f * light->color.x + 0.7152f * light->color.y + 0.0722f * light->color.z) * light->intensity * scale;
        gl_uniform_4f(&u_rt_local_params[t], program, light->decay, penumbra_cos, luminance, 0.0f);
    }
    g_rt.local_count = traced;
    return traced;
}

static void use_program(const unsigned int program) {
    glUseProgram(program);
    g_active_shader = program;
}

static void bind_unit(const int unit, const unsigned int texture) {
    glActiveTexture(GL_TEXTURE0 + (unsigned int) unit);
    glBindTexture(GL_TEXTURE_2D, texture);
}

static void fullscreen(void) {
    glBindVertexArray(g_rt.vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

// prepass_begin / prepass_end bracket the per-mesh draws of prepass() (the traced instances) and of
// prepass_scene() (the first-person scene): the same targets, program and state
static void prepass_begin(const float view_projection[16], const float velocity_view_projection[16]) {
    glBindFramebuffer(GL_FRAMEBUFFER, g_rt.prepass_fbo);
    glViewport(0, 0, g_rt.width, g_rt.height);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClearDepthf(1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
#ifndef KN_GLES
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
#endif
    use_program(g_rt.prepass_program);
    gl_uniform_mat4(&u_view_projection, g_rt.prepass_program, GL_TRUE, view_projection);
    // motion vectors: the previous frame's camera (this one's on the first frame: the velocity is then
    // the mesh motion alone, and the denoiser has no history to reproject anyway)
    // TAA: both ends of the velocity delta are unjittered (velocity_view_projection is this frame's camera
    // without the sub-pixel jitter; previous_view_projection is stored unjittered), else the jitter sequence
    // would show up as motion
    glUniformMatrix4fv(g_rt.prepass_loc_previous_view_projection, 1, GL_TRUE, g_rt.history_valid ? g_rt.previous_view_projection : velocity_view_projection);
    glUniformMatrix4fv(g_rt.prepass_loc_velocity_view_projection, 1, GL_TRUE, velocity_view_projection);
    if (g_rt.prepass_loc_vertex_normal >= 0) glUniform1i(g_rt.prepass_loc_vertex_normal, g_rt.vm_running); //

    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

static void prepass_end(void) {
    glDisable(GL_CULL_FACE);
    glFrontFace(GL_CCW);
    glCullFace(GL_BACK);
    glBindVertexArray(0);
}

// Frustum planes (a, b, c, d: inside when a*x + b*y + c*z + d >= 0) of a row-major view-projection matrix
// (Gribb / Hartmann, as scene.c's), for prepass(): an instance whose world box lies wholly outside one plane
// covers no pixel of the prepass targets, so skipping its draw leaves them identical.
static void prepass_frustum_planes(const float *vp, float planes[6][4]) {
    for (int i = 0; i < 6; i++) {
        const int row = i / 2;
        const float sign = i % 2 ? -1.0f : 1.0f;
        for (int j = 0; j < 4; j++) planes[i][j] = vp[12 + j] + sign * vp[row * 4 + j];
    }
}

static int prepass_box_outside(const float planes[6][4], const float lo[3], const float hi[3]) {
    for (int i = 0; i < 6; i++) {
        const float *p = planes[i];
        // the box corner farthest along the plane normal: outside only if even that one is behind the plane
        const float x = p[0] >= 0.0f ? hi[0] : lo[0], y = p[1] >= 0.0f ? hi[1] : lo[1], z = p[2] >= 0.0f ? hi[2] : lo[2];
        if (p[0] * x + p[1] * y + p[2] * z + p[3] < 0.0f) return 1;
    }
    return 0;
}

static void prepass(const float view_projection[16], const float velocity_view_projection[16]) {
    prepass_begin(view_projection, velocity_view_projection);
    float planes[6][4];
    prepass_frustum_planes(view_projection, planes);
    // per-draw state is only sent when it differs from the previous draw's: culling, winding, the dynamic flag,
    // the material triple and the VAO (consecutive instances of one geometry, the unit cube above all, share it)
    int culling = -1, winding = -1;
    float dynamic_sent = -1.0f, material_sent[3] = {-1.0f, -1.0f, -1.0f};
    unsigned int vao_sent = 0;
    for (int i = 0; i < g_rt.scene.instance_count; i++) {
        const RtInstance *inst = &g_rt.scene.instances[i];
        const Mesh *mesh = inst->mesh;
        if (!mesh || !mesh->geometry || (inst->flags & (RT_INST_TRANSPARENT | RT_INST_SHADOW_ONLY))) continue; // the shadow proxy is a caster only (rt_self.c)
        if (prepass_box_outside(planes, inst->aabb_min, inst->aabb_max)) continue; // off screen: no pixel to write
        const int cull = !(inst->flags & RT_INST_DOUBLE_SIDED);
        if (cull != culling) {
            if (cull) glEnable(GL_CULL_FACE);
            else glDisable(GL_CULL_FACE);
            culling = cull;
        }
        if (cull) {
            const int cw = determinant3(mesh->transform_matrix) < 0.0f;
            if (cw != winding) { glFrontFace(cw ? GL_CW : GL_CCW); winding = cw; }
        }
        glUniformMatrix4fv(g_rt.prepass_loc_transform, 1, GL_TRUE, mesh->transform_matrix);
        const float dynamic = inst->flags & RT_INST_DYNAMIC ? 1.0f : 0.0f;
        if (dynamic != dynamic_sent) { glUniform1f(g_rt.prepass_loc_dynamic, dynamic); dynamic_sent = dynamic; }
        if (g_rt.prepass_loc_material >= 0) { // phase 4: the pixel's reflection material (0 = none)
            const RtMaterial *rm = inst->material >= 0 && inst->material < g_rt.scene.material_count ? &g_rt.scene.materials[inst->material] : NULL;
            const float m[3] = {rm && g_rt.refl_active ? rm->reflect_intensity : 0.0f, rm ? rm->roughness : 1.0f, rm ? rm->metalness : 0.0f};
            if (m[0] != material_sent[0] || m[1] != material_sent[1] || m[2] != material_sent[2]) {
                glUniform3f(g_rt.prepass_loc_material, m[0], m[1], m[2]);
                memcpy(material_sent, m, sizeof(m));
            }
        }
        // the previous build's pose (RtInstance.prev_local_to_world, rows 0..2) as a full 4x4
        float prev_transform[16];
        memcpy(prev_transform, inst->prev_local_to_world, 12 * sizeof(float));
        prev_transform[12] = prev_transform[13] = prev_transform[14] = 0.0f;
        prev_transform[15] = 1.0f;
        glUniformMatrix4fv(g_rt.prepass_loc_prev_transform, 1, GL_TRUE, prev_transform);
        if (mesh->geometry->vao != vao_sent) {
            glBindVertexArray(mesh->geometry->vao);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh->geometry->ebo);
            vao_sent = mesh->geometry->vao;
        }
        glDrawElements(GL_TRIANGLES, mesh->geometry->index_count, GL_UNSIGNED_INT, NULL);
    }
    prepass_end();
}

static void trace(const float inverse_view_projection[16], const Camera *camera, const int effects) {
    const ClientRenderSettings *s = client_render_settings();
    glBindFramebuffer(GL_FRAMEBUFFER, g_rt.trace_fbo);
    glDisable(GL_DEPTH_TEST);
    const unsigned int program = g_rt.trace_program;
    use_program(program);
    rt_gpu_bind(&g_rt.gpu, program, 0); // units 0..4
    gl_uniform_1i(&u_rt_self_root, program, g_rt.scene.self_tlas_root);
    bind_unit(5, g_rt.depth_texture);
    bind_unit(6, g_rt.normal_texture);
    gl_uniform_1i(&u_rt_depth, program, 5);
    gl_uniform_1i(&u_rt_normal, program, 6);
    // alpha-tested casters (chain-link, cut-out user assets): the atlas on units 7 / 8 lets the any-hit rays
    // through their cut-outs (rt_common.glsl rt_alpha_passes); without any, rt_atlas_ready 0 keeps the old cost
    if (g_rt.scene.alpha_test_count > 0) rt_atlas_bind(program, 7, 8);
    else gl_uniform_1i(&u_rt_atlas_ready, program, 0);
    gl_uniform_mat4(&u_inverse_view_projection, program, GL_TRUE, inverse_view_projection);
    gl_uniform_3f(&u_camera_position, program, camera->position.x, camera->position.y, camera->position.z);
    vec3 sun = g_render_lighting.light_direction;
    const float length = sqrtf(sun.x * sun.x + sun.y * sun.y + sun.z * sun.z);
    if (length > 0.0f) { sun.x /= length; sun.y /= length; sun.z /= length; }
    gl_uniform_3f(&u_sun_direction, program, sun.x, sun.y, sun.z);
    gl_uniform_1f(&u_sun_tan_radius, program, tanf(s->rt_sun_radius * (float) M_PI / 180.0f));
    gl_uniform_1i(&u_frame_index, program, g_rt.frame_index);
    int ao_samples = effects & RT_EFFECT_AO ? s->rt_samples : 0;
    if (ao_samples > 8) ao_samples = 8;
    gl_uniform_1i(&u_rt_ao_samples, program, ao_samples);
    gl_uniform_1f(&u_rt_ao_radius, program, (effects & RT_EFFECT_CINEMATIC) ? RT_CINE_AO_RADIUS : RT_AO_RADIUS);
    gl_uniform_1f(&u_rt_ao_min, program, RT_AO_MIN);
    {
        // AO split over frames (rt_trace.frag rt_ao_split): half the AO rays per pixel each frame, the temporal history
        // adds them up; needs the denoiser. KRUNKNATIVE_RT_AO_FULL=1 (dev) traces all of them every frame (A/B).
        static GlUniform u_rt_ao_split = GL_UNIFORM_INIT("rt_ao_split");
        static int ao_full = -1;
        if (ao_full < 0) ao_full = getenv("KRUNKNATIVE_RT_AO_FULL") != NULL;
        gl_uniform_1i(&u_rt_ao_split, program, ao_samples > 1 && s->rt_denoise && !ao_full);
    }
    {
        static GlUniform u_rt_sky_vis = GL_UNIFORM_INIT("rt_sky_vis");
        static GlUniform u_rt_sky_vis_radius = GL_UNIFORM_INIT("rt_sky_vis_radius");
        gl_uniform_1i(&u_rt_sky_vis, program, (effects & RT_EFFECT_SKY_VIS) && !ao_samples ? (s->rt_denoise ? 2 : 1) : 0);
        gl_uniform_1f(&u_rt_sky_vis_radius, program, RT_SKY_VIS_RADIUS);
    }
    gl_uniform_2f(&u_rt_texel, program, 1.0f / (float) g_rt.width, 1.0f / (float) g_rt.height);
    // local light shadows (phase 5): the lights were selected before the effects were decided
    const int local_count = effects & RT_EFFECT_LOCAL_LIGHTS ? g_rt.local_count : 0;
    g_rt.local_mask = local_count ? local_frame_mask(local_count, s->rt_denoise) : 0;
    gl_uniform_1i(&u_rt_local_count, program, local_count);
    gl_uniform_1f(&u_rt_local_radius, program, RT_LOCAL_LIGHT_RADIUS);
    gl_uniform_1i(&u_rt_local_mask, program, g_rt.local_mask);
    gl_uniform_1f(&u_rt_local_min, program, RT_LOCAL_MIN);
    gl_uniform_1i(&u_rt_correct_lights, program, g_render_lighting.correct_lights ? 1 : 0);
    fullscreen();
}

static void denoise(const float view_projection[16], const float inverse_view_projection[16], const Camera *camera, const int effects) {
    const unsigned int program = g_rt.denoise_program;
    use_program(program);
    bind_unit(2, g_rt.depth_texture);
    bind_unit(3, g_rt.normal_texture);
    gl_uniform_1i(&u_rt_input, program, 0);
    gl_uniform_1i(&u_rt_history, program, 1);
    gl_uniform_1i(&u_rt_depth, program, 2);
    gl_uniform_1i(&u_rt_normal, program, 3);
    gl_uniform_mat4(&u_view_projection, program, GL_TRUE, view_projection);
    gl_uniform_mat4(&u_inverse_view_projection, program, GL_TRUE, inverse_view_projection);
    gl_uniform_mat4(&u_previous_view_projection, program, GL_TRUE, g_rt.history_valid ? g_rt.previous_view_projection : view_projection);
    gl_uniform_3f(&u_camera_position, program, camera->position.x, camera->position.y, camera->position.z);
    gl_uniform_2f(&u_rt_texel, program, 1.0f / (float) g_rt.width, 1.0f / (float) g_rt.height);
    gl_uniform_1i(&u_history_valid, program, g_rt.history_valid);
    // motion vectors (phase 6): the prepass velocity on unit 5; without the target the shader
    // reprojects with the camera alone as before
    bind_unit(5, g_rt.velocity_texture);
    gl_uniform_1i(&u_rt_velocity, program, 5);
    gl_uniform_1i(&u_rt_velocity_valid, program, g_rt.velocity_texture ? 1 : 0); // 0 under KRUNKNATIVE_RT_NO_MOTION (no target)
    gl_uniform_1i(&u_ao_gradient, program, !rt_no_gradient());

    // temporal: trace + history[read] -> history[write]
    const int read = g_rt.history_index, write = 1 - read;
    glBindFramebuffer(GL_FRAMEBUFFER, g_rt.history_fbo[write]);
    bind_unit(0, g_rt.trace_texture);
    bind_unit(1, g_rt.history_texture[read]);
    gl_uniform_1i(&u_pass, program, 0);
    gl_uniform_1i(&u_step_size, program, 1);
    fullscreen();

    // local lights (phase 5): temporal only, on the packed target, validated against the main
    // history of the previous frame (history[read] still holds it)
    g_rt.local_result_texture = g_rt.local_texture;
    if (effects & RT_EFFECT_LOCAL_LIGHTS) {
        glBindFramebuffer(GL_FRAMEBUFFER, g_rt.local_history_fbo[write]);
        bind_unit(0, g_rt.local_texture);
        bind_unit(1, g_rt.local_history_texture[read]);
        bind_unit(4, g_rt.history_texture[read]);
        gl_uniform_1i(&u_rt_history_main, program, 4);
        gl_uniform_1i(&u_rt_local_mask, program, g_rt.local_mask); // the slots not traced this frame keep their history
        {
            static GlUniform u_rt_local_fresh = GL_UNIFORM_INIT("rt_local_fresh");
            gl_uniform_1i(&u_rt_local_fresh, program, g_rt.local_fresh_mask); // transient (flash) slots: no history
        }
        gl_uniform_1i(&u_pass, program, 2);
        fullscreen();
        g_rt.local_result_texture = g_rt.local_history_texture[write];
    }
    g_rt.history_index = write;
    g_rt.history_valid = 1;

    // spatial: two a-trous steps on R and G, then a third, wider one on the AO alone (it is low
    // frequency; the shadow edges would smear)
    gl_uniform_1i(&u_pass, program, 1);
    gl_uniform_2f(&u_filter_mask, program, 1.0f, 1.0f);
    glBindFramebuffer(GL_FRAMEBUFFER, g_rt.filter_fbo[0]);
    bind_unit(0, g_rt.history_texture[write]);
    gl_uniform_1i(&u_step_size, program, 1);
    fullscreen();
    glBindFramebuffer(GL_FRAMEBUFFER, g_rt.filter_fbo[1]);
    bind_unit(0, g_rt.filter_texture[0]);
    gl_uniform_1i(&u_step_size, program, 2);
    fullscreen();
    g_rt.result_texture = g_rt.filter_texture[1];
    if (effects & RT_EFFECT_AO) {
        gl_uniform_2f(&u_filter_mask, program, 0.0f, 1.0f);
        glBindFramebuffer(GL_FRAMEBUFFER, g_rt.filter_fbo[0]);
        bind_unit(0, g_rt.filter_texture[1]);
        gl_uniform_1i(&u_step_size, program, 4);
        fullscreen();
        g_rt.result_texture = g_rt.filter_texture[0];
    }
}

// ---------------------------------------------------------------- phase 4: reflections

static GlUniform u_refl_inverse_view_projection = GL_UNIFORM_INIT("inverse_view_projection");
static GlUniform u_refl_previous_view_projection = GL_UNIFORM_INIT("previous_view_projection");
static GlUniform u_refl_camera_position = GL_UNIFORM_INIT("camera_position");
static GlUniform u_refl_sun_direction = GL_UNIFORM_INIT("sun_direction");
static GlUniform u_refl_ambient_light = GL_UNIFORM_INIT("ambient_light");
static GlUniform u_refl_sun_light = GL_UNIFORM_INIT("sun_light");
static GlUniform u_refl_sky_color = GL_UNIFORM_INIT("sky_color");
static GlUniform u_refl_fog_color = GL_UNIFORM_INIT("fog_color");
static GlUniform u_refl_fog_near = GL_UNIFORM_INIT("fog_near");
static GlUniform u_refl_fog_far = GL_UNIFORM_INIT("fog_far");
static GlUniform u_refl_frame_index = GL_UNIFORM_INIT("frame_index");
static GlUniform u_refl_texel = GL_UNIFORM_INIT("rt_texel");
static GlUniform u_refl_depth = GL_UNIFORM_INIT("rt_depth");
static GlUniform u_refl_normal = GL_UNIFORM_INIT("rt_normal");
static GlUniform u_refl_material = GL_UNIFORM_INIT("rt_material");
static GlUniform u_refl_input = GL_UNIFORM_INIT("rt_input");
static GlUniform u_refl_history = GL_UNIFORM_INIT("rt_history");
static GlUniform u_refl_velocity = GL_UNIFORM_INIT("rt_velocity");
static GlUniform u_refl_velocity_valid = GL_UNIFORM_INIT("rt_velocity_valid");
static GlUniform u_refl_gradient = GL_UNIFORM_INIT("rt_refl_gradient");
static GlUniform u_refl_depth_history = GL_UNIFORM_INIT("rt_depth_history");
static GlUniform u_refl_pass = GL_UNIFORM_INIT("pass");
static GlUniform u_refl_step_size = GL_UNIFORM_INIT("step_size");
static GlUniform u_refl_history_valid = GL_UNIFORM_INIT("history_valid");
static GlUniform u_refl_debug = GL_UNIFORM_INIT("refl_debug");
static GlUniform u_refl_normal_only = GL_UNIFORM_INIT("rt_prepass_normal_only"); // 1 inside rt_viewmodel_frame
static GlUniform u_refl_hit_self = GL_UNIFORM_INIT("rt_hit_shadow_only");       // 1: the rays may hit the shadow puppet (unused: always 0, see the upload)

// 1 when reflections can run: basic.frag gets a 17th sampler (rt_refl on unit 16; OpenGL ES 3.0 guarantees
// only 16 per fragment stage), the two programs compiled and the material attachment completed.
static int refl_ready(void) {
    if (!g_rt.refl_checked) {
        g_rt.refl_checked = 1;
        int units = 0;
        glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &units);
        if (units < RT_REFL_TEXTURE_UNIT + 1) {
            fprintf(stderr, "[rt] %d fragment texture units (< %d): reflections unavailable on this GL\n", units, RT_REFL_TEXTURE_UNIT + 1);
            return 0;
        }
        g_rt.refl_program = compile("shaders/scene_composite.vert", "shaders/rt_reflect.frag");
        g_rt.refl_denoise_program = compile("shaders/scene_composite.vert", "shaders/rt_refl_denoise.frag");
        if (!g_rt.refl_program || !g_rt.refl_denoise_program) {
            fprintf(stderr, "[rt] reflection shaders failed to compile: reflections off for this run\n");
            return 0;
        }
        g_rt.refl_supported = 1;
    }
    return g_rt.refl_supported && g_rt.material_attached && g_rt.refl_fbo;
}

// rt_reflect.frag into refl_texture: units 0..4 the tables, 5 depth, 6 normal, 7 atlas, 8 atlas means, 9 material
static void rt_sun_map_bind(unsigned int program, int unit);
static void reflect_pass(const float inverse_view_projection[16], const Camera *camera) {
    rt_atlas_update(g_rt.vao); // the textures that joined the atlas since the last frame
    const unsigned int program = g_rt.refl_program;
    glBindFramebuffer(GL_FRAMEBUFFER, g_rt.refl_fbo);
    glViewport(0, 0, g_rt.width, g_rt.height);
    glDisable(GL_DEPTH_TEST);
    use_program(program);
    rt_gpu_bind(&g_rt.gpu, program, 0);
    gl_uniform_1i(&u_rt_self_root, program, g_rt.scene.self_tlas_root);
    bind_unit(5, g_rt.depth_texture);
    bind_unit(6, g_rt.normal_texture);
    rt_atlas_bind(program, 7, 8);
    bind_unit(9, g_rt.material_texture);
    gl_uniform_1i(&u_refl_depth, program, 5);
    gl_uniform_1i(&u_refl_normal, program, 6);
    gl_uniform_1i(&u_refl_material, program, 9);
    gl_uniform_mat4(&u_refl_inverse_view_projection, program, GL_TRUE, inverse_view_projection);
    gl_uniform_3f(&u_refl_camera_position, program, camera->position.x, camera->position.y, camera->position.z);
    const RenderLighting *l = &g_render_lighting;
    vec3 sun = l->light_direction;
    const float length = sqrtf(sun.x * sun.x + sun.y * sun.y + sun.z * sun.z);
    if (length > 0.0f) { sun.x /= length; sun.y /= length; sun.z /= length; }
    gl_uniform_3f(&u_refl_sun_direction, program, sun.x, sun.y, sun.z);
    const float light_scale = l->correct_lights ? 1.0f : (float) M_PI; // basic.frag krunker_lambert
    gl_uniform_3f(&u_refl_ambient_light, program, l->ambient_color.x * l->ambient_intensity * light_scale,
                  l->ambient_color.y * l->ambient_intensity * light_scale, l->ambient_color.z * l->ambient_intensity * light_scale);
    gl_uniform_3f(&u_refl_sun_light, program, l->light_color.x * l->light_intensity * light_scale,
                  l->light_color.y * l->light_intensity * light_scale, l->light_color.z * l->light_intensity * light_scale);
    gl_uniform_3f(&u_refl_sky_color, program, l->sky_color.x, l->sky_color.y, l->sky_color.z);
    sky_physical_uniforms(program, 1); // rtSky (psky_on 0 while it is off)
    gl_uniform_3f(&u_refl_fog_color, program, l->fog_color.x, l->fog_color.y, l->fog_color.z);
    gl_uniform_1f(&u_refl_fog_near, program, l->fog_near);
    gl_uniform_1f(&u_refl_fog_far, program, l->fog_enabled ? l->fog_far : 0.0f);
    gl_uniform_1i(&u_refl_frame_index, program, g_rt.frame_index);
    gl_uniform_2f(&u_refl_texel, program, 1.0f / (float) g_rt.width, 1.0f / (float) g_rt.height);
    static int refl_debug = -1; // KRUNKNATIVE_RT_REFL_DEBUG=<1..4> (dev): rt_reflect.frag shows the hit albedo / normal / length / hit-miss
    if (refl_debug < 0) { const char *d = getenv("KRUNKNATIVE_RT_REFL_DEBUG"); refl_debug = d ? atoi(d) : 0; }
    gl_uniform_1i(&u_refl_debug, program, refl_debug);
    gl_uniform_1i(&u_refl_normal_only, program, g_rt.vm_running);
    // 0 for the viewmodel too: the shadow puppet's arms and rifle stand where the first-person weapon is, in the
    // third-person pose, so the weapon's rays started inside them and reflected that dark geometry (the gun went
    // near-black at rtMode 3). Krunker shows no body in first person, so the weapon reflects the world only.
    gl_uniform_1i(&u_refl_hit_self, program, 0);
    rt_sun_map_bind(program, 11);
    fullscreen();
    bind_unit(11, 0);
    for (int unit = 9; unit >= 7; unit--) bind_unit(unit, 0); // restore_state clears 0..6
}

// rt_refl_denoise.frag: temporal (against the main denoiser's previous history for the surface test), then
// two a-trous steps scaled by roughness. `main_previous` is the main history the temporal pass just read.
static void refl_denoise(const float inverse_view_projection[16], const Camera *camera, const unsigned int main_previous, const int history_was_valid) {
    const unsigned int program = g_rt.refl_denoise_program;
    use_program(program);
    bind_unit(2, g_rt.depth_texture);
    bind_unit(3, g_rt.normal_texture);
    bind_unit(4, g_rt.material_texture);
    bind_unit(5, main_previous);
    gl_uniform_1i(&u_refl_input, program, 0);
    gl_uniform_1i(&u_refl_history, program, 1);
    gl_uniform_1i(&u_refl_depth, program, 2);
    gl_uniform_1i(&u_refl_normal, program, 3);
    gl_uniform_1i(&u_refl_material, program, 4);
    gl_uniform_1i(&u_refl_depth_history, program, 5);
    gl_uniform_mat4(&u_refl_inverse_view_projection, program, GL_TRUE, inverse_view_projection);
    gl_uniform_mat4(&u_refl_previous_view_projection, program, GL_TRUE, g_rt.previous_view_projection);
    gl_uniform_3f(&u_refl_camera_position, program, camera->position.x, camera->position.y, camera->position.z);
    gl_uniform_2f(&u_refl_texel, program, 1.0f / (float) g_rt.width, 1.0f / (float) g_rt.height);
    gl_uniform_1i(&u_refl_history_valid, program, history_was_valid && g_rt.refl_history_valid);
    // motion vectors (the world pass only: the viewmodel's target set has no velocity attachment, its prepass writes none)
    {
        const int velocity = g_rt.velocity_texture && !g_rt.vm_running && !rt_no_gradient();
        bind_unit(6, velocity ? g_rt.velocity_texture : g_rt.depth_texture); // a float 2D texture either way (ES sampler rules)
        gl_uniform_1i(&u_refl_velocity, program, 6);
        gl_uniform_1i(&u_refl_velocity_valid, program, velocity);
        gl_uniform_1i(&u_refl_gradient, program, !rt_no_gradient());
    }

    const int read = g_rt.refl_history_index, write = 1 - read;
    glBindFramebuffer(GL_FRAMEBUFFER, g_rt.refl_history_fbo[write]);
    bind_unit(0, g_rt.refl_texture);
    bind_unit(1, g_rt.refl_history_texture[read]);
    gl_uniform_1i(&u_refl_pass, program, 0);
    gl_uniform_1i(&u_refl_step_size, program, 1);
    fullscreen();
    g_rt.refl_history_index = write;
    g_rt.refl_history_valid = 1;

    gl_uniform_1i(&u_refl_pass, program, 1);
    glBindFramebuffer(GL_FRAMEBUFFER, g_rt.refl_filter_fbo[0]);
    bind_unit(0, g_rt.refl_history_texture[write]);
    gl_uniform_1i(&u_refl_step_size, program, 1);
    fullscreen();
    glBindFramebuffer(GL_FRAMEBUFFER, g_rt.refl_filter_fbo[1]);
    bind_unit(0, g_rt.refl_filter_texture[0]);
    gl_uniform_1i(&u_refl_step_size, program, 2);
    fullscreen();
    g_rt.refl_result_texture = g_rt.refl_filter_texture[1];
}

// ---------------------------------------------------------------- one-bounce diffuse GI (rt.h RT_EFFECT_GI)

static GlUniform u_gi_inverse_view_projection = GL_UNIFORM_INIT("inverse_view_projection");
static GlUniform u_gi_previous_view_projection = GL_UNIFORM_INIT("previous_view_projection");
static GlUniform u_gi_camera_position = GL_UNIFORM_INIT("camera_position");
static GlUniform u_gi_sun_direction = GL_UNIFORM_INIT("sun_direction");
static GlUniform u_gi_ambient_light = GL_UNIFORM_INIT("ambient_light");
static GlUniform u_gi_sun_light = GL_UNIFORM_INIT("sun_light");
static GlUniform u_gi_sky_color = GL_UNIFORM_INIT("sky_color");
static GlUniform u_gi_fog_color = GL_UNIFORM_INIT("fog_color");
static GlUniform u_gi_fog_near = GL_UNIFORM_INIT("fog_near");
static GlUniform u_gi_fog_far = GL_UNIFORM_INIT("fog_far");
static GlUniform u_gi_emissive = GL_UNIFORM_INIT("rt_gi_emissive");
static GlUniform u_gi_sky_mix = GL_UNIFORM_INIT("rt_gi_sky_mix");
static GlUniform u_gi_frame_index = GL_UNIFORM_INIT("frame_index");
static GlUniform u_gi_texel = GL_UNIFORM_INIT("rt_texel");
static GlUniform u_gi_depth = GL_UNIFORM_INIT("rt_depth");
static GlUniform u_gi_normal = GL_UNIFORM_INIT("rt_normal");
static GlUniform u_gi_input = GL_UNIFORM_INIT("rt_input");
static GlUniform u_gi_history = GL_UNIFORM_INIT("rt_history");
static GlUniform u_gi_depth_history = GL_UNIFORM_INIT("rt_depth_history");
static GlUniform u_gi_velocity = GL_UNIFORM_INIT("rt_velocity");
static GlUniform u_gi_velocity_valid = GL_UNIFORM_INIT("rt_velocity_valid");
static GlUniform u_gi_gradient = GL_UNIFORM_INIT("rt_gi_gradient"); // rt_gi_denoise.frag pass 0: the temporal-gradient clip
static GlUniform u_gi_pass = GL_UNIFORM_INIT("pass");
static GlUniform u_gi_step_size = GL_UNIFORM_INIT("step_size");
static GlUniform u_gi_history_valid = GL_UNIFORM_INIT("history_valid");
static GlUniform u_gi_normal_only = GL_UNIFORM_INIT("rt_prepass_normal_only"); // 1 inside rt_viewmodel_frame
static GlUniform u_gi_hit_self = GL_UNIFORM_INIT("rt_hit_shadow_only");       // 1: the rays may hit the shadow puppet (unused: always 0, see the upload)
static GlUniform u_gi_far = GL_UNIFORM_INIT("rt_gi_far");                      // the bounce ray's reach (RT_VM_GI_FAR for the viewmodel)
#define RT_VM_GI_FAR 400.0f // the viewmodel's bounce rays: the room around the player, not the map's far end (rt_gi.frag rt_gi_far)

static int rt_no_gi(void) { // KRUNKNATIVE_RT_NO_GI=1 (dev only): mode 2 without the bounce (A/B)
    static int no_gi = -1;
    if (no_gi < 0) { const char *e = getenv("KRUNKNATIVE_RT_NO_GI"); no_gi = e && *e && strcmp(e, "0") != 0; }
    return no_gi;
}

// ---- emissive lights (rt.h "emissive lights"): the GI pass samples them directly (rt_gi.frag emissive_direct)
static RtEmissiveTable g_emissive;
static RtEmissiveLight g_emissive_lights[RT_EMISSIVE_MAX];
static int g_emissive_logged;
static GlUniform u_gi_emissive_table = GL_UNIFORM_INIT("rt_emissive");
static GlUniform u_gi_emissive_count = GL_UNIFORM_INIT("rt_emissive_count");

// rt_frame, after rt_scene_build: this frame's list around the camera, uploaded (only while GI runs)
static void emissive_lights_frame(const Camera *camera, const int gi) {
    if (!gi) { g_emissive.count = 0; return; }
    const float eye[3] = {camera->position.x, camera->position.y, camera->position.z};
    const int n = rt_scene_emissive_lights(&g_rt.scene, eye, g_emissive_lights, RT_EMISSIVE_MAX);
    rt_gpu_upload_emissive(&g_emissive, g_emissive_lights, n);
    if (!g_emissive_logged) {
        g_emissive_logged = 1;
        fprintf(stderr, "[rt] emissive lights: %d sampled by the GI pass (of up to %d)\n", n, RT_EMISSIVE_MAX);
        if (getenv("KRUNKNATIVE_RT_EMISSIVE_LOG")) for (int i = 0; i < n; i++) { const RtEmissiveLight *l = &g_emissive_lights[i];
            fprintf(stderr, "[rt]   light %d inst %d at (%.0f %.0f %.0f) half (%.1f %.1f %.1f) Le (%.2f %.2f %.2f)\n", i, l->instance, l->center[0], l->center[1], l->center[2],
                    sqrtf(l->axis[0][0]*l->axis[0][0]+l->axis[0][1]*l->axis[0][1]+l->axis[0][2]*l->axis[0][2]), sqrtf(l->axis[1][0]*l->axis[1][0]+l->axis[1][1]*l->axis[1][1]+l->axis[1][2]*l->axis[1][2]),
                    sqrtf(l->axis[2][0]*l->axis[2][0]+l->axis[2][1]*l->axis[2][1]+l->axis[2][2]*l->axis[2][2]), l->radiance[0], l->radiance[1], l->radiance[2]); }
    }
}

static void emissive_lights_bind(const unsigned int program) {
    bind_unit(RT_EMISSIVE_UNIT, g_emissive.texture);
    gl_uniform_1i(&u_gi_emissive_table, program, RT_EMISSIVE_UNIT);
    gl_uniform_1i(&u_gi_emissive_count, program, g_emissive.texture ? g_emissive.count : 0);
}

// 1 when GI can run: basic.frag gets an 18th sampler (rt_gi on unit 17), the two programs compiled, the targets exist.
static int gi_ready(void) {
    if (rt_no_gi()) return 0;
    if (!g_rt.gi_checked) {
        g_rt.gi_checked = 1;
        int units = 0;
        glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &units);
        if (units < RT_GI_TEXTURE_UNIT + 1) {
            fprintf(stderr, "[rt] %d fragment texture units (< %d): global illumination unavailable on this GL\n", units, RT_GI_TEXTURE_UNIT + 1);
            return 0;
        }
        g_rt.gi_program = compile("shaders/scene_composite.vert", "shaders/rt_gi.frag");
        g_rt.gi_denoise_program = compile("shaders/scene_composite.vert", "shaders/rt_gi_denoise.frag");
        if (!g_rt.gi_program || !g_rt.gi_denoise_program) {
            fprintf(stderr, "[rt] GI shaders failed to compile: global illumination off for this run\n");
            return 0;
        }
        g_rt.gi_supported = 1;
    }
    return g_rt.gi_supported && g_rt.gi_fbo;
}

// rt_gi.frag into gi_texture: units 0..4 the tables, 5 depth, 6 normal, 7 atlas, 8 atlas means (as reflect_pass)
// The sun shadow map (shadow.c) as the sun visibility at GI / reflection hits (rt_gi.frag / rt_reflect.frag map_sun):
// bound on `unit` when a map is active this frame, else rt_sun_map_on 0 (they fall back to screen reuse / a ray).
// KRUNKNATIVE_RT_HIT_SUN_RAY=1 (dev) keeps the traced ray (A/B).
static void rt_sun_map_bind(const unsigned int program, const int unit) {
    static GlUniform u_on = GL_UNIFORM_INIT("rt_sun_map_on");
    static GlUniform u_map = GL_UNIFORM_INIT("rt_sun_map");
    static GlUniform u_matrix = GL_UNIFORM_INIT("rt_sun_matrix");
    static GlUniform u_bias = GL_UNIFORM_INIT("rt_sun_bias");
    static int ray = -1;
    if (ray < 0) ray = getenv("KRUNKNATIVE_RT_HIT_SUN_RAY") != NULL;
    unsigned int texture = 0;
    float matrix[16], bias = 0.0f;
    const int on = !ray && shadow_sun_map(&texture, matrix, &bias);
    gl_uniform_1i(&u_on, program, on);
    if (!on) return;
    bind_unit(unit, texture);
    gl_uniform_1i(&u_map, program, unit);
    gl_uniform_mat4(&u_matrix, program, GL_TRUE, matrix);
    gl_uniform_1f(&u_bias, program, bias);
}

static void gi_pass(const float inverse_view_projection[16], const Camera *camera) {
    rt_atlas_update(g_rt.vao); // the hit albedo reads the atlas (a no-op when nothing joined)
    const unsigned int program = g_rt.gi_program;
    glBindFramebuffer(GL_FRAMEBUFFER, g_rt.gi_fbo);
    glViewport(0, 0, g_rt.gi_width, g_rt.gi_height); // half the RT resolution (RT_GI_DIVISOR)
    glDisable(GL_DEPTH_TEST);
    use_program(program);
    rt_gpu_bind(&g_rt.gpu, program, 0);
    gl_uniform_1i(&u_rt_self_root, program, g_rt.scene.self_tlas_root);
    bind_unit(5, g_rt.depth_texture);
    bind_unit(6, g_rt.normal_texture);
    rt_atlas_bind(program, 7, 8);
    emissive_lights_bind(program); // unit 9: the emissive light list
    gl_uniform_1i(&u_gi_depth, program, 5);
    gl_uniform_1i(&u_gi_normal, program, 6);
    gl_uniform_mat4(&u_gi_inverse_view_projection, program, GL_TRUE, inverse_view_projection);
    gl_uniform_3f(&u_gi_camera_position, program, camera->position.x, camera->position.y, camera->position.z);
    const RenderLighting *l = &g_render_lighting;
    vec3 sun = l->light_direction;
    const float length = sqrtf(sun.x * sun.x + sun.y * sun.y + sun.z * sun.z);
    if (length > 0.0f) { sun.x /= length; sun.y /= length; sun.z /= length; }
    gl_uniform_3f(&u_gi_sun_direction, program, sun.x, sun.y, sun.z);
    const float light_scale = l->correct_lights ? 1.0f : (float) M_PI; // basic.frag krunker_lambert
    const int cine = (g_rt.effects & RT_EFFECT_CINEMATIC) != 0; // Cinematic: the hits' ambient at the fill level (rt.h)
    const float amb_k = light_scale * (cine ? RT_CINE_FILL : 1.0f);
    gl_uniform_3f(&u_gi_ambient_light, program, l->ambient_color.x * l->ambient_intensity * amb_k,
                  l->ambient_color.y * l->ambient_intensity * amb_k, l->ambient_color.z * l->ambient_intensity * amb_k);
    {
        static GlUniform u_gi_cine = GL_UNIFORM_INIT("rt_gi_cine");
        gl_uniform_1i(&u_gi_cine, program, cine);
    }
    const float sun_k = light_scale * (cine ? RT_CINE_SUN : 1.0f);
    gl_uniform_3f(&u_gi_sun_light, program, l->light_color.x * l->light_intensity * sun_k,
                  l->light_color.y * l->light_intensity * sun_k, l->light_color.z * l->light_intensity * sun_k);
    gl_uniform_3f(&u_gi_sky_color, program, l->sky_color.x, l->sky_color.y, l->sky_color.z);
    sky_physical_uniforms(program, 1); // rtSky (psky_on 0 while it is off)
    gl_uniform_3f(&u_gi_fog_color, program, l->fog_color.x, l->fog_color.y, l->fog_color.z);
    gl_uniform_1f(&u_gi_fog_near, program, l->fog_near);
    gl_uniform_1f(&u_gi_fog_far, program, l->fog_enabled ? l->fog_far : 0.0f);
    gl_uniform_1f(&u_gi_emissive, program, RT_GI_EMISSIVE * (cine ? RT_CINE_EMISSIVE : 1.0f));
    gl_uniform_1f(&u_gi_sky_mix, program, cine ? RT_CINE_GI_SKY_MIX : RT_GI_SKY_MIX); // Cinematic: a miss sees mostly the sky
    gl_uniform_1i(&u_gi_frame_index, program, g_rt.frame_index);
    gl_uniform_1i(&u_gi_normal_only, program, g_rt.vm_running);
    gl_uniform_1i(&u_gi_hit_self, program, 0); // as the reflect pass: the viewmodel's rays skip the puppet too
    gl_uniform_1f(&u_gi_far, program, g_rt.vm_running ? RT_VM_GI_FAR : 20000.0f); // rt_gi.frag's former RT_GI_FAR for the world
    gl_uniform_2f(&u_gi_texel, program, 1.0f / (float) g_rt.width, 1.0f / (float) g_rt.height); // the prepass texel: surface_normal reads depth neighbours
    {
        // the world pass reuses this frame's sun visibility at on-screen hits (rt_gi.frag screen_sun; unit 10);
        // the viewmodel's camera is not the trace's, so it traces every hit. KRUNKNATIVE_RT_GI_TRACE_SUN=1 (dev) A/B.
        static GlUniform u_gi_reuse_sun = GL_UNIFORM_INIT("rt_gi_reuse_sun");
        static GlUniform u_gi_trace_vis = GL_UNIFORM_INIT("rt_trace_vis");
        static GlUniform u_gi_view_projection = GL_UNIFORM_INIT("view_projection");
        static int trace_sun = -1;
        if (trace_sun < 0) trace_sun = getenv("KRUNKNATIVE_RT_GI_TRACE_SUN") != NULL;
        float vp[16];
        const int reuse = !g_rt.vm_running && !trace_sun && g_rt.trace_texture && mat4_invert(inverse_view_projection, vp);
        gl_uniform_1i(&u_gi_reuse_sun, program, reuse);
        if (reuse) {
            bind_unit(10, g_rt.trace_texture);
            gl_uniform_1i(&u_gi_trace_vis, program, 10);
            gl_uniform_mat4(&u_gi_view_projection, program, GL_TRUE, vp);
        }
    }
    rt_sun_map_bind(program, 11);
    fullscreen();
    bind_unit(10, 0);
    bind_unit(11, 0);
    for (int unit = RT_EMISSIVE_UNIT; unit >= 7; unit--) bind_unit(unit, 0); // restore_state clears 0..6
    glViewport(0, 0, g_rt.width, g_rt.height); // the main denoiser runs next at RT resolution
}

// rt_gi_denoise.frag: temporal (validated against the main denoiser's previous history), then three
// widening a-trous steps. `main_previous` is the main history the temporal pass just read.
static void gi_denoise(const float inverse_view_projection[16], const Camera *camera, const unsigned int main_previous, const int history_was_valid) {
    const unsigned int program = g_rt.gi_denoise_program;
    use_program(program);
    bind_unit(2, g_rt.depth_texture);
    bind_unit(3, g_rt.normal_texture);
    bind_unit(4, main_previous);
    bind_unit(5, g_rt.velocity_texture);
    gl_uniform_1i(&u_gi_input, program, 0);
    gl_uniform_1i(&u_gi_history, program, 1);
    gl_uniform_1i(&u_gi_depth, program, 2);
    gl_uniform_1i(&u_gi_normal, program, 3);
    gl_uniform_1i(&u_gi_depth_history, program, 4);
    gl_uniform_1i(&u_gi_velocity, program, 5);
    gl_uniform_1i(&u_gi_velocity_valid, program, g_rt.velocity_texture ? 1 : 0);
    gl_uniform_mat4(&u_gi_inverse_view_projection, program, GL_TRUE, inverse_view_projection);
    gl_uniform_mat4(&u_gi_previous_view_projection, program, GL_TRUE, g_rt.previous_view_projection);
    gl_uniform_3f(&u_gi_camera_position, program, camera->position.x, camera->position.y, camera->position.z);
    gl_uniform_2f(&u_gi_texel, program, 1.0f / (float) g_rt.gi_width, 1.0f / (float) g_rt.gi_height); // the GI texel: the a-trous steps
    gl_uniform_1i(&u_gi_history_valid, program, history_was_valid && g_rt.gi_history_valid);
    gl_uniform_1i(&u_gi_gradient, program, !rt_no_gradient());
    glViewport(0, 0, g_rt.gi_width, g_rt.gi_height);

    const int read = g_rt.gi_history_index, write = 1 - read;
    glBindFramebuffer(GL_FRAMEBUFFER, g_rt.gi_history_fbo[write]);
    bind_unit(0, g_rt.gi_texture);
    bind_unit(1, g_rt.gi_history_texture[read]);
    gl_uniform_1i(&u_gi_pass, program, 0);
    gl_uniform_1i(&u_gi_step_size, program, 1);
    fullscreen();
    g_rt.gi_history_index = write;
    g_rt.gi_history_valid = 1;

    gl_uniform_1i(&u_gi_pass, program, 1);
    unsigned int source = g_rt.gi_history_texture[write];
    const int steps[3] = {1, 2, 4};
    for (int i = 0; i < 3; i++) {
        glBindFramebuffer(GL_FRAMEBUFFER, g_rt.gi_filter_fbo[i & 1]);
        bind_unit(0, source);
        gl_uniform_1i(&u_gi_step_size, program, steps[i]);
        fullscreen();
        source = g_rt.gi_filter_texture[i & 1];
    }
    g_rt.gi_result_texture = source;
    glViewport(0, 0, g_rt.width, g_rt.height);
}

// ---------------------------------------------------------------- volumetric sun shafts (rtShafts)
//
// rt_shafts.frag marches each view ray through the map's fog at RT resolution / RT_SHAFTS_DIVISOR, one sun
// shadow ray per step (the tables are the world's, so it runs inside rt_frame after the trace),
// rt_shafts_denoise.frag accumulates it over frames, and rt_shafts_composite (main.c, after the world draw and
// its effects, before the first-person scene) adds the lit fog's in-scattered sun light and removes the
// shadowed share of the map's own fog. Any rtMode > 0 with the rtShafts switch; its own state (g_shafts),
// separate from RtState. A map without fog (fogD unset) gets a faint additive haze (RT_SHAFTS_HAZE*) and no
// darkening, since there is no fog to remove. KRUNKNATIVE_RT_SHAFTS=1 turns the switch on for a run.

static struct {
    int checked, supported;
    unsigned int program, denoise_program, composite_program;
    int width, height;
    unsigned int fbo, texture;
    unsigned int history_fbo[2], history_texture[2];
    int history_index, history_valid;
    unsigned int result_texture;      // what the composite reads (not owned)
    int ready;                        // the march ran this frame: rt_shafts_composite draws
    float previous_view_projection[16]; // the last frame's (unjittered) camera: the temporal reprojection
    float inverse_view_projection[16];  // this frame's world camera (the composite rebuilds the view rays)
    float camera[3];
    float fog_near, fog_far, strength, shadow_strength;
} g_shafts;

static GlUniform u_sh_depth = GL_UNIFORM_INIT("rt_depth");
static GlUniform u_sh_inverse_view_projection = GL_UNIFORM_INIT("inverse_view_projection");
static GlUniform u_sh_previous_view_projection = GL_UNIFORM_INIT("previous_view_projection");
static GlUniform u_sh_camera_position = GL_UNIFORM_INIT("camera_position");
static GlUniform u_sh_sun_direction = GL_UNIFORM_INIT("sun_direction");
static GlUniform u_sh_sun_tan_radius = GL_UNIFORM_INIT("sun_tan_radius");
static GlUniform u_sh_fog_near = GL_UNIFORM_INIT("fog_near");
static GlUniform u_sh_fog_far = GL_UNIFORM_INIT("fog_far");
static GlUniform u_sh_density = GL_UNIFORM_INIT("density");
static GlUniform u_sh_frame_index = GL_UNIFORM_INIT("frame_index");
static GlUniform u_sh_input = GL_UNIFORM_INIT("rt_input");
static GlUniform u_sh_history = GL_UNIFORM_INIT("rt_history");
static GlUniform u_sh_history_valid = GL_UNIFORM_INIT("history_valid");
static GlUniform u_sh_texel = GL_UNIFORM_INIT("rt_texel");
static GlUniform u_sh_shafts = GL_UNIFORM_INIT("rt_shafts");
static GlUniform u_sh_sun_light = GL_UNIFORM_INIT("sun_light");
static GlUniform u_sh_fog_color = GL_UNIFORM_INIT("fog_color");
static GlUniform u_sh_strength = GL_UNIFORM_INIT("strength");
static GlUniform u_sh_shadow_strength = GL_UNIFORM_INIT("shadow_strength");
static GlUniform u_sh_phase_g = GL_UNIFORM_INIT("phase_g");
static GlUniform u_sh_phase_iso = GL_UNIFORM_INIT("phase_iso");
static GlUniform u_sh_size = GL_UNIFORM_INIT("shafts_size");
static GlUniform u_sh_term = GL_UNIFORM_INIT("term");

static void shafts_delete_targets(void) {
    if (g_shafts.fbo) glDeleteFramebuffers(1, &g_shafts.fbo);
    if (g_shafts.texture) glDeleteTextures(1, &g_shafts.texture);
    glDeleteFramebuffers(2, g_shafts.history_fbo);
    glDeleteTextures(2, g_shafts.history_texture);
    g_shafts.fbo = g_shafts.texture = 0;
    memset(g_shafts.history_fbo, 0, sizeof(g_shafts.history_fbo));
    memset(g_shafts.history_texture, 0, sizeof(g_shafts.history_texture));
    g_shafts.width = g_shafts.height = 0;
    g_shafts.history_valid = 0;
    g_shafts.result_texture = 0;
}

// 1 when the shafts can run at RT resolution `width` x `height`: the programs compiled, the targets exist.
static int shafts_ready(const int width, const int height) {
    if (!g_shafts.checked) {
        g_shafts.checked = 1;
        g_shafts.program = compile("shaders/scene_composite.vert", "shaders/rt_shafts.frag");
        g_shafts.denoise_program = compile("shaders/scene_composite.vert", "shaders/rt_shafts_denoise.frag");
        g_shafts.composite_program = compile("shaders/scene_composite.vert", "shaders/rt_shafts_composite.frag");
        g_shafts.supported = g_shafts.program && g_shafts.denoise_program && g_shafts.composite_program;
        if (!g_shafts.supported) fprintf(stderr, "[rt] sun shaft shaders failed to compile: rtShafts off for this run\n");
    }
    if (!g_shafts.supported) return 0;
    int w = width / RT_SHAFTS_DIVISOR, h = height / RT_SHAFTS_DIVISOR;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (g_shafts.fbo && g_shafts.width == w && g_shafts.height == h) return 1;
    shafts_delete_targets();
    // RGBA16F (the trace target's format: a colour-renderable float format is part of rt_supported())
    g_shafts.texture = make_texture(w, h, GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, 1);
    g_shafts.fbo = make_fbo(g_shafts.texture, 0);
    for (int i = 0; i < 2; i++) {
        g_shafts.history_texture[i] = make_texture(w, h, GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, 1);
        g_shafts.history_fbo[i] = make_fbo(g_shafts.history_texture[i], 0);
    }
    if (!g_shafts.fbo || !g_shafts.history_fbo[0] || !g_shafts.history_fbo[1]) {
        fprintf(stderr, "[rt] sun shaft targets incomplete: rtShafts off\n");
        shafts_delete_targets();
        g_shafts.supported = 0;
        return 0;
    }
    g_shafts.width = w;
    g_shafts.height = h;
    return 1;
}

// march + temporal, inside rt_frame (the world's tables on units 0..4, the prepass depth on 5)
static void shafts_pass(const float inverse_view_projection[16], const float velocity_view_projection[16], const Camera *camera,
                        const int history_was_valid) {
    const ClientRenderSettings *s = client_render_settings();
    const RenderLighting *l = &g_render_lighting;
    vec3 sun = l->light_direction;
    const float length = sqrtf(sun.x * sun.x + sun.y * sun.y + sun.z * sun.z);
    if (length > 0.0f) { sun.x /= length; sun.y /= length; sun.z /= length; }
    const int fog = l->fog_enabled && l->fog_far > l->fog_near;
    g_shafts.fog_near = fog ? l->fog_near : 1.0f;
    g_shafts.fog_far = fog ? l->fog_far : RT_SHAFTS_HAZE_FAR;
    g_shafts.strength = RT_SHAFTS_STRENGTH * (fog ? 1.0f : RT_SHAFTS_HAZE) * (rt_cinematic() ? RT_CINE_SHAFTS_GAIN : 1.0f);
    g_shafts.shadow_strength = fog ? RT_SHAFTS_SHADOW : 0.0f;

    unsigned int program = g_shafts.program;
    glBindFramebuffer(GL_FRAMEBUFFER, g_shafts.fbo);
    glViewport(0, 0, g_shafts.width, g_shafts.height);
    glDisable(GL_DEPTH_TEST);
    use_program(program);
    rt_gpu_bind(&g_rt.gpu, program, 0); // units 0..4
    gl_uniform_1i(&u_rt_self_root, program, g_rt.scene.self_tlas_root); // your own shadow cuts the shafts too
    bind_unit(5, g_rt.depth_texture);
    gl_uniform_1i(&u_sh_depth, program, 5);
    if (g_rt.scene.alpha_test_count > 0) rt_atlas_bind(program, 7, 8); // through the cut-outs, as the sun shadow
    else gl_uniform_1i(&u_rt_atlas_ready, program, 0);
    gl_uniform_mat4(&u_sh_inverse_view_projection, program, GL_TRUE, inverse_view_projection);
    gl_uniform_3f(&u_sh_camera_position, program, camera->position.x, camera->position.y, camera->position.z);
    gl_uniform_3f(&u_sh_sun_direction, program, sun.x, sun.y, sun.z);
    gl_uniform_1f(&u_sh_sun_tan_radius, program, tanf(s->rt_sun_radius * (float) M_PI / 180.0f));
    gl_uniform_1f(&u_sh_fog_near, program, g_shafts.fog_near);
    gl_uniform_1f(&u_sh_fog_far, program, g_shafts.fog_far);
    gl_uniform_1f(&u_sh_density, program, RT_SHAFTS_DENSITY * (rt_cinematic() ? RT_CINE_SHAFTS_DENSITY : 1.0f) / g_shafts.fog_far);
    gl_uniform_1i(&u_sh_frame_index, program, g_rt.frame_index);
    fullscreen();
    for (int unit = 8; unit >= 7; unit--) bind_unit(unit, 0); // restore_state puts 1..6 back

    program = g_shafts.denoise_program;
    use_program(program);
    const int read = g_shafts.history_index, write = 1 - read;
    glBindFramebuffer(GL_FRAMEBUFFER, g_shafts.history_fbo[write]);
    bind_unit(0, g_shafts.texture);
    bind_unit(1, g_shafts.history_texture[read]);
    gl_uniform_1i(&u_sh_input, program, 0);
    gl_uniform_1i(&u_sh_history, program, 1);
    gl_uniform_1i(&u_sh_history_valid, program, history_was_valid && g_shafts.history_valid && s->rt_denoise);
    gl_uniform_mat4(&u_sh_inverse_view_projection, program, GL_TRUE, inverse_view_projection);
    gl_uniform_mat4(&u_sh_previous_view_projection, program, GL_TRUE, g_shafts.previous_view_projection);
    gl_uniform_3f(&u_sh_camera_position, program, camera->position.x, camera->position.y, camera->position.z);
    gl_uniform_2f(&u_sh_texel, program, 1.0f / (float) g_shafts.width, 1.0f / (float) g_shafts.height);
    fullscreen();
    g_shafts.history_index = write;
    g_shafts.history_valid = 1;
    g_shafts.result_texture = g_shafts.history_texture[write];

    memcpy(g_shafts.previous_view_projection, velocity_view_projection, sizeof(g_shafts.previous_view_projection));
    memcpy(g_shafts.inverse_view_projection, inverse_view_projection, sizeof(g_shafts.inverse_view_projection));
    g_shafts.camera[0] = camera->position.x;
    g_shafts.camera[1] = camera->position.y;
    g_shafts.camera[2] = camera->position.z;
    g_shafts.ready = 1;
    glViewport(0, 0, g_rt.width, g_rt.height);
}

void rt_shafts_composite(void) {
    if (!g_shafts.ready || !g_shafts.result_texture || !g_rt.depth_texture) return;
    g_shafts.ready = 0; // once per traced frame
    const RenderLighting *l = &g_render_lighting;
    vec3 sun = l->light_direction;
    const float length = sqrtf(sun.x * sun.x + sun.y * sun.y + sun.z * sun.z);
    if (length > 0.0f) { sun.x /= length; sun.y /= length; sun.z /= length; }
    // the sun's radiance off a white Lambert surface facing it, as basic.frag lights one (krunker_lambert:
    // irradiance = colour * intensity * light_scale, BRDF albedo / PI)
    const float light_scale = (l->correct_lights ? 1.0f : (float) M_PI) / (float) M_PI;

    int bound0 = 0, bound1 = 0, blend_on = glIsEnabled(GL_BLEND), depth_on = glIsEnabled(GL_DEPTH_TEST), cull_on = glIsEnabled(GL_CULL_FACE);
    int src_rgb = 0, dst_rgb = 0, src_a = 0, dst_a = 0, eq_rgb = 0, eq_a = 0;
    glGetIntegerv(GL_BLEND_SRC_RGB, &src_rgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &dst_rgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &src_a);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &dst_a);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &eq_rgb);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &eq_a);
    glActiveTexture(GL_TEXTURE1);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound1);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound0);

    const unsigned int program = g_shafts.composite_program;
    use_program(program);
    bind_unit(0, g_shafts.result_texture);
    bind_unit(1, g_rt.depth_texture);
    gl_uniform_1i(&u_sh_shafts, program, 0);
    gl_uniform_1i(&u_sh_depth, program, 1);
    gl_uniform_mat4(&u_sh_inverse_view_projection, program, GL_TRUE, g_shafts.inverse_view_projection);
    gl_uniform_3f(&u_sh_camera_position, program, g_shafts.camera[0], g_shafts.camera[1], g_shafts.camera[2]);
    gl_uniform_3f(&u_sh_sun_direction, program, sun.x, sun.y, sun.z);
    // Cinematic (rt.h "Cinematic colour pipeline"): the shafts take the physical sky's sun colour (reddened by the
    // atmosphere, luminance 1) and a sharper lobe, so their glow agrees with the sky's aureole
    float sun_tint[3] = {1.0f, 1.0f, 1.0f};
    const int cine_shafts = rt_cinematic();
    if (cine_shafts) sky_physical_sun_color(sun_tint);
    gl_uniform_3f(&u_sh_sun_light, program, l->light_color.x * l->light_intensity * light_scale * sun_tint[0],
                  l->light_color.y * l->light_intensity * light_scale * sun_tint[1], l->light_color.z * l->light_intensity * light_scale * sun_tint[2]);
    gl_uniform_3f(&u_sh_fog_color, program, l->fog_color.x, l->fog_color.y, l->fog_color.z);
    gl_uniform_1f(&u_sh_fog_near, program, g_shafts.fog_near);
    gl_uniform_1f(&u_sh_fog_far, program, g_shafts.fog_far);
    gl_uniform_1f(&u_sh_strength, program, g_shafts.strength);
    gl_uniform_1f(&u_sh_shadow_strength, program, g_shafts.shadow_strength);
    gl_uniform_1f(&u_sh_phase_g, program, cine_shafts ? 0.75f : RT_SHAFTS_G);
    gl_uniform_1f(&u_sh_phase_iso, program, RT_SHAFTS_ISO);
    gl_uniform_2f(&u_sh_size, program, (float) g_shafts.width, (float) g_shafts.height);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDepthMask(GL_FALSE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE); // the target's alpha (the page composite) is not ours
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    if (g_shafts.shadow_strength > 0.0f) {
        glBlendEquation(GL_FUNC_REVERSE_SUBTRACT); // dst - shadowed fog
        gl_uniform_1i(&u_sh_term, program, 0);
        fullscreen();
    }
    glBlendEquation(GL_FUNC_ADD); // dst + in-scattered sun light
    gl_uniform_1i(&u_sh_term, program, 1);
    fullscreen();

    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glBlendEquationSeparate((unsigned int) eq_rgb, (unsigned int) eq_a);
    glBlendFuncSeparate((unsigned int) src_rgb, (unsigned int) dst_rgb, (unsigned int) src_a, (unsigned int) dst_a);
    if (!blend_on) glDisable(GL_BLEND);
    if (depth_on) glEnable(GL_DEPTH_TEST);
    if (cull_on) glEnable(GL_CULL_FACE);
    glBindVertexArray(0);
    use_program(0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, (unsigned int) bound1);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (unsigned int) bound0);
    g_active_texture = (unsigned int) bound0;
}

// Texture units 1..6 as the world pass had them (shadow_begin_world's shadow map on unit 1 among them);
// unit 0 is left empty, materials rebind theirs per draw (g_active_texture).
static void save_units(void) {
    for (int unit = 1; unit <= 6; unit++) {
        int bound = 0;
        glActiveTexture(GL_TEXTURE0 + (unsigned int) unit);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
        g_rt.saved_units[unit] = bound > 0 ? (unsigned int) bound : 0;
    }
    glActiveTexture(GL_TEXTURE0);
}

static void restore_state(const int previous_fbo, const int viewport[4]) {
    for (int unit = 6; unit >= 1; unit--) bind_unit(unit, g_rt.saved_units[unit]);
    bind_unit(0, 0);
    g_active_texture = 0;
    glBindVertexArray(0);
    use_program(0);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glBindFramebuffer(GL_FRAMEBUFFER, (unsigned int) previous_fbo);
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
}

static void water_frame(const Scene *scene, const Camera *camera, const float view_projection[16], const float inverse_view_projection[16],
                        const float velocity_view_projection[16], int effects); // water and glass (rt.h), below

// ---------------------------------------------------------------- sub-pass GPU timing (dev)
//
// KRUNKNATIVE_RT_TIMING=1 (desktop only: ES 3.0 has no timer queries) prints, once a second, the
// average GPU time of each world sub-pass of rt_frame. Same scheme as gpu_timer.c: a GL_TIMESTAMP
// query per mark into one of RTT_SLOTS sets, read back RTT_SLOTS - 1 frames later only when available.
enum { RTT_UPLOAD, RTT_PREPASS, RTT_TRACE, RTT_REFLECT, RTT_GI, RTT_DENOISE, RTT_REFL_DENOISE, RTT_GI_DENOISE,
       RTT_WATER, RTT_SHAFTS, RTT_COUNT };
static const char *const RTT_NAME[RTT_COUNT] = {"upload", "prepass", "trace", "reflect", "gi", "denoise",
                                                "refl_dn", "gi_dn", "water", "shafts"};
#define RTT_SLOTS 4
static struct {
    int state; // 0 unknown, 1 on, -1 off
#ifndef KN_GLES
    GLuint query[RTT_SLOTS][RTT_COUNT + 1];
    int mark_pass[RTT_SLOTS][RTT_COUNT + 1];
    int count[RTT_SLOTS], pending[RTT_SLOTS];
    int ready, current;
    double sum[RTT_COUNT];
    int frames;
    double last_print;
#endif
} g_rtt;

static int rtt_on(void) {
    if (!g_rtt.state) {
        const char *v = getenv("KRUNKNATIVE_RT_TIMING");
        g_rtt.state = v && *v && strcmp(v, "0") ? 1 : -1;
    }
    return g_rtt.state > 0;
}

#ifndef KN_GLES
static void rtt_read(const int slot) {
    if (!g_rtt.pending[slot]) return;
    g_rtt.pending[slot] = 0;
    const int n = g_rtt.count[slot];
    if (n < 2) return;
    GLint available = 0;
    glGetQueryObjectiv(g_rtt.query[slot][n - 1], GL_QUERY_RESULT_AVAILABLE, &available);
    if (!available) return;
    GLuint64 prev = 0;
    for (int i = 0; i < n; i++) {
        GLuint64 t = 0;
        glGetQueryObjectui64v(g_rtt.query[slot][i], GL_QUERY_RESULT, &t);
        if (i) g_rtt.sum[g_rtt.mark_pass[slot][i]] += (double) (t - prev) / 1.0e6;
        prev = t;
    }
    g_rtt.frames++;
}
#endif

static void rtt_begin(void) {
#ifndef KN_GLES
    if (!rtt_on()) return;
    if (!g_rtt.ready) {
        for (int i = 0; i < RTT_SLOTS; i++) glGenQueries(RTT_COUNT + 1, g_rtt.query[i]);
        g_rtt.ready = 1;
        g_rtt.last_print = glfwGetTime();
    }
    g_rtt.current = (g_rtt.current + 1) % RTT_SLOTS;
    rtt_read(g_rtt.current);
    g_rtt.count[g_rtt.current] = 1;
    glQueryCounter(g_rtt.query[g_rtt.current][0], GL_TIMESTAMP);
#endif
}

static void rtt_mark(const int pass) { // `pass` ends here
#ifndef KN_GLES
    if (!rtt_on() || !g_rtt.ready) return;
    const int s = g_rtt.current, n = g_rtt.count[s];
    if (n > RTT_COUNT) return;
    g_rtt.mark_pass[s][n] = pass;
    glQueryCounter(g_rtt.query[s][n], GL_TIMESTAMP);
    g_rtt.count[s] = n + 1;
#else
    (void) pass;
#endif
}

static void rtt_end(void) {
#ifndef KN_GLES
    if (!rtt_on() || !g_rtt.ready) return;
    g_rtt.pending[g_rtt.current] = 1;
    const double now = glfwGetTime();
    if (now - g_rtt.last_print < 1.0 || !g_rtt.frames) return;
    char line[512];
    int len = snprintf(line, sizeof line, "[rt-timing] %d frames:", g_rtt.frames);
    double total = 0.0;
    for (int i = 0; i < RTT_COUNT; i++) total += g_rtt.sum[i];
    for (int i = 0; i < RTT_COUNT && len < (int) sizeof line - 32; i++)
        len += snprintf(line + len, sizeof line - (size_t) len, " %s %.3f", RTT_NAME[i], g_rtt.sum[i] / g_rtt.frames);
    fprintf(stderr, "%s | total %.3f ms\n", line, total / g_rtt.frames);
    memset(g_rtt.sum, 0, sizeof g_rtt.sum);
    g_rtt.frames = 0;
    g_rtt.last_print = now;
#endif
}

void rt_frame(const Scene *scene, Camera *camera) {
    const ClientRenderSettings *s = client_render_settings();
    g_rt.world_active = 0;
    g_rt.effects = 0;
    g_rt.velocity_ready = 0;
    g_rt.taa_sky_cached = 0;
    g_rt.traced_frame = 0; // rt_viewmodel_frame runs only after a full trace of this frame
    g_rt.vm_ready = 0;
    // TAA (rtTaa) needs the prepass's motion vectors even with ray tracing off: the velocity-only path
    // below runs the prepass alone (no BVH upload, no trace); render_target.c reads rt_taa_velocity()
    const int taa = s->rt_taa ? 1 : 0;
    // world texture filtering (rt_filtering.c): trilinear + anisotropic while rtMode > 0, Krunker's own at 0
    rt_filtering_update(s->rt_mode != RT_MODE_OFF && rt_supported());
    if ((s->rt_mode == RT_MODE_OFF && !taa) || !scene || !camera) return;
    rt_normals_frame(); // rt_normals.c: the generated normal maps materials asked for last frame (no-op when none)
    if (!rt_supported() || !programs_ready()) return;
    if (!g_rt.debug_env_read) { // KRUNKNATIVE_RT_DEBUG=<what>: the console's rt_debug for headless captures
        g_rt.debug_env_read = 1;
        const char *debug = getenv("KRUNKNATIVE_RT_DEBUG");
        if (debug && *debug) rt_debug_set(debug);
    }
    int velocity_only = s->rt_mode == RT_MODE_OFF;
    if (!g_render_lighting.enabled || g_render_lighting.light_intensity <= 0.0f) { // no sun: nothing to shadow
        if (!taa) return;
        velocity_only = 1;
    }

    int viewport[4], previous_fbo;
    glGetIntegerv(GL_VIEWPORT, viewport);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous_fbo);
    if (viewport[2] <= 0 || viewport[3] <= 0) return;
    g_rt.scene_width = viewport[2];
    g_rt.scene_height = viewport[3];
    save_units();

    float scale = s->rt_scale;
    if (scale < 0.25f) scale = 0.25f;
    if (scale > 1.0f) scale = 1.0f;
    int width = (int) lroundf((float) viewport[2] * scale), height = (int) lroundf((float) viewport[3] * scale);
    if (width < 1) width = 1;
    if (height < 1) height = 1;

    // the world pass's camera matrices (scene_render recomputes the same)
    camera_update_projection_matrix(camera, (float) viewport[2] / (float) viewport[3]);
    camera_update_world_inverse_matrix(camera);
    float view_projection[16], inverse_view_projection[16];
    mat4x4(camera->projection_matrix, camera->world_inverse_matrix, view_projection);
    if (!mat4_invert(view_projection, inverse_view_projection)) return;
    // the same camera without the TAA jitter (camera.c puts -jitter in row 0 / 1, column 2): the
    // velocity's two ends and the camera-only reprojections compare unjittered frames
    float unjittered_projection[16], velocity_view_projection[16], jitter_x, jitter_y;
    memcpy(unjittered_projection, camera->projection_matrix, sizeof(unjittered_projection));
    camera_get_jitter(&jitter_x, &jitter_y);
    unjittered_projection[2] += jitter_x;
    unjittered_projection[6] += jitter_y;
    mat4x4(unjittered_projection, camera->world_inverse_matrix, velocity_view_projection);

    rt_scene_build(&g_rt.scene, scene, rt_self_frame()); // + the local player's first-person shadow proxy (rt_self.c)
    if (!g_rt.build_logged && !velocity_only) {
        g_rt.build_logged = 1;
        fprintf(stderr, "[rt] rtMode %d: %d instances (%d alpha-tested casters), %d BLAS, %d triangles, %d TLAS nodes, %d materials (scale %.2f, %dx%d)\n",
                s->rt_mode, g_rt.scene.instance_count, g_rt.scene.alpha_test_count, g_rt.scene.blas_count, g_rt.scene.tri_count,
                g_rt.scene.tlas_count, g_rt.scene.material_count, scale, width, height);
    }
    if (!g_rt.scene.instance_count || !g_rt.scene.tlas_count) return;

    // nothing of ours may stay bound where a pass renders into it (feedback loops on WebGL / ANGLE)
    bind_unit(RT_TEXTURE_UNIT, 0);
    bind_unit(RT_LOCAL_TEXTURE_UNIT, 0);
    bind_unit(RT_REFL_TEXTURE_UNIT, 0);
    bind_unit(RT_GI_TEXTURE_UNIT, 0);
    glActiveTexture(GL_TEXTURE0);
    if (!ensure_targets(width, height)) { restore_state(previous_fbo, viewport); return; }
    if (velocity_only) { // TAA with the tracer off: the prepass alone, for its motion vectors
        g_rt.refl_active = 0;
        prepass(view_projection, velocity_view_projection);
        memcpy(g_rt.taa_previous_view_projection, g_rt.history_valid ? g_rt.previous_view_projection : velocity_view_projection, sizeof(velocity_view_projection));
        memcpy(g_rt.taa_view_projection, velocity_view_projection, sizeof(velocity_view_projection));
        memcpy(g_rt.previous_view_projection, velocity_view_projection, sizeof(velocity_view_projection));
        g_rt.history_valid = 1; // the velocity's previous camera is real from the next frame on
        g_rt.velocity_ready = g_rt.velocity_texture != 0;
        g_rt.frame_index++;
        restore_state(previous_fbo, viewport);
        return;
    }
    rtt_begin();
    if (!rt_gpu_upload(&g_rt.gpu, &g_rt.scene)) { rtt_end(); restore_state(previous_fbo, viewport); return; }

    int effects = RT_EFFECT_SHADOWS;
    if (s->rt_mode >= RT_MODE_AO) effects |= RT_EFFECT_AO;
    if (rt_materials_loaded() && !rt_no_materials()) effects |= RT_EFFECT_MATERIALS; // Box Office material classes (rt_materials.c, basic.frag)
    if (!rt_no_sky_ambient()) effects |= RT_EFFECT_SKY_AMBIENT; // sky-dome ambient with any rt_mode (basic.c / basic.frag)
    if (!rt_no_bevel()) effects |= RT_EFFECT_BEVEL; // bevelled map cube edges, shading only (rt.h, basic.frag krunker_rt_bevel)
    if (rt_cinematic()) effects |= RT_EFFECT_CINEMATIC; // look-first lighting (rt.h "Cinematic lighting")
    // specular occlusion of the classed materials' sky reflection (rt.h): mode 2+ has the traced AO; at mode 1
    // the world's sky-visibility ray only with KRUNKNATIVE_RT_SKY_VIS=1; the viewmodel's camera probe always
    if ((effects & RT_EFFECT_MATERIALS) && !rt_no_spec_occlusion()) {
        if (!(effects & RT_EFFECT_AO) && rt_sky_vis_ray()) effects |= RT_EFFECT_SKY_VIS; // opt-in: see rt.h
        sky_probe_update(camera);
    }
    g_rt.refl_active = s->rt_mode >= RT_MODE_REFLECTIONS && refl_ready(); // phase 4 (the prepass reads it)
    if (g_rt.refl_active) effects |= RT_EFFECT_REFLECTIONS;
    if (g_rt.refl_active && rt_atlas_force_reflections()) effects |= RT_EFFECT_FORCE_REFL;
    const int gi_active = s->rt_mode >= RT_MODE_AO && gi_ready(); // one-bounce GI rides with AO
    emissive_lights_frame(camera, gi_active); // the neon / emissive surfaces the GI pass samples directly
    if (gi_active) effects |= RT_EFFECT_GI;
    use_program(g_rt.trace_program);
    if (select_local_lights(g_rt.trace_program, camera) > 0) effects |= RT_EFFECT_LOCAL_LIGHTS;
    if (g_rt.scene.alpha_test_count > 0) rt_atlas_update(g_rt.vao); // the cut-out textures the shadow rays will read
    // the passes below read this frame's bits from g_rt.effects (gi_pass: the Cinematic fill / sky share / emissive
    // scale); it was only assigned at the end, so the world GI ran with the previous frame's 0 and Cinematic GI was
    // Custom GI (a miss at the full RT_GI_SKY_MIX: the physical sky's blue zenith turned Sandstorm's shade blue)
    g_rt.effects = effects;
    rtt_mark(RTT_UPLOAD);
    prepass(view_projection, velocity_view_projection);
    rtt_mark(RTT_PREPASS);
    trace(inverse_view_projection, camera, effects);
    rtt_mark(RTT_TRACE);
    if (g_rt.refl_active) reflect_pass(inverse_view_projection, camera);
    rtt_mark(RTT_REFLECT);
    if (gi_active) gi_pass(inverse_view_projection, camera);
    rtt_mark(RTT_GI);
    const int history_was_valid = g_rt.history_valid, main_history_read = g_rt.history_index; // phase 4: the reflection denoiser reads the same previous history
    if (s->rt_denoise) denoise(velocity_view_projection, inverse_view_projection, camera, effects); // unjittered pair (TAA)
    else g_rt.result_texture = g_rt.trace_texture, g_rt.local_result_texture = g_rt.local_texture;
    rtt_mark(RTT_DENOISE);
    if (g_rt.refl_active) {
        if (s->rt_denoise) refl_denoise(inverse_view_projection, camera, g_rt.history_texture[main_history_read], history_was_valid);
        else g_rt.refl_result_texture = g_rt.refl_texture;
    }
    rtt_mark(RTT_REFL_DENOISE);
    if (gi_active) {
        if (s->rt_denoise) gi_denoise(inverse_view_projection, camera, g_rt.history_texture[main_history_read], history_was_valid);
        else g_rt.gi_result_texture = g_rt.gi_texture;
    }
    rtt_mark(RTT_GI_DENOISE);
    water_frame(scene, camera, view_projection, inverse_view_projection, velocity_view_projection, effects); // water and glass (rt.h)
    rtt_mark(RTT_WATER);
    g_shafts.ready = 0;
    if (s->rt_shafts && shafts_ready(width, height)) shafts_pass(inverse_view_projection, velocity_view_projection, camera, history_was_valid); // rtShafts
    rtt_mark(RTT_SHAFTS);
    rtt_end();
    memcpy(g_rt.taa_previous_view_projection, history_was_valid ? g_rt.previous_view_projection : velocity_view_projection, sizeof(velocity_view_projection));
    memcpy(g_rt.taa_view_projection, velocity_view_projection, sizeof(velocity_view_projection));
    memcpy(g_rt.previous_view_projection, velocity_view_projection, sizeof(velocity_view_projection)); // unjittered (TAA)
    g_rt.velocity_ready = g_rt.velocity_texture != 0;
    g_rt.frame_index++;

    restore_state(previous_fbo, viewport);
    g_rt.world_active = 1;
    g_rt.effects = effects;
    g_rt.traced_frame = 1;
}

// ---------------------------------------------------------------- the first-person viewmodel
//
// main.c draws the weapon and arms (client->fps_scene) with the fps camera after the world; they are
// not in the BVH (camera-relative meshes), so the world's trace says nothing about them, and until now
// they kept Krunker's flat look. rt_viewmodel_frame runs the prepass over the fps scene's meshes with
// the fps camera, then the reflect and GI passes from those surfaces into the world's tables: a metal
// gun reflects the room, the arms take its bounce. Not traced for the viewmodel: the sun shadow / AO
// (it keeps the shadow map like before: those rays would mostly leave through the player's own body)
// and the local lights. The weapon is not in the BVH, so it never reflects / shadows itself.
//
// Targets: a set of its own (g_vm: prepass depth / normal / material, the reflect and GI radiance,
// histories and filters, and a depth history pair for the denoisers' surface test), because the world's
// prepass velocity feeds the TAA resolve after the fps draw and the world's results feed the debug blit:
// none of them may be overwritten here. The passes read g_rt.<field>, so g_vm's fields are swapped in
// for the duration (vm_swap) and the unchanged pass functions run on them. History reprojection: the
// weapon is camera-locked, so the previous camera is taken to be this one (identity in screen space;
// sway / bob are small) and no velocity target exists (camera-only path in the GI denoiser).
// KRUNKNATIVE_RT_NO_VIEWMODEL=1 (dev) leaves the viewmodel untouched for A/B.
//
// Cost: the weapon's rays are the costliest of the frame per pixel (its small facets scatter every warp's rays),
// so the set runs at half the world's RT resolution (RT_VM_DIVISOR), the fullscreen passes are scissored to the
// weapon's screen rectangle (near-plane-clipped local boxes: vm_scissor_rect), and each effect is traced once
// per VM_PERIOD frames while the weapon holds still on screen, every other frame while it moves (rt_viewmodel_frame).
// Dev knobs for A/B: KRUNKNATIVE_RT_VM_DIV=<1..4>, KRUNKNATIVE_RT_VM_EVERY=1, KRUNKNATIVE_RT_VM_TIMING=1 (desktop
// GL: each sub-pass's GPU time every 120 frames; stalls the pipeline while on).

static RtState g_vm; // only the VM_SWAP_FIELDS members are used

#define VM_SWAP_FIELDS(X) \
    X(prepass_fbo) X(depth_texture) X(normal_texture) X(material_texture) X(material_attached) X(velocity_texture) \
    X(history_fbo) X(history_texture) X(history_index) X(history_valid) X(previous_view_projection) \
    X(refl_fbo) X(refl_texture) X(refl_history_fbo) X(refl_history_texture) X(refl_history_index) X(refl_history_valid) \
    X(refl_filter_fbo) X(refl_filter_texture) X(refl_result_texture) \
    X(gi_fbo) X(gi_texture) X(gi_history_fbo) X(gi_history_texture) X(gi_history_index) X(gi_history_valid) \
    X(gi_filter_fbo) X(gi_filter_texture) X(gi_result_texture) X(gi_width) X(gi_height) X(width) X(height)
#define VM_SWAP_ONE(f) { unsigned char t[sizeof(g_rt.f)]; memcpy(t, &g_rt.f, sizeof t); memcpy(&g_rt.f, &g_vm.f, sizeof t); memcpy(&g_vm.f, t, sizeof t); }

static void vm_swap(void) {
    VM_SWAP_FIELDS(VM_SWAP_ONE)
}

static int vm_divisor(void) { // KRUNKNATIVE_RT_VM_DIV=<1..4> (dev) overrides RT_VM_DIVISOR for A/B
    static int div = 0;
    if (!div) {
        const char *e = getenv("KRUNKNATIVE_RT_VM_DIV");
        div = e && *e ? atoi(e) : RT_VM_DIVISOR;
        if (div < 1) div = 1;
        if (div > 4) div = 4;
    }
    return div;
}

static int vm_every_frame(void) { // KRUNKNATIVE_RT_VM_EVERY=1 (dev): the reflection and the bounce both every frame
    static int every = -1;
    if (every < 0) { const char *e = getenv("KRUNKNATIVE_RT_VM_EVERY"); every = e && *e && strcmp(e, "0") != 0; }
    return every;
}

static int rt_no_viewmodel(void) { // KRUNKNATIVE_RT_NO_VIEWMODEL=1 (dev only): the viewmodel keeps Krunker's look (A/B)
    static int no_vm = -1;
    if (no_vm < 0) { const char *e = getenv("KRUNKNATIVE_RT_NO_VIEWMODEL"); no_vm = e && *e && strcmp(e, "0") != 0; }
    return no_vm;
}

// g_vm swapped in: frees the viewmodel's own targets (the reflect / GI sets through their delete_* helpers)
static void vm_delete_targets(void) {
    delete_refl_targets();
    delete_gi_targets();
    unsigned int fbos[3] = {g_rt.prepass_fbo, g_rt.history_fbo[0], g_rt.history_fbo[1]};
    unsigned int textures[4] = {g_rt.depth_texture, g_rt.normal_texture, g_rt.history_texture[0], g_rt.history_texture[1]};
    for (int i = 0; i < 3; i++) if (fbos[i]) glDeleteFramebuffers(1, &fbos[i]);
    for (int i = 0; i < 4; i++) if (textures[i]) gl_release_texture(textures[i]), glDeleteTextures(1, &textures[i]);
    g_rt.prepass_fbo = g_rt.history_fbo[0] = g_rt.history_fbo[1] = 0;
    g_rt.depth_texture = g_rt.normal_texture = g_rt.history_texture[0] = g_rt.history_texture[1] = 0;
    g_rt.width = g_rt.height = 0;
    g_rt.history_valid = 0;
    g_rt.velocity_texture = 0; // never allocated for the viewmodel (camera-locked: identity reprojection)
}

// g_vm swapped in: the viewmodel's targets at the world's RT resolution / RT_VM_DIVISOR (prepass + depth history
// here, the reflect / GI sets by the shared ensure_* steps, which attach the material target to this prepass FBO)
static int vm_ensure_targets(const int width, const int height) {
    if (g_rt.prepass_fbo && g_rt.width == width && g_rt.height == height) return 1;
    vm_delete_targets();
    glActiveTexture(GL_TEXTURE0 + RT_TEXTURE_UNIT);
    g_rt.depth_texture = make_texture(width, height, GL_DEPTH_COMPONENT24, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, 0);
    g_rt.normal_texture = make_texture(width, height, GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, 0);
    for (int i = 0; i < 2; i++) g_rt.history_texture[i] = make_texture(width, height, GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, 1);
    glBindTexture(GL_TEXTURE_2D, 0);
    g_rt.prepass_fbo = make_fbo(g_rt.normal_texture, g_rt.depth_texture);
    int ok = g_rt.prepass_fbo != 0;
    for (int i = 0; i < 2; i++) {
        g_rt.history_fbo[i] = make_fbo(g_rt.history_texture[i], 0);
        ok = ok && g_rt.history_fbo[i];
    }
    if (!ok) {
        static int logged;
        if (!logged) logged = 1, fprintf(stderr, "[rt] viewmodel targets incomplete: the viewmodel keeps Krunker's look\n");
        vm_delete_targets();
        return 0;
    }
    ensure_refl_targets(width, height);
    // GI at a quarter of the RT resolution (the world's is a half): the weapon's bounce rays are the costliest
    // of the frame per ray (its tiny facets send every warp's rays in all directions), and its bounce is low
    // frequency on a small part of the screen
    ensure_gi_targets(width / 2 > 0 ? width / 2 : 1, height / 2 > 0 ? height / 2 : 1);
    g_rt.width = width;
    g_rt.height = height;
    g_rt.history_valid = 0;
    return 1;
}

// The prepass material of a first-person mesh: the same RtMaterial rt_scene.c's material table would hold
// for it (material_index), so the reflect pass sees the weapon as the tracer would (a skin env, the
// Box Office material table through rt_atlas_material, the forced-reflection knob)
static void vm_material(const BasicMaterial *m, float out[3], const int refl) {
    RtMaterial want;
    memset(&want, 0, sizeof(want));
    want.color[0] = m->color.x; want.color[1] = m->color.y; want.color[2] = m->color.z; want.color[3] = m->color.w;
    want.emissive[0] = m->emissive.x; want.emissive[1] = m->emissive.y; want.emissive[2] = m->emissive.z;
    want.alpha_test = m->alpha_test;
    want.metalness = m->metalness_factor;
    want.roughness = m->roughness_factor;
    want.reflect_intensity = m->env_map ? m->env_map_intensity : m->scene_env_intensity;
    want.unlit = m->unlit || m->emissive.x > 0.0f || m->emissive.y > 0.0f || m->emissive.z > 0.0f;
    rt_atlas_material(&want, m);
    out[0] = refl ? want.reflect_intensity : 0.0f;
    out[1] = want.roughness;
    out[2] = want.metalness;
}

// The prepass over a scene's meshes (the fps scene): like prepass() over the instances, but the meshes are
// not in the BVH, so their material / flags come straight from the BasicMaterial; all dynamic, no mesh motion
// (prev_transform = transform: the weapon is camera-locked)
static int g_vm_motion; // the view moved this frame (rt_viewmodel_frame): the viewmodel prepass marks the weapon dynamic

static void prepass_scene(const Scene *scene, const float view_projection[16], const int refl) {
    prepass_begin(view_projection, view_projection);
    int culling = -1;
    for (size_t i = 0; i < scene->mesh_count; i++) {
        Mesh *mesh = scene->meshes[i];
        if (!mesh->visible || !mesh->geometry || !mesh->geometry->vao || mesh->geometry->draw_mode || !mesh->material) continue;
        if (mesh->material->vtable != &basic_material_vtable || mesh->material->transparent) continue;
        const BasicMaterial *bm = (const BasicMaterial *) mesh->material;
        if (bm->sprite) continue;
        mesh_update_transform_matrix(mesh); // scene_render computes the same right after
        const int cull = !bm->double_sided;
        if (cull != culling) {
            if (cull) glEnable(GL_CULL_FACE);
            else glDisable(GL_CULL_FACE);
            culling = cull;
        }
        if (cull) glFrontFace((determinant3(mesh->transform_matrix) < 0.0f) != (bm->cull_front != 0) ? GL_CW : GL_CCW);
        glUniformMatrix4fv(g_rt.prepass_loc_transform, 1, GL_TRUE, mesh->transform_matrix);
        // "dynamic" for the denoisers only while the view moves (g_vm_motion: walk, jump, bob, turn, aim): the weapon
        // is camera-locked and reprojected as identity, but the world it reflects and takes bounce light from moves
        // past it, so a static-length history (24 / 32 frames) trailed the motion. Still: a static surface's
        // history (the 4-frame dynamic cap left a still view's reflection noisy).
        glUniform1f(g_rt.prepass_loc_dynamic, g_vm_motion ? 1.0f : 0.0f);
        if (g_rt.prepass_loc_material >= 0) {
            float material[3];
            vm_material(bm, material, refl);
            glUniform3f(g_rt.prepass_loc_material, material[0], material[1], material[2]);
        }
        glUniformMatrix4fv(g_rt.prepass_loc_prev_transform, 1, GL_TRUE, mesh->transform_matrix);
        glBindVertexArray(mesh->geometry->vao);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh->geometry->ebo);
        glDrawElements(GL_TRIANGLES, mesh->geometry->index_count, GL_UNSIGNED_INT, NULL);
    }
    prepass_end();
}

// The viewmodel's depth history: the main denoiser's temporal pass (rt_denoise.frag pass 0) over the
// viewmodel's depth, so the reflect / GI denoisers get the (view depth, length) history their surface
// test reads (rt_depth_history). Its R / G (the trace's visibility / AO) are meaningless here and unread:
// the normal target stands in for the trace input.
static void vm_depth_history(const float view_projection[16], const float inverse_view_projection[16], const Camera *camera) {
    const unsigned int program = g_rt.denoise_program;
    use_program(program);
    bind_unit(2, g_rt.depth_texture);
    bind_unit(3, g_rt.normal_texture);
    bind_unit(5, 0);
    gl_uniform_1i(&u_rt_input, program, 0);
    gl_uniform_1i(&u_rt_history, program, 1);
    gl_uniform_1i(&u_rt_depth, program, 2);
    gl_uniform_1i(&u_rt_normal, program, 3);
    gl_uniform_mat4(&u_view_projection, program, GL_TRUE, view_projection);
    gl_uniform_mat4(&u_inverse_view_projection, program, GL_TRUE, inverse_view_projection);
    gl_uniform_mat4(&u_previous_view_projection, program, GL_TRUE, view_projection); // camera-locked: identity reprojection
    gl_uniform_3f(&u_camera_position, program, camera->position.x, camera->position.y, camera->position.z);
    gl_uniform_2f(&u_rt_texel, program, 1.0f / (float) g_rt.width, 1.0f / (float) g_rt.height);
    gl_uniform_1i(&u_history_valid, program, g_rt.history_valid);
    gl_uniform_1i(&u_rt_velocity, program, 5);
    gl_uniform_1i(&u_rt_velocity_valid, program, 0);
    const int read = g_rt.history_index, write = 1 - read;
    glBindFramebuffer(GL_FRAMEBUFFER, g_rt.history_fbo[write]);
    glViewport(0, 0, g_rt.width, g_rt.height);
    bind_unit(0, g_rt.normal_texture);
    bind_unit(1, g_rt.history_texture[read]);
    gl_uniform_1i(&u_pass, program, 0);
    gl_uniform_1i(&u_step_size, program, 1);
    fullscreen();
    g_rt.history_index = write;
    g_rt.history_valid = 1;
}

// KRUNKNATIVE_RT_VM_TIMING=1 (dev, desktop GL only): GPU time of each viewmodel sub-pass, read back at once
// (stalls the pipeline: for measuring the split only) and printed every 120 frames with the rectangle's share.
#define VM_T_MAX 8
static struct { int on, n; unsigned int q[VM_T_MAX + 1]; double sum[VM_T_MAX]; const char *name[VM_T_MAX]; int frames, moved, traced; double area; } g_vmt = {-1};
static void vm_t_mark(const char *name) {
#ifndef KN_GLES
    if (g_vmt.on < 0) { const char *e = getenv("KRUNKNATIVE_RT_VM_TIMING"); g_vmt.on = e && *e && strcmp(e, "0") != 0; if (g_vmt.on) glGenQueries(VM_T_MAX + 1, g_vmt.q); }
    if (!g_vmt.on || g_vmt.n > VM_T_MAX) return;
    if (g_vmt.n > 0) g_vmt.name[g_vmt.n - 1] = name;
    glQueryCounter(g_vmt.q[g_vmt.n++], GL_TIMESTAMP);
#else
    (void) name;
#endif
}
static void vm_t_end(void) {
#ifndef KN_GLES
    if (g_vmt.on <= 0) return;
    for (int i = 1; i < g_vmt.n; i++) {
        GLuint64 a = 0, b = 0;
        glGetQueryObjectui64v(g_vmt.q[i - 1], GL_QUERY_RESULT, &a);
        glGetQueryObjectui64v(g_vmt.q[i], GL_QUERY_RESULT, &b);
        g_vmt.sum[i - 1] += (double) (b - a) / 1e6;
    }
    const int marks = g_vmt.n;
    g_vmt.n = 0;
    if (marks < 2) return;
    if (++g_vmt.frames < 120) return;
    fprintf(stderr, "[vm] %d frames (moved %d, traced %d), rect %.1f%%:", g_vmt.frames, g_vmt.moved, g_vmt.traced, 100.0 * g_vmt.area / g_vmt.frames);
    g_vmt.moved = g_vmt.traced = 0;
    for (int i = 0; i < marks - 1; i++) fprintf(stderr, " %s %.3f", g_vmt.name[i] ? g_vmt.name[i] : "?", g_vmt.sum[i] / g_vmt.frames), g_vmt.sum[i] = 0.0;
    fprintf(stderr, " ms\n");
    g_vmt.frames = 0;
    g_vmt.area = 0.0;
#endif
}

// The screen rectangle (RT-resolution pixels) the fps scene's meshes can cover, padded by the denoisers' widest
// reach: the fullscreen passes of the viewmodel are scissored to it, since the weapon covers a corner of the frame
// and every other pixel would only early-out. Each mesh contributes its geometry's local bounding box (from the
// CPU triangle copy, cached per geometry and tri_epoch; the bounding sphere otherwise) through the fps camera,
// clipped against the near plane: the weapon sits a unit from the camera, and its bounding sphere (radius 5-12
// units) always reached behind it, which made every pass shade the whole frame.
#define VM_SCISSOR_PAD 16
#define VM_PERIOD 4 // frames between two traces of one viewmodel effect while the weapon holds still (rt_viewmodel_frame)
#define VM_BOX_CACHE 64
typedef struct { const Geometry *geometry; unsigned epoch; int count; float lo[3], hi[3]; } VmBox;
static VmBox g_vm_boxes[VM_BOX_CACHE];
static int g_vm_box_next;
// the motion test of rt_viewmodel_frame's amortisation: each visible mesh's projected box centre (RT pixels)
// this frame and the last; vm_scissor_rect fills g_vm_centre, vm_moved compares
#define VM_CENTRES 64
static float g_vm_centre[VM_CENTRES][2], g_vm_centre_last[VM_CENTRES][2];
static int g_vm_centres, g_vm_centres_last = -1;

// The camera's own motion counts too: the weapon is camera-locked, so while the player walks or turns it holds still
// on screen, yet the world it reflects and takes its bounce from changes every frame. Any change of the fps camera's
// world_inverse_matrix beyond float noise (a turn of ~0.06 degrees, a step of 0.01 units) is motion.
static float g_vm_camera_last[16];
static int g_vm_camera_valid;

static int vm_camera_moved(const Camera *camera) {
    int moved = !g_vm_camera_valid;
    for (int i = 0; i < 16 && !moved; i++) {
        const float tolerance = (i == 3 || i == 7 || i == 11) ? 0.01f : 0.001f; // translation column vs rotation
        moved = fabsf(camera->world_inverse_matrix[i] - g_vm_camera_last[i]) > tolerance;
    }
    memcpy(g_vm_camera_last, camera->world_inverse_matrix, sizeof(g_vm_camera_last));
    g_vm_camera_valid = 1;
    return moved;
}

static int vm_moved(void) {
    int moved = g_vm_centres != g_vm_centres_last;
    for (int i = 0; i < g_vm_centres && !moved; i++)
        moved = fabsf(g_vm_centre[i][0] - g_vm_centre_last[i][0]) > 1.0f || fabsf(g_vm_centre[i][1] - g_vm_centre_last[i][1]) > 1.0f;
    memcpy(g_vm_centre_last, g_vm_centre, sizeof(g_vm_centre));
    g_vm_centres_last = g_vm_centres;
    return moved;
}

static int vm_local_box(const Geometry *geometry, float lo[3], float hi[3]) {
    if (geometry->tri_positions && geometry->tri_vertex_count > 0) {
        for (int i = 0; i < VM_BOX_CACHE; i++) {
            const VmBox *b = &g_vm_boxes[i];
            if (b->geometry == geometry && b->epoch == geometry->tri_epoch && b->count == geometry->tri_vertex_count) {
                memcpy(lo, b->lo, sizeof(b->lo)); memcpy(hi, b->hi, sizeof(b->hi));
                return 1;
            }
        }
        VmBox *b = &g_vm_boxes[g_vm_box_next];
        g_vm_box_next = (g_vm_box_next + 1) % VM_BOX_CACHE;
        const float *p = geometry->tri_positions;
        for (int k = 0; k < 3; k++) b->lo[k] = b->hi[k] = p[k];
        for (int v = 1; v < geometry->tri_vertex_count; v++)
            for (int k = 0; k < 3; k++) {
                const float x = p[v * 3 + k];
                if (x < b->lo[k]) b->lo[k] = x;
                if (x > b->hi[k]) b->hi[k] = x;
            }
        b->geometry = geometry; b->epoch = geometry->tri_epoch; b->count = geometry->tri_vertex_count;
        memcpy(lo, b->lo, sizeof(b->lo)); memcpy(hi, b->hi, sizeof(b->hi));
        return 1;
    }
    if (geometry->bound_radius <= 0.0f) return 0;
    for (int k = 0; k < 3; k++) { lo[k] = geometry->bound_center[k] - geometry->bound_radius; hi[k] = geometry->bound_center[k] + geometry->bound_radius; }
    return 1;
}

static void vm_scissor_rect(const Scene *scene, const float view_projection[16], int rect[4]) {
    float min_x = 1.0f, min_y = 1.0f, max_x = -1.0f, max_y = -1.0f;
    int any = 0, whole = 0;
    g_vm_centres = 0;
    for (size_t i = 0; i < scene->mesh_count && !whole; i++) {
        const Mesh *mesh = scene->meshes[i];
        const Geometry *geometry = mesh->geometry;
        if (!mesh->visible || !geometry || !mesh->material || mesh->material->vtable != &basic_material_vtable) continue;
        float lo[3], hi[3];
        if (!vm_local_box(geometry, lo, hi)) { whole = 1; break; }
        float mvp[16];
        mat4x4(view_projection, mesh->transform_matrix, mvp); // row-major, like scene.c
        // clip space x, y, w and the near-plane distance d = z + w (GL: visible where z >= -w)
        float clip[8][4];
        for (int c = 0; c < 8; c++) {
            const float p[3] = {c & 1 ? hi[0] : lo[0], c & 2 ? hi[1] : lo[1], c & 4 ? hi[2] : lo[2]};
            clip[c][0] = mvp[0] * p[0] + mvp[1] * p[1] + mvp[2] * p[2] + mvp[3];
            clip[c][1] = mvp[4] * p[0] + mvp[5] * p[1] + mvp[6] * p[2] + mvp[7];
            clip[c][2] = mvp[12] * p[0] + mvp[13] * p[1] + mvp[14] * p[2] + mvp[15];
            clip[c][3] = mvp[8] * p[0] + mvp[9] * p[1] + mvp[10] * p[2] + mvp[11] + clip[c][2];
        }
        if (g_vm_centres < VM_CENTRES) { // the box centre's screen position (the motion test), 0 0 behind the camera
            float cx = 0.0f, cy = 0.0f, cw = 0.0f;
            for (int c = 0; c < 8; c++) { cx += clip[c][0]; cy += clip[c][1]; cw += clip[c][2]; }
            float *out = g_vm_centre[g_vm_centres++];
            out[0] = cw > 1e-4f ? (cx / cw * 0.5f + 0.5f) * (float) g_rt.width : 0.0f;
            out[1] = cw > 1e-4f ? (cy / cw * 0.5f + 0.5f) * (float) g_rt.height : 0.0f;
        }
        // the corners in front of the near plane, plus where the box's edges cross it (there w = the near
        // distance, so they project to finite points)
        float pts[8 + 12][3];
        int n = 0;
        for (int c = 0; c < 8; c++) if (clip[c][3] >= 0.0f && clip[c][2] > 1e-6f) { pts[n][0] = clip[c][0]; pts[n][1] = clip[c][1]; pts[n][2] = clip[c][2]; n++; }
        for (int a = 0; a < 8; a++)
            for (int k = 0; k < 3; k++) {
                const int b = a | (1 << k);
                if (b == a) continue;
                const float da = clip[a][3], db = clip[b][3];
                if ((da >= 0.0f) == (db >= 0.0f)) continue;
                const float t = da / (da - db);
                const float w = clip[a][2] + (clip[b][2] - clip[a][2]) * t;
                if (w <= 1e-6f) { whole = 1; break; } // degenerate projection (no near plane): be conservative
                pts[n][0] = clip[a][0] + (clip[b][0] - clip[a][0]) * t;
                pts[n][1] = clip[a][1] + (clip[b][1] - clip[a][1]) * t;
                pts[n][2] = w;
                n++;
            }
        if (whole) break;
        for (int k = 0; k < n; k++) {
            float x = pts[k][0] / pts[k][2], y = pts[k][1] / pts[k][2];
            if (x < -1.0f) x = -1.0f; else if (x > 1.0f) x = 1.0f;
            if (y < -1.0f) y = -1.0f; else if (y > 1.0f) y = 1.0f;
            if (x < min_x) min_x = x;
            if (x > max_x) max_x = x;
            if (y < min_y) min_y = y;
            if (y > max_y) max_y = y;
            any = 1;
        }
    }
    if (whole || !any) { rect[0] = rect[1] = 0; rect[2] = g_rt.width; rect[3] = g_rt.height; return; }
    int x0 = (int) floorf((min_x * 0.5f + 0.5f) * (float) g_rt.width) - VM_SCISSOR_PAD;
    int y0 = (int) floorf((min_y * 0.5f + 0.5f) * (float) g_rt.height) - VM_SCISSOR_PAD;
    int x1 = (int) ceilf((max_x * 0.5f + 0.5f) * (float) g_rt.width) + VM_SCISSOR_PAD;
    int y1 = (int) ceilf((max_y * 0.5f + 0.5f) * (float) g_rt.height) + VM_SCISSOR_PAD;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > g_rt.width) x1 = g_rt.width;
    if (y1 > g_rt.height) y1 = g_rt.height;
    if (x1 <= x0 || y1 <= y0) { x0 = y0 = 0; x1 = 1; y1 = 1; } // off screen: a 1 px rectangle, nothing to shade
    rect[0] = x0; rect[1] = y0; rect[2] = x1 - x0; rect[3] = y1 - y0;
}

// The viewmodel rectangle scaled from RT resolution (rect) to a pass's own (`width` x `height`)
static void vm_scissor(const int rect[4], const int width, const int height) {
    const float sx = (float) width / (float) g_rt.width, sy = (float) height / (float) g_rt.height;
    const int x0 = (int) floorf((float) rect[0] * sx), y0 = (int) floorf((float) rect[1] * sy);
    const int x1 = (int) ceilf((float) (rect[0] + rect[2]) * sx), y1 = (int) ceilf((float) (rect[1] + rect[3]) * sy);
    glScissor(x0, y0, x1 > x0 ? x1 - x0 : 1, y1 > y0 ? y1 - y0 : 1);
}

void rt_viewmodel_frame(const Scene *fps_scene, Camera *fps_camera) {
    const ClientRenderSettings *s = client_render_settings();
    g_rt.vm_ready = 0;
    g_rt.vm_effects = 0;
    const int traced = g_rt.traced_frame;
    g_rt.traced_frame = 0; // one viewmodel pass per traced frame
    if (!traced || !fps_scene || !fps_camera || !fps_scene->mesh_count || rt_no_viewmodel()) return;
    // the material-level bits (sky ambient, the class table) reach the fps draw at any traced rt_mode: the
    // weapon's class shading and its sky-specular fallback do not need the per-pixel passes below (mode >= AO)
    g_rt.vm_effects = g_rt.effects & (RT_EFFECT_SKY_AMBIENT | RT_EFFECT_MATERIALS | RT_EFFECT_CINEMATIC);
    if ((g_rt.effects & RT_EFFECT_MATERIALS) && g_rt.sky_probe_valid && !rt_no_spec_occlusion()) g_rt.vm_effects |= RT_EFFECT_SKY_PROBE; // rt.h
    g_rt.vm_ready = g_rt.vm_effects != 0;
    if (s->rt_mode < RT_MODE_AO || !g_rt.width || !g_rt.height) return;
    const int want_refl = s->rt_mode >= RT_MODE_REFLECTIONS && refl_ready(); // cached checks (rt_frame ran them)
    const int want_gi = gi_ready();
    if (!want_refl && !want_gi) return;

    int viewport[4], previous_fbo;
    glGetIntegerv(GL_VIEWPORT, viewport);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous_fbo);
    if (viewport[2] <= 0 || viewport[3] <= 0) return;
    // the fps draw's camera matrices (scene_render recomputes the same; the TAA jitter is in camera.c's
    // projection, so the prepass and the fps draw agree)
    camera_update_projection_matrix(fps_camera, (float) viewport[2] / (float) viewport[3]);
    camera_update_world_inverse_matrix(fps_camera);
    float view_projection[16], inverse_view_projection[16];
    mat4x4(fps_camera->projection_matrix, fps_camera->world_inverse_matrix, view_projection);
    if (!mat4_invert(view_projection, inverse_view_projection)) return;
    save_units();
    bind_unit(RT_REFL_TEXTURE_UNIT, 0); // the world's results stay bound from rt_begin_world: not while ours render
    bind_unit(RT_GI_TEXTURE_UNIT, 0);
    glActiveTexture(GL_TEXTURE0);

    // the viewmodel's set runs at the world's RT resolution / vm_divisor() (rt.h RT_VM_DIVISOR): the weapon's rays
    // are the costliest of the frame per pixel, and its reflection / bounce are sampled bilinearly anyway
    const int div = vm_divisor();
    const int width = g_rt.width / div > 0 ? g_rt.width / div : 1, height = g_rt.height / div > 0 ? g_rt.height / div : 1;
    const int world_refl_active = g_rt.refl_active;
    vm_swap();
    if (vm_ensure_targets(width, height)) {
        memcpy(g_rt.previous_view_projection, view_projection, sizeof(view_projection)); // camera-locked (see above)
        const int refl = want_refl && g_rt.material_attached && g_rt.refl_fbo;
        const int gi = want_gi && g_rt.gi_fbo;
        if (refl || gi) {
            g_rt.refl_active = refl;
            g_rt.vm_running = 1;
            int rect[4];
            vm_scissor_rect(fps_scene, view_projection, rect); // the fullscreen passes: the weapon's rectangle only
            g_vmt.area += (double) rect[2] * rect[3] / ((double) g_rt.width * g_rt.height);
            // Amortisation: the weapon is camera-locked and its reflection / bounce are accumulated over frames
            // already, so a result a few frames old does not show while the weapon holds still on screen. Each
            // effect is traced (prepass, trace, denoise) once per VM_PERIOD frames, the two staggered; the rest
            // reuse the last result. While any weapon / arm mesh moves on screen (aim, swap, reload, bob) or the camera
            // moves or turns (vm_camera_moved: the world around the camera-locked weapon changes) the period
            // shrinks to 2 (one effect a frame), so nothing is more than a frame old while it moves. A result
            // that is not valid yet is traced at once. KRUNKNATIVE_RT_VM_EVERY=1 (dev): both every frame.
            int trace_refl = refl, trace_gi = gi;
            const int mesh_moved = vm_moved(); // any weapon / arm mesh moved by more than an RT pixel on screen
            const int moved = vm_camera_moved(fps_camera) | mesh_moved; // both run: each keeps its last state
            g_vmt.moved += moved;
            g_vm_motion = moved; // prepass_scene: a short denoiser history while the view moves
            const int refl_ok = g_rt.refl_history_valid && g_rt.refl_result_texture;
            const int gi_ok = g_rt.gi_history_valid && g_rt.gi_result_texture;
            // While anything moves (bob, jump, walk, turn, aim) both effects are traced every frame: a period of 2
            // updated the weapon's lighting at half the frame rate in visible steps against the bob (the user: "when i
            // jump/anything bobs the raytraced shit just stutters behind"). Amortisation is for a still view only.
            if (!vm_every_frame() && !moved && (!refl || refl_ok) && (!gi || gi_ok)) {
                const int period = VM_PERIOD, phase = g_rt.frame_index % period;
                if (refl && gi) { trace_refl = phase == 0; trace_gi = phase == period / 2; }
                else { trace_refl = refl && phase == 0; trace_gi = gi && phase == 0; }
            }
            g_vmt.traced += trace_refl || trace_gi;
            vm_t_mark(NULL);
            if (trace_refl || trace_gi) prepass_scene(fps_scene, view_projection, refl); // the whole target: cleared, the weapon drawn
            vm_t_mark("prepass");
            glEnable(GL_SCISSOR_TEST);
            vm_scissor(rect, g_rt.width, g_rt.height);
            if (trace_refl) reflect_pass(inverse_view_projection, fps_camera);
            vm_t_mark("refl");
            if (trace_gi) { vm_scissor(rect, g_rt.gi_width, g_rt.gi_height); gi_pass(inverse_view_projection, fps_camera); }
            vm_t_mark("gi");
            const int history_was_valid = g_rt.history_valid, history_read = g_rt.history_index;
            if (s->rt_denoise && (trace_refl || trace_gi)) {
                vm_scissor(rect, g_rt.width, g_rt.height);
                vm_depth_history(view_projection, inverse_view_projection, fps_camera);
                vm_t_mark("dhist");
                if (trace_refl) refl_denoise(inverse_view_projection, fps_camera, g_rt.history_texture[history_read], history_was_valid);
                vm_t_mark("rdn");
                if (trace_gi) { vm_scissor(rect, g_rt.gi_width, g_rt.gi_height); gi_denoise(inverse_view_projection, fps_camera, g_rt.history_texture[history_read], history_was_valid); }
                vm_t_mark("gidn");
            } else if (!s->rt_denoise) {
                if (trace_refl) g_rt.refl_result_texture = g_rt.refl_texture;
                if (trace_gi) g_rt.gi_result_texture = g_rt.gi_texture;
            }
            g_rt.vm_refl_result_texture = refl ? g_rt.refl_result_texture : 0;
            g_rt.vm_gi_result_texture = gi ? g_rt.gi_result_texture : 0;
            // the world's material-level bits (sky ambient, the material table) carry over; the per-pixel ones
            // (sun shadow, AO, local lights) were not traced here
            int effects = g_rt.effects & ~(RT_EFFECT_SHADOWS | RT_EFFECT_AO | RT_EFFECT_LOCAL_LIGHTS | RT_EFFECT_REFLECTIONS | RT_EFFECT_GI | RT_EFFECT_FORCE_REFL | RT_EFFECT_SKY_VIS);
            effects |= g_rt.vm_effects & RT_EFFECT_SKY_PROBE; // the camera probe (rt_result.g is the world's, not the weapon's)
            if (refl) effects |= RT_EFFECT_REFLECTIONS | (g_rt.effects & RT_EFFECT_FORCE_REFL);
            if (gi) effects |= RT_EFFECT_GI;
            g_rt.vm_effects = effects;
            g_rt.vm_ready = 1;
        }
    }
    glDisable(GL_SCISSOR_TEST);
    vm_t_end();
    vm_swap();
    g_rt.vm_running = 0;
    g_rt.refl_active = world_refl_active;
    restore_state(previous_fbo, viewport);
}

void rt_begin_viewmodel(void) {
    g_rt.vm_drawing = 1;
    if (!g_rt.vm_ready) return;
    g_rt.vm_active = 1;
    if ((g_rt.vm_effects & RT_EFFECT_REFLECTIONS) && g_rt.vm_refl_result_texture) bind_unit(RT_REFL_TEXTURE_UNIT, g_rt.vm_refl_result_texture);
    if ((g_rt.vm_effects & RT_EFFECT_GI) && g_rt.vm_gi_result_texture) bind_unit(RT_GI_TEXTURE_UNIT, g_rt.vm_gi_result_texture);
    glActiveTexture(GL_TEXTURE0);
}

void rt_end_viewmodel(void) {
    g_rt.vm_active = 0;
    g_rt.vm_drawing = 0;
}

int rt_viewmodel_drawing(void) { return g_rt.vm_drawing; }


// ---------------------------------------------------------------- water and glass (rt.h)
//
// Transparent water / glass meshes of the world scene: the opaque frame copied between the opaque and the
// transparent draws (rt_water_copy_scene, from scene.c) for the refraction, and at rtMode 3 a traced reflection of
// their own: a prepass of those meshes alone on a third target set (g_wt, swapped in like the viewmodel's g_vm) and
// the shared reflect + reflection denoise passes. basic.frag composites (krunker_rt_water).

static RtState g_wt; // only the VM_SWAP_FIELDS members are used
#define WT_SWAP_ONE(f) { unsigned char t[sizeof(g_rt.f)]; memcpy(t, &g_rt.f, sizeof t); memcpy(&g_rt.f, &g_wt.f, sizeof t); memcpy(&g_wt.f, t, sizeof t); }
static void wt_swap(void) {
    VM_SWAP_FIELDS(WT_SWAP_ONE)
}

static struct {
    int active;                 // this frame's world draw composites water / glass (RT_EFFECT_WATER on those meshes)
    Mesh *meshes[RT_WATER_MAX]; // the visible water / glass meshes found by water_frame
    int mesh_count;
    unsigned int refl_texture;  // this frame's traced reflection of them (g_wt's result), 0 = the sky estimate
    unsigned int copy_fbo, copy_texture; // the opaque frame (rt_water_copy_scene)
    int copy_width, copy_height;
    unsigned int copy_internal;
    int copy_valid;             // the copy holds this frame's opaque world
    float depth_ab[2];          // the world camera's projection A, B: view depth = B / (ndc z + A)
} g_water;

static int rt_no_water(void) { // KRUNKNATIVE_RT_NO_WATER=1 (dev only): Krunker's alpha blend for water / glass (A/B)
    static int no_water = -1;
    if (no_water < 0) { const char *e = getenv("KRUNKNATIVE_RT_NO_WATER"); no_water = e && *e && strcmp(e, "0") != 0; }
    return no_water;
}

// The table's "water" class (the class a texture named water_0 resolves to), looked up once a frame by water_frame
static const RtMaterialClass *water_table_class(void) {
    const RtMaterialKey key = {"water_0", NULL, -1, RT_PART_NONE, -1};
    const RtMaterialClass *c = rt_materials_lookup(&key);
    return c && !strcmp(c->name, "water") ? c : NULL;
}

// 1 water, 2 glass, 0 neither: a transparent, lit BasicMaterial whose resolved class is water / glass / window, or a
// Krunker LIQUID plane (prefab.c gives every liquid a wave height; most are untextured colours, which no texture rule
// can class, so the table's water class stands in for them). Glowing liquids (an emissive colour) keep Krunker's look.
// *class_out: the class the draw shades with.
static int water_kind_class(const Mesh *mesh, const RtMaterialClass **class_out) {
    if (class_out) *class_out = NULL;
    const Material *material = mesh->material;
    if (!material || material->vtable != &basic_material_vtable || !material->transparent) return 0;
    const BasicMaterial *m = (const BasicMaterial *) material;
    if (m->unlit || m->sprite || m->additive_blend || material->wireframe) return 0;
    const RtMaterialClass *c = (const RtMaterialClass *) m->rt_class;
    if (c && !strcmp(c->name, "water")) { if (class_out) *class_out = c; return 1; }
    if (c && (!strcmp(c->name, "glass") || !strcmp(c->name, "window"))) { if (class_out) *class_out = c; return 2; }
    if (m->wave_height > 0.0f && m->emissive.x <= 0.0f && m->emissive.y <= 0.0f && m->emissive.z <= 0.0f) {
        const RtMaterialClass *w = water_table_class();
        if (!w) return 0;
        if (class_out) *class_out = w;
        return 1;
    }
    return 0;
}
static int water_kind(const Mesh *mesh) { return water_kind_class(mesh, NULL); }

// The prepass over the water / glass meshes (prepass_scene without its transparency skip): their depth, normal and
// the reflect / roughness / metalness the reflect pass keys on. World-fixed surfaces: not "dynamic", no mesh motion.
static void prepass_water(const Scene *water, const float view_projection[16]) {
    prepass_begin(view_projection, view_projection);
    int culling = -1;
    for (size_t i = 0; i < water->mesh_count; i++) {
        Mesh *mesh = water->meshes[i];
        const BasicMaterial *bm = (const BasicMaterial *) mesh->material;
        const int cull = !bm->double_sided;
        if (cull != culling) {
            if (cull) glEnable(GL_CULL_FACE);
            else glDisable(GL_CULL_FACE);
            culling = cull;
        }
        if (cull) glFrontFace((determinant3(mesh->transform_matrix) < 0.0f) != (bm->cull_front != 0) ? GL_CW : GL_CCW);
        glUniformMatrix4fv(g_rt.prepass_loc_transform, 1, GL_TRUE, mesh->transform_matrix);
        glUniform1f(g_rt.prepass_loc_dynamic, 0.0f);
        if (g_rt.prepass_loc_material >= 0) {
            float material[3];
            const RtMaterialClass *c;
            water_kind_class(mesh, &c);
            if (c) { material[0] = c->reflect > 0.0f ? c->reflect : 1.0f; material[1] = c->roughness; material[2] = c->metalness; } // the water / glass class
            else vm_material(bm, material, 1);
            glUniform3f(g_rt.prepass_loc_material, material[0], material[1], material[2]);
        }
        glUniformMatrix4fv(g_rt.prepass_loc_prev_transform, 1, GL_TRUE, mesh->transform_matrix);
        glBindVertexArray(mesh->geometry->vao);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh->geometry->ebo);
        glDrawElements(GL_TRIANGLES, mesh->geometry->index_count, GL_UNSIGNED_INT, NULL);
    }
    prepass_end();
}

// vm_depth_history with a real previous camera (water is world-fixed: the camera's motion reprojects it)
static void water_depth_history(const float view_projection[16], const float inverse_view_projection[16], const Camera *camera,
                                const float previous_view_projection[16]) {
    const unsigned int program = g_rt.denoise_program;
    use_program(program);
    bind_unit(2, g_rt.depth_texture);
    bind_unit(3, g_rt.normal_texture);
    bind_unit(5, 0);
    gl_uniform_1i(&u_rt_input, program, 0);
    gl_uniform_1i(&u_rt_history, program, 1);
    gl_uniform_1i(&u_rt_depth, program, 2);
    gl_uniform_1i(&u_rt_normal, program, 3);
    gl_uniform_mat4(&u_view_projection, program, GL_TRUE, view_projection);
    gl_uniform_mat4(&u_inverse_view_projection, program, GL_TRUE, inverse_view_projection);
    gl_uniform_mat4(&u_previous_view_projection, program, GL_TRUE, previous_view_projection);
    gl_uniform_3f(&u_camera_position, program, camera->position.x, camera->position.y, camera->position.z);
    gl_uniform_2f(&u_rt_texel, program, 1.0f / (float) g_rt.width, 1.0f / (float) g_rt.height);
    gl_uniform_1i(&u_history_valid, program, g_rt.history_valid);
    gl_uniform_1i(&u_rt_velocity, program, 5);
    gl_uniform_1i(&u_rt_velocity_valid, program, 0);
    const int read = g_rt.history_index, write = 1 - read;
    glBindFramebuffer(GL_FRAMEBUFFER, g_rt.history_fbo[write]);
    glViewport(0, 0, g_rt.width, g_rt.height);
    bind_unit(0, g_rt.normal_texture);
    bind_unit(1, g_rt.history_texture[read]);
    gl_uniform_1i(&u_pass, program, 0);
    gl_uniform_1i(&u_step_size, program, 1);
    fullscreen();
    g_rt.history_index = write;
    g_rt.history_valid = 1;
}

// rt_frame, after the world's passes: finds the visible water / glass meshes and, at rtMode 3, traces their reflection.
// `velocity_view_projection` is this frame's unjittered camera (the denoisers' reprojection pair).
static void water_frame(const Scene *scene, const Camera *camera, const float view_projection[16], const float inverse_view_projection[16],
                        const float velocity_view_projection[16], const int effects) {
    g_water.active = 0;
    g_water.mesh_count = 0;
    g_water.refl_texture = 0;
    g_water.copy_valid = 0;
    if (rt_no_water() || !(effects & RT_EFFECT_MATERIALS) || !scene) return;
    for (size_t i = 0; i < scene->mesh_count && g_water.mesh_count < RT_WATER_MAX; i++) {
        Mesh *mesh = scene->meshes[i];
        if (!mesh->visible || !mesh->geometry || !mesh->geometry->vao || mesh->geometry->draw_mode) continue;
        if (water_kind(mesh)) g_water.meshes[g_water.mesh_count++] = mesh;
    }
    if (!g_water.mesh_count) return;
    g_water.active = 1;
    // the world camera's depth linearisation for basic.frag's thickness (row-major projection: m[10] = A, m[11] = B)
    g_water.depth_ab[0] = camera->projection_matrix[10];
    g_water.depth_ab[1] = camera->projection_matrix[11];
    if (!g_rt.refl_active || !g_rt.width || !g_rt.height) return; // rtMode < 3: basic.frag's sky estimate

    // the reflection at 1 / RT_WATER_DIVISOR of the RT resolution: a water / glass reflection is the costliest ray of the
    // frame (an incoherent closest hit plus its shadow ray) and the surface often fills half the screen; basic.frag's
    // bilinear lookup upsamples it, and the waves blur it anyway
    const int width = g_rt.width / RT_WATER_DIVISOR > 0 ? g_rt.width / RT_WATER_DIVISOR : 1;
    const int height = g_rt.height / RT_WATER_DIVISOR > 0 ? g_rt.height / RT_WATER_DIVISOR : 1;
    const int denoise = client_render_settings()->rt_denoise;
    wt_swap();
    if (vm_ensure_targets(width, height) && g_rt.material_attached && g_rt.refl_fbo) {
        mesh_update_transform_matrix(g_water.meshes[0]); // (scene_render recomputes them; the prepass needs them now)
        for (int i = 1; i < g_water.mesh_count; i++) mesh_update_transform_matrix(g_water.meshes[i]);
        Scene water = {(size_t) g_water.mesh_count, g_water.meshes, 0};
        prepass_water(&water, view_projection);
        int rect[4];
        vm_scissor_rect(&water, view_projection, rect); // liquids have no bounds (waves): the whole frame, early-outs off them
        glEnable(GL_SCISSOR_TEST);
        vm_scissor(rect, g_rt.width, g_rt.height);
        reflect_pass(inverse_view_projection, camera);
        if (denoise) {
            const int history_was_valid = g_rt.history_valid, history_read = g_rt.history_index;
            float previous[16];
            memcpy(previous, history_was_valid ? g_rt.previous_view_projection : velocity_view_projection, sizeof(previous));
            water_depth_history(velocity_view_projection, inverse_view_projection, camera, previous);
            refl_denoise(inverse_view_projection, camera, g_rt.history_texture[history_read], history_was_valid);
        } else {
            g_rt.refl_result_texture = g_rt.refl_texture;
        }
        memcpy(g_rt.previous_view_projection, velocity_view_projection, sizeof(g_rt.previous_view_projection));
        g_water.refl_texture = g_rt.refl_result_texture;
        glDisable(GL_SCISSOR_TEST);
    }
    wt_swap();
}

static void water_delete_copy(void) {
    if (g_water.copy_fbo) glDeleteFramebuffers(1, &g_water.copy_fbo);
    if (g_water.copy_texture) gl_release_texture(g_water.copy_texture), glDeleteTextures(1, &g_water.copy_texture);
    g_water.copy_fbo = g_water.copy_texture = 0;
    g_water.copy_width = g_water.copy_height = 0;
    g_water.copy_valid = 0;
}

void rt_water_copy_scene(const Scene *scene) {
    (void) scene;
    g_water.copy_valid = 0;
    if (!g_water.active || !g_rt.world_active) return;
    int viewport[4], fbo;
    glGetIntegerv(GL_VIEWPORT, viewport);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
    const int width = viewport[0] + viewport[2], height = viewport[1] + viewport[3];
    if (viewport[2] <= 0 || viewport[3] <= 0) return;
    // the copy's format matches the frame's (a multisample resolve blit wants identical formats): the scene target is
    // RGBA16F with rtBloom, else RGBA8 (render_target.c); the default framebuffer is taken as RGBA8
    unsigned int internal = GL_RGBA8;
    if (fbo) {
        int type = 0;
        glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE, &type);
        if (type == GL_FLOAT) internal = GL_RGBA16F;
    }
    if (!g_water.copy_fbo || g_water.copy_width != width || g_water.copy_height != height || g_water.copy_internal != internal) {
        water_delete_copy();
        glActiveTexture(GL_TEXTURE0 + RT_WATER_SCENE_UNIT); // the opaque draws are over: rt_result's unit is free
        g_water.copy_texture = make_texture(width, height, internal, GL_RGBA, internal == GL_RGBA16F ? GL_HALF_FLOAT : GL_UNSIGNED_BYTE, 1);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE0);
        g_water.copy_fbo = make_fbo(g_water.copy_texture, 0);
        g_water.copy_width = width;
        g_water.copy_height = height;
        g_water.copy_internal = internal;
        glBindFramebuffer(GL_FRAMEBUFFER, (unsigned int) fbo);
        if (!g_water.copy_fbo) {
            static int logged;
            if (!logged) logged = 1, fprintf(stderr, "[rt] water: copy target incomplete, water / glass keep the alpha blend\n");
            water_delete_copy();
            return;
        }
    }
    const int scissor = glIsEnabled(GL_SCISSOR_TEST);
    if (scissor) glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (unsigned int) fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_water.copy_fbo);
    glBlitFramebuffer(viewport[0], viewport[1], width, height, viewport[0], viewport[1], width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, (unsigned int) fbo);
    if (scissor) glEnable(GL_SCISSOR_TEST);
    g_water.copy_valid = 1;
}

// rt_apply_mesh_uniforms for a transparent mesh of the world draw: RT_EFFECT_WATER (with the material-level bits)
// and its textures / uniforms when the mesh is water / glass, else 0 (Krunker's alpha blend)
static int water_effects(const Mesh *mesh, const unsigned int program) {
    if (!g_water.active) return 0;
    const RtMaterialClass *c;
    const int kind = water_kind_class(mesh, &c);
    if (!kind || !c) return 0;
    static GlUniform u_kind = GL_UNIFORM_INIT("rt_water_kind");
    static GlUniform u_copy = GL_UNIFORM_INIT("rt_water_copy");
    static GlUniform u_traced = GL_UNIFORM_INIT("rt_water_traced");
    static GlUniform u_scene = GL_UNIFORM_INIT("rt_water_scene");
    static GlUniform u_depth = GL_UNIFORM_INIT("rt_water_depth");
    static GlUniform u_inv_size = GL_UNIFORM_INIT("rt_water_inv_size");
    static GlUniform u_depth_ab = GL_UNIFORM_INIT("rt_water_depth_ab");
    static GlUniform u_refract = GL_UNIFORM_INIT("rt_water_refract");
    static GlUniform u_absorb = GL_UNIFORM_INIT("rt_water_absorb_depth");
    static GlUniform u_refl = GL_UNIFORM_INIT("rt_refl");
    if (gl_uniform_location(&u_kind, program) < 0) return 0;
    gl_uniform_1i(&u_kind, program, kind);
    gl_uniform_1i(&u_copy, program, g_water.copy_valid);
    gl_uniform_1i(&u_traced, program, g_water.refl_texture != 0);
    gl_uniform_1i(&u_scene, program, RT_WATER_SCENE_UNIT);
    gl_uniform_1i(&u_depth, program, RT_WATER_DEPTH_UNIT);
    gl_uniform_1i(&u_refl, program, RT_REFL_TEXTURE_UNIT);
    gl_uniform_2f(&u_inv_size, program, g_water.copy_width ? 1.0f / (float) g_water.copy_width : 0.0f,
                  g_water.copy_height ? 1.0f / (float) g_water.copy_height : 0.0f);
    gl_uniform_2f(&u_depth_ab, program, g_water.depth_ab[0], g_water.depth_ab[1]);
    gl_uniform_1f(&u_refract, program, RT_WATER_REFRACT);
    gl_uniform_1f(&u_absorb, program, RT_WATER_DEPTH);
    // the class factors of this draw (basic.c uploaded the material's own class, e.g. the default class of an untextured
    // liquid): the water / glass class's. Same uniform cache entries as basic.c's, so its next upload is not skipped.
    static GlUniform u_mat_enabled = GL_UNIFORM_INIT("rt_mat_enabled");
    static GlUniform u_mat_metalness = GL_UNIFORM_INIT("rt_mat_metalness");
    static GlUniform u_mat_roughness = GL_UNIFORM_INIT("rt_mat_roughness");
    static GlUniform u_mat_reflect = GL_UNIFORM_INIT("rt_mat_reflect");
    static GlUniform u_mat_f0 = GL_UNIFORM_INIT("rt_mat_f0");
    gl_uniform_1i(&u_mat_enabled, program, 1);
    gl_uniform_1f(&u_mat_metalness, program, c->metalness);
    gl_uniform_1f(&u_mat_roughness, program, c->roughness);
    gl_uniform_1f(&u_mat_reflect, program, c->reflect);
    gl_uniform_1f(&u_mat_f0, program, c->f0);
    // transparent draws come after every opaque draw of the world scene: these units' world results are no longer read
    bind_unit(RT_WATER_SCENE_UNIT, g_water.copy_valid ? g_water.copy_texture : 0);
    bind_unit(RT_WATER_DEPTH_UNIT, g_rt.depth_texture);
    bind_unit(RT_REFL_TEXTURE_UNIT, g_water.refl_texture);
    glActiveTexture(GL_TEXTURE0);
    // the sky reflection's occlusion: the camera's directional sky probe (rt.h "specular occlusion"), when it ran this
    // frame; the per-pixel AO / sky visibility describe the opaque surface behind the water, not the water
    const int probe = g_rt.sky_probe_valid && !rt_no_spec_occlusion() ? RT_EFFECT_SKY_PROBE : 0;
    return (g_rt.effects & (RT_EFFECT_SKY_AMBIENT | RT_EFFECT_MATERIALS)) | RT_EFFECT_WATER | probe;
}

// The RT_EFFECT_* bits of the draw in progress: the world's between rt_begin_world / rt_end_world, the
// viewmodel's between rt_begin_viewmodel / rt_end_viewmodel, else none
static int active_effects(void) {
    if (g_rt.world_active) return g_rt.effects;
    if (g_rt.vm_active) return g_rt.vm_effects;
    return 0;
}

// ---------------------------------------------------------------- TAA (render_target.c)

unsigned int rt_taa_velocity(int *width, int *height, float sky_reproject[16]) {
    if (!g_rt.velocity_ready || !g_rt.velocity_texture) return 0;
    if (width) *width = g_rt.width;
    if (height) *height = g_rt.height;
    if (sky_reproject) {
        if (!g_rt.taa_sky_cached) { // previous * inverse(current), both unjittered: where no surface wrote a velocity
            float inverse[16];
            if (mat4_invert(g_rt.taa_view_projection, inverse)) mat4x4(g_rt.taa_previous_view_projection, inverse, g_rt.taa_sky_reproject);
            else { memset(g_rt.taa_sky_reproject, 0, sizeof(g_rt.taa_sky_reproject)); for (int i = 0; i < 4; i++) g_rt.taa_sky_reproject[i * 5] = 1.0f; }
            g_rt.taa_sky_cached = 1;
        }
        memcpy(sky_reproject, g_rt.taa_sky_reproject, sizeof(g_rt.taa_sky_reproject));
    }
    return g_rt.velocity_texture;
}

unsigned int rt_history_epoch(void) {
    return g_rt.history_epoch;
}

int rt_world_effects(void) {
    return active_effects(); // the viewmodel's bits too, between rt_begin_viewmodel / rt_end_viewmodel
}

void rt_begin_world(void) {
    if (!g_rt.world_active || !g_rt.result_texture) return;
    bind_unit(RT_TEXTURE_UNIT, g_rt.result_texture);
    if ((g_rt.effects & RT_EFFECT_LOCAL_LIGHTS) && g_rt.local_result_texture) bind_unit(RT_LOCAL_TEXTURE_UNIT, g_rt.local_result_texture);
    if ((g_rt.effects & RT_EFFECT_REFLECTIONS) && g_rt.refl_result_texture) bind_unit(RT_REFL_TEXTURE_UNIT, g_rt.refl_result_texture);
    if ((g_rt.effects & RT_EFFECT_GI) && g_rt.gi_result_texture) bind_unit(RT_GI_TEXTURE_UNIT, g_rt.gi_result_texture);
    glActiveTexture(GL_TEXTURE0);
}

static GlUniform u_rt_effects = GL_UNIFORM_INIT("rt_effects");
static GlUniform u_rt_inv_viewport = GL_UNIFORM_INIT("rt_inv_viewport");
static GlUniform u_rt_result = GL_UNIFORM_INIT("rt_result");
static GlUniform u_rt_refl = GL_UNIFORM_INIT("rt_refl"); // phase 4
static GlUniform u_rt_gi = GL_UNIFORM_INIT("rt_gi");
static GlUniform u_rt_gi_tint = GL_UNIFORM_INIT("rt_gi_tint");
static GlUniform u_rt_local = GL_UNIFORM_INIT("rt_local");
static GlUniform u_rt_local_size = GL_UNIFORM_INIT("rt_local_size");
static GlUniform u_rt_local_index[BASIC_MAX_LIGHTS];

// ---------------------------------------------------------------- Cinematic lighting (rt.h)

static float lum3(const float r, const float g, const float b) { return 0.2126f * r + 0.7152f * g + 0.0722f * b; }

// The per-map exposure of the Cinematic preset: Krunker's frame is lit by its flat ambient plus the sun; the preset
// keeps RT_CINE_FILL of the ambient and RT_CINE_SUN of the sun (plus the bounce, about a third of what the ambient
// was). The ratio of the two estimates (the sun weighted by the average lit, unshadowed share of a scene, 0.5) brings
// the mean back to Krunker's, so the frame keeps its brightness and gains contrast; RT_CINE_EXPOSURE_BIAS on top.
static float cine_exposure(void) {
    const RenderLighting *l = &g_render_lighting;
    const float ls = l->correct_lights ? 1.0f : (float) M_PI;
    const float amb = lum3(l->ambient_color.x, l->ambient_color.y, l->ambient_color.z) * l->ambient_intensity * ls;
    const float sun = lum3(l->light_color.x, l->light_color.y, l->light_color.z) * l->light_intensity * ls;
    const float before = amb + 0.5f * sun;
    const float after = amb * (RT_CINE_FILL + 0.35f) + 0.5f * sun * RT_CINE_SUN;
    if (after <= 1e-4f) return 1.0f;
    // night maps (sky_physical_day ~0) stay darker: the curve's exposure would otherwise lift them to daylight levels
    const float day = sky_physical_day();
    float e = before / after * RT_CINE_EXPOSURE_BIAS * (0.8f + 0.2f * (day < 0.0f ? 0.0f : day > 1.0f ? 1.0f : day));
    return e < 0.6f ? 0.6f : e > 1.8f ? 1.8f : e;
}

// The same exposure for the passes outside basic.frag (the sky, rt_grade.c); 1 outside the preset.
float rt_cinematic_exposure(void) {
    return rt_cinematic() ? cine_exposure() : 1.0f;
}

// The sun's warmth by elevation: a low sun is amber, a high one keeps the map's colour (multiplies the map's sun)
static void cine_sun_tint(float out[3]) {
    vec3 d = g_render_lighting.light_direction;
    const float len = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
    const float elevation = len > 0.0f ? d.y / len : 1.0f;
    float t = (elevation - 0.05f) / 0.55f; // 0 at the horizon .. 1 from ~35 degrees
    t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
    t = t * t * (3.0f - 2.0f * t);
    const float warm[3] = {1.10f, 0.92f, 0.72f};
    for (int c = 0; c < 3; c++) out[c] = RT_CINE_SUN * (warm[c] + (1.0f - warm[c]) * (0.4f + 0.6f * t));
}

static void cine_uniforms(const unsigned int program) {
    static GlUniform u_fill = GL_UNIFORM_INIT("rt_cine_fill");
    static GlUniform u_sun = GL_UNIFORM_INIT("rt_cine_sun");
    static GlUniform u_exposure = GL_UNIFORM_INIT("rt_cine_exposure");
    float tint[3];
    cine_sun_tint(tint);
    gl_uniform_1f(&u_fill, program, RT_CINE_FILL);
    gl_uniform_3f(&u_sun, program, tint[0], tint[1], tint[2]);
    gl_uniform_1f(&u_exposure, program, cine_exposure());
}

void rt_apply_mesh_uniforms(const Mesh *mesh) {
    const unsigned int program = mesh->material->program;
    if (gl_uniform_location(&u_rt_effects, program) < 0) return;
    // transparent meshes sit in front of the surface the RT pass traced: they keep Krunker's shadow map
    int effects = !mesh->material->transparent ? active_effects() : 0; // the world's or the viewmodel's bits
    if (mesh->material->transparent && g_rt.world_active) effects = water_effects(mesh, program); // water / glass (rt.h)
    // Cinematic: every draw of the traced world / viewmodel gets the same lighting balance and exposure, transparent
    // ones included (else glass and liquids would keep Krunker's brightness next to the re-exposed world)
    if ((g_rt.world_active || g_rt.vm_active) && (active_effects() & RT_EFFECT_CINEMATIC)) effects |= RT_EFFECT_CINEMATIC;
    gl_uniform_1i(&u_rt_effects, program, effects);
    if (!effects) return;
    gl_uniform_2f(&u_rt_inv_viewport, program, 1.0f / (float) g_rt.scene_width, 1.0f / (float) g_rt.scene_height);
    gl_uniform_1i(&u_rt_result, program, RT_TEXTURE_UNIT);
    if (effects & RT_EFFECT_REFLECTIONS) gl_uniform_1i(&u_rt_refl, program, RT_REFL_TEXTURE_UNIT);
    if (effects & RT_EFFECT_SKY_PROBE) { // the ambient cube: (+x, +y, +z) and (-x, -y, -z)
        static GlUniform u_rt_sky_probe_pos = GL_UNIFORM_INIT("rt_sky_probe_pos");
        static GlUniform u_rt_sky_probe_neg = GL_UNIFORM_INIT("rt_sky_probe_neg");
        gl_uniform_3f(&u_rt_sky_probe_pos, program, g_rt.sky_probe[0], g_rt.sky_probe[2], g_rt.sky_probe[4]);
        gl_uniform_3f(&u_rt_sky_probe_neg, program, g_rt.sky_probe[1], g_rt.sky_probe[3], g_rt.sky_probe[5]);
    }
    if (effects & RT_EFFECT_GI) {
        gl_uniform_1i(&u_rt_gi, program, RT_GI_TEXTURE_UNIT);
        gl_uniform_1f(&u_rt_gi_tint, program, RT_GI_TINT);
    }
    if (effects & RT_EFFECT_LOCAL_LIGHTS) {
        static char names[BASIC_MAX_LIGHTS][24];
        if (!u_rt_local_index[0].name) {
            for (int i = 0; i < BASIC_MAX_LIGHTS; i++) {
                snprintf(names[i], sizeof(names[i]), "rt_local_index[%d]", i);
                u_rt_local_index[i] = (GlUniform) GL_UNIFORM_INIT(names[i]);
            }
        }
        gl_uniform_1i(&u_rt_local, program, RT_LOCAL_TEXTURE_UNIT);
        gl_uniform_2f(&u_rt_local_size, program, (float) g_rt.width, (float) g_rt.height);
        for (int i = 0; i < BASIC_MAX_LIGHTS; i++) gl_uniform_1i(&u_rt_local_index[i], program, g_rt.local_index[i]);
    }
    if (effects & RT_EFFECT_CINEMATIC) cine_uniforms(program); // look-first lighting (rt.h "Cinematic lighting")
    if (effects & RT_EFFECT_BEVEL) { // bevelled map cube edges (rt.h): the cube's face mask, 0 = this draw is not bevelled
        static GlUniform u_bevel_faces = GL_UNIFORM_INIT("rt_bevel_faces");
        static GlUniform u_bevel_width = GL_UNIFORM_INIT("rt_bevel_width");
        static GlUniform u_bevel_fraction = GL_UNIFORM_INIT("rt_bevel_face_fraction");
        int faces = 0;
        if (mesh->material->vtable == &basic_material_vtable) {
            const BasicMaterial *m = (const BasicMaterial *) mesh->material;
            const int skip = m->alpha_test > 0.0f || m->unlit || m->sprite || m->use_flat_normal || mesh->material->wireframe;
            faces = skip ? 0 : m->rt_bevel_faces;
        }
        gl_uniform_1i(&u_bevel_faces, program, faces);
        if (faces) {
            gl_uniform_1f(&u_bevel_width, program, RT_BEVEL_WIDTH);
            gl_uniform_1f(&u_bevel_fraction, program, RT_BEVEL_FACE_FRACTION);
        }
    }
}

static void debug_blit(void) {
    const unsigned int program = g_rt.debug_program;
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    use_program(program);
    bind_unit(0, g_rt.trace_texture);
    bind_unit(1, g_rt.normal_texture);
    bind_unit(2, g_rt.result_texture);
    bind_unit(3, g_rt.local_result_texture);
    bind_unit(4, g_rt.refl_result_texture);
    bind_unit(5, g_rt.velocity_texture); // motion vectors (phase 6; unit 4 is left to the reflection view)
    bind_unit(6, g_rt.gi_result_texture); // GI (mode 9)
    gl_uniform_1i(&u_debug_trace, program, 0);
    gl_uniform_1i(&u_debug_normal, program, 1);
    gl_uniform_1i(&u_debug_result, program, 2);
    gl_uniform_1i(&u_debug_local, program, 3);
    gl_uniform_1i(&u_debug_refl, program, 4);
    gl_uniform_1i(&u_debug_velocity, program, 5);
    gl_uniform_1i(&u_debug_gi, program, 6);
    gl_uniform_1i(&u_debug_mode, program, g_rt.debug_mode);
    fullscreen();
    glBindVertexArray(0);
    for (int unit = 6; unit >= 0; unit--) bind_unit(unit, 0);
    g_active_texture = 0;
    use_program(0);
    glEnable(GL_DEPTH_TEST);
}

void rt_end_world(void) {
    if (g_rt.world_active && g_rt.debug_mode) debug_blit();
    g_rt.world_active = 0;
}

void rt_debug_set(const char *what) {
    // name -> rt_debug.frag mode (7 is the reflection view's)
    static const struct { const char *name; int mode; } views[] = {
        {"bvh", 1}, {"normals", 2}, {"visibility", 3}, {"history", 4}, {"ao", 5}, {"local", 6}, {"motion", 8}, {"refl", 7}, {"gi", 9},
    };
    g_rt.debug_mode = RT_DEBUG_NONE;
    if (!what) return;
    for (size_t i = 0; i < sizeof(views) / sizeof(views[0]); i++) if (!strcmp(what, views[i].name)) g_rt.debug_mode = views[i].mode;
}

void rt_unload_map(void) {
    g_rt.history_valid = 0;
    g_rt.history_epoch++;
    g_rt.velocity_ready = 0;
    g_rt.world_active = 0;
    g_rt.build_logged = 0;
    g_emissive_logged = 0; // the next map logs its emissive light count
    rt_self_unload_map();  // the shadow proxy's meshes go before the BLAS cache they are in
    rt_scene_unload_map(&g_rt.scene);
    rt_atlas_unload_map(); // phase 4: the map's textures go with it
    rt_normals_unload_map(); // rt_normals.c: the generated normal maps (the materials' caches follow the epoch)
    g_rt.refl_history_valid = 0;
    g_rt.gi_history_valid = 0;
    g_shafts.history_valid = g_shafts.ready = 0; // rtShafts
    g_vm.history_valid = g_vm.refl_history_valid = g_vm.gi_history_valid = 0; // the viewmodel's histories
    g_wt.history_valid = g_wt.refl_history_valid = 0; // water / glass (rt.h)
    g_water.active = g_water.copy_valid = 0;
    g_water.refl_texture = 0;
    g_rt.sky_probe_valid = 0; // the viewmodel's sky probe restarts on the new map
    g_rt.vm_ready = g_rt.vm_active = 0;
}

void rt_fini(void) {
    rt_gpu_free_emissive(&g_emissive); // the emissive light list
    g_emissive_logged = 0;
    rt_self_fini();
    vm_swap(); vm_delete_targets(); vm_swap(); // the viewmodel's targets
    wt_swap(); vm_delete_targets(); wt_swap(); // water / glass: its reflection targets
    water_delete_copy();
    memset(&g_water, 0, sizeof(g_water));
    rt_masks_fini(); // rt_masks.c: the missing-mask key cache
    rt_normals_fini(); // rt_normals.c: the generated normal maps and their program
    rt_filtering_fini(); // rt_filtering.c: the applied-texture list (the textures belong to loaders.c)
    shafts_delete_targets(); // rtShafts
    {
        const unsigned int shafts_programs[3] = {g_shafts.program, g_shafts.denoise_program, g_shafts.composite_program};
        for (int i = 0; i < 3; i++) {
            if (!shafts_programs[i]) continue;
            if (g_active_shader == shafts_programs[i]) g_active_shader = 0;
            glDeleteProgram(shafts_programs[i]);
        }
        memset(&g_shafts, 0, sizeof(g_shafts));
    }
    delete_targets();
    rt_gpu_free(&g_rt.gpu);
    rt_scene_free(&g_rt.scene);
    rt_atlas_fini();
    if (g_rt.refl_program) { if (g_active_shader == g_rt.refl_program) g_active_shader = 0; glDeleteProgram(g_rt.refl_program); }
    if (g_rt.refl_denoise_program) { if (g_active_shader == g_rt.refl_denoise_program) g_active_shader = 0; glDeleteProgram(g_rt.refl_denoise_program); }
    if (g_rt.gi_program) { if (g_active_shader == g_rt.gi_program) g_active_shader = 0; glDeleteProgram(g_rt.gi_program); }
    if (g_rt.gi_denoise_program) { if (g_active_shader == g_rt.gi_denoise_program) g_active_shader = 0; glDeleteProgram(g_rt.gi_denoise_program); }
    const unsigned int programs[4] = {g_rt.prepass_program, g_rt.trace_program, g_rt.denoise_program, g_rt.debug_program};
    for (int i = 0; i < 4; i++) {
        if (!programs[i]) continue;
        if (g_active_shader == programs[i]) g_active_shader = 0;
        glDeleteProgram(programs[i]);
    }
    if (g_rt.vao) glDeleteVertexArrays(1, &g_rt.vao);
    memset(&g_rt, 0, sizeof(g_rt));
}
