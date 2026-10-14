// GAME.INPUTS and GAME.CAMERA (doc/v7/krunkscript.md 3.1.8, 3.1.16; v7 createKrunkScriptApiContext.js
// :1581-1655, 2427-2580) and the input hooks (4.1, 4.3, 4.4): onKeyPress / onKeyUp /
// onKeyHeld, onMouseClick / onMouseUp / onMouseScroll and onController*. The hooks follow
// controls.js: keys only reach the script while the game owns the controls (Escape always,
// CTL:125), mouse buttons on the game view, the wheel always.
#include "ks_client_internal.h"
#include <ks_client.h>
#include <ks_overlay.h>
#include <camera_feel.h>
#include <html_dom.h>
#include <html_ui.h>
#include <ui_logic.h>
#include <stdio.h>
#include <string.h>
#ifdef __EMSCRIPTEN__
#include <emscripten/html5.h>
#endif

#define KSC_KEY_CODES 256
static struct {
    unsigned char keys[KSC_KEY_CODES]; // controls.keys[keyCode]
    unsigned char mouse[8];            // controls.keys[10000 + which]
    int tmp_unlock;                    // controls.tmpUnlock (unlockMouse)
    int force_free;                    // controls.forceFreeCam (freeMouse)
    int dont_send;                     // game.dontSendInputs (INPUTS.disableDefault)
    double mouse_x, mouse_y;           // RENDER.tmpMouse (window CSS px)
    double move_x, move_y;             // RENDER.tmpMouse2 (the last mousemove's movement)
    double last_wx, last_wy;           // previous cursor callback (window coordinates)
    int have_last;
    // GAME.CAMERA: the controls rig (object position / rotation, pchObjc pitch, camera rotation)
    int detached;                      // game.detachedCam
    vec3 rig_pos, rig_rot, inner_rot;
    float rig_pitch;
    int fov_set;                       // CAMERA.fov changed RENDER.camera.fov (restored on unload)
} g_in;

// ---------------------------------------------------------------------------
// Key names and codes (4.3)

int ks_input_key_code(const int glfw_key) {
    switch (glfw_key) {
        case GLFW_KEY_LEFT_SHIFT: case GLFW_KEY_RIGHT_SHIFT: return 16;
        case GLFW_KEY_LEFT_CONTROL: case GLFW_KEY_RIGHT_CONTROL: return 17;
        case GLFW_KEY_LEFT_ALT: case GLFW_KEY_RIGHT_ALT: return 18;
        case GLFW_KEY_LEFT_SUPER: case GLFW_KEY_RIGHT_SUPER: return 91;
        default: break;
    }
    const int code = ui_controls_dom_key(glfw_key);
    return code > 0 && code < KSC_KEY_CODES ? code : 0;
}

// KeyboardEvent.key, lowercased (US layout for shifted symbols; letters follow the keyboard
// layout through glfwGetKeyName natively).
int ks_input_key_name(const int glfw_key, const int scancode, const int mods, char *name, const size_t n) {
    static const struct { int key; const char *name; } NAMED[] = {
        {GLFW_KEY_SPACE, " "}, {GLFW_KEY_ENTER, "enter"}, {GLFW_KEY_KP_ENTER, "enter"}, {GLFW_KEY_ESCAPE, "escape"},
        {GLFW_KEY_TAB, "tab"}, {GLFW_KEY_BACKSPACE, "backspace"}, {GLFW_KEY_LEFT_SHIFT, "shift"},
        {GLFW_KEY_RIGHT_SHIFT, "shift"}, {GLFW_KEY_LEFT_CONTROL, "control"}, {GLFW_KEY_RIGHT_CONTROL, "control"},
        {GLFW_KEY_LEFT_ALT, "alt"}, {GLFW_KEY_RIGHT_ALT, "alt"}, {GLFW_KEY_LEFT_SUPER, "meta"},
        {GLFW_KEY_RIGHT_SUPER, "meta"}, {GLFW_KEY_LEFT, "arrowleft"}, {GLFW_KEY_UP, "arrowup"},
        {GLFW_KEY_RIGHT, "arrowright"}, {GLFW_KEY_DOWN, "arrowdown"}, {GLFW_KEY_DELETE, "delete"},
        {GLFW_KEY_INSERT, "insert"}, {GLFW_KEY_HOME, "home"}, {GLFW_KEY_END, "end"}, {GLFW_KEY_PAGE_UP, "pageup"},
        {GLFW_KEY_PAGE_DOWN, "pagedown"}, {GLFW_KEY_CAPS_LOCK, "capslock"}, {GLFW_KEY_NUM_LOCK, "numlock"},
        {GLFW_KEY_SCROLL_LOCK, "scrolllock"}, {GLFW_KEY_PAUSE, "pause"}, {GLFW_KEY_PRINT_SCREEN, "printscreen"},
        {GLFW_KEY_MENU, "contextmenu"}, {GLFW_KEY_KP_ADD, "+"}, {GLFW_KEY_KP_SUBTRACT, "-"},
        {GLFW_KEY_KP_MULTIPLY, "*"}, {GLFW_KEY_KP_DIVIDE, "/"}, {GLFW_KEY_KP_DECIMAL, "."},
    };
    if (!n) return 0;
    name[0] = 0;
    for (size_t i = 0; i < sizeof(NAMED) / sizeof(NAMED[0]); i++) {
        if (NAMED[i].key == glfw_key) {
            snprintf(name, n, "%s", NAMED[i].name);
            return 1;
        }
    }
    if (glfw_key >= GLFW_KEY_F1 && glfw_key <= GLFW_KEY_F24) {
        snprintf(name, n, "f%d", glfw_key - GLFW_KEY_F1 + 1);
        return 1;
    }
    if (glfw_key >= GLFW_KEY_KP_0 && glfw_key <= GLFW_KEY_KP_9) {
        snprintf(name, n, "%d", glfw_key - GLFW_KEY_KP_0);
        return 1;
    }
    const int shift = (mods & GLFW_MOD_SHIFT) != 0;
    // US layout: unshifted / shifted characters of the symbol and digit keys
    static const struct { int key; char plain, shifted; } SYMBOLS[] = {
        {GLFW_KEY_0, '0', ')'}, {GLFW_KEY_1, '1', '!'}, {GLFW_KEY_2, '2', '@'}, {GLFW_KEY_3, '3', '#'},
        {GLFW_KEY_4, '4', '$'}, {GLFW_KEY_5, '5', '%'}, {GLFW_KEY_6, '6', '^'}, {GLFW_KEY_7, '7', '&'},
        {GLFW_KEY_8, '8', '*'}, {GLFW_KEY_9, '9', '('}, {GLFW_KEY_MINUS, '-', '_'}, {GLFW_KEY_EQUAL, '=', '+'},
        {GLFW_KEY_LEFT_BRACKET, '[', '{'}, {GLFW_KEY_RIGHT_BRACKET, ']', '}'}, {GLFW_KEY_BACKSLASH, '\\', '|'},
        {GLFW_KEY_SEMICOLON, ';', ':'}, {GLFW_KEY_APOSTROPHE, '\'', '"'}, {GLFW_KEY_COMMA, ',', '<'},
        {GLFW_KEY_PERIOD, '.', '>'}, {GLFW_KEY_SLASH, '/', '?'}, {GLFW_KEY_GRAVE_ACCENT, '`', '~'},
    };
    for (size_t i = 0; i < sizeof(SYMBOLS) / sizeof(SYMBOLS[0]); i++) {
        if (SYMBOLS[i].key == glfw_key) {
            snprintf(name, n, "%c", shift ? SYMBOLS[i].shifted : SYMBOLS[i].plain);
            return 1;
        }
    }
    if (glfw_key >= GLFW_KEY_A && glfw_key <= GLFW_KEY_Z) {
        const char *layout = NULL;
#ifndef __EMSCRIPTEN__
        layout = scancode ? glfwGetKeyName(glfw_key, scancode) : NULL;
#else
        (void) scancode;
#endif
        char c = layout && layout[0] && !layout[1] ? layout[0] : (char) ('a' + (glfw_key - GLFW_KEY_A));
        if (c >= 'A' && c <= 'Z') c = (char) (c - 'A' + 'a');
        snprintf(name, n, "%c", c);
        return 1;
    }
    snprintf(name, n, "unidentified");
    return 0;
}

void ks_input_set_key(const int code, const int down) {
    if (code > 0 && code < KSC_KEY_CODES) g_in.keys[code] = (unsigned char) (down ? 1 : 0);
}

void ks_input_reset_keys(void) {
    memset(g_in.keys, 0, sizeof(g_in.keys));
    memset(g_in.mouse, 0, sizeof(g_in.mouse));
}

// ---------------------------------------------------------------------------
// Pointer state

int ks_input_tmp_unlocked(void) { return g_in.tmp_unlock; }
int ks_input_force_free(void) { return g_in.force_free; }
int ks_client_pointer_free(void) { return g_ksc.loaded && (g_in.tmp_unlock || g_in.force_free); }
int ks_client_input_unlocked(void) { return g_ksc.loaded && g_in.tmp_unlock; }
int ks_client_inputs_disabled(void) { return g_ksc.loaded && g_in.dont_send; }
int ks_client_hide_crosshair(void) { return g_ksc.loaded && ks_ui_crosshair_hidden(); }

// document.exitPointerLock() / requestPointerLock() without the menu's lock-loss flow: the
// pointer-lock owner (client_pointer_update, pointer_lock.h) reads tmp_unlock / force_free
// (ks_client_input_unlocked / ks_client_pointer_free), so after changing them it only re-runs.
static void pointer_lock_changed(Client *client) {
    if (client) client_pointer_update(client);
}

// lockMouse / a click on the game view: tmpUnlock off, controls.toggle(true)
void ks_input_relock(Client *client) {
    g_in.tmp_unlock = 0;
    if (!client) return;
    pointer_lock_changed(client); // locked again unless freeMouse keeps it free (controls on)
    client->menu_lock_grace_until = glfwGetTime() + 1.0;
}

// ---------------------------------------------------------------------------
// GAME.INPUTS

// keyDown(keyCode): !!controls.keys[keyCode]; non-number -> undefined
static KsValue in_key_down(KsScript *s, void *u, const KsValue *a, int n) {
    (void) s; (void) u;
    const KsValue v = ksc_arg(a, n, 0);
    if (!ksc_is_num(v)) return ks_undef();
    const double code = v.as.num;
    if (code >= 10000 && code < 10008) return ks_bool(g_in.mouse[(int) code - 10000]);
    if (code != (int) code || code <= 0 || code >= KSC_KEY_CODES) return ks_bool(0);
    return ks_bool(g_in.keys[(int) code]);
}

void ks_input_mouse_css(Client *client, double *x, double *y) {
    (void) client;
    *x = g_in.mouse_x;
    *y = g_in.mouse_y;
}

static KsValue in_mouse_pos(KsScript *s, void *u, const KsValue *a, int n) {
    (void) u; (void) a; (void) n;
    return ksc_xy(s, "x", g_in.mouse_x, "y", g_in.mouse_y);
}

static KsValue in_mouse_movement(KsScript *s, void *u, const KsValue *a, int n) {
    (void) u; (void) a; (void) n;
    return ksc_xy(s, "x", g_in.move_x, "y", g_in.move_y);
}

// mousePosOverlay(): canvas.width / scale * (mx / innerWidth): overlay units
static KsValue in_mouse_pos_overlay(KsScript *s, void *u, const KsValue *a, int n) {
    (void) u; (void) a; (void) n;
    float w = 0, h = 0, dpr = 1;
    html_dom_viewport(&w, &h, &dpr);
    const KsOvResult size = ks_overlay_call(KS_OV_GET_SIZE, NULL, 0);
    if (w <= 0 || h <= 0 || size.type != KS_OV_RET_SIZE) return ks_undef();
    return ksc_xy(s, "x", size.width * (g_in.mouse_x / w), "y", size.height * (g_in.mouse_y / h));
}

static KsValue in_free_mouse(KsScript *s, void *u, const KsValue *a, int n) {
    (void) s; (void) u; (void) a; (void) n;
    g_in.force_free = 1;
    pointer_lock_changed(g_ksc.client);
    return ks_undef();
}

// unlockMouse(): tmpUnlock, controls disabled, keys reset, pointer lock released
static KsValue in_unlock_mouse(KsScript *s, void *u, const KsValue *a, int n) {
    (void) s; (void) u; (void) a; (void) n;
    Client *client = g_ksc.client;
    if (!client) return ks_undef();
    g_in.tmp_unlock = 1;
    ui_controls_reset_keys();
    ks_input_reset_keys();
    pointer_lock_changed(client); // controls off, pointer free
    return ks_undef();
}

static KsValue in_lock_mouse(KsScript *s, void *u, const KsValue *a, int n) {
    (void) s; (void) u; (void) a; (void) n;
    Client *client = g_ksc.client;
    if (!client || html_ui_is_menu_open()) return ks_undef();
    ks_input_relock(client);
    return ks_undef();
}

static KsValue in_disable_default(KsScript *s, void *u, const KsValue *a, int n) {
    (void) s; (void) u; (void) a; (void) n;
    g_in.dont_send = 1;
    return ks_undef();
}

static KsValue in_enable_default(KsScript *s, void *u, const KsValue *a, int n) {
    (void) s; (void) u; (void) a; (void) n;
    g_in.dont_send = 0;
    return ks_undef();
}

// ---------------------------------------------------------------------------
// GAME.CAMERA

int ks_input_camera_detached(void) { return g_ksc.loaded && g_in.detached; }

static KsValue cam_detach(KsScript *s, void *u, const KsValue *a, int n) {
    (void) s; (void) u; (void) a; (void) n;
    if (!g_in.detached && g_ksc.client) {
        // the rig keeps where the camera was
        g_in.rig_pos = g_ksc.client->camera.position;
        g_in.rig_rot = (vec3) {0.0f, g_ksc.client->camera.rotation.y, 0.0f};
        g_in.rig_pitch = g_ksc.client->camera.rotation.x;
        g_in.inner_rot = (vec3) {0};
    }
    g_in.detached = 1;
    return ks_undef();
}

static KsValue cam_attach(KsScript *s, void *u, const KsValue *a, int n) {
    (void) s; (void) u; (void) a; (void) n;
    g_in.detached = 0;
    return ks_undef();
}

// move(x, y, z): controls.moveCam -> object.position.set
static KsValue cam_move(KsScript *s, void *u, const KsValue *a, int n) {
    (void) s; (void) u;
    g_in.rig_pos = (vec3) {(float) ksc_num_or(ksc_arg(a, n, 0), 0), (float) ksc_num_or(ksc_arg(a, n, 1), 0), (float) ksc_num_or(ksc_arg(a, n, 2), 0)};
    if (g_ksc.client && !g_in.detached) g_ksc.client->camera.position = g_in.rig_pos;
    return ks_undef();
}

// rotate(yaw, pitch, roll): RENDER.camera.rotation (y, x, z) inside the rig
static KsValue cam_rotate(KsScript *s, void *u, const KsValue *a, int n) {
    (void) s; (void) u;
    g_in.inner_rot = (vec3) {(float) ksc_num_or(ksc_arg(a, n, 1), 0), (float) ksc_num_or(ksc_arg(a, n, 0), 0), (float) ksc_num_or(ksc_arg(a, n, 2), 0)};
    return ks_undef();
}

// lookAt(x, y, z): rig yaw / pitch toward the point, camera rotation and offset reset
static KsValue cam_look_at(KsScript *s, void *u, const KsValue *a, int n) {
    (void) s; (void) u;
    if (!g_ksc.client) return ks_undef();
    const vec3 from = g_in.detached ? g_in.rig_pos : g_ksc.client->camera.position;
    const double dx = from.x - ksc_num_or(ksc_arg(a, n, 0), 0);
    const double dy = from.y - ksc_num_or(ksc_arg(a, n, 1), 0);
    const double dz = from.z - ksc_num_or(ksc_arg(a, n, 2), 0);
    const double flat = sqrt(dx * dx + dz * dz);
    double pitch = -atan2(dy, flat), yaw = -atan2(-dx, dz);
    if (pitch != pitch) pitch = 0;
    if (yaw != yaw) yaw = 0;
    g_in.rig_rot.y = (float) yaw;
    g_in.rig_pitch = (float) pitch;
    g_in.inner_rot = (vec3) {0};
    if (!g_in.detached) {
        g_ksc.client->camera.rotation.x = (float) pitch;
        g_ksc.client->camera.rotation.y = (float) yaw;
        if (g_ksc.client->me) g_ksc.client->me->direction = (vec2) {(float) pitch, (float) yaw};
    }
    return ks_undef();
}

static KsValue cam_shake(KsScript *s, void *u, const KsValue *a, int n) {
    (void) s; (void) u;
    camera_feel_shake((float) ksc_num_or(ksc_arg(a, n, 0), 0));
    return ks_undef();
}

// fov(deg): RENDER.camera.fov (the settings' fov comes back with the next script)
static KsValue cam_fov(KsScript *s, void *u, const KsValue *a, int n) {
    (void) s; (void) u;
    const double deg = ksc_num_or(ksc_arg(a, n, 0), 0);
    if (!g_ksc.client || deg <= 0.0 || deg >= 180.0) return ks_undef(); // a 0 fov would break the projection
    g_ksc.client->camera.fov = (float) (deg * M_PI / 180.0);
    g_in.fov_set = 1;
    return ks_undef();
}

// getObj(): {id} of the camera as an attach target
static KsValue cam_get_obj(KsScript *s, void *u, const KsValue *a, int n) {
    (void) u; (void) a; (void) n;
    const KsValue o = ks_obj_new(s);
    ks_obj_set(s, o, "id", ks_num(KSC_CAMERA_OBJECT_ID));
    return o;
}

static KsValue cam_env_zone(KsScript *s, void *u, const KsValue *a, int n) {
    (void) s; (void) u; (void) a; (void) n;
    return ks_num(-1); // RENDER.lastEnvId: environment zones are not tracked by the KN renderer yet
}

static vec3 *cam_pos_ref(void) {
    if (g_in.detached || !g_ksc.client) return &g_in.rig_pos;
    return &g_ksc.client->camera.position;
}

static vec3 cam_rot_value(void) {
    if (g_in.detached || !g_ksc.client) return g_in.rig_rot;
    return (vec3) {0.0f, g_ksc.client->camera.rotation.y, 0.0f}; // object.rotation: the yaw object
}

#define CAM_AXIS(name, field)                                                                         \
    static KsValue cam_pos_##name(KsScript *s, void *u, const KsValue *a, int n) {                    \
        (void) s; (void) u; (void) a; (void) n;                                                       \
        return ks_num(cam_pos_ref()->field);                                                          \
    }                                                                                                 \
    static KsValue cam_pos_set_##name(KsScript *s, void *u, const KsValue *a, int n) {                \
        (void) s; (void) u;                                                                           \
        const float v = (float) ksc_float(ksc_arg(a, n, 0));                                          \
        g_in.rig_pos.field = v;                                                                       \
        cam_pos_ref()->field = v;                                                                     \
        return ks_undef();                                                                            \
    }                                                                                                 \
    static KsValue cam_rot_##name(KsScript *s, void *u, const KsValue *a, int n) {                    \
        (void) s; (void) u; (void) a; (void) n;                                                       \
        return ks_num(cam_rot_value().field);                                                         \
    }                                                                                                 \
    static KsValue cam_rot_set_##name(KsScript *s, void *u, const KsValue *a, int n) {                \
        (void) s; (void) u;                                                                           \
        g_in.rig_rot.field = (float) ksc_float(ksc_arg(a, n, 0));                                     \
        return ks_undef();                                                                            \
    }
CAM_AXIS(x, x)
CAM_AXIS(y, y)
CAM_AXIS(z, z)

void ks_input_api(KsApiImpl *api) {
    api[KS_API_INPUTS_KEYDOWN].fn = in_key_down;
    api[KS_API_INPUTS_MOUSEPOS].fn = in_mouse_pos;
    api[KS_API_INPUTS_MOUSEPOSOVERLAY].fn = in_mouse_pos_overlay;
    api[KS_API_INPUTS_MOUSEMOVEMENT].fn = in_mouse_movement;
    api[KS_API_INPUTS_FREEMOUSE].fn = in_free_mouse;
    api[KS_API_INPUTS_UNLOCKMOUSE].fn = in_unlock_mouse;
    api[KS_API_INPUTS_LOCKMOUSE].fn = in_lock_mouse;
    api[KS_API_INPUTS_DISABLEDEFAULT].fn = in_disable_default;
    api[KS_API_INPUTS_ENABLEDEFAULT].fn = in_enable_default;

    api[KS_API_CAMERA_ENVZONE].fn = cam_env_zone;
    api[KS_API_CAMERA_DETACH].fn = cam_detach;
    api[KS_API_CAMERA_ATTACH].fn = cam_attach;
    api[KS_API_CAMERA_MOVE].fn = cam_move;
    api[KS_API_CAMERA_ROTATE].fn = cam_rotate;
    api[KS_API_CAMERA_LOOKAT].fn = cam_look_at;
    api[KS_API_CAMERA_SHAKE].fn = cam_shake;
    api[KS_API_CAMERA_FOV].fn = cam_fov;
    api[KS_API_CAMERA_GETOBJ].fn = cam_get_obj;
    api[KS_API_CAMERA_POSITION_X] = (KsApiImpl) {cam_pos_x, cam_pos_set_x};
    api[KS_API_CAMERA_POSITION_Y] = (KsApiImpl) {cam_pos_y, cam_pos_set_y};
    api[KS_API_CAMERA_POSITION_Z] = (KsApiImpl) {cam_pos_z, cam_pos_set_z};
    api[KS_API_CAMERA_ROTATION_X] = (KsApiImpl) {cam_rot_x, cam_rot_set_x};
    api[KS_API_CAMERA_ROTATION_Y] = (KsApiImpl) {cam_rot_y, cam_rot_set_y};
    api[KS_API_CAMERA_ROTATION_Z] = (KsApiImpl) {cam_rot_z, cam_rot_set_z};
}

// The detached rig replaces the camera pose the engine computed (index.js skips moveCam /
// rotateCam while Game.detachedCam). Euler composition as the renderer's camera: yaw, then
// pitch, then the inner camera rotation.
void ks_input_apply_camera(Client *client) {
    if (!g_in.detached) return;
    client->camera.position = g_in.rig_pos;
    client->camera.rotation.x = g_in.rig_rot.x + g_in.rig_pitch + g_in.inner_rot.x;
    client->camera.rotation.y = g_in.rig_rot.y + g_in.inner_rot.y;
    client->camera.rotation.z = g_in.rig_rot.z + g_in.inner_rot.z;
}

// ---------------------------------------------------------------------------
// Hooks

static int controls_enabled(Client *client) {
    return client && client->game.ready && ui_controls_enabled(client) && !ks_client_input_unlocked();
}

static void call_key_hook(const char *hook, const char *name, const int code) {
    KsScript *s = ksc_script();
    if (!s || !ksc_has_hook(hook)) return;
    const KsValue args[2] = {ks_str(s, name, strlen(name)), ks_num(code)};
    ksc_call(hook, args, 2);
}

void ks_client_key(Client *client, const int key, const int scancode, const int action, const int mods) {
    if (!g_ksc.loaded || !client) return;
    if (ui_text_input_focused() || ui_controls_capturing()) return; // typing: the keys belong to the field
    const int code = ks_input_key_code(key);
    if (!code) return;
    // controls.js f2133: nothing while the controls are off, Escape aside
    if (!controls_enabled(client) && code != 27) return;
    char name[32];
    ks_input_key_name(key, scancode, mods, name, sizeof(name));
    if (action == GLFW_PRESS) {
        if (g_in.keys[code]) {
            call_key_hook("onKeyHeld", name, code);
            return;
        }
        g_in.keys[code] = 1;
        call_key_hook("onKeyPress", name, code);
    } else if (action == GLFW_REPEAT) {
        g_in.keys[code] = 1;
        call_key_hook("onKeyHeld", name, code);
    } else if (action == GLFW_RELEASE) {
        if (!g_in.keys[code]) return; // releaseButton runs only on a state change
        g_in.keys[code] = 0;
        call_key_hook("onKeyUp", name, code);
    }
}

static int mouse_which(const int button) {
    switch (button) {
        case GLFW_MOUSE_BUTTON_LEFT: return 1;
        case GLFW_MOUSE_BUTTON_MIDDLE: return 2;
        case GLFW_MOUSE_BUTTON_RIGHT: return 3;
        default: return button + 1;
    }
}

static void call_mouse_hook(const char *hook, const int which) {
    KsScript *s = ksc_script();
    if (!s || !ksc_has_hook(hook)) return;
    const KsValue args[3] = {ks_num(which), ks_num(g_in.mouse_x), ks_num(g_in.mouse_y)};
    ksc_call(hook, args, 3);
}

int ks_client_mouse_button(Client *client, const int button, const int action) {
    if (!g_ksc.loaded || !client || !client->game.ready) return 0;
    if (html_ui_is_menu_open() || hud_ui_end_screen_interactive()) return 0;
    const int which = mouse_which(button);
    if (ks_client_pointer_free()) {
        // the page takes clicks on script elements (onclick -> onDIVClicked)
        if (ks_ui_point_on_script_element((float) g_in.mouse_x, (float) g_in.mouse_y)) return 0;
        // the game view: CTL:585-603 re-lock after unlockMouse, onMouseClick, the button as a key
        if (action == GLFW_PRESS) {
            if (g_in.tmp_unlock) ks_input_relock(client);
            if (which < 8) g_in.mouse[which] = 1;
            call_mouse_hook("onMouseClick", which);
        } else if (action == GLFW_RELEASE) {
            if (which < 8) g_in.mouse[which] = 0;
            call_mouse_hook("onMouseUp", which);
        }
        ui_controls_mouse_event(client, button, action, 1);
        return 1;
    }
    if (!controls_enabled(client)) return 0;
    if (action == GLFW_PRESS) {
        if (which < 8) g_in.mouse[which] = 1;
        call_mouse_hook("onMouseClick", which);
    } else if (action == GLFW_RELEASE) {
        if (which < 8) g_in.mouse[which] = 0;
        call_mouse_hook("onMouseUp", which);
    }
    return 0;
}

// Window coordinates -> window CSS px (RENDER.tmpMouse is clientX / clientY).
static void window_to_css(Client *client, const double wx, const double wy, double *x, double *y) {
    *x = wx;
    *y = wy;
    if (!client || !client->window) return;
    int ww = 0, wh = 0;
    glfwGetWindowSize(client->window, &ww, &wh);
    float vw = 0, vh = 0, dpr = 1;
    html_dom_viewport(&vw, &vh, &dpr);
    if (ww > 0 && wh > 0 && vw > 0 && vh > 0) {
        *x = wx * vw / ww;
        *y = wy * vh / wh;
    }
}

void ks_input_cursor(Client *client, const double wx, const double wy) {
    if (g_in.have_last) {
        g_in.move_x = wx - g_in.last_wx;
        g_in.move_y = wy - g_in.last_wy;
    }
    g_in.last_wx = wx;
    g_in.last_wy = wy;
    g_in.have_last = 1;
    // a locked pointer keeps clientX / clientY where the lock started
    const int locked = client && client->mouse_state.locked && !ks_client_pointer_free() && !html_ui_is_menu_open();
    if (!locked) window_to_css(client, wx, wy, &g_in.mouse_x, &g_in.mouse_y);
}

void ks_client_cursor(Client *client, const double x, const double y) {
    if (!g_ksc.loaded) return;
    ks_input_cursor(client, x, y);
}

// The wheel over the game view: onMouseScroll(-1 up / 1 down) (CTL:662-706)
void ks_client_scroll(Client *client, const double dy) {
    if (!g_ksc.loaded || !client || !client->game.ready || dy == 0.0) return;
    if (html_ui_is_menu_open()) return;
    KsScript *s = ksc_script();
    if (!s || !ksc_has_hook("onMouseScroll")) return;
    const KsValue arg = ks_num(dy > 0.0 ? -1 : 1);
    ksc_call("onMouseScroll", &arg, 1);
}

void ks_client_controller(const int type, const char *button, const int code) {
    if (!g_ksc.loaded || !button) return;
    static const char *const HOOKS[] = {"onControllerPress", "onControllerHeld", "onControllerUp"};
    if (type < 0 || type > 2) return;
    KsScript *s = ksc_script();
    if (!s || !ksc_has_hook(HOOKS[type])) return;
    const KsValue args[2] = {ks_str(s, button, strlen(button)), ks_num(code)};
    ksc_call(HOOKS[type], args, 2);
}

// ---------------------------------------------------------------------------
// Frame / lifecycle

void ks_input_frame(Client *client) {
    if (!client) return;
    // The menu took the pointer (Escape): a temporary unlock ends with it.
    if (html_ui_is_menu_open()) {
        if (g_in.tmp_unlock) g_in.tmp_unlock = 0;
        ks_input_reset_keys();
        return;
    }
    // freeMouse lasts for the script's life: natively the pointer-lock owner keeps the cursor free;
    // on the web the menu's Play requests the lock itself (a gesture), so release it again.
#ifdef __EMSCRIPTEN__
    if (g_in.force_free || g_in.tmp_unlock) {
        EmscriptenPointerlockChangeEvent st;
        if (emscripten_get_pointerlock_status(&st) == EMSCRIPTEN_RESULT_SUCCESS && st.isActive) emscripten_exit_pointerlock();
    }
#endif
}

void ks_input_round_reset(Client *client) {
    (void) client;
    ks_input_reset_keys();
}

void ks_input_unload(Client *client) {
    const int was_free = g_in.tmp_unlock || g_in.force_free;
    const int fov_set = g_in.fov_set;
    memset(&g_in, 0, sizeof(g_in));
    if (!client) return;
    if (was_free && !html_ui_is_menu_open() && client->in_game) pointer_lock_changed(client); // locked again
    if (fov_set) {
        const float fov = menu_ui()->settings.fov;
        if (fov >= 10.0f && fov <= 179.0f) client->camera.fov = fov * (float) M_PI / 180.0f;
    }
}
