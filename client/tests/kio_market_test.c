// kio_market: a krunker.io account's market paths and its answers as the economy's rows (client/src/ui/kio_market.c):
// the History rows, the item view's listings, the sales charts' rows and paths, the Black Market body and reply.
#include <kio_market.h>
#include <stdio.h>
#include <string.h>

static int failures = 0, checks = 0;
#define CHECK(cond, ...)                                       \
    do {                                                       \
        checks++;                                              \
        if (!(cond)) {                                         \
            failures++;                                        \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);        \
            printf(__VA_ARGS__);                               \
            printf("\n");                                      \
        }                                                      \
    } while (0)

static double n(const cJSON *o, const char *k) { return cJSON_GetNumberValue(cJSON_GetObjectItem(o, k)); }
static const char *s(const cJSON *o, const char *k) {
    const char *v = cJSON_GetStringValue(cJSON_GetObjectItem(o, k));
    return v ? v : "";
}

int main(void) {
    char path[128];

    // the dates: epoch ms as they are, ISO 8601 in UTC (new Date(x))
    cJSON *t = cJSON_CreateString("2026-11-20T10:00:00.000Z");
    CHECK(kio_market_time_ms(t) == 1795168800000.0, "ISO date -> epoch ms (%.0f)", kio_market_time_ms(t));
    cJSON_Delete(t);
    t = cJSON_CreateNumber(1700000000000.0);
    CHECK(kio_market_time_ms(t) == 1700000000000.0, "epoch ms kept");
    cJSON_Delete(t);
    t = cJSON_CreateString("soon");
    CHECK(kio_market_time_ms(t) == 0, "not a date: 0");
    cJSON_Delete(t);

    // History: GET /market/history?sort=date_desc&limit=50 (deob-10.0.7.js:135786-135790) -> the table's rows, other_party_name as Who
    CHECK(kio_market_history_path(path, sizeof(path)) && !strcmp(path, "/market/history?sort=date_desc&limit=50"), "history path %s", path);
    cJSON *hist = cJSON_Parse("{\"history\":[{\"type\":\"buy\",\"date\":\"2026-11-20T10:00:00.000Z\",\"skinindex\":1,\"amount\":777,"
                              "\"other_party_name\":\"KioSeller\",\"status\":\"completed\"},"
                              "{\"type\":\"sell\",\"date\":1700000000000,\"skinindex\":\"5\",\"amount\":\"20\",\"other_party_name\":null,"
                              "\"status\":\"cancelled\"},{\"type\":\"buy\",\"amount\":3}],\"has_more\":false,\"next_cursor\":null}");
    cJSON *rows = kio_market_history_rows(hist);
    CHECK(cJSON_GetArraySize(rows) == 2, "a row with no skinindex is dropped (%d rows)", cJSON_GetArraySize(rows));
    const cJSON *r0 = cJSON_GetArrayItem(rows, 0), *r1 = cJSON_GetArrayItem(rows, 1);
    CHECK(!strcmp(s(r0, "type"), "buy") && n(r0, "date") == 1795168800000.0 && n(r0, "skinindex") == 1 && n(r0, "amount") == 777 &&
              !strcmp(s(r0, "who"), "KioSeller") && !strcmp(s(r0, "status"), "completed"),
          "history row 0 is {type, date ms, skinindex, amount, who, status}");
    CHECK(n(r1, "skinindex") == 5 && n(r1, "amount") == 20 && !strcmp(s(r1, "who"), "") && n(r1, "date") == 1700000000000.0,
          "numeric strings read, no other party -> empty Who");
    cJSON_Delete(rows);
    cJSON_Delete(hist);
    rows = kio_market_history_rows(NULL);
    CHECK(cJSON_IsArray(rows) && cJSON_GetArraySize(rows) == 0, "no data: no rows");
    cJSON_Delete(rows);

    // the item view and the charts' paths (v10:26957, 26895, 26963)
    CHECK(kio_market_view_path(path, sizeof(path), 42) && !strcmp(path, "/items/42/view"), "view path %s", path);
    CHECK(kio_market_view_path(path, sizeof(path), -1) == 0 && !path[0], "no item, no view path");
    CHECK(kio_market_sales_path(path, sizeof(path), 42, 30) && !strcmp(path, "/market/items/42/sales-statistics?daysback=30"),
          "days: sales-statistics (%s)", path);
    CHECK(kio_market_sales_path(path, sizeof(path), 42, 7) && !strcmp(path, "/market/items/42/sales-statistics?daysback=7"),
          "7 days is days, not a range (%s)", path);
    CHECK(kio_market_sales_path(path, sizeof(path), 42, 3) && !strcmp(path, "/items/42/sales?range=3"), "a range index: items sales (%s)", path);
    CHECK(kio_market_sales_path(path, 8, 42, 3) == 0 && !path[0], "a path that does not fit is refused");

    // the sales rows: d cut to the day
    cJSON *sales = cJSON_Parse("[{\"d\":\"2026-11-20\",\"f\":2000,\"t\":4},{\"d\":\"2026-11-21T00:00:00.000Z\",\"f\":\"30\",\"t\":\"1\"},{\"f\":1}]");
    rows = kio_market_sales_rows(sales);
    CHECK(cJSON_GetArraySize(rows) == 2, "a sales row with no day is dropped");
    CHECK(!strcmp(s(cJSON_GetArrayItem(rows, 0), "d"), "2026-11-20") && n(cJSON_GetArrayItem(rows, 0), "f") == 2000 &&
              n(cJSON_GetArrayItem(rows, 0), "t") == 4, "sales row 0 kept");
    CHECK(!strcmp(s(cJSON_GetArrayItem(rows, 1), "d"), "2026-11-21") && n(cJSON_GetArrayItem(rows, 1), "f") == 30 &&
              n(cJSON_GetArrayItem(rows, 1), "t") == 1, "an ISO day is its date, numeric strings read");
    cJSON_Delete(rows);
    cJSON_Delete(sales);

    // the view's listings -> {m, i, f, p, ms}, the item's only, cheapest first
    cJSON *lst = cJSON_Parse("[{\"marketid\":9,\"skinindex\":1,\"price\":500,\"seller_name\":\"B\",\"listed_time\":\"2026-11-20T10:00:00Z\"},"
                             "{\"marketid\":8,\"skinindex\":1,\"funds\":200,\"seller_name\":\"A\",\"listed_time\":1700000000000,\"seller_id\":4},"
                             "{\"marketid\":7,\"skinindex\":2,\"price\":1,\"seller_name\":\"C\"}]");
    rows = kio_market_view_listings(lst, 1);
    CHECK(cJSON_GetArraySize(rows) == 2, "another item's listing is dropped");
    r0 = cJSON_GetArrayItem(rows, 0);
    r1 = cJSON_GetArrayItem(rows, 1);
    CHECK(n(r0, "m") == 8 && n(r0, "i") == 1 && n(r0, "f") == 200 && !strcmp(s(r0, "p"), "A") && n(r0, "ms") == 1700000000000.0 &&
              n(r0, "seller_id") == 4, "cheapest first; funds read as the price");
    CHECK(n(r1, "m") == 9 && n(r1, "f") == 500 && n(r1, "ms") == 1795168800000.0 && !cJSON_GetObjectItem(r1, "seller_id"),
          "price and an ISO listed_time");
    cJSON_Delete(rows);
    cJSON_Delete(lst);
    rows = kio_market_view_listings(NULL, 1);
    CHECK(cJSON_IsArray(rows) && cJSON_GetArraySize(rows) == 0, "no listings: none");
    cJSON_Delete(rows);

    // the Black Market: {item_index} (v10:26934) and {item, kr, funds[, title]} -> "bmr" {item, kr, f[, title]}
    cJSON *b = kio_market_bm_body(1642);
    char *text = cJSON_PrintUnformatted(b);
    CHECK(text && !strcmp(text, "{\"item_index\":1642}"), "bm body %s", text ? text : "(null)");
    cJSON_free(text);
    cJSON_Delete(b);
    cJSON *ans = cJSON_Parse("{\"item\":1866,\"kr\":10000,\"funds\":2500,\"title\":91}");
    cJSON *bmr = kio_market_bm_reply(ans, 1866, 99);
    CHECK(bmr && n(bmr, "item") == 1866 && n(bmr, "kr") == 10000 && n(bmr, "f") == 2500 && n(bmr, "title") == 91, "bm reply with a title");
    cJSON_Delete(bmr);
    cJSON_Delete(ans);
    ans = cJSON_Parse("{\"kr\":3000}");
    bmr = kio_market_bm_reply(ans, 1642, 99);
    CHECK(bmr && n(bmr, "item") == 1642 && n(bmr, "f") == 99 && !cJSON_GetObjectItem(bmr, "title"), "no funds: the account's; the asked item");
    cJSON_Delete(bmr);
    cJSON_Delete(ans);
    CHECK(kio_market_bm_reply(NULL, 1, 0) == NULL, "no data: the purchase failed");

    printf("kio_market_test: %d/%d\n", checks - failures, checks);
    return failures ? 1 : 0;
}
