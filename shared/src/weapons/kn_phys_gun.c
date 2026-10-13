#include <weapons.h>

// KrunkNative Physics Gun (WEAPON_ID_PHYS_GUN), Garry's Mod's physgun: not a Krunker v7.1.4 weapon
// (doc/v7/decisions.md "Physics Gun"). The Object Manager's Tools pane equips it. Holding fire grabs
// the spawned object at the crosshair and carries it at its grab distance (the wheel changes it,
// the rotate key turns it); the client sends the poses as OM_ACTION_EDIT (client/src/ui/om_build.c,
// shared/src/object_manager.c). It never shoots (player.c skips the fire path: kn_physgun). The
// Zapper's model, sound and feel (weapon 24, shared/src/weapons/v7_zapper.c) with a tint.
const Weapon g_phys_gun = {
    .name = "Physics Gun",
    .src = "weapon_25",
    .icon = "icon_25",

    .no_auto = 1,

    .ammo = 1,
    .swap_time = 0.26f,
    .reload_time = 0.5f,
    .aim_speed = 0.11f,
    .speed_mlt = 1.0f,
    .headshot_mlt = 1.0f,
    .range = 300.0f, // OM_PHYSGUN_DIST_MAX
    .rate = 0.2f,
    .recover = 0.98f,
    .recover_y = 0.99f,
    .recover_f = 0.98f,
    .zoom = 1.2f,
    .recoil_y_mlt = 0.2f,
    .y_rotation = -0.005f,
    .jump_y_mlt = 0.1f,
    .aim_recoil_mlt = 0.4f,
    .lean_mlt = 1.0f,
    .inspect_rotation = 0.3f,
    .inspect_mlt = 0.8f,
    .swap_wiggle = 0.3f,
    .crouch_lean = 0.3f,
    .crouch_rotation = 0.3f,
    .scale = 0.84f,
    .hold_width = 1.3f,

    .origin = {0.0f, -0.23f, -3.9f},
    .offset = {1.2f, -0.6f, -3.7f},
    .left_hold = {0.0f, -1.1f, -0.3f},
    .right_hold = {0.13f, -0.62f, -0.32f},

    .sound = "weapon_25",
    .type = 0,          // not a secondary: bots and the secondary pickers never roll it
    .no_comp = 1,
    .custom_only = 1,   // never a random / onlyGoodWeapons pick
    .kn_physgun = 1,
    .tint = 0x4f7bff,   // physgun blue, so it never reads as the Zapper
};
