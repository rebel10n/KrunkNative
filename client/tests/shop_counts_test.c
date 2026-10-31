// shop_counts: a display's purchase counts after a purchase (client/src/ui/shop_counts.c; updateDisplayPurchaseCount,
// deob-10.0.6.js:115979-116030, and its gift rule :117701).
#include <shop_counts.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond, ...)                                       \
    do {                                                       \
        if (!(cond)) {                                         \
            failures++;                                        \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);        \
            printf(__VA_ARGS__);                               \
            printf("\n");                                      \
        }                                                      \
    } while (0)

static double count_of(const cJSON *o) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, "purchase_count");
    return cJSON_IsNumber(v) ? v->valuedouble : -1;
}

int main(void) {
    // a front with a nested node (depth first), a node without a count and a product without one
    cJSON *shop = cJSON_Parse("{\"id\":1,\"purchase_count\":0,\"children\":["
                              "{\"id\":10,\"product_id\":35,\"purchase_count\":0},"
                              "{\"id\":11,\"children\":[{\"id\":12,\"product_id\":36}]},"
                              "{\"id\":13,\"product_id\":37,\"purchase_count\":null}],"
                              "\"products\":{\"35\":{\"purchase_limit\":1,\"purchase_count\":0},"
                              "\"36\":{\"purchase_count\":4},\"37\":{\"name\":\"no count\"}}}");
    CHECK(shop != NULL, "fixture parses");
    const cJSON *children = cJSON_GetObjectItem(shop, "children"), *products = cJSON_GetObjectItem(shop, "products");

    // the bought node and its product count one more: a 1-limit product reads sold out at once
    CHECK(shop_display_purchased(shop, 10) == 1, "node 10 found");
    CHECK(count_of(cJSON_GetArrayItem(children, 0)) == 1, "node 10 counts 1 (%g)", count_of(cJSON_GetArrayItem(children, 0)));
    CHECK(count_of(cJSON_GetObjectItem(products, "35")) == 1, "product 35 counts 1");
    CHECK(count_of(shop) == 0, "the root is not a candidate (children only)");
    CHECK(shop_display_purchased(shop, 10) == 1 && count_of(cJSON_GetObjectItem(products, "35")) == 2, "a second purchase counts 2");

    // a nested node; the node has no count of its own, so only its product moves
    CHECK(shop_display_purchased(shop, 12) == 1, "nested node 12 found");
    const cJSON *n12 = cJSON_GetArrayItem(cJSON_GetObjectItem(cJSON_GetArrayItem(children, 1), "children"), 0);
    CHECK(!cJSON_GetObjectItem(n12, "purchase_count"), "a node without a count keeps none");
    CHECK(count_of(cJSON_GetObjectItem(products, "36")) == 5, "product 36 counts 5");

    // null counts and products without one are left alone (void 0 !== purchase_count)
    CHECK(shop_display_purchased(shop, 13) == 1, "node 13 found");
    CHECK(cJSON_IsNull(cJSON_GetObjectItem(cJSON_GetArrayItem(children, 2), "purchase_count")), "a null count stays null");
    CHECK(!cJSON_GetObjectItem(cJSON_GetObjectItem(products, "37"), "purchase_count"), "a product without a count keeps none");

    // a node the shop does not have, the root's id, no shop: nothing changes
    CHECK(shop_display_purchased(shop, 99) == 0, "missing node: 0");
    CHECK(shop_display_purchased(shop, 1) == 0, "the root's id: 0");
    CHECK(shop_display_purchased(NULL, 10) == 0, "no shop: 0");
    CHECK(count_of(cJSON_GetObjectItem(products, "35")) == 2, "nothing moved");
    cJSON_Delete(shop);

    // a string product id (the display's products map is keyed by it)
    cJSON *s2 = cJSON_Parse("{\"children\":[{\"id\":5,\"product_id\":\"8\",\"purchase_count\":2}],\"products\":{\"8\":{\"purchase_count\":2}}}");
    CHECK(shop_display_purchased(s2, 5) == 1 && count_of(cJSON_GetObjectItem(cJSON_GetObjectItem(s2, "products"), "8")) == 3,
          "a string product id counts");
    cJSON_Delete(s2);

    // who counts: the buyer always; a gift only for a product display with a global limit (:117701)
    CHECK(shop_purchase_counts("product", "account", 0) == 1, "own purchase counts");
    CHECK(shop_purchase_counts(NULL, NULL, 0) == 1, "own purchase counts without a display");
    CHECK(shop_purchase_counts("product", "global", 1) == 1, "a gift of a global-limit product counts");
    CHECK(shop_purchase_counts("product", "account", 1) == 0, "a gift of a per-account product does not");
    CHECK(shop_purchase_counts("bundle", "global", 1) == 0, "a gift of a non-product display does not");
    CHECK(shop_purchase_counts(NULL, "global", 1) == 0, "a gift without a display does not");

    if (failures) {
        printf("shop_counts_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("shop_counts_test: ok\n");
    return 0;
}
