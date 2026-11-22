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
