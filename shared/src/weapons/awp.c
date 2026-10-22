#include <weapons.h>

// Krunker v7.1.4 weapon 0 (src/data/weapons.js; values checked against shared/tests/data/v7/weapons.json
// by shared/tests/weapons_v7_test.c). Times are converted from ms to s and
// recoil from per-ms to per-s; fields not ported yet: flap, noAo, type, muzOff, muzMlt, rumble, rumbleDur, icnPad.
const Weapon g_awp = {
    .name = "Sniper Rifle",
    .src = "weapon_1",
    .icon = "icon_1",
    .trail = 1, // v10 trail: the bullet trail on every shot (AWP Blaster, the upgrade, copies it)

    .secondary = 0,
    .no_spread = 0,
    .akimbo = 0,
    .no_aim = 0,
    .no_auto = 1,
    .animate_while_aiming = 1,
    .scope = 1,
    .projectile = 1,
    .projectile_disable = 1,

    .ammo = 3,
    .swap_time = 0.3f,
    .reload_time = 1.9f,
    .aim_speed = 0.12f,
    .speed_mlt = 0.95f,
    .damage = 100.0f,
    .headshot_mlt = 1.5f,
    .pierce = 0.2f,
    .range = 1000.0f,
    .drop_start = 230.0f,
    .damage_drop = 30.0f,
    .rate = 1.0f,
    .spread = 260.0f,
    .recoil = 9.0f,
    .recoil_r = 0.05f,
    .recover = 0.993f,
    .recover_y = 0.997f,
    .recover_f = 0.975f,
    .physical_power = 0.0f,
    .physical_range = 0.0f,
    .zoom = 2.7f,
    .recoil_y_mlt = 0.55f,
    .recoil_z = 1.5f,
    .jump_bob_y_mlt = 0.15f,
    .lean_mlt = 1.5f,
    .inspect_rotation = 0.2f,
    .inspect_mlt = 0.1f,
    .crouch_lean = 0.2f,
    .crouch_rotation = 0.2f,
    .crouch_drop = 0.1f,
    .scale = 0.00115608717587935f,
    .hold_width = 1.0f,
    .burst_rate = 0.05f,

    .origin = {0.0f, -0.55f, -0.8f},
    .offset = {0.8f, -0.65f, -1.8f},
    .left_hold = {0.0f, -0.65f, 2.1f},
    .right_hold = {0.0f, -0.68f, 0.75f},
    // v3.1.1 first-person feel (players.js shoot recoilAnim / swapWeapon swapWiggle / inspect insX)
    .has_recoil_anim = 1,
    .recoil_anim_time = 0.28f,
    .recoil_anim_aim_time = 0.5f,
    .recoil_tween_y = 0.24f,
    .recoil_anim_mask = 1,

    // ---- Krunker v7.1.4 (WP-B) ----
    .sound = "weapon_1",
    .type = 0,
    .mag_reserve = 5,
    .can_upgrade = 1,
    .projectile_config = {.speed = 2.5f, .gravity = 0.0001f, .range = 7000.0f, .damage = 100.0f},
};
