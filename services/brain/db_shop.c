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
 * The "limited" display also carries the official products of krunker.io's display (0354_store_catalog,
 * shop_official_products), verbatim, first; their ids are krunker.io's and Box Office's Junk products' display
 * ids are shop_products.id + SHOP_JUNK_BASE, so the two never share a key. A purchase below the offset is an
 * official product: official_purchase sells its KR prices (the KR ledger, db_add_funds), grants its skins and Junk,
 * and refuses USD products and badges ("Payment is not available on Box Office", "Not sold on Box Office").
 * The display lists only the official products official_purchase can sell (official_products' filter), so nothing
 * listed is unbuyable: the USD products and the badge stay in the table but are not shown on Box Office.
 * P_SHOP_BUNDLES (account) -> the store's bundle rows (store_bundles, 0354/0356), as v10's ALL_BUNDLE_DATA keys with
 * DATE and the account's OWNED, in v10's order (GET /api/shop/bundles). P_SHOP_BUNDLE_BUY (account, bundle): every
 * bundle is USD, so a bundle that is not owned is refused "Payment is not available on Box Office".
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
/* the Junk-priced products' display ids are shop_products.id + this: the official "limited" products keep krunker.io's
 * ids (0354_store_catalog, all below it), so the two never share a key; a purchase below it is krunker.io's */
#define SHOP_JUNK_BASE 100000

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

static double jnum(const cJSON *o, const char *key) {
    const cJSON *v = cJSON_GetObjectItem(o, key);
    return cJSON_IsNumber(v) ? v->valuedouble : 0.0;
}

/* the official "limited" products (0354_store_catalog: krunker.io's display, verbatim): their nodes and
 * products go in first, under the front. A product's display id is its node's id (v10's display_ids), so
 * the shop reads them the way it reads krunker.io's display. Returns the count (0 with dbx_failed on error). */
static int64_t official_products(Dbx *x, const char *tag, const int64_t account, cJSON *children, cJSON *products) {
    cJSON *rows = dbx_rows(x, SQL(.any = "SELECT id AS id__i, node_id AS node__i, node_order AS ord__i, node_type,"
                                         " node_data, name, description, image_url, price_id AS price_id__i, price_ref,"
                                         " price_type, price_usd AS usd__f, price_amount AS amount__i,"
                                         " price_currency_id AS currency_id__i, price_currency, purchase_limit AS limit__i,"
                                         " purchase_limit_scope, display_id AS display__i FROM shop_official_products"
                                         " WHERE display_tag = $1 AND active = 1 AND price_type = 'virtual' AND price_currency_id = 1"
                                         " AND NOT EXISTS (SELECT 1 FROM shop_official_contents c WHERE c.product_id = shop_official_products.id"
                                         " AND NOT (c.content_type = 'skin' OR (c.content_type = 'currency' AND c.ref = '2')))"
                                         " ORDER BY node_order, id"),
                           KN_DB_ARGS(tag));
    if (dbx_failed(x)) {
        cJSON_Delete(rows);
        return 0;
    }
    int64_t count = 0;
    for (int i = 0; rows && i < cJSON_GetArraySize(rows); i++) {
        const cJSON *row = cJSON_GetArrayItem(rows, i);
        const int64_t id = jint(row, "id");
        char key[24];
        snprintf(key, sizeof key, "%lld", (long long) id);
        const char *text = jtext(row, "node_data");
        cJSON *node_data = kn_json_parse(text, strlen(text));
        if (!node_data) node_data = cJSON_CreateObject();
        const char *ptype = jtext(row, "price_type");
        cJSON *pr = cJSON_CreateObject();
        cJSON_AddNumberToObject(pr, "id", (double) id);
        cJSON_AddStringToObject(pr, "name", jtext(row, "name"));
        cJSON_AddStringToObject(pr, "description", jtext(row, "description"));
        cJSON_AddStringToObject(pr, "image_url", jtext(row, "image_url"));
        cJSON_AddTrueToObject(pr, "active");
        cJSON_AddTrueToObject(pr, "available");
        cJSON *price = cJSON_CreateObject();
        cJSON_AddNumberToObject(price, "id", (double) jint(row, "price_id"));
        cJSON_AddStringToObject(price, "price_ref", jtext(row, "price_ref"));
        cJSON_AddStringToObject(price, "type", ptype);
        if (!strcmp(ptype, "real")) {
            cJSON *real = cJSON_CreateObject();
            cJSON_AddNumberToObject(real, "price_usd", jnum(row, "usd"));
            cJSON_AddItemToObject(price, "real", real);
        } else {
            cJSON *virt = cJSON_CreateObject();
            cJSON_AddNumberToObject(virt, "amount", (double) jint(row, "amount"));
            cJSON_AddNumberToObject(virt, "currency_id", (double) jint(row, "currency_id"));
            cJSON_AddStringToObject(virt, "currency", jtext(row, "price_currency"));
            cJSON_AddItemToObject(price, "virtual", virt);
        }
        cJSON_AddTrueToObject(price, "available");
        cJSON *prices = cJSON_CreateArray();
        cJSON_AddItemToArray(prices, price);
        cJSON_AddItemToObject(pr, "prices", prices);

        cJSON *contents = dbx_rows(x, SQL(.any = "SELECT content_id AS id__i, content_type AS type, ref,"
                                                  " quantity AS quantity__i FROM shop_official_contents"
                                                  " WHERE product_id = $1 ORDER BY ord"),
                                   KN_DB_ARGS(I64S(id)));
        if (dbx_failed(x)) {
            cJSON_Delete(contents);
            cJSON_Delete(node_data);
            cJSON_Delete(pr);
            cJSON_Delete(rows);
            return 0;
        }
        cJSON_AddItemToObject(pr, "contents", contents ? contents : cJSON_CreateArray());

        const int64_t limit = jint(row, "limit");
        if (limit > 0) {
            cJSON_AddNumberToObject(pr, "purchase_limit", (double) limit);
            cJSON_AddStringToObject(pr, "purchase_limit_scope", jtext(row, "purchase_limit_scope"));
        } else {
            cJSON_AddNullToObject(pr, "purchase_limit");
        }
        const int64_t bought = account > 0 ? dbx_int(x, SQL(.any = "SELECT COALESCE(SUM(quantity), 0) FROM shop_official_purchases"
                                                                   " WHERE account_id = $1 AND product_id = $2"),
                                                     KN_DB_ARGS(I64S(account), I64S(id)), 0) : 0;
        if (dbx_failed(x)) {
            cJSON_Delete(node_data);
            cJSON_Delete(pr);
            cJSON_Delete(rows);
            return 0;
        }
        cJSON_AddNumberToObject(pr, "purchase_count", (double) bought);
        cJSON *dids = cJSON_CreateArray();
        const cJSON *dv = cJSON_GetObjectItem(row, "display");
        if (cJSON_IsNumber(dv)) cJSON_AddItemToArray(dids, cJSON_CreateNumber(dv->valuedouble)); // NULL: no display id
        cJSON_AddItemToObject(pr, "display_ids", dids);

        cJSON *node = cJSON_CreateObject();
        cJSON_AddNumberToObject(node, "id", (double) jint(row, "node"));
        cJSON_AddNumberToObject(node, "parent_id", 1);
        cJSON_AddNumberToObject(node, "order", (double) jint(row, "ord"));
        cJSON_AddStringToObject(node, "type", jtext(row, "node_type"));
        cJSON_AddItemToObject(node, "data", node_data);
        cJSON_AddNumberToObject(node, "product_id", (double) id);
        cJSON_AddItemToArray(children, node);
        cJSON_AddItemToObject(products, key, pr);
        count++;
    }
    cJSON_Delete(rows);
    return count;
}

static kn_db_result *run_display(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const char *tag = P(0) && *P(0) ? P(0) : "limited";
    const int64_t account = PI(1);
    cJSON *display = cJSON_CreateObject();
    cJSON *children = cJSON_CreateArray();
    cJSON *products = cJSON_CreateObject();
    cJSON_AddNumberToObject(display, "id", 1);
    cJSON_AddStringToObject(display, "type", "front");
    cJSON_AddNumberToObject(display, "order", 0);
    cJSON_AddItemToObject(display, "data", cJSON_CreateObject());
    const int64_t official = official_products(&x, tag, account, children, products);
    if (dbx_failed(&x)) {
        cJSON_Delete(display);
        cJSON_Delete(children);
        cJSON_Delete(products);
        return dbx_finish(&x, NULL);
    }
    cJSON *rows = dbx_rows(&x, SQL(.any = "SELECT id AS id__i, name, description, image_url, price_amount AS price__i,"
                                          " purchase_limit AS limit__i FROM shop_products"
                                          " WHERE display_tag = $1 AND active = 1 ORDER BY sort_order, id"),
                           KN_DB_ARGS(tag));
    if (dbx_failed(&x)) {
        cJSON_Delete(rows);
        cJSON_Delete(display);
        cJSON_Delete(children);
        cJSON_Delete(products);
        return dbx_finish(&x, NULL);
    }
    if (official == 0 && (!rows || cJSON_GetArraySize(rows) == 0)) {
        cJSON_Delete(rows);
        cJSON_Delete(display);
        cJSON_Delete(children);
        cJSON_Delete(products);
        dbx_refuse(&x, "No display found with this tag");
        return dbx_finish(&x, NULL);
    }
    for (int i = 0; rows && i < cJSON_GetArraySize(rows); i++) {
        const cJSON *row = cJSON_GetArrayItem(rows, i);
        const int64_t raw = jint(row, "id");
        const int64_t id = raw + SHOP_JUNK_BASE; // the display id (see SHOP_JUNK_BASE)
        char key[24];
        snprintf(key, sizeof key, "%lld", (long long) id);
        if (cJSON_HasObjectItem(products, key)) continue; // unreachable with the offset; kept as a guard
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
                                   KN_DB_ARGS(I64S(raw)));
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
        cJSON_AddNumberToObject(pr, "purchase_count", (double) (account > 0 ? bought_so_far(&x, account, raw) : 0));
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
        cJSON_AddNumberToObject(node, "order", (double) cJSON_GetArraySize(children));
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

/* A Box Office purchase of an official "limited" product (0354_store_catalog, verbatim from krunker.io's display).
 * Only KR products are sold: a virtual price in currency 1 (KR). Its price is debited through db_add_funds (the KR
 * ledger, kr_transactions, reason "shop"); its skins are granted through db_add_skin and its Junk (currency 2,
 * hundredths) through junk_credit; the row goes to shop_official_purchases. A product that is money (USD) or grants
 * a badge is refused before anything moves: Box Office has no payment and no badge grant. The limits (per_user or
 * global) count the purchase rows. Returns the reply JSON, or NULL with the refusal recorded. */
static cJSON *official_purchase(Dbx *x, const int64_t account, const int64_t product, const int64_t quantity) {
    // the product row lock, first: two purchases of a limited product queue here, so the second counts the first's row
    // (v10.0.6 "Fixed limited items sometimes being purchasable more than once"; the Junk products' run_purchase does
    // the same). Without it both read the same count and both pass the limit.
    if (dbx_exec(x, SQL(.any = "UPDATE shop_official_products SET active = active WHERE id = $1"), KN_DB_ARGS(I64S(product))) < 0)
        return NULL;
    cJSON *row = dbx_row(x, SQL(.any = "SELECT price_type, price_amount AS amount__i, price_currency_id AS cur__i,"
                                       " purchase_limit AS limit__i, purchase_limit_scope, active AS active__i"
                                       " FROM shop_official_products WHERE id = $1"),
                         KN_DB_ARGS(I64S(product)));
    if (dbx_failed(x)) {
        cJSON_Delete(row);
        return NULL;
    }
    if (!row || jint(row, "active") != 1) {
        cJSON_Delete(row);
        dbx_refuse(x, "Product not found");
        return NULL;
    }
    const char *ptype = jtext(row, "price_type");
    if (strcmp(ptype, "virtual") != 0 || jint(row, "cur") != 1) {
        cJSON_Delete(row);
        dbx_refuse(x, "Payment is not available on Box Office");
        return NULL;
    }
    const int64_t price = jint(row, "amount"), limit = jint(row, "limit");
    const int per_user = strcmp(jtext(row, "purchase_limit_scope"), "global") != 0;
    cJSON_Delete(row);

    cJSON *contents = dbx_rows(x, SQL(.any = "SELECT content_type, ref, quantity AS quantity__i FROM shop_official_contents"
                                             " WHERE product_id = $1 ORDER BY ord"),
                               KN_DB_ARGS(I64S(product)));
    if (dbx_failed(x)) {
        cJSON_Delete(contents);
        return NULL;
    }
    for (int i = 0; contents && i < cJSON_GetArraySize(contents); i++) {
        const cJSON *c0 = cJSON_GetArrayItem(contents, i);
        const char *type = jtext(c0, "content_type"), *ref = jtext(c0, "ref");
        const int junk = !strcmp(type, "currency") && !strcmp(ref, "2");
        if (strcmp(type, "skin") != 0 && !junk) {
            cJSON_Delete(contents);
            dbx_refuse(x, "Not sold on Box Office");
            return NULL;
        }
    }

    if (limit > 0) {
        const int64_t bought = per_user
            ? dbx_int(x, SQL(.any = "SELECT COALESCE(SUM(quantity), 0) FROM shop_official_purchases WHERE account_id = $1 AND product_id = $2"),
                      KN_DB_ARGS(I64S(account), I64S(product)), 0)
            : dbx_int(x, SQL(.any = "SELECT COALESCE(SUM(quantity), 0) FROM shop_official_purchases WHERE product_id = $1"),
                      KN_DB_ARGS(I64S(product)), 0);
        if (dbx_failed(x)) {
            cJSON_Delete(contents);
            return NULL;
        }
        if (bought + quantity > limit) {
            cJSON_Delete(contents);
            dbx_refuse(x, "Purchase limit reached");
            return NULL;
        }
    }

    const int64_t cost = price * quantity;
    char ref[48];
    snprintf(ref, sizeof ref, "shop:%lld", (long long) product);
    int64_t funds = 0;
    if (db_add_funds(x, account, -cost, "shop", ref, &funds) < 0) { // refused "Not enough KR" (recorded) or no account
        cJSON_Delete(contents);
        return NULL;
    }

    cJSON *rewards = cJSON_CreateArray();
    for (int i = 0; contents && i < cJSON_GetArraySize(contents) && !dbx_failed(x); i++) {
        const cJSON *c0 = cJSON_GetArrayItem(contents, i);
        const char *type = jtext(c0, "content_type"), *ref_text = jtext(c0, "ref");
        const int64_t amount = jint(c0, "quantity") * quantity;
        cJSON *rw = cJSON_CreateObject();
        if (!strcmp(type, "skin")) {
            const int64_t skin = strtoll(ref_text, NULL, 10);
            if (db_add_skin(x, account, skin, amount, "shop", ref, NULL) < 0) break;
            cJSON_AddStringToObject(rw, "type", "skin");
            cJSON_AddNumberToObject(rw, "skin_id", (double) skin);
            cJSON_AddNumberToObject(rw, "amount", (double) amount);
        } else {
            int64_t balance = 0;
            if (junk_credit(x, account, amount * 100, "shop", &balance) < 0) break;
            cJSON_AddStringToObject(rw, "type", "currency");
            cJSON_AddNumberToObject(rw, "currency_id", 2);
            cJSON_AddNumberToObject(rw, "currency", 2); /* v10.0.3 reads reward.currency (deob-10.0.3.js:130210-130214) */
            cJSON_AddNumberToObject(rw, "amount", (double) (amount * 100));
        }
        cJSON_AddItemToArray(rewards, rw);
    }
    cJSON_Delete(contents);
    if (dbx_failed(x)) {
        cJSON_Delete(rewards);
        return NULL;
    }
    if (dbx_exec(x, SQL(.any = "INSERT INTO shop_official_purchases (account_id, product_id, quantity, price_paid) VALUES ($1, $2, $3, $4)"),
                 KN_DB_ARGS(I64S(account), I64S(product), I64S(quantity), I64S(cost))) < 0) {
        cJSON_Delete(rewards);
        return NULL;
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddTrueToObject(o, "success");
    cJSON_AddStringToObject(o, "message", "Purchase successful");
    cJSON_AddItemToObject(o, "rewards", rewards);
    cJSON_AddNumberToObject(o, "currency_id", 1);
    cJSON_AddNumberToObject(o, "price_paid", (double) cost);
    cJSON_AddNumberToObject(o, "kr", (double) funds);
    return o;
}

static kn_db_result *run_purchase(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const int64_t account = PI(0), quantity = PI(2);
    int64_t product = PI(1);
    /* p3: v10.0.3's product_id, the price's price_ref (deob-10.0.3.js:132049): "kn.shop.<display id>" (a Junk shop
     * product, run_display) or an official product's own ref (shop_official_products.price_ref) */
    const char *price_ref = P(3) ? P(3) : "";
    if (product <= 0 && !strncmp(price_ref, "kn.shop.", 8)) {
        product = strtoll(price_ref + 8, NULL, 10);
    } else if (product <= 0 && price_ref[0]) {
        cJSON *hit = dbx_row(&x, SQL(.any = "SELECT id AS id__i FROM shop_official_products WHERE price_ref = $1 AND active = 1"
                                            " ORDER BY id LIMIT 1"),
                             KN_DB_ARGS(price_ref));
        if (dbx_failed(&x)) {
            cJSON_Delete(hit);
            return dbx_finish(&x, NULL);
        }
        product = hit ? jint(hit, "id") : 0;
        cJSON_Delete(hit);
    }
    if (quantity < 1 || quantity > SHOP_MAX_QUANTITY) {
        dbx_refuse(&x, "Invalid quantity.");
        return dbx_finish(&x, NULL);
    }
    if (product < SHOP_JUNK_BASE) {
        // below the offset: krunker.io's official product (0354_store_catalog), bought with KR (official_purchase)
        cJSON *o = official_purchase(&x, account, product, quantity);
        if (!o) return dbx_finish(&x, NULL);
        return dbx_json(&x, "shop_purchase", o);
    }
    product -= SHOP_JUNK_BASE;
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

/* ------------------------------------------------------------------ bundles */

/* P_SHOP_BUNDLES () -> the rows v10 receives as ALL_BUNDLE_DATA (deob-10.0.3.js 111331 "abd", handler 242852),
 * as an array of its keys: ID, NAME, PRICE, ORIGINAL_PRICE, DISCOUNT_PCT, SKINS, EXPIRY, INFO, FUNDS, JUNK,
 * PREMIUM, EVENT_NAME, EVENT_ENDS_AT. The table (0354_store_catalog) is staff-published; none ships. */
/* v10's bundle order (deob-10.0.3.js 144815-144830): its three stable sorts give owned bundles last, then the price
 * low first, then DATE newest first. The level rule (no price sort above level 10) is not kept: Box Office has
 * no account level in this path. */
static int bundle_order(const void *pa, const void *pb) {
    const cJSON *a = *(cJSON *const *) pa, *b = *(cJSON *const *) pb;
    const int oa = cJSON_IsTrue(cJSON_GetObjectItem(a, "OWNED")), ob = cJSON_IsTrue(cJSON_GetObjectItem(b, "OWNED"));
    if (oa != ob) return oa - ob;
    const double qa = jnum(a, "PRICE"), qb = jnum(b, "PRICE");
    if (qa != qb) return qa < qb ? -1 : 1;
    const int d = strcmp(jtext(b, "DATE"), jtext(a, "DATE"));
    if (d) return d;
    return jint(a, "ID") < jint(b, "ID") ? -1 : jint(a, "ID") > jint(b, "ID");
}

static kn_db_result *run_bundles(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const int64_t account = PI(0);
    cJSON *rows = dbx_rows(&x, SQL(.any = "SELECT id AS id__i, name, price AS price__f, original_price AS original__f,"
                                          " discount_pct AS discount__f, skins, expiry, info, funds AS funds__i,"
                                          " junk AS junk__f, premium AS premium__i, event_name, event_ends_at, bundle_date"
                                          " FROM store_bundles WHERE active = $1 ORDER BY sort_order, id"),
                           KN_DB_ARGS("1"));
    if (dbx_failed(&x)) {
        cJSON_Delete(rows);
        return dbx_finish(&x, NULL);
    }
    // the bundles this account owns (OWNED: its bundle ids; a guest owns none)
    cJSON *owned_rows = dbx_rows(&x, SQL(.any = "SELECT bundle_id AS id__i FROM store_bundle_purchases WHERE account_id = $1"),
                                 KN_DB_ARGS(I64S(account)));
    if (dbx_failed(&x)) {
        cJSON_Delete(owned_rows);
        cJSON_Delete(rows);
        return dbx_finish(&x, NULL);
    }
    cJSON *owned = cJSON_CreateObject();
    for (int i = 0; owned_rows && i < cJSON_GetArraySize(owned_rows); i++) {
        char key[24];
        snprintf(key, sizeof key, "%lld", (long long) jint(cJSON_GetArrayItem(owned_rows, i), "id"));
        cJSON_AddTrueToObject(owned, key);
    }
    cJSON_Delete(owned_rows);
    cJSON *out = cJSON_CreateArray();
    for (int i = 0; rows && i < cJSON_GetArraySize(rows); i++) {
        const cJSON *row = cJSON_GetArrayItem(rows, i);
        cJSON *b = cJSON_CreateObject();
        cJSON_AddNumberToObject(b, "ID", (double) jint(row, "id"));
        cJSON_AddStringToObject(b, "NAME", jtext(row, "name"));
        cJSON_AddNumberToObject(b, "PRICE", jnum(row, "price"));
        const cJSON *orig = cJSON_GetObjectItem(row, "original");
        const cJSON *disc = cJSON_GetObjectItem(row, "discount");
        const cJSON *exp = cJSON_GetObjectItem(row, "expiry");
        const cJSON *ev = cJSON_GetObjectItem(row, "event_name");
        const cJSON *evend = cJSON_GetObjectItem(row, "event_ends_at");
        if (cJSON_IsNumber(orig)) cJSON_AddNumberToObject(b, "ORIGINAL_PRICE", orig->valuedouble);
        else cJSON_AddNullToObject(b, "ORIGINAL_PRICE");
        if (cJSON_IsNumber(disc)) cJSON_AddNumberToObject(b, "DISCOUNT_PCT", disc->valuedouble);
        else cJSON_AddNullToObject(b, "DISCOUNT_PCT");
        cJSON_AddStringToObject(b, "SKINS", jtext(row, "skins"));
        if (cJSON_IsString(exp)) cJSON_AddStringToObject(b, "EXPIRY", exp->valuestring);
        else cJSON_AddNullToObject(b, "EXPIRY");
        cJSON_AddStringToObject(b, "INFO", jtext(row, "info"));
        cJSON_AddNumberToObject(b, "FUNDS", (double) jint(row, "funds"));
        cJSON_AddNumberToObject(b, "JUNK", jnum(row, "junk"));
        cJSON_AddNumberToObject(b, "PREMIUM", (double) jint(row, "premium"));
        if (cJSON_IsString(ev)) cJSON_AddStringToObject(b, "EVENT_NAME", ev->valuestring);
        else cJSON_AddNullToObject(b, "EVENT_NAME");
        if (cJSON_IsString(evend)) cJSON_AddStringToObject(b, "EVENT_ENDS_AT", evend->valuestring);
        else cJSON_AddNullToObject(b, "EVENT_ENDS_AT");
        char key[24];
        snprintf(key, sizeof key, "%lld", (long long) jint(row, "id"));
        cJSON_AddBoolToObject(b, "OWNED", cJSON_HasObjectItem(owned, key));
        cJSON_AddStringToObject(b, "DATE", jtext(row, "bundle_date"));
        cJSON_AddItemToArray(out, b);
    }
    cJSON_Delete(rows);
    cJSON_Delete(owned);
    const int count = cJSON_GetArraySize(out);
    cJSON **items = calloc(count > 0 ? (size_t) count : 1, sizeof *items);
    if (!items) {
        cJSON_Delete(out);
        dbx_refuse(&x, "internal");
        return dbx_finish(&x, NULL);
    }
    for (int i = 0; i < count; i++) items[i] = cJSON_DetachItemFromArray(out, 0);
    cJSON_Delete(out);
    qsort(items, (size_t) count, sizeof *items, bundle_order);
    cJSON *sorted = cJSON_CreateArray();
    for (int i = 0; i < count; i++) cJSON_AddItemToArray(sorted, items[i]);
    free(items);
    return dbx_json(&x, "shop_bundles", sorted);
}
const kn_proc P_SHOP_BUNDLES = {"shop_bundles", NULL, run_bundles};

/* P_SHOP_BUNDLE_BUY (account, bundle) -> the purchase's reply. Every bundle is priced in USD (v10 "BUY $PRICE"; its
 * purchase is the payment provider's, deob-10.0.3.js 242624): Box Office has no payment, so a bundle that is
 * not owned is refused "Payment is not available on Box Office", after the checks that come first: "Bundle not
 * found" (no active bundle with the id), "Already purchased" (an owned bundle). Nothing is written. */
static kn_db_result *run_bundle_buy(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const int64_t account = PI(0), bundle = PI(1);
    cJSON *row = dbx_row(&x, SQL(.any = "SELECT id AS id__i FROM store_bundles WHERE id = $1 AND active = $2"),
                         KN_DB_ARGS(I64S(bundle), "1"));
    if (dbx_failed(&x)) {
        cJSON_Delete(row);
        return dbx_finish(&x, NULL);
    }
    if (!row) {
        dbx_refuse(&x, "Bundle not found");
        return dbx_finish(&x, NULL);
    }
    cJSON_Delete(row);
    const int64_t owned = dbx_int(&x, SQL(.any = "SELECT COUNT(*) FROM store_bundle_purchases WHERE account_id = $1 AND bundle_id = $2"),
                                  KN_DB_ARGS(I64S(account), I64S(bundle)), 0);
    if (dbx_failed(&x)) return dbx_finish(&x, NULL);
    dbx_refuse(&x, owned > 0 ? "Already purchased" : "Payment is not available on Box Office");
    return dbx_finish(&x, NULL);
}
const kn_proc P_SHOP_BUNDLE_BUY = {"shop_bundle_buy", NULL, run_bundle_buy};
