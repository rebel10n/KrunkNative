// Stub HTML UI backend for minimal builds (KRUNKNATIVE_HTML_ENGINE=OFF, native only):
// no page is loaded, html_ui_init() fails and html_ui_available() is 0, so the client
// falls back to its built-in HUD/menu code. The C UI logic (client/src/ui/*) is still
// compiled and linked -- it keeps the connection state (client_menu_tick) -- so every
// DOM op below is a no-op that reports "no element". Handlers, timers and storage come
// from html_dom_core.c as in the engine build.
#ifndef KRUNKNATIVE_HTML_ENGINE

#include "html_ui_backend.h"
#include <html_dom.h>
#include <string.h>

// ------------------------------------------------------------------ backend

int html_ui_backend_init(const char *ui_dir, const int width, const int height) {
    (void) ui_dir;
    (void) width;
    (void) height;
    return 0;
}

void html_ui_backend_fini(void) {
}

void html_ui_backend_resize(const int width, const int height) {
    (void) width;
    (void) height;
}

void html_ui_backend_update(void) {
}

void html_ui_backend_render(UI *ui, const int layers) {
    (void) ui;
    (void) layers;
}

void html_ui_backend_mouse_move(const double x, const double y) {
    (void) x;
    (void) y;
}

void html_ui_backend_mouse_button(const int glfw_button, const int glfw_action, const int mods) {
    (void) glfw_button;
    (void) glfw_action;
    (void) mods;
}

void html_ui_backend_scroll(const double dx, const double dy) {
    (void) dx;
    (void) dy;
}

void html_ui_backend_key(const int glfw_key, const int scancode, const int glfw_action, const int mods) {
    (void) glfw_key;
    (void) scancode;
    (void) glfw_action;
    (void) mods;
}

void html_ui_backend_char(const unsigned int codepoint) {
    (void) codepoint;
}

int html_ui_backend_load_page(const char *page) {
    (void) page;
    return 0;
}

int html_ui_backend_add_stylesheet(const char *href) {
    (void) href;
    return 0;
}

int html_ui_backend_set_custom_css(const char *css) {
    (void) css;
    return 0;
}

int html_ui_backend_available(void) {
    return 0;
}

// ------------------------------------------------------------------ DOM ops (html_dom.h)

static void clear_buf(char *buf, const size_t n) {
    if (buf && n) buf[0] = 0;
}

HtmlElem html_dom_root(void) { return 0; }
HtmlElem html_dom_body(void) { return 0; }
HtmlElem html_dom_by_id(const char *id) { (void) id; return 0; }
HtmlElem html_dom_query(const HtmlElem root, const char *selector) { (void) root; (void) selector; return 0; }

int html_dom_query_all(const HtmlElem root, const char *selector, HtmlElem *out, const int max) {
    (void) root;
    (void) selector;
    (void) out;
    (void) max;
    return 0;
}

HtmlElem html_dom_parent(const HtmlElem e) { (void) e; return 0; }
HtmlElem html_dom_first_child(const HtmlElem e) { (void) e; return 0; }
HtmlElem html_dom_next(const HtmlElem e) { (void) e; return 0; }
HtmlElem html_dom_closest(const HtmlElem e, const char *selector) { (void) e; (void) selector; return 0; }
int html_dom_child_count(const HtmlElem e) { (void) e; return 0; }
int html_dom_tag(const HtmlElem e, char *buf, const size_t n) { (void) e; clear_buf(buf, n); return 0; }

HtmlElem html_dom_create(const char *tag, const char *id_or_null) { (void) tag; (void) id_or_null; return 0; }
HtmlElem html_dom_create_text(const char *text) { (void) text; return 0; }
void html_dom_append(const HtmlElem parent, const HtmlElem child) { (void) parent; (void) child; }

void html_dom_insert_html(const HtmlElem ref, const HtmlInsertPos pos, const char *html) {
    (void) ref;
    (void) pos;
    (void) html;
}

void html_dom_remove(const HtmlElem e) { (void) e; }
void html_dom_set_html(const HtmlElem e, const char *html) { (void) e; (void) html; }
void html_dom_set_text(const HtmlElem e, const char *text) { (void) e; (void) text; }
int html_dom_get_text(const HtmlElem e, char *buf, const size_t n) { (void) e; clear_buf(buf, n); return 0; }
int html_dom_map_text(const HtmlElem root, const char *skip, HtmlTextMapFn fn, void *user) { (void) root; (void) skip; (void) fn; (void) user; return 0; }
void html_dom_set_lang(const char *lang) { (void) lang; }
int html_dom_missing_glyphs(const HtmlElem e, uint32_t *first_cp) { (void) e; if (first_cp) *first_cp = 0; return -1; }

void html_dom_set_attr(const HtmlElem e, const char *name, const char *value) { (void) e; (void) name; (void) value; }
void html_dom_canvas_put(const HtmlElem e, const unsigned char *rgba, const int w, const int h) { (void) e; (void) rgba; (void) w; (void) h; }
void html_dom_canvas_gl(const HtmlElem e, const unsigned tex, const unsigned fbo, const int w, const int h) { (void) e; (void) tex; (void) fbo; (void) w; (void) h; }
void html_dom_remove_attr(const HtmlElem e, const char *name) { (void) e; (void) name; }
int html_dom_get_attr(const HtmlElem e, const char *name, char *buf, const size_t n) { (void) e; (void) name; clear_buf(buf, n); return -1; }
void html_dom_set_class(const HtmlElem e, const char *cls, const int on) { (void) e; (void) cls; (void) on; }
int html_dom_has_class(const HtmlElem e, const char *cls) { (void) e; (void) cls; return 0; }
void html_dom_set_display(const HtmlElem e, const char *value) { (void) e; (void) value; }
void html_dom_set_style(const HtmlElem e, const char *prop, const char *value) { (void) e; (void) prop; (void) value; }
int html_dom_get_style(const HtmlElem e, const char *prop, char *buf, const size_t n) { (void) e; (void) prop; clear_buf(buf, n); return -1; }

void html_dom_set_value(const HtmlElem e, const char *value) { (void) e; (void) value; }
int html_dom_get_value(const HtmlElem e, char *buf, const size_t n) { (void) e; clear_buf(buf, n); return 0; }
void html_dom_set_checked(const HtmlElem e, const int checked) { (void) e; (void) checked; }
int html_dom_get_checked(const HtmlElem e) { (void) e; return 0; }
void html_dom_focus(const HtmlElem e) { (void) e; }
void html_dom_blur(const HtmlElem e) { (void) e; }
void html_dom_scroll_to_bottom(const HtmlElem e) { (void) e; }
int html_dom_get_scroll(const HtmlElem e, HtmlScroll *out) {
    (void) e;
    if (out) *out = (HtmlScroll) {0};
    return 0;
}
void html_dom_set_scroll(const HtmlElem e, const float left, const float top) {
    (void) e;
    (void) left;
    (void) top;
}
int html_dom_text_selection(const HtmlElem e, int *start, int *end) {
    (void) e;
    if (start) *start = 0;
    if (end) *end = 0;
    return 0;
}
void html_dom_text_set_selection(const HtmlElem e, const int start, const int end) { (void) e; (void) start; (void) end; }
void html_dom_text_replace(const HtmlElem e, const int start, const int end, const char *text) { (void) e; (void) start; (void) end; (void) text; }
int html_dom_text_caret_rect(const HtmlElem e, HtmlRect *out) {
    (void) e;
    if (out) memset(out, 0, sizeof(*out));
    return 0;
}
void html_dom_code_view(const HtmlElem e, const HtmlCodeView *cfg) { (void) e; (void) cfg; }
void html_dom_code_spans(const HtmlElem e, const HtmlCodeSpan *spans, const int n) { (void) e; (void) spans; (void) n; }
void html_dom_code_lines(const HtmlElem e, const HtmlCodeLine *lines, const int n) { (void) e; (void) lines; (void) n; }

int html_dom_get_rect(const HtmlElem e, HtmlRect *out) {
    (void) e;
    if (out) *out = (HtmlRect) {0};
    return 0;
}

int html_dom_is_shown(const HtmlElem e) { (void) e; return 0; }

float html_dom_offset_width(const HtmlElem e) { (void) e; return 0.0f; }
float html_dom_offset_height(const HtmlElem e) { (void) e; return 0.0f; }

HtmlElem html_dom_element_at(const float x, const float y) {
    (void) x;
    (void) y;
    return 0;
}

HtmlElem html_dom_painted_at(const float x, const float y) {
    (void) x;
    (void) y;
    return 0;
}

void html_dom_set_top_layer(const HtmlElem e) { (void) e; }

HtmlElem html_dom_active_element(void) { return 0; }

void html_dom_viewport(float *w, float *h, float *dpr) {
    if (w) *w = 0.0f;
    if (h) *h = 0.0f;
    if (dpr) *dpr = 1.0f;
}

void html_dom_observe_resize(const HtmlElem e, const HtmlResizeFn fn, void *user) { (void) e; (void) fn; (void) user; }

#else
typedef int html_ui_stub_backend_not_compiled;
#endif
