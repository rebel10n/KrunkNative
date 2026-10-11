/*
 * test_widgets.c - form-control tests for widgets.c.
 *
 * Loads the settings header + General tab (ui/windows/settings-head.html,
 * settings-tab-0.html) plus a few extra controls into a Dom, runs the real
 * cascade (UA sheet + ui/css/main.css) and drives widgets.c through its
 * public API with a stub host (geometry table, event log).  Checks:
 *   - control classification (unknown/undefined/input types -> text)
 *   - Chrome intrinsic sizes against the capture-derived numbers
 *   - range value <-> x mapping (min/max/step), number formatting
 *   - a text editing sequence (caret/selection/backspace/delete/select-all,
 *     maxlength, input/change rules, :placeholder-shown)
 *   - number ArrowUp/Down stepping and character filtering
 *   - range drag with capture: input per change, change on release
 *   - select popup geometry, mouse choose, keyboard navigation
 *   - checkbox toggling -> `checked` + `input:checked + .slider` restyle
 *   - color input: swatch, picker popup writing #rrggbb
 *   - the DlItems emitted for the General tab controls (thumb/swatch/clip/
 *     chevron/caret/placeholder)
 *   - with KN_WIDGETS_GL: renders the General tab control row through
 *     gl_backend into /tmp/kn_widgets_row.ppm for a visual check against
 *     doc/ref/krunker-v3.1.1/windows/win-settings-general.png.
 *
 * Build: see the CMakeLists.txt block at the end of this file.
 * Usage: test_widgets [repo root]   Exit code: number of failed checks.
 */
#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_TRUETYPE_TABLES_H

#include "css_cascade.h"
#include "css_sheet.h"
#include "dom.h"
#include "text.h"
#include "ua_css.h"
#include "util.h"
#include "widgets.h"
#include "events.h"

#ifdef KN_WIDGETS_GL
#include <gl.h>
#include <GLFW/glfw3.h>
#include "gl_backend.h"
unsigned int g_active_shader = 0;
unsigned int g_active_texture = 0;
#endif

#ifndef KN_REPO_ROOT
#define KN_REPO_ROOT "."
#endif

/* widgets_paint_host() (the PaintHost adapter) allocates from paint.c; the
 * test links without paint.c, so provide the allocator. */
struct PaintCtx;
void *paint_alloc(struct PaintCtx *ctx, size_t size);
void *paint_alloc(struct PaintCtx *ctx, size_t size) { (void)ctx; return malloc(size); }

static int g_pass, g_fail;
static const char *g_root = KN_REPO_ROOT;

#define WK_SPACE_FOR_TEST 32
#define CHECK(cond, ...) do { if (cond) g_pass++; else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define CHECK_STR(got, want, what) do { const char *g_ = (got), *w_ = (want); CHECK(strcmp(g_, w_) == 0, "%s: got \"%s\" want \"%s\"", what, g_, w_); } while (0)
#define CHECK_NEAR(got, want, tol, what) do { float g_ = (float)(got), w_ = (float)(want); CHECK(fabsf(g_ - w_) <= (tol), "%s: got %g want %g", what, g_, w_); } while (0)

/* ------------------------------------------------------------------------- */
/* Files                                                                      */
/* ------------------------------------------------------------------------- */

static char *read_file(const char *rel, size_t *len)
{
    char path[1024];
    FILE *f;
    long n;
    char *buf;
    snprintf(path, sizeof path, "%s/%s", g_root, rel);
    f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (char *)malloc((size_t)n + 1);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(buf); return NULL; }
    buf[n] = '\0';
    fclose(f);
    if (len) *len = (size_t)n;
    return buf;
}

/* ------------------------------------------------------------------------- */
/* Stub host                                                                  */
/* ------------------------------------------------------------------------- */

static Dom *g_dom;
static CssSheet *g_ua, *g_main;
static TextSystem *g_text;
static Widgets *g_w;
static FT_Library g_ft;
static FT_Face g_gamefont;

typedef struct { HtmlElem e; WidgetGeom g; } GeomEntry;
static GeomEntry g_geoms[64];
static int g_ngeoms;

static char g_log[64][64];
static int g_nlog;
static int g_invalidate_count;
static HtmlElem g_focus;
static int g_capture;

static void set_geom(HtmlElem e, float x, float y, float w, float h, float bt, float br, float bb, float bl, float pt, float pr, float pb, float pl)
{
    GeomEntry *ge = NULL;
    int i;
    for (i = 0; i < g_ngeoms; i++) if (g_geoms[i].e == e) ge = &g_geoms[i];
    if (!ge) ge = &g_geoms[g_ngeoms++];
    ge->e = e;
    ge->g.x = x; ge->g.y = y; ge->g.w = w; ge->g.h = h;
    ge->g.border[CSS_TOP] = bt; ge->g.border[CSS_RIGHT] = br; ge->g.border[CSS_BOTTOM] = bb; ge->g.border[CSS_LEFT] = bl;
    ge->g.padding[CSS_TOP] = pt; ge->g.padding[CSS_RIGHT] = pr; ge->g.padding[CSS_BOTTOM] = pb; ge->g.padding[CSS_LEFT] = pl;
}

static const CssComputed *host_computed(void *u, HtmlElem e) { (void)u; return cascade_elem_computed(g_dom, e); }

static CssComputed g_pseudo_buf;
static const CssComputed *host_pseudo(void *u, HtmlElem e, int pe)
{
    (void)u;
    return cascade_pseudo_computed(g_dom, e, pe, &g_pseudo_buf) ? &g_pseudo_buf : NULL;
}

static int host_geom(void *u, HtmlElem e, WidgetGeom *out)
{
    int i;
    (void)u;
    for (i = 0; i < g_ngeoms; i++) if (g_geoms[i].e == e) { *out = g_geoms[i].g; return 1; }
    return 0;
}

static void host_invalidate(void *u, HtmlElem e, int relayout) { (void)u; (void)e; (void)relayout; g_invalidate_count++; }

static int host_fire(void *u, HtmlElem e, const char *type, int key)
{
    const char *id = dom_get_attr(g_dom, e, "id", NULL);
    (void)u; (void)key;
    if (g_nlog < 64) snprintf(g_log[g_nlog++], 64, "%s@%s", type, id ? id : "?");
    return 1;
}

static void host_capture(void *u, HtmlElem e) { (void)u; (void)e; g_capture = 1; }
static void host_release(void *u) { (void)u; g_capture = 0; }

static void host_focus(void *u, HtmlElem e)
{
    (void)u;
    if (g_focus == e) return;
    if (g_focus) { dom_set_pseudo_state(g_dom, g_focus, DOM_PSEUDO_FOCUS, 0); widgets_on_blur(g_w, g_focus); }
    g_focus = e;
    if (e) { dom_set_pseudo_state(g_dom, e, DOM_PSEUDO_FOCUS, 1); widgets_on_focus(g_w, e); }
}

static void host_blur(void *u, HtmlElem e) { (void)u; if (g_focus == e) host_focus(u, 0); }

/* Chrome's avg/max char widths straight from the font tables (what text.h
 * will eventually expose). */
static int host_char_widths(void *u, TextFont *font, float *avg, float *max)
{
    const char *family = text_face_family(text_font_face(font));
    float size = text_font_size(font);
    TT_OS2 *os2;
    (void)u;
    if (!g_gamefont || !family || strcmp(family, "gamefont") != 0) return 0;
    os2 = (TT_OS2 *)FT_Get_Sfnt_Table(g_gamefont, FT_SFNT_OS2);
    if (!os2) return 0;
    *avg = (float)os2->xAvgCharWidth * size / (float)g_gamefont->units_per_EM;
    *max = (float)(g_gamefont->bbox.xMax - g_gamefont->bbox.xMin) * size / (float)g_gamefont->units_per_EM;
    return 1;
}

static char g_dispatch_value[128];
static char g_dispatch_name[64];
int html_ui_dispatch(const char *name, const HtmlEvent *ev, const HtmlArg *args, int argc)
{
    (void)args; (void)argc;
    snprintf(g_dispatch_name, sizeof g_dispatch_name, "%s", name ? name : "");
    snprintf(g_dispatch_value, sizeof g_dispatch_value, "%s", ev && ev->value ? ev->value : "(null)");
    return 1;
}
int html_ui_dispatch_listener(const char *elem_id, const HtmlEvent *ev) { (void)elem_id; (void)ev; return 1; }

static void log_reset(void) { g_nlog = 0; }
static int log_count(const char *entry)
{
    int i, n = 0;
    for (i = 0; i < g_nlog; i++) if (strcmp(g_log[i], entry) == 0) n++;
    return n;
}

static void restyle(void) { cascade_restyle(g_dom, g_ua, &g_main, 1); }

static void *dl_alloc(void *u, size_t n)
{
    static void *blocks[256];
    static int nblocks;
    (void)u;
    if (nblocks >= 256) return NULL;
    blocks[nblocks] = malloc(n);
    return blocks[nblocks++];
}

/* ------------------------------------------------------------------------- */
/* Document                                                                   */
/* ------------------------------------------------------------------------- */

static const char *k_extra =
    "<div id=\"extra\">"
    "<input id=\"t4\" type=\"text\" maxlength=\"4\" value=\"\">"
    "<input id=\"tu\" type=\"undefined\">"
    "<input id=\"ti\" type=\"input\" class=\"inputGrey2\" placeholder=\"1280x720\" value=\"\">"
    "<input id=\"th\" type=\"hidden\">"
    "<input id=\"turl\" type=\"url\" class=\"inputGrey2\" placeholder=\"Crosshair Image\" value=\"\">"
    "<input id=\"tc\" type=\"color\" value=\"#EB5656\" style=\"float:right\">"
    "<input id=\"tcx\" type=\"color\" value=\"nope\">"
    "<input id=\"tp\" type=\"password\" value=\"ab\">"
    "<textarea id=\"ta\" style=\"font-family:gamefont;font-size:16px\">\nfirst\r\nsecond</textarea>"
    "<textarea id=\"tb\" rows=\"3\" cols=\"10\" maxlength=\"12\" placeholder=\"Say\" style=\"font-family:gamefont;font-size:16px\"></textarea>"
    "<input id=\"rgz\" type=\"range\" style=\"zoom:2\">"
    "<input id=\"cbz\" type=\"checkbox\" style=\"zoom:0.5\">"
    "<label class=\"switch\"><input id=\"cb\" type=\"checkbox\"><span id=\"cbs\" class=\"slider\"></span></label>"
    "<select id=\"long\" onchange=\"setSetting(&quot;long&quot;, this.value)\"><option value=\"v0\">Alpha</option><option value=\"v1\">Bravo</option><option value=\"v2\">Charlie</option><option value=\"v3\">Chess</option><option value=\"v4\">Delta</option><option value=\"v5\">Echo</option><option value=\"v6\">Foxtrot</option><option value=\"v7\">Golf</option><option value=\"v8\">Hotel</option><option value=\"v9\">India</option><option value=\"v10\">Juliet</option><option value=\"v11\">Kilo</option><option value=\"v12\">Lima</option><option value=\"v13\">Mike</option><option value=\"v14\">November</option><option value=\"v15\">Oscar</option><option value=\"v16\">Papa</option><option value=\"v17\">Quebec</option><option value=\"v18\">Romeo</option><option value=\"v19\">Sierra</option><option value=\"v20\">Tango</option><option value=\"v21\">Uniform</option><option value=\"v22\">Victor</option><option value=\"v23\">Whiskey</option><option value=\"v24\">Xray</option><option value=\"v25\" selected>Yankee</option><option value=\"v26\">Zulu</option><option value=\"v27\">Zebra</option><option value=\"v28\">Zeta</option><option value=\"v29\">Zinc</option></select>"
    "</div>";

static int load_document(void)
{
    size_t hlen, tlen, clen;
    char *head = read_file("ui/windows/settings-head.html", &hlen);
    char *tab = read_file("ui/windows/settings-tab-0.html", &tlen);
    char *css = read_file("ui/css/main.css", &clen);
    KnStrBuf sb = {0};
    int rc;
    if (!head || !tab || !css) return -1;
    kn_sb_append_cstr(&sb, "<!DOCTYPE html><html><head></head><body><div id=\"menuWindow\">");
    kn_sb_append(&sb, head, hlen);
    kn_sb_append_cstr(&sb, "<div id=\"settHolder\">");
    kn_sb_append(&sb, tab, tlen);
    kn_sb_append_cstr(&sb, "</div></div>");
    kn_sb_append_cstr(&sb, k_extra);
    kn_sb_append_cstr(&sb, "</body></html>");
    g_dom = dom_create();
    rc = dom_load_html(g_dom, sb.data, sb.len);
    kn_sb_free(&sb);
    free(head);
    free(tab);
    g_ua = css_sheet_load(KN_UA_CSS, strlen(KN_UA_CSS), "ua.css");
    g_main = css_sheet_load(css, clen, "ui/css/main.css");
    free(css);
    restyle();
    return rc;
}

static HtmlElem by_id(const char *id)
{
    HtmlElem e = dom_by_id(g_dom, id);
    CHECK(e != 0, "element #%s exists", id);
    return e;
}

/* ------------------------------------------------------------------------- */
/* Tests                                                                      */
/* ------------------------------------------------------------------------- */

static void test_kinds(void)
{
    CHECK(widgets_kind(g_w, by_id("settSearch")) == WIDGET_TEXT, "type=text -> TEXT");
    CHECK(widgets_kind(g_w, by_id("tu")) == WIDGET_TEXT, "type=undefined -> TEXT");
    CHECK(widgets_kind(g_w, by_id("ti")) == WIDGET_TEXT, "type=input -> TEXT");
    CHECK(widgets_kind(g_w, by_id("turl")) == WIDGET_TEXT, "type=url -> TEXT");
    CHECK(widgets_kind(g_w, by_id("th")) == WIDGET_NONE, "type=hidden -> NONE");
    CHECK(widgets_kind(g_w, by_id("tp")) == WIDGET_PASSWORD, "password");
    CHECK(widgets_kind(g_w, by_id("slid_input_lagComp")) == WIDGET_NUMBER, "number");
    CHECK(widgets_kind(g_w, by_id("slid_lagComp")) == WIDGET_RANGE, "range");
    CHECK(widgets_kind(g_w, by_id("tc")) == WIDGET_COLOR, "color");
    CHECK(widgets_kind(g_w, by_id("cb")) == WIDGET_CHECKBOX, "checkbox");
    CHECK(widgets_kind(g_w, by_id("settingsPreset")) == WIDGET_SELECT, "select");
    CHECK(widgets_kind(g_w, by_id("menuWindow")) == WIDGET_NONE, "div -> NONE");
}

static void test_intrinsic(void)
{
    float w = 0, h = 0;
    HtmlElem lang = dom_query(g_dom, 0, "#setBod_local .settName:nth-child(2) select");
    HtmlElem region = dom_query(g_dom, 0, "#setBod_local select");
    CHECK(lang && region && lang != region, "found the two localisation selects");

    /* .inputGrey2 url input, 12px gamefont: 20 * round(0.731em) + (round(1.757em) - 9) = 180 + 12 */
    CHECK(widgets_intrinsic_size(g_w, by_id("turl"), NULL, &w, &h), "url intrinsic");
    CHECK_NEAR(w, 192, 0.5f, "url input content width (capture: 127 device px = 202 css border box)");
    CHECK_NEAR(h, 17, 0.5f, "url input content height = line-height normal of 12px gamefont");

    /* #settSearch: 16px gamefont -> 18 + 4 */
    CHECK(widgets_intrinsic_size(g_w, by_id("settSearch"), NULL, &w, &h), "search intrinsic");
    CHECK_NEAR(h, 22, 0.5f, "#settSearch content height");

    /* selects: 4 + ceil(widest) + 1 + 15 by 1 + lh + 1 */
    CHECK(widgets_intrinsic_size(g_w, region, NULL, &w, &h), "region select intrinsic");
    CHECK_NEAR(w, 4 + 41 + 1 + 15, 0.5f, "\"Local\" select content width (capture: 45 device px = 71.7 css border box)");
    CHECK_NEAR(h, 19, 0.5f, "\"Local\" select content height");
    CHECK(widgets_intrinsic_size(g_w, lang, NULL, &w, &h), "language select intrinsic");
    CHECK_NEAR(w, 4 + 89 + 16, 0.5f, "Language select width from the longest option (Portuguese)");
    CHECK(widgets_intrinsic_size(g_w, by_id("settingsPreset"), NULL, &w, &h), "preset select intrinsic");
    CHECK_NEAR(h, 22, 0.5f, "14px preset select content height (capture: 21 device px = 33.5 css border box = 6+1+20+1+6)");

    CHECK(widgets_intrinsic_size(g_w, by_id("cb"), NULL, &w, &h) && w == 13 && h == 13, "checkbox 13x13");
    CHECK(widgets_intrinsic_size(g_w, by_id("slid_lagComp"), NULL, &w, &h) && w == 129 && h == 25, "appearance:none range: 129 x the track height (auto = the 25px thumb)");
    CHECK(widgets_intrinsic_size(g_w, by_id("tc"), NULL, &w, &h) && w == 44 && h == 23, "color 44x23 content (50x27 border box)");
    CHECK(!widgets_intrinsic_size(g_w, by_id("th"), NULL, &w, &h), "hidden has no intrinsic size");
}

static void test_range_math(void)
{
    char buf[32];
    CHECK_NEAR(widgets_range_value_from_x(0, 1, 0.1, 0, 250, 15, 0), 0, 1e-9f, "x at track start -> min");
    CHECK_NEAR(widgets_range_value_from_x(0, 1, 0.1, 0, 250, 15, 250), 1, 1e-9f, "x at track end -> max");
    CHECK_NEAR(widgets_range_value_from_x(0, 1, 0.1, 0, 250, 15, 125), 0.5, 1e-9f, "x at centre -> 0.5");
    CHECK_NEAR(widgets_range_value_from_x(0, 1, 0.1, 0, 250, 15, 130), 0.5, 1e-9f, "snaps to step 0.1");
    CHECK_NEAR(widgets_range_value_from_x(0, 500, 1, 100, 244, 15, 100 + 7.5f + 229 * 0.4f), 200, 1e-9f, "0..500 step 1 at 40%%");
    CHECK_NEAR(widgets_range_value_from_x(0, 500, 1, 100, 244, 15, -50), 0, 1e-9f, "clamped below");
    CHECK_NEAR(widgets_range_thumb_x(0, 1, 1, 0, 250, 15), 235, 1e-5f, "thumb x at max = track_w - thumb_w");
    CHECK_NEAR(widgets_range_thumb_x(0, 1, 0.5, 0, 250, 15), 117.5, 1e-5f, "thumb x at 0.5");
    CHECK_NEAR(widgets_range_thumb_x(0, 500, 200, 0, 244, 15), 229 * 0.4f, 1e-4f, "thumb x 200/500");

    widgets_format_number(1, 1, buf, sizeof buf); CHECK_STR(buf, "1", "format 1 (1 decimal)");
    widgets_format_number(0.5, 1, buf, sizeof buf); CHECK_STR(buf, "0.5", "format 0.5");
    widgets_format_number(0.30000000000000004, 1, buf, sizeof buf); CHECK_STR(buf, "0.3", "format 0.3 (fp noise)");
    widgets_format_number(200, 0, buf, sizeof buf); CHECK_STR(buf, "200", "format 200");
    widgets_format_number(-0.0, 0, buf, sizeof buf); CHECK_STR(buf, "0", "format -0");
    widgets_format_number(1.25, -1, buf, sizeof buf); CHECK_STR(buf, "1.25", "format auto 1.25");
}

static void test_text_editing(void)
{
    HtmlElem e = by_id("settSearch");
    set_geom(e, 0, 0, 220, 33, 0, 0, 0, 0, 6, 10, 5, 10);
    log_reset();
    CHECK(dom_pseudo_state(g_dom, e) & DOM_PSEUDO_PLACEHOLDER_SHOWN, "empty input with placeholder is :placeholder-shown");

    host_focus(NULL, e);
    CHECK(g_focus == e, "focused");
    widgets_char(g_w, e, 'a');
    widgets_char(g_w, e, 'b');
    widgets_char(g_w, e, 'c');
    CHECK_STR(widgets_get_value(g_w, e), "abc", "typed abc");
    CHECK_STR(dom_get_value(g_dom, e), "abc", "value mirrored into the attribute");
    CHECK(log_count("input@settSearch") == 3, "input fired per character (%d)", log_count("input@settSearch"));
    CHECK(!(dom_pseudo_state(g_dom, e) & DOM_PSEUDO_PLACEHOLDER_SHOWN), ":placeholder-shown cleared");

    widgets_key(g_w, e, 263, 0, EV_PRESS, 0);              /* Left  -> caret 2 */
    widgets_key(g_w, e, 263, 0, EV_PRESS, 0);              /* Left  -> caret 1 */
    widgets_key(g_w, e, 262, 0, EV_PRESS, EV_MOD_SHIFT);   /* shift+Right -> selection [1,2) */
    widgets_char(g_w, e, 'X');
    CHECK_STR(widgets_get_value(g_w, e), "aXc", "typing replaces the selection");
    widgets_key(g_w, e, 259, 0, EV_PRESS, 0);              /* Backspace */
    CHECK_STR(widgets_get_value(g_w, e), "ac", "backspace");
    widgets_key(g_w, e, 268, 0, EV_PRESS, 0);              /* Home */
    widgets_key(g_w, e, 261, 0, EV_PRESS, 0);              /* Delete */
    CHECK_STR(widgets_get_value(g_w, e), "c", "home + delete");
    widgets_key(g_w, e, 65, 0, EV_PRESS, EV_MOD_CONTROL);  /* ctrl+A */
    widgets_char(g_w, e, 'z');
    CHECK_STR(widgets_get_value(g_w, e), "z", "select-all + type");
    widgets_key(g_w, e, 269, 0, EV_PRESS, 0);              /* End */
    widgets_char(g_w, e, 0xe9);                            /* é (2 bytes) */
    widgets_char(g_w, e, '!');
    widgets_key(g_w, e, 263, 0, EV_PRESS, 0);
    widgets_key(g_w, e, 259, 0, EV_PRESS, 0);              /* Backspace removes the whole é */
    CHECK_STR(widgets_get_value(g_w, e), "z!", "utf-8 aware backspace");
    CHECK(log_count("change@settSearch") == 0, "no change before commit");
    widgets_key(g_w, e, EV_KEY_ENTER, 0, EV_PRESS, 0);
    CHECK(log_count("change@settSearch") == 1, "Enter commits -> change");
    widgets_key(g_w, e, EV_KEY_ENTER, 0, EV_PRESS, 0);
    CHECK(log_count("change@settSearch") == 1, "Enter again without edits -> no change");
    widgets_char(g_w, e, '?');
    host_focus(NULL, 0);
    CHECK(log_count("change@settSearch") == 2, "blur after an edit -> change");
    CHECK(log_count("keypress@settSearch") == 0, "widgets never fires keypress (events.c does)");

    /* programmatic set_value: no events, caret at end, attribute mirrored */
    log_reset();
    widgets_set_value(g_w, e, "hello");
    CHECK_STR(dom_get_value(g_dom, e), "hello", "set_value mirrored");
    CHECK(g_nlog == 0, "set_value fires nothing");
    host_focus(NULL, e);
    host_focus(NULL, 0);
    CHECK(g_nlog == 0, "focus/blur without edits fires nothing");
    widgets_set_value(g_w, e, "");
    CHECK(dom_pseudo_state(g_dom, e) & DOM_PSEUDO_PLACEHOLDER_SHOWN, "cleared -> :placeholder-shown again");

    /* mouse caret placement + drag selection */
    {
        WidgetGeom g;
        float adv1;
        TextFont *f = text_font(g_text, "gamefont", 16, 400, 0);
        widgets_set_value(g_w, e, "abcd");
        host_geom(NULL, e, &g);
        adv1 = text_measure(f, "ab", 2, 0);
        log_reset();
        CHECK(widgets_mouse_down(g_w, e, 10 + adv1 + 0.5f, 15, EV_BUTTON_LEFT, 0) & EV_WIDGET_CAPTURE, "text press captures");
        CHECK(g_focus == e, "press focuses");
        widgets_mouse_move(g_w, e, 10 + text_measure(f, "abcd", 4, 0) + 20, 15, 1);
        widgets_mouse_up(g_w, e, 10 + text_measure(f, "abcd", 4, 0) + 20, 15, EV_BUTTON_LEFT);
        widgets_char(g_w, e, 'Z');
        CHECK_STR(widgets_get_value(g_w, e), "abZ", "drag selection [2,4) replaced");
        host_focus(NULL, 0);
    }

    /* maxlength */
    {
        HtmlElem m = by_id("t4");
        set_geom(m, 0, 0, 100, 21, 2, 2, 2, 2, 1, 2, 1, 2);
        host_focus(NULL, m);
        widgets_char(g_w, m, '1'); widgets_char(g_w, m, '2'); widgets_char(g_w, m, '3');
        widgets_char(g_w, m, '4'); widgets_char(g_w, m, '5'); widgets_char(g_w, m, '6');
        CHECK_STR(widgets_get_value(g_w, m), "1234", "maxlength=4 caps typing");
        host_focus(NULL, 0);
    }

    /* password shows bullets but keeps the value */
    {
        HtmlElem p = by_id("tp");
        CHECK_STR(widgets_get_value(g_w, p), "ab", "password value");
    }
}

#define KEY(e, k, m) widgets_key(g_w, (e), (k), 0, EV_PRESS, (m))
#define CTRL EV_MOD_CONTROL

/* <textarea>: value from the child text, CRLF -> LF, Enter, Up/Down by line,
 * wrapping, maxlength, cols/rows intrinsic size, mouse caret on line 2. */
static void test_textarea(void)
{
    HtmlElem e = by_id("ta"), b = by_id("tb");
    TextFont *f = text_font(g_text, "gamefont", 16, 400, 0);
    float w, h;
    CHECK(widgets_kind(g_w, e) == WIDGET_TEXTAREA, "textarea kind");
    CHECK_STR(widgets_get_value(g_w, e), "first\nsecond", "default value = text, leading newline dropped, CRLF -> LF");
    CHECK(widgets_intrinsic_size(g_w, b, NULL, &w, &h), "textarea intrinsic size");
    CHECK_NEAR(w, 10 * 12 + 15, 0.5, "cols=10: 10 * avgCharWidth (12) + scrollbar");
    CHECK_NEAR(h, 3 * 22, 0.5, "rows=3 lines of 22px");
    CHECK(widgets_intrinsic_size(g_w, e, NULL, &w, &h) && fabsf(w - (20 * 12 + 15)) < 0.5f && fabsf(h - 44) < 0.5f,
          "default 20 cols x 2 rows (%g x %g)", w, h);
    set_geom(e, 0, 0, 200, 80, 1, 1, 1, 1, 2, 2, 2, 2);
    host_focus(NULL, e);
    KEY(e, 268, 0);                                        /* Home: start of "second" */
    widgets_char(g_w, e, 'f');
    CHECK_STR(widgets_get_value(g_w, e), "first\nfsecond", "Home goes to the line start");
    KEY(e, 265, 0);                                        /* Up: line 1, same x */
    widgets_char(g_w, e, '<');
    CHECK_STR(widgets_get_value(g_w, e), "f<irst\nfsecond", "Up keeps the column");
    KEY(e, 269, 0);                                        /* End of line 1 */
    KEY(e, EV_KEY_ENTER, 0);
    widgets_char(g_w, e, 'x');
    CHECK_STR(widgets_get_value(g_w, e), "f<irst\nx\nfsecond", "Enter inserts a line feed");
    KEY(e, 264, 0);                                        /* Down: into "fsecond" */
    KEY(e, 268, 0);                                        /* Home */
    KEY(e, 261, 0);                                        /* Delete */
    CHECK_STR(widgets_get_value(g_w, e), "f<irst\nx\nsecond", "Down + Home + Delete");
    /* mouse: second visual line, after "x" */
    widgets_set_value(g_w, e, "ab\ncd");
    {
        float adv = text_measure(f, "c", 1, 0);
        widgets_mouse_down(g_w, e, 3 + adv + 0.4f, 3 + 22 + 5, EV_BUTTON_LEFT, 0);
        widgets_mouse_up(g_w, e, 3 + adv + 0.4f, 3 + 22 + 5, EV_BUTTON_LEFT);
        widgets_char(g_w, e, '!');
        CHECK_STR(widgets_get_value(g_w, e), "ab\nc!d", "click on line 2 places the caret");
    }
    /* soft wrap: Down moves to the wrapped row */
    {
        float wd = text_measure(f, "aaaa ", 5, 0);
        set_geom(e, 0, 0, wd + 6 + 2, 80, 1, 1, 1, 1, 2, 2, 2, 2);
        widgets_set_value(g_w, e, "aaaa bbbb");
        KEY(e, 268, CTRL);
        KEY(e, 265, 0);                                    /* Up on the first row: caret 0 */
        KEY(e, 264, 0);                                    /* Down -> wrapped "bbbb" row */
        widgets_char(g_w, e, '^');
        CHECK_STR(widgets_get_value(g_w, e), "aaaa ^bbbb", "Down follows the soft wrap");
    }
    host_focus(NULL, 0);
    host_focus(NULL, b);
    for (const char *p = "hello world, again"; *p; p++) widgets_char(g_w, b, (unsigned char)*p);
    CHECK_STR(widgets_get_value(g_w, b), "hello world,", "maxlength caps a textarea");
    host_focus(NULL, 0);
}

/* zoom scales the themed control sizes */
static void test_zoomed_controls(void)
{
    float w, h;
    CHECK(widgets_intrinsic_size(g_w, by_id("rgz"), NULL, &w, &h) && fabsf(w - 258) < 0.01f && fabsf(h - 32) < 0.01f,
          "zoom:2 range = 258 x 32 (%g x %g)", w, h);
    CHECK(widgets_intrinsic_size(g_w, by_id("cbz"), NULL, &w, &h) && fabsf(w - 6.5f) < 0.01f, "zoom:.5 checkbox = 6.5 (%g)", w);
}

#define KEY(e, k, m) widgets_key(g_w, (e), (k), 0, EV_PRESS, (m))
#define CTRL EV_MOD_CONTROL
#define SHIFT EV_MOD_SHIFT

static void test_clipboard_editing(void)
{
    HtmlElem e = by_id("settSearch"), m = by_id("t4"), n = by_id("slid_input_flickClamp"), p = by_id("tp");
    set_geom(e, 0, 0, 220, 33, 0, 0, 0, 0, 6, 10, 5, 10);
    set_geom(m, 0, 40, 100, 21, 2, 2, 2, 2, 1, 2, 1, 2);
    set_geom(n, 0, 80, 70, 34, 0, 0, 0, 0, 5, 10, 5, 10);
    widgets_set_value(g_w, e, "hello world foo");
    host_focus(NULL, e);
    log_reset();
    /* word jumps (Linux/macOS rule: Ctrl+Right stops at word ends) */
    KEY(e, 263, CTRL);                                   /* Ctrl+Left: start of "foo" */
    widgets_char(g_w, e, '#');
    CHECK_STR(widgets_get_value(g_w, e), "hello world #foo", "Ctrl+Left -> start of the last word");
    KEY(e, 90, CTRL);
    CHECK_STR(widgets_get_value(g_w, e), "hello world foo", "Ctrl+Z undoes the typing");
    KEY(e, 268, 0);                                      /* Home */
    KEY(e, 262, CTRL);                                   /* Ctrl+Right -> end of "hello" */
    widgets_char(g_w, e, '!');
    CHECK_STR(widgets_get_value(g_w, e), "hello! world foo", "Ctrl+Right -> end of the word");
    KEY(e, 90, CTRL);
    /* Ctrl+Shift+Left selects by word, Ctrl+C / Ctrl+V */
    KEY(e, 269, 0);                                      /* End */
    KEY(e, 263, CTRL | SHIFT);
    KEY(e, 263, CTRL | SHIFT);                           /* selection "world foo" */
    KEY(e, 67, CTRL);                                    /* copy */
    KEY(e, 268, 0);
    KEY(e, 86, CTRL);                                    /* paste at the start */
    CHECK_STR(widgets_get_value(g_w, e), "world foohello world foo", "Ctrl+Shift+Left x2, Ctrl+C, Home, Ctrl+V");
    CHECK(log_count("input@settSearch") >= 1, "paste fires input");
    KEY(e, 90, CTRL);
    CHECK_STR(widgets_get_value(g_w, e), "hello world foo", "paste is one undo step");
    KEY(e, 89, CTRL);
    CHECK_STR(widgets_get_value(g_w, e), "world foohello world foo", "Ctrl+Y redoes");
    KEY(e, 90, CTRL | SHIFT);
    CHECK_STR(widgets_get_value(g_w, e), "world foohello world foo", "Ctrl+Shift+Z with nothing left to redo is a no-op");
    KEY(e, 90, CTRL);
    KEY(e, 90, CTRL | SHIFT);
    CHECK_STR(widgets_get_value(g_w, e), "world foohello world foo", "Ctrl+Shift+Z redoes after an undo");
    KEY(e, 90, CTRL);
    CHECK_STR(widgets_get_value(g_w, e), "hello world foo", "back to the pre-paste value");
    /* word deletes */
    KEY(e, 269, 0);
    KEY(e, 259, CTRL);                                   /* Ctrl+Backspace */
    CHECK_STR(widgets_get_value(g_w, e), "hello world ", "Ctrl+Backspace deletes the previous word");
    KEY(e, 268, 0);
    KEY(e, 261, CTRL);                                   /* Ctrl+Delete */
    CHECK_STR(widgets_get_value(g_w, e), " world ", "Ctrl+Delete deletes to the word end");
    KEY(e, 90, CTRL);
    KEY(e, 90, CTRL);
    CHECK_STR(widgets_get_value(g_w, e), "hello world foo", "two undos restore both word deletes");
    /* typing coalesces into one undo step; a caret move starts a new one */
    KEY(e, 269, 0);
    widgets_char(g_w, e, 'a'); widgets_char(g_w, e, 'b'); widgets_char(g_w, e, 'c');
    KEY(e, 263, 0);
    widgets_char(g_w, e, 'X');
    KEY(e, 90, CTRL);
    CHECK_STR(widgets_get_value(g_w, e), "hello world fooabc", "undo removes only the typing after the caret move");
    KEY(e, 90, CTRL);
    CHECK_STR(widgets_get_value(g_w, e), "hello world foo", "second undo removes the coalesced 'abc'");
    /* Shift+Home / Shift+End, cut */
    KEY(e, 269, 0);
    KEY(e, 263, 0); KEY(e, 263, 0); KEY(e, 263, 0);      /* caret before "foo" */
    KEY(e, 269, SHIFT);                                  /* select "foo" */
    log_reset();
    KEY(e, 88, CTRL);                                    /* cut */
    CHECK_STR(widgets_get_value(g_w, e), "hello world ", "Shift+End + Ctrl+X cuts the tail");
    CHECK(log_count("input@settSearch") == 1, "cut fires input");
    KEY(e, 268, SHIFT);                                  /* Shift+Home from the end: everything */
    KEY(e, 86, CTRL);
    CHECK_STR(widgets_get_value(g_w, e), "foo", "Shift+Home selection replaced by the paste");
    KEY(e, 65, CTRL);
    KEY(e, 261, SHIFT);                                  /* Shift+Delete = cut */
    CHECK_STR(widgets_get_value(g_w, e), "", "Ctrl+A + Shift+Delete");
    KEY(e, 260, SHIFT);                                  /* Shift+Insert = paste */
    CHECK_STR(widgets_get_value(g_w, e), "foo", "Shift+Insert pastes");
    /* double / triple click */
    {
        TextFont *f = text_font(g_text, "gamefont", 16, 400, 0);
        float x = 10 + text_measure(f, "hello wo", 8, 0);
        widgets_set_value(g_w, e, "hello world-foo");
        widgets_set_time(g_w, 60000);
        widgets_mouse_down(g_w, e, x, 15, EV_BUTTON_LEFT, 0); widgets_mouse_up(g_w, e, x, 15, EV_BUTTON_LEFT);
        widgets_set_time(g_w, 60200);
        widgets_mouse_down(g_w, e, x, 15, EV_BUTTON_LEFT, 0); widgets_mouse_up(g_w, e, x, 15, EV_BUTTON_LEFT);
        KEY(e, 67, CTRL);
        widgets_set_time(g_w, 60400);
        widgets_mouse_down(g_w, e, x, 15, EV_BUTTON_LEFT, 0); widgets_mouse_up(g_w, e, x, 15, EV_BUTTON_LEFT);
        widgets_char(g_w, e, '[');
        KEY(e, 86, CTRL);
        CHECK_STR(widgets_get_value(g_w, e), "[world", "double-click copied the word, triple-click selected all");
        widgets_set_time(g_w, 70000);   /* a late click is a single click again */
        widgets_mouse_down(g_w, e, 12, 15, EV_BUTTON_LEFT, 0); widgets_mouse_up(g_w, e, 12, 15, EV_BUTTON_LEFT);
        widgets_char(g_w, e, '>');
        CHECK(strchr(widgets_get_value(g_w, e), '>') != NULL && strlen(widgets_get_value(g_w, e)) == 7, "single click after 500ms places the caret (%s)", widgets_get_value(g_w, e));
    }
    host_focus(NULL, 0);

    /* paste: newlines stripped, maxlength honoured */
    widgets_set_value(g_w, e, "12\n34567");   /* sanitised to "1234567" */
    host_focus(NULL, e);
    KEY(e, 65, CTRL);
    KEY(e, 67, CTRL);
    host_focus(NULL, m);
    widgets_set_value(g_w, m, "");
    log_reset();
    KEY(m, 86, CTRL);
    CHECK_STR(widgets_get_value(g_w, m), "1234", "paste truncated to maxlength=4");
    CHECK(log_count("input@t4") == 1, "paste fires one input");
    KEY(m, 86, CTRL);
    CHECK_STR(widgets_get_value(g_w, m), "1234", "paste into a full field changes nothing");
    host_focus(NULL, 0);
    {
        /* number: only number characters survive a paste */
        widgets_set_value(g_w, e, "a1.5b");
        host_focus(NULL, e); KEY(e, 65, CTRL); KEY(e, 67, CTRL);
        host_focus(NULL, n);
        widgets_set_value(g_w, n, "");
        KEY(n, 86, CTRL);
        CHECK_STR(widgets_get_value(g_w, n), "1.5", "number paste keeps digits and '.'");
        widgets_set_value(g_w, n, "200");
        host_focus(NULL, 0);
    }
    /* password: never copied */
    host_focus(NULL, e); widgets_set_value(g_w, e, "keep"); KEY(e, 65, CTRL); KEY(e, 67, CTRL); host_focus(NULL, 0);
    host_focus(NULL, p);
    KEY(p, 65, CTRL);
    KEY(p, 67, CTRL);
    KEY(p, 88, CTRL);
    CHECK_STR(widgets_get_value(g_w, p), "ab", "Ctrl+X does nothing in a password field");
    KEY(p, 262, 0);
    KEY(p, 86, CTRL);
    CHECK_STR(widgets_get_value(g_w, p), "abkeep", "clipboard still holds the last non-password copy");
    widgets_set_value(g_w, p, "ab");
    host_focus(NULL, 0);
    widgets_set_value(g_w, e, "");
    widgets_set_value(g_w, m, "");
}

static void test_number(void)
{
    HtmlElem e = by_id("slid_input_lagComp");   /* min 0 max 1 value 1 */
    HtmlElem f = by_id("slid_input_flickClamp"); /* min 0 max 500 value 200 */
    set_geom(e, 0, 0, 70, 34, 0, 0, 0, 0, 5, 10, 5, 10);
    set_geom(f, 0, 0, 70, 34, 0, 0, 0, 0, 5, 10, 5, 10);
    log_reset();
    host_focus(NULL, e);
    CHECK_STR(widgets_get_value(g_w, e), "1", "number initial");
    widgets_key(g_w, e, 265, 0, EV_PRESS, 0); /* Up: clamped at max */
    CHECK_STR(widgets_get_value(g_w, e), "1", "ArrowUp clamps to max");
    CHECK(g_nlog == 0, "no events when clamped unchanged");
    widgets_key(g_w, e, 264, 0, EV_PRESS, 0); /* Down */
    CHECK_STR(widgets_get_value(g_w, e), "0", "ArrowDown steps by 1");
    CHECK(log_count("input@slid_input_lagComp") == 1 && log_count("change@slid_input_lagComp") == 1, "ArrowDown fires input + change");
    widgets_key(g_w, e, 269, 0, EV_PRESS, 0);
    widgets_char(g_w, e, 'x');
    CHECK_STR(widgets_get_value(g_w, e), "0", "letters are filtered in number inputs");
    widgets_char(g_w, e, '.');
    widgets_char(g_w, e, '5');
    CHECK_STR(widgets_get_value(g_w, e), "0.5", "digits and '.' accepted");
    host_focus(NULL, 0);
    CHECK(log_count("change@slid_input_lagComp") == 2, "blur commits the typed number");

    log_reset();
    host_focus(NULL, f);
    widgets_key(g_w, f, 265, 0, EV_PRESS, 0);
    CHECK_STR(widgets_get_value(g_w, f), "201", "200 + ArrowUp = 201");
    host_focus(NULL, 0);
}

static void test_range_events(void)
{
    HtmlElem e = by_id("slid_lagComp"); /* min 0 max 1 step 0.1 value 1 */
    set_geom(e, 100, 50, 250, 15, 0, 0, 0, 0, 0, 0, 0, 0);
    log_reset();
    CHECK_STR(widgets_get_value(g_w, e), "1", "range initial");
    CHECK(widgets_mouse_down(g_w, e, 100 + 125, 57, EV_BUTTON_LEFT, 0) == EV_WIDGET_CAPTURE, "range press captures");
    CHECK_STR(widgets_get_value(g_w, e), "0.5", "press moves the thumb to the pointer");
    CHECK(log_count("input@slid_lagComp") == 1, "input on press");
    widgets_mouse_move(g_w, e, 100 + 125 + 2, 57, 1);
    CHECK(log_count("input@slid_lagComp") == 1, "no input while the snapped value is unchanged");
    widgets_mouse_move(g_w, e, 100 + 7.5f + 235 * 0.3f, 57, 1);
    CHECK_STR(widgets_get_value(g_w, e), "0.3", "drag to 30%%");
    CHECK(log_count("input@slid_lagComp") == 2, "input per change during drag");
    CHECK(log_count("change@slid_lagComp") == 0, "no change during drag");
    widgets_mouse_up(g_w, e, 100 + 7.5f + 235 * 0.3f, 57, EV_BUTTON_LEFT);
    CHECK(log_count("change@slid_lagComp") == 1, "change on release");
    CHECK_STR(dom_get_value(g_dom, e), "0.3", "attribute mirrors the range value");
    /* keyboard */
    log_reset();
    widgets_key(g_w, e, 262, 0, EV_PRESS, 0); /* Right */
    CHECK_STR(widgets_get_value(g_w, e), "0.4", "ArrowRight + step");
    CHECK(log_count("input@slid_lagComp") == 1 && log_count("change@slid_lagComp") == 1, "key step fires input + change");
    widgets_set_value(g_w, e, "7");
    CHECK_STR(widgets_get_value(g_w, e), "1", "set_value clamps to max");
    widgets_set_value(g_w, e, "abc");
    CHECK_STR(widgets_get_value(g_w, e), "0.5", "invalid value -> midpoint");
    widgets_set_value(g_w, e, "1");
    host_focus(NULL, 0);
}

/* #uiBase scale(0.5) at viewport (100, 50): the host maps the pointer into the
 * untransformed control so the 15px thumb and the 250px track stay in CSS px. */
static int host_local_scaled(void *u, HtmlElem e, float vx, float vy, WidgetGeom *out, float *lx, float *ly)
{
    (void)u; (void)e;
    *out = (WidgetGeom){0, 0, 250, 15, {0, 0, 0, 0}, {0, 0, 0, 0}};
    *lx = (vx - 100) / 0.5f;
    *ly = (vy - 50) / 0.5f;
    return 1;
}

static void test_scaled_range(void)
{
    WidgetsHost host = {0};
    Widgets *w2;
    HtmlElem e = by_id("slid_lagComp");
    host.computed = host_computed;
    host.pseudo_computed = host_pseudo;
    host.fire = host_fire;
    host.elem_local = host_local_scaled;
    w2 = widgets_create(g_dom, g_text, &host);
    widgets_set_value(w2, e, "1");
    /* viewport x of value 0.3 = 100 + (7.5 + 235 * 0.3) * 0.5 */
    widgets_mouse_down(w2, e, 100 + (7.5f + 235 * 0.3f) * 0.5f, 54, EV_BUTTON_LEFT, 0);
    CHECK_STR(widgets_get_value(w2, e), "0.3", "scaled range: pointer mapped through elem_local");
    widgets_mouse_up(w2, e, 0, 0, EV_BUTTON_LEFT);
    widgets_destroy(w2);
    widgets_set_value(g_w, e, "1");
}

static void test_select(void)
{
    HtmlElem lang = dom_query(g_dom, 0, "#setBod_local .settName:nth-child(2) select");
    float x, y, w, h;
    int idx;
    set_geom(lang, 785, 279, 109 + 12, 19 + 12, 0, 0, 0, 0, 6, 6, 6, 6);
    widgets_set_viewport(g_w, 1280, 720);
    log_reset();
    CHECK(widgets_select_index(g_w, lang) == 0, "English selected initially");
    CHECK_STR(widgets_get_value(g_w, lang), "en", "select value = option value");

    CHECK(widgets_open_select(g_w, lang, 790, 285) == (EV_WIDGET_CAPTURE | EV_WIDGET_CONSUMED), "open_select captures + consumes");
    CHECK(widgets_popup_owner(g_w) == lang, "popup owner");
    CHECK(g_focus == lang, "opening focuses the select");
    CHECK(widgets_overlay_rect(g_w, &x, &y, &w, &h), "overlay rect");
    CHECK_NEAR(x, 785, 0.01f, "popup left = select left");
    CHECK_NEAR(y, 279 + 31, 0.01f, "popup top = select bottom");
    CHECK_NEAR(w, 121, 0.01f, "popup width = select width (options fit)");
    CHECK_NEAR(h, 5 * 18 + 2, 0.01f, "popup height = 5 rows x (max(1.2em, lh) + 1) + border");

    widgets_pointer_moved(g_w, 800, y + 1 + 2 * 18 + 5);  /* row 2 = German */
    CHECK(widgets_mouse_down(g_w, lang, 800, y + 1 + 2 * 18 + 5, EV_BUTTON_LEFT, 0) & EV_WIDGET_CONSUMED, "press on a row is consumed");
    widgets_mouse_up(g_w, lang, 800, y + 1 + 2 * 18 + 5, EV_BUTTON_LEFT);
    CHECK(widgets_popup_owner(g_w) == 0, "popup closed after choosing");
    CHECK(widgets_select_index(g_w, lang) == 2, "German chosen (index %d)", widgets_select_index(g_w, lang));
    CHECK_STR(widgets_get_value(g_w, lang), "de", "value de");
    CHECK(log_count("input@?") == 1 && log_count("change@?") == 1, "choose fires input + change");
    CHECK(dom_has_attr(g_dom, dom_query(g_dom, lang, "option[value=de]"), "selected"), "selected attr moved");
    CHECK(!dom_has_attr(g_dom, dom_query(g_dom, lang, "option[value=en]"), "selected"), "old selected removed");

    /* keyboard while closed: Down changes immediately */
    log_reset();
    widgets_key(g_w, lang, 264, 0, EV_PRESS, 0);
    CHECK(widgets_select_index(g_w, lang) == 3, "ArrowDown while closed selects Korean");
    CHECK(log_count("change@?") == 1, "keyboard change fires change");
    /* keyboard while open: Up moves the hover, Enter commits */
    widgets_key(g_w, lang, EV_KEY_ENTER, 0, EV_PRESS, 0);
    CHECK(widgets_popup_owner(g_w) == lang, "Enter opens the popup");
    widgets_key(g_w, lang, 265, 0, EV_PRESS, 0);
    widgets_key(g_w, lang, 265, 0, EV_PRESS, 0);
    widgets_key(g_w, lang, EV_KEY_ENTER, 0, EV_PRESS, 0);
    CHECK(widgets_popup_owner(g_w) == 0 && widgets_select_index(g_w, lang) == 1, "Up Up Enter -> Spanish (%d)", widgets_select_index(g_w, lang));
    /* click outside closes without choosing */
    widgets_open_select(g_w, lang, 790, 285);
    CHECK(widgets_mouse_down(g_w, 0, 10, 10, EV_BUTTON_LEFT, 0) == EV_WIDGET_CONSUMED, "press outside the popup is swallowed");
    CHECK(widgets_popup_owner(g_w) == 0 && widgets_select_index(g_w, lang) == 1, "closed, selection kept");
    idx = widgets_select_index(g_w, lang);
    widgets_set_value(g_w, lang, "en");
    CHECK(widgets_select_index(g_w, lang) == 0 && idx == 1, "set_value picks by option value");
    host_focus(NULL, 0);
    /* popup flips above when it would overflow the viewport */
    set_geom(lang, 785, 700, 121, 31, 0, 0, 0, 0, 6, 6, 6, 6);
    widgets_open_select(g_w, lang, 790, 705);
    widgets_overlay_rect(g_w, &x, &y, &w, &h);
    CHECK_NEAR(y, 700 - 92, 0.01f, "popup flips above near the bottom");
    widgets_close_popup(g_w);
    host_focus(NULL, 0);
}

static void test_long_select(void)
{
    HtmlElem e = by_id("long");
    float x, y, w, h;
    float sb_before;
    widgets_set_viewport(g_w, 600, 400);
    widgets_set_time(g_w, 10000);
    set_geom(e, 50, 100, 120, 31, 0, 0, 0, 0, 6, 6, 6, 6);
    log_reset();
    CHECK(widgets_select_index(g_w, e) == 25, "Yankee preselected");

    widgets_open_select(g_w, e, 60, 110);
    widgets_mouse_up(g_w, e, 60, 110, EV_BUTTON_LEFT);   /* release on the control: stays open */
    CHECK(widgets_popup_owner(g_w) == e, "popup open after click");
    CHECK(widgets_overlay_rect(g_w, &x, &y, &w, &h), "rect");
    CHECK_NEAR(y, 131, 0.01f, "below the control (more room below)");
    CHECK(y + h <= 400.01f, "popup stays inside the viewport (bottom %g)", y + h);
    CHECK_NEAR(h, 269, 0.01f, "height shortened to the room below (400 - 131)");
    CHECK(w >= 120 + 15 - 0.01f || w >= 120, "width >= control width");

    /* the selected option is scrolled into view: row 25 bottom = 26*18 = 468, list_h = 267 */
    {
        DisplayList dl = {0};
        WidgetPaintArgs a = {&dl, dl_alloc, NULL, 1.0f, 0};
        int i, hl = -1, clip = -1;
        widgets_paint_overlay(g_w, &a);
        for (i = 0; i < dl.n; i++) {
            if (dl.items[i].op == DL_PUSH_CLIP_RECT && clip < 0) clip = i;
            if (dl.items[i].op == DL_RECT && dl.items[i].u.rect.color.r == 0x1e && dl.items[i].u.rect.color.b == 0xff) hl = i;
        }
        CHECK(clip >= 0 && hl >= 0, "popup clips its list and highlights the current row");
        if (hl >= 0) {
            const DlRect *r = &dl.items[hl].u.rect.rect;
            CHECK(fabsf(r->y + r->h - (132 + 267)) < 0.01f, "current row scrolled to the bottom edge (y=%g)", r->y);
        }
        CHECK(dl.n < 40, "bounded item count for 30 options (%d items)", dl.n);
        /* scrollbar track + thumb */
        {
            int track = -1, thumb = -1;
            for (i = 0; i < dl.n; i++) {
                if (dl.items[i].op == DL_RECT && dl.items[i].u.rect.color.r == 0xf1) track = i;
                if (dl.items[i].op == DL_RRECT && dl.items[i].u.rrect.color.r == 0xc1) thumb = i;
            }
            CHECK(track >= 0 && thumb >= 0, "scrollbar track + thumb painted");
            if (track >= 0) CHECK_NEAR(dl.items[track].u.rect.rect.w, 15, 0.01f, "15px scrollbar");
        }
        dl_free(&dl);
    }

    /* keyboard while open */
    widgets_key(g_w, e, 264, 0, EV_PRESS, 0);            /* Down -> 26 */
    widgets_key(g_w, e, EV_KEY_ENTER, 0, EV_PRESS, 0);
    CHECK(widgets_popup_owner(g_w) == 0 && widgets_select_index(g_w, e) == 26, "Down + Enter chooses Zulu (%d)", widgets_select_index(g_w, e));
    CHECK(log_count("change@long") == 1, "one change");
    widgets_key(g_w, e, EV_KEY_ENTER, 0, EV_PRESS, 0);  /* reopen */
    widgets_key(g_w, e, 268, 0, EV_PRESS, 0);            /* Home */
    widgets_key(g_w, e, 267, 0, EV_PRESS, 0);            /* PageDown: vis_rows (14) - 1 = 13 */
    CHECK(widgets_select_index(g_w, e) == 26, "highlight moves do not change the value while open");
    widgets_key(g_w, e, WK_SPACE_FOR_TEST, 0, EV_PRESS, 0);
    CHECK(widgets_popup_owner(g_w) == 0 && widgets_select_index(g_w, e) == 13, "Home, PageDown, Space -> Mike (%d)", widgets_select_index(g_w, e));
    widgets_key(g_w, e, 269, 0, EV_PRESS, 0);            /* End while closed: changes immediately */
    CHECK(widgets_select_index(g_w, e) == 29, "End while closed selects the last option");
    widgets_key(g_w, e, 266, 0, EV_PRESS, 0);            /* PageUp while closed: one step */
    CHECK(widgets_select_index(g_w, e) == 28, "PageUp while closed steps one option");

    /* wheel + scrollbar */
    widgets_key(g_w, e, 268, 0, EV_PRESS, 0);            /* Home (closed) -> 0 */
    widgets_open_select(g_w, e, 60, 110);
    widgets_mouse_up(g_w, e, 60, 110, EV_BUTTON_LEFT);
    widgets_overlay_rect(g_w, &x, &y, &w, &h);
    CHECK(widgets_scroll(g_w, 0, -1, x + 20, y + 50) == 1, "wheel over the popup is consumed");
    log_reset();
    widgets_mouse_down(g_w, 0, x + 20, y + 1 + 2, EV_BUTTON_LEFT, 0);
    widgets_mouse_up(g_w, 0, x + 20, y + 1 + 2, EV_BUTTON_LEFT);
    CHECK(widgets_select_index(g_w, e) == 2, "after one wheel click (53px) the first visible row is Charlie (%d)", widgets_select_index(g_w, e));
    widgets_open_select(g_w, e, 60, 110);
    widgets_mouse_up(g_w, e, 60, 110, EV_BUTTON_LEFT);
    widgets_overlay_rect(g_w, &x, &y, &w, &h);
    sb_before = y;
    /* drag the thumb to the bottom: the last row becomes visible */
    {
        float bx = x + w - 1 - 7.5f;
        CHECK(widgets_mouse_down(g_w, 0, bx, y + 1 + 5, EV_BUTTON_LEFT, 0) & EV_WIDGET_CAPTURE, "thumb press captures");
        widgets_mouse_move(g_w, 0, bx, y + h + 50, 1);
        widgets_mouse_up(g_w, 0, bx, y + h + 50, EV_BUTTON_LEFT);
        CHECK(widgets_popup_owner(g_w) == e, "thumb drag keeps the popup open");
        widgets_mouse_down(g_w, 0, x + 20, y + h - 1 - 3, EV_BUTTON_LEFT, 0);
        widgets_mouse_up(g_w, 0, x + 20, y + h - 1 - 3, EV_BUTTON_LEFT);
        CHECK(widgets_select_index(g_w, e) == 29, "thumb dragged to the end -> last row clickable (%d)", widgets_select_index(g_w, e));
    }
    (void)sb_before;
    /* wheel outside closes */
    widgets_open_select(g_w, e, 60, 110);
    CHECK(widgets_scroll(g_w, 0, -1, 500, 20) == 0 && widgets_popup_owner(g_w) == 0, "wheel outside closes the popup");

    /* flip above: control near the bottom with more room above */
    set_geom(e, 50, 330, 120, 31, 0, 0, 0, 0, 6, 6, 6, 6);
    widgets_open_select(g_w, e, 60, 340);
    widgets_overlay_rect(g_w, &x, &y, &w, &h);
    CHECK_NEAR(y + h, 330, 0.01f, "flipped above the control");
    CHECK(y >= -0.01f && h <= 330.01f, "clamped to the room above (y=%g h=%g)", y, h);
    CHECK_NEAR(h, 20 * 18 + 2 > 330 ? 330 : 362, 0.01f, "max 20 rows or the room above");
    widgets_close_popup(g_w);

    /* type-ahead (closed) */
    widgets_key(g_w, e, 268, 0, EV_PRESS, 0);            /* Alpha */
    widgets_set_time(g_w, 20000);
    widgets_char(g_w, e, 'c');
    CHECK(widgets_select_index(g_w, e) == 2, "'c' -> Charlie");
    widgets_set_time(g_w, 20200);
    widgets_char(g_w, e, 'c');
    CHECK(widgets_select_index(g_w, e) == 3, "'c' again cycles -> Chess");
    widgets_set_time(g_w, 22000);
    widgets_char(g_w, e, 'z');
    widgets_set_time(g_w, 22100);
    widgets_char(g_w, e, 'e');
    CHECK(widgets_select_index(g_w, e) == 27, "'ze' prefix -> Zebra (%d)", widgets_select_index(g_w, e));
    /* type-ahead while open moves the highlight only, and Space extends the prefix */
    widgets_open_select(g_w, e, 60, 340);
    widgets_set_time(g_w, 30000);
    widgets_char(g_w, e, 'z');
    widgets_set_time(g_w, 30100);
    widgets_key(g_w, e, WK_SPACE_FOR_TEST, 0, EV_PRESS, 0);
    CHECK(widgets_popup_owner(g_w) == e, "Space during type-ahead does not choose");
    widgets_close_popup(g_w);
    host_focus(NULL, 0);
}

static void test_preset_select_and_value(void)
{
    HtmlElem e = by_id("settingsPreset");     /* Default, Pro, Performance, Custom(selected) */
    EventSystem es;
    EventsHost eh = {0};
    widgets_set_viewport(g_w, 1280, 720);
    set_geom(e, 572.8f, 162.4f, 94, 21.3f, 0, 0, 0, 0, 3.8f, 3.8f, 3.8f, 3.8f);
    CHECK(widgets_select_index(g_w, e) == 3, "Custom preselected");
    widgets_set_time(g_w, 50000);
    widgets_char(g_w, e, 'p');
    CHECK(widgets_select_index(g_w, e) == 1, "'p' -> Pro");
    widgets_set_time(g_w, 50300);
    widgets_char(g_w, e, 'e');
    CHECK(widgets_select_index(g_w, e) == 2, "'pe' -> Performance");
    CHECK_STR(widgets_get_value(g_w, e), "3", "Performance value = 3");

    /* events.c elem_value: a select passes its chosen option's value as this.value */
    events_init(&es, g_dom, &eh);
    g_dispatch_value[0] = 0;
    events_fire(&es, e, "change", 0, 0);
    CHECK_STR(g_dispatch_name, "selectSettingPre", "onchange handler dispatched");
    CHECK_STR(g_dispatch_value, "3", "this.value of the select = chosen option value");
    {
        HtmlElem lang = dom_query(g_dom, 0, "#setBod_local .settName:nth-child(2) select");
        widgets_set_value(g_w, lang, "kr");
        events_fire(&es, lang, "change", 0, 0);
        CHECK_STR(g_dispatch_value, "kr", "language select this.value");
    }
    events_fini(&es);
    widgets_select_set_index(g_w, e, 3, 0);
}

static void test_checkbox(void)
{
    HtmlElem cb = by_id("cb"), slider = by_id("cbs");
    const CssComputed *st;
    log_reset();
    CHECK(!widgets_get_checked(g_w, cb), "unchecked initially");
    widgets_activate(g_w, cb);
    CHECK(widgets_get_checked(g_w, cb), "activate toggles on");
    CHECK(dom_get_checked(g_dom, cb), "checked state in the DOM");
    CHECK(log_count("input@cb") == 1 && log_count("change@cb") == 1, "checkbox activation fires input + change");
    CHECK(dom_matches(g_dom, slider, "input:checked + .slider"), "input:checked + .slider matches the sibling");
    restyle();
    st = cascade_elem_computed(g_dom, slider);
    CHECK(st && st->background_color.r == 0x21 && st->background_color.g == 0x96 && st->background_color.b == 0xf3,
          ".slider turns #2196f3 after the toggle (got #%02x%02x%02x)", st ? st->background_color.r : 0, st ? st->background_color.g : 0, st ? st->background_color.b : 0);
    widgets_activate(g_w, cb);
    CHECK(!widgets_get_checked(g_w, cb), "activate toggles off");
    restyle();
    st = cascade_elem_computed(g_dom, slider);
    CHECK(st && st->background_color.r == 0xa8, ".slider back to #a8a8a8");
    widgets_set_checked(g_w, cb, 1);
    CHECK(widgets_get_checked(g_w, cb), "set_checked");
    widgets_set_checked(g_w, cb, 0);
    /* an appearance:none / display:none checkbox paints nothing */
    {
        DisplayList dl = {0};
        WidgetPaintArgs a = {&dl, dl_alloc, NULL, 1.0f, 0};
        WidgetGeom g = {0, 0, 13, 13, {0, 0, 0, 0}, {0, 0, 0, 0}};
        const CssComputed *cst = cascade_elem_computed(g_dom, cb);
        CHECK(cst && cst->display == CSS_DISPLAY_NONE, ".switch input is display:none");
        widgets_paint(g_w, cb, &g, &a);
        CHECK(dl.n > 0, "a native (appearance:auto) checkbox paints its box when given a geometry (%d items)", dl.n);
        dl_free(&dl);
    }
}

static void test_color(void)
{
    HtmlElem c = by_id("tc"), cx = by_id("tcx");
    uint8_t r, g, b;
    float h, s, v, x, y, w, hh;
    CHECK_STR(widgets_get_value(g_w, c), "#eb5656", "color value lower-cased");
    CHECK_STR(widgets_get_value(g_w, cx), "#000000", "invalid color -> #000000");
    CHECK(widgets_parse_hex_color("#eb5656", &r, &g, &b) && r == 0xeb && g == 0x56 && b == 0x56, "parse hex");
    widgets_rgb_to_hsv(0xeb, 0x56, 0x56, &h, &s, &v);
    widgets_hsv_to_rgb(h, s, v, &r, &g, &b);
    CHECK(r == 0xeb && g == 0x56 && b == 0x56, "hsv round trip (%02x%02x%02x)", r, g, b);

    set_geom(c, 300, 100, 55, 24, 4, 4, 4, 4, 0, 0, 0, 0);
    log_reset();
    CHECK(widgets_mouse_down(g_w, c, 310, 110, EV_BUTTON_LEFT, 0) == EV_WIDGET_CONSUMED, "color press opens the picker");
    CHECK(widgets_popup_owner(g_w) == c, "picker owner");
    CHECK(widgets_overlay_rect(g_w, &x, &y, &w, &hh), "picker rect");
    CHECK_NEAR(y, 124, 0.01f, "picker below the input");
    /* press the SV square's top-right corner: s = 1, v = 1 -> pure hue */
    CHECK(widgets_mouse_down(g_w, 0, x + 8 + 179.9f, y + 8 + 0.1f, EV_BUTTON_LEFT, 0) & EV_WIDGET_CAPTURE, "picker drag captures");
    CHECK(log_count("input@tc") == 1, "input while picking");
    widgets_rgb_to_hsv(0xeb, 0x56, 0x56, &h, &s, &v);
    widgets_hsv_to_rgb(h, 1, 1, &r, &g, &b);
    {
        char want[8];
        snprintf(want, sizeof want, "#%02x%02x%02x", r, g, b);
        CHECK_STR(widgets_get_value(g_w, c), want, "SV corner -> saturated hue");
    }
    widgets_mouse_up(g_w, 0, x + 8 + 179.9f, y + 8, EV_BUTTON_LEFT);
    CHECK(log_count("change@tc") == 0, "no change until the picker closes");
    CHECK(widgets_mouse_down(g_w, 0, 5, 5, EV_BUTTON_LEFT, 0) == EV_WIDGET_CONSUMED, "outside press closes the picker");
    CHECK(log_count("change@tc") == 1, "change when the picker closes");
    widgets_set_value(g_w, c, "#EB5656");
    host_focus(NULL, 0);
}

/* ---- display list checks ---------------------------------------------------- */

static int find_op(const DisplayList *dl, int from, DlOp op)
{
    int i;
    for (i = from; i < dl->n; i++) if (dl->items[i].op == op) return i;
    return -1;
}

static void test_paint(void)
{
    DisplayList dl = {0};
    WidgetPaintArgs a = {&dl, dl_alloc, NULL, 1.0f, 0};
    HtmlElem range = by_id("slid_lagComp"), num = by_id("slid_input_lagComp"), color = by_id("tc");
    HtmlElem region = dom_query(g_dom, 0, "#setBod_local select"), search = by_id("settSearch");
    int i;

    /* range .sliderM 250x15, value 1: thumb 15x25 #2196f3 r4 at x = 235, centred (y = -5) */
    {
        WidgetGeom g = {0, 0, 250, 15, {0, 0, 0, 0}, {0, 0, 0, 0}};
        dl_clear(&dl);
        widgets_paint(g_w, range, &g, &a);
        i = find_op(&dl, 0, DL_RRECT);
        CHECK(i >= 0, "range emits the thumb rrect");
        if (i >= 0) {
            const DlItem *it = &dl.items[i];
            CHECK_NEAR(it->u.rrect.rect.x, 235, 0.01f, "thumb x at value=max");
            CHECK_NEAR(it->u.rrect.rect.y, -5, 0.01f, "thumb centred on the 15px track (overhangs 5px)");
            CHECK_NEAR(it->u.rrect.rect.w, 15, 0.01f, "thumb width from ::-webkit-slider-thumb");
            CHECK_NEAR(it->u.rrect.rect.h, 25, 0.01f, "thumb height from ::-webkit-slider-thumb");
            CHECK_NEAR(it->u.rrect.radii.tl, 4, 0.01f, "thumb radius 4");
            CHECK(it->u.rrect.color.r == 0x21 && it->u.rrect.color.g == 0x96 && it->u.rrect.color.b == 0xf3, "thumb colour #2196f3");
        }
        CHECK(find_op(&dl, 0, DL_RECT) < 0, "appearance:none range paints no native track (the CSS background is the track)");
    }
    /* number .sliderVal 70x34 padding 5 10: text clipped to the content box */
    {
        WidgetGeom g = {0, 0, 70, 34, {0, 0, 0, 0}, {5, 10, 5, 10}};
        dl_clear(&dl);
        widgets_paint(g_w, num, &g, &a);
        i = find_op(&dl, 0, DL_PUSH_CLIP_RECT);
        CHECK(i >= 0, "number input clips");
        if (i >= 0) {
            const DlRect *r = &dl.items[i].u.clip_rect.rect;
            CHECK(r->x == 10 && r->y == 5 && r->w == 50 && r->h == 24, "clip = content box (%g %g %g %g)", r->x, r->y, r->w, r->h);
        }
        CHECK(find_op(&dl, 0, DL_POP_CLIP) > i, "clip popped");
        CHECK(find_op(&dl, 0, DL_RECT) < 0, "no caret when unfocused");
    }
    /* color 55x24 border 4: swatch 47x16 at (4,4), no swatch border (author ::-webkit-color-swatch border:none) */
    {
        WidgetGeom g = {0, 0, 55, 24, {4, 4, 4, 4}, {0, 0, 0, 0}};
        dl_clear(&dl);
        widgets_paint(g_w, color, &g, &a);
        i = find_op(&dl, 0, DL_RECT);
        CHECK(i >= 0, "swatch rect");
        if (i >= 0) {
            const DlItem *it = &dl.items[i];
            CHECK(it->u.rect.rect.x == 4 && it->u.rect.rect.y == 4 && it->u.rect.rect.w == 47 && it->u.rect.rect.h == 16,
                  "swatch fills the padding box (capture 29x10 device px = 47x16)");
            CHECK(it->u.rect.color.r == 0xeb && it->u.rect.color.g == 0x56, "swatch colour = value");
        }
        CHECK(find_op(&dl, 0, DL_BORDER) < 0, "swatch border none");
    }
    /* select .inputGrey2 "Local" 72.5x29 padding 6: chevron 7x3.5 centred 7.5px from the right edge */
    {
        WidgetGeom g = {0, 0, 72.5f, 29, {0, 0, 0, 0}, {6, 6, 6, 6}};
        int t0, t1;
        dl_clear(&dl);
        widgets_paint(g_w, region, &g, &a);
        i = find_op(&dl, 0, DL_PUSH_CLIP_RECT);
        CHECK(i >= 0, "select clips its text");
        if (i >= 0) {
            const DlRect *r = &dl.items[i].u.clip_rect.rect;
            CHECK_NEAR(r->w, 72.5f - 12 - 16, 0.01f, "text clip excludes the 1px pad + 15px arrow box");
        }
        t0 = find_op(&dl, 0, DL_PUSH_TRANSFORM);
        t1 = t0 >= 0 ? find_op(&dl, t0 + 1, DL_PUSH_TRANSFORM) : -1;
        CHECK(t0 >= 0 && t1 >= 0, "chevron = two segments");
        if (t0 >= 0 && t1 >= 0) {
            const Mat2D *m0 = &dl.items[t0].u.transform.m, *m1 = &dl.items[t1].u.transform.m;
            CHECK_NEAR(m0->e, 72.5f - 7.5f - 3.5f, 0.01f, "chevron left arm starts 7.5 + 3.5 from the right edge");
            CHECK_NEAR(m0->f, 14.5f - 1.75f, 0.01f, "chevron top at centre - 1.75");
            CHECK_NEAR(m1->e, 72.5f - 7.5f, 0.01f, "chevron apex at the arrow box centre");
            CHECK_NEAR(m1->f, 14.5f + 1.75f, 0.01f, "chevron apex at centre + 1.75");
        }
    }
    /* text input: placeholder when empty, caret + blink when focused */
    {
        WidgetGeom g = {0, 0, 220, 33, {0, 0, 0, 0}, {6, 10, 5, 10}};
        int caret;
        widgets_set_value(g_w, search, "");
        dl_clear(&dl);
        widgets_paint(g_w, search, &g, &a);
        CHECK(find_op(&dl, 0, DL_RECT) < 0, "unfocused empty input: no caret");
        host_focus(NULL, search);
        a.now_ms = 1000;
        dl_clear(&dl);
        widgets_paint(g_w, search, &g, &a);
        caret = find_op(&dl, 0, DL_RECT);
        CHECK(caret >= 0, "focused input paints the caret");
        if (caret >= 0) {
            const DlRect *r = &dl.items[caret].u.rect.rect;
            CHECK(r->x == 10 && r->w == 1, "caret 1px at the content left (x=%g w=%g)", r->x, r->w);
            CHECK_NEAR(r->h, 22, 0.01f, "caret spans ascent + descent of 16px gamefont");
        }
        a.now_ms = 1000 + 600;
        dl_clear(&dl);
        widgets_paint(g_w, search, &g, &a);
        CHECK(find_op(&dl, 0, DL_RECT) < 0, "caret hidden 600ms later (500ms blink)");
        CHECK_NEAR(widgets_next_repaint_ms(g_w, 1600), 2000, 0.01f, "next blink toggle at +1000");
        a.now_ms = 1000 + 1100;
        dl_clear(&dl);
        widgets_paint(g_w, search, &g, &a);
        CHECK(find_op(&dl, 0, DL_RECT) >= 0, "caret visible again at +1100");
        widgets_char(g_w, search, 'a');
        a.now_ms = 1000 + 1600;
        dl_clear(&dl);
        widgets_paint(g_w, search, &g, &a);
        CHECK(find_op(&dl, 0, DL_RECT) >= 0, "typing resets the blink (visible right after an edit)");
        widgets_key(g_w, search, 65, 0, EV_PRESS, EV_MOD_CONTROL);
        dl_clear(&dl);
        widgets_paint(g_w, search, &g, &a);
        i = find_op(&dl, 0, DL_RECT);
        CHECK(i >= 0 && dl.items[i].u.rect.color.r == 0x33 && dl.items[i].u.rect.color.g == 0x90 && dl.items[i].u.rect.color.b == 0xff,
              "selection highlight #3390ff");
        host_focus(NULL, 0);
        widgets_set_value(g_w, search, "");
    }
    dl_free(&dl);
}

/* ---- GL render of the General tab control row --------------------------------- */

#ifdef KN_WIDGETS_GL
static void push_rrect(DisplayList *dl, float x, float y, float w, float h, float r, CssColor c)
{
    DlItem it;
    memset(&it, 0, sizeof it);
    it.op = DL_RRECT;
    it.u.rrect.rect = dl_rect(x, y, w, h);
    it.u.rrect.radii = dl_radii_all(r);
    it.u.rrect.color = c;
    dl_push(dl, &it);
}

static int write_ppm(const char *path, const unsigned char *rgba, int w, int h)
{
    FILE *f = fopen(path, "wb");
    int x, y;
    if (!f) return -1;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (y = h - 1; y >= 0; y--)
        for (x = 0; x < w; x++) fwrite(rgba + (y * w + x) * 4, 1, 3, f);
    fclose(f);
    return 0;
}

static GLFWwindow *g_win;

static void test_gl_render(void)
{
    GLFWwindow *win;
    GlBackend *b;
    DisplayList dl = {0};
    WidgetPaintArgs a = {&dl, dl_alloc, NULL, 1.0f, 1000};
    const int W = 480, H = 230;
    GLuint fbo = 0, tex = 0;
    unsigned char *img;
    HtmlElem range = by_id("slid_lagComp"), num = by_id("slid_input_lagComp"), color = by_id("tc");
    HtmlElem region = dom_query(g_dom, 0, "#setBod_local select"), search = by_id("settSearch");
    HtmlElem lang = dom_query(g_dom, 0, "#setBod_local .settName:nth-child(2) select");
    HtmlElem preset = by_id("settingsPreset"), url = by_id("turl"), aspect = by_id("ti");
    WidgetGeom g;

    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    win = glfwCreateWindow(64, 64, "test_widgets", NULL, NULL);
    if (!win) { printf("  (no GL context: skipping the render)\n"); return; }
    glfwMakeContextCurrent(win);
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) { printf("  (glad failed: skipping the render)\n"); return; }
    widgets_set_gl_ready(g_w, 1);
    b = glb_create();
    if (!b) { printf("  (glb_create failed: skipping the render)\n"); return; }

    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    glViewport(0, 0, W, H);
    glClearColor(1, 1, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    /* row 1: #settSearch (220x33 #e3e3e3 r4) + preset select (#e3e3e3 r4, 14px) */
    push_rrect(&dl, 10, 10, 220, 33, 4, dl_color(0xe3, 0xe3, 0xe3, 255));
    g = (WidgetGeom){10, 10, 220, 33, {0, 0, 0, 0}, {6, 10, 5, 10}};
    widgets_set_value(g_w, search, "");
    widgets_paint(g_w, search, &g, &a);
    push_rrect(&dl, 250, 10, 104, 34, 4, dl_color(0xe3, 0xe3, 0xe3, 255));
    g = (WidgetGeom){250, 10, 104, 34, {0, 0, 0, 0}, {6, 6, 6, 6}};
    widgets_paint(g_w, preset, &g, &a);
    /* row 2: Default Region "Local" select + Language select */
    push_rrect(&dl, 10, 60, 72.5f, 29, 4, dl_color(0xe3, 0xe3, 0xe3, 255));
    g = (WidgetGeom){10, 60, 72.5f, 29, {0, 0, 0, 0}, {6, 6, 6, 6}};
    widgets_paint(g_w, region, &g, &a);
    push_rrect(&dl, 100, 60, 121, 31, 4, dl_color(0xe3, 0xe3, 0xe3, 255));
    g = (WidgetGeom){100, 60, 121, 31, {0, 0, 0, 0}, {6, 6, 6, 6}};
    widgets_paint(g_w, lang, &g, &a);
    /* row 3: Lag Compensation range (250x15 #a8a8a8 r4) + .sliderVal number (70x34 #2196f3 r6) */
    push_rrect(&dl, 10, 115, 250, 15, 4, dl_color(0xa8, 0xa8, 0xa8, 255));
    g = (WidgetGeom){10, 115, 250, 15, {0, 0, 0, 0}, {0, 0, 0, 0}};
    widgets_set_value(g_w, range, "1");
    widgets_paint(g_w, range, &g, &a);
    push_rrect(&dl, 275, 105, 70, 34, 6, dl_color(0x21, 0x96, 0xf3, 255));
    g = (WidgetGeom){275, 105, 70, 34, {0, 0, 0, 0}, {5, 10, 5, 10}};
    widgets_set_value(g_w, num, "1");
    widgets_paint(g_w, num, &g, &a);
    /* row 4: colour input (55x24, 4px #2196f3 border r5) + url input with placeholder + aspect text */
    push_rrect(&dl, 10, 160, 55, 24, 5, dl_color(0x21, 0x96, 0xf3, 255));
    g = (WidgetGeom){10, 160, 55, 24, {4, 4, 4, 4}, {0, 0, 0, 0}};
    widgets_paint(g_w, color, &g, &a);
    push_rrect(&dl, 80, 160, 204, 29, 4, dl_color(0xe3, 0xe3, 0xe3, 255));
    g = (WidgetGeom){80, 160, 204, 29, {0, 0, 0, 0}, {6, 6, 6, 6}};
    widgets_paint(g_w, url, &g, &a);
    push_rrect(&dl, 10, 195, 204, 29, 4, dl_color(0xe3, 0xe3, 0xe3, 255));
    g = (WidgetGeom){10, 195, 204, 29, {0, 0, 0, 0}, {6, 6, 6, 6}};
    widgets_set_value(g_w, aspect, "1280x720");
    host_focus(NULL, aspect);
    widgets_key(g_w, aspect, 269, 0, EV_PRESS, 0);
    widgets_key(g_w, aspect, 263, 0, EV_PRESS, EV_MOD_SHIFT);
    widgets_key(g_w, aspect, 263, 0, EV_PRESS, EV_MOD_SHIFT);
    widgets_key(g_w, aspect, 263, 0, EV_PRESS, EV_MOD_SHIFT);
    widgets_paint(g_w, aspect, &g, &a);
    /* open the language popup last (overlay) */
    set_geom(lang, 100, 60, 121, 31, 0, 0, 0, 0, 6, 6, 6, 6);
    widgets_set_viewport(g_w, W, H);
    widgets_open_select(g_w, lang, 110, 70);
    widgets_pointer_moved(g_w, 110, 60 + 31 + 1 + 18 + 5);
    widgets_paint_overlay(g_w, &a);
    CHECK(dl.n > 20, "render list has items (%d)", dl.n);

    glb_begin(b, W, H, 1.0f);
    glb_submit(b, &dl);
    glb_end(b);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    img = (unsigned char *)malloc((size_t)W * H * 4);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, img);
    CHECK(write_ppm("/tmp/kn_widgets_row.ppm", img, W, H) == 0, "wrote /tmp/kn_widgets_row.ppm");
    /* thumb pixel: x = 10 + 235 + 7 = 252, y = 115 + 7 */
    {
        const unsigned char *p = img + ((H - 1 - 122) * W + 252) * 4;
        CHECK(abs(p[0] - 0x21) < 8 && abs(p[1] - 0x96) < 8 && abs(p[2] - 0xf3) < 8, "thumb pixel is #2196f3 (%d %d %d)", p[0], p[1], p[2]);
        p = img + ((H - 1 - 111) * W + 252) * 4; /* 4px above the track: still thumb (5px overhang) */
        CHECK(abs(p[0] - 0x21) < 8 && abs(p[2] - 0xf3) < 8, "thumb overhangs the track (%d %d %d)", p[0], p[1], p[2]);
        p = img + ((H - 1 - 172) * W + 30) * 4;
        CHECK(abs(p[0] - 0xeb) < 8 && abs(p[1] - 0x56) < 8, "swatch pixel is #eb5656 (%d %d %d)", p[0], p[1], p[2]);
    }
    free(img);
    widgets_close_popup(g_w);
    host_focus(NULL, 0);
    dl_free(&dl);
    glb_destroy(b);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &tex);
    g_win = win; /* destroyed after text_destroy (atlas texture needs the context) */
}
#endif

/* ------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    WidgetsHost host = {0};
    char fontpath[1024];
    if (argc > 1) g_root = argv[1];
    if (load_document() != 0) { fprintf(stderr, "document load failed\n"); return 1; }

    {
        char ui_dir[1024];
        snprintf(ui_dir, sizeof ui_dir, "%s/ui", g_root);
        g_text = text_create(ui_dir);
        CHECK(g_text != NULL, "text_create");
        if (g_text) {
            text_register_face(g_text, "gamefont", "css/fonts/font2.ttf", 400, TEXT_STYLE_NORMAL);
            text_register_face(g_text, "headerfont", "css/fonts/header.ttf", 400, TEXT_STYLE_NORMAL);
        }
    }
    snprintf(fontpath, sizeof fontpath, "%s/ui/css/fonts/font2.ttf", g_root);
    if (FT_Init_FreeType(&g_ft) == 0 && FT_New_Face(g_ft, fontpath, 0, &g_gamefont) != 0) g_gamefont = NULL;
    CHECK(g_gamefont != NULL, "gamefont loaded for char widths");

    host.computed = host_computed;
    host.pseudo_computed = host_pseudo;
    host.elem_geom = host_geom;
    host.invalidate = host_invalidate;
    host.fire = host_fire;
    host.set_capture = host_capture;
    host.release_capture = host_release;
    host.focus = host_focus;
    host.blur = host_blur;
    host.font_char_widths = host_char_widths;
    g_w = widgets_create(g_dom, g_text, &host);
    CHECK(g_w != NULL, "widgets_create");

#ifdef KN_WIDGETS_GL
    if (!glfwInit()) printf("  (glfwInit failed: no render)\n");
#endif

    test_kinds();
    test_intrinsic();
    test_range_math();
    test_text_editing();
    test_clipboard_editing();
    test_textarea();
    test_zoomed_controls();
    test_number();
    test_range_events();
    test_scaled_range();
    test_select();
    test_long_select();
    test_preset_select_and_value();
    test_checkbox();
    test_color();
    test_paint();
#ifdef KN_WIDGETS_GL
    test_gl_render();
#endif

    widgets_destroy(g_w);
    if (g_text) text_destroy(g_text);
#ifdef KN_WIDGETS_GL
    if (g_win) glfwDestroyWindow(g_win);
    glfwTerminate();
#endif
    if (g_gamefont) FT_Done_Face(g_gamefont);
    if (g_ft) FT_Done_FreeType(g_ft);
    css_sheet_destroy(g_main);
    css_sheet_destroy(g_ua);
    cascade_shutdown();
    dom_destroy(g_dom);
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}

/*
Throwaway CMakeLists.txt (e.g. /tmp/kn_widgets/CMakeLists.txt):

cmake_minimum_required(VERSION 3.16)
project(kn_widgets_test C)
set(CMAKE_C_STANDARD 11)
set(KN /home/user/src/KrunkNative)
foreach(dep ZLIB BZIP2 PNG HARFBUZZ BROTLI)
  set(FT_DISABLE_${dep} TRUE CACHE BOOL "" FORCE)
endforeach()
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
add_subdirectory(${KN}/client/lib/lexbor lexbor)
add_subdirectory(${KN}/client/lib/freetype freetype)
add_subdirectory(${KN}/client/lib/glad glad)
add_subdirectory(${KN}/client/lib/glfw glfw)
set(E ${KN}/client/src/html/engine)
add_executable(test_widgets ${E}/tests/test_widgets.c ${E}/widgets.c ${E}/dom.c ${E}/util.c
    ${E}/css_sheet.c ${E}/css_values.c ${E}/css_cascade.c ${E}/text.c
    ${E}/gl_backend.c ${KN}/client/src/shader.c ${KN}/shared/src/utils.c)
target_include_directories(test_widgets PRIVATE ${E} ${KN}/client/include ${KN}/shared/include)
target_compile_definitions(test_widgets PRIVATE KN_REPO_ROOT="${KN}" KN_WIDGETS_GL)
target_compile_options(test_widgets PRIVATE -g -O1 -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer)
target_link_options(test_widgets PRIVATE -fsanitize=address,undefined)
target_link_libraries(test_widgets PRIVATE lexbor freetype glad glfw m)
*/
