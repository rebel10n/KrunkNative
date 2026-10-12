// GAME.OVERLAY in the client frame: viewport/UI-scale plumbing and the script render hook
// around ks_overlay.c (which stays free of client state so tools/canvas2d can drive it).
#include <gl.h>
#include <client.h>
#include <headless.h>
#include <html_dom.h>
#include <ks_overlay.h>
#include <ui_logic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

static KsOverlayRenderHook g_hook;
static void *g_hook_user;
static KsOverlayScene *g_debug_scene; // KRUNKNATIVE_OVERLAY_SCENE
static int g_debug_scene_tried;

void ks_overlay_set_render_hook(const KsOverlayRenderHook fn, void *user) {
    ks_overlay_client_init();
    g_hook = fn;
    g_hook_user = user;
}

// index.js scaleBase(): uiScale fits 1700x900 * uiCustomScale (2 - scaleUI) into the window,
// 1 with uiScaling off; overlay.render(uiScale, ...) sets overlay.scale (same as nametags.c).
static double overlay_ui_scale(const float vw, const float vh) {
    if (ui_setting_is_false("uiScaling")) return 1.0;
    double scale_ui;
    if (!ui_setting_number("scaleUI", &scale_ui) || scale_ui != scale_ui) scale_ui = 0.8;
    const double custom = 2.0 - scale_ui;
    const double sx = vw / (custom * 1700.0), sy = vh / (custom * 900.0);
    return sy < sx ? sy : sx;
}

static void debug_scene_hook(void *user, const double delta_ms) {
    (void) delta_ms;
    ks_overlay_scene_replay(user, 1, NULL);
}

// CSS viewport (v7 canvas = window.innerWidth x innerHeight) for a framebuffer of fb_w x fb_h.
static void css_viewport(const int fb_w, const int fb_h, float *vw, float *vh) {
    float dpr = 1;
    html_dom_viewport(vw, vh, &dpr);
    if (*vw <= 0 || *vh <= 0) { // no HTML page (HTML_ENGINE=OFF): CSS px = framebuffer px
        *vw = (float) fb_w;
        *vh = (float) fb_h;
    }
}

static int client_size(int *canvas_w, int *canvas_h, int *device_w, int *device_h, double *scale) {
    GLFWwindow *window = headless_current_window();
    int fb_w = 0, fb_h = 0;
    if (window) glfwGetFramebufferSize(window, &fb_w, &fb_h);
    if (fb_w <= 0 || fb_h <= 0) return 0;
    float vw, vh;
    css_viewport(fb_w, fb_h, &vw, &vh);
    *canvas_w = (int) vw;
    *canvas_h = (int) vh;
    *device_w = fb_w;
    *device_h = fb_h;
    *scale = overlay_ui_scale(vw, vh);
    return 1;
}

void ks_overlay_client_init(void) { ks_overlay_set_size_provider(client_size); }

// KRUNKNATIVE_OVERLAY_SCENE=<file>; on the web ?overlayScene=<path under the asset host>.
static void debug_scene_load(void) {
    g_debug_scene_tried = 1;
    char path[512] = "";
    const char *env = getenv("KRUNKNATIVE_OVERLAY_SCENE");
    if (env && *env) snprintf(path, sizeof(path), "%s", env);
#ifdef __EMSCRIPTEN__
    char *query = (char *) EM_ASM_PTR({
        var value = null;
        try { value = new URLSearchParams(location.search).get('overlayScene'); } catch (e) {}
        return value ? stringToNewUTF8(value) : 0;
    });
    if (query) {
        snprintf(path, sizeof(path), "%s%s", assets_path(), query[0] == '/' ? query + 1 : query);
        free(query);
        ensure_asset_file(path);
    }
#endif
    if (!path[0]) return;
    g_debug_scene = ks_overlay_scene_load(path);
    if (g_debug_scene) ks_overlay_scene_use_images(g_debug_scene);
    fprintf(stderr, "[overlay] debug scene %s: %s\n", path, g_debug_scene ? "loaded" : "unreadable");
}

void ks_overlay_frame(const float frame_delta_seconds) {
    if (!g_debug_scene_tried) {
        ks_overlay_client_init();
        debug_scene_load();
    }
    KsOverlayRenderHook hook = g_hook;
    void *hook_user = g_hook_user;
    if (!hook && g_debug_scene) {
        hook = debug_scene_hook;
        hook_user = g_debug_scene;
    }
    if (!hook && !ks_overlay_active()) return;

    int viewport[4];
    glGetIntegerv(GL_VIEWPORT, viewport);
    if (viewport[2] <= 0 || viewport[3] <= 0) return;
    float vw, vh;
    css_viewport(viewport[2], viewport[3], &vw, &vh);
    // v7 canvas.width = window.innerWidth (integer CSS px); the layer renders at framebuffer px
    ks_overlay_begin_frame((int) vw, (int) vh, viewport[2], viewport[3], overlay_ui_scale(vw, vh));
    if (hook) {
        ks_overlay_render_begin();
        hook(hook_user, (double) frame_delta_seconds * 1000.0);
        ks_overlay_render_end();
    }
    ks_overlay_composite();
}
