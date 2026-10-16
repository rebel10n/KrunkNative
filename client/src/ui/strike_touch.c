// Krunker Strike's mobile controls (strike_ui.h), the C port of FRVR 1.37.2's a5 component and the
// CONTROLS mobile hooks it drives (assets/index-Cz2uYXJQ.js, read as text):
//   a5.view (mode Play)   .joystick-zones (two .joystick-zone: movement, the invisible aim zone)
//                         + .control-buttons with Gw's controls in order (vhe: ohe button, lhe toggle,
//                         rhe joystick; che hint-only renders nothing while playing; disabled ones
//                         - crouch, swap - are dropped by uhe)
//   Gw                    every control's icon, position (xA: calc(N * var(--base-scale))), size and
//                         visibility; the saved mobile_layouts override positions (customize mode)
//   nF / mhe.init         movement zone (nipplejs dynamic, size Pce(106)): R start, Q move (the move
//                         keys from direction + |angle|), V end; aim zone: tap-to-jump (cf <= 9) and
//                         drag look (Y: fakeMouse(2, raw dx, dy, "mblInput")); aimJoyStick (static):
//                         aim + shoot while held, look while dragged
//   JU (nipplejs 0.10)    the nipple DOM (div.nipple.collection_N#nipple_N_M > .back + .front) with its
//                         inline styles, show / hide / rest timings (fadeTime 250 ms), the clamp and
//                         direction maths
// The page is the same on every platform: natively the mouse is one finger (left button), on the web
// real touches arrive through strike_touch_event (emscripten touch callbacks on the page) and the
// mouse works the same way as natively.
// The layout numbers (Gw's positions and sizes, the 640x360 base, the tap-to-jump travel, the move
// angles) are FRVR's data: StrikeRules.touch (shared/include/strike_rules.h, extracted into
// tools/strike/strike_data.json "input.touch"); the table below keeps what is code (binds, kinds).
#include <strike_ui.h>
#include <strike_rules.h>
#include <flavor.h>
#include <html_ui.h>
#include <ui_logic.h>
#include <ui_mobile.h>
#include <cJSON.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/html5.h>
#endif

#define FADE_MS 250          // nipplejs fadeTime
#define THRESHOLD 0.1f       // nipplejs threshold (force)
#define POINTERS_MAX 8
#define MOUSE_ID (-1L)
#define DEG(r) ((r) * 180.0f / (float) M_PI)
#define RAD(d) ((d) * (float) M_PI / 180.0f)

// Gw, in object order; type 0 button, 1 toggle, 2 joystick (cr). Sides: right/left x top/bottom.
typedef enum { C_JUMP, C_RELOAD, C_MELEE, C_SHOOT, C_SHOOT_AIM, C_FAKE_MOVE, C_AIM_STICK, C_MENU, C_COUNT } Control;
typedef struct {
    const char *key;     // Gw key (mobile_layouts entries)
    int type;
    const char *icon, *icon_active;
    int bind;            // UI_CONTROL_* the action presses, -1 none
    float right, left, top, bottom; // NAN = unset
    int size;
    const char *cls;     // rhe's attrs.class (p1 turns undefined into the class "undefined")
} ControlDef;

static ControlDef CONTROLS[C_COUNT] = { // positions / sizes replaced from StrikeRules.touch (layout_from_rules)
    [C_JUMP] = {"jump", 0, "jump", NULL, UI_CONTROL_JUMP, 26, NAN, 97, NAN, 46, NULL},
    [C_RELOAD] = {"reload", 0, "reload", NULL, UI_CONTROL_RELOAD, 190, NAN, NAN, 16, 46, NULL},
    [C_MELEE] = {"melee", 1, "knife", "gun", UI_CONTROL_MELEE, 100, NAN, NAN, 16, 46, NULL},
    [C_SHOOT] = {"shoot", 0, "shoot", NULL, UI_CONTROL_SHOOT, 190, NAN, 140, NAN, 46, NULL},
    [C_SHOOT_AIM] = {"shootWhenAim", 0, "shoot", NULL, UI_CONTROL_SHOOT, NAN, 52, 140, NAN, 46, NULL},
    [C_FAKE_MOVE] = {"fakeMoveJoystick", 2, NULL, NULL, -1, NAN, 69, NAN, 49, 106, "undefined"},
    [C_AIM_STICK] = {"aimJoyStick", 2, NULL, NULL, -1, 97, NAN, 74, NAN, 88, "joystick--aim"},
    [C_MENU] = {"backToMenu", 0, "menu", NULL, -1, NAN, 24, 24, NAN, 32, NULL},
};

// one nipplejs nipple (Ws)
typedef struct {
    HtmlElem el, back, front;
    float size;            // px
    float px, py;          // position (page px)
    int shown;
    int hide_timer;
} Nipple;

typedef enum { T_NONE, T_BUTTON, T_MOVE_ZONE, T_AIM_ZONE, T_AIM_STICK } TargetKind;
typedef struct {
    long id;
    TargetKind kind;
    Control control;
    float x, y;
} Pointer;

static struct {
    Client *client;
    int built;
    HtmlElem zones, move_zone, aim_zone, buttons;
    HtmlElem el[C_COUNT];
    Nipple move_nipple;    // dynamic: created on touch
    Nipple fake_nipple, aim_nipple; // static
    int collection_ids;    // Cs.id
    int nipple_ids;        // Ws.id
    int move_collection, aim_zone_collection, fake_collection, aim_collection;
    Pointer pointers[POINTERS_MAX];
    int pointer_count;
    // CONTROLS state (mhe.init closures)
    int moving;            // Ry: the movement joystick is held
    int m1;                // an aim-zone drag is going on
    int io;                // Io: 0 none, 1 aim-zone look, 2 aim-joystick look
    float cf;              // aim-zone travel since its start (tap-to-jump)
    int look_active;       // r.joystick.active
    float look_x, look_y;  // r.joystick.x / y (raw position)
    float look_dx, look_dy;
    int look_pending;
    int keys_down[UI_CONTROL_COUNT];
    // last applied visibility (redraw)
    int vis[C_COUNT];
    int melee_active;
    char ammo[32];
    int shown;
    int touch_input;       // a real touch has played (strike_touch_event): the touch layer is the input
} T = {.io = 0};

// ---------------------------------------------------------------- FRVR's data

static const StrikeTouch *touch_rules(void) {
    const StrikeRules *r = flavor_current()->rules;
    return r ? &r->touch : &strike_rules()->touch;
}

// Gw's positions and sizes from the extracted table (a side -1 there is unset here)
static void layout_from_rules(void) {
    static int done;
    if (done) return;
    done = 1;
    const StrikeTouch *t = touch_rules();
    for (int c = 0; c < C_COUNT; c++) {
        ControlDef *d = &CONTROLS[c];
        for (size_t i = 0; i < t->control_count; i++) {
            const StrikeTouchControl *s = &t->controls[i];
            if (strcmp(s->id, d->key)) continue;
            d->left = s->left >= 0 ? (float) s->left : NAN;
            d->right = s->right >= 0 ? (float) s->right : NAN;
            d->top = s->top >= 0 ? (float) s->top : NAN;
            d->bottom = s->bottom >= 0 ? (float) s->bottom : NAN;
            if (s->size > 0) d->size = s->size;
            break;
        }
    }
}

static float tap_jump_travel(void) { return (float) touch_rules()->tap_jump_max_travel; } // cf <= 9 is a tap

// ---------------------------------------------------------------- helpers

static float base_scale(void) {
    float w = 0.0f, h = 0.0f;
    html_dom_viewport(&w, &h, NULL);
    if (w <= 0.0f || h <= 0.0f) return 1.0f;
    const StrikeTouch *t = touch_rules();
    return w / h < 16.0f / 9.0f ? w / (float) t->base_width : h / (float) t->base_height; // Pce / --base-scale
}

static void key(const int id, const int down) {
    if (id < 0 || id >= UI_CONTROL_COUNT) return;
    T.keys_down[id] = down;
    ui_controls_controller_key(T.client, id, down);
}

static int game_enabled(void) {
    return T.client && ui_controls_enabled(T.client);
}

static int setting_true(const char *key) {
    char buf[16];
    return html_storage_get(key, buf, sizeof(buf)) >= 0 && !strcmp(buf, "true");
}

static int auto_fire_on(void) { // l(): frvrMenu && settings.tmp.autoFireMbl (a switch: true by default)
    double v;
    if (ui_setting_number("autoFireMbl", &v)) return v != 0.0;
    const cJSON *s = ui_setting_value("autoFireMbl");
    if (cJSON_IsBool(s)) return cJSON_IsTrue(s);
    if (cJSON_IsString(s)) return !strcmp(cJSON_GetStringValue(s), "true");
    const StrikeSetting *def = strike_setting(flavor_current()->rules, "autoFireMbl");
    return def ? def->val != 0.0 : 1;
}

static int melee_selected(void) {
    const Player *me = T.client ? T.client->me : NULL;
    return me && me->weapon && me->weapon->melee;
}

// ---------------------------------------------------------------- nipplejs

static void style(const HtmlElem el, const char *prop, const char *value) {
    if (el) html_dom_set_style(el, prop, value);
}

// Ws: buildEl + stylize (+ $2 for the static position); appended to the zone (addToDom)
static void nipple_build(Nipple *n, const HtmlElem zone, const int collection, const float size, const int dynamic) {
    char html[1536];
    const float s = size;
    const int id = T.nipple_ids++;
    char sz[24], half[24], quarter[24], hneg[24], qneg[24];
    snprintf(sz, sizeof(sz), "%gpx", s);
    snprintf(half, sizeof(half), "%gpx", s / 2.0f);
    snprintf(hneg, sizeof(hneg), "%gpx", -s / 2.0f);
    snprintf(quarter, sizeof(quarter), "%gpx", s / 2.0f);
    snprintf(qneg, sizeof(qneg), "%gpx", -s / 4.0f);
    (void) quarter;
    snprintf(html, sizeof(html),
        "<div class=\"nipple collection_%d\" id=\"nipple_%d_%d\" style=\"position: absolute; opacity: %s; display: block; z-index: 999; transition: opacity 250ms;%s\">"
        "<div class=\"back\" style=\"position: absolute; display: block; width: %s; height: %s; left: 0px; margin-left: %s; margin-top: %s; background: black; opacity: 0.5; border-radius: 50%%;\"></div>"
        "<div class=\"front\" style=\"width: %s; height: %s; position: absolute; display: block; left: 0px; margin-left: %s; margin-top: %s; background: black; opacity: 0.5; transform: translate(0px, 0px); border-radius: 50%%; top: 0px;\"></div>"
        "</div>",
        collection, collection, id, dynamic ? "0" : "0.5", dynamic ? "" : " top: 50%; right: 50%;",
        sz, sz, hneg, hneg, half, half, qneg, qneg);
    html_dom_insert_html(zone, HTML_INSERT_BEFORE_END, html);
    char sel[48];
    snprintf(sel, sizeof(sel), "#nipple_%d_%d", collection, id);
    n->el = html_dom_query(zone, sel);
    n->back = n->el ? html_dom_query(n->el, ".back") : 0;
    n->front = n->el ? html_dom_query(n->el, ".front") : 0;
    n->size = s;
    n->shown = 0;
}

static void nipple_front(const Nipple *n, const float x, const float y, const int animate) {
    char v[64];
    style(n->front, "transition", animate ? "transform 250ms" : "none");
    snprintf(v, sizeof(v), "translate(%gpx,%gpx)", x, y);
    style(n->front, "transform", v);
}

static void nipple_measure(Nipple *n) { // dynamicPage: position = the element's rect
    HtmlRect r;
    if (n->el && html_dom_get_rect(n->el, &r)) {
        n->px = r.x;
        n->py = r.y;
    }
}

static void t_dynamic_removed(void *user) {
    Nipple *n = user;
    n->hide_timer = 0;
    if (n->el) html_dom_remove(n->el); // dynamic: destroy() after the fade
    n->el = n->back = n->front = 0;
}

static void t_static_hidden(void *user) {
    Nipple *n = user;
    n->hide_timer = 0;
    style(n->front, "transition", "none"); // restCallback
}

static void nipple_show(Nipple *n) {
    if (n->hide_timer) html_timer_cancel(n->hide_timer);
    n->hide_timer = 0;
    style(n->front, "transition", "none");
    style(n->el, "opacity", "1"); // setTimeout(0): opacity 1
    n->shown = 1;
}

static void nipple_hide(Nipple *n, const int dynamic) {
    style(n->el, "opacity", dynamic ? "0" : "0.5"); // restOpacity
    nipple_front(n, 0.0f, 0.0f, 1);                 // restJoystick: back to the centre over fadeTime
    n->shown = 0;
    if (n->hide_timer) html_timer_cancel(n->hide_timer);
    n->hide_timer = html_timer_add(FADE_MS, dynamic ? t_dynamic_removed : t_static_hidden, n);
}

// processOnMove: clamp to the circle, move the front; the event data the handlers read
typedef struct {
    float distance, degree; // degree = 180 - f (nipplejs reports the flipped angle)
    float raw_x, raw_y;     // raw.position (the touch, unclamped)
    int dir_x, dir_y;       // -1 left/down, +1 right/up, 0 none (below the threshold)
} StickMove;

static StickMove nipple_move(Nipple *n, const float x, const float y) {
    nipple_measure(n);
    StickMove m = {0};
    const float r = n->size / 2.0f;
    const float dx0 = n->px - x, dy0 = n->py - y;           // Xue(touch, position): position - touch
    const float f = DEG(atan2f(dy0, dx0));
    const float u = sqrtf((x - n->px) * (x - n->px) + (y - n->py) * (y - n->py));
    const float force = r > 0.0f ? u / r : 0.0f;
    const float h = u < r ? u : r;
    const float cx = n->px - h * cosf(RAD(f)), cy = n->py - h * sinf(RAD(f)); // que(position, h, f)
    nipple_front(n, cx - n->px, cy - n->py, 0);
    m.distance = h;
    m.raw_x = x;
    m.raw_y = y;
    m.degree = 180.0f - f;
    if (force > THRESHOLD) { // computeDirection on the radian of f
        const float e = RAD(f);
        m.dir_x = e > -(float) M_PI / 2.0f && e < (float) M_PI / 2.0f ? -1 : 1; // "left" : "right"
        m.dir_y = e > 0.0f ? 1 : -1;                                             // "up" : "down"
    }
    return m;
}

// ---------------------------------------------------------------- CONTROLS handlers (R Q V, H Y j)

static void move_keys(const StickMove *m) { // Q(direction, |angle - 180|, speed)
    const float o = fabsf(m->degree - 180.0f);
    const int *fb = touch_rules()->no_forward_back, *st = touch_rules()->no_strafe;
    if ((o >= (float) fb[2] && o <= (float) fb[3]) || (o >= (float) fb[0] && o <= (float) fb[1])) {
        key(UI_CONTROL_FORWARD, 0);
        key(UI_CONTROL_BACKWARD, 0);
    } else {
        if (m->dir_y > 0) { key(UI_CONTROL_BACKWARD, 0); key(UI_CONTROL_FORWARD, 1); }
        else key(UI_CONTROL_FORWARD, 0);
        if (m->dir_y < 0) { key(UI_CONTROL_FORWARD, 0); key(UI_CONTROL_BACKWARD, 1); }
        else key(UI_CONTROL_BACKWARD, 0);
    }
    if (o >= (float) st[0] && o <= (float) st[1]) {
        key(UI_CONTROL_LEFT, 0);
        key(UI_CONTROL_RIGHT, 0);
    } else {
        if (m->dir_x < 0) { key(UI_CONTROL_RIGHT, 0); key(UI_CONTROL_LEFT, 1); }
        else key(UI_CONTROL_LEFT, 0);
        if (m->dir_x > 0) { key(UI_CONTROL_LEFT, 0); key(UI_CONTROL_RIGHT, 1); }
        else key(UI_CONTROL_RIGHT, 0);
    }
}

static void move_end(void) { // V
    key(UI_CONTROL_FORWARD, 0);
    key(UI_CONTROL_BACKWARD, 0);
    key(UI_CONTROL_LEFT, 0);
    key(UI_CONTROL_RIGHT, 0);
}

static void look_start(void) { T.look_active = 0; } // H
static void look_end(void) { T.look_active = 0; }   // j

static void look_move(const float x, const float y, float *dx, float *dy) { // Y
    const float nx = x - T.look_x, ny = y - T.look_y;
    if (T.look_active) {
        if (game_enabled()) {
            T.look_dx += nx;
            T.look_dy += ny;
            T.look_pending = 1;
        }
    } else {
        T.look_active = 1;
    }
    T.look_x = x;
    T.look_y = y;
    if (dx) *dx = nx;
    if (dy) *dy = ny;
}

int strike_touch_take_look(float *dx, float *dy) {
    if (!T.look_pending) return 0;
    *dx = T.look_dx;
    *dy = T.look_dy;
    T.look_dx = T.look_dy = 0.0f;
    T.look_pending = 0;
    return 1;
}

static void aim_zone_start(void) { // nF.aim.start -> iF
    if (!setting_true("mob_ctrl_disable_tap_jump")) {
        T.cf = 0.0f;
        key(UI_CONTROL_JUMP, 0);
    }
    T.m1 = 1;
    if (T.io == 2) look_end();
    T.io = 1;
    look_start();
}

static void aim_zone_move(const float x, const float y) {
    float dx = 0.0f, dy = 0.0f;
    if (T.io == 1) look_move(x, y, &dx, &dy);
    if (!setting_true("mob_ctrl_disable_tap_jump")) {
        if (T.cf == 0.0f) T.cf += 1.0f;
        else T.cf += fabsf(dx) + fabsf(dy);
    }
}

static void aim_zone_end(void) {
    T.m1 = 0;
    if (T.io == 1) {
        look_end();
        T.io = T.keys_down[UI_CONTROL_AIM] ? 2 : 0;
    }
    if (setting_true("mob_ctrl_disable_tap_jump")) return;
    const float t = T.cf;
    T.cf = 0.0f;
    if (t <= tap_jump_travel()) key(UI_CONTROL_JUMP, 1);
}

static int aim_stick_visible(void); // Gw.aimJoyStick.visible(bh())

static void aim_stick_start(void) {
    if (!aim_stick_visible()) return;
    key(UI_CONTROL_AIM, 1);
    if (!auto_fire_on()) key(UI_CONTROL_SHOOT, 1);
    if (T.io == 1) look_end();
    T.io = 2;
    look_start();
}

static void aim_stick_end(void) {
    if (!aim_stick_visible()) return;
    if (!auto_fire_on()) key(UI_CONTROL_SHOOT, 0);
    key(UI_CONTROL_AIM, 0);
    if (T.io == 2) {
        look_end();
        T.io = T.m1 ? 1 : 0;
    }
}

// ---------------------------------------------------------------- the control buttons (vhe)

static int spectating(void) { return 0; } // Se.isSpectatingOn: KrunkNative spectators keep their own HUD

static int control_visible(const Control c) {
    const Player *me = T.client ? T.client->me : NULL;
    switch (c) {
        case C_JUMP: return !spectating() && setting_true("mob_ctrl_disable_tap_jump");
        // the touch reload ring is the phone's reload button: a keyboard player gets the desktop overlay (strike_hud.c)
        case C_RELOAD: return strike_touch_input() && !spectating() && me && me->weapon && me->weapon->reload_time > 0.0f && !me->weapon->no_reload; // weapon.reload
        case C_MELEE: return !spectating();
        case C_SHOOT: return !spectating() && !T.keys_down[UI_CONTROL_AIM] && !auto_fire_on();
        case C_SHOOT_AIM: return 0;
        case C_FAKE_MOVE: return !T.moving;
        case C_AIM_STICK: return !spectating();
        case C_MENU: return 1; // !Mt.isBotGame
        default: return 1;
    }
}

static int aim_stick_visible(void) { return control_visible(C_AIM_STICK); }

// ---------------------------------------------------------------- the saved layout (Yp / WI)
// FRVR: s5 = getSavedVal("mobile_layout") || "default", WI = JSON(getSavedVal("mobile_layouts") ||
// '{"default": {}}'), Yp = WI[s5]; a control's entry {pos: {right|left, top|bottom}} (base units)
// replaces Gw's pos ({...a, ...Yp[s]}). r5() saves WI; Ahe() empties Yp. Kept in the page storage
// under FRVR's keys (html_storage: localStorage on the web, a file per key natively).
typedef struct { float right, left, top, bottom; } Pos;
static struct {
    int loaded;
    char name[64];       // s5
    cJSON *all;          // WI
    int has[C_COUNT];    // Yp[key] exists
    Pos pos[C_COUNT];
} L;

static Pos default_pos(const Control c) {
    const ControlDef *d = &CONTROLS[c];
    return (Pos) {d->right, d->left, d->top, d->bottom};
}

static float json_num(const cJSON *o, const char *k) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? (float) v->valuedouble : NAN;
}

static void layout_load(void) {
    if (L.loaded) return;
    L.loaded = 1;
    char name[64];
    snprintf(L.name, sizeof(L.name), "%s", html_storage_get("mobile_layout", name, sizeof(name)) >= 0 && name[0] ? name : "default");
    char small[2];
    const int n = html_storage_get("mobile_layouts", small, sizeof(small));
    char *text = n > 0 ? malloc((size_t) n + 1) : NULL;
    if (text) html_storage_get("mobile_layouts", text, (size_t) n + 1);
    L.all = text ? cJSON_Parse(text) : NULL;
    free(text);
    if (!cJSON_IsObject(L.all)) {
        cJSON_Delete(L.all);
        L.all = cJSON_Parse("{\"default\": {}}");
    }
    const cJSON *yp = cJSON_GetObjectItemCaseSensitive(L.all, L.name);
    for (int c = 0; c < C_COUNT; c++) {
        const cJSON *entry = cJSON_GetObjectItemCaseSensitive(yp, CONTROLS[c].key);
        const cJSON *pos = cJSON_GetObjectItemCaseSensitive(entry, "pos");
        L.has[c] = cJSON_IsObject(pos);
        L.pos[c] = L.has[c] ? (Pos) {json_num(pos, "right"), json_num(pos, "left"), json_num(pos, "top"), json_num(pos, "bottom")} : default_pos((Control) c);
    }
}

static Pos layout_pos(const Control c) {
    layout_load();
    return L.has[c] ? L.pos[c] : default_pos(c);
}

static void teardown(void);

// r5(): WI back to the storage, Yp from the edited entries. The in-game layer picks the layout up
// when it is built again.
static void layout_save(void) {
    layout_load();
    cJSON *yp = cJSON_CreateObject();
    for (int c = 0; c < C_COUNT; c++) {
        if (!L.has[c]) continue;
        cJSON *pos = cJSON_CreateObject();
        const Pos *p = &L.pos[c];
        if (!isnan(p->right)) cJSON_AddNumberToObject(pos, "right", p->right);
        if (!isnan(p->left)) cJSON_AddNumberToObject(pos, "left", p->left);
        if (!isnan(p->top)) cJSON_AddNumberToObject(pos, "top", p->top);
        if (!isnan(p->bottom)) cJSON_AddNumberToObject(pos, "bottom", p->bottom);
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddItemToObject(entry, "pos", pos);
        cJSON_AddItemToObject(yp, CONTROLS[c].key, entry);
    }
    if (cJSON_GetObjectItemCaseSensitive(L.all, L.name)) cJSON_ReplaceItemInObjectCaseSensitive(L.all, L.name, yp);
    else cJSON_AddItemToObject(L.all, L.name, yp);
    char *text = cJSON_PrintUnformatted(L.all);
    if (text) html_storage_set("mobile_layouts", text);
    free(text);
    if (T.built) teardown();
}

static void append_pos(StrBuf *sb, const Control c) {
    const Pos p = layout_pos(c);
    if (!isnan(p.right)) sb_appendf(sb, "right: calc(%g * var(--base-scale)); ", p.right);
    if (!isnan(p.left)) sb_appendf(sb, "left: calc(%g * var(--base-scale)); ", p.left);
    if (!isnan(p.top)) sb_appendf(sb, "top: calc(%g * var(--base-scale)); ", p.top);
    if (!isnan(p.bottom)) sb_appendf(sb, "bottom: calc(%g * var(--base-scale)); ", p.bottom);
}

static void apply_pos(const HtmlElem el, const Control c) {
    const Pos p = layout_pos(c);
    const float v[4] = {p.right, p.left, p.top, p.bottom};
    static const char *const SIDES[4] = {"right", "left", "top", "bottom"};
    for (int i = 0; i < 4; i++) {
        char css[48] = "";
        if (!isnan(v[i])) snprintf(css, sizeof(css), "calc(%g * var(--base-scale))", v[i]);
        html_dom_set_style(el, SIDES[i], css);
    }
}

// ---------------------------------------------------------------- Customize Controls (a5, mode Customize)

static struct {
    HtmlElem layer;            // .control-buttons--customize
    HtmlElem el[C_COUNT];
    int moving;                // controlMoving
    Control control;           // controlMovingKey
    float start_x, start_y;
} K = {.control = C_COUNT};

// The captured layer's controls by what they show (the template has no keys): jump / reload / menu
// buttons by icon, the two shoot buttons by side (shoot right, shootWhenAim left), the toggle, the
// two joysticks by class. Each gets the saved position.
void strike_touch_customize_apply(const HtmlElem layer) {
    memset(K.el, 0, sizeof(K.el));
    K.layer = layer;
    K.moving = 0;
    if (!layer) return;
    HtmlElem els[16];
    const int n = html_dom_query_all(layer, ".control-button, .control-toggle, .joystick", els, 16);
    for (int i = 0; i < n; i++) {
        char cls[128] = "", style_text[256] = "", src[96] = "";
        html_dom_get_attr(els[i], "class", cls, sizeof(cls));
        html_dom_get_attr(els[i], "style", style_text, sizeof(style_text));
        const HtmlElem img = html_dom_query(els[i], "img");
        if (img) html_dom_get_attr(img, "src", src, sizeof(src));
        Control c = C_COUNT;
        if (strstr(cls, "joystick--aim")) c = C_AIM_STICK;
        else if (strstr(cls, "joystick")) c = C_FAKE_MOVE;
        else if (strstr(cls, "control-toggle")) c = C_MELEE;
        else if (strstr(src, "jump.svg")) c = C_JUMP;
        else if (strstr(src, "reload.svg")) c = C_RELOAD;
        else if (strstr(src, "menu.svg")) c = C_MENU;
        else if (strstr(src, "shoot.svg")) c = strstr(style_text, "left:") ? C_SHOOT_AIM : C_SHOOT;
        if (c == C_COUNT || K.el[c]) continue;
        K.el[c] = els[i];
        apply_pos(els[i], c);
    }
}

// Ahe(): Yp = WI[s5] = {}, r5()
void strike_touch_layout_reset(void) {
    layout_load();
    memset(L.has, 0, sizeof(L.has));
    layout_save();
    for (int c = 0; c < C_COUNT; c++) {
        if (K.el[c]) apply_pos(K.el[c], (Control) c);
    }
}

// onCustomizeMove: finger travel in base units (s = layer height / 360) moves the control's sides,
// clamped to the layer (a x 360 units, minus the control's size, at least .01), and a control past
// the middle is anchored to the nearer side again
static void customize_move(const float x, const float y) {
    const Control c = K.control;
    const float h = K.layer ? html_dom_offset_height(K.layer) : 0.0f;
    const float w = K.layer ? html_dom_offset_width(K.layer) : 0.0f;
    const float s = (h > 0.0f ? h : 360.0f) / 360.0f;
    const float a = (w > 0.0f ? w : 640.0f) / s, o = 360.0f;
    const float r = (x - K.start_x) / s, l = (y - K.start_y) / s;
    const float size = (float) CONTROLS[c].size;
    Pos *u = &L.pos[c];
#define CLAMP_SIDE(v, d) do { if (!isnan(v) && (v) != 0.0f) { (v) += (d); if ((v) > lim - size) (v) = lim - size; else if ((v) < 0.01f) (v) = 0.01f; } } while (0)
    float lim = a;
    CLAMP_SIDE(u->right, -r);
    CLAMP_SIDE(u->left, r);
    lim = o;
    CLAMP_SIDE(u->top, l);
    CLAMP_SIDE(u->bottom, -l);
#undef CLAMP_SIDE
    if (!isnan(u->right) && u->right > a / 2.0f) {
        u->left = a - u->right - size;
        u->right = NAN;
    } else if (!isnan(u->left) && u->left > a / 2.0f) {
        u->right = a - u->left - size;
        u->left = NAN;
    }
    if (!isnan(u->top) && u->top > o / 2.0f) {
        u->bottom = o - u->top - size;
        u->top = NAN;
    } else if (!isnan(u->bottom) && u->bottom > o / 2.0f) {
        u->top = o - u->bottom - size;
        u->bottom = NAN;
    }
    K.start_x = x;
    K.start_y = y;
    if (K.el[c]) apply_pos(K.el[c], c);
}

// A finger (or the mouse) on the Customize Controls layer: phase 0 down, 1 move, 2 up. 1 = used.
int strike_touch_customize_pointer(const int phase, const float x, const float y) {
    if (!K.layer || strike_screen_top() != STRIKE_SCREEN_CUSTOMIZE_CONTROLS || !strike_screen_root(STRIKE_SCREEN_CUSTOMIZE_CONTROLS)) {
        K.layer = 0;
        K.moving = 0;
        return 0;
    }
    if (phase == 0) {
        const HtmlElem at = html_dom_element_at(x, y);
        for (int c = 0; c < C_COUNT && at; c++) {
            if (!K.el[c]) continue;
            for (HtmlElem e = at; e; e = html_dom_parent(e)) {
                if (e != K.el[c] && e != html_dom_parent(K.el[c])) continue;
                layout_load();
                if (!L.has[c]) { // ontouchstart: Yp[s] || (Yp[s] = {pos: {...a.pos}})
                    L.has[c] = 1;
                    L.pos[c] = default_pos((Control) c);
                }
                K.moving = 1;
                K.control = (Control) c;
                K.start_x = x;
                K.start_y = y;
                return 1;
            }
        }
        return 0;
    }
    if (!K.moving) return 0;
    if (phase == 1) {
        customize_move(x, y);
        return 1;
    }
    K.moving = 0; // ontouchend: controlMoving = null, r5()
    K.control = C_COUNT;
    layout_save();
    printf("[strike] controls layout saved\n");
    return 1;
}

int strike_touch_input(void) { return T.touch_input; }

// $U: the reload button's ammo ring and "cur / max" label (vx = 290). The desktop overlay (strike_hud.c)
// draws the same ring and label.
void strike_touch_ammo_text(char *out, const size_t n, int *offset_pct, int *visible) {
    const Player *me = T.client ? T.client->me : NULL;
    const Weapon *w = me ? me->weapon : NULL;
    int cur = -1, max = -1;
    if (me && w) {
        max = w->ammo > 0 ? (int) w->ammo : -1;
        cur = me->ammo && me->loadout_index >= 0 && me->loadout_index < me->loadout_size ? (int) me->ammo[me->loadout_index] : 0;
        if (w->melee) cur = -1;
    }
    const int reloading = me && me->reload_timer > 0.0f;
    int a = 290;
    if (!reloading) {
        if (max >= 0 && cur >= 0) {
            a = 290 - cur * 290 / max;
            if (a < 0) a = 0;
        } else {
            a = 291;
        }
    } else {
        a = 0;
    }
    *offset_pct = a;
    *visible = max >= 0;
    char c[16], m[16];
    if (cur >= 0 && !reloading) snprintf(c, sizeof(c), "%d", cur); else snprintf(c, sizeof(c), "-");
    if (max >= 0) snprintf(m, sizeof(m), "%d", max); else snprintf(m, sizeof(m), "-");
    snprintf(out, n, "%s / %s", c, m);
}

static void build(void) {
    layout_from_rules();
    const HtmlElem holder = html_dom_by_id("frvrMenuMobileHolder");
    const HtmlElem anchor = holder ? html_dom_query(holder, ".frvr-notifications-holder") : 0;
    if (!anchor) return;
    StrBuf sb = {0};
    // .joystick-zones: the movement zone and the aim zone (visible: false -> opacity 0)
    sb_append(&sb, "<div class=\"joystick-zones\"><div class=\"joystick-zone\" oncontextmenu=\"return false;\" style=\"touch-action: none;\"></div>"
                   "<div class=\"joystick-zone\" oncontextmenu=\"return false;\" style=\"opacity: 0; touch-action: none;\"></div></div>");
    sb_append(&sb, "<div class=\"control-buttons\">");
    for (int c = 0; c < C_COUNT; c++) {
        const ControlDef *d = &CONTROLS[c];
        const int vis = control_visible((Control) c);
        T.vis[c] = vis;
        if (d->type == 0) {
            sb_append(&sb, "<div class=\"control-button\" style=\"");
            append_pos(&sb, (Control) c);
            sb_appendf(&sb, "--control-button-size: %d;%s\" data-kn-control=\"%s\"><img src=\"./img/controls/%s.svg\">", d->size, vis ? "" : " display: none;", d->key, d->icon);
            if (c == C_RELOAD) {
                int off, show;
                strike_touch_ammo_text(T.ammo, sizeof(T.ammo), &off, &show);
                sb_appendf(&sb, "<svg><circle style=\"stroke-dashoffset: %d%%; transition: stroke-dashoffset 0s ease-out; visibility: %s;\"></circle></svg>"
                                "<div class=\"control-button__label\"><div class=\"control-button__label-text\"><span>%s</span></div></div>",
                           off, show ? "visible" : "hidden", T.ammo);
            }
            sb_append(&sb, "</div>");
        } else if (d->type == 1) {
            T.melee_active = melee_selected();
            sb_appendf(&sb, "<div class=\"control-toggle%s\" style=\"", T.melee_active ? " control-toggle--active" : "");
            append_pos(&sb, (Control) c);
            sb_appendf(&sb, "--control-toggle-size: %d;%s\" data-kn-control=\"%s\"><img src=\"./img/controls/%s.svg\"></div>", d->size, vis ? "" : " display: none;", d->key,
                       T.melee_active ? d->icon_active : d->icon);
        } else {
            sb_appendf(&sb, "<div class=\"joystick %s\" style=\"position: absolute; ", d->cls);
            append_pos(&sb, (Control) c);
            sb_appendf(&sb, "--control-joystick-size: %d;%s%s\" data-kn-control=\"%s\"></div>", d->size, vis ? "" : " opacity: 0;",
                       c == C_FAKE_MOVE ? " pointer-events: none;" : "", d->key);
        }
    }
    sb_append(&sb, "</div>");
    char *html = sb_finish(&sb);
    html_dom_insert_html(anchor, HTML_INSERT_BEFORE_BEGIN, html);
    free(html);
    T.zones = html_dom_query(holder, ".joystick-zones");
    T.buttons = html_dom_query(holder, ".control-buttons");
    HtmlElem zones[2] = {0};
    if (T.zones) html_dom_query_all(T.zones, ".joystick-zone", zones, 2);
    T.move_zone = zones[0];
    T.aim_zone = zones[1];
    for (int c = 0; c < C_COUNT; c++) {
        char sel[64];
        snprintf(sel, sizeof(sel), "[data-kn-control=\"%s\"]", CONTROLS[c].key);
        T.el[c] = T.buttons ? html_dom_query(T.buttons, sel) : 0;
        if (T.el[c]) html_dom_remove_attr(T.el[c], "data-kn-control");
    }
    // nipplejs managers in creation order: the two zones (tF.oncreate), then the static joysticks
    const float bs = base_scale();
    T.move_collection = T.collection_ids++;
    T.aim_zone_collection = T.collection_ids++;
    T.fake_collection = T.collection_ids++;
    T.aim_collection = T.collection_ids++;
    if (T.el[C_FAKE_MOVE]) nipple_build(&T.fake_nipple, T.el[C_FAKE_MOVE], T.fake_collection, CONTROLS[C_FAKE_MOVE].size * bs, 0);
    if (T.el[C_AIM_STICK]) nipple_build(&T.aim_nipple, T.el[C_AIM_STICK], T.aim_collection, CONTROLS[C_AIM_STICK].size * bs, 0);
    T.built = 1;
    T.shown = 1;
}

static void teardown(void) {
    if (T.zones) html_dom_remove(T.zones);
    if (T.buttons) html_dom_remove(T.buttons);
    if (T.move_nipple.hide_timer) html_timer_cancel(T.move_nipple.hide_timer);
    if (T.fake_nipple.hide_timer) html_timer_cancel(T.fake_nipple.hide_timer);
    if (T.aim_nipple.hide_timer) html_timer_cancel(T.aim_nipple.hide_timer);
    memset(&T.move_nipple, 0, sizeof(T.move_nipple));
    memset(&T.fake_nipple, 0, sizeof(T.fake_nipple));
    memset(&T.aim_nipple, 0, sizeof(T.aim_nipple));
    T.zones = T.buttons = T.move_zone = T.aim_zone = 0;
    memset(T.el, 0, sizeof(T.el));
    T.built = 0;
    T.shown = 0;
}

// g.redraw(): visibility, the melee toggle, the reload ring
static void redraw(void) {
    for (int c = 0; c < C_COUNT; c++) {
        const int vis = control_visible((Control) c);
        if (vis == T.vis[c] || !T.el[c]) continue;
        T.vis[c] = vis;
        if (CONTROLS[c].type == 2) style(T.el[c], "opacity", vis ? "" : "0");
        else style(T.el[c], "display", vis ? "" : "none");
    }
    const int melee = melee_selected();
    if (melee != T.melee_active && T.el[C_MELEE]) {
        T.melee_active = melee;
        html_dom_set_class(T.el[C_MELEE], "control-toggle--active", melee);
        const HtmlElem img = html_dom_query(T.el[C_MELEE], "img");
        char src[64];
        snprintf(src, sizeof(src), "./img/controls/%s.svg", melee ? CONTROLS[C_MELEE].icon_active : CONTROLS[C_MELEE].icon);
        if (img) html_dom_set_attr(img, "src", src);
    }
    if (T.el[C_RELOAD]) {
        char text[32];
        int off, show;
        strike_touch_ammo_text(text, sizeof(text), &off, &show);
        if (strcmp(text, T.ammo) != 0) {
            snprintf(T.ammo, sizeof(T.ammo), "%s", text);
            const HtmlElem span = html_dom_query(T.el[C_RELOAD], ".control-button__label-text span");
            if (span) html_dom_set_text(span, text);
            const HtmlElem circle = html_dom_query(T.el[C_RELOAD], "circle");
            if (circle) {
                char v[16];
                snprintf(v, sizeof(v), "%d%%", off);
                style(circle, "stroke-dashoffset", v);
                style(circle, "visibility", show ? "visible" : "hidden");
            }
        }
    }
    if (T.el[C_AIM_STICK]) html_dom_set_class(T.el[C_AIM_STICK], "joystick--active", T.aim_nipple.shown);
}

// ---------------------------------------------------------------- pointers

static Pointer *find_pointer(const long id) {
    for (int i = 0; i < T.pointer_count; i++) {
        if (T.pointers[i].id == id) return &T.pointers[i];
    }
    return NULL;
}

static int kind_held(const TargetKind kind) {
    for (int i = 0; i < T.pointer_count; i++) {
        if (T.pointers[i].kind == kind) return 1;
    }
    return 0;
}

static void drop_pointer(const Pointer *p) {
    const int i = (int) (p - T.pointers);
    T.pointers[i] = T.pointers[--T.pointer_count];
}

static TargetKind hit(const float x, const float y, Control *control) {
    const HtmlElem at = html_dom_element_at(x, y);
    if (!at) return T_NONE;
    for (int c = 0; c < C_COUNT; c++) {
        if (!T.el[c]) continue;
        for (HtmlElem e = at; e; e = html_dom_parent(e)) {
            if (e == T.el[c]) {
                *control = (Control) c;
                return c == C_AIM_STICK ? T_AIM_STICK : T_BUTTON;
            }
        }
    }
    for (HtmlElem e = at; e; e = html_dom_parent(e)) {
        if (e == T.move_zone) return T_MOVE_ZONE;
        if (e == T.aim_zone) return T_AIM_ZONE;
    }
    return T_NONE;
}

static void control_down(const Control c) {
    switch (c) {
        case C_MENU: break; // menu.down: nothing
        default: key(CONTROLS[c].bind, 1); break;
    }
}

static void control_up(const Control c) {
    switch (c) {
        case C_MENU: // menu.up: setTimeout(r.toggle(false); setTimeout(r.resetKeys))
            if (T.client) ui_controls_toggle(T.client, 0);
            ui_controls_reset_keys();
            break;
        default: key(CONTROLS[c].bind, 0); break;
    }
}

static int touch_start(const long id, const float x, const float y) {
    if (!T.built || !T.shown || T.pointer_count == POINTERS_MAX || find_pointer(id)) return 0;
    Control c = C_COUNT;
    const TargetKind kind = hit(x, y, &c);
    if (kind == T_NONE) return 0;
    // maxNumberOfNipples 1 per zone / joystick
    if ((kind == T_MOVE_ZONE || kind == T_AIM_ZONE || kind == T_AIM_STICK) && kind_held(kind)) return 1;
    Pointer *p = &T.pointers[T.pointer_count++];
    *p = (Pointer) {.id = id, .kind = kind, .control = c, .x = x, .y = y};
    menu_idle_reset();
    switch (kind) {
        case T_BUTTON:
            control_down(c);
            break;
        case T_MOVE_ZONE: { // dynamic nipple at the touch (the zone's parent is flex: page coordinates)
            if (T.move_nipple.el) {
                if (T.move_nipple.hide_timer) html_timer_cancel(T.move_nipple.hide_timer);
                html_dom_remove(T.move_nipple.el);
                memset(&T.move_nipple, 0, sizeof(T.move_nipple));
            }
            nipple_build(&T.move_nipple, T.move_zone, T.move_collection, CONTROLS[C_FAKE_MOVE].size * base_scale(), 1);
            char v[24];
            snprintf(v, sizeof(v), "%gpx", x);
            style(T.move_nipple.el, "left", v);
            snprintf(v, sizeof(v), "%gpx", y);
            style(T.move_nipple.el, "top", v);
            T.move_nipple.px = x;
            T.move_nipple.py = y;
            T.moving = 1; // R (Ry = true)
            nipple_show(&T.move_nipple);
            const StickMove m = nipple_move(&T.move_nipple, x, y);
            if (m.dir_x || m.dir_y) move_keys(&m);
            break;
        }
        case T_AIM_ZONE:
            aim_zone_start();
            aim_zone_move(x, y);
            break;
        case T_AIM_STICK:
            nipple_show(&T.aim_nipple);
            aim_stick_start();
            (void) nipple_move(&T.aim_nipple, x, y);
            if (T.io == 2) look_move(x, y, NULL, NULL);
            break;
        default: break;
    }
    redraw();
    return 1;
}

static int touch_move(const long id, const float x, const float y) {
    Pointer *p = find_pointer(id);
    if (!p) return 0;
    p->x = x;
    p->y = y;
    switch (p->kind) {
        case T_MOVE_ZONE: {
            const StickMove m = nipple_move(&T.move_nipple, x, y);
            if (m.dir_x || m.dir_y) move_keys(&m);
            break;
        }
        case T_AIM_ZONE:
            aim_zone_move(x, y);
            break;
        case T_AIM_STICK:
            (void) nipple_move(&T.aim_nipple, x, y);
            if (aim_stick_visible() && T.io == 2) look_move(x, y, NULL, NULL);
            break;
        default: break;
    }
    return 1;
}

static int touch_end(const long id) {
    Pointer *p = find_pointer(id);
    if (!p) return 0;
    const Pointer done = *p;
    drop_pointer(p);
    switch (done.kind) {
        case T_BUTTON: control_up(done.control); break;
        case T_MOVE_ZONE:
            nipple_hide(&T.move_nipple, 1);
            T.moving = 0; // V (Ry = false)
            move_end();
            break;
        case T_AIM_ZONE: aim_zone_end(); break;
        case T_AIM_STICK:
            nipple_hide(&T.aim_nipple, 0);
            aim_stick_end();
            break;
        default: break;
    }
    redraw();
    return 1;
}

int strike_touch_event(Client *client, const int phase, const long id, const float x, const float y) {
    T.client = client;
    if (phase == 0) T.touch_input = 1;
    if (strike_touch_customize_pointer(phase > 2 ? 2 : phase, x, y)) return 1; // Customize Controls drags
    if (phase == 0) return touch_start(id, x, y);
    if (phase == 1) return touch_move(id, x, y);
    return touch_end(id);
}

int strike_touch_mouse_button(Client *client, const int button, const int action, const float x, const float y) {
    T.client = client;
    if (button != 0) return 1; // one finger: the left button; the others do nothing on a phone
    if (action == 1) return touch_start(MOUSE_ID, x, y) || 1;
    return touch_end(MOUSE_ID) || 1;
}

int strike_touch_mouse_move(Client *client, const float x, const float y) {
    T.client = client;
    return touch_move(MOUSE_ID, x, y);
}

int strike_touch_sticks_active(void) { // CONTROLS joystick.active || moveJoystick.active
    return T.look_active || T.moving;
}

void strike_touch_reset(void) {
    while (T.pointer_count > 0) touch_end(T.pointers[T.pointer_count - 1].id);
    T.io = 0;
    T.m1 = 0;
    T.moving = 0;
    T.look_active = 0;
    T.look_pending = 0;
    T.look_dx = T.look_dy = 0.0f;
    for (int i = 0; i < UI_CONTROL_COUNT; i++) {
        if (T.keys_down[i]) key(i, 0);
    }
}

#ifdef __EMSCRIPTEN__
// Real touches on the page (FRVR listened on the zones and buttons; nipplejs binds touchstart on
// its zone and touchmove / touchend on the document while active).
static EM_BOOL on_touch(const int type, const EmscriptenTouchEvent *ev, void *user) {
    Client *client = user;
    const int phase = type == EMSCRIPTEN_EVENT_TOUCHSTART ? 0 : type == EMSCRIPTEN_EVENT_TOUCHMOVE ? 1 : 2;
    int used = 0;
    for (int i = 0; i < ev->numTouches; i++) {
        const EmscriptenTouchPoint *t = &ev->touches[i];
        if (!t->isChanged) continue;
        used |= strike_touch_event(client, phase, t->identifier, (float) t->clientX, (float) t->clientY);
    }
    return used ? EM_TRUE : EM_FALSE;
}

static void register_touch(Client *client) {
    static int done;
    if (done) return;
    done = 1;
    emscripten_set_touchstart_callback(EMSCRIPTEN_EVENT_TARGET_DOCUMENT, client, EM_FALSE, on_touch);
    emscripten_set_touchmove_callback(EMSCRIPTEN_EVENT_TARGET_DOCUMENT, client, EM_FALSE, on_touch);
    emscripten_set_touchend_callback(EMSCRIPTEN_EVENT_TARGET_DOCUMENT, client, EM_FALSE, on_touch);
    emscripten_set_touchcancel_callback(EMSCRIPTEN_EVENT_TARGET_DOCUMENT, client, EM_FALSE, on_touch);
}
#endif

// rwe(): the controls exist while Mt.controls.enabled && Mt.mobileControls.visible (the game has
// the input, no kill card: io_killPlayer sets mobileControls.visible = false)
void strike_touch_tick(Client *client) {
    T.client = client;
#ifdef __EMSCRIPTEN__
    register_touch(client);
#endif
    const Player *me = client ? client->me : NULL;
    const int alive = client && client->in_game && me && me->active;
    const int want = !html_ui_is_menu_open() && !hud_ui_end_screen_up() && alive && game_enabled();
    if (want && !T.built) build();
    else if (!want && T.built) {
        strike_touch_reset();
        teardown();
    }
    if (T.built) redraw();
}
