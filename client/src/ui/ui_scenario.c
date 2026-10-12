// `client_bin --render-scenario <scenario.json> <out.png>`: renders one tools/uidiff
// scenario through the real client and its C UI logic (menu_ui.c, hud_ui.c, ui_handlers.c)
// and writes the default framebuffer as a PNG. Contract: tools/uidiff/README.md.
//
// The client runs as usual (offline local server, the scenario's map, html_ui over the
// frame) with four differences, all keyed on ui_scenario_active():
//   - main.c creates a non-resizable hidden window of exactly the scenario `size` (with
//     --headless / KRUNKNATIVE_HEADLESS=1 no window at all: an EGL pbuffer of that size,
//     client/include/headless.h) and calls ui_scenario_run() instead of its main loop;
//   - the world is never drawn: ui_scenario_draw_background() paints the scenario
//     `background` (#rrggbb, or an image path such as the reference capture itself) after
//     the frame's glClear; the first-person weapon and the class preview are drawn only
//     when `state.preview` is true;
//   - the engine's animation/timer clock and the HUD clock follow a virtual 60 Hz clock
//     (engine_set_clock_override / hud_ui_set_clock), so the capture is deterministic;
//   - html_storage (settings, class index, name) lives in a temporary XDG_CONFIG_HOME.
//
// State is applied through the same calls the game and the page use: client_set_menu_open,
// client_enter_game, menu_show_window / the windows.0.changeTab handler, menu_set_class_index,
// the MenuUi net fields + menu_update_instructions, hud_ui_* (override, chat, kill feed,
// death card, end screen, hit marker). See ui_scenario_apply().
#if defined(KRUNKNATIVE_HTML_ENGINE) && !defined(__EMSCRIPTEN__)
#include <gl.h>
#include <GLFW/glfw3.h>
#include <headless.h> // swap/current window that also work without a window (KRUNKNATIVE_HEADLESS)
#include <ui_logic.h>
#include <app_mode.h> // state.page: hub / editor / viewer pages
#include <debug_capture.h> // state.pageActions calls with arguments
#include <html_ui.h>
#include <html_dom.h>
#include <png_write.h>
#include <pcg_basic.h>
#include <stb_image.h>
#include <cJSON.h>
#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <process.h>
#endif
#include "../html/engine/engine.h"
#include <gltf.h> // gltf_set_clock_override: animated GLTF items follow the pinned clock

// Portable temp dir / env helpers (MinGW has no mkdtemp, setenv or lstat).
static int scenario_make_storage_dir(char *out, const size_t n) {
#ifdef _WIN32
    const char *tmp = getenv("TEMP");
    if (!tmp || !*tmp) tmp = ".";
    for (int attempt = 0; attempt < 100; attempt++) {
        snprintf(out, n, "%s\\kn-scenario-%d-%d", tmp, (int) _getpid(), attempt);
        if (_mkdir(out) == 0) return 1;
    }
    return 0;
#else
    snprintf(out, n, "/tmp/kn-scenario-XXXXXX");
    return mkdtemp(out) != NULL;
#endif
}

static void scenario_setenv(const char *name, const char *value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

#define SCENARIO_FRAME_MS (1000.0 / 60.0)
// Every infinite animation the menu shows has a period dividing this: #instructions
// `grow 0.8s infinite alternate` (1.6 s cycle) and #merchImg `scaleBob 0.4s alternate`
// (0.8 s). The page's animations start with the first frame at clock 0, so any capture
// clock k * cycle + anim_time shows the phase of `state.anim_time`.
#define SCENARIO_ANIM_CYCLE_MS 1600.0
#define SCENARIO_BOOT_MAX_FRAMES 3000   // page + map + local server ready
#define SCENARIO_SPAWN_MAX_FRAMES 1200  // inGame: ENT -> SPAWN round trip
#define SCENARIO_WALL_LIMIT_S 120.0     // real-time cap for each wait loop
#define SCENARIO_SETTLE_FRAMES 4        // frames between state application and capture
#define SCENARIO_RNG_SEED 0x4b4eu         // deterministic map/spawn picks

void client_tick(Client*, float, float); // main.c

typedef struct {
    int active;
    cJSON *root;
    const cJSON *state;   // state object of root (never NULL while active)
    char out[1024];
    int w, h, frames;
    double anim_time_s;
    double after_ms;      // state.after_ms > 0: capture exactly this long after the state was applied
    int preview;

    int bg_is_color;
    float bg_rgb[3];
    char bg_image[1200];
    unsigned bg_tex, bg_fbo;
    int bg_w, bg_h;

    char root_dir[1024];  // repo root with trailing slash (assets_path() minus "assets/")
    char map_path[1200];
    char storage_dir[256];
    double clock_ms;
    double last_now_s;
} Scenario;

static Scenario g_sc;

/* ------------------------------------------------------------ json helpers */

static const cJSON *st(const char *key) {
    return cJSON_GetObjectItemCaseSensitive(g_sc.state, key);
}

static int st_has(const char *key) {
    const cJSON *item = st(key);
    return item && !cJSON_IsNull(item);
}

static int st_bool(const char *key, const int fallback) {
    const cJSON *item = st(key);
    if (cJSON_IsBool(item)) return cJSON_IsTrue(item);
    if (cJSON_IsNumber(item)) return item->valuedouble != 0.0;
    return fallback;
}

static int st_int(const char *key, const int fallback) {
    const cJSON *item = st(key);
    return cJSON_IsNumber(item) ? item->valueint : fallback;
}

static const char *st_str(const char *key) {
    const cJSON *item = st(key);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static const char *obj_str(const cJSON *obj, const char *key) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static double obj_num(const cJSON *obj, const char *key, const double fallback) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(item)) return item->valuedouble;
    if (cJSON_IsBool(item)) return cJSON_IsTrue(item);
    return fallback;
}

static char *read_text(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = n >= 0 ? malloc((size_t) n + 1) : NULL;
    if (buf && fread(buf, 1, (size_t) n, f) != (size_t) n) {
        free(buf);
        buf = NULL;
    }
    if (buf) buf[n] = 0;
    fclose(f);
    return buf;
}

/* ------------------------------------------------------------ setup */

static void resolve_path(const char *path, char *out, const size_t n) {
    if (path[0] == '/') snprintf(out, n, "%s", path);
    else snprintf(out, n, "%s%s", g_sc.root_dir, path);
}

// "name" of a map JSON file, malloc'd ("" when unreadable).
static char *map_file_name(const char *path) {
    char *text = read_text(path);
    cJSON *json = text ? cJSON_Parse(text) : NULL;
    free(text);
    const char *name = obj_str(json, "name");
    char *copy = strdup(name ? name : "");
    cJSON_Delete(json);
    return copy;
}

// assets/maps/<file>.json whose "name" is `name`: the lowercase/underscore file name
// first ("Soul Sanctum" -> soul_sanctum.json), else every map file.
static int find_map(const char *name, char *out, const size_t n) {
    const char *assets = assets_path();
    char guess[128];
    size_t len = 0;
    for (const char *p = name; *p && len + 1 < sizeof(guess); p++) guess[len++] = *p == ' ' ? '_' : (char) tolower((unsigned char) *p);
    guess[len] = 0;
    snprintf(out, n, "%smaps/%s.json", assets, guess);
    char *found = map_file_name(out);
    const int ok = !strcmp(found, name);
    free(found);
    if (ok) return 1;

    char dir_path[512];
    snprintf(dir_path, sizeof(dir_path), "%smaps", assets);
    DIR *dir = opendir(dir_path);
    if (!dir) return 0;
    int hit = 0;
    for (struct dirent *e; !hit && (e = readdir(dir));) {
        const size_t l = strlen(e->d_name);
        if (l < 6 || strcmp(e->d_name + l - 5, ".json") != 0) continue;
        snprintf(out, n, "%s/%s", dir_path, e->d_name);
        char *map_name = map_file_name(out);
        hit = !strcmp(map_name, name);
        free(map_name);
    }
    closedir(dir);
    return hit;
}

static void warn_unknown_keys(void) {
    static const char *const known[] = {
        "menu", "window", "settingsTab", "connected", "ready", "inGame", "mapName", "modeName", "region", "name",
        "gameName", "classIndex", "anim_time", "hud", "chat", "chatOpen", "chatInput", "killfeed", "scoreboard",
        "dead", "killcard", "endscreen", "hit", "preview", "scrollbars", "page", "pageQuery", "pageActions",
        "clientChrome", "settingsView", "windowActions", "after_ms",
    };
    const cJSON *item;
    cJSON_ArrayForEach(item, g_sc.state) {
        int ok = 0;
        for (size_t i = 0; i < sizeof(known) / sizeof(known[0]) && !ok; i++) ok = !strcmp(item->string, known[i]);
        if (!ok) fprintf(stderr, "[scenario] warning: unknown state key '%s' ignored\n", item->string);
    }
}

int ui_scenario_begin(const char *scenario_path, const char *out_png) {
    memset(&g_sc, 0, sizeof(g_sc));
    snprintf(g_sc.out, sizeof(g_sc.out), "%s", out_png);

    // assets_path() is "./assets/" (repo root) or "../assets/" (cmake-build-*): the
    // scenario's repo-relative paths resolve against the same root.
    const char *assets = assets_path();
    const size_t alen = strlen(assets);
    snprintf(g_sc.root_dir, sizeof(g_sc.root_dir), "%.*s", (int) (alen >= 7 ? alen - 7 : 0), assets);

    char *text = read_text(scenario_path);
    if (!text) {
        fprintf(stderr, "[scenario] cannot read %s\n", scenario_path);
        return 0;
    }
    g_sc.root = cJSON_Parse(text);
    free(text);
    if (!cJSON_IsObject(g_sc.root)) {
        fprintf(stderr, "[scenario] %s is not a JSON object\n", scenario_path);
        return 0;
    }

    const cJSON *size = cJSON_GetObjectItemCaseSensitive(g_sc.root, "size");
    if (!cJSON_IsArray(size) || cJSON_GetArraySize(size) != 2) {
        fprintf(stderr, "[scenario] `size` must be [w, h]\n");
        return 0;
    }
    g_sc.w = cJSON_GetArrayItem(size, 0)->valueint;
    g_sc.h = cJSON_GetArrayItem(size, 1)->valueint;
    if (g_sc.w <= 0 || g_sc.h <= 0) {
        fprintf(stderr, "[scenario] bad size %dx%d\n", g_sc.w, g_sc.h);
        return 0;
    }
    g_sc.frames = (int) obj_num(g_sc.root, "frames", 3);
    if (g_sc.frames < 1) g_sc.frames = 1;

    static cJSON empty_state = {.type = cJSON_Object};
    const cJSON *state = cJSON_GetObjectItemCaseSensitive(g_sc.root, "state");
    g_sc.state = cJSON_IsObject(state) ? state : &empty_state;
    warn_unknown_keys();
    g_sc.anim_time_s = obj_num(g_sc.state, "anim_time", 0.0);
    if (g_sc.anim_time_s < 0.0) g_sc.anim_time_s = 0.0;
    // one-shot animations (the death screen's 1.5 s slide-in): the reference was taken a fixed
    // time after the event, so the capture follows the state by that much instead of the cycle
    g_sc.after_ms = obj_num(g_sc.state, "after_ms", 0.0);
    g_sc.preview = st_bool("preview", 0);

    // background: "#rrggbb" or an image path; default opaque black
    const char *bg = obj_str(g_sc.root, "background");
    g_sc.bg_is_color = 1;
    if (bg && bg[0] == '#' && strlen(bg) == 7) {
        const unsigned v = (unsigned) strtoul(bg + 1, NULL, 16);
        g_sc.bg_rgb[0] = (float) ((v >> 16) & 255) / 255.0f;
        g_sc.bg_rgb[1] = (float) ((v >> 8) & 255) / 255.0f;
        g_sc.bg_rgb[2] = (float) (v & 255) / 255.0f;
    } else if (bg && bg[0]) {
        g_sc.bg_is_color = 0;
        resolve_path(bg, g_sc.bg_image, sizeof(g_sc.bg_image));
        struct stat sb;
        if (stat(g_sc.bg_image, &sb) != 0) {
            fprintf(stderr, "[scenario] background '%s' is neither #rrggbb nor a readable image (%s)\n", bg, g_sc.bg_image);
            return 0;
        }
    }

    // state.mapName: start the local server on that map file, exactly like
    // `client_bin <map.json>` (the map list becomes [that map] + the rotation). Without
    // it the client keeps its default rotation list (what the host/games windows show)
    // and the seeded RNG below makes its random map pick repeatable.
    const char *map_name = st_str("mapName");
    if (map_name && map_name[0] && !find_map(map_name, g_sc.map_path, sizeof(g_sc.map_path))) {
        fprintf(stderr, "[scenario] warning: no map named '%s' in %smaps, using the rotation\n", map_name, assets);
        g_sc.map_path[0] = 0;
    }
    pcg32_srandom(SCENARIO_RNG_SEED, 1u); // map/mode/spawn picks
    srand(SCENARIO_RNG_SEED);             // menu speech bubbles
    const char *mode = st_str("modeName");
    if (mode && strcmp(mode, "Free for All") != 0) fprintf(stderr, "[scenario] warning: modeName '%s': only Free for All is available\n", mode);

    // html_storage (settings, class index, name) must not read or write the user's files.
    if (!scenario_make_storage_dir(g_sc.storage_dir, sizeof(g_sc.storage_dir))) {
        fprintf(stderr, "[scenario] could not create a temporary storage directory\n");
        g_sc.storage_dir[0] = 0;
        return 0;
    }
    scenario_setenv("XDG_CONFIG_HOME", g_sc.storage_dir);

    // The page's animations start at the first style flush: pin the clock before html_ui_init.
    engine_set_clock_override(0.0);
    // Most of doc/ref was captured with headless Chromium's --hide-scrollbars; state.scrollbars
    // = true keeps them (references captured over CDP without it, e.g. win-reticle).
    engine_set_hide_scrollbars(!st_bool("scrollbars", 0));
    hud_ui_set_clock(0.0);
    g_sc.active = 1;
    return 1;
}

int ui_scenario_active(void) { return g_sc.active; }

// state.clientChrome: the desktop client's additions (Exit menu item, Settings > Client
// tab, custom CSS) are drawn; otherwise a scenario renders krunker.io's browser page,
// which is what the v3.1.1 reference captures show.
int ui_scenario_client_chrome(void) { return g_sc.active && st_bool("clientChrome", 0); }

void ui_scenario_window_size(int *w, int *h) {
    *w = g_sc.w;
    *h = g_sc.h;
}

const char *ui_scenario_map_path(void) { return g_sc.map_path[0] ? g_sc.map_path : NULL; }

int ui_scenario_draws_preview(void) { return g_sc.preview; }

static void remove_tree(const char *path) {
    DIR *dir = opendir(path);
    if (dir) {
        for (struct dirent *e; (e = readdir(dir));) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char child[512];
            snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
            struct stat sb;
#ifdef _WIN32
            if (stat(child, &sb) == 0 && S_ISDIR(sb.st_mode)) remove_tree(child);
#else
            if (lstat(child, &sb) == 0 && S_ISDIR(sb.st_mode)) remove_tree(child);
#endif
            else unlink(child);
        }
        closedir(dir);
    }
    rmdir(path);
}

void ui_scenario_end(void) {
    if (g_sc.storage_dir[0]) remove_tree(g_sc.storage_dir);
    g_sc.storage_dir[0] = 0;
    if (g_sc.bg_fbo) glDeleteFramebuffers(1, &g_sc.bg_fbo);
    if (g_sc.bg_tex) glDeleteTextures(1, &g_sc.bg_tex);
    g_sc.bg_fbo = g_sc.bg_tex = 0;
    hud_ui_set_override(NULL);
    hud_ui_set_override_scoreboard(NULL);
    cJSON_Delete(g_sc.root);
    g_sc.root = NULL;
    g_sc.active = 0;
}

/* ------------------------------------------------------------ background */

static int background_init(void) {
    if (g_sc.bg_is_color) return 1;
    int n = 0;
    stbi_set_flip_vertically_on_load_thread(0);
    unsigned char *px = stbi_load(g_sc.bg_image, &g_sc.bg_w, &g_sc.bg_h, &n, 4);
    if (!px) {
        fprintf(stderr, "[scenario] cannot decode background %s\n", g_sc.bg_image);
        return 0;
    }
    glGenTextures(1, &g_sc.bg_tex);
    glBindTexture(GL_TEXTURE_2D, g_sc.bg_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, g_sc.bg_w, g_sc.bg_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);
    g_active_texture = 0; // the client's texture-binding cache (main.c)
    stbi_image_free(px);
    glGenFramebuffers(1, &g_sc.bg_fbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_sc.bg_fbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_sc.bg_tex, 0);
    const GLenum status = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "[scenario] background framebuffer incomplete (0x%x)\n", status);
        return 0;
    }
    return 1;
}

// Called by client_tick() right after its glClear: the world is replaced by the
// scenario background (flat colour, or the image stretched over the framebuffer).
void ui_scenario_draw_background(void) {
    if (!g_sc.active) return;
    GLfloat clear[4];
    glGetFloatv(GL_COLOR_CLEAR_VALUE, clear);
    const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(g_sc.bg_rgb[0], g_sc.bg_rgb[1], g_sc.bg_rgb[2], 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glClearColor(clear[0], clear[1], clear[2], clear[3]);
    // The window's stencil is never cleared by the client and starts undefined (non-zero
    // on NVIDIA, 0 on llvmpipe). Dirty it like a real driver may so a renderer that relies
    // on its contents (the HTML engine's nested clips once did) fails uidiff on llvmpipe too.
    GLint clear_stencil = 0;
    glGetIntegerv(GL_STENCIL_CLEAR_VALUE, &clear_stencil);
    glStencilMask(0xFF);
    glClearStencil(0xFF);
    glClear(GL_STENCIL_BUFFER_BIT);
    glClearStencil(clear_stencil);
    if (g_sc.bg_tex) {
        // image rows are top-down: flip while blitting
        GLint draw = 0;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, g_sc.bg_fbo);
        glBlitFramebuffer(0, 0, g_sc.bg_w, g_sc.bg_h, 0, g_sc.h, g_sc.w, 0, GL_COLOR_BUFFER_BIT,
                          g_sc.bg_w == g_sc.w && g_sc.bg_h == g_sc.h ? GL_NEAREST : GL_LINEAR);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint) draw);
    }
    if (scissor) glEnable(GL_SCISSOR_TEST);
}

// class_preview.c dims the scene behind the model (PREVIEW_BACKDROP_ALPHA 0.5) inside the
// hole the page cuts into #instructionHolder. Without the preview that rect would show
// the background undimmed: fill it with the dimmed flat colour instead. An image
// background (a reference capture) already holds the real preview pixels there.
#define SCENARIO_PREVIEW_BACKDROP_ALPHA 0.5f

void ui_scenario_preview_backdrop(Client *client) {
    const ClassPreview *p = &client->class_preview;
    if (!g_sc.active || !g_sc.bg_is_color || !html_ui_is_menu_open() || !p->has_rect || !p->visible) return;
    const int x = (int) lroundf(p->x), top = (int) lroundf(p->y);
    const int w = (int) lroundf(p->width), h = (int) lroundf(p->height);
    if (w <= 0 || h <= 0) return;
    GLfloat clear[4];
    GLint box[4];
    glGetFloatv(GL_COLOR_CLEAR_VALUE, clear);
    const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    glGetIntegerv(GL_SCISSOR_BOX, box);
    glEnable(GL_SCISSOR_TEST);
    glScissor(x, g_sc.h - top - h, w, h);
    const float k = 1.0f - SCENARIO_PREVIEW_BACKDROP_ALPHA;
    glClearColor(g_sc.bg_rgb[0] * k, g_sc.bg_rgb[1] * k, g_sc.bg_rgb[2] * k, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glClearColor(clear[0], clear[1], clear[2], clear[3]);
    glScissor(box[0], box[1], box[2], box[3]);
    if (!scissor) glDisable(GL_SCISSOR_TEST);
}

/* ------------------------------------------------------------ frames */

// One client frame at virtual time `clock_ms` (the real main loop's client_frame with
// a pinned clock). `present` = 0 keeps the back buffer for the read-back.
static void scenario_frame(Client *client, const double clock_ms, const int present) {
    g_sc.clock_ms = clock_ms;
    engine_set_clock_override(clock_ms);
    hud_ui_set_clock(clock_ms / 1000.0);
    gltf_set_clock_override(clock_ms / 1000.0);
    const double now_s = clock_ms / 1000.0;
    double delta = now_s - g_sc.last_now_s;
    if (delta <= 0.0) delta = SCENARIO_FRAME_MS / 1000.0;
    g_sc.last_now_s = now_s;
    client->last_frame_time = now_s;
    client_tick(client, (float) now_s, (float) delta);
    if (present) headless_swap_buffers(client->window);
    glfwPollEvents();
}

static int game_connected(const Client *c) {
    return c->game.is_local || c->offline || c->net_connected;
}

static int boot_done(const Client *c) {
    const MenuUi *ui = menu_ui();
    return html_ui_is_ready() && ui->dom_ready && c->game.ready && c->map_loaded && game_connected(c);
}

static int spawned(const Client *c) {
    return c->in_game && c->me && c->me->active;
}

// Runs frames until `done(client)` (or the frame / wall-clock cap). Returns 1 when done.
static int run_until(Client *client, int (*done)(const Client*), const int max_frames, const char *what) {
    const double wall0 = glfwGetTime();
    for (int i = 0; i < max_frames; i++) {
        if (done(client)) return 1;
        scenario_frame(client, g_sc.clock_ms + SCENARIO_FRAME_MS, 1);
        if (glfwGetTime() - wall0 > SCENARIO_WALL_LIMIT_S) break;
    }
    if (done(client)) return 1;
    fprintf(stderr, "[scenario] timed out waiting for %s\n", what);
    return 0;
}

static void run_frames(Client *client, const int n) {
    for (int i = 0; i < n; i++) scenario_frame(client, g_sc.clock_ms + SCENARIO_FRAME_MS, 1);
}

/* ------------------------------------------------------------ state */

// state.window -> MenuUi window id. Scenario ids are v7.1.4 showWindow(N) ids except 0, which
// is windows[0] (settings; showWindow(1) in Krunker and menu_ui.c); "host" = 8 (Host Custom
// Game), "join" = 24.
static int scenario_window_id(const cJSON *window, int *id) {
    if (cJSON_IsNumber(window)) {
        *id = window->valueint == 0 ? 1 : window->valueint;
        return 1;
    }
    if (cJSON_IsString(window)) {
        if (!strcmp(window->valuestring, "host")) *id = MENU_WINDOW_HOST;
        else if (!strcmp(window->valuestring, "join")) *id = MENU_WINDOW_JOIN;
        else {
            fprintf(stderr, "[scenario] warning: unknown window '%s'\n", window->valuestring);
            return 0;
        }
        return 1;
    }
    return 0;
}

// state.windowActions: inline-handler calls dispatched once the window is open, as the
// window's own markup would (["windows.2.switchTab", 0], ["windows.6.switchTab", 2], ...):
// each entry is [name, args...] with number / string / bool arguments.
static void apply_window_actions(Client *client) {
    const cJSON *actions = st("windowActions");
    const cJSON *action;
    cJSON_ArrayForEach(action, actions) {
        const cJSON *name = cJSON_IsArray(action) ? cJSON_GetArrayItem(action, 0) : action;
        if (!cJSON_IsString(name)) continue;
        HtmlArg args[8];
        int argc = 0;
        for (const cJSON *a = cJSON_IsArray(action) ? name->next : NULL; a && argc < 8; a = a->next) {
            HtmlArg *arg = &args[argc++];
            memset(arg, 0, sizeof(*arg));
            if (cJSON_IsNumber(a)) { arg->kind = HTML_ARG_NUM; arg->num = a->valuedouble; }
            else if (cJSON_IsBool(a)) { arg->kind = HTML_ARG_BOOL; arg->num = cJSON_IsTrue(a); }
            else { arg->kind = HTML_ARG_STR; arg->str = cJSON_IsString(a) ? a->valuestring : ""; }
        }
        HtmlEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = "click";
        ev.value = "";
        html_ui_dispatch(name->valuestring, &ev, argc ? args : NULL, argc);
        run_frames(client, 2);
    }
}

static void apply_window(Client *client) {
    int id;
    if (!scenario_window_id(st("window"), &id)) return;
    if (id == 1) {
        // windows[0].settingType: "basic" (v7's default) or "advanced" (state.settingsView)
        const char *view = st_str("settingsView");
        if (view) menu_ui()->settings_view = !strcmp(view, "advanced");
        else if (st_has("settingsView")) menu_ui()->settings_view = st_int("settingsView", 0) != 0;
    }
    if (id == 1 && st_has("settingsTab")) {
        // windows[0].changeTab(N): the page's own handler (stores the tab, opens settings)
        const HtmlArg arg = {HTML_ARG_NUM, (double) st_int("settingsTab", 0), NULL};
        HtmlEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = "click";
        ev.value = "";
        html_ui_dispatch("windows.0.changeTab", &ev, &arg, 1);
        if (menu_ui()->active_window != 1) menu_show_window(1, 1);
    } else {
        menu_show_window(id, 1);
    }
    apply_window_actions(client);
}

// connected/ready: the `connect` + `state` net fields that drive #instructions and the
// region label. Applied last (the client's own state mirror rewrites them on changes).
static void apply_net_state(void) {
    MenuUi *ui = menu_ui();
    if (st_has("connected")) ui->connected = st_bool("connected", ui->connected);
    if (st_has("ready")) ui->ready = st_bool("ready", ui->ready);
    menu_update_top_bar();
    menu_update_instructions();
}

static void apply_chat(void) {
    const cJSON *chat = st("chat");
    if (!cJSON_IsArray(chat)) return;
    hud_ui_clear_chat();
    const cJSON *row;
    cJSON_ArrayForEach(row, chat) {
        const char *name = obj_str(row, "name"), *msg = obj_str(row, "msg");
        const char *text = obj_str(row, "text"), *color = obj_str(row, "color");
        if (name && msg) {
            hud_ui_add_chat(name, msg, NULL);
            continue;
        }
        // White rows are Krunker's plain game lines (type 1: .chatMsg default colour).
        const int plain = !color || !strcasecmp(color, "#ffffff") || !strcasecmp(color, "#fff");
        hud_ui_add_chat(NULL, text ? text : "", plain ? NULL : color);
    }
}

static void apply_chat_input(void) {
    const HtmlElem input = html_dom_by_id("chatInput");
    if (!input) return;
    const char *value = st_str("chatInput");
    if (value) html_dom_set_value(input, value);
    if (st_bool("chatOpen", 0)) html_dom_focus(input);
}

static void apply_killcard(Client *client) {
    if (!st_bool("dead", 0)) return;
    const cJSON *kc = st("killcard");
    HudKillCard card = {0};
    if (cJSON_IsObject(kc)) {
        card.killer = obj_str(kc, "killer");
        card.weapon = obj_str(kc, "weapon");
        card.level = (int) obj_num(kc, "level", 1);
        card.score = (int) obj_num(kc, "score", 0);
        card.class_index = (int) obj_num(kc, "classIndex", 0);
        // v7 death screen: weaponIcon / myLevel and the life's stats [timeAlive ms, shots, "acc%", kills, score]
        card.weapon_icon = obj_str(kc, "weaponIcon");
        card.my_level = (int) obj_num(kc, "myLevel", 0);
        const cJSON *stats = cJSON_GetObjectItemCaseSensitive(kc, "stats");
        if (cJSON_IsArray(stats) && cJSON_GetArraySize(stats) >= 5) {
            card.stats = 1;
            card.time_alive_ms = cJSON_GetArrayItem(stats, 0)->valueint;
            card.shots = cJSON_GetArrayItem(stats, 1)->valueint;
            card.accuracy = cJSON_GetStringValue(cJSON_GetArrayItem(stats, 2));
            card.kills = cJSON_GetArrayItem(stats, 3)->valueint;
            card.life_score = cJSON_GetArrayItem(stats, 4)->valueint;
        }
    }
    hud_ui_show_death(client, cJSON_IsObject(kc) ? &card : NULL);
}

static void apply_endscreen(Client *client) {
    const cJSON *end = st("endscreen");
    if (!cJSON_IsObject(end)) return;
    // FFA: first place wins. `text` ("#1 VICTORY") is the overlay-canvas title of the end
    // animation (aV.vicTitle), not page markup: the reference background carries it.
    const int place = (int) obj_num(end, "place", 1);
    const cJSON *result = cJSON_GetObjectItemCaseSensitive(end, "result");
    hud_ui_show_end_screen(client, cJSON_IsNumber(result) ? result->valueint : place == 1, obj_str(end, "nextRound"));
}

// Time-dependent pieces, applied at `clock_ms` for a capture at `capture_ms`.
static void apply_timed(Client *client, const double capture_ms) {
    const double ahead = capture_ms - g_sc.clock_ms; // time until the captured frame
    const cJSON *feed = st("killfeed");
    if (cJSON_IsArray(feed)) {
        const cJSON *row;
        cJSON_ArrayForEach(row, feed) {
            const char *icon = obj_str(row, "weaponIcon");
            hud_ui_add_killfeed_ex(client, obj_str(row, "killer"), (int) obj_num(row, "killerIsYou", 0),
                                   obj_str(row, "victim"), (int) obj_num(row, "victimIsYou", 0),
                                   icon, (int) obj_num(row, "headshot", 0), obj_num(row, "age_ms", 0.0) - ahead);
        }
    }
    const cJSON *hit = st("hit");
    if (cJSON_IsObject(hit)) {
        hud_ui_hit_elapsed(client, (int) obj_num(hit, "kill", 0), obj_num(hit, "elapsed_ms", 0.0) - ahead);
    }
}

// state.page ("hub" | "editor" | "viewer") + state.pageQuery: show that page (app_mode.h)
// like `client_bin --hub "<query>"`; state.pageActions: handler names dispatched once the
// page is up (e.g. "openSettings", "loginPopup"), or calls with literal arguments
// ("showWindow(3)"), as the page's inline handlers would.
static int page_entered(const Client *c) {
    (void) c;
    return app_mode() != APP_MODE_GAME && html_ui_is_ready();
}

static int apply_page(Client *client) {
    const char *page = st_str("page");
    if (!page || !page[0]) return 1;
    AppMode mode = APP_MODE_COUNT;
    for (int m = 1; m < APP_MODE_COUNT; m++) {
        if (!strcmp(page, app_mode_name((AppMode) m))) mode = (AppMode) m;
    }
    if (mode == APP_MODE_COUNT) {
        fprintf(stderr, "[scenario] unknown page '%s'\n", page);
        return 0;
    }
    app_set_mode(client, mode, st_str("pageQuery"));
    if (!run_until(client, page_entered, SCENARIO_BOOT_MAX_FRAMES, "the page")) return 0;
    run_frames(client, 6); // enter() + the server's replies (in-process: a frame or two)
    const cJSON *actions = st("pageActions");
    const cJSON *action;
    cJSON_ArrayForEach(action, actions) {
        if (!cJSON_IsString(action)) continue;
        if (strchr(action->valuestring, '(')) {
            debug_capture_dispatch(action->valuestring); // "showWindow(3)": a call with arguments
        } else {
            HtmlEvent ev = {.type = "click", .value = ""};
            html_ui_dispatch(action->valuestring, &ev, NULL, 0);
        }
        run_frames(client, 2);
    }
    return 1;
}

static int ui_scenario_apply(Client *client) {
    if (st_str("page")) return apply_page(client);
    const int in_game = st_bool("inGame", 0);

    // identity: name tag, class box, region label
    const char *name = st_str("name");
    if (name && name[0]) {
        snprintf(client->menu_name, sizeof(client->menu_name), "%s", name);
        menu_update_class_box();
    }
    if (st_has("classIndex")) menu_set_class_index(st_int("classIndex", 0), 1);
    menu_set_region_label(st_str("region"));

    // HUD values before the first HUD write
    const cJSON *hud = st("hud");
    if (cJSON_IsObject(hud)) hud_ui_set_override(hud);
    const cJSON *board = st("scoreboard");
    if (cJSON_IsArray(board)) hud_ui_set_override_scoreboard(board);

    if (in_game) {
        // what `play` does: close the menu, ENT -> SPAWN through the local server
        client_set_menu_open(client, 0);
        client_enter_game(client);
        client->menu_lock_grace_until = 1e18; // a hidden window never holds the pointer lock
        if (!run_until(client, spawned, SCENARIO_SPAWN_MAX_FRAMES, "the local player to spawn")) return 0;
        run_frames(client, 2); // HUD visible + first value write (session edge seeds chat)
        apply_chat();
    }

    const int menu_open = st_bool("menu", !in_game);
    if (menu_open != html_ui_is_menu_open()) client_set_menu_open(client, menu_open);
    if (in_game) client->menu_lock_grace_until = 1e18;
    if (st_has("gameName")) menu_set_game_name(st_str("gameName"));
    if (menu_open) apply_window(client);

    run_frames(client, 2);
    apply_killcard(client);
    apply_endscreen(client);
    apply_chat_input();
    apply_net_state();
    return 1;
}

/* ------------------------------------------------------------ capture */

static int read_back(const char *path) {
    int fb_w = 0, fb_h = 0;
    glfwGetFramebufferSize(headless_current_window(), &fb_w, &fb_h);
    if (fb_w != g_sc.w || fb_h != g_sc.h) {
        fprintf(stderr, "[scenario] framebuffer is %dx%d, scenario wants %dx%d\n", fb_w, fb_h, g_sc.w, g_sc.h);
        return 0;
    }
    const size_t stride = (size_t) g_sc.w * 4;
    unsigned char *px = malloc(stride * (size_t) g_sc.h);
    unsigned char *flip = malloc(stride * (size_t) g_sc.h);
    if (!px || !flip) {
        free(px);
        free(flip);
        return 0;
    }
    glFinish();
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glReadBuffer(GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, g_sc.w, g_sc.h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    for (int y = 0; y < g_sc.h; y++) {  // GL rows are bottom-up
        memcpy(flip + (size_t) y * stride, px + (size_t) (g_sc.h - 1 - y) * stride, stride);
        for (int x = 0; x < g_sc.w; x++) flip[(size_t) y * stride + (size_t) x * 4 + 3] = 255;
    }
    const int ok = png_write_rgba(path, flip, g_sc.w, g_sc.h);
    free(px);
    free(flip);
    return ok;
}

static void dump_line(const char *line, void *user) {
    (void) user;
    fprintf(stderr, "%s\n", line);
}

int ui_scenario_run(Client *client) {
    if (!g_sc.active) return 2;
    if (!html_ui_available()) {
        fprintf(stderr, "[scenario] the HTML UI is not available\n");
        return 1;
    }
    if (!background_init()) return 1;

    // boot: first frame at clock 0 (the page's animation origin), then 60 Hz
    g_sc.last_now_s = -1.0;
    scenario_frame(client, 0.0, 1);
    if (!run_until(client, boot_done, SCENARIO_BOOT_MAX_FRAMES, "the page, map and local server")) return 1;
    if (!ui_scenario_apply(client)) return 1;
    const double applied_ms = g_sc.clock_ms;
    run_frames(client, SCENARIO_SETTLE_FRAMES);

    // capture clock: the first k * cycle + anim_time after the settle frames, reached by
    // one jump and `frames` 60 Hz frames ending exactly on it
    const double anim_ms = g_sc.anim_time_s * 1000.0;
    const double earliest = g_sc.clock_ms + (double) g_sc.frames * SCENARIO_FRAME_MS;
    const double k = ceil((earliest - anim_ms) / SCENARIO_ANIM_CYCLE_MS);
    double capture_ms = (k > 0.0 ? k : 0.0) * SCENARIO_ANIM_CYCLE_MS + anim_ms;
    if (g_sc.after_ms > 0.0 && applied_ms + g_sc.after_ms >= earliest) capture_ms = applied_ms + g_sc.after_ms;
    const double first_ms = capture_ms - (double) (g_sc.frames - 1) * SCENARIO_FRAME_MS;

    g_sc.clock_ms = first_ms - SCENARIO_FRAME_MS; // timed state is applied one frame before the first
    engine_set_clock_override(g_sc.clock_ms);
    hud_ui_set_clock(g_sc.clock_ms / 1000.0);
    apply_timed(client, capture_ms);
    for (int i = 0; i < g_sc.frames; i++) {
        const int last = i == g_sc.frames - 1;
        scenario_frame(client, first_ms + (double) i * SCENARIO_FRAME_MS, !last);
    }
    // Debug: KRUNKNATIVE_SCENARIO_DUMP=<selector> prints the captured frame's box tree of
    // the first match (engine_dump_boxes), for routing layout findings.
    const char *dump = getenv("KRUNKNATIVE_SCENARIO_DUMP");
    if (dump && dump[0]) {
        const HtmlElem e = html_dom_query(0, dump);
        if (e && engine_get()) engine_dump_boxes(engine_get(), e, dump_line, NULL);
        else fprintf(stderr, "[scenario] dump: no match for '%s'\n", dump);
    }
    const int ok = read_back(g_sc.out);
    if (ok) printf("[scenario] %dx%d at %.3f s -> %s\n", g_sc.w, g_sc.h, capture_ms / 1000.0, g_sc.out);
    return ok ? 0 : 1;
}

#elif !defined(__EMSCRIPTEN__)
// KRUNKNATIVE_HTML_ENGINE=OFF: no page to render, so no scenario is ever active (ui_logic.h
// still declares the API for the native callers).
#include <ui_logic.h>
#include <stdio.h>

int ui_scenario_begin(const char *scenario_path, const char *out_png) {
    (void) out_png;
    fprintf(stderr, "[scenario] %s: this client was built without the HTML engine\n", scenario_path ? scenario_path : "");
    return 0;
}
int ui_scenario_active(void) { return 0; }
int ui_scenario_client_chrome(void) { return 0; }
void ui_scenario_window_size(int *w, int *h) { (void) w; (void) h; }
const char *ui_scenario_map_path(void) { return NULL; }
int ui_scenario_run(Client *client) { (void) client; return 1; }
void ui_scenario_end(void) {}
void ui_scenario_draw_background(void) {}
int ui_scenario_draws_preview(void) { return 0; }
void ui_scenario_preview_backdrop(Client *client) { (void) client; }
#endif // KRUNKNATIVE_HTML_ENGINE && !__EMSCRIPTEN__
