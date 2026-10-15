#pragma once
// Backend account data the game server sends about players (doc/backend.md section 8):
// NET_PACKET_PLAYER_INFO (level, clan, premium / verified / dev flags per player uid) for the
// nametags, scoreboard and kill card. Servers without a brain send none: every player is a
// guest. The server's billboard ad (NET_PACKET_BILLBOARD) is client/include/billboard_ad.h.
#include <shared.h>

void player_accounts_on_info(const NetPlayerInfoPacket *info);
// NULL when the server sent nothing for `uid` (guest / standalone server).
const NetPlayerInfoPacket *player_accounts_get(int uid);
// 1 when the server said `uid` is a bot player (NET_PLAYER_INFO_BOT); 0 for humans and unknown uids.
int player_accounts_is_bot(int uid);

// INIT (new session or round): infos are resent with the spawns.
void player_accounts_on_init(void);
