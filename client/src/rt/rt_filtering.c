// Ray tracing: world texture filtering (rt.h "texture filtering"; doc/raytracing.md).
//
// Krunker samples its map textures with NearestFilter for both min and mag (the map texture loader,
// deob-10.0.6.js:159855-159856, also :159005-159006 and :159793-159794) and never raises three's
// anisotropy (default 1; the renderer only applies texture.anisotropy, deob-10.0.6.js:81806), so a
// far floor or a wall at a grazing angle shimmers: the min filter reads one texel of the base level.
// loaders.c matches that (GL_NEAREST / GL_NEAREST; it builds mipmaps, which GL_NEAREST never reads).
//
// While ray tracing is on (rtMode > 0) the textures of the texture cache (g_texture_cache: every map,
// model, weapon and skin texture loaded through load_texture, plus prefab.c's composites and the
// viewer's) minify trilinearly (GL_LINEAR_MIPMAP_LINEAR) with anisotropic filtering up to
// RT_FILTER_ANISOTROPY where the context offers it. The mag filter stays Krunker's (nearest: the
// pixel-art look up close). Switching rtMode back to 0 restores each texture's own min filter and
// anisotropy 1, so the frame is pixel-identical to Krunker's again (the extra mip levels this pass
// may have built are never read by a GL_NEAREST min filter).
//
// Texture parameters, not a sampler object: a sampler on unit 0 would also override the wrap mode
// and the min filter of every other texture drawn there (clamped textures, render targets and video
// frames without mipmaps would turn incomplete, i.e. black), while the cache holds exactly the
// world's REPEAT, image-backed textures.
#include <gl.h>
#include <rt.h>
#include <render_settings.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// EXT_texture_filter_anisotropic (GL 4.6 core as GL_TEXTURE_MAX_ANISOTROPY: same values). Neither
// glad loader (desktop 4.5, ES 3.0) declares them.
#define RT_GL_TEXTURE_MAX_ANISOTROPY 0x84FE
#define RT_GL_MAX_TEXTURE_MAX_ANISOTROPY 0x84FF

typedef struct {
    unsigned int texture;
    int min_filter; // the texture's own GL_TEXTURE_MIN_FILTER, restored on revert
} RtFiltered;

static struct {
    int on;               // the filtering is applied
    int caps_read;
    float max_anisotropy; // 0: no anisotropic filtering in this context
    RtFiltered *items;
    size_t count, cap;
    size_t cache_size;    // vt_size(&g_texture_cache) at the last walk (new entries trigger another)
} g_filter;

static void read_caps(void) {
    if (g_filter.caps_read) return;
    g_filter.caps_read = 1;
    int extensions = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &extensions);
    int have = 0;
    for (int i = 0; i < extensions && !have; i++) {
        const char *name = (const char *) glGetStringi(GL_EXTENSIONS, (unsigned int) i);
        if (!name) continue;
        have = !strcmp(name, "GL_EXT_texture_filter_anisotropic") || !strcmp(name, "GL_ARB_texture_filter_anisotropic") ||
               !strcmp(name, "EXT_texture_filter_anisotropic"); // WebGL2's name
    }
    if (have) {
        float max = 0.0f;
        glGetFloatv(RT_GL_MAX_TEXTURE_MAX_ANISOTROPY, &max);
        while (glGetError() != GL_NO_ERROR) {} // a driver that lists it but rejects the query
        g_filter.max_anisotropy = max > 1.0f ? (max < RT_FILTER_ANISOTROPY ? max : RT_FILTER_ANISOTROPY) : 0.0f;
    }
    fprintf(stderr, "[rt] texture filtering: trilinear, anisotropy %s%.0fx\n",
            g_filter.max_anisotropy > 0.0f ? "" : "unavailable, ", g_filter.max_anisotropy > 0.0f ? g_filter.max_anisotropy : 1.0f);
}

static int known(const unsigned int texture) {
    for (size_t i = 0; i < g_filter.count; i++) if (g_filter.items[i].texture == texture) return 1;
    return 0;
}

// Applies the filtering to the cache entries not yet applied (all of them on the first call).
static void apply_new(void) {
    for (texture_cache_map_itr it = vt_first(&g_texture_cache); !vt_is_end(it); it = vt_next(it)) {
        const unsigned int texture = it.data->val;
        if (!texture || known(texture)) continue;
        if (g_filter.count == g_filter.cap) {
            const size_t cap = g_filter.cap ? g_filter.cap * 2 : 256;
            RtFiltered *grown = realloc(g_filter.items, cap * sizeof(RtFiltered));
            if (!grown) return;
            g_filter.items = grown;
            g_filter.cap = cap;
        }
        glBindTexture(GL_TEXTURE_2D, texture);
        int min_filter = GL_NEAREST;
        glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &min_filter);
        if (min_filter == GL_NEAREST || min_filter == GL_LINEAR) {
            glGenerateMipmap(GL_TEXTURE_2D); // complete the chain (prefab.c's composites have none)
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        }
        if (g_filter.max_anisotropy > 0.0f) glTexParameterf(GL_TEXTURE_2D, RT_GL_TEXTURE_MAX_ANISOTROPY, g_filter.max_anisotropy);
        g_filter.items[g_filter.count++] = (RtFiltered) {texture, min_filter};
    }
}

// Restores every applied texture still in the cache (an id that left the cache may name another
// texture by now: not touched).
static void revert_all(void) {
    for (texture_cache_map_itr it = vt_first(&g_texture_cache); !vt_is_end(it); it = vt_next(it)) {
        const unsigned int texture = it.data->val;
        for (size_t i = 0; i < g_filter.count; i++) {
            if (g_filter.items[i].texture != texture) continue;
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, g_filter.items[i].min_filter);
            if (g_filter.max_anisotropy > 0.0f) glTexParameterf(GL_TEXTURE_2D, RT_GL_TEXTURE_MAX_ANISOTROPY, 1.0f);
            break;
        }
    }
    g_filter.count = 0;
}

void rt_filtering_update(int want) {
    static int no_filter = -1; // KRUNKNATIVE_RT_NO_FILTER=1 (dev): Krunker's sampling with rtMode > 0, for A/B captures
    if (no_filter < 0) {
        const char *env = getenv("KRUNKNATIVE_RT_NO_FILTER");
        no_filter = env && *env && *env != '0';
    }
    if (no_filter) want = 0;
    if (!want && !g_filter.on) return;
    const size_t size = vt_size(&g_texture_cache);
    if (want && g_filter.on && size == g_filter.cache_size) return;

    int active = GL_TEXTURE0, bound = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);

    if (want) {
        read_caps();
        apply_new();
    } else {
        revert_all();
    }
    g_filter.on = want ? 1 : 0;
    g_filter.cache_size = size;

    glBindTexture(GL_TEXTURE_2D, (unsigned int) bound); // unit 0 as found: loaders.c's g_active_texture stays true
    glActiveTexture((GLenum) active);
}

void rt_filtering_fini(void) {
    free(g_filter.items);
    memset(&g_filter, 0, sizeof(g_filter));
}
