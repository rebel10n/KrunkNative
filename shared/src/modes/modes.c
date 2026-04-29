#include <shared.h>

const GameModeConfig g_default_game_mode_config = {
    .lives = 0,
    .real_movement = 0,
    .no_weapons = 0,
    .no_regen = 0,
    .no_reloads = 0,
    .no_pickups = 0,
    .teams = 0,
    .dmg_team = 0,
    .site = 0,
    .flags = 0,
    .convert_team = 0,
    .kill_on_convert = 0,
    .friendly = 0,
    .clan_war = 0,
    .starting_loadout = NULL,
    .starting_loadout_size = 0,
    .team_class = {-1, -1, -1},
    .speed_mlt = {1.0f, 1.0f, 1.0f},
    .force_class = 0,
    .health = 0,
    .ammo_limit = 0,
    .hitbox_pad = 1.0f
};

const int g_rotation_modes[] = {0};

GameMode *mode_init(const int index) {
    switch (index) {
        case 0:
            return (GameMode *) ffa_init();
        default:
            return NULL;
    }
}

void mode_fini(GameMode *mode) {
    free(mode);
}