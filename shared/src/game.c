#include <shared.h>

const GameConfig g_default_game_config = {
    .max_players = 2,
    .min_players = 0,
    .game_time = 4,
    .warmup_time = 0.0f,
    .auto_respawn = 0.0f,
    .lives = 0,
    .score_limit = 0,
    .gravity_mlt = 1.0f,
    .jump_mlt = 1.0f,
    .delta_mlt = 1.0f,
    .slide_time = 1.0f,
    .impulse_mlt = 1.0f,
    .wall_jump = 1.0f,
    .strafe_speed = 1.2f,
    .can_slide = 1,
    .air_strafe = 0,
    .auto_jump = 0,
    .bullet_drop = 0,
    .health_regen = 1,
    .kill_rewards = 1,
    .headshots_only = 0,
    .no_secondaries = 0,
    .no_streaks = 0,
    .disable_borders = 0,
    .throwable_melees = 1,
    .select_team = 0,
    .allow_spectate = 1,
    .third_person = 0,
    .hide_nametags = 0,
    .team1_name = "Team 1",
    .team2_name = "Team 2",
    .team1_damage = 1.0f,
    .team2_damage = 1.0f,
    .health_mlt = 1.0f,
    .fire_rate = 1.0f,
    .reload_speed = 1.0f
};

void game_players_add(Game *game, Player *player, const int is_you) {
    if (!game->players) {
        game->player_count = is_you ? 1 : 2;
        game->players = calloc(game->player_count, sizeof(Player));

        if (!game->players) {
            game->player_count = 0;
            return;
        }

        game->players[is_you ? 0 : 1] = player;
        return;
    }

    if (is_you) game->players[0] = player;
    else {
        Player **new_players = realloc(game->players, (game->player_count + 1) * sizeof(Player *));
        if (!new_players) return;

        new_players[game->player_count++] = player;
        game->players = new_players;
    }
}