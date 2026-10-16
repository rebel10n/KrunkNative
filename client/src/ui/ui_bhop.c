// Bhop: the v3.1.1 offline build's client mod, ported 1:1 from
// /home/user/jails/kru/src/Krunker_v3.1.1/mod/mods.ts (BhopMode, BHOP_MODE_ORDER,
// BHOP_MODE_NAMES, localStorage "bhop", krk_bhop_mode_bind, toggleBhopMode, the
// window.bhop / window.slideHold key state and patchInputs, which mods.ts runs on every
// input frame pushed to CONTROLS.tmpInpts). It only rewrites the frame's jump/crouch
// slots, so prediction and the server see ordinary inputs.
//
// KrunkNative mapping:
//   - patchInputs(inputs) -> ui_bhop_fill_input(client, input), the tail of
//     ui_controls_fill_input (the input frame before main.c queues it); `player` is the
//     predicted local player (client->me) before this input is processed, like `player`
//     when the mod's push hook runs;
//   - window.bhop / window.slideHold (Space / ShiftLeft|ShiftRight held, ignored while an
//     INPUT has focus, cleared on blur / pointer-lock loss) -> the CONTROLS key state for
//     DOM keyCodes 32 / 16 (only set while the game owns the keyboard, reset when the
//     menu opens or the lock is lost);
//   - Date.now() -> the GLFW clock; inputs[iInputs.frame] -> a per-input counter;
//   - notify() -> bhop_toast_show (bhop_toast.c, notify.ts): the bhop toast is the bhop
//     system's alone; the toggle key and /bhop (ui_bhop_announce) are the only callers. The
//     "Developer Tools" bind row and the document keydown matching that calls toggleBhopMode
//     belong to ui_mods.c.
#include <ui_logic.h>
#include <bhop_toast.h>
#include <html_ui.h>
#include <spectate.h> // ui_controls_code_down (CONTROLS.keys by DOM keyCode)
#include <GLFW/glfw3.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BHOP_STORAGE_KEY "bhop"
#define BHOP_BIND_KEY "krk_bhop_mode_bind"
#define BHOP_BIND_DEFAULT "KeyB"
#define BHOP_LEGACY_HOLD_JUMP_CROUCH 4
#define BHOP_DOM_SPACE 32
#define BHOP_DOM_SHIFT 16
#define RAMP_BHOP_DELAY_MS 20.0
#define RAMP_BHOP_DELAY_JITTER_MS 10.0

// BHOP_MODE_ORDER and BHOP_MODE_NAMES
static const struct {
    int mode;
    const char *name;
} BHOP_MODES[] = {
    {UI_BHOP_DISABLED, "Disabled"},
    {UI_BHOP_SLIDEHOP, "Slidehop (while moving)"},
    {UI_BHOP_HOLD_JUMP_CROUCH, "Hold Jump/Crouch"},
    {UI_BHOP_ALWAYS_JUMP, "Always Jump (while moving)"},
    {UI_BHOP_HOLD_JUMP, "Hold Jump"},
};

// The names the toggle notice shows (KrunkNative strings: kn.bhop.*); ui_bhop_mode_name and the
// console log keep the English names.
static const char *bhop_shown_name(const int mode) {
    switch (mode) {
        case UI_BHOP_SLIDEHOP: return I18N("kn.bhop.slidehop", "Slidehop (while moving)");
        case UI_BHOP_HOLD_JUMP_CROUCH: return I18N("kn.bhop.holdJumpCrouch", "Hold Jump/Crouch");
        case UI_BHOP_ALWAYS_JUMP: return I18N("kn.bhop.alwaysJump", "Always Jump (while moving)");
        case UI_BHOP_HOLD_JUMP: return I18N("kn.bhop.holdJump", "Hold Jump");
        default: return I18N("kn.bhop.disabled", "Disabled");
    }
}
#define BHOP_MODE_COUNT ((int) (sizeof(BHOP_MODES) / sizeof(BHOP_MODES[0])))

static struct {
    int loaded;
    int mode;                // window.bhopMode
    char bind[32];           // window.bhopModeKey
    // patchInputs state
    double zero_some, next_zero_some;
    int did_crouch;          // bhopDidCrouch
    int state;               // bhopState
    double ramp_timer;       // rampBhopTimer (ms, GLFW clock)
    unsigned ramp_start;     // rampBhopStart (input frame)
    unsigned frame;          // inputs[iInputs.frame]
} g_bhop;

static int order_index(const int mode) {
    for (int i = 0; i < BHOP_MODE_COUNT; i++) {
        if (BHOP_MODES[i].mode == mode) return i;
    }
    return -1;
}

// bhopRandom(min, max, isFloat)
static double bhop_random(const double min, const double max, const int is_float) {
    const double v = (double) rand() / ((double) RAND_MAX + 1.0) * (max - min) + min;
    return is_float ? v : floor(v);
}

// pickZeroSome(): the falling velocity below which the slide crouch starts
static double pick_zero_some(void) {
    return bhop_random(-0.015, 0.005, 1);
}

static void store_mode(void) {
    char text[16];
    snprintf(text, sizeof(text), "%d", g_bhop.mode);
    html_storage_set(BHOP_STORAGE_KEY, text);
}

// window.bhopMode = Number(localStorage.getItem("bhop")) (+ the legacy 4 migration and
// the isBhopMode fallback); window.bhopModeKey = getSavedVal("krk_bhop_mode_bind") ?? "KeyB".
static void bhop_load(void) {
    if (g_bhop.loaded) return;
    g_bhop.loaded = 1;
    g_bhop.zero_some = pick_zero_some();
    g_bhop.next_zero_some = pick_zero_some();

    char text[64];
    g_bhop.mode = UI_BHOP_DISABLED;
    if (html_storage_get(BHOP_STORAGE_KEY, text, sizeof(text)) >= 0) {
        char *end;
        const double v = strtod(text, &end);
        const int number = end != text || !text[0]; // Number("") === 0
        if (number && v == BHOP_LEGACY_HOLD_JUMP_CROUCH) {
            g_bhop.mode = UI_BHOP_HOLD_JUMP_CROUCH;
            store_mode();
        } else if (number && v == floor(v) && order_index((int) v) >= 0) {
            g_bhop.mode = (int) v;
        }
    }
    if (html_storage_get(BHOP_BIND_KEY, text, sizeof(text)) >= 0) snprintf(g_bhop.bind, sizeof(g_bhop.bind), "%s", text);
    else snprintf(g_bhop.bind, sizeof(g_bhop.bind), "%s", BHOP_BIND_DEFAULT);
}

int ui_bhop_mode(void) {
    bhop_load();
    return g_bhop.mode;
}

void ui_bhop_set_mode(const int mode) {
    bhop_load();
    g_bhop.mode = mode == BHOP_LEGACY_HOLD_JUMP_CROUCH ? UI_BHOP_HOLD_JUMP_CROUCH : order_index(mode) >= 0 ? mode : UI_BHOP_DISABLED;
    store_mode();
}

const char *ui_bhop_mode_name(const int mode) {
    const int i = order_index(mode);
    return BHOP_MODES[i >= 0 ? i : 0].name;
}

// The mode message, "Bhop - <current mode>" (the names the toggle notice shows), into `text`,
// and the bhop toast. Both the toggle key and /bhop come through here (v3.1.1: /bhop's chat
// line and its window.notify(msg) carry the same text).
void ui_bhop_announce(char *text, const size_t size) {
    bhop_load();
    char shown[192];
    I18N_F(shown, "kn.bhop.notify", "Bhop - {0}", bhop_shown_name(g_bhop.mode));
    snprintf(text, size, "%s", shown);
    bhop_toast_show(text);
}

// toggleBhopMode()
void ui_bhop_toggle(void) {
    bhop_load();
    const int index = order_index(g_bhop.mode);
    g_bhop.mode = BHOP_MODES[((index >= 0 ? index : 0) + 1) % BHOP_MODE_COUNT].mode;
    store_mode();
    char msg[192];
    ui_bhop_announce(msg, sizeof(msg));
    printf("Bhop - %s\n", ui_bhop_mode_name(g_bhop.mode)); // console.log(msg): the English name
}

const char *ui_bhop_bind(void) {
    bhop_load();
    return g_bhop.bind;
}

// setBhopModeBind(code)
void ui_bhop_set_bind(const char *code) {
    bhop_load();
    snprintf(g_bhop.bind, sizeof(g_bhop.bind), "%s", code ? code : "");
    html_storage_set(BHOP_BIND_KEY, g_bhop.bind);
}

/* ------------------------------------------------------------ patchInputs */

static void reset_ramp_bhop_timer(void) {
    g_bhop.ramp_timer = 0.0;
    g_bhop.ramp_start = 0;
}

// isOnRampObject(lp): standing in a ramp object's box below its top, under or on its
// slope (or still on the ground from the last input without a jump).
static int is_on_ramp_object(const Player *lp) {
    const Map *map = lp->game ? lp->game->map : NULL;
    const int was_on_ground = lp->on_ground && !lp->did_jump;
    for (size_t i = 0; map && i < map->object_count; i++) {
        const Object *o = map->objects[i];
        if (!o || !o->active || !o->ramp) continue;
        // collidesRampObject: x/y/z centre, width/height/length half extents
        const float half_w = o->scale.x * 0.5f, half_h = o->scale.y * 0.5f, half_l = o->scale.z * 0.5f;
        const float cy = o->position.y + half_h;
        if (!(lp->position.x - lp->scale < o->position.x + half_w && lp->position.x + lp->scale > o->position.x - half_w &&
              lp->position.z - lp->scale < o->position.z + half_l && lp->position.z + lp->scale > o->position.z - half_l &&
              lp->position.y <= cy + half_h && lp->position.y + lp->height >= cy - half_h)) continue;
        if (lp->position.y >= cy + half_h) continue;
        const float dir = (float) M_PI * 0.5f * (float) o->direction;
        const vec2 point = {lp->position.x + lp->scale * cosf(dir), lp->position.z + lp->scale * sinf(dir)};
        const float progress = fmaxf(0.0f, fminf(1.0f, progress_on_line(o->ramp->start, o->ramp->end, point)));
        const float ramp_y = cy - half_h + (half_h * 2.0f + 0.01f) * progress;
        if (lp->position.y <= ramp_y || was_on_ground) return 1;
    }
    return 0;
}

static int is_on_ramp(const Player *lp) {
    return lp && lp->on_ground && is_on_ramp_object(lp);
}

// patchRampBhopJump: on a ramp the jump is pressed for a short random window instead of
// being toggled every input.
static void patch_ramp_bhop_jump(Input *input) {
    const double now = glfwGetTime() * 1000.0;
    const unsigned frame = g_bhop.frame;
    if (g_bhop.ramp_timer <= now) reset_ramp_bhop_timer();
    if (!g_bhop.ramp_timer) {
        g_bhop.ramp_timer = now + RAMP_BHOP_DELAY_MS + bhop_random(0.0, RAMP_BHOP_DELAY_JITTER_MS, 0);
        g_bhop.ramp_start = frame + (unsigned) bhop_random(1.0, 4.0, 0);
        input->jump = 0;
        return;
    }
    input->jump = frame >= g_bhop.ramp_start && now < g_bhop.ramp_timer;
}

void ui_bhop_fill_input(Client *client, Input *input) {
    bhop_load();
    const Player *lp = client ? client->me : NULL;
    if (!input || !lp || !lp->active) return; // no input frame is pushed without a live player
    g_bhop.frame++;
    if (lp->noclip) {
        g_bhop.state = 0;
        reset_ramp_bhop_timer();
        return;
    }
    const int mode = g_bhop.mode;
    if (mode == UI_BHOP_DISABLED) {
        g_bhop.state = 0;
        reset_ramp_bhop_timer();
        return;
    }

    const int moving = input->move_dir != -1;
    const int jump_held = input->jump || ui_controls_code_down(BHOP_DOM_SPACE);     // || window.bhop
    const int crouch_held = input->crouch || ui_controls_code_down(BHOP_DOM_SHIFT); // || window.slideHold
    const int auto_bhop = (mode == UI_BHOP_SLIDEHOP || mode == UI_BHOP_ALWAYS_JUMP) && moving;
    const int hold_bhop = (mode == UI_BHOP_HOLD_JUMP_CROUCH || mode == UI_BHOP_HOLD_JUMP) && jump_held;

    if (auto_bhop || hold_bhop) {
        if (is_on_ramp(lp)) {
            g_bhop.state = 0;
            patch_ramp_bhop_jump(input);
        } else {
            reset_ramp_bhop_timer();
            g_bhop.state ^= 1;
            input->jump = (unsigned char) g_bhop.state;
        }
    } else {
        g_bhop.state = 0;
        reset_ramp_bhop_timer();
    }

    // Slidehop crouch: SLIDEHOP always, HOLD_JUMP_CROUCH only while crouch is held,
    // HOLD_JUMP never.
    const int want_slidehop = mode == UI_BHOP_SLIDEHOP || (mode == UI_BHOP_HOLD_JUMP_CROUCH && crouch_held);
    if (!want_slidehop) {
        g_bhop.did_crouch = 0;
        return;
    }
    const int on_ground = lp->on_ground;
    if (on_ground && !input->jump && crouch_held) {
        input->crouch = 1;
        g_bhop.did_crouch = 1;
        return;
    }
    if (!g_bhop.did_crouch) g_bhop.zero_some = g_bhop.next_zero_some;
    const int will_crouch = !on_ground && lp->velocity.y < g_bhop.zero_some;
    input->crouch = (unsigned char) will_crouch;
    if (!g_bhop.did_crouch && will_crouch) g_bhop.next_zero_some = pick_zero_some();
    g_bhop.did_crouch = will_crouch;
}
