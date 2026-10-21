/*
 * services/brain/v10_empty.h - v10 REST routes that have no KrunkNative data behind them: the
 * empty shape v10 expects for reads, a v10 refusal for writes (v10_empty.c).
 */
#ifndef KN_BRAIN_V10_EMPTY_H
#define KN_BRAIN_V10_EMPTY_H

#include "brain_internal.h"

int v10_empty_init(Brain *b);

#endif
