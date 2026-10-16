// v10 gameState and its "gsc" packet (game.deob.js setGameState :191761-191792): the phase of
// the match, sent to every client on each change (NET_EXT_GAME_STATE, net_ext.h 0x0901). The
// phases are driven by game.c (INITIALIZING at a round's start, POST_MATCH at its end, ENDED when
// the end screen runs out) and mode_round.c (IN_PROGRESS when the wait timers drain).
#include <shared.h>
#include <net_ext.h>
#include <string.h>

void game_set_state(Game *game, const GameState state) {
    if (!game || game->game_state == (unsigned char) state) return;
    game->game_state = (unsigned char) state;

    NetGameStatePacket packet = {.state = (uint8_t) state};
    uint8_t buf[NET_EXT_HEADER_SIZE + sizeof(packet)];
    const uint16_t n = net_ext_pack(buf, sizeof(buf), NET_EXT_GAME_STATE, &packet, sizeof(packet));
    if (n) game_send(game, NET_TARGET_ALL, NET_PACKET_EXT, buf, n);
    if (game->hooks.state_changed) game->hooks.state_changed(game, state);
}

// v10 isUsingRankedNames: a ranked match's names are hidden until the match starts (the roster
// names come in with the IN_PROGRESS push, server/src/net.c).
int game_names_masked(const Game *game) {
    return game && game->is_ranked && game->game_state <= GAME_STATE_WARMUP;
}
