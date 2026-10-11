// Backend-independent part of the HTML UI layer: init/fini bookkeeping, menu/ready
// state and input forwarding to the compiled-in backend. See html_ui.h and
// doc/html-ui-engine.md. The DOM-ops half (handlers, listeners, timers, storage) is
// client/src/html/html_dom_core.c.
#include "html_ui_backend.h"

#include <html_dom.h>
#include <stdio.h>
#include <string.h>

static int g_initialized;
static int g_ready;
static int g_menu_open;
static char g_page[128] = "index.html";
static unsigned g_page_serial;

// html_dom_core.c: per-page handler/listener/timer contexts.
void html_dom_core_select_page(int base_page, int fresh);

int html_ui_init(void *user, const char *ui_dir, const int width, const int height) {
    (void) user;
    if (g_initialized) html_ui_fini();

    g_ready = 0;
    snprintf(g_page, sizeof(g_page), "%s", "index.html");
    html_dom_core_select_page(1, 0);

    if (!html_ui_backend_init(ui_dir ? ui_dir : "./ui/", width > 0 ? width : 1, height > 0 ? height : 1)) {
        return 0;
    }

    g_initialized = 1;
    return 1;
}

void html_ui_fini(void) {
    if (g_initialized) html_ui_backend_fini();

    g_initialized = 0;
    g_ready = 0;
}

void html_ui_resize(const int width, const int height) {
    if (!g_initialized || width <= 0 || height <= 0) return;
    html_ui_backend_resize(width, height);
}

void html_ui_update(void) {
    if (g_initialized) html_ui_backend_update();
}

void html_ui_render(UI *ui) {
    if (g_initialized) html_ui_backend_render(ui);
}

void html_ui_set_menu_open(const int open) {
    g_menu_open = !!open;
}

int html_ui_is_menu_open(void) {
    return g_menu_open;
}

int html_ui_is_ready(void) {
    return g_ready;
}

void html_ui_mouse_move(const double x, const double y) {
    if (g_initialized) html_ui_backend_mouse_move(x, y);
}

void html_ui_mouse_button(const int glfw_button, const int glfw_action, const int mods) {
    if (g_initialized) html_ui_backend_mouse_button(glfw_button, glfw_action, mods);
}

void html_ui_scroll(const double dx, const double dy) {
    if (g_initialized) html_ui_backend_scroll(dx, dy);
}

void html_ui_key(const int glfw_key, const int scancode, const int glfw_action, const int mods) {
    if (g_initialized) html_ui_backend_key(glfw_key, scancode, glfw_action, mods);
}

void html_ui_char(const unsigned int codepoint) {
    if (g_initialized) html_ui_backend_char(codepoint);
}

int html_ui_available(void) {
    return html_ui_backend_available();
}

// --- backend -> dispatcher -------------------------------------------------------

void html_ui_page_ready(void) {
    g_ready = 1;
    g_page_serial++;
}

// --- pages ------------------------------------------------------------------------

int html_ui_add_stylesheet(const char *href) {
    if (!g_initialized || !href || !*href) return 0;
    return html_ui_backend_add_stylesheet(href);
}

int html_ui_load_page(const char *page) {
    if (!g_initialized || !page || !*page) return 0;
    const int base = !strcmp(page, "index.html");
    // Select the target page's DOM context first: the backend may fire events (resize)
    // into it while the document loads. A failed load shows the game page again.
    char previous[sizeof(g_page)];
    snprintf(previous, sizeof(previous), "%s", g_page);
    html_dom_core_select_page(base, !base);
    snprintf(g_page, sizeof(g_page), "%s", page);
    g_ready = 0;
    fprintf(stderr, "[ui] page %s -> %s\n", previous, page);
    if (!html_ui_backend_load_page(page)) {
        html_dom_core_select_page(1, 0);
        snprintf(g_page, sizeof(g_page), "%s", "index.html");
        g_ready = 1;
        return 0;
    }
    return 1;
}

const char *html_ui_current_page(void) {
    return g_page;
}

unsigned html_ui_page_serial(void) {
    return g_page_serial;
}
