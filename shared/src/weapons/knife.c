#include <shared.h>
#include <math.h>

#define MA(channel) MELEE_ANIM_BIT(channel)
#define MI(channel) MELEE_ANIM_##channel

// Krunker v10.0.2 Combat Knife `anim` (v10.js:100280-100345; v7.1.4 / v3.1.1 27.js the same but the
// release time): the local player's swing (1), fist punch once the knife is thrown (2) and throw
// release (3), all on player->melee_anim. `charge` is the throw's d (players.js melee, 0.1..1).
static void knife_anim(Player *player, const int type, const float charge) {
    if (type == 1) {
        player->hand_anim_ind = (signed char) ((player->hand_anim_ind ? player->hand_anim_ind : 1) * -1);
        // j: a swing started while the last one's arm is still out (armM >= 2) is the backhand.
        const int backhand = player->melee_anim[MI(ARM_M)] >= 2.0f;
        player_melee_anim_stop(player);
        player_melee_anim_reset(player);

        float wea_r = -1.2f;
        float wea_m = 0.7f;
        float arm_r = -0.4f;
        if (backhand) {
            wea_r += 0.1f;
            player->melee_anim[MI(WEA_R)] = wea_r;
            arm_r += 0.8f;
            player->melee_anim[MI(ARM_R)] = arm_r;
            wea_m += 0.2f;
            player->melee_anim[MI(WEA_M)] = wea_m;
            player->melee_anim[MI(FLIP_W)] = (float) M_PI;
        } else {
            wea_r -= 0.4f;
        }

        const MeleeAnimStep swing = {
            .mask = MA(ARM_R) | MA(L_ARM) | MA(ARM_T) | MA(ARM_Y) | MA(ARM_M) | MA(ARM_E) | MA(WEA_R) | MA(WEA_M),
            .to = {
                [MI(ARM_R)] = arm_r, [MI(L_ARM)] = 1.0f, [MI(ARM_T)] = backhand ? -0.2f : -0.8f, [MI(ARM_Y)] = -3.0f,
                [MI(ARM_M)] = backhand ? -10.0f : 13.0f, [MI(ARM_E)] = -2.0f, [MI(WEA_R)] = wea_r, [MI(WEA_M)] = wea_m,
            },
            .duration = (backhand ? 1.3f : 1.0f) * 0.22f,
            .ease = PLAYER_EASE_CUBIC_OUT,
        };
        const MeleeAnimStep back = {
            .mask = MA(ARM_R) | MA(ARM_T) | MA(ARM_Y) | MA(L_ARM) | MA(ARM_M) | MA(ARM_E) | MA(WEA_R) | MA(WEA_M) | MA(FLIP_W),
            .duration = 0.35f,
            .ease = PLAYER_EASE_CUBIC_OUT,
        };
        player_melee_anim_start(player, &swing, &back);
    } else if (type == 2) {
        player->hand_anim_ind = (signed char) ((player->hand_anim_ind ? player->hand_anim_ind : 1) * -1);
        player_melee_anim_stop(player);
        player_melee_anim_reset(player);

        const int left = player->hand_anim_ind == 1;
        const MeleeAnimStep punch = {
            .mask = left ? MA(L_ARM) : MA(R_ARM),
            .to = {[MI(L_ARM)] = -2.7f, [MI(R_ARM)] = -2.7f},
            .duration = 0.01f,
            .ease = PLAYER_EASE_LINEAR,
        };
        const MeleeAnimStep back = {
            .mask = MA(L_ARM) | MA(R_ARM),
            .duration = 0.15f,
            .delay = 0.1f,
            .ease = PLAYER_EASE_LINEAR,
        };
        player_melee_anim_start(player, &punch, &back);
    } else if (type == 3) {
        player_melee_anim_stop(player);
        player_melee_anim_reset(player);
        player->melee_anim[MI(ARM_R)] = -1.3f;
        player->melee_anim[MI(ARM_M)] = 1.7f;
        player->melee_anim[MI(ARM_E)] = 1.1f;
        player->melee_anim[MI(ARM_Y)] = 2.9f;
        player->melee_anim[MI(ARM_T)] = 1.0f;
        player->melee_anim[MI(ARM_S)] = 1.7f;
        player->melee_anim[MI(WEA_R)] = -0.6f;

        const MeleeAnimStep release = {
            .mask = MA(ARM_Y) | MA(ARM_S) | MA(ARM_E) | MA(ARM_M) | MA(L_ARM) | MA(WEA_R),
            .to = {[MI(ARM_Y)] = -9.0f, [MI(ARM_S)] = -0.1f, [MI(ARM_E)] = -7.5f, [MI(ARM_M)] = -1.5f, [MI(L_ARM)] = 0.5f, [MI(WEA_R)] = -3.0f},
            // 200 - 200 * (d || 1) + 300 ms (v10.js:100334): a quick (low-charge) throw's release is slower
            .duration = (200.0f - 200.0f * (charge > 0.0f ? charge : 1.0f) + 300.0f) / 1000.0f,
            .ease = PLAYER_EASE_CUBIC_OUT,
        };
        const MeleeAnimStep back = {
            .mask = MA(ARM_R) | MA(ARM_T) | MA(ARM_Y) | MA(L_ARM) | MA(ARM_M) | MA(ARM_S) | MA(ARM_E) | MA(WEA_R),
            .duration = 0.35f,
            .ease = PLAYER_EASE_CUBIC_OUT,
        };
        player_melee_anim_start(player, &release, &back);
    }
}

// Krunker v7.1.4 weapon 12 (src/data/weapons.js; values checked against shared/tests/data/v7/weapons.json
// by shared/tests/weapons_v7_test.c). Times are converted from ms to s and
// recoil from per-ms to per-s; fields not ported yet: noSkins, sound, type, rumble, rumbleDur, icnPad.
const Weapon g_knife = {
    .name = "Combat Knife",
    .icon = "icon_0",

    .can_throw = 1,
    .melee = 1,

    .swap_time = 0.25f,
    .aim_speed = 0.1f,
    .speed_mlt = 1.1f,
    .damage = 50.0f,
    .range = 15.0f,
    .damage_drop = 0.0f,
    .rate = 0.25f,
    .spread = 100.0f,
    .recoil = 6.0f,
    .recoil_r = 0.01f,
    .recover = 0.98f,
    .recover_f = 0.98f,
    .zoom = 1.2f,
    .z_rotation_mlt = 0.35f,
    .lean_mlt = 0.8f,
    .hold_width = 0.9f,
    .aim_lean = 0.01f,
    .burst_rate = 0.05f,
    .headshot_mlt = 1.0f,

    .origin = {0.0001f, -0.6f, -3.6f},
    .offset = {0.0f, -0.6f, -3.6f},
    .left_hold = {1.5f, -0.82f, -0.5f},
    .right_hold = {-1.5f, -0.82f, -0.5f},
    // v3.1.1 first-person feel (players.js shoot recoilAnim / swapWeapon swapWiggle / inspect insX)
    .swap_wiggle = 0.3f,
    .inspect_x = 0.1f,
    .has_inspect_anim = 1,
    .inspect_anim = {0.3f, 0.6f, -2.2f, -1.3f, -0.1f, 1.0f, -0.19f, -0.4f, -0.2f}, // inspAnim arZ..wrY
    .melee_anim = knife_anim,

    // ---- Krunker v7.1.4 (WP-B) ----
    .sound = "melee",
    .type = 2,
    .leg_mlt = 1.0f,
    .over_charge = 1,
};
