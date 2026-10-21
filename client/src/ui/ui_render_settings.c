// Krunker v3.1.1 display/render/game settings on the client (render_settings.h).
//
// The store (ui_settings.c) hands every changed or pushed key to client_apply_setting(),
// which calls client_apply_render_setting() first. Each key below is one row of
// Krunker.Unpacked-v3.1.1/134.js `this.list`; the comment says what its `set` does there
// and what KrunkNative does with it. Values the renderer acts on are applied here or read
// per frame (render_target.c, main.c); the rest are kept in ClientRenderSettings for the
// system that draws the thing (HUD, VFX, viewmodel), see the report table in the header.
#include <render_settings.h>
#include <ui_logic.h>
#include <html_dom.h>
#include <html_ui.h>
#include <GLFW/glfw3.h>
#include <headless.h> // headless_swap_interval
#include <client_settings.h> // Settings > Client "Resolution" (exclusive fullscreen mode)
#include <window_state.h>    // the window across F4 / relaunch / restarts
#include <app_mode.h>        // app_window_has_router: page windows keep no geometry
#include <ctype.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#elif defined(_WIN32)
#include <windows.h>
#undef near // minwindef.h empty macros: Camera has near/far members
#undef far
#else
#include <time.h>
#endif

// 134.js defaults (`val`), colours from 13.js (COLORS).
static ClientRenderSettings g_rs = {
    // Render
    .resolution = 0.6f, .frame_cap = 0, .anti_alias = 0, .post_processing = 1,
    .low_spec = 0, .no_textures = 0, .map_details = 1,
    .particles = 1, .particles_dist = 160.0f, .render_dist = 100.0f,
    .shadows = 1, .soft_shadows = 0, .high_res_shadows = 0, .dynamic_shadows = 0, .ambient_shading = 1,
    .show_trails = 1, .your_trails = 1, .muzzle_flash = 1, .bullet_tracers = 1, .bullet_tracer_color = "#ffffdb",
    .sniper_flap = 0, .texture_anim = 1, .object_anim = 1, .screen_shake = 1, .weapon_shine = 0, .lighting = 1,
    .hud_health_high = "#9eeb56", .hud_health_low = "#eb5656",
    .speed_lines = 0.6f, .speed_lines_color = "#e6e6e6",
    .show_popup_score = 1, .popup_score_color = "#ffea78", .popup_score_shadow = 0,
    .popup_score_scale = 1.0f, .popup_score_off_x = 5.0f, .popup_score_off_y = 3.7f,
    .saturation = 1.0f, .hue = 0.0f, .menu_saturation = 1.0f, .menu_hue = 0.0f, .vignette = 0.0f, .feed_limit = 8,
    // Display
    .show_ui = 1, .old_scoreboard = 0, .old_browser = 0, .disable_rarity_anim = 0, .show_pfp = 1,
    .dynamic_hp = 1, .show_hit_ind = 1, .hit_color = "#FF3F3F", .show_dmg = 1, .dmg_color = 1,
    .crit_color = "#FFD542", .dmg_scale = 1.0f,
    .show_kills = 1, .show_kill_c = 0, .show_deaths = 0, .show_score = 0, .show_streak = 0,
    .show_ping = 1, .show_net_stat = 0, .show_fps = 0, .fps_rate = 100.0f, .dmg_type = 1, .damage_number_mode = 2, // Legacy: FRVR's default_damage_numbers_mode (1.37.2 config)
    .show_speed = 0, .speed_off_x = 5.0f, .speed_off_y = 5.4f, .speed_scale = 2.4f, .speed_opacity = 1.0f,
    .speed_color = "#ffea78", .speed_color_max = "#ff0000",
    .show_medals = 1, .hide_names = 0, .nametag_opacity = 1.0f, .nametag_scale = 1.0f, .nametag_style = 0, .health_num = 0,
    .health_col_team = "#9eeb56", .health_col_enemy = "#eb5656",
    .kpd_vision = 1, .kpd_vision_col = "#00ffff", // v10 settings kpdVision (val true) / kpdVisionCol
    // Player chat is on by default, as v7's and v10's showMessages are (doc/v7/player-viewer.md:1722); the switch hides it, see apply_show_messages
    .chat_filter = 0, .show_messages = 1, .show_unboxings = 1, .chat_text_outline = 0,
    .chat_opacity = 1.0f, .chat_bg_opacity = 0.2f, .chat_height = 2.5f, .chat_timer = 0.0f,
    .crosshair_type = 1, .crosshair_style = 0, .crosshair_opacity = 1.0f,
    .crosshair_use_opacity = 0, .crosshair_always = 0, .crosshair_dot = 1, // crosshairDot val true (v7.1.4 and v10)
    .crosshair_color = "#ffffff", .crosshair_shadow = "#000000",
    .crosshair_shadow_thickness = 1.0f, .crosshair_thick = 2.0f, .crosshair_len = 16.0f, .crosshair_gap = 0.0f,
    .crosshair_type_m = 3, .crosshair_style_m = 0, .crosshair_opacity_m = 1.0f, .crosshair_color_m = "#ffffff",
    .crosshair_shadow_m = "#000000", .crosshair_shadow_thickness_m = 1.0f, .crosshair_thick_m = 3.0f,
    .crosshair_len_m = 16.0f, .crosshair_gap_m = 14.0f,
    .hitmarker = 1, .hitmarker_color = "#FFFFFF", .hitmarker_kill_color = "#FF0000",
    .hitmarker_opacity = 1.0f, .hitmarker_length = 25.0f, .hitmarker_thickness = 7.0f, .hitmarker_spacing = 37.0f,
    .hitmarker_anim_size = 0.14f, .hitmarker_anim_speed = 0.008f, .hitmarker_fade = 0.0062f,
    // Game
    .weapon_bob = 0.9f, .weapon_lean = 1.0f, .weapon_rot = 0.0f,
    .weapon_off_x = 1.0f, .weapon_off_y = 1.0f, .weapon_off_z = 1.0f, .weapon_ads_off_y = 1.0f,
    .left_hand = 0, .aim_anim = 1, .show_hands = 1, .round_arms = 0, .hide_ads = 0,
    .weap_swap_y = 1.0f, .weap_reload_y = 1.0f, .wall_lean = 0, .wall_lean_mlt = 0.5f,
    .can_load_mods = 1, .can_change_logo = 1, .auto_load_last = 0, .scope_borders = 1,
    .scope_border_color = "#000000", .scope_border_opacity = 1.0f, .scope_width = 6.8f, .scope_height = 6.8f, .scope_opacity = 1.0f,
    .reticle_width = 2.0f, .reticle_height = 2.0f, .use_damage_overlay = 1,
    // General
    .lag_comp = 1.0f,
    // KrunkNative. vsync on by default (the user: "whenever we run the client bin we run it with
    // vsync/performance settings so its not makin my pc chug"): v10's own default is updateRate 0
    // (no cap) under the browser's requestAnimationFrame, which always runs at the display rate, so
    // swap interval 1 is what v10 does natively. updateRate stays 0 like v10's.
    .vsync = 1, .window_mode = RS_WINDOWED,
};

const ClientRenderSettings *client_render_settings(void) {
    return &g_rs;
}

// ---------------------------------------------------------------- parsing

static int parse_number(const char *s, double *out) {
    if (!s || !*s) return 0;
    char *end;
    const double d = strtod(s, &end);
    while (*end && isspace((unsigned char) *end)) end++;
    if (*end || end == s) return 0;
    *out = d;
    return 1;
}

// !!value for the store's string forms ("true"/"false", numbers, strings).
static int parse_bool(const char *s) {
    if (!s || !*s || !strcmp(s, "false")) return 0;
    if (!strcmp(s, "true")) return 1;
    double d;
    return parse_number(s, &d) ? d != 0.0 : 1;
}

// g.isURL (2.js) without the full regex: optional http(s)://, a dotted host with an
// alphabetic TLD (or an IPv4 address), no whitespace; and not an .svg (134.js).
static int is_image_url(const char *s) {
    if (!s || !*s || strstr(s, ".svg")) return 0;
    for (const char *p = s; *p; p++) if (isspace((unsigned char) *p)) return 0;
    const char *host = s;
    if (!strncmp(host, "https://", 8)) host += 8;
    else if (!strncmp(host, "http://", 7)) host += 7;
    const size_t host_len = strcspn(host, "/:?#");
    if (host_len < 3) return 0;
    const char *last_dot = NULL;
    for (size_t i = 0; i < host_len; i++) {
        const char c = host[i];
        if (c == '.') last_dot = host + i;
        else if (!isalnum((unsigned char) c) && c != '-') return 0;
    }
    if (!last_dot) return 0;
    const size_t tld_len = (size_t) (host + host_len - (last_dot + 1));
    int alpha = tld_len >= 2, digits = tld_len >= 1;
    for (size_t i = 0; i < tld_len; i++) {
        if (!isalpha((unsigned char) last_dot[1 + i])) alpha = 0;
        if (!isdigit((unsigned char) last_dot[1 + i])) digits = 0;
    }
    return alpha || digits;
}

// Krunker asset URLs -> the local copy. Krunker's own defaults and presets point at its
// servers (utils assetsUrl(): "https://assets.krunker.io/" + path, plus "?build=..." from
// versionifyUrl; page images under https://krunker.io/img/...), and ui/ mirrors those
// paths (pro_crosshair.png, textures/..., img/...). Such a URL becomes the page-relative
// path when the file exists under ui/ (natively checked on disk; on the web dist/web is a
// copy of ui/, so the path is used as is).
//
// Any other remote URL: the web page loads it like Krunker does; the native engine has no
// HTTP image fetching (image.c rejects scheme URLs), so natively it is dropped and the
// element keeps its default image.
static int krunker_asset_path(const char *url, char *path, const size_t n) {
    static const char *const HOSTS[] = {"assets.krunker.io/", "krunker.io/", "www.krunker.io/"};
    const char *p = url;
    if (!strncmp(p, "https://", 8)) p += 8;
    else if (!strncmp(p, "http://", 7)) p += 7;
    else if (!strncmp(p, "//", 2)) p += 2;
    const char *rest = NULL;
    for (size_t i = 0; i < sizeof(HOSTS) / sizeof(HOSTS[0]) && !rest; i++) {
        const size_t len = strlen(HOSTS[i]);
        if (!strncmp(p, HOSTS[i], len)) rest = p + len;
    }
    if (!rest) return 0;
    while (*rest == '/') rest++;
    const size_t len = strcspn(rest, "?#");
    if (!len || len >= n || strstr(rest, "..")) return 0;
    memcpy(path, rest, len);
    path[len] = 0;
    return 1;
}

#ifndef __EMSCRIPTEN__
static int ui_file_exists(const char *relative) {
    const char *override = getenv("KRUNKNATIVE_UI_DIR"); // same lookup as main.c's html_ui_init
    char full[1024];
    if (override && *override) snprintf(full, sizeof(full), "%s/%s", override, relative);
    else snprintf(full, sizeof(full), "%s%s", ui_default_path(), relative);
    FILE *f = fopen(full, "rb");
    if (f) fclose(f);
    return f != NULL;
}
#endif

int client_render_ui_file(const char *relative, char *full, const size_t n) {
    if (!relative || !relative[0] || strstr(relative, "://")) return 0; // remote (web only): not a GL texture
#ifndef __EMSCRIPTEN__
    const char *override = getenv("KRUNKNATIVE_UI_DIR");
    if (override && *override) snprintf(full, n, "%s/%s", override, relative);
    else snprintf(full, n, "%s%s", ui_default_path(), relative);
#else
    snprintf(full, n, "%s", relative); // dist/web is the ui/ tree: page-relative
#endif
    return 1;
}

// Value of an image setting as the DOM / HUD should use it ("" = default image).
static void localize_image_url(const char *value, char *out, const size_t n) {
    out[0] = 0;
    if (!is_image_url(value)) return; // g.isURL && !.svg (134.js), else ""
    char path[RS_URL_MAX];
    if (krunker_asset_path(value, path, sizeof(path))) {
#ifndef __EMSCRIPTEN__
        if (ui_file_exists(path)) snprintf(out, n, "%s", path);
        else fprintf(stderr, "[render] %s: no local copy under ui/, ignored natively\n", value);
#else
        snprintf(out, n, "%s", path);
#endif
        return;
    }
#ifdef __EMSCRIPTEN__
    snprintf(out, n, "%s", value);
#else
    fprintf(stderr, "[render] %s: remote image, no HTTP image loading natively (ignored)\n", value);
#endif
}

// ---------------------------------------------------------------- side effects

static int g_dom_dirty = 1;       // DOM pieces need (re)applying
static int g_swap_dirty = 1;      // swap interval needs syncing (native)
static int g_forced_window_mode = -1;
static int g_window_mode_applied = RS_WINDOWED;
static float g_resolution_override = 0.0f; // KRUNKNATIVE_RESOLUTION
static int g_frame_cap_override = -1;      // KRUNKNATIVE_FRAME_CAP (0 = uncapped); -1 = the store's
static int g_vsync_override = -1;          // KRUNKNATIVE_VSYNC; -1 = the store's
static int g_fullscreen_kind = RS_FULLSCREEN; // F11 target: the last non-windowed mode chosen
static int g_initialized;         // client_render_settings_init ran (not in --render-scenario runs)
static int g_fps_log;             // KRUNKNATIVE_FPS_LOG
// Headless runs (tests, agents, render scenarios) have no vsync to pace them, so they would
// render uncapped (~700 fps) and load the desktop they share. Unless KRUNKNATIVE_FRAME_CAP is set
// (0 = uncapped), they are capped here; the updateRate setting still wins when it is lower.
#define KN_HEADLESS_FRAME_CAP 120
static int g_headless_cap;

_Static_assert(RS_WINDOWED == 0 && RS_FULLSCREEN == 1 && RS_BORDERLESS == 2, "window_state.c mode values");

#ifndef __EMSCRIPTEN__
// Window state (window_state.h): client_render_window_prepare / _created
static WindowStartup g_ws;        // what this process opened as
static int g_ws_enabled;          // a real window (not headless, not the scenario renderer)
static int g_track_rect;          // pos/size callbacks keep client->windowed_rect current
static Client *g_ws_client;
static int g_rect_pending;        // the window moved / resized: record it once it settles
static double g_rect_pending_at;  // glfwGetTime() of the last such event
#define WINDOW_RECT_SETTLE_S 0.3  // window managers move a window before marking it maximized
static int g_desktop_w, g_desktop_h; // the monitor's mode before exclusive fullscreen changed it
#endif

static void mark_dom(Client *client) { (void) client; g_dom_dirty = 1; }
static void mark_swap(Client *client) { (void) client; g_swap_dirty = 1; }

// Settings > Chat "Show Player Messages" (showMessages), human players only. Off: the human lines already
// in #chatList go (hud_ui_clear_player_chat); bot and system lines stay. The receive gate is hud_ui.c's
// hud_ui_add_player_chat / hud_ui_add_chat_channel, the send gate ui_controls.c's chat_send and
// hud_social.c's quick chat; all read g_rs.show_messages.
static void apply_show_messages(Client *client) {
    (void) client;
    if (!g_rs.show_messages) hud_ui_clear_player_chat();
}

// renderDist.set: camera.far = v * 60 (55.js camera: PerspectiveCamera(.., 0.1, 6000)).
static void apply_render_dist(Client *client) {
    if (client && g_rs.render_dist > 0.0f) client->camera.far = g_rs.render_dist * 60.0f;
}

#ifndef __EMSCRIPTEN__
static int window_is_windowed(GLFWwindow *window) {
    return !glfwGetWindowMonitor(window) && glfwGetWindowAttrib(window, GLFW_DECORATED);
}

// Wayland has no client-side window positions (GLFW reports an error and 0,0).
static int can_position(void) {
    return glfwGetPlatform() != GLFW_PLATFORM_WAYLAND;
}

static void track_windowed_rect(GLFWwindow *window);

static void set_window_mode(Client *client, const int mode) {
    GLFWwindow *window = client ? client->window : NULL;
    if (!window) return;

    // without the tracking callbacks (headless): the rect windowed mode returns to, from the window now
    if (!g_track_rect && window_is_windowed(window) && mode != RS_WINDOWED) {
        glfwGetWindowPos(window, &client->windowed_rect.x, &client->windowed_rect.y);
        glfwGetWindowSize(window, &client->windowed_rect.width, &client->windowed_rect.height);
    } else if (g_rect_pending && mode != RS_WINDOWED) {
        track_windowed_rect(window); // F11 right after a move / resize: take it before leaving windowed
    }
    g_window_mode_applied = mode; // first: the callbacks this switch causes see the new mode

    GLFWmonitor *monitor = glfwGetWindowMonitor(window);
    if (!monitor) monitor = glfwGetPrimaryMonitor();
    const GLFWvidmode *vid = monitor ? glfwGetVideoMode(monitor) : NULL;
    if (vid && !glfwGetWindowMonitor(window)) {
        g_desktop_w = vid->width; // client_render_window_exit: restore it if fullscreen changed it
        g_desktop_h = vid->height;
    }

    if (mode == RS_FULLSCREEN && vid) {
        // exclusive fullscreen at the monitor's current mode, or the Client tab's Resolution
        int fw = vid->width, fh = vid->height;
        client_settings_fullscreen_size(&fw, &fh);
        glfwSetWindowAttrib(window, GLFW_DECORATED, GLFW_TRUE);
        glfwSetWindowMonitor(window, monitor, 0, 0, fw, fh, fw == vid->width && fh == vid->height ? vid->refreshRate : GLFW_DONT_CARE);
    } else if (mode == RS_BORDERLESS && vid) {
        // undecorated window covering the monitor (no video mode change)
        int mx = 0, my = 0;
        glfwGetMonitorPos(monitor, &mx, &my);
        glfwSetWindowMonitor(window, NULL, mx, my, vid->width, vid->height, 0);
        glfwSetWindowAttrib(window, GLFW_DECORATED, GLFW_FALSE);
        glfwSetWindowPos(window, mx, my);
        glfwSetWindowSize(window, vid->width, vid->height);
    } else {
        int w = client->windowed_rect.width, h = client->windowed_rect.height;
        if (w <= 0 || h <= 0) { w = 1280; h = 720; }
        glfwSetWindowMonitor(window, NULL, client->windowed_rect.x, client->windowed_rect.y, w, h, 0);
        glfwSetWindowAttrib(window, GLFW_DECORATED, GLFW_TRUE);
        if (client->windowed_maximized) glfwMaximizeWindow(window); // maximized before fullscreen: again
    }
    g_swap_dirty = 1; // monitor changes can reset the swap interval on some drivers
    client_pointer_refresh(client); // F11 / windowMode: a locked pointer is grabbed again on the new geometry
    printf("[render] window mode %s\n", mode == RS_FULLSCREEN ? "fullscreen" : mode == RS_BORDERLESS ? "borderless" : "windowed");
}
#endif

#ifdef __EMSCRIPTEN__
// The browser owns the window: Fullscreen and Borderless both fill the screen (the page's root
// element), Windowed leaves it. A request the browser refuses (one made without a click, at
// startup) is ignored.
EM_JS(void, web_set_fullscreen, (int on), {
    var root = document.documentElement;
    try {
        if (on && !document.fullscreenElement && root.requestFullscreen) root.requestFullscreen().catch(function () {});
        else if (!on && document.fullscreenElement && document.exitFullscreen) document.exitFullscreen().catch(function () {});
    } catch (e) {}
});
#endif

static void apply_window_mode(Client *client) {
#ifndef __EMSCRIPTEN__
    if (g_rs.window_mode < RS_WINDOWED || g_rs.window_mode > RS_BORDERLESS) g_rs.window_mode = RS_WINDOWED;
    if (g_rs.window_mode != RS_WINDOWED) g_fullscreen_kind = g_rs.window_mode;
    if (g_forced_window_mode > RS_WINDOWED) g_fullscreen_kind = g_forced_window_mode;
    if (!g_initialized) return; // the scenario renderer keeps its fixed hidden window
    const int mode = g_forced_window_mode >= 0 ? g_forced_window_mode : g_rs.window_mode;
    if (client && client->window && mode != g_window_mode_applied) set_window_mode(client, mode);
#else
    (void) client;
    web_set_fullscreen(g_rs.window_mode != RS_WINDOWED); // the Client tab's Window Mode (F11 is the browser's own)
#endif
}

// ---------------------------------------------------------------- key table

typedef enum { RS_T_BOOL, RS_T_INT, RS_T_FLOAT, RS_T_STR, RS_T_URL, RS_T_ASPECT } RsType;

typedef struct {
    const char *key;
    RsType type;
    size_t offset;
    size_t size;
    int shared;                  // ClientSettings (ui_settings.c) handles it too: return 0
    void (*hook)(Client*);
} RsEntry;

#define RS_FIELD(field) offsetof(ClientRenderSettings, field), sizeof(((ClientRenderSettings *) 0)->field)
#define RS_B(key, field, hook) {key, RS_T_BOOL, RS_FIELD(field), 0, hook}
#define RS_I(key, field, hook) {key, RS_T_INT, RS_FIELD(field), 0, hook}
#define RS_F(key, field, hook) {key, RS_T_FLOAT, RS_FIELD(field), 0, hook}
#define RS_S(key, field, hook) {key, RS_T_STR, RS_FIELD(field), 0, hook}
#define RS_U(key, field, hook) {key, RS_T_URL, RS_FIELD(field), 0, hook}
#define RS_SHARED_B(key, field, hook) {key, RS_T_BOOL, RS_FIELD(field), 1, hook}
#define RS_SHARED_F(key, field, hook) {key, RS_T_FLOAT, RS_FIELD(field), 1, hook}

static const RsEntry RS_TABLE[] = {
    // ---- Render tab
    RS_SHARED_F("resolution", resolution, NULL),        // k.setResMlt(v): render_target.c scene FBO scale
    RS_I("updateRate", frame_cap, mark_swap),           // gP(): skip frames until 1000/v ms passed
    {"aspectRatio", RS_T_ASPECT, RS_FIELD(aspect_w), 0, NULL}, // k.aspectRatio = v; resize(): camera aspect
    RS_B("antiAlias", anti_alias, NULL),                // WebGLRenderer({antialias}): MSAA scene FBO
    RS_B("postProcessing", post_processing, NULL),      // composer.enabled (CopyShader: no visible change)
    RS_B("lowSpec", low_spec, NULL),
    RS_B("noTex", no_textures, NULL),
    RS_B("mapDet", map_details, NULL),
    RS_B("particles", particles, NULL),                 // PARTICLES.active
    RS_F("particlesDist", particles_dist, NULL),        // PARTICLES.partDst
    RS_F("renderDist", render_dist, apply_render_dist), // camera.far = v * 60
    RS_SHARED_B("shadows", shadows, NULL),              // toggleShadowMap
    RS_SHARED_B("softShad", soft_shadows, NULL),        // toggleShadQual: PCFSoft vs Basic
    RS_SHARED_B("highResShad", high_res_shadows, NULL), // shadow map 4096
    RS_B("shadowsDynamic", dynamic_shadows, NULL),      // shadowMap.autoUpdate
    RS_SHARED_B("ambientShading", ambient_shading, NULL), // toggleMeshGroup("ambient_0/1")
    RS_B("showTrails", show_trails, NULL),
    RS_B("yourTrails", your_trails, NULL),
    RS_B("muzzleFlash", muzzle_flash, NULL),
    RS_B("bulletTracers", bullet_tracers, NULL),
    RS_S("bulletTracerCol", bullet_tracer_color, NULL),
    RS_B("sniperFlap", sniper_flap, NULL),
    RS_B("textureAnim", texture_anim, NULL),            // main.c freezes client_tick_textures at frame 0
    RS_B("objectAnim", object_anim, NULL),
    RS_B("screenShake", screen_shake, NULL),
    RS_B("weaponShine", weapon_shine, NULL),
    RS_I("lighting", lighting, NULL),
    RS_S("hudHealthHigh", hud_health_high, NULL),       // set(){} in v3.1.1
    RS_S("hudHealthLow", hud_health_low, NULL),         // set(){} in v3.1.1
    RS_F("showSpdLines", speed_lines, NULL),
    RS_S("spdLinesCol", speed_lines_color, NULL),
    RS_B("showPScore", show_popup_score, NULL),
    RS_S("scoreColor", popup_score_color, NULL),
    RS_B("scoreShadow", popup_score_shadow, NULL),
    RS_F("scoreScale", popup_score_scale, NULL),
    RS_F("scoreOffX", popup_score_off_x, NULL),
    RS_F("scoreOffY", popup_score_off_y, NULL),
    RS_F("saturationn", saturation, NULL),              // bundleFilters(0): saturate() on the 3D canvas
    RS_F("hue", hue, NULL),                             // bundleFilters(0): hue-rotate(v * 3.6deg)
    RS_F("menuSaturation", menu_saturation, mark_dom),  // bundleFilters(1): #uiBase filter
    RS_F("menuHue", menu_hue, mark_dom),
    RS_F("vignette", vignette, mark_dom),               // #vignette opacity
    RS_I("feedLimit", feed_limit, NULL),                // refreshFeed(v)
    // ---- Display tab
    RS_B("showUI", show_ui, NULL),
    RS_B("oldScoreboard", old_scoreboard, mark_dom),    // #leaderDisplay / #killFeed
    RS_B("oldBrowser", old_browser, NULL),
    RS_B("disableRarityAnim", disable_rarity_anim, NULL),
    RS_B("showPfp", show_pfp, NULL),
    RS_B("dynamicHP", dynamic_hp, NULL),
    RS_B("showHitInd", show_hit_ind, NULL),
    RS_S("hitColor", hit_color, NULL),
    RS_B("showDMG", show_dmg, NULL),
    RS_I("dmgType", dmg_type, NULL),                    // hud_ui_damage_number: 0 ceil, 1 the exact value
    RS_I("damageNumberDisplayMode", damage_number_mode, NULL), // Strike's Display tab: strike_hud_damage_number (strike_hud.c)
    RS_B("dmgColor", dmg_color, NULL),
    RS_S("critColor", crit_color, NULL),
    RS_SHARED_F("dmgScale", dmg_scale, NULL),
    RS_B("showKills", show_kills, mark_dom),            // #killFeed
    RS_B("showKillC", show_kill_c, mark_dom),           // #killCount
    RS_B("showDeaths", show_deaths, mark_dom),          // #deathCount
    RS_B("showScore", show_score, mark_dom),            // #scoreCount
    RS_B("showStreak", show_streak, mark_dom),          // #streakCount
    RS_B("showKD", show_kd, mark_dom),                  // #kdCount (v7.1.4)
    RS_B("showPing", show_ping, mark_dom),              // #pingDisplay
    RS_B("showNetStat", show_net_stat, mark_dom),       // #ingressDisplay/#egressDisplay
    RS_SHARED_B("showFPS", show_fps, mark_dom),         // #fpsDisplay (ui_settings.c) + #menuFPSDisplay
    RS_F("fpsRate", fps_rate, NULL),                    // the readouts' refresh (hud_ui.c, below; v10.js:221350)
    RS_B("showSpeed", show_speed, NULL),
    RS_F("speedOffX", speed_off_x, NULL),
    RS_F("speedOffY", speed_off_y, NULL),
    RS_SHARED_F("speedScale", speed_scale, NULL),
    RS_F("speedOpacity", speed_opacity, NULL),
    RS_S("speedColor", speed_color, NULL),
    RS_S("speedColorMax", speed_color_max, NULL),
    RS_B("showMedals", show_medals, NULL),
    RS_I("hideNames", hide_names, NULL),
    RS_F("nametagOpac", nametag_opacity, NULL),
    RS_F("nametagScale", nametag_scale, NULL),          // nametags.c: player tags' scale
    RS_I("nametagStyle", nametag_style, NULL),
    RS_B("healthNum", health_num, NULL),
    RS_S("healthColT", health_col_team, NULL),
    RS_S("healthColE", health_col_enemy, NULL),
    RS_B("kpdVision", kpd_vision, NULL),
    RS_S("kpdVisionCol", kpd_vision_col, NULL),
    RS_B("chatFilter", chat_filter, NULL),
    RS_B("showMessages", show_messages, apply_show_messages), // player chat: receive + send gates
    RS_B("showUnboxings", show_unboxings, NULL),
    RS_F("chatOp", chat_opacity, mark_dom),             // #chatList opacity
    RS_F("chatBGOp", chat_bg_opacity, NULL),
    RS_B("chatTextOutline", chat_text_outline, NULL),
    RS_F("chatHeight", chat_height, mark_dom),          // #chatList max-height v * 100 px
    RS_F("chatTimer", chat_timer, NULL),
    RS_I("crosshairSho", crosshair_type, NULL),
    RS_I("crosshairStyle", crosshair_style, NULL),
    RS_U("crosshairImage", crosshair_image, NULL),
    RS_F("crosshairOpacity", crosshair_opacity, NULL),
    RS_B("crosshairUseOpacity", crosshair_use_opacity, NULL),
    RS_B("crosshairAlways", crosshair_always, NULL),
    RS_S("crosshairColor", crosshair_color, NULL),
    RS_S("crosshairShadow", crosshair_shadow, NULL),
    RS_F("crosshairShadowThickess", crosshair_shadow_thickness, NULL),
    RS_F("crosshairThick", crosshair_thick, NULL),
    RS_F("crosshairLen", crosshair_len, NULL),
    RS_F("crosshairGap", crosshair_gap, NULL),
    RS_B("crosshairDot", crosshair_dot, NULL),
    RS_F("crosshairImageW", crosshair_image_w, NULL),   // hud_ui.c type 4: drawImage(.., W || img.width, H || ..)
    RS_F("crosshairImageH", crosshair_image_h, NULL),
    RS_I("crosshairShoM", crosshair_type_m, NULL),      // the throwing crosshair (hud_ui.c)
    RS_I("crosshairStyleM", crosshair_style_m, NULL),
    RS_U("crosshairImageM", crosshair_image_m, NULL),
    RS_F("crosshairImageWM", crosshair_image_w_m, NULL),
    RS_F("crosshairImageHM", crosshair_image_h_m, NULL),
    RS_F("crosshairOpacityM", crosshair_opacity_m, NULL),
    RS_B("crosshairUseOpacityM", crosshair_use_opacity_m, NULL),
    RS_S("crosshairColorM", crosshair_color_m, NULL),
    RS_S("crosshairShadowM", crosshair_shadow_m, NULL),
    RS_F("crosshairShadowThickessM", crosshair_shadow_thickness_m, NULL),
    RS_F("crosshairThickM", crosshair_thick_m, NULL),
    RS_F("crosshairLenM", crosshair_len_m, NULL),
    RS_F("crosshairGapM", crosshair_gap_m, NULL),
    RS_B("hitm", hitmarker, NULL),
    RS_S("hitmCol", hitmarker_color, NULL),
    RS_S("hitmKCol", hitmarker_kill_color, NULL),
    RS_F("hitOpac", hitmarker_opacity, NULL),
    RS_F("hitmLen", hitmarker_length, NULL),
    RS_F("hitmThick", hitmarker_thickness, NULL),
    RS_F("hitmSpac", hitmarker_spacing, NULL),
    RS_F("hitmAnimD", hitmarker_anim_size, NULL),
    RS_F("hitmAnimS", hitmarker_anim_speed, NULL),
    RS_F("hitmFad", hitmarker_fade, NULL),
    RS_U("customHitmarker", custom_hitmarker, NULL),    // hud_ui_hit: #hitmarker img (animateDiv 350, 100, 105)
    // ---- Game tab
    RS_B("streamMode", stream_mode, NULL),
    RS_B("anonMode", anon_mode, NULL),
    RS_F("weaponBob", weapon_bob, NULL),
    RS_F("weaponLean", weapon_lean, NULL),
    RS_F("weaponRot", weapon_rot, NULL),
    RS_F("weaponOffX", weapon_off_x, NULL),
    RS_F("weaponOffY", weapon_off_y, NULL),
    RS_F("weaponOffZ", weapon_off_z, NULL),
    RS_F("weaponADSOffY", weapon_ads_off_y, NULL),
    RS_F("weapSwapY", weap_swap_y, NULL),               // player_mesh.c: the swap dip
    RS_F("weapReloadY", weap_reload_y, NULL),           // player_mesh.c: the reload dip
    RS_B("wallLean", wall_lean, NULL),                  // camera_feel.c: the wall lean roll
    RS_F("wallLeanMlt", wall_lean_mlt, NULL),
    RS_B("leftHand", left_hand, NULL),
    RS_B("aimAnim", aim_anim, NULL),
    RS_B("hideADS", hide_ads, NULL),                    // main.c: no first-person weapon while fully aimed
    RS_B("showHands", show_hands, NULL),
    RS_B("roundArms", round_arms, NULL),
    RS_B("canLoadMods", can_load_mods, NULL),
    RS_B("canChangeLogo", can_change_logo, NULL),
    RS_B("autoLoadLast", auto_load_last, NULL),
    RS_B("scopeBorders", scope_borders, mark_dom),      // .black scope bars
    RS_S("scopeBorderCol", scope_border_color, mark_dom), // .black background-color
    RS_F("scopeBorderOpac", scope_border_opacity, mark_dom), // .black opacity
    RS_F("scopeWidth", scope_width, mark_dom),          // #recticleImg width, #leftBox / #rightBox
    RS_F("scopeHeight", scope_height, mark_dom),        // #recticleImg height, #topBox / #bottomBox
    RS_F("scopeOpac", scope_opacity, mark_dom),         // #recticleImg opacity (hud_ui.c shows #aimRecticle)
    RS_U("customScope", custom_scope, mark_dom),        // #recticleImg
    RS_U("customADSDot", custom_ads_dot, mark_dom),     // updateAimDot(-2, v): #aimDot src
    RS_F("reticleWidth", reticle_width, mark_dom),      // #aimDot width v * 10 px
    RS_F("reticleHeight", reticle_height, mark_dom),    // #aimDot height v * 10 px
    RS_S("endMessage", end_message, NULL),              // game_events.c: chat line once per match end
    RS_U("customAmmo", custom_ammo, mark_dom),          // #ammoIcon src
    RS_U("customKills", custom_kills, mark_dom),        // #killsIcon src
    RS_U("customDeaths", custom_deaths, mark_dom),      // #deathsIcon src
    RS_U("customStreak", custom_streak, mark_dom),      // #streakIcon src
    RS_U("customTimer", custom_timer, mark_dom),        // #timerIcon src
    RS_U("customGameOverlay", custom_game_overlay, mark_dom), // #overlay background-image
    RS_B("useDamageOverlay", use_damage_overlay, NULL),
    RS_U("customDamage", custom_damage, NULL),          // vfx.c: the blood overlay image
    // ---- General tab
    RS_F("lagComp", lag_comp, NULL),
    RS_B("netRerouting", net_rerouting, NULL),
    RS_B("servSendR", serv_send_r, NULL),
    RS_B("shaderRendering", shader_rendering, NULL),    // set(){} in v3.1.1
    // ---- KrunkNative-only
    RS_B("vsync", vsync, mark_swap),
    RS_I("windowMode", window_mode, apply_window_mode),
};

static const RsEntry *find_entry(const char *key) {
    for (size_t i = 0; i < sizeof(RS_TABLE) / sizeof(RS_TABLE[0]); i++) {
        if (!strcmp(RS_TABLE[i].key, key)) return &RS_TABLE[i];
    }
    return NULL;
}

// aspectRatio: /^[-]?\d*x[-]?\d*$/ ("" = the window's own aspect).
static void parse_aspect(const char *value) {
    int w = 0, h = 0;
    if (value && sscanf(value, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
        g_rs.aspect_w = w;
        g_rs.aspect_h = h;
    } else {
        g_rs.aspect_w = g_rs.aspect_h = 0;
    }
}

int client_apply_render_setting(Client *client, const char *key, const char *value) {
    if (!key || !value) return 0;
    const RsEntry *e = find_entry(key);
    if (!e) return 0;

    char *field = (char *) &g_rs + e->offset;
    double d;
    switch (e->type) {
        case RS_T_BOOL: *(int *) field = parse_bool(value); break;
        case RS_T_INT: if (parse_number(value, &d)) *(int *) field = (int) d; break;
        case RS_T_FLOAT: if (parse_number(value, &d)) *(float *) field = (float) d; break;
        case RS_T_STR: snprintf(field, e->size, "%s", value); break;
        case RS_T_URL: localize_image_url(value, field, e->size); break;
        case RS_T_ASPECT: parse_aspect(value); break;
    }
    // The KRUNKNATIVE_* overrides beat the store (a saved updateRate / vsync) for the whole run
    if (!strcmp(key, "updateRate") && g_frame_cap_override >= 0) g_rs.frame_cap = g_frame_cap_override;
    if (!strcmp(key, "vsync") && g_vsync_override >= 0) g_rs.vsync = g_vsync_override;
    if (!strcmp(key, "resolution")) {
        if (g_resolution_override > 0.0f) g_rs.resolution = g_resolution_override;
        if (g_rs.resolution < 0.1f) g_rs.resolution = 0.1f;
        if (g_rs.resolution > 2.0f) g_rs.resolution = 2.0f;
    }
    if (e->hook) e->hook(client);
    if (e->shared) return 0;
    printf("[render] setting %s=%s\n", key, value);
    return 1;
}

// ---------------------------------------------------------------- init / window

void client_render_force_window_mode(const int mode) {
    g_forced_window_mode = mode;
}

void client_render_settings_init(Client *client) {
    const char *res = getenv("KRUNKNATIVE_RESOLUTION"); // e.g. 1 for full-resolution parity captures
    double d;
    if (res && parse_number(res, &d) && d >= 0.1 && d <= 2.0) {
        g_resolution_override = (float) d;
        g_rs.resolution = (float) d;
    }
    const char *cap = getenv("KRUNKNATIVE_FRAME_CAP");
    if (cap && parse_number(cap, &d) && d >= 0.0) g_rs.frame_cap = g_frame_cap_override = (int) d;
    else if (headless_enabled()) g_headless_cap = KN_HEADLESS_FRAME_CAP;
    const char *vsync = getenv("KRUNKNATIVE_VSYNC");
    if (vsync && *vsync) g_rs.vsync = g_vsync_override = parse_bool(vsync);
    const char *fps_log = getenv("KRUNKNATIVE_FPS_LOG"); // "[render] fps N" once a second (stderr)
    g_fps_log = fps_log && *fps_log && strcmp(fps_log, "0");
    g_initialized = 1;
    apply_render_dist(client);
    apply_window_mode(client);
    g_swap_dirty = 1;
    g_dom_dirty = 1;
}

int client_render_window_mode(void) {
#ifdef __EMSCRIPTEN__
    return g_rs.window_mode; // the web has no GLFW window to switch: the store's mode is the one in effect
#else
    return g_window_mode_applied;
#endif
}

void client_render_toggle_fullscreen(Client *client) {
#ifndef __EMSCRIPTEN__
    if (!client || !client->window) return;
    const int next = g_window_mode_applied == RS_WINDOWED ? g_fullscreen_kind : RS_WINDOWED;
    if (g_forced_window_mode >= 0) {
        g_forced_window_mode = next; // --windowed/--fullscreen/--borderless: session only, the store keeps its value
        apply_window_mode(client);
        return;
    }
    // Persisted; the store calls client_apply_render_setting("windowMode") -> apply_window_mode.
    char text[8];
    snprintf(text, sizeof(text), "%d", next);
    ui_settings_set_string("windowMode", text);
    apply_window_mode(client); // no page/store (HTML engine off): apply directly
#else
    // The web: the browser's F11 is its own, so only the page's buttons (Strike's Fullscreen Mode) call this. Its
    // store write runs apply_window_mode, which asks the page for fullscreen (web_set_fullscreen), as Box Office's
    // Window Mode row does.
    char text[8];
    snprintf(text, sizeof(text), "%d", g_rs.window_mode == RS_WINDOWED ? RS_FULLSCREEN : RS_WINDOWED);
    ui_settings_set_string("windowMode", text);
    (void) client;
#endif
}

// ---------------------------------------------------------------- window state (window_state.h)

#ifndef __EMSCRIPTEN__
static void set_env(const char *name, const char *value) {
#ifdef _WIN32
    _putenv_s(name, value ? value : "");
#else
    if (value) setenv(name, value, 1);
    else unsetenv(name);
#endif
}

// The stored "windowMode" (knSettings), read before the page exists: -1 when not stored.
static int stored_window_mode(void) {
    ui_settings_load(); // idempotent: main.c loads it again once the page is up
    const cJSON *v = ui_setting_saved("windowMode");
    if (!v) return -1;
    char text[32];
    ui_setting_to_string(v, text, sizeof(text));
    double d;
    return parse_number(text, &d) ? (int) d : -1;
}

// The rect windowed mode returns to: recorded while windowed and not maximized / minimized
// (fullscreen, borderless and maximized geometry never). Pos/size events only mark it
// pending; it is read once the window has settled for WINDOW_RECT_SETTLE_S, because a
// window manager can move/resize a window before it sets the maximized state.
static void track_windowed_rect(GLFWwindow *window) {
    Client *client = g_ws_client;
    g_rect_pending = 0;
    if (!client || window != client->window || g_window_mode_applied != RS_WINDOWED || !window_is_windowed(window)) return;
    if (!glfwGetWindowAttrib(window, GLFW_VISIBLE) || glfwGetWindowAttrib(window, GLFW_ICONIFIED) ||
        glfwGetWindowAttrib(window, GLFW_MAXIMIZED))
        return;
    int x = client->windowed_rect.x, y = client->windowed_rect.y, w, h;
    if (can_position()) glfwGetWindowPos(window, &x, &y);
    glfwGetWindowSize(window, &w, &h);
    if (w <= 0 || h <= 0) return;
    client->windowed_rect.x = x;
    client->windowed_rect.y = y;
    client->windowed_rect.width = w;
    client->windowed_rect.height = h;
}

static void mark_rect_pending(GLFWwindow *window) {
    Client *client = g_ws_client;
    if (!client || window != client->window || g_window_mode_applied != RS_WINDOWED) return;
    g_rect_pending = 1;
    g_rect_pending_at = glfwGetTime();
}

static void window_pos_cb(GLFWwindow *window, const int x, const int y) {
    (void) x;
    (void) y;
    mark_rect_pending(window);
}

static void window_size_cb(GLFWwindow *window, const int w, const int h) {
    (void) w;
    (void) h;
    mark_rect_pending(window);
}

static void window_maximize_cb(GLFWwindow *window, const int maximized) {
    Client *client = g_ws_client;
    if (!client || window != client->window || g_window_mode_applied != RS_WINDOWED || !window_is_windowed(window)) return;
    client->windowed_maximized = maximized != 0;
    if (maximized) g_rect_pending = 0; // the moves that came with maximizing are not the windowed rect
    else mark_rect_pending(window);    // the window manager's restored rect
}

// Per frame (client_render_frame_end): a settled move / resize.
static void settle_windowed_rect(Client *client) {
    if (g_rect_pending && client && client->window && glfwGetTime() - g_rect_pending_at >= WINDOW_RECT_SETTLE_S)
        track_windowed_rect(client->window);
}

// The state this window would reopen as now.
static void capture_window_state(Client *client, WindowState *s) {
    if (g_rect_pending) track_windowed_rect(client->window); // F4 right after a move: take it now
    memset(s, 0, sizeof(*s));
    s->mode = g_window_mode_applied;
    s->forced = g_forced_window_mode >= 0;
    s->x = client->windowed_rect.x;
    s->y = client->windowed_rect.y;
    s->w = client->windowed_rect.width;
    s->h = client->windowed_rect.height;
    s->maximized = client->windowed_maximized;
    if (s->w < WINDOW_STATE_MIN_SIZE || s->h < WINDOW_STATE_MIN_SIZE) {
        s->w = WINDOW_STATE_DEFAULT_W;
        s->h = WINDOW_STATE_DEFAULT_H;
    }
}

// X11 keeps a video mode exclusive fullscreen changed (Settings > Client > Resolution) after
// the process is gone: GLFW restores it only when the window leaves the monitor, and neither
// exec nor exit does that. Leave the monitor when the mode differs from the desktop's.
static void release_display_mode(Client *client) {
    GLFWmonitor *monitor = glfwGetWindowMonitor(client->window);
    const GLFWvidmode *vid = monitor ? glfwGetVideoMode(monitor) : NULL;
    if (!vid || !g_desktop_w || (vid->width == g_desktop_w && vid->height == g_desktop_h)) return;
    printf("[window] restoring the desktop video mode %dx%d\n", g_desktop_w, g_desktop_h);
    glfwSetWindowMonitor(client->window, NULL, client->windowed_rect.x, client->windowed_rect.y,
                         client->windowed_rect.width > 0 ? client->windowed_rect.width : WINDOW_STATE_DEFAULT_W,
                         client->windowed_rect.height > 0 ? client->windowed_rect.height : WINDOW_STATE_DEFAULT_H, 0);
}

// A kept position must still be on a monitor (one may be gone since): the middle of the
// title bar area, so the window can be grabbed. Else the window manager places it.
static int position_on_a_monitor(const WindowState *s) {
    int count = 0;
    GLFWmonitor **monitors = glfwGetMonitors(&count);
    const int px = s->x + s->w / 2, py = s->y + 16;
    for (int i = 0; i < count; i++) {
        const GLFWvidmode *vid = glfwGetVideoMode(monitors[i]);
        int mx = 0, my = 0;
        glfwGetMonitorPos(monitors[i], &mx, &my);
        if (vid && px >= mx && px < mx + vid->width && py >= my && py < my + vid->height) return 1;
    }
    return 0;
}

int client_window_centered_pos(GLFWwindow *window, const int w, const int h, const int has_near, const int near_x, const int near_y,
                               int *x, int *y) {
    if (!can_position()) return 0; // Wayland: the compositor places windows
    int count = 0;
    GLFWmonitor **monitors = glfwGetMonitors(&count);
    if (!monitors || count <= 0) return 0;
    if (count > 16) count = 16;
    int wa[16][4], primary = 0;
    GLFWmonitor *prim = glfwGetPrimaryMonitor();
    for (int i = 0; i < count; i++) {
        glfwGetMonitorWorkarea(monitors[i], &wa[i][0], &wa[i][1], &wa[i][2], &wa[i][3]);
        if (wa[i][2] <= 0 || wa[i][3] <= 0) { // no work area reported: the whole monitor
            const GLFWvidmode *vid = glfwGetVideoMode(monitors[i]);
            glfwGetMonitorPos(monitors[i], &wa[i][0], &wa[i][1]);
            wa[i][2] = vid ? vid->width : 0;
            wa[i][3] = vid ? vid->height : 0;
        }
        if (monitors[i] == prim) primary = i;
    }
    const int m = window_state_pick_monitor((const int (*)[4]) wa, count, primary, has_near, near_x, near_y);
    if (m < 0 || wa[m][2] <= 0 || wa[m][3] <= 0) return 0;
    int frame[4] = {0, 0, 0, 0};
    if (window && glfwGetWindowAttrib(window, GLFW_DECORATED)) glfwGetWindowFrameSize(window, &frame[0], &frame[1], &frame[2], &frame[3]);
    window_state_center(wa[m][0], wa[m][1], wa[m][2], wa[m][3], w, h, frame, x, y);
    return 1;
}

// KRUNKNATIVE_WINDOW_NEAR=<x>,<y> (app_window.c: the opener's centre): the monitor a page window
// opens on. Read once, not inherited by the windows this one opens.
static int window_near(int *x, int *y) {
    const char *near = getenv("KRUNKNATIVE_WINDOW_NEAR");
    const int ok = near && sscanf(near, "%d,%d", x, y) == 2;
    if (near) set_env("KRUNKNATIVE_WINDOW_NEAR", NULL);
    return ok;
}

static void persist_window_state(const WindowState *s) {
    if (app_window_has_router()) return; // a page window (editor/viewer/...): the main window's geometry is the one kept
    char text[WINDOW_STATE_TEXT_MAX];
    if (window_state_encode(s, text, sizeof(text))) html_storage_set(WINDOW_STATE_STORAGE_KEY, text);
}
#else
int client_window_centered_pos(GLFWwindow *window, const int w, const int h, const int has_near, const int near_x, const int near_y,
                               int *x, int *y) {
    (void) window, (void) w, (void) h, (void) has_near, (void) near_x, (void) near_y, (void) x, (void) y;
    return 0; // the browser places the canvas
}
#endif

int client_render_window_prepare(int *width, int *height, const int enabled) {
#ifndef __EMSCRIPTEN__
    // KRUNKNATIVE_RESTORE_WINDOW is for this process only: never inherited by page windows or a later exec
    WindowState restore;
    const char *env = getenv(WINDOW_STATE_ENV);
    const int has_restore = env && window_state_decode(env, &restore);
    if (env) {
        if (!has_restore) fprintf(stderr, "[window] ignoring malformed %s=%s\n", WINDOW_STATE_ENV, env);
        set_env(WINDOW_STATE_ENV, NULL);
    }
    if (!enabled) return 0;

    WindowState persisted;
    char text[WINDOW_STATE_TEXT_MAX];
    const int has_persisted = html_storage_get(WINDOW_STATE_STORAGE_KEY, text, sizeof(text)) >= 0 &&
                              window_state_decode(text, &persisted);
    int env_w = 0, env_h = 0;
    const char *size = getenv("KRUNKNATIVE_WINDOW_SIZE");
    if (!size || sscanf(size, "%dx%d", &env_w, &env_h) != 2) env_w = env_h = 0;

    const WindowStartupIn in = {
        .restore = has_restore ? &restore : NULL,
        .cli_mode = g_forced_window_mode,
        .stored_mode = stored_window_mode(),
        .env_w = env_w,
        .env_h = env_h,
        .persisted = has_persisted ? &persisted : NULL,
    };
    window_state_resolve(&in, &g_ws);
    g_ws_enabled = 1;
    // the forced (CLI) mode of the previous process carries over as forced, a stored one
    // as the store's value: apply_window_mode then already agrees with the window
    g_forced_window_mode = g_ws.state.forced ? g_ws.state.mode : -1;
    if (!g_ws.state.forced) g_rs.window_mode = g_ws.state.mode;
    *width = g_ws.state.w;
    *height = g_ws.state.h;
    return 1;
#else
    (void) width;
    (void) height;
    (void) enabled;
    return 0;
#endif
}

void client_render_window_created(Client *client) {
#ifndef __EMSCRIPTEN__
    if (!g_ws_enabled || !client || !client->window) return;
    GLFWwindow *window = client->window;
    WindowState *s = &g_ws.state;
    g_ws_client = client;
    if (g_ws.has_pos && (!can_position() || !position_on_a_monitor(s))) g_ws.has_pos = 0;
    // no kept position (a first launch, page windows, a kept one off every monitor): centred on the
    // work area, placed while hidden like a kept one, so the window maps where it stays
    int near_x = 0, near_y = 0, centred = 0;
    const int near = window_near(&near_x, &near_y);
    if (!g_ws.has_pos && client_window_centered_pos(window, s->w, s->h, near, near_x, near_y, &s->x, &s->y)) {
        g_ws.has_pos = 1;
        centred = 1;
    }
    client->windowed_rect.x = s->x;
    client->windowed_rect.y = s->y;
    client->windowed_rect.width = s->w;
    client->windowed_rect.height = s->h;
    client->windowed_maximized = s->maximized;
    g_track_rect = 1; // the rect above is the windowed one: set_window_mode must not read the hidden window's

    WindowStep steps[WINDOW_STEPS_MAX];
    const int count = window_state_steps(&g_ws, steps);
    for (int i = 0; i < count; i++) {
        switch (steps[i]) {
            case WINDOW_STEP_CREATE_HIDDEN: break; // main.c: GLFW_VISIBLE false
            case WINDOW_STEP_POSITION:
                if (can_position()) glfwSetWindowPos(window, s->x, s->y);
                break;
            case WINDOW_STEP_FULLSCREEN: set_window_mode(client, RS_FULLSCREEN); break;
            case WINDOW_STEP_BORDERLESS: set_window_mode(client, RS_BORDERLESS); break;
            case WINDOW_STEP_MAXIMIZE: glfwMaximizeWindow(window); break;
            case WINDOW_STEP_SHOW: glfwShowWindow(window); break;
        }
    }
    if (!g_ws.has_pos && can_position()) // no monitor to centre on: where the window manager put it
        glfwGetWindowPos(window, &client->windowed_rect.x, &client->windowed_rect.y);
    glfwSetWindowPosCallback(window, window_pos_cb);
    glfwSetWindowSizeCallback(window, window_size_cb);
    glfwSetWindowMaximizeCallback(window, window_maximize_cb);
    char where[64] = "";
    if (g_ws.has_pos) snprintf(where, sizeof(where), " at %d,%d (%s)", s->x, s->y, centred ? "centred on the work area" : "kept");
    printf("[window] opened %s%s, windowed %dx%d%s%s%s\n",
           s->mode == RS_FULLSCREEN ? "fullscreen" : s->mode == RS_BORDERLESS ? "borderless" : "windowed",
           s->forced ? " (session)" : "", s->w, s->h, s->maximized ? " maximized" : "", where,
           g_ws.restored ? " (restored after restart)" : "");
#else
    (void) client;
#endif
}

int client_render_window_restored(void) {
#ifndef __EMSCRIPTEN__
    return g_ws_enabled && g_ws.restored;
#else
    return 0;
#endif
}

void client_render_window_before_exec(Client *client) {
#ifndef __EMSCRIPTEN__
    if (!g_ws_enabled || !client || !client->window) return;
    WindowState s;
    capture_window_state(client, &s);
    char text[WINDOW_STATE_TEXT_MAX];
    if (window_state_encode(&s, text, sizeof(text))) {
        set_env(WINDOW_STATE_ENV, text);
        printf("[window] restart with %s=%s\n", WINDOW_STATE_ENV, text);
    }
    persist_window_state(&s);
    release_display_mode(client);
    fflush(stdout);
#else
    (void) client;
#endif
}

void client_render_window_exit(Client *client) {
#ifndef __EMSCRIPTEN__
    if (!g_ws_enabled || !client || !client->window) return;
    WindowState s;
    capture_window_state(client, &s);
    persist_window_state(&s);
    release_display_mode(client);
#else
    (void) client;
#endif
}

// ---------------------------------------------------------------- frame pacing

#define FPS_RING 4096
static double g_frame_ms[FPS_RING]; // timestamps (ms) of the frames in the last second
static int g_frame_head, g_frame_count;
static double g_next_frame;          // native frame cap deadline (s)
static double g_last_due_ms;         // web: Krunker's `bu`
static double g_last_fps_text_ms;    // Krunker's cT (readout refresh every 100 ms)
static double g_last_fps_log_ms;
static int g_swap_interval = -1;

static double now_ms(void) {
#ifdef __EMSCRIPTEN__
    return emscripten_get_now();
#else
    return glfwGetTime() * 1000.0;
#endif
}

int client_render_frame_due(void) {
#ifdef __EMSCRIPTEN__
    // index.js gP(): if (updateRate && now - bu < 1000 / updateRate) skip this frame.
    if (g_rs.frame_cap > 0) {
        const double now = now_ms();
        if (now - g_last_due_ms < 1000.0 / g_rs.frame_cap) return 0;
        g_last_due_ms = now;
    }
#endif
    return 1;
}

#ifndef __EMSCRIPTEN__
static void sleep_seconds(const double seconds) {
    if (seconds <= 0.0) return;
#ifdef _WIN32
    Sleep((DWORD) (seconds * 1000.0));
#else
    struct timespec ts;
    ts.tv_sec = (time_t) seconds;
    ts.tv_nsec = (long) ((seconds - (double) ts.tv_sec) * 1e9);
    nanosleep(&ts, NULL);
#endif
}

// Sleep most of the wait, spin the last bit (Windows Sleep() is ~1-15 ms coarse).
static void wait_until(const double deadline) {
#ifdef _WIN32
    const double slack = 0.002;
#else
    const double slack = 0.0002;
#endif
    double now = glfwGetTime();
    if (deadline - now > slack) sleep_seconds(deadline - now - slack);
    while ((now = glfwGetTime()) < deadline) {}
}
#endif

static void count_frame(void) {
    const double now = now_ms();
    g_frame_ms[g_frame_head] = now;
    g_frame_head = (g_frame_head + 1) % FPS_RING;
    if (g_frame_count < FPS_RING) g_frame_count++;
    // drop the timestamps older than a second (cR.shift() while cR[0] <= now - 1000)
    while (g_frame_count > 0) {
        const int oldest = (g_frame_head - g_frame_count + FPS_RING) % FPS_RING;
        if (g_frame_ms[oldest] > now - 1000.0) break;
        g_frame_count--;
    }
}

int client_render_fps(void) {
    return g_frame_count;
}

// The refresh rate (Hz) of the monitor the window overlaps most; 0 when unknown (headless, no window).
// The fps log prints it: a no-vsync window on a 120 Hz monitor whose driver still syncs swaps
// runs at that rate, which reads as a cap.
static int window_refresh_hz(const Client *client) {
#ifndef __EMSCRIPTEN__
    if (!client || !client->window || headless_enabled()) return 0;
    int wx = 0, wy = 0, ww = 0, wh = 0;
    glfwGetWindowPos(client->window, &wx, &wy);
    glfwGetWindowSize(client->window, &ww, &wh);
    int count = 0;
    GLFWmonitor **monitors = glfwGetMonitors(&count);
    const GLFWvidmode *best = NULL;
    long best_area = 0;
    for (int i = 0; i < count; i++) {
        int mx = 0, my = 0;
        glfwGetMonitorPos(monitors[i], &mx, &my);
        const GLFWvidmode *mode = glfwGetVideoMode(monitors[i]);
        if (!mode) continue;
        const int right = wx + ww < mx + mode->width ? wx + ww : mx + mode->width;
        const int bottom = wy + wh < my + mode->height ? wy + wh : my + mode->height;
        const long ox = right - (wx > mx ? wx : mx), oy = bottom - (wy > my ? wy : my);
        if (ox > 0 && oy > 0 && ox * oy > best_area) best_area = ox * oy, best = mode;
    }
    return best ? best->refreshRate : 0;
#else
    (void) client;
    return 0;
#endif
}

void client_render_frame_end(Client *client) {
    count_frame();
#ifndef __EMSCRIPTEN__
    settle_windowed_rect(client);
#else
    (void) client;
#endif

    const double now = now_ms();
    if (g_fps_log && now - g_last_fps_log_ms >= 1000.0) {
        g_last_fps_log_ms = now;
        const int hz = window_refresh_hz(client);
        char display[32] = "";
        if (hz > 0) snprintf(display, sizeof(display), ", display %d Hz", hz);
        fprintf(stderr, "[render] fps %d (cap %d%s, vsync %d, swap %d%s)\n", g_frame_count, g_rs.frame_cap,
                g_headless_cap > 0 ? " headless" : "", g_rs.vsync, g_swap_interval < 0 ? 0 : g_swap_interval, display);
    }

#ifndef __EMSCRIPTEN__
    // vsync on: swap interval 1; otherwise 0 (uncapped or paced below). emscripten's
    // glfwSwapInterval(0) would leave requestAnimationFrame, so the web never calls it.
    if (g_swap_dirty) {
        // Every sync, not only a change: a window-mode switch can reset the driver's swap interval, which is
        // why set_window_mode marks it dirty (headless: glfwSwapInterval is a no-op).
        g_swap_interval = g_rs.vsync ? 1 : 0;
        headless_swap_interval(g_swap_interval); // glfwSwapInterval (no-op headless: no GLFW context)
        g_swap_dirty = 0;
    }

    int frame_cap = g_rs.frame_cap;
    if (g_headless_cap > 0 && (frame_cap <= 0 || frame_cap > g_headless_cap)) frame_cap = g_headless_cap;
    if (frame_cap > 0) {
        const double period = 1.0 / frame_cap;
        const double t = glfwGetTime();
        if (g_next_frame <= 0.0 || t - g_next_frame > period) g_next_frame = t; // first frame / fell behind: resync
        else wait_until(g_next_frame);
        g_next_frame += period;
    } else {
        g_next_frame = 0.0;
    }
#endif
}

// ---------------------------------------------------------------- DOM pieces

static void style_if(const char *id, const char *prop, const char *value) {
    const HtmlElem el = html_dom_by_id(id);
    if (!el) return;
    char current[RS_URL_MAX + 16];
    const int has = html_dom_get_style(el, prop, current, sizeof(current)) >= 0;
    if (!*value) {
        if (has && current[0]) html_dom_set_style(el, prop, "");
    } else if (!has || strcmp(current, value)) {
        html_dom_set_style(el, prop, value);
    }
}

static void display_if(const char *id, const int on, const char *shown) {
    style_if(id, "display", on ? shown : "none");
}

static void src_if(const char *id, const char *url, const char *fallback) {
    const HtmlElem el = html_dom_by_id(id);
    if (!el) return;
    const char *want = url[0] ? url : fallback;
    char current[RS_URL_MAX];
    if (html_dom_get_attr(el, "src", current, sizeof(current)) < 0 || strcmp(current, want)) html_dom_set_attr(el, "src", want);
}

static void fmt(char *buf, const size_t n, const char *format, const double v) {
    snprintf(buf, n, format, v);
}

// How far each scope bar reaches under the picture, in px (see apply_dom's scopeWidth note).
#define SCOPE_BAR_OVERLAP 2

// The bars' inset for a half size e (floor(v / 2)): made even (stepped down when odd), then
// overlapping the picture by SCOPE_BAR_OVERLAP. A zero-size picture has no edge to hide.
static int scope_bar_offset(const int e) {
    const int even = abs(e) % 2 == 1 && e > 0 ? e - 1 : e;
    return even > 0 ? even - SCOPE_BAR_OVERLAP : even;
}

// bundleFilters(1): "saturate(v*100%) hue-rotate(v*3.6deg)" on #uiBase.
static void apply_menu_filter(void) {
    char filter[96] = "";
    size_t len = 0;
    if (g_rs.menu_saturation != 1.0f) len += (size_t) snprintf(filter + len, sizeof(filter) - len, "saturate(%g%%)", g_rs.menu_saturation * 100.0);
    if (g_rs.menu_hue != 0.0f && len < sizeof(filter)) {
        snprintf(filter + len, sizeof(filter) - len, "%shue-rotate(%gdeg)", len ? " " : "", g_rs.menu_hue * 3.6);
    }
    style_if("uiBase", "filter", filter);
}

static int apply_dom(void) {
    if (!html_dom_by_id("uiBase")) return 0;
    char buf[RS_URL_MAX + 16];

    display_if("killCount", g_rs.show_kill_c, "inline-block");
    display_if("deathCount", g_rs.show_deaths, "inline-block");
    display_if("scoreCount", g_rs.show_score, "inline-block");
    display_if("streakCount", g_rs.show_streak, "inline-block");
    display_if("kdCount", g_rs.show_kd, "inline-block");
    display_if("pingDisplay", g_rs.show_ping, "block");
    display_if("ingressDisplay", g_rs.show_net_stat, "block");
    display_if("egressDisplay", g_rs.show_net_stat, "block");
    display_if("tickDisplay", g_rs.show_net_stat, "block"); // v7.1.4 showNetStat: the TPS readout too
    display_if("menuFPSDisplay", g_rs.show_fps, "block");
    // oldScoreboard.set: leaderDisplay shown, killFeed only without it (and with showKills)
    display_if("leaderDisplay", g_rs.old_scoreboard, "block");
    display_if("killFeed", !g_rs.old_scoreboard && g_rs.show_kills, "inline-grid"); // v7.1.4 showKills: inline-grid

    // vignette.set: 1 removes the inline opacity (stylesheet value), else opacity = v
    fmt(buf, sizeof(buf), "%g", g_rs.vignette);
    style_if("vignette", "opacity", g_rs.vignette == 1.0f ? "" : buf);
    fmt(buf, sizeof(buf), "%g", g_rs.chat_opacity);
    style_if("chatList", "opacity", buf);
    fmt(buf, sizeof(buf), "%gpx", g_rs.chat_height * 100.0);
    style_if("chatList", "max-height", buf);
    fmt(buf, sizeof(buf), "%gpx", g_rs.reticle_width * 10.0);
    style_if("aimDot", "width", buf);
    fmt(buf, sizeof(buf), "%gpx", g_rs.reticle_height * 10.0);
    style_if("aimDot", "height", buf);
    apply_menu_filter();

    // scopeBorders / scopeBorderCol / scopeBorderOpac.set: every .black bar (v10.js:207204..207254)
    HtmlElem bars[8];
    const int n = html_dom_query_all(0, ".black", bars, 8);
    char opacity[24];
    snprintf(opacity, sizeof(opacity), "%g", g_rs.scope_border_opacity);
    for (int i = 0; i < n; i++) {
        char current[RS_COLOR_MAX + 8];
        const char *want = g_rs.scope_borders ? "block" : "none";
        if (html_dom_get_style(bars[i], "display", current, sizeof(current)) < 0 || strcmp(current, want)) html_dom_set_display(bars[i], want);
        if (html_dom_get_style(bars[i], "background-color", current, sizeof(current)) < 0 || strcmp(current, g_rs.scope_border_color))
            html_dom_set_style(bars[i], "background-color", g_rs.scope_border_color);
        if (html_dom_get_style(bars[i], "opacity", current, sizeof(current)) < 0 || strcmp(current, opacity)) html_dom_set_style(bars[i], "opacity", opacity);
    }
    // scopeWidth / scopeHeight.set (v10.js:207255): v *= 100; recticleImg width / height = v px; the bars
    // meet it at calc(50% + e px), e = floor(v / 2) made even (stepped down when odd). scopeOpac
    // (aimRecticle.opacMlt) fades the scope image only: the bars keep scopeBorderOpac.
    // The bars reach SCOPE_BAR_OVERLAP px under the picture (v10 draws its bars 2 px past the image
    // edge, v10.js:207255): the picture's edge is anti-aliased, and where it coincides with the bar's
    // edge the scene shows through as a faint outline round the square.
    const double sw = g_rs.scope_width * 100.0, sh = g_rs.scope_height * 100.0;
    const int ew = (int) floor(sw / 2.0), eh = (int) floor(sh / 2.0);
    const int bw = scope_bar_offset(ew), bh = scope_bar_offset(eh);
    snprintf(buf, sizeof(buf), "%gpx", sw);
    style_if("recticleImg", "width", buf);
    snprintf(buf, sizeof(buf), "%gpx", sh);
    style_if("recticleImg", "height", buf);
    snprintf(buf, sizeof(buf), "calc(50%% + %dpx)", bw);
    style_if("leftBox", "right", buf);
    style_if("rightBox", "left", buf);
    snprintf(buf, sizeof(buf), "calc(50%% + %dpx)", bh);
    style_if("topBox", "top", buf);
    style_if("bottomBox", "bottom", buf);
    snprintf(buf, sizeof(buf), "%g", g_rs.scope_opacity);
    style_if("recticleImg", "opacity", buf);

    src_if("ammoIcon", g_rs.custom_ammo, "textures/ammo_0.png");
    src_if("killsIcon", g_rs.custom_kills, "img/skull_0.png");
    src_if("deathsIcon", g_rs.custom_deaths, "img/skull_1.png");
    src_if("streakIcon", g_rs.custom_streak, "img/skull_2.png");
    src_if("timerIcon", g_rs.custom_timer, "img/timer.png");
    src_if("recticleImg", g_rs.custom_scope, "textures/recticle.png");
    // customADSDot: updateAimDot(-2, v) shows the custom reticle; empty leaves the picked
    // reticle (#aimDot's markup default / the loadout window) alone.
    if (g_rs.custom_ads_dot[0]) src_if("aimDot", g_rs.custom_ads_dot, "textures/reticles/reticle_0.png");
    if (g_rs.custom_game_overlay[0]) snprintf(buf, sizeof(buf), "url('%s')", g_rs.custom_game_overlay);
    else buf[0] = 0;
    style_if("overlay", "background-image", buf);
    return 1;
}

static int g_hud_visible = -1;

void client_render_settings_tick(Client *client) {
    (void) client;
    const double now = now_ms();

    // hud_ui.c reads client_render_settings() for the HUD pieces it owns; the custom
    // images/overlay below are re-applied when settings change or the HUD shows/hides.
    const HtmlElem root = html_dom_root();
    const int hud_visible = root ? html_dom_has_class(root, "kn-hud-visible") : 0;
    if (hud_visible != g_hud_visible) {
        g_hud_visible = hud_visible;
        g_dom_dirty = 1;
    }
    if (g_dom_dirty && apply_dom()) {
        g_dom_dirty = 0;
    }

    // showFPS: the menu readout (v10.js:221350: menuFPS while #menuHolder shows, every fpsRate ms)
    if (g_rs.show_fps && now - g_last_fps_text_ms >= g_rs.fps_rate && html_ui_is_menu_open()) {
        g_last_fps_text_ms = now;
        const HtmlElem fps = html_dom_by_id("menuFPS");
        if (fps) {
            char text[16];
            snprintf(text, sizeof(text), "%d", g_frame_count);
            html_dom_set_text(fps, text);
        }
    }
}

int client_render_hide_weapon_ads(const Client *client) {
    // 95.js: with hideADS the in-hand weapon is hidden once fully aimed (hvJK5(P, 1))
    // and shown again as soon as the aim starts going out.
    const Player *me = client ? client->me : NULL;
    const int weapon = me && me->loadout && me->loadout_index >= 0 && me->loadout_index < me->loadout_size ? me->loadout[me->loadout_index] : -1;
    const int hide_ads = ui_setting_weapon_float("hideADS", weapon, (float) g_rs.hide_ads) != 0.0f; // vmSetts Per Weapon
    return hide_ads && me && me->weapon && me->weapon->zoom && me->aim_val >= 1.0f;
}
