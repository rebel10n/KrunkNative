#pragma once
// The game time tile's total (hud_ui.c, the top-right stat tiles): the time this device has spent in a
// game, kept locally (the signed-in account plays no part: guests have the tile too). GL / DOM free:
// client/tests/game_time_test.c. The stored value (html_storage "knGameTime", decimal ms) is read once
// at the first HUD frame and written every GAME_TIME_FLUSH_MS of play, when a game ends and at exit.
//
// What counts: every game the client is in, the local player's session (Box Office games, official
// krunker.io games, spectating and dead included), while the game is on screen: not over the ESC menu
// and not on the end screen (between rounds nothing is played). The count runs whether or not the tile
// is shown (the Client tab's "Show Game Time" only shows it), so the total is complete when it is turned on.

#include <stddef.h>

#define GAME_TIME_FLUSH_MS 10000.0 // the stored total is at most this much behind the play (a crash loses it)

typedef struct GameTime {
    double saved_ms;   // the total as stored (ms, whole)
    double pending_ms; // play since the last store
    int in_session;    // a game was up on the last step
    double last_ms;    // the HUD clock of the last step (ms)
} GameTime;

// The stored text (NULL / "" / junk = 0). Called once, before the first step.
void game_time_load(GameTime *t, const char *text);

// One HUD frame. session: a game is up for the local player (in game, whatever the server); counting: its
// time runs now (session, the menu closed, the end screen not up); now_ms: the HUD clock.
// Returns 1 when the total must be stored now: GAME_TIME_FLUSH_MS of play since the last store, or the
// game just ended with play to keep.
int game_time_step(GameTime *t, int session, int counting, double now_ms);

// The total: stored plus pending, ms.
double game_time_ms(const GameTime *t);

// Writes the total as the store keeps it (decimal whole ms) into out and moves the pending play into the
// stored total: call with the html_storage_set of the returned text. Returns the text length.
int game_time_store(GameTime *t, char *out, size_t n);

// UTILS.getReadableTime(ms, true) of v7.1.4 (src/libs/utils.js): "1y 2d 3h 4m 5s", the years, days
// and hours only when non-zero, minutes and seconds always ("0m 5s"), no trailing space.
void game_time_readable(char *out, size_t n, double ms);
