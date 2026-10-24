#pragma once
// The server's KrunkScript host (doc/v7/krunkscript.md 2.1-2.8, 3.x server entries, 4.1 server
// hooks, 6.5): g_server->game runs the map's scripts.server. server/src/ks_server_host.c, part of
// the server core, so the offline sandbox (sandbox.c) runs the same code. Every function runs
// with g_server->lock held (or before the net thread starts / after it stopped).
#include <server.h>

// After every game_init (startup, map change, next round): a new or changed map script is
// (re)loaded (onServerClosed for the old one), the same one keeps its globals (2.2); then
// start(). No-op on a map without scripts.server.
void ks_server_round_init(void);
// After game_tick: update(delta) and, at the state rate, the custom sync values (2.6).
void ks_server_tick(float delta);
// Server exit / sandbox stop: onServerClosed, then the script and its state are freed.
void ks_server_shutdown(void);

// Connection events from server/src/net.c
void ks_server_on_hello(ClientConnection*, const NetPacket*);       // records the peer's protocol minor
void ks_server_on_leave(ClientConnection*);                         // onPlayerLeave (player removed or spectating)
void ks_server_on_disconnect(ClientConnection*);                    // drops the per-connection state
void ks_server_on_chat(ClientConnection*, const char *text);        // onChatMessage (accepted, not a command)
void ks_server_on_script_msg(ClientConnection*, const NetPacket*);  // NET_PACKET_SCRIPT_MSG -> onNetworkMessage
// NET_EXT_CHARGE_CONSENT_RESPONSE "chrsp" [id, accept]: GAME.PAYMENTS' consent (game.resolveChargeConsent)
void ks_server_charge_response(ClientConnection*, const uint8_t *body, uint16_t length);

// 1 when the connection announced NET_PROTOCOL_MINOR_V7 in its HELLO: it understands the
// KrunkScript packets (NET_PACKET_BLOCK_KRUNKSCRIPT).
int ks_server_peer_v7(const ClientConnection*);

// Test hook: the wall clock (ms) of the script's TIME.now and rate-limit windows; NULL = real.
void ks_server_set_clock(double (*now_ms)(void));
