#include <weapons.h>

// Krunker v7.1.4 weapon 20 (src/data/weapons.js), generated from shared/tests/data/v7/weapons.json
// (tools/kref/v7-dump.js) with the same field mapping as weapons 0-18. Times ms -> s, recoil per ms -> per s.
const Weapon g_grappler = {
    .name = "Grappler",
    .src = "weapon_21",
    .icon = "icon_21",

    .secondary = 1,
    .no_spread = 1,
    .no_auto = 1,

    .burst_rate = 0.05f,
    .ammo = 10,
    .swap_time = 0.2f,
    .reload_time = 1.0f,
    .aim_speed = 0.11f,
    .speed_mlt = 1.0f,
    .damage = 1.0f,
    .headshot_mlt = 1.5f,
    .pierce = 0.85f,
    .range = 700.0f,
    .rate = 0.3f,
    .recoil = 10.0f,
    .recoil_r = 0.01f,
    .recover = 0.982f,
    .recover_y = 0.995f,
    .recover_f = 0.98f,
    .zoom = 1.2f,
    .recoil_y_mlt = 0.62f,
    .recoil_z = 4.8f,
    .recoil_z_mlt = 0.2f,
    .jump_y_mlt = 0.5f,
    .aim_recoil_mlt = 0.23f,
    .lean_mlt = 1.6f,
    .inspect_rotation = 0.35f,
    .inspect_mlt = 0.9f,
    .crouch_lean = 0.3f,
    .crouch_rotation = 0.3f,
    .scale = 1.0f,
    .hold_width = 1.1f,

    .origin = {0.0f, -0.195f, -3.8f},
    .offset = {1.0f, -0.5f, -4.1f},
    .left_hold = {0.0f, -0.9f, -0.5f},
    .right_hold = {0.0f, -0.7f, -0.5f},
    .has_recoil_anim = 1,
    .recoil_anim_time = 0.18f,
    .recoil_tween_y = 0.34f,
    .recoil_anim_mask = 1,

    // ---- Krunker v7.1.4 (WP-B) ----
    .sound = "weapon_21",
    .type = 1,
    .kill_score = 50,
    .custom_only = 1,
    .grapple_range = 500.0f,
};
