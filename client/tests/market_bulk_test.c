// market_bulk_test: item locks, select-mode picks and the bulk request bodies (client/src/ui/market_bulk.c)
// against deob-10.0.4.js: the lock sets (:108837-108853, :147254), the lock body (:108862), the quick-sell
// chunks of 250 (:146851), the bulk list's 100-unit requests that split a row (:131956-131968) and its fee (:132071),
// the buy-multiple body (:132755) and ladder (:132813, :132827), the quick sell value (:108926-108962), and the
// MARKET_BUSY text (:104255).
//   market_bulk_test
#include <market_bulk.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cJSON.h>

static int failures, checks;

#define CHECK(cond, ...)                                  \
    do {                                                  \
        checks++;                                         \
        if (!(cond)) {                                    \
            failures++;                                   \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);   \
            printf(__VA_ARGS__);                          \
            printf("\n");                                 \
        }                                                 \
    } while (0)

static cJSON *parse(const char *text) { return cJSON_Parse(text); }

static void test_locks(void) {
    MarketLocks locks = {0};
    cJSON *rows = parse("[{\"skin_id\":7,\"item_id\":0,\"stack\":false},"
                        "{\"skin_id\":9,\"item_id\":42,\"stack\":true},"
                        "{\"skin_id\":7,\"item_id\":43,\"stack\":true}]");
    market_locks_apply(&locks, rows);
    CHECK(locks.skin_n == 2, "skin ids are de-duplicated (got %d)", locks.skin_n);
    CHECK(locks.item_n == 2, "stack item ids are collected (got %d)", locks.item_n);
    CHECK(market_locks_has(&locks, 7, 0, 0), "a non-stack row locked by its skin id");
    CHECK(market_locks_has(&locks, 9, 99, 0), "a row whose skin id is locked is locked, stackable or not");
    CHECK(market_locks_has(&locks, 3, 42, 1), "a stackable row locked by its item id");
    CHECK(!market_locks_has(&locks, 3, 42, 0), "a non-stackable row is not locked by an item id");
    CHECK(!market_locks_has(&locks, 3, 5, 1), "an unlocked row is not locked");

    cJSON *empty = parse("[]");
    market_locks_apply(&locks, empty); // replaces, not merges
    CHECK(locks.skin_n == 0 && locks.item_n == 0 && !market_locks_has(&locks, 7, 42, 1), "a new answer replaces the state");
    market_locks_apply(&locks, NULL);
    CHECK(locks.skin_n == 0 && locks.item_n == 0, "a missing list leaves no locks");

    market_locks_free(&locks);
    cJSON_Delete(rows);
    cJSON_Delete(empty);
}

static void test_lock_body(void) {
    cJSON *by_skin = market_lock_body(12, 0, 1);
    CHECK(by_skin && cJSON_GetObjectItem(by_skin, "skin_id") && cJSON_GetObjectItem(by_skin, "skin_id")->valueint == 12 &&
              !cJSON_GetObjectItem(by_skin, "item_id") && cJSON_IsTrue(cJSON_GetObjectItem(by_skin, "locked")),
          "a row with a skin id sends skin_id and locked: true");
    cJSON *by_item = market_lock_body(0, 55, 0);
    CHECK(by_item && cJSON_GetObjectItem(by_item, "item_id") && cJSON_GetObjectItem(by_item, "item_id")->valueint == 55 &&
              cJSON_IsFalse(cJSON_GetObjectItem(by_item, "locked")),
          "a row without a skin id sends item_id and locked: false");
    cJSON_Delete(by_skin);
    cJSON_Delete(by_item);
}

static void test_picks(void) {
    MarketLocks locks = {0};
    cJSON *rows = parse("[{\"skin_id\":5,\"item_id\":0,\"stack\":false}]");
    market_locks_apply(&locks, rows);
    MarketPicks picks = {0};
    CHECK(market_picks_toggle(&picks, &locks, 4, 10, 0) == 1 && picks.n == 1, "an unlocked row is picked");
    CHECK(market_picks_toggle(&picks, &locks, 5, 11, 0) == 0 && picks.n == 1, "a locked row is refused");
    CHECK(market_picks_toggle(&picks, &locks, 4, 10, 0) == 0 && picks.n == 0, "a picked row toggles off");
    for (int i = 0; i < 100; i++) market_picks_toggle(&picks, &locks, 100 + i, 1000 + i, 0);
    CHECK(picks.n == 100, "picks grow past the first allocation (n=%d)", picks.n);
    CHECK(picks.skin_ids[99] == 199 && picks.item_ids[99] == 1099, "the last pick keeps its skin and item ids");

    cJSON *body = market_quick_sell_body(0, &picks, 0, 250);
    cJSON *items = body ? cJSON_GetObjectItem(body, "items") : NULL;
    CHECK(items && cJSON_GetArraySize(items) == 100, "a chunk holds the picks it covers (%d)", items ? cJSON_GetArraySize(items) : -1);
    CHECK(body && cJSON_GetObjectItem(body, "action")->valueint == 0, "the body carries the action (0 Get KR)");
    cJSON *first = items ? cJSON_GetArrayItem(items, 0) : NULL;
    CHECK(first && cJSON_GetObjectItem(first, "skin_id")->valueint == 100 && cJSON_GetObjectItem(first, "item_id")->valueint == 1000,
          "each row is {skin_id, item_id}");
    cJSON_Delete(body);

    cJSON *tail = market_quick_sell_body(0, &picks, 60, 250);
    cJSON *tail_items = tail ? cJSON_GetObjectItem(tail, "items") : NULL;
    CHECK(tail_items && cJSON_GetArraySize(tail_items) == 40, "a chunk from offset 60 holds the rest (%d)",
          tail_items ? cJSON_GetArraySize(tail_items) : -1);
    cJSON_Delete(tail);

    cJSON *past = market_quick_sell_body(0, &picks, 100, 250);
    cJSON *past_items = past ? cJSON_GetObjectItem(past, "items") : NULL;
    CHECK(past_items && cJSON_GetArraySize(past_items) == 0, "a chunk past the end is empty");
    cJSON_Delete(past);

    market_picks_free(&picks);
    market_locks_free(&locks);
    cJSON_Delete(rows);
}

static int item_int(const cJSON *items, const int i, const char *key) {
    const cJSON *row = cJSON_GetArrayItem(items, i);
    const cJSON *v = row ? cJSON_GetObjectItem(row, key) : NULL;
    return v ? v->valueint : -1;
}

static void test_list(void) {
    // a stack of 150 at 25, a stack of 60 at 7, two copies at 40: 100 units per request, rows split across them
    const MarketListRow rows[] = {{901, 501, 25, 150}, {902, 502, 7, 60}, {903, 601, 40, 1}, {903, 602, 40, 1}};
    int row = 0, used = 0, units = 0;
    cJSON *b1 = market_list_batch(rows, 4, &row, &used, &units);
    cJSON *i1 = b1 ? cJSON_GetObjectItem(b1, "items") : NULL;
    CHECK(i1 && cJSON_GetArraySize(i1) == 1 && units == 100 && item_int(i1, 0, "quantity") == 100 && item_int(i1, 0, "skin_id") == 501 &&
              item_int(i1, 0, "price") == 25 && item_int(i1, 0, "item_id") == 901,
          "the first request takes 100 of the 150-stack (%d rows, %d units)", i1 ? cJSON_GetArraySize(i1) : -1, units);
    CHECK(row == 0 && used == 100, "the cursor stays in the stack (%d, %d)", row, used);
    cJSON *b2 = market_list_batch(rows, 4, &row, &used, &units);
    cJSON *i2 = b2 ? cJSON_GetObjectItem(b2, "items") : NULL;
    CHECK(i2 && cJSON_GetArraySize(i2) == 2 && units == 100 && item_int(i2, 0, "quantity") == 50 && item_int(i2, 1, "quantity") == 50 &&
              item_int(i2, 1, "skin_id") == 502 && item_int(i2, 1, "price") == 7,
          "the second takes the stack's last 50 and 50 of the next row");
    cJSON *b3 = market_list_batch(rows, 4, &row, &used, &units);
    cJSON *i3 = b3 ? cJSON_GetObjectItem(b3, "items") : NULL;
    CHECK(i3 && cJSON_GetArraySize(i3) == 3 && units == 12 && item_int(i3, 0, "quantity") == 10 && item_int(i3, 1, "skin_id") == 601 &&
              item_int(i3, 2, "skin_id") == 602 && item_int(i3, 2, "quantity") == 1,
          "the last request takes the rest (%d rows, %d units)", i3 ? cJSON_GetArraySize(i3) : -1, units);
    CHECK(market_list_batch(rows, 4, &row, &used, &units) == NULL && units == 0, "then nothing is left");
    cJSON_Delete(b1);
    cJSON_Delete(b2);
    cJSON_Delete(b3);

    const MarketListRow empty[] = {{1, 1, 5, 0}};
    row = used = 0;
    CHECK(market_list_batch(empty, 1, &row, &used, &units) == NULL, "a row with no units sends nothing");

    CHECK(market_list_fee(25, 4) == 12 && market_list_fee(10, 3) == 3 && market_list_fee(1, 1) == 1 && market_list_fee(0, 5) == 0,
          "the fee is quantity * ceil(price / 10) (%ld %ld %ld)", market_list_fee(25, 4), market_list_fee(10, 3), market_list_fee(1, 1));
}

static void test_ladder(void) {
    cJSON *data = parse("{\"prices\":[5,6,6,9,120]}");
    double *p = NULL;
    const int n = market_ladder_parse(data, &p);
    CHECK(n == 5 && p && p[0] == 5 && p[4] == 120, "the ladder is data.prices, cheapest first (%d)", n);
    CHECK(market_ladder_total(p, n, 3) == 17, "three cost the three cheapest (%g)", market_ladder_total(p, n, 3));
    CHECK(market_ladder_total(p, n, 9) == 146, "a quantity past the ladder takes all of it");
    CHECK(market_ladder_total(p, n, 0) == 0, "none costs nothing");
    free(p);
    cJSON_Delete(data);
    cJSON *none = parse("{\"prices\":[]}");
    CHECK(market_ladder_parse(none, &p) == 0 && p == NULL, "an empty ladder");
    CHECK(market_ladder_parse(NULL, &p) == 0 && p == NULL, "no data");
    cJSON_Delete(none);
}

static void test_qs_value(void) {
    cJSON *legendary = parse("{\"rarity\":3}");
    MarketQsValue v = market_qs_value(legendary);
    CHECK(v.kr == 100 && v.junk == 0 && v.can_kr && !v.can_junk && !v.can_destroy && v.can_qs, "a Legendary sells for 100 KR, no Junk");
    cJSON *contra = parse("{\"rarity\":5}");
    v = market_qs_value(contra);
    CHECK(v.kr == 2500 && v.junk == 5 && v.can_kr && v.can_junk, "a Contraband sells for KR or Junk");
    cJSON *nqs = parse("{\"rarity\":6,\"noQuickSell\":true}");
    v = market_qs_value(nqs);
    CHECK(!v.can_kr && !v.can_junk && v.can_destroy && !v.can_qs, "noQuickSell is destroy only");
    cJSON *bm = parse("{\"rarity\":4,\"noSale\":true,\"blackM\":true}");
    v = market_qs_value(bm);
    CHECK(!v.can_kr && !v.can_junk && !v.can_destroy, "a Black Market item that is noSale does none of them");
    cJSON *min = parse("{\"rarity\":6,\"minQuickSell\":1}");
    v = market_qs_value(min);
    CHECK(v.kr == 5 && v.junk == 0.01, "minQuickSell: 5 KR, 0.01 Junk (%g %g)", v.kr, v.junk);
    cJSON *ovr = parse("{\"rarity\":0,\"qsKR\":7,\"qsJunk\":3}");
    v = market_qs_value(ovr);
    CHECK(v.kr == 7 && v.junk == 0, "qsKR overrides a KR value; qsJunk needs a Junk value to override");
    v = market_qs_value(NULL);
    CHECK(!v.can_kr && !v.can_junk && !v.can_destroy, "no item, no value");
    cJSON_Delete(legendary);
    cJSON_Delete(contra);
    cJSON_Delete(nqs);
    cJSON_Delete(bm);
    cJSON_Delete(min);
    cJSON_Delete(ovr);
}

static void test_groups(void) {
    cJSON *rows = parse("[{\"i\":5,\"st\":1,\"sid\":900,\"cnt\":7},{\"i\":5,\"st\":0,\"sid\":901},"
                        "{\"i\":5,\"st\":0,\"sid\":902},{\"i\":8,\"st\":0,\"sid\":903},{\"i\":9,\"st\":0,\"sid\":0}]");
    MarketGroup *g = NULL;
    int n = 0;
    CHECK(market_groups_parse(rows, &g, &n) == 0 && n == 2, "rows group by item index (got %d)", n);
    CHECK(n == 2 && g[0].item == 5 && g[0].stack_id == 900 && g[0].stack_max == 7, "a stack row sets the stack id and count");
    CHECK(n == 2 && g[0].n_instances == 2 && g[0].instances[0] == 901 && g[0].instances[1] == 902, "copies keep their ids in order");
    CHECK(n == 2 && g[1].item == 8 && g[1].stack_id == 0 && g[1].n_instances == 1 && g[1].instances[0] == 903, "an item with one copy");
    market_groups_free(g, n);
    cJSON_Delete(rows);

    cJSON *none = parse("[]");
    CHECK(market_groups_parse(none, &g, &n) == 0 && g == NULL && n == 0, "no rows, no groups");
    market_groups_free(g, n);
    cJSON_Delete(none);
}

static void test_chunk_limit(void) {
    MarketLocks locks = {0};
    MarketPicks picks = {0};
    for (int i = 0; i < 260; i++) market_picks_toggle(&picks, &locks, 1 + i, 2000 + i, 0);
    cJSON *body = market_quick_sell_body(2, &picks, 0, 1000);
    cJSON *items = body ? cJSON_GetObjectItem(body, "items") : NULL;
    CHECK(items && cJSON_GetArraySize(items) == 250, "a chunk never holds more than 250 rows (%d)", items ? cJSON_GetArraySize(items) : -1);
    cJSON_Delete(body);
    market_picks_free(&picks);
}

static void test_buy_multiple_and_errors(void) {
    cJSON *body = market_buy_multiple_body(77, 3, 1500.0);
    CHECK(body && cJSON_GetObjectItem(body, "item_id")->valueint == 77 && cJSON_GetObjectItem(body, "quantity")->valueint == 3 &&
              cJSON_GetObjectItem(body, "max_total")->valuedouble == 1500.0,
          "the buy-multiple body is {item_id, quantity, max_total}");
    cJSON_Delete(body);

    CHECK(market_error_key("MARKET_BUSY") && !strcmp(market_error_key("MARKET_BUSY"), "errors.exchange.busy"),
          "MARKET_BUSY maps to errors.exchange.busy");
    CHECK(market_error_key("MARKET_NOT_A_CODE") == NULL, "an unmapped code has no key");
    CHECK(market_error_key(NULL) == NULL, "no code, no key");
}

int main(void) {
    test_locks();
    test_lock_body();
    test_picks();
    test_list();
    test_ladder();
    test_qs_value();
    test_groups();
    test_chunk_limit();
    test_buy_multiple_and_errors();
    printf("market_bulk_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
