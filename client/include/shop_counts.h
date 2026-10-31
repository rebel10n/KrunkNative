#pragma once
// A shop display's purchase counts after a purchase (Krunker v10.0.6 updateDisplayPurchaseCount,
// deob-10.0.6.js:115979-116030): the bought display node's purchase_count and its product's purchase_count go up by
// one at once, so a limited item reads sold out right after it is bought (v10.0.6 "Limited items now show as sold out
// right after you buy them"). Pure cJSON (client/tests/shop_counts_test.c).
#include <cJSON.h>

// shop: a display as shop.getDisplayByTag answers it ({id, children [nodes], products {"<id>": product}}). The node
// whose id is display_id is looked for among the shop's children, at any depth (not the shop itself, as v10); its
// purchase_count goes up when it has one, and so does products[node.product_id].purchase_count when that product has
// one. 1 = the node was found (counts changed or not), 0 = not in this shop.
int shop_display_purchased(cJSON *shop, int display_id);

// Whether a purchase of this display counts on it (deob-10.0.6.js:116095, :117701-117774, :213596, :213783): a
// purchase for oneself always does; a gift only for a "product" display whose product's limit is global.
int shop_purchase_counts(const char *display_type, const char *limit_scope, int gift);
