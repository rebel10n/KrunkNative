/*
 * test_css_values.c - unit tests for css_values.c / css_props.h plus a
 * coverage run over the shipped stylesheets and inline styles.
 *
 *   test_css_values [repo_root]
 *
 * repo_root defaults to "." and is used to find the ui/css sheets, ui/index.html,
 * ui/social.html and the window templates in ui/windows/ (Krunker v10.0.2's page).
 * Exit status is non-zero when any unit case fails or any shipped declaration fails
 * to parse (declarations Chrome drops and the listed known gaps are reported apart).
 *
 * Build (from the repository root):
 *   cc -std=c11 -g -Iclient/src/html/engine -Iclient/include -Ishared/include -Ishared/lib/cJSON \
 *      -Iclient/lib/lexbor client/src/html/engine/tests/test_css_values.c client/src/html/engine/css_values.c \
 *      client/src/html/engine/css_sheet.c client/src/html/engine/css_cascade.c client/src/html/engine/dom.c \
 *      client/src/html/engine/util.c client/lib/lexbor/lexbor.c -lm -o test_css_values && ./test_css_values .
 */
#include "../css_values.h"
#include "../util.h"

#include <math.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail, g_pass;

#define CHECK(cond) do { \
    if (cond) g_pass++; \
    else { g_fail++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

static int feq(float a, float b) { return fabsf(a - b) < 1e-3f; }

/* ------------------------------------------------------------------------- */
/* helpers                                                                    */
/* ------------------------------------------------------------------------- */

static const char *g_vars[][2] = {
    { "--yellow", "#fbc02d" }, { "--blue", "#2196f3" }, { "--green", "#b2f252" }, { "--red", "#ed4242" },
    { "--kn-cross-size", "182px" }, { "--kn-hit-len", "25px" }, { "--kn-hit-thick", "7px" },
    { "--kn-hit-spacing", "37px" }, { "--kn-hit-color", "#fff" },
};

/* Custom properties the scanned stylesheets declare (v7.1.4's main.css / bundled :root
 * blocks: --div-black, --lCSize, ...), collected before the coverage pass so declarations
 * using var() are checked with the value the cascade would substitute. */
static char *dup_n(const char *s, size_t n)
{
    char *d = malloc(n + 1);
    if (!d) exit(99);
    memcpy(d, s, n);
    d[n] = 0;
    return d;
}

#define MAX_SHEET_VARS 256
static char *g_sheet_vars[MAX_SHEET_VARS][2];
static int g_sheet_var_count;

static void collect_sheet_vars(const char *s, size_t len)
{
    for (size_t i = 0; i + 2 < len; i++) {
        if (s[i] != '-' || s[i + 1] != '-' || (i > 0 && s[i - 1] != '{' && s[i - 1] != ';' && s[i - 1] != ' ' &&
                                                s[i - 1] != '\n' && s[i - 1] != '\t')) continue;
        size_t j = i + 2;
        while (j < len && ((kn_is_alpha(s[j]) || kn_is_digit(s[j])) || s[j] == '-' || s[j] == '_')) j++;
        size_t k = j;
        while (k < len && (s[k] == ' ' || s[k] == '\t' || s[k] == '\n')) k++;
        if (k >= len || s[k] != ':') continue;
        size_t v = k + 1, e = v;
        int depth = 0;
        while (e < len && !(depth == 0 && (s[e] == ';' || s[e] == '}'))) {
            if (s[e] == '(') depth++;
            else if (s[e] == ')') depth--;
            e++;
        }
        if (g_sheet_var_count == MAX_SHEET_VARS) return;
        int known = 0;
        for (int n = 0; n < g_sheet_var_count && !known; n++)
            known = strlen(g_sheet_vars[n][0]) == j - i && !memcmp(g_sheet_vars[n][0], s + i, j - i);
        if (known) continue;
        size_t vl = e - v;
        const char *val = kn_str_trim(s + v, &vl);
        g_sheet_vars[g_sheet_var_count][0] = dup_n(s + i, j - i);
        g_sheet_vars[g_sheet_var_count][1] = dup_n(val, vl);
        g_sheet_var_count++;
        i = e;
    }
}

static const char *resolve_var(void *user, const char *name, size_t len)
{
    (void)user;
    for (size_t i = 0; i < sizeof(g_vars) / sizeof(g_vars[0]); i++)
        if (kn_str_eq(name, len, g_vars[i][0])) return g_vars[i][1];
    for (int i = 0; i < g_sheet_var_count; i++)
        if (kn_str_eq(name, len, g_sheet_vars[i][0])) return g_sheet_vars[i][1];
    return NULL;
}

typedef struct ApplyCtx {
    CssComputed *c;
    const CssComputed *parent;
    int calls, failed;
    char failed_prop[64];
} ApplyCtx;

static void apply_cb(void *user, int prop, const CssToken *t, int n, int important)
{
    ApplyCtx *a = user;
    (void)important;
    a->calls++;
    CssToken sub[512];
    const CssToken *use = t;
    int un = n;
    if (css_tokens_have_var(t, n)) {
        un = css_substitute_vars(t, n, sub, 512, resolve_var, NULL);
        if (un < 0) { a->failed++; snprintf(a->failed_prop, sizeof(a->failed_prop), "%s(var)", css_prop_name(prop)); return; }
        use = sub;
    }
    float fs = a->c->font_size > 0 ? a->c->font_size : 16.0f;
    if (!css_computed_apply(a->c, prop, use, un, a->parent, fs)) {
        a->failed++;
        snprintf(a->failed_prop, sizeof(a->failed_prop), "%s", css_prop_name(prop) ? css_prop_name(prop) : "?");
    }
}

/* Parses `name: value` and applies every resulting longhand to *c. */
static int apply(CssComputed *c, const CssComputed *parent, const char *name, const char *value)
{
    ApplyCtx a = { c, parent, 0, 0, "" };
    int ok = css_parse_declaration(name, strlen(name), value, strlen(value), 0, apply_cb, &a);
    return ok && a.calls > 0 && a.failed == 0;
}

static CssComputed fresh(void)
{
    CssComputed c;
    css_computed_init_defaults(&c);
    return c;
}

static int tok(const char *s, CssToken *t, int max)
{
    return css_tokenize(s, strlen(s), t, max);
}

static int color_is(CssColor c, int r, int g, int b, int a)
{
    return c.kind == CSS_COLOR_RGBA && c.r == r && c.g == g && c.b == b && c.a == a;
}

static int len_is(CssLength l, float px, float pct)
{
    return l.kind == CSS_LEN_VALUE && feq(l.px, px) && feq(l.pct, pct) && feq(l.em, 0);
}

/* ------------------------------------------------------------------------- */
/* unit cases                                                                 */
/* ------------------------------------------------------------------------- */

static void test_tokenizer(void)
{
    CssToken t[64];
    int n = tok("calc(50% + 705px - 210px)", t, 64);
    n = css_tokens_strip_ws(t, n);
    CHECK(n == 7);
    CHECK(t[0].type == CSS_TOK_FUNCTION && kn_str_ieq(t[0].text, t[0].len, "calc"));
    CHECK(t[1].type == CSS_TOK_PERCENTAGE && t[1].num == 50);
    CHECK(t[3].type == CSS_TOK_DIMENSION && t[3].unit == CSS_UNIT_PX && t[3].num == 705);
    CHECK(t[6].type == CSS_TOK_RPAREN);

    n = css_tokens_strip_ws(t, tok("url(../img/a.png) #fff 'x y' -0.5em .3 1e2 /* c */ !", t, 64));
    CHECK(n == 7);
    CHECK(t[0].type == CSS_TOK_URL && kn_str_eq(t[0].text, t[0].len, "../img/a.png"));
    CHECK(t[1].type == CSS_TOK_HASH && kn_str_eq(t[1].text, t[1].len, "fff"));
    CHECK(t[2].type == CSS_TOK_STRING && kn_str_eq(t[2].text, t[2].len, "x y"));
    CHECK(t[3].type == CSS_TOK_DIMENSION && t[3].unit == CSS_UNIT_EM && feq((float)t[3].num, -0.5f));
    CHECK(t[4].type == CSS_TOK_NUMBER && feq((float)t[4].num, 0.3f));
    CHECK(t[5].type == CSS_TOK_NUMBER && t[5].num == 100);
    CHECK(t[6].type == CSS_TOK_DELIM && t[6].text[0] == '!');

    n = css_tokens_strip_ws(t, tok("rgb(255 255 255/12%)", t, 64));
    CHECK(n == 7 && t[4].type == CSS_TOK_DELIM && t[5].type == CSS_TOK_PERCENTAGE);

    n = tok("", t, 64);
    CHECK(n == 0);
    n = tok("'unterminated", t, 64);
    CHECK(n == 1 && t[0].type == CSS_TOK_BAD);
    /* truncation is safe */
    n = tok("a b c d e f", t, 3);
    CHECK(n == 3);
}

static void test_props(void)
{
    CHECK(css_prop_lookup("color", 5) == CSS_PROP_COLOR);
    CHECK(css_prop_lookup("COLOR", 5) == CSS_PROP_COLOR);
    CHECK(css_prop_lookup("-webkit-transform", 17) == CSS_PROP_TRANSFORM);
    /* vendor aliases follow Blink: -webkit- ones it honours resolve, everything
     * Chrome drops (-moz-/-o-/-ms-/-khtml-, -webkit-box-flex) is unknown */
    CHECK(css_prop_lookup("-webkit-box-flex", 16) == CSS_PROP_INVALID);
    CHECK(css_prop_lookup("-webkit-box-shadow", 18) == CSS_PROP_BOX_SHADOW);
    CHECK(css_prop_lookup("-webkit-flex-grow", 17) == CSS_PROP_FLEX_GROW);
    CHECK(css_prop_lookup("-webkit-appearance", 18) == CSS_PROP_APPEARANCE);
    {
        static const char *dropped[] = { "-moz-box-shadow", "-moz-transform", "-ms-transform", "-o-transform",
            "-moz-transform-origin", "-moz-border-radius", "-moz-animation", "-moz-transition", "-o-transition",
            "-moz-user-select", "-ms-user-select", "-khtml-user-select", "-moz-appearance", "-moz-box-sizing",
            "-moz-font-feature-settings", "-moz-osx-font-smoothing", "-webkit-text-decoration" };
        for (size_t i = 0; i < sizeof(dropped) / sizeof(dropped[0]); i++)
            CHECK(css_prop_lookup(dropped[i], strlen(dropped[i])) == CSS_PROP_INVALID);
    }
    CHECK(css_prop_lookup("word-wrap", 9) == CSS_PROP_OVERFLOW_WRAP);
    CHECK(css_prop_lookup("margin", 6) == CSS_PROP_MARGIN);
    CHECK(css_prop_lookup("nope", 4) == CSS_PROP_INVALID);
    int a = css_prop_lookup("--kn-cross-size", 15);
    CHECK(CSS_PROP_IS_CUSTOM(a) && css_prop_lookup("--kn-cross-size", 15) == a);
    CHECK(strcmp(css_prop_name(a), "--kn-cross-size") == 0);
    CHECK(strcmp(css_prop_name(CSS_PROP_BACKGROUND_COLOR), "background-color") == 0);
    CHECK(css_prop_is_inherited(CSS_PROP_COLOR) && !css_prop_is_inherited(CSS_PROP_WIDTH));
    CHECK(css_prop_is_inherited(a));
}

static void test_lengths(void)
{
    CssComputed c = fresh();
    CHECK(apply(&c, NULL, "width", "calc(100% - 35px)") && len_is(c.width, -35, 100));
    CHECK(apply(&c, NULL, "width", "-webkit-calc(100% - 35px)") && len_is(c.width, -35, 100));
    CHECK(!apply(&c, NULL, "width", "-moz-calc(1px)") && !apply(&c, NULL, "width", "-o-calc(1px)"));
    CHECK(apply(&c, NULL, "left", "calc(50% + 705px - 210px)") && len_is(c.inset[CSS_LEFT], 495, 50));
    CHECK(apply(&c, NULL, "width", "calc( 100% / 4 )") && len_is(c.width, 0, 25));
    CHECK(apply(&c, NULL, "width", "calc(2 * (10px + 5%))") && len_is(c.width, 20, 10));
    CHECK(apply(&c, NULL, "max-height", "calc(100% - 461px)") && len_is(c.max_height, -461, 100));

    /* viewport units (absolute: resolved at computed time) and min()/max()/clamp() */
    css_values_set_viewport(1280, 720);
    CHECK(apply(&c, NULL, "width", "50vw") && len_is(c.width, 640, 0));
    CHECK(apply(&c, NULL, "height", "calc(100vh - 16px)") && len_is(c.height, 704, 0));
    CHECK(apply(&c, NULL, "width", "10vmin") && len_is(c.width, 72, 0) && css_values_viewport_units_used());
    CHECK(apply(&c, NULL, "width", "min(744px, calc(100vw - 16px))") && len_is(c.width, 744, 0));   /* devconsole.css */
    CHECK(apply(&c, NULL, "height", "min(280px, calc(100vh - 16px))") && len_is(c.height, 280, 0));
    css_values_set_viewport(600, 200);
    CHECK(apply(&c, NULL, "width", "min(744px, calc(100vw - 16px))") && len_is(c.width, 584, 0));
    CHECK(apply(&c, NULL, "height", "min(280px, calc(100vh - 16px))") && len_is(c.height, 184, 0));
    css_values_set_viewport(1280, 720);
    CHECK(apply(&c, NULL, "width", "max(10px, 2em, 30px)") == 0);              /* px vs em: not comparable */
    CHECK(apply(&c, NULL, "width", "max(10px, 20px, 5px)") && len_is(c.width, 20, 0));
    CHECK(apply(&c, NULL, "width", "clamp(100px, 50vw, 400px)") && len_is(c.width, 400, 0));
    CHECK(apply(&c, NULL, "width", "clamp(100px, 5vw, 400px)") && len_is(c.width, 100, 0));
    CHECK(apply(&c, NULL, "width", "calc(min(10%, 20%) + 5px)") && len_is(c.width, 5, 10));
    CHECK(apply(&c, NULL, "width", "max(0px, 25%)") && len_is(c.width, 0, 25));
    CHECK(apply(&c, NULL, "width", "min(50%, 300px)") == 0);                    /* needs layout: rejected */
    CHECK(apply(&c, NULL, "width", "clamp(1px, 2px)") == 0);
    CHECK(apply(&c, NULL, "width", "auto") && c.width.kind == CSS_LEN_AUTO);
    CHECK(apply(&c, NULL, "height", "min-content") && c.height.kind == CSS_LEN_MIN_CONTENT);
    CHECK(apply(&c, NULL, "width", "0") && len_is(c.width, 0, 0));
    CHECK(!apply(&c, NULL, "width", "12") && len_is(c.width, 0, 0));      /* unitless non-zero rejected */
    CHECK(!apply(&c, NULL, "width", "-5px"));                            /* negative width */
    CHECK(apply(&c, NULL, "margin-right", "-50%") && len_is(c.margin[CSS_RIGHT], 0, -50));
    CHECK(!apply(&c, NULL, "width", "calc(100% - )"));
    CHECK(!apply(&c, NULL, "width", "calc(10px / 0)"));
    CHECK(!apply(&c, NULL, "width", "calc(10px * 5px)"));
    /* em resolves against the element's font size */
    c.font_size = 20;
    CHECK(apply(&c, NULL, "width", "1.5em") && len_is(c.width, 30, 0));
    CHECK(apply(&c, NULL, "padding", "1em") && len_is(c.padding[CSS_TOP], 20, 0) && len_is(c.padding[CSS_LEFT], 20, 0));
    /* raw parser keeps em separate */
    CssToken t[16];
    CssLength l;
    int n = tok("calc(1em + 4px)", t, 16);
    CHECK(css_parse_length(t, n, 0, &l) && feq(l.em, 1) && feq(l.px, 4));
    css_length_resolve_em(&l, 10);
    CHECK(feq(l.px, 14) && l.em == 0);
    CHECK(feq(css_length_resolve(&c.max_height, 1000), 539));
    /* font-size em/% resolve against the parent */
    CssComputed parent = fresh();
    parent.font_size = 30;
    CssComputed child = fresh();
    CHECK(apply(&child, &parent, "font-size", "0.5em") && feq(child.font_size, 15));
    CHECK(apply(&child, &parent, "font-size", "200%") && feq(child.font_size, 60));
    CHECK(apply(&child, &parent, "font-size", "165px") && feq(child.font_size, 165));
    /* line-height */
    CHECK(apply(&c, NULL, "line-height", "1") && c.line_height.kind == CSS_LEN_NUMBER && feq(c.line_height.px, 1));
    CHECK(apply(&c, NULL, "line-height", "28px") && len_is(c.line_height, 28, 0));
    CHECK(apply(&c, NULL, "line-height", "normal") && c.line_height.kind == CSS_LEN_NORMAL);
    CHECK(apply(&c, NULL, "letter-spacing", "-2px") && len_is(c.letter_spacing, -2, 0));
}

static void test_var(void)
{
    CssComputed c = fresh();
    CHECK(apply(&c, NULL, "top", "calc(var(--kn-cross-size) / -2)") && len_is(c.inset[CSS_TOP], -91, 0));
    CHECK(apply(&c, NULL, "top", "calc(var(--kn-cross-size) / 2 - 21px)") && len_is(c.inset[CSS_TOP], 70, 0));
    CHECK(apply(&c, NULL, "top", "calc(var(--kn-hit-len) / -2 - var(--kn-hit-spacing))") && len_is(c.inset[CSS_TOP], -49.5f, 0));
    CHECK(apply(&c, NULL, "left", "calc(var(--kn-hit-len) / -2 + var(--kn-hit-spacing))") && len_is(c.inset[CSS_LEFT], 24.5f, 0));
    CHECK(apply(&c, NULL, "color", "var(--yellow)") && color_is(c.color, 0xfb, 0xc0, 0x2d, 255));
    CHECK(apply(&c, NULL, "border", "4px solid var(--blue)") && color_is(c.border_color[CSS_LEFT], 0x21, 0x96, 0xf3, 255) &&
          feq(c.border_width[CSS_TOP], 4) && c.border_style[CSS_BOTTOM] == CSS_BORDER_STYLE_SOLID);
    CHECK(!apply(&c, NULL, "color", "var(--missing)"));
    CHECK(apply(&c, NULL, "color", "var(--missing, red)") && color_is(c.color, 255, 0, 0, 255));
    CssToken in[16], out[16];
    int n = tok("var(--kn-hit-thick)", in, 16);
    CHECK(css_tokens_have_var(in, n));
    int m = css_substitute_vars(in, n, out, 16, resolve_var, NULL);
    CHECK(m == 1 && out[0].type == CSS_TOK_DIMENSION && out[0].num == 7);
    /* custom property storage and lookup */
    CHECK(apply(&c, NULL, "--kn-cross-size", " 182px ") && strcmp(css_computed_custom_get(&c, "--kn-cross-size", 15), "182px") == 0);
    CssComputed child = fresh();
    css_computed_inherit(&child, &c);
    CHECK(css_computed_custom_get(&child, "--kn-cross-size", 15) != NULL);
}

static void test_colors(void)
{
    CssComputed c = fresh();
    CHECK(apply(&c, NULL, "color", "#fff") && color_is(c.color, 255, 255, 255, 255));
    CHECK(apply(&c, NULL, "color", "#2196F3") && color_is(c.color, 0x21, 0x96, 0xf3, 255));
    CHECK(apply(&c, NULL, "color", "#11223344") && color_is(c.color, 0x11, 0x22, 0x33, 0x44));
    CHECK(apply(&c, NULL, "color", "rgba(255, 255, 255, 0.7)") && color_is(c.color, 255, 255, 255, 179));
    CHECK(apply(&c, NULL, "color", "rgba(150, 150, 150)") && color_is(c.color, 150, 150, 150, 255));
    CHECK(apply(&c, NULL, "color", "rgb(255 255 255)") && color_is(c.color, 255, 255, 255, 255));
    CHECK(apply(&c, NULL, "color", "rgb(255 255 255/12%)") && color_is(c.color, 255, 255, 255, 31));
    CHECK(apply(&c, NULL, "color", "rgb(100%, 0%, 0%)") && color_is(c.color, 255, 0, 0, 255));
    CHECK(apply(&c, NULL, "color", "hsl(120, 100%, 50%)") && color_is(c.color, 0, 255, 0, 255));
    CHECK(apply(&c, NULL, "color", "Crimson") && color_is(c.color, 0xdc, 0x14, 0x3c, 255));
    CHECK(apply(&c, NULL, "color", "MediumBlue") && color_is(c.color, 0, 0, 0xcd, 255));
    CHECK(apply(&c, NULL, "color", "lightgrey") && color_is(c.color, 0xd3, 0xd3, 0xd3, 255));
    CHECK(apply(&c, NULL, "background-color", "transparent") && color_is(c.background_color, 0, 0, 0, 0));
    CHECK(apply(&c, NULL, "border-top-color", "currentColor") && c.border_color[CSS_TOP].kind == CSS_COLOR_CURRENT);
    CssColor before = c.color;
    CHECK(!apply(&c, NULL, "color", "#ggg") && memcmp(&before, &c.color, sizeof(before)) == 0);
    CHECK(!apply(&c, NULL, "color", "rgb(1,2)"));
    CHECK(!apply(&c, NULL, "color", "notacolor"));
    CssComputed parent = fresh();
    parent.color = css_color_rgba(1, 2, 3, 255);
    CHECK(apply(&c, &parent, "color", "inherit") && color_is(c.color, 1, 2, 3, 255));
    CHECK(apply(&c, &parent, "color", "currentcolor") && color_is(c.color, 1, 2, 3, 255));
    CHECK(apply(&c, &parent, "color", "initial") && color_is(c.color, 0, 0, 0, 255));
    CHECK(apply(&c, &parent, "color", "unset") && color_is(c.color, 1, 2, 3, 255));
}

static void test_transforms(void)
{
    CssComputed c = fresh();
    CHECK(apply(&c, NULL, "transform", "translate(-50%,-50%) scale(1.1)") && c.transform.count == 2 &&
          c.transform.ops[0].kind == CSS_TRANSFORM_TRANSLATE && len_is(c.transform.ops[0].tx, 0, -50) &&
          len_is(c.transform.ops[0].ty, 0, -50) && c.transform.ops[1].kind == CSS_TRANSFORM_SCALE &&
          feq(c.transform.ops[1].sx, 1.1f) && feq(c.transform.ops[1].sy, 1.1f));
    CHECK(apply(&c, NULL, "transform", "translateZ(0)") && c.transform.count == 0 && c.transform.has_3d);
    CHECK(apply(&c, NULL, "-webkit-transform", "scaleX(-1)") && c.transform.count == 1 && feq(c.transform.ops[0].sx, -1) && feq(c.transform.ops[0].sy, 1));
    CHECK(apply(&c, NULL, "transform", "translateX(-50%) rotate(6.5deg)") && c.transform.count == 2 && feq(c.transform.ops[1].angle_deg, 6.5f));
    CHECK(apply(&c, NULL, "transform", "translateY(-180px)") && len_is(c.transform.ops[0].ty, -180, 0));
    CHECK(apply(&c, NULL, "transform", "rotate(0.25turn)") && feq(c.transform.ops[0].angle_deg, 90));
    CHECK(apply(&c, NULL, "transform", "matrix(1,0,0,1,10,20)") && feq(c.transform.ops[0].m.e, 10));
    CHECK(apply(&c, NULL, "transform", "none") && c.transform.count == 0);
    CHECK(apply(&c, NULL, "transform", "unset") && c.transform.count == 0);
    CHECK(apply(&c, NULL, "transform", "scale(2)"));
    CHECK(!apply(&c, NULL, "transform", "scale(2") || c.transform.count == 1);
    CHECK(!apply(&c, NULL, "transform", "spin(3deg)") && c.transform.count == 1);
    CHECK(!apply(&c, NULL, "transform", "rotate(5px)"));
    CHECK(apply(&c, NULL, "transform-origin", "0 0") && len_is(c.transform_origin_x, 0, 0) && len_is(c.transform_origin_y, 0, 0));
    CHECK(apply(&c, NULL, "transform-origin", "right bottom") && len_is(c.transform_origin_x, 0, 100) && len_is(c.transform_origin_y, 0, 100));
    CssComputed d = fresh();
    CHECK(len_is(d.transform_origin_x, 0, 50) && len_is(d.transform_origin_y, 0, 50));
}

/* E2 (ui-e2-paint): 3D transforms, backdrop-filter, masks, background-clip:text,
 * color-mix(), outline-offset, accent-color, aspect-ratio, line clamp, @property. */
static void test_e2_values(void)
{
    CssComputed c = fresh();
    /* 3D transforms (svelte.css .reward-card.flipped, main.css rotateX/Y) */
    CHECK(apply(&c, NULL, "transform", "rotateY(180deg)") && c.transform.count == 1 && c.transform.has_3d &&
          c.transform.ops[0].kind == CSS_TRANSFORM_ROTATE_3D && feq(c.transform.ops[0].angle_deg, 180) &&
          feq(c.transform.ops[0].axis[1], 1) && css_transform_is_3d(&c.transform));
    CHECK(apply(&c, NULL, "transform", "rotateX(-37.5deg) rotateY(45deg)") && c.transform.count == 2 &&
          feq(c.transform.ops[0].axis[0], 1) && feq(c.transform.ops[1].axis[1], 1));
    CHECK(apply(&c, NULL, "transform", "rotate3d(0, 0, 2, 30deg)") && c.transform.ops[0].kind == CSS_TRANSFORM_ROTATE &&
          feq(c.transform.ops[0].angle_deg, 30) && !css_transform_is_3d(&c.transform));
    CHECK(apply(&c, NULL, "transform", "perspective(500px) translate3d(1px, 2px, 30px)") && c.transform.count == 2 &&
          c.transform.ops[0].kind == CSS_TRANSFORM_PERSPECTIVE && feq(c.transform.ops[0].persp, 500) && feq(c.transform.ops[1].tz, 30));
    CHECK(apply(&c, NULL, "transform", "translateZ(0) translateY(10px)") && c.transform.count == 1 && c.transform.has_3d &&
          !css_transform_is_3d(&c.transform));
    CHECK(apply(&c, NULL, "transform", "scale3d(2, 3, 4)") && feq(c.transform.ops[0].sz, 4));
    CHECK(apply(&c, NULL, "transform", "scale(2)") && feq(c.transform.ops[0].sz, 1) && !css_transform_is_3d(&c.transform));
    {
        /* rotateY(60deg) without perspective flattens to scaleX(0.5) */
        float m[16];
        CHECK(apply(&c, NULL, "transform", "rotateY(60deg)"));
        css_transform_to_mat4(&c.transform, 100, 100, m);
        CHECK(feq(m[0], 0.5f) && feq(m[5], 1) && feq(m[2], -0.8660254f) && feq(m[8], 0.8660254f));
        CHECK(apply(&c, NULL, "transform", "translate(50%, 10px) rotate(90deg)"));
        css_transform_to_mat4(&c.transform, 200, 100, m);
        CHECK(feq(m[12], 100) && feq(m[13], 10) && feq(m[0], 0) && feq(m[1], 1) && feq(m[4], -1));
        CHECK(apply(&c, NULL, "transform", "perspective(100px)"));
        css_transform_to_mat4(&c.transform, 1, 1, m);
        CHECK(feq(m[11], -0.01f));
        /* none -> rotateY(180deg) interpolates the angle about the same axis */
        CssTransformOp id, mid;
        CHECK(apply(&c, NULL, "transform", "rotateY(180deg)"));
        css_transform_op_identity(&id, &c.transform.ops[0]);
        CHECK(id.kind == CSS_TRANSFORM_ROTATE_3D && feq(id.axis[1], 1) && feq(id.angle_deg, 0));
        CHECK(css_transform_op_lerp(&id, &c.transform.ops[0], 0.5f, &mid) && feq(mid.angle_deg, 90) && feq(mid.axis[1], 1));
        CHECK(!css_transform_op_equal(&id, &c.transform.ops[0]) && css_transform_op_equal(&mid, &mid));
    }
    CHECK(apply(&c, NULL, "transform-style", "preserve-3d") && c.transform_style == CSS_TRANSFORM_STYLE_PRESERVE_3D);
    CHECK(apply(&c, NULL, "-webkit-transform-style", "flat") && c.transform_style == CSS_TRANSFORM_STYLE_FLAT);
    CHECK(apply(&c, NULL, "backface-visibility", "hidden") && c.backface_visibility == CSS_BACKFACE_HIDDEN);
    CHECK(apply(&c, NULL, "perspective", "800px") && c.perspective.kind == CSS_LEN_VALUE && feq(c.perspective.px, 800));
    CHECK(apply(&c, NULL, "perspective", "none") && c.perspective.kind == CSS_LEN_NONE);
    CHECK(apply(&c, NULL, "perspective-origin", "left top") && len_is(c.perspective_origin_x, 0, 0));

    /* backdrop-filter (main.css .death-ui-bottom) */
    CHECK(apply(&c, NULL, "backdrop-filter", "blur(10px)") && c.backdrop_filter_count == 1 &&
          c.backdrop_filters[0].kind == CSS_FILTER_BLUR && feq(c.backdrop_filters[0].amount, 10));
    CHECK(apply(&c, NULL, "-webkit-backdrop-filter", "blur(6px) brightness(0.8)") && c.backdrop_filter_count == 2);
    CHECK(apply(&c, NULL, "backdrop-filter", "none") && c.backdrop_filter_count == 0);

    /* masks (.menuItemIconMask, the lottery frame) */
    CHECK(apply(&c, NULL, "-webkit-mask-image", "url(img/icon.png)") && c.mask_layer_count == 1 &&
          c.mask_layers[0].image.kind == CSS_IMAGE_URL);
    CHECK(apply(&c, NULL, "mask-size", "contain") && c.mask_layers[0].size.kind == CSS_BG_SIZE_CONTAIN);
    CHECK(apply(&c, NULL, "mask-repeat", "no-repeat") && c.mask_layers[0].repeat_x == CSS_BG_REPEAT_NO_REPEAT);
    CHECK(apply(&c, NULL, "mask-position", "center") && len_is(c.mask_layers[0].x, 0, 50) && len_is(c.mask_layers[0].y, 0, 50));
    CHECK(apply(&c, NULL, "mask-mode", "alpha") && c.mask_mode == CSS_MASK_MODE_ALPHA);
    CHECK(apply(&c, NULL, "mask", "linear-gradient(#000, #000) 0 0 / 100% 30px no-repeat, linear-gradient(#000, #000) 0 100% / 100% 30px no-repeat,"
                                  " linear-gradient(#000, #000) 0 0 / 30px 100% no-repeat, linear-gradient(#000, #000) 100% 0 / 30px 100% no-repeat") &&
          c.mask_layer_count == 4 && c.mask_layers[3].image.kind == CSS_IMAGE_GRADIENT && len_is(c.mask_layers[3].x, 0, 100) &&
          len_is(c.mask_layers[1].y, 0, 100) && len_is(c.mask_layers[2].size.w, 30, 0) && c.mask_layers[0].repeat_y == CSS_BG_REPEAT_NO_REPEAT);
    CHECK(apply(&c, NULL, "mask-image", "none") && c.mask_layer_count == 0);

    /* background-clip: text + -webkit-text-fill-color (.vs-text) */
    CHECK(apply(&c, NULL, "background", "linear-gradient(135deg, #5ce05a 0%, #ff6b6b 100%)") && c.background_clip == CSS_BG_BOX_BORDER);
    CHECK(apply(&c, NULL, "-webkit-background-clip", "text") && c.background_clip == CSS_BG_BOX_TEXT);
    CHECK(apply(&c, NULL, "background", "red padding-box") && c.background_clip == CSS_BG_BOX_PADDING && c.background_origin == CSS_BG_BOX_PADDING);
    CHECK(apply(&c, NULL, "background-origin", "content-box") && c.background_origin == CSS_BG_BOX_CONTENT);
    CHECK(!apply(&c, NULL, "background-origin", "text"));
    CHECK(fresh().text_fill_color.kind == CSS_COLOR_CURRENT);
    CHECK(apply(&c, NULL, "-webkit-text-fill-color", "transparent") && c.text_fill_color.a == 0 && c.text_fill_color.kind == CSS_COLOR_RGBA);

    /* color-mix() (main.css .chalRewTile / .titleRow) */
    CHECK(apply(&c, NULL, "background-color", "color-mix(in srgb, currentColor 30%, transparent)") &&
          c.background_color.kind == CSS_COLOR_CURRENT_MIX && c.background_color.mix == 60);
    {
        CssColor r = css_color_resolve(c.background_color, css_color_rgba(255, 0, 0, 255));
        CHECK(color_is(r, 255, 0, 0, 77));
    }
    CHECK(apply(&c, NULL, "color", "color-mix(in srgb, red 50%, blue)") && color_is(c.color, 128, 0, 128, 255));
    CHECK(apply(&c, NULL, "color", "color-mix(in srgb, red, blue 25%)") && color_is(c.color, 191, 0, 64, 255));
    CHECK(apply(&c, NULL, "color", "color-mix(in oklab, white, black)") && c.color.r > 90 && c.color.r < 110 && c.color.r == c.color.g);
    CHECK(apply(&c, NULL, "color", "color-mix(in srgb, red 20%, blue 20%)") && color_is(c.color, 128, 0, 128, 102));
    CHECK(apply(&c, NULL, "background", "linear-gradient(to top, color-mix(in srgb, currentColor 30%, transparent), transparent)") &&
          c.bg_layers[0].image.gradient.stops[0].color.kind == CSS_COLOR_CURRENT_MIX);
    CHECK(apply(&c, NULL, "border-bottom", "2px solid color-mix(in srgb, currentColor 45%, transparent)") &&
          c.border_color[CSS_BOTTOM].kind == CSS_COLOR_CURRENT_MIX);
    CHECK(!apply(&c, NULL, "color", "color-mix(in hsl, red, blue)") && !apply(&c, NULL, "color", "color-mix(in srgb red, blue)"));
    /* the interpolation method is optional (CSS Color 5 §2, default oklab; Chrome 152 accepts it) */
    /* Chrome: color-mix(red, blue) == oklab(0.539974 0.0962086 -0.0928316) = sRGB (140, 83, 162) */
    CHECK(apply(&c, NULL, "color", "color-mix(red, blue)") && c.color.nonlegacy == 1 && c.color.kind == CSS_COLOR_RGBA);
    CHECK(c.color.r > 135 && c.color.r < 145 && c.color.g > 75 && c.color.g < 90 && c.color.b > 155 && c.color.b < 170);
    /* non-legacy results (Oklab transitions); plain colours stay legacy */
    CHECK(apply(&c, NULL, "color", "color-mix(in srgb, red 50%, blue)") && c.color.nonlegacy == 1);
    CHECK(apply(&c, NULL, "background-color", "color-mix(in srgb, currentColor 30%, transparent)") && c.background_color.nonlegacy == 1 &&
          css_color_resolve(c.background_color, css_color_rgba(255, 0, 0, 255)).nonlegacy == 1);
    CHECK(apply(&c, NULL, "color", "#ff0000") && c.color.nonlegacy == 0);
    CHECK(apply(&c, NULL, "color", "rgb(1 2 3 / 50%)") && c.color.nonlegacy == 0);
    {
        CssComputed parent = fresh(), child = fresh();
        parent.color = css_color_rgba(0, 0, 255, 255);
        CHECK(apply(&child, &parent, "color", "color-mix(in srgb, currentColor 50%, white)") && color_is(child.color, 128, 128, 255, 255));
    }

    /* outline-offset, accent-color */
    CHECK(apply(&c, NULL, "outline-offset", "-2px") && feq(c.outline_offset, -2));
    CHECK(apply(&c, NULL, "outline-style", "auto") && c.outline_style == CSS_BORDER_STYLE_AUTO);
    CHECK(apply(&c, NULL, "outline", "2px solid #4a9c0f") && c.outline_style == CSS_BORDER_STYLE_SOLID);
    CHECK(fresh().accent_color_auto == 1);
    CHECK(apply(&c, NULL, "accent-color", "#2196f3") && !c.accent_color_auto && color_is(c.accent_color, 0x21, 0x96, 0xf3, 255));
    CHECK(css_prop_is_inherited(CSS_PROP_ACCENT_COLOR) && css_prop_is_inherited(CSS_PROP_TEXT_FILL_COLOR));

    /* E3: aspect-ratio, -webkit-line-clamp, -webkit-box-orient, legacy box display */
    CHECK(apply(&c, NULL, "aspect-ratio", "1170 / 150") && feq(c.aspect_ratio, 7.8f) && !c.aspect_ratio_auto);
    CHECK(apply(&c, NULL, "aspect-ratio", "auto 3/4") && feq(c.aspect_ratio, 0.75f) && c.aspect_ratio_auto);
    CHECK(apply(&c, NULL, "aspect-ratio", "1") && feq(c.aspect_ratio, 1));
    CHECK(apply(&c, NULL, "aspect-ratio", "0 / 1") && c.aspect_ratio == 0 && c.aspect_ratio_auto);
    CHECK(apply(&c, NULL, "aspect-ratio", "auto") && c.aspect_ratio == 0 && c.aspect_ratio_auto);
    CHECK(!apply(&c, NULL, "aspect-ratio", "-1"));
    CHECK(apply(&c, NULL, "-webkit-line-clamp", "3") && c.line_clamp == 3);
    CHECK(apply(&c, NULL, "-webkit-line-clamp", "none") && c.line_clamp == 0);
    CHECK(!apply(&c, NULL, "-webkit-line-clamp", "0"));
    CHECK(apply(&c, NULL, "-webkit-box-orient", "vertical") && c.box_orient == CSS_BOX_ORIENT_VERTICAL);
    CHECK(apply(&c, NULL, "display", "-webkit-box") && c.display == CSS_DISPLAY_FLEX && c.display_legacy_box);
    CHECK(apply(&c, NULL, "display", "flex") && c.display == CSS_DISPLAY_FLEX && !c.display_legacy_box);

    /* @property (svelte.css --progress) */
    {
        int id = css_property_register("--e2-progress", 13, "'<number>'", 10, "false", 5, "0", 1);
        CHECK(CSS_PROP_IS_CUSTOM(id) && !css_prop_is_inherited(id));
        const CssRegisteredProperty *r = css_property_registration(id);
        CHECK(r && r->syntax == CSS_SYNTAX_NUMBER && !r->inherits && r->initial && strcmp(r->initial, "0") == 0);
        CHECK(css_property_register("--e2-bad", 8, "'<number>'", 10, "false", 5, NULL, 0) == CSS_PROP_INVALID);
        CHECK(css_property_register("--e2-bad", 8, "'<image>'", 9, "true", 4, "none", 4) == CSS_PROP_INVALID);
        CHECK(css_parse_syntax("\"<length-percentage>\"", 21) == CSS_SYNTAX_LENGTH_PERCENTAGE);
        CssTypedValue a, b, m;
        CHECK(css_typed_value_parse(CSS_SYNTAX_NUMBER, "0", 1, 16, &a) && css_typed_value_parse(CSS_SYNTAX_NUMBER, "1", 1, 16, &b));
        css_typed_value_lerp(&a, &b, 0.25f, &m);
        char buf[64];
        css_typed_value_format(&m, buf, sizeof buf);
        CHECK(feq(m.num, 0.25f) && strcmp(buf, "0.25") == 0);
        CHECK(css_typed_value_parse(CSS_SYNTAX_LENGTH, "2em", 3, 10, &a) && feq(a.len.px, 20));
        css_typed_value_format(&a, buf, sizeof buf);
        CHECK(strcmp(buf, "20px") == 0);
        CHECK(!css_typed_value_parse(CSS_SYNTAX_NUMBER, "1px", 3, 16, &a));
        css_property_registry_clear();
        CHECK(css_property_registration(id) == NULL && css_prop_is_inherited(id));
    }
}

static void test_shadows(void)
{
    CssComputed c = fresh();
    CHECK(apply(&c, NULL, "box-shadow", "0 0 0 4000px rgba(0,0,0,.5)") && c.box_shadow_count == 1 &&
          feq(c.box_shadow.spread, 4000) && color_is(c.box_shadow.color, 0, 0, 0, 128) && !c.box_shadow.inset);
    CHECK(apply(&c, NULL, "box-shadow", "inset 0 -7px 0 0 #b3b3b3") && c.box_shadow.inset && feq(c.box_shadow.y, -7));
    CHECK(apply(&c, NULL, "box-shadow", "#000 0 0 1000px inset") && c.box_shadow.inset && feq(c.box_shadow.blur, 1000) && feq(c.box_shadow.spread, 0) && color_is(c.box_shadow.color, 0, 0, 0, 255));
    CHECK(apply(&c, NULL, "box-shadow", "0 0 1px #2196f3") && feq(c.box_shadow.blur, 1) && feq(c.box_shadow.spread, 0));
    CHECK(apply(&c, NULL, "box-shadow", "0 0 8px") && c.box_shadow.color.kind == CSS_COLOR_CURRENT);
    CHECK(apply(&c, NULL, "box-shadow", "0 1px red, 0 2px blue") && c.box_shadow_count == 2 && color_is(c.box_shadow.color, 255, 0, 0, 255) && color_is(c.box_shadow_more[0].color, 0, 0, 255, 255));
    CHECK(apply(&c, NULL, "box-shadow", "none") && c.box_shadow_count == 0);
    CHECK(!apply(&c, NULL, "box-shadow", "0 0 -2px red"));
    CHECK(!apply(&c, NULL, "box-shadow", "0 red 0"));
    CHECK(!apply(&c, NULL, "box-shadow", "0"));
    CHECK(apply(&c, NULL, "text-shadow", "#000 0 0 3px, 0 1px 0 #fff") && c.text_shadow.count == 2 &&
          feq(c.text_shadow.items[0].blur, 3) && color_is(c.text_shadow.items[1].color, 255, 255, 255, 255) && feq(c.text_shadow.items[1].y, 1));
    CHECK(apply(&c, NULL, "text-shadow", "-1px -1px 0 #202020, 1px -1px 0 #202020, -1px 1px 0 #202020, 1px 1px 0 #202020, -2px -2px 0 #202020, 2px -2px 0 #202020, -2px 2px 0 #202020, 2px 2px 0 #202020, -3px -3px 0 #202020, 3px -3px 0 #202020, -3px 3px 0 #202020, 3px 3px 0 #202020") &&
          c.text_shadow.count == 12 && feq(c.text_shadow.items[11].x, 3));
    /* Krunker Strike's text outline (.class-selection__class-name span): 9 shadows of calc() offsets,
     * ~260 tokens with the whitespace; a longer value must not be cut short and dropped. */
    {
        char outline[2048] = "";
        static const int OFF[9][2] = {{-1, -1}, {-1, 0}, {-1, 1}, {0, -1}, {0, 0}, {0, 1}, {1, -1}, {1, 0}, {1, 1}};
        for (int i = 0; i < 9; i++) {
            char one[160];
            snprintf(one, sizeof(one), "%scalc(%d * 2px) calc(%d * 2px) 0 rgba(0,0,0,.5)", i ? ", " : "", OFF[i][0], OFF[i][1]);
            strcat(outline, one);
        }
        CHECK(apply(&c, NULL, "text-shadow", outline) && c.text_shadow.count == 9 &&
              feq(c.text_shadow.items[8].x, 2) && feq(c.text_shadow.items[8].y, 2) && (color_is(c.text_shadow.items[8].color, 0, 0, 0, 128) || color_is(c.text_shadow.items[8].color, 0, 0, 0, 127)));
    }
    CHECK(!apply(&c, NULL, "text-shadow", "inset 0 0 1px red"));
    CHECK(!apply(&c, NULL, "text-shadow", "0 0 0 0 red"));
    CHECK(apply(&c, NULL, "text-shadow", "none") && c.text_shadow.count == 0);
}

static void test_backgrounds(void)
{
    CssComputed c = fresh();
    CHECK(apply(&c, NULL, "background", "linear-gradient(to right, #fff 85%, #000 85%)") && c.bg_layer_count == 1 &&
          c.bg_layers[0].image.kind == CSS_IMAGE_GRADIENT && feq(c.bg_layers[0].image.gradient.angle_deg, 90) &&
          c.bg_layers[0].image.gradient.stop_count == 2 && len_is(c.bg_layers[0].image.gradient.stops[1].pos, 0, 85) &&
          color_is(c.background_color, 0, 0, 0, 0));
    CHECK(apply(&c, NULL, "background", "-webkit-linear-gradient( left, rgba(0, 0, 0, 0) 0%, rgba(0, 0, 0, 0.3) 40%, rgba(0, 0, 0, 0.3) 60%, rgba(0, 0, 0, 0) 100% )") &&
          feq(c.bg_layers[0].image.gradient.angle_deg, 90) && c.bg_layers[0].image.gradient.stop_count == 4);
    /* devconsole.css .grip: double-position hard stops (13 stops) */
    CHECK(apply(&c, NULL, "background", "linear-gradient(135deg, transparent 0 6px, #9a9a9a 6px 7px, transparent 7px 9px, "
                "#9a9a9a 9px 10px, transparent 10px 12px, #9a9a9a 12px 13px, transparent 13px)") &&
          c.bg_layers[0].image.gradient.stop_count == 13 && len_is(c.bg_layers[0].image.gradient.stops[1].pos, 6, 0) &&
          len_is(c.bg_layers[0].image.gradient.stops[2].pos, 6, 0) && color_is(c.bg_layers[0].image.gradient.stops[2].color, 0x9a, 0x9a, 0x9a, 255) &&
          len_is(c.bg_layers[0].image.gradient.stops[12].pos, 13, 0));
    CHECK(apply(&c, NULL, "background", "linear-gradient(45deg, red, blue)") && feq(c.bg_layers[0].image.gradient.angle_deg, 45) &&
          !c.bg_layers[0].image.gradient.stops[0].has_pos);
    CHECK(apply(&c, NULL, "background", "radial-gradient(ellipse at 50% 40%, #6f8aa6 0%, #3c4a5c 55%, #1a2029 100%)") &&
          c.bg_layers[0].image.gradient.kind == CSS_GRADIENT_RADIAL && len_is(c.bg_layers[0].image.gradient.cy, 0, 40) &&
          c.bg_layers[0].image.gradient.stop_count == 3);
    CHECK(apply(&c, NULL, "background", "url(../img/lock-icon-black.svg) no-repeat right 8px center") &&
          c.bg_layers[0].image.kind == CSS_IMAGE_URL && strcmp(c.bg_layers[0].image.url, "../img/lock-icon-black.svg") == 0 &&
          c.bg_layers[0].repeat_x == CSS_BG_REPEAT_NO_REPEAT && len_is(c.bg_layers[0].x, -8, 100) && len_is(c.bg_layers[0].y, 0, 50));
    CHECK(apply(&c, NULL, "background", "rgba(255, 255, 255, 0.8)") && c.bg_layer_count == 0 && color_is(c.background_color, 255, 255, 255, 204));
    CHECK(apply(&c, NULL, "background", "#000 url(\"a.png\") center / cover repeat-x") && c.bg_layers[0].size.kind == CSS_BG_SIZE_COVER &&
          c.bg_layers[0].repeat_y == CSS_BG_REPEAT_NO_REPEAT && strcmp(c.bg_layers[0].image.url, "a.png") == 0);
    CHECK(apply(&c, NULL, "background-image", "url(../img/dropper_fg.svg), url(../img/dropper_bg.svg)") && c.bg_layer_count == 2 &&
          strcmp(c.bg_layers[1].image.url, "../img/dropper_bg.svg") == 0);
    CHECK(apply(&c, NULL, "background-size", "60%, contain") && len_is(c.bg_layers[0].size.w, 0, 60) && c.bg_layers[0].size.h.kind == CSS_LEN_AUTO &&
          c.bg_layers[1].size.kind == CSS_BG_SIZE_CONTAIN);
    CHECK(apply(&c, NULL, "background-size", "100% 100%") && len_is(c.bg_layers[0].size.h, 0, 100));
    CHECK(apply(&c, NULL, "background-size", "18px auto") && len_is(c.bg_layers[0].size.w, 18, 0));
    CHECK(apply(&c, NULL, "background-size", "cover"));
    CHECK(apply(&c, NULL, "background-repeat", "no-repeat, no-repeat") && c.bg_layers[1].repeat_x == CSS_BG_REPEAT_NO_REPEAT);
    CHECK(apply(&c, NULL, "background-repeat", "round") && c.bg_layers[0].repeat_x == CSS_BG_REPEAT_ROUND);
    CHECK(apply(&c, NULL, "background-position", "center") && len_is(c.bg_layers[0].x, 0, 50) && len_is(c.bg_layers[0].y, 0, 50));
    CHECK(apply(&c, NULL, "background-position", "-32px -64px") && len_is(c.bg_layers[0].x, -32, 0) && len_is(c.bg_layers[0].y, -64, 0));
    CHECK(apply(&c, NULL, "background-position-x", "calc(50% - 5px), center") && len_is(c.bg_layers[0].x, -5, 50) && len_is(c.bg_layers[1].x, 0, 50));
    CHECK(apply(&c, NULL, "background-position", "bottom 10px center") && len_is(c.bg_layers[0].y, -10, 100) && len_is(c.bg_layers[0].x, 0, 50));
    CHECK(apply(&c, NULL, "background-position-x", "right 8px") && len_is(c.bg_layers[0].x, -8, 100));
    CHECK(apply(&c, NULL, "background-position", "right 8px") && len_is(c.bg_layers[0].x, 0, 100) && len_is(c.bg_layers[0].y, 8, 0));
    CHECK(!apply(&c, NULL, "background-position-x", "top"));
    CHECK(!apply(&c, NULL, "background", "url(a.png) bogus"));
    CHECK(!apply(&c, NULL, "background", "linear-gradient(to middle, red, blue)"));
    CHECK(!apply(&c, NULL, "background-size", "-10px"));
}

static void test_filters_clip(void)
{
    CssComputed c = fresh();
    CHECK(apply(&c, NULL, "filter", "grayscale(1) drop-shadow(0 0 7px #000)") && c.filter_count == 2 &&
          c.filters[0].kind == CSS_FILTER_GRAYSCALE && feq(c.filters[0].amount, 1) &&
          c.filters[1].kind == CSS_FILTER_DROP_SHADOW && feq(c.filters[1].shadow.blur, 7));
    CHECK(apply(&c, NULL, "-webkit-filter", "grayscale(100%) brightness(0)") && feq(c.filters[0].amount, 1) && feq(c.filters[1].amount, 0));
    CHECK(apply(&c, NULL, "filter", "saturate(40) hue-rotate(-40deg)") && feq(c.filters[0].amount, 40) && feq(c.filters[1].amount, -40));
    CHECK(apply(&c, NULL, "filter", "drop-shadow(rgba(0, 0, 0, 0.5) 0px 0px 10px)") && feq(c.filters[0].shadow.blur, 10));
    CHECK(apply(&c, NULL, "filter", "drop-shadow(1px 0px 0 black) drop-shadow(0px 1px 0 black) drop-shadow(-1px -0px 0 black) drop-shadow(-0px -1px 0 black)") && c.filter_count == 4);
    CHECK(apply(&c, NULL, "filter", "none") && c.filter_count == 0);
    CHECK(!apply(&c, NULL, "filter", "grayscale(-1)"));
    CHECK(!apply(&c, NULL, "filter", "wobble(1)"));
    CHECK(apply(&c, NULL, "clip-path", "polygon(0 0, 100% 0, 100% 100%, 11% 100%, 0 0)") && c.clip_path.kind == CSS_CLIP_PATH_POLYGON &&
          c.clip_path.point_count == 5 && len_is(c.clip_path.x[3], 0, 11) && len_is(c.clip_path.y[3], 0, 100));
    CHECK(!apply(&c, NULL, "clip-path", "polygon(0 0, 10px)"));
    /* Krunker Strike's frames: 20 points in two rings (more than the old 8), and the fill rule */
    CHECK(apply(&c, NULL, "clip-path", "polygon(0 3px,3.5px 0,calc(100% - 4.5px) 0,100% 4.5px,100% calc(100% - 3.5px),"
          "calc(100% - 3.5px) 100%,4.5px 100%,2.75px calc(100% - 3.5px),0 calc(100% - 4.5px),0 3px,3px 4.4px,3px calc(100% - 6.6px),"
          "4.9px calc(100% - 5.9px),6.4px calc(100% - 3px),calc(100% - 4.7px) calc(100% - 3px),calc(100% - 3px) calc(100% - 4.7px),"
          "calc(100% - 3px) 5.6px,calc(100% - 5.4px) 3px,4.8px 3px,3px 4.4px)") &&
          c.clip_path.point_count == 20 && !c.clip_path.evenodd && c.clip_path.x[19].px == 3.0f);
    CHECK(apply(&c, NULL, "clip-path", "polygon(evenodd, 0 0, 10px 0, 0 10px)") && c.clip_path.point_count == 3 && c.clip_path.evenodd);
    CHECK(apply(&c, NULL, "clip-path", "none") && c.clip_path.kind == CSS_CLIP_PATH_NONE);
}

static void test_animation(void)
{
    CssComputed c = fresh();
    CHECK(apply(&c, NULL, "animation", "grow 0.8s infinite alternate") && c.animation_count == 1 &&
          strcmp(c.animations[0].name, "grow") == 0 && feq(c.animations[0].duration_s, 0.8f) &&
          isinf(c.animations[0].iterations) && c.animations[0].direction == CSS_ANIM_DIRECTION_ALTERNATE &&
          c.animations[0].timing.kind == CSS_TIMING_EASE);
    CHECK(apply(&c, NULL, "animation", "typewriter 4s steps(44) 1s 1 normal both, blinkTextCursor 500ms steps(44) infinite normal") &&
          c.animation_count == 2 && c.animations[0].timing.kind == CSS_TIMING_STEPS && c.animations[0].timing.steps == 44 &&
          feq(c.animations[0].delay_s, 1) && c.animations[0].fill == CSS_ANIM_FILL_BOTH && feq(c.animations[1].duration_s, 0.5f) &&
          strcmp(c.animations[1].name, "blinkTextCursor") == 0);
    CHECK(apply(&c, NULL, "animation", "lds-ring 1.2s cubic-bezier(0.5, 0, 0.5, 1) infinite") &&
          c.animations[0].timing.kind == CSS_TIMING_CUBIC_BEZIER && feq(c.animations[0].timing.x1, 0.5f) && feq(c.animations[0].timing.y2, 1));
    CHECK(apply(&c, NULL, "-webkit-animation", "rgbHue 0.8s steps(36) infinite") && c.animations[0].timing.steps == 36 &&
          c.animations[0].timing.step_pos == CSS_STEP_END);
    CHECK(apply(&c, NULL, "animation-timing-function", "steps(4, start)") && c.animations[0].timing.step_pos == CSS_STEP_START);
    CHECK(apply(&c, NULL, "animation-delay", "-0.45s") && feq(c.animations[0].delay_s, -0.45f));
    CHECK(apply(&c, NULL, "animation-duration", "2000ms") && feq(c.animations[0].duration_s, 2));
    CHECK(apply(&c, NULL, "animation-name", "spin") && strcmp(c.animations[0].name, "spin") == 0);
    CHECK(apply(&c, NULL, "animation", "none") && c.animation_count == 1 && c.animations[0].name == NULL);
    CHECK(apply(&c, NULL, "animation", "shake 0.5s") && feq(c.animations[0].iterations, 1));
    CHECK(!apply(&c, NULL, "animation", "a 1s 2s 3s"));
    CHECK(!apply(&c, NULL, "animation-timing-function", "steps(0)"));
    CHECK(!apply(&c, NULL, "animation-timing-function", "cubic-bezier(2,0,0,1)"));
    CHECK(!apply(&c, NULL, "animation-duration", "-1s"));
    /* <time> math (v10's ranked map carousel: calc(var(--total-maps) * 5s), var() substituted first) */
    CHECK(apply(&c, NULL, "animation", "scroll calc(5 * 5s) linear infinite") && c.animation_count == 1 &&
          feq(c.animations[0].duration_s, 25) && isinf(c.animations[0].iterations) && c.animations[0].timing.kind == CSS_TIMING_LINEAR);
    CHECK(apply(&c, NULL, "animation-duration", "calc(1s + 500ms)") && feq(c.animations[0].duration_s, 1.5f));
    CHECK(apply(&c, NULL, "animation-duration", "max(2s, 300ms)") && feq(c.animations[0].duration_s, 2));
    CHECK(apply(&c, NULL, "transition", "opacity calc(0.1s * 2) ease calc(50ms)") && feq(c.transitions[0].duration_s, 0.2f) &&
          feq(c.transitions[0].delay_s, 0.05f));
    CHECK(!apply(&c, NULL, "animation-duration", "calc(5)"));
    CHECK(!apply(&c, NULL, "animation-duration", "calc(1s + 5px)"));

    CHECK(apply(&c, NULL, "transition", "all 0.15s ease-in-out") && c.transition_count == 1 && c.transitions[0].prop == CSS_PROP_ALL &&
          feq(c.transitions[0].duration_s, 0.15f) && c.transitions[0].timing.kind == CSS_TIMING_EASE_IN_OUT);
    CHECK(apply(&c, NULL, "transition", "0.2s opacity") && c.transitions[0].prop == CSS_PROP_OPACITY && feq(c.transitions[0].duration_s, 0.2f));
    CHECK(apply(&c, NULL, "transition", "0.4s") && c.transitions[0].prop == CSS_PROP_ALL);
    CHECK(apply(&c, NULL, "-webkit-transition", "-moz-transform 0.1s ease-out") && c.transitions[0].prop == -2); /* unknown ident */
    CHECK(apply(&c, NULL, "-webkit-transition", "-webkit-transform 0.1s ease-out") && c.transitions[0].prop == CSS_PROP_TRANSFORM);
    CHECK(apply(&c, NULL, "transition", "width 1s ease-in-out, opacity 0.5s linear") && c.transition_count == 2 &&
          c.transitions[0].prop == CSS_PROP_WIDTH && c.transitions[1].prop == CSS_PROP_OPACITY && c.transitions[1].timing.kind == CSS_TIMING_LINEAR);
    CHECK(apply(&c, NULL, "transition", "font-size 0.2s") && c.transitions[0].prop == CSS_PROP_FONT_SIZE);
    CHECK(!apply(&c, NULL, "transition", "all 1s 2s 3s"));
}

/* font shorthand (dat-gui.css: `font: 11px 'Lucida Grande', sans-serif`). */
static void test_font_shorthand(void)
{
    CssComputed c = fresh();
    CHECK(apply(&c, NULL, "font-weight", "700") && apply(&c, NULL, "line-height", "30px"));
    CHECK(apply(&c, NULL, "font", "11px 'Lucida Grande', sans-serif") && feq(c.font_size, 11) && c.font_weight == 400 &&
          c.font_style == CSS_FONT_STYLE_NORMAL && c.line_height.kind == CSS_LEN_NORMAL && c.font_family_count == 2 &&
          !strcmp(c.font_families[0], "lucida grande") && !strcmp(c.font_families[1], "sans-serif"));
    CHECK(apply(&c, NULL, "font", "italic bold 12px/30px Georgia, serif") && c.font_style == CSS_FONT_STYLE_ITALIC &&
          c.font_weight == 700 && feq(c.font_size, 12) && len_is(c.line_height, 30, 0) && !strcmp(c.font_families[0], "georgia"));
    CHECK(apply(&c, NULL, "font", "normal small-caps 600 condensed 2em/1.5 Lucida Grande") && c.font_weight == 600 &&
          feq(c.font_size, 32) && c.line_height.kind == CSS_LEN_NUMBER && feq(c.line_height.px, 1.5f) &&
          !strcmp(c.font_families[0], "lucida grande"));
    CHECK(apply(&c, NULL, "font", "13px Monaco, monospace") && feq(c.font_size, 13) && c.font_family_count == 2);
    CHECK(apply(&c, NULL, "font", "larger serif") && feq(c.font_size, 16 * 1.2f));
    CHECK(!apply(&c, NULL, "font", "12px"));            /* family required */
    CHECK(!apply(&c, NULL, "font", "bold Arial"));      /* size required */
    CHECK(!apply(&c, NULL, "font", "12px/ Arial"));
    CHECK(!apply(&c, NULL, "font", "menu"));            /* system fonts unsupported */
    CHECK(apply(&c, NULL, "font", "inherit"));
}

/* CSS zoom: own value parsing and css_computed_zoom() scaling. */
static void test_zoom(void)
{
    CssComputed c = fresh();
    CHECK(feq(c.zoom, 1));
    CHECK(apply(&c, NULL, "zoom", "0.7") && feq(c.zoom, 0.7f));
    CHECK(apply(&c, NULL, "zoom", "150%") && feq(c.zoom, 1.5f));
    CHECK(apply(&c, NULL, "zoom", "normal") && feq(c.zoom, 1));
    CHECK(apply(&c, NULL, "zoom", "0") && feq(c.zoom, 1));
    CHECK(!apply(&c, NULL, "zoom", "-1"));
    CHECK(apply(&c, NULL, "box-shadow", "0 .15em 1.5em 0 rgba(0,0,0,.1), 0 0 1em 0 rgba(0,0,0,.03)") &&
          c.box_shadow_count == 2 && feq(c.box_shadow.y, 2.4f) && feq(c.box_shadow.blur, 24) &&     /* em folded at 16px */
          feq(c.box_shadow_more[0].blur, 16) && c.box_shadow_more[0].color.a == 8);                  /* the second shadow */
    CHECK(apply(&c, NULL, "box-shadow", "1px 1px red, 2px 2px blue, 3px 3px green, 4px 4px #000, 5px 5px #fff") &&
          c.box_shadow_count == 4 && feq(c.box_shadow_more[2].x, 4));                                /* 4 kept */
    CHECK(!apply(&c, NULL, "zoom", "10px"));
    c = fresh();
    CHECK(apply(&c, NULL, "width", "calc(50% - 10px)") && apply(&c, NULL, "margin-left", "8px") &&
          apply(&c, NULL, "border-top-width", "3px") && apply(&c, NULL, "line-height", "1.5") &&
          apply(&c, NULL, "letter-spacing", "2px") && apply(&c, NULL, "box-shadow", "4px 2px 6px 1px #000") &&
          apply(&c, NULL, "transform", "translateX(30px) scale(2)") && apply(&c, NULL, "text-shadow", "1px 2px 3px red") &&
          apply(&c, NULL, "grid-template-columns", "fit-content(40px) 10%"));
    css_computed_zoom(&c, 0.5f, 0);
    CHECK(feq(c.font_size, 8));
    CHECK(len_is(c.width, -5, 50));                        /* px scales, % does not */
    CHECK(len_is(c.margin[CSS_LEFT], 4, 0));
    CHECK(feq(c.border_width[CSS_TOP], 1.5f));
    CHECK(c.line_height.kind == CSS_LEN_NUMBER && feq(c.line_height.px, 1.5f));   /* numbers untouched */
    CHECK(len_is(c.letter_spacing, 1, 0));
    CHECK(feq(c.box_shadow.x, 2) && feq(c.box_shadow.y, 1) && feq(c.box_shadow.blur, 3) && feq(c.box_shadow.spread, 0.5f));
    CHECK(len_is(c.transform.ops[0].tx, 15, 0) && feq(c.transform.ops[1].sx, 2));
    CHECK(feq(c.text_shadow.items[0].x, 0.5f) && feq(c.text_shadow.items[0].blur, 1.5f));
    CHECK(len_is(c.grid_columns[0].size, 20, 0) && len_is(c.grid_columns[1].size, 0, 10));
    CssComputed d = fresh();
    CHECK(apply(&d, NULL, "width", "100px"));
    css_computed_zoom(&d, 2.0f, 1);                        /* inherited range only */
    CHECK(feq(d.font_size, 32) && len_is(d.width, 100, 0));
    CHECK(feq(css_snap_border_width(0.7f), 1) && feq(css_snap_border_width(2.1f), 2) &&
          feq(css_snap_border_width(1.5f), 1) && feq(css_snap_border_width(0), 0) && feq(css_snap_border_width(3), 3));
}

static void test_layout_props(void)
{
    CssComputed c = fresh();
    CHECK(apply(&c, NULL, "grid-template-columns", "40px 50px auto 50px 40px") && c.grid_column_count == 5 &&
          c.grid_columns[2].size.kind == CSS_LEN_AUTO && len_is(c.grid_columns[4].size, 40, 0));
    CHECK(apply(&c, NULL, "grid-template-columns", "calc(100%/4) calc(100%/4)") && c.grid_column_count == 2 && len_is(c.grid_columns[1].size, 0, 25));
    CHECK(apply(&c, NULL, "grid-template-columns", "calc(20% - 5px) calc(80% - 5px)") && len_is(c.grid_columns[1].size, -5, 80));
    CHECK(apply(&c, NULL, "grid-template-columns", "auto auto auto auto auto auto auto auto") && c.grid_column_count == 8);
    CHECK(apply(&c, NULL, "grid-template-columns", "calc((100% / 3) - 2px) calc((100% / 3) - 2px) calc((100% / 3) - 2px)") &&
          c.grid_column_count == 3 && len_is(c.grid_columns[2].size, -2, 100.0f / 3) && c.grid_columns[2].kind == CSS_TRACK_SIZE);
    CHECK(apply(&c, NULL, "grid-template-columns", "fit-content(0) fit-content(100%) auto") && c.grid_column_count == 3 &&
          c.grid_columns[0].kind == CSS_TRACK_FIT_CONTENT && len_is(c.grid_columns[0].size, 0, 0) &&
          c.grid_columns[1].kind == CSS_TRACK_FIT_CONTENT && len_is(c.grid_columns[1].size, 0, 100) &&
          c.grid_columns[2].kind == CSS_TRACK_SIZE && c.grid_columns[2].size.kind == CSS_LEN_AUTO);
    CHECK(apply(&c, NULL, "grid-template-columns", "1fr 4fr 0fr") && c.grid_columns[1].kind == CSS_TRACK_FR &&
          feq(c.grid_columns[1].fr, 4) && c.grid_columns[2].kind == CSS_TRACK_FR && feq(c.grid_columns[2].fr, 0));
    CHECK(apply(&c, NULL, "grid-template-columns", "minmax(0, 1fr) minmax(10px, 20%) minmax(min-content, auto)") &&
          c.grid_columns[0].kind == CSS_TRACK_MINMAX_FR && len_is(c.grid_columns[0].size, 0, 0) && feq(c.grid_columns[0].fr, 1) &&
          c.grid_columns[1].kind == CSS_TRACK_MINMAX && len_is(c.grid_columns[1].size, 10, 0) && len_is(c.grid_columns[1].max, 0, 20) &&
          c.grid_columns[2].size.kind == CSS_LEN_MIN_CONTENT && c.grid_columns[2].max.kind == CSS_LEN_AUTO);
    CHECK(apply(&c, NULL, "grid-template-columns", "repeat(3, minmax(0, 1fr)) 40px") && c.grid_column_count == 4 &&
          c.grid_columns[2].kind == CSS_TRACK_MINMAX_FR && len_is(c.grid_columns[3].size, 40, 0));
    CHECK(apply(&c, NULL, "grid-template-columns", "repeat(2, 10px 1fr)") && c.grid_column_count == 4 && c.grid_columns[3].kind == CSS_TRACK_FR);
    CHECK(!apply(&c, NULL, "grid-template-columns", "fit-content(auto)"));
    CHECK(!apply(&c, NULL, "grid-template-columns", "minmax(1fr, 10px)"));
    CHECK(!apply(&c, NULL, "grid-template-columns", "repeat(0, 10px)"));
    CHECK(!apply(&c, NULL, "grid-template-columns", "-1fr"));
    CHECK(apply(&c, NULL, "grid-template-rows", "5fr auto auto") && c.grid_row_count == 3 && c.grid_rows[0].kind == CSS_TRACK_FR &&
          feq(c.grid_rows[0].fr, 5) && c.grid_rows[2].size.kind == CSS_LEN_AUTO);
    {
        const CssGridLine *gl = c.grid_lines;
        CHECK(apply(&c, NULL, "grid-area", "2/1/4/1") && gl[CSS_GRID_ROW_START].line == 2 && gl[CSS_GRID_COLUMN_START].line == 1 &&
              gl[CSS_GRID_ROW_END].line == 4 && gl[CSS_GRID_COLUMN_END].line == 1 && !gl[CSS_GRID_ROW_END].span);
        CHECK(apply(&c, NULL, "grid-area", "3 / 2") && gl[CSS_GRID_ROW_START].line == 3 && gl[CSS_GRID_COLUMN_START].line == 2 &&
              gl[CSS_GRID_ROW_END].line == 0 && gl[CSS_GRID_COLUMN_END].line == 0);   /* omitted parts: auto */
        CHECK(apply(&c, NULL, "grid-column", "2 / span 2") && gl[CSS_GRID_COLUMN_START].line == 2 && gl[CSS_GRID_COLUMN_END].span &&
              gl[CSS_GRID_COLUMN_END].line == 2);
        CHECK(apply(&c, NULL, "grid-row", "span 3") && gl[CSS_GRID_ROW_START].span && gl[CSS_GRID_ROW_START].line == 3 && gl[CSS_GRID_ROW_END].line == 0);
        CHECK(apply(&c, NULL, "grid-column-end", "-1") && gl[CSS_GRID_COLUMN_END].line == -1 && !gl[CSS_GRID_COLUMN_END].span);
        CHECK(apply(&c, NULL, "grid-row-start", "2 span") && gl[CSS_GRID_ROW_START].span && gl[CSS_GRID_ROW_START].line == 2);
        CHECK(!apply(&c, NULL, "grid-row-start", "0"));
        CHECK(!apply(&c, NULL, "grid-row-start", "span -1"));
        CHECK(!apply(&c, NULL, "grid-area", "1/2/3/4/5"));
        CHECK(apply(&c, NULL, "grid-column", "a / b") && gl[CSS_GRID_COLUMN_START].name == kn_atom_cstr("a") &&
              gl[CSS_GRID_COLUMN_END].name == kn_atom_cstr("b") && gl[CSS_GRID_COLUMN_END].line == 0);
        CHECK(apply(&c, NULL, "grid-area", "hd") && gl[CSS_GRID_ROW_START].name == kn_atom_cstr("hd") &&
              gl[CSS_GRID_COLUMN_START].name == gl[CSS_GRID_ROW_START].name && gl[CSS_GRID_ROW_END].name == gl[CSS_GRID_ROW_START].name &&
              gl[CSS_GRID_COLUMN_END].name == gl[CSS_GRID_ROW_START].name);   /* omitted idents copy */
        CHECK(apply(&c, NULL, "grid-area", "2 / x") && gl[CSS_GRID_ROW_END].line == 0 && !gl[CSS_GRID_ROW_END].name &&
              gl[CSS_GRID_COLUMN_END].name == kn_atom_cstr("x"));
        CHECK(apply(&c, NULL, "grid-row", "2 a / span b") && gl[CSS_GRID_ROW_START].line == 2 && gl[CSS_GRID_ROW_START].name == kn_atom_cstr("a") &&
              gl[CSS_GRID_ROW_END].span && gl[CSS_GRID_ROW_END].line == 1 && gl[CSS_GRID_ROW_END].name == kn_atom_cstr("b"));
        CHECK(!apply(&c, NULL, "grid-row", "span"));
        CHECK(!apply(&c, NULL, "grid-row", "a span 2 b"));
        CHECK(!apply(&c, NULL, "grid-row", "2 span auto"));
    }
    {
        const CssGridNames *cn = &c.grid_names[CSS_GRID_AXIS_COLUMN];
        CHECK(apply(&c, NULL, "grid-template-columns", "[a] 10px [b c] 1fr [d]") && c.grid_column_count == 2 &&
              cn->line[0] == kn_atom_cstr("a") && cn->line[1] == kn_atom_cstr("b c") && cn->line[2] == kn_atom_cstr("d") && !cn->repeat);
        CHECK(apply(&c, NULL, "grid-template-columns", "repeat(2, [x] 5px [y])") && c.grid_column_count == 2 &&
              cn->line[0] == kn_atom_cstr("x") && cn->line[1] == kn_atom_cstr("y x") && cn->line[2] == kn_atom_cstr("y"));
        CHECK(apply(&c, NULL, "grid-template-columns", "repeat(auto-fit, 1.75em)") && c.grid_column_count == 1 &&
              cn->repeat == CSS_GRID_REPEAT_AUTO_FIT && cn->repeat_at == 0 && cn->repeat_n == 1 && len_is(c.grid_columns[0].size, 28, 0));
        CHECK(apply(&c, NULL, "grid-template-columns", "[s] 20px repeat(auto-fill, [a] minmax(50px, 1fr) [b]) [e] 30px") &&
              c.grid_column_count == 3 && cn->repeat == CSS_GRID_REPEAT_AUTO_FILL && cn->repeat_at == 1 && cn->repeat_n == 1 &&
              cn->rep_first == kn_atom_cstr("a") && cn->rep_last == kn_atom_cstr("b") && cn->line[1] == NULL && cn->line[2] == kn_atom_cstr("e"));
        CHECK(!apply(&c, NULL, "grid-template-columns", "repeat(auto-fill, 1fr)"));          /* not a <fixed-size> */
        CHECK(!apply(&c, NULL, "grid-template-columns", "repeat(auto-fill, 10px) auto"));    /* <auto-track-list>: fixed sizes only */
        CHECK(!apply(&c, NULL, "grid-template-columns", "repeat(auto-fill, 10px) repeat(auto-fit, 10px)"));
        CHECK(!apply(&c, NULL, "grid-template-columns", "[span] 10px"));
        CHECK(apply(&c, NULL, "grid-auto-rows", "10px minmax(20px, auto)") && c.grid_auto_count[CSS_GRID_AXIS_ROW] == 2 &&
              c.grid_auto_tracks[CSS_GRID_AXIS_ROW][1].kind == CSS_TRACK_MINMAX);
        CHECK(apply(&c, NULL, "grid-auto-flow", "dense column") && c.grid_auto_flow == (CSS_GRID_FLOW_COLUMN | CSS_GRID_FLOW_DENSE));
        CHECK(!apply(&c, NULL, "grid-auto-flow", "row column"));
        CHECK(apply(&c, NULL, "grid-template-areas", "'a a .' \"b  c c\"") && c.grid_area_rows == 2 && c.grid_area_cols == 3 &&
              c.grid_areas == kn_atom_cstr("a a .\nb c c"));
        CHECK(apply(&c, NULL, "grid-template-areas", "'a ... b'") && c.grid_areas == kn_atom_cstr("a . b"));
        CHECK(!apply(&c, NULL, "grid-template-areas", "'a b a'"));                   /* not a rectangle */
        CHECK(!apply(&c, NULL, "grid-template-areas", "'a b' 'c'"));                 /* ragged */
        CHECK(!apply(&c, NULL, "grid-template-areas", "'a $'"));                     /* trash token */
        CHECK(apply(&c, NULL, "grid-template", "[t] 'h h' 20px [m] 'n m' / 30px 1fr") && c.grid_row_count == 2 &&
              len_is(c.grid_rows[0].size, 20, 0) && c.grid_rows[1].size.kind == CSS_LEN_AUTO && c.grid_column_count == 2 &&
              c.grid_area_rows == 2 && c.grid_names[CSS_GRID_AXIS_ROW].line[0] == kn_atom_cstr("t") &&
              c.grid_names[CSS_GRID_AXIS_ROW].line[1] == kn_atom_cstr("m"));
        CHECK(apply(&c, NULL, "grid-template", "10px 20px / 30px") && c.grid_row_count == 2 && c.grid_column_count == 1 && !c.grid_areas);
        CHECK(apply(&c, NULL, "grid", "auto-flow dense 15px / 40px 40px") && c.grid_auto_flow == CSS_GRID_FLOW_DENSE &&
              c.grid_row_count == 0 && c.grid_column_count == 2 && len_is(c.grid_auto_tracks[CSS_GRID_AXIS_ROW][0].size, 15, 0));
        CHECK(apply(&c, NULL, "grid", "50px / auto-flow 25px") && c.grid_auto_flow == CSS_GRID_FLOW_COLUMN && c.grid_row_count == 1 &&
              len_is(c.grid_auto_tracks[CSS_GRID_AXIS_COLUMN][0].size, 25, 0) && c.grid_auto_tracks[CSS_GRID_AXIS_ROW][0].size.kind == CSS_LEN_AUTO);
        CHECK(apply(&c, NULL, "grid", "'a' / 10px") && c.grid_area_rows == 1 && c.grid_auto_flow == 0);
        CHECK(!apply(&c, NULL, "grid", "auto-flow / auto-flow"));
    }
    CHECK(c.align_self == CSS_ALIGN_ITEMS_AUTO && c.justify_self == CSS_ALIGN_ITEMS_AUTO && c.justify_items == CSS_ALIGN_ITEMS_STRETCH);
    CHECK(apply(&c, NULL, "justify-self", "center") && c.justify_self == CSS_ALIGN_ITEMS_CENTER);
    CHECK(apply(&c, NULL, "align-self", "flex-start") && c.align_self == CSS_ALIGN_ITEMS_FLEX_START);
    CHECK(apply(&c, NULL, "place-self", "end center") && c.align_self == CSS_ALIGN_ITEMS_FLEX_END && c.justify_self == CSS_ALIGN_ITEMS_CENTER);
    CHECK(apply(&c, NULL, "place-self", "auto") && c.align_self == CSS_ALIGN_ITEMS_AUTO && c.justify_self == CSS_ALIGN_ITEMS_AUTO);
    CHECK(apply(&c, NULL, "justify-items", "legacy") && c.justify_items == CSS_ALIGN_ITEMS_STRETCH);
    CHECK(!apply(&c, NULL, "justify-items", "auto"));
    CHECK(apply(&c, NULL, "row-gap", "10px") && len_is(c.row_gap, 10, 0));
    CHECK(apply(&c, NULL, "column-gap", "10px") && len_is(c.column_gap, 10, 0));
    CHECK(apply(&c, NULL, "border-radius", "0 0 6px 0") && len_is(c.border_radius[CSS_BOTTOM_RIGHT], 6, 0) && len_is(c.border_radius[CSS_TOP_LEFT], 0, 0));
    CHECK(apply(&c, NULL, "border-radius", "50%") && len_is(c.border_radius[CSS_BOTTOM_LEFT], 0, 50));
    CHECK(apply(&c, NULL, "border-radius", "4px 8px") && len_is(c.border_radius[CSS_TOP_RIGHT], 8, 0) && len_is(c.border_radius[CSS_BOTTOM_RIGHT], 4, 0));
    CHECK(apply(&c, NULL, "border-radius", "unset") && len_is(c.border_radius[CSS_TOP_LEFT], 0, 0));
    CHECK(apply(&c, NULL, "border", "6px dotted #ccc") && c.border_style[CSS_TOP] == CSS_BORDER_STYLE_DOTTED && feq(c.border_width[CSS_RIGHT], 6));
    CHECK(apply(&c, NULL, "border-bottom", "10px solid transparent") && color_is(c.border_color[CSS_BOTTOM], 0, 0, 0, 0));
    CHECK(apply(&c, NULL, "border", "none") && c.border_style[CSS_LEFT] == CSS_BORDER_STYLE_NONE && feq(c.border_width[CSS_LEFT], 3));
    CHECK(apply(&c, NULL, "border", "0") && feq(c.border_width[CSS_TOP], 0));
    CHECK(apply(&c, NULL, "border-color", "#fff transparent transparent transparent") && color_is(c.border_color[CSS_TOP], 255, 255, 255, 255) &&
          color_is(c.border_color[CSS_LEFT], 0, 0, 0, 0));
    CHECK(apply(&c, NULL, "border-width", "0px") && feq(c.border_width[CSS_BOTTOM], 0));
    CHECK(!apply(&c, NULL, "border", "solid solid"));
    CHECK(apply(&c, NULL, "margin", "0 auto 10px") && c.margin[CSS_RIGHT].kind == CSS_LEN_AUTO && c.margin[CSS_LEFT].kind == CSS_LEN_AUTO &&
          len_is(c.margin[CSS_BOTTOM], 10, 0));
    CHECK(apply(&c, NULL, "padding", "6px 10px 5px") && len_is(c.padding[CSS_LEFT], 10, 0) && len_is(c.padding[CSS_BOTTOM], 5, 0));
    CHECK(!apply(&c, NULL, "margin", "1px 2px 3px 4px 5px"));
    CHECK(!apply(&c, NULL, "padding", "-1px"));
    CHECK(apply(&c, NULL, "z-index", "99999999999999999999") && c.z_index == 2147483647 && !c.z_index_auto);
    CHECK(apply(&c, NULL, "z-index", "-1") && c.z_index == -1);
    CHECK(apply(&c, NULL, "z-index", "auto") && c.z_index_auto);
    CHECK(!apply(&c, NULL, "z-index", "1.5"));
    CHECK(apply(&c, NULL, "opacity", "0.7") && feq(c.opacity, 0.7f));
    CHECK(apply(&c, NULL, "opacity", "2") && feq(c.opacity, 1));
    CHECK(apply(&c, NULL, "display", "table-cell") && c.display == CSS_DISPLAY_TABLE_CELL);
    CHECK(apply(&c, NULL, "display", "inline-flex") && c.display == CSS_DISPLAY_INLINE_FLEX);
    CHECK(!apply(&c, NULL, "position", "-webkit-sticky") && apply(&c, NULL, "position", "sticky") && c.position == CSS_POSITION_STICKY);
    CHECK(apply(&c, NULL, "overflow", "hidden") && c.overflow_x == CSS_OVERFLOW_HIDDEN && c.overflow_y == CSS_OVERFLOW_HIDDEN);
    CHECK(apply(&c, NULL, "overflow-y", "scroll") && c.overflow_y == CSS_OVERFLOW_SCROLL);
    CHECK(!apply(&c, NULL, "image-rendering", "-moz-crisp-edges") && !apply(&c, NULL, "image-rendering", "-webkit-crisp-edges"));
    CHECK(apply(&c, NULL, "image-rendering", "-webkit-optimize-contrast") && c.image_rendering == CSS_IMAGE_RENDERING_CRISP_EDGES);
    CHECK(apply(&c, NULL, "image-rendering", "pixelated") && c.image_rendering == CSS_IMAGE_RENDERING_PIXELATED);
    CHECK(!apply(&c, NULL, "-webkit-box-flex", "1"));
    CHECK(apply(&c, NULL, "flex-basis", "200px") && len_is(c.flex_basis, 200, 0));
    CHECK(apply(&c, NULL, "flex", "1") && feq(c.flex_grow, 1) && len_is(c.flex_basis, 0, 0));
    CHECK(apply(&c, NULL, "flex-flow", "column wrap") && c.flex_direction == CSS_FLEX_DIRECTION_COLUMN && c.flex_wrap == CSS_FLEX_WRAP_WRAP);
    CHECK(apply(&c, NULL, "justify-content", "space-around") && c.justify_content == CSS_JUSTIFY_CONTENT_SPACE_AROUND);
    CHECK(apply(&c, NULL, "align-items", "flex-end") && c.align_items == CSS_ALIGN_ITEMS_FLEX_END);
    CHECK(apply(&c, NULL, "vertical-align", "text-top") && c.vertical_align == CSS_VERTICAL_ALIGN_TEXT_TOP);
    CHECK(apply(&c, NULL, "vertical-align", "-2px") && c.vertical_align == CSS_VERTICAL_ALIGN_LENGTH && len_is(c.vertical_align_length, -2, 0));
    CHECK(apply(&c, NULL, "inset", "0") && len_is(c.inset[CSS_BOTTOM], 0, 0));
    CHECK(apply(&c, NULL, "gap", "10px 5px") && len_is(c.row_gap, 10, 0) && len_is(c.column_gap, 5, 0));
    CHECK(apply(&c, NULL, "border-spacing", "10px") && len_is(c.border_spacing_y, 10, 0));
}

static void test_text_props(void)
{
    CssComputed c = fresh();
    CHECK(apply(&c, NULL, "font-family", "material icons") && c.font_family_count == 1 && strcmp(c.font_families[0], "material icons") == 0);
    CHECK(apply(&c, NULL, "font-family", "'Secular One', gamefont, sans-serif") && c.font_family_count == 3 &&
          strcmp(c.font_families[0], "secular one") == 0 && strcmp(c.font_families[2], "sans-serif") == 0);
    CHECK(c.font_families[1] == kn_atom_cstr("gamefont"));
    CHECK(!apply(&c, NULL, "font-family", "a,,b"));
    CHECK(apply(&c, NULL, "font-weight", "999") && c.font_weight == 999);
    CHECK(apply(&c, NULL, "font-weight", "bold") && c.font_weight == 700);
    CHECK(!apply(&c, NULL, "font-weight", "1001"));
    CHECK(apply(&c, NULL, "font-style", "italic") && c.font_style == CSS_FONT_STYLE_ITALIC);
    CHECK(apply(&c, NULL, "content", "\"\"") && c.content.kind == CSS_CONTENT_STRING && c.content.str[0] == 0);
    CHECK(apply(&c, NULL, "content", "attr(data-badge)") && c.content.kind == CSS_CONTENT_ATTR && strcmp(c.content.str, "data-badge") == 0);
    CHECK(apply(&c, NULL, "content", "none") && c.content.kind == CSS_CONTENT_NONE);
    CHECK(apply(&c, NULL, "content", "'x'") && strcmp(c.content.str, "x") == 0);
    CHECK(apply(&c, NULL, "cursor", "pointer") && c.cursor == CSS_CURSOR_POINTER);
    CHECK(apply(&c, NULL, "cursor", "crosshair") && c.cursor == CSS_CURSOR_CROSSHAIR);
    CHECK(apply(&c, NULL, "cursor", "url(a.cur), default") && c.cursor == CSS_CURSOR_DEFAULT);
    CHECK(apply(&c, NULL, "text-decoration", "underline") && c.text_decoration == CSS_TEXT_DECORATION_UNDERLINE);
    CHECK(apply(&c, NULL, "text-decoration", "none") && c.text_decoration == 0);
    CHECK(apply(&c, NULL, "text-overflow", "ellipsis") && c.text_overflow == CSS_TEXT_OVERFLOW_ELLIPSIS);
    CHECK(apply(&c, NULL, "white-space", "nowrap") && c.white_space == CSS_WHITE_SPACE_NOWRAP);
    /* text-wrap (v10's .lockedClassText in a nowrap .classCard): auto wraps again, nowrap stops it */
    CHECK(apply(&c, NULL, "text-wrap", "auto") && c.white_space == CSS_WHITE_SPACE_NORMAL);
    CHECK(apply(&c, NULL, "white-space", "nowrap") && apply(&c, NULL, "text-wrap", "wrap") && c.white_space == CSS_WHITE_SPACE_NORMAL);
    CHECK(apply(&c, NULL, "text-wrap", "nowrap") && c.white_space == CSS_WHITE_SPACE_NOWRAP);
    CHECK(!apply(&c, NULL, "text-wrap", "sideways"));
    CHECK(apply(&c, NULL, "word-wrap", "break-word") && c.overflow_wrap == CSS_OVERFLOW_WRAP_BREAK_WORD);
    CHECK(apply(&c, NULL, "word-break", "break-all") && c.word_break == CSS_WORD_BREAK_BREAK_ALL);
    CHECK(apply(&c, NULL, "text-transform", "uppercase") && c.text_transform == CSS_TEXT_TRANSFORM_UPPERCASE);
    CHECK(apply(&c, NULL, "direction", "rtl") && c.direction == CSS_DIRECTION_RTL);
    CHECK(apply(&c, NULL, "-webkit-user-select", "text") && c.user_select == CSS_USER_SELECT_TEXT);
    CHECK(apply(&c, NULL, "-webkit-appearance", "textfield") && c.appearance == CSS_APPEARANCE_TEXTFIELD);
    CHECK(css_computed_initial()->appearance == CSS_APPEARANCE_NONE); /* initial: none (the UA sheet says auto for controls) */
    CHECK(!apply(&c, NULL, "-moz-appearance", "none"));
    CHECK(apply(&c, NULL, "pointer-events", "all") && c.pointer_events == CSS_POINTER_EVENTS_ALL);
    CHECK(apply(&c, NULL, "visibility", "hidden") && c.visibility == CSS_VISIBILITY_HIDDEN);
    CHECK(apply(&c, NULL, "will-change", "transform, opacity") && c.will_change == (CSS_WILL_CHANGE_TRANSFORM | CSS_WILL_CHANGE_OPACITY));
    CHECK(apply(&c, NULL, "mix-blend-mode", "overlay") && c.mix_blend_mode == CSS_BLEND_OVERLAY);
    CHECK(apply(&c, NULL, "mix-blend-mode", "color") && c.mix_blend_mode == CSS_BLEND_COLOR);
    CHECK(apply(&c, NULL, "contain", "content") && c.contain == (CSS_CONTAIN_LAYOUT | CSS_CONTAIN_PAINT | CSS_CONTAIN_STYLE));
    CHECK(apply(&c, NULL, "contain", "paint layout") && c.contain == (CSS_CONTAIN_LAYOUT | CSS_CONTAIN_PAINT));
    CHECK(apply(&c, NULL, "contain", "none") && c.contain == CSS_CONTAIN_NONE);
    CHECK(!apply(&c, NULL, "contain", "layout layout") && !apply(&c, NULL, "contain", "strict paint")
          && !apply(&c, NULL, "contain", "size inline-size"));   /* repeated / combined keywords are invalid */
    CHECK(apply(&c, NULL, "display", "flow-root") && c.display == CSS_DISPLAY_FLOW_ROOT);
    CHECK(apply(&c, NULL, "outline", "none") && c.outline_style == CSS_BORDER_STYLE_NONE);
    CHECK(apply(&c, NULL, "list-style", "none") && c.list_style_type == CSS_LIST_STYLE_NONE);
    CHECK(!apply(&c, NULL, "display", "blockish"));
    CHECK(!apply(&c, NULL, "position", "absolute relative"));
    /* `all`: a CSS-wide keyword for every longhand but direction (v10's .io-button { all: unset }) */
    {
        CssComputed p = fresh(), a = fresh();
        p.color.r = 200; p.direction = CSS_DIRECTION_LTR;
        a.box_sizing = CSS_BOX_SIZING_BORDER_BOX; a.display = CSS_DISPLAY_INLINE_BLOCK; a.direction = CSS_DIRECTION_RTL;
        CHECK(apply(&a, &p, "all", "unset") && a.box_sizing == CSS_BOX_SIZING_CONTENT_BOX && a.display == CSS_DISPLAY_INLINE
              && a.color.r == 200 && a.direction == CSS_DIRECTION_RTL);
        CHECK(apply(&a, &p, "ALL", " initial ") && a.color.r == 0);
        CHECK(!apply(&a, &p, "all", "none") && !apply(&a, &p, "all", "unset unset"));
    }
}

/* the block test needs a plain C callback */
typedef struct BlockRec { int n; int props[16]; int important[16]; } BlockRec;
static void block_cb(void *user, int prop, const CssToken *t, int n, int important)
{
    BlockRec *r = user;
    (void)t; (void)n;
    if (r->n < 16) { r->props[r->n] = prop; r->important[r->n] = important; }
    r->n++;
}

static void test_decl_block_real(void)
{
    BlockRec r;
    memset(&r, 0, sizeof(r));
    const char *css = "color: red !important; ; width:calc(100% - 2px);garbage; font-family: 'a;b', x; margin:0 auto; bad: ; "
                      "/* c: d; */ opacity: .5 ! IMPORTANT";
    int got = css_parse_declaration_block(css, strlen(css), block_cb, &r);
    CHECK(got == 5);
    CHECK(r.n == 8); /* color, width, font-family, 4 margins, opacity */
    CHECK(r.props[0] == CSS_PROP_COLOR && r.important[0] == 1);
    CHECK(r.props[1] == CSS_PROP_WIDTH && r.important[1] == 0);
    CHECK(r.props[2] == CSS_PROP_FONT_FAMILY);
    CHECK(r.props[3] == CSS_PROP_MARGIN_TOP && r.props[6] == CSS_PROP_MARGIN_LEFT);
    CHECK(r.props[7] == CSS_PROP_OPACITY && r.important[7] == 1);
    /* shorthand with var() emits pending refs */
    memset(&r, 0, sizeof(r));
    const char *v = "border: 4px solid var(--blue)";
    CHECK(css_parse_declaration_block(v, strlen(v), block_cb, &r) == 1 && r.n == 12);
    /* garbage never crashes */
    const char *junk[] = { "", ";;;", ":", "a:", ":b", "{}", "color: (", "color: rgb(", "x: \"", "width: calc(((((", "!important",
                           "background: url(", "transform: translate(", "animation: , , ,", "--x:", "--y: {a:b}" };
    for (size_t i = 0; i < sizeof(junk) / sizeof(junk[0]); i++) {
        BlockRec rr;
        memset(&rr, 0, sizeof(rr));
        css_parse_declaration_block(junk[i], strlen(junk[i]), block_cb, &rr);
        CssComputed c = fresh();
        apply(&c, NULL, "width", junk[i]);
        apply(&c, NULL, "background", junk[i]);
        apply(&c, NULL, "animation", junk[i]);
    }
    g_pass++;
}

static void test_computed(void)
{
    CssComputed c = fresh();
    CHECK(c.display == CSS_DISPLAY_INLINE && feq(c.font_size, 16) && c.font_weight == 400 && feq(c.opacity, 1));
    CHECK(c.max_width.kind == CSS_LEN_NONE && c.width.kind == CSS_LEN_AUTO && c.z_index_auto);
    CHECK(feq(c.border_width[0], 3) && c.border_style[0] == CSS_BORDER_STYLE_NONE && c.border_color[0].kind == CSS_COLOR_CURRENT);
    CHECK(c.transition_count == 1 && c.transitions[0].prop == CSS_PROP_ALL && feq(c.transitions[0].duration_s, 0));
    CHECK(c.animation_count == 1 && c.animations[0].name == NULL);
    CHECK(feq(c.flex_shrink, 1) && c.flex_basis.kind == CSS_LEN_AUTO);
    css_computed_hash(&c);
    uint64_t hi = c.hash_inherited, hn = c.hash_non_inherited;
    CHECK(apply(&c, NULL, "width", "10px"));
    css_computed_hash(&c);
    CHECK(c.hash_inherited == hi && c.hash_non_inherited != hn);
    CHECK(apply(&c, NULL, "color", "red"));
    css_computed_hash(&c);
    CHECK(c.hash_inherited != hi);
    CssComputed child = fresh();
    css_computed_inherit(&child, &c);
    CHECK(color_is(child.color, 255, 0, 0, 255) && child.width.kind == CSS_LEN_AUTO);
    CHECK(apply(&child, &c, "width", "inherit") && len_is(child.width, 10, 0));
    CHECK(apply(&child, &c, "width", "unset") && child.width.kind == CSS_LEN_AUTO);
    /* failed apply leaves the record untouched */
    CssComputed snap = child;
    CHECK(!apply(&child, &c, "width", "banana") && memcmp(&snap, &child, sizeof(snap)) == 0);
}

/* ------------------------------------------------------------------------- */
/* coverage over shipped CSS + inline styles                                  */
/* ------------------------------------------------------------------------- */

typedef struct Coverage {
    int ok, fail, dropped, gaps;
} Coverage;

/* Declarations headless Chromium drops (checked with CSS.supports /
 * style.setProperty): non-webkit vendor properties, -webkit-box-flex (a
 * display:-webkit-box property, not flex-grow), and -moz-/-o- values.  The
 * engine must reject these too, or they override the real declaration. */
static int chrome_drops(const char *name, size_t nl, const char *val, size_t vl)
{
    static const char *pfx[] = { "-moz-", "-o-", "-ms-", "-khtml-" };
    for (size_t i = 0; i < sizeof(pfx) / sizeof(pfx[0]); i++) {
        size_t pl = strlen(pfx[i]);
        if (nl > pl && kn_str_ieq(name, pl, pfx[i])) return 1;
    }
    if (kn_str_ieq(name, nl, "-webkit-box-flex") || kn_str_ieq(name, nl, "font-smooth")) return 1;
    /* a property name with a space in it (social.css's "c olor") is no property at all */
    for (size_t i = 0; i < nl; i++) if (name[i] == ' ' || name[i] == '\t') return 1;
    /* a colour is no <image> (social.css: background-image: #1d2329) */
    {
        size_t tl = vl;
        const char *tv = kn_str_trim(val, &tl);
        if (kn_str_ieq(name, nl, "background-image") && tl && tv[0] == '#') return 1;
    }
    /* -moz-/-o- functions and keywords Blink rejects (an unknown ident such as
     * `transition: -moz-transform 1s` stays valid there) */
    static const char *bad[] = { "-moz-calc(", "-o-calc(", "-moz-linear-gradient(", "-o-linear-gradient(",
                                 "-moz-radial-gradient(", "-moz-crisp-edges", "-o-crisp-edges",
                                 "-webkit-crisp-edges", "-webkit-sticky", "-moz-available" };
    /* whole values Blink rejects (CSS.supports false, Chrome 154): v7.1.4 main.css */
    static const char *bad_values[] = { "optimizeSpeed", "fill-available" };
    {
        size_t tl = vl;
        const char *tv = kn_str_trim(val, &tl);
        for (size_t i = 0; i < sizeof(bad_values) / sizeof(bad_values[0]); i++)
            if (kn_str_ieq(tv, tl, bad_values[i])) return 1;
    }
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        size_t bl = strlen(bad[i]);
        for (size_t k = 0; k + bl <= vl; k++)
            if (kn_str_ieq(val + k, bl, bad[i])) return 1;
    }
    return 0;
}

/* Declarations Chrome accepts (CSS.supports true) that the engine does not implement yet:
 * known gaps of the page, counted apart from failures (a new unparsed declaration
 * still fails the test). None changes the menu's layout: they are visual extras
 * (backdrop blur, 3D, smooth scrolling, thin scrollbars, tap highlight, SVG fill, underline
 * colour, scrollbar gutter, textarea resize), the store's clip-path polygon
 * with calc(), image `content:` on elements, `align-content` of single-line flex
 * containers, and #menuWindow.forceFill's -webkit-fill-available (its height: 100%
 * fallback applies). Krunker v10.0.2's sheets add the gaps ui/README.md lists: aspect-ratio
 * (thumbnails size from their content), object-position, content-visibility /
 * contain-intrinsic-size (render hints), text-wrap, and color-mix() values (the tint drops). */
static int known_gap(const char *name, size_t nl, const char *val, size_t vl)
{
    static const char *props[] = { "backdrop-filter", "transform-style", "scroll-behavior", "scrollbar-width",
                                   "background-origin", "align-content", "-webkit-tap-highlight-color", "fill",
                                   "text-decoration-color", "scrollbar-gutter", "resize", "aspect-ratio",
                                   "object-position", "content-visibility", "contain-intrinsic-size",
                                   /* the v10 Svelte / hub / editor sheets: SVG presentation properties of
                                    * inline icons, @property descriptors, masks, form-control tints,
                                    * text stroke, line clamping and other visual extras */
                                   "stroke", "stroke-width", "stroke-dasharray", "stroke-dashoffset", "stroke-opacity",
                                   "stroke-linecap", "r", "cx", "cy", "accent-color", "font-variant-numeric",
                                   "color-scheme", "outline-offset", "syntax", "initial-value", "inherits",
                                   "-webkit-text-stroke-width", "-webkit-text-stroke-color", "-webkit-text-fill-color",
                                   "-webkit-line-clamp", "-webkit-box-orient", "-webkit-backdrop-filter", "scrollbar-color",
                                   "mask", "mask-size", "mask-repeat", "mask-position", "mask-mode", "mask-image",
                                   "-webkit-mask", "-webkit-mask-size", "-webkit-mask-repeat", "-webkit-mask-position",
                                   "-webkit-mask-mode", "-webkit-mask-image", "font-variant-ligatures",
                                   "-webkit-font-variant-ligatures", "-webkit-background-clip", "touch-action", "tab-size",
                                   "container-type", "caret-color", "border-image", "backface-visibility" };
    for (size_t i = 0; i < sizeof(props) / sizeof(props[0]); i++)
        if (kn_str_ieq(name, nl, props[i])) return 1;
    size_t tl = vl;
    const char *tv = kn_str_trim(val, &tl);
    if (kn_str_ieq(name, nl, "height") && kn_str_ieq(tv, tl, "-webkit-fill-available")) return 1;
    if (kn_str_ieq(name, nl, "content") && tl > 4 && kn_str_ieq(tv, 4, "url(")) return 1;
    for (size_t k = 0; k + 10 <= tl; k++) if (kn_str_ieq(tv + k, 10, "color-mix(")) return 1;
    /* v10's Svelte sheets, not parsed yet. Visual only: SVG pointer-events keywords, background-clip:
     * text, elliptical border-radius (`/`), -webkit-grab. LAYOUT-AFFECTING (TODO engine: the box
     * keeps its other width / height rule): min() / max() / clamp() sizes, ch and container-query
     * (cq*) units, as in the Svelte popups' widths. */
    if (kn_str_ieq(name, nl, "pointer-events") || (kn_str_ieq(name, nl, "background-clip") && kn_str_ieq(tv, tl, "text")) ||
        (kn_str_ieq(name, nl, "cursor") && tl > 8 && kn_str_ieq(tv, 8, "-webkit-")) ||
        (kn_str_ieq(name, nl, "border-radius") && memchr(tv, '/', tl)))
        return 1;
    if (kn_str_ieq(name, nl, "width") || kn_str_ieq(name, nl, "height") || kn_str_ieq(name, nl, "min-width") ||
        kn_str_ieq(name, nl, "max-width") || kn_str_ieq(name, nl, "min-height") || kn_str_ieq(name, nl, "max-height")) {
        if ((tl > 4 && (kn_str_ieq(tv, 4, "min(") || kn_str_ieq(tv, 4, "max("))) || (tl > 6 && kn_str_ieq(tv, 6, "clamp("))) return 1;
        if (tl > 2 && kn_str_ieq(tv + tl - 2, 2, "ch")) return 1;
    }
    if (kn_str_ieq(name, nl, "background-image") || kn_str_ieq(name, nl, "background")) {
        /* a gradient with two-position colour stops (`#hex 0 11px`) */
        for (size_t k = 0; k + 9 <= tl; k++) if (kn_str_ieq(tv + k, 9, "gradient(")) return 1;
    }
    /* var() of custom properties no sheet defines (bundledStyles' FRVR overlay sets
     * --overlay-image / --prompt-filter from its script): valid at parse time in Chrome */
    for (size_t k = 0; k + 6 <= tl; k++) {
        if (!kn_str_ieq(tv + k, 6, "var(--")) continue;
        size_t e = k + 4;
        while (e < tl && tv[e] != ')' && tv[e] != ',') e++;
        if (!resolve_var(NULL, tv + k + 4, e - k - 4)) return 1;
    }
    if (kn_str_ieq(name, nl, "clip-path") && tl > 8 && kn_str_ieq(tv, 8, "polygon(")) {
        for (size_t k = 0; k + 5 <= tl; k++) if (kn_str_ieq(tv + k, 5, "calc(")) return 1;
    }
    return 0;
}

static char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t r = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[r] = 0;
    *len = r;
    return buf;
}

/* Splits a declaration block into single declarations (outside strings and
 * parens) and checks each one parses and applies. */
static void cover_block(const char *where, const char *text, size_t len, Coverage *cv)
{
    size_t i = 0;
    while (i < len) {
        size_t start = i;
        int depth = 0;
        char q = 0;
        while (i < len) {
            char ch = text[i];
            if (q) { if (ch == '\\') i++; else if (ch == q) q = 0; }
            else if (ch == '"' || ch == '\'') q = ch;
            else if (ch == '(') depth++;
            else if (ch == ')') depth--;
            else if (ch == ';' && depth <= 0) break;
            i++;
        }
        size_t dl = i - start;
        const char *d = kn_str_trim(text + start, &dl);
        i++;
        if (dl == 0) continue;
        const char *colon = memchr(d, ':', dl);
        if (!colon) continue;
        size_t nl = (size_t)(colon - d);
        const char *name = kn_str_trim(d, &nl);
        CssComputed c = fresh();
        ApplyCtx a = { &c, NULL, 0, 0, "" };
        char one[4096];
        size_t w = dl < sizeof(one) - 1 ? dl : sizeof(one) - 1;
        memcpy(one, d, w);
        one[w] = 0;
        /* parse via the block parser so !important handling is exercised */
        int n = css_parse_declaration_block(one, w, apply_cb, &a);
        int parsed = n == 1 && a.calls > 0 && a.failed == 0;
        size_t vlen = dl - nl - 1;
        if (chrome_drops(name, nl, colon + 1, vlen)) {
            if (!parsed) cv->dropped++;
            else {
                cv->fail++;
                fprintf(stderr, "  [%s] %.*s: %.*s   <- accepted, but Chrome drops it\n", where, (int)nl, name,
                        (int)vlen, colon + 1);
            }
        } else if (parsed) cv->ok++;
        else if (known_gap(name, nl, colon + 1, vlen)) {
            cv->gaps++;
            if (getenv("KN_SHOW_GAPS"))
                fprintf(stderr, "  gap [%s] %.*s: %.*s\n", where, (int)nl, name, (int)vlen, colon + 1);
        }
        else {
            cv->fail++;
            size_t vl = dl - nl - 1;
            fprintf(stderr, "  [%s] %.*s: %.*s%s%s\n", where, (int)nl, name, (int)vl, colon + 1,
                    a.failed ? "   <- failed longhand " : "   <- not recognised", a.failed ? a.failed_prop : "");
        }
    }
}

/* Walks a stylesheet; every innermost {...} block is a declaration block.
 * @font-face bodies (src/unicode-range/font-display) are skipped. */
static void cover_sheet(const char *path, Coverage *cv)
{
    size_t len;
    char *s = read_file(path, &len);
    if (!s) { fprintf(stderr, "missing %s\n", path); cv->fail++; return; }
    /* strip comments */
    for (size_t i = 0; i + 1 < len; i++) {
        if (s[i] == '/' && s[i + 1] == '*') {
            size_t j = i + 2;
            while (j + 1 < len && !(s[j] == '*' && s[j + 1] == '/')) j++;
            for (size_t k = i; k < j + 2 && k < len; k++) s[k] = ' ';
            i = j + 1;
        }
    }
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    size_t prelude_start = 0;
    char outer_prelude[64] = "";
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '{') {
            /* find matching close or nested open */
            size_t j = i + 1;
            while (j < len && s[j] != '{' && s[j] != '}') j++;
            size_t pl = i - prelude_start;
            const char *pre = kn_str_trim(s + prelude_start, &pl);
            if (j < len && s[j] == '{') {
                snprintf(outer_prelude, sizeof(outer_prelude), "%.*s", (int)(pl < 60 ? pl : 60), pre);
                prelude_start = i + 1;
                continue;
            }
            if (!(pl >= 10 && kn_str_ieq(pre, 10, "@font-face")))
                cover_block(base, s + i + 1, j - i - 1, cv);
            i = j;
            prelude_start = j + 1;
        } else if (s[i] == '}') {
            prelude_start = i + 1;
            outer_prelude[0] = 0;
        }
    }
    free(s);
}

/* style="..." attributes in HTML and in the JS window strings (\" escaped). */
static void cover_inline(const char *path, Coverage *cv)
{
    size_t len;
    char *s = read_file(path, &len);
    if (!s) { fprintf(stderr, "missing %s\n", path); cv->fail++; return; }
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    char where[96];
    snprintf(where, sizeof(where), "%.80s style=", base);
    for (size_t i = 0; i + 6 < len; i++) {
        if (memcmp(s + i, "style=", 6) != 0) continue;
        if (i > 0 && (kn_is_alpha((unsigned char)s[i - 1]) || s[i - 1] == '-')) continue;
        size_t j = i + 6;
        int escaped = 0;
        if (s[j] == '\\') { escaped = 1; j++; }
        char q = s[j];
        if (q != '"' && q != '\'') continue;
        j++;
        size_t start = j;
        while (j < len && j - start < 4000 && !(s[j] == q && (!escaped || s[j - 1] == '\\'))) j++;
        size_t end = j;
        if (escaped && end > start && s[end - 1] == '\\') end--;
        /* unescape \" \' and &quot; is not used in style values */
        char buf[4096];
        size_t w = 0;
        for (size_t k = start; k < end && w < sizeof(buf) - 1; k++) {
            if (s[k] == '\\' && k + 1 < end) { k++; }
            buf[w++] = s[k];
        }
        buf[w] = 0;
        /* skip JS-concatenated templates: "' + x + '" */
        if (strchr(buf, '+') && strchr(buf, '\'')) { i = j; continue; }
        cover_block(where, buf, w, cv);
        i = j;
    }
    free(s);
}

int main(int argc, char **argv)
{
    const char *root = argc > 1 ? argv[1] : ".";
    test_tokenizer();
    test_props();
    test_lengths();
    test_var();
    test_colors();
    test_transforms();
    test_e2_values();
    test_shadows();
    test_backgrounds();
    test_filters_clip();
    test_animation();
    test_layout_props();
    test_zoom();
    test_font_shorthand();
    test_text_props();
    test_decl_block_real();
    test_computed();
    printf("unit: %d passed, %d failed\n", g_pass, g_fail);

    /* every stylesheet the pages link (ui/index.html: Krunker v10.0.2's menu, the hub, the editor),
     * then the ones the C UI adds at run time */
    static const char *pages_linking[] = { "ui/index.html", "ui/social.html", "ui/editor.html" };
    static const char *runtime_sheets[] = { "ui/css/devconsole.css", "ui/css/ranked-kn.css" };
    static char sheet_names[64][192];
    const char *sheets[64];
    size_t nsheets = 0;
    for (size_t i = 0; i < sizeof(pages_linking) / sizeof(pages_linking[0]); i++) {
        char p[1024];
        size_t len;
        snprintf(p, sizeof(p), "%s/%s", root, pages_linking[i]);
        char *html = read_file(p, &len);
        if (!html) { fprintf(stderr, "missing %s\n", p); g_fail++; continue; }
        for (const char *q = html; (q = strstr(q, "rel=\"stylesheet\"")) != NULL; q++) {
            const char *tag = q;
            while (tag > html && *tag != '<') tag--;
            const char *h = strstr(tag, "href=\"");
            const char *end = h ? strchr(h + 6, '"') : NULL;
            const char *close = strchr(tag, '>');
            if (!h || !end || !close || h > close) continue;
            char rel[192];
            snprintf(rel, sizeof(rel), "ui/%.*s", (int)(end - h - 6), h + 6);
            char *qm = strchr(rel, '?');
            if (qm) *qm = 0;
            if (strstr(rel, "://")) continue;
            int dup = 0;
            for (size_t k = 0; k < nsheets; k++) dup |= !strcmp(sheets[k], rel);
            if (dup || nsheets >= 62) continue;
            snprintf(sheet_names[nsheets], sizeof(sheet_names[nsheets]), "%s", rel);
            sheets[nsheets] = sheet_names[nsheets];
            nsheets++;
        }
        free(html);
    }
    for (size_t i = 0; i < sizeof(runtime_sheets) / sizeof(runtime_sheets[0]) && nsheets < 64; i++) {
        int dup = 0;
        for (size_t k = 0; k < nsheets; k++) dup |= !strcmp(sheets[k], runtime_sheets[i]);
        if (!dup) sheets[nsheets++] = runtime_sheets[i];
    }
    if (nsheets < 10) { fprintf(stderr, "only %zu stylesheets found in the pages\n", nsheets); g_fail++; }
    Coverage total = { 0, 0, 0, 0 };
    char path[1024];
    for (size_t i = 0; i < nsheets; i++) {
        size_t len;
        snprintf(path, sizeof(path), "%s/%s", root, sheets[i]);
        char *text = read_file(path, &len);
        if (text) collect_sheet_vars(text, len);
        free(text);
    }
    for (size_t i = 0; i < nsheets; i++) {
        Coverage cv = { 0, 0, 0, 0 };
        snprintf(path, sizeof(path), "%s/%s", root, sheets[i]);
        cover_sheet(path, &cv);
        printf("sheet %-40s %5d ok %3d failed %3d dropped as Chrome %3d known gaps\n", sheets[i], cv.ok, cv.fail, cv.dropped, cv.gaps);
        total.ok += cv.ok;
        total.fail += cv.fail;
        total.dropped += cv.dropped;
        total.gaps += cv.gaps;
    }
    /* inline style="" of the pages and of every window template (ui/windows) */
    static const char *pages[] = { "ui/index.html", "ui/social.html" };
    for (size_t i = 0; i < sizeof(pages) / sizeof(pages[0]); i++) {
        Coverage cv = { 0, 0, 0, 0 };
        snprintf(path, sizeof(path), "%s/%s", root, pages[i]);
        cover_inline(path, &cv);
        printf("inline %-39s %5d ok %3d failed %3d dropped as Chrome %3d known gaps\n", pages[i], cv.ok, cv.fail, cv.dropped, cv.gaps);
        total.ok += cv.ok;
        total.fail += cv.fail;
        total.dropped += cv.dropped;
        total.gaps += cv.gaps;
    }
    {
        Coverage cv = { 0, 0, 0, 0 };
        snprintf(path, sizeof(path), "%s/ui/windows", root);
        DIR *d = opendir(path);
        int files = 0;
        if (!d) { fprintf(stderr, "missing %s\n", path); cv.fail++; }
        for (struct dirent *de; d && (de = readdir(d)) != NULL;) {
            size_t n = strlen(de->d_name);
            if (n < 6 || strcmp(de->d_name + n - 5, ".html")) continue;
            snprintf(path, sizeof(path), "%s/ui/windows/%s", root, de->d_name);
            cover_inline(path, &cv);
            files++;
        }
        if (d) closedir(d);
        printf("inline ui/windows/*.html (%2d files)            %5d ok %3d failed %3d dropped as Chrome %3d known gaps\n", files, cv.ok, cv.fail, cv.dropped, cv.gaps);
        total.ok += cv.ok;
        total.fail += cv.fail;
        total.dropped += cv.dropped;
        total.gaps += cv.gaps;
    }
    printf("coverage: %d declarations parsed, %d failed, %d dropped as Chrome does, %d known gaps\n", total.ok, total.fail, total.dropped, total.gaps);
    return (g_fail || total.fail) ? 1 : 0;
}
