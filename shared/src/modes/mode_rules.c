// The mode functions (Krunker v7.1.4 src/modes/<id>.js; v3.1.1 Krunker.Unpacked-v3.1.1/50.js):
// round conditions, winConditions, endSorts / leaderSorts, gameStarts, onKills and the
// waitTimer triggers that pick seekers / bosses / stalkers / traitors (game.js pickSeeker,
// infectRandom, ...). The objective updates live in mode_objectives.c, mode_dom.c and
// mode_deposit.c.
#include "mode_internal.h"
#include <ai.h>
#include <stalker.h> // STALKER_PICKED
#include <map_catalog.h> // raid.onEnd: game.mapIndex (Bastion)
#include <game_config.h> // pickSimon: game.host (game_host_player)
#include <defuse.h>      // def.onKill: the bomb
#include <math.h>
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

int mode_cond_oinc(const Game *game) {
    int alive = 0;
    for (size_t i = 0; i < game->player_count; i++) {
        const Player *player = game->players[i];
        if (player && player->mode.lives > 0) alive++;
    }
    return alive > 1;
}

// kc.js / depo.js condition: !(teamScores[1] >= tagLimit) && !(teamScores[2] >= tagLimit)
int mode_cond_tag_limit(const Game *game) {
    const GameMode *mode = game->mode;
    return !(mode->team_scores[1] >= mode->config.tag_limit) && !(mode->team_scores[2] >= mode->config.tag_limit);
}

// depoffa.js condition: nobody has deposited tagLimit KR Tags
int mode_cond_deposits(const Game *game) {
    for (size_t i = 0; i < game->player_count; i++) {
        const Player *player = game->players[i];
        if (player && player->mode.deposits >= game->mode->config.tag_limit) return 0;
    }
    return 1;
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

// raid.js v7: (lives > 0 && active) players left. v3.1.1 also ended the round 10 s after the
// boss AI (behavType 2) died; v7.1.4 dropped that, the raid ends on time or when everyone is out.
int mode_cond_raid(const Game *game) {
    for (size_t i = 0; i < game->player_count; i++) {
        const Player *player = game->players[i];
        if (player && player->mode.lives > 0 && player->active) return 1;
    }
    return 0;
}

// The official map index of the map being played (game.mapIndex), -1 none.
static int raid_map_index(const Game *game) {
    if (!game->maps || game->current_map_index < 0 || (size_t) game->current_map_index >= game->map_count) return -1;
    return map_catalog_map_index_of_json(game->maps[game->current_map_index]);
}

#define RAID_MAP_BASTION 43 // game.mapIndex 43: its run challenges need runProg.bthrone (the throne)

// raid.onEnd (v10.0.3, deob-10.0.3.js:118390-118425), when the clock did not run out (gameTimerP > 0):
// - every active player without a time gets the elapsed clock (timer, gameTimer) and, with an account in a
//   public-type game (`!isCustom || mimicPubs`), challenges.update("time", gameTimer ms);
// - every winner (didWin: the team winCondition gave the round) still active, with an account: on any map but
//   Bastion (43), or there after "bthrone": "rnodmg" (no damage taken), "r1hp" (health <= 1), "rnogun" (no shot);
//   and on any map "rsolo" when no other non-bot player was ever in the run with it (runPeakPlayers <= 1),
//   "rsolofull" when that solo run also ends at full health.
// Raid keeps its winner in a public game (mode_public_no_winner), so didWin is the winCondition's.
void mode_end_raid(Game *game) {
    if (game->round_timer <= 0.0f) return; // gameTimerP <= 0
    const float total = game->mode->started_timer;
    for (size_t i = 0; i < game->player_count; i++) {
        Player *player = game->players[i];
        if (!player || player->mode.has_timer || !player->active) continue;
        player->mode.has_timer = 1;
        player->mode.timer = total - game->round_timer;
        const int ms = (int) floorf(player->mode.timer * 1000.0f);
        if (ms > 0) game_challenge(game, player, "time", ms, -1, -1); // game_challenge: an account, pub type
    }
    const ModeWinner winner = mode_pick_winner(game);
    const int bastion = raid_map_index(game) == RAID_MAP_BASTION;
    for (size_t i = 0; i < game->player_count; i++) {
        Player *player = game->players[i];
        if (!player || !player->active) continue;
        const int did_win = (player->team && player->team == winner.team) || player == winner.player;
        if (!did_win) continue;
        const int solo = player->mode.run_peak_players <= 1;
        const int run = !bastion || player->mode.run_bthrone;
        if (run && player->mode.run_dmg_taken <= 0.0f) game_challenge(game, player, "rnodmg", 1, -1, -1);
        if (run && player->health <= 1.0f) game_challenge(game, player, "r1hp", 1, -1, -1);
        if (run && !player->mode.run_shots) game_challenge(game, player, "rnogun", 1, -1, -1);
        if (solo) game_challenge(game, player, "rsolo", 1, -1, -1);
        if (solo && player->health >= (float) player->max_health) game_challenge(game, player, "rsolofull", 1, -1, -1);
    }
}

// raid.update (deob-10.0.3.js:118426-118439): the non-bot players in the game; an active player's
// runPeakPlayers keeps the most it saw.
void mode_update_raid(Game *game, const float delta) {
    (void) delta;
    int humans = 0;
    for (size_t i = 0; i < game->player_count; i++) {
        if (game->players[i] && !game->players[i]->is_bot) humans++;
    }
    for (size_t i = 0; i < game->player_count; i++) {
        Player *player = game->players[i];
        if (player && player->active && humans > player->mode.run_peak_players) player->mode.run_peak_players = humans;
    }
}

// raid.onAIDeath (deob-10.0.3.js:118440-118447) and zom.onAIDeath (deob-10.0.3.js:119232-119235): a player's AI kill
// is challenges.update("aik", 1, {wep: loadout[loadoutIndex]}), the weapon it holds then.
void mode_ai_kill_challenge(Game *game, Player *killer) {
    if (!game || !killer) return;
    const int held = killer->loadout && killer->loadout_index >= 0 && killer->loadout_index < killer->loadout_size
                         ? killer->loadout[killer->loadout_index] : -1;
    game_challenge(game, killer, "aik", 1, held, -1);
}

// Player.init's run fields (deob-10.0.3.js:190648-190651) are reset in mode_player_spawned; these feed them.
void mode_run_shot(Game *game, Player *player) {
    if (game && player && game_is_authority(game)) player->mode.run_shots++;
}

void mode_run_damage(Game *game, Player *player, const float damage) {
    if (game && player && damage > 0.0f && game_is_authority(game)) player->mode.run_dmg_taken += damage;
}

void mode_run_progress(Player *player, const char *key) {
    if (player && key && !strcmp(key, "bthrone")) player->mode.run_bthrone = 1;
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

// finishInit multiTeam lives condition: players with lives left on more than one team
int mode_cond_lives_multi(const Game *game) {
    int seen[MODE_TEAMS] = {0}, teams = 0;
    for (size_t i = 0; i < game->player_count; i++) {
        const Player *player = game->players[i];
        if (!player || player->mode.lives <= 0 || player->team <= 0 || player->team >= MODE_TEAMS) continue;
        if (!seen[player->team]++) teams++;
    }
    return teams > 1;
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

ModeWinner mode_win_first_alive(const Game *game) {
    for (size_t i = 0; i < game->player_count; i++) {
        Player *player = game->players[i];
        if (player && player->mode.lives > 0) return winner_player(player);
    }
    return winner_player(NULL);
}

// kc.js / depo.js winCondition: the higher team score; a tie goes to denies, then kills, then
// score. v7.1.4 quirk kept (map-logic.md Appendix D): team 2's "score" sums its kills.
ModeWinner mode_win_tag_teams(const Game *game) {
    const int score1 = game->mode->team_scores[1], score2 = game->mode->team_scores[2];
    if (score1 != score2) return winner_team(score1 > score2 ? 1 : 2);

    int kills1 = 0, kills2 = 0, points1 = 0, points2 = 0, denies1 = 0, denies2 = 0;
    for (size_t i = 0; i < game->player_count; i++) {
        const Player *player = game->players[i];
        if (!player) continue;
        if (player->team == 1) {
            kills1 += (int) player->kills;
            points1 += player->score;
            denies1 += player->mode.denies;
        } else {
            kills2 += (int) player->kills;
            points2 += (int) player->kills; // the v7.1.4 bug: vLN0488 += kills
            denies2 += player->mode.denies;
        }
    }
    if (denies1 != denies2) return winner_team(denies1 > denies2 ? 1 : 2);
    if (kills1 != kills2) return winner_team(kills1 > kills2 ? 1 : 2);
    return winner_team(points1 > points2 ? 1 : 2);
}

// raid.js winCondition: someone has lives left and the clock did not run out
ModeWinner mode_win_raid(const Game *game) {
    int alive = 0;
    for (size_t i = 0; i < game->player_count; i++) {
        const Player *player = game->players[i];
        if (player && player->mode.lives > 0) alive++;
    }
    return winner_team(game->round_timer > 0.0f && alive > 0 ? 1 : 2);
}

ModeWinner mode_win_team2(const Game *game) {
    (void) game;
    return winner_team(2);
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

// ctf.js endSort == leaderSort: caps, then (both scored) the lower score first, as written in v7
int mode_sort_ctf(const Player *a, const Player *b) {
    if (a->mode.caps == b->mode.caps && a->score && b->score) return a->score - b->score;
    return b->mode.caps - a->mode.caps;
}

// depo.js / depoffa.js endSort: deposits, then more kills first
int mode_sort_depo(const Player *a, const Player *b) {
    if (a->mode.deposits == b->mode.deposits && a->kills && b->kills) return (int) b->kills - (int) a->kills;
    return b->mode.deposits - a->mode.deposits;
}

// The leaderSorts (client leaderboard) of gun / oinc / kc / depo: as the endSorts, but a tie
// puts the player with fewer kills first (a.kills - b.kills), exactly as v7.1.4 writes them.
int mode_leader_sort_gun(const Player *a, const Player *b) {
    if (a->mode.weapon_tier == b->mode.weapon_tier && a->kills && b->kills) return (int) a->kills - (int) b->kills;
    return b->mode.weapon_tier - a->mode.weapon_tier;
}

int mode_leader_sort_oinc(const Player *a, const Player *b) {
    if (a->mode.lives == b->mode.lives && a->kills && b->kills) return (int) a->kills - (int) b->kills;
    return b->mode.lives - a->mode.lives;
}

int mode_leader_sort_kc(const Player *a, const Player *b) {
    if (a->mode.confirms == b->mode.confirms && a->kills && b->kills) return (int) a->kills - (int) b->kills;
    return b->mode.confirms - a->mode.confirms;
}

int mode_leader_sort_depo(const Player *a, const Player *b) {
    if (a->mode.deposits == b->mode.deposits && a->kills && b->kills) return (int) a->kills - (int) b->kills;
    return b->mode.deposits - a->mode.deposits;
}

// ------------------------------------------------------------------ gameStart

// point.js gameStart: activeObjective = -1; nextObjective(null, true)
void mode_start_objective(Game *game) {
    game->mode->active_objective = -1;
    mode_next_objective(game, 0.0f, 1, 1, -1);
}

// king.js / depo.js / depoffa.js gameStart: activeObjective = -1; randObjective(null, true)
void mode_start_rand_objective(Game *game) {
    game->mode->active_objective = -1;
    mode_rand_objective(game, 0.0f, 1, 1);
}

void mode_start_lock(Game *game) {
    mode_lock_move(game);
}

// race: lockMove + targetScore = the score zones' points
void mode_start_race(Game *game) {
    mode_lock_move(game);
}

// def: warmupReset (players are re-placed by their next spawn) + lockMove
// def.gameStart: warmupReset, then lockMove (deob-10.0.3.js:119800-119803). v10 runs gameStart after the round's
// setGameState (deob-10.0.3.js:220047-220049, 220145), so warmupReset leaves Defuse IN_PROGRESS through "match
// starting in"; KN's game_start_round sets the state after gameStart, so that timer keeps it IDLE.
void mode_start_def(Game *game) {
    mode_warmup_reset(game);
    mode_lock_move(game);
}

void mode_start_weapon(Game *game) {
    game->mode->active_objective = 0;
    mode_next_weapon(game, 0.0f, 1);
}

// ------------------------------------------------------------------ onKill

// ctf / blitz / tdf / car onKill: dropFlag(victim.flag). A death zone / deathY resets the flag
// first (player.js kill objects, players.update deathY: resetFlag, then kill).
void mode_kill_flag(Game *game, Player *victim, Player *killer, const int weapon_id) {
    (void) killer;
    (void) weapon_id;
    if (!victim->mode.flag) return;

    if (mode_in_kill_zone(game, victim)) mode_reset_flag(game, victim->mode.flag - 1);
    else mode_drop_flag(game, victim);
}

// gun.js onKill: a streak kill (info.stk; KN: a kill without a weapon) does not promote.
void mode_kill_gun(Game *game, Player *victim, Player *killer, const int weapon_id) {
    if (killer && killer != victim && weapon_id < 0) {
        // streak kill: no tier change
    } else if (killer && killer != victim) {
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

// oinc: a kill refills one bullet (max 7), "uamo"
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

// def.onKill (deob-10.0.3.js:119839-119856): a bomb carrier killed by someone drops the bomb where it died, one
// that died without a killer sends it back to its pickup (shared/src/defuse.c)
void mode_kill_def(Game *game, Player *victim, Player *killer, const int weapon_id) {
    (void) weapon_id;
    defuse_on_kill(game, victim, killer);
}

// shrp.js onKill: killStreak % 2 -> effect 1, % 3 -> effect 2, % 5 -> effect 0 (players.giveEffect)
void mode_kill_shrp(Game *game, Player *victim, Player *killer, const int weapon_id) {
    (void) game;
    (void) weapon_id;
    if (!killer || killer == victim) return;
#ifdef PLAYER_EFFECTS_V7 // player-viewer WP-B effects (shared/src/effects.c)
    if (killer->kill_streak % 2 == 0) player_give_effect(killer, 1, 1);
    if (killer->kill_streak % 3 == 0) player_give_effect(killer, 2, 1);
    if (killer->kill_streak % 5 == 0) player_give_effect(killer, 0, 1);
#endif
}

// krank.js onKill: the killer is (re)kranked for krankedTimer ("chrg"); a kill while kranked or
// of a kranked victim gives +35 "Recharge" / "Clock Work".
void mode_kill_krank(Game *game, Player *victim, Player *killer, const int weapon_id) {
    (void) weapon_id;
    if (!killer || killer == victim) return;
    const int was_kranked = killer->mode.is_kranked;
    killer->mode.kranked_timer = game->mode->config.kranked_timer;
    killer->mode.is_kranked = 1;
    mode_send_counter(game, NET_MODE_COUNTER_CHARGE, killer, killer->mode.kranked_timer, 0);
    if (was_kranked || victim->mode.is_kranked) {
        player_score(killer, 35, 0);
        mode_send_medal(game, killer, was_kranked ? "Recharge" : "Clock Work", 35);
    }
}

// krank.js update: an expired kranked timer explodes (radius 50, 300 damage) and kills its player.
void mode_update_krank(Game *game, const float delta) {
    for (size_t i = 0; i < game->player_count; i++) {
        Player *player = game->players[i];
        if (!player || !player->active || player->mode.kranked_timer <= 0.0f) continue;
        player->mode.kranked_timer -= delta;
        if (player->mode.kranked_timer > 0.0f) continue;
        player->mode.kranked_timer = 0.0f;
        const vec3 at = {player->position.x, player->position.y + 6.0f, player->position.z};
        game_explosion(game, at, 50.0f, 300.0f, player, -1); // game.explosion(x, y + 6, z, 50, 300, player, {})
        if (player->active) player_kill(player, NULL, NULL, 0);
    }
}

// bhffa.js onKill: headMlt = min(headMltMax, 1 + headMlt * kills) -> updateHeadSize, "bh"
void mode_kill_bhffa(Game *game, Player *victim, Player *killer, const int weapon_id) {
    (void) weapon_id;
    if (!killer || killer == victim) return;
    const GameModeConfig *config = &game->mode->config;
    const float step = config->head_mlt > 0.0f ? config->head_mlt : 0.1f;
    const float max = config->head_mlt_max > 0.0f ? config->head_mlt_max : 3.0f;
    killer->mode.head_mlt = fminf(max, 1.0f + step * (float) killer->kills);
    mode_send_counter(game, NET_MODE_COUNTER_HEAD, killer, killer->mode.head_mlt, 0);
}

// imp.js onKill: a killer whose knife is out (thrown) gets it back (refillPlayer)
void mode_kill_imp(Game *game, Player *victim, Player *killer, const int weapon_id) {
    (void) weapon_id;
    if (!killer || killer == victim || killer->can_throw) return;
    killer->can_throw = 1;
    game_send(game, killer->uid, NET_PACKET_REFILL, NULL, 0);
}

// aon.js onKill: the victim drops an Ammo Pack (its magazine, data.c) and 30% a Melee Pickup; a
// killer on a streak of 5+ gets a random effect of {0, 2, 9} half of the time.
void mode_kill_aon(Game *game, Player *victim, Player *killer, const int weapon_id) {
    (void) weapon_id;
    int use_ground = victim->mode.has_last_ground && (!killer || killer == victim);
    if ((killer && killer != victim) || use_ground) {
        if (victim->mode.has_last_ground && !victim->on_ground) use_ground = 1;
        const vec3 at = use_ground ? victim->mode.last_ground : victim->position;
        const SpawnableData pack = {.c = victim->ammo && victim->loadout_size ? (int) victim->ammo[0] : 0, .w = -1,
                                    .xd = victim->direction.x + (float) M_PI / 2.0f};
        spawnable_drop_ex(game, SPAWNABLE_AMMO_PACK, victim, &pack, at);
        if (mode_rand_int(1, 100) <= 30) spawnable_drop(game, SPAWNABLE_MELEE_PICKUP, victim, 0, at);
    }
    if (killer && killer != victim && killer->kill_streak >= 5 && mode_rand_int(1, 100) >= 50) {
        static const int k_effects[] = {0, 2, 9};
        const int effect = k_effects[mode_rand_int(0, 2)];
#ifdef PLAYER_EFFECTS_V7 // player-viewer WP-B effects (shared/src/effects.c)
        player_give_effect(killer, effect, 1);
#else
        (void) effect;
#endif
    }
}

// md.js onKill: a Weapon Box with a random weapon, and 20% a Melee Pickup 3-8 units away
void mode_kill_md(Game *game, Player *victim, Player *killer, const int weapon_id) {
    (void) weapon_id;
    static const int k_weapons[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 14, 16, 18, 21, 27};
    if (!killer || killer == victim) return;
    vec3 at;
    mode_kill_point(victim, &at);
    const SpawnableData box = {.w = k_weapons[mode_rand_int(0, (int) (sizeof(k_weapons) / sizeof(k_weapons[0])) - 1)],
                               .xd = victim->direction.x + (float) M_PI / 2.0f};
    spawnable_drop_ex(game, SPAWNABLE_WEAPON_BOX, victim, &box, at);
    if (mode_rand_int(1, 100) <= 20) {
        // UTILS.randInt2(3, 8): 3..8 with a random sign
        const float dx = (float) mode_rand_int(3, 8) * (mode_random() < 0.5f ? -1.0f : 1.0f);
        const float dz = (float) mode_rand_int(3, 8) * (mode_random() < 0.5f ? -1.0f : 1.0f);
        spawnable_drop(game, SPAWNABLE_MELEE_PICKUP, victim, 0, (vec3) {at.x + dx, at.y, at.z + dz});
    }
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

// pickSimon (deob-10.0.3.js:217891-217911): the host (game.host, game_host_player) when it is an active player, else
// a random active player (no random draw when the host plays); 5000 health.
void mode_trigger_pick_simon(Game *game, const float overflow) {
    (void) overflow;
    Player *simon = game_host_player(game);
    if (simon && !simon->active) simon = NULL;
    if (simon || pick_random(game, &simon, 1)) {
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

// Krunker v10.0.2 pickStalker (v10:188459-188474): knife only, no regen, health floor(200 + 50 x
// (active players - 1)) (v7.1.4: 400). v10 then broadcasts STALKER_PICKED to everyone but the stalker
// (deob-10.0.3.js:217949; their "You are the Living" popup, client/src/ui/hud_stalker.c).
void mode_trigger_pick_stalker(Game *game, const float overflow) {
    (void) overflow;
    const int active = mode_active_count(game);
    Player *stalker;
    if (pick_random(game, &stalker, 1)) {
        mode_update_team(game, stalker, 2);
        // v7.1.4 / Krunker Strike: maxHealth = 400 (GameModeConfig.stalker_v7)
        const int health = game->mode && game->mode->config.stalker_v7 ? 400 : (int) floorf(200.0f + 50.0f * (float) (active - 1));
        mode_set_role_health(game, stalker, health);
        const int knife = WEAPON_ID_KNIFE;
        mode_set_player_loadout(stalker, &knife, 1, 0);
        mode_send_loadout(game, stalker);
        if (game->mode && game->mode->config.stalker_v10) stalker_send_picked(game, stalker); // v7.1.4 had no STALKER_PICKED
    }
}

// v10 stalk.onKill (v10:104388-104391): a kill by the stalker jump-scares the victim (send
// JUMP_SCARE 0, v7's onMelee "scr") and heals the stalker 50.
void mode_kill_stalk(Game *game, Player *victim, Player *killer, const int weapon_id) {
    (void) weapon_id;
    if (!killer || killer->team != 2 || !game_is_authority(game)) return;
    if (victim) {
        const NetJumpScarePacket packet = {.index = 0};
        game_send(game, victim->uid, NET_PACKET_JUMP_SCARE, &packet, sizeof(packet));
    }
    player_change_health(killer, NULL, -50.0f, 0);
}

// v10 stalk.update (v10:104392-104404): once the stalker is picked (no wait timer left), a Living
// player who stays within 200 of where they settled for 20 s turns Dizzy (effect 5) until they
// have been 2 s outside; coming back within 10 s of leaving re-applies it, and 10 s away moves the
// camp origin to where they are. v10 times leaving with Date.now(); the game clock here.
#define STALK_CAMP_RADIUS 200.0f
#define STALK_CAMP_TIME 20.0f
#define STALK_CAMP_CLEAR 2.0f
#define STALK_CAMP_RESET 10.0f
void mode_update_stalk(Game *game, const float delta) {
    if (!game->mode || game->mode->wait_timer_count) return;
    for (size_t i = 0; i < game->player_count; i++) {
        Player *p = game->players[i];
        if (!p || !p->active || p->team != 1) continue;
        PlayerStalkerCamp *c = &p->stalker_camp;
        if (!c->has_origin) {
            c->has_origin = 1;
            c->origin = p->position;
            c->timer = 0.0f;
            c->has_effect = 0;
            c->remove_timer = 0.0f;
            c->left_at = -1.0;
        }
        const float dx = p->position.x - c->origin.x, dy = p->position.y - c->origin.y, dz = p->position.z - c->origin.z;
        if (sqrtf(dx * dx + dy * dy + dz * dz) <= STALK_CAMP_RADIUS) {
            c->timer += delta;
            if (c->left_at >= 0.0 && game->now - c->left_at <= STALK_CAMP_RESET && c->has_effect) player_give_effect(p, EFFECT_DIZZY, 1);
            if (c->timer >= STALK_CAMP_TIME && !c->has_effect) {
                player_give_effect(p, EFFECT_DIZZY, 1);
                c->has_effect = 1;
            }
            c->remove_timer = 0.0f;
            c->left_at = -1.0;
        } else {
            if (c->left_at < 0.0) c->left_at = game->now;
            if (c->has_effect) {
                c->remove_timer += delta;
                if (c->remove_timer >= STALK_CAMP_CLEAR) {
                    // removeEffect(player, 5, 4): four stacks off, cleared at 0
                    for (int k = 0; k < 4 && p->effects[EFFECT_DIZZY]; k++) player_remove_effect(p, EFFECT_DIZZY, p->effects[EFFECT_DIZZY] > 1);
                    c->has_effect = 0;
                }
            }
            if (game->now - c->left_at > STALK_CAMP_RESET) {
                c->origin = p->position;
                c->timer = 0.0f;
                c->has_effect = 0;
                c->remove_timer = 0.0f;
                c->left_at = -1.0;
            }
        }
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
    mode_next_objective(game, overflow, 0, 0, -1);
}

void mode_trigger_rand_objective(Game *game, const float overflow) {
    mode_rand_objective(game, overflow, 0, 0);
}

void mode_trigger_next_weapon(Game *game, const float overflow) {
    mode_next_weapon(game, overflow, 0);
}

void mode_trigger_skip(Game *game, const float overflow) {
    (void) overflow;
    game->mode->skip = 1;
}
