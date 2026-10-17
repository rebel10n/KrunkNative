// gamepad_test: the controller module (client/src/gamepad.c) against Krunker v10.0.2's Gamepad
// class and CONTROLS' controller block (~/.cache/kn-v10/v10.js): the right trigger is the "shoot"
// KEY (press / release) and a fake mouse button at triggerThres while held, the left stick's
// 8-way sectors after the rescaling dead zone, KrunkScript hooks for every button and stick
// (vendor included), Disable Controllers (toggleGamepad) and vibrationMlt.
// GL- and window-free: it includes gamepad.c and stubs GLFW's joystick API and the ui_controls
// entry points, recording what the pad did.
//   gamepad_test
#include "../src/gamepad.c"

#include <stdarg.h>

static int failures, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

// ---------------------------------------------------------------- GLFW stubs (one pad in slot 0)

static double g_time;
static GLFWgamepadstate g_state;
static int g_present = 1;
static GLFWjoystickfun g_joy_cb;

double glfwGetTime(void) { return g_time; }
int glfwJoystickPresent(int jid) { return jid == 0 && g_present; }
int glfwJoystickIsGamepad(int jid) { return jid == 0 && g_present; }
int glfwGetGamepadState(int jid, GLFWgamepadstate *state) {
    if (jid != 0 || !g_present) return GLFW_FALSE;
    *state = g_state;
    return GLFW_TRUE;
}
const char *glfwGetJoystickName(int jid) { (void) jid; return "test pad"; }
const char *glfwGetGamepadName(int jid) { (void) jid; return "Test Pad"; }
int glfwUpdateGamepadMappings(const char *s) { (void) s; return GLFW_TRUE; }
GLFWjoystickfun glfwSetJoystickCallback(GLFWjoystickfun cb) { GLFWjoystickfun old = g_joy_cb; g_joy_cb = cb; return old; }
GLFWmonitor *glfwGetPrimaryMonitor(void) { return NULL; }
const GLFWvidmode *glfwGetVideoMode(GLFWmonitor *m) { (void) m; return NULL; } // 60 Hz

// ---------------------------------------------------------------- ui_controls / ks stubs

static UiControllerInput g_cntrl = {20.0f, 20.0f, 15.0f, 15.0f, 0.3f, 0.25f, 0.1f, 0, 1, 0, 1.0f};
static int g_keys[UI_CONTROL_COUNT];
static int g_mouse_down, g_mouse_up, g_toggles, g_enabled = 1;
static char g_ks[64][48];
static int g_ks_count;
static float g_speed = 1.0f;

UiControllerInput *ui_controls_cntrl_input(void) { return &g_cntrl; }
void ui_controls_controller_key(Client *c, int id, int down) { (void) c; if (id >= 0 && id < UI_CONTROL_COUNT) g_keys[id] = down; }
void ui_controls_fake_mouse(Client *c, int type, float dx, float dy) {
    (void) c; (void) dx; (void) dy;
    if (type == 0) g_mouse_down++;
    else if (type == 1) g_mouse_up++;
}
void ui_controls_set_speed_limit(float limit) { g_speed = limit; }
int ui_controls_bind(int id) { return id == UI_CONTROL_SHOOT ? 10001 : id == UI_CONTROL_JUMP ? 32 : 0; }
void ks_client_controller(int type, const char *button, int code) {
    if (g_ks_count < 64) snprintf(g_ks[g_ks_count++], sizeof(g_ks[0]), "%s %s %d", GP_TYPE[type], button, code);
}
void ui_controls_toggle(Client *c, int on) { (void) c; g_toggles++; g_enabled = on; }
int ui_controls_enabled(const Client *c) { (void) c; return g_enabled; }
int ui_text_input_focused(void) { return 0; }
void menu_update_instructions(void) {}
int strike_menu_pad_button(int button, int pressed) { (void) button; (void) pressed; return 0; }
int strike_menu_pad_stick(double x, double y) { (void) x; (void) y; return 0; }
int html_ui_is_menu_open(void) { return 0; }
int menu_window_is_open(void) { return 0; }
int ui_controls_input_is_cntrl(void) { return 0; }
int ui_controls_down(int id) { (void) id; return 0; }
void ui_controls_look_frame_end(void) {}

// ---------------------------------------------------------------- helpers

static void reset_pad(void) {
    memset(&g_state, 0, sizeof(g_state));
    g_state.axes[GLFW_GAMEPAD_AXIS_LEFT_TRIGGER] = -1.0f; // a trigger at rest reads -1
    g_state.axes[GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER] = -1.0f;
}

// One animation frame of the gamepad loop.
static void frame(void) {
    g_time += 1.0 / 60.0;
    gamepad_frame(NULL, 0);
}

static int ks_has(const char *line) {
    for (int i = 0; i < g_ks_count; i++) {
        if (!strcmp(g_ks[i], line)) return 1;
    }
    return 0;
}

static void stick(const float x, const float y) {
    g_state.axes[GLFW_GAMEPAD_AXIS_LEFT_X] = x;
    g_state.axes[GLFW_GAMEPAD_AXIS_LEFT_Y] = y;
    frame(); // press
    frame(); // hold: the move binds
}

#define MOVE(f, b, l, r, what) CHECK(g_keys[UI_CONTROL_FORWARD] == (f) && g_keys[UI_CONTROL_BACKWARD] == (b) && \
    g_keys[UI_CONTROL_LEFT] == (l) && g_keys[UI_CONTROL_RIGHT] == (r), "%s: F%d B%d L%d R%d (want %d %d %d %d)", what, \
    g_keys[UI_CONTROL_FORWARD], g_keys[UI_CONTROL_BACKWARD], g_keys[UI_CONTROL_LEFT], g_keys[UI_CONTROL_RIGHT], f, b, l, r)

// ---------------------------------------------------------------- tests

static void test_trigger(void) {
    reset_pad();
    g_mouse_down = g_mouse_up = 0;
    g_ks_count = 0;
    g_state.axes[GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER] = 0.0f; // value 0.5
    frame();
    CHECK(g_keys[UI_CONTROL_SHOOT] == 1, "trigger press holds the shoot bind");
    CHECK(g_mouse_down == 0, "the press is the KEY, not a fake mouse button (%d)", g_mouse_down);
    CHECK(ks_has("press shoulder_bottom_right 10001"), "KrunkScript onControllerPress gets the shoot bind's code");
    frame();
    CHECK(g_mouse_down == 1 && g_mouse_up == 0, "held past triggerThres: fake mousedown (%d / %d)", g_mouse_down, g_mouse_up);
    g_state.axes[GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER] = -0.74f; // value 0.13: pressed (> 30/255) but under triggerThres 0.15
    g_cntrl.trigger_thres = 0.15f;
    frame();
    CHECK(g_mouse_up == 1, "held under triggerThres: fake mouseup (%d)", g_mouse_up);
    CHECK(g_keys[UI_CONTROL_SHOOT] == 1, "v10: the shoot bind stays down while the trigger is pressed");
    g_cntrl.trigger_thres = 0.1f;
    reset_pad();
    frame();
    CHECK(g_keys[UI_CONTROL_SHOOT] == 0, "trigger release lets the shoot bind go");
    CHECK(ks_has("release shoulder_bottom_right 10001"), "KrunkScript onControllerUp");
}

static void test_left_stick(void) {
    reset_pad();
    memset(g_keys, 0, sizeof(g_keys));
    stick(0.0f, -0.9f);
    MOVE(1, 0, 0, 0, "up: forward");
    stick(0.6f, -0.6f);
    MOVE(1, 0, 0, 1, "up-right: forward + right");
    stick(0.9f, 0.0f);
    MOVE(0, 0, 0, 1, "right");
    stick(0.6f, 0.6f);
    MOVE(0, 1, 0, 1, "down-right: backward + right");
    stick(0.0f, 0.9f);
    MOVE(0, 1, 0, 0, "down: backward");
    stick(-0.6f, 0.6f);
    MOVE(0, 1, 1, 0, "down-left: backward + left");
    stick(-0.9f, 0.0f);
    MOVE(0, 0, 1, 0, "left");
    stick(-0.6f, -0.6f);
    MOVE(1, 0, 1, 0, "up-left: forward + left");
    // v3.1.1 held forward and right for (0.35, -0.9) (both axes past deadZoneL); v10's sector is up
    stick(0.35f, -0.9f);
    MOVE(1, 0, 0, 0, "mostly up (a little right): forward only");
    // past the 0.3 event threshold on x, inside deadZoneL 0.4 after a change: all four up
    g_cntrl.dead_zone_l = 0.4f;
    stick(0.35f, 0.0f);
    MOVE(0, 0, 0, 0, "inside the dead zone: nothing held");
    g_cntrl.dead_zone_l = 0.3f;
    stick(0.0f, -0.9f);
    MOVE(1, 0, 0, 0, "up again");
    reset_pad();
    frame();
    MOVE(0, 0, 0, 0, "stick released: every move bind up");
    g_cntrl.grad_speed = 1;
    stick(0.0f, -0.65f);
    CHECK(fabsf(g_speed - 0.65f / 0.7f) < 1e-4f, "gradSpeed: speedLmt = max(|x|, |y|) / (1 - deadZoneL) = %f", g_speed);
    g_cntrl.grad_speed = 0;
    reset_pad();
    frame();
}

static void test_ks_every_button(void) {
    reset_pad();
    g_ks_count = 0;
    g_toggles = 0;
    g_state.buttons[GLFW_GAMEPAD_BUTTON_GUIDE] = GLFW_PRESS;
    g_state.buttons[GLFW_GAMEPAD_BUTTON_START] = GLFW_PRESS;
    g_state.buttons[GLFW_GAMEPAD_BUTTON_A] = GLFW_PRESS;
    g_state.axes[GLFW_GAMEPAD_AXIS_RIGHT_X] = 0.8f;
    frame();
    CHECK(ks_has("press vendor 0"), "v10 names button 16 vendor and hands it to KrunkScript");
    CHECK(ks_has("press start 0"), "start reaches KrunkScript");
    CHECK(ks_has("press button_1 32"), "button_1: the jump bind's code");
    CHECK(ks_has("press stick_axis_right 0"), "the right stick reaches KrunkScript");
    CHECK(g_toggles == 1, "start toggles the controls (%d)", g_toggles);
    CHECK(g_keys[UI_CONTROL_JUMP] == 1, "button_1 holds jump");
    reset_pad();
    frame();
    CHECK(g_keys[UI_CONTROL_JUMP] == 0, "button_1 release");
}

static void test_disable(void) {
    reset_pad();
    memset(g_keys, 0, sizeof(g_keys));
    CHECK(gamepad_connected(), "the plugged-in pad is connected at start");
    gamepad_set_disabled(1);
    CHECK(!gamepad_connected(), "Disable Controllers disconnects the pad");
    g_state.buttons[GLFW_GAMEPAD_BUTTON_A] = GLFW_PRESS;
    frame();
    frame();
    CHECK(g_keys[UI_CONTROL_JUMP] == 0, "a disabled pad presses nothing");
    if (g_joy_cb) g_joy_cb(0, GLFW_CONNECTED); // a hotplug while disabled
    CHECK(!gamepad_connected(), "a disabled pad does not connect on hotplug");
    gamepad_set_disabled(0);
    CHECK(gamepad_connected(), "enabling again picks the plugged-in pad up");
    frame();
    frame();
    CHECK(g_keys[UI_CONTROL_JUMP] == 1, "and it works");
    reset_pad();
    frame();
}

static void test_deadzone_math(void) {
    CHECK(stick_deadzone(0.2, 0.3) == 0.0, "inside: 0");
    CHECK(fabs(stick_deadzone(0.65, 0.3) - 0.5) < 1e-9, "0.65 past 0.3 rescales to 0.5");
    CHECK(fabs(stick_deadzone(-1.0, 0.3) + 1.0) < 1e-9, "full deflection stays 1");
    CHECK(stick_deadzone(1.0, 1.0) == 0.0, "deadZoneL 1: 0, not a non-number");
    CHECK(stick_sector(0.0, 1.0) == 0 && stick_sector(1.0, 0.0) == 2 && stick_sector(0.0, -1.0) == 4 &&
          stick_sector(-1.0, 0.0) == 6 && stick_sector(-0.1, 1.0) == 8, "sectors: down 0, right 2, up 4, left 6, down-a-bit-left 8");
}

int main(void) {
    reset_pad();
    g_time = 1.0;
    gamepad_init(NULL); // the pad in slot 0 is already plugged in
    frame();            // the loop starts on the next animation frame
    test_deadzone_math();
    test_trigger();
    test_left_stick();
    test_ks_every_button();
    test_disable();
    printf("gamepad_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
