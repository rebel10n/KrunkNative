#pragma once
// Bots play the mode (shared/src/bot_objective.c): the goal a NextBot's Hunt state walks to and
// the objective-vs-combat weighting of its Attack state, per game mode:
//   CTF / Blitz      get the enemy flag, carry it home, return a dropped flag, intercept the
//                    enemy carrier, defenders guard the base (1 from 3 players, 2 from 6),
//                    escorts follow a carrier
//   Team Defender /  the neutral flag: get it, keep it away from the enemy (carriers score over
//   Carrier          time), chase the enemy carrier
//   Hardpoint / KOTH hold the active objective (spread over it)
//   Domination       capture the zones the team does not own (spread by rank), defend them
//   Deposit (FFA)    collect KR Tags, deposit them on the active objective
//   Kill Confirmed   collect tags
//   Defuse           attackers take the bomb (the nearest bot), carry it to the nearest site and plant it
//                    (shared/src/defuse.c), escort / push the sites, guard it once planted; defenders hold
//                    the sites, then rush the planted bomb and defuse it
//   lives modes,     elimination: hunt the remaining enemies, regroup when outnumbered
//   Infected
//   anything else    no goal (FFA / TDM / Parkour / ...: the Hunt roam)
// Team coordination: the bots of a team are ranked (bot list order); roles and spread spots come
// from the rank so they do not all go for the same thing, and the nearest bot claims a dropped
// flag / the carrier. Krunker v7.1.4's NextBot has none of this (Hunt roams, Attack fights): a
// KrunkNative addition, documented as such in bots.c.
#include <shared.h>
#include <bots.h>

// Hunt: 1 = the bot has an objective goal this tick (bot->objective.goal / hold / arrived),
// 0 = none (roam as usual). Decides every ~0.5 s (at once on a flag / tag change) and gives
// up on a goal the nav grid cannot reach for a few seconds.
int bot_objective_update(Game*, GameBot*, Player *self, float dt);
// The yaw to face while holding a point (sweeps around the threat direction).
float bot_objective_hold_yaw(GameBot*, const Player *self, float dt);
// Where to walk while holding (arrived on a hold goal): v10's bots contest a zone, they keep moving
// inside it and strafe, never parked where they stepped in. A walkable spot inside the volume near
// the bot (around the goal without a volume), a new one once reached or every 1.2-2.5 s.
vec3 bot_objective_hold_spot(Game*, GameBot*, const Player *self, float dt);
// Attack: 1 when the objective outweighs chasing `target` (a flag carrier runs home, a holder
// stays on its point while the enemy is far): *goal is where to walk while fighting.
int bot_objective_over_combat(Game*, GameBot*, Player *self, const Player *target, float dt, vec3 *goal);
// Defuse: 1 when the bot stands where it plants (a site, the bomb in hand) or defuses (at the planted bomb):
// stand still and hold M1 this tick; 2 stand still without M1 (the bomb is coming in hand); 0 neither.
// *swap: the input[11] code to send (4 equips the bomb, 3 puts a bomb carried in hand away while it is not
// on a site), 0 none.
int bot_objective_defuse_hold(Game*, GameBot*, Player *self, int *swap);
const char *bot_role_name(BotRole);
const char *bot_goal_name(BotGoalKind);
