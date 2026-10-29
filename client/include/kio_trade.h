#pragma once
// krunker.io trades (client/src/ui/kio_trade.c): the request paths and bodies of v10.0.4's trades API
// (deob-10.0.4.js:27162-27193 createTrade / acceptTrade / cancelTrade / getInventory / searchPlayer), sent with
// kio_store_write. The answers go to the market's own trade handlers (v10_market.c), as its other actions do.
#include <economy_client.h> // EconTradeItem: one unit of an offer (a stack, or a painted copy with its sid)
#include <cJSON.h>
#include <stddef.h>

// The offer as the bundle serializes it (deob-10.0.4.js:111287-111297): {i, c} per stack, {i, c, iids: [sid]} per
// copy. The units come from the official trade inventories (kio_trade_inventory), so a copy's sid is its official
// instance id. *copies (may be NULL) counts the copy units. Returns a new array (cJSON_Delete it).
cJSON *kio_trade_offer(const EconTradeItem *items, int n, int *copies);

// One side of GET trades/inventory/<id> (my_inventory / their_inventory: rows {i, st, sid, cnt | c, m, nt},
// deob-10.0.4.js:111208-111262) as the rows the Create Trade window reads: a stack (st 1) is {i, c: its count} with no
// sid; any other row is one copy {i, c: 1, sid: its instance id, m}. nt is kept. Returns a new array (cJSON_Delete it).
cJSON *kio_trade_inventory(const cJSON *official);

// POST trades/create's body {target_player_id, my_offer, their_offer, message} (deob-10.0.4.js:111326-111331).
// Takes ownership of my_offer and their_offer; returns a new object (cJSON_Delete it).
cJSON *kio_trade_create_body(int target_player_id, cJSON *my_offer, cJSON *their_offer, const char *message);

// The trade's path under the API root: action "accept" or "cancel" gives trades/<id>/<action> (:27172, :27177);
// "inventory" gives trades/inventory/<id> (:27188). The decline of an incoming offer is the cancel path too.
void kio_trade_path(char *out, size_t n, const char *action, int id);

// One official trade row (trades/history's open and completed, deob-10.0.4.js:27155) as the native trade row that
// v10_market.c's offer_html reads: trid, pl, player, buyer, message and dte keep their names, sksell / skbuy become
// the stack units of each side ("ind,ind,"), krsell / krbuy the side's KR, jnsell / jnbuy its JNK and rpsell / rpbuy
// its RP (deob-10.0.4.js:133113-133117). other_cur is 1 when a side carries a currency id other than 1-3, which the
// native view cannot show. Returns a new object (cJSON_Delete it).
cJSON *kio_trade_row(const cJSON *official);
