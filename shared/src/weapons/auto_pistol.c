#include <weapons.h>

// Krunker v7.1.4 weapon 16 (src/data/weapons.js), unchanged in v10.0.3: the ruleset's entry, checked against
// shared/tests/data/v10/weapons.json by shared/tests/weapons_v10_test.c. Times are converted from ms to s and
// recoil from per-ms to per-s; fields not ported yet: minRec, noAo, kill, type, muzOff, muzMlt, rumble, icnPad, expScale.
const Weapon g_auto_pistol = {
    .name = "Auto Pistol",
    .src = "weapon_17",
    .icon = "icon_17",

    .secondary = 1,
    .no_spread = 0,
    .akimbo = 0,
    .no_aim = 0,
    .no_auto = 0,

    .ammo = 15,
    .swap_time = 0.2f,
    .reload_time = 1.0f,
    .aim_speed = 0.1f,
    .speed_mlt = 1.0f,
    .damage = 16.0f,
    .headshot_mlt = 1.1f,
    .pierce = 0.95f,
    .range = 700.0f,
    .damage_drop = 2.0f,
    .rate = 0.1f,
    .spread = 150.0f,
    .recoil = 4.0f,
    .recoil_r = 0.01f,
    .recover = 0.98f,
    .recover_y = 0.99f,
    .recover_f = 0.98f,
    .physical_power = 0.0f,
    .physical_range = 0.0f,
    .zoom = 1.3f,
    .recoil_y_mlt = 0.2f,
    .recoil_z = 3.8f,
    .recoil_z_mlt = -0.4f,
    .y_rotation = -0.005f,
    .jump_y_mlt = 0.5f,
    .aim_recoil_mlt = 0.4f,
    .lean_mlt = 1.6f,
    .inspect_rotation = 0.35f,
    .inspect_mlt = 0.9f,
    .crouch_lean = 0.3f,
    .crouch_rotation = 0.3f,
    .scale = 0.0003158947055276376f,
    .hold_width = 1.35f,
    .burst_rate = 0.05f,

    .origin = {0.0f, -0.23f, -3.9f},
    .offset = {1.2f, -0.6f, -3.7f},
    .left_hold = {0.0f, -1.1f, -0.3f},
    .right_hold = {0.13f, -0.62f, -0.32f},
    // v3.1.1 first-person feel (players.js shoot recoilAnim / swapWeapon swapWiggle / inspect insX)
    .has_recoil_anim = 1,
    .recoil_anim_time = 0.2f,
    .recoil_tween_y = 0.28f,
    .recoil_anim_mask = 1,

    // ---- Krunker v7.1.4 (WP-B) ----
    .sound = "weapon_17",
    .type = 1,
    .kill_score = 50,
    .min_rec = 30,
    .mag_reserve = 4,
    .can_upgrade = 1,
};
