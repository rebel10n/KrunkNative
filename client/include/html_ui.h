#pragma once
// HTML/CSS UI layer (doc/html-ui-engine.md). The page is ui/index.html (Krunker's own
// menu markup + CSS); the C UI logic in client/src/ui drives it through the DOM-ops API
// (html_dom.h). No JavaScript runs natively.
//
// One API, three backends selected at compile time:
//   - client/src/html/engine/engine.c  native, the in-house HTML/CSS engine
//                                      (KRUNKNATIVE_HTML_ENGINE=ON, the default)
//   - client/src/html/web_dom.c        Emscripten: the browser page is the UI
//   - client/src/html/stub.c           KRUNKNATIVE_HTML_ENGINE=OFF: every call is a no-op
//
// client/src/html/html_ui.c is the backend-independent dispatcher (init state,
// menu/ready flags, input forwarding).
//
// Threading: every html_ui_* function must be called from the main (GL/GLFW) thread.
#include <client.h>

// Create the renderer and load <ui_dir>/index.html. `ui_dir` may be relative to the
// working directory ("../ui/" for dev builds run from cmake-build-debug, "./ui/" for
// release). `width`/`height` are the framebuffer size in pixels. `user` is reserved
// (the UI logic keeps its own Client pointer). Returns 0 on failure (also when built
// without HTML UI support).
int  html_ui_init(void *user, const char *ui_dir, int width, int height);
void html_ui_fini(void);

// Call when the framebuffer size changes (pixels).
void html_ui_resize(int width, int height);

// Pump the HTML layer: page events -> handlers (html_dom.h), resize observers and the
// setTimeout-style timers. Call once per frame before rendering.
void html_ui_update(void);

// Composite the page over the current framebuffer (native engine; no-op on the web,
// where the browser composites the page over the canvas). The engine restyles/lays
// out/repaints only what changed and resubmits cached display lists otherwise. Call
// after the 3D scene and the native HUD have been drawn. Leaves depth test disabled
// and blending enabled, like the other UI helpers.
// Debugging: KRUNKNATIVE_HTML_PROFILE=1 prints per-stage timings every ~2 s,
// KRUNKNATIVE_HTML_VERBOSE=1 logs loads and failures (client/src/html/engine/README.md).
void html_ui_render(UI *ui);

// Menu state as the client sees it. The UI logic (client_set_menu_open in
// client/src/ui/menu_ui.c) records it here and updates the page itself.
void html_ui_set_menu_open(int open);
int  html_ui_is_menu_open(void);

// 1 once the backend has a page whose DOM the UI logic may query.
int  html_ui_is_ready(void);

// Native input forwarding (no-ops on web and stub). Coordinates are in the same
// pixel space as html_ui_resize()/html_ui_init(), i.e. framebuffer pixels: scale
// GLFW cursor coordinates by framebuffer/window size on HiDPI displays. Only
// forward input while html_ui_is_menu_open() is true.
void html_ui_mouse_move(double x, double y);
void html_ui_mouse_button(int glfw_button, int glfw_action, int mods);
void html_ui_scroll(double dx, double dy);
void html_ui_key(int glfw_key, int scancode, int glfw_action, int mods);
void html_ui_char(unsigned int codepoint);

// 1 when a real backend is compiled in, 0 for the stub (KRUNKNATIVE_HTML_ENGINE=OFF).
int  html_ui_available(void);

// --- Pages (doc/html-ui-engine.md "Pages") -----------------------------------------
// html_ui_init() loads the game page, index.html. html_ui_load_page() shows another
// page of the same ui_dir ("social.html", "editor.html", "viewer.html") or the game page
// again. The game page's document stays alive with its DOM state while another page is
// shown; every other page gets a fresh document each time and is destroyed when left.
// Each page has its own handler table, listeners and timers (html_dom_core.c): loading
// a page selects its context (a fresh one for a fresh document), so the page's UI logic
// installs its handlers after html_ui_is_ready() turns 1 again. Natively the switch is
// immediate; on the web the page is fetched first (html_ui_is_ready() is 0 meanwhile).
// Client code switches pages through app_mode.h, not directly. Returns 0 on failure.
int  html_ui_load_page(const char *page);
// File name of the page shown (or being loaded), "index.html" after html_ui_init().
const char *html_ui_current_page(void);
// Bumped every time a document becomes ready (init, each page load, the return to the
// game page): UI logic compares it with the serial it last set up to detect a new page.
unsigned html_ui_page_serial(void);

// A stylesheet added to the current page at runtime, like a <link rel=stylesheet href>
// appended to <head> (page mods inject theirs this way, e.g. the developer console's
// ui/css/devconsole.css). `href` is relative to the page. Its @font-face/@keyframes
// register and the whole page restyles. Adding the same href again is a no-op.
// Returns 1 when the sheet is (now) part of the page.
int  html_ui_add_stylesheet(const char *href);
