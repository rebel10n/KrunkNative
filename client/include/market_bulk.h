#pragma once
// Item locks and the bulk market actions of Krunker v10.0.4 (client-side state and request bodies only: the
// krunker.io calls are the networking layer's, not this file's). Ground truth: deob-10.0.4.js.
//   - locks: GET / POST /items/locks; the lock state is two sets, ids (skin ids) and stacks (item ids of
//     stackable items), read by the bundle's locked check (deob-10.0.4.js:108837-108853, :147248-147256).
//     KrunkNative's rows have no instance ids, so a row's id is its skin id.
//   - select mode: a set of rows picked for one bulk action (quick-sell, list). A locked row is never picked.
//   - bodies: POST /market/quick-sell/bulk {action, items:[{skin_id, item_id}]} in chunks of 250
//     (deob-10.0.4.js:146851-146854), POST /market/list/bulk {items:[{item_id, skin_id, price, quantity}]}
//     (deob-10.0.4.js:131978-131985), POST /market/buy-multiple {item_id, quantity, max_total}
//     (deob-10.0.4.js:132755-132758), POST /items/locks {skin_id | item_id, locked} (deob-10.0.4.js:108862-108866).
//   - errors: a MARKET_BUSY answer is "Server is busy. Please try again in a few seconds." (errors.exchange.busy,
//     deob-10.0.4.js:104255 and :236406).
#include <stddef.h>

#include <cJSON.h>

#define MARKET_BULK_QUICK_SELL_CHUNK 250 // deob-10.0.4.js:146851 (slice of 0xfa)
#define MARKET_BULK_LIST_CHUNK 100 // deob-10.0.4.js:131955 (0x64 quantity per request)

// The lock state, as the last GET /items/locks answered it.
typedef struct {
    int *skin_ids; // the skin ids locked (ids)
    int skin_n;
    int *item_ids; // the item ids of locked stacks (stacks)
    int item_n;
} MarketLocks;

// Replaces the lock state with a GET /items/locks answer: data.locks[] of {skin_id, item_id, stack}.
// A null or malformed list leaves no locks. The state is owned by the caller and freed by market_locks_free.
void market_locks_apply(MarketLocks *locks, const cJSON *rows);
// True when the row is locked: its skin id is in ids, or it is a stack whose item id is in stacks
// (deob-10.0.4.js:147254). stackable is the row's stackable flag.
int market_locks_has(const MarketLocks *locks, int skin_id, int item_id, int stackable);
// Frees the lock state; the struct is empty afterwards.
void market_locks_free(MarketLocks *locks);

// The POST /items/locks body for one row: {skin_id} when the row has a skin id, else {item_id}, plus locked.
// Caller frees the returned cJSON.
cJSON *market_lock_body(int skin_id, int item_id, int locked);

// A select-mode pick: rows in pick order, each with its skin and item id. A locked row is refused.
typedef struct {
    int *skin_ids;
    int *item_ids;
    int n;
    int cap;
} MarketPicks;

// Toggles a row in or out of the picks. Returns 1 when it is picked now, 0 when it is not (removed, or
// refused because the row is locked, or the picks are full).
int market_picks_toggle(MarketPicks *picks, const MarketLocks *locks, int skin_id, int item_id, int stackable);
// Frees the picks.
void market_picks_free(MarketPicks *picks);

// The POST /market/quick-sell/bulk body for picks [from, from + count): {action, items:[{skin_id, item_id}]}.
// action: 0 Get KR, 1 Get Junk, 2 Destroy (the single quick-sell's modes, v10_market.c qsi). Caller frees.
cJSON *market_quick_sell_body(int action, const MarketPicks *picks, int from, int count);

// How many picks from `from` one bulk-list request takes: their quantities total at most 100
// (deob-10.0.4.js:131955-131963). quantity[i] is pick i's quantity (a stack's count, else 1).
int market_list_fit(const MarketPicks *picks, const int *quantity, int from);

// The POST /market/list/bulk body for picks [from, from + count): {items:[{item_id, skin_id, price, quantity}]}
// (deob-10.0.4.js:131978-131985). price is one unit price for every pick. Caller frees.
cJSON *market_list_body(const MarketPicks *picks, const int *quantity, int price, int from, int count);

// The POST /market/buy-multiple body: {item_id, quantity, max_total}. Caller frees.
cJSON *market_buy_multiple_body(int item_id, int quantity, double max_total);

// One item of GET /market/inventory: its stack (stack_id, stack_max) and its copies (instances). A stack and
// the instances of one item can both be there (deob-10.0.4.js:132190-132209).
typedef struct {
    int item;       // the item index (the market card's index)
    int stack_id;   // the stack's id (its skin_id in a bulk request), 0 when the item has no stack
    int stack_max;  // the stack's count
    int *instances; // each copy's id (its skin_id in a bulk request)
    char **paints;  // each copy's paint key (market_paint_key), NULL when unpainted; parallel to instances
    int n_instances;
} MarketGroup;

// The paint a copy's metadata (deob-10.0.4.js:106577-106578: metadata.paint, the colour with its '#' dropped, and
// metadata.paintShader) as the key a match compares. malloc'd; NULL when the copy is unpainted.
char *market_paint_key(const cJSON *meta);
// Frees a group's instances and paint keys (not the group itself).
void market_group_clear(MarketGroup *g);

// The rows {i, st, sid, cnt} of GET /market/inventory, one group per item index, in the answer's order (st 1 is
// a stack: sid its id, cnt its count; any other row is one copy: sid its id). *out is NULL when there are none.
// Caller frees with market_groups_free. Returns 0 on success.
int market_groups_parse(const cJSON *rows, MarketGroup **out, int *n);
void market_groups_free(MarketGroup *groups, int n);

// The i18n key of a market error code (MARKET_BUSY, ...), or NULL when the code has no mapped text.
// The caller shows I18N(key, default text).
const char *market_error_key(const char *code);
