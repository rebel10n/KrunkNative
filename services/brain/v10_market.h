/*
 * services/brain/v10_market.h - v10 REST routes that have no KrunkNative data behind them: the
 * empty shape v10 expects for reads, a v10 refusal for writes (v10_market.c).
 */
#ifndef KN_BRAIN_V10_MARKET_H
#define KN_BRAIN_V10_MARKET_H

#include "brain_internal.h"

int v10_market_init(Brain *b);

#endif
