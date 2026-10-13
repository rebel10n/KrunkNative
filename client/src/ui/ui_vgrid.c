// Virtual card grids: see client/include/ui_vgrid.h.
//
// The list's children are always
//   <div data-kn-vg=serial style='height:TOPpx'></div> cards[first, last) <div style='height:BOTpx'></div>
// with first a multiple of the column count, so the cards wrap into the same rows (and the
// same x positions) as the full list, the top spacer standing in for the rows above them and
// the bottom one for the rows below. Geometry comes from the page, never from CSS constants:
// the columns are the cards sharing the first card's y, the pitch the distance between the
// first and the last rendered row, both converted from viewport px (getBoundingClientRect,
// after #uiBase's transform: scale) to the list's own px (offsetHeight, scrollTop).
#include <ui_vgrid.h>

#include <shared.h> // concat
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned g_vgrid_serial;

static int clampi(const int v, const int lo, const int hi) { return v < lo ? lo : v > hi ? hi : v; }

static HtmlElem vgrid_list(const VGrid *g) {
    const HtmlElem list = g->list_id[0] ? html_dom_by_id(g->list_id) : 0;
    char mark[24];
    HtmlElem top = list ? html_dom_first_child(list) : 0;
    for (int i = 0; top && i < 3 && html_dom_get_attr(top, "data-kn-vg", mark, sizeof(mark)) < 0; i++) top = html_dom_next(top); // after the prefix
    if (!top || html_dom_get_attr(top, "data-kn-vg", mark, sizeof(mark)) < 0) return 0;
    return (unsigned) strtoul(mark, NULL, 10) == g->serial ? list : 0;
}

static int rows_of(const VGrid *g, const int count) { return g->cols > 0 ? (count + g->cols - 1) / g->cols : 0; }

// Spacer heights for rendered positions [first, last): the rows above and below them. Before the
// first measurement the rest of the list is unknown (a measuring render: both 0).
static void vgrid_spacers(const VGrid *g, const int first, const int last, float *top, float *bottom) {
    *top = g->cols > 0 ? (float) (first / g->cols) * g->pitch : 0.0f;
    const float b = g->cols > 0 ? (float) (rows_of(g, g->count) - rows_of(g, last)) * g->pitch : 0.0f;
    *bottom = b > 0.0f ? b : 0.0f;
}

// The list's children for positions [first, last).
static void vgrid_write(VGrid *g, StrBuf *out, const int first, const int last) {
    g->first = first;
    g->last = last;
    float top, bottom;
    vgrid_spacers(g, first, last, &top, &bottom);
    if (g->prefix) sb_append(out, g->prefix);
    sb_appendf(out, "<div data-kn-vg='%u' style='height:%.2fpx'></div>", g->serial, top);
    for (int pos = first; pos < last; pos++) g->card(out, pos, g->user);
    sb_appendf(out, "<div style='height:%.2fpx'></div>", bottom);
}

static void vgrid_render(VGrid *g, const HtmlElem list, const int first, const int last) {
    StrBuf out = {0};
    vgrid_write(g, &out, first, last);
    char *html = sb_finish(&out);
    html_dom_set_html(list, html);
    free(html);
    g->renders++;
}

static void vgrid_cards_html(VGrid *g, const HtmlElem ref, const HtmlInsertPos where, const int from, const int to) {
    if (from >= to) return;
    StrBuf out = {0};
    for (int pos = from; pos < to; pos++) g->card(&out, pos, g->user);
    char *html = sb_finish(&out);
    html_dom_insert_html(ref, where, html);
    free(html);
}

// Scrolling: keep the cards both windows share, drop the rows that left, add the rows that came
// in and resize the spacers (a few cards per step instead of the whole window; hovered and
// animated cards that stay on screen keep their state). A full render when they do not overlap.
static void vgrid_shift(VGrid *g, const HtmlElem list, const int first, const int last) {
    const int n = g->last - g->first;
    HtmlElem top = html_dom_first_child(list);
    char mark[24];
    while (top && html_dom_get_attr(top, "data-kn-vg", mark, sizeof(mark)) < 0) top = html_dom_next(top);
    HtmlElem *cards = n > 0 ? malloc((size_t) n * sizeof(*cards)) : NULL;
    HtmlElem c = top ? html_dom_next(top) : 0;
    for (int i = 0; cards && i < n && c; i++, c = html_dom_next(c)) cards[i] = c;
    const HtmlElem bottom = c;
    if (!cards || !top || !bottom || g->cols <= 0 || last <= g->first || first >= g->last) {
        free(cards);
        vgrid_render(g, list, first, last);
        return;
    }
    for (int pos = g->first; pos < first; pos++) html_dom_remove(cards[pos - g->first]);
    for (int pos = last; pos < g->last; pos++) html_dom_remove(cards[pos - g->first]);
    free(cards);
    vgrid_cards_html(g, top, HTML_INSERT_AFTER_END, first, g->first);
    vgrid_cards_html(g, bottom, HTML_INSERT_BEFORE_BEGIN, g->last, last);
    float top_px, bottom_px;
    vgrid_spacers(g, first, last, &top_px, &bottom_px);
    char v[32];
    snprintf(v, sizeof(v), "%.2fpx", top_px);
    html_dom_set_style(top, "height", v);
    snprintf(v, sizeof(v), "%.2fpx", bottom_px);
    html_dom_set_style(bottom, "height", v);
    g->first = first;
    g->last = last;
    g->renders++;
}

void vgrid_begin(VGrid *g, const char *list_id, const int count, const VGridCardFn card, void *user) {
    free(g->prefix);
    memset(g, 0, sizeof(*g));
    snprintf(g->list_id, sizeof(g->list_id), "%s", list_id ? list_id : "");
    g->serial = ++g_vgrid_serial;
    g->count = count > 0 ? count : 0;
    g->card = card;
    g->user = user;
    g->pending_top = -1;
}

void vgrid_set_prefix(VGrid *g, const char *html) {
    free(g->prefix);
    g->prefix = html && html[0] ? concat(html, "") : NULL;
}

void vgrid_html(VGrid *g, StrBuf *out) {
    vgrid_write(g, out, 0, g->count < VGRID_FIRST_CHUNK ? g->count : VGRID_FIRST_CHUNK);
}

// Columns, pitch and origin from the rendered cards; 0 while the list is not laid out (hidden)
// or shows fewer than two rows (then it shows every card anyway).
static int vgrid_measure(VGrid *g, const HtmlElem list) {
    HtmlRect lr, first_r = {0}, last_r = {0}, r;
    HtmlScroll sc;
    const float off_h = html_dom_offset_height(list);
    if (!html_dom_get_rect(list, &lr) || !html_dom_get_scroll(list, &sc) || off_h <= 0.0f || lr.h <= 0.0f) return 0;
    const float scale = lr.h / off_h; // viewport px per list px (ancestor transforms)
    char mark[24];
    HtmlElem top = html_dom_first_child(list);
    while (top && html_dom_get_attr(top, "data-kn-vg", mark, sizeof(mark)) < 0) top = html_dom_next(top);
    int cols = 0, n = 0, rows = 1;
    for (HtmlElem c = top ? html_dom_next(top) : 0; c && n < g->last - g->first; c = html_dom_next(c), n++) {
        if (!html_dom_get_rect(c, &r)) return 0;
        if (!n) first_r = r;
        if (fabsf(r.y - first_r.y) < 0.5f) cols++;
        else if (fabsf(r.y - last_r.y) >= 0.5f) rows++;
        last_r = r;
    }
    if (cols <= 0 || rows < 2 || g->first != 0) return 0;
    g->cols = cols;
    g->pitch = roundf((last_r.y - first_r.y) / scale / (float) (rows - 1) * 100.0f) / 100.0f;
    g->origin = (first_r.y - lr.y) / scale + sc.top; // a measuring render starts at position 0, no top spacer
    g->width = html_dom_offset_width(list);
    return g->pitch > 0.0f;
}

static void vgrid_window(const VGrid *g, const HtmlScroll *sc, int *first, int *last) {
    const int rows = rows_of(g, g->count);
    const int r0 = (int) floorf((sc->top - g->origin) / g->pitch) - VGRID_BUFFER_ROWS;
    const int r1 = (int) ceilf((sc->top + sc->client_h - g->origin) / g->pitch) + VGRID_BUFFER_ROWS;
    *first = clampi(r0, 0, rows) * g->cols;
    *last = clampi(clampi(r1, 0, rows) * g->cols, 0, g->count);
}

// A measuring render: the first cards from the top (no spacer heights yet), scrolled to 0.
static void vgrid_measuring_render(VGrid *g, const HtmlElem list) {
    g->cols = 0;
    vgrid_render(g, list, 0, g->count < VGRID_FIRST_CHUNK ? g->count : VGRID_FIRST_CHUNK);
    html_dom_set_scroll(list, 0.0f, 0.0f);
}

int vgrid_tick(VGrid *g) {
    const HtmlElem list = vgrid_list(g);
    if (!list) return 0;
    if (g->cols > 0 && fabsf(html_dom_offset_width(list) - g->width) > 0.5f) { // resized: measure again
        HtmlScroll sc;
        g->pending_top = html_dom_get_scroll(list, &sc) && g->pitch > 0.0f
            ? clampi((int) floorf((sc.top - g->origin) / g->pitch), 0, rows_of(g, g->count)) * g->cols : 0;
        vgrid_measuring_render(g, list);
        return 1;
    }
    if (g->cols <= 0) {
        if (g->count <= VGRID_FIRST_CHUNK && g->last >= g->count) return 1; // every card is rendered
        if (!vgrid_measure(g, list)) return 1;
        if (g->pending_top >= 0) { // keep the card that was at the top before the resize
            html_dom_set_scroll(list, 0.0f, g->origin + (float) (g->pending_top / g->cols) * g->pitch);
            g->pending_top = -1;
        }
        g->dirty = 1; // render below: the spacers now carry the real heights
    }
    HtmlScroll sc;
    if (!html_dom_get_scroll(list, &sc) || sc.client_h <= 0.0f) return 1;
    int first, last;
    vgrid_window(g, &sc, &first, &last);
    // Hysteresis: keep the rendered rows while they still cover the viewport plus one row.
    const int need_first = clampi(first + VGRID_BUFFER_ROWS * g->cols - g->cols, 0, g->count);
    const int need_last = clampi(last - VGRID_BUFFER_ROWS * g->cols + g->cols, 0, g->count);
    if (!g->dirty && g->first <= need_first && g->last >= need_last) return 1;
    if (g->dirty) vgrid_render(g, list, first, last);
    else vgrid_shift(g, list, first, last);
    g->dirty = 0;
    return 1;
}

void vgrid_reset(VGrid *g, const int count) {
    g->count = count > 0 ? count : 0;
    const HtmlElem list = vgrid_list(g);
    if (!list) return;
    if (g->cols <= 0) {
        vgrid_measuring_render(g, list);
        return;
    }
    html_dom_set_scroll(list, 0.0f, 0.0f);
    g->dirty = 1;
    vgrid_tick(g); // renders the rows at the top (just the spacers when empty)
}

void vgrid_refresh(VGrid *g) {
    const HtmlElem list = vgrid_list(g);
    if (list) vgrid_render(g, list, g->first, g->last < g->count ? g->last : g->count);
}
