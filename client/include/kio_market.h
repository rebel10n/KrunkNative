#pragma once
// krunker.io's market reads and the Black Market buy for a krunker.io account (client/src/ui/kio_market.c): the
// gapi paths v10 asks and its answers turned into the rows v10_market.c already draws (the economy socket's rows).
// Pure: no network, no UI. The routes (doc/v10/api-inventory.md rows 131, 138-141; v10.js lines of v10.0.2's client
// _10077, the readers' deob-10.0.3.js lines):
//   GET  /market/history?sort=date_desc&limit=50     market.getHistory (v10:26948, deob-10.0.7.js:27050; reader deob-10.0.3.js:147567, 150717)
//   GET  /items/<i>/view                             items.getView (v10:26957; reader deob-10.0.3.js:122532, 123271-123335)
//   GET  /market/items/<i>/sales-statistics?daysback=<d>   market.getItemSalesStatistics (v10:26895; reader :151414)
//   GET  /items/<i>/sales?range=<r>                  items.getSales (v10:26963; reader :123825)
//   POST market/black-market/purchase {item_index}   market.purchaseBlackMarketItem (v10:26934; reader :152786-152831)
//   GET  /market/inventory                           market.getInventory (deob-10.0.7.js:26949; reader :109077-109084, the
//                                                    My Inventory tab's load :134356-134368)
#include <cJSON.h>
#include <stddef.h>

// ISO 8601 ("2026-11-20T10:00:00.000Z", "2026-11-20 10:00:00") or epoch ms as epoch ms (v10: new Date(x)); 0 unknown
double kio_market_time_ms(const cJSON *v);

// The history read's path. Returns the length (0 when it does not fit).
size_t kio_market_history_path(char *out, size_t n);
// GET /market/history's data {history: [{type, date, skinindex, amount, other_party_name, status}], has_more,
// next_cursor} as the History table's rows {type, date: epoch ms, skinindex, amount, who, status}. A row with no
// skinindex is dropped. Returns a new array (cJSON_Delete it); an empty one for anything that is not that object.
cJSON *kio_market_history_rows(const cJSON *data);

// "/items/<i>/view". Returns the length (0 when it does not fit or item < 0).
size_t kio_market_view_path(char *out, size_t n, int item);
// The item detail's sales read: a range index 0..6 (the Catalog item page's ranges) is items.getSales
// "/items/<i>/sales?range=<r>", anything else a number of days for Listing Details' Stats tab,
// "/market/items/<i>/sales-statistics?daysback=<d>". Returns the length (0 when it does not fit or item < 0).
size_t kio_market_sales_path(char *out, size_t n, int item, int days_or_range);
// A sales answer ([{d, f, t}]: the day, its KR, its units) as the chart's rows: d cut to "YYYY-MM-DD" (an ISO
// timestamp keeps its date), f and t numbers. Returns a new array (cJSON_Delete it).
cJSON *kio_market_sales_rows(const cJSON *data);
// The view's listings ({marketid, skinindex, price | funds, seller_name, listed_time, seller_id}) as the economy's
// listing rows {m, i, f, p, ms[, seller_id]} of the item (rows of another item are dropped), cheapest first, as the
// brain sends them. Returns a new array (cJSON_Delete it).
cJSON *kio_market_view_listings(const cJSON *listings, int item);

// The Black Market buy's body {item_index}. Returns a new object (cJSON_Delete it).
cJSON *kio_market_bm_body(int item);
// Its answer {item, kr, funds[, title]} as the economy's "bmr" object {item, kr, f[, title]}; f is the account's
// funds when the answer has none (funds_now). NULL when data is not an object (the purchase failed).
cJSON *kio_market_bm_reply(const cJSON *data, int item, double funds_now);

// GET /market/inventory's data, an array of rows {i, st, sid, cnt, m} (v10.0.7 keeps the rows whose cnt is absent or > 0,
// deob-10.0.7.js:134361; a row with st 1 is a stack, any other one copy: :135061), as the Inventory tab's rows
// {i, st, c, sid, kid[, m][, nt]}: c is the stack's cnt (a copy is one unit), kid the official sid the writes send as
// skin_id (listItem :109104-109108, quickSellItem :132700-132706). sid is KrunkNative's unit id, as the brain's rows
// have it: a stack's is its item index, a copy's copy_base + its position in the answer (krunker.io's instance ids
// are not in KrunkNative's copy id space). A row without a numeric i is dropped. Returns a new array (cJSON_Delete it);
// an empty one for anything that is not an array.
cJSON *kio_market_inventory_rows(const cJSON *data, int copy_base);

// The i18n key v10.0.7 shows for a gapi error's code (iíiììiì with its code table iíïííiï, deob-10.0.7.js:104431-104513):
// the table's key, "errors.common.unknown" for a code not in it (v10 then shows that text, not the message: :104510),
// NULL when there is no code (the message or the caller's fallback is shown).
const char *kio_market_error_key(const char *code);
