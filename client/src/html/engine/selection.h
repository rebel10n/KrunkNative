/*
 * selection.h - text selection over laid-out text (the page's Selection, not a
 * text field's: widgets.c owns those).
 *
 * A selection lives inside one selectable region (`root`): the outermost
 * ancestor-or-self of the pressed element whose ancestors up to there all have
 * a computed user-select other than none (the dev console's .logs, whose
 * parent sets user-select:none; <body> on a page without such a rule).  Text
 * with user-select:none inside the region is skipped (not highlighted, not
 * copied), form controls never start one, generated ::before/::after text is
 * not part of it (Chrome).
 *
 * Points are (text node, byte offset into its box's collapsed text): they
 * survive box-tree rebuilds (a console line appended under the selection).
 * The engine clears the selection when a node holding an endpoint is removed
 * or when the region stops being rendered.
 *
 * Order and geometry come from the line boxes of the region (layout.h): the
 * text items of every block container in box-tree pre-order are the segments
 * a selection spans.  Copied text (sel_text) inserts "\n" between blocks and
 * at <br>, and keeps the source text between two runs of one text node (the
 * space or newline a wrap consumed).
 *
 * Pure C11 on dom.h + layout.h + css_cascade.h; no GL, no GLFW.
 */
#ifndef KN_HTML_SELECTION_H
#define KN_HTML_SELECTION_H

#include <stddef.h>
#include <stdint.h>

#include "css_types.h"
#include "dom.h"
#include "layout.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Double / triple click: presses this close in time (ms) and space (CSS px). */
#define SEL_MULTI_CLICK_MS 500.0
#define SEL_MULTI_CLICK_PX 4.0f
/* Autoscroll of a scrolling region while dragging outside it: px per frame per
 * px outside, capped. */
#define SEL_AUTOSCROLL_MAX_PX 40.0f /* (engine.c sel_autoscroll) */

/* Highlight when no ::selection rule applies (Chrome's default active
 * selection background; the text keeps its colour). */
#define SEL_DEFAULT_BG_R 51
#define SEL_DEFAULT_BG_G 144
#define SEL_DEFAULT_BG_B 255

typedef struct SelPoint {
    lxb_dom_node_t *node;   /* text node, NULL = unset                         */
    uint32_t off;           /* byte offset into the text box's collapsed text  */
} SelPoint;

typedef enum SelGranularity { SEL_CHAR = 0, SEL_WORD, SEL_BLOCK } SelGranularity;

/* One text item of a line (or a <br>: tb == NULL). */
typedef struct SelSegment {
    Box *tb;                /* BOX_TEXT                                         */
    const TextRun *run;
    const LineBox *line;
    const Box *block;       /* block container owning the line                  */
    uint32_t start, end;    /* byte range of the run in tb->text (full_len)     */
} SelSegment;

/* Selected byte range of one text box, plus the ::selection colours. */
typedef struct SelEntry {
    const Box *tb;
    uint32_t start, end;    /* in tb->text                                      */
    CssColor bg, fg;
    uint8_t has_fg;
} SelEntry;

typedef struct Selection {
    HtmlElem root;          /* 0 = no selection                                 */
    SelPoint anchor, focus;
    uint8_t dragging;
    uint8_t granularity;    /* SelGranularity of the press that started it      */
    SelPoint word_a, word_b;/* the double-clicked word / triple-clicked block    */

    /* multi-click detection */
    double last_press_ms;
    float last_press_x, last_press_y;
    int clicks;

    /* scratch: segments of the region, resolved entries (sorted by tb) */
    SelSegment *segs;
    int n_segs, cap_segs;
    SelEntry *entries;
    int n_entries, cap_entries;
} Selection;

void sel_init(Selection *s);
void sel_free(Selection *s);
/* Drops the selection (keeps the multi-click state).  Returns 1 when there was one. */
int  sel_clear(Selection *s);
int  sel_active(const Selection *s);       /* root set and anchor != focus      */

/* The selectable region a press on `target` starts a selection in, 0 when
 * none (user-select:none, a form control, nothing hit). */
HtmlElem sel_region_for(Dom *dom, HtmlElem target);

/* Collects the segments of `root_box` into s->segs; returns the count. */
int  sel_collect(Selection *s, Box *root_box);
/* Nearest caret position to the viewport point among the collected segments
 * (lines first by vertical distance, then by horizontal).  0 when the
 * region has no text. */
int  sel_point_at(const Selection *s, float vx, float vy, SelPoint *out);
/* Word / block around a point (collected segments). */
int  sel_word_at(const Selection *s, SelPoint p, SelPoint *a, SelPoint *b);
int  sel_block_at(const Selection *s, SelPoint p, SelPoint *a, SelPoint *b);
/* The whole text of the collected segments (Ctrl+A). */
int  sel_all(const Selection *s, SelPoint *a, SelPoint *b);
/* Sets focus to `p`, applying the granularity of the press (word/block drags
 * extend by whole words/blocks and keep the first one selected). */
void sel_extend_to(Selection *s, SelPoint p);

/* Recomputes s->entries from the collected segments (call after sel_collect
 * on the current layout).  Returns 0 when an endpoint is no longer laid out
 * (the caller clears the selection). */
int  sel_resolve(Selection *s, Dom *dom);
/* Selected part of `run` (a run of text box `tb`): byte range relative to
 * run->text and the colours.  0 when nothing of it is selected. */
int  sel_run_range(const Selection *s, const Box *tb, const TextRun *run, uint32_t *start, uint32_t *end,
                   CssColor *bg, CssColor *fg, int *has_fg);
/* Selected text, malloc'd UTF-8 (NULL when empty).  Uses the collected segments. */
char *sel_text(const Selection *s);

/* 1 when `node` lies inside the subtree of `root` (exclusive). */
int  sel_node_inside(const lxb_dom_node_t *node, const lxb_dom_node_t *root);

#ifdef __cplusplus
}
#endif

#endif /* KN_HTML_SELECTION_H */
