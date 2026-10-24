#include <weapons.h>

// Krunker v7.1.4 weapon 9 (src/data/weapons.js): the base of its upgradeWeapons entry (upgraded.c); v10
// plays the rebalanced copy in shared/src/rules/v10/weapons_rebalance_v10.c. Times are converted from ms to s and
// recoil from per-ms to per-s; fields not ported yet: noAo, altSkin, type, caseYOff, caseZOff, muzOff, movSpread, expScale, rumble, icnPad.
const Weapon g_akimbo = {
    .name = "Akimbo Uzi",
    .src = "weapon_10",
    .icon = "icon_10",

    .secondary = 0,
    .no_spread = 0,
    .akimbo = 1,
    .no_aim = 1,
    .no_auto = 0,
    .no_inspect = 1,

    .ammo = 28,
    .swap_time = 0.3f,
    .reload_time = 1.3f,
    .aim_speed = 0.12f,
    .speed_mlt = 1.04f,
    .damage = 15.0f,
    .headshot_mlt = 1.0f,
    .pierce = 1.0f,
    .range = 500.0f,
    .damage_drop = 13.0f,
    .rate = 0.07f,
    .min_spread = 10.0f,
    .recoil = 3.4f,
    .recoil_r = 0.015f,
    .recover = 0.978f,
    .recover_y = 0.996f,
    .recover_f = 0.975f,
    .physical_power = 0.0f,
    .physical_range = 0.0f,
    .zoom = 1.5f,
    .recoil_y_mlt = 0.7f,
    .recoil_z = 5.0f,
    .z_lean_mlt = 0.4f,
    .lean_mlt = 0.6f,
    .crouch_lean = 0.1f,
    .crouch_rotation = 0.1f,
    .crouch_drop = 0.2f,
    .scale = 0.9f,
    .hold_width = 1.3f,
    .burst_rate = 0.05f,

    .origin = {0.0f, -0.62f, -2.5f},
    .offset = {1.5f, -0.95f, -3.3f},
    .left_hold = {-0.1f, -0.55f, 0.2f},
    .right_hold = {-0.1f, -0.55f, 0.2f},
    // v3.1.1 first-person feel (players.js shoot recoilAnim / swapWeapon swapWiggle / inspect insX)
    .has_recoil_anim = 1,
    .recoil_tween_y = 0.01f,
    .recoil_anim_mask = 1,

    // ---- Krunker v7.1.4 (WP-B) ----
    .sound = "weapon_10",
    .type = 0,
    .mag_reserve = 4,
    .move_spread = 0.01f,
    .can_upgrade = 1,
};
