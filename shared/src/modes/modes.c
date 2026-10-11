// Krunker v3.1.1 mode table (src/data/modes.js, Krunker.Unpacked-v3.1.1/50.js): every
// entry in table order, so the index is the id the server sends in "init". The
// function hooks (conditions, winConditions, waitTimer triggers, onKill, gameStart,
// endSort) live in mode_rules.c; this file only holds the data.
#include <shared.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "mode_internal.h"

const GameModeConfig g_default_game_mode_config = {
    .id = MODE_ID_FFA,
    .key = "ffa",
    .name = "Free for All",
    .alias = "ffa",
    .game_time = MODE_GAME_TIME_CONFIG,
    .team_class = {-1, -1, -1},
    .speed_mlt = {1.0f, 1.0f, 1.0f},
    .force_class = -1,
    .hitbox_pad = 1.0f,
};

const int g_rotation_modes[5] = {MODE_ID_FFA, MODE_ID_TDM, MODE_ID_POINT, MODE_ID_CTF, MODE_ID_KC};

// endStats ("sid" first, as in modes.js; the end table skips it)
static const char *const k_stats_tdm[] = {"sid", "name", "score", "kills", "deaths", "assists", "reward", NULL};
static const char *const k_stats_point[] = {"sid", "name", "score", "kills", "deaths", "objective", "reward", NULL};
static const char *const k_stats_ctf[] = {"sid", "name", "score", "kills", "deaths", "caps", "reward", NULL};
static const char *const k_stats_bhop[] = {"sid", "name", "score", "deaths", NULL};
static const char *const k_stats_hide[] = {"sid", "name", "score", "found", "reward", NULL};
static const char *const k_stats_infect[] = {"sid", "name", "score", "kills", "infected", "reward", NULL};
static const char *const k_stats_race[] = {"sid", "name", "score", "time", NULL};
static const char *const k_stats_lms[] = {"sid", "name", "score", "kills", "deaths", "assists", NULL};
static const char *const k_stats_simon[] = {"sid", "name", "score", "reward", NULL};
static const char *const k_stats_gun[] = {"sid", "name", "weapon", "kills", "melees", "setbacks", "reward", NULL};
static const char *const k_stats_prop[] = {"sid", "name", "score", "kills", NULL};
static const char *const k_stats_ranked[] = {"sid", "name", "score", "kills", "deaths", "MMR", NULL};
static const char *const k_stats_stalk[] = {"sid", "name", "score", "reward", NULL};
static const char *const k_stats_oinc[] = {"sid", "name", "kills", "deaths", "melees", NULL};
static const char *const k_stats_trade[] = {"sid", "name", "score", NULL};
static const char *const k_stats_kc[] = {"sid", "name", "score", "kills", "deaths", "confirms", "denies", "reward", NULL};
static const char *const k_stats_def[] = {"sid", "name", "score", "kills", "deaths", "reward", NULL};
static const char *const k_stats_raid[] = {"sid", "name", "score", "time", "dmg", "reward", NULL};

static const int k_gun_order[] = {2, 16, 10, 4, 11, 5, 3, 9, 1, 14, 6, 0, 7, 8, 13, 15, 12};
static const int k_gun_start[] = {2};

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
static const ModeWaitTimer k_wait_prop[] = {
    {.time = 10.0f, .msg = "locking seekers in ", .trigger = mode_trigger_lock_seekers},
    {.time = 30.0f, .msg = "releasing seekers in ", .trigger = mode_trigger_release_seeker, .no_join = 1},
};
static const ModeWaitTimer k_wait_boss[] = {
    {.time = 15.0f, .msg = "picking boss in ", .trigger = mode_trigger_pick_boss},
};
static const ModeWaitTimer k_wait_ranked[] = {
    {.time = 10.0f, .msg = "starts in ", .trigger = mode_trigger_unlock_move},
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
#define BASE \
    .game_time = MODE_GAME_TIME_CONFIG, \
    .team_class = {-1, -1, -1}, \
    .speed_mlt = {1.0f, 1.0f, 1.0f}, \
    .force_class = -1, \
    .hitbox_pad = 1.0f

const GameModeConfig g_mode_configs[MODE_COUNT] = {
    [MODE_ID_FFA] = {BASE, .id = MODE_ID_FFA, .key = "ffa", .name = "Free for All", .alias = "ffa"},
    [MODE_ID_TDM] = {BASE, .id = MODE_ID_TDM, .key = "tdm", .name = "Team Deathmatch", .alias = "tdm",
        .end_stats = k_stats_tdm, .teams = 1},
    [MODE_ID_POINT] = {BASE, .id = MODE_ID_POINT, .key = "point", .name = "Hardpoint", .alias = "point",
        .end_stats = k_stats_point, .objective = 1, .teams = 1, .game_start = mode_start_objective},
    [MODE_ID_CTF] = {BASE, .id = MODE_ID_CTF, .key = "ctf", .name = "Capture the Flag", .alias = "ctf",
        .end_stats = k_stats_ctf, .on_kill = mode_kill_ctf, .flags = 1, .teams = 1, .no_score_c = 1},
    [MODE_ID_BHOP] = {BASE, .id = MODE_ID_BHOP, .key = "bhop", .name = "Parkour", .alias = "bhop",
        .send_rate = 5, .end_stats = k_stats_bhop, .no_kick = 1, .friendly = 1},
    [MODE_ID_HIDE] = {BASE, .id = MODE_ID_HIDE, .key = "hide", .name = "Hide & Seek", .alias = "hide",
        .end_stats = k_stats_hide, .win_condition = mode_win_team1_active, .condition = mode_cond_team1_active,
        .min_players = 2, WAIT(k_wait_hide), .speed_mlt = {1.0f, 0.8f, 1.0f}, .teams = 1,
        .team_options = {"Hiders", "Seekers"}, .kill_on_convert = 1, .no_kick = 1, .show_team = 1,
        .convert_team = 1, .conv_team = 2, .conv_word = "found", .no_weapons = 1, .friendly = 1, .hide_names = 1},
    [MODE_ID_INFECT] = {BASE, .id = MODE_ID_INFECT, .key = "infect", .name = "Infected", .alias = "infect",
        .end_stats = k_stats_infect, .win_condition = mode_win_team1_active, .condition = mode_cond_team1_active,
        .min_players = 2, WAIT(k_wait_infect), .team_class = {-1, -1, 9}, .speed_mlt = {1.0f, 1.0f, 1.15f},
        .kill_on_convert = 1, .no_kick = 1, .teams = 1, .team_options = {"Survivors", "Infected"},
        .conv_word = "infected", .convert_team = 1, .conv_team = 2},
    [MODE_ID_RACE] = {BASE, .id = MODE_ID_RACE, .key = "race", .name = "Race", .alias = "race",
        .send_rate = 5, .end_stats = k_stats_race, .game_time = 0.0f, .show_ms = 1, .timed = 1,
        .end_sort = mode_sort_race, .game_start = mode_start_race, .condition = mode_cond_race,
        WAIT(k_wait_race), .no_weapons = 1, .friendly = 1},
    [MODE_ID_LMS] = {BASE, .id = MODE_ID_LMS, .key = "lms", .name = "Last Man Standing", .alias = "lms",
        .end_stats = k_stats_lms, .no_regen = 1, .no_kick = 1, .teams = 1, .lives = 1,
        .win_condition = mode_win_lives, .game_time = 300.0f},
    [MODE_ID_SIMON] = {BASE, .id = MODE_ID_SIMON, .key = "simon", .name = "Simon Says", .alias = "simon",
        .end_stats = k_stats_simon, .condition = mode_cond_team1_active, .min_players = 3, WAIT(k_wait_simon),
        .teams = 1, .team_options = {"Followers", "Simon"}, .no_kick = 1},
    [MODE_ID_GUN] = {BASE, .id = MODE_ID_GUN, .key = "gun", .name = "Gun Game", .alias = "gun",
        .end_stats = k_stats_gun, .leader_stat = "weaponTier", .end_sort = mode_sort_gun,
        .win_condition = mode_win_gun, .condition = mode_cond_gun, .on_kill = mode_kill_gun,
        LOADOUT(starting_loadout, k_gun_start), .keep_loadout = 1, .reward_mlt = 50, .no_reward = 1,
        .no_assists = 1, .no_streaks = 1, .min_players = 2, .no_weapons = 1, .no_pickups = 1,
        LOADOUT(weapon_order, k_gun_order)},
    [MODE_ID_PROP] = {BASE, .id = MODE_ID_PROP, .key = "prop", .name = "Prop Hunt", .alias = "prop",
        .end_stats = k_stats_prop, .no_regen = 1, .no_kick = 1, .teams = 1, .lives = 1,
        .win_condition = mode_win_team1_active, .condition = mode_cond_teams_alive, .min_players = 2,
        WAIT(k_wait_prop), .team_options = {"Prop", "Seekers"}, .team_sort = 1, .show_team = 1, .hide_names = 1},
    [MODE_ID_BOSS] = {BASE, .id = MODE_ID_BOSS, .key = "boss", .name = "Boss Hunt", .alias = "boss",
        .win_condition = mode_win_team2_active, .condition = mode_cond_team2_active, WAIT(k_wait_boss),
        .teams = 1, .team_options = {"Heros", "Boss"}, .no_reward = 1, .min_players = 2},
    [MODE_ID_RANKED_POINT] = {BASE, .id = MODE_ID_RANKED_POINT, .key = "ranked", .name = "Ranked", .alias = "ranked",
        .no_reward = 1, .prevent_custom = 1, .end_stats = k_stats_ranked, .is_ranked = 1, .no_kick = 1,
        .need_all = 1, .inf_end_timer = 1, .objective = 1, .teams = 1, .force_spawn = 15.0f,
        WAIT(k_wait_ranked), .game_start = mode_start_lock_objective},
    [MODE_ID_RANKED] = {BASE, .id = MODE_ID_RANKED, .key = "ranked", .name = "Ranked", .alias = "ranked",
        .no_reward = 1, .prevent_custom = 1, .end_stats = k_stats_ranked, .is_ranked = 1, .no_kick = 1,
        .need_all = 1, .inf_end_timer = 1, .no_regen = 1, .kill_sort = 1, .force_spawn = 15.0f,
        .win_condition = mode_win_kills, .condition = mode_cond_kills, WAIT(k_wait_ranked),
        .game_start = mode_start_lock},
    [MODE_ID_STALK] = {BASE, .id = MODE_ID_STALK, .key = "stalk", .name = "Stalker", .alias = "stalk",
        .end_stats = k_stats_stalk, .amb_ind = 5, .min_players = 2, .has_fog_dist = 1, .fog_dist = 250.0f,
        .has_fog_col = 1, .fog_col = 1842204, .has_sky_col = 1, .sky_col = 1842204,
        .has_light_col = 1, .light_col = 855309, .has_amb_col = 1, .amb_col = 1513239,
        .lives = 1, .condition = mode_cond_stalk, .win_condition = mode_win_team2_active, WAIT(k_wait_stalk),
        .hide_names = 1, .no_reward = 1, .no_kick = 1, .teams = 1, .team_options = {"Living", "Stalker"},
        .speed_mlt = {1.0f, 0.65f, 1.0f}},
    [MODE_ID_KING] = {BASE, .id = MODE_ID_KING, .key = "king", .name = "King of the Hill", .alias = "king",
        .end_stats = k_stats_point, .objective = 1, .no_score_k = 1, .obj_score = 20,
        .game_start = mode_start_objective},
    [MODE_ID_OINC] = {BASE, .id = MODE_ID_OINC, .key = "oinc", .name = "One in the Chamber", .alias = "oinc",
        .end_stats = k_stats_oinc, .leader_stat = "lives", .end_sort = mode_sort_oinc, .condition = mode_cond_oinc,
        .win_condition = mode_win_first_alive, WAIT(k_wait_oinc), .game_start = mode_start_lock,
        .on_kill = mode_kill_oinc, .force_spawn = 3.0f, .lives = 3, .health = 10, .force_class = 10,
        .ammo_limit = 1, .no_reloads = 1, .hide_names = 1, .no_reward = 1, .no_assists = 1, .no_streaks = 1,
        .min_players = 2, .no_pickups = 1},
    [MODE_ID_TRADE] = {BASE, .id = MODE_ID_TRADE, .key = "trade", .name = "Trade", .alias = "trade",
        .send_rate = 5, .end_stats = k_stats_trade, .no_kick = 1, .can_trade = 1, .friendly = 1, .game_time = 0.0f},
    [MODE_ID_KC] = {BASE, .id = MODE_ID_KC, .key = "kc", .name = "Kill Confirmed", .alias = "kc",
        .end_stats = k_stats_kc, .end_sort = mode_sort_kc, .win_condition = mode_win_kc, .condition = mode_cond_kc,
        .on_kill = mode_kill_kc, .tag_limit = 65, .teams = 1, .no_score_c = 1},
    [MODE_ID_DEF] = {BASE, .id = MODE_ID_DEF, .key = "def", .name = "Defuse", .alias = "def",
        .site = 1, .end_stats = k_stats_def, .lives = 1, .no_regen = 1, .teams = 1, .hitbox_pad = 0.5f,
        .game_start = mode_start_def, .real_movement = 1, .win_condition = mode_win_def, .condition = mode_cond_def,
        .on_kill = mode_kill_def, WAIT(k_wait_def)},
    [MODE_ID_SHRP] = {BASE, .id = MODE_ID_SHRP, .key = "shrp", .name = "Sharp Shooter", .alias = "shrp",
        .end_stats = k_stats_def, .obj_weapon = 1, .game_start = mode_start_weapon, .force_class = 9},
    [MODE_ID_TRAI] = {BASE, .id = MODE_ID_TRAI, .key = "trai", .name = "Traitor", .alias = "trai",
        .lives = 1, .teams = 1, .no_dead_chat = 1, .no_kill_feed = 1, .team_options = {"Innocent", "Traitor"},
        .team_colors = {"#37ad47", "#d13838"}, .show_team = 1, .no_regen = 1, .fake_names = 1, .no_score_k = 1,
        .dmg_team = 1, .win_condition = mode_win_lives, .condition = mode_cond_teams_alive, WAIT(k_wait_trai),
        .no_reward = 1, .min_players = 3},
    [MODE_ID_RAID] = {BASE, .id = MODE_ID_RAID, .key = "raid", .name = "Raid", .alias = "raid",
        .send_rate = 5, .end_stats = k_stats_raid, .prevent_custom = 1, .show_ms = 1, .no_regen = 1, .lives = 1,
        .game_time = 360.0f, .min_players = 1, WAIT(k_wait_raid), .condition = mode_cond_raid,
        .no_weapons = 1, .friendly = 1, .no_clan = 1, .no_free_cam = 1},
};

int mode_count(void) {
    return MODE_COUNT;
}

const GameModeConfig *mode_config(const int id) {
    return id >= 0 && id < MODE_COUNT ? &g_mode_configs[id] : NULL;
}

const GameModeConfig *mode_list(int *count) {
    if (count) *count = MODE_COUNT;
    return g_mode_configs;
}

const char *mode_name(const int id) {
    const GameModeConfig *config = mode_config(id);
    return config ? config->name : "unknown mode";
}

int mode_hostable(const int id) {
    const GameModeConfig *config = mode_config(id);
    return config && !config->prevent_custom;
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
    for (int i = 0; i < MODE_MAX_FLAGS; i++) mode->flags[i].carrier_uid = -1;

    return mode;
}

void mode_fini(GameMode *mode) {
    free(mode);
}

int mode_is_team(const Game *game) {
    return game && game->mode && game->mode->config.teams;
}

int mode_team_class(const Game *game, const int team) {
    if (!game || !game->mode || team < 0 || team >= MODE_TEAMS) return -1;
    return game->mode->config.team_class[team];
}

// teamNm1/teamNm2: teamOptions, else the host's team names, else "Team N".
const char *mode_team_name(const Game *game, const int team) {
    if (team < 1 || team > 2) return "";
    if (game && game->mode && game->mode->config.team_options[team - 1]) return game->mode->config.team_options[team - 1];

    const char *configured = game ? (team == 1 ? game->config.team1_name : game->config.team2_name) : NULL;
    for (const char *c = configured; c && *c; c++) {
        if (!isspace((unsigned char) *c)) return configured;
    }

    return team == 1 ? "Team 1" : "Team 2";
}
