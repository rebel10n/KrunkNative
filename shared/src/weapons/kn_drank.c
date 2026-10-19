#include <stddef.h>
#include <weapons.h>

// KrunkNative Purple Drank (WEAPON_ID_DRANK): not a Krunker v7.1.4 weapon (doc/v7/decisions.md
// "Purple Drank"; shared/include/drank.h). The Nettspend class's primary (classes_v10.c). A copy
// of the Piss (kn_piss.c): the same traced stream arc, the same SPAWNED_OBJ puddles, in purple
// (#7b2cbf), with the three KrunkNative twists in WeaponStream: puddles slow the players standing
// in them (puddle_slow 0.55), the reload is a 1.4 s "pour-up" (long enough for the viewmodel's
// pour, the scream and the "DRANK DRANK DRANK" line, client/), and each pour-up has a 4% chance
// (laced_chance, authority-rolled, NET_EXT_KN_LACED) to lace the pourer for 8 s (laced_ms).
// Magazine: 40 sips (a cup; the Piss's 999 never reloaded), rate 60 ms. No model: the client
// draws a white styrofoam double cup keyed on the id (client/src/nettspend_vfx.c), never the hose.
static const WeaponStream k_drank_stream = {
    .color = 0x7b2cbf, // purple (the Piss: mustard #ffd231)
    .particles = 7,
    .speed = 0.055f,
    .spread = 0.36f,
    .gravity = 0.000065f,
    .life = 850.0f,
    .size = 2.8f,
    .pelvis_forward = 1.25f,
    .pelvis_height = 4.55f,
    .puddle_radius = 3.0f,
    .puddle_life = 6000.0f, // 6 s: a little longer than the Piss's 5 s, the slow is the point
    .puddle_dps = 10.0f,    // 2.5 a 250 ms tick (the Piss: 18 dps); the slow makes up for it
    .puddle_tick = 250.0f,
    .puddles_per_player = 16,
    .puddles_max = 64,
    .hose_radius = 0.18f,
    .hose_length = 1.25f,
    .hose_view = {0.0f, 0.15f, -0.7f},
    .hose_view_rot = {0.35f, 0.0f, 0.0f},
    // KrunkNative Drank
    .puddle_slow = 0.55f,   // move accel x while standing in a Drank puddle (victims only)
    .laced_chance = 0.04f,  // per pour-up (reload)
    .laced_ms = 8000.0f,    // the laced spell
};

const Weapon g_drank = {
    .name = "Purple Drank",
    .src = NULL, // no model: the client's double cup (the hose_* fields are unused)
    .icon = "icon_kn_drank", // textures/weapons/icon_kn_drank.png (client asset)

    .no_spread = 1,
    .no_aim = 1,
    .no_auto = 0, // hold to pour

    .ammo = 40,
    .swap_time = 0.3f,
    .reload_time = 1.4f, // the pour-up
    .aim_speed = 0.1f,
    .speed_mlt = 1.0f,
    // KrunkNative balance (doc/v10/gameplay.md "Nettspend balance", round 2): a sip is 5, up from the Piss's 4.
    // The bench (Kanji FFA, arc-aware bot aim, seeds 1-6): Nettspend field KDR 0.83 -> 0.88, Drank damage +10%,
    // duels even; the Money Spread, reach, rate, puddles and pour-up are unchanged. No head / leg zones.
    .damage = 5.0f,
    .headshot_mlt = 1.0f,
    .leg_mlt = 1.0f,
    .range = 47.0f, // the stream's reach (speed x life)
    .rate = 0.06f,
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

    .sound = "kn_drank_pour", // the pour / splash (assets/sound/kn_drank_pour.mp3, client asset), at most every 220 ms like the Piss's spray
    .type = 0,
    .no_comp = 1,
    .custom_only = 1, // never a random / onlyGoodWeapons pick
    .stream = &k_drank_stream,
};
