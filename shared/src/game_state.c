// v10 gameState and its "gsc" packet (game.deob.js setGameState :191761-191792): the phase of
// the match, sent to every client on each change (NET_EXT_GAME_STATE, net_ext.h 0x0901). The
// phases are driven by game.c (INITIALIZING at a round's start, POST_MATCH at its end, ENDED when
// the end screen runs out) and mode_round.c (IN_PROGRESS when the wait timers drain).
#include <shared.h>
#include <net_ext.h>
#include <raid_drops.h> // v10.0.4 isDoubleRaidDrops
#include <ks_game.h>    // ks_wall_ms (the wall clock for the event flags)
#include <string.h>

static int64_t (*g_event_clock)(void); // tests: a fixed "now"

void game_set_event_clock(int64_t (*now_ms)(void)) {
    g_event_clock = now_ms;
}

static int64_t event_now_ms(void) {
    if (g_event_clock) return g_event_clock();
    return (int64_t) ks_wall_ms(); // the shared wall clock (ks_game.c), portable to MinGW
}

// updateEventFlags (deob-10.0.4.js:195384-195385): isDoubleRaidDrops = isPubType() && a Saturday or Sunday (UTC). Called at
// a round's start (startGame, :193368: the INITIALIZING state here) and whenever the state is set to IN_PROGRESS
// (setGameState, :195391; the wait timers draining, :195001, set it), on the authority only (isServer || sandbox)
void game_update_event_flags(Game *game) {
    if (!game || !game_is_authority(game)) return;
    game->double_raid_drops = (unsigned char) raid_double_drops(game_is_pub_type(game), event_now_ms());
}

void game_set_state(Game *game, const GameState state) {
    if (!game) return;
    // v10 setGameState: `state === IN_PROGRESS && updateEventFlags()` before the unchanged check; startGame's own call
    if (state == GAME_STATE_IN_PROGRESS || state == GAME_STATE_INITIALIZING) game_update_event_flags(game);
    if (game->game_state == (unsigned char) state) return;
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
