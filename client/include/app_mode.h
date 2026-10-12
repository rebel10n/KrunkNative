#pragma once
// Client "app modes": the game (ui/index.html, the menu + HUD over the 3D world) and the
// other Krunker pages ported into the client, each a page of ui/ driven by its own C
// logic: the social hub (ui/social.html, client/src/ui/social_ui.c), the map editor
// (ui/editor.html) and the model viewer (ui/viewer.html). doc/html-ui-engine.md "Pages".
//
// Like Krunker, every page opens in its own window and leaves the game alone: natively a
// client_bin process per page kind (app_window.c, the v3.1.1 Electron client's windows),
// on the web a page URL of its own (social.html, editor.html, viewer.html; a new tab where
// Krunker opens one). A page process starts like the game (`--hub|--editor|--viewer
// <query>`), then app_mode.c shows its page instead of the menu: client_tick() hands the
// frame to app_mode_frame() (no game tick, menu/HUD logic or world render), the pointer
// stays free and app_mode.c's GLFW callbacks replace main.c's. Links to the page's own
// kind reload it in place.
//
// The editor is the exception inside its own window: it opens several maps at once (project
// tabs), detached 3D views of them as extra GLFW windows sharing its GL context, and joins
// other editor processes and tools over the editor bus (client/src/editor/editor_project.c,
// editor_view.c, editor_bus_link.c; `--editor [flags] [maps...]`, `--help`).
//
// Entry points: `client_bin --hub|--editor|--viewer [query]` (`--game [query]` for a game
// window); the web pages' URLs; Krunker's links to /social.html, /editor.html,
// /viewer.html, / (app_route_url, from ui_open_external) and app_set_mode() /
// app_mode_open_window() from any UI logic.
#include <client.h>
#include <cJSON.h>
#include <stdio.h>

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
    // Optional: 1 once the page has settled (its startup test scripts ran), so
    // KRUNKNATIVE_SCREENSHOT / KRUNKNATIVE_INPUT_SCRIPT start on it (debug_capture.c,
    // input_script.c). NULL = settled as soon as it is entered.
    int (*ready)(Client *client);
} AppPage;

// Page providers (one per page module; the table is in app_mode.c).
const AppPage *social_app_page(void);
const AppPage *editor_app_page(void);
const AppPage *viewer_app_page(void);

AppMode app_mode(void);
// 1 when this window shows a page (not the game) that was entered and reports ready
// (AppPage.ready): page-mode screenshots and input scripts wait for it.
int app_mode_page_ready(Client *client);
const char *app_mode_name(AppMode mode); // "game", "hub", "editor", "viewer"
// Switch THIS window's page at the start of the next frame (safe from handlers); `query`
// as for enter(), NULL = "". Natively used for the startup page and in-place reloads; to
// open another page use app_mode_open_window() / app_route_url(). On the web, after
// startup, this navigates the browser tab to the page's URL.
void app_set_mode(Client *client, AppMode mode, const char *query);
// "/social.html?p=profile&q=x", "./editor.html", "https://krunker.io/viewer.html", "/" ...:
// opens that page and returns 1; 0 for any other URL. Natively the page opens in its own
// window (app_window.c; this window's own page kind reloads in place); on the web it is a
// browser navigation, in a new tab when
// `new_tab` (Krunker's window.open(url, "_blank") / openURL / target=_blank links), else
// in the same tab (openLink(url) / location). app_route_url() = new tab.
int app_route_url_target(const char *url, int new_tab);
int app_route_url(const char *url);
// location.reload() (url NULL) / location.href = url: the v3.1.1 mod's F5 / F4 (only the
// game page has them). The web build reloads / navigates the tab. Natively the window has
// no URL, so both restart the client like a browser reload: the process re-executes
// itself with its command line (page switches like --editor dropped: the game page).
// Returns only when that failed.
void app_reload(const char *url);

// Editor "Test Map" (Krunker opens the game with the map): the game window (natively: the
// existing one is raised, else a new one opens with `--game testMap=1`; web:
// index.html?offline=1&testMap=1) plays `map` (a Krunker map JSON object; ownership
// passes) in the offline game. The map travels through html_storage key "knTestMap"
// (shared config dir).
// `return_to` is the page the test came from (APP_MODE_EDITOR). Hosting needs the offline
// game's custom-map hook (LOCAL_GAME_HAVE_HOST_CUSTOM, client/src/net/local_game.c);
// without it the game page just opens.
#define APP_HAVE_TEST_MAP 1
void app_test_map(Client *client, cJSON *map, AppMode return_to);

// --- windows (native, client/src/app_window.c) -------------------------------------
// Natively every page has its own window (a client_bin process per page kind, like the
// v3.1.1 Electron client's windows): opening a page whose window exists reloads it with
// the query and raises it, else a new window opens; the game window is left alone.
void app_window_init(const char *argv0);
void app_window_open(Client *client, AppMode mode, const char *query);
void app_window_poll(Client *client);   // per frame: messages between the windows, closed windows
void app_window_fini(void);
void app_window_place(Client *client);   // KRUNKNATIVE_WINDOW_POS from the opener (offset from it)
int  app_window_has_router(void);        // 1 in a window opened by another one
// Called by app_window.c: open `mode` with `query` from this process's point of view (its
// own page reloads in place, other pages go to their windows), and a reload request for
// this window's own page.
void app_mode_open_window(Client *client, AppMode mode, const char *query);
void app_mode_navigate_self(Client *client, const char *query);

// --- main.c hooks ---------------------------------------------------------------
// Removes --game/--hub/--editor/--viewer [query] from argv (before the normal parsing); the web
// build boots in the mode of its page URL (social.html, editor.html, viewer.html).
void app_mode_parse_args(int *argc, char **argv);
// The editor's command line (client/src/editor): `--editor [flags] [maps...]` hands each map
// file to editor_cli_add_file (one project each) and the bus flags to editor_bus_parse_arg
// (returns 1 and advances *i past what it consumed). editor_usage() prints them.
void editor_cli_add_file(const char *path);
int editor_bus_parse_arg(int argc, char **argv, int *i);
void editor_usage(FILE *out);
// Once, after html_ui_init(): enters the startup page, if any.
void app_mode_start(Client *client);
// 0 when this window's page has no use for the game server: the editor and the viewer
// (their window / tab never shows the game). The hub keeps its connection for its data.
int app_mode_needs_server(void);
// From client_tick() right after html_ui_update(): runs a pending switch and, while a
// page is shown, the whole frame (clear, page tick, page composite). Returns 1 when the
// frame belonged to the page (client_tick returns), 0 in game mode.
int app_mode_frame(Client *client, float now, float delta);
