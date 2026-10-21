/*
 * services/brain/v10_market.c - v10 REST routes with no KrunkNative data behind them (filled in
 * per doc/v10/api-inventory.md; the market, items, trades, moderation, mods and KPD family).
 */
#include "v10_market.h"

#include "v10_api.h"

int v10_market_init(Brain *b) {
    (void) b;
    return 0;
}
