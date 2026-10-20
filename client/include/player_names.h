#pragma once
// Hide player names (Settings > Game > Gameplay Privacy "Hide player names", knClient hidePlayerNames, default off):
// the one place a player's name, clan tag and profile picture are read for display
// (client/src/ui/player_names.c). A surface shows these results instead of the raw value; the server's
// data and what the client sends are never changed.
//
// Hidden, every other player shows as Player_<n> and the local player as Player. <n> counts the players
// the client first saw in the match (player_names_see at SPAWN: 1, 2, 3 ...), so a player keeps its
// number for the whole match; leaving the game (client_leave_game) starts the count again. The clan tag
// is "" and the picture is the default one. Not hidden, the name is what the surface showed before
// (streamer mode's scrambling is still applied by the caller, and it passes through a hidden name).
#include <stddef.h>

// The name to show for a player. uid: its uid (-1: no uid, numbered by the player of that name: one player, one number,
// so a name first seen with no uid, an official server's join line, keeps its number when its uid comes). self: the
// local player.
// Not hidden: `name` itself. Hidden: "Player" (self) or "Player_<n>", written to `out` (size bytes) and returned.
const char *player_names_shown(int uid, int self, const char *name, char *out, size_t size);
// The account's own name (menu, account window, the local player's rows): "Player" when hidden.
const char *player_names_account(const char *name);
// The clan tag to show: "" when hidden (NULL too), else `clan`.
const char *player_names_clan(const char *clan);
// A player the client first sees in the match (SPAWN, with its name): numbered now, in that order. Repeats are
// no-ops except that the name is kept as the newest one.
void player_names_see(int uid, const char *name);
// The i-th player numbered in this match, the ones who left too (the chat rewrite of their leave line): 1 with
// its uid (-1: numbered by name) and name, 0 past the last.
int player_names_entry(int index, int *uid, const char **name);
// The match is left (client_leave_game): the numbers start again.
void player_names_reset(void);
// The setting (client_settings_hide_player_names): 1 while names are hidden.
int player_names_hidden(void);
// Another player's extras (account level, verified / premium / creator / custom badges, the scoreboard stats,
// the killed-by card's level and player card stat): hidden while names are hidden, except for the local player.
int player_names_extras_hidden(int self);
// Whole-word match of the raw name at text[pos] (no word character right before or after it, where the name's
// own edge is a word character): the name's length, else 0. Used to rewrite the names in a chat line.
size_t player_names_word_at(const char *text, size_t pos, const char *raw);
