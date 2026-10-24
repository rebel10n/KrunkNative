#include <weapons.h>

// Krunker v7.1.4 weapon 17 (src/data/weapons.js), unchanged in v10.0.3: the ruleset's entry, checked against
// shared/tests/data/v10/weapons.json by shared/tests/weapons_v10_test.c. Times are converted from ms to s and
// recoil from per-ms to per-s; fields not ported yet: transp, noAo, nCase, noCross, nMuz, type, icnPad.
const Weapon g_bomb = {
    .name = "Bomb",
    .src = "weapon_18",
    .icon = "icon_18",

    .no_aim = 1,
    .no_inspect = 1,
    .equipment = 1,
    .bomb = 1,

    .swap_time = 0.25f,
    .aim_speed = 0.14f,
    .speed_mlt = 0.65f,
    .spread = 120.0f,
    .recover = 0.98f,
    .recover_f = 0.98f,
    .zoom = 1.0f,
    .jump_y_mlt = 0.95f,
    .lean_mlt = 0.3f,
    .crouch_lean = 0.0f,
    .crouch_rotation = 0.0f,
    .crouch_drop = 0.12f,
    .scale = 0.63f,
    .hold_width = 1.2f,
    .rotation = {0.0f, 1.5707963267948966f, -0.4f},
    .has_rotation = 1,
    .burst_rate = 0.05f,

    .origin = {0.0001f, 0.0001f, 0.0001f},
    .offset = {0.0f, -0.8f, -3.5f},
    .left_hold = {0.82f, -0.3f, 0.33f},
    .right_hold = {-0.82f, -0.3f, 0.33f},

    // ---- Krunker v7.1.4 (WP-B) ----
    .sound = "weapon_18",
    .type = 3,
    .no_cross = 1,
};
