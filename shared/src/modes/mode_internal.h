// Private to shared/src/modes: the modes.js hooks referenced by the mode table and the
// helpers the rule files share. Everything here runs on the authority.
#pragma once
#include <shared.h>
#include <stdio.h>

// ---- modes.js conditions (mode_rules.c): 0 ends the round
int mode_cond_team1_active(const Game*);  // hide / infect / simon
int mode_cond_team2_active(const Game*);  // boss
int mode_cond_teams_alive(const Game*);   // prop / trai: lives > 0 and active on both teams
int mode_cond_stalk(const Game*);
int mode_cond_race(const Game*);
int mode_cond_gun(const Game*);
int mode_cond_kills(const Game*);         // ranked: first to 10 kills
int mode_cond_oinc(const Game*);
int mode_cond_kc(const Game*);
int mode_cond_def(const Game*);
int mode_cond_raid(const Game*);
int mode_cond_lives_ffa(const Game*);     // finishInit defaults for lives modes
int mode_cond_lives_teams(const Game*);
int mode_cond_score_ffa(const Game*);     // finishInit scoreLimit conditions
int mode_cond_score_teams(const Game*);

// ---- winConditions
ModeWinner mode_win_team1_active(const Game*);
ModeWinner mode_win_team2_active(const Game*);
ModeWinner mode_win_lives(const Game*);
ModeWinner mode_win_gun(const Game*);
ModeWinner mode_win_kills(const Game*);
ModeWinner mode_win_first_alive(const Game*);
ModeWinner mode_win_kc(const Game*);
ModeWinner mode_win_def(const Game*);

// ---- endSort
int mode_sort_race(const Player*, const Player*);
int mode_sort_gun(const Player*, const Player*);
int mode_sort_oinc(const Player*, const Player*);
int mode_sort_kc(const Player*, const Player*);

// ---- gameStart
void mode_start_objective(Game*);
void mode_start_lock_objective(Game*);
void mode_start_lock(Game*);
void mode_start_race(Game*);
void mode_start_def(Game*);
void mode_start_weapon(Game*);

// ---- onKill
void mode_kill_ctf(Game*, Player *victim, Player *killer, int weapon_id);
void mode_kill_gun(Game*, Player *victim, Player *killer, int weapon_id);
void mode_kill_oinc(Game*, Player *victim, Player *killer, int weapon_id);
void mode_kill_kc(Game*, Player *victim, Player *killer, int weapon_id);
void mode_kill_def(Game*, Player *victim, Player *killer, int weapon_id);
void mode_kill_trai(Game*, Player *victim, Player *killer, int weapon_id); // Traitor ghosts (ai.c)

// ---- waitTimer triggers
void mode_trigger_pick_seeker(Game*, float);
void mode_trigger_release_seeker(Game*, float);
void mode_trigger_infect_random(Game*, float);
void mode_trigger_unlock_move(Game*, float);
void mode_trigger_pick_simon(Game*, float);
void mode_trigger_lock_seekers(Game*, float);
void mode_trigger_pick_boss(Game*, float);
void mode_trigger_pick_stalker(Game*, float);
void mode_trigger_pick_traitors(Game*, float);
void mode_trigger_def_start(Game*, float);
void mode_trigger_lives_start(Game*, float);
void mode_trigger_next_objective(Game*, float);
void mode_trigger_next_weapon(Game*, float);
void mode_trigger_skip(Game*, float);

// ---- game actions (mode_actions.c)
void mode_update_team(Game*, Player*, int team);         // updateTeam ("tm")
void mode_pick_spawn(Game*, Player*);                    // getSpawnPoint(team): sets position + direction (team players only)
void mode_set_role_health(Game*, Player*, int health);   // aK3 = health, health = aK3, regen off ("h")
void mode_lock_player(Game*, Player*, int locked);       // lockPlayer ("lock")
void mode_lock_move(Game*);                              // lockMove ("lm")
void mode_unlock_move(Game*);                            // unlockMove ("ulm")
void mode_next_objective(Game*, float overflow, int initial); // nextObjective
void mode_next_weapon(Game*, float overflow, int initial);    // nextWeapon
void mode_push_wait_timer(Game*, const ModeWaitTimer*, int replace); // waitTimers.push / = [timer]
void mode_increase_weapon(Game*, Player*, int weapon_id);
void mode_decrease_weapon(Game*, Player*);
int mode_gun_loadout(const Game*, const Player*, int *out); // Gun Game / Stalker / Sharp Shooter loadout, 0 none
int mode_pick_active(const Game*, Player **out, int max);   // the active players (Y list)

// ---- objectives / flags / tags (mode_objectives.c)
void mode_load_objects(Game*);                           // objectives, flags and score zones from the map JSON
void mode_tick_objectives(Game*, float delta);
void mode_tick_flags(Game*, float delta);
void mode_tick_spawnables(Game*, float delta);
void mode_tick_conversions(Game*);                       // players.playerCollisions (convTeam)
void mode_drop_flag(Game*, Player *carrier);             // dropFlag
void mode_reset_flag(Game*, int index);                  // resetFlag
void mode_drop_spawnable(Game*, const Player *victim);   // dropSpawnable(0, victim)
int mode_player_collides(const Player*, vec3 center, vec3 half); // Player.collides (box)
int mode_in_kill_zone(const Game*, const Player*);       // a death zone / below deathY (flag resets, not drops)

// ---- packets (mode_net.c); all no-ops without game hooks
void mode_send_message(Game*, const char *text);         // gmsg to everyone
void mode_send_message_to(Game*, int uid, const char *text);
void mode_send_team(Game*, const Player*);
void mode_send_team_score(Game*, int team);
void mode_send_flag(Game*, int index);
void mode_send_objective(Game*, int kind, int value);
void mode_send_lives(Game*, const Player*);
void mode_send_action(Game*, const Player*, const Player *other, const char *text);
void mode_send_loadout(Game*, const Player*);
void mode_send_lock(Game*, int uid, int kind, int locked);
void mode_send_spawnable(Game*, int index);
void mode_send_medal(Game*, const Player*, const char *text, int score);
void mode_send_health(Game*, const Player*);             // the "h" that follows a role change

// ---- misc
float mode_random(void);                                 // [0, 1)
int mode_rand_int(int min, int max);                     // UTILS.randInt (inclusive)
void mode_team_score_add(Game*, int team, int amount);   // players.teamScore
