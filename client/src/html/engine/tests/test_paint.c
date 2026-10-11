/*
 * test_paint.c - DlItem sequence tests for paint.c over hand-made Box trees.
 *
 * Build (throwaway, see README): a /tmp CMake project compiling this file,
 * paint.c and util.c (or a stub) with the engine dir on the include path.
 * Text and image modules are stubbed here so no FreeType / GL is needed.
 */
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "paint.h"
#include "text.h"
#include "image.h"

/* ---- stubs ---------------------------------------------------------------- */

static float g_last_glyph_size;
static uint32_t g_atlas_gen = 1;
static uint32_t g_reset_at_glyph;   /* nonzero: the atlas "fills up" at this glyph index (once) */

int text_glyph_quad(TextSystem *ts, const Glyph *g, float device_size, unsigned synth, GlyphQuad *out)
{
    (void) ts; (void) synth;
    g_last_glyph_size = device_size;
    memset(out, 0, sizeof *out);
    if (g->glyph_index == 0) return 0; /* space: no ink */
    if (g_reset_at_glyph && g->glyph_index == g_reset_at_glyph) { g_atlas_gen++; g_reset_at_glyph = 0; }
    const float k = device_size / 20.0f;
    out->texture = 7;
    out->generation = g_atlas_gen;
    out->u0 = 0.1f + 0.001f * (float) (g_atlas_gen & 63); /* where the glyph lives depends on the atlas */
    out->v0 = 0.2f; out->u1 = 0.3f; out->v1 = 0.4f;
    out->x_off = 1 * k;
    out->y_off = -14 * k;
    out->w = 10 * k;
    out->h = 16 * k;
    return 1;
}

uint32_t text_atlas_generation(const TextSystem *ts) { (void) ts; return g_atlas_gen; }
int text_subpixel(const TextSystem *ts) { (void) ts; return 1; }

static ImageEntry g_img_a = {1, 200, 80, 0, 101, 0};   /* map thumb */
static ImageEntry g_img_b = {2, 128, 128, 0, 102, 0};  /* menu icon */
static ImageEntry g_img_c = {3, 8, 8, 0, 103, 0};      /* class icon */
static ImageEntry g_img_fail = {0, 0, 0, 1, 0, 0};

static const char *g_last_image_base;

const ImageEntry *image_get(ImageCache *cache, const char *url, const char *base_url)
{
    (void) cache;
    g_last_image_base = base_url;
    if (!strcmp(url, "a.png")) return &g_img_a;
    if (!strcmp(url, "b.png")) return &g_img_b;
    if (!strcmp(url, "c.png")) return &g_img_c;
    return &g_img_fail;
}

unsigned image_texture(const ImageEntry *entry, int nearest)
{
    if (entry->failed) return 0;
    return nearest ? entry->tex_nearest : entry->tex;
}

/* ---- tiny test framework ---------------------------------------------------- */

static int g_fail, g_pass;

#define CHECK(cond) do { if (cond) g_pass++; else { g_fail++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_F(a, b) do { if (fabsf((float) (a) - (float) (b)) < 1e-3f) g_pass++; else { g_fail++; printf("  FAIL %s:%d: %s = %g, expected %g\n", __FILE__, __LINE__, #a, (double) (a), (double) (b)); } } while (0)

static const char *op_name(int op)
{
    static const char *names[DL_OP_COUNT] = {
        "RECT", "RRECT", "BORDER", "IMAGE", "GRADIENT", "GLYPHS", "BOX_SHADOW", "TEXT_SHADOW",
        "PUSH_CLIP_RECT", "PUSH_CLIP_RRECT", "PUSH_CLIP_POLY", "POP_CLIP", "PUSH_TRANSFORM",
        "POP_TRANSFORM", "PUSH_GROUP", "POP_GROUP", "SCROLL_OFFSET"};
    return op >= 0 && op < DL_OP_COUNT ? names[op] : "?";
}

static void dump(const DisplayList *dl)
{
    for (int i = 0; i < dl->n; i++) {
        const DlItem *it = &dl->items[i];
        printf("    [%d] %s", i, op_name(it->op));
        switch (it->op) {
        case DL_RECT: printf(" (%g,%g %gx%g) #%02x%02x%02x%02x", it->u.rect.rect.x, it->u.rect.rect.y, it->u.rect.rect.w, it->u.rect.rect.h, it->u.rect.color.r, it->u.rect.color.g, it->u.rect.color.b, it->u.rect.color.a); break;
        case DL_RRECT: printf(" (%g,%g %gx%g) r%g #%02x%02x%02x%02x", it->u.rrect.rect.x, it->u.rrect.rect.y, it->u.rrect.rect.w, it->u.rrect.rect.h, it->u.rrect.radii.tl, it->u.rrect.color.r, it->u.rrect.color.g, it->u.rrect.color.b, it->u.rrect.color.a); break;
        case DL_IMAGE: printf(" tex%u dst(%g,%g %gx%g) uv(%g,%g,%g,%g)", it->u.image.tex, it->u.image.dst.x, it->u.image.dst.y, it->u.image.dst.w, it->u.image.dst.h, it->u.image.uv[0], it->u.image.uv[1], it->u.image.uv[2], it->u.image.uv[3]); break;
        case DL_PUSH_TRANSFORM: printf(" [%g %g %g %g %g %g]", it->u.transform.m.a, it->u.transform.m.b, it->u.transform.m.c, it->u.transform.m.d, it->u.transform.m.e, it->u.transform.m.f); break;
        case DL_SCROLL_OFFSET: printf(" (%g,%g)", it->u.scroll.dx, it->u.scroll.dy); break;
        case DL_PUSH_CLIP_RECT: printf(" (%g,%g %gx%g)", it->u.clip_rect.rect.x, it->u.clip_rect.rect.y, it->u.clip_rect.rect.w, it->u.clip_rect.rect.h); break;
        case DL_PUSH_CLIP_RRECT: printf(" (%g,%g %gx%g) r%g", it->u.clip_rrect.rect.x, it->u.clip_rrect.rect.y, it->u.clip_rrect.rect.w, it->u.clip_rrect.rect.h, it->u.clip_rrect.radii.tl); break;
        case DL_BOX_SHADOW: printf(" inset=%d spread=%g blur=%g", it->u.box_shadow.inset, it->u.box_shadow.spread, it->u.box_shadow.blur); break;
        case DL_TEXT_SHADOW: printf(" n=%d (%g,%g) blur=%g", it->u.text_shadow.n, it->u.text_shadow.dx, it->u.text_shadow.dy, it->u.text_shadow.blur); break;
        case DL_GLYPHS: printf(" n=%d", it->u.glyphs.n); break;
        case DL_PUSH_GROUP: printf(" opacity=%g filters=%d", it->u.group.opacity, it->u.group.n_filters); break;
        default: break;
        }
        printf("\n");
    }
}

/* Compares the op sequence; -1 terminates the expectation list. */
static int expect_ops(const char *name, const DisplayList *dl, const int *ops)
{
    int n = 0;
    while (ops[n] >= 0) n++;
    int ok = dl->n == n;
    for (int i = 0; ok && i < n; i++) ok = (int) dl->items[i].op == ops[i];
    if (ok) { g_pass++; return 1; }
    g_fail++;
    printf("  FAIL %s: expected", name);
    for (int i = 0; i < n; i++) printf(" %s", op_name(ops[i]));
    printf("\n  got %d items:\n", dl->n);
    dump(dl);
    return 0;
}

/* ---- box tree helpers ----------------------------------------------------- */

#define MAX_BOXES 256
static Box g_boxes[MAX_BOXES];
static CssComputed g_styles[MAX_BOXES];
static int g_nboxes;

static CssColor rgba(int r, int g, int b, int a) { CssColor c = {(uint8_t) r, (uint8_t) g, (uint8_t) b, (uint8_t) a, CSS_COLOR_RGBA}; return c; }
static CssLength px(float v) { CssLength l = {v, 0, 0, CSS_LEN_VALUE}; return l; }
static CssLength pct(float v) { CssLength l = {0, v, 0, CSS_LEN_VALUE}; return l; }
static CssLength len_auto(void) { CssLength l = {0, 0, 0, CSS_LEN_AUTO}; return l; }

static void style_defaults(CssComputed *s)
{
    memset(s, 0, sizeof *s);
    s->color = rgba(0x35, 0x35, 0x35, 255);
    s->font_size = 16;
    s->font_weight = 400;
    s->display = CSS_DISPLAY_BLOCK;
    s->z_index_auto = 1;
    s->opacity = 1;
    s->transform_origin_x = pct(50);
    s->transform_origin_y = pct(50);
    s->background_color = rgba(0, 0, 0, 0);
    for (int i = 0; i < 4; i++) {
        s->border_color[i].kind = CSS_COLOR_CURRENT;
        s->border_style[i] = CSS_BORDER_STYLE_NONE;
    }
    s->max_width.kind = s->max_height.kind = CSS_LEN_NONE;
    s->flex_basis = len_auto();
}

static void reset_tree(void)
{
    g_nboxes = 0;
    memset(g_boxes, 0, sizeof g_boxes);
}

/* Boxes get absolute (x, y); `container` defaults to the tree parent. */
static Box *mk(Box *parent, int type, float x, float y, float w, float h)
{
    Box *b = &g_boxes[g_nboxes];
    CssComputed *s = &g_styles[g_nboxes];
    g_nboxes++;
    style_defaults(s);
    b->type = (uint8_t) type;
    b->style = s;
    b->z_auto = 1;
    b->x = x; b->y = y; b->w = w; b->h = h;
    b->lx = parent ? x - parent->x : x;
    b->ly = parent ? y - parent->y : y;
    b->paint_dirty = 1;
    b->parent = parent;
    b->container = parent;
    if (parent) {
        if (!parent->first_child) parent->first_child = b;
        else parent->last_child->next = b;
        b->prev = parent->last_child;
        parent->last_child = b;
    }
    if (!parent) b->flags |= BOXF_STACKING_CONTEXT;
    return b;
}

static CssComputed *st(Box *b) { return (CssComputed *) b->style; }

static Box *mk_root(void) { return mk(NULL, BOX_BLOCK, 0, 0, 1280, 720); }

static void set_border(Box *b, float w, CssColor c)
{
    for (int i = 0; i < 4; i++) {
        b->border[i] = w;
        st(b)->border_width[i] = w;
        st(b)->border_style[i] = CSS_BORDER_STYLE_SOLID;
        st(b)->border_color[i] = c;
    }
}

static void set_positioned(Box *b, int z)
{
    st(b)->position = CSS_POSITION_ABSOLUTE;
    st(b)->z_index_auto = 0;
    st(b)->z_index = z;
    b->flags |= BOXF_POSITIONED | BOXF_OUT_OF_FLOW | BOXF_STACKING_CONTEXT;
    b->z_auto = 0;
    b->z_index = z;
}

static void set_relative(Box *b)
{
    st(b)->position = CSS_POSITION_RELATIVE;
    b->flags |= BOXF_POSITIONED;
}

static void set_scroller(Box *b, float scroll_y, float scroll_h, float bar)
{
    st(b)->overflow_y = CSS_OVERFLOW_AUTO;
    st(b)->overflow_x = CSS_OVERFLOW_HIDDEN;
    b->flags |= BOXF_SCROLL_CONTAINER | BOXF_CLIPS | (bar > 0 ? BOXF_VBAR : 0);
    b->scroll_y = scroll_y;
    b->scroll_h = scroll_h;
    b->scrollbar_w = bar;
    b->bar_w = bar;
}

/* One line holding one text run (glyph i at pen x = 10 * i). */
static Glyph g_glyphs[8];
static TextRun g_run;
static LineBox g_line;
static InlineItem g_items[1];

static Box *mk_text(Box *block, float x, float baseline, const char *text, float font_size)
{
    Box *t = mk(block, BOX_TEXT, x, baseline - font_size, (float) strlen(text) * 10, font_size * 1.2f);
    t->style = block->style;
    t->flags |= BOXF_INLINE_LEVEL;
    int n = (int) strlen(text);
    if (n > 8) n = 8;
    memset(g_glyphs, 0, sizeof g_glyphs);
    for (int i = 0; i < n; i++) {
        g_glyphs[i].glyph_index = text[i] == ' ' ? 0 : (uint32_t) text[i];
        g_glyphs[i].x = 10.0f * (float) i;
        g_glyphs[i].advance = 10;
    }
    memset(&g_run, 0, sizeof g_run);
    g_run.font_size = font_size;
    g_run.text = text;
    g_run.len = (uint32_t) strlen(text);
    g_run.x = x;
    g_run.y = baseline;
    g_run.width = 10.0f * (float) n;
    g_run.glyphs = g_glyphs;
    g_run.glyph_count = n;
    t->runs = &g_run;
    t->run_count = 1;
    memset(g_items, 0, sizeof g_items);
    g_items[0].kind = ITEM_TEXT;
    g_items[0].box = t;
    g_items[0].x = x;
    g_items[0].y = baseline - font_size;
    g_items[0].w = g_run.width;
    g_items[0].h = font_size * 1.2f;
    g_items[0].baseline = baseline;
    g_items[0].run = &g_run;
    memset(&g_line, 0, sizeof g_line);
    g_line.x = block->x; g_line.y = baseline - font_size; g_line.w = block->w; g_line.h = font_size * 1.2f;
    g_line.baseline = baseline;
    g_line.items = g_items;
    g_line.item_count = 1;
    block->lines = &g_line;
    block->line_count = 1;
    block->flags |= BOXF_HAS_LINES;
    return t;
}

static PaintCtx g_ctx;
static DisplayList g_dl;
static struct { int dummy; } g_fake_text, g_fake_images;

static void build(Box *root)
{
    CHECK(paint_build(&g_ctx, root, &g_dl) == 0);
}

static void fresh_ctx(void)
{
    paint_ctx_free(&g_ctx);
    paint_ctx_init(&g_ctx);
    g_ctx.device_scale = 1;
    g_ctx.viewport_w = 1280;
    g_ctx.viewport_h = 720;
    g_ctx.text = (TextSystem *) &g_fake_text;
    g_ctx.images = (ImageCache *) &g_fake_images;
}

/* Index of the n-th item with op `op` (or -1). */
static int find_op(const DisplayList *dl, int op, int nth)
{
    for (int i = 0; i < dl->n; i++)
        if ((int) dl->items[i].op == op && nth-- == 0) return i;
    return -1;
}

/* ---- tests --------------------------------------------------------------- */

static void test_background_radius_clip(void)
{
    printf("background + radius clip\n");
    reset_tree(); fresh_ctx();
    Box *root = mk_root();
    Box *b = mk(root, BOX_BLOCK, 10, 10, 100, 50);
    st(b)->background_color = rgba(255, 0, 0, 255);
    for (int i = 0; i < 4; i++) st(b)->border_radius[i] = px(10);
    st(b)->bg_layer_count = 1;
    st(b)->bg_layers[0].image.kind = CSS_IMAGE_URL;
    st(b)->bg_layers[0].image.url = "b.png";
    st(b)->bg_layers[0].repeat_x = st(b)->bg_layers[0].repeat_y = CSS_BG_REPEAT_NO_REPEAT;
    st(b)->bg_layers[0].size.kind = CSS_BG_SIZE_CONTAIN;
    st(b)->bg_layers[0].x = pct(50);
    st(b)->bg_layers[0].y = pct(50);
    build(root);
    const int ops[] = {DL_RRECT, DL_PUSH_CLIP_RRECT, DL_IMAGE, DL_POP_CLIP, -1};
    if (expect_ops("bg", &g_dl, ops)) {
        CHECK_F(g_dl.items[0].u.rrect.radii.tl, 10);
        CHECK(g_dl.items[0].u.rrect.color.r == 255);
        CHECK_F(g_dl.items[1].u.clip_rrect.rect.w, 100);
        /* contain in 100x50 -> 50x50 centred */
        CHECK_F(g_dl.items[2].u.image.dst.w, 50);
        CHECK_F(g_dl.items[2].u.image.dst.h, 50);
        CHECK_F(g_dl.items[2].u.image.dst.x, 35);
        CHECK_F(g_dl.items[2].u.image.dst.y, 10);
        CHECK(g_dl.items[2].u.image.tex == 2);
        CHECK(g_dl.items[2].u.image.premultiplied == 1);
    }

    /* radius clamp: 50% on a 100x50 box -> 25 */
    for (int i = 0; i < 4; i++) st(b)->border_radius[i] = pct(50);
    DlRadii r = paint_border_radii(b);
    CHECK_F(r.tl, 25);
    CHECK_F(r.br, 25);

    /* pixelated: nearest texture; style change picked up without paint_dirty via the style hash */
    st(b)->image_rendering = CSS_IMAGE_RENDERING_PIXELATED;
    st(b)->hash_non_inherited ^= 1;
    build(root);
    CHECK(g_dl.n == 4 && g_dl.items[2].u.image.tex == 102 && g_dl.items[2].u.image.nearest == 1);
}

static void test_border_triangle(void)
{
    printf("border triangle\n");
    reset_tree(); fresh_ctx();
    Box *root = mk_root();
    Box *b = mk(root, BOX_BLOCK, 0, 0, 0, 0);  /* .triangle: width/height 0 */
    set_border(b, 10, rgba(0, 0, 0, 0));
    st(b)->border_color[CSS_BOTTOM] = rgba(0x21, 0x96, 0xf3, 255);
    st(b)->border_style[CSS_TOP] = CSS_BORDER_STYLE_NONE;
    b->border[CSS_TOP] = 0;
    st(b)->border_style[CSS_LEFT] = CSS_BORDER_STYLE_DASHED;
    build(root);
    const int ops[] = {DL_BORDER, -1};
    if (expect_ops("triangle", &g_dl, ops)) {
        const DlItem *it = &g_dl.items[0];
        CHECK_F(it->u.border.width[DL_EDGE_TOP], 0);
        CHECK(it->u.border.style[DL_EDGE_TOP] == DL_BORDER_NONE);
        CHECK_F(it->u.border.width[DL_EDGE_BOTTOM], 10);
        CHECK(it->u.border.color[DL_EDGE_BOTTOM].b == 0xf3);
        CHECK(it->u.border.color[DL_EDGE_LEFT].a == 0);
        CHECK(it->u.border.style[DL_EDGE_LEFT] == DL_BORDER_DASHED);
        CHECK_F(it->u.border.rect.w, 0);
    }
    /* currentColor border resolves to color */
    for (int i = 0; i < 4; i++) st(b)->border_color[i].kind = CSS_COLOR_CURRENT;
    st(b)->color = rgba(1, 2, 3, 255);
    b->paint_dirty = 1;
    build(root);
    CHECK(g_dl.n == 1 && g_dl.items[0].u.border.color[1].g == 2);
}

static void test_box_shadow(void)
{
    printf("box-shadow inset / 4000px hole\n");
    reset_tree(); fresh_ctx();
    Box *root = mk_root();
    Box *b = mk(root, BOX_BLOCK, 10, 10, 200, 40);
    set_border(b, 2, rgba(0, 0, 0, 255));
    st(b)->background_color = rgba(0xe0, 0xe0, 0xe0, 255);
    st(b)->box_shadow_count = 1;
    st(b)->box_shadow.inset = 1;
    st(b)->box_shadow.y = -7;
    st(b)->box_shadow.color = rgba(0xb3, 0xb3, 0xb3, 255);
    build(root);
    const int ops[] = {DL_RECT, DL_BOX_SHADOW, DL_BORDER, -1};
    if (expect_ops("inset", &g_dl, ops)) {
        const DlItem *it = &g_dl.items[1];
        CHECK(it->u.box_shadow.inset == 1);
        CHECK_F(it->u.box_shadow.dy, -7);
        CHECK_F(it->u.box_shadow.inset_pad[0], 2);
        CHECK_F(it->u.box_shadow.rect.w, 200);
    }

    /* #knPreviewHole: 0 0 0 4000px rgba(0,0,0,.5) inside overflow:hidden */
    reset_tree(); fresh_ctx();
    root = mk_root();
    Box *holder = mk(root, BOX_BLOCK, 100, 100, 600, 400);
    st(holder)->overflow_x = st(holder)->overflow_y = CSS_OVERFLOW_HIDDEN;
    holder->flags |= BOXF_CLIPS;
    set_relative(holder);
    Box *hole = mk(holder, BOX_BLOCK, 200, 150, 300, 200);
    st(hole)->position = CSS_POSITION_ABSOLUTE;
    hole->flags |= BOXF_POSITIONED | BOXF_OUT_OF_FLOW;
    st(hole)->box_shadow_count = 1;
    st(hole)->box_shadow.spread = 4000;
    st(hole)->box_shadow.color = rgba(0, 0, 0, 128);
    build(root);
    const int ops2[] = {DL_PUSH_CLIP_RECT, DL_BOX_SHADOW, DL_POP_CLIP, -1};
    if (expect_ops("hole", &g_dl, ops2)) {
        CHECK(g_dl.items[1].u.box_shadow.inset == 0);
        CHECK_F(g_dl.items[1].u.box_shadow.spread, 4000);
        CHECK_F(g_dl.items[1].u.box_shadow.rect.x, 200);
        CHECK_F(g_dl.items[0].u.clip_rect.rect.x, 100);
        CHECK_F(g_dl.items[0].u.clip_rect.rect.w, 600);
    }
}

static void test_two_layer_background(void)
{
    printf("two-layer background (cover, N%% auto, round, gradient)\n");
    reset_tree(); fresh_ctx();
    Box *root = mk_root();
    Box *b = mk(root, BOX_BLOCK, 0, 0, 400, 100);
    st(b)->bg_layer_count = 2;
    /* layer 0 (top): a.png 200x80 cover, `right 8px center`, no-repeat */
    CssBgLayer *l0 = &st(b)->bg_layers[0];
    l0->image.kind = CSS_IMAGE_URL;
    l0->image.url = "a.png";
    l0->size.kind = CSS_BG_SIZE_COVER;
    l0->x.px = -8; l0->x.pct = 100;
    l0->y = pct(50);
    l0->repeat_x = l0->repeat_y = CSS_BG_REPEAT_NO_REPEAT;
    /* layer 1 (bottom): c.png 8x8 at `50% auto`, repeat */
    CssBgLayer *l1 = &st(b)->bg_layers[1];
    l1->image.kind = CSS_IMAGE_URL;
    l1->image.url = "c.png";
    l1->size.kind = CSS_BG_SIZE_LENGTHS;
    l1->size.w = pct(50);
    l1->size.h = len_auto();
    l1->repeat_x = l1->repeat_y = CSS_BG_REPEAT_REPEAT;
    build(root);
    CHECK(g_dl.n >= 4 && g_dl.items[0].op == DL_PUSH_CLIP_RECT && g_dl.items[g_dl.n - 1].op == DL_POP_CLIP);
    int n_c = 0, n_a = 0, last_c = -1, first_a = 1 << 30;
    for (int i = 0; i < g_dl.n; i++) {
        const DlItem *it = &g_dl.items[i];
        if (it->op != DL_IMAGE) continue;
        if (it->u.image.tex == 3) { n_c++; last_c = i; CHECK_F(it->u.image.dst.w, 200); CHECK_F(it->u.image.dst.h, 200); }
        if (it->u.image.tex == 1) {
            n_a++;
            if (i < first_a) first_a = i;
            /* cover 400x100 with 200x80 -> 400x160; x = (400-400)*1 - 8 = -8; y = (100-160)/2 = -30 */
            CHECK_F(it->u.image.dst.w, 400); CHECK_F(it->u.image.dst.h, 160);
            CHECK_F(it->u.image.dst.x, -8); CHECK_F(it->u.image.dst.y, -30);
        }
    }
    CHECK(n_a == 1);
    CHECK(n_c == 2);               /* 200x200 tiles repeated over 400x100 */
    CHECK(last_c < first_a);       /* bottom layer painted before the top one */

    /* round: 90px tiles over 400px -> round(4.44) = 4 tiles of 100px */
    l1->size.w = px(90); l1->size.h = px(90);
    l1->repeat_x = CSS_BG_REPEAT_ROUND; l1->repeat_y = CSS_BG_REPEAT_NO_REPEAT;
    b->paint_dirty = 1;
    build(root);
    n_c = 0;
    for (int i = 0; i < g_dl.n; i++)
        if (g_dl.items[i].op == DL_IMAGE && g_dl.items[i].u.image.tex == 3) { n_c++; CHECK_F(g_dl.items[i].u.image.dst.w, 100); }
    CHECK(n_c == 4);

    /* gradient layer, currentColor stop */
    reset_tree(); fresh_ctx();
    root = mk_root();
    b = mk(root, BOX_BLOCK, 0, 0, 100, 20);
    st(b)->bg_layer_count = 1;
    st(b)->bg_layers[0].image.kind = CSS_IMAGE_GRADIENT;
    st(b)->bg_layers[0].image.gradient.angle_deg = 90;
    st(b)->bg_layers[0].image.gradient.stop_count = 2;
    st(b)->bg_layers[0].image.gradient.stops[0].color = rgba(255, 0, 0, 255);
    st(b)->bg_layers[0].image.gradient.stops[1].color.kind = CSS_COLOR_CURRENT;
    st(b)->bg_layers[0].repeat_x = st(b)->bg_layers[0].repeat_y = CSS_BG_REPEAT_NO_REPEAT;
    st(b)->bg_layers[0].size.w = st(b)->bg_layers[0].size.h = len_auto();
    build(root);
    const int ops[] = {DL_GRADIENT, -1};
    if (expect_ops("gradient", &g_dl, ops)) {
        CHECK_F(g_dl.items[0].u.gradient.box.w, 100);
        CHECK(g_dl.items[0].u.gradient.gradient.stops[1].color.r == 0x35);
    }

    /* missing image paints nothing */
    st(b)->bg_layers[0].image.kind = CSS_IMAGE_URL;
    st(b)->bg_layers[0].image.url = "missing.png";
    b->paint_dirty = 1;
    build(root);
    CHECK(g_dl.n == 0);
}

static void test_z_order(void)
{
    printf("z-index ordering\n");
    reset_tree(); fresh_ctx();
    Box *root = mk_root();
    st(root)->background_color = rgba(1, 1, 1, 255);
    Box *neg = mk(root, BOX_BLOCK, 0, 0, 10, 10);  set_positioned(neg, -1);  st(neg)->background_color = rgba(2, 2, 2, 255);
    Box *flow = mk(root, BOX_BLOCK, 0, 0, 10, 10); st(flow)->background_color = rgba(3, 3, 3, 255);
    Box *huge = mk(root, BOX_BLOCK, 0, 0, 10, 10); set_positioned(huge, INT32_MAX); st(huge)->background_color = rgba(4, 4, 4, 255);
    Box *one = mk(root, BOX_BLOCK, 0, 0, 10, 10);  set_positioned(one, 1);   st(one)->background_color = rgba(5, 5, 5, 255);
    Box *aut = mk(root, BOX_BLOCK, 0, 0, 10, 10);  set_relative(aut); st(aut)->background_color = rgba(6, 6, 6, 255);
    Box *zero = mk(root, BOX_BLOCK, 0, 0, 10, 10); set_positioned(zero, 0);  st(zero)->background_color = rgba(7, 7, 7, 255);
    Box *one2 = mk(root, BOX_BLOCK, 0, 0, 10, 10); set_positioned(one2, 1);  st(one2)->background_color = rgba(8, 8, 8, 255);
    /* positioned z:auto box with a nested z:-1 child: that child belongs to root's context */
    Box *nested = mk(aut, BOX_BLOCK, 0, 0, 5, 5); set_positioned(nested, -1); st(nested)->background_color = rgba(9, 9, 9, 255);
    /* a float and an in-flow block inside flow */
    Box *fl = mk(flow, BOX_BLOCK, 0, 0, 5, 5); st(fl)->float_ = CSS_FLOAT_RIGHT; fl->flags |= BOXF_FLOAT; st(fl)->background_color = rgba(10, 10, 10, 255);
    Box *blk = mk(flow, BOX_BLOCK, 0, 0, 5, 5); st(blk)->background_color = rgba(11, 11, 11, 255);
    (void) blk;
    build(root);
    /* root bg, z<0 (nested -1 [order 1st found? tree order: neg before nested]), blocks, floats, z0/auto, z>0 */
    const int expect[] = {1, 2, 9, 3, 11, 10, 6, 7, 5, 8, 4};
    int got[16], n = 0;
    for (int i = 0; i < g_dl.n && n < 16; i++)
        if (g_dl.items[i].op == DL_RECT) got[n++] = g_dl.items[i].u.rect.color.r;
    int ok = n == 11;
    for (int i = 0; ok && i < 11; i++) ok = got[i] == expect[i];
    CHECK(ok);
    if (!ok) { printf("  order:"); for (int i = 0; i < n; i++) printf(" %d", got[i]); printf("\n"); dump(&g_dl); }
}

static void test_opacity(void)
{
    printf("opacity group vs fast path, filters\n");
    reset_tree(); fresh_ctx();
    Box *root = mk_root();
    Box *b = mk(root, BOX_BLOCK, 0, 0, 50, 50);
    st(b)->opacity = 0.5f;
    st(b)->background_color = rgba(0, 0, 255, 200);
    build(root);
    const int ops[] = {DL_RECT, -1};
    if (expect_ops("fast", &g_dl, ops)) CHECK(g_dl.items[0].u.rect.color.a == 100);

    /* two primitives -> group */
    set_border(b, 1, rgba(0, 0, 0, 255));
    b->paint_dirty = 1;
    build(root);
    const int ops2[] = {DL_PUSH_GROUP, DL_RECT, DL_BORDER, DL_POP_GROUP, -1};
    if (expect_ops("group", &g_dl, ops2)) {
        CHECK_F(g_dl.items[0].u.group.opacity, 0.5f);
        CHECK_F(g_dl.items[0].u.group.rect.w, 50);
        CHECK(g_dl.items[1].u.rect.color.a == 200);
    }

    /* filter: always a group, ops passed through */
    st(b)->opacity = 1;
    st(b)->filter_count = 2;
    st(b)->filters[0].kind = CSS_FILTER_GRAYSCALE; st(b)->filters[0].amount = 1;
    st(b)->filters[1].kind = CSS_FILTER_DROP_SHADOW; st(b)->filters[1].shadow.x = 7; st(b)->filters[1].shadow.blur = 10;
    st(b)->filters[1].shadow.color.kind = CSS_COLOR_CURRENT;
    b->paint_dirty = 1;
    build(root);
    if (expect_ops("filter", &g_dl, ops2)) {
        const DlGroup *g = &g_dl.items[0].u.group;
        CHECK(g->n_filters == 2);
        CHECK(g->filters[0].kind == CSS_FILTER_GRAYSCALE);
        CHECK(g->filters[1].kind == CSS_FILTER_DROP_SHADOW);
        CHECK_F(g->filters[1].shadow.x, 7);
        CHECK(g->filters[1].shadow.color.kind == CSS_COLOR_RGBA && g->filters[1].shadow.color.r == 0x35);
        CHECK(g->rect.w > 50);   /* drop-shadow extends the FBO bounds */
    }
}

static void test_transform(void)
{
    printf("nested transform (scale then rotate)\n");
    reset_tree(); fresh_ctx();
    Box *root = mk_root();
    /* #uiBase-like: scale(0.5) with origin 0 0 */
    Box *base = mk(root, BOX_BLOCK, 0, 0, 1700, 900);
    st(base)->transform.count = 1;
    st(base)->transform.ops[0].kind = CSS_TRANSFORM_SCALE;
    st(base)->transform.ops[0].sx = st(base)->transform.ops[0].sy = 0.5f;
    st(base)->transform_origin_x = px(0);
    st(base)->transform_origin_y = px(0);
    /* child: translate(-50%,-50%) rotate(90deg) around its centre, at (100,100) 40x20 */
    Box *c = mk(base, BOX_BLOCK, 100, 100, 40, 20);
    st(c)->transform.count = 2;
    st(c)->transform.ops[0].kind = CSS_TRANSFORM_TRANSLATE;
    st(c)->transform.ops[0].tx = pct(-50);
    st(c)->transform.ops[0].ty = pct(-50);
    st(c)->transform.ops[1].kind = CSS_TRANSFORM_ROTATE;
    st(c)->transform.ops[1].angle_deg = 90;
    st(c)->background_color = rgba(9, 9, 9, 255);
    mk_text(c, 100, 115, "ab", 20);
    g_ctx.device_scale = 2;
    build(root);
    /* root frame -> base frame (its own transform) -> translate to c's origin -> c's transform */
    const int ops[] = {DL_PUSH_TRANSFORM, DL_PUSH_TRANSFORM, DL_PUSH_TRANSFORM, DL_RECT, DL_GLYPHS,
                       DL_POP_TRANSFORM, DL_POP_TRANSFORM, DL_POP_TRANSFORM, -1};
    if (expect_ops("transform", &g_dl, ops)) {
        const Mat2D *m0 = &g_dl.items[0].u.transform.m;
        CHECK_F(m0->a, 0.5f); CHECK_F(m0->d, 0.5f); CHECK_F(m0->e, 0); CHECK_F(m0->f, 0);
        const Mat2D *t1 = &g_dl.items[1].u.transform.m;
        CHECK_F(t1->a, 1); CHECK_F(t1->e, 100); CHECK_F(t1->f, 100);
        const Mat2D *m1 = &g_dl.items[2].u.transform.m;
        /* rotate(90): a=0 b=1 c=-1 d=0; local origin (20,10) maps to itself minus (20,10) translate */
        CHECK_F(m1->a, 0); CHECK_F(m1->b, 1); CHECK_F(m1->c, -1); CHECK_F(m1->d, 0);
        float x = m1->a * 20 + m1->c * 10 + m1->e, y = m1->b * 20 + m1->d * 10 + m1->f;
        CHECK_F(x, 0); CHECK_F(y, 0);
        /* the rect is in c's local frame */
        CHECK_F(g_dl.items[3].u.rect.rect.x, 0);
        CHECK_F(g_dl.items[3].u.rect.rect.w, 40);
        /* glyphs rasterised at 20px * 2 (device) * 0.5 (base) * 1 (rotation) = 20 */
        CHECK_F(g_last_glyph_size, 20);
        /* glyph 0: pen 100 -> local 0; x_off 1 at scale 1 */
        CHECK_F(g_dl.items[4].u.glyphs.quads[0].x, 1);
        CHECK_F(g_dl.items[4].u.glyphs.quads[0].y, 15 - 14);
        CHECK_F(g_dl.items[4].u.glyphs.quads[0].w, 10);
    }
}

static void test_scroll_container(void)
{
    printf("scroll container clip + offset + scrollbar\n");
    reset_tree(); fresh_ctx();
    Box *root = mk_root();
    Box *sc = mk(root, BOX_BLOCK, 100, 100, 400, 300);
    set_border(sc, 5, rgba(0, 0, 0, 255));
    set_scroller(sc, 100, 1000, 12);
    for (int i = 0; i < 4; i++) st(sc)->border_radius[i] = px(8);
    Box *child = mk(sc, BOX_BLOCK, 105, 105, 200, 1000);
    st(child)->background_color = rgba(1, 2, 3, 255);
    build(root);
    const int ops[] = {DL_BORDER, DL_PUSH_CLIP_RRECT, DL_SCROLL_OFFSET, DL_RECT, DL_POP_CLIP,
                       DL_RRECT, DL_BOX_SHADOW, DL_RRECT, DL_BOX_SHADOW, -1};
    if (expect_ops("scroll", &g_dl, ops)) {
        const DlItem *clip = &g_dl.items[1];
        CHECK_F(clip->u.clip_rrect.rect.x, 105);
        CHECK_F(clip->u.clip_rrect.rect.w, 400 - 10 - 12);
        CHECK_F(clip->u.clip_rrect.rect.h, 290);
        CHECK_F(clip->u.clip_rrect.radii.tl, 3);
        CHECK_F(g_dl.items[2].u.scroll.dy, -100);
        CHECK_F(g_dl.items[3].u.rect.rect.y, 105);   /* unscrolled: the offset item moves it */
        const DlItem *track = &g_dl.items[5];
        CHECK_F(track->u.rrect.rect.x, 100 + 400 - 5 - 12);
        CHECK_F(track->u.rrect.rect.w, 12);
        CHECK_F(track->u.rrect.rect.h, 290);
        CHECK_F(track->u.rrect.radii.tl, 6);   /* radius 10 clamped to half the 12px width */
        CHECK(track->u.rrect.color.r == 0xf5);
        CHECK(g_dl.items[6].u.box_shadow.inset == 1);
        const DlItem *thumb = &g_dl.items[7];
        CHECK_F(thumb->u.rrect.rect.h, 290 * 290.0f / 1000.0f);
        CHECK_F(thumb->u.rrect.rect.y, 105 + (290 - 290 * 0.29f) * (100.0f / 710.0f));
        CHECK(thumb->u.rrect.color.r == 0x86);
    }

    /* rtl: bar on the left, clip shifted */
    sc->flags |= BOXF_RTL;
    sc->paint_dirty = 1;
    build(root);
    if (g_dl.n == 9) {
        CHECK_F(g_dl.items[1].u.clip_rrect.rect.x, 105 + 12);
        CHECK_F(g_dl.items[5].u.rrect.rect.x, 105);
    } else { CHECK(0); dump(&g_dl); }

    /* scrollbar style hook (#chatList: 8px, rgba(0,0,0,.4) square track, no shadows) */
    sc->flags &= ~(uint32_t) BOXF_RTL;
    sc->paint_dirty = 1;
    extern int chat_scrollbar(void *user, const Box *box, PaintScrollbarStyle *st);
    g_ctx.host.scrollbar_style = chat_scrollbar;
    build(root);
    const int ops2[] = {DL_BORDER, DL_PUSH_CLIP_RRECT, DL_SCROLL_OFFSET, DL_RECT, DL_POP_CLIP, DL_RECT, DL_RECT, -1};
    if (expect_ops("chat scrollbar", &g_dl, ops2)) CHECK(g_dl.items[5].u.rect.color.a == 102);

    /* hide_scrollbars: clip and content only */
    g_ctx.hide_scrollbars = 1;
    build(root);
    const int ops3[] = {DL_BORDER, DL_PUSH_CLIP_RRECT, DL_SCROLL_OFFSET, DL_RECT, DL_POP_CLIP, -1};
    expect_ops("hidden scrollbars", &g_dl, ops3);
    g_ctx.hide_scrollbars = 0;
}

int chat_scrollbar(void *user, const Box *box, PaintScrollbarStyle *st)
{
    (void) user; (void) box;
    st->width = 8;
    st->track_color = rgba(0, 0, 0, 102);
    st->track_radius = 0;
    st->track_shadow_count = 0;
    st->thumb_radius = 0;
    st->thumb_shadow_count = 0;
    return 1;
}

static void test_scrolled_positioned(void)
{
    printf("absolute box inside a scroller (ancestor clip chain, folded scroll)\n");
    reset_tree(); fresh_ctx();
    Box *root = mk_root();
    Box *sc = mk(root, BOX_BLOCK, 0, 0, 500, 300);
    set_relative(sc);
    set_scroller(sc, 50, 1000, 0);
    Box *abs = mk(sc, BOX_BLOCK, 10, 400, 20, 20);
    set_positioned(abs, 2);
    st(abs)->background_color = rgba(0, 0xff, 0, 255);
    Box *fixed = mk(sc, BOX_BLOCK, 0, 0, 20, 20);
    st(fixed)->position = CSS_POSITION_FIXED;
    fixed->flags |= BOXF_POSITIONED | BOXF_OUT_OF_FLOW | BOXF_STACKING_CONTEXT;
    fixed->container = root;
    st(fixed)->background_color = rgba(0xff, 0, 0, 255);
    build(root);
    /* abs (z=2) after fixed (z=0 context); abs inside a clip with folded scroll: frame dy = 400 - 50 */
    /* the scroller's own (empty) content clip is rolled back */
    const int ops[] = {DL_RECT,                                              /* fixed (no clip, no scroll) */
                       DL_PUSH_CLIP_RECT, DL_SCROLL_OFFSET, DL_PUSH_TRANSFORM, DL_RECT, DL_POP_TRANSFORM, DL_POP_CLIP, -1};
    if (expect_ops("scrolled abs", &g_dl, ops)) {
        CHECK(g_dl.items[0].u.rect.color.r == 0xff);
        CHECK_F(g_dl.items[2].u.scroll.dy, -50);
        CHECK_F(g_dl.items[3].u.transform.m.e, 10);
        CHECK_F(g_dl.items[3].u.transform.m.f, 400);
        CHECK_F(g_dl.items[4].u.rect.rect.x, 0);
    }
}

static void test_clip_path(void)
{
    printf("clip-path polygon\n");
    reset_tree(); fresh_ctx();
    Box *root = mk_root();
    Box *b = mk(root, BOX_BLOCK, 10, 20, 200, 100);
    st(b)->background_color = rgba(1, 1, 1, 255);
    CssClipPath *cp = &st(b)->clip_path;
    cp->kind = CSS_CLIP_PATH_POLYGON;
    cp->point_count = 5;
    cp->x[0] = px(0);    cp->y[0] = px(0);
    cp->x[1] = pct(100); cp->y[1] = px(0);
    cp->x[2] = pct(100); cp->y[2] = pct(100);
    cp->x[3] = pct(11);  cp->y[3] = pct(100);
    cp->x[4] = px(0);    cp->y[4] = px(0);
    build(root);
    /* context frame translated to (10,20); polygon in its local space */
    const int ops[] = {DL_PUSH_TRANSFORM, DL_PUSH_CLIP_POLY, DL_RECT, DL_POP_CLIP, DL_POP_TRANSFORM, -1};
    if (expect_ops("clip-path", &g_dl, ops)) {
        const float *pts = g_dl.items[1].u.clip_poly.pts;
        CHECK_F(g_dl.items[0].u.transform.m.e, 10);
        CHECK(g_dl.items[1].u.clip_poly.n == 5);
        CHECK_F(pts[0], 0); CHECK_F(pts[1], 0);
        CHECK_F(pts[2], 200); CHECK_F(pts[5], 100);
        CHECK_F(pts[6], 22); CHECK_F(pts[7], 100);
    }
}

static void test_text_shadow(void)
{
    printf("text run with 4-layer text-shadow\n");
    reset_tree(); fresh_ctx();
    Box *root = mk_root();
    Box *b = mk(root, BOX_BLOCK, 0, 0, 300, 40);
    st(b)->color = rgba(255, 255, 255, 255);
    st(b)->text_shadow.count = 4;
    const float off[4][2] = {{-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
    for (int i = 0; i < 4; i++) {
        st(b)->text_shadow.items[i].x = off[i][0];
        st(b)->text_shadow.items[i].y = off[i][1];
        st(b)->text_shadow.items[i].color = rgba(0, 0, 0, 255);
    }
    st(b)->text_decoration = CSS_TEXT_DECORATION_UNDERLINE;
    mk_text(b, 5, 30, "ab cd", 20);
    build(root);
    const int ops[] = {DL_RECT, DL_TEXT_SHADOW, DL_TEXT_SHADOW, DL_TEXT_SHADOW, DL_TEXT_SHADOW, DL_GLYPHS, -1};
    if (expect_ops("text-shadow", &g_dl, ops)) {
        /* underline under the run, text colour */
        CHECK_F(g_dl.items[0].u.rect.rect.x, 5);
        CHECK_F(g_dl.items[0].u.rect.rect.w, 50);
        CHECK(g_dl.items[0].u.rect.color.r == 255);
        /* last specified shadow painted first; zero-blur layers are their own
         * glyph runs offset before phase selection (dx = dy = 0) */
        CHECK_F(g_dl.items[1].u.text_shadow.dx, 0); CHECK_F(g_dl.items[1].u.text_shadow.dy, 0);
        CHECK_F(g_dl.items[1].u.text_shadow.quads[0].x, 7); CHECK_F(g_dl.items[1].u.text_shadow.quads[0].y, 17);
        CHECK_F(g_dl.items[4].u.text_shadow.quads[0].x, 5); CHECK_F(g_dl.items[4].u.text_shadow.quads[0].y, 15);
        CHECK(g_dl.items[1].u.text_shadow.quads != g_dl.items[5].u.glyphs.quads);
        CHECK(g_dl.items[1].u.text_shadow.n == 4);
        CHECK(g_dl.items[5].u.glyphs.n == 4);  /* space has no ink */
        CHECK_F(g_dl.items[5].u.glyphs.quads[0].x, 6);
        CHECK_F(g_dl.items[5].u.glyphs.quads[0].y, 16);
        CHECK_F(g_dl.items[5].u.glyphs.quads[3].x, 46);
        CHECK_F(g_dl.items[1].u.text_shadow.bounds.x, 7);
        CHECK_F(g_dl.items[1].u.text_shadow.bounds.w, 50);
    }
    /* blurred `#000 0 0 3px` layer */
    st(b)->text_shadow.count = 1;
    st(b)->text_shadow.items[0].x = st(b)->text_shadow.items[0].y = 0;
    st(b)->text_shadow.items[0].blur = 3;
    st(b)->text_decoration = 0;
    b->paint_dirty = 1;
    build(root);
    const int ops2[] = {DL_TEXT_SHADOW, DL_GLYPHS, -1};
    if (expect_ops("blur", &g_dl, ops2)) {
        CHECK_F(g_dl.items[0].u.text_shadow.blur, 3);
        CHECK(g_dl.items[0].u.text_shadow.quads == g_dl.items[1].u.glyphs.quads);   /* blurred: shared quads */
    }

    /* subpixel x: pen 5.3 -> 5.425 -> pixel 5, phase 1; quad x = 5 + x_off */
    g_run.x = 5.3f;
    g_items[0].x = 5.3f;
    b->paint_dirty = 1;
    build(root);
    CHECK(g_dl.n == 2 && fabsf(g_dl.items[1].u.glyphs.quads[0].x - 6.0f) < 1e-3f);

    /* visibility: hidden skips the box but not visible children */
    reset_tree(); fresh_ctx();
    root = mk_root();
    b = mk(root, BOX_BLOCK, 0, 0, 300, 40);
    st(b)->visibility = CSS_VISIBILITY_HIDDEN;
    st(b)->background_color = rgba(1, 1, 1, 255);
    Box *vis = mk(b, BOX_BLOCK, 0, 0, 10, 10);
    st(vis)->background_color = rgba(2, 2, 2, 255);
    build(root);
    const int ops3[] = {DL_RECT, -1};
    if (expect_ops("visibility", &g_dl, ops3)) CHECK(g_dl.items[0].u.rect.color.r == 2);
}

static void test_replaced(void)
{
    printf("replaced images, object-fit cover, canvas\n");
    reset_tree(); fresh_ctx();
    Box *root = mk_root();
    Box *img = mk(root, BOX_IMAGE, 0, 0, 100, 100);
    img->flags |= BOXF_BLOCK_LEVEL;
    st(img)->object_fit = CSS_OBJECT_FIT_COVER;
    img->src = "a.png";     /* 200x80 */
    Box *canvas = mk(root, BOX_CANVAS, 0, 0, 660, 350);
    (void) canvas;
    Box *missing = mk(root, BOX_IMAGE, 0, 0, 10, 10);
    missing->src = "nope.png";
    Box *nosrc = mk(root, BOX_IMAGE, 0, 0, 10, 10);
    (void) nosrc;
    build(root);
    const int ops[] = {DL_IMAGE, -1};
    if (expect_ops("img", &g_dl, ops)) {
        const DlItem *it = &g_dl.items[0];
        CHECK_F(it->u.image.dst.w, 100);
        CHECK_F(it->u.image.dst.h, 100);
        /* cover: scale 1.25 -> 250x100, crop 75px each side horizontally */
        CHECK_F(it->u.image.uv[0], 0.3f);
        CHECK_F(it->u.image.uv[2], 0.7f);
        CHECK_F(it->u.image.uv[1], 0);
        CHECK_F(it->u.image.uv[3], 1);
    }
}

static Box *g_widget_seen;
static float g_widget_x;
static void widget_hook(void *user, const PaintWidgetArgs *a)
{
    DlItem it;
    (void) user;
    g_widget_seen = a->box;
    g_widget_x = a->x;
    memset(&it, 0, sizeof it);
    it.op = DL_GLYPHS;
    it.u.glyphs.quads = (DlGlyphQuad *) paint_alloc(a->ctx, sizeof(DlGlyphQuad));
    it.u.glyphs.n = 1;
    CHECK(it.u.glyphs.quads != NULL);
    dl_push(a->out, &it);
}

static void test_widgets(void)
{
    printf("widget hook\n");
    reset_tree(); fresh_ctx();
    Box *root = mk_root();
    Box *in = mk(root, BOX_INPUT, 30, 0, 129, 20);
    in->flags |= BOXF_BLOCK_LEVEL;
    st(in)->background_color = rgba(5, 5, 5, 255);
    build(root);
    const int ops[] = {DL_RECT, -1};
    expect_ops("input plain", &g_dl, ops);

    g_ctx.host.widget_paint = widget_hook;
    in->paint_dirty = 1;
    build(root);
    const int ops2[] = {DL_RECT, DL_GLYPHS, -1};
    expect_ops("input hook", &g_dl, ops2);
    CHECK(g_widget_seen == in);
    CHECK_F(g_widget_x, 30);
    CHECK(paint_alloc(&g_ctx, 8) == NULL);   /* only valid during recording */
}

static void test_caching(void)
{
    printf("per-context caching\n");
    reset_tree(); fresh_ctx();
    Box *root = mk_root();
    st(root)->background_color = rgba(1, 1, 1, 255);
    Box *a = mk(root, BOX_BLOCK, 0, 0, 10, 10); set_positioned(a, 1); st(a)->background_color = rgba(2, 2, 2, 255);
    Box *a_child = mk(a, BOX_BLOCK, 0, 0, 5, 5); st(a_child)->background_color = rgba(3, 3, 3, 255);
    Box *b = mk(root, BOX_BLOCK, 0, 0, 10, 10); st(b)->opacity = 0.5f; b->flags |= BOXF_STACKING_CONTEXT;
    st(b)->background_color = rgba(4, 4, 4, 255);
    build(root);
    CHECK(g_ctx.stats_contexts == 3);
    CHECK(g_ctx.stats_rebuilt == 3);
    CHECK(g_dl.n == 4);
    DisplayList first = {0};
    for (int i = 0; i < g_dl.n; i++) dl_push(&first, &g_dl.items[i]);

    /* nothing dirty: same output, no rebuild */
    build(root);
    CHECK(g_ctx.stats_rebuilt == 0);
    CHECK(g_dl.n == first.n && memcmp(g_dl.items, first.items, (size_t) g_dl.n * sizeof(DlItem)) == 0);

    /* a leaf inside context `a` dirty: only `a` rebuilt */
    st(a_child)->background_color = rgba(30, 30, 30, 255);
    a_child->paint_dirty = 1;
    build(root);
    CHECK(g_ctx.stats_rebuilt == 1);
    /* order: root, b (opacity context, z 0), a (z 1), a_child */
    CHECK(g_dl.n == 4 && g_dl.items[1].u.rect.color.r == 4 && g_dl.items[3].u.rect.color.r == 30);

    /* z-index change on context root `a`: root reorders (a now first), a itself unchanged */
    set_positioned(a, -1);
    build(root);
    CHECK(g_ctx.stats_rebuilt >= 1);
    CHECK(g_dl.n == 4 && g_dl.items[1].u.rect.color.r == 2 && g_dl.items[3].u.rect.color.r == 4);

    /* explicit mark */
    paint_mark_dirty(&g_ctx, a_child);
    build(root);
    CHECK(g_ctx.stats_rebuilt == 1);

    /* device scale change rebuilds everything */
    g_ctx.device_scale = 2;
    build(root);
    CHECK(g_ctx.stats_rebuilt == 3);

    /* atlas generation change rebuilds everything */
    g_atlas_gen++;
    build(root);
    CHECK(g_ctx.stats_rebuilt == 3);

    /* removing a context evicts its cache; invalidate_all rebuilds */
    root->first_child = b;
    b->prev = NULL;
    build(root);
    CHECK(g_ctx.stats_contexts == 2);
    paint_invalidate_all(&g_ctx);
    build(root);
    CHECK(g_ctx.stats_rebuilt == 2);
    dl_free(&first);
}

static void test_fixed_in_nested_context(void)
{
    printf("fixed inside a nested (non-transformed) context surfaces to the root\n");
    reset_tree(); fresh_ctx();
    Box *root = mk_root();
    Box *scroller = mk(root, BOX_BLOCK, 0, 0, 500, 300);
    set_relative(scroller);
    set_scroller(scroller, 50, 1000, 0);
    Box *wrap = mk(scroller, BOX_BLOCK, 0, 0, 500, 1000);
    st(wrap)->opacity = 0.9f;
    wrap->flags |= BOXF_STACKING_CONTEXT;
    Box *fixed = mk(wrap, BOX_BLOCK, 0, 0, 20, 20);
    st(fixed)->position = CSS_POSITION_FIXED;
    fixed->flags |= BOXF_POSITIONED | BOXF_OUT_OF_FLOW | BOXF_STACKING_CONTEXT;
    fixed->container = root;
    st(fixed)->background_color = rgba(0xff, 0, 0, 255);
    Box *abs = mk(wrap, BOX_BLOCK, 0, 0, 20, 20);
    set_positioned(abs, 2);
    abs->container = scroller;
    st(abs)->background_color = rgba(0, 0xff, 0, 255);
    Box *rel = mk(wrap, BOX_BLOCK, 0, 0, 20, 20);
    set_relative(rel);
    st(rel)->background_color = rgba(0, 0, 0xff, 255);
    build(root);
    int fixed_idx = -1, abs_idx = -1, rel_idx = -1, last_pop = -1;
    for (int i = 0; i < g_dl.n; i++) {
        const DlItem *it = &g_dl.items[i];
        if (it->op == DL_RECT && it->u.rect.color.r == 0xff) fixed_idx = i;
        if (it->op == DL_RECT && it->u.rect.color.g == 0xff) abs_idx = i;
        if (it->op == DL_RECT && it->u.rect.color.b == 0xff) rel_idx = i;
        if (it->op == DL_POP_CLIP) last_pop = i;
    }
    CHECK(fixed_idx > last_pop);            /* painted after the scroller's clip, unclipped */
    CHECK(rel_idx >= 0 && abs_idx > rel_idx); /* z:2 after z:auto inside wrap */
    CHECK(abs_idx < last_pop);
    /* balanced stacks */
    int clip = 0, xf = 0, grp = 0, ok = 1;
    for (int i = 0; i < g_dl.n; i++) {
        switch (g_dl.items[i].op) {
        case DL_PUSH_CLIP_RECT: case DL_PUSH_CLIP_RRECT: case DL_PUSH_CLIP_POLY: clip++; break;
        case DL_POP_CLIP: if (--clip < 0) ok = 0; break;
        case DL_PUSH_TRANSFORM: xf++; break;
        case DL_POP_TRANSFORM: if (--xf < 0) ok = 0; break;
        case DL_PUSH_GROUP: grp++; break;
        case DL_POP_GROUP: if (--grp < 0) ok = 0; break;
        default: break;
        }
    }
    CHECK(ok && clip == 0 && xf == 0 && grp == 0);
    if (!ok) dump(&g_dl);
}

static void test_viewport_overflow(void)
{
    printf("root / body overflow propagates to the viewport\n");
    /* html, body { overflow: hidden }, <html> is 1280x0 (all children positioned) */
    reset_tree(); fresh_ctx();
    Box *root = mk(NULL, BOX_BLOCK, 0, 0, 1280, 0);
    st(root)->overflow_x = st(root)->overflow_y = CSS_OVERFLOW_HIDDEN;
    Box *body = mk(root, BOX_BLOCK, 0, 0, 1280, 0);
    st(body)->overflow_x = st(body)->overflow_y = CSS_OVERFLOW_HIDDEN;
    g_ctx.body = body;
    Box *abs = mk(body, BOX_BLOCK, 10, 10, 100, 100);
    set_positioned(abs, 1);
    abs->container = root;
    st(abs)->background_color = rgba(1, 2, 3, 255);
    build(root);
    /* body still clips its own content (html is not visible), but abs's containing block is root */
    const int ops[] = {DL_PUSH_CLIP_RECT, DL_PUSH_TRANSFORM, DL_RECT, DL_POP_TRANSFORM, DL_POP_CLIP, -1};
    if (expect_ops("root clip", &g_dl, ops)) {
        CHECK_F(g_dl.items[0].u.clip_rect.rect.w, 1280);
        CHECK_F(g_dl.items[0].u.clip_rect.rect.h, 720);
    }

    /* only body has overflow: hidden -> it propagates; body itself does not clip its in-flow child */
    st(root)->overflow_x = st(root)->overflow_y = CSS_OVERFLOW_VISIBLE;
    st(root)->hash_non_inherited ^= 1;
    Box *flow = mk(body, BOX_BLOCK, 0, 0, 50, 50);
    st(flow)->background_color = rgba(9, 9, 9, 255);
    build(root);
    const int ops2[] = {DL_PUSH_CLIP_RECT, DL_RECT, DL_PUSH_TRANSFORM, DL_RECT, DL_POP_TRANSFORM, DL_POP_CLIP, -1};
    if (expect_ops("body propagation", &g_dl, ops2)) {
        CHECK_F(g_dl.items[0].u.clip_rect.rect.h, 720);
        CHECK(g_dl.items[1].u.rect.color.r == 9);
    }
}

static void test_anonymous_borrowed_style(void)
{
    printf("anonymous box borrowing its parent's style paints no decorations / context\n");
    reset_tree(); fresh_ctx();
    Box *root = mk_root();
    /* .reqW: inline-block, relative, translateX(-50%), rgba(0,0,0,.3) */
    Box *card = mk(root, BOX_BLOCK, 100, 100, 693, 217);
    set_relative(card);
    st(card)->background_color = rgba(0, 0, 0, 77);
    st(card)->transform.count = 1;
    st(card)->transform.ops[0].kind = CSS_TRANSFORM_TRANSLATE;
    st(card)->transform.ops[0].tx = pct(-50);
    card->flags |= BOXF_STACKING_CONTEXT | BOXF_HAS_TRANSFORM;
    card->xform = (Mat2D){1, 0, 0, 1, -346.5f, 0};
    /* anonymous block wrapping the <i>, same style pointer and flags as layout produces today */
    Box *anon = mk(card, BOX_BLOCK, 100, 100, 693, 217);
    anon->style = card->style;
    anon->flags |= BOXF_ANONYMOUS | BOXF_POSITIONED | BOXF_STACKING_CONTEXT | BOXF_HAS_TRANSFORM;
    anon->xform = card->xform;
    build(root);
    int bg = 0, xf = 0;
    for (int i = 0; i < g_dl.n; i++) {
        if (g_dl.items[i].op == DL_RECT && g_dl.items[i].u.rect.color.a == 77) bg++;
        if (g_dl.items[i].op == DL_PUSH_TRANSFORM) xf++;
    }
    CHECK(bg == 1);
    CHECK(xf == 2);   /* card frame translate + card transform; nothing for the anonymous box */
    if (bg != 1 || xf != 2) dump(&g_dl);
}

static void test_composited_origin_snap(void)
{
    printf("translateZ(0) / will-change layers: stacking context, origin not snapped\n");
    reset_tree(); fresh_ctx();
    Box *root = mk_root();
    Box *base = mk(root, BOX_BLOCK, 0, 0, 2040, 1148);        /* #uiBase scale(0.627), origin 0 0 */
    st(base)->transform.count = 1;
    st(base)->transform.ops[0].kind = CSS_TRANSFORM_SCALE;
    st(base)->transform.ops[0].sx = st(base)->transform.ops[0].sy = 0.627f;
    st(base)->transform_origin_x = px(0);
    st(base)->transform_origin_y = px(0);
    Box *ammo = mk(base, BOX_BLOCK, 100.3f, 1032.5f, 60, 40);   /* #ammoDisplay: translateZ(0) */
    st(ammo)->transform.has_3d = 1;
    st(ammo)->background_color = rgba(1, 2, 3, 255);
    Box *wc = mk(base, BOX_BLOCK, 10.5f, 20.5f, 10, 10);        /* will-change: transform */
    st(wc)->will_change = CSS_WILL_CHANGE_TRANSFORM;
    st(wc)->background_color = rgba(4, 5, 6, 255);
    CHECK(paint_is_stacking_context(ammo));
    CHECK(paint_is_stacking_context(wc));
    build(root);
    CHECK(g_ctx.stats_contexts == 4);
    int found = 0;
    for (int i = 0; i < g_dl.n; i++) {
        const DlItem *it = &g_dl.items[i];
        if (it->op != DL_PUSH_TRANSFORM || it->u.transform.m.a != 1) continue;
        const float dx = 0.627f * it->u.transform.m.e, dy = 0.627f * it->u.transform.m.f;
        if (fabsf(it->u.transform.m.f - 1032.5f) < 1.0f) {
            found++;
            (void) dx;
            CHECK_F(dy, 0.627f * 1032.5f);   /* sub-pixel origin kept (Chrome subpixel accumulation) */
        }
    }
    CHECK(found == 1);
    if (found != 1) dump(&g_dl);
}

static void test_scaled_scroller_text_snap(void)
{
    printf("scaled body: text baselines rounded in layout space, composited scroller origin snapped\n");
    reset_tree(); fresh_ctx();
    Box *root = mk_root();
    const float k = 0.5791855f;                                  /* the hub's body scale(1280 / 2210) */
    Box *body = mk(root, BOX_BLOCK, 0, 0, 2210, 1243);
    st(body)->transform.count = 1;
    st(body)->transform.ops[0].kind = CSS_TRANSFORM_SCALE;
    st(body)->transform.ops[0].sx = st(body)->transform.ops[0].sy = k;
    st(body)->transform_origin_x = px(0);
    st(body)->transform_origin_y = px(0);
    /* #modsHolder: overflow-y auto with overflow, top 80 -> device 46.33 */
    Box *sc = mk(body, BOX_BLOCK, 150, 80, 1910, 1013);
    set_scroller(sc, 0, 1084, 0);
    Box *line = mk(sc, BOX_BLOCK, 150, 80, 1910, 54);
    mk_text(line, 300, 105.6f, "ab", 20);                        /* fractional layout baseline */
    build(root);
    const int so = find_op(&g_dl, DL_SCROLL_OFFSET, 0), gi = find_op(&g_dl, DL_GLYPHS, 0);
    CHECK(so >= 0 && gi >= 0);
    if (so >= 0 && gi >= 0) {
        /* Chrome: the scroll contents layer is drawn at round(T): contents move by (46 - 46.33) / k */
        CHECK_F(g_dl.items[so].u.scroll.dy, (roundf(80 * k) - 80 * k) / k);
        CHECK_F(g_dl.items[so].u.scroll.dx, (roundf(150 * k) - 150 * k) / k);
        /* baseline 105.6 rounds to 106 before the transform; y_off -14 at k */
        CHECK_F(g_dl.items[gi].u.glyphs.quads[0].y, 106 - 14);
    } else dump(&g_dl);
    /* positioned descendants painted outside the scroller's clip get the same
     * layer snap: an abs box (z-index auto) and a z-indexed one, both at y 300 */
    Box *ab = mk(sc, BOX_BLOCK, 160, 300, 20, 20);
    set_positioned(ab, 0);
    st(ab)->z_index_auto = 1;
    ab->z_auto = 1;
    ab->flags &= ~(uint32_t) BOXF_STACKING_CONTEXT;
    st(ab)->background_color = rgba(0, 0xff, 0, 255);
    Box *az = mk(sc, BOX_BLOCK, 200, 300, 20, 20);
    set_positioned(az, 3);
    st(az)->background_color = rgba(0, 0, 0xff, 255);
    sc->paint_dirty = 1;
    build(root);
    for (int want = 0; want < 2; want++) {
        /* body-space y of the box: rect y + enclosing translations + scroll offsets */
        float ty[32]; int sp = 0; float y = 0; int found = 0;
        ty[0] = 0;
        for (int i = 0; i < g_dl.n && !found; i++) {
            const DlItem *it = &g_dl.items[i];
            const float cur = sp ? ty[sp - 1] : 0;
            /* transforms and clips both scope the in-list transform state */
            if ((it->op == DL_PUSH_TRANSFORM || it->op == DL_PUSH_CLIP_RECT || it->op == DL_PUSH_CLIP_RRECT) && sp < 31)
                ty[sp++] = cur + (it->op == DL_PUSH_TRANSFORM && it->u.transform.m.a == 1 ? it->u.transform.m.f : 0);
            else if ((it->op == DL_POP_TRANSFORM || it->op == DL_POP_CLIP) && sp > 0) sp--;
            else if (it->op == DL_SCROLL_OFFSET && sp > 0) ty[sp - 1] += it->u.scroll.dy;
            else if (it->op == DL_RECT && it->u.rect.color.g == (want ? 0 : 0xff) && it->u.rect.color.b == (want ? 0xff : 0)) {
                y = cur + it->u.rect.rect.y;
                found = 1;
            }
        }
        CHECK(found);
        CHECK_F(y, 300 + (roundf(80 * k) - 80 * k) / k);
        if (!found || fabsf(y - (300 + (roundf(80 * k) - 80 * k) / k)) > 1e-3f) dump(&g_dl);
    }
    ab->flags &= ~(uint32_t) BOXF_POSITIONED;   /* keep the next cases about the scroller only */
    st(ab)->background_color = rgba(0, 0, 0, 0);
    st(az)->background_color = rgba(0, 0, 0, 0);
    /* no overflow -> stays in its parent layer: no snap, no scroll item */
    sc->scroll_h = 1013;
    sc->paint_dirty = 1;
    build(root);
    CHECK(find_op(&g_dl, DL_SCROLL_OFFSET, 0) < 0);
    /* ...unless it holds a sticky box (win-servers' .serverHeader): layerized, snapped */
    st(line)->position = CSS_POSITION_STICKY;
    sc->paint_dirty = 1;
    build(root);
    {
        const int so2 = find_op(&g_dl, DL_SCROLL_OFFSET, 0);
        CHECK(so2 >= 0 && fabsf(g_dl.items[so2].u.scroll.dy - (roundf(80 * k) - 80 * k) / k) < 1e-3f);
    }
    st(line)->position = CSS_POSITION_STATIC;
    /* overflow hidden is not composited either */
    sc->scroll_h = 1084;
    st(sc)->overflow_y = CSS_OVERFLOW_HIDDEN;
    sc->paint_dirty = 1;
    build(root);
    CHECK(find_op(&g_dl, DL_SCROLL_OFFSET, 0) < 0);
}

static void test_atlas_reset_while_recording(void)
{
    printf("glyph atlas rebuilt mid-recording: no quad of the old generation survives\n");
    reset_tree(); fresh_ctx();
    Box *root = mk_root();
    Box *line = mk(root, BOX_BLOCK, 0, 0, 200, 30);
    mk_text(line, 10, 20, "abc", 20);
    build(root);
    CHECK(find_op(&g_dl, DL_GLYPHS, 0) >= 0);
    /* the next paint's 'b' fills the atlas: 'a' was already resolved against the old one */
    g_reset_at_glyph = 'b';
    line->paint_dirty = 1;
    const uint32_t before = g_atlas_gen;
    build(root);
    CHECK(g_atlas_gen == before + 1);
    CHECK(g_ctx.atlas_generation == g_atlas_gen);
    const int gi = find_op(&g_dl, DL_GLYPHS, 0);
    CHECK(gi >= 0 && g_dl.items[gi].u.glyphs.n == 3);
    /* 'a' was resolved before the reset: its uvs must be re-requested too */
    for (int i = 0; gi >= 0 && i < g_dl.items[gi].u.glyphs.n; i++)
        CHECK_F(g_dl.items[gi].u.glyphs.quads[i].uv[0], 0.1f + 0.001f * (float) (g_atlas_gen & 63));
    /* nothing dirty: the list is current, no rebuild */
    build(root);
    CHECK(g_ctx.stats_rebuilt == 0);
}

static void test_url_base_and_cutout(void)
{
    printf("per-layer url() base; viewport cut-out shadow on whole device px\n");
    reset_tree(); fresh_ctx();
    g_ctx.doc_base_url = "index.html";
    Box *root = mk_root();
    Box *b = mk(root, BOX_BLOCK, 0, 0, 40, 40);
    st(b)->bg_layer_count = 1;
    st(b)->bg_layers[0].image.kind = CSS_IMAGE_URL;
    st(b)->bg_layers[0].image.url = "c.png";
    st(b)->bg_layers[0].image.base = "css/main.css";      /* declared in a sheet */
    st(b)->bg_layers[0].repeat_x = st(b)->bg_layers[0].repeat_y = CSS_BG_REPEAT_NO_REPEAT;
    st(b)->bg_layers[0].size.w = st(b)->bg_layers[0].size.h = len_auto();
    build(root);
    CHECK(g_last_image_base && !strcmp(g_last_image_base, "css/main.css"));
    st(b)->bg_layers[0].image.base = NULL;                 /* inline style="": the document */
    b->paint_dirty = 1;
    build(root);
    CHECK(g_last_image_base && !strcmp(g_last_image_base, "index.html"));

    /* #knPreviewHole inside #uiBase scale(0.627): cut-out edges on whole device px */
    reset_tree(); fresh_ctx();
    root = mk_root();
    Box *base = mk(root, BOX_BLOCK, 0, 0, 2040, 1148);
    st(base)->transform.count = 1;
    st(base)->transform.ops[0].kind = CSS_TRANSFORM_SCALE;
    st(base)->transform.ops[0].sx = st(base)->transform.ops[0].sy = 0.627f;
    st(base)->transform_origin_x = px(0);
    st(base)->transform_origin_y = px(0);
    Box *hole = mk(base, BOX_BLOCK, 1535.3f, 423.7f, 526.4f, 279.1f);
    st(hole)->position = CSS_POSITION_ABSOLUTE;
    hole->flags |= BOXF_POSITIONED | BOXF_OUT_OF_FLOW;
    st(hole)->box_shadow_count = 1;
    st(hole)->box_shadow.spread = 4000;
    st(hole)->box_shadow.color = rgba(0, 0, 0, 128);
    build(root);
    const int i = find_op(&g_dl, DL_BOX_SHADOW, 0);
    CHECK(i >= 0);
    if (i >= 0) {
        const DlRect r = g_dl.items[i].u.box_shadow.rect;
        const float x0 = 0.627f * r.x, y0 = 0.627f * r.y, w = 0.627f * r.w, h = 0.627f * r.h;
        CHECK(fabsf(x0 - roundf(x0)) < 1e-3f && fabsf(y0 - roundf(y0)) < 1e-3f);
        CHECK(fabsf(w - roundf(w)) < 1e-3f && fabsf(h - roundf(h)) < 1e-3f);
        CHECK_F(roundf(x0), roundf(0.627f * 1535.3f));    /* same rule as client_class_preview: lroundf(x) */
        CHECK_F(roundf(w), roundf(0.627f * 526.4f));      /* and lroundf(w) */
    }
}

static void test_three_dot_ellipsis(void)
{
    printf("ellipsis: U+2026 or Blink's three-period fallback\n");
    reset_tree(); fresh_ctx();
    Box *root = mk_root();
    Box *b = mk(root, BOX_BLOCK, 0, 0, 100, 40);
    mk_text(b, 0, 30, "Spray", 20);
    g_run.ellipsis = 1;
    g_run.ellipsis_x = 50;
    g_run.ellipsis_count = 3;
    for (int i = 0; i < 3; i++) {
        memset(&g_run.ellipsis_glyphs[i], 0, sizeof(Glyph));
        g_run.ellipsis_glyphs[i].glyph_index = '.';
        g_run.ellipsis_glyphs[i].x = 4.0f * (float) i;
    }
    g_run.ellipsis_glyph = g_run.ellipsis_glyphs[0];
    build(root);
    const int i = find_op(&g_dl, DL_GLYPHS, 0);
    CHECK(i >= 0 && g_dl.items[i].u.glyphs.n == 5 + 3);
    if (i >= 0 && g_dl.items[i].u.glyphs.n == 8) {
        CHECK_F(g_dl.items[i].u.glyphs.quads[5].x, 51);
        CHECK_F(g_dl.items[i].u.glyphs.quads[6].x, 55);
        CHECK_F(g_dl.items[i].u.glyphs.quads[7].x, 59);
    }
    /* older runs: one U+2026 glyph at ellipsis_x */
    g_run.ellipsis_count = 0;
    b->paint_dirty = 1;
    build(root);
    const int j = find_op(&g_dl, DL_GLYPHS, 0);
    CHECK(j >= 0 && g_dl.items[j].u.glyphs.n == 6);
    g_run.ellipsis = 0;
}

/* .hostOpt > span.optName { position: relative; z-index: 1 } inside the
 * scrolled #menuWindow: the inline stacking context's glyphs must scroll
 * exactly once (with the parent's SCROLL_OFFSET), like the boxes. */
static void test_inline_context_in_scroller(void)
{
    printf("inline stacking context text inside a scrolled container\n");
    float qy[2] = {0, 0}, qx[2] = {0, 0}, sy[2] = {0, 0}, fy[2] = {0, 0};
    for (int pass = 0; pass < 2; pass++) {
        reset_tree(); fresh_ctx();
        Box *root = mk_root();
        Box *sc = mk(root, BOX_BLOCK, 100, 100, 400, 300);
        set_scroller(sc, pass ? 120.0f : 0.0f, 1000, 0);
        Box *blk = mk(sc, BOX_BLOCK, 100, 500, 200, 40);       /* label.hostOpt (block container) */
        st(blk)->background_color = rgba(0x86, 0x86, 0x86, 255);
        Box *span = mk(blk, BOX_INLINE, 110, 510, 80, 20);     /* span.optName */
        set_relative(span);
        st(span)->z_index_auto = 0; st(span)->z_index = 1;
        span->z_auto = 0; span->z_index = 1;
        span->flags |= BOXF_STACKING_CONTEXT | BOXF_INLINE_LEVEL;
        span->container = blk;
        static BoxFrag frag;
        frag.x = 110; frag.y = 510; frag.w = 80; frag.h = 20; frag.first = frag.last = 1;
        span->frags = &frag; span->frag_count = 1;
        st(span)->color = rgba(255, 255, 255, 255);
        Box *t = mk_text(span, 110, 526, "Free", 16);         /* lines land on span: move them to blk */
        t->container = blk;
        blk->lines = span->lines; blk->line_count = span->line_count; blk->flags |= BOXF_HAS_LINES;
        span->lines = NULL; span->line_count = 0; span->flags &= ~(uint32_t) BOXF_HAS_LINES;
        build(root);
        int so = find_op(&g_dl, DL_SCROLL_OFFSET, 0), gl = find_op(&g_dl, DL_GLYPHS, 0);
        CHECK(gl >= 0);
        if (pass) CHECK(so >= 0 && so < gl);
        if (gl < 0) { dump(&g_dl); return; }
        if (so >= 0) sy[pass] = g_dl.items[so].u.scroll.dy;
        qx[pass] = g_dl.items[gl].u.glyphs.quads[0].x;
        qy[pass] = g_dl.items[gl].u.glyphs.quads[0].y;
        /* the span frame translate just before its glyphs */
        for (int i = gl; i >= 0; i--)
            if (g_dl.items[i].op == DL_PUSH_TRANSFORM) { fy[pass] = g_dl.items[i].u.transform.m.f; break; }
    }
    /* quads are frame-local and the frame placement is unscrolled: the only
     * difference between the two builds is the single SCROLL_OFFSET */
    CHECK_F(sy[1], -120);
    CHECK_F(qx[0], qx[1]);
    CHECK_F(qy[0], qy[1]);
    CHECK_F(fy[0], fy[1]);
    CHECK_F(qy[1], 526 - 510 - 14.0f * 16.0f / 20.0f);   /* baseline in the span frame; stub y_off scales with size */
}

int main(void)
{
    paint_ctx_init(&g_ctx);
    test_background_radius_clip();
    test_border_triangle();
    test_box_shadow();
    test_two_layer_background();
    test_z_order();
    test_opacity();
    test_transform();
    test_scroll_container();
    test_scrolled_positioned();
    test_clip_path();
    test_text_shadow();
    test_replaced();
    test_widgets();
    test_caching();
    test_fixed_in_nested_context();
    test_viewport_overflow();
    test_anonymous_borrowed_style();
    test_composited_origin_snap();
    test_scaled_scroller_text_snap();
    test_atlas_reset_while_recording();
    test_url_base_and_cutout();
    test_three_dot_ellipsis();
    test_inline_context_in_scroller();
    paint_ctx_free(&g_ctx);
    dl_free(&g_dl);
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
