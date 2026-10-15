#pragma once

// Text subsystem of the native HTML/CSS engine: FreeType faces, CSS font
// matching, shaping-lite (kerning + Material Icons ligatures + fallback),
// device-size glyph rasterisation into one GL_R8 atlas, UAX#14-subset line
// breaking, text-transform and a Skia-style gamma LUT.
//
// Pure C11 + FreeType, no HarfBuzz.  GL access goes through client/include/gl.h
// and only happens inside text_glyph_quad() / text_atlas_*() so layout and
// measurement can run without a context.
//
// Units: every public float is in CSS px unless the name says "device".
// Metrics follow Blink's SimpleFontData::PlatformInit on Linux (see
// text_font_metrics()).  Environment knobs (read once in text_create()):
//   KRUNKNATIVE_HTML_HINTING=none|slight|full   (default slight = Chrome on
//       Linux with fontconfig hintslight): slight = FT_LOAD_TARGET_LIGHT
//       outlines + linear advances + 1/4-px phases; none = unhinted outlines,
//       same positioning; full = native/full hinting, hinted integer advances,
//       no phases (Chrome's full-hinting-at-DSF-1 behaviour).
//   KRUNKNATIVE_HTML_SUBPX=0|1             override 1/4-px x phases
//   KRUNKNATIVE_HTML_LINEAR_METRICS=0|1    override linear (unhinted) advances
//   KRUNKNATIVE_HTML_AUTOHINT=0|1          add FT_LOAD_FORCE_AUTOHINT
//   KRUNKNATIVE_HTML_FALLBACK_FONT=path[:path...]   fallback faces for glyphs
//       missing from the requested family (default: DejaVu Sans / Liberation
//       Sans / Noto Sans, then the bundled ui/css/fonts/kn-noto-* script faces
//       (text_set_lang), then Noto Sans CJK, whichever exist).  Always first:
//       the bundled ui/css/fonts/kn-symbols.ttf (arrows, shapes, dingbats...),
//       per-glyph only.  Generic `monospace` uses Noto Sans Mono / DejaVu Sans
//       Mono / Liberation Mono when present (Chrome's fontconfig monospace).
//   KRUNKNATIVE_HTML_OBLIQUE_SKEW=0.2      synthetic oblique shear (x += k*y).
//   KRUNKNATIVE_HTML_FAKE_BOLD_DIVISOR=n   synthetic-bold width = size / n instead of
//       Skia's fake-bold curve (size/24 at <= 9 px down to size/32 at >= 36 px).
//   KRUNKNATIVE_HTML_FAKE_BOLD_ADVANCE=0|1 add the embolden strength to the
//       advance of synthetic-bold glyphs (Skia/FreeType leave it unchanged).
//   KRUNKNATIVE_HTML_TEXT_GAMMA=gamma:contrast   text_gamma_lut() parameters
//       (default 1.2:0.2, Chromium's Linux SK_GAMMA_EXPONENT/CONTRAST).

#include <math.h>
#include <stddef.h>
#include <stdint.h>

typedef struct TextSystem TextSystem;
typedef struct TextFace TextFace;
typedef struct TextFont TextFont;

enum {
    TEXT_STYLE_NORMAL = 0,
    TEXT_STYLE_ITALIC = 1,
};

enum {
    TEXT_SYNTH_BOLD = 1,
    TEXT_SYNTH_OBLIQUE = 2,
};

enum {
    TEXT_HINT_NONE = 0,
    TEXT_HINT_SLIGHT = 1,
    TEXT_HINT_FULL = 2,
};

// Break flags written by text_break_opportunities(), one per byte of input.
enum {
    TEXT_BREAK_ALLOWED = 1,   // a line may break before this byte
    TEXT_BREAK_SPACE = 2,     // this byte starts a collapsible space (hangs at line end)
    TEXT_BREAK_ZERO_WIDTH = 4 // this byte starts a zero-width (invisible) character
};

typedef struct Glyph {
    uint32_t glyph_index;  // index in `face`
    TextFace *face;        // face that supplies the glyph (fallbacks differ per glyph)
    float x;               // pen x of this glyph in CSS px from run start
    float advance;         // advance in CSS px (kerning applied, letter-spacing excluded)
    uint8_t phase;         // 1/4-px subpixel x phase (0..3) of `x`; recompute with text_phase() after scaling
    uint8_t synth;         // TEXT_SYNTH_* flags of the font that produced the glyph
    uint16_t cluster;      // byte offset of the source character (or ligature start) in the run
} Glyph;

typedef struct GlyphQuad {
    uint32_t texture;      // GL atlas texture (0 when the glyph has no ink)
    float u0, v0, u1, v1;  // normalised atlas rect
    float x_off, y_off;    // device px offset from (floor(pen_x), baseline_y), y down
    float w, h;            // device px size of the bitmap
    uint32_t generation;   // atlas generation the quad belongs to (see text_atlas_generation)
} GlyphQuad;

typedef struct TextMetrics {
    float size;        // CSS px font size
    float ascent;      // rounded, CSS px above the baseline
    float descent;     // rounded, CSS px below the baseline (positive)
    float line_gap;    // unrounded
    float line_height; // `line-height: normal` = round(ascent)+round(descent)+round(line_gap)
    float x_height;    // OS/2 sxHeight or 'x' glyph bounds (unrounded)
    float cap_height;  // OS/2 sCapHeight or 'H' glyph bounds (unrounded)
    float units_per_em;
    float ascent_raw;  // unrounded, for vertical-align/baseline maths that need it
    float descent_raw;
} TextMetrics;

// ---- system / faces ----------------------------------------------------------

TextSystem *text_create(const char *ui_dir);
void text_destroy(TextSystem *ts);

// Registers one @font-face descriptor.  `path` is relative to ui_dir unless
// absolute; a `.woff2` path is retried as `.ttf`.  weight = CSS weight (400
// default), style = TEXT_STYLE_*.  Faces load lazily.  Returns 0 on success.
int text_register_face(TextSystem *ts, const char *family, const char *path, int weight, int style);

// Document language (BCP 47, <html lang>): "ja", "zh-CN", "zh-TW", "ko", "hi" move the bundled
// script face of that language (ui/css/fonts/kn-noto-*) to the front of the per-glyph fallback
// chain; others (NULL) keep the default order.  Returns 1 when the choice changed (re-lay out).
int text_set_lang(TextSystem *ts, const char *lang);
// 1 when `cp` gets a real glyph (not .notdef) from the font or its fallback chain.
int text_font_has_glyph(TextFont *font, uint32_t cp);

// Current hinting mode (TEXT_HINT_*) from KRUNKNATIVE_HTML_HINTING.
int text_hinting(const TextSystem *ts);
// 1 when glyphs are positioned with 1/4-px x phases (and linear advances).
int text_subpixel(const TextSystem *ts);

const char *text_face_family(const TextFace *face);
int text_face_is_icon_font(const TextFace *face);

// ---- fonts -------------------------------------------------------------------

// Resolves a CSS font-family list ("headerfont, sans-serif") + size/weight/
// italic to a cached TextFont.  Generic/unknown families use the fallback
// list.  Never returns NULL once any face (or fallback) is loadable.
TextFont *text_font(TextSystem *ts, const char *family_list, float size_px, int weight, int italic);
const TextMetrics *text_font_metrics(const TextFont *font);
// Blink's SimpleFontData avg/max character widths, used for <input size=N> and
// <select> intrinsic widths: avg = round(OS/2 xAvgCharWidth) (x-height when the
// table is missing or zero), max = round(head.xMax). CSS px at the font size.
// Returns 0 when the face cannot be loaded.
int text_font_char_widths(TextFont *font, float *avg, float *max);
TextFace *text_font_face(const TextFont *font);
unsigned text_font_synth(const TextFont *font); // TEXT_SYNTH_* flags
float text_font_size(const TextFont *font);

// ---- shaping / measuring -----------------------------------------------------

// Shapes one run (single font, single direction).  Writes up to `max` glyphs
// to `out` (may be NULL to count only) and the total advance incl.
// letter_spacing after every glyph (CSS semantics) to *total_advance.
// Returns the number of glyphs produced (even if > max).
int text_shape(TextFont *font, const char *utf8, size_t len, float letter_spacing,
               Glyph *out, int max, float *total_advance);

float text_measure(TextFont *font, const char *utf8, size_t len, float letter_spacing);

// 1/4-px phase of a device x position (truncating; kept for callers that
// floor() the integer part themselves).
static inline uint8_t text_phase(float device_x) {
    float f = device_x - floorf(device_x);
    return (uint8_t) ((int) (f * 4.0f) & 3);
}

// Skia-exact device snapping of a glyph origin (SkGlyphRunPainter /
// SkPackedGlyphID): x is biased by 1/8 px, then split into an integer pixel and
// a 1/4-px phase; y (horizontal text is axis aligned) is rounded to a whole
// pixel.  Without subpixel positioning x is rounded like y and phase is 0.
// Draw the GlyphQuad of a glyph at (*ix + x_off, *iy + y_off).
static inline uint8_t text_snap_origin(const TextSystem *ts, float device_x, float device_y, float *ix, float *iy) {
    *iy = floorf(device_y + 0.5f);
    if (!text_subpixel(ts)) {
        *ix = floorf(device_x + 0.5f);
        return 0;
    }
    float x = device_x + 0.125f;
    *ix = floorf(x);
    return (uint8_t) ((int) ((x - *ix) * 4.0f) & 3);
}

// text-transform: uppercase for ASCII + Latin-1 (ß -> SS, ÿ -> Ÿ, µ kept).
// Returns bytes written (excluding the NUL that is added when it fits).
size_t text_uppercase(const char *utf8, size_t len, char *out, size_t cap);
// Simple case mapping of one code point (Latin-1 / Extended-A / Additional, Greek, Cyrillic).
uint32_t text_case_upper(uint32_t cp);
uint32_t text_case_lower(uint32_t cp);

// UAX#14 subset: flags[i] gets TEXT_BREAK_* for byte i (0 for continuation
// bytes).  Break after spaces, after hyphens (not before, not before digits),
// after U+200B, around CJK ideographs/kana/Hangul (not before closing or after
// opening CJK punctuation).  `nowrap` and <br> are the caller's business.
void text_break_opportunities(const char *utf8, size_t len, uint8_t *flags);

// ---- rasterisation / atlas ---------------------------------------------------

// Rasterises (or fetches) the glyph at `device_size` px with `synth` flags and
// the glyph's `phase`, packs it into the atlas and fills *out.  Returns 1 on
// success, 0 if the glyph has no ink (out->texture == 0) or failed.
// Requires a current GL context.  Draw at
//   x = floor(pen_device_x) + out->x_off,  y = baseline_device_y + out->y_off.
int text_glyph_quad(TextSystem *ts, const Glyph *g, float device_size, unsigned synth, GlyphQuad *out);

// Atlas texture id (0 before the first glyph) and its current generation.
// The generation increments whenever the atlas is rebuilt (>90 % full);
// cached GlyphQuads from an older generation must be re-requested.
uint32_t text_atlas_texture(const TextSystem *ts);
uint32_t text_atlas_generation(const TextSystem *ts);
int text_atlas_size(const TextSystem *ts);
float text_atlas_fill(const TextSystem *ts); // 0..1 fraction of rows consumed

// Debug: reads the atlas back (desktop GL) and writes a binary PGM.
int text_atlas_dump_pgm(TextSystem *ts, const char *path);

// ---- gamma -------------------------------------------------------------------

// Skia SkMaskGamma preblend tables: out[lum_bucket][coverage] -> corrected
// coverage, 8 buckets over the text colour's luminance (index = lum >> 5).
// text_gamma_lut() uses KRUNKNATIVE_HTML_TEXT_GAMMA (default 1.2:0.2).
void text_gamma_lut(uint8_t out[8][256]);
void text_gamma_lut_ex(uint8_t out[8][256], float gamma, float contrast);
// gamma/contrast that text_gamma_lut() uses (defaults or KRUNKNATIVE_HTML_TEXT_GAMMA).
void text_gamma_params(float *gamma, float *contrast);
// Bucket index for a text colour (sRGB 0..255), same luminance as Skia.
int text_gamma_bucket(uint8_t r, uint8_t g, uint8_t b, float gamma);
