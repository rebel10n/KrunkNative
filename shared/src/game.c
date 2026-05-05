#include <shared.h>
#include <pcg_basic.h>

#ifdef KRUNKNATIVE_CLIENT
#include <client.h>
#endif

const GameConfig g_default_game_config = {
    .max_players = 2,
    .min_players = 0,
    .game_time = 4,
    .warmup_time = 0.0f,
    .auto_respawn = 0,
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

void game_clear_players(Game *game) {
    if (game->player_count) {
        free(game->players);
        game->player_count = 0;
    }
}

void game_configure(
    Game *game, const GameConfig *config,
    const cJSON **maps, const size_t map_count,
    int *modes, const size_t mode_count,
    Weapon **weapons, const size_t weapon_count,
    ClassConfig *classes, size_t class_count
) {
    if (config) game->config = *config;
    else game->config = g_default_game_config;

    if (game->map_count) {
        free(game->maps);
        game->map_count = 0;
    }

    if (map_count) {
        game->maps = maps;
        game->map_count = map_count;
    } else {
        load_default_maps();

        game->map_count = sizeof(g_rotation_maps) / sizeof(g_rotation_maps[0]);
        game->maps = calloc(game->map_count, sizeof(cJSON*));

        if (game->maps) {
            for (size_t i = 0; i < game->map_count; i++) {
                game->maps[i] = g_maps[g_rotation_maps[i]];
            }
        } else {
            game->map_count = 0;
        }
    }

    if (game->mode_count) {
        free(game->modes);
        game->mode_count = 0;
    }

    if (mode_count) {
        game->modes = modes;
        game->mode_count = mode_count;
    } else {
        game->mode_count = sizeof(g_rotation_modes) / sizeof(g_rotation_modes[0]);
        game->modes = calloc(game->mode_count, sizeof(int));

        if (game->modes) memcpy(game->modes, g_rotation_modes, sizeof(g_rotation_modes));
        else game->mode_count = 0;
    }

    if (game->weapon_count) {
        free(game->weapons);
        game->weapon_count = 0;
    }

    if (weapon_count) {
        game->weapons = weapons;
        game->weapon_count = weapon_count;
    } else {
        game->weapon_count = sizeof(g_weapons) / sizeof(g_weapons[0]);
        game->weapons = calloc(game->weapon_count, sizeof(Weapon*));

        if (game->weapons) memcpy(game->weapons, g_weapons, sizeof(g_weapons));
        else game->weapon_count = 0;
    }

    if (game->class_count) {
        free(game->classes);
        game->class_count = 0;
    }

    if (class_count) {
        game->classes = classes;
        game->class_count = class_count;
    } else {
        game->class_count = sizeof(g_classes) / sizeof(g_classes[0]);
        game->classes = calloc(game->class_count, sizeof(ClassConfig));

        if (game->classes) memcpy(game->classes, g_classes, sizeof(g_classes));
        else game->class_count = 0;
    }
}

void game_init(Game *game, const int map_index, const int mode_index, const unsigned char is_local) {
    game->ready = 0;
    game->is_local = is_local;

    game_clear_players(game);

    if (!game->map_count || !game->mode_count) return;

    const int map_to_load = map_index >= 0 && map_index < game->map_count ? map_index : (int) pcg32_boundedrand(game->map_count);
    const int map_changed = !game->map || game->map->raw_data != game->maps[map_to_load];

    const int mode_to_load = mode_index >= 0 && mode_index < game->mode_count ? mode_index : game->modes[(int) pcg32_boundedrand(game->mode_count)];

    if (map_changed) {
        Map *new_map = map_init(game->maps[map_to_load]);

        if (game->map) map_fini(game->map);
        game->map = new_map;
    } else {
        map_reset(game->map);
    }

    GameMode *new_mode = mode_init(game->modes[mode_to_load]);

    if (game->mode) mode_fini(game->mode);
    game->mode = new_mode;

    if (game->vote_kick) {
        free(game->vote_kick);
        game->vote_kick = NULL;
    }

    if (!game->mode || !game->map) return;

    game->ready = 1;
}

void game_tick(Game *game, const float now, const float delta) {
    if (!game->ready) return;

    // TODO: lots of stuff xD

    if (game->is_local || game->server) {
        if (!game->end_timer && game->nuke_timer) {
            game->nuke_timer = MAX(0.0f, game->nuke_timer - delta);

            if (!game->nuke_timer) {
                int nuke_reward = 0;

                for (size_t i = 0; i < game->player_count; i++) {
                    Player *player = game->players[i];
                    if (!player->active || player == game->nuke_player || (player->team && game->nuke_player->team == player->team) || player->god_mode) continue;

                    PlayerKillInfo kill_info = {
                        .streak = 0,
                        .weapon_id = -1,
                    };

                    nuke_reward += 50;
                    player_kill(player, game->nuke_player, &kill_info, 1);
                }

                if (nuke_reward) {
                    // TODO: score
                }
            }
        }

        if (game->vote_kick) {
            game->vote_kick->timer = MAX(0.0f, game->vote_kick->timer - delta);

            if (!game->vote_kick->timer) {
                free(game->vote_kick);
                game->vote_kick = NULL;

                // TODO: send vote kick fail
            }
        }
    }

    for (size_t i = 0; i < game->player_count; i++) {
        Player *player = game->players[i];
        player_update(player, delta * game->config.delta_mlt);

        if (player->position.y <= game->map->death_y) {
            // TODO: reset flag
            player_kill(player, NULL, NULL, 0);
        }
    }
}

void game_players_add(Game *game, Player *player) {
    if (!game->player_count) {
        game->players = calloc(1, sizeof(Player *));
        if (!game->players) return;
    } else {
        Player **new_players = realloc(game->players, (game->player_count + 1) * sizeof(Player *));
        if (!new_players) return;

        game->players = new_players;
    }

    game->players[game->player_count++] = player;
}