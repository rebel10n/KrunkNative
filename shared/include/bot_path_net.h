#pragma once
// NET_EXT_KN_BOT_PATH (shared/include/net_ext.h): the spectated bot's route, goal and target
// memory as the server sends them to a spectator following it (server/src/net.c
// net_broadcast_bot_paths) and the client decodes them (client/src/nav_overlay.c). Shared so
// both sides agree on the body and the unit test (client/tests/nav_overlay_test.c) covers both.
#include <net_ext.h>
#include <bots.h>

// Fills *out from `bot`: bot_path_current() / bot_path_goal() (bot_path.h), its state and its
// target memory (the target's live position through game_find_player when `game` is given).
// Returns the body length (NET_BOT_PATH_SIZE(n)), 0 when the bot has no player.
uint16_t bot_path_packet_fill(Game *game, const GameBot *bot, NetBotPathPacket *out);

// Decodes a received body into *out (points past n zeroed): 1, or 0 when it is shorter than
// its head, cut inside a point, or claims more points than NET_BOT_PATH_POINTS_MAX.
int bot_path_packet_decode(const void *body, uint16_t length, NetBotPathPacket *out);
