/*
 * services/brain/db_shop.c - the Junk-priced shop (doc/v10/junk.md section 6; migration
 * 0349_shop_junk; routes in shop.c). v10's shop purchase (POST shop/purchase {product_id, quantity})
 * for a product whose price is in Junk (currency_id 2, the "virtual" price block of v10's product).
 *
 * P_SHOP_DISPLAY (tag, account) -> the display v10's shop.getDisplayByTag reads: {id, type "front",
 *   order, data, children [product nodes], products {"<product id>": product}}. A product is v10's
 *   shape: name, description, image_url, prices [{type "virtual", virtual {amount (hundredths),
 *   currency_id 2, currency "JNK"}}], contents [{type "skin", ref, quantity}], purchase_limit (per
 *   account, null for none), purchase_count (the account's quantity so far). No active product with the
 *   tag: refused "No display found with this tag" (v10's 404).
 * P_SHOP_PURCHASE (account, product, quantity) -> {success, message, rewards [{type "skin", skin_id,
 *   amount}], currency_id 2, price_paid (hundredths), junk ("12.34")}. The price is spent through
 *   junk_spend (reason "shop"; "Not enough Junk" writes nothing), the contents are granted through
 *   db_add_skin (source "shop"), and the purchase row is written, all in one transaction.
 *   Refused: "Product not found", "Invalid quantity.", "Purchase limit reached".
 */
#include "db_procs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "junk.h"
#include "knsvc/json.h"

#define P(i) (n > (i) ? p[i] : NULL)
#define PI(i) pint(P(i))
#define SHOP_MAX_QUANTITY 100
#define SHOP_NODE_BASE 1000000 /* the product nodes' display ids (the front is 1) */

static int64_t pint(const char *s) { return s ? strtoll(s, NULL, 10) : 0; }

static int64_t jint(const cJSON *o, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(v) ? (int64_t) v->valuedouble : 0;
}

static const char *jtext(const cJSON *o, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(v) ? v->valuestring : "";
}

/* the account's quantity of a product so far (0 for a guest: account 0 has no purchases) */
static int64_t bought_so_far(Dbx *x, const int64_t account, const int64_t product) {
    return dbx_int(x, SQL(.any = "SELECT COALESCE(SUM(quantity), 0) FROM shop_purchases WHERE account_id = $1 AND product_id = $2"),
                   KN_DB_ARGS(I64S(account), I64S(product)), 0);
}

/* ------------------------------------------------------------------ display */

static kn_db_result *run_display(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const char *tag = P(0) && *P(0) ? P(0) : "limited";
    const int64_t account = PI(1);
    cJSON *rows = dbx_rows(&x, SQL(.any = "SELECT id AS id__i, name, description, image_url, price_amount AS price__i,"
                                          " purchase_limit AS limit__i FROM shop_products"
                                          " WHERE display_tag = $1 AND active = 1 ORDER BY sort_order, id"),
                           KN_DB_ARGS(tag));
    if (dbx_failed(&x)) {
        cJSON_Delete(rows);
        return dbx_finish(&x, NULL);
    }
    if (!rows || cJSON_GetArraySize(rows) == 0) {
        cJSON_Delete(rows);
        dbx_refuse(&x, "No display found with this tag");
        return dbx_finish(&x, NULL);
    }
    cJSON *display = cJSON_CreateObject();
    cJSON *children = cJSON_CreateArray();
    cJSON *products = cJSON_CreateObject();
    cJSON_AddNumberToObject(display, "id", 1);
    cJSON_AddStringToObject(display, "type", "front");
    cJSON_AddNumberToObject(display, "order", 0);
    cJSON_AddItemToObject(display, "data", cJSON_CreateObject());
    for (int i = 0; i < cJSON_GetArraySize(rows); i++) {
        const cJSON *row = cJSON_GetArrayItem(rows, i);
        const int64_t id = jint(row, "id");
        char key[24];
        snprintf(key, sizeof key, "%lld", (long long) id);
        cJSON *pr = cJSON_CreateObject();
        cJSON_AddNumberToObject(pr, "id", (double) id);
        cJSON_AddStringToObject(pr, "name", jtext(row, "name"));
        cJSON_AddStringToObject(pr, "description", jtext(row, "description"));
        cJSON_AddStringToObject(pr, "image_url", jtext(row, "image_url"));
        cJSON_AddTrueToObject(pr, "active");
        cJSON_AddTrueToObject(pr, "available");

        cJSON *price = cJSON_CreateObject();
        cJSON_AddNumberToObject(price, "id", (double) id);
        char ref[64];
        snprintf(ref, sizeof ref, "kn.shop.%lld", (long long) id);
        cJSON_AddStringToObject(price, "price_ref", ref);
        cJSON_AddStringToObject(price, "type", "virtual");
        cJSON *virt = cJSON_CreateObject();
        cJSON_AddNumberToObject(virt, "amount", (double) jint(row, "price"));
        cJSON_AddNumberToObject(virt, "currency_id", 2);
        cJSON_AddStringToObject(virt, "currency", "JNK");
        cJSON_AddItemToObject(price, "virtual", virt);
        cJSON_AddTrueToObject(price, "available");
        cJSON *prices = cJSON_CreateArray();
        cJSON_AddItemToArray(prices, price);
        cJSON_AddItemToObject(pr, "prices", prices);

        cJSON *contents = dbx_rows(&x, SQL(.any = "SELECT skin_id AS ref__i, quantity AS quantity__i FROM shop_product_contents"
                                                  " WHERE product_id = $1 ORDER BY ord"),
                                   KN_DB_ARGS(I64S(id)));
        if (dbx_failed(&x)) {
            cJSON_Delete(contents);
            cJSON_Delete(pr);
            cJSON_Delete(display);
            cJSON_Delete(children);
            cJSON_Delete(products);
            cJSON_Delete(rows);
            return dbx_finish(&x, NULL);
        }
        cJSON *items = cJSON_CreateArray();
        for (int k = 0; contents && k < cJSON_GetArraySize(contents); k++) {
            const cJSON *c0 = cJSON_GetArrayItem(contents, k);
            cJSON *it = cJSON_CreateObject();
            char skin[24];
            snprintf(skin, sizeof skin, "%lld", (long long) jint(c0, "ref"));
            cJSON_AddNumberToObject(it, "id", (double) (k + 1));
            cJSON_AddStringToObject(it, "type", "skin");
            cJSON_AddStringToObject(it, "ref", skin);
            cJSON_AddNumberToObject(it, "quantity", (double) jint(c0, "quantity"));
            cJSON_AddItemToArray(items, it);
        }
        cJSON_Delete(contents);
        cJSON_AddItemToObject(pr, "contents", items);

        const int64_t limit = jint(row, "limit");
        if (limit > 0) cJSON_AddNumberToObject(pr, "purchase_limit", (double) limit);
        else cJSON_AddNullToObject(pr, "purchase_limit");
        cJSON_AddNumberToObject(pr, "purchase_count", (double) (account > 0 ? bought_so_far(&x, account, id) : 0));
        cJSON_AddStringToObject(pr, "purchase_limit_scope", "per_user");
        cJSON *dids = cJSON_CreateArray();
        cJSON_AddItemToArray(dids, cJSON_CreateNumber(1));
        cJSON_AddItemToObject(pr, "display_ids", dids);
        if (dbx_failed(&x)) {
            cJSON_Delete(pr);
            cJSON_Delete(display);
            cJSON_Delete(children);
            cJSON_Delete(products);
            cJSON_Delete(rows);
            return dbx_finish(&x, NULL);
        }

        cJSON *node = cJSON_CreateObject();
        cJSON_AddNumberToObject(node, "id", (double) (SHOP_NODE_BASE + id));
        cJSON_AddNumberToObject(node, "parent_id", 1);
        cJSON_AddNumberToObject(node, "order", i);
        cJSON_AddStringToObject(node, "type", "product");
        cJSON_AddItemToObject(node, "data", cJSON_CreateObject());
        cJSON_AddNumberToObject(node, "product_id", (double) id);
        cJSON_AddItemToArray(children, node);
        cJSON_AddItemToObject(products, key, pr);
    }
    cJSON_Delete(rows);
    cJSON_AddItemToObject(display, "children", children);
    cJSON_AddItemToObject(display, "products", products);
    return dbx_json(&x, "shop_display", display);
}
const kn_proc P_SHOP_DISPLAY = {"shop_display", NULL, run_display};

/* ------------------------------------------------------------------ purchase */

static kn_db_result *run_purchase(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const int64_t account = PI(0), product = PI(1), quantity = PI(2);
    if (quantity < 1 || quantity > SHOP_MAX_QUANTITY) {
        dbx_refuse(&x, "Invalid quantity.");
        return dbx_finish(&x, NULL);
    }
    // the product row lock: two purchases of the last unit queue here
    if (dbx_exec(&x, SQL(.any = "UPDATE shop_products SET sort_order = sort_order WHERE id = $1"), KN_DB_ARGS(I64S(product))) < 0)
        return dbx_finish(&x, NULL);
    cJSON *row = dbx_row(&x, SQL(.any = "SELECT price_amount AS price__i, purchase_limit AS limit__i FROM shop_products"
                                       " WHERE id = $1 AND active = 1"),
                         KN_DB_ARGS(I64S(product)));
    if (dbx_failed(&x)) {
        cJSON_Delete(row);
        return dbx_finish(&x, NULL);
    }
    if (!row) {
        dbx_refuse(&x, "Product not found");
        return dbx_finish(&x, NULL);
    }
    const int64_t price = jint(row, "price"), limit = jint(row, "limit");
    cJSON_Delete(row);
    if (limit > 0 && bought_so_far(&x, account, product) + quantity > limit) {
        dbx_refuse(&x, "Purchase limit reached");
        return dbx_finish(&x, NULL);
    }
    if (dbx_failed(&x)) return dbx_finish(&x, NULL);

    const int64_t cost = price * quantity;
    int64_t balance = 0;
    const int r = junk_spend(&x, account, cost, "shop", &balance);
    if (r == JUNK_INSUFFICIENT) {
        dbx_refuse(&x, "Not enough Junk");
        return dbx_finish(&x, NULL);
    }
    if (r < 0) return dbx_finish(&x, NULL);

    cJSON *contents = dbx_rows(&x, SQL(.any = "SELECT skin_id AS skin__i, quantity AS quantity__i FROM shop_product_contents"
                                              " WHERE product_id = $1 ORDER BY ord"),
                               KN_DB_ARGS(I64S(product)));
    cJSON *rewards = cJSON_CreateArray();
    char ref[48];
    snprintf(ref, sizeof ref, "shop:%lld", (long long) product);
    for (int i = 0; contents && i < cJSON_GetArraySize(contents) && !dbx_failed(&x); i++) {
        const cJSON *c0 = cJSON_GetArrayItem(contents, i);
        const int64_t skin = jint(c0, "skin"), amount = jint(c0, "quantity") * quantity;
        if (db_add_skin(&x, account, skin, amount, "shop", ref, NULL) < 0) break;
        cJSON *rw = cJSON_CreateObject();
        cJSON_AddStringToObject(rw, "type", "skin");
        cJSON_AddNumberToObject(rw, "skin_id", (double) skin);
        cJSON_AddNumberToObject(rw, "amount", (double) amount);
        cJSON_AddItemToArray(rewards, rw);
    }
    cJSON_Delete(contents);
    if (dbx_failed(&x)) {
        cJSON_Delete(rewards);
        return dbx_finish(&x, NULL);
    }
    if (dbx_exec(&x, SQL(.any = "INSERT INTO shop_purchases (account_id, product_id, quantity, price_paid) VALUES ($1, $2, $3, $4)"),
                 KN_DB_ARGS(I64S(account), I64S(product), I64S(quantity), I64S(cost))) < 0) {
        cJSON_Delete(rewards);
        return dbx_finish(&x, NULL);
    }

    cJSON *o = cJSON_CreateObject();
    cJSON_AddTrueToObject(o, "success");
    cJSON_AddStringToObject(o, "message", "Purchase successful");
    cJSON_AddItemToObject(o, "rewards", rewards);
    cJSON_AddNumberToObject(o, "currency_id", 2);
    cJSON_AddNumberToObject(o, "price_paid", (double) cost);
    cJSON *jb = junk_json(balance);
    cJSON_AddStringToObject(o, "junk", cJSON_GetObjectItemCaseSensitive(jb, "junk")->valuestring);
    cJSON_Delete(jb);
    return dbx_json(&x, "shop_purchase", o);
}
const kn_proc P_SHOP_PURCHASE = {"shop_purchase", NULL, run_purchase};
