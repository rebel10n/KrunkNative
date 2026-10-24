#pragma once
// The live trade of Trade mode on the server (server/src/trade.c, doc/v10/trade.md section 3): the
// requests ("strd", int [5, sid]), the sessions (trade_session.h), their packets and the brain ops
// that load the inventories ("trade_inventories") and move the items ("live_trade").
#include <server.h>
#include <cJSON.h>
#include <stdint.h>

// NET_PACKET_EXT subtypes NET_EXT_TRADE_REQUEST .. NET_EXT_TRADE_CHAT; 1 when it was one
int trade_handle_ext(ClientConnection *client, uint16_t subtype, const uint8_t *body, uint16_t length);
// "int" [5, sid]: the interact key on another player's trade prompt (interact type GAME_INTERACT_TRADE)
void trade_handle_interact(ClientConnection *client, int target_uid);
// the connection leaves (net_remove_client): its requests go, its session ends (a settle in flight finishes)
void trade_on_leave(ClientConnection *client);
// every server tick: request expiry, the inventory timeout, the countdown, mode / round changes
void trade_tick(float now, float dt);
// an "acct-r" frame of the brain; 1 when it answered one of ours
int trade_on_brain_frame(const cJSON *frame);
// the brain link dropped: sessions waiting for their inventories end (their replies never come)
void trade_link_down(void);
// 1 while the connection is in a session (any state)
int trade_is_trading(const ClientConnection *client);
// tests: where the brain frames go (default brain_post; returns 0 sent, -1 dropped). NULL restores it.
void trade_set_post_hook(int (*post)(cJSON *frame, int reliable));
