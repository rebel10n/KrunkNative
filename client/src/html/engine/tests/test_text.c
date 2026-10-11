// Standalone test for client/src/html/engine/text.c.
//
// Build (throwaway CMake project, see the comment block at the end) and run:
//   LIBGL_ALWAYS_SOFTWARE=1 xvfb-run -a ./test_text /path/to/KrunkNative/ui
// Writes /tmp/kn_text_atlas.pgm (the glyph atlas) and /tmp/kn_text_strips.pgm
// (rendered sample lines) for visual inspection.

#include "text.h"
#include "material_icons_liga.h"

#include <gl.h>
#include <GLFW/glfw3.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0;
static int g_pass = 0;

#define CHECK(cond, ...) do { \
    if (cond) g_pass++; \
    else { g_fail++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

// Chrome reference (doc/ref/krunker-v3.1.1/menu/real-1280.png, headless
// Chromium 1280x720, #uiBase scale(0.753)): ink extents of text-driven boxes.
// #uiBase style="transform: scale(0.627); width: 2040px" in the DOM dump.
#define UI_SCALE 0.627f
typedef struct RefInk { const char *text; float css_size; float extra_scale; int ink_w, ink_h; const char *note; } RefInk;
static const RefInk g_ref[] = {
    {"Profile", 16, 1, 43, 13, ".menuItemTitle"},
    {"Settings", 16, 1, 54, 17, ".menuItemTitle"},
    {"Shop", 16, 1, 30, 17, ".menuItemTitle"},
    {"Challenges", 13, 1, 55, 13, "#menuBtnChall style=font-size:13px"},
    {"Triggerman", 30, 0.8f, 108, 25, "#menuClassName in #menuClassContainer scale(.8)"},
    {"CLICK TO PLAY", 32, 1, 187, 24, "#instructions (grow anim 30..32px)"},
};

// ---- strip compositor (CPU) --------------------------------------------------

#define STRIP_W 640
#define STRIP_LINE_H 48
#define STRIP_LINES 12
static uint8_t g_strips[STRIP_LINES * STRIP_LINE_H * STRIP_W];
static int g_strip_line = 0;
static uint8_t *g_atlas_pixels = NULL;

static void read_atlas(TextSystem *ts) {
    int n = text_atlas_size(ts);
    if (!g_atlas_pixels) g_atlas_pixels = malloc((size_t) n * (size_t) n);
    glBindTexture(GL_TEXTURE_2D, text_atlas_texture(ts));
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RED, GL_UNSIGNED_BYTE, g_atlas_pixels);
}

// Renders a run at device scale and returns its ink bbox (pixels with
// coverage > INK_THRESHOLD, matching the brightness threshold used to measure
// the Chrome screenshot); also draws it into the strip image when `draw` is set.
#define INK_THRESHOLD 105
static int render_run(TextSystem *ts, TextFont *font, const char *text, float scale, int draw,
                      int *ink_x0, int *ink_x1, int *ink_y0, int *ink_y1, float *advance_css) {
    Glyph glyphs[128];
    GlyphQuad quads[128];
    int px[128], py[128];
    float total = 0;
    int n = text_shape(font, text, strlen(text), 0, glyphs, 128, &total);
    if (n > 128) n = 128;
    if (advance_css) *advance_css = total;
    float device_size = text_font_size(font) * scale;
    unsigned synth = text_font_synth(font);
    int x0 = 1 << 30, x1 = -(1 << 30), y0 = 1 << 30, y1 = -(1 << 30);
    int baseline = g_strip_line * STRIP_LINE_H + 36;
    int count = 0;
    for (int i = 0; i < n; i++) {
        float dx = glyphs[i].x * scale;
        glyphs[i].phase = text_phase(dx);
        if (!text_glyph_quad(ts, &glyphs[i], device_size, synth, &quads[count])) continue;
        px[count] = (int) floorf(dx) + (int) quads[count].x_off;
        py[count] = (int) quads[count].y_off; // relative to baseline
        count++;
    }
    read_atlas(ts);
    int as = text_atlas_size(ts);
    for (int i = 0; i < count; i++) {
        const GlyphQuad *q = &quads[i];
        int ax = (int) lroundf(q->u0 * (float) as), ay = (int) lroundf(q->v0 * (float) as);
        for (int r = 0; r < (int) q->h; r++) {
            for (int c = 0; c < (int) q->w; c++) {
                uint8_t v = g_atlas_pixels[(size_t) (ay + r) * (size_t) as + (size_t) (ax + c)];
                if (v > INK_THRESHOLD) {
                    int gx = px[i] + c, gy = py[i] + r;
                    if (gx < x0) x0 = gx;
                    if (gx > x1) x1 = gx;
                    if (gy < y0) y0 = gy;
                    if (gy > y1) y1 = gy;
                }
                if (!draw) continue;
                int sy = baseline + py[i] + r, sx = 4 + px[i] + c;
                if (sy < 0 || sy >= STRIP_LINES * STRIP_LINE_H || sx < 0 || sx >= STRIP_W) continue;
                uint8_t *d = &g_strips[(size_t) sy * STRIP_W + (size_t) sx];
                if (v > *d) *d = v;
            }
        }
    }
    if (draw) g_strip_line++;
    if (!count || x1 < x0) { *ink_x0 = *ink_x1 = *ink_y0 = *ink_y1 = 0; return 0; }
    *ink_x0 = x0; *ink_x1 = x1; *ink_y0 = y0; *ink_y1 = y1;
    return count;
}

static void write_pgm(const char *path, const uint8_t *px, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P5\n%d %d\n255\n", w, h);
    fwrite(px, 1, (size_t) w * (size_t) h, f);
    fclose(f);
}

// ---- tests -------------------------------------------------------------------

static void test_fonts_and_metrics(TextSystem *ts) {
    printf("[fonts]\n");
    TextFont *game = text_font(ts, "gamefont", 16, 400, 0);
    TextFont *head = text_font(ts, "headerfont", 32, 400, 0);
    TextFont *icon = text_font(ts, "material icons", 24, 400, 0);
    CHECK(game && head && icon, "all three fonts resolve");
    if (!game || !head || !icon) return;
    CHECK(!strcmp(text_face_family(text_font_face(game)), "gamefont"), "gamefont face");
    CHECK(!strcmp(text_face_family(text_font_face(head)), "headerfont"), "headerfont face");
    CHECK(text_face_is_icon_font(text_font_face(icon)), "material icons flagged as icon font");
    CHECK(text_font(ts, "GameFont", 16, 400, 0) == game, "case-insensitive family match + cache");
    CHECK(text_font(ts, "nope, 'HeaderFont', serif", 32, 400, 0) == head, "family list skips unknown");
    TextFont *generic = text_font(ts, "sans-serif", 16, 400, 0);
    CHECK(generic && text_font_face(generic) != text_font_face(game), "generic family -> fallback face (%s)",
          generic ? text_face_family(text_font_face(generic)) : "none");

    const TextMetrics *m = text_font_metrics(game);
    printf("  gamefont 16px: ascent %.3f (raw %.3f) descent %.3f (raw %.3f) gap %.3f line-height %.1f x-height %.2f cap %.2f\n",
           m->ascent, m->ascent_raw, m->descent, m->descent_raw, m->line_gap, m->line_height, m->x_height, m->cap_height);
    // font2.ttf: hhea 1125/-250/0 at 1000 upem, no USE_TYPO_METRICS
    CHECK(m->ascent == 18 && m->descent == 4 && m->line_height == 22, "gamefont metrics from hhea (18/4/22)");
    m = text_font_metrics(head);
    printf("  headerfont 32px: ascent %.3f (raw %.3f) descent %.3f (raw %.3f) gap %.3f line-height %.1f x-height %.2f cap %.2f\n",
           m->ascent, m->ascent_raw, m->descent, m->descent_raw, m->line_gap, m->line_height, m->x_height, m->cap_height);
    // header.ttf: hhea 887/-151/9 -> 28.384/4.832/0.288 -> 28/5/33
    CHECK(m->ascent == 28 && m->descent == 5 && m->line_height == 33, "headerfont metrics from hhea (28/5/33)");
    m = text_font_metrics(icon);
    printf("  material icons 24px: ascent %.3f descent %.3f gap %.3f line-height %.1f\n", m->ascent, m->descent, m->line_gap, m->line_height);
    // Material Icons: USE_TYPO_METRICS set, typo 512/0/0 at 512 upem
    CHECK(m->ascent == 24 && m->descent == 0 && m->line_height == 24, "icon metrics from OS/2 typo (24/0/24)");
}

static void test_shaping(TextSystem *ts) {
    printf("[shaping]\n");
    TextFont *head = text_font(ts, "headerfont", 32, 400, 0);
    TextFont *game = text_font(ts, "gamefont", 32, 400, 0);
    TextFont *icon = text_font(ts, "material icons", 24, 400, 0);
    Glyph g[64];
    float total;

    int n = text_shape(head, "CLICK TO PLAY", 13, 0, g, 64, &total);
    CHECK(n == 13, "CLICK TO PLAY -> 13 glyphs (got %d)", n);
    CHECK(total > 100 && total < 400, "headerfont 32px CLICK TO PLAY advance %.2f", total);
    printf("  headerfont 32px 'CLICK TO PLAY' = %.3f px, gamefont 32px = %.3f px\n", total,
           text_measure(game, "CLICK TO PLAY", 13, 0));
    for (int i = 0; i < n; i++) CHECK(g[i].face == text_font_face(head) && g[i].glyph_index, "glyph %d from headerfont", i);
    CHECK(g[0].x == 0 && g[1].x > 0 && fabsf(g[1].x - g[0].advance) < 1e-4f, "pen positions accumulate");

    n = text_shape(game, "Triggerman", 10, 0, g, 64, &total);
    CHECK(n == 10, "Triggerman -> 10 glyphs");

    // letter-spacing is added after every glyph (CSS)
    float ls = text_measure(game, "ab", 2, 3.0f), nols = text_measure(game, "ab", 2, 0);
    CHECK(fabsf((ls - nols) - 6.0f) < 1e-3f, "letter-spacing 3px x 2 glyphs = +6 (got %.3f)", ls - nols);

    // Material Icons ligatures
    n = text_shape(icon, "delete_forever", 14, 0, g, 64, &total);
    CHECK(n == 1, "delete_forever -> 1 glyph (got %d)", n);
    CHECK(n == 1 && g[0].glyph_index == 1019, "delete_forever glyph 1019 (got %u)", n ? g[0].glyph_index : 0);
    n = text_shape(icon, "keyboard_arrow_down", 19, 0, g, 64, &total);
    CHECK(n == 1 && g[0].glyph_index == 395, "keyboard_arrow_down -> glyph 395 (n=%d gi=%u)", n, n ? g[0].glyph_index : 0);
    CHECK(fabsf(total - 24.0f) < 0.01f, "icon advance = 24px at 24px (got %.3f)", total);
    n = text_shape(icon, "person_add", 10, 0, g, 64, &total);
    CHECK(n == 1, "longest match person_add -> 1 glyph (got %d)", n);
    n = text_shape(icon, "personx", 7, 0, g, 64, &total);
    CHECK(n == 2, "person + x -> 2 glyphs (got %d)", n);
    n = text_shape(icon, "keyboard_arrow_down keyboard_arrow_up", 37, 0, g, 64, &total);
    CHECK(n == 3, "two icons + space -> 3 glyphs (got %d)", n);
    static const char *const used[] = {"delete_forever", "keyboard_arrow_down", "add_circle_outline", "clear",
        "signal_cellular_alt", "navigate_next", "colorize", "person", "email", "mic", "star", "keyboard_arrow_up",
        "remove_red_eye", "play_arrow", "grade", "check_circle", "keyboard_arrow_right"};
    for (size_t i = 0; i < sizeof used / sizeof *used; i++) {
        n = text_shape(icon, used[i], strlen(used[i]), 0, g, 64, &total);
        CHECK(n == 1 && g[0].glyph_index, "inventory ligature %s -> 1 glyph", used[i]);
    }

    // kerning: header.ttf has a kern table, L/T = -108/1000 em
    float l = text_measure(head, "L", 1, 0), t = text_measure(head, "T", 1, 0), lt = text_measure(head, "LT", 2, 0);
    printf("  headerfont kerning: L %.3f + T %.3f = %.3f, LT = %.3f (kern %.3f, expect %.3f)\n", l, t, l + t, lt, lt - l - t, -108 * 32 / 1000.0f);
    CHECK(fabsf((lt - l - t) - (-108 * 32 / 1000.0f)) < 0.05f || !text_subpixel(ts), "LT kerned by -3.456px");
    n = text_shape(head, "LT", 2, 0, g, 64, &total);
    CHECK(n == 2 && fabsf(g[0].advance - (lt - t)) < 0.05f, "kern applied to first glyph's advance (%.3f vs %.3f)", g[0].advance, lt - t);
    float hh = text_measure(head, "HH", 2, 0), h1 = text_measure(head, "H", 1, 0);
    CHECK(fabsf(hh - 2 * h1) < 1e-3f, "unkerned pair HH adds up");
    CHECK(fabsf(text_measure(game, "LT", 2, 0) - text_measure(game, "L", 1, 0) - text_measure(game, "T", 1, 0)) < 1e-3f,
          "gamefont has no kern table");

    // zero-width marks and fallback
    n = text_shape(game, "a\xE2\x80\x8E" "b\xE2\x80\x8F", 8, 0, g, 64, &total);
    CHECK(n == 2, "U+200E/U+200F skipped (got %d)", n);
    n = text_shape(game, "\xE6\x97\xA5\xE6\x9C\xAC", 6, 0, g, 64, &total);
    CHECK(n == 2, "CJK -> 2 glyphs");
    if (n == 2) {
        printf("  CJK fallback face: %s (glyph %u, advance %.2f)\n", text_face_family(g[0].face), g[0].glyph_index, g[0].advance);
        CHECK(g[0].face != text_font_face(game), "CJK glyph taken from a fallback face");
        CHECK(g[0].glyph_index != 0 || !strcmp(text_face_family(g[0].face), "gamefont"), "CJK glyph found (or no CJK fallback installed)");
    }
    n = text_shape(game, "\xC3\xA9", 2, 0, g, 64, &total);
    CHECK(n == 1 && g[0].face == text_font_face(game) && g[0].glyph_index, "e-acute present in gamefont");
    // U+2715 (dev console close button): the bundled symbol face, on every machine
    n = text_shape(game, "\xE2\x9C\x95", 3, 0, g, 64, &total);
    CHECK(n == 1 && g[0].glyph_index && !strcmp(text_face_family(g[0].face), "fallback-symbols"),
          "U+2715 from the bundled kn-symbols face (%s)", n == 1 ? text_face_family(g[0].face) : "-");
    // x-height without an OS/2 v2 value comes from the hinted 'x' outline (Skia's
    // fXHeight under slight hinting): whole px, as vertical-align: middle sees it
    // in Chrome (server list headers, win-servers)
    const TextMetrics *gm22 = text_font_metrics(text_font(ts, "gamefont", 22, 400, 0));
    CHECK(gm22 && gm22->x_height > 15 && fabsf(gm22->x_height - roundf(gm22->x_height)) < 1e-4f,
          "gamefont 22px x-height grid-fitted (%.3f)", gm22 ? gm22->x_height : -1);
    TextFont *mono = text_font(ts, "monospace", 13, 400, 0);
    const char *mono_family = mono ? text_face_family(text_font_face(mono)) : "none";
    CHECK(mono && strcmp(mono_family, "fallback-symbols") != 0, "monospace never resolves to the symbol face (%s)", mono_family);
    printf("  monospace -> %s, sans-serif -> %s\n", mono_family,
           text_face_family(text_font_face(text_font(ts, "sans-serif", 13, 400, 0))));
    // subpixel phases
    n = text_shape(head, "iiii", 4, 0, g, 64, &total);
    int phases = 0;
    for (int i = 0; i < n; i++) phases |= 1 << g[i].phase;
    printf("  headerfont 'iiii' phases mask %x (x: %.3f %.3f %.3f %.3f)\n", phases, g[0].x, g[1].x, g[2].x, g[3].x);
    CHECK(!text_subpixel(ts) || g[1].phase == text_phase(g[1].x), "phase matches pen x");
    CHECK(!text_subpixel(ts) || (phases & ~1), "fractional advances produce non-zero phases");
}

static void test_raster(TextSystem *ts) {
    printf("[raster]\n");
    TextFont *reg = text_font(ts, "gamefont", 32, 400, 0);
    TextFont *bold = text_font(ts, "gamefont", 32, 700, 0);
    TextFont *obl = text_font(ts, "gamefont", 32, 400, 1);
    TextFont *boldobl = text_font(ts, "gamefont", 32, 900, 1);
    CHECK(text_font_synth(reg) == 0, "regular has no synth");
    CHECK(text_font_synth(bold) == TEXT_SYNTH_BOLD, "700 -> synthetic bold");
    CHECK(text_font_synth(obl) == TEXT_SYNTH_OBLIQUE, "italic -> synthetic oblique");
    CHECK(text_font_synth(boldobl) == (TEXT_SYNTH_BOLD | TEXT_SYNTH_OBLIQUE), "900 italic -> both");

    int x0, x1, y0, y1;
    float adv_r, adv_b;
    render_run(ts, reg, "Bold vs Regular", 1, 1, &x0, &x1, &y0, &y1, &adv_r);
    int reg_w = x1 - x0 + 1;
    render_run(ts, bold, "Bold vs Regular", 1, 1, &x0, &x1, &y0, &y1, &adv_b);
    int bold_w = x1 - x0 + 1;
    printf("  regular ink %d px (advance %.2f), synthetic bold ink %d px (advance %.2f)\n", reg_w, adv_r, bold_w, adv_b);
    CHECK(bold_w > reg_w, "synthetic bold ink wider than regular");
    CHECK(fabsf(adv_b - adv_r) < 1e-3f || getenv("KRUNKNATIVE_HTML_FAKE_BOLD_ADVANCE"), "fake bold leaves advances unchanged (Skia)");
    render_run(ts, obl, "Oblique text", 1, 1, &x0, &x1, &y0, &y1, NULL);
    int obl_w = x1 - x0 + 1;
    render_run(ts, reg, "Oblique text", 1, 0, &x0, &x1, &y0, &y1, NULL);
    CHECK(obl_w > x1 - x0 + 1, "oblique ink wider (sheared)");

    // single glyph quads: regular vs bold width
    Glyph g[4];
    text_shape(reg, "T", 1, 0, g, 4, NULL);
    GlyphQuad qr, qb;
    CHECK(text_glyph_quad(ts, &g[0], 32, 0, &qr), "quad for T");
    CHECK(text_glyph_quad(ts, &g[0], 32, TEXT_SYNTH_BOLD, &qb), "bold quad for T");
    printf("  'T' 32px quad %gx%g off (%g,%g); bold %gx%g off (%g,%g); uv (%.4f,%.4f)-(%.4f,%.4f)\n",
           qr.w, qr.h, qr.x_off, qr.y_off, qb.w, qb.h, qb.x_off, qb.y_off, qr.u0, qr.v0, qr.u1, qr.v1);
    CHECK(qb.w > qr.w && qb.h > qr.h, "emboldened T bigger in both axes");
    CHECK(qr.texture == text_atlas_texture(ts) && qr.generation == text_atlas_generation(ts), "quad texture/generation");
    GlyphQuad again;
    text_glyph_quad(ts, &g[0], 32, 0, &again);
    CHECK(again.u0 == qr.u0 && again.v0 == qr.v0, "cached glyph returns the same atlas rect");
    // phases produce distinct entries
    Glyph p = g[0];
    p.phase = 2;
    GlyphQuad qp;
    text_glyph_quad(ts, &p, 32, 0, &qp);
    CHECK(!text_subpixel(ts) || (qp.u0 != qr.u0 || qp.v0 != qr.v0), "phase 2 rasterised separately");
    // space has no ink
    text_shape(reg, " ", 1, 0, g, 4, NULL);
    GlyphQuad qs;
    CHECK(!text_glyph_quad(ts, &g[0], 32, 0, &qs) && qs.texture == 0, "space yields no quad");
}

static void test_chrome_reference(TextSystem *ts) {
    printf("[chrome reference, ui scale %.3f]\n", UI_SCALE);
    for (size_t i = 0; i < sizeof g_ref / sizeof *g_ref; i++) {
        const RefInk *r = &g_ref[i];
        TextFont *f = text_font(ts, "gamefont", r->css_size, 400, 0);
        int x0, x1, y0, y1;
        float adv;
        float scale = UI_SCALE * r->extra_scale;
        render_run(ts, f, r->text, scale, 1, &x0, &x1, &y0, &y1, &adv);
        int w = x1 - x0 + 1, h = y1 - y0 + 1;
        printf("  %-14s %4.0fpx css -> device %.2fpx: ink %3dx%-3d chrome %3dx%-3d  dw %+d dh %+d  (advance %.2f css px = %.2f device)  %s\n",
               r->text, r->css_size, r->css_size * scale, w, h, r->ink_w, r->ink_h, w - r->ink_w, h - r->ink_h,
               adv, adv * scale, r->note);
        int tol = r->css_size >= 32 ? 6 : 1; // #instructions animates 30..32px while captured
        CHECK(abs(w - r->ink_w) <= tol, "%s ink width within %d px of Chrome", r->text, tol);
    }
    // Chrome advances of the item-picker card titles (doc/ref/krunker-v3.1.1/
    // interactions/README.md): gamefont 20px, linear advances, no kerning.
    {
        TextFont *f20 = text_font(ts, "gamefont", 20, 400, 0);
        const float rs = text_measure(f20, "Reflex Sight", 12, 0), ck = text_measure(f20, "Combat Knife", 12, 0);
        const float dots = text_measure(f20, "...", 3, 0);
        printf("  gamefont 20px: 'Reflex Sight' %.2f (chrome 157.5), 'Combat Knife' %.2f (chrome 172.5), '...' %.2f\n", rs, ck, dots);
        CHECK(fabsf(rs - 157.5f) < 0.01f && fabsf(ck - 172.5f) < 0.01f, "card titles match Chrome's advances");
        CHECK(fabsf(dots - 22.5f) < 0.01f, "three-period ellipsis fallback = 22.5px (gamefont has no U+2026)");
    }
    // #instructions animates font-size 30px -> 32px; show both ends
    for (int s = 30; s <= 32; s++) {
        TextFont *f = text_font(ts, "gamefont", (float) s, 400, 0);
        int x0, x1, y0, y1;
        float adv;
        render_run(ts, f, "CLICK TO PLAY", UI_SCALE, 0, &x0, &x1, &y0, &y1, &adv);
        printf("  CLICK TO PLAY @%dpx: ink %dx%d (chrome 187x24), advance %.2f device\n", s, x1 - x0 + 1, y1 - y0 + 1, adv * UI_SCALE);
    }
    // icon + header samples for the strip image
    TextFont *icon = text_font(ts, "material icons", 24, 400, 0);
    int x0, x1, y0, y1;
    render_run(ts, icon, "delete_forever keyboard_arrow_down person star play_arrow", 1, 1, &x0, &x1, &y0, &y1, NULL);
    TextFont *head = text_font(ts, "headerfont", 32, 400, 0);
    render_run(ts, head, "CLICK TO PLAY LT AV", 1, 1, &x0, &x1, &y0, &y1, NULL);
    TextFont *fb = text_font(ts, "sans-serif", 20, 400, 0);
    render_run(ts, fb, "Fallback \xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E Hello", 1, 1, &x0, &x1, &y0, &y1, NULL);
}

static void test_breaking_and_transform(void) {
    printf("[breaking/transform]\n");
    const char *s = "foo bar-baz 日本語x a-1 \"q\" (p) e—f x/y";
    size_t n = strlen(s);
    uint8_t flags[128];
    text_break_opportunities(s, n, flags);
    char vis[256];
    size_t o = 0;
    for (size_t i = 0; i < n && o < 250; i++) {
        if (flags[i] & TEXT_BREAK_ALLOWED) vis[o++] = '|';
        vis[o++] = s[i];
    }
    vis[o] = 0;
    printf("  %s\n", vis);
    CHECK(!(flags[1] & TEXT_BREAK_ALLOWED), "no break inside 'foo'");
    CHECK(flags[3] & TEXT_BREAK_SPACE, "space flagged");
    CHECK(!(flags[3] & TEXT_BREAK_ALLOWED), "no break before space");
    CHECK(flags[4] & TEXT_BREAK_ALLOWED, "break after space");
    CHECK(!(flags[7] & TEXT_BREAK_ALLOWED), "no break before hyphen");
    CHECK(flags[8] & TEXT_BREAK_ALLOWED, "break after hyphen");
    const char *cjk = strstr(s, "日");
    size_t c0 = (size_t) (cjk - s);
    CHECK(flags[c0] & TEXT_BREAK_ALLOWED, "break before CJK (after space)");
    CHECK(flags[c0 + 3] & TEXT_BREAK_ALLOWED, "break between ideographs");
    CHECK(flags[c0 + 6] & TEXT_BREAK_ALLOWED, "break between ideographs 2");
    CHECK(flags[c0 + 9] & TEXT_BREAK_ALLOWED, "break between ideograph and latin");
    const char *a1 = strstr(s, "a-1");
    CHECK(!(flags[(a1 - s) + 2] & TEXT_BREAK_ALLOWED), "no break after hyphen before digit");
    const char *q = strstr(s, "\"q\"");
    CHECK(!(flags[(q - s) + 1] & TEXT_BREAK_ALLOWED) && !(flags[(q - s) + 2] & TEXT_BREAK_ALLOWED), "no break around quotes");
    const char *p = strstr(s, "(p)");
    CHECK(!(flags[(p - s) + 1] & TEXT_BREAK_ALLOWED) && !(flags[(p - s) + 2] & TEXT_BREAK_ALLOWED), "no break inside (p)");
    const char *em = strstr(s, "e—f");
    CHECK((flags[(em - s) + 1] & TEXT_BREAK_ALLOWED) && (flags[(em - s) + 4] & TEXT_BREAK_ALLOWED), "break around em dash");
    const char *xy = strstr(s, "x/y");
    CHECK(!(flags[(xy - s) + 1] & TEXT_BREAK_ALLOWED) && (flags[(xy - s) + 2] & TEXT_BREAK_ALLOWED), "break after slash only");
    uint8_t zw[8];
    text_break_opportunities("a\xE2\x80\x8B" "b", 5, zw);
    CHECK((zw[1] & TEXT_BREAK_ZERO_WIDTH) && (zw[4] & TEXT_BREAK_ALLOWED) && !(zw[1] & TEXT_BREAK_ALLOWED), "ZWSP: break after, not before");
    uint8_t nb[8];
    text_break_opportunities("a\xC2\xA0" "b", 4, nb);
    CHECK(!(nb[1] & TEXT_BREAK_ALLOWED) && !(nb[3] & TEXT_BREAK_ALLOWED), "NBSP glues");

    char up[64];
    const char *src = "Click to play \xC3\xA9 \xC3\x9F \xC3\xBF \xC3\xB7";
    size_t w = text_uppercase(src, strlen(src), up, sizeof up);
    printf("  upper: %s\n", up);
    CHECK(!strcmp(up, "CLICK TO PLAY \xC3\x89 SS \xC5\xB8 \xC3\xB7"), "uppercase ASCII/Latin-1 (%zu bytes)", w);
}

static void test_gamma(void) {
    printf("[gamma]\n");
    static uint8_t lut[8][256];
    text_gamma_lut(lut);
    int mono = 1;
    for (int b = 0; b < 8; b++)
        for (int a = 1; a < 256; a++)
            if (lut[b][a] < lut[b][a - 1]) mono = 0;
    CHECK(mono, "LUT monotonic");
    CHECK(lut[0][0] == 0 && lut[0][255] == 255 && lut[7][0] == 0 && lut[7][255] == 255, "LUT endpoints");
    printf("  bucket0 (black text): 32->%d 64->%d 128->%d 192->%d\n", lut[0][32], lut[0][64], lut[0][128], lut[0][192]);
    printf("  bucket7 (white text): 32->%d 64->%d 128->%d 192->%d\n", lut[7][32], lut[7][64], lut[7][128], lut[7][192]);
    CHECK(lut[7][128] > 128, "white-on-dark coverage boosted by contrast");
    CHECK(text_gamma_bucket(255, 255, 255, 2.2f) == 7 && text_gamma_bucket(0, 0, 0, 2.2f) == 0, "luminance buckets");
    printf("  bucket for #353535 = %d, for rgba(255,255,255,.8)->#cccccc = %d\n",
           text_gamma_bucket(0x35, 0x35, 0x35, 2.2f), text_gamma_bucket(0xcc, 0xcc, 0xcc, 2.2f));
    static uint8_t lin[8][256];
    text_gamma_lut_ex(lin, 1.0f, 0.0f);
    int identity = 1;
    for (int a = 0; a < 256; a++) if (lin[3][a] != a) identity = 0;
    CHECK(identity, "gamma 1 / contrast 0 is identity");
}

static void test_atlas_rebuild(TextSystem *ts) {
    printf("[atlas]\n");
    uint32_t gen = text_atlas_generation(ts);
    // Fill the atlas with many distinct large glyphs until it rebuilds.
    TextFont *f = text_font(ts, "gamefont", 160, 400, 0);
    Glyph g[64];
    int n = text_shape(f, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789", 62, 0, g, 64, NULL);
    int rebuilt = 0;
    for (int size = 150; size < 400 && !rebuilt; size += 7) {
        for (int i = 0; i < n; i++) {
            GlyphQuad q;
            text_glyph_quad(ts, &g[i], (float) size, 0, &q);
            if (text_atlas_generation(ts) != gen) { rebuilt = 1; break; }
        }
    }
    printf("  fill %.2f, generation %u -> %u\n", text_atlas_fill(ts), gen, text_atlas_generation(ts));
    CHECK(rebuilt, "atlas rebuilt (generation bumped) when >90%% full");
    GlyphQuad q;
    CHECK(text_glyph_quad(ts, &g[0], 160, 0, &q) && q.generation == text_atlas_generation(ts), "glyphs usable after rebuild");
}

int main(int argc, char **argv) {
    const char *ui_dir = argc > 1 ? argv[1] : "ui";
    if (!glfwInit()) { fprintf(stderr, "glfwInit failed\n"); return 2; }
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    GLFWwindow *win = glfwCreateWindow(64, 64, "text test", NULL, NULL);
    if (!win) { fprintf(stderr, "no GL window\n"); return 2; }
    glfwMakeContextCurrent(win);
    if (!gladLoadGLLoader((GLADloadproc) glfwGetProcAddress)) { fprintf(stderr, "glad failed\n"); return 2; }
    printf("GL: %s\n", glGetString(GL_VERSION));

    TextSystem *ts = text_create(ui_dir);
    if (!ts) { fprintf(stderr, "text_create failed\n"); return 2; }
    printf("hinting=%d subpixel=%d\n", text_hinting(ts), text_subpixel(ts));
    CHECK(!text_register_face(ts, "gamefont", "css/fonts/font2.ttf", 400, TEXT_STYLE_NORMAL), "register gamefont");
    CHECK(!text_register_face(ts, "headerfont", "css/fonts/header.ttf", 400, TEXT_STYLE_NORMAL), "register headerfont");
    // registered exactly as the cascade sees it: the woff2 url from material-icons.css
    CHECK(!text_register_face(ts, "material icons", "css/fonts/flUhRq6tzZclQEJ-Vdg-IuiaDsNc.woff2", 400, TEXT_STYLE_NORMAL), "register material icons (woff2 -> ttf)");

    test_fonts_and_metrics(ts);
    test_shaping(ts);
    test_raster(ts);
    test_chrome_reference(ts);
    test_breaking_and_transform();
    test_gamma();

    write_pgm("/tmp/kn_text_strips.pgm", g_strips, STRIP_W, STRIP_LINES * STRIP_LINE_H);
    text_atlas_dump_pgm(ts, "/tmp/kn_text_atlas.pgm");
    printf("  wrote /tmp/kn_text_strips.pgm and /tmp/kn_text_atlas.pgm (fill %.2f)\n", text_atlas_fill(ts));

    test_atlas_rebuild(ts);

    text_destroy(ts);
    glfwDestroyWindow(win);
    glfwTerminate();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

/*
Throwaway CMakeLists.txt (e.g. /tmp/kn_text_test/CMakeLists.txt):

cmake_minimum_required(VERSION 3.16)
project(kn_text_test C)
set(CMAKE_C_STANDARD 11)
set(KN /home/user/src/KrunkNative)
foreach(dep ZLIB BZIP2 PNG HARFBUZZ BROTLI)
  set(FT_DISABLE_${dep} TRUE CACHE BOOL "" FORCE)
endforeach()
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
add_subdirectory(${KN}/client/lib/freetype freetype)
add_subdirectory(${KN}/client/lib/glad glad)
add_subdirectory(${KN}/client/lib/glfw glfw)
add_executable(test_text ${KN}/client/src/html/engine/text.c ${KN}/client/src/html/engine/tests/test_text.c)
target_include_directories(test_text PRIVATE ${KN}/client/include ${KN}/client/src/html/engine)
target_compile_options(test_text PRIVATE -Wall -Wextra)
target_link_libraries(test_text PRIVATE freetype glad glfw m)
*/
