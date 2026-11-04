// Ray tracing: the texture atlas and the material rules of the reflection rays (phase 4,
// client/include/rt.h "atlas"). A reflection ray that hits a textured map object needs that
// texture, but a fragment shader cannot bind the map's ~100 textures: every world texture a
// traced material uses is copied once into one RGBA8 atlas of RT_ATLAS_SLOTS fixed cells
// (RT_ATLAS_CELL px square: the GPU's linear / mip filter downscales bigger textures when the
// cell is drawn), and the material table carries the cell. Tiled Krunker textures wrap inside
// their cell (fract in rt_trace.frag). Texture sizes are not known here (no glGetTexLevelParameter
// in ES 3.0), which is why the cells are fixed and drawn rather than read back.
//
// Each cell's mean colour is read back once (glReadPixels RGBA/UNSIGNED_BYTE of the cell) into a
// RT_ATLAS_SLOTS x 1 texture: geometry whose UVs the tracer does not have (OBJ / glTF models carry
// positions only in the triangle table) reflects its texture's average instead of the flat
// material colour.
//
// rt_scene.c's material_index() calls rt_atlas_material() to fill RtMaterial.atlas; the slot is
// assigned lazily (a map's textures arrive as its objects build) and drawn at the next rt_frame
// (rt_atlas_update(), before the trace). Slots are per map (rt_atlas_unload_map()).
//
// KRUNKNATIVE_RT_FORCE_REFL=1 (dev only; =<roughness> picks the roughness, 0.05 = a mirror): every lit
// material reflects (intensity 1, roughness 0.25) so the effect can be looked at on maps without GLB env materials or worn skins; basic.frag
// then also blends the traced radiance into every lit surface (RT_EFFECT_FORCE_REFL).
// KRUNKNATIVE_RT_ATLAS_DUMP=<out.png> (dev) writes the atlas after each update; KRUNKNATIVE_RT_REFL_DEBUG=<1..6>
// (rt_pass.c reflect_pass) makes rt_reflect.frag show the hit albedo / normal / length / hit-miss / material colour / atlas code.
#include <gl.h>
#include <rt.h>
#include <png_write.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    unsigned int texture;   // the source GL texture (BasicMaterial.texture)
    unsigned char drawn;    // copied into the atlas
} AtlasSlot;

static struct {
    AtlasSlot slots[RT_ATLAS_SLOTS];
    int count;
    int pending;            // slots not drawn yet
    unsigned int atlas_texture, mean_texture, fbo, program;
    int program_tried;
    unsigned char mean[RT_ATLAS_SLOTS][4];
    int force_checked, force;
    float force_roughness;  // KRUNKNATIVE_RT_FORCE_REFL=<roughness> (1 = the default 0.25; 0.05 = a mirror)
    int full_logged;
} g_atlas;

int rt_atlas_force_reflections(void) {
    if (!g_atlas.force_checked) {
        g_atlas.force_checked = 1;
        const char *force = getenv("KRUNKNATIVE_RT_FORCE_REFL");
        g_atlas.force = force && *force && strcmp(force, "0") != 0;
        g_atlas.force_roughness = 0.25f;
        if (g_atlas.force) {
            const float value = (float) atof(force);
            if (value > 0.0f && value < 1.0f) g_atlas.force_roughness = value < 0.02f ? 0.02f : value;
        }
    }
    return g_atlas.force;
}

// The atlas slot of `texture` (assigned on first sight), -1 when the atlas is full or the texture is none.
static int atlas_slot(const unsigned int texture) {
    if (!texture || texture == g_blank_texture) return -1;
    for (int i = 0; i < g_atlas.count; i++) if (g_atlas.slots[i].texture == texture) return i;
    if (g_atlas.count >= RT_ATLAS_SLOTS) {
        if (!g_atlas.full_logged) {
            g_atlas.full_logged = 1;
            fprintf(stderr, "[rt] texture atlas full (%d textures): further materials reflect their flat colour\n", RT_ATLAS_SLOTS);
        }
        return -1;
    }
    g_atlas.slots[g_atlas.count] = (AtlasSlot) {texture, 0};
    g_atlas.pending++;
    return g_atlas.count++;
}

void rt_atlas_material(RtMaterial *m, const BasicMaterial *bm) {
    // t2 = (slot + 1 + RT_ATLAS_UV_MODE_STEP * uv_mode, uv_size.xyz); 0 = no texture
    const int slot = atlas_slot(bm->texture);
    int uv_mode = bm->uv_mode == BASIC_UV_CUBE ? 1 : bm->uv_mode == BASIC_UV_PLANE ? 2 : 0;
    m->atlas[0] = slot < 0 ? 0.0f : (float) (slot + 1 + RT_ATLAS_UV_MODE_STEP * uv_mode);
    m->atlas[1] = bm->uv_size.x;
    m->atlas[2] = bm->uv_size.y;
    m->atlas[3] = bm->uv_size.z;
    if (bm->env_map) { // a v7 store skin's env look: albedo * env, i.e. a metal mirror (skin_fx.c)
        m->metalness = 1.0f;
        m->roughness = 0.15f;
    }
    // Box Office material class (rt.h "material classes"): when no Krunker env rule applies, the class gives the
    // tracer's metalness / roughness and its `reflect` becomes reflect_intensity (the reflect pass traces it)
    if (!bm->env_map && m->reflect_intensity <= 0.0f) {
        rt_materials_resolve((BasicMaterial *) bm); // resolution cache on the material
        if (bm->rt_class) {
            m->metalness = bm->rt_metalness;
            m->roughness = bm->rt_roughness;
            m->reflect_intensity = bm->rt_reflect;
        }
    }
    if (rt_atlas_force_reflections() && !m->unlit && m->reflect_intensity <= 0.0f) {
        m->reflect_intensity = 1.0f;
        m->roughness = g_atlas.force_roughness;
        m->metalness = 0.5f;
    }
}

static int atlas_ready(void) {
    if (g_atlas.program_tried) return g_atlas.program != 0;
    g_atlas.program_tried = 1;
    char *vert = concat(assets_path(), "shaders/scene_composite.vert");
    char *frag = concat(assets_path(), "shaders/rt_atlas.frag");
    if (vert && frag) g_atlas.program = shader_compile(vert, frag);
    free(vert);
    free(frag);
    if (!g_atlas.program) {
        fprintf(stderr, "[rt] rt_atlas.frag failed to compile: reflections use flat colours\n");
        return 0;
    }
    int previous_fbo;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous_fbo);
    glGenTextures(1, &g_atlas.atlas_texture);
    glBindTexture(GL_TEXTURE_2D, g_atlas.atlas_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, RT_ATLAS_SIZE, RT_ATLAS_SIZE, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenTextures(1, &g_atlas.mean_texture);
    glBindTexture(GL_TEXTURE_2D, g_atlas.mean_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, RT_ATLAS_SLOTS, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    glGenFramebuffers(1, &g_atlas.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g_atlas.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_atlas.atlas_texture, 0);
    const int complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, (unsigned int) previous_fbo);
    if (!complete) {
        fprintf(stderr, "[rt] atlas framebuffer incomplete: reflections use flat colours\n");
        glDeleteProgram(g_atlas.program);
        g_atlas.program = 0;
        return 0;
    }
    return 1;
}

static GlUniform u_atlas_src = GL_UNIFORM_INIT("src");

// Draws the pending slots. Uses texture unit 0 and `vao` (a fullscreen triangle); restores the
// framebuffer, viewport, program and unit 0 it found (the caller is rt_frame, mid-pass).
void rt_atlas_update(const unsigned int vao) {
    if (!g_atlas.pending || !atlas_ready()) return;
    int viewport[4], previous_fbo;
    glGetIntegerv(GL_VIEWPORT, viewport);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g_atlas.fbo);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glUseProgram(g_atlas.program);
    g_active_shader = g_atlas.program;
    gl_uniform_1i(&u_atlas_src, g_atlas.program, 0);
    glBindVertexArray(vao);
    glActiveTexture(GL_TEXTURE0);
    unsigned char *pixels = malloc((size_t) RT_ATLAS_CELL * RT_ATLAS_CELL * 4);
    int mean_lo = RT_ATLAS_SLOTS, mean_hi = -1;
    for (int i = 0; i < g_atlas.count; i++) {
        AtlasSlot *slot = &g_atlas.slots[i];
        if (slot->drawn) continue;
        slot->drawn = 1;
        const int x = (i % RT_ATLAS_COLS) * RT_ATLAS_CELL, y = (i / RT_ATLAS_COLS) * RT_ATLAS_CELL;
        glViewport(x, y, RT_ATLAS_CELL, RT_ATLAS_CELL);
        glBindTexture(GL_TEXTURE_2D, slot->texture);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        unsigned long sum[4] = {0, 0, 0, 0};
        if (pixels) {
            glReadPixels(x, y, RT_ATLAS_CELL, RT_ATLAS_CELL, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
            for (int p = 0; p < RT_ATLAS_CELL * RT_ATLAS_CELL; p++) for (int c = 0; c < 4; c++) sum[c] += pixels[p * 4 + c];
            for (int c = 0; c < 4; c++) g_atlas.mean[i][c] = (unsigned char) (sum[c] / (RT_ATLAS_CELL * RT_ATLAS_CELL));
        } else {
            memset(g_atlas.mean[i], 255, 4);
        }
        if (i < mean_lo) mean_lo = i;
        if (i > mean_hi) mean_hi = i;
    }
    free(pixels);
    g_atlas.pending = 0;
    const char *dump = getenv("KRUNKNATIVE_RT_ATLAS_DUMP"); // dev: write the atlas as a PNG after each update
    if (dump && *dump) {
        unsigned char *all = malloc((size_t) RT_ATLAS_SIZE * RT_ATLAS_SIZE * 4);
        if (all) {
            glReadPixels(0, 0, RT_ATLAS_SIZE, RT_ATLAS_SIZE, GL_RGBA, GL_UNSIGNED_BYTE, all);
            png_write_rgba(dump, all, RT_ATLAS_SIZE, RT_ATLAS_SIZE);
            free(all);
        }
    }
    if (mean_hi >= mean_lo) {
        glBindTexture(GL_TEXTURE_2D, g_atlas.mean_texture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, mean_lo, 0, mean_hi - mean_lo + 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, g_atlas.mean[mean_lo]);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    g_active_texture = 0;
    glBindVertexArray(0);
    glUseProgram(0);
    g_active_shader = 0;
    glEnable(GL_DEPTH_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, (unsigned int) previous_fbo);
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
}

static GlUniform u_rt_atlas = GL_UNIFORM_INIT("rt_atlas");
static GlUniform u_rt_atlas_mean = GL_UNIFORM_INIT("rt_atlas_mean");
static GlUniform u_rt_atlas_ready = GL_UNIFORM_INIT("rt_atlas_ready");

void rt_atlas_bind(const unsigned int program, const int atlas_unit, const int mean_unit) {
    const int ready = g_atlas.program != 0;
    glActiveTexture(GL_TEXTURE0 + (unsigned int) atlas_unit);
    glBindTexture(GL_TEXTURE_2D, ready ? g_atlas.atlas_texture : 0);
    glActiveTexture(GL_TEXTURE0 + (unsigned int) mean_unit);
    glBindTexture(GL_TEXTURE_2D, ready ? g_atlas.mean_texture : 0);
    gl_uniform_1i(&u_rt_atlas, program, atlas_unit);
    gl_uniform_1i(&u_rt_atlas_mean, program, mean_unit);
    gl_uniform_1i(&u_rt_atlas_ready, program, ready);
}

int rt_atlas_count(void) {
    return g_atlas.count;
}

void rt_atlas_unload_map(void) {
    g_atlas.count = 0;
    g_atlas.pending = 0;
    g_atlas.full_logged = 0;
}

void rt_atlas_fini(void) {
    if (g_atlas.fbo) glDeleteFramebuffers(1, &g_atlas.fbo);
    if (g_atlas.atlas_texture) gl_release_texture(g_atlas.atlas_texture), glDeleteTextures(1, &g_atlas.atlas_texture);
    if (g_atlas.mean_texture) gl_release_texture(g_atlas.mean_texture), glDeleteTextures(1, &g_atlas.mean_texture);
    if (g_atlas.program) {
        if (g_active_shader == g_atlas.program) g_active_shader = 0;
        glDeleteProgram(g_atlas.program);
    }
    memset(&g_atlas, 0, sizeof(g_atlas));
}

// rt.h emissive lights: a cell's mean colour on the CPU (the means are read back when the cell is drawn)
int rt_atlas_mean_rgb(const int slot, float rgb[3]) {
    if (slot < 0 || slot >= g_atlas.count || !g_atlas.slots[slot].drawn) return 0;
    for (int c = 0; c < 3; c++) rgb[c] = (float) g_atlas.mean[slot][c] / 255.0f;
    return 1;
}
