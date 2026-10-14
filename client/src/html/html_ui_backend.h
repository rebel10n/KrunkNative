#pragma once
// Internal contract between the dispatcher (html_ui.c) and the compiled-in backend:
// engine/engine.c natively, web_dom.c under Emscripten, stub.c when the engine is OFF.
// Not part of the public API. The DOM operations themselves (html_dom.h) are
// implemented by the same backend (engine/dom_ops.c, web_dom.c, stub.c).
#include <html_ui.h>

// Implemented by the backend.
int  html_ui_backend_init(const char *ui_dir, int width, int height);
void html_ui_backend_fini(void);
void html_ui_backend_resize(int width, int height);
void html_ui_backend_update(void);
void html_ui_backend_render(UI *ui, int layers); // HTML_LAYER_PAGE | HTML_LAYER_TOP (html_ui.h)
void html_ui_backend_mouse_move(double x, double y);
void html_ui_backend_mouse_button(int glfw_button, int glfw_action, int mods);
void html_ui_backend_scroll(double dx, double dy);
void html_ui_backend_key(int glfw_key, int scancode, int glfw_action, int mods);
void html_ui_backend_char(unsigned int codepoint);
int  html_ui_backend_available(void);
// Show another page of ui_dir (html_ui_load_page). Natively synchronous; on the web the
// page is fetched and swapped in asynchronously. Either way the backend calls
// html_ui_page_ready() once the new document can be queried. Returns 0 on failure
// (the current page stays).
int  html_ui_backend_load_page(const char *page);
// html_ui_add_stylesheet on the current document (1 = loaded or already there).
int  html_ui_backend_add_stylesheet(const char *href);
// html_ui_set_custom_css on the game page (NULL/"" removes the sheet).
int  html_ui_backend_set_custom_css(const char *css);

// Implemented by the dispatcher, called by the backend (main thread only) once the
// page's DOM exists and the UI logic may query it: html_ui_is_ready() returns 1 from
// then on (until html_ui_fini()).
void html_ui_page_ready(void);
