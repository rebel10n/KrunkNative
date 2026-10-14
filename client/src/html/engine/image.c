/*
 * image.c - raster image cache for the HTML/CSS engine.  See image.h for the
 * contract (URL resolution, premultiplied alpha, GL texture parameters, web
 * fetching) and doc/html-engine-plan.md "GL rendering" / "Images".
 */
#include "image.h"
#include "svg.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <gl.h>
#include <shared.h>
#include <stb_image.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#define IMAGE_KEY_MAX 512

/* Keys below this prefix name files of the game asset tree (assets_path()), not of
 * ui/: the URLs html_dom_asset_url() writes ("/assets/textures/previews/..."), the
 * same layout dist/web serves next to the page. */
#define IMAGE_ASSETS_PREFIX "assets/"

/* SVG entries keep their document and the textures rasterised at the device
 * sizes they were last drawn at (image_texture_sized), least recently used
 * first out. */
#define IMAGE_SVG_SIZES 8

typedef struct ImageSvgTex {
    int w, h;              /* texels (device px) */
    int qw, qh, qx, qy;    /* drawn size and sub-pixel offset, 1/8 px units */
    unsigned tex;          /* 0 = free slot */
    unsigned last_use;
} ImageSvgTex;

typedef struct ImageSlot {
    ImageEntry entry;      /* first so &slot->entry == slot                    */
    char *key;             /* root-relative path, or the raw URL when rejected */
    uint32_t hash;
    SvgImage *svg;         /* vector source (entry.vector), else NULL           */
    ImageSvgTex sized[IMAGE_SVG_SIZES];
    unsigned use_clock;
    uint8_t lazy;          /* raster file: pixels decoded on first draw, evictable */
    unsigned last_frame;   /* image_cache_frame() count of its last draw (LRU)      */
    size_t bytes;          /* GL bytes held (level 0 + mips)   */
} ImageSlot;

struct ImageCache {
    char *root;            /* ui dir with a trailing slash                     */
    size_t root_len;
    char *fallback;        /* another page root for files `root` lacks (NULL: none)   */
    ImageSlot **slots;     /* individually allocated: entry addresses are stable */
    int count;
    int cap;
    int *index;            /* open-addressing hash of slot indices (-1 empty)    */
    int index_cap;         /* power of two, > 2 * count                          */
    unsigned frame;        /* image_cache_frame() calls                          */
    size_t bytes;          /* GL bytes of the resident lazy entries              */
    int decodes;           /* lazy decodes so far (profile)                      */
};

/* ------------------------------------------------------------------------- */
/* Cache                                                                      */
/* ------------------------------------------------------------------------- */

static uint32_t image_hash(const char *s) {
    uint32_t h = 2166136261u;
    for (; *s; s++) h = (h ^ (unsigned char) *s) * 16777619u;
    return h;
}

ImageCache *image_cache_create(const char *ui_dir) {
    ImageCache *cache = calloc(1, sizeof(*cache));
    if (!cache) return NULL;

    const char *dir = ui_dir && *ui_dir ? ui_dir : "./ui/";
    const size_t len = strlen(dir);
    const int needs_slash = dir[len - 1] != '/' && dir[len - 1] != '\\';

    cache->root = malloc(len + 2);
    if (!cache->root) {
        free(cache);
        return NULL;
    }

    memcpy(cache->root, dir, len);
    if (needs_slash) cache->root[len] = '/';
    cache->root[len + needs_slash] = 0;
    cache->root_len = len + needs_slash;

    return cache;
}

static void image_slot_free(ImageSlot *slot) {
    if (slot->entry.tex) glDeleteTextures(1, &slot->entry.tex);
    for (int i = 0; i < IMAGE_SVG_SIZES; i++) {
        if (slot->sized[i].tex) glDeleteTextures(1, &slot->sized[i].tex);
    }
    svg_image_destroy(slot->svg);

    free(slot->key);
    free(slot);
}

void image_cache_destroy(ImageCache *cache) {
    if (!cache) return;

    for (int i = 0; i < cache->count; i++) image_slot_free(cache->slots[i]);

    free(cache->slots);
    free(cache->index);
    free(cache->root);
    free(cache->fallback);
    free(cache);
}

int image_cache_count(const ImageCache *cache) {
    return cache ? cache->count : 0;
}

static ImageSlot *image_cache_find(const ImageCache *cache, const char *key, const uint32_t hash) {
    if (!cache->index_cap) return NULL;
    const unsigned mask = (unsigned) cache->index_cap - 1;
    for (unsigned i = hash & mask;; i = (i + 1) & mask) {
        const int at = cache->index[i];
        if (at < 0) return NULL;
        ImageSlot *slot = cache->slots[at];
        if (slot->hash == hash && !strcmp(slot->key, key)) return slot;
    }
}

static void image_index_put(ImageCache *cache, const int at) {
    const unsigned mask = (unsigned) cache->index_cap - 1;
    unsigned i = cache->slots[at]->hash & mask;
    while (cache->index[i] >= 0) i = (i + 1) & mask;
    cache->index[i] = at;
}

static int image_index_grow(ImageCache *cache) {
    const int cap = cache->index_cap ? cache->index_cap * 2 : 256;
    int *index = malloc((size_t) cap * sizeof(*index));
    if (!index) return 0;
    for (int i = 0; i < cap; i++) index[i] = -1;
    free(cache->index);
    cache->index = index;
    cache->index_cap = cap;
    for (int i = 0; i < cache->count; i++) image_index_put(cache, i);
    return 1;
}

static ImageSlot *image_cache_add(ImageCache *cache, const char *key, const uint32_t hash) {
    if (cache->count == cache->cap) {
        const int cap = cache->cap ? cache->cap * 2 : 64;
        ImageSlot **grown = realloc(cache->slots, (size_t) cap * sizeof(*grown));
        if (!grown) return NULL;

        cache->slots = grown;
        cache->cap = cap;
    }
    if ((cache->count + 1) * 2 > cache->index_cap && !image_index_grow(cache)) return NULL;

    ImageSlot *slot = calloc(1, sizeof(*slot));
    if (!slot) return NULL;

    slot->key = concat(key, "");
    if (!slot->key) {
        free(slot);
        return NULL;
    }

    slot->hash = hash;
    cache->slots[cache->count++] = slot;
    image_index_put(cache, cache->count - 1);

    return slot;
}

/* ------------------------------------------------------------------------- */
/* URL resolution                                                             */
/* ------------------------------------------------------------------------- */

static int image_url_has_scheme(const char *url, const size_t len) {
    if (len >= 2 && url[0] == '/' && url[1] == '/') return 1; /* protocol-relative */

    for (size_t i = 0; i < len; i++) {
        const char c = url[i];

        if (c == ':') return i > 0;
        if (c == '/' || c == '\\' || c == '?' || c == '#') return 0;

        const int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                       (i > 0 && ((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.'));
        if (!ok) return 0;
    }

    return 0;
}

/* Appends `text` (len bytes) to the segment stack in `out`, collapsing "." and
 * "..".  Returns 0 when ".." would climb above the root. */
static int image_url_push(const char *text, size_t len, char *out, size_t cap, size_t *out_len) {
    size_t i = 0;

    while (i < len) {
        size_t j = i;
        while (j < len && text[j] != '/' && text[j] != '\\') j++;

        const size_t seg_len = j - i;
        const char *seg = text + i;

        if (seg_len == 0 || (seg_len == 1 && seg[0] == '.')) {
            /* empty or "." : nothing */
        } else if (seg_len == 2 && seg[0] == '.' && seg[1] == '.') {
            if (*out_len == 0) return 0;

            size_t k = *out_len;
            while (k > 0 && out[k - 1] != '/') k--;
            *out_len = k > 0 ? k - 1 : 0;
        } else {
            const size_t need = *out_len + (*out_len ? 1 : 0) + seg_len;
            if (need + 1 > cap) return 0;

            if (*out_len) out[(*out_len)++] = '/';
            memcpy(out + *out_len, seg, seg_len);
            *out_len += seg_len;
        }

        i = j + 1;
    }

    out[*out_len] = 0;
    return 1;
}

int image_resolve_url(const char *url, const char *base_url, char *out, size_t cap) {
    if (!url || !out || cap < 2) return 0;

    while (*url == ' ' || *url == '\t' || *url == '\n' || *url == '\r') url++;

    const size_t len = strcspn(url, "?#");
    if (len == 0) return 0;
    if (url[len - 1] == '/' || url[len - 1] == '\\') return 0; /* a directory, not a file */
    if (image_url_has_scheme(url, len)) return 0;

    size_t out_len = 0;
    out[0] = 0;

    if (url[0] != '/' && url[0] != '\\' && base_url && *base_url) {
        /* directory part of the base: everything up to its last slash */
        size_t base_dir = strlen(base_url);
        while (base_dir > 0 && base_url[base_dir - 1] != '/' && base_url[base_dir - 1] != '\\') base_dir--;

        if (!image_url_push(base_url, base_dir, out, cap, &out_len)) return 0;
    }

    if (!image_url_push(url, len, out, cap, &out_len)) return 0;

    return out_len > 0;
}

/* ------------------------------------------------------------------------- */
/* File access                                                                */
/* ------------------------------------------------------------------------- */

#ifdef __EMSCRIPTEN__

static int image_mkdir_parents(const char *path) {
    char buffer[IMAGE_KEY_MAX * 2];
    const size_t length = strlen(path);
    if (length >= sizeof(buffer)) return 0;

    memcpy(buffer, path, length + 1);

    for (char *cursor = buffer + 1; *cursor; cursor++) {
        if (*cursor != '/') continue;

        *cursor = 0;
        if (mkdir(buffer, 0777) && errno != EEXIST) return 0;
        *cursor = '/';
    }

    return 1;
}

/* Fetch `key` from the page root (ui/ is the page root) and mirror it into
 * MEMFS at `path` so later readers find it.  Synchronous under ASYNCIFY. */
static int image_fetch_web(const char *key, const char *path, unsigned char **bytes, size_t *len) {
    void *data = NULL;
    int size = 0;
    int error = 0;

    emscripten_wget_data(key, &data, &size, &error);

    if (error || !data || size <= 0) {
        free(data);
        return 0;
    }

    if (image_mkdir_parents(path)) {
        FILE *file = fopen(path, "wb");

        if (file) {
            fwrite(data, 1, (size_t) size, file);
            fclose(file);
        }
    }

    *bytes = data;
    *len = (size_t) size;
    return 1;
}

#endif

/* Reads `key` from the asset tree (assets_path(), the game content).  On the web
 * ensure_asset_file() fetches it from the asset host first. */
static int image_read_assets(const char *rel, unsigned char **bytes, size_t *len) {
    const char *assets = assets_path();
    char *asset = assets ? concat(assets, rel) : NULL;
    const int ok = asset && ensure_asset_file(asset) && read_file(asset, bytes, len) == 0;
    free(asset);
    return ok;
}

void image_cache_set_fallback(ImageCache *cache, const char *dir) {
    if (!cache) return;
    free(cache->fallback);
    cache->fallback = dir && *dir ? concat(dir, "") : NULL;
}

/* Reads `key` from the page root (ui/), else from the fallback root (image_cache_set_fallback). */
static int image_read_page(const ImageCache *cache, const char *key, unsigned char **bytes, size_t *len) {
    char *path = concat(cache->root, key);
    if (!path) return 0;

    int ok;

#ifdef __EMSCRIPTEN__
    if (!access(path, F_OK)) ok = read_file(path, bytes, len) == 0;
    else ok = image_fetch_web(key, path, bytes, len);
#else
    ok = read_file(path, bytes, len) == 0;
#endif

    free(path);
    if (!ok && cache->fallback) {
        path = concat(cache->fallback, key);
        ok = path && read_file(path, bytes, len) == 0;
        free(path);
    }
    return ok;
}

/* The two trees a page image can live in are tried in both directions, because which one
 * holds a file is not something the URL can be trusted to say:
 *   - "assets/<path>" (html_dom_asset_url, Krunker's UTILS.assetsUrl) is game content, but
 *     some of v7's assetsUrl paths are page files its asset host also serves (img/shop/...,
 *     img/backgrounds/...), and assets/ has no img/ tree;
 *   - a page-relative URL is a page file, but ui/ carries only the slice of textures/ the
 *     v7.1.4 reference tree had (weapon_1..19, 53 of 563 sprays, one melee icon), while the
 *     content tree has all of it: every later weapon, spray or melee preview is there.
 * Trying only the named root left those elements blank (the Grappler's and every v7 weapon
 * past 19's loadout preview, the shop's spin card backgrounds). */
static int image_read(const ImageCache *cache, const char *key, unsigned char **bytes, size_t *len) {
    if (!strncmp(key, IMAGE_ASSETS_PREFIX, sizeof(IMAGE_ASSETS_PREFIX) - 1)) {
        const char *rel = key + sizeof(IMAGE_ASSETS_PREFIX) - 1;
        return image_read_assets(rel, bytes, len) || image_read_page(cache, rel, bytes, len);
    }
    return image_read_page(cache, key, bytes, len) || image_read_assets(key, bytes, len);
}

/* ------------------------------------------------------------------------- */
/* Decode + upload                                                            */
/* ------------------------------------------------------------------------- */

/* Straight -> premultiplied, rounding to nearest (matches round(c * a / 255)). */
static void image_premultiply(unsigned char *px, const size_t count) {
    for (size_t i = 0; i < count; i++, px += 4) {
        const unsigned a = px[3];
        if (a == 255) continue;

        if (a == 0) {
            px[0] = px[1] = px[2] = 0;
            continue;
        }

        for (int c = 0; c < 3; c++) {
            const unsigned t = px[c] * a + 128;
            px[c] = (unsigned char) ((t + (t >> 8)) >> 8);
        }
    }
}

/* floor(log2(max(w, h))) + 1: the levels of a full chain down to 1x1. */
static int image_mip_levels(int w, int h) {
    int size = w > h ? w : h;
    int levels = 1;
    while (size > 1) {
        size >>= 1;
        levels++;
    }
    return levels;
}

static void image_drain_gl_errors(void) {
    for (int i = 0; i < 16 && glGetError() != GL_NO_ERROR; i++) {
    }
}

/* Uploads w x h RGBA8 (premultiplied) pixels into a new GL_TEXTURE_2D with
 * the engine's fixed sampling state.  Restores the texture binding of the
 * active unit and the unpack alignment.  Returns 0 on GL error.
 *
 * smooth != 0: the image-rendering:auto texture.  A full mip chain is built
 * (glGenerateMipmap averages the premultiplied texels, which is the correct
 * space for box filtering) and sampled GL_LINEAR_MIPMAP_LINEAR /
 * GL_LINEAR, approximating Skia's mipmapped bilinear (Chrome's default
 * filter quality) for downscaled images and plain bilinear when upscaled.
 * smooth == 0: the pixelated twin, GL_NEAREST with MAX_LEVEL 0 (level 0 alone
 * is then mipmap complete, so any MIN_FILTER the backend sets stays valid). */
static unsigned image_upload(const unsigned char *px, const int w, const int h, const int smooth) {
    GLint prev_tex = 0;
    GLint prev_align = 4;

    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &prev_align);
    image_drain_gl_errors();

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);

    glPixelStorei(GL_UNPACK_ALIGNMENT, 4); /* RGBA8 rows are always 4-byte aligned */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, smooth ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, smooth ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, smooth ? image_mip_levels(w, h) - 1 : 0);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    if (smooth) glGenerateMipmap(GL_TEXTURE_2D);

    const GLenum error = glGetError();

    glPixelStorei(GL_UNPACK_ALIGNMENT, prev_align);
    glBindTexture(GL_TEXTURE_2D, (GLuint) prev_tex);

    if (error != GL_NO_ERROR) {
        glDeleteTextures(1, &tex);
        return 0;
    }

    return tex;
}

/* Reason of the latest image_fail(), for the KRUNKNATIVE_HTML_IMAGE_LOG line. */
static const char *image_last_reason = "";

static void image_fail(ImageSlot *slot, const char *reason) {
    slot->entry.failed = 1;
    slot->entry.tex = 0;
    slot->entry.w = slot->entry.h = 0;
    image_last_reason = reason;
    fprintf(stderr, "[image] %s: %s\n", slot->key, reason);
}

/* KRUNKNATIVE_HTML_IMAGE_LOG (debug knob, doc/html-ui-engine.md): unset/""/"0" = off;
 * "1" = one tab-separated line per failed image on stderr; anything else = a file
 * the lines are appended to (one run of every menu/window/HUD state collects the
 * full list of missing images).  Each failing key is logged once per cache:
 *   image-miss <key> url=<as written> base=<referencing document> path=<file> reason=<...> */
static void image_log_miss(const ImageCache *cache, const char *key, const char *url, const char *base_url) {
    const char *knob = getenv("KRUNKNATIVE_HTML_IMAGE_LOG");
    if (!knob || !*knob || !strcmp(knob, "0")) return;

    FILE *out = strcmp(knob, "1") ? fopen(knob, "a") : stderr;
    if (!out) return;

    fprintf(out, "image-miss\t%s\turl=%s\tbase=%s\tpath=%s%s\treason=%s\n", key, url ? url : "",
            base_url ? base_url : "", cache->root, key, image_last_reason);

    if (out != stderr) fclose(out);
}

/* ------------------------------------------------------------------------- */
/* data: URLs (RFC 2397): data:[<mediatype>][;base64],<data>                  */
/* ------------------------------------------------------------------------- */

static int image_b64_value(const unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

static int image_hex_value(const char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int image_is_data_url(const char *url) {
    if (!url) return 0;
    while (*url == ' ' || *url == '\t' || *url == '\n' || *url == '\r') url++;
    return (url[0] | 32) == 'd' && (url[1] | 32) == 'a' && (url[2] | 32) == 't' && (url[3] | 32) == 'a' && url[4] == ':';
}

/* Decodes the payload of a data: URL (base64, or percent-encoded text) into
 * a malloc'd buffer.  Whitespace inside base64 is skipped like browsers do. */
int image_decode_data_url(const char *url, unsigned char **out, size_t *out_len) {
    if (!image_is_data_url(url)) return 0;
    while (*url == ' ' || *url == '\t' || *url == '\n' || *url == '\r') url++;
    const char *comma = strchr(url, ',');
    if (!comma) return 0;
    int base64 = 0;
    for (const char *p = url + 5; p + 7 <= comma; p++)
        if (p[0] == ';' && !strncmp(p + 1, "base64", 6) && (p + 7 == comma || p[7] == ';')) base64 = 1;
    const char *d = comma + 1;
    const size_t n = strlen(d);
    unsigned char *buf = malloc(n + 1);
    if (!buf) return 0;
    size_t w = 0;
    if (base64) {
        unsigned acc = 0;
        int bits = 0;
        for (size_t i = 0; i < n; i++) {
            const unsigned char c = (unsigned char) d[i];
            if (c == '=') break;
            if (c == '%' && i + 2 < n && image_hex_value(d[i + 1]) >= 0 && image_hex_value(d[i + 2]) >= 0) {
                /* %2B / %2F / %3D inside base64 */
                const unsigned char dc = (unsigned char) (image_hex_value(d[i + 1]) * 16 + image_hex_value(d[i + 2]));
                i += 2;
                if (dc == '=') break;
                const int v = image_b64_value(dc);
                if (v < 0) continue;
                acc = (acc << 6) | (unsigned) v;
            } else {
                const int v = image_b64_value(c);
                if (v < 0) continue;   /* whitespace / garbage */
                acc = (acc << 6) | (unsigned) v;
            }
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                buf[w++] = (unsigned char) ((acc >> bits) & 0xff);
            }
        }
    } else {
        for (size_t i = 0; i < n; i++) {
            if (d[i] == '%' && i + 2 < n && image_hex_value(d[i + 1]) >= 0 && image_hex_value(d[i + 2]) >= 0) {
                buf[w++] = (unsigned char) (image_hex_value(d[i + 1]) * 16 + image_hex_value(d[i + 2]));
                i += 2;
            } else {
                buf[w++] = (unsigned char) d[i];
            }
        }
    }
    *out = buf;
    *out_len = w;
    return 1;
}

/* Cache key of a data: URL: its length + FNV-1a 64 (the URL itself can be far
 * longer than IMAGE_KEY_MAX). */
/* WebP.  stb_image has no WebP decoder and the engine vendors none: v7's only
 * WebP images are the player card banners (assets/textures/playercards/
 * card_<id>.webp, viewer_skin_preview_path() with texExt "webp").  At import time
 * tools/ui-import/game_assets.py --v7 decodes every real WebP of the asset tree
 * into "<name>.webp.png" next to it (the first frame of an animated one; stb
 * gives the first frame of a GIF too), and a ".webp" that stb can't decode is
 * read from that sidecar.  Some of v7's .webp files are PNG data, which stb
 * decodes directly.  The page keeps v7's URLs: the web build's browser decodes
 * the .webp itself. */
#define IMAGE_WEBP_EXT ".webp"
#define IMAGE_WEBP_SIDECAR ".png"

static int image_is_webp_key(const char *key) {
    const size_t len = strlen(key), ext = sizeof(IMAGE_WEBP_EXT) - 1;
    return len > ext && !strcmp(key + len - ext, IMAGE_WEBP_EXT);
}

static unsigned char *image_load_webp_sidecar(const ImageCache *cache, const char *key, int *w, int *h) {
    char sidecar[IMAGE_KEY_MAX + sizeof(IMAGE_WEBP_SIDECAR)];
    snprintf(sidecar, sizeof(sidecar), "%s%s", key, IMAGE_WEBP_SIDECAR);

    unsigned char *bytes = NULL;
    size_t len = 0;
    if (!image_read(cache, sidecar, &bytes, &len)) return NULL;

    int channels = 0;
    unsigned char *px = len <= (size_t) INT32_MAX ? stbi_load_from_memory(bytes, (int) len, w, h, &channels, 4) : NULL;
    free(bytes);
    return px;
}

static void image_data_url_key(const char *url, char *key, size_t cap) {
    uint64_t h = 1469598103934665603ull;
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *) url; *p; p++, n++) h = (h ^ *p) * 1099511628211ull;
    snprintf(key, cap, "data:%zu:%016llx", n, (unsigned long long) h);
}

static void image_load(const ImageCache *cache, ImageSlot *slot, const char *data_url) {
    unsigned char *bytes = NULL;
    size_t len = 0;

    if (data_url) {
        if (!image_decode_data_url(data_url, &bytes, &len)) {
            image_fail(slot, "malformed data: URL");
            return;
        }
    } else if (!image_read(cache, slot->key, &bytes, &len)) {
        image_fail(slot, "file not found");
        return;
    }

    if (len > (size_t) INT32_MAX) {
        free(bytes);
        image_fail(slot, "file too large");
        return;
    }

    /* loaders.c sets the process-global flip flag; the thread-local override
     * wins over it, so this decode is never flipped. */
    stbi_set_flip_vertically_on_load_thread(0);

    int w = 0, h = 0, channels = 0;
    const int svg = svg_sniff(bytes, len);
    unsigned char *px = NULL;
    if (svg) {
        /* tex holds the intrinsic-size raster (the fallback);
         * paint draws device-size rasters through image_texture_sized() */
        slot->svg = svg_image_create(bytes, len);
        float fw = 0, fh = 0;
        svg_image_size(slot->svg, &fw, &fh);
        w = (int) lrintf(fw < 1 ? 1 : fw > 4096 ? 4096 : fw);
        h = (int) lrintf(fh < 1 ? 1 : fh > 4096 ? 4096 : fh);
        px = slot->svg ? svg_image_rasterize(slot->svg, w, h) : NULL;
    } else {
        px = stbi_load_from_memory(bytes, (int) len, &w, &h, &channels, 4);
    }
    free(bytes);

    const int webp = !px && !svg && !data_url && image_is_webp_key(slot->key);
    if (webp) px = image_load_webp_sidecar(cache, slot->key, &w, &h);

    if (!px || w <= 0 || h <= 0) {
        if (svg) free(px);
        else stbi_image_free(px);
        image_fail(slot, svg ? "unsupported SVG"
                         : webp ? "WebP without its .webp.png sidecar (tools/ui-import/game_assets.py --v7)"
                         : stbi_failure_reason() ? stbi_failure_reason() : "decode failed");
        return;
    }

    image_premultiply(px, (size_t) w * (size_t) h);

    const unsigned tex = image_upload(px, w, h, 1);
    if (svg) free(px);
    else stbi_image_free(px);

    if (!tex) {
        image_fail(slot, "GL texture upload failed");
        return;
    }

    slot->entry.tex = tex;
    slot->entry.w = w;
    slot->entry.h = h;
    slot->entry.failed = 0;
    slot->entry.vector = svg;
}

/* A file image's natural size from its header (layout needs no pixels); the pixels are
 * decoded on its first draw (image_entry_load).  SVG and WebP (sidecar) load now. */
static void image_probe(const ImageCache *cache, ImageSlot *slot) {
    unsigned char *bytes = NULL;
    size_t len = 0;
    if (!image_read(cache, slot->key, &bytes, &len)) {
        image_fail(slot, "file not found");
        return;
    }
    int w = 0, h = 0, comp = 0;
    const int raster = len <= (size_t) INT32_MAX && !svg_sniff(bytes, len) && !image_is_webp_key(slot->key) &&
                       stbi_info_from_memory(bytes, (int) len, &w, &h, &comp) && w > 0 && h > 0;
    free(bytes);
    if (!raster) {
        image_load(cache, slot, NULL);
        return;
    }
    slot->entry.w = w;
    slot->entry.h = h;
    slot->entry.failed = 0;
    slot->lazy = 1;
}

int image_entry_lazy(const ImageEntry *entry) {
    return entry && !entry->failed && ((const ImageSlot *) entry)->lazy;
}

int image_entry_resident(const ImageEntry *entry) {
    const ImageSlot *slot = (const ImageSlot *) entry;
    return !entry || entry->failed || entry->tex || !slot->lazy;
}

int image_entry_load(ImageCache *cache, const ImageEntry *entry) {
    ImageSlot *slot = (ImageSlot *) entry;
    if (!cache || image_entry_resident(entry)) return entry && entry->tex;
    image_load(cache, slot, NULL);
    cache->decodes++;
    if (slot->entry.failed) {
        image_log_miss(cache, slot->key, slot->key, NULL);
        return 0;
    }
    slot->bytes = (size_t) slot->entry.w * (size_t) slot->entry.h * 4u * 4u / 3u;
    cache->bytes += slot->bytes;
    slot->last_frame = cache->frame;
    return 1;
}

void image_entry_touch(ImageCache *cache, const ImageEntry *entry) {
    if (cache && entry) ((ImageSlot *) entry)->last_frame = cache->frame;
}

/* Frees a lazy entry's textures; it keeps its size and decodes again when drawn. */
static void image_evict(ImageCache *cache, ImageSlot *slot) {
    if (slot->entry.tex) glDeleteTextures(1, &slot->entry.tex);
    slot->entry.tex = 0;
    cache->bytes -= slot->bytes < cache->bytes ? slot->bytes : cache->bytes;
    slot->bytes = 0;
}

static int image_slot_older(const void *a, const void *b) {
    const unsigned fa = (*(ImageSlot *const *) a)->last_frame, fb = (*(ImageSlot *const *) b)->last_frame;
    return fa < fb ? -1 : fa > fb;
}

void image_cache_frame(ImageCache *cache) {
    if (!cache) return;
    cache->frame++;
    if (cache->bytes <= IMAGE_CACHE_BUDGET_BYTES) return;
    /* over budget: drop the least recently drawn lazy entries not drawn for a while */
    ImageSlot **old = malloc((size_t) cache->count * sizeof(*old));
    if (!old) return;
    int n = 0;
    for (int i = 0; i < cache->count; i++) {
        ImageSlot *slot = cache->slots[i];
        if (slot->lazy && slot->entry.tex && slot->last_frame + IMAGE_CACHE_IDLE_FRAMES < cache->frame) old[n++] = slot;
    }
    qsort(old, (size_t) n, sizeof(*old), image_slot_older);
    for (int i = 0; i < n && cache->bytes > IMAGE_CACHE_BUDGET_BYTES / 4u * 3u; i++) image_evict(cache, old[i]);
    free(old);
}

void image_cache_stats(const ImageCache *cache, int *resident, size_t *bytes, int *decodes) {
    int r = 0;
    for (int i = 0; cache && i < cache->count; i++) r += cache->slots[i]->entry.tex != 0;
    if (resident) *resident = r;
    if (bytes) *bytes = cache ? cache->bytes : 0;
    if (decodes) *decodes = cache ? cache->decodes : 0;
}

const ImageEntry *image_get(ImageCache *cache, const char *url, const char *base_url) {
    static const ImageEntry failed_entry = {0, 0, 0, 1, 0};
    if (!cache) return &failed_entry;

    char key[IMAGE_KEY_MAX];
    const int data_url = image_is_data_url(url);
    const int resolved = data_url || image_resolve_url(url, base_url, key, sizeof(key));

    if (data_url) {
        image_data_url_key(url, key, sizeof(key));
    } else if (!resolved) {
        /* keep the raw URL as the key so the rejection is logged only once */
        snprintf(key, sizeof(key), "%s", url ? url : "");
    }

    const uint32_t hash = image_hash(key);
    ImageSlot *slot = image_cache_find(cache, key, hash);
    if (slot) return &slot->entry;

    slot = image_cache_add(cache, key, hash);
    if (!slot) return &failed_entry;

    if (!resolved) image_fail(slot, "rejected URL (scheme, empty, or escapes the ui root)");
    else if (data_url) image_load(cache, slot, url);
    else image_probe(cache, slot);

    if (slot->entry.failed) image_log_miss(cache, key, url, base_url);

    return &slot->entry;
}

/* One texture serves both samplings: gl_backend sets GL_NEAREST (base level only, which
 * is what the old MAX_LEVEL 0 twin held) or the mipmapped linear filter on it per draw
 * from DL_IMAGE's `nearest` flag.  A readback twin used to back `pixelated`; when it could
 * not be made the lazy path drew nothing at all (blank class icons, tab icons, skins). */
unsigned image_texture(const ImageEntry *entry, const int nearest) {
    (void) nearest;
    if (!entry || entry->failed) return 0;
    return entry->tex;
}

unsigned image_texture_sized(const ImageEntry *entry, const int nearest, float dev_w, float dev_h, float frac_x,
                             float frac_y, int *tex_w, int *tex_h) {
    *tex_w = *tex_h = 0;
    if (!entry || entry->failed || !entry->tex) return 0;
    if (!entry->vector) return image_texture(entry, nearest);
    /* vector entries always come from a cache slot (failed_entry is not) */
    ImageSlot *slot = (ImageSlot *) entry;
    if (!slot->svg || !(dev_w >= 0.5f) || !(dev_h >= 0.5f) || dev_w > 4000.0f || dev_h > 4000.0f) return entry->tex;
    /* nearest and smooth sampling coincide when drawn texel-aligned */
    const int qw = (int) lrintf(dev_w * 8.0f), qh = (int) lrintf(dev_h * 8.0f);
    const int qx = (int) lrintf(frac_x * 8.0f) & 7, qy = (int) lrintf(frac_y * 8.0f) & 7;
    const float vw = (float) qw / 8.0f, vh = (float) qh / 8.0f, ox = (float) qx / 8.0f, oy = (float) qy / 8.0f;
    const int w = (int) ceilf(ox + vw - 1e-4f), h = (int) ceilf(oy + vh - 1e-4f);

    ImageSvgTex *victim = &slot->sized[0];
    for (int i = 0; i < IMAGE_SVG_SIZES; i++) {
        ImageSvgTex *t = &slot->sized[i];
        if (t->tex && t->qw == qw && t->qh == qh && t->qx == qx && t->qy == qy) {
            t->last_use = ++slot->use_clock;
            *tex_w = t->w;
            *tex_h = t->h;
            return t->tex;
        }
        if (!t->tex || (victim->tex && t->last_use < victim->last_use)) victim = t;
    }

    unsigned char *px = svg_image_rasterize_placed(slot->svg, w, h, vw, vh, ox, oy);
    if (!px) return entry->tex;
    image_premultiply(px, (size_t) w * (size_t) h);
    const unsigned tex = image_upload(px, w, h, 1);
    free(px);
    if (!tex) return entry->tex;
    if (victim->tex) glDeleteTextures(1, &victim->tex);
    victim->w = w;
    victim->h = h;
    victim->qw = qw;
    victim->qh = qh;
    victim->qx = qx;
    victim->qy = qy;
    victim->tex = tex;
    victim->last_use = ++slot->use_clock;
    *tex_w = w;
    *tex_h = h;
    return tex;
}

/* ------------------------------------------------------------------------- */
/* Geometry helpers                                                           */
/* ------------------------------------------------------------------------- */

void image_fit_size(const float img_w, const float img_h, const float box_w, const float box_h, const int fit,
                    float *out_w, float *out_h) {
    if (img_w <= 0 || img_h <= 0) {
        const int fill = fit == CSS_OBJECT_FIT_FILL;
        *out_w = fill ? box_w : 0;
        *out_h = fill ? box_h : 0;
        return;
    }

    const float sx = box_w / img_w;
    const float sy = box_h / img_h;
    float scale;

    switch (fit) {
        case CSS_OBJECT_FIT_CONTAIN:
            scale = sx < sy ? sx : sy;
            break;
        case CSS_OBJECT_FIT_COVER:
            scale = sx > sy ? sx : sy;
            break;
        case CSS_OBJECT_FIT_NONE:
            scale = 1;
            break;
        case CSS_OBJECT_FIT_SCALE_DOWN:
            scale = sx < sy ? sx : sy;
            if (scale > 1) scale = 1;
            break;
        case CSS_OBJECT_FIT_FILL:
        default:
            *out_w = box_w;
            *out_h = box_h;
            return;
    }

    *out_w = img_w * scale;
    *out_h = img_h * scale;
}

void image_cover_contain_rect(const float img_w, const float img_h, const float box_w, const float box_h,
                              const int fit, float *out_x, float *out_y, float *out_w, float *out_h) {
    image_fit_size(img_w, img_h, box_w, box_h, fit, out_w, out_h);
    *out_x = (box_w - *out_w) * 0.5f;
    *out_y = (box_h - *out_h) * 0.5f;
}

static int image_length_is_auto(const CssLength *length) {
    return !length || length->kind != CSS_LEN_VALUE;
}

static float image_length_resolve(const CssLength *length, const float reference) {
    return length->px + length->pct * 0.01f * reference;
}

void image_bg_size(const float img_w, const float img_h, const float box_w, const float box_h,
                   const CssBgSize *size, float *out_w, float *out_h) {
    if (!size || size->kind == CSS_BG_SIZE_COVER || size->kind == CSS_BG_SIZE_CONTAIN) {
        const int fit = size && size->kind == CSS_BG_SIZE_COVER ? CSS_OBJECT_FIT_COVER : CSS_OBJECT_FIT_CONTAIN;
        if (!size) {
            *out_w = img_w;
            *out_h = img_h;
            return;
        }

        image_fit_size(img_w, img_h, box_w, box_h, fit, out_w, out_h);
        return;
    }

    const int auto_w = image_length_is_auto(&size->w);
    const int auto_h = image_length_is_auto(&size->h);

    if (auto_w && auto_h) {
        *out_w = img_w;
        *out_h = img_h;
    } else if (auto_h) {
        *out_w = image_length_resolve(&size->w, box_w);
        *out_h = img_w > 0 ? *out_w * img_h / img_w : box_h;
    } else if (auto_w) {
        *out_h = image_length_resolve(&size->h, box_h);
        *out_w = img_h > 0 ? *out_h * img_w / img_h : box_w;
    } else {
        *out_w = image_length_resolve(&size->w, box_w);
        *out_h = image_length_resolve(&size->h, box_h);
    }

    if (*out_w < 0) *out_w = 0;
    if (*out_h < 0) *out_h = 0;
}

float image_resolve_position_axis(const float px, const float pct, const float box, const float img) {
    return pct * 0.01f * (box - img) + px;
}

void image_resolve_position(const CssLength *x, const CssLength *y, const float box_w, const float box_h,
                            const float img_w, const float img_h, float *out_x, float *out_y) {
    const int x_value = x && x->kind == CSS_LEN_VALUE;
    const int y_value = y && y->kind == CSS_LEN_VALUE;

    *out_x = image_resolve_position_axis(x_value ? x->px : 0, x_value ? x->pct : 0, box_w, img_w);
    *out_y = image_resolve_position_axis(y_value ? y->px : 0, y_value ? y->pct : 0, box_h, img_h);
}

int image_repeat_round_count(const float box, const float tile) {
    if (box <= 0 || tile <= 0) return 1;

    const int count = (int) floorf(box / tile + 0.5f);
    return count < 1 ? 1 : count;
}

float image_repeat_round_size(const float box, const float tile) {
    if (box <= 0 || tile <= 0) return box;
    return box / (float) image_repeat_round_count(box, tile);
}
