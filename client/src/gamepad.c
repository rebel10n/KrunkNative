// Game controllers: Krunker's gamepad module and the controller half of CONTROLS, ported from
// v3.1.1 and brought to v10.0.2 (~/.cache/kn-v10/v10.js, the Gamepad class and CONTROLS'
// `if (!this.isMobile)` block). v10 kept v3.1.1's loop, threshold, event machine and rumble;
// what it changed, and this file follows:
//   - _keyMapping.gamepad names button 16 "vendor" (no CONTROLS mapping: KrunkScript only);
//   - CONTROLS.mapping.shoulder_bottom_right is {THRES: true, KEY: "shoot"} (no MOUSE): the
//     trigger's press / release press / release the shoot bind; while held, at or past
//     triggerThres it is a which-0 mousedown (takes the pointer back into the game), below it
//     the mouseup (ui_controls_fake_mouse);
//   - the left stick: each axis through a rescaling dead zone (|v| < deadZoneL ? 0 :
//     (v - sign(v) * deadZoneL) / (1 - deadZoneL)), then one of 8 directions (45-degree
//     sectors of atan2(x, y)) holds the move binds;
//   - KrunkScript onControllerPress / Held / Up fire for every named button and both sticks;
//   - Settings > Controller "Disable Controllers" (disableCons: toggleGamepad) and the
//     vibration strength vibrationMlt.
//
// Sources (/home/user/jails/kru/src/Krunker_v3.1.1/src/libs, = Krunker.Unpacked-v3.1.1):
//   gamepad.js (133.js)   the polling loop: requestAnimationFrame + navigator.getGamepads()[0],
//                         the button names (_keyMapping), the 0.3 stick threshold, the
//                         press/hold/release event machine, rumble(), the
//                         gamepadconnected/gamepaddisconnected handling;
//   controls.js (132.js)  CONTROLS.mapping (button -> bind), the stick handlers, start ->
//                         toggle, applyDeadzone, fakeMouse (ui_controls.c), cntrlInput;
//   95.js shoot()/melee() rumble(weapon.rumbleDur || 200, weapon.rumble || 0.5) while
//                         cntrlInput.vibration && gamepad.active, else navigator.vibrate.
//
// Platform: the browser hands v3.1.1 the "standard" Gamepad layout (buttons 0-3 A B X Y,
// 4/5 bumpers, 6/7 triggers as analog buttons, 8 back, 9 start, 10/11 stick clicks,
// 12-15 d-pad, 16 guide; axes 0-3 = left x/y, right x/y, +y down). On the web this file
// reads exactly that through emscripten/html5.h (navigator.getGamepads()). Natively GLFW's
// gamepad API (SDL_GameControllerDB mappings) is reshaped into the same layout; a
// trigger becomes value = (axis + 1) / 2, pressed = value > 30/255 like Chromium's
// AxisToButton. Only joysticks with a gamepad mapping count as gamepads: GLFW also opens
// devices the browser never lists (absolute pointers, tablets: anything with keys and
// axes), so unmapped devices are ignored (SDL_GAMECONTROLLERCONFIG can map them).
// navigator.getGamepads()[0] is the lowest GLFW slot holding a gamepad.
//
// KrunkNative notes:
//   - Native has no rumble: GLFW exposes no force feedback. The rumble call is logged
//     once and otherwise dropped; the web build plays it through vibrationActuator.
//   - Native pads that are already plugged in count as connected at start-up (the
//     browser fires gamepadconnected on the pad's first input after the page loads).
//   - requestAnimationFrame cadence: the web build runs the loop once per animation frame
//     (every main-loop callback, frame-capped or not). Natively the loop runs at the
//     display refresh rate (several times per frame when frames are slower), so the
//     right stick, which adds its deflection once per loop, turns at v3.1.1's speed
//     whatever the native frame rate.
//   - applyDeadzone with deadZoneR = 1 divides by zero (NaN rotation in v3.1.1); here it
//     yields 0.
//   - gradSpeed sets speedLmt; the input frame sends 1 - speedLmt.round(1) as
//     Input.slowdown (input[12]) and CONTROLS.reset() (each spawn) puts it back to 1.
//   - KRUNKNATIVE_GAMEPAD_LOG=1 prints every press/release (and hold) event.
//   - SDL_GAMECONTROLLERCONFIG (SDL's variable: mapping lines) and
//     KRUNKNATIVE_GAMEPAD_DB (a gamecontrollerdb.txt path) add/override native mappings.
#include <gamepad.h>
#include <ks_client.h> // KrunkScript controller hooks
#include <ui_logic.h>
#include <html_ui.h>
#include <GLFW/glfw3.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/html5.h>
#endif

#define GP_MAX_BUTTONS 64
#define GP_MAX_AXES 64
#define GP_THRESHOLD 0.3                    // Gamepad._threshold
#define GP_TRIGGER_PRESSED (30.0 / 255.0)   // Chromium kDefaultButtonPressedThreshold
#define GP_MAX_LOOPS 8                       // native: animation frames caught up per main-loop pass

// One navigator.getGamepads()[i] sample.
typedef struct {
    int connected;
    int standard;          // mapping == "standard"
    int button_count, axis_count;
    unsigned char pressed[GP_MAX_BUTTONS];
    double value[GP_MAX_BUTTONS];
    double axes[GP_MAX_AXES];
} GpSample;

// _keyMapping.gamepad (button names = their standard index) + _keyMapping.axes.
enum {
    GP_BUTTON_1, GP_BUTTON_2, GP_BUTTON_3, GP_BUTTON_4,
    GP_SHOULDER_TOP_LEFT, GP_SHOULDER_TOP_RIGHT, GP_SHOULDER_BOTTOM_LEFT, GP_SHOULDER_BOTTOM_RIGHT,
    GP_SELECT, GP_START, GP_STICK_BUTTON_LEFT, GP_STICK_BUTTON_RIGHT,
    GP_D_PAD_UP, GP_D_PAD_DOWN, GP_D_PAD_LEFT, GP_D_PAD_RIGHT,
    GP_VENDOR, // v10
    GP_BUTTON_NAMES,
    GP_STICK_AXIS_LEFT = GP_BUTTON_NAMES, GP_STICK_AXIS_RIGHT,
    GP_NAMES
};

static const char *const GP_NAME[GP_NAMES] = {
    "button_1", "button_2", "button_3", "button_4",
    "shoulder_top_left", "shoulder_top_right", "shoulder_bottom_left", "shoulder_bottom_right",
    "select", "start", "stick_button_left", "stick_button_right",
    "d_pad_up", "d_pad_down", "d_pad_left", "d_pad_right", "vendor",
    "stick_axis_left", "stick_axis_right",
};

// CONTROLS.mapping (v10): KEY = the bind pressed through Q()/U(); the right trigger's entry
// also has THRES (its hold is a fake mouse button at triggerThres, GP_THRES below).
// -1 = no entry (start has its own press listener; the stick buttons and vendor do nothing).
static const int GP_MAPPING[GP_BUTTON_NAMES] = {
    [GP_BUTTON_1] = UI_CONTROL_JUMP,
    [GP_BUTTON_2] = UI_CONTROL_CROUCH,
    [GP_BUTTON_3] = UI_CONTROL_RELOAD,
    [GP_BUTTON_4] = UI_CONTROL_SWAP,
    [GP_SHOULDER_TOP_LEFT] = UI_CONTROL_SWAP,
    [GP_SHOULDER_TOP_RIGHT] = UI_CONTROL_MELEE,
    [GP_SHOULDER_BOTTOM_LEFT] = UI_CONTROL_AIM,
    [GP_SHOULDER_BOTTOM_RIGHT] = UI_CONTROL_SHOOT, // {THRES: true, KEY: "shoot"}
    [GP_SELECT] = UI_CONTROL_PLAYER_LIST,
    [GP_START] = -1,
    [GP_STICK_BUTTON_LEFT] = -1,
    [GP_STICK_BUTTON_RIGHT] = -1,
    [GP_D_PAD_UP] = UI_CONTROL_VOICE,
    [GP_D_PAD_DOWN] = UI_CONTROL_SPRAY,
    [GP_D_PAD_LEFT] = UI_CONTROL_INTERACT,
    [GP_D_PAD_RIGHT] = UI_CONTROL_DROP,
    [GP_VENDOR] = -1,
};
#define GP_THRES GP_SHOULDER_BOTTOM_RIGHT // the one entry with THRES

// One _events.gamepad[name] / _events.axes[name] entry.
typedef struct {
    int name;
    unsigned char pressed, hold, released;
    double value;          // button value
    double axis[2];        // stick value
} GpEvent;

// An _events.* object: entries in insertion order (Object.keys order).
typedef struct {
    GpEvent ev[GP_NAMES];
    int count;
} GpEvents;

enum { GP_PRESS, GP_HOLD, GP_RELEASE };
static const char *const GP_TYPE[] = {"press", "hold", "release"};

static struct {
    Client *client;
    int connected;         // _connected
    int active;            // active
    int disabled;          // disabled (v10 toggleGamepad: Settings > Controller "Disable Controllers")
    int running;           // _requestAnimation armed
    GpEvents buttons, axes;
    int log;
    double next_frame;     // native: time of the next animation frame
    double frame_period;
    // own-shot detection for the 95.js rumble (fire cooldown jumps, as sound_events.c)
    const Player *shot_player;
    const Weapon *shot_weapon;
    int shot_index;
    float shot_cooldown;
    int me_active;         // spawn edge for CONTROLS.reset()'s speedLmt = 1
} g_gp;

// ---------------------------------------------------------------- platform

#ifdef __EMSCRIPTEN__
static void sample_from_html5(GpSample *out, const EmscriptenGamepadEvent *e) {
    memset(out, 0, sizeof(*out));
    out->connected = e->connected;
    out->standard = !strcmp(e->mapping, "standard");
    out->button_count = e->numButtons < GP_MAX_BUTTONS ? e->numButtons : GP_MAX_BUTTONS;
    out->axis_count = e->numAxes < GP_MAX_AXES ? e->numAxes : GP_MAX_AXES;
    for (int i = 0; i < out->button_count; i++) {
        out->pressed[i] = e->digitalButton[i];
        out->value[i] = e->analogButton[i];
    }
    for (int i = 0; i < out->axis_count; i++) out->axes[i] = e->axis[i];
}

// navigator.getGamepads()[index]
static int sample_pad(const int index, GpSample *out) {
    if (emscripten_sample_gamepad_data() != EMSCRIPTEN_RESULT_SUCCESS) return 0;
    if (index >= emscripten_get_num_gamepads()) return 0;
    EmscriptenGamepadEvent e;
    if (emscripten_get_gamepad_status(index, &e) != EMSCRIPTEN_RESULT_SUCCESS) return 0;
    sample_from_html5(out, &e);
    return 1;
}
#else
static void set_button(GpSample *s, const int index, const int pressed, const double value) {
    s->pressed[index] = (unsigned char) !!pressed;
    s->value[index] = value;
}

// Chromium AxisToButton: a [-1, 1] trigger axis as an analog button.
static void set_trigger(GpSample *s, const int index, const float axis) {
    const double value = (axis + 1.0) / 2.0;
    set_button(s, index, value > GP_TRIGGER_PRESSED, value);
}

// The GLFW joystick slots that hold gamepads (bit per jid), in connection order of slots.
static unsigned g_pad_slots;

static int pad_jid(const int index) {
    int n = 0;
    for (int jid = GLFW_JOYSTICK_1; jid <= GLFW_JOYSTICK_LAST; jid++) {
        if (!(g_pad_slots & (1u << jid))) continue;
        if (n++ == index) return jid;
    }
    return -1;
}

// One GLFW gamepad as a standard-mapping Gamepad sample.
static int sample_jid(const int jid, GpSample *out) {
    memset(out, 0, sizeof(*out));
    if (jid < GLFW_JOYSTICK_1 || jid > GLFW_JOYSTICK_LAST || !glfwJoystickPresent(jid)) return 0;
    out->connected = 1;
    GLFWgamepadstate st;
    if (glfwJoystickIsGamepad(jid) && glfwGetGamepadState(jid, &st)) {
        // The standard Gamepad layout.
        static const int BUTTONS[] = {
            GLFW_GAMEPAD_BUTTON_A, GLFW_GAMEPAD_BUTTON_B, GLFW_GAMEPAD_BUTTON_X, GLFW_GAMEPAD_BUTTON_Y,
            GLFW_GAMEPAD_BUTTON_LEFT_BUMPER, GLFW_GAMEPAD_BUTTON_RIGHT_BUMPER, -1, -1,
            GLFW_GAMEPAD_BUTTON_BACK, GLFW_GAMEPAD_BUTTON_START,
            GLFW_GAMEPAD_BUTTON_LEFT_THUMB, GLFW_GAMEPAD_BUTTON_RIGHT_THUMB,
            GLFW_GAMEPAD_BUTTON_DPAD_UP, GLFW_GAMEPAD_BUTTON_DPAD_DOWN,
            GLFW_GAMEPAD_BUTTON_DPAD_LEFT, GLFW_GAMEPAD_BUTTON_DPAD_RIGHT,
            GLFW_GAMEPAD_BUTTON_GUIDE,
        };
        out->standard = 1;
        out->button_count = (int) (sizeof(BUTTONS) / sizeof(BUTTONS[0]));
        for (int i = 0; i < out->button_count; i++) {
            if (BUTTONS[i] >= 0) set_button(out, i, st.buttons[BUTTONS[i]] == GLFW_PRESS, st.buttons[BUTTONS[i]] == GLFW_PRESS ? 1.0 : 0.0);
        }
        set_trigger(out, 6, st.axes[GLFW_GAMEPAD_AXIS_LEFT_TRIGGER]);
        set_trigger(out, 7, st.axes[GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER]);
        out->axis_count = 4;
        out->axes[0] = st.axes[GLFW_GAMEPAD_AXIS_LEFT_X];
        out->axes[1] = st.axes[GLFW_GAMEPAD_AXIS_LEFT_Y];
        out->axes[2] = st.axes[GLFW_GAMEPAD_AXIS_RIGHT_X];
        out->axes[3] = st.axes[GLFW_GAMEPAD_AXIS_RIGHT_Y];
        return 1;
    }
    return 0;
}

// navigator.getGamepads()[index]
static int sample_pad(const int index, GpSample *out) {
    return sample_jid(pad_jid(index), out);
}
#endif

// ---------------------------------------------------------------- _events

static GpEvent *events_find(GpEvents *list, const int name) {
    for (int i = 0; i < list->count; i++) {
        if (list->ev[i].name == name) return &list->ev[i];
    }
    return NULL;
}

static GpEvent *events_add(GpEvents *list, const int name) {
    if (list->count >= GP_NAMES) return NULL;
    GpEvent *e = &list->ev[list->count++];
    memset(e, 0, sizeof(*e));
    e->name = name;
    return e;
}

static void events_remove(GpEvents *list, const int index) {
    if (index < 0 || index >= list->count) return;
    memmove(&list->ev[index], &list->ev[index + 1], (size_t) (list->count - index - 1) * sizeof(GpEvent));
    list->count--;
}

// _handleGamepadEventListener(gamepad): every pressed button with a name gets an event
// (pressed until handled), its value refreshed; a released one is marked released.
static void listen_buttons(const GpSample *pad) {
    if (!pad || !pad->connected) return;
    for (int k = 0; k < pad->button_count && k < GP_BUTTON_NAMES; k++) {
        GpEvent *e = events_find(&g_gp.buttons, k);
        if (pad->pressed[k]) {
            if (!e) {
                e = events_add(&g_gp.buttons, k);
                if (!e) continue;
                e->pressed = 1;
            }
            e->value = pad->value[k];
        } else if (e) {
            e->released = 1;
            e->hold = 0;
        }
    }
}

// _handleGamepadAxisEventListener(gamepad): a stick past the threshold on either axis
// is pressed (first loop) then held; back inside it is released.
static void listen_axes(const GpSample *pad) {
    if (!pad || !pad->connected) return;
    for (int j = 0; j < 2; j++) {
        const int name = GP_STICK_AXIS_LEFT + j;
        const int first = j * 2; // stick_axis_left: [0, 2), stick_axis_right: [2, 4)
        const double x = first < pad->axis_count ? pad->axes[first] : 0.0;
        const double y = first + 1 < pad->axis_count ? pad->axes[first + 1] : 0.0;
        GpEvent *e = events_find(&g_gp.axes, name);
        if (fabs(x) > GP_THRESHOLD || fabs(y) > GP_THRESHOLD) {
            const int existed = e != NULL;
            if (!e) e = events_add(&g_gp.axes, name);
            if (!e) continue;
            e->pressed = (unsigned char) !existed;
            e->hold = (unsigned char) existed;
            e->released = 0;
            e->axis[0] = x;
            e->axis[1] = y;
        } else if (e) {
            e->pressed = 0;
            e->hold = 0;
            e->released = 1;
            e->axis[0] = x;
            e->axis[1] = y;
        }
    }
}

// ---------------------------------------------------------------- CONTROLS listeners

// CONTROLS.applyDeadzone(value, deadZone)
static double apply_deadzone(const double value, const double dead_zone) {
    double v = (fabs(value) - dead_zone) / (1.0 - dead_zone);
    if (v < 0.0 || isnan(v)) v = 0.0; // KrunkNative: deadZone 1 gives 0, not NaN
    return v * (value > 0.0 ? 1.0 : -1.0);
}

static void key(const int id, const int down) {
    ui_controls_controller_key(g_gp.client, id, down);
}

// gamepad.on(["press", "hold", "release"], Object.keys(mapping), ...) (v10): press / release
// the KEY's bind; the THRES entry's hold is fakeMouse down at triggerThres, up below it.
static void on_mapped_button(const int type, const int name, const double value) {
    const int map = GP_MAPPING[name];
    if (map < 0) return;
    if (type == GP_PRESS) key(map, 1);
    else if (type == GP_RELEASE) key(map, 0);
    else if (type == GP_HOLD && name == GP_THRES) {
        ui_controls_fake_mouse(g_gp.client, value >= ui_controls_cntrl_input()->trigger_thres ? 0 : 1, 0.0f, 0.0f);
    }
}

// v10's left-stick dead zone: inside it 0, outside rescaled to start at 0.
static double stick_deadzone(double v, const double dz) {
    if (fabs(v) < dz) return 0.0;
    v -= (v > 0.0 ? 1.0 : v < 0.0 ? -1.0 : 0.0) * dz;
    v /= 1.0 - dz;
    return isfinite(v) ? v : 0.0; // KrunkNative: deadZoneL 1 (only reached at |v| = 1) gives 0, not Infinity
}

// The 8-way left-stick sector (v10): atan2(x, y) in degrees [0, 360), +22.5, / 45, floored.
// 0 / 8 = down (+y), 2 = right, 4 = up, 6 = left.
static int stick_sector(const double x, const double y) {
    double deg = atan2(x, y);
    if (deg < 0.0) deg += 2.0 * M_PI;
    deg = 180.0 * deg / M_PI;
    return (int) floor((deg + 22.5) / 45.0);
}

// gamepad.on("hold", "stick_axis_left") (v10): the move binds of the stick's sector go down,
// the others up; inside the dead zone all four are up.
static void on_left_stick_hold(const double raw_x, const double raw_y) {
    const UiControllerInput *c = ui_controls_cntrl_input();
    const double dz = c->dead_zone_l;
    if (c->grad_speed) ui_controls_set_speed_limit((float) (fmax(fabs(raw_x), fabs(raw_y)) / (1.0 - dz)));
    const double x = stick_deadzone(raw_x, dz), y = stick_deadzone(raw_y, dz);
    // forward, backward, left, right held per sector (binds.move[0..3])
    static const unsigned char HELD[9][4] = {
        {0, 1, 0, 0}, {0, 1, 0, 1}, {0, 0, 0, 1}, {1, 0, 0, 1}, {1, 0, 0, 0},
        {1, 0, 1, 0}, {0, 0, 1, 0}, {0, 1, 1, 0}, {0, 1, 0, 0},
    };
    static const int MOVE[4] = {UI_CONTROL_FORWARD, UI_CONTROL_BACKWARD, UI_CONTROL_LEFT, UI_CONTROL_RIGHT};
    if (x == 0.0 && y == 0.0) {
        for (int i = 0; i < 4; i++) key(MOVE[i], 0);
        return;
    }
    int sector = stick_sector(x, y);
    if (sector < 0 || sector > 8) sector = 0;
    for (int i = 0; i < 4; i++) {
        if (!HELD[sector][i]) key(MOVE[i], 0); // v10 releases first, then presses
    }
    for (int i = 0; i < 4; i++) {
        if (HELD[sector][i]) key(MOVE[i], 1);
    }
}

// Gamepad.trigger(type, button, value): the listeners CONTROLS registered.
static void trigger(const int type, const GpEvent *e) {
    if (g_gp.log && (type != GP_HOLD || g_gp.log > 1)) {
        if (e->name >= GP_BUTTON_NAMES) printf("[gamepad] %s %s [%.3f, %.3f]\n", GP_TYPE[type], GP_NAME[e->name], e->axis[0], e->axis[1]);
        else printf("[gamepad] %s %s %.3f\n", GP_TYPE[type], GP_NAME[e->name], e->value);
    }
    // KrunkNative: no pad press / hold acts while a text field has focus (ui_text_input_focused,
    // the keyboard hotkeys' rule; v3.1.1 lets the pad through). Releases still land.
    if (type != GP_RELEASE && ui_text_input_focused()) return;
    if (e->name < GP_BUTTON_NAMES) on_mapped_button(type, e->name, e->value);
    // KrunkScript onControllerPress / Held / Up (CTL:1137-1141), after the mapping, for every
    // button and stick name (v10 registers the listener on all of them): the name and the key
    // code of the bind it presses (0 for the unmapped buttons and the sticks)
    const int map = e->name < GP_BUTTON_NAMES ? GP_MAPPING[e->name] : -1;
    const int bound = map >= 0 ? ui_controls_bind(map) : 0;
    ks_client_controller(type, GP_NAME[e->name], bound > 0 ? bound : 0);
    if (e->name < GP_BUTTON_NAMES) {
        if (e->name == GP_START && type == GP_PRESS) ui_controls_toggle(g_gp.client, !ui_controls_enabled(g_gp.client));
        return;
    }
    const UiControllerInput *c = ui_controls_cntrl_input();
    if (e->name == GP_STICK_AXIS_LEFT) {
        if (type == GP_HOLD) on_left_stick_hold(e->axis[0], e->axis[1]);
        else if (type == GP_RELEASE) {
            key(UI_CONTROL_FORWARD, 0);
            key(UI_CONTROL_BACKWARD, 0);
            key(UI_CONTROL_LEFT, 0);
            key(UI_CONTROL_RIGHT, 0);
        }
    } else if (e->name == GP_STICK_AXIS_RIGHT && type == GP_HOLD) {
        ui_controls_fake_mouse(g_gp.client, 2, (float) apply_deadzone(e->axis[0], c->dead_zone_r),
                               (float) apply_deadzone(e->axis[1], c->dead_zone_r));
    }
}

// _handleEvent(name, events)
static void handle_events(GpEvents *list) {
    if (g_gp.disabled) return;
    for (int i = 0; i < list->count;) {
        GpEvent *e = &list->ev[i];
        g_gp.active = 1;
        if (e->pressed) {
            trigger(GP_PRESS, e);
            e->pressed = 0;
            e->hold = 1;
        } else if (e->hold) {
            trigger(GP_HOLD, e);
        } else if (e->released) {
            trigger(GP_RELEASE, e);
            events_remove(list, i);
            continue;
        }
        i++;
    }
}

// ---------------------------------------------------------------- the loop

// Gamepad._loop: sample getGamepads()[0], update the events, fire them.
static void gamepad_loop(void) {
    if (g_gp.disabled) return;
    GpSample pad;
    if (sample_pad(0, &pad) && pad.connected) {
        listen_buttons(&pad);
        listen_axes(&pad);
    }
    handle_events(&g_gp.buttons);
    handle_events(&g_gp.axes);
}

// _handleGamepadConnected: only the first pad is taken; the loop starts next frame.
static void on_connected(const GpSample *pad, const char *name) {
    if (g_gp.disabled || g_gp.connected) return;
    g_gp.connected = 1;
    g_gp.active = 1;
    memset(&g_gp.buttons, 0, sizeof(g_gp.buttons));
    memset(&g_gp.axes, 0, sizeof(g_gp.axes));
    listen_buttons(pad);
    listen_axes(pad);
    g_gp.running = 1;
    g_gp.next_frame = 0.0;
    printf("[gamepad] connected: %s (%s mapping, %d buttons, %d axes)\n", name ? name : "?",
           pad && pad->standard ? "standard" : "raw", pad ? pad->button_count : 0, pad ? pad->axis_count : 0);
}

// _handleGamepadDisconnected: any pad going away stops the loop. Keys a stick or
// button holds stay down (v3.1.1 drops its events without releasing them).
static void on_disconnected(void) {
    if (g_gp.connected) printf("[gamepad] disconnected\n");
    g_gp.running = 0;
    g_gp.connected = 0;
    memset(&g_gp.buttons, 0, sizeof(g_gp.buttons));
    memset(&g_gp.axes, 0, sizeof(g_gp.axes));
    g_gp.active = 0;
    // KrunkNative: rewrite #instructions now, so a "PRESS START" written while the pad
    // was active does not stay up without a controller (v3.1.1 keeps it until its next
    // updatePlayInstructions() call).
    menu_update_instructions();
}

#ifdef __EMSCRIPTEN__
static bool web_gamepad_event(const int type, const EmscriptenGamepadEvent *e, void *user) {
    (void) user;
    if (type == EMSCRIPTEN_EVENT_GAMEPADCONNECTED) {
        GpSample pad;
        sample_from_html5(&pad, e);
        on_connected(&pad, e->id);
    } else {
        on_disconnected();
    }
    return false;
}
#else
// gamepadconnected / gamepaddisconnected for the joysticks that are gamepads.
static void joystick_callback(const int jid, const int event) {
    if (jid < GLFW_JOYSTICK_1 || jid > GLFW_JOYSTICK_LAST) return;
    if (event == GLFW_CONNECTED) {
        GpSample pad;
        if (!glfwJoystickIsGamepad(jid) || !sample_jid(jid, &pad)) {
            if (g_gp.log) printf("[gamepad] joystick %d (%s) has no gamepad mapping: ignored\n", jid, glfwGetJoystickName(jid));
            return;
        }
        g_pad_slots |= 1u << jid;
        on_connected(&pad, glfwGetGamepadName(jid));
    } else if (event == GLFW_DISCONNECTED && (g_pad_slots & (1u << jid))) {
        g_pad_slots &= ~(1u << jid);
        on_disconnected();
    }
}

// Extra mappings: SDL_GAMECONTROLLERCONFIG lines and a gamecontrollerdb.txt.
static void load_extra_mappings(void) {
    const char *env = getenv("SDL_GAMECONTROLLERCONFIG");
    if (env && *env && !glfwUpdateGamepadMappings(env)) fprintf(stderr, "[gamepad] SDL_GAMECONTROLLERCONFIG rejected\n");
    const char *path = getenv("KRUNKNATIVE_GAMEPAD_DB");
    if (!path || !*path) return;
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[gamepad] cannot open %s\n", path);
        return;
    }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *text = size > 0 ? malloc((size_t) size + 1) : NULL;
    if (text && fread(text, 1, (size_t) size, f) == (size_t) size) {
        text[size] = 0;
        printf("[gamepad] mappings from %s: %s\n", path, glfwUpdateGamepadMappings(text) ? "ok" : "rejected");
    }
    free(text);
    fclose(f);
}
#endif

void gamepad_init(Client *client) {
    memset(&g_gp, 0, sizeof(g_gp));
    g_gp.client = client;
    const char *log = getenv("KRUNKNATIVE_GAMEPAD_LOG");
    g_gp.log = log && *log ? atoi(log) : 0;
#ifdef __EMSCRIPTEN__
    emscripten_set_gamepadconnected_callback(NULL, 1, web_gamepad_event);
    emscripten_set_gamepaddisconnected_callback(NULL, 1, web_gamepad_event);
#else
    load_extra_mappings();
    g_pad_slots = 0;
    glfwSetJoystickCallback(joystick_callback);
    const GLFWvidmode *mode = glfwGetVideoMode(glfwGetPrimaryMonitor());
    const int hz = mode && mode->refreshRate > 0 ? mode->refreshRate : 60;
    g_gp.frame_period = 1.0 / hz;
    for (int jid = GLFW_JOYSTICK_1; jid <= GLFW_JOYSTICK_LAST; jid++) {
        if (glfwJoystickPresent(jid)) joystick_callback(jid, GLFW_CONNECTED);
    }
#endif
}

// ---------------------------------------------------------------- rumble

// weapons.js (27.js; v10.0.2's list adds the later weapons) rumble / rumbleDur by weapon name;
// 0 = unset (|| 0.5 / || 200).
static const struct {
    const char *name;
    float rumble;
    int duration;
} WEAPON_RUMBLE[] = {
    {"Sniper Rifle", 0.9f, 500}, {"Assault Rifle", 0.5f, 0}, {"Pistol", 0.4f, 0}, {"Submachine Gun", 0.4f, 0},
    {"Revolver", 0.7f, 0}, {"Shotgun", 0.8f, 0}, {"Machine Gun", 0.65f, 0}, {"Semi Auto", 0.75f, 0},
    {"Rocket Launcher", 1.0f, 750}, {"Akimbo Uzi", 0.4f, 0}, {"Desert Eagle", 0.8f, 0}, {"Alien Blaster", 0.4f, 0},
    {"Combat Knife", 0.4f, 150}, {"Crossbow", 0.5f, 0}, {"Famas", 0.5f, 0}, {"Sawed Off", 0.8f, 0},
    {"Auto Pistol", 0.8f, 0}, {"Bomb", 0.0f, 0}, {"Blaster", 0.5f, 0},
    // v10.0.2's later weapons
    {"Grappler", 0.8f, 0}, {"Tehchy-9", 0.8f, 0}, {"Noob Tube", 0.8f, 0}, {"Slimer", 0.4f, 0},
    {"Zapper", 0.4f, 0}, {"Minigun", 0.65f, 0}, {"War Machine", 0.8f, 0}, {"Akimbo Pistol", 0.4f, 0},
    {"Charge Rifle", 0.9f, 500}, {"Compressor", 0.8f, 0},
};

// Gamepad.rumble(duration, magnitude)
static void rumble(const int duration, const float magnitude) {
#ifdef __EMSCRIPTEN__
    EM_ASM({
        var pads = navigator.getGamepads ? navigator.getGamepads() : [];
        var p = pads && pads[0];
        if (p && p.vibrationActuator && p.vibrationActuator.type == "dual-rumble" && p.vibrationActuator.playEffect) {
            p.vibrationActuator.playEffect("dual-rumble", {startDelay: 0, duration: $0, weakMagnitude: $1 * 0.5, strongMagnitude: $1});
        }
    }, duration, magnitude);
    if (g_gp.log) printf("[gamepad] rumble %d ms, strong %.2f weak %.2f\n", duration, magnitude, magnitude * 0.5f);
#else
    static int logged;
    if (g_gp.log || !logged++) {
        printf("[gamepad] rumble %d ms, strong %.2f weak %.2f: no force feedback through GLFW (dropped)\n",
               duration, magnitude, magnitude * 0.5f);
    }
#endif
}

// 95.js shoot()/melee() for our own player: the controller rumbles while it is the
// active input, else navigator.vibrate (mblInput.vibration, on by default; a no-op on
// desktop browsers and absent natively).
static void shot_rumble(const Weapon *weapon) {
    float magnitude = 0.0f;
    int duration = 0;
    for (size_t i = 0; weapon && weapon->name && i < sizeof(WEAPON_RUMBLE) / sizeof(WEAPON_RUMBLE[0]); i++) {
        if (!strcmp(weapon->name, WEAPON_RUMBLE[i].name)) {
            magnitude = WEAPON_RUMBLE[i].rumble;
            duration = WEAPON_RUMBLE[i].duration;
            break;
        }
    }
    if (duration <= 0) duration = 200;
    if (magnitude <= 0.0f) magnitude = 0.5f;
    const UiControllerInput *c = ui_controls_cntrl_input();
    if (c->vibration && g_gp.active) {
        rumble(duration, magnitude * c->vibration_mlt); // v10: (weapon.rumble || 0.5) * vibrationMlt
        return;
    }
#ifdef __EMSCRIPTEN__
    EM_ASM({ if ("vibrate" in navigator) { try { navigator.vibrate($0); } catch (e) {} } }, duration);
#endif
}

// A shot or swing taken by our player in this frame's game tick: the weapon's fire
// cooldown (reloads[slot]) only jumps up then.
static void detect_shot(const Client *client) {
    const Player *me = client ? client->me : NULL;
    const int index = me ? me->loadout_index : -1;
    if (!me || !me->active || !me->weapon || !me->reloads || index < 0 || index >= me->loadout_size) {
        g_gp.shot_player = NULL;
        return;
    }
    const float cooldown = me->reloads[index];
    const int same = g_gp.shot_player == me && g_gp.shot_weapon == me->weapon && g_gp.shot_index == index;
    if (same && cooldown > g_gp.shot_cooldown + 1e-6f) shot_rumble(me->weapon);
    g_gp.shot_player = me;
    g_gp.shot_weapon = me->weapon;
    g_gp.shot_index = index;
    g_gp.shot_cooldown = cooldown;
}

// KRUNKNATIVE_GAMEPAD_LOG: a state line every 250 ms (verification runs).
static void log_state(const Client *client) {
    static double last;
    const double now = glfwGetTime();
    if (!g_gp.log || now - last < 0.25) return;
    last = now;
    const Player *me = client ? client->me : NULL;
    const int index = me ? me->loadout_index : -1;
    // held binds: forward back left right jump crouch reload aim (CONTROLS.keys)
    static const int IDS[] = {UI_CONTROL_FORWARD, UI_CONTROL_BACKWARD, UI_CONTROL_LEFT, UI_CONTROL_RIGHT,
                              UI_CONTROL_JUMP, UI_CONTROL_CROUCH, UI_CONTROL_RELOAD, UI_CONTROL_AIM};
    char keys[sizeof(IDS) / sizeof(IDS[0]) + 1];
    for (size_t i = 0; i < sizeof(IDS) / sizeof(IDS[0]); i++) keys[i] = ui_controls_down(IDS[i]) ? "WSADJCRA"[i] : '.';
    keys[sizeof(IDS) / sizeof(IDS[0])] = 0;
    printf("[gamepad] state t=%.2f connected=%d active=%d cntrl=%d menu=%d window=%d enabled=%d keys=%s", now, g_gp.connected,
           g_gp.active, ui_controls_input_is_cntrl(), html_ui_is_menu_open(), menu_window_is_open(), ui_controls_enabled(client), keys);
    if (client) printf(" cam_pitch=%.4f cam_yaw=%.4f", client->camera.rotation.x, client->camera.rotation.y);
    if (me && me->active) {
        printf(" pos=%.2f,%.2f,%.2f vel=%.4f,%.4f,%.4f pitch=%.4f yaw=%.4f ground=%d crouch=%.2f aim=%.2f weapon=%s ammo=%d reload=%.0f",
               me->position.x, me->position.y, me->position.z, me->velocity.x, me->velocity.y, me->velocity.z,
               me->direction.x, me->direction.y, me->on_ground, me->crouch_val, me->aim_val,
               me->weapon && me->weapon->name ? me->weapon->name : "-",
               me->ammo && index >= 0 && index < me->loadout_size ? (int) me->ammo[index] : -1, me->reload_timer);
    }
    printf("\n");
    fflush(stdout);
}

// ---------------------------------------------------------------- per frame

// Start of a game frame: index.js gP() runs CONTROLS.updateMouseInp() first, which sets
// gamepad.active = false unless the last look input was the stick (inputType
// "cntrlInput"). Done here, before the frame's UI/menu work, so an `active` raised by
// gamepadconnected or by a gamepad loop never outlives the next frame's start; the
// instruction text ("PRESS START") only reads it at the moments v3.1.1 does.
void gamepad_frame_begin(void) {
    if (!ui_controls_input_is_cntrl()) g_gp.active = 0;
}

void gamepad_frame(Client *client, const int rendered) {
    g_gp.client = client;
    if (rendered) {
        // The game frame dropped the unused a4/a5 (end of updateMouseInp) and ran the
        // game update (shots -> rumble, gated by the `active` gamepad_frame_begin left).
        ui_controls_look_frame_end();
        detect_shot(client);
        // CONTROLS.reset() on spawn (index.js): speedLmt = 1
        const int me_active = client && client->me && client->me->active;
        if (me_active && !g_gp.me_active) ui_controls_set_speed_limit(1.0f);
        g_gp.me_active = me_active;
    }
    if (!g_gp.running) {
        if (rendered) log_state(client);
        return;
    }
#ifdef __EMSCRIPTEN__
    gamepad_loop(); // this main-loop callback is the animation frame
#else
    const double now = glfwGetTime();
    if (g_gp.next_frame <= 0.0) g_gp.next_frame = now;
    if (now < g_gp.next_frame) {
        if (rendered) log_state(client);
        return;
    }
    int frames = 1 + (int) ((now - g_gp.next_frame) / g_gp.frame_period);
    if (frames > GP_MAX_LOOPS) frames = GP_MAX_LOOPS;
    g_gp.next_frame += frames * g_gp.frame_period;
    if (g_gp.next_frame <= now) g_gp.next_frame = now + g_gp.frame_period; // after a stall
    for (int i = 0; i < frames && g_gp.running; i++) gamepad_loop();
#endif
    if (rendered) log_state(client);
}

int gamepad_active(void) {
    return g_gp.active;
}

void gamepad_set_active(const int active) {
    g_gp.active = active;
}

// v10 CONTROLS.toggleGamepad(v): gamepad.disabled = v; disabling also runs the disconnect.
// KrunkNative: enabling again picks up a pad that is still plugged in (natively; the browser
// fires no new gamepadconnected for it, so on the web the pad needs an input or a replug).
void gamepad_set_disabled(const int disabled) {
    if (g_gp.disabled == !!disabled) return;
    g_gp.disabled = !!disabled;
    if (disabled) {
        on_disconnected();
        return;
    }
#ifndef __EMSCRIPTEN__
    g_pad_slots = 0;
    for (int jid = GLFW_JOYSTICK_1; jid <= GLFW_JOYSTICK_LAST; jid++) {
        if (glfwJoystickPresent(jid)) joystick_callback(jid, GLFW_CONNECTED);
    }
#endif
}

int gamepad_connected(void) {
    return g_gp.connected;
}
