#pragma once
// Krunker v10.0.4's bulk market overlays for a krunker.io account (client/src/ui/v10_bulk.c), pushed on the v10 view
// stack over the Wallet or the market:
//   - the bulk quick-sell confirm (svelte-1pxa8ry, deob-10.0.4.js:146908-147037): the picks of one action grouped by
//     item (8 shown, "and N more"), what they pay, the warning, a progress bar while the run goes, Cancel / Confirm;
//     the result is a notification (deob-10.0.4.js:147352-147369)
//   - the List Items modal (svelte-1u5rc76, deob-10.0.4.js:131877-132185): one row per picked item, a stack's quantity
//     and each row's price, the listings / fee / value summary, the fee against the balance, a progress bar
//   - Buy Multiple (svelte-dk2h1b, deob-10.0.4.js:132755-132945): the buy ladder of an item, its quantity (input,
//     chips, range), the total, the highest price paid and the balance, then POST market/buy-multiple
// The writes go through kio_inventory.c / kio_store_write: the official ids only.
#include <market_bulk.h>
#include <kio_inventory.h>
#include <html_dom.h>

// Opens the bulk quick-sell confirm for picks (action: 0 KR, 1 Junk, 2 Destroy). Takes ownership of picks. after runs
// once the run ends (ok 1) or the overlay is cancelled (ok 0, message NULL).
void v10_bulk_sell_open(int action, MarketPicks picks, KioInvDone after, void *user);
// Opens the List Items modal for picks (official ids from the inventory's groups). Takes ownership of picks.
void v10_bulk_list_open(MarketPicks picks, KioInvDone after, void *user);
// Opens Buy Multiple for a store item (krunker.io's listings).
void v10_bulk_buy_open(int item, KioInvDone after, void *user);

// The picks' totals for the wallet's bar (deob-10.0.4.js:147346-147351): the ones each action takes, and what the
// KR and Junk ones pay. n_* may be NULL.
void v10_bulk_sums(const MarketPicks *picks, int *n_kr, int *n_junk, int *n_destroy, double *kr, double *junk);
// The picks an action takes (a copy of picks filtered by it). Caller frees.
MarketPicks v10_bulk_filter(const MarketPicks *picks, int action);

// --render-scenario state.bulk: {kind: "sell" | "list" | "buy", action, items: [[item, skin_id], ...], price, prices: [...]}
// opens that overlay with those picks (a list's rows are copies: the scenario has no official inventory) or that ladder
void v10_bulk_scenario(const cJSON *state);

int  v10_bulk_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, int argc); // knV10Bulk*; -1 not ours
void v10_bulk_tick(void); // a run's progress bar (the wallet's and the market's ticks)
int  v10_bulk_open(void); // an overlay is up
