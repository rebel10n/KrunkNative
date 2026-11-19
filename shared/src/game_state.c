// v10 gameState and its "gsc" packet (game.deob.js setGameState :191761-191792): the phase of
// the match, sent to every client on each change (NET_EXT_GAME_STATE, net_ext.h 0x0901). The
// phases are driven by game.c (INITIALIZING at a round's start, POST_MATCH at its end, ENDED when
// the end screen runs out) and mode_round.c (IN_PROGRESS when the wait timers drain).
#include <shared.h>
#include <net_ext.h>
#include <raid_drops.h> // v10.0.4 isDoubleRaidDrops
#include <ks_game.h>    // ks_wall_ms (the wall clock for the event flags)
#include <stdio.h>
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

// v10.0.7 "Anonymous players now stay anonymous": the name an account viewer sees for an anonMode account player.
// Krunker's server sets it (the lobby record's "an", the player's fakeName: getName, deob-10.0.7.js:171688;
// lobbyPayload :190985); the assignment is not in the shipped bundle (fakeName is only initialised, :171331). The
// form is Krunker's own: the live i18n's anonymous tip addresses the player as "Anonymous_1" (generic.tips.anonymous,
// deob-10.0.7.js:204284 and krunker.io/i18n/en.js?v=5fHFc, fetched 2026-11-19), and public descriptions of
// Anonymous Mode say it shows as Anonymous plus a number (en.namu.wiki/w/Krunker, as quoted by search results
// 2026-11-19). The number Krunker picks is server-side; KN uses the player's join-order number (Player.name_no,
// Krunker's per-game sid counterpart, the n of "Player_<n>").
void game_anon_alias(const Player *player, char *out, const size_t size) {
    if (!out || !size) return;
    if (player && player->name_no > 0) snprintf(out, size, "Anonymous_%d", player->name_no);
    else snprintf(out, size, "Anonymous");
}

// getName's guest-viewer form (deob-10.0.7.js:171688: no viewer account -> 'Player_' + this.sid; the add-player
// sync, chat and end board pass the viewer's account, :176753, :177091, :195257): what a viewer without an account
// sees for an account player. Krunker's wiki says the same ("guests cannot see the usernames of accounts in game,
// they show up as Player_1, Player_2", krunkerio.fandom.com/wiki/Accounts, as quoted by search results 2026-11-19).
void game_guest_view_name(const Player *player, char *out, const size_t size) {
    if (!out || !size) return;
    if (player && player->name_no > 0) snprintf(out, size, "Player_%d", player->name_no);
    else snprintf(out, size, "Player");
}

// v10 isUsingRankedNames: a ranked match's names are hidden until the match starts (the roster
// names come in with the IN_PROGRESS push, server/src/net.c).
int game_names_masked(const Game *game) {
    return game && game->is_ranked && game->game_state <= GAME_STATE_WARMUP;
}
