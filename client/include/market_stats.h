#pragma once
#include <cJSON.h>

// The Market's Stats tab reply (v10_market.c, ["0","market","stats", rows]): one row per skin. The
// brain answers a database error with null (services/brain economy.c market_done). An error is not
// "no skins": the rows already shown stay, and the next open of the tab asks again. Returns 1 when
// the rows were replaced, 0 for an error reply (`*slot` unchanged).
static inline int market_stats_apply(cJSON **slot, const cJSON *reply) {
    if (!cJSON_IsArray(reply)) return 0;
    cJSON_Delete(*slot);
    *slot = cJSON_Duplicate(reply, 1);
    return 1;
}
