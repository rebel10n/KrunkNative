/*
 * test_cascade.c - the cascade on Krunker v7.1.4's menu: loads
 * tests/data/v7-menu-1280.html (Chrome's live menu DOM) with ui/index.html's ten
 * <link> sheets, the page's <style> blocks and the UA sheet, then
 *  - compares every value in tests/data/v7-menu-1280.computed: Chrome's
 *    getComputedStyle of each element with an id, and the changes forcing :hover
 *    makes (written by tests/capture_computed.js; Chrome's serialization is
 *    rebuilt from CssComputed, used-value lengths are skipped, known engine gaps
 *    are counted, not failed),
 *  - checks class / pseudo-class / inline-style / tree changes, generated markup
 *    and a stylesheet added at run time (ranked-kn.css) against Chrome's values
 *    after the same changes,
 *  - times a full restyle and the restyle after innerHTML of the host-game window
 *    (ui/windows/8.html).
 *
 * Build and run from the repository root:
 *   cc -std=c11 -g -Iclient/src/html/engine -Iclient/include -Iclient/lib/lexbor \
 *      client/src/html/engine/tests/test_cascade.c client/src/html/engine/dom.c \
 *      client/src/html/engine/css_sheet.c client/src/html/engine/css_cascade.c \
 *      client/src/html/engine/css_values.c client/src/html/engine/util.c \
 *      client/lib/lexbor/lexbor.c -lm -o test_cascade && ./test_cascade .
 * -O2 -DKN_OPTIMIZED adds the timing checks; KN_TEST_VERBOSE=1 lists the known gaps.
 * After a sheet or page change: node client/src/html/engine/tests/capture_computed.js
 *
 * Usage: test_cascade [repo root]   (default: the compile-time KN_REPO_ROOT)
 * Exit code: number of failed checks.
 */
#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "css_cascade.h"
#include "css_sheet.h"
#include "css_values.h"
#include "dom.h"
#include "ua_css.h"

#ifndef KN_REPO_ROOT
#define KN_REPO_ROOT "."
#endif

static int g_fail = 0;
static int g_pass = 0;
static const char *g_root = KN_REPO_ROOT;

static Dom *g_dom;
static CssSheet *g_ua;
/* ui/index.html's <link> sheets in order, then the page's own <style> blocks; names
 * are relative to ui/ (the url() bases). */
#define MAX_SHEETS 14   /* css_cascade's author sheet limit */
static CssSheet *g_sheets[MAX_SHEETS];
static int g_nsheets;
static const char *const k_sheet_files[] = {
    "css/main.css", "css/bundled/bundledStyles.min.css", "css/main_custom.css", "css/flag.css",
    "css/material-icons.css", "css/material-icons-outlined.css", "css/material-symbols-outlined.css",
    "css/google-play.css", "css/krunknative.css", "css/hud.css",
};
#define N_SHEET_FILES ((int)(sizeof(k_sheet_files) / sizeof(k_sheet_files[0])))

/* ------------------------------------------------------------------------- */
/* Files / timing                                                             */
/* ------------------------------------------------------------------------- */

static char *read_file(const char *rel, size_t *len)
{
    char path[1024];
    FILE *f;
    long n;
    char *buf;
    snprintf(path, sizeof(path), "%s/%s", g_root, rel);
    f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "cannot open %s\n", path);
        exit(99);
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (char *)malloc((size_t)n + 1);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "short read %s\n", path);
        exit(99);
    }
    buf[n] = '\0';
    fclose(f);
    if (len != NULL) {
        *len = (size_t)n;
    }
    return buf;
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

/* ------------------------------------------------------------------------- */
/* Computed value formatting                                                  */
/* ------------------------------------------------------------------------- */

static void fmt_num(char *out, size_t cap, float v)
{
    float r = roundf(v * 1000.0f) / 1000.0f;
    snprintf(out, cap, "%g", (double)(r == 0.0f ? 0.0f : r));
}

static void fmt_length(const CssLength *l, char *out, size_t cap)
{
    char a[32], b[32];
    switch (l->kind) {
    case CSS_LEN_AUTO: snprintf(out, cap, "auto"); return;
    case CSS_LEN_NONE: snprintf(out, cap, "none"); return;
    case CSS_LEN_NORMAL: snprintf(out, cap, "normal"); return;
    case CSS_LEN_MIN_CONTENT: snprintf(out, cap, "min-content"); return;
    case CSS_LEN_MAX_CONTENT: snprintf(out, cap, "max-content"); return;
    case CSS_LEN_FIT_CONTENT: snprintf(out, cap, "fit-content"); return;
    case CSS_LEN_NUMBER: fmt_num(a, sizeof(a), l->px); snprintf(out, cap, "%s", a); return;
    default: break;
    }
    fmt_num(a, sizeof(a), l->px);
    fmt_num(b, sizeof(b), l->pct);
    if (l->pct == 0.0f) {
        snprintf(out, cap, "%spx", a);
    } else if (l->px == 0.0f) {
        snprintf(out, cap, "%s%%", b);
    } else {
        snprintf(out, cap, "calc(%s%% %c %spx)", b, l->px < 0 ? '-' : '+', a[0] == '-' ? a + 1 : a);
    }
}

static void fmt_color(const CssColor *c, char *out, size_t cap)
{
    if (c->kind == CSS_COLOR_CURRENT) {
        snprintf(out, cap, "currentcolor");
    } else if (c->a == 255) {
        snprintf(out, cap, "rgb(%d, %d, %d)", c->r, c->g, c->b);
    } else {
        char a[32];
        fmt_num(a, sizeof(a), roundf((float)c->a / 255.0f * 100.0f) / 100.0f);
        snprintf(out, cap, "rgba(%d, %d, %d, %s)", c->r, c->g, c->b, a);
    }
}

static const char *kw(const char *const *names, int n, int v)
{
    return (v >= 0 && v < n) ? names[v] : "?";
}

static const char *const k_display[] = { "inline", "block", "none", "inline-block", "flex", "inline-flex", "grid", "inline-grid", "table", "inline-table", "table-row", "table-cell", "table-row-group", "list-item", "contents" };
static const char *const k_position[] = { "static", "relative", "absolute", "fixed", "sticky" };
static const char *const k_float[] = { "none", "left", "right" };
static const char *const k_clear[] = { "none", "left", "right", "both" };
static const char *const k_overflow[] = { "visible", "hidden", "scroll", "auto", "clip" };
static const char *const k_pointer[] = { "auto", "none", "all" };
static const char *const k_cursor[] = { "auto", "default", "pointer", "none", "crosshair", "text", "move", "not-allowed", "grab", "grabbing", "wait", "help" };
static const char *const k_user_select[] = { "auto", "none", "text", "all" };
static const char *const k_direction[] = { "ltr", "rtl" };
static const char *const k_appearance[] = { "auto", "none", "textfield", "menulist", "button" };
static const char *const k_box_sizing[] = { "content-box", "border-box" };
static const char *const k_text_align[] = { "start", "left", "right", "center", "justify", "end" };
static const char *const k_text_transform[] = { "none", "uppercase", "lowercase", "capitalize" };
static const char *const k_white_space[] = { "normal", "nowrap", "pre", "pre-wrap", "pre-line" };
static const char *const k_valign[] = { "baseline", "top", "middle", "bottom", "sub", "super", "text-top", "text-bottom", "length" };
static const char *const k_image_rendering[] = { "auto", "pixelated", "crisp-edges", "smooth" };
static const char *const k_flex_direction[] = { "row", "row-reverse", "column", "column-reverse" };
static const char *const k_justify[] = { "flex-start", "flex-end", "center", "space-between", "space-around", "space-evenly" };
static const char *const k_align_items[] = { "stretch", "flex-start", "flex-end", "center", "baseline" };
static const char *const k_border_style[] = { "none", "hidden", "solid", "dashed", "dotted", "double", "groove", "ridge", "inset", "outset" };
static const char *const k_font_style[] = { "normal", "italic", "oblique" };
static const char *const k_visibility[] = { "visible", "hidden", "collapse" };
static const char *const k_text_overflow[] = { "clip", "ellipsis" };
static const char *const k_object_fit[] = { "fill", "contain", "cover", "none", "scale-down" };
static const char *const k_anim_dir[] = { "normal", "reverse", "alternate", "alternate-reverse" };
static const char *const k_anim_fill[] = { "none", "forwards", "backwards", "both" };
static const char *const k_bg_repeat[] = { "repeat", "no-repeat", "round", "space" };
static const char *const k_timing[] = { "ease", "linear", "ease-in", "ease-out", "ease-in-out", "cubic-bezier", "steps" };

#define KW(tab, v) kw(tab, (int)(sizeof(tab) / sizeof(tab[0])), (v))

static void fmt_shadow(const CssShadow *s, int box, char *out, size_t cap)
{
    char x[32], y[32], b[32], sp[32], col[64];
    fmt_num(x, sizeof(x), s->x);
    fmt_num(y, sizeof(y), s->y);
    fmt_num(b, sizeof(b), s->blur);
    fmt_num(sp, sizeof(sp), s->spread);
    fmt_color(&s->color, col, sizeof(col));
    if (box) {
        snprintf(out, cap, "%s%spx %spx %spx %spx %s", s->inset ? "inset " : "", x, y, b, sp, col);
    } else {
        snprintf(out, cap, "%spx %spx %spx %s", x, y, b, col);
    }
}

static void fmt_transform(const CssTransform *t, char *out, size_t cap)
{
    size_t o = 0;
    if (t->count == 0) {
        snprintf(out, cap, "none");
        return;
    }
    out[0] = '\0';
    for (int i = 0; i < t->count; i++) {
        const CssTransformOp *op = &t->ops[i];
        char a[64], b[64];
        if (i > 0 && o < cap) {
            o += (size_t)snprintf(out + o, cap - o, " ");
        }
        switch (op->kind) {
        case CSS_TRANSFORM_TRANSLATE:
            fmt_length(&op->tx, a, sizeof(a));
            fmt_length(&op->ty, b, sizeof(b));
            o += (size_t)snprintf(out + o, cap - o, "translate(%s, %s)", a, b);
            break;
        case CSS_TRANSFORM_SCALE:
            fmt_num(a, sizeof(a), op->sx);
            fmt_num(b, sizeof(b), op->sy);
            o += (size_t)snprintf(out + o, cap - o, "scale(%s, %s)", a, b);
            break;
        case CSS_TRANSFORM_ROTATE:
            fmt_num(a, sizeof(a), op->angle_deg);
            o += (size_t)snprintf(out + o, cap - o, "rotate(%sdeg)", a);
            break;
        default:
            o += (size_t)snprintf(out + o, cap - o, "op%d", op->kind);
            break;
        }
    }
}

static void fmt_timing(const CssTiming *t, char *out, size_t cap)
{
    if (t->kind == CSS_TIMING_STEPS) {
        snprintf(out, cap, "steps(%d)", t->steps);
    } else if (t->kind == CSS_TIMING_CUBIC_BEZIER) {
        char a[32], b[32], c[32], d[32];
        fmt_num(a, sizeof(a), t->x1);
        fmt_num(b, sizeof(b), t->y1);
        fmt_num(c, sizeof(c), t->x2);
        fmt_num(d, sizeof(d), t->y2);
        snprintf(out, cap, "cubic-bezier(%s, %s, %s, %s)", a, b, c, d);
    } else {
        snprintf(out, cap, "%s", KW(k_timing, t->kind));
    }
}

/* Formats one property of a computed record into `out`.  Returns 0 for an
 * unknown property name. */
static int fmt_prop(const CssComputed *c, const char *prop, char *out, size_t cap)
{
    char a[96], b[96], d[192], e[96];
#define LEN(field) do { fmt_length(&(c->field), out, cap); return 1; } while (0)
#define COL(field) do { fmt_color(&(c->field), out, cap); return 1; } while (0)
#define KWD(tab, field) do { snprintf(out, cap, "%s", KW(tab, c->field)); return 1; } while (0)
#define NUM(field) do { fmt_num(out, cap, c->field); return 1; } while (0)
    if (!strcmp(prop, "color")) COL(color);
    if (!strcmp(prop, "background-color")) COL(background_color);
    if (!strcmp(prop, "font-size")) { fmt_num(a, sizeof(a), c->font_size); snprintf(out, cap, "%spx", a); return 1; }
    if (!strcmp(prop, "font-weight")) { snprintf(out, cap, "%d", c->font_weight); return 1; }
    if (!strcmp(prop, "font-style")) KWD(k_font_style, font_style);
    if (!strcmp(prop, "font-family")) {
        size_t o = 0;
        out[0] = '\0';
        for (int i = 0; i < c->font_family_count && o < cap; i++) {
            o += (size_t)snprintf(out + o, cap - o, "%s%s", i ? ", " : "", c->font_families[i]);
        }
        return 1;
    }
    if (!strcmp(prop, "line-height")) LEN(line_height);
    if (!strcmp(prop, "letter-spacing")) LEN(letter_spacing);
    if (!strcmp(prop, "text-align")) KWD(k_text_align, text_align);
    if (!strcmp(prop, "text-transform")) KWD(k_text_transform, text_transform);
    if (!strcmp(prop, "white-space")) KWD(k_white_space, white_space);
    if (!strcmp(prop, "visibility")) KWD(k_visibility, visibility);
    if (!strcmp(prop, "cursor")) KWD(k_cursor, cursor);
    if (!strcmp(prop, "pointer-events")) KWD(k_pointer, pointer_events);
    if (!strcmp(prop, "user-select")) KWD(k_user_select, user_select);
    if (!strcmp(prop, "direction")) KWD(k_direction, direction);
    if (!strcmp(prop, "image-rendering")) KWD(k_image_rendering, image_rendering);
    if (!strcmp(prop, "text-shadow")) {
        if (c->text_shadow.count == 0) {
            snprintf(out, cap, "none");
        } else {
            fmt_shadow(&c->text_shadow.items[0], 0, a, sizeof(a));
            snprintf(out, cap, "%d layers; first %s", c->text_shadow.count, a);
        }
        return 1;
    }
    if (prop[0] == '-' && prop[1] == '-') {
        const char *v = css_computed_custom_get(c, prop, strlen(prop));
        snprintf(out, cap, "%s", v != NULL ? v : "(unset)");
        return 1;
    }
    if (!strcmp(prop, "display")) KWD(k_display, display);
    if (!strcmp(prop, "position")) KWD(k_position, position);
    if (!strcmp(prop, "float")) KWD(k_float, float_);
    if (!strcmp(prop, "clear")) KWD(k_clear, clear);
    if (!strcmp(prop, "box-sizing")) KWD(k_box_sizing, box_sizing);
    if (!strcmp(prop, "overflow-x")) KWD(k_overflow, overflow_x);
    if (!strcmp(prop, "overflow-y")) KWD(k_overflow, overflow_y);
    if (!strcmp(prop, "appearance")) KWD(k_appearance, appearance);
    if (!strcmp(prop, "z-index")) {
        if (c->z_index_auto) {
            snprintf(out, cap, "auto");
        } else {
            snprintf(out, cap, "%d", (int)c->z_index);
        }
        return 1;
    }
    if (!strcmp(prop, "opacity")) NUM(opacity);
    if (!strcmp(prop, "top")) LEN(inset[CSS_TOP]);
    if (!strcmp(prop, "right")) LEN(inset[CSS_RIGHT]);
    if (!strcmp(prop, "bottom")) LEN(inset[CSS_BOTTOM]);
    if (!strcmp(prop, "left")) LEN(inset[CSS_LEFT]);
    if (!strcmp(prop, "width")) LEN(width);
    if (!strcmp(prop, "height")) LEN(height);
    if (!strcmp(prop, "min-width")) LEN(min_width);
    if (!strcmp(prop, "min-height")) LEN(min_height);
    if (!strcmp(prop, "max-width")) LEN(max_width);
    if (!strcmp(prop, "max-height")) LEN(max_height);
    if (!strcmp(prop, "margin-top")) LEN(margin[CSS_TOP]);
    if (!strcmp(prop, "margin-right")) LEN(margin[CSS_RIGHT]);
    if (!strcmp(prop, "margin-bottom")) LEN(margin[CSS_BOTTOM]);
    if (!strcmp(prop, "margin-left")) LEN(margin[CSS_LEFT]);
    if (!strcmp(prop, "padding-top")) LEN(padding[CSS_TOP]);
    if (!strcmp(prop, "padding-right")) LEN(padding[CSS_RIGHT]);
    if (!strcmp(prop, "padding-bottom")) LEN(padding[CSS_BOTTOM]);
    if (!strcmp(prop, "padding-left")) LEN(padding[CSS_LEFT]);
    if (!strcmp(prop, "border-left-width")) { fmt_num(a, sizeof(a), c->border_width[CSS_LEFT]); snprintf(out, cap, "%spx", a); return 1; }
    if (!strcmp(prop, "border-top-width")) { fmt_num(a, sizeof(a), c->border_width[CSS_TOP]); snprintf(out, cap, "%spx", a); return 1; }
    if (!strcmp(prop, "border-bottom-width")) { fmt_num(a, sizeof(a), c->border_width[CSS_BOTTOM]); snprintf(out, cap, "%spx", a); return 1; }
    if (!strcmp(prop, "border-left-style")) KWD(k_border_style, border_style[CSS_LEFT]);
    if (!strcmp(prop, "border-top-style")) KWD(k_border_style, border_style[CSS_TOP]);
    if (!strcmp(prop, "border-bottom-style")) KWD(k_border_style, border_style[CSS_BOTTOM]);
    if (!strcmp(prop, "border-left-color")) COL(border_color[CSS_LEFT]);
    if (!strcmp(prop, "border-bottom-color")) COL(border_color[CSS_BOTTOM]);
    if (!strcmp(prop, "border-top-left-radius")) LEN(border_radius[CSS_TOP_LEFT]);
    if (!strcmp(prop, "border-bottom-right-radius")) LEN(border_radius[CSS_BOTTOM_RIGHT]);
    if (!strcmp(prop, "outline-style")) KWD(k_border_style, outline_style);
    if (!strcmp(prop, "background-image")) {
        if (c->bg_layer_count == 0) {
            snprintf(out, cap, "none");
        } else if (c->bg_layers[0].image.kind == CSS_IMAGE_URL) {
            snprintf(out, cap, "url(%s)", c->bg_layers[0].image.url);
        } else if (c->bg_layers[0].image.kind == CSS_IMAGE_GRADIENT) {
            snprintf(out, cap, "%s-gradient(%d stops)", c->bg_layers[0].image.gradient.kind == CSS_GRADIENT_LINEAR ? "linear" : "radial",
                     c->bg_layers[0].image.gradient.stop_count);
        } else {
            snprintf(out, cap, "none");
        }
        return 1;
    }
    if (!strcmp(prop, "background-size")) {
        const CssBgSize *s = &c->bg_layers[0].size;
        if (s->kind == CSS_BG_SIZE_COVER) {
            snprintf(out, cap, "cover");
        } else if (s->kind == CSS_BG_SIZE_CONTAIN) {
            snprintf(out, cap, "contain");
        } else {
            fmt_length(&s->w, a, sizeof(a));
            fmt_length(&s->h, b, sizeof(b));
            snprintf(out, cap, "%s %s", a, b);
        }
        return 1;
    }
    if (!strcmp(prop, "background-repeat")) { snprintf(out, cap, "%s", KW(k_bg_repeat, c->bg_layers[0].repeat_x)); return 1; }
    if (!strcmp(prop, "transform")) { fmt_transform(&c->transform, out, cap); return 1; }
    if (!strcmp(prop, "transform-origin")) {
        fmt_length(&c->transform_origin_x, a, sizeof(a));
        fmt_length(&c->transform_origin_y, b, sizeof(b));
        snprintf(out, cap, "%s %s", a, b);
        return 1;
    }
    if (!strcmp(prop, "box-shadow")) {
        if (c->box_shadow_count == 0) {
            snprintf(out, cap, "none");
        } else {
            fmt_shadow(&c->box_shadow, 1, out, cap);
        }
        return 1;
    }
    if (!strcmp(prop, "filter")) {
        if (c->filter_count == 0) {
            snprintf(out, cap, "none");
        } else {
            fmt_num(a, sizeof(a), c->filters[0].amount);
            snprintf(out, cap, "%d ops; first kind %d amount %s", c->filter_count, c->filters[0].kind, a);
        }
        return 1;
    }
    if (!strcmp(prop, "clip-path")) { snprintf(out, cap, c->clip_path.kind == CSS_CLIP_PATH_NONE ? "none" : "polygon(%d points)", c->clip_path.point_count); return 1; }
    if (!strcmp(prop, "object-fit")) KWD(k_object_fit, object_fit);
    if (!strcmp(prop, "content")) {
        switch (c->content.kind) {
        case CSS_CONTENT_NONE: snprintf(out, cap, "none"); break;
        case CSS_CONTENT_STRING: snprintf(out, cap, "\"%s\"", c->content.str != NULL ? c->content.str : ""); break;
        case CSS_CONTENT_ATTR: snprintf(out, cap, "attr(%s)", c->content.str != NULL ? c->content.str : ""); break;
        default: snprintf(out, cap, "normal"); break;
        }
        return 1;
    }
    if (!strcmp(prop, "text-decoration")) {
        snprintf(out, cap, "%s", c->text_decoration == 0 ? "none" : (c->text_decoration & CSS_TEXT_DECORATION_UNDERLINE) ? "underline" : "other");
        return 1;
    }
    if (!strcmp(prop, "text-overflow")) KWD(k_text_overflow, text_overflow);
    if (!strcmp(prop, "vertical-align")) KWD(k_valign, vertical_align);
    if (!strcmp(prop, "flex-direction")) KWD(k_flex_direction, flex_direction);
    if (!strcmp(prop, "justify-content")) KWD(k_justify, justify_content);
    if (!strcmp(prop, "align-items")) KWD(k_align_items, align_items);
    if (!strcmp(prop, "flex-grow")) NUM(flex_grow);
    if (!strcmp(prop, "grid-template-columns")) {
        size_t o = 0;
        if (c->grid_column_count == 0) {
            snprintf(out, cap, "none");
            return 1;
        }
        out[0] = '\0';
        for (int i = 0; i < c->grid_column_count && o < cap; i++) {
            fmt_length(&c->grid_columns[i].size, a, sizeof(a));
            o += (size_t)snprintf(out + o, cap - o, "%s%s", i ? " " : "", a);
        }
        return 1;
    }
    if (!strcmp(prop, "row-gap")) LEN(row_gap);
    if (!strcmp(prop, "animation")) {
        const CssAnimation *an = &c->animations[0];
        if (c->animation_count == 0 || an->name == NULL) {
            snprintf(out, cap, "none");
            return 1;
        }
        fmt_num(a, sizeof(a), an->duration_s);
        fmt_num(b, sizeof(b), an->delay_s);
        fmt_timing(&an->timing, d, sizeof(d));
        if (isinf(an->iterations)) {
            snprintf(e, sizeof(e), "infinite");
        } else {
            fmt_num(e, sizeof(e), an->iterations);
        }
        snprintf(out, cap, "%s %ss %s %ss %s %s %s", an->name, a, d, b, e, KW(k_anim_dir, an->direction), KW(k_anim_fill, an->fill));
        return 1;
    }
    if (!strcmp(prop, "animation-delay")) { fmt_num(a, sizeof(a), c->animations[0].delay_s); snprintf(out, cap, "%ss", a); return 1; }
    if (!strcmp(prop, "transition")) {
        const CssTransition *t = &c->transitions[0];
        if (c->transition_count == 0) {
            snprintf(out, cap, "none");
            return 1;
        }
        fmt_num(a, sizeof(a), t->duration_s);
        fmt_timing(&t->timing, d, sizeof(d));
        snprintf(out, cap, "%s %ss %s", t->prop == CSS_PROP_ALL ? "all" : t->prop >= 0 ? css_prop_name(t->prop) : "none", a, d);
        return 1;
    }
    return 0;
#undef LEN
#undef COL
#undef KWD
#undef NUM
}

/* ------------------------------------------------------------------------- */
/* Checks                                                                     */
/* ------------------------------------------------------------------------- */

static void check_str(const char *what, const char *got, const char *expected)
{
    if (got != NULL && expected != NULL && strcmp(got, expected) == 0) {
        g_pass++;
        return;
    }
    g_fail++;
    printf("FAIL %s: got [%s] expected [%s]\n", what, got != NULL ? got : "(null)", expected);
}

static void check_int(const char *what, long got, long expected)
{
    if (got == expected) {
        g_pass++;
        return;
    }
    g_fail++;
    printf("FAIL %s: got %ld expected %ld\n", what, got, expected);
}

static HtmlElem find(const char *selector)
{
    HtmlElem e = selector[0] == '#' && strpbrk(selector + 1, " .:>[") == NULL ? dom_by_id(g_dom, selector + 1) : 0;
    if (e == 0) {
        e = dom_query(g_dom, 0, selector);
    }
    if (e == 0) {
        printf("FAIL no element for %s\n", selector);
        g_fail++;
    }
    return e;
}

static void check(const char *selector, const char *prop, const char *expected)
{
    HtmlElem e = find(selector);
    const CssComputed *c;
    char buf[512];
    char what[256];
    if (e == 0) {
        return;
    }
    c = cascade_elem_computed(g_dom, e);
    snprintf(what, sizeof(what), "%s { %s }", selector, prop);
    if (c == NULL) {
        check_str(what, "(no computed)", expected);
        return;
    }
    if (!fmt_prop(c, prop, buf, sizeof(buf))) {
        check_str(what, "(unknown property in test formatter)", expected);
        return;
    }
    check_str(what, buf, expected);
}

static void check_pe(const char *selector, const CssComputed *c, const char *pe, const char *prop, const char *expected)
{
    char buf[512];
    char what[256];
    snprintf(what, sizeof(what), "%s::%s { %s }", selector, pe, prop);
    if (c == NULL) {
        check_str(what, "(no pseudo)", expected);
        return;
    }
    if (!fmt_prop(c, prop, buf, sizeof(buf))) {
        check_str(what, "(unknown property in test formatter)", expected);
        return;
    }
    check_str(what, buf, expected);
}

static uint32_t restyle(void)
{
    return cascade_restyle(g_dom, g_ua, g_sheets, g_nsheets);
}

/* ------------------------------------------------------------------------- */
/* Chrome's getComputedStyle values (tests/data/v7-menu-1280.computed)        */
/* ------------------------------------------------------------------------- */

static int g_skipped = 0;
static int g_verbose = 0;   /* KN_TEST_VERBOSE=1: list the known gaps */

/* Lengths Chrome reports as used values (percentages, auto, calc() resolved
 * against the layout): only absolute px values are compared. */
static const CssLength *px_only_length(const CssComputed *c, const char *prop)
{
    static const char *const names[] = { "top", "right", "bottom", "left", "margin-top", "margin-right", "margin-bottom",
                                         "margin-left", "padding-top", "padding-right", "padding-bottom", "padding-left" };
    const CssLength *lens[] = { &c->inset[CSS_TOP], &c->inset[CSS_RIGHT], &c->inset[CSS_BOTTOM], &c->inset[CSS_LEFT],
                                &c->margin[CSS_TOP], &c->margin[CSS_RIGHT], &c->margin[CSS_BOTTOM], &c->margin[CSS_LEFT],
                                &c->padding[CSS_TOP], &c->padding[CSS_RIGHT], &c->padding[CSS_BOTTOM], &c->padding[CSS_LEFT] };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (!strcmp(prop, names[i])) {
            return lens[i];
        }
    }
    return NULL;
}

static void fmt_shadow_chrome(const CssShadow *s, const CssColor *current, int box, char *out, size_t cap)
{
    char x[32], y[32], b[32], sp[32], col[64];
    fmt_num(x, sizeof(x), s->x);
    fmt_num(y, sizeof(y), s->y);
    fmt_num(b, sizeof(b), s->blur);
    fmt_num(sp, sizeof(sp), s->spread);
    fmt_color(s->color.kind == CSS_COLOR_CURRENT ? current : &s->color, col, sizeof(col));
    if (box) {
        snprintf(out, cap, "%s %spx %spx %spx %spx%s", col, x, y, b, sp, s->inset ? " inset" : "");
    } else {
        snprintf(out, cap, "%s %spx %spx %spx", col, x, y, b);
    }
}

/* url() as Chrome reports it relative to ui/: the sheet's directory + the url, "../" folded. */
static void fmt_url(const CssImage *img, char *out, size_t cap)
{
    char path[512] = "";
    const char *url = img->url != NULL ? img->url : "";
    const char *slash = img->base != NULL ? strrchr(img->base, '/') : NULL;
    if (slash != NULL) {
        snprintf(path, sizeof(path), "%.*s", (int)(slash - img->base), img->base);
    }
    while (url[0] == '.' && url[1] == '/') {
        url += 2;
    }
    while (!strncmp(url, "../", 3)) {
        char *up = strrchr(path, '/');
        url += 3;
        if (up != NULL) {
            *up = '\0';
        } else {
            path[0] = '\0';
        }
    }
    snprintf(out, cap, "url(\"%s%s%s\")", path, path[0] ? "/" : "", url);
}

static const char *bg_repeat_chrome(uint8_t x, uint8_t y)
{
    if (x == y) {
        return KW(k_bg_repeat, x);
    }
    if (x == CSS_BG_REPEAT_REPEAT && y == CSS_BG_REPEAT_NO_REPEAT) {
        return "repeat-x";
    }
    if (x == CSS_BG_REPEAT_NO_REPEAT && y == CSS_BG_REPEAT_REPEAT) {
        return "repeat-y";
    }
    return "?";
}

/* Formats `prop` the way Chrome serializes it.  Returns 1, 0 for a property the
 * formatter does not know, -1 when Chrome reports a layout-resolved value. */
static int fmt_chrome(const CssComputed *c, const char *prop, char *out, size_t cap)
{
    const CssLength *len = px_only_length(c, prop);
    char a[96], b[96];
    if (len != NULL) {
        if (len->kind != CSS_LEN_VALUE || len->pct != 0.0f) {
            return -1;
        }
        fmt_length(len, out, cap);
        return 1;
    }
    if (!strcmp(prop, "border-left-color") || !strcmp(prop, "border-bottom-color")) {
        const CssColor *col = &c->border_color[prop[7] == 'l' ? CSS_LEFT : CSS_BOTTOM];
        fmt_color(col->kind == CSS_COLOR_CURRENT ? &c->color : col, out, cap);
        return 1;
    }
    if (!strcmp(prop, "line-height") && c->line_height.kind == CSS_LEN_NUMBER) {
        fmt_num(a, sizeof(a), c->line_height.px * c->font_size);
        snprintf(out, cap, "%spx", a);
        return 1;
    }
    if (!strcmp(prop, "vertical-align") && c->vertical_align == CSS_VERTICAL_ALIGN_LENGTH) {
        fmt_length(&c->vertical_align_length, out, cap);
        return 1;
    }
    if (!strcmp(prop, "text-decoration-line")) {
        return fmt_prop(c, "text-decoration", out, cap);
    }
    if (!strcmp(prop, "text-shadow")) {
        size_t o = 0;
        snprintf(out, cap, "none");
        for (int i = 0; i < c->text_shadow.count && o < cap; i++) {
            fmt_shadow_chrome(&c->text_shadow.items[i], &c->color, 0, a, sizeof(a));
            o += (size_t)snprintf(out + o, cap - o, "%s%s", i ? ", " : "", a);
        }
        return 1;
    }
    if (!strcmp(prop, "box-shadow")) {
        if (c->box_shadow_count == 0) {
            snprintf(out, cap, "none");
        } else {
            fmt_shadow_chrome(&c->box_shadow, &c->color, 1, out, cap);
        }
        return 1;
    }
    if (!strcmp(prop, "background-image")) {
        const CssImage *img = &c->bg_layers[0].image;
        if (c->bg_layer_count == 0 || img->kind == CSS_IMAGE_NONE) {
            snprintf(out, cap, "none");
        } else if (img->kind == CSS_IMAGE_URL) {
            fmt_url(img, out, cap);
        } else {
            snprintf(out, cap, "%s-gradient(%d stops)", img->gradient.kind == CSS_GRADIENT_LINEAR ? "linear" : "radial",
                     img->gradient.stop_count);
        }
        return 1;
    }
    if (!strcmp(prop, "background-size")) {
        const CssBgSize *s = &c->bg_layers[0].size;
        if (s->kind == CSS_BG_SIZE_COVER || s->kind == CSS_BG_SIZE_CONTAIN) {
            return fmt_prop(c, prop, out, cap);
        }
        fmt_length(&s->w, a, sizeof(a));
        fmt_length(&s->h, b, sizeof(b));
        snprintf(out, cap, s->h.kind == CSS_LEN_AUTO ? "%s" : "%s %s", a, b);
        return 1;
    }
    if (!strcmp(prop, "background-repeat")) {
        snprintf(out, cap, "%s", bg_repeat_chrome(c->bg_layers[0].repeat_x, c->bg_layers[0].repeat_y));
        return 1;
    }
    return fmt_prop(c, prop, out, cap);
}

/* Copies the first top-level comma item of `in`. */
static void first_layer(const char *in, char *out, size_t cap)
{
    int depth = 0;
    size_t o = 0;
    for (; *in != '\0' && o + 1 < cap; in++) {
        depth += *in == '(' ? 1 : *in == ')' ? -1 : 0;
        if (*in == ',' && depth == 0) {
            break;
        }
        out[o++] = *in;
    }
    out[o] = '\0';
}

/* "linear-gradient(to right, rgb(..) 0%, 94%, rgb(..))" -> "linear-gradient(2 stops)": counts
 * the color stops (the direction/shape argument and position-only hints are not stops). */
static void gradient_stops(const char *in, char *out, size_t cap)
{
    const char *p = strchr(in, '(');
    int depth = 0, stops = 0, start = 1;
    if (p == NULL) {
        snprintf(out, cap, "%s", in);
        return;
    }
    for (const char *q = p + 1; *q != '\0' && depth >= 0; q++) {
        if (start) {
            while (*q == ' ') {
                q++;
            }
            stops += !strncmp(q, "rgb", 3) || *q == '#' || !strncmp(q, "hsl", 3) || !strncmp(q, "color(", 6);
            start = 0;
        }
        depth += *q == '(' ? 1 : *q == ')' ? -1 : 0;
        start = *q == ',' && depth == 0;
    }
    snprintf(out, cap, "%.*s(%d stops)", (int)(p - in), in, stops);
}

/* Lowercase, quotes dropped, whitespace collapsed (font-family / custom properties). */
static void loose(const char *in, char *out, size_t cap, int keep_spaces)
{
    size_t o = 0;
    for (; *in != '\0' && o + 1 < cap; in++) {
        if (*in == '"' || *in == '\'' || (*in == ' ' && (!keep_spaces || (o > 0 && out[o - 1] == ' ')))) {
            continue;
        }
        out[o++] = (char)(*in >= 'A' && *in <= 'Z' ? *in + 32 : *in);
    }
    out[o] = '\0';
}

/* Same text, numbers equal within 0.02 (+0.5%): Chrome keeps floats where the engine
 * rounds (13.3333px, 8-bit alpha). */
static int values_match(const char *got, const char *exp)
{
    while (*got != '\0' && *exp != '\0') {
        int gnum = (*got >= '0' && *got <= '9') || ((*got == '-' || *got == '.') && got[1] >= '0' && got[1] <= '9');
        int enm = (*exp >= '0' && *exp <= '9') || ((*exp == '-' || *exp == '.') && exp[1] >= '0' && exp[1] <= '9');
        if (gnum && enm) {
            char *ge, *ee;
            double g = strtod(got, &ge), e = strtod(exp, &ee);
            if (fabs(g - e) > 0.02 + 0.005 * fabs(e)) {
                return 0;
            }
            got = ge;
            exp = ee;
            continue;
        }
        if (*got++ != *exp++) {
            return 0;
        }
    }
    return *got == '\0' && *exp == '\0';
}

/* Chrome values the engine represents differently on purpose; returns 1 when `got` is the
 * engine's form of `exp` for this element. */
static int engine_equivalent(HtmlElem e, const CssComputed *c, const char *prop, const char *got, const char *exp)
{
    const char *tag = dom_tag(g_dom, e);
    if (!strcmp(prop, "display")) {
        /* floats, out-of-flow boxes and flex/grid items are blockified when the box tree is
         * built (layout_build.c blockify()), not in the computed style */
        HtmlElem parent = dom_parent(g_dom, e);
        const CssComputed *pc = parent != 0 ? cascade_elem_computed(g_dom, parent) : NULL;
        while (pc != NULL && pc->display == CSS_DISPLAY_CONTENTS) {   /* the box-tree parent */
            parent = dom_parent(g_dom, parent);
            pc = parent != 0 ? cascade_elem_computed(g_dom, parent) : NULL;
        }
        int parent_fg = pc != NULL && (pc->display == CSS_DISPLAY_FLEX || pc->display == CSS_DISPLAY_INLINE_FLEX ||
                                       pc->display == CSS_DISPLAY_GRID || pc->display == CSS_DISPLAY_INLINE_GRID);
        int blockified = c->float_ != CSS_FLOAT_NONE || c->position == CSS_POSITION_ABSOLUTE ||
                         c->position == CSS_POSITION_FIXED || parent_fg;
        if (!blockified) {
            return 0;
        }
        if (!strcmp(got, "inline") || !strcmp(got, "inline-block")) {
            return !strcmp(exp, "block");
        }
        return !strncmp(got, "inline-", 7) && !strcmp(got + 7, exp);
    }
    if (!strncmp(prop, "overflow-", 9)) {
        /* Chrome's UA sheet clips replaced elements and form controls (overflow: clip); the
         * engine never paints their content outside the box */
        static const char *const replaced[] = { "canvas", "img", "video", "iframe", "input", "textarea", "select", "embed", "object" };
        for (size_t i = 0; i < sizeof(replaced) / sizeof(replaced[0]); i++) {
            if (tag != NULL && !strcmp(tag, replaced[i])) {
                return !strcmp(exp, "clip") && !strcmp(got, "visible");
            }
        }
        return 0;
    }
    if (!strcmp(prop, "image-rendering")) {
        return !strcmp(exp, "-webkit-optimize-contrast") && !strcmp(got, "crisp-edges");  /* parsed as its alias */
    }
    if (!strcmp(prop, "background-size")) {
        return !strncmp(exp, "0% ", 3) && !strncmp(got, "0px ", 4) && !strcmp(exp + 3, got + 4);  /* 0% == 0px in CssLength */
    }
    return 0;
}

/* Known engine gaps: the reason, or NULL.  Counted, not failed. */
static int g_known_gaps = 0;
static const char *known_gap(const CssComputed *c, const char *prop, const char *got, const char *exp)
{
    if (prop[0] == '-' && prop[1] == '-' && strstr(exp, "0.15625vw") != NULL && strstr(got, "0.277778vh") != NULL) {
        /* bundledStyles' @media (max-aspect-ratio: 16/9) { --base-scale: 0.15625vw }: css_sheet.c
         * ignores @media (the same 2px per unit at 1280x720) */
        return "@media not evaluated";
    }
    if (!strcmp(prop, "text-align") && !strcmp(exp, "-webkit-center")) {
        return "text-align: -webkit-center unsupported (ad containers only)";
    }
    if (!strcmp(prop, "text-shadow") && c->text_shadow.count == CSS_MAX_TEXT_SHADOWS && !strncmp(exp, got, strlen(got))) {
        return "text-shadow keeps the first CSS_MAX_TEXT_SHADOWS layers";
    }
    return NULL;
}

static void check_chrome(const char *selector, const char *prop, const char *chrome)
{
    HtmlElem e = find(selector);
    const CssComputed *c;
    char got[1024], exp[1024], g2[1024];
    char what[512];
    const char *gap;
    int r;
    if (e == 0) {
        return;
    }
    c = cascade_elem_computed(g_dom, e);
    snprintf(what, sizeof(what), "%s { %s }", selector, prop);
    if (c == NULL) {
        check_str(what, "(no computed)", chrome);
        return;
    }
    r = fmt_chrome(c, prop, got, sizeof(got));
    if (r < 0) {
        g_skipped++;
        return;
    }
    if (r == 0) {
        check_str(what, "(unknown property in test formatter)", chrome);
        return;
    }
    snprintf(exp, sizeof(exp), "%s", chrome);
    if (!strcmp(prop, "background-image") || !strcmp(prop, "background-size") || !strcmp(prop, "background-repeat") ||
        !strcmp(prop, "box-shadow")) {
        first_layer(chrome, exp, sizeof(exp));
        if (strstr(exp, "gradient(") != NULL) {
            gradient_stops(exp, g2, sizeof(g2));
            snprintf(exp, sizeof(exp), "%s", g2);
        }
    } else if (!strcmp(prop, "font-family") || (prop[0] == '-' && prop[1] == '-')) {
        loose(got, g2, sizeof(g2), prop[0] != '-');
        snprintf(got, sizeof(got), "%s", g2);
        loose(chrome, exp, sizeof(exp), prop[0] != '-');
    } else if (!strcmp(prop, "justify-content") && !strcmp(chrome, "normal")) {
        snprintf(exp, sizeof(exp), "flex-start");   /* the engine has no `normal`: flex-start behaviour */
    } else if (!strcmp(prop, "align-items") && !strcmp(chrome, "normal")) {
        snprintf(exp, sizeof(exp), "stretch");      /* `normal` behaves as stretch */
    }
    if (values_match(got, exp) || engine_equivalent(e, c, prop, got, exp)) {
        g_pass++;
        return;
    }
    gap = known_gap(c, prop, got, exp);
    if (gap != NULL) {
        g_known_gaps++;
        if (g_verbose) {
            printf("known gap %s: %s\n", what, gap);
        }
        return;
    }
    g_fail++;
    printf("FAIL %s: got [%s] expected [%s] (Chrome)\n", what, got, chrome);
}

static void test_chrome_computed(void)
{
    size_t len;
    char *text = read_file("client/src/html/engine/tests/data/v7-menu-1280.computed", &len);
    HtmlElem hovered = 0;
    int lines = 0, sections = 0;
    int fail0 = g_fail;
    for (char *line = text, *next; line != NULL && *line != '\0'; line = next) {
        char *prop, *value;
        next = strchr(line, '\n');
        if (next != NULL) {
            *next++ = '\0';
        }
        if (!strncmp(line, "# ", 2)) {
            continue;   /* header comment ("#id" lines are selectors) */
        }
        if (!strncmp(line, "@hover ", 7)) {
            if (hovered != 0) {
                dom_set_pseudo_state(g_dom, hovered, DOM_PSEUDO_HOVER, 0);
            }
            hovered = find(line + 7);
            if (hovered != 0) {
                dom_set_pseudo_state(g_dom, hovered, DOM_PSEUDO_HOVER, 1);
            }
            restyle();
            sections++;
            continue;
        }
        prop = strchr(line, '\t');
        value = prop != NULL ? strchr(prop + 1, '\t') : NULL;
        if (value == NULL) {
            continue;
        }
        *prop++ = '\0';
        *value++ = '\0';
        check_chrome(line, prop, value);
        lines++;
    }
    if (hovered != 0) {
        dom_set_pseudo_state(g_dom, hovered, DOM_PSEUDO_HOVER, 0);
        restyle();
    }
    printf("Chrome computed values: %d lines (%d :hover sections), %d failed, %d known gaps, %d layout-resolved skipped\n",
           lines, sections, g_fail - fail0, g_known_gaps, g_skipped);
    check_int("v7-menu-1280.computed has > 4000 values", lines > 4000, 1);
    free(text);
}

/* ------------------------------------------------------------------------- */
/* Test body                                                                  */
/* ------------------------------------------------------------------------- */

/* Generated markup inside #menuWindow.  The ranked bits are styled by ranked-kn.css, the
 * KrunkNative sheet ranked_ui.c adds at run time. */
static const char k_fragment[] =
    "<input id=\"tslider\" type=\"range\" class=\"sliderM\" min=\"0\" max=\"1\">"
    "<label class=\"switch\"><input id=\"tcheck\" type=\"checkbox\"><span id=\"tslide\" class=\"slider\"></span></label>"
    "<div class=\"tabHeader selected\" id=\"ttab\">x</div>"
    "<input id=\"rankedPartyKey\" placeholder=\"Key\"><span><a class=\"partyJoin\" id=\"tjoin\"></a><a class=\"partyCreate\" id=\"tcreate\"></a></span>"
    "<b id=\"tbold\">b</b><i id=\"tital\">i</i>"
    "<div id=\"tnone\" style=\"display:none\"><div id=\"tinner\" style=\"color: #123456\"></div></div>"
    "</br><input id=\"tundef\" type=\"undefined\"></input>"
    "<div id=\"tbadge\" class=\"itemCnt\" data-badge=\"3\"></div><canvas id=\"tcanvas\"></canvas>"
    "<div class=\"accountEloIcon\" id=\"telo\" style=\"font-size:20px\"></div><input type=\"color\" id=\"tcolor\">";

/* Expected values are Chrome's getComputedStyle after the same DOM changes on the same page
 * and sheets, except where noted (lengths Chrome reports as used values, pseudo-elements
 * getComputedStyle cannot read). */
static void test_dynamic(void)
{
    HtmlElem html = dom_root(g_dom);
    HtmlElem btn = dom_by_id(g_dom, "menuBtnHost");
    HtmlElem item = dom_query(g_dom, 0, ".menuItem");
    HtmlElem ui_base = dom_by_id(g_dom, "uiBase");
    HtmlElem menu_window = dom_by_id(g_dom, "menuWindow");
    HtmlElem holder = dom_by_id(g_dom, "windowHolder");
    HtmlElem e;
    const CssComputed *c;
    CssComputed pe;
    uint32_t n;
    double t0;

    /* html.kn-end-open (end-screen results, menu_ui.c) lifts it too: the result tabs are clickable */
    dom_add_class(g_dom, html, "kn-end-open");
    restyle();
    check("#endUI", "pointer-events", "all");                  /* main.css #endUI */
    check("#menuWindow", "pointer-events", "auto");
    dom_remove_class(g_dom, html, "kn-end-open");
    restyle();
    check("#endUI", "pointer-events", "none");

    /* html.kn-menu-open lifts krunknative.css's pointer-events: none !important */
    dom_add_class(g_dom, html, "kn-menu-open");
    n = restyle();
    check_int("restyle count after html class change >= 50", n >= 50, 1);
    dom_mark_all_dirty(g_dom);
    t0 = now_ms();
    n = restyle();
    t0 = now_ms() - t0;
    printf("full restyle, menu open: %.2f ms (%u elements)\n", t0, n);
#ifdef KN_OPTIMIZED
    check_int("full restyle (menu open) < 20 ms (-O2)", t0 < 20.0, 1);
#endif
    check("#menuBtnHost", "pointer-events", "all");
    check("#menuWindow", "pointer-events", "auto");
    check_str("html class attr", dom_get_attr(g_dom, html, "class", NULL), "kn-menu-open");

    /* :hover (the values for every :hover rule are in the .computed data) */
    dom_set_pseudo_state(g_dom, btn, DOM_PSEUDO_HOVER, 1);
    n = restyle();
    check_int("restyle count after hover (self + following siblings only)", n >= 1 && n <= 6, 1);
    check_chrome("#menuBtnHost", "border-left-color", "rgb(255, 255, 255)");
    dom_set_pseudo_state(g_dom, btn, DOM_PSEUDO_HOVER, 0);
    restyle();
    check_chrome("#menuBtnHost", "border-left-color", "rgb(180, 71, 255)");   /* .buttonP */

    dom_set_pseudo_state(g_dom, item, DOM_PSEUDO_HOVER, 1);
    restyle();
    check(".menuItem .menBtnIcn", "color", "rgb(255, 255, 255)");      /* .menuItem:hover !important over style="" */
    dom_set_pseudo_state(g_dom, item, DOM_PSEUDO_HOVER, 0);
    restyle();
    check(".menuItem .menBtnIcn", "color", "rgb(255, 106, 11)");

    /* inline style precedence and dom_set_style */
    e = dom_by_id(g_dom, "errorNotification");
    check("#errorNotification", "display", "none");                   /* style="display:none" */
    dom_set_style(g_dom, e, "display", "block");
    restyle();
    check("#errorNotification", "display", "block");
    check_str("get_style display", dom_get_style(g_dom, e, "display"), "block");
    check_str("style attr text", dom_get_attr(g_dom, e, "style", NULL), "display: block;");
    dom_set_style(g_dom, e, "display", NULL);
    restyle();
    check("#errorNotification", "display", "block");                 /* .menuCard */
    dom_set_style(g_dom, e, "background-color", "var(--red)");
    restyle();
    check("#errorNotification", "background-color", "rgb(237, 66, 66)"); /* var() in inline style */
    dom_set_style(g_dom, e, "width", "calc(50% - 10px)");
    restyle();
    check("#errorNotification", "width", "calc(50% - 10px)");          /* computed (Chrome: used px) */

    /* #uiBase.onMenu / .onCompMenu descendant rules */
    check("#spectButton", "display", "flex");                         /* #uiBase.onMenu #spectButton */
    dom_remove_class(g_dom, ui_base, "onMenu");
    n = restyle();
    check("#spectButton", "display", "none");
    printf("uiBase.onMenu removal restyle: %u elements\n", n);
    dom_add_class(g_dom, ui_base, "onMenu");
    restyle();
    check("#spectButton", "display", "flex");
    check("#chatHolder", "left", "150px");
    dom_add_class(g_dom, ui_base, "onCompMenu");
    restyle();
    check("#chatHolder", "left", "20px");                             /* #uiBase.onCompMenu #chatHolder, later */
    dom_remove_class(g_dom, ui_base, "onCompMenu");
    restyle();
    check("#chatHolder", "left", "150px");

    /* generated markup inside #menuWindow: sliders, switches, var(--yellow),
     * :placeholder-shown, <b>, ::before, with ranked-kn.css added at run time */
    {
        size_t clen;
        char *css = read_file("ui/css/ranked-kn.css", &clen);
        g_sheets[g_nsheets++] = css_sheet_load(css, clen, "css/ranked-kn.css");
        free(css);
    }
    dom_set_style(g_dom, holder, "display", "block");
    dom_set_html(g_dom, menu_window, k_fragment, strlen(k_fragment));
    restyle();
    check("#tslider", "height", "15px");
    check("#tslider", "width", "100%");                               /* Chrome: 705px used */
    check("#tslider", "appearance", "none");
    check("#tslider", "background-color", "rgb(168, 168, 168)");
    check("#tslider", "border-top-left-radius", "4px");
    /* getComputedStyle cannot read ::-webkit-slider-thumb: main.css .sliderM::-webkit-slider-thumb */
    n = cascade_pseudo_computed(g_dom, dom_by_id(g_dom, "tslider"), CSS_PE_SLIDER_THUMB, &pe);
    check_int("slider thumb matched declarations > 0", n > 0, 1);
    check_pe("#tslider", &pe, "-webkit-slider-thumb", "width", "15px");
    check_pe("#tslider", &pe, "-webkit-slider-thumb", "height", "36px");
    check_pe("#tslider", &pe, "-webkit-slider-thumb", "background-color", "rgb(255, 255, 255)");
    check_pe("#tslider", &pe, "-webkit-slider-thumb", "appearance", "none");
    check_pe("#tslider", &pe, "-webkit-slider-thumb", "cursor", "pointer");

    check("label.switch", "width", "65px");
    check("label.switch", "height", "34px");
    check("#tslide", "position", "absolute");
    c = cascade_elem_before(g_dom, dom_by_id(g_dom, "tslide"));
    check_pe("#tslide", c, "before", "content", "\"\"");
    check_pe("#tslide", c, "before", "width", "26px");
    check_pe("#tslide", c, "before", "height", "22px");
    check_pe("#tslide", c, "before", "left", "4px");
    check_pe("#tslide", c, "before", "position", "absolute");
    check_pe("#tslide", c, "before", "transform", "none");
    check_pe("#tslide", c, "before", "background-color", "rgb(255, 255, 255)");
    check_pe("#tslide", c, "before", "color", "rgb(53, 53, 53)");       /* inherits from the element */
    check_int("#tslide has no ::after", cascade_elem_after(g_dom, dom_by_id(g_dom, "tslide")) == NULL, 1);

    dom_set_checked(g_dom, dom_by_id(g_dom, "tcheck"), 1);
    restyle();
    check_int("checked attr", dom_get_checked(g_dom, dom_by_id(g_dom, "tcheck")), 1);
    check("#tslide", "background-color", "rgb(33, 150, 243)");          /* input:checked + .slider */
    c = cascade_elem_before(g_dom, dom_by_id(g_dom, "tslide"));
    check_pe("#tslide", c, "before", "transform", "translate(30px, 0px)"); /* Chrome: matrix(1, 0, 0, 1, 30, 0) */
    dom_set_checked(g_dom, dom_by_id(g_dom, "tcheck"), 0);
    restyle();
    c = cascade_elem_before(g_dom, dom_by_id(g_dom, "tslide"));
    check_pe("#tslide", c, "before", "transform", "none");

    check("#ttab", "color", "rgb(251, 192, 45)");                       /* var(--yellow) */
    check("#ttab", "border-bottom-color", "rgb(251, 192, 45)");         /* var() inside a shorthand */
    check("#ttab", "border-bottom-width", "4px");

    /* :placeholder-shown via the kn-placeholder-shown attribute (ranked-kn.css) */
    check("#tcreate", "display", "block");
    check("#tjoin", "display", "none");
    dom_set_value(g_dom, dom_by_id(g_dom, "rankedPartyKey"), "abc");
    restyle();
    check("#tcreate", "display", "none");
    check("#tjoin", "display", "block");
    dom_set_value(g_dom, dom_by_id(g_dom, "rankedPartyKey"), "");
    restyle();
    check("#tcreate", "display", "block");

    c = cascade_elem_after(g_dom, dom_by_id(g_dom, "tbadge"));
    check_pe("#tbadge", c, "after", "content", "attr(data-badge)");     /* Chrome: "3" */
    check_pe("#tbadge", c, "after", "top", "146px");
    check_pe("#tbadge", c, "after", "font-size", "12px");
    check_pe("#tbadge", c, "after", "color", "rgb(255, 255, 255)");
    check("#tcanvas", "width", "300px");
    check("#tcanvas", "height", "150px");
    check("#telo", "width", "30px");                                   /* 1.5em at font-size 20px */
    check("#telo", "height", "30px");
    check("#tcolor", "width", "66px");                                 /* author over UA 50px */
    check("#tcolor", "border-left-color", "rgb(33, 150, 243)");        /* border: 4px solid var(--blue) */
    check("#tcolor", "border-left-width", "4px");
    check("#tcolor", "appearance", "none");
    n = cascade_pseudo_computed(g_dom, dom_by_id(g_dom, "tcolor"), CSS_PE_COLOR_SWATCH, &pe);
    check_int("color swatch pseudo matched", n > 0, 1);
    check("#tbold", "font-weight", "400");                             /* main.css b { font-weight: 400 } */
    check("#tital", "font-style", "italic");
    check("#tundef", "font-family", "gamefont");
    check_int("</br> parsed as <br>", dom_query(g_dom, menu_window, "br") != 0, 1);
    check_int("display:none subtree not computed by restyle", dom_elem_data(g_dom, dom_by_id(g_dom, "tinner"))->computed == NULL, 1);
    check("#tinner", "color", "rgb(18, 52, 86)");                        /* on-demand compute */
    dom_set_style(g_dom, dom_by_id(g_dom, "tnone"), "display", "block");
    n = restyle();
    check_int("restyle computes the newly displayed subtree", n >= 2, 1);
    check("#tinner", "color", "rgb(18, 52, 86)");

    /* duplicate ids: first in document order wins; rescan after removal */
    e = dom_by_id(g_dom, "editorBtnM");
    check_int("first #editorBtnM is the client button", e != 0 && strstr(dom_get_attr(g_dom, e, "src", NULL), "client") != NULL, 1);
    dom_remove(g_dom, e);
    e = dom_by_id(g_dom, "editorBtnM");
    check_int("second #editorBtnM after removal", e != 0 && strstr(dom_get_attr(g_dom, e, "src", NULL), "editor") != NULL, 1);

    /* custom property + calc(var()) and style.setProperty('--kn-*') (hud.css) */
    dom_insert_html(g_dom, dom_body(g_dom), DOM_INSERT_BEFORE_END,
                    "<div id=\"knCrosshair\"><div id=\"tarm\" class=\"knCrossArm knCrossArm-t\"></div></div>",
                    strlen("<div id=\"knCrosshair\"><div id=\"tarm\" class=\"knCrossArm knCrossArm-t\"></div></div>"));
    restyle();
    check("#knCrosshair", "--kn-cross-size", "182px");
    check("#tarm", "top", "-91px");
    check("#tarm", "box-shadow", "0px 0px 0px 2px rgba(0, 0, 0, 0.5)");
    dom_set_style(g_dom, dom_by_id(g_dom, "knCrosshair"), "--kn-cross-size", "100px");
    restyle();
    check("#tarm", "top", "-50px");
    check_str("--kn-cross-size inline text", dom_get_style(g_dom, dom_by_id(g_dom, "knCrosshair"), "--kn-cross-size"), "100px");

    /* text / tree ops */
    e = dom_create_element(g_dom, "span");
    dom_set_text(g_dom, e, "hello", 5);
    dom_append(g_dom, dom_by_id(g_dom, "menuClassName"), e);
    {
        char *t = dom_get_text(g_dom, dom_by_id(g_dom, "menuClassName"));
        check_str("textContent after append", t, "Triggermanhello");
        free(t);
    }
    check_int("parent navigation", dom_parent(g_dom, e) == dom_by_id(g_dom, "menuClassName"), 1);
    check("#menuClassName > span", "font-size", "30px");              /* inherited into the new element */
    check("#menuClassName > span", "display", "inline");

    /* class list ops */
    dom_toggle_class(g_dom, e, "foo", -1);
    dom_add_class(g_dom, e, "bar");
    check_str("class after toggle/add", dom_get_attr(g_dom, e, "class", NULL), "foo bar");
    dom_remove_class(g_dom, e, "foo");
    check_str("class after remove", dom_get_attr(g_dom, e, "class", NULL), "bar");
    check_int("has_class", dom_has_class(g_dom, e, "bar"), 1);
}

/* innerHTML of the host-game window (ui/windows/8.html, the largest v7 window template). */
static void test_host_window(void)
{
    size_t len;
    char *host = read_file("ui/windows/8.html", &len);
    HtmlElem menu_window = dom_by_id(g_dom, "menuWindow");
    uint32_t before = dom_elem_count(g_dom);
    double t0, t1, t2;
    uint32_t n;

    printf("host window html: %zu bytes\n", len);
    t0 = now_ms();
    dom_set_html(g_dom, menu_window, host, len);
    t1 = now_ms();
    n = restyle();
    t2 = now_ms();
    printf("host window: parse %.2f ms, restyle %.2f ms (%u elements computed, %d new elements)\n",
           t1 - t0, t2 - t1, n, (int)dom_elem_count(g_dom) - (int)before);
    check_int("host window adds ~800 elements", (int)dom_elem_count(g_dom) - (int)before > 700, 1);
    /* only the displayed part is computed (inactive tabs are display:none) */
    check_int("host window restyle computes the displayed part", n > 100, 1);

    check("#menuWindow .sliderM", "height", "15px");
    check("#menuWindow .settNameSmall", "font-size", "22px");
    check("#menuWindow .settNameSmall", "margin-top", "10px");
    check("#menuWindow .settNameSmall", "clear", "both");
    check("#menuWindow .settNameSmall", "color", "rgba(255, 255, 255, 0.6)");
    check("#menuWindow .hostOpt", "display", "inline-block");
    check("#menuWindow .hostOpt", "width", "203px");
    check("#menuWindow .hostOpt", "border-left-width", "2px");
    check("#menuWindow .hostOpt", "border-left-style", "solid");
    check("#menuWindow .sliderVal", "float", "right");
    check("#menuWindow .sliderVal", "width", "70px");
    check("#menuWindow .hostMap", "display", "inline-block");
    check("#menuWindow .setHed", "font-size", "22px");
#ifdef KN_OPTIMIZED
    check_int("host window restyle < 30 ms (-O2)", (t2 - t1) < 30.0, 1);
#endif
    dom_mark_all_dirty(g_dom);
    t1 = now_ms();
    n = restyle();
    t2 = now_ms();
    printf("full restyle with host window open: %.2f ms (%u elements)\n", t2 - t1, n);
    free(host);
}

int main(int argc, char **argv)
{
    size_t len;
    char *html;
    double t0, t1;
    uint32_t n;

    if (argc > 1) {
        g_root = argv[1];
    }
    g_verbose = getenv("KN_TEST_VERBOSE") != NULL;
    g_dom = dom_create();
    html = read_file("client/src/html/engine/tests/data/v7-menu-1280.html", &len);
    check_int("dom_load_html", dom_load_html(g_dom, html, len), 0);
    printf("v7-menu-1280.html: %u elements\n", dom_elem_count(g_dom));

    t0 = now_ms();
    g_ua = css_sheet_load(KN_UA_CSS, strlen(KN_UA_CSS), "ua.css");
    for (int i = 0; i < N_SHEET_FILES; i++) {
        char rel[256];
        size_t clen;
        char *css;
        snprintf(rel, sizeof(rel), "ui/%s", k_sheet_files[i]);
        css = read_file(rel, &clen);
        g_sheets[g_nsheets++] = css_sheet_load(css, clen, k_sheet_files[i]);
        free(css);
        printf("%-40s %5u rules %5u selectors\n", k_sheet_files[i], css_sheet_rule_count(g_sheets[i]),
               css_sheet_selector_count(g_sheets[i]));
    }
    /* the page's <style> blocks (index.html's ad rules, the capture's animation-off rule) as one
     * sheet: css_cascade takes at most 14 author sheets */
    {
        char *page_css = (char *)malloc(len + 1);
        size_t o = 0;
        int blocks = 0;
        for (const char *p = html; (p = strstr(p, "<style>")) != NULL; blocks++) {
            const char *e = strstr(p += 7, "</style>");
            if (e == NULL) {
                break;
            }
            memcpy(page_css + o, p, (size_t)(e - p));
            o += (size_t)(e - p);
            page_css[o++] = '\n';
            p = e;
        }
        check_int("page <style> blocks", blocks, 5);
        g_sheets[g_nsheets++] = css_sheet_load(page_css, o, "index.html");
        free(page_css);
    }
    free(html);
    t1 = now_ms();
    printf("%d sheets loaded in %.2f ms\n", g_nsheets, t1 - t0);
    check_int("main.css rules > 1000", css_sheet_rule_count(g_sheets[0]) > 1000, 1);
    {
        const CssKeyframes *kf;
        uint32_t nf = 0;
        const CssFontFace *ff = css_sheet_font_faces(g_sheets[4], &nf);
        restyle(); /* sets the sheet set */
        kf = cascade_find_keyframes("grow");
        check_int("grow has one frame", kf != NULL ? (long)kf->nframes : -1, 1);
        check_int("grow frame offset 0", kf != NULL && kf->frames[0].offset == 0.0f, 1);
        check_int("grow frame has 2 decls", kf != NULL ? (long)kf->frames[0].ndecls : -1, 2);
        kf = cascade_find_keyframes("lds-ring");
        check_int("lds-ring has 2 frames", kf != NULL ? (long)kf->nframes : -1, 2);
        check_int("lds-ring last offset 100", kf != NULL && kf->frames[1].offset == 100.0f, 1);
        kf = cascade_find_keyframes("rainbow");
        check_int("rainbow has 6 frames", kf != NULL ? (long)kf->nframes : -1, 6);
        check_int("material-icons.css one @font-face", nf, 1);
        check_str("material icons family", nf > 0 ? ff[0].family : "", "material icons");
        check_str("material icons src", nf > 0 && ff[0].src_count > 0 ? ff[0].src[0] : "", "./fonts/flUhRq6tzZclQEJ-Vdg-IuiaDsNc.woff2");
        ff = css_sheet_font_faces(g_sheets[0], &nf);
        check_int("main.css @font-face count", nf, 2);
    }

    /* full restyle timing (fresh) */
    dom_mark_all_dirty(g_dom);
    t0 = now_ms();
    n = restyle();
    t1 = now_ms();
    printf("full restyle: %.2f ms (%u elements)\n", t1 - t0, n);
#ifdef KN_OPTIMIZED
    check_int("full restyle < 20 ms (-O2)", (t1 - t0) < 20.0, 1);
#endif
    t0 = now_ms();
    n = restyle();
    t1 = now_ms();
    check_int("second restyle is a no-op", n, 0);
    printf("no-op restyle: %.3f ms\n", t1 - t0);

    test_chrome_computed();
    test_dynamic();
    test_host_window();

    printf("%d passed, %d failed\n", g_pass, g_fail);

    for (int i = 0; i < g_nsheets; i++) {
        css_sheet_destroy(g_sheets[i]);
    }
    css_sheet_destroy(g_ua);
    cascade_shutdown();
    dom_destroy(g_dom);
    return g_fail;
}
