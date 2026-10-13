#include <weapons.h>

// KrunkNative Tool Gun (WEAPON_ID_TOOL_GUN): not a Krunker v7.1.4 weapon (doc/v7/decisions.md
// "Tool Gun"). The Object Manager's Tools pane equips it; each shot is an OM_ACTION_TOOL request
// for the tool picked there (shared/src/object_manager.c), so player_shoot only runs the fire
// timer, the recoil and the client's tracer: no bullet, no damage, no ammo spent. The Alien
// Blaster's model, sound and feel (weapon 11, shared/src/weapons/alien_blaster.c) with a tint.
const Weapon g_tool_gun = {
    .name = "Tool Gun",
    .src = "weapon_12",
    .icon = "icon_12",

    .no_auto = 1,

    .ammo = 1,
    .swap_time = 0.2f,
    .reload_time = 0.5f,
    .aim_speed = 0.12f,
    .speed_mlt = 1.0f,
    .damage = 0.0f,
    .headshot_mlt = 1.0f,
    .range = 400.0f, // OM_AIM_RANGE: the tracer ends where the tool reaches
    .rate = 0.3f,    // below the Object Manager's request refill (OM_RATE_PER_SECOND)
    .spread = 0.0f,
    .recoil = 1.0f,
    .recoil_r = 0.0f,
    .recover = 0.98f,
    .recover_y = 0.99f,
    .recover_f = 0.98f,
    .zoom = 1.2f,
    .recoil_y_mlt = 0.2f,
    .recoil_z = 1.2f,
    .y_rotation = -0.01f,
    .jump_y_mlt = 0.8f,
    .aim_recoil_mlt = 0.3f,
    .lean_mlt = 1.6f,
    .inspect_rotation = 0.1f,
    .inspect_mlt = 0.8f,
    .crouch_lean = 0.2f,
    .crouch_rotation = 0.2f,
    .scale = 1.1f,
    .hold_width = 1.0f,

    .origin = {0.0f, -0.53f, -3.8f},
    .offset = {1.3f, -0.83f, -4.1f},
    .left_hold = {0.0f, -1.0f, -0.2f},
    .right_hold = {0.0f, -0.65f, -0.2f},
    .has_recoil_anim = 1,
    .recoil_anim_time = 0.2f,
    .recoil_tween_y = 0.16f,
    .recoil_anim_mask = 1,

    .sound = "weapon_12",
    .type = 0,          // not a secondary: bots and the secondary pickers never roll it
    .no_comp = 1,
    .custom_only = 1,   // never a random / onlyGoodWeapons pick
    .kn_tool = 1,
    .tint = 0x46e6ff,   // cyan, so it never reads as the Alien Blaster
};
