// WP-E: Krunker v7.1.4 custom game config, host types, map "config", match votes and network
// rates (shared/include/game_config.h; doc/v7/map-logic.md §1.9, §4.3, §5.3-5.4, §7 WP-E).
//
// Sources: src/config.js (serverConfig, classConfig, weaponConfig, effectConfig, hostTypes),
// src/data/customGameConfig.js (validateConfigs :8-151, setupGameConfig :369-448),
// src/data/game.js (applyConfig :346-604, applyNetworking :325-337, voteMatch :3350,
// init's tally :3398-3424, endGame's mts :4623-4660, getPossibleMatches / genMatchList :5068-5133).
//
// v7 bugs fixed here (decisions.md "v7 bugs"), each one also noted where it applies:
//   - serverConfig "input" values: v7 assigns sanitizeStr(value) before its character / length
//     checks, so they never apply. Here an invalid name falls back to the default and a long one
//     is cut to 16 characters, as the code intends.
//   - validateCustomConfig's "Too much data" test measures `obj + ""` ("[object Object]"), so it
//     never fires. Here it measures the JSON text.
//   - setupGameConfig writes into the hostTypes' overDat / forceDat objects (shared config.js
//     state, so one comp host's settings leak into the next). Here it works on copies.
//   - classConfig regenDelay: no v7 class defines the key, so applyConfig's "base has the key"
//     test drops every override the editor offers. Here class keys always apply.
//   - genMatchList pushes undefined picks when fewer normal matches than picks exist. Here the
//     list just ends.
#include <game_config.h>
#include <modes.h>
#include <pcg_basic.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ruleset.h>
#include <flavor_rules.h> // Krunker Strike: its classes, lobby defaults

// ---------------------------------------------------------------- option tables

enum { // GameConfigOption.store
    GC_STORE_INT, GC_STORE_FLOAT, GC_STORE_U8, GC_STORE_BIT, GC_STORE_STR,      // serverConfig -> GameConfig
    GC_STORE_CLS_NAME, GC_STORE_CLS_INT, GC_STORE_CLS_FLOAT, GC_STORE_CLS_BIT,   // classConfig -> ClassConfig
    GC_STORE_EXT_INT, GC_STORE_EXT_FLOAT, GC_STORE_EXT_U8, GC_STORE_EXT_STR,     //             -> ClassConfig.ext
    GC_STORE_MOVE,                                                               //             -> ClassConfig.movement (WP-A)
    GC_STORE_WEP_NAME, GC_STORE_WEP_FLOAT, GC_STORE_WEP_UINT, GC_STORE_WEP_BIT, GC_STORE_WEP_PJ, // weaponConfig -> Weapon
    GC_STORE_EFF_NAME, GC_STORE_EFF_INT, GC_STORE_EFF_FLOAT,                     // effectConfig -> EffectConfig
    GC_STORE_NONE,                                                               // validated, not consumed by KN
};

enum { U_SAME, U_MS, U_X1000 }; // v7 -> KN unit: as is, ms -> s, x1000 (weapon recoil per ms -> per s)

enum { // GameConfig bitfields (not addressable through offsetof)
    GC_BIT_CAN_SLIDE, GC_BIT_AIR_STRAFE, GC_BIT_AUTO_JUMP, GC_BIT_BULLET_DROP, GC_BIT_HEALTH_REGEN,
    GC_BIT_KILL_REWARDS, GC_BIT_HEADSHOTS_ONLY, GC_BIT_NO_SECONDARIES, GC_BIT_NO_STREAKS,
    GC_BIT_DISABLE_BORDERS, GC_BIT_THROWABLE_MELEES, GC_BIT_NO_CHARGE_WEAPONS, GC_BIT_SELECT_TEAM,
    GC_BIT_ALLOW_SPECTATE, GC_BIT_THIRD_PERSON, GC_BIT_HIDE_NAMETAGS, GC_BIT_RAMP_FIX,
};
enum { CB_SECONDARY, CB_WALL_JUMP, CB_BACK_SLIDE };
enum { WB_BURST, WB_OVER_CHARGE, WB_SECONDARY, WB_EQUIPMENT, WB_AKIMBO, WB_NO_AUTO, WB_NO_SPREAD, WB_NO_AIM, WB_PROJECTILE_DISABLE };
enum { // ClassConfig.movement keys (WP-A ClassMovement)
    CM_SCHEME, CM_SPEED_X, CM_SPEED_Y, CM_SPEED_Z, CM_MOVE_DIR_OFFSET, CM_TURN_RATE, CM_GROUND_ACCEL,
    CM_GROUND_DECEL, CM_AIR_ACCEL, CM_AIR_DECEL, CM_CAN_CROUCH, CM_CROUCH_SPEED, CM_LADDER_ACCEL,
    CM_FREE_SLIDE, CM_STEP_FREQ, CM_DISABLE_JUMP, CM_INFINITE_JUMP, CM_OVERRIDE_JUMP,
    CM_PLAYER_COLLISION, CM_LOCK_PITCH, CM_WALL_JUMP_DECEL, CM_WALL_JUMP_VEL,
};

static const char *const k_team_size_options[] = {"1v1", "2v2", "3v3", "4v4"};

// config.serverConfig (v7 order; nameTeam3..5 / t3Dmg..t5Dmg spliced in as config.js does; v10.0.2's
// hurtboxScale, noPvp, transferHostOnLeave and webhook are in their v10 places, GC_STORE_NONE where KN has no effect).
static const GameConfigOption k_server_options[] = {
    {"maxPlayers", "Players", "Server Size", NULL, GC_NUM, GC_STORE_INT, GC_BASIC, 0, .def = 2.0f, .min = 1.0f, .max = 10.0f, .max_p = 16.0f, .max_f = 20.0f, .max_d = 50.0f, .max_g = 6.0f, .step = 1.0f, .offset = offsetof(GameConfig, max_players)},
    {"minPlayers", "Min Players", "Server Size", NULL, GC_NUM, GC_STORE_INT, GC_BASIC, 0, .def = 0.0f, .min = 0.0f, .max = 10.0f, .max_p = 16.0f, .max_f = 20.0f, .max_d = 50.0f, .max_g = 6.0f, .step = 1.0f, .offset = offsetof(GameConfig, min_players)},
    {"gameTime", "Minutes", "Timers", NULL, GC_NUM, GC_STORE_INT, GC_BASIC | GC_HAS_DEF_C, GC_HT(0) | GC_HT(1), .def = 4.0f, .def_c = 4.0f, .min = 0.0f, .max = 60.0f, .step = 1.0f, .offset = offsetof(GameConfig, game_time)},
    {"warmupTime", "Warmup", "Timers", NULL, GC_NUM, GC_STORE_FLOAT, GC_BASIC, 0, .def = 0.0f, .min = 0.0f, .max = 4.0f, .step = 0.1f, .offset = offsetof(GameConfig, warmup_time)},
    {"gamRounds", "Rounds", "Timers", NULL, GC_NUM, GC_STORE_INT, GC_BASIC | GC_HAS_DEF_C, GC_HT(0) | GC_HT(1), .def = 1.0f, .def_c = 3.0f, .min = 1.0f, .max = 10.0f, .step = 1.0f, .offset = offsetof(GameConfig, game_rounds)},
    {"intermTmr", "Intermission (s)", "Timers", NULL, GC_NUM, GC_STORE_FLOAT, GC_BASIC | GC_HAS_DEF_C, GC_HT(0) | GC_HT(1), .def = 30.0f, .def_c = 30.0f, .min = 5.0f, .max = 120.0f, .step = 1.0f, .offset = offsetof(GameConfig, intermission_time)},
    {"forceSpawn", "Auto Respawn", "Timers", NULL, GC_NUM, GC_STORE_INT, GC_DONT_CHANGE, 0, .def = 0.0f, .min = 0.0f, .max = 15.0f, .step = 1.0f, .offset = offsetof(GameConfig, auto_respawn)},
    {"lives", "Lives", "Health & Lives", NULL, GC_NUM, GC_STORE_INT, GC_BASIC | GC_DONT_CHANGE, 0, .def = 0.0f, .min = 0.0f, .max = 10.0f, .step = 1.0f, .offset = offsetof(GameConfig, lives)},
    {"scoreLimit", "Score Limit", "Score & Rewards", NULL, GC_NUM, GC_STORE_INT, 0, 0, .def = 0.0f, .min = 0.0f, .max = 10000.0f, .step = 100.0f, .offset = offsetof(GameConfig, score_limit)},
    {"keepTScore", "Keep Team Score", "Score & Rewards", NULL, GC_BOOL, GC_STORE_U8, GC_DONT_CHANGE, 0, .def = 0.0f, .offset = offsetof(GameConfig, keep_team_score)},
    {"objtvTime", "Objective Time", "Mode Settings", "Mode Settings", GC_NUM, GC_STORE_FLOAT, GC_HAS_DEF_C, GC_HT(0) | GC_HT(1), .def = 1.0f, .def_c = 1.0f, .min = 0.1f, .max = 10.0f, .step = 0.1f, .offset = offsetof(GameConfig, objective_time)},
    {"forceC", "Force Mode Classes", "Mode Settings", NULL, GC_BOOL, GC_STORE_U8, GC_DONT_CHANGE, 0, .def = 1.0f, .offset = offsetof(GameConfig, force_classes)},
    {"logTim", "Log Time (Race/Bhop)", "Mode Settings", NULL, GC_BOOL, GC_STORE_U8, GC_DONT_CHANGE, 0, .def = 1.0f, .offset = offsetof(GameConfig, log_time)},
    {"lstChkT", "Last Checkpoint Time Rewind (Race/Bhop)", "Mode Settings", NULL, GC_BOOL, GC_STORE_U8, GC_DONT_CHANGE, 0, .def = 0.0f, .offset = offsetof(GameConfig, last_checkpoint_time)},
    {"gravMlt", "Gravity", "World Physics", "Physics", GC_NUM, GC_STORE_FLOAT, GC_DONT_CHANGE, 0, .def = 1.0f, .min = 0.0f, .max = 2.0f, .step = 0.001f, .offset = offsetof(GameConfig, gravity_mlt)},
    {"deltaMlt", "Time Scale", "World Physics", NULL, GC_NUM, GC_STORE_FLOAT, GC_DONT_CHANGE, 0, .def = 1.0f, .min = 0.1f, .max = 1.5f, .step = 0.1f, .offset = offsetof(GameConfig, delta_mlt)},
    {"fallDmg", "Fall Damage", "World Physics", NULL, GC_NUM, GC_STORE_FLOAT, 0, 0, .def = 0.0f, .min = 0.0f, .max = 10.0f, .step = 0.01f, .offset = offsetof(GameConfig, fall_damage)},
    {"fallDmgThr", "Fall Damage Thresh", "World Physics", NULL, GC_NUM, GC_STORE_FLOAT, 0, 0, .def = 0.0f, .min = 0.0f, .max = 10.0f, .step = 0.01f, .offset = offsetof(GameConfig, fall_damage_threshold)},
    {"jumpMlt", "Jump Force", "Jumping", NULL, GC_NUM, GC_STORE_FLOAT, GC_DONT_CHANGE, 0, .def = 1.0f, .min = 0.1f, .max = 3.0f, .step = 0.001f, .offset = offsetof(GameConfig, jump_mlt)},
    {"fixMov", "Fixed Movement", "Movement", NULL, GC_BOOL, GC_STORE_U8, GC_DONT_CHANGE, 0, .def = 0.0f, .offset = offsetof(GameConfig, fixed_movement)},
    {"slidTime", "Slide Time", "Movement", NULL, GC_NUM, GC_STORE_FLOAT, GC_DONT_CHANGE, 0, .def = 1.0f, .min = 1.0f, .max = 5.0f, .step = 0.1f, .offset = offsetof(GameConfig, slide_time)},
    {"slidSpd", "Slide Speed", "Movement", NULL, GC_NUM, GC_STORE_FLOAT, GC_DONT_CHANGE, 0, .def = 1.0f, .min = 0.0f, .max = 2.0f, .step = 0.01f, .offset = offsetof(GameConfig, slide_speed)},
    {"impulseMlt", "Weapon Impulse", "Gameplay Logic", NULL, GC_NUM, GC_STORE_FLOAT, GC_DONT_CHANGE, 0, .def = 1.0f, .min = 0.0f, .max = 3.0f, .step = 0.1f, .offset = offsetof(GameConfig, impulse_mlt)},
    {"wallJP", "Wall Jump Power", "Jumping", NULL, GC_NUM, GC_STORE_FLOAT, GC_DONT_CHANGE, 0, .def = 1.0f, .min = 0.0f, .max = 2.0f, .step = 1.0f, .offset = offsetof(GameConfig, wall_jump)},
    {"strafeSpd", "Strafe Speed", "Movement", NULL, GC_NUM, GC_STORE_FLOAT, GC_DONT_CHANGE, 0, .def = 1.2f, .min = 1.0f, .max = 2.0f, .step = 0.1f, .offset = offsetof(GameConfig, strafe_speed)},
    {"canSlide", "Sliding", "Movement", NULL, GC_BOOL, GC_STORE_BIT, GC_DONT_CHANGE, 0, .def = 1.0f, .offset = 0, .bit = GC_BIT_CAN_SLIDE},
    {"airStrf", "Air Strafe", "Movement", NULL, GC_BOOL, GC_STORE_BIT, GC_DONT_CHANGE, 0, .def = 0.0f, .offset = 0, .bit = GC_BIT_AIR_STRAFE},
    {"autoJump", "Auto Jump", "Jumping", NULL, GC_BOOL, GC_STORE_BIT, GC_DONT_CHANGE, 0, .def = 0.0f, .offset = 0, .bit = GC_BIT_AUTO_JUMP},
    {"bDrop", "Bullet Drop", "World Physics", NULL, GC_BOOL, GC_STORE_BIT, GC_DONT_CHANGE, 0, .def = 0.0f, .offset = 0, .bit = GC_BIT_BULLET_DROP},
    {"healthMlt", "Health Multiplier", "Health & Lives", "Game Logic", GC_NUM, GC_STORE_FLOAT, GC_DONT_CHANGE, 0, .def = 1.0f, .min = 0.1f, .max = 3.0f, .step = 0.1f, .offset = offsetof(GameConfig, health_mlt)},
    {"hitBoxPad", "Hitbox Padding", "Collisions", NULL, GC_NUM, GC_STORE_FLOAT, GC_DONT_CHANGE | GC_HAS_DEF_C, GC_HT(0) | GC_HT(1), .def = 0.6f, .def_c = 0.1f, .min = -2.0f, .max = 2.0f, .step = 0.1f, .offset = offsetof(GameConfig, hitbox_pad)},
    // v10.0.2 (v10:16931 "Hurtbox Scale", sub "Hitboxes", def 1, 1..2): the hit boxes' scale (shared/src/hitboxes.c)
    {"hurtboxScale", "Hurtbox Scale", "Hitboxes", NULL, GC_NUM, GC_STORE_FLOAT, GC_DONT_CHANGE, GC_HT(0) | GC_HT(1), .def = 1.0f, .min = 1.0f, .max = 2.0f, .step = 0.1f, .offset = offsetof(GameConfig, hurtbox_scale)},
    {"fiRat", "Fire Rate", "Gameplay Logic", NULL, GC_NUM, GC_STORE_FLOAT, GC_DONT_CHANGE, 0, .def = 1.0f, .min = 0.5f, .max = 2.0f, .step = 0.1f, .offset = offsetof(GameConfig, fire_rate)},
    {"reSpd", "Reload Speed", "Gameplay Logic", NULL, GC_NUM, GC_STORE_FLOAT, GC_DONT_CHANGE, 0, .def = 1.0f, .min = 0.5f, .max = 2.0f, .step = 0.1f, .offset = offsetof(GameConfig, reload_speed)},
    {"hpRegen", "Health Regen", "Health & Lives", NULL, GC_BOOL, GC_STORE_BIT, GC_DONT_CHANGE, 0, .def = 1.0f, .offset = 0, .bit = GC_BIT_HEALTH_REGEN},
    {"killRewards", "Kill Rewards", "Score & Rewards", NULL, GC_BOOL, GC_STORE_BIT, GC_DONT_CHANGE, 0, .def = 1.0f, .offset = 0, .bit = GC_BIT_KILL_REWARDS},
    {"noAssists", "No Assists", "Score & Rewards", NULL, GC_BOOL, GC_STORE_U8, GC_DONT_CHANGE, 0, .def = 0.0f, .offset = offsetof(GameConfig, no_assists)},
    {"headshotOnly", "Headshots Only", "Gameplay Logic", NULL, GC_BOOL, GC_STORE_BIT, GC_DONT_CHANGE, 0, .def = 0.0f, .offset = 0, .bit = GC_BIT_HEADSHOTS_ONLY},
    {"noSecondary", "No Secondaries", "Gameplay Logic", NULL, GC_BOOL, GC_STORE_BIT, GC_DONT_CHANGE, 0, .def = 0.0f, .offset = 0, .bit = GC_BIT_NO_SECONDARIES},
    {"noStreaks", "Disable Streaks", "Other", NULL, GC_BOOL, GC_STORE_BIT, GC_DONT_CHANGE, 0, .def = 0.0f, .offset = 0, .bit = GC_BIT_NO_STREAKS},
    {"disableB", "Disable Borders", "Other", NULL, GC_BOOL, GC_STORE_BIT, GC_DONT_CHANGE, 0, .def = 0.0f, .offset = 0, .bit = GC_BIT_DISABLE_BORDERS},
    {"throwMel", "Throwable Melees", "Gameplay Logic", NULL, GC_BOOL, GC_STORE_BIT, GC_DONT_CHANGE, 0, .def = 1.0f, .offset = 0, .bit = GC_BIT_THROWABLE_MELEES},
    {"chrgWeps", "Chargable Weapons", "Gameplay Logic", NULL, GC_BOOL, GC_STORE_BIT, GC_DONT_CHANGE, 0, .def = 1.0f, .offset = 0, .bit = GC_BIT_NO_CHARGE_WEAPONS},
    {"selTeam", "Select Team", "Teams", "Teams", GC_BOOL, GC_STORE_BIT, GC_DONT_CHANGE, 0, .def = 0.0f, .offset = 0, .bit = GC_BIT_SELECT_TEAM},
    {"frFire", "Friendly Fire", "Teams", NULL, GC_BOOL, GC_STORE_U8, GC_DONT_CHANGE, 0, .def = 0.0f, .offset = offsetof(GameConfig, friendly_fire)},
    // v10.0.2 (CFG:7641) "Player Damage Off": v10 only draws the enemy health colour of nametags
    // from it (v10:159971); KN validates and keeps the setting, it does not draw nametags by it
    {"noPvp", "Player Damage Off", "Teams", NULL, GC_BOOL, GC_STORE_NONE, GC_DONT_CHANGE, 0, .def = 0.0f, .offset = 0},
    {"nameTeam1", "Alpha Team Name", "Teams", NULL, GC_INPUT, GC_STORE_STR, 0, GC_HT(0) | GC_HT(1), .def_str = "Alpha", .len = 16, .offset = offsetof(GameConfig, team_names[1])},
    {"nameTeam2", "Bravo Team Name", "Teams", NULL, GC_INPUT, GC_STORE_STR, 0, GC_HT(0) | GC_HT(1), .def_str = "Bravo", .len = 16, .offset = offsetof(GameConfig, team_names[2])},
    {"nameTeam3", "Charlie Team Name", "Teams", NULL, GC_INPUT, GC_STORE_STR, GC_HIDE, 0, .def_str = "Charlie", .len = 16, .offset = offsetof(GameConfig, team_names[3])},
    {"nameTeam4", "Delta Team Name", "Teams", NULL, GC_INPUT, GC_STORE_STR, GC_HIDE, 0, .def_str = "Delta", .len = 16, .offset = offsetof(GameConfig, team_names[4])},
    {"nameTeam5", "Echo Team Name", "Teams", NULL, GC_INPUT, GC_STORE_STR, GC_HIDE, 0, .def_str = "Echo", .len = 16, .offset = offsetof(GameConfig, team_names[5])},
    {"t1Dmg", "Alpha Team Damage", "Teams", NULL, GC_NUM, GC_STORE_FLOAT, GC_DONT_CHANGE, 0, .def = 1.0f, .min = 0.0f, .max = 2.0f, .step = 0.1f, .offset = offsetof(GameConfig, team_damage[1])},
    {"t2Dmg", "Bravo Team Damage", "Teams", NULL, GC_NUM, GC_STORE_FLOAT, GC_DONT_CHANGE, 0, .def = 1.0f, .min = 0.0f, .max = 2.0f, .step = 0.1f, .offset = offsetof(GameConfig, team_damage[2])},
    {"t3Dmg", "Charlie Team Damage", "Teams", NULL, GC_NUM, GC_STORE_FLOAT, GC_DONT_CHANGE | GC_HIDE, 0, .def = 1.0f, .min = 0.0f, .max = 2.0f, .step = 0.1f, .offset = offsetof(GameConfig, team_damage[3])},
    {"t4Dmg", "Delta Team Damage", "Teams", NULL, GC_NUM, GC_STORE_FLOAT, GC_DONT_CHANGE | GC_HIDE, 0, .def = 1.0f, .min = 0.0f, .max = 2.0f, .step = 0.1f, .offset = offsetof(GameConfig, team_damage[4])},
    {"t5Dmg", "Echo Team Damage", "Teams", NULL, GC_NUM, GC_STORE_FLOAT, GC_DONT_CHANGE | GC_HIDE, 0, .def = 1.0f, .min = 0.0f, .max = 2.0f, .step = 0.1f, .offset = offsetof(GameConfig, team_damage[5])},
    {"allowSpect", "Spectating", "Other", "Other", GC_BOOL, GC_STORE_BIT, 0, GC_HT(0) | GC_HT(1), .def = 1.0f, .offset = 0, .bit = GC_BIT_ALLOW_SPECTATE},
    {"thirdPerson", "3rd Person", "Other", NULL, GC_BOOL, GC_STORE_BIT, GC_DONT_CHANGE, 0, .def = 0.0f, .offset = 0, .bit = GC_BIT_THIRD_PERSON},
    {"nameTags", "Hide Nametags (Enemies)", "Other", NULL, GC_BOOL, GC_STORE_BIT, 0, 0, .def = 0.0f, .offset = 0, .bit = GC_BIT_HIDE_NAMETAGS},
    {"nameTagsFR", "Hide Nametags (Allies)", "Other", NULL, GC_BOOL, GC_STORE_U8, 0, 0, .def = 0.0f, .offset = offsetof(GameConfig, hide_nametags_allies)},
    {"kCams", "Kill Cams", "Other", NULL, GC_BOOL, GC_STORE_U8, 0, 0, .def = 1.0f, .offset = offsetof(GameConfig, kill_cams)},
    {"aAnon", "Allow Anonymous", "Other", NULL, GC_BOOL, GC_STORE_U8, 0, 0, .def = 1.0f, .offset = offsetof(GameConfig, allow_anonymous)},
    // v10.0.2 (CFG:7719) hosted games: the host passes on to another player when it leaves. Validated and
    // kept in the map; the KN host does not transfer (doc/v10/editor-social.md §1.3 "Essential")
    {"transferHostOnLeave", "Transfer Host on Leave", "Hosting", "Hosting", GC_BOOL, GC_STORE_NONE, 0, GC_HT(0) | GC_HT(1), .def = 1.0f, .offset = 0},
    {"specSlots", "Designated Spectators", "Other", NULL, GC_NUM, GC_STORE_INT, GC_BASIC | GC_HIDE_E, GC_HT(1), .def = 2.0f, .min = 0.0f, .max = 4.0f, .step = 1.0f, .offset = offsetof(GameConfig, spec_slots)},
    {"tmSize", "Team Size", "Other", NULL, GC_SELECT, GC_STORE_INT, GC_BASIC | GC_HIDE_E, GC_HT(1), .def = 3.0f, .options = k_team_size_options, .option_count = 4, .offset = offsetof(GameConfig, team_size)},
    {"noCosm", "Hide Cosmetics", "Other", NULL, GC_BOOL, GC_STORE_U8, GC_BASIC | GC_HIDE_E | GC_HAS_DEF_C, GC_HT(1), .def = 0.0f, .def_c = 1.0f, .offset = offsetof(GameConfig, no_cosmetics)},
    {"tstCmp", "Test Mode", "Other", NULL, GC_BOOL, GC_STORE_U8, GC_BASIC | GC_HIDE_E | GC_HAS_DEF_C, GC_HT(1), .def = 0.0f, .def_c = 1.0f, .offset = offsetof(GameConfig, test_mode)},
    {"noDraws", "No Draws", "Other", NULL, GC_BOOL, GC_STORE_U8, GC_BASIC | GC_HIDE_E | GC_HAS_DEF_C, GC_HT(1), .def = 0.0f, .def_c = 1.0f, .offset = offsetof(GameConfig, no_draws)},
    {"bstOfR", "Best of", "Other", NULL, GC_BOOL, GC_STORE_U8, GC_BASIC | GC_HIDE_E | GC_HAS_DEF_C, GC_HT(1), .def = 0.0f, .def_c = 1.0f, .offset = offsetof(GameConfig, best_of)},
    {"headClipFix", "Stop Head Clipping", "Other", NULL, GC_BOOL, GC_STORE_U8, GC_HAS_DEF_C, GC_HT(0) | GC_HT(1), .def = 0.0f, .def_c = 1.0f, .offset = offsetof(GameConfig, head_clip_fix)},
    // v10.0.3 "Solid Ramp Sides" (deob-10.0.3.js:20068-20075): right after headClipFix, hostTypes [0, 1], def false, defC true
    {"rampFix", "Solid Ramp Sides", "Other", NULL, GC_BOOL, GC_STORE_BIT, GC_HAS_DEF_C, GC_HT(0) | GC_HT(1), .def = 0.0f, .def_c = 1.0f, .offset = 0, .bit = GC_BIT_RAMP_FIX},
    // a comp host's webhook URL (deob-10.0.3.js:20077-20082). Validated as an input; v10's applyConfig keeps it for a
    // comp game only and takes it out of the config (deob-10.0.3.js:216122-216129), so game_config_write never sends
    // it. The server posts the match_end there (server/src/accounts.c, comp_webhook.h)
    {"webhook", "Webhook", "Other", NULL, GC_INPUT, GC_STORE_NONE, 0, GC_HT(1), .def_str = "", .len = 200, .offset = 0},
    {"minimaps", "Minimaps", "Other", NULL, GC_BOOL, GC_STORE_U8, 0, 0, .def = 1.0f, .offset = offsetof(GameConfig, minimaps)},
    // KrunkNative addition (not a v7.1.4 serverConfig key): the Object Manager's extra loadout weapons and
    // weapon pickups for every player of a custom game (shared/src/object_manager.c)
    {"knOmWeps", "Object Manager Weapons", "Other", NULL, GC_BOOL, GC_STORE_U8, 0, 0, .def = 1.0f, .offset = offsetof(GameConfig, om_weapons)},
    // KrunkNative addition (not a v7.1.4 serverConfig key): the host of a custom game lets every
    // player use the third-person camera (console `thirdperson`, doc/v7/decisions.md "Third person").
    {"knTPCam", "Third Person Camera", "Other", NULL, GC_BOOL, GC_STORE_U8, 0, 0, .def = 0.0f, .offset = offsetof(GameConfig, tp_cam)},
    // KrunkNative: v10 mode.clanWar as host options (doc/v10/ranked-impl.md "Clan wars"): clan vs clan
    {"knClanWar", "Clan War", "Teams", NULL, GC_BOOL, GC_STORE_U8, 0, 0, .def = 0.0f, .offset = offsetof(GameConfig, clan_war)},
    {"knClanA", "Alpha Clan", "Teams", NULL, GC_INPUT, GC_STORE_STR, 0, 0, .def_str = "", .len = 16, .offset = offsetof(GameConfig, clan_war_tags[0])},
    {"knClanB", "Bravo Clan", "Teams", NULL, GC_INPUT, GC_STORE_STR, 0, 0, .def_str = "", .len = 16, .offset = offsetof(GameConfig, clan_war_tags[1])},
    {"limitClasses", "Limit Classes", "Class Limits", "Class Limits", GC_BOOL, GC_STORE_U8, GC_BASIC | GC_HIDE_E | GC_HAS_DEF_C, GC_HT(1), .def = 0.0f, .def_c = 1.0f, .offset = offsetof(GameConfig, limit_classes)},
    {"classLim0", "Triggerman", "Other", NULL, GC_NUM, GC_STORE_INT, GC_BASIC | GC_HIDE_E | GC_HAS_DEF_C, GC_HT(1), .def = 0.0f, .def_c = 1.0f, .min = 0.0f, .max = 20.0f, .step = 1.0f, .offset = offsetof(GameConfig, class_limits[0])},
    {"classLim1", "Hunter", "Other", NULL, GC_NUM, GC_STORE_INT, GC_BASIC | GC_HIDE_E | GC_HAS_DEF_C, GC_HT(1), .def = 0.0f, .def_c = 1.0f, .min = 0.0f, .max = 20.0f, .step = 1.0f, .offset = offsetof(GameConfig, class_limits[1])},
    {"classLim2", "Run N Gun", "Other", NULL, GC_NUM, GC_STORE_INT, GC_BASIC | GC_HIDE_E | GC_HAS_DEF_C, GC_HT(1), .def = 0.0f, .def_c = 1.0f, .min = 0.0f, .max = 20.0f, .step = 1.0f, .offset = offsetof(GameConfig, class_limits[2])},
    {"classLim3", "Spray N Pray", "Other", NULL, GC_NUM, GC_STORE_INT, GC_BASIC | GC_HIDE_E | GC_HAS_DEF_C, GC_HT(1), .def = 0.0f, .def_c = 1.0f, .min = 0.0f, .max = 20.0f, .step = 1.0f, .offset = offsetof(GameConfig, class_limits[3])},
    {"classLim4", "Vince", "Other", NULL, GC_NUM, GC_STORE_INT, GC_BASIC | GC_HIDE_E | GC_HAS_DEF_C, GC_HT(1), .def = 0.0f, .def_c = 1.0f, .min = 0.0f, .max = 20.0f, .step = 1.0f, .offset = offsetof(GameConfig, class_limits[4])},
    {"classLim5", "Detective", "Other", NULL, GC_NUM, GC_STORE_INT, GC_BASIC | GC_HIDE_E | GC_HAS_DEF_C, GC_HT(1), .def = 0.0f, .def_c = 1.0f, .min = 0.0f, .max = 20.0f, .step = 1.0f, .offset = offsetof(GameConfig, class_limits[5])},
    {"classLim6", "Marksman", "Other", NULL, GC_NUM, GC_STORE_INT, GC_BASIC | GC_HIDE_E | GC_HAS_DEF_C, GC_HT(1), .def = 0.0f, .def_c = 1.0f, .min = 0.0f, .max = 20.0f, .step = 1.0f, .offset = offsetof(GameConfig, class_limits[6])},
    {"classLim7", "Rocketeer", "Other", NULL, GC_NUM, GC_STORE_INT, GC_BASIC | GC_HIDE_E | GC_HAS_DEF_C, GC_HT(1), .def = 0.0f, .def_c = 0.0f, .min = 0.0f, .max = 20.0f, .step = 1.0f, .offset = offsetof(GameConfig, class_limits[7])},
    {"classLim8", "Agent", "Other", NULL, GC_NUM, GC_STORE_INT, GC_BASIC | GC_HIDE_E | GC_HAS_DEF_C, GC_HT(1), .def = 0.0f, .def_c = 1.0f, .min = 0.0f, .max = 20.0f, .step = 1.0f, .offset = offsetof(GameConfig, class_limits[8])},
    {"classLim9", "Runner", "Other", NULL, GC_NUM, GC_STORE_INT, GC_BASIC | GC_HIDE_E | GC_HAS_DEF_C, GC_HT(1), .def = 0.0f, .def_c = 0.0f, .min = 0.0f, .max = 20.0f, .step = 1.0f, .offset = offsetof(GameConfig, class_limits[9])},
    {"classLim10", "Deagler", "Other", NULL, GC_NUM, GC_STORE_INT, GC_BASIC | GC_HIDE_E | GC_HAS_DEF_C, GC_HT(1), .def = 0.0f, .def_c = 0.0f, .min = 0.0f, .max = 20.0f, .step = 1.0f, .offset = offsetof(GameConfig, class_limits[10])},
    {"classLim11", "Bowman", "Other", NULL, GC_NUM, GC_STORE_INT, GC_BASIC | GC_HIDE_E | GC_HAS_DEF_C, GC_HT(1), .def = 0.0f, .def_c = 0.0f, .min = 0.0f, .max = 20.0f, .step = 1.0f, .offset = offsetof(GameConfig, class_limits[11])},
    {"classLim12", "Commando", "Other", NULL, GC_NUM, GC_STORE_INT, GC_BASIC | GC_HIDE_E | GC_HAS_DEF_C, GC_HT(1), .def = 0.0f, .def_c = 1.0f, .min = 0.0f, .max = 20.0f, .step = 1.0f, .offset = offsetof(GameConfig, class_limits[12])},
    {"classLim13", "Trooper", "Other", NULL, GC_NUM, GC_STORE_INT, GC_BASIC | GC_HIDE_E | GC_HAS_DEF_C, GC_HT(1), .def = 0.0f, .def_c = 1.0f, .min = 0.0f, .max = 20.0f, .step = 1.0f, .offset = offsetof(GameConfig, class_limits[13])},
    {"classLim14", "Survivor", "Other", NULL, GC_NUM, GC_STORE_INT, GC_BASIC | GC_HIDE_E | GC_HAS_DEF_C, GC_HT(1), .def = 0.0f, .def_c = 0.0f, .min = 0.0f, .max = 20.0f, .step = 1.0f, .offset = offsetof(GameConfig, class_limits[14])},
    {"classLim15", "Infiltrator", "Other", NULL, GC_NUM, GC_STORE_INT, GC_BASIC | GC_HIDE_E | GC_HAS_DEF_C, GC_HT(1), .def = 0.0f, .def_c = 1.0f, .min = 0.0f, .max = 20.0f, .step = 1.0f, .offset = offsetof(GameConfig, class_limits[15])},
};

// classConfig camType (deob-10.0.3.js:20162-20168): the values 0..4 are controls.camType isNormal / isTop /
// isSideX / isSideZ / isOrto (deob-10.0.3.js:257869-257873); 1 "3rd Person" is v3.1.1's "Top Down" value.
static const char *const k_cls_camType_options[] = {"FPS", "3rd Person", "Platformer (X)", "Platformer (Z)", "Orthographic"};
static const char *const k_cls_mdl_options[] = {"Default", "Sprite", "Model"};
// classConfig movSch (deob-10.0.3.js:20337-20344): the game.movement indexes (MovementScheme, shared.h)
static const char *const k_cls_movSch_options[] = {"Default", "Follow Mouse", "Move Right", "Fly", "Dir Only", "SRM", "Vehicle", "MMORPG"};

// config.classConfig (79 entries).
static const GameConfigOption k_class_options[] = {
    {"name", "Name", NULL, NULL, GC_INPUT, GC_STORE_CLS_NAME, 0, 0, .def = 0.0f, .def_str = "", .len = 24, .cat = -1, .offset = 0, .bit = 0, .unit = U_SAME},
    {"camType", "Type", NULL, NULL, GC_SELECT, GC_STORE_EXT_INT, GC_HAS_DEF, 0, .def = 0.0f, .options = k_cls_camType_options, .option_count = 5, .cat = 0, .offset = offsetof(ClassOverrides, cam_type), .bit = 0, .unit = U_SAME},
    {"camOffX", "X Offset", NULL, NULL, GC_NUM, GC_STORE_EXT_FLOAT, GC_HAS_DEF, 0, .def = 0.0f, .min = 0.0f, .max = 500.0f, .step = 1.0f, .cat = 0, .offset = offsetof(ClassOverrides, cam_offset[0]), .bit = 0, .unit = U_SAME},
    {"camOffY", "Y Offset", NULL, NULL, GC_NUM, GC_STORE_EXT_FLOAT, GC_HAS_DEF, 0, .def = 0.0f, .min = 0.0f, .max = 500.0f, .step = 1.0f, .cat = 0, .offset = offsetof(ClassOverrides, cam_offset[1]), .bit = 0, .unit = U_SAME},
    {"camOffZ", "Z Offset", NULL, NULL, GC_NUM, GC_STORE_EXT_FLOAT, GC_HAS_DEF, 0, .def = 0.0f, .min = 0.0f, .max = 500.0f, .step = 1.0f, .cat = 0, .offset = offsetof(ClassOverrides, cam_offset[2]), .bit = 0, .unit = U_SAME},
    {"orthoF", "Ortho Frustum Size", NULL, NULL, GC_NUM, GC_STORE_EXT_FLOAT, GC_HAS_DEF, 0, .def = 50.0f, .min = 1.0f, .max = 1000.0f, .step = 1.0f, .cat = 0, .offset = offsetof(ClassOverrides, ortho_f), .bit = 0, .unit = U_SAME},
    {"lockCamP", "Face Player", NULL, NULL, GC_NUM, GC_STORE_EXT_FLOAT, GC_HAS_DEF, 0, .def = 0.0f, .min = 0.0f, .max = 1.0f, .step = 0.01f, .cat = 0, .offset = offsetof(ClassOverrides, lock_cam_pitch), .bit = 0, .unit = U_SAME},
    {"lckCmYR", "Lock Y Rot", NULL, NULL, GC_NUM, GC_STORE_MOVE, GC_HAS_DEF, 0, .def = 0.0f, .min = -1.0f, .max = 1.0f, .step = 0.01f, .cat = 0, .offset = 0, .bit = CM_LOCK_PITCH, .unit = U_SAME},
    {"yCmLim", "Y Limit", NULL, NULL, GC_NUM, GC_STORE_EXT_FLOAT, GC_HAS_DEF, 0, .def = 0.0f, .min = 0.0f, .max = 1.0f, .step = 0.01f, .cat = 0, .offset = offsetof(ClassOverrides, y_cam_limit), .bit = 0, .unit = U_SAME},
    {"camRot", "Mouse Rotation", NULL, NULL, GC_BOOL, GC_STORE_EXT_U8, GC_HAS_DEF, 0, .def = 1.0f, .cat = 0, .offset = offsetof(ClassOverrides, cam_rotation), .bit = 0, .unit = U_SAME},
    {"fPLock", "Force Pointer Lock", NULL, NULL, GC_BOOL, GC_STORE_EXT_U8, GC_HAS_DEF, 0, .def = 0.0f, .cat = 0, .offset = offsetof(ClassOverrides, fp_lock), .bit = 0, .unit = U_SAME},
    {"mdl", "Player Type", NULL, NULL, GC_SELECT, GC_STORE_EXT_INT, GC_HAS_DEF, 0, .def = 0.0f, .options = k_cls_mdl_options, .option_count = 3, .cat = 1, .offset = offsetof(ClassOverrides, model), .bit = 0, .unit = U_SAME},
    {"aID", "Custom Asset", NULL, NULL, GC_INPUT, GC_STORE_EXT_STR, GC_HAS_DEF, 0, .def = 0.0f, .def_str = "", .len = 12, .cat = 1, .offset = offsetof(ClassOverrides, asset_id), .bit = 0, .unit = U_SAME},
    {"mdlFP", "Show First Person", NULL, NULL, GC_BOOL, GC_STORE_EXT_U8, GC_HAS_DEF, 0, .def = 1.0f, .cat = 1, .offset = offsetof(ClassOverrides, model_fp), .bit = 0, .unit = U_SAME},
    {"animS", "Asset Anim Speed", NULL, NULL, GC_NUM, GC_STORE_EXT_FLOAT, GC_HAS_DEF, 0, .def = 1.0f, .min = 0.1f, .max = 10.0f, .step = 0.1f, .cat = 1, .offset = offsetof(ClassOverrides, anim_speed), .bit = 0, .unit = U_SAME},
    {"mdlS", "Asset Scale", NULL, NULL, GC_NUM, GC_STORE_EXT_FLOAT, GC_HAS_DEF, 0, .def = 1.0f, .min = 0.1f, .max = 100.0f, .step = 0.1f, .cat = 1, .offset = offsetof(ClassOverrides, model_scale), .bit = 0, .unit = U_SAME},
    {"mdPS", "Asset Preview Scale", NULL, NULL, GC_NUM, GC_STORE_EXT_FLOAT, GC_HAS_DEF, 0, .def = 1.0f, .min = 0.01f, .max = 10.0f, .step = 0.01f, .cat = 1, .offset = offsetof(ClassOverrides, model_pos_scale), .bit = 0, .unit = U_SAME},
    {"mdPY", "Asset Preview Y Off", NULL, NULL, GC_NUM, GC_STORE_EXT_FLOAT, GC_HAS_DEF, 0, .def = 0.0f, .min = -50.0f, .max = 50.0f, .step = 0.01f, .cat = 1, .offset = offsetof(ClassOverrides, model_pos_y), .bit = 0, .unit = U_SAME},
    {"mdlDr", "Asset Dir", NULL, NULL, GC_NUM, GC_STORE_EXT_FLOAT, GC_HAS_DEF, 0, .def = 0.0f, .min = 0.0f, .max = 360.0f, .step = 5.0f, .cat = 1, .offset = offsetof(ClassOverrides, model_dir), .bit = 0, .unit = U_SAME},
    {"mdlYO", "Asset Y Offset", NULL, NULL, GC_NUM, GC_STORE_EXT_FLOAT, GC_HAS_DEF, 0, .def = 0.0f, .min = -50.0f, .max = 50.0f, .step = 0.01f, .cat = 1, .offset = offsetof(ClassOverrides, model_y_offset), .bit = 0, .unit = U_SAME},
    {"movSch", "Move Scheme", NULL, NULL, GC_SELECT, GC_STORE_MOVE, GC_HAS_DEF, 0, .def = 0.0f, .options = k_cls_movSch_options, .option_count = 8, .cat = 1, .offset = 0, .bit = CM_SCHEME, .unit = U_SAME},
    {"vhdr", "Drag", NULL, NULL, GC_NUM, GC_STORE_EXT_FLOAT, GC_HAS_DEF, 0, .def = 0.97f, .min = 0.001f, .max = 0.999f, .step = 0.001f, .cat = 1, .offset = offsetof(ClassOverrides, vehicle_drag), .bit = 0, .unit = U_SAME},
    {"vhaDr", "Angular Drag", NULL, NULL, GC_NUM, GC_STORE_EXT_FLOAT, GC_HAS_DEF, 0, .def = 0.94f, .min = 0.001f, .max = 0.999f, .step = 0.001f, .cat = 1, .offset = offsetof(ClassOverrides, vehicle_angular_drag), .bit = 0, .unit = U_SAME},
    {"vhtL", "Turn Lean", NULL, NULL, GC_NUM, GC_STORE_EXT_FLOAT, GC_HAS_DEF, 0, .def = -2.7f, .min = -100.0f, .max = 100.0f, .step = 0.1f, .cat = 1, .offset = offsetof(ClassOverrides, vehicle_turn_lean), .bit = 0, .unit = U_SAME},
    {"vhtrsh", "Turn Threshold", NULL, NULL, GC_NUM, GC_STORE_EXT_FLOAT, GC_HAS_DEF, 0, .def = 0.0025f, .min = 0.0f, .max = 0.01f, .step = 0.0001f, .cat = 1, .offset = offsetof(ClassOverrides, vehicle_turn_threshold), .bit = 0, .unit = U_SAME},
    {"vhtrsv", "Turn Conv", NULL, NULL, GC_NUM, GC_STORE_EXT_FLOAT, GC_HAS_DEF, 0, .def = 0.0f, .min = 0.0f, .max = 1.0f, .step = 0.01f, .cat = 1, .offset = offsetof(ClassOverrides, vehicle_turn_conv), .bit = 0, .unit = U_SAME},
    {"vhmP", "Max Power", NULL, NULL, GC_NUM, GC_STORE_EXT_FLOAT, GC_HAS_DEF, 0, .def = 0.12f, .min = 0.0f, .max = 1.0f, .step = 0.001f, .cat = 1, .offset = offsetof(ClassOverrides, vehicle_max_power), .bit = 0, .unit = U_SAME},
    {"vhpF", "Power Force", NULL, NULL, GC_NUM, GC_STORE_EXT_FLOAT, GC_HAS_DEF, 0, .def = 0.0012f, .min = 0.0f, .max = 0.1f, .step = 0.0001f, .cat = 1, .offset = offsetof(ClassOverrides, vehicle_power_force), .bit = 0, .unit = U_SAME},
    {"vhmR", "Max Reverse", NULL, NULL, GC_NUM, GC_STORE_EXT_FLOAT, GC_HAS_DEF, 0, .def = 0.0375f, .min = 0.0f, .max = 1.0f, .step = 0.0001f, .cat = 1, .offset = offsetof(ClassOverrides, vehicle_max_reverse), .bit = 0, .unit = U_SAME},
    {"vhrF", "Reverse Force", NULL, NULL, GC_NUM, GC_STORE_EXT_FLOAT, GC_HAS_DEF, 0, .def = 0.0005f, .min = 0.0f, .max = 0.1f, .step = 0.0001f, .cat = 1, .offset = offsetof(ClassOverrides, vehicle_reverse_force), .bit = 0, .unit = U_SAME},
    {"speed", "Speed", NULL, NULL, GC_NUM, GC_STORE_CLS_FLOAT, 0, 0, .def = 0.0f, .min = 0.01f, .max = 5.0f, .step = 0.01f, .cat = 1, .offset = offsetof(ClassConfig, speed), .bit = 0, .unit = U_SAME},
    {"speedX", "Speed X", NULL, NULL, GC_NUM, GC_STORE_MOVE, GC_HAS_DEF, 0, .def = 1.0f, .min = -50.0f, .max = 50.0f, .step = 0.1f, .cat = 1, .offset = 0, .bit = CM_SPEED_X, .unit = U_SAME},
    {"speedY", "Speed Y", NULL, NULL, GC_NUM, GC_STORE_MOVE, GC_HAS_DEF, 0, .def = 1.0f, .min = -50.0f, .max = 50.0f, .step = 0.1f, .cat = 1, .offset = 0, .bit = CM_SPEED_Y, .unit = U_SAME},
    {"speedZ", "Speed Z", NULL, NULL, GC_NUM, GC_STORE_MOVE, GC_HAS_DEF, 0, .def = 1.0f, .min = -50.0f, .max = 50.0f, .step = 0.1f, .cat = 1, .offset = 0, .bit = CM_SPEED_Z, .unit = U_SAME},
    {"spdLnTh", "SpeedLines Threshold", NULL, NULL, GC_NUM, GC_STORE_EXT_FLOAT, GC_HAS_DEF, 0, .def = 500.0f, .min = 1.0f, .max = 500.0f, .step = 1.0f, .cat = 1, .offset = offsetof(ClassOverrides, speed_length), .bit = 0, .unit = U_SAME},
    {"movDrP", "Move Dir", NULL, NULL, GC_NUM, GC_STORE_MOVE, GC_HAS_DEF, 0, .def = 0.0f, .min = -1.0f, .max = 1.0f, .step = 0.1f, .cat = 1, .offset = 0, .bit = CM_MOVE_DIR_OFFSET, .unit = U_SAME},
    {"trnSp", "Turn Speed", NULL, NULL, GC_NUM, GC_STORE_MOVE, GC_HAS_DEF, 0, .def = 1.0f, .min = 0.0f, .max = 2.0f, .step = 0.01f, .cat = 1, .offset = 0, .bit = CM_TURN_RATE, .unit = U_SAME},
    {"grA", "Ground Accel", NULL, NULL, GC_NUM, GC_STORE_MOVE, GC_HAS_DEF, 0, .def = 0.042f, .min = 0.0001f, .max = 0.1f, .step = 0.0001f, .cat = 1, .offset = 0, .bit = CM_GROUND_ACCEL, .unit = U_SAME},
    {"grDec", "Ground Decel", NULL, NULL, GC_NUM, GC_STORE_MOVE, GC_HAS_DEF, 0, .def = 0.99f, .min = 0.9f, .max = 0.9999f, .step = 0.0001f, .cat = 1, .offset = 0, .bit = CM_GROUND_DECEL, .unit = U_SAME},
    {"arA", "Air Accel", NULL, NULL, GC_NUM, GC_STORE_MOVE, GC_HAS_DEF, 0, .def = 0.0047f, .min = 0.0001f, .max = 0.1f, .step = 0.0001f, .cat = 1, .offset = 0, .bit = CM_AIR_ACCEL, .unit = U_SAME},
    {"airDec", "Air Decel", NULL, NULL, GC_NUM, GC_STORE_MOVE, GC_HAS_DEF, 0, .def = 0.9996f, .min = 0.9f, .max = 0.9999f, .step = 0.0001f, .cat = 1, .offset = 0, .bit = CM_AIR_DECEL, .unit = U_SAME},
    {"canCrch", "Can Crouch", NULL, NULL, GC_BOOL, GC_STORE_MOVE, GC_HAS_DEF, 0, .def = 1.0f, .cat = 1, .offset = 0, .bit = CM_CAN_CROUCH, .unit = U_SAME},
    {"crchSpd", "Crouch Speed", NULL, NULL, GC_NUM, GC_STORE_MOVE, GC_HAS_DEF, 0, .def = 0.3f, .min = 0.0f, .max = 4.0f, .step = 0.1f, .cat = 1, .offset = 0, .bit = CM_CROUCH_SPEED, .unit = U_SAME},
    {"ldA", "Ladder Accel", NULL, NULL, GC_NUM, GC_STORE_MOVE, GC_HAS_DEF, 0, .def = 1.0f, .min = 0.1f, .max = 5.0f, .step = 0.1f, .cat = 1, .offset = 0, .bit = CM_LADDER_ACCEL, .unit = U_SAME},
    {"slT", "Slide Time", NULL, NULL, GC_NUM, GC_STORE_CLS_FLOAT, 0, 0, .def = 0.0f, .min = 0.01f, .max = 3.0f, .step = 0.01f, .cat = 1, .offset = offsetof(ClassConfig, slide_time_mlt), .bit = 0, .unit = U_SAME},
    {"slA", "Slide Accel", NULL, NULL, GC_NUM, GC_STORE_CLS_FLOAT, 0, 0, .def = 0.0f, .min = 0.01f, .max = 1.5f, .step = 0.01f, .cat = 1, .offset = offsetof(ClassConfig, slide_accel_mlt), .bit = 0, .unit = U_SAME},
    {"frSlid", "Free Range Sliding", NULL, NULL, GC_BOOL, GC_STORE_MOVE, GC_HAS_DEF, 0, .def = 0.0f, .cat = 1, .offset = 0, .bit = CM_FREE_SLIDE, .unit = U_SAME},
    {"bkSlid", "Backwards Sliding", NULL, NULL, GC_BOOL, GC_STORE_CLS_BIT, 0, 0, .def = 0.0f, .cat = 1, .offset = 0, .bit = CB_BACK_SLIDE, .unit = U_SAME},
    {"stpFr", "Step Frequency", NULL, NULL, GC_NUM, GC_STORE_MOVE, GC_HAS_DEF, 0, .def = 1.0f, .min = 0.1f, .max = 20.0f, .step = 0.1f, .cat = 1, .offset = 0, .bit = CM_STEP_FREQ, .unit = U_SAME},
    {"stpSD", "Step Sound Dynamic", NULL, NULL, GC_NUM, GC_STORE_EXT_FLOAT, GC_HAS_DEF, 0, .def = 0.0f, .min = 0.0f, .max = 1.0f, .step = 0.01f, .cat = 1, .offset = offsetof(ClassOverrides, step_sd), .bit = 0, .unit = U_SAME},
    {"health", "Health", NULL, NULL, GC_NUM, GC_STORE_CLS_INT, 0, 0, .def = 0.0f, .min = 1.0f, .max = 5000.0f, .step = 1.0f, .cat = -1, .offset = offsetof(ClassConfig, health), .bit = 0, .unit = U_SAME},
    {"segs", "HP Segments", NULL, NULL, GC_NUM, GC_STORE_CLS_INT, 0, 0, .def = 0.0f, .min = 1.0f, .max = 10.0f, .step = 1.0f, .cat = -1, .offset = offsetof(ClassConfig, health_segments), .bit = 0, .unit = U_SAME},
    {"regen", "Regen", NULL, NULL, GC_NUM, GC_STORE_CLS_FLOAT, 0, 0, .def = 0.0f, .min = 0.01f, .max = 3.0f, .step = 0.01f, .cat = -1, .offset = offsetof(ClassConfig, regen), .bit = 0, .unit = U_SAME},
    {"regenDelay", "Regen Delay", NULL, NULL, GC_NUM, GC_STORE_EXT_FLOAT, 0, 0, .def = 0.0f, .min = 0.0f, .max = 10000.0f, .step = 1.0f, .cat = -1, .offset = offsetof(ClassOverrides, regen_delay), .bit = 0, .unit = U_MS},
    {"hbS", "Hitbox Scale", NULL, NULL, GC_NUM, GC_STORE_CLS_FLOAT, GC_HAS_DEF, 0, .def = 0.0f, .min = 0.0f, .max = 20.0f, .step = 0.1f, .cat = 1, .offset = offsetof(ClassConfig, hitbox_pad_w), .bit = 0, .unit = U_SAME},
    {"hbH", "Hitbox Height", NULL, NULL, GC_NUM, GC_STORE_CLS_FLOAT, GC_HAS_DEF, 0, .def = 0.0f, .min = 0.0f, .max = 20.0f, .step = 0.1f, .cat = 1, .offset = offsetof(ClassConfig, hitbox_pad_h), .bit = 0, .unit = U_SAME},
    // v10.0.2 (v10:17699 the class "Hurtbox Scale", sub "Hitbox", cat 1)
    {"hurtboxScale", "Hurtbox Scale", NULL, NULL, GC_NUM, GC_STORE_CLS_FLOAT, GC_HAS_DEF, 0, .def = 1.0f, .min = 1.0f, .max = 2.0f, .step = 0.1f, .cat = 1, .offset = offsetof(ClassConfig, hurtbox_scale), .bit = 0, .unit = U_SAME},
    {"jpC", "Jump Cooldown", NULL, NULL, GC_NUM, GC_STORE_EXT_FLOAT, GC_HAS_DEF, 0, .def = 1.0f, .min = 0.1f, .max = 3.0f, .step = 0.1f, .cat = 1, .offset = offsetof(ClassOverrides, jump_cooldown), .bit = 0, .unit = U_SAME},
    {"disJump", "Disable Jump", NULL, NULL, GC_BOOL, GC_STORE_MOVE, GC_HAS_DEF, 0, .def = 0.0f, .cat = 1, .offset = 0, .bit = CM_DISABLE_JUMP, .unit = U_SAME},
    {"infJ", "Infinite Jump", NULL, NULL, GC_BOOL, GC_STORE_MOVE, GC_HAS_DEF, 0, .def = 0.0f, .cat = 1, .offset = 0, .bit = CM_INFINITE_JUMP, .unit = U_SAME},
    {"ovrJump", "Override Jump", NULL, NULL, GC_BOOL, GC_STORE_MOVE, GC_HAS_DEF, 0, .def = 0.0f, .cat = 1, .offset = 0, .bit = CM_OVERRIDE_JUMP, .unit = U_SAME},
    {"canShoot", "Can Shoot", NULL, NULL, GC_BOOL, GC_STORE_EXT_U8, GC_HAS_DEF, 0, .def = 1.0f, .cat = 1, .offset = offsetof(ClassOverrides, can_shoot), .bit = 0, .unit = U_SAME},
    {"shtToCam", "Shoot to Camera", NULL, NULL, GC_BOOL, GC_STORE_EXT_U8, GC_HAS_DEF, 0, .def = 0.0f, .cat = 1, .offset = offsetof(ClassOverrides, shoot_to_cam), .bit = 0, .unit = U_SAME},
    {"shtDrg", "Shot Dir Drag", NULL, NULL, GC_NUM, GC_STORE_EXT_FLOAT, GC_HAS_DEF, 0, .def = 1.0f, .min = 0.01f, .max = 1.0f, .step = 0.01f, .cat = 1, .offset = offsetof(ClassOverrides, shoot_drag), .bit = 0, .unit = U_SAME},
    {"shotX", "Shot Dir XOff", NULL, NULL, GC_NUM, GC_STORE_EXT_FLOAT, GC_HAS_DEF, 0, .def = 0.0f, .min = 0.0f, .max = 360.0f, .step = 5.0f, .cat = 1, .offset = offsetof(ClassOverrides, shot_x), .bit = 0, .unit = U_SAME},
    {"shotY", "Shot Dir YOff", NULL, NULL, GC_NUM, GC_STORE_EXT_FLOAT, GC_HAS_DEF, 0, .def = 0.0f, .min = 0.0f, .max = 360.0f, .step = 5.0f, .cat = 1, .offset = offsetof(ClassOverrides, shot_y), .bit = 0, .unit = U_SAME},
    {"hipF", "Can Hipfire", NULL, NULL, GC_BOOL, GC_STORE_EXT_U8, GC_HAS_DEF, 0, .def = 1.0f, .cat = -1, .offset = offsetof(ClassOverrides, hip_fire), .bit = 0, .unit = U_SAME},
    {"croSh", "Crouch Shooting", NULL, NULL, GC_BOOL, GC_STORE_EXT_U8, GC_HAS_DEF, 0, .def = 1.0f, .cat = -1, .offset = offsetof(ClassOverrides, crouch_shoot), .bit = 0, .unit = U_SAME},
    {"airSh", "Mid Air Shooting", NULL, NULL, GC_BOOL, GC_STORE_EXT_U8, GC_HAS_DEF, 0, .def = 1.0f, .cat = -1, .offset = offsetof(ClassOverrides, air_shoot), .bit = 0, .unit = U_SAME},
    {"pColl", "Player Collision", NULL, NULL, GC_BOOL, GC_STORE_MOVE, GC_HAS_DEF, 0, .def = 0.0f, .cat = 1, .offset = 0, .bit = CM_PLAYER_COLLISION, .unit = U_SAME},
    {"canMelee", "Can Melee", NULL, NULL, GC_BOOL, GC_STORE_EXT_U8, GC_HAS_DEF, 0, .def = 1.0f, .cat = 1, .offset = offsetof(ClassOverrides, can_melee), .bit = 0, .unit = U_SAME},
    {"throwChrg", "Melee Charge Time", NULL, NULL, GC_NUM, GC_STORE_CLS_FLOAT, 0, 0, .def = 0.0f, .min = 0.0f, .max = 5000.0f, .step = 10.0f, .cat = 1, .offset = offsetof(ClassConfig, throw_charge), .bit = 0, .unit = U_MS},
    {"melRate", "Melee Rate Multiplier", NULL, NULL, GC_NUM, GC_STORE_CLS_FLOAT, 0, 0, .def = 0.0f, .min = 0.1f, .max = 5.0f, .step = 0.1f, .cat = 1, .offset = offsetof(ClassConfig, melee_rate), .bit = 0, .unit = U_SAME},
    {"wallJ", "Can Wall Jump", NULL, NULL, GC_BOOL, GC_STORE_CLS_BIT, 0, 0, .def = 0.0f, .cat = -1, .offset = 0, .bit = CB_WALL_JUMP, .unit = U_SAME},
    {"wallJM", "Walljumps Max", NULL, NULL, GC_NUM, GC_STORE_CLS_INT, 0, 0, .def = 0.0f, .min = 0.0f, .max = 100.0f, .step = 1.0f, .cat = 1, .offset = offsetof(ClassConfig, wall_jump_max), .bit = 0, .unit = U_SAME},
    {"wallJD", "Wall Jump Decel", NULL, NULL, GC_NUM, GC_STORE_MOVE, GC_HAS_DEF, 0, .def = 0.3f, .min = 0.0f, .max = 1.0f, .step = 0.1f, .cat = -1, .offset = 0, .bit = CM_WALL_JUMP_DECEL, .unit = U_SAME},
    {"wallJV", "Wall Jump Velocity", NULL, NULL, GC_NUM, GC_STORE_MOVE, GC_HAS_DEF, 0, .def = 0.03f, .min = 0.01f, .max = 0.1f, .step = 0.01f, .cat = -1, .offset = 0, .bit = CM_WALL_JUMP_VEL, .unit = U_SAME},
    {"secondary", "Has Secondary", NULL, NULL, GC_BOOL, GC_STORE_CLS_BIT, 0, 0, .def = 0.0f, .cat = -1, .offset = 0, .bit = CB_SECONDARY, .unit = U_SAME},
    {"cID", "Custom Crosshair", NULL, NULL, GC_INPUT, GC_STORE_EXT_STR, GC_HAS_DEF, 0, .def = 0.0f, .def_str = "", .len = 12, .cat = 1, .offset = offsetof(ClassOverrides, crosshair_id), .bit = 0, .unit = U_SAME},
    {"ccID", "Chase Crosshair", NULL, NULL, GC_INPUT, GC_STORE_EXT_STR, GC_HAS_DEF, 0, .def = 0.0f, .def_str = "", .len = 12, .cat = 1, .offset = offsetof(ClassOverrides, crosshair_color_id), .bit = 0, .unit = U_SAME},
};

// config.weaponConfig (48 entries).
static const GameConfigOption k_weapon_options[] = {
    {"name", "Name", NULL, NULL, GC_INPUT, GC_STORE_WEP_NAME, 0, 0, .def = 0.0f, .def_str = "", .len = 24, .cat = -1, .offset = 0, .bit = 0, .unit = U_SAME},
    {"dmg", "Damage", NULL, NULL, GC_NUM, GC_STORE_WEP_FLOAT, 0, 0, .def = 0.0f, .min = -200.0f, .max = 200.0f, .step = 1.0f, .cat = -1, .offset = offsetof(Weapon, damage), .bit = 0, .unit = U_SAME},
    {"hsMlt", "Headshot Mlt", NULL, NULL, GC_NUM, GC_STORE_WEP_FLOAT, 0, 0, .def = 0.0f, .min = 0.5f, .max = 3.0f, .step = 0.0001f, .cat = -1, .offset = offsetof(Weapon, headshot_mlt), .bit = 0, .unit = U_SAME},
    {"lsMlt", "Legshot Mlt", NULL, NULL, GC_NUM, GC_STORE_WEP_FLOAT, 0, 0, .def = 0.0f, .min = 0.01f, .max = 3.0f, .step = 0.0001f, .cat = -1, .offset = offsetof(Weapon, leg_mlt), .bit = 0, .unit = U_SAME},
    {"ammo", "Ammo", NULL, NULL, GC_NUM, GC_STORE_WEP_UINT, 0, 0, .def = 0.0f, .min = 1.0f, .max = 500.0f, .step = 1.0f, .cat = -1, .offset = offsetof(Weapon, ammo), .bit = 0, .unit = U_SAME},
    {"rate", "Rate", NULL, NULL, GC_NUM, GC_STORE_WEP_FLOAT, 0, 0, .def = 0.0f, .min = 70.0f, .max = 1000.0f, .step = 5.0f, .cat = -1, .offset = offsetof(Weapon, rate), .bit = 0, .unit = U_MS},
    {"burst", "Is Burst", NULL, NULL, GC_BOOL, GC_STORE_WEP_BIT, 0, 0, .def = 0.0f, .cat = -1, .offset = 0, .bit = WB_BURST, .unit = U_SAME},
    {"burstC", "Burst Count", NULL, NULL, GC_NUM, GC_STORE_WEP_UINT, 0, 0, .def = 0.0f, .min = 0.0f, .max = 10.0f, .step = 1.0f, .cat = -1, .offset = offsetof(Weapon, burst_count), .bit = 0, .unit = U_SAME},
    {"burstR", "Burst Rate", NULL, NULL, GC_NUM, GC_STORE_WEP_FLOAT, 0, 0, .def = 0.0f, .min = 50.0f, .max = 1000.0f, .step = 10.0f, .cat = -1, .offset = offsetof(Weapon, burst_rate), .bit = 0, .unit = U_MS},
    {"reload", "Reload Time", NULL, NULL, GC_NUM, GC_STORE_WEP_FLOAT, 0, 0, .def = 0.0f, .min = 500.0f, .max = 5000.0f, .step = 25.0f, .cat = -1, .offset = offsetof(Weapon, reload_time), .bit = 0, .unit = U_MS},
    {"reloY", "Reload Y", NULL, NULL, GC_NUM, GC_STORE_NONE, 0, 0, .def = 0.0f, .min = 0.0f, .max = 1.0f, .step = 0.01f, .cat = -1, .offset = 0, .bit = 0, .unit = U_SAME},
    {"swapTime", "Swap Time", NULL, NULL, GC_NUM, GC_STORE_WEP_FLOAT, 0, 0, .def = 0.0f, .min = 100.0f, .max = 1000.0f, .step = 10.0f, .cat = -1, .offset = offsetof(Weapon, swap_time), .bit = 0, .unit = U_MS},
    {"aimSpd", "Aim Speed", NULL, NULL, GC_NUM, GC_STORE_WEP_FLOAT, 0, 0, .def = 0.0f, .min = 100.0f, .max = 500.0f, .step = 10.0f, .cat = -1, .offset = offsetof(Weapon, aim_speed), .bit = 0, .unit = U_MS},
    {"ovrChrg", "Overcharge", NULL, NULL, GC_BOOL, GC_STORE_WEP_BIT, 0, 0, .def = 0.0f, .cat = -1, .offset = 0, .bit = WB_OVER_CHARGE, .unit = U_SAME},
    {"chrgTime", "Charge Time", NULL, NULL, GC_NUM, GC_STORE_WEP_FLOAT, 0, 0, .def = 0.0f, .min = 0.0f, .max = 10000.0f, .step = 10.0f, .cat = -1, .offset = offsetof(Weapon, charge_time), .bit = 0, .unit = U_MS},
    {"spdMlt", "Move Speed", NULL, NULL, GC_NUM, GC_STORE_WEP_FLOAT, 0, 0, .def = 0.0f, .min = 0.5f, .max = 3.0f, .step = 0.01f, .cat = -1, .offset = offsetof(Weapon, speed_mlt), .bit = 0, .unit = U_SAME},
    {"pierce", "Pierce", NULL, NULL, GC_NUM, GC_STORE_WEP_FLOAT, 0, 0, .def = 0.0f, .min = 0.0f, .max = 1.0f, .step = 0.05f, .cat = -1, .offset = offsetof(Weapon, pierce), .bit = 0, .unit = U_SAME},
    {"range", "Range", NULL, NULL, GC_NUM, GC_STORE_WEP_FLOAT, 0, 0, .def = 0.0f, .min = 5.0f, .max = 2000.0f, .step = 1.0f, .cat = -1, .offset = offsetof(Weapon, range), .bit = 0, .unit = U_SAME},
    {"dmgDrop", "Damage Drop", NULL, NULL, GC_NUM, GC_STORE_WEP_FLOAT, 0, 0, .def = 0.0f, .min = 0.0f, .max = 100.0f, .step = 1.0f, .cat = -1, .offset = offsetof(Weapon, damage_drop), .bit = 0, .unit = U_SAME},
    {"movSpread", "Move Spread", NULL, NULL, GC_NUM, GC_STORE_WEP_FLOAT, 0, 0, .def = 0.0f, .min = 0.0f, .max = 3.0f, .step = 0.01f, .cat = -1, .offset = offsetof(Weapon, move_spread), .bit = 0, .unit = U_SAME},
    {"adSpread", "Ads Spread", NULL, NULL, GC_NUM, GC_STORE_WEP_FLOAT, 0, 0, .def = 0.0f, .min = 0.0f, .max = 1.0f, .step = 0.01f, .cat = -1, .offset = offsetof(Weapon, aim_spread), .bit = 0, .unit = U_SAME},
    {"spread", "Spread", NULL, NULL, GC_NUM, GC_STORE_WEP_FLOAT, 0, 0, .def = 0.0f, .min = 0.0f, .max = 500.0f, .step = 0.1f, .cat = -1, .offset = offsetof(Weapon, spread), .bit = 0, .unit = U_SAME},
    {"minSpread", "Min Spread", NULL, NULL, GC_NUM, GC_STORE_WEP_FLOAT, 0, 0, .def = 0.0f, .min = 0.0f, .max = 100.0f, .step = 0.1f, .cat = -1, .offset = offsetof(Weapon, min_spread), .bit = 0, .unit = U_SAME},
    {"zoom", "Zoom", NULL, NULL, GC_NUM, GC_STORE_WEP_FLOAT, 0, 0, .def = 0.0f, .min = 1.0f, .max = 15.0f, .step = 0.01f, .cat = -1, .offset = offsetof(Weapon, zoom), .bit = 0, .unit = U_SAME},
    {"recoil", "Recoil", NULL, NULL, GC_NUM, GC_STORE_WEP_FLOAT, 0, 0, .def = 0.0f, .min = 0.0f, .max = 0.05f, .step = 0.0001f, .cat = -1, .offset = offsetof(Weapon, recoil), .bit = 0, .unit = U_X1000},
    {"recoilYM", "Recoil Y", NULL, NULL, GC_NUM, GC_STORE_WEP_FLOAT, 0, 0, .def = 0.0f, .min = 0.0f, .max = 2.0f, .step = 0.01f, .cat = -1, .offset = offsetof(Weapon, recoil_y_mlt), .bit = 0, .unit = U_SAME},
    {"recoilZ", "Recoil Z", NULL, NULL, GC_NUM, GC_STORE_WEP_FLOAT, 0, 0, .def = 0.0f, .min = 0.0f, .max = 10.0f, .step = 0.1f, .cat = -1, .offset = offsetof(Weapon, recoil_z), .bit = 0, .unit = U_SAME},
    {"recoilZM", "Recoil Z Aim", NULL, NULL, GC_NUM, GC_STORE_WEP_FLOAT, 0, 0, .def = 0.0f, .min = -1.0f, .max = 1.0f, .step = 0.01f, .cat = -1, .offset = offsetof(Weapon, recoil_z_mlt), .bit = 0, .unit = U_SAME},
    {"jumpYM", "Jump Power", NULL, NULL, GC_NUM, GC_STORE_WEP_FLOAT, 0, 0, .def = 0.0f, .min = 0.1f, .max = 1.0f, .step = 0.01f, .cat = -1, .offset = offsetof(Weapon, jump_bob_y_mlt), .bit = 0, .unit = U_SAME},
    {"physRang", "Impulse Range", NULL, NULL, GC_NUM, GC_STORE_WEP_FLOAT, 0, 0, .def = 0.0f, .min = 0.0f, .max = 2000.0f, .step = 1.0f, .cat = -1, .offset = offsetof(Weapon, physical_range), .bit = 0, .unit = U_SAME},
    {"physPow", "Impulse Power", NULL, NULL, GC_NUM, GC_STORE_WEP_FLOAT, 0, 0, .def = 0.0f, .min = -2.0f, .max = 2.0f, .step = 0.001f, .cat = -1, .offset = offsetof(Weapon, physical_power), .bit = 0, .unit = U_SAME},
    {"secondary", "Is Secondary", NULL, NULL, GC_BOOL, GC_STORE_WEP_BIT, 0, 0, .def = 0.0f, .cat = -1, .offset = 0, .bit = WB_SECONDARY, .unit = U_SAME},
    {"equipment", "Is Equipment", NULL, NULL, GC_BOOL, GC_STORE_WEP_BIT, 0, 0, .def = 0.0f, .cat = -1, .offset = 0, .bit = WB_EQUIPMENT, .unit = U_SAME},
    {"akimbo", "Akimbo", NULL, NULL, GC_BOOL, GC_STORE_WEP_BIT, 0, 0, .def = 0.0f, .cat = -1, .offset = 0, .bit = WB_AKIMBO, .unit = U_SAME},
    {"nAuto", "Single Fire", NULL, NULL, GC_BOOL, GC_STORE_WEP_BIT, 0, 0, .def = 0.0f, .cat = -1, .offset = 0, .bit = WB_NO_AUTO, .unit = U_SAME},
    {"noSpread", "No Spread", NULL, NULL, GC_BOOL, GC_STORE_WEP_BIT, 0, 0, .def = 0.0f, .cat = -1, .offset = 0, .bit = WB_NO_SPREAD, .unit = U_SAME},
    {"noAim", "Disable ADS", NULL, NULL, GC_BOOL, GC_STORE_WEP_BIT, 0, 0, .def = 0.0f, .cat = -1, .offset = 0, .bit = WB_NO_AIM, .unit = U_SAME},
    {"assetIcon", "Custom Icon", NULL, NULL, GC_INPUT, GC_STORE_NONE, 0, 0, .def = 0.0f, .def_str = "", .len = 12, .cat = -1, .offset = 0, .bit = 0, .unit = U_SAME},
    {"refDist", "Reference Distance (Sound)", NULL, NULL, GC_NUM, GC_STORE_NONE, 0, 0, .def = 0.0f, .min = 0.0f, .max = 5000.0f, .step = 0.1f, .cat = -1, .offset = 0, .bit = 0, .unit = U_SAME},
    {"rollOff", "RollOff Factor (Sound)", NULL, NULL, GC_NUM, GC_STORE_NONE, 0, 0, .def = 0.0f, .min = 0.0f, .max = 1.0f, .step = 0.01f, .cat = -1, .offset = 0, .bit = 0, .unit = U_SAME},
    {"pj_disable", "Disable Projectile", NULL, NULL, GC_BOOL, GC_STORE_WEP_BIT, 0, 0, .def = 0.0f, .cat = -1, .offset = 0, .bit = WB_PROJECTILE_DISABLE, .unit = U_SAME},
    {"pj_asset", "Projectile Asset", NULL, NULL, GC_INPUT, GC_STORE_NONE, 0, 0, .def = 0.0f, .def_str = "", .len = 12, .cat = -1, .offset = 0, .bit = 0, .unit = U_SAME},
    {"pj_dmg", "Projectile Damage", NULL, NULL, GC_NUM, GC_STORE_WEP_PJ, 0, 0, .def = 0.0f, .min = -200.0f, .max = 200.0f, .step = 1.0f, .cat = -1, .offset = offsetof(ProjectileConfig, damage), .bit = 0, .unit = U_SAME},
    {"pj_spd", "Projectile Speed", NULL, NULL, GC_NUM, GC_STORE_WEP_PJ, 0, 0, .def = 0.0f, .min = 0.01f, .max = 5.0f, .step = 0.01f, .cat = -1, .offset = offsetof(ProjectileConfig, speed), .bit = 0, .unit = U_SAME},
    {"pj_scale", "Projectile Scale", NULL, NULL, GC_NUM, GC_STORE_NONE, 0, 0, .def = 0.0f, .min = 0.01f, .max = 10.0f, .step = 0.01f, .cat = -1, .offset = 0, .bit = 0, .unit = U_SAME},
    {"pj_length", "Projectile Length", NULL, NULL, GC_NUM, GC_STORE_NONE, 0, 0, .def = 0.0f, .min = 1.0f, .max = 50.0f, .step = 1.0f, .cat = -1, .offset = 0, .bit = 0, .unit = U_SAME},
    {"pj_range", "Projectile Range", NULL, NULL, GC_NUM, GC_STORE_WEP_PJ, 0, 0, .def = 0.0f, .min = 10.0f, .max = 10000.0f, .step = 10.0f, .cat = -1, .offset = offsetof(ProjectileConfig, range), .bit = 0, .unit = U_SAME},
    {"pj_grav", "Projectile Gravity", NULL, NULL, GC_NUM, GC_STORE_WEP_PJ, 0, 0, .def = 0.0f, .min = -0.01f, .max = 0.01f, .step = 0.00001f, .cat = -1, .offset = offsetof(ProjectileConfig, gravity), .bit = 0, .unit = U_SAME},
};

// config.effectConfig (5 entries).
static const GameConfigOption k_effect_options[] = {
    {"name", "Name", NULL, NULL, GC_INPUT, GC_STORE_EFF_NAME, 0, 0, .def = 0.0f, .def_str = "", .len = 24, .cat = -1, .offset = 0, .bit = 0, .unit = U_SAME},
    {"max", "Max Limit", NULL, NULL, GC_NUM, GC_STORE_EFF_INT, 0, 0, .def = 0.0f, .min = 1.0f, .max = 10.0f, .step = 1.0f, .cat = 0, .offset = offsetof(EffectConfig, max), .bit = 0, .unit = U_SAME},
    {"timer", "Timer", NULL, NULL, GC_NUM, GC_STORE_EFF_FLOAT, 0, 0, .def = 0.0f, .min = 0.0f, .max = 120.0f, .step = 1.0f, .cat = 0, .offset = offsetof(EffectConfig, timer), .bit = 0, .unit = U_SAME},
    {"mlt", "Effect Mlt", NULL, NULL, GC_NUM, GC_STORE_EFF_FLOAT, 0, 0, .def = 0.0f, .min = 0.0f, .max = 10.0f, .step = 0.01f, .cat = 0, .offset = offsetof(EffectConfig, mlt), .bit = 0, .unit = U_SAME},
    {"hp", "Health", NULL, NULL, GC_NUM, GC_STORE_EFF_FLOAT, 0, 0, .def = 0.0f, .min = 0.0f, .max = 100.0f, .step = 1.0f, .cat = 0, .offset = offsetof(EffectConfig, hp), .bit = 0, .unit = U_SAME},
};

// applyConfig applies a weps / prks key only when the base entry defines it (`this.weapons[i][varN]
// != null`): bit b = k_weapon_options[b] present in src/data/weapons.js entry i (generated from v7).
static const unsigned long long k_weapon_keys[] = {
    0x0000fffff7bfffffull, // 0 Sniper Rifle
    0x0000ffffffffffffull, // 1 Assault Rifle
    0x0000ffffefbeffffull, // 2 Pistol
    0x0000ffffefffffffull, // 3 Submachine Gun
    0x0000ffffefbfffffull, // 4 Revolver
    0x000000fffffeffffull, // 5 Shotgun
    0x0000ffffffffffffull, // 6 Machine Gun
    0x0000ffffffbfffffull, // 7 Semi Auto
    0x0000fefffdf8ffffull, // 8 Rocket Launcher
    0x000000ffe7ffffffull, // 9 Akimbo Uzi
    0x0000ffffefbfffffull, // 10 Desert Eagle
    0x000000ffe7bfffffull, // 11 Alien Blaster
    0x000000e001befdefull, // 12 Combat Knife
    0x00007effe5bfffffull, // 13 Crossbow
    0x0000ffffffffffffull, // 14 Famas
    0x000000fffffeffffull, // 15 Sawed Off
    0x000000ffefbfffffull, // 16 Auto Pistol
    0x000000f100b8fdc9ull, // 17 Bomb
    0x0000feffffffffffull, // 18 Blaster
    0x000000f100bafde9ull, // 19 Build Tool
    0x000000ffefbfffffull, // 20 Grappler
    0x000000ffefbfffffull, // 21 Tehchy-9
    0x0000fefbfffeffffull, // 22 Noob Tube
    0x000000ffffffffffull, // 23 Slimer
    0x0000feffefbeffffull, // 24 Zapper
    0x0000ffff7fffffffull, // 25 Minigun
    0x0000fefffffeffffull, // 26 War Machine
    0x000000ffe7ffffffull, // 27 Akimbo Pistol
    0x0000ffffffffffffull, // 28 Charge Rifle
};
static const unsigned char k_effect_keys[] = {
    0xf, // 0 Boosted
    0xf, // 1 Sleight of Hand
    0xf, // 2 Rapid Fire
    0x1f, // 3 Siphon
    0xf, // 4 Rooted
    0xf, // 5 Dizzy
    0x1f, // 6 Poisoned
    0xf, // 7 Jump Boost
    0x1f, // 8 Zapped
    0xf, // 9 Strength
};

#define GC_COUNT(a) (sizeof(a) / sizeof((a)[0]))

// The serverConfig / classConfig rows.
static const GameConfigOption *gc_server_options(size_t *count) {
    *count = GC_COUNT(k_server_options);
    return k_server_options;
}

static const GameConfigOption *gc_class_options(size_t *count) {
    *count = GC_COUNT(k_class_options);
    return k_class_options;
}

static const struct {
    const GameConfigOption *table;
    size_t count;
} k_tables[GAME_CONFIG_TABLE_COUNT] = {
    [GAME_CONFIG_SERVER] = {k_server_options, sizeof(k_server_options) / sizeof(k_server_options[0])},
    [GAME_CONFIG_CLASS] = {k_class_options, sizeof(k_class_options) / sizeof(k_class_options[0])},
    [GAME_CONFIG_WEAPON] = {k_weapon_options, sizeof(k_weapon_options) / sizeof(k_weapon_options[0])},
    [GAME_CONFIG_EFFECT] = {k_effect_options, sizeof(k_effect_options) / sizeof(k_effect_options[0])},
};

const GameConfigOption *game_config_options(const GameConfigTable table, size_t *count) {
    if ((int) table < 0 || table >= GAME_CONFIG_TABLE_COUNT) {
        if (count) *count = 0;
        return NULL;
    }
    if (count) *count = k_tables[table].count;
    return k_tables[table].table;
}

const GameConfigOption *game_config_find_option(const GameConfigTable table, const char *var) {
    size_t count = 0;
    const GameConfigOption *options = game_config_options(table, &count);
    for (size_t i = 0; var && i < count; i++) {
        if (!strcmp(options[i].var, var)) return &options[i];
    }
    return NULL;
}

// ---------------------------------------------------------------- host types (config.hostTypes)

typedef struct {
    const char *title;
    unsigned int flags;      // GAME_HOST_FLAG_* (the Host Game window's comp / restrict / showAll / hideHead / noCustM / buyServ)
    unsigned char comp;      // comp: defC defaults, comp sizes, isComp
    const char *over_dat;    // overDat (JSON)
    const char *force_dat;   // forceDat (JSON)
    const char *allowed_dat; // allowedDat (JSON)
} GameHostType;

// "maps" are Krunker mapIndexes (map_catalog_official_json). Comp: deob-10.0.3.js:19442-19455 (overDat; its maps
// gained 36 Lush, 39 Eterno Simulator and 42 Frontier in v10. A request's maps replace them: game_config_setup).
static const GameHostType k_host_types[GAME_HOST_TYPE_COUNT] = {
    [GAME_HOST_CUSTOM] = {"Custom Game Setup", 0, 0, NULL, NULL, NULL},
    [GAME_HOST_COMP] = {"Comp Server Setup", GAME_HOST_FLAG_COMP | GAME_HOST_FLAG_SHOW_ALL | GAME_HOST_FLAG_NO_CUSTOM_MAP, 1,
        "{\"private\":true,\"allowSpect\":true,\"maps\":[0,2,4,5,11,12,14,17,23,36,39,42],\"modes\":[2],\"maxPlayers\":10,"
        "\"minPlayers\":8,\"fixMov\":true,\"jumpMlt\":1.005,\"slidSpd\":0.97,\"warmupTime\":0.2,\"selTeam\":true,\"throwMel\":false}",
        NULL, NULL},
    [GAME_HOST_RENT] = {"Rent Dedicated Server", GAME_HOST_FLAG_BUY_SERVER | GAME_HOST_FLAG_NO_CUSTOM_MAP, 0, NULL, NULL, NULL},
    [GAME_HOST_ZOMBIES] = {"Zombies Server Setup", GAME_HOST_FLAG_RESTRICT | GAME_HOST_FLAG_SHOW_ALL | GAME_HOST_FLAG_HIDE_HEAD | GAME_HOST_FLAG_NO_CUSTOM_MAP, 0, NULL,
        "{\"private\":true,\"maxPlayers\":4,\"maps\":[27],\"modes\":[35],\"mimicPubs\":true}", "{\"maps\":[27]}"},
    [GAME_HOST_RAID] = {"Raid Server Setup", GAME_HOST_FLAG_RESTRICT | GAME_HOST_FLAG_SHOW_ALL | GAME_HOST_FLAG_HIDE_HEAD | GAME_HOST_FLAG_NO_CUSTOM_MAP, 0, NULL,
        // v10.0.3 Raid Server Setup (deob-10.0.3.js:19486-19511): allowedDat maps [18, 29, 31, 35, 43] (Tortuga,
        // Laboratory, Soul Sanctum, Khepri, Bastion)
        "{\"private\":true,\"maxPlayers\":8,\"maps\":[18],\"modes\":[23],\"mimicPubs\":true}", "{\"maps\":[18,29,31,35,43]}"},
};

const char *game_host_type_name(const int host_index) {
    return host_index >= 0 && host_index < GAME_HOST_TYPE_COUNT ? k_host_types[host_index].title : NULL;
}

// A real mode id (mode_config() also returns the empty slots of the v7 table).
static const GameModeConfig *gc_mode(const int id) {
    const GameModeConfig *mode = mode_config(id);
    return mode && mode->name ? mode : NULL;
}

unsigned int game_host_type_flags(const int host_index) {
    return host_index >= 0 && host_index < GAME_HOST_TYPE_COUNT ? k_host_types[host_index].flags : 0;
}

static int gc_host_comp(const int host_index) {
    return host_index >= 0 && host_index < GAME_HOST_TYPE_COUNT && k_host_types[host_index].comp;
}

// ---------------------------------------------------------------- JSON helpers (UTILS)

// cJSON_GetObjectItemCaseSensitive that treats JSON null as absent (v7 `cfg[k] == null`).
static const cJSON *gc_item(const cJSON *obj, const char *key) {
    const cJSON *item = cJSON_IsObject(obj) && key ? cJSON_GetObjectItemCaseSensitive(obj, key) : NULL;
    return item && !cJSON_IsNull(item) ? item : NULL;
}

static int gc_is_number(const cJSON *item) { // UTILS.isNumber
    return cJSON_IsNumber(item) && isfinite(item->valuedouble);
}

static int gc_truthy(const cJSON *item) { // JS !!value
    if (!item || cJSON_IsNull(item) || cJSON_IsFalse(item)) return 0;
    if (cJSON_IsNumber(item)) return item->valuedouble != 0.0 && !isnan(item->valuedouble);
    if (cJSON_IsString(item)) return item->valuestring && item->valuestring[0];
    return 1; // true, objects, arrays
}

// options[value] != null ? parseInt(value) : -1 (a number index, or its canonical string).
static int gc_select(const cJSON *item, const int count) {
    double v = -1.0;
    if (cJSON_IsNumber(item)) {
        v = item->valuedouble;
    } else if (cJSON_IsString(item) && item->valuestring && item->valuestring[0]) {
        const char *s = item->valuestring;
        if (s[0] == '0' && s[1]) return -1; // "03" is not an options key
        for (const char *c = s; *c; c++) if (*c < '0' || *c > '9') return -1;
        v = atof(s);
    }
    return v >= 0.0 && v < count && floor(v) == v ? (int) v : -1;
}

// The characters v7's /[^\w \-\|]+/ allows.
static int gc_name_char_ok(const unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == ' ' || c == '-' || c == '|';
}

static int gc_name_ok(const char *s, const int len) {
    if (!s || !s[0] || (int) strlen(s) > len) return 0;
    for (const unsigned char *c = (const unsigned char *) s; *c; c++) if (!gc_name_char_ok(*c)) return 0;
    return 1;
}

// UTILS.sanitizeStr: drop < > & and every non-ASCII byte.
static void gc_sanitize(char *out, const size_t size, const char *in) {
    size_t n = 0;
    for (const unsigned char *c = (const unsigned char *) in; c && *c && n + 1 < size; c++) {
        if (*c == '<' || *c == '>' || *c == '&' || *c >= 0x80) continue;
        out[n++] = (char) *c;
    }
    if (size) out[n] = 0;
}

static float gc_limit(const double v, const float min, const float max) { // UTILS.limitMM
    return (float) (v < min ? min : v > max ? max : v);
}

static double gc_to_kn(const double v, const int unit) {
    return unit == U_MS ? v / 1000.0 : unit == U_X1000 ? v * 1000.0 : v;
}

static double gc_to_v7(const double v, const int unit) {
    return unit == U_MS ? v * 1000.0 : unit == U_X1000 ? v / 1000.0 : v;
}

// ---------------------------------------------------------------- serverConfig <-> GameConfig

static int gc_bit_get(const GameConfig *g, const int bit) {
    switch (bit) {
        case GC_BIT_CAN_SLIDE: return g->can_slide;
        case GC_BIT_AIR_STRAFE: return g->air_strafe;
        case GC_BIT_AUTO_JUMP: return g->auto_jump;
        case GC_BIT_BULLET_DROP: return g->bullet_drop;
        case GC_BIT_HEALTH_REGEN: return g->health_regen;
        case GC_BIT_KILL_REWARDS: return g->kill_rewards;
        case GC_BIT_HEADSHOTS_ONLY: return g->headshots_only;
        case GC_BIT_NO_SECONDARIES: return g->no_secondaries;
        case GC_BIT_NO_STREAKS: return g->no_streaks;
        case GC_BIT_DISABLE_BORDERS: return g->disable_borders;
        case GC_BIT_THROWABLE_MELEES: return g->throwable_melees;
        case GC_BIT_NO_CHARGE_WEAPONS: return !g->no_charge_weapons; // chrgWeps
        case GC_BIT_SELECT_TEAM: return g->select_team;
        case GC_BIT_ALLOW_SPECTATE: return g->allow_spectate;
        case GC_BIT_THIRD_PERSON: return g->third_person;
        case GC_BIT_HIDE_NAMETAGS: return g->hide_nametags;
        case GC_BIT_RAMP_FIX: return g->ramp_fix;
        default: return 0;
    }
}

static void gc_bit_set(GameConfig *g, const int bit, const int on) {
    const unsigned int v = on ? 1u : 0u;
    switch (bit) {
        case GC_BIT_CAN_SLIDE: g->can_slide = v; break;
        case GC_BIT_AIR_STRAFE: g->air_strafe = v; break;
        case GC_BIT_AUTO_JUMP: g->auto_jump = v; break;
        case GC_BIT_BULLET_DROP: g->bullet_drop = v; break;
        case GC_BIT_HEALTH_REGEN: g->health_regen = v; break;
        case GC_BIT_KILL_REWARDS: g->kill_rewards = v; break;
        case GC_BIT_HEADSHOTS_ONLY: g->headshots_only = v; break;
        case GC_BIT_NO_SECONDARIES: g->no_secondaries = v; break;
        case GC_BIT_NO_STREAKS: g->no_streaks = v; break;
        case GC_BIT_DISABLE_BORDERS: g->disable_borders = v; break;
        case GC_BIT_THROWABLE_MELEES: g->throwable_melees = v; break;
        case GC_BIT_NO_CHARGE_WEAPONS: g->no_charge_weapons = !v; break; // chrgWeps
        case GC_BIT_SELECT_TEAM: g->select_team = v; break;
        case GC_BIT_ALLOW_SPECTATE: g->allow_spectate = v; break;
        case GC_BIT_THIRD_PERSON: g->third_person = v; break;
        case GC_BIT_HIDE_NAMETAGS: g->hide_nametags = v; break;
        case GC_BIT_RAMP_FIX: g->ramp_fix = v; break;
        default: break;
    }
}

static void gc_store_number(GameConfig *g, const GameConfigOption *o, const double v) {
    char *field = (char *) g + o->offset;
    switch (o->store) {
        case GC_STORE_INT: *(int *) field = (int) v; break;
        case GC_STORE_FLOAT: *(float *) field = (float) v; break;
        case GC_STORE_U8: *(unsigned char *) field = v != 0.0; break;
        case GC_STORE_BIT: gc_bit_set(g, o->bit, v != 0.0); break;
        default: break;
    }
}

static double gc_load_number(const GameConfig *g, const GameConfigOption *o) {
    const char *field = (const char *) g + o->offset;
    switch (o->store) {
        case GC_STORE_INT: return *(const int *) field;
        case GC_STORE_FLOAT: return *(const float *) field;
        case GC_STORE_U8: return *(const unsigned char *) field;
        case GC_STORE_BIT: return gc_bit_get(g, o->bit);
        default: return 0.0;
    }
}

static void gc_store_string(GameConfig *g, const GameConfigOption *o, const char *s) {
    if (o->store != GC_STORE_STR) return;
    char *field = (char *) g + o->offset;
    snprintf(field, 17, "%s", s ? s : "");
}

static float gc_max_for(const GameConfigOption *o, const int clamp) {
    float max = o->max;
    if (clamp == GAME_CONFIG_CLAMP_DEDICATED && o->max_d) max = o->max_d;
    else if (clamp == GAME_CONFIG_CLAMP_HOSTED && o->max_f) max = o->max_f;
    else if (clamp == GAME_CONFIG_CLAMP_PRIVATE && o->max_p) max = o->max_p;
    else if (clamp == GAME_CONFIG_CLAMP_GUEST && o->max_g) max = o->max_g;
    return max;
}

static float gc_default(const GameConfigOption *o, const int comp) {
    // Krunker Strike's serverConfig defaults (flavor_rules.h; Strike has no comp defC); base: none
    float flavor_def;
    if (flavor_config_default(o->var, &flavor_def)) return flavor_def;
    return comp && (o->flags & GC_HAS_DEF_C) ? o->def_c : o->def;
}

// One serverConfig option from `item` (NULL: the default), as applyConfig does.
static void gc_read_option(GameConfig *g, const GameConfigOption *o, const cJSON *item, const int clamp, const int comp) {
    switch (o->kind) {
        case GC_BOOL:
            gc_store_number(g, o, item ? gc_truthy(item) : gc_default(o, comp));
            break;
        case GC_INPUT: {
            // v7 bug fixed: the character / length checks run on the stored value
            char clean[64] = "";
            if (cJSON_IsString(item) && item->valuestring && item->valuestring[0]) gc_sanitize(clean, sizeof(clean), item->valuestring);
            if (!clean[0] || !gc_name_ok(clean, (int) sizeof(clean))) snprintf(clean, sizeof(clean), "%s", o->def_str ? o->def_str : "");
            clean[16] = 0; // ≤ 16 characters
            gc_store_string(g, o, clean);
            break;
        }
        case GC_SELECT: {
            const int index = item ? gc_select(item, o->option_count) : -1;
            gc_store_number(g, o, index >= 0 ? index : gc_default(o, comp));
            break;
        }
        default: {
            const double v = gc_is_number(item) ? item->valuedouble : gc_default(o, comp);
            gc_store_number(g, o, gc_limit(v, o->min, gc_max_for(o, clamp)));
            break;
        }
    }
}

void game_config_sync_teams(GameConfig *g) {
    if (!g) return;
    g->team1_name = g->team_names[1];
    g->team2_name = g->team_names[2];
    g->team1_damage = g->team_damage[1];
    g->team2_damage = g->team_damage[2];
}

void game_config_defaults(GameConfig *g, const int host_index) {
    if (!g) return;
    const int comp = gc_host_comp(host_index);
    size_t option_count;
    const GameConfigOption *options = gc_server_options(&option_count);
    for (size_t i = 0; i < option_count; i++) {
        gc_read_option(g, &options[i], NULL, GAME_CONFIG_CLAMP_DEDICATED, comp);
    }
    g->host_type = host_index >= 0 && host_index < GAME_HOST_TYPE_COUNT ? host_index : GAME_HOST_CUSTOM;
    g->comp = (unsigned char) comp;
    g->mimic_pubs = 0;
    g->is_private = 0;
    g->class_list_count = 0;
    // hitBoxPad: the comp default is a value; otherwise a lobby made comp later (cfg.isComp) keeps
    // the defC 0.1 (shared/src/effects.c game_hitbox_pad) and everything else gets def 0.6.
    g->hitbox_pad_set = (unsigned int) comp;
    game_config_sync_teams(g);
}

void game_config_read(GameConfig *g, const cJSON *cfg, const int clamp, const int host_index) {
    if (!g) return;
    const int comp = gc_host_comp(host_index);
    game_config_defaults(g, host_index);
    size_t option_count;
    const GameConfigOption *options = gc_server_options(&option_count);
    for (size_t i = 0; i < option_count; i++) {
        const GameConfigOption *o = &options[i];
        const cJSON *item = gc_item(cfg, o->var);
        gc_read_option(g, o, item, clamp, comp);
        if (item && !strcmp(o->var, "hitBoxPad")) g->hitbox_pad_set = 1;
    }
    g->comp = (unsigned char) (comp || gc_truthy(gc_item(cfg, "isComp"))); // game.isComp = !!cfg.isComp
    g->mimic_pubs = (unsigned char) gc_truthy(gc_item(cfg, "mimicPubs"));
    g->is_private = (unsigned char) gc_truthy(gc_item(cfg, "private"));
    // lockedTeams = !!accountTeams || !!cfg.lockedTeams (deob-10.0.3.js:216148); the paid upgrades !!cfg[varN] (:216372)
    g->locked_teams = (unsigned char) ((g->comp && game_config_roster_given(cfg)) || gc_truthy(gc_item(cfg, "lockedTeams")));
    g->max_ps = (unsigned char) gc_truthy(gc_item(cfg, "maxPS"));
    g->prom_serv = (unsigned char) gc_truthy(gc_item(cfg, "promServ"));
    game_config_sync_teams(g);
}

// applyConfig's account list filter (deob-10.0.3.js:216130-216138): the strings of 1..64 characters, the first 50
static int gc_roster_list(const cJSON *list, const char *name) {
    int kept = 0, found = 0;
    const cJSON *e;
    if (!cJSON_IsArray(list)) return 0;
    cJSON_ArrayForEach(e, list) {
        if (kept >= GAME_CONFIG_ROSTER_MAX) break;
        if (!cJSON_IsString(e) || !e->valuestring[0] || strlen(e->valuestring) > GAME_CONFIG_ROSTER_NAME_MAX) continue;
        kept++;
        if (name && !strcmp(e->valuestring, name)) found = 1;
    }
    return name ? found : kept;
}

int game_config_roster_given(const cJSON *cfg) {
    const cJSON *teams = gc_item(cfg, "teams");
    return cJSON_IsArray(teams) && cJSON_GetArraySize(teams) == 2 &&
           (gc_roster_list(cJSON_GetArrayItem(teams, 0), NULL) > 0 || gc_roster_list(cJSON_GetArrayItem(teams, 1), NULL) > 0);
}

int game_config_merge(GameConfig *g, const cJSON *cfg, const int clamp) {
    if (!g || !cJSON_IsObject(cfg)) return 0;
    int applied = 0;
    size_t option_count;
    const GameConfigOption *options = gc_server_options(&option_count);
    for (size_t i = 0; i < option_count; i++) {
        const GameConfigOption *o = &options[i];
        const cJSON *item = gc_item(cfg, o->var);
        if (!item) continue;
        if (o->kind == GC_NUM && !gc_is_number(item)) continue; // a key of the wrong type keeps the value
        if (o->kind == GC_SELECT && gc_select(item, o->option_count) < 0) continue;
        gc_read_option(g, o, item, clamp, g->comp);
        if (!strcmp(o->var, "hitBoxPad")) g->hitbox_pad_set = 1;
        applied++;
    }
    game_config_sync_teams(g);
    return applied;
}

cJSON *game_config_write(const GameConfig *g) {
    cJSON *out = cJSON_CreateObject();
    if (!out || !g) return out;
    size_t option_count;
    const GameConfigOption *options = gc_server_options(&option_count);
    for (size_t i = 0; i < option_count; i++) {
        const GameConfigOption *o = &options[i];
        if (!strcmp(o->var, "hitBoxPad") && !g->hitbox_pad_set) continue; // the default (comp: defC)
        // an input KN keeps nowhere (webhook): v10 deletes it from the config (deob-10.0.3.js:216128), and it has no field
        if (o->kind == GC_INPUT && o->store == GC_STORE_NONE) continue;
        if (o->kind == GC_INPUT) {
            cJSON_AddStringToObject(out, o->var, (const char *) g + o->offset);
        } else if (o->kind == GC_BOOL) {
            cJSON_AddBoolToObject(out, o->var, gc_load_number(g, o) != 0.0);
        } else {
            cJSON_AddNumberToObject(out, o->var, gc_load_number(g, o));
        }
    }
    if (g->comp) cJSON_AddBoolToObject(out, "isComp", 1);
    if (g->mimic_pubs) cJSON_AddBoolToObject(out, "mimicPubs", 1);
    if (g->is_private) cJSON_AddBoolToObject(out, "private", 1);
    if (g->locked_teams) cJSON_AddBoolToObject(out, "lockedTeams", 1); // the client's comp menu (config.lockedTeams)
    cJSON_AddBoolToObject(out, "maxPS", g->max_ps);       // v10's config always has the serverUpg keys (:216372-216376)
    cJSON_AddBoolToObject(out, "promServ", g->prom_serv);
    return out;
}

// ---------------------------------------------------------------- validateConfigs

static const char *gc_validate_overrides(const cJSON *obj, const GameConfigTable table, const char *what) {
    static char err[160];
    if (!obj || cJSON_IsNull(obj)) return NULL;
    char *text = cJSON_PrintUnformatted(obj); // v7 bug fixed: v7 measures `obj + ""`
    const size_t length = text ? strlen(text) : 0;
    cJSON_free(text);
    if (length >= GAME_CONFIG_DATA_MAX) {
        snprintf(err, sizeof(err), "%s - Too much data.", what);
        return err;
    }
    const cJSON *entry;
    cJSON_ArrayForEach(entry, obj) {
        const cJSON *value;
        cJSON_ArrayForEach(value, entry) {
            const GameConfigOption *o = game_config_find_option(table, value->string);
            const char *id = entry->string ? entry->string : "?";
            if (!o) {
                snprintf(err, sizeof(err), "Invalid Setting in %s. (ID: %s)", what, id);
                return err;
            }
            if (o->kind == GC_INPUT) {
                if (!cJSON_IsString(value) || !gc_name_ok(value->valuestring, o->len ? o->len : 16)) {
                    // an empty string is "not a string" for UTILS.isString
                    snprintf(err, sizeof(err), "Invalid %s Value. (ID: %s, Set: %s)", what, id, o->name);
                    return err;
                }
            } else if (o->kind == GC_SELECT) {
                if (gc_select(value, o->option_count) < 0) {
                    snprintf(err, sizeof(err), "Config Value out of Range. (ID: %s, Set: %s)", id, o->name);
                    return err;
                }
            } else if (o->kind == GC_NUM) {
                if (!gc_is_number(value)) {
                    snprintf(err, sizeof(err), "Invalid %s Value. (ID: %s, Set: %s)", what, id, o->name);
                    return err;
                }
                const float max = o->max_d ? o->max_d : o->max_f ? o->max_f : o->max;
                if (value->valuedouble > max || value->valuedouble < o->min) {
                    snprintf(err, sizeof(err), "%s Value out of Range. (ID: %s, Set: %s)", what, id, o->name);
                    return err;
                }
            }
        }
    }
    return NULL;
}

const char *game_config_validate(const cJSON *config) {
    static char err[160];
    if (!config) return NULL;
    if (!cJSON_IsObject(config)) return "Invalid Config.";
    const cJSON *modes = gc_item(config, "modes");
    const cJSON *entry;
    cJSON_ArrayForEach(entry, modes) {
        if (!cJSON_IsNumber(entry) || !gc_mode((int) entry->valuedouble) || floor(entry->valuedouble) != entry->valuedouble) return "Invalid Mode in Config.";
    }
    const int class_count = (int) (rules_class_count());
    const cJSON *classes = gc_item(config, "classes");
    cJSON_ArrayForEach(entry, classes) {
        if (!cJSON_IsNumber(entry) || entry->valuedouble < 0 || entry->valuedouble >= class_count || floor(entry->valuedouble) != entry->valuedouble) {
            return "Invalid Class in Config.";
        }
    }
    const cJSON *settings = gc_item(config, "settings");
    int count = 0;
    cJSON_ArrayForEach(entry, settings) {
        const GameConfigOption *o = game_config_find_option(GAME_CONFIG_SERVER, entry->string);
        if (!o) return "Invalid Setting in Config.";
        if (o->kind != GC_INPUT && o->kind != GC_BOOL) {
            if (!gc_is_number(entry)) {
                snprintf(err, sizeof(err), "Invalid Config Value. (Set: %s)", o->name);
                return err;
            }
            const float max = o->max_d ? o->max_d : o->max_f ? o->max_f : o->max;
            if (o->kind == GC_SELECT ? gc_select(entry, o->option_count) < 0 : (entry->valuedouble > max || entry->valuedouble < o->min)) {
                snprintf(err, sizeof(err), "Config Value out of Range. (Set: %s)", o->name);
                return err;
            }
        }
        size_t option_count;
        gc_server_options(&option_count);
        if (++count > (int) option_count + 1) return "Settings Mismatch.";
    }
    const char *e = gc_validate_overrides(gc_item(config, "weps"), GAME_CONFIG_WEAPON, "weaponConfig");
    if (!e) e = gc_validate_overrides(gc_item(config, "cls"), GAME_CONFIG_CLASS, "classConfig");
    if (!e) e = gc_validate_overrides(gc_item(config, "prks"), GAME_CONFIG_EFFECT, "effectConfig");
    return e;
}

// ---------------------------------------------------------------- map config / setupGameConfig

cJSON *game_config_from_map(const cJSON *map_config) {
    cJSON *out = cJSON_CreateObject();
    if (!out || !cJSON_IsObject(map_config)) return out;
    const cJSON *entry;
    cJSON_ArrayForEach(entry, gc_item(map_config, "settings")) {
        // a published map cannot make its game a pub game (mimicPubs comes from the host's request)
        if (entry->string && strcmp(entry->string, "mimicPubs")) cJSON_AddItemToObject(out, entry->string, cJSON_Duplicate(entry, 1));
    }
    static const char *const keys[] = {"modes", "classes", "weps", "cls", "prks"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        const cJSON *item = gc_item(map_config, keys[i]);
        if (item) cJSON_AddItemToObject(out, keys[i], cJSON_Duplicate(item, 1));
    }
    return out;
}

static void gc_set(cJSON *obj, const char *key, cJSON *value) { // obj[key] = value (NULL: delete)
    if (!obj || !key) {
        cJSON_Delete(value);
        return;
    }
    cJSON_DeleteItemFromObjectCaseSensitive(obj, key);
    if (value) cJSON_AddItemToObject(obj, key, value);
}

static cJSON *gc_def_json(const GameConfigOption *o) {
    if (o->kind == GC_INPUT) return cJSON_CreateString(o->def_str ? o->def_str : "");
    if (o->kind == GC_BOOL) return cJSON_CreateBool(o->def != 0.0f);
    return cJSON_CreateNumber(o->def);
}

cJSON *game_config_setup(const int host_index, const cJSON *requested, const cJSON *extra, const int allow_prevented) {
    const int host = host_index >= 0 && host_index < GAME_HOST_TYPE_COUNT ? host_index : GAME_HOST_CUSTOM;
    const GameHostType *ht = &k_host_types[host];
    cJSON *req = cJSON_IsObject(requested) ? cJSON_Duplicate(requested, 1) : cJSON_CreateObject();
    cJSON *saved = cJSON_CreateObject(); // vO427[hostIndex]: options this host type keeps; JSON null = was absent
    if (!req || !saved) {
        cJSON_Delete(req);
        cJSON_Delete(saved);
        return NULL;
    }

    size_t option_count;
    const GameConfigOption *options = gc_server_options(&option_count);
    for (size_t i = 0; i < option_count; i++) {
        const GameConfigOption *o = &options[i];
        if (!o->host_types || (o->host_types & GC_HT(host))) {
            if ((o->host_types ? o->host_types : GC_HT(0)) & GC_HT(host)) {
                const cJSON *item = gc_item(req, o->var);
                cJSON_AddItemToObject(saved, o->var, item ? cJSON_Duplicate(item, 1) : cJSON_CreateNull());
            }
        } else {
            gc_set(req, o->var, gc_def_json(o)); // not an option of this host type: its default
        }
    }

    cJSON *maps = cJSON_Duplicate(gc_item(req, "maps"), 1);
    cJSON *cfg = req;
    if (ht->over_dat) { // v7 bug fixed: v7 then writes into config.js' overDat itself
        cfg = cJSON_Parse(ht->over_dat);
        cJSON_Delete(req);
        if (!cfg) cfg = cJSON_CreateObject();
    }
    const cJSON *entry;
    cJSON_ArrayForEach(entry, saved) gc_set(cfg, entry->string, cJSON_IsNull(entry) ? NULL : cJSON_Duplicate(entry, 1));
    cJSON_Delete(saved);
    cJSON_ArrayForEach(entry, (cJSON_IsObject(extra) ? extra : NULL)) gc_set(cfg, entry->string, cJSON_Duplicate(entry, 1));

    if (ht->comp) {
        const cJSON *tm = gc_item(cfg, "tmSize"), *spec = gc_item(cfg, "specSlots");
        const double min_players = ((gc_is_number(tm) ? tm->valuedouble : 0.0) + 1.0) * 2.0;
        gc_set(cfg, "minPlayers", cJSON_CreateNumber(min_players));
        gc_set(cfg, "maxPlayers", cJSON_CreateNumber(min_players + (gc_is_number(spec) ? spec->valuedouble : 0.0)));
        gc_set(cfg, "isComp", cJSON_CreateBool(1));
    }
    gc_set(cfg, "maps", maps);

    cJSON *modes = cJSON_CreateArray();
    cJSON_ArrayForEach(entry, gc_item(cfg, "modes")) {
        const GameModeConfig *mode = cJSON_IsNumber(entry) ? gc_mode((int) entry->valuedouble) : NULL;
        if (mode && (allow_prevented || !mode->prevent_custom)) cJSON_AddItemToArray(modes, cJSON_CreateNumber(mode->id));
    }
    gc_set(cfg, "modes", modes);
    // mimicPubs is kept as the request gave it (v10 game.mimicPubs = !!cfg.mimicPubs, v10:222266): an
    // overDat host (Comp) has no such key, so it is off; a forceDat host (Zombies, Raid) sets it below.
    // A published map's settings never set it (game_config_from_map).

    if (ht->force_dat) {
        cJSON *pre = cfg;
        cfg = cJSON_Parse(ht->force_dat);
        if (!cfg) cfg = cJSON_CreateObject();
        cJSON *allowed = ht->allowed_dat ? cJSON_Parse(ht->allowed_dat) : NULL;
        cJSON_ArrayForEach(entry, allowed) {
            if (!cJSON_IsArray(entry)) {
                gc_set(cfg, entry->string, cJSON_Duplicate(entry, 1));
                continue;
            }
            cJSON *kept = cJSON_CreateArray();
            const cJSON *want;
            cJSON_ArrayForEach(want, gc_item(pre, entry->string)) {
                const cJSON *ok;
                cJSON_ArrayForEach(ok, entry) {
                    if (cJSON_IsNumber(ok) && cJSON_IsNumber(want) && ok->valuedouble == want->valuedouble) {
                        cJSON_AddItemToArray(kept, cJSON_Duplicate(want, 1));
                        break;
                    }
                }
            }
            if (!cJSON_GetArraySize(kept)) {
                cJSON_Delete(kept);
                kept = cJSON_CreateArray();
                if (cJSON_GetArraySize(entry)) cJSON_AddItemToArray(kept, cJSON_Duplicate(cJSON_GetArrayItem(entry, 0), 1));
            }
            gc_set(cfg, entry->string, kept);
        }
        cJSON_Delete(allowed);
        cJSON_Delete(pre);
    }
    gc_set(cfg, "krRewards", NULL); // KR rewards: backend-only (decisions.md), never from a request
    return cfg;
}

// ---------------------------------------------------------------- per-game state

typedef struct GameConfigState {
    unsigned char hosted;        // game_config_host: a hosted game / the sandbox (isCustom)
    unsigned char remote_custom; // a client: the server's GAME_CONFIG said "custom"
    int host_index;
    cJSON *request;              // the host request (NULL: none)
    GameConfig base;             // game->config when hosted (restored on maps without config)
    cJSON *applied;              // {classes, weps, cls, prks} of the config in force (GAME_CONFIG)

    Weapon **base_weapons;       // game->weapons as configured (NULL until the first override)
    Weapon *weapons;             // overridden copies
    char (*weapon_names)[GAME_CONFIG_INPUT_MAX + 1];
    size_t weapon_count;
    ClassConfig *base_classes;   // game->classes as configured
    size_t class_count;
    EffectConfig effects[EFFECT_COUNT];
    char effect_names[EFFECT_COUNT][GAME_CONFIG_INPUT_MAX + 1];

    char *rx;                    // a client's GAME_CONFIG being received
    uint32_t rx_total, rx_have;
    unsigned char received;      // GAME_CONFIG applied since the last INIT
    unsigned char net_received;  // NETWORK_DATA since the last INIT
    NetNetworkDataPacket net;

    int mts_count;               // endData.mts
    int mts_mode[NET_MATCH_PICKS];
    int mts_map[NET_MATCH_PICKS];
    int *vote_uid;               // endData.vo: uid -> mts index
    int *vote_pick;
    int vote_count, vote_cap;
    unsigned char force_set;     // forceMap / forceMode
    int force_mode, force_map;

    unsigned char has_host;      // game.host is set (game_config_set_host)
    int host_uid;                // game.host: the hosting player's uid (its connection id)
} GameConfigState;

static const NetNetworkDataPacket k_default_network = {.send_hz = 10, .tick_hz = 30, .sync_hz = 1, .view_distance = 0.0f};

static GameConfigState *gc_state(Game *game) {
    if (!game) return NULL;
    if (!game->config_state) {
        GameConfigState *st = calloc(1, sizeof(GameConfigState));
        if (!st) return NULL;
        st->net = k_default_network;
        st->force_mode = st->force_map = -1;
        game->config_state = st;
    }
    return game->config_state;
}

// game->weapons / classes / effects back to what game_configure set up.
static void gc_restore_tables(Game *game) {
    GameConfigState *st = game->config_state;
    if (!st) return;
    if (st->base_weapons && game->weapons && st->weapon_count == game->weapon_count) {
        memcpy(game->weapons, st->base_weapons, st->weapon_count * sizeof(Weapon *));
    }
    if (st->base_classes && game->classes && st->class_count == game->class_count) {
        memcpy(game->classes, st->base_classes, st->class_count * sizeof(ClassConfig));
    }
    game->effects = NULL;
}

void game_config_free(Game *game) {
    if (!game || !game->config_state) return;
    GameConfigState *st = game->config_state;
    gc_restore_tables(game);
    cJSON_Delete(st->request);
    cJSON_Delete(st->applied);
    free(st->base_weapons);
    free(st->weapons);
    free(st->weapon_names);
    free(st->base_classes);
    free(st->rx);
    free(st->vote_uid);
    free(st->vote_pick);
    free(st);
    game->config_state = NULL;
}

// The base tables, saved the first time something overrides them.
static int gc_save_bases(Game *game, GameConfigState *st) {
    if (game->weapons && game->weapon_count && (!st->base_weapons || st->weapon_count != game->weapon_count)) {
        free(st->base_weapons);
        free(st->weapons);
        free(st->weapon_names);
        st->weapon_count = game->weapon_count;
        st->base_weapons = malloc(st->weapon_count * sizeof(Weapon *));
        st->weapons = calloc(st->weapon_count, sizeof(Weapon));
        st->weapon_names = calloc(st->weapon_count, sizeof(*st->weapon_names));
        if (!st->base_weapons || !st->weapons || !st->weapon_names) return 0;
        memcpy(st->base_weapons, game->weapons, st->weapon_count * sizeof(Weapon *));
    }
    if (game->classes && game->class_count && (!st->base_classes || st->class_count != game->class_count)) {
        free(st->base_classes);
        st->class_count = game->class_count;
        st->base_classes = malloc(st->class_count * sizeof(ClassConfig));
        if (!st->base_classes) return 0;
        memcpy(st->base_classes, game->classes, st->class_count * sizeof(ClassConfig));
    }
    return 1;
}

// "<n>" object keys of weps / cls / prks (v7 reads cfg.weps[i] for i < length).
static int gc_entry_id(const cJSON *entry, const size_t count) {
    const char *s = entry ? entry->string : NULL;
    if (!s || !s[0] || (s[0] == '0' && s[1])) return -1;
    long v = 0;
    for (const char *c = s; *c; c++) {
        if (*c < '0' || *c > '9' || v > 100000) return -1;
        v = v * 10 + (*c - '0');
    }
    return (size_t) v < count ? (int) v : -1;
}

// A number override in v7 units: NaN when it doesn't apply (applyConfig `continue`s).
static double gc_override_number(const GameConfigOption *o, const cJSON *item, const double base_v7) {
    if (!gc_is_number(item)) return NAN;
    const double v = item->valuedouble;
    if (fabs(v - base_v7) <= 1e-9 * fmax(1.0, fabs(v))) return base_v7; // v == base: kept unclamped
    return gc_limit(v, o->min, o->max);
}

// An input override: the value when valid, NULL keeps the base.
static const char *gc_override_input(const GameConfigOption *o, const cJSON *item) {
    return cJSON_IsString(item) && gc_name_ok(item->valuestring, o->len ? o->len : 16) ? item->valuestring : NULL;
}

// ---- weps

static int gc_weapon_bit(const Weapon *w, const int bit) {
    switch (bit) {
        case WB_BURST: return w->burst;
        case WB_OVER_CHARGE: return w->over_charge;
        case WB_SECONDARY: return w->secondary;
        case WB_EQUIPMENT: return w->equipment;
        case WB_AKIMBO: return w->akimbo;
        case WB_NO_AUTO: return w->no_auto;
        case WB_NO_SPREAD: return w->no_spread;
        case WB_NO_AIM: return w->no_aim;
        case WB_PROJECTILE_DISABLE: return w->projectile_disable;
        default: return 0;
    }
}

static void gc_weapon_set_bit(Weapon *w, const int bit, const int on) {
    const unsigned char v = on ? 1 : 0;
    switch (bit) {
        case WB_BURST: w->burst = v; break;
        case WB_OVER_CHARGE: w->over_charge = v; break;
        case WB_SECONDARY: w->secondary = v; break;
        case WB_EQUIPMENT: w->equipment = v; break;
        case WB_AKIMBO: w->akimbo = v; break;
        case WB_NO_AUTO: w->no_auto = v; break;
        case WB_NO_SPREAD: w->no_spread = v; break;
        case WB_NO_AIM: w->no_aim = v; break;
        case WB_PROJECTILE_DISABLE: w->projectile_disable = v; break;
        default: break;
    }
}

// Fields that read 0 as a v7 default (weapons.h / shared.h "0 = x"): that default, else 0. An
// explicit 0 is stored as 1e-6 so it stays a value; reading back turns it into 0 again.
static float gc_weapon_zero_default(const Weapon *w, const char *field) {
    if (field == (const char *) &w->leg_mlt) return 0.5f;     // lsMlt
    if (field == (const char *) &w->move_spread) return 1.0f; // movSpread
    return 0.0f;
}

static float gc_class_zero_default(const ClassConfig *c, const char *field) {
    if (field == (const char *) &c->slide_time_mlt || field == (const char *) &c->slide_accel_mlt ||
        field == (const char *) &c->melee_rate || field == (const char *) &c->turn_speed) return 1.0f; // slT, slA, melRate
    return 0.0f;
}

static double gc_read_zero(const float stored, const float zero_default) {
    if (zero_default == 0.0f) return stored;
    return stored == 0.0f ? zero_default : fabsf(stored) <= 1e-5f ? 0.0 : stored;
}

static float gc_write_zero(const float value, const float zero_default) {
    return zero_default != 0.0f && value == 0.0f ? 1e-6f : value;
}

static void gc_apply_weapon_key(Weapon *w, char *name_buf, const GameConfigOption *o, const cJSON *item) {
    char *field = o->store == GC_STORE_WEP_PJ ? (char *) &w->projectile_config + o->offset : (char *) w + o->offset;
    switch (o->store) {
        case GC_STORE_WEP_NAME: {
            const char *name = gc_override_input(o, item);
            if (name) {
                snprintf(name_buf, GAME_CONFIG_INPUT_MAX + 1, "%s", name);
                w->name = name_buf;
            }
            break;
        }
        case GC_STORE_WEP_BIT:
            gc_weapon_set_bit(w, o->bit, gc_truthy(item));
            break;
        case GC_STORE_WEP_UINT: {
            const double v = gc_override_number(o, item, *(unsigned int *) field);
            if (!isnan(v)) *(unsigned int *) field = (unsigned int) (v < 0.0 ? 0.0 : v);
            break;
        }
        case GC_STORE_WEP_FLOAT:
        case GC_STORE_WEP_PJ: {
            const float zero = o->store == GC_STORE_WEP_FLOAT ? gc_weapon_zero_default(w, field) : 0.0f;
            const double v = gc_override_number(o, item, gc_to_v7(gc_read_zero(*(float *) field, zero), o->unit));
            if (isnan(v)) break;
            *(float *) field = gc_write_zero((float) gc_to_kn(v, o->unit), zero);
            break;
        }
        default: // GC_STORE_NONE: reloY, assetIcon, refDist, rollOff, pj_asset / pj_scale / pj_length
            break;                // (client visuals and custom projectile models KN does not load)
    }
}

static void gc_apply_weapons(Game *game, GameConfigState *st, const cJSON *weps) {
    if (!weps || !st->base_weapons) return;
    const size_t key_count = sizeof(k_weapon_options) / sizeof(k_weapon_options[0]);
    const cJSON *entry;
    cJSON_ArrayForEach(entry, weps) {
        // a host config saved before the ruleset seam keys the KN weapons 29-31 (weapons.h)
        const int raw = gc_entry_id(entry, st->weapon_count);
        const int id = raw < 0 ? -1 : weapon_id_from_legacy(raw, game_ruleset(game)->krunker_weapon_count);
        if (id < 0 || (size_t) id >= st->weapon_count || !cJSON_IsObject(entry) || !st->base_weapons[id]) continue;
        Weapon *w = &st->weapons[id];
        int touched = game->weapons[id] == w;
        for (size_t k = 0; k < key_count; k++) {
            const cJSON *item = gc_item(entry, k_weapon_options[k].var);
            if (!item) continue;
            const int known = id < (int) (sizeof(k_weapon_keys) / sizeof(k_weapon_keys[0]));
            if (known && !((k_weapon_keys[id] >> k) & 1ull)) continue; // the base weapon lacks the key
            if (!touched) {
                *w = *st->base_weapons[id];
                game->weapons[id] = w;
                touched = 1;
            }
            gc_apply_weapon_key(w, st->weapon_names[id], &k_weapon_options[k], item);
        }
    }
}

// ---- cls

const ClassOverrides g_default_class_overrides = {
    .set = 0, .cam_type = 0, .ortho_f = 50.0f, .cam_rotation = 1, .model = 0, .model_fp = 1, .anim_speed = 1.0f,
    .model_scale = 1.0f, .model_pos_scale = 1.0f, .speed_length = 500.0f, .vehicle_drag = 0.97f, .vehicle_angular_drag = 0.94f,
    .vehicle_turn_lean = -2.7f, .vehicle_turn_threshold = 0.0025f, .vehicle_turn_conv = 0.0f, .vehicle_max_power = 0.12f,
    .vehicle_power_force = 0.0012f, .vehicle_max_reverse = 0.0375f, .vehicle_reverse_force = 0.0005f, .jump_cooldown = 1.0f,
    .can_shoot = 1, .shoot_drag = 1.0f, .hip_fire = 1, .crouch_shoot = 1, .air_shoot = 1, .can_melee = 1,
};

const ClassOverrides *class_overrides(const ClassConfig *c) {
    return c && c->ext.set ? &c->ext : &g_default_class_overrides;
}

static int gc_class_bit(const ClassConfig *c, const int bit) {
    return bit == CB_SECONDARY ? c->secondary : bit == CB_WALL_JUMP ? c->wall_jump : bit == CB_BACK_SLIDE ? c->back_slide : 0;
}

// cls[id] movement keys -> ClassConfig.movement (WP-A ClassMovement). Its fields read 0 as the v7
// default, so a valid explicit 0 is written as 1e-6 (no visible difference); lckCmYR is a number:
// movement only tests it (lock_pitch), its value goes to ClassOverrides.lock_y_rot for the camera.
static void gc_class_movement(ClassConfig *c, const GameConfigOption *o, const cJSON *item) {
    ClassMovement *m = &c->movement;
    if (o->kind == GC_BOOL) {
        const unsigned char on = (unsigned char) gc_truthy(item);
        switch (o->bit) {
            case CM_CAN_CROUCH: m->no_crouch = !on; break;
            case CM_DISABLE_JUMP: m->disable_jump = on; break;
            case CM_INFINITE_JUMP: m->infinite_jump = on; break;
            case CM_OVERRIDE_JUMP: m->override_jump = on; break;
            case CM_FREE_SLIDE: m->free_slide = on; break;
            case CM_PLAYER_COLLISION: m->player_collision = on; break;
            default: break;
        }
        return;
    }
    if (o->kind == GC_SELECT) { // movSch
        const int index = gc_select(item, o->option_count);
        m->scheme = index >= 0 ? index : (int) o->def;
        return;
    }
    float *field = NULL;
    switch (o->bit) {
        case CM_SPEED_X: field = &m->speed_x; break;
        case CM_SPEED_Y: field = &m->speed_y; break;
        case CM_SPEED_Z: field = &m->speed_z; break;
        case CM_MOVE_DIR_OFFSET: field = &m->move_dir_offset; break;
        case CM_TURN_RATE: field = &m->turn_rate; break;
        case CM_GROUND_ACCEL: field = &m->ground_accel; break;
        case CM_GROUND_DECEL: field = &m->ground_decel; break;
        case CM_AIR_ACCEL: field = &m->air_accel; break;
        case CM_AIR_DECEL: field = &m->air_decel; break;
        case CM_CROUCH_SPEED: field = &m->crouch_speed; break;
        case CM_LADDER_ACCEL: field = &m->ladder_accel; break;
        case CM_STEP_FREQ: field = &m->step_freq; break;
        case CM_WALL_JUMP_DECEL: field = &m->wall_jump_decel; break;
        case CM_WALL_JUMP_VEL: field = &m->wall_jump_vel; break;
        case CM_LOCK_PITCH: {
            // procInputs only tests it (the input pitch 0, deob-10.0.3.js:191283); the class camera
            // reads its value (pchObjc.rotation.x = -PI/2 * lckCmYR, deob-10.0.3.js:260837-260839)
            const double v = gc_override_number(o, item, c->ext.lock_y_rot);
            if (!isnan(v)) {
                m->lock_pitch = v != 0.0;
                c->ext.lock_y_rot = (float) v;
            }
            return;
        }
        default: return;
    }
    const double v = gc_override_number(o, item, *field ? *field : o->def);
    if (isnan(v)) return;
    *field = v == 0.0 ? 1e-6f : (float) v;
}

static void gc_apply_class_key(ClassConfig *c, const GameConfigOption *o, const cJSON *item) {
    ClassOverrides *x = &c->ext;
    switch (o->store) {
        case GC_STORE_CLS_NAME: {
            const char *name = gc_override_input(o, item);
            if (name) {
                snprintf(x->name, sizeof(x->name), "%s", name);
                c->name = x->name;
            }
            break;
        }
        case GC_STORE_CLS_BIT: {
            const unsigned char v = (unsigned char) gc_truthy(item);
            if (o->bit == CB_SECONDARY) c->secondary = v;
            else if (o->bit == CB_WALL_JUMP) c->wall_jump = v;
            else if (o->bit == CB_BACK_SLIDE) c->back_slide = v;
            break;
        }
        case GC_STORE_CLS_INT:
        case GC_STORE_CLS_FLOAT: {
            char *field = (char *) c + o->offset;
            const float zero = gc_class_zero_default(c, field);
            const double base = o->store == GC_STORE_CLS_INT ? *(int *) field : gc_to_v7(gc_read_zero(*(float *) field, zero), o->unit);
            const double v = gc_override_number(o, item, base);
            if (isnan(v)) break;
            if (o->store == GC_STORE_CLS_INT) *(int *) field = (int) v;
            else *(float *) field = gc_write_zero((float) gc_to_kn(v, o->unit), zero);
            break;
        }
        case GC_STORE_EXT_U8:
            *((unsigned char *) x + o->offset) = (unsigned char) gc_truthy(item);
            break;
        case GC_STORE_EXT_INT:
        case GC_STORE_EXT_FLOAT: {
            char *field = (char *) x + o->offset;
            if (o->kind == GC_SELECT) {
                const int index = gc_select(item, o->option_count);
                *(int *) field = index >= 0 ? index : (int) o->def;
                break;
            }
            const double base = o->store == GC_STORE_EXT_INT ? *(int *) field : gc_to_v7(*(float *) field, o->unit);
            const double v = gc_override_number(o, item, base);
            if (isnan(v)) break;
            if (o->store == GC_STORE_EXT_INT) *(int *) field = (int) v;
            else *(float *) field = (float) gc_to_kn(v, o->unit);
            break;
        }
        case GC_STORE_EXT_STR: {
            const char *s = gc_override_input(o, item);
            if (s) snprintf((char *) x + o->offset, 13, "%s", s);
            break;
        }
        case GC_STORE_MOVE:
            gc_class_movement(c, o, item);
            break;
        default:
            break;
    }
}

// v7 applies a cls key only when the base class defines it; every v7 class has every key with a
// def (classes.js copies the defs in), so the test only dropped regenDelay (no class sets it: a
// v7 bug, fixed here) and the Survivor's secondary. Every key applies here.
static void gc_apply_classes(Game *game, const cJSON *cls) {
    if (!cls || !game->classes) return;
    size_t key_count;
    const GameConfigOption *class_options = gc_class_options(&key_count);
    const cJSON *entry;
    cJSON_ArrayForEach(entry, cls) {
        const int id = gc_entry_id(entry, game->class_count);
        if (id < 0 || !cJSON_IsObject(entry)) continue;
        ClassConfig *c = &game->classes[id];
        for (size_t k = 0; k < key_count; k++) {
            const cJSON *item = gc_item(entry, class_options[k].var);
            if (!item) continue;
            if (!c->ext.set) {
                c->ext = g_default_class_overrides;
                c->ext.set = 1;
            }
            gc_apply_class_key(c, &class_options[k], item);
        }
    }
}

// ---- prks

static void gc_apply_effects(Game *game, GameConfigState *st, const cJSON *prks) {
    if (!prks) return;
    memcpy(st->effects, g_effects, sizeof(st->effects));
    const size_t key_count = sizeof(k_effect_options) / sizeof(k_effect_options[0]);
    const cJSON *entry;
    cJSON_ArrayForEach(entry, prks) {
        const int id = gc_entry_id(entry, EFFECT_COUNT);
        if (id < 0 || !cJSON_IsObject(entry)) continue;
        EffectConfig *e = &st->effects[id];
        for (size_t k = 0; k < key_count; k++) {
            const GameConfigOption *o = &k_effect_options[k];
            const cJSON *item = gc_item(entry, o->var);
            if (!item || !((k_effect_keys[id] >> k) & 1u)) continue; // the base effect lacks the key (hp)
            if (o->store == GC_STORE_EFF_NAME) {
                const char *name = gc_override_input(o, item);
                if (name) {
                    snprintf(st->effect_names[id], sizeof(st->effect_names[id]), "%s", name);
                    e->name = st->effect_names[id];
                }
                continue;
            }
            char *field = (char *) e + o->offset;
            const double base = o->store == GC_STORE_EFF_INT ? *(int *) field : *(float *) field;
            const double v = gc_override_number(o, item, base);
            if (isnan(v)) continue;
            if (o->store == GC_STORE_EFF_INT) *(int *) field = (int) v;
            else *(float *) field = (float) v;
        }
        if (e->max > EFFECT_STACK_MAX) e->max = EFFECT_STACK_MAX; // player.effects stacks / timers per effect
        if (e->max < 1) e->max = 1;
    }
    game->effects = st->effects;
}

// ---------------------------------------------------------------- reading values back

static double gc_movement_value(const ClassMovement *m, const GameConfigOption *o) {
    float v;
    switch (o->bit) { // "0 = v7 default" fields read as their default
        case CM_SCHEME: return m->scheme;
        case CM_CAN_CROUCH: return !m->no_crouch;
        case CM_DISABLE_JUMP: return m->disable_jump;
        case CM_INFINITE_JUMP: return m->infinite_jump;
        case CM_OVERRIDE_JUMP: return m->override_jump;
        case CM_FREE_SLIDE: return m->free_slide;
        case CM_PLAYER_COLLISION: return m->player_collision;
        case CM_LOCK_PITCH: return m->lock_pitch;
        case CM_SPEED_X: v = m->speed_x; break;
        case CM_SPEED_Y: v = m->speed_y; break;
        case CM_SPEED_Z: v = m->speed_z; break;
        case CM_MOVE_DIR_OFFSET: v = m->move_dir_offset; break;
        case CM_TURN_RATE: v = m->turn_rate; break;
        case CM_GROUND_ACCEL: v = m->ground_accel; break;
        case CM_GROUND_DECEL: v = m->ground_decel; break;
        case CM_AIR_ACCEL: v = m->air_accel; break;
        case CM_AIR_DECEL: v = m->air_decel; break;
        case CM_CROUCH_SPEED: v = m->crouch_speed; break;
        case CM_LADDER_ACCEL: v = m->ladder_accel; break;
        case CM_STEP_FREQ: v = m->step_freq; break;
        case CM_WALL_JUMP_DECEL: v = m->wall_jump_decel; break;
        case CM_WALL_JUMP_VEL: v = m->wall_jump_vel; break;
        default: return 0.0;
    }
    return v != 0.0f ? (fabsf(v) <= 1e-5f ? 0.0 : v) : o->def;
}

// applyConfig's "base entry has the key" test: weapons and effects from the v7 tables; every class
// has every classConfig key but regenDelay (none) and the Survivor's secondary.
int game_config_has_key(const GameConfigTable table, const int id, const char *var) {
    size_t count = 0;
    const GameConfigOption *options = game_config_options(table, &count);
    const GameConfigOption *o = game_config_find_option(table, var);
    if (!o || id < 0) return 0;
    const size_t k = (size_t) (o - options);
    switch (table) {
        case GAME_CONFIG_SERVER: return 1;
        case GAME_CONFIG_WEAPON:
            return (size_t) id >= sizeof(k_weapon_keys) / sizeof(k_weapon_keys[0]) || ((k_weapon_keys[id] >> k) & 1ull);
        case GAME_CONFIG_EFFECT:
            return id < EFFECT_COUNT && ((k_effect_keys[id] >> k) & 1u);
        case GAME_CONFIG_CLASS:
            if (!strcmp(var, "regenDelay")) return 0;
            return !(id == CLASS_ID_SURVIVOR && !strcmp(var, "secondary"));
        default: return 0;
    }
}

int game_config_value(const GameConfigTable table, const void *entry, const char *var, double *num, const char **str) {
    const GameConfigOption *o = game_config_find_option(table, var);
    if (!o || !entry) return 0;
    double v = 0.0;
    const char *s = NULL;
    if (table == GAME_CONFIG_SERVER) {
        const GameConfig *g = entry;
        if (o->kind == GC_INPUT) s = (const char *) g + o->offset;
        else v = gc_load_number(g, o);
    } else if (table == GAME_CONFIG_CLASS) {
        const ClassConfig *c = entry;
        const ClassOverrides *x = class_overrides(c);
        switch (o->store) {
            case GC_STORE_CLS_NAME: s = c->name; break;
            case GC_STORE_CLS_INT: v = *(const int *) ((const char *) c + o->offset); break;
            case GC_STORE_CLS_FLOAT: {
                const char *field = (const char *) c + o->offset;
                v = gc_to_v7(gc_read_zero(*(const float *) field, gc_class_zero_default(c, field)), o->unit);
                break;
            }
            case GC_STORE_CLS_BIT: v = gc_class_bit(c, o->bit); break;
            case GC_STORE_EXT_INT: v = *(const int *) ((const char *) x + o->offset); break;
            case GC_STORE_EXT_FLOAT: v = gc_to_v7(*(const float *) ((const char *) x + o->offset), o->unit); break;
            case GC_STORE_EXT_U8: v = *((const unsigned char *) x + o->offset); break;
            case GC_STORE_EXT_STR: s = (const char *) x + o->offset; break;
            case GC_STORE_MOVE: v = o->bit == CM_LOCK_PITCH ? x->lock_y_rot : gc_movement_value(&c->movement, o); break;
            default: return 0;
        }
    } else if (table == GAME_CONFIG_WEAPON) {
        const Weapon *w = entry;
        switch (o->store) {
            case GC_STORE_WEP_NAME: s = w->name; break;
            case GC_STORE_WEP_BIT: v = gc_weapon_bit(w, o->bit); break;
            case GC_STORE_WEP_UINT: v = *(const unsigned int *) ((const char *) w + o->offset); break;
            case GC_STORE_WEP_FLOAT: {
                const char *field = (const char *) w + o->offset;
                v = gc_to_v7(gc_read_zero(*(const float *) field, gc_weapon_zero_default(w, field)), o->unit);
                break;
            }
            case GC_STORE_WEP_PJ: v = *(const float *) ((const char *) &w->projectile_config + o->offset); break;
            default: return 0; // not kept by KN
        }
    } else if (table == GAME_CONFIG_EFFECT) {
        const EffectConfig *e = entry;
        switch (o->store) {
            case GC_STORE_EFF_NAME: s = e->name; break;
            case GC_STORE_EFF_INT: v = *(const int *) ((const char *) e + o->offset); break;
            case GC_STORE_EFF_FLOAT: v = *(const float *) ((const char *) e + o->offset); break;
            default: return 0;
        }
    } else {
        return 0;
    }
    if (num) *num = v;
    if (str) *str = s;
    return 1;
}

// ---------------------------------------------------------------- applyConfig on a game

static void gc_read_class_list(GameConfig *g, const cJSON *classes, const size_t class_count) {
    g->class_list_count = 0;
    const int n = cJSON_IsArray(classes) ? cJSON_GetArraySize(classes) : 0;
    if (n <= 0 || n > (int) class_count || n > GAME_CONFIG_CLASS_LIMITS) return; // resetClasses: every class
    const cJSON *entry;
    cJSON_ArrayForEach(entry, classes) {
        if (!cJSON_IsNumber(entry) || entry->valuedouble < 0 || entry->valuedouble >= (double) class_count || floor(entry->valuedouble) != entry->valuedouble) {
            g->class_list_count = 0;
            return;
        }
        g->class_list[g->class_list_count++] = (int) entry->valuedouble;
    }
}

int game_config_apply(Game *game, const cJSON *cfg, const int clamp, const int host_index) {
    if (!game) return 0;
    GameConfigState *st = gc_state(game);
    if (!st) return 0;

    GameConfig c = game->config; // maps / modes pointers stay
    game_config_read(&c, cfg, clamp, host_index);
    gc_read_class_list(&c, gc_item(cfg, "classes"), game->class_count ? game->class_count : rules_class_count());
    game->config = c;
    game_config_sync_teams(&game->config);

    gc_restore_tables(game);
    const cJSON *weps = gc_item(cfg, "weps"), *cls = gc_item(cfg, "cls"), *prks = gc_item(cfg, "prks");
    if ((weps || cls || prks) && gc_save_bases(game, st)) {
        gc_apply_weapons(game, st, cJSON_IsObject(weps) ? weps : NULL);
        gc_apply_classes(game, cJSON_IsObject(cls) ? cls : NULL);
        gc_apply_effects(game, st, cJSON_IsObject(prks) ? prks : NULL);
    }

    cJSON_Delete(st->applied);
    st->applied = cJSON_CreateObject();
    static const char *const keys[] = {"weps", "cls", "prks"};
    for (size_t i = 0; st->applied && i < sizeof(keys) / sizeof(keys[0]); i++) {
        const cJSON *item = gc_item(cfg, keys[i]);
        if (cJSON_IsObject(item)) cJSON_AddItemToObject(st->applied, keys[i], cJSON_Duplicate(item, 1));
    }
    return 1;
}

int game_config_roster_team(Game *game, const char *account_name) {
    if (!game || !game->config.comp || !account_name || !account_name[0]) return 0;
    const GameConfigState *st = game->config_state;
    const cJSON *teams = st ? gc_item(st->request, "teams") : NULL;
    if (!cJSON_IsArray(teams) || cJSON_GetArraySize(teams) != 2 || !game_config_roster_given(st->request)) return 0;
    for (int t = 0; t < 2; t++) {
        if (gc_roster_list(cJSON_GetArrayItem(teams, t), account_name)) return t + 1;
    }
    return 0;
}

void game_config_host(Game *game, const cJSON *request, const int host_index) {
    GameConfigState *st = gc_state(game);
    if (!st) return;
    st->hosted = 1;
    st->host_index = host_index >= 0 && host_index < GAME_HOST_TYPE_COUNT ? host_index : GAME_HOST_CUSTOM;
    cJSON_Delete(st->request);
    st->request = cJSON_IsObject(request) ? cJSON_Duplicate(request, 1) : NULL;
    st->base = game->config;
    if (gc_host_comp(st->host_index)) {
        // a comp host's defaults (the comp flag, test mode, host type): the base a round with no map config and
        // no request restores (game_apply_map_config). Without them the public rotation of a comp server stayed a
        // custom game (INIT host 0, no comp menu). The maps and modes stay.
        const GameConfig keep = st->base;
        game_config_defaults(&st->base, st->host_index);
        st->base.maps = keep.maps;
        st->base.map_count = keep.map_count;
        st->base.modes = keep.modes;
        st->base.mode_count = keep.mode_count;
    }
}

int game_config_is_custom(const Game *game) {
    return game && game->config_state && (game->config_state->hosted || game->config_state->remote_custom);
}

int game_weapon_req(const Game *game, const Weapon *weapon, const int dev) {
    if (dev || !weapon) return dev;
    const int comp = game && game->config.comp;
    const int custom_ok = !weapon->custom_only || (game_config_is_custom(game) && !comp); // !customOnly || (isCustom && !isComp)
    const int comp_ok = !(weapon->no_comp && comp); // !(noComp && isComp)
    return custom_ok && comp_ok;
}

int game_weapon_level_ok(const Game *game, const Weapon *weapon, const int level, const int dev) {
    if (dev || !weapon || weapon->min_rec <= 0) return 1;                     // account.isDev || ... !minRec
    if (game && (game->is_sandbox || game_config_is_custom(game))) return 1; // isCustom: no level tail
    return level >= weapon->min_rec;                                          // account && account.level >= minRec
}

void game_config_set_host(Game *game, const int uid) {
    if (uid < 0) {
        if (game && game->config_state) game->config_state->has_host = 0;
        return;
    }
    GameConfigState *st = gc_state(game);
    if (!st) return;
    st->has_host = 1;
    st->host_uid = uid;
}

Player *game_host_player(const Game *game) {
    const GameConfigState *st = game ? game->config_state : NULL;
    return st && st->has_host ? game_find_player((Game *) game, st->host_uid) : NULL;
}

int game_apply_map_config(Game *game, const Map *map) {
    GameConfigState *st = game ? game->config_state : NULL;
    if (!st || !st->hosted) return 0;
    const cJSON *map_config = map ? map->game_config : NULL;
    if (!map_config && !st->request) { // a map without config: back to the hosted base
        const GameConfig keep = game->config;
        game->config = st->base;
        game->config.maps = keep.maps;
        game->config.map_count = keep.map_count;
        game->config.modes = keep.modes;
        game->config.mode_count = keep.mode_count;
        game_config_sync_teams(&game->config);
        gc_restore_tables(game);
        cJSON_Delete(st->applied);
        st->applied = NULL;
        return 0;
    }
    // the map's config is the host preset (index.js:6435-6450); the host's own settings win
    cJSON *requested = game_config_from_map(map_config);
    const cJSON *entry;
    cJSON_ArrayForEach(entry, st->request) gc_set(requested, entry->string, cJSON_Duplicate(entry, 1));
    cJSON *cfg = game_config_setup(st->host_index, requested, NULL, 0);
    cJSON_Delete(requested);
    if (!cfg) return 0;
    game_config_apply(game, cfg, GAME_CONFIG_CLAMP_HOSTED, st->host_index);
    cJSON_Delete(cfg);
    return 1;
}

int game_class_allowed(const Game *game, const int class_index) {
    const size_t count = game && game->class_count ? game->class_count : rules_class_count();
    if (class_index < 0 || (size_t) class_index >= count) return 0;
    return !(class_index >= KN_CLASS_BASE && game && game->is_official); // KrunkNative's own: not on official servers
}

int game_class_level_ok(const Game *game, const int class_index, const int level, const int dev) {
    if (!game || !game->is_official || dev) return 1;
    const size_t count = game->class_count ? game->class_count : rules_class_count();
    const ClassConfig *classes = game->classes ? game->classes : rules_classes();
    if (class_index < 0 || (size_t) class_index >= count || !classes[class_index].min_rec) return 1;
    return level >= classes[class_index].min_rec;
}

// KrunkNative's own classes (from KN_CLASS_BASE: the Nettspend) are in every game the gate allows them in, whatever
// the game's class list says: a custom game's list is the host's, and the host's picker does not know them.
// Krunker's classes follow the list (an empty list: every class).
int game_class_listed(const Game *game, const int class_index) {
    if (class_index >= KN_CLASS_BASE) return game_class_allowed(game, class_index);
    if (!game || game->config.class_list_count <= 0) return 1;
    for (int i = 0; i < game->config.class_list_count; i++) {
        if (game->config.class_list[i] == class_index) return 1;
    }
    return 0;
}

int game_config_class_for(const Game *game, const int class_index) {
    if (!game || game->config.class_list_count <= 0 || game_class_listed(game, class_index)) return class_index;
    return game->config.class_list[0];
}

// ---------------------------------------------------------------- sync

void game_config_send(Game *game, const int uid) {
    if (!game) return;
    cJSON *msg = cJSON_CreateObject();
    if (!msg) return;
    cJSON_AddNumberToObject(msg, "host", game->config.host_type);
    if (game_config_is_custom(game)) cJSON_AddBoolToObject(msg, "custom", 1);
    cJSON_AddItemToObject(msg, "settings", game_config_write(&game->config));
    if (game->config.class_list_count > 0) {
        cJSON *classes = cJSON_CreateArray();
        for (int i = 0; i < game->config.class_list_count; i++) cJSON_AddItemToArray(classes, cJSON_CreateNumber(game->config.class_list[i]));
        cJSON_AddItemToObject(msg, "classes", classes);
    }
    const GameConfigState *st = game->config_state;
    const cJSON *entry;
    cJSON_ArrayForEach(entry, (st ? st->applied : NULL)) cJSON_AddItemToObject(msg, entry->string, cJSON_Duplicate(entry, 1));
    char *text = cJSON_PrintUnformatted(msg);
    cJSON_Delete(msg);
    if (!text) return;

    const size_t total = strlen(text);
    if (total <= NET_GAME_CONFIG_MAX) {
        NetGameConfigChunk chunk;
        chunk.total = (uint32_t) total;
        for (size_t offset = 0; offset < total; offset += NET_GAME_CONFIG_CHUNK_SIZE) {
            const size_t length = total - offset < NET_GAME_CONFIG_CHUNK_SIZE ? total - offset : NET_GAME_CONFIG_CHUNK_SIZE;
            chunk.offset = (uint32_t) offset;
            memcpy(chunk.data, text + offset, length);
            game_send(game, uid, NET_PACKET_GAME_CONFIG, &chunk, (uint16_t) (NET_GAME_CONFIG_HEADER_SIZE + length));
        }
    }
    cJSON_free(text);

    const NetNetworkDataPacket *net = game_network_data(game);
    game_send(game, uid, NET_PACKET_NETWORK_DATA, net, sizeof(*net));
}

static void gc_apply_received(Game *game, const cJSON *msg) {
    const cJSON *host = gc_item(msg, "host");
    const int host_index = gc_is_number(host) ? (int) host->valuedouble : GAME_HOST_CUSTOM;
    cJSON *cfg = cJSON_Duplicate(gc_item(msg, "settings"), 1);
    if (!cfg) cfg = cJSON_CreateObject();
    static const char *const keys[] = {"classes", "weps", "cls", "prks"};
    for (size_t i = 0; cfg && i < sizeof(keys) / sizeof(keys[0]); i++) {
        const cJSON *item = gc_item(msg, keys[i]);
        if (item) cJSON_AddItemToObject(cfg, keys[i], cJSON_Duplicate(item, 1));
    }
    game_config_apply(game, cfg, GAME_CONFIG_CLAMP_DEDICATED, host_index); // the server already clamped
    cJSON_Delete(cfg);
    if (game->config_state) game->config_state->remote_custom = (unsigned char) gc_truthy(gc_item(msg, "custom"));
}

int game_config_receive(Game *game, const void *payload, const uint16_t length) {
    GameConfigState *st = gc_state(game);
    if (!st || !payload || length < NET_GAME_CONFIG_HEADER_SIZE) return 0;
    NetGameConfigChunk chunk;
    memcpy(&chunk, payload, length > sizeof(chunk) ? sizeof(chunk) : length);
    const uint32_t n = (uint32_t) (length - NET_GAME_CONFIG_HEADER_SIZE);
    if (chunk.total > NET_GAME_CONFIG_MAX || chunk.offset > chunk.total || n > chunk.total - chunk.offset) return 0;
    if (chunk.offset == 0) {
        free(st->rx);
        st->rx = malloc((size_t) chunk.total + 1);
        st->rx_total = chunk.total;
        st->rx_have = 0;
        if (!st->rx) return 0;
    }
    if (!st->rx || chunk.total != st->rx_total || chunk.offset != st->rx_have) return 0; // out of order: wait for the next offset 0
    memcpy(st->rx + st->rx_have, chunk.data, n);
    st->rx_have += n;
    if (st->rx_have < st->rx_total) return 0;

    st->rx[st->rx_total] = 0;
    cJSON *msg = cJSON_Parse(st->rx);
    free(st->rx);
    st->rx = NULL;
    st->rx_total = st->rx_have = 0;
    if (!msg) return 0;
    gc_apply_received(game, msg);
    cJSON_Delete(msg);
    st->received = 1;
    return 1;
}

void game_config_on_init(Game *game) {
    GameConfigState *st = game ? game->config_state : NULL;
    if (!st) return;
    if (!st->received) { // a server without GAME_CONFIG (older protocol): the defaults
        const GameConfig keep = game->config;
        game->config = *flavor_default_game_config(); // base: g_default_game_config
        game->config.maps = keep.maps;
        game->config.map_count = keep.map_count;
        game->config.modes = keep.modes;
        game->config.mode_count = keep.mode_count;
        game_config_sync_teams(&game->config);
        gc_restore_tables(game);
    }
    if (!st->net_received) st->net = k_default_network;
    st->received = 0;
    st->net_received = 0;
}

// ---------------------------------------------------------------- network rates

void game_network_apply(Game *game, const int tick_hz) {
    GameConfigState *st = gc_state(game);
    if (!st) return;
    st->net = k_default_network;
    if (tick_hz > 0) st->net.tick_hz = (uint16_t) tick_hz;
    if (game->mode && game->mode->config.send_rate > 0) st->net.send_hz = (uint16_t) game->mode->config.send_rate; // mode.sendR
    if (game->mode && game->mode->config.view_dis > 0.0f) st->net.view_distance = game->mode->config.view_dis; // mode.viewDis
}

const NetNetworkDataPacket *game_network_data(const Game *game) {
    return game && game->config_state ? &game->config_state->net : &k_default_network;
}

void game_network_receive(Game *game, const NetNetworkDataPacket *packet) {
    GameConfigState *st = gc_state(game);
    if (!st || !packet) return;
    st->net = *packet;
    if (!st->net.send_hz) st->net.send_hz = k_default_network.send_hz;
    if (!st->net.sync_hz) st->net.sync_hz = k_default_network.sync_hz;
    st->net_received = 1;
}

// game.js syncState: a player's "k" leaves out the players at or past mode.viewDis (spectators
// and connections without a player see everyone).
int game_network_relevant(const Game *game, const Player *viewer, const Player *target) {
    const float dis = game_network_data(game)->view_distance;
    if (dis <= 0.0f || !viewer || !target || viewer == target || !viewer->active) return 1;
    const float dx = target->position.x - viewer->position.x;
    const float dy = target->position.y - viewer->position.y;
    const float dz = target->position.z - viewer->position.z;
    return dx * dx + dy * dy + dz * dz < dis * dis;
}

// ---------------------------------------------------------------- end of game: mts, votes, next match

static int gc_rand(const int n) {
    return n > 0 ? (int) pcg32_boundedrand((uint32_t) n) : 0;
}

static int gc_has_mode(const Game *game, const int mode_id) {
    for (size_t i = 0; game->modes && i < game->mode_count; i++) {
        if (game->modes[i] == mode_id) return 1;
    }
    return 0;
}

static int gc_pick_index(const GameConfigState *st, const int mode_id, const int map_index) {
    for (int i = 0; i < st->mts_count; i++) {
        if (st->mts_mode[i] == mode_id && st->mts_map[i] == map_index) return i;
    }
    return -1;
}

static void gc_pick_add(GameConfigState *st, const int mode_id, const int map_index) {
    if (st->mts_count >= NET_MATCH_PICKS || gc_pick_index(st, mode_id, map_index) >= 0) return;
    st->mts_mode[st->mts_count] = mode_id;
    st->mts_map[st->mts_count] = map_index;
    st->mts_count++;
}

// A map's matchmaking modes: config.modes, else the top-level v3 "modes" (server/index.ts:1164-1176), else [0].
static int gc_map_modes(const cJSON *map, int *out, const int max) {
    const cJSON *list = gc_item(gc_item(map, "config"), "modes");
    if (!cJSON_IsArray(list)) list = gc_item(map, "modes");
    int n = 0;
    const cJSON *entry;
    cJSON_ArrayForEach(entry, (cJSON_IsArray(list) ? list : NULL)) {
        if (n < max && cJSON_IsNumber(entry)) out[n++] = (int) entry->valuedouble;
    }
    if (!n && max > 0) out[n++] = MODE_ID_FFA;
    return n;
}

// KN difference (decisions.md "Public mode rotation"): v7 trusts a map's config.modes. KN also
// checks the map has the objects the mode plays on, because maps without config.modes (custom
// maps, KN maps, the top-level v3 "modes") fall back to lists that were never checked.
enum { GC_MAP_OBJECTIVE = 1, GC_MAP_FLAG = 2, GC_MAP_DOM = 4, GC_MAP_SITE = 8 };

static int gc_map_features(const cJSON *map) {
    int features = 0;
    const cJSON *objects = gc_item(map, "objects"), *obj;
    cJSON_ArrayForEach(obj, (cJSON_IsArray(objects) ? objects : NULL)) {
        switch (map_raw_object_prefab(obj)) {
            case PREFAB_OBJECTIVE: features |= GC_MAP_OBJECTIVE; break;
            case PREFAB_FLAG: features |= GC_MAP_FLAG; break;
            case PREFAB_DOMINATION_ZONE: features |= GC_MAP_DOM; break;
            case PREFAB_BOMB_SITE: features |= GC_MAP_SITE; break;
            default: break;
        }
    }
    return features;
}

static int gc_mode_fits(const GameModeConfig *mode, const int features) {
    if (mode->objective && !(features & GC_MAP_OBJECTIVE)) return 0; // hardpoint, deposit, king, team defender
    if (mode->flags && !(features & GC_MAP_FLAG)) return 0;           // capture the flag, blitz
    if (mode->dom && !(features & GC_MAP_DOM)) return 0;              // domination
    if (mode->site && !(features & GC_MAP_SITE)) return 0;            // defuse
    return 1;
}

static int gc_map_lists_mode(const cJSON *map, const int mode_id) {
    int modes[64];
    const int n = gc_map_modes(map, modes, 64);
    for (int k = 0; k < n; k++) {
        if (modes[k] == mode_id) return 1;
    }
    return 0;
}

int game_map_offers_mode(const cJSON *map, const int mode_id) {
    const GameModeConfig *mode = gc_mode(mode_id);
    return mode && gc_map_lists_mode(map, mode_id) && gc_mode_fits(mode, gc_map_features(map));
}

// A public game's first round, and its next one when the end screen had no picks: the v7.1.4
// server seeds a lobby with a random rotation map (server/index.ts:1106-1114, 3657) and a random
// mode among the map's modes in the rotation (takeRandomStartupModeIndex, :1178-1204, 3659).
// KN: only maps that offer one of game->modes (game_map_offers_mode) count, so the pair is always
// playable; with more than one mode the preventCustomGames modes stay out, as in genMatchList.
int game_pick_rotation_pair(const Game *game, int *map_index, int *mode_index) {
    if (!game || !game->map_count || !game->mode_count || !game->modes) return 0;
    const int strict = game->mode_count > 1;
    int eligible = 0, pick_map = -1, pick_mode = -1;
    for (size_t i = 0; i < game->map_count; i++) {
        const cJSON *map = game->maps[i];
        const int features = gc_map_features(map);
        int candidates = 0, chosen = -1;
        for (size_t m = 0; m < game->mode_count; m++) {
            const GameModeConfig *mode = gc_mode(game->modes[m]);
            if (!mode || (strict && mode->prevent_custom)) continue;
            if (!gc_map_lists_mode(map, game->modes[m]) || !gc_mode_fits(mode, features)) continue;
            int dup = 0; // a mode listed twice in game->modes counts once
            for (size_t d = 0; d < m; d++) dup |= game->modes[d] == game->modes[m];
            if (dup) continue;
            if (gc_rand(++candidates) == 0) chosen = (int) m; // uniform among the map's modes
        }
        if (!candidates) continue;
        if (gc_rand(++eligible) == 0) { // uniform among the maps with a mode
            pick_map = (int) i;
            pick_mode = chosen;
        }
    }
    if (pick_map < 0) return 0;
    if (map_index) *map_index = pick_map;
    if (mode_index) *mode_index = pick_mode;
    return 1;
}

static void gc_shuffle(int *modes, int *maps, const int n) { // UTILS.yateShuffle
    for (int i = n - 1; i > 0; i--) {
        const int j = gc_rand(i + 1);
        const int m = modes[i], p = maps[i];
        modes[i] = modes[j];
        maps[i] = maps[j];
        modes[j] = m;
        maps[j] = p;
    }
}

// getPossibleMatches + genMatchList for a public game: every "mode,map" a map's config.modes
// offers, in the normal and party buckets, FFA first when the rotation has it. KN servers also
// configure their mode rotation (game->modes): a pick must be in it, and the map must have the
// mode's objects (gc_mode_fits). No votes: init picks one of these at random (game_next_match).
static void gc_gen_match_list(Game *game, GameConfigState *st) {
    enum { MAX_PAIRS = 512 };
    int normal_mode[MAX_PAIRS], normal_map[MAX_PAIRS], party_mode[MAX_PAIRS], party_map[MAX_PAIRS];
    int normal = 0, party = 0;
    for (size_t i = 0; i < game->map_count; i++) {
        int modes[64];
        const int n = gc_map_modes(game->maps[i], modes, 64);
        const int features = gc_map_features(game->maps[i]);
        for (int k = 0; k < n; k++) {
            const GameModeConfig *mode = gc_mode(modes[k]);
            if (!mode || mode->prevent_custom || !gc_has_mode(game, modes[k])) continue; // KN has no events (eventIndex 0)
            if (!gc_mode_fits(mode, features)) continue;
            int dup = 0;
            const int in_party = mode->party;
            int *bucket_mode = in_party ? party_mode : normal_mode, *bucket_map = in_party ? party_map : normal_map;
            int *count = in_party ? &party : &normal;
            for (int j = 0; j < *count; j++) dup |= bucket_mode[j] == modes[k] && bucket_map[j] == (int) i;
            if (dup || *count >= MAX_PAIRS) continue;
            bucket_mode[*count] = modes[k];
            bucket_map[*count] = (int) i;
            (*count)++;
        }
    }
    gc_shuffle(normal_mode, normal_map, normal);
    gc_shuffle(party_mode, party_map, party);

    int picks = NET_MATCH_PICKS;
    if (gc_has_mode(game, MODE_ID_FFA)) {
        for (int j = 0; j < normal; j++) {
            if (normal_mode[j] != MODE_ID_FFA) continue;
            gc_pick_add(st, normal_mode[j], normal_map[j]);
            normal_mode[j] = normal_mode[normal - 1];
            normal_map[j] = normal_map[normal - 1];
            normal--;
            picks--;
            break;
        }
    }
    for (int i = 0; i < picks - 1 && normal > 0; i++) { // v7 bug fixed: v7 pushes undefined once empty
        const int j = gc_rand(normal);
        gc_pick_add(st, normal_mode[j], normal_map[j]);
        normal_mode[j] = normal_mode[normal - 1];
        normal_map[j] = normal_map[normal - 1];
        normal--;
    }
    if (party > 0 && st->mts_count < NET_MATCH_PICKS) gc_pick_add(st, party_mode[0], party_map[0]);
}

// Custom and official games: 4 random "mode,map" pairs of config.modes x config.maps, mode 0
// first when listed (endGame :4648-4660).
static void gc_random_pairs(Game *game, GameConfigState *st) {
    const int ffa = gc_has_mode(game, MODE_ID_FFA);
    for (int i = 0; st->mts_count < NET_MATCH_PICKS && i <= 20; i++) {
        const int mode = i == 0 && ffa ? MODE_ID_FFA : game->mode_count ? game->modes[gc_rand((int) game->mode_count)] : MODE_ID_FFA;
        const int map = gc_rand((int) game->map_count);
        gc_pick_add(st, mode, map);
    }
}

static void gc_votes_clear(GameConfigState *st) {
    st->vote_count = 0;
}

void game_match_list_build(Game *game) {
    GameConfigState *st = gc_state(game);
    if (!st || !game->map_count) return;
    st->mts_count = 0;
    gc_votes_clear(st);
    if (st->force_set) {
        const int mode = st->force_mode >= 0 ? st->force_mode : game_mode_id(game);
        const int map = st->force_map >= 0 ? st->force_map : game->current_map_index;
        st->mts_mode[0] = mode;
        st->mts_map[0] = map;
        st->mts_count = 1; // [,,,,].fill(pick): four equal entries, one card
        st->force_set = 0;
        st->force_mode = st->force_map = -1;
    } else if (!game_config_is_custom(game)) {
        gc_gen_match_list(game, st);
        if (!st->mts_count) gc_random_pairs(game, st); // no map offers a rotation mode
    } else {
        gc_random_pairs(game, st);
    }
    game_send_match_votes(game, NET_TARGET_ALL);
}

void game_force_next_match(Game *game, const int mode_id, const int map_index) {
    GameConfigState *st = gc_state(game);
    if (!st) return;
    st->force_set = 1;
    st->force_mode = mode_id;
    st->force_map = map_index;
}

static int gc_vote_of(const GameConfigState *st, const int uid) {
    for (int i = 0; i < st->vote_count; i++) {
        if (st->vote_uid[i] == uid) return i;
    }
    return -1;
}

static void gc_fill_votes(const Game *game, const GameConfigState *st, NetMatchVoteStatePacket *out, const int voted) {
    memset(out, 0, sizeof(*out));
    out->voted = (int8_t) voted;
    out->count = (uint8_t) (st ? st->mts_count : 0);
    for (int i = 0; st && i < st->mts_count; i++) {
        NetMatchVoteEntry *e = &out->entries[i];
        e->mode_id = st->mts_mode[i];
        e->map_index = st->mts_map[i];
        for (int v = 0; v < st->vote_count; v++) e->votes += st->vote_pick[v] == i; // weight 1
        const cJSON *map = st->mts_map[i] >= 0 && (size_t) st->mts_map[i] < game->map_count ? game->maps[st->mts_map[i]] : NULL;
        const cJSON *name = gc_item(map, "name");
        snprintf(e->map_name, sizeof(e->map_name), "%s", cJSON_IsString(name) ? name->valuestring : "");
    }
}

void game_send_match_votes(Game *game, const int uid) {
    const GameConfigState *st = game ? game->config_state : NULL;
    NetMatchVoteStatePacket packet;
    int voted = -1;
    if (st && uid >= 0) {
        const int v = gc_vote_of(st, uid);
        voted = v >= 0 ? st->vote_pick[v] : -1;
    }
    gc_fill_votes(game, st, &packet, voted);
    game_send(game, uid, NET_PACKET_MATCH_VOTE_STATE, &packet, sizeof(packet));
}

int game_vote_match(Game *game, const int uid, const int mode_id, const int map_index) {
    GameConfigState *st = game ? game->config_state : NULL;
    if (!st || game->end_timer <= 0.0f || !st->mts_count) return 0; // only on the end screen
    const int pick = gc_pick_index(st, mode_id, map_index);
    if (pick < 0) return 0;
    const int v = gc_vote_of(st, uid);
    if (v >= 0 && st->vote_pick[v] == pick) { // the same card again: vote withdrawn
        st->vote_uid[v] = st->vote_uid[st->vote_count - 1];
        st->vote_pick[v] = st->vote_pick[st->vote_count - 1];
        st->vote_count--;
    } else if (v >= 0) {
        st->vote_pick[v] = pick;
    } else {
        if (st->vote_count >= st->vote_cap) {
            const int cap = st->vote_cap ? st->vote_cap * 2 : 16;
            int *uids = realloc(st->vote_uid, (size_t) cap * sizeof(int));
            if (uids) st->vote_uid = uids;
            int *picks = realloc(st->vote_pick, (size_t) cap * sizeof(int));
            if (picks) st->vote_pick = picks;
            if (!uids || !picks) return 0;
            st->vote_cap = cap;
        }
        st->vote_uid[st->vote_count] = uid;
        st->vote_pick[st->vote_count] = pick;
        st->vote_count++;
    }
    // "mv": the new totals to everyone (keeping their own vote), the voter with its vote
    NetMatchVoteStatePacket packet;
    gc_fill_votes(game, st, &packet, -2);
    game_send(game, NET_TARGET_EXCEPT(uid), NET_PACKET_MATCH_VOTE_STATE, &packet, sizeof(packet));
    game_send_match_votes(game, uid);
    return 1;
}

int game_next_match(Game *game, int *map_index, int *mode_index) {
    GameConfigState *st = game ? game->config_state : NULL;
    if (!st || !st->mts_count || !game->map_count) return 0;
    int totals[NET_MATCH_PICKS] = {0};
    for (int v = 0; v < st->vote_count; v++) {
        if (st->vote_pick[v] >= 0 && st->vote_pick[v] < st->mts_count) totals[st->vote_pick[v]]++;
    }
    int best = -1, best_votes = 0;
    for (int i = 0; i < st->mts_count; i++) {
        if (totals[i] > best_votes) { // ties keep the first
            best_votes = totals[i];
            best = i;
        }
    }
    if (best < 0) best = gc_rand(st->mts_count); // no votes: UTILS.getRandom(mts)
    const int map = st->mts_map[best], mode = st->mts_mode[best];
    st->mts_count = 0;
    gc_votes_clear(st);
    if (map < 0 || (size_t) map >= game->map_count) return 0;
    const int index = game_mode_index_for(game, mode);
    if (index < 0) return 0;
    if (map_index) *map_index = map;
    if (mode_index) *mode_index = index;
    return 1;
}
