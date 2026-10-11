#pragma once
// Client "app modes": the game (ui/index.html, the menu + HUD over the 3D world) and the
// other Krunker pages ported into the client, each a page of ui/ driven by its own C
// logic: the social hub (ui/social.html, client/src/ui/social_ui.c), the map editor
// (ui/editor.html) and the model viewer (ui/viewer.html). Krunker opens those in new
// browser tabs; KrunkNative switches the HTML layer to the page (html_ui_load_page) and
// back. doc/html-ui-engine.md "Pages".
//
// While a page is shown the game is frozen behind it: client_tick() skips the network
// queue, game tick, menu/HUD logic and world rendering (net packets queue up and are
// processed on return), the pointer stays free, and app_mode.c's GLFW callbacks replace
// main.c's (restored on return). The game page's DOM keeps its state.
//
// Entry points: `client_bin --hub|--editor|--viewer [query]`; on the web each page is its
// own URL like on Krunker (social.html?p=profile&q=x, editor.html, viewer.html?...), each
// loading the same client.js, which boots in the mode of its file name; Krunker's links
// to /social.html, /editor.html, /viewer.html (app_route_url, from ui_open_external) and
// app_set_mode() from any UI logic.
#include <client.h>
#include <cJSON.h>

typedef enum {
    APP_MODE_GAME,
    APP_MODE_HUB,
    APP_MODE_EDITOR,
    APP_MODE_VIEWER,
    APP_MODE_COUNT
} AppMode;

// One page module. Every hook but `file` and `enter` may be NULL.
typedef struct AppPage {
    const char *file; // page inside ui/: "social.html", "editor.html", "viewer.html"
    // A fresh document is ready (html_ui_is_ready(), html_dom_* work). `query` is the page's
    // URL parameters without '?' ("p=profile&q=Krunker"; CLI `--hub "<query>"`, web
    // location.search), "" when none. Install the page's handler table
    // (html_ui_set_handlers) and listeners here: each page has its own context, dropped
    // when the page is left (html_dom_core.c).
    void (*enter)(Client *client, const char *query);
    // Before the document is destroyed: free the page's own GL objects/meshes.
    void (*leave)(Client *client);
    // Per frame after html_ui_update(), before the page is composited. The framebuffer was
    // cleared (black), viewport = whole framebuffer; restore any viewport/scissor you change.
    void (*tick)(Client *client, float now, float delta);
    // Input in framebuffer pixels; return 1 when consumed (not forwarded to the page).
    int (*mouse_button)(Client *client, int button, int action, int mods, double x, double y);
    int (*cursor)(Client *client, double x, double y);
    int (*scroll)(Client *client, double dx, double dy);
    int (*key)(Client *client, int key, int scancode, int action, int mods);
    int (*chr)(Client *client, unsigned int codepoint);
    void (*resize)(Client *client, int fb_width, int fb_height);
} AppPage;

// Page providers (one per page module; the table is in app_mode.c).
const AppPage *social_app_page(void);
const AppPage *editor_app_page(void);
const AppPage *viewer_app_page(void);

AppMode app_mode(void);
const char *app_mode_name(AppMode mode); // "game", "hub", "editor", "viewer"
// Switch at the start of the next frame (safe from handlers). `query` as for enter();
// NULL = "". APP_MODE_GAME returns to the game menu. On the web every page is its own
// document (index.html, social.html, editor.html, viewer.html): after startup this
// navigates the browser tab to the page's URL instead.
void app_set_mode(Client *client, AppMode mode, const char *query);
// "/social.html?p=profile&q=x", "./editor.html", "https://krunker.io/viewer.html", "/" ...:
// opens that page and returns 1; 0 for any other URL. Natively the page replaces the
// current one in the window; on the web it is a browser navigation, in a new tab when
// `new_tab` (Krunker's window.open(url, "_blank") / openURL / target=_blank links), else
// in the same tab (openLink(url) / location). app_route_url() = new tab.
int app_route_url_target(const char *url, int new_tab);
int app_route_url(const char *url);

// Editor "Test Map" (Krunker opens the game in a new tab with the map): back to the game
// page and play `map` (a Krunker map JSON object; ownership passes) in the offline game.
// The map is kept in html_storage key "knTestMap" so the web build (where the game page is
// a fresh document: index.html?offline=1&testMap=1) and later sessions can pick it up.
// `return_to` is the page the test came from (APP_MODE_EDITOR). Hosting needs the offline
// game's custom-map hook (LOCAL_GAME_HAVE_HOST_CUSTOM, client/src/net/local_game.c);
// without it the game page just opens.
#define APP_HAVE_TEST_MAP 1
void app_test_map(Client *client, cJSON *map, AppMode return_to);

// --- main.c hooks ---------------------------------------------------------------
// Removes --hub/--editor/--viewer [query] from argv (before the normal parsing); the web
// build boots in the mode of its page URL (social.html, editor.html, viewer.html).
void app_mode_parse_args(int *argc, char **argv);
// Once, after html_ui_init(): enters the startup page, if any.
void app_mode_start(Client *client);
// 0 when the startup page has no use for the game server: on the web, editor.html and
// viewer.html (a fresh document per page, the game is never shown in that tab). Natively
// always 1 (the game page comes back in place and keeps its connection).
int app_mode_needs_server(void);
// From client_tick() right after html_ui_update(): runs a pending switch and, while a
// page is shown, the whole frame (clear, page tick, page composite). Returns 1 when the
// frame belonged to the page (client_tick returns), 0 in game mode.
int app_mode_frame(Client *client, float now, float delta);
