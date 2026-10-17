// Krunker v3.1.1's mobile controls over the DOM-ops API (contract: ui_mobile.h).
//
// Sources (Krunker v10.0.2, game.deob.js; v7.1.4 where v10 kept it):
//   CONTROLS (the controls object, isMobile): the nipplejs joystick (static, size 120,
//           color black, mobile move/end handlers), the mobileControlPositions pass
//           (iiììïîì: mobileJoystick .. mobileKSwap), the touch-target table `iiÌïîiï`
//           (down/up per #mobileUI child, move = fakeMouse(2, ..., "mblInput") for every
//           touch that started on a tracked element), the #mobileUI touchstart /
//           touchmove / touchend / touchcancel listeners, locklessChange
//   UI.toggleGameUI / toggleControlUI / toggleMobileUI: #mobileUI display, #uiBase.isMobile
//   settings: sensitivityXMbl/YMbl, aimSensitivityXMbl/YMbl, invertYMbl, gradSpeedMbl
//           (category "mobile", hide: !isMobile); scaleUI defaults to 0.9 on mobile
//   public/libs/nipplejs.min.js (v7.1.4's copy, the one v10 loads): the joystick's markup,
//           styles and rest/show/hide transitions (fadeTime 0 for Krunker's options)
//
// nipplejs is not vendored: the static joystick Krunker configures is ~100 lines of
// geometry, re-implemented below with the same DOM (div.nipple > div.back + div.front,
// same inline styles, so main.css's #mobileJoystick .back/.front rules apply), the same
// clamping/angle conventions and the same direction thresholds, so the move-key output
// is identical. Keeping it in C keeps the web build free of a third-party script and
// lets the touch logic stay next to the rest of the controls.
//
// Web touch input arrives through emscripten's html5.h touch callbacks registered on
// #mobileUI (the element Krunker listened on). Touch targets are resolved here by
// hit-testing the button rects in paint order (the DOM target Krunker read from
// Touch.target), which keeps the page free of extra JavaScript.
#include <ui_mobile.h>
#include <ui_logic.h>
#include <html_ui.h>
#include <spectate.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/html5.h>
#endif

#define MOBILE_TOUCH_MAX 16
#define NIPPLE_SIZE 120.0f        // nipplejs.create({size: 120})
#define NIPPLE_THRESHOLD 0.1f     // nipplejs default threshold (force)
#define NIPPLE_REST_OPACITY "0.5" // restOpacity (nipplejs default)
#define NIPPLE_FADE_MS 0          // fadeTime (nipplejs default: Krunker's options do not set it)
#define DEG(r) ((r) * 180.0f / (float) M_PI)

// The `ae` table of 132.js, in #mobileUI's DOM order (index.html): the joystick zone
// first, then the buttons. MB_UI is #mobileUI itself (a touch on no button = look).
typedef enum {
    MB_UI = -1,
    MB_JUMP, MB_CROUCH, MB_ESC, MB_SHOOT, MB_AIMSHOOT, MB_SCOPE, MB_RELOAD, MB_SWAP, MB_KSWAP,
    MB_COUNT,
    MB_JOYSTICK = 100,
    MB_PROMPT = 101,     // #interactMsg (the gate prompt): v10's ontouchstart = pressButton(interact)
} MobileTarget;

static const char *const BUTTON_IDS[MB_COUNT] = {
    "mobileJump", "mobileCrouch", "mobileEsc", "mobileShoot", "mobileAimShoot", "mobileScope", "mobileReload", "mobileSwap",
    "mobileKSwap",
};

// G.mobileControlPositions (v10 iiììïîì): fractions of #mobileUI; NAN = not set.
typedef struct { const char *id; float left, top, right; } ControlPosition;
static const ControlPosition POSITIONS[] = {
    {"mobileJoystick", 0.2f, 0.69f, NAN},
    {"mobileJump", NAN, 0.5f, 0.02f},
    {"mobileCrouch", NAN, 0.82f, 0.15f},
    {"mobileEsc", 0.18f, 0.01f, NAN},
    {"mobileShoot", 0.03f, 0.28f, NAN},
    {"mobileAimShoot", NAN, 0.56f, 0.13f},
    {"mobileScope", NAN, 0.35f, 0.11f},
    {"mobileReload", NAN, 0.73f, 0.27f},
    {"mobileSwap", 0.0f, 0.85f, 0.0f},
    {"mobileKSwap", 0.0f, 0.65f, 0.0f},
};
// The JS doubles the fractions above stand for (0.69 * 100 = 68.99999999999999 in JS).
static const double POSITION_VALUES[][3] = {
    {0.2, 0.69, NAN}, {NAN, 0.5, 0.02}, {NAN, 0.82, 0.15}, {0.18, 0.01, NAN}, {0.03, 0.28, NAN},
    {NAN, 0.56, 0.13}, {NAN, 0.35, 0.11}, {NAN, 0.73, 0.27}, {0.0, 0.85, 0.0}, {0.0, 0.65, 0.0},
};

typedef struct {
    long identifier;
    float screen_x, screen_y;
    int cb; // MobileTarget
} TrackedTouch;

static struct {
    int detected;            // checkMobile() ran
    int mobile;              // CONTROLS.isMobile
    int dom_ready;           // positions applied, joystick built, listeners registered
    int visible;             // #mobileUI display (-1 unknown)
    Client *client;

    HtmlElem ui, zone, nipple, back, front;
    HtmlElem buttons[MB_COUNT];
    HtmlElem prompt;         // #interactMsg

    TrackedTouch touches[MOBILE_TOUCH_MAX]; // `ad`
    int touch_count;

    // nipplejs static nipple
    int stick_active;
    long stick_id;
    float stick_cx, stick_cy;    // nipple.position (client px)
    int stick_dir_x, stick_dir_y; // last direction past the threshold: -1 left/down... 0 none
    int stick_has_dir;
    int stick_rest_timer;

    // fakeMouse accumulators (a4/a5 for inputType mblInput)
    float look_dx, look_dy;
    int look_pending;

    // mobileAimShoot's `this` state
    int already_aim, shoot_on_up, ready_to_shoot;
    int auto_fire_timer;         // setInterval(aj.rate) chain, 0 none
    int auto_fire_rate_ms;
} g_mb = {.visible = -1};

// ---------------------------------------------------------------- detection

// CONTROLS.checkMobile(): Android/webOS/iPhone/iPad/iPod/BlackBerry/Windows Phone user
// agents, or a renderer canvas without requestPointerLock. KrunkNative also treats
// iPadOS desktop-mode Safari (a "Macintosh" UA with multi-touch) as mobile, and accepts
// ?mobile=1 / ?mobile=0 to force either layout (testing on desktop browsers).
static int detect_mobile(void) {
    // Krunker Strike detects a phone the same way (FRVR's window._isMobile is the user agent test
    // below): a phone gets the touch controls of strike_touch.c, a desktop the mouse and keyboard
    // (pointer lock, the game's binds). The Strike page itself is the same on both.
#ifdef __EMSCRIPTEN__
    return EM_ASM_INT({
        try {
            var q = /[?&]mobile=([01])/.exec(location.search);
            if (q) return q[1] === '1' ? 1 : 0;
            var ua = String(navigator.userAgent);
            if (/Android|webOS|iPhone|iPad|iPod|BlackBerry|Windows Phone/i.test(ua)) return 1;
            if (/Macintosh/.test(ua) && navigator.maxTouchPoints > 1) return 1;
            var c = document.getElementById('canvas');
            if (c && !(c.requestPointerLock || c.mozRequestPointerLock || c.webkitRequestPointerLock)) return 1;
        } catch (e) {}
        return 0;
    });
#else
    // native: GLFW reports touchscreens as a mouse, so the phone layout is opt-in (the
    // mouse drives the touch controls, ui_mobile_mouse); KRUNKNATIVE_MOBILE=1 forces it
    const char *e = getenv("KRUNKNATIVE_MOBILE");
    return e && e[0] == '1';
#endif
}

int ui_mobile_active(void) {
    if (!g_mb.detected) {
        g_mb.detected = 1;
        g_mb.mobile = detect_mobile();
        if (g_mb.mobile) printf("[mobile] mobile device detected: touch controls on, pointer lock off\n");
    }
    return g_mb.mobile;
}

// The scenario renderer (state.mobile) and tests: force the phone layout on or off before
// the page reads CONTROLS.isMobile.
void ui_mobile_force(const int on) {
    g_mb.detected = 1;
    g_mb.mobile = on ? 1 : 0;
}

// ---------------------------------------------------------------- settings

static float setting_number(const char *key, const float fallback) {
    double d;
    return ui_setting_number(key, &d) ? (float) d : fallback;
}

static int setting_truthy(const char *key, const int fallback) {
    const cJSON *v = ui_setting_value(key);
    if (!v) return fallback;
    if (cJSON_IsBool(v)) return cJSON_IsTrue(v);
    if (cJSON_IsNumber(v)) return cJSON_GetNumberValue(v) != 0.0;
    if (cJSON_IsString(v)) {
        const char *s = cJSON_GetStringValue(v);
        if (!strcmp(s, "false") || !strcmp(s, "0") || !s[0]) return 0;
        return 1;
    }
    return fallback;
}

// ---------------------------------------------------------------- key helpers

static int game_enabled(void) {
    return g_mb.client && ui_controls_enabled(g_mb.client); // CONTROLS.enabled (locklessChange)
}

static void key(const int id, const int down) { // Q(null, binds[id]) / U(null, binds[id]); N() checks enabled
    ui_controls_controller_key(g_mb.client, id, down);
}

static void set_transform(const int b, const char *value) {
    if (b >= 0 && b < MB_COUNT && g_mb.buttons[b]) html_dom_set_style(g_mb.buttons[b], "transform", value);
}

static void look(const float dx, const float dy) { // fakeMouse(2, dx, dy, 0, "mblInput") -> a8
    if (!game_enabled()) return;
    g_mb.look_dx += dx;
    g_mb.look_dy += dy;
    g_mb.look_pending = 1;
}

static Weapon const *my_weapon(void) {
    const Client *c = g_mb.client;
    return c && c->me ? c->me->weapon : NULL;
}

// ---------------------------------------------------------------- timers (setTimeout)

static void t_shoot_off(void *user) { (void) user; ui_controls_touch_shoot(0); }
static void t_reset_keys(void *user) { (void) user; ui_controls_reset_keys(); }

static void t_auto_fire(void *user) { // setInterval(() => { UCUz1 = 1; setTimeout(() => UCUz1 = 0, 100); }, aj.rate)
    (void) user;
    if (!g_mb.auto_fire_timer) return;
    ui_controls_touch_shoot(1);
    html_timer_add(100, t_shoot_off, NULL);
    g_mb.auto_fire_timer = html_timer_add(g_mb.auto_fire_rate_ms, t_auto_fire, NULL);
}

static void t_aim_ready(void *user) { // setTimeout(..., aj.aimSpd + 50)
    (void) user;
    if (!g_mb.ready_to_shoot) return;
    ui_controls_touch_shoot(1);
    const Weapon *w = my_weapon();
    if (w && w->no_auto) {
        html_timer_add(100, t_shoot_off, NULL);
        g_mb.auto_fire_rate_ms = w->rate > 1.0f ? (int) w->rate : 1;
        g_mb.auto_fire_timer = html_timer_add(g_mb.auto_fire_rate_ms, t_auto_fire, NULL);
    }
}

static void stop_auto_fire(void) { // clearInterval(this.autoFire)
    if (g_mb.auto_fire_timer) html_timer_cancel(g_mb.auto_fire_timer);
    g_mb.auto_fire_timer = 0;
}

// ---------------------------------------------------------------- the `ae` handlers

static void spect_prev(void) { // `if (window.Bf5) window.spectMode(-1)`
    if (g_mb.client && spectate_active()) spectate_mouse_press(g_mb.client, 1);
}

// The transform each target's down / up handler writes (v10 iiÌïîiï; scaleMap is empty, so
// the 1.1 press scale and the rest scale 1 are the only values). Crouch never scales, and
// the shoot button keeps its rotate + 1.1 after release, as v10's up handler writes it.
static void button_down(const int b) {
    switch (b) {
        case MB_JUMP: set_transform(b, "scale(1.1)"); key(UI_CONTROL_JUMP, 1); break;
        case MB_CROUCH: set_transform(b, "scale(1)"); key(UI_CONTROL_CROUCH, 1); break;
        case MB_ESC: // V.toggle(false); setTimeout(() => V.resetKeys())
            if (g_mb.client && !html_ui_is_menu_open()) client_set_menu_open(g_mb.client, 1);
            html_timer_add(0, t_reset_keys, NULL);
            break;
        case MB_AIMSHOOT: {
            set_transform(b, "scale(1.1)");
            const Weapon *w = my_weapon();
            if (ui_controls_down(UI_CONTROL_AIM)) {
                g_mb.already_aim = 1;
                ui_controls_touch_shoot(1);
            } else if (w && w->melee) {
                ui_controls_touch_shoot(1);
            } else if (w) {
                key(UI_CONTROL_AIM, 1); // N(aimKey, 1)
                if (w->scope) {
                    g_mb.shoot_on_up = 1;
                } else {
                    g_mb.ready_to_shoot = 1;
                    html_timer_add(w->aim_speed + 50.0f, t_aim_ready, NULL);
                }
            }
            spect_prev();
            break;
        }
        case MB_SHOOT:
            set_transform(b, "rotate(90deg) scale(1.1)");
            if (game_enabled()) ui_controls_touch_shoot(1);
            spect_prev();
            break;
        case MB_SCOPE: // N(aimKey, !V.keys[aimKey])
            set_transform(b, "scale(1.1)");
            key(UI_CONTROL_AIM, !ui_controls_down(UI_CONTROL_AIM));
            break;
        case MB_RELOAD: set_transform(b, "scale(1.1)"); key(UI_CONTROL_RELOAD, 1); break;
        case MB_SWAP: set_transform(b, "scale(1.1)"); key(UI_CONTROL_SWAP, 1); break;
        case MB_KSWAP: set_transform(b, "scale(1.1)"); key(UI_CONTROL_MELEE, 1); break; // binds.melee
        case MB_PROMPT: key(UI_CONTROL_INTERACT, 1); break;
        default: break;
    }
}

static void button_up(const int b) {
    switch (b) {
        case MB_JUMP: set_transform(b, "scale(1)"); key(UI_CONTROL_JUMP, 0); break;
        case MB_CROUCH: set_transform(b, "scale(1)"); key(UI_CONTROL_CROUCH, 0); break;
        case MB_AIMSHOOT:
            g_mb.ready_to_shoot = 0;
            stop_auto_fire();
            set_transform(b, "scale(1)");
            if (g_mb.already_aim) {
                g_mb.already_aim = 0;
                ui_controls_touch_shoot(0);
            } else {
                if (g_mb.shoot_on_up) {
                    g_mb.shoot_on_up = 0;
                    ui_controls_touch_shoot(1);
                    html_timer_add(100, t_shoot_off, NULL);
                } else {
                    ui_controls_touch_shoot(0);
                }
                key(UI_CONTROL_AIM, 0);
            }
            break;
        case MB_SHOOT: set_transform(b, "rotate(90deg) scale(1.1)"); ui_controls_touch_shoot(0); break;
        case MB_SCOPE: set_transform(b, "scale(1)"); break;
        case MB_RELOAD: set_transform(b, "scale(1)"); key(UI_CONTROL_RELOAD, 0); break;
        case MB_SWAP: set_transform(b, "scale(1)"); key(UI_CONTROL_SWAP, 0); break;
        case MB_KSWAP: set_transform(b, "scale(1)"); key(UI_CONTROL_MELEE, 0); break;
        case MB_PROMPT: key(UI_CONTROL_INTERACT, 0); break;
        default: break;
    }
}

// Every tracked touch looks: v10 gives each table entry the same move handler (fakeMouse
// with the touch's screen delta), whichever element the touch started on.
static int has_move(const int cb) {
    return cb == MB_UI || (cb >= 0 && cb < MB_COUNT);
}

// ---------------------------------------------------------------- joystick (nipplejs)

static void nipple_style(const HtmlElem el, const char *prop, const char *value) {
    if (el) html_dom_set_style(el, prop, value);
}

static void stick_front(const float x, const float y) { // applyPosition(front, {x, y})
    char v[32];
    snprintf(v, sizeof(v), "%gpx", x);
    nipple_style(g_mb.front, "left", v);
    snprintf(v, sizeof(v), "%gpx", y);
    nipple_style(g_mb.front, "top", v);
}

static void t_stick_rested(void *user) { // restPosition's timeout -> restCallback: transition none
    (void) user;
    g_mb.stick_rest_timer = 0;
    nipple_style(g_mb.front, "transition", "none");
}

// X.on("move", (aj, ak) => ...) with ak.direction / ak.distance / ak.angle.degree.
static void stick_keys(const float distance, const float angle_deg) {
    if (setting_truthy("gradSpeedMbl", 1)) ui_controls_set_speed_limit((distance > 0.0f ? distance : 50.0f) / 50.0f); // ak.distance || 50
    const float an = fabsf(angle_deg - 180.0f);
    if ((an >= 165.0f && an <= 195.0f) || (an >= 0.0f && an <= 15.0f)) {
        key(UI_CONTROL_FORWARD, 0);
        key(UI_CONTROL_BACKWARD, 0);
    } else {
        if (g_mb.stick_dir_y > 0) { key(UI_CONTROL_BACKWARD, 0); key(UI_CONTROL_FORWARD, 1); }
        else key(UI_CONTROL_FORWARD, 0);
        if (g_mb.stick_dir_y < 0) { key(UI_CONTROL_FORWARD, 0); key(UI_CONTROL_BACKWARD, 1); }
        else key(UI_CONTROL_BACKWARD, 0);
    }
    if (an >= 75.0f && an <= 105.0f) {
        key(UI_CONTROL_LEFT, 0);
        key(UI_CONTROL_RIGHT, 0);
    } else {
        if (g_mb.stick_dir_x < 0) { key(UI_CONTROL_RIGHT, 0); key(UI_CONTROL_LEFT, 1); }
        else key(UI_CONTROL_LEFT, 0);
        if (g_mb.stick_dir_x > 0) { key(UI_CONTROL_LEFT, 0); key(UI_CONTROL_RIGHT, 1); }
        else key(UI_CONTROL_RIGHT, 0);
    }
}

// processOnMove: clamp to the radius, move the front, computeDirection past the
// threshold, then Krunker's move handler.
static void stick_move(const float x, const float y) {
    const float r = NIPPLE_SIZE / 2.0f;
    float dx = x - g_mb.stick_cx, dy = y - g_mb.stick_cy;
    float d = sqrtf(dx * dx + dy * dy);
    const float a = DEG(atan2f(g_mb.stick_cy - y, g_mb.stick_cx - x)); // c.angle(touch, position)
    const float force = d / r;
    if (d > r) { // findCoord(position, r, a)
        d = r;
        dx = -r * cosf(a * (float) M_PI / 180.0f);
        dy = -r * sinf(a * (float) M_PI / 180.0f);
    }
    stick_front(dx, dy);
    if (force > NIPPLE_THRESHOLD) { // computeDirection: x left/right, y up/down
        const float o = a * (float) M_PI / 180.0f;
        g_mb.stick_dir_x = o > -(float) M_PI / 2.0f && o < (float) M_PI / 2.0f ? -1 : 1;
        g_mb.stick_dir_y = o > 0.0f ? 1 : -1;
        g_mb.stick_has_dir = 1;
        stick_keys(d, 180.0f - a); // ak.angle.degree = 180 - a
    }
}

static void stick_start(const long id, const float x, const float y) {
    HtmlRect rect;
    if (!g_mb.nipple || !html_dom_get_rect(g_mb.nipple, &rect)) return;
    g_mb.stick_active = 1;
    g_mb.stick_id = id;
    g_mb.stick_cx = rect.x; // the resize handler: position = el.getBoundingClientRect()
    g_mb.stick_cy = rect.y;
    g_mb.stick_has_dir = 0;
    if (g_mb.stick_rest_timer) html_timer_cancel(g_mb.stick_rest_timer);
    g_mb.stick_rest_timer = 0;
    nipple_style(g_mb.front, "transition", "none"); // show(): restCallback
    nipple_style(g_mb.nipple, "opacity", "1");
    stick_move(x, y);
}

static void stick_end(void) {
    if (!g_mb.stick_active) return;
    g_mb.stick_active = 0;
    g_mb.stick_has_dir = 0;
    g_mb.stick_dir_x = g_mb.stick_dir_y = 0;
    // hide(): restOpacity; restPosition(): front back to the centre over fadeTime
    nipple_style(g_mb.nipple, "opacity", NIPPLE_REST_OPACITY);
    nipple_style(g_mb.front, "transition", "top 0ms, left 0ms");
    stick_front(0.0f, 0.0f);
    g_mb.stick_rest_timer = html_timer_add(NIPPLE_FADE_MS, t_stick_rested, NULL);
    // X.on("end"): U() every move key
    key(UI_CONTROL_FORWARD, 0);
    key(UI_CONTROL_BACKWARD, 0);
    key(UI_CONTROL_LEFT, 0);
    key(UI_CONTROL_RIGHT, 0);
}

// ---------------------------------------------------------------- touch routing

static int point_in_rect(const HtmlElem el, const float x, const float y) {
    HtmlRect r;
    return el && html_dom_get_rect(el, &r) && r.w > 0.0f && r.h > 0.0f && x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

static int point_in_circle(const HtmlElem el, const float x, const float y) { // border-radius: 50%
    HtmlRect r;
    if (!el || !html_dom_get_rect(el, &r) || r.w <= 0.0f) return 0;
    const float dx = x - (r.x + r.w * 0.5f), dy = y - (r.y + r.h * 0.5f);
    return dx * dx + dy * dy <= r.w * r.w * 0.25f;
}

// Touch.target: the topmost child of #mobileUI under the point. The buttons share one
// z-index, so later siblings paint on top; the joystick zone comes first in the DOM.
static int prompt_hit(const float x, const float y) {
    return point_in_rect(g_mb.prompt, x, y); // display:none (no prompt) has no box
}

static int hit_target(const float x, const float y) {
    for (int b = MB_COUNT - 1; b >= 0; b--) {
        if (point_in_rect(g_mb.buttons[b], x, y)) return b;
    }
    if (point_in_circle(g_mb.front, x, y) || point_in_circle(g_mb.back, x, y)) return MB_JOYSTICK;
    return MB_UI;
}

static int find_touch(const long identifier) { // K()
    for (int i = 0; i < g_mb.touch_count; i++) {
        if (g_mb.touches[i].identifier == identifier) return i;
    }
    return -1;
}

static void drop_touch(const int i) { // ad.splice(i, 1)
    memmove(&g_mb.touches[i], &g_mb.touches[i + 1], sizeof(TrackedTouch) * (size_t) (g_mb.touch_count - i - 1));
    g_mb.touch_count--;
}

int ui_mobile_touch(const UiTouchPhase phase, const UiTouch *changed, const int count) {
    if (!g_mb.mobile || !g_mb.dom_ready) return 0;
    menu_idle_reset(); // N(): idleTimer = 0
    for (int i = 0; i < count; i++) {
        const UiTouch *t = &changed[i];
        const int k = find_touch(t->identifier);
        switch (phase) {
            case UI_TOUCH_START: {
                if (!game_enabled()) break; // the UI is only up while the game has the input
                if (prompt_hit(t->client_x, t->client_y)) { // the gate prompt is a tap on interact
                    button_down(MB_PROMPT);
                    if (g_mb.touch_count < MOBILE_TOUCH_MAX) g_mb.touches[g_mb.touch_count++] = (TrackedTouch) {t->identifier, t->screen_x, t->screen_y, MB_PROMPT};
                    break;
                }
                const int target = hit_target(t->client_x, t->client_y);
                if (target == MB_JOYSTICK) {
                    if (!g_mb.stick_active) stick_start(t->identifier, t->client_x, t->client_y); // maxNumberOfNipples 1
                    break;
                }
                if (target != MB_UI) button_down(target);
                if (g_mb.touch_count < MOBILE_TOUCH_MAX) {
                    g_mb.touches[g_mb.touch_count++] = (TrackedTouch) {t->identifier, t->screen_x, t->screen_y, target};
                }
                break;
            }
            case UI_TOUCH_MOVE:
                if (g_mb.stick_active && t->identifier == g_mb.stick_id) {
                    stick_move(t->client_x, t->client_y);
                    break;
                }
                if (k < 0) break;
                if (has_move(g_mb.touches[k].cb)) look(t->screen_x - g_mb.touches[k].screen_x, t->screen_y - g_mb.touches[k].screen_y);
                g_mb.touches[k].screen_x = t->screen_x;
                g_mb.touches[k].screen_y = t->screen_y;
                break;
            case UI_TOUCH_END:
            case UI_TOUCH_CANCEL:
                if (g_mb.stick_active && t->identifier == g_mb.stick_id) {
                    stick_end();
                    break;
                }
                if (k < 0) break;
                if (g_mb.touches[k].cb != MB_UI) button_up(g_mb.touches[k].cb);
                drop_touch(k);
                break;
        }
    }
    return 1; // aj.preventDefault() (touchend only skips it in Krunker; no default there either)
}

// One touch by identifier and CSS-px position (native scripted multi-touch: input_script.c `touch`);
// the same routing as a finger on the web build's #mobileUI listeners.
int ui_mobile_touch_point(const UiTouchPhase phase, const long identifier, const float x, const float y) {
    const UiTouch t = {identifier, x, y, x, y};
    return ui_mobile_touch(phase, &t, 1);
}

// ---------------------------------------------------------------- native mouse as the touch

// Native builds have no touchscreen events: with the phone layout forced (KRUNKNATIVE_MOBILE=1,
// state.mobile), the left mouse button is the one touch: a press starts it on whatever
// #mobileUI element is under the pointer, a drag is its move, the release its end. Only
// while #mobileUI is up (the game has the input); otherwise the page gets the mouse.
#define MOUSE_TOUCH_ID 0x6d6f7573L
static int g_mouse_touch;

int ui_mobile_mouse(const UiTouchPhase phase, const float x, const float y) {
    if (!g_mb.mobile || !g_mb.dom_ready || g_mb.visible != 1) return 0;
    if (phase != UI_TOUCH_START && !g_mouse_touch) return 0;
    const UiTouch t = {MOUSE_TOUCH_ID, x, y, x, y};
    if (phase == UI_TOUCH_START) g_mouse_touch = 1;
    if (phase == UI_TOUCH_END || phase == UI_TOUCH_CANCEL) g_mouse_touch = 0;
    ui_mobile_touch(phase, &t, 1);
    return 1;
}

// ---------------------------------------------------------------- web backend

#ifdef __EMSCRIPTEN__
static EM_BOOL on_touch(const int type, const EmscriptenTouchEvent *ev, void *user) {
    (void) user;
    UiTouch changed[MOBILE_TOUCH_MAX];
    int n = 0;
    for (int i = 0; i < ev->numTouches && n < MOBILE_TOUCH_MAX; i++) {
        const EmscriptenTouchPoint *p = &ev->touches[i];
        if (!p->isChanged) continue;
        changed[n++] = (UiTouch) {p->identifier, (float) p->clientX, (float) p->clientY, (float) p->screenX, (float) p->screenY};
    }
    const UiTouchPhase phase = type == EMSCRIPTEN_EVENT_TOUCHSTART ? UI_TOUCH_START
        : type == EMSCRIPTEN_EVENT_TOUCHMOVE ? UI_TOUCH_MOVE
        : type == EMSCRIPTEN_EVENT_TOUCHEND ? UI_TOUCH_END : UI_TOUCH_CANCEL;
    return ui_mobile_touch(phase, changed, n) ? EM_TRUE : EM_FALSE;
}

static void register_touch_listeners(void) {
    emscripten_set_touchstart_callback("#mobileUI", NULL, EM_FALSE, on_touch);
    emscripten_set_touchmove_callback("#mobileUI", NULL, EM_FALSE, on_touch);
    emscripten_set_touchend_callback("#mobileUI", NULL, EM_FALSE, on_touch);
    emscripten_set_touchcancel_callback("#mobileUI", NULL, EM_FALSE, on_touch);
    // #interactMsg is a sibling of #mobileUI: its taps (the gate prompt) start there
    emscripten_set_touchstart_callback("#interactMsg", NULL, EM_FALSE, on_touch);
    emscripten_set_touchmove_callback("#interactMsg", NULL, EM_FALSE, on_touch);
    emscripten_set_touchend_callback("#interactMsg", NULL, EM_FALSE, on_touch);
    emscripten_set_touchcancel_callback("#interactMsg", NULL, EM_FALSE, on_touch);
}
#else
static void register_touch_listeners(void) {}
#endif

// ---------------------------------------------------------------- DOM setup

static void fmt_percent(char *buf, const size_t n, const double fraction) {
    snprintf(buf, n, "%.16g%%", fraction * 100.0);
}

// The mobileControlPositions pass (`window.mobileControlPositions` overrides are a
// mod hook; KrunkNative has no mods, so the defaults apply).
static void apply_positions(void) {
    static const char *const SIDES[3] = {"left", "top", "right"};
    for (size_t i = 0; i < sizeof(POSITIONS) / sizeof(POSITIONS[0]); i++) {
        const HtmlElem el = html_dom_by_id(POSITIONS[i].id);
        if (!el) continue;
        // v10: the shoot button is rotated a quarter turn (the rocket points along the stick)
        if (!strcmp(POSITIONS[i].id, "mobileShoot")) html_dom_set_style(el, "transform", "rotate(90deg)");
        for (int s = 0; s < 3; s++) {
            const double v = POSITION_VALUES[i][s];
            if (isnan(v)) continue;
            char value[48];
            fmt_percent(value, sizeof(value), v);
            html_dom_set_style(el, SIDES[s], value);
        }
        // `if (ak.left == ak.right) al.style.margin = "auto"` (undefined == undefined too)
        const int same = (isnan(POSITIONS[i].left) && isnan(POSITIONS[i].right)) || POSITIONS[i].left == POSITIONS[i].right;
        if (same) html_dom_set_style(el, "margin", "auto");
    }
}

// nipplejs.create({zone: #mobileJoystick, size: 120, mode: "static", color: "black"}):
// the idle static nipple with its inline styles (stylize()) at the zone origin.
static void build_joystick(void) {
    g_mb.zone = html_dom_by_id("mobileJoystick");
    if (!g_mb.zone) return;
    html_dom_set_style(g_mb.zone, "touch-action", "none");
    html_dom_set_html(g_mb.zone,
        "<div class=\"nipple collection_0\" id=\"nipple_0_0\" style=\"position: absolute; opacity: 0.5; display: block; z-index: 999; transition: opacity;\">"
        "<div class=\"back\" style=\"position: absolute; display: block; width: 120px; height: 120px; margin-left: -60px; margin-top: -60px; background: black; opacity: 0.5; border-radius: 50%;\"></div>"
        "<div class=\"front\" style=\"width: 60px; height: 60px; position: absolute; display: block; margin-left: -30px; margin-top: -30px; background: black; opacity: 0.5; border-radius: 50%; left: 0px; top: 0px;\"></div>"
        "</div>");
    g_mb.nipple = html_dom_by_id("nipple_0_0");
    g_mb.back = g_mb.nipple ? html_dom_query(g_mb.nipple, ".back") : 0;
    g_mb.front = g_mb.nipple ? html_dom_query(g_mb.nipple, ".front") : 0;
}

static void dom_init(void) {
    g_mb.ui = html_dom_by_id("mobileUI");
    if (!g_mb.ui) return;
    for (int b = 0; b < MB_COUNT; b++) g_mb.buttons[b] = html_dom_by_id(BUTTON_IDS[b]);
    g_mb.prompt = html_dom_by_id("interactMsg");
    build_joystick();
    apply_positions();
    register_touch_listeners();
    // uiBase.classList.toggle('isMobile', UI.isMobileControls) (v10 at the start of a game)
    const HtmlElem base = html_dom_by_id("uiBase");
    if (base) html_dom_set_class(base, "isMobile", 1);
    g_mb.dom_ready = 1;
}

// Every touch and held control lets go (the UI went away under the fingers): the targets
// go back to the transform their up handler leaves them in.
static void release_all(void) {
    for (int i = g_mb.touch_count - 1; i >= 0; i--) {
        const int b = g_mb.touches[i].cb;
        if (b == MB_PROMPT) key(UI_CONTROL_INTERACT, 0);
        if (b == MB_UI || b == MB_ESC || b == MB_PROMPT) continue;
        set_transform(b, b == MB_SHOOT ? "rotate(90deg) scale(1.1)" : "scale(1)");
    }
    g_mb.touch_count = 0;
    stick_end();
    stop_auto_fire();
    g_mb.ready_to_shoot = g_mb.shoot_on_up = g_mb.already_aim = 0;
    g_mb.look_dx = g_mb.look_dy = 0.0f;
    g_mb.look_pending = 0;
}

void ui_mobile_tick(Client *client) {
    g_mb.client = client;
    if (!ui_mobile_active() || !html_ui_available() || !html_ui_is_ready()) return;
    if (!g_mb.dom_ready) dom_init();
    if (!g_mb.dom_ready) return;

    // toggleControlUI(j): mobileUI shown when j && UI.active && isMobile, i.e. the game
    // has the input (menu closed) and neither the death card (toggleGameUI(false)) nor
    // the end screen (hideUI) took the HUD away. Spectators keep it (mobileShoot = prev).
    const int alive = client && client->in_game && client->me && client->me->active;
    const int visible = !html_ui_is_menu_open() && !hud_ui_end_screen_interactive() && (alive || spectate_active());
    if (visible != g_mb.visible) {
        g_mb.visible = visible;
        html_dom_set_display(g_mb.ui, visible ? "block" : "none");
        if (!visible) release_all();
    }
}

void ui_mobile_look_sens(const int aiming, float *sx, float *sy, int *invert) {
    // mblInput: sensX = sensitivityXMbl * 4 (aimX with the right button), invert = invertYMbl
    *sx = 4.0f * setting_number(aiming ? "aimSensitivityXMbl" : "sensitivityXMbl", 1.4f);
    *sy = 4.0f * setting_number(aiming ? "aimSensitivityYMbl" : "sensitivityYMbl", 1.4f);
    *invert = setting_truthy("invertYMbl", 0);
}

int ui_mobile_touch_active(void) {
    return g_mb.stick_active || g_mb.touch_count > 0;
}

void ui_mobile_add_look(Client *client, const float dx, const float dy) {
    if (client) g_mb.client = client;
    look(dx, dy);
}

int ui_mobile_take_look(const int aiming, float *dx, float *dy) {
    if (!g_mb.look_pending) return 0;
    float sx, sy;
    int invert;
    ui_mobile_look_sens(aiming, &sx, &sy, &invert);
    *dx = g_mb.look_dx * sx;
    *dy = g_mb.look_dy * sy * (invert ? -1.0f : 1.0f);
    g_mb.look_dx = g_mb.look_dy = 0.0f;
    g_mb.look_pending = 0;
    return 1;
}

// ---------------------------------------------------------------- settings window

// The tab markup was generated with isMobile false (v7.1.4 windows[0].getSettings): every
// "mobile" row is `id="<key>_div" style="display:none"` and, as none counts towards the
// category, the Mobile header and body stay `style="display:none"`. With isMobile the rows
// are display:block and the category shows.
char *ui_mobile_settings_html(const char *tab_html) {
    static const char HEAD_HIDDEN[] = "id=\"setHed_mobile\" style=\"display:none\"";
    static const char BODY_HIDDEN[] = "id=\"setBod_mobile\" style=\"display:none\"";
    static const char ROW_HIDDEN[] = "_div\" style=\"display:none\"";
    if (!tab_html || !ui_mobile_active() || !strstr(tab_html, HEAD_HIDDEN)) return NULL;
    const char *body = strstr(tab_html, BODY_HIDDEN);
    StrBuf sb = {0};
    const char *p = tab_html;
    const char *head = strstr(p, HEAD_HIDDEN);
    sb_append_n(&sb, p, (size_t) (head - p));
    sb_append(&sb, "id=\"setHed_mobile\"");
    p = head + sizeof(HEAD_HIDDEN) - 1;
    if (body && body > p) {
        sb_append_n(&sb, p, (size_t) (body - p));
        sb_append(&sb, "id=\"setBod_mobile\"");
        p = body + sizeof(BODY_HIDDEN) - 1;
        // the category's rows: up to the next category header
        const char *end = strstr(p, "<div class=\"setHed\"");
        if (!end) end = p + strlen(p);
        for (const char *row; (row = strstr(p, ROW_HIDDEN)) && row < end;) {
            sb_append_n(&sb, p, (size_t) (row - p));
            sb_append(&sb, "_div\" style=\"display:block\"");
            p = row + sizeof(ROW_HIDDEN) - 1;
        }
    }
    sb_append(&sb, p);
    return sb_finish(&sb);
}
