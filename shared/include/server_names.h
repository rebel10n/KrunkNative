#pragma once
// Game ids and their short link form (doc/backend.md "Lobby tuple"). Pure C,
// shared by kn_brain (services/brain/registry.c resolves both forms) and the client
// (client/src/ui/servers_ui.c writes the short form into ?game= / invite links).
//
// A game id is "<nodeId>:<lobbyId>" (the brain namespaces the node's local lobby id). The
// node id of an edge server is "<EDGE><n>" (deploy/ansible kn_gameserver server.cfg.j2:
// CHITOWN2), its region the 2-letter code (CH). Links use the short node id:
//   - region is 2 letters and the node id's stem (without its trailing digits) is longer
//     than 2 chars: region code + instance number   CHITOWN2 / CH  -> CH2
//   - otherwise a stem of up to 4 chars stays as is  NY2, LOC, EDGE3
//   - longer stems keep their first 3 letters/digits KRUNKCLUB1     -> KRU1
// so ?game=CHITOWN2:kz862 is written ?game=CH2:kz862. The canonical id stays what the
// brain, the game server (JOIN game_id) and the join ticket use; both forms resolve.
#include <stddef.h>

#define KN_SHORT_NODE_MAX 48

// The short node id (always NUL-terminated, never contains ':'). NULL region = none.
void kn_node_short_id(const char *node_id, const char *region, char *out, size_t n);

// "<short node>:<lobby>" for a canonical game id of a node in region. An id without ':'
// (a finder "host:port" is not a game id: callers skip those) is copied as is.
void kn_game_link_id(const char *game_id, const char *region, char *out, size_t n);

// 1 when id (canonical or short, the short node part case-insensitive) names the canonical
// game id game_id of a node in region.
int kn_game_id_matches(const char *id, const char *game_id, const char *region);

// v3.1.1 f9(text): a game link "[http(s)://][www.]<site>/?game=<id>" (%XX / '+' decoded) or a
// bare id "NODE:lobby" (/^([A-Z]+):(\w+)$/ widened to node ids with digits: CHITOWN2:kz862,
// CH2:kz862) -> id. Surrounding blanks are ignored. 0 (id = "") when text is neither.
int kn_game_link_parse(const char *text, char *id, size_t n);

// 1 when text is a game link (kn_game_link_parse) whose site is krunker.io or *.krunker.io: an
// official krunker.io lobby, joined through the official path (krunker_lobbies_open), not the brain.
int kn_game_link_is_krunker(const char *text);
