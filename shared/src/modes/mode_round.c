// Round flow and player hooks of the modes (Krunker.Unpacked-v3.1.1/106.js finishInit,
// update and endGame; 95.js players.setTeam / add / score / kill / Player.lives).
#include "mode_internal.h"
#include <pcg_basic.h>
#include <math.h>
#include <string.h>

// UTILS.getTime(ms, showMS) -> "mm:ss"
static void mode_format_time(const float seconds, char *out, const size_t size) {
    const int total = seconds > 0.0f ? (int) seconds : 0;
    snprintf(out, size, "%02d:%02d", (total / 60) % 60, total % 60);
}

// ------------------------------------------------------------------ finishInit

// warmupTime's first timer: warmupReset (players re-placed on their next spawn) + lockMove.
static void mode_trigger_warmup_end(Game *game, const float overflow) {
    (void) overflow;
    mode_lock_move(game);
}

void mode_round_start(Game *game) {
    GameMode *mode = game->mode;
    if (!mode) return;

    const GameModeConfig *config = &mode->config;

    memset(mode->team_scores, 0, sizeof(mode->team_scores));
    memset(mode->flag_info, 0, sizeof(mode->flag_info));
    memset(mode->spawnables, 0, sizeof(mode->spawnables));
    memset(&mode->bomb, 0, sizeof(mode->bomb));
    mode->wait_timer_count = 0;
    mode->wait_second_sent = -1;
    mode->waiting_sent = 0;
    mode->message_sent = 0;
    mode->skip = 0;
    mode->started = 0;
    mode->active_objective = -1;
    mode->objective_timer = 0.0f;
    mode->round_clock = 0.0f;
    game->move_lock = 0;

    mode_load_objects(game);
    for (int f = 0; f < mode->flag_count; f++) mode->flags[f].carrier_uid = -1;

    mode->force_spawn = config->force_spawn;
    mode->lives = game->config.lives ? game->config.lives : config->lives;
    mode->min_players = game->config.min_players ? game->config.min_players : config->min_players;
    if (!mode->min_players && mode->lives) mode->min_players = 2;

    // gameTimer: mode.gameTime (0 = no clock) or config.gameTime minutes
    if (config->game_time == MODE_GAME_TIME_CONFIG) {
        const float minutes = (float) game->config.game_time;
        game->round_timer = minutes > 0.0f ? minutes * 60.0f : 240.0f;
    } else {
        game->round_timer = config->game_time;
    }
    if (game->round_length > 0.0f) game->round_timer = game->round_length; // server --round-seconds / CLI `round length`

    // waitTimers: the mode's, else the 12 s "match starts in " of lives modes
    for (int i = 0; i < config->wait_timer_count && i < MODE_MAX_WAIT_TIMERS; i++) mode->wait_timers[mode->wait_timer_count++] = config->wait_timers[i];

    if (!config->wait_timer_count && mode->lives && game->config.warmup_time <= 0.0f) {
        const ModeWaitTimer start = {.time = 12.0f, .msg = "match starts in ", .trigger = mode_trigger_lives_start};
        mode_push_wait_timer(game, &start, 0);
    }

    // warmupTime: "warmup ends " then a comp 10 s "match starting in " in front
    if (game->config.warmup_time > 0.0f && mode->wait_timer_count + 2 <= MODE_MAX_WAIT_TIMERS) {
        memmove(&mode->wait_timers[2], &mode->wait_timers[0], (size_t) mode->wait_timer_count * sizeof(ModeWaitTimer));
        mode->wait_timers[0] = (ModeWaitTimer) {.time = game->config.warmup_time * 60.0f, .msg = "warmup ends ", .trigger = mode_trigger_warmup_end, .comp = 1};
        mode->wait_timers[1] = (ModeWaitTimer) {.time = 10.0f, .msg = "match starting in ", .trigger = mode_trigger_unlock_move, .comp = 1};
        mode->wait_timer_count += 2;
    }

    // conditions: the mode's, else the lives defaults; plus the score limit
    mode->condition_count = 0;
    if (config->condition) {
        mode->conditions[mode->condition_count++] = config->condition;
    } else if (mode->lives) {
        mode->conditions[mode->condition_count++] = config->team_options[0] || !config->teams ? mode_cond_lives_ffa : mode_cond_lives_teams;
    }

    mode->score_limit = game->config.score_limit ? game->config.score_limit : config->score_limit;
    if (mode->score_limit) {
        mode->conditions[mode->condition_count++] = config->team_options[0] || !config->teams ? mode_cond_score_ffa : mode_cond_score_teams;
    }

    if (game_is_authority(game) && config->game_start) config->game_start(game);
    mode->started = 1;
}

// ------------------------------------------------------------------ update: waitTimers

int mode_waiting(const Game *game) {
    const GameMode *mode = game->mode;
    return mode && mode->wait_timer_count && !mode->wait_timers[0].cont_time;
}

static void mode_message(Game *game, const char *text) {
    GameMode *mode = game->mode;
    mode->message_sent = text && text[0];
    mode->waiting_sent = text && !strcmp(text, NET_GAME_MESSAGE_WAITING);
    mode_send_message(game, text ? text : "");
}

// game.update's waitTimers block. Returns 1 when the round clock runs this tick
// (no waitTimers, or the first one keeps the clock running).
static int mode_tick_wait(Game *game, const float delta) {
    GameMode *mode = game->mode;
    if (!mode->wait_timer_count) return 1;

    ModeWaitTimer *timer = &mode->wait_timers[0];

    if (!timer->cont_time && mode->min_players && mode_active_count(game) < mode->min_players) {
        if (!mode->waiting_sent) mode_message(game, NET_GAME_MESSAGE_WAITING);
        return 0;
    }

    timer->time -= delta;

    if (timer->time <= 0.0f) {
        const ModeTriggerFn trigger = timer->trigger;
        const float overflow = -timer->time;
        if (trigger) trigger(game, overflow);

        // the trigger may have replaced the list (nextObjective / nextWeapon)
        if (mode->wait_timer_count && mode->wait_timers[0].time <= 0.0f) {
            const int no_clear = mode->wait_timers[0].no_clear;
            memmove(&mode->wait_timers[0], &mode->wait_timers[1], (size_t) (mode->wait_timer_count - 1) * sizeof(ModeWaitTimer));
            mode->wait_timer_count--;
            mode->wait_second_sent = -1;

            if (!mode->wait_timer_count && !no_clear) mode_message(game, "");
        }
    } else {
        const int second = (int) timer->time;
        if (second != mode->wait_second_sent || mode->waiting_sent) {
            mode->wait_second_sent = second;
            char text[NET_GAME_MESSAGE_SIZE], time[16];
            mode_format_time(timer->time, time, sizeof(time));
            snprintf(text, sizeof(text), "%s%s", timer->msg ? timer->msg : "", time);
            mode_message(game, text);
        }
    }

    return mode->wait_timer_count && mode->wait_timers[0].cont_time;
}

// update(): waitTimers, then (clock running) the conditions. Also counts the timed
// modes' clock (game.c owns the countdown and the "t" packets).
int mode_tick_round(Game *game, const float delta) {
    GameMode *mode = game->mode;
    if (!mode) return MODE_ROUND_RUN;

    if (!mode_tick_wait(game, delta)) return mode->skip ? MODE_ROUND_END : MODE_ROUND_HOLD;
    if (mode->skip) return MODE_ROUND_END;

    for (int i = 0; i < mode->condition_count; i++) {
        if (!mode->conditions[i](game)) return MODE_ROUND_END;
    }

    if (mode->skip) return MODE_ROUND_END;
    mode->round_clock += delta;
    return MODE_ROUND_RUN;
}

void mode_tick(Game *game, const float delta) {
    GameMode *mode = game->mode;
    if (!mode) return;

    mode_tick_conversions(game);
    mode_tick_objectives(game, delta);
    mode_tick_flags(game, delta);
    mode_tick_spawnables(game, delta);

    // deathInfo.tillNext: forceSpawn respawns ("frs")
    for (size_t i = 0; i < game->player_count; i++) {
        Player *player = game->players[i];
        if (!player || player->active || player->mode.respawn_timer <= 0.0f) continue;

        player->mode.respawn_timer -= delta;
        if (player->mode.respawn_timer > 0.0f) continue;

        player->mode.respawn_timer = 0.0f;
        if (game->hooks.respawn && mode_can_spawn(game, player)) {
            game->hooks.respawn(game, player->uid);
            return; // the hook may have reshuffled game->players
        }
    }
}

// ------------------------------------------------------------------ endGame

static int mode_order_by_score(const Player *a, const Player *b) {
    return b->score - a->score;
}

static int mode_order_by_kills(const Player *a, const Player *b) {
    return (int) b->kills - (int) a->kills;
}

// players.list.sort(endSort || orderByScore); stable like V8's TimSort.
void mode_sort_players(Game *game, Player **players, const size_t count) {
    const ModeSortFn sort = game->mode && game->mode->config.end_sort ? game->mode->config.end_sort
                          : game->mode && game->mode->config.kill_sort ? mode_order_by_kills : mode_order_by_score;

    for (size_t i = 1; i < count; i++) {
        Player *player = players[i];
        size_t at = i;
        while (at > 0 && sort(players[at - 1], player) > 0) {
            players[at] = players[at - 1];
            at--;
        }
        players[at] = player;
    }
}

// endGame() winner: winCondition, else the best team score (ties to the later team,
// nobody when every team has 0), else the first player after sorting.
ModeWinner mode_pick_winner(Game *game) {
    GameMode *mode = game->mode;
    ModeWinner winner = {0, NULL};
    if (!mode) return winner;

    if (mode->config.win_condition) return mode->config.win_condition(game);

    if (mode->config.teams) {
        int best = 0, team = 0;
        for (int t = 1; t < MODE_TEAMS; t++) {
            if (mode->team_scores[t] >= best) {
                best = mode->team_scores[t];
                team = t;
            }
        }
        winner.team = best == 0 ? 0 : team;
        return winner;
    }

    Player *sorted[GAME_LEADERBOARD_MAX];
    size_t count = 0;
    for (size_t i = 0; i < game->player_count && count < GAME_LEADERBOARD_MAX; i++) {
        if (game->players[i]) sorted[count++] = game->players[i];
    }
    mode_sort_players(game, sorted, count);
    winner.player = count ? sorted[0] : NULL;
    return winner;
}

int mode_team_score(const Game *game, const int team) {
    return game && game->mode && team >= 0 && team < MODE_TEAMS ? game->mode->team_scores[team] : 0;
}

void mode_team_score_add(Game *game, const int team, const int amount) {
    if (team <= 0 || team >= MODE_TEAMS) return;
    game->mode->team_scores[team] += amount;
    mode_send_team_score(game, team);
}

// ------------------------------------------------------------------ player hooks

// players.setTeam (not from a queue).
static int mode_pick_team(const Game *game, const Player *player) {
    const GameModeConfig *config = &game->mode->config;

    if (!game->mode->wait_timer_count && config->conv_team) return config->conv_team;
    if (config->team_options[0] && !config->team_sort) return 1;
    if (config->friendly) return 1;
    if (player->team || !config->teams) return player->team;

    int team1 = 0, team2 = 0;
    for (size_t i = 0; i < game->player_count; i++) {
        const Player *other = game->players[i];
        if (!other || other == player) continue;
        if (other->team == 1) team1++;
        if (other->team == 2) team2++;
    }

    return team1 >= team2 ? 2 : 1;
}

// players.add: setTeam and Player.lives (only while the waitTimers still run).
void mode_player_added(Game *game, Player *player) {
    if (!game->mode || !game_is_authority(game)) return;

    memset(&player->mode, 0, sizeof(player->mode));
    player->team = mode_pick_team(game, player);
    player->mode.lives = game->mode->wait_timer_count && game->mode->lives ? game->mode->lives : 0;
    player->mode.weapon_tier = 0;
}

int mode_can_spawn(const Game *game, const Player *player) {
    if (!game->mode || !player) return 1;
    if (!game->mode->lives || game->mode->wait_timer_count) return 1;
    return player->mode.lives > 0;
}

void mode_player_left(Game *game, Player *player) {
    if (!game->mode || !game_is_authority(game)) return;
    if (player->mode.flag) mode_drop_flag(game, player);
}

// Krunker getSpawnPoint(team): the spawns of your team (or teamless ones), furthest
// from the enemies (and in CTF from the enemy flags). Line of sight is not checked.
void mode_pick_spawn(Game *game, Player *player) {
    const Map *map = game->map;
    if (!map || !map->spawn_count || !player->team) return;

    const Spawn *best = NULL;
    float best_distance = -1.0f;
    int ties = 0;

    for (size_t s = 0; s < map->spawn_count; s++) {
        const Spawn *spawn = map->spawns[s];
        if (spawn->comp || (spawn->team && spawn->team != player->team)) continue;

        float distance = 0.0f;
        for (size_t i = 0; i < game->player_count; i++) {
            const Player *other = game->players[i];
            if (!other || !other->active || other == player || other->team == player->team) continue;
            const float dx = spawn->position.x - other->position.x;
            const float dy = spawn->position.y - other->position.y;
            const float dz = spawn->position.z - other->position.z;
            distance += sqrtf(dx * dx + dy * dy + dz * dz);
        }

        if (game->mode->config.flags) {
            for (int f = 0; f < game->mode->flag_count; f++) {
                const ModeFlag *flag = &game->mode->flags[f];
                if (flag->team == player->team) continue;
                const float dx = spawn->position.x - flag->position.x;
                const float dy = spawn->position.y - flag->position.y;
                const float dz = spawn->position.z - flag->position.z;
                distance += sqrtf(dx * dx + dy * dy + dz * dz);
            }
        }

        if (distance > best_distance + 1e-3f) {
            best = spawn;
            best_distance = distance;
            ties = 1;
        } else if (fabsf(distance - best_distance) <= 1e-3f && pcg32_boundedrand((uint32_t) ++ties) == 0) {
            best = spawn;
        }
    }

    if (!best) return;

    player->position = best->position;
    player->direction.x = 0.0f;
    player->direction.y = -(float) M_PI / 2.0f * (1.0f + (float) best->direction);
}

// After player_spawn built the class loadout: the mode's loadout (Gun Game tier,
// Sharp Shooter weapon, Stalker knife) and, on the authority, a team spawn point.
void mode_player_spawned(Game *game, Player *player) {
    if (!game->mode) return;

    int weapons[NET_LOADOUT_MAX];
    if (mode_gun_loadout(game, player, weapons)) mode_set_player_loadout(player, weapons, 1, 0);

    if (!game_is_authority(game)) return;

    player->mode.flag = 0;
    player->mode.respawn_timer = 0.0f;
    mode_apply_outfit(game, player); // the SPAWN that follows carries the outfit
    mode_pick_spawn(game, player);
}

void mode_on_kill(Game *game, Player *victim, Player *killer, const int weapon_id) {
    GameMode *mode = game->mode;
    if (!mode || !game_is_authority(game)) return;

    // lives only count once the match started
    if (victim->mode.lives && !mode->wait_timer_count) {
        victim->mode.lives--;
        mode_send_lives(game, victim);
    }

    if (killer && killer != victim && weapon_id >= 0 && (size_t) weapon_id < game->weapon_count &&
        game->weapons[weapon_id] && game->weapons[weapon_id]->melee && killer->can_throw) {
        killer->mode.melees++;
    }

    victim->mode.respawn_timer = mode->force_spawn > 0.0f ? mode->force_spawn + GAME_DEATH_DELAY : 0.0f;

    if (mode->config.on_kill) mode->config.on_kill(game, victim, killer, weapon_id);
}

// players.score: noScoreK kill rewards add nothing to the player's score.
int mode_score_gain(const Game *game, const int amount, const int is_kill) {
    return is_kill && game->mode && game->mode->config.no_score_k ? 0 : amount;
}

// players.score after the 13000 cap (`amount` is 0 when capped): objectiveScore and
// the team score (kill rewards do not count for objective teams, noScoreC never).
void mode_on_score(Game *game, Player *player, const int amount, const int is_kill) {
    GameMode *mode = game->mode;
    if (!mode || !game_is_authority(game)) return;

    const GameModeConfig *config = &mode->config;
    if (config->objective && !is_kill) player->mode.objective_score += amount;
    if (config->teams && player->team && !config->no_score_c && (!is_kill || !config->objective)) {
        mode_team_score_add(game, player->team, amount);
    }
}

// onMelee (Stalker): the stalker's swing heals it 40 and takes the victim's whole health.
float mode_melee_damage(Game *game, Player *dealer, Player *victim, const float damage) {
    if (!game->mode || !dealer || !victim) return damage;

    const GameModeConfig *config = &game->mode->config;
    if (!config->team_options[1] || strcmp(config->team_options[1], "Stalker") || dealer->team != 2) return damage;

    player_change_health(dealer, NULL, -40.0f, 0);
    return victim->health > 0.0f ? victim->health : damage;
}
