#pragma once
// GAME.OVERLAY (krunkscript.md 3.1.10, WP5): the v7 overlay canvas API on Canvas2D
// (client/src/ks/ks_overlay.c). This layer owns the v7 argument semantics (UTILS.isNumber /
// isHexColor / isString defaults, parseFloat for offset/scale, degrees vs radians, the
// drawCircle "center" quirk, lineDashOffset setting lineWidth, ...) so the client KrunkScript
// host (WP4) only converts KsValue arguments to KsOvArg and calls ks_overlay_call().
//
// Frame protocol (client/src/ks/ks_overlay_client.c drives it from the game frame, the test
// tools/canvas2d drives it directly):
//   ks_overlay_begin_frame(...)   size + UI scale; the engine's clearRect of overlay.render
//   ks_overlay_render_begin()     save + scale(scale): the script's render(delta) hook draws
//   ks_overlay_render_end()       restore
//   ks_overlay_composite()        the layer over the bound framebuffer (above the 3D scene and
//                                 the native overlay, below the HTML UI as in v7's z-order)
// Calls outside render draw unscaled and are erased by the next begin_frame (v7 behaviour).
#include <stddef.h>
#include <stdio.h>
#include <canvas2d.h>

// A script value as seen by an API function (WP4 converts KsValue -> KsOvArg).
typedef enum {
    KS_OV_UNDEF = 0, // missing argument / undefined
    KS_OV_NULL,
    KS_OV_BOOL,
    KS_OV_NUM,
    KS_OV_STR, // str: UTF-8, NUL-terminated, borrowed for the call
    KS_OV_OBJ, // object/array/action: truthy, never a number/string/colour
} KsOvType;

typedef struct {
    KsOvType type;
    double num;
    int boolean;
    const char *str;
} KsOvArg;

typedef enum {
    KS_OV_RET_UNDEF = 0,
    KS_OV_RET_NUM,   // num
    KS_OV_RET_SIZE,  // {width, height}
    KS_OV_RET_ERROR, // a host exception (v7: IndexSizeError from drawCircle with a negative
                     // radius): WP4 reports `Internal Error "<path>"` (krunkscript.md 3.0)
} KsOvRetType;

typedef struct {
    KsOvRetType type;
    double num;
    double width, height;
} KsOvResult;

// One id per GAME.OVERLAY member, in API order (API:1883-2221).
typedef enum {
    KS_OV_GET_SIZE, KS_OV_OFFSET, KS_OV_SCALE, KS_OV_CLEAR, KS_OV_MEASURE_TEXT,
    KS_OV_DRAW_CIRCLE, KS_OV_DRAW_RECT, KS_OV_DRAW_LINE, KS_OV_DRAW_TEXT, KS_OV_DRAW_IMAGE,
    KS_OV_BEGIN_PATH, KS_OV_CLOSE_PATH, KS_OV_SAVE, KS_OV_RESTORE, KS_OV_TRANSLATE,
    KS_OV_TRANSFORM, KS_OV_SET_TRANSFORM, KS_OV_MOVE_TO, KS_OV_LINE_TO, KS_OV_FILL,
    KS_OV_FILL_STYLE, KS_OV_STROKE, KS_OV_STROKE_TEXT, KS_OV_STROKE_RECT, KS_OV_STROKE_STYLE,
    KS_OV_LINE_WIDTH, KS_OV_LINE_DASH_OFFSET, KS_OV_LINE_JOIN, KS_OV_ELLIPSE, KS_OV_ARC,
    KS_OV_ARC_TO, KS_OV_BEZIER_CURVE_TO, KS_OV_QUADRATIC_CURVE_TO, KS_OV_GLOBAL_ALPHA,
    KS_OV_METHOD_COUNT
} KsOvMethod;

const char *ks_overlay_method_name(KsOvMethod); // "drawRect", ...
int ks_overlay_method_lookup(const char *name); // KsOvMethod, -1 when unknown

// GAME.OVERLAY.<method>(args...). argc may be smaller than the declared parameters (missing =
// undefined); extra arguments are ignored.
KsOvResult ks_overlay_call(KsOvMethod, const KsOvArg *args, int argc);

// The overlay canvas (created on first use; NULL on allocation failure).
Canvas2D *ks_overlay_canvas(void);
int ks_overlay_active(void); // the canvas exists and shows or has pending drawing

// canvas_w/h: window inner size in CSS px (v7 canvas.width/height); device_w/h: framebuffer px;
// scale: v7 overlay.scale (index.js scaleBase uiScale).
void ks_overlay_begin_frame(int canvas_w, int canvas_h, int device_w, int device_h, double scale);
void ks_overlay_render_begin(void);
void ks_overlay_render_end(void);
void ks_overlay_composite(void);
void ks_overlay_shutdown(void); // frees the canvas (GL context current)

// drawImage(assetID): full file path of a user asset, 1 when resolved. Default: assets_path()
// + "user-assets/<id>/texture.png" for non-negative integer ids (v7 userAssetsUrl(id +
// "/texture.png")). Tests and the asset downloader may install their own (NULL = default).
typedef int (*KsOverlayImageResolver)(double asset_id, char *path, size_t size);
void ks_overlay_set_image_resolver(KsOverlayImageResolver);

// Canvas size for getSize()/clear() before the first ks_overlay_begin_frame (scripts read
// GAME.OVERLAY.getSize() in global initialisers): fills the begin_frame arguments, 1 on success.
typedef int (*KsOverlaySizeProvider)(int *canvas_w, int *canvas_h, int *device_w, int *device_h, double *scale);
void ks_overlay_set_size_provider(KsOverlaySizeProvider);

// JavaScript parseFloat on a string (NaN when no number prefix), exposed for tests.
double ks_overlay_js_parse_float(const char *s);

// ---- call scripts (client/src/ks/ks_overlay_scene.c): tools/canvas2d scenes, one
// GAME.OVERLAY call per line ("//" comments), e.g. `drawRect 10 20 100 50 30 "#ff0000" 0.5 true`.
// Arguments: numbers, "strings" (\" \\ \n escapes), true, false, null, undefined, NaN, {} (object).
// Directives: @size W H, @scale S, @dpr R, @background #rrggbb,
//   @image ID path    drawImage(ID) loads `path` ("assets/..." is relative to assets_path())
//   @font <css font>  the context font before the first call, e.g. "10px GameFont"
//   @outside          the following calls run outside render (no save/scale bracket)
#define KS_OVERLAY_SCENE_MAX_IMAGES 16
typedef struct {
    int width, height;  // canvas px (default 800 x 600)
    double scale, dpr;  // overlay.scale, device px per canvas px (default 1, 1)
    float background[3];
    int image_count;
    double image_id[KS_OVERLAY_SCENE_MAX_IMAGES];
    char image_path[KS_OVERLAY_SCENE_MAX_IMAGES][256];
    char font[64]; // @font, "" = the canvas default
} KsOverlaySceneInfo;
typedef struct KsOverlayScene KsOverlayScene;
KsOverlayScene *ks_overlay_scene_load(const char *path); // NULL when unreadable
const KsOverlaySceneInfo *ks_overlay_scene_info(const KsOverlayScene*);
int ks_overlay_scene_call_count(const KsOverlayScene*);
// Replays the calls (setting @font first); in_render 0 brackets the non-@outside calls with
// ks_overlay_render_begin/end itself. Return values go to `log` ("<line>: <method> -> <value>").
void ks_overlay_scene_replay(const KsOverlayScene*, int in_render, FILE *log);
void ks_overlay_scene_use_images(const KsOverlayScene*); // its @image ids as the image resolver (NULL: default)
void ks_overlay_scene_free(KsOverlayScene*);

// ---- client game frame (client/src/ks/ks_overlay_client.c; not part of the test build)
// The client KrunkScript host (WP4) installs the script's `render(delta)` hook here; it runs
// between ks_overlay_render_begin/end every frame with the frame time in ms (v7
// Game.mainScript.render(gameTimeDiff)). NULL removes it (script unloaded).
typedef void (*KsOverlayRenderHook)(void *user, double delta_ms);
void ks_overlay_set_render_hook(KsOverlayRenderHook fn, void *user);
// Installs the client size provider (window CSS size, framebuffer, UI scale). WP4: call it when
// the client KsHost is created, before the script's global initialisers run. Idempotent; the
// render hook setter and ks_overlay_frame call it too.
void ks_overlay_client_init(void);
// main.c, in the game frame after the native overlay and before the HTML page is composited
// onto the window framebuffer: sizes the canvas from the viewport (CSS px + devicePixelRatio)
// and the UI scale, clears it, runs the render hook and composites. Without a hook and with an
// empty canvas it returns before any GL call. Debug: KRUNKNATIVE_OVERLAY_SCENE=<scene.txt> (web:
// ?overlayScene=<path under the asset host>) replays a call script as the render hook while no
// script hook is installed.
void ks_overlay_frame(float frame_delta_seconds);
