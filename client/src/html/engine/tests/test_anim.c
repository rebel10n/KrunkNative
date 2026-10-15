/*
 * test_anim.c - standalone tests for anim.c (CSS animations + transitions).
 *
 * Build (throwaway, from the repository root):
 *   clang -std=c11 -g -fsanitize=address,undefined -DKN_ANIM_NO_SHEET -Iclient/src/html/engine \
 *      client/src/html/engine/anim.c client/src/html/engine/css_values.c client/src/html/engine/util.c \
 *      client/src/html/engine/tests/test_anim.c -lm -o test_anim
 *   ./test_anim [path/to/ui/css]
 *
 * The 38 @keyframes blocks of ui/css/main.css (+ hud.css, krunknative.css) and two fixture ones are read with a
 * small scanner (css_sheet.c / Lexbor are not required), turned into
 * AnimKeyframeSets, and sampled at known times.  Expected values are either
 * exact (linear/steps) or derived from an independent bisection bezier
 * solver defined here.
 *
 * The v10 sets of svelte.css (rewardsGlow, dsGlow, the ranked footer pulse,
 * borderGlow, bannerShaderSweep, shopBadgeShine, smoothBezierFly) and main.css
 * (backMov, panBackground), the .shopCard background-size transition and the
 * `transition: all` additions (visibility, z-index, font-weight), transform
 * lists (reward-card flip, matrix fallback, padding, slerp), color-mix()
 * with currentColor and registered <length>/<color> customs are checked
 * against Chrome 152's computed values at the same times, taken from our own
 * fixture tests/anim_chrome_fixture.html (its header says how to re-run it);
 * every number is also the CSS math noted next to it.
 *
 * Cascade integration (real css_sheet.c / css_cascade.c / css_values.c:
 * @property parsing, var()/calc() re-resolve per frame, ::before/::after
 * keys) is compiled with -DKN_ANIM_TEST_CASCADE instead of -DKN_ANIM_NO_SHEET:
 *   clang -std=c11 -g -fsanitize=address,undefined -DKN_ANIM_TEST_CASCADE -Iclient/src/html/engine \
 *      -Iclient/include -Ishared/include -Iclient/lib/lexbor client/src/html/engine/anim.c \
 *      client/src/html/engine/css_values.c client/src/html/engine/css_sheet.c \
 *      client/src/html/engine/css_cascade.c client/src/html/engine/dom.c client/src/html/engine/util.c \
 *      client/lib/lexbor/lexbor.c client/src/html/engine/tests/test_anim.c -lm -o test_anim_cascade
 *   ./test_anim_cascade ui/css
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "anim.h"
#include "css_values.h"
#include "util.h"
#ifdef KN_ANIM_TEST_CASCADE
#include "css_cascade.h"
#include "css_sheet.h"
#include "dom.h"
#endif

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
    c->font_weight = 400;
    for (int i = 0; i < CSS_MAX_BG_LAYERS; i++) {   /* background-size: auto */
        c->bg_layers[i].size.w.kind = CSS_LEN_AUTO;
        c->bg_layers[i].size.h.kind = CSS_LEN_AUTO;
    }
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

/* `0%`, `-120px`, `top`/`left` (0%), `center` (50%), `bottom`/`right` (100%). */
static int parse_pos_text(const char *s, size_t n, CssLength *out)
{
    s = kn_str_trim(s, &n);
    CssLength l = {0};
    l.kind = CSS_LEN_VALUE;
    if (str_ieq_n(s, n, "top") || str_ieq_n(s, n, "left")) { *out = l; return 1; }
    if (str_ieq_n(s, n, "center")) { l.pct = 50; *out = l; return 1; }
    if (str_ieq_n(s, n, "bottom") || str_ieq_n(s, n, "right")) { l.pct = 100; *out = l; return 1; }
    return parse_length_text(s, n, out);
}

/* `x y [blur [spread]] [color]` (color first or last), comma-separated. */
static int parse_shadow_list_text(const char *s, size_t n, CssShadowList *out)
{
    memset(out, 0, sizeof *out);
    const char *end = s + n;
    while (s < end && out->count < CSS_MAX_TEXT_SHADOWS) {
        const char *comma = s;
        int depth = 0;
        while (comma < end && (depth || *comma != ',')) { if (*comma == '(') depth++; if (*comma == ')') depth--; comma++; }
        CssShadow *sh = &out->items[out->count];
        memset(sh, 0, sizeof *sh);
        sh->color.kind = CSS_COLOR_CURRENT;
        float nums[4];
        int nn = 0;
        const char *p = s;
        while (p < comma) {
            p = skip_ws(p);
            if (p >= comma) break;
            const char *q = p;
            depth = 0;
            while (q < comma && (depth || !kn_is_space(*q))) { if (*q == '(') depth++; if (*q == ')') depth--; q++; }
            CssLength l;
            if ((*p == '-' || *p == '.' || (*p >= '0' && *p <= '9')) && parse_length_text(p, (size_t)(q - p), &l)) {
                if (nn < 4) nums[nn++] = l.px;
            } else if (!str_ieq_n(p, (size_t)(q - p), "currentColor") && !parse_color_text(p, (size_t)(q - p), &sh->color)) {
                return 0;
            }
            p = q;
        }
        if (nn < 2) return 0;
        sh->x = nums[0];
        sh->y = nums[1];
        sh->blur = nn > 2 ? nums[2] : 0;
        sh->spread = nn > 3 ? nums[3] : 0;
        out->count++;
        s = comma + 1;
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
    if (str_ieq_n(name, nn, "left")) {
        if (!parse_length_text(value, vn, &v.len)) return 0;
        anim_keyframe_set_value(set, frame, ANIM_LEFT, &v);
        return 1;
    }
    if (nn > 2 && name[0] == '-' && name[1] == '-') {
        anim_keyframe_set_custom(set, frame, kn_atom(name, nn), kn_atom(value, vn));
        return 1;
    }
    if (str_ieq_n(name, nn, "text-shadow")) {
        if (!parse_shadow_list_text(value, vn, &v.text_shadow)) return 0;
        anim_keyframe_set_value(set, frame, ANIM_TEXT_SHADOW, &v);
        return 1;
    }
    if (str_ieq_n(name, nn, "background-position") || str_ieq_n(name, nn, "background-position-x") ||
        str_ieq_n(name, nn, "background-position-y")) {
        CssLength x, y;
        int both = str_ieq_n(name, nn, "background-position");
        if (both) {   /* `X Y` */
            const char *sp = value;
            while (sp < value + vn && !kn_is_space(*sp)) sp++;
            if (!parse_pos_text(value, (size_t)(sp - value), &x) || !parse_pos_text(sp, (size_t)(value + vn - sp), &y)) return 0;
        } else if (!parse_pos_text(value, vn, &x)) {
            return 0;
        }
        AnimValue vy;
        memset(&vy, 0, sizeof vy);
        for (int i = 0; i < CSS_MAX_BG_LAYERS; i++) { v.bg_pos[i] = x; vy.bg_pos[i] = both ? y : x; }
        if (both || str_ieq_n(name, nn, "background-position-x")) anim_keyframe_set_value(set, frame, ANIM_BACKGROUND_POSITION_X, &v);
        if (both) anim_keyframe_set_value(set, frame, ANIM_BACKGROUND_POSITION_Y, &vy);
        else if (str_ieq_n(name, nn, "background-position-y")) anim_keyframe_set_value(set, frame, ANIM_BACKGROUND_POSITION_Y, &v);
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
            /* one frame at a time: adding a frame may move the earlier ones (realloc) */
            for (int i = 0; i < noff; i++) {
                AnimKeyframe *frame = anim_keyframes_add_frame(set, offsets[i], NULL);
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
                    if (!kf_apply_decl(set, frame, nm, nn, val, vn) && i == 0)
                        fprintf(stderr, "  (keyframes %.*s: unhandled %.*s: %.*s)\n",
                                (int)name_len, name, (int)nn, nm, (int)vn, val);
                    d = vend + 1;
                }
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
    CHECK(anim_prop_from_css_prop(CSS_PROP_Z_INDEX) == ANIM_Z_INDEX);
    CHECK(anim_prop_from_css_prop(CSS_PROP_DISPLAY) == -1);
    CHECK(anim_prop_dirty_flags(ANIM_FONT_SIZE) == ANIM_DIRTY_LAYOUT);
    CHECK(anim_prop_dirty_flags(ANIM_TRANSFORM) == ANIM_DIRTY_TRANSFORM);
    CHECK(anim_prop_dirty_flags(ANIM_VISIBILITY) == ANIM_DIRTY_REBUILD);
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
    /* mismatched primitives: matrix interpolation of the decomposed lists (Chrome; fixture #mx:
     * rotate(90deg) -> scale(0.5) at 50% = matrix(0.53033, 0.53033, -0.53033, 0.53033, 0, 0)) */
    a.transform.count = 1; a.transform.ops[0].kind = CSS_TRANSFORM_ROTATE; a.transform.ops[0].angle_deg = 90;
    CHECK(anim_value_interpolable(ANIM_TRANSFORM, &a, &b));
    anim_value_lerp(ANIM_TRANSFORM, &a, &b, 0.5f, &o);
    CHECK(o.transform.count == 1 && o.transform.ops[0].kind == CSS_TRANSFORM_MATRIX);
    CHECK_NEAR(o.transform.ops[0].m.a, 0.53033, 1e-4); CHECK_NEAR(o.transform.ops[0].m.b, 0.53033, 1e-4);
    CHECK_NEAR(o.transform.ops[0].m.c, -0.53033, 1e-4); CHECK_NEAR(o.transform.ops[0].m.d, 0.53033, 1e-4);
    /* a percentage translation in the remainder needs the box: discrete at 50% */
    a.transform.count = 2; a.transform.ops[1].kind = CSS_TRANSFORM_TRANSLATE;
    a.transform.ops[1].tx.kind = CSS_LEN_VALUE; a.transform.ops[1].tx.pct = -50; a.transform.ops[1].ty = len_px(0);
    CHECK(!anim_value_interpolable(ANIM_TRANSFORM, &a, &b));
    anim_value_lerp(ANIM_TRANSFORM, &a, &b, 0.49f, &o);
    CHECK(o.transform.count == 2 && o.transform.ops[0].kind == CSS_TRANSFORM_ROTATE);
    anim_value_lerp(ANIM_TRANSFORM, &a, &b, 0.5f, &o);
    CHECK(o.transform.count == 1 && o.transform.ops[0].kind == CSS_TRANSFORM_SCALE);
    a.transform.count = 1;
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
    CHECK_NEAR(r->font_size, 22, 1e-4);         /* `from { font-size: 44px }` * 0.5 (v10 main.css) */
    CHECK(sample(e, &base, &out, 1400, &r));
    CHECK_NEAR(r->font_size, 19, 1e-4);         /* halfway to the zoomed base 16 */
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
    CHECK_NEAR(r->font_size, 44, 1e-5);         /* v10 main.css `grow` from 44px */

    double e05 = ref_ease(0.5);
    CHECK(sample(e, &base, &out, t0 + 400, &r));
    CHECK_NEAR(r->opacity, 0.3 + 0.7 * e05, 1e-4);
    CHECK_NEAR(r->font_size, 44 - 12 * e05, 1e-4);

    /* t = 0.8s: second iteration, alternate -> reversed, progress 0 -> base */
    CHECK(sample(e, &base, &out, t0 + 800, &r));
    CHECK_NEAR(r->opacity, 1.0, 1e-6);
    CHECK_NEAR(r->font_size, 32, 1e-5);

    /* t = 1.2s: reversed iteration at 0.5 -> same as 0.4s */
    CHECK(sample(e, &base, &out, t0 + 1200, &r));
    CHECK_NEAR(r->opacity, 0.3 + 0.7 * e05, 1e-4);
    CHECK_NEAR(r->font_size, 44 - 12 * e05, 1e-4);

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
    base.border_color[CSS_LEFT] = rgba(255, 255, 255, 255);   /* currentColor (black) -> white: interpolates (Blink) */
    base.border_style[CSS_LEFT] = CSS_BORDER_STYLE_SOLID;
    CHECK(anim_update_elem(&g_sys, e, &base, t0) == 1);
    uint32_t nt;
    anim_stats(&g_sys, NULL, NULL, &nt);
    CHECK(nt == 2);

    CHECK(sample(e, &base, &out, t0, &r));
    CHECK_NEAR(r->border_width[CSS_LEFT], 0, 1e-6);
    CHECK(r->border_color[CSS_LEFT].kind == CSS_COLOR_CURRENT);   /* the start value itself */
    CHECK(sample(e, &base, &out, t0 + 75, &r));
    CHECK(r->border_color[CSS_LEFT].kind == CSS_COLOR_RGBA && r->border_color[CSS_LEFT].r == 128);
    CHECK_NEAR(r->border_width[CSS_LEFT], 10 * ref_ease_in_out(0.5), 1e-4);   /* = 5 */
    CHECK_NEAR(r->border_width[CSS_LEFT], 5, 1e-4);
    CHECK(sample(e, &base, &out, t0 + 150, &r));
    CHECK_NEAR(r->border_width[CSS_LEFT], 10, 1e-6);
    CHECK(sample(e, &base, &out, t0 + 37.5, &r));
    CHECK_NEAR(r->border_width[CSS_LEFT], 10 * ref_ease_in_out(0.25), 1e-4);

    CHECK(anim_next_wakeup(&g_sys, t0 + 10) == t0 + 10);
    dirty_reset();
    anim_tick(&g_sys, t0 + 75);
    CHECK(g_dirty.calls == 1 && g_dirty.flags == (ANIM_DIRTY_LAYOUT | ANIM_DIRTY_PAINT));
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


/* ------------------------------------------------------------------------- */
/* E1: v10 keyframes vs Chrome 152 (tests/anim_chrome_fixture.html)           */
/* ------------------------------------------------------------------------- */

static CssLength len_pct(float pct) { CssLength l = {0}; l.pct = pct; l.kind = CSS_LEN_VALUE; return l; }

static void set_bg_pos(CssComputed *c, CssLength x, CssLength y)
{
    for (int i = 0; i < CSS_MAX_BG_LAYERS; i++) { c->bg_layers[i].x = x; c->bg_layers[i].y = y; }
}

static void run_anim(CssComputed *base, const char *name, float dur, float iters, CssTiming timing, int dir, int fill)
{
    base->animations[0] = anim_decl(name, dur, 0, iters, timing, dir, fill);
    base->animation_count = 1;
}

/* main.css #homeStoreAdC { background-position-y: top; animation: backMov 18s infinite }
 * @keyframes backMov { 50% { background-position-y: bottom } }: 0% -> 100% -> 0%, ease per half. */
static void test_back_mov(void)
{
    CssComputed base, out;
    const CssComputed *r;
    base_init(&base);
    set_bg_pos(&base, len_pct(0), len_pct(0));
    run_anim(&base, "backMov", 18, INFINITY, timing_kind(CSS_TIMING_EASE), CSS_ANIM_DIRECTION_NORMAL, CSS_ANIM_FILL_NONE);
    HtmlElem e = 300;
    const double t0 = 5000;
    CHECK(anim_update_elem(&g_sys, e, &base, t0) == 1);
    static const struct { double t; double pct; } chrome[] = {   /* 100 * ease(t / 9000) on the way down */
        { 0, 0 }, { 2250, 40.8511 }, { 4500, 80.2403 }, { 9000, 100 }, { 13500, 19.7597 }, { 17000, 0.706896 },
    };
    for (size_t i = 0; i < sizeof chrome / sizeof chrome[0]; i++) {
        CHECK(sample(e, &base, &out, t0 + chrome[i].t, &r));
        CHECK_NEAR(r->bg_layers[0].y.pct, chrome[i].pct, 2e-3);
        CHECK_NEAR(r->bg_layers[1].y.pct, chrome[i].pct, 2e-3);
        CHECK_NEAR(r->bg_layers[0].x.pct, 0, 1e-6);   /* -x is not animated */
    }
    CHECK_NEAR(r->bg_layers[0].y.pct, 100 * (1 - ref_ease(8000.0 / 9000.0)), 1e-3);   /* 17000: 2nd half, 8/9 done */
    dirty_reset();
    anim_tick(&g_sys, t0 + 100);
    CHECK(g_dirty.flags == ANIM_DIRTY_PAINT);
    anim_remove_elem(&g_sys, e);
}

/* main.css .logVersionHed { animation: panBackground 10s infinite; animation-direction: alternate }
 * from { background-position: 0px 0px } to { 0px -120px } */
static void test_pan_background(void)
{
    CssComputed base, out;
    const CssComputed *r;
    base_init(&base);
    run_anim(&base, "panBackground", 10, INFINITY, timing_kind(CSS_TIMING_EASE), CSS_ANIM_DIRECTION_ALTERNATE, CSS_ANIM_FILL_NONE);
    HtmlElem e = 301;
    CHECK(anim_update_elem(&g_sys, e, &base, 0) == 1);
    static const struct { double t; double px; } chrome[] = {   /* -120 * ease(p), p reversed on odd iterations */
        { 0, 0 }, { 2500, -49.0213 }, { 5000, -96.2884 }, { 12500, -115.255 },
    };
    for (size_t i = 0; i < sizeof chrome / sizeof chrome[0]; i++) {
        CHECK(sample(e, &base, &out, chrome[i].t, &r));
        CHECK_NEAR(r->bg_layers[0].y.px, chrome[i].px, 2e-3);
        CHECK_NEAR(r->bg_layers[0].x.px, 0, 1e-6);
    }
    CHECK_NEAR(r->bg_layers[0].y.px, -120 * ref_ease(0.75), 1e-3);
    /* zoom: keyframe px scale with the element (Chrome zooms computed lengths) */
    base.zoom = 2.0f;
    CHECK(sample(e, &base, &out, 2500, &r));
    CHECK_NEAR(r->bg_layers[0].y.px, -2 * 49.0213, 4e-3);
    anim_remove_elem(&g_sys, e);
}

/* Percentage background-position sets on an element and on a pseudo-element key:
 * svelte-mqcul7-pulse (.footer.svelte-mqcul7::before, 4s ease-in-out),
 * svelte-m5s1iy-borderGlow (3s linear), svelte-1w0uz1r-bannerShaderSweep (3s linear). */
static void test_bg_pos_pct_sets(void)
{
    CssComputed base, out;
    const CssComputed *r;
    const HtmlElem e = 302, before = ANIM_KEY(302, ANIM_PSEUDO_BEFORE);
    CHECK(ANIM_KEY_ELEM(before) == e && ANIM_KEY_PSEUDO(before) == ANIM_PSEUDO_BEFORE);

    /* the element itself has no animation; its ::before runs the pulse */
    base_init(&base);
    CHECK(anim_update_elem(&g_sys, e, &base, 0) == 0);
    run_anim(&base, "svelte-mqcul7-pulse", 4, INFINITY, timing_kind(CSS_TIMING_EASE_IN_OUT), CSS_ANIM_DIRECTION_NORMAL, CSS_ANIM_FILL_NONE);
    CHECK(anim_update_elem(&g_sys, before, &base, 0) == 1);
    static const struct { double t; double pct; } pulse[] = {   /* 100 * ease-in-out(t / 2000), mirrored */
        { 0, 0 }, { 500, 12.9162 }, { 1000, 50 }, { 2000, 100 }, { 3000, 50 },
    };
    for (size_t i = 0; i < sizeof pulse / sizeof pulse[0]; i++) {
        CHECK(sample(before, &base, &out, pulse[i].t, &r));
        CHECK_NEAR(r->bg_layers[0].x.pct, pulse[i].pct, 2e-3);
        CHECK_NEAR(r->bg_layers[0].y.pct, 50, 1e-4);
    }
    CHECK_NEAR(r->bg_layers[0].x.pct, 100 * ref_ease_in_out(0.5), 1e-3);
    CHECK(!sample(e, &base, &out, 500, &r));   /* separate targets */
    dirty_reset();
    anim_tick(&g_sys, 10);
    CHECK(g_dirty.elem == before && g_dirty.flags == ANIM_DIRTY_PAINT);
    anim_remove_elem(&g_sys, before);

    base_init(&base);
    run_anim(&base, "svelte-m5s1iy-borderGlow", 3, INFINITY, timing_kind(CSS_TIMING_LINEAR), CSS_ANIM_DIRECTION_NORMAL, CSS_ANIM_FILL_NONE);
    CHECK(anim_update_elem(&g_sys, e, &base, 0) == 1);
    CHECK(sample(e, &base, &out, 750, &r));
    CHECK_NEAR(r->bg_layers[0].x.pct, 50, 1e-3);
    CHECK(sample(e, &base, &out, 2250, &r));
    CHECK_NEAR(r->bg_layers[0].x.pct, 50, 1e-3);

    run_anim(&base, "svelte-1w0uz1r-bannerShaderSweep", 3, INFINITY, timing_kind(CSS_TIMING_LINEAR), CSS_ANIM_DIRECTION_NORMAL, CSS_ANIM_FILL_NONE);
    CHECK(anim_update_elem(&g_sys, e, &base, 0) == 1);   /* name changed: restarts at 0 */
    CHECK(sample(e, &base, &out, 1000, &r));
    CHECK_NEAR(r->bg_layers[0].x.pct, 100, 1e-3);
    CHECK(sample(e, &base, &out, 2900, &r));
    CHECK_NEAR(r->bg_layers[0].x.pct, 290, 1e-3);
    anim_remove_elem(&g_sys, e);
}

static int alpha_near(const CssColor *c, double a, int tol)
{
    return c->kind == CSS_COLOR_RGBA && abs((int)c->a - (int)lround(a * 255)) <= tol;
}

/* text-shadow: svelte-da3zva-rewardsGlow (= krSaleGlow = buffGlow), svelte-159x57l-dsGlow. */
static void test_text_shadow_glows(void)
{
    CssComputed base, out;
    const CssComputed *r;
    base_init(&base);
    base.color = rgba(255, 213, 79, 255);
    base.text_shadow.count = 1;
    base.text_shadow.items[0].blur = 6;
    base.text_shadow.items[0].color = rgba(255, 213, 79, 77);
    HtmlElem e = 303;
    const char *glows[] = { "svelte-da3zva-rewardsGlow", "svelte-159x57l-krSaleGlow", "svelte-1we2ubc-buffGlow" };
    for (int g = 0; g < 3; g++) {
        run_anim(&base, glows[g], 3, INFINITY, timing_kind(CSS_TIMING_EASE_IN_OUT), CSS_ANIM_DIRECTION_NORMAL, CSS_ANIM_FILL_NONE);
        CHECK(anim_update_elem(&g_sys, e, &base, 0) == 1);
        static const struct { double t, blur, alpha; } chrome[] = {   /* 6 + 4 * ease-in-out(t / 1500), mirrored */
            { 0, 6, 0.3 }, { 375, 6.51665, 0.33 }, { 750, 8, 0.404 }, { 1500, 10, 0.5 }, { 2250, 8, 0.404 },
        };
        for (size_t i = 0; i < sizeof chrome / sizeof chrome[0]; i++) {
            CHECK(sample(e, &base, &out, chrome[i].t, &r));
            CHECK(r->text_shadow.count == 1);
            CHECK_NEAR(r->text_shadow.items[0].blur, chrome[i].blur, 2e-4);
            CHECK(alpha_near(&r->text_shadow.items[0].color, chrome[i].alpha, 1));
            CHECK(r->text_shadow.items[0].color.r == 255 && r->text_shadow.items[0].color.g == 213 &&
                  r->text_shadow.items[0].color.b == 79);
        }
        CHECK(sample(e, &base, &out, 750, &r));
        CHECK(r->text_shadow.items[0].color.a == 103);   /* (77 + 128) / 2 = 102.5 -> 103 = Chrome's 0.404 */
    }
    dirty_reset();
    anim_tick(&g_sys, 10);
    CHECK(g_dirty.flags == ANIM_DIRTY_PAINT);

    run_anim(&base, "svelte-159x57l-dsGlow", 2, INFINITY, timing_kind(CSS_TIMING_EASE_IN_OUT), CSS_ANIM_DIRECTION_ALTERNATE, CSS_ANIM_FILL_NONE);
    CHECK(anim_update_elem(&g_sys, e, &base, 0) == 1);
    static const struct { double t, blur, alpha; } ds[] = {   /* 4 + 6 * ease-in-out(p) */
        { 0, 4, 0.3 }, { 500, 4.77497, 0.34 }, { 2500, 9.22503, 0.56 },
    };
    for (size_t i = 0; i < sizeof ds / sizeof ds[0]; i++) {
        CHECK(sample(e, &base, &out, ds[i].t, &r));
        CHECK_NEAR(r->text_shadow.items[0].blur, ds[i].blur, 2e-4);
        CHECK(alpha_near(&r->text_shadow.items[0].color, ds[i].alpha, 1));
        CHECK(r->text_shadow.items[0].color.r == 158 && r->text_shadow.items[0].color.g == 235);
    }
    anim_remove_elem(&g_sys, e);
}

/* Lists of different lengths pad with transparent black; currentColor follows `color`. */
static void test_text_shadow_lists(void)
{
    AnimKeyframeSet *set = anim_keyframes_begin(&g_sys, "e1-tsl", 6);
    AnimKeyframe *f0 = anim_keyframes_add_frame(set, 0, NULL), *f1 = anim_keyframes_add_frame(set, 1, NULL);
    AnimValue v;
    memset(&v, 0, sizeof v);
    v.text_shadow.count = 1;
    v.text_shadow.items[0].blur = 4;
    v.text_shadow.items[0].color = rgba(255, 0, 0, 255);
    anim_keyframe_set_value(set, f0, ANIM_TEXT_SHADOW, &v);
    memset(&v, 0, sizeof v);
    v.text_shadow.count = 2;
    v.text_shadow.items[0].x = v.text_shadow.items[0].y = 2;
    v.text_shadow.items[0].color = rgba(0, 128, 0, 255);
    v.text_shadow.items[1].x = v.text_shadow.items[1].y = 4;
    v.text_shadow.items[1].blur = 2;
    v.text_shadow.items[1].color.kind = CSS_COLOR_CURRENT;
    anim_keyframe_set_value(set, f1, ANIM_TEXT_SHADOW, &v);

    CssComputed base, out;
    const CssComputed *r;
    base_init(&base);
    base.color = rgba(0, 0, 255, 255);
    run_anim(&base, "e1-tsl", 1, 1, timing_kind(CSS_TIMING_LINEAR), CSS_ANIM_DIRECTION_NORMAL, CSS_ANIM_FILL_NONE);
    HtmlElem e = 304;
    CHECK(anim_update_elem(&g_sys, e, &base, 0) == 1);
    /* Chrome: rgb(191, 32, 0) 0.5px 0.5px 3px, rgba(0, 0, 255, 0.25) 1px 1px 0.5px */
    CHECK(sample(e, &base, &out, 250, &r));
    CHECK(r->text_shadow.count == 2);
    const CssShadow *a = &r->text_shadow.items[0], *b = &r->text_shadow.items[1];
    CHECK(a->color.r == 191 && a->color.g == 32 && a->color.b == 0 && a->color.a == 255);
    CHECK_NEAR(a->x, 0.5, 1e-5); CHECK_NEAR(a->blur, 3, 1e-5);
    CHECK(b->color.r == 0 && b->color.g == 0 && b->color.b == 255 && b->color.a == 64);
    CHECK_NEAR(b->x, 1, 1e-5); CHECK_NEAR(b->blur, 0.5, 1e-5);
    /* Chrome: rgb(128, 64, 0) 1px 1px 2px, rgba(0, 0, 255, 0.5) 2px 2px 1px */
    CHECK(sample(e, &base, &out, 500, &r));
    CHECK(r->text_shadow.items[0].color.r == 128 && r->text_shadow.items[0].color.g == 64);
    CHECK(r->text_shadow.items[1].color.b == 255 && r->text_shadow.items[1].color.a == 128);
    anim_remove_elem(&g_sys, e);
}

/* .shop-badge.svelte-159x57l::after { left: -60%; animation: shopBadgeShine 2.6s ease-in-out infinite }
 * 0% { left: -60% } 55%, 100% { left: 130% } (Chrome reports px of a 200px containing block). */
static void test_shop_badge_shine(void)
{
    CssComputed base, out;
    const CssComputed *r;
    base_init(&base);
    base.position = CSS_POSITION_ABSOLUTE;
    base.inset[CSS_LEFT] = len_pct(-60);
    run_anim(&base, "svelte-159x57l-shopBadgeShine", 2.6f, INFINITY, timing_kind(CSS_TIMING_EASE_IN_OUT),
             CSS_ANIM_DIRECTION_NORMAL, CSS_ANIM_FILL_NONE);
    const HtmlElem after = ANIM_KEY(305, ANIM_PSEUDO_AFTER);
    CHECK(anim_update_elem(&g_sys, after, &base, 0) == 1);
    /* Chrome's getComputedStyle(left) is the laid-out px, snapped to LayoutUnit
     * (1/64 px, towards zero): -70.91847 -> -70.90625 ("-70.9062px"). */
    static const struct { double t, px; } chrome[] = {   /* 2 * (-60 + 190 * ease-in-out(t / 1430)) */
        { 0, -120 }, { 357.5, -70.9062 }, { 715, 70 }, { 1430, 260 }, { 2000, 260 },
    };
    for (size_t i = 0; i < sizeof chrome / sizeof chrome[0]; i++) {
        CHECK(sample(after, &base, &out, chrome[i].t, &r));
        CHECK_NEAR(r->inset[CSS_LEFT].pct * 2.0, chrome[i].px, 1.0 / 64.0);
        double f = chrome[i].t / 1430.0;
        CHECK_NEAR(r->inset[CSS_LEFT].pct, f >= 1 ? 130 : -60 + 190 * ref_ease_in_out(f), 1e-3);
    }
    dirty_reset();
    anim_tick(&g_sys, 10);
    CHECK(g_dirty.elem == after && g_dirty.flags == ANIM_DIRTY_LAYOUT);
    anim_remove_elem(&g_sys, after);
}

/* main.css .shopCard { background-size: 101%; transition: opacity .1s ease, background-size .1s ease,
 * background-position .1s ease } .shopCard:hover { background-size: 110% } (3-entry list). */
static void test_shop_card_zoom(void)
{
    CssComputed base, out;
    const CssComputed *r;
    base_init(&base);
    for (int i = 0; i < CSS_MAX_BG_LAYERS; i++) base.bg_layers[i].size.w = len_pct(101);
    set_bg_pos(&base, len_pct(50), len_pct(50));
    base.transitions[0] = trans_decl(CSS_PROP_OPACITY, 0.1f, 0, timing_kind(CSS_TIMING_EASE));
    base.transitions[1] = trans_decl(CSS_PROP_BACKGROUND_SIZE, 0.1f, 0, timing_kind(CSS_TIMING_EASE));
    base.transitions[2] = trans_decl(CSS_PROP_BACKGROUND_POSITION, 0.1f, 0, timing_kind(CSS_TIMING_EASE));
    base.transition_count = 3;
    HtmlElem e = 306;
    CHECK(anim_update_elem(&g_sys, e, &base, 0) == 0);
    for (int i = 0; i < CSS_MAX_BG_LAYERS; i++) base.bg_layers[i].size.w = len_pct(110);
    set_bg_pos(&base, len_pct(50), len_px(10));   /* the 3rd entry is honoured too */
    CHECK(anim_update_elem(&g_sys, e, &base, 1000) == 1);
    static const struct { double t, pct; } chrome[] = { { 0, 101 }, { 25, 104.677 }, { 50, 108.222 }, { 75, 109.644 } };
    for (size_t i = 0; i < sizeof chrome / sizeof chrome[0]; i++) {
        CHECK(sample(e, &base, &out, 1000 + chrome[i].t, &r));
        CHECK_NEAR(r->bg_layers[0].size.w.pct, chrome[i].pct, 2e-3);   /* 101 + 9 * ease(t / 100) */
        CHECK(r->bg_layers[0].size.h.kind == CSS_LEN_AUTO);
    }
    CHECK(sample(e, &base, &out, 1050, &r));
    CHECK_NEAR(r->bg_layers[0].y.pct, 50 * (1 - ref_ease(0.5)), 1e-3);   /* 50% -> 10px: calc() mix */
    CHECK_NEAR(r->bg_layers[0].y.px, 10 * ref_ease(0.5), 1e-3);
    /* cover <-> length: not interpolable, no transition (Blink) */
    anim_tick(&g_sys, 1200);   /* drops the finished transitions */
    base.bg_layers[0].size.kind = CSS_BG_SIZE_COVER;
    CHECK(anim_update_elem(&g_sys, e, &base, 2000) == 0);
    anim_remove_elem(&g_sys, e);

    CssComputed a, b;
    AnimValue va, vb, vo;
    base_init(&a);
    base_init(&b);
    b.bg_layers[0].size.w = len_px(40);   /* auto -> 40px: incompatible, flips at 50% */
    anim_value_get(&a, ANIM_BACKGROUND_SIZE, &va);
    anim_value_get(&b, ANIM_BACKGROUND_SIZE, &vb);
    CHECK(!anim_value_interpolable(ANIM_BACKGROUND_SIZE, &va, &vb));
    anim_value_lerp(ANIM_BACKGROUND_SIZE, &va, &vb, 0.49f, &vo);
    CHECK(vo.bg_size[0].w.kind == CSS_LEN_AUTO);
    anim_value_lerp(ANIM_BACKGROUND_SIZE, &va, &vb, 0.5f, &vo);
    CHECK(vo.bg_size[0].w.kind == CSS_LEN_VALUE && vo.bg_size[0].w.px == 40);
}

/* `transition: all 1s linear` covering visibility, z-index and font-weight (Chrome values). */
static void test_transition_all_discrete_ints(void)
{
    CssComputed base, out;
    const CssComputed *r;
    base_init(&base);
    base.z_index = 1;
    base.z_index_auto = 0;
    base.transitions[0] = trans_decl(CSS_PROP_ALL, 1, 0, timing_kind(CSS_TIMING_LINEAR));
    base.transition_count = 1;
    HtmlElem e = 307;
    CHECK(anim_update_elem(&g_sys, e, &base, 0) == 0);
    base.visibility = CSS_VISIBILITY_HIDDEN;
    base.z_index = 10;
    base.font_weight = 700;
    CHECK(anim_update_elem(&g_sys, e, &base, 1000) == 1);
    static const struct { double t; int z; } zs[] = { { 0, 1 }, { 250, 3 }, { 500, 6 }, { 750, 8 } };   /* round(1 + 9p) */
    for (size_t i = 0; i < sizeof zs / sizeof zs[0]; i++) {
        CHECK(sample(e, &base, &out, 1000 + zs[i].t, &r));
        CHECK(r->z_index == zs[i].z && !r->z_index_auto);
        CHECK(r->visibility == CSS_VISIBILITY_VISIBLE);   /* visible until the very end */
    }
    CHECK(sample(e, &base, &out, 1250, &r) && r->font_weight == 475);
    CHECK(sample(e, &base, &out, 1500, &r) && r->font_weight == 550);
    CHECK(sample(e, &base, &out, 1999, &r) && r->visibility == CSS_VISIBILITY_VISIBLE);
    dirty_reset();
    anim_tick(&g_sys, 1500);
    CHECK(g_dirty.flags & ANIM_DIRTY_REBUILD);
    anim_tick(&g_sys, 2000);
    CHECK(!sample(e, &base, &out, 2000, &r));   /* finished: base (hidden) */
    /* hidden -> visible: visible at once; auto z-index: discrete, no transition */
    base.visibility = CSS_VISIBILITY_VISIBLE;
    base.z_index_auto = 1;
    CHECK(anim_update_elem(&g_sys, e, &base, 3000) == 1);
    CHECK(sample(e, &base, &out, 3001, &r) && r->visibility == CSS_VISIBILITY_VISIBLE);
    CHECK(r->z_index_auto == 1);
    anim_remove_elem(&g_sys, e);
    /* neither side visible: discrete (not transitioned) */
    AnimValue a, b;
    memset(&a, 0, sizeof a);
    memset(&b, 0, sizeof b);
    a.kw = CSS_VISIBILITY_HIDDEN;
    b.kw = CSS_VISIBILITY_COLLAPSE;
    CHECK(!anim_value_interpolable(ANIM_VISIBILITY, &a, &b));
}

static double custom_number(const CssComputed *c, const char *name)
{
    const char *atom = kn_atom_cstr(name);
    for (int i = 0; i < c->custom_count; i++)
        if (c->custom[i].name == atom) return strtod(c->custom[i].value, NULL);
    return NAN;
}

/* svelte.css @property --progress { syntax: '<number>'; inherits: false; initial-value: 0 }
 * .flying-currency.animated { animation: svelte-cow1ce-smoothBezierFly var(--duration)
 * cubic-bezier(0.25, 0.46, 0.45, 0.94) forwards } with left = the quadratic Bezier of --progress.
 * The fixture uses start 100px, mid 300px, end 900px and a 1000ms duration. */
static void test_registered_progress(void)
{
    /* the css_values.c registry (inherits / initial-value semantics: the cascade test) */
    const int id = css_property_register("--progress", 10, "'<number>'", 10, "false", 5, "0", 1);
    CHECK(id != CSS_PROP_INVALID);
    const CssRegisteredProperty *reg = css_property_registration(id);
    CHECK(reg && reg->syntax == CSS_SYNTAX_NUMBER && !reg->inherits && reg->initial == kn_atom_cstr("0"));

    const AnimKeyframeSet *set = anim_keyframes_find(&g_sys, kn_atom_cstr("svelte-cow1ce-smoothBezierFly"));
    CHECK(set && set->custom_name_count == 1 && set->custom_names[0] == kn_atom_cstr("--progress"));
    CHECK(set && (set->props & ANIM_PROP_BIT(ANIM_OPACITY)) && (set->props & ANIM_PROP_BIT(ANIM_TRANSFORM)));

    CssComputed base, cas, out;
    const CssComputed *r;
    base_init(&base);
    base.custom[0].name = kn_atom_cstr("--progress");   /* .flying-currency { --progress: 0.7 } */
    base.custom[0].value = kn_atom_cstr("0.7");
    base.custom_count = 1;
    cas = base;
    anim_custom_fixup(&g_sys, 308, &cas, 0);           /* no animation: untouched */
    CHECK_NEAR(custom_number(&cas, "--progress"), 0.7, 0);

    run_anim(&base, "svelte-cow1ce-smoothBezierFly", 1, 1, timing_bezier(0.25f, 0.46f, 0.45f, 0.94f),
             CSS_ANIM_DIRECTION_NORMAL, CSS_ANIM_FILL_FORWARDS);
    HtmlElem e = 308;
    dirty_reset();
    CHECK(anim_update_elem(&g_sys, e, &base, 0) == 1);
    CHECK(g_dirty.calls == 1 && g_dirty.flags == ANIM_DIRTY_CUSTOM);   /* re-cascade with the animated value */
    static const struct { double t, left; } chrome[] = {
        { 0, 100 }, { 200, 100 }, { 350, 363.57 }, { 500, 646.506 }, { 650, 824.902 }, { 800, 900 }, { 1000, 900 }, { 5000, 900 },
    };
    for (size_t i = 0; i < sizeof chrome / sizeof chrome[0]; i++) {
        cas = base;
        anim_custom_fixup(&g_sys, e, &cas, chrome[i].t);
        double p = custom_number(&cas, "--progress");
        double left = 100 * (1 - p) * (1 - p) + 300 * 2 * (1 - p) * p + 900 * p * p;
        CHECK_NEAR(left, chrome[i].left, 0.1);   /* values are quantized to 1e-4 */
    }
    cas = base;
    anim_custom_fixup(&g_sys, e, &cas, 350);
    CHECK_NEAR(custom_number(&cas, "--progress"), ref_bezier(0.25, 0.46, 0.45, 0.94, 0.25), 1e-4);
    CHECK(sample(e, &base, &out, 100, &r));
    CHECK_NEAR(r->opacity, 0.771324, 1e-4);   /* Chrome; = bezier(0.5) between 0% and 20% */
    CHECK(sample(e, &base, &out, 900, &r));
    CHECK_NEAR(r->opacity, 0.537206, 1e-4);
    dirty_reset();
    anim_tick(&g_sys, 400);
    CHECK(g_dirty.flags & ANIM_DIRTY_CUSTOM);
    /* the animation stops (class removed): one more re-cascade without it */
    base.animation_count = 0;
    base.animations[0].name = NULL;
    dirty_reset();
    CHECK(anim_update_elem(&g_sys, e, &base, 1200) == 0);
    CHECK(g_dirty.calls == 1 && g_dirty.flags == ANIM_DIRTY_CUSTOM);
    cas = base;
    anim_custom_fixup(&g_sys, e, &cas, 1200);
    CHECK_NEAR(custom_number(&cas, "--progress"), 0.7, 0);   /* untouched */
}

/* Unregistered custom properties animate discretely, flipping at 50% of the eased interval. */
static void test_unregistered_custom(void)
{
    AnimKeyframeSet *set = anim_keyframes_begin(&g_sys, "e1-flag", 7);
    AnimKeyframe *f0 = anim_keyframes_add_frame(set, 0, NULL), *f1 = anim_keyframes_add_frame(set, 1, NULL);
    anim_keyframe_set_custom(set, f0, kn_atom_cstr("--e1-flag"), kn_atom_cstr("0"));
    anim_keyframe_set_custom(set, f1, kn_atom_cstr("--e1-flag"), kn_atom_cstr("10"));
    CssComputed base, cas;
    base_init(&base);
    run_anim(&base, "e1-flag", 1, 1, timing_kind(CSS_TIMING_EASE_IN), CSS_ANIM_DIRECTION_NORMAL, CSS_ANIM_FILL_NONE);
    HtmlElem e = 309;
    CHECK(anim_update_elem(&g_sys, e, &base, 0) == 1);
    cas = base;
    anim_custom_fixup(&g_sys, e, &cas, 600);   /* ease-in(0.6) = 0.4318 < 0.5 */
    CHECK_NEAR(custom_number(&cas, "--e1-flag"), 0, 0);
    cas = base;
    anim_custom_fixup(&g_sys, e, &cas, 700);   /* ease-in(0.7) = 0.5672 */
    CHECK_NEAR(custom_number(&cas, "--e1-flag"), 10, 0);
    anim_remove_elem(&g_sys, e);
}


/* ------------------------------------------------------------------------- */
/* Chrome 152 ground truth for transform lists, color-mix(), typed customs     */
/* (anim_chrome_fixture.html #flip #mx #pad #slerp #tail #cmix #rlen #rcol)    */
/* ------------------------------------------------------------------------- */

static CssTransform parse_xf(const char *text)
{
    CssToken t[64];
    CssTransform x;
    memset(&x, 0, sizeof x);
    int n = css_tokens_strip_ws(t, css_tokenize(text, strlen(text), t, 64));
    CHECK(css_parse_transform(t, n, &x));
    return x;
}

static CssColor parse_col(const char *text)
{
    CssToken t[64];
    CssColor c;
    memset(&c, 0, sizeof c);
    int n = css_tokens_strip_ws(t, css_tokenize(text, strlen(text), t, 64));
    CHECK(css_parse_color(t, n, &c));
    return c;
}

/* The interpolated list's matrix against Chrome's matrix() (6) / matrix3d() (16) values. */
static void check_xf(const char *from, const char *to, float t, const double *want, int nwant)
{
    AnimValue a, b, o;
    float m[16];
    memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
    a.transform = parse_xf(from);
    b.transform = parse_xf(to);
    CHECK(anim_value_interpolable(ANIM_TRANSFORM, &a, &b));
    anim_value_lerp(ANIM_TRANSFORM, &a, &b, t, &o);
    css_transform_to_mat4(&o.transform, 100, 50, m);
    static const int idx6[6] = { 0, 1, 4, 5, 12, 13 };
    for (int i = 0; i < nwant; i++) {
        const int k = nwant == 6 ? idx6[i] : i;
        g_checks++;
        if (fabs(m[k] - want[i]) > 1e-4) {
            g_fails++;
            fprintf(stderr, "FAIL %s -> %s @%.4f: m[%d] = %.6f, Chrome %.6f\n", from, to, t, k, m[k], want[i]);
        }
    }
}

static void test_chrome_transform_lists(void)
{
    /* .reward-card { transition: transform 0.4s } -> .flipped { rotateY(180deg) } (ease) */
    static const struct { double ms; double w[16]; } flip[] = {
        { 100, { 0.283481, 0, -0.958978, 0, 0, 1, 0, 0, 0.958978, 0, 0.283481, 0, 0, 0, 0, 1 } },
        { 200, { -0.813432, 0, -0.58166, 0, 0, 1, 0, 0, 0.58166, 0, -0.813432, 0, 0, 0, 0, 1 } },
        { 300, { -0.992294, 0, -0.123903, 0, 0, 1, 0, 0, 0.123903, 0, -0.992294, 0, 0, 0, 0, 1 } },
    };
    for (size_t i = 0; i < 3; i++)
        check_xf("none", "rotateY(180deg)", (float)ref_ease(flip[i].ms / 400.0), flip[i].w, 16);
    /* mismatched primitives: decomposed matrix interpolation */
    static const double mx250[6] = { 0.334848, 0.808395, -0.808395, 0.334848, 0, 0 };
    static const double mx500[6] = { 0.53033, 0.53033, -0.53033, 0.53033, 0, 0 };
    static const double mx750[6] = { 0.577425, 0.239177, -0.239177, 0.577425, 0, 0 };
    check_xf("rotate(90deg)", "scale(0.5)", 0.25f, mx250, 6);
    check_xf("rotate(90deg)", "scale(0.5)", 0.5f, mx500, 6);
    check_xf("rotate(90deg)", "scale(0.5)", 0.75f, mx750, 6);
    /* the shorter list padded with identity functions */
    static const double pad[6] = { 0.707107, 0.707107, -0.707107, 0.707107, 15, 0 };
    check_xf("translateX(10px)", "translateX(20px) rotate(90deg)", 0.5f, pad, 6);
    /* rotate() pairs with rotateY(): different axes slerp */
    static const double sl250[16] = { 0.886024, 0.386533, -0.256033, 0, -0.386533, 0.920782, 0.052473, 0,
                                      0.256033, 0.052473, 0.965243, 0, 0, 0, 0, 1 };
    static const double sl500[16] = { 0.827398, 0.258176, -0.498757, 0, -0.258176, 0.963525, 0.0704647, 0,
                                      0.498757, 0.0704647, 0.863873, 0, 0, 0, 0, 1 };
    check_xf("rotate(30deg)", "rotateY(60deg)", 0.25f, sl250, 16);
    check_xf("rotate(30deg)", "rotateY(60deg)", 0.5f, sl500, 16);
    /* common prefix per function, the remainders as one matrix */
    static const double tail[6] = { 1.5, 0, 0.433013, 1.5, 20, 0 };
    check_xf("translateX(10px) rotate(0deg) scale(2)", "translateX(30px) skewX(30deg)", 0.5f, tail, 6);

    /* zoom scales translateZ and perspective() like the cascade does */
    CssComputed base, out;
    const CssComputed *r;
    AnimKeyframeSet *set = anim_keyframes_begin(&g_sys, "e1-z3d", 6);
    AnimKeyframe *f1 = anim_keyframes_add_frame(set, 1, NULL);
    AnimValue v;
    memset(&v, 0, sizeof v);
    v.transform = parse_xf("perspective(100px) translateZ(10px)");
    anim_keyframe_set_value(set, f1, ANIM_TRANSFORM, &v);
    base_init(&base);
    base.zoom = 2.0f;
    run_anim(&base, "e1-z3d", 1, 1, timing_kind(CSS_TIMING_LINEAR), CSS_ANIM_DIRECTION_NORMAL, CSS_ANIM_FILL_FORWARDS);
    CHECK(anim_update_elem(&g_sys, 310, &base, 0) == 1);
    CHECK(sample(310, &base, &out, 2000, &r));
    CHECK(r->transform.count == 2 && r->transform.ops[0].kind == CSS_TRANSFORM_PERSPECTIVE);
    if (r->transform.count == 2) {
        CHECK_NEAR(r->transform.ops[0].persp, 200, 1e-4);
        CHECK_NEAR(r->transform.ops[1].tz, 20, 1e-4);
    }
    anim_remove_elem(&g_sys, 310);
}

static void test_chrome_colors_and_typed_customs(void)
{
    /* color-mix(in srgb, currentColor 50%, red) with color: blue -> rgb(0, 255, 0) interpolates in
     * Oklab (Chrome: oklab(0.419725 0.164257 -0.101172) -> oklab(0.64308 -0.0347916 0.0391649) at 50%
     * = sRGB 129.6 148.14 115.06) */
    CssComputed base, out;
    const CssComputed *r;
    AnimKeyframeSet *set = anim_keyframes_begin(&g_sys, "e1-cmix", 7);
    AnimKeyframe *f0 = anim_keyframes_add_frame(set, 0, NULL), *f1 = anim_keyframes_add_frame(set, 1, NULL);
    AnimValue v;
    memset(&v, 0, sizeof v);
    v.color = parse_col("color-mix(in srgb, currentColor 50%, rgb(255, 0, 0))");
    CHECK(v.color.kind == CSS_COLOR_CURRENT_MIX);
    anim_keyframe_set_value(set, f0, ANIM_BORDER_TOP_COLOR, &v);
    v.color = css_color_rgba(0, 255, 0, 255);
    anim_keyframe_set_value(set, f1, ANIM_BORDER_TOP_COLOR, &v);
    base_init(&base);
    base.color = css_color_rgba(0, 0, 255, 255);
    run_anim(&base, "e1-cmix", 1, 1, timing_kind(CSS_TIMING_LINEAR), CSS_ANIM_DIRECTION_NORMAL, CSS_ANIM_FILL_NONE);
    CHECK(anim_update_elem(&g_sys, 311, &base, 0) == 1);
    CHECK(sample(311, &base, &out, 0, &r));
    CHECK(r->border_color[CSS_TOP].kind == CSS_COLOR_RGBA);
    CHECK_NEAR(r->border_color[CSS_TOP].r, 127.51, 1); CHECK_NEAR(r->border_color[CSS_TOP].g, 0, 1);
    CHECK_NEAR(r->border_color[CSS_TOP].b, 127.49, 1);
    CHECK(sample(311, &base, &out, 500, &r));
    CHECK_NEAR(r->border_color[CSS_TOP].r, 129.6, 1); CHECK_NEAR(r->border_color[CSS_TOP].g, 148.14, 1);
    CHECK_NEAR(r->border_color[CSS_TOP].b, 115.06, 1);
    anim_remove_elem(&g_sys, 311);

    /* registered <length> / <color> custom properties: @keyframes { to { --x: ... } } */
    CHECK(css_property_register("--e1len", 7, "'<length>'", 10, "false", 5, "0px", 3) != CSS_PROP_INVALID);
    CHECK(css_property_register("--e1col", 7, "'<color>'", 9, "false", 5, "red", 3) != CSS_PROP_INVALID);
    set = anim_keyframes_begin(&g_sys, "e1-typed", 8);
    f1 = anim_keyframes_add_frame(set, 1, NULL);
    anim_keyframe_set_custom(set, f1, kn_atom_cstr("--e1len"), kn_atom_cstr("200px"));
    anim_keyframe_set_custom(set, f1, kn_atom_cstr("--e1col"), kn_atom_cstr("rgba(0, 0, 255, 0.5)"));
    base_init(&base);
    base.custom[0].name = kn_atom_cstr("--e1len"); base.custom[0].value = kn_atom_cstr("0px");   /* initial values */
    base.custom[1].name = kn_atom_cstr("--e1col"); base.custom[1].value = kn_atom_cstr("red");
    base.custom_count = 2;
    run_anim(&base, "e1-typed", 1, 1, timing_kind(CSS_TIMING_LINEAR), CSS_ANIM_DIRECTION_NORMAL, CSS_ANIM_FILL_NONE);
    CHECK(anim_update_elem(&g_sys, 312, &base, 0) == 1);
    static const struct { double t, w; uint8_t r, b; float a; } want[] = {
        { 250, 50, 218, 37, 0.875f }, { 500, 100, 170, 85, 0.753f },   /* Chrome: rgba(218, 0, 37, 0.875) */
    };
    for (size_t i = 0; i < 2; i++) {
        CssComputed cas = base;
        anim_custom_fixup(&g_sys, 312, &cas, want[i].t);
        const char *len = NULL, *col = NULL;
        for (int k = 0; k < cas.custom_count; k++) {
            if (cas.custom[k].name == kn_atom_cstr("--e1len")) len = cas.custom[k].value;
            if (cas.custom[k].name == kn_atom_cstr("--e1col")) col = cas.custom[k].value;
        }
        CHECK(len && col);
        if (!len || !col) continue;
        CHECK_NEAR(atof(len), want[i].w, 1e-3);
        CHECK(strstr(len, "px") != NULL);
        CssColor c = parse_col(col);
        CHECK_NEAR(c.r, want[i].r, 1); CHECK_NEAR(c.g, 0, 0); CHECK_NEAR(c.b, want[i].b, 1);
        CHECK_NEAR(c.a / 255.0, want[i].a, 0.004);
    }
    anim_remove_elem(&g_sys, 312);
}

/* engine.c's display: none glue, replayed on the animation system: an element, its child and
 * its ::before run animations + transitions; hiding forgets every key of the subtree
 * (anim_forget), showing re-syncs them (anim_sync_tree), while neighbouring records churn on
 * the same hash chains.  Every live key must still resolve to its own record (the map once
 * kept a stale duplicate after elem_destroy: use after free in anim_custom_fixup, v10_hud_e2e
 * spectate -> menu), removed keys to none; then the system is freed (run under ASan). */
static void test_display_toggle_churn(void)
{
    AnimSystem sys;
    anim_init(&sys, NULL, NULL);
    AnimKeyframeSet *set = anim_keyframes_begin(&sys, "e1-churn", 8);
    AnimKeyframe *f0 = anim_keyframes_add_frame(set, 0, NULL);
    AnimValue v;
    memset(&v, 0, sizeof v);
    v.f = 0.25f;
    anim_keyframe_set_value(set, f0, ANIM_OPACITY, &v);
    anim_keyframe_set_custom(set, f0, kn_atom_cstr("--e1-churn"), kn_atom_cstr("0"));

    CssComputed base, shown, out;
    base_init(&base);
    base.opacity = 1.0f;
    base.animations[0] = anim_decl("e1-churn", 1, 0, INFINITY, timing_kind(CSS_TIMING_LINEAR), CSS_ANIM_DIRECTION_NORMAL, CSS_ANIM_FILL_NONE);
    base.animation_count = 1;
    base.transitions[0].prop = CSS_PROP_ALL;
    base.transitions[0].duration_s = 1.0f;
    base.transitions[0].timing = timing_kind(CSS_TIMING_LINEAR);
    base.transition_count = 1;

    const HtmlElem P = 40, C = 41;
    const HtmlElem keys[4] = { ANIM_KEY(P, ANIM_PSEUDO_NONE), ANIM_KEY(P, ANIM_PSEUDO_BEFORE),
                               ANIM_KEY(C, ANIM_PSEUDO_NONE), ANIM_KEY(C, ANIM_PSEUDO_AFTER) };
    double now = 0;
    int bad = 0;
    uint8_t alive[160] = { 0 };
    for (int cycle = 0; cycle < 200; cycle++) {
        /* neighbours: create and drop records around ours */
        for (HtmlElem n = 100 + (HtmlElem)(cycle % 7); n < 160; n += 3) {
            if (cycle & 1) anim_update_elem(&sys, n, &base, now);
            else anim_remove_elem(&sys, n);
            alive[n] = (uint8_t)(cycle & 1);
        }
        /* shown: sync every key, change a value to start transitions, tick, sample */
        for (int k = 0; k < 4; k++) anim_update_elem(&sys, keys[k], &base, now);
        shown = base;
        shown.opacity = (cycle & 2) ? 0.5f : 0.75f;
        now += 16;
        for (int k = 0; k < 4; k++) anim_update_elem(&sys, keys[k], &shown, now);
        anim_tick(&sys, now += 16);
        for (int k = 0; k < 4; k++) {
            CssComputed cas = shown;
            anim_custom_fixup(&sys, keys[k], &cas, now);
            const CssComputed *r = anim_apply(&sys, keys[k], &shown, &out, now);
            if (r == &shown) bad++;   /* a live key lost its record */
        }
        /* hidden: the subtree's keys are forgotten (twice: element, then descendants) */
        for (int k = 0; k < 4; k++) anim_remove_elem(&sys, keys[k]);
        for (int k = 2; k < 4; k++) anim_remove_elem(&sys, keys[k]);
        for (int k = 0; k < 4; k++) {
            CssComputed cas = shown;
            anim_custom_fixup(&sys, keys[k], &cas, now);   /* no record: untouched */
            if (anim_apply(&sys, keys[k], &shown, &out, now) != &shown) bad++;
        }
        now += 100;
    }
    CHECK(bad == 0);
    uint32_t elems = 0, want = 0;
    for (int n = 0; n < 160; n++) want += alive[n];
    anim_stats(&sys, &elems, NULL, NULL);
    CHECK(elems == want);   /* only the neighbours still alive; our keys all forgotten */
    for (HtmlElem n = 100; n < 160; n++) {   /* each neighbour resolves to its own record or none */
        const CssComputed *r = anim_apply(&sys, n, &base, &out, now);
        if ((r != &base) != alive[n]) bad++;
    }
    CHECK(bad == 0);
    CHECK(sys.map_cap <= 256);   /* tombstones are rehashed away, the map does not grow with churn */

    /* random churn over element and pseudo keys: membership checked after every operation */
    static uint8_t live[3][96];
    memset(live, 0, sizeof live);
    for (HtmlElem n = 100; n < 160; n++) anim_remove_elem(&sys, n);
    uint32_t seed = 12345u;
    int mismatches = 0;
    for (int op = 0; op < 20000; op++) {
        seed = seed * 1664525u + 1013904223u;
        const int ps = (int)((seed >> 8) % 3), e = 1 + (int)((seed >> 12) % 95);
        const HtmlElem key = ANIM_KEY((HtmlElem)e, ps);
        if ((seed >> 24) & 1) {
            anim_update_elem(&sys, key, &base, now);
            live[ps][e] = 1;
        } else {
            anim_remove_elem(&sys, key);
            live[ps][e] = 0;
        }
        for (int c = 0; c < 4; c++) {
            seed = seed * 1664525u + 1013904223u;
            const int cps = (int)((seed >> 8) % 3), ce = 1 + (int)((seed >> 12) % 95);
            const HtmlElem ck = ANIM_KEY((HtmlElem)ce, cps);
            CssComputed cas = base;
            anim_custom_fixup(&sys, ck, &cas, now + 100);
            const int has = anim_apply(&sys, ck, &base, &out, now + 100) != &base;
            if (has != live[cps][ce]) mismatches++;
        }
    }
    CHECK(mismatches == 0);
    anim_free(&sys);
}

#ifdef KN_ANIM_TEST_CASCADE
/* ------------------------------------------------------------------------- */
/* Cascade integration: the real sheet loader, cascade hook and var()/calc()  */
/* ------------------------------------------------------------------------- */

static AnimSystem g_csys;
static double g_cnow;
static Dom *g_cdom;
static int g_custom_marks;

static void cascade_dirty(void *user, HtmlElem key, unsigned flags)
{
    (void)user;
    if (flags & ANIM_DIRTY_CUSTOM) {   /* what engine.c's anim_dirty_cb does */
        dom_mark_dirty(g_cdom, ANIM_KEY_ELEM(key), DOM_DIRTY_STYLE);
        g_custom_marks++;
    }
}

static void cascade_hook(void *user, HtmlElem e, int pe, CssComputed *out)
{
    (void)user;
    int ps = pe == CSS_PE_NONE ? ANIM_PSEUDO_NONE : pe == CSS_PE_BEFORE ? ANIM_PSEUDO_BEFORE
           : pe == CSS_PE_AFTER ? ANIM_PSEUDO_AFTER : -1;
    if (ps >= 0) anim_custom_fixup(&g_csys, ANIM_KEY(e, ps), out, g_cnow);
}

static CssSheet *load_css_file(const char *dir, const char *name)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    char *text = read_file(path);
    if (!text) return NULL;
    CssSheet *sh = css_sheet_load(text, strlen(text), name);
    free(text);
    return sh;
}

/* restyle -> anim sync (the engine's post_restyle), repeated while custom
 * animations asked for a re-cascade (engine_flush_style's bounded loop) */
static void cascade_frame(CssSheet **sheets, int n, HtmlElem e, double now)
{
    g_cnow = now;
    anim_tick(&g_csys, now);   /* html_ui_backend_update: marks custom animations for a re-cascade */
    for (int pass = 0; pass < 3; pass++) {
        g_custom_marks = 0;
        cascade_restyle(g_cdom, NULL, sheets, n);
        const CssComputed *c = cascade_elem_computed(g_cdom, e);
        anim_update_elem(&g_csys, e, c, now);
        if (!g_custom_marks) break;
    }
}

static void test_cascade_integration(const char *css_dir)
{
    anim_init(&g_csys, cascade_dirty, NULL);
    CssSheet *svelte = load_css_file(css_dir, "svelte.css"), *main_css = load_css_file(css_dir, "main.css");
    CHECK(svelte && main_css);
    if (!svelte || !main_css) return;

    /* @property: the three --progress rules of svelte.css parse and register */
    uint32_t np = 0;
    const CssPropertyRule *pr = css_sheet_properties(svelte, &np);
    CHECK(np == 3);
    CHECK(np && pr[0].name == kn_atom_cstr("--progress") && pr[0].syntax == kn_atom_cstr("<number>") &&
          pr[0].initial == kn_atom_cstr("0") && pr[0].inherits == 0);
    CHECK(anim_properties_load_sheet(svelte) == 3);
    const CssRegisteredProperty *reg = css_property_registration(css_prop_lookup("--progress", 10));
    CHECK(reg && reg->syntax == CSS_SYNTAX_NUMBER && !reg->inherits);

    /* the real loader: new properties and custom entries reach the sets */
    CHECK(anim_keyframes_load_sheet(&g_csys, main_css, css_computed_initial()) > 30);
    CHECK(anim_keyframes_load_sheet(&g_csys, svelte, css_computed_initial()) > 40);
    static const struct { const char *name; uint64_t props; } sets[] = {
        { "backMov", ANIM_PROP_BIT(ANIM_BACKGROUND_POSITION_Y) },
        { "panBackground", ANIM_PROP_BIT(ANIM_BACKGROUND_POSITION_X) | ANIM_PROP_BIT(ANIM_BACKGROUND_POSITION_Y) },
        { "svelte-mqcul7-pulse", ANIM_PROP_BIT(ANIM_BACKGROUND_POSITION_X) | ANIM_PROP_BIT(ANIM_BACKGROUND_POSITION_Y) },
        { "svelte-m5s1iy-borderGlow", ANIM_PROP_BIT(ANIM_BACKGROUND_POSITION_X) | ANIM_PROP_BIT(ANIM_BACKGROUND_POSITION_Y) },
        { "svelte-1w0uz1r-bannerShaderSweep", ANIM_PROP_BIT(ANIM_BACKGROUND_POSITION_X) | ANIM_PROP_BIT(ANIM_BACKGROUND_POSITION_Y) },
        { "svelte-da3zva-rewardsGlow", ANIM_PROP_BIT(ANIM_TEXT_SHADOW) },
        { "svelte-159x57l-dsGlow", ANIM_PROP_BIT(ANIM_TEXT_SHADOW) },
        { "svelte-159x57l-krSaleGlow", ANIM_PROP_BIT(ANIM_TEXT_SHADOW) },
        { "svelte-1we2ubc-buffGlow", ANIM_PROP_BIT(ANIM_TEXT_SHADOW) },
        { "svelte-159x57l-shopBadgeShine", ANIM_PROP_BIT(ANIM_LEFT) },
    };
    for (size_t i = 0; i < sizeof sets / sizeof sets[0]; i++) {
        const AnimKeyframeSet *set = anim_keyframes_find(&g_csys, kn_atom_cstr(sets[i].name));
        g_checks++;
        if (!set || set->props != sets[i].props) {
            g_fails++;
            fprintf(stderr, "FAIL keyframes %s: props %llx\n", sets[i].name, set ? (unsigned long long)set->props : 0ull);
        }
    }
    const char *flies[] = { "svelte-cow1ce-smoothBezierFly", "svelte-5yw9uo-smoothBezierFly", "svelte-ory46c-smoothBezierFly" };
    for (int i = 0; i < 3; i++) {
        const AnimKeyframeSet *set = anim_keyframes_find(&g_csys, kn_atom_cstr(flies[i]));
        CHECK(set && set->custom_name_count == 1 && set->custom_names[0] == kn_atom_cstr("--progress"));
    }
    const AnimKeyframeSet *rg = anim_keyframes_find(&g_csys, kn_atom_cstr("svelte-da3zva-rewardsGlow"));
    const AnimValue *v50 = NULL;
    for (uint32_t f = 0; rg && f < rg->count; f++)
        if (rg->frames[f].offset == 0.5f) v50 = &rg->frames[f].entries[0].value;
    CHECK(v50 && v50->text_shadow.count == 1 && v50->text_shadow.items[0].blur == 10 && v50->text_shadow.items[0].color.a == 128);

    /* a flying-currency element: left re-resolves from the animated --progress every frame */
    static const char html[] =
        "<!doctype html><html><body><div id=fly class=animated "
        "style=\"--start-x: 100px; --mid-x: 300px; --end-x: 900px; --duration: 1000ms\">"
        "<span id=kid></span></div><div id=badge></div><div id=flyT></div><div id=bad></div></body></html>";
    static const char css[] =
        ".animated { position: fixed; --progress: 0; left: calc(var(--start-x) * (1 - var(--progress)) * (1 - var(--progress))"
        " + var(--mid-x) * 2 * (1 - var(--progress)) * var(--progress) + var(--end-x) * var(--progress) * var(--progress));"
        " animation: svelte-cow1ce-smoothBezierFly var(--duration) cubic-bezier(0.25, 0.46, 0.45, 0.94) forwards; }\n"
        "#kid { width: calc(var(--progress, 7) * 1px); }\n"
        "#badge { position: absolute; width: 200px; }\n"
        "#badge::after { content: ''; position: absolute; left: -60%; animation: svelte-159x57l-shopBadgeShine 2.6s ease-in-out infinite; }\n"
        "#flyT { position: fixed; --progress: 0; transition: all 1s linear; left: calc(100px + 800px * var(--progress));"
        " animation: e1-flyT 1s linear forwards; }\n"
        "@keyframes e1-flyT { to { --progress: 1; } }\n"
        "#bad { --progress: nope; width: calc(var(--progress) * 1px + 5px); }\n";
    g_cdom = dom_create();
    dom_load_html(g_cdom, html, sizeof html - 1);
    CssSheet *fixture = css_sheet_load(css, sizeof css - 1, "fixture");
    CssSheet *sheets[3] = { svelte, main_css, fixture };
    cascade_set_custom_hook(cascade_hook, NULL);
    CHECK(anim_keyframes_load_sheet(&g_csys, fixture, css_computed_initial()) == 1);
    HtmlElem fly = dom_by_id(g_cdom, "fly"), kid = dom_by_id(g_cdom, "kid"), badge = dom_by_id(g_cdom, "badge");
    HtmlElem flyT = dom_by_id(g_cdom, "flyT"), bad = dom_by_id(g_cdom, "bad");
    CHECK(fly && kid && badge && flyT && bad);

    static const struct { double t, left; } chrome[] = {
        { 0, 100 }, { 200, 100 }, { 350, 363.57 }, { 500, 646.506 }, { 650, 824.902 }, { 800, 900 }, { 1000, 900 }, { 3000, 900 },
    };
    for (size_t i = 0; i < sizeof chrome / sizeof chrome[0]; i++) {
        cascade_frame(sheets, 3, fly, chrome[i].t);
        const CssComputed *c = cascade_elem_computed(g_cdom, fly);
        CHECK(c && c->animation_count == 1 && c->animations[0].duration_s == 1.0f);   /* var(--duration) */
        CHECK(c && c->inset[CSS_LEFT].kind == CSS_LEN_VALUE);
        if (c) CHECK_NEAR(c->inset[CSS_LEFT].px, chrome[i].left, 0.1);
        /* inherits: false: the child does not see the animated value (var() fallback) */
        const CssComputed *k = cascade_elem_computed(g_cdom, kid);
        if (k && i == 3) CHECK_NEAR(k->width.px, 0, 1e-6);   /* initial-value 0, not the parent's --progress */
    }

    /* a registered <number> whose value does not parse: invalid at computed-value time -> initial 0 */
    const CssComputed *bc = cascade_elem_computed(g_cdom, bad);
    if (bc) CHECK_NEAR(bc->width.px, 5, 1e-6);

    /* var() users moved by an animated custom property start no transition under `transition: all`
     * (Chrome #flyT: left 100px, 300px, 500px at 0, 250, 500ms) */
    static const struct { double t, left; } chromeT[] = { { 0, 100 }, { 250, 300 }, { 500, 500 } };
    for (size_t i = 0; i < 3; i++) {
        cascade_frame(sheets, 3, flyT, chromeT[i].t);
        const CssComputed *c = cascade_elem_computed(g_cdom, flyT);
        CssComputed tmp;
        const CssComputed *r = c ? anim_apply(&g_csys, flyT, c, &tmp, chromeT[i].t) : NULL;
        CHECK(r && r->inset[CSS_LEFT].kind == CSS_LEN_VALUE);
        if (r) CHECK_NEAR(r->inset[CSS_LEFT].px, chromeT[i].left, 0.1);
    }

    /* ::after under its own key: the cascade's record + anim_apply */
    cascade_restyle(g_cdom, NULL, sheets, 3);
    const CssComputed *after = cascade_elem_after(g_cdom, badge);
    CHECK(after != NULL);
    if (after) {
        const HtmlElem key = ANIM_KEY(badge, ANIM_PSEUDO_AFTER);
        CssComputed tmp;
        CHECK(anim_update_elem(&g_csys, key, after, 0) == 1);
        const CssComputed *r = anim_apply(&g_csys, key, after, &tmp, 357.5);
        CHECK(r != after);
        CHECK_NEAR(r->inset[CSS_LEFT].pct, -60 + 190 * ref_ease_in_out(0.25), 1e-3);
        CHECK(anim_update_elem(&g_csys, badge, cascade_elem_computed(g_cdom, badge), 0) == 0);   /* the element: none */
    }

    cascade_set_custom_hook(NULL, NULL);
    dom_destroy(g_cdom);
    css_sheet_destroy(fixture);
    css_sheet_destroy(svelte);
    css_sheet_destroy(main_css);
    cascade_shutdown();
    anim_free(&g_csys);
}
#endif

int main(int argc, char **argv)
{
    const char *css_dir = argc > 1 ? argv[1] : "ui/css";
    anim_init(&g_sys, on_dirty, NULL);

    test_timing();
    test_prop_masks();
    test_value_lerp();

    int loaded = 0;
    /* the page's sheets (Krunker v10.0.2's main.css has 38 @keyframes; hud.css and krunknative.css
     * none today), then typewriter / blinkTextCursor, the old map terminal's (gone from the page
     * since the v7.1.4 import), kept here as a fixture for steps() / fill-mode / negative delays */
    static const char k_terminal_keyframes[] =
        "@keyframes typewriter { from { width: 0; } to { width: 25.25em; } }\n"
        "@keyframes blinkTextCursor { from { border-right-color: #33d011; } to { border-right-color: transparent; } }\n";
    const char *files[] = { "main.css", "hud.css", "krunknative.css" };
    for (size_t i = 0; i < 3; i++) {
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
    loaded += load_keyframes_text(&g_sys, k_terminal_keyframes);
    printf("keyframes loaded: %d (%d sets)\n", loaded, g_sys.set_count);
    CHECK(loaded == 40);
    CHECK(g_sys.set_count == 40);
    {   /* v10's Svelte component sheet: only the sets the E1 tests sample are asserted */
        char path[512];
        snprintf(path, sizeof path, "%s/svelte.css", css_dir);
        char *text = read_file(path);
        if (!text) {
            fprintf(stderr, "cannot read %s\n", path);
            return 2;
        }
        int n = load_keyframes_text(&g_sys, text);
        free(text);
        printf("svelte.css keyframes loaded: %d\n", n);
        CHECK(n > 40);
    }
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
    test_back_mov();
    test_pan_background();
    test_bg_pos_pct_sets();
    test_text_shadow_glows();
    test_text_shadow_lists();
    test_shop_badge_shine();
    test_shop_card_zoom();
    test_transition_all_discrete_ints();
    test_registered_progress();
    test_unregistered_custom();
    test_chrome_transform_lists();
    test_chrome_colors_and_typed_customs();
    test_display_toggle_churn();
#ifdef KN_ANIM_TEST_CASCADE
    test_cascade_integration(css_dir);
#endif

    uint32_t ne;
    anim_stats(&g_sys, &ne, NULL, NULL);
    CHECK(ne == 0);
    anim_free(&g_sys);

    printf("test_anim: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
