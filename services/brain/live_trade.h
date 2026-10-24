/*
 * services/brain/live_trade.h - the live trade of Trade mode (doc/v10/trade.md): the game server's two "acct" ops,
 * implemented in economy.c (rules, tradability) and db_economy.c (P_LIVE_TRADE_OPEN / P_LIVE_TRADE, one transaction).
 */
#ifndef KN_BRAIN_LIVE_TRADE_H
#define KN_BRAIN_LIVE_TRADE_H

#include "accounts.h"

/* trade_inventories {a, b} -> {a: {inv, funds, premium, level, notrade}, b: {...}} */
int econ_live_trade_inventories(const cJSON *args, AcctReplyFn cb, void *ud);
/* live_trade {key, a: {id, items, kr}, b: {id, items, kr}, cost} -> {id, a: {funds, removed, added, meta, owned}, b} */
int econ_live_trade(const cJSON *args, AcctReplyFn cb, void *ud);

#endif
