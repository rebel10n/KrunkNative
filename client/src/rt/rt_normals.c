// Generated normal maps for the ray-traced material pass (client/include/rt.h "generated normal maps",
// doc/raytracing.md). Krunker's textures are flat colour; while ray tracing is on, a classed material with a
// `bump` strength and no normal map of its own gets one built from its base texture's luminance, so brick
// mortar, plank gaps and panel seams catch the sun, the GGX highlights and the local lights.
//
// One map per base GL texture, built on the GPU (assets/shaders/rt_bump.frag, one fullscreen pass, no
// readback) into a texture 1-4x the base's size (the longer side up to RT_BUMP_FINE_MAX) so the bilinear
// height turns pixel-art steps into soft one-texel bevels, mipmapped and repeating like the base. The base's
// size comes from its file (stbi_info on the texture cache's path: OpenGL ES 3.0 cannot query a texture's
// size); a texture the cache does not know (generated canvases, the blank texture) gets no map.
//
// Lazy and deferred: rt_normals_material(), called by basic.c during the draw, only queues an unknown texture;
// rt_normals_frame() (start of rt_frame, outside every draw loop) builds the queue. The material caches its
// answer against the module epoch, which rt_normals_unload_map() bumps when it drops the maps.
#include <gl.h>
#include <rt.h>
#include <render_settings.h>
#include <stb_image.h>
#include <GLFW/glfw3.h> // glfwGetTime: the build-time log line
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { BUMP_PENDING = 0, BUMP_READY = 1, BUMP_NONE = 2 };

typedef struct {
    unsigned int base;
    unsigned int map; // 0 unless READY
    int state;
} BumpEntry;

static BumpEntry *g_entries;
static int g_count, g_cap;
static unsigned g_epoch = 1; // 0 = "never resolved" on a calloc'd material
static int g_pending;

static struct {
    int tried;
    unsigned int program, vao, fbo;
    int loc_src, loc_size, loc_scale, loc_slope, loc_blur;
} g_gpu;

static struct {
    int built;
    double ms;
} g_stats;

static int bump_disabled(void) { // KRUNKNATIVE_RT_NO_BUMP=1: dev A/B
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("KRUNKNATIVE_RT_NO_BUMP");
        v = e && *e && *e != '0';
    }
    return v;
}

static BumpEntry *entry_find(const unsigned int base) {
    for (int i = 0; i < g_count; i++) if (g_entries[i].base == base) return &g_entries[i];
    return NULL;
}

static BumpEntry *entry_add(const unsigned int base) {
    if (g_count == g_cap) {
        const int cap = g_cap ? g_cap * 2 : 64;
        BumpEntry *grown = realloc(g_entries, (size_t) cap * sizeof(BumpEntry));
        if (!grown) return NULL;
        g_entries = grown;
        g_cap = cap;
    }
    BumpEntry *e = &g_entries[g_count++];
    *e = (BumpEntry) {base, 0, BUMP_PENDING};
    g_pending++;
    return e;
}

unsigned int rt_normals_material(BasicMaterial *material) {
    if (!material || material->rt_bump <= 0.0f || !material->texture || material->texture == g_blank_texture) return 0;
    if (bump_disabled() || !client_render_settings()->rt_mode) return 0;
    if (material->rt_bump_epoch == g_epoch && material->rt_bump_source == material->texture) return material->rt_bump_texture;
    BumpEntry *e = entry_find(material->texture);
    if (!e) e = entry_add(material->texture);
    if (!e || e->state == BUMP_PENDING) return 0; // built at the next rt_frame; asked again until then
    material->rt_bump_epoch = g_epoch;
    material->rt_bump_source = material->texture;
    material->rt_bump_texture = e->state == BUMP_READY ? e->map : 0;
    return material->rt_bump_texture;
}

// ---------------------------------------------------------------- building

static int gpu_ready(void) {
    if (g_gpu.tried) return g_gpu.program != 0;
    g_gpu.tried = 1;
    char *vert = concat(assets_path(), "shaders/scene_composite.vert");
    char *frag = concat(assets_path(), "shaders/rt_bump.frag");
    if (vert && frag) g_gpu.program = shader_compile(vert, frag);
    free(vert);
    free(frag);
    if (!g_gpu.program) {
        fprintf(stderr, "[rt] bump: rt_bump.frag failed to compile: no generated normal maps\n");
        return 0;
    }
    g_gpu.loc_src = glGetUniformLocation(g_gpu.program, "src");
    g_gpu.loc_size = glGetUniformLocation(g_gpu.program, "src_size");
    g_gpu.loc_scale = glGetUniformLocation(g_gpu.program, "scale");
    g_gpu.loc_slope = glGetUniformLocation(g_gpu.program, "slope");
    g_gpu.loc_blur = glGetUniformLocation(g_gpu.program, "blur");
    glGenVertexArrays(1, &g_gpu.vao);
    glGenFramebuffers(1, &g_gpu.fbo);
    return 1;
}

// The texture cache's path for `base` (load_texture keys it by path), NULL when the cache does not know it.
static const char *texture_path(const unsigned int base) {
    for (texture_cache_map_itr it = vt_first(&g_texture_cache); !vt_is_end(it); it = vt_next(it))
        if (it.data->val == base) return it.data->key;
    return NULL;
}

static int texture_size(const unsigned int base, int *w, int *h) {
    const char *path = texture_path(base);
    if (!path) return 0;
    char swapped[1024];
    const char *real = file_redirect(path, swapped, sizeof(swapped));
    int comp;
    return stbi_info(real, w, h, &comp) && *w > 0 && *h > 0;
}

static float bump_blur(void) { // KRUNKNATIVE_RT_BUMP_BLUR=<0..1> (dev tuning; default below)
    static float v = -1.0f;
    if (v < 0.0f) {
        const char *e = getenv("KRUNKNATIVE_RT_BUMP_BLUR");
        v = e && *e ? (float) atof(e) : 0.5f;
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
    }
    return v;
}

// Builds `e`'s map into a new texture; state READY or NONE. The caller saved / restores the GL state.
static void build_one(BumpEntry *e) {
    e->state = BUMP_NONE;
    int w = 0, h = 0;
    if (!texture_size(e->base, &w, &h)) return;
    const int longer = w > h ? w : h;
    int scale = RT_BUMP_FINE_MAX / longer;
    if (scale > 4) scale = 4;
    if (scale < 1) scale = 1;
    const int fw = w * scale, fh = h * scale;

    unsigned int map = 0;
    glGenTextures(1, &map);
    glBindTexture(GL_TEXTURE_2D, map);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, fw, fh, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);

    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, map, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
        glDeleteTextures(1, &map);
        return;
    }
    glViewport(0, 0, fw, fh);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, e->base);
    glUniform1i(g_gpu.loc_src, 0);
    glUniform2f(g_gpu.loc_size, (float) w, (float) h);
    glUniform1f(g_gpu.loc_scale, (float) scale);
    glUniform1f(g_gpu.loc_slope, RT_BUMP_SLOPE);
    glUniform1f(g_gpu.loc_blur, bump_blur());
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);

    // mipmapped and repeating like the base (distant walls flatten instead of shimmering)
    glBindTexture(GL_TEXTURE_2D, map);
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glBindTexture(GL_TEXTURE_2D, 0);
    e->map = map;
    e->state = BUMP_READY;
}

void rt_normals_frame(void) {
    if (!g_pending) return;
    if (bump_disabled() || !gpu_ready()) { // nothing will ever build: settle the queue as "no map"
        for (int i = 0; i < g_count; i++) if (g_entries[i].state == BUMP_PENDING) g_entries[i].state = BUMP_NONE;
        g_pending = 0;
        return;
    }
    int draw_fbo, read_fbo, viewport[4], vao, program, active_unit, unit0_texture;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fbo);
    glGetIntegerv(GL_VIEWPORT, viewport);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active_unit);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &unit0_texture);
    const GLboolean blend = glIsEnabled(GL_BLEND), depth = glIsEnabled(GL_DEPTH_TEST), cull = glIsEnabled(GL_CULL_FACE),
                    scissor = glIsEnabled(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, g_gpu.fbo);
    glBindVertexArray(g_gpu.vao);
    glUseProgram(g_gpu.program);

    const double t0 = glfwGetTime();
    int built = 0, none = 0;
    for (int i = 0; i < g_count; i++) {
        if (g_entries[i].state != BUMP_PENDING) continue;
        build_one(&g_entries[i]);
        if (g_entries[i].state == BUMP_READY) built++;
        else none++;
    }
    const double ms = (glfwGetTime() - t0) * 1000.0;
    g_pending = 0;

    glUseProgram((unsigned int) program);
    g_active_shader = (unsigned int) program;
    glBindVertexArray((unsigned int) vao);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (unsigned int) draw_fbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (unsigned int) read_fbo);
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (unsigned int) unit0_texture);
    g_active_texture = (unsigned int) unit0_texture;
    glActiveTexture((GLenum) active_unit);
    if (blend) glEnable(GL_BLEND);
    if (depth) glEnable(GL_DEPTH_TEST);
    if (cull) glEnable(GL_CULL_FACE);
    if (scissor) glEnable(GL_SCISSOR_TEST);

    g_stats.built += built;
    g_stats.ms += ms;
    if (built || none)
        fprintf(stderr, "[rt] bump: %d normal maps built (%d textures without one) in %.2f ms CPU; %d maps, %.1f ms so far\n",
                built, none, ms, g_stats.built, g_stats.ms);
}

static void drop_all(void) {
    for (int i = 0; i < g_count; i++) if (g_entries[i].map) glDeleteTextures(1, &g_entries[i].map);
    g_count = 0;
    g_pending = 0;
    g_epoch++;
    if (!g_epoch) g_epoch = 1;
}

void rt_normals_unload_map(void) {
    drop_all();
    g_stats.built = 0;
    g_stats.ms = 0.0;
}

void rt_normals_fini(void) {
    drop_all();
    free(g_entries);
    g_entries = NULL;
    g_cap = 0;
    if (g_gpu.program) {
        if (g_active_shader == g_gpu.program) g_active_shader = 0;
        glDeleteProgram(g_gpu.program);
    }
    if (g_gpu.vao) glDeleteVertexArrays(1, &g_gpu.vao);
    if (g_gpu.fbo) glDeleteFramebuffers(1, &g_gpu.fbo);
    memset(&g_gpu, 0, sizeof(g_gpu));
}
