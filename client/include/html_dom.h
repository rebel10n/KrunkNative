// DOM-ops API: how C UI logic (client/src/ui/*) drives the HTML page without JS.
// Implemented natively by the in-house engine (client/src/html/engine/engine.c)
// and on the web by client/src/html/web_dom.c (EM_ASM over the browser DOM).
// Contract and rationale: doc/html-engine-plan.md. Everything runs on the main thread.
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef uint32_t HtmlElem;                      // 0 = none. native: node-table index; web: data-kn-id
typedef struct HtmlRect { float x, y, w, h; } HtmlRect;  // CSS px, viewport origin (getBoundingClientRect)

typedef enum { HTML_INSERT_BEFORE_BEGIN, HTML_INSERT_AFTER_BEGIN, HTML_INSERT_BEFORE_END, HTML_INSERT_AFTER_END } HtmlInsertPos;

// ---- lookup / tree
HtmlElem html_dom_root(void);                                   // <html>
HtmlElem html_dom_body(void);
HtmlElem html_dom_by_id(const char *id);                        // first match in document order (duplicate ids exist)
HtmlElem html_dom_query(HtmlElem root, const char *selector);   // querySelector; root 0 = document
int      html_dom_query_all(HtmlElem root, const char *selector, HtmlElem *out, int max);
HtmlElem html_dom_parent(HtmlElem);
HtmlElem html_dom_first_child(HtmlElem);
HtmlElem html_dom_next(HtmlElem);
HtmlElem html_dom_closest(HtmlElem, const char *selector);
int      html_dom_child_count(HtmlElem);
int      html_dom_tag(HtmlElem, char *buf, size_t n);           // lowercase tag name

// ---- mutation
HtmlElem html_dom_create(const char *tag, const char *id_or_null);   // detached element
HtmlElem html_dom_create_text(const char *text);                     // detached text node
void     html_dom_append(HtmlElem parent, HtmlElem child);
void     html_dom_insert_html(HtmlElem ref, HtmlInsertPos pos, const char *html); // insertAdjacentHTML
void     html_dom_remove(HtmlElem);
void     html_dom_set_html(HtmlElem, const char *html);              // innerHTML
void     html_dom_set_text(HtmlElem, const char *text);              // textContent
int      html_dom_get_text(HtmlElem, char *buf, size_t n);           // returns length needed

// ---- attributes / classes / style
void     html_dom_set_attr(HtmlElem, const char *name, const char *value);
void     html_dom_remove_attr(HtmlElem, const char *name);
int      html_dom_get_attr(HtmlElem, const char *name, char *buf, size_t n); // -1 if absent
void     html_dom_set_class(HtmlElem, const char *cls, int on);      // classList.toggle(cls, on)
int      html_dom_has_class(HtmlElem, const char *cls);
void     html_dom_set_display(HtmlElem, const char *value);          // style.display; "" clears
void     html_dom_set_style(HtmlElem, const char *prop, const char *value); // style.setProperty (incl. --kn-*); "" removes
int      html_dom_get_style(HtmlElem, const char *prop, char *buf, size_t n); // inline style only; -1 if unset

// ---- form state
void     html_dom_set_value(HtmlElem, const char *value);
int      html_dom_get_value(HtmlElem, char *buf, size_t n);
void     html_dom_set_checked(HtmlElem, int checked);
int      html_dom_get_checked(HtmlElem);
void     html_dom_focus(HtmlElem);
void     html_dom_blur(HtmlElem);
void     html_dom_scroll_to_bottom(HtmlElem);                        // scrollTop = scrollHeight

// ---- text controls (<textarea>, text <input>) edited from C. Offsets are UTF-8 byte offsets
// into the value (the web backend converts from the browser's UTF-16 indices).
int      html_dom_text_selection(HtmlElem, int *start, int *end);    // selectionStart/End (start <= end); 0 = not a text control
void     html_dom_text_set_selection(HtmlElem, int start, int end);  // setSelectionRange: caret at end, scrolled into view
void     html_dom_text_replace(HtmlElem, int start, int end, const char *text); // setRangeText(text, start, end, "end"), one undo step; fires no input event natively
int      html_dom_text_caret_rect(HtmlElem, HtmlRect *out);          // the caret box, viewport CSS px; 0 = unknown

// ---- code view: a <textarea> drawn as a code editor (KrunkScript window, client/src/editor/
// editor_script.c): no soft wrap, tab stops, a line-number gutter, the caret line, per-line
// backgrounds and per-byte-range text colours. Colours are 0xRRGGBBAA (alpha 0 = none).
// Native: the engine paints it (widgets.c); web: a <pre> overlay under a transparent-text textarea.
typedef struct HtmlCodeView {
    int tab_size;              // columns per tab stop (CodeMirror tabSize)
    int gutter;                // line numbers
    uint32_t gutter_fg, gutter_bg;
    float gutter_pad_left, gutter_pad_right, gutter_min_w; // number column (CSS px)
    float gutter_extra;        // marker column right of the numbers (CSS px)
    float gutter_gap;          // space between the gutter and the rows (CSS px)
    float text_pad;            // text inset after the gutter (CSS px)
    uint32_t active_line_bg;   // the caret line while nothing is selected
    uint32_t caret;            // 0 = the text colour
    uint32_t selection_bg;     // 0 = the default
} HtmlCodeView;
enum { HTML_CODE_UNDERLINE = 1 };
typedef struct { int start, end; uint32_t color; int flags; } HtmlCodeSpan;   // value bytes, sorted, disjoint
typedef struct { int line; uint32_t bg; int full; uint32_t marker; uint32_t marker_color; } HtmlCodeLine; // 1-based line; bg behind its text (full: the whole row); marker: a gutter code point
void     html_dom_code_view(HtmlElem, const HtmlCodeView *cfg);      // NULL: back to a plain textarea
void     html_dom_code_spans(HtmlElem, const HtmlCodeSpan *spans, int n);
void     html_dom_code_lines(HtmlElem, const HtmlCodeLine *lines, int n);

// ---- geometry (valid after layout; 0/false when display:none)
int      html_dom_get_rect(HtmlElem, HtmlRect *out);
float    html_dom_offset_width(HtmlElem);
float    html_dom_offset_height(HtmlElem);
void     html_dom_viewport(float *w, float *h, float *dpr);          // innerWidth/innerHeight/devicePixelRatio
typedef void (*HtmlResizeFn)(HtmlElem, void *user);
void     html_dom_observe_resize(HtmlElem, HtmlResizeFn fn, void *user); // ResizeObserver-equivalent (coalesced per frame)
HtmlElem html_dom_element_at(float x, float y);                     // document.elementFromPoint (CSS px; pointer-events aware); 0 = none
HtmlElem html_dom_active_element(void);                             // document.activeElement; 0 = none / body

// ---- events: page -> C
typedef struct {
    const char *type;      // "click" "mousedown" "input" "change" "keypress" "keydown" "mouseenter" "mouseover" "focus" "blur" "resize"
    HtmlElem    target;    // element the inline handler / listener is attached to (bubbling already resolved)
    const char *value;     // this.value for form controls, else ""
    int         checked;   // this.checked
    int         key;       // keydown/keypress: GLFW key code natively, DOM keyCode on web (Enter=257/13, Escape=256/27 are normalised: use html_key_is_enter/escape)
    int         button;    // mouse button 0/1/2
    float       x, y;      // CSS px
    int         mods;      // key events: modifier bits as GLFW_MOD_* (1 Shift, 2 Control, 4 Alt, 8 Super)
} HtmlEvent;
int html_key_is_enter(int key);
int html_key_is_escape(int key);

typedef enum { HTML_ARG_NUM, HTML_ARG_STR, HTML_ARG_BOOL, HTML_ARG_THIS, HTML_ARG_THIS_VALUE, HTML_ARG_THIS_CHECKED, HTML_ARG_EVENT } HtmlArgKind;
typedef struct { HtmlArgKind kind; double num; const char *str; } HtmlArg;
// Inline handler `onclick="showWindow(1)"` -> handler "showWindow" with args [{NUM,1}]. `windows[0].changeTab(2)` -> "windows.0.changeTab".
// Return 0 to prevent default (like `return false`), 1 otherwise.
typedef int (*HtmlHandlerFn)(void *user, const HtmlEvent *ev, const HtmlArg *args, int argc);
typedef struct { const char *name; HtmlHandlerFn fn; } HtmlHandlerEntry;
void html_ui_set_handlers(const HtmlHandlerEntry *table, size_t count, void *user, HtmlHandlerFn fallback);
// A handler for markup another module inserts at run time (KrunkScript GAME.UI elements carry
// onclick="knScriptClick()", client/src/ks/ks_ui.c): looked up before the page's table, on the
// game page only. fn NULL removes it.
void html_ui_add_handler(const char *name, HtmlHandlerFn fn);
// Normalised name of the handler being dispatched ("showWindow", "windows.0.changeTab"); NULL outside a
// dispatch. Lets the fallback tell calls apart (its signature carries no name).
const char *html_ui_current_handler(void);
// event.stopPropagation() from C (v7's f1822(window.event) inside a handler): no ancestor's
// inline handler of the same event runs after the current one. The backend consumes the
// request with html_ui_take_stop_propagation() after each dispatch.
void html_ui_stop_propagation(void);
int  html_ui_take_stop_propagation(void);
// Non-inline listeners krunknative.js used addEventListener for. elem_id NULL = window/document.
void html_ui_set_listener(const char *elem_id, const char *type, HtmlHandlerFn fn);
// Dispatch entry used by backends (native events.c / web ccall): resolves the handler by name and calls it.
int  html_ui_dispatch(const char *handler_name, const HtmlEvent *ev, const HtmlArg *args, int argc);
int  html_ui_dispatch_listener(const char *elem_id, const HtmlEvent *ev);

// ---- timers (setTimeout equivalent; fired from html_ui_update on both platforms)
int  html_timer_add(double ms, void (*fn)(void *user), void *user);   // returns id > 0
void html_timer_cancel(int id);
void html_timers_tick(double now_ms);                                 // called by html_ui_update

// ---- game content URLs
// The URL of assets/<path> (e.g. "textures/previews/cosmetics/1_2.png") for an <img src> or a
// CSS url() the C UI writes into the page: Krunker's UTILS.assetsUrl(), whose textures live on
// the asset host, not next to the page. Native "/assets/<path>" (the engine reads it from
// assets_path(), client/src/html/engine/image.c), web the asset host's versioned URL
// (shared/src/utils.c asset_url: Module.krunkNativeAssetBase + assets/<path>?v=<version>).
void html_dom_asset_url(const char *path, char *out, size_t n);

// ---- page address + clipboard
#ifndef KN_PUBLIC_ORIGIN
#define KN_PUBLIC_ORIGIN "https://krunk.io" // the public web client (native invite links point there)
#endif
// The page URL is web only (window.location / window.history): the native window has no
// address bar, so html_page_query() answers -1 and html_page_set_url() does nothing there.
int  html_page_query(char *buf, size_t n);              // location.search without the '?'; -1 natively
void html_page_set_url(const char *href, int push);     // history.replaceState / pushState(document.title, document.title, href)
void html_page_origin(char *buf, size_t n);             // location.protocol + "//" + location.host; native: the public site (KN_PUBLIC_ORIGIN)
void html_clipboard_write(const char *text);            // UTILS.copyToClipboard: web navigator.clipboard, native the OS clipboard

// ---- cross-window channel: web, a BroadcastChannel between same-origin tabs (the editor's
// tabs share their clipboard and project lists over it); natively unavailable (0 / NULL):
// native editor windows talk over the editor bus (shared/include/editor_bus.h). Text messages.
int   html_channel_open(const char *name);                  // 1 when available
void  html_channel_post(const char *name, const char *text); // to every other tab on the channel
char *html_channel_take(const char *name);                  // next received message (malloc'd) or NULL

// ---- storage (localStorage equivalent): native $XDG_CONFIG_HOME/krunknative/<key>.json, web localStorage
int  html_storage_get(const char *key, char *buf, size_t n);          // -1 if absent
void html_storage_set(const char *key, const char *value);
