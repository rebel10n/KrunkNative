#include <stddef.h>
#include <weapons.h>

// KrunkNative Piss (WEAPON_ID_PISS): not a Krunker v7.1.4 weapon (doc/v7/decisions.md "Piss"). Ported
// from the v3.1.1 reconstruction's grappling-gun branch (/home/user/jails/kru/src/Krunker_v3.1.1,
// 0c317bf "Piss" + e1624e4 puddles and hose): weapons.js entry 20 field for field (times ms -> s,
// recoil per ms -> per s, like weapons 0-28), the mustard* / mustardPuddle* / hose* fields in
// WeaponStream. The branch's Piss has no model (no `src`): the hose (render.js genHose) is the
// weapon mesh (client/src/player_mesh.c). Never in a class loadout (the branch's "Urineator" class
// is not ported); the Object Manager's Weapons tab and `give` hand it out.
static const WeaponStream k_piss_stream = {
    .color = 0xffd231, // 16765745
    .particles = 7,
    .speed = 0.055f,
    .spread = 0.36f,
    .gravity = 0.000065f,
    .life = 850.0f,
    .size = 2.8f,
    .pelvis_forward = 1.25f,
    .pelvis_height = 4.55f,
    .puddle_radius = 3.2f,
    .puddle_life = 5000.0f,
    .puddle_dps = 18.0f,
    .puddle_tick = 250.0f,
    .puddles_per_player = 24, // KN: the branch only capped the game (mustardMaxPuddles 64)
    .puddles_max = 64,
    .hose_radius = 0.18f,
    .hose_length = 1.25f,
    // KN: the branch's hoseViewX 0, hoseViewY - 1.5 (-2.15), hoseViewZ -0.95 sits under KN's
    // viewmodel frame (not on screen); here beside the right hand, pointing ahead
    .hose_view = {0.15f, -1.45f, -1.0f},
};

const Weapon g_piss = {
    .name = "Piss",
    .src = NULL, // no model: the hose (WeaponStream.hose_*)
    .icon = "icon_24", // KN: the branch's "icon_20" is v7's Build Tool; the Slimer's spray gun reads closer

    .no_spread = 1,
    .no_aim = 1,
    .no_auto = 0, // nAuto false: hold to spray

    .ammo = 999,
    .swap_time = 0.22f,
    .reload_time = 0.65f,
    .aim_speed = 0.1f,
    .speed_mlt = 1.0f,
    // KN: the branch's dmg 0 left all damage to the puddles; here the stream hits too (4 a shot,
    // ~73 per second at the 55 ms rate), no head / leg zones (hsMlt 1)
    .damage = 4.0f,
    .headshot_mlt = 1.0f,
    .leg_mlt = 1.0f,
    .range = 47.0f, // the stream's reach (speed x life); the branch's range 120 had no reader
    .rate = 0.055f, // mustardRate 55 (= rate)
    .recoil = 1.0f,
    .recoil_r = 0.003f,
    .recover = 0.99f,
    .recover_y = 0.995f,
    .recover_f = 0.99f,
    .zoom = 1.0f,
    .recoil_y_mlt = 0.08f,
    .recoil_z = 0.6f,
    .aim_recoil_mlt = 0.2f,
    .lean_mlt = 0.5f,
    .inspect_rotation = 0.1f,
    .inspect_mlt = 0.6f,
    .crouch_lean = 0.08f,
    .crouch_rotation = 0.05f,
    .crouch_drop = 0.05f,
    .scale = 0.0007946847095477368f,
    .hold_width = 0.55f,

    .origin = {0.0f, -1.4f, -3.05f},
    .offset = {0.2f, -1.55f, -3.15f},
    .left_hold = {-0.35f, -1.85f, 0.7f},
    .right_hold = {0.32f, -1.62f, -0.35f},
    .has_recoil_anim = 1,
    .recoil_anim_time = 0.08f,
    .recoil_tween_y = 0.025f,
    .recoil_anim_mask = 1,

    .sound = "spray", // played at most every 220 ms (client/src/sound_events.c)
    .type = 0,
    .no_comp = 1,
    .custom_only = 1, // never a random / onlyGoodWeapons pick
    .stream = &k_piss_stream,
};
