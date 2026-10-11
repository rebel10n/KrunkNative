/*
 * widgets.h - form controls for the KrunkNative HTML/CSS engine.
 *
 * Owns the per-control editing state (value string, caret, selection,
 * horizontal scroll, popup open flag, drag state), the Chrome-default control
 * geometry, the painting of control internals and the keyboard/mouse
 * behaviour of:
 *
 *   text / url / password / number   caret, selection, placeholder, maxlength,
 *                                    scroll-to-caret, ArrowUp/Down stepping
 *   checkbox / radio                 checked state (`checked` attr + :checked)
 *   range                            drag with pointer capture, min/max/step
 *   color                            swatch + minimal picker popup (hue + SV)
 *   select                           Chrome menulist (text + chevron) + popup
 *
 * Unknown input types, `type="undefined"` and `type="input"` are text.
 *
 * Value model (matches the DOM-ops contract in client/include/html_dom.h):
 *   - text-like / range / color: the live value lives in the WidgetState and
 *     is mirrored into the `value` attribute on every change
 *     (dom_set_value), so `this.value` read through dom_get_value() and the
 *     `:placeholder-shown` state stay coherent.  The attribute is also the
 *     initial value (first access).
 *   - checkbox: dom_set_checked / dom_get_checked (the :checked state bit).
 *   - select: the `selected` attribute of the chosen <option>, first option
 *     when none carries it.  `value` = that option's value attribute (or its
 *     text when absent).
 *
 * Event rules (browser parity):
 *   range: `input` on every value change while dragging / keying, `change`
 *          on release / key commit when the value differs from the start.
 *   text/url/password/number: `input` per edit; `change` on Enter or blur
 *          when the value changed since focus / the last change; number
 *          ArrowUp/Down fire `input` + `change` immediately.
 *          Enter also fires `keypress` (GLFW never delivers a char for it).
 *   checkbox: widgets_activate() toggles and fires `input` + `change`
 *          (events.c calls it from the click default action).
 *   select: choosing an option (popup click / Enter / Up/Down while closed)
 *          fires `input` + `change`.
 *   color: `input` while picking, `change` when the picker closes.
 *   Escape in any focused control blurs it (and commits like blur).
 *
 * Geometry evidence: doc/ref/krunker-v3.1.1/windows/win-settings-*.png
 * (see widgets.c "Chrome defaults" for the measurements).
 *
 * Threading: main thread only.  No GL calls unless gl_ready is set
 * (text_glyph_quad needs a current context).
 */
#ifndef KN_HTML_WIDGETS_H
#define KN_HTML_WIDGETS_H

#include <stddef.h>
#include <stdint.h>

#include "css_types.h"
#include "dom.h"
#include "events.h"
#include "paint_types.h"
#include "text.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Widgets Widgets;
struct PaintWidgetArgs;   /* paint.h */

typedef enum WidgetKind {
    WIDGET_NONE = 0,       /* not a control we paint (hidden, button, div...) */
    WIDGET_TEXT,           /* text, url, search, email, tel, unknown types   */
    WIDGET_PASSWORD,
    WIDGET_NUMBER,
    WIDGET_CHECKBOX,
    WIDGET_RADIO,
    WIDGET_RANGE,
    WIDGET_COLOR,
    WIDGET_SELECT,
    WIDGET_TEXTAREA        /* <textarea>: multi-line pre-wrap editor         */
} WidgetKind;

/* Border box plus resolved border/padding widths (CSS side order).  Paint
 * passes it in the local space of the DisplayList; events in viewport px. */
typedef struct WidgetGeom {
    float x, y, w, h;
    float border[4];
    float padding[4];
} WidgetGeom;

static inline float widget_geom_content_x(const WidgetGeom *g) { return g->x + g->border[CSS_LEFT] + g->padding[CSS_LEFT]; }
static inline float widget_geom_content_y(const WidgetGeom *g) { return g->y + g->border[CSS_TOP] + g->padding[CSS_TOP]; }
static inline float widget_geom_content_w(const WidgetGeom *g) { return g->w - g->border[CSS_LEFT] - g->border[CSS_RIGHT] - g->padding[CSS_LEFT] - g->padding[CSS_RIGHT]; }
static inline float widget_geom_content_h(const WidgetGeom *g) { return g->h - g->border[CSS_TOP] - g->border[CSS_BOTTOM] - g->padding[CSS_TOP] - g->padding[CSS_BOTTOM]; }

/* Everything tunable lives here (Chrome refreshed-controls values). */
typedef struct WidgetsTheme {
    /* text editing */
    float caret_blink_ms;          /* 500                                       */
    float caret_width;             /* 1                                         */
    CssColor selection_bg;         /* #3390ff (Chrome active selection)         */
    CssColor placeholder_color;    /* #757575 fallback when no ::placeholder    */
    int default_size_attr;         /* 20 (input size="")                        */
    /* menulist (select, appearance auto / menulist-button) */
    float menulist_arrow_box;      /* 15 (MenuListArrowWidthInDIP)              */
    float menulist_pad_start;      /* 4  (PopupInternalPaddingStart)            */
    float menulist_pad_end;        /* 1  (+ arrow box)                          */
    float menulist_pad_top;        /* 1                                         */
    float menulist_pad_bottom;     /* 1                                         */
    float menulist_arrow_w;        /* 7  chevron ink width (minus stroke)       */
    float menulist_arrow_h;        /* 3.5 chevron drop                          */
    float menulist_arrow_stroke;   /* 2                                         */
    /* popup list */
    CssColor popup_bg;             /* #ffffff                                   */
    CssColor popup_border;         /* #767676                                   */
    CssColor popup_hover_bg;       /* #1e90ff                                   */
    CssColor popup_hover_fg;       /* #ffffff                                   */
    float popup_row_pad_x;         /* 2                                         */
    float popup_row_pad_bottom;    /* 1                                         */
    float popup_row_min_em;        /* 1.2 (option min-height)                   */
    int popup_max_rows;            /* 20 (Chrome's popup list)                  */
    float popup_scrollbar_w;       /* 15                                        */
    float popup_thumb_min;         /* 18                                        */
    CssColor popup_track;          /* #f1f1f1                                   */
    CssColor popup_thumb;          /* #c1c1c1                                   */
    /* native-looking parts (appearance: auto) */
    CssColor control_border;       /* #767676                                   */
    CssColor control_bg;           /* #ffffff                                   */
    CssColor control_face;         /* #efefef                                   */
    CssColor control_accent;       /* #101010                                   */
    float native_range_track_h;    /* 4                                         */
    float native_range_thumb_d;    /* 16                                        */
    float native_checkbox_radius;  /* 2                                         */
    /* color input */
    float swatch_wrapper_pad_x;    /* 2 (::-webkit-color-swatch-wrapper)        */
    float swatch_wrapper_pad_y;    /* 4                                         */
    float swatch_border;           /* 1 (::-webkit-color-swatch)                */
    CssColor swatch_border_color;  /* #777777                                   */
    float picker_sv_w, picker_sv_h;   /* 180 x 150                              */
    float picker_hue_w;            /* 20                                        */
    float picker_pad;              /* 8                                         */
    float picker_preview_h;        /* 16                                        */
} WidgetsTheme;

/* Services the engine provides.  Any pointer may be NULL (tests). */
typedef struct WidgetsHost {
    void *user;
    /* Computed style of an element (NULL when not styled). */
    const CssComputed *(*computed)(void *user, HtmlElem e);
    /* Computed style of a pseudo element (CssPseudoElem from css_sheet.h:
     * ::-webkit-slider-thumb, ::placeholder, ::-webkit-color-swatch,
     * ::-webkit-color-swatch-wrapper, ::-webkit-inner-spin-button).  NULL
     * when no author rule applies: widgets then uses the UA defaults. */
    const CssComputed *(*pseudo_computed)(void *user, HtmlElem e, int pseudo);
    /* Border box + edges in viewport CSS px.  Return 0 when the element has
     * no box (display:none). */
    int (*elem_geom)(void *user, HtmlElem e, WidgetGeom *out);
    /* Paint (relayout == 0) or layout (1) invalidation of the element. */
    void (*invalidate)(void *user, HtmlElem e, int relayout);
    /* events_fire(): returns 0 when default was prevented. */
    int (*fire)(void *user, HtmlElem e, const char *type, int key);
    /* pointer capture (events_set_pointer_capture on the element's box). */
    void (*set_capture)(void *user, HtmlElem e);
    void (*release_capture)(void *user);
    /* events_focus / events_blur. */
    void (*focus)(void *user, HtmlElem e);
    void (*blur)(void *user, HtmlElem e);
    /* Optional: Chrome's avgCharWidth (OS/2 xAvgCharWidth) and maxCharWidth
     * (head xMax - xMin) of a TextFont in CSS px, for the text-input
     * intrinsic width.  Return 0 when unknown (widgets then uses the '0'
     * advance and the font height). */
    int (*font_char_widths)(void *user, TextFont *font, float *avg, float *max);
    /* Optional but needed under transforms (#uiBase scale()): the control's
     * UNTRANSFORMED border box with its origin at (0, 0) (layout px:
     * box->w/h/border/padding) plus the viewport point (vx, vy) mapped into
     * that space (layout_point_to_box).  Used for caret hit testing and the
     * range value mapping so fonts and thumb sizes stay in CSS px.  When NULL
     * widgets uses elem_geom() and the viewport point as is. */
    int (*elem_local)(void *user, HtmlElem e, float vx, float vy, WidgetGeom *out, float *lx, float *ly);
    /* System clipboard (glfwGetClipboardString / glfwSetClipboardString).
     * get returns UTF-8 owned by the host (valid until the next call) or
     * NULL.  When both are NULL widgets keeps a process-local clipboard. */
    const char *(*clipboard_get)(void *user);
    void (*clipboard_set)(void *user, const char *utf8);
} WidgetsHost;

/* Arguments of a paint call. */
typedef struct WidgetPaintArgs {
    DisplayList *out;
    /* Memory for glyph quads; must outlive the list (paint_alloc). */
    void *(*alloc)(void *user, size_t size);
    void *alloc_user;
    float device_scale;           /* device px per CSS px (glyph rasterisation) */
    double now_ms;                /* caret blink clock                           */
} WidgetPaintArgs;

/* ---- lifetime ------------------------------------------------------------ */

/* `text` may be NULL: measurements then use a synthetic monospace font
 * (0.6 em advance) so logic tests run without FreeType. */
Widgets *widgets_create(Dom *dom, TextSystem *text, const WidgetsHost *host);
void widgets_destroy(Widgets *w);
WidgetsTheme *widgets_theme(Widgets *w);
/* 1 once a GL context is current (enables glyph painting). */
void widgets_set_gl_ready(Widgets *w, int ready);
/* Viewport size in CSS px (popup placement). */
void widgets_set_viewport(Widgets *w, float vw, float vh);
/* Element (subtree) about to be destroyed: drops its state, closes its popup. */
void widgets_elem_removed(Widgets *w, HtmlElem e);

/* ---- classification / geometry --------------------------------------------- */

WidgetKind widgets_kind(Widgets *w, HtmlElem e);
/* Intrinsic CONTENT-box size (CSS px) per Chrome: text-like = size attr x
 * avgCharWidth + (maxCharWidth - avgCharWidth) by line-height; checkbox
 * 13x13; range 129x20; color 44x23 (50x27 border box); select = longest
 * option + 4 + 1 + 15 arrow box by 1 + line-height + 1.  `st` may be NULL
 * (UA font: 13.333px Arial).  Returns 0 for WIDGET_NONE. */
int widgets_intrinsic_size(Widgets *w, HtmlElem e, const CssComputed *st, float *out_w, float *out_h);

/* ---- DOM-ops ----------------------------------------------------------------- */

void widgets_set_value(Widgets *w, HtmlElem e, const char *value);
/* Live value; for select the chosen option's value.  Never NULL. */
const char *widgets_get_value(Widgets *w, HtmlElem e);
void widgets_set_checked(Widgets *w, HtmlElem e, int on);
int widgets_get_checked(Widgets *w, HtmlElem e);
/* Click default action for checkbox/radio: toggle + input + change. */
void widgets_activate(Widgets *w, HtmlElem e);
/* Index of the selected option (-1 when the select has none). */
int widgets_select_index(Widgets *w, HtmlElem e);
void widgets_select_set_index(Widgets *w, HtmlElem e, int index, int fire_events);

/* ---- paint ------------------------------------------------------------------- */

/* Paints the control internals of `e` whose border box is `g` (local space
 * of args->out).  The element's own background/border are painted by
 * paint.c; this adds track thumb / swatch / text / caret / selection /
 * chevron / native parts. */
void widgets_paint(Widgets *w, HtmlElem e, const WidgetGeom *g, const WidgetPaintArgs *args);
/* Frame clock for the caret blink (same clock as WidgetPaintArgs.now_ms);
 * set once per frame before paint_build(). */
void widgets_set_time(Widgets *w, double now_ms);
/* PaintHost.widget_paint adapter (paint.h): user = the Widgets*.  Converts
 * the PaintWidgetArgs box into a WidgetGeom and allocates glyph quads from
 * paint_alloc(args->ctx). */
void widgets_paint_host(void *user, const struct PaintWidgetArgs *args);
/* Paints the open popup (select list or color picker) in viewport space.
 * Call after everything else.  Returns 1 when something was emitted. */
int widgets_paint_overlay(Widgets *w, const WidgetPaintArgs *args);
/* Viewport rect of the open popup (0 when none). */
int widgets_overlay_rect(Widgets *w, float *x, float *y, float *out_w, float *out_h);
/* Element that owns the open popup, 0 when none. */
HtmlElem widgets_popup_owner(Widgets *w);
/* Time (ms, same clock as WidgetPaintArgs.now_ms) of the next caret blink
 * toggle, or -1 when nothing animates.  The engine repaints the focused
 * control at that time. */
double widgets_next_repaint_ms(Widgets *w, double now_ms);

/* ---- events (EventsWidgetHooks) ------------------------------------------------ */

int widgets_mouse_down(Widgets *w, HtmlElem e, float x, float y, int button, int mods);
void widgets_mouse_move(Widgets *w, HtmlElem e, float x, float y, int buttons);
void widgets_mouse_up(Widgets *w, HtmlElem e, float x, float y, int button);
/* Every cursor move (not only captured ones): popup hover highlight. */
void widgets_pointer_moved(Widgets *w, float x, float y);
int widgets_key(Widgets *w, HtmlElem e, int key, int scancode, int action, int mods);
int widgets_char(Widgets *w, HtmlElem e, uint32_t codepoint);
void widgets_on_focus(Widgets *w, HtmlElem e);
void widgets_on_blur(Widgets *w, HtmlElem e);
int widgets_open_select(Widgets *w, HtmlElem e, float x, float y);
/* Mouse wheel (GLFW clicks).  The engine calls it before events_scroll():
 * over an open select popup it scrolls the list; elsewhere it closes the
 * popup.  Returns 1 when consumed (do not scroll the page). */
int widgets_scroll(Widgets *w, float dx, float dy, float x, float y);
void widgets_close_popup(Widgets *w);
/* Fills an EventsWidgetHooks table bound to `w` (events_set_widget_hooks). */
void widgets_fill_event_hooks(Widgets *w, EventsWidgetHooks *out);

/* ---- helpers shared with tests ------------------------------------------------- */

/* Range value <-> thumb x.  track_x/track_w = content box, thumb_w = thumb
 * width.  value_from_x snaps to step and clamps to [min, max]. */
double widgets_range_value_from_x(double min, double max, double step, float track_x, float track_w, float thumb_w, float x);
float widgets_range_thumb_x(double min, double max, double value, float track_x, float track_w, float thumb_w);
/* Chrome-like number serialisation ("1", "0.5", "200"). */
void widgets_format_number(double v, int decimals, char *out, size_t cap);
/* #rrggbb <-> HSV helpers (used by the color picker). */
int widgets_parse_hex_color(const char *s, uint8_t *r, uint8_t *g, uint8_t *b);
void widgets_rgb_to_hsv(uint8_t r, uint8_t g, uint8_t b, float *h, float *s, float *v);
void widgets_hsv_to_rgb(float h, float s, float v, uint8_t *r, uint8_t *g, uint8_t *b);

#ifdef __cplusplus
}
#endif

#endif /* KN_HTML_WIDGETS_H */
