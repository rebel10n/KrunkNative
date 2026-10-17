// Krunker Strike's damage numbers and Dynamic crosshair: the rules of FRVR 1.37.2's bundle (the spawn,
// step and draw of newDmgNums / dmgNums, and the crosshair's updateCrosshair and type 1 draw), see strike_hud_fx.h.
#include <strike_hud_fx.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// FRVR's constants, by name in the 1.37.2 bundle
#define DMG_LIFE_STACKING 1000.0    // newDmgVal: l = 1e3 (the lookup by id and the height 1.25 * playerHeight)
#define DMG_LIFE_INDIVIDUAL 750.0   // newDmgVal: l = 750, height playerHeight
#define DMG_LIFE_LEGACY 1000.0      // dmgVal: life 1e3, y + 7
#define DMG_LEGACY_Y 7.0
#define DMG_RISE_SLOW 0.001         // eye: the first half of life, per ms
#define DMG_RISE_FAST 0.025         // tye: the second half, per ms
#define DMG_LEGACY_RISE 0.022       // dmgVal's drift up, per ms
#define DMG_LEGACY_DRIFT 0.013      // dmgVal's drift sideways, per ms
#define DMG_BOOST_RANGE 15.0        // a number within 15 units (horizontally) of a new Individual one is lifted
#define DMG_BOOST_MIN 2.0           // V.lerp(2, 15, w * zoom / 300)
#define DMG_BOOST_MAX 15.0
#define DMG_BOOST_SCALE 300.0
#define DMG_OFFSET_MAX 0.03         // the screen nudge is capped at 3 % of the page

static double clampd(const double v, const double lo, const double hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

static double min_d(const double a, const double b) {
    return a < b ? a : b;
}

// FRVR's V.interpolateColors (y8): per channel round(a + (b - a) * t), "#rrggbb"
static void mix_hex(const char *a, const char *b, const double t, char *out, const size_t n) {
    unsigned ca[3], cb[3];
    for (int i = 0; i < 3; i++) {
        char pa[3] = {a[1 + 2 * i], a[2 + 2 * i], 0}, pb[3] = {b[1 + 2 * i], b[2 + 2 * i], 0};
        ca[i] = (unsigned) strtoul(pa, NULL, 16);
        cb[i] = (unsigned) strtoul(pb, NULL, 16);
    }
    unsigned c[3];
    for (int i = 0; i < 3; i++) c[i] = (unsigned) lround(ca[i] + (cb[i] - (double) ca[i]) * t);
    snprintf(out, n, "#%02x%02x%02x", c[0], c[1], c[2]);
}

void strike_damage_reset(StrikeDamageSet *set) {
    if (!set) return;
    memset(set, 0, sizeof(*set));
}

// The slot for a new number: a free one, else the one closest to its end (FRVR's array grows; the HUD caps it).
static StrikeDamageNum *free_slot(StrikeDamageSet *set) {
    StrikeDamageNum *best = &set->nums[0];
    for (int i = 0; i < STRIKE_DMG_MAX; i++) {
        StrikeDamageNum *n = &set->nums[i];
        if (!n->used) return n;
        if (n->life < best->life) best = n;
    }
    return best;
}

StrikeDamageNum *strike_damage_add(StrikeDamageSet *set, const int mode, const int key, const float x, const float feet_y, const float z,
                                   const double value, const int crit, const float player_height, const float legacy_dir) {
    StrikeDamageNum *num = NULL;
    if (mode == STRIKE_DMG_LEGACY) {
        // dmgVal: its own list, no key, no merge
        num = free_slot(set);
        memset(num, 0, sizeof(*num));
        num->used = 1;
        num->mode = STRIKE_DMG_LEGACY;
        num->key = STRIKE_DMG_KEY_AI;
        num->serial = ++set->serial;
        num->initial_life = num->life = DMG_LIFE_LEGACY;
        num->x = x;
        num->y = feet_y + (float) DMG_LEGACY_Y;
        num->z = z;
        num->dir = legacy_dir;
        num->value = fabs(value);
        num->crit = crit;
        return num;
    }

    // newDmgVal: Stacking looks for the victim's live number (its key), Individual never does
    const int stacking = mode != STRIKE_DMG_INDIVIDUAL;
    if (stacking) {
        for (int i = 0; i < STRIKE_DMG_MAX && !num; i++) {
            StrikeDamageNum *n = &set->nums[i];
            if (n->used && n->key == key && n->mode != STRIKE_DMG_LEGACY) num = n;
        }
    }
    const int fresh = num == NULL;
    if (fresh) {
        num = free_slot(set);
        memset(num, 0, sizeof(*num));
        num->used = 1;
        num->mode = stacking ? STRIKE_DMG_STACKING : STRIKE_DMG_INDIVIDUAL;
        num->key = key;
        num->serial = ++set->serial;
        num->initial_life = stacking ? DMG_LIFE_STACKING : DMG_LIFE_INDIVIDUAL;
        num->value = 0.0;
    }
    const double height = stacking ? 1.25 * player_height : player_height;
    num->crit = crit;
    num->life = num->initial_life;
    num->x = x;
    num->y = feet_y + (float) height;
    num->z = z;
    num->value += fabs(value);
    num->needs_adjust = 1;
    if (!stacking) {
        // the Individual numbers near the new one are lifted apart once
        for (int i = 0; i < STRIKE_DMG_MAX; i++) {
            StrikeDamageNum *n = &set->nums[i];
            if (!n->used || n == num || n->mode != STRIKE_DMG_INDIVIDUAL) continue;
            const double dx = n->x - num->x, dz = n->z - num->z;
            if (sqrt(dx * dx + dz * dz) < DMG_BOOST_RANGE) n->height_boost = 1;
        }
    }
    return num;
}

void strike_damage_step(StrikeDamageSet *set, double dt_ms, const float cam_x, const float cam_y, const float cam_z, const float zoom,
                        const float player_scale, const float player_height) {
    if (dt_ms < 0.0) dt_ms = 0.0;
    const double zoom_v = zoom > 0.0f ? zoom : 1.0;
    for (int i = 0; i < STRIKE_DMG_MAX; i++) {
        StrikeDamageNum *n = &set->nums[i];
        if (!n->used) continue;
        n->life -= dt_ms;
        if (n->life <= 0.0) {
            n->life = 0.0;
            n->used = 0;
            continue;
        }
        if (n->mode == STRIKE_DMG_LEGACY) {
            n->y += (float) (DMG_LEGACY_RISE * dt_ms);
            n->x += (float) (DMG_LEGACY_DRIFT * dt_ms * cos(n->dir));
            n->z += (float) (DMG_LEGACY_DRIFT * dt_ms * sin(n->dir));
            continue;
        }
        const double half = n->initial_life * 0.5;
        // aie: slow for the first half of life, fast for the second
        n->y += (float) ((n->life > half ? DMG_RISE_SLOW : DMG_RISE_FAST) * dt_ms);
        if (n->height_boost) {
            n->height_boost = 0;
            const double dx = cam_x - n->x, dz = cam_z - n->z;
            const double w = sqrt(dx * dx + dz * dz);
            const double t = w * (zoom_v > 1.0 ? 1.1 : 1.0) / DMG_BOOST_SCALE;
            n->y += (float) (DMG_BOOST_MIN + (DMG_BOOST_MAX - DMG_BOOST_MIN) * t);
        }
        if (n->needs_adjust) {
            n->needs_adjust = 0;
            const double dx = cam_x - n->x, dy = cam_y - n->y, dz = cam_z - n->z;
            const double w = sqrt(dx * dx + dy * dy + dz * dz);
            if (w > 0.0) {
                n->offset_x = (float) min_d(DMG_OFFSET_MAX, player_scale * 0.5 / (75.0 / w * 100.0) * (zoom_v * 2.0));
                if (w > 30.0) n->offset_y = (float) min_d(DMG_OFFSET_MAX, player_height * 0.2 / (50.0 / w * 100.0) * (zoom_v * 2.0));
            }
        }
    }
}

double strike_damage_alpha(const StrikeDamageNum *num) {
    if (!num || num->mode == STRIKE_DMG_LEGACY || num->initial_life <= 0.0) return 1.0;
    const double half = num->initial_life * 0.5;
    return clampd(num->life / half, 0.0, 1.0);
}

void strike_damage_color(const StrikeDamageNum *num, const int stroke, char *out, const size_t n) {
    // t: 1 at the hit (life 2 * half), 0 from the half on: the colours run from the hit's to the rest's
    const double half = num->initial_life * 0.5;
    const double t = half > 0.0 ? clampd((num->life - half) / half, 0.0, 1.0) : 0.0;
    // 1.37.2: fill interpolateColors(crit ? Jwe : qwe, crit ? Ywe : Kwe, t), stroke ($we : Zwe, Xwe : jwe, t)
    if (num->crit) {
        if (stroke) mix_hex("#5f0000", "#D40B0B", t, out, n); // $we -> Xwe
        else mix_hex("#ffeaa0", "#FFD542", t, out, n);       // Jwe -> Ywe
    } else {
        if (stroke) mix_hex("#000000", "#8b0000", t, out, n); // Zwe -> jwe
        else mix_hex("#ffffff", "#f87c7c", t, out, n);        // qwe -> Kwe
    }
}

void strike_damage_text(const double value, char *out, const size_t n) {
    char tmp[32];
    snprintf(tmp, sizeof(tmp), "%.1f", fabs(value));
    // "40.0" -> "40", "33.5" stays
    size_t len = strlen(tmp);
    if (len > 2 && tmp[len - 2] == '.' && tmp[len - 1] == '0') tmp[len - 2] = 0;
    snprintf(out, n, "%s", tmp);
}

const char *strike_damage_mode_label(const int mode) {
    switch (mode) {
    case STRIKE_DMG_STACKING: return "Stacking";
    case STRIKE_DMG_INDIVIDUAL: return "Individual";
    case STRIKE_DMG_LEGACY: return "Legacy";
    default: return "";
    }
}

float strike_damage_font_px(const StrikeDamageNum *num, const float dmg_scale) {
    const float scale = dmg_scale > 0.0f ? dmg_scale : 1.0f;
    // Legacy's draw (dmgNums) has no crit size; Stacking / Individual (newDmgNums) do
    const int crit_up = num && num->crit && num->mode != STRIKE_DMG_LEGACY;
    return 30.0f * scale * (crit_up ? 1.1f : 1.0f);
}

float strike_crosshair_scale(const float spread, const float page_height) {
    const double qr = page_height > 0.0f ? page_height / 693.0 : 1.0;
    const double r = (double) spread * 0.85 * qr;
    return (float) (r > 58.0 ? r : 58.0);
}

void strike_crosshair_span(const float scale, float *near, float *far) {
    // FRVR's type 1 lines: rect(-3, -(3 + J), 6, 23) rotated by 0 / 90 / 180 / -90
    const float j = scale / 2.0f - 2.0f;
    if (near) *near = j - 20.0f;
    if (far) *far = j + 3.0f;
}

float strike_crosshair_opacity(const float aim_val, const int inspecting, const int no_cross, const float reload_timer) {
    // FRVR's aimVal is 1 at the hip and falls to 0 when aimed; KN's aim_val is the reverse
    const double aim_v = 1.0 - (double) aim_val;
    if (inspecting || no_cross || reload_timer > 0.0f) return 0.0f;
    return (float) clampd(aim_v, 0.0, 1.0);
}
