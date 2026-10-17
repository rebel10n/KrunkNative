#pragma once
// Hide player names (Client settings "Hide player names", knClient hidePlayerNames, default off):
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

// The name to show for a player. uid: its uid (-1: no uid, numbered by name). self: the local player.
// Not hidden: `name` itself. Hidden: "Player" (self) or "Player_<n>", written to `out` (size bytes).
const char *player_names_shown(int uid, int self, const char *name, char *out, size_t size);
// The account's own name (menu, account window, the local player's rows): "Player" when hidden.
const char *player_names_account(const char *name);
// The clan tag to show: "" when hidden (NULL too), else `clan`.
const char *player_names_clan(const char *clan);
// A player the client first sees in the match (SPAWN): numbered now, in that order. Repeats are no-ops.
void player_names_see(int uid);
// The match is left (client_leave_game): the numbers start again.
void player_names_reset(void);
// The setting (client_settings_hide_player_names): 1 while names are hidden.
int player_names_hidden(void);
