/*
 * engine.c - native backend of the HTML UI layer (html_ui_backend.h) on the
 * in-house HTML/CSS engine.  See engine.h for the frame pipeline and
 * doc/html-engine-plan.md for the architecture.
 *
 * Compositing and pacing contract with the client:
 *   - the page is composited last, premultiplied (GL_ONE, GL_ONE_MINUS_SRC_ALPHA),
 *     and the client's global blend function (GL_SRC_ALPHA,
 *     GL_ONE_MINUS_SRC_ALPHA) is restored afterwards; the depth test is left
 *     as it was found, the viewport covers the whole framebuffer and the
 *     g_active_shader / g_active_texture caches stay coherent (gl_backend.c);
 *   - html_ui_backend_update() does its work at most HTML_ENGINE_UPDATE_HZ
 *     times a second (timers, animation ticks, resize observers) because the
 *     client loop is unthrottled; animation ticks are additionally capped at
 *     the display refresh rate (KRUNKNATIVE_HTML_DISPLAY_HZ overrides);
 *   - a static page costs one display-list submit per frame: restyle, layout
 *     and paint only run when something is dirty or an animation is active;
 *   - 53 px per wheel click; input is not paced;
 *   - KRUNKNATIVE_HTML_PROFILE=1 prints avg/max ms per stage every 2 s,
 *     KRUNKNATIVE_HTML_VERBOSE=1 logs loads and failures.
 *
 * No JavaScript: <script> elements are ignored; the C UI logic drives the
 * page through html_dom.h (dom_ops.c).
 */
#if defined(KRUNKNATIVE_HTML_ENGINE) && !defined(__EMSCRIPTEN__)

#if !defined(_WIN32) && !defined(_XOPEN_SOURCE)
#define _XOPEN_SOURCE 700 /* realpath */
#endif

#include <gl.h> /* before client.h / GLFW: GLFW would otherwise pull the system GL header */
#include "html_ui_backend.h"
#include <GLFW/glfw3.h>
#include <headless.h> /* headless_current_window: the UI window, also without a GLFW context */

#include <math.h>
#include <stdarg.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <limits.h>
#endif

#include "engine.h"
#include "dom.h"
#include "css_sheet.h"
#include "css_cascade.h"
#include "css_values.h"
#include "ua_css.h"
#include "layout.h"
#include "paint.h"
#include "gl_backend.h"
#include "text.h"
#include "image.h"
#include "video.h"
#include "anim.h"
#include "events.h"
#include "widgets.h"
#include "selection.h"
#include "util.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define HTML_ENGINE_UPDATE_HZ 120.0
#define HTML_ENGINE_DEFAULT_DISPLAY_HZ 60.0
#define HTML_ENGINE_MIN_DISPLAY_HZ 30.0
#define HTML_ENGINE_MAX_DISPLAY_HZ 240.0
#define HTML_ENGINE_PROFILE_INTERVAL_S 2.0
#define HTML_ENGINE_MAX_SHEETS 32
#define HTML_ENGINE_PSEUDO_RING 8

/* ------------------------------------------------------------------------- */
/* State                                                                      */
/* ------------------------------------------------------------------------- */

typedef struct SavedScroll {
    HtmlElem elem;
    float x, y;
    const void *box;   /* the box saved from: restore only when the element's box was rebuilt */
} SavedScroll;

typedef struct ProfStat {
    double sum, max;
    unsigned count;
} ProfStat;

struct Engine {
    char ui_dir[PATH_MAX];                   /* absolute, trailing slash */
    char page[128];                          /* document file inside ui_dir ("index.html", "social.html", ...) */

    Dom *dom;
    CssSheet *ua;
    CssSheet *sheets[HTML_ENGINE_MAX_SHEETS];
    int n_sheets;

    TextSystem *text;
    ImageCache *images;
    AnimSystem anim;
    EventSystem events;
    EventsHost ev_base;                      /* events_host_layout() defaults, wrapped */
    EventsLayoutLink ev_link;
    Widgets *widgets;
    GlBackend *glb;

    LayoutCtx lctx;
    LayoutTree *tree;
    PaintCtx paint;
    DisplayList dl;                          /* last built list, resubmitted every frame */
    DisplayList overlay;                     /* select popup / colour picker */
    DisplayList top_dl;                      /* the top layer's context (html_dom_set_top_layer) */
    HtmlElem top_layer;                      /* html_dom_set_top_layer; 0 = none */
    KnArena overlay_arena;

    int fb_w, fb_h;                          /* framebuffer px */
    float vw, vh;                            /* CSS px */

    /* dirty state */
    int need_rebuild;                        /* box tree must be rebuilt */
    int need_layout;                         /* layout_run on the current tree */
    int need_paint;                          /* display list must be re-recorded */
    int observe_pending;                     /* a layout ran since events_after_layout */
    int in_flush;
    int in_after_layout;

    /* <canvas> bitmaps written by html_dom_canvas_put (premultiplied RGBA8, MAX_LEVEL 0, owned)
     * or shown with html_dom_canvas_gl (the client's render target: external, row 0 at the bottom) */
    struct EngineCanvas { HtmlElem e; unsigned tex; int w, h; int external; } *canvases;
    VideoPlayers *videos;                    /* <video> playback (video.h), keyed by HtmlElem */
    int n_canvases, cap_canvases;

    /* animation side tables: base (un-animated) computed style per element
     * ([ANIM_PSEUDO_NONE]) and per ::before / ::after record */
    CssComputed **anim_base[3];
    uint32_t anim_base_cap[3];
    uint32_t anim_count;
    /* per element: 1 = its animations were cancelled by display: none */
    uint8_t *anim_hidden;
    uint32_t anim_hidden_cap;

    /* style records the layout tree last saw (layout_restyled's old_style) */
    CssComputed **shadow;
    uint32_t shadow_cap;
    int structure_dirty;                     /* DOM child lists changed: run post_restyle */
    int restore_scroll_pending;
    int anim_scroll_saved;                   /* this anim tick already saved the scroll offsets */

    /* scroll offsets carried across box-tree rebuilds */
    SavedScroll *saved_scroll;
    size_t saved_scroll_len, saved_scroll_cap;

    /* pacing (seconds on the engine clock) */
    double display_hz;
    double next_update, next_anim;
    int sent_initial_resize;

    /* input */
    float mouse_x, mouse_y;
    int key_consumed;
    Selection sel;                           /* page text selection (selection.c) */
    int cursor_shape;                        /* GLFW standard cursor id, 0 = default */
    GLFWcursor *cursors[9];

    /* pseudo-element computed styles handed to widgets (short-lived) */
    CssComputed pseudo_ring[HTML_ENGINE_PSEUDO_RING];
    int pseudo_next;

    /* diagnostics */
    int verbose;
    int profile;
    int dump_dl;
    double prof_window_start, prof_frame_start;
    ProfStat st_restyle, st_cascade, st_post, st_layout, st_paint, st_submit, st_frame;
    unsigned prof_frames, prof_rebuilds, prof_restyled;
    double decode_ms;                        /* lazy image decodes this frame (budget) */
    int decodes, decodes_deferred;           /* this frame: done / over budget         */
    ProfStat st_decode;                      /* per frame with a decode (profile)       */
    EngineStats stats;                       /* accumulated for engine_stats() */
};

/* The active document.  html_ui_backend_init() loads the base page (index.html,
 * the game menu) into g_base; html_ui_backend_load_page() shows another page in
 * g_page (a fresh document every time) and keeps g_base alive with its DOM state,
 * so returning to the game is a pointer swap (doc/html-ui-engine.md "Pages"). */
static Engine *g_engine;
static Engine *g_base;
static Engine *g_page;
static double g_clock_override = -1.0;

/* ------------------------------------------------------------------------- */
/* Small helpers                                                              */
/* ------------------------------------------------------------------------- */

static void vlog(const char *fmt, ...)
{
    if (!g_engine || !g_engine->verbose) return;
    va_list ap;
    va_start(ap, fmt);
    fputs("[html_engine] ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

double engine_now_ms(void)
{
    if (g_clock_override >= 0.0) return g_clock_override;
    return glfwGetTime() * 1000.0;
}

void engine_set_clock_override(double ms)
{
    g_clock_override = ms;
}

/* Headless Chromium (used for the doc/ref captures) runs with --hide-scrollbars:
 * scrollbars take no space and are not painted. uidiff scenarios match that;
 * the game itself keeps Chrome's normal custom scrollbars. */
static int g_hide_scrollbars;

void engine_set_hide_scrollbars(int hide)
{
    hide = hide ? 1 : 0;
    if (g_hide_scrollbars == hide) return;
    g_hide_scrollbars = hide;
    if (g_engine) {
        g_engine->paint.hide_scrollbars = hide;
        if (g_engine->tree) layout_rebuild_all(g_engine->tree);
    }
}

/* html_dom_set_lang: <html lang> of every page (the base page and a loaded one) and the text
 * system's script preference (text_set_lang); a changed preference re-lays out all text. */
static char g_doc_lang[24];

static void engine_apply_lang(Engine *e)
{
    if (!e) return;
    if (e->dom && dom_root(e->dom)) {
        if (g_doc_lang[0]) dom_set_attr(e->dom, dom_root(e->dom), "lang", g_doc_lang);
        else dom_remove_attr(e->dom, dom_root(e->dom), "lang");
    }
    if (e->text && text_set_lang(e->text, g_doc_lang[0] ? g_doc_lang : NULL)) {
        if (e->tree) layout_rebuild_all(e->tree);
        if (e->dom) { e->lctx.memo_epoch++; dom_mark_all_dirty(e->dom); }
        e->need_layout = 1;
        e->need_paint = 1;
    }
}

void engine_set_lang(const char *lang)
{
    snprintf(g_doc_lang, sizeof g_doc_lang, "%s", lang ? lang : "");
    engine_apply_lang(g_engine);
    if (g_base != g_engine) engine_apply_lang(g_base);
}

static double now_s(void)
{
    return engine_now_ms() / 1000.0;
}

/* True once per `period`, advancing the schedule. */
static int pace_due(double *next, double period, double now)
{
    if (now + period * 0.25 < *next) return 0;
    *next = *next > now - period ? *next + period : now + period;
    return 1;
}

static void prof_add(ProfStat *s, double ms)
{
    s->sum += ms;
    if (ms > s->max) s->max = ms;
    s->count++;
}

static void prof_print(const char *name, const ProfStat *s)
{
    if (s->count) fprintf(stderr, "  %s %.3f/%.3f ms (%u)", name, s->sum / s->count, s->max, s->count);
    else fprintf(stderr, "  %s -", name);
}

static void prof_report(Engine *g, double now)
{
    const double elapsed = now - g->prof_window_start;
    if (elapsed < HTML_ENGINE_PROFILE_INTERVAL_S) return;
    fprintf(stderr, "[html_engine profile] %.1fs: %u frames (%.1f fps)", elapsed, g->prof_frames, g->prof_frames / elapsed);
    prof_print("frame", &g->st_frame);
    prof_print("restyle", &g->st_restyle);
    fprintf(stderr, " [%u elems]", g->prof_restyled);
    prof_print("(cascade", &g->st_cascade);
    prof_print("post)", &g->st_post);
    prof_print("layout", &g->st_layout);
    fprintf(stderr, " (%u rebuilds)", g->prof_rebuilds);
    prof_print("paint", &g->st_paint);
    prof_print("submit", &g->st_submit);
    int resident = 0, decodes = 0;
    size_t bytes = 0;
    image_cache_stats(g->images, &resident, &bytes, &decodes);
    fprintf(stderr, "  dl %d items  images %d (%d resident, %.1f MB lazy, %d decodes",
            g->dl.n, image_cache_count(g->images), resident, (double) bytes / 1048576.0, decodes);
    prof_print("decode/frame", &g->st_decode);
    const GlbStats gs = glb_stats(g->glb);
    fprintf(stderr, ")  last frame %d draws %d culled\n", gs.draw_calls, gs.culled);
    memset(&g->st_decode, 0, sizeof(ProfStat));
    memset(&g->st_restyle, 0, sizeof(ProfStat));
    memset(&g->st_cascade, 0, sizeof(ProfStat));
    memset(&g->st_post, 0, sizeof(ProfStat));
    memset(&g->st_layout, 0, sizeof(ProfStat));
    memset(&g->st_paint, 0, sizeof(ProfStat));
    memset(&g->st_submit, 0, sizeof(ProfStat));
    memset(&g->st_frame, 0, sizeof(ProfStat));
    g->prof_frames = 0;
    g->prof_rebuilds = 0;
    g->prof_restyled = 0;
    g->prof_window_start = now;
}

static double wall_ms(void)
{
    return glfwGetTime() * 1000.0;
}

static int dir_exists(const char *path)
{
    struct stat st;
    return !stat(path, &st) && S_ISDIR(st.st_mode);
}

static void make_ui_dir(const char *ui_dir, char *out, size_t cap)
{
    char tmp[PATH_MAX];
#ifdef _WIN32
    if (!_fullpath(tmp, ui_dir, sizeof(tmp))) snprintf(tmp, sizeof(tmp), "%s", ui_dir);
#else
    if (!realpath(ui_dir, tmp)) snprintf(tmp, sizeof(tmp), "%s", ui_dir);
#endif
    for (char *c = tmp; *c; c++) {
        if (*c == '\\') *c = '/';
    }
    size_t len = strlen(tmp);
    if (len && tmp[len - 1] != '/' && len + 1 < sizeof(tmp)) {
        tmp[len++] = '/';
        tmp[len] = 0;
    }
    snprintf(out, cap, "%s", tmp);
}

/* Reads <ui_dir><rel> into a malloc'd NUL-terminated buffer. */
static char *read_ui_file(Engine *g, const char *rel, size_t *len_out)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s%s", g->ui_dir, rel);
    unsigned char *buf = NULL;
    size_t len = 0;
    if (read_file(path, &buf, &len) != 0 || !buf) {
        fprintf(stderr, "[html_engine] cannot read %s\n", path);
        return NULL;
    }
    if (len_out) *len_out = len;
    return (char *) buf;
}

/* ------------------------------------------------------------------------- */
/* Stylesheets, fonts, keyframes                                              */
/* ------------------------------------------------------------------------- */

/* unicode-range covers U+0041 ("A")? NULL / unparsable = yes. The text system keeps one face
 * per family, weight and style, so of Google's per-script @font-face splits (hebrew, latin-ext,
 * latin: google-secular-one.css) only the one with the Latin letters is registered. */
static int range_covers_latin(const char *r)
{
    if (!r) return 1;
    int any = 0;
    while (*r) {
        while (*r == ' ' || *r == ',' || *r == '\t') r++;
        if ((r[0] != 'U' && r[0] != 'u') || r[1] != '+') {
            if (*r) r++;
            continue;
        }
        r += 2;
        char lo[16] = "", hi[16] = "";
        size_t k = 0;
        while ((isxdigit((unsigned char) *r) || *r == '?') && k + 1 < sizeof lo) lo[k++] = *r++;
        lo[k] = 0;
        snprintf(hi, sizeof hi, "%s", lo);
        for (char *q = lo; *q; q++) if (*q == '?') *q = '0';
        for (char *q = hi; *q; q++) if (*q == '?') *q = 'F';
        if (*r == '-') {
            r++;
            k = 0;
            while (isxdigit((unsigned char) *r) && k + 1 < sizeof hi) hi[k++] = *r++;
            hi[k] = 0;
        }
        any = 1;
        if (strtol(lo, NULL, 16) <= 0x41 && strtol(hi, NULL, 16) >= 0x41) return 1;
    }
    return !any;
}

static void register_font_faces(Engine *g, const CssSheet *sheet)
{
    uint32_t count = 0;
    const CssFontFace *faces = css_sheet_font_faces(sheet, &count);
    const char *base = css_sheet_name(sheet);
    for (uint32_t i = 0; i < count; i++) {
        const CssFontFace *f = &faces[i];
        if (!f->family || !range_covers_latin(f->unicode_range)) continue;
        int ok = 0;
        for (int s = 0; s < f->src_count && !ok; s++) {
            char key[512];
            if (!f->src[s] || !image_resolve_url(f->src[s], base, key, sizeof(key))) continue;
            const int style = f->style == CSS_FONT_STYLE_NORMAL ? TEXT_STYLE_NORMAL : TEXT_STYLE_ITALIC;
            ok = text_register_face(g->text, f->family, key, f->weight ? f->weight : 400, style) == 0;
            vlog("@font-face '%s' %s -> %s", f->family, key, ok ? "registered" : "FAILED");
        }
        if (!ok && f->src_count) fprintf(stderr, "[html_engine] @font-face '%s': no usable src in %s\n", f->family, base ? base : "?");
    }
}

/* @keyframes -> AnimSystem (later sheets replace sets of the same name). */
static void register_keyframes(Engine *g, const CssSheet *sheet)
{
    const int n = anim_keyframes_load_sheet(&g->anim, sheet, css_computed_initial());
    if (n) vlog("%s: %d @keyframes sets", css_sheet_name(sheet), n);
    const int np = anim_properties_load_sheet(sheet);
    if (np) vlog("%s: %d @property rules", css_sheet_name(sheet), np);
}

static CssSheet *load_sheet_text(Engine *g, const char *text, size_t len, const char *name)
{
    CssSheet *sheet = css_sheet_load(text, len, name);
    if (!sheet) {
        fprintf(stderr, "[html_engine] stylesheet %s: out of memory\n", name);
        return NULL;
    }
    vlog("stylesheet %s: %u rules, %u selectors, %u keyframes", name, css_sheet_rule_count(sheet),
         css_sheet_selector_count(sheet), css_sheet_keyframes_count(sheet));
    (void) g;
    return sheet;
}

/* <link rel=stylesheet href> and <style> in document order. */
static void load_author_sheets(Engine *g)
{
    HtmlElem found[64];
    const size_t n = dom_query_all(g->dom, 0, "link, style", found, 64);
    for (size_t i = 0; i < n && g->n_sheets < HTML_ENGINE_MAX_SHEETS; i++) {
        const char *tag = dom_tag(g->dom, found[i]);
        if (tag && !strcmp(tag, "style")) {
            char *text = dom_get_text(g->dom, found[i]);
            if (!text) continue;
            CssSheet *sheet = load_sheet_text(g, text, strlen(text), g->page);
            free(text);
            if (sheet) g->sheets[g->n_sheets++] = sheet;
            continue;
        }
        const char *rel = dom_get_attr(g->dom, found[i], "rel", NULL);
        const char *href = dom_get_attr(g->dom, found[i], "href", NULL);
        if (!rel || !href) continue;
        /* rel is a space-separated token list ("stylesheet", "alternate stylesheet" is not applied) */
        if (strcmp(rel, "stylesheet") != 0 && strcmp(rel, "Stylesheet") != 0) continue;
        char key[512];
        if (!image_resolve_url(href, g->page, key, sizeof(key))) {
            fprintf(stderr, "[html_engine] stylesheet href '%s' rejected\n", href);
            continue;
        }
        size_t len = 0;
        char *text = read_ui_file(g, key, &len);
        if (!text) continue;
        CssSheet *sheet = load_sheet_text(g, text, len, key);
        free(text);
        if (sheet) g->sheets[g->n_sheets++] = sheet;
    }
}

/* ------------------------------------------------------------------------- */
/* Layout callbacks (image.h / cascade)                                       */
/* ------------------------------------------------------------------------- */

/* gl_backend: lazy rasters (DL_IMAGE.lazy, image.h).  Decodes run on the draw that first
 * needs the pixels, at most ENGINE_IMAGE_DECODE_BUDGET_MS a frame (always one), so a list
 * scrolled into view fills over a few frames instead of stalling one; deterministic
 * captures (the clock override: --render-scenario, ui_test) decode everything at once. */
#define ENGINE_IMAGE_DECODE_BUDGET_MS 4.0

static unsigned glb_lazy_image_resolver(void *user, const void *lazy, int nearest)
{
    Engine *g = user;
    const ImageEntry *e = lazy;
    if (!g || !e) return 0;
    if (!image_entry_resident(e)) {
        /* Over the frame's budget: this draw is skipped, but the next frame resolves it
         * again (glb_submit runs every frame) and at least one decode always goes through,
         * so a deferred image appears a frame or two later instead of never. */
        if (g->decodes && g->decode_ms >= ENGINE_IMAGE_DECODE_BUDGET_MS && g_clock_override < 0.0) {
            g->decodes_deferred++;
            return 0;
        }
        const double t0 = wall_ms();
        image_entry_load(g->images, e); /* a decode / upload failure is logged there, once */
        g->decode_ms += wall_ms() - t0;
        g->decodes++;
    }
    image_entry_touch(g->images, e);
    return image_texture(e, nearest);   /* the same texture for pixelated draws: no twin to fail */
}

/* gl_backend: SVG images (DL_IMAGE.vector = their ImageEntry) at device size. */
static unsigned glb_image_resolver(void *user, const void *vector, int nearest, float dev_w, float dev_h, float frac_x,
                                   float frac_y, int *tex_w, int *tex_h)
{
    (void)user;
    return image_texture_sized((const ImageEntry *)vector, nearest, dev_w, dev_h, frac_x, frac_y, tex_w, tex_h);
}

static int lh_image_size(void *user, const char *url, const char *base, float *w, float *h)
{
    Engine *g = user;
    const ImageEntry *e = image_get(g->images, url, base);
    if (!e || e->failed || e->w <= 0 || e->h <= 0) return 0;
    *w = (float) e->w;
    *h = (float) e->h;
    return 1;
}

static float lh_scrollbar_width(void *user, lxb_dom_node_t *node, float default_w)
{
    Engine *g = user;
    if (g_hide_scrollbars) return 0.0f;
    const HtmlElem e = dom_node_elem(g->dom, node);
    if (!e) return default_w;
    CssComputed tmp;
    if (!cascade_pseudo_computed(g->dom, e, CSS_PE_SCROLLBAR, &tmp)) return default_w;
    if (tmp.width.kind == CSS_LEN_VALUE && tmp.width.pct == 0.0f) return tmp.width.px;
    return default_w;
}

/* ------------------------------------------------------------------------- */
/* Paint hooks                                                                */
/* ------------------------------------------------------------------------- */

static float resolve_radius(const CssLength *l, float ref)
{
    return l->kind == CSS_LEN_VALUE ? l->px + l->pct * ref / 100.0f : 0.0f;
}

static int paint_scrollbar_style_hook(void *user, const Box *box, PaintScrollbarStyle *st)
{
    Engine *g = user;
    if (!box || !box->node) return 0;
    const HtmlElem e = dom_node_elem(g->dom, box->node);
    if (!e) return 0;
    CssComputed bar, track, thumb;
    const uint32_t nb = cascade_pseudo_computed(g->dom, e, CSS_PE_SCROLLBAR, &bar);
    const uint32_t nt = cascade_pseudo_computed(g->dom, e, CSS_PE_SCROLLBAR_TRACK, &track);
    const uint32_t nh = cascade_pseudo_computed(g->dom, e, CSS_PE_SCROLLBAR_THUMB, &thumb);
    if (!nb && !nt && !nh) return 0;
    paint_scrollbar_defaults(st);
    if (nb && bar.width.kind == CSS_LEN_VALUE) st->width = bar.width.px;
    if (nt) {
        st->track_color = paint_resolve_color(track.background_color, &track);
        st->track_radius = resolve_radius(&track.border_radius[0], st->width);
        st->track_shadow = track.box_shadow;
        st->track_shadow_count = track.box_shadow_count;
    }
    if (nh) {
        st->thumb_color = paint_resolve_color(thumb.background_color, &thumb);
        st->thumb_radius = resolve_radius(&thumb.border_radius[0], st->width);
        st->thumb_shadow = thumb.box_shadow;
        st->thumb_shadow_count = thumb.box_shadow_count;
        if (thumb.min_height.kind == CSS_LEN_VALUE) st->thumb_min = thumb.min_height.px;
    }
    return 1;
}

static void *overlay_alloc_cb(void *user, size_t size)
{
    return kn_arena_alloc((KnArena *) user, size);
}

/* Form-control internals: widgets_paint_host() wants the Widgets* as user,
 * PaintHost has one user for all hooks, so forward. */
static void paint_widget_hook(void *user, const PaintWidgetArgs *pa)
{
    Engine *g = user;
    if (g->widgets) widgets_paint_host(g->widgets, pa);
}

/* Form-control intrinsic size from the author font (layout callback). */
static int lh_widget_size(void *user, lxb_dom_node_t *node, const CssComputed *st, float *w, float *h)
{
    Engine *g = user;
    const HtmlElem e = g->widgets ? dom_node_elem(g->dom, node) : 0;
    return e ? widgets_intrinsic_size(g->widgets, e, st, w, h) : 0;
}

static const char *paint_image_src_hook(void *user, const Box *box)
{
    (void) user;
    return box ? box->src : NULL;
}

/* ------------------------------------------------------------------------- */
/* <canvas> bitmaps (html_dom_canvas_put)                                      */
/* ------------------------------------------------------------------------- */

static struct EngineCanvas *canvas_find(Engine *g, const HtmlElem e)
{
    for (int i = 0; i < g->n_canvases; i++)
        if (g->canvases[i].e == e) return &g->canvases[i];
    return NULL;
}

static void canvas_drop(Engine *g, const int i)
{
    if (g->canvases[i].tex && !g->canvases[i].external) {
        GLuint tex = g->canvases[i].tex;
        glDeleteTextures(1, &tex);
    }
    g->canvases[i] = g->canvases[--g->n_canvases];
}

/* ------------------------------------------------------------------------- */
/* <video> (video.h)                                                          */
/* ------------------------------------------------------------------------- */

static int lh_video_size(void *user, const char *url, const char *base, float *w, float *h)
{
    Engine *g = user;
    char path[1024];
    int nw = 0, nh = 0;
    if (!g || !video_sidecar_path(g->ui_dir, url, base, path, sizeof(path)) || !video_probe(path, &nw, &nh)) return 0;
    *w = (float) nw;
    *h = (float) nh;
    return 1;
}

/* The current frame of a <video> box: its source's sidecar played on the engine clock. */
static int engine_video_frame(Engine *g, const Box *box, const HtmlElem e, uint32_t *tex, float *w, float *h)
{
    size_t len = 0;
    const char *src = layout_media_src(box, &len);
    char url[512], path[1024];
    if (!g->videos || !src || !len || len >= sizeof(url)) return 0;
    memcpy(url, src, len);
    url[len] = 0;
    if (!video_sidecar_path(g->ui_dir, url, g->page, path, sizeof(path))) return 0;
    const int flags = (dom_has_attr(g->dom, e, "autoplay") ? VIDEO_AUTOPLAY : 0) | (dom_has_attr(g->dom, e, "loop") ? VIDEO_LOOP : 0);
    unsigned t = 0;
    int tw = 0, th = 0;
    if (!video_players_frame(g->videos, (uintptr_t) e, path, flags, engine_now_ms() / 1000.0, &t, &tw, &th)) return 0;
    *tex = t;
    *w = (float) tw;
    *h = (float) th;
    return 1;
}

static int video_alive(void *user, uintptr_t key)
{
    Engine *g = user;
    return engine_elem_box(g, (HtmlElem) key) != NULL;
}

/* Every frame: playing videos advance in place (the display list keeps the texture,
 * sampled at every submit: no restyle, layout or paint).  One whose sidecar just
 * arrived has a natural size and a first frame to record: relayout and repaint, as
 * for a page image that pops in. */
static void engine_video_tick(Engine *g)
{
    if (!g->videos || !video_players_count(g->videos)) return;
    if (video_players_tick(g->videos, engine_now_ms() / 1000.0, video_alive, g, NULL) > 0) {
        paint_invalidate_all(&g->paint);
        g->need_layout = 1;
        g->lctx.memo_epoch++;
        dom_mark_all_dirty(g->dom);
        g->need_paint = 1;
    }
}

/* The bitmap of a <canvas> box: what the page drew with html_dom_canvas_put, 0 = none
 * (an untouched canvas paints nothing, like a fresh transparent bitmap).  A <video> box:
 * its current frame (engine_video_frame); 0 paints its poster or nothing. */
static int paint_canvas_image_hook(void *user, const Box *box, uint32_t *tex, float *w, float *h, int *flip_y)
{
    Engine *g = user;
    if (!g || !box || !box->node) return 0;
    const HtmlElem e = dom_node_elem(g->dom, box->node);
    if (box->type == BOX_VIDEO) {
        *flip_y = 0;
        return e ? engine_video_frame(g, box, e, tex, w, h) : 0;
    }
    const struct EngineCanvas *c = e ? canvas_find(g, e) : NULL;
    if (!c || !c->tex) return 0;
    *tex = c->tex;
    *w = (float) c->w;
    *h = (float) c->h;
    *flip_y = c->external;
    return 1;
}

static struct EngineCanvas *canvas_slot(Engine *g, const HtmlElem e, int *created)
{
    struct EngineCanvas *c = canvas_find(g, e);
    *created = 0;
    if (c) return c;
    if (g->n_canvases == g->cap_canvases) {
        const int cap = g->cap_canvases ? g->cap_canvases * 2 : 4;
        struct EngineCanvas *grown = realloc(g->canvases, (size_t) cap * sizeof(*grown));
        if (!grown) return NULL;
        g->canvases = grown;
        g->cap_canvases = cap;
    }
    c = &g->canvases[g->n_canvases++];
    memset(c, 0, sizeof(*c));
    c->e = e;
    *created = 1;
    return c;
}

/* html_dom_canvas_gl: the canvas box paints the client's GL texture `tex` (premultiplied
 * RGBA, w x h, row 0 at the bottom) as its bitmap, sampled live at every submit: no copy,
 * the client renders into it before html_ui_render(). The client owns the texture; tex 0
 * forgets it (the canvas paints nothing). */
void engine_canvas_gl(Engine *g, const HtmlElem e, const unsigned tex, const int w, const int h)
{
    if (!g || !e) return;
    if (!tex || w <= 0 || h <= 0) {
        struct EngineCanvas *c = canvas_find(g, e);
        if (c) {
            canvas_drop(g, (int) (c - g->canvases));
            engine_mark_paint(g, e);
        }
        return;
    }
    int created;
    struct EngineCanvas *c = canvas_slot(g, e, &created);
    if (!c) return;
    if (!created && c->tex == tex && c->external && c->w == w && c->h == h) return;
    if (c->tex && !c->external) {
        GLuint own = c->tex;
        glDeleteTextures(1, &own);
    }
    c->tex = tex;
    c->external = 1;
    c->w = w;
    c->h = h;
    engine_mark_paint(g, e); /* the display list gains / resizes / retargets the image */
}

int engine_canvas_put(Engine *g, const HtmlElem e, const unsigned char *rgba, const int w, const int h)
{
    if (!g || !e || !rgba || w <= 0 || h <= 0) return 0;
    int created;
    struct EngineCanvas *c = canvas_slot(g, e, &created);
    if (!c) return 0;
    if (c->external) { /* back from html_dom_canvas_gl to an own bitmap */
        c->tex = 0;
        c->external = 0;
        created = 1;
    }
    /* the engine composites premultiplied (image.h) */
    unsigned char *px = malloc((size_t) w * (size_t) h * 4);
    if (!px) return 0;
    for (size_t i = 0, n = (size_t) w * (size_t) h; i < n; i++) {
        const unsigned a = rgba[i * 4 + 3];
        px[i * 4 + 0] = (unsigned char) ((rgba[i * 4 + 0] * a + 127) / 255);
        px[i * 4 + 1] = (unsigned char) ((rgba[i * 4 + 1] * a + 127) / 255);
        px[i * 4 + 2] = (unsigned char) ((rgba[i * 4 + 2] * a + 127) / 255);
        px[i * 4 + 3] = (unsigned char) a;
    }
    GLint prev_tex = 0, prev_align = 4;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &prev_align);
    if (!c->tex) {
        GLuint tex = 0;
        glGenTextures(1, &tex);
        c->tex = tex;
    }
    glBindTexture(GL_TEXTURE_2D, c->tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    if (created || c->w != w || c->h != h) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0); /* level 0 alone is complete for any MIN_FILTER */
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, prev_align);
    glBindTexture(GL_TEXTURE_2D, (GLuint) prev_tex);
    free(px);
    const int changed = created || c->w != w || c->h != h;
    c->w = w;
    c->h = h;
    if (changed) engine_mark_paint(g, e); /* the display list gains / resizes the image */
    return 1;
}

/* ------------------------------------------------------------------------- */
/* Animation glue                                                             */
/* ------------------------------------------------------------------------- */

/* `key`: ANIM_KEY(elem, ANIM_PSEUDO_*). */
static CssComputed **anim_base_slot(Engine *g, HtmlElem key, int create)
{
    const int ps = ANIM_KEY_PSEUDO(key);
    const HtmlElem e = ANIM_KEY_ELEM(key);
    if (ps > ANIM_PSEUDO_AFTER) return NULL;
    if (e >= g->anim_base_cap[ps]) {
        if (!create) return NULL;
        uint32_t cap = g->anim_base_cap[ps] ? g->anim_base_cap[ps] : 256;
        while (cap <= e) cap *= 2;
        CssComputed **grown = realloc(g->anim_base[ps], cap * sizeof(*grown));
        if (!grown) return NULL;
        memset(grown + g->anim_base_cap[ps], 0, (cap - g->anim_base_cap[ps]) * sizeof(*grown));
        g->anim_base[ps] = grown;
        g->anim_base_cap[ps] = cap;
    }
    return &g->anim_base[ps][e];
}

/* The element's computed record (ps 0) or its ::before / ::after record. */
static CssComputed *anim_record(ElemData *d, int ps)
{
    return ps == ANIM_PSEUDO_BEFORE ? d->before : ps == ANIM_PSEUDO_AFTER ? d->after : d->computed;
}

static void anim_drop_base(Engine *g, HtmlElem key)
{
    CssComputed **slot = anim_base_slot(g, key, 0);
    if (slot && *slot) {
        free(*slot);
        *slot = NULL;
        g->anim_count--;
    }
}

static void anim_forget(Engine *g, HtmlElem e)
{
    if (e < g->anim_hidden_cap) g->anim_hidden[e] = 0;
    for (int ps = ANIM_PSEUDO_NONE; ps <= ANIM_PSEUDO_AFTER; ps++) {
        anim_drop_base(g, ANIM_KEY(e, ps));
        anim_remove_elem(&g->anim, ANIM_KEY(e, ps));
    }
}

static int inherited_differs(const CssComputed *a, const CssComputed *b)
{
    return memcmp((const char *) a + CSS_COMPUTED_INHERITED_BEGIN, (const char *) b + CSS_COMPUTED_INHERITED_BEGIN,
                  CSS_COMPUTED_INHERITED_END - CSS_COMPUTED_INHERITED_BEGIN) != 0;
}

/* Writes the animated style of target `key` over its computed record (the base
 * is kept in the side table).  Children re-inherit when an inherited property
 * of the element moved (pseudo-elements have no element children). */
static void anim_write(Engine *g, HtmlElem key, ElemData *d, const CssComputed *base, double now)
{
    const int ps = ANIM_KEY_PSEUDO(key);
    CssComputed *rec = anim_record(d, ps);
    if (!rec) return;
    CssComputed tmp;
    const CssComputed *s = anim_apply(&g->anim, key, base, &tmp, now);
    const int inherit_moved = ps == ANIM_PSEUDO_NONE && inherited_differs(rec, s);
    if (s != rec) memcpy(rec, s, sizeof(CssComputed));
    const HtmlElem e = ANIM_KEY_ELEM(key);
    if (inherit_moved && dom_first_child(g->dom, e)) dom_mark_dirty(g->dom, e, DOM_DIRTY_DESCENDANTS);
}

/* The cascade just stored a fresh (un-animated) record for the target. */
static void anim_sync_one(Engine *g, HtmlElem key, ElemData *d, double now)
{
    CssComputed *rec = anim_record(d, ANIM_KEY_PSEUDO(key));
    if (!rec) { /* the pseudo-element is gone */
        anim_drop_base(g, key);
        anim_remove_elem(&g->anim, key);
        return;
    }
    const int active = anim_update_elem(&g->anim, key, rec, now);
    if (!active) {
        anim_drop_base(g, key);
        return;
    }
    CssComputed **slot = anim_base_slot(g, key, 1);
    if (!slot) return;
    if (!*slot) {
        *slot = malloc(sizeof(CssComputed));
        if (!*slot) return;
        g->anim_count++;
    }
    memcpy(*slot, rec, sizeof(CssComputed));
    anim_write(g, key, d, *slot, now);
}

static void anim_sync_tree(Engine *g, HtmlElem e, double now);

/* anim_hidden flag of `e` (set: 1/0; get: set < 0). */
static int anim_hidden_flag(Engine *g, HtmlElem e, int set)
{
    if (e >= g->anim_hidden_cap) {
        if (set <= 0) return 0;
        uint32_t cap = g->anim_hidden_cap ? g->anim_hidden_cap : 256;
        while (cap <= e) cap *= 2;
        uint8_t *grown = realloc(g->anim_hidden, cap);
        if (!grown) return 0;
        memset(grown + g->anim_hidden_cap, 0, cap - g->anim_hidden_cap);
        g->anim_hidden = grown;
        g->anim_hidden_cap = cap;
    }
    if (set >= 0) g->anim_hidden[e] = (uint8_t)set;
    return g->anim_hidden[e];
}

/* Element and ::before / ::after: the cascade recomputes all three together.
 *
 * display: none (Chrome, CSS Animations 1 section 3 / CSS Transitions 1
 * section 3): an element that is not rendered runs no animations and no
 * transitions; they are cancelled for it and its whole subtree.  When it is
 * displayed again its animations start over and nothing transitions from the
 * hidden style (no before-change style).  The cascade skips hidden subtrees
 * and does not flag unchanged descendants on re-display, so the whole
 * displayed subtree is re-synced here. */
static void anim_sync_elem(Engine *g, HtmlElem e, ElemData *d, double now)
{
    if (!d->computed) return;
    if (d->computed->display == CSS_DISPLAY_NONE) {
        anim_forget(g, e);
        for (HtmlElem c = dom_next_in_tree(g->dom, e, e); c; c = dom_next_in_tree(g->dom, c, e)) anim_forget(g, c);
        anim_hidden_flag(g, e, 1);
        return;
    }
    if (anim_hidden_flag(g, e, -1)) {
        anim_hidden_flag(g, e, 0);
        anim_sync_tree(g, e, now);
        return;
    }
    for (int ps = ANIM_PSEUDO_NONE; ps <= ANIM_PSEUDO_AFTER; ps++) anim_sync_one(g, ANIM_KEY(e, ps), d, now);
}

/* `e` and its rendered descendants (display: none subtrees skipped).
 * Descendants the cascade flagged themselves (DOM_DIRTY_STYLE_CHANGED) are
 * left to post_restyle, which reaches them later in tree order: syncing them
 * twice would take the animated record for the base. */
static void anim_sync_tree(Engine *g, HtmlElem e, double now)
{
    ElemData *d = dom_elem_data(g->dom, e);
    if (!d || !d->computed) return;
    if (d->computed->display == CSS_DISPLAY_NONE) {
        anim_hidden_flag(g, e, 1);   /* its subtree re-syncs when it is displayed */
        return;
    }
    for (int ps = ANIM_PSEUDO_NONE; ps <= ANIM_PSEUDO_AFTER; ps++) anim_sync_one(g, ANIM_KEY(e, ps), d, now);
    for (HtmlElem c = dom_first_child(g->dom, e); c; c = dom_next(g->dom, c)) {
        const ElemData *cd = dom_elem_data(g->dom, c);
        if (cd && (cd->dirty & DOM_DIRTY_STYLE_CHANGED)) {
            if (cd->computed && cd->computed->display != CSS_DISPLAY_NONE) anim_hidden_flag(g, c, 1);
            continue;   /* post_restyle: anim_sync_elem -> anim_sync_tree of its subtree */
        }
        anim_sync_tree(g, c, now);
    }
}

/* Cascade hook: animated custom properties of the element being computed.
 * The cascade's hook is global and two engines can be alive (g_base and
 * g_page, separate Doms with overlapping element handles), so every cascade
 * entry that can compute ::before/::after records binds its engine first
 * (cascade_bind); engine_destroy unbinds. */
static void anim_cascade_custom_cb(void *user, HtmlElem e, int pe, CssComputed *out)
{
    Engine *g = user;
    const int ps = pe == CSS_PE_NONE ? ANIM_PSEUDO_NONE : pe == CSS_PE_BEFORE ? ANIM_PSEUDO_BEFORE
                 : pe == CSS_PE_AFTER ? ANIM_PSEUDO_AFTER : -1;
    if (ps < 0) return;
    anim_custom_fixup(&g->anim, ANIM_KEY(e, ps), out, engine_now_ms());
}

static void cascade_bind(Engine *g)
{
    cascade_set_custom_hook(anim_cascade_custom_cb, g);
}

/* ------------------------------------------------------------------------- */
/* Layout-side style shadows                                                  */
/* ------------------------------------------------------------------------- */
/* layout_restyled() needs the style an element had before the restyle; the
 * cascade (and anim_write) overwrite ElemData.computed in place, so the
 * engine keeps a copy of the record the layout tree last saw per element. */

static CssComputed **shadow_slot(Engine *g, HtmlElem e, int create)
{
    if (e >= g->shadow_cap) {
        if (!create) return NULL;
        uint32_t cap = g->shadow_cap ? g->shadow_cap : 512;
        while (cap <= e) cap *= 2;
        CssComputed **grown = realloc(g->shadow, cap * sizeof(*grown));
        if (!grown) return NULL;
        memset(grown + g->shadow_cap, 0, (cap - g->shadow_cap) * sizeof(*grown));
        g->shadow = grown;
        g->shadow_cap = cap;
    }
    return &g->shadow[e];
}

static void shadow_set(Engine *g, HtmlElem e, const CssComputed *style)
{
    CssComputed **slot = shadow_slot(g, e, 1);
    if (!slot) return;
    if (!style) {
        free(*slot);
        *slot = NULL;
        return;
    }
    if (!*slot && !(*slot = malloc(sizeof(CssComputed)))) return;
    memcpy(*slot, style, sizeof(CssComputed));
}

static void shadow_free(Engine *g, HtmlElem e)
{
    CssComputed **slot = shadow_slot(g, e, 0);
    if (slot && *slot) {
        free(*slot);
        *slot = NULL;
    }
}

static void shadow_snapshot_all(Engine *g)
{
    for (HtmlElem e = dom_root(g->dom); e; e = dom_next_in_tree(g->dom, e, 0)) {
        ElemData *d = dom_elem_data(g->dom, e);
        shadow_set(g, e, d ? d->computed : NULL);
    }
}

/* What a layout_restyled()/layout_children_changed() result needs next. */
static void account_layout_change(Engine *g, int r)
{
    switch (r) {
        case LAYOUT_RESTYLE_PAINT:
            g->need_paint = 1;
            break;
        case LAYOUT_RESTYLE_TRANSFORM:
            g->need_paint = 1;
            g->observe_pending = 1; /* viewport rects moved */
            break;
        case LAYOUT_RESTYLE_LAYOUT:
            g->need_layout = 1;
            break;
        case LAYOUT_RESTYLE_REBUILT:
            g->need_layout = 1;
            g->stats.partial_rebuilds++;
            g->prof_rebuilds++;
            break;
        default:
            break;
    }
}

static void save_scroll_offsets(Engine *g);

/* Hands the element's current computed record to layout (old = shadow). */
static void restyle_to_layout(Engine *g, HtmlElem e, ElemData *d)
{
    if (!g->tree) {
        g->need_rebuild = 1;
        return;
    }
    CssComputed **slot = shadow_slot(g, e, 0);
    const CssComputed *old = slot ? *slot : NULL;
    /* an animated property can rebuild a subtree (layout_restyled): keep the scroll
     * offsets of the scrollers inside it, like post_restyle does (once per anim tick) */
    if (!g->anim_scroll_saved) {
        save_scroll_offsets(g);
        g->anim_scroll_saved = 1;
        if (g->saved_scroll_len) g->restore_scroll_pending = 1;
    }
    account_layout_change(g, layout_restyled(g->tree, d->node, old));
    shadow_set(g, e, d->computed);
}

/* A ::before/::after box: the pseudo-element's own box among the element box's
 * children (looking through anonymous wrappers), NULL when not found. */
static Box *find_pseudo_box(Box *b, const lxb_dom_node_t *node, int ps)
{
    for (Box *c = b->first_child; c; c = c->next) {
        if (c->node == node && c->pseudo == ps) return c;
        if (c->flags & BOXF_ANONYMOUS) {
            Box *r = find_pseudo_box(c, node, ps);
            if (r) return r;
        }
    }
    return NULL;
}

/* Hands an animated pseudo-element record to layout.  Its box borrows the
 * record (ElemData.before/after, updated in place); layout_restyled() only
 * knows elements, so this does its in-place part for the pseudo box. */
static void pseudo_to_layout(Engine *g, HtmlElem e, ElemData *d, int ps, unsigned flags)
{
    if (!g->tree) {
        g->need_rebuild = 1;
        return;
    }
    Box *eb = d->box;
    if (!eb) return; /* not rendered */
    Box *pb = find_pseudo_box(eb, d->node, ps);
    if (!pb || (flags & ANIM_DIRTY_REBUILD)) { /* visibility (box flags), or not found: rebuild like a restyle */
        restyle_to_layout(g, e, d);
        return;
    }
    if (flags & ANIM_DIRTY_LAYOUT) {
        layout_invalidate(g->tree, pb, 0);
        g->need_layout = 1;
    }
    if (flags & ANIM_DIRTY_TRANSFORM) {
        layout_invalidate(g->tree, pb, 1);
        g->observe_pending = 1;
    }
    pb->paint_dirty = 1;
    for (Box *c = pb->first_child; c; c = c->next) c->paint_dirty = 1; /* the generated text */
    g->need_paint = 1;
}

static void anim_dirty_cb(void *user, HtmlElem key, unsigned flags)
{
    Engine *g = user;
    const HtmlElem e = ANIM_KEY_ELEM(key);
    const int ps = ANIM_KEY_PSEUDO(key);
    ElemData *d = dom_elem_data(g->dom, e);
    if (!d || !d->computed) return;
    if (flags & ANIM_DIRTY_CUSTOM) {
        /* animated custom properties: re-cascade (var()/calc() users re-resolve
         * through anim_cascade_custom_cb); post_restyle then re-applies the rest */
        dom_mark_dirty(g->dom, e, DOM_DIRTY_STYLE);
        return;
    }
    CssComputed **slot = anim_base_slot(g, key, 0);
    if (!slot || !*slot || !anim_record(d, ps)) return;
    anim_write(g, key, d, *slot, engine_now_ms());
    if (ps == ANIM_PSEUDO_NONE) restyle_to_layout(g, e, d); /* layout_restyled() classifies it */
    else pseudo_to_layout(g, e, d, ps, flags);
}

/* ------------------------------------------------------------------------- */
/* Restyle / layout                                                           */
/* ------------------------------------------------------------------------- */

static int scroll_saved(const Engine *g, const HtmlElem e)
{
    for (size_t i = 0; i < g->saved_scroll_len; i++) {
        if (g->saved_scroll[i].elem == e) return 1;
    }
    return 0;
}

/* While a restore is pending (an earlier save in this frame, boxes possibly rebuilt
 * since), a new save keeps those entries and only adds scrollers not saved yet: the
 * earlier values are the ones from before the rebuild. */
static void save_scroll_offsets(Engine *g)
{
    const int merge = g->restore_scroll_pending && g->saved_scroll_len > 0;
    if (!merge) g->saved_scroll_len = 0;
    if (!g->tree) return;
    for (HtmlElem e = dom_root(g->dom); e; e = dom_next_in_tree(g->dom, e, 0)) {
        ElemData *d = dom_elem_data(g->dom, e);
        Box *b = d ? d->box : NULL;
        if (!b || !(b->flags & BOXF_SCROLL_CONTAINER)) continue;
        if (b->scroll_x == 0.0f && b->scroll_y == 0.0f) continue;
        if (merge && scroll_saved(g, e)) continue;
        if (g->saved_scroll_len == g->saved_scroll_cap) {
            const size_t cap = g->saved_scroll_cap ? g->saved_scroll_cap * 2 : 16;
            SavedScroll *grown = realloc(g->saved_scroll, cap * sizeof(*grown));
            if (!grown) return;
            g->saved_scroll = grown;
            g->saved_scroll_cap = cap;
        }
        g->saved_scroll[g->saved_scroll_len++] = (SavedScroll){e, b->scroll_x, b->scroll_y, b};
    }
}

/* Re-applies saved offsets to scrollers whose boxes were rebuilt (call after
 * layout_run so the clamping range is current). */
static void restore_scroll_offsets(Engine *g)
{
    for (size_t i = 0; i < g->saved_scroll_len; i++) {
        Box *b = engine_elem_box(g, g->saved_scroll[i].elem);
        if (!b || !(b->flags & BOXF_SCROLL_CONTAINER)) continue;
        if (b->scroll_x == g->saved_scroll[i].x && b->scroll_y == g->saved_scroll[i].y) continue;
        /* the same box kept its own offset (scrolled since the save: wheel, scroll_to_bottom);
         * a box reallocated at the same address starts unscrolled */
        if ((const void *) b == g->saved_scroll[i].box && (b->scroll_x != 0.0f || b->scroll_y != 0.0f)) continue;
        layout_set_scroll(g->tree, b, g->saved_scroll[i].x, g->saved_scroll[i].y);
    }
    g->saved_scroll_len = 0;
}

/* Boxes keep raw node pointers and borrow styles from ElemData, and layout's
 * box release reads them (ElemData.box bookkeeping): before nodes are
 * destroyed (or moved), layout cuts every box of the affected subtree loose
 * (layout_nodes_removed).  The boxes themselves are released by the
 * layout_children_changed() that the DOM's CHILDREN bit triggers on the next
 * flush; until then they are inert. */
static void detach_boxes(Engine *g, HtmlElem e, int include_root, int clear_elem_boxes)
{
    lxb_dom_node_t *root = dom_elem_node(g->dom, e);
    if (!root || !g->tree) return;
    layout_nodes_removed(g->tree, root, include_root);
    if (clear_elem_boxes) {
        for (HtmlElem c = include_root ? e : dom_next_in_tree(g->dom, e, e); c; c = dom_next_in_tree(g->dom, c, e)) {
            ElemData *d = dom_elem_data(g->dom, c);
            if (d) d->box = NULL;
        }
    }
}

/* After a restyle / DOM mutation: animation sync for re-computed elements,
 * then the incremental layout updates.  Elements under a subtree rebuilt in
 * this pass skip layout_restyled() (their boxes were built from the new
 * styles already). */
#define ENGINE_MAX_PASS_REBUILDS 64

static int under_rebuilt(Dom *dom, HtmlElem e, const HtmlElem *roots, int n)
{
    if (n >= ENGINE_MAX_PASS_REBUILDS) return 1; /* overflowed: assume covered (styles are current anyway) */
    for (HtmlElem a = e; a; a = dom_parent(dom, a)) {
        for (int i = 0; i < n; i++) {
            if (roots[i] == a) return 1;
        }
    }
    return 0;
}

/* Exact: is `e` (or an ancestor) one of the subtrees rebuilt in this pass?  0 once the
 * list overflowed (a child-list change must not be dropped on a guess). */
static int within_rebuilt(Dom *dom, HtmlElem e, const HtmlElem *roots, int n)
{
    if (n > ENGINE_MAX_PASS_REBUILDS || n <= 0) return 0;
    for (HtmlElem a = e; a; a = dom_parent(dom, a)) {
        for (int i = 0; i < n && i < ENGINE_MAX_PASS_REBUILDS; i++) {
            if (roots[i] == a) return 1;
        }
    }
    return 0;
}

static void post_restyle(Engine *g)
{
    const double now = engine_now_ms();
    const uint32_t consumed = DOM_DIRTY_STYLE_CHANGED | DOM_DIRTY_CHILDREN | DOM_DIRTY_TEXT | DOM_DIRTY_LAYOUT;
    HtmlElem rebuilt[ENGINE_MAX_PASS_REBUILDS];
    int n_rebuilt = 0;
    int saved = 0;
    g->structure_dirty = 0;
    for (HtmlElem e = dom_root(g->dom); e; e = dom_next_in_tree(g->dom, e, 0)) {
        ElemData *d = dom_elem_data(g->dom, e);
        if (!d || !(d->dirty & consumed)) continue;
        const uint32_t bits = d->dirty;
        d->dirty &= ~consumed;
        if (bits & DOM_DIRTY_STYLE_CHANGED) anim_sync_elem(g, e, d, now);
        if (!g->tree) {
            g->need_rebuild = 1; /* the full build snapshots every style */
            continue;
        }
        if (!saved) {
            save_scroll_offsets(g);
            saved = 1;
        }
        /* Inside a subtree rebuilt earlier in this pass (tree order: ancestors first) its
         * boxes were built from the current children: an innerHTML of N cards rebuilds
         * the list once, not once per new element below it. */
        if ((bits & (DOM_DIRTY_CHILDREN | DOM_DIRTY_TEXT)) && !within_rebuilt(g->dom, e, rebuilt, n_rebuilt)) {
            const int r = layout_children_changed(g->tree, d->node);
            account_layout_change(g, r);
            if (r == LAYOUT_RESTYLE_REBUILT && n_rebuilt <= ENGINE_MAX_PASS_REBUILDS) {
                if (n_rebuilt < ENGINE_MAX_PASS_REBUILDS) rebuilt[n_rebuilt] = e;
                n_rebuilt++;
            }
        }
        if ((bits & DOM_DIRTY_STYLE_CHANGED) && d->computed) {
            CssComputed **slot = shadow_slot(g, e, 0);
            const int fresh = !slot || !*slot;
            if (fresh && under_rebuilt(g->dom, e, rebuilt, n_rebuilt)) {
                shadow_set(g, e, d->computed);
            } else {
                const int r = layout_restyled(g->tree, d->node, fresh ? NULL : *slot);
                account_layout_change(g, r);
                shadow_set(g, e, d->computed);
                if (r == LAYOUT_RESTYLE_REBUILT && n_rebuilt <= ENGINE_MAX_PASS_REBUILDS) {
                    if (n_rebuilt < ENGINE_MAX_PASS_REBUILDS) rebuilt[n_rebuilt] = e;
                    n_rebuilt++;
                }
            }
        }
    }
    if (saved && g->saved_scroll_len) g->restore_scroll_pending = 1;
    else if (!g->restore_scroll_pending) g->saved_scroll_len = 0;
}

void engine_flush_style(Engine *g)
{
    if (!g || !g->dom) return;
    /* Animated inherited values may have queued descendant restyles; loop a
     * bounded number of times so those settle in the same frame. */
    for (int pass = 0; pass < 3 && (dom_any_style_dirty(g->dom) || g->structure_dirty); pass++) {
        const double t0 = wall_ms();
        cascade_bind(g);
        const uint32_t n = dom_any_style_dirty(g->dom) ? cascade_restyle(g->dom, g->ua, g->sheets, g->n_sheets) : 0;
        const double t1 = wall_ms();
        post_restyle(g);
        const double ms = wall_ms() - t0;
        if (g->profile) {
            prof_add(&g->st_cascade, t1 - t0);
            prof_add(&g->st_post, ms - (t1 - t0));
        }
        g->stats.restyle_ms += ms;
        g->stats.restyles++;
        g->stats.restyled_elems += n;
        g->prof_restyled += n;
        if (g->profile) prof_add(&g->st_restyle, ms);
    }
}

LayoutTree *engine_flush_layout(Engine *g)
{
    if (!g || !g->dom) return NULL;
    if (g->in_flush) return g->tree;
    g->in_flush = 1;
    engine_flush_style(g);
    if (g->need_rebuild || !g->tree || g->need_layout) {
        const double t0 = wall_ms();
        int full = 0;
        if (g->need_rebuild || !g->tree) {
            if (g->tree) {
                save_scroll_offsets(g);
                layout_tree_free(g->tree);
                g->restore_scroll_pending = g->saved_scroll_len > 0;
            }
            g->tree = layout_build(g->dom, &g->lctx);
            full = 1;
            g->stats.rebuilds++;
            g->prof_rebuilds++;
        }
        if (g->tree) layout_run(g->tree, g->vw, g->vh);
        if (full && g->tree) {
            shadow_snapshot_all(g);
            paint_invalidate_all(&g->paint);
        }
        g->need_rebuild = 0;
        g->need_layout = 0;
        g->need_paint = 1;
        g->observe_pending = 1;
        const double ms = wall_ms() - t0;
        g->stats.layout_ms += ms;
        g->stats.layouts++;
        if (g->profile) prof_add(&g->st_layout, ms);
    }
    if (g->restore_scroll_pending && g->tree) {
        g->restore_scroll_pending = 0;
        restore_scroll_offsets(g);
    }
    g->in_flush = 0;
    return g->tree;
}

Box *engine_elem_box(Engine *g, HtmlElem e)
{
    if (!g || !g->tree) return NULL;
    ElemData *d = dom_elem_data(g->dom, e);
    return d ? (Box *) d->box : NULL;
}

/* Resize observers + hover re-hit after a layout (re-entrancy safe). */
static void run_after_layout(Engine *g)
{
    if (!g->observe_pending || g->in_after_layout) return;
    g->in_after_layout = 1;
    g->observe_pending = 0;
    engine_flush_layout(g);
    events_after_layout(&g->events);
    g->in_after_layout = 0;
}

/* ------------------------------------------------------------------------- */
/* Events host: the default layout host, flushed before every query          */
/* ------------------------------------------------------------------------- */

static Box *eh_hit_test(void *user, float x, float y)
{
    Engine *g = user;
    LayoutTree *tree = engine_flush_layout(g);
    if (!tree) return NULL;
    return g->ev_base.hit_test(g->ev_base.user, x, y);
}

static HtmlElem eh_box_elem(void *user, Box *box)
{
    Engine *g = user;
    return g->ev_base.box_elem(g->ev_base.user, box);
}

static Box *eh_elem_box(void *user, HtmlElem e)
{
    Engine *g = user;
    engine_flush_layout(g);
    return g->ev_base.elem_box(g->ev_base.user, e);
}

static int eh_elem_rect(void *user, HtmlElem e, HtmlRect *out)
{
    Engine *g = user;
    engine_flush_layout(g);
    return g->ev_base.elem_rect(g->ev_base.user, e, out);
}

static int eh_scroll_info(void *user, Box *box, EvScrollInfo *out)
{
    Engine *g = user;
    return g->ev_base.scroll_info(g->ev_base.user, box, out);
}

static int eh_scrollbar(void *user, Box *box, int vertical, float x, float y, EvScrollbar *out)
{
    Engine *g = user;
    return g->ev_base.scrollbar(g->ev_base.user, box, vertical, x, y, out);
}

static void eh_set_scroll(void *user, Box *box, float x, float y)
{
    Engine *g = user;
    if (!g->tree) return;
    layout_set_scroll(g->tree, box, x, y); /* marks the scroller's paint dirty */
    g->need_paint = 1;
    g->observe_pending = 1;
}

static void eh_restyle(void *user)
{
    engine_flush_style((Engine *) user);
}

/* ------------------------------------------------------------------------- */
/* Widgets host                                                               */
/* ------------------------------------------------------------------------- */

static const CssComputed *wh_computed(void *user, HtmlElem e)
{
    Engine *g = user;
    cascade_bind(g);
    return cascade_elem_computed(g->dom, e);
}

static const CssComputed *wh_pseudo_computed(void *user, HtmlElem e, int pseudo)
{
    Engine *g = user;
    CssComputed *slot = &g->pseudo_ring[g->pseudo_next];
    if (!cascade_pseudo_computed(g->dom, e, pseudo, slot)) return NULL;
    g->pseudo_next = (g->pseudo_next + 1) % HTML_ENGINE_PSEUDO_RING;
    return slot;
}

static int wh_elem_geom(void *user, HtmlElem e, WidgetGeom *out)
{
    Engine *g = user;
    engine_flush_layout(g);
    Box *box = engine_elem_box(g, e);
    HtmlRect r;
    if (!box || !layout_box_rect_in_viewport(box, &r)) return 0;
    /* Edges scale with the ancestor transforms (#uiBase is scaled). */
    const float kx = box->w > 0 ? r.w / box->w : 1.0f;
    const float ky = box->h > 0 ? r.h / box->h : 1.0f;
    out->x = r.x;
    out->y = r.y;
    out->w = r.w;
    out->h = r.h;
    for (int i = 0; i < 4; i++) {
        const float k = (i == CSS_TOP || i == CSS_BOTTOM) ? ky : kx;
        out->border[i] = box->border[i] * k;
        out->padding[i] = box->padding[i] * k;
    }
    return 1;
}

/* Untransformed border box at (0,0) + the viewport point in that space
 * (caret hit testing and range mapping stay in CSS px under #uiBase's scale). */
static int wh_elem_local(void *user, HtmlElem e, float vx, float vy, WidgetGeom *out, float *lx, float *ly)
{
    Engine *g = user;
    engine_flush_layout(g);
    Box *box = engine_elem_box(g, e);
    if (!box) return 0;
    out->x = 0.0f;
    out->y = 0.0f;
    out->w = box->w;
    out->h = box->h;
    memcpy(out->border, box->border, sizeof(out->border));
    memcpy(out->padding, box->padding, sizeof(out->padding));
    float ax = 0.0f, ay = 0.0f;
    layout_point_to_box(box, vx, vy, &ax, &ay); /* absolute layout space, like box->x/y */
    if (lx) *lx = ax - box->x;
    if (ly) *ly = ay - box->y;
    return 1;
}

static int wh_font_char_widths(void *user, TextFont *font, float *avg, float *max)
{
    (void) user;
    return font ? text_font_char_widths(font, avg, max) : 0;
}

static void wh_invalidate(void *user, HtmlElem e, int relayout)
{
    Engine *g = user;
    Box *box = engine_elem_box(g, e);
    if (relayout) {
        if (box && g->tree) layout_invalidate(g->tree, box, 0);
        else g->need_rebuild = 1;
        g->need_layout = 1;
    }
    /* Widget internals are recorded into the control's stacking context. */
    if (box) paint_mark_dirty(&g->paint, box);
    else paint_invalidate_all(&g->paint);
    g->need_paint = 1;
}

static int wh_fire(void *user, HtmlElem e, const char *type, int key)
{
    Engine *g = user;
    return events_fire(&g->events, e, type, key, 0);
}

static void wh_set_capture(void *user, HtmlElem e)
{
    Engine *g = user;
    Box *box = engine_elem_box(g, e);
    if (box) events_set_pointer_capture(&g->events, box);
}

static void wh_release_capture(void *user)
{
    Engine *g = user;
    events_release_pointer_capture(&g->events);
}

static void wh_focus(void *user, HtmlElem e)
{
    Engine *g = user;
    events_focus(&g->events, e);
}

/* System clipboard for text inputs (Ctrl+C/X/V): GLFW on the window whose
 * context is current (the UI renders on it; headless: the null-platform window). */
static const char *wh_clipboard_get(void *user)
{
    (void) user;
    GLFWwindow *window = headless_current_window();
    return window ? glfwGetClipboardString(window) : NULL;
}

static void wh_clipboard_set(void *user, const char *utf8)
{
    (void) user;
    GLFWwindow *window = headless_current_window();
    if (window && utf8) glfwSetClipboardString(window, utf8);
}

static void wh_blur(void *user, HtmlElem e)
{
    Engine *g = user;
    if (!e || events_focused(&g->events) == e) events_blur(&g->events);
}

/* ------------------------------------------------------------------------- */
/* Page text selection (selection.c): mousedown's default action outside form */
/* controls, Ctrl+C / Ctrl+A, the highlight through PaintHost.selection_range */
/* ------------------------------------------------------------------------- */

static int paint_selection_hook(void *user, const Box *tb, const TextRun *run, uint32_t *start, uint32_t *end,
                                CssColor *bg, CssColor *fg, int *has_fg)
{
    Engine *g = user;
    return sel_run_range(&g->sel, tb, run, start, end, bg, fg, has_fg);
}

static void sel_mark_contexts(Engine *g, Box *b)
{
    if (paint_is_stacking_context(b)) paint_mark_dirty(&g->paint, b);
    for (Box *c = b->first_child; c; c = c->next) sel_mark_contexts(g, c);
}

/* The region's paint contexts re-record (the highlight changed). */
static void sel_repaint(Engine *g, HtmlElem root)
{
    Box *b = root ? engine_elem_box(g, root) : NULL;
    if (!b) return;
    paint_mark_dirty(&g->paint, b);
    sel_mark_contexts(g, b);
    g->need_paint = 1;
}

static void sel_drop(Engine *g)
{
    const HtmlElem root = g->sel.root;
    const int was = sel_active(&g->sel);
    sel_clear(&g->sel);
    if (was) sel_repaint(g, root);
}

/* Segments of the current region on the current layout; 0 when it is gone. */
static int sel_refresh_segments(Engine *g)
{
    if (!g->sel.root || !engine_flush_layout(g)) return 0;
    if (!dom_elem_data(g->dom, g->sel.root)) return 0;
    Box *b = engine_elem_box(g, g->sel.root);
    if (!b) return 0;
    sel_collect(&g->sel, b);
    return 1;
}

/* Before paint: entries hold Box pointers, so re-resolve on the new layout. */
static void sel_before_paint(Engine *g)
{
    if (!g->sel.root) return;
    if (!sel_active(&g->sel)) { /* a caret only (a click): nothing to paint */
        g->sel.n_entries = 0;
        return;
    }
    if (!sel_refresh_segments(g) || !sel_resolve(&g->sel, g->dom)) {
        sel_clear(&g->sel);
        paint_invalidate_all(&g->paint); /* the region (or its boxes) is gone: no cached highlight survives */
    }
}

static void sel_node_removed(Engine *g, HtmlElem e, int include_root)
{
    if (!g->sel.root) return;
    lxb_dom_node_t *n = dom_elem_node(g->dom, e);
    if (!n) return;
    lxb_dom_node_t *root = dom_elem_node(g->dom, g->sel.root);
    const int root_gone = root && ((include_root && root == n) || sel_node_inside(root, n));
    const int ends_gone = sel_node_inside(g->sel.anchor.node, n) || sel_node_inside(g->sel.focus.node, n);
    if (root_gone || ends_gone) {
        sel_clear(&g->sel);
        paint_invalidate_all(&g->paint); /* rare: the highlight may sit in contexts the removal leaves alone */
        g->need_paint = 1;
    }
}

static HtmlElem sel_hit(Engine *g, float x, float y)
{
    Box *b = eh_hit_test(g, x, y);
    return b ? eh_box_elem(g, b) : 0;
}

/* A left press: mousedown's default action, after events_mouse_button ran it
 * (a prevented mousedown or a widget capture starts nothing and keeps the
 * selection, like Chrome). */
static void sel_press(Engine *g, int mods)
{
    Selection *s = &g->sel;
    if (g->events.down_prevented) return;
    const double now = engine_now_ms();
    const float x = g->mouse_x, y = g->mouse_y;
    if (now - s->last_press_ms <= SEL_MULTI_CLICK_MS && fabsf(x - s->last_press_x) <= SEL_MULTI_CLICK_PX
        && fabsf(y - s->last_press_y) <= SEL_MULTI_CLICK_PX) s->clicks = s->clicks >= 3 ? 1 : s->clicks + 1;
    else s->clicks = 1;
    s->last_press_ms = now;
    s->last_press_x = x;
    s->last_press_y = y;

    const HtmlElem target = sel_hit(g, x, y);
    HtmlElem region = sel_region_for(g->dom, target);
    const HtmlElem old_root = s->root;
    const int had = sel_active(s);
    /* only a press on an element holding selectable text starts one: a canvas, an image or an
     * empty panel (the editor's viewport) clears instead of selecting the page's text */
    Box *tbox = region ? engine_elem_box(g, target) : NULL;
    while (tbox && tbox->type == BOX_INLINE && tbox->parent) tbox = tbox->parent; /* its text sits in the block's lines */
    if (tbox && tbox->scrollbar_w > 0) { /* a press on the vertical scrollbar is not a text press */
        float lx, ly;
        layout_point_to_box(tbox, x, y, &lx, &ly);
        const float bar_x = (tbox->flags & BOXF_RTL) ? box_padding_x(tbox) : box_padding_x(tbox) + box_padding_w(tbox) - tbox->scrollbar_w;
        if (lx >= bar_x && lx < bar_x + tbox->scrollbar_w) return;
    }
    if (!tbox || !sel_collect(s, tbox)) region = 0;
    if (!region) { /* click-away */
        sel_drop(g);
        return;
    }
    Box *rb = engine_elem_box(g, region);
    if (!rb) return;
    if (s->root != region) sel_clear(s);
    s->root = region;
    sel_collect(s, rb);
    SelPoint p;
    if (!sel_point_at(s, x, y, &p)) {
        sel_clear(s);
        if (had) sel_repaint(g, old_root);
        return;
    }
    if ((mods & EV_MOD_SHIFT) && had && old_root == region && s->clicks == 1) { /* shift+click extends */
        s->focus = p;
        s->granularity = SEL_CHAR;
    } else if (s->clicks >= 2) {
        SelPoint a, b;
        const int got = s->clicks == 2 ? sel_word_at(s, p, &a, &b) : sel_block_at(s, p, &a, &b);
        if (!got) a = b = p;
        s->granularity = s->clicks == 2 ? SEL_WORD : SEL_BLOCK;
        s->word_a = a;
        s->word_b = b;
        s->anchor = a;
        s->focus = b;
    } else {
        s->granularity = SEL_CHAR;
        s->anchor = s->focus = p;
    }
    s->dragging = 1;
    if (had && old_root != region) sel_repaint(g, old_root);
    sel_repaint(g, region);
}

static void sel_drag_to(Engine *g, float x, float y)
{
    Selection *s = &g->sel;
    if (!s->dragging || !sel_refresh_segments(g)) return;
    SelPoint p;
    if (!sel_point_at(s, x, y, &p)) return;
    if (p.node == s->focus.node && p.off == s->focus.off && s->granularity == SEL_CHAR) return;
    sel_extend_to(s, p);
    sel_repaint(g, s->root);
}

/* Dragging above / below a scrolling region scrolls it (once per frame). */
static void sel_autoscroll(Engine *g)
{
    Selection *s = &g->sel;
    if (!s->dragging || !s->root) return;
    Box *b = engine_elem_box(g, s->root);
    if (!b) return;
    Box *sc = (b->flags & BOXF_SCROLL_CONTAINER) ? b : b->scroller;
    EvScrollInfo si;
    HtmlRect r;
    if (!sc || !eh_scroll_info(g, sc, &si) || !si.can_y || !layout_box_rect_in_viewport(sc, &r)) return;
    float d = g->mouse_y < r.y ? g->mouse_y - r.y : g->mouse_y > r.y + r.h ? g->mouse_y - (r.y + r.h) : 0.0f;
    if (d == 0.0f) return;
    if (d > SEL_AUTOSCROLL_MAX_PX) d = SEL_AUTOSCROLL_MAX_PX;
    if (d < -SEL_AUTOSCROLL_MAX_PX) d = -SEL_AUTOSCROLL_MAX_PX;
    const float max_y = si.scroll_h - si.client_h > 0 ? si.scroll_h - si.client_h : 0.0f;
    float ny = si.scroll_y + d;
    ny = ny < 0 ? 0 : ny > max_y ? max_y : ny;
    if (ny == si.scroll_y) return;
    eh_set_scroll(g, sc, si.scroll_x, ny);
    sel_drag_to(g, g->mouse_x, g->mouse_y);
}

static void sel_copy(Engine *g)
{
    if (!sel_refresh_segments(g)) return;
    char *text = sel_text(&g->sel);
    if (!text) return;
    wh_clipboard_set(g, text);
    vlog("selection copied (%zu bytes)", strlen(text));
    free(text);
}

/* Ctrl+A outside a text control: the focused element's region (the console's
 * .logs, focusable through tabindex), else the current selection's region. */
static int sel_select_all(Engine *g)
{
    Selection *s = &g->sel;
    const HtmlElem f = events_focused(&g->events);
    HtmlElem region = f ? sel_region_for(g->dom, f) : 0;
    if (!region) region = s->root;
    if (!region) return 0;
    const HtmlElem old_root = s->root;
    if (old_root && old_root != region) sel_drop(g);
    s->root = region;
    SelPoint a, b;
    if (!sel_refresh_segments(g) || !sel_all(s, &a, &b)) {
        sel_clear(s);
        return 0;
    }
    s->anchor = a;
    s->focus = b;
    s->granularity = SEL_CHAR;
    s->dragging = 0;
    sel_repaint(g, region);
    return 1;
}

/* Ctrl/Cmd+C, Ctrl/Cmd+A and Escape for the page selection.  Text controls
 * keep their own (widgets.c).  Returns 1 when the key was taken. */
static int sel_key(Engine *g, int key, int action, int mods)
{
    if (action != GLFW_PRESS && action != GLFW_REPEAT) return 0;
    const HtmlElem f = events_focused(&g->events);
    if (f && events_takes_text(&g->events, f)) return 0;
    const int accel = (mods & (GLFW_MOD_CONTROL | GLFW_MOD_SUPER)) != 0 && !(mods & GLFW_MOD_ALT);
    if (accel && key == GLFW_KEY_C && sel_active(&g->sel)) {
        if (action == GLFW_PRESS) sel_copy(g);
        return 1;
    }
    if (accel && key == GLFW_KEY_A) return sel_select_all(g);
    if (key == GLFW_KEY_ESCAPE && sel_active(&g->sel)) sel_drop(g); /* and the key goes on */
    return 0;
}

int engine_selection_text(Engine *g, char *buf, size_t cap)
{
    if (!g || !buf || !cap) return -1;
    buf[0] = 0;
    if (!sel_active(&g->sel) || !sel_refresh_segments(g)) return -1;
    char *t = sel_text(&g->sel);
    if (!t) return -1;
    snprintf(buf, cap, "%s", t);
    const int n = (int) strlen(t);
    free(t);
    return n;
}

/* ------------------------------------------------------------------------- */
/* engine.h accessors                                                         */
/* ------------------------------------------------------------------------- */

Engine *engine_get(void) { return g_engine; }
Dom *engine_dom(Engine *g) { return g ? g->dom : NULL; }
Widgets *engine_widgets(Engine *g) { return g ? g->widgets : NULL; }
EventSystem *engine_events(Engine *g) { return g ? &g->events : NULL; }

void engine_viewport(Engine *g, float *w, float *h)
{
    if (w) *w = g ? g->vw : 0.0f;
    if (h) *h = g ? g->vh : 0.0f;
}

void engine_children_changed(Engine *g, HtmlElem e)
{
    if (!g || !e) return;
    if (dom_elem_data(g->dom, e)) dom_mark_dirty(g->dom, e, DOM_DIRTY_CHILDREN);
    else g->need_rebuild = 1;
    g->structure_dirty = 1;
    g->need_paint = 1;
}

void engine_mark_paint(Engine *g, HtmlElem e)
{
    if (!g) return;
    Box *box = e ? engine_elem_box(g, e) : NULL;
    if (box) paint_mark_dirty(&g->paint, box);
    else paint_invalidate_all(&g->paint);
    g->need_paint = 1;
}

void engine_before_remove(Engine *g, HtmlElem e, int include_root)
{
    if (!g || !e) return;
    /* canvas bitmaps of the removed elements go with them */
    for (int i = g->n_canvases - 1; i >= 0; i--) {
        HtmlElem a = g->canvases[i].e;
        if (!include_root && a == e) continue;
        while (a && a != e) a = dom_parent(g->dom, a);
        if (a == e) canvas_drop(g, i);
    }
    /* <video> players are dropped by engine_video_tick once their element has no box */
    sel_node_removed(g, e, include_root);
    /* Boxes point at the nodes about to be destroyed (text nodes included). */
    detach_boxes(g, e, include_root, 0);
    if (include_root) {
        events_elem_removed(&g->events, e);
        if (g->widgets) widgets_elem_removed(g->widgets, e);
        anim_forget(g, e);
        shadow_free(g, e);
    }
    for (HtmlElem c = dom_next_in_tree(g->dom, e, e); c; c = dom_next_in_tree(g->dom, c, e)) {
        if (!include_root) {
            /* direct children: events/widgets handle whole subtrees */
            if (dom_parent(g->dom, c) == e) {
                events_elem_removed(&g->events, c);
                if (g->widgets) widgets_elem_removed(g->widgets, c);
            }
        }
        anim_forget(g, c);
        shadow_free(g, c);
    }
    /* the element losing children relayouts (dom.c marks it as well) */
    const HtmlElem owner = include_root ? dom_parent(g->dom, e) : e;
    if (owner) engine_children_changed(g, owner);
    else g->need_rebuild = 1;
}

void engine_before_move(Engine *g, HtmlElem e)
{
    if (!g || !e) return;
    const HtmlElem old_parent = dom_parent(g->dom, e);
    if (!old_parent) return; /* detached: nothing laid out */
    /* The old parent's boxes for the subtree are released by its rebuild; the
     * new parent's rebuild must be able to register fresh boxes. */
    detach_boxes(g, e, 1, 1);
    engine_children_changed(g, old_parent);
}

int html_ui_backend_key_consumed(void)
{
    return g_engine ? g_engine->key_consumed : 0;
}

void engine_stats(EngineStats *out, int reset)
{
    if (!g_engine) {
        if (out) memset(out, 0, sizeof(*out));
        return;
    }
    if (out) {
        *out = g_engine->stats;
        out->dl_items = g_engine->dl.n;
        if (g_engine->glb) {
            const GlbStats s = glb_stats(g_engine->glb);
            out->draw_calls = s.draw_calls;
            out->vertices = s.vertices;
        }
    }
    if (reset) memset(&g_engine->stats, 0, sizeof(g_engine->stats));
}

/* ------------------------------------------------------------------------- */
/* Debug dump                                                                 */
/* ------------------------------------------------------------------------- */

typedef struct DumpCtx {
    Engine *g;
    void (*print)(const char *, void *);
    void *user;
} DumpCtx;

static const char *box_type_name(int t)
{
    static const char *names[] = {"block", "inline", "text", "br", "img", "canvas", "video", "input", "select",
                                  "flex", "grid", "table", "row", "cell"};
    return t >= 0 && t < (int) (sizeof(names) / sizeof(names[0])) ? names[t] : "?";
}

static void dump_box(DumpCtx *c, const Box *b, int depth)
{
    char line[512];
    int len = snprintf(line, sizeof(line), "%*s%s", depth * 2, "", box_type_name(b->type));
    if (b->node && b->node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
        const HtmlElem e = dom_node_elem(c->g->dom, b->node);
        const char *id = e ? dom_get_attr(c->g->dom, e, "id", NULL) : NULL;
        const char *cls = e ? dom_get_attr(c->g->dom, e, "class", NULL) : NULL;
        len += snprintf(line + len, sizeof(line) - (size_t) len, " <%s%s%s%s%s>", e ? dom_tag(c->g->dom, e) : "?",
                        id ? "#" : "", id ? id : "", cls && *cls ? "." : "", cls && *cls ? cls : "");
    } else if (b->type == BOX_TEXT && b->text) {
        const uint32_t n = b->text_len > 40 ? 40 : b->text_len;
        len += snprintf(line + len, sizeof(line) - (size_t) len, " \"%.*s%s\"", (int) n, b->text, b->text_len > 40 ? "..." : "");
    } else if (b->flags & BOXF_ANONYMOUS) {
        len += snprintf(line + len, sizeof(line) - (size_t) len, " (anon)");
    }
    if (b->pseudo) len += snprintf(line + len, sizeof(line) - (size_t) len, b->pseudo == 1 ? "::before" : "::after");
    HtmlRect r = {0, 0, 0, 0};
    const int has_rect = b->type != BOX_TEXT && layout_box_rect_in_viewport(b, &r);
    if (len < (int) sizeof(line)) {
        if (has_rect) {
            snprintf(line + len, sizeof(line) - (size_t) len, "  local(%.1f,%.1f %.1fx%.1f) vp(%.1f,%.1f %.1fx%.1f)%s%s%s%s", b->x, b->y, b->w, b->h,
                     r.x, r.y, r.w, r.h, (b->flags & BOXF_HAS_TRANSFORM) ? " T" : "", (b->flags & BOXF_STACKING_CONTEXT) ? " SC" : "",
                     (b->flags & BOXF_NO_POINTER) ? " PE-none" : "", (b->flags & BOXF_HIDDEN) ? " hidden" : "");
        } else {
            snprintf(line + len, sizeof(line) - (size_t) len, "  local(%.1f,%.1f %.1fx%.1f)", b->x, b->y, b->w, b->h);
        }
    }
    c->print(line, c->user);
    for (const Box *ch = b->first_child; ch; ch = ch->next) dump_box(c, ch, depth + 1);
}

void engine_dump_boxes(Engine *g, HtmlElem e, void (*print)(const char *, void *), void *user)
{
    if (!g || !print) return;
    engine_flush_layout(g);
    const Box *root = e ? engine_elem_box(g, e) : (g->tree ? layout_tree_root(g->tree) : NULL);
    if (!root) {
        print("(no box)", user);
        return;
    }
    DumpCtx c = {g, print, user};
    dump_box(&c, root, 0);
}

/* KRUNKNATIVE_HTML_DUMP_DL=1: prints the display list once per paint build. */
static void dump_display_list(const DisplayList *dl)
{
    static const char *ops[] = {"RECT", "RRECT", "BORDER", "IMAGE", "GRADIENT", "GLYPHS", "BOX_SHADOW", "TEXT_SHADOW",
                                "PUSH_CLIP_RECT", "PUSH_CLIP_RRECT", "PUSH_CLIP_POLY", "POP_CLIP", "PUSH_TRANSFORM",
                                "POP_TRANSFORM", "PUSH_GROUP", "POP_GROUP", "SCROLL_OFFSET", "BACKDROP", "MASK_BEGIN"};
    fprintf(stderr, "[html_engine] display list: %d items\n", dl->n);
    for (int i = 0; i < dl->n; i++) {
        const DlItem *it = &dl->items[i];
        fprintf(stderr, "  %4d %-16s", i, it->op < DL_OP_COUNT ? ops[it->op] : "?");
        switch (it->op) {
            case DL_SCROLL_OFFSET:
                fprintf(stderr, " %.1f,%.1f", it->u.scroll.dx, it->u.scroll.dy);
                break;
            case DL_RECT: {
                const DlRect r = it->u.rect.rect;
                const CssColor c = it->u.rect.color;
                fprintf(stderr, " %.1f,%.1f %.1fx%.1f rgba(%d,%d,%d,%d)", r.x, r.y, r.w, r.h, c.r, c.g, c.b, c.a);
                break;
            }
            case DL_RRECT: {
                const DlRect r = it->u.rrect.rect;
                const CssColor c = it->u.rrect.color;
                fprintf(stderr, " %.1f,%.1f %.1fx%.1f rgba(%d,%d,%d,%d)", r.x, r.y, r.w, r.h, c.r, c.g, c.b, c.a);
                break;
            }
            case DL_IMAGE: {
                const DlRect r = it->u.image.dst;
                fprintf(stderr, " tex %u %.1f,%.1f %.1fx%.1f op %.2f", it->u.image.tex, r.x, r.y, r.w, r.h, it->u.image.opacity);
                break;
            }
            case DL_GLYPHS: {
                const CssColor c = it->u.glyphs.color;
                fprintf(stderr, " n %d rgba(%d,%d,%d,%d)", it->u.glyphs.n, c.r, c.g, c.b, c.a);
                if (it->u.glyphs.n) fprintf(stderr, " q0 %.1f,%.1f %.1fx%.1f tex %u", it->u.glyphs.quads[0].x, it->u.glyphs.quads[0].y,
                                            it->u.glyphs.quads[0].w, it->u.glyphs.quads[0].h, it->u.glyphs.quads[0].tex);
                break;
            }
            case DL_PUSH_CLIP_RECT: {
                const DlRect r = it->u.clip_rect.rect;
                fprintf(stderr, " %.1f,%.1f %.1fx%.1f", r.x, r.y, r.w, r.h);
                break;
            }
            case DL_PUSH_TRANSFORM: {
                const Mat2D m = it->u.transform.m;
                fprintf(stderr, " [%.3f %.3f %.3f %.3f %.1f %.1f]", m.a, m.b, m.c, m.d, m.e, m.f);
                break;
            }
            case DL_PUSH_GROUP:
                fprintf(stderr, " opacity %.2f filters %d", it->u.group.opacity, it->u.group.n_filters);
                break;
            default: break;
        }
        fputc('\n', stderr);
    }
}

/* ------------------------------------------------------------------------- */
/* Cursor                                                                     */
/* ------------------------------------------------------------------------- */

static void update_cursor(Engine *g)
{
    GLFWwindow *window = headless_current_window();
    if (!window || !html_ui_is_menu_open()) return; /* in game the client owns the cursor (disabled) */
    int shape;
    switch (events_cursor(&g->events)) {
        case CSS_CURSOR_POINTER: shape = 1; break;
        case CSS_CURSOR_TEXT: shape = 2; break;
        case CSS_CURSOR_CROSSHAIR: shape = 3; break;
        case CSS_CURSOR_EW_RESIZE: shape = 4; break;
        case CSS_CURSOR_NS_RESIZE: shape = 5; break;
        case CSS_CURSOR_NWSE_RESIZE: shape = 6; break;
        case CSS_CURSOR_NESW_RESIZE: shape = 7; break;
        case CSS_CURSOR_MOVE: shape = 8; break;
        default: shape = 0; break; /* auto, default, none (not hidden while the menu is open), ... */
    }
    if (shape == g->cursor_shape) return;
    g->cursor_shape = shape;
    static const int ids[9] = {GLFW_ARROW_CURSOR, GLFW_HAND_CURSOR, GLFW_IBEAM_CURSOR, GLFW_CROSSHAIR_CURSOR,
                               GLFW_RESIZE_EW_CURSOR, GLFW_RESIZE_NS_CURSOR, GLFW_RESIZE_NWSE_CURSOR,
                               GLFW_RESIZE_NESW_CURSOR, GLFW_RESIZE_ALL_CURSOR};
    if (!g->cursors[shape]) g->cursors[shape] = glfwCreateStandardCursor(ids[shape]);
    glfwSetCursor(window, shape ? g->cursors[shape] : NULL);
}

/* ------------------------------------------------------------------------- */
/* html_ui_backend.h                                                          */
/* ------------------------------------------------------------------------- */

static double detect_display_hz(void)
{
    const char *env = getenv("KRUNKNATIVE_HTML_DISPLAY_HZ");
    double hz = env ? atof(env) : 0.0;
    if (hz <= 0.0) {
        GLFWmonitor *monitor = glfwGetPrimaryMonitor();
        const GLFWvidmode *mode = monitor ? glfwGetVideoMode(monitor) : NULL;
        hz = mode && mode->refreshRate > 0 ? (double) mode->refreshRate : HTML_ENGINE_DEFAULT_DISPLAY_HZ;
    }
    if (hz < HTML_ENGINE_MIN_DISPLAY_HZ) hz = HTML_ENGINE_MIN_DISPLAY_HZ;
    if (hz > HTML_ENGINE_MAX_DISPLAY_HZ) hz = HTML_ENGINE_MAX_DISPLAY_HZ;
    return hz;
}

static void engine_destroy(Engine *g)
{
    if (!g) return;
    if (g->tree) layout_tree_free(g->tree);
    paint_ctx_free(&g->paint);
    dl_free(&g->dl);
    dl_free(&g->overlay);
    dl_free(&g->top_dl);
    kn_arena_free(&g->overlay_arena);
    if (g->widgets) widgets_destroy(g->widgets);
    sel_free(&g->sel);
    events_fini(&g->events);
    anim_free(&g->anim);
    cascade_set_custom_hook(NULL, NULL);
    for (int ps = 0; ps < 3; ps++) {
        for (uint32_t i = 0; i < g->anim_base_cap[ps]; i++) free(g->anim_base[ps][i]);
        free(g->anim_base[ps]);
    }
    free(g->anim_hidden);
    for (uint32_t i = 0; i < g->shadow_cap; i++) free(g->shadow[i]);
    free(g->shadow);
    free(g->saved_scroll);
    while (g->n_canvases) canvas_drop(g, g->n_canvases - 1);
    free(g->canvases);
    video_players_destroy(g->videos);
    if (g->glb) glb_destroy(g->glb);
    if (g->images) image_cache_destroy(g->images);
    if (g->text) text_destroy(g->text);
    for (int i = 0; i < g->n_sheets; i++) css_sheet_destroy(g->sheets[i]);
    if (g->ua) css_sheet_destroy(g->ua);
    if (g->dom) dom_destroy(g->dom);
    for (size_t i = 0; i < sizeof g->cursors / sizeof g->cursors[0]; i++) {
        if (g->cursors[i]) glfwDestroyCursor(g->cursors[i]);
    }
    free(g);
}

/* Creates the engine for <ui_dir><page> and makes it current (g_engine) while it
 * loads; NULL (and g_engine NULL) on failure. */
static Engine *engine_create(const char *ui_dir, const char *page, const int width, const int height)
{
    Engine *g = calloc(1, sizeof(Engine));
    if (!g) return NULL;
    g_engine = g;
    snprintf(g->page, sizeof(g->page), "%s", page && *page ? page : "index.html");
    g->verbose = getenv("KRUNKNATIVE_HTML_VERBOSE") != NULL;
    g->profile = getenv("KRUNKNATIVE_HTML_PROFILE") != NULL;
    g->dump_dl = getenv("KRUNKNATIVE_HTML_DUMP_DL") != NULL;
    g->fb_w = width > 0 ? width : 1;
    g->fb_h = height > 0 ? height : 1;
    g->vw = (float) g->fb_w;
    g->vh = (float) g->fb_h;
    css_values_set_viewport(g->vw, g->vh);
    {   /* sheets loaded below evaluate their @media against this viewport */
        CssMediaEnv env = *css_media_env();
        env.width = g->vw;
        env.height = g->vh;
        css_media_set_env(&env);
    }
    g->display_hz = detect_display_hz();

    make_ui_dir(ui_dir, g->ui_dir, sizeof(g->ui_dir));
    if (!dir_exists(g->ui_dir)) {
        fprintf(stderr, "[html_engine] ui dir %s not found\n", g->ui_dir);
        goto fail;
    }
    vlog("ui dir %s, viewport %dx%d, display %.0f Hz", g->ui_dir, g->fb_w, g->fb_h, g->display_hz);

    /* document */
    size_t html_len = 0;
    char *html = read_ui_file(g, g->page, &html_len);
    if (!html) goto fail;
    g->dom = dom_create();
    if (!g->dom || dom_load_html(g->dom, html, html_len) != 0) {
        free(html);
        fprintf(stderr, "[html_engine] %s: parse failed\n", g->page);
        goto fail;
    }
    free(html);
    vlog("%s: %u elements", g->page, dom_elem_count(g->dom));

    /* stylesheets (UA first, then the author sheets in document order) */
    g->ua = css_sheet_load(KN_UA_CSS, sizeof(KN_UA_CSS) - 1, "ua.css");
    load_author_sheets(g);

    /* text, images, animation */
    g->text = text_create(g->ui_dir);
    if (g->text && g_doc_lang[0]) text_set_lang(g->text, g_doc_lang); /* engine_set_lang before this page */
    g->images = image_cache_create(g->ui_dir);
    g->videos = video_players_create();
    if (!g->text || !g->images) {
        fprintf(stderr, "[html_engine] text/image system creation failed\n");
        goto fail;
    }
    anim_init(&g->anim, anim_dirty_cb, g);
    if (g->ua) register_keyframes(g, g->ua);
    for (int i = 0; i < g->n_sheets; i++) {
        register_font_faces(g, g->sheets[i]);
        register_keyframes(g, g->sheets[i]);
    }

    /* events + widgets */
    g->ev_link.dom = g->dom;
    g->ev_link.tree = &g->tree;
    events_host_layout(&g->ev_base, &g->ev_link);
    EventsHost host = g->ev_base;
    host.user = g;
    host.hit_test = eh_hit_test;
    host.box_elem = eh_box_elem;
    host.elem_box = eh_elem_box;
    host.elem_rect = eh_elem_rect;
    host.scroll_info = eh_scroll_info;
    host.set_scroll = eh_set_scroll;
    host.scrollbar = eh_scrollbar;
    host.restyle = eh_restyle;
    events_init(&g->events, g->dom, &host);

    WidgetsHost wh;
    memset(&wh, 0, sizeof(wh));
    wh.user = g;
    wh.computed = wh_computed;
    wh.pseudo_computed = wh_pseudo_computed;
    wh.elem_geom = wh_elem_geom;
    wh.invalidate = wh_invalidate;
    wh.fire = wh_fire;
    wh.set_capture = wh_set_capture;
    wh.release_capture = wh_release_capture;
    wh.focus = wh_focus;
    wh.blur = wh_blur;
    wh.clipboard_get = wh_clipboard_get;
    wh.clipboard_set = wh_clipboard_set;
    wh.elem_local = wh_elem_local;
    wh.font_char_widths = wh_font_char_widths;
    g->widgets = widgets_create(g->dom, g->text, &wh);
    if (g->widgets) {
        EventsWidgetHooks hooks;
        widgets_fill_event_hooks(g->widgets, &hooks);
        events_set_widget_hooks(&g->events, &hooks);
        widgets_set_gl_ready(g->widgets, 1);
        widgets_set_viewport(g->widgets, g->vw, g->vh);
    }

    /* layout + paint */
    layout_ctx_init(&g->lctx);
    g->lctx.text = g->text;
    g->lctx.user = g;
    g->lctx.image_size = lh_image_size;
    g->lctx.video_size = lh_video_size;
    g->lctx.scrollbar_width = lh_scrollbar_width;
    g->lctx.widget_size = lh_widget_size;
    g->lctx.base_url = g->page;
    g->lctx.relaxed_boundaries = 1;

    paint_ctx_init(&g->paint);
    g->paint.device_scale = 1.0f;
    g->paint.hide_scrollbars = g_hide_scrollbars;
    g->paint.viewport_w = g->vw;
    g->paint.viewport_h = g->vh;
    g->paint.text = g->text;
    g->paint.images = g->images;
    /* every author sheet lives in css/, so one base resolves their url()s */
    g->paint.css_base_url = "css/main.css";
    g->paint.doc_base_url = g->page;
    g->paint.host.user = g;
    g->paint.host.image_src = paint_image_src_hook;
    g->paint.host.canvas_image = paint_canvas_image_hook;
    g->paint.host.scrollbar_style = paint_scrollbar_style_hook;
    g->paint.host.widget_paint = paint_widget_hook;
    g->paint.host.selection_range = paint_selection_hook;
    sel_init(&g->sel);
    kn_arena_init(&g->overlay_arena, 16 * 1024);

    /* GL */
    g->glb = glb_create();
    if (!g->glb) {
        fprintf(stderr, "[html_engine] GL backend creation failed (assets/shaders/html_ui.*)\n");
        goto fail;
    }
    uint8_t lut[8][256];
    text_gamma_lut(lut);
    glb_set_text_gamma(g->glb, lut);
    glb_set_image_resolver(g->glb, glb_image_resolver, NULL);
    glb_set_lazy_image_resolver(g->glb, glb_lazy_image_resolver, g);

    /* first style + layout (scripts are ignored: no JS natively) */
    g->need_rebuild = 1;
    engine_flush_layout(g);
    vlog("first layout: %u elements styled", g->stats.restyled_elems);

    g->prof_window_start = now_s();
    return g;

fail:
    engine_destroy(g);
    g_engine = NULL;
    return NULL;
}

int html_ui_backend_init(const char *ui_dir, const int width, const int height)
{
    if (g_base || g_page) html_ui_backend_fini();
    g_base = engine_create(ui_dir, "index.html", width, height);
    if (!g_base) return 0;
    g_engine = g_base;
    html_ui_page_ready();
    return 1;
}

void html_ui_backend_fini(void)
{
    if (g_page) engine_destroy(g_page);
    if (g_base) engine_destroy(g_base);
    g_page = NULL;
    g_base = NULL;
    g_engine = NULL;
    css_property_registry_clear();   /* @property rules of the pages' sheets */
}

static void engine_apply_size(Engine *g, int width, int height);
static void fire_resize_listener(Engine *g);

/* Shows `page`: the base page again (its document as it was left) or a fresh
 * document for any other page; a previously shown secondary page is destroyed. */
/* A <link rel=stylesheet href> added to the current document at runtime: appended after
 * its author sheets (it wins ties), @font-face/@keyframes registered, everything
 * restyled. The game page (g_base) keeps it across page switches. */
int html_ui_backend_add_stylesheet(const char *href)
{
    Engine *g = g_engine;
    if (!g || !g->dom || !href || !*href) return 0;
    char key[512];
    if (!image_resolve_url(href, g->page, key, sizeof(key))) {
        fprintf(stderr, "[html_engine] stylesheet href '%s' rejected\n", href);
        return 0;
    }
    for (int i = 0; i < g->n_sheets; i++) {
        const char *name = css_sheet_name(g->sheets[i]);
        if (name && !strcmp(name, key)) return 1;
    }
    if (g->n_sheets >= HTML_ENGINE_MAX_SHEETS) return 0;
    size_t len = 0;
    char *text = read_ui_file(g, key, &len);
    if (!text) return 0;
    CssSheet *sheet = load_sheet_text(g, text, len, key);
    free(text);
    if (!sheet) return 0;
    g->sheets[g->n_sheets++] = sheet;
    register_font_faces(g, sheet);
    register_keyframes(g, sheet);
    g->lctx.memo_epoch++;
    dom_mark_all_dirty(g->dom);
    return 1;
}

/* The Client tab's custom CSS (html_ui_set_custom_css): one sheet named
 * KN_CUSTOM_SHEET on the game page, kept last among its author sheets; replaced in
 * place, removed for NULL/"". Named under css/ so its url()s resolve like main.css's. */
#define KN_CUSTOM_SHEET "css/kn-custom.css"
int html_ui_backend_set_custom_css(const char *css)
{
    Engine *g = g_base ? g_base : g_engine;
    if (!g || !g->dom) return 0;
    for (int i = 0; i < g->n_sheets; i++) {
        const char *name = css_sheet_name(g->sheets[i]);
        if (!name || strcmp(name, KN_CUSTOM_SHEET)) continue;
        css_sheet_destroy(g->sheets[i]);
        memmove(&g->sheets[i], &g->sheets[i + 1], (size_t) (g->n_sheets - i - 1) * sizeof(g->sheets[0]));
        g->n_sheets--;
        break;
    }
    if (css && *css) {
        if (g->n_sheets >= HTML_ENGINE_MAX_SHEETS) return 0;
        CssSheet *sheet = load_sheet_text(g, css, strlen(css), KN_CUSTOM_SHEET);
        if (!sheet) return 0;
        g->sheets[g->n_sheets++] = sheet;
        register_font_faces(g, sheet);
        register_keyframes(g, sheet);
    }
    g->lctx.memo_epoch++;
    dom_mark_all_dirty(g->dom);
    g->need_layout = 1;
    g->need_paint = 1;
    return 1;
}

int html_ui_backend_load_page(const char *page)
{
    if (!g_base || !page || !*page) return 0;
    const int fb_w = g_engine ? g_engine->fb_w : g_base->fb_w;
    const int fb_h = g_engine ? g_engine->fb_h : g_base->fb_h;
    if (g_page) {
        engine_destroy(g_page);
        g_page = NULL;
    }
    if (!strcmp(page, g_base->page)) {
        g_engine = g_base;
        engine_apply_size(g_base, fb_w, fb_h);
        paint_invalidate_all(&g_base->paint);
        g_base->need_paint = 1;
        if (g_base->sent_initial_resize) fire_resize_listener(g_base);
        html_ui_page_ready();
        return 1;
    }
    Engine *g = engine_create(g_base->ui_dir, page, fb_w, fb_h);
    if (!g) {
        fprintf(stderr, "[html_engine] page %s failed to load, staying on %s\n", page, g_base->page);
        g_engine = g_base;
        return 0;
    }
    g_page = g;
    g_engine = g;
    html_ui_page_ready();
    return 1;
}

/* @media: the viewport in CSS px (1 CSS px per framebuffer px, like vw/vh).
 * Returns 1 when a sheet's @media result flipped (its rule index changed). */
static int engine_media_update(Engine *g)
{
    CssMediaEnv env = *css_media_env();
    int changed = 0;
    env.width = g->vw;
    env.height = g->vh;
    css_media_set_env(&env);
    if (g->ua) changed |= css_sheet_media_update(g->ua);
    for (int i = 0; i < g->n_sheets; i++) changed |= css_sheet_media_update(g->sheets[i]);
    if (changed) vlog("@media: viewport %.0fx%.0f changed the matching rules", g->vw, g->vh);
    return changed;
}

static void fire_resize_listener(Engine *g)
{
    HtmlEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = "resize";
    ev.value = "";
    ev.x = g->vw;
    ev.y = g->vh;
    html_ui_dispatch_listener(NULL, &ev);
}

/* Framebuffer size change; 0 when the size is unchanged. */
static int engine_set_size(Engine *g, const int width, const int height)
{
    if (!g || width <= 0 || height <= 0) return 0;
    if (width == g->fb_w && height == g->fb_h) return 0;
    g->fb_w = width;
    g->fb_h = height;
    g->vw = (float) width;
    g->vh = (float) height;
    /* vw/vh/vmin/vmax compute to px: restyle everything when a sheet used them */
    css_values_set_viewport(g->vw, g->vh);
    const int media_changed = engine_media_update(g);
    if ((media_changed || css_values_viewport_units_used()) && g->dom) { g->lctx.memo_epoch++; dom_mark_all_dirty(g->dom); }
    g->paint.viewport_w = g->vw;
    g->paint.viewport_h = g->vh;
    if (g->widgets) widgets_set_viewport(g->widgets, g->vw, g->vh);
    g->need_layout = 1;
    paint_invalidate_all(&g->paint);
    g->need_paint = 1;
    vlog("resize %dx%d", width, height);
    return 1;
}

static void engine_apply_size(Engine *g, const int width, const int height)
{
    engine_set_size(g, width, height);
}

void html_ui_backend_resize(const int width, const int height)
{
    Engine *g = g_engine;
    if (engine_set_size(g, width, height) && g->sent_initial_resize) fire_resize_listener(g);
}

void html_ui_backend_update(void)
{
    Engine *g = g_engine;
    if (!g) return;
    const double now = now_s();

    if (g->profile) {
        const double wall = wall_ms();
        if (g->prof_frame_start > 0.0) prof_add(&g->st_frame, wall - g->prof_frame_start);
        g->prof_frame_start = wall;
        g->prof_frames++;
        prof_report(g, now);
    }

    if (!g->sent_initial_resize) {
        g->sent_initial_resize = 1;
        fire_resize_listener(g);
    }

    if (!pace_due(&g->next_update, 1.0 / HTML_ENGINE_UPDATE_HZ, now)) return;

    html_timers_tick(engine_now_ms());
    if (pace_due(&g->next_anim, 1.0 / g->display_hz, now)) {
        g->anim_scroll_saved = 0;
        anim_tick(&g->anim, engine_now_ms());
        engine_video_tick(g);
    }
    if (g->observe_pending) run_after_layout(g);
}

void engine_set_top_layer(Engine *g, HtmlElem e)
{
    if (!g || g->top_layer == e) return;
    g->top_layer = e;
    engine_mark_paint(g, 0);
}

void html_ui_backend_render(UI *ui, const int layers)
{
    (void) ui;
    Engine *g = g_engine;
    if (!g || !g->glb) return;
    if (!(layers & HTML_LAYER_PAGE)) { /* the top layer of the list the page pass built this frame */
        if (!g->top_dl.n && !g->overlay.n) return;
        const GLboolean depth_on = glIsEnabled(GL_DEPTH_TEST);
        glb_begin(g->glb, g->fb_w, g->fb_h, g->paint.device_scale);
        if (g->top_dl.n) glb_submit(g->glb, &g->top_dl);
        if (g->overlay.n) glb_submit(g->glb, &g->overlay);
        glb_end(g->glb);
        if (depth_on) glEnable(GL_DEPTH_TEST);
        glViewport(0, 0, g->fb_w, g->fb_h);
        return;
    }

    /* game content fetched in the background (asset_fetch.h): page images it brought pop in */
    if (image_cache_retry_missing(g->images) > 0) {
        g->lctx.memo_epoch++;
        dom_mark_all_dirty(g->dom);
        g->need_layout = 1;
        g->need_paint = 1;
    }

    /* restyle -> layout -> observers/hover -> paint (only when dirty) */
    engine_flush_layout(g);
    run_after_layout(g);
    engine_flush_layout(g); /* observers may have mutated the page */
    sel_autoscroll(g);

    const int overlay_open = g->widgets && widgets_popup_owner(g->widgets);
    const double widget_wake = g->widgets ? widgets_next_repaint_ms(g->widgets, engine_now_ms()) : -1.0;
    if (widget_wake >= 0.0 && widget_wake <= engine_now_ms()) engine_mark_paint(g, events_focused(&g->events));

    if (g->need_paint && g->tree) {
        const double t0 = wall_ms();
        sel_before_paint(g);
        g->paint.body = engine_elem_box(g, dom_body(g->dom)); /* boxes are recycled on rebuilds */
        g->paint.top_layer = g->top_layer ? engine_elem_box(g, g->top_layer) : NULL;
        g->paint.top_out = &g->top_dl;
        /* the overlay's glyphs can rebuild a full atlas under g->dl (see paint_build):
         * then paint both again against the new atlas */
        for (int pass = 0; pass < 2; pass++) {
            paint_build(&g->paint, layout_tree_root(g->tree), &g->dl);
            const uint32_t atlas_gen = g->text ? text_atlas_generation(g->text) : 0;
            dl_clear(&g->overlay);
            kn_arena_reset(&g->overlay_arena);
            if (overlay_open) {
                WidgetPaintArgs args;
                args.out = &g->overlay;
                args.alloc = overlay_alloc_cb;
                args.alloc_user = &g->overlay_arena;
                args.device_scale = g->paint.device_scale;
                args.now_ms = engine_now_ms();
                widgets_paint_overlay(g->widgets, &args);
            }
            if (!g->text || text_atlas_generation(g->text) == atlas_gen) break;
        }
        g->need_paint = 0;
        if (g->dump_dl) dump_display_list(&g->dl);
        const double ms = wall_ms() - t0;
        g->stats.paint_ms += ms;
        g->stats.paints++;
        if (g->profile) prof_add(&g->st_paint, ms);
    }

    /* submit every frame: the scene under the page changes */
    const double t1 = wall_ms();
    const GLboolean depth_was_on = glIsEnabled(GL_DEPTH_TEST);
    g->decode_ms = 0.0;
    g->decodes = g->decodes_deferred = 0;
    glb_begin(g->glb, g->fb_w, g->fb_h, g->paint.device_scale);
    if (g->dl.n) glb_submit(g->glb, &g->dl);
    if (layers & HTML_LAYER_TOP) { /* else html_ui_render_top() draws them after the client's GL */
        if (g->top_dl.n) glb_submit(g->glb, &g->top_dl);
        if (g->overlay.n) glb_submit(g->glb, &g->overlay);
    }
    glb_end(g->glb);
    image_cache_frame(g->images);
    if (g->profile && g->decodes) prof_add(&g->st_decode, g->decode_ms);
    /* glb_end(): blend on with GL_SRC_ALPHA/GL_ONE_MINUS_SRC_ALPHA, depth off,
     * caches updated.  Put back what the client had. */
    if (depth_was_on) glEnable(GL_DEPTH_TEST);
    glViewport(0, 0, g->fb_w, g->fb_h);
    const double ms = wall_ms() - t1;
    g->stats.submit_ms += ms;
    g->stats.frames++;
    if (g->profile) prof_add(&g->st_submit, ms);

    update_cursor(g);
}

void html_ui_backend_mouse_move(const double x, const double y)
{
    Engine *g = g_engine;
    if (!g) return;
    g->mouse_x = (float) x;
    g->mouse_y = (float) y;
    events_mouse_move(&g->events, g->mouse_x, g->mouse_y);
    if (g->widgets) widgets_pointer_moved(g->widgets, g->mouse_x, g->mouse_y);
    sel_drag_to(g, g->mouse_x, g->mouse_y);
}

void html_ui_backend_mouse_button(const int glfw_button, const int glfw_action, const int mods)
{
    Engine *g = g_engine;
    if (!g || glfw_button < 0 || glfw_button > GLFW_MOUSE_BUTTON_LAST) return;
    if (glfw_action != GLFW_PRESS && glfw_action != GLFW_RELEASE) return;
    events_mouse_button(&g->events, glfw_button, glfw_action, mods);
    if (glfw_button != GLFW_MOUSE_BUTTON_LEFT) return;
    if (glfw_action == GLFW_PRESS) sel_press(g, mods);
    else g->sel.dragging = 0;
}

void html_ui_backend_scroll(const double dx, const double dy)
{
    Engine *g = g_engine;
    if (!g) return;
    /* An open <select> popup takes the wheel (and closes on a wheel elsewhere). */
    if (g->widgets && widgets_scroll(g->widgets, (float) dx, (float) dy, g->mouse_x, g->mouse_y)) return;
    /* events.c converts wheel clicks to px (EV_SCROLL_PIXELS_PER_CLICK = 53). */
    events_scroll(&g->events, (float) dx, (float) dy, g->mouse_x, g->mouse_y);
}

void html_ui_backend_key(const int glfw_key, const int scancode, const int glfw_action, const int mods)
{
    Engine *g = g_engine;
    if (!g) return;
    if (sel_key(g, glfw_key, glfw_action, mods)) {
        g->key_consumed = 1;
        return;
    }
    g->key_consumed = events_key(&g->events, glfw_key, scancode, glfw_action, mods);
}

void html_ui_backend_char(const unsigned int codepoint)
{
    Engine *g = g_engine;
    if (!g) return;
    events_char(&g->events, codepoint);
}

int html_ui_backend_available(void)
{
    return 1;
}

#else
typedef int html_engine_backend_not_compiled;
#endif

