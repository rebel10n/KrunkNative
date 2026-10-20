#pragma once
// Quick Play retry: a Quick Play search (Quick Match and F4 Find New Game, Find Game, the launch's
// Auto Seek, Quick Join, and the krunker.io ones) that is answered "full" is not the end of it. The
// same seek is made again after a short backoff (1 s, then 2, 3, 4 and 5 s, the cap), until a lobby
// takes the player. Nothing is shown while it waits (v10's Quick Match seek shows no popup, v10.js:222040).
// A cancel (F4 or Quick Match again, Escape, leaving the menu, a manual join) stops it, and an answer that
// was on its way is then ignored.
//
// The matchmaking code keeps its own state (which lobbies were full, the find flags); this module
// only owns the timer, the run's busy / waiting state and the generation that tells a stale answer.
#include <stddef.h>

typedef void (*QuickPlayRetryFn)(void *user);

// A Quick Play search starts (or replaces the running one): the backoff restarts, and an answer still
// on its way from the earlier search is ignored (quick_play_retry_stale).
void quick_play_retry_begin(void);
// 1 while a search is out (a seek or a join in flight) or a retry waits; 0 once it has settled.
int quick_play_retry_running(void);
// A search is out: the answer that lands clears it (quick_play_retry_answered). Matchmaking calls it
// when it issues a seek of a search.
void quick_play_retry_out(void);
// The answer of the seek in flight arrived: the search is out no more (a retry may be scheduled after).
void quick_play_retry_answered(void);
// Schedules fn(user) after the backoff, with no popup. Returns 1: the caller must not show
// the full error. A waiting retry is replaced by the new one.
int quick_play_retry_schedule(QuickPlayRetryFn fn, void *user);
// Fires the retry that is due (servers_ui_tick, once per frame). Nothing happens when none is due.
void quick_play_retry_tick(void);
// Stops the search: a waiting retry is dropped, an answer still on its way is ignored.
void quick_play_retry_cancel(void);
// The search is over (a lobby was joined, or the error is shown for good): nothing waits, nothing is out.
void quick_play_retry_end(void);
// The generation a seek of the search carries; quick_play_retry_stale(gen) is 1 once it was cancelled
// or replaced, so its answer must be dropped.
unsigned quick_play_retry_generation(void);
int quick_play_retry_stale(unsigned gen);
// krunker.io joins: the retry for the join that is about to start (krunker_lobbies_open's in-client
// join). krunker_net takes it when the join starts; disarm after the call (a join that did not start
// leaves nothing armed).
void quick_play_retry_arm(QuickPlayRetryFn fn, void *user);
int quick_play_retry_take(QuickPlayRetryFn *fn, void **user);
void quick_play_retry_disarm(void);
