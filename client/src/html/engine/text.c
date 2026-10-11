// Text subsystem of the native HTML/CSS engine.  See text.h for the contract.
//
// Chrome parity notes (Linux / Skia FreeType backend, headless):
//  * Metrics: FreeType's face->ascender/descender/height are what Skia reads.
//    FreeType picks OS/2 sTypo* when fsSelection bit 7 (USE_TYPO_METRICS) is
//    set, otherwise hhea, otherwise OS/2 sTypo*, otherwise usWin*.  Blink then
//    rounds ascent and descent to integers (SkScalarRoundToScalar) and computes
//    line-height:normal as lroundf(ascent)+lroundf(descent)+lroundf(lineGap)
//    (third_party/blink/renderer/platform/fonts/simple_font_data.cc).  Blink
//    also consults a VDMX table when hinting is enabled; header.ttf ships one
//    but that path is not reproduced here (document if #gameName drifts).
//  * Hinting / positioning (Blink WebFontRenderStyle::ApplyToSkFont + Skia's
//    FreeType scaler, fontconfig `hintslight` as on stock Linux): outlines are
//    loaded with FT_LOAD_TARGET_LIGHT (y-only autohint), advances are linear
//    (linearHoriAdvance, fractional) because light hinting is y-only, and
//    subpixel positioning is forced on (1/4-px x phases).  Only `full`
//    hinting at device scale 1 turns subpixel positioning off and uses hinted
//    integer advances.  Measured against doc/ref (menu + windows + in-game):
//    slight >> none > full; see text_snap_origin() for the device snapping.
//  * Synthetic bold: FT_Outline_Embolden with strength = em/24 px
//    (SkFontHost_FreeType_common.cpp kOutlineEmboldenDivisor); Skia does not
//    widen the advance (KRUNKNATIVE_HTML_FAKE_BOLD_ADVANCE=1 does).
//  * Synthetic oblique: Blink calls SkFont::setSkewX(-1/4); the plan asks for
//    0.2 to be compared visually, so the shear is a knob (default 0.2).
//  * Gamma: SkMaskGamma preblend, see text_gamma_lut_ex().

#include "text.h"
#include "material_icons_liga.h"

#include <gl.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include FT_TRUETYPE_TABLES_H

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEXT_ATLAS_SIZE 2048
#define TEXT_ATLAS_PAD 1
#define TEXT_ATLAS_REBUILD_FILL 0.9f
#define TEXT_MAX_FACES 48
#define TEXT_FAMILY_MAX 64
#define TEXT_EMBOLDEN_DIVISOR 24.0f   // Skia kOutlineEmboldenDivisor
#define TEXT_DEFAULT_OBLIQUE_SKEW 0.2f
#ifndef TEXT_DEFAULT_HINTING
#define TEXT_DEFAULT_HINTING TEXT_HINT_SLIGHT
#endif
// Chromium's Linux Skia build sets SK_GAMMA_EXPONENT=1.2, SK_GAMMA_CONTRAST=0.2;
// measured best (tied with 1.0..1.4) against doc/ref, 2.2:0.5 clearly worse.
#define TEXT_DEFAULT_GAMMA 1.2f
#define TEXT_DEFAULT_CONTRAST 0.2f

struct TextFace {
    char family[TEXT_FAMILY_MAX]; // lower-case
    char *path;
    int weight;
    int style;
    int is_icon;
    int is_fallback;
    int id;
    FT_Face ft;          // NULL until loaded
    int load_failed;
    int has_kerning;
    long cur_size64;     // last FT_Set_Char_Size applied to ft
};

struct TextFont {
    TextSystem *ts;
    TextFace *face;
    float size;
    long size64;
    unsigned synth;
    TextMetrics metrics;
    // advance cache: open addressing, key = face_id<<24 | glyph, bit 32 = used
    uint64_t *adv_keys;
    float *adv_vals;
    int adv_cap;
    int adv_count;
};

typedef struct AtlasEntry {
    uint64_t key;    // 0 = empty
    uint16_t x, y;   // glyph rect (inside the padding)
    uint16_t w, h;
    int16_t left, top;
    uint32_t last_used;
} AtlasEntry;

struct TextSystem {
    FT_Library ft;
    char *ui_dir;
    TextFace faces[TEXT_MAX_FACES];
    int face_count;
    TextFace *fallback[8];
    int fallback_count;
    TextFont **fonts;
    int font_count, font_cap;

    int hinting;
    int subpixel;        // 1/4-px x phases + linear advances (Chrome: on unless full hinting)
    int linear_metrics;  // unhinted fractional advances
    int force_autohint;
    float bold_divisor;  // embolden strength = size / bold_divisor
    float oblique_skew;
    int fake_bold_advance;
    float gamma, contrast;

    // ligature index: table ranges per first byte
    uint16_t liga_lo[256], liga_hi[256];

    // atlas
    GLuint tex;
    uint32_t generation;
    int shelf_y, shelf_h, shelf_x;
    AtlasEntry *entries;
    int entry_cap, entry_count;
    uint32_t tick;
    uint8_t *scratch;
    size_t scratch_cap;
};

// ---- small helpers -----------------------------------------------------------

static char *text_strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

static int file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static int is_abs_path(const char *p) {
    return p[0] == '/' || (isalpha((unsigned char) p[0]) && p[1] == ':') || p[0] == '\\';
}

static void str_lower_copy(char *dst, size_t cap, const char *src, size_t len) {
    size_t n = len < cap - 1 ? len : cap - 1;
    for (size_t i = 0; i < n; i++) dst[i] = (char) tolower((unsigned char) src[i]);
    dst[n] = 0;
}

static float round_half_up(float v) { // SkScalarRoundToScalar
    return floorf(v + 0.5f);
}

// Decodes one UTF-8 code point; invalid bytes map to U+FFFD and consume 1 byte.
static uint32_t utf8_next(const char *s, size_t len, size_t *i) {
    const unsigned char *u = (const unsigned char *) s;
    size_t p = *i;
    unsigned char c = u[p];
    if (c < 0x80) { *i = p + 1; return c; }
    int n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    if (!n || p + n >= len) { *i = p + 1; return 0xFFFD; }
    uint32_t cp = c & (0x3F >> n);
    for (int k = 1; k <= n; k++) {
        if ((u[p + k] & 0xC0) != 0x80) { *i = p + 1; return 0xFFFD; }
        cp = (cp << 6) | (u[p + k] & 0x3F);
    }
    *i = p + n + 1;
    return cp;
}

static size_t utf8_put(uint32_t cp, char *out, size_t cap) {
    char tmp[4];
    size_t n;
    if (cp < 0x80) { tmp[0] = (char) cp; n = 1; }
    else if (cp < 0x800) { tmp[0] = (char) (0xC0 | (cp >> 6)); tmp[1] = (char) (0x80 | (cp & 0x3F)); n = 2; }
    else if (cp < 0x10000) { tmp[0] = (char) (0xE0 | (cp >> 12)); tmp[1] = (char) (0x80 | ((cp >> 6) & 0x3F)); tmp[2] = (char) (0x80 | (cp & 0x3F)); n = 3; }
    else { tmp[0] = (char) (0xF0 | (cp >> 18)); tmp[1] = (char) (0x80 | ((cp >> 12) & 0x3F)); tmp[2] = (char) (0x80 | ((cp >> 6) & 0x3F)); tmp[3] = (char) (0x80 | (cp & 0x3F)); n = 4; }
    if (n > cap) return 0;
    memcpy(out, tmp, n);
    return n;
}

static int env_int(const char *name, int def) {
    const char *v = getenv(name);
    return v && *v ? atoi(v) : def;
}

static float env_float(const char *name, float def) {
    const char *v = getenv(name);
    return v && *v ? (float) atof(v) : def;
}

// ---- system ------------------------------------------------------------------

static void text_build_liga_index(TextSystem *ts) {
    for (int c = 0; c < 256; c++) { ts->liga_lo[c] = 0; ts->liga_hi[c] = 0; }
    int i = 0;
    while (i < MATERIAL_ICONS_LIGA_COUNT) {
        unsigned char c = (unsigned char) material_icons_liga[i].name[0];
        int j = i;
        while (j < MATERIAL_ICONS_LIGA_COUNT && (unsigned char) material_icons_liga[j].name[0] == c) j++;
        ts->liga_lo[c] = (uint16_t) i;
        ts->liga_hi[c] = (uint16_t) j;
        i = j;
    }
}

static TextFace *text_add_face(TextSystem *ts, const char *family, const char *path, int weight, int style, int is_fallback) {
    if (ts->face_count >= TEXT_MAX_FACES) return NULL;
    TextFace *f = &ts->faces[ts->face_count];
    memset(f, 0, sizeof *f);
    f->id = ts->face_count;
    str_lower_copy(f->family, sizeof f->family, family, strlen(family));
    f->path = text_strdup(path);
    f->weight = weight > 0 ? weight : 400;
    f->style = style;
    f->is_fallback = is_fallback;
    f->is_icon = strncmp(f->family, "material icons", 14) == 0 || strncmp(f->family, "material symbols", 16) == 0;
    f->cur_size64 = -1;
    ts->face_count++;
    return f;
}

/* Bundled symbol fallback (ui/css/fonts/kn-symbols.ttf, an OFL subset of
 * Adwaita Mono Regular: arrows, misc technical, box drawing, blocks,
 * geometric shapes, misc symbols, dingbats, misc symbols and arrows).  Chrome
 * draws these from system fonts; on the reference machine its fontconfig
 * fallback for e.g. U+2715 (the dev console close button) is Adwaita Mono
 * (synthetic bold when bold).  Listed first so the glyph does not depend on
 * which system fonts a machine has. */
static void text_add_bundled_symbols(TextSystem *ts) {
    char path[1024];
    const size_t n = strlen(ts->ui_dir);
    snprintf(path, sizeof path, "%s%scss/fonts/kn-symbols.ttf", ts->ui_dir, n && ts->ui_dir[n - 1] == '/' ? "" : "/");
    if (ts->fallback_count < 8 && file_exists(path)) {
        TextFace *f = text_add_face(ts, "fallback-symbols", path, 400, TEXT_STYLE_NORMAL, 1);
        if (f) ts->fallback[ts->fallback_count++] = f;
    }
}

static void text_add_fallbacks(TextSystem *ts) {
    text_add_bundled_symbols(ts);
    const int bundled = ts->fallback_count;
    const char *env = getenv("KRUNKNATIVE_HTML_FALLBACK_FONT");
    if (env && *env) {
        char *copy = text_strdup(env);
        for (char *tok = copy; tok && *tok;) {
            char *sep = strpbrk(tok, ":;");
            if (sep) *sep = 0;
            if (*tok && ts->fallback_count < 8 && file_exists(tok)) {
                TextFace *f = text_add_face(ts, "fallback", tok, 400, TEXT_STYLE_NORMAL, 1);
                if (f) ts->fallback[ts->fallback_count++] = f;
            }
            tok = sep ? sep + 1 : NULL;
        }
        free(copy);
        if (ts->fallback_count > bundled) return;
    }
    static const char *const latin[] = {
        "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/liberation-sans-fonts/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/google-noto-vf/NotoSans[wght].ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "/System/Library/Fonts/Supplemental/Arial.ttf",
        "C:\\Windows\\Fonts\\arial.ttf",
    };
    static const char *const cjk[] = {
        "/usr/share/fonts/google-noto-sans-cjk-vf-fonts/NotoSansCJK-VF.ttc",
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf",
        "C:\\Windows\\Fonts\\msyh.ttc",
    };
    for (size_t i = 0; i < sizeof latin / sizeof *latin; i++) {
        if (file_exists(latin[i])) {
            TextFace *f = text_add_face(ts, "fallback", latin[i], 400, TEXT_STYLE_NORMAL, 1);
            if (f) ts->fallback[ts->fallback_count++] = f;
            break;
        }
    }
    for (size_t i = 0; i < sizeof cjk / sizeof *cjk; i++) {
        if (file_exists(cjk[i])) {
            TextFace *f = text_add_face(ts, "fallback-cjk", cjk[i], 400, TEXT_STYLE_NORMAL, 1);
            if (f) ts->fallback[ts->fallback_count++] = f;
            break;
        }
    }
    /* generic `monospace` (text_resolve_family_list): Chrome on Linux uses
     * fontconfig's monospace, Noto Sans Mono on the reference machine */
    static const char *const mono[] = {
        "/usr/share/fonts/google-noto-vf/NotoSansMono[wght].ttf",
        "/usr/share/fonts/truetype/noto/NotoSansMono-Regular.ttf",
        "/usr/share/fonts/noto/NotoSansMono-Regular.ttf",
        "/usr/share/fonts/dejavu-sans-mono-fonts/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
        "/usr/share/fonts/liberation-mono-fonts/LiberationMono-Regular.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
        "/System/Library/Fonts/Menlo.ttc",
        "C:\\Windows\\Fonts\\consola.ttf",
    };
    for (size_t i = 0; i < sizeof mono / sizeof *mono && ts->fallback_count < 8; i++) {
        if (file_exists(mono[i])) {
            TextFace *f = text_add_face(ts, "fallback-mono", mono[i], 400, TEXT_STYLE_NORMAL, 1);
            if (f) ts->fallback[ts->fallback_count++] = f;
            break;
        }
    }
}

TextSystem *text_create(const char *ui_dir) {
    TextSystem *ts = calloc(1, sizeof *ts);
    if (!ts) return NULL;
    if (FT_Init_FreeType(&ts->ft)) {
        free(ts);
        return NULL;
    }
    ts->ui_dir = text_strdup(ui_dir ? ui_dir : ".");

    const char *hint = getenv("KRUNKNATIVE_HTML_HINTING");
    ts->hinting = TEXT_DEFAULT_HINTING;
    if (hint && !strcmp(hint, "none")) ts->hinting = TEXT_HINT_NONE;
    else if (hint && !strcmp(hint, "slight")) ts->hinting = TEXT_HINT_SLIGHT;
    else if (hint && !strcmp(hint, "full")) ts->hinting = TEXT_HINT_FULL;
    // Blink WebFontRenderStyle::ApplyToSkFont: subpixel positioning is forced on
    // except for full hinting at device scale 1; linear metrics follow it (Skia
    // uses linear advances for light hinting because it is y-only).
    ts->subpixel = env_int("KRUNKNATIVE_HTML_SUBPX", ts->hinting != TEXT_HINT_FULL) != 0;
    ts->linear_metrics = env_int("KRUNKNATIVE_HTML_LINEAR_METRICS", ts->subpixel || ts->hinting == TEXT_HINT_NONE) != 0;
    ts->force_autohint = env_int("KRUNKNATIVE_HTML_AUTOHINT", 0) != 0;
    ts->bold_divisor = env_float("KRUNKNATIVE_HTML_FAKE_BOLD_DIVISOR", TEXT_EMBOLDEN_DIVISOR);
    if (ts->bold_divisor <= 0) ts->bold_divisor = TEXT_EMBOLDEN_DIVISOR;
    ts->oblique_skew = env_float("KRUNKNATIVE_HTML_OBLIQUE_SKEW", TEXT_DEFAULT_OBLIQUE_SKEW);
    ts->fake_bold_advance = env_int("KRUNKNATIVE_HTML_FAKE_BOLD_ADVANCE", 0);
    text_gamma_params(&ts->gamma, &ts->contrast);

    text_build_liga_index(ts);
    text_add_fallbacks(ts);
    ts->generation = 1;
    return ts;
}

static void text_font_free(TextFont *font) {
    free(font->adv_keys);
    free(font->adv_vals);
    free(font);
}

void text_destroy(TextSystem *ts) {
    if (!ts) return;
    for (int i = 0; i < ts->font_count; i++) text_font_free(ts->fonts[i]);
    free(ts->fonts);
    for (int i = 0; i < ts->face_count; i++) {
        if (ts->faces[i].ft) FT_Done_Face(ts->faces[i].ft);
        free(ts->faces[i].path);
    }
    if (ts->tex) glDeleteTextures(1, &ts->tex);
    free(ts->entries);
    free(ts->scratch);
    FT_Done_FreeType(ts->ft);
    free(ts->ui_dir);
    free(ts);
}

int text_hinting(const TextSystem *ts) { return ts->hinting; }
int text_subpixel(const TextSystem *ts) { return ts->subpixel; }
const char *text_face_family(const TextFace *face) { return face->family; }
int text_face_is_icon_font(const TextFace *face) { return face->is_icon; }

int text_register_face(TextSystem *ts, const char *family, const char *path, int weight, int style) {
    if (!ts || !family || !path) return -1;
    char full[1024];
    if (is_abs_path(path)) snprintf(full, sizeof full, "%s", path);
    else snprintf(full, sizeof full, "%s/%s", ts->ui_dir, path);

    // .woff2 -> .ttf (the import tool converts; no brotli at runtime)
    size_t n = strlen(full);
    if (n > 6 && !strcmp(full + n - 6, ".woff2")) {
        char alt[1024];
        snprintf(alt, sizeof alt, "%.*s.ttf", (int) (n - 6), full);
        if (file_exists(alt)) snprintf(full, sizeof full, "%s", alt);
        else {
            // Material Icons: the google file name is opaque, use the canonical ttf
            char dir[sizeof full - 32];
            snprintf(dir, sizeof dir, "%.*s", (int) sizeof dir - 1, full);
            char *slash = strrchr(dir, '/');
            if (slash) *slash = 0; else snprintf(dir, sizeof dir, ".");
            char lower[TEXT_FAMILY_MAX];
            str_lower_copy(lower, sizeof lower, family, strlen(family));
            if (!strncmp(lower, "material icons", 14)) {
                snprintf(alt, sizeof alt, "%s/material-icons.ttf", dir);
                if (file_exists(alt)) snprintf(full, sizeof full, "%s", alt);
            }
        }
    }
    return text_add_face(ts, family, full, weight, style, 0) ? 0 : -1;
}

// ---- face loading ------------------------------------------------------------

static int face_load(TextSystem *ts, TextFace *f) {
    if (f->ft) return 1;
    if (f->load_failed) return 0;
    if (FT_New_Face(ts->ft, f->path, 0, &f->ft)) {
        fprintf(stderr, "text: failed to load face '%s' from %s\n", f->family, f->path);
        f->load_failed = 1;
        f->ft = NULL;
        return 0;
    }
    f->has_kerning = FT_HAS_KERNING(f->ft) ? 1 : 0;
    f->cur_size64 = -1;
    if (!f->is_fallback && !f->is_icon && f->ft->family_name &&
        !strncmp(f->ft->family_name, "Material Icons", 14)) f->is_icon = 1;
    return 1;
}

static int face_set_size(TextFace *f, long size64) {
    if (!f->ft) return 0;
    if (f->cur_size64 == size64) return 1;
    if (FT_Set_Char_Size(f->ft, 0, size64, 72, 72)) return 0;
    f->cur_size64 = size64;
    return 1;
}

static FT_Int32 text_load_flags(const TextSystem *ts) {
    FT_Int32 flags = FT_LOAD_NO_BITMAP;
    switch (ts->hinting) {
    case TEXT_HINT_SLIGHT: flags |= FT_LOAD_TARGET_LIGHT; break;
    case TEXT_HINT_FULL: flags |= FT_LOAD_TARGET_NORMAL; break;
    default: flags |= FT_LOAD_NO_HINTING | FT_LOAD_TARGET_NORMAL; break;
    }
    if (ts->force_autohint && ts->hinting != TEXT_HINT_NONE) flags |= FT_LOAD_FORCE_AUTOHINT;
    return flags;
}

// ---- font matching -----------------------------------------------------------

static int is_generic_family(const char *lower) {
    static const char *const generics[] = {
        "serif", "sans-serif", "monospace", "cursive", "fantasy", "system-ui",
        "ui-sans-serif", "ui-serif", "ui-monospace", "ui-rounded", "math",
        "emoji", "fangsong", "inherit", "initial", "unset", "arial", "helvetica",
        "verdana", "tahoma", "roboto", "segoe ui", "sans",
    };
    for (size_t i = 0; i < sizeof generics / sizeof *generics; i++)
        if (!strcmp(lower, generics[i])) return 1;
    return 0;
}

// Simplified CSS font matching inside one family: style first, then nearest
// weight (for desired <= 500 prefer lighter candidates, else heavier).
static TextFace *text_match_family(TextSystem *ts, const char *lower, int weight, int italic) {
    TextFace *best = NULL;
    int best_score = 0x7fffffff;
    for (int i = 0; i < ts->face_count; i++) {
        TextFace *f = &ts->faces[i];
        if (f->is_fallback || strcmp(f->family, lower)) continue;
        int style_pen = ((f->style == TEXT_STYLE_ITALIC) != (italic != 0)) ? 10000 : 0;
        int d = f->weight - weight;
        int w_pen = d < 0 ? -d : d;
        if (weight <= 500 && d > 0) w_pen += 500;
        if (weight > 500 && d < 0) w_pen += 500;
        int score = style_pen + w_pen;
        if (score < best_score) { best_score = score; best = f; }
    }
    return best;
}

static TextFace *text_primary_fallback(TextSystem *ts) {
    for (int i = 0; i < ts->fallback_count; i++) {
        if (!strcmp(ts->fallback[i]->family, "fallback-symbols") || !strcmp(ts->fallback[i]->family, "fallback-mono"))
            continue; /* per-glyph / `monospace` only */
        if (face_load(ts, ts->fallback[i])) return ts->fallback[i];
    }
    return NULL;
}

static TextFace *text_mono_fallback(TextSystem *ts) {
    for (int i = 0; i < ts->fallback_count; i++)
        if (!strcmp(ts->fallback[i]->family, "fallback-mono") && face_load(ts, ts->fallback[i])) return ts->fallback[i];
    return NULL;
}

static TextFace *text_resolve_family_list(TextSystem *ts, const char *list, int weight, int italic) {
    const char *p = list ? list : "";
    while (*p) {
        while (*p == ' ' || *p == ',' || *p == '\t') p++;
        const char *start = p;
        while (*p && *p != ',') p++;
        const char *end = p;
        while (end > start && (end[-1] == ' ' || end[-1] == '\t')) end--;
        if (end > start && (*start == '"' || *start == '\'')) { start++; if (end > start && (end[-1] == '"' || end[-1] == '\'')) end--; }
        if (end > start) {
            char lower[TEXT_FAMILY_MAX];
            str_lower_copy(lower, sizeof lower, start, (size_t) (end - start));
            if (is_generic_family(lower)) {
                TextFace *fb = !strcmp(lower, "monospace") ? text_mono_fallback(ts) : NULL;
                if (!fb) fb = text_primary_fallback(ts);
                if (fb) return fb;
                continue;
            }
            TextFace *f = text_match_family(ts, lower, weight, italic);
            if (f && face_load(ts, f)) return f;
        }
    }
    return text_primary_fallback(ts);
}

// ---- metrics -----------------------------------------------------------------

/* Top of the glyph's outline box at the current size, loaded with the
 * rendering flags like Skia's SkScalerContext_FreeType (fXHeight / fCapHeight
 * without an OS/2 v2 value): under slight hinting the height is grid-fitted,
 * which is what vertical-align: middle (x-height / 2) sees in Chrome. */
static float glyph_height_px(FT_Face ft, uint32_t cp, FT_Int32 flags) {
    FT_UInt gi = FT_Get_Char_Index(ft, cp);
    if (!gi) return 0;
    if (FT_Load_Glyph(ft, gi, flags)) return 0;
    if (ft->glyph->format == FT_GLYPH_FORMAT_OUTLINE) {
        FT_BBox bbox;
        FT_Outline_Get_CBox(&ft->glyph->outline, &bbox);
        return (float) bbox.yMax / 64.0f;
    }
    return (float) ft->glyph->metrics.horiBearingY / 64.0f;
}

static void text_compute_metrics(TextFont *font) {
    FT_Face ft = font->face->ft;
    TextMetrics *m = &font->metrics;
    float upem = (float) ft->units_per_EM;
    float scale = font->size / upem;
    m->size = font->size;
    m->units_per_em = upem;
    m->ascent_raw = (float) ft->ascender * scale;
    m->descent_raw = -(float) ft->descender * scale;
    m->line_gap = (float) (ft->height - (ft->ascender - ft->descender)) * scale;
    m->ascent = round_half_up(m->ascent_raw);
    m->descent = round_half_up(m->descent_raw);
    m->line_height = (float) (lroundf(m->ascent) + lroundf(m->descent) + lroundf(m->line_gap));

    TT_OS2 *os2 = FT_Get_Sfnt_Table(ft, FT_SFNT_OS2);
    m->x_height = 0;
    m->cap_height = 0;
    if (os2 && os2->version != 0xFFFF) {
        if (os2->version >= 2) {
            if (os2->sxHeight) m->x_height = (float) os2->sxHeight * scale;
            if (os2->sCapHeight) m->cap_height = (float) os2->sCapHeight * scale;
        }
    }
    face_set_size(font->face, font->size64);
    const FT_Int32 flags = text_load_flags(font->ts);
    if (m->x_height <= 0) m->x_height = glyph_height_px(ft, 'x', flags);
    if (m->cap_height <= 0) m->cap_height = glyph_height_px(ft, 'H', flags);
}

TextFont *text_font(TextSystem *ts, const char *family_list, float size_px, int weight, int italic) {
    if (!ts) return NULL;
    if (size_px <= 0) size_px = 16;
    if (weight <= 0) weight = 400;
    TextFace *face = text_resolve_family_list(ts, family_list, weight, italic);
    if (!face) return NULL;
    unsigned synth = 0;
    if (weight >= 600 && face->weight < 600) synth |= TEXT_SYNTH_BOLD;
    if (italic && face->style != TEXT_STYLE_ITALIC) synth |= TEXT_SYNTH_OBLIQUE;
    long size64 = lroundf(size_px * 64.0f);
    if (size64 < 64) size64 = 64;

    for (int i = 0; i < ts->font_count; i++) {
        TextFont *f = ts->fonts[i];
        if (f->face == face && f->size64 == size64 && f->synth == synth) return f;
    }
    TextFont *font = calloc(1, sizeof *font);
    if (!font) return NULL;
    font->ts = ts;
    font->face = face;
    font->size = (float) size64 / 64.0f;
    font->size64 = size64;
    font->synth = synth;
    text_compute_metrics(font);
    if (ts->font_count == ts->font_cap) {
        int cap = ts->font_cap ? ts->font_cap * 2 : 16;
        TextFont **n = realloc(ts->fonts, (size_t) cap * sizeof *n);
        if (!n) { free(font); return NULL; }
        ts->fonts = n;
        ts->font_cap = cap;
    }
    ts->fonts[ts->font_count++] = font;
    return font;
}

const TextMetrics *text_font_metrics(const TextFont *font) { return &font->metrics; }

int text_font_char_widths(TextFont *font, float *avg, float *max) {
    if (!font || !font->face) return 0;
    TextFace *f = font->face;
    if (!face_load(font->ts, f) || !f->ft || !f->ft->units_per_EM) return 0;
    const float scale = font->size / (float) f->ft->units_per_EM;
    const TT_OS2 *os2 = (const TT_OS2 *) FT_Get_Sfnt_Table(f->ft, FT_SFNT_OS2);
    float a = (os2 && os2->version != 0xFFFFu && os2->xAvgCharWidth > 0) ? (float) os2->xAvgCharWidth * scale : 0.0f;
    if (a <= 0.0f) a = font->metrics.x_height;
    const float m = (float) f->ft->bbox.xMax * scale;
    if (avg) *avg = roundf(a);
    if (max) *max = roundf(m > 0.0f ? m : a);
    return 1;
}
TextFace *text_font_face(const TextFont *font) { return font->face; }
unsigned text_font_synth(const TextFont *font) { return font->synth; }
float text_font_size(const TextFont *font) { return font->size; }

// ---- advances ----------------------------------------------------------------

static int adv_lookup(const TextFont *font, uint64_t key, float *out) {
    if (!font->adv_cap) return 0;
    unsigned mask = (unsigned) font->adv_cap - 1;
    unsigned h = (unsigned) (key * 0x9E3779B97F4A7C15ull >> 32) & mask;
    for (;;) {
        uint64_t k = font->adv_keys[h];
        if (!k) return 0;
        if (k == key) { *out = font->adv_vals[h]; return 1; }
        h = (h + 1) & mask;
    }
}

static void adv_insert(TextFont *font, uint64_t key, float val) {
    if ((font->adv_count + 1) * 10 > font->adv_cap * 7) {
        int cap = font->adv_cap ? font->adv_cap * 2 : 64;
        uint64_t *keys = calloc((size_t) cap, sizeof *keys);
        float *vals = calloc((size_t) cap, sizeof *vals);
        if (!keys || !vals) { free(keys); free(vals); return; }
        for (int i = 0; i < font->adv_cap; i++) {
            if (!font->adv_keys[i]) continue;
            unsigned h = (unsigned) (font->adv_keys[i] * 0x9E3779B97F4A7C15ull >> 32) & (unsigned) (cap - 1);
            while (keys[h]) h = (h + 1) & (unsigned) (cap - 1);
            keys[h] = font->adv_keys[i];
            vals[h] = font->adv_vals[i];
        }
        free(font->adv_keys);
        free(font->adv_vals);
        font->adv_keys = keys;
        font->adv_vals = vals;
        font->adv_cap = cap;
    }
    unsigned mask = (unsigned) font->adv_cap - 1;
    unsigned h = (unsigned) (key * 0x9E3779B97F4A7C15ull >> 32) & mask;
    while (font->adv_keys[h]) h = (h + 1) & mask;
    font->adv_keys[h] = key;
    font->adv_vals[h] = val;
    font->adv_count++;
}

static float text_glyph_advance(TextFont *font, TextFace *face, uint32_t gi) {
    uint64_t key = (1ull << 32) | ((uint64_t) face->id << 24) | (gi & 0xFFFFFF);
    float adv;
    if (adv_lookup(font, key, &adv)) return adv;
    adv = 0;
    TextSystem *ts = font->ts;
    if (face_set_size(face, font->size64) && !FT_Load_Glyph(face->ft, gi, text_load_flags(ts))) {
        if (ts->linear_metrics) adv = (float) face->ft->glyph->linearHoriAdvance / 65536.0f;
        else adv = (float) face->ft->glyph->advance.x / 64.0f;
        if ((font->synth & TEXT_SYNTH_BOLD) && ts->fake_bold_advance) adv += font->size / ts->bold_divisor;
    }
    adv_insert(font, key, adv);
    return adv;
}

static float text_kerning(TextFont *font, TextFace *face, uint32_t prev, uint32_t cur) {
    if (!face->has_kerning || !prev || !cur) return 0;
    FT_Vector d;
    FT_UInt mode = font->ts->linear_metrics ? FT_KERNING_UNFITTED : FT_KERNING_DEFAULT;
    if (!face_set_size(face, font->size64)) return 0;
    if (FT_Get_Kerning(face->ft, prev, cur, mode, &d)) return 0;
    return (float) d.x / 64.0f;
}

// ---- shaping -----------------------------------------------------------------

static int is_skipped_cp(uint32_t cp) {
    return cp == 0x200E || cp == 0x200F || cp == 0x200B || cp == 0x200C || cp == 0x200D ||
           cp == 0xFEFF || cp == 0x00AD || cp == 0x2060 || (cp >= 0x202A && cp <= 0x202E) ||
           (cp >= 0x2066 && cp <= 0x2069);
}

// Longest Material Icons ligature name starting at s; returns its length or 0.
static int text_match_liga(const TextSystem *ts, const char *s, size_t len, const MaterialIconLiga **out) {
    unsigned char c = (unsigned char) s[0];
    int best_len = 0;
    for (int i = ts->liga_lo[c]; i < ts->liga_hi[c]; i++) {
        const char *name = material_icons_liga[i].name;
        size_t n = strlen(name);
        if (n > len || (int) n <= best_len) continue;
        if (!memcmp(name, s, n)) { best_len = (int) n; *out = &material_icons_liga[i]; }
    }
    return best_len;
}

static TextFace *text_find_glyph(TextSystem *ts, TextFace *primary, uint32_t cp, uint32_t *gi) {
    *gi = FT_Get_Char_Index(primary->ft, cp);
    if (*gi) return primary;
    // Blink shapes U+00A0 with the font's space glyph when the font lacks a
    // dedicated no-break space (gamefont does), instead of falling back to
    // another family; its advance must stay the primary font's space width.
    if (cp == 0x00A0) {
        *gi = FT_Get_Char_Index(primary->ft, 0x20);
        if (*gi) return primary;
    }
    for (int i = 0; i < ts->fallback_count; i++) {
        TextFace *fb = ts->fallback[i];
        if (fb == primary || !face_load(ts, fb)) continue;
        uint32_t g = FT_Get_Char_Index(fb->ft, cp);
        if (g) { *gi = g; return fb; }
    }
    *gi = 0;
    return primary;
}

int text_shape(TextFont *font, const char *utf8, size_t len, float letter_spacing,
               Glyph *out, int max, float *total_advance) {
    if (!font || !utf8) { if (total_advance) *total_advance = 0; return 0; }
    TextSystem *ts = font->ts;
    TextFace *primary = font->face;
    int count = 0;
    float pen = 0;
    uint32_t prev_gi = 0;
    TextFace *prev_face = NULL;
    int prev_out = -1;
    int hinted = !ts->linear_metrics;

    size_t i = 0;
    while (i < len) {
        size_t start = i;
        uint32_t cp;
        TextFace *face;
        uint32_t gi;

        const MaterialIconLiga *liga = NULL;
        int liga_len = primary->is_icon ? text_match_liga(ts, utf8 + i, len - i, &liga) : 0;
        if (liga_len) {
            i += (size_t) liga_len;
            face = primary;
            gi = liga->codepoint ? FT_Get_Char_Index(primary->ft, liga->codepoint) : 0;
            if (!gi) gi = liga->glyph_index;
        } else {
            cp = utf8_next(utf8, len, &i);
            if (is_skipped_cp(cp)) continue;
            if (cp == '\t' || cp == '\n' || cp == '\r' || cp == '\f') cp = ' ';
            face = text_find_glyph(ts, primary, cp, &gi);
        }

        // kerning adjusts the previous glyph's advance (HarfBuzz semantics)
        if (prev_face == face && prev_gi && gi) {
            float k = text_kerning(font, face, prev_gi, gi);
            if (k != 0) {
                pen += k;
                if (prev_out >= 0 && out) out[prev_out].advance += k;
            }
        }
        float adv = text_glyph_advance(font, face, gi);
        if (hinted) pen = round_half_up(pen);
        if (out && count < max) {
            Glyph *g = &out[count];
            g->glyph_index = gi;
            g->face = face;
            g->x = pen;
            g->advance = adv;
            g->phase = ts->subpixel ? text_phase(pen) : 0;
            g->synth = (uint8_t) font->synth;
            g->cluster = (uint16_t) (start > 0xFFFF ? 0xFFFF : start);
            prev_out = count;
        } else prev_out = -1;
        count++;
        pen += adv + letter_spacing;
        prev_gi = gi;
        prev_face = face;
    }
    if (total_advance) *total_advance = pen;
    return count;
}

float text_measure(TextFont *font, const char *utf8, size_t len, float letter_spacing) {
    float w = 0;
    text_shape(font, utf8, len, letter_spacing, NULL, 0, &w);
    return w;
}

// ---- text-transform ----------------------------------------------------------

size_t text_uppercase(const char *utf8, size_t len, char *out, size_t cap) {
    size_t i = 0, o = 0;
    while (i < len) {
        size_t before = i;
        uint32_t cp = utf8_next(utf8, len, &i);
        if (cp >= 'a' && cp <= 'z') cp -= 0x20;
        else if (cp >= 0xE0 && cp <= 0xFE && cp != 0xF7) cp -= 0x20;
        else if (cp == 0xFF) cp = 0x178;
        else if (cp == 0xDF) {
            if (o + 2 > cap) break;
            out[o++] = 'S';
            out[o++] = 'S';
            continue;
        } else if (cp == 0xFFFD) {
            // pass invalid bytes through untouched
            if (o + (i - before) > cap) break;
            memcpy(out + o, utf8 + before, i - before);
            o += i - before;
            continue;
        }
        size_t n = utf8_put(cp, out + o, cap - o);
        if (!n) break;
        o += n;
    }
    if (o < cap) out[o] = 0;
    return o;
}

// ---- line breaking -----------------------------------------------------------

enum {
    LB_AL, LB_NU, LB_SP, LB_ZW, LB_GL, LB_HY, LB_BA, LB_B2, LB_EX, LB_SY,
    LB_CL, LB_OP, LB_OPW, LB_QU, LB_NS, LB_ID
};

static int lb_class(uint32_t cp) {
    if (cp == ' ' || cp == '\t' || cp == 0x3000) return LB_SP;
    if (cp == 0x200B) return LB_ZW;
    if (cp == 0xA0 || cp == 0x202F || cp == 0x2007 || cp == 0x2011) return LB_GL;
    if (cp == '-') return LB_HY;
    if (cp == 0x2013 || cp == 0xAD) return LB_BA;
    if (cp == 0x2014) return LB_B2;
    if (cp == '!' || cp == '?') return LB_EX;
    if (cp == '/') return LB_SY;
    if (cp == ')' || cp == ']' || cp == '}') return LB_CL;
    if (cp == '(' || cp == '[' || cp == '{') return LB_OP;
    if (cp == '"' || cp == '\'' || cp == 0x2018 || cp == 0x2019 || cp == 0x201C || cp == 0x201D) return LB_QU;
    if (cp >= '0' && cp <= '9') return LB_NU;
    if (cp < 0x2E80) return LB_AL;
    // CJK punctuation
    switch (cp) {
    case 0x3001: case 0x3002: case 0x3009: case 0x300B: case 0x300D: case 0x300F:
    case 0x3011: case 0x3015: case 0x3017: case 0x3019: case 0x301B: case 0x301E: case 0x301F:
    case 0xFF01: case 0xFF09: case 0xFF0C: case 0xFF0E: case 0xFF1A: case 0xFF1B:
    case 0xFF1F: case 0xFF3D: case 0xFF5D: case 0xFF60: case 0xFF63: case 0xFF64:
        return LB_CL;
    case 0x3008: case 0x300A: case 0x300C: case 0x300E: case 0x3010: case 0x3014:
    case 0x3016: case 0x3018: case 0x301A: case 0x301D:
    case 0xFF08: case 0xFF3B: case 0xFF5B: case 0xFF5F: case 0xFF62:
        return LB_OPW;
    case 0x3005: case 0x301C: case 0x30FB: case 0x30FC: case 0x30FD: case 0x30FE:
    case 0x3041: case 0x3043: case 0x3045: case 0x3047: case 0x3049: case 0x3063:
    case 0x3083: case 0x3085: case 0x3087: case 0x308E: case 0x3095: case 0x3096:
    case 0x30A1: case 0x30A3: case 0x30A5: case 0x30A7: case 0x30A9: case 0x30C3:
    case 0x30E3: case 0x30E5: case 0x30E7: case 0x30EE: case 0x30F5: case 0x30F6:
    case 0xFF70:
        return LB_NS;
    default: break;
    }
    if ((cp >= 0x2E80 && cp <= 0x2FFF) || (cp >= 0x3040 && cp <= 0x30FF) ||
        (cp >= 0x3100 && cp <= 0x31FF) || (cp >= 0x3300 && cp <= 0x4DBF) ||
        (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0xA000 && cp <= 0xA4CF) ||
        (cp >= 0xAC00 && cp <= 0xD7AF) || (cp >= 0xF900 && cp <= 0xFAFF) ||
        (cp >= 0xFE30 && cp <= 0xFE4F) || (cp >= 0xFF10 && cp <= 0xFF5A) ||
        (cp >= 0xFF66 && cp <= 0xFFDC) || (cp >= 0x1F000 && cp <= 0x1FAFF) ||
        (cp >= 0x20000 && cp <= 0x3FFFF))
        return LB_ID;
    return LB_AL;
}

static int lb_break_between(int prev, int cur) {
    if (cur == LB_SP || cur == LB_ZW) return 0;                         // LB7
    if (prev == LB_ZW) return 1;                                        // LB8
    if (prev == LB_GL || cur == LB_GL) return 0;                        // LB12
    if (cur == LB_CL || cur == LB_EX || cur == LB_SY || cur == LB_NS) return 0; // LB13, LB16, LB21
    if (prev == LB_OP || prev == LB_OPW) return 0;                      // LB14
    if (cur == LB_QU || prev == LB_QU) return 0;                        // LB19
    if (cur == LB_HY || cur == LB_BA) return 0;                         // LB21
    if (prev == LB_SP) return 1;                                        // LB18
    if (prev == LB_HY || prev == LB_BA) return cur != LB_NU;            // LB21/LB25
    if (prev == LB_B2 || cur == LB_B2) return 1;                        // LB17 (no B2 run handling)
    if (prev == LB_SY) return cur != LB_NU;                             // LB25
    if (prev == LB_CL || prev == LB_EX) return 1;                       // LB31
    if (cur == LB_OP) return !(prev == LB_AL || prev == LB_NU);         // LB30
    if (cur == LB_OPW) return 1;
    if (cur == LB_ID || prev == LB_ID) return 1;                        // ID ÷
    return 0;                                                           // AL/NU runs
}

void text_break_opportunities(const char *utf8, size_t len, uint8_t *flags) {
    if (!flags) return;
    memset(flags, 0, len);
    int prev = -1;
    size_t i = 0;
    while (i < len) {
        size_t at = i;
        uint32_t cp = utf8_next(utf8, len, &i);
        int cls = lb_class(cp);
        uint8_t f = 0;
        if (cls == LB_SP) f |= TEXT_BREAK_SPACE;
        if (cls == LB_ZW || is_skipped_cp(cp)) f |= TEXT_BREAK_ZERO_WIDTH;
        if (prev >= 0 && lb_break_between(prev, cls)) f |= TEXT_BREAK_ALLOWED;
        flags[at] = f;
        if (!(cls == LB_ZW || cls == LB_SP) && is_skipped_cp(cp)) continue; // transparent
        prev = cls;
    }
}

// ---- atlas -------------------------------------------------------------------

static int atlas_create(TextSystem *ts) {
    if (ts->tex) return 1;
    GLint prev_tex = 0, prev_align = 4, prev_unpack = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &prev_align);
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &prev_unpack);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glGenTextures(1, &ts->tex);
    glBindTexture(GL_TEXTURE_2D, ts->tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    uint8_t *zero = calloc((size_t) TEXT_ATLAS_SIZE * TEXT_ATLAS_SIZE, 1);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, TEXT_ATLAS_SIZE, TEXT_ATLAS_SIZE, 0, GL_RED, GL_UNSIGNED_BYTE, zero);
    free(zero);
    glPixelStorei(GL_UNPACK_ALIGNMENT, prev_align);
    glBindTexture(GL_TEXTURE_2D, (GLuint) prev_tex);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, (GLuint) prev_unpack);
    ts->shelf_y = 0;
    ts->shelf_h = 0;
    ts->shelf_x = 0;
    return ts->tex != 0;
}

static void atlas_reset(TextSystem *ts) {
    if (ts->entries) memset(ts->entries, 0, (size_t) ts->entry_cap * sizeof *ts->entries);
    ts->entry_count = 0;
    ts->shelf_y = 0;
    ts->shelf_h = 0;
    ts->shelf_x = 0;
    ts->generation++;
}

static AtlasEntry *atlas_find(TextSystem *ts, uint64_t key) {
    if (!ts->entry_cap) return NULL;
    unsigned mask = (unsigned) ts->entry_cap - 1;
    unsigned h = (unsigned) (key * 0x9E3779B97F4A7C15ull >> 32) & mask;
    for (;;) {
        AtlasEntry *e = &ts->entries[h];
        if (!e->key) return NULL;
        if (e->key == key) return e;
        h = (h + 1) & mask;
    }
}

static AtlasEntry *atlas_insert(TextSystem *ts, uint64_t key) {
    if ((ts->entry_count + 1) * 10 > ts->entry_cap * 7) {
        int cap = ts->entry_cap ? ts->entry_cap * 2 : 1024;
        AtlasEntry *n = calloc((size_t) cap, sizeof *n);
        if (!n) return NULL;
        for (int i = 0; i < ts->entry_cap; i++) {
            if (!ts->entries[i].key) continue;
            unsigned h = (unsigned) (ts->entries[i].key * 0x9E3779B97F4A7C15ull >> 32) & (unsigned) (cap - 1);
            while (n[h].key) h = (h + 1) & (unsigned) (cap - 1);
            n[h] = ts->entries[i];
        }
        free(ts->entries);
        ts->entries = n;
        ts->entry_cap = cap;
    }
    unsigned mask = (unsigned) ts->entry_cap - 1;
    unsigned h = (unsigned) (key * 0x9E3779B97F4A7C15ull >> 32) & mask;
    while (ts->entries[h].key) h = (h + 1) & mask;
    ts->entries[h].key = key;
    ts->entry_count++;
    return &ts->entries[h];
}

// Shelf packer: returns 1 and the padded rect origin, 0 when full.
static int atlas_alloc(TextSystem *ts, int pw, int ph, int *x, int *y) {
    if (pw > TEXT_ATLAS_SIZE || ph > TEXT_ATLAS_SIZE) return 0;
    if (ts->shelf_h && ph <= ts->shelf_h && ts->shelf_x + pw <= TEXT_ATLAS_SIZE) {
        *x = ts->shelf_x;
        *y = ts->shelf_y;
        ts->shelf_x += pw;
        return 1;
    }
    int ny = ts->shelf_y + ts->shelf_h;
    int nh = (ph + 3) & ~3;
    if (ny + nh > TEXT_ATLAS_SIZE) return 0;
    if ((float) (ny + nh) / (float) TEXT_ATLAS_SIZE > TEXT_ATLAS_REBUILD_FILL) return 0;
    ts->shelf_y = ny;
    ts->shelf_h = nh;
    ts->shelf_x = pw;
    *x = 0;
    *y = ny;
    return 1;
}

static uint64_t glyph_key(const TextFace *face, uint32_t gi, long size64, unsigned synth, unsigned phase) {
    return (1ull << 63) | ((uint64_t) (size64 & 0xFFFFFF) << 38) | ((uint64_t) (phase & 3) << 36) |
           ((uint64_t) (synth & 3) << 34) | ((uint64_t) (face->id & 0x3F) << 28) | (gi & 0x0FFFFFFF);
}

static void fill_quad(const TextSystem *ts, const AtlasEntry *e, GlyphQuad *out) {
    const float inv = 1.0f / (float) TEXT_ATLAS_SIZE;
    out->texture = ts->tex;
    out->u0 = (float) e->x * inv;
    out->v0 = (float) e->y * inv;
    out->u1 = (float) (e->x + e->w) * inv;
    out->v1 = (float) (e->y + e->h) * inv;
    out->x_off = (float) e->left;
    out->y_off = -(float) e->top;
    out->w = (float) e->w;
    out->h = (float) e->h;
    out->generation = ts->generation;
}

int text_glyph_quad(TextSystem *ts, const Glyph *g, float device_size, unsigned synth, GlyphQuad *out) {
    memset(out, 0, sizeof *out);
    if (!ts || !g || !g->face || device_size <= 0) return 0;
    if (!atlas_create(ts)) return 0;
    TextFace *face = g->face;
    if (!face_load(ts, face)) return 0;
    long size64 = lroundf(device_size * 64.0f);
    if (size64 < 64) size64 = 64;
    unsigned phase = ts->subpixel ? (g->phase & 3) : 0;
    uint64_t key = glyph_key(face, g->glyph_index, size64, synth, phase);
    ts->tick++;

    AtlasEntry *e = atlas_find(ts, key);
    if (e) {
        e->last_used = ts->tick;
        if (!e->w || !e->h) return 0;
        fill_quad(ts, e, out);
        return 1;
    }

    if (!face_set_size(face, size64)) return 0;
    if (FT_Load_Glyph(face->ft, g->glyph_index, text_load_flags(ts))) return 0;
    FT_GlyphSlot slot = face->ft->glyph;
    if (slot->format != FT_GLYPH_FORMAT_OUTLINE) return 0;
    if (synth & TEXT_SYNTH_BOLD) {
        FT_Pos strength = (FT_Pos) lroundf((float) size64 / ts->bold_divisor);
        FT_Outline_EmboldenXY(&slot->outline, strength, strength);
    }
    if (synth & TEXT_SYNTH_OBLIQUE) {
        FT_Matrix m = {0x10000, (FT_Fixed) (ts->oblique_skew * 65536.0f), 0, 0x10000};
        FT_Outline_Transform(&slot->outline, &m);
    }
    if (phase) FT_Outline_Translate(&slot->outline, (FT_Pos) phase * 16, 0);
    if (FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL)) return 0;

    FT_Bitmap *bm = &slot->bitmap;
    int w = (int) bm->width, h = (int) bm->rows;
    if (w <= 0 || h <= 0) {
        AtlasEntry *n = atlas_insert(ts, key);
        if (n) { n->w = 0; n->h = 0; n->last_used = ts->tick; }
        return 0;
    }
    if (bm->pixel_mode != FT_PIXEL_MODE_GRAY) return 0;

    int pw = w + 2 * TEXT_ATLAS_PAD, ph = h + 2 * TEXT_ATLAS_PAD;
    int x, y;
    if (!atlas_alloc(ts, pw, ph, &x, &y)) {
        atlas_reset(ts);
        if (!atlas_alloc(ts, pw, ph, &x, &y)) return 0;
    }

    size_t need = (size_t) pw * (size_t) ph;
    if (ts->scratch_cap < need) {
        uint8_t *s = realloc(ts->scratch, need);
        if (!s) return 0;
        ts->scratch = s;
        ts->scratch_cap = need;
    }
    memset(ts->scratch, 0, need);
    for (int r = 0; r < h; r++)
        memcpy(ts->scratch + (size_t) (r + TEXT_ATLAS_PAD) * (size_t) pw + TEXT_ATLAS_PAD,
               bm->buffer + (ptrdiff_t) r * bm->pitch, (size_t) w);

    GLint prev_tex = 0, prev_align = 4, prev_unpack = 0, prev_active = GL_TEXTURE0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_active);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &prev_align);
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &prev_unpack);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, ts->tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, pw, ph, GL_RED, GL_UNSIGNED_BYTE, ts->scratch);
    glPixelStorei(GL_UNPACK_ALIGNMENT, prev_align);
    glBindTexture(GL_TEXTURE_2D, (GLuint) prev_tex);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, (GLuint) prev_unpack);
    glActiveTexture((GLenum) prev_active);

    AtlasEntry *n = atlas_insert(ts, key);
    if (!n) return 0;
    n->x = (uint16_t) (x + TEXT_ATLAS_PAD);
    n->y = (uint16_t) (y + TEXT_ATLAS_PAD);
    n->w = (uint16_t) w;
    n->h = (uint16_t) h;
    n->left = (int16_t) slot->bitmap_left;
    n->top = (int16_t) slot->bitmap_top;
    n->last_used = ts->tick;
    fill_quad(ts, n, out);
    return 1;
}

uint32_t text_atlas_texture(const TextSystem *ts) { return ts ? ts->tex : 0; }
uint32_t text_atlas_generation(const TextSystem *ts) { return ts ? ts->generation : 0; }
int text_atlas_size(const TextSystem *ts) { (void) ts; return TEXT_ATLAS_SIZE; }
float text_atlas_fill(const TextSystem *ts) {
    return ts ? (float) (ts->shelf_y + ts->shelf_h) / (float) TEXT_ATLAS_SIZE : 0;
}

int text_atlas_dump_pgm(TextSystem *ts, const char *path) {
#ifdef KN_GLES // no glGetTexImage in OpenGL ES
    (void) ts; (void) path;
    return -1;
#else
    if (!ts || !ts->tex) return -1;
    size_t n = (size_t) TEXT_ATLAS_SIZE * TEXT_ATLAS_SIZE;
    uint8_t *buf = malloc(n);
    if (!buf) return -1;
    GLint prev_tex = 0, prev_pack = 4;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
    glGetIntegerv(GL_PACK_ALIGNMENT, &prev_pack);
    glBindTexture(GL_TEXTURE_2D, ts->tex);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RED, GL_UNSIGNED_BYTE, buf);
    glPixelStorei(GL_PACK_ALIGNMENT, prev_pack);
    glBindTexture(GL_TEXTURE_2D, (GLuint) prev_tex);
    FILE *f = fopen(path, "wb");
    if (!f) { free(buf); return -1; }
    fprintf(f, "P5\n%d %d\n255\n", TEXT_ATLAS_SIZE, TEXT_ATLAS_SIZE);
    fwrite(buf, 1, n, f);
    fclose(f);
    free(buf);
    return 0;
#endif
}

// ---- gamma -------------------------------------------------------------------
//
// Port of Skia's SkTMaskGamma_build_correcting_lut (src/core/SkMaskGamma.cpp)
// with device gamma `gamma` (0 = sRGB curve) used for both the source colour
// and the destination guess, as in current Skia (paint gamma was removed):
//
//   src     = bucket luminance / 255            (bucket i -> i*255/7)
//   linSrc  = toLuma(src)                       (x^gamma or sRGB->linear)
//   dst     = 1 - src                           (guess at the background)
//   linDst  = toLuma(dst)
//   contrast' = contrast * linDst               (tapers off as src -> white)
//   for coverage a in 0..255:
//     srca   = a/255 + (1 - a/255) * contrast' * a/255
//     linOut = linSrc * srca + (1 - srca) * linDst
//     out    = fromLuma(linOut)
//     table[a] = round(255 * (out - dst) / (src - dst))   (undo the blend)
//   when |src - dst| < 1/256 the table is just the contrast-boosted srca.
//
// The bucket for a text colour is Skia's computeLuminance: Rec.709 weights on
// linearised rgb, back through fromLuma, *255, >> 5.

static float lum_to_linear(float gamma, float x) {
    if (gamma <= 0) return x <= 0.04045f ? x / 12.92f : powf((x + 0.055f) / 1.055f, 2.4f);
    return powf(x, gamma);
}

static float lum_from_linear(float gamma, float y) {
    if (gamma <= 0) return y <= 0.0031308f ? y * 12.92f : 1.055f * powf(y, 1.0f / 2.4f) - 0.055f;
    return powf(y, 1.0f / gamma);
}

static uint8_t to_u8(float v) {
    int i = (int) floorf(v * 255.0f + 0.5f);
    return (uint8_t) (i < 0 ? 0 : i > 255 ? 255 : i);
}

void text_gamma_lut_ex(uint8_t out[8][256], float gamma, float contrast) {
    for (int bucket = 0; bucket < 8; bucket++) {
        int src_i = bucket * 255 / 7;
        float src = (float) src_i / 255.0f;
        float lin_src = lum_to_linear(gamma, src);
        float dst = 1.0f - src;
        float lin_dst = lum_to_linear(gamma, dst);
        float adjusted = contrast * lin_dst;
        if (fabsf(src - dst) < 1.0f / 256.0f) {
            for (int a = 0; a < 256; a++) {
                float raw = (float) a / 255.0f;
                float srca = raw + (1.0f - raw) * adjusted * raw;
                out[bucket][a] = to_u8(srca);
            }
        } else {
            for (int a = 0; a < 256; a++) {
                float raw = (float) a / 255.0f;
                float srca = raw + (1.0f - raw) * adjusted * raw;
                float dsta = 1.0f - srca;
                float lin_out = lin_src * srca + dsta * lin_dst;
                float o = lum_from_linear(gamma, lin_out);
                float result = (o - dst) / (src - dst);
                out[bucket][a] = to_u8(result);
            }
        }
        out[bucket][0] = 0;
        out[bucket][255] = 255;
    }
}

void text_gamma_params(float *gamma, float *contrast) {
    *gamma = TEXT_DEFAULT_GAMMA;
    *contrast = TEXT_DEFAULT_CONTRAST;
    const char *g = getenv("KRUNKNATIVE_HTML_TEXT_GAMMA");
    if (g && *g) {
        float a = 0, b = 0;
        int n = sscanf(g, "%f:%f", &a, &b);
        if (n >= 1) *gamma = a;
        if (n >= 2) *contrast = b;
    }
}

void text_gamma_lut(uint8_t out[8][256]) {
    float gamma, contrast;
    text_gamma_params(&gamma, &contrast);
    text_gamma_lut_ex(out, gamma, contrast);
}

int text_gamma_bucket(uint8_t r, uint8_t g, uint8_t b, float gamma) {
    float lr = lum_to_linear(gamma, (float) r / 255.0f);
    float lg = lum_to_linear(gamma, (float) g / 255.0f);
    float lb = lum_to_linear(gamma, (float) b / 255.0f);
    float luma = lum_from_linear(gamma, 0.2126f * lr + 0.7152f * lg + 0.0722f * lb);
    int l = (int) floorf(luma * 255.0f + 0.5f);
    if (l < 0) l = 0;
    if (l > 255) l = 255;
    return l >> 5;
}
