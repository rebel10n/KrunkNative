#pragma once
#include <cJSON.h>

struct ViewerSkin; // the store table entry (viewer_store.h)

// One row of a Create Trade window's inventory side (v10_market.c ct_side): a store skin and its units. A
// painted copy is its own row (sid its copy id, count 1; doc/v10/paint-copy.md section 5); a stack has sid 0.
typedef struct {
    int skin, count, locked; // locked: the row's "nt" (not tradable)
    int sid;                 // the copy id of a painted copy row (its "sid"), 0 for a stack
    const cJSON *row;        // the inventory row itself (its paint: "m", or paint_color / paint_shader)
} TradeListRow;

// Case-insensitive "hay contains needle" (an empty needle matches, a null hay does not): v10's text search.
int market_text_contains(const char *hay, const char *needle);

// The rows one trade inventory side draws. `inv` holds the side's rows {i, c, nt}; `other` the other side's
// (read only with `missing`: the rows the other player already owns are left out). `skin` looks a store
// index up (viewer_skin: the editor's overlay first); NULL = not in the table. A row is kept when its skin
// exists, it has copies, and `search` (empty = any) matches the skin's name either way round. The kept rows
// are sorted as v10 sorts them (rarity, season, index, all descending) and the first `limit` go to `out`
// (room for `limit`); `*total` gets every kept row. One pass over `inv` (plus a sort of the `other` indices
// when `missing`) and a sort of the kept rows: the markup is bounded by `limit`, not by the inventory's size.
// Returns the number of rows written to `out`.
int market_trade_list_select(const struct ViewerSkin *(*skin)(int index), const cJSON *inv, const cJSON *other,
                             const char *search, int missing, int limit, TradeListRow *out, int *total);
