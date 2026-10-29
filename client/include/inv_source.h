#pragma once
// The inventory the bulk market UI acts on (v10_bulk.c, v10_wallet.c, v10_market.c): krunker.io's official one
// (kio_inventory.h) while krunker.io is the active account, else the Box Office account's (bo_inventory.h). The same
// calls, the same answers; each side sends its own requests.
#include <kio_inventory.h>
#include <bo_inventory.h>
#include <kio_store.h>
#include <i18n.h>

static inline int inv_official(void) { return kio_store_active(); }
static inline int inv_ready(void) { return inv_official() ? kio_inventory_ready() : bo_inventory_ready(); }
static inline int inv_loading(void) { return inv_official() ? kio_inventory_loading() : bo_inventory_loading(); }
static inline void inv_load(KioInvDone done, void *user) {
    if (inv_official()) kio_inventory_load(done, user);
    else bo_inventory_load(done, user);
}
static inline const MarketGroup *inv_groups(int *n) { return inv_official() ? kio_inventory_groups(n) : bo_inventory_groups(n); }
static inline const MarketLocks *inv_locks(void) { return inv_official() ? kio_inventory_locks() : bo_inventory_locks(); }
static inline int inv_locked(const MarketGroup *g, int copy) { return inv_official() ? kio_inventory_locked(g, copy) : bo_inventory_locked(g, copy); }
static inline int inv_set_lock(const MarketGroup *g, int copy, int locked, KioInvDone done, void *user) {
    return inv_official() ? kio_inventory_set_lock(g, copy, locked, done, user) : bo_inventory_set_lock(g, copy, locked, done, user);
}
static inline int inv_bulk_sell(int action, MarketPicks picks, KioInvDone done, void *user) {
    return inv_official() ? kio_inventory_bulk_sell(action, picks, done, user) : bo_inventory_bulk_sell(action, picks, done, user);
}
static inline int inv_bulk_list_rows(const MarketListRow *rows, int n, KioInvDone done, void *user) {
    return inv_official() ? kio_inventory_bulk_list_rows(rows, n, done, user) : bo_inventory_bulk_list_rows(rows, n, done, user);
}
static inline const KioBulkResult *inv_bulk_result(void) { return inv_official() ? kio_inventory_bulk_result() : bo_inventory_bulk_result(); }
static inline void inv_bulk_progress(int *done, int *total) {
    if (inv_official()) kio_inventory_bulk_progress(done, total);
    else bo_inventory_bulk_progress(done, total);
}
static inline const char *inv_gate_text(void) {
    return inv_official() ? kio_inventory_gate_text() : I18N("kn.inv.boNotLoaded", "Your inventory is still loading. Try again in a moment.");
}
