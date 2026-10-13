/*
 * engine.h - internal glue of the native HTML/CSS engine backend (engine.c,
 * dom_ops.c, ui_test.c).  Not a public header: the client talks to the
 * engine through client/include/html_ui.h and client/include/html_dom.h.
 *
 * engine.c implements html_ui_backend.h (init/fini/resize/update/render and
 * the GLFW input entry points) and owns one Engine instance: the Dom, the
 * stylesheets, the text/image/anim/event/widget systems, the layout tree,
 * the paint context and the GL backend.  dom_ops.c implements the html_dom_*
 * DOM operations on top of it and calls back into the helpers below so that
 * every mutation keeps the layout tree, the paint cache, the event state and
 * the animation state coherent.
 *
 * Frame pipeline (engine_flush + render):
 *   anim_tick -> cascade_restyle (dirty) -> animation sync of restyled
 *   elements -> layout (box tree rebuilt after structural/style changes,
 *   layout_run otherwise) -> events_after_layout (resize observers, hover)
 *   -> paint_build (only when something changed) -> glb_submit (every frame).
 *
 * Threading: main thread only (GL context current for render/init/fini).
 */
#ifndef KN_HTML_ENGINE_H
#define KN_HTML_ENGINE_H

#include <stddef.h>
#include <stdint.h>

#include "dom.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Engine Engine;
typedef struct Widgets Widgets;
typedef struct EventSystem EventSystem;
typedef struct LayoutTree LayoutTree;
typedef struct Box Box;

/* The live engine, NULL before html_ui_backend_init() / after fini. */
Engine *engine_get(void);
Dom *engine_dom(Engine *g);
Widgets *engine_widgets(Engine *g);
EventSystem *engine_events(Engine *g);
/* Viewport in CSS px (== framebuffer px, devicePixelRatio 1). */
void engine_viewport(Engine *g, float *w, float *h);

/* Brings computed styles up to date (cascade + animation sync). */
void engine_flush_style(Engine *g);
/* Styles + layout up to date (what a browser does before a geometry read).
 * Returns the layout tree (NULL when nothing could be laid out). */
LayoutTree *engine_flush_layout(Engine *g);
/* Box of an element after engine_flush_layout(), NULL when display:none. */
Box *engine_elem_box(Engine *g, HtmlElem e);

/* Must be called before DOM nodes are destroyed (remove, innerHTML,
 * textContent): unlinks the boxes of those nodes (layout reads box->node when
 * it releases boxes), schedules the incremental box rebuild and
 * tells events/widgets/animations that the elements go away.  include_root
 * = 0 notifies only the descendants of `e`. */
void engine_before_remove(Engine *g, HtmlElem e, int include_root);
/* The child list / text of `e` changed (or a layout-relevant attribute of one
 * of its children): its boxes are rebuilt incrementally on the next flush
 * (layout_children_changed). */
void engine_children_changed(Engine *g, HtmlElem e);
/* Before dom_append()/insert moves an attached element to a new parent. */
void engine_before_move(Engine *g, HtmlElem e);
/* html_dom_canvas_put: a <canvas>'s bitmap (straight RGBA8, row 0 on top), w x h px.
 * Uploaded as a texture the paint of the canvas box draws; 0 on failure. */
int engine_canvas_put(Engine *g, HtmlElem e, const unsigned char *rgba, int w, int h);
/* html_dom_canvas_gl: the canvas paints the client's GL texture (premultiplied RGBA, w x h,
 * row 0 at the bottom; not owned, sampled at every submit); tex 0 forgets it. */
void engine_canvas_gl(Engine *g, HtmlElem e, unsigned tex, int w, int h);
/* Only the paint output changed (widget value, scroll offset). */
void engine_mark_paint(Engine *g, HtmlElem e); /* e = 0: everything */

/* Clock used for animations, timers and the caret blink (ms).  ui_test pins
 * it for deterministic renders; ms < 0 returns to glfwGetTime(). */
double engine_now_ms(void);
void engine_set_clock_override(double ms);
// 1: scrollbars take no space and are not painted (matches headless Chromium --hide-scrollbars; uidiff only).
void engine_set_hide_scrollbars(int hide);

/* 1 when the last html_ui_key() was consumed by the page (a focused control,
 * e.g. Escape blurred an input).  The client must not act on that key (no
 * menu toggle).  Proposed public wrapper: html_ui_key_consumed(). */
int html_ui_backend_key_consumed(void);

/* Per-stage timings of the last frames (ui_test bench / profile). */
typedef struct EngineStats {
    double restyle_ms, layout_ms, paint_ms, submit_ms, frame_ms;
    unsigned restyles, layouts, rebuilds, partial_rebuilds, paints, frames;
    unsigned restyled_elems;
    int draw_calls, vertices, dl_items;
} EngineStats;
void engine_stats(EngineStats *out, int reset);

/* Debug: box tree with rects of the subtree of `e` (0 = whole document). */
void engine_dump_boxes(Engine *g, HtmlElem e, void (*print)(const char *line, void *user), void *user);

#ifdef __cplusplus
}
#endif

#endif /* KN_HTML_ENGINE_H */
