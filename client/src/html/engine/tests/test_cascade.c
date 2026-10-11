/*
 * test_cascade.c - loads ui/index.html with its six stylesheets (+ the UA
 * sheet), runs the cascade and checks (selector, property, expected computed
 * value) triples derived from Chrome's loaded DOM
 * (doc/ref/krunker-v3.1.1/menu/real-1280.dom.html) and its known computed
 * values.  Also times a full restyle and the restyle after innerHTML of the
 * 138 KB host window.
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
static CssSheet *g_sheets[6];
static const char *k_sheet_files[6] = {
    "ui/css/main.css", "ui/css/flag.css", "ui/css/google-secular-one.css",
    "ui/css/material-icons.css", "ui/css/krunknative.css", "ui/css/hud.css",
};

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

/* Extracts windows.host.html from tools/ui-import/krunker-windows.js (a JSON string). */
static char *extract_host_html(size_t *out_len)
{
    size_t len;
    char *js = read_file("tools/ui-import/krunker-windows.js", &len);
    const char *p = strstr(js, "\"host\": {");
    const char *q;
    char *out;
    size_t o = 0;
    if (p == NULL) {
        fprintf(stderr, "windows.host not found\n");
        exit(99);
    }
    q = strstr(p, "\"html\": \"");
    if (q == NULL) {
        fprintf(stderr, "windows.host.html not found\n");
        exit(99);
    }
    q += strlen("\"html\": \"");
    out = (char *)malloc(len);
    while (*q != '\0' && *q != '"') {
        if (*q == '\\') {
            q++;
            switch (*q) {
            case 'n': out[o++] = '\n'; break;
            case 't': out[o++] = '\t'; break;
            case 'r': out[o++] = '\r'; break;
            case 'b': out[o++] = '\b'; break;
            case 'f': out[o++] = '\f'; break;
            case 'u': {
                unsigned cp = 0;
                for (int i = 1; i <= 4; i++) {
                    char c = q[i];
                    cp = cp * 16 + (unsigned)(c >= 'a' ? c - 'a' + 10 : c >= 'A' ? c - 'A' + 10 : c - '0');
                }
                if (cp < 0x80) {
                    out[o++] = (char)cp;
                } else if (cp < 0x800) {
                    out[o++] = (char)(0xC0 | (cp >> 6));
                    out[o++] = (char)(0x80 | (cp & 0x3F));
                } else {
                    out[o++] = (char)(0xE0 | (cp >> 12));
                    out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                    out[o++] = (char)(0x80 | (cp & 0x3F));
                }
                q += 4;
                break;
            }
            default: out[o++] = *q; break;
            }
            q++;
        } else {
            out[o++] = *q++;
        }
    }
    out[o] = '\0';
    free(js);
    *out_len = o;
    return out;
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
    return cascade_restyle(g_dom, g_ua, g_sheets, 6);
}

/* ------------------------------------------------------------------------- */
/* Test body                                                                  */
/* ------------------------------------------------------------------------- */

static void test_static(void)
{
    /* universal rule + inheritance */
    check("#menuWindow", "color", "rgb(53, 53, 53)");
    check("#menuWindow", "font-family", "gamefont");
    check("#menuWindow", "user-select", "none");
    check("#menuWindow", "outline-style", "none");
    check("#reactInput", "font-family", "gamefont");           /* author * beats UA input font */
    check("#reactInput", "font-size", "23px");
    check("#reactInput", "width", "500px");
    check("#reactInput", "appearance", "auto");
    check("#menuPingIcon", "font-family", "material icons");    /* unquoted multi-word family */
    check("#menuPingIcon", "font-style", "normal");             /* author beats UA i { italic } */
    check("#menuPingIcon", "font-size", "20px");                /* #menuPingDisplay .material-icons (1,1,0) */
    check("#menuPingIcon", "line-height", "1");
    check("#menuPingIcon", "display", "inline-block");
    check("#menuPingIcon", "color", "rgb(128, 128, 128)");      /* inline style="color: gray" */
    check("#mailIcon", "font-size", "37px");                    /* inline beats .material-icons */

    /* html / body / :root */
    check("html", "--yellow", "#fbc02d");
    check("html", "--red", "#ed4242");
    check("#menuWindow", "--yellow", "#fbc02d");                /* custom props inherit */
    check("body", "margin-top", "0px");                         /* main.css over UA 8px */
    check("body", "background-color", "rgba(0, 0, 0, 0)");      /* krunknative.css !important */
    check("html", "overflow-y", "hidden");
    check("body", "display", "block");

    /* #uiBase */
    check("#uiBase", "transform-origin", "0px 0px");
    check("#uiBase", "position", "absolute");
    check("#uiBase", "width", "100%");
    check("#uiBase", "height", "100%");
    check("#uiBase", "z-index", "1");
    check("#uiBase", "pointer-events", "none");
    check("#uiBase", "top", "0px");

    /* #instructions */
    check("#instructions", "font-size", "32px");
    check("#instructions", "color", "rgba(255, 255, 255, 0.8)");
    check("#instructions", "animation", "grow 0.8s ease 0s infinite alternate none");
    check("#instructions", "z-index", "9");
    check("#instructions", "transform", "translate(-50%, -50%)");
    check("#instructions", "margin-right", "-50%");
    check("#instructions", "top", "50%");
    check("#instructions", "text-shadow", "none");

    /* #menuWindow */
    check("#menuWindow", "max-height", "calc(100% - 480px)");
    check("#menuWindow", "width", "705px");
    check("#menuWindow", "padding-top", "20px");
    check("#menuWindow", "padding-left", "20px");
    check("#menuWindow", "font-size", "20px");
    check("#menuWindow", "display", "inline-block");
    check("#menuWindow", "overflow-y", "auto");
    check("#menuWindow", "overflow-x", "visible");
    check("#menuWindow", "z-index", "2");
    check("#menuWindow", "border-top-left-radius", "6px");
    check("#menuWindow", "text-align", "left");
    check("#menuWindow", "pointer-events", "none");            /* html:not(.kn-menu-open) #uiBase * !important */
    check("#menuWindow", "left", "50%");
    check("#menuWindow", "transform", "translate(-50%, -50%)");

    /* buttons: shorthand/longhand order, !important, shadows */
    check("#menuBtnHost", "padding-top", "5px");               /* .button.small !important over .button padding-top */
    check("#menuBtnHost", "padding-left", "22px");
    check("#menuBtnHost", "padding-bottom", "13px");
    check("#menuBtnHost", "background-color", "rgb(180, 71, 255)");  /* .buttonP !important */
    check("#menuBtnHost", "box-shadow", "inset 0px -7px 0px 0px rgb(103, 41, 146)");
    check("#menuBtnHost", "display", "inline-block");
    check("#menuBtnHost", "box-sizing", "border-box");
    check("#menuBtnHost", "border-top-left-radius", "4px");
    check("#menuBtnHost", "color", "rgb(255, 255, 255)");
    check("#menuBtnHost", "text-shadow", "12 layers; first -1px -1px 0px rgb(32, 32, 32)"); /* .button.small !important */
    check("#menuBtnHost", "font-size", "22px");
    check("#menuBtnHost", "cursor", "pointer");
    check("#menuBtnHost", "transition", "all 0.1s ease");
    check("#menuBtnHost", "margin-bottom", "6px");
    check("#menuBtnHost", "pointer-events", "none");           /* html:not(.kn-menu-open) #uiBase * !important */

    /* flex container / items */
    check("#menuItemContainer", "display", "flex");
    check("#menuItemContainer", "width", "130px");
    check(".menuItem", "display", "flex");
    check(".menuItem", "flex-direction", "column");
    check(".menuItem", "justify-content", "space-around");
    check(".menuItem", "align-items", "center");
    check(".menuItem", "background-color", "rgba(0, 0, 0, 0.75)");
    check(".menuItem", "flex-grow", "1");
    check(".menuItem", "border-left-width", "0px");
    check(".menuItem", "border-left-style", "none");

    /* misc */
    check("#respinUI", "z-index", "2147483647");               /* 99999999999 clamped */
    check("#respinUI", "display", "none");
    check("#menuClassName", "text-shadow", "16 layers; first -1px -1px 0px rgb(32, 32, 32)");
    check("#menuClassName", "font-size", "30px");
    check("#menuClassNameTag", "margin-top", "-350px");
    check("#menuClassNameTag", "text-shadow", "none");
    check("#chatList", "overflow-y", "auto");                  /* later declaration wins over overflow-y: hidden */
    check("#chatList", "overflow-x", "hidden");
    check("#chatList", "direction", "rtl");
    check("#chatList", "z-index", "999999");
    check("#ammoDisplay", "letter-spacing", "-2px");
    check("#ammoDisplay", "font-size", "35px");
    check("#ammoDisplay", "padding-top", "8px");
    check("#ammoDisplay", "padding-left", "20px");
    check("#ammoDisplay", "padding-right", "7px");
    check("#ammoDisplay", "margin-right", "-15px");
    check("#ammoDisplay", "background-size", "0px 100%");      /* 0% == 0px in CssLength */
    check("#ammoDisplay", "background-repeat", "no-repeat");
    check("#ammoDisplay", "transform", "none");                /* translateZ(0) dropped */
    check("#ammoDisplay", "background-image", "url(../img/blank_2.png)");
    check(".verticalSeparatorInline", "border-left-width", "4px");
    check(".verticalSeparatorInline", "border-left-style", "solid");
    check(".verticalSeparatorInline", "border-left-color", "rgba(255, 255, 255, 0.12)");
    check("#purchLoadRing > div:nth-child(2)", "animation-delay", "-0.3s");
    check("#purchLoadRing > div:nth-child(3)", "animation-delay", "-0.15s");
    check("#purchLoadRing > div:nth-child(1)", "animation-delay", "-0.45s");
    check("#teamNm1", "color", "rgb(255, 255, 255)");          /* inline style over .teamNm */
    check("#teamNm2", "color", "rgba(255, 255, 255, 0.5)");
    check("#teamSelector", "display", "none");
    check("#errorNotification", "display", "none");            /* inline style="display: none" */
    check("#errorNotification a", "color", "rgb(33, 150, 243)");/* a { color } over UA link color */
    check("#errorNotification a", "text-decoration", "none");
    check("#errorNotification a", "cursor", "pointer");
    check("#classPreviewCanvas", "width", "300px");             /* UA canvas intrinsic */
    check("#classPreviewCanvas", "height", "150px");
    check("#classPreviewCanvas", "margin-bottom", "-100px");
    check("#spinWindow", "width", "620px");                     /* author over UA */
    check("#killReactWindow", "border-top-left-radius", "6px");
    check("#killReactWindow", "background-color", "rgba(0, 0, 0, 0.6)");
    check("#killReactWindow", "padding-top", "10px");
}

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

/* The display/class writes menu_ui.c does when the menu opens (subset of
 * ui_test's ut_menu_preset). */
static void menu_open_preset(void)
{
    static const char *const shown[] = { "menuHolder", "menuHider", "instructionHolder" };
    static const char *const hidden[] = { "loadingBg", "consentBlock", "bdayBlock", "spinUI", "respinUI", "endUI" };
    for (size_t i = 0; i < sizeof(shown) / sizeof(shown[0]); i++) {
        dom_set_style(g_dom, dom_by_id(g_dom, shown[i]), "display", "block");
    }
    for (size_t i = 0; i < sizeof(hidden) / sizeof(hidden[0]); i++) {
        dom_set_style(g_dom, dom_by_id(g_dom, hidden[i]), "display", "none");
    }
    dom_add_class(g_dom, dom_by_id(g_dom, "uiBase"), "onMenu");
}

static void test_dynamic(void)
{
    HtmlElem html = dom_root(g_dom);
    HtmlElem btn = dom_by_id(g_dom, "menuBtnHost");
    HtmlElem item = dom_query(g_dom, 0, ".menuItem");
    HtmlElem menu_window = dom_by_id(g_dom, "menuWindow");
    HtmlElem holder = dom_by_id(g_dom, "windowHolder");
    HtmlElem e;
    const CssComputed *c;
    CssComputed pe;
    uint32_t n;
    double t0;

    /* html.kn-menu-open lifts the pointer-events: none !important */
    dom_add_class(g_dom, html, "kn-menu-open");
    n = restyle();
    check_int("restyle count after html class change >= 50", n >= 50, 1);
    menu_open_preset();
    t0 = now_ms();
    n = restyle();
    printf("menu-open preset restyle: %.2f ms (%u elements)\n", now_ms() - t0, n);
    check_int("menu-open preset restyles the shown menu", n > 100, 1);
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

    /* :hover */
    dom_set_pseudo_state(g_dom, btn, DOM_PSEUDO_HOVER, 1);
    n = restyle();
    check_int("restyle count after hover (self + following siblings only)", n >= 1 && n <= 6, 1);
    check("#menuBtnHost", "background-color", "rgb(255, 255, 255)");   /* .buttonP:hover !important */
    check("#menuBtnHost", "box-shadow", "inset 0px -7px 0px 0px rgb(179, 179, 179)");
    dom_set_pseudo_state(g_dom, btn, DOM_PSEUDO_HOVER, 0);
    restyle();
    check("#menuBtnHost", "background-color", "rgb(180, 71, 255)");

    dom_set_pseudo_state(g_dom, item, DOM_PSEUDO_HOVER, 1);
    restyle();
    check(".menuItem", "border-left-width", "10px");
    check(".menuItem", "border-left-style", "solid");
    check(".menuItem", "border-left-color", "rgb(255, 255, 255)");
    check(".menuItem .iconProfile", "background-image", "url(../img/menu-icons/profile-white.png)");
    dom_set_pseudo_state(g_dom, item, DOM_PSEUDO_HOVER, 0);
    restyle();
    check(".menuItem", "border-left-width", "0px");
    check(".menuItem .iconProfile", "background-image", "url(../img/menu-icons/profile.png)");

    /* inline style precedence and dom_set_style */
    e = dom_by_id(g_dom, "errorNotification");
    dom_set_style(g_dom, e, "display", "block");
    restyle();
    check("#errorNotification", "display", "block");
    check_str("get_style display", dom_get_style(g_dom, e, "display"), "block");
    check_str("style attr text", dom_get_attr(g_dom, e, "style", NULL), "display: block;");
    dom_set_style(g_dom, e, "display", NULL);
    restyle();
    check("#errorNotification", "display", "block");                 /* div default */
    dom_set_style(g_dom, e, "background-color", "var(--red)");
    restyle();
    check("#errorNotification", "background-color", "rgb(237, 66, 66)"); /* var() in inline style */
    dom_set_style(g_dom, e, "width", "calc(50% - 10px)");
    restyle();
    check("#errorNotification", "width", "calc(50% - 10px)");

    /* #uiBase.onMenu descendant rule */
    check("#chatHolder", "left", "150px");                      /* #uiBase.onMenu #chatHolder */
    dom_remove_class(g_dom, dom_by_id(g_dom, "uiBase"), "onMenu");
    n = restyle();
    check("#chatHolder", "left", "20px");
    printf("uiBase.onMenu removal restyle: %u elements\n", n);
    dom_add_class(g_dom, dom_by_id(g_dom, "uiBase"), "onMenu");
    restyle();
    check("#chatHolder", "left", "150px");

    /* generated markup inside #menuWindow: sliders, switches, var(--yellow),
     * :placeholder-shown, <b>, ::before */
    dom_set_style(g_dom, holder, "display", "block");
    dom_set_html(g_dom, menu_window, k_fragment, strlen(k_fragment));
    restyle();
    check("#tslider", "height", "15px");
    check("#tslider", "width", "100%");
    check("#tslider", "appearance", "none");
    check("#tslider", "background-color", "rgb(168, 168, 168)");
    check("#tslider", "border-top-left-radius", "4px");
    n = cascade_pseudo_computed(g_dom, dom_by_id(g_dom, "tslider"), CSS_PE_SLIDER_THUMB, &pe);
    check_int("slider thumb matched declarations > 0", n > 0, 1);
    check_pe("#tslider", &pe, "-webkit-slider-thumb", "width", "15px");
    check_pe("#tslider", &pe, "-webkit-slider-thumb", "height", "25px");
    check_pe("#tslider", &pe, "-webkit-slider-thumb", "background-color", "rgb(33, 150, 243)");
    check_pe("#tslider", &pe, "-webkit-slider-thumb", "appearance", "none");
    check_pe("#tslider", &pe, "-webkit-slider-thumb", "cursor", "pointer");

    check("label.switch", "width", "65px");
    check("label.switch", "height", "34px");
    check("#tslide", "position", "absolute");
    c = cascade_elem_before(g_dom, dom_by_id(g_dom, "tslide"));
    check_pe("#tslide", c, "before", "content", "\"\"");
    check_pe("#tslide", c, "before", "width", "26px");
    check_pe("#tslide", c, "before", "height", "26px");
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
    check_pe("#tslide", c, "before", "transform", "translate(30px, 0px)"); /* input:checked + .slider:before */
    dom_set_checked(g_dom, dom_by_id(g_dom, "tcheck"), 0);
    restyle();
    c = cascade_elem_before(g_dom, dom_by_id(g_dom, "tslide"));
    check_pe("#tslide", c, "before", "transform", "none");

    check("#ttab", "color", "rgb(251, 192, 45)");                       /* var(--yellow) */
    check("#ttab", "border-bottom-color", "rgb(251, 192, 45)");         /* var() inside a shorthand */
    check("#ttab", "border-bottom-width", "4px");

    /* :placeholder-shown via the kn-placeholder-shown attribute */
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
    check_pe("#tbadge", c, "after", "content", "attr(data-badge)");
    check_pe("#tbadge", c, "after", "top", "146px");
    check_pe("#tbadge", c, "after", "font-size", "12px");
    check_pe("#tbadge", c, "after", "color", "rgb(255, 255, 255)");
    check("#tcanvas", "width", "300px");
    check("#tcanvas", "height", "150px");
    check("#telo", "width", "30px");                                   /* 1.5em at font-size 20px */
    check("#telo", "height", "30px");
    check("#tcolor", "width", "55px");                                 /* author over UA 50px */
    check("#tcolor", "border-left-color", "rgb(33, 150, 243)");        /* border: 4px solid var(--blue) */
    check("#tcolor", "border-left-width", "4px");
    check("#tcolor", "appearance", "none");
    n = cascade_pseudo_computed(g_dom, dom_by_id(g_dom, "tcolor"), CSS_PE_COLOR_SWATCH, &pe);
    check_int("color swatch pseudo matched", n > 0, 1);
    check("#tbold", "font-weight", "700");
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
    e = dom_by_id(g_dom, "androidPayB");
    check_int("first #androidPayB is the Apple button", strstr(dom_get_attr(g_dom, e, "onclick", NULL), "apple") != NULL, 1);
    dom_remove(g_dom, e);
    e = dom_by_id(g_dom, "androidPayB");
    check_int("second #androidPayB after removal", e != 0 && strstr(dom_get_attr(g_dom, e, "onclick", NULL), "google") != NULL, 1);

    /* custom property + calc(var()) and style.setProperty('--kn-*') */
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
        check_str("textContent after append", t, "hello");
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

static void test_host_window(void)
{
    size_t len;
    char *host = extract_host_html(&len);
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
    printf("host window: parse %.2f ms, restyle %.2f ms (%u elements computed, %u new elements)\n",
           t1 - t0, t2 - t1, n, dom_elem_count(g_dom) - before);
    check_int("host window element count ~2111", dom_elem_count(g_dom) - before > 2000, 1);
    /* only the displayed part is computed (inactive tabs are display:none) */
    check_int("host window restyle computes the displayed part", n > 300, 1);

    check("#menuWindow .sliderM", "height", "15px");
    check("#menuWindow .settNameSmall", "font-size", "18px");   /* second rule wins */
    check("#menuWindow .settNameSmall", "margin-top", "10px");  /* later margin shorthand over margin-top */
    check("#menuWindow .settNameSmall", "clear", "both");
    check("#menuWindow .settNameSmall", "color", "rgba(0, 0, 0, 0.5)");
    check("#menuWindow .hostOpt", "display", "inline-block");
    check("#menuWindow .hostOpt", "width", "203px");
    check("#menuWindow .hostOpt", "border-left-width", "2px");
    check("#menuWindow .hostOpt", "border-left-style", "solid");
    check("#menuWindow .sliderVal", "float", "right");
    check("#menuWindow .sliderVal", "width", "50px");
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
    g_dom = dom_create();
    html = read_file("ui/index.html", &len);
    check_int("dom_load_html", dom_load_html(g_dom, html, len), 0);
    free(html);
    printf("index.html: %u elements\n", dom_elem_count(g_dom));

    t0 = now_ms();
    g_ua = css_sheet_load(KN_UA_CSS, strlen(KN_UA_CSS), "ua.css");
    for (int i = 0; i < 6; i++) {
        size_t clen;
        char *css = read_file(k_sheet_files[i], &clen);
        g_sheets[i] = css_sheet_load(css, clen, k_sheet_files[i]);
        free(css);
        printf("%-32s %5u rules %5u selectors\n", k_sheet_files[i], css_sheet_rule_count(g_sheets[i]),
               css_sheet_selector_count(g_sheets[i]));
    }
    t1 = now_ms();
    printf("sheets loaded in %.2f ms\n", t1 - t0);
    check_int("main.css rules ~1,100", css_sheet_rule_count(g_sheets[0]) > 1000, 1);
    {
        const CssKeyframes *kf;
        uint32_t nf = 0;
        const CssFontFace *ff = css_sheet_font_faces(g_sheets[3], &nf);
        cascade_restyle(g_dom, g_ua, g_sheets, 6); /* sets the sheet set */
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
        check_str("material icons src", nf > 0 && ff[0].src_count > 0 ? ff[0].src[0] : "", "fonts/flUhRq6tzZclQEJ-Vdg-IuiaDsNc.woff2");
        ff = css_sheet_font_faces(g_sheets[0], &nf);
        check_int("main.css @font-face count", nf, 2);
        ff = css_sheet_font_faces(g_sheets[2], &nf);
        check_int("secular one @font-face count", nf, 3);
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

    test_static();
    test_dynamic();
    test_host_window();

    printf("%d passed, %d failed\n", g_pass, g_fail);

    for (int i = 0; i < 6; i++) {
        css_sheet_destroy(g_sheets[i]);
    }
    css_sheet_destroy(g_ua);
    cascade_shutdown();
    dom_destroy(g_dom);
    return g_fail;
}
