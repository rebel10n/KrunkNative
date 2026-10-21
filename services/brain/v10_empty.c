/*
 * services/brain/v10_empty.c - v10 REST routes with no KrunkNative data behind them (filled in
 * per doc/v10/api-inventory.md).
 */
#include "v10_empty.h"

#include "v10_api.h"

int v10_empty_init(Brain *b) {
    (void) b;
    return 0;
}
