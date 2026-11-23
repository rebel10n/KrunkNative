// krunker.io's market reads and the Black Market buy: paths, bodies and the answers as the economy's rows
// (kio_market.h). Pure: no network, no UI.
#include <kio_market.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

double kio_market_time_ms(const cJSON *v) {
    if (cJSON_IsNumber(v)) return v->valuedouble;
    if (!cJSON_IsString(v)) return 0;
    int y, mo, d, h = 0, mi = 0;
    double s = 0;
    if (sscanf(v->valuestring, "%d-%d-%d%*c%d:%d:%lf", &y, &mo, &d, &h, &mi, &s) < 3) return 0;
    y -= mo <= 2; // days from the civil date (UTC; no timegm on every platform)
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned) (y - era * 400);
    const unsigned doy = (153u * (unsigned) (mo + (mo > 2 ? -3 : 9)) + 2u) / 5u + (unsigned) d - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    const double days = (double) era * 146097.0 + (double) doe - 719468.0;
    return ((days * 24.0 + h) * 60.0 + mi) * 60000.0 + s * 1000.0;
}

static size_t put(char *out, const size_t n, const int m) {
    if (m <= 0 || (size_t) m >= n) {
        if (out && n) out[0] = 0;
        return 0;
    }
    return (size_t) m;
}

static double num(const cJSON *o, const char *k, const double dflt) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsNumber(v)) return v->valuedouble;
    if (cJSON_IsString(v) && v->valuestring[0]) { // a numeric string (a bigint column sent as text)
        char *end;
        const double d = strtod(v->valuestring, &end);
        if (end != v->valuestring && !*end) return d;
    }
    return dflt;
}

static const char *str(const cJSON *o, const char *k) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) ? v->valuestring : "";
}

// the History tab's first page: sort "date" + order "desc" is date_desc, 50 rows (deob-10.0.7.js:135772-135790,
// market.getHistory :27050-27063)
size_t kio_market_history_path(char *out, const size_t n) { return put(out, n, snprintf(out, n, "/market/history?sort=date_desc&limit=50")); }

cJSON *kio_market_history_rows(const cJSON *data) {
    cJSON *rows = cJSON_CreateArray();
    const cJSON *r;
    cJSON_ArrayForEach(r, cJSON_GetObjectItemCaseSensitive(data, "history")) {
        const double sk = num(r, "skinindex", -1);
        if (sk < 0) continue;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "type", str(r, "type"));
        cJSON_AddNumberToObject(o, "date", kio_market_time_ms(cJSON_GetObjectItemCaseSensitive(r, "date")));
        cJSON_AddNumberToObject(o, "skinindex", sk);
        cJSON_AddNumberToObject(o, "amount", num(r, "amount", 0));
        cJSON_AddStringToObject(o, "who", str(r, "other_party_name"));
        cJSON_AddStringToObject(o, "status", str(r, "status"));
        cJSON_AddItemToArray(rows, o);
    }
    return rows;
}

size_t kio_market_view_path(char *out, const size_t n, const int item) {
    if (item < 0) return put(out, n, 0);
    return put(out, n, snprintf(out, n, "/items/%d/view", item));
}

size_t kio_market_sales_path(char *out, const size_t n, const int item, const int days_or_range) {
    if (item < 0) return put(out, n, 0);
    if (days_or_range >= 0 && days_or_range < 7) return put(out, n, snprintf(out, n, "/items/%d/sales?range=%d", item, days_or_range));
    return put(out, n, snprintf(out, n, "/market/items/%d/sales-statistics?daysback=%d", item, days_or_range));
}

cJSON *kio_market_sales_rows(const cJSON *data) {
    cJSON *rows = cJSON_CreateArray();
    const cJSON *r;
    cJSON_ArrayForEach(r, data) {
        const char *d = str(r, "d");
        if (strlen(d) < 10) continue;
        char day[11];
        memcpy(day, d, 10);
        day[10] = 0;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "d", day);
        cJSON_AddNumberToObject(o, "f", num(r, "f", 0));
        cJSON_AddNumberToObject(o, "t", num(r, "t", 0));
        cJSON_AddItemToArray(rows, o);
    }
    return rows;
}

static int cmp_price(const void *pa, const void *pb) {
    const double a = num(*(cJSON *const *) pa, "f", 0), b = num(*(cJSON *const *) pb, "f", 0);
    return a < b ? -1 : a > b ? 1 : 0;
}

cJSON *kio_market_view_listings(const cJSON *listings, const int item) {
    const int n = cJSON_GetArraySize(listings);
    cJSON **v = calloc((size_t) (n > 0 ? n : 1), sizeof(*v));
    int k = 0;
    const cJSON *l;
    cJSON_ArrayForEach(l, listings) {
        if (!v || !cJSON_IsObject(l) || num(l, "skinindex", item) != item) continue;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "m", num(l, "marketid", 0));
        cJSON_AddNumberToObject(o, "i", item);
        cJSON_AddNumberToObject(o, "f", num(l, "price", num(l, "funds", 0)));
        cJSON_AddStringToObject(o, "p", str(l, "seller_name"));
        cJSON_AddNumberToObject(o, "ms", kio_market_time_ms(cJSON_GetObjectItemCaseSensitive(l, "listed_time")));
        if (cJSON_GetObjectItemCaseSensitive(l, "seller_id")) cJSON_AddNumberToObject(o, "seller_id", num(l, "seller_id", 0));
        v[k++] = o;
    }
    if (v) qsort(v, (size_t) k, sizeof(*v), cmp_price);
    cJSON *rows = cJSON_CreateArray();
    for (int i = 0; i < k; i++) cJSON_AddItemToArray(rows, v[i]);
    free(v);
    return rows;
}

cJSON *kio_market_bm_body(const int item) {
    cJSON *b = cJSON_CreateObject();
    cJSON_AddNumberToObject(b, "item_index", item);
    return b;
}

cJSON *kio_market_bm_reply(const cJSON *data, const int item, const double funds_now) {
    if (!cJSON_IsObject(data)) return NULL;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "item", num(data, "item", item));
    cJSON_AddNumberToObject(o, "kr", num(data, "kr", 0));
    cJSON_AddNumberToObject(o, "f", num(data, "funds", funds_now));
    // the item's title, granted with it (deob-10.0.3.js:152803-152806)
    if (cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(data, "title"))) cJSON_AddNumberToObject(o, "title", num(data, "title", -1));
    return o;
}

cJSON *kio_market_inventory_rows(const cJSON *data, const int copy_base) {
    cJSON *rows = cJSON_CreateArray();
    const cJSON *r;
    const cJSON *list = cJSON_IsArray(data) ? data : NULL;
    int copies = 0;
    cJSON_ArrayForEach(r, list) {
        const cJSON *i = cJSON_GetObjectItemCaseSensitive(r, "i"), *cnt = cJSON_GetObjectItemCaseSensitive(r, "cnt");
        if (!cJSON_IsNumber(i) || (cnt && !cJSON_IsNull(cnt) && num(r, "cnt", 0) <= 0)) continue; // deob-10.0.7.js:134361
        const int stack = num(r, "st", 0) == 1;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "i", i->valuedouble);
        cJSON_AddNumberToObject(o, "st", stack ? 1 : 0);
        cJSON_AddNumberToObject(o, "c", stack ? num(r, "cnt", 1) : 1);
        cJSON_AddNumberToObject(o, "sid", stack ? i->valuedouble : (double) copy_base + copies++);
        const double kid = num(r, "sid", 0);
        if (kid > 0) cJSON_AddNumberToObject(o, "kid", kid);
        const cJSON *m = cJSON_GetObjectItemCaseSensitive(r, "m");
        if (cJSON_IsObject(m) || (cJSON_IsString(m) && m->valuestring[0])) cJSON_AddItemToObject(o, "m", cJSON_Duplicate(m, 1));
        if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r, "nt")) || num(r, "nt", 0) > 0) cJSON_AddNumberToObject(o, "nt", 1);
        // a listed unit: its listing's marketid (the row is not bulk-selectable: deob-10.0.7.js:135063-135064)
        const double market_id = num(r, "marketid", 0);
        if (market_id > 0) cJSON_AddNumberToObject(o, "marketid", market_id);
        cJSON_AddItemToArray(rows, o);
    }
    return rows;
}

// iíïííiï, deob-10.0.7.js:104431-104501
static const struct {
    const char *code, *key;
} k_error_keys[] = {
    {"MARKET_DISABLED", "errors.exchange.marketDisabled"},
    {"MARKET_LEVEL_TOO_LOW", "errors.exchange.levelTooLow"},
    {"MARKET_ITEM_NOT_FOUND", "errors.exchange.itemNotFound"},
    {"MARKET_ITEM_ALREADY_SOLD", "errors.exchange.itemAlreadySold"},
    {"MARKET_MAX_ITEMS", "errors.exchange.maxItemsOnSale"},
    {"MARKET_PRICE_TOO_HIGH", "errors.exchange.priceTooHigh"},
    {"MARKET_PRICE_TOO_LOW", "errors.exchange.priceTooLow"},
    {"MARKET_INVALID_ACCOUNT", "errors.exchange.invalidAccount"},
    {"MARKET_ITEM_NOT_SELLABLE", "errors.exchange.itemNotSellable"},
    {"MARKET_RATE_LIMITED", "errors.exchange.rateLimited"},
    {"MARKET_INSUFFICIENT_FUNDS", "errors.exchange.insufficientFunds"},
    {"MARKET_ITEM_NOT_OWNED", "errors.exchange.itemNotOwned"},
    {"MARKET_ITEM_ALREADY_LISTED", "errors.exchange.itemAlreadyListed"},
    {"MARKET_LISTING_GONE", "errors.exchange.listingGone"},
    {"MARKET_SELF_PURCHASE", "errors.exchange.selfPurchase"},
    {"MARKET_MAINTENANCE", "errors.exchange.maintenance"},
    {"MARKET_BUSY", "errors.exchange.busy"},
    {"MARKET_QUICK_SELL_NOT_SELLABLE", "errors.exchange.quickSellNotSellable"},
    {"MARKET_QUICK_SELL_NO_VALUE", "errors.exchange.quickSellNoValue"},
    {"MARKET_QUICK_SELL_INVALID_ACTION", "errors.exchange.quickSellInvalidAction"},
    {"MARKET_QUICK_SELL_INVALID_AMOUNT", "errors.exchange.quickSellInvalidAmount"},
    {"MARKET_QUICK_SELL_FAILED", "errors.exchange.quickSellFailed"},
    {"MARKET_PRICE_CHANGED", "errors.exchange.priceChanged"},
    {"MARKET_NOT_ENOUGH_LISTINGS", "errors.exchange.notEnoughListings"},
    {"MARKET_ITEM_LOCKED", "errors.exchange.itemLocked"},
    {"ITEM_LOCK_LISTED", "errors.exchange.itemLockListed"},
    {"ITEM_LOCK_NOT_OWNED", "errors.exchange.itemLockNotOwned"},
    {"TRADE_ITEM_NOT_OWNED", "errors.exchange.tradeItemNotOwned"},
    {"TRADE_PLAYER_NOT_FOUND", "errors.exchange.tradePlayerNotFound"},
    {"TRADE_MESSAGE_TOO_LONG", "errors.exchange.tradeMessageTooLong"},
    {"TRADE_INVALID_OFFER", "errors.exchange.tradeInvalidOffer"},
    {"TRADE_MAX_ITEMS_EXCEEDED", "errors.exchange.tradeMaxItems"},
    {"TRADE_PROC_ERROR", "errors.exchange.tradeProcError"},
    {"TRADE_ITEM_LOCKED", "errors.exchange.tradeItemLocked"},
    {"TRADE_LEVEL_TOO_LOW", "errors.exchange.tradeLevelTooLow"},
    {"TRADE_ITEM_NOT_TRADABLE", "errors.exchange.tradeItemNotTradable"},
    {"PLAYERS_NOT_FOUND", "errors.profiles.notFound"},
    {"PLAYERS_SEARCH_INVALID", "errors.profiles.searchInvalid"},
    {"PLAYERS_SEARCH_LIMIT_INVALID", "errors.profiles.searchLimitInvalid"},
    {"PLAYERS_INVALID_RANGE", "errors.profiles.invalidRange"},
    {"PLAYERS_PROFILE_FAILED", "errors.profiles.profileFailed"},
    {"PLAYERS_SALES_FAILED", "errors.profiles.salesFailed"},
    {"PLAYERS_SEARCH_FAILED", "errors.profiles.searchFailed"},
    {"PLAYERS_PROXY_FAILED", "errors.profiles.proxyFailed"},
    {"EB_AUTH_REQUIRED", "errors.eventBus.authRequired"},
    {"EB_AUTH_INVALID", "errors.eventBus.authInvalid"},
    {"EB_TARGET_OFFLINE", "errors.eventBus.targetOffline"},
    {"EB_NOT_FRIEND", "errors.eventBus.notFriend"},
    {"EB_RATE_LIMITED", "errors.eventBus.rateLimited"},
    {"EB_MSG_INVALID", "errors.eventBus.msgInvalid"},
    {"EB_TEXT_TOO_LONG", "errors.eventBus.textTooLong"},
    {"EB_UNKNOWN_TYPE", "errors.eventBus.unknownType"},
    {"EB_INTERNAL", "errors.eventBus.internal"},
    {"ITEMS_INVALID_ID", "errors.items.invalidId"},
    {"ITEMS_INVALID_RANGE", "errors.items.invalidRange"},
    {"ITEMS_STATS_FAILED", "errors.items.statsFailed"},
    {"ITEMS_OWNERS_FAILED", "errors.items.ownersFailed"},
    {"ITEMS_SALES_FAILED", "errors.items.salesFailed"},
    {"SOCIAL_RATE_LIMITED", "errors.social.rateLimited"},
    {"SOCIAL_INSUFFICIENT_FUNDS", "errors.social.insufficientFunds"},
    {"GIFT_INSUFFICIENT_FUNDS", "errors.social.giftInsufficientFunds"},
    {"SOCIAL_GIVEAWAY_TOO_SMALL", "errors.social.giveawayTooSmall"},
    {"SOCIAL_POST_TEXT_REQUIRED", "errors.social.postTextRequired"},
    {"SOCIAL_POST_NOT_FOUND", "errors.social.postNotFound"},
    {"SOCIAL_CREATE_FAILED", "errors.social.createFailed"},
    {"SOCIAL_COMMENT_TEXT_REQUIRED", "errors.social.commentTextRequired"},
    {"SOCIAL_ALREADY_VOTED", "errors.social.alreadyVoted"},
    {"SOCIAL_LEVEL_TOO_LOW", "errors.social.levelTooLow"},
    {"SOCIAL_GIVEAWAY_LEVEL_TOO_LOW", "errors.social.giveawayLevelTooLow"},
};

const char *kio_market_error_key(const char *code) {
    if (!code || !code[0]) return NULL;
    for (size_t i = 0; i < sizeof(k_error_keys) / sizeof(k_error_keys[0]); i++)
        if (!strcmp(code, k_error_keys[i].code)) return k_error_keys[i].key;
    return "errors.common.unknown"; // iiîííii (:104502)
}
