// Krunker v7.1.4 mode table (src/data/gameModes.js -> gameModes/*.js; the readable copies are
// src/modes/<id>.js): every entry in table order, so the index is the id the server sends in
// "init". The function hooks (conditions, winConditions, waitTimer triggers, onKill, update,
// collisionCheck, gameStart, endSort) live in mode_rules.c, mode_objectives.c, mode_dom.c,
// mode_deposit.c and mode_zombies.c; this file holds the data and the mode-key accessors.
// v7 has no ranked mode: ids 37 / 38 stay reserved for ranked (modes.h, doc/v7/decisions.md "Ranked").
#include <shared.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "mode_internal.h"
#include <ruleset.h>

const GameModeConfig g_default_game_mode_config = {
    .id = MODE_ID_FFA,
    .key = "ffa",
    .name = "Free for All",
    .alias = "ffa",
    .game_time = MODE_GAME_TIME_CONFIG,
    .team_class = {-1, -1, -1, -1, -1, -1},
    .speed_mlt = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f},
    .force_class = -1,
    .hitbox_pad = 1.0f,
};

// map.js rotationModes (v3.1.1 [0, 1, 2, 3, 19])
const int g_rotation_modes[5] = {MODE_ID_FFA, MODE_ID_POINT, MODE_ID_CTF, MODE_ID_KC, MODE_ID_DOM};

// endStats ("sid" first, as in the mode files; the end table skips it)
static const char *const k_stats_tdm[] = {"sid", "name", "score", "kills", "deaths", "assists", "reward", NULL};
static const char *const k_stats_point[] = {"sid", "name", "score", "kills", "deaths", "obj", "dmg", "reward", NULL};
static const char *const k_stats_obj[] = {"sid", "name", "score", "kills", "deaths", "obj", "reward", NULL};
static const char *const k_stats_ctf[] = {"sid", "name", "score", "kills", "deaths", "caps", "reward", NULL};
static const char *const k_stats_blitz[] = {"sid", "name", "score", "kills", "deaths", "caps", NULL};
static const char *const k_stats_bhop[] = {"sid", "name", "score", "deaths", NULL};
static const char *const k_stats_hide[] = {"sid", "name", "score", "found", "reward", NULL};
static const char *const k_stats_infect[] = {"sid", "name", "score", "kills", "infected", "reward", NULL};
static const char *const k_stats_race[] = {"sid", "name", "score", "time", NULL};
static const char *const k_stats_lms[] = {"sid", "name", "score", "kills", "deaths", "assists", NULL};
static const char *const k_stats_simon[] = {"sid", "name", "score", "reward", NULL};
static const char *const k_stats_gun[] = {"sid", "name", "weapon", "kills", "melees", "setbacks", "reward", NULL};
static const char *const k_stats_prop[] = {"sid", "name", "score", "kills", NULL};
static const char *const k_stats_stalk[] = {"sid", "name", "score", "reward", NULL};
static const char *const k_stats_oinc[] = {"sid", "name", "kills", "deaths", "melees", NULL};
static const char *const k_stats_trade[] = {"sid", "name", "score", NULL};
static const char *const k_stats_kc[] = {"sid", "name", "score", "kills", "deaths", "confirms", "denies", "reward", NULL};
static const char *const k_stats_def[] = {"sid", "name", "score", "kills", "deaths", "reward", NULL};
static const char *const k_stats_raid[] = {"sid", "name", "score", "time", "dmg", "reward", NULL};
static const char *const k_stats_depo[] = {"sid", "name", "score", "kills", "deaths", "deposits", "denies", "reward", NULL};
static const char *const k_stats_depoffa[] = {"sid", "name", "score", "kills", "deaths", "deposits", "reward", NULL};
static const char *const k_stats_md[] = {"sid", "name", "score", "kills", "deaths", "pickups", "reward", NULL};
static const char *const k_stats_zom[] = {"sid", "name", "score", "zombies", "downs", "rounds", "reward", NULL};

// leaderStats
static const char *const k_lead_skd[] = {"score", "kills", "deaths", NULL};
static const char *const k_lead_sk[] = {"score", "kills", NULL};
static const char *const k_lead_point[] = {"score", "objective", "kills", NULL};
static const char *const k_lead_ctf[] = {"caps", "score", NULL};
static const char *const k_lead_gun[] = {"weaponTier", "kills", NULL};
static const char *const k_lead_depo[] = {"deposits", "kills", "denies", "score", NULL};
static const char *const k_lead_oinc[] = {"lives", "kills", NULL};
static const char *const k_lead_kc[] = {"confirms", "score", "denies", NULL};
static const char *const k_lead_dom[] = {"score", "caps", NULL};
static const char *const k_lead_obj[] = {"objective", "score", NULL};
static const char *const k_lead_depoffa[] = {"deposits", "kills", "score", NULL};
static const char *const k_lead_md[] = {"score", "kills", "pickups", NULL};
static const char *const k_lead_zom[] = {"points", "zombies", "downs", "dmg", NULL};

// gun.js weaponOrder (v3.1.1 [2,16,10,4,11,5,3,9,1,14,6,0,7,8,13,15,12]: adds 27, 21, 18, 22, drops 15)
static const int k_gun_order[] = {2, 27, 16, 10, 4, 11, 21, 5, 3, 9, 1, 14, 6, 0, 18, 7, 8, 13, 22, 12};
static const int k_gun_start[] = {2};
static const int k_aon_start[] = {16};
static const int k_md_start[] = {2};
static const int k_chs_start[] = {0};
static const int k_imp_start[] = {5, 22};
static const int k_zom_start[] = {2};
static const int k_no_big_streaks[] = {4, 5, 6}; // disabledStreaks

static const ModeWaitTimer k_wait_hide[] = {
    {.time = 10.0f, .msg = "picking seeker in ", .trigger = mode_trigger_pick_seeker},
    {.time = 45.0f, .msg = "releasing seeker in ", .trigger = mode_trigger_release_seeker},
};
static const ModeWaitTimer k_wait_infect[] = {
    {.time = 20.0f, .msg = "infection in ", .trigger = mode_trigger_infect_random},
};
static const ModeWaitTimer k_wait_race[] = {
    {.time = 15.0f, .msg = "race starting in ", .trigger = mode_trigger_unlock_move},
};
static const ModeWaitTimer k_wait_simon[] = {
    {.time = 10.0f, .msg = "starts in ", .trigger = mode_trigger_pick_simon},
};
// prop.js: the 2nd trigger also starts the taunt timer (nextTaunt: not ported, KN has no prop taunts)
static const ModeWaitTimer k_wait_prop[] = {
    {.time = 10.0f, .msg = "locking seekers in ", .trigger = mode_trigger_lock_seekers},
    {.time = 30.0f, .msg = "releasing seekers in ", .trigger = mode_trigger_release_seeker, .no_join = 1},
};
static const ModeWaitTimer k_wait_boss[] = {
    {.time = 15.0f, .msg = "picking boss in ", .trigger = mode_trigger_pick_boss},
};
static const ModeWaitTimer k_wait_stalk[] = {
    {.time = 15.0f, .msg = "picking stalker in ", .trigger = mode_trigger_pick_stalker},
};
static const ModeWaitTimer k_wait_oinc[] = {
    {.time = 10.0f, .msg = "starts in ", .trigger = mode_trigger_unlock_move},
    {.time = 0.1f, .msg = "", .no_join = 1},
};
static const ModeWaitTimer k_wait_def[] = {
    {.time = 10.0f, .msg = "match starting in ", .trigger = mode_trigger_def_start, .comp = 1, .no_clear = 1},
};
static const ModeWaitTimer k_wait_trai[] = {
    {.time = 15.0f, .msg = "picking traitors in ", .trigger = mode_trigger_pick_traitors},
};
static const ModeWaitTimer k_wait_raid[] = {
    {.time = 20.0f, .msg = "starting in "},
};

#define WAIT(list) .wait_timers = list, .wait_timer_count = (int) (sizeof(list) / sizeof(list[0]))
#define LOADOUT(field, list) .field = list, .field##_size = (int) (sizeof(list) / sizeof(list[0]))
#define STREAKS_OFF(list) .disabled_streaks = list, .disabled_streak_count = (int) (sizeof(list) / sizeof(list[0]))
#define BASE \
    .game_time = MODE_GAME_TIME_CONFIG, \
    .team_class = {-1, -1, -1, -1, -1, -1}, \
    .speed_mlt = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f}, \
    .force_class = -1, \
    .hitbox_pad = 1.0f
// chs.js / imp.js movement (wallJ: true reads as 1)
#define CHAOS_MOVEMENT \
    .jump_mlt = 1.5f, .ground_accel = 2.0f, .air_accel = 1.4f, .reload_mlt = 0.5f, .fr_slid = 1, \
    .has_wall_jump = 1, .wall_jump = 1.0f, .wall_jump_max = 100.0f, .wall_jump_d = 0.05f, .wall_jump_v = 0.06f, \
    .wall_jump_yv = 2.0f

const GameModeConfig g_mode_configs[MODE_COUNT] = {
    [MODE_ID_FFA] = {BASE, .id = MODE_ID_FFA, .key = "ffa", .name = "Free for All", .alias = "ffa", .leader_stats = k_lead_skd},
    [MODE_ID_TDM] = {BASE, .id = MODE_ID_TDM, .key = "tdm", .name = "Team Deathmatch", .alias = "tdm",
        .end_stats = k_stats_tdm, .teams = 1, .leader_stats = k_lead_skd},
    [MODE_ID_POINT] = {BASE, .id = MODE_ID_POINT, .key = "point", .name = "Hardpoint", .alias = "point",
        .sub = "Attack the Objective", .end_stats = k_stats_point, .leader_stats = k_lead_point, .objective = 1, .teams = 1,
        .game_start = mode_start_objective, .update = mode_update_point},
    [MODE_ID_CTF] = {BASE, .id = MODE_ID_CTF, .key = "ctf", .name = "Capture the Flag", .alias = "ctf",
        .end_stats = k_stats_ctf, .leader_stats = k_lead_ctf, .leader_sort = mode_sort_ctf, .end_sort = mode_sort_ctf,
        .on_kill = mode_kill_flag, .flags = 1, .teams = 1, .no_score_c = 1},
    [MODE_ID_BHOP] = {BASE, .id = MODE_ID_BHOP, .key = "bhop", .name = "Parkour", .alias = "bhop",
        .send_rate = 5, .end_stats = k_stats_bhop, .no_kick = 1, .friendly = 1, .log_time = 1, .show_ms = 1,
        .own_time = 1, .allow_reset = 1, .show_hour = 1, .view_dis = 1000.0f},
    [MODE_ID_HIDE] = {BASE, .id = MODE_ID_HIDE, .key = "hide", .name = "Hide & Seek", .alias = "hide",
        .end_stats = k_stats_hide, .win_condition = mode_win_team1_active, .condition = mode_cond_team1_active,
        .min_players = 2, WAIT(k_wait_hide), .speed_mlt = {1.0f, 0.8f, 1.0f, 1.0f, 1.0f, 1.0f}, .teams = 1,
        .team_options = {"Hiders", "Seekers"}, .kill_on_convert = 1, .no_kick = 1, .show_team = 1, .view_dis = 2000.0f,
        .convert_team = 1, .conv_team = 2, .conv_word = "found", .no_weapons = 1, .friendly = 1, .hide_names = 1},
    [MODE_ID_INFECT] = {BASE, .id = MODE_ID_INFECT, .key = "infect", .name = "Infected", .alias = "infect",
        .end_stats = k_stats_infect, .win_condition = mode_win_team1_active, .condition = mode_cond_team1_active,
        .min_players = 2, WAIT(k_wait_infect), .team_class = {-1, -1, 9, -1, -1, -1},
        .speed_mlt = {1.0f, 1.0f, 1.15f, 1.0f, 1.0f, 1.0f},
        .kill_on_convert = 1, .no_kick = 1, .teams = 1, .team_options = {"Survivors", "Infected"},
        .conv_word = "infected", .convert_team = 1, .conv_team = 2},
    [MODE_ID_RACE] = {BASE, .id = MODE_ID_RACE, .key = "race", .name = "Race", .alias = "race",
        .send_rate = 5, .end_stats = k_stats_race, .game_time = 0.0f, .show_ms = 1, .timed = 1, .log_time = 1,
        .view_dis = 1000.0f, .end_sort = mode_sort_race, .game_start = mode_start_race, .condition = mode_cond_race,
        WAIT(k_wait_race), .no_weapons = 1, .friendly = 1},
    [MODE_ID_LMS] = {BASE, .id = MODE_ID_LMS, .key = "lms", .name = "Last Man Standing", .alias = "lms",
        .end_stats = k_stats_lms, .no_regen = 1, .no_kick = 1, .teams = 1, .lives = 1,
        .win_condition = mode_win_lives, .game_time = 300.0f},
    [MODE_ID_SIMON] = {BASE, .id = MODE_ID_SIMON, .key = "simon", .name = "Simon Says", .alias = "simon",
        .end_stats = k_stats_simon, .condition = mode_cond_team1_active, .min_players = 3, WAIT(k_wait_simon),
        .teams = 1, .view_dis = 1000.0f, .team_options = {"Followers", "Simon"}, .no_kick = 1, .team_see = 1},
    // v7.1.4: no noReward / noStreaks / rewardMlt any more; disabledStreaks [4, 5, 6] instead
    [MODE_ID_GUN] = {BASE, .id = MODE_ID_GUN, .key = "gun", .name = "Gun Game", .alias = "gun", .party = 1,
        .rand_class_scr = 0.5f, .end_stats = k_stats_gun, .leader_stat = "weaponTier", .leader_stats = k_lead_gun,
        .leader_sort = mode_leader_sort_gun, .end_sort = mode_sort_gun,
        .win_condition = mode_win_gun, .condition = mode_cond_gun, .on_kill = mode_kill_gun,
        LOADOUT(starting_loadout, k_gun_start), .keep_loadout = 1, .no_assists = 1, .min_players = 2,
        .no_weapons = 1, .no_pickups = 1, LOADOUT(weapon_order, k_gun_order), STREAKS_OFF(k_no_big_streaks)},
    [MODE_ID_PROP] = {BASE, .id = MODE_ID_PROP, .key = "prop", .name = "Prop Hunt", .alias = "prop", .prop = 1,
        .end_stats = k_stats_prop, .no_regen = 1, .no_kick = 1, .teams = 1, .lives = 1, .view_dis = 2000.0f,
        .win_condition = mode_win_team1_active, .condition = mode_cond_teams_alive, .min_players = 2,
        WAIT(k_wait_prop), .prop_switches = 3, .team_options = {"Prop", "Seekers"}, .team_sort = 1, .show_team = 1,
        .hide_names = 1},
    [MODE_ID_BOSS] = {BASE, .id = MODE_ID_BOSS, .key = "boss", .name = "Boss Hunt", .alias = "boss",
        .win_condition = mode_win_team2_active, .condition = mode_cond_team2_active, WAIT(k_wait_boss),
        .teams = 1, .team_options = {"Heros", "Boss"}, .no_reward = 1, .min_players = 2},
    [MODE_ID_CLAS] = {BASE, .id = MODE_ID_CLAS, .key = "clas", .name = "Classic FFA", .alias = "clas",
        .leader_stats = k_lead_skd, .party = 1, .no_sliding = 1, .no_knives = 1, .no_streaks = 1, .no_wall_jump = 1,
        .has_impulse_mlt = 1, .impulse_mlt = 0.0f, .no_pickups = 1, .no_melee = 1, .real_movement = 1},
    [MODE_ID_DEPO] = {BASE, .id = MODE_ID_DEPO, .key = "depo", .name = "Deposit", .alias = "depo",
        .end_stats = k_stats_depo, .leader_stats = k_lead_depo, .objective = 1, .teams = 1, .tag_limit = 65,
        .obj_score = 50, .no_score_c = 1, .tag_hold_limit = 20, .deposit = 1, .max_per_cluster = 3,
        .game_start = mode_start_rand_objective, .leader_sort = mode_leader_sort_depo, .end_sort = mode_sort_depo,
        .win_condition = mode_win_tag_teams, .condition = mode_cond_tag_limit, .on_kill = mode_kill_deposit,
        .update = mode_update_deposit},
    [MODE_ID_STALK] = {BASE, .id = MODE_ID_STALK, .key = "stalk", .name = "Stalker", .alias = "stalk",
        .end_stats = k_stats_stalk, .amb_ind = 5, .min_players = 2, .has_fog_dist = 1, .fog_dist = 250.0f,
        .has_fog_col = 1, .fog_col = 1842204, .has_sky_col = 1, .sky_col = 1842204,
        .has_light_col = 1, .light_col = 855309, .has_amb_col = 1, .amb_col = 1513239,
        .lives = 1, .condition = mode_cond_stalk, .win_condition = mode_win_team2_active, WAIT(k_wait_stalk),
        .hide_names = 1, .no_reward = 1, .no_kick = 1, .teams = 1, .team_options = {"Living", "Stalker"},
        .speed_mlt = {1.0f, 0.65f, 1.0f, 1.0f, 1.0f, 1.0f}},
    // king.js (rewritten in v7): random hills, +objScore 10 only for a lone occupant, kill bonuses
    [MODE_ID_KING] = {BASE, .id = MODE_ID_KING, .key = "king", .name = "King of the Hill", .alias = "king",
        .end_stats = k_stats_obj, .objective = 1, .kill_mlt = 0.15f, .hill_mlt = 0.05f, .obj_score = 10,
        .game_start = mode_start_rand_objective, .collision_check = mode_collision_king, .update = mode_update_king,
        .on_kill = mode_kill_king},
    [MODE_ID_OINC] = {BASE, .id = MODE_ID_OINC, .key = "oinc", .name = "One in the Chamber", .alias = "oinc", .party = 1,
        .end_stats = k_stats_oinc, .leader_stat = "lives", .leader_stats = k_lead_oinc, .leader_sort = mode_leader_sort_oinc,
        .end_sort = mode_sort_oinc, .condition = mode_cond_oinc,
        .win_condition = mode_win_first_alive, WAIT(k_wait_oinc), .game_start = mode_start_lock,
        .on_kill = mode_kill_oinc, .force_spawn = 3.0f, .lives = 3, .health = 10, .force_class = 10,
        .ammo_limit = 1, .has_ammo_limit = 1, .no_reloads = 1, .hide_names = 1, .no_assists = 1, .no_streaks = 1,
        .min_players = 2, .no_pickups = 1},
    [MODE_ID_TRADE] = {BASE, .id = MODE_ID_TRADE, .key = "trade", .name = "Trade", .alias = "trade",
        .send_rate = 5, .end_stats = k_stats_trade, .no_client_kick = 1, .can_trade = 1, .friendly = 1, .no_hp_bars = 1,
        .view_dis = 500.0f, .name_dist = 60, .game_time = 0.0f, .no_minimap = 1},
    // kc.js v7: the winner is the team score (confirms) with denies / kills / score tie-breaks
    [MODE_ID_KC] = {BASE, .id = MODE_ID_KC, .key = "kc", .name = "Kill Confirmed", .alias = "kc",
        .end_stats = k_stats_kc, .leader_stats = k_lead_kc, .leader_sort = mode_leader_sort_kc, .end_sort = mode_sort_kc,
        .win_condition = mode_win_tag_teams, .condition = mode_cond_tag_limit,
        .on_kill = mode_kill_kc, .tag_limit = 65, .teams = 1, .no_score_c = 1},
    [MODE_ID_DEF] = {BASE, .id = MODE_ID_DEF, .key = "def", .name = "Defuse", .alias = "def",
        .site = 1, .end_stats = k_stats_def, .lives = 1, .no_regen = 1, .teams = 1, .hitbox_pad = 0.5f,
        .game_start = mode_start_def, .real_movement = 1, .win_condition = mode_win_def, .condition = mode_cond_def,
        .on_kill = mode_kill_def, WAIT(k_wait_def)},
    [MODE_ID_SHRP] = {BASE, .id = MODE_ID_SHRP, .key = "shrp", .name = "Sharp Shooter", .alias = "shrp", .party = 1,
        .end_stats = k_stats_def, .leader_stats = k_lead_skd, .obj_weapon = 1, .rand_class_scr = 0.5f,
        .game_start = mode_start_weapon, .on_kill = mode_kill_shrp, .force_class = 9, STREAKS_OFF(k_no_big_streaks)},
    [MODE_ID_TRAI] = {BASE, .id = MODE_ID_TRAI, .key = "trai", .name = "Traitor", .alias = "trai",
        .lives = 1, .teams = 1, .no_dead_chat = 1, .no_kill_feed = 1, .team_options = {"Innocent", "Traitor"},
        .team_colors = {"#37ad47", "#d13838"}, .show_team = 1, .no_regen = 1, .fake_names = 1, .no_score_k = 1,
        .dmg_team = 1, .win_condition = mode_win_lives, .condition = mode_cond_teams_alive, WAIT(k_wait_trai),
        .no_reward = 1, .min_players = 3, .on_kill = mode_kill_trai},
    // raid.js v7: hostable again (hostType 4), new winCondition / condition, onEnd times
    [MODE_ID_RAID] = {BASE, .id = MODE_ID_RAID, .key = "raid", .name = "Raid", .alias = "raid", .event = 0, .party = 1,
        .rand_class_scr = 0.5f, .send_rate = 5, .end_stats = k_stats_raid, .show_ms = 1, .no_regen = 1, .lives = 1,
        .min_players = 1, WAIT(k_wait_raid), .win_condition = mode_win_raid, .condition = mode_cond_raid,
        .on_end = mode_end_raid, .no_weapons = 1, .friendly = 1, .no_clan = 1, .no_free_cam = 1, .view_dis = 500.0f,
        .game_time = 360.0f, .bots_as_ai = 1, .no_assists = 1, .bot_bypass_class = 1, .no_minimap = 1},
    [MODE_ID_BLITZ] = {BASE, .id = MODE_ID_BLITZ, .key = "blitz", .name = "Blitz", .alias = "blitz",
        .end_stats = k_stats_blitz, .on_kill = mode_kill_flag, .rush = 1, .flags = 1, .teams = 1, .no_score_c = 1},
    [MODE_ID_DOM] = {BASE, .id = MODE_ID_DOM, .key = "dom", .name = "Domination", .alias = "dom",
        .sub = "Dominate the Objective", .end_stats = k_stats_ctf, .leader_stats = k_lead_dom, .dom = 1, .teams = 1,
        .no_score_c = 1, .capture_time = 10.0f, .lockout_mlt = 1.5f, .per_player_mlt = 0.2f,
        .collision_check = mode_collision_dom, .on_kill = mode_kill_dom},
    [MODE_ID_SDM] = {BASE, .id = MODE_ID_SDM, .key = "sdm", .name = "Squad Deathmatch", .alias = "sdm",
        .end_stats = k_stats_tdm, .teams = 1, .multi_team = 4},
    [MODE_ID_KRANK] = {BASE, .id = MODE_ID_KRANK, .key = "krank", .name = "Kranked FFA", .alias = "krank", .party = 1,
        .end_stats = k_stats_def, .leader_stats = k_lead_skd, .force_spawn = 5.0f, .kranked_timer = 15.0f,
        .bonus_speed = 1.25f, .bonus_reload = 0.35f, .bonus_fire_rate = 0.85f,
        .update = mode_update_krank, .on_kill = mode_kill_krank},
    [MODE_ID_TDF] = {BASE, .id = MODE_ID_TDF, .key = "tdf", .name = "Team Defender", .alias = "tdf",
        .end_stats = k_stats_obj, .leader_stats = k_lead_obj, .teams = 1, .obj_score = 20, .team_carrier_mlt = 1.0f,
        .non_carrier_mlt = 0.5f, .carrier_mlt = 1.25f, .on_kill = mode_kill_flag, .oneflag = 1,
        .update = mode_update_carrier},
    [MODE_ID_DEPOFFA] = {BASE, .id = MODE_ID_DEPOFFA, .key = "depoffa", .name = "Deposit FFA", .alias = "depoffa",
        .party = 1, .end_stats = k_stats_depoffa, .leader_stats = k_lead_depoffa, .objective = 1, .tag_limit = 65,
        .obj_score = 75, .kill_mlt = 0.5f, .no_score_c = 1, .tag_hold_limit = 20, .deposit = 1, .max_per_cluster = 3,
        .game_start = mode_start_rand_objective, .leader_sort = mode_leader_sort_depo, .end_sort = mode_sort_depo,
        .condition = mode_cond_deposits, .on_kill = mode_kill_deposit, .update = mode_update_deposit},
    [MODE_ID_CAR] = {BASE, .id = MODE_ID_CAR, .key = "car", .name = "Carrier", .alias = "car",
        .end_stats = k_stats_obj, .leader_stats = k_lead_obj, .prevent_custom = 1, .obj_score = 20,
        .team_carrier_mlt = 1.0f, .non_carrier_mlt = 0.5f, .carrier_mlt = 1.25f, .on_kill = mode_kill_flag,
        .oneflag = 1, .update = mode_update_carrier},
    [MODE_ID_AON] = {BASE, .id = MODE_ID_AON, .key = "aon", .name = "All or Nothing", .alias = "aon",
        .prevent_custom = 1, .event = 4, .end_stats = k_stats_def, .leader_stats = k_lead_sk, .on_kill = mode_kill_aon,
        .double_xp = 1, .force_spawn = 3.0f, .force_class = 9, .ammo_limit = 0, .has_ammo_limit = 1, .no_reloads = 1,
        .no_pickups = 1, .rand_class_scr = 0.5f, .no_scr_limit = 1, .no_knives = 1, LOADOUT(starting_loadout, k_aon_start),
        STREAKS_OFF(k_no_big_streaks)},
    [MODE_ID_MD] = {BASE, .id = MODE_ID_MD, .key = "md", .name = "Mag Dump", .alias = "md",
        .prevent_custom = 1, .event = 5, .end_stats = k_stats_md, .leader_stats = k_lead_md, .on_kill = mode_kill_md,
        .double_xp = 1, .force_spawn = 3.0f, .force_class = 9, .no_reloads = 1, .no_pickups = 1,
        .rand_class_scr = 0.5f, .no_scr_limit = 1, .no_knives = 1, LOADOUT(starting_loadout, k_md_start),
        STREAKS_OFF(k_no_big_streaks)},
    [MODE_ID_CHS] = {BASE, .id = MODE_ID_CHS, .key = "chs", .name = "Chaos Snipers", .alias = "chs", .party = 1,
        .end_stats = k_stats_def, .leader_stats = k_lead_sk, .force_spawn = 3.0f, .force_class = 0, .no_pickups = 1,
        .no_knives = 1, .grav_mlt = 0.92f, CHAOS_MOVEMENT, .health = 10, .ammo_limit = 1, .has_ammo_limit = 1,
        .no_scr_limit = 1, LOADOUT(starting_loadout, k_chs_start), STREAKS_OFF(k_no_big_streaks)},
    [MODE_ID_BHFFA] = {BASE, .id = MODE_ID_BHFFA, .key = "bhffa", .name = "Bighead FFA", .alias = "bhffa",
        .leader_stats = k_lead_skd, .party = 1, .bighead = 1, .head_mlt = 0.1f, .head_mlt_max = 3.0f,
        .on_kill = mode_kill_bhffa},
    [MODE_ID_ZOM] = {BASE, .id = MODE_ID_ZOM, .key = "zom", .name = "Zombies", .alias = "zom",
        .no_sliding = 1, .no_streaks = 1, .no_wall_jump = 1, .has_impulse_mlt = 1, .impulse_mlt = 0.0f, .lives = 1,
        .end_stats = k_stats_zom, .leader_stats = k_lead_zom, .send_rate = 5, .min_players = 1, .friendly = 1,
        .view_dis = 500.0f, .no_class_scr = 1, .force_class = 9, .finite_ammo = 1, LOADOUT(starting_loadout, k_zom_start),
        .no_scr_limit = 1, .unrestricted_weapons = 1, .points_per_hit = 0.085f, .starter_score = 150,
        .game_time = 0.0f, .regen_mlt = 0.1f, .prevent_custom = 1, .force_charge = 1, .no_minimap = 1,
        .win_condition = mode_win_team2, .condition = mode_cond_raid, // zom.js: winCondition 2, someone alive
        .game_start = mode_zombies_game_start, .update = mode_zombies_update, .on_kill = mode_zombies_on_kill,
        .on_pre_end = mode_zombies_on_pre_end},
    [MODE_ID_IMP] = {BASE, .id = MODE_ID_IMP, .key = "imp", .name = "Impulsed", .alias = "imp", .party = 1,
        .end_stats = k_stats_def, .leader_stats = k_lead_sk, .force_spawn = 3.0f, .force_class = 0, .no_pickups = 1,
        .expl_mlt = 5.0f, .expl_size_mlt = 2.0f, .grav_mlt = 1.05f, .has_impulse_mlt = 1, .impulse_mlt = 3.0f,
        .non_obj_impulse = 1, CHAOS_MOVEMENT, .no_scr_limit = 1, LOADOUT(starting_loadout, k_imp_start),
        STREAKS_OFF(k_no_big_streaks), .prevent_custom = 1, .on_kill = mode_kill_imp},
};

int mode_count(void) {
    return MODE_COUNT;
}

const GameModeConfig *mode_config(const int id) {
    // the process ruleset's table (ruleset.h; v7.1.4's is g_mode_configs itself)
    const Ruleset *rules = ruleset_current();
    const GameModeConfig *table = rules->modes ? rules->modes : g_mode_configs;
    return id >= 0 && id < MODE_COUNT && (size_t) id < (rules->modes ? rules->mode_count : MODE_COUNT) ? &table[id] : NULL;
}

const GameModeConfig *mode_list(int *count) {
    if (count) *count = MODE_COUNT;
    const Ruleset *rules = ruleset_current();
    return rules->modes ? rules->modes : g_mode_configs;
}

const char *mode_name(const int id) {
    const GameModeConfig *config = mode_config(id);
    return config ? config->name : "unknown mode";
}

// customGameConfig.js setupGameConfig strips the preventCustomGames modes; Zombies (35) is hosted
// through hostType 3 only (WP-E), Ranked (37 / 38) never.
int mode_hostable(const int id) {
    const GameModeConfig *config = mode_config(id);
    return config && id < MODE_V7_TABLE_SIZE && !config->prevent_custom;
}

static int mode_text_equal(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        if (tolower((unsigned char) *a) != tolower((unsigned char) *b)) return 0;
        a++;
        b++;
    }
    return !*a && !*b;
}

int mode_find(const char *query) {
    if (!query || !*query) return -1;

    char *end = NULL;
    const long number = strtol(query, &end, 10);
    if (end && !*end) return number >= 0 && number < MODE_COUNT ? (int) number : -1;

    for (int i = 0; i < MODE_COUNT; i++) {
        const GameModeConfig *config = &g_mode_configs[i];
        if (mode_text_equal(query, config->alias) || mode_text_equal(query, config->key) || mode_text_equal(query, config->name)) return i;
    }

    return -1;
}

GameMode *mode_init(const int id) {
    const GameModeConfig *config = mode_config(id);
    if (!config) return NULL;

    GameMode *mode = calloc(1, sizeof(GameMode));
    if (!mode) return NULL;

    mode->config = *config;
    mode->active_objective = -1;
    mode->flag_carrier_uid = -1;
    for (int i = 0; i < MODE_MAX_FLAGS; i++) mode->flags[i].carrier_uid = -1;

    return mode;
}

void mode_fini(GameMode *mode) {
    free(mode);
}

int mode_is_team(const Game *game) {
    return game && game->mode && game->mode->config.teams;
}

int mode_team_count(const Game *game) {
    if (!mode_is_team(game)) return 0;
    const int teams = game->mode->config.multi_team ? game->mode->config.multi_team : 2;
    return teams < MODE_TEAMS ? teams : MODE_TEAMS - 1;
}

int mode_team_class(const Game *game, const int team) {
    if (!game || !game->mode || team < 0 || team >= MODE_TEAMS) return -1;
    return game->mode->config.team_class[team];
}

// teamNm1..5: teamOptions, else the host's team names (nameTeam1..5), else "Team N".
const char *mode_team_name(const Game *game, const int team) {
    static const char *const k_default[MODE_TEAMS] = {"", "Team 1", "Team 2", "Team 3", "Team 4", "Team 5"};
    if (team < 1 || team >= MODE_TEAMS) return "";
    if (team <= 2 && game && game->mode && game->mode->config.team_options[team - 1]) return game->mode->config.team_options[team - 1];

#ifdef GAME_CONFIG_V7 // WP-E: nameTeam1..5
    const char *configured = game ? game->config.team_names[team] : NULL;
#else
    const char *configured = game && team <= 2 ? (team == 1 ? game->config.team1_name : game->config.team2_name) : NULL;
#endif
    for (const char *c = configured; c && *c; c++) {
        if (!isspace((unsigned char) *c)) return configured;
    }

    return k_default[team];
}

// ------------------------------------------------------------------ mode keys for the simulation

static const GameModeConfig *cfg(const Game *game) {
    return game && game->mode ? &game->mode->config : NULL;
}

float mode_grav_mlt(const Game *game) {
    const GameModeConfig *c = cfg(game);
    return c && c->grav_mlt > 0.0f ? c->grav_mlt : 1.0f;
}

float mode_jump_mlt(const Game *game) {
    const GameModeConfig *c = cfg(game);
    return c && c->jump_mlt > 0.0f ? c->jump_mlt : 1.0f;
}

float mode_ground_accel(const Game *game) {
    const GameModeConfig *c = cfg(game);
    return c && c->ground_accel > 0.0f ? c->ground_accel : 1.0f;
}

int mode_fr_slid(const Game *game) {
    const GameModeConfig *c = cfg(game);
    return c && c->fr_slid;
}

int mode_no_sliding(const Game *game) {
    const GameModeConfig *c = cfg(game);
    return c && c->no_sliding;
}

int mode_no_wall_jump(const Game *game) {
    const GameModeConfig *c = cfg(game);
    return c && c->no_wall_jump;
}

int mode_wall_jump(const Game *game, float *wall_jump, float *wall_jump_max, float *d, float *v, float *yv) {
    const GameModeConfig *c = cfg(game);
    if (!c || !c->has_wall_jump) return 0;
    if (wall_jump) *wall_jump = c->wall_jump;
    if (wall_jump_max) *wall_jump_max = c->wall_jump_max;
    if (d && c->wall_jump_d > 0.0f) *d = c->wall_jump_d;
    if (v && c->wall_jump_v > 0.0f) *v = c->wall_jump_v;
    if (yv) *yv = c->wall_jump_yv > 0.0f ? c->wall_jump_yv : 1.0f;
    return 1;
}

float mode_reload_mlt(const Game *game, const Player *player) {
    const GameModeConfig *c = cfg(game);
    if (!c) return 1.0f;
    const float kranked = player && player->mode.is_kranked && c->bonus_reload > 0.0f ? c->bonus_reload : 1.0f;
    return kranked * (c->reload_mlt > 0.0f ? c->reload_mlt : 1.0f);
}

float mode_fire_rate_mlt(const Game *game, const Player *player) {
    const GameModeConfig *c = cfg(game);
    return c && player && player->mode.is_kranked && c->bonus_fire_rate > 0.0f ? c->bonus_fire_rate : 1.0f;
}

float mode_speed_mlt(const Game *game, const Player *player) {
    const GameModeConfig *c = cfg(game);
    return c && player && player->mode.is_kranked && c->bonus_speed > 0.0f ? c->bonus_speed : 1.0f;
}

int mode_ammo_limit(const Game *game, const int weapon_ammo) {
    const GameModeConfig *c = cfg(game);
    if (!c || weapon_ammo <= 0) return weapon_ammo; // weapon.ammo != null && ammoLimit != null
    if (c->has_ammo_limit) return c->ammo_limit;
    return c->ammo_limit > 0 ? c->ammo_limit : weapon_ammo;
}

int mode_no_knives(const Game *game) {
    const GameModeConfig *c = cfg(game);
    return c && c->no_knives;
}

int mode_no_melee(const Game *game) {
    const GameModeConfig *c = cfg(game);
    return c && c->no_melee;
}

float mode_explosion_damage_mlt(const Game *game, const Player *source, const Player *target) {
    const GameModeConfig *c = cfg(game);
    return c && c->expl_mlt > 0.0f && target != source ? c->expl_mlt : 1.0f;
}

float mode_explosion_size_mlt(const Game *game) {
    const GameModeConfig *c = cfg(game);
    return c && c->expl_size_mlt > 0.0f ? c->expl_size_mlt : 1.0f;
}

float mode_head_mlt(const Player *player) {
    return player && player->mode.head_mlt > 0.0f ? player->mode.head_mlt : 1.0f;
}

int mode_streak_disabled(const Game *game, const int streak) {
    const GameModeConfig *c = cfg(game);
    if (!c) return 0;
    for (int i = 0; i < c->disabled_streak_count; i++) {
        if (c->disabled_streaks[i] == streak) return 1;
    }
    return 0;
}
