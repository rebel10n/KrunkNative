/*
 * dom_ops.c - native implementation of the DOM-ops API (client/include/html_dom.h)
 * on the in-house engine.  Handlers, listeners, timers, storage and key helpers
 * are platform-independent and live in client/src/html/html_dom_core.c; the
 * web build implements the same functions in client/src/html/web_dom.c.
 *
 * Every function is a no-op (returns 0 / -1) before html_ui_init() succeeded.
 * Mutations go through dom.c (which marks style dirty) and tell the engine what
 * else is stale (engine.h): node destruction drops the layout tree first,
 * structural changes schedule a box-tree rebuild, widget state changes repaint.
 * Geometry reads flush style + layout like a browser does on
 * getBoundingClientRect / offsetWidth.
 */
#if defined(KRUNKNATIVE_HTML_ENGINE) && !defined(__EMSCRIPTEN__)

#include <html_dom.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine.h"
#include "dom.h"
#include "events.h"
#include "layout.h"
#include "widgets.h"

/* ------------------------------------------------------------------------- */
/* Detached text nodes (html_dom_create_text)                                 */
/* ------------------------------------------------------------------------- */
/* dom.h only hands out element handles, so a text node created before it is
 * appended lives here under a handle from a reserved range and becomes a real
 * text node (dom_append_text) when it is appended.  After that the handle is
 * dead, like most uses of document.createTextNode(). */

#define TEXT_HANDLE_BIT 0x80000000u
#define TEXT_HANDLE_MAX 256

static char *g_text_nodes[TEXT_HANDLE_MAX];

static int is_text_handle(const HtmlElem e)
{
    return (e & TEXT_HANDLE_BIT) != 0;
}

static char **text_slot(const HtmlElem e)
{
    const uint32_t i = e & ~TEXT_HANDLE_BIT;
    return is_text_handle(e) && i < TEXT_HANDLE_MAX && g_text_nodes[i] ? &g_text_nodes[i] : NULL;
}

static void text_free(const HtmlElem e)
{
    char **slot = text_slot(e);
    if (!slot) return;
    free(*slot);
    *slot = NULL;
}

/* ------------------------------------------------------------------------- */
/* Helpers                                                                    */
/* ------------------------------------------------------------------------- */

static Dom *dom_or_null(void)
{
    return engine_dom(engine_get());
}

static int alive(Dom *dom, const HtmlElem e)
{
    return dom && e && !is_text_handle(e) && dom_elem_data(dom, e) != NULL;
}

/* Copies s into buf (truncated, NUL-terminated); returns strlen(s). */
static int copy_out(const char *s, char *buf, const size_t n)
{
    if (!s) s = "";
    const size_t len = strlen(s);
    if (buf && n) {
        const size_t k = len < n - 1 ? len : n - 1;
        memcpy(buf, s, k);
        buf[k] = 0;
    }
    return (int) len;
}

/* Attributes whose value affects boxes without going through the cascade. */
static int attr_affects_boxes(const char *name)
{
    static const char *names[] = {"src", "type", "width", "height", "value", "placeholder", "size", "colspan", "rowspan", "maxlength", "min", "max", "step", NULL};
    for (int i = 0; names[i]; i++) {
        if (!strcmp(names[i], name)) return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Lookup / tree                                                              */
/* ------------------------------------------------------------------------- */

HtmlElem html_dom_root(void)
{
    Dom *dom = dom_or_null();
    return dom ? dom_root(dom) : 0;
}

HtmlElem html_dom_body(void)
{
    Dom *dom = dom_or_null();
    return dom ? dom_body(dom) : 0;
}

HtmlElem html_dom_by_id(const char *id)
{
    Dom *dom = dom_or_null();
    return dom && id && *id ? dom_by_id(dom, id) : 0;
}

HtmlElem html_dom_query(const HtmlElem root, const char *selector)
{
    Dom *dom = dom_or_null();
    if (!dom || !selector || (root && !alive(dom, root))) return 0;
    return dom_query(dom, root, selector);
}

int html_dom_query_all(const HtmlElem root, const char *selector, HtmlElem *out, const int max)
{
    Dom *dom = dom_or_null();
    if (!dom || !selector || max < 0 || (root && !alive(dom, root))) return 0;
    return (int) dom_query_all(dom, root, selector, out, out ? (size_t) max : 0);
}

HtmlElem html_dom_parent(const HtmlElem e)
{
    Dom *dom = dom_or_null();
    return alive(dom, e) ? dom_parent(dom, e) : 0;
}

HtmlElem html_dom_first_child(const HtmlElem e)
{
    Dom *dom = dom_or_null();
    return alive(dom, e) ? dom_first_child(dom, e) : 0;
}

HtmlElem html_dom_next(const HtmlElem e)
{
    Dom *dom = dom_or_null();
    return alive(dom, e) ? dom_next(dom, e) : 0;
}

HtmlElem html_dom_closest(const HtmlElem e, const char *selector)
{
    Dom *dom = dom_or_null();
    return alive(dom, e) && selector ? dom_closest(dom, e, selector) : 0;
}

int html_dom_child_count(const HtmlElem e)
{
    Dom *dom = dom_or_null();
    return alive(dom, e) ? (int) dom_child_count(dom, e) : 0;
}

int html_dom_tag(const HtmlElem e, char *buf, const size_t n)
{
    Dom *dom = dom_or_null();
    if (!alive(dom, e)) {
        if (buf && n) buf[0] = 0;
        return 0;
    }
    return copy_out(dom_tag(dom, e), buf, n);
}

/* ------------------------------------------------------------------------- */
/* Mutation                                                                   */
/* ------------------------------------------------------------------------- */

HtmlElem html_dom_create(const char *tag, const char *id_or_null)
{
    Dom *dom = dom_or_null();
    if (!dom || !tag || !*tag) return 0;
    const HtmlElem e = dom_create_element(dom, tag);
    if (e && id_or_null && *id_or_null) dom_set_attr(dom, e, "id", id_or_null);
    return e;
}

HtmlElem html_dom_create_text(const char *text)
{
    if (!dom_or_null()) return 0;
    for (uint32_t i = 0; i < TEXT_HANDLE_MAX; i++) {
        if (g_text_nodes[i]) continue;
        const size_t len = text ? strlen(text) : 0;
        g_text_nodes[i] = malloc(len + 1);
        if (!g_text_nodes[i]) return 0;
        if (len) memcpy(g_text_nodes[i], text, len);
        g_text_nodes[i][len] = 0;
        return TEXT_HANDLE_BIT | i;
    }
    return 0;
}

void html_dom_append(const HtmlElem parent, const HtmlElem child)
{
    Engine *g = engine_get();
    Dom *dom = engine_dom(g);
    if (!alive(dom, parent)) return;
    char **text = text_slot(child);
    if (text) {
        dom_append_text(dom, parent, *text, strlen(*text));
        text_free(child);
    } else if (alive(dom, child)) {
        engine_before_move(g, child);
        dom_append(dom, parent, child);
    } else {
        return;
    }
    engine_children_changed(g, parent);
}

void html_dom_insert_html(const HtmlElem ref, const HtmlInsertPos pos, const char *html)
{
    Engine *g = engine_get();
    Dom *dom = engine_dom(g);
    if (!alive(dom, ref) || !html) return;
    DomInsertWhere where;
    switch (pos) {
        case HTML_INSERT_BEFORE_BEGIN: where = DOM_INSERT_BEFORE_BEGIN; break;
        case HTML_INSERT_AFTER_BEGIN: where = DOM_INSERT_AFTER_BEGIN; break;
        case HTML_INSERT_AFTER_END: where = DOM_INSERT_AFTER_END; break;
        case HTML_INSERT_BEFORE_END:
        default: where = DOM_INSERT_BEFORE_END; break;
    }
    const HtmlElem owner = (where == DOM_INSERT_BEFORE_BEGIN || where == DOM_INSERT_AFTER_END) ? dom_parent(dom, ref) : ref;
    dom_insert_html(dom, ref, where, html, strlen(html));
    engine_children_changed(g, owner ? owner : ref);
}

void html_dom_remove(const HtmlElem e)
{
    Engine *g = engine_get();
    Dom *dom = engine_dom(g);
    if (is_text_handle(e)) {
        text_free(e);
        return;
    }
    if (!alive(dom, e)) return;
    engine_before_remove(g, e, 1);
    dom_remove(dom, e);
}

void html_dom_set_html(const HtmlElem e, const char *html)
{
    Engine *g = engine_get();
    Dom *dom = engine_dom(g);
    if (!alive(dom, e)) return;
    if (!html) html = "";
    engine_before_remove(g, e, 0);
    dom_set_html(dom, e, html, strlen(html));
}

void html_dom_set_text(const HtmlElem e, const char *text)
{
    Engine *g = engine_get();
    Dom *dom = engine_dom(g);
    char **slot = text_slot(e);
    if (slot) {
        char *copy = malloc((text ? strlen(text) : 0) + 1);
        if (!copy) return;
        strcpy(copy, text ? text : "");
        free(*slot);
        *slot = copy;
        return;
    }
    if (!alive(dom, e)) return;
    if (!text) text = "";
    engine_before_remove(g, e, 0);
    dom_set_text(dom, e, text, strlen(text));
}

int html_dom_get_text(const HtmlElem e, char *buf, const size_t n)
{
    Dom *dom = dom_or_null();
    char **slot = text_slot(e);
    if (slot) return copy_out(*slot, buf, n);
    if (!alive(dom, e)) return copy_out("", buf, n);
    char *text = dom_get_text(dom, e);
    const int len = copy_out(text, buf, n);
    free(text);
    return len;
}

static void map_text_changed(void *user, const HtmlElem parent)
{
    engine_children_changed((Engine *) user, parent);
}

int html_dom_map_text(const HtmlElem root, const char *skip_selector, HtmlTextMapFn fn, void *user)
{
    Engine *g = engine_get();
    Dom *dom = engine_dom(g);
    if (!dom || !fn || (root && !alive(dom, root))) return 0;
    return (int) dom_map_text(dom, root, skip_selector, fn, user, map_text_changed, g);
}

void html_dom_set_lang(const char *lang)
{
    engine_set_lang(lang);
}

/* Text runs of the boxes under `b`: glyphs with index 0 whose character is not white space. */
static int missing_in_box(const Box *b, uint32_t *first)
{
    int n = 0;
    for (; b; b = b->next) {
        for (int r = 0; r < b->run_count; r++) {
            const TextRun *run = &b->runs[r];
            for (int i = 0; i < run->glyph_count; i++) {
                if (run->glyphs[i].glyph_index) continue;
                const size_t at = run->glyphs[i].cluster;
                if (at >= run->len) continue;
                const unsigned char c = (unsigned char) run->text[at];
                if (c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;
                uint32_t cp = c;
                if (c >= 0xF0 && at + 3 < run->len) cp = ((c & 7u) << 18) | (((unsigned char) run->text[at + 1] & 63u) << 12) | (((unsigned char) run->text[at + 2] & 63u) << 6) | ((unsigned char) run->text[at + 3] & 63u);
                else if (c >= 0xE0 && at + 2 < run->len) cp = ((c & 15u) << 12) | (((unsigned char) run->text[at + 1] & 63u) << 6) | ((unsigned char) run->text[at + 2] & 63u);
                else if (c >= 0xC0 && at + 1 < run->len) cp = ((c & 31u) << 6) | ((unsigned char) run->text[at + 1] & 63u);
                if (cp == 0xA0 || cp == 0x3000) continue;
                if (!n && first) *first = cp;
                n++;
            }
        }
        n += missing_in_box(b->first_child, n ? NULL : first);
    }
    return n;
}

int html_dom_missing_glyphs(const HtmlElem e, uint32_t *first_cp)
{
    Engine *g = engine_get();
    Dom *dom = engine_dom(g);
    if (first_cp) *first_cp = 0;
    if (!alive(dom, e)) return 0;
    engine_flush_layout(g);
    const Box *box = engine_elem_box(g, e);
    if (!box) return 0;
    return missing_in_box(box->first_child, first_cp); /* an element's box carries no runs itself */
}

/* ------------------------------------------------------------------------- */
/* Attributes / classes / style                                               */
/* ------------------------------------------------------------------------- */

void html_dom_set_attr(const HtmlElem e, const char *name, const char *value)
{
    Engine *g = engine_get();
    Dom *dom = engine_dom(g);
    if (!alive(dom, e) || !name || !*name) return;
    dom_set_attr(dom, e, name, value ? value : "");
    if (attr_affects_boxes(name)) engine_children_changed(g, dom_parent(dom, e) ? dom_parent(dom, e) : e);
}

void html_dom_canvas_put(const HtmlElem e, const unsigned char *rgba, const int w, const int h)
{
    Engine *g = engine_get();
    Dom *dom = engine_dom(g);
    if (!alive(dom, e) || !rgba || w <= 0 || h <= 0) return;
    /* canvas.width / height = the bitmap size: the canvas's natural size (layout) */
    char want[16], have[16];
    snprintf(want, sizeof want, "%d", w);
    const char *v = dom_get_attr(dom, e, "width", NULL);
    snprintf(have, sizeof have, "%s", v ? v : "");
    if (strcmp(want, have)) html_dom_set_attr(e, "width", want);
    snprintf(want, sizeof want, "%d", h);
    v = dom_get_attr(dom, e, "height", NULL);
    snprintf(have, sizeof have, "%s", v ? v : "");
    if (strcmp(want, have)) html_dom_set_attr(e, "height", want);
    engine_canvas_put(g, e, rgba, w, h);
}

/* html_dom_canvas_gl: canvas.width / height follow the render target like a WebGL canvas's
 * drawing buffer; the box then samples the client's texture itself (engine_canvas_gl). */
void html_dom_canvas_gl(const HtmlElem e, const unsigned tex, const unsigned fbo, const int w, const int h)
{
    Engine *g = engine_get();
    Dom *dom = engine_dom(g);
    (void) fbo; /* the web backend reads it back; natively the texture is drawn directly */
    if (!alive(dom, e)) return;
    if (tex && w > 0 && h > 0) {
        char want[16];
        const char *v = dom_get_attr(dom, e, "width", NULL);
        snprintf(want, sizeof want, "%d", w);
        if (!v || strcmp(want, v)) html_dom_set_attr(e, "width", want);
        v = dom_get_attr(dom, e, "height", NULL);
        snprintf(want, sizeof want, "%d", h);
        if (!v || strcmp(want, v)) html_dom_set_attr(e, "height", want);
    }
    engine_canvas_gl(g, e, tex, w, h);
}

void html_dom_remove_attr(const HtmlElem e, const char *name)
{
    Engine *g = engine_get();
    Dom *dom = engine_dom(g);
    if (!alive(dom, e) || !name || !*name) return;
    dom_remove_attr(dom, e, name);
    if (attr_affects_boxes(name)) engine_children_changed(g, dom_parent(dom, e) ? dom_parent(dom, e) : e);
}

int html_dom_get_attr(const HtmlElem e, const char *name, char *buf, const size_t n)
{
    Dom *dom = dom_or_null();
    const char *v = alive(dom, e) && name ? dom_get_attr(dom, e, name, NULL) : NULL;
    if (!v) {
        if (buf && n) buf[0] = 0;
        return -1;
    }
    return copy_out(v, buf, n);
}

void html_dom_set_class(const HtmlElem e, const char *cls, const int on)
{
    Dom *dom = dom_or_null();
    if (!alive(dom, e) || !cls || !*cls) return;
    dom_toggle_class(dom, e, cls, on ? 1 : 0);
}

int html_dom_has_class(const HtmlElem e, const char *cls)
{
    Dom *dom = dom_or_null();
    return alive(dom, e) && cls && *cls ? dom_has_class(dom, e, cls) : 0;
}

void html_dom_set_display(const HtmlElem e, const char *value)
{
    html_dom_set_style(e, "display", value);
}

void html_dom_set_style(const HtmlElem e, const char *prop, const char *value)
{
    Dom *dom = dom_or_null();
    if (!alive(dom, e) || !prop || !*prop) return;
    dom_set_style(dom, e, prop, value && *value ? value : NULL);
}

int html_dom_get_style(const HtmlElem e, const char *prop, char *buf, const size_t n)
{
    Dom *dom = dom_or_null();
    const char *v = alive(dom, e) && prop ? dom_get_style(dom, e, prop) : NULL;
    if (!v) {
        if (buf && n) buf[0] = 0;
        return -1;
    }
    return copy_out(v, buf, n);
}

/* ------------------------------------------------------------------------- */
/* Form state                                                                 */
/* ------------------------------------------------------------------------- */

void html_dom_set_value(const HtmlElem e, const char *value)
{
    Engine *g = engine_get();
    Dom *dom = engine_dom(g);
    if (!alive(dom, e)) return;
    Widgets *w = engine_widgets(g);
    if (w && widgets_kind(w, e) != WIDGET_NONE) widgets_set_value(w, e, value ? value : "");
    else dom_set_value(dom, e, value ? value : "");
    engine_mark_paint(g, e);
}

int html_dom_get_value(const HtmlElem e, char *buf, const size_t n)
{
    Engine *g = engine_get();
    Dom *dom = engine_dom(g);
    if (!alive(dom, e)) return copy_out("", buf, n);
    Widgets *w = engine_widgets(g);
    if (w && widgets_kind(w, e) != WIDGET_NONE) return copy_out(widgets_get_value(w, e), buf, n);
    return copy_out(dom_get_value(dom, e), buf, n);
}

void html_dom_set_checked(const HtmlElem e, const int checked)
{
    Engine *g = engine_get();
    Dom *dom = engine_dom(g);
    if (!alive(dom, e)) return;
    Widgets *w = engine_widgets(g);
    if (w) widgets_set_checked(w, e, checked ? 1 : 0);
    else dom_set_checked(dom, e, checked ? 1 : 0);
    engine_mark_paint(g, e);
}

int html_dom_get_checked(const HtmlElem e)
{
    Engine *g = engine_get();
    Dom *dom = engine_dom(g);
    if (!alive(dom, e)) return 0;
    Widgets *w = engine_widgets(g);
    return w ? widgets_get_checked(w, e) : dom_get_checked(dom, e);
}

void html_dom_focus(const HtmlElem e)
{
    Engine *g = engine_get();
    if (!alive(engine_dom(g), e)) return;
    events_focus(engine_events(g), e);
}

void html_dom_blur(const HtmlElem e)
{
    Engine *g = engine_get();
    EventSystem *es = engine_events(g);
    if (!es || !e) return;
    if (events_focused(es) == e) events_blur(es);
}

void html_dom_scroll_to_bottom(const HtmlElem e)
{
    Engine *g = engine_get();
    if (!alive(engine_dom(g), e)) return;
    LayoutTree *tree = engine_flush_layout(g);
    Box *box = engine_elem_box(g, e);
    if (!tree || !box || !(box->flags & BOXF_SCROLL_CONTAINER)) return;
    /* scrollTop = scrollHeight: layout_set_scroll clamps to the maximum */
    layout_set_scroll(tree, box, box->scroll_x, box->scroll_h);
    engine_mark_paint(g, e);
}

int html_dom_get_scroll(const HtmlElem e, HtmlScroll *out)
{
    if (out) memset(out, 0, sizeof *out);
    Engine *g = engine_get();
    if (!alive(engine_dom(g), e)) return 0;
    engine_flush_layout(g);
    Box *box = engine_elem_box(g, e);
    if (!box || !(box->flags & BOXF_SCROLL_CONTAINER)) return 0;
    if (out) {
        out->left = box->scroll_x;
        out->top = box->scroll_y;
        out->width = box->scroll_w;
        out->height = box->scroll_h;
        /* clientWidth / clientHeight: the padding box minus the scrollbars */
        out->client_w = box->w - box->border[1] - box->border[3] - box->scrollbar_w;
        out->client_h = box->h - box->border[0] - box->border[2] - box->scrollbar_h;
    }
    return 1;
}

void html_dom_set_scroll(const HtmlElem e, const float left, const float top)
{
    Engine *g = engine_get();
    if (!alive(engine_dom(g), e)) return;
    LayoutTree *tree = engine_flush_layout(g);
    Box *box = engine_elem_box(g, e);
    if (!tree || !box || !(box->flags & BOXF_SCROLL_CONTAINER)) return;
    layout_set_scroll(tree, box, left, top); /* clamps to the scroll range */
    engine_mark_paint(g, e);
}

/* ------------------------------------------------------------------------- */
/* Text editing + code view (widgets.c)                                       */
/* ------------------------------------------------------------------------- */

static Widgets *live_widgets(const HtmlElem e)
{
    Engine *g = engine_get();
    return alive(engine_dom(g), e) ? engine_widgets(g) : NULL;
}

int html_dom_text_selection(const HtmlElem e, int *start, int *end)
{
    Widgets *w = live_widgets(e);
    if (start) *start = 0;
    if (end) *end = 0;
    return w ? widgets_selection(w, e, start, end) : 0;
}

void html_dom_text_set_selection(const HtmlElem e, const int start, const int end)
{
    Widgets *w = live_widgets(e);
    if (!w) return;
    widgets_set_selection(w, e, start, end);
    engine_mark_paint(engine_get(), e);
}

void html_dom_text_replace(const HtmlElem e, const int start, const int end, const char *text)
{
    Widgets *w = live_widgets(e);
    if (w && widgets_replace_range(w, e, start, end, text ? text : "")) engine_mark_paint(engine_get(), e);
}

int html_dom_text_caret_rect(const HtmlElem e, HtmlRect *out)
{
    Widgets *w = live_widgets(e);
    if (out) memset(out, 0, sizeof(*out));
    if (!w || !out) return 0;
    engine_flush_layout(engine_get());
    return widgets_caret_rect(w, e, &out->x, &out->y, &out->w, &out->h);
}

static CssColor code_color(const uint32_t rgba)
{
    CssColor c;
    memset(&c, 0, sizeof(c));
    c.r = (uint8_t) (rgba >> 24);
    c.g = (uint8_t) (rgba >> 16);
    c.b = (uint8_t) (rgba >> 8);
    c.a = (uint8_t) rgba;
    return c;
}

void html_dom_code_view(const HtmlElem e, const HtmlCodeView *cfg)
{
    Widgets *w = live_widgets(e);
    if (!w) return;
    if (!cfg) {
        widgets_set_code_view(w, e, NULL);
    } else {
        WidgetsCodeView v = {
            .tab_size = cfg->tab_size, .gutter = cfg->gutter,
            .gutter_fg = code_color(cfg->gutter_fg), .gutter_bg = code_color(cfg->gutter_bg),
            .gutter_pad_left = cfg->gutter_pad_left, .gutter_pad_right = cfg->gutter_pad_right,
            .gutter_min_w = cfg->gutter_min_w, .gutter_extra = cfg->gutter_extra, .gutter_gap = cfg->gutter_gap, .text_pad = cfg->text_pad,
            .active_line_bg = code_color(cfg->active_line_bg), .caret = code_color(cfg->caret),
            .selection_bg = code_color(cfg->selection_bg),
        };
        widgets_set_code_view(w, e, &v);
    }
    engine_mark_paint(engine_get(), e);
}

void html_dom_code_spans(const HtmlElem e, const HtmlCodeSpan *spans, const int n)
{
    Widgets *w = live_widgets(e);
    if (!w) return;
    WidgetsCodeSpan *tmp = n > 0 ? malloc((size_t) n * sizeof(*tmp)) : NULL;
    int m = 0;
    for (int i = 0; tmp && i < n; i++) {
        if (spans[i].end <= spans[i].start) continue;
        tmp[m].start = spans[i].start;
        tmp[m].end = spans[i].end;
        tmp[m].color = code_color(spans[i].color);
        tmp[m].flags = spans[i].flags & HTML_CODE_UNDERLINE ? WIDGETS_SPAN_UNDERLINE : 0;
        m++;
    }
    widgets_set_code_spans(w, e, tmp, m);
    free(tmp);
    engine_mark_paint(engine_get(), e);
}

void html_dom_code_lines(const HtmlElem e, const HtmlCodeLine *lines, const int n)
{
    Widgets *w = live_widgets(e);
    if (!w) return;
    WidgetsCodeLine *tmp = n > 0 ? malloc((size_t) n * sizeof(*tmp)) : NULL;
    for (int i = 0; tmp && i < n; i++) {
        tmp[i].line = lines[i].line;
        tmp[i].bg = code_color(lines[i].bg);
        tmp[i].full = lines[i].full;
        tmp[i].marker = lines[i].marker;
        tmp[i].marker_color = code_color(lines[i].marker_color);
    }
    widgets_set_code_lines(w, e, tmp, tmp ? n : 0);
    free(tmp);
    engine_mark_paint(engine_get(), e);
}

/* ------------------------------------------------------------------------- */
/* Geometry                                                                   */
/* ------------------------------------------------------------------------- */

int html_dom_get_rect(const HtmlElem e, HtmlRect *out)
{
    Engine *g = engine_get();
    if (out) memset(out, 0, sizeof(*out));
    if (!out || !alive(engine_dom(g), e)) return 0;
    engine_flush_layout(g);
    Box *box = engine_elem_box(g, e);
    if (!box) return 0;
    if (!layout_box_rect_in_viewport(box, out)) {
        memset(out, 0, sizeof(*out));
        return 0;
    }
    return 1;
}

/* A box paints something: a non-empty area of its own that is not visibility:hidden, or a
 * descendant that does (absolutely positioned HUD parts hang off zero-height holders). */
static int box_shows_area(const Box *box)
{
    if (!box || (box->style && box->style->opacity <= 0.01f)) return 0;
    if (!(box->flags & BOXF_HIDDEN) && box->w > 0.0f && box->h > 0.0f) return 1;
    for (const Box *c = box->first_child; c; c = c->next) {
        if (box_shows_area(c)) return 1;
    }
    return 0;
}

int html_dom_is_shown(const HtmlElem e)
{
    Engine *g = engine_get();
    if (!alive(engine_dom(g), e)) return 0;
    engine_flush_layout(g);
    const Box *box = engine_elem_box(g, e);
    if (!box) return 0;
    for (const Box *b = box->parent; b; b = b->parent) {
        if (b->style && b->style->opacity <= 0.01f) return 0;
    }
    return box_shows_area(box);
}

float html_dom_offset_width(const HtmlElem e)
{
    Engine *g = engine_get();
    if (!alive(engine_dom(g), e)) return 0.0f;
    engine_flush_layout(g);
    Box *box = engine_elem_box(g, e);
    return box ? layout_offset_width(box) : 0.0f;
}

float html_dom_offset_height(const HtmlElem e)
{
    Engine *g = engine_get();
    if (!alive(engine_dom(g), e)) return 0.0f;
    engine_flush_layout(g);
    Box *box = engine_elem_box(g, e);
    return box ? layout_offset_height(box) : 0.0f;
}

void html_dom_viewport(float *w, float *h, float *dpr)
{
    engine_viewport(engine_get(), w, h);
    if (dpr) *dpr = engine_device_pixel_ratio(engine_get());
}

void html_dom_set_device_pixel_ratio(const float dpr)
{
    engine_set_device_pixel_ratio(engine_get(), dpr);
}

HtmlElem html_dom_element_at(const float x, const float y)
{
    Engine *g = engine_get();
    Dom *dom = engine_dom(g);
    if (!dom) return 0;
    LayoutTree *tree = engine_flush_layout(g);
    Box *box = tree ? layout_hit_test(tree, x, y, 1) : NULL;
    while (box && !box->node) box = box->parent;
    return box ? dom_node_elem(dom, box->node) : 0;
}

HtmlElem html_dom_painted_at(const float x, const float y)
{
    Engine *g = engine_get();
    Dom *dom = engine_dom(g);
    if (!dom) return 0;
    LayoutTree *tree = engine_flush_layout(g);
    Box *box = tree ? layout_hit_test(tree, x, y, 0) : NULL;
    while (box && !box->node) box = box->parent;
    return box ? dom_node_elem(dom, box->node) : 0;
}

void html_dom_set_top_layer(const HtmlElem e)
{
    Engine *g = engine_get();
    if (!g) return;
    engine_set_top_layer(g, e && alive(engine_dom(g), e) ? e : 0);
}

HtmlElem html_dom_active_element(void)
{
    Engine *g = engine_get();
    EventSystem *es = engine_events(g);
    return es ? events_focused(es) : 0;
}

void html_dom_observe_resize(const HtmlElem e, const HtmlResizeFn fn, void *user)
{
    Engine *g = engine_get();
    EventSystem *es = engine_events(g);
    if (!es || !alive(engine_dom(g), e)) return;
    if (fn) events_observe_resize(es, e, fn, user);
    else events_unobserve_resize(es, e);
}

#else
typedef int html_engine_dom_ops_not_compiled;
#endif
