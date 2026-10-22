// Client app modes (app_mode.h): the game page and the Krunker pages ported into the
// client (social hub, editor, viewer). Owns the page switch, the per-mode frame and the
// GLFW callbacks while a page is shown. doc/html-ui-engine.md "Pages".
#include <gl.h> // glad natively, GLES3 on the web (before GLFW)
#include <app_mode.h>
#include <app_url.h> // the page URL split (parse_page_url)
#include <i18n.h> // the page markup in the chosen language
#include <backend_ui.h>
#include <download_ui.h> // ?download
#include <debug_capture.h>
#include <map_environment.h> // the game's environment back after a page (leave_page)
#include <input_script.h>
#include <render_settings.h> // client_render_toggle_fullscreen (F11)
#include <html_ui.h>
#include <html_dom.h>
#include <ui_logic.h> // menu_close_window (Test Map) // html_storage_get/set (knTestMap)
#include <viewer_embed.h> // the hub's item view showcase
#include <console.h>      // the developer console on every page
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#elif defined(_WIN32)
#include <process.h> // _execv
#include <windows.h> // GetModuleFileNameA
#else
#include <fcntl.h>
#include <limits.h>
#include <unistd.h>
#endif

#define APP_QUERY_MAX 1024

typedef struct {
    AppMode mode;
    const AppPage *(*get)(void);
} AppPageEntry;

// One line per page module (the providers live in the modules themselves).
static const AppPageEntry k_pages[] = {
    {APP_MODE_HUB, social_app_page},
    {APP_MODE_EDITOR, editor_app_page},
    {APP_MODE_VIEWER, viewer_app_page},
};

static const char *const k_mode_names[APP_MODE_COUNT] = {"game", "hub", "editor", "viewer"};

typedef struct {
    Client *client;
    AppMode mode;
    const AppPage *page;        // NULL in game mode
    int entered;                // page->enter ran for the current document
    unsigned serial;            // html_ui_page_serial() of the document entered
    char query[APP_QUERY_MAX];

    int pending;                // app_set_mode() waiting for the next frame
    AppMode pending_mode;
    char pending_query[APP_QUERY_MAX];

    int returning;              // back to the game page, waiting for it to be ready
    int started;                // app_mode_start ran (web: later switches navigate)
    int test_pending;           // app_test_map: host knTestMap once the game page is back
    AppMode test_return;
    int callbacks_swapped;
    GLFWframebuffersizefun prev_resize;
    GLFWcursorposfun prev_cursor;
    GLFWmousebuttonfun prev_mouse;
    GLFWscrollfun prev_scroll;
    GLFWkeyfun prev_key;
    GLFWcharfun prev_char;
} AppState;

static AppState g_app;

#ifdef __EMSCRIPTEN__
static void web_navigate(AppMode mode, const char *query, int new_tab);
#endif

// Startup page from the command line / URL (app_mode_parse_args).
static AppMode g_start_mode = APP_MODE_GAME;
#ifndef __EMSCRIPTEN__
static char **g_exec_argv; // app_reload: argv after app_mode_parse_args, NULL-terminated
#endif
static char g_start_query[APP_QUERY_MAX];

static const AppPage *page_for(const AppMode mode) {
    for (size_t i = 0; i < sizeof(k_pages) / sizeof(k_pages[0]); i++) {
        if (k_pages[i].mode == mode) return k_pages[i].get();
    }
    return NULL;
}

AppMode app_mode(void) {
    return g_app.mode;
}

const char *app_mode_name(const AppMode mode) {
    return mode >= 0 && mode < APP_MODE_COUNT ? k_mode_names[mode] : "?";
}

void app_set_mode(Client *client, const AppMode mode, const char *query) {
    if (mode < 0 || mode >= APP_MODE_COUNT) return;
    if (client) g_app.client = client;
    if (mode != APP_MODE_GAME && !page_for(mode)) {
        fprintf(stderr, "[app] no page for mode %s in this build\n", app_mode_name(mode));
        return;
    }
#ifdef __EMSCRIPTEN__
    if (g_app.started) {
        web_navigate(mode, query, 0);
        return;
    }
#endif
    g_app.pending = 1;
    g_app.pending_mode = mode;
    snprintf(g_app.pending_query, sizeof(g_app.pending_query), "%s", query ? query : "");
}

// ------------------------------------------------------------------ URLs

// "<prefix>social.html[?query][#...]" -> mode + query.
// The split is app_url.c (client/tests/app_url_test.c); the game keeps its query ("/?play=<map>").
static int parse_page_url(const char *url, AppMode *mode, char *query, const size_t n) {
    static const struct { const char *file; AppMode mode; } files[] = {
        {"social.html", APP_MODE_HUB}, {"editor.html", APP_MODE_EDITOR}, {"viewer.html", APP_MODE_VIEWER},
    };
    const char *page = NULL;
    const int kind = app_url_split(url, &page, query, n);
    if (kind == 1) {
        *mode = APP_MODE_GAME;
        return 1;
    }
    if (kind != 2) return 0;
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        if (!strcmp(page, files[i].file)) {
            *mode = files[i].mode;
            return 1;
        }
    }
    return 0;
}

int app_route_url_target(const char *url, const int new_tab) {
    AppMode mode;
    char query[APP_QUERY_MAX];
    if (!parse_page_url(url, &mode, query, sizeof(query))) return 0;
    if (mode != APP_MODE_GAME && !page_for(mode)) return 0; // not ported in this build: the caller opens the browser
#ifdef __EMSCRIPTEN__
    web_navigate(mode, query, new_tab);
#else
    (void) new_tab; // natively a page always opens in its own window (app_window.c)
    fprintf(stderr, "[app] %s -> %s window\n", url, app_mode_name(mode));
    app_mode_open_window(g_app.client, mode, query);
#endif
    return 1;
}

#ifndef __EMSCRIPTEN__
static void raise_window(Client *client) {
    if (client && client->window) glfwFocusWindow(client->window);
}

static AppMode g_start_mode_fwd(void);

void app_mode_open_window(Client *client, const AppMode mode, const char *query) {
    if (mode == g_start_mode_fwd()) app_mode_navigate_self(client, query); // this window's own page
    else app_window_open(client, mode, query);
}

// "loadURL" of this window: a page reloads with the new query, the game window takes
// its query (testMap=1: the editor's Test Map) and comes to the front.
void app_mode_navigate_self(Client *client, const char *query) {
    const AppMode self = g_start_mode_fwd();
    if (self == APP_MODE_GAME) {
        if (query && strstr(query, "testMap=1")) g_app.test_pending = 1;
        servers_ui_query(query); // ?play=<map> / ?game=<id> (hub Play / Join links; brain, backend_ui.h)
        download_ui_query(query); // ?download: the Download window (the hub's app badges)
    } else {
        app_set_mode(client, self, query);
    }
    raise_window(client);
}
#endif

int app_route_url(const char *url) {
    return app_route_url_target(url, 1);
}

// ------------------------------------------------------------------ editor Test Map

#define APP_TEST_MAP_KEY "knTestMap"

// Hosts the stored test map in the offline game (once the game page is shown).
static void test_map_host(Client *client) {
    g_app.test_pending = 0;
    const int size = html_storage_get(APP_TEST_MAP_KEY, NULL, 0);
    if (size <= 0) return;
    char *json = malloc((size_t) size + 1);
    if (!json) return;
    html_storage_get(APP_TEST_MAP_KEY, json, (size_t) size + 1);
    cJSON *map = cJSON_Parse(json);
    free(json);
    if (!cJSON_IsObject(map)) {
        cJSON_Delete(map);
        fprintf(stderr, "[app] test map: stored map is not a JSON object\n");
        return;
    }
    // Krunker reloads the game tab on the map (a fresh menu): no menu window stays open
    if (menu_window_is_open()) menu_close_window();
#ifdef LOCAL_GAME_HAVE_HOST_CUSTOM
    if (!local_game_host_custom(client, map)) fprintf(stderr, "[app] test map: hosting failed\n");
    else fprintf(stderr, "[app] test map hosted in this window\n");
#else
    (void) client;
    fprintf(stderr, "[app] test map: this build cannot host custom maps offline yet (LOCAL_GAME_HAVE_HOST_CUSTOM)\n");
#endif
    cJSON_Delete(map);
}

void app_test_map(Client *client, cJSON *map, const AppMode return_to) {
    char *json = map ? cJSON_PrintUnformatted(map) : NULL;
    cJSON_Delete(map);
    if (!json) return;
    html_storage_set(APP_TEST_MAP_KEY, json);
    cJSON_free(json);
    g_app.test_return = return_to;
#ifdef __EMSCRIPTEN__
    (void) client;
    web_navigate(APP_MODE_GAME, "offline=1&testMap=1", 0);
#else
    app_mode_open_window(client, APP_MODE_GAME, "testMap=1"); // the game window, like Krunker's
#endif
}

// ------------------------------------------------------------------ startup

static AppMode mode_from_name(const char *name) {
    if (!name) return APP_MODE_GAME;
    if (!strcmp(name, "hub") || !strcmp(name, "social")) return APP_MODE_HUB;
    if (!strcmp(name, "editor")) return APP_MODE_EDITOR;
    if (!strcmp(name, "viewer")) return APP_MODE_VIEWER;
    return APP_MODE_GAME;
}

void app_mode_parse_args(int *argc, char **argv) {
    if (!argc || !argv) return;
#ifndef __EMSCRIPTEN__
    app_window_init(argv[0]); // the executable the page windows run
#endif
    // `--editor [flags] [map files...]`: every map file opens as an editor project and the
    // editor's own flags (--mcp-bus, --bus-dir, --mcp-auto-accept, ...) are taken here
    int editor = 0;
    for (int i = 1; i < *argc; i++) editor |= !strcmp(argv[i], "--editor");
    int out = 1;
    for (int i = 1; i < *argc; i++) {
        const char *arg = argv[i];
        if (editor && editor_bus_parse_arg(*argc, argv, &i)) continue;
        if (editor && arg[0] != '-') {
            const size_t len = strlen(arg);
            if ((len > 5 && !strcmp(arg + len - 5, ".json")) || (len > 4 && !strcmp(arg + len - 4, ".txt"))) {
                editor_cli_add_file(arg);
                continue;
            }
        }
        if (!strcmp(arg, "--game") || !strcmp(arg, "--hub") || !strcmp(arg, "--social") || !strcmp(arg, "--editor") || !strcmp(arg, "--viewer")) {
            g_start_mode = mode_from_name(arg + 2);
            g_start_query[0] = 0;
            // Optional query: the next argument unless it is a flag or a map file.
            if (i + 1 < *argc && argv[i + 1][0] != '-') {
                const char *next = argv[i + 1];
                const size_t len = strlen(next);
                if ((len < 5 || strcmp(next + len - 5, ".json") != 0) && (len < 4 || strcmp(next + len - 4, ".txt") != 0)) {
                    snprintf(g_start_query, sizeof(g_start_query), "%s", next[0] == '?' ? next + 1 : next);
                    i++;
                }
            }
            continue;
        }
        argv[out++] = argv[i];
    }
    argv[out] = NULL;
    *argc = out;
#ifndef __EMSCRIPTEN__
    // app_reload: the same command line minus the page switches (strings outlive main's argv use)
    g_exec_argv = calloc((size_t) out + 1, sizeof(char *));
    for (int i = 0; g_exec_argv && i < out; i++) g_exec_argv[i] = argv[i];
#endif
}

char *const *app_exec_args(void) {
#ifdef __EMSCRIPTEN__
    return NULL;
#else
    return g_exec_argv;
#endif
}

void app_reload_prepare(void) {
#ifndef __EMSCRIPTEN__
    // input-script tests (client/src/input_script.c): the relaunched process plays this script
    // instead of starting the current one (which pressed the key) over
    const char *next_script = getenv("KRUNKNATIVE_INPUT_SCRIPT_RELAUNCH");
    if (next_script && next_script[0]) {
#ifdef _WIN32
        _putenv_s("KRUNKNATIVE_INPUT_SCRIPT", next_script);
        _putenv_s("KRUNKNATIVE_INPUT_SCRIPT_RELAUNCH", "");
#else
        setenv("KRUNKNATIVE_INPUT_SCRIPT", next_script, 1);
        unsetenv("KRUNKNATIVE_INPUT_SCRIPT_RELAUNCH");
#endif
    }
    client_render_window_before_exec(g_app.client); // the new process reopens the window as it is now
    fflush(NULL);
#endif
}

void app_reload(const char *url) {
#ifdef __EMSCRIPTEN__
    if (url) EM_ASM({ location.href = UTF8ToString($0); }, url); // F4: location.href = "/"
    else EM_ASM({ location.reload(); });                          // F5: location.reload()
#else
    (void) url;
    if (!g_exec_argv || !g_exec_argv[0]) return;
    char path[4096] = "";
#ifdef _WIN32
    if (!GetModuleFileNameA(NULL, path, sizeof(path))) snprintf(path, sizeof(path), "%s", g_exec_argv[0]);
#else
    const ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (n > 0) path[n] = 0;
    else if (!realpath(g_exec_argv[0], path)) snprintf(path, sizeof(path), "%s", g_exec_argv[0]);
    // the file this process runs was replaced (an update installed over it): Linux names the old
    // inode "<path> (deleted)"; the file now at <path> is the new build, which is what to run
    static const char deleted[] = " (deleted)";
    const size_t len = strlen(path), dl = sizeof(deleted) - 1;
    if (len > dl && !strcmp(path + len - dl, deleted)) path[len - dl] = 0;
#endif
    fprintf(stderr, "[app] reload: restarting %s\n", path);
    app_reload_prepare();
#ifdef _WIN32
    _execv(path, (const char *const *) g_exec_argv);
#else
    // Sockets, files and the GL/X connection must not leak into the new image (a kept
    // listening socket would block the offline server's port): close them on exec only,
    // so a failed exec leaves this process intact.
    for (int fd = 3; fd < 1024; fd++) {
        const int flags = fcntl(fd, F_GETFD);
        if (flags >= 0) fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
    }
    execv(path, g_exec_argv);
#endif
    perror("[app] reload: exec");
#endif
}

#ifdef __EMSCRIPTEN__
// The web build serves every page at its own URL, like Krunker: index.html (game),
// social.html, editor.html, viewer.html each load the same client.js and boot in the
// mode their file name says; the page's query is location.search.
static char *web_document_file(void) {
    return (char *) EM_ASM_PTR({
        var f = "";
        try { f = location.pathname.substring(location.pathname.lastIndexOf("/") + 1); } catch (e) {}
        if (f && f.indexOf(".") < 0) f += ".html"; // Cloudflare Pages serves social.html as /social
        return stringToNewUTF8(f || "index.html");
    });
}

static void web_start_page(void) {
    char *file = web_document_file();
    char query[APP_QUERY_MAX];
    char url[APP_QUERY_MAX + 32];
    snprintf(url, sizeof(url), "%s", file ? file : "index.html");
    free(file);
    AppMode mode = APP_MODE_GAME;
    if (!parse_page_url(url, &mode, query, sizeof(query))) mode = APP_MODE_GAME;
    g_start_mode = mode;
    char *search = (char *) EM_ASM_PTR({
        var s = "";
        try { s = location.search.substring(1); } catch (e) {}
        return stringToNewUTF8(s);
    });
    snprintf(g_start_query, sizeof(g_start_query), "%s", search ? search : "");
    free(search);
}

// Another page of the site: a normal browser navigation (same tab, or a new tab where
// Krunker opens one), the new document boots its own client.
static void web_navigate(const AppMode mode, const char *query, const int new_tab) {
    static const char *const files[APP_MODE_COUNT] = {"index.html", "social.html", "editor.html", "viewer.html"};
    char url[APP_QUERY_MAX + 32];
    snprintf(url, sizeof(url), "%s%s%s", files[mode], query && *query ? "?" : "", query ? query : "");
    fprintf(stderr, "[app] navigate %s%s\n", url, new_tab ? " (new tab)" : "");
    EM_ASM({
        var u = UTF8ToString($0);
        try {
            // session options of this build (?offline=1 local game, ?ws= server) carry over
            var cur = new URLSearchParams(location.search);
            var add = [];
            "offline ws".split(" ").forEach(function (k) {
                if (cur.has(k) && !new URLSearchParams(u.split("?")[1] || "").has(k)) add.push(k + "=" + encodeURIComponent(cur.get(k)));
            });
            if (add.length) u += (u.indexOf("?") < 0 ? "?" : "&") + add.join("&");
        } catch (e) {}
        try {
            if ($1) (window.knRealOpen || window.open)(u, "_blank");
            else location.href = u;
        } catch (e) {}
    }, url, new_tab);
}
#endif

void app_mode_start(Client *client) {
    g_app.client = client;
    // (a page window is placed before it is shown: client_render_window_created, KRUNKNATIVE_WINDOW_NEAR)
#ifdef __EMSCRIPTEN__
    web_start_page();
#endif
    if (g_start_mode != APP_MODE_GAME) app_set_mode(client, g_start_mode, g_start_query);
    // index.html?offline=1&testMap=1 / --game "testMap=1": the editor's Test Map in a new game window
    if (g_start_mode == APP_MODE_GAME && strstr(g_start_query, "testMap=1")) g_app.test_pending = 1;
    if (g_start_mode == APP_MODE_GAME) servers_ui_query(g_start_query); // ?play= / ?game= (backend_ui.h)
    if (g_start_mode == APP_MODE_GAME) download_ui_query(g_start_query); // ?download (download_ui.h)
    g_app.started = 1;
}

int app_mode_needs_server(void) {
    // the editor/viewer windows (web: pages) never show the game
    return g_start_mode != APP_MODE_EDITOR && g_start_mode != APP_MODE_VIEWER;
}

#ifndef __EMSCRIPTEN__
static AppMode g_start_mode_fwd(void) {
    return g_start_mode;
}
#endif

// ------------------------------------------------------------------ input while a page is shown

static void cursor_scale(GLFWwindow *window, double *x, double *y) {
    int ww, wh, fw, fh;
    glfwGetWindowSize(window, &ww, &wh);
    glfwGetFramebufferSize(window, &fw, &fh);
    if (ww > 0 && wh > 0) {
        *x *= (double) fw / ww;
        *y *= (double) fh / wh;
    }
}

static const AppPage *input_page(void) {
    return g_app.page && g_app.entered ? g_app.page : NULL;
}

static void page_resize_cb(GLFWwindow *window, const int width, const int height) {
    if (g_app.prev_resize) g_app.prev_resize(window, width, height); // glViewport + html_ui_resize
    const AppPage *page = input_page();
    if (page && page->resize) page->resize(g_app.client, width, height);
}

static void page_cursor_cb(GLFWwindow *window, double x, double y) {
    cursor_scale(window, &x, &y);
    if (ui_console_mouse(-1, x, y, 0)) return; // dev console titlebar drag / grip resize
    const AppPage *page = input_page();
    if (page && page->cursor && page->cursor(g_app.client, x, y)) return;
    html_ui_mouse_move(x, y);
}

static void page_mouse_cb(GLFWwindow *window, const int button, const int action, const int mods) {
    double x, y;
    glfwGetCursorPos(window, &x, &y);
    cursor_scale(window, &x, &y);
    if (button == GLFW_MOUSE_BUTTON_LEFT && ui_console_mouse(action, x, y, 0)) return; // the dev console's modal
    const AppPage *page = input_page();
    if (page && page->mouse_button && page->mouse_button(g_app.client, button, action, mods, x, y)) return;
    html_ui_mouse_move(x, y);
    html_ui_mouse_button(button, action, mods);
}

static void page_scroll_cb(GLFWwindow *window, const double dx, const double dy) {
    (void) window;
    const AppPage *page = input_page();
    if (page && page->scroll && page->scroll(g_app.client, dx, dy)) return;
    html_ui_scroll(dx, dy);
}

static void page_key_cb(GLFWwindow *window, const int key, const int scancode, const int action, const int mods) {
    (void) window;
#ifndef __EMSCRIPTEN__
    if (key == GLFW_KEY_F11) { // fullscreen on the editor / viewer pages too (main.c key_callback)
        if (action == GLFW_PRESS && g_app.client) client_render_toggle_fullscreen(g_app.client);
        return;
    }
#endif
    if (ui_console_app_page_key_event(g_app.client, key, scancode, action, mods)) return; // the dev console first, as in the game
    const AppPage *page = input_page();
    if (page && page->key && page->key(g_app.client, key, scancode, action, mods)) return;
    html_ui_key(key, scancode, action, mods);
}

static void page_char_cb(GLFWwindow *window, const unsigned int codepoint) {
    (void) window;
    const AppPage *page = input_page();
    // the open console has the keyboard (its key callback took the keys): its input line types
    if (!ui_console_is_open() && page && page->chr && page->chr(g_app.client, codepoint)) return;
    html_ui_char(codepoint);
}

static void swap_callbacks(GLFWwindow *window, const int to_page) {
    if (!window || to_page == g_app.callbacks_swapped) return;
    if (to_page) {
        g_app.prev_resize = glfwSetFramebufferSizeCallback(window, page_resize_cb);
        g_app.prev_cursor = glfwSetCursorPosCallback(window, page_cursor_cb);
        g_app.prev_mouse = glfwSetMouseButtonCallback(window, page_mouse_cb);
        g_app.prev_scroll = glfwSetScrollCallback(window, page_scroll_cb);
        g_app.prev_key = glfwSetKeyCallback(window, page_key_cb);
        g_app.prev_char = glfwSetCharCallback(window, page_char_cb);
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    } else {
        glfwSetFramebufferSizeCallback(window, g_app.prev_resize);
        glfwSetCursorPosCallback(window, g_app.prev_cursor);
        glfwSetMouseButtonCallback(window, g_app.prev_mouse);
        glfwSetScrollCallback(window, g_app.prev_scroll);
        glfwSetKeyCallback(window, g_app.prev_key);
        glfwSetCharCallback(window, g_app.prev_char);
    }
    g_app.callbacks_swapped = to_page;
}

// ------------------------------------------------------------------ switching

static void leave_page(void) {
    if (g_app.page && g_app.entered && g_app.page->leave) g_app.page->leave(g_app.client);
    // the page may have set the global lighting / sky / shadows for its own view (the editor):
    // the loaded game map's environment comes back for the game page behind it
    if (g_app.page) map_environment_restore();
    g_app.page = NULL;
    g_app.entered = 0;
}

static void apply_pending(Client *client) {
    g_app.pending = 0;
    const AppMode mode = g_app.pending_mode;
    const AppPage *page = mode == APP_MODE_GAME ? NULL : page_for(mode);
    if (mode != APP_MODE_GAME && !page) return;

    leave_page();
    fprintf(stderr, "[app] mode %s -> %s%s%s\n", app_mode_name(g_app.mode), app_mode_name(mode),
            g_app.pending_query[0] ? " ?" : "", g_app.pending_query);
    if (!html_ui_load_page(page ? page->file : "index.html")) {
        // Failed (or no HTML layer): the game page is shown.
        g_app.mode = APP_MODE_GAME;
        g_app.returning = 1;
        return;
    }
    g_app.mode = mode;
    g_app.page = page;
    snprintf(g_app.query, sizeof(g_app.query), "%s", g_app.pending_query);
    if (page) {
        swap_callbacks(client->window, 1);
        g_app.returning = 0;
    } else {
        g_app.returning = 1;
    }
}

int app_mode_page_ready(Client *client) {
    if (g_app.mode == APP_MODE_GAME || !g_app.page || !g_app.entered) return 0;
    return g_app.page->ready ? g_app.page->ready(client) : 1;
}

int app_mode_frame(Client *client, const float now, const float delta) {
    g_app.client = client;
#ifndef __EMSCRIPTEN__
    app_window_poll(client);
#endif
    if (g_app.pending) apply_pending(client);

    if (g_app.mode == APP_MODE_GAME) {
        if (!g_app.returning) {
            if (g_app.test_pending && html_ui_is_ready()) test_map_host(client);
            return 0;
        }
        if (!html_ui_is_ready()) {
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            return 1; // web: the game page is being restored
        }
        g_app.returning = 0;
        swap_callbacks(client->window, 0);
        client_set_menu_open(client, 1); // back in the menu, pointer free
        return 0;
    }

    if (!g_app.entered && html_ui_is_ready() && g_app.page) {
        g_app.entered = 1;
        g_app.serial = html_ui_page_serial();
        g_app.page->enter(client, g_app.query);
        // the hub / editor / viewer markup in the chosen language (the strings their own C writes
        // are English: ui/i18n/README.md "Gaps")
        html_dom_set_lang(i18n_lang_find(i18n_locale())->bcp47);
        i18n_translate_tree(0);
#ifndef __EMSCRIPTEN__
        // the window takes the page's <title> ("Krunker Hub", ...), as Electron's windows do
        const HtmlElem title = html_dom_query(0, "title");
        char text[128] = "";
        if (title && html_dom_get_text(title, text, sizeof(text)) > 0 && client->window) glfwSetWindowTitle(client->window, text);
#endif
    }

    int fb_w, fb_h;
    glfwGetFramebufferSize(client->window, &fb_w, &fb_h);
    glViewport(0, 0, fb_w, fb_h);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    input_script_page_tick(client); // KRUNKNATIVE_INPUT_SCRIPT on a page (keys / mouse / shot)
    if (g_app.entered && g_app.page && g_app.page->tick) g_app.page->tick(client, now, delta);
    if (g_app.entered) ui_console_tick(client); // the developer console on every page (ui_console.c)
    if (!debug_capture_hide_ui()) {
        html_ui_render_page(client->ui);
        viewer_embed_frame(client, delta); // the hub's item view showcase iframe (no-op on the viewer page)
        html_ui_render_top(client->ui);    // the developer console over it (ui_console.c)
    }
    input_script_page_frame_end(client, delta);
    input_script_frame_presented(client); // `frames` on a page too
    debug_capture_frame_end(client);
    return 1;
}
