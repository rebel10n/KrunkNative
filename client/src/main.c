#include <gl.h>
#include <GLFW/glfw3.h>
#include <client.h>
#include <cosmetics.h>
#include <skin_fx.h>
#include <pets.h>
#include <build_info.h>
#include <vfx.h>
#include <ai_render.h>
#include <nametags.h>
#include <sprays.h> // WP-E: the v7 spray wheel overlay
#include <hud_social.h> // WP-E: social_reset_server
#include <camera_feel.h>
#include <third_person.h>
#include <input_script.h> // KRUNKNATIVE_INPUT_SCRIPT movement/feel test driver
#include <frame_target.h>  // web: the frame draws offscreen, the canvas gets it whole
#include <html_ui.h>
#include <html_dom.h> // html_storage_*: the version toggle's old knRuleset entry
#include <ruleset.h>  // the startup log: Krunker v10.0.2
#include <flavor.h>   // --flavor / KN_FLAVOR: the product flavor (Krunker Strike branding)
#include <object_manager.h> // object_manager_restore_map (client_unload_map)
#include <om_interp.h>     // om_interp_frame
#include <ui_logic.h> // keybinds, chat and input settings (client/src/ui/ui_controls.c), every build
#include <console.h>   // developer console pointer hooks (client/src/ui/ui_console.c)
#include <debug_capture.h>
#include <install_root.h> // startup chdir of packaged installs
#include <map_post.h>
#include <sky.h>
#include <shadow.h>
#include <nav_overlay.h>
#include <map_environment.h> // v7 environment zones, tone mapping, ocean (map_environment.c)
#include <ocean.h>
#include <zombies_render.h> // map-logic WP-H in-world (zombies_render.c)
#include <hud_gui.h> // map "gui" custom HUD (ui/hud_gui.c)
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <pcg_basic.h>
#include <string.h>
#include <time.h>
#include <stb_image.h>
#include <audio.h>
#include <gltf.h>     // gltf_model_forget (mod_pack.c hooks)
#include <mod_pack.h> // map resource packs (mod_pack.c)
#include <sound_events.h>
#include <voice_chat.h> // voice chat: recorder interval, clips, playback (voice.c)
#include <render_settings.h> // render/display/game settings, frame pacing, scene target (ui_render_settings.c, render_target.c)
#include <menu_view.h> // the world behind the menu: menu/death cameras, end slow motion (menu_view.c)
#include <spectate.h> // Krunker spectating (window.Bf5): cameras, controls, #spectateUI (spectate.c, ui/hud_spectate.c)
#include <hud_map.h>  // WP-E part 2a: minimap + death markers
#include <gamepad.h> // game controllers: v3.1.1 gamepad.js + the controller half of CONTROLS (gamepad.c)
#include <ui_mobile.h> // Krunker's #mobileUI touch controls (ui/ui_mobile.c)
#include <app_mode.h> // game / social hub / editor / viewer pages (app_mode.c)
#include <viewer_embed.h> // the store item view's 3D showcase (viewer/viewer.c)
#include <backend_ui.h> // brain backend: account, matchmaker, --brain / --connect (net/backend_startup.c)
#include <startup_banner.h> // startup log: build, platform, GL (startup_banner.c)
#include <headless.h> // KRUNKNATIVE_HEADLESS / --headless: EGL device context, no window (headless.c)
#include <client_settings.h> // Settings > Client startup options + Exit (ui/ui_client_settings.c)
#include <spin_wheel.h> // the Store's prize wheel canvas + confetti (ui/spin_wheel.c, ui/store_ui.c)
#include <ks_overlay.h> // KrunkScript GAME.OVERLAY canvas (ks/ks_overlay.c, ks/canvas2d.c)
#include <lobby_ui.h> // WP-E: lobby packets, match vote cards (net/lobby_events.c)
#include <krunker_net.h> // KN_KRUNKER_OFFICIAL: --krunker / ?krunker= joins an official krunker.io game (net/krunker_net.c)
#include <game_config.h> // WP-E: GAME_CONFIG on INIT, the round's networkData
#include <map_catalog.h> // startup map lookup (--map, --sandbox, ?map=, ?sandbox)
#include <game_reload.h> // F5: the full reload (game_reload.c)
#include <ks_client.h> // KrunkScript client host: the map's client script (ks/ks_client_host.c)
#include <ks_game.h> // ks_game_free

#ifdef WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/html5.h>
#endif

// `--render-scenario` (tools/uidiff, client/src/ui/ui_scenario.c): native engine builds only.
#if defined(KRUNKNATIVE_HTML_ENGINE) && !defined(__EMSCRIPTEN__)
#include <ui_logic.h>
#ifndef __EMSCRIPTEN__
#include <asset_fetch.h>
#endif
#define SCENARIO_ACTIVE() ui_scenario_active()
#define SCENARIO_PREVIEW() ui_scenario_draws_preview()
#else
#define SCENARIO_ACTIVE() 0
#define SCENARIO_PREVIEW() 0
#endif

#define DEFAULT_SERVER_ADDRESS "127.0.0.1"

geometry_cache_map g_geometry_cache;
texture_cache_map g_texture_cache;
glyph_cache_map g_glyph_cache;

FT_Library g_freetype;
FT_Face g_game_font;

Geometry *g_cube_geometry;
Geometry *g_plane_geometry;
Geometry *g_ramp_geometry;

unsigned int g_blank_texture;
unsigned int g_active_texture;
unsigned int g_active_shader;
RenderLighting g_render_lighting = {
    .ambient_color = {0.592f, 0.627f, 0.659f, 1.0f},
    .light_color = {0.949f, 0.973f, 0.988f, 1.0f},
    .sky_color = {0.863f, 0.910f, 0.929f, 1.0f},
    .fog_color = {0.553f, 0.604f, 0.627f, 1.0f},
    .light_direction = {0.0f, 0.809f, -0.588f},
    .ambient_intensity = 1.0f,
    .light_intensity = 1.3f,
    .fog_near = 1.0f,
    .fog_far = 2000.0f,
    .enabled = 1,
    .fog_enabled = 1,
};

void client_load_map(Client*);
void client_unload_map(Client*);
void client_tick(Client*, float, float);
void client_tick_net(Client*);
void resize_viewport(GLFWwindow*, int, int);
static void client_sync_window_size(Client*);
void scroll_callback(GLFWwindow*, double, double);
static void cursor_pos_callback(GLFWwindow*, double, double);
static void mouse_button_callback(GLFWwindow*, int, int, int);
static void key_callback(GLFWwindow*, int, int, int, int);
static void window_focus_callback(GLFWwindow*, int);
static void char_callback(GLFWwindow*, unsigned int);

static vec4 json_color_or(const cJSON *json, const vec4 fallback) {
    if (cJSON_IsNumber(json)) return hex_to_vec((int) cJSON_GetNumberValue(json));

    if (cJSON_IsString(json)) {
        vec4 parsed = fallback;
        if (!parse_hex_color(cJSON_GetStringValue(json), &parsed)) return parsed;
    }

    return fallback;
}

static float json_float_or(const cJSON *json, const float fallback) {
    return cJSON_IsNumber(json) ? (float) cJSON_GetNumberValue(json) : fallback;
}

static int json_truthy(const cJSON *json) {
    if (!json || cJSON_IsNull(json) || cJSON_IsFalse(json)) return 0;
    if (cJSON_IsNumber(json)) return cJSON_GetNumberValue(json) != 0;
    if (cJSON_IsString(json)) return cJSON_GetStringValue(json)[0] != 0;
    return 1;
}

// Krunker v3.1.1 render.js initScene(map, mode); the mode's ambCol / lightCol / fogC /
// fogD / skyCol win over the map's (Stalker), and skyCol also drops the sky dome:
//   if (map.ambient) AmbientLight(map.ambient, map.ambientI ?? config.ambIntensity (1))
//   if (map.light)   DirectionalLight(map.light, map.lightI ?? config.lightIntensity (1.3)),
//                    position = lightD (500) * (cos v, sin v sin u, sin v cos u),
//                    u = PI * (sunAngY ?? 54) / -180, v = PI * (sunAngX ?? 90) / -180
//   scene.fog = new THREE.Fog(map.fog, 1, map.fogD)  (linear smoothstep on view-space depth)
//   renderer.setClearColor(map.sky)
// A missing light/ambient means no such light at all (intensity 0), not a default.
static void client_apply_map_lighting(const Map *map, const GameMode *mode) {
    if (!map || !map->raw_data) return;

    const cJSON *raw = map->raw_data;
    const vec4 black = {0.0f, 0.0f, 0.0f, 1.0f};
    const vec4 white = {1.0f, 1.0f, 1.0f, 1.0f};

    const cJSON *ambient = cJSON_GetObjectItem(raw, "ambient");
    const cJSON *light = cJSON_GetObjectItem(raw, "light");
    const cJSON *fog_far = cJSON_GetObjectItem(raw, "fogD");

    g_render_lighting.ambient_color = json_truthy(ambient) ? json_color_or(ambient, black) : black;
    g_render_lighting.ambient_intensity = json_truthy(ambient) ? json_float_or(cJSON_GetObjectItem(raw, "ambientI"), 1.0f) : 0.0f;
    g_render_lighting.light_color = json_truthy(light) ? json_color_or(light, black) : black;
    g_render_lighting.light_intensity = json_truthy(light) ? json_float_or(cJSON_GetObjectItem(raw, "lightI"), 1.3f) : 0.0f;

    // setClearColor(undefined) keeps the previous colour.
    g_render_lighting.sky_color = json_color_or(cJSON_GetObjectItem(raw, "sky"), g_render_lighting.sky_color);
    // new THREE.Color(undefined) is white.
    g_render_lighting.fog_color = json_color_or(cJSON_GetObjectItem(raw, "fog"), white);
    g_render_lighting.fog_near = 1.0f;
    g_render_lighting.fog_far = json_float_or(fog_far, 0.0f);
    // settings "lighting" (restart): 0 = Low swaps map materials for MeshBasicMaterial (unlit,
    // still fogged); 1 = Normal (Lambert, default); 2 = High (Phong + point lights) is not
    // ported and renders as Normal.
    g_render_lighting.enabled = client_render_settings()->lighting != 0;
    // Without fogD three.js would upload an undefined fogFar; treat it as no fog.
    g_render_lighting.fog_enabled = cJSON_IsNumber(fog_far) && g_render_lighting.fog_far > g_render_lighting.fog_near;

    const float sun_y = json_float_or(cJSON_GetObjectItem(raw, "sunAngY"), 54.0f);
    const float sun_x = json_float_or(cJSON_GetObjectItem(raw, "sunAngX"), 90.0f);
    const float u = (float) M_PI * (sun_y / -180.0f);
    const float v = (float) M_PI * (sun_x / -180.0f);

    // Direction towards the light (position - target at the origin), normalised.
    g_render_lighting.light_direction.x = cosf(v);
    g_render_lighting.light_direction.y = sinf(v) * sinf(u);
    g_render_lighting.light_direction.z = sinf(v) * cosf(u);

    const float len = sqrtf(
        g_render_lighting.light_direction.x * g_render_lighting.light_direction.x +
        g_render_lighting.light_direction.y * g_render_lighting.light_direction.y +
        g_render_lighting.light_direction.z * g_render_lighting.light_direction.z
    );

    if (len > 0.0f) {
        g_render_lighting.light_direction.x /= len;
        g_render_lighting.light_direction.y /= len;
        g_render_lighting.light_direction.z /= len;
    }

    // mode overrides (q.ambCol != null ? q.ambCol : p.ambient, ...)
    const GameModeConfig *mode_config = mode ? &mode->config : NULL;
    if (mode_config && mode_config->has_amb_col) {
        g_render_lighting.ambient_color = hex_to_vec(mode_config->amb_col);
        if (!json_truthy(ambient)) g_render_lighting.ambient_intensity = json_float_or(cJSON_GetObjectItem(raw, "ambientI"), 1.0f);
    }
    if (mode_config && mode_config->has_light_col) {
        g_render_lighting.light_color = hex_to_vec(mode_config->light_col);
        if (!json_truthy(light)) g_render_lighting.light_intensity = json_float_or(cJSON_GetObjectItem(raw, "lightI"), 1.3f);
    }
    if (mode_config && mode_config->has_fog_col) g_render_lighting.fog_color = hex_to_vec(mode_config->fog_col);
    if (mode_config && mode_config->has_fog_dist) {
        g_render_lighting.fog_far = mode_config->fog_dist;
        g_render_lighting.fog_enabled = g_render_lighting.fog_far > g_render_lighting.fog_near;
    }
    const int mode_sky = mode_config && mode_config->has_sky_col;
    if (mode_sky) g_render_lighting.sky_color = hex_to_vec(mode_config->sky_col);

    glClearColor(g_render_lighting.sky_color.x, g_render_lighting.sky_color.y, g_render_lighting.sky_color.z, 1.0f);
    sky_load(mode_sky ? NULL : raw); // renderer.init: no sky dome when the mode sets skyCol
    map_environment_map_loaded(map, mode); // v7 toneMapping / outputEncoding / correctLights, ocean, zone -1 (map_environment.c)
}

// CONTROLS.skipScroll (index.js b9): an input frame that carried a wheel step swallows the
// first wheel event before the next input frame (which clears it again).
static int g_skip_scroll;

// The unrounded look the input frames are built from (client_tick, CONTROLS xDr/yDr).
static vec2 g_look;
static vec2 g_look_sent;      // WP-C: g_look as the last sampled input frame took it (me->direction after it)
static int g_look_unsampled;  // WP-C: this frame sampled no input frame (netRate), the camera shows g_look
static int g_look_valid;
static vec2 g_look_shown;     // the look an unsampled frame put on me->direction (player_look_follows_camera)
static int g_look_shown_valid;

// Our spawn: the look restarts from the spawn's (me->direction from SPAWN) instead of the last life's,
// whatever the resync test in the input stage would make of it (v7.1.4 / v10 "start" ->
// CONTROLS.reset(true): xDr = yDr = 0, pchObjc / object rotation 0, then io_addPlayers sets them
// from the spawned player's xDire / yDire).
static void client_look_reset(void) {
    g_look_valid = 0;
    g_look_shown_valid = 0;
    g_look_unsampled = 0;
}

static void print_usage_to(FILE *out, const char *program) {
    fprintf(out, "Usage: %s [--offline|--sandbox] [--map <name>] [--headless] [--windowed|--fullscreen|--borderless]\n"
                 "          [--hub|--viewer [query]] [--brain <url>] [--connect <host[:port]>] [map.json]\n"
                 "  --offline       play on the in-process sandbox server (no network); a random rotation map\n"
                 "                  unless --map / map.json (console `offline`, web ?offline=1)\n"
                 "  --sandbox       --offline on the sandbox map, v7's blank Test map (console `sandbox`, web ?sandbox)\n"
                 "  --map <name>    start on a catalogue map: key, name or path under assets/maps (web ?map=)\n"
                 "  --krunker [id]  join an official krunker.io game (that game id, else any) over Krunker's own\n"
                 "                  protocol (web ?krunker=<id|1>; client/include/krunker_net.h)\n"
                 "       %s --editor [query] [editor flags] [map.json ...]   (map editor, one project per map)\n"
                 "       %s --render-scenario <scenario.json> <out.png>   (tools/uidiff; HTML_ENGINE=ON builds)\n"
                 "       %s --help | --version\n", program, program, program, program);
    editor_usage(out); // client/src/editor/editor_bus_link.c
}

static void print_usage(const char *program) {
    print_usage_to(stderr, program);
}

#ifdef __EMSCRIPTEN__

// Query parameters replace command line arguments on the web:
//   ?offline=1     run the in-process local server (same as --offline): a random rotation
//                  map unless ?map
//   ?sandbox       offline on v7.1.4's sandbox map (--sandbox; ?test too, as in v7's index.js)
//   ?map=<name>    start on that map (--map: local_game_map_lookup, else assets/maps/<name>.json)
// While offline the address bar keeps ?offline=1&map=<key> / ?sandbox up to date
// (servers_ui.c), so a reload (F5) lands on the same map.
//   social.html, editor.html, viewer.html boot in that page's app mode (app_mode.c)
//   ?ws=<url>      WebSocket server URL (see net_web.c)
static int web_query_flag(const char *name) {
    return EM_ASM_INT({
        var value = null;
        try { value = new URLSearchParams(location.search).get(UTF8ToString($0)); } catch (e) {}
        return value !== null && value !== '0' && value !== 'false' ? 1 : 0;
    }, name);
}

// Returned string is malloc'd and owned by the caller; NULL when absent.
static char *web_query_string(const char *name) {
    return (char *) EM_ASM_PTR({
        var value = null;
        try { value = new URLSearchParams(location.search).get(UTF8ToString($0)); } catch (e) {}
        return value ? stringToNewUTF8(value) : 0;
    }, name);
}

// The GL framebuffer follows the CSS size of the #canvas element as the stylesheet lays
// it out (ui/css/krunknative.css: fixed, 100% x 100%). The HiDPI GLFW canvas pins inline
// style.width/height to the last window size, so those are dropped while measuring;
// otherwise the canvas would keep its first size forever (glfwSetWindowSize sets them
// again right after).
static void web_canvas_size(int *width, int *height) {
    double css_width = 0.0;
    double css_height = 0.0;

    EM_ASM({
        var c = document.getElementById('canvas');
        if (!c) return;
        var w = c.style.width;
        var h = c.style.height;
        c.style.width = "";
        c.style.height = "";
        var r = c.getBoundingClientRect();
        c.style.width = w;
        c.style.height = h;
        HEAPF64[$0 >> 3] = r.width;
        HEAPF64[$1 >> 3] = r.height;
    }, &css_width, &css_height);

    *width = css_width >= 1.0 ? (int) css_width : 1280;
    *height = css_height >= 1.0 ? (int) css_height : 720;
}

// Browser resizes are only noted here and applied by the next frame (web_sync_canvas_size): the
// callback is registered right after the window exists, and startup yields to the browser at every
// asset fetch (ASYNCIFY), so a resize while the page, the settings or the map load must not be lost
// (it was, with the callback registered after startup) nor resize the canvas in the middle of a
// startup step. 1 at start: the first frame measures the canvas.
static int g_web_resize_pending = 1;

static bool web_on_resize(const int event_type, const EmscriptenUiEvent *event, void *user_data) {
    (void) event_type;
    (void) event;
    (void) user_data;
    g_web_resize_pending = 1;
    return true;
}

static void web_sync_canvas_size(Client *client) {
    if (!g_web_resize_pending) return;
    g_web_resize_pending = 0;

    int width, height;
    web_canvas_size(&width, &height);
    glfwSetWindowSize(client->window, width, height); // emscripten GLFW resizes the canvas and fires resize_viewport()
}

static void web_main_loop(void *user_data) {
    client_frame(user_data);
}

// Mouse look on the web, as v3.1.1 CONTROLS reads it: the canvas mousemove handler sums
// movementX/Y while the canvas holds the pointer lock (a4/a5 in a8) and updateMouseInp
// consumes the sums once per frame. Emscripten's GLFW cursor is no substitute: under the
// lock it is the running sum of movementX, but the first move after the lock goes
// (Escape, alt-tab) snaps it back to the absolute page position, and the game applied
// that jump as one huge look delta before the menu came up. Returns the movement since
// the last call (0 while unlocked).
EM_JS(void, web_take_look, (double *dx, double *dy), {
    var look = Module.knLook;
    if (!look) {
        look = Module.knLook = {x: 0, y: 0};
        var canvas = document.getElementById('canvas');
        if (canvas) canvas.addEventListener('mousemove', function (e) {
            if (document.pointerLockElement !== canvas) return;
            look.x += e.movementX || 0;
            look.y += e.movementY || 0;
        });
    }
    HEAPF64[dx >> 3] = look.x;
    HEAPF64[dy >> 3] = look.y;
    look.x = look.y = 0;
});

#endif

static int file_exists(const char *path) {
    FILE *file = fopen(path, "rb");
    if (file) fclose(file);
    return file != NULL;
}

// --krunker [gameId] / ?krunker=<gameId|1>: start in an official krunker.io game (krunker_net.h)
static int g_krunker_start;
static const char *g_krunker_game;

static int parse_startup_args(int argc, char **argv, int *offline, int *sandbox, const char **map_name, const char **map_path, const char **scenario, const char **scenario_out) {
    *offline = 0;
    *sandbox = 0;
    *map_name = NULL;
    *map_path = NULL;
    *scenario = NULL;
    *scenario_out = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            print_usage_to(stdout, argv[0]);
            exit(0);
        }
        if (!strcmp(argv[i], "--offline")) {
            *offline = 1;
        } else if (!strcmp(argv[i], "--sandbox")) {
            *offline = *sandbox = 1;
        } else if (!strcmp(argv[i], "--map") && i + 1 < argc) {
            *map_name = argv[++i];
        } else if (!strcmp(argv[i], "--krunker")) {
            g_krunker_start = 1;
            if (i + 1 < argc && argv[i + 1][0] != '-' && !strstr(argv[i + 1], ".json")) g_krunker_game = argv[++i];
        } else if (!strcmp(argv[i], "--windowed") || !strcmp(argv[i], "--fullscreen") || !strcmp(argv[i], "--borderless")) {
            // window mode for this session (render_settings.h); overrides the stored windowMode
            client_render_force_window_mode(argv[i][2] == 'w' ? RS_WINDOWED : argv[i][2] == 'f' ? RS_FULLSCREEN : RS_BORDERLESS);
        } else if (!strcmp(argv[i], "--render-scenario") && i + 2 < argc) {
            *scenario = argv[++i];
            *scenario_out = argv[++i];
        } else if (argv[i][0] == '-') {
            print_usage(argv[0]);
            return 0;
        } else if (!*map_path) {
            *map_path = argv[i];
        } else {
            print_usage(argv[0]);
            return 0;
        }
    }

    return 1;
}

// The product flavor (flavor.h): --flavor base|strike wins, then KN_FLAVOR, then the compile-time
// default (base, or Strike for a -DKRUNKNATIVE_FLAVOR=strike build). The flavor brands the client and,
// for Strike, swaps in FRVR's rules (ruleset_current(), strike_rules.h) before any game is configured.
// Sets the flavor from KN_FLAVOR / --flavor and REMOVES --flavor[=key] (and a separate key
// argument) from argv so the ordinary argument parser below never sees it and treats it as unknown.
static void client_resolve_flavor(int *argc, char **argv) {
    const char *env = getenv("KN_FLAVOR");
    if (env && env[0]) flavor_set_key(env);
    int out = 1;
    for (int i = 1; i < *argc; i++) {
        if (!strncmp(argv[i], "--flavor=", 9)) {
            flavor_set_key(argv[i] + 9);
        } else if (!strcmp(argv[i], "--flavor")) {
            if (i + 1 < *argc) flavor_set_key(argv[++i]); // consume the key too
        } else {
            argv[out++] = argv[i];
        }
    }
    if (out < *argc) argv[out] = NULL;
    *argc = out;
}

int main(int argc, char **argv) {
    client_resolve_flavor(&argc, argv); // branding for --version, the banner and the window title; strips --flavor
    // --version before anything needs assets or a GL context (build identity: build_info.h)
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--version")) {
            // "KrunkNative client" stays in the line: launchers check it before installing a
            // staged update (launcher.c staged_client_runs), the installed ones included
            printf("%s (KrunkNative client) %s\n", flavor_display_name(flavor_get()), kn_build_string());
            return 0;
        }
        if (!strcmp(argv[i], "--voice-info")) { // packaging checks: the voice codec is linked in
            printf("opus=%s\n", voice_opus_version());
            return 0;
        }
    }
    printf("%s (KrunkNative client) %s\n", flavor_display_name(flavor_get()), kn_build_string());
    install_root_chdir(); // packaged installs started from elsewhere: ui/ + assets/ (install_root.h)
    debug_capture_startup(); // KRUNKNATIVE_CAMERA/SCREENSHOT runs ignore stored settings (debug_capture.c)
    game_reload_startup();   // F5: files changed from now on are new; the session an F5 restart hands over

    char *rand_memory = malloc(1);
    pcg32_srandom(time(NULL), *(unsigned int *) &rand_memory);
    free(rand_memory);

    vt_init(&g_geometry_cache);
    vt_init(&g_texture_cache);
    // map resource packs: what is already loaded follows a pack coming or going (mod_pack.c)
    const ModPackHooks mod_hooks = {texture_cache_reload, geometry_cache_reload, gltf_model_forget, audio_reload};
    mod_pack_set_hooks(&mod_hooks);
    vt_init(&g_glyph_cache);
#ifndef __EMSCRIPTEN__
    asset_fetch_client_init(); // missing game content is downloaded in the background (asset_fetch.h)
#endif

    char *game_font_path = concat(assets_path(), "css/fonts/font2.ttf");
    ensure_asset_file(game_font_path);

    if (FT_Init_FreeType(&g_freetype) || FT_New_Face(g_freetype, game_font_path, 0, &g_game_font)) {
        fprintf(stderr, "Failed to load font %s\n", game_font_path);
        return -1;
    }

    free(game_font_path);

    // --headless / KRUNKNATIVE_HEADLESS=1: GLFW's null platform + an EGL pbuffer context, no
    // window on any display (client/include/headless.h). Headless sessions keep the window
    // size unless --fullscreen/--borderless asks otherwise (parse_startup_args below).
    if (headless_parse_args(&argc, argv)) client_render_force_window_mode(RS_WINDOWED);
    if (!headless_glfw_init()) return -1;

    cJSON *startup_map = NULL;
    int offline = 0;
    int sandbox = 0;
    const char *startup_map_name = NULL;
    const char *startup_map_path = NULL;
    const char *scenario_path = NULL;
    const char *scenario_out = NULL;

    backend_parse_args(&argc, argv);  // --brain <url> / --connect <host[:port]> (net/backend_startup.c)
    app_mode_parse_args(&argc, argv); // --hub/--editor/--viewer [query] (app_mode.c)
    if (!parse_startup_args(argc, argv, &offline, &sandbox, &startup_map_name, &startup_map_path, &scenario_path, &scenario_out)) {
        return -1;
    }

    if (scenario_path) {
#ifdef UI_SCENARIO_SUPPORTED
        // Offline local server on the scenario's map; the window gets the scenario size below.
        if (!ui_scenario_begin(scenario_path, scenario_out)) return 2;
        offline = 1;
        if (!startup_map_path) startup_map_path = ui_scenario_map_path();
#else
        fprintf(stderr, "--render-scenario needs the in-house HTML engine (make HTML_ENGINE=ON)\n");
        return 2;
#endif
    }

#ifdef __EMSCRIPTEN__
    // the page's query instead of the command line (web_query_flag above)
    if (web_query_flag("offline")) offline = 1;
    if (web_query_flag("sandbox") || web_query_flag("test")) offline = sandbox = 1;
    char *web_map_name = web_query_string("map");
    if (web_map_name && web_map_name[0]) startup_map_name = web_map_name;
    char *web_krunker = web_query_string("krunker");
    if (web_krunker) {
        g_krunker_start = 1;
        if (web_krunker[0] && strcmp(web_krunker, "1") != 0) g_krunker_game = web_krunker;
    }
#endif
    if (offline || scenario_path) g_krunker_start = 0; // an offline / scenario run stays local

    // --sandbox / ?sandbox without a map: v7.1.4 loadSandbox(config.blankMap)
    if (sandbox && !startup_map_name && !startup_map_path) startup_map_name = LOCAL_GAME_SANDBOX_MAP;
    // a map.json argument that is no file may name a catalogue map ("client_bin --offline kanji")
    if (startup_map_path && !startup_map_name && !file_exists(startup_map_path) && local_game_map_lookup(startup_map_path) >= 0) {
        startup_map_name = startup_map_path;
        startup_map_path = NULL;
    }
    char *named_map_path = NULL;
    if (startup_map_name) {
        // the catalogue's own (cached, sanitised) JSON, so the game knows which map it plays
        // (the address bar's ?map=, servers_ui.c); otherwise a file under assets/maps
        const int index = local_game_map_lookup(startup_map_name);
        if (index >= 0) {
            startup_map = (cJSON *) map_catalog_json((size_t) index);
            if (!startup_map) {
                fprintf(stderr, "Failed to load map %s (%s)\n", startup_map_name, map_catalog_at((size_t) index)->path);
                return -1;
            }
            printf("startup map %s: %s\n", startup_map_name, map_catalog_at((size_t) index)->path);
            startup_map_path = NULL;
        } else {
            const size_t name_length = strlen(startup_map_name);
            const int has_extension = name_length > 5 && !strcmp(startup_map_name + name_length - 5, ".json");
            char *relative = concat("maps/", startup_map_name);
            char *with_extension = relative && !has_extension ? concat(relative, ".json") : relative;
            if (with_extension) named_map_path = concat(assets_path(), with_extension);
            if (with_extension != relative) free(with_extension);
            free(relative);
            startup_map_path = named_map_path;
        }
    }

    // map_catalog_load_file: a renamed map's old path (assets/maps/v7/kanji.json) loads its new file
    if (startup_map_path && map_catalog_load_file(startup_map_path, &startup_map)) {
        fprintf(stderr, "Failed to load map JSON: %s\n", startup_map_path);
        return -1;
    }

    free(named_map_path);
#ifdef __EMSCRIPTEN__
    free(web_map_name);
#endif

    static Client INSTANCE = {};
    INSTANCE.startup_map = startup_map;

    pthread_mutex_init(&INSTANCE.net_lock, NULL);
    INSTANCE.net_socket = -1;
    INSTANCE.net_ping_ms = -1;             // Krunker pingText "-" until the first pong
    client_game_events_reset(&INSTANCE);   // no round clock / end screen / death yet
    strncpy(INSTANCE.net_address, DEFAULT_SERVER_ADDRESS, sizeof(INSTANCE.net_address) - 1);
    INSTANCE.net_port = NET_DEFAULT_PORT;
    {
        // KRUNKNATIVE_SERVER_PORT: connect to another native server port (tests, several servers on one host)
        const char *port = getenv("KRUNKNATIVE_SERVER_PORT");
        const long value = port ? strtol(port, NULL, 10) : 0;
        if (value > 0 && value < 65536) INSTANCE.net_port = (unsigned short) value;
    }

    INSTANCE.camera.zoom = 1.0f;
    INSTANCE.camera.fov = M_PI / 2.0f;
    INSTANCE.camera.near = 0.1f;
    INSTANCE.camera.far = 10000.0f;

    int window_width = 1280;
    int window_height = 720;
    {
        // KRUNKNATIVE_WINDOW_SIZE=<w>x<h>: initial window (headless: pbuffer) size
        const char *size = getenv("KRUNKNATIVE_WINDOW_SIZE");
        int w = 0, h = 0;
        if (size && sscanf(size, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
            window_width = w;
            window_height = h;
        }
    }

#ifdef __EMSCRIPTEN__
    // WebGL2 is exposed as OpenGL ES 3.0; the canvas starts at its CSS size.
    glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    // The canvas sits under the HTML UI, never over it, so it needs no alpha
    // channel: an opaque WebGL context avoids browser premultiplied compositing
    // (translucent draws would otherwise show the page background through the
    // canvas) and is cheaper to composite.
    glfwWindowHint(GLFW_ALPHA_BITS, 0);
    // v3.1.1 draws at the device's pixel density: renderer.setPixelRatio(devicePixelRatio
    // * resolution) with the canvas at window size (55.js setResMlt). A HiDPI-aware GLFW
    // canvas makes the framebuffer CSS px x devicePixelRatio (window/cursor sizes stay CSS
    // px); render_target.c then scales the scene by `resolution` on top of it.
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);
    web_canvas_size(&window_width, &window_height);
#elif defined(KRUNKNATIVE_GL_ANGLE)
    // OpenGL ES 3.0 on ANGLE (gl.h KN_GLES; the macOS default, rendering through Metal):
    // GLFW's EGL backend on the ANGLE display headless_glfw_init picked (GLFW_ANGLE_PLATFORM_TYPE).
    glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
    glfwWindowHint(GLFW_CONTEXT_CREATION_API, GLFW_EGL_CONTEXT_API);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#elif defined(__APPLE__)
    // KRUNKNATIVE_GL_BACKEND=desktop on macOS (fallback to Apple's deprecated OpenGL):
    // macOS only offers forward-compatible core profiles up to 4.1; shader.c
    // rewrites the shaders' `#version 450 core` line to `410 core` there.
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#else
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
#endif
#ifdef UI_SCENARIO_SUPPORTED
    if (SCENARIO_ACTIVE()) {
        // exactly the scenario size; hidden unless KRUNKNATIVE_SCENARIO_VISIBLE=1 (debugging)
        const char *visible = getenv("KRUNKNATIVE_SCENARIO_VISIBLE");
        ui_scenario_window_size(&window_width, &window_height);
        glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
        glfwWindowHint(GLFW_VISIBLE, visible && !strcmp(visible, "1") ? GLFW_TRUE : GLFW_FALSE);
    }
#endif

#ifndef __EMSCRIPTEN__
    // The window as it was before F4 / a Settings restart (KRUNKNATIVE_RESTORE_WINDOW), else the
    // stored mode + kept rect: created hidden at the windowed rect, then placed, switched and
    // shown before the first frame (client_render_window_created), so it never flashes windowed.
    if (client_render_window_prepare(&window_width, &window_height, !SCENARIO_ACTIVE() && !headless_enabled()))
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
#endif
    headless_window_hints(); // headless: no GLFW context (headless_make_context_current creates it)
    INSTANCE.window = glfwCreateWindow(window_width, window_height, flavor_display_name(flavor_get()), NULL, NULL);
    if (!INSTANCE.window) {
        const char *reason = NULL;
#ifndef __EMSCRIPTEN__ // emscripten's GLFW port has no glfwGetError
        glfwGetError(&reason);
#endif
        fprintf(stderr, "glfwCreateWindow failed: %s\n", reason ? reason : "unknown error");
        return -1;
    }

#ifndef __EMSCRIPTEN__ // browsers have no window icon (the page's favicon / PWA icons)
    // assets/img/icon.png: krunker.io's 256 px app icon (tools/branding/fetch_branding.py).
    // macOS windows have no icon (the .app's .icns is the Dock's).
    GLFWimage icon = {0};
    char *icon_path = concat(assets_path(), "img/icon.png");

    int icon_channels;
    icon.pixels = stbi_load(icon_path, &icon.width, &icon.height, &icon_channels, 4);

    if (icon.pixels) {
#ifndef __APPLE__
        glfwSetWindowIcon(INSTANCE.window, 1, &icon);
#endif
        stbi_image_free(icon.pixels);
    }
    printf("[window] title \"%s\", icon %s (%dx%d)\n", glfwGetWindowTitle(INSTANCE.window), icon.pixels ? icon_path : "none",
           icon.width, icon.height);

    free(icon_path);
#endif

    if (!headless_make_context_current(INSTANCE.window)) return -1;
    glfwSetWindowUserPointer(INSTANCE.window, &INSTANCE);
#ifndef __EMSCRIPTEN__
    client_render_window_created(&INSTANCE); // mode / position / maximized, then shown
#endif
    glfwSetFramebufferSizeCallback(INSTANCE.window, resize_viewport);
    glfwSetScrollCallback(INSTANCE.window, scroll_callback);
    glfwSetCursorPosCallback(INSTANCE.window, cursor_pos_callback);
    glfwSetMouseButtonCallback(INSTANCE.window, mouse_button_callback);
    glfwSetKeyCallback(INSTANCE.window, key_callback);
    glfwSetCharCallback(INSTANCE.window, char_callback);
#ifdef __EMSCRIPTEN__
    emscripten_set_resize_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, NULL, 0, web_on_resize); // before startup's fetches yield
#else
    glfwSetWindowFocusCallback(INSTANCE.window, window_focus_callback); // alt-tab = pointer lock lost
#endif
    if (!SCENARIO_ACTIVE()) gamepad_init(&INSTANCE); // controller hotplug (GLFW joysticks / browser gamepads)
    ks_client_init(&INSTANCE); // KrunkScript client host (scripts load per INIT: ks_client_round)

    if (glfwRawMouseMotionSupported()) {
        glfwSetInputMode(INSTANCE.window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
    }

#ifndef __EMSCRIPTEN__
    // On the web GL functions are provided by the WebGL binding, no loader needed.
    if (!kn_gl_load(headless_get_proc_address)) return -1;
    fprintf(stderr, "[gl] GL_RENDERER %s | GL_VERSION %s\n", (const char *) glGetString(GL_RENDERER), (const char *) glGetString(GL_VERSION));

    // emscripten's glfwSwapInterval(0) would switch the main loop from
    // requestAnimationFrame to setTimeout(0); keep vsync-paced frames there.
    // Natively the vsync/updateRate settings re-sync it (client_render_frame_end).
    headless_swap_interval(0);
#endif
    startup_banner(); // build, platform, GL backend + renderer (web: v7's console badges)
    // v3.1.1 render defaults (camera far from renderDist, window mode, KRUNKNATIVE_RESOLUTION
    // / _FRAME_CAP / _VSYNC overrides) before the page pushes the stored settings.
    if (!SCENARIO_ACTIVE()) client_render_settings_init(&INSTANCE);

    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    INSTANCE.scene = scene_init();
    INSTANCE.fps_scene = scene_init();
    g_pet_world_scene = INSTANCE.scene; // the local player's pet flies in the world (pets.h)
    INSTANCE.ui = ui_init();

    if (!INSTANCE.scene || !INSTANCE.fps_scene || !INSTANCE.ui) return -1;

    // Sound (client/src/audio.c); not for the scenario renderer. Failure leaves the game silent.
    // Settings > Client startup options (audio backend env, priority, raw input, resolution,
    // Start Offline, resource swapper, Discord RPC) before the audio device and the page.
    if (!SCENARIO_ACTIVE()) client_settings_startup(&INSTANCE, &offline);

    if (!SCENARIO_ACTIVE()) audio_init();

    // Krunker v10.0.2 is the only version (shared/include/ruleset.h): the version toggle's saved
    // choice (storage "knRuleset") means nothing any more and is removed
    if (html_storage_get("knRuleset", NULL, 0) >= 0) html_storage_set("knRuleset", NULL);
    printf("[client] Krunker v%s\n", ruleset_current()->version);

    // HTML/CSS menu (doc/html-ui-engine.md). ui/ sits next to assets/; natively the
    // in-house engine (client/src/html/engine) renders it over the scene, on the web
    // the browser renders the same page and the C UI logic drives it through web_dom.c.
    {
        const char *ui_dir_override = getenv("KRUNKNATIVE_UI_DIR"); // alternate page tree (tests/dev)
        char *ui_dir = ui_dir_override && *ui_dir_override ? concat(ui_dir_override, "/") : concat(ui_default_path(), "");
        int fb_width, fb_height;
        glfwGetFramebufferSize(INSTANCE.window, &fb_width, &fb_height);

        client_menu_bind(&INSTANCE);

        if (html_ui_available() && !html_ui_init(&INSTANCE, ui_dir, fb_width, fb_height)) {
            fprintf(stderr, "HTML UI unavailable, falling back to the built-in menu\n");
        }

        free(ui_dir);

        if (html_ui_available()) client_set_menu_open(&INSTANCE, 1);
        if (!SCENARIO_ACTIVE()) app_mode_start(&INSTANCE); // startup page: --hub/--editor/--viewer, web: the page URL

        // Apply the stored settings now, before the first map is built (noTex, mapDet,
        // ambientShading, lighting, resolution, ... affect map/mesh construction); the
        // menu pushes them again once its DOM is set up, which is idempotent.
        if (!SCENARIO_ACTIVE()) {
            ui_settings_load();
            ui_settings_push_saved();
        }
    }

    size_t startup_map_count = 0;
    const cJSON **startup_maps = map_list_with_custom(startup_map, &startup_map_count);

    if (startup_map && !startup_maps) {
        fprintf(stderr, "Failed to allocate startup map list\n");
        return -1;
    }

    game_configure(&INSTANCE.game, NULL, startup_maps, startup_map_count, NULL, 0, NULL, 0, NULL, 0);

    NetMainArgs net_args = {
        .client = &INSTANCE,
        .address = INSTANCE.net_address, // DEFAULT_SERVER_ADDRESS unless --connect
    };

    // brain backend (backend_ui.h): with a brain URL the matchmaker picks the game
    // server (servers_ui.c starts the transport); offline / --connect / no brain: as before
    // an official krunker.io start skips the brain's matchmaking like an offline one
    const int brain_mode = backend_startup(&INSTANCE, offline || g_krunker_start, SCENARIO_ACTIVE());

    if (g_krunker_start) {
        (void) net_args;
        krunker_net_join(&INSTANCE, g_krunker_game, NULL); // matchmaker, then Krunker's WebSocket (krunker_net.c)
    } else if (offline) {
        if (!local_game_start(&INSTANCE)) return -1; // the in-process sandbox server, joined like any server (net/offline.c)
    } else {
#ifdef __EMSCRIPTEN__
        (void) net_args;
        // Not fatal: the failure is reported to the menu (connection error screen,
        // which can retry or host an offline game) and net_web.c keeps retrying.
        // editor.html / viewer.html need no game server (app_mode.c)
        if (app_mode_needs_server() && !brain_mode && !net_web_connect(&INSTANCE)) fprintf(stderr, "Failed to open websocket connection\n");
#else
        if (brain_mode) {
            // the matchmaker starts the transport once seek-game answers (client_net_switch_server)
        } else if (pthread_create(&INSTANCE.net_thread, NULL, net_main, &net_args)) {
            fprintf(stderr, "Failed to start network thread\n");
            return -1;
        } else {
            INSTANCE.net_thread_running = 1;
        }
#endif
    }

    INSTANCE.last_frame_time = glfwGetTime();

#ifdef UI_SCENARIO_SUPPORTED
    if (SCENARIO_ACTIVE()) {
        const int status = ui_scenario_run(&INSTANCE);
        INSTANCE.net_should_quit = 1;
        local_game_stop(&INSTANCE);
        client_class_preview_fini(&INSTANCE);
        viewer_embed_fini();
        spin_wheel_fini();
        ui_scenario_end();
        html_ui_fini();
        headless_shutdown();
        glfwTerminate();
        return status;
    }
#endif

#ifdef __EMSCRIPTEN__
    // The browser drives the frame loop (requestAnimationFrame). main() returns
    // and the emscripten runtime stays alive because EXIT_RUNTIME is off; the
    // page going away is the only shutdown.
    emscripten_set_main_loop_arg(web_main_loop, &INSTANCE, 0, 0);
    return 0;
#else
    while (!glfwWindowShouldClose(INSTANCE.window)) {
        client_frame(&INSTANCE);
    }

    client_render_window_exit(&INSTANCE); // keep the windowed rect + maximized flag for the next start
    INSTANCE.net_should_quit = 1;

    if (INSTANCE.net_thread_running) {
        pthread_join(INSTANCE.net_thread, NULL);
        INSTANCE.net_thread_running = 0;
    }

    local_game_stop(&INSTANCE);
    asset_fetch_client_shutdown(); // stops the content downloads before the caches go
    mod_pack_shutdown(); // the extracted resource pack files
    ks_client_unload(&INSTANCE); // KrunkScript: script objects / DIVs / sounds
    ks_game_free(&INSTANCE.game);
    client_class_preview_fini(&INSTANCE);
    viewer_embed_fini();
    spin_wheel_fini();
    vfx_fini();
    ai_render_reset(&INSTANCE); // AI meshes in the world scene (ai_render.c)
    nav_overlay_fini(); // show_navmeshes: waits for a running build, GL buffers, the line program
    om_build_fini();    // the Object Manager overlay's GL buffers (om_gizmo.c)
    shadow_fini(); // sun shadow FBO/texture/program (shadow.c)
    sky_fini();    // sky dome VAO/buffers/program (sky.c)
    render_target_fini();
    frame_target_fini();
    html_ui_fini();
    voice_fini();
    sprays_wheel_fini(); // its canvas (GL)
    hud_objectives_fini(); // the objective markers canvas (GL)
    build_ui_fini();     // the charge bar / build wheel canvas (GL)
    audio_fini();
    headless_shutdown();
    client_settings_exit(); // Discord RPC close; relaunch when a "*" setting asked for a restart

    return 0;
#endif
}

// One main-loop iteration, shared by the native while loop and the emscripten
// main loop callback: advance time, tick/render (offline included: the sandbox server
// steps inside the frame, net/offline.c), present and poll input.
void client_frame(Client *client) {
    // updateRate on the web: Krunker skips whole animation frames (index.js gP); the
    // canvas keeps the last frame. Natively the cap sleeps in client_render_frame_end.
    if (!client_render_frame_due()) {
        gamepad_frame(client, 0); // v3.1.1's gamepad loop is its own requestAnimationFrame: it runs on skipped frames
        return;
    }

    const double frame_time = glfwGetTime();
    const float now = (float) frame_time;
    const float delta = input_script_delta((float) (frame_time - client->last_frame_time)); // KRUNKNATIVE_FIXED_DELTA

    client->last_frame_time = frame_time;

#ifdef __EMSCRIPTEN__
    web_sync_canvas_size(client); // a browser resize since the last frame (startup included)
#endif
    client_sync_window_size(client); // before anything draws: viewport, HTML layout, pbuffer
#ifndef __EMSCRIPTEN__
    asset_fetch_client_frame(); // fetched game content pops in (placeholders filled in place)
#endif
    gamepad_frame_begin(); // updateMouseInp's gamepad.active reset leads the game frame (gamepad.c)
    // Web: asset fetches inside the frame yield to the browser, which then composites the
    // canvas; the frame draws offscreen and reaches the canvas whole (frame_target.h).
    frame_target_begin(client);
    client_tick(client, now, delta);
    gamepad_frame(client, 1); // the gamepad loop after the game frame (its rAF is registered after the game's)
    frame_target_present(client);
    headless_swap_buffers(client->window); // glfwSwapBuffers; headless: the pbuffer flushes (at most 2 frames in flight)
    glfwPollEvents();
    client_render_frame_end(client); // swap interval (vsync), frame cap sleep, FPS count
}

void client_load_map(Client *client) {
    if (!client->game.map) return;

    client_apply_map_lighting(client->game.map, client->game.mode);

    for (size_t i = 0; i < client->game.map->object_count; i++) {
        const Object *object = client->game.map->objects[i];
        if (object->mesh) scene_add_mesh(client->scene, object->mesh);
    }

    shadow_load_map(client->game.map); // caster/receiver flags + light camera (shadow.c)
    map_post_load(client->scene, client->game.map); // AO planes, noise planes, point lights (map_post.c)
    zombies_render_load(client->scene, client->game.map); // WP-H machine weapons, beams, airdrops (zombies_render.c)
}

void client_unload_map(Client *client) {
    if (!client->game.map) return;
    object_manager_restore_map(&client->game); // the Object Manager's map edits: the map's own objects (and meshes) back first

    shadow_unload_map();
    map_post_unload(client->scene);
    zombies_render_unload();
    map_environment_unload(); // ocean, v7 output state (map_environment.c)
    hud_gui_clear();          // map "gui" elements (ui/hud_gui.c)

    for (size_t i = 0; i < client->game.map->object_count; i++) {
        const Object *object = client->game.map->objects[i];
        if (object->mesh) scene_remove_mesh(client->scene, object->mesh);
    }
}

void client_clear_players(Client *client) {
    for (size_t i = 0; i < client->game.player_count; i++) {
        Player *player = client->game.players[i];
        if (!player) continue;

        if (player->mesh) {
            scene_remove_player_mesh(player->render_you ? client->fps_scene : client->scene, player->mesh, player->loadout_size);
            player_meshes_fini(player);
        }

        free(player->loadout);
        free(player->ammo);
        free(player->reloads);
        free(player->input_queue);
        free(player);
    }

    free(client->game.players);
    client->game.players = NULL;
    client->game.player_count = 0;
    client->me = NULL;
    client->in_game = 0;
    client->spawn_requested = 0;
    client_recon_reset(client); // net_next_input_seq 0, no pending inputs
    client->last_swap_key = 0;
    spectate_end(client); // new round / lost session: the server re-sends SPECTATE to spectators
}

Player *client_find_player(Client *client, const int uid) {
    for (size_t i = 0; i < client->game.player_count; i++) {
        Player *player = client->game.players[i];
        if (player && player->uid == uid) return player;
    }

    return NULL;
}

// ENT ent[22..25]: SETTINGS.tmp.verifiedBadge / premiumBadge / partnerBadge / customBadge (default on)
static uint8_t client_show_badges(void) {
    static const char *const keys[4] = {"verifiedBadge", "premiumBadge", "partnerBadge", "customBadge"};
    uint8_t show = 0;
    for (int i = 0; i < 4; i++) {
        if (!ui_setting_is_false(keys[i])) show |= (uint8_t) (1u << i);
    }
    return show;
}

void client_add_player_mesh(Client *client, Player *player, const int render_you) {
    if (player->mesh) return;

    player_generate_meshes(player, render_you);
    player_swap_weapon(player, player->loadout_index, 1, 1, 1);
    player_update_meshes(player, 0);

    if (render_you) {
        scene_add_player_mesh(client->fps_scene, player->mesh, player->loadout_size);
    } else {
        scene_add_player_mesh(client->scene, player->mesh, player->loadout_size);
    }
}

void client_drop_player_mesh(Client *client, Player *player) {
    if (!player->mesh) return;

    scene_remove_player_mesh(player->render_you ? client->fps_scene : client->scene, player->mesh, player->loadout_size);
    player_meshes_fini(player);
}

// SKINS (sent right before every SPAWN): a player's validated weapon / melee skins, kept by uid
// for its SPAWN (servers without SKINS: none). A full table recycles slot uid % CLIENT_SKINS_MAX.
#define CLIENT_SKINS_MAX 256
static NetSkinsPacket g_client_skins[CLIENT_SKINS_MAX];
static int g_client_skins_count;

static PlayerSkins client_skins_of(const int uid) {
    for (int i = 0; i < g_client_skins_count; i++) {
        if (g_client_skins[i].uid == uid) return g_client_skins[i].skins;
    }
    return PLAYER_SKINS_NONE;
}

static void client_apply_skins(const NetSkinsPacket *packet) {
    int slot = 0;
    while (slot < g_client_skins_count && g_client_skins[slot].uid != packet->uid) slot++;
    if (slot == g_client_skins_count) {
        if (g_client_skins_count < CLIENT_SKINS_MAX) g_client_skins_count++;
        else slot = (int) ((unsigned) packet->uid % CLIENT_SKINS_MAX);
    }
    g_client_skins[slot] = *packet;
}

// SPAWN: a player (re)spawned, or (active = 0) a player the server knows who is
// dead right now: kept without meshes so the scoreboard and kill feed have its name.
void client_apply_spawn(Client *client, const NetSpawnPacket *packet) {
    Player *player = client_find_player(client, packet->uid);

    if (!packet->active) {
        if (!player) {
            player = player_init(&client->game);
            if (!player) return;

            player->uid = packet->uid;
            player->class_index = packet->class_index;
            game_players_add(&client->game, player);
        }

        snprintf(player->name, sizeof(player->name), "%.*s", (int) sizeof(packet->name) - 1, packet->name);
        player->active = 0;
        client_drop_player_mesh(client, player);
        return;
    }

    const PlayerSkins skins = client_skins_of(packet->uid);
    if (!player) {
        player = player_init(&client->game);
        if (!player) return;

        player->uid = packet->uid;
        player->class_index = packet->class_index;
        player->secondary_index = packet->secondary_index;
        player->cosmetics = packet->cosmetics;
        player->cosmetics_v7 = packet->cosmetics_v7;
        player->skins = skins;
        game_players_add(&client->game, player);
        player_spawn(player);
    } else if (!player->active || !player->loadout_size || player->class_index != packet->class_index || player->secondary_index != packet->secondary_index ||
               memcmp(&player->cosmetics, &packet->cosmetics, sizeof(packet->cosmetics)) != 0 ||
               memcmp(&player->cosmetics_v7, &packet->cosmetics_v7, sizeof(packet->cosmetics_v7)) != 0 ||
               memcmp(&player->skins, &skins, sizeof(skins)) != 0 || player->attach_index != packet->attach_index ||
               player->head_mlt != packet->head_mlt) {
        client_drop_player_mesh(client, player); // respawn: fresh loadout, timers and meshes
        player->class_index = packet->class_index;
        player->secondary_index = packet->secondary_index;
        player->cosmetics = packet->cosmetics;
        player->cosmetics_v7 = packet->cosmetics_v7; // WP-F: SPAWN V4 (shorter: none)
        player->skins = skins;
        player_spawn(player);
    } else {
        // players.add runs player.init on every spawn: a player alive already with the same look keeps
        // its loadout and meshes, but its look feel (recoilAnim(Y), landBobY, leanAnim*, aimVal,
        // crouchVal) starts from rest like any spawn's.
        player_reset_feel(player);
        player->aim_val = 0.0f;
        player->crouch_val = 0.0f;
        player_update_height(player);
    }

    snprintf(player->name, sizeof(player->name), "%.*s", (int) sizeof(packet->name) - 1, packet->name);
    player->active = packet->active;
    player->team = packet->team;
    player->attach_index = packet->attach_index; // WP-C: SPAWN V3 attachIndex / headMlt (V2: 0 / 1)
    player->head_mlt = packet->head_mlt;
    player->health = packet->health;
    player->max_health = packet->max_health;
    player->position.x = packet->position[0];
    player->position.y = packet->position[1];
    player->position.z = packet->position[2];
    player->direction.x = packet->direction[0];
    player->direction.y = packet->direction[1];

    player->interpolate = 0; // players.js forcePos(): the first state after a spawn snaps
    player->force_pos = 1;
    client_player_spawned(client, player, packet->is_you);
}

// players.js forcePos() (index.js resetGameSessionState on "start": our spawn, spectating):
// the server sends no player positions while we are dead or in the menu (net.c
// net_broadcast_states_tick), so every remote player snaps to its next STATE instead of
// gliding from where it was when we died.
void client_force_player_positions(Client *client) {
    for (size_t i = 0; i < client->game.player_count; i++) {
        Player *player = client->game.players[i];
        if (player && player != client->me) player->force_pos = 1;
    }
}

// A player just (re)spawned in client->game (SPAWN above, or a bot of the client's own game):
// its meshes, and for us the in-game state.
void client_player_spawned(Client *client, Player *player, const int is_you) {
    if (is_you) {
        client->me = player;
        player->is_you = 1;
        client_recon_reset(client); // "start" -> CONTROLS.reset(): tmpInpts emptied, isn = 0
        // CONTROLS.reset(!detachedCam) + io_addPlayers (v7.1.4 index.js:17198 / 14680-14683, v10
        // v10.js:218714 / 220059): the camera looks along the spawn's yaw (xDire), pitch yDire = 0
        // (player.init), never the last life's look; the death chase target (CONTROLS.target) and
        // the third-person camera's distance start over too. Camera shake is not reset (rotateCam
        // re-applies RENDER.shakeX/Y every frame).
        client_look_reset();
        menu_view_reset();
        third_person_on_spawn();
        client_force_player_positions(client);
        // Our prediction copy has to slide like the server's player (v3.1.1 reads it back from
        // the spawn data, index.js serverSetting.slidCont); the offline sandbox server too.
        player->slid_cont = ui_controls_settings()->slid_cont ? 1 : 0;
        client_add_player_mesh(client, player, 1);

        // Our (re)spawn: in the game again, death card down (UI.toggleGameUI(true)).
        client->in_game = 1;
        client->spawn_requested = 0;
        client->net_death_time = 0.0;
        client->net_death_menu_pending = 0;
        hud_ui_hide_death(client);
        hud_popups_on_spawn(); // "start": medal queue cleared, overlay.reset()
        spectate_end(client); // playing again ("start" with spectating = false)
        // startGame(): UI.toggleMenu(false) -- e.g. the next round respawning everyone who
        // played while INIT had brought the menu back.
        if (html_ui_available() && html_ui_is_menu_open()) client_set_menu_open(client, 0);
    } else {
        client_add_player_mesh(client, player, 0);
    }
    ks_client_player_spawned(client, player); // KrunkScript onPlayerSpawn (index.js:14700)
}

// LEAVE (Krunker "2"): the player disconnected; forget it entirely.
static void client_remove_player(Client *client, const int uid) {
    for (size_t i = 0; i < client->game.player_count; i++) {
        Player *player = client->game.players[i];
        if (!player || player->uid != uid) continue;

        if (player == client->me) return; // never drop ourselves; INIT resets the session

        client_drop_player_mesh(client, player);
        free(player->loadout);
        free(player->ammo);
        free(player->reloads);
        free(player->input_queue);
        free(player);

        memmove(&client->game.players[i], &client->game.players[i + 1], (client->game.player_count - i - 1) * sizeof(Player *));
        client->game.player_count--;
        return;
    }
}

void client_apply_state(Client *client, const NetStatePacket *packet) {
    Player *player = client_find_player(client, packet->uid);
    if (!player) return;

    // Our own player is predicted; the server corrects it with RECON ("l", net/recon.c) only.
    if (player == client->me) return;

    // index.js updatePlayerPositions ("k"): with forcePos the state snaps, otherwise
    // x1/y1/z1 and xDir1/yDir1 are where the model is now and the packet is the target that
    // players.js update() reaches in (1000 / sendRateA) * interpolation ms.
    const vec3 position = player->position;
    const vec2 direction = player->direction;
    net_packet_apply_player(player, packet);
    player->send_rate = (float) game_network_data(&client->game)->send_hz; // WP-E: the round's networkData (sendRateA)

    if (player->force_pos) {
        player->interpolate = 0;
        player->force_pos = 0;
        return;
    }

    player->dt = 0.0f;
    player->interp_pos_start = position;
    player->interp_pos_end = player->position;
    player->interp_dir_start = direction;
    player->interp_dir_end = player->direction;
    player->position = position;
    player->direction = direction;
    player->interpolate = 1;
}

void client_send_cycle_packet(Client *client) {
    client_net_send(client, NET_PACKET_CYCLE, NULL, 0); // io_cycle (offline: the sandbox server's)
}

// Krunker Strike: a direct native server advertises its product flavor in INIT (shared.h); a client
// of a different flavor refuses the server with a clear message instead of silently mixing, the
// mirror of the server's HELLO refusal. Returns 1 (and disconnects) when refused. The official
// krunker.io bridge and offline play are never refused: their INIT carries our own flavor by
// construction (krunker_link emits base; offline is this same process).
static int client_refuse_wrong_flavor(Client *client, const NetInitPacket *init) {
    if (client->offline || krunker_net_active()) return 0;
    const GameFlavor server_flavor = (GameFlavor) NET_WIRE_FLAVOR(init->ruleset);
    if (server_flavor == flavor_get()) return 0;
    char msg[sizeof(((NetErrorPacket *) 0)->message)];
    snprintf(msg, sizeof(msg), "This server runs %s, not %s", flavor_display_name(server_flavor),
             flavor_display_name(flavor_get()));
    printf("[client] refusing server: flavor mismatch (server %u, client %s)\n",
           (unsigned int) server_flavor, flavor_key(flavor_get()));
    fflush(stdout);
    client_net_disconnect(client, msg, NET_ERROR_CODE_FLAVOR);
    return 1;
}

// INIT: a new session or round on map init->map_index (already resolved to the
// client's own map list by client_map_prepare_init).
void client_apply_init(Client *client, const NetInitPacket *init) {
    client->net_ent_size = net_ent_size_for(init->major, init->minor, init->patch);
    client->net_server_minor = init->major == NET_PROTOCOL_MAJOR ? init->minor : 0; // WP-B packet gate
    client->net_client_id = init->client_id;
    client->net_server_patch = init->major == NET_PROTOCOL_MAJOR ? init->patch : 0; // WP-C (ENT V8)
    social_reset_server(); // WP-E part 2b: v7 social packets after the server's NET_PACKET_SOCIAL_V7
    object_manager_ui_reset(); // KrunkNative Object Manager: this server's caps are asked again
    if (client->map_loaded) client_unload_map(client);
    client_clear_players(client);
    client_game_events_reset(client); // next round: end screen down, fresh clock

    game_config_on_init(&client->game); // WP-E: no GAME_CONFIG before this INIT (older server): the defaults
    lobby_vote_reset();                 // WP-E: the end screen's vote cards go
    // INIT carries the Krunker mode id (modes.js index), not a rotation index
    game_init(&client->game, init->map_index, game_mode_index_for(&client->game, init->mode_index), 0);
    mod_pack_map_loaded(client->game.map ? client->game.map->raw_data : NULL); // the map's modURL resource pack
    client_load_map(client);
    client->map_loaded = client->game.ready;
    // KrunkScript: the map's client script, reloaded every INIT, then start(); script packets
    // only exist on servers of protocol minor >= NET_PROTOCOL_MINOR_V7
    ks_client_round(client, init->major == NET_PROTOCOL_MAJOR ? init->minor : 0);
    hud_ui_on_init(client); // the HUD per the new round's state, the comp lobby's menu or the default one
}

static void client_handle_net_packet(Client *client, const NetPacket *packet) {
    // While the server streams a map we do not have, everything after its INIT
    // waits (spawns, states, events all refer to that map).
    if (client_map_hold(client, packet)) return;

    switch (packet->type) {
        case NET_PACKET_INIT: {
            NetInitPacket init;
            if (!net_packet_read_init(&init, packet)) break;
            if (client_refuse_wrong_flavor(client, &init)) break; // Krunker Strike: a cross-flavor server
            menu_idle_reset(); // v7 io_init: CONTROLS.idleTimer = 0 (a map change restarts the menu idle kick)
            if (client_map_prepare_init(client, &init)) client_apply_init(client, &init);
            break;
        }
        case NET_PACKET_SPAWN: {
            NetSpawnPacket spawn;
            if (!client->game.ready || !net_packet_read_spawn(&spawn, packet)) break;
            client_apply_spawn(client, &spawn);
            spectate_on_player_spawn(client, spawn.uid); // WP-E: a focused player is followed again
            menu_rerender_if_open(23);                   // a new player's row in the Player List
            break;
        }
        case NET_PACKET_SKINS: {
            if (packet->length != sizeof(NetSkinsPacket)) break;

            NetSkinsPacket skins;
            memcpy(&skins, packet->payload, sizeof(skins));
            client_apply_skins(&skins);
            break;
        }
        case NET_PACKET_STATE: {
            if (packet->length != sizeof(NetStatePacket) || !client->game.ready) break;

            NetStatePacket state;
            memcpy(&state, packet->payload, sizeof(state));
            client_apply_state(client, &state);
            break;
        }
        case NET_PACKET_RECON: {
            NetReconPacket recon; // V1 (1.3.0 servers, net-bridge) keeps our own v7 movement locks
            if (!client->game.ready || !net_packet_read_recon(&recon, packet, client->me)) break;
            client_recon_apply(client, &recon);
            break;
        }
        case NET_PACKET_LEAVE: {
            if (packet->length != sizeof(NetLeavePacket)) break;

            NetLeavePacket leave;
            memcpy(&leave, packet->payload, sizeof(leave));
            spectate_on_player_leave(leave.uid); // WP-E: the focused player left
            client_remove_player(client, leave.uid);
            menu_rerender_if_open(23);           // the Player List drops the row (KN: v10 has no leave refresh)
            break;
        }
        default:
            client_handle_game_packet(client, packet); // chat, kills, hits, score, round, ping, maps (net/game_events.c)
            break;
    }

    // A streamed map just completed: start its INIT, then replay what waited for it.
    NetInitPacket init;
    NetPacket *held = NULL;
    size_t held_count = 0;

    if (client_map_take_ready(client, &init, &held, &held_count)) {
        client_apply_init(client, &init);
        for (size_t i = 0; i < held_count; i++) client_handle_net_packet(client, &held[i]);
        free(held);
    }
}

void client_tick_net(Client *client) {
    pthread_mutex_lock(&client->net_lock);

    const size_t packet_count = client->net_packet_count;
    NetPacket *packets = client->net_packets;

    client->net_packet_count = 0;
    client->net_packets = NULL;

    pthread_mutex_unlock(&client->net_lock);

    for (size_t i = 0; i < packet_count; i++) {
        if (!client_recon_delay_inbound(client, &packets[i])) client_handle_net_packet(client, &packets[i]);
    }
    client_recon_drain_delayed(client, client_handle_net_packet); // KRUNKNATIVE_NET_DELAY_MS (off by default)

    free(packets);
}

// The framebuffer size resize_viewport() last applied to the viewport, the HTML layout and the
// headless pbuffer (0x0: not yet). client_sync_window_size() compares it with the window's.
static int g_applied_fb_width, g_applied_fb_height;

void resize_viewport(GLFWwindow *window, const int width, const int height) {
    if (headless_current_window() != window) return;
    headless_resize(width, height); // headless: the pbuffer follows the null-platform window
    glViewport(0, 0, width, height);
    html_ui_resize(width, height);
    g_applied_fb_width = width;
    g_applied_fb_height = height;

    static int log = -1; // KRUNKNATIVE_RESIZE_LOG=1: what each resize applied (client/tests/resize_early_e2e.py)
    if (log < 0) {
        const char *env = getenv("KRUNKNATIVE_RESIZE_LOG");
        log = env && *env && strcmp(env, "0") != 0;
    }
    if (log) {
        GLint viewport[4] = {0};
        float page_width = 0.0f, page_height = 0.0f;
        glGetIntegerv(GL_VIEWPORT, viewport);
        html_dom_viewport(&page_width, &page_height, NULL);
        fprintf(stderr, "[resize] %dx%d viewport %dx%d page %.0fx%.0f\n", width, height, viewport[2], viewport[3], page_width, page_height);
    }
}

// Window size changes are applied idempotently, not only on GLFW's framebuffer-size event: every
// frame compares the framebuffer size with the size resize_viewport() last applied. An event that
// never came or came with a stale size (a synthetic ConfigureNotify) left the viewport, the HTML
// layout and the headless pbuffer at the old size until the next resize ("resizing the window
// before it loads the game"), and a resize while startup blocks the loop drew the first frame at
// the old size (events wait for its glfwPollEvents). The camera aspect comes from the viewport
// (scene.c); the scene / frame targets query the size every frame.
// The installed callback is called, so a hub/editor/viewer page (app_mode.c) hears it too.
static void client_sync_window_size(Client *client) {
    int width = 0, height = 0;
    glfwGetFramebufferSize(client->window, &width, &height);
    if (width <= 0 || height <= 0) return; // minimized: keep the last size
    if (width == g_applied_fb_width && height == g_applied_fb_height) return;

    const GLFWframebuffersizefun callback = glfwSetFramebufferSizeCallback(client->window, NULL);
    glfwSetFramebufferSizeCallback(client->window, callback);
    if (callback) callback(client->window, width, height);
    else resize_viewport(client->window, width, height);
}

// The HTML layer works in framebuffer pixels; GLFW reports cursor positions in
// window coordinates, which differ on HiDPI displays.
static void html_cursor_scale(GLFWwindow *window, double *x, double *y) {
    int window_width, window_height, fb_width, fb_height;
    glfwGetWindowSize(window, &window_width, &window_height);
    glfwGetFramebufferSize(window, &fb_width, &fb_height);

    if (window_width > 0 && window_height > 0) {
        *x *= (double) fb_width / window_width;
        *y *= (double) fb_height / window_height;
    }
}

// The page takes pointer input while the menu is open or the end-screen results are up.
static int html_pointer_input(void) {
    // ks_client_pointer_free: KrunkScript INPUTS.unlockMouse / freeMouse (clicks reach script DIVs)
    return html_ui_is_menu_open() || hud_ui_end_screen_interactive() || ks_client_pointer_free();
}

static void cursor_pos_callback(GLFWwindow *window, double x, double y) {
    ks_client_cursor(glfwGetWindowUserPointer(window), x, y); // KrunkScript INPUTS.mousePos / mouseMovement
    if (!html_pointer_input()) return;
    html_cursor_scale(window, &x, &y);
    if (ui_console_mouse(-1, x, y, 0)) return; // dev console titlebar drag / grip resize
    html_ui_mouse_move(x, y);
}

static void mouse_button_callback(GLFWwindow *window, const int button, const int action, const int mods) {
    Client *client = glfwGetWindowUserPointer(window);
    if (ks_client_mouse_button(client, button, action)) return; // KrunkScript onMouseClick / onMouseUp, re-lock after unlockMouse
    if (!html_pointer_input()) {
        // CONTROLS mousedown with the controls off (a free end title): toggle(true), no shot
        if (action == GLFW_PRESS && client_pointer_click(client)) return;
        // In game: Krunker's canvas mousedown/mouseup (left shoots, right aims, the
        // other buttons are bindable keys M2/M4/M5), sampled by client_tick_game.
        ui_controls_mouse_event(client, button, action, 1);
        return;
    }
    if (ui_controls_mouse_event(client, button, action, 0)) return; // "Press any Key" capture

    double x, y;
    glfwGetCursorPos(window, &x, &y);
    html_cursor_scale(window, &x, &y);
    if (button == GLFW_MOUSE_BUTTON_LEFT && ui_console_mouse(action, x, y, 0)) return; // dev console modal + mod bind rows
    html_ui_mouse_move(x, y);
    html_ui_mouse_button(button, action, mods);
}

void scroll_callback(GLFWwindow *window, const double xoffset, const double yoffset) {
    Client *client = glfwGetWindowUserPointer(window);
    if (html_pointer_input()) {
        if (ui_controls_scroll_event(client, yoffset, 0)) return; // the wheel bound as SCROLL
        html_ui_scroll(xoffset, yoffset);
        return; // menu scrolling must not swap weapons
    }

    if (!client || !yoffset) return;
    ks_client_scroll(client, yoffset); // KrunkScript onMouseScroll
    if (ui_controls_scroll_event(client, yoffset, 1)) return; // a bind uses the wheel: no weapon swap

    if (g_skip_scroll) {
        g_skip_scroll = 0;
        return;
    }
    client->mouse_state.scroll_delta = yoffset > 0.0 ? 1 : -1;
}

// Alt-tab / focus loss in game. In v3.1.1 the browser drops the pointer lock, and the
// pointerlockchange handler (132.js) then runs toggleControlUI(false), which shows the menu,
// and resetKeys(). GLFW keeps GLFW_CURSOR_DISABLED across focus loss, so the native lock
// check (menu_ui.c client_pointer_locked) never sees it: do the same here. On the web the
// browser's own pointer-lock loss already does this (client_menu_tick).
void client_window_focus_lost(Client *client) {
    if (!client || app_mode() != APP_MODE_GAME || SCENARIO_ACTIVE()) return;
    if (!client->mouse_state.locked) return; // menu / end screen: the pointer is already free
    ui_controls_reset_keys();
    if (html_ui_available()) {
        // Over the end screen v7 shows no menu (hideUI: UI.active = false): the title goes on with
        // the pointer free, a click on the game view locks it again (pointer_lock.h).
        if (hud_ui_end_screen_up()) client_pointer_end_free(client);
        else if (!html_ui_is_menu_open()) client_set_menu_open(client, 1);
    } else {
        glfwSetInputMode(client->window, GLFW_CURSOR, GLFW_CURSOR_NORMAL); // no HTML menu: just free the pointer
        client->mouse_state.locked = 0;
    }
}

static void window_focus_callback(GLFWwindow *window, const int focused) {
    Client *client = glfwGetWindowUserPointer(window);
    if (!focused) client_window_focus_lost(client);
#ifndef __EMSCRIPTEN__ // the browser drops its lock itself (pointer_lock.h: `focused` is always 1 there)
    if (client && html_ui_available()) client_pointer_focus(client, focused); // after the menu took the pointer
#endif
}

static void key_callback(GLFWwindow *window, const int key, const int scancode, const int action, const int mods) {
    Client *client = glfwGetWindowUserPointer(window);

#ifndef __EMSCRIPTEN__ // the browser owns fullscreen on the web (and emscripten's GLFW has no monitors)
    // F11: windowed <-> fullscreen, as the browser's F11 in Krunker. An event (not a per-frame poll),
    // so a quick tap is never missed at a low frame rate, and in every state: menu, end screen,
    // death, console and chat included. Reserved: never reaches keybinds or the page.
    if (key == GLFW_KEY_F11) {
        if (action == GLFW_PRESS && client) client_render_toggle_fullscreen(client); // persisted as windowMode
        return;
    }
#endif

    ks_client_key(client, key, scancode, action, mods); // KrunkScript onKeyPress / onKeyUp / onKeyHeld (observes only)

    // Keybinds first (ui_controls.c): the "Press any Key" capture, typing in #chatInput
    // and the bound game keys / hotkeys (chat key, leaderboard, toggles, ...).
    if (ui_controls_key_event(client, key, scancode, action, mods)) return;

#ifndef __EMSCRIPTEN__
    // ESC toggles the menu natively. On the web the page handles ESC itself while
    // the menu is open, and the browser drops pointer lock (detected in
    // client_menu_tick) while in game, so toggling here would double up.
    if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS && html_ui_available() && client) {
        // While the menu is open the page's Escape handling decides (ui_handlers.c:
        // a focused input blurs, an open window closes, otherwise the menu closes),
        // like Krunker's own menu script. main.c only opens the menu.
        if (html_ui_is_menu_open()) {
            html_ui_key(key, scancode, action, mods);
            return;
        }
        if (hud_ui_end_screen_up()) return; // the end screen keeps precedence (e.g. Escape blurring the chat)
        client_set_menu_open(client, !html_ui_is_menu_open());
        return;
    }
#else
    (void) client;
#endif

    if (html_ui_is_menu_open()) html_ui_key(key, scancode, action, mods);
}

static void char_callback(GLFWwindow *window, const unsigned int codepoint) {
    (void) window;
    if (html_ui_is_menu_open() || ui_controls_chat_focused()) html_ui_char(codepoint);
}

void client_animate_object_texture(Object *object, const float now) {
    if (!object->mesh || !object->tex_anim) return;

    const Mesh *mesh = object->mesh;
    const TextureAnimation *tex_anim = object->tex_anim;

    if (mesh->material->vtable != &basic_material_vtable) return;

    BasicMaterial *material = (BasicMaterial *) mesh->material;

    if (tex_anim->frames) {
        // v7 render.js loadTexture / frameMats: a sheet of numX = min(10, frames) columns and
        // numY = ceil(frames / 10) rows (0: one row), frame `cur` at offset (cur % numX / numX,
        // (numY - floor(cur / numX) - 1) / numY); frameT 0 keeps the first frame
        const int frames = tex_anim->frames;
        const int columns = frames < 10 ? frames : 10;
        const int rows = frames > 10 ? (frames + 9) / 10 : 0;
        const int frame = tex_anim->frame_time > 0.0f ? (int) (fmodf(now, (float) frames * tex_anim->frame_time) / tex_anim->frame_time) % frames : 0;

        material->texture_repeat.x = 1.0f / (float) columns;
        material->texture_offset.x = (float) (frame % columns);
        if (rows) {
            material->texture_repeat.y = 1.0f / (float) rows;
            material->texture_offset.y = (float) (rows - frame / columns - 1);
        }
    }

    if (tex_anim->move) {
        const float period = 1.0f / tex_anim->move;
        const float progress = fmodf(now, period) / period;

        if (tex_anim->move_direction) material->texture_offset.y = progress;
        else material->texture_offset.x = progress;
    }
}

void client_tick_textures(Client *client, const float now) {
    if (!client->game.map) return;

    for (size_t i = 0; i < client->game.map->object_count; i++) {
        Object *object = client->game.map->objects[i];
        client_animate_object_texture(object, now);
    }
}

void client_enter_game(Client *client) {
    if (!client->game.ready) return;

    // Spectating (window.Bf5): enterGame acts again only once the Spectate switch is off
    // (`Bf5 && !co`): one plain ENT, and the spectator view stays until our SPAWN.
    const int leave_spectate = spectate_active() && !spectate_requested();
    if (spectate_active() && (!leave_spectate || client->spawn_requested)) return;

    // Respawn after a death: one ENT at a time, not before config.deathDelay. During the
    // round's end phase too: v3.1.1's io_ent spawns the player and sends a late "end"
    // (events_end shows the end screen then); only the end screen itself never re-enters.
    const int respawn = client->in_game && client->me && !client->me->active;
    if (client->in_game && !respawn && !leave_spectate) return;
    if (respawn && (client->spawn_requested || client->net_end_result != INT32_MIN)) return;
    if (respawn && client->net_death_time > 0.0 && glfwGetTime() < client->net_death_time + client->net_respawn_delay) return;

    client->in_game = 1;

    // The Change Loadout look (ui_loadout_look, the same one the menu's class preview wears; Random
    // resolved to owned items): setClass(classIndex, selectedSecondaryIndex), skin colour + cosmetic
    // items (echoed back in SPAWN), ent[2] / ent[9] weapon + melee skins (SKINS), ent[12] attachIndex
    // (forceAttach ? 0 : the pick) and WP-F ENT V9's ent[15] shoeIndex, [21] petIndex, [26] wristIndex,
    // [30] charms, [31] backIndex, [32] headIndex, [33] playerCardIndex, routed to their v7 slots.
    // NetEntPacket is #pragma pack(1) (shared.h): its PlayerSkins sits at an odd offset, so the
    // look is built in an aligned local and copied in; never hand out pointers to its members.
    LoadoutLook look;
    ui_loadout_look(client->menu_class_index, 1, &look);
    NetEntPacket packet = {.class_index = client->menu_class_index, .secondary_index = look.secondary_index};
    // ent[6] = toggleSpect and ent[15] = slidCont ride after the cosmetics (NET_ENT_PACKET_V4_SIZE).
    packet.spectate = (uint8_t) spectate_requested();
    packet.slid_cont = (uint8_t) (ui_controls_settings()->slid_cont ? 1 : 0); // ent[15] settings.tmp.slidCont ("Requires respawn")
    packet.spray_index = ui_loadout_spray(); // ent[1] selectedSprayIndex
    packet.lag_comp = client_render_settings()->lag_comp; // ent[11] parseFloat(settings.tmp.lagCompensation)
    // WP-C ENT V8 (v7.1.4 io.send("en")): ent[12] attachIndex, ent[7] challengeModeEnabled,
    // ent[19] SETTINGS.tmp.anonMode; ent[22..25] showBadges (cosmetics.c)
    packet.attach_index = look.attach_index;
    packet.chall_mode = (uint8_t) (ui_controls_settings()->chall_mode ? 1 : 0);
    packet.anon_mode = (uint8_t) (client_render_settings()->anon_mode ? 1 : 0);
    packet.cosmetics = look.cosmetics;
    packet.skins = look.skins;
    packet.cosmetics_v7 = look.cosmetics_v7;
    packet.show_badges = client_show_badges();
    // A 1.0 server (INIT minor 0) drops anything longer than V4 without a spawn (no spray / skins),
    // a 1.1 server anything longer than V6 (no lagComp), a 1.3.0 server anything longer than V7
    // (no attachIndex / challMode / anonMode): net_ent_size_for.
    const uint16_t ent_size = client->net_ent_size ? client->net_ent_size : (uint16_t) NET_ENT_PACKET_V4_SIZE;

    pthread_mutex_lock(&client->net_lock);
    const int socket = client->net_socket;
    pthread_mutex_unlock(&client->net_lock);

    if (client->offline || socket >= 0) {
        client->spawn_requested = 1; // SPAWN clears it
        combat_send_streak_slots(client); // WP-B: v7 ENT streakIndexes, ahead of the ENT (1.3+ servers)
        client_send_pc_stat(client);      // WP-E: v7 ENT pcStatIndex, ahead of the ENT (1.3+ servers)
        client_net_send(client, NET_PACKET_ENT, &packet, ent_size);
    } else {
        client->in_game = 0;
    }
}

static void client_tick_game(Client*, float, float);

void client_tick(Client *client, const float now, const float delta) {
    html_ui_update(); // pumps the HTML engine and runs the bridge handlers (menu/play/...)
    backend_frame(client); // brain HTTP / social socket delivery, account header, matchmaker (backend_startup.c)
    if (app_mode_frame(client, now, delta)) return; // hub/editor/viewer page: the game is frozen behind it (app_mode.c)

    // renderer.setClearColor(map.sky): the clear colour is GL state that other passes
    // overwrite (the HTML engine leaves it at transparent black), so set it every frame
    // before this clear and render_target_begin()'s. Maps without a sky dome (Littletown,
    // Freight, Subzero, ...) show it as the whole sky.
    glClearColor(g_render_lighting.sky_color.x, g_render_lighting.sky_color.y, g_render_lighting.sky_color.z, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
#ifdef UI_SCENARIO_SUPPORTED
    ui_scenario_draw_background(); // scenario mode: its background replaces the world
#endif

    client_offline_pump(client, delta); // offline: the sandbox server's step on this frame (net/offline.c)
    client_tick_net(client);
    client_game_events_tick(client); // death delay -> menu (net/game_events.c)

    if (client->game.ready) client_tick_game(client, now, delta);
    else input_script_idle_frame(client, delta); // a `boot` script before any map (input_script.h)
    // SOUND.rate = deltaMlt * endAnimationTimeLeft / endAnim (menu_view.c); 1 without a game
    audio_set_rate(client->game.ready ? menu_view_time_scale() : 1.0f);
    sound_events_tick(client, now, delta); // after the game tick: diffs player state into sounds
    voice_update(client);                  // voice chat: push-to-talk clips out, received clips played

    client_menu_tick(client);
    hud_spectate_tick(client); // #spectateUI while spectating (after hud_ui.c's pass in client_menu_tick)
    hud_map_tick(client);      // WP-E part 2a: minimap + death markers (client/src/ui/hud_map.c)
    ui_mobile_tick(client);    // #mobileUI touch controls on phones (client/src/ui/ui_mobile.c)
    if (debug_capture_hide_ui()) {
        // KRUNKNATIVE_HIDE_UI (debug_capture.c): world only, no page/preview
    } else if (!SCENARIO_ACTIVE() || SCENARIO_PREVIEW()) {
        client_class_preview_render(client); // selected class model: the frame of the page's #classPreviewCanvas
    }
    if (!SCENARIO_ACTIVE()) client_render_settings_tick(client); // DOM pieces the display settings control (after the HUD pass)
    if (!debug_capture_hide_ui()) {
        spin_wheel_render(client);           // the prize wheel's canvas under its #spinButton (ui/spin_wheel.c)
        ks_client_ui_late(client);           // KrunkScript UI.hideDefault / hideCrosshair over the HUD's writes
        html_ui_render_page(client->ui);     // composites the page over the scene and HUD
        viewer_embed_frame(client, delta);   // the item view's 3D showcase over its iframe box (viewer_embed.h)
        spin_wheel_render_confetti(client);  // canvas-confetti draws above the page
        html_ui_render_top(client->ui);      // last: the developer console (the top layer) over all of it
    }
    input_script_frame_presented(client); // `frames`: every finished frame (input_script.c)
    debug_capture_frame_end(client); // KRUNKNATIVE_SCREENSHOT: reads the finished frame
}

static void client_tick_game(Client *client, const float now, const float frame_delta) {
    // deltaMs *= endAnimationTimeLeft / config.endAnim: the round-end slow motion (menu_view.c)
    const float delta = menu_view_world_delta(client, frame_delta);
    input_script_tick(client); // KRUNKNATIVE_INPUT_SCRIPT: due scripted keys/mouse/pose (no-op otherwise)
    const int html_menu = html_ui_available();
    // Scenario mode draws no world and no legacy HUD/overlay (only the background
    // under the page); the first-person weapon only with state.preview.
    const int scenario = SCENARIO_ACTIVE();

    // The scroll reset and the HUD-or-overlay choice below use the pointer lock as it was
    // before this frame's input sampling (as when the render ran first in the frame).
    const int was_locked = client->mouse_state.locked;
    if (!was_locked) client->mouse_state.scroll_delta = g_skip_scroll = 0;
    // The pointer-lock owner (menu_ui.c): mouse_state.locked = CONTROLS.enabled from the UI state,
    // the cursor mode / canvas lock to match, before this frame samples the mouse.
    if (html_menu) client_pointer_update(client);


    // v3.1.1 keeps the whole game loop running behind the menu (index.js gameLoop): the
    // world, other players, effects and our own player (procInputs with the keys reset)
    // keep updating; only local input is cut off. Cameras per state: menu_view.c.
    // The end screen's results disable the controls too (endGame -> CONTROLS.disable()).
    const int menu_open = html_menu && (html_ui_is_menu_open() || hud_ui_end_screen_interactive());
    if (menu_open) {
        // Menu owns the input: no hotkeys, no look/move sampling.
        ui_controls_reset_keys(); // CONTROLS.resetKeys() when the pointer lock is lost
        double menu_x, menu_y;
        glfwGetCursorPos(client->window, &menu_x, &menu_y);
        client->mouse_state.last_pos.x = (float) menu_x;
        client->mouse_state.last_pos.y = (float) menu_y;
    }

    // F11 (fullscreen) is handled in key_callback. N is premiumKeys[0] in ui_controls.c. ` opens
    // the developer console (ui_console.c); the map cycle packet it used to send is the console
    // command `cycle_packet`.
    double x, y;
    glfwGetCursorPos(client->window, &x, &y);
    if (menu_open) {
        // client_set_menu_open freed the pointer; nothing to lock or enter here.
    } else if (html_menu) {
        // Menu closed: the game owns the mouse (client_pointer_update above), unless the end title
        // runs free or a KrunkScript INPUTS.unlockMouse freed it until lockMouse / a click.
    } else {
        client->mouse_state.locked = glfwGetInputMode(client->window, GLFW_CURSOR) == GLFW_CURSOR_DISABLED;

        if (client->mouse_state.locked && glfwGetKey(client->window, GLFW_KEY_ESCAPE)) {
            glfwSetInputMode(client->window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
            client->mouse_state.locked = 0;
        } else if (!client->mouse_state.locked && glfwGetMouseButton(client->window, GLFW_MOUSE_BUTTON_LEFT)) {
            glfwSetInputMode(client->window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);

            client->mouse_state.locked = 1;
            client->mouse_state.last_pos.x = (float) x;
            client->mouse_state.last_pos.y = (float) y;
        }
    }

    // Dead: v3.1.1 only respawns from the menu cE() reopens after config.deathDelay
    // (enterGame runs on pointer lock, never per frame), so no ENT while that is pending;
    // otherwise the frame the delay runs out could respawn before the menu shows.
    if (!menu_open && client->mouse_state.locked && (!client->me || !client->me->active) && !client->net_death_menu_pending) {
        client_enter_game(client);
    }


    // This frame's look movement: the cursor travel natively, the locked movement sums on the web.
    float look_x = (float) x - client->mouse_state.last_pos.x;
    float look_y = (float) y - client->mouse_state.last_pos.y;
#ifdef __EMSCRIPTEN__
    double web_x, web_y;
    web_take_look(&web_x, &web_y);
    look_x = (float) web_x;
    look_y = (float) web_y;
#endif
    // v7 f2137: mouse movement only reaches the camera under the pointer lock. A free cursor (the
    // menu, the end screen, a free end title, the console, freeMouse) never turns it.
    if (!client_pointer_look_allowed(client)) look_x = look_y = 0.0f;

    // WP-C v7.1.4 netRate: an input frame is sampled (predicted, sent) only once the frame time since
    // the last one reaches half a netRate period; it carries that summed delta (net/recon.c).
    int input_sampled = 0;
    g_look_unsampled = 0;
    third_person_frame(client); // KrunkNative third person: the local model's mode, the re-ask after INIT
    if (client->me && client->me->active) {
        Input input = {0};
        vec2 mouse_delta = {0};

        mouse_delta.x = look_x;
        mouse_delta.y = look_y;

        const float input_delta = client_recon_sample(client, delta);
        input_sampled = input_delta >= 0.0f;
        input.move_dir = -1;
        input.delta = input_sampled ? input_delta : 0.0f;

        // Krunker's camera keeps the unrounded look (CONTROLS xDr/yDr) and only the input
        // frame is rounded to 0.001 rad, so slow mouse movement still adds up.
        // player_proc_input rounds, so build the input from a client-side copy. It resyncs
        // whenever me->direction stops matching the look of the last sampled frame (spawn,
        // reconciliation, teleport, pose); between samples the look keeps turning (netRate).
        const Player *me = client->me;
        // An unsampled frame's own look on me->direction (below) is in sync too.
        const float look_pitch = CLAMP(player_round3(g_look_sent.x), -(float) M_PI / 2.0f, (float) M_PI / 2.0f);
        const int look_sent = fabsf(look_pitch - me->direction.x) <= 1e-5f && fabsf(player_round3(g_look_sent.y) - me->direction.y) <= 1e-5f;
        const int look_shown = g_look_shown_valid && me->direction.x == g_look_shown.x && me->direction.y == g_look_shown.y;
        if (!g_look_valid || (!look_sent && !look_shown)) {
            g_look.x = g_look_sent.x = me->direction.x;
            g_look.y = g_look_sent.y = me->direction.y;
            g_look_valid = 1;
        }
        g_look_shown_valid = 0;
        input.x_dir = g_look.x;
        input.y_dir = g_look.y;
        const float om_prev_pitch = input.x_dir, om_prev_yaw = input.y_dir; // the look before this frame's mouse (om_build.c)

        if (client->mouse_state.locked && !menu_open) {
            if (!input_sampled) {
                // CONTROLS.updateMouseInp runs every frame: only the look turns, keys wait for the sample
                ui_controls_fill_look(client, &input, mouse_delta.x, mouse_delta.y);
                input_script_adjust_input(client, &input); // scripted look (tests)
            } else {
                // Look (sensitivity/aim sensitivity/invertY), move binds, bound buttons and
                // the weapon keys (pressed this frame) from the keybind table (ui_controls.c).
                ui_controls_fill_input(client, &input, mouse_delta.x, mouse_delta.y);
                input_script_adjust_input(client, &input); // scripted look (tests)

                // input[10] = wDG6 * scrollDirM: wheel up (wDG6 -1) steps back a slot, down forward;
                // procInputs only scrolls when input[11] (a swap key) is 0. The wheel only reaches
                // here when no bind uses it (bMoQ7, ui_controls_scroll_event).
                const int scroll_delta = client->mouse_state.scroll_delta;
                input.scroll = (signed char) (scroll_delta ? (scroll_delta > 0 ? -1 : 1) * ui_controls_scroll_dir() : 0);
                g_skip_scroll = scroll_delta != 0; // skipScroll: false before b9, then bMoQ7 when wDG6 was set
                client->mouse_state.scroll_delta = 0;
                client->last_swap_key = 0; // weapon keys are press events now (CONTROLS.aba6)
            }
            // KrunkNative Object Manager: the Physics Gun / btools take fire, the wheel and the rotate key
            om_build_input(client, &input, om_prev_pitch, om_prev_yaw, input_sampled);
        } else if (input_sampled) {
            // a window over the game (the Object Manager's Tool Gun / Physics Gun tiles): the slot pick
            // still goes out, so the gun is in hand at once and not only when the window closes
            input.select_slot = (unsigned char) ui_controls_take_slot();
        }
        // Round over, v3.1.1 parity (index.js endGame / gameLoop): through the endAnim title the
        // controls stay enabled, so WASD/jump/shots/ADS still act locally in the shrinking slow
        // motion; nothing is sent (the "q" batch needs !endAnimationTimeLeft, recon.c). Once
        // endAnim ran out, CONTROLS.disable() (toggle(false) -> resetKeys, locked) frees the
        // pointer for the results (hud_ui_end_screen_interactive above resets the keys) and
        // the world stands still (deltaMs * 0). Without the HTML end screen this keeps the
        // frozen results input empty too. INIT resets the keys: nothing reaches the next round.
        if (client_round_ended(client) && menu_view_time_scale() <= 0.0f) {
            input = (Input) {.move_dir = -1, .delta = input.delta, .x_dir = input.x_dir, .y_dir = input.y_dir};
        }
        // controls.js: the pitch is clamped and xDr = rotation.y % Math.PI2 before the input
        // frame takes them, so the yaw sent (and predicted) never grows without bound.
        input.x_dir = player_clamp_pitch(input.x_dir);
        input.y_dir = player_wrap_yaw(input.y_dir);
        g_look.x = input.x_dir;
        g_look.y = input.y_dir;
        // KrunkNative third person sends this look unchanged, as v10's thirdPerson does (the input
        // frame takes CONTROLS yDr/xDr, v10.js 221504): the camera only rides behind the shoulder.

        if (input_sampled) {
            g_look_sent.x = input.x_dir; // what me->direction takes (the resync test above)
            g_look_sent.y = input.y_dir;
            input.seq = client->net_next_input_seq++;
            // input[1] = Math.round(inputFrameDelta * config.deltaMlt) (v10:221748): a time-scaled lobby
            // runs (and sends) every frame scaled; player_proc_input rounds it to whole ms
            if (client->game.config.delta_mlt > 0.0f && client->game.config.delta_mlt != 1.0f) input.delta *= client->game.config.delta_mlt;
            // KrunkScript: INPUTS.disableDefault (dontSendInputs) neither predicts nor sends,
            // DEFAULT.disablePrediction (noPred) sends without predicting (index.js:16840-16847)
            if (!ks_client_inputs_disabled() && !ks_client_no_prediction()) player_queue_input(client->me, &input); // prediction: procInputs(b9) this frame
            if (!ks_client_inputs_disabled()) client_recon_record(client, &input); // tmpInpts + the "q" window (online only, net/recon.c)
        } else {
            g_look_unsampled = 1; // the camera shows g_look (camera_feel_apply below poses me->direction)
            // v7.1.4 gameLoop, no input frame this frame: tmpPlayer.yDire = CONTROLS.yDr; xDire =
            // cameraYaw; updateMshClient. The player (so its first-person arms, posed in the world
            // tick below) turns with the camera between input frames. Without it the arms kept the
            // last sampled look while the camera turned on: in the round-end slow motion the
            // scaled delta samples ever fewer frames (none at all once it reaches 0, for the whole
            // results screen), so the viewmodel fell behind the camera and stayed off.
            if (player_look_follows_camera(client->me)) {
                client->me->direction.x = input.x_dir;
                client->me->direction.y = input.y_dir;
                g_look_shown.x = input.x_dir;
                g_look_shown.y = input.y_dir;
                g_look_shown_valid = 1;
            }
        }

        // The camera is posed after game_tick() below, once this input has moved the player.
    } else if (spectate_active()) {
        // window.Bf5: free cam / follow cams from the mouse and the move keys (spectate.c).
        // The branch runs every frame, so follow cams keep tracking behind the menu.
        const int look = client->mouse_state.locked && !menu_open;
        spectate_tick(client, delta, look ? look_x : 0.0f, look ? look_y : 0.0f);
    } else {
        // dead: CONTROLS.updateMouseInp turns the death rig with the mouse (menu_view.c), the chase
        // (menu_view_camera below) keeps pulling it toward the killer
        const int look = client->mouse_state.locked && !menu_open;
        menu_view_look(client, look ? look_x : 0.0f, look ? look_y : 0.0f);
    }
    // didPressed / aba6 / the wheel key last one input frame (v7: wSwap and scrollDelta are cleared
    // when a frame is sampled, so a press between samples reaches the next one)
    if (input_sampled || !client->me || !client->me->active) ui_controls_end_frame();

    client->mouse_state.last_pos.x = (float) x;
    client->mouse_state.last_pos.y = (float) y;

    // The world on the endAnim-scaled delta; the offline game's round/end clocks on real time.
    game_tick_world(&client->game, now, frame_delta, delta);
    client_recon_after_tick(client); // this input's shot knockback, kept for replays (input.velObj)
    ks_client_frame(client, delta, frame_delta); // KrunkScript update(delta) + script scene objects (game.js:4960)

    // Krunker poses the camera right after procInputs (index.js gameLoop: rotateCam with
    // recoil/landing bob/shake, moveCam, ADS zoom) and renders in the same frame, so this
    // frame shows this frame's input.
    if (client->me && client->me->active) {
        camera_feel_apply(client);
        if (third_person_view(client)) {
            third_person_apply_camera(client, g_look, delta); // over the shoulder (third_person.c)
        } else if (g_look_unsampled) { // WP-C netRate: the look of this unsampled frame (rotateCam from CONTROLS)
            client->camera.rotation.x += player_clamp_pitch(g_look.x) - client->me->direction.x;
            const float yaw = g_look.y - client->me->direction.y;
            client->camera.rotation.y += yaw - 2.0f * (float) M_PI * roundf(yaw / (2.0f * (float) M_PI));
        }
    }
    menu_view_camera(client, delta); // no active player: pre-spawn menu camera / death chase cam
    ks_client_camera(client); // KrunkScript GAME.CAMERA: a detached camera replaces the pose
    camera_feel_update(MIN(delta, game_constants.max_delta)); // RENDER.updateShake(deltaMs)
    hud_ui_nuke_flash_step(delta); // index.js gameLoop: the Nuke's #imgFlash fades by gameTimeDiff
    for (size_t i = 0; i < client->game.player_count; i++) player_mesh_debug_log(client->game.players[i], now); // KRUNKNATIVE_PLAYER_LOG

    // World + first-person scene, then the native HUD / overlay (client_tick composites the page after).
    // resolution/antiAlias/aspectRatio/saturationn/hue: world + first-person passes go
    // through the scaled scene target when one is needed (render_target.c).
    const int scene_target = !scenario && render_target_begin(client);
    int tp_own_model = 0; // KrunkNative third person: the local model drawn in the world pass

    if (!scenario) vfx_update(client, delta); // combat effects from this frame's game tick (vfx.c)
    // store-item material animations: frames, movT, pulsT, rgb, rAnim (skin_fx.c), with textureAnim
    skin_fx_tick(delta * 1000.0f, client_render_settings()->texture_anim, client_render_settings()->lighting == 0);
    if (!scenario) ai_render_tick(client, delta); // AI meshes / orbs (ai_render.c), drawn by the world scene
    nav_overlay_frame(client); // show_navmeshes: a finished build, map switches (nav_overlay.c)

    if (!scenario && !ks_client_3d_disabled()) { // KrunkScript DEFAULT.disable3D: RENDER.disable()
        // textureAnim off: Krunker resets moving/framed textures to their first frame.
        client_tick_textures(client, client_render_settings()->texture_anim ? now : 0.0f);
        debug_capture_apply_camera(client); // KRUNKNATIVE_CAMERA pose (debug_capture.c)
        map_environment_update(client->game.map, client->camera.position); // v7 ENVIRONMENT_ZONE swaps (map_environment.c)
        map_post_frame(client->scene, &client->game, &client->camera, delta * 1000.0f); // spin, sign distance, GLTF assets, builds, zone (map_post.c)
        om_interp_frame(client); // Object Manager: others' edits eased, our own carry where it is now (om_interp.c)
        zombies_render_frame(client, delta * 1000.0f); // WP-H: mystery box / krum weapons, beams, airdrops (zombies_render.c)
        shadow_update(); // static sun shadow map, re-rendered after a map load (shadow.c)
        ocean_update(client->scene, &client->camera, delta * 1000.0f); // v7 ocean: time + mirror reflection pass (ocean.c)
        sky_tick(delta * 1000.0f);
        sky_render(&client->camera); // Krunker's backgroundScene pass: sky dome first (sky.c)
        shadow_begin_world();
        spectate_render_begin(client); // first-person spectating hides the followed player's model
        // KrunkNative third person: the local (third-person) model is drawn with the world
        Player *tp_me = third_person_draw_model(client) ? client->me : NULL;
        tp_own_model = tp_me != NULL;
        if (tp_me) {
            scene_remove_player_mesh(client->fps_scene, tp_me->mesh, tp_me->loadout_size);
            scene_add_player_mesh(client->scene, tp_me->mesh, tp_me->loadout_size);
        }
        scene_render(client->scene, &client->camera);
        if (tp_me) {
            scene_remove_player_mesh(client->scene, tp_me->mesh, tp_me->loadout_size);
            scene_add_player_mesh(client->fps_scene, tp_me->mesh, tp_me->loadout_size);
        }
        spectate_render_end(client);
        shadow_end_world();
        vfx_render_world(client);   // tracers, impacts, casings, explosions, map particles
        nav_overlay_render(client); // show_navmeshes: nav areas + connections, over the world (nav_overlay.c)
        om_build_render(client);    // Object Manager: the Physics Gun's beam, the btools' box and handles (om_build.c)
    }

    if ((!scenario || SCENARIO_PREVIEW()) && !ks_client_3d_disabled()) {
        glClear(GL_DEPTH_BUFFER_BIT);
        // fpsFOV: the first-person camera has its own fov (Krunker fpsCamera, zoomed alike);
        // showWeapon/Sec/Mel hide the model of the weapon in hand.
        Camera fps_camera = client->camera;
        fps_camera.fov = ui_controls_fps_fov();
        // hideADS: no weapon model while fully aimed (render_settings.h)
        // KrunkNative third person (third_person.h): the local model went into the world pass (or is
        // hidden, camera pulled into the head); never a viewmodel. v7's own thirdPerson keeps this pass.
        const int viewmodel = ui_controls_weapon_visible(client) && !client_render_hide_weapon_ads(client) && !third_person_view(client);
        third_person_note_frame(tp_own_model, viewmodel);
        if (viewmodel) {
            if (!scenario) shadow_begin_world(); // v3.1.1 arms/weapon receive the sun shadow too
            scene_render(client->fps_scene, &fps_camera);
            if (!scenario) shadow_end_world();
            if (!scenario) vfx_render_fps(client, &fps_camera); // first-person muzzle flash
        }
    }

    if (scene_target) render_target_end(client); // upscale + filter onto the window; HUD/overlay/page stay native

    ui_update(client->ui);

    if (!was_locked) {
        if (!html_menu && !scenario) hud_render(client, now); // the HTML page is the menu otherwise
    } else if (!scenario) {
        overlay_render(client, delta);
    }

    if (!scenario) vfx_render_screen(client); // overlay-canvas muzzle flash + damage overlay
    if (!scenario) nametags_tick(client); // overlay-canvas nametags + mode markers (page elements)
    if (!scenario) hud_objectives_render(client);     // v7 flag pings, Hardpoint / Domination markers (game-overlay canvas)
    if (!scenario || build_ui_scenario_active()) build_ui_render(client); // v7 charge bar (interactMProg) and build wheel (game-overlay canvas)
    if (!scenario) sprays_wheel_render(client, delta); // WP-E: v7 spray wheel (game-overlay canvas)
    if (!scenario) ks_overlay_frame(frame_delta); // KrunkScript overlay canvas: script render(), below the HTML page
    input_script_frame_end(client, delta, frame_delta); // KRUNKNATIVE_INPUT_LOG row
}
