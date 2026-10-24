/*
 * services/brain/node_events.h - what KrunkNative's game servers need from the brain to send Krunker v10.0.3's
 * WARS_CLAN, SPIN_WHEEL_RESPONSE / UNBOX_MESSSAGE_S, UNBOX_MESSAGE, LOTTERY_PRIZE_WON and CHARGE_CONSENT_REQUEST
 * the way 10.0.3's own game server does (doc/v10/s2c-coverage.md "KN's server"; the game server's side is
 * server/src/brain_events.c).
 *
 * Game server -> brain: "acct" ops of the internal WS (accounts_internal_call routes them to node_events_op)
 *   turf_clan     {map}                      -> the clan holding the map today {clan_id, clan_name, clan_logo,
 *                                               clan_banner_color} or null (turf.c, db_turf.c view "controller")
 *   map_spin      {id, wheel}                -> {skin, rand, funds, rarity, wheel, price}: one KR spin of a store wheel
 *                                               for a map PRIZE_WHEEL, KR and skin in one transaction (store.c)
 *   studio_game   {map}                      -> {map_id, studio_id, monetized}: game.isMonetized() of a published map
 *   studio_charge {id, map, amount, key}     -> {success, krBalance, charged, earned, duplicate}: the KR of a consented
 *                                               PAYMENTS charge / transact, key idempotent (db_studios.c)
 *
 * Brain -> game server: pushes to the node where the account plays (game_presence.c: this brain's nodes)
 *   unbox       {accountId, item, kr, rarity}               a store / Free KR spin's win ("unb")
 *   raffle-win  {accountId, lotteryId, drawId, prize: {type, ref, quantity}}   a lottery prize ("lotWin")
 */
#ifndef KN_BRAIN_NODE_EVENTS_H
#define KN_BRAIN_NODE_EVENTS_H

#include <stdint.h>

#include "accounts.h"
#include "cJSON.h"

/* accounts_internal_call: 1 when op is one of the ops above (cb called now or later), 0 otherwise */
int node_events_op(const char *op, const cJSON *args, AcctReplyFn cb, void *ud);

/* turf.c / store.c: their ops (node_events_op dispatches) */
int turf_clan_op(const cJSON *args, AcctReplyFn cb, void *ud);
int store_map_spin_op(const cJSON *args, AcctReplyFn cb, void *ud);

/* A spin's win to the account's game server: item = store.skins index (kr 0), or the store.freeKR index with the KR
 * won (kr > 0); rarity = the item's (freeKR: the index). Nothing when the account plays nowhere on this brain. */
void node_events_unbox(int64_t account_id, int item, int kr, int rarity);
/* A lottery prize to the winner's game server (type "skin" | "currency" | "subscription", ref as text). */
void node_events_raffle_win(int64_t account_id, int64_t lottery_id, int64_t draw_id, const char *type, const char *ref,
                            double quantity);

#endif
