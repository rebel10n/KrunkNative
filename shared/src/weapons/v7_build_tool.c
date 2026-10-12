#include <weapons.h>

// Krunker v7.1.4 weapon 19 (src/data/weapons.js), generated from shared/tests/data/v7/weapons.json
// (tools/kref/v7-dump.js) with the same field mapping as weapons 0-18. Times ms -> s, recoil per ms -> per s.
const Weapon g_build_tool = {
    .name = "Build Tool",
    .src = "weapon_20",
    .icon = "icon_20",

    .no_inspect = 1,
    .equipment = 1,
    .has_rotation = 1,

    .burst_rate = 0.05f,
    .swap_time = 0.2f,
    .aim_speed = 0.1f,
    .speed_mlt = 0.9f,
    .range = 50.0f,
    .rate = 0.3f,
    .spread = 120.0f,
    .recover = 0.98f,
    .recover_f = 0.98f,
    .zoom = 1.1f,
    .jump_y_mlt = 0.95f,
    .lean_mlt = 0.3f,
    .aim_lean = 0.000001f,
    .crouch_drop = 0.12f,
    .scale = 1.2f,
    .hold_width = 1.3f,
    .rotation = {0.0f, 1.5707963267948966f, -0.4f},

    .origin = {0.00001f, -3.8f, -3.2f},
    .offset = {0.0f, -2.6f, -3.5f},
    .left_hold = {2.2f, 0.6f, 0.1f},
    .right_hold = {-2.2f, 0.6f, 0.1f},

    // ---- Krunker v7.1.4 (WP-B) ----
    .sound = "weapon_20",
    .type = 3,
    .build = 1,
    .no_cross = 1,
};
