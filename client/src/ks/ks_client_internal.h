// KrunkScript client host (doc/v7/krunkscript.md 6.6 WP4): state shared by client/src/ks/
// ks_client_host.c (lifecycle, hooks, KsHost, PLAYERS / NETWORK / DEFAULT), ks_ui.c (GAME.UI),
// ks_scene.c (GAME.SCENE, obj3D / bone / light objects), ks_sound.c (GAME.SOUND), ks_input.c
// (GAME.INPUTS, GAME.CAMERA, key names) and ks_misc.c (COOKIES, SETTINGS, PLATFORM, MODS, ADS,
// setTitle, changeGame). Not a public header: main.c and the net code use ks_client.h.
#pragma once
#include <client.h>
#include <ks.h>
#include <ks_api.h>
#include <ks_game.h>
#include <ks_net.h>
#include <math.h>
#include <stdint.h>

// ---- argument helpers (v7 UTILS.isNumber / isString / isHexColor, krunkscript.md 3.0)
static inline KsValue ksc_arg(const KsValue *args, const int argc, const int i) { return i < argc ? args[i] : ks_undef(); }
static inline int ksc_is_num(const KsValue v) { return v.tag == KS_NUM && isfinite(v.as.num); }
static inline double ksc_num_or(const KsValue v, const double fallback) { return ksc_is_num(v) ? v.as.num : fallback; }
// UTILS.isString: a non-empty string
int ksc_is_str(KsValue v);
// "#rgb" / "#rrggbb" (UTILS.isHexColor)
int ksc_is_hex(KsValue v);
int ksc_parse_hex(const char *s, vec4 *out); // 1 on a valid isHexColor string (alpha 1)
// ToString into buf (the script's own conversion)
const char *ksc_str(KsScript*, KsValue v, char *buf, size_t n);
// JS truthiness (!!x)
int ksc_truthy(KsValue v);
// parseFloat(x) || 0 (host setters)
double ksc_float(KsValue v);
// clamp (UTILS.limitMM)
static inline double ksc_clamp(const double x, const double lo, const double hi) { return x < lo ? lo : x > hi ? hi : x; }
// The value of key `name` of an option object (undefined when `obj` is not an object).
KsValue ksc_opt(KsScript*, KsValue obj, const char *name);
// Returns a fresh {x, y} / {width, height} object.
KsValue ksc_xy(KsScript*, const char *kx, double x, const char *ky, double y);

// ---- the client host (ks_client_host.c)
typedef struct {
    Client *client;
    KsHost host;
    KsApiImpl api[KS_API_COUNT];
    KsHostClass player_class;   // ks_player_props + the client-only members
    KsHostProp *player_props;
    const Map *map;             // the map whose client script is loaded (NULL none)
    int loaded;                 // a script instance exists for the current round
    uint16_t net_seq;           // NETWORK.send message counter
    KsNetReassembler reasm;     // server -> client SCRIPT_MSG
    uint64_t frame;             // ks_client_frame counter (click de-duplication)
    int server_minor;           // the INIT's protocol minor (NETWORK.send needs NET_PROTOCOL_MINOR_V7)
    double last_update_delta;   // update() delta (ms)
    int warned_lights;          // the unsupported-light warning was printed (once per process)
} KsClientState;

extern KsClientState g_ksc;

KsScript *ksc_script(void);      // the loaded client script (NULL none / failed)
Game *ksc_game(void);            // &client->game
void ksc_log(int is_error, const char *fmt, ...); // "[KRUNKSCRIPT] ..." on the console + stderr
// Calls a hook of the loaded script (no-op without one); args are converted by the caller.
int ksc_call(const char *hook, const KsValue *args, int argc);
int ksc_has_hook(const char *hook);
// One-time warning (krunkscript.md 7 #18): lights the KN renderer does not draw.
void ksc_warn_light(const char *what);

// ---- GAME.UI (ks_ui.c)
void ks_ui_api(KsApiImpl *api);
void ks_ui_round_reset(void);    // overlay.resetAll: #customGUIHolder emptied (every init)
void ks_ui_unload(void);         // every element the script created, any parent
void ks_ui_click(const char *id);// onDIVClicked(id) once per click (innermost element first)
int  ks_ui_point_on_script_element(float css_x, float css_y);
void ks_ui_frame(void);          // the fallback #customGUIHolder follows the in-game UI
void ks_ui_late(void);           // hideCrosshair / hideDefault, after the HUD pass
int  ks_ui_crosshair_hidden(void);

// ---- GAME.SCENE (ks_scene.c)
void ks_scene_api(KsApiImpl *api);
void ks_scene_unload(void);      // every script object leaves the scene and is freed
void ks_scene_frame(Client*, float delta_ms); // sprites face the camera, texture moves, models animate, lights
int  ks_scene_player_asset(KsScript*, const Player*, KsValue *out); // player.getAsset()
void ks_scene_player_play_anim(const Player*, const char *clip, int reps);
void ks_scene_player_stop_anim(const Player*);
// The camera as an attach target (CAMERA.getObj().id)
#define KSC_CAMERA_OBJECT_ID 1u
MeshTransform *ks_scene_camera_transform(void);

// ---- GAME.SOUND (ks_sound.c)
void ks_sound_api(KsApiImpl *api);
void ks_sound_unload(void);

// ---- GAME.INPUTS + GAME.CAMERA (ks_input.c)
void ks_input_api(KsApiImpl *api);
void ks_input_unload(Client*);
void ks_input_round_reset(Client*);
void ks_input_frame(Client*);
// v7 KeyboardEvent.key (lowercased) and keyCode of a GLFW key (krunkscript.md 4.3).
int  ks_input_key_name(int glfw_key, int scancode, int mods, char *name, size_t n);
int  ks_input_key_code(int glfw_key);
void ks_input_set_key(int code, int down); // controls.keys[code] (INPUTS.keyDown)
void ks_input_reset_keys(void);
int  ks_input_tmp_unlocked(void);
int  ks_input_force_free(void);
void ks_input_relock(Client*);   // a click on the game view after unlockMouse (CTL:585-589)
void ks_input_mouse_css(Client*, double *x, double *y); // last mouse position, window CSS px
void ks_input_cursor(Client*, double wx, double wy);
int  ks_input_camera_detached(void);
void ks_input_apply_camera(Client*);

// ---- COOKIES / SETTINGS / PLATFORM / MODS / ADS / setTitle / changeGame / URLS (ks_misc.c)
void ks_misc_api(KsApiImpl *api);
void ks_misc_unload(void);
