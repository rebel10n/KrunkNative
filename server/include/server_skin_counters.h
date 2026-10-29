#pragma once
// v10.0.4 counter skins on the server (server/src/skin_counters_server.c). A counter skin (store.skins attach type 1,
// deob-10.0.4.js:172726) counts one stat (attach.stat, "kills" by default, :106489-106492) with its weapon. The server is
// the authority: countSkinStat's server branch adds to player.skinStats[copy] (:175889-175893), takeSkinStats saves them
// with the match into the copy's m[stat] (:191561-191585), and a server-only stat (candy) is sent to the owner as
// SKIN_STAT (:184308-184311). Box Office keeps the stat per store index (the brain's skin_stats, 0365).
#include <server.h>
#include <cJSON.h>

// The stat (index in SKIN_STATS) a store skin counts, -1 when it is not a counter skin (assets/skins.json, read once).
int skin_counters_stat_of(int skin);
// The account's counter skins from its owned list (account.data[10] / skins: {ind, cnt[, id, m]}): m[stat] of the first
// entry of each counter skin's index is its base; and each index's copy seed (the first entry with an id or m.seed).
// Called by accounts_load_skins.
void skin_counters_load(ClientConnection *client, const cJSON *skins);
// The counter base of each worn skin, PlayerSkins order, -1 = none, and each worn copy's seed, 0 none
// (NET_EXT_KN_SKIN_COPIES). seed may be NULL.
void skin_counters_bases(const ClientConnection *owner, const PlayerSkins *skins, int32_t base[3], int32_t seed[3]);
// NET_EXT_KN_SKIN_COPIES of `player` to `to` (an EXT peer), after its SKINS. Nothing when no worn copy has a base or a seed.
void skin_counters_send(ClientConnection *to, const ClientConnection *owner, const Player *player);
// takeSkinStats (deob-10.0.4.js:191561-191585): the counts since the last take as [{skin, stat, count}] (NULL none),
// folded into each counter's base, when `save` (an account in a public-type game, not a hacker: the caller's check);
// the counts are dropped either way (skinStats = {}).
cJSON *skin_counters_take(ClientConnection *client, int save);
// countSkinStat (GameHooks.skin_stat) on the server's game.
void skin_counters_install_game_hooks(Game *game);
