/*
 * services/brain/v10_market.h - the v10 REST routes of the market, items, trades and players (answered
 * from the social socket's own handlers, v10_market.c), and the moderation, mods and KPD routes that
 * refuse or answer empty.
 */
#ifndef KN_BRAIN_V10_MARKET_H
#define KN_BRAIN_V10_MARKET_H

#include "brain_internal.h"

int v10_market_init(Brain *b);

#endif
