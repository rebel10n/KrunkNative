// The Settings window's "Client" tab and the menu's Exit button (client/include/client_settings.h).
//
// Tab: Krunker v7.1.4's desktop client adds {name: "Client", categories: ["client"]} to
// both views of windows[0].tabs (index.js, vF1730) and fills it from ClientSession.settings;
// glorp does the same with categories of rows (src/cSettings.json, frontend/settings.js).
// Rows here use the markup of v7.1.4's own settings generator (_html.settings.input:
// .setHed / .setBodH setBod_ folders, .settName rows, label.switch with its grooves,
// select.inputGrey2, sliderVal + sliderM, settingsBtn; "<span style='color: #eb5656'>*</span>"
// = needsRestart), so the tab looks like Krunker's other tabs. Every row does something real in KrunkNative; rows that do
// not apply to the platform are not generated (the web build shows only Frame Cap, UI
// Scale, the game options and the storage buttons).
//
// Exit: Krunker v7.1.4's #clientExit menu item -> clientExitPopup() ("Are you sure you
// want to close krunker?" YES/NO, the #confPop popup natively) -> closeClient(), which
// natively ends the main loop (glfwSetWindowShouldClose) so main.c runs its normal
// shutdown. The web build keeps the item hidden, like krunker.io in a browser.
#include <client_settings.h>
#include <ruleset_ui.h> // Settings > General "Game version"
#include <ui_logic.h>
#include <backend_ui.h> // servers_ui_region_options (Default Region)
#include <discord_rpc.h>
#include <render_settings.h>
#include <html_ui.h>
#include <audio.h>
#include <spectate.h>
#include <shared.h>
#include <GLFW/glfw3.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#else
#include <headless.h>
#include "../audio_miniaudio.h" // ma_is_backend_enabled: the Audio Backend options of this build
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <process.h>
#include <windows.h>
#include <shellapi.h> // after windows.h: it uses its types
#else
#include <limits.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#endif

#define CS_STORAGE_KEY "knClient"
#define CS_BRAIN_KEY "knBrainUrl" // backend_native.c reads it at startup
#define CS_MODE_KEY "knMode"       // online_mode.c: online / offline
#define CS_FINDER_KEY "knFinderUrl" // online_mode.c: the server finder list
static const char *cs_storage_key(const char *key) {
    return !strcmp(key, "accountMode") ? CS_MODE_KEY : !strcmp(key, "finderUrl") ? CS_FINDER_KEY : CS_BRAIN_KEY;
}
#define CS_JSON_MAX (16 * 1024)
#define CS_TICK_MS 250.0
#define CS_PATH_MAX 1024

// ---------------------------------------------------------------- table

enum {
    CS_NATIVE = 1,      // generated in the native client
    CS_WEB = 2,         // generated in the web build
    CS_RESTART = 4,     // needsRestart: red "*", applied on the next launch (restart offered)
    CS_GAME = 8,        // a game setting (knSettings, setSetting) surfaced here
    CS_SOUND_TAB = 16,  // generated in the game's Sound tab (ui_client_settings_extend_tab), not in Client
};

typedef enum { CS_SWITCH, CS_SELECT, CS_TEXT, CS_SLIDER, CS_UISCALE, CS_BUTTON } CsKind;

typedef struct {
    const char *value, *label;
} CsOption;

typedef struct {
    const char *key;      // knClient key (CS_GAME: the game setting's key; CS_BUTTON: the action)
    const char *name;
    const char *cat;      // folder header
    CsKind kind;
    unsigned flags;
    const char *def;      // default, string form ("true"/"false" for switches)
    const char *info;     // title tooltip
    const CsOption *options; // static select options (NULL: generated, see select_options)
    int option_count;
    double min, max, step;   // CS_SLIDER
    const char *button;      // label of the row's button (CS_BUTTON, or next to a switch)
    const char *action;      // knClientAction(...) of that button
} ClientSetting;

static const CsOption WINDOW_MODES[] = {{"0", "Windowed"}, {"2", "Borderless"}, {"1", "Fullscreen"}};
// online_mode.h: accounts at the account service, or guest with every skin and nothing saved
static const CsOption ACCOUNT_MODES[] = {{"online", "Online"}, {"offline", "Offline"}};

#if defined(_WIN32)
static const CsOption PRIORITIES[] = {
    {"High", "High"}, {"Above Normal", "Above Normal"}, {"Normal", "Normal"},
    {"Below Normal", "Below Normal"}, {"Low", "Low"}, {"Idle", "Idle"},
};
#else
// POSIX: an unprivileged process can only lower its priority (raise its nice value)
static const CsOption PRIORITIES[] = {{"Normal", "Normal"}, {"Below Normal", "Below Normal"}, {"Low", "Low"}, {"Idle", "Idle"}};
#endif
#define COUNT(a) ((int) (sizeof(a) / sizeof((a)[0])))

static const ClientSetting SETTINGS[] = {
    // ---- Display: window/presentation (glorp "Start Mode", "Uncap FPS"; the game's own rows surfaced)
    {"windowMode", "Window Mode", "Display", CS_SELECT, CS_NATIVE | CS_GAME, "0",
     "F11 switches between windowed and the last fullscreen mode", WINDOW_MODES, COUNT(WINDOW_MODES)},
    {"resolution", "Resolution", "Display", CS_SELECT, CS_NATIVE, "",
     "Window size in windowed mode, display mode in fullscreen (borderless always covers the monitor)"},
    {"vsync", "V-Sync", "Display", CS_SWITCH, CS_NATIVE | CS_GAME, "false", "Wait for the display refresh (swap interval 1)"},
    {"updateRate", "Frame Cap", "Display", CS_SLIDER, CS_NATIVE | CS_WEB | CS_GAME, "0",
     "0 = uncapped (also Render > Frame Cap)", NULL, 0, 0, 1200, 10},
    {"scaleUI", "UI Scale", "Display", CS_UISCALE, CS_NATIVE | CS_WEB | CS_GAME, "0.7", "Also Display > UI Scale"},
    // ---- System (glorp "Raw Input", "Webview Priority"; KRUNKNATIVE_AUDIO_BACKEND)
    {"rawInput", "Raw Input", "System", CS_SWITCH, CS_NATIVE, "true", "Unaccelerated mouse motion while the pointer is locked"},
    {"audioBackend", "Audio Backend", "System", CS_SELECT, CS_NATIVE | CS_RESTART, "",
     "Sound output (and voice chat input); Auto tries every backend of this build in order"},
    // ---- Sound tab: after Audio/Mic Input (audio_set_output_device; "" = the backend's default)
    {"audioOutput", "Audio Output", "Sound", CS_SELECT, CS_NATIVE | CS_SOUND_TAB, "",
     "Select hardware device for audio output"},
    {"priority", "Process Priority", "System", CS_SELECT, CS_NATIVE | CS_RESTART, "Normal",
     "CPU scheduling priority of the client process", PRIORITIES, COUNT(PRIORITIES)},
    // ---- Client (glorp "Exit Button", "Discord Rich Presence", "Resource Swapper"; custom CSS)
    {"exitButton", "Exit Button", "Client", CS_SWITCH, CS_NATIVE, "true", "The Exit item in the menu"},
    {"discordRPC", "Discord Rich Presence", "Client", CS_SWITCH, CS_NATIVE, "true", "Show the mode and map you play on your Discord profile"},
    {"swapper", "Resource Swapper", "Client", CS_SWITCH, CS_NATIVE | CS_RESTART, "true",
     "Files in the swapper folder replace the game's files at the same path (css/main.css, img/..., textures/..., sound/...)",
     NULL, 0, 0, 0, 0, "Open folder", "openSwapper"},
    {"customCSS", "Custom CSS", "Client", CS_SWITCH, CS_NATIVE, "true", "Applies custom.css from the client folder over the menu and HUD",
     NULL, 0, 0, 0, 0, "Edit", "editCSS"},
    // ---- Startup (--offline; auto join; the matchmaker)
    {"startOffline", "Start Offline", "Startup", CS_SWITCH, CS_NATIVE | CS_RESTART, "false",
     "Start in an offline game on the built-in server (like --offline)"},
    {"autoJoin", "Auto Join", "Startup", CS_SWITCH, CS_NATIVE, "false", "Enter the match as soon as the game has loaded"},
    {"accountMode", "Account Mode", "Startup", CS_SELECT, CS_NATIVE | CS_WEB | CS_RESTART, "online",
     "Online: log in at the account service, inventory and stats are saved. Offline: guest, every skin unlocked, nothing saved",
     ACCOUNT_MODES, COUNT(ACCOUNT_MODES)},
    {"brainUrl", "Account Service URL", "Startup", CS_TEXT, CS_NATIVE | CS_RESTART, "",
     "Account service / community coordinator (kn_brain); empty = https://coordinator.krunk.io"},
    {"finderUrl", "Server Finder URL", "Startup", CS_TEXT, CS_NATIVE | CS_WEB | CS_RESTART, "",
     "Public server list; empty = https://matchmaker.krunk.io/servers, off = none"},
    // ---- Game (glorp "Auto Spectate", "Always Play HS Sound on Kill")
    {"autoSpec", "Auto Spectate", "Game", CS_SWITCH, CS_NATIVE | CS_WEB, "false", "Turn Spectate on when the game loads"},
    {"hsSound", "Always Play HS Sound on Kill", "Game", CS_SWITCH, CS_NATIVE | CS_WEB, "false", ""},
    // ---- Storage (glorp "Clear Cache & Storage"; the v3.1.1 client's Clear Cache / Reset Addons)
    {"openConfig", "Client Folder", "Storage", CS_BUTTON, CS_NATIVE, "", "Settings, swapper and custom.css",
     NULL, 0, 0, 0, 0, "Open", "openConfig"},
    {"clearCache", "Clear Cache & Storage", "Storage", CS_BUTTON, CS_NATIVE | CS_WEB, "",
     "Clears the game's stored data (settings, binds, account) and restarts; client settings are kept",
     NULL, 0, 0, 0, 0, "Clear Now", "clearCache"},
    {"resetClient", "Reset Client Settings", "Storage", CS_BUTTON, CS_NATIVE | CS_WEB, "", "",
     NULL, 0, 0, 0, 0, "Reset", "resetClient"},
};

static const ClientSetting *find_setting(const char *key) {
    for (int i = 0; i < COUNT(SETTINGS); i++) {
        if (key && !strcmp(SETTINGS[i].key, key)) return &SETTINGS[i];
    }
    return NULL;
}

static int setting_shown(const ClientSetting *s) {
#ifdef __EMSCRIPTEN__
    return (s->flags & CS_WEB) != 0;
#else
    return (s->flags & CS_NATIVE) != 0;
#endif
}

// ---------------------------------------------------------------- state

static struct {
    cJSON *store;
    Client *client;
    long long launch_time;
    int relaunch;
    int timer;
    int auto_joined;
    int brain_env_was_set;   // KRUNKNATIVE_BRAIN_URL came from the user, not from backend_startup
    int audio_env_set;       // KRUNKNATIVE_AUDIO_BACKEND was set from the setting
    int rpc_allowed;         // not a headless / scripted / scenario run
    int hs_sound;
} g_cs;

// ---------------------------------------------------------------- store

static void store_load(void) {
    if (g_cs.store) return;
    char *buf = malloc(CS_JSON_MAX);
    if (buf && html_storage_get(CS_STORAGE_KEY, buf, CS_JSON_MAX) >= 0) {
        cJSON *parsed = cJSON_Parse(buf);
        if (cJSON_IsObject(parsed)) g_cs.store = parsed;
        else cJSON_Delete(parsed);
    }
    free(buf);
    if (!g_cs.store) g_cs.store = cJSON_CreateObject();
}

static void store_save(void) {
    char *text = cJSON_PrintUnformatted(g_cs.store);
    if (text) html_storage_set(CS_STORAGE_KEY, text);
    free(text);
}

// The value as a string: stored, else the default; CS_GAME rows read the game store.
static void setting_value(const ClientSetting *s, char *out, const size_t n) {
    snprintf(out, n, "%s", s->def ? s->def : "");
    if (s->flags & CS_GAME) {
        const cJSON *v = ui_setting_value(s->key);
        if (v) ui_setting_to_string(v, out, n);
        return;
    }
    if (!strcmp(s->key, "brainUrl") || !strcmp(s->key, "finderUrl") || !strcmp(s->key, "accountMode")) {
        char url[256];
        if (html_storage_get(cs_storage_key(s->key), url, sizeof(url)) >= 0 && (url[0] || strcmp(s->key, "accountMode")))
            snprintf(out, n, "%s", url);
        return;
    }
    store_load();
    const cJSON *v = cJSON_GetObjectItem(g_cs.store, s->key);
    if (cJSON_IsBool(v)) snprintf(out, n, "%s", cJSON_IsTrue(v) ? "true" : "false");
    else if (cJSON_IsString(v)) snprintf(out, n, "%s", cJSON_GetStringValue(v));
}

static int cs_bool(const char *key) {
    const ClientSetting *s = find_setting(key);
    if (!s) return 0;
    char v[16];
    setting_value(s, v, sizeof(v));
    return !strcmp(v, "true");
}

static void cs_string(const char *key, char *out, const size_t n) {
    const ClientSetting *s = find_setting(key);
    if (s) setting_value(s, out, n);
    else if (n) out[0] = 0;
}

static void store_put(const ClientSetting *s, const char *value) {
    if (!strcmp(s->key, "accountMode")) {
        html_storage_set(CS_MODE_KEY, !strcmp(value, "offline") ? "offline" : "online");
        return;
    }
    if (!strcmp(s->key, "brainUrl") || !strcmp(s->key, "finderUrl")) {
        // trimmed, no trailing slash (backend.h: "the brain URL (no trailing slash)")
        char url[256];
        while (*value && isspace((unsigned char) *value)) value++;
        snprintf(url, sizeof(url), "%s", value);
        size_t len = strlen(url);
        while (len && (isspace((unsigned char) url[len - 1]) || url[len - 1] == '/')) url[--len] = 0;
        html_storage_set(cs_storage_key(s->key), len ? url : NULL);
        return;
    }
    store_load();
    cJSON_DeleteItemFromObject(g_cs.store, s->key);
    if (s->kind == CS_SWITCH) cJSON_AddBoolToObject(g_cs.store, s->key, !strcmp(value, "true"));
    else cJSON_AddStringToObject(g_cs.store, s->key, value);
    store_save();
}

// ---------------------------------------------------------------- paths (native)

#ifndef __EMSCRIPTEN__
// html_dom_core.c's storage directory: $XDG_CONFIG_HOME/krunknative, else ~/.config/krunknative.
static void config_dir(char *out, const size_t n) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) {
        snprintf(out, n, "%s/krunknative", xdg);
        return;
    }
    const char *home = getenv("HOME");
#ifdef _WIN32
    if (!home || !*home) home = getenv("USERPROFILE");
#endif
    snprintf(out, n, "%s/.config/krunknative", home && *home ? home : ".");
}

static int make_dirs(const char *path) {
    char tmp[CS_PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/' && *p != '\\') continue;
        const char c = *p;
        *p = 0;
#ifdef _WIN32
        _mkdir(tmp);
#else
        mkdir(tmp, 0700);
#endif
        *p = c;
    }
#ifdef _WIN32
    return _mkdir(tmp) == 0 || errno == EEXIST;
#else
    return mkdir(tmp, 0700) == 0 || errno == EEXIST;
#endif
}

static void client_path(const char *name, char *out, const size_t n) {
    char dir[CS_PATH_MAX];
    config_dir(dir, sizeof(dir));
    snprintf(out, n, "%s/%s", dir, name);
}

// A folder or file in the system's file manager / default application (xdg-open, open,
// ShellExecute); KRUNKNATIVE_NO_BROWSER=1 (test runs) only logs it, like ui_open_external.
static void open_path(const char *path) {
    const char *off = getenv("KRUNKNATIVE_NO_BROWSER");
    if (off && *off && strcmp(off, "0")) {
        menu_log("open (disabled by KRUNKNATIVE_NO_BROWSER): %s", path);
        return;
    }
    menu_log("open: %s", path);
#ifdef _WIN32
    ShellExecuteA(NULL, "open", path, NULL, NULL, SW_SHOWNORMAL);
#else
#ifdef __APPLE__
    const char *opener = "open";
#else
    const char *opener = "xdg-open";
#endif
    const pid_t pid = fork();
    if (pid == 0) {
        if (fork() == 0) {
            setsid();
            execlp(opener, opener, path, (char *) NULL);
            _exit(127);
        }
        _exit(0);
    }
    if (pid > 0) waitpid(pid, NULL, 0);
#endif
}

static void set_env(const char *name, const char *value) {
#ifdef _WIN32
    _putenv_s(name, value ? value : "");
#else
    if (value) setenv(name, value, 1);
    else unsetenv(name);
#endif
}
#endif

// ---------------------------------------------------------------- resource swapper (native)

#ifndef __EMSCRIPTEN__
// glorp's swapper (src/modules/swapper.rs): every file under <client folder>/swapper,
// scanned once at startup, replaces the file at the same path below the page (ui/) or
// the game assets (assets/), i.e. the path krunker.io serves it at: css/main.css,
// img/..., textures/..., sound/..., models/... Hooked into read_file() (page, CSS,
// images, sounds, maps, shaders) and the texture loader (file_set_redirect).
#define SWAP_MAX 4096
static struct {
    char dir[CS_PATH_MAX];
    char *files[SWAP_MAX]; // paths relative to dir, '/' separated
    int count;
    char roots[3][CS_PATH_MAX];
    int root_count;
} g_swap;

static void swap_scan(const char *rel) {
    char path[CS_PATH_MAX];
    snprintf(path, sizeof(path), "%s%s%s", g_swap.dir, *rel ? "/" : "", rel);
    DIR *d = opendir(path);
    if (!d) return;
    for (struct dirent *e; (e = readdir(d)) && g_swap.count < SWAP_MAX;) {
        if (e->d_name[0] == '.') continue;
        char child[CS_PATH_MAX];
        snprintf(child, sizeof(child), "%s%s%s", rel, *rel ? "/" : "", e->d_name);
        char full[CS_PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", g_swap.dir, child);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) swap_scan(child);
        else if (S_ISREG(st.st_mode)) g_swap.files[g_swap.count++] = strdup(child);
    }
    closedir(d);
}

static void swap_add_root(const char *path) {
    if (g_swap.root_count >= 3 || !path || !*path) return;
    char resolved[CS_PATH_MAX];
#ifdef _WIN32
    if (!_fullpath(resolved, path, sizeof(resolved))) return;
    for (char *c = resolved; *c; c++) if (*c == '\\') *c = '/';
#else
    if (!realpath(path, resolved)) return;
#endif
    const size_t len = strlen(resolved);
    snprintf(g_swap.roots[g_swap.root_count++], CS_PATH_MAX, "%s%s", resolved, len && resolved[len - 1] == '/' ? "" : "/");
}

static int swap_redirect(const char *path, char *out, const size_t n) {
    if (!g_swap.count || !path) return 0;
    const char *rel = NULL;
    const char *assets = assets_path(); // "./assets/" or "../assets/": paths built with concat(assets_path(), ...)
    if (assets && !strncmp(path, assets, strlen(assets))) rel = path + strlen(assets);
    for (int i = 0; !rel && i < g_swap.root_count; i++) {
        const size_t len = strlen(g_swap.roots[i]);
        if (!strncmp(path, g_swap.roots[i], len)) rel = path + len;
    }
    if (!rel) return 0;
    while (rel[0] == '.' && rel[1] == '/') rel += 2;
    for (int i = 0; i < g_swap.count; i++) {
        if (strcmp(g_swap.files[i], rel)) continue;
        snprintf(out, n, "%s/%s", g_swap.dir, rel);
        return 1;
    }
    return 0;
}

static void swap_init(void) {
    client_path("swapper", g_swap.dir, sizeof(g_swap.dir));
    swap_scan("");
    if (!g_swap.count) return;
    const char *assets = assets_path();
    swap_add_root(assets);
    const char *ui_override = getenv("KRUNKNATIVE_UI_DIR");
    if (ui_override && *ui_override) {
        swap_add_root(ui_override);
    } else if (assets) {
        char ui[CS_PATH_MAX];
        snprintf(ui, sizeof(ui), "%s../ui", assets);
        swap_add_root(ui);
    }
    file_set_redirect(swap_redirect);
    printf("[client] resource swapper: %d file(s) from %s\n", g_swap.count, g_swap.dir);
}
#endif

// ---------------------------------------------------------------- appliers

static int chrome_visible(void) {
#ifdef UI_SCENARIO_SUPPORTED
    if (ui_scenario_active()) return ui_scenario_client_chrome(); // uidiff: krunker.io's browser page unless asked
#endif
    return 1;
}

static void apply_exit_button(void) {
    HtmlElem el = html_dom_by_id("clientExit");
    if (!el) return;
#ifdef __EMSCRIPTEN__
    html_dom_set_display(el, "none"); // a browser tab cannot close itself: hidden, as on krunker.io
#else
    // v7.1.4 / glorp: document.querySelector("#clientExit").style.display = "flex"
    html_dom_set_display(el, chrome_visible() && cs_bool("exitButton") ? "flex" : "none");
#endif
}

static void apply_raw_input(void) {
#ifndef __EMSCRIPTEN__
    Client *client = g_cs.client;
    if (!client || !client->window || !glfwRawMouseMotionSupported()) return;
    glfwSetInputMode(client->window, GLFW_RAW_MOUSE_MOTION, cs_bool("rawInput") ? GLFW_TRUE : GLFW_FALSE);
#endif
}

static void apply_custom_css(void) {
#ifndef __EMSCRIPTEN__
    if (!chrome_visible() || !cs_bool("customCSS")) {
        html_ui_set_custom_css(NULL);
        return;
    }
    char path[CS_PATH_MAX];
    client_path("custom.css", path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) {
        html_ui_set_custom_css(NULL);
        return;
    }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *css = size > 0 && size < (1 << 20) ? malloc((size_t) size + 1) : NULL;
    size_t got = 0;
    if (css) {
        got = fread(css, 1, (size_t) size, f);
        css[got] = 0;
    }
    fclose(f);
    html_ui_set_custom_css(css);
    if (css) menu_log("custom css: %zu bytes from %s", got, path);
    free(css);
#endif
}

#ifndef __EMSCRIPTEN__
static int parse_size(const char *s, int *w, int *h) {
    return s && sscanf(s, "%dx%d", w, h) == 2 && *w >= 320 && *h >= 200;
}
#endif

// "Resolution": the windowed size now (and the size windowed mode returns to), or the
// display mode when the window is in exclusive fullscreen.
static void apply_resolution(void) {
#ifndef __EMSCRIPTEN__
    Client *client = g_cs.client;
    if (!client || !client->window || headless_enabled()) return;
    char value[32];
    cs_string("resolution", value, sizeof(value));
    int w = 0, h = 0;
    GLFWmonitor *monitor = glfwGetWindowMonitor(client->window);
    if (monitor) {
        const GLFWvidmode *vid = glfwGetVideoMode(monitor);
        if (!parse_size(value, &w, &h) && vid) {
            w = vid->width;
            h = vid->height;
        }
        glfwSetWindowMonitor(client->window, monitor, 0, 0, w, h, GLFW_DONT_CARE);
        return;
    }
    if (!parse_size(value, &w, &h)) return;
    client->windowed_rect.width = w;
    client->windowed_rect.height = h;
    // not borderless; maximized: the size un-maximize returns to (the rect kept above)
    if (glfwGetWindowAttrib(client->window, GLFW_DECORATED) && !glfwGetWindowAttrib(client->window, GLFW_MAXIMIZED))
        glfwSetWindowSize(client->window, w, h);
#endif
}

void client_settings_fullscreen_size(int *w, int *h) {
#ifndef __EMSCRIPTEN__
    char value[32];
    cs_string("resolution", value, sizeof(value));
    int rw, rh;
    if (w && h && parse_size(value, &rw, &rh)) {
        *w = rw;
        *h = rh;
    }
#else
    (void) w;
    (void) h;
#endif
}

int client_settings_hs_sound_always(void) {
    return g_cs.hs_sound;
}

static void apply_auto_spectate(void) {
    if (!cs_bool("autoSpec")) return;
    spectate_set_requested(1);
    HtmlElem box = html_dom_query(0, "#spectButton input");
    if (box) html_dom_set_checked(box, 1);
    menu_update_instructions();
}

static void apply_discord(void) {
#ifndef __EMSCRIPTEN__
    discord_rpc_set_enabled(g_cs.rpc_allowed && cs_bool("discordRPC"));
#endif
}

// ---------------------------------------------------------------- restart / exit

static void request_close(void) {
#ifndef __EMSCRIPTEN__
    Client *client = g_cs.client ? g_cs.client : menu_ui()->client;
    if (client && client->window) glfwSetWindowShouldClose(client->window, GLFW_TRUE);
#endif
}

static void restart_client(void) {
#ifdef __EMSCRIPTEN__
    EM_ASM({ location.reload(); });
#else
    g_cs.relaunch = 1;
    request_close();
#endif
}

static void restart_answered(const int accepted, const char *text, void *user) {
    (void) text;
    (void) user;
    if (accepted) restart_client();
}

static void exit_answered(const int accepted, const char *text, void *user) {
    (void) text;
    (void) user;
    if (accepted) request_close();
}

#ifndef __EMSCRIPTEN__
#if defined(__linux__)
// /proc/self/cmdline: the original argv (main.c compacts argv while parsing it).
static char **read_cmdline(void) {
    FILE *f = fopen("/proc/self/cmdline", "rb");
    if (!f) return NULL;
    static char buf[16384];
    const size_t len = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (!len) return NULL;
    buf[len] = 0;
    static char *argv[256];
    int argc = 0;
    for (size_t i = 0; i < len && argc < 255; i += strlen(buf + i) + 1) argv[argc++] = buf + i;
    argv[argc] = NULL;
    return argv;
}
#endif

void client_settings_before_exec(void) {
    // the next process reads the settings again: drop what this one put into the environment
    if (!g_cs.brain_env_was_set) set_env("KRUNKNATIVE_BRAIN_URL", NULL);
    client_ruleset_env_cleanup(); // the next process picks the page of the saved Game version
    if (g_cs.audio_env_set) set_env("KRUNKNATIVE_AUDIO_BACKEND", NULL);
}

static void relaunch(void) {
    client_settings_before_exec();
    client_render_window_before_exec(g_cs.client ? g_cs.client : menu_ui()->client); // KRUNKNATIVE_RESTORE_WINDOW
    fflush(stdout);
    fflush(stderr);
#if defined(_WIN32)
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    if (CreateProcessA(NULL, GetCommandLineA(), NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
#elif defined(__linux__)
    char exe[PATH_MAX];
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    char **argv = read_cmdline();
    if (n > 0 && argv) {
        exe[n] = 0;
        execv(exe, argv);
    }
    fprintf(stderr, "[client] restart failed: %s\n", strerror(errno));
#else
    fprintf(stderr, "[client] restart: start Krunker Box Office again to apply the settings\n");
#endif
}
#endif

void client_settings_exit(void) {
#ifndef __EMSCRIPTEN__
    discord_rpc_shutdown();
    if (g_cs.relaunch) relaunch();
#endif
}

// ---------------------------------------------------------------- startup

#ifndef __EMSCRIPTEN__
static void apply_priority(void) {
    char value[32];
    cs_string("priority", value, sizeof(value));
    if (!value[0] || !strcmp(value, "Normal")) return;
#ifdef _WIN32
    static const struct { const char *name; DWORD cls; } MAP[] = {
        {"High", HIGH_PRIORITY_CLASS}, {"Above Normal", ABOVE_NORMAL_PRIORITY_CLASS},
        {"Below Normal", BELOW_NORMAL_PRIORITY_CLASS}, {"Low", IDLE_PRIORITY_CLASS}, {"Idle", IDLE_PRIORITY_CLASS},
    };
    for (int i = 0; i < COUNT(MAP); i++) {
        if (strcmp(MAP[i].name, value)) continue;
        if (!SetPriorityClass(GetCurrentProcess(), MAP[i].cls)) fprintf(stderr, "[client] priority %s failed\n", value);
        else printf("[client] process priority %s\n", value);
    }
#else
    static const struct { const char *name; int nice; } MAP[] = {{"Below Normal", 5}, {"Low", 10}, {"Idle", 19}};
    for (int i = 0; i < COUNT(MAP); i++) {
        if (strcmp(MAP[i].name, value)) continue;
        if (setpriority(PRIO_PROCESS, 0, MAP[i].nice) != 0) fprintf(stderr, "[client] priority %s failed: %s\n", value, strerror(errno));
        else printf("[client] process priority %s (nice %d)\n", value, MAP[i].nice);
    }
#endif
}

// Discord stays off for runs nobody watches: headless, scripted input, debug captures.
static int rpc_allowed(void) {
    static const char *const TEST_ENV[] = {"KRUNKNATIVE_INPUT_SCRIPT", "KRUNKNATIVE_SCREENSHOT", "KRUNKNATIVE_CAMERA"};
    if (headless_enabled()) return 0;
    for (int i = 0; i < COUNT(TEST_ENV); i++) {
        const char *v = getenv(TEST_ENV[i]);
        if (v && *v) return 0;
    }
    return 1;
}
#endif

void client_settings_startup(Client *client, int *offline) {
    g_cs.client = client;
    g_cs.launch_time = (long long) time(NULL);
    store_load();
    g_cs.hs_sound = cs_bool("hsSound");
#ifndef __EMSCRIPTEN__
    const char *brain = getenv("KRUNKNATIVE_BRAIN_URL");
    g_cs.brain_env_was_set = brain && *brain;
    char backend[32];
    cs_string("audioBackend", backend, sizeof(backend));
    const char *env_backend = getenv("KRUNKNATIVE_AUDIO_BACKEND");
    if (backend[0] && !(env_backend && *env_backend)) {
        set_env("KRUNKNATIVE_AUDIO_BACKEND", backend);
        g_cs.audio_env_set = 1;
        printf("[client] audio backend %s\n", backend);
    }
    char output[AUDIO_DEVICE_NAME_MAX];
    cs_string("audioOutput", output, sizeof(output));
    audio_set_output_device(output); // before audio_init: opened there (default when missing)
    apply_priority();
    apply_raw_input();
    // KRUNKNATIVE_WINDOW_SIZE and the window F4 / a relaunch restored keep their size
    const char *size = getenv("KRUNKNATIVE_WINDOW_SIZE");
    if (!(size && *size) && !client_render_window_restored()) apply_resolution();
    if (offline && cs_bool("startOffline")) {
        *offline = 1;
        printf("[client] start offline (Client tab)\n");
    }
    if (cs_bool("swapper")) swap_init();
    g_cs.rpc_allowed = rpc_allowed();
    apply_discord();
#else
    (void) offline;
#endif
}

// ---------------------------------------------------------------- per-frame (timer)

static void update_presence(void) {
#ifndef __EMSCRIPTEN__
    if (!discord_rpc_enabled()) return;
    const MenuUi *ui = menu_ui();
    char details[128] = "", state[64];
    if (ui->connected && ui->ready && ui->map_name[0]) {
        // glorp: state "<mode> on <map>"
        snprintf(details, sizeof(details), "%s on %s", ui->mode_name[0] ? ui->mode_name : "Free for All", ui->map_name);
        snprintf(state, sizeof(state), "%s", ui->menu_open ? "In Menu" : "Playing");
    } else {
        snprintf(state, sizeof(state), "%s", ui->connect_error[0] ? "Disconnected" : "Loading");
    }
    discord_rpc_set_activity(details, state, g_cs.launch_time);
    discord_rpc_poll();
#endif
}

static void auto_join(void) {
#ifndef __EMSCRIPTEN__
    if (g_cs.auto_joined) return;
    const MenuUi *ui = menu_ui();
    if (!ui->dom_ready || !ui->connected || !ui->ready) return;
    g_cs.auto_joined = 1; // only the first time the game is ready
    if (!cs_bool("autoJoin") || !ui->menu_open || menu_window_is_open()) return;
    menu_log("auto join (Client tab)");
    menu_play();
#endif
}

static void tick(void *user) {
    (void) user;
    g_cs.timer = html_timer_add(CS_TICK_MS, tick, NULL);
    update_presence();
    auto_join();
}

void ui_client_settings_dom_init(void) {
    if (!g_cs.client) g_cs.client = menu_ui()->client;
    store_load();
    g_cs.hs_sound = cs_bool("hsSound");
    apply_exit_button();
    apply_custom_css();
    apply_auto_spectate();
#ifdef UI_SCENARIO_SUPPORTED
    if (ui_scenario_active()) return; // nothing polls in scenario renders
#endif
    if (!g_cs.timer) g_cs.timer = html_timer_add(CS_TICK_MS, tick, NULL);
}

// ---------------------------------------------------------------- tab markup

int ui_client_settings_tab(void) {
    return chrome_visible() ? ui_settings_view_tab_count(menu_ui()->settings_view) : -1;
}

char *ui_client_settings_head(const char *head, const int active_tab) {
    StrBuf out = {0};
    const int index = ui_client_settings_tab();
    const char *layout = head ? strstr(head, "id=\"settingsTabLayout\"") : NULL;
    const char *last_tab = NULL;
    for (const char *p = layout; p && (p = strstr(p, "class=\"settingTab")); p++) last_tab = p;
    const char *tab_end = last_tab ? strstr(last_tab, "</div>") : NULL;
    if (index < 0 || !layout || !tab_end) {
        sb_append(&out, head ? head : "");
        return sb_finish(&out);
    }
    tab_end += strlen("</div>");
    const char *id_end = layout + strlen("id=\"settingsTabLayout\"");
    sb_append_n(&out, head, (size_t) (id_end - head));
    sb_append(&out, " class=\"knClientTabs\"");
    sb_append_n(&out, id_end, (size_t) (tab_end - id_end));
    // getTabs(): "<div class='settingTab " + (active ? "tabANew" : "") + "' onclick=...>" + name
    sb_appendf(&out, "\n<div class=\"settingTab %s\" onclick=\"window.windows[0].changeTab(%d)\">Client</div>",
               active_tab == index ? "tabANew" : "", index);
    sb_append(&out, tab_end);
    return sb_finish(&out);
}

static void append_attr_escaped(StrBuf *sb, const char *text) {
    sb_append_esc(sb, text ? text : "");
}

// <option>s of the generated selects: the monitor's video modes, the audio backends of
// this build.
static void append_option(StrBuf *sb, const char *value, const char *label, const char *current) {
    sb_append(sb, "<option value=\"");
    append_attr_escaped(sb, value);
    sb_append(sb, "\"");
    if (!strcmp(value, current)) sb_append(sb, " selected=\"\"");
    sb_append(sb, ">");
    sb_append_esc(sb, label);
    sb_append(sb, "</option>");
}

static void resolution_options(StrBuf *sb, const char *current) {
    append_option(sb, "", "Default", current);
    int listed_current = !current[0];
#ifndef __EMSCRIPTEN__
    Client *client = g_cs.client;
    GLFWmonitor *monitor = client && client->window ? glfwGetWindowMonitor(client->window) : NULL;
    if (!monitor) monitor = glfwGetPrimaryMonitor();
    int count = 0;
    const GLFWvidmode *modes = monitor ? glfwGetVideoModes(monitor, &count) : NULL;
    for (int i = count - 1; i >= 0; i--) { // GLFW sorts ascending: largest first
        char value[32];
        snprintf(value, sizeof(value), "%dx%d", modes[i].width, modes[i].height);
        int dup = 0;
        for (int j = count - 1; j > i && !dup; j--) dup = modes[j].width == modes[i].width && modes[j].height == modes[i].height;
        if (dup) continue;
        append_option(sb, value, value, current);
        if (!strcmp(value, current)) listed_current = 1;
    }
#endif
    if (!listed_current) append_option(sb, current, current, current);
}

static void audio_backend_options(StrBuf *sb, const char *current) {
    append_option(sb, "", "Auto", current);
#ifndef __EMSCRIPTEN__
    static const struct { const char *value, *label; ma_backend backend; } BACKENDS[] = {
        {"pulseaudio", "PulseAudio / PipeWire", ma_backend_pulseaudio}, {"alsa", "ALSA", ma_backend_alsa},
        {"jack", "JACK", ma_backend_jack}, {"sndio", "sndio", ma_backend_sndio}, {"audio4", "audio(4)", ma_backend_audio4},
        {"oss", "OSS", ma_backend_oss}, {"coreaudio", "Core Audio", ma_backend_coreaudio},
        {"wasapi", "WASAPI", ma_backend_wasapi}, {"dsound", "DirectSound", ma_backend_dsound},
        {"winmm", "WinMM", ma_backend_winmm}, {"null", "None (silent)", ma_backend_null},
    };
    int listed_current = !current[0];
    for (int i = 0; i < COUNT(BACKENDS); i++) {
        if (!ma_is_backend_enabled(BACKENDS[i].backend)) continue;
        append_option(sb, BACKENDS[i].value, BACKENDS[i].label, current);
        if (!strcmp(BACKENDS[i].value, current)) listed_current = 1;
    }
    if (!listed_current) append_option(sb, current, current, current);
#endif
}

// The backend's playback devices; a saved device that is not connected stays listed (audio
// plays on the default device meanwhile, audio.c) so the choice is not lost.
static void audio_output_options(StrBuf *sb, const char *current) {
    append_option(sb, "", "Default", current);
#ifndef __EMSCRIPTEN__
    AudioDeviceInfo devices[32];
    const int count = audio_output_devices(devices, COUNT(devices));
    int listed_current = !current[0];
    for (int i = 0; i < count; i++) {
        int dup = 0;
        for (int j = 0; j < i && !dup; j++) dup = !strcmp(devices[j].name, devices[i].name);
        if (dup) continue;
        append_option(sb, devices[i].name, devices[i].name, current);
        if (!strcmp(devices[i].name, current)) listed_current = 1;
    }
    if (!listed_current) {
        char label[AUDIO_DEVICE_NAME_MAX + 32];
        snprintf(label, sizeof(label), "%s (not connected)", current);
        append_option(sb, current, label, current);
    }
#endif
}

// "<div class='settName' title='info'>Name <span style='color: #eb5656'>*</span> " (134.js genSettings)
static void row_begin(StrBuf *sb, const ClientSetting *s) {
    sb_append(sb, "<div class=\"settName\" title=\"");
    append_attr_escaped(sb, (s->flags & CS_RESTART) ? (s->info && *s->info ? s->info : "Requires Restart") : s->info);
    sb_append(sb, "\">");
    sb_append_esc(sb, s->name);
    if (s->flags & CS_RESTART) sb_append(sb, " <span style=\"color: #eb5656\">*</span>");
    sb_append(sb, " ");
}

static void append_button(StrBuf *sb, const char *label, const char *action) {
    sb_appendf(sb, "<div class=\"settingsBtn\" style=\"width: auto;\" onclick=\"knClientAction(&quot;%s&quot;)\">", action);
    sb_append_esc(sb, label);
    sb_append(sb, "</div>");
}

// The handler a row's control calls: setSetting for the game's own settings,
// knClientSet for the client store.
static const char *row_handler(const ClientSetting *s) {
    return (s->flags & CS_GAME) ? "window.setSetting" : "knClientSet";
}

static void append_slider(StrBuf *sb, const char *key, const char *value, const double min, const double max,
                          const double step, const char *container_style) {
    sb_appendf(sb, "<input type=\"number\" class=\"sliderVal\" id=\"slid_input_%s\" min=\"%g\" max=\"%g\" value=\"%s\" "
                   "onkeypress=\"return window.delayExecute(&quot;%s&quot;, this, undefined)\" style=\"margin-right:0px;border-width:0px\">\n"
                   "\t\t\t\t\t\t\t<div class=\"slidecontainer\" style=\"%s\"><input type=\"range\" id=\"slid_%s\" min=\"%g\" "
                   "max=\"%g\" step=\"%g\" value=\"%s\" class=\"sliderM\" oninput=\"window.setSetting(&quot;%s&quot;, this.value)\"></div>",
               key, min, max, value, key, container_style, key, min, max, step, value, key);
}

static void append_row(StrBuf *sb, const ClientSetting *s) {
    char value[256];
    setting_value(s, value, sizeof(value));
    row_begin(sb, s);
    switch (s->kind) {
        case CS_SWITCH:
            sb_appendf(sb, "<label class=\"switch\" style=\"margin-left:10px\"><input type=\"checkbox\" "
                           "onclick=\"%s(&quot;%s&quot;, this.checked)\"%s><span class=\"slider\"><span class=\"grooves\"></span></span></label>",
                       row_handler(s), s->key, !strcmp(value, "true") ? " checked=\"\"" : "");
            if (s->button) append_button(sb, s->button, s->action);
            break;
        case CS_SELECT:
            sb_appendf(sb, "<select onchange=\"%s(&quot;%s&quot;, this.value)\" class=\"inputGrey2\">\n                     ",
                       row_handler(s), s->key);
            if (s->options) {
                for (int i = 0; i < s->option_count; i++) append_option(sb, s->options[i].value, s->options[i].label, value);
            } else if (!strcmp(s->key, "resolution")) {
                resolution_options(sb, value);
            } else if (!strcmp(s->key, "audioBackend")) {
                audio_backend_options(sb, value);
            } else if (!strcmp(s->key, "audioOutput")) {
                audio_output_options(sb, value);
            }
            sb_append(sb, "</select>");
            break;
        case CS_TEXT:
            sb_appendf(sb, "<input type=\"text\" name=\"text\" id=\"knClient_%s\" class=\"inputGrey2\" placeholder=\"", s->key);
            append_attr_escaped(sb, !strcmp(s->key, "brainUrl") ? "https://coordinator.krunk.io"
                                    : !strcmp(s->key, "finderUrl") ? "https://matchmaker.krunk.io/servers" : s->name);
            sb_append(sb, "\" value=\"");
            append_attr_escaped(sb, value);
            sb_appendf(sb, "\" onchange=\"knClientSet(&quot;%s&quot;, this.value)\">", s->key);
            break;
        case CS_SLIDER:
            append_slider(sb, s->key, value, s->min, s->max, s->step, "");
            break;
        case CS_UISCALE: {
            // Display > Interface > UI Scale: the uiScaling switch + the scaleUI slider
            const cJSON *on = ui_setting_value("uiScaling");
            sb_appendf(sb, "<label class=\"switch\" style=\"margin-left:10px\"><input type=\"checkbox\" "
                           "onclick=\"window.setSetting(&quot;uiScaling&quot;, this.checked)\"%s><span class=\"slider\"><span class=\"grooves\"></span></span></label>",
                       cJSON_IsFalse(on) ? "" : " checked=\"\"");
            append_slider(sb, "scaleUI", value, 0.1, 1.0, 0.1, "width: 244px;");
            break;
        }
        case CS_BUTTON:
            append_button(sb, s->button, s->action);
            break;
    }
    sb_append(sb, "</div>");
}

char *ui_client_settings_tab_html(const int searching) {
    StrBuf sb = {0};
    if (!chrome_visible()) return sb_finish(&sb);
    const char *cat = NULL;
    for (int i = 0; i < COUNT(SETTINGS); i++) {
        const ClientSetting *s = &SETTINGS[i];
        if (!setting_shown(s) || (s->flags & CS_SOUND_TAB)) continue; // Sound tab rows: ui_client_settings_extend_tab
        if (searching && (s->flags & CS_GAME)) continue; // the game's own tab lists it (one id per page)
        if (!cat || strcmp(cat, s->cat)) {
            if (cat) sb_append(&sb, "</div>");
            cat = s->cat;
            char id[48];
            snprintf(id, sizeof(id), "kn%s", cat);
            sb_appendf(&sb, "<div class=\"setHed\" id=\"setHed_%s\" onclick=\"window.windows[0].collapseFolder(this)\">"
                            "<span class=\"material-icons plusOrMinus\">keyboard_arrow_down</span> %s</div><div class=\"setBodH\" id=\"setBod_%s\">",
                       id, cat, id);
        }
        append_row(&sb, s);
    }
    if (cat) sb_append(&sb, "</div>");
    return sb_finish(&sb);
}

// The game's Sound tab (ui/windows/settings-tab-5.html) with the client's rows after its
// Audio/Mic Input row; NULL when `tab` is another tab or nothing is added.
static char *extend_sound_tab(const char *tab) {
    if (!tab || !chrome_visible()) return NULL;
    const char *row = strstr(tab, "setSetting(&quot;audioInput&quot;");
    const char *end = row ? strstr(row, "</select></div>") : NULL;
    if (!end) return NULL;
    end += strlen("</select></div>");
    StrBuf sb = {0};
    sb_append_n(&sb, tab, (size_t) (end - tab));
    for (int i = 0; i < COUNT(SETTINGS); i++) {
        if ((SETTINGS[i].flags & CS_SOUND_TAB) && setting_shown(&SETTINGS[i])) append_row(&sb, &SETTINGS[i]);
    }
    sb_append(&sb, end);
    return sb_finish(&sb);
}

// Version toggle (ruleset_ui.h): General > Localization's first row, "Game version". v3.1.1 is its
// menu page over the v7.1.4 ruleset (native only; the web lists it disabled). Shown in both views
// and in uidiff only when asked (clientChrome), so the captured v7 tabs stay pixel-identical.
static char *ui_client_settings_extend_tab_rest(const char *tab);
static void rerender(void);

static void append_game_version_row(StrBuf *sb) {
    const ClientGameVersion *saved = client_game_version_saved();
    sb_append(sb, "<div class=\"settName\" id=\"knGameVersionRow\" title=\"The Krunker version of offline games and quick join; "
                  "a server you join plays its own. A different menu page loads after a restart. "
                  "v3.1.1 shows its own menu and plays v7.1.4 rules until its gameplay is ported\">Game version "
                  "<span style=\"color: #eb5656\">*</span> <select id=\"knGameVersion\" onchange=\"knSetRuleset(this.value)\" class=\"inputGrey2\">");
    const int v156 = client_page_v156_saved(); // menu page only (ruleset_page_v156.c): saved reads v7
#ifndef __EMSCRIPTEN__
    sb_appendf(sb, "<option value=\"%s\"%s>v1.5.6 (menu only, plays v7.1.4)</option>", CLIENT_PAGE_V156_KEY, v156 ? " selected=\"\"" : "");
#endif
    int count = 0;
    const ClientGameVersion *versions = client_game_versions(&count);
    for (int i = 0; i < count; i++) {
        const ClientGameVersion *v = &versions[i];
        const int available = client_game_version_available(v);
        sb_appendf(sb, "<option value=\"%s\"%s%s>v%s%s</option>", v->key, v == saved && !v156 ? " selected=\"\"" : "",
                   available ? "" : " disabled=\"\"", v->version,
                   !available ? " (desktop client)" : v->stand_in ? " (menu only, plays v7.1.4)" : "");
    }
    sb_append(sb, "</select></div>");
}

static char *extend_general_tab(const char *tab) {
    if (!tab || !chrome_visible()) return NULL;
    const char *body = strstr(tab, "id=\"setBod_local\">");
    if (!body) return NULL;
    body += strlen("id=\"setBod_local\">");
    StrBuf sb = {0};
    sb_append_n(&sb, tab, (size_t) (body - tab));
    append_game_version_row(&sb);
    sb_append(&sb, body);
    return sb_finish(&sb);
}

static void ruleset_restart_answered(const int accepted, const char *text, void *user) {
    (void) text;
    (void) user;
    if (accepted) restart_client();
}

// knSetRuleset(value) / the console's `ruleset`: save, then offer the restart a page change needs
void ui_client_settings_select_ruleset(const char *value) {
    Client *client = g_cs.client ? g_cs.client : menu_ui()->client;
    const ClientRulesetResult r = client_ruleset_select(client, value);
    rerender(); // an open Settings window shows the new choice (the console sets it too)
    const ClientGameVersion *saved = client_game_version_saved();
    const char *notice = client_game_version_notice(saved); // v3.1.1: its menu, v7.1.4's rules
    if (r == CLIENT_RULESET_UNAVAILABLE) {
        ui_notify("Krunker v3.1.1 is available in the desktop client only");
    } else if (r == CLIENT_RULESET_SAVED_RELOAD) {
        char text[256];
        if (client_page_v156_saved())
            snprintf(text, sizeof(text), "The Krunker v1.5.6 menu loads after a client restart. It is the menu only: v1.5.6 "
                                         "gameplay is not ported, games play Krunker v7.1.4. Restart now?");
        else
            snprintf(text, sizeof(text), "%s%sThe menu of this Game version loads after a client restart. Restart now?",
                     notice ? notice : "", notice ? ". " : "");
        ui_confirm(text, ruleset_restart_answered, NULL);
    } else if (r == CLIENT_RULESET_SAVED) {
        char text[192];
        if (client_page_v156_saved()) snprintf(text, sizeof(text), "Game version: the Krunker v1.5.6 menu, games play v%s", saved->rules->version);
        else if (notice) snprintf(text, sizeof(text), "%s", notice);
        else snprintf(text, sizeof(text), "Game version: Krunker v%s", saved->version);
        ui_notify(text);
    }
}

// A settings tab as shown: Localization > Default Region lists the matchmaker's regions (the
// static capture only has "Local"; servers_ui_region_options), the Sound tab gains the
// client's rows, General gains "Game version". NULL = `tab` unchanged.
char *ui_client_settings_extend_tab(const char *tab) {
    if (!tab) return NULL;
    char *general = extend_general_tab(tab);
    if (general) {
        char *more = ui_client_settings_extend_tab_rest(general);
        if (more) {
            free(general);
            return more;
        }
        return general;
    }
    return ui_client_settings_extend_tab_rest(tab);
}

static char *ui_client_settings_extend_tab_rest(const char *tab) {
    char *regions = NULL;
    const char *row = strstr(tab, "setSetting(&quot;defaultRegion&quot;");
    const char *open = row ? strchr(row, '>') : NULL;
    const char *close = open ? strstr(open, "</select>") : NULL;
    if (close) {
        const cJSON *saved = ui_setting_saved("defaultRegion");
        char *options = servers_ui_region_options(cJSON_IsString(saved) ? saved->valuestring : "local");
        StrBuf sb = {0};
        sb_append_n(&sb, tab, (size_t) (open + 1 - tab));
        if (options) sb_append(&sb, options);
        sb_append(&sb, close);
        free(options);
        regions = sb_finish(&sb);
    }
    char *sound = extend_sound_tab(regions ? regions : tab);
    if (sound) {
        free(regions);
        return sound;
    }
    return regions;
}

// ---------------------------------------------------------------- handlers

static void rerender(void) {
    if (menu_ui()->active_window == 1 && menu_window_is_open()) menu_show_window(1, 1);
}

static void apply_setting(const ClientSetting *s) {
    const char *k = s->key;
    if (!strcmp(k, "rawInput")) apply_raw_input();
    else if (!strcmp(k, "exitButton")) apply_exit_button();
    else if (!strcmp(k, "discordRPC")) apply_discord();
    else if (!strcmp(k, "customCSS")) apply_custom_css();
    else if (!strcmp(k, "resolution")) apply_resolution();
    else if (!strcmp(k, "autoSpec")) apply_auto_spectate();
    else if (!strcmp(k, "hsSound")) g_cs.hs_sound = cs_bool("hsSound");
    else if (!strcmp(k, "audioOutput")) {
        char output[AUDIO_DEVICE_NAME_MAX];
        cs_string("audioOutput", output, sizeof(output));
        audio_set_output_device(output); // re-opens the device live
    }
}

// F5 (game_reload.c): the store read again from disk; each changed live setting applied like a
// change in the tab (apply_setting), custom.css and the swapper folder read again. Rows of the
// game store (CS_GAME) follow the game settings' own reload.
const char *ui_client_settings_reload(void) {
#ifdef __EMSCRIPTEN__
    return NULL; // the web reload is the browser's (location.reload)
#else
    enum { N = COUNT(SETTINGS) };
    static char before[N][256];
    for (int i = 0; i < N; i++) setting_value(&SETTINGS[i], before[i], sizeof(before[i]));
    cJSON_Delete(g_cs.store);
    g_cs.store = NULL;
    store_load();
    const char *restart = NULL;
    for (int i = 0; i < N; i++) {
        const ClientSetting *s = &SETTINGS[i];
        if ((s->flags & CS_GAME) || s->kind == CS_BUTTON || !setting_shown(s)) continue;
        char now[256];
        setting_value(s, now, sizeof(now));
        if (!strcmp(now, before[i])) continue;
        printf("[client] reload: %s %s -> %s\n", s->key, before[i], now);
        if (s->flags & CS_RESTART) {
            if (!restart) restart = s->name; // applies at startup only
            continue;
        }
        apply_setting(s);
    }
    apply_custom_css(); // custom.css may have changed with the switch left on
    if (!restart && cs_bool("swapper")) { // new or removed files in the swapper folder
        for (int i = 0; i < g_swap.count; i++) free(g_swap.files[i]);
        g_swap.count = g_swap.root_count = 0;
        swap_init();
    }
    rerender();
    return restart;
#endif
}

static void set_client_setting(const char *key, const char *value) {
    const ClientSetting *s = find_setting(key);
    if (!s || (s->flags & CS_GAME) || s->kind == CS_BUTTON) return;
    char before[256];
    setting_value(s, before, sizeof(before));
    store_put(s, value);
    char after[256];
    setting_value(s, after, sizeof(after));
    menu_log("client setting %s=%s", key, after);
    apply_setting(s);
    // needsRestart: the v3.1.1 client alerts "App will now restart"; here the restart is offered
    if ((s->flags & CS_RESTART) && strcmp(before, after))
        ui_confirm("This setting requires a client restart to take effect. Restart now?", restart_answered, NULL);
}

// ---- actions

static void reset_answered(const int accepted, const char *text, void *user) {
    (void) text;
    (void) user;
    if (!accepted) return;
    cJSON_Delete(g_cs.store);
    g_cs.store = cJSON_CreateObject();
    store_save();
    html_storage_set(CS_BRAIN_KEY, NULL);
    html_storage_set(CS_MODE_KEY, NULL);
    html_storage_set(CS_FINDER_KEY, NULL);
    for (int i = 0; i < COUNT(SETTINGS); i++) {
        if (!(SETTINGS[i].flags & CS_GAME)) apply_setting(&SETTINGS[i]);
    }
    rerender();
}

#ifndef __EMSCRIPTEN__
// Every storage entry of the client folder except the client's own (knClient, knBrainUrl).
static void clear_storage(void) {
    char dir[CS_PATH_MAX];
    config_dir(dir, sizeof(dir));
    DIR *d = opendir(dir);
    if (!d) return;
    int removed = 0;
    for (struct dirent *e; (e = readdir(d));) {
        const char *name = e->d_name;
        const size_t len = strlen(name);
        if (len < 6 || strcmp(name + len - 5, ".json")) continue;
        if (!strcmp(name, CS_STORAGE_KEY ".json") || !strcmp(name, CS_BRAIN_KEY ".json") || !strcmp(name, CS_MODE_KEY ".json") ||
            !strcmp(name, CS_FINDER_KEY ".json")) continue;
        char path[CS_PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", dir, name);
        if (remove(path) == 0) removed++;
    }
    closedir(d);
    menu_log("clear cache & storage: %d entries removed from %s", removed, dir);
}
#endif

static void clear_answered(const int accepted, const char *text, void *user) {
    (void) text;
    (void) user;
    if (!accepted) return;
#ifdef __EMSCRIPTEN__
    // glorp: localStorage.clear(); sessionStorage.clear(); clear-cache (+ reload) -- the
    // client's own entries survive like its settings.json does
    EM_ASM({
        try {
            var keep = {};
            // (no array literal: a bare comma inside [] splits the EM_ASM macro arguments)
            var save = function (k) { var v = localStorage.getItem(k); if (v !== null) keep[k] = v; };
            save('knClient');
            save('knBrainUrl');
            localStorage.clear();
            sessionStorage.clear();
            Object.keys(keep).forEach(function (k) { localStorage.setItem(k, keep[k]); });
        } catch (e) {}
        var reload = function () { location.reload(); };
        try {
            if (window.caches) caches.keys().then(function (ks) { return Promise.all(ks.map(function (k) { return caches.delete(k); })); }).then(reload, reload);
            else reload();
        } catch (e) { reload(); }
    });
#else
    clear_storage();
    restart_client();
#endif
}

static void do_action(const char *action) {
    if (!action) return;
    if (!strcmp(action, "clearCache")) {
        ui_confirm("Are you sure you want to clear cache and local storage?", clear_answered, NULL);
    } else if (!strcmp(action, "resetClient")) {
        ui_confirm("Are you sure you want to reset all your client settings?", reset_answered, NULL);
    }
#ifndef __EMSCRIPTEN__
    else if (!strcmp(action, "openConfig")) {
        char dir[CS_PATH_MAX];
        config_dir(dir, sizeof(dir));
        make_dirs(dir);
        open_path(dir);
    } else if (!strcmp(action, "openSwapper")) {
        char dir[CS_PATH_MAX];
        client_path("swapper", dir, sizeof(dir));
        make_dirs(dir);
        open_path(dir);
    } else if (!strcmp(action, "editCSS")) {
        char dir[CS_PATH_MAX], path[CS_PATH_MAX];
        config_dir(dir, sizeof(dir));
        make_dirs(dir);
        client_path("custom.css", path, sizeof(path));
        FILE *f = fopen(path, "rb");
        if (f) {
            fclose(f);
        } else if ((f = fopen(path, "wb"))) {
            fputs("/* Krunker Box Office custom CSS: applied over the menu and HUD (Settings > Client > Custom CSS).\n"
                  " * Toggle Custom CSS off and on to reload it after editing. */\n", f);
            fclose(f);
        }
        open_path(path);
    }
#endif
    else {
        menu_log("knClientAction: unknown action %s", action);
    }
}

static const char *arg_text(const HtmlEvent *ev, const HtmlArg *args, const int argc, const int i, char *buf, const size_t n) {
    if (i >= argc) return "";
    const HtmlArg *a = &args[i];
    switch (a->kind) {
        case HTML_ARG_STR: return a->str ? a->str : "";
        case HTML_ARG_BOOL: return a->num != 0.0 ? "true" : "false";
        case HTML_ARG_NUM: snprintf(buf, n, "%.15g", a->num); return buf;
        case HTML_ARG_THIS_CHECKED: return ev && ev->checked ? "true" : "false";
        case HTML_ARG_THIS_VALUE:
            if (ev && ev->value && ev->value[0]) return ev->value;
            if (ev && ev->target && html_dom_get_value(ev->target, buf, n) >= 0) return buf; // <select> snapshot reads ""
            return "";
        default: return "";
    }
}

int ui_client_settings_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    if (!name) return -1;
    if (!strcmp(name, "clientExitPopup")) {
        // v7.1.4 clientExitPopup(): genericPopup "Are you sure you want to close krunker?" YES / NO
        ui_confirm("Are you sure you want to close krunker?", exit_answered, NULL);
        return 1;
    }
    if (!strcmp(name, "closeClient") || !strcmp(name, "window.closeClient")) {
        request_close();
        return 1;
    }
    char kbuf[64], vbuf[256];
    if (!strcmp(name, "knClientSet")) {
        const char *key = arg_text(ev, args, argc, 0, kbuf, sizeof(kbuf));
        set_client_setting(key, arg_text(ev, args, argc, 1, vbuf, sizeof(vbuf)));
        return 1;
    }
    if (!strcmp(name, "knClientAction")) {
        do_action(arg_text(ev, args, argc, 0, kbuf, sizeof(kbuf)));
        return 1;
    }
    if (!strcmp(name, "knSetRuleset")) {
        ui_client_settings_select_ruleset(arg_text(ev, args, argc, 0, kbuf, sizeof(kbuf)));
        return 1;
    }
    return -1; // not ours: the modules after it in MODULE_HANDLERS (ui_handlers.c) get the name
}
