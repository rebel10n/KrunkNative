#include <stddef.h>
#include <weapons.h>

// KrunkNative Money Spread (WEAPON_ID_MONEY_SPREAD): not a Krunker v7.1.4 weapon (doc/v7/decisions.md
// "Money Spread"). The Nettspend class's secondary (classes_v10.c): a throw of cash, a fan of 8
// hitscan "bills" (shots 8, cSpread below: a flat fan, about +-5 degrees of yaw at the x0.3-0.32
// cSpread factor of player_shoot, the pattern's yaw x0.24 of the first version) of 14 damage each
// (112 point blank with all 8 in, no head zone: hsMlt 1), falling by 8 over `range` 120 (dmgDrop 8:
// a bill at 47 units deals about 10.9, at 120 units 6), 8 throws a magazine, 250 ms between throws,
// a 180 ms swap and a 1.2 s reload. The Sawed Off (sawed_off.c) was the shape: a wide close-range
// secondary that trades damage for reach. No knockback (physPow 0).
// Balance (KrunkNative, not v10.0.2: doc/v10/gameplay.md "Nettspend balance", the table of every
// value and the bench). Three versions: the first (7 a bill, dmgDrop 7 over 60, 5 a magazine, 420 ms,
// yaw about +-20 degrees) had the Nettspend at KDR 0.03 in the bots' field (Kanji FFA, 6 seeds x 300 s).
// The second (18 a bill, dmgDrop 4 over 120, 8 a magazine, 250 ms, yaw +-8 degrees) reached KDR 0.96,
// but its falloff was nearly flat (a bill at 47 units kept 16.4 of 18) and a point-blank full fan was
// 144. This one (14 a bill, dmgDrop 8 over 120, yaw x0.6 of the second) keeps the reach and the rate,
// cuts the point-blank full fan to 112, makes a 47-unit bill about a fifth less than a close one, and
// holds KDR 0.91 (seeds 7-12: 0.93) with the duels even. Only the Money Spread's own entries changed.
// No model (`src` NULL): the client draws a fan of cash keyed on the id (client/src/nettspend_vfx.c,
// player_mesh.c), as it draws the bill-flutter burst; the sound is a client asset. (The first
// version reused the Pistol's model at this table's scale 1, about 3000x the Pistol's 0.000316: the
// camera sat inside a giant green pistol, and a throw's recoil swung it like a changing sky.)
const Weapon g_money_spread = {
    .name = "Money Spread",
    .src = NULL,             // the client's fan of cash (no model)
    .icon = "icon_kn_money", // textures/weapons/icon_kn_money.png (client asset)

    .secondary = 1,
    .no_spread = 0,
    .akimbo = 0,
    .no_aim = 0,
    .no_auto = 1, // a click a throw

    .ammo = 8,
    .shots = 8,
    .swap_time = 0.18f,
    .reload_time = 1.2f,
    .aim_speed = 0.1f,
    .speed_mlt = 1.05f,
    .damage = 14.0f,
    .headshot_mlt = 1.0f,
    .leg_mlt = 1.0f,
    .range = 120.0f,
    .damage_drop = 8.0f, // the fall over `range` (a bill at full range keeps 6): the bills flutter down
    .rate = 0.25f,
    .spread = 120.0f,
    .min_spread = 20.0f,
    .inaccuracy = 110.0f,
    .recoil = 12.0f,
    .recoil_r = 0.012f,
    .recover = 0.99f,
    .recover_f = 0.97f,
    .physical_power = 0.0f,
    .physical_range = 0.0f,
    .zoom = 1.2f,
    .recoil_y_mlt = 0.6f,
    .recoil_z = 1.6f,
    .recoil_z_mlt = 0.3f,
    .jump_y_mlt = 0.2f,
    .jump_bob_y_mlt = 0.5f,
    .aim_recoil_mlt = 0.3f,
    .lean_mlt = 1.4f,
    .inspect_rotation = 0.1f,
    .inspect_mlt = 1.6f,
    .crouch_lean = 0.2f,
    .crouch_rotation = 0.2f,
    .crouch_drop = 0.1f,
    .scale = 1.0f,
    .hold_width = 1.0f,

    .origin = {0.0f, -0.3f, -2.8f},
    .offset = {0.95f, -0.65f, -3.8f},
    .left_hold = {0.0f, -0.5f, 0.4f},
    .right_hold = {0.1f, -0.6f, -1.5f},

    // one pattern of 8 (x yaw, y pitch; player_shoot scales a pair by 0.30-0.32): a flat fan. The
    // yaw is x0.24 of the first version's pattern (-1.1 .. 1.1, about +-20 degrees): see the header
    .custom_spread_count = 8,
    .custom_spread = (const vec2[]) {
        {-0.264f, 0.05f},
        {-0.18f, -0.12f},
        {-0.096f, 0.18f},
        {-0.0288f, -0.05f},
        {0.0288f, 0.08f},
        {0.096f, -0.16f},
        {0.18f, 0.14f},
        {0.264f, -0.06f},
    },
    .swap_wiggle = 0.4f,
    .has_recoil_anim = 1,
    .recoil_anim_time = 0.3f,
    .recoil_tween_y = 0.3f,
    .recoil_anim_mask = 1,

    .sound = "kn_money_throw", // assets/sound/kn_money_throw.mp3 (client asset)
    .type = 1,
    .no_comp = 1,
    .custom_only = 1, // never a random / onlyGoodWeapons pick; the Nettspend's or `give money`
};
