/*
 * test_layout.c - layout module tests (client/src/html/engine/layout*.c).
 *
 * Part 1: unit fixtures through the real DOM + cascade + text stack
 *         (margin collapsing, floats + clear, inline-block baselines, flex,
 *         grid (tracks, line placement, fr rows, self-alignment), table,
 *         absolute right/bottom + translate, abs static position after
 *         inline content, fixed inside a
 *         transform, sticky, rtl scrollbar, custom scrollbar width, ellipsis,
 *         text-transform, min-content, hit testing, <br>/nowrap, inline
 *         fragments, select intrinsic width, incremental boundary relayout).
 * (Part 2, the Krunker v7.1.4 menu against Chrome's rects, was retired with the v7
 *  page: ui/ is Krunker v10.0.2's page since ee90bd70.)
 *
 * Usage: test_layout [repo root]      exit code = number of failures
 */
#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "css_cascade.h"
#include "css_sheet.h"
#include "dom.h"
#include "image.h"
#include "layout.h"
#include "text.h"
#include "ua_css.h"

#ifndef KN_REPO_ROOT
#define KN_REPO_ROOT "."
#endif

static const char *g_root = KN_REPO_ROOT;
static int g_fail, g_pass;
static TextSystem *g_ts;
static LayoutCtx g_ctx;
static CssSheet *g_ua;

#define CHECK(cond, ...) do { if (cond) g_pass++; else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define NEAR(a, b, tol) (fabsf((float)(a) - (float)(b)) <= (tol))
#define CHECK_NEAR(a, b, tol, what) CHECK(NEAR(a, b, tol), "%s: got %.3f expected %.3f (tol %.2f)", what, (double)(a), (double)(b), (double)(tol))

/* ------------------------------------------------------------------------- */
/* host services                                                              */
/* ------------------------------------------------------------------------- */

static char *read_file(const char *rel, size_t *len)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", g_root, rel);
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(99); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "short read %s\n", path); exit(99); }
    buf[n] = 0;
    fclose(f);
    if (len) *len = (size_t)n;
    return buf;
}

/* PNG IHDR reader standing in for image_get() (which needs a GL context). */
static int png_size(void *user, const char *url, const char *base, float *w, float *h)
{
    (void)user; (void)base;
    char rel[512], path[1100];
    size_t n = strcspn(url, "?#");
    while (n && *url == '/') { url++; n--; }
    if (n >= sizeof(rel)) return 0;
    memcpy(rel, url, n); rel[n] = 0;
    snprintf(path, sizeof(path), "%s/ui/%s", g_root, rel);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    unsigned char hdr[24];
    size_t got = fread(hdr, 1, sizeof(hdr), f);
    fclose(f);
    if (got < 24 || memcmp(hdr, "\x89PNG", 4) != 0) return 0;
    *w = (float)((hdr[16] << 24) | (hdr[17] << 16) | (hdr[18] << 8) | hdr[19]);
    *h = (float)((hdr[20] << 24) | (hdr[21] << 16) | (hdr[22] << 8) | hdr[23]);
    return 1;
}

/* Stands in for image.c's resolver (layout_build.c's content: url() on <img>;
 * image.c needs GL and the asset tree): the URL as-is, query/fragment cut. */
int image_resolve_url(const char *url, const char *base_url, char *out, size_t cap)
{
    (void)base_url;
    size_t n = strcspn(url, "?#");
    while (n && *url == '/') { url++; n--; }
    if (n >= cap) return 0;
    memcpy(out, url, n);
    out[n] = 0;
    return 1;
}

/* ::-webkit-scrollbar widths from the inventory: 12px global, 8px #chatList. */
static float sb_width(void *user, lxb_dom_node_t *node, float def)
{
    (void)user;
    if (node && node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
        size_t len;
        const lxb_char_t *id = lxb_dom_element_id(lxb_dom_interface_element(node), &len);
        if (id && len == 8 && memcmp(id, "chatList", 8) == 0) return 8.0f;
    }
    return def;
}

/* ------------------------------------------------------------------------- */
/* fixtures                                                                   */
/* ------------------------------------------------------------------------- */

typedef struct Fx {
    Dom *dom;
    CssSheet *sheets[8];
    int ns;
    LayoutTree *tree;
    float vw, vh;
} Fx;

static void fx_layout(Fx *fx)
{
    cascade_restyle(fx->dom, g_ua, fx->sheets, fx->ns);
    if (fx->tree) layout_tree_free(fx->tree);
    fx->tree = layout_build(fx->dom, &g_ctx);
    layout_run(fx->tree, fx->vw, fx->vh);
}

static Fx *fx_html(const char *body, const char *css)
{
    static const char *base_css = "html, body { margin: 0; padding: 0; } * { font-family: gamefont; }\n";
    Fx *fx = calloc(1, sizeof(Fx));
    fx->vw = 800; fx->vh = 600;
    fx->dom = dom_create();
    size_t hl = strlen(body) + 64;
    char *html = malloc(hl);
    if (!html) exit(99);
    snprintf(html, hl, "<!doctype html><html><body>%s</body></html>", body);
    dom_load_html(fx->dom, html, strlen(html));
    free(html);
    fx->sheets[fx->ns++] = css_sheet_load(base_css, strlen(base_css), "base");
    if (css) fx->sheets[fx->ns++] = css_sheet_load(css, strlen(css), "fixture");
    fx_layout(fx);
    return fx;
}

static void fx_free(Fx *fx)
{
    if (!fx) return;
    layout_tree_free(fx->tree);
    dom_destroy(fx->dom);
    for (int i = 0; i < fx->ns; i++) css_sheet_destroy(fx->sheets[i]);
    free(fx);
}

static Box *bx(Fx *fx, const char *id)
{
    HtmlElem e = dom_by_id(fx->dom, id);
    ElemData *ed = e ? dom_elem_data(fx->dom, e) : NULL;
    Box *b = ed ? (Box *)ed->box : NULL;
    if (!b) { printf("FAIL: no box for #%s\n", id); g_fail++; }
    return b;
}

static void dump_id(Fx *fx, const char *id)
{
    const char *want = getenv("KN_DUMP_ID");
    if (!want || strcmp(want, id) != 0) return;
    HtmlElem e = dom_by_id(fx->dom, id);
    ElemData *ed = e ? dom_elem_data(fx->dom, e) : NULL;
    if (!ed || !ed->box) return;
    KnStrBuf sb = { 0 };
    layout_dump((Box *)ed->box, &sb);
    printf("--- dump #%s\n%s", id, sb.data ? sb.data : "");
    kn_sb_free(&sb);
}

static HtmlRect rc(Fx *fx, const char *id)
{
    HtmlRect r = { 0, 0, 0, 0 };
    dump_id(fx, id);
    Box *b = bx(fx, id);
    if (b) layout_box_rect_in_viewport(b, &r);
    return r;
}

static const char *box_id(const Box *b)
{
    static char buf[64];
    buf[0] = 0;
    if (b && b->node && b->node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
        size_t len;
        const lxb_char_t *id = lxb_dom_element_id(lxb_dom_interface_element(b->node), &len);
        if (id) { snprintf(buf, sizeof(buf), "%.*s", (int)len, (const char *)id); }
    }
    return buf;
}

static float tw(const char *family, float size, const char *s)
{
    TextFont *f = text_font(g_ts, family, size, 400, 0);
    return f ? text_measure(f, s, strlen(s), 0) : 0;
}

static const TextRun *first_run(const Box *b)
{
    for (const Box *c = b->first_child; c; c = c->next) {
        if (c->type == BOX_TEXT && c->run_count) return &c->runs[0];
        const TextRun *r = first_run(c);
        if (r) return r;
    }
    return NULL;
}

static int count_lines(const Box *b)
{
    if (b->line_count) return b->line_count;
    for (const Box *c = b->first_child; c; c = c->next) if (c->line_count) return c->line_count;
    return 0;
}

/* ------------------------------------------------------------------------- */
/* unit cases                                                                 */
/* ------------------------------------------------------------------------- */

static void t_margin_collapse(void)
{
    Fx *fx = fx_html(
        "<div id=a style='height:10px;margin-bottom:20px'></div>"
        "<div id=b style='height:10px;margin-top:30px'></div>"
        "<div id=p style='margin-top:10px'><div id=c style='margin-top:25px;height:5px'></div></div>"
        "<div id=e style='margin-top:8px;margin-bottom:12px'></div>"
        "<div id=f style='height:5px;margin-top:-4px'></div>"
        "<div id=g style='overflow:hidden;margin-top:10px'><div id=gc style='margin-top:7px;height:3px'></div></div>", NULL);
    CHECK_NEAR(rc(fx, "a").y, 0, 0.01, "collapse: a.y");
    CHECK_NEAR(rc(fx, "b").y, 40, 0.01, "collapse: siblings max(20,30)");
    CHECK_NEAR(rc(fx, "p").y, 75, 0.01, "collapse: parent/child top margins");
    CHECK_NEAR(rc(fx, "c").y, 75, 0.01, "collapse: child at parent top");
    CHECK_NEAR(rc(fx, "p").h, 5, 0.01, "collapse: parent height");
    CHECK_NEAR(rc(fx, "f").y, 88, 0.01, "collapse: through empty block with negative margin (12 + -4)");
    CHECK_NEAR(rc(fx, "g").y, 103, 0.01, "collapse: BFC root after f");
    CHECK_NEAR(rc(fx, "gc").y, 110, 0.01, "collapse: no collapse into a BFC root");
    fx_free(fx);
}

static void t_floats(void)
{
    Fx *fx = fx_html(
        "<div id=w style='width:300px'>"
        "<div id=fl style='float:left;width:100px;height:50px'></div>"
        "<div id=fr style='float:right;width:80px;height:30px'></div>"
        "<span id=t>hello</span>"
        "<div id=cl style='clear:both;height:10px'></div></div>"
        "<div id=w2 style='width:200px'><div id=fl2 style='float:left;width:150px;height:20px'></div>"
        "<div id=bfc style='overflow:hidden;width:100px;height:10px'></div></div>", NULL);
    HtmlRect fl = rc(fx, "fl"), fr = rc(fx, "fr"), cl = rc(fx, "cl"), w = rc(fx, "w");
    CHECK_NEAR(fl.x, 0, 0.01, "float left x");
    CHECK_NEAR(fr.x, 220, 0.01, "float right x");
    Box *t = bx(fx, "t");
    const TextRun *r = t ? first_run(t) : NULL;
    CHECK(r != NULL, "float: text run exists");
    if (r) CHECK_NEAR(r->x, 100, 0.01, "line box avoids the left float");
    CHECK_NEAR(cl.y, 50, 0.01, "clear: both below floats");
    CHECK_NEAR(w.h, 60, 0.01, "container height includes cleared block");
    CHECK_NEAR(rc(fx, "bfc").y, rc(fx, "w2").y + 20, 0.01, "BFC root that does not fit beside a float moves below it");
    fx_free(fx);
}

static void t_line_below_floats(void)
{
    /* CSS 2.1 9.5: a line box shortened by floats that cannot hold its first piece of
     * content moves below them (Blink: the next layout opportunity). The HUD's #topRight:
     * the stat counters and the kill feed stack under the float:right leaderboard. */
    Fx *fx = fx_html(
        "<div id=tr style='position:absolute;right:0;top:0;text-align:right'>"
        "<div id=lb style='float:right;width:350px;height:100px;margin-bottom:10px'></div>"
        "<div></div>"
        "<div id=cnt><div id=k1 style='display:inline-block;vertical-align:top;width:60px;height:40px'></div>"
        "<div id=k2 style='display:inline-block;vertical-align:top;width:60px;height:40px'></div></div>"
        "<div></div>"
        "<div id=feed style='display:inline-block;vertical-align:top;width:120px;height:30px'></div></div>"
        "<div id=w3 style='width:200px'><div style='float:left;width:200px;height:20px'></div><span id=t3>word</span></div>"
        "<div id=w4 style='width:200px;white-space:nowrap'><div style='float:left;width:200px;height:20px'></div><span id=t4>word</span></div>"
        "<div id=w5 style='width:300px'><div style='float:left;width:100px;height:20px'></div>"
        "<div id=ib5 style='display:inline-block;vertical-align:top;width:150px;height:10px'></div></div>"
        "<div id=w6 style='width:300px'><div style='float:left;width:100px;height:20px'></div>"
        "<div id=ib6 style='display:inline-block;vertical-align:top;width:250px;height:10px'></div></div>", NULL);
    HtmlRect tr = rc(fx, "tr"), lb = rc(fx, "lb"), k1 = rc(fx, "k1"), k2 = rc(fx, "k2"), feed = rc(fx, "feed");
    CHECK_NEAR(tr.w, 350, 0.01, "shrink-to-fit width: the float");
    CHECK_NEAR(lb.x, 450, 0.01, "float right at the container's right edge");
    CHECK_NEAR(lb.y, 0, 0.01, "float at the top");
    CHECK_NEAR(k1.y, 110, 0.01, "first inline-block line moves under the float's margin box");
    CHECK_NEAR(k2.y, 110, 0.01, "second inline-block on the same line");
    CHECK_NEAR(k2.x, 740, 0.01, "line right-aligned on the full width under the float");
    CHECK_NEAR(k1.x, 680, 0.01, "first inline-block before the second");
    CHECK_NEAR(feed.y, 150, 0.01, "next line under the counters");
    CHECK_NEAR(feed.x, 680, 0.01, "next line right-aligned");
    CHECK_NEAR(tr.h, 180, 0.01, "container height covers the stacked lines");
    Box *w3 = bx(fx, "w3"), *w4 = bx(fx, "w4");
    CHECK(w3 && w3->line_count == 1, "text after a full-width float: one line");
    if (w3 && w3->line_count) CHECK_NEAR(w3->lines[0].y - w3->y, 20, 0.01, "text line moves below a full-width float");
    CHECK(w4 && w4->line_count == 1, "nowrap text after a full-width float: one line");
    if (w4 && w4->line_count) CHECK_NEAR(w4->lines[0].y - w4->y, 0, 0.01, "white-space: nowrap keeps the line beside the float");
    HtmlRect w5 = rc(fx, "w5"), ib5 = rc(fx, "ib5"), w6 = rc(fx, "w6"), ib6 = rc(fx, "ib6");
    CHECK_NEAR(ib5.x, 100, 0.01, "an item that fits beside the float stays there");
    CHECK_NEAR(ib5.y, w5.y, 0.01, "an item that fits beside the float keeps the line's top");
    CHECK_NEAR(ib6.y, w6.y + 20, 0.01, "an item wider than the room beside the float moves below it");
    CHECK_NEAR(ib6.x, w6.x, 0.01, "the moved line starts at the content edge");
    fx_free(fx);
}

static void t_inline_block_baseline(void)
{
    Fx *fx = fx_html(
        "<div id=lb style='font-size:20px'><span id=tx>Ab</span>"
        "<div id=ib style='display:inline-block;width:30px;height:40px'></div>"
        "<div id=ibm style='display:inline-block;width:10px;height:10px;vertical-align:top'></div>"
        "<div id=ibt style='display:inline-block;font-size:10px'>x</div></div>", NULL);
    Box *tx = bx(fx, "tx");
    const TextRun *r = tx ? first_run(tx) : NULL;
    TextFont *f = text_font(g_ts, "gamefont", 20, 400, 0);
    const TextMetrics *m = text_font_metrics(f);
    HtmlRect ib = rc(fx, "ib"), ibm = rc(fx, "ibm"), lb = rc(fx, "lb"), ibt = rc(fx, "ibt");
    CHECK(r != NULL, "inline-block: text run");
    if (r) {
        CHECK_NEAR(ib.y + ib.h, r->y, 0.01, "empty inline-block: bottom margin edge on the baseline");
        CHECK_NEAR(r->y, 40, 0.01, "baseline pushed down by the 40px inline-block");
        Box *bt = bx(fx, "ibt");
        float bl = bt ? bt->y + bt->first_baseline : 0;
        CHECK_NEAR(bl, r->y, 0.01, "inline-block with text: its last line baseline on the parent baseline");
    }
    CHECK_NEAR(ibm.y, 0, 0.01, "vertical-align: top");
    CHECK_NEAR(lb.h, 40 + m->descent, 0.01, "line height = baseline + strut descent");
    (void)ibt;
    fx_free(fx);
}

static void t_flex(void)
{
    Fx *fx = fx_html(
        "<div id=fx style='display:flex;justify-content:space-between;align-items:center;width:400px;height:100px'>"
        "<div id=f1 style='width:50px;height:20px'></div><div id=f2 style='width:50px;height:40px'></div>"
        "<div id=f3 style='width:50px;height:20px'></div></div>"
        "<div id=fc style='display:flex;flex-direction:column;height:300px'>"
        "<div id=g1 style='flex-grow:1'></div><div id=g2 style='height:100px'></div></div>"
        "<div id=fa style='display:flex;justify-content:space-around;width:300px'>"
        "<div id=a1 style='width:50px;height:5px'></div><div id=a2 style='width:50px;height:5px'></div></div>"
        "<div id=fb style='display:flex;width:300px'><div id=b1 style='flex-basis:200px'>x</div>"
        "<div id=b2 style='flex-grow:1'>y</div></div>"
        "<div id=fw style='display:flex;flex-wrap:wrap;width:120px'>"
        "<div id=w1 style='width:50px;height:10px'></div><div id=w2 style='width:50px;height:10px'></div>"
        "<div id=w3 style='width:50px;height:10px'></div></div>", NULL);
    HtmlRect fxr = rc(fx, "fx");
    CHECK_NEAR(rc(fx, "f1").x, fxr.x + 0, 0.01, "space-between first");
    CHECK_NEAR(rc(fx, "f2").x, fxr.x + 175, 0.01, "space-between middle");
    CHECK_NEAR(rc(fx, "f3").x, fxr.x + 350, 0.01, "space-between last");
    CHECK_NEAR(rc(fx, "f1").y - fxr.y, 40, 0.01, "align-items center (20px item)");
    CHECK_NEAR(rc(fx, "f2").y - fxr.y, 30, 0.01, "align-items center (40px item)");
    HtmlRect fc = rc(fx, "fc"), g1 = rc(fx, "g1"), g2 = rc(fx, "g2");
    CHECK_NEAR(g1.h, 200, 0.01, "column flex-grow");
    CHECK_NEAR(g2.y - fc.y, 200, 0.01, "column second item");
    CHECK_NEAR(g1.w, 800, 0.01, "column stretch width");
    HtmlRect fa = rc(fx, "fa");
    CHECK_NEAR(rc(fx, "a1").x - fa.x, 50, 0.01, "space-around first");
    CHECK_NEAR(rc(fx, "a2").x - fa.x, 200, 0.01, "space-around second");
    CHECK_NEAR(rc(fx, "b1").w, 200, 0.01, "flex-basis 200px");
    CHECK_NEAR(rc(fx, "b2").w, 100, 0.01, "flex-grow takes the rest");
    HtmlRect fw = rc(fx, "fw");
    CHECK_NEAR(rc(fx, "w3").y - fw.y, 10, 0.01, "wrap: third item on the second line");
    CHECK_NEAR(fw.h, 20, 0.01, "wrap: container height = two lines");
    fx_free(fx);
}

/* flex: 1 (flex-basis 0%) in an auto-height column is content-sized (v10's generic table in a
 * block .table-section); align-content on wrapped lines (v10's leaderboard category grid) */
static void t_flex_pct_basis_align_content(void)
{
    Fx *fx = fx_html(
        "<div id=sec><div id=tw style='display:flex;flex-direction:column'>"
        "<div id=tc style='flex:1;min-height:0'><div style='height:70px'></div></div></div></div>"
        "<div id=tz style='display:flex;flex-direction:column'>"
        "<div id=tzc style='flex:1 1 0px;min-height:0'><div style='height:70px'></div></div></div>"
        "<div id=ac style='display:flex;flex-wrap:wrap;align-content:center;width:100px;height:200px'>"
        "<div id=c1 style='width:60px;height:30px'></div><div id=c2 style='width:60px;height:30px'></div></div>"
        "<div id=ab style='display:flex;flex-wrap:wrap;align-content:space-between;width:100px;height:200px'>"
        "<div id=d1 style='width:60px;height:30px'></div><div id=d2 style='width:60px;height:30px'></div></div>", NULL);
    CHECK_NEAR(rc(fx, "tc").h, 70, 0.01, "flex: 1 in an auto-height column: content height (0% basis is indefinite)");
    CHECK_NEAR(rc(fx, "tw").h, 70, 0.01, "its column container: content height");
    CHECK_NEAR(rc(fx, "tzc").h, 0, 0.01, "flex-basis 0px stays definite");
    HtmlRect ac = rc(fx, "ac");
    CHECK_NEAR(rc(fx, "c1").y - ac.y, 70, 0.01, "align-content center: first line");
    CHECK_NEAR(rc(fx, "c2").y - ac.y, 100, 0.01, "align-content center: second line");
    CHECK_NEAR(rc(fx, "c1").h, 30, 0.01, "align-content center: lines not stretched");
    HtmlRect ab = rc(fx, "ab");
    CHECK_NEAR(rc(fx, "d2").y - ab.y, 170, 0.01, "align-content space-between: last line at the end");
    fx_free(fx);
}

static void t_grid(void)
{
    char html[4096];
    int o = snprintf(html, sizeof(html), "<div id=gr style='display:grid;grid-template-columns:auto auto auto auto auto auto;"
                     "column-gap:10px;row-gap:10px;width:650px'>");
    for (int i = 0; i < 12; i++)
        o += snprintf(html + o, sizeof(html) - (size_t)o, "<div id=g%d><span style='display:inline-block;width:40px;height:10px'></span></div>", i);
    snprintf(html + o, sizeof(html) - (size_t)o, "</div><div id=gp style='display:grid;grid-template-columns:40px 50px auto 50px 40px;width:400px'>"
             "<div id=p0>a</div><div id=p1>b</div><div id=p2>c</div><div id=p3>d</div><div id=p4>e</div></div>");
    Fx *fx = fx_html(html, NULL);
    HtmlRect gr = rc(fx, "gr");
    char id[16];
    for (int i = 0; i < 6; i++) {
        snprintf(id, sizeof(id), "g%d", i);
        HtmlRect r = rc(fx, id);
        CHECK_NEAR(r.x - gr.x, i * 110.0f, 0.01, "grid: 6 auto columns x");
        CHECK_NEAR(r.w, 100, 0.01, "grid: auto column width (650-50)/6");
    }
    HtmlRect g0 = rc(fx, "g0"), g6 = rc(fx, "g6");
    CHECK_NEAR(g6.y, g0.y + g0.h + 10, 0.01, "grid: row-gap");
    CHECK_NEAR(g6.x, g0.x, 0.01, "grid: auto-placement wraps to column 0");
    CHECK_NEAR(rc(fx, "p2").x - rc(fx, "gp").x, 90, 0.01, "grid: px tracks then auto");
    CHECK_NEAR(rc(fx, "p2").w, 220, 0.01, "grid: auto track takes the free space");
    fx_free(fx);
}

/* calc() percentages, fit-content(), fr / minmax(), repeat() and stretched
 * form controls; expected values measured in Chrome 152 (same markup). */
static void t_grid_tracks(void)
{
    Fx *fx = fx_html(
        "<div style='width:300px'><div id=v3 style='width:100%;display:grid;grid-template-columns:"
        "calc((100% / 3) - 2px) calc((100% / 3) - 2px) calc((100% / 3) - 2px);column-gap:3px'>"
        "<input id=v0 type=number><input id=v1 type=number><input id=v2 type=number></div></div>"
        "<div id=pb style='width:300px;display:grid;grid-template-columns:fit-content(0) fit-content(100%) auto'>"
        "<span id=pb0>Lock it</span><span id=pb1>Label</span><span id=pb2>X</span></div>"
        "<div id=pl style='width:300px;display:grid;grid-template-columns:fit-content(0) fit-content(100%) auto'>"
        "<span id=pl0>Lock it</span><span id=pl1>A long label text that wraps around the box twice or more often</span><span id=pl2>X</span></div>"
        "<div id=fr style='width:310px;display:grid;grid-template-columns:1fr 2fr 50px;column-gap:5px'>"
        "<div id=fr0></div><div id=fr1></div><div id=fr2></div></div>"
        "<div id=mm style='width:300px;display:grid;grid-template-columns:repeat(3, minmax(0, 1fr))'>"
        "<div id=mm0>aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa</div><div id=mm1></div><div id=mm2></div></div>"
        "<div style='display:inline-block'><div id=ib style='display:grid;grid-template-columns:fit-content(0) fit-content(100%) auto'>"
        "<span>Lock it</span><span id=ib1>Label</span><span>X</span></div></div>", NULL);
    HtmlRect v3 = rc(fx, "v3");
    const float track = 100.0f - 2.0f;
    for (int i = 0; i < 3; i++) {
        char id[8];
        snprintf(id, sizeof(id), "v%d", i);
        HtmlRect r = rc(fx, id);
        CHECK_NEAR(r.x - v3.x, i * (track + 3), 0.01, "grid: calc(%) track x");
        CHECK_NEAR(r.w, track, 0.01, "grid: inputs stretch to their calc(%) track");
    }
    float lock_min = tw("gamefont", 16, "Lock"), label = tw("gamefont", 16, "Label");
    HtmlRect pb = rc(fx, "pb");
    CHECK_NEAR(rc(fx, "pb0").w, lock_min, 0.5, "grid: fit-content(0) = min-content");
    CHECK_NEAR(rc(fx, "pb1").x - pb.x, rc(fx, "pb0").w, 0.01, "grid: fit-content track x");
    CHECK_NEAR(rc(fx, "pb1").w, label, 0.5, "grid: fit-content(100%) = max-content when it fits");
    CHECK_NEAR(rc(fx, "pb2").x + rc(fx, "pb2").w, pb.x + 300, 0.01, "grid: auto track stretches, fit-content tracks do not");
    HtmlRect pl = rc(fx, "pl");
    float x_w = tw("gamefont", 16, "X");
    CHECK_NEAR(rc(fx, "pl2").w, x_w, 0.5, "grid: long fit-content label leaves the auto track at max-content");
    CHECK_NEAR(rc(fx, "pl1").w, 300 - rc(fx, "pl0").w - rc(fx, "pl2").w, 0.5, "grid: long fit-content label takes the rest");
    CHECK(rc(fx, "pl1").h > rc(fx, "pb1").h + 1, "grid: the long label wraps");
    (void)pl;
    CHECK_NEAR(rc(fx, "fr0").w, 250.0f / 3, 0.01, "grid: 1fr of (310-50-10)/3");
    CHECK_NEAR(rc(fx, "fr1").w, 500.0f / 3, 0.01, "grid: 2fr");
    CHECK_NEAR(rc(fx, "fr2").x - rc(fx, "fr").x, 260, 0.01, "grid: fixed track after the fr tracks");
    CHECK_NEAR(rc(fx, "mm0").w, 100, 0.01, "grid: repeat(3, minmax(0, 1fr)) ignores min-content");
    CHECK_NEAR(rc(fx, "mm2").x - rc(fx, "mm").x, 200, 0.01, "grid: repeat() expands to 3 tracks");
    HtmlRect ib = rc(fx, "ib");
    CHECK_NEAR(ib.w, lock_min + label + x_w, 1.0, "grid: shrink-to-fit width from fit-content tracks");
    fx_free(fx);
}

/* Line-based placement, fr rows in a definite height, spanning items,
 * sparse auto-placement, self-alignment and auto margins.  Expected values
 * from Chrome (getBoundingClientRect of the same markup). */
static void t_grid_placement(void)
{
    Fx *fx = fx_html(
        /* pickr nano .pcr-selection: 5fr row fills the definite height, 2/2/2/2 = row 2 span 1 */
        "<div id=sel style='display:grid;grid-gap:10px;grid-template-columns:1fr 4fr;grid-template-rows:5fr auto auto;"
        "align-items:center;height:168px;width:300px'>"
        "<div id=pv style='grid-area:2/1/4/1;height:100%'><div style='height:32px'></div></div>"
        "<div id=pa style='grid-area:1/1/2/3;height:100%'></div>"
        "<div id=hu style='grid-area:2/2/2/2;height:8px'></div>"
        "<div id=op style='grid-area:3/2/3/2;height:8px'></div></div>"
        "<div id=g2 style='display:grid;grid-template-columns:50px 50px 50px;grid-template-rows:20px 30px;width:200px;column-gap:4px'>"
        "<div id=a style='grid-column:2 / span 2'></div><div id=b style='grid-row:2;grid-column:3'></div><div id=c></div>"
        "<div id=d style='grid-area:2/1'></div><div id=e></div><div id=f style='grid-column:-2'></div>"
        "<div id=h style='grid-row:span 2 / 7'></div></div>"
        "<div id=al style='display:grid;grid-template-columns:100px 100px;grid-template-rows:40px 40px;width:300px'>"
        "<div id=al0 style='justify-self:center;width:30px;height:20px'></div>"
        "<div id=al1 style='justify-self:end;align-self:end;width:30px;height:20px'></div>"
        "<div id=al2 style='place-self:center;width:30px;height:20px'></div>"
        "<div id=al3 style='margin:auto;width:30px;height:20px'></div></div>"
        /* editor .right.panelButton: margin-left:auto pushes it to the end of its auto track */
        "<div id=pbl style='width:260px;display:grid;grid-template-columns:40px auto'><div style='height:10px'></div>"
        "<div id=lk style='width:max-content;margin-left:auto;margin-right:-15px'>"
        "<span style='display:inline-block;width:20px;height:10px'></span></div></div>", NULL);
    HtmlRect sel = rc(fx, "sel");
    struct { const char *id; float x, y, w, h; } want[] = {
        { "pv", 0, 136, 58, 32 }, { "pa", 0, 0, 300, 126 }, { "hu", 68, 137.5f, 232, 8 }, { "op", 68, 158.5f, 232, 8 },
    };
    for (size_t i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        HtmlRect r = rc(fx, want[i].id);
        CHECK(NEAR(r.x - sel.x, want[i].x, 0.01) && NEAR(r.y - sel.y, want[i].y, 0.01) && NEAR(r.w, want[i].w, 0.01) && NEAR(r.h, want[i].h, 0.01),
              "grid placement: #%s at %.2f,%.2f %.2fx%.2f, expected %.2f,%.2f %.2fx%.2f", want[i].id,
              (double)(r.x - sel.x), (double)(r.y - sel.y), (double)r.w, (double)r.h,
              (double)want[i].x, (double)want[i].y, (double)want[i].w, (double)want[i].h);
    }
    HtmlRect g2 = rc(fx, "g2");
    struct { const char *id; float x, y, w, h; } want2[] = {
        { "a", 54, 0, 104, 20 }, { "b", 108, 20, 50, 30 }, { "c", 54, 20, 50, 30 }, { "d", 0, 20, 50, 30 },
        { "e", 0, 50, 50, 0 }, { "f", 108, 50, 50, 0 }, { "h", 0, 50, 50, 0 },
    };
    for (size_t i = 0; i < sizeof(want2) / sizeof(want2[0]); i++) {
        HtmlRect r = rc(fx, want2[i].id);
        CHECK(NEAR(r.x - g2.x, want2[i].x, 0.01) && NEAR(r.y - g2.y, want2[i].y, 0.01) && NEAR(r.w, want2[i].w, 0.01) && NEAR(r.h, want2[i].h, 0.01),
              "grid auto-placement: #%s at %.2f,%.2f %.2fx%.2f, expected %.2f,%.2f %.2fx%.2f", want2[i].id,
              (double)(r.x - g2.x), (double)(r.y - g2.y), (double)r.w, (double)r.h,
              (double)want2[i].x, (double)want2[i].y, (double)want2[i].w, (double)want2[i].h);
    }
    CHECK_NEAR(g2.h, 50, 0.01, "grid: empty implicit rows are 0 tall");
    HtmlRect al = rc(fx, "al");
    CHECK_NEAR(rc(fx, "al0").x - al.x, 35, 0.01, "grid: justify-self center");
    CHECK_NEAR(rc(fx, "al0").y - al.y, 0, 0.01, "grid: non-stretch fixed height at start");
    CHECK_NEAR(rc(fx, "al1").x - al.x, 170, 0.01, "grid: justify-self end");
    CHECK_NEAR(rc(fx, "al1").y - al.y, 20, 0.01, "grid: align-self end");
    CHECK_NEAR(rc(fx, "al2").x - al.x, 35, 0.01, "grid: place-self center (x)");
    CHECK_NEAR(rc(fx, "al2").y - al.y, 50, 0.01, "grid: place-self center (y)");
    CHECK_NEAR(rc(fx, "al3").x - al.x, 135, 0.01, "grid: margin:auto centres (x)");
    CHECK_NEAR(rc(fx, "al3").y - al.y, 50, 0.01, "grid: margin:auto centres (y)");
    HtmlRect lk = rc(fx, "lk");
    CHECK_NEAR(lk.x - rc(fx, "pbl").x, 255, 0.01, "grid: margin-left:auto pushes the item to the area end (+15px overhang)");
    CHECK_NEAR(lk.w, 20, 0.01, "grid: width:max-content item keeps its width");
    fx_free(fx);
}

/* grid-template-areas, named lines, grid-auto-rows/columns, grid-auto-flow
 * (column, dense), repeat(auto-fill/auto-fit), implicit tracks before the
 * explicit grid, the grid / grid-template shorthands and baseline
 * self-alignment.  Expected rects (relative to #ar) from Chrome
 * getBoundingClientRect of the same markup. */
static void t_grid_named(void)
{
    Fx *fx = fx_html(
        "<div style='font-size:16px'>"
        "<div id=ar style=\"display:grid;grid-template-areas:'h h' 's m' 'f f';grid-template-columns:50px 1fr;grid-template-rows:20px auto 15px;width:300px;gap:4px\">"
        "<div id=ar_m style=\"grid-area:m;height:30px\"></div><div id=ar_h style=\"grid-area:h\"></div><div id=ar_s style=\"grid-area:s\"></div><div id=ar_f style=\"grid-area:f\"></div><div id=ar_x style=\"grid-row:m-start/f-end;grid-column:2\"></div><div id=ar_a></div></div>"
        "<div id=nl style=\"display:grid;grid-template-columns:[a] 40px [b c] 50px [d] 60px [a];grid-auto-rows:10px;width:300px\">"
        "<div id=nl1 style=\"grid-column:b / d\"></div><div id=nl2 style=\"grid-column:a 2 / span 1\"></div><div id=nl3 style=\"grid-column:c\"></div><div id=nl4 style=\"grid-column:span a / 3\"></div><div id=nl5 style=\"grid-column:x 2;\"></div><div id=nl6 style=\"grid-column:d / span a\"></div></div>"
        "<div id=au style=\"display:grid;grid-template-columns:30px 30px;grid-auto-rows:10px 20px;grid-auto-columns:7px 9px;width:300px\">"
        "<div id=au1></div><div id=au2></div><div id=au3></div><div id=au4 style=\"grid-column:-5\"></div><div id=au5 style=\"grid-column:4;grid-row:5\"></div><div id=au6></div></div>"
        "<div id=fc style=\"display:grid;grid-auto-flow:column;grid-template-rows:10px 10px;grid-auto-columns:20px;width:300px;gap:2px\">"
        "<div id=fc1></div><div id=fc2 style=\"grid-row:span 2\"></div><div id=fc3></div><div id=fc4></div><div id=fc5 style=\"grid-row:2\"></div></div>"
        "<div id=dn style=\"display:grid;grid-auto-flow:row dense;grid-template-columns:repeat(3,20px);grid-auto-rows:10px;width:300px\">"
        "<div id=dn1></div><div id=dn2 style=\"grid-column:span 3\"></div><div id=dn3 style=\"grid-column:span 2\"></div><div id=dn4></div><div id=dn5 style=\"grid-column:2\"></div></div>"
        "<div id=af style=\"display:grid;grid-template-columns:repeat(auto-fill,50px);column-gap:10px;grid-auto-rows:10px;width:230px\">"
        "<div id=af1></div><div id=af2></div><div id=af3></div><div id=af4></div><div id=af5></div><div id=af6 style=\"grid-column:-2\"></div></div>"
        "<div id=ft style=\"display:grid;grid-template-columns:repeat(auto-fit,40px);justify-content:start;column-gap:10px;grid-auto-rows:10px;width:300px\">"
        "<div id=ft1></div><div id=ft2></div><div id=ft3 style=\"grid-column:-2\"></div></div>"
        "<div id=mm style=\"display:grid;grid-template-columns:repeat(auto-fill,minmax(60px,1fr));gap:5px;grid-auto-rows:10px;width:250px\">"
        "<div id=mm1></div><div id=mm2></div><div id=mm3></div><div id=mm4></div><div id=mm5></div></div>"
        "<div id=gt style=\"display:grid;grid-template:[t] 'a b' 12px [m] 'c c' 8px / 40px 1fr;width:200px\">"
        "<div id=gt1 style=\"grid-area:c\"></div><div id=gt2 style=\"grid-row:m\"></div><div id=gt3 style=\"grid-area:b\"></div></div>"
        "<div id=gs style=\"display:grid;grid:auto-flow dense 6px / 30px 30px;width:200px\">"
        "<div id=gs1 style=\"grid-column:2\"></div><div id=gs2></div><div id=gs3></div></div>"
        "<div id=ng style=\"display:grid;grid-template-columns:20px 20px;grid-template-rows:10px;grid-auto-rows:5px;width:200px\">"
        "<div id=ng1 style=\"grid-row:-3/-2\"></div><div id=ng2 style=\"grid-row:1;grid-column:-4\"></div><div id=ng3 style=\"grid-row:span 2 / 1;grid-column:2\"></div></div>"
        ""
        "<div id=a2 style=\"display:grid;grid-template-areas:'a b' 'c d' 'e e';grid-template-rows:10px;grid-auto-rows:7px 9px;grid-template-columns:30px;grid-auto-columns:11px;width:300px\">"
        "<div id=a2a style=\"grid-area:e\"></div><div id=a2b style=\"grid-area:d\"></div><div id=a2c style=\"grid-area:zz\"></div><div id=a2d></div></div>"
        "<div id=nn style=\"display:grid;grid-template-columns:[x] 10px [x] 20px [x] 30px;grid-auto-columns:5px;grid-auto-rows:6px;width:300px\">"
        "<div id=nn1 style=\"grid-column:x -1 / x -2\"></div><div id=nn2 style=\"grid-column:x -5\"></div><div id=nn3 style=\"grid-column:span x 2 / x 3\"></div><div id=nn4 style=\"grid-column:x 1 / span x 5\"></div><div id=nn5 style=\"grid-column:span x / 7\"></div></div>"
        "<div id=fx style=\"display:flex;flex-direction:column;width:170px\"><div id=fsw style=\"display:grid;grid-template-columns:repeat(auto-fit,1.75em);align-items:center;margin-top:.75em\"><div id=fs1 style=\"height:10px;justify-self:center;width:10px\"></div><div id=fs2 style=\"height:20px\"></div></div></div>"
        "<div id=cf style=\"display:grid;grid-auto-flow:column dense;grid-template-rows:repeat(3,8px);grid-auto-columns:15px;width:300px\">"
        "<div id=cf1 style=\"grid-row:span 2\"></div><div id=cf2 style=\"grid-row:span 2\"></div><div id=cf3></div><div id=cf4 style=\"grid-column:1\"></div><div id=cf5 style=\"grid-row:3\"></div></div>"
        "<div id=mh style=\"display:grid;grid-template-rows:repeat(auto-fill,20px);height:70px;width:100px;row-gap:5px\"><div id=mh1 style=\"grid-row:-2\"></div></div>"
        "<div id=pr style=\"display:grid;grid-template-columns:repeat(auto-fill,[l] 30% [r]);width:200px;grid-auto-rows:4px\"><div id=pr1 style=\"grid-column:r 2\"></div><div id=pr2 style=\"grid-column:l 3 / r 3\"></div></div>"
        "<div id=em style=\"display:grid;grid-template-rows:12px 13px;grid-template-columns:repeat(3,10px);row-gap:3px;width:100px\"></div><div id=bb style=\"display:grid;grid-template-columns:20px 20px 20px;align-items:baseline;width:100px\"><div id=bb1 style=\"height:20px\"></div><div id=bb2 style=\"height:30px;margin-top:4px\"></div><div id=bb3 style=\"height:10px;align-self:start\"></div><div id=bb4 style=\"height:5px;grid-row:2;grid-column:2\"></div></div>"
        "</div>", NULL);
    static const struct { const char *id; float x, y, w, h; } want[] = {
        { "ar", 0, 0, 300, 77 },
        { "ar_m", 54, 24, 246, 30 },
        { "ar_h", 0, 0, 300, 20 },
        { "ar_s", 0, 24, 50, 30 },
        { "ar_f", 0, 58, 300, 15 },
        { "ar_x", 54, 24, 246, 49 },
        { "ar_a", 0, 77, 50, 0 },
        { "nl", 0, 77, 300, 40 },
        { "nl1", 40, 77, 50, 10 },
        { "nl2", 150, 77, 50, 10 },
        { "nl3", 40, 87, 50, 10 },
        { "nl4", 0, 97, 90, 10 },
        { "nl5", 250, 97, 50, 10 },
        { "nl6", 90, 107, 60, 10 },
        { "au", 0, 117, 300, 70 },
        { "au1", 0, 117, 7, 10 },
        { "au2", 7, 117, 9, 10 },
        { "au3", 16, 117, 30, 10 },
        { "au4", 0, 127, 7, 20 },
        { "au5", 83, 177, 9, 10 },
        { "au6", 7, 127, 9, 20 },
        { "fc", 0, 187, 300, 22 },
        { "fc1", 0, 187, 20, 10 },
        { "fc2", 22, 187, 20, 22 },
        { "fc3", 44, 187, 20, 10 },
        { "fc4", 44, 199, 20, 10 },
        { "fc5", 66, 199, 20, 10 },
        { "dn", 0, 209, 300, 30 },
        { "dn1", 0, 209, 20, 10 },
        { "dn2", 0, 219, 60, 10 },
        { "dn3", 20, 209, 40, 10 },
        { "dn4", 0, 229, 20, 10 },
        { "dn5", 20, 229, 20, 10 },
        { "af", 0, 239, 230, 20 },
        { "af1", 0, 239, 50, 10 },
        { "af2", 60, 239, 50, 10 },
        { "af3", 120, 239, 50, 10 },
        { "af4", 180, 239, 50, 10 },
        { "af5", 0, 249, 50, 10 },
        { "af6", 180, 249, 50, 10 },
        { "ft", 0, 259, 300, 10 },
        { "ft1", 0, 259, 40, 10 },
        { "ft2", 50, 259, 40, 10 },
        { "ft3", 100, 259, 40, 10 },
        { "mm", 0, 269, 250, 25 },
        { "mm1", 0, 269, 80, 10 },
        { "mm2", 85, 269, 80, 10 },
        { "mm3", 170, 269, 80, 10 },
        { "mm4", 0, 284, 80, 10 },
        { "mm5", 85, 284, 80, 10 },
        { "gt", 0, 294, 200, 20 },
        { "gt1", 0, 306, 200, 8 },
        { "gt2", 200, 306, 0, 8 },
        { "gt3", 40, 294, 160, 12 },
        { "gs", 0, 314, 200, 12 },
        { "gs1", 30, 314, 30, 6 },
        { "gs2", 0, 314, 30, 6 },
        { "gs3", 0, 320, 30, 6 },
        { "ng", 0, 326, 200, 20 },
        { "ng1", 0, 331, 160, 5 },
        { "ng2", 0, 336, 160, 10 },
        { "ng3", 180, 326, 20, 10 },
        { "a2", 0, 346, 300, 42 },
        { "a2a", 0, 363, 41, 9 },
        { "a2b", 30, 356, 11, 7 },
        { "a2c", 52, 379, 11, 9 },
        { "a2d", 0, 346, 30, 10 },
        { "nn", 0, 388, 300, 24 },
        { "nn1", 20, 388, 20, 6 },
        { "nn2", 0, 394, 5, 6 },
        { "nn3", 10, 394, 30, 6 },
        { "nn4", 10, 400, 75, 6 },
        { "nn5", 40, 406, 45, 6 },
        { "fx", 0, 412, 170, 32 },
        { "fsw", 0, 424, 170, 20 },
        { "fs1", 9, 429, 10, 10 },
        { "fs2", 28, 424, 28, 20 },
        { "cf", 0, 444, 300, 24 },
        { "cf1", 0, 452, 15, 16 },
        { "cf2", 15, 444, 15, 16 },
        { "cf3", 15, 460, 15, 8 },
        { "cf4", 0, 444, 15, 8 },
        { "cf5", 30, 460, 15, 8 },
        { "mh", 0, 468, 100, 70 },
        { "mh1", 0, 518, 100, 20 },
        { "pr", 0, 538, 200, 8 },
        { "pr1", 120, 538, 60, 4 },
        { "pr2", 120, 542, 60, 4 },
        { "em", 0, 546, 100, 28 },
        { "bb", 0, 574, 100, 39 },
        { "bb1", 0, 588, 20, 20 },
        { "bb2", 20, 578, 20, 30 },
        { "bb3", 40, 574, 20, 10 },
        { "bb4", 20, 608, 20, 5 }
    };
    HtmlRect o = rc(fx, "ar");
    for (size_t i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        HtmlRect r = rc(fx, want[i].id);
        CHECK(NEAR(r.x - o.x, want[i].x, 0.05) && NEAR(r.y - o.y, want[i].y, 0.05) && NEAR(r.w, want[i].w, 0.05) && NEAR(r.h, want[i].h, 0.05),
              "grid named/auto: #%s at %.2f,%.2f %.2fx%.2f, expected %.2f,%.2f %.2fx%.2f", want[i].id,
              (double)(r.x - o.x), (double)(r.y - o.y), (double)r.w, (double)r.h,
              (double)want[i].x, (double)want[i].y, (double)want[i].w, (double)want[i].h);
    }
    fx_free(fx);
}

/* align-self overrides the flex container's align-items per item. */
/* display: contents (v7's #headerRightSocial): the element has no box, its children are
 * flex items of the parent; text inside takes the contents element's style. Custom
 * properties in calc() (hud.css crosshair arms: top: calc(var(--s) / -2)). */
static void t_display_contents_calc_var(void)
{
    Fx *fx = fx_html(
        "<div id=dc style='display:flex;width:300px;height:20px'>"
        "<div id=dc0 style='width:10px;height:10px'></div>"
        "<div id=dcc style='display:contents'><div id=dc1 style='width:20px;height:10px'></div>"
        "<div id=dc2 style='width:30px;height:10px'></div></div></div>"
        "<div id=cv style='--s:100px;position:absolute;left:200px;top:300px'>"
        "<div id=cv0 style='position:absolute;top:calc(var(--s) / -2);left:calc(var(--s) * 0.5 - 2px);width:4px;height:4px'></div></div>", NULL);
    HtmlRect dc = rc(fx, "dc");
    CHECK_NEAR(rc(fx, "dc1").x - dc.x, 10, 0.01, "display:contents: first child is the 2nd flex item");
    CHECK_NEAR(rc(fx, "dc2").x - dc.x, 30, 0.01, "display:contents: second child is the 3rd flex item");
    CHECK_NEAR(rc(fx, "dc1").y - dc.y, 0, 0.01, "display:contents: same flex line");
    HtmlElem dcc = dom_by_id(fx->dom, "dcc");
    CHECK(dcc && !((ElemData *)dom_elem_data(fx->dom, dcc))->box, "display:contents: no box of its own");
    HtmlRect cv0 = rc(fx, "cv0");
    CHECK_NEAR(cv0.y, 250, 0.01, "calc(var(--s) / -2)");
    CHECK_NEAR(cv0.x, 248, 0.01, "calc(var(--s) * 0.5 - 2px)");
    /* the custom property set through the inline-style API (hud.css #knCrosshair) */
    dom_set_style(fx->dom, dom_by_id(fx->dom, "cv"), "--s", "60px");
    fx_layout(fx);
    CHECK_NEAR(rc(fx, "cv0").y, 270, 0.01, "calc(var(--s) / -2) after dom_set_style(--s)");
    fx_free(fx);
    /* the same through stylesheet rules (ui/css/hud.css #knCrosshair / .knCrossArm-*) */
    fx = fx_html("<div id=kc><div id=kct class='arm t'></div><div id=kcb class='arm b'></div></div>",
                 /* v7.1.4's main.css defines 21 :root custom properties: all inherited */
                 ":root { --a0:0;--a1:0;--a2:0;--a3:0;--a4:0;--a5:0;--a6:0;--a7:0;--a8:0;--a9:0;--b0:0;--b1:0;--b2:0;"
                 "--b3:0;--b4:0;--b5:0;--b6:0;--b7:0;--b8:0;--b9:0;--c0:0; }\n"
                 "#kc { --kn-cross-size: 182px; position: absolute; top: 300px; left: 400px; width: 0; height: 0; }\n"
                 ".arm { position: absolute; width: 4px; height: 21px; left: -2px; }\n"
                 ".t { top: calc(var(--kn-cross-size) / -2); }\n.b { top: calc(var(--kn-cross-size) / 2 - 21px); }\n");
    CHECK_NEAR(rc(fx, "kct").y, 300 - 91, 0.01, "sheet: calc(var(--kn-cross-size) / -2)");
    CHECK_NEAR(rc(fx, "kcb").y, 300 + 91 - 21, 0.01, "sheet: calc(var(--kn-cross-size) / 2 - 21px)");
    dom_set_style(fx->dom, dom_by_id(fx->dom, "kc"), "--kn-cross-size", "58px");
    fx_layout(fx);
    CHECK_NEAR(rc(fx, "kct").y, 300 - 29, 0.01, "sheet: calc(var(--kn-cross-size) / -2) after inline --cs");
    fx_free(fx);
}

/* v7's empty Player List: an empty <table> with overflow-y: scroll and a percentage height
 * inside an auto-height block. Tables are not scroll containers (Chrome ignores overflow
 * scrolling on them) and the percentage height is indefinite: the table is 0 tall. */
static void t_empty_table_overflow(void)
{
    Fx *fx = fx_html("<div id=pl><table id=pt style='margin-top:8px;overflow-y:scroll;height:calc(100% - 75px)'><tbody></tbody></table></div>", NULL);
    CHECK_NEAR(rc(fx, "pt").h, 0, 0.01, "empty table with overflow-y:scroll: 0 tall");
    CHECK_NEAR(rc(fx, "pl").h, 0, 0.01, "its auto-height parent: 0 (the margin collapses through)");
    fx_free(fx);
}

static void t_flex_align_self(void)
{
    Fx *fx = fx_html(
        "<div id=fa style='display:flex;align-items:center;height:50px;width:200px'>"
        "<div id=fa0 style='width:10px;height:10px;align-self:flex-end'></div>"
        "<div id=fa1 style='width:10px;height:10px'></div>"
        "<div id=fa2 style='width:10px;align-self:stretch'></div>"
        "<div id=fa3 style='width:10px;height:10px;align-self:flex-start'></div></div>", NULL);
    HtmlRect fa = rc(fx, "fa");
    CHECK_NEAR(rc(fx, "fa0").y - fa.y, 40, 0.01, "flex: align-self flex-end");
    CHECK_NEAR(rc(fx, "fa1").y - fa.y, 20, 0.01, "flex: align-self auto uses align-items center");
    CHECK_NEAR(rc(fx, "fa2").h, 50, 0.01, "flex: align-self stretch");
    CHECK_NEAR(rc(fx, "fa3").y - fa.y, 0, 0.01, "flex: align-self flex-start");
    fx_free(fx);
}

/* Static position of absolutely positioned boxes inside inline content:
 * a block-level box (original display) goes to the start of the next line,
 * an inline-level one stays at the pen (Chrome). */
static void t_abs_static_inline(void)
{
    Fx *fx = fx_html(
        "<div id=t6 style='width:200px;line-height:20px;padding-left:7px'>"
        "<span style='display:inline-block;width:50px;height:10px'></span>"
        "<div id=a6 style='position:absolute;width:10px;height:10px'></div>"
        "<span id=i6 style='position:absolute;display:inline-block;width:10px;height:10px'></span></div>"
        "<div id=t7 style='width:200px;line-height:20px;text-align:right'>"
        "<div id=a7 style='position:absolute;width:10px;height:10px'></div>text</div>", NULL);
    HtmlRect t6 = rc(fx, "t6"), a6 = rc(fx, "a6"), i6 = rc(fx, "i6");
    CHECK_NEAR(a6.x - t6.x, 7, 0.01, "abs block after inline content: x at the content start");
    CHECK_NEAR(a6.y, t6.y + t6.h, 0.01, "abs block after inline content: y at the next line");
    CHECK_NEAR(i6.x - t6.x, 57, 0.01, "abs inline-block after inline content: x at the pen");
    CHECK_NEAR(i6.y, t6.y, 0.01, "abs inline-block after inline content: y at the line top");
    HtmlRect t7 = rc(fx, "t7"), a7 = rc(fx, "a7");
    CHECK_NEAR(a7.x, t7.x, 0.01, "abs block on an empty line: x at the content start (text-align ignored)");
    CHECK_NEAR(a7.y, t7.y, 0.01, "abs block on an empty line: y at the line top");
    fx_free(fx);
}

/* CSS zoom (Chrome semantics, values from Chrome 152 getBoundingClientRect). */
/* content: url() on an element replaces it with the image (Chrome; Krunker Strike's .watch-ad-img on a <div>) */
static void t_content_url_element(void)
{
    float nw = 0, nh = 0;
    CHECK(png_size(NULL, "img/app_0.png", "index.html", &nw, &nh) && nw > 0, "content url: test image present");
    Fx *fx = fx_html(
        "<div id=cu style='content:url(img/app_0.png)'>text</div>"
        "<div><span id=cs style='content:url(img/app_0.png);height:20px'>text</span></div>"
        "<div id=cn style='content:\"x\"'>abc</div>", NULL);
    Box *cu = bx(fx, "cu"), *cs = bx(fx, "cs"), *cn = bx(fx, "cn");
    CHECK(cu && cu->type == BOX_IMAGE && cu->src, "content url: a <div> becomes an image box");
    CHECK_NEAR(rc(fx, "cu").w, nw, 0.01, "content url: natural width");
    CHECK_NEAR(rc(fx, "cu").h, nh, 0.01, "content url: natural height");
    CHECK(cs && cs->type == BOX_IMAGE && (cs->flags & BOXF_INLINE_LEVEL), "content url: a <span> becomes an inline replaced image");
    CHECK_NEAR(rc(fx, "cs").h, 20, 0.01, "content url: sized like an image (height given)");
    CHECK_NEAR(rc(fx, "cs").w, 20 * nw / nh, 0.01, "content url: width from the aspect ratio");
    CHECK(cn && cn->type == BOX_BLOCK, "content url: a string content leaves the element alone");
    fx_free(fx);
}

static void t_zoom(void)
{
    Fx *fx = fx_html(
        "<div id=za style='width:100px;height:10px;border:1px solid red;zoom:0.7'></div>"
        "<div id=zb style='width:100px;height:10px;border:3px solid red;zoom:0.7'></div>"
        "<div id=zc style='width:100px;height:10px;border:1.5px solid red'></div>"
        "<div style='line-height:20px;zoom:0.5'><div id=ze style='zoom:0.5'>x</div></div>"
        "<div style='width:200px'><div id=zf style='width:50%;height:10px;zoom:0.5;margin-left:10%'></div></div>"
        "<div style='font-size:20px;zoom:2'><div id=zh style='width:1em;height:1rem'></div></div>"
        "<div id=zi style='width:2em;height:10px;font-size:10px;zoom:3'></div>"
        "<div style='zoom:0.7'><input id=zj type=checkbox><canvas id=zn></canvas></div>"
        "<div style='zoom:0.5'><div id=zo style='zoom:0.5;width:100px;height:4px'></div></div>"
        "<div style='zoom:50%;padding:10px'><div id=zp style='height:10px;margin:4px'></div></div>", NULL);
    HtmlRect r = rc(fx, "za");
    CHECK_NEAR(r.w, 72, 0.01, "zoom: 100px*0.7 + 1px borders (0.7 snaps to 1)");
    CHECK_NEAR(r.h, 9, 0.01, "zoom: height 10*0.7 + 2");
    CHECK_NEAR(rc(fx, "zb").w, 74, 0.01, "zoom: 3px border * 0.7 floors to 2");
    CHECK_NEAR(rc(fx, "zc").w, 102, 0.01, "border widths floor (1.5px -> 1)");
    CHECK_NEAR(rc(fx, "ze").h, 5, 0.01, "zoom: inherited line-height follows the child's zoom");
    CHECK_NEAR(rc(fx, "zf").w, 100, 0.01, "zoom: % width resolves against the zoomed containing block");
    CHECK_NEAR(rc(fx, "zf").x, 20, 0.01, "zoom: % margin resolves against the containing block");
    CHECK_NEAR(rc(fx, "zh").w, 40, 0.01, "zoom: em uses the zoomed font size");
    CHECK_NEAR(rc(fx, "zh").h, 32, 0.01, "zoom: rem is zoomed");
    CHECK_NEAR(rc(fx, "zi").w, 60, 0.01, "zoom: em with the element's own zoom");
    CHECK_NEAR(rc(fx, "zj").w, 9.1, 0.01, "zoom: checkbox natural size");
    CHECK_NEAR(rc(fx, "zj").x, 2.8, 0.01, "zoom: UA margins");
    CHECK_NEAR(rc(fx, "zn").w, 210, 0.01, "zoom: canvas natural size");
    CHECK_NEAR(rc(fx, "zo").w, 25, 0.01, "zoom: nested zoom multiplies");
    CHECK_NEAR(rc(fx, "zp").x, 7, 0.01, "zoom: percentage zoom (10px pad + 4px margin) * 0.5");
    const CssComputed *st = bx(fx, "zo")->style;
    CHECK_NEAR(st->zoom, 0.25, 1e-4, "zoom: effective zoom stored");
    CHECK_NEAR(st->font_size, 4, 1e-3, "zoom: font-size 16 * 0.25");
    /* hit testing follows the zoomed boxes */
    HtmlRect zo = rc(fx, "zo");
    Box *hit = layout_hit_test(fx->tree, zo.x + zo.w - 1, zo.y + 0.5f, 0);
    CHECK(hit && !strcmp(box_id(hit), "zo"), "zoom: hit test inside the zoomed box (%s)", box_id(hit));
    hit = layout_hit_test(fx->tree, zo.x + zo.w + 2, zo.y + 0.5f, 0);
    CHECK(!hit || strcmp(box_id(hit), "zo"), "zoom: hit test outside the zoomed width");
    fx_free(fx);
}

/* Chrome UA defaults for <hr>, headings, text inputs (content-box), <b>. */
static void t_ua_defaults(void)
{
    Fx *fx = fx_html(
        "<div style='width:300px;font-size:16px'><div id=above style='height:10px'></div><hr id=hr><hr id=hw style='width:50%'>"
        "<input id=tx style='width:100px;padding:10px'><textarea id=ta style='width:100px;padding:10px'></textarea>"
        "<select id=se style='width:100px;padding:10px'></select></div>"
        "<div style='font-weight:700'><b id=bb>x</b></div>", NULL);
    HtmlRect hr = rc(fx, "hr");
    CHECK_NEAR(hr.h, 2, 0.01, "hr: 1px inset border top and bottom");
    CHECK_NEAR(hr.y - (rc(fx, "above").y + 10), 8, 0.01, "hr: margin 0.5em");
    CHECK_NEAR(hr.w, 300, 0.01, "hr: auto width");
    HtmlRect hw = rc(fx, "hw");
    CHECK_NEAR(hw.x, 74, 0.01, "hr: margin-inline auto centres (300 - 152) / 2");
    const CssComputed *s = bx(fx, "hr")->style;
    CHECK(s->border_style[CSS_TOP] == CSS_BORDER_STYLE_INSET && s->color.r == 128 && s->overflow_x == CSS_OVERFLOW_HIDDEN, "hr: inset, gray, overflow hidden");
    CHECK_NEAR(rc(fx, "tx").w, 124, 0.01, "input: content-box (100 + 2*10 + 2*2)");
    CHECK_NEAR(rc(fx, "ta").w, 122, 0.01, "textarea: content-box, 1px border");
    CHECK_NEAR(rc(fx, "se").w, 100, 0.01, "select: border-box");
    CHECK(bx(fx, "bb")->style->font_weight == 900, "b: bolder (700 -> 900)");
    fx_free(fx);
}

static void t_table(void)
{
    Fx *fx = fx_html(
        "<div id=tb style='display:table;width:300px'>"
        "<div id=c1 style='display:table-cell;width:25%'>a</div>"
        "<div id=c2 style='display:table-cell'>b<br>c</div>"
        "<div id=c3 style='display:table-cell;width:25%;vertical-align:middle'>d</div></div>"
        "<div id=ta style='display:table;width:400px'><div id=t1 style='display:table-cell'>aaaa</div>"
        "<div id=t2 style='display:table-cell'>aa</div></div>"
        "<div id=ts style='display:table;border-spacing:10px'><div id=s1 style='display:table-cell;width:50px'>x</div></div>", NULL);
    HtmlRect tb = rc(fx, "tb"), c1 = rc(fx, "c1"), c2 = rc(fx, "c2"), c3 = rc(fx, "c3");
    CHECK_NEAR(c1.w, 75, 0.01, "table: 25% cell");
    CHECK_NEAR(c2.x - tb.x, 75, 0.01, "table: auto cell x");
    CHECK_NEAR(c2.w, 150, 0.01, "table: auto cell gets the rest");
    CHECK_NEAR(c3.x - tb.x, 225, 0.01, "table: last cell x");
    CHECK(c1.h > 0 && NEAR(c1.h, c2.h, 0.01) && NEAR(c3.h, c2.h, 0.01), "table: cells share the row height (%.1f %.1f %.1f)", c1.h, c2.h, c3.h);
    Box *c3b = bx(fx, "c3");
    const TextRun *r3 = c3b ? first_run(c3b) : NULL;
    const TextRun *r1 = first_run(bx(fx, "c1"));
    if (r3 && r1) CHECK(r3->y > r1->y + 1, "table: vertical-align middle moves the cell content down");
    float wa = tw("gamefont", 16, "aaaa"), wb = tw("gamefont", 16, "aa");
    CHECK_NEAR(rc(fx, "t1").w, 400 * wa / (wa + wb), 0.05, "table: auto layout distributes by max-content");
    HtmlRect ts = rc(fx, "ts"), s1 = rc(fx, "s1");
    CHECK_NEAR(ts.w, 70, 0.01, "table: border-spacing around the cell");
    CHECK_NEAR(s1.x - ts.x, 10, 0.01, "table: border-spacing offset");
    fx_free(fx);
}

/* colspan (HTML 4.4.6): a spanning cell starts at its first column and is as wide as its
 * columns together (border-spacing between them included); the rows below keep their own
 * columns. Strike's mini scoreboard puts a colspan=3 team header over the player columns. */
static void t_table_colspan(void)
{
    /* auto layout: a header spanning the first two columns of 3-cell body rows */
    Fx *fx = fx_html(
        "<table id=cs style='border-spacing:0;width:400px'>"
        "<tr id=ch><td id=hh colspan=2>Team</td><td id=hs>12</td></tr>"
        "<tr id=cb><td id=p1>1</td><td id=n1>Guest long name</td><td id=k1>3</td></tr></table>", NULL);
    HtmlRect hh = rc(fx, "hh"), hs = rc(fx, "hs"), p1 = rc(fx, "p1"), n1 = rc(fx, "n1"), k1 = rc(fx, "k1"), cs = rc(fx, "cs");
    CHECK(hh.y < p1.y, "colspan: header row above the body row");
    CHECK_NEAR(hh.x, p1.x, 0.01, "colspan auto: spanning cell starts at its column");
    CHECK_NEAR(hh.w, p1.w + n1.w, 0.01, "colspan auto: spanning cell is as wide as its two columns");
    CHECK_NEAR(hs.x, k1.x, 0.01, "colspan auto: the cell after the span lines up with the body's third column");
    CHECK_NEAR(hs.w, k1.w, 0.01, "colspan auto: the third column keeps its width");
    CHECK_NEAR(cs.w, 400, 0.01, "colspan auto: table width is the specified width");
    fx_free(fx);

    /* the spanning cell is wider than its columns' max-content: the extra goes to the columns */
    fx = fx_html(
        "<table id=cw style='border-spacing:0;width:400px'>"
        "<tr><td id=wh colspan=2>A very long team header text</td><td id=ws>1</td></tr>"
        "<tr><td id=wa>1</td><td id=wb>b</td><td id=wk>3</td></tr></table>", NULL);
    HtmlRect wh = rc(fx, "wh"), ws = rc(fx, "ws"), wa = rc(fx, "wa"), wb = rc(fx, "wb"), wk = rc(fx, "wk");
    CHECK_NEAR(wh.w, wa.w + wb.w, 0.01, "colspan wide: the two columns grow to the header's width");
    CHECK_NEAR(ws.x, wk.x, 0.01, "colspan wide: cell after the span stays aligned");
    CHECK_NEAR(wh.w + ws.w, 400, 0.01, "colspan wide: the row fills the table");
    fx_free(fx);

    /* table-layout: fixed: the first row's widths set the columns; a spanning cell's px width
     * is split across its columns (200px over two columns, then 100px for the third) */
    fx = fx_html(
        "<table id=cf style='table-layout:fixed;width:300px;border-spacing:0'>"
        "<tr><td id=fh colspan=2 style='width:200px'>x</td><td id=fs style='width:100px'>y</td></tr>"
        "<tr><td id=f1>1</td><td id=f2>2</td><td id=f3>3</td></tr></table>", "td, th { padding: 0; }");
    HtmlRect fh = rc(fx, "fh"), fs = rc(fx, "fs"), f1 = rc(fx, "f1"), f2 = rc(fx, "f2"), f3 = rc(fx, "f3");
    CHECK_NEAR(fh.x, f1.x, 0.01, "colspan fixed: spanning cell starts at its column");
    CHECK_NEAR(fh.w, 200, 0.01, "colspan fixed: spanning cell has its px width");
    CHECK_NEAR(f1.w + f2.w, 200, 0.01, "colspan fixed: the two columns take the span's width");
    CHECK_NEAR(fs.x, f3.x, 0.01, "colspan fixed: cell after the span lines up with the third column");
    CHECK_NEAR(f3.w, 100, 0.01, "colspan fixed: third column keeps its px width");
    fx_free(fx);

    /* colspan="0" is read as 1 (HTML 4.4.6); a huge colspan is clamped and does not overflow */
    fx = fx_html(
        "<table style='border-spacing:0;width:300px'>"
        "<tr><td id=z0 colspan=0>a</td><td id=z1>b</td></tr>"
        "<tr><td id=z2>c</td><td id=z3>d</td></tr>"
        "<tr><td id=bg colspan=100000>e</td></tr></table>", NULL);
    CHECK_NEAR(rc(fx, "z1").x, rc(fx, "z3").x, 0.01, "colspan=0 counts as one column");
    CHECK(rc(fx, "bg").w > 0, "colspan huge: clamped, cell laid out");
    fx_free(fx);
}

/* rowspan (HTML 4.4.6): a cell takes its column in the rows it covers, so those rows skip the
 * column; its width is its column's, its height is shared out over its rows. */
static void t_table_rowspan(void)
{
    /* auto widths: the next row's cells start after the spanned column */
    Fx *fx = fx_html(
        "<table id=rs style='border-spacing:0;width:400px'>"
        "<tr><td id=ra rowspan=2>Team</td><td id=rb>x</td><td id=rc>1</td></tr>"
        "<tr><td id=rd>longer second row text</td><td id=re>2</td></tr></table>", NULL);
    HtmlRect ra = rc(fx, "ra"), rb = rc(fx, "rb"), rcc = rc(fx, "rc"), rd = rc(fx, "rd"), re = rc(fx, "re");
    CHECK_NEAR(rb.x - ra.x, ra.w, 0.01, "rowspan auto: the cell after it starts past its column");
    CHECK_NEAR(rd.x, rb.x, 0.01, "rowspan auto: the next row's first cell takes the second column");
    CHECK_NEAR(re.x, rcc.x, 0.01, "rowspan auto: the next row's second cell lines up with the third column");
    CHECK_NEAR(rd.w, rb.w, 0.01, "rowspan auto: the second column is shared by both rows");
    CHECK_NEAR(ra.y, rb.y, 0.01, "rowspan auto: the cell starts on its row");
    CHECK_NEAR(ra.y + ra.h, rd.y + rd.h, 0.01, "rowspan auto: the cell covers both rows");
    fx_free(fx);

    /* a rowspan cell wider than its column's content widens that column */
    fx = fx_html(
        "<table id=rw style='border-spacing:0;width:300px'>"
        "<tr><td id=wa rowspan=2>A very long header in the spanning cell</td><td id=wb>b</td></tr>"
        "<tr><td id=wc>c</td></tr></table>", NULL);
    HtmlRect wa = rc(fx, "wa"), wb = rc(fx, "wb"), wc = rc(fx, "wc");
    CHECK(wa.w > wb.w, "rowspan wide: the spanning cell's column is wider (%.1f > %.1f)", wa.w, wb.w);
    CHECK_NEAR(wc.x, wb.x, 0.01, "rowspan wide: the next row's cell is in the second column");
    CHECK_NEAR(wc.w, wb.w, 0.01, "rowspan wide: the second column keeps one width");
    fx_free(fx);

    /* table-layout: fixed: the first row sets the columns; the cell below skips the spanned one */
    fx = fx_html(
        "<table id=rf style='table-layout:fixed;width:300px;border-spacing:0'>"
        "<tr><td id=fa rowspan=2 style='width:100px;padding:0'>a</td><td id=fb style='width:200px;padding:0'>b</td></tr>"
        "<tr><td id=fc style='padding:0'>c</td></tr></table>", NULL);
    HtmlRect fa = rc(fx, "fa"), fb = rc(fx, "fb"), fc = rc(fx, "fc");
    CHECK_NEAR(fa.w, 100, 0.01, "rowspan fixed: spanning cell keeps its px width");
    CHECK_NEAR(fb.w, 200, 0.01, "rowspan fixed: the second column keeps its px width");
    CHECK_NEAR(fc.x - fa.x, 100, 0.01, "rowspan fixed: the cell below starts in the second column");
    CHECK_NEAR(fc.w, 200, 0.01, "rowspan fixed: the cell below has the second column's width");
    fx_free(fx);

    /* height: the spanning cell (100px) is taller than its two 30px rows and the 4px spacing
     * between them (64px): the 36px extra gives each row 18px more (48px each) */
    fx = fx_html(
        "<table id=rh style='border-spacing:4px;width:300px'>"
        "<tr id=h1 style='height:30px'><td id=ha rowspan=2 style='height:100px;padding:0'>x</td><td id=hb style='padding:0'>b</td></tr>"
        "<tr id=h2 style='height:30px'><td id=hc style='padding:0'>c</td></tr></table>", NULL);
    HtmlRect ha = rc(fx, "ha"), hb = rc(fx, "hb"), hc = rc(fx, "hc"), h1 = rc(fx, "h1"), h2 = rc(fx, "h2");
    CHECK_NEAR(h1.h, 48, 0.01, "rowspan height: first row takes its share of the extra");
    CHECK_NEAR(h2.h, 48, 0.01, "rowspan height: second row takes its share of the extra");
    CHECK_NEAR(ha.h, 100, 0.01, "rowspan height: spanning cell is as tall as its rows and the spacing");
    CHECK_NEAR(hc.y - ha.y, 52, 0.01, "rowspan height: the row after starts after the first row and spacing");
    CHECK_NEAR(hb.h, h1.h, 0.01, "rowspan height: the row's own cell is as tall as the row");
    fx_free(fx);

    /* rowspan=0 spans to the last row; a rowspan past the table's end is clamped to it;
     * colspan and rowspan together take a 2x2 block */
    fx = fx_html(
        "<table id=rx style='border-spacing:0;width:300px'>"
        "<tr><td id=xa colspan=2 rowspan=2 style='padding:0'>a</td><td id=xb style='padding:0'>b</td></tr>"
        "<tr><td id=xc style='padding:0'>c</td></tr>"
        "<tr><td id=xd rowspan=0 style='padding:0'>d</td><td id=xe style='padding:0'>e</td></tr>"
        "<tr><td id=xf rowspan=9 style='padding:0'>f</td><td id=xg style='padding:0'>g</td></tr></table>", NULL);
    HtmlRect xa = rc(fx, "xa"), xb = rc(fx, "xb"), xc = rc(fx, "xc"), xd = rc(fx, "xd"), xe = rc(fx, "xe"), xf = rc(fx, "xf"), xg = rc(fx, "xg");
    CHECK_NEAR(xa.w, xd.w + xe.w, 0.01, "colspan+rowspan: the 2x2 cell is as wide as its two columns");
    CHECK_NEAR(xa.y + xa.h, xc.y + xc.h, 0.01, "colspan+rowspan: the 2x2 cell covers both of its rows");
    CHECK_NEAR(xc.x, xb.x, 0.01, "colspan+rowspan: the row below starts after the block");
    CHECK_NEAR(xd.y + xd.h, xg.y + xg.h, 0.01, "rowspan=0: the cell covers the rows to the table's end");
    CHECK_NEAR(xf.x, xe.x, 0.01, "rowspan=0: the last row's cell skips the cell that spans down");
    CHECK_NEAR(xg.x, xb.x, 0.01, "rowspan=0: the last column lines up");
    CHECK_NEAR(xf.h, xg.h, 0.01, "rowspan past the table's end: clamped to one row");
    fx_free(fx);
}

static void t_positioned(void)
{
    Fx *fx = fx_html(
        "<div id=rc style='position:relative;width:400px;height:300px;margin-left:20px'>"
        "<div id=ab style='position:absolute;right:10px;bottom:20px;width:50px;height:40px'></div>"
        "<div id=ctr style='position:absolute;left:50%;top:50%;width:100px;height:60px;transform:translate(-50%,-50%)'></div>"
        "<div id=st style='position:absolute;left:10%;right:10%;top:0;height:5px'></div>"
        "<div id=rel style='position:relative;left:7px;top:3px;height:5px'></div></div>"
        "<div id=tf style='transform:translateX(10px);height:50px'>"
        "<div id=fx2 style='position:fixed;left:0;top:0;width:5px;height:5px'></div></div>"
        "<div id=fx3 style='position:fixed;right:0;bottom:0;width:5px;height:5px'></div>", NULL);
    HtmlRect ab = rc(fx, "ab");
    CHECK(NEAR(ab.x, 20 + 340, 0.01) && NEAR(ab.y, 240, 0.01) && NEAR(ab.w, 50, 0.01) && NEAR(ab.h, 40, 0.01),
          "abs right/bottom: (%.1f,%.1f %.1fx%.1f)", ab.x, ab.y, ab.w, ab.h);
    HtmlRect ctr = rc(fx, "ctr");
    CHECK(NEAR(ctr.x, 20 + 150, 0.01) && NEAR(ctr.y, 120, 0.01), "abs 50%% + translate(-50%%,-50%%): (%.1f,%.1f)", ctr.x, ctr.y);
    CHECK_NEAR(layout_offset_width(bx(fx, "ctr")), 100, 0.01, "offsetWidth ignores transforms");
    HtmlRect st = rc(fx, "st");
    CHECK(NEAR(st.x, 20 + 40, 0.01) && NEAR(st.w, 320, 0.01), "abs left/right %% stretch: x %.1f w %.1f", st.x, st.w);
    HtmlRect rel = rc(fx, "rel");
    CHECK(NEAR(rel.x, 27, 0.01) && NEAR(rel.y, 3, 0.01), "relative offset: (%.1f,%.1f)", rel.x, rel.y);
    HtmlRect tf = rc(fx, "tf"), f2 = rc(fx, "fx2");
    CHECK(NEAR(f2.x, tf.x, 0.01) && NEAR(f2.y, tf.y, 0.01), "fixed inside transform uses the transformed ancestor (%.1f,%.1f vs %.1f,%.1f)", f2.x, f2.y, tf.x, tf.y);
    HtmlRect f3 = rc(fx, "fx3");
    CHECK(NEAR(f3.x, 795, 0.01) && NEAR(f3.y, 595, 0.01), "fixed right/bottom against the viewport (%.1f,%.1f)", f3.x, f3.y);
    fx_free(fx);
}

static void t_sticky_scroll(void)
{
    Fx *fx = fx_html(
        "<div id=sc style='height:100px;overflow-y:auto;width:200px;margin-top:10px'>"
        "<div id=sec style='height:300px'><div id=hd style='position:sticky;top:0;height:20px'></div></div>"
        "<div style='height:500px'></div></div>"
        "<div id=rs style='direction:rtl;overflow-y:scroll;width:200px;height:100px'><div id=rsc style='height:300px'></div></div>"
        "<div id=chatList style='overflow-y:auto;height:50px;width:100px'><div style='height:200px'></div></div>"
        "<div id=hid style='overflow:hidden;height:30px;width:30px'><div style='height:300px'></div></div>", NULL);
    Box *sc = bx(fx, "sc");
    HtmlRect scr = rc(fx, "sc");
    CHECK(sc && (sc->flags & BOXF_VBAR), "auto scroller with overflow gets a bar");
    if (sc) {
        CHECK_NEAR(sc->scroll_h, 800, 0.01, "scrollHeight");
        CHECK_NEAR(box_client_w(sc), 188, 0.01, "clientWidth excludes the 12px bar");
        layout_set_scroll(fx->tree, sc, 0, 150);
        CHECK_NEAR(rc(fx, "hd").y, scr.y, 0.01, "sticky header stuck at the scroller top");
        layout_set_scroll(fx->tree, sc, 0, 290);
        CHECK_NEAR(rc(fx, "hd").y, scr.y + 280 - 290, 0.01, "sticky header clamped to its containing block");
        layout_set_scroll(fx->tree, sc, 0, 5000);
        CHECK_NEAR(sc->scroll_y, 700, 0.01, "scroll offset clamped to scrollHeight - clientHeight");
        layout_set_scroll(fx->tree, sc, 0, 0);
        CHECK_NEAR(rc(fx, "hd").y, scr.y, 0.01, "sticky header in place at scroll 0");
    }
    Box *rs = bx(fx, "rs");
    LayoutScrollbar sb;
    if (rs && layout_scrollbar(fx->tree, rs, 1, &sb)) {
        CHECK_NEAR(sb.track_x, rs->x, 0.01, "rtl: vertical bar on the left");
        CHECK_NEAR(sb.track_w, 12, 0.01, "rtl: bar width 12");
        CHECK_NEAR(rc(fx, "rsc").x, rs->x + 12, 0.01, "rtl: content shifted right of the bar");
        CHECK_NEAR(rc(fx, "rsc").w, 188, 0.01, "rtl: content width");
    } else CHECK(0, "rtl scrollbar present");
    Box *cl = bx(fx, "chatList");
    if (cl && layout_scrollbar(fx->tree, cl, 1, &sb)) {
        CHECK_NEAR(sb.track_w, 8, 0.01, "#chatList bar 8px");
        CHECK_NEAR(sb.thumb_h, 12.5, 0.01, "thumb = visible/total * track");
        layout_set_scroll(fx->tree, cl, 0, 75);
        layout_scrollbar(fx->tree, cl, 1, &sb);
        CHECK_NEAR(sb.thumb_y - sb.track_y, 18.75, 0.01, "thumb position");
    } else CHECK(0, "#chatList scrollbar present");
    Box *hid = bx(fx, "hid");
    CHECK(hid && !(hid->flags & BOXF_VBAR) && NEAR(hid->scroll_h, 300, 0.01), "overflow:hidden: no bar but scrollable overflow");
    fx_free(fx);
}

static void t_text(void)
{
    Fx *fx = fx_html(
        "<div id=el style='width:100px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis'>The quick brown fox jumps</div>"
        "<div id=up style='text-transform:uppercase'>abc</div>"
        "<div id=mc style='width:min-content'>hello world</div>"
        "<div id=br1>a<br>b</div>"
        "<div id=nw style='width:20px;white-space:nowrap'>aaa bbb ccc</div>"
        "<div id=wr style='width:60px'>aaa bbb ccc ddd</div>"
        "<div style='width:60px'><span id=sp style='padding:0 5px;background:red'>aaa bbb ccc ddd</span></div>"
        "<div id=ls style='letter-spacing:-2px'>abc</div>"
        "<div id=ce style='text-align:center;width:200px'>ab</div>"
        "<select id=sel><option>x</option><option>longer option</option></select>", NULL);
    Box *el = bx(fx, "el");
    const TextRun *r = el ? first_run(el) : NULL;
    CHECK(r && r->ellipsis, "ellipsis applied");
    if (r) {
        CHECK(r->width + r->ellipsis_w <= 100.01f, "ellipsis fits: %.2f + %.2f", r->width, r->ellipsis_w);
        CHECK(r->width + r->ellipsis_w > 80.0f, "ellipsis keeps as much text as fits: %.2f", r->width + r->ellipsis_w);
        CHECK(r->len < r->full_len || r->len < 25, "ellipsis truncates text");
    }
    r = first_run(bx(fx, "up"));
    CHECK(r && r->len == 3 && memcmp(r->text, "ABC", 3) == 0, "text-transform: uppercase");
    float mw = fmaxf(tw("gamefont", 16, "hello"), tw("gamefont", 16, "world"));
    CHECK_NEAR(rc(fx, "mc").w, mw, 0.01, "width: min-content");
    CHECK(count_lines(bx(fx, "br1")) == 2, "<br> forces a line break (%d)", count_lines(bx(fx, "br1")));
    CHECK(count_lines(bx(fx, "nw")) == 1, "white-space: nowrap keeps one line");
    CHECK(count_lines(bx(fx, "wr")) >= 2, "normal wrapping (%d lines)", count_lines(bx(fx, "wr")));
    Box *sp = bx(fx, "sp");
    CHECK(sp && sp->frag_count >= 2 && sp->frags[0].first && !sp->frags[0].last, "inline box fragments across lines (%d)", sp ? sp->frag_count : -1);
    TextFont *f = text_font(g_ts, "gamefont", 16, 400, 0);
    float wl = f ? text_measure(f, "abc", 3, -2) : 0;
    CHECK_NEAR(first_run(bx(fx, "ls"))->width, wl, 0.01, "letter-spacing");
    const TextRun *rce = first_run(bx(fx, "ce"));
    if (rce) CHECK_NEAR(rce->x, (200 - tw("gamefont", 16, "ab")) * 0.5f, 0.01, "text-align: center");
    Box *sel = bx(fx, "sel");
    if (sel) {
        float lw = sel->font ? text_measure(sel->font, "longer option", 13, 0) : 0;
        CHECK_NEAR(sel->intrinsic_w, ceilf(lw) + 20, 0.01, "select: longest option + 20px arrow (UA fallback)");
    }
    fx_free(fx);
}

static void t_hit_test(void)
{
    Fx *fx = fx_html(
        "<div id=h1 style='position:absolute;left:0;top:0;width:100px;height:100px;z-index:2'></div>"
        "<div id=h2 style='position:absolute;left:0;top:0;width:100px;height:100px;z-index:1'></div>"
        "<div id=h4 style='position:absolute;left:200px;top:0;width:100px;height:100px'></div>"
        "<div id=h3 style='position:absolute;left:200px;top:0;width:100px;height:100px;z-index:5;pointer-events:none'></div>"
        "<div id=h5 style='position:absolute;left:400px;top:0;width:100px;height:100px;transform:scale(0.5)'></div>"
        "<div id=hc style='position:absolute;left:0;top:200px;width:50px;height:50px;overflow:hidden'>"
        "<div id=hcc style='width:200px;height:20px'></div></div>"
        "<div id=hn style='position:absolute;left:0;top:300px;width:100px;height:100px;z-index:-1'></div>"
        /* v7's comp menu: two overlapping pointer-events:none lists (#uiBase inherits none), each
         * with a pointer-events:all button; the later list's anonymous line box and text must
         * let the point through to the earlier list's button */
        "<div style='pointer-events:none'>"
        "<div id=hpa style='position:absolute;left:0;top:500px;width:300px;transform:scale(0.9)'><div>Alpha</div>"
        "<span id=hpab style='display:inline-block;width:60px;height:30px;pointer-events:all'></span></div>"
        "<div id=hpb style='position:absolute;left:0;top:500px;width:300px;transform:scale(0.9);text-align:right'>"
        "<div>Bravo</div>xxxx xxxx xxxx xxxx xxxx xxxx xxxx xxxx xxxx xxxx xxxx xxxx xxxx xxxx xxxx xxxx xxxx xxxx xxxx xxxx xxxx xxxx xxxx xxxx xxxx xxxx xxxx xxxx xxxx xxxx "
        "<span id=hpbb style='display:inline-block;width:60px;height:30px;pointer-events:all'></span></div></div>", NULL);
    Box *b = layout_hit_test(fx->tree, 50, 50, 1);
    CHECK(b && strcmp(box_id(b), "h1") == 0, "hit: higher z-index wins (%s)", box_id(b));
    b = layout_hit_test(fx->tree, 250, 50, 1);
    CHECK(b && strcmp(box_id(b), "h4") == 0, "hit: pointer-events none passes through (%s)", box_id(b));
    b = layout_hit_test(fx->tree, 250, 50, 0);
    CHECK(b && strcmp(box_id(b), "h3") == 0, "hit: without pointer-events awareness (%s)", box_id(b));
    b = layout_hit_test(fx->tree, 410, 10, 1);
    CHECK(!b || strcmp(box_id(b), "h5") != 0, "hit: outside the scaled box (%s)", box_id(b));
    b = layout_hit_test(fx->tree, 450, 50, 1);
    CHECK(b && strcmp(box_id(b), "h5") == 0, "hit: inside the scaled box (%s)", box_id(b));
    b = layout_hit_test(fx->tree, 100, 210, 1);
    CHECK(!b || strcmp(box_id(b), "hcc") != 0, "hit: overflow hidden clips descendants (%s)", box_id(b));
    b = layout_hit_test(fx->tree, 20, 210, 1);
    CHECK(b && strcmp(box_id(b), "hcc") == 0, "hit: child inside the clip (%s)", box_id(b));
    b = layout_hit_test(fx->tree, 50, 350, 1);
    CHECK(b && strcmp(box_id(b), "hn") == 0, "hit: negative z-index still above the root background (%s)", box_id(b));
    HtmlRect pab = { 0, 0, 0, 0 }, pbb = { 0, 0, 0, 0 };
    if (bx(fx, "hpab")) layout_box_rect_in_viewport(bx(fx, "hpab"), &pab);
    if (bx(fx, "hpbb")) layout_box_rect_in_viewport(bx(fx, "hpbb"), &pbb);
    b = layout_hit_test(fx->tree, pab.x + 5, pab.y + pab.h * 0.5f, 1);
    CHECK(b && strcmp(box_id(b), "hpab") == 0, "hit: a pointer-events:none block's anonymous lines / text let the point through (%s)", box_id(b));
    b = layout_hit_test(fx->tree, pbb.x + 5, pbb.y + pbb.h * 0.5f, 1);
    CHECK(b && strcmp(box_id(b), "hpbb") == 0, "hit: the later list's own button (%s)", box_id(b));
    Box *list[16];
    int n = layout_stacking_children(layout_tree_root(fx->tree), list, 16);
    CHECK(n >= 6 && strcmp(box_id(list[0]), "hn") == 0 && strcmp(box_id(list[n - 1]), "h3") == 0,
          "stacking order: negative first, highest last (%d: %s .. %s)", n, n ? box_id(list[0]) : "", n ? box_id(list[n - 1]) : "");
    fx_free(fx);
}

static void t_end_table(void)
{
    /* the HUD's #endTable markup (hud_ui.c) with the shipped main.css, as in /tmp/kn-tabletest */
    size_t len;
    char *css = read_file("ui/css/main.css", &len);
    Fx *fx = fx_html("<div id='endTabbedView'><div class='endCard'><div id='tabHolder'><div class='tabHeader selected'>Leaderboard</div>"
                     "<div class='tabHeader '>Performance</div></div><div class='tabBody'><table id='endTable'>"
                     "<tr><th id=h0>Rank</th><th id=h1>Name</th><th id=h2>Score</th><th>Kills</th><th>Deaths</th><th id=h5>Reward</th><th></th></tr>"
                     "<tr style='border-bottom:none'><td id=d0 style='color:#fff'>1.</td><td id=d1><a class='endTableN' style='color:#fff'>Guest_1</a></td>"
                     "<td id=d2>350</td><td>0</td><td>0</td><td id=d5>0</td>"
                     "<td style='width: 1px;white-space: nowrap;padding-right: 0px;padding-left: 0px;'></td></tr></table></div></div></div>", css);
    free(css);
    HtmlRect h0 = rc(fx, "h0"), h1 = rc(fx, "h1"), h2 = rc(fx, "h2"), h5 = rc(fx, "h5"), d0 = rc(fx, "d0"), d1 = rc(fx, "d1"), d5 = rc(fx, "d5");
    printf("endTable: th x %.1f %.1f %.1f .. %.1f y %.1f | td y %.1f x %.1f %.1f %.1f\n", h0.x, h1.x, h2.x, h5.x, h0.y, d0.y, d0.x, d1.x, d5.x);
    CHECK(NEAR(h0.y, h1.y, 0.01) && NEAR(h1.y, h5.y, 0.01) && h0.x < h1.x && h1.x < h2.x && h2.x < h5.x, "#endTable header cells share one row");
    CHECK(NEAR(d0.y, d1.y, 0.01) && NEAR(d1.y, d5.y, 0.01) && d0.y > h0.y && NEAR(d0.x, h0.x, 0.01) && NEAR(d5.x, h5.x, 0.01),
          "#endTable data row below, columns aligned with the header");
    /* th's UA text-align (-internal-center) yields to #endTable's text-align:left */
    Box *th = bx(fx, "h2");
    CHECK(th && th->style->text_align == CSS_TEXT_ALIGN_LEFT, "#endTable th inherits text-align:left (%d)", th ? th->style->text_align : -1);
    fx_free(fx);
    fx = fx_html("<table><tr><th id=tc>A</th></tr></table>", NULL);
    th = bx(fx, "tc");
    CHECK(th && th->style->text_align == CSS_TEXT_ALIGN_CENTER, "th centers by default (%d)", th ? th->style->text_align : -1);
    fx_free(fx);
}

static void t_html_table(void)
{
    /* like #endTable (hud_ui.c): real table elements (parser adds <tbody>), UA table styles,
     * overflow-y:auto + height:min-content + max-height:100% */
    Fx *fx = fx_html("<div style='height:300px;width:400px'>"
                     "<table id=et style='overflow-y:auto;height:min-content;max-height:100%;width:100%'>"
                     "<tr id=tr0><td id=a0>1.</td><td id=b0>Guest</td><td id=c0>0</td></tr>"
                     "<tr id=tr1><td id=a1>2.</td><td id=b1>Somebody Long</td><td id=c1>12</td></tr></table></div>", NULL);
    HtmlRect a0 = rc(fx, "a0"), b0 = rc(fx, "b0"), c0 = rc(fx, "c0"), a1 = rc(fx, "a1"), b1 = rc(fx, "b1"), et = rc(fx, "et");
    CHECK(NEAR(a0.y, b0.y, 0.01) && NEAR(b0.y, c0.y, 0.01) && a0.x < b0.x && b0.x < c0.x, "cells of a row share a line (y %.1f %.1f %.1f)", a0.y, b0.y, c0.y);
    CHECK(a1.y > a0.y + a0.h - 0.01 && NEAR(a1.x, a0.x, 0.01) && NEAR(b1.x, b0.x, 0.01), "second row below, columns aligned");
    CHECK(NEAR(et.w, 400, 0.01), "table width:100%% (%.1f)", et.w);
    CHECK(NEAR(a0.x - et.x, 2, 0.01) && NEAR(a0.y - et.y, 2, 0.01), "UA border-spacing 2px (%.1f,%.1f)", a0.x - et.x, a0.y - et.y);
    CHECK(et.h < 100, "table height is content-sized (%.1f)", et.h);
    Box *tr = bx(fx, "tr0");
    CHECK(tr && tr->type == BOX_TABLE_ROW && tr->parent == bx(fx, "et"), "<tr> rows are direct rows of the table (tbody hoisted)");
    fx_free(fx);

    /* Krunker's `td { height: 50px }` (main.css): the specified height already makes the cell
     * as tall as the row, so vertical-align: middle (UA tbody) must centre by the content's
     * height, not by c->h - row_h (= 0): Chrome puts the text (row 50px) 50/2 - line/2 down. */
    fx = fx_html("<table style='border-spacing:0'><tr><td id=hc style='height:50px;padding:1px;font-size:20px'>1.</td>"
                 "<td id=hn style='padding:1px;font-size:20px'>Guest</td></tr></table>", NULL);
    Box *hc = bx(fx, "hc"), *hn = bx(fx, "hn");
    const TextRun *rh = hc ? first_run(hc) : NULL, *rn = hn ? first_run(hn) : NULL;
    HtmlRect hcr = rc(fx, "hc");
    CHECK_NEAR(hcr.h, 52, 0.01, "td height:50px + padding is the row height");
    if (rh && rn && hc) {
        const float line = hc->line_count ? hc->lines[0].h : 0;
        const float top = hc->line_count ? hc->lines[0].y : -1; /* the line box, cell-local */
        CHECK(NEAR(top, (52 - line) * 0.5f, 0.6f), "height:50px cell centres its text (line top %.2f, line %.2f)", top, line);
        CHECK(NEAR(rn->y, rh->y, 0.01), "the auto-height cell of the row centres the same (%.2f %.2f)", rn->y, rh->y);
    } else {
        CHECK(0, "height:50px cells have text runs");
    }
    fx_free(fx);
}

static void t_ellipsis_owner(void)
{
    /* like .classCard: the card clips + ellipsis, the title is direct text next to block children
     * (anonymous block wrapper), and another card has the title inside a span */
    const char *css = ".card { overflow: hidden; white-space: nowrap; text-overflow: ellipsis; width: 120px; font-size: 26px; }";
    Fx *fx = fx_html("<div class=card id=c1>Spray N Pray Long<div style='height:10px'></div></div>"
                     "<div class=card id=c2><span id=sp>Reflex Sight Long</span></div>"
                     "<div class=card id=c3>Short<div style='height:10px'></div></div>"
                     "<div id=c4 style='overflow:hidden;white-space:nowrap;width:120px;font-size:26px'>Combat Knife Long<div></div></div>", css);
    const TextRun *r1 = first_run(bx(fx, "c1")), *r2 = first_run(bx(fx, "c2")), *r3 = first_run(bx(fx, "c3")), *r4 = first_run(bx(fx, "c4"));
    CHECK(r1 && r1->ellipsis && r1->width + r1->ellipsis_w <= 120.01f, "anonymous block uses the card's text-overflow (%d, %.1f)",
          r1 ? r1->ellipsis : -1, r1 ? r1->width + r1->ellipsis_w : -1);
    CHECK(r2 && r2->ellipsis && r2->width + r2->ellipsis_w <= 120.01f, "ellipsis applies to text inside a span");
    /* gamefont has no U+2026: three '.' from gamefont, like Chrome */
    TextFont *gf = text_font(g_ts, "gamefont", 26, 400, 0);
    CHECK(r1 && r1->ellipsis_count == 3 && r1->ellipsis_glyphs[0].face == text_font_face(gf)
          && NEAR(r1->ellipsis_w, text_measure(gf, "...", 3, 0), 0.01) && NEAR(r1->ellipsis_glyphs[2].x, 2 * r1->ellipsis_glyphs[1].x, 0.01),
          "ellipsis falls back to three periods of the primary font (count %d)", r1 ? r1->ellipsis_count : -1);
    CHECK(r3 && !r3->ellipsis && r3->len == 5, "text that fits is not truncated");
    CHECK(r4 && !r4->ellipsis, "no ellipsis without text-overflow (clip)");
    fx_free(fx);
}

static void t_inline_block_block_baseline(void)
{
    /* like win-games .mapListItem: inline-block whose last child is a flex row of text */
    Fx *fx = fx_html("<div id=grid style='font-size:20px;width:460px'>"
                     "<div id=i1 style='display:inline-block;width:220px;margin-bottom:12px;font-size:17px'>"
                     "<div style='height:100px'></div><div>Name</div>"
                     "<div id=fl style='display:flex'><span style='font-size:15px'>by x</span><div style='font-size:15px;flex-grow:1'>0</div></div></div>"
                     "<div id=i2 style='display:inline-block;width:220px;margin-bottom:12px;font-size:17px'>"
                     "<div style='height:100px'></div><div>Name</div>"
                     "<div style='display:flex'><span style='font-size:15px'>by x</span><div style='font-size:15px;flex-grow:1'>0</div></div></div>"
                     "<div id=i3 style='display:inline-block;width:220px;margin-bottom:12px;font-size:17px'>"
                     "<div style='height:100px'></div><div>Name</div></div></div>", NULL);
    TextFont *f17 = text_font(g_ts, "gamefont", 17, 400, 0), *f15 = text_font(g_ts, "gamefont", 15, 400, 0), *f20 = text_font(g_ts, "gamefont", 20, 400, 0);
    float h17 = text_font_metrics(f17)->line_height, h15 = text_font_metrics(f15)->line_height;
    float a15 = text_font_metrics(f15)->ascent, d20 = text_font_metrics(f20)->descent;
    Box *i1 = bx(fx, "i1");
    float item_h = 100 + h17 + h15;
    CHECK(i1 && NEAR(i1->h, item_h, 0.01), "item height (%.1f vs %.1f)", i1 ? i1->h : -1, item_h);
    CHECK(i1 && NEAR(i1->baseline_offset, 100 + h17 + a15, 0.01), "inline-block baseline from the last flex line (%.2f)", i1 ? i1->baseline_offset : -1);
    HtmlRect r1 = rc(fx, "i1"), r3 = rc(fx, "i3");
    float below = item_h + 12 - (100 + h17 + a15);   /* margin-box part under the baseline */
    float pitch = item_h + 12 + (d20 > below ? d20 - below : 0);
    CHECK_NEAR(r3.y - r1.y, pitch, 0.01, "row pitch: no extra strut descent under inline-blocks with a baseline");
    fx_free(fx);
}

static void t_scroll_extent(void)
{
    /* like #menuWindow (padding:20px, overflow-y:auto): content that fits gets no bar */
    Fx *fx = fx_html("<div id=fit style='overflow-y:auto;padding:20px;height:100px;width:200px'><div style='height:100px'></div></div>"
                     "<div id=over style='overflow-y:auto;padding:20px;height:100px;width:200px'><div style='height:120px'></div></div>"
                     "<div id=mrg style='overflow-y:auto;padding:20px;height:100px;width:200px'><div style='height:100px;margin-bottom:10px'></div></div>"
                     "<div id=txt style='overflow-y:auto;padding:20px;height:100px;width:200px;line-height:25px'>a<br>b<br>c<br>d</div>"
                     "<div id=abs style='overflow:hidden;position:relative;padding:10px;height:50px;width:200px'>"
                     "<div style='position:absolute;top:100px;height:30px;width:10px'></div></div>", NULL);
    Box *fit = bx(fx, "fit"), *over = bx(fx, "over"), *mrg = bx(fx, "mrg"), *txt = bx(fx, "txt"), *ab = bx(fx, "abs");
    CHECK(fit && NEAR(fit->scroll_h, 140, 0.01) && fit->scrollbar_w == 0, "fitting content: scrollHeight = clientHeight 140, no bar (%.1f, bar %.0f)",
          fit ? fit->scroll_h : -1, fit ? fit->scrollbar_w : -1);
    CHECK(over && NEAR(over->scroll_h, 160, 0.01) && over->scrollbar_w == 12, "overflow: 20 + 120 + 20 = 160 (%.1f)", over ? over->scroll_h : -1);
    CHECK(mrg && NEAR(mrg->scroll_h, 150, 0.01), "last child's margin-bottom counted, padding once (%.1f)", mrg ? mrg->scroll_h : -1);
    CHECK(txt && NEAR(txt->scroll_h, 140, 0.01), "four 25px lines + padding = 140 (%.1f)", txt ? txt->scroll_h : -1);
    CHECK(ab && NEAR(ab->scroll_h, 130, 0.01), "abs box extends scrollHeight by its border box only (%.1f)", ab ? ab->scroll_h : -1);
    fx_free(fx);
}

static void t_closed_inline_states(void)
{
    /* like #pingDisplay: top-aligned text span (closed) + taller top-aligned inline-block icon */
    Fx *fx = fx_html("<div id=pd style='font-size:18px;margin-top:20px'><span id=pt style='vertical-align:top'>2</span>"
                     "<i id=ic style='display:inline-block;vertical-align:top;width:32px;height:32px'></i></div>"
                     "<div id=two style='width:400px'><span id=s1 style='padding:0 4px'>ab</span> mid <span id=s2>cd</span></div>", NULL);
    HtmlRect pd = rc(fx, "pd"), ic = rc(fx, "ic");
    Box *pt = bx(fx, "pt");
    const TextRun *r = pt ? first_run(pt) : NULL;
    TextFont *f = text_font(g_ts, "gamefont", 18, 400, 0);
    const TextMetrics *m = text_font_metrics(f);
    float lead = floorf((m->line_height - m->ascent - m->descent) * 0.5f + 1e-4f);   /* Blink: floor(leading / 2) above */
    CHECK(r && NEAR(r->y, pd.y + lead + m->ascent, 0.01), "closed top-aligned span: text at the line top (baseline %.2f, want %.2f)",
          r ? r->y : -1, pd.y + lead + m->ascent);
    CHECK_NEAR(ic.y, pd.y, 0.01, "top-aligned inline-block at the line top");
    CHECK_NEAR(pd.h, 32, 0.01, "line height from the tall top-aligned box");
    Box *s1 = bx(fx, "s1"), *s2 = bx(fx, "s2");
    CHECK(s1 && s1->frag_count == 1 && s1->frags[0].first && s1->frags[0].last, "inline box closed mid-line has its fragment");
    CHECK(s2 && s2->frag_count == 1 && s2->w > 0, "last inline box on the line has its fragment (w %.1f)", s2 ? s2->w : -1);
    fx_free(fx);
}

static void t_half_leading_floor(void)
{
    /* Blink CalculateLeadingSpace: the ascent side gets floor(leading / 2),
     * so an odd / fractional leading keeps the baseline on a whole px.
     * Chrome 154: gamefont 25px (ascent 28, descent 6), line-height 34 + L. */
    Fx *fx = fx_html("<div id=a style='font-size:25px;line-height:35px'>F<i id=ai style='display:inline-block;width:1px;height:0'></i></div>"
                     "<div id=b style='font-size:25px;line-height:35.6px'>F<i id=bi style='display:inline-block;width:1px;height:0'></i></div>"
                     "<div id=c style='font-size:25px;line-height:33px'>F<i id=ci style='display:inline-block;width:1px;height:0'></i></div>", NULL);
    TextFont *f = text_font(g_ts, "gamefont", 25, 400, 0);
    const TextMetrics *m = text_font_metrics(f);
    const float A = m->ascent, D = m->descent;
    HtmlRect a = rc(fx, "a"), ai = rc(fx, "ai"), b = rc(fx, "b"), bi = rc(fx, "bi"), c = rc(fx, "c"), ci = rc(fx, "ci");
    CHECK_NEAR(ai.y - a.y, floorf((35.0f - A - D) * 0.5f) + A, 0.01, "line-height 35: odd leading, baseline floor(L/2) + ascent");
    CHECK_NEAR(bi.y - b.y, floorf((35.6f - A - D) * 0.5f) + A, 0.01, "line-height 35.6: fractional leading floored above");
    CHECK_NEAR(ci.y - c.y, floorf((33.0f - A - D) * 0.5f) + A, 0.01, "line-height 33: negative leading floors (-1 above)");
    CHECK_NEAR(b.h, 35.6f, 0.01, "line box keeps the full line-height");
    fx_free(fx);
}

static void t_float_relayout(void)
{
    /* like the settings windows: a float inside a block whose top margin collapses
     * (forces the parent to re-lay the child out), then clearing content */
    Fx *fx = fx_html("<div id=hold style='width:400px'>"
                     "<div id=hed style='margin-top:15px'>Head<span id=fl style='float:right;width:50px;height:10px'></span></div>"
                     "<div id=bod style='margin-top:5px'><div id=row style='clear:both;height:10px'>x"
                     "<span id=fl2 style='float:right;width:20px;height:5px'></span></div>"
                     "<div id=row2 style='clear:both;height:10px'></div></div></div>", NULL);
    HtmlRect hed = rc(fx, "hed"), fl = rc(fx, "fl"), bod = rc(fx, "bod"), row = rc(fx, "row"), row2 = rc(fx, "row2"), fl2 = rc(fx, "fl2");
    CHECK_NEAR(fl.y, hed.y, 0.01, "float stays in its line after the parent re-layout");
    CHECK_NEAR(fl.x, 350, 0.01, "float right edge");
    CHECK_NEAR(row.y, bod.y, 0.01, "clear: no stale float copies below");
    CHECK_NEAR(fl2.y, row.y, 0.01, "nested float on its line");
    CHECK_NEAR(row2.y, row.y + 10, 0.01, "second row right after the first");
    fx_free(fx);
}

static void t_anonymous_style(void)
{
    /* like .classCard: padding + height on the element, mixed block/inline children */
    Fx *fx = fx_html("<div id=card style='padding:10px;height:286px;margin-bottom:10px;width:200px;background:red'>"
                     "Name<div id=blk style='height:20px'></div>tail<div id=last style='height:5px'></div></div>"
                     "<div id=after style='height:5px'></div>", NULL);
    Box *card = bx(fx, "card");
    Box *anon = card ? card->first_child : NULL;
    CHECK(anon && (anon->flags & BOXF_ANONYMOUS), "anonymous wrapper exists");
    if (anon) {
        CHECK(anon->style != card->style, "anonymous box has its own style");
        CHECK(NEAR(anon->padding[CSS_TOP], 0, 0.01) && NEAR(anon->margin[CSS_BOTTOM], 0, 0.01), "anonymous box: no padding/margin");
        CHECK(anon->style->height.kind == CSS_LEN_AUTO && anon->style->background_color.a == 0, "anonymous box: initial height/background");
        CHECK(NEAR(anon->x, 10, 0.01) && NEAR(anon->y, 10, 0.01) && anon->h < 40, "anonymous box inside the content box (%.1f,%.1f h %.1f)", anon->x, anon->y, anon->h);
        CHECK(anon->style->font_size == card->style->font_size && anon->style->color.r == card->style->color.r, "anonymous box inherits inherited properties");
    }
    HtmlRect blk = rc(fx, "blk"), last = rc(fx, "last");
    CHECK(blk.y > 10 && blk.y < 40, "block after the anonymous line (%.1f)", blk.y);
    CHECK(last.y + last.h < 10 + 286, "children stay inside the card (%.1f)", last.y + last.h);
    CHECK_NEAR(rc(fx, "after").y, 306 + 10, 0.01, "next sibling after card height + margin");
    /* restyle of the parent re-inherits */
    HtmlElem ce = dom_by_id(fx->dom, "card");
    CssComputed old = *dom_elem_data(fx->dom, ce)->computed;
    dom_set_style(fx->dom, ce, "font-size", "30px");
    cascade_restyle(fx->dom, g_ua, fx->sheets, fx->ns);
    layout_restyled(fx->tree, dom_elem_node(fx->dom, ce), &old);
    layout_run(fx->tree, fx->vw, fx->vh);
    anon = bx(fx, "card")->first_child;
    CHECK(anon && anon->style->font_size == 30 && NEAR(anon->padding[CSS_TOP], 0, 0.01), "anonymous style re-inherited on restyle");
    fx_free(fx);
}

/* CSS Sizing 3 5.2.2, Chrome 152: a replaced box whose width or max-width is a percentage is
 * compressible, its min-content contribution only its border and padding; and a max-width that
 * clamps an auto-height image's width carries over to its height (256 x 256 img/app_0.png). */
static void t_compressible_replaced(void)
{
    Fx *fx = fx_html(
        "<div id=a style='width:min-content'><img id=ai style='width:100%' src='img/app_0.png'></div>"
        "<div id=b style='width:min-content'><img style='width:50%;padding:5px;border:2px solid red' src='img/app_0.png'></div>"
        "<div id=c style='width:min-content'><img id=ci style='max-width:100%' src='img/app_0.png'></div>"
        "<div id=d style='width:max-content'><img style='width:100%' src='img/app_0.png'></div>"
        "<div id=e style='width:min-content'><img style='width:100px' src='img/app_0.png'></div>", NULL);
    CHECK_NEAR(rc(fx, "a").w, 0, 0.01, "width: 100%% img: min-content 0");
    CHECK_NEAR(rc(fx, "b").w, 14, 0.01, "width: 50%% img: min-content = padding + border");
    CHECK_NEAR(rc(fx, "c").w, 0, 0.01, "max-width: 100%% img: min-content 0");
    CHECK_NEAR(rc(fx, "ci").h, 0, 0.01, "max-width clamps the width to 0: the height follows the ratio");
    CHECK_NEAR(rc(fx, "d").w, 256, 0.01, "max-content keeps the natural width");
    CHECK_NEAR(rc(fx, "e").w, 100, 0.01, "a fixed width is not compressible");
    fx_free(fx);
}

static void t_root_overflow(void)
{
    /* <html>/<body> overflow:hidden and 0px tall (everything positioned), like ui/index.html */
    Fx *fx = fx_html("<div id=ov style='position:absolute;left:10px;top:10px;width:100px;height:100px'></div>",
                     "html, body { overflow: hidden; }");
    Box *root = layout_tree_root(fx->tree);
    CHECK(root && NEAR(root->h, 0, 0.01) && (root->flags & BOXF_CLIPS), "root is 0px tall and clipping (h %.1f)", root ? root->h : -1);
    Box *b = layout_hit_test(fx->tree, 50, 50, 1);
    CHECK(b && strcmp(box_id(b), "ov") == 0, "root overflow clips to the viewport, not to <html> (%s)", box_id(b));
    fx_free(fx);
    /* root visible, body hidden: body's overflow propagates to the viewport, body does not clip */
    fx = fx_html("<div id=ov2 style='position:absolute;left:10px;top:10px;width:100px;height:100px'></div>",
                 "body { overflow: hidden; height: 0; }");
    b = layout_hit_test(fx->tree, 50, 50, 1);
    CHECK(b && strcmp(box_id(b), "ov2") == 0, "propagated body overflow clips to the viewport (%s)", box_id(b));
    fx_free(fx);
    /* a non-propagating body clip still clips (root has its own overflow) */
    fx = fx_html("<div id=ov3 style='position:relative;left:10px;top:10px;width:100px;height:100px'></div>",
                 "html { overflow: hidden; } body { overflow: hidden; height: 20px; }");
    b = layout_hit_test(fx->tree, 50, 50, 1);
    CHECK(!b || strcmp(box_id(b), "ov3") != 0, "body's own clip applies when the root's overflow propagates (%s)", box_id(b));
    b = layout_hit_test(fx->tree, 50, 15, 1);
    CHECK(b && strcmp(box_id(b), "ov3") == 0, "inside body's clip (%s)", box_id(b));
    fx_free(fx);
}

static void t_container_scrollbars(void)
{
    Fx *fx = fx_html(
        "<div id=fs style='display:flex;flex-direction:column;align-items:center;overflow-y:scroll;width:200px;height:100px'>"
        "<div id=fs1 style='height:30px;width:50px'></div></div>"
        "<div id=fa style='display:flex;flex-direction:column;overflow-y:auto;width:200px;height:100px'>"
        "<div id=fa1 style='height:300px;flex-shrink:0'></div></div>"
        "<div id=fr style='display:flex;overflow-y:scroll;direction:rtl;width:200px;height:100px'>"
        "<div id=fr1 style='flex-grow:1;height:20px'></div></div>"
        "<div id=gs style='display:grid;grid-template-columns:auto auto;overflow-y:auto;width:200px;height:50px'>"
        "<div id=gs1 style='height:80px'>a</div><div>b</div></div>"
        "<div id=ts style='display:table;overflow-y:scroll;width:200px;height:50px'>"
        "<div id=tc style='display:table-cell'>a</div></div>"
        "<div id=fn style='display:flex;overflow-y:auto;width:200px;height:100px'><div style='height:10px'></div></div>", NULL);
    struct { const char *id; int bar; } cases[] = { { "fs", 1 }, { "fa", 1 }, { "fr", 1 }, { "gs", 1 }, { "ts", 1 }, { "fn", 0 } };
    for (int i = 0; i < 6; i++) {
        Box *b = bx(fx, cases[i].id);
        LayoutScrollbar sb;
        int has = b && layout_scrollbar(fx->tree, b, 1, &sb);
        CHECK(has == cases[i].bar && (!b || NEAR(b->scrollbar_w, cases[i].bar ? 12 : 0, 0.01)),
              "#%s: vertical bar %d (scrollbar_w %.1f)", cases[i].id, has, b ? b->scrollbar_w : -1);
    }
    HtmlRect fs = rc(fx, "fs"), fs1 = rc(fx, "fs1");
    CHECK_NEAR(fs1.x - fs.x, (188 - 50) * 0.5f, 0.01, "flex scroll: items centred in the content box minus the bar");
    HtmlRect fa = rc(fx, "fa"), fa1 = rc(fx, "fa1");
    CHECK_NEAR(fa1.w, 188, 0.01, "flex auto: overflowing column stretches to the width minus the bar");
    CHECK_NEAR(bx(fx, "fa")->scroll_h, 300, 0.01, "flex auto: scrollHeight");
    (void)fa;
    HtmlRect fr = rc(fx, "fr"), fr1 = rc(fx, "fr1");
    Box *frb = bx(fx, "fr");
    LayoutScrollbar sb;
    if (frb && layout_scrollbar(fx->tree, frb, 1, &sb)) CHECK_NEAR(sb.track_x, fr.x, 0.01, "flex rtl: bar on the left");
    CHECK(NEAR(fr1.x, fr.x + 12, 0.01) && NEAR(fr1.w, 188, 0.01), "flex rtl: items right of the bar (%.1f, w %.1f)", fr1.x - fr.x, fr1.w);
    HtmlRect gs = rc(fx, "gs"), gs1 = rc(fx, "gs1");
    CHECK(NEAR(gs1.x, gs.x, 0.01) && gs1.w < 100 - 5, "grid auto: columns share 188px (w %.1f)", gs1.w);
    HtmlRect tc = rc(fx, "tc"), ts = rc(fx, "ts");
    CHECK_NEAR(tc.w, 188, 0.01, "table scroll: cell width excludes the bar");
    (void)ts;
    fx_free(fx);
}

/* The Object Manager's window (ui/css/krunknative.css .knOm*): an abs-pos flex column under
 * max-height: calc(100% - 230px) whose middle row shrinks (min-height:0) and whose panes scroll:
 * the grid (flex 0 1 auto, max-height) beside the Tools pane, an abs-pos inset:0 flex column in a
 * stretched row item (no height of its own) holding a fixed title and a scrolling list. */
static void t_flex_column_scroll_panes(void)
{
    static const char *css =
        "#cb { position:absolute; left:0; top:0; width:1200px; height:900px; }"
        "#win { position:absolute; left:20px; top:10px; width:1000px; padding:20px; max-height:calc(100% - 230px);"
        "  overflow-y:auto; display:flex; flex-direction:column; }"
        ".om { display:flex; flex-direction:column; flex:1 1 auto; min-height:0; }"
        "#title { height:31px; flex:none; } #top { height:75px; flex:none; } #foot { height:72px; flex:none; }"
        "#main { display:flex; align-items:stretch; flex:1 1 auto; min-height:0; }"
        "#body { display:flex; flex-direction:column; flex-grow:1; min-width:0; min-height:0; }"
        "#status { height:24px; flex:none; }"
        "#grid { flex:0 1 auto; min-height:0; max-height:404px; overflow-y:auto; }"
        "#tools { position:relative; width:216px; flex-shrink:0; }"
        "#tin { position:absolute; top:0; right:0; bottom:0; left:0; display:flex; flex-direction:column; padding:6px; }"
        "#ttitle { height:22px; flex:none; }"
        "#list { flex:1 1 auto; min-height:0; overflow-y:auto; }"
        ".row { height:30px; }";
    char rows[2048] = "", tiles[2048] = "";
    for (int i = 0; i < 16; i++) strcat(rows, "<div class=row></div>");     /* 480px of tools */
    for (int i = 0; i < 12; i++) strcat(tiles, "<div class=row style='height:100px'></div>"); /* 1200px of grid */
    char html[8192];
    snprintf(html, sizeof(html),
             "<div id=cb><div id=win><div class=om><div id=title></div><div id=top></div><div id=main>"
             "<div id=body><div id=status></div><div id=grid>%s</div></div>"
             "<div id=tools><div id=tin><div id=ttitle></div><div id=list>%s<div id=last class=row></div></div></div></div>"
             "</div><div id=foot></div></div></div></div>", tiles, rows);
    Fx *fx = fx_html(html, css);
    Box *win = bx(fx, "win"), *grid = bx(fx, "grid"), *list = bx(fx, "list"), *mainb = bx(fx, "main"), *tools = bx(fx, "tools");
    if (!win || !grid || !list || !mainb || !tools) { fx_free(fx); return; }
    /* max-height 900 - 230 = 670 (content box): the natural 31 + 75 + 428 + 72 = 606 fits */
    CHECK_NEAR(win->h, 606 + 40, 0.5, "om window: its natural height under max-height calc(100% - 230px)");
    CHECK_NEAR(mainb->h, 428, 0.5, "om window: the row is the status line + the grid's max-height");
    CHECK_NEAR(tools->h, mainb->h, 0.5, "om window: the Tools pane stretches to the row (its content is abs-pos)");
    CHECK_NEAR(list->h, mainb->h - 12 - 22, 0.5, "om window: the Tools list fills the pane below its title");
    CHECK(list->flags & BOXF_SCROLL_CONTAINER, "om window: the Tools list is a scroll container");
    CHECK_NEAR(list->scroll_h, 17 * 30, 0.5, "om window: the Tools list's scrollHeight is all its rows");
    CHECK(list->scrollbar_w > 0, "om window: the Tools list shows a bar (scrollbar_w %.1f)", list->scrollbar_w);
    CHECK_NEAR(grid->h, 404, 0.5, "om window: the grid keeps its max-height");
    CHECK(win->scroll_h <= win->h - win->border[0] - win->border[2] + 0.5f, "om window: the window itself does not overflow (scrollHeight %.1f)", win->scroll_h);
    fx_free(fx);

    /* a short window (a 700 px cb: max-height 470, the content box): the row shrinks, the header
     * and footer stay (it used to keep its natural 428 px and the footer overflowed the window) */
    char css2[2048];
    snprintf(css2, sizeof(css2), "%s #cb { height:700px; }", css);
    fx = fx_html(html, css2);
    win = bx(fx, "win"); grid = bx(fx, "grid"); list = bx(fx, "list"); mainb = bx(fx, "main");
    Box *foot = bx(fx, "foot"), *title = bx(fx, "title");
    if (win && grid && list && mainb && foot && title) {
        CHECK_NEAR(win->h, 470 + 40, 0.5, "short om window: max-height");
        CHECK_NEAR(mainb->h, 470 - 31 - 75 - 72, 0.5, "short om window: the row shrinks to what is left");
        CHECK_NEAR(grid->h, mainb->h - 24, 0.5, "short om window: the grid shrinks under its max-height");
        CHECK_NEAR(list->h, mainb->h - 12 - 22, 0.5, "short om window: the Tools list shrinks with the row");
        CHECK_NEAR(foot->y + foot->h, win->y + win->h - 20, 0.5, "short om window: the footer is at the bottom, in view");
        CHECK(win->scroll_h <= win->h - win->border[0] - win->border[2] + 0.5f, "short om window: nothing overflows the window (scrollHeight %.1f)", win->scroll_h);
        CHECK(title->y >= win->y, "short om window: the title stays");
    }
    fx_free(fx);
}

/* A restyled block's new font must be used when a shrink-to-fit ancestor measures it:
 * layout_box() resolves the font later, and measuring with the stale one made the
 * abs-pos card narrower than its text (the spin result's "by <creator>" wrapped). */
static void t_intrinsic_font_restyle(void)
{
    Fx *fx = fx_html(
        "<div id=icb style='position:absolute;width:2000px;height:400px'>"
        "<div id=card style='position:absolute;left:50%;font-size:30px'>Ab<div id=sub style='font-size:10px'>by Krunker.io</div></div></div>",
        NULL);
    HtmlElem e = dom_by_id(fx->dom, "sub");
    ElemData *ed = dom_elem_data(fx->dom, e);
    CssComputed old = *ed->computed;
    dom_set_style(fx->dom, e, "font-size", "20px");
    cascade_restyle(fx->dom, g_ua, fx->sheets, fx->ns);
    layout_restyled(fx->tree, dom_elem_node(fx->dom, e), &old);
    layout_run(fx->tree, fx->vw, fx->vh);
    CHECK(count_lines(bx(fx, "sub")) == 1, "intrinsic after font restyle: text fits its shrink-to-fit card (%d lines)", count_lines(bx(fx, "sub")));
    CHECK_NEAR(rc(fx, "card").w, tw("gamefont", 20.0f, "by Krunker.io"), 0.01, "intrinsic after font restyle: card width = new max-content");
    fx_free(fx);
}

/* A display:none subtree shown again restyles only what changed while it was hidden (om-perf-2:
 * forcing the whole subtree restyled every Object Manager tile on each tab click), and those
 * changes all arrive: a descendant's own style, a class on another, an inherited value from above,
 * and a child inserted while hidden. */
static void t_show_restyles_changes(void)
{
    Fx *fx = fx_html("<div id=top style='color:red'><div id=pane style='display:none'><div id=x class=a>x</div><div id=y>y</div>"
                     "<div id=z>z</div></div></div>", ".a { height: 5px; } .b { height: 7px; }");
    Dom *d = fx->dom;
    HtmlElem pane = dom_by_id(d, "pane"), x = dom_by_id(d, "x"), y = dom_by_id(d, "y"), top = dom_by_id(d, "top");
    dom_set_style(d, x, "width", "30px");          /* own style while hidden */
    dom_set_class(d, y, "b");                       /* a class while hidden */
    dom_set_style(d, top, "color", "blue");         /* an inherited value from above */
    cascade_restyle(d, g_ua, fx->sheets, fx->ns);
    dom_set_style(d, pane, "display", "block");
    const uint32_t n = cascade_restyle(d, g_ua, fx->sheets, fx->ns);
    const CssComputed *cx = dom_elem_data(d, x)->computed, *cy = dom_elem_data(d, y)->computed, *cz = dom_elem_data(d, dom_by_id(d, "z"))->computed;
    CHECK(cx && cx->width.kind == CSS_LEN_VALUE && NEAR(cx->width.px, 30, 0.01), "shown: the hidden child's own style applied");
    CHECK(cy && NEAR(cy->height.px, 7, 0.01), "shown: the class set while hidden applied");
    CHECK(cz && cz->color.b == 255 && cz->color.r == 0, "shown: the inherited colour from above reached every child");
    CHECK(n >= 1, "shown: restyled %u elements", n);
    fx_free(fx);
}

static void t_incremental(void)
{
    Fx *fx = fx_html(
        "<div id=wrap style='width:300px'><div id=a style='height:10px;color:red'></div>"
        "<div id=b style='height:20px'>text</div><div id=c style='height:5px'></div></div>"
        "<div id=fc style='display:flex;flex-direction:column;justify-content:center;align-items:center;"
        "width:200px;height:200px;position:relative'><div id=ab style='position:absolute;width:20px;height:10px;margin-top:-6px'></div>"
        "<div id=it style='width:10px;height:10px'></div></div>"
        "<div id=hud style='width:100px'><span id=sp>a</span></div>"
        "<div id=host style='width:100px'><div id=hidden><img id=bob style='width:10px;height:10px'></div>"
        "<div id=keep style='height:10px'></div></div>", NULL);
    HtmlRect fc = rc(fx, "fc"), ab = rc(fx, "ab");
    CHECK(NEAR(ab.x - fc.x, 90, 0.01) && NEAR(ab.y - fc.y, 95 - 3, 0.01),
          "flex static position: abs child centred like a sole item (%.1f,%.1f)", ab.x - fc.x, ab.y - fc.y);

    HtmlElem ea = dom_by_id(fx->dom, "a");
    ElemData *ed = dom_elem_data(fx->dom, ea);
    CssComputed old = *ed->computed;
    Box *box_a = (Box *)ed->box;
    dom_set_style(fx->dom, ea, "color", "blue");
    cascade_restyle(fx->dom, g_ua, fx->sheets, fx->ns);
    int r = layout_restyled(fx->tree, dom_elem_node(fx->dom, ea), &old);
    CHECK(r == LAYOUT_RESTYLE_PAINT && ed->box == box_a && box_a->paint_dirty, "restyle: color -> PAINT in place (%d)", r);
    CHECK(layout_run(fx->tree, fx->vw, fx->vh) == 0, "restyle: paint-only change needs no layout");

    old = *ed->computed;
    dom_set_style(fx->dom, ea, "height", "40px");
    cascade_restyle(fx->dom, g_ua, fx->sheets, fx->ns);
    r = layout_restyled(fx->tree, dom_elem_node(fx->dom, ea), &old);
    CHECK(r == LAYOUT_RESTYLE_LAYOUT, "restyle: height -> LAYOUT (%d)", r);
    layout_run(fx->tree, fx->vw, fx->vh);
    CHECK_NEAR(rc(fx, "b").y, 40, 0.01, "restyle: sibling moved after relayout");

    old = *ed->computed;
    dom_set_style(fx->dom, ea, "transform", "translateX(5px)");
    cascade_restyle(fx->dom, g_ua, fx->sheets, fx->ns);
    r = layout_restyled(fx->tree, dom_elem_node(fx->dom, ea), &old);
    CHECK(r == LAYOUT_RESTYLE_TRANSFORM && NEAR(rc(fx, "a").x, 5, 0.01), "restyle: transform -> TRANSFORM (%d, x %.1f)", r, rc(fx, "a").x);

    old = *ed->computed;
    dom_set_style(fx->dom, ea, "display", "none");
    cascade_restyle(fx->dom, g_ua, fx->sheets, fx->ns);
    r = layout_restyled(fx->tree, dom_elem_node(fx->dom, ea), &old);
    layout_run(fx->tree, fx->vw, fx->vh);
    CHECK(r == LAYOUT_RESTYLE_REBUILT && ed->box == NULL, "restyle: display none -> REBUILT, box gone (%d)", r);
    CHECK_NEAR(rc(fx, "b").y, 0, 0.01, "restyle: siblings close the gap");

    old = *ed->computed;
    dom_set_style(fx->dom, ea, "display", "");
    cascade_restyle(fx->dom, g_ua, fx->sheets, fx->ns);
    r = layout_restyled(fx->tree, dom_elem_node(fx->dom, ea), &old);
    layout_run(fx->tree, fx->vw, fx->vh);
    CHECK(r == LAYOUT_RESTYLE_REBUILT && ed->box != NULL && NEAR(rc(fx, "b").y, 40, 0.01), "restyle: displayed again (%d)", r);

    /* an animation inside a display:none subtree (the menu's #merchImg while in game):
     * its (stale, still set) computed style is animated, no box appears, and the nearest
     * rendered ancestor keeps its boxes */
    HtmlElem hidden = dom_by_id(fx->dom, "hidden"), bob = dom_by_id(fx->dom, "bob");
    ElemData *hed = dom_elem_data(fx->dom, hidden), *bed = dom_elem_data(fx->dom, bob);
    old = *hed->computed;
    dom_set_style(fx->dom, hidden, "display", "none");
    cascade_restyle(fx->dom, g_ua, fx->sheets, fx->ns);
    layout_restyled(fx->tree, dom_elem_node(fx->dom, hidden), &old);
    layout_run(fx->tree, fx->vw, fx->vh);
    Box *keep_box = bx(fx, "keep");
    const int had_style = bed && bed->computed;
    if (had_style) old = *bed->computed;
    r = layout_restyled(fx->tree, dom_elem_node(fx->dom, bob), had_style ? &old : NULL);
    CHECK(had_style && r == LAYOUT_RESTYLE_NONE && bed->box == NULL && bx(fx, "keep") == keep_box,
          "restyle inside display:none: nothing rebuilt (%d)", r);

    HtmlElem hud = dom_by_id(fx->dom, "hud");
    for (int i = 0; i < 50; i++) {   /* HUD-style innerHTML churn reuses released boxes */
        char html[128];
        snprintf(html, sizeof(html), "<span id=sp>%d</span><div id=row style='height:%dpx'>x</div>", i, 10 + i % 3);
        dom_set_html(fx->dom, hud, html, strlen(html));
        cascade_restyle(fx->dom, g_ua, fx->sheets, fx->ns);
        r = layout_children_changed(fx->tree, dom_elem_node(fx->dom, hud));
        layout_run(fx->tree, fx->vw, fx->vh);
    }
    CHECK(r == LAYOUT_RESTYLE_REBUILT, "children changed -> REBUILT");
    HtmlRect row = rc(fx, "row"), hr = rc(fx, "hud");
    Box *spb = bx(fx, "sp");
    const TextRun *tr = spb ? first_run(spb) : NULL;
    CHECK(tr && tr->len == 2 && memcmp(tr->text, "49", 2) == 0, "innerHTML: new text laid out");
    CHECK(NEAR(row.h, 10 + 49 % 3, 0.01) && row.y > hr.y, "innerHTML: new block laid out (%.1f)", row.h);
    layout_rebuild_all(fx->tree);
    layout_run(fx->tree, fx->vw, fx->vh);
    CHECK_NEAR(rc(fx, "b").y, 40, 0.01, "rebuild_all keeps the same tree usable");
    fx_free(fx);
}

/* Flex shrink (CSS Flexbox 9.7) against Chrome 152: a width: Npx item shrinks down to its
 * automatic minimum size, the min-content of its contents (not N); the shrink weight is
 * flex-shrink x the inner (content-box) base size; flex factors summing below 1 take only that
 * share of the overflow; a column item with a fixed height shrinks the same way. */
static void t_flex_shrink_auto_min(void)
{
    Fx *fx = fx_html(
        "<div style='display:flex;width:300px'><div id=a1 style='width:200px;height:4px'></div><div id=a2 style='width:200px;height:4px'></div></div>"
        "<div style='display:flex;width:300px'><div id=b1 style='width:250px;padding-left:40px;height:4px'></div>"
        "<div id=b2 style='width:200px;max-width:180px;height:4px'></div><div id=b3 style='flex:1 1 auto;width:24px;height:4px'></div></div>"
        "<div style='display:flex;width:300px'><div id=c1 style='width:250px;flex-shrink:.25;height:4px'></div><div id=c2 style='width:150px;flex-shrink:.25;height:4px'></div></div>"
        "<div style='display:flex;width:300px'><div id=d1 style='width:250px'><div style='width:220px;height:4px'></div></div><div id=d2 style='width:120px;height:4px'></div></div>"
        "<div style='display:flex;flex-direction:column;height:100px'><div id=e1 style='height:80px'><div style='height:18px'></div></div>"
        "<div id=e2 style='height:80px'><div style='height:18px'></div></div></div>", NULL);
    CHECK_NEAR(rc(fx, "a1").w, 150, 0.01, "width: 200px items share the 100 px overflow");
    CHECK_NEAR(rc(fx, "a2").w, 150, 0.01, "width: 200px items share the 100 px overflow (2)");
    CHECK_NEAR(rc(fx, "b1").w, 177.125, 0.01, "shrink weight is the content-box base (210)");
    CHECK_NEAR(rc(fx, "b2").w, 109.703, 0.01, "max-width 180 then shrink (weight 180)");
    CHECK_NEAR(rc(fx, "b3").w, 13.172, 0.01, "flex: 1 1 auto 24 px item: weight 24");
    CHECK_NEAR(rc(fx, "c1").w, 218.75, 0.01, "flex-shrink .25 + .25: half the overflow, by base");
    CHECK_NEAR(rc(fx, "c2").w, 131.25, 0.01, "flex-shrink .25 + .25 (2)");
    CHECK_NEAR(rc(fx, "d1").w, 220, 0.01, "stops at its min-content (a 220 px child)");
    CHECK_NEAR(rc(fx, "d2").w, 80, 0.01, "the other item takes the rest of the overflow");
    CHECK_NEAR(rc(fx, "e1").h, 50, 0.01, "column: height 80 items shrink to share 100 px");
    CHECK_NEAR(rc(fx, "e2").h, 50, 0.01, "column: height 80 items shrink (2)");
    fx_free(fx);
}

/* textContent of a flex container whose text sat in a child element (v10's #inviteButton:
 * <div class=match-action-btn style=display:flex><span>Invite</span></div> -> "Copied URL"):
 * the bare text becomes an anonymous flex item, laid out and sized like the span was. */
static void t_flex_text_content(void)
{
    /* #subLogoButtons (absolute flex column) > #matchInfoHolder (fixed, translateX(-50%), flex row) */
    /* #uiBase (transformed: the fixed box's containing block) > a sized absolute layer (a layout
     * boundary) > #subLogoButtons: the dirty item's boundary must reach the fixed box's container */
    Fx *fx = fx_html("<div style='position:absolute;left:0;top:0;width:800px;height:600px;transform:scale(1)'>"
                     "<div style='position:absolute;left:0;top:0;width:800px;height:600px'>"
                     "<div style='position:absolute;right:21px;bottom:80px;display:flex;flex-direction:column;align-items:flex-end'>"
                     "<div style='position:fixed;bottom:148px;left:50%;transform:translateX(-50%);display:flex;align-items:center;gap:14px;padding:8px 16px'>"
                     "<div style='display:flex'>a</div><div id=row style='display:flex;gap:6px;align-items:center'>"
                     "<div id=btn style='display:flex;align-items:center;gap:4px;font-size:14px;font-weight:700;text-transform:uppercase;letter-spacing:0.5px;opacity:0.7;cursor:pointer;white-space:nowrap'><span style=\"color:#fff\">Invite</span></div>"
                     "<span id=sep>-</span></div></div><div style='width:50px;height:20px'></div></div></div></div>", NULL);
    const HtmlRect before = rc(fx, "btn"), sep0 = rc(fx, "sep");
    HtmlElem btn = dom_by_id(fx->dom, "btn");
    layout_nodes_removed(fx->tree, dom_elem_node(fx->dom, btn), 0); /* engine_before_remove(btn, 0) */
    dom_set_text(fx->dom, btn, "Copied URL", 10);
    cascade_restyle(fx->dom, g_ua, fx->sheets, fx->ns);
    layout_children_changed(fx->tree, dom_elem_node(fx->dom, btn));
    layout_run(fx->tree, fx->vw, fx->vh);
    const HtmlRect after = rc(fx, "btn"), sep1 = rc(fx, "sep");
    CHECK(after.w > before.w + 20.0f, "flex textContent: the item grows with its new text (%.1f, was %.1f)", after.w, before.w);
    CHECK_NEAR(sep1.x - sep0.x, (after.w - before.w) / 2, 0.01, "flex textContent: the next item moved (the centred row grew both ways)");
    Box *b = bx(fx, "btn");
    const TextRun *tr = NULL;
    for (Box *c = b ? b->first_child : NULL; c && !tr; c = c->next) tr = first_run(c);
    CHECK(tr && tr->len == 10, "flex textContent: the text is laid out");
    fx_free(fx);
}

/* New markup holding a position: fixed box inserted under a layout boundary whose fixed
 * containing block (a transformed ancestor) lies above it: Krunker Strike's report dropdown, whose
 * options overlay is fixed under FRVR's @media (max-height: 400px). The rebuilt subtree's boundary
 * is laid out, and so is the fixed box's container: the overlay gets its box. */
static void t_inserted_fixed(void)
{
    Fx *fx = fx_html("<div style='position:absolute;left:0;top:0;width:800px;height:600px;transform:scale(1)'>"
                     "<div style='position:absolute;left:0;top:0;width:800px;height:600px'>"
                     "<div id=host style='width:200px'><span>label</span></div></div></div>", NULL);
    HtmlElem host = dom_by_id(fx->dom, "host");
    static const char html[] = "<span>label</span><div id=dd><div id=ov style='position:fixed;left:0;top:0;width:100%;height:100%'>"
                               "<div id=opt style='height:20px'>Other</div></div></div>";
    layout_nodes_removed(fx->tree, dom_elem_node(fx->dom, host), 0);
    dom_set_html(fx->dom, host, html, strlen(html));
    cascade_restyle(fx->dom, g_ua, fx->sheets, fx->ns);
    layout_children_changed(fx->tree, dom_elem_node(fx->dom, host));
    layout_run(fx->tree, fx->vw, fx->vh);
    const HtmlRect ov = rc(fx, "ov"), opt = rc(fx, "opt");
    CHECK(NEAR(ov.w, 800, 0.01) && NEAR(ov.h, 600, 0.01), "inserted fixed: the overlay fills its container (%.1f x %.1f)", ov.w, ov.h);
    CHECK(NEAR(opt.h, 20, 0.01) && NEAR(opt.w, 800, 0.01), "inserted fixed: its content is laid out (%.1f x %.1f)", opt.w, opt.h);
    fx_free(fx);
}

/* innerHTML replacement under a restyled ancestor, in the engine's order
 * (engine_before_remove -> dom_set_html -> restyle -> post_restyle walks the
 * tree: the ancestor's layout_restyled() before the host's
 * layout_children_changed()).  The removed boxes stay in the tree until that
 * rebuild; their borrowed styles (ElemData computed / ::before) were freed with
 * the nodes, so the ancestor's transform toggle must not read them (a
 * heap-use-after-free under ASan; without ASan the stale position:fixed made it
 * REBUILT).  The menu's #instructions innerHTML at startup hit this. */
static void t_innerhtml_detach(void)
{
    Fx *fx = fx_html(
        "<div id=outer style='width:300px'><div id=host style='width:200px'>"
        "<div id=gone><span id=gsp>a<div id=gblk>b</div>c</span>"
        "<div id=fx style='position:fixed;top:0;left:0;width:5px;height:5px'>d</div></div></div></div>",
        "#gone::before { content: 'p'; } #gsp::after { content: 'q'; }");
    HtmlElem outer = dom_by_id(fx->dom, "outer"), host = dom_by_id(fx->dom, "host");
    CHECK(bx(fx, "fx") && bx(fx, "gblk"), "detach: removed subtree laid out first");

    layout_nodes_removed(fx->tree, dom_elem_node(fx->dom, host), 0);
    static const char html[] = "<b id=fresh>new</b>";
    dom_set_html(fx->dom, host, html, strlen(html));
    ElemData *oed = dom_elem_data(fx->dom, outer);
    CssComputed old = *oed->computed;
    dom_set_style(fx->dom, outer, "transform", "translateX(3px)");
    cascade_restyle(fx->dom, g_ua, fx->sheets, fx->ns);
    int r = layout_restyled(fx->tree, dom_elem_node(fx->dom, outer), &old);
    CHECK(r == LAYOUT_RESTYLE_TRANSFORM, "detach: removed position:fixed no longer counts (%d)", r);
    r = layout_children_changed(fx->tree, dom_elem_node(fx->dom, host));
    CHECK(r == LAYOUT_RESTYLE_REBUILT, "detach: host rebuilt (%d)", r);
    layout_run(fx->tree, fx->vw, fx->vh);
    HtmlRect fr = rc(fx, "fresh"), hr = rc(fx, "host");
    CHECK(bx(fx, "fresh") && NEAR(fr.x, hr.x, 0.01) && NEAR(hr.x, 3, 0.01) && fr.w > 0,
          "detach: new content laid out under the moved ancestor (%.1f, %.1f)", fr.x, hr.x);

    /* removing the host itself (include_root, like html_dom_remove) */
    layout_nodes_removed(fx->tree, dom_elem_node(fx->dom, host), 1);
    dom_remove(fx->dom, host);
    old = *oed->computed;
    dom_set_style(fx->dom, outer, "transform", "");
    cascade_restyle(fx->dom, g_ua, fx->sheets, fx->ns);
    r = layout_restyled(fx->tree, dom_elem_node(fx->dom, outer), &old);
    CHECK(r == LAYOUT_RESTYLE_TRANSFORM, "detach: removed host, transform cleared (%d)", r);
    r = layout_children_changed(fx->tree, dom_elem_node(fx->dom, outer));
    layout_run(fx->tree, fx->vw, fx->vh);
    CHECK(r == LAYOUT_RESTYLE_REBUILT && rc(fx, "outer").h == 0, "detach: outer empty after the rebuild (%d)", r);
    fx_free(fx);
}

/* ------------------------------------------------------------------------- */
/* menu                                                                       */
/* ------------------------------------------------------------------------- */

typedef struct Expect {
    const char *what;
    const char *source;
    float x, y, w, h;      /* viewport px; NAN = not checked */
    float tol;
} Expect;

static int g_menu_rows;
static void report(const char *what, const char *source, HtmlRect got, float x, float y, float w, float h, float tol)
{
    float d[4] = { isnan(x) ? 0 : got.x - x, isnan(y) ? 0 : got.y - y, isnan(w) ? 0 : got.w - w, isnan(h) ? 0 : got.h - h };
    int ok = 1;
    for (int i = 0; i < 4; i++) if (fabsf(d[i]) > tol) ok = 0;
    printf("| %-28s | %-9s | %7.1f %6.1f %7.1f %6.1f | %7.1f %6.1f %7.1f %6.1f | %+5.1f %+5.1f %+5.1f %+5.1f | %s\n",
           what, source, isnan(x) ? -1 : x, isnan(y) ? -1 : y, isnan(w) ? -1 : w, isnan(h) ? -1 : h,
           got.x, got.y, got.w, got.h, d[0], d[1], d[2], d[3], ok ? "ok" : "FAIL");
    if (ok) g_pass++; else g_fail++;
    g_menu_rows++;
}

static HtmlRect rect_of(Box *b) { HtmlRect r = { 0, 0, 0, 0 }; if (b) layout_box_rect_in_viewport(b, &r); return r; }

/* ------------------------------------------------------------------------- */

/* `all: unset` (v10's .io-button { all: unset; padding: 8px 15px; ... }): it drops the UA button
 * style, so box-sizing is content-box again, the font inherits, and a later declaration of the
 * same rule wins. Values from Chrome 152 (the v10 login modal's Login button: 334 px wide in a
 * 724 px column flex box, width: 100% capped by max-width: 300px). A <button> it makes
 * display: inline still lays out as inline-block (HTML button layout). */
static void t_all_unset(void)
{
    Fx *fx = fx_html(
        "<div id=col style='display:flex;flex-direction:column;align-items:center;width:724px;font-family:headerfont'>"
        "<button id=acc class=iob style='width:100%;max-width:300px'>Login</button>"
        "<button id=ua style='width:100%;max-width:300px;padding:8px 15px'>Login</button></div>"
        "<div id=flow style='width:400px;line-height:20px;direction:rtl;color:#ff0000'>"
        "<button id=inl class=iob2>x</button><span id=all0 class=ini>y</span><span id=bad class=bogus>z</span></div>",
        ".iob { all: unset; cursor: pointer; padding: 8px 15px; border: 2px solid #494949; }\n"
        ".iob2 { all: unset; width: 100px; height: 30px; padding: 0 10px; }\n"
        ".ini { all: initial; }\n"
        ".bogus { all: bogus; }\n");
    const CssComputed *a = bx(fx, "acc")->style;
    CHECK(a->box_sizing == CSS_BOX_SIZING_CONTENT_BOX, "all: unset: box-sizing back to content-box (UA button: border-box)");
    CHECK(a->font_family_count > 0 && a->font_families[0] && strcmp(a->font_families[0], "headerfont") == 0,
          "all: unset: font-family inherits (not the UA's Arial): %s", a->font_family_count ? a->font_families[0] : "-");
    CHECK_NEAR(a->padding[CSS_LEFT].px, 15, 0.01, "all: unset then padding: the later declaration wins");
    CHECK_NEAR(rc(fx, "acc").w, 334, 0.01, "all: unset button in a column flex box: 300 content + 2*15 + 2*2");
    CHECK_NEAR(rc(fx, "ua").w, 300, 0.01, "UA button (no all): border-box, max-width 300");
    CHECK_NEAR(rc(fx, "inl").w, 120, 0.01, "all: unset button in a line: inline-block, content-box width 100 + 2*10");
    CHECK_NEAR(rc(fx, "inl").h, 30, 0.01, "all: unset button in a line: its height applies (atomic inline)");
    const CssComputed *i = bx(fx, "all0")->style;
    CHECK(i->color.r == 0 && i->color.g == 0, "all: initial: color back to its initial value");
    CHECK(i->direction == CSS_DIRECTION_RTL, "all: initial leaves direction alone");
    CHECK(bx(fx, "bad")->style->color.r == 255, "all: <not a CSS-wide keyword> is dropped");
    fx_free(fx);
}

/* Content keywords of min-width / max-width (CSS Sizing 3 5.1), values from Chrome 152: the
 * contents are two floats in a BFC (min-content 50, max-content 80). v10's .game-announcement label is a
 * block with max-width: fit-content, so its underline spans the text only. */
static void t_min_max_width_content(void)
{
#define KW_FL "<div style='overflow:hidden'><div style='float:left;width:50px;height:4px'></div>" \
              "<div style='float:left;width:30px;height:4px'></div></div>"
    Fx *fx = fx_html(
        "<div style='width:400px'>"
        "<div id=fit style='max-width:fit-content;padding:0 5px'>" KW_FL "</div>"
        "<div id=fitc style='max-width:fit-content;margin:0 auto'>" KW_FL "</div>"
        "<div id=px style='max-width:200px;margin:0 auto'>" KW_FL "</div>"
        "<div id=minc style='max-width:min-content'>" KW_FL "</div>"
        "<div id=maxc style='width:300px;max-width:max-content'>" KW_FL "</div>"
        "<div id=minw style='width:30px;min-width:max-content'>" KW_FL "</div>"
        "<div style='width:60px'><div id=narrow style='max-width:fit-content'>" KW_FL "</div></div>"
        "<div style='display:flex'><div id=fl style='flex:1;max-width:fit-content'>" KW_FL "</div><div id=fl2 style='flex:1'></div></div>"
        "<div style='position:relative;height:10px'><div id=abs style='position:absolute;left:0;right:0;max-width:fit-content'>" KW_FL "</div></div>"
        "<div id=ib style='display:inline-block;max-width:min-content'>" KW_FL "</div>"
        "</div>", NULL);
#undef KW_FL
    CHECK_NEAR(rc(fx, "fit").w, 90, 0.01, "max-width: fit-content: max-content 80 + padding");
    CHECK_NEAR(rc(fx, "fitc").w, 80, 0.01, "max-width: fit-content with auto margins: 80 wide");
    CHECK_NEAR(rc(fx, "fitc").x, 160, 0.01, "max-width: fit-content with auto margins: centred");
    CHECK_NEAR(rc(fx, "px").x, 100, 0.01, "max-width: 200px with auto margins: centred");
    CHECK_NEAR(rc(fx, "minc").w, 50, 0.01, "max-width: min-content");
    CHECK_NEAR(rc(fx, "maxc").w, 80, 0.01, "width: 300px; max-width: max-content");
    CHECK_NEAR(rc(fx, "minw").w, 80, 0.01, "width: 30px; min-width: max-content");
    CHECK_NEAR(rc(fx, "narrow").w, 60, 0.01, "max-width: fit-content in 60 px: min(80, max(50, 60))");
    CHECK_NEAR(rc(fx, "fl").w, 80, 0.01, "flex: 1 item with max-width: fit-content");
    CHECK_NEAR(rc(fx, "fl2").w, 320, 0.01, "its flex: 1 sibling takes the rest");
    CHECK_NEAR(rc(fx, "abs").w, 80, 0.01, "left: 0; right: 0 with max-width: fit-content");
    CHECK_NEAR(rc(fx, "ib").w, 50, 0.01, "inline-block max-width: min-content (intrinsic contribution)");
    fx_free(fx);
}

/* aspect-ratio (CSS Sizing 4), values from Chrome: the v10 uses (.eventBanner 1170 / 150 of a
 * 1176 px card, #charPreviewSlot canvas 3 / 4 of 600 px, the market grids' aspect-ratio: 1 cells) */
static void t_aspect_ratio(void)
{
    Fx *fx = fx_html(
        "<div style='width:1176px'><div id=banner class=banner></div></div>"
        "<div id=hfix style='height:100px;aspect-ratio:2'></div>"
        "<div id=grid style='display:grid;grid-template-columns:repeat(auto-fill, minmax(150px, 1fr));gap:10px;width:640px'>"
        "<div id=cell style='position:relative;aspect-ratio:1'></div><div style='aspect-ratio:1'></div></div>"
        /* main.css #charPreviewSlot canvas, in a block (the engine's flex basis ignores a replaced
         * item's percentage width, which Chrome resolves: a separate flex gap) */
        "<div id=slot style='width:600px;height:500px;overflow:hidden'>"
        "<canvas id=cv style='width:100%;height:auto;aspect-ratio:3 / 4'></canvas></div>"
        "<div id=grow style='width:100px;aspect-ratio:4'><div style='height:50px'></div></div>"
        "<div id=clip style='width:100px;aspect-ratio:4;overflow:hidden'><div style='height:50px'></div></div>"
        "<div id=cbox style='width:100px;padding:10px;aspect-ratio:1'></div>"
        "<div id=bbox style='width:100px;padding:10px;box-sizing:border-box;aspect-ratio:1'></div>"
        "<div id=none style='width:100px;aspect-ratio:auto'></div>"
        "<div id=pct style='width:200px;aspect-ratio:2'><div id=pctc style='height:50%'></div></div>",
        ".banner { display: block; width: 100%; aspect-ratio: 1170 / 150; }");
    CHECK_NEAR(rc(fx, "banner").h, 1176.0f * 150.0f / 1170.0f, 0.01, ".eventBanner height from its width");
    CHECK_NEAR(rc(fx, "hfix").w, 200, 0.01, "auto width from a definite height (no stretch)");
    CHECK_NEAR(rc(fx, "cell").w, 152.5, 0.01, "auto-fill grid column");
    CHECK_NEAR(rc(fx, "cell").h, 152.5, 0.01, "aspect-ratio: 1 grid item: height = column width (no stretch)");
    CHECK_NEAR(rc(fx, "grid").h, 152.5, 0.01, "grid row sized by the ratio items");
    CHECK_NEAR(rc(fx, "cv").w, 600, 0.01, "canvas width: 100%");
    CHECK_NEAR(rc(fx, "cv").h, 800, 0.01, "canvas aspect-ratio 3 / 4 replaces its natural 2:1");
    CHECK_NEAR(rc(fx, "grow").h, 50, 0.01, "min-height: auto: content taller than the ratio grows the box");
    CHECK_NEAR(rc(fx, "clip").h, 25, 0.01, "overflow: hidden: the ratio height holds");
    CHECK_NEAR(rc(fx, "cbox").h, 120, 0.01, "content-box ratio: 100 content + 2*10 padding");
    CHECK_NEAR(rc(fx, "bbox").h, 100, 0.01, "border-box ratio: the border box is square");
    CHECK_NEAR(rc(fx, "none").h, 0, 0.01, "aspect-ratio: auto on a non-replaced box: no ratio");
    CHECK_NEAR(rc(fx, "pctc").h, 50, 0.01, "the ratio height is definite for percentage children");
    fx_free(fx);
}

/* v10's market card (v10_market.c: svelte-hk8nq9 .market-card-shell { width: 100%; aspect-ratio: 1 }
 * around svelte-r5taai .market-card with inline width / height: 100%, holding a width / height: 100%
 * <img> and a space).  The ratio height is definite, so the card's height: 100% resolves against it and
 * the shell stays square (Chrome); a content-sized child taller than the ratio grows the shell
 * instead (min-height: auto). */
static void t_aspect_ratio_content(void)
{
    Fx *fx = fx_html(
        "<div style='display:grid;grid-template-columns:repeat(2, 237px);gap:10px;width:484px;font-size:16px;line-height:normal'>"
        "<div id=shell style='position:relative;width:100%;aspect-ratio:1'>"
        "<div id=card style='position:relative;overflow:hidden;width:100%;height:100%'><img style='width:100%;height:100%' src='img/app_0.png'> </div></div>"
        "<div id=shell2 style='position:relative;width:100%;aspect-ratio:1'>"
        "<div id=card2 style='position:relative;overflow:hidden'><img style='width:100%;height:100%' src='img/app_0.png'> </div></div></div>",
        "");
    CHECK_NEAR(rc(fx, "shell").h, 237, 0.01, "market card shell: square");
    CHECK_NEAR(rc(fx, "card").h, 237, 0.01, "height: 100% of the ratio height (definite)");
    CHECK(rc(fx, "card2").h > 237.5f, "a content-sized child: image + the line's descent (%.2f)", rc(fx, "card2").h);
    CHECK_NEAR(rc(fx, "shell2").h, rc(fx, "card2").h, 0.01, "min-height: auto: the ratio box grows to that content");
    fx_free(fx);
}

/* Percentage heights of images in flex boxes, Chrome 152 (256 x 256 img/app_0.png): Strike's class
 * weapon slot (an absolute display:flex box of height 34 px: the img 34 x 34 and the slot as wide);
 * the Customize box (a flex box of height:100% clamped by max-height 54: the img 54 x 54, centred);
 * a column capped by max-height alone (its item's height:100% child stays auto: 256). */
static void t_flex_pct_height_images(void)
{
    Fx *fx = fx_html(
        "<div id=s1 style='position:absolute;left:0;top:0;height:34px;display:flex'><img id=s1i style='height:100%' src='img/app_0.png'></div>"
        "<div style='position:absolute;left:0;top:40px;width:90px;height:80px;display:flex;flex-direction:column;align-items:center'>"
        "<div style='height:20px;width:100%;flex:none'></div><div style='width:100%;height:100%;max-height:54px;display:flex;"
        "justify-content:center;align-items:center'><img id=s2i style='height:100%' src='img/app_0.png'></div></div>"
        "<div style='position:absolute;left:0;top:200px;width:200px;max-height:60px;display:flex;flex-direction:column'>"
        "<div style='height:100%'><img id=s4i style='height:100%' src='img/app_0.png'></div></div>", NULL);
    CHECK_NEAR(rc(fx, "s1").w, 34, 0.01, "abs flex slot of height 34: as wide as its 34 px image");
    CHECK_NEAR(rc(fx, "s1i").w, 34, 0.01, "height:100%% of a definite 34 px slot: 34 wide through the ratio");
    CHECK_NEAR(rc(fx, "s2i").w, 54, 0.01, "height:100%% of a max-height-clamped flex box: 54 x 54");
    CHECK_NEAR(rc(fx, "s2i").x, 18, 0.01, "and centred in the 90 px box");
    CHECK_NEAR(rc(fx, "s4i").h, 256, 0.01, "a column capped by max-height alone: the percentage stays auto");
}

/* A replaced flex item with a percentage width (Krunker Strike's .frvr-lboard-end-screen__pfp: a 43px
 * display:flex box, align-items center, holding an 8x8 class icon at width: 100%). flex-basis auto
 * is the width, 100% of the definite container: 43x43 centred on the 36px row (Chrome), not the
 * 8px natural width with the 43px ratio height. A fixed width and a block parent are unchanged. */
static void t_flex_replaced_pct(void)
{
    const char *img = "strike/cdn/textures/classes/icon_0.png"; /* 8x8 */
    char html[1024];
    snprintf(html, sizeof(html),
        "<div style='display:flex;width:400px;height:36px;align-items:center'>"
        "<div id=pfp style='display:flex;width:43px;min-width:43px;height:100%%;align-items:center;justify-content:center;overflow:hidden'>"
        "<img id=pi style='width:100%%' src='%s'></div><div>name</div></div>"
        "<div style='display:flex;width:43px;height:50px'><img id=fx style='width:20px' src='%s'></div>"
        "<div style='width:43px'><img id=bk style='width:100%%' src='%s'></div>", img, img, img);
    Fx *fx = fx_html(html, "");
    HtmlRect p = rc(fx, "pfp"), i = rc(fx, "pi");
    CHECK_NEAR(i.w, 43, 0.01, "flex row: <img width:100%%> fills the 43px box");
    CHECK_NEAR(i.h, 43, 0.01, "flex row: its height follows the 1:1 ratio");
    CHECK_NEAR(i.x - p.x, 0, 0.01, "flex row: no free space left to centre in");
    CHECK_NEAR(i.y - p.y, -3.5, 0.01, "flex row: centred on the 36px cross size");
    CHECK_NEAR(rc(fx, "fx").w, 20, 0.01, "flex row: a fixed width stays as it is");
    CHECK_NEAR(rc(fx, "bk").w, 43, 0.01, "block parent: width 100%% (unchanged)");
    fx_free(fx);
}

/* A replaced flex item with a percentage height and an auto width (Krunker Strike's .reward-screen-bg img:
 * height 100% in a full-screen centred flex row). The height resolves against the definite container and
 * the width follows through the natural ratio (Chrome): an 8x8 icon in a 60px tall row is 60x60, centred;
 * in a container without a definite height the percentage is auto (the natural 8x8). */
static void t_flex_replaced_pct_height(void)
{
    const char *img = "strike/cdn/textures/classes/icon_0.png"; /* 8x8 */
    char html[1024];
    snprintf(html, sizeof(html),
        "<div id=row style='display:flex;width:400px;height:60px;justify-content:center;align-items:center'>"
        "<img id=ph style='height:100%%' src='%s'></div>"
        "<div style='display:flex;width:400px'><img id=pi style='height:100%%' src='%s'></div>", img, img);
    Fx *fx = fx_html(html, "");
    HtmlRect r = rc(fx, "row"), i = rc(fx, "ph");
    CHECK_NEAR(i.h, 60, 0.01, "flex row: <img height:100%%> is the container's 60px");
    CHECK_NEAR(i.w, 60, 0.01, "flex row: its width follows the 1:1 ratio");
    CHECK_NEAR(i.x - r.x, 170, 0.01, "flex row: centred in the 400px row");
    CHECK_NEAR(rc(fx, "pi").w, 8, 0.01, "indefinite container height: the natural width");
    CHECK_NEAR(rc(fx, "pi").h, 8, 0.01, "indefinite container height: the natural height");
    fx_free(fx);
}

/* A grid flex item with repeat(auto-fit, minmax(..)) columns and max-width: 100% (Krunker Strike's
 * .item-selection__items-grid in its flex holder): the definite max size sets the repetitions, so the
 * max-content width holds as many columns as fit (Chrome), not one. A 430px row with 100px minimum
 * columns and 10px gaps: 4 columns for 6 items; auto-fit collapses the empty ones of a 2-item grid. */
static void t_grid_autofit_max_width(void)
{
    const char *cell = "<div style='height:20px'></div>";
    char html[2048];
    snprintf(html, sizeof(html),
        "<div style='display:flex;width:430px'><div id=g6 style='display:grid;max-width:100%%;"
        "grid-template-columns:repeat(auto-fit,minmax(100px,1fr));gap:10px'>%s%s%s%s%s%s</div></div>"
        "<div style='display:flex;width:430px'><div id=g2 style='display:grid;max-width:100%%;"
        "grid-template-columns:repeat(auto-fit,minmax(100px,1fr));gap:10px'>%s%s</div></div>",
        cell, cell, cell, cell, cell, cell, cell, cell);
    Fx *fx = fx_html(html, "");
    HtmlRect g6 = rc(fx, "g6"), g2 = rc(fx, "g2");
    CHECK_NEAR(g6.w, 430, 0.01, "auto-fit + max-width 100%%: four 100px columns and gaps fill the row");
    CHECK_NEAR(g6.h, 50, 0.01, "six items: two rows of 20px and a 10px gap");
    CHECK_NEAR(g2.w, 210, 0.01, "two items: the empty repeated tracks collapse (auto-fit)");
    fx_free(fx);
}

/* -webkit-line-clamp with display: -webkit-box + -webkit-box-orient: vertical (v10's .eventDesc /
 * .pollDesc): the box ends after the Nth line, which ends in an ellipsis when text follows */
static void t_line_clamp(void)
{
    const char *css = ".d { display: -webkit-box; -webkit-line-clamp: 2; -webkit-box-orient: vertical; overflow: hidden;"
                      " width: 200px; font-size: 13px; line-height: 20px; }";
    Fx *fx = fx_html(
        "<div id=long class=d>Join the team for a night of custom 4v4 games on Kanji. Everyone is welcome, bring friends,"
        " bring snacks, bring your best aim and your worst puns.</div>"
        "<div id=short class=d>Short text.</div>"
        "<div id=two class=d>Vote for the community map you want</div>"
        "<div id=col style='display:-webkit-box;-webkit-box-orient:vertical;width:300px'><div id=c1 style='height:10px'></div><div id=c2 style='height:10px'></div></div>"
        "<div id=row style='display:-webkit-box;width:300px'><div id=r1 style='width:50px;height:10px'></div><div id=r2 style='width:50px;height:10px'></div></div>",
        css);
    Box *lb = bx(fx, "long");
    CHECK_NEAR(rc(fx, "long").h, 40, 0.01, "clamped to 2 lines of 20px");
    CHECK(lb && count_lines(lb) >= 3, "the text needs more than 2 lines (%d)", lb ? count_lines(lb) : -1);
    const TextRun *last = NULL;
    if (lb && lb->line_count >= 2) {
        const LineBox *ln = &lb->lines[1];
        for (int i = 0; i < ln->item_count; i++) if (ln->items[i].run) last = ln->items[i].run;
    }
    CHECK(last && last->ellipsis, "the 2nd line ends in an ellipsis");
    CHECK(last && last->x + last->width + last->ellipsis_w <= 200.01f, "text + ellipsis fit the line (%.1f)",
          last ? last->x + last->width + last->ellipsis_w : -1.0);
    if (lb && lb->line_count) {
        const LineBox *ln = &lb->lines[0];
        int e = 0;
        for (int i = 0; i < ln->item_count; i++) if (ln->items[i].run && ln->items[i].run->ellipsis) e = 1;
        CHECK(!e, "the 1st line has no ellipsis");
    }
    CHECK_NEAR(rc(fx, "short").h, 20, 0.01, "one line: not clamped");
    const TextRun *sr = first_run(bx(fx, "short"));
    CHECK(sr && !sr->ellipsis, "one line: no ellipsis");
    CHECK_NEAR(rc(fx, "c2").y - rc(fx, "c1").y, 10, 0.01, "-webkit-box-orient: vertical stacks the children");
    CHECK_NEAR(rc(fx, "r2").x - rc(fx, "r1").x, 50, 0.01, "-webkit-box (horizontal) lays them out in a row");
    CHECK_NEAR(rc(fx, "r2").y, rc(fx, "r1").y, 0.01, "-webkit-box row: same top");
    fx_free(fx);
}

static int video_size_stub(void *user, const char *url, const char *base, float *w, float *h)
{
    (void)user; (void)base;
    if (!url || !strstr(url, "basics.mp4")) return 0;
    *w = 1280; *h = 720;
    return 1;
}

/* <video>: 300 x 150 without a known source, the source's natural size once known; no UA size */
static void t_video_intrinsic(void)
{
    g_ctx.video_size = video_size_stub;
    Fx *fx = fx_html("<video id=v1 style='width:700px' src='/assets/videos/tutorial/basics.mp4' autoplay loop></video>"
                     "<video id=v2></video>"
                     "<video id=v3><source src='/assets/videos/tutorial/basics.mp4' type='video/mp4'></video>"
                     "<video id=v4 src='/assets/videos/missing.mp4' style='width:600px'></video>", NULL);
    CHECK_NEAR(rc(fx, "v1").w, 700, 0.01, "video width");
    CHECK_NEAR(rc(fx, "v1").h, 393.75, 0.01, "video height from the source's 16:9");
    CHECK_NEAR(rc(fx, "v2").w, 300, 0.01, "no source: 300 wide");
    CHECK_NEAR(rc(fx, "v2").h, 150, 0.01, "no source: 150 tall");
    CHECK_NEAR(rc(fx, "v3").w, 1280, 0.01, "<source src> child: natural width");
    CHECK_NEAR(rc(fx, "v4").h, 300, 0.01, "unknown source: the default 2:1 ratio");
    fx_free(fx);
    g_ctx.video_size = NULL;
}

/* ------------------------------------------------------------------------- */
/* incremental memo: relayout after an edit equals a full layout of the same edit */
/* ------------------------------------------------------------------------- */
/* The Object Manager's shape: an absolute window with a definite size (the relayout boundary) over
 * a flex column chain, a scroll grid of tiles (one with an absolute badge) and a table row. Each
 * step edits one element. The incremental side relayouts from the boundary (layout_restyled +
 * layout_run, the memo serves the clean siblings); the full side builds the whole tree again. Every
 * element's rect must agree after every step. */
#define MEMO_TILES 30
static const char *const memo_ids[] = {"wbox", "menu", "title", "body", "status", "grid", "foot", "tabs", "tab0", "tab1", "badge3"};
typedef struct MemoStep { const char *id, *prop, *val; } MemoStep;
static const MemoStep memo_steps[] = {
    {"t5", "display", "none"}, {"t5", "display", ""}, {"t7", "height", "80px"}, {"title", "height", "40px"},
    {"status", "height", "30px"}, {"t3", "display", "none"}, {"foot", "display", "none"}, {"foot", "display", ""},
    {"body", "display", "none"}, {"body", "display", "flex"}, {"t10", "width", "50%"}, {"grid", "max-height", "120px"},
    {"t20", "height", "20px"}, {"t3", "display", ""}, {"tab1", "border-bottom", "5px solid #fff"}, {"tab1", "border-bottom", ""},
    {"title", "width", "200px"}, {"title", "width", ""}, {"foot", "width", "300px"}, {"foot", "width", ""},
    {"t0", "height", "60px"}, {"status", "height", "70px"}, {"grid", "max-height", "400px"}, {"t29", "display", "none"},
    {"t29", "display", ""}, {"menu", "max-height", "160px"}, {"menu", "max-height", "400px"}, {"t12", "height", "10px"},
};

static void memo_page(char *buf, size_t n)
{
    size_t k = (size_t)snprintf(buf, n,
        "<div id=wbox style='position:absolute;left:0;top:0;width:600px;height:500px'>"
        "<div id=menu style='display:flex;flex-direction:column;width:560px;height:400px;max-height:400px'>"
        "<div id=title style='height:24px'>Title<div id=badge3 style='position:absolute;top:0;right:0;width:8px;height:8px'></div></div>"
        "<div id=tabs style='display:table;width:100%%'><div id=tab0 style='display:table-cell;padding:4px'>A</div>"
        "<div id=tab1 style='display:table-cell;padding:4px'>B</div></div>"
        "<div id=body style='display:flex;flex-direction:column;flex:1 1 auto;min-height:0'>"
        "<div id=status style='height:16px'>status</div>"
        "<div id=grid style='display:grid;grid-template-columns:repeat(auto-fill,minmax(100px,1fr));gap:8px;"
        "max-height:200px;overflow-y:auto'>");
    for (int i = 0; i < MEMO_TILES && k < n; i++) {
        k += (size_t)snprintf(buf + k, n - k,
            "<div id=t%d style='position:relative;height:%dpx'><span id=s%d>tile %d</span></div>",
            i, 40 + (i % 3) * 10, i, i);
    }
    snprintf(buf + k, n - k, "</div></div><div id=foot style='height:30px'>foot</div></div></div>");
}

/* the element's rect, or 0 when it has no box (display: none): no FAIL, unlike rc() */
static int memo_rect(Fx *fx, const char *id, HtmlRect *r)
{
    HtmlElem e = dom_by_id(fx->dom, id);
    ElemData *ed = e ? dom_elem_data(fx->dom, e) : NULL;
    if (!ed || !ed->box) return 0;
    layout_box_rect_in_viewport((Box *)ed->box, r);
    return 1;
}

static void t_memo_equivalence(void)
{
    static char body[16384];
    memo_page(body, sizeof(body));
    Fx *inc = fx_html(body, NULL);
    Fx *full = fx_html(body, NULL);
    const int nsteps = (int)(sizeof(memo_steps) / sizeof(memo_steps[0]));
    int mismatches = 0;
    for (int i = 0; i < nsteps; i++) {
        const MemoStep *st = &memo_steps[i];
        HtmlElem e = dom_by_id(inc->dom, st->id);
        ElemData *ed = e ? dom_elem_data(inc->dom, e) : NULL;
        if (!ed || !ed->computed) { CHECK(0, "memo step %d: no element %s", i, st->id); continue; }
        CssComputed old = *ed->computed;
        dom_set_style(inc->dom, e, st->prop, st->val);
        cascade_restyle(inc->dom, g_ua, inc->sheets, inc->ns);
        layout_restyled(inc->tree, dom_elem_node(inc->dom, e), &old);
        layout_run(inc->tree, inc->vw, inc->vh);

        dom_set_style(full->dom, dom_by_id(full->dom, st->id), st->prop, st->val);
        fx_layout(full);

        int bad_here = 0;
        const size_t n_ids = sizeof(memo_ids) / sizeof(memo_ids[0]);
        for (size_t k = 0; k < n_ids + 2 * MEMO_TILES; k++) {
            char id[16];
            if (k < n_ids) snprintf(id, sizeof(id), "%s", memo_ids[k]);
            else if (k < n_ids + MEMO_TILES) snprintf(id, sizeof(id), "t%d", (int)(k - n_ids));
            else snprintf(id, sizeof(id), "s%d", (int)(k - n_ids - MEMO_TILES));
            HtmlRect a = { 0, 0, 0, 0 }, b = { 0, 0, 0, 0 };
            const int pa = memo_rect(inc, id, &a), pb = memo_rect(full, id, &b);
            if (pa != pb || (pa && !(NEAR(a.x, b.x, 0.01) && NEAR(a.y, b.y, 0.01) && NEAR(a.w, b.w, 0.01) && NEAR(a.h, b.h, 0.01)))) {
                if (bad_here++ < 2)
                    printf("  step %d (%s %s=%s) #%s: incremental %.2f,%.2f %.2fx%.2f full %.2f,%.2f %.2fx%.2f\n", i, st->id, st->prop,
                           st->val, id, a.x, a.y, a.w, a.h, b.x, b.y, b.w, b.h);
            }
        }
        mismatches += bad_here;
    }
    CHECK(mismatches == 0, "memo: incremental relayout equals the full layout after every edit (%d mismatches)", mismatches);
    fx_free(inc);
    fx_free(full);
}

/* Block formatting context roots contain their floats (auto height) and keep clear of the
 * floats around them. Values from Chrome 152: a 50x30 float inside each box; overflow: clip
 * and contain: style do not establish a BFC (CSS Overflow 3, CSS Containment 2). */
static void t_bfc_roots(void)
{
#define BFC_CASE(id, st) "<div style='width:300px;overflow:hidden'><div id=" id " style='" st "'>" \
    "<div style='float:left;width:50px;height:30px'></div></div>" \
    "<div id=" id "n style='overflow:hidden;height:10px'></div></div>"
    Fx *fx = fx_html(
        BFC_CASE("fr", "display:flow-root") BFC_CASE("blk", "display:block") BFC_CASE("clip", "overflow:clip")
        BFC_CASE("cl", "contain:layout") BFC_CASE("cp", "contain:paint") BFC_CASE("cc", "contain:content")
        BFC_CASE("cs", "contain:style"), NULL);
#undef BFC_CASE
    static const struct { const char *id; float h, next_x; } want[] = {
        { "fr", 30, 0 }, { "blk", 0, 50 }, { "clip", 0, 50 }, { "cl", 30, 0 }, { "cp", 30, 0 }, { "cc", 30, 0 }, { "cs", 0, 50 },
    };
    for (size_t i = 0; i < sizeof want / sizeof *want; i++) {
        char next[16];
        snprintf(next, sizeof next, "%sn", want[i].id);
        CHECK_NEAR(rc(fx, want[i].id).h, want[i].h, 0.01, want[i].id);   /* height with a 50x30 float inside */
        CHECK_NEAR(rc(fx, next).x, want[i].next_x, 0.01, next);         /* the BFC after it: beside an escaped float */
    }
    fx_free(fx);
}

/* A clean `margin: auto` block served from the memo keeps its centring (the Change Loadout tiles'
 * <img class=custIcon>, display:block;margin:auto, drew at the tile's left edge once 88bdbe21's memo
 * served them: layout_resolve_edges makes auto margins 0, and the memo had not kept the resolved ones).
 * overflow:hidden makes #c a BFC root, like the replaced <img>: only those are memoised in a block flow. */
static void t_memo_auto_margin(void)
{
    Fx *fx = fx_html("<div id=wrap style='position:absolute;left:0;top:0;width:400px;height:300px'>"
                     "<div id=c style='width:100px;height:20px;margin:auto;overflow:hidden'></div>"
                     "<div id=d style='height:20px'>d</div></div>", NULL);
    HtmlRect r = { 0, 0, 0, 0 };
    CHECK(memo_rect(fx, "c", &r), "memo auto margin: #c has a box");
    CHECK_NEAR(r.x, 150, 0.01, "memo auto margin: centred on the full layout");
    HtmlElem d = dom_by_id(fx->dom, "d");
    ElemData *ed = d ? dom_elem_data(fx->dom, d) : NULL;
    if (ed && ed->computed) {
        CssComputed old = *ed->computed;
        dom_set_style(fx->dom, d, "height", "40px");
        cascade_restyle(fx->dom, g_ua, fx->sheets, fx->ns);
        layout_restyled(fx->tree, dom_elem_node(fx->dom, d), &old);
        layout_run(fx->tree, fx->vw, fx->vh);
    }
    CHECK(memo_rect(fx, "c", &r), "memo auto margin: #c still has a box");
    CHECK_NEAR(r.x, 150, 0.01, "memo auto margin: still centred after a sibling's relayout (memo served)");
    fx_free(fx);
}

int main(int argc, char **argv)
{
    if (argc > 1) g_root = argv[1];
    char ui[1024];
    snprintf(ui, sizeof(ui), "%s/ui", g_root);
    g_ts = text_create(ui);
    if (!g_ts) { fprintf(stderr, "text_create failed\n"); return 99; }
    /* main.css: gamefont = FrVrKrunkerBold2.ttf (v3.1.1 used font2.ttf) */
    text_register_face(g_ts, "gamefont", "css/fonts/FrVrKrunkerBold2.ttf", 400, TEXT_STYLE_NORMAL);
    text_register_face(g_ts, "headerfont", "css/fonts/header.ttf", 400, TEXT_STYLE_NORMAL);
    text_register_face(g_ts, "material icons", "css/fonts/material-icons.ttf", 400, TEXT_STYLE_NORMAL);
    text_register_face(g_ts, "material icons outlined", "css/fonts/material-icons-outlined.ttf", 400, TEXT_STYLE_NORMAL);
    layout_ctx_init(&g_ctx);
    g_ctx.text = g_ts;
    g_ctx.image_size = png_size;
    g_ctx.scrollbar_width = sb_width;
    g_ctx.base_url = "index.html";
    g_ua = css_sheet_load(KN_UA_CSS, strlen(KN_UA_CSS), "ua.css");

    t_margin_collapse();
    t_floats();
    t_line_below_floats();
    t_inline_block_baseline();
    t_flex();
    t_flex_pct_basis_align_content();
    t_grid();
    t_grid_tracks();
    t_grid_placement();
    t_abs_static_inline();
    t_flex_align_self();
    t_display_contents_calc_var();
    t_empty_table_overflow();
    t_grid_named();
    t_zoom();
    t_content_url_element();
    t_half_leading_floor();
    t_ua_defaults();
    t_all_unset();
    t_table();
    t_table_colspan();
    t_table_rowspan();
    t_positioned();
    t_sticky_scroll();
    t_text();
    t_hit_test();
    t_html_table();
    t_end_table();
    t_ellipsis_owner();
    t_inline_block_block_baseline();
    t_scroll_extent();
    t_closed_inline_states();
    t_float_relayout();
    t_anonymous_style();
    t_root_overflow();
    t_compressible_replaced();
    t_container_scrollbars();
    t_show_restyles_changes();
    t_incremental();
    t_memo_equivalence();
    t_memo_auto_margin();
    t_flex_text_content();
    t_flex_shrink_auto_min();
    t_innerhtml_detach();
    t_intrinsic_font_restyle();
    t_flex_column_scroll_panes();
    t_min_max_width_content();
    t_aspect_ratio();
    t_aspect_ratio_content();
    t_flex_replaced_pct();
    t_inserted_fixed();
    t_flex_replaced_pct_height();
    t_grid_autofit_max_width();
    t_line_clamp();
    t_flex_pct_height_images();
    t_video_intrinsic();
    t_bfc_roots();

    css_sheet_destroy(g_ua);
    cascade_shutdown();
    text_destroy(g_ts);
    printf("test_layout: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
