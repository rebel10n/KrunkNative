#pragma once
// Backend account data the game server sends about players (doc/backend.md section 8):
// NET_PACKET_PLAYER_INFO (level, clan, premium / verified / dev flags per player uid) for the
// nametags, scoreboard and kill card, and NET_PACKET_BILLBOARD (v3.1.1 init "bill":
// game.updateBillboards txt / txtCol / bgCol) for the "sad" billboard canvases.
// Servers without a brain send neither: every player is a guest and billboards keep the
// engine defaults ("Your Ad Here", #e3e3e3 on #000000).
#include <shared.h>

void player_accounts_on_info(const NetPlayerInfoPacket *info);
// NULL when the server sent nothing for `uid` (guest / standalone server).
const NetPlayerInfoPacket *player_accounts_get(int uid);

void player_accounts_on_billboard(const NetBillboardPacket *billboard);
// The active billboard (defaults filled in). Never NULL.
const NetBillboardPacket *player_accounts_billboard(void);

// INIT (new session or round): infos are resent with the spawns; a server that sent no
// BILLBOARD before this INIT gets the defaults back.
void player_accounts_on_init(void);
