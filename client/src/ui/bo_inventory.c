// A Box Office account's inventory for the v10.0.4 bulk actions (client/include/bo_inventory.h): the groups come from the
// account's own skin rows, the locks and the writes from kn_brain (services/brain/economy.c "ilk" / "ils" / "qsb" / "lib").
// One run (a bulk sell or list) and one lock write at a time, as the UI sends them; an answer for another uid is not ours.
#include <bo_inventory.h>
#include <backend.h>
#include <backend_ui.h>
#include <economy_client.h> // econ_new_op: a frame's idempotency key
#include <i18n.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BO_SELL_CHUNK 250 // deob-10.0.4.js:146851 (the official chunk; kn_brain takes at most this many a frame)
#define BO_COPY_ID_BASE 1000000 // kn_brain's V10_COPY_ID_BASE: a copy id is never a store index

static struct {
    int listening;
    int ready, loading, account_id;
    MarketLocks locks;
    MarketGroup *groups;
    int n_groups;
    KioInvDone load_done;
    void *load_user;
    // the lock write in flight
    int lock_uid;
    KioInvDone lock_done;
    void *lock_user;
    // the run in flight: kind 1 sell, 2 list
    int kind, uid, action, from, count;
    MarketPicks picks;
    MarketListRow *rows;
    int n_rows, row, used;
    KioInvDone run_done;
    void *run_user;
    KioBulkResult result;
    int progress, total;
} O;

static int next_uid(void) {
    static int uid = 7000;
    return ++uid;
}

static void on_frame(void *user, const cJSON *frame);

static void listen_once(void) {
    if (!O.listening) O.listening = backend_social_listen(on_frame, NULL);
}

static int send_frame(cJSON *f) {
    const int sent = f && backend_social_send(f);
    cJSON_Delete(f);
    return sent;
}

static cJSON *op_object(void) {
    char key[64];
    econ_new_op(key, sizeof(key), "bulk");
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "op", key);
    return o;
}

// ---------------------------------------------------------------- the groups (the account's rows)

static void groups_free(void) {
    market_groups_free(O.groups, O.n_groups);
    O.groups = NULL;
    O.n_groups = 0;
}

// data[10]'s rows as krunker.io's inventory rows {i, st, sid, cnt, m}: a stack's sid is its store index, a copy's its id
const MarketGroup *bo_inventory_groups(int *n) {
    groups_free();
    const AccountInfo *a = active_account();
    const cJSON *skins = a && a->data ? cJSON_GetArrayItem(a->data, 10) : NULL, *e;
    cJSON *rows = cJSON_CreateArray();
    cJSON_ArrayForEach(e, skins) {
        const cJSON *ind = cJSON_GetObjectItemCaseSensitive(e, "ind"), *id = cJSON_GetObjectItemCaseSensitive(e, "id");
        const cJSON *cnt = cJSON_GetObjectItemCaseSensitive(e, "cnt"), *m = cJSON_GetObjectItemCaseSensitive(e, "m");
        if (!cJSON_IsNumber(ind)) continue;
        const int copy = cJSON_IsNumber(id) && id->valuedouble >= BO_COPY_ID_BASE;
        cJSON *r = cJSON_CreateObject();
        cJSON_AddNumberToObject(r, "i", ind->valuedouble);
        cJSON_AddNumberToObject(r, "st", copy ? 0 : 1);
        cJSON_AddNumberToObject(r, "sid", copy ? id->valuedouble : ind->valuedouble);
        cJSON_AddNumberToObject(r, "cnt", cJSON_IsNumber(cnt) ? cnt->valuedouble : 1);
        if (copy && m) cJSON_AddItemToObject(r, "m", cJSON_Duplicate(m, 1));
        cJSON_AddItemToArray(rows, r);
    }
    market_groups_parse(rows, &O.groups, &O.n_groups);
    cJSON_Delete(rows);
    *n = O.n_groups;
    return O.groups;
}

const MarketLocks *bo_inventory_locks(void) { return &O.locks; }

int bo_inventory_locked(const MarketGroup *g, const int copy) {
    if (!g) return 0;
    if (copy < 0) return market_locks_has(&O.locks, g->stack_id, g->item, 1);
    return copy < g->n_instances && market_locks_has(&O.locks, g->instances[copy], g->item, 0);
}

// ---------------------------------------------------------------- the locks

int bo_inventory_ready(void) {
    const AccountInfo *a = active_account();
    return O.ready && a && a->id == O.account_id;
}

int bo_inventory_loading(void) { return O.loading; }

void bo_inventory_load(KioInvDone done, void *user) {
    const AccountInfo *a = active_account();
    if (!a) {
        if (done) done(user, 0, I18N("kn.inv.signIn", "Sign in to use your inventory."));
        return;
    }
    listen_once();
    O.load_done = done;
    O.load_user = user;
    O.loading = 1;
    O.account_id = a->id;
    cJSON *f = cJSON_CreateArray();
    cJSON_AddItemToArray(f, cJSON_CreateString("ilk"));
    if (!send_frame(f)) {
        O.loading = 0;
        O.load_done = NULL;
        if (done) done(user, 0, I18N("generic.error", "Error"));
    }
}

void bo_inventory_clear(void) {
    market_locks_free(&O.locks);
    groups_free();
    O.ready = O.loading = 0;
    O.account_id = 0;
    O.load_done = NULL;
    O.lock_done = NULL;
    O.lock_uid = 0;
}

int bo_inventory_set_lock(const MarketGroup *g, const int copy, const int locked, KioInvDone done, void *user) {
    if (!g || copy >= g->n_instances || O.lock_uid) return 0;
    listen_once();
    O.lock_uid = next_uid();
    O.lock_done = done;
    O.lock_user = user;
    cJSON *f = cJSON_CreateArray();
    cJSON_AddItemToArray(f, cJSON_CreateString("ils"));
    cJSON_AddItemToArray(f, cJSON_CreateNumber(g->item));
    cJSON_AddItemToArray(f, cJSON_CreateNumber(copy < 0 ? 0 : g->instances[copy]));
    cJSON_AddItemToArray(f, cJSON_CreateNumber(locked ? 1 : 0));
    cJSON_AddItemToArray(f, cJSON_CreateNumber(O.lock_uid));
    if (send_frame(f)) return 1;
    O.lock_uid = 0;
    O.lock_done = NULL;
    return 0;
}

// ---------------------------------------------------------------- the runs

static void run_end(const int ok, const char *message) {
    const KioInvDone done = O.run_done;
    void *user = O.run_user;
    market_picks_free(&O.picks);
    free(O.rows);
    O.rows = NULL;
    O.kind = O.uid = 0;
    O.run_done = NULL;
    if (done) done(user, ok, message);
}

static void run_next(void) {
    cJSON *f = cJSON_CreateArray();
    O.uid = next_uid();
    if (O.kind == 1) {
        if (O.from >= O.picks.n) {
            cJSON_Delete(f);
            run_end(1, NULL);
            return;
        }
        O.count = O.picks.n - O.from < BO_SELL_CHUNK ? O.picks.n - O.from : BO_SELL_CHUNK;
        cJSON *items = cJSON_CreateArray();
        for (int i = O.from; i < O.from + O.count; i++) {
            cJSON *u = cJSON_CreateArray();
            cJSON_AddItemToArray(u, cJSON_CreateNumber(O.picks.item_ids[i]));
            cJSON_AddItemToArray(u, cJSON_CreateNumber(O.picks.skin_ids[i]));
            cJSON_AddItemToArray(u, cJSON_CreateNumber(O.picks.keeps ? O.picks.keeps[i] : 0)); // a stack's units to keep
            cJSON_AddItemToArray(items, u);
        }
        cJSON_AddItemToArray(f, cJSON_CreateString("qsb"));
        cJSON_AddItemToArray(f, cJSON_CreateNumber(O.action));
        cJSON_AddItemToArray(f, items);
    } else {
        int units = 0;
        cJSON *batch = market_list_batch(O.rows, O.n_rows, &O.row, &O.used, &units); // the 100-unit batches (:131955)
        if (!batch) {
            cJSON_Delete(f);
            const int finished = O.row >= O.n_rows;
            run_end(finished, finished ? NULL : I18N("generic.error", "Error"));
            return;
        }
        O.count = units;
        cJSON *items = cJSON_CreateArray(), *it;
        cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(batch, "items")) {
            cJSON *u = cJSON_CreateArray();
            cJSON_AddItemToArray(u, cJSON_CreateNumber(cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(it, "item_id"))));
            cJSON_AddItemToArray(u, cJSON_CreateNumber(cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(it, "skin_id"))));
            cJSON_AddItemToArray(u, cJSON_CreateNumber(cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(it, "price"))));
            cJSON_AddItemToArray(u, cJSON_CreateNumber(cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(it, "quantity"))));
            cJSON_AddItemToArray(items, u);
        }
        cJSON_Delete(batch);
        cJSON_AddItemToArray(f, cJSON_CreateString("lib"));
        cJSON_AddItemToArray(f, items);
    }
    cJSON_AddItemToArray(f, cJSON_CreateNumber(O.uid));
    cJSON_AddItemToArray(f, op_object());
    if (!send_frame(f)) run_end(0, I18N("generic.error", "Error"));
}

static void result_reset(const int total) {
    memset(&O.result, 0, sizeof(O.result));
    const AccountInfo *a = active_account();
    O.result.funds_before = a ? a->funds : 0;
    O.result.junk_before = a ? a->junk : 0;
    O.progress = 0;
    O.total = total;
}

int bo_inventory_bulk_sell(const int action, MarketPicks picks, KioInvDone done, void *user) {
    if (O.kind || picks.n == 0 || !active_account()) {
        market_picks_free(&picks);
        if (done) done(user, 0, picks.n == 0 ? I18N("kn.inv.noneSelected", "Select some items first.") : I18N("generic.error", "Error"));
        return 0;
    }
    listen_once();
    O.kind = 1;
    O.action = action >= 0 && action <= 2 ? action : 0;
    O.picks = picks;
    O.from = 0;
    O.run_done = done;
    O.run_user = user;
    result_reset(picks.n);
    run_next();
    return 1;
}

int bo_inventory_bulk_list_rows(const MarketListRow *rows, const int n, KioInvDone done, void *user) {
    int units = 0;
    for (int i = 0; i < n; i++) units += rows[i].quantity > 0 ? rows[i].quantity : 0;
    MarketListRow *copy = O.kind || n <= 0 || units <= 0 ? NULL : malloc(sizeof(MarketListRow) * (size_t) n);
    if (!copy) {
        if (done) done(user, 0, units <= 0 ? I18N("kn.inv.noneSelected", "Select some items first.") : I18N("generic.error", "Error"));
        return 0;
    }
    memcpy(copy, rows, sizeof(MarketListRow) * (size_t) n);
    listen_once();
    O.kind = 2;
    O.rows = copy;
    O.n_rows = n;
    O.row = O.used = 0;
    O.run_done = done;
    O.run_user = user;
    result_reset(units);
    run_next();
    return 1;
}

const KioBulkResult *bo_inventory_bulk_result(void) { return &O.result; }

void bo_inventory_bulk_progress(int *done, int *total) {
    *done = O.progress < O.total ? O.progress : O.total;
    *total = O.total;
}

// ---------------------------------------------------------------- answers

static void on_frame(void *user, const cJSON *frame) {
    (void) user;
    const char *label = cJSON_GetStringValue(cJSON_GetArrayItem(frame, 0));
    if (!label) return;
    const cJSON *v = cJSON_GetArrayItem(frame, 1);
    const int uid = (int) cJSON_GetNumberValue(cJSON_GetArrayItem(frame, 2));
    const char *err = cJSON_GetStringValue(cJSON_GetArrayItem(frame, 3));
    if (!strcmp(label, "ilkr")) { // the locks (sh_item_locks)
        O.loading = 0;
        if (cJSON_IsObject(v)) {
            market_locks_apply(&O.locks, cJSON_GetObjectItemCaseSensitive(v, "locks"));
            O.ready = 1;
        }
        const KioInvDone done = O.load_done;
        O.load_done = NULL;
        if (done) done(O.load_user, cJSON_IsObject(v), cJSON_IsObject(v) ? NULL : I18N("generic.error", "Error"));
        return;
    }
    if (!strcmp(label, "ilsr")) { // a lock write: the locks are read again, done runs once they are in
        if (!O.lock_uid || uid != O.lock_uid) return;
        const KioInvDone done = O.lock_done;
        void *u = O.lock_user;
        O.lock_uid = 0;
        O.lock_done = NULL;
        if (!cJSON_IsObject(v)) {
            if (done) done(u, 0, err ? err : I18N("generic.error", "Error"));
            return;
        }
        bo_inventory_load(done, u);
        return;
    }
    if (strcmp(label, "qsbr") && strcmp(label, "libr")) return;
    if (!O.kind || uid != O.uid) return;
    if (!cJSON_IsObject(v)) {
        run_end(0, err ? err : I18N("generic.error", "Error"));
        return;
    }
    const cJSON *funds = cJSON_GetObjectItemCaseSensitive(v, "funds");
    if (O.kind == 1) { // sh_quick_sell_bulk's {sold, skipped, funds, junk}
        const cJSON *junk = cJSON_GetObjectItemCaseSensitive(v, "junk");
        O.result.sold += cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(v, "sold"));
        O.result.skipped += cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(v, "skipped"));
        if (cJSON_IsNumber(funds)) O.result.kr_gained = funds->valuedouble - O.result.funds_before;
        if (cJSON_IsNumber(junk)) O.result.junk_gained = junk->valuedouble - O.result.junk_before;
        O.progress += O.count;
        O.from += O.count;
        run_next();
        return;
    }
    // sh_market_list_bulk's {listed, funds, results}: a batch with failed rows ends the run (deob-10.0.4.js:132020-132022)
    const cJSON *listed = cJSON_GetObjectItemCaseSensitive(v, "listed"), *e;
    if (cJSON_IsNumber(listed)) O.result.listed += listed->valueint;
    int failed = 0;
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(v, "results")) {
        const char *why = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(e, "error"));
        if (!why || !why[0]) continue;
        failed++;
        if (!O.result.error[0]) snprintf(O.result.error, sizeof(O.result.error), "%s", why);
    }
    O.progress += O.count;
    if (failed) run_end(1, NULL);
    else run_next();
}
