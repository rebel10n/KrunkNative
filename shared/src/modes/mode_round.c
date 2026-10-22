// Round flow and player hooks of the modes (Krunker v7.1.4 game.js finishInit :3464-3700,
// update :4743-4870 and endGame :4407-4560 with the rounds / best-of / overtime flow; players.js
// setTeam / add / score; doc/v7/map-logic.md §4.1 "Engine flow changes").
#include "mode_internal.h"
#include <ruleset.h>   // game_is_ranked: a ranked match is one round
#include <ai.h>
#include <forfeit.h>
#include <pcg_basic.h>
#include <limits.h>
#include <math.h>
#include <string.h>

// UTILS.getTime(ms, showMS) -> "mm:ss"
static void mode_format_time(const float seconds, char *out, const size_t size) {
    const int total = seconds > 0.0f ? (int) seconds : 0;
    snprintf(out, size, "%02d:%02d", (total / 60) % 60, total % 60);
}

// ------------------------------------------------------------------ WP-E config (GAME_CONFIG_V7)

static int cfg_game_rounds(const Game *game) {
#ifdef GAME_CONFIG_V7
    return game->config.game_rounds > 0 ? game->config.game_rounds : 1;
#else
    (void) game;
    return 1; // serverConfig gamRounds default
#endif
}

static int cfg_best_of(const Game *game) {
#ifdef GAME_CONFIG_V7
    return !!game->config.best_of;
#else
    (void) game;
    return 0;
#endif
}

static int cfg_no_draws(const Game *game) {
#ifdef GAME_CONFIG_V7
    return !!game->config.no_draws;
#else
    (void) game;
    return 0;
#endif
}

static float cfg_intermission(const Game *game) {
#ifdef GAME_CONFIG_V7
    if (game->config.intermission_time > 0.0f) return game->config.intermission_time;
#else
    (void) game;
#endif
    return MODE_INTERMISSION; // (config.intermTmr || 10)
}

static int cfg_keep_team_score(const Game *game) {
#ifdef GAME_CONFIG_V7
    return !!game->config.keep_team_score;
#else
    (void) game;
    return 0;
#endif
}

static int cfg_comp(const Game *game) {
#ifdef GAME_CONFIG_V7
    return !!game->config.comp;
#else
    (void) game;
    return 0;
#endif
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
    memset(&mode->bomb, 0, sizeof(mode->bomb));
    mode->wait_timer_count = 0;
    mode->wait_second_sent = -1;
    mode->waiting_sent = 0;
    mode->message_sent = 0;
    mode->skip = 0;
    mode->started = 0;
    mode->active_objective = -1;
    mode->objective_timer = 0.0f;
    mode->objective_time = 0.0f;
    mode->round_clock = 0.0f;
    mode->not_winnable = 0;
    mode->in_overtime = 0;
    mode->flipped_spawns = 0;
    mode->lockout_timer = 0.0f;
    mode->has_round_wins = 0;
    memset(mode->team_round_wins, 0, sizeof(mode->team_round_wins));
    game->move_lock = 0;

    // finishInit resets: flags, spawnables, dom zones (the objects come from the map)
    mode_load_objects(game);
    mode_reset_flags(game);
    spawnables_reset(game);
    mode_reset_dom_zones(game);

    // forceSpawn = config.forceSpawn || mode.forceSpawn; minPlayers = config || mode, 2 with lives
    mode->force_spawn = game->config.auto_respawn > 0 ? (float) game->config.auto_respawn : config->force_spawn;
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
    mode->started_timer = game->round_timer; // startedTimer: every round of the match restarts here

    // rounds: roundCount = gamRounds - 1, bestOf = bstOfR, needWinner = noDraws
    mode->game_rounds = cfg_game_rounds(game);
    mode->round_count = mode->game_rounds - 1;
    mode->best_of = (unsigned char) cfg_best_of(game);
    mode->need_winner = (unsigned char) cfg_no_draws(game);

    // waitTimers: the mode's, else the 12 s "match starts in " of lives modes (no warmup)
    for (int i = 0; i < config->wait_timer_count && i < MODE_MAX_WAIT_TIMERS; i++) mode->wait_timers[mode->wait_timer_count++] = config->wait_timers[i];

    if (!config->wait_timer_count && mode->lives && game->config.warmup_time <= 0.0f) {
        const ModeWaitTimer start = {.time = 12.0f, .msg = "match starts in ", .trigger = mode_trigger_lives_start};
        mode_push_wait_timer(game, &start, 0);
    }

    // warmupTime: "warmup ends " then a 5 s "match starting in " in front (comp: game.isComp)
    if (game->config.warmup_time > 0.0f && mode->wait_timer_count + 2 <= MODE_MAX_WAIT_TIMERS) {
        const int comp = cfg_comp(game);
        memmove(&mode->wait_timers[2], &mode->wait_timers[0], (size_t) mode->wait_timer_count * sizeof(ModeWaitTimer));
        mode->wait_timers[0] = (ModeWaitTimer) {.time = game->config.warmup_time * 60.0f, .msg = "warmup ends ", .trigger = mode_trigger_warmup_end, .comp = (unsigned char) comp};
        mode->wait_timers[1] = (ModeWaitTimer) {.time = 5.0f, .msg = "match starting in ", .trigger = mode_trigger_unlock_move, .comp = (unsigned char) comp};
        mode->wait_timer_count += 2;
    }

    // conditions: the mode's, else the lives defaults (FFA / multiTeam / two teams); scoreLimit
    mode->condition_count = 0;
    if (config->condition) {
        mode->conditions[mode->condition_count++] = config->condition;
    } else if (mode->lives) {
        mode->conditions[mode->condition_count++] = config->team_options[0] || !config->teams ? mode_cond_lives_ffa
                                                  : config->multi_team ? mode_cond_lives_multi : mode_cond_lives_teams;
    }

    mode->score_limit = game->config.score_limit ? game->config.score_limit : config->score_limit;
    if (mode->score_limit) {
        mode->conditions[mode->condition_count++] = config->team_options[0] || !config->teams ? mode_cond_score_ffa : mode_cond_score_teams;
    }

    if (game_is_authority(game) && config->game_start) config->game_start(game);
    mode->started = 1; // onGameStart fires next, from game_start_round (WP-C)
}

// ------------------------------------------------------------------ update: waitTimers

int mode_waiting(const Game *game) {
    const GameMode *mode = game->mode;
    return mode && mode->wait_timer_count && !mode->wait_timers[0].cont_time;
}

// A timer that is not contTime counts down before the match (warmup ends, match starting in, the
// lives' 12 s start). Objective rotations (contTime) run through the round and never end the wait.
int mode_match_starting(const Game *game) {
    const GameMode *mode = game->mode;
    if (!mode) return 0;
    for (int i = 0; i < mode->wait_timer_count; i++) {
        if (!mode->wait_timers[i].cont_time) return 1;
    }
    return 0;
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

    if (!timer->dont_wait && !timer->cont_time && mode->min_players && mode_active_count(game) < mode->min_players) {
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
            if (!mode_match_starting(game)) game_set_state(game, GAME_STATE_IN_PROGRESS); // v10: the wait timers are done

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

// update(): lockoutTimer ("lkt", the clock and conditions wait), else waitTimers, then (clock
// running) the conditions. Also counts the timed modes' clock (game.c owns the countdown and
// the "t" packets).
int mode_tick_round(Game *game, const float delta) {
    GameMode *mode = game->mode;
    if (!mode) return MODE_ROUND_RUN;

    if (mode->lockout_timer > 0.0f) {
        const int before = (int) mode->lockout_timer;
        mode->lockout_timer -= delta;
        if (mode->lockout_timer <= 0.0f) mode->lockout_timer = 0.0f;
        if (!mode->lockout_timer || (int) mode->lockout_timer != before) mode_send_lockout(game);
        return MODE_ROUND_HOLD;
    }

    if (!mode_tick_wait(game, delta)) return mode->skip ? MODE_ROUND_END : MODE_ROUND_HOLD;
    if (mode->skip) return MODE_ROUND_END;

    for (int i = 0; i < mode->condition_count; i++) {
        if (!mode->conditions[i](game)) return MODE_ROUND_END;
    }

    if (mode->skip) return MODE_ROUND_END;
    mode->round_clock += delta;
    return MODE_ROUND_RUN;
}

// The parts of game.update / players.update the modes own, in v7 order: player collisions
// (lastGround, onZone + collisionCheck, flags), conversions, objective time, flags (#18),
// dom zones (#20), mode.update (#23), then the forceSpawn respawns. game.c runs the
// spawnables (#19) just before this.
void mode_tick(Game *game, const float delta) {
    GameMode *mode = game->mode;
    if (!mode) return;

    for (size_t i = 0; i < game->player_count; i++) {
        Player *player = game->players[i];
        if (!player || !player->active) continue;
        if (player->on_ground) { // players.setOnGround: lastGround
            player->mode.last_ground = player->position;
            player->mode.has_last_ground = 1;
        }
        player->mode.on_zone = -1; // player.js: onZone = null, then mode.collisionCheck
        if (mode->config.collision_check) mode->config.collision_check(game, player);
    }

    mode_tick_conversions(game);
    if (!mode->wait_timer_count || mode->wait_timers[0].cont_time) { // updateObjectiveTime
        mode->objective_time -= delta;
        if (mode->objective_time < 0.0f) mode->objective_time = 0.0f;
    }
    mode_tick_flags(game, delta);
    mode_tick_dom(game, delta);
    if (mode->config.update) mode->config.update(game, delta);

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

// ------------------------------------------------------------------ endGame rounds

// game.getWinTeam(useScores): the best of teamRoundWins (when kept) or teamScores; ties go to
// the later team, a draw is 0 when nobody scored, and with needWinner when the top is tied.
static int mode_win_team(const Game *game, const int use_scores) {
    const GameMode *mode = game->mode;
    if (!mode->config.teams) return 0;
    const int *scores = !use_scores && mode->has_round_wins ? mode->team_round_wins : mode->team_scores;
    int best = 0, team = 0, unique = 0;
    for (int t = 1; t < MODE_TEAMS; t++) {
        if (scores[t] >= best) {
            unique = scores[t] > best;
            best = scores[t];
            team = t;
        }
    }
    if (best == 0 || (!unique && mode->need_winner)) return 0;
    return team;
}

static void mode_intermission_end(Game *game, const float overflow) {
    (void) overflow;
    mode_unlock_move(game);
}

// endGame() without an endIndex. bestOf: the round winner by team score gets a round win (a
// noDraws tie plays 60 s of overtime instead); the match is won at gamRounds / 2 round wins.
// Rounds left (and not won): everyone is taken out, "rds", spawns flip, objective modes start
// over, and an intermission of intermTmr seconds (movement locked) leads into the next round.
// Then (no winner + noDraws) overtime. Returns 1 when the match continues.
int mode_end_game(Game *game) {
    GameMode *mode = game->mode;
    if (!mode || !game_is_authority(game)) return 0;
    // a ranked match is one round: v10's round logic (round wins, overtime, the next round) is for pub-type
    // games only (v10:191041 `isPubType()`), and a ranked match is hosted, so the round ends the match
    if (game_is_ranked(game)) return 0;

    int match_won = 0;
    if (mode->best_of) {
        const int round_winner = mode_win_team(game, 1);
        memset(mode->team_scores, 0, sizeof(mode->team_scores));
        for (int t = 1; t < MODE_TEAMS; t++) mode_send_team_score(game, t); // "ts" (cleared)
        game->leaders_dirty = 1;
        mode->in_overtime = 0;
        mode->not_winnable = 0;
        if (round_winner) {
            mode->has_round_wins = 1;
            mode->team_round_wins[round_winner]++;
            // v7.1.4 throws on teamRoundWins[0] when a round ends tied without noDraws; KN counts
            // a tied round as not won.
            match_won = mode->team_round_wins[round_winner] * 2 >= mode->game_rounds;
        } else if (mode->need_winner) {
            mode->in_overtime = 1;
            mode_send_text_popup(game, "Overtime", "+60 Seconds");
            game->round_timer = MODE_OVERTIME;
            mode->skip = 0;
            return 1;
        }
    }

    if (mode->round_count > 0 && !match_won) {
        game->move_lock = 1; // moveLock (clients take it from the "rds" intermission timer)
        for (size_t i = 0; i < game->player_count; i++) {
            Player *player = game->players[i];
            if (!player) continue;
            player->active = 0; // active = false, deathInfo = null: everyone respawns for the round
            player->input_queue_size = 0;
            player->mode.flag = 0;
            player->mode.respawn_timer = 0.001f; // KN: the server respawns them (v7 clients send "ent")
        }
        const int lead = mode_win_team(game, 0);
        mode->round_count--;
        mode_send_rounds(game, lead);
        mode->flipped_spawns = !mode->flipped_spawns;
        mode_reset_flags(game);
        for (int f = 0; f < mode->flag_count; f++) mode_send_flag(game, f);

        if (mode->config.objective && !mode->config.dom) {
            mode->active_objective = -1;
            mode->objective_time = 0.0f;
            mode_next_objective(game, 0.0f, 1, 0, -1); // nextObjective(null)
        }

        const ModeWaitTimer intermission = {
            .time = cfg_intermission(game),
            .msg = "intermission ",
            .trigger = mode_intermission_end,
            .comp = 1,
            .dont_wait = 1,
        };
        if (mode->wait_timer_count >= MODE_MAX_WAIT_TIMERS) mode->wait_timer_count = MODE_MAX_WAIT_TIMERS - 1;
        memmove(&mode->wait_timers[1], &mode->wait_timers[0], (size_t) mode->wait_timer_count * sizeof(ModeWaitTimer));
        mode->wait_timers[0] = intermission;
        mode->wait_timer_count++;
        mode->wait_second_sent = -1;
        mode_send_lock(game, NET_TARGET_ALL, NET_LOCK_MOVE, 1);

        game->round_timer = mode->started_timer; // gameTimer = startedTimer
        mode->skip = 0;
        return 1;
    }

    // endGame(): needWinner and no winner -> "Overtime", +60 s
    if (mode->need_winner) {
        const ModeWinner winner = mode_pick_winner(game);
        if (!winner.team && !winner.player) {
            mode_send_text_popup(game, "Overtime", "+60 Seconds");
            game->round_timer = MODE_OVERTIME;
            mode->skip = 0;
            return 1;
        }
    }
    return 0;
}

// onPreEnd + onEnd (game_end_round, after the winner is known and before the end packets)
void mode_on_game_end(Game *game) {
    GameMode *mode = game->mode;
    if (!mode || !game_is_authority(game)) return;
    if (mode->config.on_pre_end) mode->config.on_pre_end(game);
    if (mode->config.on_end) mode->config.on_end(game);
}

// ------------------------------------------------------------------ endGame

static int mode_order_by_score(const Player *a, const Player *b) {
    return b->score - a->score;
}

// players.list.sort(endSort || orderByScore); stable like V8's TimSort.
void mode_sort_players(Game *game, Player **players, const size_t count) {
    const ModeSortFn sort = game->mode && game->mode->config.end_sort ? game->mode->config.end_sort : mode_order_by_score;

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

// endGame() winner: winCondition, else (teams) getWinTeam(), else the first player after sorting.
ModeWinner mode_pick_winner(Game *game) {
    GameMode *mode = game->mode;
    ModeWinner winner = {0, NULL};
    if (!mode) return winner;

    // v10 prepareForfeit: a team forfeit gave the round to the other team (forfeit.c)
    if (forfeit_forced_team(game)) {
        winner.team = forfeit_forced_team(game);
        return winner;
    }

    if (mode->config.win_condition) return mode->config.win_condition(game);

    if (mode->config.teams) {
        winner.team = mode_win_team(game, 0);
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

// game.giveTeamScore (the "ts" goes out at once; v7 batches teamScoreUpd per update)
void mode_team_score_add(Game *game, const int team, const int amount) {
    if (team <= 0 || team >= MODE_TEAMS) return;
    game->mode->team_scores[team] += amount;
    mode_send_team_score(game, team);
}

// ------------------------------------------------------------------ player hooks

// players.setTeam (not from a queue). `current`: the player's team before (player->team);
// `player` may be NULL (game_spawn_point_free asks for a player not added yet).
static int mode_pick_team_of(const Game *game, const Player *player, const int current) {
    const GameModeConfig *config = &game->mode->config;

    if (!game->mode->wait_timer_count && config->conv_team) return config->conv_team;
    if (config->team_options[0] && !config->team_sort) return 1;
    if (config->friendly) return 1;
    if (current || !config->teams) return current;

    if (config->multi_team) { // sdm: the smallest team (ties keep the lowest team after team 2's start)
        int counts[MODE_TEAMS] = {0};
        const int teams = config->multi_team < MODE_TEAMS ? config->multi_team : MODE_TEAMS - 1;
        for (size_t i = 0; i < game->player_count; i++) {
            const Player *other = game->players[i];
            if (other && other != player && other->team > 0 && other->team <= teams) counts[other->team]++;
        }
        int team = 2; // v7: vLN229 = 2, then any team with fewer players
        for (int t = 1; t <= teams; t++) {
            if (counts[t] < counts[team]) team = t;
        }
        return team;
    }

    int team1 = 0, team2 = 0;
    for (size_t i = 0; i < game->player_count; i++) {
        const Player *other = game->players[i];
        if (!other || other == player) continue;
        if (other->team == 1) team1++;
        if (other->team == 2) team2++;
    }

    return team1 >= team2 ? 2 : 1;
}

static int mode_pick_team(const Game *game, const Player *player) {
    return mode_pick_team_of(game, player, player->team);
}

// players.add: setTeam and Player.lives (only while the waitTimers still run).
void mode_player_added(Game *game, Player *player) {
    if (!game->mode || !game_is_authority(game)) return;

    memset(&player->mode, 0, sizeof(player->mode));
    player->mode.on_zone = -1;
    player->team = mode_pick_team(game, player);
    player->mode.lives = game->mode->wait_timer_count && game->mode->lives ? game->mode->lives : 0;
    player->mode.weapon_tier = 0;
    // zom.js starterScore (didStarterScore)
    if (game->mode->config.starter_score) player->score = game->mode->config.starter_score;
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

// ------------------------------------------------------------------ getSpawnPoint

// game.js getPointSpread(x, z, y, spread, dir): a random direction, cut short by the first
// object the `spread`-unit ray (1 above the feet) enters, then 10 units back from it, rounded
// to whole units. Objects the ray starts inside do not count (line_in_rect only reports
// entries ahead); KN also skips non-colliding objects.
static vec3 spawn_point_spread(const Game *game, const Spawn *spawn, const float radius) {
    const float angle = (float) M_PI * (2.0f * (float) ldexp((double) pcg32_random(), -32) - 1.0f) + (float) M_PI;
    const vec3 origin = {spawn->position.x, spawn->position.y + 1.0f, spawn->position.z};
    const vec3 ray = {radius * sinf(angle), 1.0f, radius * cosf(angle)};
    float reach = 1.0f;
    for (size_t i = 0; i < game->map->object_count; i++) {
        const Object *object = game->map->objects[i];
        if (!object->active || object->collision_type == COLLISION_TYPE_NONE) continue;
        const float t = line_in_rect(origin, ray, object->position, object->scale);
        if (t > 0.0f && t <= reach) reach = t;
    }
    const float distance = (radius - 10.0f) * reach;
    return (vec3) {roundf(spawn->position.x + distance * sinf(angle)), spawn->position.y, roundf(spawn->position.z + distance * cosf(angle))};
}

// The players getSpawnPoint weighs a spawn against: active, not the spawning player, and
// enemies only when teamed (`!team || player.team != team`).
static int spawn_counts_player(const Player *player, const Player *other, const int team) {
    return other && other->active && other != player && (!team || other->team != team);
}

// KrunkNative (not in v7.1.4): an active player of any team (bots included, and players
// placed earlier in the same tick) stands on the spawn, i.e. the two bodies would overlap.
// v7.1.4 never checks this, so teammates respawning together all got the same point.
static int spawn_occupied(const Game *game, const Player *player, const Spawn *spawn) {
    const float radius = 2.0f * game_constants.player_scale;
    for (size_t i = 0; i < game->player_count; i++) {
        const Player *other = game->players[i];
        if (!other || !other->active || other == player) continue;
        const float dx = other->position.x - spawn->position.x;
        const float dz = other->position.z - spawn->position.z;
        if (dx * dx + dz * dz < radius * radius && fabsf(other->position.y - spawn->position.y) < game_constants.player_height) return 1;
    }
    return 0;
}

// Candidate tiers: 0 = the spawns getSpawnPoint may return (no comp ones unless useCompSpawns,
// no teamOnly ones of another team; your (flipped) team's or teamless) that nobody stands on,
// 1 = the same occupied or not, 2 = any spawn (v7's `vA670[0] || this.map.spawns[0]` fallback).
static int spawn_candidate(const Game *game, const Player *player, const Spawn *spawn, const int spawn_team, const int use_comp, const int tier) {
    if (tier >= 2) return 1;
    if (spawn->comp && !use_comp) return 0;
    if (spawn->team_only && spawn->team != spawn_team) return 0;
    if (spawn->team && spawn->team != spawn_team) return 0;
    return tier == 1 || !spawn_occupied(game, player, spawn);
}

static float spawn_distance(const Spawn *spawn, const vec3 to) {
    const float dx = spawn->position.x - to.x;
    const float dy = spawn->position.y - to.y;
    const float dz = spawn->position.z - to.z;
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

// Krunker v7.1.4 game.js getSpawnPoint(team, player, false, false) (map-logic.md §2.5; the
// respawn, the server's addPlayer, the Respawn triggers):
// - flippedSpawns (a round swap) trades teams 1 and 2 for the spawn's team tests;
// - useCompSpawns (waitTimers[0].comp: warmup, Defuse's "match starting in", the intermission):
//   your team's comp spawn (any comp spawn in FFA), at the spawn itself: getPointSpread(.., 10) (v10.0.3
//   deob line 217148: spread 10 less 10 is no offset; ruleset comp_spawn_spread);
// - otherwise the spawns seen by the fewest counted players (canSee to y + playerHeight),
//   then of those the one with the largest summed distance to them, plus (flag modes) to the
//   flags not of your team and not neutral, plus (Domination) to the zones captured by another
//   team. Counted players: active, not you, enemies when teamed. teamOnly spawns of another
//   team are never picked; a spawn with `spread` returns getPointSpread(.., spread + 7).
// KrunkNative differences: spawns someone already stands on are skipped while a free one
// is left (spawn_occupied), the minimum sight count is taken over the candidates only (v7
// takes it over every spawn and then falls back to the sorted spawns[0], which can be the
// enemy base), ties are broken at random, a team without a comp spawn gets the regular pick,
// and sight is only checked on the authority (clients take the server's SPAWN position).
// canSee here also lets transparent objects block (game_ai_player_can_see).
int game_get_spawn_point(Game *game, const Player *player, vec3 *position, int *direction) {
    const Map *map = game->map;
    if (!map || !map->spawn_count) return 0;

    const GameMode *mode = game->mode;
    const int team = player ? player->team : 0;
    const int spawn_team = mode && mode->flipped_spawns && (team == 1 || team == 2) ? 3 - team : team;
    const int use_comp = mode && mode->wait_timer_count && mode->wait_timers[0].comp;

    if (use_comp) {
        for (size_t s = 0; s < map->spawn_count; s++) {
            const Spawn *spawn = map->spawns[s];
            if (!spawn->comp || (mode->config.teams && spawn->team != spawn_team)) continue;
            if (spawn->team_only && spawn->team != spawn_team) continue;
            *position = spawn_point_spread(game, spawn, game_ruleset(game)->comp_spawn_spread);
            *direction = spawn->direction;
            return 1;
        }
    }

    const int check_sight = game_is_authority(game);
    int *sight = calloc(map->spawn_count, sizeof(int));
    const Spawn *best = NULL;

    for (int tier = 0; tier < 3 && !best; tier++) {
        int min_sight = INT_MAX;
        for (size_t s = 0; s < map->spawn_count; s++) {
            const Spawn *spawn = map->spawns[s];
            if (!spawn_candidate(game, player, spawn, spawn_team, use_comp, tier)) continue;
            int seen = 0;
            if (check_sight) {
                const vec3 target = {spawn->position.x, spawn->position.y + game_constants.player_height, spawn->position.z};
                for (size_t i = 0; i < game->player_count; i++) {
                    const Player *other = game->players[i];
                    if (spawn_counts_player(player, other, team) && game_ai_player_can_see(game, other, target)) seen++;
                }
            }
            if (sight) sight[s] = seen;
            if (seen < min_sight) min_sight = seen;
        }
        if (min_sight == INT_MAX) continue; // no candidate in this tier

        float best_distance = -1.0f;
        int ties = 0;
        for (size_t s = 0; s < map->spawn_count; s++) {
            const Spawn *spawn = map->spawns[s];
            if (!spawn_candidate(game, player, spawn, spawn_team, use_comp, tier) || (sight && sight[s] != min_sight)) continue;

            float distance = 0.0f;
            for (size_t i = 0; i < game->player_count; i++) {
                const Player *other = game->players[i];
                if (spawn_counts_player(player, other, team)) distance += spawn_distance(spawn, other->position);
            }

            if (mode && (mode->config.flags || mode->config.oneflag)) {
                for (int f = 0; f < mode->flag_count; f++) {
                    const ModeFlag *flag = &mode->flags[f];
                    if (!flag->neutral && flag->team != team) distance += spawn_distance(spawn, flag->position);
                }
            }

            if (mode && mode->config.dom) {
                for (int z = 0; z < mode->dom_zone_count; z++) {
                    const ModeDomZone *zone = &mode->dom_zones[z];
                    if (zone->captured_by && zone->captured_by != team) distance += spawn_distance(spawn, zone->position);
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
    }

    free(sight);
    if (!best) return 0;
    *position = best->spread > 0.0f ? spawn_point_spread(game, best, best->spread + 7.0f) : best->position;
    *direction = best->direction;
    return 1;
}

// KrunkNative (the bot fill, shared/src/bots.c bot_quota_tick): would a player added now, on the
// team players.add gives a teamless newcomer, get a spawn nobody stands on (tier 0 above)? Comp
// spawns spread every player, a map without spawns has nothing to wait for (both 1); with no spawn
// of that team at all (tier 2's fallback) any unoccupied spawn counts.
int game_spawn_point_free(const Game *game) {
    const Map *map = game->map;
    if (!map || !map->spawn_count) return 1;
    const GameMode *mode = game->mode;
    const int team = mode && game_is_authority(game) ? mode_pick_team_of(game, NULL, 0) : 0;
    const int spawn_team = mode && mode->flipped_spawns && (team == 1 || team == 2) ? 3 - team : team;
    const int use_comp = mode && mode->wait_timer_count && mode->wait_timers[0].comp;
    if (use_comp) return 1;
    int eligible = 0;
    for (size_t s = 0; s < map->spawn_count; s++) {
        if (spawn_candidate(game, NULL, map->spawns[s], spawn_team, use_comp, 0)) return 1;
        eligible += spawn_candidate(game, NULL, map->spawns[s], spawn_team, use_comp, 1);
    }
    if (eligible) return 0;
    for (size_t s = 0; s < map->spawn_count; s++) {
        if (!spawn_occupied(game, NULL, map->spawns[s])) return 1;
    }
    return 0;
}

// After player_spawn built the class loadout (and picked the spawn point, game_get_spawn_point):
// the mode's loadout (Gun Game tier, Sharp Shooter weapon, Stalker knife, startingLoadout) and
// Player.init's per-spawn mode fields.
void mode_player_spawned(Game *game, Player *player) {
    if (!game->mode) return;

    int weapons[NET_LOADOUT_MAX];
    int count = mode_gun_loadout(game, player, weapons);
    if (!count && mode_no_melee(game)) { // setClass: no knife slot with noMelee (Classic FFA)
        for (int i = 0; i < player_loadout_base(player) && count < NET_LOADOUT_MAX; i++) {
            if (player->loadout[i] != WEAPON_ID_KNIFE) weapons[count++] = player->loadout[i];
        }
    }
    if (count) mode_set_player_loadout(player, weapons, count, 0);
    else if (mode_no_knives(game)) player->can_throw = 0; // Player.init: canThrow = !noKnives
    player->mode.is_kranked = 0; // Player.init (both sides: the kranked speed is predicted)
    player->mode.kranked_timer = 0.0f;

    if (!game_is_authority(game)) return;

    player->mode.flag = 0;
    player->mode.respawn_timer = 0.0f;
    player->mode.dog_tags = 0;          // Player.init
    player->mode.on_zone = -1;
    player->mode.has_last_ground = 0;
    mode_apply_outfit(game, player); // the SPAWN that follows carries the outfit
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
    victim->mode.is_kranked = 0;
    victim->mode.kranked_timer = 0.0f;

    if (mode->config.on_kill) mode->config.on_kill(game, victim, killer, weapon_id);
}

// players.score: noScoreK kill rewards add nothing to the player's score.
int mode_score_gain(const Game *game, const int amount, const int is_kill) {
    return is_kill == 1 && game->mode && game->mode->config.no_score_k ? 0 : amount;
}

// players.score multipliers of a kill reward (type 1): x (killMlt + player.killMlt) (king,
// depoffa), and in oneflag modes x carrierMlt for the carrier, teamCarrierMlt for its team,
// nonCarrierMlt for everyone else (Math.ceil each). zom pointsPerHit is WP-F / WP-H's.
int mode_score_scale(const Game *game, const Player *player, int amount, const int is_kill) {
    if (!game || !game->mode || !player || is_kill != 1) return amount;
    const GameModeConfig *config = &game->mode->config;
    if (config->kill_mlt > 0.0f) amount = (int) ceilf((float) amount * (config->kill_mlt + player->mode.kill_mlt));
    if (config->oneflag) {
        const int team_has = config->teams && game->mode->flag_team == player->team;
        const int carrier = game->mode->flag_carrier_uid == player->uid;
        const float mlt = carrier ? config->carrier_mlt : team_has ? config->team_carrier_mlt : config->non_carrier_mlt;
        amount = (int) ceilf((float) amount * mlt);
    }
    return amount;
}

// The 13000 public score cap, off with noScrLimit (returns 0 then)
int mode_score_cap(const Game *game) {
    return game && game->mode && game->mode->config.no_scr_limit ? 0 : GAME_SCORE_CAP;
}

// players.score after the 13000 cap (`amount` is 0 when capped): objectiveScore (objective and
// oneflag modes, not kill rewards) and the team score (kill rewards do not count for objective /
// oneflag teams, noScoreC never). keepTScore: only type-2 objective scores feed either.
void mode_on_score(Game *game, Player *player, const int amount, const int is_kill) {
    GameMode *mode = game->mode;
    if (!mode || !game_is_authority(game)) return;

    const GameModeConfig *config = &mode->config;
    const int keep = cfg_keep_team_score(game) && is_kill != 2;
    const int objective = config->objective || config->oneflag;
    if (objective && is_kill != 1 && !keep) player->mode.objective_score += amount;
    if (config->teams && player->team && !keep && !config->no_score_c && (is_kill != 1 || !objective)) {
        mode_team_score_add(game, player->team, amount);
    }
}

// onMelee as a melee damage factor (v10 Stalker; v7.1.4's took the victim's whole health)
float mode_melee_damage(Game *game, Player *dealer, Player *victim, const float damage) {
    if (!game->mode || !dealer || !victim) return damage;
    if (game->mode->config.on_melee) return game->mode->config.on_melee(game, dealer, victim, damage);
    return damage;
}

// v10.0.2 stalk.onMelee (v10:104385-104387, applied as a damage factor at v10:172543-172544): the
// stalker's swing at someone else heals it 10 and deals x0.6; any other knife hit in the mode x0.8.
float mode_melee_stalk(Game *game, Player *dealer, Player *victim, const float damage) {
    if (game && game->mode && game->mode->config.stalker_v7) {
        // v7.1.4 stalk.onMelee (Krunker Strike: vfe.onMelee): a stalker's swing jump-scares the target,
        // heals the stalker 40 and takes the target's whole health (`re = d.obj.health || re`)
        if (!dealer || !victim || dealer->team != 2) return damage;
        if (game_is_authority(game)) {
            const NetJumpScarePacket packet = {.index = 0};
            game_send(game, victim->uid, NET_PACKET_JUMP_SCARE, &packet, sizeof(packet));
        }
        player_change_health(dealer, NULL, -40.0f, 0);
        return victim->health > 0.0f ? victim->health : damage;
    }
    if (dealer && dealer->team == 2 && dealer != victim) {
        player_change_health(dealer, NULL, -10.0f, 0);
        return damage * 0.6f;
    }
    return damage * 0.8f;
}
