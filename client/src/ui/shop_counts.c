// A shop display's purchase counts after a purchase (client/include/shop_counts.h; deob-10.0.6.js:115979-116030).
#include <shop_counts.h>
#include <stdio.h>
#include <string.h>

static void bump(cJSON *o) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, "purchase_count");
    if (!v || cJSON_IsNull(v)) return; // void 0 !== purchase_count: a node or product without a count keeps none
    const double n = cJSON_IsNumber(v) ? v->valuedouble : 0; // (purchase_count || 0) + 1
    cJSON_ReplaceItemInObjectCaseSensitive(o, "purchase_count", cJSON_CreateNumber(n + 1));
}

// iIíìîïî (:115989-116012): the first node with that id, depth first through children
static cJSON *find_node(cJSON *children, const int id, const int depth) {
    if (!cJSON_IsArray(children) || depth > 16) return NULL;
    cJSON *c;
    cJSON_ArrayForEach(c, children) {
        const cJSON *cid = cJSON_GetObjectItemCaseSensitive(c, "id");
        if (cJSON_IsNumber(cid) && cid->valueint == id) return c;
        cJSON *f = find_node(cJSON_GetObjectItemCaseSensitive(c, "children"), id, depth + 1);
        if (f) return f;
    }
    return NULL;
}

int shop_display_purchased(cJSON *shop, const int display_id) {
    if (!cJSON_IsObject(shop)) return 0;
    cJSON *node = find_node(cJSON_GetObjectItemCaseSensitive(shop, "children"), display_id, 0);
    if (!node) return 0; // :116015-116017: a shop without the node is left as it is
    bump(node);
    const cJSON *pid = cJSON_GetObjectItemCaseSensitive(node, "product_id"); // iÎiíiíi = node.product_id
    if (cJSON_IsNumber(pid) && pid->valueint) {
        char key[24];
        snprintf(key, sizeof(key), "%d", pid->valueint); // products[product_id.toString()]
        cJSON *product = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(shop, "products"), key);
        if (cJSON_IsObject(product)) bump(product);
    } else if (cJSON_IsString(pid) && pid->valuestring[0]) {
        cJSON *product = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(shop, "products"), pid->valuestring);
        if (cJSON_IsObject(product)) bump(product);
    }
    return 1;
}

int shop_purchase_counts(const char *display_type, const char *limit_scope, const int gift) {
    if (!gift) return 1;
    return display_type && !strcmp(display_type, "product") && limit_scope && !strcmp(limit_scope, "global");
}
