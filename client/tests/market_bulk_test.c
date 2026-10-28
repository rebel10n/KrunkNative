// market_bulk_test: item locks, select-mode picks and the bulk request bodies (client/src/ui/market_bulk.c)
// against deob-10.0.4.js: the lock sets (:108837-108853, :147254), the lock body (:108862), the quick-sell
// chunks of 250 (:146851), the buy-multiple body (:132755), and the MARKET_BUSY text (:104255).
//   market_bulk_test
#include <market_bulk.h>

#include <stdio.h>
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

static void test_list(void) {
    MarketLocks locks = {0};
    MarketPicks picks = {0};
    int quantity[8];
    for (int i = 0; i < 8; i++) {
        market_picks_toggle(&picks, &locks, 500 + i, 900 + i, 0);
        quantity[i] = i < 2 ? 60 : 1; // 60 + 60 > 100: the first request takes one stack
    }
    CHECK(market_list_fit(&picks, quantity, 0) == 1, "a list request takes picks while their quantity fits 100");
    CHECK(market_list_fit(&picks, quantity, 1) == 7, "the next request takes the next picks while they fit (%d)", market_list_fit(&picks, quantity, 1));
    cJSON *body = market_list_body(&picks, quantity, 25, 1, 7);
    cJSON *items = body ? cJSON_GetObjectItem(body, "items") : NULL;
    cJSON *row = items ? cJSON_GetArrayItem(items, 0) : NULL;
    CHECK(items && cJSON_GetArraySize(items) == 7, "the list body holds the rows it covers (%d)", items ? cJSON_GetArraySize(items) : -1);
    CHECK(row && cJSON_GetObjectItem(row, "item_id")->valueint == 901 && cJSON_GetObjectItem(row, "skin_id")->valueint == 501 &&
              cJSON_GetObjectItem(row, "price")->valueint == 25 && cJSON_GetObjectItem(row, "quantity")->valueint == 60,
          "each row is {item_id, skin_id, price, quantity}");
    cJSON_Delete(body);
    market_picks_free(&picks);
    market_locks_free(&locks);
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
    test_groups();
    test_chunk_limit();
    test_buy_multiple_and_errors();
    printf("market_bulk_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
