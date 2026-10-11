/*
 * image.h - raster image cache for the KrunkNative HTML/CSS engine
 * (doc/html-engine-plan.md: "image.c/.h stb_image decode (no flip),
 * LINEAR/NEAREST per image-rendering, cache by URL, missing -> paint nothing").
 *
 * Ownership and lifetime
 * ----------------------
 * One ImageCache per document (root = the ui/ directory).  Entries are
 * created on first use, never evicted, and stay at a stable address until
 * image_cache_destroy(), so paint may keep `const ImageEntry *` across frames.
 * Loading is synchronous: image_get() decodes and uploads on the calling
 * thread, which must own the GL context.  GL texture binding state is
 * restored after every upload, so paint can call image_get() mid-frame.
 *
 * URL resolution (image_resolve_url)
 * ----------------------------------
 * `base_url` is the URL of the document or stylesheet the reference was
 * written in, relative to the ui/ root ("index.html", "css/main.css",
 * "windows/host.html"; NULL or "" == root).  `url` is resolved against it
 * like a browser does: query (`?build=...`) and fragment are stripped, "."
 * and ".." segments are collapsed, a leading "/" means the ui/ root.  A URL
 * that escapes above the root, has a scheme (http:, ...; data: URLs are
 * decoded in place, see image_decode_data_url), or is
 * empty resolves to a failed entry.  The normalised root-relative path
 * ("img/logo_4.png") is both the cache key and the request URL on the web.
 *
 * Decoding and pixel format
 * -------------------------
 * SVG documents (files or data: URLs, detected by content) are parsed and
 * rasterised by svg.c (nanosvg).  `tex` holds a raster at the intrinsic size
 * (width/height, see svg.h) and `vector` is set; image_texture_sized() returns
 * rasters at the device size actually drawn (cached per size, re-rasterised
 * when the scale changes), like Blink drawing an SVG image.  The pixels then
 * follow the same premultiply/mip/upload path as a decoded PNG.
 * stb_image, forced to RGBA8 with NO vertical flip (row 0 is the top of the
 * image, which is what the y-down display list expects).  The decoder
 * expands palette + tRNS and RGB PNGs itself, so every entry is a straight
 * 8-bit RGBA bitmap before the step below.  Note that client/src/loaders.c
 * flips through the process-global stb flag; this module uses the
 * thread-local override, so the two loaders do not interfere.
 *
 * PREMULTIPLIED ALPHA.  Pixels are converted to premultiplied alpha on load
 * (rgb = round(rgb * a / 255)) and the GL texture holds premultiplied data.
 * The engine composites premultiplied (one blend mode, correct LINEAR
 * filtering across transparent edges, FBO groups compose without fringes);
 * paint must set DlItem.image.premultiplied = 1 for these textures.
 *
 * GL textures
 * -----------
 * GL_TEXTURE_2D, internal format GL_RGBA8, GL_CLAMP_TO_EDGE on both axes.
 * `tex` (CSS `image-rendering: auto/smooth`) has a full mip chain built from
 * the premultiplied pixels and is created GL_LINEAR_MIPMAP_LINEAR /
 * GL_LINEAR.  Chrome (Skia, default filter quality) draws a downscaled image
 * bilinearly from one mip level, floor(log2(inverse scale)); html_ui.frag
 * reproduces that with textureLod.  Upscaled images are plain bilinear.
 * Without mips, large PNGs drawn small (logo_4 at ~0.47x, menu icons,
 * sprites) alias visibly.
 * image_texture(entry, 1) lazily creates `tex_nearest`, a second texture
 * object with the same level-0 pixels, GL_NEAREST and no mips (MAX_LEVEL 0)
 * for `pixelated`/`crisp-edges` (the 8x8 class icons depend on this).
 * gl_backend.c re-applies the matching filters per draw from DL_IMAGE's
 * `nearest` flag.  Everything here stays within the
 * GLES 3.0 subset (see client/include/gl.h).
 *
 * Web build
 * ---------
 * ui/ is served at the page root and preloaded/fetched into MEMFS under
 * ui_dir ("/ui/").  A file missing from MEMFS is fetched synchronously
 * (ASYNCIFY) with emscripten_wget_data() from the root-relative key, exactly
 * like shared/src/utils.c does for /assets/, then stored in MEMFS so a
 * second cache in the same session finds it.  ensure_asset_file() itself is
 * not used because it maps "/ui/img/x.png" to "ui/img/x.png", which is not
 * where the page serves it from.
 *
 * Geometry helpers (CSS Backgrounds 3 / CSS Images 3 semantics)
 * -------------------------------------------------------------
 * These are pure functions in CSS px so paint.c does not re-derive the math.
 */
#ifndef KN_IMAGE_H
#define KN_IMAGE_H

#include <stddef.h>
#include "css_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ImageEntry {
    unsigned tex;          /* mipmapped GL_LINEAR texture, 0 when failed      */
    int w, h;              /* intrinsic size in image px (0 when failed)      */
    int failed;            /* 1: missing, unreadable, undecodable, bad URL    */
    unsigned tex_nearest;  /* GL_NEAREST twin, created by image_texture()     */
    int vector;            /* 1: SVG, image_texture_sized() rasterises per size */
} ImageEntry;

typedef struct ImageCache ImageCache;

/* `ui_dir` is the document root ("../ui/", "./ui/", "/ui/"); a trailing slash
 * is added when missing.  Returns NULL only on allocation failure. */
ImageCache *image_cache_create(const char *ui_dir);

/* Deletes every GL texture (the context must be current) and frees the cache. */
void image_cache_destroy(ImageCache *cache);

/* Look up (and load on first use) the image `url` referenced from `base_url`.
 * Never returns NULL: a failed load returns an entry with failed == 1 and
 * tex == 0, and the failure was logged once.  Paint draws nothing for it. */
/* data: URLs: 1 when `url` is one; decodes its payload (base64 or
 * percent-encoded) into a malloc'd buffer.  image_get() accepts them (cached
 * by a hash of the whole URL); SVG payloads go through svg.c like files. */
int image_is_data_url(const char *url);
int image_decode_data_url(const char *url, unsigned char **out, size_t *out_len);

const ImageEntry *image_get(ImageCache *cache, const char *url, const char *base_url);

/* Texture for the requested sampling: nearest != 0 returns `tex_nearest`,
 * creating it on first request (same pixels, GL_NEAREST).  Returns 0 for a
 * failed entry or when the twin could not be created (caller may fall back
 * to `tex`). */
unsigned image_texture(const ImageEntry *entry, int nearest);

/* Texture for drawing `entry` over dev_w x dev_h device pixels (the size the
 * whole image covers on the target, fractional) whose top-left sits at
 * (frac_x, frac_y) inside its first device pixel.  Raster images:
 * image_texture(), *tex_w = *tex_h = 0.  SVG (entry->vector): a raster of
 * *tex_w x *tex_h = ceil(frac + dev) texels with the image placed at that
 * sub-pixel offset, meant to be drawn texel-aligned on the device grid (as
 * Chrome draws the vector there: no resampling blur).  Created on first
 * request, kept in a small per-image LRU (positions quantised to 1/8 px);
 * falls back to `tex` (*tex_w = 0) when rasterising fails.  The GL context
 * must be current; the texture binding of the active unit is preserved. */
unsigned image_texture_sized(const ImageEntry *entry, int nearest, float dev_w, float dev_h, float frac_x, float frac_y,
                             int *tex_w, int *tex_h);

/* Resolve `url` against `base_url` into the root-relative key ("img/x.png").
 * Returns 1 and writes at most `cap` bytes (NUL-terminated) on success, 0 when
 * the URL is rejected (scheme, escape above the root, empty, too long). */
int image_resolve_url(const char *url, const char *base_url, char *out, size_t cap);

/* Number of entries currently in the cache (loaded or failed). */
int image_cache_count(const ImageCache *cache);

/* ---- geometry helpers ---------------------------------------------------- */

/* Concrete object size for `fit` (CssObjectFit: FILL, CONTAIN, COVER, NONE,
 * SCALE_DOWN) of an img_w x img_h image inside a box_w x box_h box.  A zero
 * or negative image dimension yields the box size (FILL) or 0. */
void image_fit_size(float img_w, float img_h, float box_w, float box_h, int fit,
                    float *out_w, float *out_h);

/* image_fit_size() plus the rect of that size centred in the box (object
 * position / background-position 50% 50%).  `out_x`/`out_y` are relative to
 * the box origin and may be negative for COVER. */
void image_cover_contain_rect(float img_w, float img_h, float box_w, float box_h, int fit,
                              float *out_x, float *out_y, float *out_w, float *out_h);

/* background-size: cover / contain / <length|pct|auto>{1,2} against the
 * positioning area.  `auto` on one axis keeps the intrinsic aspect ratio,
 * `auto auto` is the intrinsic size.  Percentages refer to the box. */
void image_bg_size(float img_w, float img_h, float box_w, float box_h, const CssBgSize *size,
                   float *out_w, float *out_h);

/* One axis of background-position / object-position.  Percentages place the
 * same fraction of image and box together (offset = pct/100 * (box - img)),
 * px is an extra offset; css_values encodes `right 8px` as {px:-8, pct:100}
 * and `center` as {pct:50}, so edge keywords need no flags here. */
float image_resolve_position_axis(float px, float pct, float box, float img);

/* Both axes from CssLength (kind VALUE expected; AUTO behaves like 0%). */
void image_resolve_position(const CssLength *x, const CssLength *y, float box_w, float box_h,
                            float img_w, float img_h, float *out_x, float *out_y);

/* background-repeat: round on one axis: number of whole tiles that fit after
 * rescaling (max(1, round(box / tile))) and the rescaled tile size.  A
 * non-positive tile or box yields 1 tile of the box size. */
int image_repeat_round_count(float box, float tile);
float image_repeat_round_size(float box, float tile);

#ifdef __cplusplus
}
#endif

#endif /* KN_IMAGE_H */
