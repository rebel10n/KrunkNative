#include <string.h>
#include <shared.h>
#include "../../modes/mode_internal.h"

// Krunker v10.0.2's mode table (doc/v10/gameplay.md section 6; v10.js mode list v10:105413): the
// same 37 modes in the same order as v7.1.4 (shared/src/modes/modes.c g_mode_configs), built as a
// copy of v7's table with v10's changes. "v10:N" = line N of the renamed 10.0.2 bundle
// (~/.cache/kn-v10/v10.js); the extractor diff ~/.cache/kn-v10/ext/diff-modes.txt lists every
// changed key, and shared/tests/ruleset_test.c checks the values below.
//
//   gun   weaponOrder adds 28 (Charge Rifle) after 0 (Sniper) (v10:105580)
//   stalk the rework (v10:104350-104437): wallJ / wallJM 100 / wallJD 0.2 / wallJV 0.04 / wallJYV 1.2,
//         canSlide = isStalker, onKill (jump scare + heal 50), update (camping turns the Living
//         Dizzy), onMelee as a damage factor, pickStalker health floor(200 + 50 x (players - 1)),
//         and the stalker's own moves (leap, wall stick, coyote time: shared/src/player.c,
//         GameModeConfig.stalker_v10)
//   raid  gameTime 360000 -> 1500000 (25 min) (v10:104536)
//   krank party: true and the Party tag removed (v10:104640)
//
// Not ported: stalk event / tag ["Event", ...] (menu lists), the Stalker model opacity 0.1 and
// hiding a still stalker (render), STALKER_PICKED / the leap HUD timer and stalker taunts (HUD);
// raid noProfileStats, runPeakPlayers, the raid challenges (rnodmg, r1hp, rnogun, rsolo, rsolofull)
// and onAIDeath "aik" (challenge service).

GameModeConfig g_mode_configs_v10[MODE_COUNT];

// v10:105580
static const int k_gun_order_v10[] = {2, 27, 16, 10, 4, 11, 21, 5, 3, 9, 1, 14, 6, 0, 28, 18, 7, 8, 13, 22, 12};

// v10:104380-104384: the same 15 s wait as v7, with v10's pickStalker
static const ModeWaitTimer k_wait_stalk_v10[] = {
    {.time = 15.0f, .msg = "picking stalker in ", .trigger = mode_trigger_pick_stalker_v10},
};

void modes_v10_build(void) {
    static int built;
    if (built) return;
    memcpy(g_mode_configs_v10, g_mode_configs, sizeof(g_mode_configs_v10));

    GameModeConfig *gun = &g_mode_configs_v10[MODE_ID_GUN];
    gun->weapon_order = k_gun_order_v10;
    gun->weapon_order_size = (int) (sizeof(k_gun_order_v10) / sizeof(k_gun_order_v10[0]));

    GameModeConfig *stalk = &g_mode_configs_v10[MODE_ID_STALK];
    stalk->has_wall_jump = 1;
    stalk->wall_jump = 1.0f;      // wallJ: !0
    stalk->wall_jump_max = 100.0f;// wallJM: 100
    stalk->wall_jump_d = 0.2f;    // wallJD: 0.2
    stalk->wall_jump_v = 0.04f;   // wallJV: 0.04
    stalk->wall_jump_yv = 1.2f;   // wallJYV: 1.2
    stalk->stalker_v10 = 1;       // canSlide: p => p.isStalker, and the stalker's leap / wall stick
    stalk->on_kill = mode_kill_stalk_v10;
    stalk->update = mode_update_stalk_v10;
    stalk->on_melee = mode_melee_stalk_v10;
    stalk->wait_timers = k_wait_stalk_v10;
    stalk->wait_timer_count = (int) (sizeof(k_wait_stalk_v10) / sizeof(k_wait_stalk_v10[0]));

    g_mode_configs_v10[MODE_ID_RAID].game_time = 1500.0f; // gameTime: 1500000 (ms)
    g_mode_configs_v10[MODE_ID_KRANK].party = 0;          // party / tag "Party" removed
    built = 1;
}
