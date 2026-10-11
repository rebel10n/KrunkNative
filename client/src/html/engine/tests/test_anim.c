/*
 * test_anim.c - standalone tests for anim.c (CSS animations + transitions).
 *
 * Build (throwaway, from the repository root):
 *   cc -std=c11 -g -fsanitize=address,undefined -DKN_ANIM_NO_SHEET -Iclient/src/html/engine \
 *      client/src/html/engine/anim.c client/src/html/engine/util.c \
 *      client/src/html/engine/tests/test_anim.c -lm -o /tmp/test_anim
 *   /tmp/test_anim [path/to/ui/css]
 *
 * The 13 @keyframes blocks are read from ui/css/main.css + hud.css with a
 * small scanner (css_sheet.c / css_values.c are not required), turned into
 * AnimKeyframeSets, and sampled at known times.  Expected values are either
 * exact (linear/steps) or derived from an independent bisection bezier
 * solver defined here.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "anim.h"
#include "util.h"

/* ------------------------------------------------------------------------- */
/* Harness                                                                    */
/* ------------------------------------------------------------------------- */

static int g_checks, g_fails;

#define CHECK(cond) do { g_checks++; if (!(cond)) { g_fails++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_NEAR(a, b, eps) do { double _a = (a), _b = (b); g_checks++; \
    if (fabs(_a - _b) > (eps)) { g_fails++; \
    fprintf(stderr, "FAIL %s:%d: %s = %.6f, expected %.6f (+-%g)\n", __FILE__, __LINE__, #a, _a, _b, (double)(eps)); } } while (0)

/* Reference bezier: bisection on x(t), independent of anim_cubic_bezier. */
static double ref_bezier(double x1, double y1, double x2, double y2, double x)
{
    double lo = 0, hi = 1, t = 0.5;
    for (int i = 0; i < 200; i++) {
        t = 0.5 * (lo + hi);
        double u = 1 - t;
        double bx = 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t;
        if (bx < x) lo = t; else hi = t;
    }
    double u = 1 - t;
    return 3 * u * u * t * y1 + 3 * u * t * t * y2 + t * t * t;
}
static double ref_ease(double x) { return ref_bezier(0.25, 0.1, 0.25, 1, x); }
static double ref_ease_out(double x) { return ref_bezier(0, 0, 0.58, 1, x); }
static double ref_ease_in_out(double x) { return ref_bezier(0.42, 0, 0.58, 1, x); }

/* ------------------------------------------------------------------------- */
/* Value helpers                                                              */
/* ------------------------------------------------------------------------- */

static CssLength len_px(float px) { CssLength l = {0}; l.px = px; l.kind = CSS_LEN_VALUE; return l; }
static CssColor rgba(int r, int g, int b, int a) { CssColor c = {0}; c.r = (uint8_t)r; c.g = (uint8_t)g; c.b = (uint8_t)b; c.a = (uint8_t)a; c.kind = CSS_COLOR_RGBA; return c; }
static CssTiming timing_kind(int kind) { CssTiming t = {0}; t.kind = (uint8_t)kind; return t; }
static CssTiming timing_bezier(float x1, float y1, float x2, float y2)
{ CssTiming t = {0}; t.kind = CSS_TIMING_CUBIC_BEZIER; t.x1 = x1; t.y1 = y1; t.x2 = x2; t.y2 = y2; return t; }
static CssTiming timing_steps(int n, int pos) { CssTiming t = {0}; t.kind = CSS_TIMING_STEPS; t.steps = n; t.step_pos = (uint8_t)pos; return t; }

static CssAnimation anim_decl(const char *name, float dur, float delay, float iters, CssTiming timing, int dir, int fill)
{
    CssAnimation a = {0};
    a.name = kn_atom_cstr(name);
    a.duration_s = dur;
    a.delay_s = delay;
    a.iterations = iters;
    a.timing = timing;
    a.direction = (uint8_t)dir;
    a.fill = (uint8_t)fill;
    return a;
}

static CssTransition trans_decl(int prop, float dur, float delay, CssTiming timing)
{
    CssTransition t = {0};
    t.prop = (int16_t)prop;
    t.duration_s = dur;
    t.delay_s = delay;
    t.timing = timing;
    return t;
}

/* A plausible base style (css_values.c is not linked: fill by hand). */
static void base_init(CssComputed *c)
{
    memset(c, 0, sizeof *c);
    c->opacity = 1.0f;
    c->font_size = 16.0f;
    c->color = rgba(0, 0, 0, 255);
    c->background_color = rgba(0, 0, 0, 0);
    for (int i = 0; i < 4; i++) {
        c->border_color[i].kind = CSS_COLOR_CURRENT;
        c->border_radius[i] = len_px(0);
        c->margin[i] = len_px(0);
        c->padding[i] = len_px(0);
        c->inset[i].kind = CSS_LEN_AUTO;
    }
    c->outline_color.kind = CSS_COLOR_CURRENT;
    c->width.kind = CSS_LEN_AUTO;
    c->height.kind = CSS_LEN_AUTO;
    c->min_width = len_px(0);
    c->min_height = len_px(0);
    c->max_width.kind = CSS_LEN_NONE;
    c->max_height.kind = CSS_LEN_NONE;
    c->line_height.kind = CSS_LEN_NORMAL;
    c->letter_spacing.kind = CSS_LEN_NORMAL;
    c->z_index_auto = 1;
}

/* ------------------------------------------------------------------------- */
/* Minimal @keyframes scanner for the shipped CSS                             */
/* ------------------------------------------------------------------------- */

#define KF_EM_PX 16.0f   /* em context used when resolving keyframe lengths */

static const char *skip_ws(const char *p)
{
    for (;;) {
        while (*p && kn_is_space(*p)) p++;
        if (p[0] == '/' && p[1] == '*') {
            const char *e = strstr(p + 2, "*/");
            p = e ? e + 2 : p + strlen(p);
            continue;
        }
        return p;
    }
}

static int str_ieq_n(const char *a, size_t n, const char *lit)
{
    return kn_str_ieq(a, n, lit);
}

static int parse_named_color(const char *s, size_t n, CssColor *out)
{
    static const struct { const char *name; int r, g, b, a; } tab[] = {
        { "crimson", 220, 20, 60, 255 }, { "orange", 255, 165, 0, 255 }, { "lime", 0, 255, 0, 255 },
        { "mediumblue", 0, 0, 205, 255 }, { "transparent", 0, 0, 0, 0 }, { "white", 255, 255, 255, 255 },
        { "black", 0, 0, 0, 255 }, { "red", 255, 0, 0, 255 }, { "blue", 0, 0, 255, 255 },
    };
    for (size_t i = 0; i < sizeof tab / sizeof tab[0]; i++) {
        if (str_ieq_n(s, n, tab[i].name)) {
            *out = rgba(tab[i].r, tab[i].g, tab[i].b, tab[i].a);
            return 1;
        }
    }
    return 0;
}

static int parse_color_text(const char *s, size_t n, CssColor *out)
{
    s = kn_str_trim(s, &n);
    if (n > 0 && s[0] == '#') {
        const char *h = s + 1;
        size_t hn = n - 1;
        int v[8];
        if (hn != 3 && hn != 4 && hn != 6 && hn != 8) return 0;
        for (size_t i = 0; i < hn; i++) {
            if (!kn_is_hex(h[i])) return 0;
            v[i] = kn_hex_val(h[i]);
        }
        if (hn <= 4) {
            *out = rgba(v[0] * 17, v[1] * 17, v[2] * 17, hn == 4 ? v[3] * 17 : 255);
        } else {
            *out = rgba(v[0] * 16 + v[1], v[2] * 16 + v[3], v[4] * 16 + v[5], hn == 8 ? v[6] * 16 + v[7] : 255);
        }
        return 1;
    }
    if (n > 5 && (str_ieq_n(s, 4, "rgba") || str_ieq_n(s, 3, "rgb"))) {
        const char *q = strchr(s, '(');
        if (!q) return 0;
        double c[4] = { 0, 0, 0, 1 };
        int k = 0;
        q++;
        while (k < 4) {
            char *end;
            c[k++] = strtod(q, &end);
            if (end == q) return 0;
            q = end;
            while (*q == ' ' || *q == ',' || *q == '/') q++;
            if (*q == ')') break;
        }
        *out = rgba((int)(c[0] + 0.5), (int)(c[1] + 0.5), (int)(c[2] + 0.5), (int)(c[3] * 255 + 0.5));
        return 1;
    }
    return parse_named_color(s, n, out);
}

/* number + unit -> px (em resolved with KF_EM_PX) */
static int parse_length_text(const char *s, size_t n, CssLength *out)
{
    s = kn_str_trim(s, &n);
    char buf[64];
    if (n >= sizeof buf) return 0;
    memcpy(buf, s, n);
    buf[n] = 0;
    char *end;
    double v = strtod(buf, &end);
    if (end == buf) return 0;
    CssLength l = {0};
    l.kind = CSS_LEN_VALUE;
    if (!strcmp(end, "em")) l.px = (float)(v * KF_EM_PX);
    else if (!strcmp(end, "%")) l.pct = (float)v;
    else l.px = (float)v;
    *out = l;
    return 1;
}

static double parse_angle_deg(const char *s)
{
    char *end;
    double v = strtod(s, &end);
    if (!strncmp(end, "turn", 4)) v *= 360.0;
    else if (!strncmp(end, "rad", 3)) v *= 180.0 / 3.14159265358979;
    return v;
}

/* f(a, b) ... lists: calls cb per function. */
static int parse_transform_text(const char *s, size_t n, CssTransform *out)
{
    memset(out, 0, sizeof *out);
    const char *end = s + n;
    while (s < end) {
        s = skip_ws(s);
        if (s >= end) break;
        const char *paren = memchr(s, '(', (size_t)(end - s));
        if (!paren) return 0;
        const char *close = memchr(paren, ')', (size_t)(end - paren));
        if (!close) return 0;
        size_t fn = (size_t)(paren - s);
        char args[128];
        size_t an = (size_t)(close - paren - 1);
        if (an >= sizeof args) return 0;
        memcpy(args, paren + 1, an);
        args[an] = 0;
        if (out->count == CSS_MAX_TRANSFORM_OPS) return 0;
        CssTransformOp *op = &out->ops[out->count];
        memset(op, 0, sizeof *op);
        char *comma = strchr(args, ',');
        if (comma) *comma = 0;
        if (str_ieq_n(s, fn, "rotate")) {
            op->kind = CSS_TRANSFORM_ROTATE;
            op->angle_deg = (float)parse_angle_deg(args);
        } else if (str_ieq_n(s, fn, "scale")) {
            op->kind = CSS_TRANSFORM_SCALE;
            op->sx = (float)strtod(args, NULL);
            op->sy = comma ? (float)strtod(comma + 1, NULL) : op->sx;
        } else if (str_ieq_n(s, fn, "translate") || str_ieq_n(s, fn, "translateX") || str_ieq_n(s, fn, "translateY")) {
            op->kind = CSS_TRANSFORM_TRANSLATE;
            op->tx = len_px(0);
            op->ty = len_px(0);
            if (str_ieq_n(s, fn, "translateY")) parse_length_text(args, strlen(args), &op->ty);
            else parse_length_text(args, strlen(args), &op->tx);
            if (comma) parse_length_text(comma + 1, strlen(comma + 1), &op->ty);
        } else {
            return 0;   /* translateZ etc. are not in the keyframes we test */
        }
        out->count++;
        s = close + 1;
    }
    return 1;
}

static int parse_filter_text(const char *s, size_t n, AnimFilterList *out)
{
    memset(out, 0, sizeof *out);
    const char *end = s + n;
    while (s < end) {
        s = skip_ws(s);
        if (s >= end) break;
        const char *paren = memchr(s, '(', (size_t)(end - s));
        if (!paren) return 0;
        const char *close = memchr(paren, ')', (size_t)(end - paren));
        if (!close) return 0;
        size_t fn = (size_t)(paren - s);
        if (out->count == CSS_MAX_FILTER_OPS) return 0;
        CssFilterOp *op = &out->ops[out->count];
        memset(op, 0, sizeof *op);
        if (str_ieq_n(s, fn, "hue-rotate")) {
            op->kind = CSS_FILTER_HUE_ROTATE;
            op->amount = (float)parse_angle_deg(paren + 1);
        } else if (str_ieq_n(s, fn, "brightness")) {
            op->kind = CSS_FILTER_BRIGHTNESS;
            op->amount = (float)strtod(paren + 1, NULL);
        } else {
            return 0;
        }
        out->count++;
        s = close + 1;
    }
    return 1;
}

/* Applies one `name: value` declaration to a frame.  Mirrors what the real
 * loader does with css_computed_apply() + anim_prop_mask_from_css_prop(). */
static int kf_apply_decl(AnimKeyframeSet *set, AnimKeyframe *frame, const char *name, size_t nn,
                         const char *value, size_t vn)
{
    AnimValue v;
    memset(&v, 0, sizeof v);
    if (nn > 8 && !strncmp(name, "-webkit-", 8)) { name += 8; nn -= 8; }

    if (str_ieq_n(name, nn, "opacity")) {
        v.f = (float)strtod(value, NULL);
        anim_keyframe_set_value(set, frame, ANIM_OPACITY, &v);
        return 1;
    }
    if (str_ieq_n(name, nn, "font-size")) {
        CssLength l;
        if (!parse_length_text(value, vn, &l)) return 0;
        v.f = l.px;
        anim_keyframe_set_value(set, frame, ANIM_FONT_SIZE, &v);
        return 1;
    }
    if (str_ieq_n(name, nn, "width")) {
        if (!parse_length_text(value, vn, &v.len)) return 0;
        anim_keyframe_set_value(set, frame, ANIM_WIDTH, &v);
        return 1;
    }
    if (str_ieq_n(name, nn, "transform")) {
        if (!parse_transform_text(value, vn, &v.transform)) return 0;
        anim_keyframe_set_value(set, frame, ANIM_TRANSFORM, &v);
        return 1;
    }
    if (str_ieq_n(name, nn, "filter")) {
        if (!parse_filter_text(value, vn, &v.filter)) return 0;
        anim_keyframe_set_value(set, frame, ANIM_FILTER, &v);
        return 1;
    }
    static const struct { const char *name; int props[4]; int n; } colors[] = {
        { "color", { ANIM_COLOR }, 1 },
        { "background-color", { ANIM_BACKGROUND_COLOR }, 1 },
        { "border-right-color", { ANIM_BORDER_RIGHT_COLOR }, 1 },
        { "border-color", { ANIM_BORDER_TOP_COLOR, ANIM_BORDER_RIGHT_COLOR, ANIM_BORDER_BOTTOM_COLOR, ANIM_BORDER_LEFT_COLOR }, 4 },
    };
    for (size_t i = 0; i < sizeof colors / sizeof colors[0]; i++) {
        if (str_ieq_n(name, nn, colors[i].name)) {
            if (!parse_color_text(value, vn, &v.color)) return 0;
            for (int k = 0; k < colors[i].n; k++)
                anim_keyframe_set_value(set, frame, (AnimProp)colors[i].props[k], &v);
            return 1;
        }
    }
    return 0;
}

static int parse_offset(const char *s, size_t n, float *out)
{
    s = kn_str_trim(s, &n);
    if (str_ieq_n(s, n, "from")) { *out = 0.0f; return 1; }
    if (str_ieq_n(s, n, "to")) { *out = 1.0f; return 1; }
    if (n && s[n - 1] == '%') { *out = (float)(strtod(s, NULL) / 100.0); return 1; }
    return 0;
}

/* Returns the number of @keyframes blocks loaded from `text`. */
static int load_keyframes_text(AnimSystem *sys, const char *text)
{
    int loaded = 0;
    const char *p = text;
    while ((p = strstr(p, "@keyframes")) != NULL) {
        p = skip_ws(p + 10);
        const char *name = p;
        while (*p && !kn_is_space(*p) && *p != '{') p++;
        size_t name_len = (size_t)(p - name);
        p = skip_ws(p);
        if (*p != '{') continue;
        p++;
        AnimKeyframeSet *set = anim_keyframes_begin(sys, name, name_len);
        loaded++;
        for (;;) {
            p = skip_ws(p);
            if (*p == '}' || !*p) { if (*p) p++; break; }
            /* selector list */
            const char *sel = p;
            const char *brace = strchr(p, '{');
            if (!brace) break;
            float offsets[8];
            int noff = 0;
            const char *s = sel;
            while (s < brace && noff < 8) {
                const char *comma = memchr(s, ',', (size_t)(brace - s));
                const char *e = comma ? comma : brace;
                if (parse_offset(s, (size_t)(e - s), &offsets[noff])) noff++;
                s = e + 1;
            }
            p = brace + 1;
            /* body */
            const char *body = p;
            const char *close = strchr(p, '}');
            if (!close) break;
            AnimKeyframe *frames[8];
            for (int i = 0; i < noff; i++)
                frames[i] = anim_keyframes_add_frame(set, offsets[i], NULL);
            const char *d = body;
            while (d < close) {
                d = skip_ws(d);
                if (d >= close) break;
                const char *colon = memchr(d, ':', (size_t)(close - d));
                if (!colon) break;
                const char *semi = memchr(colon, ';', (size_t)(close - colon));
                const char *vend = semi ? semi : close;
                size_t nn = (size_t)(colon - d);
                const char *nm = kn_str_trim(d, &nn);
                size_t vn = (size_t)(vend - colon - 1);
                const char *val = kn_str_trim(colon + 1, &vn);
                for (int i = 0; i < noff; i++) {
                    if (!kf_apply_decl(set, frames[i], nm, nn, val, vn)) {
                        fprintf(stderr, "  (keyframes %.*s: unhandled %.*s: %.*s)\n",
                                (int)name_len, name, (int)nn, nm, (int)vn, val);
                        break;
                    }
                }
                d = vend + 1;
            }
            p = close + 1;
        }
    }
    return loaded;
}

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    size_t got = fread(buf, 1, (size_t)n, f);
    buf[got] = 0;
    fclose(f);
    return buf;
}

/* ------------------------------------------------------------------------- */
/* Dirty callback capture                                                     */
/* ------------------------------------------------------------------------- */

static struct { HtmlElem elem; unsigned flags; int calls; } g_dirty;

static void on_dirty(void *user, HtmlElem elem, unsigned flags)
{
    (void)user;
    g_dirty.elem = elem;
    g_dirty.flags |= flags;
    g_dirty.calls++;
}

static void dirty_reset(void) { memset(&g_dirty, 0, sizeof g_dirty); }

/* ------------------------------------------------------------------------- */
/* Tests                                                                      */
/* ------------------------------------------------------------------------- */

static void test_timing(void)
{
    /* cubic-bezier(0.5,0,0.5,1): published values 0.1059 / 0.5 / 0.8941 */
    CssTiming cb = timing_bezier(0.5f, 0, 0.5f, 1);
    CHECK_NEAR(anim_timing_eval(&cb, 0.25f), 0.1059, 2e-3);
    CHECK_NEAR(anim_timing_eval(&cb, 0.5f), 0.5, 1e-4);
    CHECK_NEAR(anim_timing_eval(&cb, 0.75f), 0.8941, 2e-3);
    for (int i = 0; i <= 20; i++) {
        float x = (float)i / 20.0f;
        CHECK_NEAR(anim_cubic_bezier(0.5f, 0, 0.5f, 1, x), ref_bezier(0.5, 0, 0.5, 1, x), 1e-4);
        CssTiming ease = timing_kind(CSS_TIMING_EASE);
        CHECK_NEAR(anim_timing_eval(&ease, x), ref_ease(x), 1e-4);
        CssTiming eo = timing_kind(CSS_TIMING_EASE_OUT);
        CHECK_NEAR(anim_timing_eval(&eo, x), ref_ease_out(x), 1e-4);
        CssTiming eio = timing_kind(CSS_TIMING_EASE_IN_OUT);
        CHECK_NEAR(anim_timing_eval(&eio, x), ref_ease_in_out(x), 1e-4);
        CssTiming ei = timing_kind(CSS_TIMING_EASE_IN);
        CHECK_NEAR(anim_timing_eval(&ei, x), ref_bezier(0.42, 0, 1, 1, x), 1e-4);
        CssTiming lin = timing_kind(CSS_TIMING_LINEAR);
        CHECK_NEAR(anim_timing_eval(&lin, x), x, 1e-7);
    }
    CHECK_NEAR(ref_ease(0.5), 0.8024, 1e-3);   /* sanity: the well-known ease(0.5) */

    /* steps(36) == steps(36, end) */
    CssTiming st = timing_steps(36, CSS_STEP_END);
    CHECK_NEAR(anim_timing_eval(&st, 0.0f), 0.0, 0);
    CHECK_NEAR(anim_timing_eval(&st, 0.5f), 18.0 / 36.0, 1e-6);
    CHECK_NEAR(anim_timing_eval(&st, 1.0f / 36.0f - 1e-4f), 0.0, 0);
    CHECK_NEAR(anim_timing_eval(&st, 1.0f / 36.0f), 1.0 / 36.0, 1e-6);
    CHECK_NEAR(anim_timing_eval(&st, 0.999f), 35.0 / 36.0, 1e-6);
    CHECK_NEAR(anim_timing_eval(&st, 1.0f), 1.0, 0);
    CssTiming ss = timing_steps(4, CSS_STEP_START);
    CHECK_NEAR(anim_timing_eval(&ss, 0.0f), 0.25, 1e-6);
    CHECK_NEAR(anim_timing_eval(&ss, 0.3f), 0.5, 1e-6);
    CHECK_NEAR(anim_timing_eval(&ss, 1.0f), 1.0, 0);
    CssTiming s2 = timing_steps(2, CSS_STEP_END);   /* sprayAnm's steps(2) */
    CHECK_NEAR(anim_timing_eval(&s2, 0.49f), 0.0, 0);
    CHECK_NEAR(anim_timing_eval(&s2, 0.51f), 0.5, 1e-6);
}

static void test_prop_masks(void)
{
    CHECK(anim_prop_mask_from_css_prop(CSS_PROP_ALL) == ANIM_PROP_ALL_MASK);
    CHECK(anim_prop_mask_from_css_prop(CSS_PROP_OPACITY) == ANIM_PROP_BIT(ANIM_OPACITY));
    CHECK(anim_prop_mask_from_css_prop(CSS_PROP_BORDER_LEFT) ==
          (ANIM_PROP_BIT(ANIM_BORDER_LEFT_WIDTH) | ANIM_PROP_BIT(ANIM_BORDER_LEFT_COLOR)));
    CHECK(anim_prop_mask_from_css_prop(CSS_PROP_BORDER_COLOR) ==
          (ANIM_PROP_BIT(ANIM_BORDER_TOP_COLOR) | ANIM_PROP_BIT(ANIM_BORDER_RIGHT_COLOR) |
           ANIM_PROP_BIT(ANIM_BORDER_BOTTOM_COLOR) | ANIM_PROP_BIT(ANIM_BORDER_LEFT_COLOR)));
    CHECK(anim_prop_mask_from_css_prop(CSS_PROP_DISPLAY) == 0);
    CHECK(anim_prop_mask_from_css_prop(CSS_PROP_ANIMATION) == 0);
    CHECK(anim_prop_from_css_prop(CSS_PROP_FILTER) == ANIM_FILTER);
    CHECK(anim_prop_from_css_prop(CSS_PROP_Z_INDEX) == -1);
    CHECK(anim_prop_dirty_flags(ANIM_FONT_SIZE) == ANIM_DIRTY_LAYOUT);
    CHECK(anim_prop_dirty_flags(ANIM_TRANSFORM) == ANIM_DIRTY_PAINT);
    CHECK(!strcmp(anim_prop_name(ANIM_BORDER_RIGHT_COLOR), "border-right-color"));

    /* get/set round trip through CssComputed */
    CssComputed c;
    base_init(&c);
    AnimValue v;
    v.len = len_px(42);
    anim_value_set(&c, ANIM_PADDING_LEFT, &v);
    CHECK(c.padding[CSS_LEFT].px == 42.0f);
    anim_value_get(&c, ANIM_PADDING_LEFT, &v);
    CHECK(v.len.px == 42.0f && v.len.kind == CSS_LEN_VALUE);
}

static void test_value_lerp(void)
{
    AnimValue a, b, o;
    /* colors: premultiplied (Chrome): red -> transparent keeps the hue */
    memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
    a.color = rgba(255, 0, 0, 255);
    b.color = rgba(0, 0, 0, 0);
    anim_value_lerp(ANIM_COLOR, &a, &b, 0.5f, &o);
    CHECK(o.color.r == 255 && o.color.g == 0 && o.color.b == 0 && o.color.a == 128);
    /* opaque colors: plain channel lerp */
    a.color = rgba(220, 20, 60, 255);
    b.color = rgba(255, 165, 0, 255);
    anim_value_lerp(ANIM_COLOR, &a, &b, 0.5f, &o);
    CHECK(o.color.r == 238 && o.color.g == 93 && o.color.b == 30 && o.color.a == 255);
    /* currentColor: discrete */
    b.color.kind = CSS_COLOR_CURRENT;
    CHECK(!anim_value_interpolable(ANIM_COLOR, &a, &b));
    anim_value_lerp(ANIM_COLOR, &a, &b, 0.4f, &o);
    CHECK(o.color.kind == CSS_COLOR_RGBA && o.color.r == 220);
    anim_value_lerp(ANIM_COLOR, &a, &b, 0.6f, &o);
    CHECK(o.color.kind == CSS_COLOR_CURRENT);

    /* lengths: px/pct mixed */
    memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
    a.len = len_px(10);
    b.len.kind = CSS_LEN_VALUE; b.len.pct = 50;
    anim_value_lerp(ANIM_WIDTH, &a, &b, 0.5f, &o);
    CHECK_NEAR(o.len.px, 5, 1e-6); CHECK_NEAR(o.len.pct, 25, 1e-6);
    b.len.kind = CSS_LEN_AUTO;
    CHECK(!anim_value_interpolable(ANIM_WIDTH, &a, &b));
    anim_value_lerp(ANIM_WIDTH, &a, &b, 0.75f, &o);
    CHECK(o.len.kind == CSS_LEN_AUTO);

    /* transform: none vs scale -> identity synthesised */
    memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
    b.transform.count = 1; b.transform.ops[0].kind = CSS_TRANSFORM_SCALE; b.transform.ops[0].sx = 0.5f; b.transform.ops[0].sy = 0.5f;
    anim_value_lerp(ANIM_TRANSFORM, &a, &b, 0.5f, &o);
    CHECK(o.transform.count == 1 && o.transform.ops[0].kind == CSS_TRANSFORM_SCALE);
    CHECK_NEAR(o.transform.ops[0].sx, 0.75, 1e-6);
    /* mismatched structure: discrete at 50% */
    a.transform.count = 1; a.transform.ops[0].kind = CSS_TRANSFORM_ROTATE; a.transform.ops[0].angle_deg = 90;
    CHECK(!anim_value_interpolable(ANIM_TRANSFORM, &a, &b));
    anim_value_lerp(ANIM_TRANSFORM, &a, &b, 0.49f, &o);
    CHECK(o.transform.ops[0].kind == CSS_TRANSFORM_ROTATE);
    anim_value_lerp(ANIM_TRANSFORM, &a, &b, 0.5f, &o);
    CHECK(o.transform.ops[0].kind == CSS_TRANSFORM_SCALE);
    /* translate pairwise */
    a.transform.ops[0].kind = CSS_TRANSFORM_TRANSLATE; a.transform.ops[0].tx = len_px(-6); a.transform.ops[0].ty = len_px(0);
    b.transform.ops[0].kind = CSS_TRANSFORM_TRANSLATE; b.transform.ops[0].tx = len_px(8); b.transform.ops[0].ty = len_px(0);
    anim_value_lerp(ANIM_TRANSFORM, &a, &b, 0.5f, &o);
    CHECK_NEAR(o.transform.ops[0].tx.px, 1, 1e-6);

    /* filter: none vs hue-rotate+brightness -> identity (0deg, 1) */
    memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
    b.filter.count = 2;
    b.filter.ops[0].kind = CSS_FILTER_HUE_ROTATE; b.filter.ops[0].amount = 180;
    b.filter.ops[1].kind = CSS_FILTER_BRIGHTNESS; b.filter.ops[1].amount = 2;
    anim_value_lerp(ANIM_FILTER, &a, &b, 0.5f, &o);
    CHECK(o.filter.count == 2);
    CHECK_NEAR(o.filter.ops[0].amount, 90, 1e-5);
    CHECK_NEAR(o.filter.ops[1].amount, 1.5, 1e-6);

    /* box-shadow */
    memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
    a.shadow.count = 1; a.shadow.shadow.color = rgba(0, 0, 0, 255); a.shadow.shadow.blur = 0;
    b.shadow.count = 1; b.shadow.shadow.color = rgba(0, 0, 0, 255); b.shadow.shadow.blur = 10; b.shadow.shadow.y = 4;
    anim_value_lerp(ANIM_BOX_SHADOW, &a, &b, 0.25f, &o);
    CHECK_NEAR(o.shadow.shadow.blur, 2.5, 1e-6);
    CHECK_NEAR(o.shadow.shadow.y, 1, 1e-6);
}

/* Shared: keyframes loaded from the shipped CSS. */
static AnimSystem g_sys;

static int sample(HtmlElem e, const CssComputed *base, CssComputed *out, double now, const CssComputed **res)
{
    *res = anim_apply(&g_sys, e, base, out, now);
    return *res != base;
}

/* zoom: keyframe lengths (unzoomed) scale with the element's zoom; lists of
 * box-shadows interpolate pairwise, padding with transparent shadows. */
static void test_zoom_and_shadow_lists(void)
{
    CssComputed base, out;
    const CssComputed *r;
    base_init(&base);
    base.zoom = 0.5f;
    base.font_size = 16.0f;                     /* 32px under zoom .5 */
    base.animations[0] = anim_decl("grow", 0.8f, 0, INFINITY, timing_kind(CSS_TIMING_LINEAR), CSS_ANIM_DIRECTION_NORMAL, CSS_ANIM_FILL_NONE);
    base.animation_count = 1;
    HtmlElem e = 91;
    CHECK(anim_update_elem(&g_sys, e, &base, 1000) == 1);
    CHECK(sample(e, &base, &out, 1000, &r));
    CHECK_NEAR(r->font_size, 15, 1e-4);         /* `from { font-size: 30px }` * 0.5 */
    CHECK(sample(e, &base, &out, 1400, &r));
    CHECK_NEAR(r->font_size, 15.5, 1e-4);       /* halfway to the zoomed base 16 */
    anim_remove_elem(&g_sys, e);

    AnimValue a, b, o;
    memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
    a.shadow.count = 2;
    a.shadow.shadow.color = rgba(255, 0, 0, 255); a.shadow.shadow.x = 2;
    a.shadow.more[0].color = rgba(0, 0, 255, 255); a.shadow.more[0].blur = 8;
    b.shadow.count = 1;
    b.shadow.shadow.color = rgba(255, 0, 0, 255); b.shadow.shadow.x = 6;
    CHECK(anim_value_interpolable(ANIM_BOX_SHADOW, &a, &b));
    anim_value_lerp(ANIM_BOX_SHADOW, &a, &b, 0.5f, &o);
    CHECK(o.shadow.count == 2);
    CHECK_NEAR(o.shadow.shadow.x, 4, 1e-6);
    CHECK_NEAR(o.shadow.more[0].blur, 4, 1e-6);           /* towards the padding shadow */
    CHECK(o.shadow.more[0].color.a == 128 || o.shadow.more[0].color.a == 127);
    b.shadow.shadow.inset = 1;                              /* inset mismatch -> discrete */
    CHECK(!anim_value_interpolable(ANIM_BOX_SHADOW, &a, &b));
    memset(&b, 0, sizeof b);                                /* none -> list: interpolable */
    CHECK(anim_value_interpolable(ANIM_BOX_SHADOW, &b, &a));
}

static void test_grow(void)
{
    const AnimKeyframeSet *grow = anim_keyframes_find(&g_sys, kn_atom_cstr("grow"));
    CHECK(grow && grow->count == 1 && grow->frames[0].offset == 0.0f);
    CHECK(grow && (grow->props == (ANIM_PROP_BIT(ANIM_OPACITY) | ANIM_PROP_BIT(ANIM_FONT_SIZE))));

    /* #instructions { font-size: 32px; animation: grow 0.8s infinite alternate } */
    CssComputed base, out;
    const CssComputed *r;
    base_init(&base);
    base.font_size = 32.0f;
    base.animations[0] = anim_decl("grow", 0.8f, 0, INFINITY, timing_kind(CSS_TIMING_EASE), CSS_ANIM_DIRECTION_ALTERNATE, CSS_ANIM_FILL_NONE);
    base.animation_count = 1;

    HtmlElem e = 11;
    double t0 = 5000.0;
    CHECK(anim_update_elem(&g_sys, e, &base, t0) == 1);
    CHECK(anim_has_active(&g_sys, t0));
    CHECK(anim_next_wakeup(&g_sys, t0) == t0);

    CHECK(sample(e, &base, &out, t0, &r));
    CHECK_NEAR(r->opacity, 0.3, 1e-6);
    CHECK_NEAR(r->font_size, 30, 1e-5);

    double e05 = ref_ease(0.5);
    CHECK(sample(e, &base, &out, t0 + 400, &r));
    CHECK_NEAR(r->opacity, 0.3 + 0.7 * e05, 1e-4);
    CHECK_NEAR(r->font_size, 30 + 2 * e05, 1e-4);

    /* t = 0.8s: second iteration, alternate -> reversed, progress 0 -> base */
    CHECK(sample(e, &base, &out, t0 + 800, &r));
    CHECK_NEAR(r->opacity, 1.0, 1e-6);
    CHECK_NEAR(r->font_size, 32, 1e-5);

    /* t = 1.2s: reversed iteration at 0.5 -> same as 0.4s */
    CHECK(sample(e, &base, &out, t0 + 1200, &r));
    CHECK_NEAR(r->opacity, 0.3 + 0.7 * e05, 1e-4);
    CHECK_NEAR(r->font_size, 30 + 2 * e05, 1e-4);

    /* t = 1.6s: back at the from frame */
    CHECK(sample(e, &base, &out, t0 + 1600, &r));
    CHECK_NEAR(r->opacity, 0.3, 1e-5);

    /* restyle keeps the running animation (same start time) */
    CHECK(anim_update_elem(&g_sys, e, &base, t0 + 1600) == 1);
    CHECK(sample(e, &base, &out, t0 + 1600, &r));
    CHECK_NEAR(r->opacity, 0.3, 1e-5);

    /* other properties are untouched copies of base */
    CHECK(r->color.r == 0 && r->width.kind == CSS_LEN_AUTO && r->animation_count == 1);

    /* dirty flags: font-size -> layout, opacity -> paint */
    dirty_reset();
    anim_tick(&g_sys, t0 + 1700);
    CHECK(g_dirty.calls == 1 && g_dirty.elem == e);
    CHECK(g_dirty.flags == (ANIM_DIRTY_LAYOUT | ANIM_DIRTY_PAINT));

    /* animation-name removed -> record dropped */
    base.animation_count = 0;
    CHECK(anim_update_elem(&g_sys, e, &base, t0 + 1800) == 0);
    CHECK(!sample(e, &base, &out, t0 + 1800, &r));
    uint32_t ne;
    anim_stats(&g_sys, &ne, NULL, NULL);
    CHECK(ne == 0);
}

static void test_scale_bob(void)
{
    CssComputed base, out;
    const CssComputed *r;
    base_init(&base);
    base.animations[0] = anim_decl("scaleBob", 0.4f, 0, INFINITY, timing_kind(CSS_TIMING_EASE_IN_OUT), CSS_ANIM_DIRECTION_ALTERNATE, CSS_ANIM_FILL_NONE);
    base.animation_count = 1;
    HtmlElem e = 12;
    anim_update_elem(&g_sys, e, &base, 0);

    CHECK(sample(e, &base, &out, 0, &r));
    CHECK(r->transform.count == 1 && r->transform.ops[0].kind == CSS_TRANSFORM_SCALE);
    CHECK_NEAR(r->transform.ops[0].sx, 1.0, 1e-6);
    CHECK(sample(e, &base, &out, 400, &r));
    CHECK_NEAR(r->transform.ops[0].sx, 0.9, 1e-6);
    CHECK_NEAR(r->transform.ops[0].sy, 0.9, 1e-6);
    CHECK(sample(e, &base, &out, 200, &r));
    CHECK_NEAR(r->transform.ops[0].sx, 1.0 - 0.1 * ref_ease_in_out(0.5), 1e-5);   /* = 0.95 */
    CHECK(sample(e, &base, &out, 100, &r));
    CHECK_NEAR(r->transform.ops[0].sx, 1.0 - 0.1 * ref_ease_in_out(0.25), 1e-5);
    anim_remove_elem(&g_sys, e);
}

static void test_lds_ring(void)
{
    /* .lds-ring div { animation: lds-ring 1.2s cubic-bezier(0.5,0,0.5,1) infinite }
       nth-child(1..3) { animation-delay: -0.45s / -0.3s / -0.15s } */
    const float delays[4] = { -0.45f, -0.3f, -0.15f, 0.0f };
    CssComputed base[4], out;
    const CssComputed *r;
    double t0 = 1000.0;
    float rot[4];
    for (int i = 0; i < 4; i++) {
        base_init(&base[i]);
        base[i].animations[0] = anim_decl("lds-ring", 1.2f, delays[i], INFINITY, timing_bezier(0.5f, 0, 0.5f, 1), CSS_ANIM_DIRECTION_NORMAL, CSS_ANIM_FILL_NONE);
        base[i].animation_count = 1;
        anim_update_elem(&g_sys, (HtmlElem)(20 + i), &base[i], t0);
        CHECK(sample((HtmlElem)(20 + i), &base[i], &out, t0, &r));
        CHECK(r->transform.count == 1 && r->transform.ops[0].kind == CSS_TRANSFORM_ROTATE);
        rot[i] = r->transform.ops[0].angle_deg;
        double expect = 360.0 * ref_bezier(0.5, 0, 0.5, 1, -delays[i] / 1.2);
        CHECK_NEAR(rot[i], expect, 1e-2);
    }
    CHECK(rot[0] > rot[1] && rot[1] > rot[2] && rot[2] > rot[3]);
    CHECK_NEAR(rot[3], 0, 1e-6);
    /* a full period later everything is back where it started */
    CHECK(sample(21, &base[1], &out, t0 + 1200, &r));
    CHECK_NEAR(r->transform.ops[0].angle_deg, rot[1], 1e-2);
    /* just before the wrap, the delayed element is near 360 */
    CHECK(sample(23, &base[3], &out, t0 + 1199.9, &r));
    CHECK(r->transform.ops[0].angle_deg > 359.0f);
    for (int i = 0; i < 4; i++) anim_remove_elem(&g_sys, (HtmlElem)(20 + i));
}

static void test_rgb_hue(void)
{
    /* animation: rgbHue 0.8s steps(36) infinite; hue-rotate 10deg -> 360deg, brightness(2) */
    CssComputed base, out;
    const CssComputed *r;
    base_init(&base);
    base.animations[0] = anim_decl("rgbHue", 0.8f, 0, INFINITY, timing_steps(36, CSS_STEP_END), CSS_ANIM_DIRECTION_NORMAL, CSS_ANIM_FILL_NONE);
    base.animation_count = 1;
    HtmlElem e = 30;
    anim_update_elem(&g_sys, e, &base, 0);

    CHECK(sample(e, &base, &out, 10, &r));
    CHECK(r->filter_count == 2 && r->filters[0].kind == CSS_FILTER_HUE_ROTATE && r->filters[1].kind == CSS_FILTER_BRIGHTNESS);
    CHECK_NEAR(r->filters[0].amount, 10, 1e-4);
    CHECK_NEAR(r->filters[1].amount, 2, 1e-6);
    CHECK(sample(e, &base, &out, 400, &r));
    CHECK_NEAR(r->filters[0].amount, 10 + 350.0 * 18 / 36, 1e-3);
    double seg = 800.0 / 36.0;
    CHECK(sample(e, &base, &out, seg + 0.01, &r));
    CHECK_NEAR(r->filters[0].amount, 10 + 350.0 / 36, 1e-3);
    CHECK(sample(e, &base, &out, seg - 0.01, &r));
    CHECK_NEAR(r->filters[0].amount, 10, 1e-3);

    /* stepped: wakeup is the next step boundary, dirty only when it changes */
    CHECK_NEAR(anim_next_wakeup(&g_sys, 5.0), seg, 1e-6);
    CHECK_NEAR(anim_next_wakeup(&g_sys, seg + 1), 2 * seg, 1e-6);
    dirty_reset();
    anim_tick(&g_sys, 1.0);
    CHECK(g_dirty.calls == 1);           /* first tick: phase became active */
    dirty_reset();
    anim_tick(&g_sys, 2.0);
    CHECK(g_dirty.calls == 0);           /* same step: nothing changed */
    anim_tick(&g_sys, seg + 0.5);
    CHECK(g_dirty.calls == 1 && g_dirty.flags == ANIM_DIRTY_PAINT);
    anim_remove_elem(&g_sys, e);
}

static void test_rainbow(void)
{
    const AnimKeyframeSet *set = anim_keyframes_find(&g_sys, kn_atom_cstr("rainbow"));
    CHECK(set && set->count == 6);
    CssComputed base, out;
    const CssComputed *r;
    base_init(&base);
    base.animations[0] = anim_decl("rainbow", 0.5f, 0, INFINITY, timing_kind(CSS_TIMING_LINEAR), CSS_ANIM_DIRECTION_NORMAL, CSS_ANIM_FILL_NONE);
    base.animation_count = 1;
    HtmlElem e = 40;
    anim_update_elem(&g_sys, e, &base, 0);

    CHECK(sample(e, &base, &out, 0, &r));
    CHECK(r->color.r == 220 && r->color.g == 20 && r->color.b == 60 && r->color.a == 255);
    for (int i = 0; i < 4; i++)
        CHECK(r->border_color[i].kind == CSS_COLOR_RGBA && r->border_color[i].r == 220);
    CHECK(sample(e, &base, &out, 50, &r));    /* 10%: half way Crimson -> orange */
    CHECK(r->color.r == 238 && r->color.g == 93 && r->color.b == 30);
    CHECK(sample(e, &base, &out, 300, &r));   /* 60%: lime */
    CHECK(r->color.r == 0 && r->color.g == 255 && r->color.b == 0);
    CHECK(sample(e, &base, &out, 450, &r));   /* 90%: MediumBlue -> Crimson */
    CHECK(r->color.r == 110 && r->color.g == 10 && r->color.b == 133);
    CHECK(sample(e, &base, &out, 500, &r));   /* wrap */
    CHECK(r->color.r == 220 && r->color.g == 20 && r->color.b == 60);
    anim_remove_elem(&g_sys, e);

    /* poT: `0%, 100%` + 25/50/75% = 5 frames with holds */
    const AnimKeyframeSet *pot = anim_keyframes_find(&g_sys, kn_atom_cstr("poT"));
    CHECK(pot && pot->count == 5);
}

static void test_shake(void)
{
    /* animation: shake 0.5s; frames 20%..100%, implicit 0% = base (none) */
    const AnimKeyframeSet *set = anim_keyframes_find(&g_sys, kn_atom_cstr("shake"));
    CHECK(set && set->count == 9 && set->frames[0].offset == 0.2f);
    CssComputed base, out;
    const CssComputed *r;
    base_init(&base);
    base.animations[0] = anim_decl("shake", 0.5f, 0, 1, timing_kind(CSS_TIMING_EASE), CSS_ANIM_DIRECTION_NORMAL, CSS_ANIM_FILL_NONE);
    base.animation_count = 1;
    HtmlElem e = 50;
    double t0 = 100.0;
    anim_update_elem(&g_sys, e, &base, t0);

    CHECK(sample(e, &base, &out, t0 + 50, &r));   /* 10%: none -> 6px, ease(0.5) */
    CHECK(r->transform.count == 1 && r->transform.ops[0].kind == CSS_TRANSFORM_TRANSLATE);
    CHECK_NEAR(r->transform.ops[0].tx.px, 6 * ref_ease(0.5), 1e-4);
    CHECK(sample(e, &base, &out, t0 + 125, &r));  /* 25%: 6 -> -6, ease applies per interval */
    CHECK_NEAR(r->transform.ops[0].tx.px, 6 - 12 * ref_ease(0.5), 1e-4);
    CHECK(sample(e, &base, &out, t0 + 250, &r));  /* 50%: -8 */
    CHECK_NEAR(r->transform.ops[0].tx.px, -8, 1e-5);
    CHECK(!sample(e, &base, &out, t0 + 500, &r)); /* end: fill none -> base untouched */
    CHECK(r->transform.count == 0);
    CHECK(sample(e, &base, &out, t0 + 499.99, &r) && r->transform.count == 1);

    dirty_reset();
    anim_tick(&g_sys, t0 + 250);   /* active */
    anim_tick(&g_sys, t0 + 600);   /* ended: one more dirty to settle */
    CHECK(g_dirty.calls == 2);
    dirty_reset();
    anim_tick(&g_sys, t0 + 700);
    CHECK(g_dirty.calls == 0);
    CHECK(!anim_has_active(&g_sys, t0 + 700));
    CHECK(isinf(anim_next_wakeup(&g_sys, t0 + 700)));

    /* finished animations do not replay on restyle */
    CHECK(anim_update_elem(&g_sys, e, &base, t0 + 800) == 1);
    CHECK(!sample(e, &base, &out, t0 + 800, &r));
    anim_remove_elem(&g_sys, e);
}

static void test_fill_and_negative_delay(void)
{
    /* .terminalEffect: typewriter 4s steps(44) 1s 1 normal both, width 0 -> 25.25em */
    CssComputed base, out;
    const CssComputed *r;
    base_init(&base);
    base.width = len_px(999);
    base.animations[0] = anim_decl("typewriter", 4.0f, 1.0f, 1, timing_steps(44, CSS_STEP_END), CSS_ANIM_DIRECTION_NORMAL, CSS_ANIM_FILL_BOTH);
    base.animations[1] = anim_decl("blinkTextCursor", 0.5f, 0, INFINITY, timing_steps(44, CSS_STEP_END), CSS_ANIM_DIRECTION_NORMAL, CSS_ANIM_FILL_NONE);
    base.animation_count = 2;
    HtmlElem e = 60;
    anim_update_elem(&g_sys, e, &base, 0);

    CHECK(sample(e, &base, &out, 0, &r));          /* delay: fill backwards -> from */
    CHECK_NEAR(r->width.px, 0, 1e-6);
    CHECK_NEAR(anim_next_wakeup(&g_sys, 0), 500.0 / 44.0, 1e-6);   /* both stepped: next blink step */
    CHECK(sample(e, &base, &out, 3000, &r));       /* 2s in: 22/44 */
    CHECK_NEAR(r->width.px, 25.25f * KF_EM_PX * 0.5, 1e-3);
    CHECK(sample(e, &base, &out, 6000, &r));       /* after: fill forwards -> to */
    CHECK_NEAR(r->width.px, 25.25f * KF_EM_PX, 1e-3);
    /* blinkTextCursor at 50%: #33d011 -> transparent, premultiplied => alpha only */
    CHECK(sample(e, &base, &out, 250, &r));
    CHECK(r->border_color[CSS_RIGHT].r == 0x33 && r->border_color[CSS_RIGHT].g == 0xd0 && r->border_color[CSS_RIGHT].a == 128);
    CHECK(r->border_color[CSS_TOP].kind == CSS_COLOR_CURRENT);   /* other sides untouched */
    anim_remove_elem(&g_sys, e);

    /* negative delay beyond the duration of a finite animation: already over */
    base_init(&base);
    base.animations[0] = anim_decl("spin", 2.0f, -3.0f, 1, timing_kind(CSS_TIMING_LINEAR), CSS_ANIM_DIRECTION_NORMAL, CSS_ANIM_FILL_FORWARDS);
    base.animation_count = 1;
    anim_update_elem(&g_sys, e, &base, 0);
    CHECK(sample(e, &base, &out, 0, &r));
    CHECK_NEAR(r->transform.ops[0].angle_deg, 360, 1e-4);
    base.animations[0].fill = CSS_ANIM_FILL_NONE;
    anim_update_elem(&g_sys, e, &base, 0);
    CHECK(!sample(e, &base, &out, 0, &r));
    anim_remove_elem(&g_sys, e);

    /* paused: frozen */
    base_init(&base);
    base.animations[0] = anim_decl("spin", 2.0f, 0, INFINITY, timing_kind(CSS_TIMING_LINEAR), CSS_ANIM_DIRECTION_NORMAL, CSS_ANIM_FILL_NONE);
    base.animation_count = 1;
    anim_update_elem(&g_sys, e, &base, 0);
    base.animations[0].play_state = CSS_ANIM_PLAY_PAUSED;
    anim_update_elem(&g_sys, e, &base, 500);
    CHECK(sample(e, &base, &out, 1500, &r));
    CHECK_NEAR(r->transform.ops[0].angle_deg, 90, 1e-4);
    CHECK(!anim_has_active(&g_sys, 1500));
    base.animations[0].play_state = CSS_ANIM_PLAY_RUNNING;
    anim_update_elem(&g_sys, e, &base, 1500);
    CHECK(sample(e, &base, &out, 2000, &r));
    CHECK_NEAR(r->transform.ops[0].angle_deg, 180, 1e-4);
    anim_remove_elem(&g_sys, e);
}

static void test_unknown_name(void)
{
    /* animation: sprayAnm 1.4s steps(2) infinite -> no keyframes -> no-op */
    CssComputed base, out;
    const CssComputed *r;
    base_init(&base);
    base.animations[0] = anim_decl("sprayAnm", 1.4f, 0, INFINITY, timing_steps(2, CSS_STEP_END), CSS_ANIM_DIRECTION_NORMAL, CSS_ANIM_FILL_NONE);
    base.animation_count = 1;
    HtmlElem e = 70;
    CHECK(anim_update_elem(&g_sys, e, &base, 0) == 0);
    CHECK(!sample(e, &base, &out, 700, &r));
    CHECK(!anim_has_active(&g_sys, 700));
    dirty_reset();
    anim_tick(&g_sys, 700);
    CHECK(g_dirty.calls == 0);
    anim_remove_elem(&g_sys, e);
}

static void test_transition_all(void)
{
    /* .menuItem { transition: all 0.15s ease-in-out } .menuItem:hover { border-left: 10px solid #fff } */
    CssComputed base, out;
    const CssComputed *r;
    base_init(&base);
    base.transitions[0] = trans_decl(CSS_PROP_ALL, 0.15f, 0, timing_kind(CSS_TIMING_EASE_IN_OUT));
    base.transition_count = 1;
    HtmlElem e = 80;
    double t0 = 1000.0;
    CHECK(anim_update_elem(&g_sys, e, &base, t0 - 500) == 0);   /* first style: snapshot only */
    CHECK(!sample(e, &base, &out, t0 - 500, &r));
    uint32_t ne;
    anim_stats(&g_sys, &ne, NULL, NULL);
    CHECK(ne == 1);   /* snapshot kept because the style declares a transition */

    base.border_width[CSS_LEFT] = 10.0f;
    base.border_color[CSS_LEFT] = rgba(255, 255, 255, 255);   /* currentColor -> rgb: discrete, no transition */
    base.border_style[CSS_LEFT] = CSS_BORDER_STYLE_SOLID;
    CHECK(anim_update_elem(&g_sys, e, &base, t0) == 1);
    uint32_t nt;
    anim_stats(&g_sys, NULL, NULL, &nt);
    CHECK(nt == 1);

    CHECK(sample(e, &base, &out, t0, &r));
    CHECK_NEAR(r->border_width[CSS_LEFT], 0, 1e-6);
    CHECK(r->border_color[CSS_LEFT].r == 255);   /* discrete change lands immediately */
    CHECK(sample(e, &base, &out, t0 + 75, &r));
    CHECK_NEAR(r->border_width[CSS_LEFT], 10 * ref_ease_in_out(0.5), 1e-4);   /* = 5 */
    CHECK_NEAR(r->border_width[CSS_LEFT], 5, 1e-4);
    CHECK(sample(e, &base, &out, t0 + 150, &r));
    CHECK_NEAR(r->border_width[CSS_LEFT], 10, 1e-6);
    CHECK(sample(e, &base, &out, t0 + 37.5, &r));
    CHECK_NEAR(r->border_width[CSS_LEFT], 10 * ref_ease_in_out(0.25), 1e-4);

    CHECK(anim_next_wakeup(&g_sys, t0 + 10) == t0 + 10);
    dirty_reset();
    anim_tick(&g_sys, t0 + 75);
    CHECK(g_dirty.calls == 1 && g_dirty.flags == ANIM_DIRTY_LAYOUT);
    dirty_reset();
    anim_tick(&g_sys, t0 + 150);
    CHECK(g_dirty.calls == 1);   /* final settle, then dropped */
    anim_stats(&g_sys, NULL, NULL, &nt);
    CHECK(nt == 0);
    CHECK(!anim_has_active(&g_sys, t0 + 150));
    CHECK(!sample(e, &base, &out, t0 + 151, &r));

    /* a restyle with no change starts nothing */
    CHECK(anim_update_elem(&g_sys, e, &base, t0 + 200) == 0);
    anim_remove_elem(&g_sys, e);
}

static void test_transition_transform(void)
{
    /* .button { transition: transform 0.1s ease-out } .button:hover { transform: scale(.95) } */
    CssComputed base, out;
    const CssComputed *r;
    base_init(&base);
    base.transitions[0] = trans_decl(CSS_PROP_TRANSFORM, 0.1f, 0, timing_kind(CSS_TIMING_EASE_OUT));
    base.transition_count = 1;
    base.transform.count = 1;
    base.transform.ops[0].kind = CSS_TRANSFORM_SCALE;
    base.transform.ops[0].sx = base.transform.ops[0].sy = 1.0f;
    HtmlElem e = 81;
    anim_update_elem(&g_sys, e, &base, 0);
    base.transform.ops[0].sx = base.transform.ops[0].sy = 0.95f;
    CHECK(anim_update_elem(&g_sys, e, &base, 0) == 1);
    CHECK(sample(e, &base, &out, 50, &r));
    CHECK_NEAR(r->transform.ops[0].sx, 1 - 0.05 * ref_ease_out(0.5), 1e-5);
    CHECK_NEAR(r->transform.ops[0].sy, 1 - 0.05 * ref_ease_out(0.5), 1e-5);
    CHECK(sample(e, &base, &out, 100, &r));
    CHECK_NEAR(r->transform.ops[0].sx, 0.95, 1e-6);

    /* opacity is not covered by `transform`: changes land immediately */
    base.opacity = 0.5f;
    anim_update_elem(&g_sys, e, &base, 50);
    CHECK(sample(e, &base, &out, 60, &r));
    CHECK_NEAR(r->opacity, 0.5, 1e-6);
    anim_remove_elem(&g_sys, e);
}

static void test_transition_opacity_and_reverse(void)
{
    /* hud: transition: opacity 0.5s linear */
    CssComputed base, out;
    const CssComputed *r;
    base_init(&base);
    base.transitions[0] = trans_decl(CSS_PROP_OPACITY, 0.5f, 0, timing_kind(CSS_TIMING_LINEAR));
    base.transition_count = 1;
    HtmlElem e = 82;
    anim_update_elem(&g_sys, e, &base, 0);
    base.opacity = 0.0f;
    anim_update_elem(&g_sys, e, &base, 0);
    CHECK(sample(e, &base, &out, 250, &r));
    CHECK_NEAR(r->opacity, 0.5, 1e-6);
    CHECK(sample(e, &base, &out, 100, &r));
    CHECK_NEAR(r->opacity, 0.8, 1e-6);

    /* reverse mid-way: 0.5 -> 1 with the reversing shortening factor 0.5 => 250 ms */
    base.opacity = 1.0f;
    CHECK(anim_update_elem(&g_sys, e, &base, 250) == 1);
    CHECK(sample(e, &base, &out, 250, &r));
    CHECK_NEAR(r->opacity, 0.5, 1e-6);
    CHECK(sample(e, &base, &out, 375, &r));
    CHECK_NEAR(r->opacity, 0.75, 1e-6);
    CHECK(sample(e, &base, &out, 500, &r));
    CHECK_NEAR(r->opacity, 1.0, 1e-6);
    anim_tick(&g_sys, 500);
    uint32_t nt;
    anim_stats(&g_sys, NULL, NULL, &nt);
    CHECK(nt == 0);

    /* retarget to a third value (not a reversal): full duration from current */
    base.opacity = 0.0f;
    anim_update_elem(&g_sys, e, &base, 1000);
    base.opacity = 0.2f;
    anim_update_elem(&g_sys, e, &base, 1250);   /* current 0.5 -> 0.2 over 500 ms */
    CHECK(sample(e, &base, &out, 1500, &r));
    CHECK_NEAR(r->opacity, 0.35, 1e-6);
    CHECK(sample(e, &base, &out, 1750, &r));
    CHECK_NEAR(r->opacity, 0.2, 1e-6);

    /* same target again mid-flight: keep running */
    anim_update_elem(&g_sys, e, &base, 1600);
    CHECK(sample(e, &base, &out, 1625, &r));
    CHECK_NEAR(r->opacity, 0.5 - 0.3 * 0.75, 1e-6);
    anim_remove_elem(&g_sys, e);
}

static void test_transition_shorthand_all_and_delay(void)
{
    /* transition: 0.4s  => property omitted = all, timing ease */
    CssComputed base, out;
    const CssComputed *r;
    base_init(&base);
    base.transitions[0] = trans_decl(CSS_PROP_ALL, 0.4f, 0, timing_kind(CSS_TIMING_EASE));
    base.transition_count = 1;
    base.width = len_px(100);
    base.background_color = rgba(0, 0, 0, 255);
    HtmlElem e = 83;
    anim_update_elem(&g_sys, e, &base, 0);
    base.width = len_px(200);
    base.background_color = rgba(255, 255, 255, 255);
    base.display = CSS_DISPLAY_BLOCK;   /* not animatable: ignored */
    CHECK(anim_update_elem(&g_sys, e, &base, 0) == 1);
    uint32_t nt;
    anim_stats(&g_sys, NULL, NULL, &nt);
    CHECK(nt == 2);
    CHECK(sample(e, &base, &out, 200, &r));
    CHECK_NEAR(r->width.px, 100 + 100 * ref_ease(0.5), 1e-3);
    CHECK(r->background_color.r == (int)(255 * ref_ease(0.5) + 0.5));
    CHECK(r->display == CSS_DISPLAY_BLOCK);
    anim_remove_elem(&g_sys, e);

    /* positive delay: value holds, wakeup = start */
    base_init(&base);
    base.transitions[0] = trans_decl(CSS_PROP_WIDTH, 0.2f, 0.1f, timing_kind(CSS_TIMING_LINEAR));
    base.transition_count = 1;
    base.width = len_px(0);
    anim_update_elem(&g_sys, e, &base, 0);
    base.width = len_px(100);
    anim_update_elem(&g_sys, e, &base, 0);
    CHECK(sample(e, &base, &out, 50, &r));
    CHECK_NEAR(r->width.px, 0, 1e-6);
    CHECK_NEAR(anim_next_wakeup(&g_sys, 50), 100, 1e-9);
    dirty_reset();
    anim_tick(&g_sys, 50);
    CHECK(g_dirty.calls == 0);
    CHECK(sample(e, &base, &out, 200, &r));
    CHECK_NEAR(r->width.px, 50, 1e-4);
    anim_remove_elem(&g_sys, e);

    /* a shorthand transition-property: border-color covers all four sides */
    base_init(&base);
    base.transitions[0] = trans_decl(CSS_PROP_BORDER_COLOR, 1.0f, 0, timing_kind(CSS_TIMING_LINEAR));
    base.transition_count = 1;
    for (int i = 0; i < 4; i++) base.border_color[i] = rgba(0, 0, 0, 255);
    anim_update_elem(&g_sys, e, &base, 0);
    for (int i = 0; i < 4; i++) base.border_color[i] = rgba(200, 0, 0, 255);
    anim_update_elem(&g_sys, e, &base, 0);
    CHECK(sample(e, &base, &out, 500, &r));
    for (int i = 0; i < 4; i++) CHECK(r->border_color[i].r == 100);
    anim_remove_elem(&g_sys, e);

    /* zero duration: nothing starts, record not kept */
    base_init(&base);
    base.transitions[0] = trans_decl(CSS_PROP_ALL, 0.0f, 0, timing_kind(CSS_TIMING_EASE));
    base.transition_count = 1;
    CHECK(anim_update_elem(&g_sys, e, &base, 0) == 0);
    uint32_t ne;
    anim_stats(&g_sys, &ne, NULL, NULL);
    CHECK(ne == 0);
}

static void test_animation_plus_transition(void)
{
    /* transitions win over animations for the same property */
    CssComputed base, out;
    const CssComputed *r;
    base_init(&base);
    base.animations[0] = anim_decl("rainbowT", 0.5f, 0, INFINITY, timing_kind(CSS_TIMING_LINEAR), CSS_ANIM_DIRECTION_NORMAL, CSS_ANIM_FILL_NONE);
    base.animation_count = 1;
    base.transitions[0] = trans_decl(CSS_PROP_ALL, 1.0f, 0, timing_kind(CSS_TIMING_LINEAR));
    base.transition_count = 1;
    HtmlElem e = 90;
    anim_update_elem(&g_sys, e, &base, 0);
    CHECK(sample(e, &base, &out, 0, &r) && r->color.r == 220);
    base.opacity = 0.0f;
    anim_update_elem(&g_sys, e, &base, 0);
    CHECK(sample(e, &base, &out, 500, &r));
    CHECK_NEAR(r->opacity, 0.5, 1e-6);
    CHECK(r->color.r == 220);   /* rainbowT wrapped to Crimson */
    anim_remove_elem(&g_sys, e);
}

static void test_many_elements(void)
{
    /* hash map growth/removal with many elements */
    CssComputed base, out;
    const CssComputed *r;
    base_init(&base);
    base.animations[0] = anim_decl("spin", 2.0f, 0, INFINITY, timing_kind(CSS_TIMING_LINEAR), CSS_ANIM_DIRECTION_NORMAL, CSS_ANIM_FILL_NONE);
    base.animation_count = 1;
    for (HtmlElem e = 1000; e < 1500; e++)
        anim_update_elem(&g_sys, e, &base, 0);
    uint32_t ne;
    anim_stats(&g_sys, &ne, NULL, NULL);
    CHECK(ne == 500);
    for (HtmlElem e = 1000; e < 1500; e += 2)
        anim_remove_elem(&g_sys, e);
    anim_stats(&g_sys, &ne, NULL, NULL);
    CHECK(ne == 250);
    CHECK(sample(1001, &base, &out, 1000, &r));
    CHECK_NEAR(r->transform.ops[0].angle_deg, 180, 1e-4);
    CHECK(!sample(1000, &base, &out, 1000, &r));
    dirty_reset();
    anim_tick(&g_sys, 10);
    CHECK(g_dirty.calls == 250);
    for (HtmlElem e = 1001; e < 1500; e += 2)
        anim_remove_elem(&g_sys, e);
    anim_stats(&g_sys, &ne, NULL, NULL);
    CHECK(ne == 0);
}

int main(int argc, char **argv)
{
    const char *css_dir = argc > 1 ? argv[1] : "ui/css";
    anim_init(&g_sys, on_dirty, NULL);

    test_timing();
    test_prop_masks();
    test_value_lerp();

    int loaded = 0;
    const char *files[] = { "main.css", "hud.css" };
    for (size_t i = 0; i < 2; i++) {
        char path[512];
        snprintf(path, sizeof path, "%s/%s", css_dir, files[i]);
        char *text = read_file(path);
        if (!text) {
            fprintf(stderr, "cannot read %s\n", path);
            return 2;
        }
        loaded += load_keyframes_text(&g_sys, text);
        free(text);
    }
    CHECK(loaded == 13);
    CHECK(g_sys.set_count == 13);
    static const struct { const char *name; uint32_t frames; } expect[] = {
        { "blinkTextCursor", 2 }, { "typewriter", 2 }, { "grow", 1 }, { "lds-ring", 2 }, { "spin", 2 },
        { "scaleBob", 2 }, { "rainbow", 6 }, { "rainbowT", 6 }, { "rainbowB", 6 }, { "rainbowBG", 6 },
        { "rgbHue", 2 }, { "poT", 5 }, { "shake", 9 },
    };
    for (size_t i = 0; i < sizeof expect / sizeof expect[0]; i++) {
        const AnimKeyframeSet *s = anim_keyframes_find(&g_sys, kn_atom_cstr(expect[i].name));
        g_checks++;
        if (!s || s->count != expect[i].frames) {
            g_fails++;
            fprintf(stderr, "FAIL keyframes %s: %s (%u frames)\n", expect[i].name, s ? "frame count" : "missing", s ? s->count : 0);
        }
    }
    CHECK(anim_keyframes_find(&g_sys, kn_atom_cstr("sprayAnm")) == NULL);

    test_grow();
    test_zoom_and_shadow_lists();
    test_scale_bob();
    test_lds_ring();
    test_rgb_hue();
    test_rainbow();
    test_shake();
    test_fill_and_negative_delay();
    test_unknown_name();
    test_transition_all();
    test_transition_transform();
    test_transition_opacity_and_reverse();
    test_transition_shorthand_all_and_delay();
    test_animation_plus_transition();
    test_many_elements();

    uint32_t ne;
    anim_stats(&g_sys, &ne, NULL, NULL);
    CHECK(ne == 0);
    anim_free(&g_sys);

    printf("test_anim: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
