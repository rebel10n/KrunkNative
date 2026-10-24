#pragma once
// The team forfeit vote (Krunker v10.0.3 voteToForfeit, deob-10.0.3.js:221463-221546; shared/src/forfeit.c). The state is
// Game.forfeit (shared.h GameForfeit); the packets are NET_EXT_VOTE_FORFEIT (net_ext.h 0x0902).
#include <stdint.h>

struct Game_t;
struct Player_t;

// startGame: no vote runs, nobody has started one (v10 :189818), no forced winner
void forfeit_round_start(struct Game_t *game);
// game.players.remove: a team player who leaves stays eligible for FORFEIT_GRACE_S (v10 voteForfeitDisconnectGrace)
void forfeit_player_left(struct Game_t *game, const struct Player_t *player);
// game.update: the vote's timer (FORFEIT_TIMER_S, then "vf" null to everyone), and the pass check
void forfeit_tick(struct Game_t *game, float delta);
// A player's join mid-vote: the open vote of its team (the joiner's "vf")
void forfeit_send_to(struct Game_t *game, int uid);
// The chat command /ff (also /forfeit): start a vote, or vote on the running one (ranked only)
void forfeit_command(struct Game_t *game, const struct Player_t *sender);
// The team the forfeit gave the round to (mode_pick_winner), 0 when no forfeit ended the round
int forfeit_forced_team(const struct Game_t *game);
