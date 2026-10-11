// The modes.js functions (Krunker.Unpacked-v3.1.1/50.js): round conditions,
// winConditions, endSorts, gameStarts, onKills and the waitTimer triggers that pick
// seekers / bosses / stalkers / traitors (106.js pickSeeker, infectRandom, ...).
#include "mode_internal.h"
#include <ai.h>
#include <string.h>

// ------------------------------------------------------------------ helpers

static int count_active_team(const Game *game, const int team) {
    int count = 0;
    for (size_t i = 0; i < game->player_count; i++) {
        const Player *player = game->players[i];
        if (player && player->active && player->team == team) count++;
    }
    return count;
}

static int count_lives_team(const Game *game, const int team, const int need_active) {
    int count = 0;
    for (size_t i = 0; i < game->player_count; i++) {
        const Player *player = game->players[i];
        if (!player || player->mode.lives <= 0 || (need_active && !player->active)) continue;
        if (team == 1 ? player->team == 1 : player->team != 1) count++;
    }
    return count;
}

static ModeWinner winner_team(const int team) {
    return (ModeWinner) {.team = team, .player = NULL};
}

static ModeWinner winner_player(Player *player) {
    return (ModeWinner) {.team = 0, .player = player};
}

// ------------------------------------------------------------------ conditions

int mode_cond_team1_active(const Game *game) {
    return count_active_team(game, 1) > 0;
}

int mode_cond_team2_active(const Game *game) {
    return count_active_team(game, 2) > 0;
}

// prop / trai: (lives > 0 && active) on team 1 and on "the rest"
int mode_cond_teams_alive(const Game *game) {
    return count_lives_team(game, 1, 1) >= 1 && count_lives_team(game, 2, 1) >= 1;
}

// stalk: lives > 0 on team 1 and on team 2 (exactly)
int mode_cond_stalk(const Game *game) {
    int living = 0, stalkers = 0;
    for (size_t i = 0; i < game->player_count; i++) {
        const Player *player = game->players[i];
        if (!player || player->mode.lives <= 0) continue;
        if (player->team == 1) living++;
        else if (player->team == 2) stalkers++;
    }
    return living >= 1 && stalkers >= 1;
}

// race: whoever reaches targetScore gets their time, then a 10 s "race ends in " timer
// (contTime) starts once.
int mode_cond_race(const Game *game) {
    GameMode *mode = game->mode;
    for (size_t i = 0; i < game->player_count; i++) {
        Player *player = game->players[i];
        if (!player || player->score < mode->target_score || player->mode.finished) continue;

        player->mode.finished = 1;
        player->mode.race_time = mode->round_clock;
        player->mode.has_timer = 1;                 // c.players.list[d].timer = c.lastTimer
        player->mode.timer = mode->round_clock;

        if (!mode->wait_timer_count) {
            const ModeWaitTimer end = {.time = 10.0f, .msg = "race ends in ", .trigger = mode_trigger_skip, .cont_time = 1};
            mode_push_wait_timer((Game *) game, &end, 1);
        }
    }
    return 1;
}

int mode_cond_gun(const Game *game) {
    for (size_t i = 0; i < game->player_count; i++) {
        const Player *player = game->players[i];
        if (player && player->mode.weapon_tier >= game->mode->config.weapon_order_size) return 0;
    }
    return 1;
}

int mode_cond_kills(const Game *game) {
    for (size_t i = 0; i < game->player_count; i++) {
        const Player *player = game->players[i];
        if (player && player->kills >= 10) return 0;
    }
    return 1;
}

int mode_cond_oinc(const Game *game) {
    int alive = 0;
    for (size_t i = 0; i < game->player_count; i++) {
        const Player *player = game->players[i];
        if (player && player->mode.lives > 0) alive++;
    }
    return alive > 1;
}

static void kc_team_confirms(const Game *game, int *team1, int *team2) {
    *team1 = *team2 = 0;
    for (size_t i = 0; i < game->player_count; i++) {
        const Player *player = game->players[i];
        if (!player) continue;
        if (player->team == 1) *team1 += player->mode.confirms;
        else *team2 += player->mode.confirms;
    }
}

int mode_cond_kc(const Game *game) {
    int team1, team2;
    kc_team_confirms(game, &team1, &team2);
    return team1 < game->mode->config.tag_limit && team2 < game->mode->config.tag_limit;
}

int mode_cond_def(const Game *game) {
    int defenders = 0;
    int attackers = game->mode->bomb.active ? 1 : 0;

    for (size_t i = 0; i < game->player_count; i++) {
        const Player *player = game->players[i];
        if (!player) continue;
        if (player->mode.diffuses) return 0;
        if (player->mode.lives <= 0) continue;
        if (player->team == 1) defenders++;
        else attackers++;
    }

    return defenders >= 1 && attackers >= 1;
}

// raid: someone still has a life; once the behavType 2 boss AI (Lord of Souls) is dead
// and no wait timer runs, "raid ends in " 10 s ends the round (gameTimer = "skip").
// Every player's "time" end stat becomes the round clock shown at that moment (lastTimer).
int mode_cond_raid(const Game *game) {
    int alive = 0;
    for (size_t i = 0; i < game->player_count && !alive; i++) {
        const Player *player = game->players[i];
        alive = player && player->mode.lives > 0;
    }
    if (!alive) return 0;

    for (int i = 0; game->ai && i < game->ai->count; i++) {
        const GameAI *ai = &game->ai->ais[i];
        if (!ai->active && ai->behaviour_type == 2 && !game->mode->wait_timer_count) {
            for (size_t p = 0; p < game->player_count; p++) { // players.list[g].timer = c.lastTimer
                Player *player = game->players[p];
                if (!player) continue;
                player->mode.has_timer = 1;
                player->mode.timer = game->round_timer;
            }
            const ModeWaitTimer end = {.time = 10.0f, .msg = "raid ends in ", .trigger = mode_trigger_skip};
            mode_push_wait_timer((Game *) game, &end, 1);
        }
    }
    return 1;
}

// trai onKill: outside the wait timers, "<victim>'s Ghost" (AI kind 0) where they died.
void mode_kill_trai(Game *game, Player *victim, Player *killer, const int weapon_id) {
    (void) killer;
    (void) weapon_id;
    if (game->mode->wait_timer_count) return;
    char name[GAME_AI_NAME_SIZE];
    snprintf(name, sizeof(name), "%s's Ghost", victim->name);
    game_ai_spawn(game, 0, 0, name, victim->position, NULL);
}

int mode_cond_lives_ffa(const Game *game) {
    int alive = 0;
    for (size_t i = 0; i < game->player_count; i++) {
        const Player *player = game->players[i];
        if (player && player->mode.lives > 0) alive++;
    }
    return alive > 1;
}

int mode_cond_lives_teams(const Game *game) {
    return count_lives_team(game, 1, 0) >= 1 && count_lives_team(game, 2, 0) >= 1;
}

int mode_cond_score_ffa(const Game *game) {
    for (size_t i = 0; i < game->player_count; i++) {
        const Player *player = game->players[i];
        if (player && player->score >= game->mode->score_limit) return 0;
    }
    return 1;
}

int mode_cond_score_teams(const Game *game) {
    for (int team = 0; team < MODE_TEAMS; team++) {
        if (game->mode->team_scores[team] && game->mode->team_scores[team] >= game->mode->score_limit) return 0;
    }
    return 1;
}

// ------------------------------------------------------------------ winConditions

ModeWinner mode_win_team1_active(const Game *game) {
    return winner_team(count_active_team(game, 1) > 0 ? 1 : 2);
}

ModeWinner mode_win_team2_active(const Game *game) {
    return winner_team(count_active_team(game, 2) > 0 ? 2 : 1);
}

// lms / trai: the side with more lives left (ties to team 2)
ModeWinner mode_win_lives(const Game *game) {
    int team1 = 0, team2 = 0;
    for (size_t i = 0; i < game->player_count; i++) {
        const Player *player = game->players[i];
        if (!player || player->mode.lives <= 0) continue;
        if (player->team == 1) team1 += player->mode.lives;
        else team2 += player->mode.lives;
    }
    return winner_team(team1 > team2 ? 1 : 2);
}

ModeWinner mode_win_gun(const Game *game) {
    int best = 0;
    Player *winner = NULL;
    for (size_t i = 0; i < game->player_count; i++) {
        Player *player = game->players[i];
        if (!player) continue;
        if (!winner || player->mode.weapon_tier > best) {
            best = player->mode.weapon_tier;
            winner = player;
        }
    }
    return best == 0 ? winner_player(NULL) : winner_player(winner);
}

ModeWinner mode_win_kills(const Game *game) {
    unsigned int best = 0;
    Player *winner = NULL;
    for (size_t i = 0; i < game->player_count; i++) {
        Player *player = game->players[i];
        if (!player) continue;
        if (!winner || player->kills > best) {
            best = player->kills;
            winner = player;
        }
    }
    return best == 0 ? winner_player(NULL) : winner_player(winner);
}

ModeWinner mode_win_first_alive(const Game *game) {
    for (size_t i = 0; i < game->player_count; i++) {
        Player *player = game->players[i];
        if (player && player->mode.lives > 0) return winner_player(player);
    }
    return winner_player(NULL);
}

ModeWinner mode_win_kc(const Game *game) {
    int team1, team2;
    kc_team_confirms(game, &team1, &team2);
    return winner_team(team1 > team2 ? 1 : 2);
}

ModeWinner mode_win_def(const Game *game) {
    int defenders = 0;
    for (size_t i = 0; i < game->player_count; i++) {
        const Player *player = game->players[i];
        if (!player) continue;
        if (player->mode.diffuses) return winner_team(player->team);
        if (player->active && player->team == 1) defenders++;
    }
    return winner_team(defenders > 0 ? 1 : 2);
}

// ------------------------------------------------------------------ endSort (< 0: a first)

int mode_sort_race(const Player *a, const Player *b) {
    if (a->score == b->score && a->mode.finished && b->mode.finished) {
        return a->mode.race_time < b->mode.race_time ? -1 : a->mode.race_time > b->mode.race_time;
    }
    return b->score - a->score;
}

// modes.js: (a.weaponTier === b.weaponTier && a.kills && b.kills) ? a.kills - b.kills : b.tier - a.tier
int mode_sort_gun(const Player *a, const Player *b) {
    if (a->mode.weapon_tier == b->mode.weapon_tier && a->kills && b->kills) return (int) a->kills - (int) b->kills;
    return b->mode.weapon_tier - a->mode.weapon_tier;
}

int mode_sort_oinc(const Player *a, const Player *b) {
    if (a->mode.lives == b->mode.lives && a->kills && b->kills) return (int) b->kills - (int) a->kills;
    return b->mode.lives - a->mode.lives;
}

int mode_sort_kc(const Player *a, const Player *b) {
    if (a->mode.confirms == b->mode.confirms && a->kills && b->kills) return (int) b->kills - (int) a->kills;
    return b->mode.confirms - a->mode.confirms;
}

// ------------------------------------------------------------------ gameStart

void mode_start_objective(Game *game) {
    game->mode->active_objective = -1;
    mode_next_objective(game, 0.0f, 1);
}

void mode_start_lock_objective(Game *game) {
    mode_lock_move(game);
    mode_start_objective(game);
}

void mode_start_lock(Game *game) {
    mode_lock_move(game);
}

// race: lockMove + targetScore = the score zones' points
void mode_start_race(Game *game) {
    mode_lock_move(game);
}

// def: warmupReset (players are re-placed by their next spawn) + lockMove
void mode_start_def(Game *game) {
    mode_lock_move(game);
}

void mode_start_weapon(Game *game) {
    game->mode->active_objective = 0;
    mode_next_weapon(game, 0.0f, 1);
}

// ------------------------------------------------------------------ onKill

void mode_kill_ctf(Game *game, Player *victim, Player *killer, const int weapon_id) {
    (void) killer;
    (void) weapon_id;
    if (!victim->mode.flag) return;

    // A death zone / falling resets the flag (players.update, Player.update kill objects),
    // any other death drops it where the carrier fell.
    if (mode_in_kill_zone(game, victim)) mode_reset_flag(game, victim->mode.flag - 1);
    else mode_drop_flag(game, victim);
}

void mode_kill_gun(Game *game, Player *victim, Player *killer, const int weapon_id) {
    if (killer && killer != victim) {
        const int id = weapon_id >= 0 ? weapon_id : (killer->loadout_size ? killer->loadout[killer->loadout_index] : -1);
        const Weapon *weapon = id >= 0 && (size_t) id < game->weapon_count ? game->weapons[id] : NULL;

        if (weapon && weapon->melee) {
            mode_decrease_weapon(game, victim);
            if (killer->mode.weapon_tier == game->mode->config.weapon_order_size - 1) mode_increase_weapon(game, killer, weapon_id);
        } else {
            mode_increase_weapon(game, killer, weapon_id);
        }
    } else {
        mode_decrease_weapon(game, victim);
    }

    game->leaders_dirty = 1;
}

// oinc: a kill refills one bullet (max 7)
void mode_kill_oinc(Game *game, Player *victim, Player *killer, const int weapon_id) {
    (void) weapon_id;
    if (killer && killer != victim && killer->loadout_size && killer->ammo) {
        killer->ammo[0]++;
        if (killer->ammo[0] > 7) killer->ammo[0] = 7;
        mode_send_loadout(game, killer);
    }
    game->leaders_dirty = 1;
}

void mode_kill_kc(Game *game, Player *victim, Player *killer, const int weapon_id) {
    (void) killer;
    (void) weapon_id;
    mode_drop_spawnable(game, victim);
}

// def: the bomb carrier drops it (bombs / sites are not in KrunkNative's maps; nothing to drop).
void mode_kill_def(Game *game, Player *victim, Player *killer, const int weapon_id) {
    (void) game;
    (void) victim;
    (void) killer;
    (void) weapon_id;
}

// ------------------------------------------------------------------ waitTimer triggers

int mode_pick_active(const Game *game, Player **out, const int max) {
    int count = 0;
    for (size_t i = 0; i < game->player_count && count < max; i++) {
        Player *player = game->players[i];
        if (player && player->active) out[count++] = player;
    }
    return count;
}

#define PICK_MAX 64

// Y = active players; pick `count` of them at random (UTILS.randInt + splice).
static int pick_random(const Game *game, Player **picked, const int count) {
    Player *pool[PICK_MAX];
    int size = mode_pick_active(game, pool, PICK_MAX);
    int out = 0;

    for (int i = 0; i < count && size > 0; i++) {
        const int index = mode_rand_int(0, size - 1);
        picked[out++] = pool[index];
        memmove(&pool[index], &pool[index + 1], (size_t) (size - index - 1) * sizeof(Player *));
        size--;
    }

    return out;
}

void mode_trigger_pick_seeker(Game *game, const float overflow) {
    (void) overflow;
    Player *seeker;
    if (pick_random(game, &seeker, 1)) {
        mode_update_team(game, seeker, 2);
        mode_lock_player(game, seeker, 1);
    }
}

void mode_trigger_release_seeker(Game *game, const float overflow) {
    (void) overflow;
    for (size_t i = 0; i < game->player_count; i++) {
        Player *player = game->players[i];
        if (player && player->team == 2) mode_lock_player(game, player, 0);
    }
}

void mode_trigger_lock_seekers(Game *game, const float overflow) {
    (void) overflow;
    for (size_t i = 0; i < game->player_count; i++) {
        Player *player = game->players[i];
        if (player && player->active && player->team == 2) mode_lock_player(game, player, 1);
    }
}

// infectRandom: 1 infected, 2 from 8 active players, 3 from 14; they die and come back infected.
void mode_trigger_infect_random(Game *game, const float overflow) {
    (void) overflow;
    const int active = mode_active_count(game);
    const int count = active >= 14 ? 3 : active >= 8 ? 2 : 1;
    Player *picked[3];
    const int got = pick_random(game, picked, count);

    for (int i = 0; i < got; i++) {
        mode_update_team(game, picked[i], 2);
        player_kill(picked[i], NULL, NULL, 1);
        mode_send_action(game, picked[i], NULL, "got infected");
    }
}

void mode_trigger_unlock_move(Game *game, const float overflow) {
    (void) overflow;
    mode_unlock_move(game);
}

// pickSimon: the host when playing, else a random active player; 5000 health.
void mode_trigger_pick_simon(Game *game, const float overflow) {
    (void) overflow;
    Player *simon;
    if (pick_random(game, &simon, 1)) {
        mode_update_team(game, simon, 2);
        mode_set_role_health(game, simon, 5000);
        simon->mode.no_regen = 0; // pickSimon keeps regen
    }
}

void mode_trigger_pick_boss(Game *game, const float overflow) {
    (void) overflow;
    Player *boss;
    if (pick_random(game, &boss, 1)) {
        mode_update_team(game, boss, 2);
        mode_set_role_health(game, boss, 10000);
    }
}

// pickStalker: 400 health, no regen, knife only.
void mode_trigger_pick_stalker(Game *game, const float overflow) {
    (void) overflow;
    Player *stalker;
    if (pick_random(game, &stalker, 1)) {
        mode_update_team(game, stalker, 2);
        mode_set_role_health(game, stalker, 400);
        const int knife = WEAPON_ID_KNIFE;
        mode_set_player_loadout(stalker, &knife, 1, 0);
        mode_send_loadout(game, stalker);
    }
}

// pickTraitors: 1 traitor, 2 from 6 active players, 3 from 12.
void mode_trigger_pick_traitors(Game *game, const float overflow) {
    (void) overflow;
    const int active = mode_active_count(game);
    const int count = active >= 12 ? 3 : active >= 6 ? 2 : 1;
    Player *picked[3];
    const int got = pick_random(game, picked, count);
    for (int i = 0; i < got; i++) mode_update_team(game, picked[i], 2);
}

// def "match starting in ": DESTROY / DEFEND THE OBJECTIVE, then unlockMove.
void mode_trigger_def_start(Game *game, const float overflow) {
    (void) overflow;
    for (size_t i = 0; i < game->player_count; i++) {
        const Player *player = game->players[i];
        if (player) mode_send_message_to(game, player->uid, player->team == 2 ? "DESTROY THE OBJECTIVE" : "DEFEND THE OBJECTIVE");
    }
    mode_unlock_move(game);
}

// finishInit's default lives timer: spectators lose their lives (no spectators natively).
void mode_trigger_lives_start(Game *game, const float overflow) {
    (void) game;
    (void) overflow;
}

void mode_trigger_next_objective(Game *game, const float overflow) {
    mode_next_objective(game, overflow, 0);
}

void mode_trigger_next_weapon(Game *game, const float overflow) {
    mode_next_weapon(game, overflow, 0);
}

void mode_trigger_skip(Game *game, const float overflow) {
    (void) overflow;
    game->mode->skip = 1;
}
