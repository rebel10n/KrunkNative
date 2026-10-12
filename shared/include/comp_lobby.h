#pragma once
// Krunker v7.1.4 comp lobby: game.lobbyPlayers of a comp game (game.isComp, config.hostTypes[1]
// "Comp Server Setup", and every ranked match: services/brain/ranked.c). src/data/game.js
// syncLobbyP / updLobbyP / getLobbyDat / canSpawn; index.js io_syncPConData ("cd"),
// io_syncPConObj ("cda"), updSyncDat ("cd" key, value), f1892 (the comp menu's lists).
//
// Every connection of a comp lobby has an entry keyed by its id (v7 lobbyId = player.id) with
// its account identity, its team `t` (selTeam: chosen in the comp menu, at most tmSize + 1 per
// team) and its class `c` (limitClasses: at most classLim<c> per team). A player spawns only
// with a team and a class (canSpawn) and spawns on them (getLobbyDat "t" / "c"). The authority
// (server / offline sandbox) owns the table and sends it as NET_PACKET_COMP_LOBBY; clients
// mirror it for the comp menu (client/src/ui/comp_ui.c).
#include <shared.h>

#define COMP_LOBBY_MAX 64

typedef NetCompLobbyEntry CompLobbyEntry;

typedef struct CompLobby_t {
    CompLobbyEntry entries[COMP_LOBBY_MAX];
    int count;
} CompLobby;

// The entry of connection `uid`, NULL none.
const CompLobbyEntry *comp_lobby_get(const Game*, int uid);
// syncLobbyP(player, id, 0): `uid` joined (or rejoined) a comp lobby. `identity` carries the
// account fields (i, n, v, cl, l, f); team / class of an existing entry are kept. Sends the whole
// table to `uid` ("cd") and the entry to the others ("cda"). No-op outside a comp game.
void comp_lobby_join(Game*, int uid, const CompLobbyEntry *identity);
// syncLobbyP(player, id, 1): `uid` left: a = 0, t / c dropped (rej when it had a team), "cda" null.
void comp_lobby_leave(Game*, int uid);
// updLobbyP(player, id, key, value): 't' (selTeam, tmSize + 1 per team) or 'c' (limitClasses:
// classLim<c> per team). Returns 1 when the value changed (and was broadcast).
int comp_lobby_update(Game*, int uid, char key, int value);
// The authority's own team pick (a ranked roster team): no selTeam / size check.
void comp_lobby_set_team(Game*, int uid, int team);
// canSpawn(playerId): NULL when `uid` may spawn, else v7's message.
const char *comp_lobby_spawn_block(const Game*, int uid);
// The whole table ("cd") to `uid`, or NET_TARGET_ALL.
void comp_lobby_send_all(Game*, int uid);
// Client: apply a NET_COMP_LOBBY packet to the mirrored table.
void comp_lobby_apply(Game*, const NetCompLobbyPacket*);
// Count of active entries on `team` with class `class_index` (the comp menu's "n/limit").
int comp_lobby_class_count(const Game*, int team, int class_index);
void comp_lobby_free(Game*);
