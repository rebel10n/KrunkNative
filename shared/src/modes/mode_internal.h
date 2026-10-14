// Private to shared/src/modes: the mode hooks referenced by the mode table and the helpers the
// rule files share. Everything here runs on the authority unless noted.
#pragma once
#include <shared.h>
#include <spawnables.h>
#include <stdio.h>

// ---- conditions (mode_rules.c): 0 ends the round
int mode_cond_team1_active(const Game*);  // hide / infect / simon
int mode_cond_team2_active(const Game*);  // boss
int mode_cond_teams_alive(const Game*);   // prop / trai: lives > 0 and active on both teams
int mode_cond_stalk(const Game*);
int mode_cond_race(const Game*);
int mode_cond_gun(const Game*);
int mode_cond_oinc(const Game*);
int mode_cond_tag_limit(const Game*);     // kc / depo: a team score reached tagLimit
int mode_cond_deposits(const Game*);      // depoffa: a player deposited tagLimit
int mode_cond_def(const Game*);
int mode_cond_raid(const Game*);
int mode_cond_lives_ffa(const Game*);     // finishInit defaults for lives modes
int mode_cond_lives_teams(const Game*);
int mode_cond_lives_multi(const Game*);   // multiTeam: lives on more than one team
int mode_cond_score_ffa(const Game*);     // finishInit scoreLimit conditions
int mode_cond_score_teams(const Game*);

// ---- winConditions
ModeWinner mode_win_team1_active(const Game*);
ModeWinner mode_win_team2_active(const Game*);
ModeWinner mode_win_lives(const Game*);
ModeWinner mode_win_gun(const Game*);
ModeWinner mode_win_first_alive(const Game*);
ModeWinner mode_win_tag_teams(const Game*);  // kc / depo: team score, then denies, kills, score
ModeWinner mode_win_def(const Game*);
ModeWinner mode_win_raid(const Game*);
ModeWinner mode_win_team2(const Game*);       // zom: always 2

// ---- endSort / leaderSort
int mode_sort_race(const Player*, const Player*);
int mode_sort_gun(const Player*, const Player*);
int mode_sort_oinc(const Player*, const Player*);
int mode_sort_kc(const Player*, const Player*);
int mode_sort_ctf(const Player*, const Player*);
int mode_sort_depo(const Player*, const Player*);
int mode_leader_sort_gun(const Player*, const Player*);
int mode_leader_sort_oinc(const Player*, const Player*);
int mode_leader_sort_kc(const Player*, const Player*);
int mode_leader_sort_depo(const Player*, const Player*);

// ---- gameStart
void mode_start_objective(Game*);         // point: nextObjective(null, true)
void mode_start_rand_objective(Game*);    // king / depo / depoffa: randObjective(null, true)
void mode_start_lock(Game*);
void mode_start_race(Game*);
void mode_start_def(Game*);
void mode_start_weapon(Game*);

// ---- onKill
void mode_kill_flag(Game*, Player *victim, Player *killer, int weapon_id); // ctf / blitz / tdf / car: dropFlag
void mode_kill_gun(Game*, Player *victim, Player *killer, int weapon_id);
void mode_kill_oinc(Game*, Player *victim, Player *killer, int weapon_id);
void mode_kill_kc(Game*, Player *victim, Player *killer, int weapon_id);
void mode_kill_def(Game*, Player *victim, Player *killer, int weapon_id);
void mode_kill_trai(Game*, Player *victim, Player *killer, int weapon_id); // Traitor ghosts (ai.c)
void mode_kill_shrp(Game*, Player *victim, Player *killer, int weapon_id); // effects by killstreak
void mode_kill_king(Game*, Player *victim, Player *killer, int weapon_id); // Defender / Attacker
void mode_kill_dom(Game*, Player *victim, Player *killer, int weapon_id);
void mode_kill_krank(Game*, Player *victim, Player *killer, int weapon_id);
void mode_kill_deposit(Game*, Player *victim, Player *killer, int weapon_id); // depo / depoffa: KR Tag clusters
void mode_kill_aon(Game*, Player *victim, Player *killer, int weapon_id);
void mode_kill_md(Game*, Player *victim, Player *killer, int weapon_id);
void mode_kill_bhffa(Game*, Player *victim, Player *killer, int weapon_id);
void mode_kill_imp(Game*, Player *victim, Player *killer, int weapon_id);

// ---- update (game.update #23, server) and collisionCheck
void mode_update_point(Game*, float delta);
void mode_update_king(Game*, float delta);
void mode_update_deposit(Game*, float delta);   // depo (500 ms) / depoffa (750 ms)
void mode_update_carrier(Game*, float delta);   // tdf / car: the flag carrier scores
void mode_update_krank(Game*, float delta);
void mode_collision_king(Game*, Player*);
void mode_collision_dom(Game*, Player*);
void mode_end_raid(Game*);                      // raid onEnd: the survivors' times

// ---- waitTimer triggers
void mode_trigger_pick_seeker(Game*, float);
void mode_trigger_release_seeker(Game*, float);
void mode_trigger_infect_random(Game*, float);
void mode_trigger_unlock_move(Game*, float);
void mode_trigger_pick_simon(Game*, float);
void mode_trigger_lock_seekers(Game*, float);
void mode_trigger_pick_boss(Game*, float);
void mode_trigger_pick_stalker(Game*, float);
// v10.0.2 Stalker rework (shared/src/modes/modes.c MODE_ID_STALK)
void mode_kill_stalk(Game*, Player *victim, Player *killer, int weapon_id);
void mode_update_stalk(Game*, float delta);
float mode_melee_stalk(Game*, Player *dealer, Player *victim, float damage);
void mode_trigger_pick_traitors(Game*, float);
void mode_trigger_def_start(Game*, float);
void mode_trigger_lives_start(Game*, float);
void mode_trigger_next_objective(Game*, float);
void mode_trigger_rand_objective(Game*, float);
void mode_trigger_next_weapon(Game*, float);
void mode_trigger_skip(Game*, float);

// ---- game actions (mode_actions.c)
void mode_update_team(Game*, Player*, int team);         // updateTeam ("tm")
void mode_set_role_health(Game*, Player*, int health);   // maxHealth = health, health = maxHealth, regen off ("h")
void mode_lock_player(Game*, Player*, int locked);       // lockPlayer ("lock")
void mode_lock_move(Game*);                              // lockMove ("lm")
void mode_unlock_move(Game*);                            // unlockMove ("ulm")
// nextObjective(time, push, forceUid): `initial` = time == null ("obj" initial flag), `push`
// appends the rotation timer to the waitTimers, force_uid < 0 = the next uid (getNextObjective)
void mode_next_objective(Game*, float overflow, int initial, int push, int force_uid);
void mode_rand_objective(Game*, float overflow, int initial, int push); // randObjective
void mode_next_weapon(Game*, float overflow, int initial);    // nextWeapon
void mode_push_wait_timer(Game*, const ModeWaitTimer*, int replace); // waitTimers.push / = [timer]
void mode_increase_weapon(Game*, Player*, int weapon_id);
void mode_decrease_weapon(Game*, Player*);
int mode_gun_loadout(const Game*, const Player*, int *out); // Gun Game / Stalker / Sharp Shooter loadout, 0 none
int mode_pick_active(const Game*, Player **out, int max);   // the active players (Y list)

// ---- objectives / flags / conversions (mode_objectives.c)
void mode_load_objects(Game*);                           // objectives, flags, dom zones and score zones from the map
void mode_tick_flags(Game*, float delta);                // updateFlags (server half) + pickupFlag / capFlag
void mode_tick_conversions(Game*);                       // players.playerCollisions (convTeam)
void mode_drop_flag(Game*, Player *carrier);             // dropFlag
void mode_reset_flag(Game*, int index);                  // resetFlag
void mode_reset_flags(Game*);                            // resetFlags (finishInit)
int mode_player_collides(const Player*, vec3 center, vec3 half); // Player.collides (box)
int mode_in_kill_zone(const Game*, const Player*);       // a death zone / below deathY (flag resets, not drops)
ModeObjective *mode_active_objective(Game*);             // the objective whose uid is activeObjective, NULL none
Player *mode_find_player(const Game*, int uid);
int mode_kill_point(const Player *victim, vec3 *out);    // the lastGround-or-position drop point of a kill (see .c)

// ---- domination (mode_dom.c)
void mode_reset_dom_zones(Game*);
void mode_tick_dom(Game*, float delta);                  // updateDomZones (server half)
int mode_has_all_zones(const Game*);                     // hasAllZones: the team holding every zone, 0 none

// ---- packets (mode_net.c); all no-ops without game hooks
void mode_send_message(Game*, const char *text);         // gmsg to everyone
void mode_send_message_to(Game*, int uid, const char *text);
void mode_send_team(Game*, const Player*);
void mode_send_team_score(Game*, int team);
void mode_send_flag(Game*, int index);
void mode_send_objective(Game*, int kind, int value);
void mode_send_objective_v7(Game*, int uid, int initial); // "obj uid, objtvTime, initial"
void mode_send_occupancy(Game*, const ModeObjective*);   // "uobj uid, onZone"
void mode_send_dom_update(Game*, const int *zones, int count, int uid); // "uct" (zones by index)
void mode_send_dom_capture(Game*, int zone, int team);   // "cct uid, team"
void mode_send_rounds(Game*, int win_team);              // "rds roundsLeft, false, winTeam, teamRoundWins"
void mode_send_lockout(Game*);                           // "lkt"
void mode_send_text_popup(Game*, const char *title, const char *text); // "tx"
void mode_send_counter(Game*, int kind, const Player*, float value, int to_player); // "krt" / "chrg" / "bh"
void mode_send_lives(Game*, const Player*);
void mode_send_action(Game*, const Player*, const Player *other, const char *text);
void mode_send_loadout(Game*, const Player*);
void mode_send_lock(Game*, int uid, int kind, int locked);
void mode_send_spawnable(Game*, int index);              // "crsp" / "dsp" (v7 packet)
void mode_send_medal(Game*, const Player*, const char *text, int score);
void mode_send_health(Game*, const Player*);             // the "h" that follows a role change

// ---- spawnables (shared/src/spawnables.c)
void mode_drop_spawnable(Game*, const Player *victim);   // dropSpawnable(0, victim): the KC tag

// ---- misc
float mode_random(void);                                 // [0, 1)
int mode_rand_int(int min, int max);                     // UTILS.randInt (inclusive)
void mode_team_score_add(Game*, int team, int amount);   // game.giveTeamScore + "ts"
float mode_cfg_objective_time(const Game*);              // config.objtvTime (minutes, 1)
