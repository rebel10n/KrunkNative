#include <stddef.h>
#include <weapons.h>

// Krunker v10's two new secondaries (doc/v10/gameplay.md section 5 "New weapons"; added in v10.0.2,
// unchanged in v10.0.3), only in the v10 ruleset's table (weapons_v10.c). Every value is the v10.0.3
// entry ("deob-10.0.3.js:N" = line N of ~/jails/kru/src/Krunker_v10.0.3/deob/game.deob.js); checked
// field by field against shared/tests/data/v10/weapons.json by shared/tests/weapons_v10_test.c. Times in s (JS ms / 1000),
// recoil per s (JS per ms x 1000). Not ported (cosmetic / no KN consumer): assetIcon, refDist,
// rollOff, transp, noAo, nCase, nMuz, nRing, shine, wyOff, muzOff, muzMlt, rumble, icnPad, expScale.

// id 29 Boulder (deob-10.0.3.js:115235-115286): a carried rock (models/weapons/weapon_30.obj "Rock",
// textures/weapons/weapon_30.png, icon_30). type 3 (equipment slot) but `secondary`, no damage
// fields, no `req`: no class or mode hands it out in the bundle (map logic / KrunkScript give it).
// Holding it slows (spdMlt 0.85, jumMlt 0.88) and makes gravity 1.7x (gravMlt, new in v10:
// player procInputs deob-10.0.3.js:191697 `* (this.weapon.gravMlt || 1)`). It never fires: no ammo, and
// canThrow false.
const Weapon g_boulder = {
    .name = "Boulder",
    .src = "weapon_30",
    .icon = "icon_30",
    .sound = NULL,          // 'sound': '' (deob-10.0.3.js:115240)

    .secondary = 1,         // 'secondary': !0
    .no_inspect = 1,        // 'nInsp': !0
    .no_aim = 1,            // 'noAim': !0
    .can_throw = 0,         // 'canThrow': !1
    .no_cross = 1,          // 'noCross': !0
    .type = 3,              // 'type': 3

    .aim_speed = 0.14f,     // 'aimSpd': 140
    .swap_time = 0.2f,      // 'swapTime': 200
    .speed_mlt = 0.85f,     // 'spdMlt': 0.85
    .gravity_mlt = 1.7f,    // 'gravMlt': 1.7 (new field)
    .jump_mlt = 0.88f,      // 'jumMlt': 0.88
    .scale = 0.63f,         // 'scale': 0.63
    .hold_width = 1.2f,     // 'holdW': 1.2
    .zoom = 1.0f,           // 'zoom': 1
    .rotation = {0.0f, 1.5707963267948966f, -0.4f}, // 'rot': [0, PI / 2, -0.4]
    .has_rotation = 1,
    .left_hold = {0.82f, -0.3f, 0.33f},   // leftHoldX / Y / Z
    .right_hold = {-0.82f, -0.3f, 0.33f}, // rightHoldX / Y / Z
    .offset = {0.0f, -0.8f, -3.5f},       // xOff / yOff / zOff
    .origin = {0.0f, -0.8f, -3.5f},       // xOrg / yOrg / zOrg
    .crouch_lean = 0.0f,    // 'cLean': 0
    .crouch_rotation = 0.0f,// 'cRot': 0
    .crouch_drop = 0.12f,   // 'cDrop': 0.12
    .jump_y_mlt = 0.95f,    // 'jYMlt': 0.95
    .spread = 120.0f,       // 'spread': 120
    .recover = 0.98f,       // 'recover': 0.98
    .recover_f = 0.98f,     // 'recoverF': 0.98
    .lean_mlt = 0.3f,       // 'leanMlt': 0.3
    .burst_rate = 0.05f,    // weapons post-processing: burstR 50 when not a burst weapon (deob-10.0.3.js:115390-115394)
};

// id 30 Compressor (deob-10.0.3.js:115286-115372): a level-45 secondary (minRec 45 + req, noComp) that fires
// only its physPow ray: noDmg skips the bullet ray (players.shoot deob-10.0.3.js:196341, 196599), and
// nonObjImpulse (weapon-level, new in v10; v7 had it on modes only, deob-10.0.3.js:196538) makes that ray a
// fixed push of the shooter along the shot, physPow 0.1 x impulseMlt, whatever it hits. physPull
// flips the sign when the shot came from the aim input (deob-10.0.3.js:196537 `physPull && rMouse`, x -1
// at :196585), and rShoot lets the aim input fire (procInputs deob-10.0.3.js:192359): left click pushes the
// shooter back, right click pulls it forward. Other players and objects are not moved (the
// nonObjImpulse path skips the object push, deob-10.0.3.js:196731). Model weapon_31.obj, sound
// weapon_31.mp3, icon_31.
const Weapon g_compressor = {
    .name = "Compressor",
    .src = "weapon_31",
    .icon = "icon_31",
    .sound = "weapon_31",   // 'sound': "weapon_31"

    .secondary = 1,         // 'secondary': !0
    .no_spread = 0,         // 'noSpread': !1
    .no_aim = 1,            // 'noAim': !0
    .akimbo = 0,            // 'akimbo': !1
    .no_auto = 1,           // 'nAuto': !0
    .type = 1,              // 'type': 1
    .min_rec = 45,          // 'minRec': 45 (+ 'req': _34256, the v7 f701 rule)
    .no_comp = 1,           // 'noComp': !0
    .no_dmg = 1,            // 'noDmg': !0 (new)
    .phys_pull = 1,         // 'physPull': !0 (new)
    .non_obj_impulse = 1,   // 'nonObjImpulse': !0 (new on a weapon)
    .r_shoot = 1,           // 'rShoot': !0 (new)

    .swap_wiggle = 0.4f,    // 'swapWiggle': 0.4
    .physical_range = 50.0f,// 'physRang': 50
    .physical_power = 0.1f, // 'physPow': 0.1
    .swap_time = 0.35f,     // 'swapTime': 350
    .aim_speed = 0.1f,      // 'aimSpd': 100
    .charge_time = 0.0f,    // 'chrgTime': 0
    .speed_mlt = 1.05f,     // 'spdMlt': 1.05
    .ammo = 1,              // 'ammo': 1
    .reload_time = 2.0f,    // 'reload': 2000
    .damage = 1.0f,         // 'dmg': 1
    .headshot_mlt = 1.25f,  // 'hsMlt': 1.25
    .damage_drop = 10.0f,   // 'dmgDrop': 10
    .scale = 0.9f,          // 'scale': 0.9
    .left_hold = {0.0f, -1.3f, -0.45f},   // leftHoldY / Z
    .right_hold = {0.1f, -0.75f, -0.26f}, // rightHoldX / Y / Z
    .hold_width = 1.0f,     // 'holdW': 1
    .offset = {0.7f, -0.6f, -3.7f},       // xOff / yOff / zOff
    .origin = {0.0f, -0.29f, -3.9f},      // xOrg / yOrg / zOrg
    .y_rotation = -0.005f,  // 'yRot': -0.005
    .crouch_lean = 0.2f,    // 'cLean': 0.2
    .crouch_rotation = 0.2f,// 'cRot': 0.2
    .crouch_drop = 0.1f,    // 'cDrop': 0.1
    .jump_y_mlt = 0.2f,     // 'jYMlt': 0.2
    .inspect_rotation = 0.1f, // 'inspectR': 0.1
    .inspect_mlt = 1.9f,    // 'inspectM': 1.9
    .range = 40.0f,         // 'range': 40
    .rate = 0.45f,          // 'rate': 450
    .inaccuracy = 110.0f,   // 'innac': 110
    .spread = 180.0f,       // 'spread': 180
    .min_spread = 20.0f,    // 'minSpread': 20
    .zoom = 0.0f,           // 'zoom': 0
    .lean_mlt = 1.6f,       // 'leanMlt': 1.6
    .recoil = 20.0f,        // 'recoil': 0.02 per ms
    .recoil_r = 0.015f,     // 'recoilR': 0.015
    .recover = 0.99f,       // 'recover': 0.99
    .recover_f = 0.97f,     // 'recoverF': 0.97
    .recoil_z = 2.1f,       // 'recoilZ': 2.1
    .recoil_z_mlt = 0.2f,   // 'recoilZM': 0.2
    .aim_recoil_mlt = 0.67f,// 'aimRecMlt': 0.67
    .recoil_y_mlt = 0.65f,  // 'recoilYM': 0.65
    .has_recoil_anim = 1,   // 'recoilAnim': {time: 340, recoilTweenY: 0.22}
    .recoil_anim_time = 0.34f,
    .recoil_tween_y = 0.22f,
    .recoil_anim_mask = 1,  // Y only
    .jump_bob_y_mlt = 0.5f, // 'jumpYM': 0.5
    .burst_rate = 0.05f,    // weapons post-processing: burstR 50 (deob-10.0.3.js:115390-115394)
};
