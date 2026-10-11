/*
 * events.h - input and event system for the KrunkNative HTML/CSS engine.
 *
 * Owns: hover/active/focus state (stored as pseudo-class state on the DOM so
 * the Lexbor matcher sees :hover/:active/:focus), pointer capture for
 * widgets, mouse/keyboard routing, inline `on*` handler parsing + caching +
 * bubbling dispatch, wheel scrolling of scroll containers, and
 * ResizeObserver-style callbacks.  Pure C11, no GL, no GLFW headers.
 *
 * What it consumes:
 *   - dom.h                 the element tree, attributes, pseudo-class bits
 *   - html_dom.h            HtmlEvent/HtmlArg + html_ui_dispatch*() (html_dom_core.c)
 *   - EventsHost            layout/cascade services (hit testing, boxes,
 *                           scroll geometry).  engine.c fills it from
 *                           layout.h / css_cascade.h; tests stub it.
 *   - EventsWidgetHooks     widgets.c registers these to receive focused
 *                           keyboard input, drags and the select popup.
 *
 * Coordinates are CSS px in viewport space (same space as html_dom_get_rect).
 * Key/button/action/mods values are GLFW's (see EV_KEY_* below); the engine
 * forwards GLFW callbacks unchanged.  Everything is main-thread only.
 *
 * Event model (documented choices, matching Chrome where it matters):
 *   - Inline handlers bubble target -> root.  Every element on the path with
 *     an `on<type>` attribute runs; there is no stopPropagation for inline
 *     handlers.  `ev.target` passed to an inline handler is the element the
 *     attribute sits on (`this` / currentTarget), and `value`/`checked` are
 *     read from that element.  A statement `return f(...)` whose handler
 *     returned 0, or a literal `return false`, cancels the *default action*
 *     only (checkbox toggle revert, label forwarding, character insertion);
 *     handlers on ancestors still run.
 *   - Listeners registered through html_ui_set_listener() are called with
 *     `ev.target` = the original event target (DOM semantics, so the
 *     #windowHolder backdrop test `e.target === wholder` works).  They run
 *     for every element on the path that has an id, then once with
 *     elem_id == NULL (document/window).  For keydown/keyup the NULL
 *     listener runs first (capture phase).  A listener returning 0 prevents
 *     default.
 *   - mouseenter/mouseleave do not bubble (fired per entered/left element,
 *     outermost first for enter, innermost first for leave); mouseover/
 *     mouseout bubble from the new/old target.  focus/blur do not bubble.
 *   - click is synthesised on the nearest common ancestor of the mousedown
 *     and mouseup targets (button 0 only); contextmenu fires on right press.
 *   - A disabled form control receives no mouse events and cannot be focused.
 *   - Keys while an input/select is focused never reach the game: every
 *     events_key()/events_char() call reports consumed.  Escape blurs the
 *     focused control and is consumed (the engine must not toggle the menu).
 *   - Not implemented (deliberately): Tab traversal, title tooltips,
 *     dblclick, text selection outside inputs, IME.
 */
#ifndef KN_HTML_EVENTS_H
#define KN_HTML_EVENTS_H

#include <stddef.h>
#include <stdint.h>

#include "dom.h"
#include "html_dom.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- GLFW constants we depend on (mirrored so this header stays GLFW-free) */
#define EV_RELEASE 0
#define EV_PRESS 1
#define EV_REPEAT 2
#define EV_BUTTON_LEFT 0
#define EV_BUTTON_RIGHT 1
#define EV_BUTTON_MIDDLE 2
#define EV_KEY_ESCAPE 256
#define EV_KEY_ENTER 257
#define EV_KEY_TAB 258
#define EV_KEY_KP_ENTER 335
#define EV_MOD_SHIFT 1
#define EV_MOD_CONTROL 2
#define EV_MOD_ALT 4
#define EV_MOD_SUPER 8

/* Pixels scrolled per wheel click (Chrome on Linux/Windows). */
#define EV_SCROLL_PIXELS_PER_CLICK 53.0f

/* ---- inline handler parsing ---------------------------------------------- */

#define EV_HANDLER_MAX_ARGS 8
#define EV_HANDLER_MAX_CALLS 8

/* One statement of an inline handler attribute.
 *   name == NULL && prevent_default   : `return false`
 *   name != NULL                      : a call; `returns` set when written
 *                                       `return name(...)`, in which case a
 *                                       handler result of 0 cancels default. */
typedef struct HandlerCall {
    const char *name;                  /* interned: "showWindow", "windows.0.changeTab", "SOUND.play", "window.open", "kn.hideErrorNotification" */
    HtmlArg args[EV_HANDLER_MAX_ARGS]; /* str fields are interned (never freed) */
    int argc;
    uint8_t returns;
    uint8_t prevent_default;
} HandlerCall;

/* Parses an (entity-decoded) inline handler attribute.
 *
 * Grammar:
 *   program := stmt (';' stmt)* [';']            (empty statements allowed)
 *   stmt    := 'return' ('false' | 'true' | call) | call | special
 *   call    := callee '(' [arg (',' arg)*] ')'
 *   callee  := ['window.'] ident (('.' ident) | ('[' digits ']'))*
 *              rendered with '.' separators: windows[0].changeTab -> "windows.0.changeTab";
 *              a leading "window." is dropped unless it is the whole
 *              qualifier (window.open stays "window.open")
 *   arg     := 'str' | "str" (\' \" \\ \n escapes) | number (-?d+(.d+)?)
 *            | true | false | this | this.value | this.checked | event
 *            | ident[.ident]*  (bare identifier, incl. undefined/null -> HTML_ARG_STR of its text)
 *   special := errorNotification.style.display = 'none'  -> call "kn.hideErrorNotification"
 *
 * Returns the number of statements written to out (0 for an empty/blank
 * attribute), or -1 on a syntax error (nothing is dispatched for that
 * attribute; the engine logs it once). */
int events_parse_handler(const char *text, HandlerCall *out, int max);

/* ---- host services (layout / cascade), filled by engine.c ---------------- */

typedef struct Box Box; /* layout.h; opaque here */

typedef struct EvScrollInfo {
    float scroll_x, scroll_y;   /* current offsets, px                         */
    float scroll_w, scroll_h;   /* scrollWidth / scrollHeight                  */
    float client_w, client_h;   /* clientWidth / clientHeight                  */
    uint8_t can_x, can_y;       /* overflow-x/-y is auto|scroll                */
} EvScrollInfo;

typedef struct EventsHost {
    void *user;
    /* layout_hit_test(root, x, y): deepest box under the point, honouring
     * pointer-events, transforms, clips and scroll offsets.  NULL = nothing. */
    Box *(*hit_test)(void *user, float x, float y);
    /* Owning element of a box (anonymous/text boxes -> nearest element). */
    HtmlElem (*box_elem)(void *user, Box *box);
    /* ElemData.box, NULL when not laid out (display:none). */
    Box *(*elem_box)(void *user, HtmlElem e);
    /* Border box in viewport CSS px; returns 0 when the element has no box. */
    int (*elem_rect)(void *user, HtmlElem e, HtmlRect *out);
    /* 0 when the box is not a scroll container. */
    int (*scroll_info)(void *user, Box *box, EvScrollInfo *out);
    /* Sets (already clamped) offsets and marks paint dirty. */
    void (*set_scroll)(void *user, Box *box, float x, float y);
    /* Optional: cascade_restyle_dirty().  Called after hover/active/focus
     * changes so computed `cursor` is current; may be NULL when the frame
     * loop restyles anyway. */
    void (*restyle)(void *user);
} EventsHost;

/* Ready-made host over layout.h (hit test, boxes, scroll state).  `tree`
 * points at the engine's LayoutTree slot because layout_build() replaces the
 * tree; `restyle` is left NULL (set it afterwards when wanted).  Not built
 * when KN_EVENTS_STANDALONE is defined (unit tests without layout.c). */
typedef struct LayoutTree LayoutTree;
typedef struct EventsLayoutLink {
    Dom *dom;
    LayoutTree **tree;
} EventsLayoutLink;
void events_host_layout(EventsHost *out, EventsLayoutLink *link);

/* ---- widget hooks, registered by widgets.c -------------------------------- */

enum {
    EV_WIDGET_CAPTURE = 1,   /* mouse_down: take pointer capture until release */
    EV_WIDGET_CONSUMED = 2   /* mouse_down: handled entirely (popup); skip DOM dispatch */
};

typedef struct EventsWidgetHooks {
    void *user;
    /* Called first on every press with the hit element (0 when none).  Return
     * EV_WIDGET_* bits. */
    int (*mouse_down)(void *user, HtmlElem e, float x, float y, int button, int mods);
    /* Called while captured (e == capture target) with the held button mask. */
    void (*mouse_move)(void *user, HtmlElem e, float x, float y, int buttons);
    /* Called on release for the capture target (or the hit element). */
    void (*mouse_up)(void *user, HtmlElem e, float x, float y, int button);
    /* Keyboard for the focused control; return 1 when consumed (caret
     * moves, backspace, Enter in a select popup, ...). */
    int (*key)(void *user, HtmlElem e, int key, int scancode, int action, int mods);
    /* Character insertion for the focused control; return 1 when consumed. */
    int (*chr)(void *user, HtmlElem e, uint32_t codepoint);
    void (*focus)(void *user, HtmlElem e);
    void (*blur)(void *user, HtmlElem e);
    /* mousedown on a <select>: open its popup.  Return EV_WIDGET_* bits. */
    int (*open_select)(void *user, HtmlElem e, float x, float y);
} EventsWidgetHooks;

/* ---- the system ----------------------------------------------------------- */

#define EV_MAX_CHAIN 64

typedef struct EvResizeObserver {
    HtmlElem elem;
    HtmlResizeFn fn;
    void *user;
    HtmlRect last;
    uint8_t seen;
} EvResizeObserver;

typedef struct EvHandlerEntry EvHandlerEntry;

typedef struct EventSystem {
    Dom *dom;
    EventsHost host;
    EventsWidgetHooks widgets;

    float mouse_x, mouse_y;
    uint8_t has_mouse;
    uint32_t buttons;              /* held button mask (1 << button)             */
    int mods;                      /* last modifier state                        */
    int last_key;                  /* last keydown GLFW key, used for keypress   */

    HtmlElem hover_chain[EV_MAX_CHAIN];  /* target first, root last              */
    int hover_len;
    HtmlElem active_chain[EV_MAX_CHAIN];
    int active_len;
    HtmlElem focus;
    HtmlElem capture;              /* pointer capture target (widgets)           */
    HtmlElem down_target;          /* mousedown target for click synthesis       */
    uint8_t cursor;                /* CssCursor of the hovered element           */

    EvResizeObserver *observers;
    int observer_count, observer_cap;

    EvHandlerEntry *handlers;      /* parsed inline handler cache (hash table)   */
    uint32_t handler_cap, handler_count;

    char value_buf[512];           /* this.value scratch for <select>            */
    uint8_t in_after_layout;
} EventSystem;

void events_init(EventSystem *es, Dom *dom, const EventsHost *host);
void events_fini(EventSystem *es);
void events_set_widget_hooks(EventSystem *es, const EventsWidgetHooks *hooks);

/* ---- input entry points (engine forwards GLFW callbacks) ------------------ */

void events_mouse_move(EventSystem *es, float x, float y);
void events_mouse_button(EventSystem *es, int button, int action, int mods);
/* dx/dy in wheel clicks (GLFW scroll offsets); x/y the cursor.  Returns 1
 * when a scroll container consumed the delta. */
int events_scroll(EventSystem *es, float dx, float dy, float x, float y);
/* Returns 1 when the page consumed the key (focused control, or a listener/
 * handler prevented default): the engine must not act on it (menu toggle). */
int events_key(EventSystem *es, int key, int scancode, int action, int mods);
int events_char(EventSystem *es, uint32_t codepoint);
/* The mouse left the window: clears hover. */
void events_mouse_leave(EventSystem *es);

/* ---- state ----------------------------------------------------------------- */

HtmlElem events_focused(const EventSystem *es);
HtmlElem events_hovered(const EventSystem *es);
HtmlElem events_captured(const EventSystem *es);
/* Programmatic focus()/blur() (html_dom_focus/blur).  focus on 0 == blur. */
void events_focus(EventSystem *es, HtmlElem e);
void events_blur(EventSystem *es);
void events_set_pointer_capture(EventSystem *es, Box *box);
void events_release_pointer_capture(EventSystem *es);
/* Computed `cursor` of the hovered element (CssCursor) and its CSS keyword;
 * the engine maps it to a GLFW cursor after each frame. */
int events_cursor(const EventSystem *es);
const char *events_cursor_name(int css_cursor);
/* Element-level focusability as used by click focus (input/select/textarea/
 * button, a[href], [tabindex]; not disabled, not type=hidden). */
int events_is_focusable(EventSystem *es, HtmlElem e);

/* ---- dispatch ------------------------------------------------------------- */

/* Dispatches an event of `type` at `target` through inline handlers
 * (bubbling) and registered listeners.  key/button are placed in the
 * HtmlEvent.  Returns 0 when default was prevented.  widgets.c uses this for
 * "input"/"change" after editing a control. */
int events_fire(EventSystem *es, HtmlElem target, const char *type, int key, int button);
/* Fires `input` then `change` at a control (checkbox/select/range commit). */
void events_fire_input_change(EventSystem *es, HtmlElem control);
/* Programmatic click() with full activation behaviour (label forwarding,
 * checkbox toggle, input/change). */
void events_click(EventSystem *es, HtmlElem target);

/* ---- DOM lifecycle notifications ------------------------------------------ */

/* Must be called by the engine before an element (and its subtree) is
 * destroyed so stale handles never linger in hover/active/focus/capture/
 * observers/handler cache. */
void events_elem_removed(EventSystem *es, HtmlElem e);
/* Drops cached parsed handlers whose attribute text changed is automatic
 * (the cache compares the live attribute text); this forces a full drop. */
void events_invalidate_handlers(EventSystem *es);

/* ---- resize observers + post-layout ----------------------------------------- */

void events_observe_resize(EventSystem *es, HtmlElem e, HtmlResizeFn fn, void *user);
void events_unobserve_resize(EventSystem *es, HtmlElem e);
/* Called by the frame loop after layout: fires resize observers whose
 * viewport rect changed since last seen (once per observer per call, first
 * call always fires like ResizeObserver's initial notification) and re-runs
 * hover hit testing under the last known mouse position. */
void events_after_layout(EventSystem *es);

#ifdef __cplusplus
}
#endif

#endif /* KN_HTML_EVENTS_H */
