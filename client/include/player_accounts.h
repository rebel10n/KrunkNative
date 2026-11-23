#pragma once
// Backend account data the game server sends about players (doc/backend.md section 8):
// NET_PACKET_PLAYER_INFO (level, clan, premium / verified / dev flags per player uid) for the
// nametags, scoreboard and kill card. Servers without a brain send none: every player is a
// guest. The server's billboard ad (NET_PACKET_BILLBOARD) is client/include/billboard_ad.h.
#include <net_ext.h> // NetPlayerRankFlagPacket
#include <shared.h>

void player_accounts_on_info(const NetPlayerInfoPacket *info);
// NULL when the server sent nothing for `uid` (guest / standalone server).
const NetPlayerInfoPacket *player_accounts_get(int uid);
// NET_EXT_KN_PLAYER_RANK_FLAG: v10's rankedRank / flagIndex of a player whose PLAYER_INFO came first (else ignored).
void player_accounts_on_rank_flag(const NetPlayerRankFlagPacket *packet);
// ADD_PLAYERS' rankedRank (shared/include/ranked.h tier) of `uid`, 0 when none was sent (guests, bots, older servers).
int player_accounts_ranked_rank(int uid);
// ADD_PLAYERS' flagIndex (client/src/ui/account_flags.h) of `uid`, -1 when none.
int player_accounts_flag(int uid);
// 1 when the server said `uid` is a bot player (NET_PLAYER_INFO_BOT); 0 for humans and unknown uids.
int player_accounts_is_bot(int uid);

// INIT (new session or round): infos are resent with the spawns.
void player_accounts_on_init(void);
