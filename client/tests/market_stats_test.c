// client/include/market_stats.h (v10_market.c Stats tab): a database error at busy times comes back
// as a null reply, and the tab must keep the rows it shows instead of going empty ("the item Stats
// tab showing no data at busy times", Krunker v10.0.3).
//   cmake-build-debug/market_stats_test
#include <market_stats.h>

#include <stdio.h>
#include <string.h>

static int g_fail, g_pass;
#define CHECK(cond, ...) do { if (cond) g_pass++; else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void) {
    cJSON *shown = NULL;

    // a reply of rows replaces the shown rows
    cJSON *rows = cJSON_Parse("[{\"ski\":4,\"onS\":2}]");
    CHECK(market_stats_apply(&shown, rows) == 1, "rows applied");
    CHECK(shown && cJSON_GetArraySize(shown) == 1, "one row shown");
    cJSON_Delete(rows);

    // a database error (null) keeps the rows already shown
    cJSON *err = cJSON_Parse("null");
    CHECK(market_stats_apply(&shown, err) == 0, "error reply is not applied");
    CHECK(shown && cJSON_GetArraySize(shown) == 1, "error reply keeps the shown rows (size %d)", shown ? cJSON_GetArraySize(shown) : -1);
    CHECK(err == NULL || cJSON_IsNull(err), "error reply untouched");
    cJSON_Delete(err);

    // an error reply before any rows arrived leaves the tab empty and without a crash
    cJSON *none = NULL;
    cJSON *null_reply = cJSON_CreateNull();
    CHECK(market_stats_apply(&none, null_reply) == 0 && none == NULL, "error with no rows keeps none");
    cJSON_Delete(null_reply);

    // an empty array is a real answer ("no skins"): it replaces the rows
    cJSON *empty = cJSON_Parse("[]");
    CHECK(market_stats_apply(&shown, empty) == 1 && cJSON_GetArraySize(shown) == 0, "empty array replaces the rows");
    cJSON_Delete(empty);

    cJSON_Delete(shown);
    printf("market_stats_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
