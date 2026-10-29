#pragma once
// A Box Office account's inventory for the v10.0.4 bulk actions (client/src/ui/bo_inventory.c): the groups of the
// account's own skin rows (data[10]: {ind, cnt, st: 1} per stack, {ind, cnt: 1, st: 0, id, m} per painted copy,
// services/brain/db_common.c db_owned_skins) and its item locks, with kn_brain's frames over the social socket:
//   ["ilk"] -> ["ilkr", {locks}]                       the locks (krunker.io's GET items/locks rows)
//   ["ils", item, unit, locked, uid] -> ["ilsr", ...]  lock / unlock (POST items/locks)
//   ["qsb", action, [[item, unit, keep], ...], uid, {op}] bulk quick sell, 250 units a frame (POST market/quick-sell/bulk)
//   ["lib", [[item, unit, price, qty], ...], uid, {op}] bulk list, 100 units a frame (POST market/list/bulk)
// A unit is a painted copy's id, or the store index for a stack. The API is kio_inventory.h's (inv_source.h picks one).
#include <kio_inventory.h>

int  bo_inventory_ready(void);
int  bo_inventory_loading(void);
void bo_inventory_load(KioInvDone done, void *user);
void bo_inventory_clear(void);

const MarketGroup *bo_inventory_groups(int *n);
const MarketLocks *bo_inventory_locks(void);
int  bo_inventory_locked(const MarketGroup *g, int copy);
int  bo_inventory_set_lock(const MarketGroup *g, int copy, int locked, KioInvDone done, void *user);
int  bo_inventory_bulk_sell(int action, MarketPicks picks, KioInvDone done, void *user);
int  bo_inventory_bulk_list_rows(const MarketListRow *rows, int n, KioInvDone done, void *user);
const KioBulkResult *bo_inventory_bulk_result(void);
void bo_inventory_bulk_progress(int *done, int *total);
