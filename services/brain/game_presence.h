/*
 * services/brain/game_presence.h - which game each signed-in account is playing in
 * (Krunker v10.0.2's event-bus "presence:game": a friend's gameInstanceId and whether that
 * game needs a password, shown as the social panel's "Join <name>'s match" button and the
 * profile window's Friends tab "Join").
 *
 * Fed by the game servers connected to this brain (internal_ws.c: player-join / player-leave
 * with accountId + lobbyId, a node's disconnect). In memory, this brain's nodes only: with
 * brain.federation a player on another brain's node shows no game (the deployment runs one
 * coordinator, doc/backend.md 6). On a change the account's friends (mutual follows, v10's
 * friends list) get ["eb", {"type":"presence:game","id":<accountId>,"gameInstanceId":
 * "<gameId>"|null,"gamePasswordProtected":bool}] on their social sockets (social_push_account:
 * this brain's sockets and, with the Redis hot tier, the others').
 */
#ifndef KN_BRAIN_GAME_PRESENCE_H
#define KN_BRAIN_GAME_PRESENCE_H

#include <stddef.h>
#include <stdint.h>

#include "brain.h"

/* internal_ws.c: a player of a node's lobby is in (READY) / left. lobby_id is the node's own
 * lobby id or a full "<nodeId>:<lobby>" game id. account_id <= 0 (guests) is ignored. */
void game_presence_join(Brain *b, const char *node_id, const char *lobby_id, int64_t account_id);
void game_presence_leave(Brain *b, const char *node_id, const char *lobby_id, int64_t account_id);
/* internal_ws.c: the node disconnected: its players are in no game any more. */
void game_presence_node_gone(Brain *b, const char *node_id);

/* 1 when the account plays in a game of this brain's nodes: its game id ("<nodeId>:<lobby>",
 * what /mm/seek-game?game= and the client's switchServer take) into game_id and whether the
 * game needs a password (registry_lobby_protected) into *password_protected (may be NULL).
 * 0 = in no game (game_id ""). The social friends list calls it for each friend. */
int game_presence_get(int64_t account_id, char *game_id, size_t n, int *password_protected);

#endif
