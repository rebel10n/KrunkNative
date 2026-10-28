#pragma once
// The official inventory of a krunker.io account (client/src/ui/kio_inventory.c): GET market/inventory (the
// groups, market_bulk.h) and GET items/locks, loaded through kio_store_write with the account's Bearer token
// (deob-10.0.4.js:108887-108905 for the inventory, :108837-108853 for the locks). Its ids are the ones the
// official writes take, so an action here never names an item KrunkNative's own inventory shows. KrunkNative's
// brain rows are not used for these actions.
//   - lock toggle: POST items/locks, {skin_id} for a copy, {item_id} for a stack (deob-10.0.4.js:108862)
//   - bulk quick-sell: POST market/quick-sell/bulk in chunks of 250 (deob-10.0.4.js:146851)
//   - bulk list: POST market/list/bulk, the rows' quantities total 100 per request (deob-10.0.4.js:131955)
// Refused (done = 0, with kio_inventory_gate_text) until the inventory has loaded for this account.
#include <market_bulk.h>

// done: the action ended (ok = 1, message NULL) or failed (ok = 0, message: the server's text, or the busy text).
typedef void (*KioInvDone)(void *user, int ok, const char *message);

int  kio_inventory_ready(void);                     // loaded for the account now
int  kio_inventory_loading(void);                   // a load is in flight
void kio_inventory_load(KioInvDone done, void *user); // (re)load: done runs once when it ends (NULL: no callback)
void kio_inventory_clear(void);                     // the account changed or signed out: forget it

const MarketGroup *kio_inventory_groups(int *n);    // the groups of the last load (NULL when none)
const MarketLocks *kio_inventory_locks(void);       // the lock sets of the last load
int  kio_inventory_locked(const MarketGroup *g, int copy); // copy: index into instances, -1 the stack

// Lock or unlock one copy (copy >= 0) or the stack (copy -1). 1 = sent (done runs when it ends).
int  kio_inventory_set_lock(const MarketGroup *g, int copy, int locked, KioInvDone done, void *user);
// The official instance id (the bundle's instanceId, deob-10.0.4.js:131141, :133997) of the item's one copy whose
// paint key is key (market_paint_key), or 0 when no copy or more than one has that paint (the pairing is not certain).
int  kio_inventory_copy_for_paint(int item, const char *key);
// Bulk quick-sell of picks (action: 0 Get KR, 1 Get Junk, 2 Destroy). Takes ownership of picks: they are freed
// when the run ends. 1 = sent.
int  kio_inventory_bulk_sell(int action, MarketPicks picks, KioInvDone done, void *user);
// Bulk list of picks at one unit price; each stack lists its whole count. Takes ownership of picks. 1 = sent.
int  kio_inventory_bulk_list(int price, MarketPicks picks, KioInvDone done, void *user);

// The notice for an action refused because the inventory is not loaded.
const char *kio_inventory_gate_text(void);
